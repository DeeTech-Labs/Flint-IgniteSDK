#include <filesystem>
#include <flint/core.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>
using namespace flint;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)
static Bytes load(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), {}};
}
static bool snapshotScene(Core &core, const FlintRender::Scene &scene,
    const std::filesystem::path &path, std::string_view expected) {
    const std::string text(reinterpret_cast<const char *>(scene.data), scene.used);
    if (text.find(expected) == std::string::npos) {
        std::cerr << path << ": " << text << '\n';
        return false;
    }
    for (unsigned i = 0; i < scene.count; ++i) {
        const auto &op = scene.ops[i];
        if (op.kind != FlintRender::Text || !op.length)
            continue;
        const auto b = FlintRender::bounds(scene, op);
        if (b.x < 8 || b.x + b.w > 272 || b.y < 8 || b.y + b.h > 238) {
            std::cerr << path << " unsafe text bounds: " << text << '\n';
            return false;
        }
    }
    std::vector<uint16_t> pixels(280 * 240);
    FlintRender::Canvas canvas(pixels.data(), 280, 240);
    core.paint(scene, canvas);
    std::ofstream image(path, std::ios::binary);
    image << "P6\n280 240\n255\n";
    for (auto pixel : pixels) {
        const char rgb[]{char(((pixel >> 11) & 31) * 255 / 31),
            char(((pixel >> 5) & 63) * 255 / 63), char((pixel & 31) * 255 / 31)};
        image.write(rgb, 3);
    }
    return bool(image);
}
static bool snapshot(Core &core, const std::filesystem::path &path, std::string_view expected) {
    FlintRender::Scene scene;
    return core.scene(scene) == Error::ok && snapshotScene(core, scene, path, expected);
}
int main(int argc, char **argv) {
    CHECK(argc == 4);
    const std::filesystem::path root(argv[1]), output(argv[3]);
    std::filesystem::create_directories(output);
    const auto payload = load(root / "payload.ignit");
    CHECK(!payload.empty());
    Sha256 sha;
    sha.update(payload);
    std::string digest;
    for (auto byte : sha.finish()) {
        digest += "0123456789abcdef"[byte >> 4];
        digest += "0123456789abcdef"[byte & 15];
    }
    for (const auto ru : {false, true}) {
        uint64_t now = 1000;
        Platform platform;
        platform.system_ui = true;
        platform.monotonic = [&] { return now; };
        Platform::PairingPrompt pairing;
        platform.pairing_prompt = [&] { return pairing; };
        Core *viewer = nullptr;
        bool removingPresented = false, packagePresentDuringFrame = false;
        const auto store = root / (ru ? "ui-ru" : "ui-en");
        platform.present = [&](const FlintRender::Scene &scene) {
            packagePresentDuringFrame = std::filesystem::exists(store / "test.ui-payload.ref");
            removingPresented = viewer &&
                snapshotScene(*viewer, scene, output / (ru ? "ru-removing.ppm" : "en-removing.ppm"),
                    ru ? "Удаление данных" : "Removing data");
        };
        Core core(store.string(), argv[2], platform);
        viewer = &core;
        CHECK(core.begin() == Error::ok);
        Session session{true, true, 37};
        const auto request = [&](Method method, Writer writer = {}) {
            const auto reply = core.handle({method, 1, false, writer.data}, session);
            Reader reader{reply.payload};
            return Error(reader.n(2));
        };
        Writer selection;
        selection.n(unsigned(Role::locale), 1);
        selection.text(ru ? "system.locale-ru/ru" : "system.locale/en");
        CHECK(request(Method::role_set, selection) == Error::ok);
        const auto shot = [&](const char *name, std::string_view expected) {
            return snapshot(
                core, output / (std::string(ru ? "ru-" : "en-") + name + ".ppm"), expected);
        };
        CHECK(shot("clock-unsynced", ru ? "Задайте время" : "Sync time"));
        const auto generation = core.runtime().generation();
        Writer begin;
        begin.n(payload.size());
        begin.text(digest);
        CHECK(request(Method::install_begin, begin) == Error::ok);
        CHECK(core.tick());
        CHECK(shot("receiving", "0%"));
        Writer chunk;
        chunk.n(0);
        chunk.raw(payload);
        CHECK(request(Method::install_chunk, chunk) == Error::ok);
        CHECK(shot("received", "100%"));
        CHECK(!core.process({Method::install_commit, 29, false, {}}, session));
        CHECK(shot("verifying", ru ? "Проверка пакета" : "Verifying package"));
        for (int i = 0; !core.replyReady(session) && i < 1000; ++i) {
            now += 100;
            core.tick();
        }
        Frame reply;
        CHECK(core.takeReply(session, reply));
        CHECK(u16(reply.payload.data()) == 0);
        CHECK(core.runtime().generation() == generation);
        CHECK(shot("installed", ru ? "Готово" : "Complete"));
        now += 1500;
        CHECK(core.tick());
        CHECK(shot("restored", "--:--"));
        Writer removal;
        removal.text("test.ui-payload");
        CHECK(request(Method::remove, removal) == Error::ok);
        CHECK(removingPresented && packagePresentDuringFrame);
        CHECK(shot("removed", ru ? "Удаление" : "Removing"));
        now += 1500;
        core.tick();
        // A local Cancel remains effective even if the complete body arrived before commit.
        CHECK(request(Method::install_begin, begin) == Error::ok);
        CHECK(request(Method::install_chunk, chunk) == Error::ok);
        CHECK(core.postInput({InputKind::touch_down, 0, core.runtime().generation(), 140, 212}) ==
            Error::ok);
        CHECK(core.postInput({InputKind::touch_up, 0, core.runtime().generation(), 140, 212}) ==
            Error::ok);
        const auto cancelled = core.process({Method::install_commit, 75, false, {}}, session);
        CHECK(cancelled && u16(cancelled->payload.data()) == unsigned(Error::cancelled));
        Installed absent;
        CHECK(core.packages().locate("test.ui-payload", absent) == Error::source_unavailable);
        CHECK(request(Method::install_begin, begin) == Error::ok);
        core.disconnect(session);
        CHECK(shot("interrupted", ru ? "Передача прервана" : "Transfer interrupted"));
        now += 1500;
        core.tick();
        session = {true, true, 38};
        CHECK(request(Method::install_begin, begin) == Error::ok);
        now += 15000;
        CHECK(core.tick());
        CHECK(shot("no-connection", ru ? "Передача прервана" : "Transfer interrupted"));
        now += 1500;
        core.tick();
        Writer invalidBegin;
        invalidBegin.n(payload.size());
        invalidBegin.text(std::string(64, '0'));
        CHECK(request(Method::install_begin, invalidBegin) == Error::ok);
        auto corrupt = payload;
        corrupt.back() ^= 1;
        Writer bad;
        bad.n(0);
        bad.raw(corrupt);
        CHECK(request(Method::install_chunk, bad) == Error::ok);
        CHECK(!core.process({Method::install_commit, 30, false, {}}, session));
        for (int i = 0; !core.replyReady(session) && i < 1000; ++i) {
            now += 100;
            core.tick();
        }
        CHECK(core.takeReply(session, reply));
        CHECK(u16(reply.payload.data()) != 0);
        CHECK(shot("install-error", ru ? "Ошибка операции" : "Operation failed"));
        now += 1500;
        core.tick();
        pairing.code = 123456;
        pairing.address = {6, 5, 4, 3, 2, 1};
        CHECK(core.tick());
        CHECK(shot("pairing", "123456"));
        now += 10000;
        core.tick();
        CHECK(shot("pairing-pending", "123456"));
        pairing.code = -1;
        CHECK(core.tick());
        CHECK(shot("pairing-ended", "--:--"));
        // Faulty Lua runs in the foreground; presentation recovers to a real clock.
        auto fault = load(root / "fault.ignit");
        CHECK(writeAtomic(core.packages().staging(), fault));
        std::string id;
        CHECK(core.packages().commit(core.packages().staging(), id) == Error::ok);
        CHECK(core.launch(id, "main") == Error::ok);
        now += 100;
        CHECK(core.tick());
        CHECK(core.runtime().manifest().id == "system.clock");
        CHECK(shot("application-error", ru ? "Ошибка приложения" : "Application error"));
        now += 4000;
        core.tick();
        CHECK(shot("error-restored", "--:--"));
        // Explicit consent is already executed by the trusted client; show its actual result.
        CHECK(writeAtomic(core.packages().staging(), payload));
        CHECK(core.packages().commit(core.packages().staging(), id) == Error::ok);
        Writer grant;
        grant.text(id);
        grant.text("companion.pc.metrics");
        grant.text("cpu,ram");
        grant.n(1, 1);
        CHECK(request(Method::permission_set, grant) == Error::ok);
        CHECK(shot("access-allowed", ru ? "Доступ разрешён" : "Access allowed"));
        grant.data.back() = 0;
        CHECK(request(Method::permission_set, grant) == Error::ok);
        CHECK(shot("access-revoked", ru ? "Доступ отозван" : "Access revoked"));
    }
    std::cout << "Real transfer, validation, removal, interruption, error, pairing and permission "
                 "screens passed\n";
}
