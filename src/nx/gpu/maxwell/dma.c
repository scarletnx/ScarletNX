/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_dma.cpp, src/video_core/engines/maxwell_dma.h,
   src/video_core/textures/decoders.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* The copy engine (MAXWELL_DMA_COPY_A, 0xB0B5; open-gpu-doc's clb0b5.h): one-dimensional copies, and multi-line
   copies between pitch linear and block linear surfaces, with the remap of components and its constant fills, and
   the completion semaphore, done on the GPU (vk/dma.c). Also the copies of linear sources read at submission
   (dma_snap_*) */
#include "nx/runtime/state_io.h"
#include "nx/gpu/maxwell/engines_int.h"

#define DMA_MAX_BYTES (256u << 20) /* a launch past this is a bad register value */

/* ------------------------------------------------------------ a launch's geometry */
/* a launch is a GpuDmaOp (gpu.h): the renderer does it on the GPU */
typedef GpuDmaSide DmaSide;
typedef GpuDmaOp DmaOp;
static void dma_side(DmaSide* s, const u32* r, u64 addr, int bl, u32 pitch, u32 params, u32 elem) {
    const u32* p = r + params;
    s->addr = addr;
    s->bl = bl;
    s->pitch = pitch;
    s->bh = (p[0] >> 4) & 15;
    s->bd = (p[0] >> 8) & 15;
    s->wb = p[1] * elem;
    s->h = p[2];
    s->z = p[4];
    s->ox = (p[5] & 0xFFFF) * elem;
    s->oy = p[5] >> 16;
}
static void dma_geometry(const u32* r, u32 v, DmaOp* o) {
    u32 k, elem_in = 1, elem_out = 1, c = r[0x1C2];
    memset(o, 0, sizeof *o);
    o->multi = (v >> 9) & 1;
    o->remap = (v >> 10) & 1;
    o->width = r[0x106];
    o->lines = o->multi ? r[0x107] : 1;
    if (o->remap) {
        o->cs = ((c >> 16) & 3) + 1;
        o->ns = ((c >> 20) & 3) + 1;
        o->nd = ((c >> 24) & 3) + 1;
        elem_in = o->cs * o->ns;
        elem_out = o->cs * o->nd;
        o->fill = 1;
        for (k = 0; k < o->nd; k++) {
            o->sel[k] = (c >> (4 * k)) & 7;
            if (o->sel[k] < 4 || o->sel[k] > 5) o->fill = 0; /* CONST_A and CONST_B only: nothing is read */
            if (o->sel[k] != k) o->shuffle = 1;
            if (o->sel[k] >= 6) o->keep = 1; /* NO_WRITE: the destination's own bytes stay */
        }
        if (o->ns != o->nd) o->shuffle = 1;
    }
    o->line_in = o->width * elem_in;
    o->line_out = o->width * elem_out;
    o->const_a = r[0x1C0];
    o->const_b = r[0x1C1];
    /* a single line is one-dimensional, as Eden and Ryujinx copy it, whatever the layouts */
    dma_side(&o->src, r, ((u64)(r[0x100] & 0xFF) << 32) | r[0x101], o->multi && !((v >> 7) & 1), r[0x104], 0x1CA,
             elem_in);
    dma_side(&o->dst, r, ((u64)(r[0x102] & 0xFF) << 32) | r[0x103], o->multi && !((v >> 8) & 1), r[0x105], 0x1C3,
             elem_out);
}
/* the bytes of a pitch linear operand's lines */
static u64 dma_pitch_span(const DmaSide* s, u32 lines, u32 line) { return (u64)s->pitch * (lines - 1) + line; }

/* ------------------------------------------------------------ copies of linear sources made at submission */
/* The console's copy engine reads a source microseconds after the submission, while the GPU thread may run the copy
   frames later, and games free a copy's source right after submitting it (Catherine frees its texture uploads'
   staging at the start of the next frame, and the piano's keys reflected garbage). So a launch with a linear source
   has its source read at submission, in the channel's order; the copy uses it unless the GPU has written the source
   since (guest memory then has the newer data) */
#define SNAP_RING 64
#define SNAP_TOTAL (256u << 20) /* bytes held at most across the channels: past it copies read memory */
typedef struct {
    u64 job, src, len, gen;
    u32 launch;
    u8* data;
} DmaSnap;
typedef struct {
    DmaSnap e[SNAP_RING];
    u32 head, tail;
} DmaSnapRing;
static DmaSnapRing* g_snaps[128];
static u64 g_snap_bytes;
static PMutex g_snap_lk = PMUTEX_INIT;
static void snap_drop(DmaSnapRing* q) { /* the oldest, the lock held */
    DmaSnap* e = &q->e[q->tail++ % SNAP_RING];
    g_snap_bytes -= e->len;
    free(e->data);
    e->data = NULL;
}
static int dma_linear_source(const DmaOp* o) { return !o->fill && !o->src.bl && o->width && o->lines; }
void dma_snap_launch(u32 channel, u64 job_id, u32 launch_index, const u32* r, u32 v) {
    DmaOp o;
    DmaSnapRing* q;
    DmaSnap* e;
    u64 len, gen;
    u8* data;
    if (channel >= 128) return;
    dma_geometry(r, v, &o);
    if (!dma_linear_source(&o)) return;
    len = dma_pitch_span(&o.src, o.lines, o.line_in);
    pm_lock(&g_snap_lk);
    if (!g_snaps[channel]) g_snaps[channel] = (DmaSnapRing*)calloc(1, sizeof(DmaSnapRing));
    q = g_snaps[channel];
    if (!q || q->head - q->tail == SNAP_RING || len > SNAP_TOTAL - g_snap_bytes) {
        pm_unlock(&g_snap_lk);
        return;
    }
    g_snap_bytes += len;
    pm_unlock(&g_snap_lk);
    gen = gpu_mem_generation(); /* before the read: a GPU write after it shows */
    data = (u8*)malloc((size_t)len);
    if (data) gmem_read_raw(o.src.addr, data, len);
    pm_lock(&g_snap_lk);
    if (!data) {
        g_snap_bytes -= len;
        pm_unlock(&g_snap_lk);
        return;
    }
    e = &q->e[q->head++ % SNAP_RING];
    e->job = job_id;
    e->launch = launch_index;
    e->src = o.src.addr;
    e->len = len;
    e->gen = gen;
    e->data = data;
    pm_unlock(&g_snap_lk);
}
/* the copy of launch (job, launch) when there is one for this source, taken over by the caller; older ones go */
static u8* snap_take(u32 channel, u64 job, u32 launch, u64 src, u64 len, u64* gen) {
    DmaSnapRing* q;
    u8* data = NULL;
    if (channel >= 128) return NULL;
    pm_lock(&g_snap_lk);
    q = g_snaps[channel];
    while (q && q->tail != q->head) {
        DmaSnap* e = &q->e[q->tail % SNAP_RING];
        if (e->job < job || (e->job == job && e->launch < launch)) { /* stale */
            snap_drop(q);
            continue;
        }
        if (e->job == job && e->launch == launch) {
            if (e->src != src || e->len != len) {
                WARN_LIMIT(4, "gpu: the copy of DMA source %llx (%llu bytes) doesn't match its launch (%llx, %llu)\n",
                           (unsigned long long)e->src, (unsigned long long)e->len, (unsigned long long)src,
                           (unsigned long long)len);
                snap_drop(q);
                break;
            }
            data = e->data;
            *gen = e->gen;
            e->data = NULL;
            g_snap_bytes -= e->len;
            q->tail++;
        }
        break;
    }
    pm_unlock(&g_snap_lk);
    return data;
}
void dma_snap_finish(u32 channel, u64 job_id) {
    DmaSnapRing* q;
    if (channel >= 128) return;
    pm_lock(&g_snap_lk);
    q = g_snaps[channel];
    while (q && q->tail != q->head && q->e[q->tail % SNAP_RING].job <= job_id) snap_drop(q);
    pm_unlock(&g_snap_lk);
}
void dma_snap_reset(u32 channel) {
    DmaSnapRing* q;
    if (channel >= 128) return;
    pm_lock(&g_snap_lk);
    q = g_snaps[channel];
    g_snaps[channel] = NULL;
    while (q && q->tail != q->head) snap_drop(q);
    pm_unlock(&g_snap_lk);
    free(q);
}

/* ------------------------------------------------------------ the copy */
/* on the GPU, its destination kept there (vk/dma.c); a render target over the destination takes the new bytes */
static void dma_copy(const DmaOp* o, const u8* from) {
    DmaOp op = *o;
    u64 in_bytes = o->fill ? 0 : (u64)o->line_in * o->lines, out_bytes = (u64)o->line_out * o->lines;
    if (in_bytes > DMA_MAX_BYTES || out_bytes > DMA_MAX_BYTES) {
        WARN_LIMIT(4, "gpu: a DMA copy of %u lines of %u elements is over %u MiB: nothing is copied\n", o->lines,
                   o->width, DMA_MAX_BYTES >> 20);
        return;
    }
    op.snap = from;
    if (!g_renderer_up || !g_renderer->dma_copy) {
        WARN_LIMIT(4, "gpu: a DMA copy before the renderer started, skipped\n");
        return;
    }
    if (!g_renderer->dma_copy(&op)) WARN_LIMIT(4, "gpu: the GPU could not do a DMA copy, skipped\n");
    if (g_renderer->rt_reload) g_renderer->rt_reload(o->dst.addr);
}
/* LAUNCH_DMA (0xC0) */
static void dma_launch(GpuChannel* ch, u32 v) {
    const u32* r = ch->rdma;
    u32 launch = ch->dma_launch_index++, sem = (v >> 3) & 3;
    DmaOp o;
    dma_geometry(r, v, &o);
    if (o.width && o.lines) {
        u8* snap = NULL;
        u64 gen = 0, len = dma_pitch_span(&o.src, o.lines, o.line_in);
        if (dma_linear_source(&o)) {
            snap = snap_take(ch->id, ch->dma_job_id, launch, o.src.addr, len, &gen);
            if (snap) {
                gmem_need(o.src.addr, len); /* pending GPU results over the source land, and count as writes */
                if (gpu_mem_dirty_since(gen, o.src.addr, len)) {
                    free(snap);
                    snap = NULL;
                }
            }
        }
        dma_copy(&o, snap);
        free(snap);
    }
    /* the completion semaphore, after the copy: one word, or four (the payload, 0, and the timestamp) */
    if (sem == 1 || sem == 2) {
        u64 a = ((u64)(r[0x90] & 0xFF) << 32) | r[0x91];
        u8 w[16]; /* guest memory's bytes, little-endian */
        snx_st_le32(w, r[0x92]);
        snx_st_le32(w + 4, 0);
        if (sem == 2) snx_st_le64(w + 8, gpu_ticks());
        gpu_release_write(a, w, sem == 1 ? 4 : 16);
    }
}
void dma_method(GpuChannel* ch, u32 m, u32 v) {
    if (m >= 0x200) return;
    ch->rdma[m] = v;
    if (m == 0xC0) dma_launch(ch, v);
}

/* ------------------------------------------------------------ save states */
/* the snapshots still to be used, ring by ring in order: the jobs a load puts back in the queue take them */
int dma_snap_save(StateIo* io) {
    u32 c, i;
    int r = 0;
    pm_lock(&g_snap_lk);
    for (c = 0; c < 128 && !r; c++) {
        DmaSnapRing* q = g_snaps[c];
        if (!q || q->tail == q->head) continue;
        r |= io_put32(io, c + 1) | io_put32(io, q->head - q->tail);
        for (i = q->tail; i != q->head && !r; i++) {
            const DmaSnap* e = &q->e[i % SNAP_RING];
            r |= io_put64(io, e->job) | io_put64(io, e->src) | io_put64(io, e->len) | io_put32(io, e->launch);
            r |= io_put(io, e->data, e->len);
        }
    }
    r |= io_put32(io, 0);
    pm_unlock(&g_snap_lk);
    return r;
}
int dma_snap_load(StateIo* io) {
    u32 c, n, i;
    u64 gen = gpu_mem_generation(); /* the guest memory they were read from is the state's, as new as any */
    for (c = 0; c < 128; c++) dma_snap_reset(c);
    for (;;) {
        DmaSnapRing* q;
        if (io_get32(io, &c)) return -1;
        if (!c) return 0;
        if (c > 128 || io_get32(io, &n) || n > SNAP_RING) return -1;
        q = (DmaSnapRing*)calloc(1, sizeof *q);
        if (!q) return -1;
        for (i = 0; i < n; i++) {
            DmaSnap* e = &q->e[i];
            if (io_get64(io, &e->job) || io_get64(io, &e->src) || io_get64(io, &e->len) || io_get32(io, &e->launch) ||
                e->len > SNAP_TOTAL || !(e->data = (u8*)malloc(e->len ? (size_t)e->len : 1)) ||
                io_get(io, e->data, e->len)) {
                q->head = i + 1;
                pm_lock(&g_snap_lk);
                while (q->tail != q->head) snap_drop(q);
                pm_unlock(&g_snap_lk);
                free(q);
                return -1;
            }
            e->gen = gen;
            q->head = i + 1;
        }
        pm_lock(&g_snap_lk);
        g_snaps[c - 1] = q;
        for (i = 0; i < n; i++) g_snap_bytes += q->e[i].len;
        pm_unlock(&g_snap_lk);
    }
}
