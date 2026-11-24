/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdio.h>
#include <stdatomic.h>
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/gpu/vk/ledger.h"

static const char* const k_owner[VK_OWN_N] = {
    [VK_OWN_TEXTURE] = "textures",
    [VK_OWN_TARGET] = "render targets",
    [VK_OWN_IMAGE_OTHER] = "other images",
    [VK_OWN_BUFFER] = "buffer cache",
    [VK_OWN_MIRROR] = "mirror",
    [VK_OWN_OWNED] = "kept results",
    [VK_OWN_ARENA] = "compute arenas",
    [VK_OWN_WRITELOG] = "write logs",
    [VK_OWN_RESULT] = "draw results",
    [VK_OWN_STAGING] = "rings",
    [VK_OWN_READBACK] = "readback",
    [VK_OWN_ENCODER] = "encoders",
    [VK_OWN_CONVERT] = "conversions",
    [VK_OWN_DESCRIPTORS] = "descriptors",
    [VK_OWN_QUERY] = "queries",
    [VK_OWN_XFB] = "transform feedback",
    [VK_OWN_DMA] = "DMA",
    [VK_OWN_UI] = "overlay",
    [VK_OWN_MISC] = "other",
};

/* Each owner's class (DM_PRIO_*, devmem.h): the order in which the driver moves its memory to system memory when the
   GPU's own memory is short (VK_EXT_pageable_device_local_memory). Moved memory loses nothing, but each use of it by
   the GPU crosses the bus, so the class follows how much each frame uses it, and the order of the caches' own
   evictions (vk_mem_trim):
   - high: the render targets and their copies that draws sample, which each frame's draws write and read many
     times, and the rings and fixed buffers
     that each frame's work goes through (staging, arenas, descriptors, queries, results, the mirror). These are
     also the bytes that guest memory lacks, or that the CPU waits for.
   - medium: the buffer cache and the images of no cache, which draws read but which hold a copy of guest memory or
     of a target. Also the textures that the cache keeps under pressure: a texture with pixels copied from render
     targets (texture.c raises it, tex_drawn), as the cache evicts it only when nothing lossless is left.
   - low: the textures that the cache evicts first, those loaded from memory, which it can load again. The driver
     moves them before anything else, as vk_mem_trim frees them before anything else */
static const u8 k_owner_prio[VK_OWN_N] = {
    [VK_OWN_TEXTURE] = DM_PRIO_LOW,  [VK_OWN_TARGET] = DM_PRIO_HIGH,      [VK_OWN_IMAGE_OTHER] = DM_PRIO_MID,
    [VK_OWN_BUFFER] = DM_PRIO_MID,   [VK_OWN_MIRROR] = DM_PRIO_HIGH,      [VK_OWN_OWNED] = DM_PRIO_HIGH,
    [VK_OWN_ARENA] = DM_PRIO_HIGH,   [VK_OWN_WRITELOG] = DM_PRIO_HIGH,    [VK_OWN_RESULT] = DM_PRIO_HIGH,
    [VK_OWN_STAGING] = DM_PRIO_HIGH, [VK_OWN_READBACK] = DM_PRIO_HIGH,    [VK_OWN_ENCODER] = DM_PRIO_HIGH,
    [VK_OWN_CONVERT] = DM_PRIO_HIGH, [VK_OWN_DESCRIPTORS] = DM_PRIO_HIGH, [VK_OWN_QUERY] = DM_PRIO_HIGH,
    [VK_OWN_XFB] = DM_PRIO_HIGH,     [VK_OWN_DMA] = DM_PRIO_HIGH,         [VK_OWN_UI] = DM_PRIO_HIGH,
    [VK_OWN_MISC] = DM_PRIO_HIGH,
};

/* the driver's calls of one kind since the start: all of them, and those made by a thread that held vk_lock, which
   is a thread in the middle of the renderer's work */
typedef struct {
    _Atomic u64 n, bytes, ns, slow; /* slow: those that took a millisecond or more */
    _Atomic u64 max_ns;             /* the longest since the last report */
    _Atomic u64 locked_n, locked_ns;
} DriverCalls;

/* Every thread that allocates adds to these (the GPU thread, the encoders' workers), each with one atomic add: the
   counts are read for a report only, so they share their lines */
static struct {
    _Atomic u64 device[VK_OWN_N], host[VK_OWN_N], heap[VK_OWN_N], count[VK_OWN_N];
    DriverCalls allocate, release, priority;
    DriverCalls reported[3]; /* the three above at the last report */
} g_ledger;

static void add(_Atomic u64* to, u64 v) { atomic_fetch_add_explicit(to, v, memory_order_relaxed); }
static u64 get(const _Atomic u64* from) { return atomic_load_explicit(from, memory_order_relaxed); }

/* where an allocation's bytes count: the heap the caches budget is the device's */
static _Atomic u64* bytes_of(u32 owner, const DevMemInfo* info) {
    const VkPhysicalDeviceMemoryProperties* mp;
    mp = vk_devmem_props();
    return (int)mp->memoryTypes[info->type].heapIndex == vk_mem_heap() ? &g_ledger.device[owner]
                                                                             : &g_ledger.host[owner];
}

u32 vk_mem_owner_prio(u32 owner) { return owner < VK_OWN_N ? k_owner_prio[owner] : DM_PRIO_MID; }

void vk_mem_track(DevMem a, u32 owner) {
    if (!a || owner >= VK_OWN_N) return;
    DevMemInfo info;
    vk_devmem_info(a, &info);
    vk_devmem_tag(a, owner + 1);
    vk_devmem_priority(a, k_owner_prio[owner]);
    add(bytes_of(owner, &info), info.size);
    add(&g_ledger.count[owner], 1);
    SNX_MEM_ALLOC((const void*)(uintptr_t)a, info.size, k_owner[owner]);
}

void vk_mem_untrack(DevMem a) {
    if (!a) return;
    DevMemInfo info;
    vk_devmem_info(a, &info);
    if (!info.tag || info.tag > VK_OWN_N) return;
    u32 owner = info.tag - 1;
    vk_devmem_tag(a, 0);
    add(bytes_of(owner, &info), 0 - (u64)info.size);
    add(&g_ledger.count[owner], 0 - 1ull);
    SNX_MEM_FREE((const void*)(uintptr_t)a, k_owner[owner]);
}

void vk_ledger_heap(u32 owner, i64 delta) {
    if (owner < VK_OWN_N) add(&g_ledger.heap[owner], (u64)delta);
}

void vk_ledger_owner(u32 owner, u64* device, u64* host, u64* heap) {
    *device = get(&g_ledger.device[owner]);
    *host = get(&g_ledger.host[owner]);
    *heap = get(&g_ledger.heap[owner]);
}

static void driver_call(DriverCalls* c, u64 bytes, u64 ns) {
    add(&c->n, 1);
    add(&c->bytes, bytes);
    add(&c->ns, ns);
    if (ns >= 1000000ull) add(&c->slow, 1);
    if (vk_lock_held()) {
        add(&c->locked_n, 1);
        add(&c->locked_ns, ns);
    }
    u64 max = get(&c->max_ns);
    while (ns > max &&
           !atomic_compare_exchange_weak_explicit(&c->max_ns, &max, ns, memory_order_relaxed, memory_order_relaxed)) {}
}

VkResult VKAPI_CALL vk_ledger_allocate(VkDevice dev, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* cb,
                                       VkDeviceMemory* mem) {
    u64 t0 = plat_ns();
    SNX_ZONE_V(z, "driver allocation", ai->allocationSize);
    SNX_ZONE_TEXT(z, "type %u", ai->memoryTypeIndex); /* which heap the allocator grew in */
    VkResult r = vkAllocateMemory(dev, ai, cb, mem);
    SNX_ZONE_END(z);
    driver_call(&g_ledger.allocate, r == VK_SUCCESS ? ai->allocationSize : 0, plat_ns() - t0);
    return r;
}

void vk_ledger_priority(VkDevice dev, VkDeviceMemory mem, float priority) {
    u64 t0 = plat_ns();
    SNX_ZONE(z, "driver priority");
    vkSetDeviceMemoryPriorityEXT(dev, mem, priority);
    SNX_ZONE_END(z);
    driver_call(&g_ledger.priority, 0, plat_ns() - t0);
}
void VKAPI_CALL vk_ledger_free(VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks* cb) {
    u64 t0 = plat_ns();
    SNX_ZONE(z, "driver free");
    vkFreeMemory(dev, mem, cb);
    SNX_ZONE_END(z);
    driver_call(&g_ledger.release, 0, plat_ns() - t0);
}

static double mb(u64 bytes) { return (double)bytes / 1048576.0; }
static double ms(u64 ns) { return (double)ns / 1e6; }

/* one kind of the driver's calls since the last report, into out; its count */
static u64 driver_report(DriverCalls* c, DriverCalls* reported, DriverCalls* out) {
    DriverCalls now = {.n = get(&c->n),
                       .bytes = get(&c->bytes),
                       .ns = get(&c->ns),
                       .slow = get(&c->slow),
                       .max_ns = atomic_exchange_explicit(&c->max_ns, 0, memory_order_relaxed),
                       .locked_n = get(&c->locked_n),
                       .locked_ns = get(&c->locked_ns)};
    *out = (DriverCalls){.n = atomic_load_explicit(&now.n, memory_order_relaxed) -
                              atomic_load_explicit(&reported->n, memory_order_relaxed),
                         .bytes = atomic_load_explicit(&now.bytes, memory_order_relaxed) -
                                  atomic_load_explicit(&reported->bytes, memory_order_relaxed),
                         .ns = atomic_load_explicit(&now.ns, memory_order_relaxed) -
                               atomic_load_explicit(&reported->ns, memory_order_relaxed),
                         .slow = atomic_load_explicit(&now.slow, memory_order_relaxed) -
                                 atomic_load_explicit(&reported->slow, memory_order_relaxed),
                         .max_ns = atomic_load_explicit(&now.max_ns, memory_order_relaxed),
                         .locked_n = atomic_load_explicit(&now.locked_n, memory_order_relaxed) -
                                     atomic_load_explicit(&reported->locked_n, memory_order_relaxed),
                         .locked_ns = atomic_load_explicit(&now.locked_ns, memory_order_relaxed) -
                                      atomic_load_explicit(&reported->locked_ns, memory_order_relaxed)};
    *reported = now;
    return atomic_load_explicit(&out->n, memory_order_relaxed);
}

u64 vk_ledger_owners(void) {
    char line[320];
    size_t at = 0;
    u64 tagged = 0;
    for (u32 o = 0; o < VK_OWN_N; o++) {
        u64 device = get(&g_ledger.device[o]), host = get(&g_ledger.host[o]), heap = get(&g_ledger.heap[o]);
        tagged += device + host;
        if (!(device | host | heap)) continue;
        int n = snprintf(line + at, sizeof line - at, "%s %s %.1f/%.1f/%.1f in %llu", at ? "," : "", k_owner[o],
                         mb(device), mb(host), mb(heap), (unsigned long long)get(&g_ledger.count[o]));
        if (n > 0) at += (size_t)n < sizeof line - at ? (size_t)n : sizeof line - at - 1;
        if (at > 200) { /* a few owners a line */
            LOG("vulkan: memory by owner (MB of the device, of the host, of the heap, in allocations):%s\n", line);
            at = 0;
        }
    }
    if (at)
        LOG("vulkan: memory by owner (MB of the device, of the host, of the heap, in allocations):%s\n", line);
    return tagged;
}

void vk_ledger_log(void) {
    u64 tagged = vk_ledger_owners();
    /* what devmem.c holds beside the owners: the free space inside its blocks, which is the device's memory all the
       same, and what a site allocated and did not enter */
    DevMemBudget b[VK_MAX_MEMORY_HEAPS];
    const VkPhysicalDeviceMemoryProperties* mp;
    u64 blocks = 0, allocated = 0;
    int heap = vk_mem_heap();
    vk_devmem_budgets(b);
    mp = vk_devmem_props();
    for (u32 h = 0; h < mp->memoryHeapCount; h++) {
        blocks += b[h].block_bytes;
        allocated += b[h].allocated_bytes;
    }
    LOG("vulkan: device memory: %.1f MB in %u blocks, %.1f MB of it free inside them (%.1f MB of that in heap %d); "
        "%.1f MB allocated under no owner; %.1f MB of freed memory waits for its frames\n",
        mb(blocks), b[heap].blocks, mb(blocks - allocated),
        mb(b[heap].block_bytes - b[heap].allocated_bytes), heap,
        (double)(i64)(allocated - tagged) / 1048576.0, mb(vk_deferred_bytes()));
    vk_devmem_log();

    DriverCalls a, f;
    u64 na = driver_report(&g_ledger.allocate, &g_ledger.reported[0], &a);
    u64 nf = driver_report(&g_ledger.release, &g_ledger.reported[1], &f);
    {
        static const char k_calls[] = "vk mem driver allocs";
        static const char k_mb[] = "vk mem driver MB";
        static const char k_ms[] = "vk mem driver ms";
        SNX_PLOT(k_calls, (double)na);
        SNX_PLOT(k_mb, mb(atomic_load_explicit(&a.bytes, memory_order_relaxed)));
        SNX_PLOT(k_ms, ms(atomic_load_explicit(&a.ns, memory_order_relaxed)));
    }
    if (na || nf)
        LOG("vulkan: the driver allocated %llu blocks (%.0f MB) in %.1f ms, the longest %.1f ms, %llu of a millisecond "
            "or more, %llu (%.1f ms) inside the renderer's work; it freed %llu in %.1f ms, the longest %.1f ms, %llu "
            "(%.1f ms) inside the renderer's work\n",
            (unsigned long long)atomic_load_explicit(&a.n, memory_order_relaxed),
            mb(atomic_load_explicit(&a.bytes, memory_order_relaxed)),
            ms(atomic_load_explicit(&a.ns, memory_order_relaxed)),
            ms(atomic_load_explicit(&a.max_ns, memory_order_relaxed)),
            (unsigned long long)atomic_load_explicit(&a.slow, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&a.locked_n, memory_order_relaxed),
            ms(atomic_load_explicit(&a.locked_ns, memory_order_relaxed)),
            (unsigned long long)atomic_load_explicit(&f.n, memory_order_relaxed),
            ms(atomic_load_explicit(&f.ns, memory_order_relaxed)),
            ms(atomic_load_explicit(&f.max_ns, memory_order_relaxed)),
            (unsigned long long)atomic_load_explicit(&f.locked_n, memory_order_relaxed),
            ms(atomic_load_explicit(&f.locked_ns, memory_order_relaxed)));
    DriverCalls p;
    if (driver_report(&g_ledger.priority, &g_ledger.reported[2], &p))
        LOG("vulkan: the driver set %llu priorities of device memory in %.1f ms, the longest %.2f ms, %llu of a "
            "millisecond or more, %llu (%.1f ms) inside the renderer's work\n",
            (unsigned long long)atomic_load_explicit(&p.n, memory_order_relaxed),
            ms(atomic_load_explicit(&p.ns, memory_order_relaxed)),
            ms(atomic_load_explicit(&p.max_ns, memory_order_relaxed)),
            (unsigned long long)atomic_load_explicit(&p.slow, memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&p.locked_n, memory_order_relaxed),
            ms(atomic_load_explicit(&p.locked_ns, memory_order_relaxed)));
}
