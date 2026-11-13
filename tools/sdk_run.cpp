#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <flint/core.h>
#include <flint/sensor_simulation.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <sstream>
#include <vector>
namespace {
bool number(std::string_view text, uint64_t &out) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool real(std::string_view text, double &out) {
    const auto result = std::from_chars(text.data(), text.data() + text.size(), out);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() &&
        std::isfinite(out) && std::abs(out) <= 1e12;
}
bool hex(std::string_view text, std::string &out) {
    if (text == "-") {
        out.clear();
        return true;
    }
    if (text.size() > 384 || text.size() % 2)
        return false;
    out.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        unsigned value{};
        const auto result = std::from_chars(text.data() + i, text.data() + i + 2, value, 16);
        if (result.ec != std::errc{} || result.ptr != text.data() + i + 2)
            return false;
        out.push_back(char(value));
    }
    return flint::appText(out);
}
flint::Error expectService(flint::Core &core, std::string_view package, std::string_view component,
    uint64_t state, uint64_t reason, bool &matches) {
    matches = false;
    for (unsigned offset = 0; offset < 256; offset += 8) {
        flint::Writer request;
        request.n(offset, 2);
        request.n(8, 1);
        flint::Session session{true, true, 1};
        const auto response =
            core.handle({flint::Method::services, 1, false, request.data}, session);
        flint::Reader reader{response.payload};
        const auto error = flint::Error(reader.n(2));
        if (error != flint::Error::ok)
            return error;
        const auto count = reader.n(1), more = reader.n(1);
        reader.n(1); // Model max-services and four u32 policy fields.
        for (unsigned i = 0; i < 4; ++i)
            reader.n();
        reader.n(1); // Low-energy flag.
        if (count > 8 || more > 1 || (more && !count))
            return flint::Error::invalid_argument;
        for (unsigned i = 0; i < count; ++i) {
            const auto id = reader.text(64), target = reader.text(64);
            (void)reader.text(64); // Display label, not application state.
            const auto actualState = reader.n(1), actualReason = reader.n(1);
            const auto status = reader.n(2);
            if (actualState > 4 || actualReason > 8 || status > unsigned(flint::Error::conflict))
                return flint::Error::invalid_argument;
            reader.n(8);
            reader.n(8); // Instance identity.
            for (unsigned j = 0; j < 3; ++j)
                reader.n();
            for (unsigned j = 0; j < 7; ++j)
                reader.n(8);
            for (unsigned j = 0; j < 3; ++j)
                if (reader.n(1) > 1)
                    return flint::Error::invalid_argument;
            if (id == package && target == component)
                matches = actualState == state && (reason == 255 || actualReason == reason);
        }
        if (!reader.done())
            return flint::Error::invalid_argument;
        if (!more)
            return flint::Error::ok;
    }
    return flint::Error::quota_exceeded;
}
struct Temporary {
    std::filesystem::path path;
    ~Temporary() {
        if (!path.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }
    }
};
} // namespace
int main(int argc, char **argv) {
    if (argc != 5 && !(argc == 8 && std::string_view(argv[5]) == "--scenario")) {
        std::cerr << "Usage: ignite-run package.ignit frames press-every output.ppm [--scenario "
                     "storage factory]\n";
        return 2;
    }
    const bool scenario = argc == 8;
    uint64_t frames{}, interval{};
    if (!number(argv[2], frames) || !number(argv[3], interval) || !frames || frames > 3600 ||
        interval > 3600)
        return 2;
    std::error_code fsError;
    if (std::filesystem::equivalent(argv[1], argv[4], fsError))
        return 2;
    flint::File file;
    flint::Package package;
    flint::Manifest manifest;
    constexpr auto max = std::numeric_limits<uint32_t>::max();
    flint::Platform platform;
    platform.build_id = "ignite-simulator";
    platform.memory = {max, max, max};
    platform.background.maxServices = 2;
    platform.background.memoryBytes = 131072;
    platform.capabilities |= flint::CapBackground | flint::CapComponentIpc;
    platform.background_available_memory = [] {
        return flint::MemoryBudget{1024 * 1024, 1024 * 1024, 100000};
    };
    bool lowEnergy = false, simulationControl = false;
    platform.background_low_energy = [&] { return lowEnergy; };
    platform.microseconds = [] {
        return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
                .count());
    };
    uint64_t now = 1000;
    platform.monotonic = [&] { return now; };
    if (!file.open(argv[1]) || file.size > flint::MaxPackageBytes ||
        package.open(file.source()) != flint::Error::ok ||
        flint::validateApplication(package, manifest, platform.memory, platform.capabilities) !=
            flint::Error::ok)
        return 1;
    flint::sensors::Simulation sensors;
    if (manifest.capabilities & flint::sensors::Capability) {
        using namespace flint::sensors;
        Source source;
        source.id = 1;
        source.channels = 7;
        source.temperature = TemperatureKind::ambient;
        std::array<Vector3, 6> values{};
        values[0].x = 23.5;
        values[1].x = 101325;
        values[2].x = 45;
        if (sensors.add(source, values) != flint::Error::ok)
            return 1;
        source = {};
        source.id = 2;
        source.channels = bit(Channel::ambient_light);
        values = {};
        values[3].x = 100;
        if (sensors.add(source, values) != flint::Error::ok)
            return 1;
        source = {};
        source.id = 3;
        source.channels = bit(Channel::acceleration) | bit(Channel::angular_velocity);
        source.coordinates = Coordinates::device;
        values = {};
        values[4].z = 9.80665;
        if (sensors.add(source, values) != flint::Error::ok)
            return 1;
        source = {};
        source.id = 4;
        source.channels = bit(Channel::temperature);
        source.temperature = TemperatureKind::die;
        source.priority = 10;
        values = {};
        values[0].x = 35;
        if (sensors.add(source, values) != flint::Error::ok)
            return 1;
        platform.sensors = &sensors;
    }
    Temporary temporary;
    std::string storage, factory;
    if (scenario) {
        storage = argv[6];
        factory = argv[7];
    } else {
        auto base = std::filesystem::temp_directory_path(fsError);
        if (fsError)
            return 1;
        for (unsigned attempt = 0; attempt < 16; ++attempt) {
            auto candidate = base /
                ("ignite-sim-" +
                    std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate, fsError)) {
                temporary.path = candidate;
                break;
            }
        }
        if (temporary.path.empty())
            return 1;
        storage = temporary.path.string();
        factory = (temporary.path / "factory").string();
    }
    std::unique_ptr<flint::Core> core;
    const auto begin = [&] {
        core = std::make_unique<flint::Core>(storage, factory, platform);
        const auto error = core->begin();
        return error == flint::Error::ok || error == flint::Error::source_unavailable;
    };
    if (!begin())
        return 1;
    flint::Installed installed;
    const auto found = core->packages().locate(manifest.id, installed);
    if (found == flint::Error::ok && installed.manifest.version == manifest.version) {
        flint::File previous;
        flint::Package previousPackage;
        std::array<uint8_t, 64> previousHeader{}, currentHeader{};
        if (!previous.open(installed.path) ||
            previousPackage.open(previous.source()) != flint::Error::ok ||
            !previous.source().read(&previous, 0, previousHeader) ||
            !file.source().read(&file, 0, currentHeader) || previousHeader != currentHeader)
            return 1;
    } else {
        if (!std::filesystem::copy_file(argv[1], core->packages().staging(),
                std::filesystem::copy_options::overwrite_existing, fsError) ||
            fsError)
            return 1;
        std::string id;
        if (core->packages().commit(core->packages().staging(), id) != flint::Error::ok)
            return 1;
    }
    const flint::ComponentInfo *foreground = nullptr;
    for (const auto &component : manifest.components)
        if ((component.type == flint::Component::app ||
                component.type == flint::Component::watchface) &&
            component.logicSection) {
            foreground = &component;
            break;
        }
    // Installing or showing UI never starts a service. Service-only packages may use the
    // factory clock already selected by this same Core, if the caller supplied factory packages.
    const auto launch = [&] {
        return foreground                ? core->launch(manifest.id, foreground->id)
            : core->runtime().scripted() ? flint::Error::ok
                                         : flint::Error::not_supported;
    };
    if (launch() != flint::Error::ok || !core->runtime().scripted())
        return 1;
    FlintRender::Scene scene;
    size_t steps = 0;
    if (scenario) {
        // Host-only scenario transport. The public CLI validates JSON and emits bounded tokens.
        std::array<char, 1026> line{};
        while (std::cin.getline(line.data(), line.size())) {
            if (++steps > 4096)
                return 2;
            std::istringstream input(line.data());
            std::string op, a, b, c, d, trailing;
            input >> op;
            flint::Error error = flint::Error::ok;
            bool valid = true;
            uint64_t value{}, expected{};
            if (op == "advance") {
                input >> a;
                valid = number(a, value) && value <= 86400000 && value <= UINT64_MAX - now;
                if (valid) {
                    now += value;
                    core->tick();
                }
            } else if (op == "input") {
                input >> a >> b >> c;
                int32_t amount{};
                const auto parsed = std::from_chars(b.data(), b.data() + b.size(), amount);
                valid = parsed.ec == std::errc{} && parsed.ptr == b.data() + b.size() &&
                    number(c, expected);
                bool known = false;
                for (unsigned kind = 0; kind <= unsigned(flint::InputKind::focus_gained); ++kind) {
                    if (flint::inputName(flint::InputKind(kind)) != a)
                        continue;
                    known = true;
                    if (valid)
                        error = core->postInput(
                            {flint::InputKind(kind), amount, core->runtime().generation()});
                }
                valid &= known;
                if (error == flint::Error::ok && valid)
                    core->tick();
            } else if (op == "suspend")
                error = core->runtime().suspend();
            else if (op == "resume")
                error = core->runtime().resume();
            else if (op == "restart")
                error = launch();
            else if (op == "reboot") {
                core.reset();
                if (!begin())
                    return 1;
                error = launch();
            } else if (op == "expect_text") {
                input >> a;
                std::string text;
                valid = hex(a, text);
                error = core->scene(scene);
                if (error == flint::Error::ok) {
                    bool foundText = false;
                    for (unsigned i = 0; i < scene.count; ++i) {
                        const auto &node = scene.ops[i];
                        if (node.kind == FlintRender::Text &&
                            std::string_view(
                                reinterpret_cast<const char *>(scene.data + node.offset),
                                node.length)
                                    .find(text) != std::string_view::npos)
                            foundText = true;
                    }
                    valid &= foundText;
                }
            } else if (op == "expect_state") {
                input >> a;
                valid = a == "active" ? core->runtime().state() == flint::State::active
                    : a == "suspended"
                    ? core->runtime().state() == flint::State::suspended
                    : a == "empty" && core->runtime().state() == flint::State::empty;
            } else if (op == "expect_error") {
                input >> a;
                valid = number(a, value) && value == uint16_t(core->lastLogicError());
            } else if (op == "permission") {
                input >> a >> b >> c;
                std::string scope;
                valid = hex(b, scope) && (c == "0" || c == "1");
                flint::Writer writer;
                writer.text(manifest.id);
                writer.text(a);
                writer.text(scope);
                writer.n(c == "1", 1);
                flint::Session session{true, true, 1};
                const auto response = core->handle(
                    {flint::Method::permission_set, uint32_t(steps), false, writer.data}, session);
                flint::Reader reader{response.payload};
                error = flint::Error(reader.n(2));
            } else if (op == "service_start" || op == "service_stop" || op == "service_pause" ||
                op == "service_resume" || op == "service_auto" || op == "service_revoke") {
                input >> a >> b;
                std::string targetPackage, targetComponent;
                valid = hex(a, targetPackage) && hex(b, targetComponent);
                if (targetPackage.empty())
                    targetPackage = manifest.id;
                valid &= flint::identifier(targetPackage) && flint::identifier(targetComponent);
                unsigned action = op == "service_start" ? 0
                    : op == "service_stop"              ? 1
                    : op == "service_pause"             ? 2
                    : op == "service_resume"            ? 3
                                                        : 6;
                if (op == "service_auto") {
                    input >> c;
                    valid &= c == "0" || c == "1";
                    action = c == "1" ? 4 : 5;
                }
                input >> d;
                valid &= number(d, expected) && expected <= unsigned(flint::Error::conflict);
                if (input >> trailing)
                    valid = false;
                if (valid) {
                    flint::Writer writer;
                    writer.n(action, 1);
                    writer.text(targetPackage);
                    writer.text(targetComponent);
                    flint::Session session{true, true, 1};
                    const auto response = core->handle(
                        {flint::Method::service_control, uint32_t(steps), false, writer.data},
                        session);
                    flint::Reader reader{response.payload};
                    error = flint::Error(reader.n(2));
                    valid &= reader.done();
                }
            } else if (op == "expect_service") {
                input >> a >> b >> c >> d;
                std::string targetPackage, targetComponent;
                uint64_t state{}, reason{};
                valid = hex(a, targetPackage) && hex(b, targetComponent) && number(c, state) &&
                    state <= 4 && number(d, reason) && (reason <= 8 || reason == 255);
                if (targetPackage.empty())
                    targetPackage = manifest.id;
                valid &= flint::identifier(targetPackage) && flint::identifier(targetComponent);
                if (valid) {
                    bool matches{};
                    error = expectService(
                        *core, targetPackage, targetComponent, state, reason, matches);
                    valid &= matches;
                }
            } else if (op == "low_energy") {
                input >> a;
                valid = a == "0" || a == "1";
                if (valid) {
                    lowEnergy = a == "1";
                    simulationControl = true;
                    core->tick();
                }
            } else if (op == "sensor_value") {
                std::string x, y, z;
                input >> a >> b >> x >> y >> z;
                uint64_t channel{};
                flint::sensors::Vector3 vector;
                valid = number(a, value) && value && value <= UINT16_MAX && number(b, channel) &&
                    channel < 6 && real(x, vector.x) && real(y, vector.y) && real(z, vector.z);
                if (valid)
                    error = sensors.setValue(
                        flint::sensors::SourceId(value), flint::sensors::Channel(channel), vector);
            } else if (op == "sensor_state") {
                input >> a >> b >> c;
                uint64_t status{}, reason{};
                valid = number(a, value) && value && value <= UINT16_MAX && number(b, status) &&
                    status <= 5 && number(c, reason) &&
                    reason <= uint16_t(flint::Error::execution_limit);
                if (valid)
                    error = sensors.status(flint::sensors::SourceId(value),
                        flint::sensors::Status(status), flint::Error(reason));
            } else if (op == "sensor_reset") {
                input >> a;
                valid = number(a, value) && value && value <= UINT16_MAX;
                bool present = false;
                std::array<flint::sensors::Source, flint::sensors::MaxSources> catalog{};
                const auto count = sensors.catalog(catalog);
                for (size_t i = 0; i < count; ++i)
                    present |= catalog[i].id == value;
                valid &= present;
                if (valid)
                    sensors.reset(flint::sensors::SourceId(value));
            } else
                valid = false;
            if (input >> trailing)
                valid = false;
            if (!valid || uint16_t(error) != expected) {
                std::cerr << "Scenario failed at step " << steps << " (" << op << "), error "
                          << uint16_t(error) << '\n';
                return 1;
            }
        }
        if (!std::cin.eof())
            return 2;
    } else {
        for (uint64_t i = 0; i < frames; ++i) {
            if (interval && i % interval == 0 &&
                core->postInput({flint::InputKind::press, 1, core->runtime().generation()}) !=
                    flint::Error::ok)
                return 1;
            now += 33;
            core->tick();
            if (core->lastLogicError() != flint::Error::ok) {
                std::cerr << "Lua application failed at frame " << i << '\n';
                return 1;
            }
        }
    }
    if (core->scene(scene) != flint::Error::ok)
        return 1;
    std::vector<uint16_t> pixels(scene.width * scene.height);
    FlintRender::Canvas canvas(pixels.data(), scene.width, scene.height);
    core->paint(scene, canvas);
    std::ofstream image(argv[4], std::ios::binary);
    image << "P6\n" << scene.width << ' ' << scene.height << "\n255\n";
    for (auto pixel : pixels) {
        char rgb[3]{char(((pixel >> 11) & 31) * 255 / 31), char(((pixel >> 5) & 63) * 255 / 63),
            char((pixel & 31) * 255 / 31)};
        image.write(rgb, 3);
    }
    image.close();
    if (!image)
        return 1;
    std::cout << "{\"frames\":" << (scenario ? steps : frames)
              << ",\"sensor_simulation\":" << (platform.sensors ? "true" : "false")
              << ",\"background_simulation\":true,\"background_max_services\":2"
              << ",\"simulation_control\":" << (simulationControl ? "true" : "false")
              << ",\"background_low_energy\":" << (lowEnergy ? "true" : "false")
              << ",\"pixel_hash\":" << FlintRender::pixelHash(canvas)
              << ",\"lua_peak_bytes\":" << core->runtime().logicPeak() << "}\n";
}
