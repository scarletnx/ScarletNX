/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/nvn_obj.h */
/* nvn_direct's NVN objects and command records (Part C of the NVN plan). The game allocates every NVN object itself
   and hands the runtime its address, so an object's state lives in place, in the game's own object memory: each
   layout here fits in the fewest bytes any SDK's driver measured for that object (tools/nvn_census.py
   --footprints over seven games, SDK 5.4 to 20.5), and a static assertion holds it there. The game copies the small
   state objects (blend, color, depth-stencil...) by value, so their state is packed in their 4 to 24 bytes.

   Objects refer to each other and to memory by guest address (u64), never by host pointer: a copy the game makes
   holds together, and the same 8 bytes nxnative's native objects held. A memory pool's GPU address is its guest CPU
   address for now (phase 1, no rendering); phase 2 maps pools through the GMMU, and the addresses the records hold
   are then GPU addresses the replay translates.

   The layouts are the runtime's own: the game never reads them (it only calls NVN's getters), so they are read and
   written as host structures over guest memory (GP(), which the little-endian hosts the runtime runs games on lay
   out as the guest would).

   Commands are recorded into the game's command memory in nxnative's record format: a header {op, size} (size
   with the header, a multiple of 8), then the payload. A recording's handle is the guest address of its first record;
   OP_JUMP continues in another segment, OP_CALL runs another recording, OP_END ends one. Phase 2's Vulkan replay reads
   these records; phase 1 walks them at submit only for what the CPU sees (counters, fences, events) */
#ifndef SNX_NVN_OBJ_H
#define SNX_NVN_OBJ_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/gpu/nvn/nvn.h"
#include <stddef.h>

#define NVN_MAGIC(a, b, c, d) ((u32)(a) | (u32)(b) << 8 | (u32)(c) << 16 | (u32)(d) << 24)
/* the fewest bytes any measured SDK's driver writes for the object (footprints: SetDefaults and Initialize) */
#define NVN_FITS(T, n) typedef char nvn_fits_##T[sizeof(T) <= (n) ? 1 : -1]

/* ---- device ---- */
typedef struct {
    u32 flags;
} NvnDeviceBuilder;
NVN_FITS(NvnDeviceBuilder, 0x18);
/* NVNdevice's own size isn't measured (the driver's Initialize branches through a pointer it fills at run time): the
   header reserves 0x3000, and the state stays within the device builder's 0x18 */
typedef struct {
    u32 magic, flags, origin, depth_mode;
} NvnDevice;
NVN_FITS(NvnDevice, 0x18);

/* ---- queue ---- */
typedef struct {
    u32 flags, pad;
    u64 cmd_size, compute_size, control_size, flush_threshold, memory, memory_size;
} NvnQueueBuilder; /* nxnative's device field doesn't fit an SDK 5 builder's 0x38: the runtime has one device */
NVN_FITS(NvnQueueBuilder, 0x38);
typedef struct {
    u32 magic, pad;
    u64 submits;
} NvnQueue;
NVN_FITS(NvnQueue, 0x1748);

/* ---- memory pools and buffers ---- */
typedef struct {
    u64 device, memory, size;
    u32 flags, pad;
} NvnPoolBuilder;
NVN_FITS(NvnPoolBuilder, 0x40);
/* memory: the storage's guest address, which is also its GPU address in phase 1 */
typedef struct {
    u32 magic, flags;
    u64 memory, size;
} NvnPool;
NVN_FITS(NvnPool, 0x100);

typedef struct {
    u64 device, pool;
    i64 offset;
    u64 size;
} NvnBufferBuilder;
NVN_FITS(NvnBufferBuilder, 0x28);
typedef struct {
    u32 magic, pad;
    u64 address; /* the pool's memory + offset */
    u64 size, pool;
    i64 offset;
} NvnBuffer;
NVN_FITS(NvnBuffer, 0x30);

/* a texture or sampler pool: its descriptors live in pool memory, 32 bytes each, as the hardware's do. offset is
   32 bits to fit SDK 5's 0x14 bytes */
typedef struct {
    u64 pool;
    u32 offset;
    i32 count;
} NvnDescPool;
NVN_FITS(NvnDescPool, 0x14);
#define NVN_DESCRIPTOR_SIZE 32

/* ---- textures ---- */
/* what a texture builder describes and a texture keeps. bits: the packaged layout's block height + 1 (0: the
   automatic one) in bits 0-3, NVN_TEX_STENCIL, NVN_TEX_HAS_LAYOUT. swizzle: r | g << 3 | b << 6 | a << 9 (NVN's
   channel enums), with NVN_TEX_SWIZZLE_SET once TextureBuilderSetSwizzle was called */
typedef struct {
    u16 format, swizzle;
    u32 flags;
    u8 target, samples, levels, bits;
    u32 width, height, depth, stride;
} NvnTexDesc;
#define NVN_TEX_BLOCK_H     0x0fu
#define NVN_TEX_STENCIL     0x10u /* TextureBuilderSetDepthStencilMode(STENCIL): it reads its storage's stencil */
#define NVN_TEX_HAS_LAYOUT  0x20u
#define NVN_TEX_SWIZZLE_SET 0x8000u

typedef struct {
    u64 device;
    NvnTexDesc d;
    u32 pad;
    u64 pool;
    i64 offset;
    u8 layout[8];      /* SetPackagedTextureLayout's bytes */
    u64 packaged_data; /* SetPackagedTextureData: copied into the storage at TextureInitialize */
} NvnTexBuilder;
NVN_FITS(NvnTexBuilder, 0x68);
/* serial: a number per initialization, never reused, for phase 2's host texture (the game may copy the object, so its
   address names nothing). memory: the storage's guest address (pool memory + offset), size its bytes */
typedef struct {
    u32 magic;
    NvnTexDesc d;
    u64 pool;
    i64 offset;
    u64 memory, size;
    u8 layout[8];
    u64 serial;
} NvnTexture;
NVN_FITS(NvnTexture, 0x70); /* one game's SDK (NVN 55.8) allocates NVNtexture as 0x70 bytes */
/* NVNtextureView: 16 bytes in newer SDKs (a game keeps one in a 16-byte stack slot). set: which parts were set */
typedef struct {
    u8 set, target, base_level, levels;
    u16 format, min_layer, layers;
    u8 swizzle[4];
    u16 reserved;
} NvnView;
NVN_FITS(NvnView, 0x10);
#define NVN_VIEW_LEVELS  0x01u
#define NVN_VIEW_LAYERS  0x02u
#define NVN_VIEW_FORMAT  0x04u
#define NVN_VIEW_TARGET  0x08u
#define NVN_VIEW_SWIZZLE 0x10u
#define NVN_VIEW_DSMODE  0x20u /* the depth-stencil mode in bits 6-7 */

/* a texture pool's descriptor: what TexturePoolRegisterTexture or RegisterImage put at its ID (the view zeroed when
   none was given). Phase 2 finds its host texture by serial */
typedef struct {
    u64 serial, texture;
    NvnView view;
} NvnTexDescriptor;
typedef char nvn_check_texdesc[sizeof(NvnTexDescriptor) == NVN_DESCRIPTOR_SIZE ? 1 : -1];

/* ---- samplers ---- */
typedef struct {
    u8 min_filter, mag_filter, wrap_s, wrap_t, wrap_r, compare_mode, compare_func, reduction;
    f32 lod_min, lod_max, lod_bias, anisotropy, border[4];
} NvnSamplerPar;
typedef struct {
    u64 device;
    NvnSamplerPar p;
} NvnSamplerBuilder;
NVN_FITS(NvnSamplerBuilder, 0x30);
typedef struct {
    u32 magic;
    NvnSamplerPar p;
    u64 serial;
} NvnSampler;
NVN_FITS(NvnSampler, 0x40);
/* a sampler pool's descriptor: what SamplerPoolRegisterSampler put at its ID */
typedef struct {
    u64 serial, sampler, reserved[2];
} NvnSamplerDescriptor;
typedef char nvn_check_sampdesc[sizeof(NvnSamplerDescriptor) == NVN_DESCRIPTOR_SIZE ? 1 : -1];

/* ---- programs and syncs ---- */
/* sd: the stages SetShaders gave ({code address, control section address}). The control section is the caller's
   only during that call: phase 2 copies or hashes it there */
typedef struct {
    u32 magic, stages;
    NVNshaderData sd[6];
    u64 serial;
} NvnProgram;
NVN_FITS(NvnProgram, 0x78);
/* fence: the submission whose completion signals it. Every submission completes as it is made in phase 1 */
typedef struct {
    u32 magic, fenced;
    u64 fence;
} NvnSync;
NVN_FITS(NvnSync, 0x20);

/* ---- windows ---- */
typedef struct {
    u64 texture[4];
    i32 count, interval;
} NvnWindowBuilder; /* SDK 5's builder is 0x2c bytes: no device or native window, which the runtime doesn't use */
NVN_FITS(NvnWindowBuilder, 0x2c);
typedef struct {
    u32 magic;
    i32 count, interval, next, crop[4];
    u64 texture[4];
    u64 presents;
} NvnWindow;
NVN_FITS(NvnWindow, 0x148);

/* ---- command buffers ---- */
/* cur, end: the current command memory segment (guest addresses). begin: the recording's first record, its handle.
   recording: 0 no, 1 yes, 2 yes with no record yet. used: bytes written since the game last added command memory, the
   segments' JUMPs included (GetCommandMemoryUsed). +0x10 is the game's: its inlined NVN code writes the byte 1 there
   after AddCommandMemory (an internal flag of the driver's) */
typedef struct {
    u32 magic, recording;
    u64 cur;
    u64 game_flag10;
    u64 end, begin;
    u64 callback, callback_data;
    u64 device, used;
} NvnCmdBuf;
NVN_FITS(NvnCmdBuf, 0x90);
typedef char nvn_check_cmd_flag[offsetof(NvnCmdBuf, game_flag10) == 0x10 ? 1 : -1];

/* ---- state objects, packed in place: fields of one word, lowest bits first (nxnative's bit-fields) ---- */
static inline u64 nvn_field(u64 w, unsigned lo, unsigned n) { return (w >> lo) & ((1ull << n) - 1); }
static inline u64 nvn_set_field(u64 w, unsigned lo, unsigned n, u64 v) {
    u64 m = ((1ull << n) - 1) << lo;
    return (w & ~m) | ((v << lo) & m);
}
/* NVNblendState (8 bytes, one u64) */
#define NVN_BLEND_TARGET  0, 3
#define NVN_BLEND_SRC_RGB 3, 7
#define NVN_BLEND_DST_RGB 10, 7
#define NVN_BLEND_SRC_A   17, 7
#define NVN_BLEND_DST_A   24, 7
#define NVN_BLEND_EQ_RGB  31, 3
#define NVN_BLEND_EQ_A    34, 3
#define NVN_BLEND_ADV     37, 6 /* the advanced mode, 0 none */
#define NVN_BLEND_ADV_NPM 43, 1
#define NVN_BLEND_ADV_OVL 44, 2
/* NVNcolorState (4 bytes, one u32) */
#define NVN_COLOR_BLEND_EN   0, 8
#define NVN_COLOR_LOGIC_OP   8, 4
#define NVN_COLOR_ALPHA_FUNC 12, 4
/* NVNchannelMaskState (one u32): 4 bits per target, r 1 g 2 b 4 a 8, set = written */
/* NVNpolygonState (one u32) */
#define NVN_POLY_CULL      0, 2
#define NVN_POLY_FRONT     2, 1
#define NVN_POLY_MODE      3, 2
#define NVN_POLY_OFFSET_EN 5, 3
/* NVNdepthStencilState (8 bytes, one u64) */
#define NVN_DS_TEST       0, 1
#define NVN_DS_WRITE      1, 1
#define NVN_DS_FUNC       2, 4
#define NVN_DS_STENCIL    6, 1
#define NVN_DS_SFUNC_F    7, 4
#define NVN_DS_SFUNC_B    11, 4
#define NVN_DS_FAIL_F     15, 4
#define NVN_DS_ZFAIL_F    19, 4
#define NVN_DS_ZPASS_F    23, 4
#define NVN_DS_FAIL_B     27, 4
#define NVN_DS_ZFAIL_B    31, 4
#define NVN_DS_ZPASS_B    35, 4
/* NVNvertexAttribState (one u32) */
#define NVN_VA_FORMAT 0, 8
#define NVN_VA_OFFSET 8, 19
#define NVN_VA_STREAM 27, 5
/* NVNmultisampleState (0x18 bytes) */
typedef struct {
    u8 enable, samples, alpha_to_coverage, a2c_dither, raster_samples, coverage_mod, cov_to_color, cov_to_color_out;
    u8 sample_locations, sample_grid;
} NvnMultisample;
NVN_FITS(NvnMultisample, 0x18);
/* NVNvertexStreamState (8 bytes) */
typedef struct {
    u32 stride, divisor;
} NvnVertexStream;
NVN_FITS(NvnVertexStream, 0x8);

/* ---- command records ---- */
typedef struct {
    u32 op, size;
} NvnRecHdr;
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL, /* OP_BEGIN isn't recorded: a recording starts with its first command */
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB, /* effects the CPU can see */
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG,
    OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_CLEAR_BUF, OP_DEPTH_CLAMP, OP_RENDER_COND, OP_FENCE_SYNC,
    OP_TF_BIND, OP_TF_STATE, OP_RAST_DISCARD, OP_VP_SWIZZLE, OP_BIND_TEX1, OP_PRIM_RESTART, OP_COUNT_
};
/* the payloads, right after the header. Where nxnative held a host pointer, these hold a guest address */
/* OP_JUMP and OP_CALL: u64 target. OP_FENCE_SYNC: u64 sync. OP_RESET_COUNTER: u64 type. OP_BARRIER: i32 bits */
typedef struct {
    u64 type, address;
} NvnPReportCounter;
typedef struct {
    u64 value; /* the u32's address */
    u32 mode, v;
} NvnPSignalEvent;
/* color[i] and depth: the texture snapshots after the payload (the game may finalize or reuse an object before it
   submits), or the game's object when it wasn't a live texture as recorded */
typedef struct {
    i32 count, pad;
    u64 color[8];
    NvnView cview[8];
    u8 has_cview[8];
    u64 depth;
    NvnView dview;
    u8 has_dview, pad2[7];
} NvnPSetRT;
typedef struct {
    i32 index, mask;
    f32 rgba[4];
} NvnPClearColor;
typedef struct {
    f32 depth;
    i32 depth_mask, stencil, stencil_mask;
} NvnPClearDS;
typedef struct {
    i32 first, count;
    f32 v[16][6]; /* x, y, w, h, near, far (near and far come from OP_DEPTH_RANGE) */
} NvnPViewports;
typedef struct {
    i32 x, y, w, h;
} NvnPScissor;
typedef struct {
    f32 n, f;
} NvnPDepthRange;
typedef struct {
    u64 program;
    i32 stages, pad;
} NvnPBindProgram;
/* kind: 0 blend 1 color 2 channel mask 3 multisample 4 polygon 5 depth-stencil; the object's bytes */
typedef struct {
    i32 kind;
    u8 bytes[0x18];
} NvnPBindState;
typedef struct {
    i32 first, count;
    u32 st[16];
} NvnPBindVAttr; /* recorded with count entries */
typedef struct {
    i32 first, count;
    NvnVertexStream st[16];
} NvnPBindVStream; /* recorded with count entries */
typedef struct {
    i32 first, count;
    NVNbufferRange r[16];
} NvnPBindVBuf; /* recorded with count entries */
typedef struct {
    i32 stage, index;
    u64 address, size;
} NvnPBindBuf; /* uniform and storage buffers */
/* separate textures, samplers and images: handles as the game gives them (recorded with count entries). A handle is
   the descriptor's ID in the pool the command buffer last set: the descriptors live in the pool's memory, so unlike
   nxnative's host table no per-pool base is added */
typedef struct {
    i32 stage, first, count, defer;
    u64 src;
    u64 h[32];
} NvnPBindHandles;
/* nvnCommandBufferBindTexture's combined handle (texture ID | sampler ID << 20) 16 bytes into the record, where the
   hardware's command has it and an engine that patches recorded commands writes a new one */
typedef struct {
    i32 stage, slot;
    u64 h;
} NvnPBindTex1;
typedef char nvn_check_tex1[offsetof(NvnPBindTex1, h) == 8 ? 1 : -1];
/* which: 0 value mask 1 write mask 2 reference */
typedef struct {
    i32 face, value_mask, mask, ref, which;
} NvnPStencil;
/* a draw stops before ind unless it is indirect or a multi-draw: games size command memory by what
   GetCommandMemoryUsed reports. instances 0 counts as 1 */
typedef struct {
    i32 prim, first, count, instances, base_instance, pad;
    u64 ind, mdi_count;
    i32 mdi_index, pad2;
} NvnPDraw;
typedef struct {
    i32 prim, index_type, count, base_vertex, base_instance, instances;
    u64 index_buffer, ind, mdi_count;
    i32 mdi_index, pad;
} NvnPDrawIdx;
/* OP_SET_TEX_POOL, OP_SET_SAMP_POOL: the pool's descriptors (memory + offset) and its count */
typedef struct {
    u64 descriptors;
    i32 count, pad;
} NvnPSetPool;
typedef struct {
    u64 address, size;
    u32 value, pad;
} NvnPClearBuf;
typedef struct {
    f32 rgba[4];
} NvnPBlendColor;
typedef struct {
    i32 index, pad;
    u64 address, size;
} NvnPTfBind;
/* what: 0 begin, 1 end, 2 pause, 3 resume; control: the game's 32-byte control buffer */
typedef struct {
    i32 what, pad;
    u64 control;
} NvnPTfState;
typedef struct {
    i32 on;
} NvnPRastDiscard;

#endif
