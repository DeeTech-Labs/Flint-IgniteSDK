#include <charconv>
#include <flint/registry.h>
#include <flint/runtime.h>
namespace flint {
static size_t manifestHeapEstimate(const Manifest &m) {
    size_t bytes = m.id.capacity() + m.name.capacity() + m.publisher.capacity() + 3;
    bytes += m.permissions.capacity() * sizeof(Declaration) +
        m.components.capacity() * sizeof(ComponentInfo);
    for (const auto &d : m.permissions)
        bytes += d.permission.capacity() + d.scope.capacity() + 2;
    for (const auto &c : m.components) {
        bytes += c.id.capacity() + 1 + c.permissions.capacity() * sizeof(Declaration);
        for (const auto &d : c.permissions)
            bytes += d.permission.capacity() + d.scope.capacity() + 2;
    }
    return bytes;
}
Error Runtime::launch(
    const Installed &installed, std::string_view id, MemoryBudget available, uint64_t caps) {
    if (availableMemory_ && !fits(installed.manifest.budget, availableMemory_()))
        return Error::resource_exhausted;
    File candidate;
    if (!candidate.open(installed.path))
        return Error::io_error;
    Package p;
    auto error = p.open(candidate.source());
    if (error != Error::ok)
        return error;
    Manifest m;
    error = validateApplication(p, m, available, caps);
    if (error != Error::ok)
        return error;
    return launchValidated(candidate, p, std::move(m), id, available, caps);
}
Error Runtime::launchPrepared(
    PreparedInstall &prepared, std::string_view id, MemoryBudget available, uint64_t caps) {
    if (!prepared.ready() || !prepared.committed())
        return Error::invalid_state;
    if (availableMemory_ && !fits(prepared.manifest().budget, availableMemory_()))
        return Error::resource_exhausted;
    return launchValidated(
        prepared.file(), prepared.package(), prepared.releaseManifest(), id, available, caps);
}
Error Runtime::launchValidated(File &candidate, Package &p, Manifest m, std::string_view id,
    MemoryBudget available, uint64_t caps) {
    if (!fits(m.budget, available) || (availableMemory_ && !fits(m.budget, availableMemory_())))
        return Error::resource_exhausted;
    if (!candidate.unchanged())
        return Error::stale_handle;
    if ((m.capabilities & caps) != m.capabilities)
        return Error::not_supported;
    auto error = Error::ok;
    ComponentInfo component;
    bool found = false;
    for (auto &c : m.components)
        if (c.id == id) {
            component = c;
            found = true;
            break;
        }
    if (!found)
        return Error::source_unavailable;
    if (component.type != Component::app && component.type != Component::watchface &&
        component.type != Component::widget && component.type != Component::complication)
        return Error::not_supported;
    bool canRender = false;
    for (auto &d : component.permissions)
        if (d.permission == "ui.render")
            canRender = true;
    if (!canRender)
        return Error::permission_denied;
    Entry e;
    bool ui = false;
    for (uint32_t i = 0; i < p.count(); ++i) {
        p.entry(i, e);
        if (e.id == component.section) {
            ui = e.type == Section::ui;
            break;
        }
    }
    if (!ui)
        return Error::not_supported;
    Bytes bytes(e.size);
    FlintRender::Scene scene;
    if (p.read(e, 0, bytes) != Error::ok || !FlintRender::decode(bytes.data(), bytes.size(), scene))
        return Error::invalid_package;
    bool ownResources = false;
    for (const auto &declaration : component.permissions) {
        ownResources = ownResources || declaration.permission == "ui.resources";
    }
    for (int index = 0; index < scene.count; ++index) {
        const auto &operation = scene.ops[index];
        if (operation.kind == FlintRender::CatalogIcon) {
            const bool declared = std::any_of(component.permissions.begin(),
                component.permissions.end(),
                [](const auto &d) { return d.permission == "packages.list" && d.scope.empty(); });
            if (!declared || !(caps & CapInteractiveUi) || !(m.capabilities & CapInteractiveUi))
                return Error::permission_denied;
        }
        if ((operation.kind == FlintRender::Text && operation.font >= 4) ||
            operation.kind == FlintRender::ResourceImage) {
            if (!ownResources) {
                return Error::permission_denied;
            }
            const uint32_t resource =
                operation.kind == FlintRender::ResourceImage ? operation.extra : operation.font;
            bool foundResource = false;
            for (uint32_t index = 0; index < p.count(); ++index) {
                Entry entry;
                if (p.entry(index, entry) == Error::ok && entry.id == resource &&
                    entry.type == Section::resource) {
                    foundResource = true;
                    break;
                }
            }
            if (!foundResource) {
                return Error::invalid_package;
            }
        }
    }
    std::array<uint16_t, FlintRender::MaxData> bindingEnds{};
    for (unsigned i = 0; i < scene.count; ++i) {
        const auto &op = scene.ops[i];
        if (op.kind != FlintRender::Text)
            continue;
        const std::string_view text(
            reinterpret_cast<const char *>(scene.data + op.offset), op.length);
        size_t start = 0;
        unsigned count = 0;
        while ((start = text.find("${", start)) != std::string_view::npos) {
            if (++count > 32)
                return Error::execution_limit;
            const auto end = text.find('}', start);
            if (end == std::string_view::npos)
                return Error::invalid_package;
            bindingEnds[op.offset + start] = op.offset + end + 1;
            start = end + 1;
        }
    }
    auto nativeManifest = manifestHeapEstimate(m) + component.id.capacity() + 1 +
        component.permissions.capacity() * sizeof(Declaration);
    for (const auto &d : component.permissions)
        nativeManifest += d.permission.capacity() + d.scope.capacity() + 2;
    const auto sceneBytes = bytes.capacity();
    Bytes{}.swap(bytes);
    stop();
    nativeHeap_ = nativeManifest;
    nativePeak_ = nativeManifest + sceneBytes;
    bindingEnds_ = bindingEnds;
    file_ = std::move(candidate);
    package_ = std::move(p);
    package_.relocate(file_.source());
    p.close();
    manifest_ = std::move(m);
    component_ = std::move(component);
    template_ = scene;
    capabilities_ = caps;
    focused_ = true;
    resources_.bind(&package_);
    for (uint32_t i = 0; i < package_.count(); ++i) {
        Entry code;
        package_.entry(i, code);
        if (code.id != component_.logicSection)
            continue;
        const auto nativeLoad = nativeManifest + LuaApp::nativeBytes() + code.size;
        // Preserve the existing Lua heap ceiling while charging native state and source peak.
        if (nativeLoad + 16 * 1024 > component_.budget.internal_bytes) {
            stop();
            return Error::resource_exhausted;
        }
        const auto luaLimit = std::min<size_t>(component_.budget.internal_bytes - 16 * 1024,
            component_.budget.internal_bytes - nativeLoad);
        nativeHeap_ = nativeManifest + LuaApp::nativeBytes();
        nativePeak_ = std::max<size_t>(nativePeak_, nativeLoad);
        Bytes source(code.size);
        error = package_.read(code, 0, source);
        loading_ = true;
        AppHost bridge{this,
            [](void *context, AppService service, std::string_view key, std::string_view value,
                AppReply &reply) {
                return static_cast<Runtime *>(context)->appCall(service, key, value, reply);
            },
            [](void *context) {
                const auto &host = static_cast<Runtime *>(context)->host_;
                return host.monotonic ? host.monotonic(host.context) : uint64_t(0);
            },
            bool(component_.capabilities & CapAppApi), nullptr,
            [](void *context, const sensors::Request &request, sensors::Reply &reply) {
                return static_cast<Runtime *>(context)->sensorCall(request, reply);
            }};
        if (!(component_.capabilities & sensors::Capability))
            bridge.sensor = nullptr;
        bridge.microseconds = host_.microseconds ? [](void *context) {
            const auto &host = static_cast<Runtime *>(context)->host_;
            return host.microseconds(host.context);
        } : static_cast<uint64_t (*)(void *)>(nullptr);
        if (component_.capabilities & CapComponentIpc)
            bridge.ipc = [](void *context, const IpcRequest &request, IpcReply &reply) {
                return static_cast<Runtime *>(context)->ipcCall(request, reply);
            };
        if (component_.capabilities & CapBackground)
            bridge.services = [](void *context, const ServiceRequest &request, ServiceInfo &reply) {
                return static_cast<Runtime *>(context)->serviceCall(request, reply);
            };
        for (const auto &declaration : component_.permissions) {
            if (declaration.permission == "audio.play")
                bridge.outputs |= 1;
            if (declaration.permission == "ui.brightness")
                bridge.outputs |= 2;
        }
        if (error == Error::ok)
            error = lua_.load({reinterpret_cast<const char *>(source.data()), source.size()},
                uint32_t(luaLimit), component_.budget.instructions, template_, bridge);
        loading_ = false;
        if (error != Error::ok) {
            stop();
            return error;
        }
    }
    ++generation_;
    ++consumerGeneration_;
    lifecycle_.dispatch(Lifecycle::create);
    return lifecycle_.dispatch(Lifecycle::activate);
}
Error Runtime::update(uint32_t elapsedMs, bool pressed) {
    tickChanged_ = false;
    if (lifecycle_.state != State::active)
        return Error::invalid_state;
    if (!lua_.active())
        return Error::not_supported;
    if (pressed) {
        if (!focused_)
            return Error::permission_denied;
        bool allowed = false;
        for (const auto &d : component_.permissions)
            allowed |= d.permission == "input.focused";
        if (!allowed)
            return Error::permission_denied;
    }
    auto error = lua_.update(elapsedMs, pressed);
    if (error != Error::ok)
        stop();
    return error;
}
Error Runtime::stop() {
    if (host_.cancelInstance)
        host_.cancelInstance(host_.context);
    lua_.stop();
    nativeHeap_ = nativePeak_ = 0;
    inputHead_ = inputSize_ = 0;
    tickChanged_ = pendingVisual_ = false;
    loading_ = false;
    if (lifecycle_.state == State::empty) {
        resources_.bind(nullptr);
        package_.close();
        file_.close();
        manifest_ = {};
        component_ = {};
        return Error::ok;
    }
    if (lifecycle_.state == State::active)
        lifecycle_.dispatch(Lifecycle::deactivate);
    lifecycle_.dispatch(Lifecycle::destroy);
    resources_.bind(nullptr);
    package_.close();
    file_.close();
    manifest_ = {};
    component_ = {};
    ++generation_;
    ++consumerGeneration_;
    return Error::ok;
}
Error Runtime::suspend() {
    if (lifecycle_.state != State::active)
        return Error::invalid_state;
    if (host_.cancelInstance)
        host_.cancelInstance(host_.context);
    if (lua_.active() && (component_.capabilities & CapAppApi)) {
        const auto error = lua_.event("suspend", 0);
        pendingVisual_ |= lua_.visualChanged();
        if (error != Error::ok) {
            stop();
            return error;
        }
    }
    inputHead_ = inputSize_ = 0;
    if (host_.cancelInstance)
        host_.cancelInstance(host_.context);
    lifecycle_.dispatch(Lifecycle::deactivate);
    ++consumerGeneration_;
    return lifecycle_.dispatch(Lifecycle::suspend);
}
Error Runtime::resume() {
    auto error = lifecycle_.dispatch(Lifecycle::resume);
    if (error != Error::ok)
        return error;
    lifecycle_.dispatch(Lifecycle::activate);
    ++consumerGeneration_;
    lua_.resumeClock(host_.monotonic ? host_.monotonic(host_.context) : 0);
    if (lua_.active() && (component_.capabilities & CapAppApi)) {
        error = lua_.event("resume", 0);
        pendingVisual_ |= lua_.visualChanged();
        if (error != Error::ok)
            stop();
    }
    return error;
}
Error Runtime::render(const Values &values, FlintRender::Scene &output) {
    if (lifecycle_.state != State::active)
        return Error::invalid_state;
    FlintRender::Scene scene;
    scene.width = template_.width;
    scene.height = template_.height;
    uint32_t instructions = 0;
    for (int i = 0; i < template_.count; ++i) {
        if (++instructions > component_.budget.instructions)
            return Error::execution_limit;
        if (!lua_.visible(unsigned(i)))
            continue;
        auto op = template_.ops[i];
        const auto *data = template_.data + op.offset;
        if (op.kind != FlintRender::Text) {
            if (!scene.add(op, data, op.length))
                return Error::quota_exceeded;
            continue;
        }
        std::array<char, 192> text{};
        size_t cursor = 0, used = 0, projected = op.length;
        while (cursor < op.length) {
            const auto endOffset = bindingEnds_[op.offset + cursor];
            if (!endOffset) {
                if (used == text.size())
                    return Error::quota_exceeded;
                text[used++] = char(data[cursor++]);
                continue;
            }
            if (++instructions > component_.budget.instructions)
                return Error::execution_limit;
            const size_t end = endOffset - op.offset;
            const std::string_view key(
                reinterpret_cast<const char *>(data + cursor + 2), end - cursor - 3);
            std::string_view value;
            std::array<uint8_t, 192> resource{};
            std::array<char, 12> number{};
            if (key.starts_with("resource:")) {
                const bool declared =
                    std::any_of(component_.permissions.begin(), component_.permissions.end(),
                        [](const auto &d) { return d.permission == "ui.resources"; });
                if (!declared)
                    return Error::permission_denied;
                const auto id = key.substr(9);
                std::array<char, 193> identifier{};
                std::copy(id.begin(), id.end(), identifier.begin());
                char *tail{};
                const auto section = strtoul(identifier.data(), &tail, 10);
                if (!id.empty() && tail && !*tail && section <= UINT32_MAX) {
                    Entry entry;
                    if (resources_.find(section, entry) == Error::ok &&
                        entry.type == Section::resource && entry.size <= resource.size() &&
                        resources_.read(resources_.handle(section), 0,
                            std::span(resource).first(entry.size)) == Error::ok)
                        value = {reinterpret_cast<const char *>(resource.data()), entry.size};
                }
            } else {
                bool published = false;
                if (key.starts_with("app.")) {
                    for (const auto &v : lua_.values()) {
                        if (key.substr(4) == v.key) {
                            if (v.string)
                                value = {v.text.data(), v.length};
                            else {
                                const auto result = std::to_chars(
                                    number.data(), number.data() + number.size(), v.value);
                                value = {number.data(), size_t(result.ptr - number.data())};
                            }
                            published = true;
                            break;
                        }
                    }
                }
                if (!published && key.starts_with("app."))
                    value = "—";
                else if (!published) {
                    const auto it = values.find(key);
                    value = it == values.end() ? std::string_view("—") : it->second;
                }
            }
            projected -= end - cursor;
            if (value.find('\0') != std::string_view::npos ||
                value.size() > text.size() - projected)
                return Error::quota_exceeded;
            projected += value.size();
            if (value.size() > text.size() - used)
                return Error::quota_exceeded;
            std::copy(value.begin(), value.end(), text.begin() + used);
            used += value.size();
            cursor = end;
        }
        if (op.font >= 4)
            resources_.textBounds(op, reinterpret_cast<const uint8_t *>(text.data()), used);
        if (!scene.add(op, text.data(), used))
            return Error::quota_exceeded;
    }
    output = scene;
    return Error::ok;
}
Error Runtime::appCall(
    AppService service, std::string_view key, std::string_view value, AppReply &reply) {
    if (loading_ && (service == AppService::beep || service == AppService::brightness))
        return Error::invalid_state;
    if (!loading_ && state() != State::active)
        return Error::invalid_state;
    if (!(component_.capabilities & CapAppApi) || !(capabilities_ & CapAppApi))
        return Error::not_supported;
    const auto required = servicePermission(service);
    const auto *spec = permission(required);
    if (!spec)
        return Error::not_supported;
    bool declared = false;
    for (const auto &d : component_.permissions)
        if (d.permission == required && d.scope.empty())
            declared = true;
    if (!declared)
        return Error::permission_denied;
    if ((capabilities_ & spec->capability) != spec->capability)
        return Error::not_supported;
    if (spec->access != Access::automatic) {
        if (loading_ || !host_.authorize)
            return Error::permission_denied;
        const auto error = host_.authorize(host_.context, required);
        if (error != Error::ok)
            return error;
    }
    if (service == AppService::timer)
        return Error::ok;
    if (service == AppService::packages &&
        (!(capabilities_ & CapInteractiveUi) || !(component_.capabilities & CapInteractiveUi)))
        return Error::not_supported;
    if (service == AppService::navigate || service == AppService::system_action) {
        if (!(capabilities_ & CapInteractiveUi) || !(component_.capabilities & CapInteractiveUi))
            return Error::not_supported;
        if (!inputDispatch_ || loading_ || !focused_)
            return Error::user_interaction_required;
    }
    if (service == AppService::image) {
        uint32_t id{};
        const auto parsed = std::from_chars(key.data(), key.data() + key.size(), id);
        if (parsed.ec != std::errc{} || parsed.ptr != key.data() + key.size() || !id ||
            id > UINT16_MAX)
            return Error::invalid_argument;
        Entry entry;
        auto error = resources_.find(id, entry);
        if (error != Error::ok || entry.type != Section::resource || entry.size < 8)
            return Error::invalid_argument;
        Package::View section;
        if (package_.view(id, section) != Error::ok)
            return Error::invalid_argument;
        auto &decoder = resources_.decoder();
        error = decoder.open(imageSource(section));
        if (error == Error::ok)
            error = decoder.validate();
        if (error != Error::ok) {
            decoder.close();
            return error;
        }
        const auto &info = decoder.info();
        reply.number = int64_t((uint32_t(info.width) << 16) | info.height);
        decoder.close();
        reply.numeric = true;
        return Error::ok;
    }
    if (!host_.call)
        return Error::not_supported;
    const auto error = host_.call(host_.context, service, key, value, reply);
    if (error == Error::ok &&
        (reply.size > MaxAppString ||
            (!reply.numeric && !appText({reply.text.data(), reply.size}))))
        return Error::invalid_argument;
    return error;
}
Error Runtime::sensorCall(const sensors::Request &request, sensors::Reply &reply) {
    // Loading includes package validation and the first update callback. No hardware intent
    // or sensor cache access is permitted until the installed Runtime is active.
    if (loading_ || state() != State::active)
        return Error::invalid_state;
    if (!(component_.capabilities & sensors::Capability) || !(capabilities_ & sensors::Capability))
        return Error::not_supported;
    if (!host_.sensor)
        return Error::not_supported;
    return host_.sensor(host_.context, request, reply);
}
Error Runtime::ipcCall(const IpcRequest &request, IpcReply &reply) {
    if (loading_ || state() != State::active)
        return Error::invalid_state;
    if (!(component_.capabilities & CapComponentIpc) || !host_.ipc)
        return Error::not_supported;
    return host_.ipc(host_.context, request, reply);
}
Error Runtime::serviceCall(const ServiceRequest &request, ServiceInfo &reply) {
    if (loading_ || state() != State::active)
        return Error::invalid_state;
    if (!host_.services)
        return Error::not_supported;
    if (request.operation != ServiceOperation::list && !inputDispatch_)
        return Error::user_interaction_required;
    return host_.services(host_.context, request, reply);
}
Error Runtime::postInput(InputEvent event) {
    if (event.generation != generation_)
        return Error::stale_handle;
    if (state() != State::active || !scripted())
        return Error::invalid_state;
    if (inputName(event.kind).empty())
        return Error::invalid_argument;
    if (event.kind >= InputKind::touch_down && event.kind <= InputKind::touch_cancel) {
        if (!(capabilities_ & CapTouchEvents) || !(component_.capabilities & CapTouchEvents))
            return Error::not_supported;
        if (event.x < -320 || event.x > 640 || event.y < -320 || event.y > 640 ||
            event.pointer > 15 || event.at > uint64_t(INT64_MAX))
            return Error::invalid_argument;
        if (event.kind == InputKind::touch_move && inputSize_) {
            auto &last = inputs_[(inputHead_ + inputSize_ - 1) % inputs_.size()];
            if (last.kind == InputKind::touch_move && last.pointer == event.pointer) {
                last = event;
                return Error::ok;
            }
        }
    }
    bool declared = false;
    for (const auto &d : component_.permissions)
        if (d.permission == "input.focused" && d.scope.empty())
            declared = true;
    if (!declared)
        return Error::permission_denied;
    if (inputSize_ == inputs_.size())
        return Error::quota_exceeded;
    inputs_[(inputHead_ + inputSize_) % inputs_.size()] = event;
    ++inputSize_;
    return Error::ok;
}
Error Runtime::tick(uint64_t now) {
    tickChanged_ = false;
    if (state() != State::active || !scripted())
        return Error::invalid_state;
    const auto started = lua_.beginTick(now);
    if (started != Error::ok)
        return started;
    bool changed = pendingVisual_;
    pendingVisual_ = false;
    while (inputSize_) {
        const auto input = inputs_[inputHead_];
        inputHead_ = (inputHead_ + 1) % inputs_.size();
        --inputSize_;
        if (input.generation != generation_)
            continue;
        if (input.kind == InputKind::focus_lost)
            focused_ = false;
        else if (input.kind == InputKind::focus_gained)
            focused_ = true;
        else if (!focused_)
            continue;
        Error error;
        if (component_.capabilities & CapAppApi) {
            inputDispatch_ = input.kind == InputKind::touch_up || input.kind == InputKind::release;
            error = lua_.event(inputName(input.kind), input.value, &input);
            inputDispatch_ = false;
        } else if (input.kind == InputKind::press)
            error = lua_.update(0, true);
        else
            continue;
        if (error != Error::ok) {
            stop();
            return error;
        }
        changed |= lua_.visualChanged();
    }
    // Input, including touch_cancel, precedes sensor callbacks. Pull one coalesced event
    // at a time so unsubscribe in a callback invalidates later deliveries in this tick.
    for (size_t i = 0; host_.sensorEvent && i < sensors::MaxSubscriptions + sensors::MaxRefresh;
        ++i) {
        sensors::Event event;
        if (!host_.sensorEvent(host_.context, event))
            break;
        const auto sensorError = lua_.sensorEvent(event);
        if (sensorError != Error::ok) {
            stop();
            return sensorError;
        }
        changed |= lua_.visualChanged();
    }
    for (unsigned i = 0; host_.ipcEvent && i < 8; ++i) {
        IpcEvent event;
        if (!host_.ipcEvent(host_.context, event))
            break;
        const auto error = lua_.ipcEvent(event);
        if (error != Error::ok) {
            stop();
            return error;
        }
        changed |= lua_.visualChanged();
    }
    const auto error = lua_.tick(now);
    if (error != Error::ok) {
        stop();
        return error;
    }
    // A tick can run several callbacks; retain all their invalidations.
    lua_.endTick();
    tickChanged_ = changed || lua_.visualChanged();
    return Error::ok;
}
Error loadValues(const Installed &installed, const ComponentInfo &component, Values &values) {
    File file;
    if (!file.open(installed.path))
        return Error::io_error;
    Package p;
    auto error = p.open(file.source());
    if (error != Error::ok)
        return error;
    return loadValues(p, component, values);
}
Error loadValues(Package &p, const ComponentInfo &component, Values &values) {
    Package::View section;
    if (p.view(component.section, section) != Error::ok || section.info().size > 4096)
        return Error::invalid_package;
    const auto e = section.info();
    Bytes bytes(e.size);
    if (p.read(e, 0, bytes) != Error::ok)
        return Error::io_error;
    Reader r{bytes};
    auto count = r.n(2);
    if (count > 64)
        return Error::invalid_package;
    Values result;
    for (unsigned i = 0; i < count; ++i) {
        auto key = r.text(64), value = r.text(192);
        if (key.empty() || !result.emplace(key, value).second)
            return Error::invalid_package;
    }
    if (!r.done())
        return Error::invalid_package;
    values = std::move(result);
    return Error::ok;
}
} // namespace flint
