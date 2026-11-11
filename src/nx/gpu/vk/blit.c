/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 2D engine's blits that the GPU's own image blit can't take (a source that isn't a render target), done on the
   GPU from memory, the destination kept there (docs/layers/gpu/ownership.md). maxwell/fermi2d.c works out which source pixel
   each destination column and row takes (nearest sampled, scaled); here the source rows the rectangle samples are
   staged as the GPU sees them (guest bytes, what the GPU holds, render targets drawn over them) and taken apart into
   rows, the destination rectangle's rows are gathered through the tables (texconv.comp), a row of the rectangle a
   word an invocation, the components converted between float formats when the blit says so (TC_BLIT_FLOAT), and
   copied into the destination's rows, exactly the rectangle's bytes, which go back into its layout and are published
   over their span. A blit bigger than one allocation goes in bands of the rectangle's rows (blit_bands).
   Under vk_lock, in the frame's command buffer */
#include <string.h>
#include "nx/gpu/vk/vk.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"

static u64 up(u64 v, u64 a) { return (v + a - 1) / a * a; }

/* guest bytes [gva, gva + span) as the GPU sees them, in the frame's device memory, for a side that is only read */
static int stage_range(u64 gva, u64 span, VkBuffer* buf, u64* at) {
    return vk_stage_device(span, vk_stage_align(), buf, at) && vk_guest_read_seen(gva, span, *buf, *at);
}

/* whether b's tables hold positions (floats) its pixels are filtered between, not pixels (GPU_BLIT_LINEAR_BIT) */
static int linear(const GpuBlit* b) { return (b->fmt & GPU_BLIT_CONVERT_BIT) && (b->fmt & GPU_BLIT_LINEAR_BIT); }
/* the source row the rectangle's row i starts on, and the rows it reads: a filtered one between two reads both */
static u32 table_row(const GpuBlit* b, u32 i, u32* reads) {
    float f;
    u32 r;
    *reads = 1;
    if (!linear(b)) return b->srow[i];
    memcpy(&f, &b->srow[i], sizeof f);
    r = (u32)f;
    if (f != (float)r) *reads = 2;
    return r;
}

/* What the band of the rectangle's rows [y, y + n) reads and writes: the source rows its rows sample, sr0 to sr1 - 1,
   whose bytes are from src.addr + slo (sspan of them), and the destination's from dst.addr + dlo (dlen), each a pitch
   linear side's rows or a block linear one's rows of blocks, as the rest of a plane keeps its bytes and their owner;
   and the scratch: the source's rows at 0, the tables at at_t, the gathered rows at at_b, the destination's rows
   (block linear) at at_c, used bytes of it */
typedef struct {
    u32 sr0, sr1;
    u64 slo, sspan, dlo, dlen, at_t, at_b, at_c, used;
} BlitNeeds;
static BlitNeeds blit_needs(const GpuBlit* b, u32 y, u32 n) {
    BlitNeeds k = {0};
    u32 w = b->x1 - b->x0, bd = GPU_BLIT_DST_BPP(b), nb = w * bd;
    u32 pa = b->src.bl ? (u32)up(b->src.wb, 16) : b->src.pitch, pb = (u32)up(nb, 4), pc = (u32)up(b->dst.wb, 16);
    u64 hi;
    k.sr0 = ~0u;
    for (u32 i = y; i < y + n; i++) {
        u32 reads, r = table_row(b, i, &reads);
        if (r < k.sr0) k.sr0 = r;
        if (r + reads > k.sr1) k.sr1 = r + reads;
    }
    if (b->src.bl) {
        VkBlMove m = {b->src.wb, b->src.h, b->src.bh, b->src.bd, k.sr0, k.sr1 - k.sr0, b->src.z, 1, pa};
        vk_bl_range(&m, &k.slo, &hi);
        k.sspan = hi - k.slo;
    } else {
        k.slo = (u64)k.sr0 * b->src.pitch;
        k.sspan = (u64)(k.sr1 - k.sr0 - 1) * b->src.pitch + b->src.wb;
    }
    if (b->dst.bl) {
        VkBlMove m = {b->dst.wb, b->dst.h, b->dst.bh, b->dst.bd, b->y0 + y, n, b->dst.z, 1, pc};
        vk_bl_range(&m, &k.dlo, &hi);
        k.dlen = hi - k.dlo;
    } else {
        k.dlo = (u64)(b->y0 + y) * b->dst.pitch + (u64)b->x0 * bd;
        k.dlen = (u64)(n - 1) * b->dst.pitch + nb;
    }
    k.at_t = up(b->src.bl ? (u64)pa * (k.sr1 - k.sr0) : k.sspan, 16);
    k.at_b = up(k.at_t + 4ull * (w + n), 16);
    k.at_c = up(k.at_b + (u64)pb * n, 16);
    k.used = k.at_c + (b->dst.bl ? (u64)pc * n : 0) + 16;
    return k;
}

/* the band of rows from y (n of them) that could not be blitted, the bytes it lacked them for and where they go */
static int blit_fail(const GpuBlit* b, u32 y, u32 n, const char* what, u64 bytes) {
    LOG_LIMIT(8, "vulkan: a 2D blit of %ux%u pixels to %llx (rows %u to %u) %s %llu bytes: nothing is copied\n",
              b->x1 - b->x0, b->y1 - b->y0, (unsigned long long)b->dst.addr, b->y0 + y, b->y0 + y + n - 1, what,
              (unsigned long long)bytes);
    return 0;
}

/* the rectangle's rows [y, y + n) as need says */
static int blit(const GpuBlit* b, u32 y, u32 n, const BlitNeeds* need) {
    u32 w = b->x1 - b->x0, bd = GPU_BLIT_DST_BPP(b), nb = w * bd;
    u32 pa = b->src.bl ? (u32)up(b->src.wb, 16) : b->src.pitch, pb = (u32)up(nb, 4), pc = (u32)up(b->dst.wb, 16);
    u32 sr0 = need->sr0, srows = need->sr1 - need->sr0;
    u64 sgva = b->src.addr + need->slo, sspan = need->sspan, dlen = need->dlen, cap, sat, tat;
    u64 at_t = need->at_t, at_b = need->at_b, at_c = need->at_c, used = need->used;
    VkBuffer sb, tb, scr;
    VkCommandBuffer cb;
    VkOwnRes* d;
    int ok = 1;
    vk_render_end();
    if (!(scr = vk_bl_scratch(used, &cap))) return blit_fail(b, y, n, "found no scratch buffer of", used);
    u32* t = (u32*)vk_stage(4ull * (w + n), vk_stage_align(), &tb, &tat);
    if (!t) return blit_fail(b, y, n, "could not stage its tables,", 4ull * (w + n));
    /* a source no render target is drawn over: the buffer cache's copy, made again only when its bytes change */
    if (!(vk_rt_over(sgva, sspan) ? stage_range(sgva, sspan, &sb, &sat) : vk_buf_get_copy_src(sgva, sspan, &sb, &sat)))
        return blit_fail(b, y, n, "could not stage its source's", sspan);
    memcpy(t, b->scol, 4ull * w);
    for (u32 i = 0; i < n; i++) /* the rows from the band's first source row */
        if (linear(b)) {
            float f;
            memcpy(&f, &b->srow[y + i], sizeof f);
            f -= (float)sr0; /* exact: both are multiples of f's last bit */
            memcpy(&t[w + i], &f, sizeof f);
        } else
            t[w + i] = b->srow[y + i] - sr0;
    snx_le32_array(t, (size_t)w + n); /* the device reads the tables' words little-endian */
    d = vk_own_res_new(dlen);
    /* straight into the buffer that becomes its owner */
    if (!d || !vk_guest_read_seen(b->dst.addr + need->dlo, dlen, d->buf, 0)) {
        if (d) vk_own_res_unref(d);
        return blit_fail(b, y, n, "could not read its destination's", dlen);
    }
    cb = vk_cmd();
    {
        VkBufferCopy c[2] = {{tat, at_t, 4ull * (w + n)}, {sat, 0, sspan}};
        rec_CmdCopyBuffer(cb, tb, scr, 1, &c[0]);
        if (!b->src.bl) rec_CmdCopyBuffer(cb, sb, scr, 1, &c[1]); /* pitch linear rows as they are */
    }
    vk_all_barrier(cb);
    VkBlMove ms = {b->src.wb, b->src.h, b->src.bh, b->src.bd, sr0, srows, b->src.z, 1, pa};
    VkBlMove md = {b->dst.wb, b->dst.h, b->dst.bh, b->dst.bd, b->y0 + y, n, b->dst.z, 1, pc};
    if (b->src.bl) ok = vk_bl_move(cb, &ms, 0, sb, sat, scr, 0); /* sb holds its rows of blocks from sat */
    if (ok && b->dst.bl) ok = vk_bl_move(cb, &md, 0, d->buf, 0, scr, at_c);
    vk_all_barrier(cb);
    ok = ok && vk_tc_blit(cb, b->bpp, b->fmt, nb, n, scr, used, 0, pa, at_t, at_t + 4ull * w, scr, at_b, pb);
    vk_all_barrier(cb);
    if (ok) { /* the rectangle's bytes into the destination's rows, which go back into its layout */
        if (b->dst.bl)
            vk_copy_rows(cb, scr, at_b, pb, scr, at_c + (u64)b->x0 * bd, pc, nb, n);
        else
            vk_copy_rows(cb, scr, at_b, pb, d->buf, 0, b->dst.pitch, nb, n);
        vk_all_barrier(cb);
        if (b->dst.bl) ok = vk_bl_move(cb, &md, 1, d->buf, 0, scr, at_c);
        vk_all_barrier(cb);
    }
    if (ok) { /* those bytes are the buffer's from now on, run by run of guest memory */
        u64 off = 0, at = b->dst.addr + need->dlo;
        while (off < dlen) {
            u64 cpu = 0, k = gmmu_run(at + off, dlen - off, &cpu);
            if (!k) { /* an unmapped page takes no write */
                k = 0x1000 - ((at + off) & 0xFFF);
                if (k > dlen - off) k = dlen - off;
            } else
                vk_own_publish(cpu, k, d, off);
            off += k;
        }
        gpu_mem_written(at, dlen);
    } else
        blit_fail(b, y, n, "takes moves past the device's limits (said above) over", dlen);
    vk_own_res_unref(d);
    return ok;
}

/* A blit too big for one allocation goes in bands of the rectangle's rows (vk_band), each a blit of its own reading
   the source rows its rows sample, and so does one whose scratch needs more than VK_PART_SHARE of the cap, as the
   scratch buffer stays as big as the most asked of it. A block linear destination's bands end on its rows of blocks */
static u64 blit_need(const void* user, u32 y, u32 n) {
    BlitNeeds k = blit_needs((const GpuBlit*)user, y, n);
    u64 m = k.sspan > k.dlen ? k.sspan : k.dlen;
    return m > k.used ? m : k.used;
}
static int blit_bands(const GpuBlit* b) {
    u32 rows = b->y1 - b->y0;
    VkBands bands = {g_vk.alloc_cap,
                     b->dst.bl ? 8u << b->dst.bh : 1,
                     b->y0,
                     blit_need,
                     b,
                     blit_needs(b, 0, rows).used > g_vk.alloc_cap / VK_PART_SHARE};
    for (u32 y = 0, n; y < rows; y += n) {
        u64 most = 0;
        n = vk_band(&bands, y, rows - y, &most);
        if (most > bands.cap) return blit_fail(b, y, n, "needs more than an allocation may be:", most);
        BlitNeeds k = blit_needs(b, y, n);
        if (!blit(b, y, n, &k)) return 0;
        if (y + n < rows) vk_frame_check(); /* the next band may record in the other slot (cmd.c) */
    }
    return 1;
}
int vk_blit_mem(const GpuBlit* b) {
    int r;
    if (b->x1 <= b->x0 || b->y1 <= b->y0) return 1;
    vk_lock();
    r = blit_bands(b);
    vk_unlock();
    return r;
}
