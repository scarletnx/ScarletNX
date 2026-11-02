/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/video_core/engines/maxwell_dma.cpp, src/video_core/textures/decoders.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project */
/* A launch of the DMA copy engine done on the CPU in guest memory, as maxwell/dma.c did it before it went to the GPU:
   the reference the tests hold the renderer's dma_copy to (tests/vulkan_headless.c, tests/gpu_park.c). The runtime
   copies on the GPU (vk/dma.c, docs/GPU_OWNERSHIP.md), so this file isn't built into it. As Eden's MaxwellDMA splits
   CopyBlockLinearToPitch and CopyPitchToBlockLinear, an operand is read and written by its layout (pitch linear lines
   or block linear GOBs), with the remap between */
#include <stdlib.h>
#include <string.h>
#include "common/utilities/endian.h"
#include "dma_ref.h"
#include "nx/gpu/maxwell/gob.h"
#include "nx/gpu/maxwell/maxwell_dma_regs.h"

/* the bytes block linear addressing moves at once: the run of a GOB's row bl_offset keeps contiguous */
#define BL_RUN_BYTES 16u

/* the bytes an operand's lines span in guest memory: pitch linear, its lines pitch apart; block linear, every block
   of its slices up to z */
static u64 span_of(const GpuDmaSide* s, u32 lines, u32 line) {
    if (!s->bl) return (u64)s->pitch * (lines - 1) + line;
    u64 gobs_x = ((u64)s->wb + MAXWELL_GOB_SIZE_X - 1) >> MAXWELL_GOB_SIZE_X_SHIFT;
    u64 rows = ((((u64)s->h + MAXWELL_GOB_SIZE_Y - 1) >> MAXWELL_GOB_SIZE_Y_SHIFT) + (1u << s->bh) - 1) >> s->bh;
    return gobs_x * rows * ((u64)MAXWELL_GOB_SIZE << (s->bh + s->bd)) * ((u64)(s->z >> s->bd) + 1);
}
/* the byte offset of line y's byte x of a block linear operand, from its origin */
static u64 bl_line_offset(const GpuDmaSide* s, u32 y, u32 x) {
    return bl_offset(0, s->oy + y, s->z, s->wb, s->h, s->bh, s->bd) +
           bl_offset(s->ox + x, 0, 0, s->wb, s->h, s->bh, s->bd);
}
/* the bytes of a block linear line from x that stay contiguous: up to the end of its 16-byte run */
static u32 bl_run(const GpuDmaSide* s, u32 x, u32 line) {
    u32 k = BL_RUN_BYTES - ((s->ox + x) & (BL_RUN_BYTES - 1));
    return k > line - x ? line - x : k;
}
/* lines of line bytes of a block linear operand into buf (line bytes apart); lines past the surface read as 0 */
static void read_block_linear(const GpuDmaSide* s, u32 lines, u32 line, u8* buf) {
    u32 y, x, k;
    for (y = 0; y < lines; y++) {
        u8* d = buf + (u64)y * line;
        if (s->oy + y >= s->h) {
            memset(d, 0, line);
            continue;
        }
        for (x = 0; x < line; x += k) {
            k = bl_run(s, x, line);
            gmem_read_raw(s->addr + bl_line_offset(s, y, x), d + x, k);
        }
    }
}
/* lines of line bytes of an operand into buf (line bytes apart). from: a copy of a pitch linear operand */
static void ref_read(const GpuDmaSide* s, u32 lines, u32 line, const u8* from, u8* buf) {
    u32 y;
    if (from) {
        for (y = 0; y < lines; y++) memcpy(buf + (u64)y * line, from + (u64)y * s->pitch, line);
        return;
    }
    if (s->bl) {
        read_block_linear(s, lines, line, buf);
        return;
    }
    for (y = 0; y < lines; y++) gmem_read_raw(s->addr + (u64)y * s->pitch, buf + (u64)y * line, line);
}
/* buf's lines into an operand, by its layout; block linear lines past the surface are dropped */
static void ref_write(const GpuDmaSide* s, u32 lines, u32 line, const u8* buf) {
    u32 y, x, k;
    for (y = 0; y < lines; y++) {
        const u8* src = buf + (u64)y * line;
        if (!s->bl) {
            gmem_write_raw(s->addr + (u64)y * s->pitch, src, line);
            continue;
        }
        if (s->oy + y >= s->h) break;
        for (x = 0; x < line; x += k) {
            k = bl_run(s, x, line);
            gmem_write_raw(s->addr + bl_line_offset(s, y, x), src + x, k);
        }
    }
    gpu_mem_written(s->addr, span_of(s, lines, line));
}
/* the remap (Eden's RemapConst): each destination component from a source component, constant A or B, or left as
   it is (NO_WRITE). A source component the source doesn't have writes 0 */
static void ref_remap(const GpuDmaOp* o, const u8* in, u8* out) {
    u64 i, n = (u64)o->width * o->lines;
    u32 k;
    for (i = 0; i < n; i++) {
        const u8* s = in ? in + i * o->cs * o->ns : NULL;
        u8* d = out + i * o->cs * o->nd;
        for (k = 0; k < o->nd; k++) {
            u32 sel = o->sel[k];
            if (sel == REMAP_SWIZZLE_CONST_A || sel == REMAP_SWIZZLE_CONST_B) {
                u8 c[4]; /* the constant's low bytes, as guest memory orders them */
                snx_st_le32(c, sel == REMAP_SWIZZLE_CONST_A ? o->const_a : o->const_b);
                memcpy(d + k * o->cs, c, o->cs);
            } else if (sel <= REMAP_SWIZZLE_SRC_W) {
                if (s && sel < o->ns)
                    memcpy(d + k * o->cs, s + sel * o->cs, o->cs);
                else
                    memset(d + k * o->cs, 0, o->cs);
            }
        }
    }
}
/* the launch: the source read (unless a fill), remapped over the destination's own bytes when a component keeps
   them, and written */
void dma_copy_ref(const GpuDmaOp* o) {
    u64 in_bytes = o->fill ? 0 : (u64)o->line_in * o->lines;
    u64 out_bytes = o->remap && (o->fill || o->shuffle) ? (u64)o->line_out * o->lines : 0;
    u8 *buf = (u8*)malloc((size_t)(in_bytes + out_bytes) + 1), *out;
    if (!buf) return;
    if (!o->fill) ref_read(&o->src, o->lines, o->line_in, o->snap, buf);
    out = buf;
    if (out_bytes) {
        out = buf + in_bytes;
        if (o->keep)
            ref_read(&o->dst, o->lines, o->line_out, NULL, out);
        else
            memset(out, 0, (size_t)out_bytes);
        ref_remap(o, o->fill ? NULL : buf, out);
    }
    ref_write(&o->dst, o->lines, o->line_out, out);
    free(buf);
}
