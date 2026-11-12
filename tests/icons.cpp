#include <filesystem>
#include <flint/icons.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace flint;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            return 1;                                                                              \
        }                                                                                          \
    } while (false)
int main(int argc, char **argv) {
    CHECK(argc == 2);
    const std::filesystem::path root(argv[1]);
    PackageManager packages((root / "store").string(), (root / "factory").string(),
        {1024 * 1024, 1024 * 1024, 100000}, UINT64_MAX);
    CHECK(packages.begin() == Error::ok);
    IconCache cache;
    std::array<uint8_t, FlimDecoder::ScratchBytes> scratch{};
    FlimDecoder decoder(scratch);
    const auto install = [&](const char *name) {
        cache.clear(); // Core closes cached files before filesystem mutations too.
        packages.remove("test.icons");
        std::ifstream file(root / name, std::ios::binary);
        Bytes bytes{std::istreambuf_iterator<char>(file), {}};
        if (!writeAtomic(packages.staging(), bytes))
            return Error::io_error;
        std::string id;
        return packages.commit(packages.staging(), id);
    };
    std::array<uint16_t, 48 * 48> pixels;
    FlintRender::Canvas canvas(pixels.data(), 48, 48);
    FlintRender::Op op;
    op.kind = FlintRender::CatalogIcon;
    op.w = op.h = 48;
    op.extra = 1;
    op.color = 0xffff;
    const auto paint = [&](bool themed = false, bool dark = false) {
        pixels.fill(0x07e0);
        cache.draw(packages, decoder, canvas, op, 1, themed, dark);
        return canvas.get(0, 0);
    };
    CHECK(install("variants.ignit") == Error::ok);
    CHECK(paint() == 0x07e0);            // light variant
    CHECK(paint(false, true) == 0x001f); // dark variant; no stale decoded/palette cache
    CHECK(paint(true, true) == 0xffff && paint(true, false) == 0xffff); // semantic mask tint
    op.color = 0;
    CHECK(paint(true, false) == 0);
    op.color = 0xffff;
    CHECK(install("main.ignit") == Error::ok);
    CHECK(paint() == 0xf800 && paint(true, true) == 0xf800 && paint(false, true) == 0xf800);
    // Update without manually clearing: revision must invalidate a warm entry.
    {
        std::ifstream file(root / "updated.ignit", std::ios::binary);
        Bytes bytes{std::istreambuf_iterator<char>(file), {}};
        CHECK(writeAtomic(packages.staging(), bytes));
        std::string id;
        CHECK(packages.commit(packages.staging(), id) == Error::ok);
    }
    CHECK(paint() == 0x001f);
    cache.clear();
    CHECK(packages.remove("test.icons") == Error::ok);
    CHECK(paint() == 0xffff);   // cached missing resource becomes the standard tile
    canvas.clip = {0, 0, 1, 1}; // Invalid indices outside the clip must still trigger fallback.
    for (const auto name :
        {"missing.ignit", "truncated.ignit", "bad-index.ignit", "bad-type.ignit"}) {
        CHECK(install(name) == Error::ok);
        CHECK(paint() == 0xffff);
    }
    // Resource corruption with a broken package digest is rejected, not icon fallback.
    CHECK(install("integrity.ignit") == Error::integrity_failed);
    std::cout
        << "Icon variants, tint, fallback, installation/update/removal and integrity passed\n";
}
