/**
 * @file        bdengine/file.h
 *
 * @brief       XNativeFile — XObject wrapper for native file handles.
 *              Supports three modes: std::fstream, cached in-memory blob,
 *              and ramdisk (read/write in-memory for cache:\ partition).
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include "bdengine/ramdisk.h"

#include <rex/kernel/xobject.h>
#include <rex/kernel/kernel_state.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>
#include <cctype>

namespace bd {

// Win32 seek constants -- the guest PPC code passes these values.
constexpr uint32_t kSeekBegin   = 0;  // FILE_BEGIN
constexpr uint32_t kSeekCurrent = 1;  // FILE_CURRENT
constexpr uint32_t kSeekEnd     = 2;  // FILE_END
constexpr uint32_t kInvalidSetFilePointer = 0xFFFFFFFF;

struct CachedBlob;

// File handle: wraps std::fstream, in-memory cache, or ramdisk blob.
// Uses a custom type (not Type::File) so the SDK's ContentManager doesn't
// confuse these with XFile objects when iterating the ObjectTable.
class XNativeFile : public rex::kernel::XObject {
public:
    static const rex::kernel::XObject::Type kObjectType =
        static_cast<rex::kernel::XObject::Type>(100);

    // Normal mode: wraps a std::fstream.
    XNativeFile(rex::kernel::KernelState* ks, std::fstream&& stream,
                std::filesystem::path host_path, std::string xbox_path)
        : XObject(ks, kObjectType), stream_(std::move(stream)),
          host_path_(std::move(host_path)), path_(std::move(xbox_path)) {}

    // Cached mode: serves reads from an in-memory blob.
    XNativeFile(rex::kernel::KernelState* ks, std::shared_ptr<CachedBlob> blob,
                std::string path)
        : XObject(ks, kObjectType),
          cached_(std::move(blob)), path_(std::move(path)) {}

    // Ramdisk mode: read/write in-memory file for cache:\ partition.
    XNativeFile(rex::kernel::KernelState* ks, std::shared_ptr<RamdiskBlob> ram,
                std::string path, bool /*ramdisk_tag*/)
        : XObject(ks, kObjectType),
          ramdisk_(std::move(ram)), path_(std::move(path)) {}

    ~XNativeFile() override = default;

    bool is_cached() const { return cached_ != nullptr; }
    bool is_ramdisk() const { return ramdisk_ != nullptr; }
    bool is_stream() const { return stream_.is_open(); }
    std::fstream& stream() { return stream_; }
    const std::filesystem::path& host_path() const { return host_path_; }
    const std::string& path() const { return path_; }

    // Cached-mode I/O (read-only)
    const std::vector<uint8_t>& cached_data() const;
    size_t cached_size() const;

    uint32_t CachedRead(void* buffer, uint32_t bytes_requested) {
        auto sz = cached_size();
        if (position_ >= sz) return 0;
        uint32_t avail = static_cast<uint32_t>(std::min<size_t>(bytes_requested, sz - position_));
        std::memcpy(buffer, cached_data().data() + position_, avail);
        position_ += avail;
        return avail;
    }

    uint32_t CachedSeek(int32_t distance, uint32_t method) {
        auto sz = static_cast<int64_t>(cached_size());
        int64_t new_pos;
        switch (method) {
            case kSeekBegin:   new_pos = distance; break;
            case kSeekCurrent: new_pos = static_cast<int64_t>(position_) + distance; break;
            case kSeekEnd:     new_pos = sz + distance; break;
            default:           return kInvalidSetFilePointer;
        }
        if (new_pos < 0) new_pos = 0;
        if (new_pos > sz) new_pos = sz;
        position_ = static_cast<size_t>(new_pos);
        return static_cast<uint32_t>(position_);
    }

    void CachedSeekTo(uint64_t offset) {
        position_ = static_cast<size_t>(std::min<uint64_t>(offset, cached_size()));
    }

    // Ramdisk-mode I/O (read/write in-memory)
    uint32_t RamdiskRead(void* buffer, uint32_t bytes_requested) {
        std::lock_guard lock(ramdisk_->mutex);
        size_t sz = ramdisk_->data.size();
        if (position_ >= sz) return 0;
        uint32_t avail = static_cast<uint32_t>(std::min<size_t>(bytes_requested, sz - position_));
        std::memcpy(buffer, ramdisk_->data.data() + position_, avail);
        position_ += avail;
        return avail;
    }

    uint32_t RamdiskWrite(const void* buffer, uint32_t bytes_to_write) {
        std::lock_guard lock(ramdisk_->mutex);
        size_t end = position_ + bytes_to_write;
        if (end > ramdisk_->data.size())
            ramdisk_->data.resize(end);
        std::memcpy(ramdisk_->data.data() + position_, buffer, bytes_to_write);
        position_ += bytes_to_write;
        return bytes_to_write;
    }

    uint32_t RamdiskSeek(int32_t distance, uint32_t method) {
        std::lock_guard lock(ramdisk_->mutex);
        auto sz = static_cast<int64_t>(ramdisk_->data.size());
        int64_t new_pos;
        switch (method) {
            case kSeekBegin:   new_pos = distance; break;
            case kSeekCurrent: new_pos = static_cast<int64_t>(position_) + distance; break;
            case kSeekEnd:     new_pos = sz + distance; break;
            default:           return kInvalidSetFilePointer;
        }
        if (new_pos < 0) new_pos = 0;
        if (new_pos > sz) new_pos = sz;
        position_ = static_cast<size_t>(new_pos);
        return static_cast<uint32_t>(position_);
    }

    size_t RamdiskSize() {
        std::lock_guard lock(ramdisk_->mutex);
        return ramdisk_->data.size();
    }

    bool RamdiskSetEndOfFile() {
        std::lock_guard lock(ramdisk_->mutex);
        ramdisk_->data.resize(position_);
        return true;
    }

    void RamdiskSeekTo(uint64_t offset) {
        std::lock_guard lock(ramdisk_->mutex);
        position_ = static_cast<size_t>(std::min<uint64_t>(offset, ramdisk_->data.size()));
    }

    std::shared_ptr<RamdiskBlob> ramdisk_blob() const { return ramdisk_; }

private:
    std::fstream stream_;
    std::filesystem::path host_path_;
    std::shared_ptr<CachedBlob> cached_;
    std::shared_ptr<RamdiskBlob> ramdisk_;
    size_t position_ = 0;
    std::string path_;
};

// Wraps a directory_iterator for FindFirstFile/FindNextFile emulation.
class XNativeFind : public rex::kernel::XObject {
public:
    static const rex::kernel::XObject::Type kObjectType =
        rex::kernel::XObject::Type::Enumerator;

    XNativeFind(rex::kernel::KernelState* ks,
        std::filesystem::directory_iterator iter,
        std::string glob_pattern)
        : XObject(ks, kObjectType),
        iter_(std::move(iter)), end_(),
        glob_(std::move(glob_pattern)) {
    }

    ~XNativeFind() override = default;

    // Advance to the next matching entry. Returns false when exhausted.
    bool Next() {
        if (iter_ == end_) return false;
        ++iter_;
        while (iter_ != end_) {
            if (GlobMatch(glob_, iter_->path().filename().string()))
                return true;
            ++iter_;
        }
        return false;
    }

    // Get the current directory entry (only valid if iterator != end).
    const std::filesystem::directory_entry& current() const { return *iter_; }
    bool valid() const { return iter_ != end_; }

    // Simple glob matcher for * and ? wildcards.
    static bool GlobMatch(const std::string& pattern, const std::string& name) {
        return GlobMatchImpl(pattern.c_str(), name.c_str());
    }

private:
    static bool GlobMatchImpl(const char* p, const char* s) {
        while (*p && *s) {
            if (*p == '*') {
                p++;
                if (!*p) return true;  // trailing * matches everything
                while (*s) {
                    if (GlobMatchImpl(p, s)) return true;
                    s++;
                }
                return false;
            }
            else if (*p == '?' || tolower((unsigned char)*p) == tolower((unsigned char)*s)) {
                p++; s++;
            }
            else {
                return false;
            }
        }
        while (*p == '*') p++;
        return !*p && !*s;
    }

    std::filesystem::directory_iterator iter_;
    std::filesystem::directory_iterator end_;
    std::string glob_;
};

} // namespace bd
