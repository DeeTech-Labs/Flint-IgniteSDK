#include "core_background_state.h"
#include <bit>
#include <charconv>
#include <cmath>
#include <cstring>
#include <flint/os.h>
#include <flint/registry.h>
#include <new>
#include <sys/stat.h>

namespace flint {
namespace {
template <size_t N> void copyText(std::array<char, N> &out, std::string_view text) {
    out = {};
    std::copy_n(text.begin(), std::min(text.size(), N - 1), out.begin());
}
uint64_t identityHash(std::string_view text) {
    Sha256 h;
    h.update({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
    const auto digest = h.finish();
    Reader r{digest};
    return r.n(8);
}
uint64_t installedGeneration(const Installed &p) {
    File f;
    Package package;
    return f.open(p.path) && package.open(f.source()) == Error::ok ? package.identity() : 0;
}
std::string autoKey(std::string_view id) {
    char value[17]{};
    const auto result = std::to_chars(value, value + 16, identityHash(id), 16);
    return "bg." + std::string(value, result.ptr);
}
bool autoEnabled(const Values &settings, std::string_view id, uint64_t generation) {
    const auto entry = settings.find(autoKey(id));
    return entry != settings.end() && entry->second == std::to_string(generation) + ":1";
}
bool disableAuto(Values &settings, std::string_view id, uint64_t generation) {
    const auto entry = settings.find(autoKey(id));
    if (entry == settings.end() || entry->second == std::to_string(generation) + ":0")
        return false;
    entry->second = std::to_string(generation) + ":0";
    return true;
}
bool validValue(const IpcValue &value) {
    if (unsigned(value.kind) > unsigned(IpcValueKind::text))
        return false;
    if (value.kind == IpcValueKind::boolean && value.integer != 0 && value.integer != 1)
        return false;
    if (value.kind == IpcValueKind::number && !std::isfinite(value.number))
        return false;
    return value.size <= 96 &&
        (value.kind != IpcValueKind::text || appText({value.text.data(), value.size}));
}
bool keyText(std::string_view value, size_t limit = 32) {
    if (value.empty() || value.size() > limit)
        return false;
    for (char c : value)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                c == '_' || c == '-' || c == '.'))
            return false;
    return true;
}
void writeValue(Writer &w, const IpcValue &v) {
    w.n(uint8_t(v.kind), 1);
    if (v.kind == IpcValueKind::text)
        w.text({v.text.data(), v.size});
    else if (v.kind == IpcValueKind::number)
        w.n(std::bit_cast<uint64_t>(v.number), 8);
    else
        w.n(uint64_t(v.integer), 8);
}
IpcValue readValue(Reader &r) {
    IpcValue v;
    v.kind = IpcValueKind(r.n(1));
    if (v.kind == IpcValueKind::text) {
        auto text = r.text(96);
        v.size = uint8_t(text.size());
        std::copy(text.begin(), text.end(), v.text.begin());
    } else if (v.kind == IpcValueKind::number)
        v.number = std::bit_cast<double>(r.n(8));
    else
        v.integer = std::bit_cast<int64_t>(r.n(8));
    if (!validValue(v))
        r.valid = false;
    return v;
}
} // namespace
Core::~Core() {
    if (background_)
        for (auto &slot : background_->slots)
            if (slot.runtime) {
                slot.host.closing = true;
                sensors_.cancelOwner(slot.host.key);
                slot.runtime->stop(ServiceReason::shutdown);
                slot.runtime.reset();
            }
    runtime_.stop();
    runtime_.host({});
}
AppHost Core::instanceHost(HostBinding &host) {
    AppHost result;
    result.context = &host;
    result.enabled = true;
    result.kind = host.service ? ExecutionKind::service : ExecutionKind::foreground;
    result.minTimerMs = host.service ? platform_.background.minTimerMs : 1;
    result.call = [](void *p, AppService s, std::string_view k, std::string_view v, AppReply &r) {
        auto &h = *static_cast<HostBinding *>(p);
        return h.core->appCall(h, s, k, v, r);
    };
    result.authorize = [](void *p, std::string_view name) {
        auto &h = *static_cast<HostBinding *>(p);
        return h.core->authorize(h, name);
    };
    result.monotonic = [](void *p) {
        auto &h = *static_cast<HostBinding *>(p);
        return h.core->platform_.monotonic();
    };
    if (platform_.microseconds)
        result.microseconds = [](void *p) {
            auto &h = *static_cast<HostBinding *>(p);
            return h.core->platform_.microseconds();
        };
    result.sensor = [](void *p, const sensors::Request &q, sensors::Reply &r) {
        auto &h = *static_cast<HostBinding *>(p);
        return h.core->sensorCall(h, q, r);
    };
    result.sensorEvent = [](void *p, sensors::Event &e) {
        auto &h = *static_cast<HostBinding *>(p);
        return h.core->sensorEvent(h, e);
    };
    result.cancelInstance = [](void *p) {
        auto &h = *static_cast<HostBinding *>(p);
        h.core->cancelInstance(h);
    };
    result.ipc = [](void *p, const IpcRequest &q, IpcReply &r) {
        auto &h = *static_cast<HostBinding *>(p);
        return h.core->ipcCall(h, q, r);
    };
    result.ipcEvent = [](void *p, IpcEvent &e) {
        auto &h = *static_cast<HostBinding *>(p);
        return h.core->ipcEvent(h, e);
    };
    if (!host.service)
        result.services = [](void *p, const ServiceRequest &q, ServiceInfo &r) {
            auto &h = *static_cast<HostBinding *>(p);
            return h.core->serviceControl(h, q, r);
        };
    return result;
}
void Core::cancelInstance(HostBinding &host) {
    sensors_.cancelOwner(host.key);
    if (!background_)
        return;
    auto &b = *background_;
    if (host.service && host.slot < b.slots.size())
        b.slots[host.slot].inbox.reset({});
    else
        b.foreground.reset({});
    b.foreground.cancelFrom(host.key);
    for (auto &slot : b.slots)
        slot.inbox.cancelFrom(host.key);
    if (b.flush && b.flush->owner == host.key)
        b.flush.reset();
}
Core::HostBinding *Core::binding(InstanceKey key) {
    if (foregroundBinding_.key == key && key.generation == runtime_.consumerGeneration() + 1)
        return &foregroundBinding_;
    if (background_)
        for (auto &slot : background_->slots)
            if (slot.runtime && slot.host.key == key)
                return &slot.host;
    return nullptr;
}
Core::Caller Core::caller(HostBinding &host) {
    Caller result;
    result.key = host.key;
    result.service = host.service;
    if (host.closing)
        return result;
    if (host.preview) {
        if (!edge_ || &host != &edge_->host)
            return result;
        result.manifest = &edge_->preview.manifest();
        result.component = &edge_->preview.component();
        result.principal = {identityHash(result.manifest->id),
            identityHash(result.manifest->publisher), edge_->preview.packageIdentity()};
        result.active = edge_->preview.state() == State::active;
        result.system = true;
        return result;
    }
    if (host.service) {
        if (!background_ || host.slot >= background_->slots.size())
            return result;
        auto &slot = background_->slots[host.slot];
        if (!slot.runtime || slot.host.key != host.key)
            return result;
        const auto &record = background_->records[slot.record];
        result.manifest = &slot.runtime->manifest();
        result.component = &slot.runtime->component();
        result.principal = record.principal;
        result.active = record.info.state == ServiceState::running &&
            slot.runtime->state() == ServiceState::running;
        return result;
    }
    result.manifest = &runtime_.manifest();
    result.component = &runtime_.component();
    result.active = runtime_.state() == State::active;
    const auto &m = *result.manifest;
    if (metricRuntime_ != runtime_.generation() || metricCatalog_ != packages_.revision() ||
        metricPrincipal_.app != identityHash(m.id)) {
        metricRuntime_ = runtime_.generation();
        metricCatalog_ = packages_.revision();
        Installed installed;
        metricIdentityValid_ = packages_.locate(m.id, installed) == Error::ok;
        if (metricIdentityValid_) {
            metricPrincipal_ = {
                identityHash(m.id), identityHash(m.publisher), installedGeneration(installed)};
            metricIdentityValid_ = metricPrincipal_.generation &&
                metricPrincipal_.generation == runtime_.packageIdentity() &&
                installed.manifest.publisher == m.publisher;
            metricFactory_ = installed.factory;
        }
    }
    if (metricIdentityValid_) {
        result.principal = metricPrincipal_;
        result.system = metricFactory_;
    }
    return result;
}
void Core::backgroundBegin() {
    if (!(platform_.capabilities & CapBackground) || !platform_.background.maxServices ||
        background_)
        return;
    if (platform_.background.maxServices > MaxBackgroundServices ||
        platform_.background.windowMs < 10 || !platform_.background.quantumInstructions ||
        !platform_.background.quantumNativeCalls ||
        platform_.background.instructionsPerWindow < platform_.background.quantumInstructions ||
        platform_.background.nativeCallsPerWindow < platform_.background.quantumNativeCalls ||
        !platform_.background.callbackWallUs || !platform_.background.wallUsPerWindow)
        return;
    background_.reset(new (std::nothrow) BackgroundState);
    if (!background_)
        return;
    for (size_t i = 0; i < background_->slots.size(); ++i)
        background_->slots[i].host = {this, {}, i, true, false};
}
Error Core::startService(std::string_view package, std::string_view component) {
    if (!(platform_.capabilities & CapBackground))
        return Error::not_supported;
    backgroundBegin();
    if (!background_)
        return Error::resource_exhausted;
    auto &b = *background_;
    const auto &policy = platform_.background;
    Installed installed;
    auto error = packages_.locate(package, installed);
    if (error != Error::ok)
        return error;
    const ComponentInfo *selected = nullptr;
    for (const auto &entry : installed.manifest.components)
        if (entry.id == component && entry.type == Component::service)
            selected = &entry;
    if (!selected)
        return Error::not_supported;
    const auto generation = installedGeneration(installed);
    Caller principal{&installed.manifest, selected,
        {identityHash(package), identityHash(installed.manifest.publisher), generation}, {}, true,
        true, installed.factory};
    if (!generation || (error = authorizeCaller(principal, "app.background.execute")) != Error::ok)
        return generation ? error : Error::integrity_failed;
    if (platform_.background_low_energy && platform_.background_low_energy())
        return Error::invalid_state;
    for (const auto &slot : b.slots)
        if (slot.runtime && std::string_view(b.records[slot.record].info.package.data()) == package)
            return Error::invalid_state;
    auto free = b.slots.size();
    size_t running = 0;
    for (size_t i = 0; i < b.slots.size(); ++i)
        if (b.slots[i].runtime)
            ++running;
        else if (free == b.slots.size())
            free = i;
    if (running >= policy.maxServices || free == b.slots.size() ||
        selected->budget.instructions > policy.quantumInstructions ||
        b.reserved() > policy.memoryBytes ||
        selected->budget.internal_bytes > policy.memoryBytes - b.reserved())
        return Error::resource_exhausted;
    const auto available = platform_.background_available_memory
        ? platform_.background_available_memory()
        : platform_.memory;
    if (!fits(selected->budget, available))
        return Error::resource_exhausted;
    size_t record = b.recordCount;
    for (size_t i = 0; i < b.recordCount; ++i)
        if (std::string_view(b.records[i].info.package.data()) == package) {
            record = i;
            break;
        }
    if (record == b.recordCount && record == b.records.size())
        return Error::quota_exceeded;
    if (record == b.recordCount)
        ++b.recordCount;
    auto &info = b.records[record];
    info = {};
    copyText(info.info.package, package);
    copyText(info.info.component, component);
    copyText(info.info.label, uiText(installed.manifest.name, 180));
    info.principal = principal.principal;
    info.packageGeneration = generation;
    auto &slot = b.slots[free];
    slot.record = record;
    slot.host.key = {100 + free, ++b.serial};
    slot.host.closing = false;
    slot.inbox.reset(slot.host.key);
    slot.runtime.reset(new (std::nothrow) ServiceRuntime);
    if (!slot.runtime)
        return Error::resource_exhausted;
    info.info.state = ServiceState::starting;
    info.info.instance = slot.host.key;
    error = slot.runtime->launch(installed, component, available, platform_.capabilities,
        slot.host.key, instanceHost(slot.host));
    if (error == Error::ok && slot.runtime->packageIdentity() != generation)
        error = Error::integrity_failed;
    if (error != Error::ok) {
        slot.host.closing = true;
        slot.runtime->stop(ServiceReason::error);
        slot.runtime.reset();
        info.info.state = ServiceState::faulted;
        info.info.error = error;
        info.info.reason = ServiceReason::resources;
        return error;
    }
    if (selected->capabilities & CapComponentIpc) {
        BackgroundState::Shared *state = nullptr;
        for (auto &entry : b.shared)
            if (entry && std::string_view(entry->package.data()) == package)
                state = entry.get();
        if (!state)
            for (auto &entry : b.shared)
                if (!entry) {
                    if (b.reserved() + sizeof(BackgroundState::Shared) > policy.memoryBytes)
                        break;
                    entry.reset(new (std::nothrow) BackgroundState::Shared);
                    if (entry) {
                        state = entry.get();
                        copyText(state->package, package);
                        state->publisher = principal.principal.publisher;
                    }
                    break;
                }
        if (!state) {
            slot.host.closing = true;
            slot.runtime->stop(ServiceReason::resources);
            slot.runtime.reset();
            info.info.state = ServiceState::faulted;
            return Error::resource_exhausted;
        }
        if (state->pending)
            loadSharedState(package);
    }
    info.info.state = ServiceState::running;
    info.info.reserved = uint32_t(slot.runtime->reservation());
    (void)sensors_.activate(slot.host.key);
    return Error::ok;
}
Error Core::stopService(std::string_view package, ServiceReason reason) {
    if (!(platform_.capabilities & CapBackground))
        return Error::not_supported;
    if (!background_)
        return Error::source_unavailable;
    for (auto &slot : background_->slots)
        if (slot.runtime &&
            std::string_view(background_->records[slot.record].info.package.data()) == package) {
            slot.host.closing = true;
            sensors_.cancelOwner(slot.host.key);
            slot.inbox.reset({});
            background_->foreground.cancelFrom(slot.host.key);
            for (auto &entry : background_->slots)
                entry.inbox.cancelFrom(slot.host.key);
            if (background_->flush && background_->flush->owner == slot.host.key)
                background_->flush.reset();
            const auto error = slot.runtime->stop(reason);
            auto &record = background_->records[slot.record];
            record.info.reason = reason;
            record.info.state = ServiceState::stopped;
            record.info.reserved = 0;
            record.info.used = 0;
            slot.runtime.reset();
            if (error != Error::ok) {
                record.info.state = ServiceState::faulted;
                record.info.reason =
                    error == Error::execution_limit ? ServiceReason::quota : ServiceReason::error;
                record.info.error = error;
                if (disableAuto(settings_, package, record.packageGeneration))
                    (void)persistSettings();
            }
            return error;
        }
    return Error::source_unavailable;
}
void Core::faultService(std::string_view package, Error error) {
    if (!background_)
        return;
    const auto reason = error == Error::execution_limit || error == Error::quota_exceeded
        ? ServiceReason::quota
        : error == Error::resource_exhausted ? ServiceReason::resources
                                             : ServiceReason::error;
    (void)stopService(package, reason);
    for (auto &record : background_->records)
        if (std::string_view(record.info.package.data()) == package) {
            record.info.state = ServiceState::faulted;
            record.info.error = error;
            if (disableAuto(settings_, package, record.packageGeneration))
                (void)persistSettings();
            break;
        }
}
void Core::stopPackageServices(std::string_view package, ServiceReason reason) {
    if (background_)
        (void)stopService(package, reason);
}
void Core::reconcileServices() {
    if (!background_)
        return;
    for (auto &slot : background_->slots)
        if (slot.runtime) {
            const auto &record = background_->records[slot.record];
            Caller principal{&slot.runtime->manifest(), &slot.runtime->component(),
                record.principal, slot.host.key, true, true, false};
            if (authorizeCaller(principal, "app.background.execute") != Error::ok) {
                const std::string package = record.info.package.data();
                (void)stopService(package, ServiceReason::permission);
            }
        }
}
bool Core::foregroundEventsPending() const {
    return background_ && background_->foreground.owner == foregroundBinding_.key &&
        background_->foreground.count;
}
uint64_t Core::backgroundNextWake() const {
    if (!background_)
        return UINT64_MAX;
    const auto &b = *background_;
    uint64_t next = UINT64_MAX;
    if (b.autostartPending || b.flush || foregroundEventsPending())
        return 0;
    for (const auto &state : b.shared)
        if (state && state->pending)
            return 0;
    const auto now = platform_.monotonic();
    const bool budget = b.nextWindow <= now ||
        (b.instructions >= platform_.background.quantumInstructions &&
            b.nativeCalls >= platform_.background.quantumNativeCalls && b.wallUs);
    for (const auto &slot : b.slots)
        if (slot.runtime && b.records[slot.record].info.state == ServiceState::running) {
            auto ready = slot.inbox.count
                ? uint64_t(0)
                : std::min(slot.runtime->nextWake(), sensors_.nextWake(slot.host.key));
            if (!budget && ready <= now)
                ready = b.nextWindow;
            next = std::min(next, ready);
        }
    return next;
}
} // namespace flint

namespace flint {
Error Core::ipcCall(HostBinding &host, const IpcRequest &request, IpcReply &reply) {
    if (uint8_t(request.operation) > uint8_t(IpcOperation::flush))
        return Error::invalid_argument;
    const auto execution = caller(host);
    if (!execution.active || host.closing)
        return Error::invalid_state;
    if (!execution.component || !(execution.component->capabilities & CapComponentIpc) ||
        !(platform_.capabilities & CapComponentIpc))
        return Error::not_supported;
    backgroundBegin();
    if (!background_)
        return Error::resource_exhausted;
    const auto required = request.operation == IpcOperation::send ? "app.ipc" : "storage.private";
    auto error = authorize(host, required);
    if (error != Error::ok)
        return error;
    auto &b = *background_;
    const auto package = std::string_view(execution.manifest->id);
    if (request.operation == IpcOperation::send) {
        const std::string_view target(request.target.data(),
            std::find(request.target.begin(), request.target.end(), char(0)) -
                request.target.begin()),
            topic(request.key.data(),
                std::find(request.key.begin(), request.key.end(), char(0)) - request.key.begin());
        if (!keyText(target, 64) || !keyText(topic) || !validValue(request.value))
            return Error::invalid_argument;
        BackgroundState::Inbox *destination = nullptr;
        sensorOwner();
        if (runtime_.state() == State::active &&
            (runtime_.component().capabilities & CapComponentIpc) &&
            runtime_.manifest().id == package &&
            runtime_.packageIdentity() == execution.principal.generation &&
            runtime_.component().id == target) {
            if (b.foreground.owner != foregroundBinding_.key)
                b.foreground.reset(foregroundBinding_.key);
            destination = &b.foreground;
        }
        for (auto &slot : b.slots)
            if (slot.runtime &&
                std::string_view(b.records[slot.record].info.package.data()) == package &&
                b.records[slot.record].packageGeneration == execution.principal.generation &&
                (slot.runtime->component().capabilities & CapComponentIpc) &&
                slot.runtime->component().id == target)
                destination = &slot.inbox;
        if (!destination)
            return Error::source_unavailable;
        IpcEvent event;
        event.kind = IpcEventKind::message;
        event.sender = host.key;
        copyText(event.component, execution.component->id);
        copyText(event.topic, topic);
        event.value = request.value;
        return destination->push(event);
    }
    BackgroundState::Shared *state = nullptr;
    for (auto &item : b.shared)
        if (item && std::string_view(item->package.data()) == package)
            state = item.get();
    if (!state) {
        auto free = b.shared.end();
        for (auto it = b.shared.begin(); it != b.shared.end(); ++it)
            if (!*it) {
                free = it;
                break;
            }
        if (free == b.shared.end() ||
            b.reserved() + sizeof(BackgroundState::Shared) > platform_.background.memoryBytes)
            return Error::resource_exhausted;
        free->reset(new (std::nothrow) BackgroundState::Shared);
        if (!*free)
            return Error::resource_exhausted;
        state = free->get();
        copyText(state->package, package);
        state->publisher = execution.principal.publisher;
    }
    if (state->pending)
        return Error::source_unavailable;
    if (state->publisher != execution.principal.publisher)
        return Error::permission_denied;
    if (state->loaded != Error::ok)
        return state->loaded;
    reply.revision = state->revision;
    if (request.operation == IpcOperation::read) {
        const std::string_view key(request.key.data(),
            std::find(request.key.begin(), request.key.end(), char(0)) - request.key.begin());
        if (!keyText(key))
            return Error::invalid_argument;
        for (size_t i = 0; i < state->count; ++i)
            if (std::string_view(state->values[i].key.data()) == key) {
                reply.value = state->values[i].value;
                return Error::ok;
            }
        return Error::source_unavailable;
    }
    if (request.operation == IpcOperation::commit) {
        if (request.expectedRevision != state->revision)
            return Error::conflict;
        if (!request.count || request.count > request.writes.size())
            return Error::invalid_argument;
        if (state->revision >= uint64_t(INT64_MAX))
            return Error::resource_exhausted;
        std::array<size_t, 8> indices{};
        size_t count = state->count;
        for (size_t i = 0; i < request.count; ++i) {
            const auto &write = request.writes[i];
            const std::string_view key(write.key.data(),
                std::find(write.key.begin(), write.key.end(), char(0)) - write.key.begin());
            if (!keyText(key) || !validValue(write.value))
                return Error::invalid_argument;
            for (size_t j = 0; j < i; ++j)
                if (request.writes[j].key == write.key)
                    return Error::invalid_argument;
            indices[i] = state->values.size();
            for (size_t j = 0; j < state->count; ++j)
                if (state->values[j].key == write.key)
                    indices[i] = j;
            if (indices[i] == state->values.size())
                indices[i] = count++;
        }
        if (count > state->values.size())
            return Error::quota_exceeded;
        for (size_t i = 0; i < request.count; ++i)
            state->values[indices[i]] = request.writes[i];
        state->count = count;
        reply.revision = ++state->revision;
        IpcEvent event;
        event.kind = IpcEventKind::state_changed;
        event.revision = state->revision;
        sensorOwner();
        if (runtime_.state() == State::active &&
            (runtime_.component().capabilities & CapComponentIpc) &&
            runtime_.manifest().id == package &&
            runtime_.packageIdentity() == execution.principal.generation) {
            if (b.foreground.owner != foregroundBinding_.key)
                b.foreground.reset(foregroundBinding_.key);
            (void)b.foreground.push(event);
        }
        for (auto &slot : b.slots)
            if (slot.runtime &&
                std::string_view(b.records[slot.record].info.package.data()) == package &&
                b.records[slot.record].packageGeneration == execution.principal.generation)
                (void)slot.inbox.push(event);
        return Error::ok;
    }
    if (request.operation != IpcOperation::flush)
        return Error::invalid_argument;
    const auto at = platform_.monotonic();
    if (b.flush ||
        (b.flushed && (at < b.lastFlush || at - b.lastFlush < platform_.background.minFlushMs)))
        return Error::quota_exceeded;
    if (b.nextTicket >= uint64_t(INT64_MAX))
        return Error::resource_exhausted;
    Writer writer;
    writer.n(0x31535046);
    writer.n(state->publisher, 8);
    writer.n(state->revision, 8);
    writer.n(state->count, 1);
    for (size_t i = 0; i < state->count; ++i) {
        writer.text(state->values[i].key.data());
        writeValue(writer, state->values[i].value);
    }
    writer.n(crc32(writer.data));
    if (writer.data.size() > 4096 ||
        b.reserved() + writer.data.capacity() + sizeof(BackgroundState::Flush) >
            platform_.background.memoryBytes)
        return Error::resource_exhausted;
    b.flush.reset(new (std::nothrow) BackgroundState::Flush{
        host.key, std::string(package), std::move(writer.data), ++b.nextTicket, state->revision});
    if (!b.flush)
        return Error::resource_exhausted;
    reply.ticket = b.nextTicket;
    return Error::ok;
}
bool Core::ipcEvent(HostBinding &host, IpcEvent &event) {
    const auto execution = caller(host);
    if (!background_ || host.closing || !execution.active || !execution.component ||
        !(execution.component->capabilities & CapComponentIpc))
        return false;
    if (host.service && authorize(host, "app.background.execute") != Error::ok)
        return false;
    auto *inbox = host.service ? &background_->slots[host.slot].inbox : &background_->foreground;
    if (inbox->owner != host.key) {
        inbox->reset(host.key);
        return false;
    }
    for (size_t i = 0; i < 8 && inbox->pop(event); ++i) {
        const auto permission = event.kind == IpcEventKind::message ? "app.ipc" : "storage.private";
        if (event.kind == IpcEventKind::policy || authorize(host, permission) == Error::ok)
            return true;
    }
    return false;
}
bool Core::backgroundTick(uint64_t at) {
    if (!background_)
        return false;
    auto &b = *background_;
    const auto &policy = platform_.background;
    reconcileServices();
    for (auto &state : b.shared)
        if (state && state->pending) {
            loadSharedState(state->package.data());
            break;
        }
    const bool low = platform_.background_low_energy && platform_.background_low_energy();
    if (low != b.low) {
        b.low = low;
        for (auto &slot : b.slots)
            if (slot.runtime) {
                auto &info = b.records[slot.record].info;
                if (low && info.state == ServiceState::running) {
                    slot.host.closing = true;
                    sensors_.cancelOwner(slot.host.key);
                    const auto error = slot.runtime->pause(ServiceReason::energy);
                    if (error != Error::ok) {
                        const std::string package = info.package.data();
                        faultService(package, error);
                    } else {
                        info.state = ServiceState::paused;
                        info.reason = ServiceReason::energy;
                    }
                } else if (!low && info.state == ServiceState::paused &&
                    info.reason == ServiceReason::energy) {
                    slot.host.closing = false;
                    const auto error = slot.runtime->resume(at);
                    if (error != Error::ok) {
                        const std::string package = info.package.data();
                        faultService(package, error);
                    } else {
                        info.state = ServiceState::running;
                        info.reason = ServiceReason::none;
                        (void)sensors_.activate(slot.host.key);
                        IpcEvent event;
                        event.kind = IpcEventKind::policy;
                        event.value.kind = IpcValueKind::boolean;
                        event.value.integer = 0;
                        (void)slot.inbox.push(event);
                    }
                }
            }
    }
    if (b.autostartPending) {
        b.autostartPending = false;
        // Autostart is never a dependency of begin/recovery. Admission failures remain metadata.
        packages_.visit([&](const Installed &p) {
            for (const auto &c : p.manifest.components)
                if (c.type == Component::service) {
                    const auto gen = installedGeneration(p);
                    if (autoEnabled(settings_, p.manifest.id, gen)) {
                        Caller call{&p.manifest, &c,
                            {identityHash(p.manifest.id), identityHash(p.manifest.publisher), gen},
                            {}, true, true, p.factory};
                        if (authorizeCaller(call, "app.background.autostart") == Error::ok &&
                            authorizeCaller(call, "app.background.execute") == Error::ok)
                            (void)startService(p.manifest.id, c.id);
                    }
                }
            return true;
        });
    }
    if (b.flush && !install_) {
        auto pending = std::move(b.flush);
        auto *owner = binding(pending->owner);
        if (owner && caller(*owner).active && authorize(*owner, "storage.private") == Error::ok) {
            const auto success = writeAtomic(
                packages_.root() + "/shared-" + pending->package + ".bin", pending->data);
            b.lastFlush = at;
            b.flushed = true;
            IpcEvent event;
            event.kind = IpcEventKind::persisted;
            event.ticket = pending->ticket;
            event.revision = pending->revision;
            event.error = success ? Error::ok : Error::io_error;
            auto &queue = owner->service ? b.slots[owner->slot].inbox : b.foreground;
            (void)queue.push(event);
        }
    }
    // Loading an autostart VM (or flushing state) can advance the clock during
    // this pass. Dispatch must not precede the VM's freshly initialized clock.
    at = platform_.monotonic();
    if (at >= b.nextWindow || at < b.windowAt) {
        b.windowAt = at;
        b.nextWindow = at > UINT64_MAX - policy.windowMs ? UINT64_MAX : at + policy.windowMs;
        b.instructions = policy.instructionsPerWindow;
        b.nativeCalls = policy.nativeCallsPerWindow;
        b.wallUs = policy.wallUsPerWindow;
    }
    // Foreground/input and install operations have already run. Reserve a whole quantum
    // before consuming events; lack of global tokens defers work rather than faulting it.
    for (size_t attempt = 0; attempt < b.slots.size(); ++attempt) {
        const auto index = b.cursor++ % b.slots.size();
        auto &slot = b.slots[index];
        if (!slot.runtime || b.records[slot.record].info.state != ServiceState::running)
            continue;
        const auto ready = slot.inbox.count
            ? uint64_t(0)
            : std::min(slot.runtime->nextWake(), sensors_.nextWake(slot.host.key));
        if (ready > at)
            continue;
        const auto instructions = slot.runtime->component().budget.instructions;
        if (b.instructions < instructions || b.nativeCalls < policy.quantumNativeCalls || !b.wallUs)
            break;
        b.instructions -= instructions;
        b.nativeCalls -= policy.quantumNativeCalls;
        const auto start = platform_.microseconds();
        const auto wall = std::min<uint64_t>(b.wallUs, policy.callbackWallUs);
        DispatchBudget budget{instructions, policy.quantumNativeCalls,
            start > UINT64_MAX - wall ? UINT64_MAX : start + wall, policy.nativeWallUs};
        DispatchUsage usage;
        const auto result = slot.runtime->tick(at, budget, usage);
        b.instructions += instructions - std::min(instructions, usage.instructions);
        b.nativeCalls +=
            policy.quantumNativeCalls - std::min(policy.quantumNativeCalls, usage.nativeCalls);
        const auto elapsed = platform_.microseconds() - start;
        b.wallUs -= std::min(b.wallUs, elapsed);
        auto &record = b.records[slot.record];
        auto &info = record.info;
        ++info.callbacks;
        ++info.wakeups;
        info.instructions += usage.instructions;
        info.nativeCalls += usage.nativeCalls;
        info.maxCallbackUs = std::max(info.maxCallbackUs, elapsed);
        info.used = uint32_t(slot.runtime->used() + slot.runtime->nativeEstimate());
        info.peak = uint32_t(slot.runtime->peak());
        info.eventRejected = slot.inbox.rejected;
        info.eventCoalesced = slot.inbox.coalesced;
        if (result != Error::ok) {
            const std::string package = info.package.data();
            faultService(package, result);
        }
        // At most one service callback batch per Core pass, rotating ready services.
        break;
    }
    return false; // Background callbacks never request rendering or wake the display.
}
} // namespace flint

namespace flint {
void Core::loadSharedState(std::string_view package) {
    if (!background_)
        return;
    BackgroundState::Shared *state = nullptr;
    for (auto &entry : background_->shared)
        if (entry && std::string_view(entry->package.data()) == package)
            state = entry.get();
    if (!state || !state->pending)
        return;
    state->pending = false;
    const auto path = packages_.root() + "/shared-" + std::string(package) + ".bin";
    File file;
    if (file.open(path)) {
        const auto saved = readText(path, 4096);
        if (saved.empty())
            state->loaded = Error::integrity_failed;
        else {
            Reader input{{reinterpret_cast<const uint8_t *>(saved.data()), saved.size()}};
            if (input.n() != 0x31535046 || input.n(8) != state->publisher)
                state->loaded = Error::integrity_failed;
            state->revision = input.n(8);
            if (state->revision > uint64_t(INT64_MAX))
                input.valid = false;
            state->count = size_t(input.n(1));
            if (state->count > state->values.size()) {
                state->count = 0;
                state->loaded = Error::integrity_failed;
            }
            for (size_t i = 0; i < state->count; ++i) {
                const auto key = input.text(32);
                copyText(state->values[i].key, key);
                state->values[i].value = readValue(input);
                if (!keyText(key))
                    input.valid = false;
                for (size_t j = 0; j < i; ++j)
                    if (state->values[j].key == state->values[i].key)
                        input.valid = false;
            }
            const auto checksum = input.n();
            if (!input.done() || saved.size() < 4 ||
                checksum !=
                    crc32(std::span(
                        reinterpret_cast<const uint8_t *>(saved.data()), saved.size() - 4)))
                state->loaded = Error::integrity_failed;
        }
    } else {
        struct stat info{};
        if (::stat(path.c_str(), &info) == 0)
            state->loaded = Error::io_error;
    }
    IpcEvent event;
    event.kind = IpcEventKind::state_changed;
    event.revision = state->revision;
    event.error = state->loaded;
    sensorOwner();
    if (runtime_.state() == State::active &&
        (runtime_.component().capabilities & CapComponentIpc) &&
        runtime_.manifest().id == package) {
        if (background_->foreground.owner != foregroundBinding_.key)
            background_->foreground.reset(foregroundBinding_.key);
        (void)background_->foreground.push(event);
    }
    for (auto &slot : background_->slots)
        if (slot.runtime && slot.runtime->manifest().id == package)
            (void)slot.inbox.push(event);
}
Error Core::serviceRow(uint16_t index, ServiceInfo &out) {
    if (!(platform_.capabilities & CapBackground))
        return Error::not_supported;
    size_t current = 0;
    bool found = false;
    packages_.visit([&](const Installed &package) {
        for (const auto &component : package.manifest.components)
            if (component.type == Component::service) {
                if (current++ != index)
                    continue;
                out = {};
                copyText(out.package, package.manifest.id);
                copyText(out.component, component.id);
                copyText(out.label, uiText(package.manifest.name, 180));
                const auto generation = installedGeneration(package);
                if (background_)
                    for (size_t i = 0; i < background_->recordCount; ++i)
                        if (std::string_view(background_->records[i].info.package.data()) ==
                                package.manifest.id &&
                            background_->records[i].packageGeneration == generation)
                            out = background_->records[i].info;
                Caller target{&package.manifest, &component,
                    {identityHash(package.manifest.id), identityHash(package.manifest.publisher),
                        generation},
                    {}, true, true, package.factory};
                out.executeGranted = authorizeCaller(target, "app.background.execute") == Error::ok;
                out.autostartGranted =
                    authorizeCaller(target, "app.background.autostart") == Error::ok;
                out.autostart = autoEnabled(settings_, package.manifest.id, generation);
                found = true;
                return false;
            }
        return true;
    });
    return found ? Error::ok : Error::source_unavailable;
}
Error Core::serviceControl(HostBinding &host, const ServiceRequest &request, ServiceInfo &reply) {
    if (!(platform_.capabilities & CapBackground))
        return Error::not_supported;
    const auto execution = caller(host);
    if (!execution.active || !execution.system || execution.service ||
        authorize(host, "system.diagnostics") != Error::ok)
        return Error::permission_denied;
    if (request.operation == ServiceOperation::list)
        return serviceRow(request.index, reply);
    // Same trusted path is used by local factory management and authenticated Companion.
    Writer command;
    command.n(uint8_t(request.operation) - 1, 1);
    command.text(request.package.data());
    command.text(request.component.data());
    Reader input{command.data};
    Writer output;
    return handleBackground(Method::service_control, input, output, {true, true, UINT64_MAX});
}
Error Core::handleBackground(Method method, Reader &input, Writer &output, const Session &) {
    if (!(platform_.capabilities & CapBackground))
        return Error::not_supported;
    backgroundBegin();
    if (!background_)
        return Error::resource_exhausted;
    auto &b = *background_;
    if (method == Method::services) {
        const auto offset = input.n(2), limit = input.n(1);
        if (!input.done() || !limit || limit > 8 || offset + limit > 65536)
            return Error::invalid_argument;
        std::array<ServiceInfo, 8> rows{};
        size_t count = 0;
        for (; count < limit; ++count) {
            const auto error = serviceRow(uint16_t(offset + count), rows[count]);
            if (error == Error::source_unavailable)
                break;
            if (error != Error::ok)
                return error;
        }
        ServiceInfo extra;
        const bool more = offset + count <= UINT16_MAX &&
            serviceRow(uint16_t(offset + count), extra) == Error::ok;
        output.n(count, 1);
        output.n(more, 1);
        output.n(platform_.background.maxServices, 1);
        output.n(platform_.background.memoryBytes);
        output.n(platform_.background.instructionsPerWindow);
        output.n(platform_.background.nativeCallsPerWindow);
        output.n(platform_.background.windowMs);
        output.n(b.low, 1);
        for (size_t i = 0; i < count; ++i) {
            const auto &r = rows[i];
            output.text(r.package.data());
            output.text(r.component.data());
            output.text(r.label.data());
            output.n(uint8_t(r.state), 1);
            output.n(uint8_t(r.reason), 1);
            output.n(uint16_t(r.error), 2);
            output.n(r.instance.id, 8);
            output.n(r.instance.generation, 8);
            output.n(r.used);
            output.n(r.peak);
            output.n(r.reserved);
            for (auto n : {r.callbacks, r.instructions, r.nativeCalls, r.maxCallbackUs, r.wakeups,
                     r.eventRejected, r.eventCoalesced})
                output.n(n, 8);
            output.n(r.executeGranted, 1);
            output.n(r.autostartGranted, 1);
            output.n(r.autostart, 1);
        }
        return Error::ok;
    }
    const auto action = input.n(1);
    const auto package = input.text(64), component = input.text(64);
    if (!input.done() || action > 6 || !identifier(package) || !identifier(component))
        return Error::invalid_argument;
    if (action == 0)
        return startService(package, component);
    Installed installed;
    auto error = packages_.locate(package, installed);
    if (error != Error::ok)
        return error;
    const ComponentInfo *declaration = nullptr;
    for (const auto &c : installed.manifest.components)
        if (c.id == component && c.type == Component::service)
            declaration = &c;
    if (!declaration)
        return Error::not_supported;
    const auto generation = installedGeneration(installed);
    Caller principal{&installed.manifest, declaration,
        {identityHash(package), identityHash(installed.manifest.publisher), generation}, {}, true,
        true, installed.factory};
    if (action == 1)
        return stopService(package);
    if (action == 4 || action == 5) {
        if (action == 4 &&
            (authorizeCaller(principal, "app.background.execute") != Error::ok ||
                authorizeCaller(principal, "app.background.autostart") != Error::ok))
            return Error::permission_denied;
        const auto key = autoKey(package);
        if (!settings_.contains(key)) {
            if (action == 5)
                return Error::ok;
            if (settings_.size() >= 128)
                return Error::resource_exhausted;
        }
        const auto old = settings_;
        settings_[key] = std::to_string(generation) + (action == 4 ? ":1" : ":0");
        error = persistSettings();
        if (error != Error::ok && action == 4)
            settings_ = old;
        return error;
    }
    if (action == 6) {
        bool changed = false;
        for (auto &grant : grants_)
            if (grant.package == package && grant.permission == "app.background.execute" &&
                grant.enabled) {
                grant.enabled = false;
                changed = true;
            }
        (void)stopService(package, ServiceReason::permission);
        return changed ? persistGrants() : Error::ok;
    }
    for (auto &slot : b.slots)
        if (slot.runtime &&
            std::string_view(b.records[slot.record].info.package.data()) == package) {
            auto &info = b.records[slot.record].info;
            if (action == 2) {
                if (info.state != ServiceState::running)
                    return Error::invalid_state;
                slot.host.closing = true;
                sensors_.cancelOwner(slot.host.key);
                error = slot.runtime->pause(ServiceReason::user);
                if (error != Error::ok) {
                    faultService(package, error);
                } else {
                    info.state = ServiceState::paused;
                    info.reason = ServiceReason::user;
                    info.error = Error::ok;
                }
                return error;
            }
            if (action == 3) {
                if (info.state != ServiceState::paused)
                    return Error::invalid_state;
                if (b.low)
                    return Error::invalid_state;
                if (authorizeCaller(principal, "app.background.execute") != Error::ok)
                    return Error::permission_denied;
                slot.host.closing = false;
                error = slot.runtime->resume(platform_.monotonic());
                if (error != Error::ok) {
                    faultService(package, error);
                } else {
                    info.state = ServiceState::running;
                    info.reason = ServiceReason::none;
                    info.error = Error::ok;
                    (void)sensors_.activate(slot.host.key);
                }
                return error;
            }
        }
    return Error::source_unavailable;
}
} // namespace flint
