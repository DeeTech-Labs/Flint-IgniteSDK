#include <algorithm>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <flint/lua_app.h>
#include <new>
#ifdef FLINT_ENABLE_LUA
extern "C" {
#include "lauxlib.h"
#include "lua.h"
}
#endif
namespace flint {
struct LuaApp::Impl {
    size_t used{}, peak{}, limit{};
    uint32_t instructions{}, remaining{}, nativeCalls{};
    uint32_t dispatchInstructions{}, nativeLimit{32}, wallCheck{};
    uint64_t dispatchStarted{}, deadline{}, nativeWall{}, maxNative{};
    Error error{Error::ok};
    FlintRender::Scene *scene{};
    std::array<LuaValue, 16> values{};
    size_t count{};
    bool changed{}, batch{}, loading{true};
    AppHost host{};
    LuaDiagnostic *diagnostic{};
    uint32_t hidden{};
    std::string_view pendingEvent;
    int32_t pendingValue{};
    InputEvent pendingInput{};
    const sensors::Event *pendingSensor{}; // Borrowed only for the protected synchronous callback.
    const IpcEvent *pendingIpc{};
    uint64_t clock{}, lastUpdate{}, nextUpdate{}, interval{33};
    struct Timer {
        uint64_t due{}, period{};
        bool active{};
    };
    std::array<Timer, 8> timers{};
    static uint64_t after(uint64_t now, uint64_t delay) {
        return delay > UINT64_MAX - now ? UINT64_MAX : now + delay;
    }
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
        if (!(++app.wallCheck & 63) && app.deadline && app.host.microseconds &&
            app.host.microseconds(app.host.context) >= app.deadline) {
            app.error = Error::execution_limit;
            luaL_error(state, "execution deadline");
        }
    }
    static bool nativeBegin(lua_State *state, uint64_t &started) {
        auto &app = self(state);
        if (app.nativeCalls >= app.nativeLimit) {
            if (app.host.kind == ExecutionKind::service) {
                app.error = Error::execution_limit;
                luaL_error(state, "native call limit");
            }
            return false;
        }
        ++app.nativeCalls;
        started = app.host.microseconds ? app.host.microseconds(app.host.context) : 0;
        if (app.deadline && started >= app.deadline) {
            app.error = Error::execution_limit;
            luaL_error(state, "execution deadline");
        }
        return true;
    }
    static void nativeEnd(lua_State *state, uint64_t started) {
        auto &app = self(state);
        const auto ended =
            app.host.microseconds ? app.host.microseconds(app.host.context) : started;
        const auto elapsed = ended >= started ? ended - started : 0;
        app.maxNative = std::max(app.maxNative, elapsed);
        if ((app.nativeWall && elapsed > app.nativeWall) ||
            (app.deadline && ended >= app.deadline)) {
            app.error = Error::execution_limit;
            luaL_error(state, "native execution deadline");
        }
    }
    static void arguments(lua_State *state, int expected) {
        if (lua_gettop(state) != expected)
            luaL_error(state, "invalid argument count");
    }
    static lua_Integer integer(lua_State *state, int index) {
        if (!lua_isinteger(state, index))
            luaL_error(state, "integer required");
        return lua_tointeger(state, index);
    }
    static std::string_view string(lua_State *state, int index) {
        if (lua_type(state, index) != LUA_TSTRING)
            luaL_error(state, "string required");
        size_t size{};
        const auto *data = lua_tolstring(state, index, &size);
        return {data, size};
    }
    static int status(lua_State *state, Error error) {
        lua_pushinteger(state, uint16_t(error));
        return 1;
    }
    static Error call(lua_State *state, AppService service, std::string_view key,
        std::string_view value, AppReply &reply) {
        auto &app = self(state);
        if (app.host.kind == ExecutionKind::service && app.loading)
            return Error::invalid_state;
        if (!app.host.enabled || !app.host.call)
            return Error::not_supported;
        uint64_t started{};
        if (!nativeBegin(state, started))
            return Error::quota_exceeded;
        const auto result = app.host.call(app.host.context, service, key, value, reply);
        nativeEnd(state, started);
        return result;
    }
    static int set(lua_State *state) {
        arguments(state, 3);
        auto &app = self(state);
        const auto index = integer(state, 1);
        const auto property = string(state, 2);
        const auto value = integer(state, 3);
        if (index < 2 || index > app.scene->count)
            return luaL_error(state, "invalid node");
        auto &op = app.scene->ops[index - 1];
        if (app.host.enabled && property == "visible") {
            if (value != 0 && value != 1)
                return luaL_error(state, "boolean integer required");
            const uint32_t mask = uint32_t(1) << (index - 1);
            const auto hidden = value ? app.hidden & ~mask : app.hidden | mask;
            app.changed |= hidden != app.hidden;
            app.hidden = hidden;
            return 0;
        }
        if (app.host.enabled && property == "color") {
            if (value < 0 || value > UINT16_MAX)
                return luaL_error(state, "color out of range");
            app.changed |= op.color != value;
            op.color = uint16_t(value);
            return 0;
        }
        if (app.host.enabled && property == "image" && op.kind == FlintRender::ResourceImage) {
            if (value < 1 || value > UINT16_MAX)
                return luaL_error(state, "invalid resource");
            AppReply reply;
            // The host validates ownership, format, dimensions and declared permission.
            char id[6]{};
            const auto size = std::snprintf(id, sizeof(id), "%u", unsigned(value));
            const auto error = call(state, AppService::image, {id, size_t(size)}, {}, reply);
            if (error != Error::ok)
                return status(state, error);
            app.changed |= op.extra != value;
            op.extra = uint16_t(value);
            op.w = int16_t(reply.number >> 16);
            op.h = int16_t(reply.number & 65535);
            return status(state, Error::ok);
        }
        if (app.host.enabled && property == "catalog" && op.kind == FlintRender::CatalogIcon) {
            if (value < 0 || value >= UINT16_MAX)
                return luaL_error(state, "catalog index out of range");
            app.changed |= op.extra != value + 1;
            op.extra = uint16_t(value + 1);
            return 0;
        }
        if (op.kind != FlintRender::Rect && op.kind != FlintRender::Rounded &&
            op.kind != FlintRender::Text && op.kind != FlintRender::CatalogIcon &&
            op.kind != FlintRender::ResourceImage)
            return luaL_error(state, "node kind is immutable");
        if (property == "x" || property == "y") {
            if (value < -320 || value > 640)
                return luaL_error(state, "position out of range");
            auto &target = property == "x" ? op.x : op.y;
            app.changed |= target != value;
            target = int16_t(value);
        } else if ((property == "w" || property == "h") &&
            (op.kind == FlintRender::Rect || op.kind == FlintRender::Rounded)) {
            if (value < 0 || value > 640)
                return luaL_error(state, "size out of range");
            auto &target = property == "w" ? op.w : op.h;
            app.changed |= target != value;
            target = int16_t(value);
        } else
            return luaL_error(state, "unsupported property");
        return 0;
    }
    static int publish(lua_State *state) {
        arguments(state, 2);
        auto &app = self(state);
        const auto key = string(state, 1);
        if (!appKey(key))
            return luaL_error(state, "invalid value key");
        const bool isString = lua_type(state, 2) == LUA_TSTRING && app.host.enabled;
        std::string_view text;
        int32_t value{};
        if (isString) {
            text = string(state, 2);
            if (text.size() > MaxAppString || !appText(text))
                return luaL_error(state, "invalid value text");
        } else {
            const auto n = integer(state, 2);
            if (n < INT32_MIN || n > INT32_MAX)
                return luaL_error(state, "value out of range");
            value = int32_t(n);
        }
        size_t index = 0;
        while (index < app.count && std::string_view(app.values[index].key) != key)
            ++index;
        if (index == app.count) {
            if (app.count == app.values.size())
                return luaL_error(state, "value quota");
            std::copy(key.begin(), key.end(), app.values[index].key);
            ++app.count;
            app.changed = true;
        }
        auto &entry = app.values[index];
        app.changed |= entry.string != isString || entry.value != value ||
            std::string_view(entry.text.data(), entry.length) != text;
        entry.string = isString;
        entry.value = value;
        entry.length = uint16_t(text.size());
        std::copy(text.begin(), text.end(), entry.text.begin());
        return 0;
    }
    static int read(lua_State *state) {
        const auto argc = lua_gettop(state);
        if (argc != 1 && argc != 2)
            return luaL_error(state, "invalid argument count");
        const auto name = string(state, 1);
        const auto key = argc == 2 ? string(state, 2) : std::string_view{};
        if (key.size() > 64 || !appText(key))
            return luaL_error(state, "invalid service key");
        AppService service;
        if (name == "time.read")
            service = AppService::time;
        else if (name == "time.monotonic")
            service = AppService::monotonic;
        else if (name == "system.info")
            service = AppService::info;
        else if (name == "system.power.status")
            service = AppService::power;
        else if (name == "system.locale.read")
            service = AppService::locale;
        else if (name == "system.theme.read")
            service = AppService::theme;
        else if (name == "system.settings.read")
            service = AppService::settings;
        else if (name == "packages.list")
            service = AppService::packages;
        else {
            lua_pushnil(state);
            lua_pushinteger(state, uint16_t(Error::not_supported));
            return 2;
        }
        AppReply reply;
        const auto error = call(state, service, key, {}, reply);
        if (error != Error::ok)
            lua_pushnil(state);
        else if (reply.numeric)
            lua_pushinteger(state, reply.number);
        else
            lua_pushlstring(state, reply.text.data(), reply.size);
        lua_pushinteger(state, uint16_t(error));
        return 2;
    }
    static int storage(lua_State *state, AppService service) {
        const bool reading = service == AppService::storage_get;
        const auto argc = lua_gettop(state);
        const bool numeric = reading && argc == 2;
        if (!numeric)
            arguments(state, service == AppService::storage_set ? 2 : 1);
        int32_t fallback{};
        if (numeric) {
            const auto n = integer(state, 2);
            if (n < INT32_MIN || n > INT32_MAX)
                return luaL_error(state, "default out of range");
            fallback = int32_t(n);
        }
        const auto key = string(state, 1);
        if (!appKey(key))
            return luaL_error(state, "invalid storage key");
        std::string_view value;
        std::array<char, 12> number{};
        if (service == AppService::storage_set) {
            if (lua_isinteger(state, 2)) {
                const auto n = integer(state, 2);
                if (n < INT32_MIN || n > INT32_MAX)
                    return luaL_error(state, "storage number out of range");
                const auto result = std::to_chars(number.data(), number.data() + number.size(), n);
                value = {number.data(), size_t(result.ptr - number.data())};
            } else
                value = string(state, 2);
        }
        if (value.size() > MaxAppString || !appText(value))
            return luaL_error(state, "invalid storage value");
        AppReply reply;
        auto error = call(state, service, key, value, reply);
        if (reading) {
            int32_t n{};
            if (numeric && error == Error::ok) {
                const auto result =
                    std::from_chars(reply.text.data(), reply.text.data() + reply.size, n);
                if (result.ec != std::errc{} || result.ptr != reply.text.data() + reply.size)
                    error = Error::invalid_argument;
            }
            if (numeric &&
                (error == Error::ok || error == Error::source_unavailable ||
                    error == Error::not_supported))
                lua_pushinteger(state, error == Error::ok ? n : fallback);
            else if (error == Error::ok)
                lua_pushlstring(state, reply.text.data(), reply.size);
            else
                lua_pushnil(state);
            lua_pushinteger(state, uint16_t(error));
            return 2;
        }
        return status(state, error);
    }
    static int get(lua_State *state) {
        return storage(state, AppService::storage_get);
    }
    static int put(lua_State *state) {
        return storage(state, AppService::storage_set);
    }
    static int erase(lua_State *state) {
        return storage(state, AppService::storage_delete);
    }
    static int catalog(lua_State *state) {
        arguments(state, 1);
        const auto index = integer(state, 1);
        if (index < 0 || index > 1000000)
            return luaL_error(state, "catalog index out of range");
        char key[16];
        const auto size = std::snprintf(key, sizeof(key), "row%u", unsigned(index));
        AppReply reply;
        auto error = call(state, AppService::packages, {key, size_t(size)}, {}, reply);
        const std::string_view record(reply.text.data(), reply.size);
        const auto middle = record.rfind('\n');
        if (error == Error::ok && middle == std::string_view::npos)
            error = Error::invalid_argument;
        if (error != Error::ok) {
            lua_pushnil(state);
            lua_pushnil(state);
        } else {
            lua_pushlstring(state, record.data(), middle);
            lua_pushlstring(state, record.data() + middle + 1, record.size() - middle - 1);
        }
        lua_pushinteger(state, unsigned(error));
        return 3;
    }
    static int action(lua_State *state) {
        arguments(state, 2);
        const auto key = string(state, 1), value = string(state, 2);
        if (key.size() > 32 || value.size() > 129 || !appText(key) || !appText(value))
            return status(state, Error::invalid_argument);
        AppReply reply;
        return status(state,
            call(state, key == "system" ? AppService::system_action : AppService::navigate, key,
                value, reply));
    }
    static int schedule(lua_State *state) {
        arguments(state, 1);
        const auto delay = integer(state, 1);
        if (delay < 0 || delay > 86400000 ||
            (delay && uint64_t(delay) < self(state).host.minTimerMs))
            return luaL_error(state, "invalid update interval");
        AppReply reply;
        const auto error = call(state, AppService::timer, {}, {}, reply);
        if (error != Error::ok)
            return status(state, error);
        auto &app = self(state);
        app.interval = uint64_t(delay);
        app.nextUpdate = delay ? after(app.clock, uint64_t(delay)) : UINT64_MAX;
        return status(state, Error::ok);
    }
    static int timer(lua_State *state) {
        arguments(state, 3);
        const auto id = integer(state, 1), delay = integer(state, 2);
        if (id < 1 || id > 8 || delay < 0 || delay > 86400000 ||
            (delay && uint64_t(delay) < self(state).host.minTimerMs) ||
            lua_type(state, 3) != LUA_TBOOLEAN)
            return luaL_error(state, "invalid timer");
        AppReply reply;
        const auto error = call(state, AppService::timer, {}, {}, reply);
        if (error != Error::ok)
            return status(state, error);
        auto &app = self(state);
        app.timers[id - 1] = {after(app.clock, uint64_t(delay)),
            lua_toboolean(state, 3) ? uint64_t(delay) : 0, delay != 0};
        return status(state, Error::ok);
    }
    static int log(lua_State *state) {
        arguments(state, 1);
        const auto value = string(state, 1);
        if (value.size() > 128 || !appText(value))
            return luaL_error(state, "invalid log text");
        auto &d = *self(state).diagnostic;
        if (d.logCount == d.log.size())
            return status(state, Error::quota_exceeded);
        std::copy(value.begin(), value.end(), d.log[d.logCount++].begin());
        return status(state, Error::ok);
    }
    static void field(lua_State *state, const char *key, uint64_t value) {
        lua_pushinteger(state, lua_Integer(value));
        lua_setfield(state, -2, key);
    }
    static void textField(lua_State *state, const char *key, std::string_view value) {
        lua_pushlstring(state, value.data(), value.size());
        lua_setfield(state, -2, key);
    }
    static void sensorRead(lua_State *state, const sensors::Read &reading) {
        const auto &sample = reading.sample;
        lua_createtable(state, 0, 10);
        field(state, "source_id", sample.source);
        field(state, "generation", sample.generation);
        field(state, "sample_id", sample.sampleId);
        field(state, "timestamp_ms", sample.at);
        field(state, "age_ms", reading.ageMs);
        field(state, "status", uint8_t(sample.status));
        field(state, "reason", uint16_t(sample.reason));
        field(state, "channels", sample.channels);
        lua_pushboolean(state, reading.lastKnown);
        lua_setfield(state, -2, "last_known");
        lua_pushboolean(state, reading.simulation);
        lua_setfield(state, -2, "simulation");
        // No numeric placeholders on pending/error reads. Last-known is explicit and tagged.
        if (reading.error == Error::ok || reading.lastKnown) {
            lua_createtable(state, 0, 6);
            for (unsigned i = 0; i < 6; ++i) {
                if (!(sample.channels & (1u << i)))
                    continue;
                const auto channel = sensors::Channel(i);
                const auto &v = sample.values[i];
                lua_createtable(state, 0, 4);
                lua_pushnumber(state, v.x);
                lua_setfield(state, -2, "x");
                if (i >= unsigned(sensors::Channel::acceleration)) {
                    lua_pushnumber(state, v.y);
                    lua_setfield(state, -2, "y");
                    lua_pushnumber(state, v.z);
                    lua_setfield(state, -2, "z");
                }
                textField(state, "unit", sensors::unit(channel));
                const auto name = sensors::name(channel);
                lua_setfield(state, -2, name.data());
            }
            lua_setfield(state, -2, "values");
        }
    }
    static sensors::SourceId sensorSource(lua_State *state, int index) {
        const auto value = integer(state, index);
        if (value < 0 || value > UINT16_MAX)
            luaL_error(state, "invalid source");
        return sensors::SourceId(value);
    }
    static sensors::Mask sensorChannels(lua_State *state, int index) {
        const auto value = integer(state, index);
        if (value <= 0 || value > sensors::AllChannels)
            luaL_error(state, "invalid channel mask");
        return sensors::Mask(value);
    }
    static Error sensorCall(
        lua_State *state, const sensors::Request &request, sensors::Reply &reply) {
        auto &app = self(state);
        if (app.host.kind == ExecutionKind::service && app.loading)
            return Error::invalid_state;
        if (!app.host.enabled || !app.host.sensor)
            return Error::not_supported;
        uint64_t started{};
        if (!nativeBegin(state, started))
            return Error::quota_exceeded;
        const auto result = app.host.sensor(app.host.context, request, reply);
        nativeEnd(state, started);
        return result;
    }
    static int sensorCatalog(lua_State *state) {
        arguments(state, 1);
        const auto index = integer(state, 1);
        if (index < 1 || uint64_t(index) > sensors::MaxSources)
            return luaL_error(state, "invalid catalog index");
        sensors::Request request;
        request.operation = sensors::Operation::catalog;
        request.index = uint8_t(index - 1);
        sensors::Reply reply;
        const auto error = sensorCall(state, request, reply);
        if (error == Error::ok) {
            const auto &source = reply.descriptor;
            lua_createtable(state, 0, 11);
            field(state, "source_id", source.id);
            field(state, "channels", source.channels);
            field(state, "min_interval_ms", source.minIntervalMs);
            field(state, "default_interval_ms", source.defaultIntervalMs);
            field(state, "max_interval_ms", source.maxIntervalMs);
            field(state, "stale_after_ms", source.staleAfterMs);
            field(state, "temperature_kind", uint8_t(source.temperature));
            field(state, "coordinates", uint8_t(source.coordinates));
            lua_pushnumber(state, source.accelerationRange);
            lua_setfield(state, -2, "acceleration_range_m_s2");
            lua_pushnumber(state, source.angularVelocityRange);
            lua_setfield(state, -2, "angular_velocity_range_rad_s");
            lua_pushboolean(state, source.mountingVerified);
            lua_setfield(state, -2, "mounting_verified");
            lua_pushboolean(state, source.simulation);
            lua_setfield(state, -2, "simulation");
        } else
            lua_pushnil(state);
        status(state, error);
        return 2;
    }
    static int sensorCached(lua_State *state) {
        arguments(state, 3);
        if (lua_type(state, 3) != LUA_TBOOLEAN)
            return luaL_error(state, "last_known boolean required");
        sensors::Request request;
        request.operation = sensors::Operation::read;
        request.source = sensorSource(state, 1);
        request.channels = sensorChannels(state, 2);
        request.lastKnown = lua_toboolean(state, 3);
        sensors::Reply reply;
        const auto error = sensorCall(state, request, reply);
        if (error == Error::ok || error == Error::source_unavailable)
            sensorRead(state, reply.reading);
        else
            lua_pushnil(state);
        status(state, error);
        return 2;
    }
    static int sensorDemand(lua_State *state, sensors::Operation operation) {
        arguments(state, 3);
        sensors::Request request;
        request.operation = operation;
        request.source = sensorSource(state, 1);
        request.channels = sensorChannels(state, 2);
        const auto value = integer(state, 3);
        if (value < 1 || value > 60000)
            return luaL_error(state, "invalid interval or timeout");
        request.intervalMs = request.timeoutMs = uint32_t(value);
        sensors::Reply reply;
        const auto error = sensorCall(state, request, reply);
        if (error == Error::ok)
            lua_pushinteger(state, lua_Integer(reply.handle));
        else
            lua_pushnil(state);
        status(state, error);
        return 2;
    }
    static int sensorRefresh(lua_State *state) {
        return sensorDemand(state, sensors::Operation::refresh);
    }
    static int sensorSubscribe(lua_State *state) {
        return sensorDemand(state, sensors::Operation::subscribe);
    }
    static int sensorUnsubscribe(lua_State *state) {
        arguments(state, 1);
        const auto handle = integer(state, 1);
        if (handle <= 0)
            return luaL_error(state, "invalid subscription handle");
        sensors::Request request;
        request.operation = sensors::Operation::unsubscribe;
        request.handle = uint64_t(handle);
        sensors::Reply reply;
        return status(state, sensorCall(state, request, reply));
    }
    static int output(lua_State *state, AppService service) {
        arguments(state, 2);
        const auto first = integer(state, 1), second = integer(state, 2);
        if (first < 0 || first > 10000 || second < 1 || second > 5000)
            return luaL_error(state, "invalid output range");
        std::array<char, 12> key{}, value{};
        const auto a = std::to_chars(key.data(), key.data() + key.size(), first);
        const auto b = std::to_chars(value.data(), value.data() + value.size(), second);
        AppReply reply;
        return status(state,
            call(state, service, {key.data(), size_t(a.ptr - key.data())},
                {value.data(), size_t(b.ptr - value.data())}, reply));
    }
    static int beep(lua_State *state) {
        return output(state, AppService::beep);
    }
    static int brightness(lua_State *state) {
        return output(state, AppService::brightness);
    }
    static int callSensor(lua_State *state) {
        lua_pushliteral(state, "sensor");
        lua_rawget(state, 1);
        if (lua_isnil(state, -1))
            return 0;
        if (!lua_isfunction(state, -1))
            return luaL_error(state, "sensor must be a function");
        const auto &event = *self(state).pendingSensor;
        lua_pushinteger(state, lua_Integer(event.handle));
        sensorRead(state, event.reading);
        status(state, event.reading.error);
        lua_pushinteger(state, event.drops);
        lua_pushboolean(state, event.refreshFinished);
        lua_call(state, 5, 0);
        return 0;
    }
    template <size_t N>
    static void boundedName(lua_State *state, int index, std::array<char, N> &output) {
        const auto text = string(state, index);
        if (text.empty() || text.size() >= N ||
            std::any_of(text.begin(), text.end(), [](unsigned char c) {
                return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-');
            }))
            luaL_error(state, "invalid IPC name");
        std::copy(text.begin(), text.end(), output.begin());
    }
    static IpcValue ipcValue(lua_State *state, int index) {
        IpcValue value;
        const auto type = lua_type(state, index);
        if (type == LUA_TBOOLEAN) {
            value.kind = IpcValueKind::boolean;
            value.integer = lua_toboolean(state, index);
        } else if (type == LUA_TNUMBER && lua_isinteger(state, index)) {
            value.kind = IpcValueKind::integer;
            value.integer = lua_tointeger(state, index);
        } else if (type == LUA_TNUMBER) {
            value.kind = IpcValueKind::number;
            value.number = lua_tonumber(state, index);
            if (!std::isfinite(value.number))
                luaL_error(state, "finite IPC number required");
        } else if (type == LUA_TSTRING) {
            const auto text = string(state, index);
            if (text.size() > 96 || text.find('\0') != std::string_view::npos || !appText(text))
                luaL_error(state, "IPC text exceeds 96 UTF-8 bytes");
            value.kind = IpcValueKind::text;
            value.size = uint8_t(text.size());
            std::copy(text.begin(), text.end(), value.text.begin());
        } else
            luaL_error(state, "IPC scalar value required");
        return value;
    }
    static void pushIpcValue(lua_State *state, const IpcValue &value) {
        switch (value.kind) {
        case IpcValueKind::boolean:
            lua_pushboolean(state, value.integer != 0);
            break;
        case IpcValueKind::integer:
            lua_pushinteger(state, lua_Integer(value.integer));
            break;
        case IpcValueKind::number:
            lua_pushnumber(state, value.number);
            break;
        case IpcValueKind::text:
            if (value.size > 96)
                luaL_error(state, "invalid IPC text length");
            lua_pushlstring(state, value.text.data(), value.size);
            break;
        default:
            luaL_error(state, "invalid IPC kind");
        }
    }
    static Error ipcCall(lua_State *state, const IpcRequest &request, IpcReply &reply) {
        auto &app = self(state);
        if (app.host.kind == ExecutionKind::service && app.loading)
            return Error::invalid_state;
        if (!app.host.enabled || !app.host.ipc)
            return Error::not_supported;
        uint64_t started{};
        if (!nativeBegin(state, started))
            return Error::quota_exceeded;
        const auto error = app.host.ipc(app.host.context, request, reply);
        nativeEnd(state, started);
        return error;
    }
    static int ipcSend(lua_State *state) {
        arguments(state, 3);
        IpcRequest request;
        IpcReply reply;
        request.operation = IpcOperation::send;
        boundedName(state, 1, request.target);
        boundedName(state, 2, request.key);
        request.value = ipcValue(state, 3);
        return status(state, ipcCall(state, request, reply));
    }
    static int stateRead(lua_State *state) {
        arguments(state, 1);
        IpcRequest request;
        IpcReply reply;
        request.operation = IpcOperation::read;
        boundedName(state, 1, request.key);
        const auto error = ipcCall(state, request, reply);
        if (error == Error::ok)
            pushIpcValue(state, reply.value);
        else
            lua_pushnil(state);
        lua_pushinteger(state, lua_Integer(reply.revision));
        status(state, error);
        return 3;
    }
    static int stateCommit(lua_State *state) {
        arguments(state, 2);
        IpcRequest request;
        IpcReply reply;
        request.operation = IpcOperation::commit;
        const auto revision = integer(state, 1);
        if (revision < 0 || lua_type(state, 2) != LUA_TTABLE)
            return luaL_error(state, "revision and writes table required");
        request.expectedRevision = uint64_t(revision);
        lua_pushnil(state);
        while (lua_next(state, 2)) {
            if (request.count == request.writes.size())
                return luaL_error(state, "at most eight state writes");
            auto &write = request.writes[request.count++];
            boundedName(state, -2, write.key);
            write.value = ipcValue(state, -1);
            lua_pop(state, 1);
        }
        if (!request.count)
            return luaL_error(state, "state writes required");
        const auto error = ipcCall(state, request, reply);
        lua_pushinteger(state, lua_Integer(reply.revision));
        status(state, error);
        return 2;
    }
    static int stateFlush(lua_State *state) {
        arguments(state, 0);
        IpcRequest request;
        IpcReply reply;
        request.operation = IpcOperation::flush;
        const auto error = ipcCall(state, request, reply);
        if (error == Error::ok)
            lua_pushinteger(state, lua_Integer(reply.ticket));
        else
            lua_pushnil(state);
        status(state, error);
        return 2;
    }
    static int callIpc(lua_State *state) {
        const auto &event = *self(state).pendingIpc;
        const char *handler = event.kind == IpcEventKind::message ? "message"
            : event.kind == IpcEventKind::state_changed           ? "state"
            : event.kind == IpcEventKind::persisted               ? "persisted"
                                                                  : "policy";
        lua_pushstring(state, handler);
        lua_rawget(state, 1);
        if (lua_isnil(state, -1))
            return 0;
        if (!lua_isfunction(state, -1))
            return luaL_error(state, "IPC handler must be a function");
        if (event.kind == IpcEventKind::message) {
            const auto component = std::find(event.component.begin(), event.component.end(), '\0');
            const auto topic = std::find(event.topic.begin(), event.topic.end(), '\0');
            lua_pushlstring(
                state, event.component.data(), size_t(component - event.component.begin()));
            lua_pushlstring(state, event.topic.data(), size_t(topic - event.topic.begin()));
            pushIpcValue(state, event.value);
            lua_call(state, 3, 0);
        } else if (event.kind == IpcEventKind::state_changed) {
            lua_pushinteger(state, lua_Integer(event.revision));
            lua_call(state, 1, 0);
        } else if (event.kind == IpcEventKind::persisted) {
            lua_pushinteger(state, lua_Integer(event.ticket));
            status(state, event.error);
            lua_call(state, 2, 0);
        } else {
            pushIpcValue(state, event.value);
            lua_call(state, 1, 0);
        }
        return 0;
    }
    static void registerShared(lua_State *state) {
        if (self(state).host.sensor) {
            lua_createtable(state, 0, 5);
            lua_pushcfunction(state, sensorCatalog);
            lua_setfield(state, -2, "catalog");
            lua_pushcfunction(state, sensorCached);
            lua_setfield(state, -2, "read");
            lua_pushcfunction(state, sensorRefresh);
            lua_setfield(state, -2, "refresh");
            lua_pushcfunction(state, sensorSubscribe);
            lua_setfield(state, -2, "subscribe");
            lua_pushcfunction(state, sensorUnsubscribe);
            lua_setfield(state, -2, "unsubscribe");
            lua_setfield(state, -2, "sensors");
        }
        if (self(state).host.ipc) {
            lua_createtable(state, 0, 1);
            lua_pushcfunction(state, ipcSend);
            lua_setfield(state, -2, "send");
            lua_setfield(state, -2, "ipc");
            lua_createtable(state, 0, 3);
            lua_pushcfunction(state, stateRead);
            lua_setfield(state, -2, "read");
            lua_pushcfunction(state, stateCommit);
            lua_setfield(state, -2, "commit");
            lua_pushcfunction(state, stateFlush);
            lua_setfield(state, -2, "flush");
            lua_setfield(state, -2, "state");
        }
    }
    static Error servicesCall(lua_State *state, const ServiceRequest &request, ServiceInfo &reply) {
        auto &app = self(state);
        if (!app.host.enabled || !app.host.services || app.host.kind != ExecutionKind::foreground)
            return Error::not_supported;
        uint64_t started{};
        if (!nativeBegin(state, started))
            return Error::quota_exceeded;
        const auto error = app.host.services(app.host.context, request, reply);
        nativeEnd(state, started);
        return error;
    }
    static int servicesList(lua_State *state) {
        arguments(state, 1);
        const auto index = integer(state, 1);
        if (index < 0 || index > 255)
            return luaL_error(state, "service index out of range");
        ServiceRequest request;
        request.operation = ServiceOperation::list;
        request.index = uint16_t(index);
        ServiceInfo reply;
        const auto error = servicesCall(state, request, reply);
        if (error != Error::ok)
            lua_pushnil(state);
        else {
            lua_createtable(state, 0, 18);
            auto text = [&](const char *key, const auto &value) {
                textField(state, key,
                    {value.data(),
                        size_t(std::find(value.begin(), value.end(), '\0') - value.begin())});
            };
            text("package", reply.package);
            text("component", reply.component);
            text("label", reply.label);
            field(state, "state", uint8_t(reply.state));
            field(state, "reason", uint8_t(reply.reason));
            field(state, "error", uint16_t(reply.error));
            field(state, "instance_id", reply.instance.id);
            field(state, "generation", reply.instance.generation);
            field(state, "used", reply.used);
            field(state, "peak", reply.peak);
            field(state, "reserved", reply.reserved);
            field(state, "callbacks", reply.callbacks);
            field(state, "instructions", reply.instructions);
            field(state, "native_calls", reply.nativeCalls);
            field(state, "max_callback_us", reply.maxCallbackUs);
            field(state, "wakeups", reply.wakeups);
            lua_pushboolean(state, reply.executeGranted);
            lua_setfield(state, -2, "execute_granted");
            lua_pushboolean(state, reply.autostartGranted);
            lua_setfield(state, -2, "autostart_granted");
            lua_pushboolean(state, reply.autostart);
            lua_setfield(state, -2, "autostart");
        }
        status(state, error);
        return 2;
    }
    static int servicesControl(lua_State *state) {
        arguments(state, 3);
        const auto action = integer(state, 1);
        if (action < 0 || action > 6)
            return luaL_error(state, "invalid service control action");
        ServiceRequest request;
        request.operation = ServiceOperation(action + 1);
        boundedName(state, 2, request.package);
        boundedName(state, 3, request.component);
        ServiceInfo reply;
        return status(state, servicesCall(state, request, reply));
    }
    static int environment(lua_State *state) {
        lua_createtable(state, 0, 10);
        if (self(state).host.kind == ExecutionKind::service) {
            lua_pushcfunction(state, read);
            lua_setfield(state, -2, "read");
            lua_pushcfunction(state, schedule);
            lua_setfield(state, -2, "schedule");
            lua_pushcfunction(state, timer);
            lua_setfield(state, -2, "timer");
            if (self(state).host.outputs & 1) {
                lua_pushcfunction(state, beep);
                lua_setfield(state, -2, "beep");
            }
            registerShared(state);
            lua_setglobal(state, "ignite");
            return 0;
        }
        lua_pushcfunction(state, set);
        lua_setfield(state, -2, "set");
        lua_pushcfunction(state, publish);
        lua_setfield(state, -2, "value");
        if (self(state).host.enabled) {
            lua_pushcfunction(state, read);
            lua_setfield(state, -2, "read");
            lua_pushcfunction(state, action);
            lua_setfield(state, -2, "action");
            lua_pushcfunction(state, catalog);
            lua_setfield(state, -2, "catalog");
            lua_pushcfunction(state, get);
            lua_setfield(state, -2, "storage_get");
            lua_pushcfunction(state, put);
            lua_setfield(state, -2, "storage_set");
            lua_pushcfunction(state, erase);
            lua_setfield(state, -2, "storage_delete");
            lua_pushcfunction(state, schedule);
            lua_setfield(state, -2, "schedule");
            lua_pushcfunction(state, timer);
            lua_setfield(state, -2, "timer");
            lua_pushcfunction(state, log);
            lua_setfield(state, -2, "log");
            if (self(state).host.outputs & 1) {
                lua_pushcfunction(state, beep);
                lua_setfield(state, -2, "beep");
            }
            if (self(state).host.outputs & 2) {
                lua_pushcfunction(state, brightness);
                lua_setfield(state, -2, "brightness");
            }
            registerShared(state);
            if (self(state).host.services) {
                lua_createtable(state, 0, 2);
                lua_pushcfunction(state, servicesList);
                lua_setfield(state, -2, "list");
                lua_pushcfunction(state, servicesControl);
                lua_setfield(state, -2, "control");
                lua_setfield(state, -2, "services");
            }
        }
        lua_setglobal(state, "ignite");
        return 0;
    }
    static int callEvent(lua_State *state) {
        lua_pushliteral(state, "event");
        lua_rawget(state, 1);
        if (lua_isnil(state, -1))
            return 0;
        if (!lua_isfunction(state, -1))
            return luaL_error(state, "event must be a function");
        const auto &app = self(state);
        lua_pushlstring(state, app.pendingEvent.data(), app.pendingEvent.size());
        lua_pushinteger(state, app.pendingValue);
        lua_pushinteger(state, app.pendingInput.x);
        lua_pushinteger(state, app.pendingInput.y);
        lua_pushinteger(state, app.pendingInput.pointer);
        lua_pushinteger(state, lua_Integer(app.pendingInput.at));
        lua_call(state, 6, 0);
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
bool LuaApp::visualChanged() const {
    return impl_ && impl_->changed;
}
size_t LuaApp::nativeBytes() {
    return sizeof(Impl);
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
Error LuaApp::load(std::string_view source, uint32_t memory, uint32_t instructions,
    FlintRender::Scene &scene, AppHost host) {
    host.kind = ExecutionKind::foreground;
    return loadImpl(source, memory, instructions, &scene, host);
}
Error LuaApp::loadService(
    std::string_view source, uint32_t memory, uint32_t instructions, AppHost host) {
    host.kind = ExecutionKind::service;
    return loadImpl(source, memory, instructions, nullptr, host);
}
Error LuaApp::loadImpl(std::string_view source, uint32_t memory, uint32_t instructions,
    FlintRender::Scene *scene, AppHost host) {
    stop();
    lastUsage_ = {};
    diagnostic_ = {};
    std::copy_n("load", 4, diagnostic_.phase.begin());
#ifdef FLINT_ENABLE_LUA
    if (source.empty() || source.size() > MaxLuaSourceBytes || source.front() == '\x1b' ||
        memory < 16 * 1024 || memory > 128 * 1024 || !instructions || instructions > 100000) {
        diagnostic_.error = Error::invalid_argument;
        std::copy_n("invalid Lua source or limits", 28, diagnostic_.message.begin());
        return Error::invalid_argument;
    }
    auto app = std::unique_ptr<Impl>(new (std::nothrow) Impl);
    if (!app) {
        diagnostic_.error = Error::resource_exhausted;
        return Error::resource_exhausted;
    }
    app->host = host;
    app->diagnostic = &diagnostic_;
    app->clock = app->lastUpdate = host.monotonic ? host.monotonic(host.context) : 0;
    app->interval = host.kind == ExecutionKind::service ? 0 : 33;
    app->nextUpdate = app->interval ? Impl::after(app->clock, app->interval) : UINT64_MAX;
    app->limit = memory;
    app->instructions = app->remaining = app->dispatchInstructions = instructions;
    app->scene = scene;
    app->state = lua_newstate(Impl::allocate, app.get());
    if (!app->state) {
        diagnostic_.error = Error::resource_exhausted;
        std::copy_n("Lua allocation failed", 21, diagnostic_.message.begin());
        return Error::resource_exhausted;
    }
    auto *state = app->state;
    *static_cast<Impl **>(lua_getextraspace(state)) = app.get();
    lua_sethook(state, Impl::hook, LUA_MASKCOUNT, 1);
    lua_pushcfunction(state, Impl::environment);
    int status = lua_pcall(state, 0, 0, 0);
    if (status == LUA_OK)
        status = luaL_loadbufferx(state, source.data(), source.size(), "ignite", "t");
    if (status == LUA_OK)
        status = lua_pcall(state, 0, 1, 0);
    impl_ = std::move(app);
    auto error = finish(status, "load");
    if (error != Error::ok)
        return error;
    if (!lua_istable(state, -1)) {
        diagnostic_.error = Error::invalid_argument;
        std::copy_n("application table required", 26, diagnostic_.message.begin());
        stop();
        return Error::invalid_argument;
    }
    if (host.kind == ExecutionKind::service) {
        impl_->loading = false;
        return Error::ok;
    }
    error = update(0, false);
    if (error != Error::ok)
        stop();
    else
        impl_->loading = false;
    return error;
#else
    (void)source;
    (void)memory;
    (void)instructions;
    (void)scene;
    (void)host;
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
    app.changed = false;
    if (!app.batch) {
        app.remaining = app.dispatchInstructions = app.instructions;
        app.error = Error::ok;
        app.nativeCalls = 0;
        app.nativeLimit = 32;
        app.deadline = app.nativeWall = app.maxNative = 0;
        app.dispatchStarted = app.host.microseconds ? app.host.microseconds(app.host.context) : 0;
    }
    auto *state = app.state;
    lua_pushcfunction(state, Impl::callUpdate);
    lua_pushvalue(state, 1);
    lua_pushinteger(state, elapsedMs);
    lua_pushboolean(state, pressed);
    return finish(lua_pcall(state, 3, 0, 0), "update");
#else
    (void)elapsedMs;
    (void)pressed;
    return Error::not_supported;
#endif
}
Error LuaApp::finish(int status, std::string_view phase) {
#ifdef FLINT_ENABLE_LUA
    auto &app = *impl_;
    diagnostic_.used = app.used;
    diagnostic_.peak = app.peak;
    diagnostic_.instructions = app.dispatchInstructions - app.remaining;
    lastUsage_ = usage();
    if (status == LUA_OK && app.deadline && app.host.microseconds &&
        app.host.microseconds(app.host.context) >= app.deadline) {
        app.error = Error::execution_limit;
        status = LUA_ERRRUN;
    }
    diagnostic_.phase.fill(0);
    std::copy_n(phase.begin(), std::min(phase.size(), diagnostic_.phase.size() - 1),
        diagnostic_.phase.begin());
    const auto error = app.result(status);
    if (error != Error::ok) {
        diagnostic_.error = error;
        if (lua_type(app.state, -1) == LUA_TSTRING) {
            size_t size{};
            const char *message = lua_tolstring(app.state, -1, &size);
            size = std::min(size, diagnostic_.message.size() - 1);
            while (size && !appText({message, size}))
                --size;
            if (size)
                std::copy_n(message, size, diagnostic_.message.begin());
            else
                std::copy_n("Lua execution failed", 20, diagnostic_.message.begin());
        }
        stop();
    }
    return error;
#else
    (void)status;
    (void)phase;
    return Error::not_supported;
#endif
}
Error LuaApp::event(std::string_view kind, int32_t value, const InputEvent *input) {
#ifdef FLINT_ENABLE_LUA
    if (!impl_)
        return Error::invalid_state;
    auto &app = *impl_;
    app.changed = false;
    if (!app.batch) {
        app.remaining = app.dispatchInstructions = app.instructions;
        app.error = Error::ok;
        app.nativeCalls = 0;
        app.nativeLimit = 32;
        app.deadline = app.nativeWall = app.maxNative = 0;
        app.dispatchStarted = app.host.microseconds ? app.host.microseconds(app.host.context) : 0;
    }
    auto *state = app.state;
    app.pendingEvent = kind;
    app.pendingValue = value;
    app.pendingInput = input ? *input : InputEvent{};
    lua_pushcfunction(state, Impl::callEvent);
    lua_pushvalue(state, 1);
    return finish(lua_pcall(state, 1, 0, 0), "event");
#else
    (void)kind;
    (void)value;
    (void)input;
    return Error::not_supported;
#endif
}
Error LuaApp::sensorEvent(const sensors::Event &event) {
#ifdef FLINT_ENABLE_LUA
    if (!impl_ || !impl_->batch)
        return Error::invalid_state;
    auto &app = *impl_;
    app.changed = false;
    app.pendingSensor = &event;
    lua_pushcfunction(app.state, Impl::callSensor);
    lua_pushvalue(app.state, 1);
    const auto result = lua_pcall(app.state, 1, 0, 0);
    app.pendingSensor = nullptr;
    return finish(result, "sensor");
#else
    (void)event;
    return Error::not_supported;
#endif
}
Error LuaApp::ipcEvent(const IpcEvent &event) {
#ifdef FLINT_ENABLE_LUA
    if (!impl_ || !impl_->batch)
        return Error::invalid_state;
    auto &app = *impl_;
    app.changed = false;
    app.pendingIpc = &event;
    lua_pushcfunction(app.state, Impl::callIpc);
    lua_pushvalue(app.state, 1);
    const auto result = lua_pcall(app.state, 1, 0, 0);
    app.pendingIpc = nullptr;
    return finish(result, "ipc");
#else
    (void)event;
    return Error::not_supported;
#endif
}
Error LuaApp::tick(uint64_t now) {
    if (!impl_)
        return Error::invalid_state;
    if (now < impl_->clock)
        return Error::invalid_argument;
    const bool ownBatch = !impl_->batch;
    if (ownBatch)
        beginTick(now);
    impl_->clock = now;
    bool changed = false;
    // Snapshot due timers: a callback cannot cause another delivery in this tick.
    std::array<bool, 8> due{};
    for (size_t i = 0; i < due.size(); ++i)
        due[i] = impl_->timers[i].active && impl_->timers[i].due <= now;
    for (size_t i = 0; i < due.size(); ++i) {
        auto &timer = impl_->timers[i];
        if (!due[i] || !timer.active || timer.due > now)
            continue;
        timer.active = timer.period != 0;
        timer.due = Impl::after(now, timer.period);
        const auto error = event("timer", int32_t(i + 1));
        if (error != Error::ok)
            return error;
        changed |= impl_->changed;
    }
    if (now >= impl_->nextUpdate && impl_->nextUpdate != UINT64_MAX) {
        const auto elapsed = uint32_t(std::min<uint64_t>(now - impl_->lastUpdate, 100));
        impl_->lastUpdate = now;
        impl_->nextUpdate = impl_->interval ? Impl::after(now, impl_->interval) : UINT64_MAX;
        const auto error = update(elapsed, false);
        if (error != Error::ok)
            return error;
        changed |= impl_->changed;
    }
    impl_->changed = changed;
    if (ownBatch)
        endTick();
    return Error::ok;
}
uint64_t LuaApp::nextWake() const {
    if (!impl_)
        return UINT64_MAX;
    auto next = impl_->nextUpdate;
    for (const auto &timer : impl_->timers)
        if (timer.active)
            next = std::min(next, timer.due);
    return next;
}
bool LuaApp::visible(unsigned index) const {
    return !impl_ || (index < 32 && !(impl_->hidden & (uint32_t(1) << index)));
}
Error LuaApp::beginTick(uint64_t now, DispatchBudget budget) {
    if (!impl_)
        return Error::invalid_state;
    if (now < impl_->clock)
        return Error::invalid_argument;
    impl_->clock = now;
    impl_->batch = true;
    impl_->remaining = impl_->dispatchInstructions = budget.instructions
        ? std::min(budget.instructions, impl_->instructions)
        : impl_->instructions;
    impl_->error = Error::ok;
    impl_->nativeCalls = 0;
    impl_->nativeLimit = budget.nativeCalls;
    impl_->deadline = budget.deadlineUs;
    impl_->nativeWall = budget.nativeWallUs;
    impl_->wallCheck = 0;
    impl_->maxNative = 0;
    impl_->dispatchStarted =
        impl_->host.microseconds ? impl_->host.microseconds(impl_->host.context) : 0;
    return Error::ok;
}
void LuaApp::endTick() {
    if (impl_) {
        lastUsage_ = usage();
        impl_->batch = false;
    }
}
DispatchUsage LuaApp::usage() const {
    if (!impl_)
        return lastUsage_;
    const auto &app = *impl_;
    const auto at =
        app.host.microseconds ? app.host.microseconds(app.host.context) : app.dispatchStarted;
    return {app.dispatchInstructions - app.remaining, app.nativeCalls,
        at >= app.dispatchStarted ? at - app.dispatchStarted : 0, app.maxNative};
}
void LuaApp::resumeClock(uint64_t now) {
    if (!impl_)
        return;
    impl_->clock = impl_->lastUpdate = now;
    impl_->nextUpdate = impl_->interval ? Impl::after(now, impl_->interval) : UINT64_MAX;
    impl_->timers = {};
}
} // namespace flint
