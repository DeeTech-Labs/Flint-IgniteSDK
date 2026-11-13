#pragma once
#include <flint/core.h>
#include <flint/service_runtime.h>

namespace flint {
struct Core::BackgroundState {
    struct Inbox {
        InstanceKey owner{};
        std::array<IpcEvent, 8> events{};
        size_t count{};
        uint64_t rejected{}, coalesced{};
        Error push(const IpcEvent &event) {
            if (event.kind == IpcEventKind::state_changed || event.kind == IpcEventKind::policy)
                for (size_t i = 0; i < count; ++i)
                    if (events[i].kind == event.kind) {
                        events[i] = event;
                        ++coalesced;
                        return Error::ok;
                    }
            if (count == events.size()) {
                ++rejected;
                return Error::quota_exceeded;
            }
            events[count++] = event;
            return Error::ok;
        }
        bool pop(IpcEvent &event) {
            if (!count)
                return false;
            event = events[0];
            std::move(events.begin() + 1, events.begin() + count, events.begin());
            --count;
            return true;
        }
        void reset(InstanceKey key) {
            count = 0;
            owner = key;
        }
        void cancelFrom(InstanceKey sender) {
            auto end = std::remove_if(events.begin(), events.begin() + count, [&](const auto &e) {
                return e.kind == IpcEventKind::message && e.sender == sender;
            });
            count = size_t(end - events.begin());
        }
    };
    struct Record {
        ServiceInfo info;
        Principal principal;
        uint64_t packageGeneration{};
    };
    struct Slot {
        HostBinding host;
        std::unique_ptr<ServiceRuntime> runtime;
        Inbox inbox;
        size_t record{};
    };
    struct Shared {
        std::array<char, 65> package{};
        uint64_t publisher{}, revision{};
        std::array<StateWrite, 16> values{};
        size_t count{};
        Error loaded{Error::ok};
        bool pending{true};
    };
    std::array<Record, 16> records{};
    size_t recordCount{};
    std::array<Slot, MaxBackgroundServices> slots{};
    std::array<std::unique_ptr<Shared>, 16> shared{};
    Inbox foreground;
    uint64_t serial{}, windowAt{}, nextWindow{}, nextTicket{}, lastFlush{};
    uint32_t instructions{}, nativeCalls{};
    uint64_t wallUs{};
    size_t cursor{};
    bool low{}, flushed{}, autostartPending{true};
    struct Flush {
        InstanceKey owner;
        std::string package;
        Bytes data;
        uint64_t ticket{}, revision{};
    };
    std::unique_ptr<Flush> flush;
    size_t reserved() const {
        size_t bytes = sizeof(BackgroundState);
        for (const auto &slot : slots)
            if (slot.runtime)
                bytes += slot.runtime->reservation();
        for (const auto &state : shared)
            if (state)
                bytes += sizeof(Shared);
        if (flush)
            bytes += sizeof(Flush) + flush->data.capacity() + flush->package.capacity() + 1;
        return bytes;
    }
};
} // namespace flint
