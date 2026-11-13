/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdio.h>
#include "nx/runtime/platform/log.h"
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

/* the driver's calls of one kind since the start: all of them, and those made by a thread that held vk_lock, which
   is a thread in the middle of the renderer's work */
typedef struct {
    u64 n, bytes, ns, slow; /* slow: those that took a millisecond or more */
    u64 max_ns;             /* the longest since the last report */
    u64 locked_n, locked_ns;
} DriverCalls;

/* Every thread that allocates adds to these (the GPU thread, the encoders' workers), each with one atomic add: the
   counts are read for a report only, so they share their lines */
static struct {
    u64 device[VK_OWN_N], host[VK_OWN_N], heap[VK_OWN_N], count[VK_OWN_N];
    DriverCalls allocate, release;
    DriverCalls reported[2]; /* the two above at the last report */
} g_ledger;

static void add(u64* to, u64 v) { __atomic_fetch_add(to, v, __ATOMIC_RELAXED); }
static u64 get(const u64* from) { return __atomic_load_n(from, __ATOMIC_RELAXED); }

/* where an allocation's bytes count: the heap the caches budget is the device's */
static u64* bytes_of(u32 owner, const DevMemInfo* info) {
    const VkPhysicalDeviceMemoryProperties* mp;
    mp = vk_devmem_props();
    return (int)mp->memoryTypes[info->type].heapIndex == vk_mem_heap() ? &g_ledger.device[owner]
                                                                             : &g_ledger.host[owner];
}

void vk_mem_track(DevMem a, u32 owner) {
    if (!a || owner >= VK_OWN_N) return;
    DevMemInfo info;
    vk_devmem_info(a, &info);
    vk_devmem_tag(a, owner + 1);
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
    while (ns > max && !__atomic_compare_exchange_n(&c->max_ns, &max, ns, 1, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {}
}

VkResult VKAPI_CALL vk_ledger_allocate(VkDevice dev, const VkMemoryAllocateInfo* ai, const VkAllocationCallbacks* cb,
                                       VkDeviceMemory* mem) {
    u64 t0 = plat_ns();
    SNX_ZONE_V(z, "driver allocation", ai->allocationSize);
    VkResult r = vkAllocateMemory(dev, ai, cb, mem);
    SNX_ZONE_END(z);
    driver_call(&g_ledger.allocate, r == VK_SUCCESS ? ai->allocationSize : 0, plat_ns() - t0);
    return r;
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
                       .max_ns = __atomic_exchange_n(&c->max_ns, 0, __ATOMIC_RELAXED),
                       .locked_n = get(&c->locked_n),
                       .locked_ns = get(&c->locked_ns)};
    *out = (DriverCalls){.n = now.n - reported->n,
                         .bytes = now.bytes - reported->bytes,
                         .ns = now.ns - reported->ns,
                         .slow = now.slow - reported->slow,
                         .max_ns = now.max_ns,
                         .locked_n = now.locked_n - reported->locked_n,
                         .locked_ns = now.locked_ns - reported->locked_ns};
    *reported = now;
    return out->n;
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
    if (na || nf)
        LOG("vulkan: the driver allocated %llu blocks (%.0f MB) in %.1f ms, the longest %.1f ms, %llu of a millisecond "
            "or more, %llu (%.1f ms) inside the renderer's work; it freed %llu in %.1f ms, the longest %.1f ms, %llu "
            "(%.1f ms) inside the renderer's work\n",
            (unsigned long long)a.n, mb(a.bytes), ms(a.ns), ms(a.max_ns), (unsigned long long)a.slow,
            (unsigned long long)a.locked_n, ms(a.locked_ns), (unsigned long long)f.n, ms(f.ns), ms(f.max_ns),
            (unsigned long long)f.locked_n, ms(f.locked_ns));
}
