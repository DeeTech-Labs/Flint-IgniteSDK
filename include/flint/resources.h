#pragma once
#include "diagnostics.h"
#include "graphics/renderer.h"
#include "manifest.h"
#include "flim.h"
namespace flint {
struct ResourceHandle {
    uint32_t section{};
    uint64_t generation{};
};
class ResourceManager {
    Package *package_{};
    uint64_t generation_{};
    // Glyph alpha is consumed synchronously before the next image/glyph operation.
    // Reuse it for the image palette and row: no extra image pixel buffers.
    std::array<uint8_t, 4096> glyph_{};
    FlimDecoder decoder_{std::span(glyph_).first<FlimDecoder::ScratchBytes>()};
    mutable std::array<Package::View, 32> views_{};
    mutable size_t nextView_{};
    struct GlyphInfo {
        uint32_t font{}, codepoint{}, offset{}, size{};
        FlintRender::GlyphMetrics metrics{};
    };
    std::array<GlyphInfo, 16> metrics_{};
    size_t nextMetric_{};
    std::function<uint64_t()> clock_;
    Timing readTiming_;
    uint64_t readBytes_{}, readErrors_{};
    GlyphInfo lookup(unsigned font, uint32_t codepoint);
    Error view(uint32_t id, Package::View &result) const;

public:
    ResourceManager() = default;
    ResourceManager(const ResourceManager &) = delete;
    ResourceManager &operator=(const ResourceManager &) = delete;
    FlimDecoder &decoder() { return decoder_; }
    void profilingClock(std::function<uint64_t()> clock) {
        clock_ = std::move(clock);
    }
    const Timing &readTiming() const {
        return readTiming_;
    }
    uint64_t readBytes() const {
        return readBytes_;
    }
    uint64_t readErrors() const {
        return readErrors_;
    }
    void bind(Package *package) {
        package_ = package;
        views_ = {};
        metrics_ = {};
        nextView_ = nextMetric_ = 0;
        ++generation_;
    }
    ResourceHandle handle(uint32_t id) const {
        return {id, generation_};
    }
    Error read(ResourceHandle handle, uint32_t offset, std::span<uint8_t> bytes);
    Error find(uint32_t id, Entry &entry) const;
    void image(FlintRender::Canvas &canvas, const FlintRender::Op &op);
    FlintRender::GlyphMetrics metrics(unsigned font, uint32_t codepoint);
    void textBounds(FlintRender::Op &op, const uint8_t *text, size_t length);
    FlintRender::Glyph glyph(unsigned font, uint32_t codepoint);
};
} // namespace flint
