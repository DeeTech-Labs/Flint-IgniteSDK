#include <flint/icons.h>
namespace flint {
void drawDefaultIcon(FlintRender::Canvas &canvas, const FlintRender::Op &op) {
    canvas.fill(op.x, op.y, op.w, op.h, op.color);
    const uint16_t ink = ((op.color >> 5) & 63) > 32 ? 0 : 0xffff;
    for (int y = 0; y < 2; ++y)
        for (int x = 0; x < 2; ++x)
            canvas.fill(op.x + 10 + x * 16, op.y + 10 + y * 16, 12, 12, ink, 2);
}
void IconCache::clear() {
    ++generation_;
    for (auto &slot : slots_) {
        slot.package.close();
        slot.file.close();
        slot.view = {};
        slot.index = UINT32_MAX;
        slot.valid = false;
    }
    next_ = 0;
    revision_ = UINT64_MAX;
}
void IconCache::synchronize(uint64_t revision, uint64_t owner, bool themed, bool dark) {
    const unsigned mode = unsigned(themed) | (unsigned(dark) << 1);
    if (revision_ != revision || mode_ != mode || owner_ != owner) {
        clear();
        revision_ = revision;
        mode_ = mode;
        owner_ = owner;
    }
}
bool IconCache::draw(PackageManager &packages, FlimDecoder &decoder, FlintRender::Canvas &canvas,
    const FlintRender::Op &op, uint64_t owner, bool themed, bool dark) {
    synchronize(packages.revision(), owner, themed, dark);
    const uint32_t index = uint32_t(op.extra) - 1;
    Slot *found = nullptr;
    for (auto &slot : slots_)
        if (slot.index == index) {
            found = &slot;
            break;
        }
    if (!found) {
        found = &slots_[next_];
        next_ = (next_ + 1) % slots_.size();
        found->package.close();
        found->file.close();
        found->view = {};
        found->index = index;
        found->valid = false;
        uint32_t current = 0;
        packages.visit([&](const Installed &installed) {
            for (const auto &component : installed.manifest.components) {
                if (!launcherApp(installed, component))
                    continue;
                if (current++ != index)
                    continue;
                const auto id = selectIcon(installed.manifest.icons, themed, dark);
                if (!id || !found->file.open(installed.path, false) ||
                    found->package.open(found->file.source()) != Error::ok ||
                    found->package.view(id, found->view) != Error::ok ||
                    found->view.info().type != Section::resource ||
                    decoder.open(imageSource(found->view)) != Error::ok)
                    return false;
                const auto &info = decoder.info();
                const bool mask = themed && installed.manifest.icons.mask;
                found->valid = info.width == IconSize && info.height == IconSize &&
                    (mask ? info.format == ImageFormat::a4 : info.format != ImageFormat::a4) &&
                    decoder.validate() == Error::ok && found->file.unchanged();
                return false;
            }
            return true;
        });
        if (!found->valid) {
            found->package.close();
            found->file.close();
        }
    }
    const bool drawn = found->valid && found->file.unchanged() &&
        decoder.open(imageSource(found->view)) == Error::ok &&
        decoder.draw(canvas, op) == Error::ok;
    decoder.close();
    if (drawn)
        return false;
    const bool failed = found->valid;
    found->valid = false;
    found->package.close();
    found->file.close();
    // Opaque standard app tile also covers any partially rendered resource after an I/O error.
    drawDefaultIcon(canvas, op);
    if (failed)
        ++generation_;
    return failed;
}
} // namespace flint
