/**
 * @file        bdengine/heap.cpp
 *
 * @brief       Implementation of MSVC heap hooks (RtlAllocateHeap, RtlFreeHeap, etc.) 
 *              using dlmalloc in a fixed arena.
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */
#include "bdengine/heap.h"

#include <cstring>

#include <dlmalloc.h>

#include "generated/rb2_init.h"

#include <rex/runtime/guest/function.h>
#include <rex/kernel/kernel_state.h>
#include <rex/kernel/xmemory.h>
#include <rex/logging.h>

using namespace rex::runtime::guest;

namespace {

constexpr uint32_t kHeapSize = 256 * 1024 * 1024;  // 256 MB
#ifndef HEAP_ZERO_MEMORY
constexpr uint32_t HEAP_ZERO_MEMORY = 0x00000008;
#endif

mspace g_mspace = nullptr;
uint8_t* g_membase = nullptr;
uint32_t g_arena_base = 0;   // guest base of our mspace arena
uint32_t g_arena_end  = 0;   // guest end (base + size)

inline void* GuestToHost(uint32_t guest_addr) {
    return g_membase + guest_addr;
}

inline uint32_t HostToGuest(void* host_ptr) {
    return static_cast<uint32_t>(static_cast<uint8_t*>(host_ptr) - g_membase);
}

inline bool InArena(uint32_t guest_addr) {
    return guest_addr >= g_arena_base && guest_addr < g_arena_end;
}

dword_result_t RtlAllocateHeap_entry(dword_t hHeap, dword_t dwFlags, dword_t dwBytes) {
    uint32_t size = dwBytes;
    if (size == 0) size = 1;

    void* ptr = mspace_malloc(g_mspace, size);
    if (!ptr) {
        REXLOG_WARN("RtlAllocateHeap: mspace_malloc({}) failed", size);
        return 0;
    }

    uint32_t guest = HostToGuest(ptr);
    if (!InArena(guest)) {
        REXLOG_ERROR("RtlAllocateHeap: mspace returned OOB ptr! guest=0x{:08X} host={}", guest, ptr);
    }

    if (dwFlags & HEAP_ZERO_MEMORY) {
        std::memset(ptr, 0, mspace_usable_size(ptr));
    }

    return guest;
}

dword_result_t RtlFreeHeap_entry(dword_t hHeap, dword_t dwFlags, dword_t ptr) {
    if (!ptr) return 1;
    uint32_t guest = static_cast<uint32_t>(ptr);
    if (!InArena(guest)) {
        REXLOG_WARN("RtlFreeHeap: skipping OOB ptr 0x{:08X} (not in arena 0x{:08X}-0x{:08X})",
                    guest, g_arena_base, g_arena_end);
        return 1;  // pretend success - this was allocated before our hook
    }
    mspace_free(g_mspace, GuestToHost(guest));
    return 1;
}

dword_result_t RtlSizeHeap_entry(dword_t hHeap, dword_t dwFlags, dword_t ptr) {
    if (!ptr) return ~0u;
    uint32_t guest = static_cast<uint32_t>(ptr);
    if (!InArena(guest)) {
        REXLOG_WARN("RtlSizeHeap: OOB ptr 0x{:08X}", guest);
        return ~0u;
    }
    return static_cast<uint32_t>(mspace_usable_size(GuestToHost(guest)));
}

dword_result_t RtlReAllocateHeap_entry(dword_t hHeap, dword_t dwFlags,
                                        dword_t ptr, dword_t dwBytes) {
    uint32_t old_guest = static_cast<uint32_t>(ptr);

    // If old ptr is outside our arena (pre-hook allocation), treat as fresh alloc
    if (old_guest && !InArena(old_guest)) {
        REXLOG_WARN("RtlReAllocateHeap: old ptr 0x{:08X} OOB, treating as new alloc({})",
                    old_guest, static_cast<uint32_t>(dwBytes));
        uint32_t size = dwBytes ? static_cast<uint32_t>(dwBytes) : 1;
        void* new_host = mspace_malloc(g_mspace, size);
        if (!new_host) {
            REXLOG_WARN("RtlReAllocateHeap: mspace_malloc({}) failed", size);
            return 0;
        }
        if (dwFlags & HEAP_ZERO_MEMORY) {
            std::memset(new_host, 0, mspace_usable_size(new_host));
        }
        return HostToGuest(new_host);
    }

    void* old_host = old_guest ? GuestToHost(old_guest) : nullptr;
    size_t old_size = old_host ? mspace_usable_size(old_host) : 0;

    void* new_host = mspace_realloc(g_mspace, old_host, dwBytes ? static_cast<uint32_t>(dwBytes) : 1);
    if (!new_host) {
        REXLOG_WARN("RtlReAllocateHeap: mspace_realloc({}) failed", static_cast<uint32_t>(dwBytes));
        return 0;
    }

    if ((dwFlags & HEAP_ZERO_MEMORY) && mspace_usable_size(new_host) > old_size) {
        std::memset(static_cast<uint8_t*>(new_host) + old_size, 0,
                    mspace_usable_size(new_host) - old_size);
    }

    return HostToGuest(new_host);
}

}  // namespace

GUEST_FUNCTION_HOOK(rex_RtlAllocateHeap, RtlAllocateHeap_entry)
GUEST_FUNCTION_HOOK(rex_RtlFreeHeap, RtlFreeHeap_entry)
GUEST_FUNCTION_HOOK(rex_RtlSizeHeap, RtlSizeHeap_entry)
GUEST_FUNCTION_HOOK(rex_RtlReAllocateHeap, RtlReAllocateHeap_entry)

namespace bd {

bool InitNativeHeap() {
    auto* mem = rex::kernel::kernel_memory();
    if (!mem) {
        REXLOG_ERROR("InitNativeHeap: kernel_memory() is null");
        return false;
    }

    g_membase = mem->virtual_membase();

    uint32_t guest_base = mem->SystemHeapAlloc(kHeapSize);
    if (!guest_base) {
        REXLOG_ERROR("InitNativeHeap: SystemHeapAlloc({}) failed", kHeapSize);
        return false;
    }

    uint8_t* host_base = mem->TranslateVirtual<uint8_t*>(guest_base);
    g_arena_base = guest_base;
    g_arena_end  = guest_base + kHeapSize;
    REXLOG_INFO("InitNativeHeap: guest=0x{:08X}-0x{:08X} host={} size={}MB",
                g_arena_base, g_arena_end, (void*)host_base, kHeapSize / (1024 * 1024));

    g_mspace = create_mspace_with_base(host_base, kHeapSize, 1);
    if (!g_mspace) {
        REXLOG_ERROR("InitNativeHeap: create_mspace_with_base failed");
        mem->SystemHeapFree(guest_base);
        return false;
    }

    // Prevent dlmalloc from falling back to OS mmap when the arena is full.
    // Without this, mspace_malloc can return host pointers outside guest memory.
    mspace_set_footprint_limit(g_mspace, kHeapSize);

    REXLOG_INFO("InitNativeHeap: dlmalloc mspace ready");
    return true;
}

uint32_t HeapAlloc(uint32_t size, bool zero) {
    if (size == 0) size = 1;
    void* ptr = mspace_malloc(g_mspace, size);
    if (!ptr) return 0;
    if (zero) std::memset(ptr, 0, mspace_usable_size(ptr));
    return HostToGuest(ptr);
}

void HeapFree(uint32_t guest_addr) {
    if (!guest_addr) return;
    mspace_free(g_mspace, GuestToHost(guest_addr));
}

}  // namespace bd
