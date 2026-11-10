#pragma once
#include "app_api.h"
#include "manifest.h"
#include <cstdio>
#include <functional>
#include <memory>
#include <utility>
namespace flint {
struct File {
private:
    std::array<int64_t, 5> version_{};

public:
    FILE *handle{};
    uint32_t size{};
    File() = default;
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    File(File &&other) noexcept
        : version_(other.version_), handle(std::exchange(other.handle, nullptr)),
          size(std::exchange(other.size, 0)) {}
    File &operator=(File &&other) noexcept {
        if (this != &other) {
            close();
            handle = std::exchange(other.handle, nullptr);
            size = std::exchange(other.size, 0);
            version_ = other.version_;
        }
        return *this;
    }
    ~File() {
        close();
    }
    bool open(const std::string &path);
    void close();
    Source source();
    bool unchanged() const;
    bool sameObject(const File &other) const;
};
struct Installed {
    Manifest manifest;
    std::string path;
    bool factory{};
};
// Owner-thread operation; it must not outlive the PackageManager that created it.
class PreparedInstall {
    friend class PackageManager;
    friend class Runtime;
    friend class Core;
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit PreparedInstall(std::unique_ptr<Impl> impl);
    File &file();
    Package &package();
    Manifest releaseManifest();
    bool committed() const;

public:
    ~PreparedInstall();
    Error advance(uint32_t byteBudget);
    bool ready() const;
    uint32_t verifiedBytes() const;
    uint32_t size() const;
    const Manifest &manifest() const;
};
class PackageManager {
    std::string root_, factory_;
    MemoryBudget available_;
    uint64_t capabilities_;
    uint64_t revision_{};
    bool requireFactory_{};

public:
    PackageManager(std::string root, std::string factory, MemoryBudget available,
        uint64_t capabilities, bool requireFactory = false)
        : root_(std::move(root)), factory_(std::move(factory)), available_(available),
          capabilities_(capabilities), requireFactory_(requireFactory) {}
    // Changes only at catalog mutation boundaries; never caches authorization.
    uint64_t revision() const {
        return revision_;
    }
    Error begin();
    void visit(const std::function<bool(const Installed &)> &visitor) const;
    std::vector<Installed> list() const;
    Error inspect(const std::string &path, Manifest &manifest) const;
    Error locate(std::string_view id, Installed &result) const;
    Error commit(const std::string &staging, std::string &installedId);
    Error prepareCommit(std::unique_ptr<PreparedInstall> &prepared,
        std::string_view transferHash = {}, uint32_t expectedSize = 0);
    Error commit(PreparedInstall &prepared, std::string &installedId);
    Error remove(std::string_view id);
    Error rollback(std::string_view id);
    std::string staging() const {
        return root_ + "/incoming.part";
    }
    std::string root() const {
        return root_;
    }
};
// Core supplies the installed namespace; applications cannot choose package identity.
Error privateStorage(const std::string &root, const Manifest &manifest, AppService operation,
    std::string_view key, std::string_view value, AppReply &reply);
bool writeAtomic(const std::string &path, std::span<const uint8_t> bytes);
std::string readText(const std::string &path, size_t max = 256);
} // namespace flint
