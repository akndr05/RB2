/**
 * @file        bdengine/ramdisk.h
 *
 * @brief       In-memory ramdisk for the cache:\ partition.
 *              Files written to cache:\ live entirely in RAM and are shared
 *              across all open handles to the same normalised path.
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
#include <vector>

namespace bd {

// In-memory ramdisk blob for cache:\ partition.
// Shared across all open handles to the same ramdisk path.
struct RamdiskBlob {
    std::mutex mutex;
    std::vector<uint8_t> data;
};

// Returns true if the Xbox path starts with "cache:" (case-insensitive).
bool IsCachePath(const char* xbox_path);

// Lower-cases and normalises separators for use as a ramdisk map key.
std::string NormalizeCachePath(const char* xbox_path);

// Get or create a blob for the given normalised key.
std::shared_ptr<RamdiskBlob> RamdiskGetOrCreate(const std::string& key);

// Get an existing blob, or nullptr if it doesn't exist.
std::shared_ptr<RamdiskBlob> RamdiskGet(const std::string& key);

// Rename a blob from old_key to new_key.  Returns false if old_key not found.
bool RamdiskRename(const std::string& old_key, const std::string& new_key);

// Delete a blob.  Returns false if key not found.
bool RamdiskDelete(const std::string& key);

// Check whether a blob exists for the given key.
bool RamdiskExists(const std::string& key);

} // namespace bd
