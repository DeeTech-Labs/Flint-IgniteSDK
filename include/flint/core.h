#pragma once
#include "diagnostics.h"
#include "protocol.h"
#include "runtime.h"
#include <optional>
namespace flint {
inline constexpr uint64_t CapDeclarative = 1, CapResources = 2, CapPackages = 4, CapMetrics = 8,
                          CapUsb = 16, CapBle = 32;
#ifdef FLINT_ENABLE_LUA
inline constexpr uint64_t HostLogicCapabilities = CapLua | CapAppApi;
#else
inline constexpr uint64_t HostLogicCapabilities = 0;
#endif
struct Platform {
    std::string build_id;
    // Stable package admissibility limit, independent of temporary heap pressure.
    MemoryBudget memory{48 * 1024, 0, 4096};
    std::function<MemoryBudget()> available_memory;
    uint64_t capabilities{CapDeclarative | CapResources | CapPackages | CapMetrics |
        HostLogicCapabilities | CapCompanionDemand | CapSessionClose};
    std::function<uint64_t()> monotonic;
    std::function<uint64_t()> free_storage;
    std::function<Values()> diagnostics;
    std::function<Bytes(std::string_view)> pairing;
    // Read-only power fields supplied by the device adapter; absent means unsupported.
    std::function<Error(std::string_view, AppReply &)> power;
    // Optional profiling clock in microseconds; independent of scheduling in milliseconds.
    std::function<uint64_t()> microseconds;
    bool require_factory{};
};
struct Session {
    bool authenticated{}, compatible{};
    uint64_t id{};
};
struct PermissionRecord {
    std::string package, permission, scope;
    uint64_t generation{}, expires{}, session{}, action{};
    bool enabled{};
};
class Core {
    Platform platform_;
    PackageManager packages_;
    Runtime runtime_;
    Values settings_, theme_, locale_;
    EventBus events_;
    std::vector<PermissionRecord> grants_;
    uint64_t epoch_{}, base_{}, lastSave_{};
    struct CompanionState {
        double cpu{}, ram{};
        uint64_t at{}, providerSession{}, consumerGeneration{};
        bool valid{};
    } companion_;
    struct CompanionSubscriber {
        uint64_t session{}, sentGeneration{}, sentAt{};
        bool sent{};
    };
    std::array<CompanionSubscriber, 4> companionSubscribers_{};
    uint64_t demandGeneration_{1}, demandRuntime_{~uint64_t(0)}, demandCatalog_{~uint64_t(0)},
        boundProvider_{}, boundGeneration_{}, sampleAt_{}, boundAt_{};
    CompanionDemand demand_{CompanionDemand::no_consumer};
    bool sampleReceived_{};
    void refreshCompanionDemand();
    Error handleCompanion(Method method, Reader &input, Session &session);
    Error lastLogicError_{Error::ok};
    LuaDiagnostic lastDiagnostic_{};
    std::string lastLogicPackage_, lastLogicComponent_;
    uint64_t privateWriteAt_{};
    bool privateWritten_{};
    uint64_t renderedMinute_{~uint64_t(0)};
    Error appCall(
        AppService service, std::string_view key, std::string_view value, AppReply &reply);
    Error activateRole(Role role, std::string_view selection = {});
    Error persistSettings();
    Error persistGrants();
    Error revokePackage(std::string_view id);
    void refreshProviders(std::string_view id, bool removed, PreparedInstall *prepared = nullptr);
    Principal metricPrincipal_{};
    uint64_t metricRuntime_{~uint64_t(0)}, metricCatalog_{~uint64_t(0)};
    bool metricIdentityValid_{}, metricFactory_{};
    std::string launcher_;
    uint64_t launcherCatalog_{~uint64_t(0)};
    bool canReceiveMetrics(uint64_t providerSession = 0);
    Error receiveMetrics(Reader &input, const Session &session);
    Values bindings_;
    uint64_t bindingsRuntime_{~uint64_t(0)}, bindingsMinute_{~uint64_t(0)},
        bindingsCatalog_{~uint64_t(0)};
    bool bindingsDirty_{true}, bindingsMetrics_{};
    Timing requestTiming_, logicTiming_, sceneTiming_, paintTiming_;
    uint64_t requests_{}, requestErrors_{}, metricRequests_{};
    std::unique_ptr<PreparedInstall> install_;
    Frame installRequest_;
    std::optional<Frame> installReply_;
    uint64_t installOwner_{}, installSteps_{};
    Timing installTiming_;
    Error beginInstall(const Frame &request, const Session &session);
    void advanceInstall();
    const Values &bindings();

public:
    Core(std::string storage, std::string factory, Platform platform);
    Error begin();
    Frame handle(const Frame &request, Session &session);
    // Owner-thread dispatch: a deferred install reply is delivered by takeReply().
    std::optional<Frame> process(const Frame &request, Session &session);
    bool takeReply(const Session &session, Frame &reply);
    bool replyReady(const Session &session) const;
    Error launch(std::string_view id, std::string_view component);
    Error scene(FlintRender::Scene &scene);
    void paint(const FlintRender::Scene &scene, FlintRender::Canvas &canvas);
    void disconnect(Session &session);
    bool takeCompanionEvent(const Session &session, Frame &event);
    uint64_t companionNextWake(const Session &session);
    PackageManager &packages() {
        return packages_;
    }
    Runtime &runtime() {
        return runtime_;
    }
    bool tick();
    Error postInput(InputEvent event) {
        return runtime_.postInput(event);
    }
    uint64_t nextWake() const;
    Error lastLogicError() const {
        return lastLogicError_;
    }
    uint64_t now() const;
    const Platform &platform() const {
        return platform_;
    }
};
} // namespace flint
