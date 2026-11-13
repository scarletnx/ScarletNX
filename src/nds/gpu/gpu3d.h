/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_NDS_GPU3D_H
#define SNX_NDS_GPU3D_H
#include "nds/board/nds.h"
/* The 3D engine: its geometry commands run as the game writes them (gpu3d.c), and the polygons of a frame are drawn
   on the GPU (vk/render3d.c). */
bool nds_gpu3d_init(NdsBoard* board);
void nds_gpu3d_free(NdsBoard* board);
/* the registers at 0x04000060 and 0x04000320 to 0x040006A3 */
uint32_t nds_gpu3d_read(NdsBoard* board, uint32_t offset, unsigned bytes);
void nds_gpu3d_write(NdsBoard* board, uint32_t offset, unsigned bytes, uint32_t value);
/* the geometry FIFO's interrupt is a level: raised while GXSTAT's condition holds. True when it does */
bool nds_gpu3d_irq(NdsBoard* board);
struct NdsGpu3D;
const struct NdsGpu3D* nds_gpu3d_state(const NdsBoard* board);
/* the vertical blank: a pending SWAP_BUFFERS takes effect */
void nds_gpu3d_vblank(NdsBoard* board);
/* line 215: the frame's polygons are drawn, with the textures memory holds now */
void nds_gpu3d_render(NdsBoard* board);
#endif
