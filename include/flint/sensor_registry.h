#pragma once
// Generated from spec/sensors.json; do not edit.
#include <flint/contracts.h>
namespace flint::sensors {
enum class Channel : uint8_t {temperature = 0, pressure = 1, humidity = 2, ambient_light = 3, acceleration = 4, angular_velocity = 5};
constexpr std::string_view name(Channel c) {
    constexpr std::array<std::string_view, 6> values{"temperature", "pressure", "humidity", "ambient_light", "acceleration", "angular_velocity"};
    return unsigned(c) < values.size() ? values[unsigned(c)] : std::string_view{};
}
constexpr std::string_view unit(Channel c) {
    constexpr std::array<std::string_view, 6> values{"celsius", "pascal", "percent", "lux", "m/s2", "rad/s"};
    return unsigned(c) < values.size() ? values[unsigned(c)] : std::string_view{};
}
constexpr std::string_view permission(Channel c) {
    constexpr std::array<std::string_view, 6> values{"sensors.temperature", "sensors.pressure", "sensors.humidity", "sensors.ambient_light", "sensors.motion", "sensors.motion"};
    return unsigned(c) < values.size() ? values[unsigned(c)] : std::string_view{};
}
} // namespace flint::sensors
