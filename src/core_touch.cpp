#include <flint/core.h>
#include <new>
namespace flint {
Error Core::edgeInput(InputEvent event) {
    if (event.generation != runtime_.generation())
        return Error::stale_handle;
    if (!(platform_.capabilities & CapTouchEvents))
        return Error::not_supported;
    if (event.pointer > 15 || event.x < -320 || event.x > 640 || event.y < -320 || event.y > 640 ||
        inputName(event.kind).empty())
        return Error::invalid_argument;
    const auto current = platform_.monotonic();
    if (event.at > current)
        return Error::invalid_argument;
    const auto at = event.at ? event.at : current;
    if (!edge_) {
        if (event.kind != InputKind::touch_down || event.x < 244 || event.x >= 280 || event.y < 0 ||
            event.y >= 240)
            return Error::invalid_argument;
        edge_.reset(new (std::nothrow) EdgeNavigation);
        if (!edge_)
            return Error::resource_exhausted;
        edge_->generation = runtime_.generation();
        Installed launcher;
        if (packages_.locate("system.launcher", launcher) == Error::ok && launcher.factory) {
            edge_->preview.availableMemory(platform_.available_memory);
            edge_->preview.host({this,
                [](void *context, AppService service, std::string_view key, std::string_view value,
                    AppReply &reply) {
                    if (service != AppService::packages && service != AppService::locale &&
                        service != AppService::theme && service != AppService::settings &&
                        service != AppService::timer)
                        return Error::not_supported;
                    return static_cast<Core *>(context)->appCall(service, key, value, reply);
                },
                [](void *context) { return static_cast<Core *>(context)->platform_.monotonic(); },
                true});
            edge_->ready = edge_->preview.launch(launcher, "main", platform_.memory,
                               platform_.capabilities) == Error::ok;
        }
    }
    auto &edge = *edge_;
    if (event.kind == InputKind::touch_cancel || event.kind == InputKind::focus_lost) {
        edge.pointer = -1;
        edge.settling = true;
        edge.from = edge.offset;
        edge.targetX = 280;
        edge.commit = false;
        edge.started = at;
        edge.integratedAt = at;
        edge.position = int64_t(edge.offset) * 1000;
        edge.springVelocity = int64_t(edge.velocity) * 1000;
        edge.nextFrame = at;
        edge.dirty = true;
        return Error::ok;
    }
    if (event.kind == InputKind::touch_down) {
        if (edge.pointer >= 0)
            return Error::ok;
        edge.pointer = event.pointer;
        edge.startX = event.x;
        edge.lastX = event.x;
        edge.origin = edge.offset;
        edge.at = edge.sampleAt = at;
        edge.velocity = 0;
        edge.settling = false;
        edge.dirty = true;
    } else if (event.kind == InputKind::touch_move && edge.pointer == event.pointer) {
        edge.offset = std::clamp(edge.origin + int(event.x) - edge.startX, 0, 280);
        if (at > edge.at && at - edge.at <= 1000) {
            edge.velocity = std::clamp(
                int((int64_t(event.x) - edge.lastX) * 1000 / int64_t(at - edge.at)), -1800, 1800);
            if (event.x != edge.lastX)
                edge.sampleAt = at;
        }
        edge.lastX = event.x;
        edge.at = at;
        edge.dirty = true;
    } else if (event.kind == InputKind::touch_up && edge.pointer == event.pointer) {
        edge.offset = std::clamp(edge.origin + int(event.x) - edge.startX, 0, 280);
        const bool fresh = at >= edge.sampleAt && at - edge.sampleAt < 80;
        edge.commit = !(fresh && edge.velocity > 250) &&
            (edge.offset < 232 || (fresh && edge.velocity < -250));
        edge.pointer = -1;
        edge.settling = true;
        edge.from = edge.offset;
        edge.targetX = edge.commit ? 0 : 280;
        edge.started = at;
        edge.integratedAt = at;
        edge.position = int64_t(edge.offset) * 1000;
        edge.springVelocity = int64_t(edge.velocity) * 1000;
        edge.nextFrame = at;
        edge.dirty = true;
    }
    return Error::ok;
}
bool Core::edgeTick() {
    if (!edge_)
        return false;
    if (runtime_.generation() != edge_->generation) {
        edge_.reset();
        return true;
    }
    auto &edge = *edge_;
    const auto at = platform_.monotonic();
    if (edge.settling && at >= edge.nextFrame) {
        const auto motion = settings_.find("reduce_motion");
        const bool reduced = motion != settings_.end() && motion->second == "1";
        const auto elapsed = at >= edge.started ? at - edge.started : 0;
        const auto dt =
            at >= edge.integratedAt ? std::min<uint64_t>(at - edge.integratedAt, 36) : 0;
        edge.integratedAt = at;
        // Critical spring, response 0.3s: k=(2*pi/0.3)^2, c=2*sqrt(k), scaled by 1000.
        for (unsigned step = 0; step < 4; ++step) {
            const auto acceleration = (-438649 * (edge.position - int64_t(edge.targetX) * 1000) -
                                          41888 * edge.springVelocity) /
                1000;
            edge.springVelocity += acceleration * int64_t(dt) / 4000;
            edge.position += edge.springVelocity * int64_t(dt) / 4000;
        }
        if (edge.position < 0 || edge.position > 280000) {
            edge.position = std::clamp<int64_t>(edge.position, 0, 280000);
            edge.springVelocity = 0;
        }
        edge.offset = int(edge.position / 1000);
        edge.dirty = true;
        edge.nextFrame = at <= UINT64_MAX - 33 ? at + 33 : UINT64_MAX;
        const bool settled = std::abs(edge.position - int64_t(edge.targetX) * 1000) < 1000;
        if (reduced || settled || elapsed >= 1000) {
            const bool commit = edge.commit;
            edge_.reset();
            if (commit) {
                const auto error = activateRole(Role::launcher);
                if (error != Error::ok)
                    notice("action_error", "", "error_" + std::to_string(unsigned(error)));
            }
            return true;
        }
    }
    const bool dirty = edge.dirty;
    edge.dirty = false;
    return dirty;
}
Error Core::edgeRender(FlintRender::Scene &scene) {
    auto &edge = *edge_;
    auto error = runtime_.render(bindings(), edge.source);
    if (error != Error::ok)
        return error;
    if (!edge.ready || edge.preview.render(bindings(), edge.target) != Error::ok) {
        edge.target = {};
        FlintRender::rect(edge.target, 0, 0, 280, 240, 0xfffd);
        const auto title = locale_.find("launcher");
        FlintRender::text(edge.target, 140, 40,
            title == locale_.end() ? "Apps" : title->second.c_str(), 0xfffc, FlintRender::Body,
            true);
    }
    scene = {};
    scene.resourceRevision = std::max(edge.source.resourceRevision, edge.target.resourceRevision);
    bool sourceFits = edge.source.count + edge.target.count <= FlintRender::MaxOps &&
        edge.source.used + edge.target.used <= FlintRender::MaxData;
    for (unsigned i = 1; i < edge.source.count; ++i) {
        const auto x = int(edge.source.ops[i].x) + edge.offset - 280;
        sourceFits &= x >= -320 && x <= 640;
    }
    for (unsigned i = 0; i < edge.source.count; ++i) {
        if (i && !sourceFits)
            continue;
        auto op = edge.source.ops[i];
        if (i)
            op.x += edge.offset - 280;
        if (!scene.add(op, edge.source.data + op.offset, op.length))
            return Error::quota_exceeded;
    }
    for (unsigned i = 0; i < edge.target.count; ++i) {
        auto op = edge.target.ops[i];
        op.x += edge.offset;
        if (!scene.add(op, edge.target.data + op.offset, op.length))
            return Error::quota_exceeded;
    }
    return Error::ok;
}
} // namespace flint
