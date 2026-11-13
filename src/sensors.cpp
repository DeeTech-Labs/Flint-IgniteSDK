#include <algorithm>
#include <cmath>
#include <flint/sensors.h>
#include <limits>

namespace flint::sensors {
namespace {
uint64_t add(uint64_t a, uint64_t b) {
    return b > UINT64_MAX - a ? UINT64_MAX : a + b;
}
bool validMask(Mask m) {
    return m && !(m & ~AllChannels);
}
} // namespace
bool Broker::active(InstanceKey owner) const {
    return owner.id && owner.generation &&
        std::find(owners_.begin(), owners_.end(), owner) != owners_.end();
}
Error Broker::authorized(InstanceKey owner, Mask mask) const {
    if (!validMask(mask))
        return Error::invalid_argument;
    return active(owner) && authorize_ ? authorize_(context_, owner, mask)
                                       : Error::permission_denied;
}
void Broker::attach(Backend *backend) {
    cancelAll();
    if (backend_) {
        std::array<Demand, MaxSources> idle{};
        for (size_t i = 0; i < count_; ++i)
            idle[i].source = sources_[i].id;
        backend_->demand({idle.data(), count_});
    }
    backend_ = backend;
    count_ = 0;
    sources_ = {};
    cache_ = {};
    good_ = {};
    cooldown_ = {};
    system_ = {};
    sent_ = {};
}
void Broker::cancelAll() {
    subscriptions_ = {};
    refreshes_ = {};
    aggregate();
}
Error Broker::activate(InstanceKey owner) {
    if (!owner.id || !owner.generation)
        return Error::invalid_argument;
    for (auto &entry : owners_)
        if (entry.id == owner.id) {
            if (entry == owner)
                return Error::ok;
            if (entry.generation > owner.generation)
                return Error::stale_handle;
            const auto old = entry;
            cancelOwner(old);
            entry = owner;
            return Error::ok;
        }
    for (auto &entry : owners_)
        if (!entry.id) {
            entry = owner;
            return Error::ok;
        }
    return Error::quota_exceeded;
}
void Broker::cancelOwner(InstanceKey owner) {
    for (auto &entry : owners_)
        if (entry == owner)
            entry = {};
    for (auto &entry : subscriptions_)
        if (entry.handle && entry.owner == owner)
            entry = {};
    for (auto &entry : refreshes_)
        if (entry.handle && entry.owner == owner)
            entry = {};
    aggregate();
}
Error Broker::begin() {
    if (!backend_) {
        count_ = 0;
        return Error::ok;
    }
    std::array<Source, MaxSources> catalog{};
    const size_t count = backend_->catalog(catalog);
    if (count > MaxSources)
        return Error::quota_exceeded;
    for (size_t i = 0; i < count; ++i) {
        const auto &s = catalog[i];
        if (!s.id || !validMask(s.channels) || !s.minIntervalMs ||
            s.defaultIntervalMs < s.minIntervalMs || s.maxIntervalMs < s.defaultIntervalMs ||
            !s.staleAfterMs || unsigned(s.temperature) > unsigned(TemperatureKind::die) ||
            unsigned(s.coordinates) > unsigned(Coordinates::device) ||
            !std::isfinite(s.accelerationRange) || !std::isfinite(s.angularVelocityRange) ||
            s.accelerationRange < 0 || s.angularVelocityRange < 0)
            return Error::invalid_argument;
        for (size_t j = 0; j < i; ++j)
            if (s.id == catalog[j].id)
                return Error::invalid_argument;
    }
    cancelAll();
    if (count_) {
        std::array<Demand, MaxSources> idle{};
        for (size_t i = 0; i < count_; ++i)
            idle[i].source = sources_[i].id;
        backend_->demand({idle.data(), count_});
    }
    sources_ = catalog;
    count_ = count;
    cache_ = {};
    good_ = {};
    cooldown_ = {};
    system_ = {};
    sent_ = {};
    aggregate();
    return Error::ok;
}
size_t Broker::index(SourceId id, Mask mask) const {
    size_t best = count_;
    for (size_t i = 0; i < count_; ++i) {
        const auto &s = sources_[i];
        if ((id && s.id != id) || (s.channels & mask) != mask)
            continue;
        if (best == count_ || s.priority < sources_[best].priority ||
            (s.priority == sources_[best].priority && s.id < sources_[best].id))
            best = i;
    }
    return best;
}
Read Broker::read(InstanceKey owner, SourceId id, Mask mask, bool lastKnown, uint64_t now) const {
    Read result;
    if (!validMask(mask)) {
        result.error = Error::invalid_argument;
        return result;
    }
    if (index(id, mask) == count_) {
        result.error = Error::not_supported;
        result.sample.status = Status::absent;
        return result;
    }
    result.error = authorized(owner, mask);
    if (result.error != Error::ok)
        return result;
    return inspect(id, mask, lastKnown, now);
}
Read Broker::inspect(SourceId id, Mask mask, bool lastKnown, uint64_t now) const {
    Read result;
    if (!validMask(mask)) {
        result.error = Error::invalid_argument;
        return result;
    }
    const auto i = index(id, mask);
    if (i == count_) {
        result.sample.status = Status::absent;
        result.error = Error::not_supported;
        return result;
    }
    result.simulation = sources_[i].simulation;
    result.sample = cache_[i];
    result.sample.source = sources_[i].id;
    if (result.sample.status == Status::valid &&
        (result.sample.at > now || now - result.sample.at >= sources_[i].staleAfterMs))
        result.sample.status = Status::stale;
    result.error = result.sample.status == Status::valid && (result.sample.channels & mask) == mask
        ? Error::ok
        : Error::source_unavailable;
    if (lastKnown && result.error != Error::ok && good_[i].sampleId &&
        (good_[i].channels & mask) == mask) {
        const auto status = result.sample.status;
        const auto reason = result.sample.reason;
        result.sample = good_[i];
        result.sample.status = status;
        result.sample.reason = reason;
        result.lastKnown = true;
    }
    if (result.sample.at <= now)
        result.ageMs = now - result.sample.at;
    if (result.error != Error::ok && !result.lastKnown) {
        result.sample.values = {};
        result.sample.channels = 0;
    }
    result.sample.channels &= mask;
    for (size_t c = 0; c < 6; ++c)
        if (!(result.sample.channels & (1u << c)))
            result.sample.values[c] = {};
    return result;
}
uint64_t Broker::allocate() {
    return serial_ == uint64_t(INT64_MAX) ? 0 : ++serial_;
}
Error Broker::subscribe(
    InstanceKey owner, SourceId id, Mask mask, uint32_t interval, uint64_t now, uint64_t &handle) {
    handle = 0;
    if (!validMask(mask))
        return Error::invalid_argument;
    const auto i = index(id, mask);
    if (i == count_)
        return Error::not_supported;
    auto error = authorized(owner, mask);
    if (error != Error::ok)
        return error;
    if (!interval)
        interval = sources_[i].defaultIntervalMs;
    if (interval < sources_[i].minIntervalMs || interval > sources_[i].maxIntervalMs)
        return Error::invalid_argument;
    if (std::count_if(subscriptions_.begin(), subscriptions_.end(), [&](const Entry &e) {
            return e.handle && e.owner == owner;
        }) >= std::ptrdiff_t(MaxSubscriptions))
        return Error::quota_exceeded;
    for (auto &e : subscriptions_)
        if (!e.handle) {
            const auto h = allocate();
            if (!h)
                return Error::resource_exhausted;
            e = {};
            e.handle = h;
            e.owner = owner;
            e.source = sources_[i].id;
            e.channels = mask;
            e.interval = interval;
            e.next = now;
            handle = h;
            aggregate();
            return Error::ok;
        }
    return Error::quota_exceeded;
}
Error Broker::unsubscribe(InstanceKey owner, uint64_t handle) {
    for (auto &e : subscriptions_)
        if (handle && e.handle == handle && e.owner == owner) {
            e = {};
            aggregate();
            return Error::ok;
        }
    return Error::stale_handle;
}
Error Broker::refresh(
    InstanceKey owner, SourceId id, Mask mask, uint64_t timeout, uint64_t now, uint64_t &handle) {
    handle = 0;
    if (!validMask(mask) || !timeout || timeout > 60000)
        return Error::invalid_argument;
    const auto i = index(id, mask);
    if (i == count_)
        return Error::not_supported;
    auto error = authorized(owner, mask);
    if (error != Error::ok)
        return error;
    // Drain results already queued by the worker before capturing the refresh boundary.
    // Millisecond timestamps alone cannot distinguish an old completion in the same tick.
    maintain(now);
    error = authorized(owner, mask);
    if (error != Error::ok)
        return error;
    if (std::count_if(refreshes_.begin(), refreshes_.end(), [&](const Entry &e) {
            return e.handle && e.owner == owner;
        }) >= std::ptrdiff_t(MaxRefresh))
        return Error::quota_exceeded;
    for (auto &e : refreshes_)
        if (!e.handle) {
            auto h = allocate();
            if (!h)
                return Error::resource_exhausted;
            e = {};
            e.handle = h;
            e.owner = owner;
            e.source = sources_[i].id;
            e.channels = mask;
            e.deadline = add(now, timeout);
            e.next = now;
            // Capture cache identity: a refresh cannot complete from an already cached sample.
            e.after = cache_[i].sampleId;
            e.generation = cache_[i].generation;
            handle = h;
            aggregate();
            return Error::ok;
        }
    return Error::quota_exceeded;
}
Error Broker::cancel(InstanceKey owner, uint64_t handle) {
    for (auto &e : refreshes_)
        if (handle && e.handle == handle && e.owner == owner) {
            e = {};
            aggregate();
            return Error::ok;
        }
    return Error::stale_handle;
}
Error Broker::systemDemand(SourceId id, Mask mask, uint32_t interval, uint8_t consumer) {
    if (consumer >= system_.size())
        return Error::invalid_argument;
    if (!id || !validMask(mask))
        return Error::invalid_argument;
    const auto i = index(id, mask);
    if (i == count_)
        return Error::not_supported;
    if (interval && (interval < sources_[i].minIntervalMs || interval > sources_[i].maxIntervalMs))
        return Error::invalid_argument;
    system_[consumer][i] = {id, interval ? mask : Mask(0), interval, 0, 0};
    aggregate();
    return Error::ok;
}
void Broker::aggregate() {
    if (!backend_ || !count_)
        return;
    auto demands = system_[0];
    for (size_t i = 0; i < count_; ++i) {
        const auto &other = system_[1][i];
        demands[i].channels |= other.channels;
        if (other.intervalMs &&
            (!demands[i].intervalMs || other.intervalMs < demands[i].intervalMs))
            demands[i].intervalMs = other.intervalMs;
    }
    for (size_t i = 0; i < count_; ++i)
        demands[i].source = sources_[i].id;
    auto merge = [&](const Entry &e, bool refresh) {
        if (!e.handle || e.finished || authorized(e.owner, e.channels) != Error::ok)
            return;
        const auto i = index(e.source, e.channels);
        if (i == count_)
            return;
        auto &d = demands[i];
        d.channels |= e.channels;
        if (refresh) {
            if (!d.refresh || e.handle < d.refresh)
                d.refresh = e.handle;
            if (!d.deadline || e.deadline < d.deadline)
                d.deadline = e.deadline;
        } else if (!d.intervalMs || e.interval < d.intervalMs)
            d.intervalMs = e.interval;
    };
    for (const auto &e : subscriptions_)
        merge(e, false);
    for (const auto &e : refreshes_)
        merge(e, true);
    for (size_t i = 0; i < count_; ++i)
        if (demands[i].intervalMs || demands[i].refresh)
            demands[i].notBefore = cooldown_[i];
    bool changed = false;
    for (size_t i = 0; i < count_; ++i) {
        const auto &a = demands[i], &b = sent_[i];
        changed |= a.source != b.source || a.channels != b.channels ||
            a.intervalMs != b.intervalMs || a.refresh != b.refresh || a.deadline != b.deadline ||
            a.notBefore != b.notBefore;
    }
    if (changed) {
        sent_ = demands;
        backend_->demand({sent_.data(), count_});
    }
}
void Broker::notify(Entry &e, bool finished) {
    if (e.pending && e.drops != UINT32_MAX)
        ++e.drops;
    e.pending = true;
    e.finished = finished;
}
Error Broker::ingest(const Sample &sample, uint64_t now) {
    const auto i = index(sample.source, sample.channels);
    if (!sample.source || !validMask(sample.channels) || i == count_ || !sample.generation ||
        !sample.sampleId || sample.at > now ||
        unsigned(sample.status) > unsigned(Status::initializing))
        return Error::invalid_argument;
    const auto &previous = cache_[i];
    if (sample.at < previous.at || sample.generation < previous.generation ||
        (sample.generation == previous.generation &&
            (sample.sampleId <= previous.sampleId || sample.at < previous.at)))
        return Error::stale_handle;
    if (sample.status == Status::valid) {
        if (sample.reason != Error::ok)
            return Error::invalid_argument;
        for (size_t c = 0; c < 6; ++c)
            if (sample.channels & (1u << c)) {
                const auto &v = sample.values[c];
                if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z))
                    return Error::invalid_argument;
            }
    }
    const bool completed = sample.status == Status::valid || sample.status == Status::failed ||
        sample.status == Status::absent;
    if (completed && sample.at < cooldown_[i])
        return Error::invalid_argument;
    if (completed)
        cooldown_[i] = add(sample.at, sources_[i].minIntervalMs);
    if (previous.generation != sample.generation)
        good_[i] = {};
    cache_[i] = sample;
    if (sample.status == Status::valid)
        good_[i] = sample;
    for (auto &e : subscriptions_)
        if (e.handle && !e.finished && authorized(e.owner, e.channels) == Error::ok &&
            e.source == sample.source && (sample.channels & e.channels) == e.channels &&
            now >= e.next) {
            notify(e, false);
            e.next = add(now, e.interval);
        }
    for (auto &e : refreshes_)
        if (e.handle && !e.finished && authorized(e.owner, e.channels) == Error::ok &&
            (sample.status == Status::valid || sample.status == Status::failed ||
                sample.status == Status::absent) &&
            e.source == sample.source && (sample.channels & e.channels) == e.channels &&
            sample.at >= e.next && sample.at <= e.deadline &&
            (sample.generation > e.generation ||
                (sample.generation == e.generation && sample.sampleId > e.after)))
            notify(e, true);
    aggregate();
    return Error::ok;
}
void Broker::maintain(uint64_t now) {
    for (auto &e : subscriptions_)
        if (e.handle) {
            if (authorized(e.owner, e.channels) != Error::ok)
                e = {};
        }
    for (auto &e : refreshes_)
        if (e.handle) {
            auto error = authorized(e.owner, e.channels);
            if (error != Error::ok)
                e = {};
        }
    aggregate();
    if (backend_) {
        std::array<Sample, 1> sample{};
        for (size_t i = 0; i < MaxSources; ++i) {
            if (!backend_->poll(now, sample))
                break;
            (void)ingest(sample[0], now);
        }
    }
    // A completed worker sample may have arrived before its deadline while Core was busy.
    // Drain transport first; timeout is based on acquisition/completion, not delivery delay.
    for (auto &entry : refreshes_)
        if (entry.handle && !entry.finished && now >= entry.deadline) {
            entry.expired = true;
            notify(entry, true);
        }
    aggregate();
}
bool Broker::takeEvent(InstanceKey owner, Event &event, uint64_t now) {
    auto take = [&](auto &entries, bool refresh) {
        for (auto &e : entries)
            if (e.handle && e.owner == owner && e.pending) {
                if (authorized(e.owner, e.channels) != Error::ok) {
                    e = {};
                    aggregate();
                    continue;
                }
                event = {};
                event.handle = e.handle;
                event.drops = e.drops;
                event.refreshFinished = refresh;
                event.reading = read(owner, e.source, e.channels, false, now);
                if (refresh && e.expired) {
                    event.reading = {};
                    event.reading.error = Error::source_unavailable;
                    event.reading.sample.source = e.source;
                    event.reading.sample.status = Status::failed;
                    event.reading.sample.reason = Error::source_unavailable;
                    const auto source = index(e.source, e.channels);
                    event.reading.simulation = source < count_ && sources_[source].simulation;
                }
                e.pending = false;
                e.drops = 0;
                if (e.finished || refresh)
                    e = {};
                aggregate();
                return true;
            }
        return false;
    };
    return take(subscriptions_, false) || take(refreshes_, true);
}
uint64_t Broker::nextWake(InstanceKey owner) const {
    if (!active(owner))
        return UINT64_MAX;
    uint64_t next = UINT64_MAX;
    for (const auto &entry : refreshes_)
        if (entry.handle && entry.owner == owner) {
            if (authorized(owner, entry.channels) != Error::ok)
                continue;
            if (entry.pending)
                return 0;
            if (!entry.finished)
                next = std::min(next, entry.deadline);
        }
    for (const auto &entry : subscriptions_)
        if (entry.handle && entry.owner == owner && entry.pending &&
            authorized(owner, entry.channels) == Error::ok)
            return 0;
    return next;
}
uint64_t Broker::nextWake() const {
    uint64_t next = backend_ ? backend_->nextWake() : UINT64_MAX;
    for (const auto &owner : owners_)
        if (owner.id)
            next = std::min(next, nextWake(owner));
    return next;
}
} // namespace flint::sensors
