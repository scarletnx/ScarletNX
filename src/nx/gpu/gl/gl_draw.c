/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's draws and clears on the console (gl_layer.h): each the renderer's draw or clear of a DsState the layer
   makes from the context's GL state, as the NVN replay makes one from NVN's (gpu/nvn/nvn_draw.c), with NVN's lower
   left window origin and depth from -1 to 1, which the game's own GL driver sets NVN to.

   The program is a pair of host programs (Renderer.host_program, gl_host_program.h), given once a link. The vertex
   streams and the element array are the buffers' bytes in the layer's memory, at their GPU addresses. Each stage's
   default uniform block and its texture handles go into a ring of the layer's memory, bound as constant buffers 0 and
   GL_HOST_TEX_BANK; the uniform blocks are the buffers bound at the program's binding points. Each texture bound is
   an entry of a TIC pool, at its name, and its sampler state (or the sampler object's) one of a TSC pool, both in the
   layer's memory. The targets are the bound framebuffer's textures and renderbuffers, laid out as NVN's, or the window
   surface's image and depth (gl_host_nx.c).

   The game's thread makes a record of the state groups under the object lock and hands it to the GPU thread, which
   keeps a DsState for each context, gives a group a new generation when its bytes changed, and draws, in order with
   the uploads and the presents. The renderer reads a draw's buffers there, so a draw names them to the shared layer
   (snx_gl_buffer_read_by), which gives a buffer written after it a new range */
#include "common/gpu/gl/gl_format.h"
#include "common/gpu/gl/gl_framebuffer.h"
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_state.h"
#include "common/gpu/gl/gl_texture.h"
#include "nx/gpu/gl/gl_host_program.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/hle/hook/hook.h"
#include "nx/gpu/gpu.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_tex.h"
#include "nx/gpu/nvn/nvn_tic.h"
#include "nx/gpu/renderer.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <math.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define RING_BYTES   (16u << 20)                             /* the per-draw constants' ring */
#define TSC_ENTRIES  (SNX_GL_TEXTURES + SNX_GL_SAMPLERS + 1) /* a texture's own state at its name, then samplers' */
#define OWNER_BASE   0x40000000u                             /* DsState.owner of context c: OWNER_BASE + c */
#define GENERIC_BASE 16u                                     /* the streams of attributes without arrays */

enum {
    GL_FRONT_ = 0x0404,
    GL_BACK_ = 0x0405,
    GL_FRONT_AND_BACK_ = 0x0408,
    GL_CW_ = 0x0900,
    GL_BYTE_ = 0x1400,
    GL_UNSIGNED_BYTE_ = 0x1401,
    GL_SHORT_ = 0x1402,
    GL_UNSIGNED_SHORT_ = 0x1403,
    GL_INT_ = 0x1404,
    GL_UNSIGNED_INT_ = 0x1405,
    GL_FLOAT_ = 0x1406,
    GL_HALF_FLOAT_ = 0x140b,
    GL_HALF_FLOAT_OES_ = 0x8d61,
    GL_BGRA_ = 0x80e1,
    GL_UNSIGNED_INT_2_10_10_10_REV_ = 0x8368,
    GL_INT_2_10_10_10_REV_ = 0x8d9f,
    GL_UNSIGNED_INT_10F_11F_11F_REV_ = 0x8c3b,
    GL_POINT_ = 0x1b00,
    GL_LINE_ = 0x1b01,
    GL_COLOR_ATTACHMENT0_ = 0x8ce0,
    GL_KEEP_ = 0x1e00,
    GL_REPLACE_ = 0x1e01,
    GL_INCR_ = 0x1e02,
    GL_DECR_ = 0x1e03,
    GL_INVERT_ = 0x150a,
    GL_INCR_WRAP_ = 0x8507,
    GL_DECR_WRAP_ = 0x8508,
    GL_FUNC_ADD_ = 0x8006,
    GL_MIN_ = 0x8007,
    GL_MAX_ = 0x8008,
    GL_FUNC_SUBTRACT_ = 0x800a,
    GL_FUNC_REVERSE_SUBTRACT_ = 0x800b,
    GL_COMPARE_REF_TO_TEXTURE_ = 0x884e,
};

/* the groups of a draw's DsState the game's thread makes, and the draw itself */
typedef struct {
    u32 ctx, program, generation; /* the program's name and link, which the GPU thread has as host programs */
    int clear;
    DsVertexFormat vfmt;
    DsBlend blend;
    DsRasterKey rkey;
    DsTargets tgt;
    DsDynamic dyn;
    DsViewports vp;
    DsVertexBuffers vbuf;
    DsIndex index;
    DsBindings bind;
    DrawCall call;
    DrawClear clear_args;
    u64 ring_end; /* the ring's bytes this record's constants end at */
    u64 serial;   /* the draws and clears handed to the GPU thread, this one included */
} DrawRecord;

/* a program's link for the GPU thread: each stage's SPIR-V and description */
typedef struct {
    u32 program, generation;
    ShaderInfo si[2];
    u32 words[2];
    u32* spirv[2];
} ProgramRecord;

static struct {
    /* the game's side, under the object lock */
    u64 tic, tsc;                   /* the pools' guest addresses */
    u64 ring;                       /* the ring's guest address */
    u64 head;                       /* bytes written into the ring, from its start */
    u32 sent[SNX_GL_PROGRAM_NAMES]; /* each program's link handed to the GPU thread, 0 for none */
    u64 issued;                     /* the draws, clears and uploads handed to the GPU thread */
    /* the GPU thread's */
    _Atomic u64 done;      /* the ring's bytes the GPU thread finished with */
    _Atomic u64 completed; /* the draws, clears and uploads it finished, a fence's serial (gl_sync.h) */
    DsState* state[SNX_GL_CONTEXTS];
    DrawOwner owners[SNX_GL_CONTEXTS]; /* what each context's draws take of it besides their state */
    struct {
        u32 generation, vs, fs;
    } ids[SNX_GL_PROGRAM_NAMES];
} g_gl_draw;

/* ---- the layer's memory for the draws ---- */

/* the pools and the ring, taken at the first draw: 0, or -1 when the layer's memory has no room (said) */
static int memory_ready(void) {
    if (g_gl_draw.ring) return 0;
    g_gl_draw.tic = snx_gl_memory_take((SNX_GL_TEXTURES + 1) * 32ull, 256);
    g_gl_draw.tsc = snx_gl_memory_take(TSC_ENTRIES * 32ull, 256);
    g_gl_draw.ring = snx_gl_memory_take(RING_BYTES, 256);
    if (!g_gl_draw.tic || !g_gl_draw.tsc || !g_gl_draw.ring) {
        LOG_LIMIT(1, "gl layer: no room in its memory for the draws' pools and %u-byte ring: nothing is drawn\n",
                  RING_BYTES);
        return -1;
    }
    memset(GP(g_gl_draw.tic), 0, (SNX_GL_TEXTURES + 1) * 32ull);
    memset(GP(g_gl_draw.tsc), 0, TSC_ENTRIES * 32ull);
    gpu_mem_written_cpu(g_gl_draw.tic, (SNX_GL_TEXTURES + 1) * 32ull);
    gpu_mem_written_cpu(g_gl_draw.tsc, TSC_ENTRIES * 32ull);
    return 0;
}
/* size bytes of the ring, 256-aligned, once the GPU thread finished with the bytes they take the place of: their
   guest address */
static u64 ring_take(u64 size) {
    u64 at = (g_gl_draw.head + 255) & ~255ull, offset = at % RING_BYTES;
    size = (size + 255) & ~255ull;
    if (offset + size > RING_BYTES) at += RING_BYTES - offset, offset = 0; /* a block never wraps */
    if (at + size - atomic_load_explicit(&g_gl_draw.done, memory_order_acquire) > RING_BYTES) {
        hook_wait_begin(); /* off the guest core, as every wait of the layer's */
        while (at + size - atomic_load_explicit(&g_gl_draw.done, memory_order_acquire) > RING_BYTES)
            plat_yield(); /* the GPU thread is a ring behind: the draws it has not reached hold these bytes */
        hook_wait_end();
    }
    g_gl_draw.head = at + size;
    return g_gl_draw.ring + offset;
}

/* size bytes of src into the ring: their guest address. The GPU's caches are told of them, as of every byte the
   runtime writes into guest memory the GPU reads: the ring's addresses come round again with new bytes */
static u64 ring_put(const void* src, u64 size) {
    u64 at = ring_take(size);
    memcpy(GP(at), src, size);
    gpu_mem_written_cpu(at, size);
    return at;
}
/* a texture or sampler descriptor's 8 words into its pool entry at guest address slot, when they are new there: the
   draws handed to the GPU thread before it read the entry as it was, so they are drawn first, as nvn_object.c's
   sampler_write waits for the replay; the GPU's caches are told */
static void descriptor_put(u64 slot, const u32 words[8]) {
    if (!memcmp(GP(slot), words, 32)) return;
    gl_draw_wait(0, g_gl_draw.issued);
    memcpy(GP(slot), words, 32);
    gpu_mem_written_cpu(slot, 32);
}

/* ---- textures and samplers ---- */

static u8 nvn_wrap(u32 gl) {
    switch (gl) {
        case 0x812f: return NVN_WRAP_MODE_CLAMP_TO_EDGE;
        case 0x8370: return NVN_WRAP_MODE_MIRRORED_REPEAT;
        case 0x812d: return NVN_WRAP_MODE_CLAMP_TO_BORDER;
        case 0x8743: return NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE;
        default: return NVN_WRAP_MODE_REPEAT;
    }
}
/* a sampler state's TSC entry at index */
static void sampler_entry(u32 index, const SnxGlSamplerState* s) {
    NvnSamplerPar p = {0};
    u32 words[8];
    p.min_filter = (u8)(s->min >= 0x2700 && s->min <= 0x2703 ? NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST + (s->min - 0x2700)
                        : s->min == 0x2601                   ? NVN_MIN_FILTER_LINEAR
                                                             : NVN_MIN_FILTER_NEAREST);
    /* GL counts its mipmap filters nearest-nearest, linear-nearest, nearest-linear, linear-linear, as NVN does */
    p.mag_filter = s->mag == 0x2601 ? NVN_MAG_FILTER_LINEAR : NVN_MAG_FILTER_NEAREST;
    p.wrap_s = nvn_wrap(s->wrap[0]), p.wrap_t = nvn_wrap(s->wrap[1]), p.wrap_r = nvn_wrap(s->wrap[2]);
    p.compare_mode = s->compare_mode == GL_COMPARE_REF_TO_TEXTURE_;
    p.compare_func = (u8)((s->compare_func & 7) + NVN_COMPARE_FUNC_NEVER);
    p.lod_min = s->min_lod, p.lod_max = s->max_lod, p.lod_bias = s->lod_bias;
    p.anisotropy = s->anisotropy;
    nvn_tsc_encode(&p, words);
    descriptor_put(g_gl_draw.tsc + (u64)index * 32, words);
}
/* the texture's TIC entry at its name, its levels from its base level to its last complete one: 0 for none (a
   texture GL samples as incomplete, zeros) */
static int texture_entry(u32 name) {
    const SnxGlTexture* t = snx_gl_texture(name);
    NvnTexture n;
    int32_t levels;
    u32 words[8];
    if (!t || !snx_gl_texture_complete(name, &levels) || !gl_nvn_texture(name, &n) || !n.gpu) return 0;
    NvnView v = {.set = NVN_VIEW_LEVELS, .base_level = (u8)t->base_level, .levels = (u8)levels};
    if (!nvn_tic_encode(&n, &v, 0, 0, words)) return 0;
    descriptor_put(g_gl_draw.tic + (u64)name * 32, words);
    return 1;
}
/* the handles of stage's texture slots (si's), the textures bound to the units the program's samplers name: into the
   ring, bound as constant buffer GL_HOST_TEX_BANK of the stage */
static void texture_handles(u32 ctx, const SnxGlProgram* p, const ShaderInfo* si, u32 stage, DsBindings* b) {
    const SnxGlBinding* bindings = snx_gl_program_bindings(p);
    const int32_t* units = snx_gl_program_units(p);
    u32 handles[SH_MAXTEX] = {0};
    u64 at;
    if (!si->ntex) return;
    for (u32 i = 0; i < p->binding_count; i++) {
        const SnxGlBinding* x = &bindings[i];
        u32 k = x->binding - SNX_VKB_TEX, unit, texture, sampler;
        if (x->kind != SNX_GLSLC_REFLECT_SAMPLER || x->binding < SNX_VKB_TEX || k >= SH_MAXTEX) continue;
        unit = (u32)units[x->first];
        texture = snx_gl_unit_texture(ctx, unit, x->dim);
        if (!texture_entry(texture)) continue;
        sampler = unit < SNX_GL_UNITS ? snx_gl_textures(ctx)->samplers[unit] : 0;
        if (sampler) {
            sampler_entry(SNX_GL_TEXTURES + sampler, &snx_gl_sampler(sampler)->state);
            handles[k] = texture | (SNX_GL_TEXTURES + sampler) << 20;
        } else {
            sampler_entry(texture, &snx_gl_texture(texture)->sampler);
            handles[k] = texture | texture << 20;
        }
    }
    at = ring_put(handles, sizeof handles);
    b->cb_addr[stage][GL_HOST_TEX_BANK] = gl_gpu_address(at);
    b->cb_size[stage][GL_HOST_TEX_BANK] = sizeof handles;
}

/* ---- the state groups ---- */

/* a GL vertex attribute's 3D engine size code and type (1 snorm, 2 unorm, 3 sint, 4 uint, 5 uscaled, 6 sscaled,
   7 float), as nvn_draw.c's attr_format gives NVN's: 0 for one no attribute takes */
static int attr_format(const SnxGlAttrib* a, u8* size, u8* type, int* bgra) {
    static const u8 by_comps[3][4] = {{0x1D, 0x18, 0x13, 0x0A}, {0x1B, 0x0F, 0x05, 0x03}, {0x12, 0x04, 0x02, 0x01}};
    int comps = a->size == GL_BGRA_ ? 4 : a->size, bits, sign;
    *bgra = a->size == GL_BGRA_;
    switch (a->type) {
        case GL_UNSIGNED_INT_2_10_10_10_REV_: *size = 0x30, *type = a->normalized ? 2 : 5; return 1;
        case GL_INT_2_10_10_10_REV_: *size = 0x30, *type = a->normalized ? 1 : 6; return 1;
        case GL_UNSIGNED_INT_10F_11F_11F_REV_: *size = 0x31, *type = 7; return 1;
        case GL_BYTE_:
        case GL_UNSIGNED_BYTE_: bits = 0; break;
        case GL_SHORT_:
        case GL_UNSIGNED_SHORT_:
        case GL_HALF_FLOAT_:
        case GL_HALF_FLOAT_OES_: bits = 1; break;
        case GL_INT_:
        case GL_UNSIGNED_INT_:
        case GL_FLOAT_: bits = 2; break;
        default: return 0;
    }
    if (comps < 1 || comps > 4) return 0;
    sign = a->type == GL_BYTE_ || a->type == GL_SHORT_ || a->type == GL_INT_;
    *size = by_comps[bits][comps - 1];
    if (a->type == GL_FLOAT_ || a->type == GL_HALF_FLOAT_ || a->type == GL_HALF_FLOAT_OES_)
        *type = 7;
    else if (a->integer)
        *type = sign ? 3 : 4;
    else if (a->normalized)
        *type = sign ? 1 : 2;
    else
        *type = sign ? 6 : 5;
    return 1;
}
/* the vertex arrays' attributes the vertex program reads, and their streams: the bound buffers' bytes, or each
   attribute without an array its generic value, a 16-byte vertex in the ring that every vertex reads. The program's
   input at the module's location i reads the attribute of its GL location (glBindAttribLocation's), which can differ */
static int vertex_state(u32 ctx, const SnxGlProgram* p, const ShaderInfo* vs, DrawRecord* r, u64 serial) {
    const SnxGlVertexArray* va = snx_gl_vertex_array(ctx);
    const SnxGlObjectContext* o = snx_gl_objects(ctx);
    const SnxGlInput* in = snx_gl_program_inputs(p);
    DsVertexFormat* f = &r->vfmt;
    u8 gl_of[DS_ATTRS];
    for (u32 i = 0; i < DS_ATTRS; i++) gl_of[i] = (u8)i;
    for (u32 k = 0; k < p->input_count; k++)
        for (u32 e = 0; e < (u32)in[k].count * in[k].columns; e++)
            if (in[k].spirv >= 0 && in[k].location >= 0 && (u32)in[k].spirv + e < DS_ATTRS &&
                (u32)in[k].location + e < SNX_GL_VERTEX_ATTRIBS)
                gl_of[in[k].spirv + e] = (u8)(in[k].location + e);
    for (u32 i = 0; i < DS_ATTRS; i++) {
        const u32 g = gl_of[i];
        const SnxGlAttrib* a;
        u8 size, type;
        int bgra;
        if (!(vs->in_mask >> i & 1) || g >= SNX_GL_VERTEX_ATTRIBS) continue;
        a = &va->attribs[g];
        if (a->enabled) {
            const SnxGlVertexBinding* b = &va->bindings[a->binding];
            const SnxGlBuffer* buf = snx_gl_buffer(b->buffer);
            if (!buf || !attr_format(a, &size, &type, &bgra)) {
                LOG_LIMIT(4, "gl layer: attribute %u reads %s (type 0x%x, size %d): the draw is skipped\n", (unsigned)g,
                          buf ? "a format no attribute takes" : "a client array, which a core context has not",
                          (unsigned)a->type, (int)a->size);
                return 0;
            }
            f->stream[i] = a->binding, f->offset[i] = (u16)a->relative;
            f->size[i] = size, f->type[i] = type;
            if (type == 3 || type == 4) f->is_int |= 1u << i;
            if (type == 3) f->is_sint |= 1u << i;
            if (bgra) f->bgra |= 1u << i;
            f->stream_on |= 1u << a->binding;
            f->stride[a->binding] = (u16)b->stride;
            if (b->divisor) f->instanced |= 1u << a->binding, f->divisor[a->binding] = b->divisor;
            r->vbuf.addr[a->binding] = gl_gpu_address(buf->data) + b->offset;
            r->vbuf.size[a->binding] = buf->size > b->offset ? buf->size - b->offset : 0;
            snx_gl_buffer_read_by(b->buffer, serial);
        } else { /* glVertexAttrib*'s value, at stride 0 */
            u32 s = GENERIC_BASE + g;
            u64 at = ring_put(o->generic[g].bits, 16);
            f->stream[i] = (u8)s, f->offset[i] = 0;
            f->size[i] = 0x01; /* 32_32_32_32 */
            f->type[i] = o->generic[g].base == SNX_GLSLC_BASE_INT    ? 3
                         : o->generic[g].base == SNX_GLSLC_BASE_UINT ? 4
                                                                     : 7;
            if (f->type[i] != 7) f->is_int |= 1u << i;
            if (f->type[i] == 3) f->is_sint |= 1u << i;
            f->stream_on |= 1u << s;
            f->stride[s] = 0;
            r->vbuf.addr[s] = gl_gpu_address(at), r->vbuf.size[s] = 16;
        }
    }
    return 1;
}
static u8 blend_factor(u32 f) {
    switch (f) {
        case 0: return DS_FACTOR_ZERO;
        case 0x300: return DS_FACTOR_SRC_COLOR;
        case 0x301: return DS_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return DS_FACTOR_SRC_ALPHA;
        case 0x303: return DS_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return DS_FACTOR_DST_ALPHA;
        case 0x305: return DS_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return DS_FACTOR_DST_COLOR;
        case 0x307: return DS_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return DS_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return DS_FACTOR_CONSTANT_COLOR;
        case 0x8002: return DS_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return DS_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return DS_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        case 0x88f9: return DS_FACTOR_SRC1_COLOR;
        case 0x88fa: return DS_FACTOR_ONE_MINUS_SRC1_COLOR;
        case 0x8589: return DS_FACTOR_SRC1_ALPHA;
        case 0x88fb: return DS_FACTOR_ONE_MINUS_SRC1_ALPHA;
        default: return DS_FACTOR_ONE;
    }
}
static u8 blend_op(u32 e) {
    switch (e) {
        case GL_FUNC_SUBTRACT_: return DS_BLEND_SUBTRACT;
        case GL_FUNC_REVERSE_SUBTRACT_: return DS_BLEND_REVERSE_SUBTRACT;
        case GL_MIN_: return DS_BLEND_MIN;
        case GL_MAX_: return DS_BLEND_MAX;
        default: return DS_BLEND_ADD;
    }
}
static u8 stencil_op(u32 op) {
    switch (op) {
        case 0: return DS_STENCIL_ZERO;
        case GL_REPLACE_: return DS_STENCIL_REPLACE;
        case GL_INCR_: return DS_STENCIL_INCREMENT_AND_CLAMP;
        case GL_DECR_: return DS_STENCIL_DECREMENT_AND_CLAMP;
        case GL_INVERT_: return DS_STENCIL_INVERT;
        case GL_INCR_WRAP_: return DS_STENCIL_INCREMENT_AND_WRAP;
        case GL_DECR_WRAP_: return DS_STENCIL_DECREMENT_AND_WRAP;
        default: return DS_STENCIL_KEEP;
    }
}
/* a viewport's or scissor's rectangle as the 3D engine's 16-bit start and extent, as nvn_draw.c clamps them */
static u32 clip16(double lo, double hi) {
    if (!isfinite(lo) || !isfinite(hi) || hi <= 0 || lo >= 65535 || hi <= lo) return 0;
    u32 start = lo < 0 ? 0 : (u32)lo, end = hi > 65535 ? 65535 : (u32)hi;
    return start | (end - start) << 16;
}
/* an attachment as a render target: a texture's level and layer, or a renderbuffer, laid out as NVN's */
static int gl_attachment_surface(const SnxGlAttachment* a, DsSurf* s, int* depth, u32* msaa) {
    NvnTexture n;
    NvnView v = {0};
    if (a->kind == SNX_GL_ATTACH_TEXTURE) {
        if (!gl_nvn_texture(a->name, &n)) return 0;
        v = (NvnView){.set = NVN_VIEW_LEVELS | NVN_VIEW_LAYERS,
                      .base_level = (u8)a->level,
                      .levels = 1,
                      .min_layer = (u16)a->layer,
                      .layers = 1};
    } else if (!gl_nvn_renderbuffer(a->name, &n)) {
        return 0;
    }
    return n.gpu && nvn_draw_surface_of(&n, v.set ? &v : NULL, s, depth, msaa);
}
/* the targets: the bound draw framebuffer's draw buffers and depth and stencil, or the window surface's. 0 when there
   is none to draw into */
static int targets(u32 ctx, DsTargets* t) {
    u32 fb = snx_gl_framebuffers(ctx)->draw, depth;
    const SnxGlFramebuffer* f = snx_gl_framebuffer(fb);
    u32 msaa = DS_MSAA_1X1;
    memset(t, 0, sizeof *t);
    if (!f) return gl_host_surface_targets(gl_egl_draw(), t);
    for (u32 i = 0; i < SNX_GL_COLORS; i++) {
        u32 which = f->draw[i];
        if (which < GL_COLOR_ATTACHMENT0_ || which >= GL_COLOR_ATTACHMENT0_ + SNX_GL_COLORS) continue;
        if (!gl_attachment_surface(&f->at[which - GL_COLOR_ATTACHMENT0_], &t->color[i], (int*)&depth, &msaa)) continue;
        t->count = i + 1;
    }
    {
        const SnxGlAttachment* d =
            f->at[SNX_GL_DEPTH_ATTACHMENT].kind ? &f->at[SNX_GL_DEPTH_ATTACHMENT] : &f->at[SNX_GL_STENCIL_ATTACHMENT];
        u32 dmsaa = DS_MSAA_1X1;
        if (d->kind && gl_attachment_surface(d, &t->depth, (int*)&depth, &dmsaa) && dmsaa != msaa)
            t->depth_msaa = (u8)(dmsaa + 1);
    }
    t->msaa = (u8)msaa;
    return t->count || t->depth.gva;
}
static u32 surface_height(const DsTargets* t) {
    for (u32 i = 0; i < t->count; i++)
        if (t->color[i].gva) return t->color[i].height >> ds_msaa_log2_y(t->msaa);
    return t->depth.gva ? t->depth.height >> ds_msaa_log2_y(ds_depth_msaa(t)) : 0;
}
/* the fixed state's groups: blending, rasterization, the dynamic state and the viewports */
static void fixed_state(u32 ctx, DrawRecord* r) {
    const SnxGlFixed* g = snx_gl_fixed(ctx);
    DsDynamic* y = &r->dyn;
    DsViewports* v = &r->vp;
    float n = g->depth_range[0], fz = g->depth_range[1];
    for (u32 i = 0; i < DS_TARGETS_MAX; i++) {
        r->blend.en[i] = g->blend;
        r->blend.cop[i] = blend_op(g->blend_eq[0]), r->blend.aop[i] = blend_op(g->blend_eq[1]);
        r->blend.sc[i] = blend_factor(g->blend_src[0]), r->blend.dc[i] = blend_factor(g->blend_dst[0]);
        r->blend.sa[i] = blend_factor(g->blend_src[1]), r->blend.da[i] = blend_factor(g->blend_dst[1]);
        r->blend.mask[i] =
            (u8)(g->color_mask[0] | g->color_mask[1] << 1 | g->color_mask[2] << 2 | g->color_mask[3] << 3);
    }
    r->rkey.polygon = (u8)(g->polygon_mode == GL_POINT_  ? DS_POLYGON_POINT
                           : g->polygon_mode == GL_LINE_ ? DS_POLYGON_LINE
                                                         : DS_POLYGON_FILL);
    r->rkey.provoking_last = 1; /* GL's last vertex convention */
    r->rkey.depth_pixel_clamp = 1;
    r->rkey.depth_zero_one = 1;
    r->rkey.alpha_to_coverage = g->alpha_to_coverage;
    r->rkey.sample_mask = g->sample_mask_on ? g->sample_mask : ~0u;
    y->cull = !g->cull                             ? DS_CULL_NONE
              : g->cull_face == GL_FRONT_          ? DS_CULL_FRONT
              : g->cull_face == GL_FRONT_AND_BACK_ ? DS_CULL_FRONT_AND_BACK
                                                   : DS_CULL_BACK;
    y->front_cw = g->front_face == GL_CW_;
    y->discard = g->rasterizer_discard;
    y->bias_on = g->polygon_offset ? 4 : 0;
    y->depth_test = g->depth_test, y->depth_write = g->depth_mask;
    y->depth_cmp = (u8)((g->depth_func - 0x200) & 7);
    y->stencil_test = g->stencil_test;
    y->window_origin = DS_ORIGIN_FLIP_Y; /* GL's lower left origin, as NVN's driver flips for it */
    y->sprite_origin = 1, y->point_nudge = 1;
    for (int k = 0; k < 2; k++) {
        y->face[k].fail = stencil_op(g->stencil[k].fail), y->face[k].zfail = stencil_op(g->stencil[k].zfail);
        y->face[k].pass = stencil_op(g->stencil[k].zpass), y->face[k].cmp = (u8)((g->stencil[k].func - 0x200) & 7);
        y->face[k].ref = (u32)g->stencil[k].ref, y->face[k].cmask = g->stencil[k].mask;
        y->face[k].wmask = g->stencil[k].write;
    }
    y->line_width = g->line_width;
    y->bias_slope = g->offset_factor, y->bias_units = g->offset_units * 2.0f; /* the engine's units are twice GL's */
    memcpy(y->blend_const, g->blend_color, sizeof y->blend_const);
    y->clip_height = surface_height(&r->tgt);
    v->transform_on = 1;
    v->scissor_on = (1u << DS_VIEWPORTS_MAX) - 1;
    for (u32 j = 0; j < DS_VIEWPORTS_MAX; j++) {
        float x = (float)g->viewport[0], yy = (float)g->viewport[1], w = (float)g->viewport[2],
              h = (float)g->viewport[3];
        v->scale[j][0] = w * 0.5f, v->scale[j][1] = h * 0.5f, v->scale[j][2] = (fz - n) * 0.5f;
        v->translate[j][0] = x + w * 0.5f, v->translate[j][1] = yy + h * 0.5f, v->translate[j][2] = (n + fz) * 0.5f;
        v->swizzle[j] = DS_SWIZZLE_IDENTITY;
        v->clip_x[j] = clip16(floor(x), ceil(x + w)), v->clip_y[j] = clip16(floor(yy), ceil(yy + h));
        v->z_near[j] = n, v->z_far[j] = fz;
        if (g->scissor_test) {
            v->scissor_x[j] = clip16(g->scissor[0], (double)g->scissor[0] + g->scissor[2]);
            v->scissor_y[j] = clip16(g->scissor[1], (double)g->scissor[1] + g->scissor[3]);
        } else {
            v->scissor_x[j] = v->scissor_y[j] = 65535u << 16;
        }
    }
}
/* the program's constant buffers: each stage's default block in the ring, the uniform blocks at their buffers */
static void constants(u32 ctx, const SnxGlProgram* p, const ShaderInfo si[2], DrawRecord* r, u64 serial) {
    const SnxGlBinding* bindings = snx_gl_program_bindings(p);
    const SnxGlObjectContext* o = snx_gl_objects(ctx);
    for (u32 s = 0; s < 2; s++) {
        u32 stage = s ? 4 : 0; /* DsBindings' stages: VS, TCS, TES, GS, FS */
        if (p->default_size[s]) {
            u64 at = ring_put(snx_gl_program_default(p, (int)s), p->default_size[s]);
            r->bind.cb_addr[stage][0] = gl_gpu_address(at);
            r->bind.cb_size[stage][0] = p->default_size[s];
        }
        for (u32 i = 0; i < p->binding_count; i++) {
            const SnxGlBinding* x = &bindings[i];
            u32 slot = x->binding - SNX_VKB_CB, point;
            const SnxGlIndexedBuffer* u;
            const SnxGlBuffer* buf;
            if (x->kind != SNX_GLSLC_REFLECT_UNIFORM_BLOCK || !x->name || !(x->stages >> s & 1) || !slot ||
                slot >= GL_HOST_TEX_BANK)
                continue;
            point = x->first < SNX_GL_UBO_BINDINGS ? p->block_binding[x->first] : 0;
            u = point < SNX_GL_UNIFORM_BINDINGS ? &o->uniforms[point] : NULL;
            buf = u ? snx_gl_buffer(u->buffer) : NULL;
            if (!buf || (u64)u->offset >= buf->size) continue;
            r->bind.cb_addr[stage][slot] = gl_gpu_address(buf->data) + (u64)u->offset;
            snx_gl_buffer_read_by(u->buffer, serial);
            r->bind.cb_size[stage][slot] =
                (u32)(u->size < 0 || (u64)u->offset + (u64)u->size > buf->size ? buf->size - (u64)u->offset
                                                                               : (u64)u->size);
        }
        texture_handles(ctx, p, &si[s], stage, &r->bind);
    }
    r->bind.tex_pool = gl_gpu_address(g_gl_draw.tic), r->bind.tex_max = SNX_GL_TEXTURES;
    r->bind.smp_pool = gl_gpu_address(g_gl_draw.tsc), r->bind.smp_max = TSC_ENTRIES - 1;
    r->bind.tex_bank = GL_HOST_TEX_BANK;
}

/* ---- the GPU thread ---- */

static void program_on_gpu(void* arg) {
    ProgramRecord* m = arg;
    u32 vs = 0, fs = 0;
    if (g_renderer_up && g_renderer->host_program) {
        vs = g_renderer->host_program(ST_VS, m->spirv[0], m->words[0], &m->si[0]);
        fs = m->words[1] ? g_renderer->host_program(ST_FS, m->spirv[1], m->words[1], &m->si[1]) : 0;
    }
    g_gl_draw.ids[m->program - 1].generation = m->generation;
    g_gl_draw.ids[m->program - 1].vs = vs, g_gl_draw.ids[m->program - 1].fs = fs;
    if (!vs || (m->words[1] && !fs))
        LOG_LIMIT(4, "gl layer: the renderer took no host programs for program %u: its draws are skipped\n",
                  (unsigned)m->program);
    free(m->spirv[0]);
    free(m->spirv[1]);
    free(m);
}
/* a group of the context's DsState: its bytes and a new generation when they changed */
static void keep(void* kept, const void* made, size_t n, u64* gen) {
    if (!memcmp(kept, made, n)) return;
    memcpy(kept, made, n);
    *gen = ds_generation_next();
}
static void draw_on_gpu(void* arg) {
    DrawRecord* r = arg;
    DsState* s = g_gl_draw.state[r->ctx - 1];
    if (!s) {
        s = g_gl_draw.state[r->ctx - 1] = calloc(1, sizeof *s);
        if (!s) fatal("gl layer: out of memory for context %u's draw state", (unsigned)r->ctx);
        s->owner = OWNER_BASE + r->ctx;
        for (int i = 0; i < DS_GENS; i++) s->gen[i] = ds_generation_next();
    }
    if (!r->clear) {
        DsPrograms p = {.on = 1u << 1 | 1u << 5};
        if (g_gl_draw.ids[r->program - 1].generation != r->generation || !g_gl_draw.ids[r->program - 1].vs) goto done;
        p.code[1] = DS_HOST_PROGRAM | g_gl_draw.ids[r->program - 1].vs;
        p.code[5] = DS_HOST_PROGRAM | g_gl_draw.ids[r->program - 1].fs;
        if (!g_gl_draw.ids[r->program - 1].fs) p.on = 1u << 1;
        keep(&s->prog, &p, sizeof p, &s->gen[DS_PROGRAMS]);
        keep(&s->vfmt, &r->vfmt, sizeof r->vfmt, &s->gen[DS_VFORMAT]);
        keep(&s->blend, &r->blend, sizeof r->blend, &s->gen[DS_BLEND]);
        keep(&s->rkey, &r->rkey, sizeof r->rkey, &s->gen[DS_RASTER_KEY]);
        keep(&s->vbuf, &r->vbuf, sizeof r->vbuf, &s->gen[DS_VBUF]);
        keep(&s->index, &r->index, sizeof r->index, &s->gen[DS_INDEX]);
        keep(&s->bind, &r->bind, sizeof r->bind, &s->gen[DS_BIND]);
    }
    if (memcmp(&s->tgt, &r->tgt, sizeof r->tgt)) s->gen[DS_TARGET_FORMATS] = ds_generation_next();
    keep(&s->tgt, &r->tgt, sizeof r->tgt, &s->gen[DS_TARGETS]);
    keep(&s->dyn, &r->dyn, sizeof r->dyn, &s->gen[DS_DYNAMIC]);
    keep(&s->vp, &r->vp, sizeof r->vp, &s->gen[DS_VIEWPORTS]);
    if (g_renderer_up && r->clear && g_renderer->clear) { /* each color target, the depth and stencil with the first */
        DrawClear c = r->clear_args;
        u32 colors = c.color_mask ? s->tgt.count : 0;
        for (u32 i = 0; i < colors || (i == 0 && c.aspects); i++) {
            c.slot = i;
            c.color_mask = i < colors && s->tgt.color[i].gva ? r->clear_args.color_mask : 0;
            c.aspects = i == 0 ? r->clear_args.aspects : 0;
            if (c.color_mask || c.aspects) g_renderer->clear(s, &c, &g_gl_draw.owners[r->ctx - 1]);
        }
    } else if (g_renderer_up && !r->clear && g_renderer->draw) {
        g_renderer->draw(s, &r->call, &g_gl_draw.owners[r->ctx - 1]);
    }
done:
    atomic_store_explicit(&g_gl_draw.done, r->ring_end, memory_order_release);
    atomic_store_explicit(&g_gl_draw.completed, r->serial, memory_order_release);
    free(r);
}
u64 gl_draw_recorded(void) { return g_gl_draw.issued; }
u64 gl_draw_completed(void) { return atomic_load_explicit(&g_gl_draw.completed, memory_order_acquire); }
void gl_draw_wait(u32 ctx, u64 serial) {
    (void)ctx; /* every context's draws go to the one GPU thread, in order */
    if (gl_draw_completed() >= serial) return;
    hook_wait_begin(); /* off the guest core: its other threads run meanwhile */
    while (gl_draw_completed() < serial) plat_yield();
    hook_wait_end();
}

/* ---- the calls ---- */

/* the program a draw uses, handed to the GPU thread when its link is new to it: its record, NULL (said) for none */
static const SnxGlProgram* draw_program(u32 ctx, u32* name, ShaderInfo si[2]) {
    const SnxGlLayout layout = gl_host_program_layout();
    const SnxGlProgram* p;
    gl_program_layout_refresh(); /* a pipeline's hidden program is linked here */
    *name = snx_gl_draw_program(ctx, 0);
    p = snx_gl_program(*name);
    if (!p || !p->linked) {
        LOG_LIMIT(4, "gl layer: a draw with program %u, %s: skipped\n", (unsigned)*name,
                  p ? "which did not link" : "which is none");
        return NULL;
    }
    gl_host_shader_info(p, &layout, 0, &si[0]);
    if (gl_host_shader_info(p, &layout, 1, &si[1])) memset(&si[1], 0, sizeof si[1]);
    if (g_gl_draw.sent[*name - 1] != p->generation) {
        ProgramRecord* m = calloc(1, sizeof *m);
        if (!m) fatal("gl layer: out of memory for program %u's link", (unsigned)*name);
        m->program = *name, m->generation = p->generation;
        for (int s = 0; s < 2; s++) {
            m->si[s] = si[s];
            m->words[s] = p->spirv_words[s];
            if (!m->words[s]) continue;
            m->spirv[s] = malloc((size_t)m->words[s] * 4);
            if (!m->spirv[s]) fatal("gl layer: out of memory for program %u's link", (unsigned)*name);
            memcpy(m->spirv[s], snx_gl_program_spirv(p, s), (size_t)m->words[s] * 4);
        }
        gpu_call(program_on_gpu, m, NULL, 0);
        g_gl_draw.sent[*name - 1] = p->generation;
    }
    return p;
}

int gl_draw(u32 ctx, const DrawCall* call, u32 elements_type, u64 indices) {
    ShaderInfo si[2];
    u64 serial;
    u32 program;
    const SnxGlProgram* p;
    DrawRecord* r;
    if (memory_ready()) return 0;
    p = draw_program(ctx, &program, si);
    if (!p) return 0;
    r = calloc(1, sizeof *r);
    if (!r) fatal("gl layer: out of memory for a draw");
    r->ctx = ctx, r->program = program, r->generation = p->generation, r->call = *call;
    serial = g_gl_draw.issued + 1; /* the record's below: the steps between hand nothing over */
    if (!targets(ctx, &r->tgt) || !vertex_state(ctx, p, &si[0], r, serial)) {
        free(r);
        return 0;
    }
    if (call->indexed) {
        const SnxGlBuffer* e = snx_gl_buffer(snx_gl_vertex_array(ctx)->elements);
        if (!e) {
            LOG_LIMIT(4, "gl layer: an indexed draw without an element array buffer: skipped\n");
            free(r);
            return 0;
        }
        snx_gl_buffer_read_by(snx_gl_vertex_array(ctx)->elements, serial);
        r->index.addr = gl_gpu_address(e->data) + indices;
        r->index.end = gl_gpu_address(e->data) + (e->size ? e->size - 1 : 0);
        r->index.format = elements_type == GL_UNSIGNED_BYTE_ ? 0 : elements_type == GL_UNSIGNED_SHORT_ ? 1 : 2;
        r->index.restart = snx_gl_fixed(ctx)->primitive_restart;
        r->index.restart_index = r->index.format == 0 ? 0xff : r->index.format == 1 ? 0xffff : 0xffffffffu;
    }
    fixed_state(ctx, r);
    r->dyn.depth_word = call->base_instance;
    constants(ctx, p, si, r, serial);
    r->call.state_flags |= DS_DRAW_POINT_SIZE;
    r->call.point_size = snx_gl_fixed(ctx)->point_size;
    r->ring_end = g_gl_draw.head;
    r->serial = ++g_gl_draw.issued;
    gpu_call(draw_on_gpu, r, NULL, 0);
    return 1;
}

u64 gl_draw_issue(void) { return ++g_gl_draw.issued; }
void gl_draw_done(u64 serial) { atomic_store_explicit(&g_gl_draw.completed, serial, memory_order_release); }

int gl_draw_clear(u32 ctx, u32 buffers) {
    const SnxGlFixed* g = snx_gl_fixed(ctx);
    DrawRecord* r;
    if (memory_ready()) return 0;
    r = calloc(1, sizeof *r);
    if (!r) fatal("gl layer: out of memory for a clear");
    r->ctx = ctx, r->clear = 1;
    if (!targets(ctx, &r->tgt)) {
        free(r);
        return 0;
    }
    fixed_state(ctx, r);
    r->clear_args.color_mask = buffers & 0x4000 ? r->blend.mask[0] : 0; /* GL_COLOR_BUFFER_BIT */
    for (int i = 0; i < 4; i++) memcpy(&r->clear_args.color[i], &g->clear_color[i], 4);
    r->clear_args.aspects = (buffers & 0x100 ? 1u : 0u) | (buffers & 0x400 ? 2u : 0u); /* DEPTH, STENCIL */
    r->clear_args.depth = g->clear_depth;
    r->clear_args.stencil = (u32)g->clear_stencil;
    r->clear_args.stencil_preserve = ~g->stencil[0].write & 0xff;
    r->clear_args.scissored = g->scissor_test;
    r->ring_end = g_gl_draw.head;
    r->serial = ++g_gl_draw.issued;
    gpu_call(draw_on_gpu, r, NULL, 0);
    return 1;
}
