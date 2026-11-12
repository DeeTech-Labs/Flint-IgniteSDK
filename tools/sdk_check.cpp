#include <flint/runtime.h>
#include <iostream>
#include <limits>
#include <string>

namespace {
std::string quote(std::string_view value) {
    std::string out = "\"";
    const char *hex = "0123456789abcdef";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 32) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += char(c);
        }
    }
    return out + '"';
}
const char *errorName(flint::Error error) {
    using flint::Error;
    switch (error) {
    case Error::ok:
        return "ok";
    case Error::invalid_package:
        return "invalid_package";
    case Error::incompatible_contract:
        return "incompatible_contract";
    case Error::not_supported:
        return "not_supported";
    case Error::permission_denied:
        return "permission_denied";
    case Error::quota_exceeded:
        return "quota_exceeded";
    case Error::resource_exhausted:
        return "resource_exhausted";
    case Error::integrity_failed:
        return "integrity_failed";
    case Error::io_error:
        return "io_error";
    default:
        return "validation_failed";
    }
}
int fail(flint::Error error) {
    std::cerr << "Ignite validation: " << errorName(error) << '\n';
    return 1;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 2) {
        std::cerr << "Usage: ignite-check package.ignit\n";
        return 2;
    }
    flint::File file;
    if (!file.open(argv[1]))
        return fail(flint::Error::io_error);
    if (file.size > flint::MaxPackageBytes)
        return fail(flint::Error::quota_exceeded);
    flint::Package package;
    auto error = package.open(file.source());
    if (error != flint::Error::ok)
        return fail(error);
    flint::Manifest manifest;
    // Structural/runtime compatibility only. Device admission uses its own measured budgets.
    constexpr auto max = std::numeric_limits<uint32_t>::max();
    const flint::MemoryBudget available{max, max, max};
    error = flint::validateApplication(package, manifest, available, UINT64_MAX);
    if (error != flint::Error::ok)
        return fail(error);
    for (const auto &component : manifest.components) {
        if (component.type == flint::Component::app ||
            component.type == flint::Component::watchface ||
            component.type == flint::Component::widget ||
            component.type == flint::Component::complication) {
            flint::Runtime runtime;
            error = runtime.launch({manifest, argv[1], false}, component.id, available, UINT64_MAX);
            if (error != flint::Error::ok) {
                const auto &diagnostic = runtime.diagnostic();
                std::cerr << "Runtime " << component.id << ": peak=" << diagnostic.peak
                          << " used=" << diagnostic.used << " " << diagnostic.message.data() << '\n';
                return fail(error);
            }
        } else if (component.type == flint::Component::theme ||
            component.type == flint::Component::locale) {
            flint::Values values;
            error = flint::loadValues({manifest, argv[1], false}, component, values);
            if (error != flint::Error::ok)
                return fail(error);
        }
    }
    std::cout << "{\"id\":" << quote(manifest.id) << ",\"name\":" << quote(manifest.name)
              << ",\"publisher\":" << quote(manifest.publisher)
              << ",\"version\":" << manifest.version << ",\"package_bytes\":" << file.size
              << ",\"contract_revision\":" << flint::ContractRevision
              << ",\"required_capabilities\":" << manifest.capabilities
              << ",\"declared_budget\":{\"internal_bytes\":" << manifest.budget.internal_bytes
              << ",\"external_bytes\":" << manifest.budget.external_bytes
              << ",\"instructions\":" << manifest.budget.instructions
              << "},\"runtime_compatible\":true,\"device_fit_verified\":false"
                 ",\"publisher_verified\":false,\"permissions\":[";
    bool first = true;
    for (const auto &permission : manifest.permissions) {
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"id\":" << quote(permission.permission)
                  << ",\"scope\":" << quote(permission.scope) << '}';
    }
    std::cout << "],\"components\":[";
    first = true;
    for (const auto &component : manifest.components) {
        if (!first)
            std::cout << ',';
        first = false;
        std::cout << "{\"id\":" << quote(component.id) << ",\"type\":" << unsigned(component.type)
                  << '}';
    }
    std::cout << "]}\n";
}
