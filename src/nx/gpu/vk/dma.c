/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The DMA copy engine on the GPU (maxwell/dma.c hands a launch over), its destination kept there
   (docs/GPU_OWNERSHIP.md). The destination's span is copied as it is now (guest bytes, what the GPU holds of them,
   render targets drawn over them) into a buffer of its own; the source's span is staged the same way (or from the
   copy made at submission). Block linear sides are taken apart into rows (blocklinear.comp) and the lines go between
   rows: copied, or remapped component by component (texconv.comp), exactly a line's bytes each, so what surrounds
   them keeps its bytes. The destination's rows go back into its layout, and the buffer is published over its span.
   A launch bigger than one allocation goes in bands of lines, each a copy of its own (dma_bands).
   Under vk_lock, in the frame's command buffer: the semaphore after the copy is the renderer's queued release, which
   comes after it */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/gpu/vk/descriptors.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"

static u64 up(u64 v, u64 a) { return (v + a - 1) / a * a; }

/* ------------------------------------------------ copies of linear sources made at submission (maxwell/dma.c) */
/* They were read into malloc's memory on the submitting thread and copied again into staging here: now they are read
   once, into host-cached memory the copy reads in place. Chunks of it are made as the copies need them, up to what
   maxwell/dma.c holds at most (256 MiB), each with a VMA virtual block handing out its ranges. Any thread allocates
   (vk_snap_alloc, under the lock); a copy lets go of its range when its frame's work is done, an unused one at once */
#define SNAP_CHUNK  (32ull << 20)
#define SNAP_CHUNKS 8
typedef struct {
    u32 chunk;
    VmaVirtualAllocation a;
    u64 off, len;
} SnapMem;
typedef struct {
    VkBuffer buf;
    VmaAllocation mem;
    u8* ptr;
    VmaVirtualBlock block;
} SnapChunk;
static struct {
    PMutex lk;
    SnapChunk c[SNAP_CHUNKS];
    u64 bytes;                /* held, for the test */
    u64 copies, bands, moved; /* vk_dma_stats: the launches, the bands of big ones, the bytes read and written */
} g_dma = {PMUTEX_INIT, {{0}}, 0, 0, 0, 0};

static int snap_chunk_new(SnapChunk* c) {
    VkBufferCreateInfo bci = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = SNAP_CHUNK,
                              .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo aci = {
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VmaVirtualBlockCreateInfo vbi = {.size = SNAP_CHUNK};
    VmaAllocationInfo info;
    if (!VK_OK(vk_buffer_create(g_vk.vma, &bci, &aci, &c->buf, &c->mem, &info))) return 0;
    if (!VK_OK(vmaCreateVirtualBlock(&vbi, &c->block))) {
        vmaDestroyBuffer(g_vk.vma, c->buf, c->mem);
        c->buf = VK_NULL_HANDLE;
        return 0;
    }
    vk_mem_track(c->mem);
    c->ptr = info.pMappedData;
    return 1;
}
u8* vk_snap_alloc(u64 len, u64* id) {
    VmaVirtualAllocationCreateInfo vai = {.size = len ? len : 1, .alignment = 64};
    SnapMem* m;
    u8* p = NULL;
    if (len > SNAP_CHUNK || !(m = (SnapMem*)malloc(sizeof *m))) return NULL;
    pm_lock(&g_dma.lk);
    for (u32 i = 0; i < SNAP_CHUNKS && !p; i++) {
        SnapChunk* c = &g_dma.c[i];
        if (!c->buf && !snap_chunk_new(c)) break;
        if (vmaVirtualAllocate(c->block, &vai, &m->a, &m->off) == VK_SUCCESS) { /* else the next chunk */
            m->chunk = i;
            m->len = len;
            p = c->ptr + m->off;
            g_dma.bytes += len;
        }
    }
    pm_unlock(&g_dma.lk);
    if (!p) {
        free(m);
        return NULL;
    }
    *id = (u64)(uintptr_t)m;
    return p;
}
void vk_snap_free(u64 id) {
    SnapMem* m = (SnapMem*)(uintptr_t)id;
    if (!m) return;
    pm_lock(&g_dma.lk);
    vmaVirtualFree(g_dma.c[m->chunk].block, m->a);
    g_dma.bytes -= m->len;
    pm_unlock(&g_dma.lk);
    free(m);
}
static void snap_free_later(void* m) { vk_snap_free((u64)(uintptr_t)m); }
u64 vk_snap_bytes(void) {
    u64 b;
    pm_lock(&g_dma.lk);
    b = g_dma.bytes;
    pm_unlock(&g_dma.lk);
    return b;
}

/* the rows of a block linear side's lines that lie in its surface */
static u32 side_rows(const GpuDmaSide* s, u32 lines) {
    return s->oy >= s->h ? 0 : s->h - s->oy < lines ? s->h - s->oy : lines;
}
/* guest bytes [gva, gva + span) as the GPU sees them, in the frame's device memory, for a side that is only read */
static int stage_range(u64 gva, u64 span, VkBuffer* buf, u64* at) {
    return vk_stage_device(span, vk_stage_align(), buf, at) && vk_guest_read_seen(gva, span, *buf, *at);
}

/* What a copy reads and writes: the source's bytes from src.addr + slo (sspan of them), the destination's from
   dst.addr + dlo (dlen), each a pitch linear side's lines or a block linear one's rows of blocks the lines lie in, as
   the rest of a surface keeps its bytes and their owner; and the scratch, the source's rows (block linear) at 0, the
   destination's (block linear) at at_b and the remapped lines at at_c */
typedef struct {
    u32 sv, dv; /* the lines in each side: a block linear side's rows past its surface aren't */
    u64 slo, sspan, dlo, dlen, at_b, at_c, scratch;
} DmaNeeds;
static DmaNeeds dma_needs(const GpuDmaOp* o) {
    DmaNeeds n = {0};
    u32 lines = o->lines, pa = (u32)up(o->src.wb, 16), pb = (u32)up(o->dst.wb, 16), pc = (u32)up(o->line_out, 4);
    u64 hi;
    n.sv = o->src.bl ? side_rows(&o->src, lines) : lines;
    n.dv = o->dst.bl ? side_rows(&o->dst, lines) : lines;
    if (!o->fill && !o->src.bl)
        n.sspan = (u64)o->src.pitch * (lines - 1) + o->line_in;
    else if (!o->fill && n.sv) {
        VkBlMove m = {o->src.wb, o->src.h, o->src.bh, o->src.bd, o->src.oy, n.sv, o->src.z, 1, pa};
        vk_bl_range(&m, &n.slo, &hi);
        n.sspan = hi - n.slo;
    }
    if (!o->dst.bl)
        n.dlen = (u64)o->dst.pitch * (lines - 1) + o->line_out;
    else if (n.dv) {
        VkBlMove m = {o->dst.wb, o->dst.h, o->dst.bh, o->dst.bd, o->dst.oy, n.dv, o->dst.z, 1, pb};
        vk_bl_range(&m, &n.dlo, &hi);
        n.dlen = hi - n.dlo;
    }
    n.at_b = up(o->src.bl ? (u64)pa * lines : 0, 16);
    n.at_c = n.at_b + up(o->dst.bl ? (u64)pb * lines : 0, 16);
    n.scratch = n.at_c + (o->remap && (o->fill || o->shuffle) ? (u64)pc * lines : 0) + 16;
    return n;
}

void vk_dma_stats(u64* copies, u64* bands, u64* bytes) {
    *copies = g_dma.copies;
    *bands = g_dma.bands;
    *bytes = g_dma.moved;
}

/* o's lines that could not be copied, the bytes it lacked them for and where they go */
static int dma_fail(const GpuDmaOp* o, const char* what, u64 bytes) {
    LOG_LIMIT(8, "vulkan: a DMA copy of %u lines of %u bytes to %llx (row %u on) %s %llu bytes: nothing is copied\n",
              o->lines, o->line_out, (unsigned long long)o->dst.addr, o->dst.bl ? o->dst.oy : 0, what,
              (unsigned long long)bytes);
    return 0;
}

/* o's lines as need says; a pitch linear source made at submission read from snap_skip bytes into its copy */
static int dma(const GpuDmaOp* o, const DmaNeeds* need, u64 snap_skip) {
    u32 lines = o->lines, sv = need->sv, dv = need->dv;
    u32 pa = (u32)up(o->src.wb, 16), pb = (u32)up(o->dst.wb, 16), pc = (u32)up(o->line_out, 4);
    int remap = o->remap && (o->fill || o->shuffle);
    u64 at_a = 0, at_b = need->at_b, at_c = need->at_c, sspan = need->sspan, dlo = need->dlo, dlen = need->dlen;
    u64 cap, sat = 0;
    VkBuffer sb = VK_NULL_HANDLE, scr;
    VkCommandBuffer cb;
    VkOwnRes* d;
    VkBlMove md = {o->dst.wb, o->dst.h, o->dst.bh, o->dst.bd, o->dst.oy, dv, o->dst.z, 1, pb};
    if (o->dst.bl && !dv) return 1; /* every line past the surface: nothing lands */
    vk_render_end();
    if (!(scr = vk_bl_scratch(need->scratch, &cap))) return dma_fail(o, "found no scratch buffer of", need->scratch);
    if (sspan) {
        u64 sgva = o->src.addr + need->slo;
        int got;
        if (o->snap_id) { /* the source as it was at submission, where it was read into */
            const SnapMem* m = (const SnapMem*)(uintptr_t)o->snap_id;
            const SnapChunk* c = &g_dma.c[m->chunk]; /* chunks stay once made */
            sb = c->buf;
            sat = m->off + snap_skip;
            got = VK_OK(vmaFlushAllocation(g_vk.vma, c->mem, sat, sspan));
        } else if (o->snap) { /* the same in the submitting thread's memory */
            u8* p = vk_stage(sspan, vk_stage_align(), &sb, &sat);
            if ((got = p != NULL)) memcpy(p, o->snap, sspan);
        } else if (!vk_rt_over(sgva, sspan)) {
            /* the buffer cache's copy, made again only when its bytes change: one game's menu copied the same
               1080p surface from guest memory twice a frame, 17 MB a frame over PCIe */
            got = vk_buf_get_copy_src(sgva, sspan, &sb, &sat);
        } else
            got = stage_range(sgva, sspan, &sb, &sat);
        if (!got) return dma_fail(o, "could not stage its source's", sspan);
    }
    /* those bytes as they are, read straight into the buffer that becomes their owner */
    d = vk_own_res_new(dlen);
    if (!d || !vk_guest_read_seen(o->dst.addr + dlo, dlen, d->buf, 0)) {
        if (d) vk_own_res_unref(d);
        return dma_fail(o, "could not read its destination's", dlen);
    }
    cb = vk_cmd();
    vk_all_barrier(cb);
    /* the lines in: the source's rows (block linear: taken apart, rows past its surface zero) */
    VkBuffer ib = sb;
    u64 iat = sat;
    u32 ip = o->src.pitch;
    int ok = 1;
    if (!o->fill && o->src.bl) {
        VkBlMove m = {o->src.wb, o->src.h, o->src.bh, o->src.bd, o->src.oy, sv, o->src.z, 1, pa};
        if (sv) ok = vk_bl_move(cb, &m, 0, sb, sat, scr, at_a); /* sb holds its rows of blocks from sat */
        if (sv < lines) {                                       /* zeros, from staging */
            VkBuffer zb;
            u64 zat;
            u8* z = vk_stage((u64)pa * (lines - sv), vk_stage_align(), &zb, &zat);
            if (!z) {
                vk_own_res_unref(d);
                return dma_fail(o, "could not stage the zeros of its rows past the source,", (u64)pa * (lines - sv));
            }
            memset(z, 0, (size_t)pa * (lines - sv));
            VkBufferCopy c = {zat, at_a + (u64)pa * sv, (u64)pa * (lines - sv)};
            rec_CmdCopyBuffer(cb, zb, scr, 1, &c);
        }
        ib = scr;
        iat = at_a + o->src.ox;
        ip = pa;
    } else if (o->fill) { /* nothing is read: any bound range */
        ib = scr;
        iat = at_c;
        ip = 0;
    }
    /* where the lines out go: the destination's rows (block linear: taken apart) */
    VkBuffer tb = d->buf;
    u64 tat = 0;
    u32 tp = o->dst.pitch, tl = dv;
    if (o->dst.bl) {
        ok = ok && vk_bl_move(cb, &md, 0, d->buf, 0, scr, at_b);
        tb = scr;
        tat = at_b + o->dst.ox;
        tp = pb;
    }
    vk_all_barrier(cb);
    if (ok && tl) {
        if (remap) { /* the lines as the destination holds them (what keeps its bytes), then remapped, then back */
            vk_copy_rows(cb, tb, tat, tp, scr, at_c, pc, o->line_out, tl);
            vk_all_barrier(cb);
            ok = vk_tc_dma_remap(cb, o, tl, ib, iat, ip, scr, at_c, pc);
            vk_all_barrier(cb);
            vk_copy_rows(cb, scr, at_c, pc, tb, tat, tp, o->line_out, tl);
        } else
            vk_copy_rows(cb, ib, iat, ip, tb, tat, tp, o->line_out, tl);
        vk_all_barrier(cb);
        if (ok && o->dst.bl) ok = vk_bl_move(cb, &md, 1, d->buf, 0, scr, at_b);
        vk_all_barrier(cb);
    }
    /* the textures over a block linear destination take its rows now in scratch */
    Surf* hit[8];
    int hits = ok && tl && o->dst.bl && !o->dst.bd && !o->dst.z
                   ? vk_tex_dma_rows(o->dst.addr, o->dst.wb, o->dst.bh, o->dst.ox, o->dst.oy, o->line_out, dv, scr,
                                     at_b + o->dst.ox, pb, hit, 8)
                   : 0;
    if (ok) { /* those bytes are the buffer's from now on, run by run of guest memory */
        u64 off = 0;
        while (off < dlen) {
            u64 cpu = 0, n = gmmu_run(o->dst.addr + dlo + off, dlen - off, &cpu);
            if (!n) { /* an unmapped page takes no write */
                n = 0x1000 - ((o->dst.addr + dlo + off) & 0xFFF);
                if (n > dlen - off) n = dlen - off;
            } else
                vk_own_publish(cpu, n, d, off);
            off += n;
        }
        gpu_mem_written(o->dst.addr + dlo, dlen);
        vk_tex_dma_seen(hit, hits);
        g_dma.moved += sspan + dlen;
    } else
        dma_fail(o, "takes moves past the device's limits (said above) over", dlen);
    vk_own_res_unref(d);
    return ok;
}

u32 vk_band(const VkBands* b, u32 y, u32 left, u64* most) {
    u64 limit = b->cap / VK_PART_SHARE, m = b->need(b->user, y, left);
    u32 n = left, row = b->base + y;
    if (!y && m <= b->cap) { /* in one piece */
        *most = m;
        return n;
    }
    while (m > limit) { /* half as many, the band's end on a unit */
        u32 end = row + n / 2;
        end -= end % b->unit;
        if (end <= row) end = row - row % b->unit + b->unit;
        if (end - row >= n) break; /* a unit is the least a band takes */
        n = end - row;
        m = b->need(b->user, y, n);
    }
    *most = m;
    return n;
}

/* A copy too big for one allocation goes in bands of lines (vk_band), each a copy of its own. A block linear
   destination's bands end on its rows of blocks (8 << bh rows), so each block is read and written back by one band.
   Lines [y, y + lines) of o as a copy of their own: a pitch linear side's start y lines on, a block linear one's
   origin y rows down */
static GpuDmaOp dma_band(const GpuDmaOp* o, u32 y, u32 lines) {
    GpuDmaOp b = *o;
    b.lines = lines;
    if (o->src.bl)
        b.src.oy += y;
    else if (!o->fill)
        b.src.addr += (u64)y * o->src.pitch;
    if (o->dst.bl)
        b.dst.oy += y;
    else
        b.dst.addr += (u64)y * o->dst.pitch;
    if (o->snap) b.snap = o->snap + (u64)y * o->src.pitch;
    return b;
}
static u64 most_of(const DmaNeeds* n) {
    u64 m = n->sspan > n->dlen ? n->sspan : n->dlen;
    return m > n->scratch ? m : n->scratch;
}
static u64 dma_need(const void* user, u32 y, u32 lines) {
    GpuDmaOp b = dma_band((const GpuDmaOp*)user, y, lines);
    DmaNeeds n = dma_needs(&b);
    return most_of(&n);
}
static int dma_bands(const GpuDmaOp* o) {
    VkBands bands = {g_vk.alloc_cap, o->dst.bl ? 8u << o->dst.bh : 1, o->dst.bl ? o->dst.oy : 0, dma_need, o};
    for (u32 y = 0, lines; y < o->lines; y += lines) {
        u64 most = 0;
        lines = vk_band(&bands, y, o->lines - y, &most);
        GpuDmaOp b = dma_band(o, y, lines);
        if (most > bands.cap) return dma_fail(&b, "needs more than an allocation may be:", most);
        DmaNeeds n = dma_needs(&b);
        if (!dma(&b, &n, o->fill || o->src.bl ? 0 : (u64)y * o->src.pitch)) return 0;
        if (lines < o->lines) g_dma.bands++;
    }
    return 1;
}
int vk_dma_copy(const GpuDmaOp* o) {
    int r;
    if (!o->lines || !o->line_out) {
        vk_snap_free(o->snap_id);
        return 1;
    }
    vk_lock();
    g_dma.copies++;
    r = dma_bands(o);
    if (o->snap_id) vk_defer_call(snap_free_later, (void*)(uintptr_t)o->snap_id); /* after its frame's work */
    vk_unlock();
    return r;
}
