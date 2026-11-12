#include <cstdlib>
#include <flint/core.h>
#include <new>
namespace flint {
Error Core::postInput(InputEvent event) {
    if (!presentation_) {
        if (edge_ ||
            ((runtime_.component().role == Role::clock ||
                 runtime_.component().type == Component::watchface) &&
                event.kind == InputKind::touch_down && event.x >= 244))
            return edgeInput(event);
        return runtime_.postInput(event);
    }
    if (event.generation != runtime_.generation())
        return Error::stale_handle;
    if (inputName(event.kind).empty() || event.pointer > 15 || event.x < -320 || event.x > 640 ||
        event.y < -320 || event.y > 640)
        return Error::invalid_argument;
    if (event.kind == InputKind::touch_cancel || event.kind == InputKind::focus_lost) {
        presentationPointer_ = -1;
        return Error::ok;
    }
    if (event.kind == InputKind::touch_down && presentationPointer_ == -1) {
        presentationPointer_ = event.pointer;
        presentationX_ = event.x;
        presentationY_ = event.y;
        return Error::ok;
    }
    if (event.kind == InputKind::touch_move && presentationPointer_ == event.pointer) {
        if (std::abs(event.x - presentationX_) + std::abs(event.y - presentationY_) > 8)
            presentationPointer_ = -2;
        return Error::ok;
    }
    if (event.kind != InputKind::touch_up)
        return Error::ok;
    if (presentationPointer_ >= 0 && presentationPointer_ != event.pointer)
        return Error::ok;
    const bool tapped = presentationPointer_ == event.pointer &&
        std::abs(event.x - presentationX_) + std::abs(event.y - presentationY_) <= 8;
    presentationPointer_ = -1;
    if (!tapped || event.x < 16 || event.x > 264 || event.y < 188 || event.y > 238)
        return Error::ok;
    if (!notice_ &&
        (operationPhase_ == OperationPhase::publishing ||
            operationPhase_ == OperationPhase::removing))
        return Error::invalid_state;
    if (notice_ && noticeKey_ == "pairing" && platform_.pairing)
        platform_.pairing("reject");
    if (!notice_ &&
        (operationPhase_ == OperationPhase::receiving ||
            operationPhase_ == OperationPhase::verifying)) {
        transferCancelled_ = true;
        if (install_ && !install_->publishing()) {
            Writer payload;
            payload.n(unsigned(Error::cancelled), 2);
            installReply_ =
                Frame{installRequest_.method, installRequest_.id, true, std::move(payload.data)};
            install_.reset();
        }
    }
    const bool clock = notice_ && noticeKey_ == "app_error";
    presentation_.reset();
    operationUntil_ = UINT64_MAX;
    operationDirty_ = true;
    if (clock)
        activateRole(Role::clock);
    return Error::ok;
}
void Core::applyUiAction() {
    const auto action = std::move(pendingUiAction_), value = std::move(pendingUiValue_);
    pendingUiAction_.clear();
    pendingUiValue_.clear();
    if (pendingUiGeneration_ != runtime_.generation())
        return;
    Error error = Error::invalid_argument;
    if (action == "role") {
        error = activateRole(value == "clock" ? Role::clock
                : value == "launcher"         ? Role::launcher
                                              : Role::settings);
    } else if (action == "launch") {
        const auto slash = value.find('/');
        error = launch(
            std::string_view(value).substr(0, slash), std::string_view(value).substr(slash + 1));
    } else if (action == "system") {
        Writer input;
        Method method = Method::settings_set;
        if (value == "ble_toggle") {
            input.text("ble_enabled");
            input.text(settings_["ble_enabled"] == "0" ? "1" : "0");
        } else if (value == "motion") {
            input.text("reduce_motion");
            input.text(settings_["reduce_motion"] == "1" ? "0" : "1");
        } else if (value == "language") {
            method = Method::role_set;
            input.n(unsigned(Role::locale), 1);
            input.text(settings_["role5"] == "system.locale-ru/ru" ? "system.locale/en"
                                                                   : "system.locale-ru/ru");
        } else {
            const auto offset = strtol(settings_["utc_offset"].c_str(), nullptr, 10);
            input.text("utc_offset");
            input.text(std::to_string(
                std::clamp<long>(offset + (value == "offset_up" ? 15 : -15), -720, 840)));
        }
        // Trusted local input, validated by Runtime and factory Settings ownership before queuing.
        Session local{true, true, 0};
        const auto result = handle({method, 1, false, input.data}, local);
        error =
            result.payload.size() >= 2 ? Error(u16(result.payload.data())) : Error::invalid_state;
        if (error == Error::ok && value == "language") {
            const auto id = runtime_.manifest().id, component = runtime_.component().id;
            error = launch(id, component);
        }
    }
    if (error != Error::ok)
        notice("action_error", "", "error_" + std::to_string(unsigned(error)));
}
std::string Core::uiText(std::string text, int width) {
    bool shortened = false;
    while (!text.empty() &&
        FlintRender::textWidth(0, reinterpret_cast<const uint8_t *>(text.data()), text.size()) >
            width - 16) {
        auto end = text.size() - 1;
        while (end && (uint8_t(text[end]) & 0xc0) == 0x80)
            --end;
        text.resize(end);
        shortened = true;
    }
    if (shortened)
        text += "…";
    return text;
}
bool Core::presentation(std::string_view id, std::string_view component) {
    if (!platform_.system_ui)
        return false;
    edge_.reset();
    if (presentation_ && presentation_->manifest().id == id &&
        presentation_->component().id == component)
        return true;
    presentationPointer_ = -1;
    if (!presentation_ && runtime_.scripted())
        runtime_.postInput({InputKind::touch_cancel, 0, runtime_.generation()});
    Installed package;
    if (packages_.locate(id, package) != Error::ok || !package.factory)
        return false;
    if (platform_.available_memory && platform_.available_memory().internal_bytes < sizeof(Runtime))
        return false;
    if (!presentation_)
        presentation_.reset(new (std::nothrow) Runtime);
    if (!presentation_)
        return false;
    if (presentation_->launch(package, component, platform_.memory, platform_.capabilities) !=
            Error::ok ||
        presentation_->scripted()) {
        presentation_.reset();
        operationUntil_ = UINT64_MAX;
        return false;
    }
    return true;
}
void Core::notice(
    std::string_view key, std::string subject, std::string detail, std::string scope) {
    if (install_ || (presentation_ && !notice_ && operationPhase_ == OperationPhase::receiving))
        return;
    if (!presentation("system.status", key == "pairing" ? "pairing" : "main"))
        return;
    notice_ = true;
    noticeKey_ = key;
    noticeSubject_ = uiText(std::move(subject));
    noticeDetail_ = uiText(std::move(detail));
    noticeScope_ = uiText(std::move(scope));
    const auto at = platform_.monotonic();
    operationUntil_ = at <= UINT64_MAX - 4000 ? at + 4000 : at;
    operationDirty_ = true;
}
void Core::operationUpdate(OperationPhase phase, uint32_t done, uint32_t total, Error error) {
    if (!presentation("system.operations"))
        return;
    notice_ = false;
    const auto at = platform_.monotonic();
    const bool changed = operationPhase_ != phase;
    operationPhase_ = phase;
    operationDone_ = done;
    operationTotal_ = total;
    operationError_ = error;
    const bool receiving = phase == OperationPhase::receiving;
    const bool terminal = phase == OperationPhase::complete || phase == OperationPhase::failed ||
        phase == OperationPhase::interrupted;
    const uint64_t delay = receiving ? 15000 : terminal ? 1500 : 0;
    operationUntil_ = delay && at <= UINT64_MAX - delay ? at + delay : UINT64_MAX;
    if (changed || at < operationPaintAt_ || at - operationPaintAt_ >= 100)
        operationDirty_ = true;
}
bool Core::operationTick() {
    if (platform_.system_ui && platform_.pairing_prompt) {
        const auto prompt = platform_.pairing_prompt();
        if (prompt.code >= 0 && prompt.code <= 999999) {
            if (pairingShown_ != prompt.code || !presentation_ || !notice_ ||
                noticeKey_ != "pairing") {
                char code[7], peer[32];
                std::snprintf(code, sizeof(code), "%06d", int(prompt.code));
                std::snprintf(peer, sizeof(peer), "%02X:%02X:%02X:%02X:%02X:%02X",
                    prompt.address[5], prompt.address[4], prompt.address[3], prompt.address[2],
                    prompt.address[1], prompt.address[0]);
                notice("pairing", code, peer);
                if (notice_ && noticeKey_ == "pairing")
                    pairingShown_ = prompt.code;
            }
            if (notice_ && noticeKey_ == "pairing")
                operationUntil_ = UINT64_MAX;
        } else if (pairingShown_ >= 0) {
            pairingShown_ = -1;
            if (notice_ && noticeKey_ == "pairing")
                operationUntil_ = platform_.monotonic();
        }
    }

    if (presentation_ && platform_.monotonic() >= operationUntil_) {
        if (!notice_ && operationPhase_ == OperationPhase::receiving)
            operationUpdate(
                OperationPhase::interrupted, operationDone_, operationTotal_, Error::cancelled);
        else {
            presentation_.reset();
            operationUntil_ = UINT64_MAX;
            operationDirty_ = true;
        }
    }
    const bool dirty = operationDirty_;
    operationDirty_ = false;
    return dirty;
}
Error Core::operationRender(FlintRender::Scene &scene) {
    Values values;
    for (const auto &[key, value] : locale_)
        values["locale:" + key] = value;
    const auto local = [&](const char *key, const char *fallback) -> std::string {
        const auto found = locale_.find(key);
        return found == locale_.end() ? fallback : found->second;
    };
    if (notice_) {
        values["status.title"] = local(noticeKey_.c_str(), "Status");
        values["status.subject"] = noticeSubject_;
        values["status.detail"] = local(noticeDetail_.c_str(), noticeDetail_.c_str());
        values["status.scope"] =
            noticeKey_ == "pairing" ? local("compare_flare", "Compare in Flare") : noticeScope_;
        values["status.button"] = noticeKey_ == "pairing" ? local("cancel", "Cancel")
            : noticeKey_ == "app_error"                   ? local("to_clock", "Clock")
                                                          : local("done", "Done");
    } else {
        values["operation.title"] =
            operationRemoving_ ? local("remove", "Removing") : local("install", "Installing");
        const char *key = operationPhase_ == OperationPhase::receiving ? "receiving"
            : operationPhase_ == OperationPhase::verifying             ? "verifying"
            : operationPhase_ == OperationPhase::publishing            ? "publishing"
            : operationPhase_ == OperationPhase::removing              ? "removing"
            : operationPhase_ == OperationPhase::complete              ? "complete"
            : operationPhase_ == OperationPhase::failed                ? "failed"
                                                                       : "interrupted";
        values["operation.phase"] = local(key, key);
        const bool measured = operationPhase_ == OperationPhase::receiving ||
            operationPhase_ == OperationPhase::verifying;
        values["operation.progress"] = measured && operationTotal_
            ? std::to_string(
                  std::min(uint64_t(operationDone_) * 100 / operationTotal_, uint64_t(100)))
            : operationPhase_ == OperationPhase::complete ? "100"
                                                          : "";
        values["operation.unit"] = values["operation.progress"].empty() ? "" : "%";
        values["operation.button"] = operationPhase_ == OperationPhase::receiving ||
                operationPhase_ == OperationPhase::verifying
            ? local("cancel", "Cancel")
            : operationPhase_ == OperationPhase::removing   ? local("wait", "Please wait")
            : operationPhase_ == OperationPhase::publishing ? local("publishing", "Saving")
                                                            : local("done", "Done");
        values["operation.detail"] = measured ? std::to_string(operationDone_ / 1024) + " / " +
                std::to_string((operationTotal_ + 1023) / 1024) + " KiB"
            : operationError_ != Error::ok
            ? local(("error_" + std::to_string(unsigned(operationError_))).c_str(), "Error")
            : "";
    }
    auto error = presentation_->render(values, scene);
    if (error == Error::ok && !notice_ &&
        (operationPhase_ == OperationPhase::publishing ||
            operationPhase_ == OperationPhase::removing)) {
        for (unsigned i = 0; i < scene.count; ++i) {
            auto &op = scene.ops[i];
            if (op.kind == FlintRender::Rounded && op.x == 12 && op.y == 192 && op.w == 256 &&
                op.h == 44)
                op.color = 0xfffb; // The protected presentation's disabled action surface.
            if (op.kind == FlintRender::Text && op.y == 221)
                op.color = 0xfff9;
        }
    }
    // System presentation deliberately uses built-in fonts/primitives only, so paint can
    // retain the foreground resource context without a second cache or image decoder.
    if (error == Error::ok)
        for (unsigned i = 0; i < scene.count; ++i)
            if (scene.ops[i].font >= 4 || scene.ops[i].kind == FlintRender::ResourceImage)
                return Error::not_supported;
    operationPaintAt_ = platform_.monotonic();
    return error;
}
} // namespace flint
