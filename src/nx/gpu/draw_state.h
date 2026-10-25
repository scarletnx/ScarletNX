/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A draw's state as the game asked for it, for any front end (the Maxwell 3D engine's registers today) and read by
   the renderer, which decides what the device does with it: the feature clamps, the resolution scale, empty viewports
   and the render area are its business, not this struct's. Where no guest memory shows the state, the values are
   Vulkan's enumerants as plain integers, so the header needs no Vulkan header (vk/render.c asserts they are equal) */
#ifndef SNX_GPU_DRAW_STATE_H
#define SNX_GPU_DRAW_STATE_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/endian.h"
#include "nx/gpu/shader/shader.h"

#define DS_VIEWPORTS_MAX 16
#define DS_PROGRAM_SLOTS 6 /* VertexA, VertexB, tessellation control and evaluation, geometry, fragment */
#define DS_ATTRS         32
#define DS_STREAMS       32
#define DS_TARGETS_MAX   8
#define DS_GFX_STAGES    5     /* VS, TCS, TES, GS, FS: shader_vk_set's order */
#define DS_CBUFS         18    /* constant buffer slots of a stage */
/* the bytes of the inline constant buffer blocks (DsInline), as NVN's driver lays out its banks: bank 0 holds the
   storage buffer tables (from 0x110 for the vertex stage, 0x310 compute, 0x510 fragment, 16 bytes an entry), the
   largest point size at 0x610 and the base instance at 0x644; bank 2 the handles of textures from 0x20, images from
   0x120, separate textures from 0x168 and separate samplers from 0x568 + 8 r, up to 0x968 */
#define DS_INLINE_BANK0  0x650
#define DS_INLINE_BANK2  0xA00

/* VkPolygonMode, VkCullModeFlags, VkCompareOp, VkStencilOp, VkBlendOp, VkBlendFactor */
enum { DS_POLYGON_FILL, DS_POLYGON_LINE, DS_POLYGON_POINT };
enum { DS_CULL_NONE, DS_CULL_FRONT, DS_CULL_BACK, DS_CULL_FRONT_AND_BACK };
enum {
    DS_CMP_NEVER,
    DS_CMP_LESS,
    DS_CMP_EQUAL,
    DS_CMP_LESS_OR_EQUAL,
    DS_CMP_GREATER,
    DS_CMP_NOT_EQUAL,
    DS_CMP_GREATER_OR_EQUAL,
    DS_CMP_ALWAYS
};
enum {
    DS_STENCIL_KEEP,
    DS_STENCIL_ZERO,
    DS_STENCIL_REPLACE,
    DS_STENCIL_INCREMENT_AND_CLAMP,
    DS_STENCIL_DECREMENT_AND_CLAMP,
    DS_STENCIL_INVERT,
    DS_STENCIL_INCREMENT_AND_WRAP,
    DS_STENCIL_DECREMENT_AND_WRAP
};
enum { DS_BLEND_ADD, DS_BLEND_SUBTRACT, DS_BLEND_REVERSE_SUBTRACT, DS_BLEND_MIN, DS_BLEND_MAX };
enum {
    DS_FACTOR_ZERO,
    DS_FACTOR_ONE,
    DS_FACTOR_SRC_COLOR,
    DS_FACTOR_ONE_MINUS_SRC_COLOR,
    DS_FACTOR_DST_COLOR,
    DS_FACTOR_ONE_MINUS_DST_COLOR,
    DS_FACTOR_SRC_ALPHA,
    DS_FACTOR_ONE_MINUS_SRC_ALPHA,
    DS_FACTOR_DST_ALPHA,
    DS_FACTOR_ONE_MINUS_DST_ALPHA,
    DS_FACTOR_CONSTANT_COLOR,
    DS_FACTOR_ONE_MINUS_CONSTANT_COLOR,
    DS_FACTOR_CONSTANT_ALPHA,
    DS_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
    DS_FACTOR_SRC_ALPHA_SATURATE,
    DS_FACTOR_SRC1_COLOR,
    DS_FACTOR_ONE_MINUS_SRC1_COLOR,
    DS_FACTOR_SRC1_ALPHA,
    DS_FACTOR_ONE_MINUS_SRC1_ALPHA
};
/* VkColorComponentFlagBits */
#define DS_COMPONENT_R       1
#define DS_COMPONENT_G       2
#define DS_COMPONENT_B       4
#define DS_COMPONENT_A       8
/* window_origin's bits */
#define DS_ORIGIN_LOWER_LEFT 0x01 /* y counts up from the bottom of a surface clip_height high */
#define DS_ORIGIN_FLIP_Y     0x10 /* the guest's facing is reversed */

/* The state Vulkan 1.3 sets dynamically, except the viewports */
typedef struct {
    u8 cull;                               /* DS_CULL_ */
    u8 front_cw;                           /* clockwise is front, before the window origin's flip */
    u8 discard;                            /* rasterization is off (it only matters while primitives are captured) */
    u8 bias_on;                            /* depth bias per polygon mode: bit 0 points, 1 lines, 2 filled */
    u8 depth_test, depth_write, depth_cmp; /* with a depth target */
    u8 stencil_test;                       /* with a stencil target */
    u8 alpha_test, alpha_func;             /* DS_CMP_ */
    u8 window_origin;                      /* DS_ORIGIN_ */
    u8 sprite_origin;                      /* a point sprite's T is 0 at its top */
    u8 depth_mode;                         /* 1: depth from 0 to 1, 0: from -1 to 1 */
    u8 point_nudge;                        /* points on a pixel border go to the pixel after it (OpenGL's point
                                              center and half-integer pixel centers, where that rule is known) */
    u8 pad[2];
    struct {
        u8 fail, zfail, pass, cmp; /* DS_STENCIL_, DS_CMP_ */
        u32 ref, cmask, wmask;
    } face[2];        /* front and back: the back is the front when the guest's stencil is one-sided */
    float line_width; /* of the line kind drawn (smooth or aliased) */
    float bias_units, bias_slope, bias_clamp;
    float blend_const[4];
    float alpha_ref;
    u32 clip_height; /* the surface clip's height, which a lower-left origin measures from */
    u32 msaa_mode;   /* the 3D engine's sample layout code */
    u32 depth_word;  /* the vertex stage's viewport and depth word, pushed as it is */
} DsDynamic;

/* The viewports, struct of arrays (a program that writes the viewport index selects among them) */
typedef struct {
    u32 transform_on; /* the guest's viewport transform enable (the renderer picks by the scale, and logs a mismatch) */
    u32 scissor_on;   /* bit j: viewport j's scissor */
    float scale[DS_VIEWPORTS_MAX][3], translate[DS_VIEWPORTS_MAX][3];
    u32 swizzle[DS_VIEWPORTS_MAX]; /* a 3-bit code per component at bits 0, 4, 8, 12 (x, y, z, w): y's 3 negates y */
    u32 clip_x[DS_VIEWPORTS_MAX], clip_y[DS_VIEWPORTS_MAX]; /* start in bits 0-15, extent in 16-31 */
    float z_near[DS_VIEWPORTS_MAX], z_far[DS_VIEWPORTS_MAX];
    u32 scissor_x[DS_VIEWPORTS_MAX], scissor_y[DS_VIEWPORTS_MAX]; /* min in bits 0-15, max in 16-31 */
} DsViewports;

/* The programs and what keys their translation */
typedef struct {
    u64 code[DS_PROGRAM_SLOTS]; /* each slot's program address */
    u32 on;                     /* the slots enabled (bit per slot): VertexB runs whatever its bit says */
    u32 clip_enable;            /* the clip distances that clip (bit per distance) */
    u32 tess_params;            /* the tessellator's domain (bits 0-1), spacing (4-5) and output primitive (8-9) */
    u32 gs_skip[8];             /* the geometry passthrough's skip mask */
    u8 early_z;                 /* fragment tests run before the fragment program */
    u8 xfb_on;                  /* transform feedback asked for (its layout: the front end's) */
    u8 pad[2];
} DsPrograms;

/* The vertex attributes and streams, struct of arrays. Formats are the 3D engine's codes (vk/'s attr_format) */
typedef struct {
    u32 constant;            /* attributes without data: (0, 0, 0, 1) */
    u32 is_int, is_sint;     /* integer attributes, and the signed ones among them */
    u32 bgra;                /* attributes with their first and third components swapped */
    u8 stream[DS_ATTRS];     /* the stream an attribute reads */
    u8 size[DS_ATTRS];       /* the component sizes code */
    u8 type[DS_ATTRS];       /* 1 snorm, 2 unorm, 3 sint, 4 uint, 5 uscaled, 6 sscaled, 7 float */
    u16 offset[DS_ATTRS];    /* bytes into the stream's vertex */
    u32 stream_on;           /* the streams enabled */
    u32 instanced;           /* the streams stepped per instance */
    u16 stride[DS_STREAMS];  /* bytes */
    u32 divisor[DS_STREAMS]; /* instances per element of an instanced stream, 0 as asked (taken as 1) */
} DsVertexFormat;

/* Blending and the color write, struct of arrays over the targets */
typedef struct {
    u8 en[DS_TARGETS_MAX]; /* blending asked for (the renderer leaves it off on integer targets) */
    u8 cop[DS_TARGETS_MAX], sc[DS_TARGETS_MAX], dc[DS_TARGETS_MAX]; /* DS_BLEND_, DS_FACTOR_: color */
    u8 aop[DS_TARGETS_MAX], sa[DS_TARGETS_MAX], da[DS_TARGETS_MAX]; /* and alpha */
    u8 mask[DS_TARGETS_MAX];                                        /* the components written: VkColorComponentFlags */
    u8 logic_op; /* 0, or the logic operation in VkLogicOp's order plus 1 */
    u8 pad[3];
} DsBlend;

/* Rasterization state a Vulkan 1.3 pipeline is made with */
typedef struct {
    u8 polygon;        /* DS_POLYGON_ */
    u8 provoking_last; /* the last vertex provokes (Vulkan's default is the first) */
    u8 depth_clamp;    /* depth is clamped rather than clipped */
    u8 pad;
    u32 patch_vertices; /* control points of a patch, 0 as asked (the renderer takes it as 1) */
} DsRasterKey;

/* A render target as the guest lays it out, in the 3D engine's vocabulary (its surfaces are guest memory): what
   vk_rt_array and vk_zt_array take, before the renderer checks and clamps it */
typedef struct {
    u64 gva;   /* 0: none */
    u32 width; /* in texels, or the row pitch in bytes for a pitch-linear color target */
    u32 height;
    u32 format; /* the render target or zeta format code */
    u32 mem;    /* the memory layout word: block dimensions, pitch-linear bit 12, volume bit 16 */
    u32 layers; /* the array layers asked for, or a volume's depth (0 taken as 1) */
    u32 pad;
    u64 layer_stride; /* bytes between layers */
} DsSurf;

/* The targets a draw renders to */
typedef struct {
    u32 count;     /* color slots 0 to count - 1 are bound (some may have no surface) */
    u8 srgb_write; /* sRGB targets are written as sRGB */
    u8 pad[3];
    DsSurf color[DS_TARGETS_MAX]; /* by slot, filled up to count */
    DsSurf depth;                 /* gva 0 when no depth target is bound */
} DsTargets;

/* The vertex streams' memory */
typedef struct {
    u64 addr[DS_STREAMS];
    u64 size[DS_STREAMS]; /* from the address to the stream's limit, inclusive: 0 when the limit is below */
} DsVertexBuffers;

/* The index buffer and primitive restart */
typedef struct {
    u64 addr, end;     /* the first byte and the last */
    u32 format;        /* 0 8-bit, 1 16-bit, others 32-bit indices */
    u32 restart;       /* primitive restart is on */
    u32 restart_index; /* the index that restarts */
    u32 pad;
} DsIndex;

/* A draw's own arguments */
typedef struct {
    u32 topology;     /* the 3D engine's primitive numbers (14 a patch list) */
    u32 indexed;      /* 0 not, 1 from the index buffer, 3 the channel's inline indices */
    u32 first, count; /* vertices, or indices when indexed */
    u32 base_vertex, base_instance, instances;
    const u32* inline_indices; /* indexed 3: the owner's inline indices (host words), from first on */
} DrawCall;

/* DrawState's groups, in its order */
enum {
    DS_PROGRAMS,
    DS_VFORMAT,
    DS_BLEND,
    DS_RASTER_KEY,
    DS_XFB,
    DS_TARGETS,
    DS_DYNAMIC,
    DS_VIEWPORTS,
    DS_VBUF,
    DS_INDEX,
    DS_BIND,
    DS_INLINE,
    DS_TARGET_FORMATS, /* a generation without a struct: DsTargets' count, formats and sRGB write, which the pipeline
                          key reads where the rest of the targets change every pass */
    DS_GENS
};

/* Where a constant buffer slot's bytes are (DsBindings.cb_inline) */
enum { DS_CB_MEMORY, DS_CB_BANK0, DS_CB_BANK2 };

/* The resource bindings: what the guest memory a draw reads its constants, textures, images and storage from is.
   Texture handles are the 3D engine's (the header's index in bits 0-19, the sampler's in 20-31), naming entries of
   the TIC and TSC pools in guest memory. A front end without bound constant buffers in guest memory (NVN's driver
   banks) puts their bytes in DsInline and names that block here instead */
typedef struct {
    u64 cb_addr[DS_GFX_STAGES][DS_CBUFS];  /* each slot's GPU VA, 0 unbound */
    u32 cb_size[DS_GFX_STAGES][DS_CBUFS];  /* its bytes, 0 for 64 KiB */
    u8 cb_inline[DS_GFX_STAGES][DS_CBUFS]; /* DS_CB_: guest memory, or the stage's inline block 0 or 2 */
    u8 pad[2];
    u64 tex_pool, smp_pool; /* GPU VAs of the TIC and TSC entries, 32 bytes each */
    u32 tex_max, smp_max;   /* the highest index of each pool */
    u32 smp_mode;           /* the sampler index mode (0: the handle's sampler index) */
    u32 tex_bank;           /* the constant buffer bound textures' and images' handles are read from */
    struct {
        u64 addr; /* where capture starts: the buffer's address plus its start offset */
        u32 size; /* bytes, a signed count as the guest gives it */
        u32 on;   /* enabled, with a layout to capture into */
    } xfb[4];
} DsBindings;

/* Constant buffer blocks a front end gives inline (DsBindings.cb_inline): each graphics stage's two */
typedef struct {
    u8 bank0[DS_GFX_STAGES][DS_INLINE_BANK0], bank2[DS_GFX_STAGES][DS_INLINE_BANK2];
} DsInline;

/* Everything a draw reads but its arguments, written by its owner's front end (a GPU channel's 3D engine registers,
   mx_draw_state_sync) and only read by the renderer */
typedef struct {
    u32 owner; /* who draws: a GPU channel's id, the NVN replay's queues from 0x80000000 */
    u32 pad;
    DsPrograms prog;
    DsVertexFormat vfmt;
    DsBlend blend;
    DsRasterKey rkey;
    ShXfb xfb; /* what transform feedback captures, when prog.xfb_on */
    DsTargets tgt;
    DsDynamic dyn;
    DsViewports vp;
    DsVertexBuffers vbuf;
    DsIndex index;
    DsBindings bind;
    DsInline inl; /* its front end gives gen[DS_INLINE] a new generation when it changes them */
    /* each group's generation (DS_), a new one whenever its bytes change: from one counter for every owner, so two
       generations are equal only when they are the same state of the same owner */
    u64 gen[DS_GENS];
} DrawState;

/* a generation for DrawState.gen, from one sequence for every front end (gpu/draw_state.c) */
u64 ds_generation_next(void);

/* an inline constant buffer block: its bytes, NULL for a buffer in guest memory */
typedef struct {
    const u8* bytes;
    u32 size;
} DsCbBlock;
/* the inline block a slot's DS_CB_ kind names, of the banks bank0 and bank2 */
static inline DsCbBlock ds_cb_block(u32 kind, const u8* bank0, const u8* bank2) {
    if (kind == DS_CB_BANK0) return (DsCbBlock){bank0, DS_INLINE_BANK0};
    if (kind == DS_CB_BANK2) return (DsCbBlock){bank2, DS_INLINE_BANK2};
    return (DsCbBlock){NULL, 0};
}
/* stage's constant buffer bank as an inline block of ds */
static inline DsCbBlock ds_cb_inline(const DrawState* ds, int stage, u32 bank) {
    u32 k = bank < DS_CBUFS ? ds->bind.cb_inline[stage][bank] : DS_CB_MEMORY;
    return ds_cb_block(k, ds->inl.bank0[stage], ds->inl.bank2[stage]);
}

/* A compute dispatch, for any front end: everything the renderer reads of it (Renderer.dispatch). Its constant
   buffers are guest memory or the inline blocks bank0 and bank2 (NVN's driver banks: the dispatch size at 0x200 of
   bank 0, the uniform buffer table from 0x210, storage buffers from 0x310, texture handles in bank 2). Texture
   handles name the TIC and TSC entries of the pools in guest memory, as a draw's do */
#define DX_CBUFS 8 /* constant buffer slots of a dispatch */
typedef struct {
    u64 code;               /* the program's GPU VA */
    u32 grid[3];            /* blocks, x y z */
    u32 block[3];           /* threads of a block, x y z */
    u32 shared;             /* shared memory bytes */
    u32 cb_on;              /* the constant buffer slots bound (bit per slot) */
    u64 cb_addr[DX_CBUFS];  /* each slot's GPU VA */
    u32 cb_size[DX_CBUFS];  /* its bytes, 0 for 64 KiB */
    u8 cb_inline[DX_CBUFS]; /* DS_CB_: guest memory, or the inline block 0 or 2 */
    u64 tex_pool, smp_pool; /* GPU VAs of the TIC and TSC entries, 32 bytes each */
    u32 tex_max, smp_max;   /* the highest index of each pool */
    u32 smp_mode;           /* the sampler index mode (0: the handle's sampler index) */
    u32 tex_bank;           /* the constant buffer bound textures' and images' handles are read from */
    u8 bank0[DS_INLINE_BANK0], bank2[DS_INLINE_BANK2]; /* the inline blocks, as DsInline's for a stage */
} DispatchState;
/* slot's constant buffer of the dispatch d as an inline block */
static inline DsCbBlock dx_cb_inline(const DispatchState* d, u32 slot) {
    return ds_cb_block(slot < DX_CBUFS ? d->cb_inline[slot] : DS_CB_MEMORY, d->bank0, d->bank2);
}
/* the word at byte off of an inline block: 0 past its end, as an unbound buffer reads */
static inline u32 ds_inline_word(DsCbBlock b, u64 off) {
    return b.size >= 4 && off <= b.size - 4 ? snx_ld_le32(b.bytes + off) : 0;
}

/* What a draw takes of its owner besides its state, which the front end and the renderer share */
typedef struct {
    u32 pred;       /* the render enable's predicate (renderer.h render_predicate), 0 for none: the front end's */
    u8 xfb_reload;  /* the capture buffers whose start offset the front end set: their capture starts over there */
    u8 xfb_started; /* those whose capture began, which resumes from its counter: the renderer's */
    u8 pad[2];
} DrawOwner;

/* A clear of the targets a DrawState names */
typedef struct {
    u32 aspects;    /* bit 0 depth, bit 1 stencil */
    u32 color_mask; /* the color components cleared, R in bit 0 to A in bit 3: 0 clears no color */
    u32 slot;       /* the color slot */
    u32 layer;
    u32 color[4];  /* the clear color's words: floats, or an integer target's integers */
    float depth;   /* as given (the renderer clamps it) */
    u32 stencil;   /* its low 8 bits */
    u32 scissored; /* limited to viewport 0's scissor */
} DrawClear;

/* A texture's rectangle drawn onto color target 0 (the 3D engine's DRAW_TEXTURE), in pixels */
typedef struct {
    u32 handle;         /* the texture's handle: its header's index in bits 0-19, its sampler's in 20-31 */
    float x0, y0, w, h; /* the destination, y measured as the window origin says */
    double du, dv;      /* source texels per destination pixel */
    float sx0, sy0;     /* the source's origin */
} DrawTexture;

#endif
