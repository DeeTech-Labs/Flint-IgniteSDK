#include <flint/core.h>
#include <flint/sensor_simulation.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>

using namespace flint;
namespace fs = std::filesystem;
#define CHECK(x) do { if (!(x)) { std::cerr << __LINE__ << ": " #x "\n"; std::exit(1); } } while (false)
static std::string fileBytes(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
static int published(Core &core, std::string_view key) {
    FlintRender::Scene scene; CHECK(core.runtime().render({}, scene) == Error::ok);
    const std::string text(reinterpret_cast<const char *>(scene.data), scene.used);
    const auto found = text.find(std::string(key) + "="); CHECK(found != std::string::npos);
    const auto begin = found + key.size() + 1;
    return std::stoi(text.substr(begin, text.find(';', begin) - begin));
}
struct Harness {
    fs::path root;
    std::string factory;
    uint64_t now{1000}, us{};
    bool low{}, memoryLow{};
    sensors::Simulation backend;
    Platform platform;
    std::unique_ptr<Core> core;
    Session session{true, true, 100};
    uint32_t sequence{};
    Harness(fs::path path, std::string packages, uint32_t pool = 512 * 1024) : root(std::move(path)), factory(std::move(packages)) {
        fs::create_directories(root);
        sensors::Source source; source.id = 1; source.channels = 7; source.minIntervalMs = 100;
        std::array<sensors::Vector3, 6> values{}; values[0].x = 23.5; values[1].x = 101325; values[2].x = 45;
        CHECK(backend.add(source, values) == Error::ok);
        platform.build_id = "background-core-tests";
        platform.capabilities |= CapBackground | CapComponentIpc;
        platform.memory = {512 * 1024, 0, 100000};
        platform.available_memory = [&] { return platform.memory; };
        platform.monotonic = [&] { return now; };
        platform.microseconds = [&] { return now * 1000 + ++us; };
        platform.background.maxServices = 2; platform.background.memoryBytes = pool;
        platform.background.instructionsPerWindow = 16384; platform.background.nativeCallsPerWindow = 64;
        platform.background.windowMs = 1000; platform.background.quantumInstructions = 4096;
        platform.background.quantumNativeCalls = 16; platform.background.wallUsPerWindow = 1000000;
        platform.background.callbackWallUs = 100000; platform.background.nativeWallUs = 100000;
        platform.background_available_memory = [&] { return memoryLow ? MemoryBudget{1024, 0, 100000} : platform.memory; };
        platform.background_low_energy = [&] { return low; };
        platform.sensors = &backend;
        reboot();
    }
    void reboot() {
        core.reset(); core = std::make_unique<Core>((root / "store").string(), factory, platform);
        const auto error = core->begin();
        if (error != Error::ok) std::cerr << "Core begin " << root.string() << " error " << unsigned(error) << '\n';
        CHECK(error == Error::ok); session = {true, true, ++sequence};
    }
    Frame response(Method method, Bytes data = {}) { return core->handle({method, ++sequence, false, std::move(data)}, session); }
    Error request(Method method, Bytes data = {}) {
        auto frame = response(method, std::move(data)); Reader input{frame.payload};
        auto result = Error(input.n(2)); CHECK(input.valid); return result;
    }
    std::string install(std::string name) {
        fs::copy_file(root / (name + ".ignit"), core->packages().staging(), fs::copy_options::overwrite_existing);
        std::string id; CHECK(core->packages().commit(core->packages().staging(), id) == Error::ok); return id;
    }
    Error grant(std::string_view package, std::string_view permission, bool enabled = true) {
        Writer w; w.text(package); w.text(permission); w.text(""); w.n(enabled, 1);
        return request(Method::permission_set, w.data);
    }
    Error control(unsigned action, std::string_view package) {
        Writer w; w.n(action, 1); w.text(package); w.text("worker"); return request(Method::service_control, w.data);
    }
    void tick(unsigned count = 1, uint64_t delay = 0) { now += delay; for (unsigned i = 0; i < count; ++i) core->tick(); }
    void press(int command) {
        CHECK(core->postInput({InputKind::press, command, core->runtime().generation()}) == Error::ok);
        tick();
    }
    std::map<std::string, ServiceInfo> rows() {
        Writer w; w.n(0, 2); w.n(8, 1);
        const auto frame = response(Method::services, w.data); Reader r{frame.payload}; CHECK(Error(r.n(2)) == Error::ok);
        const auto count = r.n(1); CHECK(count <= 8); r.n(1); r.n(1);
        for (unsigned i = 0; i < 4; ++i) r.n();
        r.n(1);
        std::map<std::string, ServiceInfo> result;
        for (size_t i = 0; i < count; ++i) {
            ServiceInfo row; auto id = r.text(64); r.text(64); r.text(64);
            row.state = ServiceState(r.n(1)); row.reason = ServiceReason(r.n(1)); row.error = Error(r.n(2));
            row.instance = {r.n(8), r.n(8)}; row.used = uint32_t(r.n()); row.peak = uint32_t(r.n()); row.reserved = uint32_t(r.n());
            row.callbacks = r.n(8); row.instructions = r.n(8); row.nativeCalls = r.n(8); row.maxCallbackUs = r.n(8);
            row.wakeups = r.n(8); row.eventRejected = r.n(8); row.eventCoalesced = r.n(8);
            row.executeGranted = r.n(1); row.autostartGranted = r.n(1); row.autostart = r.n(1);
            result.emplace(std::move(id), row);
        }
        CHECK(r.done()); return result;
    }
    Error update(std::string_view fixture) {
        const auto data = fileBytes(root / (std::string(fixture) + ".ignit"));
        Sha256 sha; sha.update({reinterpret_cast<const uint8_t *>(data.data()), data.size()});
        std::string digest; constexpr char hex[] = "0123456789abcdef";
        for (auto byte : sha.finish()) { digest += hex[byte >> 4]; digest += hex[byte & 15]; }
        Writer begin; begin.n(data.size()); begin.text(digest); CHECK(request(Method::install_begin, begin.data) == Error::ok);
        for (size_t offset = 0; offset < data.size(); offset += 1024) {
            Writer chunk; chunk.n(offset); chunk.raw({reinterpret_cast<const uint8_t *>(data.data()) + offset, std::min<size_t>(1024, data.size() - offset)});
            CHECK(request(Method::install_chunk, chunk.data) == Error::ok);
        }
        return request(Method::install_commit);
    }
};
int main(int argc, char **argv) {
    CHECK(argc == 3); Harness h(argv[1], argv[2]);
    const auto a = h.install("a"), b = h.install("b");
    const auto busyA = h.install("busy-a"), busyB = h.install("busy-b");
    CHECK(h.core->launch(a, "main") == Error::ok);
    CHECK(h.core->startService(a, "worker") == Error::permission_denied);
    CHECK(h.grant(a, "app.background.execute") == Error::ok);
    CHECK(h.grant(a, "sensors.temperature") == Error::ok);
    CHECK(h.core->startService(a, "worker") == Error::ok);
    h.tick(3); h.tick(3, 100);
    CHECK(h.rows().at(a).state == ServiceState::running);
    CHECK(h.rows().at(a).used <= h.rows().at(a).reserved && h.rows().at(a).peak <= h.rows().at(a).reserved);
    h.press(1); CHECK(published(*h.core, "result") == 0 && published(*h.core, "read") > 0);
    const auto conversionDue = h.backend.nextWake();
    CHECK(conversionDue != UINT64_MAX && conversionDue > h.now && h.core->nextWake() <= conversionDue);
    h.press(22); CHECK(published(*h.core, "result") == 5); // Package grant cannot replace this UI's missing declaration.
    h.press(8); CHECK(published(*h.core, "result") == 14); // UI cannot cancel a service-owned sensor handle.
    h.press(2); CHECK(published(*h.core, "result") == 16); // CAS is a real shared-state conflict.
    h.press(14); CHECK(published(*h.core, "result") == 7);
    h.press(3); h.tick(3); CHECK(published(*h.core, "messages") > 0);
    const auto deliveredBeforeStop = published(*h.core, "messages");
    h.press(3); // Response is queued after the foreground dispatch in this Core pass.
    CHECK(h.core->stopService(a) == Error::ok); h.tick(3);
    CHECK(published(*h.core, "messages") == deliveredBeforeStop);
    CHECK(h.core->startService(a, "worker") == Error::ok); h.tick(3, 1000);
    h.press(6); CHECK(published(*h.core, "read") >= 4);
    CHECK(h.rows().at(a).eventRejected >= 4);
    h.tick(5, 1000);
    CHECK(h.grant(b, "app.background.execute") == Error::ok && h.grant(b, "sensors.temperature") == Error::ok);
    CHECK(h.core->startService(b, "worker") == Error::ok); h.tick(4);
    const auto generation = h.rows().at(a).instance;
    CHECK(h.core->launch(b, "main") == Error::ok); h.tick(4, 1000);
    CHECK(h.rows().at(a).instance == generation && h.rows().at(a).state == ServiceState::running);
    CHECK(h.core->runtime().stop() == Error::ok);
    const auto physical = h.backend.sampleCount(); h.tick(4, 100);
    CHECK(h.backend.sampleCount() == physical + 1); // Both services consume one physical collection.
    CHECK(h.rows().at(a).state == ServiceState::running && h.rows().at(b).state == ServiceState::running);
    h.low = true; h.tick(2);
    CHECK(h.rows().at(a).state == ServiceState::paused && h.rows().at(b).state == ServiceState::paused);
    const auto stoppedSamples = h.backend.sampleCount(); h.tick(4, 1000);
    CHECK(h.backend.sampleCount() == stoppedSamples);
    CHECK(h.grant(a, "app.background.execute", false) == Error::ok);
    CHECK(h.rows().at(a).state == ServiceState::stopped && !h.rows().at(a).reserved);
    h.low = false; h.tick(3); CHECK(h.rows().at(b).state == ServiceState::running);
    CHECK(h.grant(a, "app.background.execute") == Error::ok);
    CHECK(h.core->startService(a, "worker") == Error::ok); h.tick(4, 1000);
    CHECK(h.core->launch(a, "main") == Error::ok);
    h.press(15); CHECK(h.core->stopService(a) == Error::ok);
    CHECK(h.core->startService(a, "worker") == Error::ok); h.tick(3, 1000);
    h.press(16); h.tick(3); h.press(17); CHECK(published(*h.core, "read") == 14);
    const auto store = fs::path(h.core->packages().root());
    fs::create_directory(store / "grants.bin.tmp");
    CHECK(h.grant(a, "app.background.execute", false) == Error::io_error);
    CHECK(h.rows().at(a).state == ServiceState::stopped);
    CHECK(h.core->startService(a, "worker") == Error::permission_denied);
    h.press(3); CHECK(published(*h.core, "result") == 7); // Another package's live "worker" is not an IPC destination.
    fs::remove(store / "grants.bin.tmp");
    CHECK(h.grant(a, "app.background.execute") == Error::ok);
    h.memoryLow = true; CHECK(h.core->startService(a, "worker") == Error::resource_exhausted);
    CHECK(h.core->runtime().manifest().id == a && h.rows().at(b).state == ServiceState::running);
    h.memoryLow = false;
    // An invalid batch faults only its calling UI and cannot partially modify shared state.
    CHECK(h.core->startService(a, "worker") == Error::ok); h.tick(3, 1000);
    h.press(4); CHECK(h.core->lastLogicError() == Error::invalid_argument);
    CHECK(h.rows().at(a).state == ServiceState::running);
    CHECK(h.core->launch(a, "main") == Error::ok); h.press(5); CHECK(published(*h.core, "result") == 7);
    // Service faults are isolated, and a fault is not automatically retried.
    for (int command : {10, 11, 12}) {
        if (command == 10) {
            CHECK(h.grant(a, "app.background.autostart") == Error::ok);
            CHECK(h.control(4, a) == Error::ok);
        }
        h.tick(3, 1000); h.press(command); h.tick(4);
        CHECK(h.rows().at(a).state == ServiceState::faulted);
        CHECK(h.rows().at(a).error == (command == 12 ? Error::resource_exhausted : Error::execution_limit));
        CHECK(!h.rows().at(a).reserved && h.rows().at(b).state == ServiceState::running);
        CHECK(h.core->runtime().manifest().id == a && h.core->runtime().state() == State::active);
        const auto callbacks = h.rows().at(a).callbacks; h.tick(10, 1000);
        CHECK(h.rows().at(a).callbacks == callbacks);
        if (command == 10) {
            CHECK(!h.rows().at(a).autostart);
            h.reboot(); h.tick(5);
            CHECK(h.rows().at(a).state == ServiceState::stopped);
            CHECK(h.core->launch(a, "main") == Error::ok);
            CHECK(h.core->startService(b, "worker") == Error::ok);
        }
        CHECK(h.core->startService(a, "worker") == Error::ok);
    }
    CHECK(h.core->stopService(a) == Error::ok);
    CHECK(h.core->stopService(b) == Error::ok);
    CHECK(h.grant(a, "app.background.autostart", false) == Error::ok);
    // Always-ready services exercise the monotonic-window budget and rotating dispatch order.
    for (const auto &id : {busyA, busyB}) { CHECK(h.grant(id, "app.background.execute") == Error::ok); CHECK(h.core->startService(id, "worker") == Error::ok); }
    h.tick(100, 1000);
    const auto fair = h.rows(); const auto &fa = fair.at(busyA), &fb = fair.at(busyB);
    CHECK(fa.state == ServiceState::running && fb.state == ServiceState::running && fa.callbacks && fb.callbacks);
    CHECK(fa.instructions + fb.instructions <= h.platform.background.instructionsPerWindow);
    CHECK(fa.nativeCalls + fb.nativeCalls <= h.platform.background.nativeCallsPerWindow);
    CHECK(std::abs(int64_t(fa.callbacks) - int64_t(fb.callbacks)) <= 1);
    h.tick(100); CHECK(h.rows().at(busyA).callbacks == fa.callbacks && h.rows().at(busyB).callbacks == fb.callbacks);
    CHECK(h.core->nextWake() > h.now); // Exhausted owner events cannot create a busy loop.
    h.tick(4, 1000); CHECK(h.rows().at(busyA).callbacks > fa.callbacks && h.rows().at(busyB).callbacks > fb.callbacks);
    CHECK(h.core->stopService(busyA) == Error::ok && h.core->stopService(busyB) == Error::ok);
    // Autostart requires both grants and the explicit generation-bound preference.
    CHECK(h.control(4, a) == Error::permission_denied);
    CHECK(h.grant(a, "app.background.autostart") == Error::ok && h.control(4, a) == Error::ok);
    h.reboot(); h.tick(4); CHECK(h.rows().at(a).state == ServiceState::running);
    CHECK(h.core->launch(a, "main") == Error::ok); h.tick(3, 1000);
    const auto grants = fileBytes(store / "grants.bin"), epoch = fileBytes(store / "epoch");
    for (unsigned i = 0; i < 10; ++i) h.tick(3, 100);
    CHECK(fileBytes(store / "grants.bin") == grants && fileBytes(store / "epoch") == epoch);
    CHECK(!fs::exists(store / "shared-test.a.bin"));
    h.press(13); h.tick(4); h.press(9); CHECK(published(*h.core, "read") == 0);
    CHECK(fs::exists(store / "shared-test.a.bin"));
    const auto persisted = fileBytes(store / "shared-test.a.bin");
    h.press(13); h.tick(4); h.press(9); CHECK(published(*h.core, "read") == 9);
    CHECK(fileBytes(store / "shared-test.a.bin") == persisted);
    h.tick(3, 60000); h.press(13); h.tick(4); h.press(9); CHECK(published(*h.core, "read") == 0);
    CHECK(h.update("a-v2") == Error::ok);
    CHECK(h.rows().at(a).state == ServiceState::stopped);
    CHECK(h.core->startService(a, "worker") == Error::permission_denied);
    h.reboot(); h.tick(4); CHECK(h.rows().at(a).state == ServiceState::stopped);
    CHECK(h.grant(a, "app.background.execute") == Error::ok && h.grant(a, "sensors.temperature") == Error::ok);
    CHECK(h.grant(a, "app.background.autostart") == Error::ok);
    h.reboot(); h.tick(4); // Reapproval alone cannot resurrect an old package-generation preference.
    CHECK(h.rows().at(a).state == ServiceState::stopped);
    CHECK(h.core->startService(a, "worker") == Error::ok); h.tick(3, 1000);
    Writer remove; remove.text(a); CHECK(h.request(Method::remove, remove.data) == Error::ok);
    CHECK(!h.rows().contains(a));
    const auto example = h.install("example");
    for (const auto *permission : {"app.background.execute", "sensors.temperature", "sensors.humidity"})
        CHECK(h.grant(example, permission) == Error::ok);
    CHECK(h.core->startService(example, "worker") == Error::ok); h.tick(4, 1000);
    const auto exampleState = store / ("shared-" + example + ".bin");
    for (unsigned i = 0; i < 11; ++i) h.tick(4, 5000);
    CHECK(!fs::exists(exampleState));
    h.tick(4, 5000); CHECK(fs::exists(exampleState));
    const auto savedExample = fileBytes(exampleState);
    h.tick(4, 5000); CHECK(fileBytes(exampleState) == savedExample);
    CHECK(h.core->stopService(example) == Error::ok);
    // A failing lifecycle callback releases the instance and its handles on every pause path.
    for (const auto *fixture : {"pause-fault", "resume-fault"}) {
        const auto package = h.install(fixture);
        CHECK(h.grant(package, "app.background.execute") == Error::ok);
        CHECK(h.grant(package, "sensors.temperature") == Error::ok);
        CHECK(h.core->startService(package, "worker") == Error::ok); h.tick(4, 1000);
        if (std::string_view(fixture) == "pause-fault") {
            CHECK(h.control(2, package) == Error::execution_limit);
        } else {
            CHECK(h.control(2, package) == Error::ok);
            CHECK(h.control(3, package) == Error::ok); h.tick(4, 1000);
        }
        CHECK(h.rows().at(package).state == ServiceState::faulted);
        CHECK(!h.rows().at(package).reserved && !h.rows().at(package).autostart);
        const auto samples = h.backend.sampleCount(); h.tick(5, 1000);
        CHECK(h.backend.sampleCount() == samples && h.backend.nextWake() == UINT64_MAX);
        CHECK(h.core->startService(package, "worker") == Error::ok); h.tick(4, 1000);
        h.low = true; h.tick(2);
        if (std::string_view(fixture) == "pause-fault") {
            CHECK(h.rows().at(package).state == ServiceState::faulted && !h.rows().at(package).reserved);
        } else {
            CHECK(h.rows().at(package).state == ServiceState::paused);
        }
        h.low = false; h.tick(4, 1000);
        CHECK(h.rows().at(package).state == ServiceState::faulted && !h.rows().at(package).reserved);
    }
    const auto stopFault = h.install("stop-fault");
    CHECK(h.grant(stopFault, "app.background.execute") == Error::ok);
    CHECK(h.grant(stopFault, "sensors.temperature") == Error::ok);
    CHECK(h.grant(stopFault, "app.background.autostart") == Error::ok);
    CHECK(h.control(4, stopFault) == Error::ok);
    CHECK(h.core->startService(stopFault, "worker") == Error::ok); h.tick(4, 1000);
    CHECK(h.control(1, stopFault) == Error::execution_limit);
    CHECK(h.rows().at(stopFault).state == ServiceState::faulted);
    CHECK(!h.rows().at(stopFault).reserved && !h.rows().at(stopFault).autostart);
    const auto samplesAfterStopFault = h.backend.sampleCount(); h.tick(4, 1000);
    CHECK(h.backend.sampleCount() == samplesAfterStopFault);
    // The actual two example VMs fit the real model pool without changing their budgets.
    Harness model(h.root / "model-pool", argv[2], 131072);
    for (const auto *name : {"pool-a", "pool-b"}) {
        fs::copy_file(h.root / (std::string(name) + ".ignit"), model.root / (std::string(name) + ".ignit"), fs::copy_options::overwrite_existing);
        const auto package = model.install(name);
        for (const auto *permission : {"app.background.execute", "sensors.temperature", "sensors.humidity"})
            CHECK(model.grant(package, permission) == Error::ok);
        CHECK(model.core->startService(package, "worker") == Error::ok);
    }
    model.tick(5, 1000);
    const auto poolRows = model.rows();
    CHECK(poolRows.size() == 2);
    for (const auto &[id, row] : poolRows) {
        (void)id;
        CHECK(row.state == ServiceState::running && row.reserved == 49152);
        CHECK(row.used <= row.reserved && row.peak <= row.reserved && row.callbacks > 0);
    }
    std::cout << "Background Core permissions, ownership, lifecycle, budgets, IPC, persistence and updates passed\n";
}
