#include <cstdlib>
#include <filesystem>
#include <flint/core.h>
#include <fstream>
#include <iostream>
#include <iterator>

#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
using namespace flint;
static Bytes load(const char *path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
struct SourceState {
    const Bytes &bytes;
    size_t calls{}, readBytes{}, failAt{};
    Source source() {
        return {this, uint32_t(bytes.size()),
            [](void *context, uint32_t offset, std::span<uint8_t> output) {
                auto &state = *static_cast<SourceState *>(context);
                if (++state.calls == state.failAt)
                    return false;
                if (offset > state.bytes.size() || output.size() > state.bytes.size() - offset)
                    return false;
                state.readBytes += output.size();
                std::copy_n(state.bytes.data() + offset, output.size(), output.data());
                return true;
            }};
    }
};
static std::string digest(const Bytes &data) {
    Sha256 hash;
    hash.update(data);
    std::string result;
    for (const auto byte : hash.finish()) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
int main(int argc, char **argv) {
    CHECK(argc == 6);
    const auto bulk = load(argv[4]);
    CHECK(bulk.size() > 700000);
    Package package;
    PackageValidation validation;
    SourceState source{bulk};
    CHECK(validation.begin(package, source.source(), true) == Error::ok);
    unsigned steps = 0;
    while (!validation.done()) {
        CHECK(package.count() == 0); // Partial verification never publishes a usable package.
        const auto previous = source.readBytes;
        CHECK(validation.advance(4096) == Error::ok);
        CHECK(source.readBytes - previous <= 4096);
        ++steps;
    }
    CHECK(steps > 100 && package.count() > 0);
    // One body pass computes both digests; index validation adds only its own bytes.
    CHECK(source.readBytes == bulk.size() + package.count() * EntrySize);
    Sha256 full;
    full.update(bulk);
    CHECK(full.finish() == validation.transferDigest());
    const auto readCalls = source.calls;
    for (const auto failAt :
        {size_t(1), size_t(2), size_t(package.count() + 2), readCalls / 2, readCalls}) {
        SourceState broken{bulk, 0, 0, failAt};
        auto error = validation.begin(package, broken.source());
        while (error == Error::ok && !validation.done())
            error = validation.advance(4096);
        CHECK(error == Error::io_error && package.count() == 0);
    }
    CHECK(validation.begin(package, source.source()) == Error::ok);
    package.close();
    CHECK(validation.advance(4096) == Error::stale_handle);

    uint64_t now = 1000;
    Platform platform;
    platform.build_id = "async-install-test";
    platform.monotonic = [&] { return now; };
    platform.free_storage = [] { return uint64_t(4 * 1024 * 1024); };
    Core core(argv[1], argv[2], platform);
    CHECK(core.begin() == Error::ok);
    const auto counter = load(argv[3]);
    CHECK(writeAtomic(core.packages().staging(), counter));
    std::string counterId;
    CHECK(core.packages().commit(core.packages().staging(), counterId) == Error::ok);
    CHECK(core.launch(counterId, "main") == Error::ok);
    const auto generation = core.runtime().generation();
    const auto metadata = std::to_string(bulk.size()) + ":" + digest(bulk);
    CHECK(writeAtomic(core.packages().staging(), bulk));
    CHECK(writeAtomic(core.packages().staging() + ".meta",
        {reinterpret_cast<const uint8_t *>(metadata.data()), metadata.size()}));
    Session owner{true, true, 17}, control{true, true, 30};
    CHECK(!core.process({Method::install_commit, 50, false, {}}, owner));
    CHECK(core.nextWake() <= now);
    for (int i = 0; i < 5; ++i) {
        now += 100;
        core.tick();
    }
    core.disconnect(owner);
    CHECK(!core.replyReady(owner) && core.runtime().state() == State::active);
    CHECK(core.runtime().generation() == generation);
    CHECK(readText(core.packages().staging() + ".meta", 100) == metadata);
    owner = {true, true, 18};
    CHECK(!core.process({Method::install_commit, 51, false, {}}, owner));
    now += 100;
    core.tick();
    const auto closed = core.process({Method::session_close, 52, false, {}}, owner);
    CHECK(closed && u16(closed->payload.data()) == uint16_t(Error::ok));
    CHECK(!owner.compatible && !core.replyReady(owner));
    CHECK(core.runtime().state() == State::active && core.runtime().generation() == generation);
    CHECK(readText(core.packages().staging() + ".meta", 100) == metadata);
    owner = {true, true, 19};
    CHECK(!core.process({Method::install_commit, 53, false, {}}, owner));
    unsigned ticks = 0, presses = 0;
    while (!core.replyReady(owner) && ticks < 1000) {
        auto info = core.process({Method::info, 1, false, {}}, control);
        CHECK(info && u16(info->payload.data()) == uint16_t(Error::ok));
        auto mutation = core.process({Method::install_chunk, 2, false, {1}}, control);
        CHECK(mutation && u16(mutation->payload.data()) == uint16_t(Error::invalid_state));
        if (!(ticks % 10)) {
            CHECK(core.postInput({InputKind::press, 1, generation}) == Error::ok);
            ++presses;
        }
        now += 100;
        core.tick();
        CHECK(core.runtime().state() == State::active && core.runtime().generation() == generation);
        ++ticks;
    }
    CHECK(ticks > 100 && ticks < 1000 && presses > 0);
    Frame reply;
    CHECK(!core.takeReply(control, reply));
    CHECK(core.takeReply(owner, reply) && reply.id == 53 && reply.method == Method::install_commit);
    Reader result{reply.payload};
    CHECK(result.n(2) == uint16_t(Error::ok));
    CHECK(result.text(64) == "test.bulk" && result.done());
    CHECK(!core.takeReply(owner, reply));
    FlintRender::Scene scene;
    CHECK(core.scene(scene) == Error::ok);
    const std::string text(reinterpret_cast<const char *>(scene.data), scene.used);
    CHECK(text.find("Count: " + std::to_string(presses)) != std::string::npos);
    Installed installed;
    CHECK(core.packages().locate("test.bulk", installed) == Error::ok);
    CHECK(readText(core.packages().staging() + ".meta").empty());
    const auto update = load(argv[5]);
    CHECK(writeAtomic(core.packages().staging(), update));
    std::unique_ptr<PreparedInstall> prepared;
    CHECK(core.packages().prepareCommit(prepared) == Error::ok);
    while (!prepared->ready())
        CHECK(prepared->advance(4096) == Error::ok);
    // Windows prevents replacement of the locked source. POSIX permits rename,
    // so publication must detect that the path names a different inode.
#ifdef _WIN32
    CHECK(!writeAtomic(core.packages().staging(), bulk));
    CHECK(
        core.packages().begin() == Error::ok); // Explicit catalog invalidation also expires proof.
#else
    CHECK(writeAtomic(core.packages().staging(), bulk));
#endif
    std::string ignored;
    CHECK(core.packages().commit(*prepared, ignored) == Error::stale_handle);
    prepared.reset();
    CHECK(core.packages().locate(counterId, installed) == Error::ok &&
        installed.manifest.version == 1);
    for (const auto suffix : {".previous.tmp", ".tmp"}) {
        CHECK(writeAtomic(core.packages().staging(), update));
        CHECK(core.packages().prepareCommit(prepared) == Error::ok);
        while (!prepared->ready())
            CHECK(prepared->advance(4096) == Error::ok);
        const auto blocked = std::filesystem::path(argv[1]) / (counterId + ".ref" + suffix);
        CHECK(std::filesystem::create_directory(blocked));
        CHECK(core.packages().commit(*prepared, ignored) == Error::io_error);
        CHECK(std::filesystem::remove(blocked));
        prepared.reset();
        CHECK(core.packages().locate(counterId, installed) == Error::ok &&
            installed.manifest.version == 1);
        CHECK(core.runtime().state() == State::active && core.runtime().generation() == generation);
    }
    CHECK(writeAtomic(core.packages().staging(), update));
    const auto updateMetadata = std::to_string(update.size()) + ":" + digest(update);
    CHECK(writeAtomic(core.packages().staging() + ".meta",
        {reinterpret_cast<const uint8_t *>(updateMetadata.data()), updateMetadata.size()}));
    CHECK(!core.process({Method::install_commit, 52, false, {}}, owner));
    ticks = 0;
    while (!core.replyReady(owner) && ticks++ < 1000) {
        now += 100;
        core.tick();
    }
    CHECK(core.takeReply(owner, reply) && u16(reply.payload.data()) == uint16_t(Error::ok));
    CHECK(core.runtime().state() == State::active && core.runtime().scripted());
    CHECK(core.runtime().manifest().id == counterId && core.runtime().manifest().version == 2);
    CHECK(core.runtime().generation() != generation);
    CHECK(core.scene(scene) == Error::ok);
    CHECK(std::string(reinterpret_cast<const char *>(scene.data), scene.used)
              .find("Count: " + std::to_string(presses)) != std::string::npos);
    std::cout << "Resumable validation budgets, read errors, live Lua/input and disconnect "
                 "cancellation: passed\n";
}
