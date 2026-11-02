/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An NVDEC picture written into its surfaces in guest memory on the CPU: the reference the tests hold the renderer's
   nvdec_write to (tests/nvdec_packets.c, tests/nvdec_vp9_vectors.c, tests/vulkan_headless.c). The runtime lays every
   picture out on the GPU (vk/nvdec.c, docs/GPU_OWNERSHIP.md), so this file isn't built into it */
#include <string.h>
#include "nvdec_ref.h"
#include "nx/memory/mem.h"

/* NV12 planes, block linear. The layout's offset is separable, offset(x, y) = offset(x, 0) + offset(0, y), so the
   columns of 16-byte runs are walked once and each row once */
void nvdec_write_ref(const NvdecPic* p) {
    u64 col[4096 / 16];
    u8 line[4096 + 2], *py = GP(p->luma), *pc = GP(p->chroma);
    u32 w = p->w, h = p->h, pairs = (w + 1) / 2, cw = 2 * pairs, ch = (h + 1) / 2, runs = (cw + 15) / 16, i, x, y;
    if (p->pitch_linear) {
        for (y = 0; y < h; ++y)
            memcpy(py + (u64)y * p->pitch_y, p->plane[0] + (size_t)y * (size_t)p->stride[0], w);
        for (y = 0; y < ch; ++y)
            for (x = 0; x < pairs; ++x) {
                pc[(u64)y * p->pitch_c + 2 * x] = p->plane[1][(size_t)y * (size_t)p->stride[1] + x];
                pc[(u64)y * p->pitch_c + 2 * x + 1] = p->plane[2][(size_t)y * (size_t)p->stride[2] + x];
            }
        return;
    }
    for (i = 0; i < runs; i++) col[i] = bl_offset(16 * i, 0, 0, p->pitch_y, p->surf_h, p->bh, 0);
    for (y = 0; y < h; y++) {
        u64 row = bl_offset(0, y, 0, p->pitch_y, p->surf_h, p->bh, 0);
        const u8* src = p->plane[0] + (size_t)y * (size_t)p->stride[0];
        for (i = 0, x = 0; x < w; i++, x += 16) memcpy(py + row + col[i], src + x, w - x < 16 ? w - x : 16);
    }
    for (y = 0; y < ch; y++) {
        u64 row = bl_offset(0, y, 0, p->pitch_c, (p->surf_h + 1) / 2, p->bh, 0);
        const u8* u = p->plane[1] + (size_t)y * (size_t)p->stride[1];
        const u8* v = p->plane[2] + (size_t)y * (size_t)p->stride[2];
        for (x = 0; x < pairs; x++) {
            line[2 * x] = u[x];
            line[2 * x + 1] = v[x];
        }
        for (i = 0, x = 0; x < cw; i++, x += 16) memcpy(pc + row + col[i], line + x, cw - x < 16 ? cw - x : 16);
    }
}
