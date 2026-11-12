#include <cstdlib>
#include <filesystem>
#include <flint/core.h>
#include <fstream>
#include <iostream>
using namespace flint;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
static std::string published(Runtime &runtime, std::string_view key) {
    FlintRender::Scene scene;
    CHECK(runtime.render({}, scene) == Error::ok);
    const std::string text(reinterpret_cast<const char *>(scene.data), scene.used);
    const auto start = text.find(std::string(key) + "=");
    CHECK(start != std::string::npos);
    const auto value = start + key.size() + 1;
    return text.substr(value, text.find(';', value) - value);
}
int main(int argc, char **argv) {
    CHECK(argc == 7);
    std::string root = argv[1];
    uint64_t at = 1000;
    Platform platform;
    platform.build_id = "app-api-tests";
    platform.monotonic = [&] { return at; };
    platform.memory = {144 * 1024, 0, 100000};
    Core core(root, argv[2], platform);
    CHECK(core.begin() == Error::ok);
    auto install = [&](const char *path) {
        std::filesystem::copy_file(
            path, core.packages().staging(), std::filesystem::copy_options::overwrite_existing);
        std::string id;
        CHECK(core.packages().commit(core.packages().staging(), id) == Error::ok);
        return id;
    };
    const auto id = install(argv[3]);
    const auto launched = core.launch(id, "main");
    if (launched != Error::ok)
        std::cerr << "launch error " << uint16_t(launched) << ": "
                  << core.runtime().diagnostic().message.data() << "\n";
    CHECK(launched == Error::ok);
    CHECK(published(core.runtime(), "counter") == "0");
    CHECK(published(core.runtime(), "text") == "Привет");
    FlintRender::Scene rendered;
    CHECK(core.scene(rendered) == Error::ok && rendered.count == 3);
    CHECK(rendered.ops[2].kind == FlintRender::ResourceImage && rendered.ops[2].extra == 101 &&
        rendered.ops[2].w == 2);
    CHECK(published(core.runtime(), "power") == "4"); // Adapter absent, not mock success.
    CHECK(core.runtime().nextWake() == at + 100);
    const auto initialGeneration = core.runtime().generation();
    CHECK(core.postInput({InputKind::press, 1, initialGeneration - 1}) == Error::stale_handle);
    CHECK(core.postInput({InputKind::press, 1, initialGeneration}) == Error::ok);
    CHECK(core.nextWake() <= at);
    CHECK(core.tick());
    CHECK(published(core.runtime(), "counter") == "1");
    CHECK(published(core.runtime(), "save") == "0");
    CHECK(core.postInput({InputKind::press, 1, initialGeneration}) == Error::ok);
    core.tick();
    CHECK(published(core.runtime(), "counter") == "2");
    CHECK(published(core.runtime(), "save") == "9"); // Global write throttle.
    at += 100;
    core.tick();
    CHECK(published(core.runtime(), "timer") == "1");
    CHECK(core.runtime().nextWake() == UINT64_MAX);
    CHECK(core.postInput({InputKind::focus_lost, 0, initialGeneration}) == Error::ok);
    CHECK(core.postInput({InputKind::press, 1, initialGeneration}) == Error::ok);
    core.tick();
    CHECK(published(core.runtime(), "counter") == "2");
    CHECK(core.postInput({InputKind::focus_gained, 0, initialGeneration}) == Error::ok);
    core.tick();
    for (unsigned i = 0; i < 16; ++i)
        CHECK(core.postInput({InputKind::release, 0, initialGeneration}) == Error::ok);
    CHECK(core.postInput({InputKind::release, 0, initialGeneration}) == Error::quota_exceeded);
    core.tick();
    CHECK(published(core.runtime(), "power") == "1100");
    CHECK(core.runtime().suspend() == Error::ok);
    CHECK(core.postInput({InputKind::press, 1, initialGeneration}) == Error::invalid_state);
    at += 5000;
    CHECK(!core.tick());
    CHECK(core.runtime().resume() == Error::ok);
    CHECK(core.launch(id, "main") == Error::ok);
    CHECK(published(core.runtime(), "counter") == "1"); // Only successful write persisted.
    CHECK(core.postInput({InputKind::press, 1, initialGeneration}) == Error::stale_handle);
    CHECK(core.runtime().suspend() == Error::ok);
    at += 5000;
    CHECK(core.runtime().resume() == Error::ok);
    core.tick();
    CHECK(published(core.runtime(), "timer") == "0"); // Timers cancelled on resume.
    const auto denied = install(argv[4]);
    CHECK(core.launch(denied, "main") == Error::ok);
    CHECK(published(core.runtime(), "denied") == "5");
    CHECK(core.postInput({InputKind::press, 1, core.runtime().generation()}) ==
        Error::permission_denied);
    // The new capability is checked before Lua can run on an older host.
    Runtime limited;
    CHECK(limited.launch({{}, argv[3], false}, "main", platform.memory,
              platform.capabilities & ~CapAppApi) == Error::not_supported);
    Runtime noHost;
    CHECK(noHost.launch({{}, argv[3], false}, "main", platform.memory, platform.capabilities) ==
        Error::ok);
    CHECK(published(noHost, "power") == "4");
    const auto fault = install(argv[5]);
    CHECK(core.launch(fault, "main") == Error::ok);
    CHECK(core.postInput({InputKind::press, 1, core.runtime().generation()}) == Error::ok);
    core.tick();
    CHECK(core.lastLogicError() == Error::execution_limit);
    CHECK(core.runtime().manifest().id == "system.clock");
    Session session{true, true, 3};
    const auto frame = core.handle({Method::diagnostics, 1, false, {}}, session);
    Reader reader{frame.payload};
    CHECK(reader.n(2) == 0);
    Values diagnostic;
    const auto count = reader.n(2);
    for (unsigned i = 0; i < count; ++i) {
        auto k = reader.text(64), v = reader.text(192);
        diagnostic[k] = v;
    }
    CHECK(reader.done());
    CHECK(diagnostic["logic_package"] == fault && diagnostic["logic_phase"] == "event");
    CHECK(diagnostic["logic_message"].find("instruction limit") != std::string::npos);
    CHECK(!diagnostic.contains("logic_log_0")); // Logs need an explicit diagnostic lease.
    CHECK(core.launch(id, "main") == Error::ok);
    CHECK(core.packages().remove(id) == Error::ok);
    CHECK(!std::filesystem::exists(root + "/app-" + id + ".kv"));
    // Bounded private storage: namespace separation, corruption and failed replacement.
    Manifest manifest;
    manifest.id = "test.storage";
    manifest.publisher = "tests";
    AppReply reply;
    for (unsigned i = 0; i < 16; ++i)
        CHECK(privateStorage(root, manifest, AppService::storage_set, "k" + std::to_string(i),
                  "value", reply) == Error::ok);
    CHECK(privateStorage(root, manifest, AppService::storage_set, "overflow", "x", reply) ==
        Error::quota_exceeded);
    CHECK(privateStorage(root, manifest, AppService::storage_get, "../other", {}, reply) ==
        Error::invalid_argument);
    manifest.publisher = "other";
    CHECK(privateStorage(root, manifest, AppService::storage_get, "k0", {}, reply) ==
        Error::permission_denied);
    manifest.publisher = "tests";
    manifest.id = "test.other";
    CHECK(privateStorage(root, manifest, AppService::storage_get, "k0", {}, reply) ==
        Error::source_unavailable);
    manifest.id = "test.storage";
    const auto dataPath = root + "/app-test.storage.kv";
    const auto previous = readText(dataPath, 4096);
    std::filesystem::create_directory(dataPath + ".tmp");
    CHECK(privateStorage(root, manifest, AppService::storage_set, "k0", "changed", reply) ==
        Error::io_error);
    CHECK(readText(dataPath, 4096) == previous);
    std::filesystem::remove(dataPath + ".tmp");
    auto broken = previous;
    broken.back() ^= 1;
    CHECK(writeAtomic(dataPath, {reinterpret_cast<const uint8_t *>(broken.data()), broken.size()}));
    CHECK(privateStorage(root, manifest, AppService::storage_get, "k0", {}, reply) ==
        Error::integrity_failed);
    // Native Lua boundary validation and bounded logs, no standard libraries enabled.
    FlintRender::Scene scene;
    FlintRender::rect(scene, 0, 0, 280, 240, 0);
    FlintRender::rect(scene, 1, 1, 10, 10, 0);
    LuaApp app;
    AppHost host{nullptr,
        [](void *, AppService service, std::string_view, std::string_view, AppReply &) {
            return service == AppService::timer ? Error::ok : Error::not_supported;
        },
        nullptr, true};
    for (const auto *source : {
             "return {update=function() ignite.value('x', 'a'..'b', 1) end}",
             "return {update=function() ignite.set(2,'x','1') end}",
             "return {update=function() ignite.set(1,'visible',0) end}",
             "return {update=function() ignite.timer(9,1,false) end}",
             "return {update=function() ignite.timer(1,1,1) end}",
             "return {update=function() ignite.storage_get('../escape') end}",
             "return {update=function() ignite.read(1) end}",
             "return {update=function() ignite.value('x','\\0') end}",
         })
        CHECK(app.load(source, 32768, 4096, scene, host) != Error::ok);
    CHECK(app.load("ignite.schedule(0); ignite.timer(1,10,true); local n=0; return "
                   "{update=function() end, event=function(k,v) n=n+1; ignite.value('n',n) end}",
              32768, 4096, scene, host) == Error::ok);
    CHECK(app.tick(1000) == Error::ok && app.values()[0].value == 1); // Coalesce missed periods.
    CHECK(app.tick(1000) == Error::ok && app.values()[0].value == 1);
    CHECK(app.tick(1010) == Error::ok && app.values()[0].value == 2);
    CHECK(app.tick(1009) == Error::invalid_argument);
    CHECK(app.load("return {update=function() for i=1,8 do ignite.log('log') end; "
                   "ignite.value('quota',ignite.log('extra')) end}",
              32768, 4096, scene, host) == Error::ok);
    CHECK(app.diagnostic().logCount == 8 && app.values()[0].value == 9);
    CHECK(app.load("return {update=function() local e; for i=1,40 do local v; "
                   "v,e=ignite.read('system.info') end; ignite.value('quota',e) end}",
              32768, 4096, scene, host) == Error::ok);
    CHECK(app.values()[0].value == int32_t(Error::quota_exceeded));
    CHECK(app.load("ignite.schedule(0); for i=1,8 do ignite.timer(i,1,false) end; return "
                   "{update=function() end,event=function() local n=0; for i=1,300 do n=n+i end; "
                   "ignite.value('n',n) end}",
              32768, 4096, scene, host) == Error::ok);
    CHECK(app.tick(1) == Error::execution_limit); // Shared budget, not eight fresh allowances.
    CHECK(app.diagnostic().error == Error::execution_limit && !app.active());
    // Untrusted value bindings cannot replace app-owned published data.
    Runtime legacy;
    CHECK(legacy.launch({{}, argv[6], false}, "main", platform.memory, platform.capabilities) ==
        Error::ok);
    CHECK(legacy.postInput({InputKind::press, 1, legacy.generation()}) == Error::ok);
    CHECK(legacy.tick(33) == Error::ok);
    std::cout << "Application input, services, persistence, timers, UI and diagnostics: passed\n";
}
