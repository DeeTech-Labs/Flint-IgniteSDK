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
    Session session{true, true, 1};
    const auto request = [&](Method method, Writer &writer) {
        const auto frame = core.handle({method, 1, false, writer.data}, session);
        Reader reader{frame.payload};
        return Error(reader.n(2));
    };
    std::filesystem::create_directories(argv[3]);
    for (const auto locale : {"en", "ru"}) {
        Writer selection;
        selection.n(unsigned(Role::locale), 1);
        selection.text(std::string(locale) == "ru" ? "system.locale-ru/ru" : "system.locale/en");
        CHECK(request(Method::role_set, selection) == Error::ok);
        for (const auto app : {"clock", "launcher", "settings", "boot"}) {
            if (std::filesystem::path(argv[3]).filename() == "empty" &&
                std::string_view(app) == "settings")
                continue;
            CHECK(core.launch(std::string("system.") + app, "main") == Error::ok);
            const auto render = [&](std::string suffix) {
                FlintRender::Scene scene;
                if (core.scene(scene) != Error::ok || !scene.valid)
                    return false;
                if (std::filesystem::path(argv[3]).filename() == "empty" &&
                    std::string_view(app) == "launcher") {
                    const std::string content(
                        reinterpret_cast<const char *>(scene.data), scene.used);
                    if (content.find(std::string_view(locale) == "ru"
                                ? "Пока пусто"
                                : "No apps yet") == std::string::npos)
                        return false;
                }
                for (unsigned i = 0; i < scene.count; ++i) {
                    const auto &op = scene.ops[i];
                    if (op.kind != FlintRender::Text || !op.length)
                        continue;
                    const auto bounds = FlintRender::bounds(scene, op);
                    const bool scroller =
                        std::string_view(app) == "launcher" || std::string_view(app) == "settings";
                    if (bounds.x < 8 || bounds.x + bounds.w > 272 ||
                        (!scroller && (bounds.y < 8 || bounds.y + bounds.h > 238))) {
                        std::cerr << locale << '/' << app << suffix << " outside safe content: "
                                  << std::string_view(
                                         reinterpret_cast<const char *>(scene.data + op.offset),
                                         op.length)
                                  << " at " << bounds.x << ',' << bounds.y << " size " << bounds.w
                                  << ',' << bounds.h << '\n';
                        return false;
                    }
                }
                std::vector<uint16_t> pixels(scene.width * scene.height);
                FlintRender::Canvas canvas(pixels.data(), scene.width, scene.height);
                core.paint(scene, canvas);
                std::ofstream image(std::filesystem::path(argv[3]) /
                        (std::string(locale) + '-' + app + suffix + ".ppm"),
                    std::ios::binary);
                image << "P6\n280 240\n255\n";
                for (auto pixel : pixels) {
                    const char rgb[]{char(((pixel >> 11) & 31) * 255 / 31),
                        char(((pixel >> 5) & 63) * 255 / 63), char((pixel & 31) * 255 / 31)};
                    image.write(rgb, 3);
                }
                return bool(image);
            };
            CHECK(render(""));
            if (std::string(app) == "clock") {
                Writer epoch;
                epoch.n(1793740000, 8);
                CHECK(request(Method::time_set, epoch) == Error::ok);
                CHECK(render("-synced"));
            }
        }
    }
    std::cout << "System UI: safe geometry, localized bindings and raster previews passed\n";
}
