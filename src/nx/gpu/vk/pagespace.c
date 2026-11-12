/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The page cache. The buffer cache of buffer.c keeps guest memory by the range a draw asked for. Two ranges that
   overlap each hold the bytes, and a range without a buffer of its own is copied to the ring again at every
   submission, changed or not: one game makes 24 submissions a present, and copied 79000 unchanged ranges in 600
   presents that way.

   Here the bytes are kept by guest page. The guest's address space is cut in regions of 1 GiB, each a sparse buffer
   made when a range in it is first asked for, and a binding is that buffer at the range's guest address in the
   region: ranges that overlap bind the same bytes, and a range binds without a copy while its pages hold still.
   Memory is bound to a region a granule of 64 KiB at a time, where a range was read. A granule of 2 MiB took more
   memory than a game's whole pools in one game (308 MB against 174), where 64 KiB takes close to what the ranges
   cover.

   One buffer over the whole address space, as the GPU mirror has (mirror.c), does not fit beside the mirror: the
   device this was written on has 1 TiB of sparse address space, the guest's space is 512 GiB, and the mirror takes
   576. A game reads from a few tens of regions.

   A page holds the guest's bytes as of the write tracking's clock in its stamp (vk_ww_now, taken before the bytes are
   read). A range asked for takes the pages the tracking says were written since, and the pages a GPU-side write
   touched (gpu_mem_dirty_since), through the ring and a copy in the submission's upload commands, which run before
   its draws. So a page that a draw of this submission was already given cannot take new bytes: the draw would read
   them too. Such a range is refused, and buffer.c copies it to the ring as it did before.

   A granule's memory is bound in the batch of sparse binds that goes to the queue ahead of the next submission
   (memory.c), so nothing waits for a fence. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/pagespace.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"

#define PG_REGION_SHIFT 30
#define PG_REGION       (1ull << PG_REGION_SHIFT)
#define PG_SHIFT        16
#define PG_SIZE         (1ull << PG_SHIFT)
#define PG_PAGES        (PG_SIZE >> 12)         /* the 4 KiB pages of a granule */
#define PG_GRANULES     (PG_REGION >> PG_SHIFT) /* the granules of a region */
#define PG_RUNS         64                      /* the copies recorded at once */
/* the longest range it serves. A longer one has a buffer of its own in buffer.c, which takes its written pages
   alone already (patch_own): the ranges copied again and again are the short ones */
#define PG_LEN_MAX      (1ull << 20)

typedef struct {
    VmaAllocation mem;
    u64 given;          /* the submission a range in it was last given out in */
    u64 gw[PG_PAGES];   /* gpu_mem_generation when a page's bytes were taken */
    u32 seen[PG_PAGES]; /* the write tracking's clock when a page's bytes were taken, 0: it has none */
    u32 used;           /* the present a range in it was last given out at */
    u32 ck_epoch;       /* the stream's epoch ck_mask is of */
    u16 ck_mask;        /* the pages the tracking found unwritten in ck_epoch: not asked again in it */
} PgGran;
typedef struct {
    VkBuffer buf;
    PgGran* gran[PG_GRANULES];
} PgRegion;

enum { PG_NO_LONG, PG_NO_RUN, PG_NO_REGION, PG_NO_ALIGN, PG_NO_TRACK, PG_NO_OWNED, PG_NO_HOT, PG_NO_MEMORY, PG_NO_N };
static struct {
    int on; /* -1 until vk_pages_on has decided */
    VkMemoryRequirements req;
    PgRegion** region; /* by region of guest memory, NULL where no range was asked for */
    u32 regions, nregion, region_max;
    u32 bound; /* granules with memory */
    u32 sweep;
    struct {
        u64 served, served_bytes, uploads, upload_bytes, refused[PG_NO_N], let_go;
    } stats;
} g_pages = {.on = -1};

int vk_pages_on(void) {
    if (g_pages.on >= 0) return g_pages.on;
    g_pages.on = 0;
    if (!g_settings.buffer_pages) return 0;
    u64 space = g_vk.props.limits.sparseAddressSpaceSize;
    const char* why = vk_uma()                                      ? "memory shared with the CPU: read in place"
                      : !g_vk.sparse                                ? "no sparse binding"
                      : !g_vk.feat.sparseResidencyBuffer            ? "no sparse residency of buffers"
                      : g_vk.max_buffer_size < PG_REGION            ? "buffers smaller than a region"
                      : space / 8 < PG_REGION                       ? "too small an address space"
                      : vk_written_since(g_mem + AS_BASE, 1, 0) < 0 ? "no write tracking"
                                                                    : NULL;
    if (why) {
        LOG("vulkan: the buffer cache keeps guest memory by range, not by page (%s)\n", why);
        return 0;
    }
    /* an eighth of the sparse address space at most: the mirror and the sparse textures have the rest */
    g_pages.region_max = (u32)(space / 8 >> PG_REGION_SHIFT);
    g_pages.regions = (u32)((AS_SIZE + PG_REGION - 1) >> PG_REGION_SHIFT);
    g_pages.region = calloc(g_pages.regions, sizeof *g_pages.region);
    if (!g_pages.region) return 0;
    LOG("vulkan: the buffer cache keeps guest memory by page: sparse buffers of %llu GiB, %u at most, with memory in "
        "granules of %llu KiB (vk_buffer_pages=1)\n",
        (unsigned long long)(PG_REGION >> 30), g_pages.region_max, (unsigned long long)(PG_SIZE >> 10));
    return g_pages.on = 1;
}

static int refuse(u32 why) {
    g_pages.stats.refused[why]++;
    return 0;
}

/* the region's buffer, made at its first range: NULL when the device makes no more */
static PgRegion* region_get(u64 rg) {
    if (g_pages.region[rg]) return g_pages.region[rg];
    if (g_pages.nregion == g_pages.region_max) return NULL;
    PgRegion* r = calloc(1, sizeof *r);
    if (!r) return NULL;
    u32 fam[2] = {g_vk.gfx_family, g_vk.compute_family};
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
        .size = PG_REGION,
        /* whatever a draw or a dispatch binds guest memory as, and a copy's source and destination (buffer.c's
           make_own_); its address too where descriptors are written into buffers (descriptors.c vk_buffer_create) */
        .usage =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            (g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                                                             : 0),
        .sharingMode = fam[0] != fam[1] ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = fam[0] != fam[1] ? 2 : 0,
        .pQueueFamilyIndices = fam};
    if (!VK_OK(vkCreateBuffer(g_vk.dev, &bci, NULL, &r->buf))) {
        free(r);
        return NULL;
    }
    if (!g_pages.nregion) { /* every region's buffer is made alike: the first one's requirements are all of theirs */
        vkGetBufferMemoryRequirements(g_vk.dev, r->buf, &g_pages.req);
        if (PG_SIZE % g_pages.req.alignment) {
            LOG("vulkan: the buffer cache keeps guest memory by range, not by page (sparse blocks of %llu bytes)\n",
                (unsigned long long)g_pages.req.alignment);
            vkDestroyBuffer(g_vk.dev, r->buf, NULL);
            free(r);
            g_pages.on = 0;
            return NULL;
        }
    }
    g_pages.nregion++;
    return g_pages.region[rg] = r;
}

/* memory for granule g of r, bound in the batch ahead of the next submission: NULL when there is none */
static PgGran* gran_get(PgRegion* r, u64 g) {
    if (r->gran[g]) return r->gran[g];
    PgGran* q = calloc(1, sizeof *q);
    if (!q) return NULL;
    VkMemoryRequirements req = g_pages.req;
    req.size = PG_SIZE;
    VmaAllocationCreateInfo aci = {.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VmaAllocationInfo info;
    if (vmaAllocateMemory(g_vk.vma, &req, &aci, &q->mem, &info) != VK_SUCCESS) {
        free(q);
        return NULL;
    }
    VkSparseMemoryBind b = {
        .resourceOffset = g << PG_SHIFT, .size = PG_SIZE, .memory = info.deviceMemory, .memoryOffset = info.offset};
    if (!vk_buffer_sparse_bind(r->buf, &b, VK_NULL_HANDLE, 0, 0)) {
        vmaFreeMemory(g_vk.vma, q->mem);
        free(q);
        return NULL;
    }
    vk_mem_track(q->mem, VK_OWN_BUFFER);
    g_pages.bound++;
    return r->gran[g] = q;
}

/* granule g of r gives its memory back, once the work that may still read it is done */
static void gran_drop(PgRegion* r, u64 g) {
    PgGran* q = r->gran[g];
    VkSparseMemoryBind b = {.resourceOffset = g << PG_SHIFT, .size = PG_SIZE};
    if (!vk_buffer_sparse_bind(r->buf, &b, q->mem, PG_SIZE, 1)) return; /* no room for the bind now: it stays */
    r->gran[g] = NULL;
    g_pages.bound--;
    g_pages.stats.let_go++;
    free(q);
}

/* the copies recorded so far into r's buffer, in the submission's upload commands: after the work submitted before,
   which may read the old bytes, and before this submission's draws */
static void runs_flush(const PgRegion* r, VkBuffer sb, VkBufferCopy* run, u32* n) {
    if (!*n) return;
    VkCommandBuffer cb = vk_cmd_upload(0);
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    rec_CmdCopyBuffer(cb, sb, r->buf, *n, run);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    rec_CmdPipelineBarrier2(cb, &d);
    *n = 0;
}

int vk_pages_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    u64 cpu = 0;
    if (g_pages.on <= 0 || !len) return 0;
    if (len > PG_LEN_MAX) return refuse(PG_NO_LONG);
    if (gmmu_run(gva, len, &cpu) != len) return refuse(PG_NO_RUN);
    u64 rg = cpu >> PG_REGION_SHIFT, at = cpu & (PG_REGION - 1);
    if (cpu + len > AS_SIZE || (cpu + len - 1) >> PG_REGION_SHIFT != rg) return refuse(PG_NO_REGION);
    if (align > 1 && at % align) return refuse(PG_NO_ALIGN);
    /* each before any byte is read: a write after them shows at the next check */
    u32 now = vk_ww_now(), epoch = vk_ww_epoch();
    u64 gnow = gpu_mem_generation(), sub = vk_submit_number();
    if (!now) return refuse(PG_NO_TRACK); /* a stamp of 0 is a page with no bytes */
    PgRegion* r = region_get(rg);
    if (!r) return refuse(PG_NO_MEMORY);
    u64 p0 = at >> 12, p1 = (at + len - 1) >> 12;    /* the range's pages in the region */
    u64 base = (rg << PG_REGION_SHIFT) + (p0 << 12); /* the guest address of its first page, at or before cpu */
    u64 bgva = gva - (cpu - base);                   /* and that page's GPU address: the pages of a run go on alike */
    /* a GPU-side write on any of its pages since the oldest of them was taken: one question for the range, and one
       a page only when the answer is yes */
    u64 oldest = ~0ull;
    for (u64 p = p0; p <= p1; p++) {
        const PgGran* q = r->gran[p >> (PG_SHIFT - 12)];
        u32 k = (u32)(p & (PG_PAGES - 1));
        if (q && q->seen[k] && q->gw[k] < oldest) oldest = q->gw[k];
    }
    int gpu_wrote = oldest != ~0ull && gpu_mem_dirty_since(oldest, bgva, (p1 - p0 + 1) << 12);
    /* what must be copied, found before anything changes: a range that is refused leaves its pages as they were.
       take[i]: page p0 + i takes the guest's bytes; 2 when it had bytes before */
    u8 take[PG_LEN_MAX / 4096 + 1];
    u64 stale = 0;
    for (u64 p = p0; p <= p1; p++) {
        const PgGran* q = r->gran[p >> (PG_SHIFT - 12)];
        u32 k = (u32)(p & (PG_PAGES - 1));
        u64 pcpu = base + ((p - p0) << 12);
        int old = 0;
        if (!q || !q->seen[k])
            old = 1;
        else if (gpu_wrote && gpu_mem_dirty_since(q->gw[k], bgva + ((p - p0) << 12), 4096))
            old = 2;
        else if (q->ck_epoch != epoch || !(q->ck_mask >> k & 1)) {
            int w = vk_written_since(GP(pcpu), 4096, q->seen[k]);
            if (w < 0) return refuse(PG_NO_TRACK);
            old = w ? 2 : 0;
        }
        take[p - p0] = (u8)old;
        if (!old) continue;
        /* a page a draw of this submission was given: the upload commands run before that draw */
        if (old == 2 && q->given == sub) {
            refuse(PG_NO_HOT);
            return -1;
        }
        /* a page with bytes only the GPU holds on it is trapped: not read here */
        if (own_any() && own_trapped(pcpu, 4096)) return refuse(PG_NO_OWNED);
        stale++;
    }
    VkBuffer sb = VK_NULL_HANDLE;
    u64 so = 0, done = 0;
    u8* host = NULL;
    if (stale && !(host = vk_stage_upload(stale << 12, vk_stage_align(), &sb, &so))) return refuse(PG_NO_MEMORY);
    for (u64 g = p0 >> (PG_SHIFT - 12); g <= p1 >> (PG_SHIFT - 12); g++)
        if (!gran_get(r, g)) return refuse(PG_NO_MEMORY); /* the ring's bytes are left unused */
    VkBufferCopy run[PG_RUNS];
    u32 nrun = 0;
    for (u64 p = p0; p <= p1; p++) {
        PgGran* q = r->gran[p >> (PG_SHIFT - 12)];
        u32 k = (u32)(p & (PG_PAGES - 1));
        if (q->ck_epoch != epoch) {
            q->ck_epoch = epoch;
            q->ck_mask = 0;
        }
        q->used = g_vk_present;
        q->given = sub;
        q->ck_mask |= (u16)(1u << k); /* taken now, or found unwritten: not asked again in this epoch */
        if (!take[p - p0]) continue;
        memcpy(host + (done << 12), GP(base + ((p - p0) << 12)), 4096);
        q->seen[k] = now;
        q->gw[k] = gnow;
        if (nrun && run[nrun - 1].dstOffset + run[nrun - 1].size == p << 12)
            run[nrun - 1].size += 4096; /* touching the last: one copy */
        else {
            if (nrun == PG_RUNS) runs_flush(r, sb, run, &nrun);
            run[nrun++] = (VkBufferCopy){so + (done << 12), p << 12, 4096};
        }
        done++;
    }
    runs_flush(r, sb, run, &nrun);
    g_pages.stats.served++;
    g_pages.stats.served_bytes += len;
    if (done) {
        g_pages.stats.uploads++;
        g_pages.stats.upload_bytes += done << 12;
    }
    *buf = r->buf;
    *off = at;
    return 1;
}

/* a sixteenth of the regions every fourth present: a granule nothing read for twenty seconds, and in 120 presents,
   gives its memory back, as a buffer of the cache's own does (buffer.c buf_frame) */
void vk_pages_frame(void) {
    if (g_pages.on <= 0 || g_vk_present % 4 || !g_pages.bound) return;
    u32 before = vk_present_before(20000000000ull, 120);
    if (!before) return;
    u32 part = g_pages.sweep++ & 15;
    for (u32 rg = part; rg < g_pages.regions; rg += 16) {
        PgRegion* r = g_pages.region[rg];
        if (!r) continue;
        for (u64 g = 0; g < PG_GRANULES; g++)
            if (r->gran[g] && (i32)(r->gran[g]->used - before) < 0) gran_drop(r, g);
    }
}

void vk_pages_forget_all(void) {
    if (g_pages.on <= 0) return;
    for (u32 rg = 0; rg < g_pages.regions; rg++) {
        PgRegion* r = g_pages.region[rg];
        if (!r) continue;
        for (u64 g = 0; g < PG_GRANULES; g++)
            if (r->gran[g]) memset(r->gran[g]->seen, 0, sizeof r->gran[g]->seen); /* no bytes: taken again */
    }
}

void vk_pages_log(void) {
    if (g_pages.on <= 0) return;
    const u64* no = g_pages.stats.refused;
    LOG("vulkan: page cache over 600 presents: %llu ranges bound (%.1f MB), %llu of them with pages copied first "
        "(%.1f MB); %u granules hold %.1f MB in %u regions, %llu let go; refused: %llu too long, %llu not one run, %llu across a "
        "region, %llu for alignment, %llu with no tracking, %llu on owned pages, %llu changed after a draw read "
        "them, %llu for memory\n",
        (unsigned long long)g_pages.stats.served, (double)g_pages.stats.served_bytes / 1048576.0,
        (unsigned long long)g_pages.stats.uploads, (double)g_pages.stats.upload_bytes / 1048576.0, g_pages.bound,
        (double)g_pages.bound * (double)PG_SIZE / 1048576.0, g_pages.nregion, (unsigned long long)g_pages.stats.let_go,
        (unsigned long long)no[PG_NO_LONG], (unsigned long long)no[PG_NO_RUN], (unsigned long long)no[PG_NO_REGION],
        (unsigned long long)no[PG_NO_ALIGN], (unsigned long long)no[PG_NO_TRACK], (unsigned long long)no[PG_NO_OWNED],
        (unsigned long long)no[PG_NO_HOT], (unsigned long long)no[PG_NO_MEMORY]);
    memset(&g_pages.stats, 0, sizeof g_pages.stats);
}
