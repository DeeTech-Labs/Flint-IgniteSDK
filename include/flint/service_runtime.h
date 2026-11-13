#pragma once
#include "lua_app.h"
#include "storage.h"

namespace flint {
// Owner-thread-only headless execution. Host context and all callbacks outlive this object.
class ServiceRuntime {
public:
    ~ServiceRuntime();
    ServiceRuntime() = default;
    ServiceRuntime(const ServiceRuntime &) = delete;
    ServiceRuntime &operator=(const ServiceRuntime &) = delete;
    Error launch(const Installed &, std::string_view component, MemoryBudget available,
        uint64_t capabilities, InstanceKey, AppHost);
    Error tick(uint64_t now, DispatchBudget, DispatchUsage &);
    Error pause(ServiceReason reason = ServiceReason::energy);
    Error resume(uint64_t now);
    Error stop(ServiceReason reason = ServiceReason::user);
    uint64_t nextWake() const;
    ServiceState state() const {
        return state_;
    }
    const Manifest &manifest() const {
        return manifest_;
    }
    const ComponentInfo &component() const {
        return component_;
    }
    uint64_t packageIdentity() const {
        return packageIdentity_;
    }
    InstanceKey key() const {
        return key_;
    }
    size_t used() const {
        return lua_.used();
    }
    size_t peak() const {
        return std::max(lua_.peak(), peak_);
    }
    size_t nativeEstimate() const {
        return native_;
    }
    size_t reservation() const {
        return component_.budget.internal_bytes;
    }
    const LuaDiagnostic &diagnostic() const {
        return lua_.diagnostic();
    }

private:
    uint64_t packageIdentity_{};
    Manifest manifest_;
    ComponentInfo component_;
    LuaApp lua_;
    AppHost host_{};
    InstanceKey key_{};
    ServiceState state_{ServiceState::stopped};
    bool loading_{}, closing_{}, pendingStart_{}, pendingResume_{};
    size_t native_{}, peak_{};
    uint64_t now_{};
    Error call(AppService, std::string_view, std::string_view, AppReply &);
    Error sensor(const sensors::Request &, sensors::Reply &);
    Error ipc(const IpcRequest &, IpcReply &);
    Error permission(std::string_view);
    Error fail(Error);
    void clear();
    Error lifecycle(std::string_view, ServiceReason);
};
} // namespace flint
