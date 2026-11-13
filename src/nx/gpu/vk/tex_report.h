/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The texture cache's measurements, which change nothing the cache does: the textures it holds for texture objects
   the game has ended, the textures it makes again after it evicted them, the memory its sparse textures bind
   against the guest memory behind them, and its bytes by format. All on the GPU thread, as the caches are. */
#ifndef SNX_VK_TEX_REPORT_H
#define SNX_VK_TEX_REPORT_H
#include "nx/gpu/vk/vk.h"

/* a texture of the cache evicted (surface.c surf_evict), before it is freed: its key is kept. A save state's load
   evicts them all, and counts here as any eviction */
void vk_tex_evicted(const Surf* t);
/* a texture made and loaded (texture.c tex_from_tic): counted when the cache evicted one of its key before */
void vk_tex_made(const Surf* t);
/* the report (renderer.c, every 600 presents): the first two always, the others with buffer_copy_stats */
void vk_tex_report_log(void);
#endif
