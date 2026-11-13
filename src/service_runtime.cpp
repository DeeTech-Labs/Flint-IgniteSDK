#include <algorithm>
#include <flint/registry.h>
#include <flint/service_runtime.h>
#include <limits>

namespace flint {
namespace {
size_t declarationsBytes(const std::vector<Declaration> &permissions) {
    size_t bytes = permissions.capacity() * sizeof(Declaration);
    for (const auto &p : permissions)
        bytes += p.permission.capacity() + p.scope.capacity() + 2;
    return bytes;
}
size_t componentBytes(const ComponentInfo &c) {
    return c.id.capacity() + 1 + declarationsBytes(c.permissions);
}
size_t manifestBytes(const Manifest &m) {
    size_t bytes = m.id.capacity() + m.name.capacity() + m.publisher.capacity() + 3 +
        m.components.capacity() * sizeof(ComponentInfo) + declarationsBytes(m.permissions);
    for (const auto &c : m.components)
        bytes += componentBytes(c);
    return bytes;
}
} // namespace
ServiceRuntime::~ServiceRuntime() {
    (void)stop(ServiceReason::shutdown);
}
Error ServiceRuntime::permission(std::string_view name) {
    if (loading_ || closing_ || state_ != ServiceState::running)
        return Error::invalid_state;
    const auto *spec = flint::permission(name);
    if (!spec)
        return Error::not_supported;
    const bool declared = std::any_of(component_.permissions.begin(), component_.permissions.end(),
        [&](const auto &d) { return d.permission == name && d.scope.empty(); });
    if (!declared)
        return Error::permission_denied;
    return host_.authorize ? host_.authorize(host_.context, name) : Error::permission_denied;
}
Error ServiceRuntime::call(
    AppService service, std::string_view key, std::string_view value, AppReply &reply) {
    if (loading_ || closing_ || state_ != ServiceState::running)
        return Error::invalid_state;
    if (service != AppService::time && service != AppService::monotonic &&
        service != AppService::timer && service != AppService::beep)
        return Error::not_supported;
    auto error = permission(servicePermission(service));
    if (error != Error::ok)
        return error;
    if (service == AppService::beep && (error = permission("audio.background")) != Error::ok)
        return error;
    if (service == AppService::timer)
        return Error::ok;
    if (!host_.call)
        return Error::not_supported;
    error = host_.call(host_.context, service, key, value, reply);
    if (error == Error::ok &&
        (reply.size > MaxAppString ||
            (!reply.numeric && !appText({reply.text.data(), reply.size}))))
        return Error::invalid_argument;
    return error;
}
Error ServiceRuntime::sensor(const sensors::Request &request, sensors::Reply &reply) {
    if (loading_ || closing_ || state_ != ServiceState::running)
        return Error::invalid_state;
    if (!(component_.capabilities & sensors::Capability) || !host_.sensor)
        return Error::not_supported;
    for (unsigned i = 0; i < 6 && request.operation != sensors::Operation::catalog &&
        request.operation != sensors::Operation::unsubscribe;
        ++i)
        if (request.channels & (1u << i)) {
            const auto error = permission(sensors::permission(sensors::Channel(i)));
            if (error != Error::ok)
                return error;
        }
    return host_.sensor(host_.context, request, reply);
}
Error ServiceRuntime::ipc(const IpcRequest &request, IpcReply &reply) {
    if (loading_ || closing_ || state_ != ServiceState::running)
        return Error::invalid_state;
    if (!(component_.capabilities & CapComponentIpc) || !host_.ipc)
        return Error::not_supported;
    return host_.ipc(host_.context, request, reply);
}
void ServiceRuntime::clear() {
    lua_.stop();
    packageIdentity_ = 0;
    manifest_ = {};
    component_ = {};
    native_ = 0;
    loading_ = closing_ = pendingStart_ = pendingResume_ = false;
}
Error ServiceRuntime::fail(Error error) {
    if (host_.cancelInstance)
        host_.cancelInstance(host_.context);
    peak_ = std::max(peak_, lua_.peak());
    clear();
    state_ = ServiceState::faulted;
    return error;
}
Error ServiceRuntime::launch(const Installed &installed, std::string_view id,
    MemoryBudget available, uint64_t capabilities, InstanceKey key, AppHost host) {
    if (!key.id || !key.generation)
        return Error::invalid_argument;
    (void)stop(ServiceReason::update);
    peak_ = 0;
    File candidate;
    Package package;
    Manifest manifest;
    if (!candidate.open(installed.path, false))
        return Error::io_error;
    auto error = package.open(candidate.source());
    if (error != Error::ok)
        return error;
    error = validateApplication(package, manifest, available, capabilities);
    if (error != Error::ok)
        return error;
    const auto selected = std::find_if(manifest.components.begin(), manifest.components.end(),
        [&](const auto &c) { return c.id == id; });
    if (selected == manifest.components.end())
        return Error::source_unavailable;
    if (selected->type != Component::service || !selected->logicSection || selected->section ||
        !(selected->capabilities & CapBackground))
        return Error::not_supported;
    if (!fits(selected->budget, available))
        return Error::resource_exhausted;
    Entry code;
    bool found = false;
    for (uint32_t i = 0; i < package.count(); ++i)
        if (package.entry(i, code) == Error::ok && code.id == selected->logicSection) {
            found = true;
            break;
        }
    if (!found || code.type != Section::lua || !code.size || code.size > MaxLuaSourceBytes)
        return Error::invalid_package;
    ComponentInfo component = *selected;
    const auto native = sizeof(ServiceRuntime) + manifestBytes(manifest) +
        componentBytes(component) + LuaApp::nativeBytes();
    // Charge the transient loader and conservative filesystem/stdio bookkeeping too.
    // Services retain neither package handles nor resource streams once code is in memory.
    const size_t loadPeak =
        native + code.size + sizeof(File) + sizeof(Package) + sizeof(FILE) + 1024;
    if (loadPeak > component.budget.internal_bytes ||
        component.budget.internal_bytes - loadPeak < 16 * 1024)
        return Error::resource_exhausted;
    Bytes source(code.size);
    if (package.read(code, 0, source) != Error::ok || !candidate.unchanged())
        return Error::io_error;
    host_ = host;
    key_ = key;
    state_ = ServiceState::starting;
    loading_ = true;
    packageIdentity_ = package.identity();
    package.close();
    candidate.close();
    manifest_ = std::move(manifest);
    component_ = std::move(component);
    native_ = native;
    AppHost bridge;
    bridge.context = this;
    bridge.enabled = true;
    bridge.kind = ExecutionKind::service;
    bridge.minTimerMs = host.minTimerMs;
    bridge.call = [](void *p, AppService s, std::string_view k, std::string_view v, AppReply &r) {
        return static_cast<ServiceRuntime *>(p)->call(s, k, v, r);
    };
    bridge.monotonic = [](void *p) {
        const auto &h = static_cast<ServiceRuntime *>(p)->host_;
        return h.monotonic ? h.monotonic(h.context) : 0;
    };
    if (host.microseconds)
        bridge.microseconds = [](void *p) {
            const auto &h = static_cast<ServiceRuntime *>(p)->host_;
            return h.microseconds(h.context);
        };
    if (component_.capabilities & sensors::Capability)
        bridge.sensor = [](void *p, const sensors::Request &q, sensors::Reply &r) {
            return static_cast<ServiceRuntime *>(p)->sensor(q, r);
        };
    if (component_.capabilities & CapComponentIpc)
        bridge.ipc = [](void *p, const IpcRequest &q, IpcReply &r) {
            return static_cast<ServiceRuntime *>(p)->ipc(q, r);
        };
    for (const auto &d : component_.permissions)
        if (d.permission == "audio.play")
            bridge.outputs |= 1;
    const auto heap = std::min<size_t>(128 * 1024, component_.budget.internal_bytes - loadPeak);
    error = lua_.loadService({reinterpret_cast<const char *>(source.data()), source.size()},
        uint32_t(heap), component_.budget.instructions, bridge);
    peak_ = std::max<size_t>(loadPeak + lua_.peak(), native_ + lua_.peak());
    if (error != Error::ok)
        return fail(error);
    loading_ = false;
    pendingStart_ = true;
    state_ = ServiceState::running;
    now_ = host.monotonic ? host.monotonic(host.context) : 0;
    return Error::ok;
}
Error ServiceRuntime::tick(uint64_t now, DispatchBudget budget, DispatchUsage &usage) {
    usage = {};
    if (state_ != ServiceState::running || now < now_)
        return Error::invalid_state;
    now_ = now;
    auto error = lua_.beginTick(now, budget);
    if (error != Error::ok)
        return fail(error);
    if (pendingStart_ || pendingResume_) {
        const auto event = pendingStart_ ? "start" : "resume";
        pendingStart_ = pendingResume_ = false;
        error = lua_.event(event, 0);
    }
    for (size_t i = 0; error == Error::ok && host_.sensorEvent &&
        i < sensors::MaxSubscriptions + sensors::MaxRefresh;
        ++i) {
        sensors::Event event;
        if (!host_.sensorEvent(host_.context, event))
            break;
        error = lua_.sensorEvent(event);
    }
    for (size_t i = 0; error == Error::ok && host_.ipcEvent && i < 8; ++i) {
        IpcEvent event;
        if (!host_.ipcEvent(host_.context, event))
            break;
        error = lua_.ipcEvent(event);
    }
    if (error == Error::ok)
        error = lua_.tick(now);
    usage = lua_.usage();
    lua_.endTick();
    peak_ = std::max(peak_, native_ + lua_.peak());
    return error == Error::ok ? Error::ok : fail(error);
}
Error ServiceRuntime::lifecycle(std::string_view event, ServiceReason reason) {
    if (!lua_.active())
        return Error::ok;
    DispatchBudget budget;
    budget.instructions = std::min<uint32_t>(component_.budget.instructions, 4096);
    budget.nativeCalls = 8;
    if (host_.microseconds) {
        const auto at = host_.microseconds(host_.context);
        budget.deadlineUs = at <= UINT64_MAX - 2000 ? at + 2000 : UINT64_MAX;
    }
    auto error = lua_.beginTick(now_, budget);
    if (error == Error::ok)
        error = lua_.event(event, int32_t(reason));
    lua_.endTick();
    return error;
}
Error ServiceRuntime::pause(ServiceReason reason) {
    if (state_ != ServiceState::running)
        return Error::invalid_state;
    closing_ = true;
    state_ = ServiceState::paused;
    if (host_.cancelInstance)
        host_.cancelInstance(host_.context);
    const auto error = lifecycle("pause", reason);
    closing_ = false;
    return error == Error::ok ? Error::ok : fail(error);
}
Error ServiceRuntime::resume(uint64_t now) {
    if (state_ != ServiceState::paused || now < now_)
        return Error::invalid_state;
    now_ = now;
    lua_.resumeClock(now);
    state_ = ServiceState::running;
    pendingResume_ = true;
    return Error::ok;
}
Error ServiceRuntime::stop(ServiceReason reason) {
    if (state_ == ServiceState::stopped)
        return Error::ok;
    closing_ = true;
    if (host_.cancelInstance)
        host_.cancelInstance(host_.context);
    const auto error = lifecycle("stop", reason);
    peak_ = std::max(peak_, native_ + lua_.peak());
    clear();
    state_ = ServiceState::stopped;
    return error;
}
uint64_t ServiceRuntime::nextWake() const {
    if (state_ != ServiceState::running)
        return UINT64_MAX;
    return pendingStart_ || pendingResume_ ? 0 : lua_.nextWake();
}
} // namespace flint
