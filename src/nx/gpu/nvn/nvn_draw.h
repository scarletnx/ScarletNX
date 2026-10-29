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
/* the NVN state a queue's records set: the state objects' words as the game packed them (nvn_obj.h). Each queue's
   is in its replay state (gpu/nvn/nvn_replay.h) */
struct NvnDraw {
    DsState ds; /* what the renderer reads */
    DrawOwner own;
    DsBindings bind; /* the bindings and inline blocks as the records left them */
    DsInline inl;
    u64 blend[DS_TARGETS_MAX], dss;
    u32 color, chmask, poly;
    NvnMultisample ms;
    u32 sample_off;                   /* the samples SetSampleMask leaves out (NVN's default mask has every one) */
    u32 svalue[2], smask[2], sref[2]; /* the stencil's compare mask, write mask and reference, front and back */
    float blend_color[4], bias[3];    /* the polygon offset's factor, units and clamp */
    u8 discard, depth_clamp, restart, render_off;
    u8 provoking_last, conservative, dilate; /* SetProvokingVertexMode's last vertex, conservative rasterization */
    u8 sample_shading;                       /* the bound fragment program's SetSampleShading mode */
    float line_width;
    u8 samples; /* the samples-passed counter was reset: the renderer counts it */
    u32 restart_index;
    float vp[16][4], depth[2]; /* x, y, w, h; depth range 0 */
    i32 scissor[4];
    u8 swizzle[16][4]; /* XOR the identity, as recorded */
    u32 attr[16], nattr;
    NvnVertexStream stream[16];
    u32 nstream;
    NVNbufferRange vbuf[16];
    u64 code[DS_PROGRAM_SLOTS];
    u32 prog_on;
    ShXfb xfb;            /* the bound program's capture layout (zero: none) */
    NVNbufferRange tf[4]; /* the transform feedback buffers */
    u8 tf_on;             /* between Begin (or Resume) and End (or Pause) */
    u32 dirty;
    DsState scratch;  /* a texture's clear, as a target of its own (OP_CLEAR_TEX) */
    DispatchState dx; /* the compute stage's program and bindings, a dispatch's state */
};
/* the file's state, g_nvn.draw (gpu/nvn/nvn_state.h): the device's window origin and depth mode, for every queue (set
   on the game's threads, read on the GPU thread; atomic). Zero is NVN's default of each, the lower left origin and
   near at -w */
typedef struct {
    u32 origin, depth_mode;
} NvnDrawState;

/* a queue's state d, all zero: NVN's defaults, its draws owner as renderer.h numbers it */
void nvn_draw_init(NvnDraw* d, u32 owner);
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
