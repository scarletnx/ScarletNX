/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include <stdlib.h>
#include "nx/gpu/vk/devmem.h"
#include "nx/gpu/vk/space.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptor_heaps.h"
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
/* A block's kind besides its memory type, which a request takes only from blocks of its own: bits of these. IMAGE:
   for images only, where they cannot share a block with buffers (g_devmem.apart). FIXED: allocations that no owner
   moves, in the capped heap where blocks are emptied (DevMemWant.moves). BRIEF, with FIXED only: those of them that
   come and go by the hundred MB (DevMemWant.brief). In one game's race under a budget of 2 GB, the mirror's granules
   went from 196 to 38 MB and the kept results from 182 to 18 MB, and the blocks that do not move held 680 MB with
   155 MB handed out: rings, encoders, write logs and written textures, which stay, were in each of the 12 blocks, so
   none was emptied and given back */
/* Memory of its own that the driver only preferred (DM_OWN_RATHER) is kept when let go, for a later request it fits:
   DM_KEEP_BYTES at most in the other heaps together, and in the GPU's own heap a DM_KEEP_SHARE of its budget where
   that is more (keep_cap), each DM_KEEP_PRESENTS presents at most (own_keep, own_take) */
#define DM_KEEP_BYTES    (256ull << 20)
#define DM_KEEP_SHARE    16u
#define DM_KEEP_PRESENTS 120u
#define DM_KIND_IMAGE 1
#define DM_KIND_FIXED 2
#define DM_KIND_BRIEF 4
#define DM_KINDS      8
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
    u32 holds[DM_PRIO_N]; /* the units its allocations of each class hold (1 for memory of its own): rec_weight */
    u8 image, own; /* image: its kind (DM_KIND_*) besides its memory type */
    u8 reserve;    /* the reserve (vk_devmem_reserve): taken only while vk_devmem_reserve_use says, never let go */
    u8 low;        /* for requests of the low class first (DevMemWant.low): with image and type, its kind */
    u8 prio;       /* the class whose priority it should have (block_prio) */
    u8 prio_drv;   /* the class whose priority the driver has for it: the thread ahead tells the driver (prio_set) */
    u8 prio_queued; /* in the thread ahead's queue of priorities */
    u8 drain;      /* being emptied (vk_devmem_moving): no request takes a run of it */
    u32 movable;   /* its allocations whose owner can move them (vk_devmem_owner) */
    u64 drain_skip; /* not emptied again before this present: a move out of it failed */
    /* memory of its own with no resource named, kept when let go for a request of its size (own_keep) */
    u8 reuse;
    u32 idle; /* kept, unused since this present plus one: 0 in use */
} Block;
/* an allocation: DevMem is its index plus one */
typedef struct {
    u32 block;     /* its block's index plus one, 0 for a free entry */
    u32 at, units; /* its place in the block, in units (a free entry's at: the next free entry plus one) */
    u32 tag;
    u32 mapped;    /* it asked for the CPU's address */
    u8 prio;       /* its class, DM_PRIO_*: in the padding before size, so a record stays 32 bytes */
    u64 size;      /* the bytes asked for */
    void* owner;   /* the owner that can move it to other memory (vk_devmem_owner), NULL when none can */
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
    u64 cache_cap; /* the caches' budget in the main heap where it is less than the driver's (vk_budget_mb), 0 none */
    int use_reserve; /* requests may take the reserve's memory now (vk_devmem_reserve_use) */
    /* The thread that makes the next block ahead of its first request, and gives memory back to the driver. A call
       to the driver for a block took 1 to 48 ms in one game's race and up to 600 ms while it loaded, on the thread
       that asked, which was the renderer's. It makes one block at a time: want is the next, busy the one it is at */
    struct {
        PCond cv;
        int on, want, busy;
        u32 type, image, low;
        u64 size;
        VkDeviceMemory gone[64]; /* memory to give back */
        u32 ngone;
        /* blocks whose priority the driver is to be told (prio_set), and the one it is told now: its index plus one
           and its memory. done: signaled when that call returns */
        u32 prio_q[256];
        u32 nprio, prio_slot;
        VkDeviceMemory prio_mem;
        PCond done;
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
    /* the allocations the driver would rather have alone that went into blocks for their size, and their bytes, since
       the last report */
    u64 rather_in_block, rather_in_block_bytes;
    /* memory of its own kept for reuse: the bytes kept now, and since the last report the requests that took kept
       memory and the kept memory given back */
    u64 kept_bytes, kept_taken, kept_gone;
    u64 kept_heap[VK_MAX_MEMORY_HEAPS]; /* kept_bytes by heap: counted as handed out (vk_devmem_budgets) */
    /* requests that found no memory since the last report: all of them, those no larger than the free space inside
       the capped heap's blocks then (no one run held them), and the largest */
    u64 failed, failed_room, failed_max;
    u64 priorities; /* calls that gave a block a new priority since the last report (block_prio) */
    /* The block being emptied (vk_devmem_moving), as DXVK defragments its chunks: no request takes its runs, and the
       owners of its allocations move them to the runs of other blocks on the GPU. Once it has none it is an empty
       block (vk_devmem_free). slot: its index plus one, 0 for none. since: the present it started. picked: the present
       of the last search for one. The counts are since the last report */
    struct {
        int on; /* a GPU with memory of its own: integrated and mobile GPUs (UMA) keep their blocks as they are */
        u32 slot;
        int moving; /* a move asks for memory (DevMemWant.existing): only blocks that hold something take it */
        int repair; /* the block is repaired, not emptied (repair_pick): only its allocations of the wrong class move */
        u64 since, picked;
        u64 emptied, kept, gave_up, moved, moved_bytes, repaired;
    } drain;
    int coherent;  /* every memory type the CPU maps is coherent: a flush has nothing to do */
    u32 since;     /* requests since the driver's budgets were asked */
    u32 fetches;   /* the times they were asked: written under lk, read without it (vk_devmem_fetches) */
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
/* the budget that kept memory keeps within: the heap's, or in the main heap the caches' where that is less. Kept memory
   counts as handed out for the caches (vk_devmem_budgets), so a cap from the driver's budget alone let it reach 1.3 GB
   under a caches' budget of 2,000 MB (lock held) */
static u64 keep_budget(u32 h) {
    const u64 b = heap_budget(h);
    return heap_capped(h) && g_devmem.cache_cap && g_devmem.cache_cap < b ? g_devmem.cache_cap : b;
}
void vk_devmem_cache_budget(u64 bytes) {
    pm_lock(&g_devmem.lk);
    g_devmem.cache_cap = bytes;
    pm_unlock(&g_devmem.lk);
}
/* a heap's use by every process: the driver's count then, and what this process took and gave back since (lock
   held) */
static u64 heap_usage(u32 h) {
    /* the block the thread ahead is making counts already, and so does the one it is asked for: the requests that
       come before the thread begins leave it its room under the cap */
    u64 ahead =
        (g_devmem.ahead.busy || g_devmem.ahead.want) && g_devmem.mp.memoryTypes[g_devmem.ahead.type].heapIndex == h
            ? g_devmem.ahead.size
            : 0;
    if (!g_vk.has_budget) return g_devmem.block_bytes[h] + ahead;
    u64 use = g_devmem.drv_usage[h] + g_devmem.block_bytes[h] + ahead;
    return use > g_devmem.drv_blocks[h] ? use - g_devmem.drv_blocks[h] : 0;
}
/* the driver's budgets (lock held) */
static void budgets_fetch(void) {
    g_devmem.since = 0;
    __atomic_add_fetch(&g_devmem.fetches, 1, __ATOMIC_RELEASE);
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
    /* only the capped heap of a discrete GPU is emptied: on a GPU that shares the system's memory (integrated, or a
       phone's or a Mac's, which snx_vk_memory_model counts as UMA) a block held is system memory, and a move would
       cost the bandwidth that GPU shares with the CPU for nothing it lacks. DXVK likewise keeps no budget there
       (DxvkMemoryAllocator: enforceBudget only on a discrete GPU that is not UMA), and the old behaviour stays */
    g_devmem.drain.on = g_devmem.main_heap >= 0 && heap_capped((u32)g_devmem.main_heap);
    g_devmem.coherent = 1;
    for (u32 t = 0; t < g_devmem.mp.memoryTypeCount; t++) {
        VkMemoryPropertyFlags f = g_devmem.mp.memoryTypes[t].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && !(f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            g_devmem.coherent = 0;
    }
    budgets_fetch();
    if (!g_devmem.ahead.on) {
        pc_init(&g_devmem.ahead.cv);
        pc_init(&g_devmem.ahead.done);
        g_devmem.ahead.on = plat_thread(ahead_run, NULL, 0) == 0;
    }
    pm_unlock(&g_devmem.lk);
    return 1;
}
static void own_expire(void);
void vk_devmem_frame(void) {
    pm_lock(&g_devmem.lk);
    g_devmem.frame++;
    budgets_fetch();
    own_expire();
    pm_unlock(&g_devmem.lk);
}
u32 vk_devmem_fetches(void) { return __atomic_load_n(&g_devmem.fetches, __ATOMIC_ACQUIRE); }
void vk_devmem_budgets(DevMemBudget* b) {
    pm_lock(&g_devmem.lk);
    if (g_devmem.since >= DM_REFETCH) budgets_fetch();
    for (u32 h = 0; h < VK_MAX_MEMORY_HEAPS; h++) {
        b[h] = (DevMemBudget){.block_bytes = g_devmem.block_bytes[h],
                              /* kept memory is not free room for the caches: it is held for a request */
                              .allocated_bytes = g_devmem.allocated[h] + g_devmem.kept_heap[h],
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
    case DM_SYSTEM:
        avoid |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
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
        if (w->at == DM_SYSTEM && (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) continue;
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
/* The priority of each class, which the driver uses to choose what it moves to system memory when the GPU's own memory
   is short (VK_EXT_pageable_device_local_memory): the lowest first. The ends of the range, with Vulkan's default in
   the middle, keep the three classes apart where a driver puts priorities in a few groups. Other memory of this
   process and of other programs has the default, between the low and the high class */
static const float k_prio[DM_PRIO_N] = {0.0f, 0.5f, 1.0f};

/* the driver's memory for a block or for one allocation, of the low class's priority for low (where the driver moves
   memory), else of Vulkan's default: no state of this file, so any thread calls it with no lock */
static VkResult block_memory(u32 type, u64 size, int image, int low, VkBuffer buf, VkImage img, VkDeviceMemory* mem) {
    VkMemoryDedicatedAllocateInfo alone = {.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                           .image = img,
                                           .buffer = buf};
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = size,
                               .memoryTypeIndex = type};
    VkMemoryPriorityAllocateInfoEXT prio = {.sType = VK_STRUCTURE_TYPE_MEMORY_PRIORITY_ALLOCATE_INFO_EXT,
                                            .priority = k_prio[DM_PRIO_LOW]};
    if (buf || img) ai.pNext = &alone;
    if (g_devmem.bda && !(image & DM_KIND_IMAGE)) {
        flags.pNext = ai.pNext;
        ai.pNext = &flags;
    }
    if (low && g_vk.has_pageable) {
        prio.pNext = ai.pNext;
        ai.pNext = &prio;
    }
    return vk_ledger_allocate(g_vk.dev, &ai, NULL, mem);
}
/* mem, of size bytes, enters the table as a block of units, or as the memory of one allocation (units 0), mapped at
   ptr when it is: its index plus one, or 0 without memory for the table (the caller gives mem back) */
static u32 block_add(u32 type, u64 size, int image, int low, u32 units, VkDeviceMemory mem, void* ptr) {
    u32 slot = block_slot();
    if (!slot) return 0;
    Block* k = &g_devmem.block[slot - 1];
    /* the priority block_memory gave it: the low class's, or Vulkan's default, which is DM_PRIO_MID's */
    *k = (Block){.size = size,
                 .type = type,
                 .image = (u8)image,
                 .own = !units,
                 .low = (u8)low,
                 .ptr = ptr,
                 .prio = low && g_vk.has_pageable ? DM_PRIO_LOW : DM_PRIO_MID,
                 .prio_drv = low && g_vk.has_pageable ? DM_PRIO_LOW : DM_PRIO_MID};
    if (units && !space_init(&k->free, units)) return 0;
    k->mem = mem;
    u32 heap = g_devmem.mp.memoryTypes[type].heapIndex;
    g_devmem.block_bytes[heap] += size;
    g_devmem.blocks[heap]++;
    return slot;
}
/* size bytes of the driver's memory of a type, as a block (of that many units) or as the memory of one allocation
   (units 0), for a buffer or an image when the driver wants it alone: its index plus one, 0 with *res */
static u32 block_new(u32 type, u64 size, int image, int low, u32 units, VkBuffer buf, VkImage img, VkResult* res) {
    u32 heap_of = g_devmem.mp.memoryTypes[type].heapIndex;
    /* the capped heap takes no more past its budget: full for this request, as if the driver had said so */
    if (heap_capped(heap_of) && heap_usage(heap_of) + size > heap_budget(heap_of)) {
        *res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        return 0;
    }
    VkDeviceMemory mem;
    *res = block_memory(type, size, image, low, buf, img, &mem);
    if (*res != VK_SUCCESS) return 0;
    u32 slot = block_add(type, size, image, low, units, mem, NULL);
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
    } else {
        /* not while the thread ahead tells the driver its priority (prio_set): the lock is let go meanwhile */
        while (g_devmem.ahead.prio_slot && g_devmem.ahead.prio_mem == k->mem)
            pc_wait(&g_devmem.ahead.done, &g_devmem.lk, -1);
        vk_ledger_free(g_vk.dev, k->mem, NULL);
    }
    g_devmem.gave[heap]++;
    space_destroy(&k->free);
    g_devmem.block_bytes[heap] -= k->size;
    g_devmem.blocks[heap]--;
    *k = (Block){0};
}

/* what an allocation counts for in its block's holds: its units, or 1 for memory of its own */
static u32 rec_weight(const Rec* r) { return r->units ? r->units : 1; }
/* The priority k takes from what it holds (lock held). A block of the low class takes the class of most of its bytes:
   a texture with drawn pixels among textures loaded from memory moves with them, as the driver keeps the pixels of
   what it moves and the GPU samples the one as it samples the others. With the highest class instead, 5% of textures
   with drawn pixels kept 766 of 1286 MiB of the low class's bytes out of the low priority, in a test of the
   allocator. Another block takes the highest class it holds: a render target or a ring is not moved for the buffers
   around it. An empty block keeps the priority it has, as the next request of its kind is most likely of the same
   class. The driver is told when that changes: one call at most, and none where it does not move memory */
/* k's priority as the class prio (lock held). The driver's call (vkSetDeviceMemoryPriorityEXT) is made on the thread
   ahead: on the thread that asked it took 2 to 4 us as a rule, but one took 1,323 ms inside the renderer's work in
   one game's race at 2 GB, and 1.9 s of 4,414 calls in all. The thread tells the driver the class a block has when it
   comes to it, so a change and its undoing (memory kept for reuse and taken again) make no call. With no thread or no
   room in its queue, the call is made here */
static void prio_set(Block* k, u32 prio) {
    k->prio = (u8)prio;
    if (!g_vk.has_pageable || k->prio_queued) return;
    if (g_devmem.ahead.on && g_devmem.ahead.nprio < sizeof g_devmem.ahead.prio_q / sizeof *g_devmem.ahead.prio_q) {
        k->prio_queued = 1;
        g_devmem.ahead.prio_q[g_devmem.ahead.nprio++] = (u32)(k - g_devmem.block);
        pc_signal(&g_devmem.ahead.cv);
        return;
    }
    if (k->prio == k->prio_drv) return;
    vk_ledger_priority(g_vk.dev, k->mem, k_prio[k->prio]);
    k->prio_drv = k->prio;
    g_devmem.priorities++;
}
static void block_prio(Block* k) {
    u32 top = DM_PRIO_N;
    for (u32 c = 0; c < DM_PRIO_N; c++)
        if (k->holds[c] && (top == DM_PRIO_N || !k->low || k->holds[c] >= k->holds[top])) top = c;
    if (top == DM_PRIO_N || top == k->prio || !g_vk.has_pageable) return;
    prio_set(k, top);
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
static u64 block_size_for(u32 type, int image, int low, u64 size, u64 cap) {
    u64 largest = 0, s = cap;
    for (u32 i = 0; i < g_devmem.nblock; i++) {
        const Block* k = &g_devmem.block[i];
        if (k->mem && !k->own && k->type == type && k->image == image && k->low == low && k->size > largest)
            largest = k->size;
    }
    for (u32 i = 0; i < 3 && s / 2 > largest && s / 2 >= size * 2 && s / 2 >= DM_UNIT; i++) s /= 2;
    return s;
}

/* The thread ahead is asked for one more block of the kind, when the kind has no empty one left and the heap has
   room for two (lock held) */
static void ahead_ask(u32 type, int image, int low) {
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
        /* an empty block of either class or either side of DM_KIND_FIXED: block_adopt changes them */
        if (k->mem && !k->own && !k->allocs && k->type == type && (k->image & DM_KIND_IMAGE) == (image & DM_KIND_IMAGE))
            return;
    }
    u64 size = block_size_for(type, image, low, 1, cap);
    if (heap_capped(heap) && heap_usage(heap) + 2 * size > heap_budget(heap)) return;
    g_devmem.ahead.type = type;
    g_devmem.ahead.image = (u32)image;
    g_devmem.ahead.low = (u32)low;
    g_devmem.ahead.size = size;
    g_devmem.ahead.want = 1;
    pc_signal(&g_devmem.ahead.cv);
}
static void ahead_run(void* arg) {
    (void)arg;
    SNX_THREAD_NAME("Vulkan memory");
    pm_lock(&g_devmem.lk);
    for (;;) {
        while (!g_devmem.ahead.want && !g_devmem.ahead.ngone && !g_devmem.ahead.nprio)
            pc_wait(&g_devmem.ahead.cv, &g_devmem.lk, -1);
        if (g_devmem.ahead.ngone) { /* memory given back first: a priority can wait, and a request may want the room */
            VkDeviceMemory gone = g_devmem.ahead.gone[--g_devmem.ahead.ngone];
            pm_unlock(&g_devmem.lk);
            vk_ledger_free(g_vk.dev, gone, NULL);
            pm_lock(&g_devmem.lk);
            continue;
        }
        if (g_devmem.ahead.nprio) { /* a block's priority, which the driver is told without the lock */
            const u32 i = g_devmem.ahead.prio_q[--g_devmem.ahead.nprio];
            Block* k = &g_devmem.block[i];
            k->prio_queued = 0;
            if (!k->mem || k->prio == k->prio_drv) continue;
            const VkDeviceMemory mem = k->mem;
            const u32 prio = k->prio;
            g_devmem.ahead.prio_slot = i + 1;
            g_devmem.ahead.prio_mem = mem;
            pm_unlock(&g_devmem.lk);
            vk_ledger_priority(g_vk.dev, mem, k_prio[prio]);
            pm_lock(&g_devmem.lk);
            g_devmem.ahead.prio_slot = 0;
            g_devmem.ahead.prio_mem = VK_NULL_HANDLE;
            /* the block may have gone meanwhile, and another may be in its place: the driver's class is that
               memory's */
            if (g_devmem.block[i].mem == mem) g_devmem.block[i].prio_drv = (u8)prio;
            g_devmem.priorities++;
            pc_broadcast(&g_devmem.ahead.done);
            continue;
        }
        u32 type = g_devmem.ahead.type, image = g_devmem.ahead.image, low = g_devmem.ahead.low;
        u64 size = g_devmem.ahead.size;
        g_devmem.ahead.want = 0;
        g_devmem.ahead.busy = 1;
        pm_unlock(&g_devmem.lk);
        VkDeviceMemory mem = VK_NULL_HANDLE;
        void* ptr = NULL;
        VkResult res = block_memory(type, size, (int)image, (int)low, VK_NULL_HANDLE, VK_NULL_HANDLE, &mem);
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
        } else if (block_add(type, size, (int)image, (int)low, (u32)(size / DM_UNIT), mem, ptr)) {
            g_devmem.ahead.made++;
        } else {
            pm_unlock(&g_devmem.lk);
            vk_ledger_free(g_vk.dev, mem, NULL);
            pm_lock(&g_devmem.lk);
        }
    }
}

/* The block of a kind (low: its class, -1 either) with the least free space that holds units at their alignment, its
   run taken at *at: the full blocks fill up, the others empty. A block whose longest run does not hold it at its
   alignment is passed over for the next one. Its index plus one, 0 for none (lock held) */
static u32 block_find(u32 type, int image, int low, u32 units, u32 align_units, u32* at) {
    u32 from_room = 0, from_i = 0, slot;
    for (;;) {
        u32 least = ~0u;
        slot = 0;
        for (u32 i = 0; i < g_devmem.nblock; i++) {
            const Block* k = &g_devmem.block[i];
            if (!k->mem || k->own || k->type != type || (low >= 0 && k->low != low)) continue;
            /* the reserve is never emptied: what moves and what does not both take it */
            if (k->reserve ? (k->image & DM_KIND_IMAGE) != (image & DM_KIND_IMAGE) : k->image != image) continue;
            if ((k->reserve && !g_devmem.use_reserve) || k->drain) continue;
            if (g_devmem.drain.moving && !k->allocs) continue; /* a move goes to blocks in use (vk_devmem_moving) */
            u32 room = k->free.units - k->free.used, piece = k->free.largest;
            if (room < from_room || (room == from_room && i < from_i)) continue; /* tried */
            if (piece >= units && piece >= align_units && room < least) {
                least = room;
                slot = i + 1;
            }
        }
        if (!slot || space_take(&g_devmem.block[slot - 1].free, units, align_units, at)) return slot;
        from_room = least;
        from_i = slot;
    }
}
/* An empty block of the other class, or of the other side of DM_KIND_FIXED, that holds units, which takes the class
   low and the kind image, its run taken at *at: its index plus one, 0 for none (lock held). An empty block holds
   nothing, so its class and kind cost nothing to change, and the classes and the two sides share one spare block of a
   memory type (and of images apart) as all requests did before them. With a spare and a partly filled block for each
   class, the game held 93 MB more free memory inside its blocks at the same point */
static u32 block_adopt(u32 type, int image, int low, u32 units, u32 align_units, u32* at) {
    if (g_devmem.drain.moving) return 0; /* a move goes to blocks in use of its own class (vk_devmem_moving) */
    for (u32 i = 0; i < g_devmem.nblock; i++) {
        Block* k = &g_devmem.block[i];
        if (!k->mem || k->own || k->allocs || k->reserve || k->drain || k->type != type ||
            (k->image & DM_KIND_IMAGE) != (image & DM_KIND_IMAGE) || (k->low == low && k->image == image))
            continue;
        if (k->free.largest < units || !space_take(&k->free, units, align_units, at)) continue;
        k->low = (u8)low;
        k->image = (u8)image;
        return i + 1;
    }
    return 0;
}

/* Memory of its own kept for reuse. Memory the driver only preferred alone, for a request of more than a block's share,
   was a call to the driver's allocate on the thread that asked, and its free on the thread ahead: under a budget of 2
   GB one game's race made 621 in 191 s, the most costly of the calls left. Such memory is made with no resource named,
   which the driver preferred but did not require, and when let go it is kept, unused, for a later request of its size
   (up to an eighth less), with no call. Kept memory goes after DM_KEEP_PRESENTS presents, the oldest of its heap first
   past keep_cap, and all of it past own_full's limit (lock held).

   A cap of 256 MiB for every heap was too small. In that race at 2 GB on a GPU of 24 GB, the requests that found no
   kept memory were 16 to 24 MB (65) and 32 to 48 MB (150), the cap gave kept memory back 189 times and age 15
   times, and 162 of 234 requests found kept memory of no size that fit them. In the GPU's own heap the cap now grows
   with the heap's budget. A small heap is held to own_full's limit, as before. The other heaps keep DM_KEEP_BYTES:
   with the share there too (950 MB of a host heap of 15 GB), two of five runs of that race failed, a lost GPU and
   a crash where the mirror read a page number of no page from its output in host memory, and three runs with the
   share in the GPU's heap alone had neither

   Memory made for an image alone, kept for another image (VK_NV_dedicated_allocation_image_aliasing, which allows
   that for the same create info and an extent and layers no greater), lost the GPU on an RTX 4090 (driver 621.01): a
   page fault in 2 of 3 runs of that race with it, and none in the run without it. The spec does not say whether an
   image may take that memory after the image it was made for is gone */
/* past this, a heap keeps no memory: the driver's preference for memory of its own no longer holds there (alloc, with
   the same budget), so no request would take it */
static int own_full(u32 heap) { return g_devmem.block_bytes[heap] > keep_budget(heap) / 5 * 4; }
/* the cap of the kept memory that heap's counts with: the GPU's own heap alone, or the other heaps together */
static u64 keep_cap(u32 heap) {
    if (!heap_capped(heap)) return DM_KEEP_BYTES;
    const u64 share = keep_budget(heap) / DM_KEEP_SHARE;
    return share > DM_KEEP_BYTES ? share : DM_KEEP_BYTES;
}
static u64 kept_with(u32 heap) {
    u64 n = 0;
    for (u32 h = 0; h < g_devmem.mp.memoryHeapCount; h++)
        if (heap_capped(h) == heap_capped(heap)) n += g_devmem.kept_heap[h];
    return n;
}
static void own_give(Block* k) {
    g_devmem.kept_bytes -= k->size;
    g_devmem.kept_heap[g_devmem.mp.memoryTypes[k->type].heapIndex] -= k->size;
    g_devmem.kept_gone++;
    block_gone(k);
}
/* every kept memory goes: a request found none other (alloc), and their blocks may take it */
static int own_give_all(void) {
    int gave = 0;
    for (u32 i = 0; g_devmem.kept_bytes && i < g_devmem.nblock; i++)
        if (g_devmem.block[i].mem && g_devmem.block[i].idle) {
            own_give(&g_devmem.block[i]);
            gave = 1;
        }
    return gave;
}
/* k, memory of its own let go: kept when it can be, at the low class's priority, so that the driver moves it out of
   the GPU's memory first where that is short */
static void own_keep(Block* k) {
    u32 heap = g_devmem.mp.memoryTypes[k->type].heapIndex;
    const u64 cap = keep_cap(heap);
    if (!k->reuse || k->size > cap || own_full(heap)) {
        block_gone(k);
        return;
    }
    while (kept_with(heap) + k->size > cap) { /* room for it: the oldest kept that counts with it goes */
        Block* old = NULL;
        for (u32 i = 0; i < g_devmem.nblock; i++) {
            Block* o = &g_devmem.block[i];
            if (o->mem && o->idle && heap_capped(g_devmem.mp.memoryTypes[o->type].heapIndex) == heap_capped(heap) &&
                (!old || o->idle < old->idle))
                old = o;
        }
        own_give(old);
    }
    k->idle = (u32)g_devmem.frame + 1;
    g_devmem.kept_bytes += k->size;
    g_devmem.kept_heap[heap] += k->size;
    if (g_vk.has_pageable && k->prio != DM_PRIO_LOW) prio_set(k, DM_PRIO_LOW); /* a taker gives it its class */
}
/* kept memory of a type that a request of size bytes takes: the smallest that holds it, an eighth more at most. Its
   index plus one, 0 for none */
static u32 own_take(u32 type, int image, u64 size) {
    u32 best = 0;
    for (u32 i = 0; i < g_devmem.nblock; i++) {
        const Block* k = &g_devmem.block[i];
        if (!k->mem || !k->idle || k->type != type || (k->image & DM_KIND_IMAGE) != (image & DM_KIND_IMAGE)) continue;
        if (k->size < size || (best && k->size >= g_devmem.block[best - 1].size)) continue;
        if (k->size > size + size / 8) continue;
        best = i + 1;
    }
    if (best) {
        Block* k = &g_devmem.block[best - 1];
        k->idle = 0;
        g_devmem.kept_bytes -= k->size;
        g_devmem.kept_heap[g_devmem.mp.memoryTypes[k->type].heapIndex] -= k->size;
        g_devmem.kept_taken++;
    }
    return best;
}
/* once a present: kept memory past DM_KEEP_PRESENTS goes, and all of it while its heap is past own_full's limit */
static void own_expire(void) {
    for (u32 i = 0; g_devmem.kept_bytes && i < g_devmem.nblock; i++) {
        Block* k = &g_devmem.block[i];
        if (!k->mem || !k->idle) continue;
        u32 heap = g_devmem.mp.memoryTypes[k->type].heapIndex;
        if (g_devmem.frame + 1 - k->idle > DM_KEEP_PRESENTS || own_full(heap)) own_give(k);
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
    /* the blocks of the low class only where the driver moves memory: elsewhere they would hold more for nothing (a
       block partly filled and an empty one of each kind more, 120 MiB in a test of the allocator) */
    int image = g_devmem.apart && want->image ? DM_KIND_IMAGE : 0, low = want->low && g_vk.has_pageable;
    /* where blocks are emptied, what no owner moves goes in blocks of its own: one such allocation keeps a block from
       being emptied (drain_can). A move goes to the blocks of what moves */
    if (g_devmem.drain.on && heap_capped(g_devmem.mp.memoryTypes[type].heapIndex) && !want->moves && !want->existing)
        image |= DM_KIND_FIXED | (want->brief ? DM_KIND_BRIEF : 0);
    /* a request that a block holds takes a free run of one, whatever its size: one of more than a block's share went
       to the driver each time, on the thread that asked, and failed at the heap's cap while the blocks had room. Only
       a request of a block's share or less opens a new block */
    int fits = !alone && cap && req->size <= cap && align <= cap;
    int shared = fits && req->size <= cap / DM_OWN_SHARE;
    if (fits) {
        units = (u32)units64;
        slot = block_find(type, image, low, units, (u32)align_units, &at); /* a block of its class */
        if (!slot) slot = block_adopt(type, image, low, units, (u32)align_units, &at); /* an empty one of the other */
        u32 heap = g_devmem.mp.memoryTypes[type].heapIndex;
        int full = g_devmem.full_frame[type] && g_devmem.frame + 1 - g_devmem.full_frame[type] < 120 &&
                   g_devmem.full_gave[type] == g_devmem.gave[heap];
        if (!slot && (want->existing || (shared && full))) *res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
        if (!slot && shared && !full && !want->existing) {
            /* a new block, of a smaller size when the driver has no room for the one wanted, down to an eighth of
               the cap: a heap with no room for that is full */
            u64 size = block_size_for(type, image, low, req->size, cap), least = cap / 8;
            if (least < req->size) least = req->size;
            if (least < align) least = align;
            if (least < DM_UNIT) least = DM_UNIT;
            for (;;) {
                slot = block_new(type, size, image, low, (u32)(size / DM_UNIT), VK_NULL_HANDLE, VK_NULL_HANDLE, res);
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
        /* a block of the other class, when its own have no room and no new one could be made: the classes only
           order what the driver moves, and must not make a request fail that would have found room */
        if (!slot && !(want->existing && g_vk.has_pageable))
            slot = block_find(type, image, -1, units, (u32)align_units, &at);
        /* then a block of the other kinds of the same side of DM_KIND_IMAGE, for the same reason. What no owner
           moves keeps the block it goes into from being emptied, but the request does not fail where it found room
           before the kinds. What stays goes to the blocks of what moves before the brief ones, which it would keep
           from being emptied when what comes and goes leaves them. What comes and goes takes the blocks of what stays
           first, and what moves those of what does not, the brief ones last. A move goes only to the blocks of what
           moves */
        if (!slot && !want->existing && g_devmem.drain.on && heap_capped(heap)) {
            const int side = image & DM_KIND_IMAGE, fixed = side | DM_KIND_FIXED, brief = fixed | DM_KIND_BRIEF;
            const int other[3] = {image == brief ? fixed : image == fixed ? side : fixed,
                                  image == brief ? side : brief, -1};
            for (u32 o = 0; !slot && other[o] >= 0; o++)
                slot = block_find(type, other[o], -1, units, (u32)align_units, &at);
        }
    }
    if (!slot && !shared && !want->existing) { /* memory of its own: by its size, or by the request */
        units = 0;
        /* what the driver only preferred: memory with no resource named, kept when let go (own_keep) */
        const int rather = alone == DM_OWN_RATHER;
        if (rather) slot = own_take(type, image, req->size);
        if (!slot) {
            slot = block_new(type, req->size, image, low, 0, rather ? VK_NULL_HANDLE : buf, rather ? VK_NULL_HANDLE : img,
                             res);
            if (slot) g_devmem.own_why[alone]++;
            if (slot && rather) g_devmem.block[slot - 1].reuse = 1;
        }
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
    u32 prio = want->low ? DM_PRIO_LOW : DM_PRIO_MID; /* until its owner gives its class (vk_devmem_priority) */
    /* a block that took a move is not emptied for 120 presents: what came in settles first */
    /* a block that took a move is not emptied or repaired for 120 presents: what came in settles first. After a
       repair's move, 600: a texture's class follows whether it took drawn pixels (texture.c), which can change back,
       and its moves between the classes would copy it each time */
    const u64 settle = g_devmem.frame + (g_devmem.drain.repair ? 600 : 120);
    if (want->existing && k->drain_skip < settle) k->drain_skip = settle;
    k->allocs++;
    k->holds[prio] += units ? units : 1;
    block_prio(k);
    if (!k->own) ahead_ask(type, image, k->low); /* the next block of the kind, before a request has to wait for it */
    u32 heap = g_devmem.mp.memoryTypes[type].heapIndex;
    g_devmem.allocated[heap] += k->own ? k->size : (u64)units * DM_UNIT;
    g_devmem.allocs[heap]++;
    g_devmem.rec[m - 1] = (Rec){
        .block = slot, .at = at, .units = units, .mapped = k->ptr && want->mapped, .size = req->size, .prio = (u8)prio};
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
static void drain_stop(void);
static VkResult alloc(const VkMemoryRequirements* req, const DevMemWant* want, int alone, VkBuffer buf, VkImage img,
                      DevMem* mem, DevMemInfo* info) {
    VkResult res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *mem = 0;
    pm_lock(&g_devmem.lk);
    g_devmem.since++;
    u32 bits = req->memoryTypeBits;
    if (want->existing) {
        /* a move: a free run of a block that is there, or nothing. A resource that must be alone has no such run */
        g_devmem.drain.moving = 1;
        for (int type; !*mem && !want->own && alone != 2 && (type = type_pick(bits, want)) >= 0;
             bits &= ~(1u << type))
            *mem = alloc_in((u32)type, req, want, 0, buf, img, &res);
        g_devmem.drain.moving = 0;
        if (*mem && info) info_of(*mem, info);
        pm_unlock(&g_devmem.lk);
        return *mem ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    for (int type; !*mem && (type = type_pick(bits, want)) >= 0; bits &= ~(1u << type)) {
        int own = want->own || alone == 2 ? DM_OWN_MUST : 0, small = 0;
        if (alone == 1 && !own) {
            /* the driver's preference holds for a request of more than a block's share, while the heap has room and
               the device has allocations left: past that, each resource with memory of its own would use up both. A
               smaller one goes into a block. Memory of its own is a call to the driver on the thread that asks, and
               its own again when it goes: under a budget of 2 GB, one game's race made 1,000 to 2,300 of them in 2
               minutes, for 0.85 to 1.2 s of the renderer's thread in the driver's allocate and calls of 50 to 164
               ms. In a block, the defrag can move it too */
            u32 heap = g_devmem.mp.memoryTypes[type].heapIndex, n = 0;
            /* the budget own_full keeps kept memory within: with the driver's here and the caches' there, under
               vk_budget_mb=2000 each such request got memory of its own that own_keep gave straight back, 209 of
               them in a window of one game's race */
            u64 budget = keep_budget(heap);
            for (u32 h = 0; h < g_devmem.mp.memoryHeapCount; h++) n += g_devmem.blocks[h];
            small = req->size <= block_cap((u32)type) / DM_OWN_SHARE;
            if (!small && g_devmem.block_bytes[heap] + req->size <= budget / 5 * 4 &&
                n < g_vk.props.limits.maxMemoryAllocationCount / 4 * 3)
                own = DM_OWN_RATHER;
        }
        *mem = alloc_in((u32)type, req, want, own, buf, img, &res);
        if (*mem && small) {
            g_devmem.rather_in_block++;
            g_devmem.rather_in_block_bytes += req->size;
        }
        /* a preference is not a need: when the driver has no memory for the resource alone, a block may have */
        if (!*mem && own == DM_OWN_RATHER) *mem = alloc_in((u32)type, req, want, 0, buf, img, &res);
        /* memory kept for reuse in this type's heap takes room the request has none of: it goes, and the request is
           tried again in this type before the next, which may be a worse one (the mapped window, system memory) */
        if (!*mem && g_devmem.kept_heap[g_devmem.mp.memoryTypes[type].heapIndex] && own_give_all())
            *mem = alloc_in((u32)type, req, want, own == DM_OWN_RATHER ? 0 : own, buf, img, &res);
    }
    bits = req->memoryTypeBits;
    for (int type; !*mem && (type = type_pick(bits, want)) >= 0; bits &= ~(1u << type))
        *mem = alloc_in((u32)type, req, want, DM_OWN_LAST, buf, img, &res);
    /* No request takes a run of the block being emptied. One that found room nowhere else has that block given back
       to the requests: the drain stops (drain_stop), and the request looks again. Emptying a block is never why a
       request finds no memory. One that must have memory of its own takes no run: its look again would bind it into
       a block, against the driver or the caller */
    if (!*mem && g_devmem.drain.slot && !g_devmem.drain.repair && !want->own && alone != 2) {
        const Block* d = &g_devmem.block[g_devmem.drain.slot - 1];
        LOG_LIMIT(16,
                  "vulkan: a request for %.1f MB of device memory found no room but in the block being emptied (%.1f "
                  "MB of %.1f MB free there): that block takes requests again\n",
                  (double)req->size / 1048576.0, (double)(d->free.units - d->free.used) * DM_UNIT / 1048576.0,
                  (double)d->size / 1048576.0);
        drain_stop();
        bits = req->memoryTypeBits;
        for (int type; !*mem && (type = type_pick(bits, want)) >= 0; bits &= ~(1u << type))
            *mem = alloc_in((u32)type, req, want, 0, buf, img, &res);
    }
    if (!*mem) {
        u64 room = 0, piece = 0;
        for (u32 i = 0; i < g_devmem.nblock; i++) {
            const Block* k = &g_devmem.block[i];
            if (!k->mem || k->own || (int)g_devmem.mp.memoryTypes[k->type].heapIndex != g_devmem.main_heap) continue;
            room += (u64)(k->free.units - k->free.used) * DM_UNIT;
            if ((u64)k->free.largest * DM_UNIT > piece) piece = (u64)k->free.largest * DM_UNIT;
        }
        g_devmem.failed++;
        g_devmem.failed_room += req->size <= room;
        if (req->size > g_devmem.failed_max) g_devmem.failed_max = req->size;
        /* the first few in full: which requests the free space failed, and how scattered it was */
        LOG_LIMIT(32,
                  "vulkan: a request for %.1f MB of device memory (%s) found none: the GPU heap's blocks have %.1f MB "
                  "free, the largest piece %.1f MB, and its use is %.1f of %.1f MB\n",
                  (double)req->size / 1048576.0, want->image ? "an image" : "a buffer", (double)room / 1048576.0,
                  (double)piece / 1048576.0, (double)heap_usage((u32)g_devmem.main_heap) / 1048576.0,
                  (double)heap_budget((u32)g_devmem.main_heap) / 1048576.0);
    }
    if (*mem && info) info_of(*mem, info);
    pm_unlock(&g_devmem.lk);
    return *mem ? VK_SUCCESS : res;
}
VkResult vk_devmem_alloc(const VkMemoryRequirements* req, const DevMemWant* want, DevMem* mem, DevMemInfo* info) {
    return alloc(req, want, 0, VK_NULL_HANDLE, VK_NULL_HANDLE, mem, info);
}
/* The reserve: a block of the GPU's memory for render targets alone, which the rest leave alone. On a GPU of 2 GB,
   Mario Kart World's depth targets found no memory for hundreds of presents as its race loaded: what the trim freed
   was the driver's again only once the frame was done, and other requests took it first. Its draws were skipped, and
   its auto-exposure, fed those frames, went to nothing: the scene stayed black for a minute or for the run. Made where
   the device's heap is 4 GiB or less; images only take it (g_devmem.apart), and only within vk_devmem_reserve_use */
void vk_devmem_reserve(u64 size, u32 type_bits) {
    pm_lock(&g_devmem.lk);
    const DevMemWant want = {.at = DM_DEVICE, .image = 1};
    int type = type_pick(type_bits, &want);
    VkResult res = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    u32 slot = type < 0 ? 0 : block_new((u32)type, size, g_devmem.apart, 0, (u32)(size / DM_UNIT), VK_NULL_HANDLE,
                                        VK_NULL_HANDLE, &res);
    if (slot) g_devmem.block[slot - 1].reserve = 1;
    pm_unlock(&g_devmem.lk);
    if (slot)
        LOG("vulkan: %llu MB of the GPU's memory held back for render targets that find none\n",
            (unsigned long long)(size >> 20));
}
void vk_devmem_reserve_use(int on) {
    pm_lock(&g_devmem.lk);
    g_devmem.use_reserve = on;
    pm_unlock(&g_devmem.lk);
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
    k->holds[r->prio] -= rec_weight(r);
    if (!k->own) space_give(&k->free, r->at, r->units);
    if (r->owner) k->movable--;
    const u32 slot = r->block;
    rec_gone(m);
    if (!--k->allocs) {
        /* an empty block stays when it is the only empty one of its kind and the heap is within its budget. The two
           classes count as one kind here: block_adopt gives an empty block to either. One that was emptied
           (vk_devmem_moving) follows the same rule: given back, the next request of its kind would have the thread
           ahead make a block of the same size again. Kept, it is a whole block for a request that no run of the others
           held */
        const int emptied = k->drain && g_devmem.drain.slot == slot;
        if (emptied) {
            g_devmem.drain.slot = 0;
            k->drain = 0;
        } else if (g_devmem.drain.slot == slot) { /* a block under repair that its frees emptied: nothing to repair */
            g_devmem.drain.slot = 0;
            g_devmem.drain.repair = 0;
        }
        if (k->own) { /* kept for a later request it fits, or given back */
            own_keep(k);
            if (k->allocs) block_prio(k);
            pm_unlock(&g_devmem.lk);
            return;
        }
        int keep = 1;
        if (keep && !k->reserve) {
            u64 budget = heap_budget(heap);
            if (g_devmem.block_bytes[heap] > budget) keep = 0;
            for (u32 i = 0; keep && i < g_devmem.nblock; i++) {
                const Block* o = &g_devmem.block[i];
                if (o != k && o->mem && !o->own && !o->allocs && o->type == k->type &&
                    (o->image & DM_KIND_IMAGE) == (k->image & DM_KIND_IMAGE))
                    keep = 0; /* one spare a memory type, for either class and either side of DM_KIND_FIXED */
            }
        }
        if (emptied) {
            if (keep)
                g_devmem.drain.kept++;
            else
                g_devmem.drain.emptied++;
        }
        if (!keep) block_gone(k);
    }
    if (k->allocs) block_prio(k); /* what the block holds now (an empty one keeps its priority) */
    pm_unlock(&g_devmem.lk);
}

void vk_devmem_owner(DevMem m, void* owner) {
    if (!m || !g_devmem.drain.on) return; /* set at init, before any allocation */
    pm_lock(&g_devmem.lk);
    Rec* r = &g_devmem.rec[m - 1];
    Block* k = &g_devmem.block[r->block - 1];
    if (!r->owner && owner) k->movable++;
    if (r->owner && !owner) k->movable--;
    r->owner = owner;
    pm_unlock(&g_devmem.lk);
}
/* a block that drain_pick may empty: of the capped heap, not the reserve or memory of one allocation, not empty, every
   allocation's owner able to move it, and not left as it was in the last 600 presents */
static int drain_can(const Block* k, u32 h) {
    return k->mem && !k->own && !k->reserve && k->allocs && k->movable == k->allocs &&
           g_devmem.mp.memoryTypes[k->type].heapIndex == h && g_devmem.frame >= k->drain_skip;
}
/* The block to empty next (lock held): its index plus one, 0 for none. The rule is DXVK's (pickDefragChunk in
   dxvk_memory.cpp), for each kind of block (memory type, and images apart where they are): a kind is defragmented when
   the heap's blocks are past the budget, or when the free space inside its blocks that hold something is more than an
   eighth of their use plus a block. Of its blocks that can be emptied, half full at most, the one with the least in it,
   when the free space in the kind's other blocks is one and a half times what it holds or more. budget: the caches'
   (vk_mem_use, which vk_budget_mb lowers) */
static u32 drain_pick(u64 budget) {
    if (!g_devmem.drain.on) return 0;
    const u32 h = (u32)g_devmem.main_heap;
    const int over = g_devmem.block_bytes[h] > budget;
    u32 best = 0;
    u64 best_used = ~0ull;
    for (u32 i = 0; i < g_devmem.nblock; i++) {
        const Block* k = &g_devmem.block[i];
        if (!drain_can(k, h)) continue;
        const u64 used = (u64)k->free.used * DM_UNIT;
        /* half full at most: DXVK's pick runs over every chunk, where the least used one is the one with most holes.
           Here it runs over the blocks every allocation of which moves, and in a game the least used of those was full
           (64 MB in 184 allocations) while the blocks with holes held allocations that do not move: emptying it moved
           64 MB and freed nothing */
        if (used >= best_used || used > k->size / 2) continue;
        /* the kind's blocks that hold something: their use, their size, and the free space of the others */
        u64 kind_used = 0, kind_size = 0, room = 0;
        for (u32 j = 0; j < g_devmem.nblock; j++) {
            const Block* o = &g_devmem.block[j];
            if (!o->mem || o->own || o->reserve || !o->allocs || o->type != k->type || o->image != k->image) continue;
            if (g_vk.has_pageable && o->low != k->low) continue; /* moves stay in their class's blocks */
            kind_used += (u64)o->free.used * DM_UNIT;
            kind_size += o->size;
            if (j != i) room += (u64)(o->free.units - o->free.used) * DM_UNIT;
        }
        /* within the budget, a kind whose free space is an eighth of its use and a block, or less, is left alone */
        const u64 next = block_size_for(k->type, k->image, k->low, 1, block_cap(k->type));
        if (!over && kind_used + kind_used / 8 + next >= kind_size) continue;
        /* room for what it holds and half as much again: DXVK's test on the chunk's used pages */
        if (2 * room < 3 * used) continue;
        best = i + 1;
        best_used = used;
    }
    return best;
}
/* the units of k's allocations whose class is not its own: higher classes in a block of the low class, or the low
   class in another block. The driver gives a block one priority (block_prio), so they move with the wrong one */
static u32 misplaced(const Block* k) {
    return k->low ? k->holds[DM_PRIO_MID] + k->holds[DM_PRIO_HIGH] : k->holds[DM_PRIO_LOW];
}
static int is_misplaced(const Block* k, const Rec* r) {
    return k->low ? r->prio != DM_PRIO_LOW : r->prio == DM_PRIO_LOW;
}
/* The block to repair next (lock held), where the driver moves memory by priority (g_vk.has_pageable): its
   index plus one, 0 for none. A class takes the other's blocks when its own have no room and no new one can be made
   (alloc_in), and a texture drawn into rises from the low class. So a block of the low class can hold render targets,
   which keep it high, and its textures cannot go to system memory first; or low textures sit in a high block. Of the
   blocks whose allocations all move, the one with most units of the wrong class, an eighth of what it holds at
   least. Only those allocations move, to blocks of their own class, and the block takes requests as before */
static u32 repair_pick(void) {
    if (!g_vk.has_pageable) return 0;
    const u32 h = (u32)g_devmem.main_heap;
    u32 best = 0, most = 0;
    for (u32 i = 0; i < g_devmem.nblock; i++) {
        const Block* k = &g_devmem.block[i];
        if (!drain_can(k, h)) continue;
        const u32 wrong = misplaced(k);
        if (wrong > most && wrong * 8 >= k->free.used) {
            best = i + 1;
            most = wrong;
        }
    }
    return best;
}
/* the block being emptied (or repaired) is left as it is, and not taken again for 600 presents (lock held) */
static void drain_stop(void) {
    if (!g_devmem.drain.slot) return;
    Block* k = &g_devmem.block[g_devmem.drain.slot - 1];
    k->drain = 0;
    k->drain_skip = g_devmem.frame + 600;
    g_devmem.drain.slot = 0;
    g_devmem.drain.repair = 0;
    g_devmem.drain.gave_up++;
}
u32 vk_devmem_moving(DevMem* mems, void** owners, u32 max, u64 budget) {
    u32 n = 0;
    if (!g_devmem.drain.on) return 0;
    pm_lock(&g_devmem.lk);
    /* one that takes more than 600 presents to empty has an owner that does not move its allocation */
    if (g_devmem.drain.slot && g_devmem.frame - g_devmem.drain.since > 600) drain_stop();
    if (!g_devmem.drain.slot && g_devmem.frame - g_devmem.drain.picked >= 30) {
        g_devmem.drain.picked = g_devmem.frame;
        g_devmem.drain.slot = drain_pick(budget);
        /* with no block to empty, one whose classes are mixed is repaired: it stays open to requests */
        if (!g_devmem.drain.slot && (g_devmem.drain.slot = repair_pick())) {
            const Block* k = &g_devmem.block[g_devmem.drain.slot - 1];
            g_devmem.drain.repair = 1;
            g_devmem.drain.since = g_devmem.frame;
            LOG_LIMIT(64, "vulkan: device memory block %u (%.1f MB, the %s class) has %.1f MB of the other class: "
                          "repaired at present %llu\n",
                      g_devmem.drain.slot - 1, (double)k->size / 1048576.0, k->low ? "low" : "higher",
                      (double)misplaced(k) * DM_UNIT / 1048576.0, (unsigned long long)g_devmem.frame);
        } else if (g_devmem.drain.slot) {
            const Block* k = &g_devmem.block[g_devmem.drain.slot - 1];
            g_devmem.block[g_devmem.drain.slot - 1].drain = 1;
            g_devmem.drain.since = g_devmem.frame;
            LOG_LIMIT(64, "vulkan: device memory block %u (%.1f MB, %.1f MB in %u allocations) is emptied at present "
                          "%llu\n",
                      g_devmem.drain.slot - 1, (double)k->size / 1048576.0,
                      (double)k->free.used * DM_UNIT / 1048576.0, k->allocs, (unsigned long long)g_devmem.frame);
        }
    }
    for (u32 i = 0; g_devmem.drain.slot && i < g_devmem.nrec && n < max; i++) {
        const Rec* r = &g_devmem.rec[i];
        if (r->block != g_devmem.drain.slot || !r->owner) continue;
        if (g_devmem.drain.repair && !is_misplaced(&g_devmem.block[r->block - 1], r)) continue;
        mems[n] = i + 1;
        owners[n++] = r->owner;
    }
    /* A repair ends when nothing of the wrong class that moves is left. The block takes requests while it is
       repaired, and one that found no room in its own class can bring an allocation of the wrong class with no owner
       (a ring's chunk, a sparse page): the block rests then, as one that was not repaired */
    if (g_devmem.drain.slot && g_devmem.drain.repair && !n) {
        Block* k = &g_devmem.block[g_devmem.drain.slot - 1];
        if (misplaced(k))
            k->drain_skip = g_devmem.frame + 600;
        else
            g_devmem.drain.repaired++;
        g_devmem.drain.slot = 0;
        g_devmem.drain.repair = 0;
    }
    pm_unlock(&g_devmem.lk);
    return n;
}
int vk_devmem_emptying(void) {
    pm_lock(&g_devmem.lk);
    const int on = g_devmem.drain.slot && !g_devmem.drain.repair;
    pm_unlock(&g_devmem.lk);
    return on;
}
void vk_devmem_moving_stop(void) {
    pm_lock(&g_devmem.lk);
    drain_stop();
    pm_unlock(&g_devmem.lk);
}
void vk_devmem_moved(u64 bytes) {
    pm_lock(&g_devmem.lk);
    g_devmem.drain.moved++;
    g_devmem.drain.moved_bytes += bytes;
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
    if (buf) vk_descriptor_heaps_forget_buffer(buf); /* its kept descriptors, before a new buffer can get its handle */
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

void vk_devmem_priority(DevMem m, u32 prio) {
    if (!m || prio >= DM_PRIO_N) return;
    pm_lock(&g_devmem.lk);
    Rec* r = &g_devmem.rec[m - 1];
    if (r->prio != prio) {
        Block* k = &g_devmem.block[r->block - 1];
        k->holds[r->prio] -= rec_weight(r);
        k->holds[prio] += rec_weight(r);
        r->prio = (u8)prio;
        block_prio(k);
    }
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
    } k[VK_MAX_MEMORY_TYPES][DM_KINDS] = {{{0}}};
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
    u64 emptied = g_devmem.drain.emptied, kept = g_devmem.drain.kept, gave_up = g_devmem.drain.gave_up,
        moved = g_devmem.drain.moved, moved_bytes = g_devmem.drain.moved_bytes;
    g_devmem.drain.emptied = g_devmem.drain.kept = g_devmem.drain.gave_up = 0;
    g_devmem.drain.moved = g_devmem.drain.moved_bytes = 0;
    const u64 repaired = g_devmem.drain.repaired;
    g_devmem.drain.repaired = 0;
    u64 made = g_devmem.ahead.made, asked = g_devmem.ahead.asked, priorities = g_devmem.priorities;
    g_devmem.priorities = 0;
    u64 why[4] = {g_devmem.own_why[0], g_devmem.own_why[1], g_devmem.own_why[2], g_devmem.own_why[3]};
    const u64 rather = g_devmem.rather_in_block, rather_bytes = g_devmem.rather_in_block_bytes;
    const u64 kept_now = g_devmem.kept_bytes, kept_taken = g_devmem.kept_taken, kept_gone = g_devmem.kept_gone;
    g_devmem.kept_taken = g_devmem.kept_gone = 0;
    g_devmem.rather_in_block = g_devmem.rather_in_block_bytes = 0;
    g_devmem.ahead.made = g_devmem.ahead.asked = 0;
    for (u32 i = 0; i < 4; i++) g_devmem.own_why[i] = 0;
    pm_unlock(&g_devmem.lk);
    if (why[0] || why[1] || why[2] || why[3])
        LOG("vulkan: allocations given device memory of their own since the last report: %llu for their size, %llu "
            "asked for or required by the driver, %llu the driver would rather have so, %llu with no room in a "
            "block\n",
            (unsigned long long)why[DM_OWN_SIZE], (unsigned long long)why[DM_OWN_MUST],
            (unsigned long long)why[DM_OWN_RATHER], (unsigned long long)why[DM_OWN_LAST]);
    if (kept_taken || kept_gone || kept_now)
        LOG("vulkan: memory of its own kept for reuse since the last report: %llu requests took it with no call to the "
            "driver, %llu given back; %.1f MB kept now\n",
            (unsigned long long)kept_taken, (unsigned long long)kept_gone, (double)kept_now / 1048576.0);
    if (rather)
        LOG("vulkan: allocations the driver would rather have alone, put in blocks for their size since the last "
            "report: %llu, %.1f MB\n",
            (unsigned long long)rather, (double)rather_bytes / 1048576.0);
    if (failed)
        LOG("vulkan: %llu requests for device memory found none since the last report, %llu of them no larger than "
            "the free space inside the GPU heap's blocks; the largest was %.1f MB\n",
            (unsigned long long)failed, (unsigned long long)failed_room, (double)failed_max / 1048576.0);
    if (emptied || kept || gave_up || moved || repaired)
        LOG("vulkan: device memory blocks emptied since the last report: %llu given back, %llu kept as their kind's "
            "empty block, %llu left as they were; %llu repaired to one priority class; %llu allocations moved, "
            "%.1f MB\n",
            (unsigned long long)emptied, (unsigned long long)kept, (unsigned long long)gave_up,
            (unsigned long long)repaired, (unsigned long long)moved, (double)moved_bytes / 1048576.0);
    if (made || asked)
        LOG("vulkan: device memory blocks since the last report: %llu made ahead of their first request, %llu made "
            "by a request on its own thread\n",
            (unsigned long long)made, (unsigned long long)asked);
    if (priorities)
        LOG("vulkan: device memory blocks given a new priority since the last report: %llu (a call to the driver "
            "each)\n",
            (unsigned long long)priorities);
    for (u32 t = 0; t < g_devmem.mp.memoryTypeCount; t++)
        for (u32 image = 0; image < DM_KINDS; image++) {
            if (!k[t][image].blocks && !k[t][image].own) continue;
            VkMemoryPropertyFlags f = g_devmem.mp.memoryTypes[t].propertyFlags;
            LOG("vulkan: device memory of type %u (heap %u%s%s%s) for %s%s: %u blocks of %.1f MB with %.1f MB handed "
                "out, %u of them empty, the largest free piece in the others %.1f MB; %u allocations with %.1f MB "
                "of their own\n",
                t, g_devmem.mp.memoryTypes[t].heapIndex, f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? ", the GPU's" : "",
                f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? ", mapped" : "",
                f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? ", cached" : "",
                !g_devmem.apart ? "buffers and images" : image & DM_KIND_IMAGE ? "images" : "buffers",
                image & DM_KIND_BRIEF ? " that do not move and come and go"
                : image & DM_KIND_FIXED ? " that do not move"
                                        : "",
                k[t][image].blocks, (double)k[t][image].bytes / 1048576.0, (double)k[t][image].used / 1048576.0,
                k[t][image].empty, (double)k[t][image].largest / 1048576.0, k[t][image].own,
                (double)k[t][image].own_bytes / 1048576.0);
        }
}
