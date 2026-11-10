#pragma once
#include "contracts.h"
#include <algorithm>

namespace flint {
// Additive application API. Packages using it must require this capability.
inline constexpr uint64_t CapAppApi = 128;
inline constexpr size_t MaxAppString = 192;
inline bool appKey(std::string_view key) {
    if (key.empty() || key.size() > 32)
        return false;
    for (char c : key)
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'))
            return false;
    return true;
}
// Strict UTF-8, excluding embedded NUL; bounded by the caller.
inline bool appText(std::string_view text) {
    size_t i = 0;
    while (i < text.size()) {
        const auto c = static_cast<uint8_t>(text[i++]);
        if (!c)
            return false;
        if (c < 128)
            continue;
        unsigned count;
        uint32_t value, minimum;
        if (c >= 0xc2 && c <= 0xdf) {
            count = 1;
            value = c & 31;
            minimum = 0x80;
        } else if (c >= 0xe0 && c <= 0xef) {
            count = 2;
            value = c & 15;
            minimum = 0x800;
        } else if (c >= 0xf0 && c <= 0xf4) {
            count = 3;
            value = c & 7;
            minimum = 0x10000;
        } else
            return false;
        if (count > text.size() - i)
            return false;
        while (count--) {
            const auto b = static_cast<uint8_t>(text[i++]);
            if ((b & 0xc0) != 0x80)
                return false;
            value = (value << 6) | (b & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff))
            return false;
    }
    return true;
}
enum class InputKind : uint8_t { press, release, hold, rotate, focus_lost, focus_gained };
struct InputEvent {
    InputKind kind{};
    int32_t value{};
    uint64_t generation{};
};
inline constexpr std::string_view inputName(InputKind kind) {
    switch (kind) {
    case InputKind::press:
        return "press";
    case InputKind::release:
        return "release";
    case InputKind::hold:
        return "hold";
    case InputKind::rotate:
        return "rotate";
    case InputKind::focus_lost:
        return "focus_lost";
    case InputKind::focus_gained:
        return "focus_gained";
    }
    return {};
}
enum class AppService : uint8_t {
    time,
    monotonic,
    info,
    power,
    locale,
    theme,
    settings,
    storage_get,
    storage_set,
    storage_delete,
    timer,
    image
};
inline constexpr std::string_view servicePermission(AppService service) {
    switch (service) {
    case AppService::time:
        return "time.read";
    case AppService::monotonic:
        return "time.monotonic";
    case AppService::info:
        return "system.info";
    case AppService::power:
        return "system.power.status";
    case AppService::locale:
        return "system.locale.read";
    case AppService::theme:
        return "system.theme.read";
    case AppService::settings:
        return "system.settings.read";
    case AppService::storage_get:
    case AppService::storage_set:
    case AppService::storage_delete:
        return "storage.private";
    case AppService::timer:
        return "time.timer.create";
    case AppService::image:
        return "ui.resources";
    }
    return {};
}
struct AppReply {
    std::array<char, MaxAppString + 1> text{};
    size_t size{};
    int64_t number{};
    bool numeric{};
    Error assign(std::string_view value) {
        if (value.size() > MaxAppString || !appText(value))
            return Error::quota_exceeded;
        std::copy(value.begin(), value.end(), text.begin());
        size = value.size();
        numeric = false;
        return Error::ok;
    }
};
// Serialized on the Core event loop. Callbacks must return without throwing;
// no callback may reenter Runtime or retain these views. No app-chosen principal.
struct AppHost {
    void *context{};
    Error (*call)(void *, AppService, std::string_view, std::string_view, AppReply &){};
    uint64_t (*monotonic)(void *){};
    bool enabled{};
};
struct LuaDiagnostic {
    Error error{Error::ok};
    std::array<char, 16> phase{};
    std::array<char, 161> message{};
    size_t used{}, peak{};
    uint32_t instructions{};
    std::array<std::array<char, 129>, 8> log{};
    size_t logCount{};
};
} // namespace flint
