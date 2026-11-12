#pragma once
#include "flim.h"
#include "storage.h"
namespace flint {
void drawDefaultIcon(FlintRender::Canvas &canvas, const FlintRender::Op &op);
inline bool launcherApp(const Installed &installed, const ComponentInfo &component) {
    return (component.type == Component::app || component.type == Component::watchface) &&
        component.role != Role::launcher && installed.manifest.id != "system.boot" &&
        installed.manifest.id != "system.operations" && installed.manifest.id != "system.status";
}
// Six visible/recycled launcher cells. No decoded images or whole packages are cached.
class IconCache {
    struct Slot {
        File file;
        Package package;
        Package::View view;
        uint32_t index{UINT32_MAX};
        bool valid{};
    };
    std::array<Slot, 6> slots_;
    uint64_t revision_{UINT64_MAX}, owner_{}, generation_{};
    unsigned mode_{}, next_{};

public:
    void clear();
    void synchronize(uint64_t revision, uint64_t owner, bool themed, bool dark);
    uint64_t generation() const {
        return generation_;
    }
    // True if a previously usable resource failed: request another scene so
    // its new generation forces a complete redraw after a clipped fallback.
    bool draw(PackageManager &packages, FlimDecoder &decoder, FlintRender::Canvas &canvas,
        const FlintRender::Op &op, uint64_t owner, bool themed, bool dark);
};
} // namespace flint
