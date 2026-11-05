/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The mirror: guest memory in the GPU's memory, at the same offsets, for the storage buffers of the programs that log
   their writes (an Ounce game's compute programs, sm86_translate.c's mark_written). Each such storage buffer is all of
   the memory mapped from its address, up to 16 MiB: made anew for each dispatch, with its input copied and its result
   copied back, it cost a game most of its CPU and GPU time and gigabytes of rings. In the mirror the programs read
   and write guest memory where it is, one dispatch after another, as on the console.

   - One sparse buffer as big as the guest's address space (vk_mirror_slice), memory bound in granules of MIR_G where
     bindings are, and a mask beside it, a bit for each byte, which the programs' stores set (SNX_VKB_WPAGES).
   - A shadow in host memory of the guest bytes the mirror was given. The pages the CPU wrote since a granule was
     last brought up to date (write tracking) are compared with it, and only the bytes the CPU changed are copied in:
     the bytes a program wrote there, which haven't landed yet, stay.
   - At each submit the pages written in it (their mask words) are copied into host memory with their bytes, and land
     in guest memory and in the shadow once it is done (vk_mirror_harvest, vk_mirror_done). The masks are cleared then.

   All under vk_lock. */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/gpu.h"
#include "nx/memory/mem.h"
#include "nx/memory/own.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/log.h"
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/pending_pages.h"
#include "common/utilities/bits.h"

#define MIR_G      (2ull << 20)   /* a granule: memory is bound and kept up to date a granule at a time */
#define MIR_PAGES  (MIR_G / 4096) /* its 4 KiB pages */
#define MIR_MASK_G (MIR_G / 8)    /* its mask's bytes */

typedef struct {
    u8* shadow;                /* the guest bytes the mirror was given, as the GPU holds them less its own writes */
    VmaAllocation mem, mask;   /* bound into the mirror and its mask */
    u64 valid[MIR_PAGES / 64]; /* the pages given guest memory's bytes: those bindings covered */
    /* each valid page's write tracking clock before it was last brought up to date: only a binding's own pages are,
       as the others may not be mapped any more */
    u32 synced[MIR_PAGES];
} Gran;

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
    const char* why = vk_uma()                           ? "memory shared with the CPU: read in place"
                      : !g_vk.sparse                     ? "no sparse binding"
                      : !g_vk.feat.sparseResidencyBuffer ? "no sparse residency of buffers"
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
    bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
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
        "memory (sparse, %llu GiB, granules of %llu MiB)\n",
        (unsigned long long)(AS_SIZE >> 30), (unsigned long long)(MIR_G >> 20));
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
            rec_CmdCopyBuffer(vk_cmd(), sb, g_mir.buf, u->n, u->r);
        } else
            ok = 0;
    }
    free(u->r);
    free(u->data);
    return ok;
}

/* the owned bytes of [cpu, cpu + n), copied on the GPU from their owners, after the copies from host memory */
static void up_owned(u64 cpu, u64 n) {
    OwnRange o[16];
    if (!own_any()) return;
    u32 k = own_lookup(cpu, n, o, 16);
    if (k > 16) fatal("vulkan: more owners under a storage binding than the GPU mirror takes");
    for (u32 i = 0; i < k; i++) {
        VkBufferCopy c = {o[i].res_off, o[i].addr, o[i].size};
        vk_render_end();
        rec_CmdCopyBuffer(vk_cmd(), ((const VkOwnRes*)o[i].res)->buf, g_mir.buf, 1, &c);
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
        u32 owned = own_stage(d->base + p * 4096, 4096, page, o, 16);
        for (u32 i = 0; i < owned && i < 16; i++)
            memcpy(page + (o[i].addr - (d->base + p * 4096)), sh + (o[i].addr - (d->base + p * 4096)), o[i].size);
        if (owned > 16) continue; /* too many to tell: left to the owners' copies */
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
        /* the binding's pages not given yet: whole, guest memory's bytes, an owned part's left for its owner */
        for (u64 p = lo; p < hi; p++) {
            if (r->valid[p / 64] >> (p % 64) & 1) continue;
            u64 a = base + p * 4096;
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
#define MIR_ENTRY 1156u /* an output entry's words: the page, 3 unused, 128 mask words, 1024 words of bytes */
#define MIR_HEAD  4u
typedef struct {
    u64 first, end; /* pages [first, end) */
} MirRange;
typedef struct {
    u64 submit;
    MirRange* r;
    u32 n, room;
    u64 pages;
    u32* out; /* in host memory, MIR_HEAD words then room entries then the pages left */
    VmaAllocation mem;
} MirBatch;
static PendPages g_mir_pend = {.also = &g_pend_any};
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
    u64 harvests, pages, landed_bytes, overflows;
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
typedef struct {
    u32 first, room, left;
} HarvestParams;
static VkDescriptorSetLayout g_hv_ds;
static VkPipelineLayout g_hv_layout;
static VkPipeline g_hv_pipe;
static void harvest_init(void) {
    if (g_hv_pipe) return;
    VkDescriptorSetLayoutBinding b[3];
    for (u32 i = 0; i < 3; i++)
        b[i] =
            (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo di = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 3, .pBindings = b};
    if (!VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &di, NULL, &g_hv_ds)))
        fatal("vulkan: cannot create the GPU mirror harvest's descriptor layout");
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HarvestParams)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1,
                                     .pSetLayouts = &g_hv_ds,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (!VK_OK(vkCreatePipelineLayout(g_vk.dev, &li, NULL, &g_hv_layout)))
        fatal("vulkan: cannot create the GPU mirror harvest's pipeline layout");
    if (!VK_OK(snx_vk_compute_pipeline(g_vk.dev, vk_pipeline_cache(), g_hv_layout, mirror_harvest_spv,
                                       mirror_harvest_spv_size, &g_hv_pipe)))
        fatal("vulkan: cannot create the GPU mirror harvest's pipeline");
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
   left, after the work that wrote them): 0 out of descriptors */
static int harvest_record(VkCommandBuffer cb, const MirRange* r, u32 n, VkBuffer out, u64 out_at, u64 out_size,
                          u32 room) {
    harvest_init();
    rec_CmdFillBuffer(cb, out, out_at, 16, 0);
    barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_hv_pipe);
    for (u32 i = 0; i < n; i++) {
        u64 pages = r[i].end - r[i].first;
        VkDescriptorSet set = vk_desc_alloc(g_hv_ds);
        if (!set) return 0;
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
        HarvestParams p = {(u32)r[i].first, room, MIR_HEAD + room * MIR_ENTRY};
        rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_hv_layout, 0, 1, &set, 0, NULL);
        rec_CmdPushConstants(cb, g_hv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        rec_CmdDispatch(cb, (u32)pages, 1, 1);
    }
    /* the output to the host; the masks cleared, before later programs set them again */
    barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
            VK_PIPELINE_STAGE_2_HOST_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
            VK_ACCESS_2_HOST_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    return 1;
}
static u64 out_bytes(u32 room, u64 pages) { return ((u64)MIR_HEAD + (u64)room * MIR_ENTRY + pages) * 4; }

void vk_mirror_harvest(VkCommandBuffer cb, u64 submit) {
    if (!g_mir_nnow) return;
    u32 n = ranges_join(g_mir_now, g_mir_nnow);
    u64 pages = 0;
    for (u32 i = 0; i < n; i++) pages += g_mir_now[i].end - g_mir_now[i].first;
    u32 room = pages < g_mir_room ? (u32)pages : g_mir_room;
    VkBuffer out;
    u64 at;
    VmaAllocation mem;
    u32* p = (u32*)(void*)vk_stage_back(out_bytes(room, pages), &out, &at, &mem);
    if (!p || !harvest_record(cb, g_mir_now, n, out, at, out_bytes(room, pages), room))
        fatal("vulkan: no memory to land the GPU mirror's writes in");
    if (g_mir_nbatch == g_mir_capbatch) {
        u32 cap = g_mir_capbatch ? g_mir_capbatch * 2 : 16;
        MirBatch* grown = realloc(g_mir_batch, (size_t)cap * sizeof *grown);
        if (!grown) fatal("Out of memory recording the GPU mirror's writes");
        g_mir_batch = grown;
        g_mir_capbatch = cap;
    }
    MirRange* r = malloc((size_t)n * sizeof *r);
    if (!r) fatal("Out of memory recording the GPU mirror's writes");
    memcpy(r, g_mir_now, (size_t)n * sizeof *r);
    g_mir_batch[g_mir_nbatch++] = (MirBatch){submit, r, n, room, pages, p, mem};
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
    u64 size = out_bytes(n, n);
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
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                   .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VK_OK(vkBeginCommandBuffer(g_mir_cb, &bi));
    if (!harvest_record(g_mir_cb, r, k, out, 0, size, n))
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
    const u32* w = info.pMappedData;
    for (u32 i = 0; i < w[0] && i < n; i++) land_entry(w + MIR_HEAD + i * MIR_ENTRY);
    vmaDestroyBuffer(g_vk.vma, out, mem);
    free(r);
}

void vk_mirror_done(u64 done) {
    u32 kept = 0;
    for (u32 b = 0; b < g_mir_nbatch; b++) {
        MirBatch* m = &g_mir_batch[b];
        if (m->submit > done) {
            g_mir_batch[kept++] = *m;
            continue;
        }
        if (!VK_OK(vmaInvalidateAllocation(g_vk.vma, m->mem, 0, VK_WHOLE_SIZE)))
            fatal("vulkan: could not read the GPU mirror's writes");
        u32 n = m->out[0], take = n < m->room ? n : m->room;
        vk_result_begin();
        for (u32 i = 0; i < take; i++) land_entry(m->out + MIR_HEAD + i * MIR_ENTRY);
        if (n > m->room) { /* the pages past its room: a pass of their own, and more room from now on */
            harvest_left(m->out + MIR_HEAD + m->room * MIR_ENTRY, m->out[1]);
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
    g_mir_nbatch = kept;
}

/* a read of guest memory over pending writes: they land first, as other results do (buffer.c's land_reason) */
static int mir_pending(u64 gva, u64 len) {
    return len == ~0ull ? g_mir_nnow || g_mir_nbatch : pend_hit(&g_mir_pend, gva, len); /* ~0: anywhere */
}
void vk_mirror_mem_need(u64 gva, u64 len) {
    if (g_mir.on <= 0 || !len || !mir_pending(gva, len)) return;
    vk_lock();
    if (mir_pending(gva, len)) vk_finish();
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
