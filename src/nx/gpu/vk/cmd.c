/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Frames in flight. A frame runs from one present to the next and may submit many times (a readback, results the
   guest needs): each submission gets its own command buffer and signals the next value of the graphics timeline
   semaphore. A frame has a staging ring for uploads and for data draws read directly (vertices, indices, constant
   buffers), a ring its storage results are copied back into for the CPU, descriptor pools, and the resources to
   destroy once its work is done; none of them is reused before its last submission has finished. Ending a frame at a
   submit would reset them mid-draw, and wait for the GPU at every submit. */
#include "nx/gpu/vk/descriptor_objects.h"
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/platform/log.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/own.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/texture_arrays.h"
#include "nx/gpu/vk/uma_budget.h"
#include "nx/runtime/platform/plat.h"
#include "common/utilities/common.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/tracy.h"

typedef struct {
    VkImage img;
    DevMem mem;
    VkImageView v1, v2;
    VkBuffer buf;
    VkBufferView view;
    void (*fn)(void*);
    void* arg;
} Dead;

/* What the rings handed out, by ring and by where its memory is, for vk_buffer_stats: a byte in host memory the GPU
   reads or writes crosses PCIe, once for each time it does. The first four are a frame's rings (Frame.ring) */
enum { XFER_HOST, XFER_MAPPED, XFER_DEVICE, XFER_N };
enum {
    XRING_STAGE,    /* streamed buffer snapshots, written by the CPU and read by the GPU (vk_stage_upload) */
    XRING_OUTPUT,   /* uploads, draw data and GPU-written storage, near the GPU when its budget allows (vk_stage) */
    XRING_BACK,     /* storage results the GPU copies back for the CPU to read (vk_stage_back) */
    XRING_DEVICE,   /* made and read by the GPU alone, in device memory (vk_stage_device) */
    XRING_READBACK, /* vk_readback_buffer's, no frame's */
    XRING_STREAM,   /* bytes the CPU fills for work recorded at once, no frame's (vk_stream_upload) */
    XRING_N
};
#define FRAME_RINGS XRING_READBACK

/* A frame's ring hands out the chunk it holds (g_cmd) and takes another from the pool when that one is full; all of
   them go back with the frame slot, once its work is done */
typedef struct {
    u32 cur;  /* the chunk it hands out of: its index in g_cmd.chunk, plus one; 0 for none */
    u32 n;    /* chunks it took in this use of the frame */
    u64 used; /* bytes of cur handed out */
} Ring;

#define SPARE_SETS 32
#define SPARE_LAYOUTS 16
typedef struct {
    VkCommandPool pool;
    VkCommandBuffer* cbs; /* one per submission in this use of the frame */
    u32 ncb, icb;         /* icb: the one recording, or the next to begin */
    VkCommandBuffer* ups; /* each submission's upload command buffer (vk_cmd_upload), from the same pool */
    u32 nup;
    int up; /* the recording submission's: 0 not begun, 1 begun, 2 with its wait for earlier GPU writes recorded */
    u64 last;             /* the timeline value of the frame's last submission, 0 when it has none */
    u64 hold;             /* the next frame's submission that reads buffers in this one's rings (vk_frame_hold), or 0 */
    int recording, fresh; /* fresh: just ended up current, not yet reset for its new use */
    Ring ring[FRAME_RINGS]; /* by XRING_ index */
    u64 filled;             /* bytes of the chunks its rings filled in this use, and of its big buffers */
    /* destroyed once this frame's work is done */
    Dead* dead;
    int ndead, capdead;
    u64 dead_bytes; /* device memory evicted cache entries among them release (vk_defer_bytes) */
    /* descriptor pools, reset when the frame is reused and kept */
    VkDescriptorPool pool_d[64];
    int npool, ipool; /* pool_d[ipool] is the one allocating; the ones after it are reset and waiting */
    int took, pool_peak; /* the pools this use allocated from, the most of this window's uses (vk_desc_pools_keep) */
    u32 pool_uses;
    /* sets allocated ahead, a batch per layout at a time: most draws make a new set, and the driver's allocation cost
       is mostly per call */
    struct {
        VkDescriptorSetLayout layout;
        VkDescriptorSet set[SPARE_SETS];
        u32 n;
    } spare[SPARE_LAYOUTS];
} Frame;

/* The rings' memory: chunks of one size, made as the rings need them and pooled, by ring, between the frame slots. A
   request bigger than a chunk gets a buffer of its own, of the chunk size doubled until it fits (its size class), pooled
   apart. A slot's chunks and big buffers go back to the pool once its work is done (frame_recycle), and the pool keeps
   as many of each as the most any of the last VK_POOL_FRAMES frames took (a chunk at least): a steady stream reuses
   them, a copy of the same big size every frame takes its buffer back, and a burst's go a few frames after it, where a
   ring that doubled up to the allocation limit stayed there for 300 frames */
typedef struct {
    VkBuffer buf; /* VK_NULL_HANDLE: an entry no buffer holds */
    DevMem mem;
    u8* ptr;    /* where the CPU maps it, NULL in device memory */
    u64 size;   /* the chunk size it was made at (chunk_size), or a big buffer's size class (big_size) */
    u64 freed;  /* the frame it went back to the pool at */
    u64 handed; /* the most of it handed out from its start since it was made: pages the CPU wrote (chunk_take) */
    u32 ring;   /* the ring it serves: an XRING_ index */
    u32 in;     /* where its memory is: an XFER_ index */
    u32 slot;   /* the frame slot it is handed out to, plus one; 0 while in the pool */
    u64 sub;    /* the stream's: the submission whose end frees it (stream_take) */
    u32 flush;  /* written by the CPU and not flushed since (flushed at the next submit) */
    u32 big;    /* a buffer for a request bigger than a chunk */
    u32 bar;    /* made for the output ring's share of the window (bar_room) and in device memory the CPU maps: on a
                   GPU that shares system memory every buffer is in such memory, asked for there or not */
} Chunk;
static struct {
    Chunk* chunk;
    u32 nchunk, capchunk;
    u32 unflushed;                         /* chunks with flush set */
    u32 took[VK_POOL_FRAMES][FRAME_RINGS][2]; /* chunks ([0]) and big buffers the last frames took, by ring: [itook]
                                                 the one being recorded */
    u32 itook;
    u64 made; /* chunks and big buffers made since the start (statistics, tests) */
    u32 recycles; /* frame slots recycled since the start (tests) */
    /* The stream: the chunk it hands out of (plus one) and the bytes handed out of that. Its chunks are no frame's:
       each has the number of a submission, and is handed out again once that submission is done */
    u32 stream_cur;
    u64 stream_used;
    u64 stream_since; /* the bytes it handed out since the last submit (frame_check) */
    u64 submits[VK_SUB_N]; /* submissions since the start, by why (statistics, tests) */
    u32 stage_told; /* the staging ring's memory type is in the log */
} g_cmd;

static Frame g_fr[VK_FRAMES];
static u64 g_dead_bytes; /* every frame's dead_bytes */
static u32 g_cur;
static u32 g_cb_begun = 1; /* command buffers begun, so a command buffer's state is told from an earlier one's */
static u64 g_frame_id;
static VkSemaphore g_gsem; /* graphics timeline: submission n signals n */
static u64 g_gval;         /* the last submission */
static u64 g_landed;       /* submissions whose storage results vk_buf_frame_done has landed */
static u64 g_sub_ns;       /* when the last submission was made */
static VkSemaphoreSubmitInfo g_after[2]; /* vk_submit_after's wait and signal, for the next submission (semaphore 0: none) */
u32 g_vk_present;

/* XXH3 (Yann Collet's xxHash 0.8.4, BSD-2-Clause, cmake/dependencies.cmake): the quality XXH64 gave the hash it
   replaced (a change in a word's high bits shows, and two don't cancel), at about three times its speed. On x86-64
   xxHash's dispatcher runs the AVX-512, AVX2 or SSE2 loop the CPU has (60 GB/s on a Zen 4 core, against XXH64's 19),
   from a build for any x86-64. Buffers' checks hashed hundreds of megabytes a frame in one game */
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86) /* as xxh3.c builds it */
#include "xxh_x86dispatch.h"
#define SNX_XXH3(p, n) XXH3_64bits_dispatch(p, n)
#define SNX_XXH128(p, n) XXH3_128bits_dispatch(p, n)
#else
#include "xxhash.h"
#define SNX_XXH3(p, n) XXH3_64bits(p, n)
#define SNX_XXH128(p, n) XXH3_128bits(p, n)
#endif
u64 vk_hash(const void* p, u64 size) { return (u64)SNX_XXH3(p, (size_t)size); }
void vk_hash128(const void* p, u64 size, u64 hash[2]) {
    XXH128_hash_t h = SNX_XXH128(p, (size_t)size);
    hash[0] = h.low64;
    hash[1] = h.high64;
}
/* the same hash as vk_hash, which an entry's next check may compute instead, with src copied to dst: the bytes are
   hashed where they are (dst may be write-combined staging, slow to read back) and then copied, from cache */
u64 vk_hash_copy(void* dst, const void* src, u64 size) {
    u64 h = (u64)SNX_XXH3(src, (size_t)size);
    memcpy(dst, src, (size_t)size);
    return h;
}

u64 vk_sample_hash(u64 gva, u64 len) {
    u8 tmp[2048 + 128];
    if (len <= sizeof tmp) {
        vk_guest_peek(gva, tmp, len);
        return vk_hash(tmp, len);
    }
    u32 n = 0;
    vk_guest_peek(gva, tmp, 64);
    n = 64;
    vk_guest_peek(gva + len - 64, tmp + n, 64);
    n += 64;
    u64 step = (len - 32) / 62;
    for (int i = 0; i < 62; i++) {
        vk_guest_peek(gva + (u64)i * step, tmp + n, 32);
        n += 32;
    }
    return vk_hash(tmp, n);
}

u64 vk_stage_align(void) {
    static u64 al;
    if (!al) {
        const VkPhysicalDeviceLimits* l = &g_vk.props.limits;
        al = 16;
        if (l->minUniformBufferOffsetAlignment > al) al = l->minUniformBufferOffsetAlignment;
        if (l->minStorageBufferOffsetAlignment > al) al = l->minStorageBufferOffsetAlignment;
        if (l->minTexelBufferOffsetAlignment > al) al = l->minTexelBufferOffsetAlignment; /* compute buffer images */
    }
    return al;
}

static void dead_push(Frame* f, Dead d) {
    if (f->ndead == f->capdead) {
        f->capdead = f->capdead ? f->capdead * 2 : 64;
        f->dead = realloc(f->dead, sizeof *f->dead * f->capdead);
    }
    f->dead[f->ndead++] = d;
}
static void dead_free(Frame* f) {
    for (int i = 0; i < f->ndead; i++) {
        Dead* d = &f->dead[i];
        if (d->v1) vk_image_view_destroy(g_vk.dev, d->v1, NULL);
        if (d->v2) vk_image_view_destroy(g_vk.dev, d->v2, NULL);
        if (d->view) vkDestroyBufferView(g_vk.dev, d->view, NULL);
        if (d->img) vk_image_destroy(d->img, d->mem);
        if (d->buf) {
            vk_mem_untrack(d->mem);
            vk_devmem_buffer_destroy(d->buf, d->mem);
        }
        if (d->fn) d->fn(d->arg);
    }
    f->ndead = 0;
}

void vk_cmd_init(void) {
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkSemaphoreTypeCreateInfo tci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                     .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &tci};
    VK_OK(vkCreateSemaphore(g_vk.dev, &sci, NULL, &g_gsem));
    for (int i = 0; i < VK_FRAMES; i++) {
        VK_OK(vkCreateCommandPool(g_vk.dev, &pci, NULL, &g_fr[i].pool));
        g_fr[i].fresh = 1;
    }
    vk_rec_init();
}

/* a ring's chunk, and the readback buffer's smallest size */
#define STAGE_MIN (16ull << 20)
static u64 chunk_size(void) { return g_vk.alloc_cap < STAGE_MIN ? g_vk.alloc_cap : STAGE_MIN; }
u64 vk_stage_chunk(void) { return chunk_size(); }

/* GPU-written storage goes in device-local memory the CPU maps when there is room for it, else in host memory. Without
   resizable BAR that memory is a window of its own heap, 246 MiB on an RTX 2080, which the whole desktop shares, and
   past its room the driver moves the memory: every CPU write to it then faulted in the kernel, and one game crawled at
   10 frames a second for twenty seconds, its GPU thread half in the kernel. Streamed buffer snapshots use host memory
   instead: their large sequential writes must not cross that window. Stable input buffers get device-local copies.

   The room is the driver's budget (VK_EXT_memory_budget, devmem.c) less the use it counts, except where the driver
   counts the whole system's use of the window: NVIDIA's (615.78) does, and budgets what is left of it, so that use and
   budget add up to the heap's size. A process that has allocated nothing there (vulkaninfo) is told 155 of 246 MiB
   used and 91 budgeted, as nvidia-smi's BAR1 count. Read as this process's use, that looked past the budget all the
   time, and each frame's ring was freed at its reuse and made again in the window: a 16 MiB vkAllocateMemory and
   vkFreeMemory every frame, about a millisecond (a menu at 760 frames a second ran at 3200 without it). A ring of
   more than a quarter of a block (a block is an eighth of a small heap, devmem.c) gets memory of its own, which no
   budget check of the allocator covers, so the rings didn't move out either. A ring's chunk is made there with room
   for it and a sixteenth of the heap to spare, for the desktop, and stays while the heap is within its
   budget. Where that memory is in the heap the caches use (resizable BAR, a GPU sharing system memory), the caches keep
   the heap within its budget, counting the rings, and a chunk goes there while the heap's use stays within it.

   A window of its own is also no more than this process's share of it: a frame's ring takes at most BAR_RING_MAX of
   it, and this process's memory in the window is at most a quarter of it. Within the budget, NVIDIA's driver (615.78, open
   module) still failed to map a 64 MiB ring with 86 MiB budgeted ("NVRM: dmaAllocMapping_GM107: can't alloc VA space
   for mapping" in the kernel's log) and reported success: CPU writes to it then ran at 18 MB/s, against 5 GB/s, and
   every frame of that slot took half a second, in two games, in most runs, at a time that changed from run to run. No
   16 or 32 MiB ring failed. The budget can't see what the desktop and the driver itself keep mapped there, so the
   window is used as a small fast path, as other Vulkan translation layers do, and the rest of a bigger ring goes to
   host memory */
#define BAR_RING_MAX (32ull << 20)
static int g_bar_heap = -2; /* the heap of the device-local memory the CPU maps, -1 without one; -2 not looked for */
static int bar_heap(void) {
    const VkPhysicalDeviceMemoryProperties* mp;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (g_bar_heap != -2) return g_bar_heap;
    mp = vk_devmem_props();
    g_bar_heap = -1;
    for (u32 t = 0; t < mp->memoryTypeCount && g_bar_heap < 0; t++)
        if ((mp->memoryTypes[t].propertyFlags & want) == want) g_bar_heap = (int)mp->memoryTypes[t].heapIndex;
    return g_bar_heap;
}
/* whether that heap has room for a ring of add more bytes (0: keeping the ones there), back: one coming back from
   host memory, after it went there for want of room */
static int bar_room(const DevMemBudget* b, u64 add, int back) {
    const VkPhysicalDeviceMemoryProperties* mp;
    int heap = bar_heap();
    u64 size, use, budget, whole;
    if (heap < 0) return 0;
    /* the mapping failure above is of a big ring in the window, whatever the window's size: with resizable BAR too,
       the staging rings grew to 256 MiB there, and one game's draws then took up to 1.5 s each to write their data */
    if (add > BAR_RING_MAX) return 0;
    use = b[heap].usage;
    budget = b[heap].budget;
    if (heap == vk_mem_heap()) return use + add <= (back ? budget / 4 * 3 : budget);
    mp = vk_devmem_props();
    size = mp->memoryHeaps[heap].size;
    if (add && b[heap].block_bytes + add > size / 4) return 0;
    whole = use + budget + (1u << 20) >= size && use + budget <= size + (1u << 20); /* NVIDIA's use */
    if (whole) use = 0;                                                           /* the budget is what is left */
    return budget >= use + (add ? add + size / 16 : 0);
}

/* A wait for the GPU, in slices of a second. A GPU that stopped (NVIDIA's Xid 109, a context switch timeout) signals
   nothing, and the driver didn't report the device lost either: the frontend and the release thread waited here for
   good, the stall report (which the frontend makes) never came, and one game's window froze with no message. Once the
   semaphore hasn't moved for VK_GPU_STUCK_NS the hang is logged, with its checkpoints and the GPU's fault report, and
   the run ends.
   The graphics timeline's clock runs only while the queue holds work the GPU hasn't done. A value is handed to the
   recording thread before its submission, and that thread waits for each new pipeline at its bind (pipeline.c's
   vk_pipeline_resolve): in one race on a loaded host it waited past 15 s for a pipeline that a worker still made,
   with the GPU idle at the last value submitted and 2 MB of commands not recorded. The run ended there as "GPU
   stopped", where the GPU had nothing to do */
#define VK_GPU_STUCK_NS 15000000000ull
/* The driver gives the GPU's own report of a fault (VK_EXT_device_fault, record.c's fault_dump) only once it has
   declared the device lost, and a GPU that stopped is not lost yet: the driver's own timeout comes later than
   VK_GPU_STUCK_NS. A stopped GPU gets 20 s more to become a lost one, so that the report has the fault */
static void stopped_wait_lost(VkSemaphore sem, u64 value, VkFence fence) {
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &sem, .pValues = &value};
    for (u32 i = 0; i < 20; i++) {
        VkResult r = sem ? vkWaitSemaphores(g_vk.dev, &wi, 1000000000ull)
                         : vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, 1000000000ull);
        if (r == VK_ERROR_DEVICE_LOST) LOG("vulkan: the driver declared the stopped GPU's device lost\n");
        if (r != VK_TIMEOUT) return;
    }
}
void vk_wait_semaphore(VkSemaphore sem, u64 value) {
    VkSemaphoreWaitInfo wi = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &sem, .pValues = &value};
    u64 moved = plat_ns(), seen = 0;
    int said = 0, idle = 0;
    for (;;) {
        VkResult r = vkWaitSemaphores(g_vk.dev, &wi, 1000000000ull);
        if (r != VK_TIMEOUT) {
            VK_OK(r);
            /* going on would take the wait as done: the next Vulkan call crashes in the driver, and the log blames the
               game */
            if (r == VK_ERROR_DEVICE_LOST) {
                vk_ck_report();
                fatal("vulkan: the GPU device was lost (driver reset or crash)");
            }
            return;
        }
        u64 now = 0;
        if (!VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, sem, &now))) return;
        if (now != seen) { /* the GPU finished more: a long wait, not a stopped GPU */
            seen = now;
            moved = plat_ns();
        } else if (sem == g_gsem && now >= vk_rec_gfx_queued()) { /* the GPU has done all it has: it waits too */
            u64 t = plat_ns();
            if (t - moved >= VK_GPU_STUCK_NS && !said) {
                said = 1;
                LOG("vulkan: the GPU has done submission %llu, the last on its queue, and has waited %llu s for the "
                    "recording thread to submit up to %llu (%llu bytes of commands not recorded)\n",
                    (unsigned long long)now, (unsigned long long)((t - moved) / 1000000000ull),
                    (unsigned long long)value, (unsigned long long)vk_rec_behind());
            }
            idle = 1;
        } else if (idle) { /* the queue has work again: the clock of a stopped GPU starts now */
            idle = 0;
            moved = plat_ns();
        } else if (plat_ns() - moved >= VK_GPU_STUCK_NS) {
            stopped_wait_lost(sem, value, VK_NULL_HANDLE);
            vk_checkpoint_dump("GPU stopped");
            vk_ck_report(); /* what the submissions it has not finished hold */
            fatal("vulkan: the GPU stopped responding: no work finished for %llu s (at submission %llu, waiting for "
                  "%llu)",
                  VK_GPU_STUCK_NS / 1000000000ull, (unsigned long long)now, (unsigned long long)value);
        }
    }
}

/* a fence's wait, the same way: no fence of the runtime's own (a bind of sparse memory, a landing) takes seconds */
/* the fence waits since the last statistics and their time, from whichever thread waited (a bind of sparse memory, a
   granule of the mirror, a landing): each is a round trip to the GPU that vk_finish's sites and the timeline's waits
   do not count */
static u64 g_fence_waits, g_fence_wait_ns;
int vk_wait_fence(VkFence fence) {
    u64 t0 = plat_ns();
    for (;;) {
        VkResult r = vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, 1000000000ull);
        if (r != VK_TIMEOUT) {
            __atomic_fetch_add(&g_fence_waits, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_fence_wait_ns, plat_ns() - t0, __ATOMIC_RELAXED);
            return VK_OK(r);
        }
        if (plat_ns() - t0 >= VK_GPU_STUCK_NS) {
            stopped_wait_lost(VK_NULL_HANDLE, 0, fence);
            vk_checkpoint_dump("GPU stopped");
            vk_ck_report();
            fatal("vulkan: the GPU stopped responding: a fence wasn't signaled for %llu s",
                  VK_GPU_STUCK_NS / 1000000000ull);
        }
    }
}

u64 g_vk_wait_ns[VK_WAIT_N], g_vk_waits[VK_WAIT_N]; /* the GPU thread's alone, which writes them */
/* kind: VK_WAIT_*, for the statistics */
static void timeline_wait(u64 value, int kind) {
    u64 t0 = plat_ns(), done = 0;
    SNX_ZONE_C(z_wait, "vk timeline wait", 0xb05252);
    /* a wait for work already done is no stall */
    if (VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_gsem, &done)) && done >= value) return;
    int mine = gpu_wait_note("a graphics submission", value);
    if (mine) vk_gpuprof_waited(value, kind);
    SNX_ZONE_V(z, "timeline wait", value);
    vk_wait_semaphore(g_gsem, value);
    SNX_ZONE_END(z);
    gpu_wait_note(NULL, 0);
    if (mine) {
        g_vk_wait_ns[kind] += plat_ns() - t0;
        g_vk_waits[kind]++;
    }
}
/* the storage results of every submission up to value, which has finished, land in guest memory, in order */
/* inside a landing, a frame's recycling or a submission: what records there goes into a command buffer being begun,
   reset or submitted (vk_rec_unsafe) */
static int g_rec_unsafe;
int vk_rec_unsafe(void) { return g_rec_unsafe; }
/* A landing's own read of guest memory can finish the GPU's work (a page the mirror holds: vk_mirror_mem_need's
   vk_finish), whose land_through would land the next submissions' results inside this one. They landed before the
   older results that this landing still writes, and their landing rewrote the runs this one was writing from
   (buffer.c's g_chg): a run that ended before it began made a GPU-side write stamp of 2^64 bytes, which walked the
   GPU address space page by page under vk_lock for minutes (the trap thread's landings, and the game's threads
   faulting on results, waited). The landing in progress lands them, in order, or the next one does */
static int g_landing;
static void submit(const VkSemaphoreSubmitInfo* xw, const VkSemaphoreSubmitInfo* xs, int land, int why);
static void land_through(u64 value) {
    if (g_landing) return;
    g_landing = 1;
    g_rec_unsafe++;
    while (g_landed < value) {
        vk_buf_frame_done(++g_landed);
        vk_gfx_image_frame_done(g_landed);
        vk_astc_frame_done(g_landed); /* BC7 blocks read back for the cache, before the ring they are in is reused */
    }
    g_rec_unsafe--;
    /* what it recorded in a fresh slot (vk_cmd) and left recording goes now: the slot's recycling waits for it, and
       a fresh slot handed out recording would never be recycled */
    if (g_fr[g_cur].fresh && g_fr[g_cur].recording) submit(NULL, NULL, 0, VK_SUB_OTHER);
    g_landing = 0;
}

/* the submission the current slot's recycling waits for, set when the slot becomes the current one and fresh, 0 once
   it is recycled: the GPU thread waits for it with no lock held (vk_frame_ready). It waited in frame_recycle with
   vk_lock held, up to 3.1 s at a time in one game's menus, and the game's threads that landed results, changed
   memory or performed releases waited for vk_lock behind it */
static u64 g_recycle_at;
void vk_frame_ready(void) {
    u64 at = __atomic_load_n(&g_recycle_at, __ATOMIC_ACQUIRE), submitted = 0, done = 0;
    if (!at || vk_lock_held()) return;
    vk_submit_progress(&submitted, &done);
    if (at > submitted || at <= done) return; /* not handed to the GPU yet (frame_recycle waits then), or done */
    SNX_ZONE(z, "vk frame ready");
    timeline_wait(at, VK_WAIT_RECYCLE);
}
static void pool_recycle(Frame* f);
/* the frame's previous use is done, its results landed and what it held freed, before it records again. The landing
   can record and submit in the slot (vk_cmd): that work is part of the previous use, waited for and landed as well */
static void frame_recycle(Frame* f) {
    u64 done, landed = 0;
    SNX_ZONE(z, "vk frame recycle");
    __atomic_store_n(&g_recycle_at, 0, __ATOMIC_RELEASE);
    while ((done = f->last > f->hold ? f->last : f->hold) != landed) {
        timeline_wait(done, VK_WAIT_RECYCLE);
        land_through(done);
        landed = done;
    }
    f->last = 0;
    f->hold = 0;
    g_cmd.recycles++;
    vk_gpuprof_frame_done((u32)(f - g_fr)); /* its timestamps, read before the slot records again */
    dead_free(f);
    g_dead_bytes -= f->dead_bytes;
    f->dead_bytes = 0;
    vkResetCommandPool(g_vk.dev, f->pool, 0);
    /* the pools stay, reset: a frame's draws fill several, and making them again every frame costs. Only those the
       slot's recent uses never reached go */
    vk_desc_pools_keep(f->pool_d, &f->npool, &f->pool_peak, &f->pool_uses, f->took);
    for (int i = 0; i < f->npool; i++) vkResetDescriptorPool(g_vk.dev, f->pool_d[i], 0);
    f->ipool = 0;
    f->took = 0;
    memset(f->spare, 0, sizeof f->spare);
    vk_devmem_frame(); /* the driver's budgets, asked again */
    pool_recycle(f);
    vk_descriptors_reset((u32)(f - g_fr));
    vk_rec_frame_reset((u32)(f - g_fr)); /* the slot's sets made on the recording thread too */
    f->icb = 0;
    f->fresh = 0;
}

int vk_cmd_ready(void) {
    const Frame* f = &g_fr[g_cur];
    u64 last = f->last > f->hold ? f->last : f->hold, done = 0;
    if (f->recording || !f->fresh || !last) return 1;
    return vkGetSemaphoreCounterValue(g_vk.dev, g_gsem, &done) == VK_SUCCESS && done >= last;
}

VkCommandBuffer vk_cmd(void) {
    Frame* f = &g_fr[g_cur];
    if (!f->recording) {
        /* not inside a landing: the slot's results may be the ones landing, out of the rings its recycling hands
           back, and the recycling's own landing would recycle the slot again under itself. What a landing records in
           a fresh slot goes after its previous use's command buffers, in that use's rings and pools */
        if (f->fresh && !g_landing) {
            g_rec_unsafe++;
            frame_recycle(f);
            g_rec_unsafe--;
        }
        if (f->icb == f->ncb) {
            /* the pool's command buffers may still be recording on the recording thread, and a pool is used by one
               thread at a time. A frame's slot only grows to its most submissions, so this is rare */
            vk_rec_sync();
            VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = f->pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = 1};
            VkCommandBuffer* cbs = realloc(f->cbs, sizeof *cbs * (f->ncb + 1));
            if (!cbs) fatal("vulkan: out of memory for command buffers");
            f->cbs = cbs;
            VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &f->cbs[f->ncb]));
            f->ncb++;
        }
        vk_rec_begin(f->cbs[f->icb]);
        vk_gpuprof_cb_begin(f->cbs[f->icb]);
        f->recording = 1;
        g_cb_begun++;
    }
    return f->cbs[f->icb];
}

/* What the rings handed out, by ring and by where its memory is (above), for vk_buffer_stats */
static u64 g_xfer[XRING_N][XFER_N];
static u32 xfer_where(const DevMemInfo* info) {
    const VkPhysicalDeviceMemoryProperties* mp;
    VkMemoryPropertyFlags f;
    mp = vk_devmem_props();
    f = mp->memoryTypes[info->type].propertyFlags;
    return !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? XFER_HOST
         : f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT    ? XFER_MAPPED
                                                      : XFER_DEVICE;
}
/* the device ring's bytes since the last statistics by the call site that asked for them (a file's literal and a
   line), as vk_finish's are counted: which reader of guest memory fills the ring is not told by its total */
#define DEVICE_SITES 16
static struct {
    const char* file;
    int line;
    u64 n, bytes;
} g_device_sites[DEVICE_SITES];
void vk_stage_report(void) {
    static const char* const ring[XRING_N] = {"staging", "output", "back", "device", "readback", "stream"};
    char line[512];
    int at = 0;
    for (u32 r = 0; r < XRING_N; r++)
        at += snprintf(line + at, sizeof line - (size_t)at, " %s %llu/%llu/%llu", ring[r],
                       (unsigned long long)(g_xfer[r][XFER_HOST] >> 20),
                       (unsigned long long)(g_xfer[r][XFER_MAPPED] >> 20),
                       (unsigned long long)(g_xfer[r][XFER_DEVICE] >> 20));
    LOG("vulkan: ring MB over 600 presents (in host/mapped device/device memory):%s; the rings hold %llu MiB, %llu "
        "buffers made for them in all\n",
        line, (unsigned long long)(vk_ring_bytes() >> 20), (unsigned long long)g_cmd.made);
    memset(g_xfer, 0, sizeof g_xfer);
    for (u32 i = 0; i < DEVICE_SITES && g_device_sites[i].file; i++) {
        const char* f = strrchr(g_device_sites[i].file, '/');
        if (g_device_sites[i].n)
            LOG("vulkan:   device ring: %llu requests, %llu MB at %s:%d\n", (unsigned long long)g_device_sites[i].n,
                (unsigned long long)(g_device_sites[i].bytes >> 20), f ? f + 1 : g_device_sites[i].file,
                g_device_sites[i].line);
        g_device_sites[i].n = g_device_sites[i].bytes = 0;
    }
}

/* Whether the rings the CPU fills are in cached host memory (vk_stage_memory), not in memory for writes in order
   (devmem.h's DM_CACHED and DM_HOST). The second is a type that is not cached where the device has one, and a driver
   can map that write-combined: each store then goes to the memory through a few line buffers, and a ring's many
   small copies, each at its own alignment, leave lines half filled that the next copy flushes. Intel's Mesa driver
   has both types over the same system memory, so there the cached one costs the GPU nothing */
static int stage_cached(void) {
    return g_settings.staging_memory ? g_settings.staging_memory == 2
                                     : g_vk.driver_id == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA;
}
/* a new buffer of size bytes for ring, handed out to slot (a frame slot plus one): in device memory the CPU maps when
   bar, else in host memory (the device ring's in device memory). Its entry's index plus one, 0 when none was made */
static u32 chunk_new(u32 ring, u64 size, int bar, u32 slot) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                       VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                       VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                                       VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                       (g_vk.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0),
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant aci = {.at = bar ? DM_WINDOW : DM_HOST, .mapped = 1};
    DevMemInfo info;
    VkBuffer buf;
    DevMem mem;
    u32 i = 0;
    if (ring == XRING_BACK) { /* storage: render targets are swizzled into it (vk/blocklinear.c) */
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        aci.at = DM_CACHED;
    } else if (ring == XRING_DEVICE)
        aci = (DevMemWant){.at = DM_DEVICE};
    else if (stage_cached() && (!bar || g_vk.mem_model == VK_MEM_UMA)) /* a discrete GPU's window has no such type */
        aci.at = DM_CACHED;
    while (i < g_cmd.nchunk && g_cmd.chunk[i].buf) i++;
    if (i == g_cmd.capchunk) {
        u32 cap = g_cmd.capchunk ? g_cmd.capchunk * 2 : 32;
        Chunk* grown = cap > g_cmd.capchunk && (u64)cap <= SIZE_MAX / sizeof(Chunk)
                           ? realloc(g_cmd.chunk, cap * sizeof(Chunk))
                           : NULL;
        if (!grown) fatal("vulkan: out of memory for the list of %u ring buffers", cap);
        g_cmd.chunk = grown;
        g_cmd.capchunk = cap;
    }
    if (!VK_OK(vk_buffer_create(&bci, &aci, &buf, &mem, &info))) return 0;
    vk_mem_track(mem, VK_OWN_STAGING);
    if (ring == XRING_STAGE && !g_cmd.stage_told) {
        const VkPhysicalDeviceMemoryProperties* mp;
        mp = vk_devmem_props();
        g_cmd.stage_told = 1;
        LOG("vulkan: the staging ring is in memory type %u, %s (vk_stage_memory=%d)\n", info.type,
            mp->memoryTypes[info.type].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "cached"
                                                                                               : "not cached",
            g_settings.staging_memory);
    }
    if (i == g_cmd.nchunk) g_cmd.nchunk++;
    g_cmd.chunk[i] = (Chunk){.buf = buf,
                             .mem = mem,
                             .ptr = info.ptr,
                             .size = size,
                             .ring = ring,
                             .in = xfer_where(&info),
                             .slot = slot,
                             .bar = bar && xfer_where(&info) == XFER_MAPPED};
    g_cmd.made++;
    return i + 1;
}
static void chunk_destroy(Chunk* c) {
    if (c->flush) g_cmd.unflushed--;
    vk_mem_untrack(c->mem);
    vk_devmem_buffer_destroy(c->buf, c->mem);
    *c = (Chunk){0};
}
/* host writes to a chunk of the staging or output ring, made visible to the GPU at the next submit */
static void chunk_written(Chunk* c) {
    if ((c->ring == XRING_STAGE || c->ring == XRING_OUTPUT || c->ring == XRING_STREAM) && !c->flush) {
        c->flush = 1;
        g_cmd.unflushed++;
    }
}
/* the size class of a buffer for a request of size bytes, bigger than a chunk: the chunk size doubled until it fits, at
   most an allocation */
static u64 big_size(u64 size) {
    u64 s = chunk_size();
    while (s < size) s *= 2;
    return s < g_vk.alloc_cap ? s : g_vk.alloc_cap;
}
/* whether pooled c is handed out before than: the smaller (a big buffer's size class), then the one handed out the
   most of, then the one given back last. Memory the CPU wrote before takes its writes several times faster than a new
   buffer's: an 8192x8192 texture's copy engine upload took 43 ms in a new 32 MiB buffer, 28 ms in a 256 MiB ring
   written before, and the CPU's first write to each page of a chunk is a page fault. Of the chunks a frame gave back
   together one it barely filled could come first: an ASTC texture's 16 MiB copy source took 14 ms in a chunk written
   64 KiB into, where its 8 MiB level took 0.3 ms in one written whole */
static int pool_before(const Chunk* c, const Chunk* than) {
    if (c->size != than->size) return c->size < than->size;
    if (c->handed != than->handed) return c->handed > than->handed;
    return c->freed >= than->freed;
}
/* A chunk of size bytes for ring, or a big buffer of that size class or more (big), handed out to slot (a frame slot
   plus one): the first of the pool that suits (pool_before), else a new one. bar: the output ring's first
   BAR_RING_MAX of a frame, in device memory the CPU maps when it has room (bar_room): one there, or a new one there,
   else one in host memory, which a new one there replaces only below the room a ring coming back from host memory
   needs. A ring that wants a chunk outside the window with none left in the pool hands out of its smallest pooled big
   buffer instead, up to that buffer's size: a frame of more small requests than the pool kept chunks for (an ASTC
   texture's strips after a texture's bands) writes memory written before, not new chunks. 0 when none could be made */
static u32 chunk_take(u32 ring, u64 size, int big, int bar, u32 slot) {
    u32 here = 0, other = 0, lend = 0; /* the chunks of the pool in the memory wanted, in the other memory, and the
                                          big buffer a ring with none of them hands out of */
    for (u32 i = 0; i < g_cmd.nchunk; i++) {
        const Chunk* c = &g_cmd.chunk[i];
        u32* pick;
        if (!c->buf || c->slot || c->ring != ring) continue;
        if (!big && c->big) {
            if (!lend || pool_before(c, &g_cmd.chunk[lend - 1])) lend = i + 1;
            continue;
        }
        if ((int)c->big != big || (big ? c->size < size : c->size != size)) continue;
        pick = (int)c->bar == bar ? &here : &other;
        if (!*pick || pool_before(c, &g_cmd.chunk[*pick - 1])) *pick = i + 1;
    }
    if (!here && bar) {
        DevMemBudget budget[VK_MAX_MEMORY_HEAPS];
        vk_devmem_budgets(budget);
        if (!bar_room(budget, size, other != 0)) {
            here = other;
            bar = 0;
        }
    }
    if (!here && !bar) here = lend;
    if (!here) {
        if ((here = chunk_new(ring, size, bar, slot))) g_cmd.chunk[here - 1].big = (u32)big;
        return here;
    }
    g_cmd.chunk[here - 1].slot = slot;
    return here;
}
/* The stream (step 5 of the buffer and budget plan). A frame's rings come back with its slot, so a frame that
   draws long without a present kept every chunk it filled, and ended early to give them back (frame_check). Most of
   what the rings carry is read by work recorded at once: a texture's or a target's bytes from guest memory, the pages
   the page cache takes, the ranges the buffer cache copies for one submission. Those go through the stream: a chunk
   has the number of the submission after the one being recorded when it last handed out bytes, and is handed out
   again once that submission is done, whatever the frame. One submission later than the bytes' own, as a wait in
   the middle of an upload (astc.c's queue) submits between a strip's bytes and the commands that read them. A busy
   chunk means another chunk, never a wait. A request bigger than a chunk gets a buffer of its size in whole MiB,
   not of the next doubled size. A chunk no request took for STREAM_IDLE frames goes, a big buffer after
   STREAM_BIG_IDLE: the stream holds what the last frames' busiest moment took */
#define STREAM_IDLE     32u
#define STREAM_BIG_IDLE 8u
static u64 stream_done(void) {
    u64 done = 0;
    if (g_gsem) VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_gsem, &done));
    return done;
}
/* a chunk of the stream of size bytes, or a big buffer of size or more (big), that no work reads: the pool's best
   (pool_before), else a new one. Its entry's index plus one, 0 when none could be made */
static u32 stream_take(u64 size, int big) {
    const u64 done = stream_done();
    u32 pick = 0;
    for (u32 i = 0; i < g_cmd.nchunk; i++) {
        const Chunk* c = &g_cmd.chunk[i];
        if (!c->buf || c->ring != XRING_STREAM || i + 1 == g_cmd.stream_cur || c->sub > done) continue;
        if ((int)c->big != big || (big ? c->size < size : c->size != size)) continue;
        if (!pick || pool_before(c, &g_cmd.chunk[pick - 1])) pick = i + 1;
    }
    if (!pick && (pick = chunk_new(XRING_STREAM, size, 0, 0))) g_cmd.chunk[pick - 1].big = (u32)big;
    return pick;
}
/* the stream's chunks that went unused: once a frame slot is recycled */
static void stream_trim(void) {
    u64 done = 0;
    int asked = 0;
    for (u32 i = 0; i < g_cmd.nchunk; i++) {
        Chunk* c = &g_cmd.chunk[i];
        if (!c->buf || c->ring != XRING_STREAM || i + 1 == g_cmd.stream_cur) continue;
        if (g_frame_id - c->freed < (c->big ? STREAM_BIG_IDLE : STREAM_IDLE)) continue;
        if (!asked) {
            done = stream_done();
            asked = 1;
        }
        if (c->sub <= done) chunk_destroy(c);
    }
}
u8* vk_stream_upload(u64 size, u64 align, VkBuffer* buf, u64* off) {
    if (!g_settings.stream_ring) return vk_stage_upload(size, align, buf, off);
    vk_cmd(); /* a frame is being recorded: the bytes' readers go in its commands */
    if (align < 16) align = 16;
    if (size > g_vk.alloc_cap) return NULL; /* no allocation is bigger (memory.c) */
    u32 at_chunk;
    u64 at = 0;
    if (size > chunk_size()) {
        const u64 mib = 1ull << 20, whole = (size + mib - 1) & ~(mib - 1);
        if (!(at_chunk = stream_take(whole < g_vk.alloc_cap ? whole : g_vk.alloc_cap, 1))) return NULL;
    } else {
        at = (g_cmd.stream_used + align - 1) & ~(align - 1);
        if (!g_cmd.stream_cur || at + size > g_cmd.chunk[g_cmd.stream_cur - 1].size) {
            const u32 next = stream_take(chunk_size(), 0);
            if (!next) return NULL;
            g_cmd.stream_cur = next;
            at = 0;
        }
        at_chunk = g_cmd.stream_cur;
        g_cmd.stream_used = at + size;
    }
    Chunk* c = &g_cmd.chunk[at_chunk - 1];
    c->sub = vk_submit_number() + 1;
    c->freed = g_frame_id; /* when it was last used: stream_trim */
    chunk_written(c);
    if (at + size > c->handed) c->handed = at + size;
    g_xfer[XRING_STREAM][c->in] += size;
    g_cmd.stream_since += c->big ? c->size : size;
    *buf = c->buf;
    *off = at;
    return c->ptr + at;
}
/* the stream's chunks that work not yet done reads: their bytes, and the oldest of their submissions that is on the
   queue (0: none is) */
static u64 stream_busy(u64* oldest) {
    const u64 done = stream_done();
    u64 bytes = 0;
    *oldest = 0;
    for (u32 i = 0; i < g_cmd.nchunk; i++) {
        const Chunk* c = &g_cmd.chunk[i];
        if (!c->buf || c->ring != XRING_STREAM || c->sub <= done) continue;
        bytes += c->size;
        if (c->sub <= g_gval && (!*oldest || c->sub < *oldest)) *oldest = c->sub;
    }
    return bytes;
}

/* The slot's chunks and big buffers back to the pool, once its work is done; the pool then keeps no more of a ring's
   chunks, and of its big buffers, than the most a frame of the last VK_POOL_FRAMES took (a chunk at least, as a ring
   kept its smallest size) for each slot, less what the other slots hold, the longest unused going first: a slot whose
   last frame took none still finds them when its next frame does. A chunk of the window whose heap has no room for it
   anymore goes too: the next frame that wants one there makes it again where chunk_take finds room */
static void pool_recycle(Frame* f) {
    u32 slot = (u32)(f - g_fr) + 1, keep[FRAME_RINGS][2], pooled[FRAME_RINGS][2] = {{0}}, held[FRAME_RINGS][2] = {{0}};
    u64 size = chunk_size();
    DevMemBudget budget[VK_MAX_MEMORY_HEAPS];
    vk_devmem_budgets(budget);
    for (u32 r = 0; r < FRAME_RINGS; r++)
        for (u32 big = 0; big < 2; big++) {
            keep[r][big] = !big;
            for (u32 k = 0; k < VK_POOL_FRAMES; k++)
                if (g_cmd.took[k][r][big] > keep[r][big]) keep[r][big] = g_cmd.took[k][r][big];
        }
    g_cmd.itook = (g_cmd.itook + 1) % VK_POOL_FRAMES;
    memset(g_cmd.took[g_cmd.itook], 0, sizeof g_cmd.took[g_cmd.itook]);
    for (u32 i = 0; i < g_cmd.nchunk; i++) {
        Chunk* c = &g_cmd.chunk[i];
        if (!c->buf || c->ring >= FRAME_RINGS) continue; /* the stream's are no slot's (stream_trim) */
        if (c->slot && c->slot != slot) {
            held[c->ring][c->big]++;
            continue;
        }
        if ((!c->big && c->size != size) || (c->bar && !bar_room(budget, 0, 0)))
            chunk_destroy(c);
        else {
            if (c->slot) c->freed = g_frame_id;
            c->slot = 0;
            pooled[c->ring][c->big]++;
        }
    }
    for (u32 r = 0; r < FRAME_RINGS; r++)
        for (u32 big = 0; big < 2; big++)
            for (; pooled[r][big] && pooled[r][big] + held[r][big] > VK_FRAMES * keep[r][big]; pooled[r][big]--) {
                u32 oldest = 0;
                for (u32 i = 0; i < g_cmd.nchunk; i++) {
                    const Chunk* c = &g_cmd.chunk[i];
                    if (c->buf && !c->slot && c->ring == r && c->big == big &&
                        (!oldest || c->freed < g_cmd.chunk[oldest - 1].freed))
                        oldest = i + 1;
                }
                chunk_destroy(&g_cmd.chunk[oldest - 1]);
            }
    memset(f->ring, 0, sizeof f->ring);
    f->filled = 0;
    stream_trim();
}

/* size bytes of the frame's ring at *off of *buf, the chunk's memory in *mem: the chunk, NULL when none could be had.
   A request no bigger than a chunk is handed out of the ring's chunk, or of the next one when that is full (a big
   buffer chunk_take lent it, counted as a big one); a bigger one gets a big buffer of its own */
static const Chunk* ring_take(Frame* f, u32 ring, u64 size, u64 align, VkBuffer* buf, u64* off, DevMem* mem) {
    Ring* r = &f->ring[ring];
    u32 slot = (u32)(f - g_fr) + 1;
    Chunk* c;
    u64 at;
    if (align < 16) align = 16;
    if (size > g_vk.alloc_cap) return NULL; /* no allocation is bigger (memory.c) */
    if (size > chunk_size()) {              /* in host memory, past the window's share (BAR_RING_MAX) */
        u32 big = chunk_take(ring, big_size(size), 1, 0, slot);
        if (!big) return NULL;
        g_cmd.took[g_cmd.itook][ring][1]++;
        c = &g_cmd.chunk[big - 1];
        chunk_written(c);
        f->filled += c->size;
        at = 0;
    } else {
        at = (r->used + align - 1) & ~(align - 1);
        if (!r->cur || at + size > g_cmd.chunk[r->cur - 1].size) {
            if (r->cur) { /* full: what was written to it is flushed with the next submit */
                chunk_written(&g_cmd.chunk[r->cur - 1]);
                f->filled += g_cmd.chunk[r->cur - 1].size;
            }
            r->used = 0;
            r->cur = chunk_take(ring, chunk_size(), 0,
                                ring == XRING_OUTPUT && (r->n + 1) * chunk_size() <= BAR_RING_MAX, slot);
            if (!r->cur) return NULL;
            r->n++;
            g_cmd.took[g_cmd.itook][ring][g_cmd.chunk[r->cur - 1].big]++; /* a big buffer lent counts as one */
            at = 0;
        }
        c = &g_cmd.chunk[r->cur - 1];
        r->used = at + size;
    }
    if (at + size > c->handed) c->handed = at + size;
    g_xfer[ring][c->in] += size;
    *buf = c->buf;
    *off = at;
    if (mem) *mem = c->mem;
    return c;
}
static u8* ring_alloc(Frame* f, u32 ring, u64 size, u64 align, VkBuffer* buf, u64* off, DevMem* mem) {
    const Chunk* c = ring_take(f, ring, size, align, buf, off, mem);
    return c ? c->ptr + *off : NULL;
}

static u8* stage_alloc(u64 size, u64 align, VkBuffer* buf, u64* off, DevMem* mem, int output)
{
	Frame* f;

	vk_cmd(); /* completion of the slot's previous use precedes any reuse */
	f = &g_fr[g_cur];
	return ring_alloc(f, output ? XRING_OUTPUT : XRING_STAGE, size, align, buf, off, mem);
}
u8* vk_stage_output(u64 size, u64 align, VkBuffer* buf, u64* off, DevMem* mem)
{
	return stage_alloc(size, align, buf, off, mem, 1);
}

u8* vk_stage(u64 size, u64 align, VkBuffer* buf, u64* off)
{
	return stage_alloc(size, align, buf, off, NULL, 1);
}

/* Streamed uploads can be hundreds of megabytes per frame. CPU writes through
 * a mapped device-memory window are slower than writes to host RAM. Keep these
 * snapshots in host memory; stable buffers get their own device-local copies. */
u8* vk_stage_upload(u64 size, u64 align, VkBuffer* buf, u64* off)
{
	return stage_alloc(size, align, buf, off, NULL, 0);
}

/* Guest input read once by a copy or conversion. Keep GPU-owned bytes near the GPU; CPU-only bytes go through
   host RAM rather than CPU writes through the device's mapped window. Both rings share the frame's fence. */
u8* vk_stage_guest_input(u64 gva, u64 source_size, u64 bytes, VkBuffer* buf, u64* off) {
    int gpu = vk_uma_pending(gva, source_size);
    if (!gpu && own_any())
        for (u64 o = 0; o < source_size;) {
            u64 cpu = 0, n = gmmu_run(gva + o, source_size - o, &cpu);
            if (n) {
                if (own_trapped(cpu, n)) {
                    gpu = 1;
                    break;
                }
            } else {
                n = 0x1000 - ((gva + o) & 0xfff);
                if (n > source_size - o) n = source_size - o;
            }
            o += n;
        }
    return gpu ? vk_stage(bytes, vk_stage_align(), buf, off) : vk_stream_upload(bytes, vk_stage_align(), buf, off);
}

/* The staging ring can be device-local memory behind the BAR, where every CPU read crosses PCIe uncached: comparing
   a frame's storage results there cost one game most of its GPU thread. The frame's results are copied
   on the GPU into host-cached memory instead, and read from there. */
u8* vk_stage_back(u64 size, VkBuffer* buf, u64* off, DevMem* mem) {
    vk_cmd();
    return ring_alloc(&g_fr[g_cur], XRING_BACK, size, g_vk.props.limits.nonCoherentAtomSize, buf, off, mem);
}

/* What the GPU makes for itself to read, in the frame's device memory. In host memory, where staging is, each byte
   crossed PCIe twice or more: written there by the GPU and read back by every draw that bound it */
int vk_stage_device_at(const char* file, int line, u64 size, u64 align, VkBuffer* buf, u64* off) {
    vk_cmd();
    for (u32 i = 0; i < DEVICE_SITES; i++) {
        if (!g_device_sites[i].file) {
            g_device_sites[i].file = file;
            g_device_sites[i].line = line;
        }
        if (g_device_sites[i].file == file && g_device_sites[i].line == line) {
            g_device_sites[i].n++;
            g_device_sites[i].bytes += size;
            break;
        }
    }
    return ring_take(&g_fr[g_cur], XRING_DEVICE, size, align, buf, off, NULL) != NULL;
}

/* the host writes to the chunks the staging and output rings hand out of, and to those they filled and the frame's
   big buffers since the last submit, made visible to the GPU */
static void rings_flush(const Frame* f) {
    g_cmd.stream_since = 0;
    for (u32 r = XRING_STAGE; r <= XRING_OUTPUT; r++)
        if (f->ring[r].cur) vk_devmem_flush(g_cmd.chunk[f->ring[r].cur - 1].mem, 0, VK_WHOLE_SIZE);
    for (u32 i = 0; g_cmd.unflushed && i < g_cmd.nchunk; i++)
        if (g_cmd.chunk[i].flush) {
            vk_devmem_flush(g_cmd.chunk[i].mem, 0, VK_WHOLE_SIZE);
            g_cmd.chunk[i].flush = 0;
            g_cmd.unflushed--;
        }
}

static void mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 src_stage, VkAccessFlags2 src_access,
                        VkPipelineStageFlags2 dst_stage, VkAccessFlags2 dst_access) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = src_stage,
                          .srcAccessMask = src_access,
                          .dstStageMask = dst_stage,
                          .dstAccessMask = dst_access};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
/* the frame's commands submitted, waiting for xw and signaling xs besides the graphics timeline (one each at most), for
   why (VK_SUB_*: the statistics) */
static void submit(const VkSemaphoreSubmitInfo* xw, const VkSemaphoreSubmitInfo* xs, int land, int why) {
    SNX_ZONE(z_submit, "vk submit");
    vk_render_end();
    g_rec_unsafe++;
    Frame* f = &g_fr[g_cur];
    VkCommandBuffer cb = vk_cmd(); /* a submit with nothing recorded still has to wait and signal */
    vk_query_flush(cb);            /* the samples-passed counts of the passes ended (query.c) */
    vk_buf_host_barrier(cb, g_gval + 1);
    VkCommandBuffer up = VK_NULL_HANDLE;
    if (f->up) { /* the upload command buffer's copies and fills before everything the frame's command buffer reads or
                    writes */
        up = f->ups[f->icb];
        mem_barrier(up, VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        f->up = 0;
    }
    /* staging memory may not be host coherent: the host writes are made visible to the GPU here */
    rings_flush(f);
    VkSemaphoreSubmitInfo w[3];
    u32 nw = 0;
    if (g_after[0].semaphore) { /* at most one of the two: vk_submit_after's comes with no other */
        xw = &g_after[0];
        xs = &g_after[1];
    }
    if (xw) w[nw++] = *xw;
    if (g_vk_cs_wait_value) { /* draws read compute results on the GPU: after that batch */
        w[nw++] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                          .semaphore = g_vk_csem,
                                          .value = g_vk_cs_wait_value,
                                          .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
        g_vk_cs_wait_value = 0;
    }
    if (vk_sparse_flush(&w[nw])) nw++; /* after the sparse binds made since the last submission (memory.c) */
    VkSemaphoreSubmitInfo s[2] = {{.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                   .semaphore = g_gsem,
                                   .value = g_gval + 1,
                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT}};
    if (xs) s[1] = *xs;
    vk_ck(cb, VK_CK_SUBMIT, (u32)why, 0, 0, 0);
    vk_gpuprof_cb_end(cb, why, g_gval + 1);
    /* ended and submitted after its commands, on the recording thread (a submission that fails signals the timeline
       from the host there, so waits for it end) */
    vk_rec_submit(up, cb, w, nw, s, xs ? 2u : 1u);
    memset(g_after, 0, sizeof g_after);
    __atomic_store_n(&g_gval, g_gval + 1, __ATOMIC_RELAXED); /* read by the stall report too */
    g_cmd.submits[why]++;
    g_sub_ns = plat_ns();
    f->recording = 0;
    f->icb++;
    SNX_ZONE_VALUE(z_submit, f->icb);
    f->last = g_gval;
    g_rec_unsafe--;
    /* results of submissions the GPU has finished land now, so a later release doesn't find them pending */
    u64 done = 0;
    if (land && VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_gsem, &done)))
        land_through(done < g_gval ? done : g_gval);
    SNX_ZONE_END(z_submit);
}

static void submit_why(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal, int why) {
    VkSemaphoreSubmitInfo w = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait,
                               .stageMask = wait_stage};
    VkSemaphoreSubmitInfo s = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = signal,
                               .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    submit(wait ? &w : NULL, signal ? &s : NULL, 1, why);
}
/* a submission that signals a binary semaphore is the present's (renderer.c) */
void vk_submit(VkSemaphore wait, VkPipelineStageFlags2 wait_stage, VkSemaphore signal) {
    submit_why(wait, wait_stage, signal, signal ? VK_SUB_PRESENT : VK_SUB_OTHER);
}
/* The upload command buffer runs first in its submission, so its copies need no rendering pass of the frame's command
   buffer to end. A pool is used by one thread at a time: its commands go through the recording thread's ring like the
   frame's, and it is made from the frame's pool with the ring drained, as vk_cmd makes its command buffers */
VkCommandBuffer vk_cmd_upload(int gpu) {
    Frame* f = &g_fr[g_cur];
    vk_cmd(); /* the frame's command buffer recording too: every submit then takes this one with it */
    if (!f->up) {
        if (f->icb >= f->nup) { /* up to this submission's: the ones before it may have had none */
            vk_rec_sync();
            VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                              .commandPool = f->pool,
                                              .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                              .commandBufferCount = f->icb + 1 - f->nup};
            VkCommandBuffer* ups = realloc(f->ups, sizeof *ups * (f->icb + 1));
            if (!ups) fatal("vulkan: out of memory for command buffers");
            f->ups = ups;
            VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &f->ups[f->nup]));
            f->nup = f->icb + 1;
        }
        vk_rec_begin_upload(f->ups[f->icb]);
        f->up = 1;
    }
    if (gpu && f->up == 1) { /* what earlier submissions wrote, before the copies read it */
        mem_barrier(f->ups[f->icb], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
        f->up = 2;
    }
    return f->ups[f->icb];
}
VkCommandBuffer vk_cmd_upload_recording(void) {
    const Frame* f = &g_fr[g_cur];
    return f->up ? f->ups[f->icb] : VK_NULL_HANDLE;
}

/* Ownership readback runs while its ranges are marked landing. Flush only an
   open command buffer: starting/recycling a frame or landing completed results
   here could acquire those same ranges and wait for this thread itself. */
void vk_submit_for_readback(void) {
    vk_submit_open();
    vk_rec_sync(); /* the immediate copy must follow the recorder's queue calls */
}
void vk_submit_open(void) {
    if (g_fr[g_cur].recording) submit(NULL, NULL, 0, VK_SUB_READBACK);
}
/* vk_submit_open's, counted apart. It takes what the next submission waits for (vk_submit_after's, the sparse binds
   made since the last), as any submission does */
void vk_submit_split(void) {
    if (g_fr[g_cur].recording) submit(NULL, NULL, 0, VK_SUB_SPLIT);
}
/* vk_submit_open's for a piece of a background encode (astc.c), counted apart. It takes what the next submission waits
   for (vk_submit_after's, the sparse binds made since the last), as any submission does */
void vk_submit_background(void) {
    if (g_fr[g_cur].recording) submit(NULL, NULL, 0, VK_SUB_BACKGROUND);
}
void vk_submit_recorded(void) {
    if (g_fr[g_cur].recording) submit_why(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_SUB_OTHER);
}
/* What is recorded goes to the GPU, and this waits until the GPU has done it. It lands nothing, so it may run while a
   draw is set up: that draw starts its setup again on the new submission number */
void vk_submit_upload_wait(void) {
    if (g_fr[g_cur].recording) submit(NULL, NULL, 0, VK_SUB_OTHER);
    timeline_wait(g_gval, VK_WAIT_FINISH);
}
void vk_submit_after(VkSemaphore sem, u64 wait, u64 signal) {
    g_after[0] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = sem,
                                         .value = wait, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    g_after[1] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = sem,
                                         .value = signal, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
}

u64 g_vk_finishes, g_vk_finish_ns;
/* the finishes since the last stats by their call site (a file's literal and a line), with their time, under vk_lock
   as vk_finish is: one game made nine a frame, a round trip each with the GPU idle after it */
#define FINISH_SITES 32
static struct {
    const char* file;
    int line;
    u64 n, ns;
} g_finish_sites[FINISH_SITES];
void vk_finish_at(const char* file, int line) {
    u64 t0 = plat_ns();
    SNX_ZONE(z_finish, "vk finish");
    submit_why(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_SUB_FINISH);
    timeline_wait(g_gval, VK_WAIT_FINISH);
    land_through(g_gval);
    u64 ns = plat_ns() - t0;
    g_vk_finishes++;
    g_vk_finish_ns += ns;
    for (u32 i = 0; i < FINISH_SITES; i++) {
        if (!g_finish_sites[i].file) {
            g_finish_sites[i].file = file;
            g_finish_sites[i].line = line;
        }
        if (g_finish_sites[i].file == file && g_finish_sites[i].line == line) {
            g_finish_sites[i].n++;
            g_finish_sites[i].ns += ns;
            break;
        }
    }
}
void vk_finishes_log(void) {
    u64 total = 0;
    for (u32 i = 0; i < FINISH_SITES && g_finish_sites[i].file; i++) total += g_finish_sites[i].n;
    for (u32 k = 0; total && k < 8; k++) {
        u32 best = FINISH_SITES;
        for (u32 i = 0; i < FINISH_SITES && g_finish_sites[i].file; i++)
            if (g_finish_sites[i].n && (best == FINISH_SITES || g_finish_sites[i].n > g_finish_sites[best].n)) best = i;
        if (best == FINISH_SITES) break;
        const char* f = strrchr(g_finish_sites[best].file, '/');
        LOG("vulkan:   %llu finishes (%.0f%%, %.0f ms) at %s:%d\n", (unsigned long long)g_finish_sites[best].n,
            100.0 * (double)g_finish_sites[best].n / (double)total, (double)g_finish_sites[best].ns / 1e6,
            f ? f + 1 : g_finish_sites[best].file, g_finish_sites[best].line);
        g_finish_sites[best].n = 0;
    }
    for (u32 i = 0; i < FINISH_SITES && g_finish_sites[i].file; i++) g_finish_sites[i].n = g_finish_sites[i].ns = 0;
    u64 fences = __atomic_exchange_n(&g_fence_waits, 0, __ATOMIC_RELAXED),
        fence_ns = __atomic_exchange_n(&g_fence_wait_ns, 0, __ATOMIC_RELAXED);
    if (fences)
        LOG("vulkan:   %llu waits for a fence (%.1f ms): sparse binds, the mirror's granules and landings\n",
            (unsigned long long)fences, (double)fence_ns / 1e6);
}

/* A present ends the frame: the next one records in the other slot, once that slot's previous use is done. What the
   submission's landing recorded in this slot and left recording goes too, without a landing of its own (a write-back
   that gives up after its first commands; every release, when a release began the slot's command buffer). Left open,
   the frames coming round to the slot found it recording: vk_cmd handed it out without beginning it on the recording
   thread, so its commands went to the driver from the GPU thread, descriptor set stand-ins too (record.c). The
   landing stays before the switch: after it, a vk_cmd in the landing would recycle the next slot, whose own results
   its land_through can't land inside this landing (g_landing), and whose rings they are in would be reused */
void vk_frame_next(void) {
    if (g_fr[g_cur].recording) submit_why(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, VK_SUB_FRAME);
    if (g_fr[g_cur].recording) submit(NULL, NULL, 0, VK_SUB_FRAME);
    g_cur = (g_cur + 1) % VK_FRAMES;
    g_fr[g_cur].fresh = 1;
    g_frame_id++;
    const Frame* f = &g_fr[g_cur];
    __atomic_store_n(&g_recycle_at, f->last > f->hold ? f->last : f->hold, __ATOMIC_RELEASE);
}
/* the GPU thread's recording time a submission holds at most */
#define SUBMIT_EVERY_NS 1000000
/* A frame that goes long without a present (a loading screen) still has to hand its pools and rings back: one with
   many submissions, descriptor pools or descriptor tables ends here, at the start of a draw or dispatch, before
   anything is set up (a draw's setup must not submit). Otherwise what was recorded goes to the GPU once it holds a
   millisecond of the GPU thread's work. Submissions came only when something needed results, so in one game the GPU
   sat idle through the first 12 ms of the frame's 21 ms on the GPU thread, got all of it in a burst at the end and
   finished 6 ms after the GPU thread, while the game waited for that frame's fence before submitting the next (34.7
   fps, 40.3 with these). Earlier submissions also land their storage results sooner, so the pending results a draw's
   buffers are looked up in stay fewer. 2 ms and 0.5 ms were both slower. The limit on submissions leaves room for
   these in a slow frame: one that reaches it records the rest in the other slot, and its present then waits for this
   one's work.
   A frame's chunks and big buffers come back only with its slot, so it also ends once it has written
   VK_FRAME_RING_CAPS allocations' worth to them: a game's load that drew without a present for 8 s kept every ring it
   made, GPU memory went from 270 to 1062 MiB, and a phone's system ended the app at its 4 GiB limit. The bands of a
   copy too big for one allocation check too (dma.c, blit.c), as nothing is set up between two of them: the copy
   engine's upload of an 8192x8192 RGBA8 texture's eight levels wrote 1365 MiB to one frame's rings before the next
   draw ended it */
/* the bytes this use of the frame has written to rings: in the chunks they hand out of, the ones they filled and its
   big buffers */
static u64 frame_rings(const Frame* f) {
    u64 n = f->filled;
    for (u32 r = 0; r < FRAME_RINGS; r++) n += f->ring[r].used;
    return n;
}
/* the bytes a frame writes to its rings before it ends, as it has written rings already: VK_FRAME_RING_CAPS
   allocations, and on a GPU that shares the system's memory, where the rings are memory the caches budget, an eighth
   of the budget, but no less than two chunks (uma_budget.h). The budget is asked only past those two */
static u64 frame_ring_cap(u64 rings) {
    u64 caps = VK_FRAME_RING_CAPS * g_vk.alloc_cap, floor = 2 * chunk_size(), usage, budget;
    if (g_vk.mem_model != VK_MEM_UMA || rings < floor) return caps;
    vk_mem_use(&usage, &budget);
    return vk_uma_ring_cap(caps, budget, floor);
}
static void frame_check(void) {
    const Frame* f = &g_fr[g_cur];
    /* a fresh frame has recorded nothing, and its counts are still its last use's until frame_recycle: ending it
       would only move on to the slot whose work was just submitted, and wait for it */
    if (f->fresh) return;
    u64 rings = frame_rings(f), cap = frame_ring_cap(rings);
    if (rings >= cap) {
        LOG_LIMIT(
            4,
            "vulkan: frame %llu ends before a present: %llu MiB written to its rings, which end a frame at %llu MiB\n",
            (unsigned long long)g_frame_id, (unsigned long long)(rings >> 20), (unsigned long long)(cap >> 20));
        vk_frame_next();
    } else if (f->icb >= 128 || f->ipool >= 32 || vk_rec_frame_sets(g_cur) >= 32 * 1024 || !vk_descriptors_room(g_cur))
        vk_frame_next();
    else if (g_cmd.stream_since >= cap) {
        /* The stream wrote a frame's worth since the last submit: what was recorded goes to the GPU, so its chunks
           come free, and the frame goes on. Past twice that in chunks still read, this waits for the oldest of them:
           the stream never waits where it hands bytes out (a draw's setup), and a copy in bands, with no draw
           between them, would take a chunk a band without end (the 1365 MiB above) */
        u64 oldest;
        submit(NULL, NULL, 1, VK_SUB_PERIODIC);
        if (stream_busy(&oldest) >= 2 * cap && oldest) vk_wait_submit(oldest);
    }
    else if (f->recording && plat_ns() - g_sub_ns >= SUBMIT_EVERY_NS)
        submit(NULL, NULL, 1, VK_SUB_PERIODIC);
}
void vk_frame_check(void) {
    u64 ended = vk_submit_number(); /* the earlier draws' and dispatches' last, before any submit here */
    frame_check();
    vk_uma_settle(ended); /* on every way out: what they looked up is read by submits up to ended */
}
/* the bytes of ring memory: the chunks and big buffers the frame slots hold and the pool keeps */
u64 vk_ring_bytes(void) {
    u64 n = 0;
    for (u32 i = 0; i < g_cmd.nchunk; i++) n += g_cmd.chunk[i].size;
    return n;
}
u64 vk_ring_made(void) { return g_cmd.made; }

u64 vk_submit_number(void) { return g_gval + 1; }
u32 vk_cb_begun(void) { return g_cb_begun; }
u32 vk_frame_recycles(void) { return g_cmd.recycles; }
u64 vk_submits(int why) { return g_cmd.submits[why]; }
u64 vk_last_submit(void) { return g_gval; }
VkSemaphore vk_gfx_timeline(void) { return g_gsem; }
void vk_submit_progress(u64* submitted, u64* done) {
    *submitted = __atomic_load_n(&g_gval, __ATOMIC_RELAXED);
    *done = 0;
    if (g_gsem) VK_OK(vkGetSemaphoreCounterValue(g_vk.dev, g_gsem, done));
}
void vk_wait_submit(u64 value) { timeline_wait(value, VK_WAIT_SUBMIT); }
void vk_land_through(u64 value) { land_through(value < g_gval ? value : g_gval); }
u64 vk_frame_id(void) { return g_frame_id; }
void vk_frame_hold(u64 frame_id) {
    if (frame_id + 1 != g_frame_id) return; /* this frame's own rings, or an older frame's, landed when reused */
    g_fr[(g_cur + VK_FRAMES - 1) % VK_FRAMES].hold = vk_submit_number();
}
u32 vk_frame_slot(void) { return g_cur; }

static u32 g_desc_pools; /* the pools that exist: the GPU, recording and compute threads make them */
VkDescriptorPool vk_desc_pool_new(u32 array_sets) {
    /* a thousand sets of the stage layout (gpu/vk/pipeline.c): a frame chains more pools when it needs them */
    static u32 said[3]; /* the line of each kind of pool once: compute's, a frame's, a frame's for more arrays */
    VkDescriptorPoolSize sz[VK_POOL_TYPES];
    VkDescriptorPoolCreateInfo ci = {0};
    vk_pool_sizes(sz, 1024, array_sets);
    if (!__atomic_exchange_n(&said[!array_sets ? 0 : array_sets == VK_POOL_ARRAY_SETS ? 1 : 2], 1, __ATOMIC_RELAXED)) {
        char arrays[96]; /* the arrays the sets hold, which a device without them leaves out (vk_pool_sizes) */
        snx_vk_pool_arrays_text(arrays, sizeof arrays, array_sets, vk_texture_array_elements());
        LOG("vulkan: descriptor pools of 1024 sets, %s: %u uniform and %u storage buffers, %u combined image samplers, "
            "%u storage images, %u texel buffers, %u sampled images\n",
            arrays, sz[0].descriptorCount, sz[1].descriptorCount, sz[2].descriptorCount, sz[3].descriptorCount,
            sz[4].descriptorCount, sz[5].descriptorCount);
    }
    ci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    ci.maxSets = 1024;
    ci.poolSizeCount = VK_POOL_TYPES;
    ci.pPoolSizes = sz;
    VkDescriptorPool p = VK_NULL_HANDLE;
    if (VK_OK(vkCreateDescriptorPool(g_vk.dev, &ci, NULL, &p))) __atomic_add_fetch(&g_desc_pools, 1, __ATOMIC_RELAXED);
    return p;
}
u32 vk_desc_pools(void) { return __atomic_load_n(&g_desc_pools, __ATOMIC_RELAXED); }
/* Each window of VK_DESC_POOL_USES uses of a slot, the pools past the most one of them took are destroyed (one stays):
   the slot's work is done when it is reset, so no set of them is in use. A pool's memory is made whole when it is
   (Qualcomm's: 98 MiB a frame's pool), and a load's burst of sets left pools its steady frames never reach */
void vk_desc_pools_keep(VkDescriptorPool* pools, int* npool, int* peak, u32* uses, int took) {
    int keep, n0 = *npool;
    if (took > *peak) *peak = took;
    if (++*uses < VK_DESC_POOL_USES) return;
    keep = *peak > 1 ? *peak : 1;
    while (*npool > keep) {
        vkDestroyDescriptorPool(g_vk.dev, pools[--*npool], NULL);
        pools[*npool] = VK_NULL_HANDLE;
        __atomic_sub_fetch(&g_desc_pools, 1, __ATOMIC_RELAXED);
    }
    if (*npool < n0)
        LOG_LIMIT(20,
                  "vulkan: %d of a frame slot's %d descriptor pools went, none of its last %d uses reaching them: %u "
                  "pools left in all\n",
                  n0 - *npool, n0, VK_DESC_POOL_USES, vk_desc_pools());
    *uses = 0;
    *peak = 0;
}
static u64 g_desc_array_sets; /* sets of the arrays layouts taken from the frames' pools, for the statistics */
void vk_desc_taken(VkDescriptorSetLayout layout, u32 n) {
    if (n && vk_dslayout_arrays(layout)) __atomic_add_fetch(&g_desc_array_sets, n, __ATOMIC_RELAXED);
}
u64 vk_desc_array_sets(void) { return __atomic_load_n(&g_desc_array_sets, __ATOMIC_RELAXED); }
/* count sets of layout from the frame's pools into sets: count, or 1 when a pool can't hold that many, 0 on failure */
static u32 desc_alloc_n(Frame* f, VkDescriptorSetLayout layout, VkDescriptorSet* sets, u32 count) {
    VkDescriptorSetLayout layouts[SPARE_SETS];
    for (u32 i = 0; i < count; i++) layouts[i] = layout;
    for (int tries = 0; tries < 3; tries++) {
        if (tries) f->ipool++;
        if (tries == 2) count = 1; /* a fresh pool short of a whole batch */
        if (f->ipool == f->npool) {
            if (f->npool == (int)(sizeof f->pool_d / sizeof f->pool_d[0])) {
                LOG_LIMIT(1, "vulkan: a frame used all its descriptor pools (draws skipped)\n");
                return 0;
            }
            VkDescriptorPool p = vk_desc_pool_new(VK_POOL_ARRAY_SETS_FOR(layout));
            if (!p) return 0;
            f->pool_d[f->npool++] = p;
        }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                          .descriptorPool = f->pool_d[f->ipool],
                                          .descriptorSetCount = count,
                                          .pSetLayouts = layouts};
        VkResult r = vkAllocateDescriptorSets(g_vk.dev, &ai, sets);
        if (r == VK_SUCCESS) {
            vk_desc_taken(layout, count);
            if (f->ipool + 1 > f->took) f->took = f->ipool + 1;
            return count;
        }
        if (r != VK_ERROR_OUT_OF_POOL_MEMORY && r != VK_ERROR_FRAGMENTED_POOL) {
            VK_OK(r);
            return 0;
        }
    }
    return 0;
}
VkDescriptorSet vk_desc_alloc(VkDescriptorSetLayout layout) {
    vk_cmd(); /* the pools are reset when the frame starts */
    Frame* f = &g_fr[g_cur];
    int i = 0;
    while (i < SPARE_LAYOUTS && f->spare[i].layout && f->spare[i].layout != layout) i++;
    if (i == SPARE_LAYOUTS) { /* more layouts than kept: one at a time */
        VkDescriptorSet set;
        return desc_alloc_n(f, layout, &set, 1) ? set : VK_NULL_HANDLE;
    }
    f->spare[i].layout = layout;
    if (!f->spare[i].n) f->spare[i].n = desc_alloc_n(f, layout, f->spare[i].set, SPARE_SETS);
    return f->spare[i].n ? f->spare[i].set[--f->spare[i].n] : VK_NULL_HANDLE;
}

/* The slot whose list a release goes on, freed when the slot is recycled. A slot in use takes the release as it is:
   every command recorded before it is in the command buffer the slot records, or in a submission no newer than the
   slot's last, which its recycling waits for. Beginning a command buffer for it there gave the landing of a present's
   submission, whose results' owners are released, one in the slot the present was leaving (vk_frame_next). A fresh
   slot still holds an older frame's work, and the release starts it first (vk_cmd): that waits for the older work
   and frees its list, so what is pushed now waits for the frame recorded from now on, which is submitted after every
   frame that can still use it. Inside a landing a fresh slot does not start (vk_cmd), and the release goes on the
   list of the slot that made the newest submission: nothing is recorded that is not submitted, and that slot's
   recycling waits for all of it */
static Frame* dead_slot(void) {
    Frame* f = &g_fr[g_cur];
    if (!f->fresh || f->recording) return f;
    if (!g_landing) {
        vk_cmd();
        return f;
    }
    for (int i = 0; i < VK_FRAMES; i++)
        if (g_fr[i].last > f->last) f = &g_fr[i];
    return f;
}
void vk_defer_image(VkImage img, DevMem a, VkImageView v1, VkImageView v2) {
    dead_push(dead_slot(), (Dead){.img = img, .mem = a, .v1 = v1, .v2 = v2});
}
void vk_defer_buffer(VkBuffer b, DevMem a) { dead_push(dead_slot(), (Dead){.buf = b, .mem = a}); }
/* on the list its release goes on, so they leave the count when the release is performed: counted on a fresh slot
   before the release that started it, they left at that slot's recycling */
void vk_defer_bytes(u64 bytes) {
    if (!bytes) return;
    dead_slot()->dead_bytes += bytes;
    g_dead_bytes += bytes;
}
u64 vk_deferred_bytes(void) { return g_dead_bytes; }
void vk_defer_buffer_view(VkBufferView v) { dead_push(dead_slot(), (Dead){.view = v}); }
void vk_defer_call(void (*fn)(void*), void* arg) { dead_push(dead_slot(), (Dead){.fn = fn, .arg = arg}); }

void vk_barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkPipelineStageFlags2 src_stage,
                VkAccessFlags2 src_access, VkImageLayout from, VkPipelineStageFlags2 dst_stage,
                VkAccessFlags2 dst_access, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src_access,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst_access,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = img,
                               .subresourceRange = {aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}

/* one buffer the GPU copies into for the host to read: vk_readback_buffer before recording the copy, vk_finish,
   then vk_readback_ready before reading the pointer */
static VkBuffer g_rb;
static DevMem g_rb_mem;
static u8* g_rb_ptr;
static u64 g_rb_cap;
static u32 g_rb_small; /* readbacks in a row of an eighth of it or less */
static u32 g_rb_in;    /* where its memory is: an XFER_ index */
u8* vk_readback_buffer(u64 size, VkBuffer* buf) {
    /* it grows to the biggest readback, and shrinks back after many small ones */
    if (size > g_vk.alloc_cap) return NULL; /* read back in parts (memory.c) */
    g_rb_small = g_rb_cap > STAGE_MIN && size <= g_rb_cap / 8 ? g_rb_small + 1 : 0;
    if (size > g_rb_cap || g_rb_small >= 64) {
        if (g_rb) vk_defer_buffer(g_rb, g_rb_mem);
        u64 cap = size > g_rb_cap ? (g_rb_cap ? g_rb_cap : STAGE_MIN) : STAGE_MIN;
        g_rb_small = 0;
        while (cap < size) cap *= 2;
        if (cap > g_vk.alloc_cap) cap = g_vk.alloc_cap;
        VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                  .size = cap,
                                  /* storage: render targets are swizzled into it (vk/blocklinear.c) */
                                  .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                  .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
        DevMemWant aci = {.at = DM_CACHED, .mapped = 1};
        DevMemInfo info;
        if (!VK_OK(vk_buffer_create(&bci, &aci, &g_rb, &g_rb_mem, &info))) {
            g_rb = VK_NULL_HANDLE;
            g_rb_cap = 0;
            return NULL;
        }
        vk_mem_track(g_rb_mem, VK_OWN_READBACK);
        g_rb_ptr = info.ptr;
        g_rb_cap = cap;
        g_rb_in = xfer_where(&info);
    }
    g_xfer[XRING_READBACK][g_rb_in] += size;
    *buf = g_rb;
    return g_rb_ptr;
}
void vk_readback_ready(void) { vk_devmem_invalidate(g_rb_mem, 0, VK_WHOLE_SIZE); }

void vk_wait_foreground(void) {
#ifdef SNX_IOS
    int said = 0;
    while (!backend_window_shown() && !g_quit) {
        if (!said++) LOG("vulkan: the app is in the background, GPU work waits until it is back\n");
        plat_sleep_ns(20000000);
    }
    if (said) LOG("vulkan: GPU work goes on\n");
#endif
}
