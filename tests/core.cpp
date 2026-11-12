#include <cstdlib>
#include <filesystem>
#include <flint/core.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <new>
static bool trackAllocations = false;
static size_t allocations = 0;
void *operator new(std::size_t size) {
    if (trackAllocations)
        ++allocations;
    if (auto *p = std::malloc(size ? size : 1))
        return p;
    throw std::bad_alloc();
}
void *operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void *p) noexcept {
    std::free(p);
}
void operator delete[](void *p) noexcept {
    std::free(p);
}
void operator delete(void *p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void *p, std::size_t) noexcept {
    std::free(p);
}
using namespace flint;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
static Bytes file(const std::string &path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}
static std::string text(const FlintRender::Scene &scene) {
    return {reinterpret_cast<const char *>(scene.data), scene.used};
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    std::string root = argv[1];
    uint64_t tick = 1000;
    bool lowMemory = false;
    Platform platform;
    platform.available_memory = [&] { return lowMemory ? MemoryBudget{} : platform.memory; };
    platform.build_id = "host-tests";
    platform.monotonic = [&] { return tick; };
    uint64_t profileClock = 0;
    platform.microseconds = [&] { return profileClock += 10; };
    platform.diagnostics = [] {
        Values values;
        for (unsigned i = 0; i < 128; ++i)
            values["zz_adapter_" + std::to_string(i)] = std::string(192, 'x');
        return values;
    };
    platform.free_storage = [] { return uint64_t(2 * 1024 * 1024); };
    platform.capabilities |= CapResources;
    {
        auto startupPlatform = platform;
        startupPlatform.power_on = true;
        Core startup(root + "/startup", root + "/factory", startupPlatform);
        CHECK(startup.begin() == Error::ok);
        CHECK(startup.runtime().manifest().id == "system.boot");
        // Initialization time does not consume the presentation interval.
        tick += 5000;
        startup.tick();
        CHECK(startup.runtime().manifest().id == "system.boot");
        FlintRender::Scene splash;
        CHECK(startup.scene(splash) == Error::ok);
        CHECK(startup.nextWake() <= tick + 33); // Animation frames precede the handoff deadline.
        tick += 699;
        startup.tick();
        CHECK(startup.runtime().manifest().id == "system.boot");
        ++tick;
        CHECK(startup.tick());
        CHECK(startup.runtime().manifest().id == "system.clock");
        CHECK(startup.scene(splash) == Error::ok);
        CHECK(startup.lastLogicError() == Error::ok);

        Core interrupted(root + "/interrupted", root + "/factory", startupPlatform);
        CHECK(interrupted.begin() == Error::ok);
        CHECK(interrupted.scene(splash) == Error::ok);
        CHECK(interrupted.launch("system.settings", "main") == Error::ok);
        tick += 1000;
        interrupted.tick();
        CHECK(interrupted.runtime().manifest().id == "system.settings");

        // A missing optional splash must not block the selected clock or recovery.
        std::filesystem::copy(
            root + "/factory", root + "/without-boot", std::filesystem::copy_options::recursive);
        std::filesystem::remove(root + "/without-boot/boot.ignit");
        Core missing(root + "/missing", root + "/without-boot", startupPlatform);
        CHECK(missing.begin() == Error::ok);
        CHECK(missing.runtime().manifest().id == "system.clock");
        tick = 1000;
    }
    Core core(root + "/store", root + "/factory", platform);
    CHECK(core.begin() == Error::ok);
    CHECK(core.runtime().manifest().id == "system.clock");
    CHECK(core.packages().list().size() == 9);
    Session session{true, false, 7};
    uint32_t sequence = 0;
    auto request = [&](Method method, Bytes data = Bytes{}, Bytes *result = nullptr) {
        auto reply = core.handle({method, ++sequence, false, std::move(data)}, session);
        Reader r{reply.payload};
        auto error = Error(r.n(2));
        CHECK(r.valid);
        if (result)
            result->assign(reply.payload.begin() + 2, reply.payload.end());
        return error;
    };
    CHECK(request(Method::info) == Error::incompatible_contract);
    auto hello = [&](uint32_t revision) {
        Writer w;
        w.n(1, 2);
        w.n(revision);
        w.text("tests");
        w.n(CapPackages, 8);
        return request(Method::hello, w.data);
    };
    CHECK(hello(0) == Error::incompatible_contract);
    CHECK(hello(ContractRevision) == Error::ok);
    session.authenticated = false;
    CHECK(request(Method::info) == Error::permission_denied);
    CHECK(request(Method::diagnostics) == Error::permission_denied);
    CHECK(request(Method::companion_subscribe) == Error::permission_denied);
    CHECK(request(Method::session_close) == Error::permission_denied);
    session.authenticated = true;
    Bytes info;
    CHECK(request(Method::info, {}, &info) == Error::ok);
    FlintRender::Scene scene;
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("--:--") != std::string::npos);
    Writer epoch;
    epoch.n(1793740000, 8);
    CHECK(request(Method::time_set, epoch.data) == Error::ok);
    CHECK(core.now() == 1793740000);
    auto begin = [&](const Bytes &bytes) {
        Sha256 sha;
        sha.update(bytes);
        std::string digest;
        const char *hex = "0123456789abcdef";
        for (auto b : sha.finish()) {
            digest += hex[b >> 4];
            digest += hex[b & 15];
        }
        Writer w;
        w.n(bytes.size());
        w.text(digest);
        Bytes reply;
        CHECK(request(Method::install_begin, w.data, &reply) == Error::ok);
        Reader r{reply};
        return uint32_t(r.n());
    };
    auto chunk = [&](const Bytes &data, uint32_t offset, uint32_t count) {
        Writer w;
        w.n(offset);
        w.raw(std::span(data).subspan(offset, count));
        return request(Method::install_chunk, w.data);
    };
    auto v1 = file(root + "/v1.ignit");
    CHECK(begin(v1) == 0);
    CHECK(chunk(v1, 0, 100) == Error::ok);
    CHECK(chunk(v1, 0, 100) == Error::ok);
    CHECK(core.tick());
    CHECK(!core.tick());
    core.disconnect(session);
    CHECK(!core.tick()); // A control-only disconnect does not dirty the local runtime.
    session = {true, false, 8};
    CHECK(hello(ContractRevision) == Error::ok);
    CHECK(begin(v1) == 100);
    CHECK(chunk(v1, 101, 10) == Error::invalid_argument);
    CHECK(chunk(v1, 100, v1.size() - 100) == Error::ok);
    CHECK(request(Method::install_commit) == Error::ok);
    CHECK(core.packages().list().size() == 10);
    lowMemory = true;
    CHECK(core.launch("test.monitor", "main") == Error::resource_exhausted);
    CHECK(core.runtime().manifest().id == "system.clock");
    CHECK(core.packages().list().size() == 10);
    CHECK(core.packages().begin() == Error::ok);
    CHECK(core.packages().list().size() == 10);
    lowMemory = false;
    CHECK(core.launch("test.monitor", "main") == Error::ok);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU") == std::string::npos);
    Writer metrics;
    metrics.n(1234);
    metrics.n(5600);
    CHECK(request(Method::metrics, metrics.data) == Error::permission_denied);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU") == std::string::npos);
    auto grant = [&](bool enabled) {
        Writer w;
        w.text("test.monitor");
        w.text("companion.pc.metrics");
        w.text("cpu,ram");
        w.n(enabled, 1);
        return request(Method::permission_set, w.data);
    };
    CHECK(grant(true) == Error::ok);
    CHECK(request(Method::metrics, metrics.data) == Error::ok);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU 12.3%") != std::string::npos);
    // Demand snapshots are unsolicited, bounded and coalesced across lifecycle changes.
    Writer subscribe;
    subscribe.n(CompanionDemandVersion, 2);
    subscribe.n(1, 1);
    CHECK(request(Method::companion_subscribe, subscribe.data) == Error::ok);
    CHECK(request(Method::companion_subscribe, subscribe.data) == Error::ok);
    Frame event;
    CHECK(core.takeCompanionEvent(session, event));
    CHECK(isCompanionEvent(event) && event.payload.size() <= 64);
    Reader demandReader{event.payload};
    CHECK(demandReader.n(2) == CompanionDemandVersion && demandReader.n(1) == 1);
    const auto demandGeneration = demandReader.n(8);
    CHECK(demandReader.n(1) == uint8_t(CompanionDemand::active));
    CHECK(!core.takeCompanionEvent(session, event));
    Writer binding;
    binding.n(CompanionDemandVersion, 2);
    binding.n(1, 1);
    binding.n(demandGeneration, 8);
    CHECK(request(Method::companion_bind, binding.data) == Error::ok);
    Writer sample;
    sample.raw(binding.data);
    sample.raw(metrics.data);
    tick += 1000;
    CHECK(request(Method::companion_sample, sample.data) == Error::ok);
    CHECK(request(Method::companion_sample, sample.data) == Error::quota_exceeded);
    Session outsider{true, true, 111};
    auto denied = core.handle({Method::companion_sample, 1, false, sample.data}, outsider);
    CHECK(u16(denied.payload.data()) == uint16_t(Error::permission_denied));
    for (uint64_t id = 20; id < 24; ++id) {
        Session peer{true, true, id};
        auto result = core.handle({Method::companion_subscribe, 1, false, subscribe.data}, peer);
        CHECK(u16(result.payload.data()) == uint16_t(id == 23 ? Error::quota_exceeded : Error::ok));
    }
    for (uint64_t id = 20; id < 24; ++id) {
        Session peer{true, true, id};
        core.disconnect(peer);
    }
    // Device Control sessions do not own another session's provider data/events.
    Session control{true, true, 99};
    core.disconnect(control);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU 12.3%") != std::string::npos);
    CHECK(core.runtime().suspend() == Error::ok);
    CHECK(core.runtime().resume() == Error::ok);
    auto stale = request(Method::companion_sample, sample.data);
    CHECK(stale == Error::stale_handle);
    tick += 100;
    CHECK(core.takeCompanionEvent(session, event));
    CHECK(!core.takeCompanionEvent(session, event));
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU") == std::string::npos);
    CHECK(request(Method::metrics, metrics.data) == Error::ok);
    tick += 10000;
    core.tick();
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU") == std::string::npos);
    // A silent provider loses its lease even if it never submitted a sample.
    Session replacement{true, true, 223};
    auto subscribed =
        core.handle({Method::companion_subscribe, 1, false, subscribe.data}, replacement);
    CHECK(u16(subscribed.payload.data()) == 0);
    CHECK(core.takeCompanionEvent(replacement, event));
    Reader state{event.payload};
    state.n(2);
    state.n(1);
    Writer takeover;
    takeover.n(CompanionDemandVersion, 2);
    takeover.n(1, 1);
    takeover.n(state.n(8), 8);
    auto boundReply = core.handle({Method::companion_bind, 2, false, takeover.data}, replacement);
    CHECK(u16(boundReply.payload.data()) == 0);
    CHECK(request(Method::metrics, metrics.data) == Error::invalid_state);
    tick += CompanionTtlMs;
    core.tick();
    CHECK(request(Method::metrics, metrics.data) == Error::ok);
    const auto closed = core.handle({Method::session_close, 3, false, {}}, replacement);
    CHECK(u16(closed.payload.data()) == 0 && !replacement.compatible);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU 12.3%") != std::string::npos);
    core.disconnect(session);
    CHECK(core.runtime().state() == State::active);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU") == std::string::npos);
    CHECK(hello(ContractRevision) == Error::ok);
    CHECK(request(Method::metrics, metrics.data) == Error::ok);
    CHECK(core.scene(scene) == Error::ok);
    allocations = 0;
    trackAllocations = true;
    for (unsigned frame = 0; frame < 10; ++frame)
        CHECK(core.scene(scene) == Error::ok);
    trackAllocations = false;
    CHECK(allocations == 0); // No catalog scan, bindings copy or temporary strings per frame.
    CHECK(grant(false) == Error::ok);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU") == std::string::npos);
    CHECK(grant(true) == Error::ok);
    auto install = [&](const std::string &path) {
        auto bytes = file(path);
        auto offset = begin(bytes);
        CHECK(chunk(bytes, offset, bytes.size() - offset) == Error::ok);
        return request(Method::install_commit);
    };
    CHECK(install(root + "/bad-canvas.ignit") == Error::invalid_package);
    CHECK(install(root + "/v2.ignit") == Error::ok);
    Installed current;
    CHECK(core.packages().locate("test.monitor", current) == Error::ok);
    CHECK(current.manifest.version == 2);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("CPU") == std::string::npos);
    CHECK(install(root + "/spoof.ignit") == Error::permission_denied);
    CHECK(core.packages().locate("system.clock", current) == Error::ok);
    CHECK(current.factory && current.manifest.version == 3);
    // Simulate power-loss/corruption with a valid previous reference and orphaned temporary files.
    core.runtime().stop();
    CHECK(core.packages().locate("test.monitor", current) == Error::ok);
    const auto damaged = current.path;
    const Bytes broken{0, 1, 2, 3};
    CHECK(writeAtomic(damaged, broken));
    {
        std::ofstream temporary(root + "/store/interrupted.ref.tmp");
        temporary << "uncommitted";
    }
    {
        Core rebooted(root + "/store", root + "/factory", platform);
        CHECK(rebooted.begin() == Error::ok);
        Installed restored;
        CHECK(rebooted.packages().locate("test.monitor", restored) == Error::ok);
        CHECK(restored.manifest.version == 1);
        CHECK(readText(root + "/store/interrupted.ref.tmp").empty());
        CHECK(rebooted.runtime().manifest().id == "system.clock");
    }
    Writer rollback;
    rollback.text("test.monitor");
    CHECK(request(Method::rollback, rollback.data) == Error::ok);
    CHECK(core.packages().locate("test.monitor", current) == Error::ok);
    CHECK(current.manifest.version == 1);
    CHECK(install(root + "/resources.ignit") == Error::ok);
    CHECK(core.launch("test.resources", "main") == Error::ok);
    CHECK(core.scene(scene) == Error::ok);
    auto handle = core.runtime().resources().handle(5);
    std::array<uint8_t, 8> header{};
    CHECK(core.runtime().resources().read(handle, 0, header) == Error::ok);
    std::vector<uint16_t> pixels(280 * 240);
    FlintRender::Canvas canvas(pixels.data(), 280, 240);
    core.paint(scene, canvas);
    CHECK(pixels[4 * 280 + 4] == FlintRender::rgb(240, 40, 20));
    CHECK(FlintRender::pixelHash(canvas) != 0);
    CHECK(core.launch("system.clock", "main") == Error::ok);
    CHECK(core.runtime().resources().read(handle, 0, header) == Error::stale_handle);
    for (int i = 0; i < 200; ++i) {
        CHECK(core.launch(i % 2 ? "system.clock" : "test.monitor", "main") == Error::ok);
        CHECK(core.scene(scene) == Error::ok);
    }
    Writer remove;
    remove.text("test.monitor");
    CHECK(core.launch("system.launcher", "main") == Error::ok);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("PC Mon") != std::string::npos);
    CHECK(request(Method::remove, remove.data) == Error::ok);
    core.tick(); // Deliver catalogue invalidation to the reactive Launcher.
    CHECK(core.scene(scene) == Error::ok);
    CHECK(text(scene).find("PC Mon") == std::string::npos);
    CHECK(core.launch("system.clock", "main") == Error::ok);
    CHECK(core.runtime().manifest().id == "system.clock");
    CHECK(core.packages().locate("test.monitor", current) == Error::source_unavailable);
    CHECK(request(Method::recover) == Error::ok);
    CHECK(core.runtime().manifest().id == "system.clock");
    // Bounded substitutions preserve literal values, missing-key fallback and output on failure.
    Runtime bound;
    CHECK(bound.launch({{}, root + "/bindings.ignit", false}, "main", platform.memory,
              platform.capabilities) == Error::ok);
    Values values{{"input", "${not-recursive}"}};
    CHECK(bound.render(values, scene) == Error::ok);
    CHECK(text(scene) == "X${not-recursive}Y:resource:—");
    allocations = 0;
    trackAllocations = true;
    CHECK(bound.render(values, scene) == Error::ok);
    trackAllocations = false;
    CHECK(allocations == 0);
    const auto unchanged = text(scene);
    values["input"] = std::string(192, 'x');
    CHECK(bound.render(values, scene) == Error::quota_exceeded && text(scene) == unchanged);
    values["input"] = std::string("a\0b", 3);
    CHECK(bound.render(values, scene) == Error::quota_exceeded && text(scene) == unchanged);
    values["input"] = "";
    CHECK(bound.render(values, scene) == Error::ok && text(scene) == "XY:resource:—");
    // Framing: fragmentation, noise, invalid CRC and concatenated frames.
    Frame original{Method::info, 42, false, {1, 2, 3}};
    auto wire = encodeFrame(original);
    FrameParser parser;
    unsigned frames = 0;
    auto receive = [&](Frame f) {
        CHECK(f.id == 42 && f.payload == original.payload);
        ++frames;
    };
    Bytes noisy{0, 1, 2, 3};
    noisy.insert(noisy.end(), wire.begin(), wire.end());
    for (auto byte : noisy)
        parser.feed({&byte, 1}, receive);
    CHECK(frames == 1);
    auto corrupt = wire;
    corrupt.back() ^= 128;
    parser.feed(corrupt, receive);
    parser.feed(wire, receive);
    CHECK(frames == 2);
    Frame maximum{Method::info, 42, false, Bytes(MaxPayload, 0)};
    const auto maximumWire = encodeFrame(maximum);
    FrameParser maximumParser;
    unsigned maximumFrames = 0;
    for (unsigned cycle = 0; cycle < 10; ++cycle)
        for (const auto byte : maximumWire)
            maximumParser.feed({&byte, 1}, [&](Frame f) {
                CHECK(f.payload.size() == MaxPayload);
                ++maximumFrames;
            });
    CHECK(maximumFrames == 10 && maximumParser.bufferCapacity() <= MaxPayload + 20);
    maximumParser.reset();
    CHECK(maximumParser.bufferCapacity() <= MaxPayload + 20);
    EventBus bus;
    for (int i = 0; i < 32; ++i)
        CHECK(bus.post({uint32_t(i)}) == Error::ok);
    CHECK(bus.post({}) == Error::quota_exceeded);
    Event e;
    for (int i = 0; i < 32; ++i) {
        CHECK(bus.poll(e));
        CHECK(e.type == unsigned(i));
    }
    CHECK(!bus.poll(e));
    CHECK(bus.peak() == 32 && bus.overflows() == 1);
    bus.clear();
    CHECK(bus.empty() && bus.peak() == 32 && bus.overflows() == 1);
    Bytes diagnosticBytes;
    CHECK(request(Method::diagnostics, {}, &diagnosticBytes) == Error::ok);
    Reader diagnosticReader{diagnosticBytes};
    Values counters;
    const auto diagnosticCount = diagnosticReader.n(2);
    for (unsigned i = 0; i < diagnosticCount; ++i) {
        const auto key = diagnosticReader.text(80);
        counters[key] = diagnosticReader.text(192);
    }
    CHECK(diagnosticReader.done());
    CHECK(diagnosticBytes.size() + 2 <= MaxPayload);
    CHECK(counters.at("diagnostics_truncated") == "1");
    CHECK(std::stoull(counters.at("request_count")) > 0);
    CHECK(std::stoull(counters.at("request_max_us")) >= 10);
    CHECK(std::stoull(counters.at("resource_read_bytes")) >= header.size());
    CHECK(std::stoull(counters.at("resource_read_errors")) > 0);
    // Scene preparation never writes epoch; retries are bounded and explicitly recoverable.
    Platform clockPlatform = platform;
    uint64_t clock = 0;
    clockPlatform.monotonic = [&] { return clock; };
    clockPlatform.diagnostics = {};
    Core checkpoint(root + "/checkpoint-store", root + "/factory", clockPlatform);
    CHECK(checkpoint.begin() == Error::ok);
    Session clockSession{true, true, 1000};
    CHECK(u16(checkpoint.handle({Method::time_set, 1, false, epoch.data}, clockSession)
                  .payload.data()) == 0);
    const auto epochPath = root + "/checkpoint-store/epoch";
    const auto persisted = readText(epochPath);
    std::filesystem::create_directory(epochPath + ".tmp");
    const auto snapshot = [&] {
        const auto reply = checkpoint.handle({Method::diagnostics, 2, false, {}}, clockSession);
        Reader fields{reply.payload};
        fields.n(2);
        Values result;
        const auto count = fields.n(2);
        for (unsigned i = 0; i < count; ++i) {
            auto key = fields.text(80);
            result[key] = fields.text(192);
        }
        CHECK(fields.done());
        return result;
    };
    clock = 60000;
    for (unsigned i = 0; i < 10; ++i)
        CHECK(checkpoint.scene(scene) == Error::ok);
    CHECK(readText(epochPath) == persisted);
    CHECK(snapshot().at("epoch_write_errors") == "0");
    for (const auto at : {60000, 120000, 240000}) {
        clock = at;
        checkpoint.tick();
    }
    CHECK(snapshot().at("epoch_write_errors") == "3");
    CHECK(snapshot().at("epoch_checkpoint_disabled") == "1");
    clock += 3600000;
    checkpoint.tick();
    CHECK(snapshot().at("epoch_write_errors") == "3");
    std::filesystem::remove(epochPath + ".tmp");
    CHECK(u16(checkpoint.handle({Method::time_set, 3, false, epoch.data}, clockSession)
                  .payload.data()) == 0);
    checkpoint.tick();
    CHECK(snapshot().at("epoch_durable_writes") == "2");
    clock += 60000;
    checkpoint.tick();
    CHECK(snapshot().at("epoch_durable_writes") == "3");
    CHECK(readText(epochPath) == std::to_string(1793740060));
    // Radio policy is persistent, separate from Companion, and applied only after durable success.
    Platform radioPlatform = platform;
    radioPlatform.capabilities |= CapBlePolicy;
    bool radioEnabled = false, rejectRadio = false;
    unsigned radioChanges = 0;
    radioPlatform.ble_check = [&](bool enabled) {
        return enabled && rejectRadio ? Error::resource_exhausted : Error::ok;
    };
    radioPlatform.ble_apply = [&](bool enabled) {
        radioEnabled = enabled;
        ++radioChanges;
    };
    const auto radioRoot = root + "/radio-store";
    Core radio(radioRoot, root + "/factory", radioPlatform);
    CHECK(radio.begin() == Error::ok && radioEnabled && radioChanges == 1);
    Session radioSession{true, true, 1001};
    const auto setRadio = [&](std::string_view value) {
        Writer setting;
        setting.text("ble_enabled");
        setting.text(value);
        const auto reply =
            radio.handle({Method::settings_set, 1, false, setting.data}, radioSession);
        return Error(u16(reply.payload.data()));
    };
    CHECK(setRadio("false") == Error::invalid_argument && radioChanges == 1);
    std::filesystem::create_directory(radioRoot + "/settings.bin.tmp");
    CHECK(setRadio("0") == Error::io_error && radioEnabled && radioChanges == 1);
    std::filesystem::remove(radioRoot + "/settings.bin.tmp");
    CHECK(setRadio("0") == Error::ok && !radioEnabled && radioChanges == 2);
    rejectRadio = true;
    CHECK(setRadio("1") == Error::resource_exhausted && !radioEnabled && radioChanges == 2);
    const auto radioDiagnostic = radio.handle({Method::diagnostics, 4, false, {}}, radioSession);
    Reader radioFields{radioDiagnostic.payload};
    CHECK(radioFields.n(2) == 0);
    const auto radioCount = radioFields.n(2);
    bool recordedRadioError = false;
    for (unsigned i = 0; i < radioCount; ++i) {
        const auto key = radioFields.text(80), value = radioFields.text(192);
        if (key == "last_ble_policy_error")
            recordedRadioError = value == "10";
    }
    CHECK(radioFields.done() && recordedRadioError);
    Core radioRestart(radioRoot, root + "/factory", radioPlatform);
    CHECK(radioRestart.begin() == Error::ok && !radioEnabled && radioChanges == 3);
    CHECK(radioRestart.runtime().manifest().id == "system.clock");
    rejectRadio = false;
    CHECK(setRadio("1") == Error::ok && radioEnabled);
    // Fixed numeric formatting preserves UTC offsets and calendar transitions.
    Core calendar(root + "/calendar-store", root + "/factory", platform);
    CHECK(calendar.begin() == Error::ok);
    Session calendarSession{true, true, 1002};
    struct CalendarCase {
        uint64_t epoch;
        const char *offset, *time, *date;
    };
    for (const auto &test : {CalendarCase{1700000000, "0", "22:13", "14.11.2023"},
             CalendarCase{1700000000, "840", "12:13", "15.11.2023"},
             CalendarCase{1700000000, "-720", "10:13", "14.11.2023"},
             CalendarCase{1709251140, "0", "23:59", "29.02.2024"},
             CalendarCase{1709251140, "60", "00:59", "01.03.2024"},
             CalendarCase{1735689540, "60", "00:59", "01.01.2025"}}) {
        Writer offset;
        offset.text("utc_offset");
        offset.text(test.offset);
        CHECK(u16(calendar.handle({Method::settings_set, 1, false, offset.data}, calendarSession)
                      .payload.data()) == 0);
        Writer value;
        value.n(test.epoch, 8);
        CHECK(u16(calendar.handle({Method::time_set, 2, false, value.data}, calendarSession)
                      .payload.data()) == 0);
        CHECK(calendar.scene(scene) == Error::ok);
        CHECK(text(scene).find(test.time) != std::string::npos &&
            text(scene).find(test.date) != std::string::npos);
    }
    std::cout << "Core installation/resume/update/rollback, consent/revocation, factory "
                 "protection, external resources, lifecycle and binary framing: passed\n";
}
