#include <charconv>
#include <flint/runtime.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <vector>
int main(int argc, char **argv) {
    if (argc != 5) {
        std::cerr << "Usage: ignite-run package.ignit frames press-every output.ppm\n";
        return 2;
    }
    unsigned frames{}, interval{};
    auto parse = [](const char *text, unsigned &out) {
        auto end = text + std::char_traits<char>::length(text);
        auto result = std::from_chars(text, end, out);
        return result.ec == std::errc{} && result.ptr == end;
    };
    if (!parse(argv[2], frames) || !parse(argv[3], interval) || !frames || frames > 3600 ||
        interval > 3600)
        return 2;
    flint::File file;
    flint::Package package;
    flint::Manifest manifest;
    constexpr auto max = std::numeric_limits<uint32_t>::max();
    if (!file.open(argv[1]) || file.size > flint::MaxPackageBytes ||
        package.open(file.source()) != flint::Error::ok ||
        flint::validateApplication(package, manifest, {max, max, max}, UINT64_MAX) !=
            flint::Error::ok)
        return 1;
    flint::Runtime runtime;
    if (runtime.launch({manifest, argv[1], false}, manifest.components[0].id, {max, max, max},
            UINT64_MAX) != flint::Error::ok ||
        !runtime.scripted())
        return 1;
    FlintRender::Scene scene;
    for (unsigned i = 0; i < frames; ++i) {
        if (runtime.update(33, interval && i % interval == 0) != flint::Error::ok ||
            runtime.render({}, scene) != flint::Error::ok) {
            std::cerr << "Lua application failed at frame " << i << '\n';
            return 1;
        }
    }
    std::vector<uint16_t> pixels(280 * 240);
    FlintRender::Canvas canvas(pixels.data(), 280, 240);
    FlintRender::resourceContext = &runtime.resources();
    FlintRender::resourceGlyph = [](void *p, unsigned f, uint32_t cp) {
        return static_cast<flint::ResourceManager *>(p)->glyph(f, cp);
    };
    FlintRender::resourceImage = [](void *p, FlintRender::Canvas &c, const FlintRender::Op &op) {
        static_cast<flint::ResourceManager *>(p)->image(c, op);
    };
    FlintRender::render(scene, canvas);
    std::ofstream image(argv[4], std::ios::binary);
    image << "P6\n280 240\n255\n";
    for (auto pixel : pixels) {
        char rgb[3]{char(((pixel >> 11) & 31) * 255 / 31), char(((pixel >> 5) & 63) * 255 / 63),
            char((pixel & 31) * 255 / 31)};
        image.write(rgb, 3);
    }
    image.close();
    if (!image)
        return 1;
    std::cout << "{\"frames\":" << frames << ",\"pixel_hash\":" << FlintRender::pixelHash(canvas)
              << ",\"lua_peak_bytes\":" << runtime.logicPeak() << "}\n";
}
