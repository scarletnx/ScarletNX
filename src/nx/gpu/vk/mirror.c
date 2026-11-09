/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The mirror: guest memory in the GPU's memory, at the same offsets, for the storage buffers of the programs that log
   their writes (an Ounce game's compute programs, sm86_translate.c's mark_written). Each such storage buffer is all of
   the memory mapped from its address, up to 16 MiB: made anew for each dispatch, with its input copied and its result
   copied back, it cost a game most of its CPU and GPU time and gigabytes of rings. In the mirror the programs read
   and write guest memory where it is, one dispatch after another, as on the console.

   - One sparse buffer as big as the guest's address space (vk_mirror_slice), memory bound in granules of MIR_G where
     bindings are, and a mask beside it, a bit for each byte. The programs' stores set bits in the dispatch's write
     log (SNX_VKB_WPAGES, SH_WLOG_*), and a pass after the dispatch ORs them into the mask (vk_mirror_fold).
   - A shadow in host memory of the guest bytes the mirror was given. The pages the CPU wrote since a granule was
     last brought up to date (write tracking) are compared with it, and only the bytes the CPU changed are copied in:
     the bytes a program wrote there, which haven't landed yet, stay.
   - At each submit the pages written in it (their mask words) are copied into host memory with their bytes, and land
     in guest memory and in the shadow once it is done (vk_mirror_harvest, vk_mirror_done). The masks are cleared then.

   All under vk_lock. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/pending_pages.h"
#include "common/utilities/bits.h"

#define MIR_G      (2ull << 20)   /* a granule: memory is bound and kept up to date a granule at a time */
#define MIR_PAGES  (MIR_G / 4096) /* its 4 KiB pages */
#define MIR_MASK_G (MIR_G / 8)    /* its mask's bytes */
#define MIR_REGIONS 1024u          /* the regions of one copy command */

typedef struct {
    u8* shadow;                /* the guest bytes the mirror was given, as the GPU holds them less its own writes */
    VmaAllocation mem, mask;   /* bound into the mirror and its mask */
    u64 valid[MIR_PAGES / 64]; /* the pages given guest memory's bytes: those bindings covered */
    /* each valid page's write tracking clock before it was last brought up to date: only a binding's own pages are,
       as the others may not be mapped any more */
    u32 synced[MIR_PAGES];
} Gran;

/* the guest pages programs wrote in the mirror that haven't landed (vk_mirror_wrote to vk_mirror_done) */
static PendPages g_mir_pend = {.also = &g_pend_any};
static struct {
    int on; /* -1 until vk_mirror_init has decided */
    VkBuffer buf, mask;
    u64 granules;
    Gran** gran; /* by granule, NULL where none is bound */
    u64 bound;   /* granules bound */
} g_mir = {.on = -1};

int vk_mirror_on(void) {
    if (g_mir.on >= 0) return g_mir.on;
    g_mir.on = 0;
    /* the mirror is one buffer as big as the guest's address space: one past the device's maxBufferSize is invalid
       (the validation layer says so), yet RADV, whose maxBufferSize is 4 GiB, makes and runs it sparse, as feat/ounce
       does: vk_mirror_past_limit (on by default) makes it there, else its results land through the CPU */
    char big[128];
    snprintf(big, sizeof big, "buffers of %llu bytes at most, where the mirror is %llu",
             (unsigned long long)g_vk.max_buffer_size, (unsigned long long)AS_SIZE);
    const char* why = vk_uma()                           ? "memory shared with the CPU: read in place"
                      : !g_vk.sparse                     ? "no sparse binding"
                      : !g_vk.feat.sparseResidencyBuffer ? "no sparse residency of buffers"
                      : g_vk.max_buffer_size < AS_SIZE && !g_settings.gpu_mirror_past_limit ? big
                      : g_vk.props.limits.sparseAddressSpaceSize < AS_SIZE + AS_SIZE / 8 ? "too small an address space"
                      : vk_written_since(g_mem + AS_BASE, 1, 0) < 0                      ? "no write tracking"
                                                                                         : NULL;
    if (why) {
        LOG("vulkan: storage buffers of the programs that log their writes are made for each dispatch (%s)\n", why);
        return 0;
    }
    u32 fam[2] = {g_vk.gfx_family, g_vk.compute_family};
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .flags = VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
                              .size = AS_SIZE,
                              /* the programs' storage, and any read of their results that hasn't landed: draws bind
                                 it as their vertices, indices, constants or indirect arguments (buffer.c's buf_get) */
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                       VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                       VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                                       VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
                              .sharingMode = fam[0] != fam[1] ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE,
                              .queueFamilyIndexCount = fam[0] != fam[1] ? 2 : 0,
                              .pQueueFamilyIndices = fam};
    VkMemoryRequirements req[2];
    if (!VK_OK(vkCreateBuffer(g_vk.dev, &bci, NULL, &g_mir.buf))) return 0;
    bci.size = AS_SIZE / 8;
    /* the mask: what the programs wrote, set by the folds (vk_mirror_fold) and read by the harvest; a copy source for
       the tests that check it */
    bci.usage =
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (!VK_OK(vkCreateBuffer(g_vk.dev, &bci, NULL, &g_mir.mask))) {
        vkDestroyBuffer(g_vk.dev, g_mir.buf, NULL);
        return 0;
    }
    vkGetBufferMemoryRequirements(g_vk.dev, g_mir.buf, &req[0]);
    vkGetBufferMemoryRequirements(g_vk.dev, g_mir.mask, &req[1]);
    if (MIR_G % req[0].alignment || MIR_MASK_G % req[1].alignment) {
        LOG("vulkan: storage buffers of the programs that log their writes are made for each dispatch (sparse blocks "
            "of %llu bytes)\n",
            (unsigned long long)req[0].alignment);
        vkDestroyBuffer(g_vk.dev, g_mir.buf, NULL);
        vkDestroyBuffer(g_vk.dev, g_mir.mask, NULL);
        return 0;
    }
    g_mir.granules = AS_SIZE / MIR_G;
    g_mir.gran = calloc(g_mir.granules, sizeof *g_mir.gran);
    if (!g_mir.gran) fatal("Out of memory for the GPU mirror's table");
    LOG("vulkan: storage buffers of the programs that log their writes are read and written in a mirror of guest "
        "memory (sparse, %llu GiB, granules of %llu MiB)%s\n",
        (unsigned long long)(AS_SIZE >> 30), (unsigned long long)(MIR_G >> 20),
        g_vk.max_buffer_size < AS_SIZE ? ", past the device's maxBufferSize (vk_mirror_past_limit=1)" : "");
    g_mir.on = 1;
    return 1;
}
VkBuffer vk_mirror_buffer(void) { return g_mir.buf; }
VkBuffer vk_mirror_mask(void) { return g_mir.mask; }

/* memory for granule g bound into the mirror and its mask, the mask cleared; 0 out of memory */
static Gran* gran_bind(u64 g) {
    Gran* r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->shadow = malloc(MIR_G);
    VkMemoryRequirements req[2];
    vkGetBufferMemoryRequirements(g_vk.dev, g_mir.buf, &req[0]);
    vkGetBufferMemoryRequirements(g_vk.dev, g_mir.mask, &req[1]);
    req[0].size = MIR_G;
    req[1].size = MIR_MASK_G;
    VmaAllocationCreateInfo aci = {.preferredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT};
    VmaAllocationInfo info[2];
    if (!r->shadow || !VK_OK(vmaAllocateMemory(g_vk.vma, &req[0], &aci, &r->mem, &info[0]))) goto fail;
    if (!VK_OK(vmaAllocateMemory(g_vk.vma, &req[1], &aci, &r->mask, &info[1]))) {
        vmaFreeMemory(g_vk.vma, r->mem);
        goto fail;
    }
    vk_mem_track(r->mem);
    vk_mem_track(r->mask);
    VkSparseMemoryBind b[2] = {
        {.resourceOffset = g * MIR_G, .size = MIR_G, .memory = info[0].deviceMemory, .memoryOffset = info[0].offset},
        {.resourceOffset = g * MIR_MASK_G,
         .size = MIR_MASK_G,
         .memory = info[1].deviceMemory,
         .memoryOffset = info[1].offset}};
    VkSparseBufferMemoryBindInfo bb[2] = {{g_mir.buf, 1, &b[0]}, {g_mir.mask, 1, &b[1]}};
    VkBindSparseInfo bi = {.sType = VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, .bufferBindCount = 2, .pBufferBinds = bb};
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    int ok = VK_OK(vkCreateFence(g_vk.dev, &fi, NULL, &fence));
    if (ok) {
        /* bound before any work that reads it is submitted: once a granule, so a wait is no cost */
        vk_queue_lock(g_vk.gfx);
        ok = VK_OK(vkQueueBindSparse(g_vk.gfx, 1, &bi, fence));
        vk_queue_unlock(g_vk.gfx);
        ok = ok && VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(g_vk.dev, fence, NULL);
    }
    if (!ok) fatal("vulkan: could not bind memory into the GPU mirror");
    vk_render_end();
    rec_CmdFillBuffer(vk_cmd(), g_mir.mask, g * MIR_MASK_G, MIR_MASK_G, 0);
    g_mir.bound++;
    return r;
fail:
    free(r->shadow);
    free(r);
    return NULL;
}

/* The bytes to copy into the mirror: gathered in host memory first, so that staging takes their size alone, then
   copied from staging in one command */
typedef struct {
    VkBufferCopy* r; /* srcOffset: in data */
    u32 n, cap;
    u8* data;
    u64 used, dcap;
} Up;
static u8* up_bytes(Up* u, u64 dst, u64 n) {
    if (u->used + n > u->dcap) {
        u64 cap = u->dcap ? u->dcap : 65536;
        while (cap < u->used + n) cap *= 2;
        u8* grown = realloc(u->data, cap);
        if (!grown) fatal("Out of memory bringing the GPU mirror up to date");
        u->data = grown;
        u->dcap = cap;
    }
    if (u->n && u->r[u->n - 1].dstOffset + u->r[u->n - 1].size == dst &&
        u->r[u->n - 1].srcOffset + u->r[u->n - 1].size == u->used)
        u->r[u->n - 1].size += n; /* goes on from the last */
    else {
        if (u->n == u->cap) {
            u32 cap = u->cap ? u->cap * 2 : 256;
            VkBufferCopy* grown = realloc(u->r, (size_t)cap * sizeof *grown);
            if (!grown) fatal("Out of memory bringing the GPU mirror up to date");
            u->r = grown;
            u->cap = cap;
        }
        u->r[u->n++] = (VkBufferCopy){u->used, dst, n};
    }
    u8* p = u->data + u->used;
    u->used += n;
    return p;
}
/* the bytes gathered, into staging and recorded as one copy into the mirror; 0 when staging has no room */
static int up_end(Up* u) {
    int ok = 1;
    if (u->n) {
        VkBuffer sb;
        u64 so;
        u8* sp = vk_stage_upload(u->used, 16, &sb, &so);
        if (sp) {
            memcpy(sp, u->data, u->used);
            for (u32 i = 0; i < u->n; i++) u->r[i].srcOffset += so;
            VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                  .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT};
            VkDependencyInfo d = {
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
            vk_render_end();
            rec_CmdPipelineBarrier2(vk_cmd(), &d);
            for (u32 i = 0; i < u->n; i += MIR_REGIONS) /* commands of a size the recording ring takes */
                rec_CmdCopyBuffer(vk_cmd(), sb, g_mir.buf, u->n - i < MIR_REGIONS ? u->n - i : MIR_REGIONS, u->r + i);
        } else
            ok = 0;
    }
    free(u->r);
    free(u->data);
    return ok;
}

/* whether the mirror owns a part of guest memory [cpu, cpu + n): its bytes there are the mirror's (own_entries) */
static int mirror_owns(u64 cpu, u64 n) {
    OwnRange o[16];
    if (!own_any()) return 0;
    u32 k = own_lookup(cpu, n, o, 16);
    for (u32 i = 0; i < k && i < 16; i++)
        if (((const VkOwnRes*)o[i].res)->buf == g_mir.buf) return 1;
    return 0;
}
/* the owned bytes of [cpu, cpu + n), copied on the GPU from their owners, after the copies from host memory, sixteen
   owners at a time. The mirror's own are where they belong */
static void up_owned(u64 cpu, u64 n) {
    OwnRange o[16];
    if (!own_any()) return;
    for (u64 at = cpu, end = cpu + n; at < end;) {
        u32 k = own_lookup(at, end - at, o, 16);
        for (u32 i = 0; i < k && i < 16; i++) {
            if (((const VkOwnRes*)o[i].res)->buf == g_mir.buf) continue;
            VkBufferCopy c = {o[i].res_off, o[i].addr, o[i].size};
            vk_render_end();
            rec_CmdCopyBuffer(vk_cmd(), ((const VkOwnRes*)o[i].res)->buf, g_mir.buf, 1, &c);
        }
        if (k <= 16) break;
        at = o[15].addr + o[15].size;
    }
}

typedef struct {
    Gran* r;
    u64 base, lo; /* the granule's guest address, and the page of it the runs count from */
    Up* u;
} Diff;
/* the bytes of a run of written pages that differ from the shadow, those of valid pages, into the copy and the
   shadow */
static void diff_run(void* user, u64 at, u64 n) {
    const Diff* d = user;
    for (u64 p = d->lo + (at >> 12); p < d->lo + ((at + n + 4095) >> 12); p++) {
        if (!(d->r->valid[p / 64] >> (p % 64) & 1)) continue;
        /* read without touching a trapped page: a page written, then made GPU-owned, is copied from its owner
           instead (up_owned), so its owned bytes are taken as the shadow has them here */
        static u8 page[4096];
        OwnRange o[16];
        u8* sh = d->r->shadow + p * 4096;
        const u8* now = page;
        u64 a = d->base + p * 4096;
        u32 owned = own_stage(a, 4096, page, o, 16);
        for (u32 i = 0; i < owned && i < 16; i++) memcpy(page + (o[i].addr - a), sh + (o[i].addr - a), o[i].size);
        if (owned > 16) continue; /* too many to tell: left to the owners' copies */
        /* a page no program's write is pending on: whole, if anything changed, as nothing of the GPU's is there to
           keep. A page written on the CPU in scattered bytes made one region of each run, 194,545 in one copy */
        if (!pend_hit_pages(&g_mir_pend, a >> 12, a >> 12)) {
            if (memcmp(page, sh, 4096)) {
                memcpy(up_bytes(d->u, a, 4096), page, 4096);
                memcpy(sh, page, 4096);
            }
            continue;
        }
        for (u32 k = 0; k < 4096;) {
            if (now[k] == sh[k]) { /* eight at a time where they agree */
                u64 x, y;
                if (!(k & 7) && k + 8 <= 4096) {
                    memcpy(&x, now + k, 8);
                    memcpy(&y, sh + k, 8);
                    if (x == y) {
                        k += 8;
                        continue;
                    }
                }
                k++;
                continue;
            }
            u32 e = k + 1;
            while (e < 4096 && now[e] != sh[e]) e++;
            memcpy(up_bytes(d->u, d->base + p * 4096 + k, e - k), now + k, e - k);
            memcpy(sh + k, now + k, e - k);
            k = e;
        }
    }
}

int vk_mirror_slice(u64 gva, u64 len, VkBuffer* buf, u64* off) {
    u64 cpu = 0;
    if (!vk_mirror_on() || !len || gmmu_run(gva, len, &cpu) != len || cpu + len > AS_SIZE) return 0;
    u32 now = vk_ww_now(); /* before any byte is read */
    u64 g0 = cpu / MIR_G, g1 = (cpu + len - 1) / MIR_G;
    Up u = {0};
    int owners = 0;
    for (u64 g = g0; g <= g1; g++) {
        Gran* r = g_mir.gran[g];
        if (!r && !(r = g_mir.gran[g] = gran_bind(g))) {
            up_end(&u);
            return 0;
        }
        u64 base = g * MIR_G;
        u64 lo = cpu > base ? (cpu - base) >> 12 : 0, hi = (cpu + len - base + 4095) >> 12;
        if (hi > MIR_PAGES) hi = MIR_PAGES;
        /* the binding's valid pages written since the oldest of them was brought up to date: the bytes the CPU
           changed, those that differ from the shadow (a page brought up to date later compares equal). Owned pages
           aren't watched: their owners are copied in below */
        u32 oldest = now;
        int any = 0;
        for (u64 p = lo; p < hi; p++)
            if (r->valid[p / 64] >> (p % 64) & 1) {
                if (!any || (i32)(r->synced[p] - oldest) < 0) oldest = r->synced[p];
                any = 1;
            }
        int written = any ? vk_written_since(GP(base + lo * 4096), (hi - lo) * 4096, oldest) : 0;
        if (written) {
            Diff d = {r, base, lo, &u};
            if (written > 0)
                vk_written_runs(GP(base + lo * 4096), (hi - lo) * 4096, oldest, diff_run, &d);
            else /* pages the tracking doesn't watch: all of them compared */
                diff_run(&d, 0, (hi - lo) * 4096);
        }
        for (u64 p = lo; p < hi; p++) r->synced[p] = now;
        /* the binding's pages not given yet: whole, guest memory's bytes, an owned part's left for its owner. One the
           mirror owns has its bytes there already */
        for (u64 p = lo; p < hi; p++) {
            if (r->valid[p / 64] >> (p % 64) & 1) continue;
            u64 a = base + p * 4096;
            if (mirror_owns(a, 4096)) continue;
            OwnRange o[16];
            u8* dst = up_bytes(&u, a, 4096);
            if (own_stage(a, 4096, dst, o, 16)) owners = 1;
            memcpy(r->shadow + p * 4096, dst, 4096);
            r->valid[p / 64] |= 1ull << (p % 64);
        }
    }
    if (!up_end(&u)) fatal("vulkan: no staging to bring the GPU mirror up to date");
    /* the owners' bytes over the binding, when there are any: copied each time, as an owner's resource changes with
       each write of its producer (own_hash would tell, at the cost of the walk) */
    if (owners || own_hash(cpu, len, &(u64){0})) up_owned(cpu, len);
    /* the copies, and a new granule's cleared mask, before the work that binds it */
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                          .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vk_render_end();
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    *buf = g_mir.buf;
    *off = cpu;
    return 1;
}

/* ---- what the programs wrote, back into guest memory ----
   A program's writable ranges in the mirror are pending from its dispatch on (vk_mirror_wrote): a read of guest memory
   there lands them first (vk_mirror_mem_need), as with any GPU result. At the submit, the pages the submit's programs
   wrote are copied into host memory with their masks (mirror_harvest.comp); once it is done they land, the bytes logged
   and no others, in guest memory and in the shadow (vk_mirror_done). The output's room is what the last submits wrote:
   pages past it are copied by a pass of their own then, which waits for the GPU, and the room grows */
#define MIR_ENTRY      1156u /* an output entry's words: the page, 3 unused, 128 mask words, 1024 words of bytes */
#define MIR_ENTRY_PAGE 132u  /* one without the bytes, for a page left GPU-owned in the mirror (own_traps) */
#define MIR_HEAD  4u
typedef struct {
    u64 first, end; /* pages [first, end) */
} MirRange;
typedef struct {
    u64 submit;
    MirRange* r;
    u32 n, room, entry;
    u64 pages;
    u32* out; /* in host memory, MIR_HEAD words then room entries then the pages left */
    VmaAllocation mem;
} MirBatch;
static MirRange* g_mir_now; /* the ranges written in the submit being recorded */
static u32 g_mir_nnow, g_mir_capnow;
static MirBatch* g_mir_batch;
static u32 g_mir_nbatch, g_mir_capbatch;
/* the output's room: past it a page takes a pass of its own, which waits for the GPU; every page of room takes
   4.5 KiB of the readback ring at each submit that wrote any. Grown at once when a submit wrote more, and every
   MIR_SPAN submits set to twice the most of them since. Kept at the most it was, one game's harvests took 2.6 GB of
   the ring every 10 s for a few pages each */
#define MIR_ROOM_MIN 64u
#define MIR_SPAN     64u
static u32 g_mir_room = MIR_ROOM_MIN, g_mir_span_max, g_mir_span_n;
static struct {
    u64 harvests, pages, landed_bytes, overflows, owned_pages;
} g_mir_stats;

void vk_mirror_wrote(u64 cpu, u64 len) {
    if (!len) return;
    if (g_mir_nnow == g_mir_capnow) {
        u32 cap = g_mir_capnow ? g_mir_capnow * 2 : 32;
        MirRange* grown = realloc(g_mir_now, (size_t)cap * sizeof *grown);
        if (!grown) fatal("Out of memory recording the GPU mirror's writes");
        g_mir_now = grown;
        g_mir_capnow = cap;
    }
    MirRange r = {cpu >> 12, (cpu + len + 4095) >> 12};
    g_mir_now[g_mir_nnow++] = r;
    pend_add_pages(&g_mir_pend, r.first, r.end - 1, 1);
}
int vk_mirror_pending(u64 gva, u64 len) { return g_mir.on > 0 && len && pend_hit(&g_mir_pend, gva, len); }
int vk_mirror_holds(u64 gva, u64 len) {
    u64 cpu = 0;
    return g_mir.on > 0 && len && own_any() && gmmu_run(gva, len, &cpu) == len && mirror_owns(cpu, len);
}

static int range_cmp(const void* a, const void* b) {
    u64 x = ((const MirRange*)a)->first, y = ((const MirRange*)b)->first;
    return (x > y) - (x < y);
}
/* the ranges sorted, those that meet joined: their count */
static u32 ranges_join(MirRange* r, u32 n) {
    qsort(r, n, sizeof *r, range_cmp);
    u32 k = 0;
    for (u32 i = 0; i < n; i++)
        if (k && r[i].first <= r[k - 1].end) {
            if (r[i].end > r[k - 1].end) r[k - 1].end = r[i].end;
        } else
            r[k++] = r[i];
    return k;
}

#include "nx/gpu/vk/mirror_harvest_spv.h" /* compiled from mirror_harvest.comp by the build (cmake/glsl.cmake) */
#include "nx/gpu/vk/mirror_fold_spv.h"    /* and mirror_fold.comp */
typedef struct {
    u32 first, room, left, entry;
} HarvestParams;
typedef struct {
    u32 flags, masks, base, shift, len;
} FoldParams;
/* a compute pipeline of the mirror's own: its storage buffers, bindings 0 on, and push constants */
typedef struct {
    VkDescriptorSetLayout ds;
    VkPipelineLayout layout;
    VkPipeline pipe;
} MirPipe;
static MirPipe g_hv, g_fold;
static void pipe_init(MirPipe* mp, const u32* code, u64 code_size, u32 nbind, u32 push_size, const char* what) {
    if (mp->pipe) return;
    VkDescriptorSetLayoutBinding b[3];
    for (u32 i = 0; i < nbind; i++)
        b[i] =
            (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nbind, .pBindings = b};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &mp->ds)))
        fatal("vulkan: cannot create the GPU mirror %s's descriptor layout", what);
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &mp->ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &mp->layout)))
        fatal("vulkan: cannot create the GPU mirror %s's pipeline layout", what);
    if (!VK_OK(snx_vk_compute_pipeline(g_vk.dev, vk_pipeline_cache(), mp->layout, code, code_size, &mp->pipe)))
        fatal("vulkan: cannot create the GPU mirror %s's pipeline", what);
}
static void barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                    VkAccessFlags2 da) {
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = ss,
                          .srcAccessMask = sa,
                          .dstStageMask = ds,
                          .dstAccessMask = da};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(cb, &d);
}
/* records into cb the harvest of the pages of ranges r (joined, sorted) into out (room entries, then the list of pages
   left, after the work that wrote them): 0 out of descriptors. The sets come from pool, or with none from the frame's
   (vk_desc_alloc), which starts the frame's command buffer: never inside a landing */
static int harvest_record(VkCommandBuffer cb, const MirRange* r, u32 n, VkBuffer out, u64 out_at, u64 out_size,
                          u32 room, u32 entry, VkDescriptorPool pool) {
    pipe_init(&g_hv, mirror_harvest_spv, mirror_harvest_spv_size, 3, sizeof(HarvestParams), "harvest");
    rec_CmdFillBuffer(cb, out, out_at, 16, 0);
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_hv.pipe);
    for (u32 i = 0; i < n; i++) {
        u64 pages = r[i].end - r[i].first;
        VkDescriptorSet set = VK_NULL_HANDLE;
        if (pool) {
            VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                              .descriptorPool = pool,
                                              .descriptorSetCount = 1,
                                              .pSetLayouts = &g_hv.ds};
            if (!VK_OK(vkAllocateDescriptorSets(g_vk.dev, &ai, &set))) return 0;
        } else if (!(set = vk_desc_alloc(g_hv.ds)))
            return 0;
        VkDescriptorBufferInfo info[3] = {{g_mir.mask, r[i].first * 512, pages * 512},
                                          {g_mir.buf, r[i].first * 4096, pages * 4096},
                                          {out, out_at, out_size}};
        VkWriteDescriptorSet w[3];
        for (u32 k = 0; k < 3; k++)
            w[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = set,
                                          .dstBinding = k,
                                          .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .pBufferInfo = &info[k]};
        vkUpdateDescriptorSets(g_vk.dev, 3, w, 0, NULL);
        HarvestParams p = {(u32)r[i].first, room, MIR_HEAD + room * entry, entry};
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_hv.layout, 0, 1, &set, 0, NULL);
        rec_CmdPushConstants(cb, g_hv.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        rec_CmdDispatch(cb, (u32)pages, 1, 1);
    }
    /* the output to the host; the masks cleared, before later programs set them again */
    barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    return 1;
}
static u64 out_bytes(u32 room, u64 pages, u32 entry) { return ((u64)MIR_HEAD + (u64)room * entry + pages) * 4; }

void vk_mirror_fold(VkCommandBuffer cb, VkBuffer log, u64 log_size, const VkMirFold* f, u32 n) {
    u64 salign = g_vk.props.limits.minStorageBufferOffsetAlignment, mask_size = AS_SIZE / 8;
    if (salign < 4) salign = 4;
    pipe_init(&g_fold, mirror_fold_spv, mirror_fold_spv_size, 2, sizeof(FoldParams), "fold");
    /* the dispatch's stores (its log), and the folds before, before the ORs */
    barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_fold.pipe);
    for (u32 i = 0; i < n; i++) {
        /* the mask's binding at a byte the device binds at, before the buffer's first bit */
        u64 at = (f[i].cpu / 8) & ~(salign - 1), rel = f[i].cpu - at * 8;
        u64 range = ((rel + f[i].len + 31) / 32 + 1) * 4;
        u32 windows = (f[i].len + 4095) / 4096;
        if (range > mask_size - at) range = mask_size - at;
        if (windows > SH_WPAGE_SLOT) windows = SH_WPAGE_SLOT;
        if (!windows) continue;
        VkDescriptorSet set = vk_desc_alloc(g_fold.ds);
        if (!set) fatal("vulkan: no descriptors to log the GPU mirror's writes"); /* the dispatch is recorded */
        VkDescriptorBufferInfo info[2] = {{log, 0, log_size}, {g_mir.mask, at, range}};
        VkWriteDescriptorSet w[2];
        for (u32 k = 0; k < 2; k++)
            w[k] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                          .dstSet = set,
                                          .dstBinding = k,
                                          .descriptorCount = 1,
                                          .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                          .pBufferInfo = &info[k]};
        vkUpdateDescriptorSets(g_vk.dev, 2, w, 0, NULL);
        u32 flags = f[i].part * SH_WLOG_PART;
        FoldParams p = {flags, flags + SH_WPAGE_SLOT, (u32)(rel / 32), (u32)(rel % 32), f[i].len};
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_fold.layout, 0, 1, &set, 0, NULL);
        rec_CmdPushConstants(cb, g_fold.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        rec_CmdDispatch(cb, windows, 1, 1);
    }
    for (u32 i = 0; i < n; i++) vk_mirror_wrote(f[i].cpu, f[i].len); /* pending until the submit's harvest */
}

void vk_mirror_harvest(VkCommandBuffer cb, u64 submit) {
    if (!g_mir_nnow) return;
    /* each range written gave its pages a count (vk_mirror_wrote), and the batch takes one away from the pages of each
       joined range once it is done (vk_mirror_done). Where the ranges met, the joined ones take their own count first,
       then the written ones lose theirs, so a page never reads as not pending meanwhile. Without it the pages of two
       buffers that overlap stayed pending for good, and each CPU read of them waited for the GPU. The batch's ranges
       hold the written ones until then */
    u32 n0 = g_mir_nnow;
    MirRange* r = malloc((size_t)n0 * sizeof *r);
    if (!r) fatal("Out of memory recording the GPU mirror's writes");
    memcpy(r, g_mir_now, (size_t)n0 * sizeof *r);
    u32 n = ranges_join(g_mir_now, g_mir_nnow);
    if (n < n0) {
        for (u32 i = 0; i < n; i++) pend_add_pages(&g_mir_pend, g_mir_now[i].first, g_mir_now[i].end - 1, 1);
        for (u32 i = 0; i < n0; i++) pend_add_pages(&g_mir_pend, r[i].first, r[i].end - 1, -1);
    }
    memcpy(r, g_mir_now, (size_t)n * sizeof *r);
    u64 pages = 0;
    for (u32 i = 0; i < n; i++) pages += g_mir_now[i].end - g_mir_now[i].first;
    u32 room = pages < g_mir_room ? (u32)pages : g_mir_room;
    /* where the host traps guest memory, the pages stay GPU-owned in the mirror and only their masks come back */
    u32 entry = own_traps() ? MIR_ENTRY_PAGE : MIR_ENTRY;
    VkBuffer out;
    u64 at;
    VmaAllocation mem;
    u32* p = (u32*)(void*)vk_stage_back(out_bytes(room, pages, entry), &out, &at, &mem);
    if (!p || !harvest_record(cb, g_mir_now, n, out, at, out_bytes(room, pages, entry), room, entry, VK_NULL_HANDLE))
        fatal("vulkan: no memory to land the GPU mirror's writes in");
    if (g_mir_nbatch == g_mir_capbatch) {
        u32 cap = g_mir_capbatch ? g_mir_capbatch * 2 : 16;
        MirBatch* grown = realloc(g_mir_batch, (size_t)cap * sizeof *grown);
        if (!grown) fatal("Out of memory recording the GPU mirror's writes");
        g_mir_batch = grown;
        g_mir_capbatch = cap;
    }
    g_mir_batch[g_mir_nbatch++] = (MirBatch){submit, r, n, room, entry, pages, p, mem};
    g_mir_nnow = 0;
    g_mir_stats.harvests++;
}

/* entry e of an output, whose bytes are the GPU's: the bytes its mask gives into guest memory and the shadow */
static int bit(const u32* m, u32 b) { return m[b / 32] >> (b % 32) & 1; }
static void land_entry(const u32* e) {
    u64 cpu = (u64)e[0] << 12;
    const u32* mask = e + 4;
    const u8* data = (const u8*)(e + 132);
    Gran* r = cpu / MIR_G < g_mir.granules ? g_mir.gran[cpu / MIR_G] : NULL;
    u64 p = (cpu % MIR_G) >> 12;
    int shadowed = r && (r->valid[p / 64] >> (p % 64) & 1);
    for (u32 a = 0; a < 4096;) {
        u32 rest = mask[a / 32] >> (a % 32);
        if (!rest) { /* none from a to the word's end */
            a = (a / 32 + 1) * 32;
            continue;
        }
        a += snx_ctz32(rest);
        u32 b = a + 1;
        while (b < 4096 && bit(mask, b)) b++;
        own_acquire(cpu + a, b - a);
        vk_rt_before_write_cpu(cpu + a, b - a);
        memcpy(GP(cpu + a), data + a, b - a);
        if (shadowed) memcpy(r->shadow + p * 4096 + a, data + a, b - a);
        g_mir_stats.landed_bytes += b - a;
        a = b;
    }
    gpu_mem_written_cpu(cpu, 4096); /* caches built from the page take its new bytes */
}

static int u64_cmp(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return (x > y) - (x < y);
}
/* the entries of the pages the mirror's programs wrote, once done, to be taken in and published (vk_mirror_publish):
   MIR_ENTRY_PAGE words each, as the harvest wrote them */
static u32* g_mir_q;
static u32 g_mir_nq, g_mir_capq;
/* The pages of an output of entries without bytes stay GPU-owned in the mirror (docs/platforms/nx/ounce.md), at the
   next point where nothing is being recorded (vk_mirror_publish), pending until then as before. Taking them in records:
   done here, in a landing, which runs where a command buffer is being begun (a frame's recycling, whose pool reset then
   dropped the copies and began the buffer again), a draw set up or a submission made, it crashed in the driver. Landing
   each page the programs wrote at every submit took most of the GPU thread in one game's menus, for pages the CPU
   mostly never read */
static void own_entries(const u32* out, u32 n) {
    if (!n) return;
    if (g_mir_nq + n > g_mir_capq) {
        u32 cap = (g_mir_nq + n) * 2;
        u32* grown = realloc(g_mir_q, (size_t)cap * MIR_ENTRY_PAGE * sizeof *grown);
        if (!grown) fatal("Out of memory keeping the GPU mirror's writes");
        g_mir_q = grown;
        g_mir_capq = cap;
    }
    memcpy(g_mir_q + (size_t)g_mir_nq * MIR_ENTRY_PAGE, out + MIR_HEAD, (size_t)n * MIR_ENTRY_PAGE * sizeof *out);
    g_mir_nq += n;
    for (u32 i = 0; i < n; i++) {
        u64 pg = out[MIR_HEAD + i * MIR_ENTRY_PAGE];
        pend_add_pages(&g_mir_pend, pg, pg, 1);
    }
}
/* First the CPU's changes to the pages since they were brought up to date, the bytes their programs didn't log, go
   into the mirror: a byte both wrote keeps the program's, as a landing of the logged bytes keeps it. Then each run of
   them is published over guest memory with the mirror as its owner, written by the submission holding those copies,
   made first. The CPU's first touch lands a page whole, which is exact: none of its bytes can change in between.
   Publishing can write a render target back, which can land again: the queue is taken first, and a publishing inside
   takes none */
void vk_mirror_publish(void) {
    static int busy;
    if (g_mir.on <= 0 || !g_mir_nq || busy || vk_rec_unsafe()) return;
    busy = 1;
    u32* q = g_mir_q;
    u32 n = g_mir_nq;
    g_mir_q = NULL;
    g_mir_nq = g_mir_capq = 0;
    u64* pages = malloc((size_t)n * sizeof *pages);
    if (!pages) fatal("Out of memory keeping the GPU mirror's writes");
    Up u = {0};
    for (u32 i = 0; i < n; i++) {
        const u32* e = q + i * MIR_ENTRY_PAGE;
        const u32* mask = e + 4;
        u64 cpu = (u64)e[0] << 12, p = (cpu % MIR_G) >> 12;
        Gran* r = cpu / MIR_G < g_mir.granules ? g_mir.gran[cpu / MIR_G] : NULL;
        pages[i] = cpu >> 12;
        if (!r || mirror_owns(cpu, 4096)) continue; /* the mirror's already: the CPU couldn't change it */
        static u8 page[4096];
        OwnRange o[16];
        /* a page landed since it was brought up to date has no shadow: every byte the program didn't log is guest
           memory's (sh at the page's own bytes compares none equal) */
        int valid = r->valid[p / 64] >> (p % 64) & 1;
        const u8* sh = valid ? r->shadow + p * 4096 : NULL;
        u32 owned = own_stage(cpu, 4096, page, o, 16);
        if (owned > 16) continue; /* other owners' bytes all over it: theirs went into the mirror when it was bound */
        if (!valid) {
            for (u32 a = 0; a < 4096;) {
                if (bit(mask, a)) {
                    a++;
                    continue;
                }
                u32 b = a + 1;
                while (b < 4096 && !bit(mask, b)) b++;
                memcpy(up_bytes(&u, cpu + a, b - a), page + a, b - a);
                a = b;
            }
            /* owned parts: their owners' bytes, copied in after these (up_owned) */
            if (owned) up_owned(cpu, 4096);
            continue;
        }
        for (u32 k = 0; k < owned; k++) memcpy(page + (o[k].addr - cpu), sh + (o[k].addr - cpu), o[k].size);
        for (u32 a = 0; a < 4096;) {
            u64 x, y;
            if (!(a & 7)) { /* eight at a time where they agree */
                memcpy(&x, page + a, 8);
                memcpy(&y, sh + a, 8);
                if (x == y) {
                    a += 8;
                    continue;
                }
            }
            if (page[a] == sh[a] || bit(mask, a)) {
                a++;
                continue;
            }
            u32 b = a + 1;
            while (b < 4096 && page[b] != sh[b] && !bit(mask, b)) b++;
            memcpy(up_bytes(&u, cpu + a, b - a), page + a, b - a);
            memcpy(r->shadow + p * 4096 + a, page + a, b - a);
            a = b;
        }
    }
    if (!up_end(&u)) fatal("vulkan: no staging to bring the GPU mirror up to date");
    /* those copies before whatever reads the mirror next */
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vk_render_end();
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    u64 submit = vk_last_submit();
    free(q);
    qsort(pages, n, sizeof *pages, u64_cmp);
    VkOwnRes* res = vk_own_res_borrow(g_mir.buf);
    if (!res) fatal("Out of memory keeping the GPU mirror's writes");
    res->written = submit;
    for (u32 i = 0; i < n;) {
        u32 j = i + 1;
        while (j < n && pages[j] <= pages[j - 1] + 1) j++;
        u64 cpu = pages[i] << 12, len = (pages[j - 1] + 1 - pages[i]) << 12;
        /* where it can't own them, they land whole from the mirror now. Their shadow stays (the CPU can't change them
           while they are owned) until they land (vk_mirror_landed) */
        vk_own_publish_submitted(cpu, len, res, cpu);
        i = j;
    }
    for (u32 i = 0; i < n; i++) pend_add_pages(&g_mir_pend, pages[i], pages[i], -1);
    vk_own_res_unref(res);
    g_mir_stats.owned_pages += n;
    free(pages);
    busy = 0;
}
/* guest memory [cpu, cpu + size) landed from an owner, the mirror or another: its pages' shadow is let go of, and the
   next binding brings them up to date from guest memory whole, which holds what landed */
void vk_mirror_landed(u64 cpu, u64 size) {
    if (g_mir.on <= 0 || !size) return;
    for (u64 pg = cpu >> 12, e = (cpu + size - 1) >> 12; pg <= e; pg++) {
        u64 g = (pg << 12) / MIR_G, p = ((pg << 12) % MIR_G) >> 12;
        Gran* r = g < g_mir.granules ? g_mir.gran[g] : NULL;
        if (r) r->valid[p / 64] &= ~(1ull << (p % 64));
    }
}
/* the pages an output couldn't take, copied by a pass of their own on a command buffer of the mirror's, waited for,
   and landed: the work that wrote them is done, and so is all that was submitted before */
static VkCommandPool g_mir_pool;
static VkCommandBuffer g_mir_cb;
static void harvest_left(const u32* left, u32 n) {
    MirRange* r = malloc((size_t)n * sizeof *r);
    if (!r) fatal("Out of memory landing the GPU mirror's writes");
    for (u32 i = 0; i < n; i++) r[i] = (MirRange){left[i], (u64)left[i] + 1};
    u32 k = ranges_join(r, n);
    if (!g_mir_pool) {
        VkCommandPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                      .queueFamilyIndex = g_vk.gfx_family};
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                          .commandBufferCount = 1};
        if (!VK_OK(vkCreateCommandPool(g_vk.dev, &pi, NULL, &g_mir_pool)))
            fatal("vulkan: no command pool for the GPU mirror");
        ai.commandPool = g_mir_pool;
        if (!VK_OK(vkAllocateCommandBuffers(g_vk.dev, &ai, &g_mir_cb)))
            fatal("vulkan: no command buffer for the GPU mirror");
    }
    u64 size = out_bytes(n, n, MIR_ENTRY);
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = size,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {.flags =
                                       VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                   .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer out;
    VmaAllocation mem;
    VmaAllocationInfo info;
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bci, &aci, &out, &mem, &info)))
        fatal("vulkan: no memory to land the GPU mirror's writes in");
    /* its sets from a pool of its own, freed once it is done: it runs inside a landing (vk_mirror_done), where the
       frame's sets would start the frame's command buffer (vk_desc_alloc's vk_cmd). Started there, that command buffer
       outlived the present: the next frame's draws recorded into it after its slot came round again, past the
       recording thread, and bound their sets at once, which crashed in the driver */
    VkDescriptorPoolSize ps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 * k};
    VkDescriptorPoolCreateInfo dpi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = k, .poolSizeCount = 1, .pPoolSizes = &ps};
    VkDescriptorPool dpool;
    if (!VK_OK(vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &dpool)))
        fatal("vulkan: no descriptor pool to land the GPU mirror's writes");
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(g_mir_cb, &bi));
    if (!harvest_record(g_mir_cb, r, k, out, 0, size, n, MIR_ENTRY, dpool))
        fatal("vulkan: no descriptor set left to land the GPU mirror's writes");
    VK_OK(vkEndCommandBuffer(g_mir_cb));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g_mir_cb};
    int ok = VK_OK(vkCreateFence(g_vk.dev, &fi, NULL, &fence));
    if (ok) {
        vk_queue_lock(g_vk.gfx);
        ok = VK_OK(vkQueueSubmit(g_vk.gfx, 1, &si, fence));
        vk_queue_unlock(g_vk.gfx);
        ok = ok && VK_OK(vkWaitForFences(g_vk.dev, 1, &fence, VK_TRUE, UINT64_MAX));
        vkDestroyFence(g_vk.dev, fence, NULL);
    }
    if (!ok || !VK_OK(vmaInvalidateAllocation(g_vk.vma, mem, 0, VK_WHOLE_SIZE)))
        fatal("vulkan: could not land the GPU mirror's writes");
    vkDestroyDescriptorPool(g_vk.dev, dpool, NULL); /* its work is done */
    const u32* w = info.pMappedData;
    for (u32 i = 0; i < w[0] && i < n; i++) land_entry(w + MIR_HEAD + i * MIR_ENTRY);
    vmaDestroyBuffer(g_vk.vma, out, mem);
    free(r);
}

void vk_mirror_done(u64 done) {
    /* publishing a page can write a render target back, which can finish the commands and land through here again:
       a batch leaves the list before it is taken in, and a landing inside one takes none */
    static int busy;
    if (busy) return;
    busy = 1;
    for (;;) {
        u32 b = 0;
        while (b < g_mir_nbatch && g_mir_batch[b].submit > done) b++;
        if (b == g_mir_nbatch) break;
        MirBatch batch = g_mir_batch[b], *m = &batch;
        memmove(g_mir_batch + b, g_mir_batch + b + 1, (size_t)(g_mir_nbatch - b - 1) * sizeof *g_mir_batch);
        g_mir_nbatch--;
        if (!VK_OK(vmaInvalidateAllocation(g_vk.vma, m->mem, 0, VK_WHOLE_SIZE)))
            fatal("vulkan: could not read the GPU mirror's writes");
        u32 n = m->out[0], take = n < m->room ? n : m->room;
        vk_result_begin();
        if (m->entry == MIR_ENTRY_PAGE)
            own_entries(m->out, take);
        else
            for (u32 i = 0; i < take; i++) land_entry(m->out + MIR_HEAD + i * MIR_ENTRY);
        if (n > m->room) { /* the pages past its room: a pass of their own, which lands them, and more room */
            harvest_left(m->out + MIR_HEAD + m->room * m->entry, m->out[1]);
            while (g_mir_room < n * 2 && g_mir_room < (1u << 16)) g_mir_room *= 2;
            g_mir_stats.overflows++;
        }
        if (n > g_mir_span_max) g_mir_span_max = n;
        if (++g_mir_span_n == MIR_SPAN) {
            u32 room = MIR_ROOM_MIN;
            while (room < g_mir_span_max * 2 && room < (1u << 16)) room *= 2;
            g_mir_room = room;
            g_mir_span_max = g_mir_span_n = 0;
        }
        vk_result_end();
        g_mir_stats.pages += n;
        for (u32 i = 0; i < m->n; i++) pend_add_pages(&g_mir_pend, m->r[i].first, m->r[i].end - 1, -1);
        free(m->r);
    }
    busy = 0;
}

/* a read of guest memory over pending writes: they land first, as other results do (buffer.c's land_reason) */
static int mir_pending(u64 gva, u64 len) {
    return len == ~0ull ? g_mir_nnow || g_mir_nbatch : pend_hit(&g_mir_pend, gva, len); /* ~0: anywhere */
}
void vk_mirror_mem_need(u64 gva, u64 len) {
    if (g_mir.on <= 0 || !len || !mir_pending(gva, len)) return;
    vk_lock();
    /* a finish takes its pages in, and publishing them makes them the mirror's: the CPU's touch lands them */
    if (mir_pending(gva, len)) vk_finish();
    vk_mirror_publish();
    vk_unlock();
}

void vk_mirror_stats(u64* granules, u64* harvests, u64* pages, u64* landed, u64* overflows, u32* room) {
    *granules = g_mir.bound;
    *harvests = g_mir_stats.harvests;
    *pages = g_mir_stats.pages;
    *landed = g_mir_stats.landed_bytes;
    *overflows = g_mir_stats.overflows;
    *room = g_mir_room;
}
