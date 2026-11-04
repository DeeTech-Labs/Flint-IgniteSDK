#include <flint/lua_app.h>
#include <flint/manifest.h>
#include <flint/registry.h>
#include <set>
namespace flint {
Error loadManifest(Package &package, Manifest &out) {
  Entry manifestEntry{};
  for (uint32_t i = 0; i < package.count(); ++i) {
    Entry e;
    auto error = package.entry(i, e);
    if (error != Error::ok)
      return error;
    if (e.type == Section::manifest)
      manifestEntry = e;
  }
  if (!manifestEntry.size || manifestEntry.size > MaxManifestBytes)
    return Error::invalid_package;
  Bytes bytes(manifestEntry.size);
  if (package.read(manifestEntry, 0, bytes) != Error::ok)
    return Error::io_error;
  Reader r{bytes};
  if (r.n() != 0x314d4c46)
    return Error::invalid_package;
  Manifest m;
  m.id = r.text(64);
  m.name = r.text(96);
  m.publisher = r.text(64);
  m.version = r.n();
  m.budget = {uint32_t(r.n()), uint32_t(r.n()), uint32_t(r.n())};
  m.capabilities = r.n(8);
  if (!identifier(m.id) || !identifier(m.publisher) || m.name.empty() ||
      !m.version)
    return Error::invalid_package;
  const auto count = r.n(2);
  if (count > 64)
    return Error::invalid_package;
  std::set<std::string> declarations;
  for (unsigned i = 0; i < count; ++i) {
    Declaration d{r.text(80), r.text(192)};
    if (!identifier(d.permission) ||
        !declarations.insert(d.permission + "\n" + d.scope).second)
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
    bool found = false;
    for (uint32_t j = 0; j < package.count(); ++j) {
      Entry e;
      package.entry(j, e);
      if (e.id == c.section && e.type != Section::manifest &&
          e.type != Section::signature)
        found = true;
    }
    if (!found)
      return Error::invalid_package;
    m.components.push_back(std::move(c));
  }
  if (!r.done())
    return Error::invalid_package;
  out = std::move(m);
  return Error::ok;
}
Error validateApplication(Package &package, Manifest &m, MemoryBudget available,
                          uint64_t caps) {
  auto error = loadManifest(package, m);
  if (error != Error::ok)
    return error;
  if ((m.capabilities & caps) != m.capabilities)
    return Error::not_supported;
  bool hasUi = false;
  for (const auto &component : m.components) {
    hasUi = hasUi || component.type == Component::app ||
            component.type == Component::watchface ||
            component.type == Component::widget ||
            component.type == Component::complication;
    if (component.type == Component::service) {
      return Error::not_supported;
    }
  }
  if (hasUi && m.budget.internal_bytes < 16 * 1024) {
    return Error::quota_exceeded;
  }
  if (!fits(m.budget, available))
    return Error::resource_exhausted;
  [[maybe_unused]] unsigned luaCount = 0;
  for (uint32_t i = 0; i < package.count(); ++i) {
    Entry e;
    package.entry(i, e);
    if (e.type == Section::lua) {
#ifndef FLINT_ENABLE_LUA
      return Error::not_supported;
#else
      if (++luaCount > 1 || m.components.size() != 1 ||
          (m.components[0].type != Component::app &&
           m.components[0].type != Component::watchface) ||
          !(m.capabilities & CapLua) || !(caps & CapLua))
        return Error::not_supported;
      if (e.size > MaxLuaSourceBytes || m.budget.internal_bytes < 48 * 1024 ||
          m.budget.internal_bytes > 144 * 1024 || !m.budget.instructions ||
          m.budget.instructions > 100000)
        return Error::quota_exceeded;
      Bytes code(e.size);
      if (package.read(e, 0, code) != Error::ok || code[0] == 0x1b ||
          std::find(code.begin(), code.end(), 0) != code.end())
        return Error::invalid_package;
#endif
    }
    if (e.type == Section::ui) {
      if (e.size > FlintRender::MaxEncoded)
        return Error::quota_exceeded;
      Bytes b(e.size);
      FlintRender::Scene scene;
      if (package.read(e, 0, b) != Error::ok ||
          (!FlintRender::decode(b.data(), b.size(), scene) ||
           scene.width != 280 || scene.height != 240))
        return Error::invalid_package;
    }
  }
  return Error::ok;
}
} // namespace flint
