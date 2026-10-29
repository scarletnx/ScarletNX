/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's draws: a queue's rendering state, which its replayed records set, as the renderer's draw state
   (gpu/nvn/nvn_draw.c) */
#ifndef SNX_NVN_DRAW_H
#define SNX_NVN_DRAW_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/draw_state.h"
#include "nx/gpu/nvn/nvn_obj.h"

typedef struct NvnDraw NvnDraw;
/* the file's state, g_nvn.draw (gpu/nvn/nvn_state.h): the device's window origin and depth mode, for every queue (set
   on the game's threads, read on the GPU thread; atomic). Zero is NVN's default of each, the lower left origin and
   near at -w */
typedef struct {
    u32 origin, depth_mode;
} NvnDrawState;

/* a queue's state, NVN's defaults, its draws owner as renderer.h numbers it (one allocation, the caller's) */
NvnDraw* nvn_draw_new(u32 owner);
/* On the GPU thread, a record a queue replays: its render targets, clears, state, bindings and draws. 0 when the
   record is none of those */
int nvn_draw_record(NvnDraw* d, u32 op, u64 payload, u32 size);
/* On the GPU thread, a samples-passed report of the queue at address ({count, ticks}), in order with its draws: 0
   when the queue's counter wasn't reset, and the caller writes a count of its own */
int nvn_draw_samples(NvnDraw* d, u64 address, u64 ticks);
/* the device's window origin and depth mode (NVN_WINDOW_ORIGIN_MODE_*, NVN_DEPTH_MODE_*), which every queue's draws
   take from now on */
void nvn_draw_device(u32 origin, u32 depth_mode);
/* the render target a texture snapshot (a command's, gpu/nvn/nvn_cmd.c) and its view (NULL: none) are, as the 3D
   engine's registers describe one (gpu/draw_state.h's DsSurf: a multisampled one's size in samples), whether it is a
   depth target, and its sample layout (DS_MSAA_): 0 when the texture's format takes no target */
int nvn_draw_surface(u64 snapshot, const NvnView* v, DsSurf* s, int* depth, u32* msaa);

#endif
