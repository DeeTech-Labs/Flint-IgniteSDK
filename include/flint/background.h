#pragma once
#include "contracts.h"

namespace flint {
inline constexpr uint64_t CapBackground = 16384, CapComponentIpc = 32768;
inline constexpr size_t MaxBackgroundServices = 4;
enum class ServiceState : uint8_t { stopped, starting, running, paused, faulted };
enum class ServiceReason : uint8_t {
    none,
    user,
    permission,
    energy,
    resources,
    update,
    error,
    quota,
    shutdown
};
struct BackgroundPolicy {
    uint8_t maxServices{};
    uint32_t memoryBytes{}, foregroundReserveBytes{48 * 1024};
    uint32_t windowMs{100}, instructionsPerWindow{16384}, nativeCallsPerWindow{64};
    uint32_t quantumInstructions{4096}, quantumNativeCalls{16};
    uint32_t wallUsPerWindow{4000}, callbackWallUs{2000}, nativeWallUs{1000};
    uint32_t minTimerMs{100}, minSensorIntervalMs{100}, minFlushMs{60000};
};
struct DispatchBudget {
    uint32_t instructions{}, nativeCalls{32};
    uint64_t deadlineUs{};
    uint32_t nativeWallUs{};
};
struct DispatchUsage {
    uint32_t instructions{}, nativeCalls{};
    uint64_t elapsedUs{}, maxNativeUs{};
};
enum class IpcValueKind : uint8_t { boolean, integer, number, text };
struct IpcValue {
    IpcValueKind kind{};
    int64_t integer{};
    double number{};
    std::array<char, 97> text{};
    uint8_t size{};
};
struct StateWrite {
    std::array<char, 33> key{};
    IpcValue value{};
};
enum class IpcOperation : uint8_t { send, read, commit, flush };
struct IpcRequest {
    IpcOperation operation{};
    std::array<char, 65> target{};
    std::array<char, 33> key{};
    IpcValue value{};
    uint64_t expectedRevision{};
    std::array<StateWrite, 8> writes{};
    uint8_t count{};
};
struct IpcReply {
    IpcValue value{};
    uint64_t revision{}, ticket{};
};
enum class IpcEventKind : uint8_t { message, state_changed, persisted, policy };
struct IpcEvent {
    IpcEventKind kind{};
    InstanceKey sender{};
    std::array<char, 65> component{};
    std::array<char, 33> topic{};
    IpcValue value{};
    uint64_t revision{}, ticket{};
    Error error{Error::ok};
};
enum class ServiceOperation : uint8_t {
    list,
    start,
    stop,
    pause,
    resume,
    autostart_on,
    autostart_off,
    revoke_execute
};
struct ServiceRequest {
    ServiceOperation operation{};
    std::array<char, 65> package{}, component{};
    uint16_t index{};
};
struct ServiceInfo {
    std::array<char, 65> package{}, component{};
    std::array<char, 65> label{};
    ServiceState state{ServiceState::stopped};
    ServiceReason reason{ServiceReason::none};
    Error error{Error::ok};
    InstanceKey instance{};
    uint32_t used{}, peak{}, reserved{};
    uint64_t callbacks{}, instructions{}, nativeCalls{}, maxCallbackUs{}, wakeups{};
    uint64_t eventRejected{}, eventCoalesced{};
    bool executeGranted{}, autostartGranted{}, autostart{};
};
} // namespace flint
