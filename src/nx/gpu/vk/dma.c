/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The DMA copy engine on the GPU (maxwell/dma.c hands a launch over), its destination kept there
   (docs/GPU_OWNERSHIP.md). The destination's span is copied as it is now (guest bytes, what the GPU holds of them,
   render targets drawn over them) into a buffer of its own; the source's span is staged the same way (or from the
   copy made at submission). Block linear sides are taken apart into rows (blocklinear.comp) and the lines go between
   rows: copied, or remapped component by component (texconv.comp), exactly a line's bytes each, so what surrounds
   them keeps its bytes. The destination's rows go back into its layout, and the buffer is published over its span.
   Under vk_lock, in the frame's command buffer: the semaphore after the copy is the renderer's queued release, which
   comes after it */
#include <stdlib.h>
#include <string.h>
#include "nx/gpu/vk/vk.h"
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
    u64 bytes; /* held, for the test */
} g_snap = {PMUTEX_INIT, {{0}}, 0};

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
    if (!VK_OK(vmaCreateBuffer(g_vk.vma, &bci, &aci, &c->buf, &c->mem, &info))) return 0;
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
    pm_lock(&g_snap.lk);
    for (u32 i = 0; i < SNAP_CHUNKS && !p; i++) {
        SnapChunk* c = &g_snap.c[i];
        if (!c->buf && !snap_chunk_new(c)) break;
        if (vmaVirtualAllocate(c->block, &vai, &m->a, &m->off) == VK_SUCCESS) { /* else the next chunk */
            m->chunk = i;
            m->len = len;
            p = c->ptr + m->off;
            g_snap.bytes += len;
        }
    }
    pm_unlock(&g_snap.lk);
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
    pm_lock(&g_snap.lk);
    vmaVirtualFree(g_snap.c[m->chunk].block, m->a);
    g_snap.bytes -= m->len;
    pm_unlock(&g_snap.lk);
    free(m);
}
static void snap_free_later(void* m) { vk_snap_free((u64)(uintptr_t)m); }
u64 vk_snap_bytes(void) {
    u64 b;
    pm_lock(&g_snap.lk);
    b = g_snap.bytes;
    pm_unlock(&g_snap.lk);
    return b;
}

/* the bytes a side's lines lie in: a pitch linear side's lines, a block linear one's surface up to its slice */
static u64 side_span(const GpuDmaSide* s, u32 lines, u32 line) {
    if (!s->bl) return (u64)s->pitch * (lines - 1) + line;
    u64 gobs_x = ((u64)s->wb + 63) >> 6, rows = ((((u64)s->h + 7) >> 3) + (1u << s->bh) - 1) >> s->bh;
    return gobs_x * rows * ((u64)512 << (s->bh + s->bd)) * ((u64)(s->z >> s->bd) + 1);
}
/* the rows of a block linear side's lines that lie in its surface */
static u32 side_rows(const GpuDmaSide* s, u32 lines) {
    return s->oy >= s->h ? 0 : s->h - s->oy < lines ? s->h - s->oy : lines;
}
/* guest bytes [gva, gva + span) staged as the GPU sees them: what the CPU holds, what the GPU holds (copied on the
   GPU), render targets drawn over them */
static u8* stage_range(u64 gva, u64 span, VkBuffer* buf, u64* at) {
    u8* p = vk_stage(span, vk_stage_align(), buf, at);
    if (!p) return NULL;
    vk_guest_read(gva, span, p, *buf, *at);
    vk_rt_overlay(gva, span, *buf, *at);
    return p;
}

static int dma(const GpuDmaOp* o) {
    u32 lines = o->lines, sv = o->src.bl ? side_rows(&o->src, lines) : lines;
    u32 dv = o->dst.bl ? side_rows(&o->dst, lines) : lines;
    u64 sspan = o->fill ? 0 : side_span(&o->src, lines, o->line_in), dspan = side_span(&o->dst, lines, o->line_out);
    u32 pa = (u32)up(o->src.wb, 16), pb = (u32)up(o->dst.wb, 16), pc = (u32)up(o->line_out, 4);
    int remap = o->remap && (o->fill || o->shuffle);
    /* the scratch rows: the source's (block linear), the destination's (block linear), the remapped lines */
    u64 at_a = 0, at_b = up(o->src.bl ? (u64)pa * lines : 0, 16), at_c = at_b + up(o->dst.bl ? (u64)pb * lines : 0, 16);
    u64 scratch_size = at_c + (remap ? (u64)pc * lines : 0) + 16, cap, sat = 0, dat;
    VkBuffer sb = VK_NULL_HANDLE, db, scr;
    VkCommandBuffer cb;
    VkOwnRes* d;
    vk_render_end();
    if (dspan > g_vk.alloc_cap || sspan > g_vk.alloc_cap || !(scr = vk_bl_scratch(scratch_size, &cap))) return 0;
    if (!o->fill) {
        u8* p;
        if (o->snap_id) { /* the source as it was at submission, where it was read into */
            const SnapMem* m = (const SnapMem*)(uintptr_t)o->snap_id;
            const SnapChunk* c = &g_snap.c[m->chunk]; /* chunks stay once made */
            if (!VK_OK(vmaFlushAllocation(g_vk.vma, c->mem, m->off, sspan))) return 0;
            p = c->ptr + m->off;
            sb = c->buf;
            sat = m->off;
        } else if (o->snap) { /* the same in the submitting thread's memory */
            p = vk_stage(sspan, vk_stage_align(), &sb, &sat);
            if (p) memcpy(p, o->snap, sspan);
        } else
            p = stage_range(o->src.addr, sspan, &sb, &sat);
        if (!p) return 0;
    }
    d = vk_own_res_new(dspan);
    if (!d || !stage_range(o->dst.addr, dspan, &db, &dat)) {
        if (d) vk_own_res_unref(d);
        return 0;
    }
    cb = vk_cmd();
    {
        VkBufferCopy c = {dat, 0, dspan};
        rec_CmdCopyBuffer(cb, db, d->buf, 1, &c);
    }
    vk_all_barrier(cb);
    /* the lines in: the source's rows (block linear: taken apart, rows past its surface zero) */
    VkBuffer ib = sb;
    u64 iat = sat;
    u32 ip = o->src.pitch;
    int ok = 1;
    if (!o->fill && o->src.bl) {
        VkBlMove m = {o->src.wb, o->src.h, o->src.bh, o->src.bd, o->src.oy, sv, o->src.z, 1, pa};
        u64 lo, hi;
        vk_bl_range(&m, &lo, &hi);
        if (sv) ok = vk_bl_move(cb, &m, 0, sb, sat + lo, scr, at_a);
        if (sv < lines) { /* zeros, from staging */
            VkBuffer zb;
            u64 zat;
            u8* z = vk_stage((u64)pa * (lines - sv), vk_stage_align(), &zb, &zat);
            if (!z) {
                vk_own_res_unref(d);
                return 0;
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
    VkBlMove md = {o->dst.wb, o->dst.h, o->dst.bh, o->dst.bd, o->dst.oy, dv, o->dst.z, 1, pb};
    u64 dlo = 0, dhi;
    if (o->dst.bl) {
        vk_bl_range(&md, &dlo, &dhi);
        if (dv) ok = ok && vk_bl_move(cb, &md, 0, d->buf, dlo, scr, at_b);
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
        if (ok && o->dst.bl && dv) ok = vk_bl_move(cb, &md, 1, d->buf, dlo, scr, at_b);
        vk_all_barrier(cb);
    }
    if (ok) { /* the destination's span is the buffer's from now on, run by run of guest memory */
        u64 off = 0;
        while (off < dspan) {
            u64 cpu = 0, n = gmmu_run(o->dst.addr + off, dspan - off, &cpu);
            if (!n) { /* an unmapped page takes no write */
                n = 0x1000 - ((o->dst.addr + off) & 0xFFF);
                if (n > dspan - off) n = dspan - off;
            } else
                vk_own_publish(cpu, n, d, off);
            off += n;
        }
        gpu_mem_written(o->dst.addr, dspan);
    } else
        LOG_LIMIT(4, "gpu: a DMA copy of %u lines of %u bytes exceeds the device's limits: skipped\n", lines,
                  o->line_out);
    vk_own_res_unref(d);
    return ok;
}
int vk_dma_copy(const GpuDmaOp* o) {
    int r;
    if (!o->lines || !o->line_out) {
        vk_snap_free(o->snap_id);
        return 1;
    }
    vk_lock();
    r = dma(o);
    if (o->snap_id) vk_defer_call(snap_free_later, (void*)(uintptr_t)o->snap_id); /* after its frame's work */
    vk_unlock();
    return r;
}
