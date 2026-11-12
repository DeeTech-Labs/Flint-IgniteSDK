#include <flint/flim.h>

namespace flint {
bool FlimDecoder::read(uint32_t offset, std::span<uint8_t> bytes) {
    return source_.read && offset <= source_.size && bytes.size() <= source_.size - offset &&
        source_.read(source_.context, offset, bytes);
}
Error FlimDecoder::open(Source source) {
    source_ = source;
    info_ = {};
    std::array<uint8_t, FlimHeaderSize> h{};
    if (!read(0, h))
        return Error::invalid_package;
    const auto format = ImageFormat(h[5]);
    const auto w = u16(h.data() + 6), height = u16(h.data() + 8), n = u16(h.data() + 10);
    if (u32(h.data()) != 0x4d494c46 || h[4] != 2 || u32(h.data() + 12) || !w || !height ||
        w > MaxImageDimension || height > MaxImageDimension ||
        (format != ImageFormat::a4 && format != ImageFormat::index8 &&
            format != ImageFormat::rgb565a8) ||
        (format == ImageFormat::index8 ? !n || n > 256 : n != 0))
        return Error::invalid_package;
    const uint32_t stride = format == ImageFormat::a4 ? (w + 1u) / 2
        : format == ImageFormat::index8               ? w
                                                      : w * 3u;
    const uint32_t offset = FlimHeaderSize + n * 3u;
    // Bounded dimensions make all arithmetic below fit uint32_t.
    if (source.size != offset + stride * height)
        return Error::invalid_package;
    if (n && !read(FlimHeaderSize, std::span(palette_).first(n * 3u)))
        return Error::io_error;
    info_ = {format, w, height, n, stride, offset};
    return Error::ok;
}
Error FlimDecoder::validate() {
    if (!info_.width)
        return Error::invalid_state;
    for (unsigned y = 0; y < info_.height; ++y) {
        if (!read(info_.dataOffset + y * info_.stride, std::span(row_).first(info_.stride)))
            return Error::io_error;
        if (info_.format == ImageFormat::index8)
            for (unsigned x = 0; x < info_.width; ++x)
                if (row_[x] >= info_.paletteCount)
                    return Error::invalid_package;
        if (info_.format == ImageFormat::a4 && (info_.width & 1) && (row_[info_.stride - 1] & 15))
            return Error::invalid_package;
    }
    return Error::ok;
}
Error FlimDecoder::draw(FlintRender::Canvas &c, const FlintRender::Op &op) {
    if (!info_.width || op.w != info_.width || op.h != info_.height)
        return Error::invalid_argument;
    const int left = std::max({0, c.clip.x, int(op.x)});
    const int right = std::min({c.width, c.clip.x + c.clip.w, op.x + int(info_.width)});
    const int top = std::max({0, c.clip.y, int(op.y)});
    const int bottom = std::min({c.height, c.clip.y + c.clip.h, op.y + int(info_.height)});
    if (left >= right || top >= bottom)
        return Error::ok;
    const unsigned first = unsigned(left - op.x), last = unsigned(right - op.x);
    const bool mask = info_.format == ImageFormat::a4,
               indexed = info_.format == ImageFormat::index8;
    const unsigned begin = mask ? first / 2 : indexed ? first : first * 3;
    const unsigned end = mask ? (last + 1) / 2 : indexed ? last : last * 3;
    for (int y = top; y < bottom; ++y) {
        if (!read(info_.dataOffset + (y - op.y) * info_.stride + begin,
                std::span(row_).first(end - begin)))
            return Error::io_error;
        if (indexed)
            for (unsigned i = 0; i < end - begin; ++i)
                if (row_[i] >= info_.paletteCount)
                    return Error::invalid_package;
        if (mask && (info_.width & 1) && last == info_.width && (row_[end - begin - 1] & 15))
            return Error::invalid_package;
        for (int x = left; x < right; ++x) {
            const unsigned sx = unsigned(x - op.x);
            uint16_t color = op.color;
            unsigned alpha;
            if (mask)
                alpha = ((row_[sx / 2 - begin] >> ((sx & 1) ? 0 : 4)) & 15) * 17;
            else {
                const auto *p = indexed ? palette_.data() + row_[sx - begin] * 3u
                                        : row_.data() + sx * 3 - begin;
                color = u16(p);
                alpha = p[2];
            }
            if (!alpha)
                continue;
            if (alpha != 255) {
                const auto bg = c.get(x, y);
                const auto blend = [=](unsigned shift, unsigned bits) {
                    return ((((color >> shift) & bits) * alpha +
                                ((bg >> shift) & bits) * (255 - alpha) + 127) /
                               255)
                        << shift;
                };
                color = uint16_t(blend(11, 31) | blend(5, 63) | blend(0, 31));
            }
            c.pixel(x, y, color);
        }
    }
    return Error::ok;
}
} // namespace flint
