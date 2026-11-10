#include <flint/storage.h>
#ifndef _WIN32
#include <dirent.h>
#else
#include <direct.h>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <algorithm>
#include <cerrno>
#include <flint/os.h>
#include <map>
#include <new>
#include <set>
#include <sys/stat.h>
namespace flint {
static int replaceFile(const char *source, const char *target) {
#ifdef _WIN32
    return MoveFileExA(source, target, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ? 0 : -1;
#else
    return rename(source, target);
#endif
}

static std::array<int64_t, 5> fileVersion(const struct stat &status) {
    std::array<int64_t, 5> result{
        int64_t(status.st_size), int64_t(status.st_mtime), int64_t(status.st_ctime), 0, 0};
#if defined(__linux__)
    result[3] = status.st_mtim.tv_nsec;
    result[4] = status.st_ctim.tv_nsec;
#elif defined(__APPLE__)
    result[3] = status.st_mtimespec.tv_nsec;
    result[4] = status.st_ctimespec.tv_nsec;
#endif
    return result;
}
bool File::open(const std::string &path) {
    close();
#ifdef _WIN32
    // Keep a verified object open across an atomic rename. Disallow writers while
    // holding this read handle; ordinary fopen does not permit FILE_SHARE_DELETE.
    const auto native = CreateFileA(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (native == INVALID_HANDLE_VALUE)
        return false;
    const auto descriptor =
        _open_osfhandle(reinterpret_cast<intptr_t>(native), _O_RDONLY | _O_BINARY);
    if (descriptor < 0) {
        CloseHandle(native);
        return false;
    }
    handle = _fdopen(descriptor, "rb");
    if (!handle) {
        _close(descriptor);
        return false;
    }
#else
    handle = fopen(path.c_str(), "rb");
    if (!handle)
        return false;
#endif
    struct stat st{};
    if (fstat(fileno(handle), &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        uint64_t(st.st_size) > MaxPackageBytes) {
        close();
        return false;
    }
    size = st.st_size;
    version_ = fileVersion(st);
    return true;
}
bool File::unchanged() const {
    struct stat status{};
    return handle && !fstat(fileno(handle), &status) && fileVersion(status) == version_;
}
bool File::sameObject(const File &other) const {
    if (!handle || !other.handle)
        return false;
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION first{}, second{};
    return GetFileInformationByHandle(
               reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(handle))), &first) &&
        GetFileInformationByHandle(
            reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(other.handle))), &second) &&
        first.dwVolumeSerialNumber == second.dwVolumeSerialNumber &&
        first.nFileIndexHigh == second.nFileIndexHigh &&
        first.nFileIndexLow == second.nFileIndexLow;
#else
    struct stat first{}, second{};
    return !fstat(fileno(handle), &first) && !fstat(fileno(other.handle), &second) &&
        first.st_dev == second.st_dev && first.st_ino == second.st_ino;
#endif
}
void File::close() {
    if (handle)
        fclose(handle);
    handle = nullptr;
    size = 0;
}
Source File::source() {
    return {this, size, [](void *context, uint32_t offset, std::span<uint8_t> out) {
                auto &f = *static_cast<File *>(context);
                return f.handle && offset <= f.size && out.size() <= f.size - offset &&
                    fseek(f.handle, offset, SEEK_SET) == 0 &&
                    fread(out.data(), 1, out.size(), f.handle) == out.size();
            }};
}
bool writeAtomic(const std::string &path, std::span<const uint8_t> bytes) {
    const auto temporary = path + ".tmp";
    FILE *file = fopen(temporary.c_str(), "wb");
    if (!file)
        return false;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    if (fflush(file) || fsync(fileno(file)))
        ok = false;
    if (fclose(file))
        ok = false;
    if (ok)
        ok = replaceFile(temporary.c_str(), path.c_str()) == 0;
    if (!ok)
        unlink(temporary.c_str());
    return ok;
}
std::string readText(const std::string &path, size_t max) {
    FILE *file = fopen(path.c_str(), "rb");
    if (!file)
        return {};
    std::string out(max + 1, '\0');
    auto size = fread(out.data(), 1, out.size(), file);
    fclose(file);
    if (size > max)
        return {};
    out.resize(size);
    return out;
}
static bool mkdirOne(const std::string &path) {
#ifdef _WIN32
    return _mkdir(path.c_str()) == 0 || errno == EEXIST;
#else
    return mkdir(path.c_str(), 0700) == 0 || errno == EEXIST;
#endif
}
static std::vector<std::string> files(const std::string &path, std::string_view suffix) {
    std::vector<std::string> result;
#ifdef _WIN32
    std::error_code error;
    for (auto &e : std::filesystem::directory_iterator(path, error)) {
        auto name = e.path().filename().string();
        if (name.size() > suffix.size() && name.ends_with(suffix))
            result.push_back(name);
    }
#else
    auto *dir = opendir(path.c_str());
    if (!dir)
        return result;
    while (auto *e = readdir(dir)) {
        std::string name = e->d_name;
        if (name.size() > suffix.size() && name.ends_with(suffix))
            result.push_back(name);
    }
    closedir(dir);
#endif
    std::sort(result.begin(), result.end());
    return result;
}

static bool objectName(std::string_view name) {
    if (name.size() != 70 || !name.ends_with(".ignit"))
        return false;
    for (auto c : name.substr(0, 64))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    return true;
}
static void collectObjects(const std::string &root) {
    std::set<std::string> used;
    for (auto &ref : files(root, ".ref")) {
        auto path = root + "/" + ref;
        used.insert(readText(path));
        used.insert(readText(path + ".previous"));
    }
    for (auto &object : files(root, ".ignit"))
        if (!used.contains(object))
            unlink((root + "/" + object).c_str());
    for (auto &temporary : files(root, ".tmp"))
        unlink((root + "/" + temporary).c_str());
}
Error PackageManager::begin() {
    ++revision_;
    if (!mkdirOne(root_))
        return Error::io_error;
    for (auto &ref : files(root_, ".ref")) {
        auto id = ref.substr(0, ref.size() - 4);
        if (!identifier(id))
            continue;
        auto path = root_ + "/" + ref;
        auto object = readText(path);
        Manifest current;
        if (!objectName(object) || inspect(root_ + "/" + object, current) != Error::ok ||
            current.id != id) {
            auto previous = readText(path + ".previous");
            Manifest backup;
            if (objectName(previous) && inspect(root_ + "/" + previous, backup) == Error::ok &&
                backup.id == id) {
                if (!writeAtomic(path,
                        {reinterpret_cast<const uint8_t *>(previous.data()), previous.size()}))
                    return Error::io_error;
            }
        }
    }
    collectObjects(root_);
    return Error::ok;
}
Error PackageManager::inspect(const std::string &path, Manifest &m) const {
    File file;
    Package package;
    return inspectOpened(path, m, file, package);
}
Error PackageManager::inspectOpened(
    const std::string &path, Manifest &manifest, File &file, Package &package) const {
    package.close();
    if (!file.open(path))
        return Error::io_error;
    const auto error = package.open(file.source());
    return error == Error::ok ? validateApplication(package, manifest, available_, capabilities_)
                              : error;
}

static void scan(const std::string &path, std::string_view suffix,
    const std::function<bool(const std::string &)> &visitor) {
#ifdef _WIN32
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(path, error)) {
        const auto name = entry.path().filename().string();
        if (name.size() > suffix.size() && name.ends_with(suffix) && !visitor(name)) {
            break;
        }
    }
#else
    auto *directory = opendir(path.c_str());
    if (!directory) {
        return;
    }
    while (const auto *entry = readdir(directory)) {
        const std::string name = entry->d_name;
        if (name.size() > suffix.size() && name.ends_with(suffix) && !visitor(name)) {
            break;
        }
    }
    closedir(directory);
#endif
}
void PackageManager::visit(const std::function<bool(const Installed &)> &visitor) const {
    // Keep a bounded set of factory identities, not full manifests or package contents.
    std::array<std::string, 32> protectedIds;
    size_t protectedCount = 0;
    bool complete = true;
    bool proceed = true;
    scan(factory_, ".ignit", [&](const auto &name) {
        Manifest manifest;
        const auto path = factory_ + "/" + name;
        if (inspect(path, manifest) == Error::ok) {
            if (protectedCount < protectedIds.size())
                protectedIds[protectedCount++] = manifest.id;
            else
                complete = false;
            proceed = visitor({std::move(manifest), path, true});
        }
        return proceed;
    });
    if (!proceed) {
        return;
    }
    scan(root_, ".ref", [&](const auto &name) {
        const auto id = name.substr(0, name.size() - 4);
        if (!identifier(id)) {
            return true;
        }
        const auto object = readText(root_ + "/" + name);
        if (!objectName(object)) {
            return true;
        }
        Manifest manifest;
        const auto path = root_ + "/" + object;
        if (inspect(path, manifest) != Error::ok || manifest.id != id) {
            return true;
        }
        bool protectedId = std::find(protectedIds.begin(), protectedIds.begin() + protectedCount,
                               id) != protectedIds.begin() + protectedCount;
        if (!complete && !protectedId)
            scan(factory_, ".ignit", [&](const auto &factoryName) {
                Manifest factoryManifest;
                if (inspect(factory_ + "/" + factoryName, factoryManifest) == Error::ok &&
                    factoryManifest.id == id) {
                    protectedId = true;
                    return false;
                }
                return true;
            });
        return protectedId || visitor({std::move(manifest), path, false});
    });
}
std::vector<Installed> PackageManager::list() const {
    std::vector<Installed> result;
    visit([&](const Installed &installed) {
        result.push_back(installed);
        return true;
    });
    return result;
}
Error PackageManager::locate(std::string_view id, Installed &result) const {
    File file;
    Package package;
    return locate(id, result, file, package);
}
Error PackageManager::locate(
    std::string_view id, Installed &result, File &file, Package &package) const {
    if (!identifier(id)) {
        return Error::invalid_argument;
    }
    bool found = false;
    scan(factory_, ".ignit", [&](const auto &name) {
        Manifest manifest;
        const auto path = factory_ + "/" + name;
        if (inspectOpened(path, manifest, file, package) == Error::ok && manifest.id == id) {
            result = {std::move(manifest), path, true};
            found = true;
            return false;
        }
        return true;
    });
    if (found)
        return Error::ok;
    const auto object = readText(root_ + "/" + std::string(id) + ".ref");
    if (!objectName(object))
        return Error::source_unavailable;
    const auto path = root_ + "/" + object;
    Manifest manifest;
    if (inspectOpened(path, manifest, file, package) != Error::ok || manifest.id != id)
        return Error::source_unavailable;
    result = {std::move(manifest), path, false};
    return Error::ok;
}
namespace {
class DirectoryCursor {
    bool missing_{};
#ifdef _WIN32
    std::filesystem::directory_iterator current_, end_;
#else
    DIR *directory_{};
#endif
public:
    DirectoryCursor() = default;
    DirectoryCursor(const DirectoryCursor &) = delete;
    DirectoryCursor &operator=(const DirectoryCursor &) = delete;
    bool missing() const {
        return missing_;
    }
    ~DirectoryCursor() {
        close();
    }
    void close() {
#ifdef _WIN32
        current_ = {};
#else
        if (directory_)
            closedir(directory_);
        directory_ = nullptr;
#endif
    }
    Error open(const std::string &path) {
        close();
        missing_ = false;
#ifdef _WIN32
        std::error_code error;
        current_ = std::filesystem::directory_iterator(path, error);
        missing_ = error == std::errc::no_such_file_or_directory;
        return error ? Error::io_error : Error::ok;
#else
        directory_ = opendir(path.c_str());
        missing_ = !directory_ && errno == ENOENT;
        return directory_ ? Error::ok : Error::io_error;
#endif
    }
    Error next(std::string &name, bool &present) {
        present = false;
#ifdef _WIN32
        if (current_ == end_)
            return Error::ok;
        name = current_->path().filename().string();
        std::error_code error;
        current_.increment(error);
        if (error)
            return Error::io_error;
#else
        if (!directory_)
            return Error::invalid_state;
        errno = 0;
        const auto *entry = readdir(directory_);
        if (!entry)
            return errno ? Error::io_error : Error::ok;
        name = entry->d_name;
#endif
        present = true;
        return Error::ok;
    }
};
static std::string hexDigest(const std::array<uint8_t, 32> &digest) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (const auto byte : digest) {
        result += hex[byte >> 4];
        result += hex[byte & 15];
    }
    return result;
}
} // namespace
struct PreparedInstall::Impl {
    enum class Phase {
        candidate_package,
        candidate_application,
        factory_next,
        factory_package,
        factory_application,
        current_open,
        current_package,
        current_application,
        ready
    };
    PackageManager *owner{};
    uint64_t revision{};
    MemoryBudget available{};
    uint64_t capabilities{};
    std::string root, factory, expectedHash, previousRef;
    File candidateFile, auxiliaryFile;
    Package candidatePackage, auxiliaryPackage;
    Manifest candidateManifest, auxiliaryManifest;
    PackageValidation candidateValidation, auxiliaryValidation;
    ApplicationValidation candidateApplication, auxiliaryApplication;
    DirectoryCursor directory;
    uint32_t total{};
    Phase phase{Phase::candidate_package};
    Error error{Error::ok};
    bool committed{};
    bool consumed{};
    bool requireFactory{};
    void clearAuxiliary() {
        auxiliaryPackage.close();
        auxiliaryFile.close();
        auxiliaryManifest = {};
    }
};
PreparedInstall::PreparedInstall(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
PreparedInstall::~PreparedInstall() = default;
bool PreparedInstall::ready() const {
    return !impl_->consumed && impl_->error == Error::ok && impl_->phase == Impl::Phase::ready;
}
bool PreparedInstall::committed() const {
    return impl_->committed && !impl_->consumed;
}
uint32_t PreparedInstall::size() const {
    return impl_->total;
}
uint32_t PreparedInstall::verifiedBytes() const {
    return impl_->candidateValidation.verifiedBytes();
}
const Manifest &PreparedInstall::manifest() const {
    return impl_->candidateManifest;
}
File &PreparedInstall::file() {
    return impl_->candidateFile;
}
Package &PreparedInstall::package() {
    return impl_->candidatePackage;
}
Manifest PreparedInstall::releaseManifest() {
    impl_->consumed = true;
    return std::move(impl_->candidateManifest);
}
Error PreparedInstall::advance(uint32_t budget) {
    auto &state = *impl_;
    using Phase = Impl::Phase;
    if (state.committed)
        return Error::invalid_state;
    if (state.error != Error::ok || state.phase == Phase::ready)
        return state.error;
    if (budget < EntrySize)
        return Error::invalid_argument;
    if (state.owner->revision() != state.revision)
        return state.error = Error::stale_handle;
    Error error = Error::ok;
    switch (state.phase) {
    case Phase::candidate_package:
        error = state.candidateValidation.advance(budget);
        if (error == Error::ok && state.candidateValidation.done()) {
            if (!state.expectedHash.empty() &&
                hexDigest(state.candidateValidation.transferDigest()) != state.expectedHash)
                return state.error = Error::integrity_failed;
            error = state.candidateApplication.begin(state.candidatePackage,
                state.candidateManifest, state.available, state.capabilities);
            state.phase = Phase::candidate_application;
        }
        break;
    case Phase::candidate_application:
        error = state.candidateApplication.advance(budget);
        if (error == Error::ok && state.candidateApplication.done()) {
            error = state.directory.open(state.factory);
            if (error != Error::ok && state.directory.missing() && !state.requireFactory) {
                error = Error::ok;
                state.phase = Phase::current_open;
            } else
                state.phase = Phase::factory_next;
        }
        break;
    case Phase::factory_next: {
        std::string name;
        bool present = false;
        error = state.directory.next(name, present);
        if (error != Error::ok)
            break;
        if (!present) {
            state.directory.close();
            state.phase = Phase::current_open;
        } else if (name.size() > 6 && name.ends_with(".ignit")) {
            state.clearAuxiliary();
            if (!state.auxiliaryFile.open(state.factory + "/" + name))
                return state.error = Error::io_error;
            error = state.auxiliaryValidation.begin(
                state.auxiliaryPackage, state.auxiliaryFile.source());
            state.phase = Phase::factory_package;
        }
        break;
    }
    case Phase::factory_package:
    case Phase::current_package: {
        const bool factory = state.phase == Phase::factory_package;
        error = state.auxiliaryValidation.advance(budget);
        if (error == Error::ok && state.auxiliaryValidation.done()) {
            error = state.auxiliaryApplication.begin(state.auxiliaryPackage,
                state.auxiliaryManifest, state.available, state.capabilities);
            state.phase = factory ? Phase::factory_application : Phase::current_application;
        }
        // A broken user reference may be repaired; a broken read-only factory
        // catalog must not make protected identities available for replacement.
        if (error != Error::ok && !factory) {
            state.clearAuxiliary();
            state.phase = Phase::ready;
            error = Error::ok;
        }
        break;
    }
    case Phase::factory_application:
    case Phase::current_application: {
        const bool factory = state.phase == Phase::factory_application;
        error = state.auxiliaryApplication.advance(budget);
        if (error != Error::ok && !factory) {
            state.clearAuxiliary();
            state.phase = Phase::ready;
            error = Error::ok;
        } else if (error == Error::ok && state.auxiliaryApplication.done()) {
            const auto &candidate = state.candidateManifest;
            const auto &current = state.auxiliaryManifest;
            if (factory) {
                if (current.id == candidate.id)
                    return state.error = Error::permission_denied;
                state.phase = Phase::factory_next;
            } else {
                if (current.id == candidate.id) {
                    if (current.publisher != candidate.publisher)
                        return state.error = Error::permission_denied;
                    if (candidate.version <= current.version)
                        return state.error = Error::incompatible_contract;
                }
                state.phase = Phase::ready;
            }
            state.clearAuxiliary();
        }
        break;
    }
    case Phase::current_open: {
        state.previousRef = readText(state.root + "/" + state.candidateManifest.id + ".ref");
        if (!objectName(state.previousRef) ||
            !state.auxiliaryFile.open(state.root + "/" + state.previousRef)) {
            state.phase = Phase::ready;
        } else {
            error = state.auxiliaryValidation.begin(
                state.auxiliaryPackage, state.auxiliaryFile.source());
            state.phase = Phase::current_package;
            if (error != Error::ok) {
                state.clearAuxiliary();
                state.phase = Phase::ready;
                error = Error::ok;
            }
        }
        break;
    }
    case Phase::ready:
        break;
    }
    return state.error = error;
}
Error PackageManager::prepareCommit(std::unique_ptr<PreparedInstall> &prepared,
    std::string_view transferHash, uint32_t expectedSize) {
    if ((!transferHash.empty() &&
            (transferHash.size() != 64 ||
                !std::all_of(transferHash.begin(), transferHash.end(),
                    [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }))) ||
        expectedSize > MaxPackageBytes)
        return Error::invalid_argument;
    auto state = std::unique_ptr<PreparedInstall::Impl>(new (std::nothrow) PreparedInstall::Impl);
    if (!state)
        return Error::resource_exhausted;
    state->owner = this;
    state->revision = revision_;
    state->root = root_;
    state->factory = factory_;
    state->available = available_;
    state->capabilities = capabilities_;
    state->requireFactory = requireFactory_;
    state->expectedHash = transferHash;
    if (!state->candidateFile.open(staging()))
        return Error::io_error;
    state->total = state->candidateFile.size;
    if (expectedSize && expectedSize != state->total)
        return Error::invalid_state;
    auto error = state->candidateValidation.begin(
        state->candidatePackage, state->candidateFile.source(), !transferHash.empty());
    if (error != Error::ok)
        return error;
    auto result =
        std::unique_ptr<PreparedInstall>(new (std::nothrow) PreparedInstall(std::move(state)));
    if (!result)
        return Error::resource_exhausted;
    prepared = std::move(result);
    return Error::ok;
}
Error PackageManager::commit(PreparedInstall &prepared, std::string &id) {
    auto &state = *prepared.impl_;
    if (state.owner != this || !prepared.ready() || state.committed)
        return Error::invalid_state;
    if (state.revision != revision_)
        return Error::stale_handle;
    File currentStaging;
    if (!state.candidateFile.unchanged() || !currentStaging.open(staging()) ||
        !state.candidateFile.sameObject(currentStaging))
        return Error::stale_handle;
    const auto &manifest = state.candidateManifest;
    const auto ref = root_ + "/" + manifest.id + ".ref";
    if (readText(ref) != state.previousRef)
        return Error::stale_handle;
    const auto object = hexDigest(state.candidatePackage.digest()) + ".ignit";
    if (!state.previousRef.empty() &&
        !writeAtomic(ref + ".previous",
            {reinterpret_cast<const uint8_t *>(state.previousRef.data()),
                state.previousRef.size()}))
        return Error::io_error;
    if (replaceFile(staging().c_str(), (root_ + "/" + object).c_str()))
        return Error::io_error;
    if (!writeAtomic(ref, {reinterpret_cast<const uint8_t *>(object.data()), object.size()}))
        return Error::io_error;
    ++revision_;
    state.committed = true;
    collectObjects(root_);
    id = manifest.id;
    return Error::ok;
}
Error PackageManager::commit(const std::string &staging, std::string &id) {
    if (staging != this->staging())
        return Error::invalid_argument;
    std::unique_ptr<PreparedInstall> prepared;
    auto error = prepareCommit(prepared);
    while (error == Error::ok && !prepared->ready())
        error = prepared->advance(4096);
    return error == Error::ok ? commit(*prepared, id) : error;
}
Error PackageManager::remove(std::string_view id) {
    if (!identifier(id))
        return Error::invalid_argument;
    Installed p;
    auto error = locate(id, p);
    if (error != Error::ok)
        return error;
    if (p.factory)
        return Error::permission_denied;
    const auto data = root_ + "/app-" + std::string(id) + ".kv";
    if (unlink(data.c_str()) && errno != ENOENT)
        return Error::io_error;
    auto ref = root_ + "/" + std::string(id) + ".ref";
    if (unlink(ref.c_str()))
        return Error::io_error;
    ++revision_;
    unlink((ref + ".previous").c_str());
    // Immutable objects can be shared by current/previous refs. Delete only unreferenced objects.
    for (auto &object : files(root_, ".ignit")) {
        bool used = false;
        for (auto &name : files(root_, ".ref")) {
            auto path = root_ + "/" + name;
            if (readText(path) == object || readText(path + ".previous") == object) {
                used = true;
                break;
            }
        }
        if (!used)
            unlink((root_ + "/" + object).c_str());
    }
    return Error::ok;
}
Error PackageManager::rollback(std::string_view id) {
    if (!identifier(id))
        return Error::invalid_argument;
    auto ref = root_ + "/" + std::string(id) + ".ref";
    auto previous = readText(ref + ".previous");
    if (!objectName(previous))
        return Error::source_unavailable;
    Manifest m;
    auto error = inspect(root_ + "/" + previous, m);
    if (error != Error::ok || m.id != id)
        return Error::invalid_package;
    if (!writeAtomic(ref, {reinterpret_cast<const uint8_t *>(previous.data()), previous.size()}))
        return Error::io_error;
    ++revision_;
    return Error::ok;
}
Error privateStorage(const std::string &root, const Manifest &manifest, AppService operation,
    std::string_view key, std::string_view value, AppReply &reply) {
    if (!identifier(manifest.id) || !identifier(manifest.publisher) || !appKey(key) ||
        value.size() > MaxAppString || !appText(value))
        return Error::invalid_argument;
    if (operation != AppService::storage_get && operation != AppService::storage_set &&
        operation != AppService::storage_delete)
        return Error::not_supported;
    const auto path = root + "/app-" + manifest.id + ".kv";
    std::map<std::string, std::string, std::less<>> values;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_size < 4 || st.st_size > 4096)
            return Error::invalid_package;
        const auto data = readText(path, 4096);
        if (data.size() != size_t(st.st_size))
            return Error::io_error;
        const std::span bytes(reinterpret_cast<const uint8_t *>(data.data()), data.size());
        Reader checksum{bytes.last(4)};
        if (crc32(bytes.first(bytes.size() - 4)) != checksum.n())
            return Error::integrity_failed;
        Reader r{bytes.first(bytes.size() - 4)};
        if (r.n() != 0x31564b46)
            return Error::invalid_package; // FKV1
        if (r.text(64) != manifest.id || r.text(64) != manifest.publisher)
            return Error::permission_denied;
        const auto count = r.n(2);
        if (count > 16)
            return Error::invalid_package;
        for (size_t i = 0; i < count; ++i) {
            const auto k = r.text(32), v = r.text(MaxAppString);
            if (!appKey(k) || !appText(v) || !values.emplace(k, v).second)
                return Error::invalid_package;
        }
        if (!r.done())
            return Error::invalid_package;
    } else if (errno != ENOENT)
        return Error::io_error;
    const auto found = values.find(key);
    if (operation == AppService::storage_get)
        return found == values.end() ? Error::source_unavailable : reply.assign(found->second);
    if (operation == AppService::storage_set) {
        if (found == values.end() && values.size() == 16)
            return Error::quota_exceeded;
        values[std::string(key)] = value;
    } else if (found != values.end())
        values.erase(found);
    else
        return Error::source_unavailable;
    Writer w;
    w.n(0x31564b46);
    w.text(manifest.id);
    w.text(manifest.publisher);
    w.n(values.size(), 2);
    for (const auto &[k, v] : values) {
        w.text(k);
        w.text(v);
    }
    w.n(crc32(w.data));
    if (w.data.size() > 4096)
        return Error::quota_exceeded;
    return writeAtomic(path, w.data) ? Error::ok : Error::io_error;
}
} // namespace flint
