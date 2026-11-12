#include <flint/lua_app.h>
#include <flint/manifest.h>
#include <flint/registry.h>
#include <set>
namespace flint {
Error loadManifest(Package &package, Manifest &out) {
    Entry manifestEntry{};
    const auto manifestError = package.manifest(manifestEntry);
    if (manifestError != Error::ok)
        return manifestError;
    if (!manifestEntry.size || manifestEntry.size > MaxManifestBytes)
        return Error::invalid_package;
    Bytes bytes(manifestEntry.size);
    if (package.read(manifestEntry, 0, bytes) != Error::ok)
        return Error::io_error;
    Reader r{bytes};
    if (r.n() != 0x324d4c46)
        return Error::invalid_package;
    Manifest m;
    m.id = r.text(64);
    m.name = r.text(96);
    m.publisher = r.text(64);
    m.version = r.n();
    m.budget = {uint32_t(r.n()), uint32_t(r.n()), uint32_t(r.n())};
    m.capabilities = r.n(8);
    if (!identifier(m.id) || !identifier(m.publisher) || m.name.empty() || !appText(m.name) ||
        std::any_of(m.name.begin(), m.name.end(), [](unsigned char c) { return c < 32 || c == 127; }) || !m.version)
        return Error::invalid_package;
    const auto count = r.n(2);
    if (count > 64)
        return Error::invalid_package;
    std::set<std::string> declarations;
    for (unsigned i = 0; i < count; ++i) {
        Declaration d{r.text(80), r.text(192)};
        if (!identifier(d.permission) || !declarations.insert(d.permission + "\n" + d.scope).second)
            return Error::invalid_package;
        if (!permission(d.permission)) {
            return Error::not_supported;
        }
        m.permissions.push_back(std::move(d));
    }
    const auto components = r.n(2);
    if (!components || components > 32)
        return Error::invalid_package;
    std::set<std::string> ids;
    for (unsigned i = 0; i < components; ++i) {
        ComponentInfo c;
        c.id = r.text(64);
        c.type = Component(r.n(2));
        c.role = Role(r.n(1));
        c.section = r.n();
        if (!identifier(c.id) || !ids.insert(c.id).second || uint16_t(c.type) < 1 ||
            uint16_t(c.type) > 8 || uint8_t(c.role) > 5)
            return Error::invalid_package;
        Package::View section;
        if (package.view(c.section, section) != Error::ok ||
            section.info().type == Section::manifest || section.info().type == Section::signature)
            return Error::invalid_package;
        m.components.push_back(std::move(c));
    }
    m.icons = {uint32_t(r.n()), uint32_t(r.n()), uint32_t(r.n()), uint32_t(r.n())};
    if (!m.icons.main && (m.icons.mask || m.icons.light || m.icons.dark))
        return Error::invalid_package;
    for (const auto id : {m.icons.main, m.icons.mask, m.icons.light, m.icons.dark}) {
        if (!id) continue;
        Package::View section;
        if (package.view(id, section) != Error::ok || section.info().type != Section::resource)
            return Error::invalid_package;
    }
    if (!r.done())
        return Error::invalid_package;
    out = std::move(m);
    return Error::ok;
}
Error ApplicationValidation::begin(
    Package &package, Manifest &m, MemoryBudget available, uint64_t caps) {
    *this = {};
    package_ = &package;
    manifest_ = &m;
    generation_ = package.generation_;
    capabilities_ = caps;
    auto error = loadManifest(package, m);
    if (error != Error::ok)
        return fail(error);
    if ((m.capabilities & caps) != m.capabilities)
        return fail(Error::not_supported);
    bool hasUi = false;
    for (const auto &component : m.components) {
        hasUi = hasUi || component.type == Component::app ||
            component.type == Component::watchface || component.type == Component::widget ||
            component.type == Component::complication;
        if (component.type == Component::service)
            return fail(Error::not_supported);
    }
    if (hasUi && m.budget.internal_bytes < 16 * 1024)
        return fail(Error::quota_exceeded);
    if (!fits(m.budget, available))
        return fail(Error::resource_exhausted);
    error_ = Error::ok;
    return Error::ok;
}
Error ApplicationValidation::advance(uint32_t budget) {
    if (done_)
        return error_;
    if (!package_ || !manifest_ || !budget)
        return Error::invalid_argument;
    if (!package_->valid_ || generation_ != package_->generation_)
        return fail(Error::stale_handle);
    if (content_.empty()) {
        if (index_ == package_->count()) {
            Bytes{}.swap(content_);
            done_ = true;
            return Error::ok;
        }
        const auto error = package_->entry(index_++, entry_);
        if (error != Error::ok)
            return fail(error);
        [[maybe_unused]] const auto &m = *manifest_;
        if (entry_.type == Section::lua) {
#ifndef FLINT_ENABLE_LUA
            return fail(Error::not_supported);
#else
            if (++luaCount_ > 1 || m.components.size() != 1 ||
                (m.components[0].type != Component::app &&
                    m.components[0].type != Component::watchface) ||
                !(m.capabilities & CapLua) || !(capabilities_ & CapLua))
                return fail(Error::not_supported);
            if (entry_.size > MaxLuaSourceBytes || m.budget.internal_bytes < 48 * 1024 ||
                m.budget.internal_bytes > 144 * 1024 || !m.budget.instructions ||
                m.budget.instructions > 100000)
                return fail(Error::quota_exceeded);
#endif
        } else if (entry_.type == Section::ui) {
            if (entry_.size > FlintRender::MaxEncoded)
                return fail(Error::quota_exceeded);
        } else {
            return Error::ok;
        }
        content_.resize(entry_.size);
        offset_ = 0;
    }
    const auto count = std::min<uint32_t>(budget, entry_.size - offset_);
    Package::View section;
    auto error = package_->view(entry_.id, section);
    if (error == Error::ok)
        error = section.read(offset_, std::span(content_).subspan(offset_, count));
    if (error != Error::ok)
        return fail(error);
    offset_ += count;
    if (offset_ != entry_.size)
        return Error::ok;
    if (entry_.type == Section::lua) {
        if (content_[0] == 0x1b || std::find(content_.begin(), content_.end(), 0) != content_.end())
            return fail(Error::invalid_package);
    } else {
        FlintRender::Scene scene;
        if (!FlintRender::decode(content_.data(), content_.size(), scene) || scene.width != 280 ||
            scene.height != 240)
            return fail(Error::invalid_package);
    }
    content_.clear();
    if (index_ == package_->count()) {
        Bytes{}.swap(content_);
        done_ = true;
    }
    return Error::ok;
}
Error validateApplication(
    Package &package, Manifest &manifest, MemoryBudget available, uint64_t caps) {
    ApplicationValidation validation;
    auto error = validation.begin(package, manifest, available, caps);
    while (error == Error::ok && !validation.done())
        error = validation.advance(4096);
    return error;
}
} // namespace flint
