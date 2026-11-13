#include <cassert>
#include <cmath>
#include <flint/sensor_simulation.h>
#include <iostream>

using namespace flint;
using namespace flint::sensors;
namespace {
Error allow(void *context, InstanceKey, Mask) {
    return *static_cast<bool *>(context) ? Error::ok : Error::permission_denied;
}
constexpr Mask temperature = bit(Channel::temperature);
struct Rig {
    Simulation backend;
    Broker broker;
    bool allowed{true};
    InstanceKey owner{1, 1};
    Rig() {
        Source s;
        s.id = 2;
        s.channels = temperature;
        s.staleAfterMs = 500;
        std::array<Vector3, 6> values{};
        values[0].x = 21.5;
        assert(backend.add(s, values) == Error::ok);
        broker.attach(&backend);
        broker.authorization(allow, &allowed);
        assert(broker.activate(owner) == Error::ok);
        assert(broker.begin() == Error::ok);
    }
};
// Transport-only fixture: a physical completion can wait in its mailbox while Core is busy.
class QueuedBackend final : public Backend {
public:
    size_t catalog(std::span<Source> output) override {
        if (!output.empty()) {
            Source source;
            source.id = 1;
            source.channels = temperature;
            source.minIntervalMs = 10;
            source.defaultIntervalMs = 100;
            output[0] = source;
        }
        return 1;
    }
    void demand(std::span<const Demand>) override {}
    size_t poll(uint64_t now, std::span<Sample> output) override {
        if (!pending_ || output.empty() || sample_.at > now)
            return 0;
        output[0] = sample_;
        pending_ = false;
        return 1;
    }
    uint64_t nextWake() const override {
        return UINT64_MAX;
    }
    void complete(uint64_t at, uint64_t id) {
        assert(!pending_);
        sample_ = {};
        sample_.source = 1;
        sample_.channels = temperature;
        sample_.generation = 1;
        sample_.sampleId = id;
        sample_.at = at;
        sample_.status = Status::valid;
        sample_.reason = Error::ok;
        sample_.values[0].x = 24;
        pending_ = true;
    }

private:
    Sample sample_{};
    bool pending_{};
};
struct MultiRig {
    Simulation backend;
    Broker broker;
    std::array<bool, MaxConsumers + 1> grants{};
    static Error authorize(void *context, InstanceKey owner, Mask) {
        const auto &self = *static_cast<MultiRig *>(context);
        return owner.id < self.grants.size() && self.grants[owner.id] ? Error::ok
                                                                      : Error::permission_denied;
    }
    MultiRig() {
        grants.fill(true);
        Source source;
        source.id = 2;
        source.channels = temperature;
        assert(backend.add(source) == Error::ok);
        broker.attach(&backend);
        broker.authorization(authorize, this);
        assert(broker.begin() == Error::ok);
        for (size_t id = 1; id <= MaxConsumers; ++id)
            assert(broker.activate({id, 1}) == Error::ok);
    }
};
void multiOwnerTests() {
    constexpr InstanceKey a{1, 1}, b{2, 1}, replacement{1, 2};
    MultiRig rig;
    uint64_t fast{}, slow{};
    assert(rig.broker.activate({0, 1}) == Error::invalid_argument);
    assert(rig.broker.activate({6, 0}) == Error::invalid_argument);
    assert(rig.broker.activate({MaxConsumers + 1, 1}) == Error::quota_exceeded);
    assert(rig.broker.subscribe(a, 2, temperature, 100, 0, fast) == Error::ok);
    assert(rig.broker.subscribe(b, 2, temperature, 300, 0, slow) == Error::ok);
    assert(rig.broker.unsubscribe(a, slow) == Error::stale_handle);
    assert(rig.broker.unsubscribe(b, fast) == Error::stale_handle);
    rig.broker.maintain(0);
    assert(rig.backend.sampleCount() == 1);
    assert(rig.broker.nextWake(a) == 0 && rig.broker.nextWake(b) == 0);
    Event event;
    assert(rig.broker.takeEvent(a, event, 0) && event.handle == fast);
    const auto shared = event.reading.sample.sampleId;
    assert(!rig.broker.takeEvent(a, event, 0)); // Other owner's pending event is untouched.
    assert(rig.broker.takeEvent(b, event, 0) && event.handle == slow &&
        event.reading.sample.sampleId == shared);
    assert(rig.broker.nextWake(a) == UINT64_MAX); // Physical wake belongs to the global scheduler.
    assert(rig.broker.nextWake() == 100);
    rig.broker.maintain(100);
    rig.broker.maintain(200);
    rig.broker.maintain(300);
    assert(rig.backend.sampleCount() == 4); // One physical source, not two per-owner caches.
    assert(rig.broker.takeEvent(b, event, 300) && event.handle == slow && event.drops == 0);
    const auto latest = event.reading.sample.sampleId;
    assert(rig.broker.takeEvent(a, event, 300) && event.handle == fast && event.drops == 2);
    assert(event.reading.sample.sampleId == latest);

    // Revoking one principal removes only its requests and leaves the slower owner live.
    rig.grants[a.id] = false;
    rig.broker.maintain(301);
    assert(rig.broker.read(a, 2, temperature, true, 301).error == Error::permission_denied);
    assert(rig.broker.unsubscribe(a, fast) == Error::stale_handle);
    rig.broker.maintain(600);
    assert(!rig.broker.takeEvent(a, event, 600));
    assert(rig.broker.takeEvent(b, event, 600) && event.handle == slow);
    assert(rig.broker.nextWake() == 900);
    rig.grants[a.id] = true;
    assert(rig.broker.subscribe(a, 2, temperature, 100, 600, fast) == Error::ok);
    rig.broker.maintain(700);
    assert(rig.broker.activate(replacement) == Error::ok); // Also succeeds with all slots occupied.
    assert(rig.broker.activate(a) == Error::stale_handle);
    assert(rig.broker.read(a, 2, temperature, false, 700).error == Error::permission_denied);
    assert(rig.broker.unsubscribe(replacement, fast) == Error::stale_handle);
    assert(!rig.broker.takeEvent(replacement, event, 700));
    rig.broker.cancelOwner(
        a); // Late destruction of the old generation cannot stop its replacement.
    assert(rig.broker.subscribe(replacement, 2, temperature, 100, 700, fast) == Error::ok);
    assert(rig.broker.unsubscribe(b, slow) == Error::ok);
    rig.broker.cancelOwner(replacement);
    assert(rig.broker.nextWake() == UINT64_MAX);

    MultiRig delivery;
    assert(delivery.broker.subscribe(a, 2, temperature, 100, 0, fast) == Error::ok);
    assert(delivery.broker.subscribe(b, 2, temperature, 100, 0, slow) == Error::ok);
    delivery.broker.maintain(0);
    delivery.grants[a.id] = false; // Revocation after ingest but before dispatch.
    assert(!delivery.broker.takeEvent(a, event, 0));
    assert(delivery.broker.takeEvent(b, event, 0) && event.handle == slow);
    assert(delivery.broker.unsubscribe(a, fast) == Error::stale_handle);
    assert(delivery.broker.unsubscribe(b, slow) == Error::ok);
    delivery.grants[a.id] = true;
    delivery.backend.status(2, Status::pending, Error::source_unavailable);
    assert(delivery.broker.refresh(a, 2, temperature, 100, 100, fast) == Error::ok);
    assert(delivery.broker.refresh(b, 2, temperature, 300, 100, slow) == Error::ok);
    delivery.broker.maintain(200);
    assert(delivery.broker.nextWake(a) == 0 && delivery.broker.nextWake(b) == 400);
    assert(delivery.broker.takeEvent(a, event, 200) && event.handle == fast &&
        event.reading.error == Error::source_unavailable);
    assert(!delivery.broker.takeEvent(b, event, 200));
    assert(delivery.broker.cancel(b, slow) == Error::ok);

    MultiRig quotas;
    quotas.backend.status(2, Status::pending, Error::source_unavailable);
    std::array<uint64_t, MaxConsumers> firstSubscription{}, firstRefresh{};
    for (size_t id = 1; id <= MaxConsumers; ++id) {
        for (size_t slot = 0; slot < MaxSubscriptions; ++slot) {
            assert(quotas.broker.subscribe({id, 1}, 2, temperature, 100, 0, fast) == Error::ok);
            if (!slot)
                firstSubscription[id - 1] = fast;
        }
        assert(quotas.broker.subscribe({id, 1}, 2, temperature, 100, 0, fast) ==
            Error::quota_exceeded);
        for (size_t slot = 0; slot < MaxRefresh; ++slot) {
            assert(quotas.broker.refresh({id, 1}, 2, temperature, 1000, 0, fast) == Error::ok);
            if (!slot)
                firstRefresh[id - 1] = fast;
        }
        assert(
            quotas.broker.refresh({id, 1}, 2, temperature, 1000, 0, fast) == Error::quota_exceeded);
    }
    assert(quotas.broker.cancel(a, firstRefresh[1]) == Error::stale_handle);
    assert(quotas.broker.cancel(b, firstRefresh[0]) == Error::stale_handle);
    quotas.grants[a.id] = false;
    quotas.broker.maintain(1);
    assert(quotas.broker.unsubscribe(a, firstSubscription[0]) == Error::stale_handle);
    assert(quotas.broker.cancel(a, firstRefresh[0]) == Error::stale_handle);
    quotas.grants[a.id] = true;
    for (size_t slot = 0; slot < MaxSubscriptions; ++slot)
        assert(quotas.broker.subscribe(a, 2, temperature, 100, 1, fast) == Error::ok);
    // Both owner's refreshes share a conversion, but their handles/events remain private.
    quotas.broker.cancelAll();
    assert(quotas.broker.refresh(a, 2, temperature, 500, 100, fast) == Error::ok);
    assert(quotas.broker.refresh(b, 2, temperature, 900, 100, slow) == Error::ok);
    assert(quotas.broker.nextWake(a) == 600 && quotas.broker.nextWake(b) == 1000);
    quotas.backend.status(2, Status::valid, Error::ok);
    quotas.broker.maintain(100);
    assert(quotas.broker.takeEvent(b, event, 100) && event.handle == slow);
    const auto refreshId = event.reading.sample.sampleId;
    assert(quotas.broker.takeEvent(a, event, 100) && event.handle == fast &&
        event.reading.sample.sampleId == refreshId);
    assert(!quotas.broker.takeEvent(a, event, 100) && !quotas.broker.takeEvent(b, event, 100));
    assert(quotas.broker.systemDemand(2, temperature, 100) == Error::ok);
    assert(quotas.broker.systemDemand(2, temperature, 300, 1) == Error::ok);
    quotas.broker.cancelAll();
    quotas.broker.cancelOwner(a);
    quotas.broker.cancelOwner(b);
    quotas.broker.maintain(200);
    assert(quotas.broker.inspect(2, temperature, false, 200).error == Error::ok);
    assert(quotas.broker.systemDemand(2, temperature, 0) == Error::ok);
    assert(quotas.broker.nextWake() != UINT64_MAX); // Trusted diagnostic demand survived.
    assert(quotas.broker.systemDemand(2, temperature, 0, 1) == Error::ok);
    assert(quotas.broker.nextWake() == UINT64_MAX);
}
void delayedRefreshTests() {
    QueuedBackend backend;
    Broker broker;
    const InstanceKey owner{1, 1};
    bool permitted = true;
    broker.attach(&backend);
    broker.authorization(allow, &permitted);
    assert(broker.activate(owner) == Error::ok);
    assert(broker.begin() == Error::ok);
    uint64_t handle{};
    Event event;
    assert(broker.refresh(owner, 1, temperature, 100, 0, handle) == Error::ok);
    backend.complete(80, 1);
    broker.maintain(120); // Delivered after deadline; physical completion was on time.
    assert(broker.takeEvent(owner, event, 120) && event.handle == handle && event.refreshFinished);
    assert(event.reading.error == Error::ok && event.reading.sample.at == 80);
    assert(event.reading.ageMs == 40 && event.reading.sample.values[0].x == 24);

    assert(broker.refresh(owner, 1, temperature, 100, 200, handle) == Error::ok);
    backend.complete(301, 2);
    broker.maintain(350); // Fresh cache data cannot make a late conversion satisfy this refresh.
    assert(broker.read(owner, 1, temperature, false, 350).error == Error::ok);
    assert(broker.takeEvent(owner, event, 350) && event.handle == handle && event.refreshFinished);
    assert(event.reading.error == Error::source_unavailable &&
        event.reading.sample.status == Status::failed);
    assert(!event.reading.sample.channels && event.reading.sample.values[0].x == 0);

    assert(broker.refresh(owner, 1, temperature, 100, 400, handle) == Error::ok);
    backend.complete(500, 3);
    broker.maintain(550);
    assert(broker.takeEvent(owner, event, 550) && event.reading.error == Error::ok);
    assert(event.reading.sample.at == 500); // Inclusive completion deadline.
}
} // namespace
int main() {
    delayedRefreshTests();
    multiOwnerTests();
    {
        Rig freshness;
        uint64_t subscription{};
        freshness.backend.available(2, false);
        assert(freshness.broker.systemDemand(2, temperature, 100) == Error::ok);
        assert(freshness.broker.subscribe(freshness.owner, 2, temperature, 1000, 0, subscription) ==
            Error::ok);
        freshness.broker.maintain(0); // Pending failure notification for the slow subscriber.
        freshness.backend.available(2, true);
        freshness.broker.maintain(100);
        Event latest;
        assert(freshness.broker.takeEvent(freshness.owner, latest, 100) &&
            latest.handle == subscription);
        assert(latest.reading.error == Error::ok && latest.reading.sample.status == Status::valid);
        assert(latest.reading.sample.at == 100 && latest.reading.sample.values[0].x == 21.5);
        assert(latest.drops == 0); // Faster physical samples do not invent slow subscriber events.
    }
    Rig r;
    auto reading = r.broker.read(r.owner, 0, temperature, false, 0);
    assert(reading.error == Error::source_unavailable && reading.sample.status == Status::pending);
    assert(r.broker.read(r.owner, 99, temperature, false, 0).sample.status == Status::absent);
    assert(r.broker.nextWake() == UINT64_MAX);
    uint64_t h{};
    assert(r.broker.refresh(r.owner, 0, temperature, 100, 0, h) == Error::ok && h);
    r.broker.maintain(0);
    Event event;
    assert(r.broker.takeEvent(r.owner, event, 0) && event.handle == h && event.refreshFinished);
    assert(event.reading.error == Error::ok && event.reading.sample.values[0].x == 21.5);
    assert(r.broker.cancel(r.owner, h) == Error::stale_handle);
    assert(r.broker.nextWake() == UINT64_MAX);
    assert(r.broker.read(r.owner, 0, temperature, false, 500).sample.status == Status::stale);
    assert(r.broker.read(r.owner, 0, temperature, true, 500).lastKnown);

    assert(r.broker.subscribe(r.owner, 2, temperature, 100, 500, h) == Error::ok);
    r.broker.maintain(500);
    r.broker.maintain(600);
    r.broker.maintain(700);
    assert(r.broker.takeEvent(r.owner, event, 700) && event.drops == 2);
    assert(event.reading.sample.at == 700);
    r.backend.available(2, false);
    r.broker.maintain(800);
    assert(r.broker.read(r.owner, 2, temperature, false, 800).sample.status == Status::failed);
    auto known = r.broker.read(r.owner, 2, temperature, true, 800);
    assert(known.lastKnown && known.sample.at == 700 && known.error != Error::ok);
    r.allowed = false;
    assert(!r.broker.takeEvent(r.owner, event, 800));
    r.broker.maintain(801);
    assert(!r.broker.takeEvent(r.owner, event, 801));
    assert(r.broker.unsubscribe(r.owner, h) == Error::stale_handle);
    r.allowed = true;
    r.backend.available(2, true);
    assert(r.broker.subscribe(r.owner, 2, temperature, 100, 900, h) == Error::ok);
    r.broker.maintain(900);
    r.backend.reset(2);
    r.broker.maintain(1000);
    assert(r.broker.read(r.owner, 2, temperature, false, 1000).sample.generation == 2);
    auto bad = r.broker.read(r.owner, 2, temperature, false, 1000).sample;
    ++bad.sampleId;
    bad.values[0].x = std::nan("");
    assert(r.broker.ingest(bad, 1000) == Error::invalid_argument);
    bad.values[0].x = 10;
    bad.generation = 1;
    assert(r.broker.ingest(bad, 1000) == Error::stale_handle);
    const auto old = h;
    r.owner = {1, 2};
    assert(r.broker.activate(r.owner) == Error::ok);
    assert(r.broker.unsubscribe(r.owner, old) == Error::stale_handle);
    assert(!r.broker.takeEvent(r.owner, event, 1000));
    assert(r.broker.subscribe(r.owner, 2, temperature, 100, 1000, h) == Error::ok && h > old);
    assert(r.broker.systemDemand(2, temperature, 200) == Error::ok);
    assert(r.broker.systemDemand(2, temperature, 100, 1) == Error::ok);
    r.broker.cancelOwner(r.owner);
    r.broker.maintain(1200);
    assert(r.broker.read(r.owner, 2, temperature, false, 1200).error == Error::permission_denied);
    assert(r.broker.nextWake() == 1300);
    assert(r.broker.inspect(2, temperature, false, 1200).error == Error::ok);
    assert(r.broker.inspect(2, temperature, false, 1200).sample.values[1].x == 0);
    assert(r.broker.systemDemand(2, temperature, 0) == Error::ok);
    assert(r.broker.nextWake() != UINT64_MAX);
    assert(r.broker.systemDemand(2, temperature, 0, 1) == Error::ok);
    assert(r.broker.nextWake() == UINT64_MAX);
    r.owner = {3, 3};
    assert(r.broker.activate(r.owner) == Error::ok);
    for (size_t i = 0; i < MaxSubscriptions; ++i)
        assert(r.broker.subscribe(r.owner, 2, temperature, 100, 0, h) == Error::ok);
    assert(r.broker.subscribe(r.owner, 2, temperature, 100, 0, h) == Error::quota_exceeded);
    r.broker.cancelAll();
    r.backend.status(2, Status::pending, Error::source_unavailable);
    for (size_t i = 0; i < MaxRefresh; ++i)
        assert(r.broker.refresh(r.owner, 2, temperature, 100, 2000, h) == Error::ok);
    assert(r.broker.refresh(r.owner, 2, temperature, 100, 2000, h) == Error::quota_exceeded);
    r.broker.maintain(2100);
    size_t events = 0;
    while (r.broker.takeEvent(r.owner, event, 2100)) {
        ++events;
        assert(event.reading.error == Error::source_unavailable);
    }
    assert(events == MaxRefresh);
    Broker denied;
    const InstanceKey deniedOwner{1, 1};
    assert(denied.activate(deniedOwner) == Error::ok);
    assert(denied.begin() == Error::ok);
    assert(denied.read(deniedOwner, 0, temperature, false, 0).error == Error::not_supported);
    Simulation selection;
    Source s;
    s.channels = temperature;
    s.id = 9;
    s.priority = 1;
    selection.add(s);
    s.id = 4;
    selection.add(s);
    denied.attach(&selection);
    denied.authorization(allow, &r.allowed);
    assert(denied.begin() == Error::ok);
    assert(denied.read(deniedOwner, 0, temperature, false, 0).sample.source == 4);

    // Demand arbitration schedules a faster consumer immediately; removing it preserves slower
    // demand.
    Rig arbitration;
    assert(arbitration.broker.systemDemand(2, temperature, 1000) == Error::ok);
    arbitration.broker.maintain(0);
    assert(arbitration.backend.sampleCount() == 1 && arbitration.broker.nextWake() == 1000);
    const auto calls = arbitration.backend.demandCount();
    arbitration.broker.maintain(1);
    assert(arbitration.backend.sampleCount() == 1 && arbitration.backend.demandCount() == calls);
    assert(arbitration.broker.systemDemand(2, temperature, 100, 1) == Error::ok);
    arbitration.broker.maintain(50);
    assert(arbitration.backend.sampleCount() == 1 && arbitration.broker.nextWake() == 100);
    arbitration.broker.maintain(100);
    assert(arbitration.backend.sampleCount() == 2 && arbitration.broker.nextWake() == 200);
    assert(arbitration.broker.systemDemand(2, temperature, 0, 1) == Error::ok);
    arbitration.broker.maintain(200);
    assert(arbitration.broker.nextWake() == 1200);
    auto future = arbitration.broker.inspect(2, temperature, false, 200).sample;
    ++future.sampleId;
    future.at = 201;
    assert(arbitration.broker.ingest(future, 200) == Error::invalid_argument);
    // A reset cannot expose the previous generation as last-known data.
    arbitration.backend.reset(2);
    arbitration.backend.available(2, false);
    arbitration.broker.maintain(300);
    const auto reset = arbitration.broker.inspect(2, temperature, true, 300);
    assert(!reset.lastKnown && reset.sample.values[0].x == 0 && reset.sample.generation == 2);
    assert(arbitration.broker.systemDemand(2, temperature, 0) == Error::ok);
    arbitration.backend.reset(2);
    assert(arbitration.broker.nextWake() == UINT64_MAX);
    // Pending conversion completes only after an explicit result, with one shared sample.
    arbitration.backend.status(2, Status::pending, Error::source_unavailable);
    uint64_t first{}, second{};
    assert(arbitration.broker.refresh(arbitration.owner, 2, temperature, 500, 400, first) ==
        Error::ok);
    assert(arbitration.broker.refresh(arbitration.owner, 2, temperature, 500, 400, second) ==
        Error::ok);
    arbitration.broker.maintain(400);
    assert(!arbitration.broker.takeEvent(arbitration.owner, event, 400));
    arbitration.backend.status(2, Status::valid, Error::ok);
    arbitration.broker.maintain(450);
    assert(arbitration.broker.takeEvent(arbitration.owner, event, 450) &&
        event.reading.error == Error::ok);
    const auto sharedId = event.reading.sample.sampleId;
    assert(arbitration.broker.takeEvent(arbitration.owner, event, 450) &&
        event.reading.sample.sampleId == sharedId);
    assert(arbitration.broker.nextWake() == UINT64_MAX);
    std::array<Vector3, 6> invalid{};
    invalid[0].x = INFINITY;
    assert(arbitration.backend.setValues(2, invalid) == Error::invalid_argument);
    assert(arbitration.broker.refresh(arbitration.owner, 2, temperature, 60001, 450, h) ==
        Error::invalid_argument);
    arbitration.backend.status(2, Status::valid, Error::ok);
    assert(arbitration.broker.refresh(arbitration.owner, 2, temperature, 500, 450, h) == Error::ok);
    const auto physical = arbitration.backend.sampleCount();
    arbitration.broker.maintain(450);
    assert(arbitration.backend.sampleCount() == physical && arbitration.broker.nextWake() == 550);
    arbitration.broker.maintain(550);
    assert(arbitration.broker.takeEvent(arbitration.owner, event, 550) && event.reading.simulation);
    assert(arbitration.backend.sampleCount() == physical + 1);
    // Unsupported channels resolve before application authorization.
    arbitration.allowed = false;
    const Mask pressure = bit(Channel::pressure);
    assert(arbitration.broker.read(arbitration.owner, 0, pressure, false, 550).error ==
        Error::not_supported);
    assert(arbitration.broker.subscribe(arbitration.owner, 0, pressure, 100, 550, h) ==
        Error::not_supported);
    assert(arbitration.broker.refresh(arbitration.owner, 0, pressure, 100, 550, h) ==
        Error::not_supported);
    assert(arbitration.broker.systemDemand(2, pressure, 100) == Error::not_supported);
    arbitration.allowed = true;
    arbitration.backend.status(2, Status::initializing, Error::source_unavailable);
    assert(arbitration.broker.refresh(arbitration.owner, 2, temperature, 500, 650, h) == Error::ok);
    arbitration.broker.maintain(650);
    assert(arbitration.broker.read(arbitration.owner, 2, temperature, false, 650).sample.status ==
        Status::initializing);
    assert(!arbitration.broker.takeEvent(arbitration.owner, event, 650));
    arbitration.allowed = false;
    arbitration.broker.maintain(651);
    assert(arbitration.broker.cancel(arbitration.owner, h) == Error::stale_handle);
    assert(!arbitration.broker.takeEvent(arbitration.owner, event, 651));
    assert(arbitration.broker.nextWake() == UINT64_MAX);
    arbitration.allowed = true;
    arbitration.backend.status(2, Status::failed, Error::invalid_argument);
    assert(arbitration.broker.refresh(arbitration.owner, 2, temperature, 500, 750, h) == Error::ok);
    arbitration.broker.maintain(750);
    assert(arbitration.broker.takeEvent(arbitration.owner, event, 750));
    assert(event.reading.sample.status == Status::failed &&
        event.reading.sample.reason == Error::invalid_argument);
    std::cout << "sizeof Broker=" << sizeof(Broker) << " Source=" << sizeof(Source)
              << " Sample=" << sizeof(Sample) << " Event=" << sizeof(Event)
              << " Simulation=" << sizeof(Simulation) << '\n';
}
