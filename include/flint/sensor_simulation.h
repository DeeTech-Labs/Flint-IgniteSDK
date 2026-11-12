#pragma once
#include <algorithm>
#include <cmath>
#include <flint/sensors.h>

namespace flint::sensors {
// Explicit, deterministic virtual-time backend. No autonomous timer or random data.
// Configured values are only published while a demand is active.
class Simulation final : public Backend {
public:
    Error add(Source source, const std::array<Vector3, 6> &values = {}) {
        if (count_ == MaxSources)
            return Error::quota_exceeded;
        for (size_t i = 0; i < count_; ++i)
            if (sources_[i].id == source.id)
                return Error::invalid_argument;
        source.simulation = true;
        for (const auto &v : values)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                return Error::invalid_argument;
        sources_[count_] = source;
        values_[count_] = values;
        ++count_;
        return Error::ok;
    }
    void available(SourceId id, bool available) {
        (void)status(id, available ? Status::valid : Status::failed,
            available ? Error::ok : Error::source_unavailable);
    }
    Error setValues(SourceId id, const std::array<Vector3, 6> &values) {
        for (const auto &v : values)
            if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                return Error::invalid_argument;
        for (size_t i = 0; i < count_; ++i)
            if (sources_[i].id == id) {
                values_[i] = values;
                return Error::ok;
            }
        return Error::source_unavailable;
    }
    Error setValue(SourceId id, Channel channel, Vector3 value) {
        if (!bit(channel) || !std::isfinite(value.x) || !std::isfinite(value.y) ||
            !std::isfinite(value.z) || std::abs(value.x) > 1e12 || std::abs(value.y) > 1e12 ||
            std::abs(value.z) > 1e12 ||
            (unsigned(channel) < unsigned(Channel::acceleration) && (value.y || value.z)))
            return Error::invalid_argument;
        for (size_t i = 0; i < count_; ++i)
            if (sources_[i].id == id) {
                if (!(sources_[i].channels & bit(channel)))
                    return Error::not_supported;
                values_[i][unsigned(channel)] = value;
                return Error::ok;
            }
        return Error::not_supported;
    }
    Error status(SourceId id, Status state, Error reason) {
        if (unsigned(state) > unsigned(Status::initializing) ||
            (state == Status::valid && reason != Error::ok))
            return Error::invalid_argument;
        for (size_t i = 0; i < count_; ++i)
            if (sources_[i].id == id) {
                states_[i] = state;
                reasons_[i] = reason;
                if (demand_[i].intervalMs || demand_[i].refresh)
                    due_[i] = 0;
                return Error::ok;
            }
        return Error::source_unavailable;
    }
    uint64_t sampleCount() const {
        return samples_;
    }
    uint64_t demandCount() const {
        return demands_;
    }
    void reset(SourceId id) {
        for (size_t i = 0; i < count_; ++i)
            if (sources_[i].id == id && generation_[i] != UINT64_MAX) {
                ++generation_[i];
                sampleId_[i] = 0;
                due_[i] = demand_[i].intervalMs || demand_[i].refresh ? 0 : UINT64_MAX;
            }
    }
    size_t catalog(std::span<Source> output) override {
        std::copy_n(sources_.begin(), std::min(count_, output.size()), output.begin());
        return count_;
    }
    void demand(std::span<const Demand> demands) override {
        ++demands_;
        for (size_t i = 0; i < count_; ++i) {
            Demand next{};
            for (const auto &d : demands)
                if (d.source == sources_[i].id)
                    next = d;
            const auto &old = demand_[i];
            if (next.refresh != old.refresh ||
                (next.intervalMs && (!old.intervalMs || next.intervalMs < old.intervalMs)) ||
                (next.channels & ~old.channels))
                due_[i] = 0;
            demand_[i] = next;
            if (!next.intervalMs && !next.refresh)
                due_[i] = UINT64_MAX;
        }
    }
    size_t poll(uint64_t now, std::span<Sample> output) override {
        size_t n = 0;
        for (size_t i = 0; i < count_ && n < output.size(); ++i) {
            const auto &d = demand_[i];
            if ((!d.intervalMs && !d.refresh) ||
                now < std::max({due_[i], d.notBefore, cooldown_[i]}) || sampleId_[i] == UINT64_MAX)
                continue;
            Sample s{};
            s.source = sources_[i].id;
            s.channels = d.channels;
            s.generation = generation_[i];
            s.sampleId = ++sampleId_[i];
            s.at = now;
            s.status = states_[i];
            s.reason = reasons_[i];
            if (s.status == Status::valid)
                s.values = values_[i];
            output[n++] = s;
            ++samples_;
            if (s.status == Status::valid || s.status == Status::failed ||
                s.status == Status::absent)
                cooldown_[i] = now <= UINT64_MAX - sources_[i].minIntervalMs
                    ? now + sources_[i].minIntervalMs
                    : UINT64_MAX;
            const auto interval = d.intervalMs;
            due_[i] = interval && now <= UINT64_MAX - interval ? now + interval : UINT64_MAX;
        }
        return n;
    }
    uint64_t nextWake() const override {
        uint64_t result = UINT64_MAX;
        for (size_t i = 0; i < count_; ++i)
            if (demand_[i].intervalMs || demand_[i].refresh)
                result = std::min(result, std::max({due_[i], demand_[i].notBefore, cooldown_[i]}));
        return result;
    }
    Simulation() {
        generation_.fill(1);
        states_.fill(Status::valid);
        reasons_.fill(Error::ok);
        due_.fill(UINT64_MAX);
    }

private:
    size_t count_{};
    std::array<Source, MaxSources> sources_{};
    std::array<std::array<Vector3, 6>, MaxSources> values_{};
    std::array<Demand, MaxSources> demand_{};
    std::array<uint64_t, MaxSources> generation_{}, sampleId_{}, due_{}, cooldown_{};
    std::array<Status, MaxSources> states_{};
    std::array<Error, MaxSources> reasons_{};
    uint64_t samples_{}, demands_{};
};
} // namespace flint::sensors
