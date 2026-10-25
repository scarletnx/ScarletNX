/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's draws: a queue's rendering state, which its replayed records set, as the renderer's draw state
   (gpu/nvn/nvn_draw.c) */
#ifndef SNX_NVN_DRAW_H
#define SNX_NVN_DRAW_H
#include "common/cpu/aarch64/cpu.h"

typedef struct NvnDraw NvnDraw;

/* a queue's state, NVN's defaults, its draws owner as renderer.h numbers it (one allocation, the caller's) */
NvnDraw* nvn_draw_new(u32 owner);
/* On the GPU thread, a record a queue replays: its render targets, clears, state, bindings and draws. 0 when the
   record is none of those */
int nvn_draw_record(NvnDraw* d, u32 op, u64 payload, u32 size);
/* the device's window origin and depth mode (NVN_WINDOW_ORIGIN_MODE_*, NVN_DEPTH_MODE_*), which every queue's draws
   take from now on */
void nvn_draw_device(u32 origin, u32 depth_mode);

#endif
