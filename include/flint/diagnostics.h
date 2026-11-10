#pragma once
#include <algorithm>
#include <cstdint>
#include <functional>

namespace flint {
// Cumulative, owner-thread-only counters. No samples, allocations or background task.
struct Timing {
    uint64_t count{}, totalUs{}, maxUs{};
    void record(uint64_t elapsed) {
        ++count;
        totalUs += elapsed;
        maxUs = std::max(maxUs, elapsed);
    }
};
class TimingScope {
    const std::function<uint64_t()> &clock_;
    Timing &timing_;
    uint64_t start_;

public:
    TimingScope(const std::function<uint64_t()> &clock, Timing &timing)
        : clock_(clock), timing_(timing), start_(clock ? clock() : 0) {}
    ~TimingScope() {
        if (clock_) {
            const auto end = clock_();
            timing_.record(end >= start_ ? end - start_ : 0);
        }
    }
    TimingScope(const TimingScope &) = delete;
    TimingScope &operator=(const TimingScope &) = delete;
};
} // namespace flint
