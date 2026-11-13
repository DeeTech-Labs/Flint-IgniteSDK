#include <flint/icons.h>
#include <flint/resources.h>
namespace flint {
Error ResourceManager::view(uint32_t id, Package::View &result) const {
    if (!package_)
        return Error::invalid_state;
    for (const auto &cached : views_) {
        if (cached.info().id == id) {
            result = cached;
            return Error::ok;
        }
    }
    for (auto &cached : views_) {
        if (!cached.info().id) {
            auto error = package_->view(id, cached);
            if (error == Error::ok)
                result = cached;
            return error;
        }
    }
    Package::View section;
    const auto error = package_->view(id, section);
    if (error == Error::ok) {
        views_[nextView_] = section;
        nextView_ = (nextView_ + 1) % views_.size();
        result = section;
    }
    return error;
}
Error ResourceManager::find(uint32_t id, Entry &out) const {
    Package::View section;
    const auto error = view(id, section);
    if (error == Error::ok)
        out = section.info();
    return error;
}
Error ResourceManager::read(ResourceHandle handle, uint32_t offset, std::span<uint8_t> bytes) {
    TimingScope timing(clock_, readTiming_);
    if (handle.generation != generation_) {
        ++readErrors_;
        return Error::stale_handle;
    }
    Package::View section;
    const auto error = view(handle.section, section);
    const auto result = error == Error::ok ? section.read(offset, bytes) : error;
    if (result == Error::ok)
        readBytes_ += bytes.size();
    else
        ++readErrors_;
    return result;
}
ResourceManager::GlyphInfo ResourceManager::lookup(unsigned font, uint32_t cp) {
    for (const auto &cached : metrics_)
        if (cached.metrics.valid && cached.font == font && cached.codepoint == cp)
            return cached;
    Entry e;
    if (find(font, e) != Error::ok || e.type != Section::resource || e.size < 12)
        return {};
    std::array<uint8_t, 24> b{};
    auto h = handle(font);
    if (read(h, 0, std::span(b).first(12)) != Error::ok || u32(b.data()) != 0x46474c46)
        return {};
    auto count = u16(b.data() + 4);
    if (!count || count > 4096 || uint32_t(count) * 24 > e.size - 12)
        return {};
    unsigned low = 0, high = count;
    while (low < high) {
        unsigned mid = (low + high) / 2;
        if (read(h, 12 + mid * 24, b) != Error::ok)
            return {};
        auto code = u32(b.data());
        if (code < cp)
            low = mid + 1;
        else if (code > cp)
            high = mid;
        else {
            auto width = u16(b.data() + 4), height = u16(b.data() + 6),
                 advance = u16(b.data() + 12);
            auto offset = u32(b.data() + 16), size = u32(b.data() + 20);
            if (width > 64 || height > 64 || advance > 128 || size != uint32_t(width) * height ||
                offset < 12 + count * 24u || offset > e.size || size > e.size - offset)
                return {};
            GlyphInfo info{font, cp, offset, size,
                {int(width), int(height), int(advance), int(int16_t(u16(b.data() + 10))),
                    int(int16_t(u16(b.data() + 8))), true}};
            metrics_[nextMetric_] = info;
            nextMetric_ = (nextMetric_ + 1) % metrics_.size();
            return info;
        }
    }
    return {};
}
FlintRender::GlyphMetrics ResourceManager::metrics(unsigned font, uint32_t cp) {
    return lookup(font, cp).metrics;
}
FlintRender::Glyph ResourceManager::glyph(unsigned font, uint32_t cp) {
    const auto info = lookup(font, cp);
    if (!info.metrics.valid ||
        read(handle(font), info.offset, std::span(glyph_).first(info.size)) != Error::ok)
        return {};
    const auto &m = info.metrics;
    return {m.width, m.height, m.advance, m.ascent, m.left, glyph_.data()};
}
void ResourceManager::textBounds(FlintRender::Op &op, const uint8_t *text, size_t length) {
    int advance = 0, left = 0, right = 0, top = 0, bottom = 0;
    size_t cursor = 0;
    while (cursor < length) {
        const auto cp = FlintRender::nextCodepoint(text, length, cursor);
        auto m = metrics(op.font, cp);
        if (!m.valid) {
            const auto g = FlintRender::glyph(FlintRender::Body, '?');
            m = {g.width, g.height, g.advance, g.ascent, g.left, true};
        }
        if (m.width && m.height) {
            left = std::min(left, advance + m.left);
            right = std::max(right, advance + m.left + m.width);
            top = std::min(top, -m.ascent);
            bottom = std::max(bottom, -m.ascent + m.height);
        }
        advance += m.advance;
    }
    // Only on-screen ink matters. Clamp far-off font bearings before narrowing;
    // scene coordinates are bounded to [-320,640] and screens to 320 pixels.
    const int shift = (op.extra & FlintRender::TextCentered) ? advance / 2 : 0;
    left = std::clamp(left - shift, -1024, 1024);
    right = std::clamp(right - shift, -1024, 1024);
    top = std::clamp(top, -1024, 1024);
    bottom = std::clamp(bottom, -1024, 1024);
    op.inkX = int16_t(left);
    op.inkY = int16_t(top);
    op.inkW = int16_t(right - left);
    op.inkH = int16_t(bottom - top);
}
void ResourceManager::image(FlintRender::Canvas &canvas, const FlintRender::Op &op) {
    // A renderer bound only to the foreground package (e.g. remote Preview)
    // cannot resolve catalog indices as resource IDs in that package.
    if (op.kind == FlintRender::CatalogIcon) {
        drawDefaultIcon(canvas, op);
        return;
    }
    Package::View section;
    if (view(op.extra, section) != Error::ok || section.info().type != Section::resource)
        return;
    struct ReadContext {
        ResourceManager &owner;
        Package::View &section;
    } context{*this, section};
    const Source source{&context, section.info().size,
        [](void *pointer, uint32_t offset, std::span<uint8_t> bytes) {
            auto &read = *static_cast<ReadContext *>(pointer);
            TimingScope timing(read.owner.clock_, read.owner.readTiming_);
            const auto error = read.section.read(offset, bytes);
            if (error == Error::ok)
                read.owner.readBytes_ += bytes.size();
            else
                ++read.owner.readErrors_;
            return error == Error::ok;
        }};
    if (decoder_.open(source) == Error::ok)
        decoder_.draw(canvas, op);
    decoder_.close();
}

} // namespace flint
