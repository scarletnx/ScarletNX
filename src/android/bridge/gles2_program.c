/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 2 and 3's shaders and programs (gles2.h): the shared GL layer's (common/gpu/gl/gl_program.h), called with the
   guest's arguments, and each program's Vulkan objects. Those are made from the program's link at its first draw (its
   modules from the link's SPIR-V, its set and pipeline layouts from its bindings), kept by name, and go when the
   layer says the link is gone; each stage's default block goes into the frame's arena when the layer counted a write
   to it since, or the frame is a new one (gles2_program_defaults) */
#include "android/bridge/gles2.h"
#include "android/bridge/hle_impl.h"
#include "common/gpu/gl/gl_program.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define V g_gles_vk

enum {
    GL_INVALID_ENUM_ = 0x0500,
    GL_INVALID_VALUE_ = 0x0501,
    GL_INVALID_OPERATION_ = 0x0502,
};

/* ---- each program's Vulkan objects ---- */

static void program_vk_release(uint32_t name) {
    G2ProgramVk* m = &g_gles2.program_vk[name - 1];
    for (int i = 0; i < 3; ++i)
        if (m->modules[i]) V.vk.vkDestroyShaderModule(V.device, m->modules[i], NULL);
    /* the pipelines made with the layout are retired by their cache when the generation changes */
    if (m->compute) gles_vk_retire((GlesGarbage){.pipeline = m->compute});
    /* the frame being recorded may have used the layouts: they go when it is done */
    if (m->layout || m->set_layout) gles_vk_retire((GlesGarbage){.layout = m->layout, .set_layout = m->set_layout});
    if (m->generation) gles2_pipelines_forget(name);
    *m = (G2ProgramVk){0};
}
static void program_changed(uint32_t name) { program_vk_release(name); }
static void program_say(const char* line) { fprintf(stderr, "scarletnx-android: GLES 2: %s\n", line); }
void gles2_programs_init(void) {
    snx_gl_program_host(&(SnxGlProgramHost){.program_changed = program_changed, .say = program_say});
}
void gles2_programs_vk_free(void) {
    for (uint32_t name = 1; name <= SNX_GL_PROGRAM_NAMES; ++name) program_vk_release(name);
}

/* the program's set: its descriptor set layout and pipeline layout */
static bool program_layouts(const SnxGlProgram* p, G2ProgramVk* m, bool compute) {
    VkDescriptorSetLayoutBinding b[64];
    const SnxGlBinding* bindings = snx_gl_program_bindings(p);
    uint32_t n = 0;
    VkShaderStageFlags stages =
        compute ? VK_SHADER_STAGE_COMPUTE_BIT : VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 0; i < p->binding_count && n < 64; ++i) {
        const SnxGlBinding* x = &bindings[i];
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (x->kind == SNX_GLSLC_REFLECT_SAMPLER)
            type =
                x->texel_buffer ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (x->kind == SNX_GLSLC_REFLECT_IMAGE)
            type = x->texel_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        else if (x->kind == SNX_GLSLC_REFLECT_BUFFER_BLOCK)
            type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[n++] = (VkDescriptorSetLayoutBinding){x->binding, type, x->count, stages, NULL};
    }
    VkDescriptorSetLayoutCreateInfo set = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = b};
    if (V.vk.vkCreateDescriptorSetLayout(V.device, &set, NULL, &m->set_layout) != VK_SUCCESS) return false;
    /* the fragment shader's flip of gl_FragCoord and gl_PointCoord (GlslcOptions.flip_y) */
    VkPushConstantRange flip = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &m->set_layout,
                                         .pushConstantRangeCount = compute ? 0 : 1,
                                         .pPushConstantRanges = &flip};
    return V.vk.vkCreatePipelineLayout(V.device, &layout, NULL, &m->layout) == VK_SUCCESS;
}

G2ProgramVk* gles2_program_vk(uint32_t name) {
    const SnxGlProgram* p = snx_gl_program(name);
    if (!p || !p->linked) return NULL;
    G2ProgramVk* m = &g_gles2.program_vk[name - 1];
    if (m->generation == p->generation) return m;
    program_vk_release(name);
    for (int i = 0; i < 3; ++i) {
        if (!p->spirv_words[i]) continue;
        m->modules[i] = gles_vk_shader(snx_gl_program_spirv(p, i), (size_t)p->spirv_words[i] * 4);
        if (!m->modules[i]) {
            gles2_warn(24, "Vulkan does not take a stage's module of a linked program: it is not drawn");
            program_vk_release(name);
            return NULL;
        }
    }
    if (!program_layouts(p, m, p->spirv_words[2] != 0)) {
        gles2_warn(25, "Vulkan does not make the layouts of a linked program: it is not drawn");
        program_vk_release(name);
        return NULL;
    }
    m->generation = p->generation;
    return m;
}

uint32_t gles2_draw_program(Gles2Context* gl, bool compute) { return snx_gl_draw_program(gl->ctx, compute); }

bool gles2_program_defaults(GlesSlot* slot, uint32_t name) {
    const SnxGlProgram* p = snx_gl_program(name);
    G2ProgramVk* m = &g_gles2.program_vk[name - 1];
    uint64_t pending = gles_vk_pending();
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment;
    if (align < 16) align = 16;
    for (int s = 0; s < 3; ++s) {
        if (!p->default_size[s] || (m->default_uploaded[s] == p->default_serial[s] && m->default_frame[s] == pending))
            continue;
        VkDeviceSize size = (p->default_size[s] + 15) & ~(VkDeviceSize)15;
        VkDeviceSize at = gles_vk_arena_take(slot, &slot->vertices, size, align, 4u << 20, 0);
        if (at == VK_WHOLE_SIZE) return false;
        memcpy(slot->vertices.map + at, snx_gl_program_default(p, s), p->default_size[s]);
        m->default_ref[s] = (G2Ref){slot->vertices.buffer, at};
        m->default_frame[s] = pending;
        m->default_uploaded[s] = p->default_serial[s];
    }
    return true;
}

/* ---- the calls, with the guest's arguments ---- */

static void g_create_shader(CPU* c, Gles2Context* gl) { X(0) = snx_gl_create_shader(gl->ctx, (uint32_t)X(0)); }
static void g_delete_shader(CPU* c, Gles2Context* gl) { snx_gl_delete_shader(gl->ctx, (uint32_t)X(0)); }
static void g_is_shader(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_shader((uint32_t)X(0));
}
/* glShaderSource(shader, count, strings, lengths)'s strings joined into the shader's text */
static void shader_source(Gles2Context* gl, uint32_t shader, int count, const unsigned char* strings,
                          const unsigned char* lengths) {
    size_t total = 0;
    char* to = NULL;
    if (count < 0) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    for (int pass = 0; pass < 2; ++pass) {
        size_t n = 0;
        for (int i = 0; i < count; ++i) {
            uint64_t p;
            memcpy(&p, strings + 8 * (size_t)i, 8);
            int32_t len = -1;
            if (lengths) memcpy(&len, lengths + 4 * (size_t)i, 4);
            const char* text = (const char*)(uintptr_t)p;
            size_t l = !text ? 0 : len < 0 ? strlen(text) : (size_t)len;
            if (pass) memcpy(to + n, text, l);
            n += l;
        }
        if (!pass) {
            total = n;
            to = snx_gl_shader_source(gl->ctx, shader, total);
            if (!to) return;
        }
    }
}
static void g_shader_source(CPU* c, Gles2Context* gl) { shader_source(gl, (uint32_t)X(0), (int)X(1), PTR(2), PTR(3)); }
static void g_compile_shader(CPU* c, Gles2Context* gl) { snx_gl_compile_shader(gl->ctx, (uint32_t)X(0)); }
static void g_get_shaderiv(CPU* c, Gles2Context* gl) {
    int32_t v;
    if (snx_gl_get_shaderiv(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), &v)) memcpy(PTR(2), &v, 4);
}
static void g_get_shader_info_log(CPU* c, Gles2Context* gl) {
    const char* text = snx_gl_shader_info_log(gl->ctx, (uint32_t)X(0));
    if (text) snx_gl_string_out(text, (int32_t)X(1), PTR(2), PTR(3));
}
static void g_get_shader_source(CPU* c, Gles2Context* gl) {
    const char* text = snx_gl_shader_text(gl->ctx, (uint32_t)X(0));
    if (text) snx_gl_string_out(text, (int32_t)X(1), PTR(2), PTR(3));
}
static void g_get_shader_precision_format(CPU* c, Gles2Context* gl) {
    int32_t range[2], bits;
    (void)gl;
    snx_gl_shader_precision_format((uint32_t)X(1), range, &bits);
    memcpy(PTR(2), range, 8);
    memcpy(PTR(3), &bits, 4);
}
static void g_shader_binary(CPU* c, Gles2Context* gl) {
    (void)c;
    gles2_error(gl, GL_INVALID_ENUM_); /* no binary formats */
}

static void g_create_program(CPU* c, Gles2Context* gl) { X(0) = snx_gl_create_program(gl->ctx); }
static void g_delete_program(CPU* c, Gles2Context* gl) { snx_gl_delete_program(gl->ctx, (uint32_t)X(0)); }
static void g_is_program(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_program((uint32_t)X(0));
}
static void g_attach_shader(CPU* c, Gles2Context* gl) { snx_gl_attach_shader(gl->ctx, (uint32_t)X(0), (uint32_t)X(1)); }
static void g_detach_shader(CPU* c, Gles2Context* gl) { snx_gl_detach_shader(gl->ctx, (uint32_t)X(0), (uint32_t)X(1)); }
static void g_get_attached_shaders(CPU* c, Gles2Context* gl) {
    int32_t n = snx_gl_attached_shaders(gl->ctx, (uint32_t)X(0), (int32_t)X(1), PTR(3));
    if (n >= 0 && X(2)) memcpy(PTR(2), &n, 4);
}
static void g_bind_attrib_location(CPU* c, Gles2Context* gl) {
    snx_gl_bind_attrib_location(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), STR(2));
}
static void g_link_program(CPU* c, Gles2Context* gl) { snx_gl_link_program(gl->ctx, (uint32_t)X(0)); }
static void g_use_program(CPU* c, Gles2Context* gl) { snx_gl_use_program(gl->ctx, (uint32_t)X(0)); }
static void g_validate_program(CPU* c, Gles2Context* gl) { snx_gl_validate_program(gl->ctx, (uint32_t)X(0)); }

#define UNIFORM_SCALARS(n, suffix, kind)                                                                               \
    static void values##n##suffix(CPU* c, unsigned first, uint32_t v[4]) {                                             \
        for (unsigned i = 0; i < n; ++i) {                                                                             \
            if (kind == 'f') {                                                                                         \
                float f = hle_f(c, i);                                                                                 \
                memcpy(&v[i], &f, 4);                                                                                  \
            } else {                                                                                                   \
                v[i] = (uint32_t)X(first + i);                                                                         \
            }                                                                                                          \
        }                                                                                                              \
    }                                                                                                                  \
    static void g_uniform##n##suffix(CPU* c, Gles2Context* gl) {                                                       \
        uint32_t v[4];                                                                                                 \
        values##n##suffix(c, 1, v);                                                                                    \
        snx_gl_uniform(gl->ctx, (int32_t)X(0), 1, v, n, kind, 0);                                                      \
    }                                                                                                                  \
    static void g_uniform##n##suffix##v(CPU* c, Gles2Context* gl) {                                                    \
        snx_gl_uniform(gl->ctx, (int32_t)X(0), (int32_t)X(1), PTR(2), n, kind, 0);                                     \
    }                                                                                                                  \
    static void g_program_uniform##n##suffix(CPU* c, Gles2Context* gl) {                                               \
        uint32_t v[4];                                                                                                 \
        values##n##suffix(c, 2, v);                                                                                    \
        snx_gl_program_uniform(gl->ctx, (uint32_t)X(0), (int32_t)X(1), 1, v, n, kind, 0);                              \
    }                                                                                                                  \
    static void g_program_uniform##n##suffix##v(CPU* c, Gles2Context* gl) {                                            \
        snx_gl_program_uniform(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (int32_t)X(2), PTR(3), n, kind, 0);             \
    }
UNIFORM_SCALARS(1, f, 'f')
UNIFORM_SCALARS(2, f, 'f')
UNIFORM_SCALARS(3, f, 'f')
UNIFORM_SCALARS(4, f, 'f')
UNIFORM_SCALARS(1, i, 'i')
UNIFORM_SCALARS(2, i, 'i')
UNIFORM_SCALARS(3, i, 'i')
UNIFORM_SCALARS(4, i, 'i')
UNIFORM_SCALARS(1, ui, 'u')
UNIFORM_SCALARS(2, ui, 'u')
UNIFORM_SCALARS(3, ui, 'u')
UNIFORM_SCALARS(4, ui, 'u')
#undef UNIFORM_SCALARS
#define UNIFORM_MATRIX(name, n)                                                                                        \
    static void g_uniform_matrix##name(CPU* c, Gles2Context* gl) {                                                     \
        snx_gl_uniform(gl->ctx, (int32_t)X(0), (int32_t)X(1), PTR(3), n, 'f', (X(2) & 0xff) != 0);                     \
    }                                                                                                                  \
    static void g_program_uniform_matrix##name(CPU* c, Gles2Context* gl) {                                             \
        snx_gl_program_uniform(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (int32_t)X(2), PTR(4), n, 'f',                  \
                               (X(3) & 0xff) != 0);                                                                    \
    }
UNIFORM_MATRIX(2, 4)
UNIFORM_MATRIX(3, 9)
UNIFORM_MATRIX(4, 16)
UNIFORM_MATRIX(2x3, 6)
UNIFORM_MATRIX(3x2, 6)
UNIFORM_MATRIX(2x4, 8)
UNIFORM_MATRIX(4x2, 8)
UNIFORM_MATRIX(3x4, 12)
UNIFORM_MATRIX(4x3, 12)
#undef UNIFORM_MATRIX

/* glGetUniform*v(program, location, params), and glGetnUniform*v(program, location, size, params) into a buffer of
   size bytes */
static void g_get_uniformfv(CPU* c, Gles2Context* gl) {
    snx_gl_get_uniform(gl->ctx, (uint32_t)X(0), (int32_t)X(1), 'f', PTR(2));
}
static void g_get_uniformiv(CPU* c, Gles2Context* gl) {
    snx_gl_get_uniform(gl->ctx, (uint32_t)X(0), (int32_t)X(1), 'i', PTR(2));
}
static void get_nuniform(CPU* c, Gles2Context* gl, char kind) {
    unsigned char value[64] = {0};
    int32_t size = (int32_t)X(2);
    snx_gl_get_uniform(gl->ctx, (uint32_t)X(0), (int32_t)X(1), kind, value);
    if (size > 0) memcpy(PTR(3), value, (size_t)size < sizeof value ? (size_t)size : sizeof value);
}
static void g_get_nuniformfv(CPU* c, Gles2Context* gl) { get_nuniform(c, gl, 'f'); }
static void g_get_nuniformiv(CPU* c, Gles2Context* gl) { get_nuniform(c, gl, 'i'); }

/* ---- the queries of programs ---- */

static void g_get_programiv(CPU* c, Gles2Context* gl) {
    int32_t v[3];
    int n = snx_gl_get_programiv(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), v);
    if (n) memcpy(PTR(2), v, 4 * (size_t)n);
}
static void g_get_program_info_log(CPU* c, Gles2Context* gl) {
    const char* text = snx_gl_program_info_log(gl->ctx, (uint32_t)X(0));
    if (text) snx_gl_string_out(text, (int32_t)X(1), PTR(2), PTR(3));
}
/* glGetActiveUniform and glGetActiveAttrib(program, index, size, length, count, type, name) */
static void active(CPU* c, Gles2Context* gl, int uniforms) {
    int32_t count;
    uint32_t type;
    const char* name = snx_gl_active(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), uniforms, &count, &type);
    if (!name) return;
    snx_gl_string_out(name, (int32_t)X(2), PTR(3), PTR(6));
    if (X(4)) memcpy(PTR(4), &count, 4);
    if (X(5)) memcpy(PTR(5), &type, 4);
}
static void g_get_active_uniform(CPU* c, Gles2Context* gl) { active(c, gl, 1); }
static void g_get_active_attrib(CPU* c, Gles2Context* gl) { active(c, gl, 0); }
static void g_get_attrib_location(CPU* c, Gles2Context* gl) {
    X(0) = (uint64_t)(int64_t)snx_gl_attrib_location(gl->ctx, (uint32_t)X(0), STR(1));
}
static void g_get_frag_data_location(CPU* c, Gles2Context* gl) {
    X(0) = (uint64_t)(int64_t)snx_gl_frag_data_location(gl->ctx, (uint32_t)X(0), STR(1));
}
static void g_get_uniform_location(CPU* c, Gles2Context* gl) {
    X(0) = (uint64_t)(int64_t)snx_gl_uniform_location(gl->ctx, (uint32_t)X(0), STR(1));
}
static void g_get_uniform_block_index(CPU* c, Gles2Context* gl) {
    X(0) = snx_gl_uniform_block_index(gl->ctx, (uint32_t)X(0), STR(1));
}
static void g_uniform_block_binding(CPU* c, Gles2Context* gl) {
    snx_gl_uniform_block_binding(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2));
}
static void g_get_active_uniform_blockiv(CPU* c, Gles2Context* gl) {
    snx_gl_active_uniform_blockiv(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), PTR(3), INT32_MAX);
}
static void g_get_active_uniform_block_name(CPU* c, Gles2Context* gl) {
    const char* name = snx_gl_active_uniform_block_name(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
    if (name) snx_gl_string_out(name, (int32_t)X(2), PTR(3), PTR(4));
}
/* glGetUniformIndices(program, count, names, indices): a block's member by its name, or by "Block.member" */
static void g_get_uniform_indices(CPU* c, Gles2Context* gl) {
    uint32_t program = (uint32_t)X(0);
    int32_t count = (int32_t)X(1);
    if (!snx_gl_is_program(program)) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    for (int32_t k = 0; k < count; ++k) {
        uint64_t ptr;
        memcpy(&ptr, (unsigned char*)PTR(2) + 8 * k, 8);
        uint32_t found = snx_gl_uniform_index(gl->ctx, program, (const char*)(uintptr_t)ptr);
        memcpy((unsigned char*)PTR(3) + 4 * k, &found, 4);
    }
}
static void g_get_active_uniformsiv(CPU* c, Gles2Context* gl) {
    int32_t count = (int32_t)X(1);
    for (int32_t k = 0; k < count; ++k) {
        uint32_t i;
        int32_t v;
        memcpy(&i, (unsigned char*)PTR(2) + 4 * k, 4);
        if (!snx_gl_active_uniformiv(gl->ctx, (uint32_t)X(0), i, (uint32_t)X(3), &v)) return;
        memcpy((unsigned char*)PTR(4) + 4 * k, &v, 4);
    }
}
static void g_get_program_interfaceiv(CPU* c, Gles2Context* gl) {
    int32_t v;
    if (snx_gl_program_interfaceiv(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), &v)) memcpy(PTR(3), &v, 4);
}
static void g_get_program_resource_index(CPU* c, Gles2Context* gl) {
    X(0) = snx_gl_program_resource_index(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), STR(2));
}
static void g_get_program_resource_name(CPU* c, Gles2Context* gl) {
    const char* name = snx_gl_program_resource_name(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2));
    if (name) snx_gl_string_out(name, (int32_t)X(3), PTR(4), PTR(5));
}
/* glGetProgramResourceiv(program, interface, index, count, props, size, length, params) */
static void g_get_program_resourceiv(CPU* c, Gles2Context* gl) {
    int32_t written = snx_gl_program_resourceiv(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), (int32_t)X(3),
                                                PTR(4), (int32_t)X(5), PTR(7));
    if (X(6)) memcpy(PTR(6), &written, 4);
}
static void g_get_program_resource_location(CPU* c, Gles2Context* gl) {
    X(0) = (uint64_t)(int64_t)snx_gl_program_resource_location(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), STR(2));
}
static void g_program_binary(CPU* c, Gles2Context* gl) { snx_gl_program_binary(gl->ctx, (uint32_t)X(0)); }
static void g_get_program_binary(CPU* c, Gles2Context* gl) {
    int32_t zero = 0;
    if (X(2)) memcpy(PTR(2), &zero, 4);
    gles2_error(gl, GL_INVALID_OPERATION_);
}
static void g_program_nothing(CPU* c, Gles2Context* gl) { (void)c, (void)gl; }
static void g_program_parameteri(CPU* c, Gles2Context* gl) {
    snx_gl_program_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (int32_t)X(2));
}
static void g_transform_feedback_varyings(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl;
    gles2_warn(7, "transform feedback is not drawn yet");
}

/* ---- program pipelines (GLES 3.1) ---- */

static void g_gen_program_pipelines(CPU* c, Gles2Context* gl) { snx_gl_gen_pipelines(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_delete_program_pipelines(CPU* c, Gles2Context* gl) {
    snx_gl_delete_pipelines(gl->ctx, (int32_t)X(0), PTR(1));
}
static void g_is_program_pipeline(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_pipeline((uint32_t)X(0));
}
static void g_bind_program_pipeline(CPU* c, Gles2Context* gl) { snx_gl_bind_pipeline(gl->ctx, (uint32_t)X(0)); }
static void g_use_program_stages(CPU* c, Gles2Context* gl) {
    snx_gl_use_program_stages(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2));
}
static void g_active_shader_program(CPU* c, Gles2Context* gl) {
    snx_gl_active_shader_program(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
static void g_validate_program_pipeline(CPU* c, Gles2Context* gl) { snx_gl_validate_pipeline(gl->ctx, (uint32_t)X(0)); }
static void g_get_program_pipelineiv(CPU* c, Gles2Context* gl) {
    int32_t v;
    if (snx_gl_get_pipelineiv(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), &v)) memcpy(PTR(2), &v, 4);
}
static void g_get_program_pipeline_info_log(CPU* c, Gles2Context* gl) {
    (void)gl;
    snx_gl_string_out("", (int32_t)X(1), PTR(2), PTR(3));
}
/* glCreateShaderProgramv(type, count, strings): a separable program of one stage, its shader kept with it */
static void g_create_shader_program(CPU* c, Gles2Context* gl) {
    uint32_t shader = snx_gl_create_shader(gl->ctx, (uint32_t)X(0));
    X(0) = 0;
    if (!shader) return;
    shader_source(gl, shader, (int)X(1), PTR(2), NULL);
    X(0) = snx_gl_create_shader_program(gl->ctx, shader);
}

const HleSymbol hle_gles2_program[] = {
    FN("glCreateShader", g_create_shader),
    FN("glDeleteShader", g_delete_shader),
    FN("glIsShader", g_is_shader),
    FN("glShaderSource", g_shader_source),
    FN("glCompileShader", g_compile_shader),
    FN("glGetShaderiv", g_get_shaderiv),
    FN("glGetShaderInfoLog", g_get_shader_info_log),
    FN("glGetShaderSource", g_get_shader_source),
    FN("glGetShaderPrecisionFormat", g_get_shader_precision_format),
    FN("glShaderBinary", g_shader_binary),
    FN("glCreateProgram", g_create_program),
    FN("glDeleteProgram", g_delete_program),
    FN("glIsProgram", g_is_program),
    FN("glAttachShader", g_attach_shader),
    FN("glDetachShader", g_detach_shader),
    FN("glGetAttachedShaders", g_get_attached_shaders),
    FN("glBindAttribLocation", g_bind_attrib_location),
    FN("glLinkProgram", g_link_program),
    FN("glUseProgram", g_use_program),
    FN("glValidateProgram", g_validate_program),
    FN("glGetProgramiv", g_get_programiv),
    FN("glGetProgramInfoLog", g_get_program_info_log),
    FN("glGetActiveUniform", g_get_active_uniform),
    FN("glGetActiveAttrib", g_get_active_attrib),
    FN("glGetAttribLocation", g_get_attrib_location),
    FN("glGetFragDataLocation", g_get_frag_data_location),
    FN("glGetUniformLocation", g_get_uniform_location),
    FN("glGetUniformfv", g_get_uniformfv),
    FN("glGetUniformiv", g_get_uniformiv),
    FN("glGetUniformuiv", g_get_uniformiv),
    FN("glGetnUniformfv", g_get_nuniformfv),
    FN("glGetnUniformiv", g_get_nuniformiv),
    FN("glGetnUniformuiv", g_get_nuniformiv),
    FN("glGetnUniformfvEXT", g_get_nuniformfv),
    FN("glGetnUniformivEXT", g_get_nuniformiv),
    FN("glGetUniformBlockIndex", g_get_uniform_block_index),
    FN("glUniformBlockBinding", g_uniform_block_binding),
    FN("glGetActiveUniformBlockiv", g_get_active_uniform_blockiv),
    FN("glGetActiveUniformBlockName", g_get_active_uniform_block_name),
    FN("glGetUniformIndices", g_get_uniform_indices),
    FN("glGetActiveUniformsiv", g_get_active_uniformsiv),
    FN("glGetProgramInterfaceiv", g_get_program_interfaceiv),
    FN("glGetProgramResourceIndex", g_get_program_resource_index),
    FN("glGetProgramResourceName", g_get_program_resource_name),
    FN("glGetProgramResourceiv", g_get_program_resourceiv),
    FN("glGetProgramResourceLocation", g_get_program_resource_location),
    FN("glProgramBinary", g_program_binary),
    FN("glProgramBinaryOES", g_program_binary),
    FN("glGetProgramBinary", g_get_program_binary),
    FN("glGetProgramBinaryOES", g_get_program_binary),
    FN("glProgramParameteri", g_program_parameteri),
    FN("glGenProgramPipelines", g_gen_program_pipelines),
    FN("glDeleteProgramPipelines", g_delete_program_pipelines),
    FN("glIsProgramPipeline", g_is_program_pipeline),
    FN("glBindProgramPipeline", g_bind_program_pipeline),
    FN("glUseProgramStages", g_use_program_stages),
    FN("glActiveShaderProgram", g_active_shader_program),
    FN("glValidateProgramPipeline", g_validate_program_pipeline),
    FN("glGetProgramPipelineiv", g_get_program_pipelineiv),
    FN("glGetProgramPipelineInfoLog", g_get_program_pipeline_info_log),
    FN("glCreateShaderProgramv", g_create_shader_program),
    FN("glTransformFeedbackVaryings", g_transform_feedback_varyings),
    FN("glGetTransformFeedbackVarying", g_program_nothing),
    FN("glUniform1f", g_uniform1f),
    FN("glUniform2f", g_uniform2f),
    FN("glUniform3f", g_uniform3f),
    FN("glUniform4f", g_uniform4f),
    FN("glUniform1fv", g_uniform1fv),
    FN("glUniform2fv", g_uniform2fv),
    FN("glUniform3fv", g_uniform3fv),
    FN("glUniform4fv", g_uniform4fv),
    FN("glUniform1i", g_uniform1i),
    FN("glUniform2i", g_uniform2i),
    FN("glUniform3i", g_uniform3i),
    FN("glUniform4i", g_uniform4i),
    FN("glUniform1iv", g_uniform1iv),
    FN("glUniform2iv", g_uniform2iv),
    FN("glUniform3iv", g_uniform3iv),
    FN("glUniform4iv", g_uniform4iv),
    FN("glUniform1ui", g_uniform1ui),
    FN("glUniform2ui", g_uniform2ui),
    FN("glUniform3ui", g_uniform3ui),
    FN("glUniform4ui", g_uniform4ui),
    FN("glUniform1uiv", g_uniform1uiv),
    FN("glUniform2uiv", g_uniform2uiv),
    FN("glUniform3uiv", g_uniform3uiv),
    FN("glUniform4uiv", g_uniform4uiv),
    FN("glUniformMatrix2fv", g_uniform_matrix2),
    FN("glUniformMatrix3fv", g_uniform_matrix3),
    FN("glUniformMatrix4fv", g_uniform_matrix4),
    FN("glUniformMatrix2x3fv", g_uniform_matrix2x3),
    FN("glUniformMatrix3x2fv", g_uniform_matrix3x2),
    FN("glUniformMatrix2x4fv", g_uniform_matrix2x4),
    FN("glUniformMatrix4x2fv", g_uniform_matrix4x2),
    FN("glUniformMatrix3x4fv", g_uniform_matrix3x4),
    FN("glUniformMatrix4x3fv", g_uniform_matrix4x3),
    FN("glProgramUniform1f", g_program_uniform1f),
    FN("glProgramUniform1fv", g_program_uniform1fv),
    FN("glProgramUniform1i", g_program_uniform1i),
    FN("glProgramUniform1iv", g_program_uniform1iv),
    FN("glProgramUniform1ui", g_program_uniform1ui),
    FN("glProgramUniform1uiv", g_program_uniform1uiv),
    FN("glProgramUniform2f", g_program_uniform2f),
    FN("glProgramUniform2fv", g_program_uniform2fv),
    FN("glProgramUniform2i", g_program_uniform2i),
    FN("glProgramUniform2iv", g_program_uniform2iv),
    FN("glProgramUniform2ui", g_program_uniform2ui),
    FN("glProgramUniform2uiv", g_program_uniform2uiv),
    FN("glProgramUniform3f", g_program_uniform3f),
    FN("glProgramUniform3fv", g_program_uniform3fv),
    FN("glProgramUniform3i", g_program_uniform3i),
    FN("glProgramUniform3iv", g_program_uniform3iv),
    FN("glProgramUniform3ui", g_program_uniform3ui),
    FN("glProgramUniform3uiv", g_program_uniform3uiv),
    FN("glProgramUniform4f", g_program_uniform4f),
    FN("glProgramUniform4fv", g_program_uniform4fv),
    FN("glProgramUniform4i", g_program_uniform4i),
    FN("glProgramUniform4iv", g_program_uniform4iv),
    FN("glProgramUniform4ui", g_program_uniform4ui),
    FN("glProgramUniform4uiv", g_program_uniform4uiv),
    FN("glProgramUniformMatrix2fv", g_program_uniform_matrix2),
    FN("glProgramUniformMatrix3fv", g_program_uniform_matrix3),
    FN("glProgramUniformMatrix4fv", g_program_uniform_matrix4),
    FN("glProgramUniformMatrix2x3fv", g_program_uniform_matrix2x3),
    FN("glProgramUniformMatrix3x2fv", g_program_uniform_matrix3x2),
    FN("glProgramUniformMatrix2x4fv", g_program_uniform_matrix2x4),
    FN("glProgramUniformMatrix4x2fv", g_program_uniform_matrix4x2),
    FN("glProgramUniformMatrix3x4fv", g_program_uniform_matrix3x4),
    FN("glProgramUniformMatrix4x3fv", g_program_uniform_matrix4x3),
    {"", NULL, NULL},
};
