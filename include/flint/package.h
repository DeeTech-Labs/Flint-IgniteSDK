#pragma once
#include "contracts.h"
#include "sha256.h"
#include <algorithm>

namespace flint {
// Wire layout is decoded explicitly; never reinterpret bytes as a packed C++ struct.
inline constexpr std::array<uint8_t, 8> PackageMagic{'F', 'L', 'I', 'G', 'N', 'I', 'T', 0};
inline constexpr uint32_t HeaderSize = 64, EntrySize = 24, MaxEntries = 1024,
                          MaxPackageBytes = 1024 * 1024;
enum class Section : uint16_t { manifest = 1, component, resource, ui, lua, locale, signature };
struct Entry {
    uint32_t id{}, offset{}, size{};
    Section type{};
    uint16_t codec{};
    uint32_t flags{};
};
using ReadAt = bool (*)(void *, uint32_t, std::span<uint8_t>);
struct Source {
    void *context{};
    uint32_t size{};
    ReadAt read{};
};
inline uint16_t u16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t u32(const uint8_t *p) {
    return uint32_t(u16(p)) | uint32_t(u16(p + 2)) << 16;
}
class Package {
    friend class PackageValidation;
    friend class Runtime;
    friend class ApplicationValidation;
    Source source_{};
    uint32_t count_{};
    bool valid_{};
    uint64_t generation_{};
    uint64_t identity_{};
    std::array<uint8_t, 32> digest_{};
    uint32_t manifestIndex_{};
    // Only Runtime may relocate a proof after moving the same open File object.
    void relocate(Source source) {
        source_ = source;
    }

public:
    class View {
        friend class Package;
        const Package *owner_{};
        uint64_t generation_{};
        Entry entry_{};

    public:
        Entry info() const {
            return entry_;
        }
        Error read(uint32_t offset, std::span<uint8_t> bytes) const {
            if (!owner_ || !owner_->valid_ || generation_ != owner_->generation_) {
                return Error::stale_handle;
            }
            if (offset > entry_.size || bytes.size() > entry_.size - offset) {
                return Error::invalid_argument;
            }
            return owner_->source_.read(owner_->source_.context, entry_.offset + offset, bytes)
                ? Error::ok
                : Error::io_error;
        }
    };
    uint64_t identity() const {
        return valid_ ? identity_ : 0;
    }
    void close() {
        valid_ = false;
        ++generation_;
    }
    Error view(uint32_t id, View &result) const {
        if (!valid_) {
            return Error::invalid_state;
        }
        uint32_t low = 0, high = count_;
        while (low < high) {
            const auto index = low + (high - low) / 2;
            Entry entry;
            const auto error = this->entry(index, entry);
            if (error != Error::ok)
                return error;
            if (entry.id < id)
                low = index + 1;
            else if (entry.id > id)
                high = index;
            else {
                result.owner_ = this;
                result.generation_ = generation_;
                result.entry_ = entry;
                return Error::ok;
            }
        }
        return Error::source_unavailable;
    }
    uint32_t count() const {
        return valid_ ? count_ : 0;
    }
    Error entry(uint32_t index, Entry &out) const {
        if (!valid_ || index >= count_)
            return Error::invalid_argument;
        std::array<uint8_t, EntrySize> b{};
        if (!source_.read(source_.context, HeaderSize + index * EntrySize, b))
            return Error::io_error;
        out = {u32(b.data()), u32(b.data() + 8), u32(b.data() + 12), Section(u16(b.data() + 4)),
            u16(b.data() + 6), u32(b.data() + 16)};
        if (u32(b.data() + 20) != 0)
            return Error::invalid_package;
        return Error::ok;
    }
    Error open(Source source);
    Error manifest(Entry &entry) const {
        return valid_ ? this->entry(manifestIndex_, entry) : Error::invalid_package;
    }
    const std::array<uint8_t, 32> &digest() const {
        return digest_;
    }
    Error read(const Entry &entry, uint32_t offset, std::span<uint8_t> bytes) const {
        if (!valid_)
            return Error::invalid_state;
        // Resolve the ID through the validated sorted index, never caller-supplied offsets.
        View section;
        const auto error = view(entry.id, section);
        return error == Error::ok                ? section.read(offset, bytes)
            : error == Error::source_unavailable ? Error::invalid_argument
                                                 : error;
    }
};
// Resumable verification owns no file or data buffer. Source must remain immutable
// until its Package is closed. Each advance reads at most the requested byte budget.
class PackageValidation {
    Package *package_{};
    Source source_{};
    Sha256 hash_, transferHash_;
    std::array<uint8_t, 32> expected_{}, transferDigest_{};
    uint32_t index_{}, end_{}, previousId_{}, manifests_{}, offset_{};
    uint64_t generation_{};
    Error error_{Error::invalid_state};
    bool indexDone_{}, done_{}, transfer_{};
    Error fail(Error error) {
        error_ = error;
        done_ = true;
        if (package_)
            package_->valid_ = false;
        return error;
    }

public:
    Error begin(Package &package, Source source, bool transferHash = false) {
        *this = {};
        package_ = &package;
        source_ = source;
        transfer_ = transferHash;
        package.close();
        generation_ = package.generation_;
        package.count_ = 0;
        package.source_ = source;
        if (!source.read || source.size < HeaderSize || source.size > MaxPackageBytes)
            return fail(Error::invalid_package);
        std::array<uint8_t, HeaderSize> header{};
        if (!source.read(source.context, 0, header))
            return fail(Error::io_error);
        if (!std::equal(PackageMagic.begin(), PackageMagic.end(), header.begin()) ||
            u16(header.data() + 8) != PackageVersion || u16(header.data() + 10) != HeaderSize ||
            u32(header.data() + 16) != source.size || u32(header.data() + 24) != HeaderSize ||
            u32(header.data() + 28) != 0)
            return fail(Error::invalid_package);
        if (u32(header.data() + 12) != ContractRevision)
            return fail(Error::incompatible_contract);
        package.count_ = u32(header.data() + 20);
        if (!package.count_ || package.count_ > MaxEntries ||
            package.count_ > (source.size - HeaderSize) / EntrySize)
            return fail(Error::invalid_package);
        std::copy(header.begin() + 32, header.end(), expected_.begin());
        if (transfer_)
            transferHash_.update(header);
        std::fill(header.begin() + 32, header.end(), 0);
        hash_.update(header);
        end_ = HeaderSize + package.count_ * EntrySize;
        offset_ = HeaderSize;
        error_ = Error::ok;
        return Error::ok;
    }
    Error advance(uint32_t budget) {
        if (done_)
            return error_;
        if (!package_ || budget < EntrySize)
            return Error::invalid_argument;
        if (package_->generation_ != generation_)
            return fail(Error::stale_handle);
        while (!indexDone_ && index_ < package_->count_ && budget >= EntrySize) {
            std::array<uint8_t, EntrySize> bytes{};
            if (!source_.read(source_.context, HeaderSize + index_ * EntrySize, bytes))
                return fail(Error::io_error);
            const auto id = u32(bytes.data()), offset = u32(bytes.data() + 8),
                       size = u32(bytes.data() + 12);
            const auto type = u16(bytes.data() + 4);
            if (id <= previousId_ || offset < end_ || offset > source_.size || !size ||
                size > source_.size - offset || type < 1 || type > 7 || u16(bytes.data() + 6) ||
                u32(bytes.data() + 16) || u32(bytes.data() + 20))
                return fail(Error::invalid_package);
            if (type == uint16_t(Section::manifest)) {
                package_->manifestIndex_ = index_;
                ++manifests_;
            }
            previousId_ = id;
            end_ = offset + size;
            ++index_;
            budget -= EntrySize;
        }
        if (!indexDone_ && index_ == package_->count_) {
            if (manifests_ != 1 || end_ != source_.size)
                return fail(Error::invalid_package);
            indexDone_ = true;
        }
        std::array<uint8_t, 256> buffer{};
        while (indexDone_ && budget && offset_ < source_.size) {
            const auto count =
                std::min<uint32_t>({budget, uint32_t(buffer.size()), source_.size - offset_});
            const auto part = std::span(buffer).first(count);
            if (!source_.read(source_.context, offset_, part))
                return fail(Error::io_error);
            hash_.update(part);
            if (transfer_)
                transferHash_.update(part);
            budget -= count;
            offset_ += count;
        }
        if (indexDone_ && offset_ == source_.size) {
            const auto digest = hash_.finish();
            if (digest != expected_)
                return fail(Error::integrity_failed);
            package_->digest_ = digest;
            package_->identity_ = 0;
            for (unsigned i = 0; i < 8; ++i)
                package_->identity_ |= uint64_t(digest[i]) << (8 * i);
            if (transfer_)
                transferDigest_ = transferHash_.finish();
            package_->valid_ = true;
            done_ = true;
        }
        return Error::ok;
    }
    bool done() const {
        return done_;
    }
    uint32_t verifiedBytes() const {
        return indexDone_ ? offset_ : 0;
    }
    const std::array<uint8_t, 32> &transferDigest() const {
        return transferDigest_;
    }
};
inline Error Package::open(Source source) {
    PackageValidation validation;
    auto error = validation.begin(*this, source);
    while (error == Error::ok && !validation.done())
        error = validation.advance(4096);
    return error;
}
} // namespace flint
