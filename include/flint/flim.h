#pragma once
#include "graphics/renderer.h"
#include "package.h"

namespace flint {
enum class ImageFormat : uint8_t { a4 = 1, index8 = 2, rgb565a8 = 3 };
inline constexpr unsigned IconSize = 48, FlimHeaderSize = 16, MaxImageDimension = 320;
struct ImageInfo {
    ImageFormat format{};
    uint16_t width{}, height{}, paletteCount{};
    uint32_t stride{}, dataOffset{};
};
// Owner-thread scratch shared between images; never retains decoded pixels.
class FlimDecoder {
    Source source_{};
    ImageInfo info_{};
    std::span<uint8_t, 768> palette_;
    std::span<uint8_t, 960> row_;
    bool read(uint32_t offset, std::span<uint8_t> bytes);

public:
    static constexpr size_t ScratchBytes = 768 + 960;
    explicit FlimDecoder(std::span<uint8_t, ScratchBytes> scratch)
        : palette_(scratch.first<768>()), row_(scratch.last<960>()) {}
    Error open(Source source);
    void close() {
        source_ = {};
        info_ = {};
    }
    Error validate();
    const ImageInfo &info() const {
        return info_;
    }
    Error draw(FlintRender::Canvas &canvas, const FlintRender::Op &op);
};
inline Source imageSource(Package::View &view) {
    return {&view, view.info().size, [](void *p, uint32_t offset, std::span<uint8_t> bytes) {
                return static_cast<Package::View *>(p)->read(offset, bytes) == Error::ok;
            }};
}
} // namespace flint
