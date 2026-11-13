#include <charconv>
#include <cmath>
#include <flint/core.h>

namespace flint {
static unsigned settingNumber(const Values &values, std::string_view key, unsigned fallback) {
    const auto found = values.find(key);
    if (found == values.end())
        return fallback;
    unsigned value{};
    const auto parsed =
        std::from_chars(found->second.data(), found->second.data() + found->second.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == found->second.data() + found->second.size() &&
            value <= 100
        ? value
        : fallback;
}
Error Core::systemSetting(std::string_view key, std::string_view value) {
    if (key == "audio_muted") {
        if (!platform_.mute_apply)
            return Error::not_supported;
        if (value != "0" && value != "1")
            return Error::invalid_argument;
    } else {
        if (!platform_.brightness_apply)
            return Error::not_supported;
        if (key == "brightness_mode") {
            if (value != "manual" && value != "automatic")
                return Error::invalid_argument;
            if (value == "automatic") {
                bool available = false;
                for (const auto &source : sensors_.catalog())
                    available |=
                        (source.channels & sensors::bit(sensors::Channel::ambient_light)) != 0;
                if (!available)
                    return Error::not_supported;
            }
        } else if (key == "brightness_manual" || key == "brightness_fallback") {
            unsigned percent{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), percent);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                percent < 5 || percent > 100)
                return Error::invalid_argument;
        } else
            return Error::invalid_argument;
    }
    const auto previous = settings_;
    settings_[std::string(key)] = value;
    const auto error = persistSettings();
    if (error != Error::ok)
        settings_ = previous;
    else {
        brightnessUntil_ = 0;
        luxValid_ = false;
        systemNext_ = 0;
        if (key == "audio_muted" && platform_.mute_apply)
            muteError_ = platform_.mute_apply(
                settings_.contains("audio_muted") && settings_.at("audio_muted") == "1");
        if (key == "audio_muted" && muteError_ != Error::ok)
            return muteError_; // Policy is durable, but hardware acknowledgement failed.
    }
    return error;
}
void Core::systemTick(uint64_t at) {
    systemNext_ = UINT64_MAX;
    if (platform_.rtc_read && !rtcBootAccepted_ && !trustedTimeSet_) {
        uint64_t epoch{};
        if (platform_.rtc_read(epoch) == Error::ok && epoch >= 1700000000 &&
            epoch <= 4102444800ULL) {
            epoch_ = epoch;
            base_ = at;
            rtcBootAccepted_ = true;
            rtcState_ = 2;
            nextEpochSave_ = UINT64_MAX;
        }
    }
    if (rtcState_ == 1) {
        bool complete = false;
        const auto result =
            platform_.rtc_result ? platform_.rtc_result(rtcToken_, complete) : Error::not_supported;
        if (complete || result != Error::ok || at >= rtcDeadline_) {
            rtcError_ =
                complete ? result : (result != Error::ok ? result : Error::source_unavailable);
            rtcState_ = rtcError_ == Error::ok ? 2 : 3;
            if (rtcState_ == 2) {
                rtcBootAccepted_ = true;
                nextEpochSave_ = UINT64_MAX;
            }
            rtcNext_ = UINT64_MAX;
        } else {
            rtcNext_ = std::min(rtcDeadline_, at > UINT64_MAX - 50 ? UINT64_MAX : at + 50);
            systemNext_ = rtcNext_;
        }
    }
    if (!platform_.brightness_apply)
        return;
    const auto mode = settings_.find("brightness_mode");
    const bool automatic = mode != settings_.end() && mode->second == "automatic";
    const auto mask = sensors::bit(sensors::Channel::ambient_light);
    sensors::SourceId wanted{};
    uint8_t priority = UINT8_MAX;
    if (automatic)
        for (const auto &source : sensors_.catalog())
            if ((source.channels & mask) &&
                (!wanted || source.priority < priority ||
                    (source.priority == priority && source.id < wanted))) {
                wanted = source.id;
                priority = source.priority;
            }
    if (wanted != lightSource_) {
        if (lightSource_)
            sensors_.systemDemand(lightSource_, mask, 0);
        lightSource_ = wanted;
        luxValid_ = false;
        if (wanted)
            brightnessError_ = sensors_.systemDemand(wanted, mask, 1000);
    }
    if (brightnessUntil_ &&
        (at >= brightnessUntil_ || runtime_.consumerGeneration() != brightnessOwner_ ||
            authorizeApp("ui.brightness") != Error::ok))
        brightnessUntil_ = 0;
    unsigned target = settingNumber(settings_, "brightness_manual", 100);
    if (automatic) {
        const auto reading = sensors_.inspect(lightSource_, mask, false, at);
        target = settingNumber(settings_, "brightness_fallback", 50);
        if (reading.error == Error::ok) {
            // Update the filter once per sample identity, never on cache reads.
            const auto lux = reading.sample.values[unsigned(sensors::Channel::ambient_light)].x;
            if (lux >= 0 && std::isfinite(lux)) {
                if (!luxValid_)
                    filteredLux_ = lux;
                else if (reading.sample.sampleId != lightSample_ ||
                    reading.sample.generation != lightGeneration_)
                    filteredLux_ += (lux - filteredLux_) * 0.25;
                lightSample_ = reading.sample.sampleId;
                lightGeneration_ = reading.sample.generation;
                luxValid_ = true;
                target =
                    unsigned(std::clamp(10.0 + 22.5 * std::log10(1.0 + filteredLux_), 10.0, 100.0));
            }
        } else
            luxValid_ = false;
        systemNext_ = std::min(systemNext_, at > UINT64_MAX - 1000 ? UINT64_MAX : at + 1000);
    }
    if (brightnessUntil_) {
        target = brightnessOverride_;
        systemNext_ = std::min(systemNext_, brightnessUntil_);
    }
    const bool due = brightness_ > 100 || at < brightnessAt_ || at - brightnessAt_ >= 1000;
    if (!due && target != brightness_ && brightnessAt_ <= UINT64_MAX - 1000)
        systemNext_ = std::min(systemNext_, brightnessAt_ + 1000);
    if (due && (brightness_ > 100 || std::abs(int(target) - int(brightness_)) >= 3)) {
        const auto value = automatic && brightness_ <= 100 && !brightnessUntil_
            ? unsigned(std::clamp(int(target), int(brightness_) - 5, int(brightness_) + 5))
            : target;
        brightnessError_ = platform_.brightness_apply(value);
        brightnessAt_ = at;
        if (brightnessError_ == Error::ok)
            brightness_ = value;
        else
            systemNext_ = std::min(systemNext_, at > UINT64_MAX - 1000 ? UINT64_MAX : at + 1000);
    }
}
} // namespace flint
