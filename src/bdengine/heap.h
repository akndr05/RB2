/**
 * @file        bdengine/heap.h
 *
 * @brief       Native heap using dlmalloc in a fixed arena.  
 *              Replaces the blue dragons's NT heap allocator
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#pragma once

#include <cstdint>

namespace bd {

// Initialize the native dlmalloc heap over guest memory.
// Must be called after rex::Runtime::Setup() and before game module launch.
bool InitNativeHeap();

// Allocate from the native heap. Returns guest virtual address (0 on failure).
// Game can free this via its free() → RtlFreeHeap → our hook.
uint32_t HeapAlloc(uint32_t size, bool zero = false);

// Free a guest address allocated by HeapAlloc.
void HeapFree(uint32_t guest_addr);

}  // namespace bd
