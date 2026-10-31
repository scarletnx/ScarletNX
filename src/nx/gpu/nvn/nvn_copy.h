/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's copies: NVN's buffer and texture copies, and the CPU's texel writes, as the DMA engine's launches the
   renderer does on the GPU (gpu/nvn/nvn_copy.c) */
#ifndef SNX_NVN_COPY_H
#define SNX_NVN_COPY_H
#include "common/cpu/aarch64/cpu.h"
#include "common/filesystem/host.h"

/* the columns and rows a texture blit's tables hold: NVN's largest texture's (16384 texels a side) */
#define NVN_BLIT_MAX 16384
/* the file's state, g_nvn.copy (gpu/nvn/nvn_state.h): the syncpoint the texel reads wait on, made at the first, under
   its lock; and the GPU thread's tables of a texture blit, each column's source column then each row's source row */
typedef struct {
    PMutex read_lock;
    u32 read_syncpt;
    u32 blit[2 * NVN_BLIT_MAX];
} NvnCopyState;

/* On the GPU thread, a copy record a queue replays (OP_COPY_*, OP_CLEAR_BUF). stride: the queue's copy row and image strides
   (SetCopyRowStride and SetCopyImageStride, 0 for tightly packed), which OP_COPY_STRIDE sets. 0 when the record is
   no copy */
int nvn_copy_record(u32 op, u64 payload, u32 size, i64 stride[2]);
/* nvnTextureWriteTexelsStrided of texture t (its view v, 0 for none) on the game's thread: the region's texels from
   the guest memory at src (rows row bytes apart, slices image bytes apart, 0 for tightly packed), read now and
   written into the texture's storage on the GPU, in order with the queues' work */
void nvn_write_texels(u64 t, u64 v, u64 region, u64 src, i64 row, i64 image);
/* nvnTextureReadTexelsStrided on the game's thread: the region's texels copied out of the texture by the GPU into the
   guest memory at dst (rows row bytes apart, slices image bytes apart, 0 for tightly packed), waited for */
void nvn_read_texels(u64 t, u64 v, u64 region, u64 dst, i64 row, i64 image);

#endif
