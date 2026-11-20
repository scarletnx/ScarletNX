/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's programs as host programs (gl_host_program.h) */
#include "nx/gpu/gl/gl_host_program.h"
#include <string.h>

enum {
    GL_SAMPLER_2D_SHADOW_ = 0x8b62,
    GL_SAMPLER_2D_ARRAY_SHADOW_ = 0x8dc4,
    GL_SAMPLER_CUBE_SHADOW_ = 0x8dc5,
    GL_INT_SAMPLER_1D_ = 0x8dc9, /* the signed ones: from it to GL_INT_SAMPLER_BUFFER */
    GL_INT_SAMPLER_BUFFER_ = 0x8dd0,
    GL_UNSIGNED_INT_SAMPLER_1D_ = 0x8dd1, /* the unsigned ones: from it to GL_UNSIGNED_INT_SAMPLER_BUFFER */
    GL_UNSIGNED_INT_SAMPLER_BUFFER_ = 0x8dd8,
    GL_INT_SAMPLER_2D_MULTISAMPLE_ = 0x9109,
    GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_ = 0x910a,
};

/* The renderer's rules, as its translations of the console's programs follow them (g_spirv_translate, which it sets
   at its start): each stage's sets, the constant buffers bound as storage buffers, and the vertex and fragment stages'
   windows with their offsets at SH_PUSH_CB_WINDOW and SH_PUSH_CB_WINDOW_FS */
SnxGlLayout gl_host_program_layout(void) {
    const u32 windows = g_spirv_translate.cb_windows;
    return (SnxGlLayout){
        .set = {(uint32_t)shader_vk_set(ST_VS), (uint32_t)shader_vk_set(ST_FS), (uint32_t)shader_vk_set(ST_CS)},
        .block_base = SNX_VKB_CB,
        .blocks = GL_HOST_TEX_BANK,
        .sampler_base = SNX_VKB_TEX,
        .samplers = SH_MAXTEX,
        .sampler_set = {(uint32_t)shader_vk_tex_set(ST_VS), (uint32_t)shader_vk_tex_set(ST_FS),
                        (uint32_t)shader_vk_tex_set(ST_CS)},
        .uniform_blocks =
            g_spirv_translate.uniform_cbufs < GL_HOST_TEX_BANK ? g_spirv_translate.uniform_cbufs : GL_HOST_TEX_BANK,
        .window_push = {windows ? SH_PUSH_CB_WINDOW : 0u, windows ? SH_PUSH_CB_WINDOW_FS : 0u, 0}};
}

/* the components of a fragment output's GL type: a scalar's 1, a vector's its count */
static u32 components(u32 type) {
    switch (type) {
        case 0x8b50:
        case 0x8b53:
        case 0x8dc6: return 2; /* vec2, ivec2, uvec2 */
        case 0x8b51:
        case 0x8b54:
        case 0x8dc7: return 3;
        case 0x8b52:
        case 0x8b55:
        case 0x8dc8: return 4;
        default: return 1;
    }
}
/* the GL type of the sampler whose first unit is unit */
static u32 sampler_type(const SnxGlProgram* p, int32_t unit) {
    const SnxGlUniform* u = snx_gl_program_uniforms(p);
    for (u32 i = 0; i < p->uniform_count; i++)
        if (u[i].sampler == unit) return u[i].type;
    return 0;
}

int gl_host_shader_info(const SnxGlProgram* p, const SnxGlLayout* y, int stage, ShaderInfo* si) {
    const SnxGlBinding* b = snx_gl_program_bindings(p);
    *si = (ShaderInfo){.stage = stage ? ST_FS : ST_VS};
    if (stage < 0 || stage > 1 || !p->linked || !p->spirv_words[stage]) return -1;
    if (stage == 0) {
        const SnxGlInput* in = snx_gl_program_inputs(p);
        for (u32 i = 0; i < p->input_count; i++)
            for (u32 k = 0; k < (u32)in[i].count * in[i].columns; k++)
                if (in[i].spirv >= 0 && (u32)in[i].spirv + k < 32) si->in_mask |= 1u << ((u32)in[i].spirv + k);
    } else {
        const SnxGlInput* out = snx_gl_program_outputs(p);
        for (u32 i = 0; i < p->output_count; i++)
            for (u32 k = 0; k < out[i].count; k++) {
                u32 at = (u32)out[i].spirv + k;
                if (out[i].spirv >= 0 && at < DS_TARGETS_MAX)
                    si->sph[18] |= ((1u << components(out[i].type)) - 1) << 4 * at;
            }
    }
    if (p->default_size[stage]) {
        si->cbuf_mask |= 1u;
        si->cbuf_max[0] = (p->default_size[stage] + 15) & ~15u;
    }
    /* its uniform blocks read through windows, the offsets the renderer pushes (render.c cb_window_place) */
    si->cb_window = y->window_push[stage] != 0;
    for (u32 i = 0; i < p->binding_count; i++) {
        const SnxGlBinding* x = &b[i];
        if (!(x->stages >> stage & 1)) continue;
        if (x->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && x->name && x->binding > y->block_base &&
            x->binding < y->block_base + y->blocks) {
            u32 slot = x->binding - y->block_base;
            si->cbuf_mask |= 1u << slot;
            si->cbuf_max[slot] = (x->size + 15) & ~15u;
        } else if (x->kind == SNX_GLSLC_REFLECT_SAMPLER && x->binding >= y->sampler_base &&
                   x->binding < y->sampler_base + y->samplers) {
            u32 k = x->binding - y->sampler_base;
            u32 type = sampler_type(p, x->first);
            ShTex* t = &si->tex[k];
            *t = (ShTex){.cbuf_off = k,
                         .type = x->dim == SNX_GL_T3D     ? 2
                                 : x->dim == SNX_GL_TCUBE ? 3
                                                          : 1,
                         .array = x->dim == SNX_GL_TARRAY,
                         .shadow = type == GL_SAMPLER_2D_SHADOW_ || type == GL_SAMPLER_2D_ARRAY_SHADOW_ ||
                                   type == GL_SAMPLER_CUBE_SHADOW_,
                         .flags = x->dim == SNX_GL_TMS ? 2 : 0};
            int sint = (type >= GL_INT_SAMPLER_1D_ && type <= GL_INT_SAMPLER_BUFFER_) ||
                       type == GL_INT_SAMPLER_2D_MULTISAMPLE_;
            int uint = (type >= GL_UNSIGNED_INT_SAMPLER_1D_ && type <= GL_UNSIGNED_INT_SAMPLER_BUFFER_) ||
                       type == GL_UNSIGNED_INT_SAMPLER_2D_MULTISAMPLE_;
            if (sint || uint) si->tex_int_mask |= 1ull << k;
            if (sint) si->tex_sint_mask |= 1ull << k;
            if ((int)k + 1 > si->ntex) si->ntex = (int)k + 1;
        }
    }
    return 0;
}
