/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The DMA copy engine on the GPU (maxwell/dma.c hands a launch over), its destination kept there
   (docs/layers/gpu/ownership.md). The destination's bytes the lines lie in (a pitch linear side's lines, a block linear one's
   tiles: the blocks of the lines' GOB columns, a run a row of blocks) are copied as they are now (guest bytes, what
   the GPU holds of them, render targets drawn over them) into a buffer of their own; the source's span is staged the
   same way (or from the copy made at submission, or the buffer cache's). Block linear sides' GOB columns are taken
   apart into rows (blocklinear.comp) and the lines go between rows: copied, or remapped component by component
   (texconv.comp), exactly a line's bytes each, so what surrounds them keeps its bytes. The destination's rows go back
   into its layout, and the buffer is published over the runs it was read from.
   A launch bigger than one allocation, or whose scratch is bigger than a quarter of one, goes in bands of lines, each
   a copy of its own (dma_bands), and the frame may end between two bands, as at a draw's start (vk_frame_check).
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
    u64 copies, bands, moved; /* vk_dma_stats: the launches, the bands of big ones, the bytes moved (vk.h) */
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

/* A block linear side's GOB columns its lines' bytes lie in: the first, *x0, and how many, *n. Blocks are a GOB wide,
   so these are the blocks of each row of blocks the copy reads or writes, its tiles; the rest keep their bytes */
static void side_cols(const GpuDmaSide* s, u32 bytes, u32* x0, u32* n) {
    u32 g = (s->wb + 63) / 64, a = s->ox / 64, b = (s->ox + bytes + 63) / 64;
    if (b > g) b = g;
    *x0 = a;
    *n = b > a ? b - a : 0;
}
/* a block linear side's bytes a block */
static u64 block_bytes(const GpuDmaSide* s) { return 512ull << (s->bh + s->bd); }

/* What a copy reads and writes. The source's bytes from src.addr + slo, sspan of them: a pitch linear side's lines,
   or a block linear one's rows of blocks the lines lie in, of which its GOB columns sx0 to sx0 + sgobs are taken apart
   (stiles bytes). The destination's: a pitch linear side's lines from dst.addr + dlo, or a block linear one's tiles,
   the blocks of the lines' GOB columns dx0 to dx0 + dgobs in each of its rows of blocks (drows of them from the one
   at dlo), druns runs of drun bytes, dstride apart in guest memory and packed one after another in the copy's buffer
   (dlen bytes): a surface of their own, dgobs GOBs wide, which the rest of the surface and its owner don't take part
   in. The scratch: the source's rows (block linear) at 0, the destination's (block linear) at at_b and the remapped
   lines at at_c */
typedef struct {
    u32 sv, dv; /* the lines in each side: a block linear side's rows past its surface aren't */
    u32 sx0, sgobs, dx0, dgobs, drows, druns;
    u64 slo, sspan, stiles, dlo, dlen, drun, dstride, at_b, at_c, scratch;
} DmaNeeds;
static DmaNeeds dma_needs(const GpuDmaOp* o) {
    DmaNeeds n = {0};
    u32 lines = o->lines, pc = (u32)up(o->line_out, 4);
    u64 hi;
    n.sv = o->src.bl ? side_rows(&o->src, lines) : lines;
    n.dv = o->dst.bl ? side_rows(&o->dst, lines) : lines;
    if (!o->fill && !o->src.bl)
        n.sspan = (u64)o->src.pitch * (lines - 1) + o->line_in;
    else if (!o->fill) { /* its columns even with no line in the surface: those read as zeros, as wide */
        side_cols(&o->src, o->line_in, &n.sx0, &n.sgobs);
        if (n.sv) {
            VkBlMove m = {o->src.wb, o->src.h, o->src.bh, o->src.bd, o->src.oy, n.sv, o->src.z, 1, 16};
            u64 row = (u64)((o->src.wb + 63) / 64) * block_bytes(&o->src);
            vk_bl_range(&m, &n.slo, &hi);
            n.sspan = hi - n.slo;
            n.stiles = n.sspan / row * n.sgobs * block_bytes(&o->src);
        }
    }
    if (!o->dst.bl) {
        n.dlen = n.drun = (u64)o->dst.pitch * (lines - 1) + o->line_out;
        n.druns = 1;
    } else if (n.dv) {
        VkBlMove m = {o->dst.wb, o->dst.h, o->dst.bh, o->dst.bd, o->dst.oy, n.dv, o->dst.z, 1, 16};
        u32 g = (o->dst.wb + 63) / 64;
        u64 bb = block_bytes(&o->dst);
        vk_bl_range(&m, &n.dlo, &hi);
        side_cols(&o->dst, o->line_out, &n.dx0, &n.dgobs);
        n.dstride = (u64)g * bb;
        n.drows = (u32)((hi - n.dlo) / n.dstride);
        n.dlo += (u64)n.dx0 * bb;
        n.drun = (u64)n.dgobs * bb;
        n.druns = n.drows;
        if (n.dgobs == g) { /* whole rows of blocks: one run */
            n.drun *= n.drows;
            n.druns = 1;
        }
        n.dlen = n.drun * n.druns;
    }
    n.at_b = up(o->src.bl ? (u64)n.sgobs * 64 * lines : 0, 16);
    n.at_c = n.at_b + up(o->dst.bl ? (u64)n.dgobs * 64 * lines : 0, 16);
    /* the remap's lines at at_c and 16 bytes past them (a fill's input binds a word there, texconv.c); without the
       remap a copy needs its rows alone, so rows that fill the cap fit it, and one with no rows 16 bytes, as dma asks
       for the buffer all the same */
    n.scratch = n.at_c + (o->remap && (o->fill || o->shuffle) ? (u64)pc * lines + 16 : n.at_c ? 0 : 16);
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
    u32 pa = need->sgobs * 64, pb = need->dgobs * 64, pc = (u32)up(o->line_out, 4);
    int remap = o->remap && (o->fill || o->shuffle);
    u64 at_a = 0, at_b = need->at_b, at_c = need->at_c, sspan = need->sspan, dlen = need->dlen;
    u64 cap, sat = 0;
    VkBuffer sb = VK_NULL_HANDLE, scr;
    VkCommandBuffer cb;
    VkOwnRes* d;
    /* the destination's tiles as a surface of their own: drows rows of blocks, the lines from the first one's top */
    u32 dsh = 3 + o->dst.bh, dy = o->dst.oy & ((1u << dsh) - 1), dz = o->dst.z & ((1u << o->dst.bd) - 1);
    VkBlMove md = {pb, need->drows << dsh, o->dst.bh, o->dst.bd, dy, dv, dz, 1, pb};
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
    /* those bytes as they are, read straight into the buffer that becomes their owner, run by run */
    d = vk_own_res_new(dlen);
    for (u32 r = 0; d && r < need->druns; r++)
        if (!vk_guest_read_seen(o->dst.addr + need->dlo + r * need->dstride, need->drun, d->buf, r * need->drun)) {
            vk_own_res_unref(d);
            d = NULL;
        }
    if (!d) return dma_fail(o, "could not read its destination's", dlen);
    cb = vk_cmd();
    vk_all_barrier(cb);
    /* the lines in: the source's rows (block linear: their GOB columns taken apart, rows past its surface zero) */
    VkBuffer ib = sb;
    u64 iat = sat;
    u32 ip = o->src.pitch;
    int ok = 1;
    if (!o->fill && o->src.bl) {
        VkBlMove m = {pa, o->src.h, o->src.bh, o->src.bd, o->src.oy, sv, o->src.z, 1, pa, (o->src.wb + 63) / 64};
        /* sb holds its rows of blocks from sat: the columns from the block sx0 of the first */
        if (sv) ok = vk_bl_move(cb, &m, 0, sb, sat + need->sx0 * block_bytes(&o->src), scr, at_a);
        if (sv < lines) { /* zeros, from staging */
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
        iat = at_a + o->src.ox - need->sx0 * 64;
        ip = pa;
    } else if (o->fill) { /* nothing is read: any bound range */
        ib = scr;
        iat = at_c;
        ip = 0;
    }
    /* where the lines out go: the destination's rows (block linear: its tiles taken apart) */
    VkBuffer tb = d->buf;
    u64 tat = 0;
    u32 tp = o->dst.pitch, tl = dv;
    if (o->dst.bl) {
        ok = ok && vk_bl_move(cb, &md, 0, d->buf, 0, scr, at_b);
        tb = scr;
        tat = at_b + o->dst.ox - need->dx0 * 64;
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
                   ? vk_tex_dma_rows(o->dst.addr, o->dst.wb, o->dst.bh, o->dst.ox, o->dst.oy, o->line_out, dv, scr, tat,
                                     pb, hit, 8)
                   : 0;
    if (ok) { /* those bytes are the buffer's from now on, run by run of guest memory */
        for (u32 r = 0; r < need->druns; r++) {
            u64 at = o->dst.addr + need->dlo + r * need->dstride, off = 0;
            while (off < need->drun) {
                u64 cpu = 0, n = gmmu_run(at + off, need->drun - off, &cpu);
                if (!n) { /* an unmapped page takes no write */
                    n = 0x1000 - ((at + off) & 0xFFF);
                    if (n > need->drun - off) n = need->drun - off;
                } else
                    vk_own_publish(cpu, n, d, r * need->drun + off);
                off += n;
            }
            gpu_mem_written(at, need->drun);
        }
        vk_tex_dma_seen(hit, hits);
        g_dma.moved += (o->src.bl ? need->stiles : sspan) + dlen;
    } else
        dma_fail(o, "takes moves past the device's limits (said above) over", dlen);
    vk_own_res_unref(d);
    return ok;
}

u32 vk_band(const VkBands* b, u32 y, u32 left, u64* most) {
    u64 limit = b->cap / VK_PART_SHARE, m = b->need(b->user, y, left);
    u32 n = left, row = b->base + y;
    if (!y && !b->split && m <= b->cap) { /* all of them */
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
/* The scratch buffer stays as big as the most a copy asked of it, rounded up to a power of two (vk_bl_scratch): a copy
   whose scratch needs more than VK_PART_SHARE of the cap goes in bands too, each asking for that share at most */
static int dma_bands(const GpuDmaOp* o) {
    DmaNeeds whole = dma_needs(o);
    VkBands bands = {g_vk.alloc_cap,
                     o->dst.bl ? 8u << o->dst.bh : 1,
                     o->dst.bl ? o->dst.oy : 0,
                     dma_need,
                     o,
                     whole.scratch > g_vk.alloc_cap / VK_PART_SHARE};
    for (u32 y = 0, lines; y < o->lines; y += lines) {
        u64 most = 0;
        lines = vk_band(&bands, y, o->lines - y, &most);
        GpuDmaOp b = dma_band(o, y, lines);
        if (most > bands.cap) return dma_fail(&b, "needs more than an allocation may be:", most);
        DmaNeeds n = dma_needs(&b);
        if (!dma(&b, &n, o->fill || o->src.bl ? 0 : (u64)y * o->src.pitch)) return 0;
        if (lines < o->lines) g_dma.bands++;
        if (y + lines < o->lines) vk_frame_check(); /* the next band may record in the other slot (cmd.c) */
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
