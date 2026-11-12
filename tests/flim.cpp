#include <chrono>
#include <cstdlib>
#include <flint/binary.h>
#include <flint/flim.h>
#include <flint/icons.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace flint;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
struct Input {
    Bytes bytes;
    size_t reads{}, readBytes{};
    Source source() {
        return {this, uint32_t(bytes.size()),
            [](void *context, uint32_t offset, std::span<uint8_t> out) {
                auto &s = *static_cast<Input *>(context);
                if (offset > s.bytes.size() || out.size() > s.bytes.size() - offset)
                    return false;
                ++s.reads;
                s.readBytes += out.size();
                std::copy_n(s.bytes.data() + offset, out.size(), out.data());
                return true;
            }};
    }
};
static Bytes header(unsigned format, unsigned w, unsigned h, unsigned colors) {
    Writer writer;
    writer.n(0x4d494c46);
    writer.n(2, 1);
    writer.n(format, 1);
    writer.n(w, 2);
    writer.n(h, 2);
    writer.n(colors, 2);
    writer.n(0);
    return writer.data;
}
static uint16_t blend(uint16_t c, uint16_t bg, unsigned a) {
    const auto channel = [=](unsigned shift, unsigned mask) {
        return ((((c >> shift) & mask) * a + ((bg >> shift) & mask) * (255 - a) + 127) / 255)
            << shift;
    };
    return uint16_t(channel(11, 31) | channel(5, 63) | channel(0, 31));
}
static void exercise(Input input, std::span<const unsigned> alpha) {
    std::array<uint8_t, FlimDecoder::ScratchBytes> scratch{};
    FlimDecoder decoder(scratch);
    CHECK(decoder.open(input.source()) == Error::ok && decoder.validate() == Error::ok);
    const auto original = input.bytes;
    FlintRender::Op op;
    op.w = 3;
    op.h = 2;
    op.x = 1;
    op.y = 1;
    op.color = 0xf800;
    for (bool swapped : {false, true}) {
        for (int cy = -1; cy < 5; ++cy)
            for (int cx = -1; cx < 6; ++cx) {
                std::array<uint16_t, 30> pixels;
                const uint16_t background = 0x17e3;
                pixels.fill(swapped ? FlintRender::swap(background) : background);
                FlintRender::Canvas canvas(pixels.data(), 6, 5, swapped);
                canvas.clip = {cx, cy, 2, 2};
                CHECK(decoder.draw(canvas, op) == Error::ok);
                for (int y = 0; y < 5; ++y)
                    for (int x = 0; x < 6; ++x) {
                        const bool inside = x >= 1 && x < 4 && y >= 1 && y < 3 && x >= cx &&
                            x < cx + 2 && y >= cy && y < cy + 2;
                        CHECK(canvas.get(x, y) ==
                            (inside ? blend(op.color, background, alpha[(y - 1) * 3 + x - 1])
                                    : background));
                    }
            }
    }
    // Every truncation, trailing byte, unsupported version/type/reserved value fails.
    for (size_t n = 0; n < original.size(); ++n) {
        input.bytes.assign(original.begin(), original.begin() + n);
        CHECK(decoder.open(input.source()) != Error::ok);
    }
    for (unsigned offset : {0u, 4u, 5u, 12u}) {
        input.bytes = original;
        input.bytes[offset] = 255;
        CHECK(decoder.open(input.source()) != Error::ok);
    }
    input.bytes = original;
    input.bytes.push_back(0);
    CHECK(decoder.open(input.source()) != Error::ok);
    for (unsigned dimension : {0u, 321u, 65535u}) {
        input.bytes = original;
        input.bytes[6] = uint8_t(dimension);
        input.bytes[7] = uint8_t(dimension >> 8);
        CHECK(decoder.open(input.source()) != Error::ok);
    }
}
int main(int argc, char **argv) {
    const std::array<unsigned, 6> alpha{0, 17, 255, 136, 255, 0};
    Input mask{header(1, 3, 2, 0)};
    mask.bytes.insert(mask.bytes.end(), {0x01, 0xf0, 0x8f, 0x00});
    exercise(mask, alpha);
    std::array<uint8_t, FlimDecoder::ScratchBytes> scratch{};
    FlimDecoder decoder(scratch);
    mask.bytes.back() = 1;
    CHECK(decoder.open(mask.source()) == Error::ok && decoder.validate() == Error::invalid_package);
    for (unsigned count : {1u, 2u, 6u, 16u, 255u, 256u}) {
        Input indexed{header(2, 3, 2, count)};
        for (unsigned i = 0; i < count; ++i)
            indexed.bytes.insert(indexed.bytes.end(), {0, 0xf8, uint8_t(alpha[i % 6])});
        std::array<unsigned, 6> expected{};
        for (unsigned i = 0; i < 6; ++i) {
            indexed.bytes.push_back(uint8_t(i % count));
            expected[i] = alpha[(i % count) % 6];
        }
        exercise(indexed, expected);
        if (count < 256) {
            indexed.bytes.back() = uint8_t(count);
            CHECK(decoder.open(indexed.source()) == Error::ok &&
                decoder.validate() == Error::invalid_package);
        }
    }
    Input rgb{header(3, 3, 2, 0)};
    for (auto a : alpha)
        rgb.bytes.insert(rgb.bytes.end(), {0, 0xf8, uint8_t(a)});
    exercise(rgb, alpha);
    CHECK(decoder.open(rgb.source()) == Error::ok);
    rgb.reads = rgb.readBytes = 0;
    std::array<uint16_t, 6> pixels{};
    FlintRender::Canvas canvas(pixels.data(), 3, 2);
    canvas.clip = {1, 0, 1, 1};
    FlintRender::Op op;
    op.w = 3;
    op.h = 2;
    CHECK(decoder.draw(canvas, op) == Error::ok && rgb.reads == 1 && rgb.readBytes == 3);
    Input maximum{header(3, 320, 320, 0)};
    for (unsigned i = 0; i < 320 * 320; ++i)
        maximum.bytes.insert(maximum.bytes.end(), {0, 0xf8, 255});
    CHECK(decoder.open(maximum.source()) == Error::ok && decoder.validate() == Error::ok);
    op.w = op.h = 320;
    CHECK(decoder.draw(canvas, op) == Error::ok && canvas.get(1, 0) == 0xf800);
    IconResources icons{10, 11, 12, 13};
    CHECK(selectIcon(icons, false, false) == 12 && selectIcon(icons, false, true) == 13);
    CHECK(selectIcon(icons, true, false) == 11 && selectIcon(icons, true, true) == 11);
    icons.mask = icons.light = icons.dark = 0;
    CHECK(selectIcon(icons, true, true) == 10 && selectIcon(icons, false, false) == 10);
    FlintRender::Scene before, after;
    FlintRender::rect(before, 0, 0, 280, 240, 0);
    after = before;
    after.resourceRevision = 1;
    CHECK(FlintRender::dirty(before, after).w == 280);
    std::cout << "decoder_bytes=" << sizeof(FlimDecoder)
              << " icon_cache_bytes=" << sizeof(IconCache) << '\n';
    // Optional prepared fixtures: benchmark real file reads separately from memory tests.
    for (int arg = 1; arg < argc; ++arg) {
        File file;
        CHECK(file.open(argv[arg], false));
        auto source = file.source();
        std::array<uint16_t, IconSize * IconSize> target{};
        FlintRender::Canvas output(target.data(), IconSize, IconSize);
        FlintRender::Op image;
        image.w = image.h = IconSize;
        image.color = 0x8f3d;
        constexpr unsigned iterations = 200;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < iterations; ++i) {
            CHECK(decoder.open(source) == Error::ok && decoder.validate() == Error::ok);
        }
        const auto loaded = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < iterations; ++i) {
            CHECK(decoder.open(source) == Error::ok && decoder.draw(output, image) == Error::ok);
        }
        const auto end = std::chrono::steady_clock::now();
        const auto us = [](auto duration) {
            return std::chrono::duration<double, std::micro>(duration).count() / iterations;
        };
        std::cout << argv[arg] << " bytes=" << file.size << " validate_us=" << us(loaded - start)
                  << " draw_us=" << us(end - loaded) << '\n';
    }
}
