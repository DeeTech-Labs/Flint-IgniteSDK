#include <charconv>
#include <cmath>
#include <ctime>
#include <flint/core.h>
#include <flint/os.h>
#include <flint/registry.h>
#include <flint/services.h>
#include <sys/stat.h>
namespace flint {
static Bytes valuesBytes(const Values &values) {
    Writer w;
    w.n(values.size(), 2);
    for (auto &[key, value] : values) {
        w.text(key);
        w.text(value);
    }
    return w.data;
}
static Values valuesRead(const std::string &path) {
    auto data = readText(path, 8192);
    Reader r{{reinterpret_cast<const uint8_t *>(data.data()), data.size()}};
    Values values;
    auto count = r.n(2);
    if (count > 128)
        return {};
    for (unsigned i = 0; i < count; ++i) {
        auto k = r.text(64), v = r.text(192);
        values[k] = v;
    }
    return r.done() ? values : Values{};
}
static uint64_t identity(std::string_view text) {
    Sha256 hash;
    hash.update({reinterpret_cast<const uint8_t *>(text.data()), text.size()});
    auto digest = hash.finish();
    Reader r{digest};
    return r.n(8);
}
static uint64_t generation(const Installed &p) {
    File file;
    if (!file.open(p.path))
        return 0;
    std::array<uint8_t, 64> h{};
    if (!file.source().read(&file, 0, h))
        return 0;
    Reader r{std::span(h).subspan(32)};
    return r.n(8);
}
Core::Core(std::string root, std::string factory, Platform platform)
    : platform_(std::move(platform)),
      packages_(std::move(root), std::move(factory), platform_.memory, platform_.capabilities,
          platform_.require_factory) {
    runtime_.host({this,
        [](void *context, AppService service, std::string_view key, std::string_view value,
            AppReply &reply) {
            return static_cast<Core *>(context)->appCall(service, key, value, reply);
        },
        [](void *context) { return static_cast<Core *>(context)->platform_.monotonic(); }, true});
    runtime_.resources().profilingClock(platform_.microseconds);
}
Error Core::appCall(
    AppService service, std::string_view key, std::string_view value, AppReply &reply) {
    // Runtime has checked the canonical declaration; this bridge never accepts app-supplied
    // identity.
    const auto &manifest = runtime_.manifest();
    if (service == AppService::storage_get || service == AppService::storage_set ||
        service == AppService::storage_delete) {
        const bool write = service != AppService::storage_get;
        const auto at = platform_.monotonic();
        if (write && privateWritten_ && (at < privateWriteAt_ || at - privateWriteAt_ < 1000))
            return Error::quota_exceeded;
        const auto error = privateStorage(packages_.root(), manifest, service, key, value, reply);
        if (error == Error::ok && write) {
            privateWriteAt_ = at;
            privateWritten_ = true;
        }
        return error;
    }
    if (service == AppService::time || service == AppService::monotonic) {
        if (!key.empty())
            return Error::invalid_argument;
        const auto number = service == AppService::time ? now() : platform_.monotonic();
        if (number > uint64_t(INT64_MAX))
            return Error::resource_exhausted;
        if (service == AppService::time && !number)
            return Error::source_unavailable;
        reply.number = int64_t(number);
        reply.numeric = true;
        return Error::ok;
    }
    if (service == AppService::info) {
        if (key.empty() || key == "profile")
            return reply.assign(DeviceProfile);
        if (key == "build_id")
            return reply.assign(platform_.build_id);
        return Error::source_unavailable;
    }
    if (service == AppService::power)
        return platform_.power ? platform_.power(key, reply) : Error::not_supported;
    const Values *values = service == AppService::locale ? &locale_
        : service == AppService::theme                   ? &theme_
        : service == AppService::settings                ? &settings_
                                                         : nullptr;
    if (!values)
        return Error::not_supported;
    const auto found = values->find(key);
    return found == values->end() ? Error::source_unavailable : reply.assign(found->second);
}
uint64_t Core::nextWake() const {
    const auto at = platform_.monotonic();
    if (install_ || !events_.empty())
        return at;
    auto next = runtime_.nextWake();
    if (boundProvider_ && boundAt_ <= UINT64_MAX - CompanionTtlMs)
        next = std::min(next, boundAt_ + CompanionTtlMs);
    if (epoch_ && at >= base_) {
        const auto minuteDelay = (60 - now() % 60) * 1000 - (at - base_) % 1000;
        if (minuteDelay <= UINT64_MAX - at)
            next = std::min(next, at + minuteDelay);
    }
    if (companion_.valid && companion_.at <= UINT64_MAX - 10000)
        next = std::min(next, companion_.at + 10000);
    return next;
}
Error Core::persistSettings() {
    return writeAtomic(packages_.root() + "/settings.bin", valuesBytes(settings_))
        ? Error::ok
        : Error::io_error;
}
Error Core::persistGrants() {
    Writer w;
    w.n(grants_.size(), 2);
    for (auto &g : grants_) {
        w.text(g.package);
        w.text(g.permission);
        w.text(g.scope);
        w.n(g.generation, 8);
        w.n(g.expires, 8);
        w.n(g.session, 8);
        w.n(g.action, 8);
        w.n(g.enabled, 1);
    }
    if (w.data.size() > 16384) {
        return Error::quota_exceeded;
    }
    return writeAtomic(packages_.root() + "/grants.bin", w.data) ? Error::ok : Error::io_error;
}
Error Core::revokePackage(std::string_view id) {
    const auto previous = grants_;
    for (auto &grant : grants_) {
        if (grant.package == id) {
            grant.enabled = false;
        }
    }
    const auto result = persistGrants();
    if (result != Error::ok) {
        grants_ = previous;
    }
    companion_.valid = false;
    return result;
}
void Core::refreshProviders(std::string_view id, bool removed, PreparedInstall *prepared) {
    bindingsDirty_ = true;
    for (const auto role : {Role::theme, Role::locale}) {
        const auto key = "role" + std::to_string(unsigned(role));
        const auto selected = settings_.find(key);
        if (selected == settings_.end() || !selected->second.starts_with(std::string(id) + "/")) {
            continue;
        }
        if (removed) {
            settings_.erase(key);
            persistSettings();
        }
        if (!removed && prepared) {
            bool loaded = false;
            const auto componentId = std::string_view(selected->second).substr(id.size() + 1);
            for (const auto &component : prepared->manifest().components)
                if (component.id == componentId) {
                    auto &values = role == Role::theme ? theme_ : locale_;
                    loaded = loadValues(prepared->package(), component, values) == Error::ok;
                    break;
                }
            if (loaded)
                continue;
        }
        activateRole(role);
    }
}
Error Core::begin() {
    auto error = packages_.begin();
    if (error != Error::ok)
        return error;
    base_ = platform_.monotonic();
    settings_ = valuesRead(packages_.root() + "/settings.bin");
    if (!settings_.contains("utc_offset")) {
        settings_["utc_offset"] = "180";
    }
    auto epoch = readText(packages_.root() + "/epoch", 24);
    if (!epoch.empty()) {
        char *tail{};
        auto value = strtoull(epoch.c_str(), &tail, 10);
        if (tail && !*tail && value >= 1700000000 && value <= 4102444800ULL)
            epoch_ = value;
    }
    auto data = readText(packages_.root() + "/grants.bin", 16384);
    Reader r{{reinterpret_cast<const uint8_t *>(data.data()), data.size()}};
    auto count = r.n(2);
    if (count <= 128) {
        for (unsigned i = 0; i < count; ++i) {
            PermissionRecord g;
            g.package = r.text(64);
            g.permission = r.text(80);
            g.scope = r.text(192);
            g.generation = r.n(8);
            g.expires = r.n(8);
            g.session = r.n(8);
            g.action = r.n(8);
            g.enabled = r.n(1);
            if (!g.session && !g.action)
                grants_.push_back(std::move(g));
        }
        if (!r.done())
            grants_.clear();
    }
    activateRole(Role::theme);
    activateRole(Role::locale);
    return activateRole(Role::clock);
}
uint64_t Core::now() const {
    return epoch_ ? epoch_ + (platform_.monotonic() - base_) / 1000 : 0;
}
Error Core::activateRole(Role role, std::string_view selection) {
    const auto key = "role" + std::to_string(unsigned(role));
    std::string selected(selection);
    if (selected.empty()) {
        auto i = settings_.find(key);
        if (i != settings_.end())
            selected = i->second;
    }
    Error result = Error::source_unavailable;
    for (unsigned pass = 0; pass < (selection.empty() ? 2u : 1u); ++pass) {
        packages_.visit([&](const Installed &installed) {
            for (const auto &component : installed.manifest.components) {
                const auto full = installed.manifest.id + "/" + component.id;
                const bool choose = pass == 0 ? !selected.empty() && full == selected
                                              : installed.factory && component.role == role;
                if (!choose) {
                    continue;
                }
                if (role == Role::theme) {
                    result = loadValues(installed, component, theme_);
                } else if (role == Role::locale) {
                    result = loadValues(installed, component, locale_);
                } else {
                    result = runtime_.launch(
                        installed, component.id, platform_.memory, platform_.capabilities);
                }
                if (result == Error::ok) {
                    if (!selection.empty()) {
                        const auto previous = settings_;
                        settings_[key] = full;
                        result = persistSettings();
                        if (result != Error::ok) {
                            settings_ = previous;
                        }
                    }
                    return false;
                }
            }
            return true;
        });
        if (result == Error::ok) {
            return result;
        }
    }
    return result;
}

Error Core::launch(std::string_view id, std::string_view component) {
    Installed p;
    auto error = packages_.locate(id, p);
    if (error != Error::ok)
        return error;
    auto result = runtime_.launch(p, component, platform_.memory, platform_.capabilities);
    if (result != Error::ok) {
        lastLogicError_ = result;
        lastDiagnostic_ = runtime_.diagnostic();
        lastLogicPackage_ = id;
        lastLogicComponent_ = component;
        if (runtime_.state() == State::empty) {
            const auto failed = std::string(id) + "/" + std::string(component);
            const auto selected = settings_.find("role1");
            if (selected != settings_.end() && selected->second == failed) {
                settings_.erase(selected);
                persistSettings();
            }
            activateRole(Role::clock);
        }
    }
    if (result == Error::ok) {
        companion_.valid = false;
        events_.post({3, 0, runtime_.generation()});
    }
    return result;
}
bool Core::canReceiveMetrics(uint64_t providerSession) {
    const auto &m = runtime_.manifest();
    bool declared = false;
    for (auto &d : m.permissions)
        if (d.permission == "companion.pc.metrics" && d.scope == "cpu,ram")
            declared = true;
    if (!declared)
        return false;
    if (metricRuntime_ != runtime_.generation() || metricCatalog_ != packages_.revision()) {
        metricRuntime_ = runtime_.generation();
        metricCatalog_ = packages_.revision();
        Installed installed;
        metricIdentityValid_ = packages_.locate(m.id, installed) == Error::ok;
        if (metricIdentityValid_) {
            metricPrincipal_ = {identity(m.id), identity(m.publisher), generation(installed)};
            metricIdentityValid_ = metricPrincipal_.generation != 0 &&
                metricPrincipal_.generation == runtime_.packageIdentity() &&
                installed.manifest.publisher == m.publisher;
            metricFactory_ = installed.factory;
        }
    }
    if (!metricIdentityValid_ || runtime_.state() != State::active)
        return false;
    Call call{metricPrincipal_, "cpu,ram", platform_.monotonic(), providerSession, 0,
        platform_.capabilities, true, true, metricFactory_};
    // Recheck live grants without allocating a temporary collection or caching a decision.
    const auto &spec = *permission("companion.pc.metrics");
    for (const auto &g : grants_) {
        if (g.package != m.id)
            continue;
        Grant grant{{metricPrincipal_.app, metricPrincipal_.publisher, g.generation}, g.permission,
            g.scope, g.expires, g.session, g.action, g.enabled};
        if (authorize(spec, call, std::span(&grant, 1)) == Error::ok)
            return true;
    }
    return false;
}
void Core::refreshCompanionDemand() {
    const auto at = platform_.monotonic();
    if (boundProvider_ && (at < boundAt_ || at - boundAt_ >= CompanionTtlMs)) {
        boundProvider_ = boundGeneration_ = 0;
        sampleReceived_ = false;
        companion_.valid = false;
        bindingsDirty_ = true;
        ++demandGeneration_;
        events_.post({2, 0, runtime_.generation()});
    }
    bool declared = false;
    if (runtime_.state() == State::active)
        for (const auto &permission : runtime_.manifest().permissions)
            declared |=
                permission.permission == "companion.pc.metrics" && permission.scope == "cpu,ram";
    const auto state = !declared ? CompanionDemand::no_consumer
        : canReceiveMetrics()    ? CompanionDemand::active
                                 : CompanionDemand::permission_denied;
    if (demandRuntime_ == runtime_.consumerGeneration() && demandCatalog_ == packages_.revision() &&
        demand_ == state)
        return;
    demandRuntime_ = runtime_.consumerGeneration();
    demandCatalog_ = packages_.revision();
    demand_ = state;
    ++demandGeneration_;
    boundProvider_ = boundGeneration_ = 0;
    sampleReceived_ = false;
    if (companion_.valid) {
        companion_.valid = false;
        bindingsDirty_ = true;
        events_.post({2, 0, runtime_.generation()});
    }
}
uint64_t Core::companionNextWake(const Session &session) {
    refreshCompanionDemand();
    if (!session.id || !session.authenticated || !session.compatible)
        return UINT64_MAX;
    for (const auto &subscriber : companionSubscribers_)
        if (subscriber.session == session.id && subscriber.sentGeneration != demandGeneration_)
            return subscriber.sent && subscriber.sentAt <= UINT64_MAX - 100
                ? subscriber.sentAt + 100
                : platform_.monotonic();
    return UINT64_MAX;
}
bool Core::takeCompanionEvent(const Session &session, Frame &event) {
    refreshCompanionDemand();
    if (!session.id || !session.authenticated || !session.compatible)
        return false;
    for (auto &subscriber : companionSubscribers_) {
        if (subscriber.session != session.id || subscriber.sentGeneration == demandGeneration_)
            continue;
        const auto at = platform_.monotonic();
        if (subscriber.sent && (at < subscriber.sentAt || at - subscriber.sentAt < 100))
            return false;
        Writer payload;
        payload.n(CompanionDemandVersion, 2);
        payload.n(1, 1);
        payload.n(demandGeneration_, 8);
        payload.n(uint8_t(demand_), 1);
        payload.n(runtime_.consumerGeneration(), 8);
        payload.n(CompanionSampleIntervalMs);
        payload.n(CompanionTtlMs);
        event = {Method::companion_state, 0, false, std::move(payload.data)};
        subscriber.sent = true;
        subscriber.sentGeneration = demandGeneration_;
        subscriber.sentAt = at;
        return true;
    }
    return false;
}
Error Core::handleCompanion(Method method, Reader &in, Session &session) {
    if (!(platform_.capabilities & CapCompanionDemand))
        return Error::not_supported;
    if (!session.id)
        return Error::permission_denied;
    const auto version = in.n(2);
    const auto kind = in.n(1);
    if (!in.valid || version != CompanionDemandVersion)
        return Error::invalid_argument;
    auto subscriber = std::find_if(companionSubscribers_.begin(), companionSubscribers_.end(),
        [&](const auto &item) { return item.session == session.id; });
    if (method == Method::companion_subscribe) {
        if (!in.done() || kind > 1)
            return Error::invalid_argument;
        if (!kind) {
            if (subscriber != companionSubscribers_.end())
                *subscriber = {};
            if (boundProvider_ == session.id) {
                boundProvider_ = boundGeneration_ = 0;
                ++demandGeneration_;
                sampleReceived_ = false;
                if (companion_.providerSession == session.id) {
                    companion_.valid = false;
                    bindingsDirty_ = true;
                    events_.post({2, 0, runtime_.generation()});
                }
            }
            return Error::ok;
        }
        if (subscriber == companionSubscribers_.end())
            subscriber = std::find_if(companionSubscribers_.begin(), companionSubscribers_.end(),
                [](const auto &item) { return !item.session; });
        if (subscriber == companionSubscribers_.end())
            return Error::quota_exceeded;
        *subscriber = {session.id, 0, 0, false};
        return Error::ok;
    }
    const auto generation = in.n(8);
    if (!in.valid || kind != 1 || (method == Method::companion_bind && !in.done()))
        return Error::invalid_argument;
    if (subscriber == companionSubscribers_.end())
        return Error::permission_denied;
    if (generation != demandGeneration_)
        return Error::stale_handle;
    if (demand_ != CompanionDemand::active || !canReceiveMetrics(session.id))
        return Error::permission_denied;
    if (boundProvider_ && boundProvider_ != session.id)
        return Error::invalid_state;
    if (method == Method::companion_bind) {
        if (!boundProvider_ || boundGeneration_ != generation)
            boundAt_ = platform_.monotonic();
        boundProvider_ = session.id;
        boundGeneration_ = generation;
        return Error::ok;
    }
    if (boundProvider_ != session.id || boundGeneration_ != generation)
        return Error::invalid_state;
    const auto at = platform_.monotonic();
    if (sampleReceived_ && (at < sampleAt_ || at - sampleAt_ < 1000))
        return Error::quota_exceeded;
    const auto error = receiveMetrics(in, session);
    if (error == Error::ok) {
        sampleReceived_ = true;
        sampleAt_ = at;
    }
    return error;
}
Error Core::receiveMetrics(Reader &in, const Session &session) {
    if (boundProvider_ && boundProvider_ != session.id)
        return Error::invalid_state;
    if (!session.id || !canReceiveMetrics(session.id))
        return Error::permission_denied;
    const auto cpu = in.n(), ram = in.n();
    if (!in.done() || cpu > 10000 || ram > 10000)
        return Error::invalid_argument;
    const auto at = platform_.monotonic();
    if (sampleReceived_ && (at < sampleAt_ || at - sampleAt_ < 1000))
        return Error::quota_exceeded;
    companion_ = {cpu / 100.0, ram / 100.0, at, session.id, runtime_.consumerGeneration(), true};
    boundProvider_ = session.id;
    boundAt_ = at;
    sampleReceived_ = true;
    sampleAt_ = at;
    return Error::ok;
}
bool Core::tick() {
    advanceInstall();
    refreshCompanionDemand();
    const auto at = platform_.monotonic();
    bool logicDirty = false;
    if (runtime_.scripted() && runtime_.state() == State::active && runtime_.nextWake() <= at) {
        // Failure is handled by the same factory recovery path as render errors.
        const auto selected = settings_.find("role1");
        const std::string_view selection =
            selected == settings_.end() ? std::string_view{} : selected->second;
        const auto &id = runtime_.manifest().id;
        const bool selectedActive =
            selection.size() == id.size() + 1 + runtime_.component().id.size() &&
            selection.starts_with(id) && selection[id.size()] == '/' &&
            selection.substr(id.size() + 1) == runtime_.component().id;
        std::array<char, 65> packageId{}, componentId{};
        std::copy(runtime_.manifest().id.begin(), runtime_.manifest().id.end(), packageId.begin());
        std::copy(
            runtime_.component().id.begin(), runtime_.component().id.end(), componentId.begin());
        const auto logicError = [&] {
            TimingScope timing(platform_.microseconds, logicTiming_);
            return runtime_.tick(at);
        }();
        if (logicError != Error::ok) {
            lastLogicError_ = logicError;
            lastDiagnostic_ = runtime_.diagnostic();
            lastLogicPackage_ = packageId.data();
            lastLogicComponent_ = componentId.data();
            runtime_.stop();
            if (selectedActive) {
                settings_.erase(selected);
                persistSettings();
            }
            activateRole(Role::clock);
        }
        logicDirty = logicError != Error::ok || runtime_.visualChanged();
    }
    const auto minute = now() / 60;
    if (minute != renderedMinute_) {
        renderedMinute_ = minute;
        events_.post({1, minute, runtime_.generation()});
    }
    if (companion_.valid &&
        (companion_.consumerGeneration != runtime_.consumerGeneration() ||
            !canReceiveMetrics(companion_.providerSession) ||
            platform_.monotonic() - companion_.at >= 10000)) {
        companion_.valid = false;
        events_.post({2, 0, runtime_.generation()});
    }
    bool dirty = logicDirty;
    Event event;
    while (events_.poll(event))
        if (event.generation == runtime_.generation()) {
            dirty = true;
        }
    return dirty;
}
const Values &Core::bindings() {
    const auto minute = now() / 60;
    const bool metrics = companion_.valid &&
        companion_.consumerGeneration == runtime_.consumerGeneration() &&
        platform_.monotonic() - companion_.at < 10000 &&
        canReceiveMetrics(companion_.providerSession);
    if (!bindingsDirty_ && bindingsRuntime_ == runtime_.generation() && bindingsMinute_ == minute &&
        bindingsCatalog_ == packages_.revision() && bindingsMetrics_ == metrics)
        return bindings_;
    bindingsDirty_ = false;
    bindingsRuntime_ = runtime_.generation();
    bindingsMinute_ = minute;
    bindingsCatalog_ = packages_.revision();
    bindingsMetrics_ = metrics;
    auto &values = bindings_;
    values.clear();
    auto epoch = now();
    int offset = 0;
    auto setting = settings_.find("utc_offset");
    if (setting != settings_.end()) {
        char *end = nullptr;
        const auto parsed = strtol(setting->second.c_str(), &end, 10);
        if (end && *end == 0 && parsed >= -720 && parsed <= 840) {
            offset = static_cast<int>(parsed);
        }
    }
    time_t local = epoch + offset * 60;
    struct tm tm{};
    gmtime_r(&local, &tm);
    char time[32], date[32];
    strftime(time, sizeof(time), "%H:%M", &tm);
    strftime(date, sizeof(date), "%d.%m.%Y", &tm);
    values["time.read"] = epoch ? time : "--:--";
    values["time.date"] = epoch ? date : "Sync time in Flare";
    values["system.info"] = std::string(DeviceProfile);
    for (auto &[key, value] : locale_)
        values["locale:" + key] = value;
    for (auto &[key, value] : settings_)
        values["settings:" + key] = value;
    const auto &m = runtime_.manifest();
    const bool needsPackages = std::any_of(m.permissions.begin(), m.permissions.end(),
        [](const Declaration &d) { return d.permission == "packages.list"; });
    if (needsPackages && launcherCatalog_ != packages_.revision()) {
        launcher_.clear();
        packages_.visit([&](const Installed &installed) {
            for (const auto &component : installed.manifest.components) {
                if (component.type == Component::app || component.type == Component::watchface) {
                    if (launcher_.size() + installed.manifest.name.size() >= 170) {
                        return false;
                    }
                    launcher_ += installed.manifest.name + "  ";
                }
            }
            return true;
        });
        launcherCatalog_ = packages_.revision();
    }
    if (needsPackages)
        values["packages.list"] = launcher_;
    if (metrics) {
        char b[48];
        snprintf(b, sizeof(b), "CPU %.1f%%  RAM %.1f%%", companion_.cpu, companion_.ram);
        values["companion.pc.metrics"] = b;
    }
    for (auto it = values.begin(); it != values.end();) {
        auto key = it->first;
        std::string required = key.starts_with("locale:") ? "system.locale.read"
            : key.starts_with("settings:")                ? "system.settings.read"
            : key == "time.date"                          ? "time.read"
                                                          : key;
        bool declared = false;
        for (auto &declaration : m.permissions)
            if (declaration.permission == required)
                declared = true;
        if (!declared)
            it = values.erase(it);
        else
            ++it;
    }
    return values;
}
Error Core::scene(FlintRender::Scene &scene) {
    TimingScope timing(platform_.microseconds, sceneTiming_);
    if (now() && platform_.monotonic() - lastSave_ >= 60000) {
        auto text = std::to_string(now());
        writeAtomic(packages_.root() + "/epoch",
            {reinterpret_cast<const uint8_t *>(text.data()), text.size()});
        lastSave_ = platform_.monotonic();
    }
    auto error = runtime_.render(bindings(), scene);
    if (error != Error::ok) {
        const auto failed = runtime_.manifest().id + "/" + runtime_.component().id;
        runtime_.stop();
        const auto selected = settings_.find("role1");
        if (selected != settings_.end() && selected->second == failed) {
            settings_.erase(selected);
            persistSettings();
        }
        if (activateRole(Role::clock) != Error::ok) {
            return error;
        }
        error = runtime_.render(bindings(), scene);
        if (error != Error::ok) {
            return error;
        }
    }
    for (int i = 0; i < scene.count; ++i) {
        auto &color = scene.ops[i].color;
        const char *key = color == 0xfffd ? "background"
            : color == 0xfffc             ? "text"
            : color == 0xfffe             ? "accent"
                                          : nullptr;
        if (key) {
            auto it = theme_.find(key);
            if (it != theme_.end())
                color = strtoul(it->second.c_str(), nullptr, 16);
        }
    }
    return Error::ok;
}
void Core::paint(const FlintRender::Scene &scene, FlintRender::Canvas &canvas) {
    TimingScope timing(platform_.microseconds, paintTiming_);
    FlintRender::resourceContext = &runtime_.resources();
    FlintRender::resourceMetrics = [](void *context, unsigned font, uint32_t cp) {
        return static_cast<ResourceManager *>(context)->metrics(font, cp);
    };
    FlintRender::resourceGlyph = [](void *context, unsigned font, uint32_t cp) {
        return static_cast<ResourceManager *>(context)->glyph(font, cp);
    };
    FlintRender::resourceImage = [](void *context, FlintRender::Canvas &c,
                                     const FlintRender::Op &op) {
        static_cast<ResourceManager *>(context)->image(c, op);
    };
    FlintRender::render(scene, canvas);
    FlintRender::resourceContext = nullptr;
    FlintRender::resourceGlyph = nullptr;
    FlintRender::resourceMetrics = nullptr;
    FlintRender::resourceImage = nullptr;
}
void Core::disconnect(Session &session) {
    if (installOwner_ == session.id) {
        install_.reset();
        installReply_.reset();
        installOwner_ = 0;
    }
    for (auto &g : grants_)
        if (g.session == session.id && g.session)
            g.enabled = false;
    session.compatible = false;
    for (auto &subscriber : companionSubscribers_)
        if (subscriber.session == session.id)
            subscriber = {};
    if (boundProvider_ == session.id) {
        boundProvider_ = boundGeneration_ = 0;
        ++demandGeneration_;
        sampleReceived_ = false;
    }
    if (companion_.valid && companion_.providerSession == session.id) {
        companion_.valid = false;
        bindingsDirty_ = true;
        events_.post({4, 0, runtime_.generation()});
    }
}
static Frame errorReply(const Frame &request, Error error) {
    Writer payload;
    payload.n(uint16_t(error), 2);
    return {request.method, request.id, true, std::move(payload.data)};
}
Error Core::beginInstall(const Frame &request, const Session &session) {
    if (install_ || installReply_)
        return Error::invalid_state;
    if (!session.id)
        return Error::permission_denied;
    if (!request.payload.empty())
        return Error::invalid_argument;
    const auto meta = readText(packages_.staging() + ".meta", 100);
    const auto separator = meta.find(':');
    if (separator == std::string::npos)
        return Error::invalid_state;
    uint32_t total = 0;
    const auto parsed = std::from_chars(meta.data(), meta.data() + separator, total);
    if (parsed.ec != std::errc{} || parsed.ptr != meta.data() + separator || !total ||
        total > MaxPackageBytes)
        return Error::invalid_state;
    const auto error =
        packages_.prepareCommit(install_, std::string_view(meta).substr(separator + 1), total);
    if (error != Error::ok)
        return error;
    installOwner_ = session.id;
    installRequest_ = {request.method, request.id, false, {}};
    return Error::ok;
}
void Core::advanceInstall() {
    if (!install_)
        return;
    TimingScope timing(platform_.microseconds, installTiming_);
    ++installSteps_;
    auto error = install_->advance(4096);
    if (error == Error::ok && !install_->ready())
        return;
    std::string installedId;
    if (error == Error::ok) {
        const auto candidateId = install_->manifest().id;
        error = revokePackage(candidateId);
        if (error == Error::ok) {
            const bool active = runtime_.manifest().id == candidateId;
            const auto component = runtime_.component().id;
            if (active)
                runtime_.stop();
            error = packages_.commit(*install_, installedId);
            if (error == Error::ok) {
                unlink((packages_.staging() + ".meta").c_str());
                refreshProviders(installedId, false, install_.get());
            }
            if (active) {
                const auto launchError = error == Error::ok
                    ? runtime_.launchPrepared(
                          *install_, component, platform_.memory, platform_.capabilities)
                    : launch(candidateId, component);
                if (launchError != Error::ok) {
                    lastLogicError_ = launchError;
                    lastDiagnostic_ = runtime_.diagnostic();
                    lastLogicPackage_ = candidateId;
                    lastLogicComponent_ = component;
                    packages_.rollback(candidateId);
                    launch(candidateId, component);
                    if (error == Error::ok)
                        error = launchError;
                }
            }
        }
    }
    auto reply = errorReply(installRequest_, error);
    if (error == Error::ok) {
        Writer payload;
        payload.raw(reply.payload);
        payload.text(installedId);
        reply.payload = std::move(payload.data);
        bindingsDirty_ = true;
        events_.post({4, 0, runtime_.generation()});
    } else
        ++requestErrors_;
    installReply_ = std::move(reply);
    install_.reset();
    refreshCompanionDemand();
}
std::optional<Frame> Core::process(const Frame &request, Session &session) {
    if (request.method != Method::install_commit)
        return handle(request, session);
    TimingScope timing(platform_.microseconds, requestTiming_);
    ++requests_;
    Error error = Error::ok;
    if (request.reply || !request.id || !request.payload.empty())
        error = Error::invalid_argument;
    else if (!session.authenticated)
        error = Error::permission_denied;
    else if (!session.compatible)
        error = Error::incompatible_contract;
    else if ((install_ || installReply_) && installOwner_ == session.id &&
        installRequest_.id == request.id) {
        Frame reply;
        return takeReply(session, reply) ? std::optional<Frame>(std::move(reply)) : std::nullopt;
    } else
        error = beginInstall(request, session);
    if (error == Error::ok)
        return std::nullopt;
    ++requestErrors_;
    return errorReply(request, error);
}
bool Core::replyReady(const Session &session) const {
    return installReply_ && session.id && session.authenticated && session.compatible &&
        session.id == installOwner_;
}
bool Core::takeReply(const Session &session, Frame &reply) {
    if (!replyReady(session))
        return false;
    reply = std::move(*installReply_);
    installReply_.reset();
    installOwner_ = 0;
    return true;
}
Frame Core::handle(const Frame &request, Session &session) {
    refreshCompanionDemand();
    TimingScope timing(platform_.microseconds, requestTiming_);
    ++requests_;
    if (request.method == Method::metrics || request.method == Method::companion_sample)
        ++metricRequests_;
    Frame reply{request.method, request.id, true, {}};
    Writer out;
    Reader in{request.payload};
    Error error = Error::ok;
    auto fail = [&](Error e) { error = e; };
    if (request.reply || !request.id)
        fail(Error::invalid_argument);
    else if (request.method == Method::hello) {
        auto protocol = in.n(2), revision = in.n();
        auto build = in.text(96);
        auto required = in.n(8);
        if (!in.done())
            fail(Error::invalid_argument);
        else {
            error = compatible(
                {uint16_t(protocol), uint32_t(revision), build, platform_.capabilities}, required);
            session.compatible = error == Error::ok;
            if (session.compatible) {
                out.n(ProtocolVersion, 2);
                out.n(ContractRevision);
                out.text(platform_.build_id);
                out.text(DeviceProfile);
                out.n(platform_.capabilities, 8);
            }
        }
    } else if (!session.authenticated)
        fail(Error::permission_denied);
    else if (!session.compatible)
        fail(Error::incompatible_contract);
    else if (install_ &&
        (request.method == Method::install_begin || request.method == Method::install_chunk ||
            request.method == Method::install_commit || request.method == Method::remove ||
            request.method == Method::rollback || request.method == Method::recover))
        fail(Error::invalid_state);
    else
        switch (request.method) {
        case Method::session_close:
            if (!(platform_.capabilities & CapSessionClose))
                error = Error::not_supported;
            else if (!in.done())
                error = Error::invalid_argument;
            else
                disconnect(session);
            break;
        case Method::companion_subscribe:
        case Method::companion_bind:
        case Method::companion_sample:
            error = handleCompanion(request.method, in, session);
            break;
        case Method::diagnostics: {
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            auto values = platform_.diagnostics ? platform_.diagnostics() : Values{};
            values["last_logic_error"] = std::to_string(uint16_t(lastLogicError_));
            values["lua_used_bytes"] = std::to_string(runtime_.logicUsed());
            values["lua_peak_bytes"] = std::to_string(runtime_.logicPeak());
            values["requests"] = std::to_string(requests_);
            values["request_errors"] = std::to_string(requestErrors_);
            values["metrics_requests"] = std::to_string(metricRequests_);
            values["install_steps"] = std::to_string(installSteps_);
            values["install_active"] = install_ ? "1" : "0";
            values["event_queue_peak"] = std::to_string(events_.peak());
            values["event_queue_overflows"] = std::to_string(events_.overflows());
            const auto addTiming = [&](const char *name, const Timing &sample) {
                values[std::string(name) + "_count"] = std::to_string(sample.count);
                values[std::string(name) + "_total_us"] = std::to_string(sample.totalUs);
                values[std::string(name) + "_max_us"] = std::to_string(sample.maxUs);
            };
            if (platform_.microseconds) {
                addTiming("request", requestTiming_);
                addTiming("install_step", installTiming_);
                addTiming("logic", logicTiming_);
                addTiming("scene", sceneTiming_);
                addTiming("paint", paintTiming_);
                addTiming("resource_read", runtime_.resources().readTiming());
            }
            values["resource_read_bytes"] = std::to_string(runtime_.resources().readBytes());
            values["resource_read_errors"] = std::to_string(runtime_.resources().readErrors());
            const auto &diagnostic =
                lastLogicError_ != Error::ok ? lastDiagnostic_ : runtime_.diagnostic();
            if (diagnostic.error != Error::ok || diagnostic.logCount) {
                values["logic_package"] =
                    lastLogicError_ != Error::ok ? lastLogicPackage_ : runtime_.manifest().id;
                values["logic_component"] =
                    lastLogicError_ != Error::ok ? lastLogicComponent_ : runtime_.component().id;
                values["logic_phase"] = diagnostic.phase.data();
                values["logic_message"] = diagnostic.message.data();
                values["logic_instructions"] = std::to_string(diagnostic.instructions);
                values["logic_fault_peak_bytes"] = std::to_string(diagnostic.peak);
                for (size_t i = 0; i < diagnostic.logCount; ++i)
                    values["logic_log_" + std::to_string(i)] = diagnostic.log[i].data();
            }
            // Fault details must remain readable even with many adapter counters.
            // This map is extensible; report truncation instead of losing the whole reply.
            constexpr std::string_view truncatedKey = "diagnostics_truncated";
            constexpr size_t limit = MaxPayload - 4 - 4 - truncatedKey.size() - 1;
            Writer rows;
            uint16_t count = 0;
            bool truncated = false;
            for (int pass = 0; pass < 2; ++pass)
                for (const auto &[key, value] : values) {
                    const bool fault = key.starts_with("logic_") || key == "last_logic_error";
                    if (fault != (pass == 0) || key == truncatedKey)
                        continue;
                    const auto size = key.size() + value.size() + 4;
                    if (count == 127 || key.size() > 80 || value.size() > 192 ||
                        size > limit - rows.data.size()) {
                        truncated = true;
                        continue;
                    }
                    rows.text(key);
                    rows.text(value);
                    ++count;
                }
            out.n(count + 1, 2);
            out.raw(rows.data);
            out.text(truncatedKey);
            out.text(truncated ? "1" : "0");
            break;
        }
        case Method::info: {
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            out.text(DeviceProfile);
            out.text(platform_.build_id);
            out.n(platform_.memory.internal_bytes);
            out.n(platform_.memory.external_bytes);
            out.n(platform_.free_storage ? platform_.free_storage() : 0, 8);
            out.text(runtime_.manifest().id);
            out.text(runtime_.component().id);
            out.n(now(), 8);
            out.n(runtime_.generation(), 8);
            break;
        }
        case Method::packages: {
            const auto page = in.n();
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            Writer rows;
            uint32_t total = 0;
            unsigned count = 0;
            bool full = false;
            packages_.visit([&](const Installed &installed) {
                const auto index = total++;
                if (index < page || full) {
                    return true;
                }
                Writer row;
                row.text(installed.manifest.id);
                row.text(installed.manifest.name);
                row.text(installed.manifest.publisher);
                row.n(installed.manifest.version);
                row.n(installed.factory, 1);
                row.n(installed.manifest.components.size(), 2);
                for (const auto &component : installed.manifest.components) {
                    row.text(component.id);
                    row.n(uint16_t(component.type), 2);
                    row.n(uint8_t(component.role), 1);
                }
                if (rows.data.size() + row.data.size() > MaxPayload - 16) {
                    full = true;
                } else {
                    rows.raw(row.data);
                    ++count;
                }
                return true;
            });
            if (page > total) {
                fail(Error::invalid_argument);
                break;
            }
            out.n(total);
            out.n(count, 2);
            out.raw(rows.data);
            break;
        }
        case Method::install_begin: {
            auto total = in.n();
            auto digest = in.text(64);
            if (!in.done() || total < 64 || total > MaxPackageBytes || digest.size() != 64) {
                fail(Error::invalid_argument);
                break;
            }
            bool hex = true;
            for (char c : digest)
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
                    hex = false;
            if (!hex) {
                fail(Error::invalid_argument);
                break;
            }
            auto meta = std::to_string(total) + ":" + digest;
            auto path = packages_.staging();
            auto previous = readText(path + ".meta", 100);
            struct stat st{};
            bool resume = previous == meta && stat(path.c_str(), &st) == 0 && st.st_size >= 0 &&
                uint64_t(st.st_size) <= total;
            if (!resume) {
                if (platform_.free_storage && platform_.free_storage() < total + 8192) {
                    fail(Error::quota_exceeded);
                    break;
                }
                FILE *f = fopen(path.c_str(), "wb");
                if (!f) {
                    fail(Error::io_error);
                    break;
                }
                fclose(f);
                if (!writeAtomic(path + ".meta",
                        {reinterpret_cast<const uint8_t *>(meta.data()), meta.size()})) {
                    fail(Error::io_error);
                    break;
                }
            }
            out.n(resume ? st.st_size : 0);
            break;
        }
        case Method::install_chunk: {
            auto offset = in.n();
            if (!in.valid || in.pos == in.data.size()) {
                fail(Error::invalid_argument);
                break;
            }
            auto path = packages_.staging();
            auto meta = readText(path + ".meta", 100);
            auto split = meta.find(':');
            if (split == std::string::npos) {
                fail(Error::invalid_state);
                break;
            }
            auto total = strtoul(meta.substr(0, split).c_str(), nullptr, 10);
            auto count = in.data.size() - in.pos;
            FILE *f = fopen(path.c_str(), "r+b");
            if (!f) {
                fail(Error::io_error);
                break;
            }
            fseek(f, 0, SEEK_END);
            auto size = ftell(f);
            if (size < 0 || offset > uint64_t(size) || offset + count > total) {
                fclose(f);
                fail(Error::invalid_argument);
                break;
            }
            if (offset < uint64_t(size)) {
                if (offset + count > uint64_t(size)) {
                    fclose(f);
                    fail(Error::invalid_argument);
                    break;
                }
                Bytes old(count);
                fseek(f, offset, SEEK_SET);
                if (fread(old.data(), 1, count, f) != count ||
                    !std::equal(old.begin(), old.end(), in.data.begin() + in.pos))
                    fail(Error::integrity_failed);
            } else if (fwrite(in.data.data() + in.pos, 1, count, f) != count || fflush(f) ||
                fsync(fileno(f)))
                fail(Error::io_error);
            fclose(f);
            if (error == Error::ok)
                out.n(std::max<uint64_t>(size, offset + count));
            break;
        }
        case Method::install_commit: {
            error = beginInstall(request, session);
            if (error == Error::ok) {
                while (install_)
                    advanceInstall();
                Frame result;
                if (takeReply(session, result))
                    return result;
                error = Error::invalid_state;
            }
            break;
        }
        case Method::remove: {
            auto id = in.text(64);
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            Installed target;
            error = packages_.locate(id, target);
            if (error != Error::ok || target.factory) {
                if (target.factory)
                    error = Error::permission_denied;
                break;
            }
            error = revokePackage(id);
            if (error != Error::ok) {
                break;
            }
            bool active = runtime_.manifest().id == id;
            if (active)
                runtime_.stop();
            error = packages_.remove(id);
            if (active)
                activateRole(Role::clock);
            if (error == Error::ok) {
                refreshProviders(id, true);
            }
            break;
        }
        case Method::launch: {
            auto id = in.text(64), component = in.text(64);
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            error = launch(id, component);
            break;
        }
        case Method::scene: {
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            FlintRender::Scene scene;
            error = this->scene(scene);
            if (error == Error::ok) {
                std::array<uint8_t, FlintRender::MaxEncoded> bytes{};
                auto size = FlintRender::encode(scene, bytes.data(), bytes.size());
                out.raw(std::span(bytes).first(size));
            }
            break;
        }
        case Method::time_set: {
            auto epoch = in.n(8);
            if (!in.done() || epoch < 1700000000 || epoch > 4102444800ULL) {
                fail(Error::invalid_argument);
                break;
            }
            auto previous = epoch_;
            auto previousBase = base_;
            epoch_ = epoch;
            base_ = platform_.monotonic();
            auto text = std::to_string(epoch);
            if (!writeAtomic(packages_.root() + "/epoch",
                    {reinterpret_cast<const uint8_t *>(text.data()), text.size()})) {
                epoch_ = previous;
                base_ = previousBase;
                fail(Error::io_error);
            }
            break;
        }
        case Method::role_set: {
            auto role = Role(in.n(1));
            auto selection = in.text(130);
            if (!in.done() || role == Role::none || uint8_t(role) > 5) {
                fail(Error::invalid_argument);
                break;
            }
            error = activateRole(role, selection);
            break;
        }
        case Method::settings_get: {
            if (!in.done())
                fail(Error::invalid_argument);
            else
                out.raw(valuesBytes(settings_));
            break;
        }
        case Method::settings_set: {
            auto key = in.text(64), value = in.text(192);
            if (!in.done() || key != "utc_offset") {
                fail(Error::invalid_argument);
                break;
            }
            char *tail{};
            auto offset = strtol(value.c_str(), &tail, 10);
            if (value.empty() || !tail || *tail || offset < -720 || offset > 840) {
                fail(Error::invalid_argument);
                break;
            }
            auto old = settings_;
            settings_[key] = value;
            error = persistSettings();
            if (error != Error::ok)
                settings_ = std::move(old);
            break;
        }
        case Method::permissions_get: {
            auto id = in.text(64);
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            Installed p;
            error = packages_.locate(id, p);
            if (error != Error::ok)
                break;
            out.n(p.manifest.permissions.size(), 2);
            auto gen = generation(p);
            for (auto &d : p.manifest.permissions) {
                auto *spec = permission(d.permission);
                out.text(d.permission);
                out.text(d.scope);
                out.n(spec ? uint8_t(spec->access) : 255, 1);
                bool enabled = spec && spec->access == Access::automatic;
                for (auto &g : grants_)
                    if (g.package == id && g.permission == d.permission && g.scope == d.scope &&
                        g.generation == gen && g.enabled &&
                        (!g.session || g.session == session.id) &&
                        (!g.expires || platform_.monotonic() < g.expires))
                        enabled = true;
                out.n(enabled, 1);
            }
            break;
        }
        case Method::permission_set: {
            auto id = in.text(64), name = in.text(80), scope = in.text(192);
            bool enabled = in.n(1);
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            auto *spec = permission(name);
            if (!spec || spec->access == Access::restricted || spec->access == Access::automatic) {
                fail(Error::permission_denied);
                break;
            }
            Installed p;
            error = packages_.locate(id, p);
            if (error != Error::ok)
                break;
            bool declared = false;
            for (auto &d : p.manifest.permissions)
                if (d.permission == name && d.scope == scope)
                    declared = true;
            if (!declared) {
                fail(Error::permission_denied);
                break;
            }
            if (spec->access == Access::explicit_action) {
                fail(Error::not_supported);
                break;
            }
            auto old = grants_;
            auto it = std::find_if(grants_.begin(), grants_.end(), [&](auto &g) {
                return g.package == id && g.permission == name && g.scope == scope;
            });
            if (it == grants_.end()) {
                if (grants_.size() >= 128) {
                    fail(Error::quota_exceeded);
                    break;
                }
                grants_.push_back({});
                it = grants_.end() - 1;
            }
            *it = {id, name, scope, generation(p), 0,
                spec->access == Access::session ? session.id : 0, 0, enabled};
            error = persistGrants();
            if (error != Error::ok)
                grants_ = std::move(old);
            break;
        }
        case Method::metrics:
            error = boundGeneration_ ? Error::invalid_state : receiveMetrics(in, session);
            break;
        case Method::pairing: {
            auto action = in.text(64);
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            if (!platform_.pairing) {
                fail(Error::not_supported);
                break;
            }
            out.raw(platform_.pairing(action));
            if (out.data.empty())
                fail(Error::permission_denied);
            break;
        }
        case Method::rollback: {
            auto id = in.text(64);
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            bool active = runtime_.manifest().id == id;
            auto component = runtime_.component().id;
            if (active)
                runtime_.stop();
            error = packages_.rollback(id);
            if (active) {
                if (launch(id, component) != Error::ok)
                    activateRole(Role::clock);
            }
            break;
        }
        case Method::package_read: {
            auto id = in.text(64);
            auto offset = in.n();
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            Installed p;
            error = packages_.locate(id, p);
            if (error != Error::ok)
                break;
            File file;
            if (!file.open(p.path)) {
                fail(Error::io_error);
                break;
            }
            if (offset > file.size) {
                fail(Error::invalid_argument);
                break;
            }
            auto count = std::min<uint32_t>(MaxPayload - 6, file.size - offset);
            Bytes data(count);
            if (!file.source().read(&file, offset, data)) {
                fail(Error::io_error);
                break;
            }
            out.n(file.size);
            out.raw(data);
            break;
        }
        case Method::recover: {
            if (!in.done()) {
                fail(Error::invalid_argument);
                break;
            }
            settings_.erase("role1");
            settings_.erase("role2");
            settings_.erase("role3");
            settings_.erase("role4");
            settings_.erase("role5");
            error = persistSettings();
            activateRole(Role::theme);
            activateRole(Role::locale);
            activateRole(Role::clock);
            break;
        }
        default:
            fail(Error::not_supported);
            break;
        }
    if (error == Error::ok && request.method != Method::scene && request.method != Method::info &&
        request.method != Method::packages && request.method != Method::package_read &&
        request.method != Method::permissions_get && request.method != Method::settings_get &&
        request.method != Method::diagnostics && request.method != Method::companion_subscribe &&
        request.method != Method::companion_bind && request.method != Method::hello &&
        request.method != Method::install_chunk && request.method != Method::install_begin &&
        request.method != Method::session_close) {
        bindingsDirty_ = true;
        events_.post({4, 0, runtime_.generation()});
    }
    Writer result;
    result.n(uint16_t(error), 2);
    if (error == Error::ok)
        result.raw(out.data);
    if (result.data.size() > MaxPayload) {
        error = Error::quota_exceeded;
        result.data.clear();
        result.n(uint16_t(Error::quota_exceeded), 2);
    }
    reply.payload = std::move(result.data);
    if (error != Error::ok)
        ++requestErrors_;
    refreshCompanionDemand();
    return reply;
}
} // namespace flint
