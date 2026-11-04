#pragma once
#include "contracts.h"
#include "graphics/scene.h"
#include <memory>
#include <string_view>
namespace flint {
inline constexpr uint32_t MaxLuaSourceBytes = 16 * 1024;
inline constexpr uint64_t CapLua = 64;
struct LuaValue {
    char key[33]{};
    int32_t value{};
};
class LuaApp {
    struct Impl;
    std::unique_ptr<Impl> impl_;

public:
    LuaApp();
    ~LuaApp();
    LuaApp(const LuaApp &) = delete;
    LuaApp &operator=(const LuaApp &) = delete;
    Error load(
        std::string_view source, uint32_t memory, uint32_t instructions, FlintRender::Scene &scene);
    Error update(uint32_t elapsedMs, bool pressed);
    void stop();
    bool active() const;
    size_t used() const;
    size_t peak() const;
    std::span<const LuaValue> values() const;
};
} // namespace flint
