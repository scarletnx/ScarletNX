/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include "nx/gpu/vk/devmem.h"
#include "nx/gpu/vk/space.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/tracy.h"
#include "common/platform/thread.h" /* PMutex */
#include "common/utilities/bits.h"

#define DM_UNIT      1024u         /* a block's unit: every allocation starts at a multiple of it */
/* A block of a large heap. One allocation that stays keeps its whole block from the driver, so a block is small:
   with blocks of 256 MiB, one game's city held 1887 MB for 922 MB handed out, 448 MB of it in three blocks that
   three 16 MiB chunks of the staging ring were left in after loading */
#define DM_BLOCK_MAX (64ull << 20)
/* A request of more than this share of a block gets memory of its own. A ring's 16 MiB chunk is a quarter of a
   block and stays in one: the rings take and give back chunks all the time (cmd.c's pool), and with memory of its
   own each chunk was a call to the driver, 13 to 15 allocations and as many frees in 600 presents of one game, 7 ms
   and 5 ms each */
#define DM_OWN_SHARE 4u
#define DM_SMALL_HEAP (1ull << 30)  /* a heap of this size or less has blocks of an eighth of it at most */
#define DM_REFETCH   30u           /* requests between two questions to the driver about its budgets */
/* What stays unused of the driver's budget for the GPU's own heap, on a GPU with memory of its own. The budget is
   what the driver thinks this process can take, with nothing left: with the heap filled to it, the kernel logged
   the desktop's compositor failing to get video memory while one game ran, and what the game then asked for went to
   system memory, which the GPU reads across the bus */
#define DM_MARGIN    (256ull << 20)
/* why an allocation has memory of its own (alloc_in's alone, but for the first) */
enum {
    DM_OWN_SIZE,   /* more than a block's share, and no block had a run for it */
    DM_OWN_MUST,   /* the caller asked, or the driver requires it */
    DM_OWN_RATHER, /* the driver would rather */
    DM_OWN_LAST    /* no memory type had a block with room */
};

/* device memory from the driver: a block that is handed out in ranges (space.h), or the memory of one allocation
   (own) */
typedef struct {
    VkDeviceMemory mem; /* VK_NULL_HANDLE: the entry is free */
    void* ptr;          /* the whole of it mapped, once an allocation wanted that */
    u64 size;
    Space free;         /* its free units, not for own memory */
    u32 type;
    u32 allocs;
    u8 image, own; /* image: for images only, where they cannot share a block with buffers (g_devmem.apart) */
} Block;
/* an allocation: DevMem is its index plus one */
typedef struct {
    u32 block;     /* its block's index plus one, 0 for a free entry */
    u32 at, units; /* its place in the block, in units (a free entry's at: the next free entry plus one) */
    u32 tag;
    u32 mapped;    /* it asked for the CPU's address */
    u64 size;      /* the bytes asked for */
} Rec;

static struct {
    PMutex lk;
    Block* block;
    u32 nblock, capblock;
    Rec* rec;
    u32 nrec, caprec, free_rec; /* free_rec: a free entry's index plus one */
    VkPhysicalDeviceMemoryProperties mp;
    int bda;       /* buffer memory is made for device addresses */
    int apart;     /* buffers and images have blocks apart: the device's granularity between them is over a unit */
    u64 frame;     /* presents so far */
    /* a memory type whose heap had no room for a block of the least size: the present after it, and the count of
       device memory given back in that heap then. No block is asked of it again until that count changes or 120
       presents pass: each request without room made four calls to the driver that failed, 35,000 in 600 presents of
       one game */
    u64 full_frame[VK_MAX_MEMORY_TYPES], full_gave[VK_MAX_MEMORY_TYPES];
    u64 gave[VK_MAX_MEMORY_HEAPS];
    int main_heap; /* the largest heap of the GPU's own memory: the one DM_MARGIN is kept free of */
    /* The thread that makes the next block ahead of its first request, and gives memory back to the driver. A call
       to the driver for a block took 1 to 48 ms in one game's race and up to 600 ms while it loaded, on the thread
       that asked, which was the renderer's. It makes one block at a time: want is the next, busy the one it is at */
    struct {
        PCond cv;
        int on, want, busy;
        u32 type, image;
        u64 size;
        VkDeviceMemory gone[64]; /* memory to give back */
        u32 ngone;
        /* the driver had no room for a block of quiet_type: the present after it and the heap's count of memory
           given back then. None is asked for ahead until that count changes or 120 presents pass. A request still
           tries, with smaller blocks too */
        u64 quiet_frame, quiet_gave;
        u32 quiet_type;
        u64 made, asked; /* blocks it made, and blocks that a request made on its own thread since */
    } ahead;
    /* allocations given memory of their own since the last report, by DM_OWN_*: each is a call to the driver on the
       thread that asked */
    u64 own_why[4];
    /* requests that found no memory since the last report: all of them, those no larger than the free space inside
       the capped heap's blocks then (no one run held them), and the largest */
    u64 failed, failed_room, failed_max;
    int coherent;  /* every memory type the CPU maps is coherent: a flush has nothing to do */
    u32 since;     /* requests since the driver's budgets were asked */
    u64 block_bytes[VK_MAX_MEMORY_HEAPS], allocated[VK_MAX_MEMORY_HEAPS];
    u32 blocks[VK_MAX_MEMORY_HEAPS], allocs[VK_MAX_MEMORY_HEAPS];
    /* the driver's last answer, and this process's blocks then */
    u64 drv_usage[VK_MAX_MEMORY_HEAPS], drv_budget[VK_MAX_MEMORY_HEAPS], drv_blocks[VK_MAX_MEMORY_HEAPS];
} g_devmem = {.lk = PMUTEX_INIT};

const VkPhysicalDeviceMemoryProperties* vk_devmem_props(void) { return &g_devmem.mp; }

/* whether heap h is the GPU's own on a GPU that does not share the system's memory: the heap that is capped */
static int heap_capped(u32 h) { return (int)h == g_devmem.main_heap && g_vk.mem_model != VK_MEM_UMA; }
/* a heap's budget: the driver's, or 4/5 of the heap without one, less DM_MARGIN in the capped heap (lock held) */
static u64 heap_budget(u32 h) {
    u64 budget = g_vk.has_budget ? g_devmem.drv_budget[h] : g_devmem.mp.memoryHeaps[h].size / 5 * 4;
    if (heap_capped(h)) budget = budget > 2 * DM_MARGIN ? budget - DM_MARGIN : budget / 2;
    return budget;
}
/* a heap's use by every process: the driver's count then, and what this process took and gave back since (lock
   held) */
static u64 heap_usage(u32 h) {
    /* the block the thread ahead is making counts already */
    u64 ahead = g_devmem.ahead.busy && g_devmem.mp.memoryTypes[g_devmem.ahead.type].heapIndex == h
                    ? g_devmem.ahead.size
                    : 0;
    if (!g_vk.has_budget) return g_devmem.block_bytes[h] + ahead;
    u64 use = g_devmem.drv_usage[h] + g_devmem.block_bytes[h] + ahead;
    return use > g_devmem.drv_blocks[h] ? use - g_devmem.drv_blocks[h] : 0;
}
/* the driver's budgets (lock held) */
static void budgets_fetch(void) {
    g_devmem.since = 0;
    if (!g_vk.has_budget) return;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bp = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2,
                                             .pNext = &bp};
    vkGetPhysicalDeviceMemoryProperties2(g_vk.phys, &mp2);
    for (u32 h = 0; h < g_devmem.mp.memoryHeapCount; h++) {
        g_devmem.drv_usage[h] = bp.heapUsage[h];
        g_devmem.drv_budget[h] = bp.heapBudget[h];
        g_devmem.drv_blocks[h] = g_devmem.block_bytes[h];
        /* a driver that reports no budget, or more than the heap */
        if (!g_devmem.drv_budget[h] || g_devmem.drv_budget[h] > g_devmem.mp.memoryHeaps[h].size)
            g_devmem.drv_budget[h] = g_devmem.mp.memoryHeaps[h].size / 5 * 4;
    }
}
static void ahead_run(void* arg);
int vk_devmem_init(int bda) {
    pm_lock(&g_devmem.lk);
    vkGetPhysicalDeviceMemoryProperties(g_vk.phys, &g_devmem.mp);
    g_devmem.bda = bda;
    g_devmem.apart = g_vk.props.limits.bufferImageGranularity > DM_UNIT;
    g_devmem.main_heap = -1;
    for (u32 h = 0; h < g_devmem.mp.memoryHeapCount; h++)
        if ((g_devmem.mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            (g_devmem.main_heap < 0 ||
             g_devmem.mp.memoryHeaps[h].size > g_devmem.mp.memoryHeaps[g_devmem.main_heap].size))
            g_devmem.main_heap = (int)h;
    g_devmem.coherent = 1;
    for (u32 t = 0; t < g_devmem.mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags f = g_devmem.mp.memoryTypes[t].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            g_devmem.coherent = 0;
    }
    budgets_fetch();
    if (!g_devmem.ahead.on) {
        pc_init(&g_devmem.ahead.cv);
        g_devmem.ahead.on = plat_thread(ahead_run, NULL, 0) == 0;
    }
    pm_unlock(&g_devmem.lk);
    return 1;
}
void vk_devmem_frame(void) {
    pm_lock(&g_devmem.lk);
    g_devmem.frame++;
    budgets_fetch();
    pm_unlock(&g_devmem.lk);
}
void vk_devmem_budgets(DevMemBudget* b) {
    pm_lock(&g_devmem.lk);
    if (g_devmem.since >= DM_REFETCH) budgets_fetch();
    for (u32 h = 0; h < VK_MAX_MEMORY_HEAPS; h++) {
        b[h] = (DevMemBudget){.block_bytes = g_devmem.block_bytes[h],
                              .allocated_bytes = g_devmem.allocated[h],
                              .blocks = g_devmem.blocks[h],
                              .allocations = g_devmem.allocs[h]};
        if (h >= g_devmem.mp.memoryHeapCount) continue;
        b[h].usage = heap_usage(h);
        b[h].budget = heap_budget(h);
    }
    pm_unlock(&g_devmem.lk);
}

/* The memory type for a request, of the types in bits, or -1. One rule for every caller: the type must have what the
   place needs, and of those that do, the one that lacks the fewest properties the place prefers and has the fewest
   it avoids wins, the first of equals. The GPU's own memory avoids a type the CPU can map: on a discrete GPU with a
   small window that type is the window, which the whole desktop shares */
static int type_pick(u32 bits, const DevMemWant* w) {
    VkMemoryPropertyFlags need = w->need, like = 0,
                          avoid = VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT |
                                  VK_MEMORY_PROPERTY_DEVICE_UNCACHED_BIT_AMD;
    switch (w->at) {
    case DM_DEVICE:
        like = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        avoid |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        /* On a GPU with memory of its own, what the GPU reads stays in that memory. A request that does not fit
           fails, and its owner makes room or does without: the caches trim and ask again (vk_mem_trim), a buffer
           takes the ring. A library's way is to give it system memory with no word, where every read of it
           crosses the bus: 612 MB of one game's textures were there */
        if (g_vk.mem_model != VK_MEM_UMA) need |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case DM_HOST:
        need |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        avoid |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case DM_WINDOW:
        need |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        like = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        avoid |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    default: /* DM_CACHED */
        need |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        like = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    }
    avoid &= ~need;
    int best = -1;
    u32 best_cost = ~0u;
    for (u32 t = 0; t < g_devmem.mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags f = g_devmem.mp.memoryTypes[t].propertyFlags;
        if (!(bits >> t & 1) || (f & need) != need) continue;
        u32 cost = snx_popcount64(~f & like) + snx_popcount64(f & avoid);
        if (cost < best_cost) {
            best = (int)t;
            best_cost = cost;
        }
    }
    return best;
}

/* the largest block of a memory type: a power of two */
static u64 block_cap(u32 type) {
    u64 heap = g_devmem.mp.memoryHeaps[g_devmem.mp.memoryTypes[type].heapIndex].size;
    u64 cap = heap <= DM_SMALL_HEAP ? heap / 8 : DM_BLOCK_MAX;
    if (cap > g_vk.alloc_cap) cap = g_vk.alloc_cap;
    if (cap < DM_UNIT) return 0;
    return 1ull << (63 - snx_clz64(cap));
}

static u32 block_slot(void) {
    for (u32 i = 0; i < g_devmem.nblock; i++)
        if (!g_devmem.block[i].mem) return i + 1;
    if (g_devmem.nblock == g_devmem.capblock) {
        u32 cap = g_devmem.capblock ? g_devmem.capblock * 2 : 64;
        Block* grown = realloc(g_devmem.block, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        g_devmem.block = grown;
        g_devmem.capblock = cap;
    }
    g_devmem.block[g_devmem.nblock] = (Block){0}; /* free until its caller fills it */
    return ++g_devmem.nblock;
}
/* the driver's memory for a block or for one allocation: no state of this file, so any thread calls it with no
   lock */
static VkResult block_memory(u32 type, u64 size, int image, VkBuffer buf, VkImage img, VkDeviceMemory* mem) {
    VkMemoryDedicatedAllocateInfo alone = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                           .image = img,
                                           .buffer = buf};
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = size,
                               .memoryTypeIndex = type};
    if (buf || img) ai.pNext = &alone;
    if (g_devmem.bda && !image) {
        flags.pNext = ai.pNext;
        ai.pNext = &flags;
    }
    return vk_ledger_allocate(g_vk.dev, &ai, NULL, mem);
}
/* mem, of size bytes, enters the table as a block of units, or as the memory of one allocation (units 0), mapped at
   ptr when it is: its index plus one, or 0 without memory for the table (the caller gives mem back) */
static u32 block_add(u32 type, u64 size, int image, u32 units, VkDeviceMemory mem, void* ptr) {
    u32 slot = block_slot();
    if (!slot) return 0;
    Block* k = &g_devmem.block[slot - 1];
    *k = (Block){.size = size, .type = type, .image = (u8)image, .own = !units, .ptr = ptr};
    if (units && !space_init(&k->free, units)) return 0;
    k->mem = mem;
    u32 heap = g_devmem.mp.memoryTypes[type].heapIndex;
    g_devmem.block_bytes[heap] += size;
    g_devmem.blocks[heap]++;
    return slot;
}
/* size bytes of the driver's memory of a type, as a block (of that many units) or as the memory of one allocation
   (units 0), for a buffer or an image when the driver wants it alone: its index plus one, 0 with *res */
static u32 block_new(u32 type, u64 size, int image, u32 units, VkBuffer buf, VkImage img, VkResult* res) {
    u32 heap_of = g_devmem.mp.memoryTypes[type].heapIndex;
    /* the capped heap takes no more past its budget: full for this request, as if the driver had said so */
    if (heap_capped(heap_of) && heap_usage(heap_of) + size > heap_budget(heap_of)) {
        *res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return 0;
    }
    VkDeviceMemory mem;
    *res = block_memory(type, size, image, buf, img, &mem);
    if (*res != VK_SUCCESS) return 0;
    u32 slot = block_add(type, size, image, units, mem, NULL);
    if (!slot) {
        vk_ledger_free(g_vk.dev, mem, NULL);
        *res = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    return slot;
}
static void block_gone(Block* k) {
    u32 heap = g_devmem.mp.memoryTypes[k->type].heapIndex;
    /* a mapping goes with it. The thread ahead gives it back when it has room in its list: the driver's free took
       5 to 21 ms on the renderer's thread */
    if (g_devmem.ahead.on && g_devmem.ahead.ngone < sizeof g_devmem.ahead.gone / sizeof *g_devmem.ahead.gone) {
        g_devmem.ahead.gone[g_devmem.ahead.ngone++] = k->mem;
        pc_signal(&g_devmem.ahead.cv);
    } else
        vk_ledger_free(g_vk.dev, k->mem, NULL);
    g_devmem.gave[heap]++;
    space_destroy(&k->free);
    g_devmem.block_bytes[heap] -= k->size;
    g_devmem.blocks[heap]--;
    *k = (Block){0};
}

static DevMem rec_new(void) {
    if (g_devmem.free_rec) {
        DevMem m = g_devmem.free_rec;
        g_devmem.free_rec = g_devmem.rec[m - 1].at;
        return m;
    }
    if (g_devmem.nrec == g_devmem.caprec) {
        u32 cap = g_devmem.caprec ? g_devmem.caprec * 2 : 1024;
        Rec* grown = realloc(g_devmem.rec, (size_t)cap * sizeof *grown);
        if (!grown) return 0;
        g_devmem.rec = grown;
        g_devmem.caprec = cap;
    }
    return ++g_devmem.nrec;
}
static void rec_gone(DevMem m) {
    g_devmem.rec[m - 1] = (Rec){.at = g_devmem.free_rec};
    g_devmem.free_rec = m;
}

/* The size of a new block for a request of size bytes: the cap, or a half, a quarter or an eighth of it while that
   is more than the kind's largest block so far and holds the request twice. The first blocks of a kind are small, so
   that a kind with little in it holds little */
static u64 block_size_for(u32 type, int image, u64 size, u64 cap) {
    u64 largest = 0, s = cap;
    for (u32 i = 0; i < g_devmem.nblock; i++) {
        const Block* k = &g_devmem.block[i];
        if (k->mem && !k->own && k->type == type && k->image == image && k->size > largest) largest = k->size;
    }
    for (u32 i = 0; i < 3 && s / 2 > largest && s / 2 >= size * 2 && s / 2 >= DM_UNIT; i++) s /= 2;
    return s;
}

/* The thread ahead is asked for one more block of the kind, when the kind has no empty one left and the heap has
   room for two (lock held) */
static void ahead_ask(u32 type, int image) {
    if (!g_devmem.ahead.on || g_devmem.ahead.want || g_devmem.ahead.busy) return;
    u32 heap = g_devmem.mp.memoryTypes[type].heapIndex;
    u64 cap = block_cap(type);
    if (cap < (8u << 20)) return; /* a small heap (the mapped window): its few blocks are made as they are asked for */
    if (g_devmem.full_frame[type] && g_devmem.frame + 1 - g_devmem.full_frame[type] < 120 &&
        g_devmem.full_gave[type] == g_devmem.gave[heap])
        return;
    if (g_devmem.ahead.quiet_frame && g_devmem.frame + 1 - g_devmem.ahead.quiet_frame < 120 &&
        g_devmem.ahead.quiet_type == type && g_devmem.ahead.quiet_gave == g_devmem.gave[heap])
        return;
    for (u32 i = 0; i < g_devmem.nblock; i++) {
        const Block* k = &g_devmem.block[i];
        if (k->mem && !k->own && !k->allocs && k->type == type && k->image == image) return;
    }
    u64 size = block_size_for(type, image, 1, cap);
    if (heap_capped(heap) && heap_usage(heap) + 2 * size > heap_budget(heap)) return;
    g_devmem.ahead.type = type;
    g_devmem.ahead.image = (u32)image;
    g_devmem.ahead.size = size;
    g_devmem.ahead.want = 1;
    pc_signal(&g_devmem.ahead.cv);
}
static void ahead_run(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("Vulkan memory");
    pm_lock(&g_devmem.lk);
    for (;;) {
        while (!g_devmem.ahead.want && !g_devmem.ahead.ngone) pc_wait(&g_devmem.ahead.cv, &g_devmem.lk, -1);
        if (g_devmem.ahead.ngone) {
            VkDeviceMemory gone = g_devmem.ahead.gone[--g_devmem.ahead.ngone];
            pm_unlock(&g_devmem.lk);
            vk_ledger_free(g_vk.dev, gone, NULL);
            pm_lock(&g_devmem.lk);
            continue;
        }
        u32 type = g_devmem.ahead.type, image = g_devmem.ahead.image;
        u64 size = g_devmem.ahead.size;
        g_devmem.ahead.want = 0;
        g_devmem.ahead.busy = 1;
        pm_unlock(&g_devmem.lk);
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void* ptr = NULL;
        VkResult res = block_memory(type, size, (int)image, VK_NULL_HANDLE, VK_NULL_HANDLE, &mem);
        /* mapped here too, where the CPU can map it: the first allocation that wants its address finds it there */
        if (res == VK_SUCCESS &&
            (g_devmem.mp.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            vkMapMemory(g_vk.dev, mem, 0, VK_WHOLE_SIZE, 0, &ptr) != VK_SUCCESS)
            ptr = NULL;
        pm_lock(&g_devmem.lk);
        g_devmem.ahead.busy = 0;
        if (res != VK_SUCCESS) { /* no room for it: none is asked for ahead until the heap gives memory back */
            g_devmem.ahead.quiet_frame = g_devmem.frame + 1;
            g_devmem.ahead.quiet_type = type;
            g_devmem.ahead.quiet_gave = g_devmem.gave[g_devmem.mp.memoryTypes[type].heapIndex];
        } else if (block_add(type, size, (int)image, (u32)(size / DM_UNIT), mem, ptr)) {
            g_devmem.ahead.made++;
        } else {
            pm_unlock(&g_devmem.lk);
            vk_ledger_free(g_vk.dev, mem, NULL);
            pm_lock(&g_devmem.lk);
        }
    }
}

/* memory of one type (lock held): the allocation, or 0 with *res */
static DevMem alloc_in(u32 type, const VkMemoryRequirements* req, const DevMemWant* want, int alone, VkBuffer buf,
                       VkImage img, VkResult* res) {
    VkMemoryPropertyFlags f = g_devmem.mp.memoryTypes[type].propertyFlags;
    u64 align = req->alignment ? req->alignment : 1, cap = block_cap(type);
    /* what the CPU flushes is whole atoms of the device: an allocation it maps starts and ends on one */
    if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
        align < g_vk.props.limits.nonCoherentAtomSize)
        align = g_vk.props.limits.nonCoherentAtomSize;
    u64 units64 = (req->size + DM_UNIT - 1) / DM_UNIT, align_units = align > DM_UNIT ? align / DM_UNIT : 1;
    DevMem m = rec_new();
    *res = VK_ERROR_OUT_OF_HOST_MEMORY;
    if (!m) return 0;
    u32 slot = 0, at = 0, units = 0;
    /* Buffers and images share the blocks where the device's granularity between the two is a unit or less: what
       one kind gives back, the other can take. In one game's race with blocks apart, 612 MB of textures went to
       system memory while the buffer blocks of the GPU's heap had 1000 MB free. With shared blocks none did */
    int image = g_devmem.apart && want->image;
    /* a request that a block holds takes a free run of one, whatever its size: one of more than a block's share went
       to the driver each time, on the thread that asked, and failed at the heap's cap while the blocks had room. Only
       a request of a block's share or less opens a new block */
    int fits = !alone && cap && req->size <= cap && align <= cap;
    int shared = fits && req->size <= cap / DM_OWN_SHARE;
    if (fits) {
        units = (u32)units64;
        /* the block of the kind with the least free space that holds it: the full blocks fill up, the others empty.
           A block whose longest run does not hold it at its alignment is passed over for the next one */
        u32 from_room = 0, from_i = 0;
        for (;;) {
            u32 least = ~0u;
            slot = 0;
            for (u32 i = 0; i < g_devmem.nblock; i++) {
                const Block* k = &g_devmem.block[i];
                if (!k->mem || k->own || k->type != type || k->image != image) continue;
                u32 room = k->free.units - k->free.used, piece = k->free.largest;
                if (room < from_room || (room == from_room && i < from_i)) continue; /* tried */
                if (piece >= units && piece >= align_units && room < least) {
                    least = room;
                    slot = i + 1;
                }
            }
            if (!slot || space_take(&g_devmem.block[slot - 1].free, units, (u32)align_units, &at)) break;
            from_room = least;
            from_i = slot;
        }
        u32 heap = g_devmem.mp.memoryTypes[type].heapIndex;
        int full = g_devmem.full_frame[type] && g_devmem.frame + 1 - g_devmem.full_frame[type] < 120 &&
                   g_devmem.full_gave[type] == g_devmem.gave[heap];
        if (!slot && shared && full) *res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (!slot && shared && !full) {
            /* a new block, of a smaller size when the driver has no room for the one wanted, down to an eighth of
               the cap: a heap with no room for that is full */
            u64 size = block_size_for(type, image, req->size, cap), least = cap / 8;
            if (least < req->size) least = req->size;
            if (least < align) least = align;
            if (least < DM_UNIT) least = DM_UNIT;
            for (;;) {
                slot = block_new(type, size, image, (u32)(size / DM_UNIT), VK_NULL_HANDLE, VK_NULL_HANDLE, res);
                if (slot || size / 2 < least) break;
                size /= 2;
            }
            if (!slot) {
                g_devmem.full_frame[type] = g_devmem.frame + 1;
                g_devmem.full_gave[type] = g_devmem.gave[heap];
            } else
                g_devmem.ahead.asked++;
            if (slot && !space_take(&g_devmem.block[slot - 1].free, units, (u32)align_units, &at)) {
                block_gone(&g_devmem.block[slot - 1]); /* never: the block is empty and holds the request */
                slot = 0;
            }
        }
    }
    if (!slot && !shared) { /* memory of its own: by its size, or by the request */
        units = 0;
        slot = block_new(type, req->size, image, 0, buf, img, res);
        if (slot) g_devmem.own_why[alone]++;
    }
    if (!slot) {
        rec_gone(m);
        return 0;
    }
    Block* k = &g_devmem.block[slot - 1];
    if (want->mapped && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !k->ptr) {
        *res = vkMapMemory(g_vk.dev, k->mem, 0, VK_WHOLE_SIZE, 0, &k->ptr);
        if (*res != VK_SUCCESS) {
            k->ptr = NULL;
            if (units) space_give(&k->free, at, units);
            if (!k->allocs && k->own) block_gone(k);
            rec_gone(m);
            return 0;
        }
    }
    k->allocs++;
    if (!k->own) ahead_ask(type, image); /* the next block of the kind, before a request has to wait for it */
    u32 heap = g_devmem.mp.memoryTypes[type].heapIndex;
    g_devmem.allocated[heap] += k->own ? k->size : (u64)units * DM_UNIT;
    g_devmem.allocs[heap]++;
    g_devmem.rec[m - 1] =
        (Rec){.block = slot, .at = at, .units = units, .mapped = k->ptr && want->mapped, .size = req->size};
    *res = VK_SUCCESS;
    return m;
}

static void info_of(DevMem m, DevMemInfo* info) {
    const Rec* r = &g_devmem.rec[m - 1];
    const Block* k = &g_devmem.block[r->block - 1];
    u64 off = (u64)r->at * DM_UNIT;
    *info = (DevMemInfo){.memory = k->mem,
                         .offset = off,
                         .size = r->size,
                         .ptr = r->mapped ? (u8*)k->ptr + off : NULL,
                         .type = k->type,
                         .tag = r->tag};
}

/* alone: the driver's word on the resource, 2 when it must have memory of its own, 1 when it would rather. A request
   that no memory type has a block for gets memory of its own in the end, in the first type that has any */
static VkResult alloc(const VkMemoryRequirements* req, const DevMemWant* want, int alone, VkBuffer buf, VkImage img,
                      DevMem* mem, DevMemInfo* info) {
    VkResult res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *mem = 0;
    pm_lock(&g_devmem.lk);
    g_devmem.since++;
    u32 bits = req->memoryTypeBits;
    for (int type; !*mem && (type = type_pick(bits, want)) >= 0; bits &= ~(1u << type)) {
        int own = want->own || alone == 2 ? DM_OWN_MUST : 0;
        if (alone == 1 && !own) {
            /* the driver's preference holds while the heap has room and the device has allocations left: past
               that, each resource with memory of its own would use up both */
            u32 heap = g_devmem.mp.memoryTypes[type].heapIndex, n = 0;
            u64 budget = heap_budget(heap);
            for (u32 h = 0; h < g_devmem.mp.memoryHeapCount; h++) n += g_devmem.blocks[h];
            if (g_devmem.block_bytes[heap] + req->size <= budget / 5 * 4 &&
                n < g_vk.props.limits.maxMemoryAllocationCount / 4 * 3)
                own = DM_OWN_RATHER;
        }
        *mem = alloc_in((u32)type, req, want, own, buf, img, &res);
    }
    bits = req->memoryTypeBits;
    for (int type; !*mem && (type = type_pick(bits, want)) >= 0; bits &= ~(1u << type))
        *mem = alloc_in((u32)type, req, want, DM_OWN_LAST, buf, img, &res);
    if (!*mem) {
        u64 room = 0;
        for (u32 i = 0; i < g_devmem.nblock; i++) {
            const Block* k = &g_devmem.block[i];
            if (k->mem && !k->own && (int)g_devmem.mp.memoryTypes[k->type].heapIndex == g_devmem.main_heap)
                room += (u64)(k->free.units - k->free.used) * DM_UNIT;
        }
        g_devmem.failed++;
        g_devmem.failed_room += req->size <= room;
        if (req->size > g_devmem.failed_max) g_devmem.failed_max = req->size;
    }
    if (*mem && info) info_of(*mem, info);
    pm_unlock(&g_devmem.lk);
    return *mem ? VK_SUCCESS : res;
}
VkResult vk_devmem_alloc(const VkMemoryRequirements* req, const DevMemWant* want, DevMem* mem, DevMemInfo* info) {
    return alloc(req, want, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, mem, info);
}
void vk_devmem_free(DevMem m) {
    if (!m) return;
    pm_lock(&g_devmem.lk);
    g_devmem.since++;
    Rec* r = &g_devmem.rec[m - 1];
    Block* k = &g_devmem.block[r->block - 1];
    u32 heap = g_devmem.mp.memoryTypes[k->type].heapIndex;
    g_devmem.allocated[heap] -= k->own ? k->size : (u64)r->units * DM_UNIT;
    g_devmem.allocs[heap]--;
    if (!k->own) space_give(&k->free, r->at, r->units);
    rec_gone(m);
    if (!--k->allocs) {
        /* an empty block stays when it is the only empty one of its kind and the heap is within its budget */
        int keep = !k->own;
        if (keep) {
            u64 budget = heap_budget(heap);
            if (g_devmem.block_bytes[heap] > budget) keep = 0;
            for (u32 i = 0; keep && i < g_devmem.nblock; i++) {
                const Block* o = &g_devmem.block[i];
                if (o != k && o->mem && !o->own && !o->allocs && o->type == k->type && o->image == k->image) keep = 0;
            }
        }
        if (!keep) block_gone(k);
    }
    pm_unlock(&g_devmem.lk);
}

VkResult vk_devmem_buffer(const VkBufferCreateInfo* bci, const DevMemWant* want, VkBuffer* buf, DevMem* mem,
                          DevMemInfo* info) {
    *buf = VK_NULL_HANDLE;
    *mem = 0;
    VkResult res = vkCreateBuffer(g_vk.dev, bci, NULL, buf);
    if (res != VK_SUCCESS) return res;
    VkBufferMemoryRequirementsInfo2 of = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                                          .buffer = *buf};
    VkMemoryDedicatedRequirements alone = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &alone};
    vkGetBufferMemoryRequirements2(g_vk.dev, &of, &req);
    DevMemWant w = *want;
    DevMemInfo where;
    w.image = 0;
    res = alloc(&req.memoryRequirements, &w,
                alone.requiresDedicatedAllocation ? 2 : alone.prefersDedicatedAllocation ? 1 : 0, *buf, VK_NULL_HANDLE,
                mem, &where);
    if (res == VK_SUCCESS) res = vkBindBufferMemory(g_vk.dev, *buf, where.memory, where.offset);
    if (res != VK_SUCCESS) {
        vk_devmem_buffer_destroy(*buf, *mem);
        *buf = VK_NULL_HANDLE;
        *mem = 0;
        return res;
    }
    if (info) *info = where;
    return VK_SUCCESS;
}
void vk_devmem_buffer_destroy(VkBuffer buf, DevMem mem) {
    if (buf) vkDestroyBuffer(g_vk.dev, buf, NULL);
    vk_devmem_free(mem);
}
VkResult vk_devmem_image(const VkImageCreateInfo* ici, const DevMemWant* want, VkImage* img, DevMem* mem) {
    *img = VK_NULL_HANDLE;
    *mem = 0;
    VkResult res = vkCreateImage(g_vk.dev, ici, NULL, img);
    if (res != VK_SUCCESS) return res;
    VkImageMemoryRequirementsInfo2 of = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, .image = *img};
    VkMemoryDedicatedRequirements alone = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 req = {.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &alone};
    vkGetImageMemoryRequirements2(g_vk.dev, &of, &req);
    DevMemWant w = *want;
    DevMemInfo where;
    w.image = 1;
    res = alloc(&req.memoryRequirements, &w,
                alone.requiresDedicatedAllocation ? 2 : alone.prefersDedicatedAllocation ? 1 : 0, VK_NULL_HANDLE, *img,
                mem, &where);
    if (res == VK_SUCCESS) res = vkBindImageMemory(g_vk.dev, *img, where.memory, where.offset);
    if (res != VK_SUCCESS) {
        vk_devmem_image_destroy(*img, *mem);
        *img = VK_NULL_HANDLE;
        *mem = 0;
    }
    return res;
}
void vk_devmem_image_destroy(VkImage img, DevMem mem) {
    if (img) vkDestroyImage(g_vk.dev, img, NULL);
    vk_devmem_free(mem);
}

void vk_devmem_info(DevMem m, DevMemInfo* info) {
    *info = (DevMemInfo){0};
    if (!m) return;
    pm_lock(&g_devmem.lk);
    info_of(m, info);
    pm_unlock(&g_devmem.lk);
}
void vk_devmem_tag(DevMem m, u32 tag) {
    if (!m) return;
    pm_lock(&g_devmem.lk);
    g_devmem.rec[m - 1].tag = tag;
    pm_unlock(&g_devmem.lk);
}

/* the range of a block's memory that covers [off, off + size) of an allocation in whole atoms, 0 for memory that is
   coherent or not mapped */
static int atoms(DevMem m, u64 off, u64 size, VkMappedMemoryRange* range) {
    int need = 0;
    pm_lock(&g_devmem.lk);
    const Rec* r = &g_devmem.rec[m - 1];
    const Block* k = &g_devmem.block[r->block - 1];
    if (r->mapped && !(g_devmem.mp.memoryTypes[k->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
        off < r->size) {
        u64 atom = g_vk.props.limits.nonCoherentAtomSize, base = (u64)r->at * DM_UNIT;
        if (size > r->size - off) size = r->size - off;
        u64 from = (base + off) / atom * atom, to = (base + off + size + atom - 1) / atom * atom;
        if (to > k->size) to = k->size;
        *range = (VkMappedMemoryRange){.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                       .memory = k->mem,
                                       .offset = from,
                                       .size = to - from};
        need = to > from;
    }
    pm_unlock(&g_devmem.lk);
    return need;
}
VkResult vk_devmem_flush(DevMem m, u64 off, u64 size) {
    VkMappedMemoryRange range;
    if (!m || g_devmem.coherent || !atoms(m, off, size, &range)) return VK_SUCCESS;
    return vkFlushMappedMemoryRanges(g_vk.dev, 1, &range);
}
VkResult vk_devmem_invalidate(DevMem m, u64 off, u64 size) {
    VkMappedMemoryRange range;
    if (!m || g_devmem.coherent || !atoms(m, off, size, &range)) return VK_SUCCESS;
    return vkInvalidateMappedMemoryRanges(g_vk.dev, 1, &range);
}

void vk_devmem_log(void) {
    struct {
        u32 blocks, empty, own;
        u64 bytes, used, own_bytes, largest;
    } k[VK_MAX_MEMORY_TYPES][2] = {{{0}}};
    pm_lock(&g_devmem.lk);
    for (u32 i = 0; i < g_devmem.nblock; i++) {
        const Block* b = &g_devmem.block[i];
        if (!b->mem) continue;
        if (b->own) {
            k[b->type][b->image].own++;
            k[b->type][b->image].own_bytes += b->size;
            continue;
        }
        u64 piece = (u64)b->free.largest * DM_UNIT;
        k[b->type][b->image].blocks++;
        k[b->type][b->image].empty += !b->allocs;
        k[b->type][b->image].bytes += b->size;
        k[b->type][b->image].used += (u64)b->free.used * DM_UNIT;
        if (b->allocs && piece > k[b->type][b->image].largest) k[b->type][b->image].largest = piece;
    }
    u64 failed = g_devmem.failed, failed_room = g_devmem.failed_room, failed_max = g_devmem.failed_max;
    g_devmem.failed = g_devmem.failed_room = g_devmem.failed_max = 0;
    u64 made = g_devmem.ahead.made, asked = g_devmem.ahead.asked;
    u64 why[4] = {g_devmem.own_why[0], g_devmem.own_why[1], g_devmem.own_why[2], g_devmem.own_why[3]};
    g_devmem.ahead.made = g_devmem.ahead.asked = 0;
    for (u32 i = 0; i < 4; i++) g_devmem.own_why[i] = 0;
    pm_unlock(&g_devmem.lk);
    if (why[0] || why[1] || why[2] || why[3])
        LOG("vulkan: allocations given device memory of their own since the last report: %llu for their size, %llu "
            "asked for or required by the driver, %llu the driver would rather have so, %llu with no room in a "
            "block\n",
            (unsigned long long)why[DM_OWN_SIZE], (unsigned long long)why[DM_OWN_MUST],
            (unsigned long long)why[DM_OWN_RATHER], (unsigned long long)why[DM_OWN_LAST]);
    if (failed)
        LOG("vulkan: %llu requests for device memory found none since the last report, %llu of them no larger than "
            "the free space inside the GPU heap's blocks; the largest was %.1f MB\n",
            (unsigned long long)failed, (unsigned long long)failed_room, (double)failed_max / 1048576.0);
    if (made || asked)
        LOG("vulkan: device memory blocks since the last report: %llu made ahead of their first request, %llu made "
            "by a request on its own thread\n",
            (unsigned long long)made, (unsigned long long)asked);
    for (u32 t = 0; t < g_devmem.mp.memoryTypeCount; t++)
        for (u32 image = 0; image < 2; image++) {
            if (!k[t][image].blocks && !k[t][image].own) continue;
            VkMemoryPropertyFlags f = g_devmem.mp.memoryTypes[t].propertyFlags;
            LOG("vulkan: device memory of type %u (heap %u%s%s%s) for %s: %u blocks of %.1f MB with %.1f MB handed "
                "out, %u of them empty, the largest free piece in the others %.1f MB; %u allocations with %.1f MB "
                "of their own\n",
                t, g_devmem.mp.memoryTypes[t].heapIndex, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? ", the GPU's" : "",
                f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? ", mapped" : "",
                f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? ", cached" : "",
                !g_devmem.apart ? "buffers and images" : image ? "images" : "buffers",
                k[t][image].blocks, (double)k[t][image].bytes / 1048576.0, (double)k[t][image].used / 1048576.0,
                k[t][image].empty, (double)k[t][image].largest / 1048576.0, k[t][image].own,
                (double)k[t][image].own_bytes / 1048576.0);
        }
}
