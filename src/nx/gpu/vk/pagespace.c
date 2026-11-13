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

   A long range (a window of a bare storage buffer, buffer.c win_get) has thousands of pages, and is asked for at every
   draw that binds it: the tracking is asked once for all of it, and its pages one at a time only where that says
   something was written. A page of a long range that changed after a draw of this submission read it is copied in
   the submission's own commands, after that draw, as a buffer of the cache's own takes its written pages
   (patch_own): the upload commands would give the draw the new bytes.

   A draw that writes guest memory writes it here too (vk_pages_write, step 6): the pages its range is on are brought
   up to date, the draw writes the region's buffer in place, and the range is published as the GPU's (owned.c), each
   granule's part under an owner of that granule, so the CPU's first touch of a page of it lands it and nothing is
   copied, read back or compared before. Only the range's own bytes are published, as every other producer's are: the
   runtime's reads of guest memory land an owner only for bytes it holds (gpu.c copy_runs), and a game keeps the
   constant buffers its draws read beside the buffers they write, on the same pages. The ownership table says which
   pages have such bytes on them (own_lookup). Such a page is trapped, so the CPU cannot change it: the region holds
   the truth of all of it and it takes nothing from guest memory. It goes back to being a copy when the table lets
   go of it, with the stamp its publish left on it (gpu_mem_written_cpu) telling it to take memory's bytes again.

   A granule's memory is bound in the batch of sparse binds that goes to the queue ahead of the next submission
   (memory.c), so nothing waits for a fence.

   A device with no sparse buffers has slabs in the regions' place (the phones and Macs of the plan, and any device
   with vk_buffer_slabs=1): a slab is a buffer with all its memory over a run of a region's granules, made for the
   range that first asked for them. A range binds one buffer, so one whose granules are in no one slab gets a slab
   over them and over every slab that held some of them: their bytes are copied into it on the GPU, and they go once
   the work that bound them is done. A page's state is kept by granule either way, so everything above holds but
   for the draws' writes in place, which a slab does not take: the owner of a granule is of its buffer, which a join
   replaces. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/ledger.h"
#include "nx/gpu/vk/descriptors.h"
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
#define PG_FULL         0xFFFFu                 /* a granule's pages, a bit each */
#define PG_RUNS         64                      /* the copies recorded at once */
#define PG_STAGE        1024                    /* the pages copied through one piece of the ring */
#define PG_LEN_MAX      (64ull << 20)           /* the longest range it serves: a window (buffer.c WIN_SIZE) */
/* past this length, a page that changed after a draw of this submission read it is copied after that draw, which
   ends the rendering pass. A range up to it is refused, and buffer.c copies it to the ring, which ends nothing */
#define PG_PATCH_MIN    (1ull << 20)
#define PG_WRITE_MAX    (1ull << 20) /* the longest range a draw writes here */
#define PG_ORDER        256          /* the ranges read and written whose order a submission keeps */
#define PG_LAND         4            /* the idle granules landed at one sweep */
#define PG_STAMPS       256          /* the ranges written in place that a submission stamps at its end */
/* the presents a granule's pages must go without a write of the CPU before a draw's result is kept on them: a page
   the game writes every frame would trap and land every frame, a wait for the GPU each time */
#define PG_QUIET        120
/* the most granules of one slab: a range and the slabs a join would bring past it are refused, and buffer.c copies the
   range (128 MiB, two of the longest ranges) */
#define PG_SLAB_MAX     2048u
/* the granules a slab made over others grows by at each end, where no slab is: ranges come beside the ones before
   them, and each one more was a join more (519 in 600 presents of one game's race, a buffer made and a rendering
   pass ended each). 1 MiB in all at most, as Ryujinx caps a buffer's growth */
#define PG_SLAB_GROW    8u
/* what a page of a range asked for does: it holds memory's bytes, takes them (it had none, or older ones), or is the
   cache's own. PG_CPU and PG_GPU mark one that holds bytes for a look of its own: something on the range was written
   since the oldest of its pages was taken */
enum { PG_CUR, PG_NEW, PG_OLD, PG_MINE, PG_OLD_CPU /* PG_OLD, by a write of the CPU */, PG_CPU = 0x10, PG_GPU = 0x20 };

typedef struct {
    DevMem mem;  /* a sparse region's granule: its memory */
    u32 slab;    /* with slabs: the region's slab it is in, plus one */
    /* the owner its pages are published under once a draw wrote them (vk_pages_write), with a reference: NULL until
       then. A landing waits for the submission that last wrote this granule, not the region */
    VkOwnRes* own;
    u64 given;          /* the submission a range in it was last given out in */
    u64 gw[PG_PAGES];   /* gpu_mem_generation when a page's bytes were taken */
    u64 gw_min;         /* the oldest of gw, and of seen below, while every page has bytes (full) */
    u32 seen_min;
    u32 seen[PG_PAGES]; /* the write tracking's clock when a page's bytes were taken, 0: it has none */
    u32 used;           /* the present a range in it was last given out at */
    u32 cpu_wrote;      /* the present a page of it last took bytes the CPU had written since (0: never) */
    u32 ck_epoch;       /* the stream's epoch ck_mask is of */
    u16 ck_mask;        /* the pages the tracking found unwritten in ck_epoch: not asked again in it */
    u16 full;           /* the pages with bytes */
} PgGran;
/* a slab: a buffer with its memory over the granules [g0, g0 + n) of its region */
typedef struct {
    VkBuffer buf; /* VK_NULL_HANDLE: an entry no slab is in */
    DevMem mem;
    u32 g0, n;
    /* the submission in which it took other slabs' bytes (a join). That copy is in the submission's own commands,
       after the draws that read those slabs, so what its pages take in that submission is copied there too: the
       upload commands run before all of them */
    u64 late;
} PgSlab;
typedef struct {
    VkBuffer buf; /* the sparse buffer, or none with slabs */
    PgSlab* slab;
    u32 nslab;
    PgGran* gran[PG_GRANULES];
} PgRegion;

enum { PG_NO_LONG, PG_NO_RUN, PG_NO_REGION, PG_NO_ALIGN, PG_NO_TRACK, PG_NO_OWNED, PG_NO_HOT, PG_NO_MEMORY, PG_NO_N };
/* why a range to be written in place was refused, beside those */
enum { PG_WNO_ASK, PG_WNO_CPU, PG_WNO_OWNER, PG_WNO_N };
/* a range of a region's bytes a draw or a dispatch read or wrote in place, by guest address, and which one (the count
   of work recorded when it was bound) */
typedef struct {
    u64 lo, hi, work;
} PgUse;
static struct {
    int on;    /* -1 until vk_pages_on has decided */
    int slabs; /* the regions' bytes are in slabs, not in sparse buffers */
    u32 nslabs;
    VkMemoryRequirements req;
    PgRegion** region; /* by region of guest memory, NULL where no range was asked for */
    u32 regions, nregion, region_max;
    u32 bound;  /* granules with memory */
    u32 owners; /* granules with an owner: 0 until a draw first writes here, and nothing is ordered until then */
    u64 writes; /* ranges bound for writing in place so far (vk_pages_written) */
    /* The ranges written in place in the submission being recorded, by GPU address. They are stamped as written by
       the GPU when it goes to the queue (vk_pages_submit), which is when a result copied to the ring was stamped at
       the earliest: stamped at each bind, a window over them was looked at again page by page at its next draw */
    struct {
        u64 gva;
        u32 len;
    } stamp[PG_STAMPS];
    u32 nstamp;
    u32 sweep;
    /* The order of this submission's work on the regions' bytes. A draw that writes them in place and another that
       reads them share one buffer, where the copies of buffer.c gave each its own: a read of bytes written since the
       last barrier from all work gets one first, and a write of bytes read or written since gets one when the front
       end orders its draws itself (Maxwell) or asked for a dependency (NVN's barrier). Kept by range, not by granule:
       two buffers of a game on one page are not each other's. all: more ranges than rd or wr holds, so every one */
    struct {
        u64 sub;
        PgUse rd[PG_ORDER], wr[PG_ORDER];
        u32 nrd, nwr;
        int rd_all, wr_all, required;
    } order;
    struct {
        u64 served, served_bytes, uploads, upload_bytes, refused[PG_NO_N], let_go;
        u64 patched, written, written_bytes, published, write_refused[PG_WNO_N], not_trapped, barriers, landed;
        u64 again, longs, long_ns; /* bound where they were; ranges past PG_PATCH_MIN looked at, and the time that took */
        u64 joins, joined_bytes;   /* slabs made over others, and the bytes copied from those */
    } stats;
} g_pages = {.on = -1};

int vk_pages_keep(void) { return vk_pages_on() && !g_pages.slabs && g_settings.buffer_pages >= 2; }
int vk_pages_on(void) {
    if (g_pages.on >= 0) return g_pages.on;
    g_pages.on = 0;
    if (!g_settings.buffer_pages) return 0;
    u64 space = g_vk.props.limits.sparseAddressSpaceSize;
    const char* why = vk_uma()                                      ? "memory shared with the CPU: read in place"
                      : vk_written_since(g_mem + AS_BASE, 1, 0) < 0 ? "no write tracking"
                                                                    : NULL;
    if (why) {
        LOG("vulkan: the buffer cache keeps guest memory by range, not by page (%s)\n", why);
        return 0;
    }
    const char* slabs = g_settings.buffer_page_slabs         ? "vk_buffer_slabs=1"
                        : !g_vk.sparse                       ? "no sparse binding"
                        : !g_vk.feat.sparseResidencyBuffer   ? "no sparse residency of buffers"
                        : g_vk.max_buffer_size < PG_REGION   ? "buffers smaller than a region"
                        : space / 8 < PG_REGION              ? "too small an address space"
                                                             : NULL;
    g_pages.slabs = slabs != NULL;
    g_pages.regions = (u32)((AS_SIZE + PG_REGION - 1) >> PG_REGION_SHIFT);
    /* an eighth of the sparse address space at most: the mirror and the sparse textures have the rest. A region of
       slabs is a table alone */
    g_pages.region_max = slabs ? g_pages.regions : (u32)(space / 8 >> PG_REGION_SHIFT);
    g_pages.region = calloc(g_pages.regions, sizeof *g_pages.region);
    if (!g_pages.region) return 0;
    if (slabs)
        LOG("vulkan: the buffer cache keeps guest memory by page: in slabs, buffers over whole granules of %llu KiB "
            "with all their memory (%s; vk_buffer_pages=%d)\n",
            (unsigned long long)(PG_SIZE >> 10), slabs, g_settings.buffer_pages);
    else
        LOG("vulkan: the buffer cache keeps guest memory by page: sparse buffers of %llu GiB, %u at most, with memory "
            "in granules of %llu KiB%s (vk_buffer_pages=%d)\n",
            (unsigned long long)(PG_REGION >> 30), g_pages.region_max, (unsigned long long)(PG_SIZE >> 10),
            g_settings.buffer_pages >= 2 ? ", and draws write guest memory there" : "", g_settings.buffer_pages);
    return g_pages.on = 1;
}

static int refuse(u32 why) {
    g_pages.stats.refused[why]++;
    return 0;
}

/* a buffer of guest memory's bytes: a region's sparse one, or a slab's with memory (flags 0). fam: two words that
   live until the buffer is made */
static VkBufferCreateInfo pg_buffer_info(VkBufferCreateFlags flags, u64 size, u32* fam) {
    fam[0] = g_vk.gfx_family;
    fam[1] = g_vk.compute_family;
    return (VkBufferCreateInfo){
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .flags = flags,
        .size = size,
        /* whatever a draw or a dispatch binds guest memory as, and a copy's source and destination (buffer.c's
           make_own_); what a draw writes there, transform feedback's captures too (owned.c vk_own_res_new); its
           address too where descriptors are written into buffers (descriptors.c vk_buffer_create) */
        .usage =
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
            VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
            VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
            (g_vk.has_xfb ? VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT : 0) |
            (g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT
                                                                             : 0),
        .sharingMode = fam[0] != fam[1] ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = fam[0] != fam[1] ? 2 : 0,
        .pQueueFamilyIndices = fam};
}
/* the region's buffer, made at its first range: NULL when the device makes no more. With slabs, its table alone */
static PgRegion* region_get(u64 rg) {
    if (g_pages.region[rg]) return g_pages.region[rg];
    if (g_pages.nregion == g_pages.region_max) return NULL;
    PgRegion* r = calloc(1, sizeof *r);
    if (!r) return NULL;
    if (g_pages.slabs) {
        g_pages.nregion++;
        return g_pages.region[rg] = r;
    }
    u32 fam[2];
    VkBufferCreateInfo bci = pg_buffer_info(
        VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT, PG_REGION, fam);
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
    DevMemWant aci = {.at = DM_DEVICE};
    DevMemInfo info;
    if (vk_devmem_alloc(&req, &aci, &q->mem, &info) != VK_SUCCESS) {
        free(q);
        return NULL;
    }
    VkSparseMemoryBind b = {
        .resourceOffset = g << PG_SHIFT, .size = PG_SIZE, .memory = info.memory, .memoryOffset = info.offset};
    if (!vk_buffer_sparse_bind(r->buf, &b, 0, 0, 0)) {
        vk_devmem_free(q->mem);
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
    if (q->own) {
        vk_own_res_unref(q->own);
        g_pages.owners--;
    }
    free(q);
}

/* the copies recorded so far into r's buffer. In the submission's upload commands: after the work submitted before,
   which may read the old bytes, and before this submission's draws. Or (after) in its own commands, outside a
   rendering pass: after the draws recorded so far, which read the old bytes */
static void runs_flush(VkBuffer dst, VkBuffer sb, VkBufferCopy* run, u32* n, int after) {
    if (!*n) return;
    VkCommandBuffer cb = after ? vk_cmd() : vk_cmd_upload(0);
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
    rec_CmdCopyBuffer(cb, sb, dst, *n, run);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT;
    rec_CmdPipelineBarrier2(cb, &d);
    *n = 0;
}

/* ---------------------------------------------------------------- slabs */
/* the slab's buffer goes once the work that bound it is done, and its entry is free */
static void slab_retire(PgSlab* k) {
    vk_defer_buffer(k->buf, k->mem);
    *k = (PgSlab){0};
    g_pages.nslabs--;
}
/* A slab of r over the granules [g0, g1], and over every slab that has some of them: its index plus one, 0 when it
   would pass PG_SLAB_MAX or memory has none. The others' bytes are copied into it after the draws recorded so far,
   which bound them, and they go: every granule in it is of it from here on, those with no bytes yet too */
static u32 slab_join(PgRegion* r, u64 g0, u64 g1, u64 sub) {
    u64 lo = g0, hi = g1;
    for (u64 g = g0; g <= g1; g++) {
        const PgGran* q = r->gran[g];
        if (!q) continue;
        const PgSlab* k = &r->slab[q->slab - 1];
        if (k->g0 < lo) lo = k->g0;
        if (k->g0 + k->n - 1 > hi) hi = k->g0 + k->n - 1;
        g = k->g0 + k->n - 1; /* past it */
    }
    if (hi - lo + 1 > PG_SLAB_MAX) return 0;
    if (lo != g0 || hi != g1 || r->gran[g0]) { /* over others: room for the ranges that come beside it */
        for (u32 k = 0; k < PG_SLAB_GROW && lo && !r->gran[lo - 1] && hi - lo + 1 < PG_SLAB_MAX; k++) lo--;
        for (u32 k = 0; k < PG_SLAB_GROW && hi + 1 < PG_GRANULES && !r->gran[hi + 1] && hi - lo + 1 < PG_SLAB_MAX; k++)
            hi++;
    }
    const u64 n = hi - lo + 1;
    u32 at = 0;
    while (at < r->nslab && r->slab[at].buf) at++;
    if (at == r->nslab) {
        PgSlab* grown = realloc(r->slab, ((size_t)r->nslab + 16) * sizeof *grown);
        if (!grown) return 0;
        memset(grown + r->nslab, 0, 16 * sizeof *grown);
        r->slab = grown;
        r->nslab += 16;
    }
    /* the granules it brings in that have no state yet, before anything is made: none is left half entered */
    for (u64 g = lo; g <= hi; g++)
        if (!r->gran[g] && !(r->gran[g] = calloc(1, sizeof(PgGran)))) {
            for (u64 x = lo; x < g; x++)
                if (!r->gran[x]->slab) {
                    free(r->gran[x]);
                    r->gran[x] = NULL;
                }
            return 0;
        }
    u32 fam[2];
    VkBufferCreateInfo bci = pg_buffer_info(0, n << PG_SHIFT, fam);
    DevMemWant want = {.at = DM_DEVICE};
    PgSlab made = {.g0 = (u32)lo, .n = (u32)n};
    if (!VK_OK(vk_buffer_create(&bci, &want, &made.buf, &made.mem, NULL))) {
        for (u64 g = lo; g <= hi; g++)
            if (!r->gran[g]->slab) {
                free(r->gran[g]);
                r->gran[g] = NULL;
            }
        return 0;
    }
    vk_mem_track(made.mem, VK_OWN_BUFFER);
    VkCommandBuffer cb = VK_NULL_HANDLE;
    for (u64 g = lo; g <= hi; g++) {
        PgGran* q = r->gran[g];
        if (!q->slab) { /* new here */
            q->slab = at + 1;
            g_pages.bound++;
            continue;
        }
        PgSlab* old = &r->slab[q->slab - 1];
        const u64 last = old->g0 + old->n - 1;
        if (!cb) {
            vk_render_end(); /* copies happen outside rendering passes */
            cb = vk_cmd();
            vk_all_barrier(cb);
        }
        const VkBufferCopy all = {0, (u64)(old->g0 - lo) << PG_SHIFT, (u64)old->n << PG_SHIFT};
        rec_CmdCopyBuffer(cb, old->buf, made.buf, 1, &all);
        g_pages.stats.joined_bytes += all.size;
        for (u64 x = old->g0; x <= last; x++) r->gran[x]->slab = at + 1;
        slab_retire(old);
        g = last;
    }
    if (cb) {
        vk_all_barrier(cb);
        made.late = sub;
        g_pages.stats.joins++;
    }
    r->slab[at] = made;
    g_pages.nslabs++;
    return at + 1;
}
/* the slab that holds the granules [g0, g1] of r, made or joined when none does: its index plus one, or 0 */
static u32 slab_cover(PgRegion* r, u64 g0, u64 g1, u64 sub) {
    const PgGran* q = r->gran[g0];
    if (q) {
        const PgSlab* k = &r->slab[q->slab - 1];
        if (g1 < (u64)k->g0 + k->n) return q->slab;
    }
    return slab_join(r, g0, g1, sub);
}
/* slab at of r, which nothing read for a while, goes with its granules' state */
static void slab_drop(PgRegion* r, u32 at) {
    PgSlab* k = &r->slab[at];
    for (u64 g = k->g0; g < (u64)k->g0 + k->n; g++) {
        free(r->gran[g]);
        r->gran[g] = NULL;
    }
    g_pages.bound -= k->n;
    g_pages.stats.let_go += k->n;
    slab_retire(k);
}

/* ---------------------------------------------------------------- the order of work in place */
static void order_reset(void) {
    g_pages.order.nrd = g_pages.order.nwr = 0;
    g_pages.order.rd_all = g_pages.order.wr_all = g_pages.order.required = 0;
}
/* a new submission: what the ones before it wrote in place is ahead of all its work, with a barrier in its upload
   commands, which no rendering pass is in */
static void order_begin(void) {
    u64 sub = vk_submit_number();
    if (g_pages.order.sub == sub) return;
    g_pages.order.sub = sub;
    order_reset();
    vk_all_barrier(vk_cmd_upload(0));
}
static int order_meets(const PgUse* u, u32 n, int all, u64 lo, u64 hi, u64 work) {
    if (all) return 1;
    for (u32 i = 0; i < n; i++) /* not the work being set up: a barrier does not come between a draw and itself */
        if (u[i].lo < hi && lo < u[i].hi && u[i].work != work) return 1;
    return 0;
}
static void order_note(PgUse* u, u32* n, int* all, u64 lo, u64 hi, u64 work) {
    if (*all) return;
    if (*n && u[*n - 1].work == work && u[*n - 1].lo <= hi && lo <= u[*n - 1].hi) { /* touching the last: one */
        if (lo < u[*n - 1].lo) u[*n - 1].lo = lo;
        if (hi > u[*n - 1].hi) u[*n - 1].hi = hi;
        return;
    }
    if (*n == PG_ORDER) {
        *all = 1;
        return;
    }
    u[(*n)++] = (PgUse){lo, hi, work};
}
/* guest memory [lo, hi) is bound in its region's buffer, to read or to write in place: the barrier it needs first */
static void order_use(u64 lo, u64 hi, int write, int ordered) {
    if (!g_pages.owners) return; /* no draw has written here: every binding reads, and the copies take their own */
    order_begin();
    u64 work = g_vk_drawn + g_vk_dispatches;
    int wait = order_meets(g_pages.order.wr, g_pages.order.nwr, g_pages.order.wr_all, lo, hi, work);
    if (write) {
        wait = (ordered || g_pages.order.required) &&
               (wait || order_meets(g_pages.order.rd, g_pages.order.nrd, g_pages.order.rd_all, lo, hi, work));
    }
    if (wait) {
        vk_render_end();
        vk_all_barrier(vk_cmd());
        order_reset();
        g_pages.stats.barriers++;
    }
    if (write)
        order_note(g_pages.order.wr, &g_pages.order.nwr, &g_pages.order.wr_all, lo, hi, work);
    else
        order_note(g_pages.order.rd, &g_pages.order.nrd, &g_pages.order.rd_all, lo, hi, work);
}
void vk_pages_submit(void) {
    for (u32 i = 0; i < g_pages.nstamp; i++) gpu_mem_written_shader(g_pages.stamp[i].gva, g_pages.stamp[i].len);
    g_pages.nstamp = 0;
}
u64 vk_pages_written(void) { return g_pages.writes; }
void vk_pages_ordered(void) {
    if (g_pages.on > 0) order_reset();
}
void vk_pages_order_request(void) {
    if (g_pages.on > 0 && g_pages.order.sub == vk_submit_number() &&
        (g_pages.order.nrd | g_pages.order.nwr | (u32)g_pages.order.rd_all | (u32)g_pages.order.wr_all))
        g_pages.order.required = 1;
}

/* ---------------------------------------------------------------- a range asked for */
typedef struct {
    PgRegion* r;
    u64 rg, at;     /* the region, and the range's first byte in it */
    u64 p0, p1;     /* its pages in the region */
    u64 base, bgva; /* the guest address of its first page, and that page's GPU address */
    u64 sub, gnow;  /* the submission, and the clocks read before any byte: a write after them shows at the next ask */
    u32 now, epoch;
    u64 stale;   /* the pages that take bytes */
    u64 covered; /* the range's bytes that the cache owns already */
    int hot;   /* one of them was given out in this submission: they are copied after its draws */
    VkBuffer buf; /* what the range binds (bring): the region's buffer, or its slab's */
    u64 bias;     /* and where that buffer starts in the region: 0, or the slab's first granule's place */
    u8 take[PG_LEN_MAX / 4096 + 1]; /* PG_*, a page each */
} PgAsk;
static void cpu_note(void* user, u64 at, u64 n) {
    PgAsk* a = user;
    for (u64 p = at >> 12, e = (at + n - 1) >> 12; p <= e; p++)
        if ((a->take[p] & 15) == PG_CUR) a->take[p] |= PG_CPU;
}
static void gpu_note(void* user, u64 at, u64 n) { /* at: a GPU address (gpu_mem_dirty_runs) */
    PgAsk* a = user;
    for (u64 p = (at - a->bgva) >> 12, e = (at - a->bgva + n - 1) >> 12; p <= e; p++)
        if ((a->take[p] & 15) == PG_CUR) a->take[p] |= PG_GPU;
}
/* What [gva, gva + len) needs before it is bound, with nothing changed: a range that is refused leaves its pages as
   they were. 1, with a filled in; 0, or -1 when a page changed after a draw of this submission read it and patch is 0.
   whole: the range is to be written in place and published, so the region must hold the truth of every page it is
   on: no other owner or watch may be on them */
static int ask(u64 gva, u64 len, u64 align, int patch, int whole, PgAsk* a) {
    u64 cpu = 0;
    if (g_pages.on <= 0 || !len) return 0;
    if (len > PG_LEN_MAX) return refuse(PG_NO_LONG);
    if (gmmu_run(gva, len, &cpu) != len) return refuse(PG_NO_RUN);
    u64 rg = a->rg = cpu >> PG_REGION_SHIFT;
    a->at = cpu & (PG_REGION - 1);
    if (cpu + len > AS_SIZE || (cpu + len - 1) >> PG_REGION_SHIFT != rg) return refuse(PG_NO_REGION);
    if (align > 1 && a->at % align) return refuse(PG_NO_ALIGN);
    u32 now = a->now = vk_ww_now(), epoch = a->epoch = vk_ww_epoch();
    a->gnow = gpu_mem_generation();
    a->sub = vk_submit_number();
    if (!now) return refuse(PG_NO_TRACK); /* a stamp of 0 is a page with no bytes */
    PgRegion* r = a->r = region_get(rg);
    if (!r) return refuse(PG_NO_MEMORY);
    u64 p0 = a->p0 = a->at >> 12, p1 = a->p1 = (a->at + len - 1) >> 12, n = p1 - p0 + 1;
    u64 base = a->base = (rg << PG_REGION_SHIFT) + (p0 << 12);
    u64 bgva = a->bgva = gva - (cpu - base); /* the pages of a run go on alike */
    memset(a->take, PG_CUR, n);
    a->stale = a->covered = 0;
    a->hot = 0;
    /* the pages with bytes the cache owns, which hold the truth; another owner's bytes in the range are not here at
       all */
    for (u64 from = base, end = base + (n << 12); own_any() && from < end;) {
        OwnRange o[64];
        u32 k = own_lookup(from, end - from, o, 64), m = k < 64 ? k : 64;
        for (u32 i = 0; i < m; i++) {
            u64 at = o[i].addr & (PG_REGION - 1);
            const PgGran* q = r->gran[at >> PG_SHIFT];
            if (q && q->own && o[i].res == q->own) {
                for (u64 p = at >> 12, e = (at + o[i].size - 1) >> 12; p <= e; p++) a->take[p - p0] = PG_MINE;
                u64 lo = o[i].addr > cpu ? o[i].addr : cpu;
                u64 hi = o[i].addr + o[i].size < cpu + len ? o[i].addr + o[i].size : cpu + len;
                if (hi > lo) a->covered += hi - lo;
            } else if (whole || (o[i].addr < cpu + len && cpu < o[i].addr + o[i].size))
                return refuse(PG_NO_OWNED);
        }
        if (k <= 64) break;
        from = o[63].addr + o[63].size;
    }
    /* the oldest stamps of the pages with bytes, for one question to each tracking over the whole range. A granule
       with all its pages in the range and bytes on each answers for them, unless it has an owner: some of its pages
       may be the cache's own then */
    u64 oldest = ~0ull;
    u32 first = ~0u;
    int unasked = 0;
    for (u64 p = p0; p <= p1;) {
        u64 g = p >> (PG_SHIFT - 12), ge = ((g + 1) << (PG_SHIFT - 12)) - 1;
        const PgGran* q = r->gran[g];
        if (ge > p1) ge = p1;
        if (!q) {
            memset(a->take + (p - p0), PG_NEW, ge - p + 1);
            a->stale += ge - p + 1;
        } else if (!q->own && q->full == PG_FULL && ge - p + 1 == PG_PAGES) {
            if (q->gw_min < oldest) oldest = q->gw_min;
            if (q->seen_min < first) first = q->seen_min;
            if (q->ck_epoch != epoch || q->ck_mask != PG_FULL) unasked = 1;
        } else
            for (u64 x = p; x <= ge; x++) {
                u32 k = (u32)(x & (PG_PAGES - 1));
                if (a->take[x - p0] == PG_MINE) continue;
                if (!q->seen[k]) {
                    a->take[x - p0] = PG_NEW;
                    a->stale++;
                    continue;
                }
                if (q->gw[k] < oldest) oldest = q->gw[k];
                if (q->seen[k] < first) first = q->seen[k];
                if (q->ck_epoch != epoch || !(q->ck_mask >> k & 1)) unasked = 1;
            }
        p = ge + 1;
    }
    int looks = 0;
    if (oldest != ~0ull && gpu_mem_dirty_since(oldest, bgva, n << 12)) {
        gpu_mem_dirty_runs(oldest, bgva, n << 12, gpu_note, a);
        looks = 1;
    }
    if (unasked) {
        int w = vk_written_since(GP(base), n << 12, first);
        if (w < 0) return refuse(PG_NO_TRACK);
        if (w) {
            vk_written_runs(GP(base), n << 12, first, cpu_note, a);
            looks = 1;
        }
    }
    /* a page to be read from guest memory must not be trapped there, and one to be the cache's own must have no watch */
    int trapped = own_any() && own_trapped(base, n << 12);
    for (u64 p = p0; p <= p1 && (looks || trapped); p++) {
        u8 t = a->take[p - p0];
        if (t == PG_MINE) continue;
        u64 pcpu = base + ((p - p0) << 12);
        if (t & (PG_CPU | PG_GPU)) { /* written since the oldest page was taken: since this one was? */
            const PgGran* q = r->gran[p >> (PG_SHIFT - 12)];
            u32 k = (u32)(p & (PG_PAGES - 1));
            int old = (t & PG_GPU) && gpu_mem_dirty_since(q->gw[k], bgva + ((p - p0) << 12), 4096), by_cpu = 0;
            if (!old && (t & PG_CPU) && (q->ck_epoch != epoch || !(q->ck_mask >> k & 1))) {
                int w = vk_written_since(GP(pcpu), 4096, q->seen[k]);
                if (w < 0) return refuse(PG_NO_TRACK);
                old = by_cpu = w;
            }
            t = a->take[p - p0] = by_cpu ? PG_OLD_CPU : old ? PG_OLD : PG_CUR;
            if (old) a->stale++;
            /* a page a draw of this submission was given: the upload commands run before that draw */
            if (old && q->given == a->sub) {
                if (!patch) {
                    refuse(PG_NO_HOT);
                    return -1;
                }
                a->hot = 1;
            }
        }
        if (t == PG_CUR && !whole) continue;
        if (trapped && own_trapped(pcpu, 4096)) return refuse(PG_NO_OWNED);
    }
    return 1;
}
/* the oldest stamps of a granule whose pages all have bytes */
static void gran_oldest(PgGran* q) {
    q->gw_min = ~0ull;
    q->seen_min = ~0u;
    for (u32 k = 0; k < PG_PAGES; k++) {
        if (q->gw[k] < q->gw_min) q->gw_min = q->gw[k];
        if (q->seen[k] < q->seen_min) q->seen_min = q->seen[k];
    }
}
/* the range a asked about brought up to date: its granules given memory, and the pages that take bytes copied from
   guest memory through the ring. 0 when memory or the ring has none, with the pages copied so far up to date */
static int bring(PgAsk* a) {
    PgRegion* r = a->r;
    u64 p0 = a->p0, p1 = a->p1, left = a->stale, piece = 0, done = 0;
    a->buf = r->buf;
    a->bias = 0;
    if (g_pages.slabs) {
        const u32 at = slab_cover(r, p0 >> (PG_SHIFT - 12), p1 >> (PG_SHIFT - 12), a->sub);
        if (!at) return refuse(PG_NO_MEMORY);
        const PgSlab* k = &r->slab[at - 1];
        a->buf = k->buf;
        a->bias = (u64)k->g0 << PG_SHIFT;
        /* a slab that took others' bytes in this submission takes them in its own commands: what its pages take now
           goes there too, after that copy */
        if (k->late == a->sub) a->hot = 1;
    } else
        for (u64 g = p0 >> (PG_SHIFT - 12); g <= p1 >> (PG_SHIFT - 12); g++)
            if (!gran_get(r, g)) return refuse(PG_NO_MEMORY);
    if (a->hot && left) {
        vk_render_end(); /* copies happen outside rendering passes */
        g_pages.stats.patched++;
    }
    VkBuffer sb = VK_NULL_HANDLE;
    u64 so = 0;
    u8* host = NULL;
    VkBufferCopy run[PG_RUNS];
    u32 nrun = 0;
    int took = 0;
    for (u64 p = p0; p <= p1; p++) {
        PgGran* q = r->gran[p >> (PG_SHIFT - 12)];
        u32 k = (u32)(p & (PG_PAGES - 1));
        u8 t = a->take[p - p0];
        if (p == p0 || !k) {
            if (q->ck_epoch != a->epoch) {
                q->ck_epoch = a->epoch;
                q->ck_mask = 0;
            }
            q->used = g_vk_present;
            q->given = a->sub;
            took = 0;
        }
        if (t == PG_MINE) /* the tracking was not asked: it is, once the page is a copy again */
            q->ck_mask &= (u16)~(1u << k);
        else
            q->ck_mask |= (u16)(1u << k); /* taken now, or found unwritten: not asked again in this epoch */
        if (t == PG_OLD_CPU) q->cpu_wrote = g_vk_present ? g_vk_present : 1;
        if (t == PG_NEW || t == PG_OLD || t == PG_OLD_CPU) {
            if (!piece) { /* the ring a piece at a time: a window's first use is tens of megabytes */
                runs_flush(a->buf, sb, run, &nrun, a->hot);
                piece = left < PG_STAGE ? left : PG_STAGE;
                done = 0;
                if (!(host = vk_stage_upload(piece << 12, vk_stage_align(), &sb, &so))) {
                    q->ck_mask &= (u16)~(1u << k); /* not taken: the tracking is asked for it again */
                    if (took && q->full == PG_FULL) gran_oldest(q);
                    return refuse(PG_NO_MEMORY);
                }
            }
            memcpy(host + (done << 12), GP(a->base + ((p - p0) << 12)), 4096);
            q->seen[k] = a->now;
            q->gw[k] = a->gnow;
            q->full |= (u16)(1u << k);
            took = 1;
            if (nrun && run[nrun - 1].dstOffset + run[nrun - 1].size == (p << 12) - a->bias)
                run[nrun - 1].size += 4096; /* touching the last: one copy */
            else {
                if (nrun == PG_RUNS) runs_flush(a->buf, sb, run, &nrun, a->hot);
                run[nrun++] = (VkBufferCopy){so + (done << 12), (p << 12) - a->bias, 4096};
            }
            done++;
            piece--;
            left--;
        }
        if (took && (p == p1 || k == PG_PAGES - 1) && q->full == PG_FULL) gran_oldest(q);
    }
    runs_flush(a->buf, sb, run, &nrun, a->hot);
    if (a->stale) {
        g_pages.stats.uploads++;
        g_pages.stats.upload_bytes += a->stale << 12;
    }
    return 1;
}

int vk_pages_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off) {
    PgAsk a;
    u64 t0 = len > PG_PATCH_MIN ? plat_ns() : 0;
    int got = ask(gva, len, align, len > PG_PATCH_MIN, 0, &a);
    if (got > 0 && !bring(&a)) got = 0;
    if (t0) {
        g_pages.stats.longs++;
        g_pages.stats.long_ns += plat_ns() - t0;
    }
    if (got <= 0) return got;
    u64 lo = (a.rg << PG_REGION_SHIFT) + a.at;
    order_use(lo, lo + len, 0, 0);
    g_pages.stats.served++;
    g_pages.stats.served_bytes += len;
    *buf = a.buf;
    *off = a.at - a.bias;
    return 1;
}
void vk_pages_read(u64 gva, u64 len) {
    u64 cpu = 0;
    g_pages.stats.again++;
    if (g_pages.on > 0 && g_pages.owners && gmmu_run(gva, len, &cpu) == len) order_use(cpu, cpu + len, 0, 0);
}

int vk_pages_write(u64 gva, u64 len, u64 align, int ordered, VkBuffer* buf, u64* off) {
    PgAsk a;
    if (!vk_pages_keep()) return 0;
    if (len > PG_WRITE_MAX) return refuse(PG_NO_LONG);
    if (ask(gva, len, align, 0, 1, &a) <= 0) {
        g_pages.stats.write_refused[PG_WNO_ASK]++;
        return 0;
    }
    PgRegion* r = a.r;
    u64 p0 = a.p0, p1 = a.p1;
    /* pages the CPU writes now, or wrote in the last PG_QUIET presents: the game writes them as it runs, each write
       of a page with a result on it would land the page, and the copies of buffer.c wait for nothing */
    for (u64 p = p0; p <= p1; p++) {
        const PgGran* q = r->gran[p >> (PG_SHIFT - 12)];
        if (a.take[p - p0] == PG_OLD_CPU || (q && q->cpu_wrote && g_vk_present - q->cpu_wrote < PG_QUIET)) {
            g_pages.stats.write_refused[PG_WNO_CPU]++;
            return 0;
        }
    }
    if (!bring(&a)) {
        g_pages.stats.write_refused[PG_WNO_ASK]++;
        return 0;
    }
    for (u64 g = p0 >> (PG_SHIFT - 12); g <= p1 >> (PG_SHIFT - 12); g++) {
        PgGran* q = r->gran[g];
        if (q->own) continue;
        if (!(q->own = vk_own_res_borrow(r->buf))) {
            g_pages.stats.write_refused[PG_WNO_OWNER]++;
            return 0;
        }
        /* the first of all: the reads recorded so far in this submission were not kept */
        if (!g_pages.owners++) {
            order_begin();
            g_pages.order.rd_all = 1;
        }
    }
    u64 cpu = (a.rg << PG_REGION_SHIFT) + a.at;
    for (u64 p = p0; p <= p1; p++) /* the tracking is asked again once a page is a copy */
        r->gran[p >> (PG_SHIFT - 12)]->ck_mask &= (u16)~(1u << (p & (PG_PAGES - 1)));
    /* the range becomes the cache's own where it is not yet, a granule's part at a time: from here on the region's
       buffer holds its bytes, and guest memory is trapped under them */
    if (a.covered != len) {
        for (u64 lo = cpu, end = cpu + len; lo < end;) {
            u64 hi = (lo | (PG_SIZE - 1)) + 1 < end ? (lo | (PG_SIZE - 1)) + 1 : end;
            vk_own_publish(lo, hi - lo, r->gran[(lo & (PG_REGION - 1)) >> PG_SHIFT]->own, lo & (PG_REGION - 1));
            lo = hi;
        }
        /* a range that cannot be trapped (memory mapped in two places) landed at once instead: memory has its bytes,
           and the draw's would reach nothing */
        u64 owned = 0;
        for (u64 from = cpu, end = cpu + len; from < end;) {
            OwnRange o[64];
            u32 k = own_lookup(from, end - from, o, 64), m = k < 64 ? k : 64;
            for (u32 i = 0; i < m; i++) {
                const PgGran* q = r->gran[(o[i].addr & (PG_REGION - 1)) >> PG_SHIFT];
                if (q && o[i].res == q->own) owned += o[i].size;
            }
            if (k <= 64) break;
            from = o[63].addr + o[63].size;
        }
        if (owned != len) {
            g_pages.stats.not_trapped++;
            return 0;
        }
        g_pages.stats.published += len - a.covered;
    }
    for (u64 g = p0 >> (PG_SHIFT - 12); g <= p1 >> (PG_SHIFT - 12); g++)
        vk_own_res_wrote(r->gran[g]->own); /* a landing of the granule waits for this submission */
    order_use(cpu, cpu + len, 1, ordered);
    u32 st = 0;
    while (st < g_pages.nstamp && (g_pages.stamp[st].gva != gva || g_pages.stamp[st].len != len)) st++;
    if (st == g_pages.nstamp) {
        if (st == PG_STAMPS) vk_pages_submit(); /* no room: those now, which is early and never wrong */
        g_pages.stamp[g_pages.nstamp].gva = gva;
        g_pages.stamp[g_pages.nstamp++].len = (u32)len;
    }
    g_pages.writes++;
    g_pages.stats.written++;
    g_pages.stats.written_bytes += len;
    *buf = r->buf;
    *off = a.at;
    return 1;
}

/* a sixteenth of the regions every fourth present: a granule nothing read for twenty seconds, and in 120 presents,
   gives its memory back, as a buffer of the cache's own does (buffer.c buf_frame). One with pages of the cache's own
   holds their only bytes: those land in guest memory first, as the CPU's touch would land them, a few granules a
   sweep (a landing is a round trip to the GPU), and it goes at a later sweep */
void vk_pages_frame(void) {
    if (g_pages.on <= 0 || g_vk_present % 4 || !g_pages.bound) return;
    u32 before = vk_present_before(20000000000ull, 120);
    if (!before) return;
    u32 part = g_pages.sweep++ & 15, landed = 0;
    for (u32 rg = part; rg < g_pages.regions; rg += 16) {
        PgRegion* r = g_pages.region[rg];
        if (!r) continue;
        for (u32 at = 0; at < r->nslab; at++) { /* a slab goes whole, once none of its granules was read lately */
            const PgSlab* k = &r->slab[at];
            int idle = k->buf != VK_NULL_HANDLE;
            for (u64 g = k->g0; idle && g < (u64)k->g0 + k->n; g++) idle = (i32)(r->gran[g]->used - before) < 0;
            if (idle) slab_drop(r, at);
        }
        for (u64 g = 0; !g_pages.slabs && g < PG_GRANULES; g++) {
            const PgGran* q = r->gran[g];
            if (!q || (i32)(q->used - before) >= 0) continue;
            u64 cpu = ((u64)rg << PG_REGION_SHIFT) + (g << PG_SHIFT);
            OwnRange o[PG_PAGES];
            u32 k = q->own && own_any() ? own_lookup(cpu, PG_SIZE, o, PG_PAGES) : 0, own = k > PG_PAGES;
            for (u32 i = 0; i < k && i < PG_PAGES; i++) own += o[i].res == q->own;
            if (!own)
                gran_drop(r, g);
            else if (landed < PG_LAND) {
                own_acquire(cpu, PG_SIZE);
                landed++;
                g_pages.stats.landed++;
            }
        }
    }
}

void vk_pages_forget_all(void) {
    if (g_pages.on <= 0) return;
    for (u32 rg = 0; rg < g_pages.regions; rg++) {
        PgRegion* r = g_pages.region[rg];
        if (!r) continue;
        for (u64 g = 0; g < PG_GRANULES; g++)
            if (r->gran[g]) { /* no bytes: taken again */
                memset(r->gran[g]->seen, 0, sizeof r->gran[g]->seen);
                r->gran[g]->full = r->gran[g]->ck_mask = 0;
            }
    }
}

void vk_pages_log(void) {
    if (g_pages.on <= 0) return;
    const u64* no = g_pages.stats.refused;
    LOG("vulkan: page cache over 600 presents: %llu ranges bound again where they were, %llu ranges of more than "
        "1 MiB looked at in %.1f ms\n",
        (unsigned long long)g_pages.stats.again, (unsigned long long)g_pages.stats.longs,
        (double)g_pages.stats.long_ns / 1e6);
    LOG("vulkan: page cache over 600 presents: %llu ranges bound (%.1f MB), %llu of them with pages copied first "
        "(%.1f MB), %llu after draws that read them; %u granules hold %.1f MB in %u regions, %llu let go; refused: "
        "%llu too long, %llu not one run, %llu across a region, %llu for alignment, %llu with no tracking, %llu on "
        "owned pages, %llu changed after a draw read them, %llu for memory\n",
        (unsigned long long)g_pages.stats.served, (double)g_pages.stats.served_bytes / 1048576.0,
        (unsigned long long)g_pages.stats.uploads, (double)g_pages.stats.upload_bytes / 1048576.0,
        (unsigned long long)g_pages.stats.patched, g_pages.bound, (double)g_pages.bound * (double)PG_SIZE / 1048576.0,
        g_pages.nregion, (unsigned long long)g_pages.stats.let_go, (unsigned long long)no[PG_NO_LONG],
        (unsigned long long)no[PG_NO_RUN], (unsigned long long)no[PG_NO_REGION], (unsigned long long)no[PG_NO_ALIGN],
        (unsigned long long)no[PG_NO_TRACK], (unsigned long long)no[PG_NO_OWNED], (unsigned long long)no[PG_NO_HOT],
        (unsigned long long)no[PG_NO_MEMORY]);
    if (g_pages.slabs)
        LOG("vulkan: page cache over 600 presents: %u slabs hold the granules; %llu made over others, with %.1f MB "
            "copied from those\n",
            g_pages.nslabs, (unsigned long long)g_pages.stats.joins,
            (double)g_pages.stats.joined_bytes / 1048576.0);
    const u64* wno = g_pages.stats.write_refused;
    if (g_pages.owners || wno[PG_WNO_ASK] || wno[PG_WNO_CPU])
        LOG("vulkan: page cache, draws' results: %llu ranges written in place (%.1f MB), %llu refused as a read is "
            "or for another owner on their pages, %llu on pages the CPU wrote lately, %llu bytes made the GPU's, "
            "%llu ranges that could not be trapped; %u granules have an owner, %llu idle ones landed; %llu barriers "
            "for the order of work in place\n",
            (unsigned long long)g_pages.stats.written, (double)g_pages.stats.written_bytes / 1048576.0,
            (unsigned long long)(wno[PG_WNO_ASK] + wno[PG_WNO_OWNER]), (unsigned long long)wno[PG_WNO_CPU],
            (unsigned long long)g_pages.stats.published,
            (unsigned long long)g_pages.stats.not_trapped, g_pages.owners, (unsigned long long)g_pages.stats.landed,
            (unsigned long long)g_pages.stats.barriers);
    memset(&g_pages.stats, 0, sizeof g_pages.stats);
}
