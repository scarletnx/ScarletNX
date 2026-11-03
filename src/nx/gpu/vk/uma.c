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

   Where the driver can't import host memory but takes buffers of a kind of its own in place, guest memory moves into
   them instead (the migrate way: Android, whose drivers all take an AHardwareBuffer, and Qualcomm's has no
   VK_EXT_external_memory_host). The windows a range touches become a region: a buffer imported once, which the pages
   then move into with their bytes, mapped where they were (mem.c's mem_migrate); the regions it meets move on into it,
   so one region of their union holds them. A region is 256 MiB at most, the biggest AHardwareBuffer Android's
   allocator gave on a Snapdragon 8 Elite (512 MiB aborted its service), and its memory is taken whole when it is made
   and never swapped: one that goes for idleness or room moves its pages back into the reservation's own (mem_unmigrate)
   first. A region's buffer is memory of its own, which no change of the guest's pages touches: one whose pages are
   unmapped, decommitted or trapped needs no wait, and goes with its last reader.

   What stays a copy, and why:
   - The pages the runtime wrote (front): constant buffers the GPU front end writes between draws (CONST_BUFFER_DATA,
     r3d_const_upload), on the CPU, when the GPU thread meets the method, and the rest of gmem_write's. A draw recorded
     before such a write, but that the GPU runs after it, would read the new bytes in place. The other constant buffers
     are read in place (vk_buf_get_uniform).
   - Ranges that GPU results not landed yet cover (buffer.c's pending results, compute results) and GPU-owned ranges
     (memory/own.c): their bytes are not in guest memory.
   - Writable storage: its results land through the pending results.
   - Textures and render targets: their layout differs (block linear, compressed formats).

   The rules that keep it correct:
   - A release that the guest can see (a syncpoint, a semaphore) is performed only after the GPU ran every command that
     read guest memory in place before the stream stopped (release.c, vk_uma_bound). The copy path read those bytes when
     the commands were recorded. In place, the GPU reads them when it runs them, and after a release the guest writes
     the next frame's data.
   - Before guest pages are trapped (a GPU result published over them) or get other host pages (an unmap, a decommit, a
     move into the alias pool), the commands that read an import over them run (quiesce). mem.c then tells which
     pages changed, and the imports on them go (pages_replaced): a driver that pins pages (NVIDIA) holds the old ones,
     and amdgpu looks them up again at each submit and refuses pages in the alias pool, a file's pages.
   - Those imports leave the driver before anything else is submitted, and what was submitted before reaches it first
     (vk_uma_mapping_begin, vk_uma_mapping_end); the imports over pages to be trapped leave it before the trap
     (vk_uma_owning): a driver keeps every import in every submission, and amdgpu takes all their pages again in each
     submission's ioctl, which fails whole (EFAULT, then the device is lost) on pages gone or trapped.
   - A write of the runtime's own (gmem_write: an inline write to memory, a constant buffer update, a target's
     write-back) to a page that a submit not done yet reads in place waits for that submit (live, the submit that last
     read each page in place), as the console's GPU read it before the write. The page is copied from then on (front):
     a constant buffer updated draw after draw waits once, not at every draw. As #690 has it.
   - Any other GPU-side write (a landed result) on a page that a command not finished yet reads in place is counted
     (the hazards line of the report). That command can read the new bytes.
   - Write tracking leaves the pages an import holds alone (vk_uma_import_in): amdgpu takes them again, writable, at
     each submission, which undoes a scan's protection, and the next scan reports them written with no CPU write. What
     trusted that report kept a guest page's old bytes over the pixels a render target drew there. And a submission
     after a scan faulted every protected page in again: 16 ms for 32 MiB imported, against 0.3 ms. Write tracking
     can't tell there (vk_written_since), and its users compare the bytes. */
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/maxwell/write_pages.h"
#include "nx/gpu/vk/vk.h"
#include "common/gpu/vk/vk_common.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#if defined(__ANDROID__)
#include <vulkan/vulkan_android.h>
#endif

#define UMA_WINDOW (2ull << 20) /* imports cover whole windows of this size where guest memory is mapped */
#define UMA_MAX    512          /* imports at most: the least recently used goes past it */
#define UMA_IDLE   600          /* presents unused before an import goes */
#define HZ_BITS    16           /* the table of pages read in place, by guest page */
#define UMA_REGION_MAX (256ull << 20) /* the migrate way's biggest region */

typedef struct {
    u64 cpu, size; /* guest memory [cpu, cpu + size) */
    VkDeviceMemory mem;
    VkBuffer buf;
    u32 used; /* g_vk_present when last bound */
    u64 sub;  /* the submit its last lookup was in (vk_submit_number): last_read gives the one to wait for */
    int dead; /* its pages are other pages now (mem.c): it goes before the next submission */
    /* a region of the migrate way: the buffer its pages were moved into (plat_gpu_buffer), and what the driver imported
       of it; fd -1 for an import of guest memory where it is */
    int fd;
    void* handle;
} Imp;
/* Spans the driver refused to import (pages it can't pin, or amdgpu's refusal of the alias pool's), whose ranges are copied
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
    Imp* gone; /* dropped from the table, the driver's until the submit that last read them is done: ngone of cap; their
                  entries and count under lk too */
    u32 ngone, capgone;
    u64 seg; /* the submit being recorded when the draw or dispatch being set up began (vk_uma_settle), or once all that
                was recorded is done (read_done): a lookup since may be used in any submit up to the one being
                recorded */
    VkResult import_result; /* the last import's, for the log */
    u64 align;              /* minImportedHostPointerAlignment, and the host's page in the migrate way */
    int migrate;            /* the migrate way: guest memory moves into regions (vk_uma_choose, vk_uma_migrate) */
    char why[96];           /* why the last region couldn't be made, for the log */
    u64 bound;              /* the submit that last read guest memory in place (vk_submit_number) */
    Seen seen[256];         /* the ranges hz_mark marked, with their submit */
    WritePages live;        /* the submit that last read each page in place */
    WritePages front;       /* the pages the runtime wrote (gmem_write): never read in place again */
    u64 lag_lo, lag_hi;     /* live stamps from lag_lo on may have been read up to submit lag_hi (vk_uma_settle) */
    /* statistics */
    u64 hits, hit_bytes, misses, imports, import_bytes, import_ns, failed, dropped, quiesces, waits;
    char apart0[UMA_LINE];
    /* mem.c's hook writes these too */
    PMutex lk;         /* a leaf, under vk_lock or mem.c's lock: the dead flags, and the imports for write tracking */
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
   extensions give: guest memory imported where it is, else (Android) moved into AHardwareBuffers. The extension to
   enable, VK_UMA_WITH_* */
#if defined(__ANDROID__)
#define UMA_NO_IMPORT "neither VK_EXT_external_memory_host nor VK_ANDROID_external_memory_android_hardware_buffer"
#else
#define UMA_NO_IMPORT "no VK_EXT_external_memory_host"
#endif
int vk_uma_choose(int has_import, int has_ahb, int has_placed) {
    int want = g_settings.guest_memory, with = VK_UMA_WITH_NONE;
    g_vk.mem_model = mem_model();
    g_vk.mem_path = VK_MEM_PATH_COPY;
    if (want == 2 || (want == 0 && g_vk.mem_model == VK_MEM_UMA)) {
        with = has_import ? VK_UMA_WITH_HOST : has_ahb ? VK_UMA_WITH_AHB : VK_UMA_WITH_NONE;
        if (with == VK_UMA_WITH_NONE)
            WARN("vulkan: guest memory in place was asked for, but the device has " UMA_NO_IMPORT
                 ": guest memory is copied\n");
        else
            g_vk.mem_path = VK_MEM_PATH_UMA;
    }
    g_uma.migrate = with == VK_UMA_WITH_AHB;
    LOG("vulkan: the device %s; guest buffers %s (vk_memory=%s%s); VK_EXT_external_memory_host %s, "
        "VK_EXT_map_memory_placed %s\n",
        model_name(g_vk.mem_model),
        g_vk.mem_path != VK_MEM_PATH_UMA ? "copied"
        : g_uma.migrate                  ? "moved into AHardwareBuffers the GPU reads in place, where they can be"
                                         : "read in place where they can be",
        want == 1   ? "copy"
        : want == 2 ? "uma"
                    : "auto",
        want == 2 && g_vk.mem_model != VK_MEM_UMA ? ", forced: the GPU reads them across the bus" : "",
        has_import ? "yes" : "no", has_placed ? "yes (not used yet)" : "no");
    return with;
}

/* ------------------------------------------------------------ imports */
/* until the GPU has done submit sub, which is submitted first if it is still the one being recorded (vk_lock held) */
static void read_done(u64 sub) {
    u64 submitted, done;
    vk_submit_progress(&submitted, &done);
    if (sub <= done) return;
    if (sub > vk_last_submit()) { /* all that is recorded: every lookup so far has been read once it is done */
        vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
        g_uma.seg = vk_submit_number();
    }
    vk_wait_submit(sub);
}
/* The last submit that may read import m: the one its last lookup was in, or for a lookup since the draw or dispatch
   being set up began, the one being recorded now. A setup that waits for a landing (buffer.c's wait_finish, surface.c's
   flush_wait) submits what it recorded so far, and records the lookup's use in the next submit */
static u64 last_read(const Imp* m) { return m->sub >= g_uma.seg ? vk_submit_number() : m->sub; }
static void imp_destroy(const Imp* m) {
    vkDestroyBuffer(g_vk.dev, m->buf, NULL);
    vkFreeMemory(g_vk.dev, m->mem, NULL);
    if (m->fd >= 0) plat_gpu_buffer_free(m->fd, m->handle, m->size); /* a region's, whose pages moved off it */
    vk_mem_changed(m->cpu, m->size); /* no scan watched its pages: what the CPU wrote there meanwhile counts */
}
/* dropped import i destroyed, then out of the list, which write tracking reads (vk_lock held) */
static void gone_out(u32 i) {
    imp_destroy(&g_uma.gone[i]);
    pm_lock(&g_uma.lk);
    g_uma.gone[i] = g_uma.gone[--g_uma.ngone];
    pm_unlock(&g_uma.lk);
}
/* the dropped imports whose last reader is done are destroyed (vk_lock held) */
static void gone_reap(void) {
    u64 submitted, done;
    vk_submit_progress(&submitted, &done);
    for (u32 i = g_uma.ngone; i-- > 0;)
        if (last_read(&g_uma.gone[i]) <= done) gone_out(i);
}
/* the dropped imports over guest memory [lo, hi) are destroyed now, once their last reader is done: their pages change
   or are trapped next (vk_lock held). Not a dropped region, whose buffer holds none of the guest's pages any more */
static void gone_kill(u64 lo, u64 hi) {
    for (u32 i = g_uma.ngone; i-- > 0;)
        if (g_uma.gone[i].fd < 0 && g_uma.gone[i].cpu < hi && lo < g_uma.gone[i].cpu + g_uma.gone[i].size) {
            read_done(last_read(&g_uma.gone[i]));
            gone_out(i);
        }
}
/* the import at i goes from the table, with its pages as they are: destroyed once the submit that last read it is done
   (gone_reap), or as soon as its pages change or are trapped (gone_kill), as the driver keeps it in every submission
   until then (vk_lock held). Never a wait here: the submit may be the one being recorded, a draw half set up */
static void imp_drop(u32 i) {
    pm_lock(&g_uma.lk);
    if (g_uma.ngone == g_uma.capgone) {
        u32 cap = g_uma.capgone ? 2 * g_uma.capgone : 64;
        Imp* g = realloc(g_uma.gone, sizeof *g * cap);
        if (!g) fatal("vulkan: out of memory for %u imports of guest memory to destroy", cap);
        g_uma.gone = g;
        g_uma.capgone = cap;
    }
    g_uma.gone[g_uma.ngone++] = g_uma.imp[i];
    g_uma.dropped++;
    memmove(&g_uma.imp[i], &g_uma.imp[i + 1], sizeof *g_uma.imp * (size_t)(g_uma.nimp - i - 1));
    g_uma.nimp--;
    pm_unlock(&g_uma.lk);
}
/* the import at i goes from the table (imp_drop), and a region's pages still in its buffer first move back into the
   reservation's own with their bytes, so that its memory, which the system never swaps, goes with the buffer. Pending
   commands read the buffer as it was (vk_lock held) */
static void imp_let_go(u32 i) {
    const Imp* m = &g_uma.imp[i];
    int r;
    if (m->fd >= 0 && (r = mem_unmigrate(m->cpu, m->size)) != 0)
        WARN("vulkan: guest memory %llx+%llx stays in the buffer it moved into (error %d) until it is unmapped\n",
             (unsigned long long)m->cpu, (unsigned long long)m->size, r);
    imp_drop(i);
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
/* the import at i goes from the table and is destroyed now, once the submits that read it are done (vk_lock held),
   not when the frame's work is: the driver keeps it in every submission until then. A dead one's were waited for before
   its pages changed (vk_uma_mapping_begin), and a submit now would take it to the driver with the new ones */
static void imp_kill(u32 i, int dead) {
    if (!dead) read_done(last_read(&g_uma.imp[i]));
    imp_destroy(&g_uma.imp[i]); /* before it leaves the table: write tracking leaves its pages alone until then */
    pm_lock(&g_uma.lk);
    memmove(&g_uma.imp[i], &g_uma.imp[i + 1], sizeof *g_uma.imp * (size_t)(g_uma.nimp - i - 1));
    g_uma.nimp--;
    pm_unlock(&g_uma.lk);
    g_uma.dropped++;
}
/* the dead ones go from the driver (vk_lock held) */
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
        if (dead && g_uma.imp[i].fd >= 0)
            imp_let_go(i); /* its buffer keeps the bytes recorded commands read: no hurry */
        else if (dead)
            imp_kill(i, 1);
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
/* m's buffer over guest memory [cpu, cpu + size), in memory the driver imports: import, an import info of handle type
   type, in a memory type that bits allows, alloc bytes of it. 0 when the driver refused it */
static int imp_bind(Imp* m, u64 cpu, u64 size, VkExternalMemoryHandleTypeFlagBits type, const void* import, u32 bits,
                    u64 alloc) {
    VkExternalMemoryBufferCreateInfo ext = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .handleTypes = (VkExternalMemoryHandleTypeFlags)type};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .pNext = &ext,
                              .size = size,
                              /* what the staging ring's buffers are bound as (cmd.c), as callers bind either */
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (g_vk.has_descriptor_buffers) bci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    VkMemoryRequirements req;
    const VkPhysicalDeviceMemoryProperties* mp;
    *m = (Imp){.cpu = cpu, .size = size, .used = g_vk_present, .fd = -1};
    if (!VK_OK(vkCreateBuffer(g_vk.dev, &bci, NULL, &m->buf))) {
        m->buf = VK_NULL_HANDLE;
        return 0;
    }
    vkGetBufferMemoryRequirements(g_vk.dev, m->buf, &req);
    vmaGetMemoryProperties(g_vk.vma, &mp);
    bits &= req.memoryTypeBits;
    u32 t = ~0u;
    /* the GPU reads it: device-local where the import can be (a GPU that shares memory), else any it takes */
    for (u32 k = 0; k < mp->memoryTypeCount; k++)
        if ((bits >> k) & 1) {
            if (t == ~0u || (mp->memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) t = k;
        }
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .pNext = import,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .pNext = g_vk.has_descriptor_buffers ? (const void*)&flags : import,
                               .allocationSize = alloc,
                               .memoryTypeIndex = t};
    if (t == ~0u || req.size > alloc ||
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
    return 1;
}
/* guest memory [cpu, cpu + size), at host, imported as a buffer: 0 when the driver refused it. host is the guest's own
   memory (GP(cpu)), or in the migrate way off Android the buffer it moves into (the tests' memfd) */
static int imp_make(Imp* m, u64 cpu, u64 size, void* host) {
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                            .pHostPointer = host};
    /* a refusal is expected (pages the driver can't pin) and said once for the span (fail), not as a failed call */
    g_uma.import_result = vkGetMemoryHostPointerPropertiesEXT(
        g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host, &hp);
    if (g_uma.import_result != VK_SUCCESS) return 0;
    return imp_bind(m, cpu, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, &imp, hp.memoryTypeBits,
                    size);
}
#if defined(__ANDROID__)
/* a region's AHardwareBuffer imported as its buffer: 0 when the driver refused it. Through the device's function, as
   volk loads only the extensions of the platforms it was built for */
static int imp_make_ahb(Imp* m, u64 cpu, u64 size, void* ahb) {
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID props =
        (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(
            g_vk.dev, "vkGetAndroidHardwareBufferPropertiesANDROID");
    VkAndroidHardwareBufferPropertiesANDROID ap = {.sType =
                                                       VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
    VkImportAndroidHardwareBufferInfoANDROID imp = {
        .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .buffer = (struct AHardwareBuffer*)ahb};
    g_uma.import_result =
        props ? props(g_vk.dev, (const struct AHardwareBuffer*)ahb, &ap) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (g_uma.import_result != VK_SUCCESS) return 0;
    return imp_bind(m, cpu, size, VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID, &imp,
                    ap.memoryTypeBits, ap.allocationSize);
}
#endif
/* guest memory [cpu, cpu + size) as a region (the migrate way): a buffer of its own, imported, that its pages then move
   into with their bytes, those of the regions it meets included. NULL, or why it can't be */
static const char* region_make(Imp* m, u64 cpu, u64 size) {
    int fd, r;
    void* handle;
    if (plat_gpu_buffer(size, &fd, &handle)) {
        snprintf(g_uma.why, sizeof g_uma.why, "no buffer of that size: %s", strerror(errno));
        return g_uma.why;
    }
#if defined(__ANDROID__)
    r = imp_make_ahb(m, cpu, size, handle);
#else
    r = imp_make(m, cpu, size, handle);
#endif
    if (!r) {
        plat_gpu_buffer_free(fd, handle, size);
        return snx_vk_result_str(g_uma.import_result);
    }
    if ((r = mem_migrate(cpu, size, fd)) != 0) {
        vkDestroyBuffer(g_vk.dev, m->buf, NULL);
        vkFreeMemory(g_vk.dev, m->mem, NULL);
        plat_gpu_buffer_free(fd, handle, size);
        snprintf(g_uma.why, sizeof g_uma.why, "its pages could not move into the buffer (error %d)", r);
        return g_uma.why;
    }
    m->fd = fd;
    m->handle = handle;
    return NULL;
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
/* [lo, hi) refused, for why: in place of the oldest span kept */
static void fail(u64 lo, u64 hi, const char* why) {
    u32 old = 0;
    g_uma.failed++;
    LOG_LIMIT(8, "vulkan: guest memory %llx+%llx could not be imported (%s): its ranges are copied for a while\n",
              (unsigned long long)lo, (unsigned long long)(hi - lo), why);
    pm_lock(&g_uma.lk);
    for (u32 i = 1; i < FAIL_N && g_uma.fail[old].hi; i++)
        if (!g_uma.fail[i].hi || (i32)(g_uma.fail[i].at - g_uma.fail[old].at) < 0) old = i;
    g_uma.fail[old] = (Fail){lo, hi, g_vk_present};
    pm_unlock(&g_uma.lk);
}
/* whether guest memory [lo, hi) can be imported: mapped, and in the migrate way the guest's data on the reservation's
   own pages, which can move */
static int imp_can(u64 lo, u64 hi) {
    return g_uma.migrate ? mem_movable(lo, hi - lo) : mem_mapped_run(lo, hi - lo) == hi - lo;
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
    /* only mapped guest memory (the migrate way: the guest's data on the reservation's own pages, which can move): the
       range's own pages where the windows are not all of it */
    if (!imp_can(lo, hi)) {
        if (last > first) return ~0u;
        lo = cpu & ~(g_uma.align - 1);
        hi = (cpu + len + g_uma.align - 1) & ~(g_uma.align - 1);
        if (!imp_can(lo, hi)) return ~0u;
    }
    u64 cap = g_uma.migrate && g_vk.alloc_cap > UMA_REGION_MAX ? UMA_REGION_MAX : g_vk.alloc_cap;
    if (hi - lo > cap || failed(lo, hi)) return ~0u;
    if (g_uma.nimp - (last - first) >= UMA_MAX) { /* full: the least recently used goes */
        u32 old = 0;
        for (u32 k = 1; k < g_uma.nimp; k++)
            if ((i32)(g_uma.imp[k].used - g_uma.imp[old].used) < 0) old = k;
        imp_let_go(old);
        return ~0u; /* the indexes moved: the next lookup makes it */
    }
    Imp m;
    u64 t0 = plat_ns();
    const char* why = g_uma.migrate                       ? region_make(&m, lo, hi - lo)
                      : imp_make(&m, lo, hi - lo, GP(lo)) ? NULL
                                                          : snx_vk_result_str(g_uma.import_result);
    if (why) {
        fail(lo, hi, why);
        return ~0u; /* the imports there stay */
    }
    g_uma.imports++;
    g_uma.import_bytes += hi - lo;
    g_uma.import_ns += plat_ns() - t0;
    for (u32 k = last; k-- > first;) imp_drop(k); /* a region's pages are the new one's now */
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
    if (write_pages_since(&g_uma.front, 0, cpu, len)) { /* the runtime wrote it: copied, as when it was bound */
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
    g_uma.imp[i].sub = vk_submit_number();
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
    write_pages_stamp(&g_uma.live, cpu, len, g_uma.bound);
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
    if (g_uma.migrate) { /* regions move whole host pages (mem_migrate) */
        if (plat_vm_page() > g_uma.align) g_uma.align = plat_vm_page();
        LOG("vulkan: guest memory moves into AHardwareBuffers as the GPU reads it, in regions of whole %llu MiB "
            "windows, %llu MiB at most\n",
            (unsigned long long)(UMA_WINDOW >> 20), (unsigned long long)(UMA_REGION_MAX >> 20));
    } else {
        vkGetPhysicalDeviceProperties2(g_vk.phys, &p2);
        if (hp.minImportedHostPointerAlignment > g_uma.align) g_uma.align = hp.minImportedHostPointerAlignment;
    }
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
    gone_reap();
    if (g_vk_present % 60 == 0)
        for (u32 i = g_uma.nimp; i-- > 0;)
            if ((u32)(g_vk_present - g_uma.imp[i].used) > UMA_IDLE) imp_let_go(i);
    if (g_vk_present % 600) return;
    u64 bytes = 0;
    for (u32 i = 0; i < g_uma.nimp; i++) bytes += g_uma.imp[i].size;
    LOG("vulkan: in place over 600 presents: %llu ranges (%llu MB bound) read in guest memory, %llu copied; %llu "
        "imports made (%llu MB, %llu ms), %llu spans refused, %llu dropped; %u imports hold %llu MB; %llu hazards (a "
        "GPU-side write on a page that unfinished work reads in place); %llu waits for that work before a trap or a "
        "remap, %llu before a write of the runtime's\n",
        (unsigned long long)g_uma.hits, (unsigned long long)(g_uma.hit_bytes >> 20), (unsigned long long)g_uma.misses,
        (unsigned long long)g_uma.imports, (unsigned long long)(g_uma.import_bytes >> 20),
        (unsigned long long)(g_uma.import_ns / 1000000), (unsigned long long)g_uma.failed,
        (unsigned long long)g_uma.dropped, g_uma.nimp, (unsigned long long)(bytes >> 20),
        (unsigned long long)__atomic_load_n(&g_uma.hazards, __ATOMIC_RELAXED), (unsigned long long)g_uma.quiesces,
        (unsigned long long)g_uma.waits);
    g_uma.hits = g_uma.hit_bytes = g_uma.misses = g_uma.imports = g_uma.import_bytes = g_uma.import_ns = g_uma.failed =
        g_uma.dropped = g_uma.quiesces = g_uma.waits = 0;
    __atomic_store_n(&g_uma.hazards, 0, __ATOMIC_RELAXED);
}
/* Guest memory [cpu, cpu + size) (size ~0: all of it) is about to be trapped (a GPU result published over it,
   memory/own.c) or to get other pages (mem.c): the commands that read an import over it in place run first, submitted
   if they are still recorded (vk_lock held, mem.c's lock not). A device whose GPU is the CPU (lavapipe) reads an import
   through the CPU's own mapping, and faulted on the trapped page in a command that ran later: landing the page waited
   for a readback queued behind that command, and edenprobe's dispatch and draw pairs over one buffer hung there.
   amdgpu looks up an import's pages again at every submit and refuses them once they are in the alias pool, whose
   pages are a file's (the trap moves them there): a command recorded before the publish and submitted after it would
   be refused. The pages the command reads are the ones they were when it was recorded, as the console's GPU reads them.
   1 when an import is over it. Not for a region, whose buffer the change doesn't touch */
static int quiesce(u64 cpu, u64 size) {
    u64 end = size == ~0ull || cpu + size < cpu ? ~0ull : cpu + size, sub = 0, submitted, done;
    int any = 0;
    for (u32 i = 0; i < g_uma.nimp; i++)
        if (g_uma.imp[i].fd < 0 && g_uma.imp[i].cpu < end && cpu < g_uma.imp[i].cpu + g_uma.imp[i].size) {
            any = 1;
            if (last_read(&g_uma.imp[i]) > sub) sub = last_read(&g_uma.imp[i]);
        }
    if (!sub) return any;
    vk_submit_progress(&submitted, &done);
    if (done >= sub) return any;
    read_done(sub);
    g_uma.quiesces++;
    return any;
}
/* Guest memory [cpu, cpu + size) becomes GPU-owned next (vk_own_publish), its pages trapped: the imports over them
   go from the driver first, after their readers ran, as amdgpu fails a later submission (EFAULT) on trapped pages.
   Whole host pages, as own_begin traps them. A region stays: the trap moves its pages into the pool, and it goes
   then (pages_replaced) */
void vk_uma_owning(u64 cpu, u64 size) {
    u64 page = plat_vm_page(), lo = cpu & ~(page - 1), hi = (cpu + size + page - 1) & ~(page - 1);
    if (!vk_uma()) return;
    gone_kill(lo, hi);
    if (!quiesce(lo, hi - lo)) return;
    for (u32 i = g_uma.nimp; i-- > 0;)
        if (g_uma.imp[i].fd < 0 && g_uma.imp[i].cpu < hi && lo < g_uma.imp[i].cpu + g_uma.imp[i].size) imp_kill(i, 0);
}
/* mem.c changes guest memory [cpu, cpu + size) (size ~0: all of it) next, maybe its host pages (vk_lock held until
   vk_uma_mapping_end): with an import over it, its readers run, and every submission made so far is in the driver,
   not only in the recording thread's queue (vk_rec_sync), as a submission's ioctl takes the import's pages */
void vk_uma_mapping_begin(u64 cpu, u64 size) {
    if (!vk_uma()) return;
    gone_kill(cpu, size == ~0ull || cpu + size < cpu ? ~0ull : cpu + size);
    if (quiesce(cpu, size)) vk_rec_sync();
}
/* they changed: the imports over pages that are other pages now go before anything else is submitted */
void vk_uma_mapping_end(void) {
    if (vk_uma()) drop_dead();
}
u64 vk_uma_import_in(u64 cpu, u64 end, u64* hi) {
    u64 lo = end;
    *hi = end;
    if (!vk_uma() || cpu >= end) return end;
    pm_lock(&g_uma.lk);
    u32 i = imp_find(cpu);
    if (i < g_uma.nimp && g_uma.imp[i].cpu < end) {
        lo = g_uma.imp[i].cpu > cpu ? g_uma.imp[i].cpu : cpu;
        *hi = g_uma.imp[i].cpu + g_uma.imp[i].size;
    }
    for (u32 k = 0; k < g_uma.ngone; k++) {
        const Imp* g = &g_uma.gone[k];
        u64 at = g->cpu > cpu ? g->cpu : cpu;
        if (g->cpu < end && cpu < g->cpu + g->size && at < lo) {
            lo = at;
            *hi = g->cpu + g->size;
        }
    }
    pm_unlock(&g_uma.lk);
    if (*hi > end) *hi = end;
    return lo;
}
u32 vk_uma_imports(u64 cpu, u64 size) {
    u32 n = 0;
    for (u32 i = 0; i < g_uma.nimp; i++)
        n += g_uma.imp[i].cpu < cpu + size && cpu < g_uma.imp[i].cpu + g_uma.imp[i].size;
    for (u32 i = 0; i < g_uma.ngone; i++)
        n += g_uma.gone[i].cpu < cpu + size && cpu < g_uma.gone[i].cpu + g_uma.gone[i].size;
    return n;
}
/* A draw or dispatch begins, before its setup (vk_frame_check): the uses of what the ones before it looked up are all
   recorded, in submits up to ended, the one being recorded when they were done (vk_lock held). Only when they
   submitted is a lookup's own submit not its last reader */
void vk_uma_settle(u64 ended) {
    if (!vk_uma()) return;
    if (ended > g_uma.seg) {
        u64 submitted, done;
        vk_submit_progress(&submitted, &done);
        if (g_uma.lag_hi <= done) g_uma.lag_lo = g_uma.seg; /* the pages stamped since were read by ended at last */
        g_uma.lag_hi = ended;
        for (u32 i = 0; i < g_uma.nimp; i++)
            if (g_uma.imp[i].sub >= g_uma.seg && g_uma.imp[i].sub < ended) g_uma.imp[i].sub = ended;
        pm_lock(&g_uma.lk);
        for (u32 i = 0; i < g_uma.ngone; i++)
            if (g_uma.gone[i].sub >= g_uma.seg && g_uma.gone[i].sub < ended) g_uma.gone[i].sub = ended;
        pm_unlock(&g_uma.lk);
    }
    g_uma.seg = vk_submit_number();
}
/* Guest memory [cpu, cpu + n) is written next by the runtime: the submits that read it in place are done first, a
   lookup's use recorded in a later submit than its own included (last_read), and it is copied from then on */
static void front_write(u64 cpu, u64 n) {
    u64 submitted, done, wait = 0;
    write_pages_stamp(&g_uma.front, cpu, n, 1);
    vk_submit_progress(&submitted, &done);
    if (write_pages_since(&g_uma.live, g_uma.seg ? g_uma.seg - 1 : 0, cpu, n))
        wait = vk_submit_number(); /* looked up since the draw being set up began */
    else if (g_uma.lag_hi > done && write_pages_since(&g_uma.live, g_uma.lag_lo ? g_uma.lag_lo - 1 : 0, cpu, n))
        wait = g_uma.lag_hi;
    else if (write_pages_since(&g_uma.live, done, cpu, n))
        wait = vk_last_submit();
    if (!wait) return;
    g_uma.waits++;
    read_done(wait);
}
void vk_uma_before_write(u64 gva, u64 len) {
    if (!vk_uma()) return;
    while (len) {
        u64 cpu = 0, n = gmmu_run(gva, len, &cpu);
        if (!n) { /* an unmapped page: the write doesn't reach memory there */
            n = 0x1000 - (gva & 0xFFF);
            if (n > len) n = len;
        } else
            front_write(cpu, n);
        gva += n;
        len -= n;
    }
}
/* a save state's load: every import goes */
void vk_uma_forget_all(void) {
    for (u32 i = g_uma.nimp; i-- > 0;) imp_let_go(i);
}
int vk_uma_migrate(int on) {
    if (!vk_uma()) return 0;
    for (u32 i = g_uma.nimp; i-- > 0;) imp_let_go(i); /* the other way's go first */
    g_uma.migrate = on;
    if (on && plat_vm_page() > g_uma.align) g_uma.align = plat_vm_page();
    return 1;
}
