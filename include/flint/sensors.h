#pragma once
#include <array>
#include <flint/contracts.h>
#include <flint/sensor_registry.h>
#include <span>

namespace flint::sensors {
inline constexpr size_t MaxSources = 16, MaxSubscriptions = 8, MaxRefresh = 8;
inline constexpr uint64_t Capability = 8192;
using Mask = uint8_t;
using SourceId = uint16_t;
constexpr Mask bit(Channel c) {
    return unsigned(c) < 6 ? Mask(1u << unsigned(c)) : Mask(0);
}
inline constexpr Mask AllChannels = 63;
enum class TemperatureKind : uint8_t { unspecified, ambient, die };
enum class Coordinates : uint8_t { none, device };
enum class Status : uint8_t { absent, pending, valid, stale, failed, initializing };
struct Source {
    SourceId id{};
    Mask channels{};
    uint32_t minIntervalMs{100}, defaultIntervalMs{1000}, maxIntervalMs{60000}, staleAfterMs{5000};
    uint8_t priority{}; // Lower numbers win automatic selection.
    bool simulation{};
    TemperatureKind temperature{TemperatureKind::unspecified};
    Coordinates coordinates{Coordinates::none};
    // Symmetric full-scale SI ranges; zero means unspecified. Device axes are fixed,
    // sensor-aligned with an explicit identity mount until verified for a board revision.
    double accelerationRange{}, angularVelocityRange{};
    bool mountingVerified{};
};
struct Vector3 {
    double x{}, y{}, z{};
};
struct Sample {
    SourceId source{};
    Mask channels{};
    uint64_t generation{}, sampleId{}, at{}; // Monotonic conversion completion time, milliseconds.
    Status status{Status::pending};
    Error reason{
        Error::source_unavailable};  // Saturated/out-of-range input: failed + invalid_argument.
    std::array<Vector3, 6> values{}; // Scalars occupy x; unused components are zero.
};
struct Demand {
    SourceId source{};
    Mask channels{};
    uint32_t intervalMs{};          // Zero means no periodic demand.
    uint64_t refresh{}, deadline{}; // Nonzero token requests one new conversion.
    uint64_t
        notBefore{}; // Earliest conversion start, monotonic milliseconds; backend must honor it.
};
class Backend {
public:
    virtual ~Backend() = default;
    // All callbacks run on Core, must be bounded/nonblocking, and must never access hardware.
    virtual size_t catalog(std::span<Source>) = 0;
    virtual void demand(std::span<const Demand>) = 0;
    virtual size_t poll(uint64_t now, std::span<Sample>) = 0;
    virtual uint64_t nextWake() const = 0;
};
struct Read {
    Sample sample{};
    Error error{Error::source_unavailable};
    bool lastKnown{};
    uint64_t ageMs{};
    bool simulation{};
};
struct Event {
    uint64_t handle{};
    Read reading{};
    uint32_t drops{};
    bool refreshFinished{};
};
enum class Operation : uint8_t { catalog, read, refresh, subscribe, unsubscribe };
struct Request {
    Operation operation{};
    SourceId source{};
    Mask channels{};
    uint32_t intervalMs{}, timeoutMs{};
    uint64_t handle{};
    bool lastKnown{};
    uint8_t index{};
};
struct Reply {
    Read reading{};
    Source descriptor{};
    uint64_t handle{};
};
class Broker {
public:
    using Authorize = Error (*)(void *, Mask);
    void attach(Backend *backend);
    void authorization(Authorize callback, void *context) {
        authorize_ = callback;
        context_ = context;
    }
    void owner(uint64_t runtime, uint64_t consumer, bool active = true);
    void cancelAll();
    Error begin();
    std::span<const Source> catalog() const {
        return {sources_.data(), count_};
    }
    Read read(SourceId, Mask, bool lastKnown, uint64_t now) const;
    Read inspect(SourceId, Mask, bool lastKnown, uint64_t now) const;
    Error subscribe(SourceId, Mask, uint32_t intervalMs, uint64_t now, uint64_t &handle);
    Error unsubscribe(uint64_t handle);
    Error refresh(SourceId, Mask, uint64_t timeoutMs, uint64_t now, uint64_t &handle);
    Error cancel(uint64_t handle);
    // Trusted system demand bypasses application authorization; one slot per source.
    Error systemDemand(SourceId, Mask, uint32_t intervalMs, uint8_t consumer = 0);
    void maintain(uint64_t now);
    Error ingest(const Sample &, uint64_t now);
    bool takeEvent(Event &, uint64_t now);
    uint64_t nextWake() const;

private:
    struct Entry {
        uint64_t handle{}, deadline{}, after{}, generation{}, next{};
        SourceId source{};
        Mask channels{};
        uint32_t interval{}, drops{};
        bool pending{}, finished{}, expired{};
    };
    size_t index(SourceId, Mask) const;
    Error authorized(Mask) const;
    uint64_t allocate();
    void aggregate();
    void notify(Entry &, bool);
    Backend *backend_{};
    Authorize authorize_{};
    void *context_{};
    uint64_t runtime_{}, consumer_{}, serial_{};
    bool active_{};
    size_t count_{};
    std::array<Source, MaxSources> sources_{};
    std::array<Sample, MaxSources> cache_{}, good_{};
    std::array<uint64_t, MaxSources> cooldown_{};
    std::array<std::array<Demand, MaxSources>, 2> system_{};
    std::array<Demand, MaxSources> sent_{};
    std::array<Entry, MaxSubscriptions> subscriptions_{};
    std::array<Entry, MaxRefresh> refreshes_{};
};
} // namespace flint::sensors
