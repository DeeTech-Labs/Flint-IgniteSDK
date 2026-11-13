#pragma once
#include "binary.h"
#include "graphics/scene.h"
namespace flint {
inline constexpr uint32_t MaxManifestBytes = 4096;
enum class Role : uint8_t { none, clock, launcher, settings, theme, locale };
struct Declaration {
    std::string permission, scope;
};
struct ComponentInfo {
    std::string id;
    Component type{Component::app};
    Role role{};
    uint32_t section{};
    uint32_t logicSection{};
    MemoryBudget budget{};
    uint64_t capabilities{};
    std::vector<Declaration> permissions;
};
struct IconResources {
    uint32_t main{}, mask{}, light{}, dark{};
};
inline uint32_t selectIcon(const IconResources &icons, bool themed, bool dark) {
    return themed ? (icons.mask ? icons.mask : icons.main)
                  : (dark ? (icons.dark ? icons.dark : icons.main)
                          : (icons.light ? icons.light : icons.main));
}
struct Manifest {
    std::string id, name, publisher;
    uint32_t version{};
    IconResources icons;
    MemoryBudget budget{};
    uint64_t capabilities{};
    std::vector<Declaration> permissions;
    std::vector<ComponentInfo> components;
};
Error loadManifest(Package &package, Manifest &manifest);
Error validateApplication(
    Package &package, Manifest &manifest, MemoryBudget available, uint64_t capabilities);
class ApplicationValidation {
    Package *package_{};
    Manifest *manifest_{};
    uint64_t generation_{}, capabilities_{};
    uint32_t index_{}, offset_{};
    Entry entry_{};
    Bytes content_;
    Error error_{Error::invalid_state};
    bool done_{};
    Error fail(Error error) {
        error_ = error;
        done_ = true;
        return error;
    }

public:
    Error begin(
        Package &package, Manifest &manifest, MemoryBudget available, uint64_t capabilities);
    Error advance(uint32_t byteBudget);
    bool done() const {
        return done_;
    }
};
} // namespace flint
