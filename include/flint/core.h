#pragma once
#include "diagnostics.h"
#include "icons.h"
#include "protocol.h"
#include "runtime.h"
#include <optional>
namespace flint {
inline constexpr uint64_t CapDeclarative = 1, CapResources = 2, CapPackages = 4, CapMetrics = 8,
                          CapUsb = 16, CapBle = 32;
#ifdef FLINT_ENABLE_LUA
inline constexpr uint64_t HostLogicCapabilities =
    CapLua | CapAppApi | CapTouchEvents | CapInteractiveUi | sensors::Capability;
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
    // Non-owning, outlives Core. Methods only exchange bounded state, never wait for hardware.
    sensors::Backend *sensors{};
    std::function<Error(uint64_t &)> rtc_read;
    std::function<Error(uint64_t, uint64_t &)> rtc_set;
    std::function<Error(uint64_t, bool &)> rtc_result;
    std::function<Error(unsigned, unsigned)> beep;
    std::function<Error(bool)> mute_apply;
    std::function<Error(unsigned)> brightness_apply;
    std::function<uint64_t()> free_storage;
    std::function<Values()> diagnostics;
    std::function<Bytes(std::string_view)> pairing;
    // Read-only power fields supplied by the device adapter; absent means unsupported.
    std::function<Error(std::string_view, AppReply &)> power;
    // Optional profiling clock in microseconds; independent of scheduling in milliseconds.
    std::function<uint64_t()> microseconds;
    // Check is read-only; apply queues a hardware intent after durable settings commit.
    std::function<Error(bool)> ble_check;
    std::function<void(bool)> ble_apply;
    bool rename_open_files{true};
    bool require_factory{};
    // Device adapter sets this only for a physical power-on reset, never wake/restart.
    bool power_on{};
    // Device-owned package status presentation; the foreground application keeps running.
    bool system_ui{};
    // Optional owner-thread paint sink before a synchronous storage mutation.
    // May paint the supplied scene; must not mutate/reenter lifecycle or package operations.
    std::function<void(const FlintRender::Scene &)> present;
    struct PairingPrompt {
        int32_t code{-1};
        std::array<uint8_t, 6> address{};
        uint8_t address_type{};
    };
    std::function<PairingPrompt()> pairing_prompt;
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
    sensors::Broker sensors_;
    Runtime runtime_;
    Error authorizeApp(std::string_view name, std::string_view scope = {}, uint64_t session = 0);
    Error sensorCall(const sensors::Request &, sensors::Reply &);
    bool sensorEvent(sensors::Event &);
    void sensorOwner();
    struct SensorDiagnostic {
        uint64_t session{}, expires{}, next{}, lastSample{}, lastGeneration{};
        sensors::SourceId source{};
        sensors::Mask channels{};
        uint32_t interval{};
    } sensorDiagnostic_;
    void stopSensorDiagnostic();
    Error handleSensor(Method, Reader &, Writer &, const Session &);
    uint64_t rtcToken_{}, rtcDeadline_{}, rtcNext_{UINT64_MAX};
    Error rtcError_{Error::ok};
    uint8_t rtcState_{};
    bool rtcBootAccepted_{}, trustedTimeSet_{};
    void systemTick(uint64_t at);
    Error systemSetting(std::string_view key, std::string_view value);
    sensors::SourceId lightSource_{};
    double filteredLux_{};
    uint64_t lightSample_{}, lightGeneration_{};
    bool luxValid_{};
    unsigned brightness_{101}, brightnessOverride_{};
    uint64_t brightnessAt_{}, brightnessUntil_{}, brightnessOwner_{};
    uint64_t systemNext_{UINT64_MAX};
    Error brightnessError_{Error::ok};
    Error muteError_{Error::ok};
    IconCache icons_;
    Values settings_, theme_, locale_;
    EventBus events_;
    std::vector<PermissionRecord> grants_;
    uint64_t epoch_{}, base_{}, lastSave_{}, nextEpochSave_{UINT64_MAX};
    uint64_t epochWrites_{}, epochWriteErrors_{};
    uint32_t epochFailures_{};
    void checkpointEpoch();
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
    Error lastLogicError_{Error::ok}, lastBlePolicyError_{Error::ok};
    LuaDiagnostic lastDiagnostic_{};
    std::string lastLogicPackage_, lastLogicComponent_;
    uint64_t privateWriteAt_{};
    bool privateWritten_{};
    uint64_t renderedMinute_{~uint64_t(0)};
    uint64_t startupGeneration_{}, startupUntil_{UINT64_MAX};
    std::string pendingUiAction_, pendingUiValue_;
    uint64_t pendingUiGeneration_{}, notifiedCatalog_{UINT64_MAX};
    void applyUiAction();
    struct EdgeNavigation {
        Runtime preview;
        FlintRender::Scene source, target;
        uint64_t generation{}, at{}, sampleAt{}, started{}, nextFrame{}, integratedAt{};
        int64_t position{280000}, springVelocity{};
        int pointer{-1}, startX{}, origin{280}, offset{280}, from{280}, targetX{280}, lastX{},
            velocity{};
        bool ready{}, settling{}, commit{}, dirty{};
    };
    std::unique_ptr<EdgeNavigation> edge_;
    Error edgeInput(InputEvent event);
    bool edgeTick();
    Error edgeRender(FlintRender::Scene &scene);
    enum class OperationPhase {
        receiving,
        verifying,
        publishing,
        removing,
        complete,
        failed,
        interrupted
    };
    std::unique_ptr<Runtime> presentation_;
    OperationPhase operationPhase_{OperationPhase::complete};
    uint64_t operationOwner_{}, operationUntil_{UINT64_MAX}, operationPaintAt_{};
    uint32_t operationDone_{}, operationTotal_{};
    Error operationError_{Error::ok};
    bool operationRemoving_{}, operationDirty_{};
    bool notice_{};
    int16_t presentationX_{}, presentationY_{};
    int presentationPointer_{-1};
    bool transferCancelled_{};
    int32_t pairingShown_{-1};
    std::string noticeKey_, noticeSubject_, noticeDetail_, noticeScope_;
    bool presentation(std::string_view package, std::string_view component = "main");
    void notice(
        std::string_view key, std::string subject, std::string detail = {}, std::string scope = {});
    static std::string uiText(std::string text, int width = 224);
    void operationUpdate(
        OperationPhase phase, uint32_t done, uint32_t total, Error error = Error::ok);
    bool operationTick();
    Error operationRender(FlintRender::Scene &scene);
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
    bool installPermissionsRevoked_{};
    std::optional<Frame> installReply_;
    uint64_t installOwner_{}, installSteps_{};
    Timing installTiming_, installPublishTiming_;
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
    Error postInput(InputEvent event);
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
