#include <cstdlib>
#include <flint/background.h>
#include <flint/binary.h>
#include <flint/graphics/scene.h>
#include <flint/lua_app.h>
#include <flint/manifest.h>
#include <flint/package.h>
#include <flint/permissions.h>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <vector>
using namespace flint;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x)) {                                                                                \
            std::cerr << __LINE__ << ": " #x "\n";                                                 \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (false)
static bool read(void *ctx, uint32_t offset, std::span<uint8_t> out) {
    auto &b = *static_cast<std::vector<uint8_t> *>(ctx);
    if (offset > b.size() || out.size() > b.size() - offset)
        return false;
    std::copy_n(b.begin() + offset, out.size(), out.begin());
    return true;
}
static std::vector<uint8_t> load(const char *file) {
    std::ifstream input(file, std::ios::binary);
    CHECK(input.good());
    return {std::istreambuf_iterator<char>(input), {}};
}
static Bytes componentPackage(
    bool duplicateLogic = false, bool orphan = false, bool legacy = false) {
    Writer manifest;
    manifest.n(legacy ? 0x324d4c46 : 0x334d4c46);
    manifest.text("test.components");
    manifest.text("Components");
    manifest.text("tests");
    manifest.n(1);
    manifest.n(2, 2);
    auto component = [&](const char *id, Component type, uint32_t section, uint32_t logic,
                         MemoryBudget budget, uint64_t caps,
                         const std::vector<std::string> &permissions) {
        manifest.text(id);
        manifest.n(uint16_t(type), 2);
        manifest.n(0, 1);
        manifest.n(section);
        manifest.n(logic);
        manifest.n(budget.internal_bytes);
        manifest.n(budget.external_bytes);
        manifest.n(budget.instructions);
        manifest.n(caps, 8);
        manifest.n(permissions.size(), 2);
        for (const auto &permission : permissions) {
            manifest.text(permission);
            manifest.text("");
        }
    };
    component("main", Component::app, 2, 3, {49152, 1024, 4096}, 1 | CapLua | CapAppApi,
        {"ui.render", "storage.private"});
    component("worker", Component::service, 0, duplicateLogic ? 3 : 4, {98304, 0, 12000},
        CapBackground | CapLua | CapAppApi | sensors::Capability,
        {"app.background.execute", "sensors.temperature", "storage.private"});
    for (int i = 0; i < 4; ++i)
        manifest.n(0);
    FlintRender::Scene scene;
    FlintRender::rect(scene, 0, 0, 280, 240, 0);
    Bytes ui(FlintRender::MaxEncoded);
    ui.resize(FlintRender::encode(scene, ui.data(), ui.size()));
    const Bytes logic{'r', 'e', 't', 'u', 'r', 'n', ' ', '{', '}'};
    std::vector<std::pair<Section, Bytes>> sections{{Section::manifest, manifest.data},
        {Section::ui, ui}, {Section::lua, logic}, {Section::lua, logic}};
    if (orphan)
        sections.push_back({Section::lua, logic});
    uint32_t offset = HeaderSize + sections.size() * EntrySize, total = offset;
    for (const auto &section : sections)
        total += section.second.size();
    Writer package;
    package.raw(PackageMagic);
    package.n(PackageVersion, 2);
    package.n(HeaderSize, 2);
    package.n(ContractRevision);
    package.n(total);
    package.n(sections.size());
    package.n(HeaderSize);
    package.n(0);
    package.raw(std::array<uint8_t, 32>{});
    uint32_t id = 1;
    for (const auto &[type, bytes] : sections) {
        package.n(id++);
        package.n(uint16_t(type), 2);
        package.n(0, 2);
        package.n(offset);
        package.n(bytes.size());
        package.n(0);
        package.n(0);
        offset += bytes.size();
    }
    for (const auto &section : sections)
        package.raw(section.second);
    Sha256 hash;
    hash.update(package.data);
    const auto digest = hash.finish();
    std::copy(digest.begin(), digest.end(), package.data.begin() + 32);
    return package.data;
}
static void componentManifestContracts() {
    auto bytes = componentPackage();
    Package package;
    CHECK(package.open({&bytes, uint32_t(bytes.size()), read}) == Error::ok);
    Manifest manifest;
    CHECK(loadManifest(package, manifest) == Error::ok);
    CHECK(manifest.components.size() == 2 && manifest.components[1].section == 0);
    CHECK(manifest.components[0].logicSection == 3 && manifest.components[1].logicSection == 4);
    CHECK(manifest.budget.internal_bytes == 98304 && manifest.budget.external_bytes == 1024 &&
        manifest.budget.instructions == 12000);
    CHECK(manifest.permissions.size() == 4 &&
        manifest.permissions.front().permission == "app.background.execute");
    CHECK(manifest.components[0].permissions.size() == 2 &&
        manifest.components[1].permissions.size() == 3);
#ifdef FLINT_ENABLE_LUA
    CHECK(validateApplication(package, manifest, {98304, 1024, 12000}, UINT64_MAX) == Error::ok);
    bytes = componentPackage(false, true);
    CHECK(package.open({&bytes, uint32_t(bytes.size()), read}) == Error::ok);
    CHECK(validateApplication(package, manifest, {98304, 1024, 12000}, UINT64_MAX) ==
        Error::invalid_package);
#endif
    bytes = componentPackage(true);
    CHECK(package.open({&bytes, uint32_t(bytes.size()), read}) == Error::ok);
    CHECK(loadManifest(package, manifest) == Error::invalid_package);
    bytes = componentPackage(false, false, true);
    CHECK(package.open({&bytes, uint32_t(bytes.size()), read}) == Error::ok);
    CHECK(loadManifest(package, manifest) == Error::invalid_package);
}
int main(int argc, char **argv) {
    componentManifestContracts();
    {
        Reader reader{{}, std::numeric_limits<size_t>::max()};
        CHECK(reader.n(2) == 0 && !reader.valid);
        FlintRender::Scene scene;
        uint8_t byte = 0;
        CHECK(!scene.add({}, &byte, std::numeric_limits<size_t>::max()));
        CHECK(!scene.valid && scene.count == 0);
        FlintRender::Scene nullData;
        CHECK(!nullData.add({}, nullptr, 1));
        CHECK(!nullData.valid && nullData.count == 0);
    }
    CHECK(ApiVersion == 1 && PackageVersion == 1 && FaceApiVersion == 1 && ProtocolVersion == 1);
    CHECK(compatible({1, ContractRevision, "local", 3}, 3) == Error::ok);
    CHECK(compatible({1, 0, "old", 3}, 3) == Error::incompatible_contract);
    CHECK(compatible({1, ContractRevision, "", 3}, 3) == Error::incompatible_contract);
    CHECK(compatible({1, ContractRevision, "local", 1}, 3) == Error::not_supported);
    CHECK(!fits({10, 0, 1}, {9, 2000000, 1}));
    AppLifecycle app;
    CHECK(app.dispatch(Lifecycle::activate) == Error::invalid_state);
    for (auto event : {Lifecycle::create, Lifecycle::activate, Lifecycle::event,
             Lifecycle::deactivate, Lifecycle::suspend, Lifecycle::resume, Lifecycle::destroy})
        CHECK(app.dispatch(event) == Error::ok);
    Permission metrics{"companion.pc.metrics", Access::persistent, 1};
    Call call{{1, 2, 3}, "cpu", 10, 7, 42, 1, true, true, false};
    std::array<Grant, 1> grants{{{call.principal, metrics.id, "cpu", 20, 7, 42, true}}};
    CHECK(authorize(metrics, call, grants) == Error::ok);
    call.principal.publisher = 9;
    CHECK(authorize(metrics, call, grants) == Error::permission_denied);
    call.principal.publisher = 2;
    call.scope = "ram";
    CHECK(authorize(metrics, call, grants) == Error::permission_denied);
    call.scope = "cpu";
    call.now = 20;
    CHECK(authorize(metrics, call, grants) == Error::permission_denied);
    call.now = 10;
    call.capabilities = 0;
    CHECK(authorize(metrics, call, grants) == Error::not_supported);
    call.capabilities = 1;
    metrics.access = Access::session;
    call.session = 8;
    CHECK(authorize(metrics, call, grants) == Error::permission_denied);
    call.session = 7;
    CHECK(authorize(metrics, call, grants) == Error::ok);
    metrics.access = Access::explicit_action;
    CHECK(authorize(metrics, call, grants) == Error::ok);
    CHECK(authorize(metrics, call, grants) == Error::permission_denied);
    grants[0].active = true;
    revoke(call.principal, grants);
    CHECK(authorize(metrics, call, grants) == Error::permission_denied);
    call.foreground = false;
    CHECK(authorize(metrics, call, grants) == Error::user_interaction_required);
    Sha256 hash;
    auto digest = hash.finish();
    const std::array<uint8_t, 32> empty{0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14, 0x9a, 0xfb,
        0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24, 0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c, 0xa4,
        0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
    CHECK(digest == empty);
    if (argc == 1)
        return 0;
    auto bytes = load(argv[1]);
    Package package;
    auto open = [&] { return package.open({&bytes, uint32_t(bytes.size()), read}); };
    CHECK(open() == Error::ok);
    CHECK(package.count() == 2);
    Entry entry;
    CHECK(package.entry(1, entry) == Error::ok);
    // Forged offsets cannot escape the validated section.
    entry.offset = 0;
    std::array<uint8_t, 3> out{};
    CHECK(package.read(entry, 0, out) == Error::ok);
    CHECK(out[0] == 'a' && out[1] == 'b' && out[2] == 'c');
    CHECK(package.read(entry, entry.size, out) == Error::invalid_argument);
    auto original = bytes;
    auto seal = [&] {
        std::fill(bytes.begin() + 32, bytes.begin() + 64, 0);
        Sha256 digest;
        digest.update(bytes);
        auto value = digest.finish();
        std::copy(value.begin(), value.end(), bytes.begin() + 32);
    };
    // Invalid structure must fail even when an attacker recomputes the hash.
    for (auto offset : {64u, 68u, 70u, 72u, 76u, 80u, 84u, 92u, 96u}) {
        bytes = original;
        std::fill_n(bytes.begin() + offset, 2, 0xff);
        seal();
        CHECK(open() != Error::ok);
    }
    bytes = original;
    CHECK(open() == Error::ok);
    for (size_t i = 0; i < bytes.size(); ++i) {
        bytes = original;
        bytes[i] ^= 0x80;
        CHECK(open() != Error::ok);
        CHECK(package.count() == 0);
    }
    for (size_t i = 0; i < original.size(); ++i) {
        bytes.assign(original.begin(), original.begin() + i);
        CHECK(open() != Error::ok);
    }
    std::cout
        << "Contracts, lifecycle, SHA-256, indexed reads, corruption and truncation: passed\n";
}
