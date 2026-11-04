#include <algorithm>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <flint/lua_app.h>
#ifdef FLINT_ENABLE_LUA
extern "C" {
#include "lauxlib.h"
#include "lua.h"
}
#endif
namespace flint {
struct LuaApp::Impl {
    size_t used{}, peak{}, limit{};
    uint32_t instructions{}, remaining{};
    Error error{Error::ok};
    FlintRender::Scene *scene{};
    std::array<LuaValue, 16> values{};
    size_t count{};
#ifdef FLINT_ENABLE_LUA
    lua_State *state{};
    ~Impl() {
        if (state)
            lua_close(state);
    }
    static Impl &self(lua_State *state) {
        return **static_cast<Impl **>(lua_getextraspace(state));
    }
    static void *allocate(void *context, void *pointer, size_t old, size_t size) {
        auto &app = *static_cast<Impl *>(context);
        if (!pointer)
            old = 0; // Lua supplies a type tag for new allocations.
        if (!size) {
            app.used -= old;
            std::free(pointer);
            return nullptr;
        }
        if (size > app.limit - (app.used - old)) {
            app.error = Error::resource_exhausted;
            return nullptr;
        }
        auto *next = std::realloc(pointer, size);
        if (!next) {
            app.error = Error::resource_exhausted;
            return nullptr;
        }
        app.used = app.used - old + size;
        app.peak = std::max(app.peak, app.used);
        return next;
    }
    static void hook(lua_State *state, lua_Debug *) {
        auto &app = self(state);
        if (!app.remaining) {
            app.error = Error::execution_limit;
            luaL_error(state, "instruction limit");
        }
        --app.remaining;
    }
    static int set(lua_State *state) {
        auto &app = self(state);
        const auto index = luaL_checkinteger(state, 1);
        size_t length = 0;
        const char *property = luaL_checklstring(state, 2, &length);
        const auto value = luaL_checkinteger(state, 3);
        // Keep the full-screen clearing operation immutable.
        if (index < 2 || index > app.scene->count || length != 1)
            return luaL_error(state, "invalid node or property");
        auto &op = app.scene->ops[index - 1];
        if (op.kind != FlintRender::Rect && op.kind != FlintRender::Rounded &&
            op.kind != FlintRender::Text)
            return luaL_error(state, "node kind is immutable");
        if (*property == 'x' || *property == 'y') {
            if (value < -320 || value > 640)
                return luaL_error(state, "position out of range");
            if (*property == 'x')
                op.x = int16_t(value);
            else
                op.y = int16_t(value);
        } else if ((*property == 'w' || *property == 'h') && op.kind != FlintRender::Text) {
            if (value < 0 || value > 640)
                return luaL_error(state, "size out of range");
            if (*property == 'w')
                op.w = int16_t(value);
            else
                op.h = int16_t(value);
        } else
            return luaL_error(state, "unsupported property");
        return 0;
    }
    static int publish(lua_State *state) {
        auto &app = self(state);
        size_t length = 0;
        const char *key = luaL_checklstring(state, 1, &length);
        const auto value = luaL_checkinteger(state, 2);
        if (!length || length > 32 || value < INT32_MIN || value > INT32_MAX)
            return luaL_error(state, "invalid value");
        for (size_t i = 0; i < length; ++i)
            if (!((key[i] >= 'a' && key[i] <= 'z') || (key[i] >= '0' && key[i] <= '9') ||
                    key[i] == '_'))
                return luaL_error(state, "invalid value key");
        size_t index = 0;
        while (index < app.count && std::strcmp(app.values[index].key, key))
            ++index;
        if (index == app.count) {
            if (app.count == app.values.size())
                return luaL_error(state, "value quota");
            std::memcpy(app.values[index].key, key, length);
            ++app.count;
        }
        app.values[index].value = int32_t(value);
        return 0;
    }
    static int environment(lua_State *state) {
        lua_createtable(state, 0, 2);
        lua_pushcfunction(state, set);
        lua_setfield(state, -2, "set");
        lua_pushcfunction(state, publish);
        lua_setfield(state, -2, "value");
        lua_setglobal(state, "ignite");
        return 0;
    }
    static int callUpdate(lua_State *state) {
        // Raw access: user tables must not invoke a metamethod outside protected
        // calls.
        lua_pushliteral(state, "update");
        lua_rawget(state, 1);
        if (!lua_isfunction(state, -1))
            return luaL_error(state, "update function required");
        lua_pushvalue(state, 2);
        lua_pushvalue(state, 3);
        lua_call(state, 2, 0);
        return 0;
    }
    Error result(int status) {
        if (status == LUA_OK)
            return Error::ok;
        return error != Error::ok ? error : Error::invalid_argument;
    }
#endif
};
LuaApp::LuaApp() = default;
LuaApp::~LuaApp() = default;
void LuaApp::stop() {
    impl_.reset();
}
bool LuaApp::active() const {
    return bool(impl_);
}
size_t LuaApp::used() const {
    return impl_ ? impl_->used : 0;
}
size_t LuaApp::peak() const {
    return impl_ ? impl_->peak : 0;
}
std::span<const LuaValue> LuaApp::values() const {
    return impl_ ? std::span(impl_->values).first(impl_->count) : std::span<const LuaValue>{};
}
Error LuaApp::load(
    std::string_view source, uint32_t memory, uint32_t instructions, FlintRender::Scene &scene) {
    stop();
#ifdef FLINT_ENABLE_LUA
    if (source.empty() || source.size() > MaxLuaSourceBytes || source.front() == '\x1b' ||
        memory < 16 * 1024 || memory > 128 * 1024 || !instructions || instructions > 100000)
        return Error::invalid_argument;
    auto app = std::make_unique<Impl>();
    app->limit = memory;
    app->instructions = app->remaining = instructions;
    app->scene = &scene;
    app->state = lua_newstate(Impl::allocate, app.get());
    if (!app->state)
        return Error::resource_exhausted;
    auto *state = app->state;
    *static_cast<Impl **>(lua_getextraspace(state)) = app.get();
    lua_sethook(state, Impl::hook, LUA_MASKCOUNT, 1);
    lua_pushcfunction(state, Impl::environment);
    int status = lua_pcall(state, 0, 0, 0);
    if (status == LUA_OK)
        status = luaL_loadbufferx(state, source.data(), source.size(), "ignite", "t");
    if (status == LUA_OK)
        status = lua_pcall(state, 0, 1, 0);
    auto error = app->result(status);
    if (error != Error::ok)
        return error;
    if (!lua_istable(state, -1))
        return Error::invalid_argument;
    impl_ = std::move(app);
    error = update(0, false);
    if (error != Error::ok)
        stop();
    return error;
#else
    (void)source;
    (void)memory;
    (void)instructions;
    (void)scene;
    return Error::not_supported;
#endif
}
Error LuaApp::update(uint32_t elapsedMs, bool pressed) {
#ifdef FLINT_ENABLE_LUA
    if (!impl_)
        return Error::invalid_state;
    if (elapsedMs > 1000)
        return Error::invalid_argument;
    auto &app = *impl_;
    app.remaining = app.instructions;
    app.error = Error::ok;
    auto *state = app.state;
    lua_pushcfunction(state, Impl::callUpdate);
    lua_pushvalue(state, 1);
    lua_pushinteger(state, elapsedMs);
    lua_pushboolean(state, pressed);
    auto error = app.result(lua_pcall(state, 3, 0, 0));
    if (error != Error::ok)
        stop();
    return error;
#else
    (void)elapsedMs;
    (void)pressed;
    return Error::not_supported;
#endif
}
} // namespace flint
