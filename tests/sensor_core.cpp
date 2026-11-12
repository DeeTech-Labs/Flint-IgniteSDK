#include <cstdlib>
#include <filesystem>
#include <flint/core.h>
#include <flint/sensor_simulation.h>
#include <fstream>
#include <iostream>

using namespace flint;
namespace fs = std::filesystem;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
static std::string value(Runtime &runtime, std::string_view key) {
    FlintRender::Scene scene;
    CHECK(runtime.render({}, scene) == Error::ok);
    const std::string text(reinterpret_cast<const char *>(scene.data), scene.used);
    const auto found = text.find(std::string(key) + "=");
    CHECK(found != std::string::npos);
    const auto begin = found + key.size() + 1;
    return text.substr(begin, text.find(';', begin) - begin);
}
static std::string bytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static void hardwareTests(const fs::path &root, const char *factory) {
    uint64_t at = 1000;
    sensors::Simulation backend;
    sensors::Source light;
    light.id = 2;
    light.channels = sensors::bit(sensors::Channel::ambient_light);
    std::array<sensors::Vector3, 6> lux{};
    lux[3].x = 100;
    CHECK(backend.add(light, lux) == Error::ok);
    sensors::Source temperature;
    temperature.id = 1;
    temperature.channels = 1;
    CHECK(backend.add(temperature) == Error::ok);
    backend.available(2, false);
    std::vector<unsigned> brightness;
    bool acknowledged = false;
    Error acknowledgement = Error::ok;
    unsigned rtcWrites{};
    bool failMute{}, muted{};
    Platform platform;
    platform.build_id = "sensor-hardware-test";
    platform.sensors = &backend;
    platform.memory = {144 * 1024, 0, 100000};
    platform.monotonic = [&] { return at; };
    platform.brightness_apply = [&](unsigned percent) {
        brightness.push_back(percent);
        return Error::ok;
    };
    platform.mute_apply = [&](bool value) {
        muted = value; // Fail closed for new beep requests even if stop acknowledgement fails.
        return failMute ? Error::io_error : Error::ok;
    };
    platform.rtc_read = [](uint64_t &) { return Error::source_unavailable; };
    platform.rtc_set = [&](uint64_t, uint64_t &token) {
        token = ++rtcWrites;
        return Error::ok;
    };
    platform.rtc_result = [&](uint64_t, bool &complete) {
        complete = acknowledged;
        return acknowledgement;
    };
    Core core((root / "hardware").string(), factory, platform);
    CHECK(core.begin() == Error::ok);
    Session session{true, true, 701};
    uint32_t sequence{};
    auto request = [&](Method method, Bytes payload = {}) {
        auto frame = core.handle({method, ++sequence, false, std::move(payload)}, session);
        Reader input{frame.payload};
        auto result = Error(input.n(2));
        CHECK(input.valid);
        return result;
    };
    auto setting = [&](std::string_view key, std::string_view value) {
        Writer w;
        w.text(key);
        w.text(value);
        CHECK(request(Method::settings_set, w.data) == Error::ok);
    };
    failMute = true;
    Writer mute;
    mute.text("audio_muted");
    mute.text("1");
    CHECK(request(Method::settings_set, mute.data) == Error::io_error && muted);
    const auto saved = core.handle({Method::settings_get, ++sequence, false, {}}, session);
    Reader settings{saved.payload};
    CHECK(settings.n(2) == 0);
    Values snapshot;
    const auto count = settings.n(2);
    for (unsigned i = 0; i < count; ++i) {
        const auto key = settings.text();
        snapshot[key] = settings.text();
    }
    CHECK(settings.done() && snapshot["audio_muted"] == "1");
    failMute = false;
    setting("audio_muted", "0");
    CHECK(!muted);
    auto step = [&](uint64_t delay = 1000) {
        at += delay;
        core.tick();
        core.tick();
    };
    auto grant = [&](bool enabled) {
        Writer w;
        w.text("test.sensors");
        w.text("ui.brightness");
        w.text("");
        w.n(enabled, 1);
        return request(Method::permission_set, w.data);
    };
    auto overrideBrightness = [&] {
        CHECK(core.postInput({InputKind::press, 5, core.runtime().generation()}) == Error::ok);
        core.tick();
        return value(core.runtime(), "result");
    };
    setting("brightness_manual", "30");
    step();
    CHECK(brightness.back() == 30);
    setting("brightness_fallback", "35");
    setting("brightness_mode", "automatic");
    step();
    CHECK(brightness.back() == 35 && backend.nextWake() != UINT64_MAX);
    backend.available(2, true);
    step();
    CHECK(brightness.back() == 40); // Slew limited by five percentage points per second.
    const auto calls = brightness.size();
    core.tick();
    CHECK(brightness.size() == calls);
    for (unsigned i = 0; i < 5; ++i)
        step();
    const auto stable = brightness.back();
    const auto stableCalls = brightness.size();
    lux[3].x = 101;
    CHECK(backend.setValues(2, lux) == Error::ok);
    step();
    CHECK(brightness.back() == stable && brightness.size() == stableCalls); // Hysteresis.
    fs::copy_file(
        root / "sensors.ignit", core.packages().staging(), fs::copy_options::overwrite_existing);
    std::string id;
    CHECK(core.packages().commit(core.packages().staging(), id) == Error::ok);
    CHECK(core.launch(id, "main") == Error::ok);
    CHECK(overrideBrightness() == "5");
    CHECK(grant(true) == Error::ok);
    CHECK(overrideBrightness() == "0");
    step();
    CHECK(brightness.back() == 90);
    CHECK(grant(false) == Error::ok);
    step();
    CHECK(brightness.back() < 90);
    CHECK(grant(true) == Error::ok);
    CHECK(overrideBrightness() == "0");
    step();
    CHECK(brightness.back() == 90);
    core.disconnect(session);
    session = {true, true, 702};
    CHECK(overrideBrightness() == "5");
    step();
    CHECK(brightness.back() < 90);
    setting("brightness_mode", "manual");
    step();
    const auto idleSamples = backend.sampleCount();
    step();
    CHECK(backend.sampleCount() == idleSamples && backend.nextWake() == UINT64_MAX);
    // Durable UTC acknowledgement is distinct from the eventual RTC write acknowledgement.
    Writer utc;
    utc.n(1793740000, 8);
    CHECK(request(Method::time_set, utc.data) == Error::ok && rtcWrites == 1);
    CHECK(core.now() == 1793740000 &&
        bytes(fs::path(core.packages().root()) / "epoch") == "1793740000");
    CHECK(request(Method::time_set, utc.data) == Error::invalid_state);
    const auto deadline = backend.nextWake();
    step(50);
    CHECK(backend.nextWake() == deadline);
    acknowledged = true;
    acknowledgement = Error::io_error;
    step(50);
    CHECK(request(Method::time_set, utc.data) == Error::ok && rtcWrites == 2);
    acknowledgement = Error::ok;
    step(50);
    // Session brightness grants are not durable across restart.
    CHECK(grant(true) == Error::ok);
    CHECK(core.runtime().stop() == Error::ok);
    Core reboot((root / "hardware").string(), factory, platform);
    CHECK(reboot.begin() == Error::ok && reboot.launch(id, "main") == Error::ok);
    CHECK(reboot.postInput({InputKind::press, 5, reboot.runtime().generation()}) == Error::ok);
    reboot.tick();
    CHECK(value(reboot.runtime(), "result") == "5");
}
int main(int argc, char **argv) {
    CHECK(argc == 3);
    const fs::path root = argv[1];
    uint64_t now = 1000;
    sensors::Simulation backend;
    sensors::Source source;
    source.id = 1;
    source.channels = 7;
    source.minIntervalMs = 100;
    std::array<sensors::Vector3, 6> values{};
    values[0].x = 23.5;
    values[1].x = 100000;
    values[2].x = 55;
    CHECK(backend.add(source, values) == Error::ok);
    Platform platform;
    platform.build_id = "sensor-core-tests";
    platform.monotonic = [&] { return now; };
    platform.memory = {144 * 1024, 0, 100000};
    platform.sensors = &backend;
    Core core((root / "store").string(), argv[2], platform);
    CHECK(core.begin() == Error::ok);
    Session session{true, true, 91};
    uint32_t sequence{};
    auto request = [&](Method method, Bytes payload = {}) {
        auto frame = core.handle({method, ++sequence, false, std::move(payload)}, session);
        Reader input{frame.payload};
        const auto result = Error(input.n(2));
        CHECK(input.valid);
        return result;
    };
    auto install = [&](std::string name) {
        fs::copy_file(root / (name + ".ignit"), core.packages().staging(),
            fs::copy_options::overwrite_existing);
        std::string id;
        const auto result = core.packages().commit(core.packages().staging(), id);
        if (result != Error::ok)
            std::cerr << "install " << name << " error " << unsigned(result) << '\n';
        CHECK(result == Error::ok);
        return id;
    };
    auto grant = [&](std::string_view id, bool enabled) {
        Writer w;
        w.text(id);
        w.text("sensors.temperature");
        w.text("");
        w.n(enabled, 1);
        return request(Method::permission_set, w.data);
    };
    auto press = [&](int command) {
        CHECK(
            core.postInput({InputKind::press, command, core.runtime().generation()}) == Error::ok);
        core.tick();
    };
    auto step = [&](uint64_t delay = 100) {
        now += delay;
        core.tick();
    };
    auto idle = [&] {
        const auto count = backend.sampleCount();
        step(1000);
        CHECK(backend.sampleCount() == count && backend.nextWake() == UINT64_MAX);
    };
    const auto id = install("sensors");
    const auto undeclared = install("undeclared");
    const auto crash = install("crash");
    const auto native = install("native");
    const auto instruction = install("instruction");
    CHECK(core.launch(id, "main") == Error::ok);
    CHECK(value(core.runtime(), "load") == "13");
    CHECK(value(core.runtime(), "loading_update") == "13");
    CHECK(backend.sampleCount() == 0); // Neither chunk nor update(0) can create hardware demand.
    press(1);
    CHECK(value(core.runtime(), "result") == "5");
    idle();
    CHECK(core.launch(undeclared, "main") == Error::ok);
    CHECK(grant(undeclared, true) == Error::permission_denied);
    press(1);
    CHECK(value(core.runtime(), "result") == "5");
    idle();
    CHECK(core.launch(id, "main") == Error::ok);
    CHECK(grant(id, true) == Error::ok);
    press(1);
    CHECK(value(core.runtime(), "result") == "0");
    step();
    CHECK(value(core.runtime(), "events") == "1" && value(core.runtime(), "filtered") == "1");
    // Removing a different installed package must not revoke this Runtime's subscriptions.
    const auto activeGeneration = core.runtime().generation();
    const auto eventsBeforeRemove = std::stoi(value(core.runtime(), "events"));
    Writer removeOther;
    removeOther.text(crash);
    CHECK(request(Method::remove, removeOther.data) == Error::ok);
    CHECK(core.runtime().manifest().id == id && core.runtime().generation() == activeGeneration);
    step();
    CHECK(std::stoi(value(core.runtime(), "events")) > eventsBeforeRemove);
    CHECK(backend.nextWake() != UINT64_MAX);
    CHECK(install("crash") == crash); // Restore the independent crash fixture used below.
    press(2);
    CHECK(value(core.runtime(), "result") == "0");
    press(3);
    CHECK(value(core.runtime(), "result") == "5");
    const auto grantsPath = fs::path(core.packages().root()) / "grants.bin";
    const auto epochPath = fs::path(core.packages().root()) / "epoch";
    const auto grantsBefore = bytes(grantsPath), epochBefore = bytes(epochPath);
    for (int i = 0; i < 10; ++i)
        step();
    CHECK(bytes(grantsPath) == grantsBefore && bytes(epochPath) == epochBefore);
    CHECK(grant(id, false) == Error::ok);
    const auto eventCount = value(core.runtime(), "events");
    press(2);
    CHECK(value(core.runtime(), "result") == "5");
    idle();
    CHECK(value(core.runtime(), "events") == eventCount);
    CHECK(grant(id, true) == Error::ok);
    press(1);
    step();
    fs::create_directory(grantsPath.string() + ".tmp");
    CHECK(grant(id, false) == Error::io_error);
    press(2);
    CHECK(value(core.runtime(), "result") == "5");
    idle();
    fs::remove(grantsPath.string() + ".tmp");
    CHECK(grant(id, true) == Error::ok);
    press(1);
    step();
    CHECK(core.runtime().suspend() == Error::ok);
    idle();
    CHECK(core.runtime().resume() == Error::ok);
    idle();
    press(4);
    CHECK(value(core.runtime(), "result") == "14");
    press(1);
    step();
    const auto oldHandle = std::stoi(value(core.runtime(), "handle"));
    CHECK(core.launch(undeclared, "main") == Error::ok);
    idle();
    press(-oldHandle);
    CHECK(value(core.runtime(), "result") == "14");

    // Diagnostic sampling is an explicit expiring lease, independent of app grants.
    Writer read;
    read.n(2, 1);
    CHECK(request(Method::sensor_diagnostic, read.data) == Error::permission_denied);
    auto diagnostic = [&](uint32_t lease) {
        Writer w;
        w.n(1, 1);
        w.n(1, 2);
        w.n(1, 1);
        w.n(200);
        w.n(lease);
        return request(Method::sensor_diagnostic, w.data);
    };
    CHECK(diagnostic(500) == Error::ok);
    step(200);
    CHECK(request(Method::sensor_diagnostic, read.data) == Error::ok);
    step(300);
    idle();
    CHECK(request(Method::sensor_diagnostic, read.data) == Error::permission_denied);
    CHECK(diagnostic(1000) == Error::ok);
    step(200);
    core.disconnect(session);
    idle();
    session = {true, true, 92};

    CHECK(core.launch(id, "main") == Error::ok);
    press(1);
    step();
    const auto sensorDeadline = backend.nextWake();
    Writer utc;
    utc.n(1793740000, 8);
    CHECK(request(Method::time_set, utc.data) == Error::ok);
    CHECK(backend.nextWake() == sensorDeadline);
    CHECK(core.launch(native, "main") == Error::ok);
    CHECK(grant(native, true) == Error::ok);
    press(1);
    step();
    CHECK(value(core.runtime(), "quota") == "0");
    CHECK(core.launch(native, "main") == Error::ok);
    press(2);
    step();
    CHECK(value(core.runtime(), "quota") == "1");
    CHECK(core.launch(crash, "main") == Error::ok);
    CHECK(grant(crash, true) == Error::ok);
    press(1);
    step();
    CHECK(core.lastLogicError() != Error::ok);
    idle();
    CHECK(core.launch(instruction, "main") == Error::ok);
    CHECK(grant(instruction, true) == Error::ok);
    press(1);
    step();
    CHECK(core.runtime().manifest().id == instruction &&
        value(core.runtime(), "result") == "8002000");
    CHECK(core.launch(instruction, "main") == Error::ok);
    press(2);
    step();
    CHECK(core.lastLogicError() == Error::execution_limit);
    idle();
    Runtime old;
    CHECK(old.launch({{}, (root / "old.ignit").string(), false}, "main", platform.memory,
              platform.capabilities) == Error::incompatible_contract);
    // An incompatible current object must not be silently replaced by its compatible backup
    // or garbage-collected after a firmware upgrade/downgrade.
    const auto preserved = root / "preserved";
    fs::create_directory(preserved);
    const auto oldObject = std::string(64, 'a') + ".ignit";
    const auto backupObject = std::string(64, 'b') + ".ignit";
    fs::copy_file(root / "old.ignit", preserved / oldObject);
    fs::copy_file(root / "sensors.ignit", preserved / backupObject);
    std::ofstream(preserved / "test.sensors.ref") << oldObject;
    std::ofstream(preserved / "test.sensors.ref.previous") << backupObject;
    std::ofstream(preserved / "app-test.sensors.kv") << "private-data";
    std::ofstream(preserved / "grants.bin") << "durable-grants";
    PackageManager retained(preserved.string(), argv[2], platform.memory, platform.capabilities);
    CHECK(retained.begin() == Error::ok);
    Installed incompatible;
    CHECK(retained.locate("test.sensors", incompatible) == Error::incompatible_contract);
    CHECK(bytes(preserved / "test.sensors.ref") == oldObject);
    CHECK(bytes(preserved / oldObject) == bytes(root / "old.ignit"));
    CHECK(fs::exists(preserved / backupObject));
    CHECK(bytes(preserved / "app-test.sensors.kv") == "private-data");
    CHECK(bytes(preserved / "grants.bin") == "durable-grants");
    hardwareTests(root, argv[2]);
    std::cout << "Sensor Core/Lua permissions, lifecycle, diagnostics and budgets passed\n";
}
