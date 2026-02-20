/**
 * @file        bdengine/file_cache.h
 * 
 * @brief       In-memory file cache with LRU eviction. Read-only files 
 *              under a size threshold are cached after first open. 
 *              Subsequent opens for the same path serve entirely from memory.
 * 
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bd {

struct CachedBlob {
    std::vector<uint8_t> data;
    uint64_t last_access = 0;
};

class FileCache {
public:
    explicit FileCache(size_t max_bytes = 64 * 1024 * 1024,
                       size_t max_file_size = 4 * 1024 * 1024);

    // Look up a cached file.  Returns nullptr on miss.
    std::shared_ptr<CachedBlob> Get(const std::string& key);

    // Insert (or replace) a cached file.  Evicts LRU entries if over budget.
    void Put(const std::string& key, std::vector<uint8_t> data);

    // Remove a single entry (e.g. after a write/delete).
    void Evict(const std::string& key);

    // Drop everything.
    void Flush();

    size_t max_file_size() const { return max_file_size_; }
    size_t current_bytes() const;

private:
    void EvictLRU();  // caller holds mutex_

    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<CachedBlob>> cache_;
    size_t max_bytes_;
    size_t max_file_size_;
    size_t current_bytes_ = 0;
    uint64_t access_counter_ = 0;
};

FileCache& GetFileCache();

} // namespace bd
