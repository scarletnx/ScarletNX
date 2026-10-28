/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/nvn_obj.h */
/* nvn_direct's NVN objects and command records (Part C of the NVN plan). The game allocates every NVN object itself
   and hands the runtime its address, so an object's state lives in place, in the game's own object memory: each
   layout here fits in the fewest bytes any SDK's driver measured for that object (tools/nvn_census.py
   --footprints over seven games, SDK 5.4 to 20.5), and a static assertion holds it there. The game copies the small
   state objects (blend, color, depth-stencil...) by value, so their state is packed in their 4 to 24 bytes.

   Objects refer to each other and to memory by guest address (u64), never by host pointer: a copy the game makes
   holds together, and the same 8 bytes nxnative's native objects held. A memory pool has GPU addresses of its own,
   mapped over its guest memory in the GPU's page table (gmmu_space_*, nvdrv.c) as nvhost-as-gpu maps a buffer, so the
   addresses the game takes from NVN (buffer, texture and descriptor addresses, the ones it records) are GPU addresses,
   which the renderer and gmmu_translate read as they read the Maxwell path's.

   The layouts are the runtime's own: the game never reads them (it only calls NVN's getters), so they are read and
   written as host structures over guest memory (GP(), which the little-endian hosts the runtime runs games on lay
   out as the guest would).

   Commands are recorded into the game's command memory in nxnative's record format: a header {op, size} (size
   with the header, a multiple of 8), then the payload. A recording's handle is the guest address of its first record;
   OP_JUMP continues in another segment, OP_CALL runs another recording, OP_END ends one. The queue's replay
   (gpu/nvn/nvn_replay.c) reads them on the GPU thread once they are submitted */
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
    u32 flags, priority;
    u64 cmd_size, compute_size, control_size, flush_threshold, memory, memory_size;
} NvnQueueBuilder; /* nxnative's device field doesn't fit an SDK 5 builder's 0x38: the runtime has one device */
NVN_FITS(NvnQueueBuilder, 0x38);
/* replay: the queue's replay state (gpu/nvn/nvn_replay.c), by index. ready: when its work so far is done on the
   pacer's clock (plat_ns), and shown: the present on the display it waited for (nvn_replay_shown_syncpt's value, 0
   none): only the syncs the queue waits for (a window texture's) hold it back there */
typedef struct {
    u32 magic, replay;
    u64 submits, ready;
    u32 shown, pad;
} NvnQueue;
NVN_FITS(NvnQueue, 0x1748);

/* ---- memory pools and buffers ---- */
typedef struct {
    u64 device, memory, size;
    u32 flags, pad;
} NvnPoolBuilder;
NVN_FITS(NvnPoolBuilder, 0x40);
/* memory: the storage's guest address (0 for a virtual pool, which has none); gpu: its GPU address, the page table
   showing memory there (a virtual pool's pages show what its mapping requests put there) */
typedef struct {
    u32 magic, flags;
    u64 memory, size, gpu;
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
    u64 address; /* the pool's GPU address + offset */
    u64 size, pool;
    i64 offset;
} NvnBuffer;
NVN_FITS(NvnBuffer, 0x30);

/* a texture or sampler pool: its descriptors live in pool memory, the hardware's TIC and TSC entries, 32 bytes
   each (gpu/nvn/nvn_tic.c). offset is 32 bits to fit SDK 5's 0x14 bytes */
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
   address names nothing). memory: the storage's guest address (pool memory + offset), 0 in a virtual pool; gpu: its
   GPU address; size its bytes */
typedef struct {
    u32 magic;
    NvnTexDesc d;
    u64 gpu;
    u64 pool;
    i64 offset;
    u64 memory, size;
    u8 layout[8];
    u64 serial;
} NvnTexture;
NVN_FITS(NvnTexture, 0x70); /* one game's SDK (NVN 55.8) allocates NVNtexture as 0x70 bytes */
/* A command's snapshot of a texture (gpu/nvn/nvn_cmd.c tex_snap): the texture's first bytes, all its replay reads
   (magic, description, GPU address). Records stay small: games size the command memory they add by what NVN's own
   commands take, and one game adds less than 1056 bytes at a time, where eight render targets' whole objects took more */
#define NVN_TEX_SNAP offsetof(NvnTexture, pool)
typedef char nvn_check_tex_snap[NVN_TEX_SNAP == 40 ? 1 : -1];
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


/* ---- samplers ---- */
typedef struct {
    u8 min_filter, mag_filter, wrap_s, wrap_t, wrap_r, compare_mode, compare_func, reduction;
    f32 lod_min, lod_max, lod_bias, anisotropy, lod_snap, border[4];
} NvnSamplerPar;
/* no device: SDK 5.4's builder has room for one, the newer ones' 0x30 bytes only for the state */
typedef struct {
    NvnSamplerPar p;
} NvnSamplerBuilder;
NVN_FITS(NvnSamplerBuilder, 0x30);
typedef struct {
    u32 magic;
    NvnSamplerPar p;
    u64 serial;
} NvnSampler;
NVN_FITS(NvnSampler, 0x40);

/* ---- programs and syncs ---- */
/* A program's stage as SetShaders read it from the stage's control section (the caller's only during that call):
   its code area (data, the GPU address SetShaders gave), the program's offset in it (the shader program header,
   after the driver's 0x30-byte wrapper; a dual vertex program's VertexB), and the compiler's constants, constant
   bank 1, at their offset with their bytes (in 256-byte units). data 0: no such stage */
typedef struct {
    u64 data;
    u32 off;
    u16 cb, cb_size;
} NvnStage;
/* st: by NVN shader stage (NVN_SHADER_STAGE_*). off_a: a dual vertex program's VertexA (0 for none), or a compute
   program's shared memory bytes; block: a compute program's workgroup size */
typedef struct {
    u32 magic, off_a;
    NvnStage st[6];
    u64 serial;
    u16 block[3], pad;
} NvnProgram;
NVN_FITS(NvnProgram, 0x78);
/* A fenced sync signals when the syncpoint reaches value (0: no syncpoint), the presents' syncpoint reaches shown (0:
   none; nvn_replay_shown_syncpt, a present on the display) and the pacer's clock (plat_ns) reaches ready: its queue's
   work before it replayed and done, and what the queue waited for before it (a window's texture comes free at the
   display's pace) */
typedef struct {
    u32 magic, fenced;
    u32 syncpt, value;
    u64 ready;
    u32 shown, pad;
} NvnSync;
NVN_FITS(NvnSync, 0x20);

/* an event: a 32-bit value in pool memory, which the CPU and the GPU signal and the CPU reads */
typedef struct {
    u64 pool;
    i64 offset;
} NvnEventBuilder;
NVN_FITS(NvnEventBuilder, 0x10);
typedef struct {
    u32 magic, pad;
    u64 value, gpu; /* the u32's guest and GPU addresses */
} NvnEvent;
NVN_FITS(NvnEvent, 0x28);

/* ---- windows ---- */
typedef struct {
    u64 texture[4];
    i32 count, interval;
} NvnWindowBuilder; /* SDK 5's builder is 0x2c bytes: no device or native window, which the runtime doesn't use */
NVN_FITS(NvnWindowBuilder, 0x2c);
/* pace: when the last present was shown on the pacer's clock; available[i]: when texture i can be acquired again,
   its last present's slot plus one period (0: at once); syncpt[i], value[i]: the point the replay reaches once texture
   i is off the display (0: none), the present after its own, or its own while none came yet (shown: those);
   on_display[0], [1]: the presents' syncpoint value once the last present, and the one before it, are on the display
   (0: not waited for); released[i]: when the game could draw in texture i, as far as the window knows (for the
   renderer's display times) */
typedef struct {
    u32 magic;
    i32 count, interval, next, crop[4];
    u64 texture[4];
    u64 presents, pace, available[4], released[4];
    u32 syncpt[4], value[4], on_display[2], shown;
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
    u64 value; /* the u32's GPU address */
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
    u32 rgba[4]; /* floats, or an integer clear's integers */
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
/* the stages bound (NVN_SHADER_STAGE_*_BIT) and the program's as they were when recorded */
typedef struct {
    i32 stages;
    u32 off_a;
    NvnStage st[6];
    u64 serial; /* the program's SetShaders (nvn_program_xfb) */
    u16 block[3], pad;
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
   GetCommandMemoryUsed reports. instances 0 counts as 1. ind: an indirect draw's arguments, which the GPU reads; a
   multi-draw's mdi_max records mdi_stride bytes apart from there, as many as the 32-bit count at mdi_count says */
typedef struct {
    i32 prim, first, count, instances, base_instance, pad;
    u64 ind, mdi_count;
    i32 mdi_max, mdi_stride;
} NvnPDraw;
typedef struct {
    i32 prim, index_type, count, base_vertex, base_instance, instances;
    u64 index_buffer, ind, mdi_count;
    i32 mdi_max, mdi_stride;
} NvnPDrawIdx;
/* OP_SET_TEX_POOL, OP_SET_SAMP_POOL: the pool's descriptors (their GPU address) and its count */
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
/* caller: the game's call site, which names the dispatcher; ind: DispatchComputeIndirect's group counts */
typedef struct {
    i32 x, y, z, pad;
    u64 caller, ind;
} NvnPDispatch;
typedef struct {
    f32 factor, units, clamp;
} NvnPPolyOffset;
typedef struct {
    i32 on;
} NvnPDepthClamp;
/* mode 0 and 1: SetRenderEnableConditional (draw when the 64-bit counters at address and address + 16 are equal, not
   equal), 2 and 3: SetRenderEnable(true, false) */
typedef struct {
    i32 mode, pad;
    u64 address;
} NvnPRenderCond;
/* OP_UPDATE_UB: the destination and size, then the bytes, copied at record time (the caller may change its own
   afterwards) */
typedef struct {
    u64 dst, size;
} NvnPUpdateUB;
typedef struct {
    u64 src, dst, size;
} NvnPCopyB2B;
/* a copy between a buffer and a texture (OP_COPY_B2T, and OP_COPY_T2B with buffer the destination), with the
   texture's snapshot after it */
typedef struct {
    u64 buffer, texture;
    NvnView view;
    u8 has_view, pad[7];
    NVNcopyRegion r;
    i32 flags;
} NvnPCopyB2T;
/* a copy between textures, the two snapshots after it. NVN_COPY_RESOLVE in flags: a Downsample (a multisample
   resolve), which reads its source and writes its destination as a copy does */
typedef struct {
    u64 src, dst;
    NvnView sv, dv;
    u8 has_sv, has_dv, pad[6];
    NVNcopyRegion sr, dr;
    i32 flags;
} NvnPCopyT2T;
#define NVN_COPY_RESOLVE 0x40000000
typedef struct {
    u64 texture;
    NvnView view;
    u8 has_view, is_uint, pad[6];
    NVNcopyRegion r;
    i32 mask;
    u32 c[4]; /* floats' bits, or unsigned integers */
} NvnPClearTex;
/* image: SetCopyImageStride (bytes per layer or slice) instead of SetCopyRowStride */
typedef struct {
    i64 stride;
    i32 image, pad;
} NvnPCopyStride;
typedef struct {
    u32 on, index;
} NvnPPrimRestart;
/* per viewport the source of clip x, y, z and w XOR the identity (0, 2, 4, 6), so 0 is unchanged */
typedef struct {
    i32 first, count;
    u8 sw[16][4];
} NvnPVpSwizzle;
/* a memory pool mapping request (NVNmappingRequest): {physical pool, physical offset, virtual offset, size, storage
   class} */
typedef struct {
    u64 physical;
    i64 physical_offset, virtual_offset;
    u64 size;
    i32 storage_class, pad;
} NvnMapRequest;
typedef char nvn_check_map_request[sizeof(NvnMapRequest) == 0x28 ? 1 : -1];

#endif
