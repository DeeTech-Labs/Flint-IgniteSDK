#pragma once
#include "app_api.h"
#include "graphics/scene.h"
#include <memory>
#include <string_view>
namespace flint {
inline constexpr uint32_t MaxLuaSourceBytes = 16 * 1024;
inline constexpr uint64_t CapLua = 64;
struct LuaValue {
    char key[33]{};
    int32_t value{};
    std::array<char, MaxAppString + 1> text{};
    uint16_t length{};
    bool string{};
};
class LuaApp {
    friend class Runtime;
    friend class ServiceRuntime;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    LuaDiagnostic diagnostic_{};
    Error finish(int status, std::string_view phase);
    void resumeClock(uint64_t now);
    Error loadImpl(std::string_view, uint32_t, uint32_t, FlintRender::Scene *, AppHost);
    DispatchUsage lastUsage_{};

public:
    LuaApp();
    ~LuaApp();
    LuaApp(const LuaApp &) = delete;
    LuaApp &operator=(const LuaApp &) = delete;
    Error load(std::string_view source, uint32_t memory, uint32_t instructions,
        FlintRender::Scene &scene, AppHost host = {});
    Error loadService(
        std::string_view source, uint32_t memory, uint32_t instructions, AppHost host);
    Error beginTick(uint64_t now, DispatchBudget budget = {});
    void endTick();
    DispatchUsage usage() const;
    Error update(uint32_t elapsedMs, bool pressed);
    Error event(std::string_view kind, int32_t value, const InputEvent *input = nullptr);
    Error sensorEvent(const sensors::Event &event);
    Error ipcEvent(const IpcEvent &event);
    Error tick(uint64_t now);
    uint64_t nextWake() const;
    bool visible(unsigned index) const;
    const LuaDiagnostic &diagnostic() const {
        return diagnostic_;
    }
    void stop();
    bool active() const;
    bool visualChanged() const;
    static size_t nativeBytes();
    size_t used() const;
    size_t peak() const;
    std::span<const LuaValue> values() const;
};
} // namespace flint
