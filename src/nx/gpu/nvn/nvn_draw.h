/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's draws: a queue's rendering state, which its replayed records set, as the renderer's draw state
   (gpu/nvn/nvn_draw.c) */
#ifndef SNX_NVN_DRAW_H
#define SNX_NVN_DRAW_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/draw_state.h"
#include "nx/gpu/gpu.h"
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
    u8 subpixel_bias_x, subpixel_bias_y;
    u8 sample_shading;                       /* the bound fragment program's SetSampleShading mode */
    u8 flip;                                 /* the window origin's flip, taken as the queue was initialized */
    float line_width, alpha_ref;
    u32 fixed_flags;
    float point_size, depth_bounds[2], tess_levels[6];
    u32 coverage_table_enable;
    u8 coverage_table[16];
    u8 samples; /* the samples-passed counter was reset: the renderer counts it */
    u32 discard_width[9]; /* view width in pixels, or 0 when whole-surface discard could affect padding or layers */
    u32 restart_index;
    u32 patch_size;                /* control points per tessellation patch, 0 until explicitly set */
    float vp[16][4], depth[16][2]; /* x, y, w, h and near/far for each viewport */
    i32 scissor[16][4];
    u8 swizzle[16][4]; /* XOR the identity, as recorded */
    u32 attr[NVN_VATTRS], nattr;
    NvnVertexStream stream[16];
    u32 nstream;
    NVNbufferRange vbuf[16];
    u8 c0rec[NVN_C0_RECORD]; /* the bound Ounce compute program's c[0] record (nvn_program_c0) */
    u64 code[DS_PROGRAM_SLOTS];
    u32 prog_on;
    u64 task, mesh;     /* the bound Ounce task and mesh programs (DsPrograms') */
    DsMeshKey mesh_key; /* their fields (nvn_program_mesh) */
    NvnTessStage tess_stage[2];
    u32 tess_params;
    u8 early_z; /* the bound fragment program asks for early tests (nvn_program_early_z) */
    ShXfb xfb;            /* the bound program's capture layout (zero: none) */
    NVNbufferRange tf[4]; /* the transform feedback buffers */
    u8 tf_on;             /* between Begin (or Resume) and End (or Pause) */
    u32 dirty;
    u32 vp_dirty; /* viewports whose transform, depth range, scissor or swizzle needs conversion */
    u8 viewport_upper_left; /* device origin used by the last viewport conversion */
    /* The dirty span of each stage's bank 0 and bank 2. Separate spans leave the unchanged banks between writes
       out of the compare and copy at a draw. */
    u16 inl_lo[2][DS_GFX_STAGES], inl_hi[2][DS_GFX_STAGES];
    u16 root_lo, root_hi; /* and of the root tables, which every graphics stage shares */
    GpuShadingRate rate_in; /* the bound shading rate image and the palettes (OP_RATE_IMAGE, OP_RATE_PALETTE) */
    u32 rate;               /* the renderer's id of it, 0 with no image bound (DrawCall.shading_rate) */
    DsState scratch;  /* a texture's clear, as a target of its own (OP_CLEAR_TEX) */
    DispatchState dx; /* the compute stage's program and bindings, a dispatch's state */
};
/* what the present log counts of the draws (nvn_draw_stats_log): the draws, those that moved a base vertex or base
   instance word of bank 0, those whose inline blocks changed, those of them changed by those words alone, and the
   blocks that changed */
enum {
    NVN_COUNT_DRAWS,
    NVN_COUNT_BASE_MOVED,
    NVN_COUNT_INLINE_MOVED,
    NVN_COUNT_BASE_ONLY,
    NVN_COUNT_BLOCKS,
    NVN_COUNTS
};
/* the file's state, g_nvn.draw (gpu/nvn/nvn_state.h): the device's window origin and depth mode, for every queue (set
   on the game's threads, read on the GPU thread; atomic). Zero is NVN's default of each, the lower left origin and
   near at -w */
typedef struct {
    _Atomic u32 origin, depth_mode;
    _Atomic u64 count[NVN_COUNTS]; /* since the start, on the GPU thread */
    u64 logged[NVN_COUNTS];        /* count at the last present log, on the presenting thread */
} NvnDrawState;

/* a queue's state d, all zero: NVN's defaults, its draws owner as renderer.h numbers it, and the window origin's flip
   of the device's origin now (the queue's initialization) */
void nvn_draw_init(NvnDraw* d, u32 owner);
/* the draws' counts (NvnDrawState) since the last call, as a line a present over the presents since then */
void nvn_draw_stats_log(u64 presents);
/* Release renderer predicate and counter state on the GPU thread after the queue's last replay. */
void nvn_draw_free(NvnDraw* d);
/* On the GPU thread, a record a queue replays: its render targets, clears, state, bindings and draws. 0 when the
   record is none of those */
int nvn_draw_record(NvnDraw* d, u32 op, u64 payload, u32 size);
/* On the GPU thread, what the renderer's dispatch gets for the OP_DISPATCH record at payload, the queue's state taking
   its grid as the record would: 0 when it dispatches nothing. gpu_hook_trace (nvn_trace.c) logs a dispatch this way */
int nvn_draw_dispatch_state(NvnDraw* d, u64 payload, DispatchState* x);
/* Where the bound Ounce compute program's packed c[0] (bank 0 of the state nvn_draw_dispatch_state gives) holds each
   list: the byte offset of its first entry and how many, the buffers 16 bytes each ({low, high, size, 0}), the handles
   8 (the handle's 32 bits, then 0). 0 when the bound program isn't an Ounce compute program. An entry past the
   bank's end (DS_INLINE_BANK0) isn't there */
enum { NVN_C0_UNIFORM, NVN_C0_STORAGE, NVN_C0_TEXTURES, NVN_C0_SEPARATE, NVN_C0_SAMPLERS, NVN_C0_IMAGES, NVN_C0_LISTS };
int nvn_draw_c0_layout(const NvnDraw* d, u32 at[NVN_C0_LISTS], u32 count[NVN_C0_LISTS]);
/* On the GPU thread, a samples-passed report of the queue at address ({count, ticks}), in order with its draws: 0
   when the queue's counter wasn't reset, and the caller writes a count of its own */
int nvn_draw_samples(NvnDraw* d, u64 address);
/* the device's window origin and depth mode (NVN_WINDOW_ORIGIN_MODE_*, NVN_DEPTH_MODE_*), which every queue's draws
   take from now on */
void nvn_draw_device(u32 origin, u32 depth_mode);
/* the render target a texture snapshot (a command's, gpu/nvn/nvn_cmd.c) and its view (NULL: none) are, as the 3D
   engine's registers describe one (gpu/draw_state.h's DsSurf: a multisampled one's size in samples), whether it is a
   depth target, and its sample layout (DS_MSAA_): 0 when the texture's format takes no target */
int nvn_draw_surface(u64 snapshot, const NvnView* v, DsSurf* s, int* depth, u32* msaa);
/* the same of a texture record the caller holds (another front end's, laid out as NVN lays one out: the GL layer's) */
int nvn_draw_surface_of(const NvnTexture* t, const NvnView* v, DsSurf* s, int* depth, u32* msaa);

#endif
