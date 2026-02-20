/**
 * @file        bdengine/file_hooks.cpp
 *
 * @brief       Native CRT File I/O Hooks. Replaces the game's recompiled PPC
 *              CRT wrappers (CreateFileA, ReadFile, SetFilePointer, etc.) with
 *              native implementations using std::fstream and std::filesystem.
 *
 *              All handles (file, find, kernel) live in the SDK's ObjectTable 
 *              at 0xF8000000+. CloseHandle is a single ReleaseHandle call,
 *              the XNativeFile / XNativeFind destructors handle cleanup.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "bdengine/file_cache.h"
#include "bdengine/bd_init.h"
#include "bdengine/file.h"
#include "bdengine/ramdisk.h"

#include <rex/runtime.h>
#include <rex/runtime/guest/function.h>
#include <rex/kernel/kernel_state.h>
#include <rex/filesystem.h>
#include <rex/filesystem/entry.h>
#include <rex/logging.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <shlobj.h>  // SHGetKnownFolderPath (save directory lookup only)
#endif
#include <algorithm>
#include <cstdint>
#include <ios>
#include <system_error>
#include <utility>
#include <vector>
#include <rex/byte_order.h>
#include <rex/kernel/xtypes.h>
#include <rex/runtime/guest/context.h>
#include <rex/runtime/guest/types.h>

using rex::X_HANDLE;

using namespace rex::runtime::guest;

namespace {

// SDK filesystem constants
using rex::filesystem::FileAccess;
using rex::filesystem::kFileAttributeDirectory;
using rex::filesystem::kFileAttributeNormal;

// CreateFile dwCreationDisposition (Win32 values, no SDK equivalent)
constexpr uint32_t kCreateNew        = 1;
constexpr uint32_t kCreateAlways     = 2;
constexpr uint32_t kOpenExisting     = 3;
constexpr uint32_t kOpenAlways       = 4;
constexpr uint32_t kTruncateExisting = 5;

//=============================================================================
// Path translation
//=============================================================================

static std::filesystem::path g_game_dir;
static std::filesystem::path g_save_dir;

static std::pair<std::string, std::string> SplitDevicePath(const char* xbox_path) {
    std::string full(xbox_path);
    auto colon_pos = full.find(':');
    if (colon_pos == std::string::npos)
        return {"", full};

    std::string device = full.substr(0, colon_pos);
    size_t start = colon_pos + 1;
    if (start < full.size() && (full[start] == '\\' || full[start] == '/'))
        start++;
    std::string rel = full.substr(start);

    for (auto& c : device) c = (char)tolower((unsigned char)c);

    return {device, rel};
}

static std::filesystem::path TranslatePath(const char* xbox_path) {
    auto [device, rel] = SplitDevicePath(xbox_path);

    // Normalize separators to forward slash (works on both platforms)
    for (auto& c : rel) {
        if (c == '\\') c = '/';
    }

    if (device == "save") {
        // Query ContentManager for the currently open save package path.
        // ContentPackage mounts a specific save subdirectory (e.g.
        // user_data_root/4D5307DF/00000001/<save_name>/) — we need that
        // exact path, not the parent g_save_dir.
        auto* rt = rex::Runtime::instance();
        if (rt && rt->kernel_state()) {
            auto* cm = rt->kernel_state()->content_manager();
            if (cm) {
                auto pkg_path = cm->GetOpenPackagePath("save");
                if (!pkg_path.empty()) {
                    return pkg_path / rel;
                }
            }
        }
        // Fallback: save not yet opened via XAM
        return g_save_dir / rel;
    }

    // game:\, d:\, cache:\, and everything else -> game_dir
    return g_game_dir / rel;
}

//=============================================================================
// Helper: get kernel state and object table
//=============================================================================
static rex::kernel::KernelState* KS() {
    return rex::kernel::kernel_state();
}

static rex::kernel::util::ObjectTable* OT() {
    return KS()->object_table();
}

// Map Win32 CreateFile parameters to std::ios::openmode.
static std::ios::openmode MapOpenMode(uint32_t access, uint32_t disposition) {
    bool write = (access & (FileAccess::kGenericWrite | FileAccess::kGenericAll)) != 0;
    std::ios::openmode mode = std::ios::binary;

    switch (disposition) {
        case kOpenExisting:
            // Open existing file. Fail if doesn't exist.
            mode |= std::ios::in;
            if (write) mode |= std::ios::out;
            break;
        case kCreateAlways:
            // Create or truncate.
            mode |= std::ios::in | std::ios::out | std::ios::trunc;
            break;
        case kCreateNew:
            // Create new. Caller must check exists() beforehand.
            mode |= std::ios::in | std::ios::out | std::ios::trunc;
            break;
        case kOpenAlways:
            // Open or create. Caller creates file first if missing.
            mode |= std::ios::in;
            if (write) mode |= std::ios::out;
            break;
        case kTruncateExisting:
            // Truncate existing. Fail if doesn't exist.
            mode |= std::ios::in | std::ios::out | std::ios::trunc;
            break;
        default:
            mode |= std::ios::in;
            if (write) mode |= std::ios::out;
            break;
    }
    return mode;
}

// Try to open as a cached read-only file.  Returns null on miss / not eligible.
static bd::XNativeFile* TryOpenCached(rex::kernel::KernelState* ks,
                                      const char* name, uint32_t access,
                                      uint32_t disposition) {
    // Only cache pure read-only opens of existing files
    bool read_only = (access & (FileAccess::kGenericWrite | FileAccess::kGenericAll)) == 0;
    bool open_existing = (disposition == kOpenExisting);
    if (!read_only || !open_existing) return nullptr;

    auto& cache = bd::GetFileCache();

    // Cache hit?
    auto blob = cache.Get(name);
    if (blob) {
        REXLOG_DEBUG("CreateFileA: CACHE HIT {}", name);
        return new bd::XNativeFile(ks, std::move(blob), name);
    }

    // Cache miss -- open, slurp if small enough, cache for next time.
    auto host_path = TranslatePath(name);
    std::error_code ec;
    auto file_size = std::filesystem::file_size(host_path, ec);
    if (ec || file_size == 0 || file_size > cache.max_file_size())
        return nullptr;

    std::ifstream ifs(host_path, std::ios::binary);
    if (!ifs.is_open()) return nullptr;

    std::vector<uint8_t> data(static_cast<size_t>(file_size));
    ifs.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!ifs || static_cast<size_t>(ifs.gcount()) != data.size()) return nullptr;

    cache.Put(name, std::vector<uint8_t>(data));  // store in cache
    auto blob2 = cache.Get(name);
    if (!blob2) return nullptr;

    REXLOG_DEBUG("CreateFileA: CACHED {} ({}KB)", name, data.size() / 1024);
    return new bd::XNativeFile(ks, std::move(blob2), name);
}

dword_result_t XCreateFileA_entry(
    lpstring_t lpFileName,
    dword_t dwDesiredAccess,
    dword_t dwShareMode,
    lpvoid_t lpSecurityAttributes,
    dword_t dwCreationDisposition,
    dword_t dwFlagsAndAttributes,
    dword_t hTemplateFile)
{
    const char* name = (const char*)lpFileName;
    auto* ks = KS();
    uint32_t access = (uint32_t)dwDesiredAccess;
    uint32_t disp = (uint32_t)dwCreationDisposition;

    // Ramdisk path: cache:\ goes to in-memory store
    if (bd::IsCachePath(name)) {
        auto key = bd::NormalizeCachePath(name);
        bool exists = bd::RamdiskExists(key);

        if (disp == kOpenExisting && !exists) {
            REXLOG_DEBUG("CreateFileA: RAMDISK not found {}", name);
            return 0xFFFFFFFF;
        }

        auto blob = bd::RamdiskGetOrCreate(key);
        if (disp == kCreateAlways || disp == kTruncateExisting) {
            std::lock_guard lock(blob->mutex);
            blob->data.clear();
        }

        auto* xfile = new bd::XNativeFile(ks, blob, name, true);
        X_HANDLE guest_handle = X_INVALID_HANDLE_VALUE;
        ks->object_table()->AddHandle(xfile, &guest_handle);
        REXLOG_DEBUG("CreateFileA: {} -> 0x{:08X} (ramdisk{})", name, guest_handle,
                     exists ? "" : " new");
        return guest_handle;
    }

    // Try cache path for read-only opens
    if (auto* cached = TryOpenCached(ks, name, access, disp)) {
        X_HANDLE guest_handle = X_INVALID_HANDLE_VALUE;
        ks->object_table()->AddHandle(cached, &guest_handle);
        REXLOG_DEBUG("CreateFileA: {} -> 0x{:08X} (cached)", name, guest_handle);
        return guest_handle;
    }

    // Normal fstream path
    auto host_path = TranslatePath(name);

    // Handle dispositions that require existence checks
    std::error_code ec;
    bool file_exists = std::filesystem::exists(host_path, ec);

    if (disp == kOpenExisting && !file_exists) {
        REXLOG_DEBUG("CreateFileA: FAILED (not found) {}", name);
        return 0xFFFFFFFF;
    }
    if (disp == kTruncateExisting && !file_exists) {
        REXLOG_DEBUG("CreateFileA: FAILED (not found for truncate) {}", name);
        return 0xFFFFFFFF;
    }
    if (disp == kCreateNew && file_exists) {
        REXLOG_DEBUG("CreateFileA: FAILED (already exists) {}", name);
        return 0xFFFFFFFF;
    }

    // OPEN_ALWAYS: create the file first if it doesn't exist,
    // then open with in|out (fstream won't create with in|out alone).
    if (disp == kOpenAlways && !file_exists) {
        std::ofstream touch(host_path, std::ios::binary);
        touch.close();
    }

    auto mode = MapOpenMode(access, disp);
    std::fstream stream(host_path, mode);
    if (!stream.is_open()) {
        REXLOG_DEBUG("CreateFileA: FAILED (open) {}", name);
        return 0xFFFFFFFF;
    }

    auto* xfile = new bd::XNativeFile(ks, std::move(stream), host_path, name);
    X_HANDLE guest_handle = X_INVALID_HANDLE_VALUE;
    ks->object_table()->AddHandle(xfile, &guest_handle);

    REXLOG_DEBUG("CreateFileA: {} -> 0x{:08X}", name, guest_handle);
    return guest_handle;
}

dword_result_t XReadFile_entry(
    dword_t hFile,
    lpvoid_t lpBuffer,
    dword_t nNumberOfBytesToRead,
    lpdword_t lpNumberOfBytesRead,
    lpvoid_t lpOverlapped)
{
    auto xfile = OT()->LookupObject<bd::XNativeFile>((X_HANDLE)(uint32_t)hFile);
    if (!xfile) {
        REXLOG_WARN("ReadFile: invalid handle 0x{:08X}", (uint32_t)hFile);
        if (lpNumberOfBytesRead) *lpNumberOfBytesRead = 0;
        return 0;
    }

    // Ramdisk file: serve from in-memory blob
    if (xfile->is_ramdisk()) {
        if (lpOverlapped) {
            auto* ov_be = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpOverlapped);
            uint64_t offset = (static_cast<uint64_t>((uint32_t)ov_be[3]) << 32) | (uint32_t)ov_be[2];
            xfile->RamdiskSeekTo(offset);
        }
        uint32_t bytes_read = xfile->RamdiskRead((void*)lpBuffer, (uint32_t)nNumberOfBytesToRead);
        if (lpOverlapped) {
            auto* ov_be = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpOverlapped);
            ov_be[0] = 0;
            ov_be[1] = bytes_read;
        } else if (lpNumberOfBytesRead) {
            *lpNumberOfBytesRead = bytes_read;
        }
        return 1;
    }

    // Cached file: serve from memory
    if (xfile->is_cached()) {
        if (lpOverlapped) {
            auto* ov_be = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpOverlapped);
            uint64_t offset = (static_cast<uint64_t>((uint32_t)ov_be[3]) << 32) | (uint32_t)ov_be[2];
            xfile->CachedSeekTo(offset);
        }
        uint32_t bytes_read = xfile->CachedRead((void*)lpBuffer, (uint32_t)nNumberOfBytesToRead);
        if (lpOverlapped) {
            auto* ov_be = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpOverlapped);
            ov_be[0] = 0;
            ov_be[1] = bytes_read;
        } else if (lpNumberOfBytesRead) {
            *lpNumberOfBytesRead = bytes_read;
        }
        return 1;
    }

    // Normal fstream path
    auto& stream = xfile->stream();

    if (lpOverlapped) {
        auto* ov_be = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpOverlapped);
        uint32_t offset_lo = ov_be[2];
        uint32_t offset_hi = ov_be[3];
        int64_t offset = (static_cast<int64_t>(offset_hi) << 32) | offset_lo;
        stream.seekg(offset, std::ios::beg);
    }

    stream.read(reinterpret_cast<char*>((void*)lpBuffer), (uint32_t)nNumberOfBytesToRead);
    uint32_t bytes_read = static_cast<uint32_t>(stream.gcount());

    // Clear EOF/fail bits after partial read (fstream sets failbit if read < requested)
    if (stream.eof() || stream.fail()) {
        stream.clear();
    }

    if (lpOverlapped) {
        auto* ov_be = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpOverlapped);
        ov_be[0] = 0;
        ov_be[1] = bytes_read;
    } else if (lpNumberOfBytesRead) {
        *lpNumberOfBytesRead = bytes_read;
    }
    return 1;
}

dword_result_t XWriteFile_entry(
    dword_t hFile,
    lpvoid_t lpBuffer,
    dword_t nNumberOfBytesToWrite,
    lpdword_t lpNumberOfBytesWritten,
    lpvoid_t lpOverlapped)
{
    auto xfile = OT()->LookupObject<bd::XNativeFile>((X_HANDLE)(uint32_t)hFile);
    if (!xfile) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
        return 0;
    }

    // Ramdisk write
    if (xfile->is_ramdisk()) {
        uint32_t written = xfile->RamdiskWrite((const void*)lpBuffer,
                                               (uint32_t)nNumberOfBytesToWrite);
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = written;
        return 1;
    }

    auto& stream = xfile->stream();
    auto pos_before = stream.tellp();
    stream.write(reinterpret_cast<const char*>((const void*)lpBuffer), (uint32_t)nNumberOfBytesToWrite);

    if (stream.fail()) {
        if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = 0;
        return 0;
    }

    uint32_t bytes_written = static_cast<uint32_t>(stream.tellp() - pos_before);
    if (lpNumberOfBytesWritten) *lpNumberOfBytesWritten = bytes_written;
    return 1;
}

dword_result_t XSetFilePointer_entry(
    dword_t hFile,
    dword_t lDistanceToMove,
    lpdword_t lpDistanceToMoveHigh,
    dword_t dwMoveMethod)
{
    auto xfile = OT()->LookupObject<bd::XNativeFile>((X_HANDLE)(uint32_t)hFile);
    if (!xfile) return 0xFFFFFFFF;

    if (xfile->is_ramdisk()) {
        uint32_t result = xfile->RamdiskSeek((int32_t)(uint32_t)lDistanceToMove,
                                             (uint32_t)dwMoveMethod);
        if (lpDistanceToMoveHigh) *lpDistanceToMoveHigh = 0;
        return result;
    }

    if (xfile->is_cached()) {
        uint32_t result = xfile->CachedSeek((int32_t)(uint32_t)lDistanceToMove,
                                            (uint32_t)dwMoveMethod);
        if (lpDistanceToMoveHigh) *lpDistanceToMoveHigh = 0;
        return result;
    }

    // Map Win32 seek method to std::ios seek direction
    std::ios::seekdir dir;
    switch ((uint32_t)dwMoveMethod) {
        case bd::kSeekBegin:   dir = std::ios::beg; break;
        case bd::kSeekCurrent: dir = std::ios::cur; break;
        case bd::kSeekEnd:     dir = std::ios::end; break;
        default:               return 0xFFFFFFFF;
    }

    int64_t distance = (int32_t)(uint32_t)lDistanceToMove;
    if (lpDistanceToMoveHigh) {
        distance |= static_cast<int64_t>((int32_t)(uint32_t)*lpDistanceToMoveHigh) << 32;
    }

    auto& stream = xfile->stream();
    stream.seekg(distance, dir);
    stream.seekp(distance, dir);

    if (stream.fail()) {
        stream.clear();
        return 0xFFFFFFFF;
    }

    auto pos = static_cast<uint64_t>(stream.tellg());
    if (lpDistanceToMoveHigh)
        *lpDistanceToMoveHigh = static_cast<uint32_t>(pos >> 32);

    return static_cast<uint32_t>(pos & 0xFFFFFFFF);
}

dword_result_t XGetFileSize_entry(dword_t hFile, lpdword_t lpFileSizeHigh) {
    auto xfile = OT()->LookupObject<bd::XNativeFile>((X_HANDLE)(uint32_t)hFile);
    if (!xfile) return 0xFFFFFFFF;

    if (xfile->is_ramdisk()) {
        if (lpFileSizeHigh) *lpFileSizeHigh = 0;
        return static_cast<uint32_t>(xfile->RamdiskSize());
    }

    if (xfile->is_cached()) {
        if (lpFileSizeHigh) *lpFileSizeHigh = 0;
        return static_cast<uint32_t>(xfile->cached_size());
    }

    std::error_code ec;
    auto size = std::filesystem::file_size(xfile->host_path(), ec);
    if (ec) return 0xFFFFFFFF;

    if (lpFileSizeHigh) *lpFileSizeHigh = static_cast<uint32_t>(size >> 32);
    return static_cast<uint32_t>(size & 0xFFFFFFFF);
}

dword_result_t XGetFileSizeEx_entry(dword_t hFile, lpvoid_t lpFileSize) {
    auto xfile = OT()->LookupObject<bd::XNativeFile>((X_HANDLE)(uint32_t)hFile);
    if (!xfile) return 0;

    if (xfile->is_ramdisk()) {
        if (lpFileSize) {
            auto* out = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpFileSize);
            out[0] = 0;
            out[1] = static_cast<uint32_t>(xfile->RamdiskSize());
        }
        return 1;
    }

    if (xfile->is_cached()) {
        if (lpFileSize) {
            auto* out = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpFileSize);
            out[0] = 0;
            out[1] = static_cast<uint32_t>(xfile->cached_size());
        }
        return 1;
    }

    std::error_code ec;
    auto size = std::filesystem::file_size(xfile->host_path(), ec);
    if (ec) return 0;

    if (lpFileSize) {
        auto* out = reinterpret_cast<rex::be<uint32_t>*>((uint8_t*)(void*)lpFileSize);
        out[0] = static_cast<uint32_t>(size >> 32);
        out[1] = static_cast<uint32_t>(size & 0xFFFFFFFF);
    }
    return 1;
}

dword_result_t XSetEndOfFile_entry(dword_t hFile) {
    auto xfile = OT()->LookupObject<bd::XNativeFile>((X_HANDLE)(uint32_t)hFile);
    if (!xfile) return 0;
    if (xfile->is_ramdisk()) return xfile->RamdiskSetEndOfFile() ? 1u : 0u;

    // Flush pending writes, get current position, truncate.
    auto& stream = xfile->stream();
    stream.flush();
    auto pos = static_cast<uintmax_t>(stream.tellp());
    std::error_code ec;
    std::filesystem::resize_file(xfile->host_path(), pos, ec);
    return ec ? 0u : 1u;
}

dword_result_t XFlushFileBuffers_entry(dword_t hFile) {
    auto xfile = OT()->LookupObject<bd::XNativeFile>((X_HANDLE)(uint32_t)hFile);
    if (!xfile) return 0;
    if (xfile->is_ramdisk()) return 1;  // no-op for in-memory
    if (xfile->is_cached()) return 1;   // no-op for cached
    xfile->stream().flush();
    return 1;
}

dword_result_t XDeleteFileA_entry(lpstring_t lpFileName) {
    const char* name = (const char*)lpFileName;

    if (bd::IsCachePath(name)) {
        auto key = bd::NormalizeCachePath(name);
        bool ok = bd::RamdiskDelete(key);
        REXLOG_DEBUG("DeleteFileA: {} (ramdisk {})", name, ok ? "OK" : "not found");
        return ok ? 1u : 0u;
    }

    auto host_path = TranslatePath(name);
    std::error_code ec;
    bool ok = std::filesystem::remove(host_path, ec);
    if (!ok) {
        REXLOG_DEBUG("DeleteFileA: FAILED {}", name);
    }
    return ok ? 1u : 0u;
}

dword_result_t XMoveFileA_entry(lpstring_t lpExistingFileName, lpstring_t lpNewFileName) {
    const char* src = (const char*)lpExistingFileName;
    const char* dst = (const char*)lpNewFileName;

    // Both cache paths: ramdisk rename
    if (bd::IsCachePath(src) && bd::IsCachePath(dst)) {
        auto src_key = bd::NormalizeCachePath(src);
        auto dst_key = bd::NormalizeCachePath(dst);
        bool ok = bd::RamdiskRename(src_key, dst_key);
        REXLOG_DEBUG("MoveFileA: {} -> {} (ramdisk {})", src, dst, ok ? "OK" : "FAILED");
        return ok ? 1u : 0u;
    }

    auto host_src = TranslatePath(src);
    auto host_dst = TranslatePath(dst);
    std::error_code ec;
    std::filesystem::rename(host_src, host_dst, ec);
    if (ec) {
        REXLOG_DEBUG("MoveFileA: FAILED {} -> {}", src, dst);
        return 0;
    }
    return 1;
}

dword_result_t XCloseHandle_entry(dword_t hObject) {
    uint32_t h = (uint32_t)hObject;

    if (h == 0xFFFFFFFF || h == 0) return 0;

    auto* ot = OT();
    if (!ot) return 0;

    auto status = ot->ReleaseHandle(h);
    if (XFAILED(status)) {
        REXLOG_WARN("CloseHandle: unknown handle 0x{:08X}", h);
        return 0;
    }
    return 1;
}

static void FillGuestFindData(uint8_t* dst, const std::filesystem::directory_entry& entry) {
    memset(dst, 0, 0x140);

    auto* be32 = reinterpret_cast<rex::be<uint32_t>*>(dst);

    // File attributes
    uint32_t attrs = entry.is_directory() ? kFileAttributeDirectory : kFileAttributeNormal;
    be32[0] = attrs;

    // Timestamps: zeroed (indices 1-6)
    // File size (indices 7=high, 8=low)
    if (!entry.is_directory()) {
        std::error_code ec;
        auto size = entry.file_size(ec);
        if (!ec) {
            be32[7] = static_cast<uint32_t>(size >> 32);
            be32[8] = static_cast<uint32_t>(size & 0xFFFFFFFF);
        }
    }

    // Filename at offset 0x2C (260 bytes max)
    std::string filename = entry.path().filename().string();
    size_t copy_len = std::min<size_t>(filename.size(), 259);
    std::memcpy(dst + 0x2C, filename.c_str(), copy_len);
    dst[0x2C + copy_len] = '\0';
    // Alternate filename at offset 0x130 (14 bytes) -- leave zeroed
}

dword_result_t XFindFirstFileA_entry(lpstring_t lpFileName, lpvoid_t lpFindFileData) {
    const char* name = (const char*)lpFileName;

    // Split the path: directory portion + glob pattern
    auto host_path = TranslatePath(name);
    auto parent = host_path.parent_path();
    auto pattern = host_path.filename().string();

    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) {
        REXLOG_DEBUG("FindFirstFileA: dir not found {}", name);
        return 0xFFFFFFFF;
    }

    std::filesystem::directory_iterator iter(parent, ec);
    if (ec) {
        REXLOG_DEBUG("FindFirstFileA: cannot iterate {}", name);
        return 0xFFFFFFFF;
    }

    // Advance to first matching entry
    std::filesystem::directory_iterator end;
    while (iter != end) {
        if (bd::XNativeFind::GlobMatch(pattern, iter->path().filename().string()))
            break;
        ++iter;
    }

    if (iter == end) {
        REXLOG_DEBUG("FindFirstFileA: not found {}", name);
        return 0xFFFFFFFF;
    }

    FillGuestFindData((uint8_t*)(void*)lpFindFileData, *iter);

    auto* ks = KS();
    auto* xfind = new bd::XNativeFind(ks, std::move(iter), pattern);
    X_HANDLE guest_handle = X_INVALID_HANDLE_VALUE;
    ks->object_table()->AddHandle(xfind, &guest_handle);

    REXLOG_DEBUG("FindFirstFileA: {} -> 0x{:08X}", name, guest_handle);
    return guest_handle;
}

dword_result_t XFindNextFileA_entry(dword_t hFindFile, lpvoid_t lpFindFileData) {
    auto xfind = OT()->LookupObject<bd::XNativeFind>((X_HANDLE)(uint32_t)hFindFile);
    if (!xfind) return 0;

    if (!xfind->Next()) return 0;

    FillGuestFindData((uint8_t*)(void*)lpFindFileData, xfind->current());
    return 1;
}

}  // namespace

//=============================================================================
// Public helpers
//=============================================================================
namespace bd {

std::filesystem::path GetUserContentRoot() {
#ifdef _WIN32
    // Use SHGetKnownFolderPath to get the real Documents folder.
    // %USERPROFILE%\Documents is WRONG when OneDrive folder backup is enabled.
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path))) {
        std::filesystem::path result(path);
        CoTaskMemFree(path);
        return result / "reblue";
    }
    // Fallback: environment variable
    if (const char* profile = std::getenv("USERPROFILE")) {
        return std::filesystem::path(profile) / "Documents" / "reblue";
    }
    return {};
#else
    // Linux: XDG_DATA_HOME or ~/.local/share
    if (const char* xdg = std::getenv("XDG_DATA_HOME")) {
        return std::filesystem::path(xdg) / "reblue";
    }
    if (const char* home = std::getenv("HOME")) {
        return std::filesystem::path(home) / ".local" / "share" / "reblue";
    }
    return {};
#endif
}

}  // namespace bd

//=============================================================================
// Initialization
//=============================================================================
namespace bd {

void InitFileHooks(const std::filesystem::path& game_dir) {
    g_game_dir = game_dir;

    // Save fallback dir: user_data_root/4D5307DF/00000001
    // (matches ContentManager's expected structure)
    auto user_content_root = GetUserContentRoot();
    if (!user_content_root.empty()) {
        g_save_dir = user_content_root / "4D5307DF" / "00000001";
    } else {
        g_save_dir = g_game_dir / "4D5307DF" / "00000001";
    }

    REXLOG_INFO("File I/O hooks initialized");
    REXLOG_INFO("  game_dir: {}", g_game_dir.string());
    REXLOG_INFO("  save_dir: {}", g_save_dir.string());
}

}  // namespace bd

//=============================================================================
// Hook registrations
//=============================================================================
GUEST_FUNCTION_HOOK(rex_CreateFileA,         XCreateFileA_entry)
GUEST_FUNCTION_HOOK(rex_ReadFile,            XReadFile_entry)
GUEST_FUNCTION_HOOK(rex_WriteFile,           XWriteFile_entry)
GUEST_FUNCTION_HOOK(rex_SetFilePointer,      XSetFilePointer_entry)
GUEST_FUNCTION_HOOK(rex_SetEndOfFile,        XSetEndOfFile_entry)
GUEST_FUNCTION_HOOK(rex_GetFileSize,         XGetFileSize_entry)
GUEST_FUNCTION_HOOK(rex_GetFileSizeEx,       XGetFileSizeEx_entry)
GUEST_FUNCTION_HOOK(rex_FlushFileBuffers,    XFlushFileBuffers_entry)
GUEST_FUNCTION_HOOK(rex_DeleteFileA,         XDeleteFileA_entry)
GUEST_FUNCTION_HOOK(rex_CloseHandle,         XCloseHandle_entry)
GUEST_FUNCTION_HOOK(rex_FindFirstFileA,      XFindFirstFileA_entry)
GUEST_FUNCTION_HOOK(rex_FindNextFileA,       XFindNextFileA_entry)
GUEST_FUNCTION_HOOK(rex_MoveFileA,           XMoveFileA_entry)
