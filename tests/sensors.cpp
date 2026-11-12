#include <cassert>
#include <cmath>
#include <flint/sensor_simulation.h>
#include <iostream>

using namespace flint;
using namespace flint::sensors;
namespace {
Error allow(void *context, Mask) {
    return *static_cast<bool *>(context) ? Error::ok : Error::permission_denied;
}
constexpr Mask temperature = bit(Channel::temperature);
struct Rig {
    Simulation backend;
    Broker broker;
    bool allowed{true};
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
        broker.owner(1, 1);
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
void delayedRefreshTests() {
    QueuedBackend backend;
    Broker broker;
    bool permitted = true;
    broker.attach(&backend);
    broker.authorization(allow, &permitted);
    broker.owner(1, 1);
    assert(broker.begin() == Error::ok);
    uint64_t handle{};
    Event event;
    assert(broker.refresh(1, temperature, 100, 0, handle) == Error::ok);
    backend.complete(80, 1);
    broker.maintain(120); // Delivered after deadline; physical completion was on time.
    assert(broker.takeEvent(event, 120) && event.handle == handle && event.refreshFinished);
    assert(event.reading.error == Error::ok && event.reading.sample.at == 80);
    assert(event.reading.ageMs == 40 && event.reading.sample.values[0].x == 24);

    assert(broker.refresh(1, temperature, 100, 200, handle) == Error::ok);
    backend.complete(301, 2);
    broker.maintain(350); // Fresh cache data cannot make a late conversion satisfy this refresh.
    assert(broker.read(1, temperature, false, 350).error == Error::ok);
    assert(broker.takeEvent(event, 350) && event.handle == handle && event.refreshFinished);
    assert(event.reading.error == Error::source_unavailable &&
        event.reading.sample.status == Status::failed);
    assert(!event.reading.sample.channels && event.reading.sample.values[0].x == 0);

    assert(broker.refresh(1, temperature, 100, 400, handle) == Error::ok);
    backend.complete(500, 3);
    broker.maintain(550);
    assert(broker.takeEvent(event, 550) && event.reading.error == Error::ok);
    assert(event.reading.sample.at == 500); // Inclusive completion deadline.
}
} // namespace
int main() {
    delayedRefreshTests();
    {
        Rig freshness;
        uint64_t subscription{};
        freshness.backend.available(2, false);
        assert(freshness.broker.systemDemand(2, temperature, 100) == Error::ok);
        assert(freshness.broker.subscribe(2, temperature, 1000, 0, subscription) == Error::ok);
        freshness.broker.maintain(0); // Pending failure notification for the slow subscriber.
        freshness.backend.available(2, true);
        freshness.broker.maintain(100);
        Event latest;
        assert(freshness.broker.takeEvent(latest, 100) && latest.handle == subscription);
        assert(latest.reading.error == Error::ok && latest.reading.sample.status == Status::valid);
        assert(latest.reading.sample.at == 100 && latest.reading.sample.values[0].x == 21.5);
        assert(latest.drops == 0); // Faster physical samples do not invent slow subscriber events.
    }
    Rig r;
    auto reading = r.broker.read(0, temperature, false, 0);
    assert(reading.error == Error::source_unavailable && reading.sample.status == Status::pending);
    assert(r.broker.read(99, temperature, false, 0).sample.status == Status::absent);
    assert(r.broker.nextWake() == UINT64_MAX);
    uint64_t h{};
    assert(r.broker.refresh(0, temperature, 100, 0, h) == Error::ok && h);
    r.broker.maintain(0);
    Event event;
    assert(r.broker.takeEvent(event, 0) && event.handle == h && event.refreshFinished);
    assert(event.reading.error == Error::ok && event.reading.sample.values[0].x == 21.5);
    assert(r.broker.cancel(h) == Error::stale_handle);
    assert(r.broker.nextWake() == UINT64_MAX);
    assert(r.broker.read(0, temperature, false, 500).sample.status == Status::stale);
    assert(r.broker.read(0, temperature, true, 500).lastKnown);

    assert(r.broker.subscribe(2, temperature, 100, 500, h) == Error::ok);
    r.broker.maintain(500);
    r.broker.maintain(600);
    r.broker.maintain(700);
    assert(r.broker.takeEvent(event, 700) && event.drops == 2);
    assert(event.reading.sample.at == 700);
    r.backend.available(2, false);
    r.broker.maintain(800);
    assert(r.broker.read(2, temperature, false, 800).sample.status == Status::failed);
    auto known = r.broker.read(2, temperature, true, 800);
    assert(known.lastKnown && known.sample.at == 700 && known.error != Error::ok);
    r.allowed = false;
    assert(!r.broker.takeEvent(event, 800));
    r.broker.maintain(801);
    assert(!r.broker.takeEvent(event, 801));
    assert(r.broker.unsubscribe(h) == Error::stale_handle);
    r.allowed = true;
    r.backend.available(2, true);
    assert(r.broker.subscribe(2, temperature, 100, 900, h) == Error::ok);
    r.broker.maintain(900);
    r.backend.reset(2);
    r.broker.maintain(1000);
    assert(r.broker.read(2, temperature, false, 1000).sample.generation == 2);
    auto bad = r.broker.read(2, temperature, false, 1000).sample;
    ++bad.sampleId;
    bad.values[0].x = std::nan("");
    assert(r.broker.ingest(bad, 1000) == Error::invalid_argument);
    bad.values[0].x = 10;
    bad.generation = 1;
    assert(r.broker.ingest(bad, 1000) == Error::stale_handle);
    const auto old = h;
    r.broker.owner(2, 2);
    assert(r.broker.unsubscribe(old) == Error::stale_handle);
    assert(!r.broker.takeEvent(event, 1000));
    assert(r.broker.subscribe(2, temperature, 100, 1000, h) == Error::ok && h > old);
    assert(r.broker.systemDemand(2, temperature, 200) == Error::ok);
    assert(r.broker.systemDemand(2, temperature, 100, 1) == Error::ok);
    r.broker.owner(2, 2, false);
    r.broker.maintain(1200);
    assert(r.broker.read(2, temperature, false, 1200).error == Error::permission_denied);
    assert(r.broker.nextWake() == 1300);
    assert(r.broker.inspect(2, temperature, false, 1200).error == Error::ok);
    assert(r.broker.inspect(2, temperature, false, 1200).sample.values[1].x == 0);
    assert(r.broker.systemDemand(2, temperature, 0) == Error::ok);
    assert(r.broker.nextWake() != UINT64_MAX);
    assert(r.broker.systemDemand(2, temperature, 0, 1) == Error::ok);
    assert(r.broker.nextWake() == UINT64_MAX);
    r.broker.owner(3, 3);
    for (size_t i = 0; i < MaxSubscriptions; ++i)
        assert(r.broker.subscribe(2, temperature, 100, 0, h) == Error::ok);
    assert(r.broker.subscribe(2, temperature, 100, 0, h) == Error::quota_exceeded);
    r.broker.cancelAll();
    r.backend.status(2, Status::pending, Error::source_unavailable);
    for (size_t i = 0; i < MaxRefresh; ++i)
        assert(r.broker.refresh(2, temperature, 100, 2000, h) == Error::ok);
    assert(r.broker.refresh(2, temperature, 100, 2000, h) == Error::quota_exceeded);
    r.broker.maintain(2100);
    size_t events = 0;
    while (r.broker.takeEvent(event, 2100)) {
        ++events;
        assert(event.reading.error == Error::source_unavailable);
    }
    assert(events == MaxRefresh);
    Broker denied;
    denied.owner(1, 1);
    assert(denied.begin() == Error::ok);
    assert(denied.read(0, temperature, false, 0).error == Error::not_supported);
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
    assert(denied.read(0, temperature, false, 0).sample.source == 4);

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
    assert(arbitration.broker.refresh(2, temperature, 500, 400, first) == Error::ok);
    assert(arbitration.broker.refresh(2, temperature, 500, 400, second) == Error::ok);
    arbitration.broker.maintain(400);
    assert(!arbitration.broker.takeEvent(event, 400));
    arbitration.backend.status(2, Status::valid, Error::ok);
    arbitration.broker.maintain(450);
    assert(arbitration.broker.takeEvent(event, 450) && event.reading.error == Error::ok);
    const auto sharedId = event.reading.sample.sampleId;
    assert(arbitration.broker.takeEvent(event, 450) && event.reading.sample.sampleId == sharedId);
    assert(arbitration.broker.nextWake() == UINT64_MAX);
    std::array<Vector3, 6> invalid{};
    invalid[0].x = INFINITY;
    assert(arbitration.backend.setValues(2, invalid) == Error::invalid_argument);
    assert(arbitration.broker.refresh(2, temperature, 60001, 450, h) == Error::invalid_argument);
    arbitration.backend.status(2, Status::valid, Error::ok);
    assert(arbitration.broker.refresh(2, temperature, 500, 450, h) == Error::ok);
    const auto physical = arbitration.backend.sampleCount();
    arbitration.broker.maintain(450);
    assert(arbitration.backend.sampleCount() == physical && arbitration.broker.nextWake() == 550);
    arbitration.broker.maintain(550);
    assert(arbitration.broker.takeEvent(event, 550) && event.reading.simulation);
    assert(arbitration.backend.sampleCount() == physical + 1);
    // Unsupported channels resolve before application authorization.
    arbitration.allowed = false;
    const Mask pressure = bit(Channel::pressure);
    assert(arbitration.broker.read(0, pressure, false, 550).error == Error::not_supported);
    assert(arbitration.broker.subscribe(0, pressure, 100, 550, h) == Error::not_supported);
    assert(arbitration.broker.refresh(0, pressure, 100, 550, h) == Error::not_supported);
    assert(arbitration.broker.systemDemand(2, pressure, 100) == Error::not_supported);
    arbitration.allowed = true;
    arbitration.backend.status(2, Status::initializing, Error::source_unavailable);
    assert(arbitration.broker.refresh(2, temperature, 500, 650, h) == Error::ok);
    arbitration.broker.maintain(650);
    assert(
        arbitration.broker.read(2, temperature, false, 650).sample.status == Status::initializing);
    assert(!arbitration.broker.takeEvent(event, 650));
    arbitration.allowed = false;
    arbitration.broker.maintain(651);
    assert(arbitration.broker.cancel(h) == Error::stale_handle);
    assert(!arbitration.broker.takeEvent(event, 651));
    assert(arbitration.broker.nextWake() == UINT64_MAX);
    arbitration.allowed = true;
    arbitration.backend.status(2, Status::failed, Error::invalid_argument);
    assert(arbitration.broker.refresh(2, temperature, 500, 750, h) == Error::ok);
    arbitration.broker.maintain(750);
    assert(arbitration.broker.takeEvent(event, 750));
    assert(event.reading.sample.status == Status::failed &&
        event.reading.sample.reason == Error::invalid_argument);
    std::cout << "sizeof Broker=" << sizeof(Broker) << " Source=" << sizeof(Source)
              << " Sample=" << sizeof(Sample) << " Event=" << sizeof(Event)
              << " Simulation=" << sizeof(Simulation) << '\n';
}
