/**
 * @file        bdengine/file_cache.cpp
 *
 * @brief       File cache implementation. See file_cache.h for details.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include "bdengine/file_cache.h"
#include "bdengine/file.h"


#include <rex/logging.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

const std::vector<uint8_t>& bd::XNativeFile::cached_data() const {
    return cached_->data;
}

size_t bd::XNativeFile::cached_size() const {
    return cached_->data.size();
}

namespace bd {

static FileCache g_file_cache;
FileCache& GetFileCache() { return g_file_cache; }

FileCache::FileCache(size_t max_bytes, size_t max_file_size)
    : max_bytes_(max_bytes), max_file_size_(max_file_size) {}

std::shared_ptr<CachedBlob> FileCache::Get(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto it = cache_.find(key);
    if (it == cache_.end()) return nullptr;
    it->second->last_access = ++access_counter_;
    return it->second;
}

void FileCache::Put(const std::string& key, std::vector<uint8_t> data) {
    if (data.size() > max_file_size_) return;

    std::lock_guard lock(mutex_);

    // Remove old entry if replacing
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        current_bytes_ -= it->second->data.size();
        cache_.erase(it);
    }

    // Evict until we have room
    while (current_bytes_ + data.size() > max_bytes_ && !cache_.empty()) {
        EvictLRU();
    }

    auto blob = std::make_shared<CachedBlob>();
    blob->last_access = ++access_counter_;
    current_bytes_ += data.size();
    blob->data = std::move(data);
    cache_[key] = std::move(blob);
}

void FileCache::Evict(const std::string& key) {
    std::lock_guard lock(mutex_);
    auto it = cache_.find(key);
    if (it != cache_.end()) {
        current_bytes_ -= it->second->data.size();
        cache_.erase(it);
    }
}

void FileCache::Flush() {
    std::lock_guard lock(mutex_);
    cache_.clear();
    current_bytes_ = 0;
}

size_t FileCache::current_bytes() const {
    std::lock_guard lock(mutex_);
    return current_bytes_;
}

void FileCache::EvictLRU() {
    // Find entry with lowest last_access
    auto victim = cache_.end();
    uint64_t oldest = UINT64_MAX;
    for (auto it = cache_.begin(); it != cache_.end(); ++it) {
        if (it->second->last_access < oldest) {
            oldest = it->second->last_access;
            victim = it;
        }
    }
    if (victim != cache_.end()) {
        REXLOG_DEBUG("FileCache: evicting {} ({}KB)",
                     victim->first, victim->second->data.size() / 1024);
        current_bytes_ -= victim->second->data.size();
        cache_.erase(victim);
    }
}

} // namespace bd
