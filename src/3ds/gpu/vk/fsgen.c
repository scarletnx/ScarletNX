/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/shader/generator/glsl_fs_shader_gen.cpp (GenerateFragmentShader),
   pica_fs_config.cpp (FSConfig), shader_uniforms.h (FSUniformData), video_core/rasterizer_accelerated.cpp
   (SyncDrawUniforms) and renderer_vulkan/vk_rasterizer.cpp (SyncAndUploadLUTs, SyncAndUploadLUTsLF), with the
   register layout of video_core/pica/regs_*.h and the LUT entries of pica_core.h (GPL-2.0-or-later, Copyright
   2014-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar 49a1e8c. The profile is Azahar's
   Vulkan one on a desktop GPU: logic ops and min/max blending in the pipeline, no framebuffer fetch, no fragment
   shader interlock or barycentrics, a [0, 1] depth range, and no custom border colours (emulated in the shader).
   The GLSL Azahar generates is written here directly as SPIR-V (common/utilities/spirv_emit.h). */
#include "3ds/gpu/vk/fsgen.h"
#include "3ds/gpu/pica_internal.h"
#include "common/utilities/spirv_emit.h"
#include <stddef.h>
#include <string.h>

/* register indices (RegsInternal) */
enum {
    DEPTH_RANGE = 0x04d,
    DEPTH_NEAR = 0x04e,
    SCISSOR_MODE = 0x065,
    SCISSOR_1 = 0x066,
    SCISSOR_2 = 0x067,
    DEPTHMAP_ENABLE = 0x06d,
    TEX_MAIN = 0x080,
    TEX_SHADOW = 0x08b,
    PROCTEX = 0x0a8,
    PROCTEX_NOISE_U = 0x0a9,
    PROCTEX_NOISE_V = 0x0aa,
    PROCTEX_NOISE_FREQUENCY = 0x0ab,
    PROCTEX_LUT = 0x0ac,
    PROCTEX_LUT_OFFSET = 0x0ad,
    FOG_MODE = 0x0e0,
    FOG_COLOR = 0x0e1,
    TEV_BUFFER_COLOR = 0x0fd,
    OM_MODE = 0x100,
    OM_BLEND_CONST = 0x103,
    OM_ALPHA_TEST = 0x104,
    FB_SHADOW = 0x130,
    LIGHT = 0x140, /* eight lights of 16 words */
    GLOBAL_AMBIENT = 0x1c0,
    MAX_LIGHT = 0x1c2,
    CONFIG0 = 0x1c3,
    CONFIG1 = 0x1c4,
    LIGHTING_DISABLE = 0x1c6,
    ABS_INPUT = 0x1d0,
    LUT_INPUT = 0x1d1,
    LUT_SCALE = 0x1d2,
    LIGHT_ENABLE = 0x1d9
};
/* a texture unit's border colour, then size, parameters, LOD */
static const uint32_t tex_config[3] = {0x081, 0x091, 0x099};
static const uint32_t tev_stage[6] = {0x0c0, 0x0c8, 0x0d0, 0x0d8, 0x0f0, 0x0f8};

/* the config's lighting LUTs, in the order of their 4-bit fields in LUT_INPUT and LUT_SCALE: d0 d1 sp fr rb rg rr */
enum { L_D0, L_D1, L_SP, L_FR, L_RB, L_RG, L_RR };
/* LightingSampler */
enum { S_D0 = 0, S_D1 = 1, S_FR = 3, S_RB = 4, S_RG = 5, S_RR = 6, S_SP = 8, S_DA = 16 };

/* LUT layout in the texel buffers (in their elements) */
enum { LF_FOG = 24 * 256, RG_NOISE = 0, RG_COLOR_MAP = 128, RG_ALPHA_MAP = 256, RGBA_COLOR = 0, RGBA_DIFF = 256 };

_Static_assert(sizeof(SnxCtrFsLight) == 112, "LightSrc is 112 bytes in std140");
_Static_assert(offsetof(SnxCtrFsLight, dist_atten_bias) == 92, "LightSrc layout");
_Static_assert(offsetof(SnxCtrFsUniforms, lighting_lut_offset) == 80, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, fog_color) == 176, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, proctex_noise_f) == 192, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, proctex_noise_p) == 208, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, lighting_global_ambient) == 224, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, light_src) == 240, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, const_color) == 1136, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, tev_combiner_buffer_color) == 1232, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, tex_lod_bias) == 1248, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, tex_border_color) == 1264, "fs_data layout");
_Static_assert(offsetof(SnxCtrFsUniforms, blend_color) == 1312, "fs_data layout");
_Static_assert(sizeof(SnxCtrFsUniforms) == 0x530, "FSUniformData is 0x530 bytes");

static int32_t sbits(uint32_t v, unsigned at, unsigned count) {
    uint32_t u = snx_bits(v, at, count), sign = 1u << (count - 1);
    return (int32_t)(u ^ sign) - (int32_t)sign;
}

/* ---- FSConfig (pica_fs_config.cpp) ---- */

void snx_ctr_fs_config(const SnxCtrPica* p, SnxCtrFsConfig* c) {
    const uint32_t* r = p->regs;
    memset(c, 0, sizeof(*c));
    /* FramebufferConfig */
    c->scissor_mode = (uint8_t)snx_bits(r[SCISSOR_MODE], 0, 2);
    c->depthmap_enable = (uint8_t)snx_bits(r[DEPTHMAP_ENABLE], 0, 1);
    c->shadow_rendering = snx_bits(r[OM_MODE], 0, 2) == 3;
    c->alpha_test_func = snx_bits(r[OM_ALPHA_TEST], 0, 1) ? (uint8_t)snx_bits(r[OM_ALPHA_TEST], 4, 3) : 1;
    /* TextureConfig */
    c->texture0_type = (uint8_t)snx_bits(r[tex_config[0] + 2], 28, 3);
    c->texture2_use_coord1 = (uint8_t)snx_bits(r[TEX_MAIN], 13, 1);
    c->combiner_buffer_input = (uint8_t)snx_bits(r[FOG_MODE], 8, 8);
    c->fog_mode = (uint8_t)snx_bits(r[FOG_MODE], 0, 3);
    c->fog_flip = (uint8_t)snx_bits(r[FOG_MODE], 16, 1);
    c->shadow_texture_orthographic = (uint8_t)snx_bits(r[TEX_SHADOW], 0, 1);
    for (unsigned i = 0; i < 3; ++i) { /* ApplyProfile without custom border colours */
        uint32_t params = r[tex_config[i] + 2];
        c->border_s[i] = snx_bits(params, 12, 3) == 1;
        c->border_t[i] = snx_bits(params, 8, 3) == 1;
    }
    for (unsigned i = 0; i < 6; ++i) {
        const uint32_t* s = &r[tev_stage[i]];
        c->tev_sources[i] = s[0];
        c->tev_modifiers[i] = s[1];
        c->tev_ops[i] = s[2];
        c->tev_scales[i] = s[4];
        if (snx_bits(s[2], 0, 4) == 7) { /* Dot3_RGBA: the alpha part is the colour result */
            c->tev_sources[i] &= 0xfff;
            c->tev_modifiers[i] &= 0xfff;
            c->tev_ops[i] &= 0xf;
        }
    }
    /* LightConfig */
    if (!snx_bits(r[LIGHTING_DISABLE], 0, 1)) {
        uint32_t c0 = r[CONFIG0], c1 = r[CONFIG1];
        unsigned max = snx_bits(r[MAX_LIGHT], 0, 3);
        c->lighting.enable = 1;
        c->lighting.src_num = (uint8_t)(max + 1);
        c->lighting.config = (uint8_t)snx_bits(c0, 4, 4);
        c->lighting.enable_primary_alpha = (uint8_t)snx_bits(c0, 2, 1);
        c->lighting.enable_secondary_alpha = (uint8_t)snx_bits(c0, 3, 1);
        c->lighting.bump_mode = (uint8_t)snx_bits(c0, 28, 2);
        c->lighting.bump_selector = (uint8_t)snx_bits(c0, 22, 2);
        c->lighting.bump_renorm = !snx_bits(c0, 30, 1);
        c->lighting.clamp_highlights = (uint8_t)snx_bits(c0, 27, 1);
        c->lighting.enable_shadow = (uint8_t)snx_bits(c0, 0, 1);
        if (c->lighting.enable_shadow) {
            c->lighting.shadow_primary = (uint8_t)snx_bits(c0, 16, 1);
            c->lighting.shadow_secondary = (uint8_t)snx_bits(c0, 17, 1);
            c->lighting.shadow_invert = (uint8_t)snx_bits(c0, 18, 1);
            c->lighting.shadow_alpha = (uint8_t)snx_bits(c0, 19, 1);
            c->lighting.shadow_selector = (uint8_t)snx_bits(c0, 24, 2);
        }
        for (unsigned i = 0; i <= max; ++i) {
            unsigned num = snx_bits(r[LIGHT_ENABLE], i * 4, 3);
            uint32_t config = r[LIGHT + num * 16 + 9];
            c->lighting.lights[i].num = (uint8_t)num;
            c->lighting.lights[i].directional = (uint8_t)snx_bits(config, 0, 1);
            c->lighting.lights[i].two_sided_diffuse = (uint8_t)snx_bits(config, 1, 1);
            c->lighting.lights[i].geometric_factor_0 = (uint8_t)snx_bits(config, 2, 1);
            c->lighting.lights[i].geometric_factor_1 = (uint8_t)snx_bits(config, 3, 1);
            c->lighting.lights[i].dist_atten = !snx_bits(c1, 24 + num, 1);
            c->lighting.lights[i].spot_atten = !snx_bits(c1, 8 + num, 1);
            c->lighting.lights[i].shadow = !snx_bits(c1, num, 1);
        }
        /* the disable bits in config1 per LUT; sp has none (Azahar's dummy enable) */
        static const int disable_bit[7] = {16, 17, -1, 19, 22, 21, 20};
        for (unsigned l = 0; l < 7; ++l) {
            if (disable_bit[l] >= 0 && snx_bits(c1, (unsigned)disable_bit[l], 1)) continue;
            c->lighting.lut[l].enable = 1;
            c->lighting.lut[l].abs_input = !snx_bits(r[ABS_INPUT], 1 + l * 4, 1);
            c->lighting.lut[l].type = (uint8_t)snx_bits(r[LUT_INPUT], l * 4, 3);
            c->lighting.lut[l].scale = (uint8_t)snx_bits(r[LUT_SCALE], l * 4, 3);
        }
    }
    /* ProcTexConfig */
    if (snx_bits(r[TEX_MAIN], 10, 1)) {
        uint32_t pt = r[PROCTEX], lut = r[PROCTEX_LUT], offset = r[PROCTEX_LUT_OFFSET];
        c->proctex.enable = 1;
        c->proctex.coord = (uint8_t)snx_bits(r[TEX_MAIN], 8, 2);
        c->proctex.u_clamp = (uint8_t)snx_bits(pt, 0, 3);
        c->proctex.v_clamp = (uint8_t)snx_bits(pt, 3, 3);
        c->proctex.color_combiner = (uint8_t)snx_bits(pt, 6, 4);
        c->proctex.alpha_combiner = (uint8_t)snx_bits(pt, 10, 4);
        c->proctex.separate_alpha = (uint8_t)snx_bits(pt, 14, 1);
        c->proctex.noise_enable = (uint8_t)snx_bits(pt, 15, 1);
        c->proctex.u_shift = (uint8_t)snx_bits(pt, 16, 2);
        c->proctex.v_shift = (uint8_t)snx_bits(pt, 18, 2);
        c->proctex.lut_filter = (uint8_t)snx_bits(lut, 0, 3);
        c->proctex.lod_min = (uint8_t)snx_bits(lut, 3, 4);
        c->proctex.lod_max = (uint8_t)snx_bits(lut, 7, 4);
        c->proctex.lut_width = (uint8_t)snx_bits(lut, 11, 8);
        for (unsigned i = 0; i < 4; ++i) c->proctex.lut_offset[i] = (uint8_t)snx_bits(offset, i * 8, 8);
    }
}

/* ---- the generator (glsl_fs_shader_gen.cpp), written as SPIR-V ---- */

/* Azahar's GLSL operation by operation, as glslang translates each: its mutable variables are SSA values here
   (Function variables only around control flow), its helper functions inlined where they have no early return.
   sampleTexUnit0..3 and SampleShadow2D stay functions */

enum { OP_IMAGE_TEXEL_POINTER = 60 };
enum { IMAGE_OPERANDS_LOD = 2, FORMAT_R32UI = 33, SCOPE_DEVICE = 1 };
/* the fs_data members, in the block's order, and a LightSrc's */
enum {
    U_FRAMEBUFFER_SCALE,
    U_ALPHATEST_REF,
    U_DEPTH_SCALE,
    U_DEPTH_OFFSET,
    U_SHADOW_BIAS_CONSTANT,
    U_SHADOW_BIAS_LINEAR,
    U_SCISSOR_X1,
    U_SCISSOR_Y1,
    U_SCISSOR_X2,
    U_SCISSOR_Y2,
    U_FOG_LUT_OFFSET,
    U_PROCTEX_NOISE_LUT_OFFSET,
    U_PROCTEX_COLOR_MAP_OFFSET,
    U_PROCTEX_ALPHA_MAP_OFFSET,
    U_PROCTEX_LUT_OFFSET,
    U_PROCTEX_DIFF_LUT_OFFSET,
    U_PROCTEX_BIAS,
    U_SHADOW_TEXTURE_BIAS,
    U_LIGHTING_LUT_OFFSET,
    U_FOG_COLOR,
    U_PROCTEX_NOISE_F,
    U_PROCTEX_NOISE_A,
    U_PROCTEX_NOISE_P,
    U_LIGHTING_GLOBAL_AMBIENT,
    U_LIGHT_SRC,
    U_CONST_COLOR,
    U_TEV_COMBINER_BUFFER_COLOR,
    U_TEX_LOD_BIAS,
    U_TEX_BORDER_COLOR,
    U_BLEND_COLOR,
    U_COUNT
};
enum {
    LS_SPECULAR_0,
    LS_SPECULAR_1,
    LS_DIFFUSE,
    LS_AMBIENT,
    LS_POSITION,
    LS_SPOT_DIRECTION,
    LS_DIST_ATTEN_BIAS,
    LS_DIST_ATTEN_SCALE,
    LS_COUNT
};
/* the inputs, by location */
enum { IN_PRIMARY_COLOR, IN_TEXCOORD0, IN_TEXCOORD1, IN_TEXCOORD2, IN_TEXCOORD0_W, IN_NORMQUAT, IN_VIEW, IN_COUNT };
enum { LUT_LF, LUT_RG, LUT_RGBA };

typedef struct {
    const SnxCtrFsConfig* c;
    SpvEmitter s;
    uint32_t t_bool, t_f, t_v2, t_v3, t_v4, t_i, t_iv2, t_iv4, t_u, t_uv4, t_bv2, t_bv4;
    uint32_t in[IN_COUNT], frag_coord, color, frag_depth;
    uint32_t fs_data, lut[3], lut_type, lut_image;
    uint32_t tex[3], tex_type[3], tex_image[3]; /* tex0's types are a face's for the shadow cube */
    uint32_t shadow_buffer, shadow_image;
    uint32_t unit[4], shadow_2d; /* the functions sampleTexUnit0..3 and SampleShadow2D */
    /* main's variables */
    uint32_t rounded_primary_color, primary_fragment_color, secondary_fragment_color;
    uint32_t combiner_buffer, next_combiner_buffer, combiner_output;
} Gen;

static unsigned tev_field(uint32_t raw, unsigned at, unsigned count) { return snx_bits(raw, at, count); }
static unsigned multiplier(unsigned scale) { return scale < 3 ? 1u << scale : 1u; }

/* LightingRegs::LightingScale */
static float lut_scale(unsigned s) {
    static const float scales[8] = {1.0f, 2.0f, 4.0f, 8.0f, 0.0f, 0.0f, 0.25f, 0.5f};
    return scales[s & 7];
}

static bool sampler_supported(unsigned config, unsigned sampler) {
    switch (sampler) {
        case S_D0: return config != 1;
        case S_D1: return config != 0 && config != 1 && config != 5;
        case S_SP: return config != 2 && config != 3;
        case S_FR: return config != 0 && config != 2 && config != 4;
        case S_RR: return config != 3;
        default: return config == 4 || config == 5 || config == 8; /* RG, RB */
    }
}

static bool pass_through(const SnxCtrFsConfig* c, unsigned i) {
    uint32_t s = c->tev_sources[i], m = c->tev_modifiers[i], o = c->tev_ops[i], k = c->tev_scales[i];
    return tev_field(o, 0, 4) == 0 && tev_field(o, 16, 4) == 0 && tev_field(s, 0, 4) == 15 &&
           tev_field(s, 16, 4) == 15 && tev_field(m, 0, 4) == 0 && tev_field(m, 12, 3) == 0 &&
           multiplier(tev_field(k, 0, 2)) == 1 && multiplier(tev_field(k, 16, 2)) == 1;
}

static bool lut_used(const SnxCtrFsConfig* c, unsigned which, unsigned sampler) {
    return c->lighting.lut[which].enable && sampler_supported(c->lighting.config, sampler);
}
/* gl_FragDepth is written (and declared) unless main discards everything, writes gas or renders shadows */
static bool writes_depth(const SnxCtrFsConfig* c) {
    return c->alpha_test_func != 0 && c->fog_mode != 7 && !c->shadow_rendering;
}

/* ---- values ---- */

static unsigned width(const Gen* g, uint32_t type) {
    if (type == g->t_v2 || type == g->t_iv2 || type == g->t_bv2) return 2;
    if (type == g->t_v3) return 3;
    if (type == g->t_v4 || type == g->t_iv4 || type == g->t_uv4 || type == g->t_bv4) return 4;
    return 1;
}
/* a float constant of type, a vector one with it in every component */
static uint32_t fc(Gen* g, uint32_t type, float value) {
    uint32_t k = spv_emit_const_f32(&g->s, value), parts[4] = {k, k, k, k};
    unsigned n = width(g, type);
    return n == 1 ? k : spv_emit_const_composite(&g->s, type, parts, n);
}
static uint32_t vec_constant(Gen* g, uint32_t type, float x, float y, float z, float w) {
    uint32_t parts[4] = {spv_emit_const_f32(&g->s, x), spv_emit_const_f32(&g->s, y),
                         spv_emit_const_f32(&g->s, z), spv_emit_const_f32(&g->s, w)};
    return spv_emit_const_composite(&g->s, type, parts, width(g, type));
}
static uint32_t ic(Gen* g, int32_t value) { return spv_emit_const_i32(&g->s, value); }
static uint32_t uc(Gen* g, uint32_t value) { return spv_emit_const_u32(&g->s, value); }
static uint32_t ivec2_constant(Gen* g, int32_t x, int32_t y) {
    uint32_t parts[2] = {ic(g, x), ic(g, y)};
    return spv_emit_const_composite(&g->s, g->t_iv2, parts, 2);
}
/* a scalar in every component of type (GLSL's vector constructor from one scalar) */
static uint32_t splat(Gen* g, uint32_t type, uint32_t scalar) {
    uint32_t parts[4] = {scalar, scalar, scalar, scalar};
    unsigned n = width(g, type);
    return n == 1 ? scalar : spv_emit_op(&g->s, SPV_OP_COMPOSITE_CONSTRUCT, type, parts, n);
}

static uint32_t op1(Gen* g, uint32_t op, uint32_t type, uint32_t a) { return SPV(&g->s, op, type, a); }
static uint32_t op2(Gen* g, uint32_t op, uint32_t type, uint32_t a, uint32_t b) { return SPV(&g->s, op, type, a, b); }
static uint32_t ext1(Gen* g, uint32_t type, uint32_t instruction, uint32_t a) {
    return SPV_EXT(&g->s, type, instruction, a);
}
static uint32_t ext2(Gen* g, uint32_t type, uint32_t instruction, uint32_t a, uint32_t b) {
    return SPV_EXT(&g->s, type, instruction, a, b);
}
static uint32_t ext3(Gen* g, uint32_t type, uint32_t instruction, uint32_t a, uint32_t b, uint32_t c) {
    return SPV_EXT(&g->s, type, instruction, a, b, c);
}
static uint32_t fadd(Gen* g, uint32_t type, uint32_t a, uint32_t b) { return op2(g, SPV_OP_F_ADD, type, a, b); }
static uint32_t fsub(Gen* g, uint32_t type, uint32_t a, uint32_t b) { return op2(g, SPV_OP_F_SUB, type, a, b); }
static uint32_t fmul(Gen* g, uint32_t type, uint32_t a, uint32_t b) { return op2(g, SPV_OP_F_MUL, type, a, b); }
/* v * k (or k * v) with a scalar k, as glslang writes it */
static uint32_t scale(Gen* g, uint32_t type, uint32_t v, uint32_t k) {
    return op2(g, width(g, type) > 1 ? SPV_OP_VECTOR_TIMES_SCALAR : SPV_OP_F_MUL, type, v, k);
}
static uint32_t clamp01(Gen* g, uint32_t type, uint32_t v) {
    return ext3(g, type, SPV_GLSL_F_CLAMP, v, fc(g, type, 0.0f), fc(g, type, 1.0f));
}
static uint32_t choose(Gen* g, uint32_t type, uint32_t cond, uint32_t a, uint32_t b) {
    if (width(g, type) == 2) cond = SPV(&g->s, SPV_OP_COMPOSITE_CONSTRUCT, g->t_bv2, cond, cond);
    return SPV(&g->s, SPV_OP_SELECT, type, cond, a, b);
}
/* a float component */
static uint32_t at(Gen* g, uint32_t v, unsigned i) { return SPV(&g->s, SPV_OP_COMPOSITE_EXTRACT, g->t_f, v, i); }
static uint32_t rgb(Gen* g, uint32_t v) { return SPV(&g->s, SPV_OP_VECTOR_SHUFFLE, g->t_v3, v, v, 0, 1, 2); }
/* v4 with its rgb replaced */
static uint32_t with_rgb(Gen* g, uint32_t v4, uint32_t rgb3) {
    return SPV(&g->s, SPV_OP_VECTOR_SHUFFLE, g->t_v4, rgb3, v4, 0, 1, 2, 6);
}
static uint32_t with_component(Gen* g, uint32_t type, uint32_t v, unsigned i, uint32_t x) {
    return SPV(&g->s, SPV_OP_COMPOSITE_INSERT, type, x, v, i);
}

static uint32_t load(Gen* g, uint32_t type, uint32_t pointer) { return SPV(&g->s, SPV_OP_LOAD, type, pointer); }
static void store(Gen* g, uint32_t pointer, uint32_t value) { SPV_VOID(&g->s, SPV_OP_STORE, pointer, value); }
static uint32_t input_type(const Gen* g, unsigned which) {
    const uint32_t types[IN_COUNT] = {g->t_v4, g->t_v2, g->t_v2, g->t_v2, g->t_f, g->t_v4, g->t_v3};
    return types[which];
}
static uint32_t input(Gen* g, unsigned which) { return load(g, input_type(g, which), g->in[which]); }
/* a member of fs_data, through constant indices */
static uint32_t uniform(Gen* g, uint32_t type, const uint32_t* chain, size_t count) {
    uint32_t operands[4] = {g->fs_data};
    for (size_t i = 0; i < count; ++i) operands[i + 1] = ic(g, (int32_t)chain[i]);
    uint32_t pointer = spv_emit_op(&g->s, SPV_OP_ACCESS_CHAIN,
                                      spv_emit_type_pointer(&g->s, SPV_STORAGE_UNIFORM, type), operands, count + 1);
    return load(g, type, pointer);
}
static uint32_t member(Gen* g, uint32_t type, unsigned m) {
    uint32_t chain[1] = {m};
    return uniform(g, type, chain, 1);
}
static uint32_t member_at(Gen* g, uint32_t type, unsigned m, unsigned index) {
    uint32_t chain[2] = {m, index};
    return uniform(g, type, chain, 2);
}
static uint32_t light(Gen* g, uint32_t type, unsigned num, unsigned m) {
    uint32_t chain[3] = {U_LIGHT_SRC, num, m};
    return uniform(g, type, chain, 3);
}
/* a constant int table indexed at run time: a Function array, as glslang makes a const array */
static uint32_t table_at(Gen* g, const int32_t* values, unsigned count, uint32_t index) {
    uint32_t parts[16], type = spv_emit_type_array(&g->s, g->t_i, count), var = spv_emit_local(&g->s, type);
    for (unsigned i = 0; i < count; ++i) parts[i] = ic(g, values[i]);
    store(g, var, spv_emit_const_composite(&g->s, type, parts, count));
    return load(g, g->t_i,
                SPV(&g->s, SPV_OP_ACCESS_CHAIN, spv_emit_type_pointer(&g->s, SPV_STORAGE_FUNCTION, g->t_i), var, index));
}

/* ---- control flow ---- */

/* if (cond) { the block placed here }: if_end places the merge, branching to it unless the block ended itself */
static uint32_t if_begin(Gen* g, uint32_t cond) {
    uint32_t then = spv_emit_id(&g->s), merge = spv_emit_id(&g->s);
    SPV_VOID(&g->s, SPV_OP_SELECTION_MERGE, merge, SPV_SELECTION_CONTROL_NONE);
    SPV_VOID(&g->s, SPV_OP_BRANCH_CONDITIONAL, cond, then, merge);
    spv_emit_label(&g->s, then);
    return merge;
}
static void if_end(Gen* g, uint32_t merge, bool falls_through) {
    if (falls_through) SPV_VOID(&g->s, SPV_OP_BRANCH, merge);
    spv_emit_label(&g->s, merge);
}
/* if (cond) discard; */
static void kill_if(Gen* g, uint32_t cond) {
    uint32_t merge = if_begin(g, cond);
    spv_emit_void_op(&g->s, SPV_OP_KILL, NULL, 0);
    if_end(g, merge, false);
}

/* ---- helpers (Azahar's GLSL functions) ---- */

static uint32_t byteround(Gen* g, uint32_t type, uint32_t x) {
    x = ext1(g, type, SPV_GLSL_ROUND, scale(g, type, x, fc(g, g->t_f, 255.0f)));
    return scale(g, type, x, fc(g, g->t_f, (float)(1.0 / 255.0)));
}
static uint32_t quaternion_rotate(Gen* g, uint32_t q, uint32_t v) {
    uint32_t xyz = rgb(g, q), w = at(g, q, 3);
    uint32_t inner = fadd(g, g->t_v3, ext2(g, g->t_v3, SPV_GLSL_CROSS, xyz, v), scale(g, g->t_v3, v, w));
    return fadd(g, g->t_v3, v,
                scale(g, g->t_v3, ext2(g, g->t_v3, SPV_GLSL_CROSS, xyz, inner), fc(g, g->t_f, 2.0f)));
}
static uint32_t get_lod(Gen* g, uint32_t coord) {
    uint32_t dx = ext1(g, g->t_v2, SPV_GLSL_F_ABS, op1(g, SPV_OP_DPDX, g->t_v2, coord));
    uint32_t dy = ext1(g, g->t_v2, SPV_GLSL_F_ABS, op1(g, SPV_OP_DPDY, g->t_v2, coord));
    uint32_t d = ext2(g, g->t_v2, SPV_GLSL_F_MAX, dx, dy);
    return ext1(g, g->t_f, SPV_GLSL_LOG2, ext2(g, g->t_f, SPV_GLSL_F_MAX, at(g, d, 0), at(g, d, 1)));
}
/* texelFetch of a LUT texel buffer */
static uint32_t lut_fetch(Gen* g, unsigned which, uint32_t index) {
    uint32_t image = op1(g, SPV_OP_IMAGE, g->lut_image, load(g, g->lut_type, g->lut[which]));
    return op2(g, SPV_OP_IMAGE_FETCH, g->t_v4, image, index);
}
/* textureSize(tex, 0) of a loaded sampler */
static uint32_t texture_size(Gen* g, unsigned unit, uint32_t sampled) {
    uint32_t image = op1(g, SPV_OP_IMAGE, g->tex_image[unit], sampled);
    return op2(g, SPV_OP_IMAGE_QUERY_SIZE_LOD, g->t_iv2, image, ic(g, 0));
}

/* LookupLightingLUT, Unsigned and Signed, for a constant LUT */
static uint32_t lighting_lut(Gen* g, unsigned lut, uint32_t index, uint32_t delta) {
    uint32_t chain[3] = {U_LIGHTING_LUT_OFFSET, lut >> 2, lut & 3};
    uint32_t entry = lut_fetch(g, LUT_LF, op2(g, SPV_OP_I_ADD, g->t_i, uniform(g, g->t_i, chain, 3), index));
    return fadd(g, g->t_f, at(g, entry, 0), fmul(g, g->t_f, at(g, entry, 1), delta));
}
static uint32_t lighting_lut_unsigned(Gen* g, unsigned lut, uint32_t pos) {
    uint32_t p = fmul(g, g->t_f, pos, fc(g, g->t_f, 256.0f));
    uint32_t index = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i,
                         ext3(g, g->t_f, SPV_GLSL_F_CLAMP, ext1(g, g->t_f, SPV_GLSL_FLOOR, p), fc(g, g->t_f, 0.0f),
                              fc(g, g->t_f, 255.0f)));
    uint32_t delta = fsub(g, g->t_f, p, op1(g, SPV_OP_CONVERT_S_TO_F, g->t_f, index));
    return lighting_lut(g, lut, index, delta);
}
static uint32_t lighting_lut_signed(Gen* g, unsigned lut, uint32_t pos) {
    uint32_t p = fmul(g, g->t_f, pos, fc(g, g->t_f, 128.0f));
    uint32_t index = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i,
                         ext3(g, g->t_f, SPV_GLSL_F_CLAMP, ext1(g, g->t_f, SPV_GLSL_FLOOR, p), fc(g, g->t_f, -128.0f),
                              fc(g, g->t_f, 127.0f)));
    uint32_t delta = fsub(g, g->t_f, p, op1(g, SPV_OP_CONVERT_S_TO_F, g->t_f, index));
    index = choose(g, g->t_i, op2(g, SPV_OP_S_LESS_THAN, g->t_bool, index, ic(g, 0)),
                   op2(g, SPV_OP_I_ADD, g->t_i, index, ic(g, 256)), index);
    return lighting_lut(g, lut, index, delta);
}

/* CompareShadow and mix2 */
static uint32_t compare_shadow(Gen* g, uint32_t pixel, uint32_t z) {
    uint32_t depth = op2(g, SPV_OP_SHIFT_RIGHT_LOGICAL, g->t_u, pixel, uc(g, 8));
    uint32_t stencil = op2(g, SPV_OP_BITWISE_AND, g->t_u, pixel, uc(g, 0xff));
    uint32_t value = fmul(g, g->t_f, op1(g, SPV_OP_CONVERT_U_TO_F, g->t_f, stencil),
                          fc(g, g->t_f, (float)(1.0 / 255.0)));
    return choose(g, g->t_f, op2(g, SPV_OP_U_LESS_THAN_EQUAL, g->t_bool, depth, z), fc(g, g->t_f, 0.0f), value);
}
static uint32_t mix2(Gen* g, uint32_t s, uint32_t a) {
    SpvEmitter* sp = &g->s;
    uint32_t t = ext3(g, g->t_v2, SPV_GLSL_F_MIX, SPV(sp, SPV_OP_VECTOR_SHUFFLE, g->t_v2, s, s, 0, 1),
                      SPV(sp, SPV_OP_VECTOR_SHUFFLE, g->t_v2, s, s, 2, 3),
                      SPV(sp, SPV_OP_VECTOR_SHUFFLE, g->t_v2, a, a, 1, 1));
    return ext3(g, g->t_f, SPV_GLSL_F_MIX, at(g, t, 0), at(g, t, 1), at(g, a, 0));
}
/* uint(max(0, int(x * float(0xFFFFFF)) - shadow_texture_bias)) */
static uint32_t shadow_depth(Gen* g, uint32_t x) {
    uint32_t z = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, fmul(g, g->t_f, x, fc(g, g->t_f, 16777215.0f)));
    z = op2(g, SPV_OP_I_SUB, g->t_i, z, member(g, g->t_i, U_SHADOW_TEXTURE_BIAS));
    return op1(g, SPV_OP_BITCAST, g->t_u, ext2(g, g->t_i, SPV_GLSL_S_MAX, ic(g, 0), z));
}
/* texelFetch(sampler, coord, 0).x of a shadow map */
static uint32_t shadow_texel(Gen* g, uint32_t sampled, uint32_t coord) {
    uint32_t image = op1(g, SPV_OP_IMAGE, g->tex_image[0], sampled);
    uint32_t texel = SPV(&g->s, SPV_OP_IMAGE_FETCH, g->t_uv4, image, coord, IMAGE_OPERANDS_LOD, ic(g, 0));
    return SPV(&g->s, SPV_OP_COMPOSITE_EXTRACT, g->t_u, texel, 0);
}

/* the function SampleShadow2D(ivec2 uv, uint z) */
static void shadow_2d_function(Gen* g) {
    SpvEmitter* s = &g->s;
    uint32_t types[2] = {g->t_iv2, g->t_u}, p[2];
    spv_emit_function(s, g->shadow_2d, g->t_f, types, 2, p);
    uint32_t sampled = load(g, g->tex_type[0], g->tex[0]);
    uint32_t outside = SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, g->t_bv4,
                           op2(g, SPV_OP_S_LESS_THAN, g->t_bv2, p[0], ivec2_constant(g, 0, 0)),
                           op2(g, SPV_OP_S_GREATER_THAN_EQUAL, g->t_bv2, p[0], texture_size(g, 0, sampled)));
    uint32_t merge = if_begin(g, op1(g, SPV_OP_ANY, g->t_bool, outside));
    SPV_VOID(s, SPV_OP_RETURN_VALUE, fc(g, g->t_f, 1.0f));
    if_end(g, merge, false);
    SPV_VOID(s, SPV_OP_RETURN_VALUE, compare_shadow(g, shadow_texel(g, sampled, p[0]), p[1]));
    spv_emit_function_end(s);
}

/* shadowTexture(texcoord0, texcoord0_w) */
static uint32_t shadow_texture(Gen* g) {
    uint32_t uv = input(g, IN_TEXCOORD0), w = input(g, IN_TEXCOORD0_W);
    if (!g->c->shadow_texture_orthographic) uv = op2(g, SPV_OP_F_DIV, g->t_v2, uv, splat(g, g->t_v2, w));
    uint32_t z = shadow_depth(g, ext2(g, g->t_f, SPV_GLSL_F_MIN, ext1(g, g->t_f, SPV_GLSL_F_ABS, w),
                                      fc(g, g->t_f, 1.0f)));
    uint32_t size = texture_size(g, 0, load(g, g->tex_type[0], g->tex[0]));
    uint32_t coord = fsub(g, g->t_v2, fmul(g, g->t_v2, op1(g, SPV_OP_CONVERT_S_TO_F, g->t_v2, size), uv),
                          fc(g, g->t_v2, 0.5f));
    uint32_t coord_floor = ext1(g, g->t_v2, SPV_GLSL_FLOOR, coord);
    uint32_t f = fsub(g, g->t_v2, coord, coord_floor);
    uint32_t i = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_iv2, coord_floor), samples[4];
    static const int32_t offsets[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (unsigned k = 0; k < 4; ++k) {
        uint32_t at_k = k ? op2(g, SPV_OP_I_ADD, g->t_iv2, i, ivec2_constant(g, offsets[k][0], offsets[k][1])) : i;
        samples[k] = SPV(&g->s, SPV_OP_FUNCTION_CALL, g->t_f, g->shadow_2d, at_k, z);
    }
    uint32_t s4 = spv_emit_op(&g->s, SPV_OP_COMPOSITE_CONSTRUCT, g->t_v4, samples, 4);
    return splat(g, g->t_v4, mix2(g, s4, f));
}

/* tex0[face] of the shadow cube */
static uint32_t shadow_face(Gen* g, unsigned face) {
    uint32_t pointer = SPV(&g->s, SPV_OP_ACCESS_CHAIN,
                           spv_emit_type_pointer(&g->s, SPV_STORAGE_UNIFORM_CONSTANT, g->tex_type[0]), g->tex[0],
                           ic(g, (int32_t)face));
    return load(g, g->tex_type[0], pointer);
}
/* shadowTextureCube(texcoord0, texcoord0_w): the face's choice of w and uv, pure, by selects; the fetches from the
   chosen face (SampleShadowCube) under a switch */
static uint32_t shadow_texture_cube(Gen* g) {
    SpvEmitter* s = &g->s;
    uint32_t size = texture_size(g, 0, shadow_face(g, 0));
    uint32_t c = SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, g->t_v3, input(g, IN_TEXCOORD0), input(g, IN_TEXCOORD0_W));
    uint32_t a = ext1(g, g->t_v3, SPV_GLSL_F_ABS, c);
    uint32_t ax = at(g, a, 0), ay = at(g, a, 1), az = at(g, a, 2), cx = at(g, c, 0), cy = at(g, c, 1),
             cz = at(g, c, 2), zero = fc(g, g->t_f, 0.0f);
    uint32_t on_x = op2(g, SPV_OP_LOGICAL_AND, g->t_bool, op2(g, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, ax, ay),
                        op2(g, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, ax, az));
    uint32_t on_y = op2(g, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, ay, az);
    /* uv = -c.zy, x negated when c.x < 0 */
    uint32_t x_uv = op1(g, SPV_OP_F_NEGATE, g->t_v2, SPV(s, SPV_OP_VECTOR_SHUFFLE, g->t_v2, c, c, 2, 1));
    x_uv = choose(g, g->t_v2, op2(g, SPV_OP_F_ORD_LESS_THAN, g->t_bool, cx, zero),
                  with_component(g, g->t_v2, x_uv, 0, op1(g, SPV_OP_F_NEGATE, g->t_f, at(g, x_uv, 0))), x_uv);
    /* uv = c.xz, y negated when c.y < 0 */
    uint32_t y_uv = SPV(s, SPV_OP_VECTOR_SHUFFLE, g->t_v2, c, c, 0, 2);
    y_uv = choose(g, g->t_v2, op2(g, SPV_OP_F_ORD_LESS_THAN, g->t_bool, cy, zero),
                  with_component(g, g->t_v2, y_uv, 1, op1(g, SPV_OP_F_NEGATE, g->t_f, at(g, y_uv, 1))), y_uv);
    /* uv = -c.xy, x negated when c.z > 0 */
    uint32_t z_uv = op1(g, SPV_OP_F_NEGATE, g->t_v2, SPV(s, SPV_OP_VECTOR_SHUFFLE, g->t_v2, c, c, 0, 1));
    z_uv = choose(g, g->t_v2, op2(g, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, cz, zero),
                  with_component(g, g->t_v2, z_uv, 0, op1(g, SPV_OP_F_NEGATE, g->t_f, at(g, z_uv, 0))), z_uv);
    uint32_t w = choose(g, g->t_f, on_x, ax, choose(g, g->t_f, on_y, ay, az));
    uint32_t uv = choose(g, g->t_v2, on_x, x_uv, choose(g, g->t_v2, on_y, y_uv, z_uv));
    uint32_t positive[3] = {op2(g, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, cx, zero),
                            op2(g, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, cy, zero),
                            op2(g, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, cz, zero)};
    uint32_t face = choose(g, g->t_i, on_x, choose(g, g->t_i, positive[0], ic(g, 0), ic(g, 1)),
                           choose(g, g->t_i, on_y, choose(g, g->t_i, positive[1], ic(g, 2), ic(g, 3)),
                                  choose(g, g->t_i, positive[2], ic(g, 4), ic(g, 5))));
    uint32_t z = shadow_depth(g, ext2(g, g->t_f, SPV_GLSL_F_MIN, w, fc(g, g->t_f, 1.0f)));
    uint32_t half = fc(g, g->t_v2, 0.5f);
    uint32_t projected = op2(g, SPV_OP_F_DIV, g->t_v2, uv, splat(g, g->t_v2, w));
    projected = fadd(g, g->t_v2, fmul(g, g->t_v2, projected, half), half);
    uint32_t coord = fsub(g, g->t_v2, fmul(g, g->t_v2, op1(g, SPV_OP_CONVERT_S_TO_F, g->t_v2, size), projected), half);
    uint32_t coord_floor = ext1(g, g->t_v2, SPV_GLSL_FLOOR, coord);
    uint32_t f = fsub(g, g->t_v2, coord, coord_floor);
    uint32_t i00 = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_iv2, coord_floor), corner[4];
    uint32_t cmin = ivec2_constant(g, 0, 0), cmax = op2(g, SPV_OP_I_SUB, g->t_iv2, size, ivec2_constant(g, 1, 1));
    static const int32_t offsets[4][2] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
    for (unsigned k = 0; k < 4; ++k) {
        uint32_t i = k ? op2(g, SPV_OP_I_ADD, g->t_iv2, i00, ivec2_constant(g, offsets[k][0], offsets[k][1])) : i00;
        corner[k] = ext3(g, g->t_iv2, SPV_GLSL_S_CLAMP, i, cmin, cmax);
    }
    /* pixels = SampleShadowCube(face, ...) */
    uint32_t pixels = spv_emit_local(s, g->t_uv4), label[6], merge = spv_emit_id(s);
    for (unsigned k = 0; k < 6; ++k) label[k] = spv_emit_id(s);
    SPV_VOID(s, SPV_OP_SELECTION_MERGE, merge, SPV_SELECTION_CONTROL_NONE);
    SPV_VOID(s, SPV_OP_SWITCH, face, label[5], 0, label[0], 1, label[1], 2, label[2], 3, label[3], 4, label[4]);
    for (unsigned k = 0; k < 6; ++k) {
        uint32_t texels[4], sampled;
        spv_emit_label(s, label[k]);
        sampled = shadow_face(g, k);
        for (unsigned n = 0; n < 4; ++n) texels[n] = shadow_texel(g, sampled, corner[n]);
        store(g, pixels, spv_emit_op(s, SPV_OP_COMPOSITE_CONSTRUCT, g->t_uv4, texels, 4));
        SPV_VOID(s, SPV_OP_BRANCH, merge);
    }
    spv_emit_label(s, merge);
    uint32_t p = load(g, g->t_uv4, pixels), compared[4];
    for (unsigned n = 0; n < 4; ++n) compared[n] = compare_shadow(g, SPV(s, SPV_OP_COMPOSITE_EXTRACT, g->t_u, p, n), z);
    return splat(g, g->t_v4, mix2(g, spv_emit_op(s, SPV_OP_COMPOSITE_CONSTRUCT, g->t_v4, compared, 4), f));
}

/* ---- procedural textures ---- */

/* ProcTexLookupLUT: NoiseLUT, ColorMap and AlphaMap: 0.0 is lut[0], 127/128 lut[127], 1.0 lut[127] + diff[127];
   floor, not fract, so that 128.0 is index 127 and fraction 1.0 */
static uint32_t proctex_lut(Gen* g, uint32_t offset, uint32_t coord) {
    coord = fmul(g, g->t_f, coord, fc(g, g->t_f, 128.0f));
    uint32_t index_i = ext3(g, g->t_f, SPV_GLSL_F_CLAMP, ext1(g, g->t_f, SPV_GLSL_FLOOR, coord), fc(g, g->t_f, 0.0f),
                            fc(g, g->t_f, 127.0f));
    uint32_t index_f = fsub(g, g->t_f, coord, index_i);
    uint32_t entry = lut_fetch(g, LUT_RG, op2(g, SPV_OP_I_ADD, g->t_i, op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, index_i),
                                              offset));
    return clamp01(g, g->t_f, fadd(g, g->t_f, at(g, entry, 0), fmul(g, g->t_f, at(g, entry, 1), index_f)));
}

/* ProcTexNoiseRand1D, ProcTexNoiseRand2D and ProcTexNoiseCoef, as swrasterizer/proctex.cpp */
static uint32_t noise_rand_1d(Gen* g, uint32_t v) {
    static const int32_t table[16] = {0, 4, 10, 8, 4, 9, 7, 12, 5, 15, 13, 14, 11, 15, 2, 11};
    uint32_t x = op2(g, SPV_OP_S_MOD, g->t_i, v, ic(g, 9));
    x = op2(g, SPV_OP_I_MUL, g->t_i, op2(g, SPV_OP_I_ADD, g->t_i, x, ic(g, 2)), ic(g, 3));
    x = op2(g, SPV_OP_BITWISE_AND, g->t_i, x, ic(g, 0xf));
    uint32_t index = op2(g, SPV_OP_BITWISE_AND, g->t_i, op2(g, SPV_OP_S_DIV, g->t_i, v, ic(g, 9)), ic(g, 0xf));
    return op2(g, SPV_OP_BITWISE_XOR, g->t_i, x, table_at(g, table, 16, index));
}
static uint32_t noise_rand_2d(Gen* g, uint32_t point) {
    static const int32_t table[16] = {10, 2, 15, 8, 0, 7, 4, 5, 5, 13, 2, 6, 13, 9, 3, 14};
    uint32_t u2 = noise_rand_1d(g, op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, at(g, point, 0)));
    uint32_t v2 = noise_rand_1d(g, op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, at(g, point, 1)));
    uint32_t odd = op2(g, SPV_OP_I_EQUAL, g->t_bool, op2(g, SPV_OP_BITWISE_AND, g->t_i, u2, ic(g, 3)), ic(g, 1));
    v2 = op2(g, SPV_OP_I_ADD, g->t_i, v2, choose(g, g->t_i, odd, ic(g, 4), ic(g, 0)));
    v2 = op2(g, SPV_OP_BITWISE_XOR, g->t_i, v2,
             op2(g, SPV_OP_I_MUL, g->t_i, op2(g, SPV_OP_BITWISE_AND, g->t_i, u2, ic(g, 1)), ic(g, 6)));
    v2 = op2(g, SPV_OP_I_ADD, g->t_i, v2, op2(g, SPV_OP_I_ADD, g->t_i, ic(g, 10), u2));
    v2 = op2(g, SPV_OP_BITWISE_AND, g->t_i, v2, ic(g, 0xf));
    v2 = op2(g, SPV_OP_BITWISE_XOR, g->t_i, v2, table_at(g, table, 16, u2));
    return fadd(g, g->t_f, fc(g, g->t_f, -1.0f),
                fmul(g, g->t_f, op1(g, SPV_OP_CONVERT_S_TO_F, g->t_f, v2), fc(g, g->t_f, (float)(2.0 / 15.0))));
}
static uint32_t noise_coef(Gen* g, uint32_t x) {
    uint32_t grid = fmul(g, g->t_v2, scale(g, g->t_v2, member(g, g->t_v2, U_PROCTEX_NOISE_F), fc(g, g->t_f, 9.0f)),
                         ext1(g, g->t_v2, SPV_GLSL_F_ABS, fadd(g, g->t_v2, x, member(g, g->t_v2, U_PROCTEX_NOISE_P))));
    uint32_t point = ext1(g, g->t_v2, SPV_GLSL_FLOOR, grid);
    uint32_t frac = fsub(g, g->t_v2, grid, point);
    uint32_t sum = fadd(g, g->t_f, at(g, frac, 0), at(g, frac, 1));
    uint32_t g0 = fmul(g, g->t_f, noise_rand_2d(g, point), sum);
    uint32_t g1 = fmul(g, g->t_f, noise_rand_2d(g, fadd(g, g->t_v2, point, vec_constant(g, g->t_v2, 1, 0, 0, 0))),
                       fsub(g, g->t_f, sum, fc(g, g->t_f, 1.0f)));
    uint32_t g2 = fmul(g, g->t_f, noise_rand_2d(g, fadd(g, g->t_v2, point, vec_constant(g, g->t_v2, 0, 1, 0, 0))),
                       fsub(g, g->t_f, sum, fc(g, g->t_f, 1.0f)));
    uint32_t g3 = fmul(g, g->t_f, noise_rand_2d(g, fadd(g, g->t_v2, point, vec_constant(g, g->t_v2, 1, 1, 0, 0))),
                       fsub(g, g->t_f, sum, fc(g, g->t_f, 2.0f)));
    uint32_t noise_offset = member(g, g->t_i, U_PROCTEX_NOISE_LUT_OFFSET);
    uint32_t x_noise = proctex_lut(g, noise_offset, at(g, frac, 0));
    uint32_t y_noise = proctex_lut(g, noise_offset, at(g, frac, 1));
    uint32_t x0 = ext3(g, g->t_f, SPV_GLSL_F_MIX, g0, g1, x_noise);
    uint32_t x1 = ext3(g, g->t_f, SPV_GLSL_F_MIX, g2, g3, x_noise);
    return ext3(g, g->t_f, SPV_GLSL_F_MIX, x0, x1, y_noise);
}

static uint32_t proctex_shift(Gen* g, uint32_t v, unsigned mode, unsigned clamp_mode) {
    uint32_t offset = fc(g, g->t_f, clamp_mode == 3 ? 1.0f : 0.5f), i;
    if (mode != 1 && mode != 2) return fc(g, g->t_f, 0.0f);
    i = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, v);
    if (mode == 2) i = op2(g, SPV_OP_I_ADD, g->t_i, i, ic(g, 1));
    i = op2(g, SPV_OP_S_MOD, g->t_i, op2(g, SPV_OP_S_DIV, g->t_i, i, ic(g, 2)), ic(g, 2));
    return fmul(g, g->t_f, offset, op1(g, SPV_OP_CONVERT_S_TO_F, g->t_f, i));
}

static uint32_t proctex_clamp(Gen* g, uint32_t v, unsigned mode) {
    uint32_t t = g->t_f, b = g->t_bool;
    switch (mode) {
        case 0: return choose(g, t, op2(g, SPV_OP_F_ORD_GREATER_THAN, b, v, fc(g, t, 1.0f)), fc(g, t, 0.0f), v);
        case 2: return ext1(g, t, SPV_GLSL_FRACT, v);
        case 3: {
            uint32_t parity = op2(g, SPV_OP_S_MOD, g->t_i, op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, v), ic(g, 2));
            uint32_t even = op2(g, SPV_OP_I_EQUAL, b, parity, ic(g, 0));
            return choose(g, t, even, ext1(g, t, SPV_GLSL_FRACT, v),
                          fsub(g, t, fc(g, t, 1.0f), ext1(g, t, SPV_GLSL_FRACT, v)));
        }
        case 4:
            return choose(g, t, op2(g, SPV_OP_F_ORD_GREATER_THAN, b, v, fc(g, t, 0.5f)), fc(g, t, 1.0f),
                          fc(g, t, 0.0f));
        default: return ext2(g, t, SPV_GLSL_F_MIN, v, fc(g, t, 1.0f)); /* to edge, and unknown modes */
    }
}

static uint32_t proctex_combine(Gen* g, unsigned combiner, unsigned offset, uint32_t u, uint32_t v) {
    uint32_t t = g->t_f, x;
    switch (combiner) {
        case 0: x = u; break;
        case 1: x = fmul(g, t, u, u); break;
        case 2: x = v; break;
        case 3: x = fmul(g, t, v, v); break;
        case 4: x = fmul(g, t, fadd(g, t, u, v), fc(g, t, 0.5f)); break;
        case 5: x = fmul(g, t, fadd(g, t, fmul(g, t, u, u), fmul(g, t, v, v)), fc(g, t, 0.5f)); break;
        case 6:
            x = ext2(g, t, SPV_GLSL_F_MIN,
                     ext1(g, t, SPV_GLSL_SQRT, fadd(g, t, fmul(g, t, u, u), fmul(g, t, v, v))), fc(g, t, 1.0f));
            break;
        case 7: x = ext2(g, t, SPV_GLSL_F_MIN, u, v); break;
        case 8: x = ext2(g, t, SPV_GLSL_F_MAX, u, v); break;
        case 9: {
            uint32_t length = ext1(g, t, SPV_GLSL_SQRT, fadd(g, t, fmul(g, t, u, u), fmul(g, t, v, v)));
            x = fmul(g, t, fadd(g, t, fmul(g, t, fadd(g, t, u, v), fc(g, t, 0.5f)), length), fc(g, t, 0.5f));
            x = ext2(g, t, SPV_GLSL_F_MIN, x, fc(g, t, 1.0f));
            break;
        }
        default: x = fc(g, t, 0.0f); break;
    }
    return proctex_lut(g, member(g, g->t_i, offset), x);
}

/* SampleProcTexColor(lut_coord, level) */
static uint32_t proctex_color(Gen* g, uint32_t lut_coord, uint32_t level, bool linear) {
    const SnxCtrFsConfig* c = g->c;
    /* the offsets of levels 4-7 seem hardcoded */
    const int32_t lut_offsets[8] = {c->proctex.lut_offset[0], c->proctex.lut_offset[1], c->proctex.lut_offset[2],
                                    c->proctex.lut_offset[3], 0xf0, 0xf8, 0xfc, 0xfe};
    uint32_t lut_width = op2(g, SPV_OP_SHIFT_RIGHT_ARITHMETIC, g->t_i, ic(g, c->proctex.lut_width), level);
    uint32_t lut_offset = table_at(g, lut_offsets, 8, level);
    /* 0.0 is lut[offset], 1.0 lut[offset + width - 1] */
    lut_coord = fmul(g, g->t_f, lut_coord,
                     op1(g, SPV_OP_CONVERT_S_TO_F, g->t_f, op2(g, SPV_OP_I_SUB, g->t_i, lut_width, ic(g, 1))));
    if (linear) {
        uint32_t index_i = op2(g, SPV_OP_I_ADD, g->t_i, op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, lut_coord), lut_offset);
        uint32_t index_f = ext1(g, g->t_f, SPV_GLSL_FRACT, lut_coord);
        uint32_t value = lut_fetch(g, LUT_RGBA,
                                   op2(g, SPV_OP_I_ADD, g->t_i, index_i, member(g, g->t_i, U_PROCTEX_LUT_OFFSET)));
        uint32_t diff = lut_fetch(g, LUT_RGBA,
                                  op2(g, SPV_OP_I_ADD, g->t_i, index_i, member(g, g->t_i, U_PROCTEX_DIFF_LUT_OFFSET)));
        return fadd(g, g->t_v4, value, scale(g, g->t_v4, diff, index_f));
    }
    lut_coord = fadd(g, g->t_f, lut_coord, op1(g, SPV_OP_CONVERT_S_TO_F, g->t_f, lut_offset));
    uint32_t index = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, ext1(g, g->t_f, SPV_GLSL_ROUND, lut_coord));
    return lut_fetch(g, LUT_RGBA, op2(g, SPV_OP_I_ADD, g->t_i, index, member(g, g->t_i, U_PROCTEX_LUT_OFFSET)));
}

/* ProcTex() */
static uint32_t proctex(Gen* g) {
    const SnxCtrFsConfig* c = g->c;
    uint32_t t = g->t_f;
    /* filters 6 and 7 are not PICA's; Azahar writes no lookup for them, here they are nearest */
    unsigned filter = c->proctex.lut_filter < 6 ? c->proctex.lut_filter : 0;
    bool linear = filter == 1 || filter == 3 || filter == 5;
    uint32_t uv = input(g, IN_TEXCOORD0 + (c->proctex.coord < 3 ? c->proctex.coord : 0));
    uv = ext1(g, g->t_v2, SPV_GLSL_F_ABS, uv);
    /* OpenGL's LOD upper bound, m_u + m_v, with the bias inside the log2 (unlike normal textures) */
    uint32_t duv = ext2(g, g->t_v2, SPV_GLSL_F_MAX, ext1(g, g->t_v2, SPV_GLSL_F_ABS, op1(g, SPV_OP_DPDX, g->t_v2, uv)),
                        ext1(g, g->t_v2, SPV_GLSL_F_ABS, op1(g, SPV_OP_DPDY, g->t_v2, uv)));
    uint32_t bias = member(g, t, U_PROCTEX_BIAS);
    uint32_t lod = ext1(g, t, SPV_GLSL_LOG2,
                        fmul(g, t, ext1(g, t, SPV_GLSL_F_ABS, fmul(g, t, fc(g, t, (float)c->proctex.lut_width), bias)),
                             fadd(g, t, at(g, duv, 0), at(g, duv, 1))));
    lod = choose(g, t, op2(g, SPV_OP_F_ORD_EQUAL, g->t_bool, bias, fc(g, t, 0.0f)), fc(g, t, 0.0f), lod);
    lod = ext3(g, t, SPV_GLSL_F_CLAMP, lod, fc(g, t, (float)c->proctex.lod_min),
               fc(g, t, (float)(c->proctex.lod_max < 7 ? c->proctex.lod_max : 7u)));
    /* the shift offsets come before the noise */
    uint32_t u_shift = proctex_shift(g, at(g, uv, 1), c->proctex.u_shift, c->proctex.u_clamp);
    uint32_t v_shift = proctex_shift(g, at(g, uv, 0), c->proctex.v_shift, c->proctex.v_clamp);
    if (c->proctex.noise_enable) {
        uv = fadd(g, g->t_v2, uv, scale(g, g->t_v2, member(g, g->t_v2, U_PROCTEX_NOISE_A), noise_coef(g, uv)));
        uv = ext1(g, g->t_v2, SPV_GLSL_F_ABS, uv);
    }
    uint32_t u = proctex_clamp(g, fadd(g, t, at(g, uv, 0), u_shift), c->proctex.u_clamp);
    uint32_t v = proctex_clamp(g, fadd(g, t, at(g, uv, 1), v_shift), c->proctex.v_clamp);
    uint32_t lut_coord = proctex_combine(g, c->proctex.color_combiner, U_PROCTEX_COLOR_MAP_OFFSET, u, v), color;
    if (filter == 0 || filter == 1)
        color = proctex_color(g, lut_coord, ic(g, 0), linear);
    else if (filter == 2 || filter == 3)
        color = proctex_color(g, lut_coord, op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, ext1(g, t, SPV_GLSL_ROUND, lod)),
                              linear);
    else {
        uint32_t lod_i = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, lod), lod_f = ext1(g, t, SPV_GLSL_FRACT, lod);
        uint32_t c0 = proctex_color(g, lut_coord, lod_i, linear);
        uint32_t c1 = proctex_color(g, lut_coord, op2(g, SPV_OP_I_ADD, g->t_i, lod_i, ic(g, 1)), linear);
        color = ext3(g, g->t_v4, SPV_GLSL_F_MIX, c0, c1, splat(g, g->t_v4, lod_f));
    }
    if (!c->proctex.separate_alpha) return color;
    /* alpha skips the colour LUT: CombineAndMap's output directly */
    return with_component(g, g->t_v4, color, 3,
                          proctex_combine(g, c->proctex.alpha_combiner, U_PROCTEX_ALPHA_MAP_OFFSET, u, v));
}

/* ---- texture units ---- */

/* textureLod(tex, coord, getLod(coord * vec2(textureSize(tex, 0))) + tex_lod_bias[unit]) */
static uint32_t sample_lod(Gen* g, unsigned unit, unsigned coord) {
    uint32_t sampled = load(g, g->tex_type[unit], g->tex[unit]), uv = input(g, coord);
    uint32_t size = op1(g, SPV_OP_CONVERT_S_TO_F, g->t_v2, texture_size(g, unit, sampled));
    uint32_t lod = fadd(g, g->t_f, get_lod(g, fmul(g, g->t_v2, uv, size)), member_at(g, g->t_f, U_TEX_LOD_BIAS, unit));
    return SPV(&g->s, SPV_OP_IMAGE_SAMPLE_EXPLICIT_LOD, g->t_v4, sampled, uv, IMAGE_OPERANDS_LOD, lod);
}

/* DefineTexUnitSampler: the function sampleTexUnit0..3 */
static void unit_function(Gen* g, unsigned unit) {
    SpvEmitter* s = &g->s;
    const SnxCtrFsConfig* c = g->c;
    uint32_t result;
    spv_emit_function(s, g->unit[unit], g->t_v4, NULL, 0, NULL);
    if (unit == 0 && c->texture0_type == SNX_CTR_FS_TEX_DISABLED) {
        SPV_VOID(s, SPV_OP_RETURN_VALUE, fc(g, g->t_v4, 0.0f));
        spv_emit_function_end(s);
        return;
    }
    if (unit < 3) {
        unsigned coord = IN_TEXCOORD0 + (unit == 2 && c->texture2_use_coord1 ? 1 : unit);
        for (unsigned k = 0; k < 2; ++k) {
            if (!(k ? c->border_t[unit] : c->border_s[unit])) continue;
            uint32_t x = at(g, input(g, coord), k);
            uint32_t merge = if_begin(g, op2(g, SPV_OP_LOGICAL_OR, g->t_bool,
                                             op2(g, SPV_OP_F_ORD_LESS_THAN, g->t_bool, x, fc(g, g->t_f, 0.0f)),
                                             op2(g, SPV_OP_F_ORD_GREATER_THAN, g->t_bool, x, fc(g, g->t_f, 1.0f))));
            SPV_VOID(s, SPV_OP_RETURN_VALUE, member_at(g, g->t_v4, U_TEX_BORDER_COLOR, unit));
            if_end(g, merge, false);
        }
    }
    switch (unit) {
        case 0:
            switch (c->texture0_type) {
                case SNX_CTR_FS_TEX_2D: result = sample_lod(g, 0, IN_TEXCOORD0); break;
                case SNX_CTR_FS_TEX_PROJECTION: /* the exact LOD of projection textures is unknown */
                case SNX_CTR_FS_TEX_CUBE: {
                    uint32_t sampled = load(g, g->tex_type[0], g->tex[0]);
                    uint32_t coord = SPV(s, SPV_OP_COMPOSITE_CONSTRUCT, g->t_v3, input(g, IN_TEXCOORD0),
                                         input(g, IN_TEXCOORD0_W));
                    result = op2(g, c->texture0_type == SNX_CTR_FS_TEX_CUBE ? SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD
                                                                           : SPV_OP_IMAGE_SAMPLE_PROJ_IMPLICIT_LOD,
                                 g->t_v4, sampled, coord);
                    break;
                }
                case SNX_CTR_FS_TEX_SHADOW_2D: result = shadow_texture(g); break;
                case SNX_CTR_FS_TEX_SHADOW_CUBE: result = shadow_texture_cube(g); break;
                default: /* types 6 and 7 */
                    result = op2(g, SPV_OP_IMAGE_SAMPLE_IMPLICIT_LOD, g->t_v4, load(g, g->tex_type[0], g->tex[0]),
                                 input(g, IN_TEXCOORD0));
                    break;
            }
            break;
        case 1: result = sample_lod(g, 1, IN_TEXCOORD1); break;
        case 2: result = sample_lod(g, 2, c->texture2_use_coord1 ? IN_TEXCOORD1 : IN_TEXCOORD2); break;
        default: result = c->proctex.enable ? proctex(g) : fc(g, g->t_v4, 0.0f); break;
    }
    SPV_VOID(s, SPV_OP_RETURN_VALUE, result);
    spv_emit_function_end(s);
}

/* ---- the combiners ---- */

/* GetSource */
static uint32_t source(Gen* g, unsigned src, unsigned index) {
    switch (src) {
        case 0: return g->rounded_primary_color;
        case 1: return g->primary_fragment_color;
        case 2: return g->secondary_fragment_color;
        case 3:
        case 4:
        case 5:
        case 6: return SPV(&g->s, SPV_OP_FUNCTION_CALL, g->t_v4, g->unit[src - 3]);
        case 13: return g->combiner_buffer;
        case 14: return member_at(g, g->t_v4, U_CONST_COLOR, index);
        case 15: return g->combiner_output;
        default: return fc(g, g->t_v4, 0.0f);
    }
}

/* AppendColorModifier: rgb (4), a component in all three, or nothing (-1) */
static uint32_t color_modifier(Gen* g, unsigned modifier, unsigned src, unsigned index) {
    static const signed char swizzles[16] = {4, 4, 3, 3, 0, 0, -1, -1, 1, 1, -1, -1, 2, 2, -1, -1};
    int swizzle = swizzles[modifier & 15];
    if (index == 0 && src == 15) src = tev_field(g->c->tev_sources[0], 8, 4); /* stage 0 reads source 3 for previous */
    if (swizzle < 0) return fc(g, g->t_v3, 0.0f);
    uint32_t v = source(g, src, index), k = (uint32_t)swizzle;
    v = swizzle == 4 ? rgb(g, v) : SPV(&g->s, SPV_OP_VECTOR_SHUFFLE, g->t_v3, v, v, k, k, k);
    return modifier & 1 ? fsub(g, g->t_v3, fc(g, g->t_v3, 1.0f), v) : v;
}

/* AppendAlphaModifier */
static uint32_t alpha_modifier(Gen* g, unsigned modifier, unsigned src, unsigned index) {
    static const unsigned components[4] = {3, 0, 1, 2}; /* a r g b */
    if (index == 0 && src == 15) src = tev_field(g->c->tev_sources[0], 24, 4);
    uint32_t v = at(g, source(g, src, index), components[(modifier >> 1) & 3]);
    return modifier & 1 ? fsub(g, g->t_f, fc(g, g->t_f, 1.0f), v) : v;
}

/* the colour (vec3) or alpha (float) combiner of op, clamped */
static uint32_t combiner(Gen* g, uint32_t t, unsigned op, uint32_t r1, uint32_t r2, uint32_t r3) {
    uint32_t x;
    switch (op) {
        case 0: x = r1; break;
        case 1: x = fmul(g, t, r1, r2); break;
        case 2: x = fadd(g, t, r1, r2); break;
        case 3: x = fsub(g, t, fadd(g, t, r1, r2), fc(g, t, 0.5f)); break;
        case 4: x = ext3(g, t, SPV_GLSL_F_MIX, r2, r1, r3); break;
        case 5: x = fsub(g, t, r1, r2); break;
        case 6:
        case 7:
            if (t == g->t_f) {
                x = fc(g, t, 0.0f);
                break;
            }
            x = op2(g, SPV_OP_DOT, g->t_f, fsub(g, t, r1, fc(g, t, 0.5f)), fsub(g, t, r2, fc(g, t, 0.5f)));
            x = splat(g, t, fmul(g, g->t_f, x, fc(g, g->t_f, 4.0f)));
            break;
        case 8: x = ext3(g, t, SPV_GLSL_FMA, r1, r2, r3); break;
        case 9: x = fmul(g, t, ext2(g, t, SPV_GLSL_F_MIN, fadd(g, t, r1, r2), fc(g, t, 1.0f)), r3); break;
        default: x = fc(g, t, 0.0f); break;
    }
    return clamp01(g, t, x);
}

static void tev_stage_code(Gen* g, unsigned i) {
    const SnxCtrFsConfig* c = g->c;
    uint32_t s = c->tev_sources[i], m = c->tev_modifiers[i], o = c->tev_ops[i], k = c->tev_scales[i];
    if (!pass_through(c, i)) {
        unsigned color_op = tev_field(o, 0, 4);
        uint32_t r1 = color_modifier(g, tev_field(m, 0, 4), tev_field(s, 0, 4), i);
        uint32_t r2 = color_modifier(g, tev_field(m, 4, 4), tev_field(s, 4, 4), i);
        uint32_t r3 = color_modifier(g, tev_field(m, 8, 4), tev_field(s, 8, 4), i);
        /* each stage's output rounded to the PICA's 8 bits */
        uint32_t color_output = byteround(g, g->t_v3, combiner(g, g->t_v3, color_op, r1, r2, r3)), alpha_output;
        if (color_op == 7) /* Dot3_RGBA puts the result in alpha too */
            alpha_output = at(g, color_output, 0);
        else {
            uint32_t a1 = alpha_modifier(g, tev_field(m, 12, 3), tev_field(s, 16, 4), i);
            uint32_t a2 = alpha_modifier(g, tev_field(m, 16, 3), tev_field(s, 20, 4), i);
            uint32_t a3 = alpha_modifier(g, tev_field(m, 20, 3), tev_field(s, 24, 4), i);
            alpha_output = byteround(g, g->t_f, combiner(g, g->t_f, tev_field(o, 16, 4), a1, a2, a3));
        }
        uint32_t color = clamp01(g, g->t_v3, scale(g, g->t_v3, color_output,
                                                   fc(g, g->t_f, (float)multiplier(tev_field(k, 0, 2)))));
        uint32_t alpha = clamp01(g, g->t_f, fmul(g, g->t_f, alpha_output,
                                                 fc(g, g->t_f, (float)multiplier(tev_field(k, 16, 2)))));
        g->combiner_output = SPV(&g->s, SPV_OP_COMPOSITE_CONSTRUCT, g->t_v4, color, alpha);
    }
    g->combiner_buffer = g->next_combiner_buffer;
    if (i < 4 && (c->combiner_buffer_input & (1u << i)))
        g->next_combiner_buffer = with_rgb(g, g->next_combiner_buffer, rgb(g, g->combiner_output));
    if (i < 4 && ((c->combiner_buffer_input >> 4) & (1u << i)))
        g->next_combiner_buffer = with_component(g, g->t_v4, g->next_combiner_buffer, 3, at(g, g->combiner_output, 3));
}

static void alpha_test(Gen* g, unsigned func) {
    static const uint32_t ops[6] = {SPV_OP_I_NOT_EQUAL,          SPV_OP_I_EQUAL,          SPV_OP_S_GREATER_THAN_EQUAL,
                                    SPV_OP_S_GREATER_THAN,       SPV_OP_S_LESS_THAN_EQUAL, SPV_OP_S_LESS_THAN};
    if (func < 2) return; /* never passes is main's, always has nothing to test */
    uint32_t a = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i,
                     fmul(g, g->t_f, at(g, g->combiner_output, 3), fc(g, g->t_f, 255.0f)));
    kill_if(g, op2(g, ops[func - 2], g->t_bool, a, member(g, g->t_i, U_ALPHATEST_REF)));
}

/* ---- lighting ---- */

typedef struct {
    uint32_t normal, tangent, view, light_vector, spot_dir, half_vector;
} Vectors;

/* get_lut_value: a LookupLightingLUT* of the sampler on the input */
static uint32_t lut_value(Gen* g, const Vectors* v, unsigned sampler, unsigned light_num, unsigned input_type,
                          bool absolute) {
    const SnxCtrFsConfig* c = g->c;
    uint32_t t = g->t_v3, index;
    switch (input_type) {
        case 0:
            index = op2(g, SPV_OP_DOT, g->t_f, v->normal, ext1(g, t, SPV_GLSL_NORMALIZE, v->half_vector));
            break;
        case 1:
            index = op2(g, SPV_OP_DOT, g->t_f, ext1(g, t, SPV_GLSL_NORMALIZE, v->view),
                        ext1(g, t, SPV_GLSL_NORMALIZE, v->half_vector));
            break;
        case 2: index = op2(g, SPV_OP_DOT, g->t_f, v->normal, ext1(g, t, SPV_GLSL_NORMALIZE, v->view)); break;
        case 3: index = op2(g, SPV_OP_DOT, g->t_f, v->light_vector, v->normal); break;
        case 4: index = op2(g, SPV_OP_DOT, g->t_f, v->light_vector, v->spot_dir); break;
        case 5:
            /* CP only in configuration 7: the half vector projected on the (perturbed) normal's plane, not
               normalized, against the tangent */
            if (c->lighting.config == 8) {
                uint32_t h = ext1(g, t, SPV_GLSL_NORMALIZE, v->half_vector);
                uint32_t projected = fsub(g, t, h, scale(g, t, v->normal, op2(g, SPV_OP_DOT, g->t_f, v->normal, h)));
                index = op2(g, SPV_OP_DOT, g->t_f, projected, v->tangent);
            } else
                index = fc(g, g->t_f, 0.0f);
            break;
        default: index = fc(g, g->t_f, 0.0f); break;
    }
    if (!absolute) return lighting_lut_signed(g, sampler, index);
    /* Azahar indexes lights by the light's number here, not its slot */
    if (c->lighting.lights[light_num].two_sided_diffuse)
        index = ext1(g, g->t_f, SPV_GLSL_F_ABS, index);
    else
        index = ext2(g, g->t_f, SPV_GLSL_F_MAX, index, fc(g, g->t_f, 0.0f));
    return lighting_lut_unsigned(g, sampler, index);
}

/* scale * value of a LUT the config enables and supports */
static uint32_t scaled_lut(Gen* g, const Vectors* v, unsigned which, unsigned sampler, unsigned light_num) {
    const SnxCtrFsConfig* c = g->c;
    return fmul(g, g->t_f, fc(g, g->t_f, lut_scale(c->lighting.lut[which].scale)),
                lut_value(g, v, sampler, light_num, c->lighting.lut[which].type, c->lighting.lut[which].abs_input));
}
/* the LUT's scaled value when it is used, else otherwise */
static uint32_t lut_or(Gen* g, const Vectors* v, unsigned which, unsigned sampler, unsigned light_num,
                       uint32_t otherwise) {
    return lut_used(g->c, which, sampler) ? scaled_lut(g, v, which, sampler, light_num) : otherwise;
}

static void lighting(Gen* g) {
    const SnxCtrFsConfig* c = g->c;
    uint32_t f = g->t_f, v3 = g->t_v3, v4 = g->t_v4;
    if (!c->lighting.enable) return;
    uint32_t diffuse_sum = vec_constant(g, v4, 0, 0, 0, 1), specular_sum = diffuse_sum;
    uint32_t clamp_highlights = fc(g, f, 1.0f), geo_factor = fc(g, f, 1.0f), surface_normal, surface_tangent;
    switch (c->lighting.bump_mode) {
        case 1: /* normal map; renorm recomputes z for precision */
            surface_normal = fsub(g, v3, scale(g, v3, rgb(g, source(g, 3 + c->lighting.bump_selector, 0)),
                                               fc(g, f, 2.0f)), fc(g, v3, 1.0f));
            if (c->lighting.bump_renorm) {
                uint32_t x = at(g, surface_normal, 0), y = at(g, surface_normal, 1);
                uint32_t z = fsub(g, f, fc(g, f, 1.0f), fadd(g, f, fmul(g, f, x, x), fmul(g, f, y, y)));
                z = ext1(g, f, SPV_GLSL_SQRT, ext2(g, f, SPV_GLSL_F_MAX, z, fc(g, f, 0.0f)));
                surface_normal = with_component(g, v3, surface_normal, 2, z);
            }
            surface_tangent = vec_constant(g, v3, 1, 0, 0, 0);
            break;
        case 2: /* tangent map: z is not recomputed, it does not change the result */
            surface_tangent = fsub(g, v3, scale(g, v3, rgb(g, source(g, 3 + c->lighting.bump_selector, 0)),
                                                fc(g, f, 2.0f)), fc(g, v3, 1.0f));
            surface_normal = vec_constant(g, v3, 0, 0, 1, 0);
            break;
        default:
            surface_normal = vec_constant(g, v3, 0, 0, 1, 0);
            surface_tangent = vec_constant(g, v3, 1, 0, 0, 0);
            break;
    }
    Vectors v = {0, 0, 0, 0, 0, 0};
    uint32_t normalized_normquat = ext1(g, v4, SPV_GLSL_NORMALIZE, input(g, IN_NORMQUAT));
    v.normal = quaternion_rotate(g, normalized_normquat, surface_normal);
    v.tangent = quaternion_rotate(g, normalized_normquat, surface_tangent);
    v.view = input(g, IN_VIEW);
    uint32_t shadow = fc(g, v4, 1.0f);
    if (c->lighting.enable_shadow) {
        shadow = source(g, 3 + c->lighting.shadow_selector, 0);
        if (c->lighting.shadow_invert) shadow = fsub(g, v4, fc(g, v4, 1.0f), shadow);
    }

    for (unsigned li = 0; li < c->lighting.src_num; ++li) {
        unsigned num = c->lighting.lights[li].num;
        bool last = li + 1 == c->lighting.src_num, shadowed = c->lighting.lights[li].shadow;
        uint32_t light_vector = light(g, v3, num, LS_POSITION);
        if (!c->lighting.lights[li].directional) light_vector = fadd(g, v3, light_vector, v.view);
        uint32_t light_distance = ext1(g, f, SPV_GLSL_LENGTH, light_vector);
        v.light_vector = ext1(g, v3, SPV_GLSL_NORMALIZE, light_vector);
        v.spot_dir = light(g, v3, num, LS_SPOT_DIRECTION);
        v.half_vector = fadd(g, v3, ext1(g, v3, SPV_GLSL_NORMALIZE, v.view), v.light_vector);
        uint32_t dot_product = op2(g, SPV_OP_DOT, f, v.light_vector, v.normal);
        dot_product = c->lighting.lights[li].two_sided_diffuse ? ext1(g, f, SPV_GLSL_F_ABS, dot_product)
                                                              : ext2(g, f, SPV_GLSL_F_MAX, dot_product, fc(g, f, 0.0f));
        if (c->lighting.clamp_highlights) clamp_highlights = ext1(g, f, SPV_GLSL_F_SIGN, dot_product);
        if (c->lighting.lights[li].geometric_factor_0 || c->lighting.lights[li].geometric_factor_1) {
            geo_factor = op2(g, SPV_OP_DOT, f, v.half_vector, v.half_vector);
            geo_factor = choose(g, f, op2(g, SPV_OP_F_ORD_EQUAL, g->t_bool, geo_factor, fc(g, f, 0.0f)), fc(g, f, 0.0f),
                                ext2(g, f, SPV_GLSL_F_MIN, op2(g, SPV_OP_F_DIV, f, dot_product, geo_factor),
                                     fc(g, f, 1.0f)));
        }
        /* spot and distance attenuation */
        uint32_t spot_atten = c->lighting.lights[li].spot_atten && sampler_supported(c->lighting.config, S_SP)
                                  ? scaled_lut(g, &v, L_SP, S_SP + num, num)
                                  : fc(g, f, 1.0f);
        uint32_t dist_atten = fc(g, f, 1.0f);
        if (c->lighting.lights[li].dist_atten)
            dist_atten = lighting_lut_unsigned(
                g, S_DA + num,
                clamp01(g, f, fadd(g, f, fmul(g, f, light(g, f, num, LS_DIST_ATTEN_SCALE), light_distance),
                                   light(g, f, num, LS_DIST_ATTEN_BIAS))));
        /* specular 0 */
        uint32_t specular_0 = scale(g, v3, light(g, v3, num, LS_SPECULAR_0),
                                    lut_or(g, &v, L_D0, S_D0, num, fc(g, f, 1.0f)));
        if (c->lighting.lights[li].geometric_factor_0) specular_0 = scale(g, v3, specular_0, geo_factor);
        /* reflection: red, then green and blue or red's */
        uint32_t refl_r = lut_or(g, &v, L_RR, S_RR, num, fc(g, f, 1.0f));
        uint32_t refl_g = lut_or(g, &v, L_RG, S_RG, num, refl_r);
        uint32_t refl_b = lut_or(g, &v, L_RB, S_RB, num, refl_r);
        uint32_t refl_value = SPV(&g->s, SPV_OP_COMPOSITE_CONSTRUCT, v3, refl_r, refl_g, refl_b);
        /* specular 1 */
        uint32_t specular_1 = fmul(g, v3, scale(g, v3, refl_value, lut_or(g, &v, L_D1, S_D1, num, fc(g, f, 1.0f))),
                                   light(g, v3, num, LS_SPECULAR_1));
        if (c->lighting.lights[li].geometric_factor_1) specular_1 = scale(g, v3, specular_1, geo_factor);
        /* Fresnel: only the last slot applies it */
        if (last && lut_used(c, L_FR, S_FR) &&
            (c->lighting.enable_primary_alpha || c->lighting.enable_secondary_alpha)) {
            uint32_t fresnel = scaled_lut(g, &v, L_FR, S_FR, num);
            if (c->lighting.enable_primary_alpha) diffuse_sum = with_component(g, v4, diffuse_sum, 3, fresnel);
            if (c->lighting.enable_secondary_alpha) specular_sum = with_component(g, v4, specular_sum, 3, fresnel);
        }
        uint32_t diffuse = scale(g, v3, light(g, v3, num, LS_DIFFUSE), dot_product);
        if (c->lighting.shadow_primary && shadowed) diffuse = fmul(g, v3, diffuse, rgb(g, shadow));
        diffuse = fadd(g, v3, diffuse, light(g, v3, num, LS_AMBIENT));
        diffuse = scale(g, v3, scale(g, v3, diffuse, dist_atten), spot_atten);
        diffuse_sum = with_rgb(g, diffuse_sum, fadd(g, v3, rgb(g, diffuse_sum), diffuse));
        uint32_t specular = scale(g, v3, fadd(g, v3, specular_0, specular_1), clamp_highlights);
        specular = scale(g, v3, scale(g, v3, specular, dist_atten), spot_atten);
        if (c->lighting.shadow_secondary && shadowed) specular = fmul(g, v3, specular, rgb(g, shadow));
        specular_sum = with_rgb(g, specular_sum, fadd(g, v3, rgb(g, specular_sum), specular));
    }
    if (c->lighting.shadow_alpha) {
        if (c->lighting.enable_primary_alpha)
            diffuse_sum = with_component(g, v4, diffuse_sum, 3, fmul(g, f, at(g, diffuse_sum, 3), at(g, shadow, 3)));
        if (c->lighting.enable_secondary_alpha)
            specular_sum = with_component(g, v4, specular_sum, 3, fmul(g, f, at(g, specular_sum, 3), at(g, shadow, 3)));
    }
    diffuse_sum = with_rgb(g, diffuse_sum, fadd(g, v3, rgb(g, diffuse_sum), member(g, v3, U_LIGHTING_GLOBAL_AMBIENT)));
    g->primary_fragment_color = clamp01(g, v4, diffuse_sum);
    g->secondary_fragment_color = clamp01(g, v4, specular_sum);
}

/* ---- main ---- */

static void fog(Gen* g, uint32_t depth) {
    uint32_t f = g->t_f;
    uint32_t fog_index = g->c->fog_flip ? fmul(g, f, fsub(g, f, fc(g, f, 1.0f), depth), fc(g, f, 128.0f))
                                        : fmul(g, f, depth, fc(g, f, 128.0f));
    uint32_t fog_i = ext3(g, f, SPV_GLSL_F_CLAMP, ext1(g, f, SPV_GLSL_FLOOR, fog_index), fc(g, f, 0.0f),
                          fc(g, f, 127.0f));
    uint32_t fog_f = fsub(g, f, fog_index, fog_i);
    uint32_t entry = lut_fetch(g, LUT_LF, op2(g, SPV_OP_I_ADD, g->t_i, op1(g, SPV_OP_CONVERT_F_TO_S, g->t_i, fog_i),
                                              member(g, g->t_i, U_FOG_LUT_OFFSET)));
    uint32_t fog_factor = clamp01(g, f, fadd(g, f, at(g, entry, 0), fmul(g, f, at(g, entry, 1), fog_f)));
    g->combiner_output =
        with_rgb(g, g->combiner_output,
                 ext3(g, g->t_v3, SPV_GLSL_F_MIX, member(g, g->t_v3, U_FOG_COLOR), rgb(g, g->combiner_output),
                      splat(g, g->t_v3, fog_factor)));
}

/* UpdateShadow(old, d, s), its ref in two Function variables */
static uint32_t update_shadow(Gen* g, uint32_t pixel, uint32_t d, uint32_t s) {
    SpvEmitter* sp = &g->s;
    uint32_t u = g->t_u, ref_x = spv_emit_local(sp, u), ref_y = spv_emit_local(sp, u);
    uint32_t x = op2(g, SPV_OP_SHIFT_RIGHT_LOGICAL, u, pixel, uc(g, 8));
    store(g, ref_x, x);
    store(g, ref_y, op2(g, SPV_OP_BITWISE_AND, u, pixel, uc(g, 0xff)));
    uint32_t outer = if_begin(g, op2(g, SPV_OP_U_LESS_THAN, g->t_bool, d, x));
    uint32_t zero = spv_emit_id(sp), nonzero = spv_emit_id(sp), merge = spv_emit_id(sp);
    uint32_t is_zero = op2(g, SPV_OP_I_EQUAL, g->t_bool, s, uc(g, 0));
    SPV_VOID(sp, SPV_OP_SELECTION_MERGE, merge, SPV_SELECTION_CONTROL_NONE);
    SPV_VOID(sp, SPV_OP_BRANCH_CONDITIONAL, is_zero, zero, nonzero);
    spv_emit_label(sp, zero);
    store(g, ref_x, d);
    SPV_VOID(sp, SPV_OP_BRANCH, merge);
    spv_emit_label(sp, nonzero);
    uint32_t bias = fadd(g, g->t_f, member(g, g->t_f, U_SHADOW_BIAS_CONSTANT),
                         op2(g, SPV_OP_F_DIV, g->t_f,
                             fmul(g, g->t_f, member(g, g->t_f, U_SHADOW_BIAS_LINEAR),
                                  op1(g, SPV_OP_CONVERT_U_TO_F, g->t_f, d)),
                             op1(g, SPV_OP_CONVERT_U_TO_F, g->t_f, load(g, u, ref_x))));
    uint32_t biased = op1(g, SPV_OP_CONVERT_F_TO_U, u,
                          op2(g, SPV_OP_F_DIV, g->t_f, op1(g, SPV_OP_CONVERT_U_TO_F, g->t_f, s), bias));
    store(g, ref_y, ext2(g, u, SPV_GLSL_U_MIN, biased, load(g, u, ref_y)));
    SPV_VOID(sp, SPV_OP_BRANCH, merge);
    spv_emit_label(sp, merge);
    if_end(g, outer, true);
    /* EncodeShadow */
    return op2(g, SPV_OP_BITWISE_OR, u, op2(g, SPV_OP_SHIFT_LEFT_LOGICAL, u, load(g, u, ref_x), uc(g, 8)),
               load(g, u, ref_y));
}

/* the shadow map's compare-and-swap loop */
static void shadow_write(Gen* g, uint32_t depth) {
    SpvEmitter* s = &g->s;
    uint32_t u = g->t_u;
    uint32_t d = op1(g, SPV_OP_CONVERT_F_TO_U, u,
                     fmul(g, g->t_f, clamp01(g, g->t_f, depth), fc(g, g->t_f, 16777215.0f)));
    uint32_t stencil = op1(g, SPV_OP_CONVERT_F_TO_U, u, fmul(g, g->t_f, at(g, g->combiner_output, 1),
                                                             fc(g, g->t_f, 255.0f)));
    uint32_t frag_coord = load(g, g->t_v4, g->frag_coord);
    uint32_t image_coord = op1(g, SPV_OP_CONVERT_F_TO_S, g->t_iv2,
                               SPV(s, SPV_OP_VECTOR_SHUFFLE, g->t_v2, frag_coord, frag_coord, 0, 1));
    uint32_t old = spv_emit_local(s, u), new1 = spv_emit_local(s, u), old2 = spv_emit_local(s, u);
    uint32_t texel = op2(g, SPV_OP_IMAGE_READ, g->t_uv4, load(g, g->shadow_image, g->shadow_buffer), image_coord);
    store(g, old, SPV(s, SPV_OP_COMPOSITE_EXTRACT, u, texel, 0));
    uint32_t header = spv_emit_id(s), body = spv_emit_id(s), next = spv_emit_id(s), merge = spv_emit_id(s);
    SPV_VOID(s, SPV_OP_BRANCH, header);
    spv_emit_label(s, header);
    SPV_VOID(s, SPV_OP_LOOP_MERGE, merge, next, SPV_LOOP_CONTROL_NONE);
    SPV_VOID(s, SPV_OP_BRANCH, body);
    spv_emit_label(s, body);
    uint32_t value = load(g, u, old);
    store(g, old2, value);
    store(g, new1, update_shadow(g, value, d, stencil));
    SPV_VOID(s, SPV_OP_BRANCH, next);
    /* while ((old = imageAtomicCompSwap(shadow_buffer, image_coord, old, new1)) != old2) */
    spv_emit_label(s, next);
    uint32_t pointer = SPV(s, OP_IMAGE_TEXEL_POINTER, spv_emit_type_pointer(s, SPV_STORAGE_IMAGE, u), g->shadow_buffer,
                           image_coord, uc(g, 0));
    uint32_t swapped = SPV(s, SPV_OP_ATOMIC_COMPARE_EXCHANGE, u, pointer, uc(g, SCOPE_DEVICE), uc(g, 0), uc(g, 0),
                           load(g, u, new1), load(g, u, old));
    store(g, old, swapped);
    SPV_VOID(s, SPV_OP_BRANCH_CONDITIONAL, op2(g, SPV_OP_I_NOT_EQUAL, g->t_bool, swapped, load(g, u, old2)), header,
             merge);
    spv_emit_label(s, merge);
}

/* FragmentModule::Generate's main */
static void main_function(Gen* g, uint32_t id) {
    SpvEmitter* s = &g->s;
    const SnxCtrFsConfig* c = g->c;
    uint32_t f = g->t_f, b = g->t_bool;
    spv_emit_function(s, id, spv_emit_type_void(s), NULL, 0, NULL);
    if (c->alpha_test_func == 0) { /* never passes */
        spv_emit_void_op(s, SPV_OP_KILL, NULL, 0);
        spv_emit_function_end(s);
        return;
    }
    /* the interpolated primary colour rounded to the PICA's 8 bits */
    g->rounded_primary_color = byteround(g, g->t_v4, input(g, IN_PRIMARY_COLOR));
    g->primary_fragment_color = g->secondary_fragment_color = fc(g, g->t_v4, 0.0f);
    if (c->scissor_mode) {
        uint32_t frag_coord = load(g, g->t_v4, g->frag_coord), x = at(g, frag_coord, 0), y = at(g, frag_coord, 1);
        uint32_t edges[4], inside;
        for (unsigned k = 0; k < 4; ++k) {
            uint32_t edge = op1(g, SPV_OP_CONVERT_S_TO_F, f, member(g, g->t_i, U_SCISSOR_X1 + k));
            edges[k] = op2(g, k < 2 ? SPV_OP_F_ORD_GREATER_THAN_EQUAL : SPV_OP_F_ORD_LESS_THAN, b, k % 2 ? y : x, edge);
        }
        inside = op2(g, SPV_OP_LOGICAL_AND, b, edges[0], edges[1]);
        inside = op2(g, SPV_OP_LOGICAL_AND, b, op2(g, SPV_OP_LOGICAL_AND, b, inside, edges[2]), edges[3]);
        kill_if(g, c->scissor_mode == 3 ? op1(g, SPV_OP_LOGICAL_NOT, b, inside) : inside);
    }
    /* depth: the vertex shader wrote -z to a [0, 1] range; un-negated, then PICA's transform */
    uint32_t frag_coord = load(g, g->t_v4, g->frag_coord);
    uint32_t z_over_w = op1(g, SPV_OP_F_NEGATE, f, at(g, frag_coord, 2));
    uint32_t depth = fadd(g, f, fmul(g, f, z_over_w, member(g, f, U_DEPTH_SCALE)), member(g, f, U_DEPTH_OFFSET));
    if (c->depthmap_enable == 0) depth = op2(g, SPV_OP_F_DIV, f, depth, at(g, frag_coord, 3));
    lighting(g);
    g->combiner_buffer = fc(g, g->t_v4, 0.0f);
    g->next_combiner_buffer = member(g, g->t_v4, U_TEV_COMBINER_BUFFER_COLOR);
    g->combiner_output = fc(g, g->t_v4, 0.0f);
    for (unsigned i = 0; i < 6; ++i) tev_stage_code(g, i);
    alpha_test(g, c->alpha_test_func);
    if (c->fog_mode == 5) fog(g, depth);
    if (c->fog_mode == 7) /* gas is not emulated: a transparent pixel (discarding breaks some games) */
        store(g, g->color, fc(g, g->t_v4, 0.0f));
    else if (c->shadow_rendering)
        shadow_write(g, depth);
    else {
        store(g, g->frag_depth, depth);
        store(g, g->color, byteround(g, g->t_v4, g->combiner_output));
    }
    spv_emit_void_op(s, SPV_OP_RETURN, NULL, 0);
    spv_emit_function_end(s);
}

/* ---- the module ---- */

static void declare(Gen* g) {
    SpvEmitter* s = &g->s;
    const SnxCtrFsConfig* c = g->c;
    unsigned type0 = c->texture0_type;
    g->t_bool = spv_emit_type_bool(s);
    g->t_f = spv_emit_type_float(s, 32);
    g->t_v2 = spv_emit_type_vector(s, g->t_f, 2);
    g->t_v3 = spv_emit_type_vector(s, g->t_f, 3);
    g->t_v4 = spv_emit_type_vector(s, g->t_f, 4);
    g->t_i = spv_emit_type_int(s, 32, true);
    g->t_iv2 = spv_emit_type_vector(s, g->t_i, 2);
    g->t_iv4 = spv_emit_type_vector(s, g->t_i, 4);
    g->t_u = spv_emit_type_int(s, 32, false);
    g->t_uv4 = spv_emit_type_vector(s, g->t_u, 4);
    g->t_bv2 = spv_emit_type_vector(s, g->t_bool, 2);
    g->t_bv4 = spv_emit_type_vector(s, g->t_bool, 4);
    for (unsigned i = 0; i < IN_COUNT; ++i) g->in[i] = spv_emit_location(s, SPV_STORAGE_INPUT, input_type(g, i), i);
    g->color = spv_emit_location(s, SPV_STORAGE_OUTPUT, g->t_v4, 0);
    g->frag_coord = spv_emit_builtin(s, SPV_STORAGE_INPUT, g->t_v4, SPV_BUILT_IN_FRAG_COORD);
    if (writes_depth(c)) g->frag_depth = spv_emit_builtin(s, SPV_STORAGE_OUTPUT, g->t_f, SPV_BUILT_IN_FRAG_DEPTH);

    /* fs_data, std140, at SnxCtrFsUniforms' offsets */
    static const uint32_t light_offsets[LS_COUNT] = {
        offsetof(SnxCtrFsLight, specular_0), offsetof(SnxCtrFsLight, specular_1),
        offsetof(SnxCtrFsLight, diffuse),    offsetof(SnxCtrFsLight, ambient),
        offsetof(SnxCtrFsLight, position),   offsetof(SnxCtrFsLight, spot_direction),
        offsetof(SnxCtrFsLight, dist_atten_bias), offsetof(SnxCtrFsLight, dist_atten_scale)};
    static const uint32_t offsets[U_COUNT] = {
        offsetof(SnxCtrFsUniforms, framebuffer_scale),
        offsetof(SnxCtrFsUniforms, alphatest_ref),
        offsetof(SnxCtrFsUniforms, depth_scale),
        offsetof(SnxCtrFsUniforms, depth_offset),
        offsetof(SnxCtrFsUniforms, shadow_bias_constant),
        offsetof(SnxCtrFsUniforms, shadow_bias_linear),
        offsetof(SnxCtrFsUniforms, scissor_x1),
        offsetof(SnxCtrFsUniforms, scissor_y1),
        offsetof(SnxCtrFsUniforms, scissor_x2),
        offsetof(SnxCtrFsUniforms, scissor_y2),
        offsetof(SnxCtrFsUniforms, fog_lut_offset),
        offsetof(SnxCtrFsUniforms, proctex_noise_lut_offset),
        offsetof(SnxCtrFsUniforms, proctex_color_map_offset),
        offsetof(SnxCtrFsUniforms, proctex_alpha_map_offset),
        offsetof(SnxCtrFsUniforms, proctex_lut_offset),
        offsetof(SnxCtrFsUniforms, proctex_diff_lut_offset),
        offsetof(SnxCtrFsUniforms, proctex_bias),
        offsetof(SnxCtrFsUniforms, shadow_texture_bias),
        offsetof(SnxCtrFsUniforms, lighting_lut_offset),
        offsetof(SnxCtrFsUniforms, fog_color),
        offsetof(SnxCtrFsUniforms, proctex_noise_f),
        offsetof(SnxCtrFsUniforms, proctex_noise_a),
        offsetof(SnxCtrFsUniforms, proctex_noise_p),
        offsetof(SnxCtrFsUniforms, lighting_global_ambient),
        offsetof(SnxCtrFsUniforms, light_src),
        offsetof(SnxCtrFsUniforms, const_color),
        offsetof(SnxCtrFsUniforms, tev_combiner_buffer_color),
        offsetof(SnxCtrFsUniforms, tex_lod_bias),
        offsetof(SnxCtrFsUniforms, tex_border_color),
        offsetof(SnxCtrFsUniforms, blend_color)};
    uint32_t f = g->t_f, i = g->t_i, v2 = g->t_v2, v3 = g->t_v3, v4 = g->t_v4;
    uint32_t light_members[LS_COUNT] = {v3, v3, v3, v3, v3, v3, f, f};
    uint32_t light_src = spv_emit_type_struct(s, light_members, light_offsets, LS_COUNT);
    uint32_t arrays[4] = {spv_emit_type_array(s, g->t_iv4, 6), spv_emit_type_array(s, light_src, 8),
                          spv_emit_type_array(s, v4, 6), spv_emit_type_array(s, v4, 3)};
    const uint32_t strides[4] = {16, sizeof(SnxCtrFsLight), 16, 16};
    for (unsigned k = 0; k < 4; ++k) spv_emit_decorate(s, arrays[k], SPV_DECORATION_ARRAY_STRIDE, &strides[k], 1);
    uint32_t members[U_COUNT] = {i, i, f, f, f, f, i, i, i, i, i, i, i, i, i, i, f, i,
                                 arrays[0], v3, v2, v2, v2, v3, arrays[1], arrays[2], v4, v3, arrays[3], v4};
    uint32_t block = spv_emit_type_struct(s, members, offsets, U_COUNT);
    spv_emit_decorate(s, block, SPV_DECORATION_BLOCK, NULL, 0);
    g->fs_data = spv_emit_resource(s, SPV_STORAGE_UNIFORM, block, 0, 2);

    /* the LUTs: samplerBuffers */
    g->lut_image = spv_emit_type_image(s, f, SPV_DIM_BUFFER, 0, 0, 0, 1, SPV_IMAGE_FORMAT_UNKNOWN);
    g->lut_type = spv_emit_type_sampled_image(s, g->lut_image);
    for (unsigned k = 0; k < 3; ++k)
        g->lut[k] = spv_emit_resource(s, SPV_STORAGE_UNIFORM_CONSTANT, g->lut_type, 0, 3 + k);

    /* the textures */
    if (type0 == SNX_CTR_FS_TEX_SHADOW_2D || type0 == SNX_CTR_FS_TEX_SHADOW_CUBE)
        g->tex_image[0] = spv_emit_type_image(s, g->t_u, SPV_DIM_2D, 0, 0, 0, 1, SPV_IMAGE_FORMAT_UNKNOWN);
    else
        g->tex_image[0] = spv_emit_type_image(s, f, type0 == SNX_CTR_FS_TEX_CUBE ? SPV_DIM_CUBE : SPV_DIM_2D, 0, 0, 0,
                                              1, SPV_IMAGE_FORMAT_UNKNOWN);
    g->tex_image[1] = g->tex_image[2] = spv_emit_type_image(s, f, SPV_DIM_2D, 0, 0, 0, 1, SPV_IMAGE_FORMAT_UNKNOWN);
    for (unsigned k = 0; k < 3; ++k) {
        g->tex_type[k] = spv_emit_type_sampled_image(s, g->tex_image[k]);
        uint32_t type = k == 0 && type0 == SNX_CTR_FS_TEX_SHADOW_CUBE ? spv_emit_type_array(s, g->tex_type[0], 6)
                                                                      : g->tex_type[k];
        g->tex[k] = spv_emit_resource(s, SPV_STORAGE_UNIFORM_CONSTANT, type, 1, k);
    }
    if (c->shadow_rendering) {
        g->shadow_image = spv_emit_type_image(s, g->t_u, SPV_DIM_2D, 0, 0, 0, 2, FORMAT_R32UI);
        g->shadow_buffer = spv_emit_resource(s, SPV_STORAGE_UNIFORM_CONSTANT, g->shadow_image, 2, 0);
    }
}

/* FragmentModule::Generate */
void snx_ctr_fs_spirv(const SnxCtrFsConfig* c, bool preserve_infnan, uint32_t** words, size_t* count) {
    Gen gen = {0}, *g = &gen;
    SpvEmitter* s = &g->s;
    g->c = c;
    spv_emit_init(s, 0);
    s->version = 0x00010300; /* SPIR-V 1.3 */
    spv_emit_glsl450_import(s);
    spv_emit_shader_capability(s);
    spv_emit_capability(s, SPV_CAPABILITY_SAMPLED_BUFFER);
    spv_emit_capability(s, SPV_CAPABILITY_IMAGE_QUERY);
    if (preserve_infnan) {
        spv_emit_extension(s, "SPV_KHR_float_controls");
        spv_emit_capability(s, SPV_CAPABILITY_SIGNED_ZERO_INF_NAN_PRESERVE);
    }
    declare(g);
    for (unsigned i = 0; i < 4; ++i) g->unit[i] = spv_emit_id(s);
    if (c->texture0_type == SNX_CTR_FS_TEX_SHADOW_2D) {
        g->shadow_2d = spv_emit_id(s);
        shadow_2d_function(g);
    }
    for (unsigned i = 0; i < 4; ++i) unit_function(g, i);
    uint32_t main_id = spv_emit_id(s);
    main_function(g, main_id);
    spv_emit_entry_str(s, SPV_STAGE_FRAGMENT, main_id, "main", s->interface, s->interface_count);
    spv_emit_execution_mode(s, main_id, SPV_MODE_ORIGIN_UPPER_LEFT, 0, 0);
    if (g->frag_depth) spv_emit_execution_mode(s, main_id, SPV_MODE_DEPTH_REPLACING, 0, 0);
    if (preserve_infnan) spv_emit_execution_mode(s, main_id, SPV_MODE_SIGNED_ZERO_INF_NAN_PRESERVE, 1, 32);
    *words = spv_emit_finish(s, count);
    spv_emit_destroy(s);
}

/* ---- uniforms (rasterizer_accelerated.cpp SyncDrawUniforms) ---- */

static float f16(uint32_t raw) { return snx_ctr_pica_float(raw & 0xffff, 10, 5); }
static float f20(uint32_t raw) { return snx_ctr_pica_float(raw & 0xfffff, 12, 7); }

static void rgba8(uint32_t c, float out[4]) {
    for (unsigned i = 0; i < 4; ++i) out[i] = (float)snx_bits(c, i * 8, 8) / 255.0f;
}
/* LightColor: 10-bit components, b g r from bit 0 */
static void light_color(uint32_t c, float out[3]) {
    out[0] = (float)snx_bits(c, 20, 10) / 255.0f;
    out[1] = (float)snx_bits(c, 10, 10) / 255.0f;
    out[2] = (float)snx_bits(c, 0, 10) / 255.0f;
}

void snx_ctr_fs_uniforms(const SnxCtrPica* p, SnxCtrFsUniforms* u) {
    const uint32_t* r = p->regs;
    memset(u, 0, sizeof(*u));
    u->framebuffer_scale = 1;
    u->depth_scale = snx_ctr_f24(r[DEPTH_RANGE] & 0xffffff);
    u->depth_offset = snx_ctr_f24(r[DEPTH_NEAR] & 0xffffff);
    u->alphatest_ref = (int32_t)snx_bits(r[OM_ALPHA_TEST], 8, 8);
    rgba8(r[OM_BLEND_CONST], u->blend_color);
    /* FramebufferHelper's scissor before the resolution scale, the target's offset and the flip */
    u->scissor_x1 = (int32_t)snx_bits(r[SCISSOR_1], 0, 10);
    u->scissor_y1 = (int32_t)snx_bits(r[SCISSOR_1], 16, 10);
    u->scissor_x2 = (int32_t)snx_bits(r[SCISSOR_2], 0, 10) + 1;
    u->scissor_y2 = (int32_t)snx_bits(r[SCISSOR_2], 16, 10) + 1;
    for (unsigned i = 0; i < 3; ++i) {
        u->tex_lod_bias[i] = (float)sbits(r[tex_config[i] + 3], 0, 13) / 256.0f;
        rgba8(r[tex_config[i]], u->tex_border_color[i]);
    }
    for (unsigned i = 0; i < 6; ++i) rgba8(r[tev_stage[i] + 3], u->const_color[i]);
    rgba8(r[TEV_BUFFER_COLOR], u->tev_combiner_buffer_color);
    light_color(r[GLOBAL_AMBIENT], u->lighting_global_ambient);
    for (unsigned i = 0; i < 8; ++i) {
        const uint32_t* l = &r[LIGHT + i * 16];
        SnxCtrFsLight* s = &u->light_src[i];
        light_color(l[0], s->specular_0);
        light_color(l[1], s->specular_1);
        light_color(l[2], s->diffuse);
        light_color(l[3], s->ambient);
        s->position[0] = f16(l[4]);
        s->position[1] = f16(l[4] >> 16);
        s->position[2] = f16(l[5]);
        s->spot_direction[0] = (float)sbits(l[6], 0, 13) / 2047.0f;
        s->spot_direction[1] = (float)sbits(l[6], 16, 13) / 2047.0f;
        s->spot_direction[2] = (float)sbits(l[7], 0, 13) / 2047.0f;
        s->dist_atten_bias = f20(l[10]);
        s->dist_atten_scale = f20(l[11]);
    }
    for (unsigned k = 0; k < 3; ++k) u->fog_color[k] = (float)snx_bits(r[FOG_COLOR], k * 8, 8) / 255.0f;
    u->proctex_noise_f[0] = f16(r[PROCTEX_NOISE_FREQUENCY]);
    u->proctex_noise_f[1] = f16(r[PROCTEX_NOISE_FREQUENCY] >> 16);
    u->proctex_noise_a[0] = (float)sbits(r[PROCTEX_NOISE_U], 0, 16) / 4095.0f;
    u->proctex_noise_a[1] = (float)sbits(r[PROCTEX_NOISE_V], 0, 16) / 4095.0f;
    u->proctex_noise_p[0] = f16(r[PROCTEX_NOISE_U] >> 16);
    u->proctex_noise_p[1] = f16(r[PROCTEX_NOISE_V] >> 16);
    u->proctex_bias = f16(snx_bits(r[PROCTEX], 20, 8) | snx_bits(r[PROCTEX_LUT], 19, 8) << 8);
    u->shadow_bias_constant = f16(r[FB_SHADOW]);
    u->shadow_bias_linear = f16(r[FB_SHADOW] >> 16);
    u->shadow_texture_bias = (int32_t)(snx_bits(r[TEX_SHADOW], 1, 23) << 1);
    /* where snx_ctr_fs_luts puts the tables */
    for (unsigned i = 0; i < 24; ++i) u->lighting_lut_offset[i / 4][i % 4] = (int32_t)(i * 256);
    u->fog_lut_offset = LF_FOG;
    u->proctex_noise_lut_offset = RG_NOISE;
    u->proctex_color_map_offset = RG_COLOR_MAP;
    u->proctex_alpha_map_offset = RG_ALPHA_MAP;
    u->proctex_lut_offset = RGBA_COLOR;
    u->proctex_diff_lut_offset = RGBA_DIFF;
}

/* ---- LUTs (vk_rasterizer.cpp SyncAndUploadLUTs, SyncAndUploadLUTsLF) ---- */

void snx_ctr_fs_luts(const SnxCtrPica* p, float (*lf)[2], float (*rg)[2], float (*rgba)[4]) {
    /* lighting: a 0.0.12 value, a 0.0.11 difference with its sign at bit 23 */
    for (unsigned t = 0; t < 24; ++t)
        for (unsigned i = 0; i < 256; ++i) {
            uint32_t e = p->lighting_lut[t][i];
            float diff = (float)snx_bits(e, 12, 11) / 2047.0f;
            lf[t * 256 + i][0] = (float)snx_bits(e, 0, 12) / 4095.0f;
            lf[t * 256 + i][1] = snx_bits(e, 23, 1) ? -diff : diff;
        }
    /* fog: a 0.0.11 value at bit 13, a 1.1.11 difference below it */
    for (unsigned i = 0; i < 128; ++i) {
        uint32_t e = p->fog_lut[i];
        lf[LF_FOG + i][0] = (float)snx_bits(e, 13, 11) / 2047.0f;
        lf[LF_FOG + i][1] = (float)sbits(e, 0, 13) / 2047.0f;
    }
    /* procedural value LUTs: a 0.0.12 value, a signed 0.0.12 difference */
    const uint32_t* const values[3] = {p->proctex_noise, p->proctex_color_map, p->proctex_alpha_map};
    for (unsigned t = 0; t < 3; ++t)
        for (unsigned i = 0; i < 128; ++i) {
            uint32_t e = values[t][i];
            rg[t * 128 + i][0] = (float)snx_bits(e, 0, 12) / 4095.0f;
            rg[t * 128 + i][1] = (float)sbits(e, 12, 12) / 4095.0f;
        }
    /* procedural colours, r g b a from bit 0, and their differences, halved signed bytes */
    for (unsigned i = 0; i < 256; ++i)
        for (unsigned k = 0; k < 4; ++k) {
            rgba[RGBA_COLOR + i][k] = (float)snx_bits(p->proctex_color[i], k * 8, 8) / 255.0f;
            rgba[RGBA_DIFF + i][k] = (float)(sbits(p->proctex_color_diff[i], k * 8, 8) * 2) / 255.0f;
        }
}
