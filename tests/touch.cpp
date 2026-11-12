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
static bool image(Core &core, const std::filesystem::path &path) {
    FlintRender::Scene scene;
    const auto err = core.scene(scene);
    if (err != Error::ok) {
        std::cerr << "scene error " << int(err) << '\n';
        return false;
    }
    std::vector<uint16_t> pixels(280 * 240);
    FlintRender::Canvas canvas(pixels.data(), 280, 240);
    core.paint(scene, canvas);
    std::ofstream file(path, std::ios::binary);
    file << "P6\n280 240\n255\n";
    for (auto c : pixels) {
        const char rgb[]{char(((c >> 11) & 31) * 255 / 31), char(((c >> 5) & 63) * 255 / 63),
            char((c & 31) * 255 / 31)};
        file.write(rgb, 3);
    }
    return bool(file);
}
static std::string text(Core &core) {
    FlintRender::Scene scene;
    if (core.scene(scene) != Error::ok)
        return {};
    return {reinterpret_cast<const char *>(scene.data), scene.used};
}
static int position(Core &core, int width, int height) {
    FlintRender::Scene scene;
    if (core.scene(scene) != Error::ok)
        return -9999;
    for (unsigned i = 0; i < scene.count; ++i)
        if (scene.ops[i].kind == FlintRender::Rounded && scene.ops[i].w == width &&
            scene.ops[i].h == height)
            return width == 6 ? scene.ops[i].x : scene.ops[i].y;
    return -9999;
}
int main(int argc, char **argv) {
    CHECK(argc == 4);
    uint64_t now = 1000;
    bool radio = true;
    Platform p;
    p.monotonic = [&] { return now; };
    p.power_on = true;
    p.system_ui = true;
    p.capabilities |= CapBlePolicy;
    p.ble_check = [](bool) { return Error::ok; };
    p.ble_apply = [&](bool enabled) { radio = enabled; };
    Core core(argv[1], argv[2], p);
    CHECK(core.begin() == Error::ok);
    const std::filesystem::path output(argv[3]);
    std::filesystem::create_directories(output);
    CHECK(core.runtime().manifest().id == "system.boot");
    CHECK(image(core, output / "boot-00.ppm"));
    const auto initialMark = position(core, 6, 45);
    for (int i = 1; i <= 7; ++i) {
        now += 33;
        core.tick();
        CHECK(image(core, output / ("boot-0" + std::to_string(i) + ".ppm")));
    }
    CHECK(position(core, 6, 45) != initialMark);
    now = 1700;
    core.tick();
    CHECK(core.runtime().manifest().id == "system.clock");
    CHECK(image(core, output / "clock.ppm"));
    const auto touch = [&](InputKind kind, int x, int y, uint8_t pointer = 0) {
        now += 16;
        const auto error = core.postInput(
            {kind, 0, core.runtime().generation(), int16_t(x), int16_t(y), pointer, now});
        core.tick();
        if (error != Error::ok || core.lastLogicError() != Error::ok)
            std::cerr << "input " << int(kind) << " error " << int(error) << " logic "
                      << int(core.lastLogicError()) << '\n';
        return error == Error::ok && core.lastLogicError() == Error::ok;
    };
    const auto frames = [&](int count) {
        for (int i = 0; i < count; ++i) {
            now += 33;
            core.tick();
        }
    };
    CHECK(touch(InputKind::touch_down, 268, 120));
    CHECK(touch(InputKind::touch_move, 190, 120));
    CHECK(image(core, output / "edge-01.ppm"));
    CHECK(core.runtime().manifest().id == "system.clock");
    CHECK(touch(InputKind::touch_down, 50, 90, 1)); // A second pointer cannot take over.
    CHECK(touch(InputKind::touch_move, 90, 120));
    CHECK(image(core, output / "edge-02.ppm"));
    CHECK(touch(InputKind::touch_up, 90, 120));
    for (int i = 0; i < 12; ++i) {
        frames(1);
        CHECK(image(core, output / ("edge-settle-0" + std::to_string(i) + ".ppm")));
    }
    CHECK(core.runtime().manifest().id == "system.launcher");
    CHECK(image(core, output / "apps.ppm"));
    // Drag does not activate a tile; release transfers its velocity to the scroll controller.
    CHECK(touch(InputKind::touch_down, 70, 200));
    CHECK(touch(InputKind::touch_move, 70, 76));
    CHECK(image(core, output / "apps-drag.ppm"));
    CHECK(touch(InputKind::touch_up, 70, 76));
    for (int i = 0; i < 12; ++i) {
        frames(1);
        CHECK(image(core, output / ("apps-inertia-" + std::to_string(i) + ".ppm")));
    }
    CHECK(core.runtime().manifest().id == "system.launcher");
    const auto before = position(core, 124, 94);
    CHECK(touch(InputKind::touch_down, 70, 120));
    CHECK(position(core, 124, 94) == before); // Regrab cannot jump the list.
    CHECK(touch(InputKind::touch_cancel, 70, 120));
    frames(20);
    // Header back target is 56x56.
    CHECK(touch(InputKind::touch_down, 24, 24));
    CHECK(touch(InputKind::touch_up, 24, 24));
    CHECK(core.runtime().manifest().id == "system.clock");
    // A short edge swipe cancels. No application launch occurs on pointer-down.
    CHECK(touch(InputKind::touch_down, 268, 120));
    CHECK(touch(InputKind::touch_move, 258, 120));
    now += 100;
    CHECK(touch(InputKind::touch_up, 258, 120));
    frames(5);
    CHECK(core.runtime().manifest().id == "system.clock");
    // Reverse an opening transition before it finishes.
    CHECK(touch(InputKind::touch_down, 268, 120));
    CHECK(touch(InputKind::touch_move, 180, 120));
    CHECK(touch(InputKind::touch_up, 180, 120));
    frames(1);
    CHECK(touch(InputKind::touch_down, 80, 120));
    CHECK(touch(InputKind::touch_move, 240, 120));
    CHECK(touch(InputKind::touch_up, 240, 120));
    frames(5);
    CHECK(core.runtime().manifest().id == "system.clock");
    // Settings is a tile in the Apps panel, never a second watchface shortcut.
    CHECK(touch(InputKind::touch_down, 268, 120));
    CHECK(touch(InputKind::touch_move, 80, 120));
    CHECK(touch(InputKind::touch_up, 80, 120));
    frames(12);
    // Filesystem enumeration order is unspecified; find Settings in the actual catalog.
    unsigned catalogIndex = 0, settingsIndex = UINT32_MAX;
    core.packages().visit([&](const Installed &installed) {
        for (const auto &component : installed.manifest.components) {
            if (!launcherApp(installed, component))
                continue;
            if (installed.manifest.id == "system.settings")
                settingsIndex = catalogIndex;
            ++catalogIndex;
        }
        return true;
    });
    CHECK(settingsIndex != UINT32_MAX);
    CHECK(core.postInput({InputKind::rotate, int32_t(settingsIndex), core.runtime().generation()}) == Error::ok);
    core.tick();
    frames(20);
    FlintRender::Scene apps;
    CHECK(core.scene(apps) == Error::ok);
    int settingsX = -1, settingsY = -1;
    for (unsigned i = 0; i < apps.count; ++i) {
        const auto &op = apps.ops[i];
        if (op.kind == FlintRender::CatalogIcon && op.extra == settingsIndex + 1) {
            settingsX = op.x + 24;
            settingsY = op.y + 24;
        }
    }
    CHECK(settingsX >= 0 && settingsY >= 56 && settingsY < 240);
    CHECK(touch(InputKind::touch_down, settingsX, settingsY));
    CHECK(touch(InputKind::touch_up, settingsX, settingsY));
    CHECK(core.runtime().manifest().id == "system.settings");
    CHECK(image(core, output / "settings.ppm"));
    CHECK(touch(InputKind::touch_down, 230, 78));
    CHECK(touch(InputKind::touch_up, 230, 78));
    CHECK(!radio);
    frames(1);
    CHECK(image(core, output / "settings-radio-off.ppm"));
    CHECK(touch(InputKind::touch_down, 234, 124));
    CHECK(touch(InputKind::touch_up, 234, 124));
    frames(1);
    CHECK(text(core).find("+03:15") != std::string::npos);
    CHECK(image(core, output / "settings-all-controls.ppm"));
    CHECK(touch(InputKind::touch_down, 230, 216));
    CHECK(touch(InputKind::touch_up, 230, 216));
    frames(1);
    Session session{true, true, 6};
    const auto settings = core.handle({Method::settings_get, 1, false, {}}, session);
    Reader r{settings.payload};
    CHECK(r.n(2) == 0);
    const auto count = r.n(2);
    bool reduced = false;
    for (unsigned i = 0; i < count; ++i) {
        const auto k = r.text(64), v = r.text(192);
        if (k == "reduce_motion")
            reduced = v == "1";
    }
    CHECK(reduced && r.done());
    CHECK(touch(InputKind::touch_down, 24, 24));
    CHECK(touch(InputKind::touch_up, 24, 24));
    CHECK(touch(InputKind::touch_down, 268, 120));
    CHECK(touch(InputKind::touch_move, 80, 120));
    CHECK(touch(InputKind::touch_up, 80, 120));
    CHECK(core.runtime().manifest().id ==
        "system.launcher"); // Reduced motion commits immediately on release.
    const auto hostilePath = std::filesystem::path(argv[1]).parent_path() / "hostile.ignit";
    std::ifstream hostileFile(hostilePath, std::ios::binary);
    Bytes hostile{std::istreambuf_iterator<char>(hostileFile), {}};
    CHECK(!hostile.empty() && writeAtomic(core.packages().staging(), hostile));
    std::string id;
    CHECK(core.packages().commit(core.packages().staging(), id) == Error::ok);
    CHECK(core.launch(id, "main") == Error::ok);
    CHECK(text(core).find("Init:6") != std::string::npos); // No launch from load/update callbacks.
    CHECK(touch(InputKind::touch_up, 100, 100));
    CHECK(text(core).find("Action:5") != std::string::npos &&
        !radio); // Factory role spoofing is denied.
    std::cout << "Animated boot, edge tracking/cancellation/reversal, touch tiles, inertial "
                 "scrolling, settings and reduced motion passed\n";
}
