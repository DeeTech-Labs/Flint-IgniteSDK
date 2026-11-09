#include <cstdlib>
#include <flint/resources.h>
#include <iostream>
#include <vector>
using namespace flint;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
struct MemorySource {
    Bytes bytes;
    size_t reads{}, readBytes{}, bitmapReads{};
    uint32_t bitmapBegin{}, bitmapEnd{};
    Source source() {
        return {this, uint32_t(bytes.size()),
            [](void *context, uint32_t offset, std::span<uint8_t> out) {
                auto &s = *static_cast<MemorySource *>(context);
                if (offset > s.bytes.size() || out.size() > s.bytes.size() - offset)
                    return false;
                ++s.reads;
                s.readBytes += out.size();
                if (offset < s.bitmapEnd && offset + out.size() > s.bitmapBegin)
                    ++s.bitmapReads;
                std::copy_n(s.bytes.data() + offset, out.size(), out.data());
                return true;
            }};
    }
    void reset() {
        reads = readBytes = bitmapReads = 0;
    }
};
int main() {
    Writer font;
    font.n(0x46474c46);
    font.n(2, 2);
    font.n(0, 2);
    font.n(0);
    for (const char cp : {'A', 'B'}) {
        font.n(cp);
        font.n(2, 2);
        font.n(3, 2);
        font.n(uint16_t(-1), 2);
        font.n(3, 2);
        font.n(3, 2);
        font.n(0, 2);
        font.n(cp == 'A' ? 60 : 66);
        font.n(6);
    }
    for (int i = 0; i < 12; ++i)
        font.n(255, 1);
    Writer image;
    image.n(0x4d494c46);
    image.n(8, 2);
    image.n(8, 2);
    for (unsigned i = 0; i < 64; ++i)
        image.n(i + 1, 2);
    struct SectionData {
        uint32_t id;
        Section type;
        Bytes data;
    };
    std::vector<SectionData> sections{{1, Section::manifest, {0}},
        {4, Section::resource, font.data}, {5, Section::resource, image.data}};
    for (unsigned id = 6; id < 50; ++id)
        sections.push_back({id, Section::resource, {uint8_t(id)}});
    Writer packageBytes;
    packageBytes.raw(PackageMagic);
    packageBytes.n(PackageVersion, 2);
    packageBytes.n(HeaderSize, 2);
    packageBytes.n(ContractRevision);
    uint32_t offset = HeaderSize + sections.size() * EntrySize;
    uint32_t total = offset;
    for (const auto &s : sections)
        total += s.data.size();
    packageBytes.n(total);
    packageBytes.n(sections.size());
    packageBytes.n(HeaderSize);
    packageBytes.n(0);
    for (int i = 0; i < 32; ++i)
        packageBytes.n(0, 1);
    MemorySource source;
    for (const auto &s : sections) {
        packageBytes.n(s.id);
        packageBytes.n(uint16_t(s.type), 2);
        packageBytes.n(0, 2);
        packageBytes.n(offset);
        packageBytes.n(s.data.size());
        packageBytes.n(0);
        packageBytes.n(0);
        if (s.id == 4) {
            source.bitmapBegin = offset + 60;
            source.bitmapEnd = offset + 72;
        }
        offset += s.data.size();
    }
    for (const auto &s : sections)
        packageBytes.raw(s.data);
    Sha256 hash;
    hash.update(packageBytes.data);
    const auto digest = hash.finish();
    std::copy(digest.begin(), digest.end(), packageBytes.data.begin() + 32);
    source.bytes = std::move(packageBytes.data);
    Package package;
    CHECK(package.open(source.source()) == Error::ok);
    ResourceManager resources;
    resources.bind(&package);
    // More than 32 distinct sections must remain accessible after cache eviction.
    const auto firstHandle = resources.handle(4);
    for (const auto &s : sections) {
        Entry e;
        CHECK(resources.find(s.id, e) == Error::ok);
    }
    std::array<uint8_t, 4> header{};
    CHECK(resources.read(firstHandle, 0, header) == Error::ok);
    CHECK(u32(header.data()) == 0x46474c46);
    Entry e;
    CHECK(resources.find(5, e) == Error::ok);
    std::vector<uint16_t> pixels(8 * 8, 0xbeef);
    FlintRender::Canvas canvas(pixels.data(), 8, 8);
    FlintRender::Op op;
    op.kind = FlintRender::ResourceImage;
    op.extra = 5;
    op.w = op.h = 8;
    canvas.clip = {2, 3, 3, 2};
    source.reset();
    resources.image(canvas, op);
    CHECK(source.readBytes == 8 + 3 * 2 * 2 && source.reads == 3);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
            CHECK(
                pixels[y * 8 + x] == (x >= 2 && x < 5 && y >= 3 && y < 5 ? y * 8 + x + 1 : 0xbeef));
    // Clipping also respects negative image origins and the framebuffer edges.
    std::fill(pixels.begin(), pixels.end(), 0xbeef);
    canvas.clip = {-3, -3, 5, 5};
    op.x = op.y = -2;
    resources.image(canvas, op);
    CHECK(pixels[0] == 19 && pixels[9] == 28 && pixels[2] == 0xbeef);
    // Layout and centering must not read glyph bitmaps.
    source.reset();
    CHECK(resources.metrics(4, 'A').valid);
    CHECK(source.bitmapReads == 0);
    source.reset();
    CHECK(resources.metrics(4, 'A').advance == 3 && source.reads == 0);
    CHECK(resources.glyph(4, 'A').alpha != nullptr);
    CHECK(source.bitmapReads == 1 && source.readBytes == 6);
    FlintRender::resourceContext = &resources;
    FlintRender::resourceMetrics = [](void *p, unsigned f, uint32_t cp) {
        return static_cast<ResourceManager *>(p)->metrics(f, cp);
    };
    FlintRender::resourceGlyph = [](void *p, unsigned f, uint32_t cp) {
        return static_cast<ResourceManager *>(p)->glyph(f, cp);
    };
    FlintRender::Scene before;
    FlintRender::rect(before, 0, 0, 280, 240, 0);
    op = {};
    op.kind = FlintRender::Text;
    op.font = 4;
    op.x = 10;
    op.y = 10;
    op.extra = 1;
    resources.textBounds(op, reinterpret_cast<const uint8_t *>("A"), 1);
    before.add(op, "A", 1);
    auto after = before;
    after.data[0] = 'B';
    resources.textBounds(after.ops[1], after.data, 1);
    const auto dirty = FlintRender::dirty(before, after);
    CHECK(dirty.x == 8 && dirty.y == 7 && dirty.w == 2 && dirty.h == 3);
    source.reset();
    canvas.clip = {0, 0, 8, 8};
    canvas.label(4, 4, reinterpret_cast<const uint8_t *>("AAA"), 3, 0xffff, 4, true);
    CHECK(source.bitmapReads == 3); // One bitmap per drawn character, none for measuring.
    CHECK(FlintRender::textWidth(4, reinterpret_cast<const uint8_t *>("?"), 1) > 0);
    FlintRender::resourceContext = nullptr;
    FlintRender::resourceMetrics = nullptr;
    FlintRender::resourceGlyph = nullptr;
    resources.bind(&package);
    CHECK(resources.read(firstHandle, 0, header) == Error::stale_handle);
    source.reset();
    CHECK(resources.metrics(4, 'A').valid && source.reads > 0);
    std::cout << "Resource clipping, bounded cache eviction, glyph I/O and text bounds: passed\n";
}
