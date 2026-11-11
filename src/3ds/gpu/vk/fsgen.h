/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's fragment pipeline as a generated SPIR-V 1.3 fragment shader for Vulkan (fsgen.c, ported from
   Azahar's GLSL generator).

   The shader's interface:
     inputs (the vertex shader's outputs, Azahar's GetVertexInterfaceDeclaration):
       location 0 vec4 primary_color, 1 vec2 texcoord0, 2 vec2 texcoord1, 3 vec2 texcoord2, 4 float texcoord0_w,
       5 vec4 normquat, 6 vec3 view; all smoothly interpolated. normquat comes flipped per vertex against the first
       vertex of its triangle when the two are opposite (dot < 0), as Azahar's AddTriangle does
     outputs: location 0 vec4 color, and FragDepth (the PICA depth, below, with DepthReplacing), except in shadow
       rendering, which writes neither, and with the gas fog mode, which writes transparent black and no depth.
       FragDepth is declared only when it is written. The origin is upper left
     set 0 binding 2: uniform block fs_data, std140, laid out as SnxCtrFsUniforms (0x530 bytes)
     set 0 binding 3: samplerBuffer texture_buffer_lut_lf (RG32F, SNX_CTR_FS_LUT_LF elements)
     set 0 binding 4: samplerBuffer texture_buffer_lut_rg (RG32F, SNX_CTR_FS_LUT_RG elements)
     set 0 binding 5: samplerBuffer texture_buffer_lut_rgba (RGBA32F, SNX_CTR_FS_LUT_RGBA elements)
       (bindings 0 and 1 of set 0 are the vertex shader's)
     set 1 binding 0: tex0, its type by texture0_type as Azahar's Vulkan renderer binds it: sampler2D (2D,
       projection, disabled and unknown types), samplerCube (cube), usampler2D (shadow 2D: an R32UI image of the
       shadow map's words, depth << 8 | stencil), usampler2D[6] (shadow cube: the six faces +X -X +Y -Y +Z -Z)
     set 1 binding 1: sampler2D tex1; set 1 binding 2: sampler2D tex2
     set 2 binding 0: r32ui uimage2D shadow_buffer, declared only in shadow rendering (shadow_rendering), read and
       written with an atomic compare-exchange at FragCoord.xy
   Logic ops and blending are the pipeline's (the generator assumes logicOp and min/max blending with their factors
   ignored, as a desktop Vulkan device has). ClampToBorder is emulated in the shader with tex_border_color, so a
   texture with it may be sampled clamped to edge. Depth: the vertex shader writes -z (PICA's [-1, 0] clip depth,
   negated) to a [0, 1] depth range; the shader takes z_over_w = -FragCoord.z and writes
   z_over_w * depth_scale + depth_offset, divided by FragCoord.w in W-buffering (depthmap_enable 0).
   The types above are GLSL's names: a samplerBuffer is a float Buffer image (SampledBuffer) read with
   OpImageFetch, a usampler2D a uint 2D one */
#ifndef SNX_CTR_FSGEN_H
#define SNX_CTR_FSGEN_H
#include "3ds/gpu/pica.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* TexturingRegs::TextureConfig::TextureType */
enum {
    SNX_CTR_FS_TEX_2D = 0,
    SNX_CTR_FS_TEX_CUBE = 1,
    SNX_CTR_FS_TEX_SHADOW_2D = 2,
    SNX_CTR_FS_TEX_PROJECTION = 3,
    SNX_CTR_FS_TEX_SHADOW_CUBE = 4,
    SNX_CTR_FS_TEX_DISABLED = 5
};

/* Azahar's FSConfig: the fragment pipeline state that picks a shader. Plain data, compared with memcmp and hashed
   bytewise: snx_ctr_fs_config zeroes it entirely (padding too) before filling it. Only what changes the generated
   shader is kept: Azahar's blend and logic-op fields drop out with a device that does both in the pipeline */
typedef struct {
    uint32_t tev_sources[6], tev_modifiers[6], tev_ops[6], tev_scales[6]; /* Dot3_RGBA stages keep the colour part */
    /* framebuffer */
    uint8_t alpha_test_func; /* CompareFunc, Always when the test is off */
    uint8_t scissor_mode;    /* 0 off, 3 keeps the inside, others keep the outside */
    uint8_t depthmap_enable; /* DepthBuffering: 0 W-buffering, 1 Z-buffering */
    uint8_t shadow_rendering;
    /* texturing */
    uint8_t texture0_type; /* SNX_CTR_FS_TEX_* (3 bits) */
    uint8_t texture2_use_coord1;
    uint8_t combiner_buffer_input; /* update_mask_rgb | update_mask_a << 4 */
    uint8_t fog_mode;              /* 5 fog, 7 gas, others none */
    uint8_t fog_flip;
    uint8_t shadow_texture_orthographic;
    uint8_t border_s[3], border_t[3]; /* ClampToBorder in s, t: emulated in the shader */
    /* lighting (all zero when it is off) */
    struct {
        uint8_t enable, src_num, bump_mode, bump_selector, bump_renorm, clamp_highlights, config;
        uint8_t enable_primary_alpha, enable_secondary_alpha;
        uint8_t enable_shadow, shadow_primary, shadow_secondary, shadow_invert, shadow_alpha, shadow_selector;
        uint8_t padding;
        /* d0, d1, sp, fr, rr, rg, rb: enabled, absolute input, LightingLutInput, LightingScale selector */
        struct {
            uint8_t enable, abs_input, type, scale;
        } lut[7];
        /* each slot below src_num: the light it uses and that light's switches */
        struct {
            uint8_t num, directional, two_sided_diffuse, dist_atten, spot_atten, geometric_factor_0,
                geometric_factor_1, shadow;
        } lights[8];
    } lighting;
    /* procedural texture (all zero when it is off) */
    struct {
        uint8_t enable, coord, u_clamp, v_clamp, color_combiner, alpha_combiner, lut_filter, separate_alpha;
        uint8_t noise_enable, u_shift, v_shift, lut_width, lod_min, lod_max;
        uint8_t lut_offset[4];
        uint8_t padding[2];
    } proctex;
} SnxCtrFsConfig;

void snx_ctr_fs_config(const SnxCtrPica* pica, SnxCtrFsConfig* config);
/* the fragment shader's SPIR-V module (the caller frees *words). preserve_infnan declares SPV_KHR_float_controls'
   SignedZeroInfNanPreserve for 32-bit floats */
void snx_ctr_fs_spirv(const SnxCtrFsConfig* config, bool preserve_infnan, uint32_t** words, size_t* count);

/* The uniform block the shader declares (Azahar FSUniformData, std140), filled from the registers. The scissor is
   the guest framebuffer's, in its pixels (x1, y1, x2 + 1, y2 + 1, y counted from the register's origin): the caller
   moves it into FragCoord's space (resolution scale, a flipped framebuffer, the target's place in its image), as
   Azahar's FramebufferHelper does. framebuffer_scale is 1 */
typedef struct {
    float specular_0[3], padding0;
    float specular_1[3], padding1;
    float diffuse[3], padding2;
    float ambient[3], padding3;
    float position[3], padding4;
    float spot_direction[3]; /* negated */
    float dist_atten_bias;
    float dist_atten_scale, padding5[3];
} SnxCtrFsLight;
typedef struct {
    int32_t framebuffer_scale, alphatest_ref;
    float depth_scale, depth_offset, shadow_bias_constant, shadow_bias_linear;
    int32_t scissor_x1, scissor_y1, scissor_x2, scissor_y2;
    int32_t fog_lut_offset, proctex_noise_lut_offset, proctex_color_map_offset, proctex_alpha_map_offset;
    int32_t proctex_lut_offset, proctex_diff_lut_offset;
    float proctex_bias;
    int32_t shadow_texture_bias;
    int32_t padding0[2];
    int32_t lighting_lut_offset[6][4]; /* LUT i at [i / 4][i % 4], in lf elements */
    float fog_color[3], padding1;
    float proctex_noise_f[2], proctex_noise_a[2], proctex_noise_p[2], padding2[2];
    float lighting_global_ambient[3], padding3;
    SnxCtrFsLight light_src[8];
    float const_color[6][4];
    float tev_combiner_buffer_color[4];
    float tex_lod_bias[3], padding4;
    float tex_border_color[3][4];
    float blend_color[4];
} SnxCtrFsUniforms;
void snx_ctr_fs_uniforms(const SnxCtrPica* pica, SnxCtrFsUniforms* uniforms);

/* The three LUT texel buffers, as Azahar packs them (value and difference pairs, or colours): lf holds the 24
   lighting LUTs (256 each, LUT i from i * 256) then the fog LUT (128, from 6144); rg the procedural noise, colour
   map and alpha map (128 each, from 0, 128, 256); rgba the procedural colour LUT (256, from 0) then its differences
   (256, from 256). The offsets go into the uniforms by snx_ctr_fs_uniforms */
#define SNX_CTR_FS_LUT_LF (24 * 256 + 128) /* in vec2 elements */
#define SNX_CTR_FS_LUT_RG (3 * 128)        /* in vec2 elements */
#define SNX_CTR_FS_LUT_RGBA (2 * 256)      /* in vec4 elements */
void snx_ctr_fs_luts(const SnxCtrPica* pica, float (*lf)[2], float (*rg)[2], float (*rgba)[4]);
#endif
