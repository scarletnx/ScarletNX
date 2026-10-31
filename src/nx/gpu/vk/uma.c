/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The memory model of the device, and guest memory that the GPU reads in place (vk_memory, docs/GPU_OWNERSHIP.md).

   The console's CPU and GPU share one memory: a game writes vertices and indices, and the GPU reads those bytes with no
   copy. The copy path (buffer.c) checks each guest range at each guest submit, and copies what changed into a staging
   ring. On a discrete GPU those bytes then cross PCIe. On a GPU that shares system memory (an integrated GPU: a Steam
   Deck, a laptop, a phone), the staging ring is the same memory as guest memory, and the copy is only cost.

   On such a device (or with vk_memory=2) the guest memory under a draw's vertex, index and read-only storage ranges is
   imported into Vulkan (VK_EXT_external_memory_host) and bound where it is. An import covers the 2 MiB windows that a
   range touches, clipped to mapped guest memory, so that an import is never the whole heap: a driver that checks each
   import's pages at each submit (amdgpu's user pointers) pays for the bytes imported, and one import of a whole guest
   heap cost 45 ms a frame there. Imports do not overlap. A range across two imports makes one import of their union.

   What stays a copy, and why:
   - Constant buffers. The GPU front end writes them between draws (CONST_BUFFER_DATA, r3d_const_upload), on the CPU,
     when the GPU thread meets the method. A draw recorded before that write, but that the GPU runs after it, would read
     the new bytes in place.
   - Ranges that GPU results not landed yet cover (buffer.c's pending results, compute results) and GPU-owned ranges
     (memory/own.c): their bytes are not in guest memory.
   - Writable storage: its results land through the pending results.
   - Textures and render targets: their layout differs (block linear, compressed formats).

   The rules that keep it correct:
   - A release that the guest can see (a syncpoint, a semaphore) is performed only after the GPU ran every command that
     read guest memory in place before the stream stopped (release.c, vk_uma_bound). The copy path read those bytes when
     the commands were recorded. In place, the GPU reads them when it runs them, and after a release the guest writes
     the next frame's data.
   - mem.c tells which guest pages get other host pages (an unmap, a decommit, a move into the alias pool): the imports
     on them go (vk_uma_pages_replaced), as the driver holds the old pages.
   - A GPU-side write (a front end write, a landed result) on a page that a command not finished yet reads in place is
     counted (the hazards line of the report). That command can read the new bytes. */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "common/gpu/vk/vk_common.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"

#define UMA_WINDOW (2ull << 20) /* imports cover whole windows of this size where guest memory is mapped */
#define UMA_MAX    512          /* imports at most: the least recently used goes past it */
#define UMA_IDLE   600          /* presents unused before an import goes */
#define HZ_BITS    16           /* the table of pages read in place, by guest page */

typedef struct {
    u64 cpu, size; /* guest memory [cpu, cpu + size) */
    VkDeviceMemory mem;
    VkBuffer buf;
    u32 used; /* g_vk_present when last bound */
    int dead; /* its pages are other pages now (mem.c): it goes at the next lookup */
} Imp;
/* Spans the driver refused to import (a page it can't pin: a trapped GPU-owned page, say), whose ranges are copied
   until UMA_IDLE presents pass or their pages change. Without them every range in such a window asked the driver
   again, twice: one game fell from 28 to 5 frames a second */
#define FAIL_N 64
typedef struct {
    u64 lo, hi; /* hi 0: none */
    u32 at;     /* g_vk_present when it failed */
} Fail;
typedef struct {
    u64 cpu, len, sub;
} Seen;
#define UMA_LINE 64 /* a cache line (x86-64, AArch64) */
/* The file's state. Different threads write its groups, each apart from the next by a cache line: the GPU thread
   (under vk_lock), the threads that change guest mappings (mem.c's hook, under lk), any thread that writes guest memory
   for the GPU (the hazard count), and the GPU thread again for the page table that those threads read */
typedef struct {
    /* the GPU thread's, under vk_lock */
    Imp imp[UMA_MAX]; /* sorted by cpu, none overlapping, nimp of them; their dead flags under lk */
    u32 nimp;
    VkResult import_result; /* the last import's, for the log */
    u64 align;              /* minImportedHostPointerAlignment */
    u64 bound;              /* the submit that last read guest memory in place (vk_submit_number) */
    Seen seen[256];         /* the ranges hz_mark marked, with their submit */
    /* statistics */
    u64 hits, hit_bytes, misses, imports, import_bytes, import_ns, failed, dropped;
    char apart0[UMA_LINE];
    /* mem.c's hook writes these too */
    PMutex lk;         /* a leaf, under vk_lock or mem.c's lock: the dead flags and the table */
    int any_dead;      /* under lk */
    Fail fail[FAIL_N]; /* under lk */
    char apart1[UMA_LINE];
    u64 hazards; /* any thread's, atomic */
    char apart2[UMA_LINE];
    /* the submit that last read each guest page in place, by page number modulo the table: page << 20 | submit's low
       20 bits would lose order, so the two in their own words. The GPU thread writes them, atomic, and the threads
       that write guest memory for the GPU read them */
    u64 hz_page[1u << HZ_BITS], hz_sub[1u << HZ_BITS];
} Uma;
/* C99 has no static assert: an array of negative size if two groups would share a line */
typedef char uma_apart[offsetof(Uma, lk) - offsetof(Uma, apart0) >= UMA_LINE &&
                               offsetof(Uma, hazards) - offsetof(Uma, apart1) >= UMA_LINE &&
                               offsetof(Uma, hz_page) - offsetof(Uma, apart2) >= UMA_LINE
                           ? 1
                           : -1];
static Uma g_uma = {.lk = PMUTEX_INIT, .align = 4096};

int vk_uma(void) { return g_vk.mem_path == VK_MEM_PATH_UMA; }
u64 vk_uma_bound(void) { return g_uma.bound; }

/* ------------------------------------------------------------ the model */
static const char* model_name(int m) {
    return m == VK_MEM_UMA     ? "shares system memory (UMA)"
           : m == VK_MEM_REBAR ? "discrete, the CPU maps all of its memory (resizable BAR)"
                               : "discrete, the CPU maps a small window of its memory";
}
/* Which memory the device has: an integrated GPU, or one that isn't discrete whose biggest device-local heap the CPU
   maps and holds most of the memory, shares the CPU's. A discrete GPU whose device-local and host-visible memory is at
   least half of its biggest device-local heap has resizable BAR. Else the window is small (256 MiB without resizable
   BAR). */
static int mem_model(void) {
    VkPhysicalDeviceMemoryProperties mp;
    u64 total = 0, dl = 0, dlhv = 0;
    int dl_heap = -1;
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &mp);
    for (u32 h = 0; h < mp.memoryHeapCount; h++) {
        total += mp.memoryHeaps[h].size;
        if ((mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && mp.memoryHeaps[h].size > dl) {
            dl = mp.memoryHeaps[h].size;
            dl_heap = (int)h;
        }
    }
    for (u32 t = 0; t < mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[t].propertyFlags;
        u64 size = mp.memoryHeaps[mp.memoryTypes[t].heapIndex].size;
        if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && size > dlhv)
            dlhv = size;
    }
    if (g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ||
        g_vk.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
        return VK_MEM_UMA;
    /* not a discrete GPU with resizable BAR and more memory of its own than the system's */
    if (g_vk.props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && dl_heap >= 0 && dlhv == dl && dl > total / 2)
        return VK_MEM_UMA;
    if (dl && dlhv >= dl / 2) return VK_MEM_REBAR;
    return VK_MEM_SMALL_BAR;
}
/* before the device is made: the model, and the path that vk_memory (0 auto, 1 copy, 2 UMA) and the device's
   extensions give. 1: VK_EXT_external_memory_host is to be enabled */
int vk_uma_choose(int has_import, int has_placed) {
    int want = g_settings.guest_memory;
    g_vk.mem_model = mem_model();
    g_vk.mem_path = VK_MEM_PATH_COPY;
    if (want == 2 || (want == 0 && g_vk.mem_model == VK_MEM_UMA)) {
        if (has_import)
            g_vk.mem_path = VK_MEM_PATH_UMA;
        else
            WARN("vulkan: guest memory in place was asked for, but the device has no VK_EXT_external_memory_host: "
                 "guest memory is copied\n");
    }
    LOG("vulkan: the device %s; guest buffers %s (vk_memory=%s%s); VK_EXT_external_memory_host %s, "
        "VK_EXT_map_memory_placed %s\n",
        model_name(g_vk.mem_model), g_vk.mem_path == VK_MEM_PATH_UMA ? "read in place where they can be" : "copied",
        want == 1   ? "copy"
        : want == 2 ? "uma"
                    : "auto",
        want == 2 && g_vk.mem_model != VK_MEM_UMA ? ", forced: the GPU reads them across the bus" : "",
        has_import ? "yes" : "no", has_placed ? "yes (not used yet)" : "no");
    return g_vk.mem_path == VK_MEM_PATH_UMA;
}

/* ------------------------------------------------------------ imports */
static void imp_free(void* p) {
    Imp* d = p;
    if (d->buf) vkDestroyBuffer(g_vk.dev, d->buf, NULL);
    if (d->mem) vkFreeMemory(g_vk.dev, d->mem, NULL);
    free(d);
}
/* the import at i goes from the table: its memory once the frame's work is done (vk_lock held) */
static void imp_drop(u32 i) {
    Imp* d = malloc(sizeof *d);
    if (!d) fatal("vulkan: out of memory for an import of guest memory");
    *d = g_uma.imp[i];
    vk_defer_call(imp_free, d);
    g_uma.dropped++;
    pm_lock(&g_uma.lk);
    memmove(&g_uma.imp[i], &g_uma.imp[i + 1], sizeof *g_uma.imp * (size_t)(g_uma.nimp - i - 1));
    g_uma.nimp--;
    pm_unlock(&g_uma.lk);
}
/* mem.c: host pages under guest memory [a, a + size) are other pages now (under its lock). The imports on them hold the
   old ones */
static void pages_replaced(u64 a, u64 size) {
    u64 end = a + size < a ? ~0ull : a + size;
    pm_lock(&g_uma.lk);
    for (u32 i = 0; i < FAIL_N; i++)
        if (g_uma.fail[i].hi && g_uma.fail[i].lo < end && a < g_uma.fail[i].hi)
            g_uma.fail[i].hi = 0; /* other pages: try again */
    for (u32 i = 0; i < g_uma.nimp; i++)
        if (g_uma.imp[i].cpu < end && a < g_uma.imp[i].cpu + g_uma.imp[i].size) {
            g_uma.imp[i].dead = 1;
            g_uma.any_dead = 1;
        }
    pm_unlock(&g_uma.lk);
}
/* the dead ones go (vk_lock held) */
static void drop_dead(void) {
    pm_lock(&g_uma.lk);
    int any = g_uma.any_dead;
    g_uma.any_dead = 0;
    pm_unlock(&g_uma.lk);
    if (!any) return;
    for (u32 i = g_uma.nimp; i-- > 0;) {
        pm_lock(&g_uma.lk);
        int dead = g_uma.imp[i].dead;
        pm_unlock(&g_uma.lk);
        if (dead) imp_drop(i);
    }
}
/* the first import that ends after cpu */
static u32 imp_find(u64 cpu) {
    u32 lo = 0, hi = g_uma.nimp;
    while (lo < hi) {
        u32 mid = (lo + hi) / 2;
        if (g_uma.imp[mid].cpu + g_uma.imp[mid].size <= cpu)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
/* guest memory [cpu, cpu + size) imported as a buffer: 0 when the driver refused it */
static int imp_make(Imp* m, u64 cpu, u64 size) {
    u8* host = GP(cpu);
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .pNext = &ext,
                              .size = size,
                              /* what the staging ring's buffers are bound as (cmd.c), as callers bind either */
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VkMemoryRequirements req;
    const VkPhysicalDeviceMemoryProperties* mp;
    u64 t0 = plat_ns();
    *m = (Imp){.cpu = cpu, .size = size, .used = g_vk_present};
    /* a refusal is expected (pages the driver can't pin) and said once for the span (fail), not as a failed call */
    g_uma.import_result = vkGetMemoryHostPointerPropertiesEXT(
        g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    if (g_uma.import_result != VK_SUCCESS) return 0;
    if (!VK_OK(vkCreateBuffer(g_vk.dev, &bci, NULL, &m->buf))) {
        m->buf = VK_NULL_HANDLE;
        return 0;
    }
    vkGetBufferMemoryRequirements(g_vk.dev, m->buf, &req);
    vmaGetMemoryProperties(g_vk.vma, &mp);
    u32 bits = hp.memoryTypeBits & req.memoryTypeBits, type = ~0u;
    /* the GPU reads it: device-local where the import can be (a GPU that shares memory), else any it takes */
    for (u32 t = 0; t < mp->memoryTypeCount; t++)
        if ((bits >> t) & 1) {
            if (type == ~0u || (mp->memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) type = t;
        }
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = host};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = &imp,
                               .allocationSize = size,
                               .memoryTypeIndex = type};
    if (type == ~0u || req.size > size ||
        (g_uma.import_result = vkAllocateMemory(g_vk.dev, &ai, NULL, &m->mem)) != VK_SUCCESS) {
        vkDestroyBuffer(g_vk.dev, m->buf, NULL);
        m->buf = VK_NULL_HANDLE;
        m->mem = VK_NULL_HANDLE;
        return 0;
    }
    if (!VK_OK(vkBindBufferMemory(g_vk.dev, m->buf, m->mem, 0))) {
        vkDestroyBuffer(g_vk.dev, m->buf, NULL);
        vkFreeMemory(g_vk.dev, m->mem, NULL);
        m->buf = VK_NULL_HANDLE;
        m->mem = VK_NULL_HANDLE;
        return 0;
    }
    g_uma.imports++;
    g_uma.import_bytes += size;
    g_uma.import_ns += plat_ns() - t0;
    return 1;
}
/* whether [lo, hi) meets a span the driver refused lately (vk_lock held) */
static int failed(u64 lo, u64 hi) {
    int r = 0;
    pm_lock(&g_uma.lk);
    for (u32 i = 0; i < FAIL_N && !r; i++)
        r = g_uma.fail[i].hi && (u32)(g_vk_present - g_uma.fail[i].at) <= UMA_IDLE && g_uma.fail[i].lo < hi &&
            lo < g_uma.fail[i].hi;
    pm_unlock(&g_uma.lk);
    return r;
}
/* [lo, hi) refused: in place of the oldest span kept */
static void fail(u64 lo, u64 hi) {
    u32 old = 0;
    g_uma.failed++;
    LOG_LIMIT(8, "vulkan: guest memory %llx+%llx could not be imported (%s): its ranges are copied for a while\n",
              (unsigned long long)lo, (unsigned long long)(hi - lo), snx_vk_result_str(g_uma.import_result));
    pm_lock(&g_uma.lk);
    for (u32 i = 1; i < FAIL_N && g_uma.fail[old].hi; i++)
        if (!g_uma.fail[i].hi || (i32)(g_uma.fail[i].at - g_uma.fail[old].at) < 0) old = i;
    g_uma.fail[old] = (Fail){lo, hi, g_vk_present};
    pm_unlock(&g_uma.lk);
}
/* the import that holds guest memory [cpu, cpu + len), made if there is none: its index, or ~0 for none (the range is
   copied) */
static u32 imp_get(u64 cpu, u64 len) {
    u32 i = imp_find(cpu);
    if (i < g_uma.nimp && g_uma.imp[i].cpu <= cpu && cpu + len <= g_uma.imp[i].cpu + g_uma.imp[i].size) return i;
    /* the windows the range touches, and the imports they meet, as one import */
    u64 lo = cpu & ~(UMA_WINDOW - 1), hi = (cpu + len + UMA_WINDOW - 1) & ~(UMA_WINDOW - 1);
    u32 first = imp_find(lo), last = first;
    while (last < g_uma.nimp && g_uma.imp[last].cpu < hi) {
        if (g_uma.imp[last].cpu < lo) lo = g_uma.imp[last].cpu;
        if (g_uma.imp[last].cpu + g_uma.imp[last].size > hi) hi = g_uma.imp[last].cpu + g_uma.imp[last].size;
        last++;
    }
    /* only mapped guest memory: the range's own pages where the windows are not all mapped */
    if (mem_mapped_run(lo, hi - lo) != hi - lo) {
        if (last > first) return ~0u;
        lo = cpu & ~(g_uma.align - 1);
        hi = (cpu + len + g_uma.align - 1) & ~(g_uma.align - 1);
        if (mem_mapped_run(lo, hi - lo) != hi - lo) return ~0u;
    }
    if (hi - lo > g_vk.alloc_cap || failed(lo, hi)) return ~0u;
    if (g_uma.nimp - (last - first) >= UMA_MAX) { /* full: the least recently used goes */
        u32 old = 0;
        for (u32 k = 1; k < g_uma.nimp; k++)
            if ((i32)(g_uma.imp[k].used - g_uma.imp[old].used) < 0) old = k;
        imp_drop(old);
        return ~0u; /* the indexes moved: the next lookup makes it */
    }
    Imp m;
    if (!imp_make(&m, lo, hi - lo)) {
        fail(lo, hi);
        return ~0u; /* the imports there stay */
    }
    for (u32 k = last; k-- > first;) imp_drop(k);
    pm_lock(&g_uma.lk);
    memmove(&g_uma.imp[first + 1], &g_uma.imp[first], sizeof *g_uma.imp * (size_t)(g_uma.nimp - first));
    g_uma.imp[first] = m;
    g_uma.nimp++;
    pm_unlock(&g_uma.lk);
    return first;
}

/* ------------------------------------------------------------ the renderer's side */
static void hz_mark(u64 cpu, u64 len);
/* The GPU range [gva, gva + len) bound where it is in guest memory, at an offset that is a multiple of align: 1 with
   the buffer and offset, 0 when it is copied (vk_lock held). The caller has checked that no pending GPU result is on
   it and that the GPU does not own it */
int vk_uma_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    u64 cpu = 0;
    if (!len || gmmu_run(gva, len, &cpu) < len) { /* not one run of guest memory */
        g_uma.misses++;
        return 0;
    }
    drop_dead();
    u32 i = imp_get(cpu, len);
    if (i == ~0u || ((cpu - g_uma.imp[i].cpu) % (align ? align : 1))) {
        g_uma.misses++;
        return 0;
    }
    g_uma.imp[i].used = g_vk_present;
    *buf = g_uma.imp[i].buf;
    *off = cpu - g_uma.imp[i].cpu;
    g_uma.bound = vk_submit_number();
    g_uma.hits++;
    g_uma.hit_bytes += len;
    hz_mark(cpu, len);
    return 1;
}
/* the pages of [cpu, cpu + len) as read in place by the submit being recorded: once a submit for each range, as draws
   bind the same ranges again and again */
static void hz_mark(u64 cpu, u64 len) {
    u32 h = (u32)(((cpu ^ (len << 32)) * 0x9E3779B97F4A7C15ull) >> 56);
    if (g_uma.seen[h].cpu == cpu && g_uma.seen[h].len == len && g_uma.seen[h].sub == g_uma.bound) return;
    g_uma.seen[h].cpu = cpu;
    g_uma.seen[h].len = len;
    g_uma.seen[h].sub = g_uma.bound;
    for (u64 p = cpu >> 12; p <= (cpu + len - 1) >> 12; p++) {
        u32 s = (u32)(p & ((1u << HZ_BITS) - 1));
        __atomic_store_n(&g_uma.hz_page[s], p, __ATOMIC_RELAXED);
        __atomic_store_n(&g_uma.hz_sub[s], g_uma.bound, __ATOMIC_RELAXED);
    }
}
/* a GPU-side write to guest memory [cpu, cpu + n): counted when a command that is not done yet read a page of it in
   place (any thread) */
static void gpu_wrote(u64 cpu, u64 n) {
    u64 done = 0;
    if (!n) return;
    for (u64 p = cpu >> 12; p <= (cpu + n - 1) >> 12; p++) {
        u32 s = (u32)(p & ((1u << HZ_BITS) - 1));
        if (__atomic_load_n(&g_uma.hz_page[s], __ATOMIC_RELAXED) != p) continue;
        u64 sub = __atomic_load_n(&g_uma.hz_sub[s], __ATOMIC_RELAXED), dummy;
        if (!done) vk_submit_progress(&dummy, &done);
        if (sub > done) {
            __atomic_add_fetch(&g_uma.hazards, 1, __ATOMIC_RELAXED);
            return;
        }
    }
}

void vk_uma_init(void) {
    if (!vk_uma()) return;
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp};
    vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
    if (hp.minImportedHostPointerAlignment > g_uma.align) g_uma.align = hp.minImportedHostPointerAlignment;
    if (g_uma.align > UMA_WINDOW) {
        WARN("vulkan: imports must be aligned to %llu KiB: guest memory is copied\n",
             (unsigned long long)(g_uma.align >> 10));
        g_vk.mem_path = VK_MEM_PATH_COPY;
        return;
    }
    mem_set_pages_replaced(pages_replaced);
    gpu_set_write_watch(gpu_wrote);
}
/* each present (vk_lock held): imports unused for long go, and every 600 presents the statistics */
void vk_uma_frame(void) {
    if (!vk_uma()) return;
    drop_dead();
    if (g_vk_present % 60 == 0)
        for (u32 i = g_uma.nimp; i-- > 0;)
            if ((u32)(g_vk_present - g_uma.imp[i].used) > UMA_IDLE) imp_drop(i);
    if (g_vk_present % 600) return;
    u64 bytes = 0;
    for (u32 i = 0; i < g_uma.nimp; i++) bytes += g_uma.imp[i].size;
    LOG("vulkan: in place over 600 presents: %llu ranges (%llu MB bound) read in guest memory, %llu copied; %llu "
        "imports made (%llu MB, %llu ms), %llu spans refused, %llu dropped; %u imports hold %llu MB; %llu hazards (a "
        "GPU-side write on a page that unfinished work reads in place)\n",
        (unsigned long long)g_uma.hits, (unsigned long long)(g_uma.hit_bytes >> 20), (unsigned long long)g_uma.misses,
        (unsigned long long)g_uma.imports, (unsigned long long)(g_uma.import_bytes >> 20),
        (unsigned long long)(g_uma.import_ns / 1000000), (unsigned long long)g_uma.failed,
        (unsigned long long)g_uma.dropped, g_uma.nimp, (unsigned long long)(bytes >> 20),
        (unsigned long long)__atomic_load_n(&g_uma.hazards, __ATOMIC_RELAXED));
    g_uma.hits = g_uma.hit_bytes = g_uma.misses = g_uma.imports = g_uma.import_bytes = g_uma.import_ns = g_uma.failed =
        g_uma.dropped = 0;
    __atomic_store_n(&g_uma.hazards, 0, __ATOMIC_RELAXED);
}
/* a save state's load: every import goes */
void vk_uma_forget_all(void) {
    for (u32 i = g_uma.nimp; i-- > 0;) imp_drop(i);
}
