#include <array>
#include <cstdlib>
#include <flint/graphics/renderer.h>
#include <iostream>
#include <vector>

using namespace FlintRender;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)

static Scene boxed(const char *value, uint16_t flags, int width = 85, int height = 90) {
    Scene s;
    rect(s, 0, 0, 280, 240, 0);
    Op op;
    op.kind = Text;
    op.x = 15;
    op.y = 13;
    op.w = width;
    op.h = height;
    op.color = 0xffff;
    op.extra = TextBox | flags;
    CHECK(s.add(op, value, strlen(value)));
    return s;
}
static std::vector<uint16_t> paint(const Scene &s) {
    std::vector<uint16_t> pixels(280 * 240);
    Canvas canvas(pixels.data(), 280, 240);
    render(s, canvas);
    CHECK(canvas.clip.x == 0 && canvas.clip.y == 0 && canvas.clip.w == 280 && canvas.clip.h == 240);
    return pixels;
}
static void contained(const Scene &s, const std::vector<uint16_t> &pixels) {
    const auto &b = s.ops[1];
    size_t ink = 0;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 280; ++x)
            if (pixels[y * 280 + x]) {
                CHECK(x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h);
                ++ink;
            }
    CHECK(ink > 0);
}
int main() {
    const int lineHeight = be32(fontData(Body) + 16) + be32(fontData(Body) + 20);
    for (auto flags : {uint16_t(0), uint16_t(TextWrap), uint16_t(TextEllipsis),
             uint16_t(TextWrap | TextCentered)}) {
        auto s = boxed("Long words: temperature humidity unavailable", flags, 85, 3 * lineHeight);
        auto pixels = paint(s);
        contained(s, pixels);
        if (flags & TextWrap) {
            bool secondLine = false;
            for (int y = s.ops[1].y + lineHeight; y < s.ops[1].y + 2 * lineHeight; ++y)
                for (int x = 15; x < 100; ++x)
                    secondLine |= pixels[y * 280 + x] != 0;
            CHECK(secondLine);
        }
        std::array<uint8_t, MaxEncoded> wire{};
        auto size = encode(s, wire.data(), wire.size());
        Scene decoded;
        CHECK(size && decode(wire.data(), size, decoded));
        CHECK(paint(decoded) == pixels);
    }
    for (auto value :
        {"\xd0\x94\xd0\xb0\xd1\x82\xd1\x87\xd0\xb8\xd0\xba\xd0\xb8 "
         "\xd0\xbd\xd0\xb5\xd0\xb4\xd0\xbe\xd1\x81\xd1\x82\xd1\x83\xd0\xbf\xd0\xbd\xd1\x8b",
            "abcdefghijklmnopqrstuvwxyz", "abc\n\ndef", "  x  y  "}) {
        auto s = boxed(value, TextWrap, 37, 3 * lineHeight);
        contained(s, paint(s));
    }
    // A one-pixel box, wide glyphs and off-screen boxes must remain bounded.
    auto tiny = boxed("MMMM", TextWrap, 1, lineHeight);
    (void)paint(tiny);
    auto off = boxed("clipped", TextWrap);
    off.ops[1].x = -12;
    off.ops[1].y = -8;
    (void)paint(off);
    // Clearing old multiline ink uses the whole explicit box, including vacated lines.
    auto old = boxed("many many many many words", TextWrap);
    auto next = boxed("short", TextWrap, 50, lineHeight);
    auto incremental = paint(old);
    Canvas canvas(incremental.data(), 280, 240);
    canvas.clip = dirty(old, next);
    render(next, canvas);
    CHECK(incremental == paint(next));
    // No opt-in: preserve baseline placement and free positioning exactly.
    Scene free;
    rect(free, 0, 0, 280, 240, 0);
    text(free, 140, 80, "unaltered", 0xffff, Body, true);
    auto direct = std::vector<uint16_t>(280 * 240);
    Canvas directCanvas(direct.data(), 280, 240);
    directCanvas.label(
        140, 80, reinterpret_cast<const uint8_t *>("unaltered"), 9, 0xffff, Body, true);
    CHECK(paint(free) == direct);
    for (auto flags :
        {uint16_t(TextWrap), uint16_t(TextBox | TextWrap | TextEllipsis), uint16_t(16)}) {
        auto invalid = boxed("invalid", 0);
        invalid.ops[1].extra = flags;
        std::array<uint8_t, MaxEncoded> wire{};
        auto size = encode(invalid, wire.data(), wire.size());
        Scene decoded;
        CHECK(!decode(wire.data(), size, decoded));
    }
    // Resource fonts use their own glyph metrics and the same clipping contract.
    static std::array<uint8_t, 8 * 13> alpha;
    alpha.fill(255);
    resourceMetrics = [](void *, unsigned, uint32_t cp) {
        if (cp == '\n')
            return GlyphMetrics{};
        return GlyphMetrics{8, 13, 9, 10, 0, true};
    };
    resourceGlyph = [](void *, unsigned, uint32_t) { return Glyph{8, 13, 9, 10, 0, alpha.data()}; };
    auto resource = boxed("resource font words", TextWrap, 60, 39);
    resource.ops[1].font = 5;
    contained(resource, paint(resource));
    auto newlines = boxed("a\nb\nc", TextWrap, 60, 39);
    newlines.ops[1].font = 5;
    const auto newlinePixels = paint(newlines);
    CHECK(std::count(newlinePixels.begin(), newlinePixels.end(), uint16_t(0xffff)) == 3 * 8 * 13);
    resourceGlyph = nullptr;
    resourceMetrics = nullptr;
    std::cout << "bounded opt-in text layout passed; line height=" << lineHeight << "\n";
}
