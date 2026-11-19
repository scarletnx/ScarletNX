/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The console's GL programs as the renderer's host programs (src/nx/gpu/gl/gl_host_program.c), with no GPU: a program
   linked in the console's layout described as a translation's ShaderInfo: the vertex stage's attributes, the
   fragment stage's output map, the constant buffers of the default block and of a uniform block, and the texture
   slots of a 2D sampler, a shadow array and an unsigned integer one */
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_state.h"
#include "nx/gpu/gl/gl_host_program.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* shader.c's: each stage's set as the renderer's translations have it (the fragment stage's 1, the others 0), its
   textures' five after it with split_sets, and the rules the renderer sets */
ShCaps g_spirv_translate = {.uniform_cbufs = 18};
int shader_vk_set(int stage) { return stage == ST_FS; }
int shader_vk_tex_set(int stage) { return shader_vk_set(stage) + (g_spirv_translate.split_sets ? 5 : 0); }

static uint32_t shader(uint32_t ctx, uint32_t type, const char* text) {
    uint32_t s = snx_gl_create_shader(ctx, type);
    char* to = snx_gl_shader_source(ctx, s, strlen(text));
    memcpy(to, text, strlen(text));
    snx_gl_compile_shader(ctx, s);
    return s;
}

int main(void) {
    static const char vs[] = "#version 300 es\nlayout(location = 3) in vec4 p;\nlayout(location = 5) in mat2 m;\n"
                             "uniform vec4 shift;\nvoid main() { gl_Position = p + vec4(m[0], m[1]) + shift; }\n";
    static const char fs[] = "#version 300 es\nprecision mediump float;\nuniform sampler2D a;\n"
                             "uniform mediump sampler2DArrayShadow b;\nuniform highp usampler2D c;\n"
                             "uniform Light { vec4 tint; vec4 more[3]; };\n"
                             "layout(location = 0) out vec4 o;\nlayout(location = 2) out vec3 q;\n"
                             "void main() { o = texture(a, vec2(0)) * tint + vec4(texture(b, vec4(0)));\n"
                             "  q = vec3(texture(c, vec2(0)).xyz) + more[2].xyz; }\n";
    uint32_t ctx = snx_gl_context_new(SNX_GL_API_ES, 30, 0), p;
    SnxGlLayout layout = gl_host_program_layout();
    ShaderInfo v, f;
    const SnxGlProgram* pr;
    assert(ctx && layout.set[0] == 0 && layout.set[1] == 1 && layout.block_base == SNX_VKB_CB);
    assert(layout.blocks == GL_HOST_TEX_BANK && layout.sampler_base == SNX_VKB_TEX && layout.samplers == SH_MAXTEX);
    snx_gl_program_host(&(SnxGlProgramHost){.stage_sets = 1, .layout = layout});
    p = snx_gl_create_program(ctx);
    snx_gl_attach_shader(ctx, p, shader(ctx, 0x8b31, vs));
    snx_gl_attach_shader(ctx, p, shader(ctx, 0x8b30, fs));
    snx_gl_link_program(ctx, p);
    pr = snx_gl_program(p);
    if (!pr->linked) {
        fprintf(stderr, "gl_host_program: %s\n", snx_gl_program_info_log(ctx, p));
        return 1;
    }
    assert(!gl_host_shader_info(pr, &layout, 0, &v) && !gl_host_shader_info(pr, &layout, 1, &f));
    assert(gl_host_shader_info(pr, &layout, 2, &f) == -1 && !gl_host_shader_info(pr, &layout, 1, &f));
    /* the vertex stage: attribute 3, and a matrix's two columns at 5 and 6; its default block at constant buffer 0 */
    assert(v.stage == ST_VS && v.in_mask == (1u << 3 | 1u << 5 | 1u << 6) && v.cbuf_mask == 1u && v.cbuf_max[0] == 16);
    assert(!v.ntex);
    /* the fragment stage: a vec4 at target 0, a vec3 at target 2; the block at constant buffer 1, its 64 bytes */
    assert(f.stage == ST_FS && f.sph[18] == (0xfu | 0x7u << 8));
    assert(f.cbuf_mask == 2u && f.cbuf_max[1] == 64);
    /* three texture slots, each handle at its slot's word: 2D, a shadow array, an unsigned integer 2D */
    assert(f.ntex == 3);
    for (int k = 0; k < 3; k++) assert(f.tex[k].cbuf_off == (u32)k && f.tex[k].type == 1);
    assert(!f.tex[0].array && !f.tex[0].shadow && f.tex[1].array && f.tex[1].shadow && !f.tex[2].shadow);
    assert(f.tex_int_mask == 4u && !f.tex_sint_mask);
    assert(!v.cb_window && !f.cb_window && layout.uniform_blocks == GL_HOST_TEX_BANK);
    assert(layout.sampler_set[1] == 1 && !layout.window_push[0] && !layout.window_push[1]);
    /* a device of split sets, constant buffer windows and 2 uniform buffers a stage: the samplers' sets five after
       the stages', the windows' offsets where the translations' are, the blocks from 2 on storage buffers, and each
       stage described as reading through windows */
    g_spirv_translate.split_sets = 1, g_spirv_translate.cb_windows = 1, g_spirv_translate.uniform_cbufs = 2;
    layout = gl_host_program_layout();
    assert(layout.sampler_set[0] == 5 && layout.sampler_set[1] == 6 && layout.uniform_blocks == 2);
    assert(layout.window_push[0] == SH_PUSH_CB_WINDOW && layout.window_push[1] == SH_PUSH_CB_WINDOW_FS);
    assert(!layout.window_push[2]);
    snx_gl_program_host(&(SnxGlProgramHost){.stage_sets = 1, .layout = layout});
    snx_gl_link_program(ctx, p);
    pr = snx_gl_program(p);
    assert(pr->linked && !gl_host_shader_info(pr, &layout, 0, &v) && !gl_host_shader_info(pr, &layout, 1, &f));
    assert(v.cb_window && f.cb_window && f.cbuf_mask == 2u && f.ntex == 3);
    puts("gl_host_program: ok");
    return 0;
}
