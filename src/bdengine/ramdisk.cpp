/**
 * @file        bdengine/ramdisk.cpp
 *
 * @brief       In-memory ramdisk implementation for cache:\ partition.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include "bdengine/ramdisk.h"

#include <cctype>
#include <cstring>
#include <unordered_map>
#include <utility>

#ifdef _MSC_VER
#define StrNoCaseCmpN(a, b, n) _strnicmp(a, b, n)
#else
#define StrNoCaseCmpN(a, b, n) strncasecmp(a, b, n)
#endif

namespace bd {

static std::mutex g_ramdisk_mutex;
static std::unordered_map<std::string, std::shared_ptr<RamdiskBlob>> g_ramdisk;

bool IsCachePath(const char* xbox_path) {
    return (StrNoCaseCmpN(xbox_path, "cache:", 6) == 0);
}

std::string NormalizeCachePath(const char* xbox_path) {
    std::string s(xbox_path);
    for (auto& c : s) {
        if (c == '/') c = '\\';
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

std::shared_ptr<RamdiskBlob> RamdiskGetOrCreate(const std::string& key) {
    std::lock_guard lock(g_ramdisk_mutex);
    auto& blob = g_ramdisk[key];
    if (!blob)
        blob = std::make_shared<RamdiskBlob>();
    return blob;
}

std::shared_ptr<RamdiskBlob> RamdiskGet(const std::string& key) {
    std::lock_guard lock(g_ramdisk_mutex);
    auto it = g_ramdisk.find(key);
    if (it == g_ramdisk.end()) return nullptr;
    return it->second;
}

bool RamdiskRename(const std::string& old_key, const std::string& new_key) {
    std::lock_guard lock(g_ramdisk_mutex);
    auto it = g_ramdisk.find(old_key);
    if (it == g_ramdisk.end()) return false;
    auto blob = std::move(it->second);
    g_ramdisk.erase(it);
    g_ramdisk[new_key] = std::move(blob);
    return true;
}

bool RamdiskDelete(const std::string& key) {
    std::lock_guard lock(g_ramdisk_mutex);
    return g_ramdisk.erase(key) > 0;
}

bool RamdiskExists(const std::string& key) {
    std::lock_guard lock(g_ramdisk_mutex);
    return g_ramdisk.count(key) > 0;
}

} // namespace bd
