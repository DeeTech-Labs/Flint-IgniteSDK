#pragma once
#include "lua_app.h"
#include "permissions.h"
#include "resources.h"
#include "storage.h"
#include <map>
namespace flint {
struct Event {
    uint32_t type{};
    uint64_t value{}, generation{};
};
class EventBus {
    std::array<Event, 32> queue_{};
    size_t head_{}, size_{};

public:
    Error post(Event e) {
        if (size_ == queue_.size())
            return Error::quota_exceeded;
        queue_[(head_ + size_) % queue_.size()] = e;
        ++size_;
        return Error::ok;
    }
    bool poll(Event &e) {
        if (!size_)
            return false;
        e = queue_[head_];
        head_ = (head_ + 1) % queue_.size();
        --size_;
        return true;
    }
    bool empty() const {
        return !size_;
    }
    void clear() {
        head_ = size_ = 0;
    }
};
using Values = std::map<std::string, std::string, std::less<>>;
class Runtime {
    File file_;
    Package package_;
    Manifest manifest_;
    ComponentInfo component_;
    AppLifecycle lifecycle_;
    FlintRender::Scene template_;
    ResourceManager resources_;
    LuaApp lua_;
    uint64_t generation_{};
    AppHost host_{};
    uint64_t capabilities_{};
    bool loading_{}, focused_{true}, tickChanged_{}, pendingVisual_{};
    std::array<InputEvent, 16> inputs_{};
    size_t inputHead_{}, inputSize_{};
    Error appCall(
        AppService service, std::string_view key, std::string_view value, AppReply &reply);
    std::array<uint16_t, FlintRender::MaxData> bindingEnds_{};

public:
    ~Runtime() {
        stop();
    }
    Error launch(const Installed &installed, std::string_view component, MemoryBudget available,
        uint64_t caps);
    Error update(uint32_t elapsedMs, bool pressed = false);
    void host(AppHost host) {
        host_ = host;
    }
    Error postInput(InputEvent event);
    Error tick(uint64_t now);
    uint64_t nextWake() const {
        return state() != State::active      ? UINT64_MAX
            : (inputSize_ || pendingVisual_) ? 0
                                             : lua_.nextWake();
    }
    const LuaDiagnostic &diagnostic() const {
        return lua_.diagnostic();
    }
    bool visualChanged() const {
        return tickChanged_ || lua_.visualChanged();
    }
    bool scripted() const {
        return lua_.active();
    }
    size_t logicUsed() const {
        return lua_.used();
    }
    size_t logicPeak() const {
        return lua_.peak();
    }
    Error stop();
    Error suspend();
    Error resume();
    Error render(const Values &values, FlintRender::Scene &scene);
    const Manifest &manifest() const {
        return manifest_;
    }
    const ComponentInfo &component() const {
        return component_;
    }
    State state() const {
        return lifecycle_.state;
    }
    uint64_t packageIdentity() const {
        return package_.identity();
    }
    uint64_t generation() const {
        return generation_;
    }
    ResourceManager &resources() {
        return resources_;
    }
};
Error loadValues(const Installed &package, const ComponentInfo &component, Values &values);
} // namespace flint
