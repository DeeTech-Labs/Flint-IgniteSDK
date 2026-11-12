#include <chrono>
#include <filesystem>
#include <flint/core.h>
#include <fstream>
#include <iostream>
#include <vector>
using namespace flint;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)
int main(int argc, char **argv) {
    CHECK(argc == 4);
    Platform platform;
    uint64_t now = 1000;
    platform.monotonic = [&] { return now; };
    Core core(argv[1], argv[2], platform);
    CHECK(core.begin() == Error::ok);
    const auto theme = [&](const char *name) {
        Session session{true, true, 1};
        Writer request;
        request.n(unsigned(Role::theme), 1);
        request.text(std::string("test.theme-") + name + "/main");
        const auto reply = core.handle({Method::role_set, 1, false, request.data}, session);
        Reader reader{reply.payload};
        return Error(reader.n(2));
    };
    CHECK(theme("dark") == Error::ok);
    CHECK(core.launch("system.launcher", "main") == Error::ok);
    core.tick();
    FlintRender::Scene before;
    CHECK(core.scene(before) == Error::ok);
    unsigned icons = 0;
    for (unsigned i = 0; i < before.count; ++i)
        if (before.ops[i].kind == FlintRender::CatalogIcon)
            ++icons;
    CHECK(icons >= 2 && icons <= 6);
    std::vector<uint16_t> full(280 * 240), partial(280 * 240);
    FlintRender::Canvas canvas(full.data(), 280, 240);
    core.paint(before, canvas);
    // Repainting a clip over real prior pixels must match the complete scene.
    partial = full;
    FlintRender::Canvas clipped(partial.data(), 280, 240, true);
    for (auto &pixel : partial)
        pixel = FlintRender::swap(pixel);
    clipped.clip = {50, 80, 9, 17};
    core.paint(before, clipped);
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 280; ++x)
            CHECK(clipped.get(x, y) == canvas.get(x, y));
#ifndef _WIN32
    // POSIX permits modifying metadata while a read handle is open. Windows
    // intentionally denies this through File's sharing policy.
    // Simulate an invalidated warm file during a one-pixel repaint. The next
    // scene must invalidate the whole screen, otherwise old icon pixels remain.
    const auto icon = *std::find_if(before.ops, before.ops + before.count,
        [](const auto &op) { return op.kind == FlintRender::CatalogIcon; });
    unsigned index = 0;
    std::string path;
    core.packages().visit([&](const Installed &installed) {
        for (const auto &component : installed.manifest.components) {
            if (launcherApp(installed, component) && ++index == icon.extra) {
                path = installed.path;
                return false;
            }
        }
        return true;
    });
    CHECK(!path.empty());
    const auto timestamp = std::filesystem::last_write_time(path);
    std::filesystem::last_write_time(path, timestamp + std::chrono::hours(1));
    clipped.clip = {icon.x, icon.y, 1, 1};
    core.paint(before, clipped);
    CHECK(core.tick());
    FlintRender::Scene repaired;
    CHECK(core.scene(repaired) == Error::ok);
    CHECK(repaired.resourceRevision != before.resourceRevision);
    CHECK(FlintRender::dirty(before, repaired).w == 280);
    std::filesystem::last_write_time(path, timestamp);
    core.paint(repaired, canvas);
    CHECK(canvas.get(icon.x + 47, icon.y + 47) == icon.color);
    before = repaired;
#endif
    for (const auto name : {"light", "themed", "dark"}) {
        CHECK(theme(name) == Error::ok);
        core.tick();
        FlintRender::Scene next;
        CHECK(core.scene(next) == Error::ok);
        // All semantic colors and operations are identical; only icon mode changes.
        std::array<uint8_t, FlintRender::MaxEncoded> a{}, b{};
        const auto size = FlintRender::encode(before, a.data(), a.size());
        CHECK(size && FlintRender::encode(next, b.data(), b.size()) == size);
        CHECK(std::equal(a.begin(), a.begin() + size, b.begin()));
        CHECK(next.resourceRevision != before.resourceRevision);
        CHECK(FlintRender::dirty(before, next).w == 280);
        core.paint(next, canvas);
        before = next;
    }
    const auto input = [&](InputKind kind, int value) {
        now += 33;
        const auto error = core.postInput({kind, value, core.runtime().generation()});
        core.tick();
        return error == Error::ok && core.lastLogicError() == Error::ok;
    };
    CHECK(input(InputKind::rotate, 90));
    FlintRender::Scene after;
    CHECK(core.scene(after) == Error::ok && FlintRender::dirty(before, after).h > 0);
    unsigned last = 0;
    for (unsigned i = 0; i < after.count; ++i) {
        const auto &op = after.ops[i];
        if (op.kind == FlintRender::CatalogIcon)
            last = std::max(last, unsigned(op.extra));
        if (op.kind == FlintRender::Text && op.length) {
            CHECK(appText({reinterpret_cast<const char *>(after.data + op.offset), op.length}));
            const auto bounds = FlintRender::bounds(after, op);
            CHECK(bounds.x >= 8 && bounds.x + bounds.w <= 272);
        }
    }
    CHECK(last >= 90);
    core.paint(after, canvas);
    std::ofstream image(argv[3], std::ios::binary);
    image << "P6\n280 240\n255\n";
    for (auto c : full) {
        const char rgb[]{char((c >> 11) * 255 / 31), char(((c >> 5) & 63) * 255 / 63),
            char((c & 31) * 255 / 31)};
        image.write(rgb, 3);
    }
    CHECK(input(InputKind::press, 0) && input(InputKind::release, 0));
    CHECK(core.runtime().manifest().id != "system.launcher");
    std::cout << "120-app launcher: bounded cells, UTF-8 names, focus/activation and partial "
                 "repaint passed\n";
}
