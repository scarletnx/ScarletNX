/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 2 and 3 (gles2.h): the contexts, the fixed state of the pipeline, the queries of state, and the HLE table.
   Each function of the table runs with the calling thread's context and g_gles_vk.lock held (call); without a
   current context it does nothing and returns 0, as GL's calls do */
#include "android/bridge/gles2.h"
#include "android/bridge/hle_impl.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

Gles2 g_gles2;
static _Thread_local struct {
    Gles2Context* gl;
} t_gles2;
#define V g_gles_vk

enum {
    GL_INVALID_ENUM_ = 0x0500,
    GL_INVALID_VALUE_ = 0x0501,
    GL_BACK_ = 0x0405,
    GL_FRONT_AND_BACK_ = 0x0408,
    GL_VENDOR_ = 0x1f00,
    GL_RENDERER_ = 0x1f01,
    GL_VERSION_ = 0x1f02,
    GL_EXTENSIONS_ = 0x1f03,
    GL_SHADING_LANGUAGE_VERSION_ = 0x8b8c,
    GL_COLOR_ATTACHMENT0_ = 0x8ce0,
    GL_UNSIGNED_BYTE_ = 0x1401,
    GL_RGBA_ = 0x1908,
};

Gles2Context* gles2_current(void) {
    return t_gles2.gl;
}

void gles2_error(Gles2Context* gl, unsigned e) {
    if (gl) snx_gl_error(gl->ctx, e);
}

uint64_t gles2_arg(const CPU* c, unsigned n) {
    if (n < 8) return c->x[n];
    uint64_t v;
    memcpy(&v, (const unsigned char*)(uintptr_t)c->x[31] + 8 * (n - 8), sizeof v);
    return v;
}

void gles2_warn(unsigned slot, const char* message) {
    if (slot < sizeof g_gles2.said && g_gles2.said[slot]) return;
    if (slot < sizeof g_gles2.said) g_gles2.said[slot] = true;
    fprintf(stderr, "scarletnx-android: GLES 2: %s\n", message);
}

Gles2Context* gles2_context_new(int version) {
    Gles2Context* gl = calloc(1, sizeof *gl);
    if (!gl) return NULL;
    gl->ctx = snx_gl_context_new(SNX_GL_API_ES, 31, 0); /* a context of version 2 or 3 is one of 3.1 */
    if (!gl->ctx) {
        char why[96];
        free(gl);
        snprintf(why, sizeof why, "no context made: the shared GL layer holds %d at once, all used", SNX_GL_CONTEXTS);
        gles2_warn(19, why);
        return NULL;
    }
    gles2_programs_init();
    gles2_buffers_init();
    gles2_textures_init();
    gles2_framebuffers_init();
    gles2_syncs_init();
    gl->version = version;
    return gl;
}

void gles2_context_free(Gles2Context* gl) {
    if (!gl) return;
    if (t_gles2.gl == gl) t_gles2.gl = NULL;
    snx_gl_context_free(gl->ctx);
    gles_surface_free(gl->own);
    free(gl);
}

void gles2_shutdown(void) {
    pthread_mutex_lock(&V.lock);
    if (V.device) {
        gles2_programs_vk_free();
        gles2_draw_vk_free();
        gles2_objects_vk_free();
        gles1_vk_free();
    }
    gles_vk_shutdown();
    pthread_mutex_unlock(&V.lock);
}

void gles2_make_current(Gles2Context* gl, GlesSurface* draw, GlesSurface* read) {
    Gles2Context* old = t_gles2.gl;
    if (old && old != gl) {
        /* what the old context recorded into a pbuffer of its own goes to the GPU before others' commands */
        pthread_mutex_lock(&V.lock);
        if (old->own) gles_frame_end(old->own);
        pthread_mutex_unlock(&V.lock);
    }
    t_gles2.gl = gl;
    if (!gl) return;
    if (draw && gl->own && gl->own->recording) {
        pthread_mutex_lock(&V.lock);
        gles_frame_end(gl->own);
        pthread_mutex_unlock(&V.lock);
    }
    /* without a surface, the context's own pbuffer, made when a call first records (gles2_surface) */
    gl->draw = draw ? draw : gl->own;
    gl->read = read ? read : gl->draw;
    if (draw && !gl->sized) {
        int w, h;
        gles_surface_size(draw, &w, &h);
        gles2_fixed(gl)->viewport[2] = gles2_fixed(gl)->scissor[2] = w;
        gles2_fixed(gl)->viewport[3] = gles2_fixed(gl)->scissor[3] = h;
        gl->sized = true;
    }
}

GlesSurface* gles2_surface(Gles2Context* gl) {
    if (gl->draw) return gl->draw;
    if (!gl->own) gl->own = gles_pbuffer_make(1, 1);
    gl->draw = gl->own;
    if (!gl->read) gl->read = gl->own;
    return gl->draw;
}

/* ---- the strings ---- */

/* the extensions, as GL_EXTENSIONS and glGetStringi give them */
static const char* const extensions[] = {
    "GL_OES_vertex_array_object", "GL_OES_mapbuffer", "GL_OES_rgb8_rgba8", "GL_OES_depth24",
    "GL_OES_depth_texture", "GL_OES_packed_depth_stencil", "GL_OES_element_index_uint", "GL_OES_texture_npot",
    "GL_OES_standard_derivatives", "GL_OES_texture_half_float", "GL_OES_texture_float",
    "GL_OES_texture_half_float_linear", "GL_OES_texture_float_linear", "GL_OES_surfaceless_context",
    "GL_EXT_texture_format_BGRA8888", "GL_EXT_read_format_bgra", "GL_EXT_discard_framebuffer",
    "GL_EXT_blend_minmax", "GL_EXT_texture_rg", "GL_EXT_shader_texture_lod", "GL_EXT_frag_depth",
    "GL_EXT_texture_filter_anisotropic", "GL_EXT_map_buffer_range", "GL_EXT_texture_storage",
    "GL_EXT_unpack_subimage", "GL_EXT_draw_buffers", "GL_EXT_instanced_arrays", "GL_EXT_draw_instanced",
    "GL_EXT_occlusion_query_boolean", "GL_EXT_multisampled_render_to_texture_disabled",
};
enum { EXTENSIONS = sizeof extensions / sizeof *extensions - 1 }; /* the last is a placeholder, not given */

const char* gles2_string(int version, unsigned name) {
    (void)version; /* a context of version 2 is one of 3.1, as on a phone */
    char* all = g_gles2.extensions;
    switch (name) {
    case GL_VENDOR_: return "ARM";
    case GL_RENDERER_: return hle_gpu_name;
    case GL_VERSION_: return "OpenGL ES 3.1 ScarletNX";
    case GL_SHADING_LANGUAGE_VERSION_: return "OpenGL ES GLSL ES 3.10";
    case GL_EXTENSIONS_:
        if (!all[0]) {
            size_t n = 0;
            for (int i = 0; i < EXTENSIONS; ++i) {
                size_t len = strlen(extensions[i]);
                if (n + len + 2 > sizeof g_gles2.extensions) break;
                memcpy(all + n, extensions[i], len);
                n += len;
                all[n++] = ' ';
            }
            all[n ? n - 1 : 0] = 0;
        }
        return all;
    default: return NULL;
    }
}

/* ---- the calls ---- */

void gles2_call(CPU* c, void* data) {
    Gles2Context* gl = t_gles2.gl;
    if (!gl) {
        c->x[0] = 0;
        return;
    }
    G2Fn fn;
    memcpy(&fn, &data, sizeof fn);
    pthread_mutex_lock(&V.lock);
    fn(c, gl);
    pthread_mutex_unlock(&V.lock);
}

static float F(const CPU* c, unsigned n) {
    return hle_f(c, n);
}

/* the fixed state's calls: the shared GL layer's (common/gpu/gl/gl_state.h), with the guest's arguments */
static void g_get_error(CPU* c, Gles2Context* gl) { X(0) = snx_gl_get_error(gl->ctx); }
static void g_enable(CPU* c, Gles2Context* gl) { snx_gl_enable(gl->ctx, (uint32_t)X(0), 1); }
static void g_disable(CPU* c, Gles2Context* gl) { snx_gl_enable(gl->ctx, (uint32_t)X(0), 0); }
static void g_is_enabled(CPU* c, Gles2Context* gl) { X(0) = (uint64_t)snx_gl_is_enabled(gl->ctx, (uint32_t)X(0)); }
static void g_blend_func(CPU* c, Gles2Context* gl) {
    snx_gl_blend_func_separate(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(0), (uint32_t)X(1));
}
static void g_blend_func_separate(CPU* c, Gles2Context* gl) {
    snx_gl_blend_func_separate(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), (uint32_t)X(3));
}
static void g_blend_equation(CPU* c, Gles2Context* gl) {
    snx_gl_blend_equation_separate(gl->ctx, (uint32_t)X(0), (uint32_t)X(0));
}
static void g_blend_equation_separate(CPU* c, Gles2Context* gl) {
    snx_gl_blend_equation_separate(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
static void g_blend_color(CPU* c, Gles2Context* gl) { snx_gl_blend_color(gl->ctx, F(c, 0), F(c, 1), F(c, 2), F(c, 3)); }
static void g_color_mask(CPU* c, Gles2Context* gl) {
    snx_gl_color_mask(gl->ctx, (X(0) & 0xff) != 0, (X(1) & 0xff) != 0, (X(2) & 0xff) != 0, (X(3) & 0xff) != 0);
}
static void g_depth_mask(CPU* c, Gles2Context* gl) { snx_gl_depth_mask(gl->ctx, (X(0) & 0xff) != 0); }
static void g_depth_func(CPU* c, Gles2Context* gl) { snx_gl_depth_func(gl->ctx, (uint32_t)X(0)); }
static void g_depth_range(CPU* c, Gles2Context* gl) { snx_gl_depth_range(gl->ctx, F(c, 0), F(c, 1)); }
static void g_cull_face(CPU* c, Gles2Context* gl) { snx_gl_cull_face(gl->ctx, (uint32_t)X(0)); }
static void g_front_face(CPU* c, Gles2Context* gl) { snx_gl_front_face(gl->ctx, (uint32_t)X(0)); }
static void g_stencil_func(CPU* c, Gles2Context* gl) {
    snx_gl_stencil_func_separate(gl->ctx, GL_FRONT_AND_BACK_, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2));
}
static void g_stencil_func_separate(CPU* c, Gles2Context* gl) {
    snx_gl_stencil_func_separate(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (int32_t)X(2), (uint32_t)X(3));
}
static void g_stencil_op(CPU* c, Gles2Context* gl) {
    snx_gl_stencil_op_separate(gl->ctx, GL_FRONT_AND_BACK_, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2));
}
static void g_stencil_op_separate(CPU* c, Gles2Context* gl) {
    snx_gl_stencil_op_separate(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), (uint32_t)X(3));
}
static void g_stencil_mask(CPU* c, Gles2Context* gl) {
    snx_gl_stencil_mask_separate(gl->ctx, GL_FRONT_AND_BACK_, (uint32_t)X(0));
}
static void g_stencil_mask_separate(CPU* c, Gles2Context* gl) {
    snx_gl_stencil_mask_separate(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
static void g_clear_color(CPU* c, Gles2Context* gl) { snx_gl_clear_color(gl->ctx, F(c, 0), F(c, 1), F(c, 2), F(c, 3)); }
static void g_clear_depth(CPU* c, Gles2Context* gl) { snx_gl_clear_depth(gl->ctx, F(c, 0)); }
static void g_clear_stencil(CPU* c, Gles2Context* gl) { snx_gl_clear_stencil(gl->ctx, (int32_t)X(0)); }
static void g_viewport(CPU* c, Gles2Context* gl) {
    snx_gl_viewport(gl->ctx, (int32_t)X(0), (int32_t)X(1), (int32_t)X(2), (int32_t)X(3));
}
static void g_scissor(CPU* c, Gles2Context* gl) {
    snx_gl_scissor(gl->ctx, (int32_t)X(0), (int32_t)X(1), (int32_t)X(2), (int32_t)X(3));
}
static void g_line_width(CPU* c, Gles2Context* gl) { snx_gl_line_width(gl->ctx, F(c, 0)); }
static void g_polygon_offset(CPU* c, Gles2Context* gl) { snx_gl_polygon_offset(gl->ctx, F(c, 0), F(c, 1)); }
static void g_sample_coverage(CPU* c, Gles2Context* gl) {
    snx_gl_sample_coverage(gl->ctx, F(c, 0), (X(0) & 0xff) != 0);
}
static void g_hint(CPU* c, Gles2Context* gl) { snx_gl_hint(gl->ctx, (uint32_t)X(0), (uint32_t)X(1)); }
static void g_pixel_store(CPU* c, Gles2Context* gl) { snx_gl_pixel_store(gl->ctx, (uint32_t)X(0), (int32_t)X(1)); }
static void g_flush(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl; /* the queue runs what the frame submits in order: nothing waits */
}
static void g_finish(CPU* c, Gles2Context* gl) {
    (void)c;
    gles2_rendering_stop(gl);
    if (gl->draw) gles_flush(gl->draw);
}

/* ---- the queries of state ---- */

static unsigned binding_of(Gles2Context* gl, unsigned target) {
    const SnxGlTextureContext* x = snx_gl_textures(gl->ctx);
    switch (target) {
        case 0x8069: return x->textures[x->active][G2_T2D];
        case 0x8514: return x->textures[x->active][G2_TCUBE];
        case 0x806a: return x->textures[x->active][G2_T3D];
        case 0x8c1d: return x->textures[x->active][G2_TARRAY];
        case 0x9104: return x->textures[x->active][G2_TMS];
        default: return 0;
    }
}

/* the bits of each channel of the draw framebuffer's color, depth and stencil */
static int channel_bits(Gles2Context* gl, unsigned pname) {
    G2Framebuffer* fb = gles2_framebuffer(gles2_fbos(gl)->draw);
    if (!gles2_fbos(gl)->draw || !fb) {
        if (pname == 0x0d56) return gl->draw ? gl->draw->depth_bits : 0;
        if (pname == 0x0d57) return gl->draw ? gl->draw->stencil_bits : 0;
        return 8;
    }
    if (pname == 0x0d56) return fb->at[G2_DEPTH_ATTACHMENT].kind ? 24 : 0;
    if (pname == 0x0d57) return fb->at[G2_STENCIL_ATTACHMENT].kind ? 8 : 0;
    return fb->at[0].kind ? 8 : 0;
}

/* the value of a state or limit: its count, 0 for a name GL does not have */
static int state(Gles2Context* gl, unsigned pname, double* v) {
    const SnxGlFixed* fs = gles2_fixed(gl);
    const VkPhysicalDeviceLimits* l = &V.props.limits;
    int max_texture = (int)(V.max_texture ? V.max_texture : 4096);
    SnxGlVertexArray* vao = snx_gl_vertex_array(gl->ctx);
    switch (pname) {
    /* the limits: the device's, at most what a Mali-G52 gives */
    case 0x0d33: /* GL_MAX_TEXTURE_SIZE */
    case 0x851c: /* GL_MAX_CUBE_MAP_TEXTURE_SIZE */
    case 0x84e8: v[0] = max_texture; return 1;
    case 0x8073: v[0] = l->maxImageDimension3D < 2048 ? l->maxImageDimension3D : 2048; return 1;
    case 0x88ff: v[0] = l->maxImageArrayLayers < 256 ? l->maxImageArrayLayers : 256; return 1;
    case 0x0d3a: v[0] = v[1] = max_texture; return 2;
    case 0x8869: v[0] = G2_ATTRIBS; return 1;
    case 0x8dfb: v[0] = 256; return 1;  /* GL_MAX_VERTEX_UNIFORM_VECTORS */
    case 0x8dfd: v[0] = 256; return 1;  /* GL_MAX_FRAGMENT_UNIFORM_VECTORS */
    case 0x8b4a:
    case 0x8b49: v[0] = 1024; return 1; /* GL_MAX_*_UNIFORM_COMPONENTS */
    case 0x8dfc: v[0] = 15; return 1;   /* GL_MAX_VARYING_VECTORS */
    case 0x8b4b: v[0] = 60; return 1;   /* GL_MAX_VARYING_COMPONENTS */
    case 0x9122: v[0] = 64; return 1;   /* GL_MAX_VERTEX_OUTPUT_COMPONENTS */
    case 0x9125: v[0] = 60; return 1;   /* GL_MAX_FRAGMENT_INPUT_COMPONENTS */
    case 0x8b4d: v[0] = G2_UNITS; return 1;
    case 0x8b4c:
    case 0x8872: v[0] = 16; return 1;
    case 0x846e: v[0] = 1, v[1] = V.features.wideLines ? l->lineWidthRange[1] : 1; return 2;
    case 0x846d: v[0] = 1, v[1] = V.features.largePoints ? l->pointSizeRange[1] : 1; return 2;
    case 0x0d50: v[0] = l->subPixelPrecisionBits; return 1;
    case 0x8cdf:
    case 0x8824: v[0] = G2_COLORS; return 1;
    case 0x80e9:
    case 0x80e8: v[0] = 1 << 20; return 1;
    case 0x8d57: v[0] = 4; return 1;    /* GL_MAX_SAMPLES */
    case 0x8a2f: v[0] = G2_UBO_BINDINGS; return 1;
    case 0x8a30: v[0] = l->maxUniformBufferRange < 65536 ? l->maxUniformBufferRange : 65536; return 1;
    case 0x8a34: v[0] = (double)l->minUniformBufferOffsetAlignment; return 1;
    case 0x8a2b:
    case 0x8a2d: v[0] = 12; return 1;
    case 0x8a2e: v[0] = 24; return 1;
    case 0x8a31:
    case 0x8a33: v[0] = 1024 + 12 * 16384; return 1;
    case 0x84fd: v[0] = l->maxSamplerLodBias; return 1;
    case 0x9111: v[0] = 0; return 1;    /* GL_MAX_SERVER_WAIT_TIMEOUT */
    case 0x8d6b: v[0] = 4294967295.0; return 1;
    case 0x8c8a: v[0] = 64; return 1;
    case 0x8c8b:
    case 0x8c80: v[0] = 4; return 1;
    case 0x8904: v[0] = -8; return 1;
    case 0x8905: v[0] = 7; return 1;
    case 0x821b: v[0] = 3; return 1;    /* GL_MAJOR_VERSION */
    case 0x821c: v[0] = 1; return 1; /* GL_MINOR_VERSION */
    case 0x821d: v[0] = EXTENSIONS; return 1;
    case 0x84ff: v[0] = V.features.samplerAnisotropy ? l->maxSamplerAnisotropy : 1; return 1;
    case 0x86a2: /* GL_NUM_COMPRESSED_TEXTURE_FORMATS */
    case 0x8df9: /* GL_NUM_SHADER_BINARY_FORMATS */
    case 0x87fe: v[0] = 0; return 1; /* GL_NUM_PROGRAM_BINARY_FORMATS */
    case 0x86a3:
    case 0x8df8:
    case 0x87ff: return 1; /* the lists: empty, one value read as 0 */
    case 0x0d52:
    case 0x0d53:
    case 0x0d54:
    case 0x0d55:
    case 0x0d56:
    case 0x0d57: v[0] = channel_bits(gl, pname); return 1;
    case 0x80a8:
    case 0x80a9: v[0] = 0; return 1; /* GL_SAMPLE_BUFFERS, GL_SAMPLES */
    case 0x8b9a: v[0] = GL_UNSIGNED_BYTE_; return 1;
    case 0x8b9b: v[0] = GL_RGBA_; return 1;
    /* GLES 3.1's: compute, storage buffers, images, vertex bindings, samples */
    case 0x90eb: v[0] = l->maxComputeWorkGroupInvocations < 1024 ? l->maxComputeWorkGroupInvocations : 1024; return 1;
    case 0x8262: v[0] = l->maxComputeSharedMemorySize < 32768 ? l->maxComputeSharedMemorySize : 32768; return 1;
    case 0x90dd: v[0] = G2_SSBO_BINDINGS; return 1;
    case 0x90de: v[0] = 1 << 27; return 1;  /* GL_MAX_SHADER_STORAGE_BLOCK_SIZE */
    case 0x90d6: v[0] = 0; return 1;        /* GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS: a Mali's */
    case 0x90da:
    case 0x90db:
    case 0x90dc: v[0] = G2_SSBO_BINDINGS; return 1;
    case 0x90df: v[0] = (double)l->minStorageBufferOffsetAlignment; return 1;
    case 0x8f38: v[0] = G2_IMAGE_UNITS; return 1;
    case 0x90ca: v[0] = 0; return 1; /* GL_MAX_VERTEX_IMAGE_UNIFORMS */
    case 0x90ce:
    case 0x91bd:
    case 0x90cf: v[0] = G2_IMAGE_UNITS; return 1;
    case 0x91bb: v[0] = 12; return 1;
    case 0x91bc: v[0] = 16; return 1;
    case 0x8263: v[0] = 1024; return 1;
    case 0x8264: /* GL_MAX_COMPUTE_ATOMIC_COUNTER_BUFFERS and the other atomic counters: none */
    case 0x8265:
    case 0x92cc:
    case 0x92d0:
    case 0x92d1:
    case 0x92d2:
    case 0x92d3:
    case 0x92d4:
    case 0x92d6:
    case 0x92d7:
    case 0x92d8:
    case 0x92d9:
    case 0x92dc: v[0] = 0; return 1;
    case 0x8266: v[0] = 1024 + 12 * 16384; return 1;
    case 0x8e59: v[0] = 1; return 1;   /* GL_MAX_SAMPLE_MASK_WORDS */
    case 0x910e:
    case 0x910f:
    case 0x9110: v[0] = 4; return 1;   /* GL_MAX_COLOR_TEXTURE_SAMPLES, DEPTH, INTEGER */
    case 0x82d9: v[0] = 2047; return 1;
    case 0x82da: v[0] = G2_ATTRIBS; return 1;
    case 0x82e5: v[0] = 2048; return 1;
    case 0x9315:
    case 0x9316: v[0] = max_texture; return 1;
    case 0x9318: v[0] = 4; return 1;
    case 0x826e: v[0] = 1024; return 1; /* GL_MAX_UNIFORM_LOCATIONS */
    case 0x8e5e: v[0] = -8; return 1;
    case 0x8e5f: v[0] = 7; return 1;
    case 0x90d3: v[0] = gles2_objects(gl)->storage; return 1;
    case 0x8f43: v[0] = gles2_objects(gl)->draw_indirect; return 1;
    case 0x90ef: v[0] = gles2_objects(gl)->dispatch_indirect; return 1;
    case 0x825a: v[0] = 0; return 1;    /* GL_PROGRAM_PIPELINE_BINDING */
    case 0x8e52: v[0] = fs->sample_mask; return 1;
    /* the state */
    case 0x0ba2:
        for (int i = 0; i < 4; ++i) v[i] = fs->viewport[i];
        return 4;
    case 0x0c10:
        for (int i = 0; i < 4; ++i) v[i] = fs->scissor[i];
        return 4;
    case 0x0c22:
        for (int i = 0; i < 4; ++i) v[i] = fs->clear_color[i];
        return 4;
    case 0x0c23:
        for (int i = 0; i < 4; ++i) v[i] = fs->color_mask[i];
        return 4;
    case 0x0b73: v[0] = fs->clear_depth; return 1;
    case 0x0b74: v[0] = fs->depth_func; return 1;
    case 0x0b70: v[0] = fs->depth_range[0], v[1] = fs->depth_range[1]; return 2;
    case 0x0b72: v[0] = fs->depth_mask; return 1;
    case 0x0b91: v[0] = fs->clear_stencil; return 1;
    case 0x0b92: v[0] = fs->stencil[0].func; return 1;
    case 0x0b93: v[0] = fs->stencil[0].mask; return 1;
    case 0x0b94: v[0] = fs->stencil[0].fail; return 1;
    case 0x0b95: v[0] = fs->stencil[0].zfail; return 1;
    case 0x0b96: v[0] = fs->stencil[0].zpass; return 1;
    case 0x0b97: v[0] = fs->stencil[0].ref; return 1;
    case 0x0b98: v[0] = fs->stencil[0].write; return 1;
    case 0x8800: v[0] = fs->stencil[1].func; return 1;
    case 0x8801: v[0] = fs->stencil[1].fail; return 1;
    case 0x8802: v[0] = fs->stencil[1].zfail; return 1;
    case 0x8803: v[0] = fs->stencil[1].zpass; return 1;
    case 0x8ca3: v[0] = fs->stencil[1].ref; return 1;
    case 0x8ca4: v[0] = fs->stencil[1].mask; return 1;
    case 0x8ca5: v[0] = fs->stencil[1].write; return 1;
    case 0x80c9: v[0] = fs->blend_src[0]; return 1;
    case 0x80c8: v[0] = fs->blend_dst[0]; return 1;
    case 0x80cb: v[0] = fs->blend_src[1]; return 1;
    case 0x80ca: v[0] = fs->blend_dst[1]; return 1;
    case 0x8009: v[0] = fs->blend_eq[0]; return 1;
    case 0x883d: v[0] = fs->blend_eq[1]; return 1;
    case 0x8005:
        for (int i = 0; i < 4; ++i) v[i] = fs->blend_color[i];
        return 4;
    case 0x0b45: v[0] = fs->cull_face; return 1;
    case 0x0b46: v[0] = fs->front_face; return 1;
    case 0x0b21: v[0] = fs->line_width; return 1;
    case 0x8038: v[0] = fs->offset_factor; return 1;
    case 0x2a00: v[0] = fs->offset_units; return 1;
    case 0x80aa: v[0] = fs->coverage_value; return 1;
    case 0x80ab: v[0] = fs->coverage_invert; return 1;
    case 0x84e0: v[0] = 0x84c0 + snx_gl_textures(gl->ctx)->active; return 1;
    case 0x8069:
    case 0x8514:
    case 0x806a:
    case 0x8c1d:
    case 0x9104: v[0] = binding_of(gl, pname); return 1;
    case 0x8919: v[0] = snx_gl_textures(gl->ctx)->samplers[snx_gl_textures(gl->ctx)->active]; return 1;
    case 0x8894: v[0] = gles2_objects(gl)->array; return 1;
    case 0x8895: v[0] = vao ? vao->elements : 0; return 1;
    case 0x8b8d: v[0] = snx_gl_current_program(gl->ctx); return 1;
    case 0x8ca6: v[0] = gles2_fbos(gl)->draw; return 1;
    case 0x8caa: v[0] = gles2_fbos(gl)->read; return 1;
    case 0x8ca7: v[0] = gles2_fbos(gl)->renderbuffer; return 1;
    case 0x85b5: v[0] = gles2_objects(gl)->vertex_array; return 1;
    case 0x8f36: v[0] = gles2_objects(gl)->copy_read; return 1;
    case 0x8f37: v[0] = gles2_objects(gl)->copy_write; return 1;
    case 0x88ed: v[0] = gles2_objects(gl)->pixel_pack; return 1;
    case 0x88ef: v[0] = gles2_objects(gl)->pixel_unpack; return 1;
    case 0x8a28: v[0] = gles2_objects(gl)->uniform; return 1;
    case 0x8c8f: v[0] = gles2_objects(gl)->feedback; return 1;
    case 0x0d05: v[0] = fs->pixel[0].alignment; return 1;
    case 0x0cf5: v[0] = fs->pixel[1].alignment; return 1;
    case 0x0d02: v[0] = fs->pixel[0].row_length; return 1;
    case 0x0d03: v[0] = fs->pixel[0].skip_rows; return 1;
    case 0x0d04: v[0] = fs->pixel[0].skip_pixels; return 1;
    case 0x0cf2: v[0] = fs->pixel[1].row_length; return 1;
    case 0x0cf3: v[0] = fs->pixel[1].skip_rows; return 1;
    case 0x0cf4: v[0] = fs->pixel[1].skip_pixels; return 1;
    case 0x806e: v[0] = fs->pixel[1].image_height; return 1;
    case 0x806d: v[0] = fs->pixel[1].skip_images; return 1;
    case 0x8192: v[0] = fs->hints[0]; return 1;
    case 0x8b8b: v[0] = fs->hints[1]; return 1;
    case 0x0c02: {
        G2Framebuffer* fb = gles2_framebuffer(gles2_fbos(gl)->read);
        v[0] = gles2_fbos(gl)->read && fb ? fb->read : GL_BACK_;
        return 1;
    }
    default: break;
    }
    if (pname >= 0x8825 && pname < 0x8825 + G2_COLORS) { /* GL_DRAW_BUFFERi */
        G2Framebuffer* fb = gles2_framebuffer(gles2_fbos(gl)->draw);
        unsigned i = pname - 0x8825;
        v[0] = gles2_fbos(gl)->draw && fb ? fb->draw[i] : i == 0 ? GL_BACK_ : 0;
        return 1;
    }
    int on = snx_gl_capability(gl->ctx, pname);
    if (on >= 0) {
        v[0] = on;
        return 1;
    }
    return 0;
}

static void g_get_integerv(CPU* c, Gles2Context* gl) {
    double v[16] = {0};
    int n = state(gl, (unsigned)X(0), v);
    if (!n) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    for (int i = 0; i < n; ++i) {
        double d = v[i];
        int32_t x = d >= 2147483647.0 ? 2147483647 : d <= -2147483648.0 ? INT32_MIN : (int32_t)(d < 0 ? d - 0.5 : d + 0.5);
        if (d >= 2147483648.0 && d <= 4294967295.0) x = (int32_t)(uint32_t)d; /* masks: their bits */
        memcpy((unsigned char*)PTR(1) + 4 * i, &x, 4);
    }
}
static void g_get_integer64v(CPU* c, Gles2Context* gl) {
    double v[16] = {0};
    int n = state(gl, (unsigned)X(0), v);
    if (!n) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    for (int i = 0; i < n; ++i) {
        int64_t x = (int64_t)v[i];
        memcpy((unsigned char*)PTR(1) + 8 * i, &x, 8);
    }
}
static void g_get_floatv(CPU* c, Gles2Context* gl) {
    double v[16] = {0};
    int n = state(gl, (unsigned)X(0), v);
    if (!n) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    for (int i = 0; i < n; ++i) {
        float x = (float)v[i];
        memcpy((unsigned char*)PTR(1) + 4 * i, &x, 4);
    }
}
static void g_get_booleanv(CPU* c, Gles2Context* gl) {
    double v[16] = {0};
    int n = state(gl, (unsigned)X(0), v);
    if (!n) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    for (int i = 0; i < n; ++i) ((unsigned char*)PTR(1))[i] = v[i] != 0;
}
/* glGetIntegeri_v and glGetInteger64i_v: the indexed buffer bindings */
static bool indexed(Gles2Context* gl, unsigned pname, unsigned i, int64_t* v) {
    if (i < 3 && pname == 0x91be) { /* GL_MAX_COMPUTE_WORK_GROUP_COUNT */
        *v = V.props.limits.maxComputeWorkGroupCount[i] < 65535 ? V.props.limits.maxComputeWorkGroupCount[i] : 65535;
        return true;
    }
    if (i < 3 && pname == 0x91bf) {
        static const uint32_t least[3] = {128, 128, 64}; /* GLES 3.1's minimums, which a Mali gives */
        uint32_t d = V.props.limits.maxComputeWorkGroupSize[i];
        *v = d < least[i] ? d : least[i];
        return true;
    }
    if (i < G2_SSBO_BINDINGS && pname >= 0x90d3 && pname <= 0x90d5) {
        const SnxGlIndexedBuffer* b = &gles2_objects(gl)->storages[i];
        *v = pname == 0x90d3 ? b->buffer : pname == 0x90d4 ? b->offset : b->size;
        return true;
    }
    if (i < G2_IMAGE_UNITS && pname >= 0x8f3a && pname <= 0x8f3e) {
        const SnxGlImageUnit* u = &snx_gl_textures(gl->ctx)->images[i];
        *v = pname == 0x8f3a   ? u->texture
             : pname == 0x8f3b ? (uint32_t)u->level
             : pname == 0x8f3c ? u->layered
             : pname == 0x8f3d ? (uint32_t)u->layer
                               : u->access;
        return true;
    }
    if (i < 1 && pname == 0x8e52) {
        *v = gles2_fixed(gl)->sample_mask;
        return true;
    }
    if (i >= G2_UBO_BINDINGS) return false;
    switch (pname) {
        case 0x8a28: *v = gles2_objects(gl)->uniforms[i].buffer; return true;
        case 0x8a29: *v = gles2_objects(gl)->uniforms[i].offset; return true;
        case 0x8a2a: *v = gles2_objects(gl)->uniforms[i].size; return true;
        default: return false;
    }
}
static void g_get_integeri(CPU* c, Gles2Context* gl) {
    int64_t v = 0;
    if (!indexed(gl, (unsigned)X(0), (unsigned)X(1), &v)) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    int32_t x = (int32_t)v;
    memcpy(PTR(2), &x, 4);
}
static void g_get_integer64i(CPU* c, Gles2Context* gl) {
    int64_t v = 0;
    if (!indexed(gl, (unsigned)X(0), (unsigned)X(1), &v)) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    memcpy(PTR(2), &v, 8);
}
static void g_get_booleani(CPU* c, Gles2Context* gl) {
    int64_t v = 0;
    if (!indexed(gl, (unsigned)X(0), (unsigned)X(1), &v)) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    *(unsigned char*)PTR(2) = v != 0;
}
static void g_get_stringi(CPU* c, Gles2Context* gl) {
    unsigned i = (unsigned)X(1);
    if ((unsigned)X(0) != GL_EXTENSIONS_ || i >= EXTENSIONS) {
        gles2_error(gl, (unsigned)X(0) != GL_EXTENSIONS_ ? GL_INVALID_ENUM_ : GL_INVALID_VALUE_);
        X(0) = 0;
        return;
    }
    X(0) = (uint64_t)(uintptr_t)extensions[i];
}
static void g_nothing(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl;
}
/* glGetGraphicsResetStatus: the device was not lost (a lost one ends the process here) */
static void g_reset_status(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = 0;
}

const HleSymbol hle_gles2[] = {
    FN("glGetError", g_get_error),
    FN("glEnable", g_enable),
    FN("glDisable", g_disable),
    FN("glIsEnabled", g_is_enabled),
    FN("glBlendFunc", g_blend_func),
    FN("glBlendFuncSeparate", g_blend_func_separate),
    FN("glBlendEquation", g_blend_equation),
    FN("glBlendEquationOES", g_blend_equation),
    FN("glBlendEquationSeparate", g_blend_equation_separate),
    FN("glBlendColor", g_blend_color),
    FN("glColorMask", g_color_mask),
    FN("glDepthMask", g_depth_mask),
    FN("glDepthFunc", g_depth_func),
    FN("glDepthRangef", g_depth_range),
    FN("glCullFace", g_cull_face),
    FN("glFrontFace", g_front_face),
    FN("glStencilFunc", g_stencil_func),
    FN("glStencilFuncSeparate", g_stencil_func_separate),
    FN("glStencilOp", g_stencil_op),
    FN("glStencilOpSeparate", g_stencil_op_separate),
    FN("glStencilMask", g_stencil_mask),
    FN("glStencilMaskSeparate", g_stencil_mask_separate),
    FN("glClearColor", g_clear_color),
    FN("glClearDepthf", g_clear_depth),
    FN("glClearStencil", g_clear_stencil),
    FN("glViewport", g_viewport),
    FN("glScissor", g_scissor),
    FN("glLineWidth", g_line_width),
    FN("glPolygonOffset", g_polygon_offset),
    FN("glSampleCoverage", g_sample_coverage),
    FN("glHint", g_hint),
    FN("glPixelStorei", g_pixel_store),
    FN("glFlush", g_flush),
    FN("glFinish", g_finish),
    FN("glGetIntegerv", g_get_integerv),
    FN("glGetInteger64v", g_get_integer64v),
    FN("glGetFloatv", g_get_floatv),
    FN("glGetBooleanv", g_get_booleanv),
    FN("glGetIntegeri_v", g_get_integeri),
    FN("glGetInteger64i_v", g_get_integer64i),
    FN("glGetStringi", g_get_stringi),
    FN("glGetBooleani_v", g_get_booleani),
    FN("glReleaseShaderCompiler", g_nothing),
    FN("glInsertEventMarkerEXT", g_nothing),
    FN("glPushGroupMarkerEXT", g_nothing),
    FN("glPopGroupMarkerEXT", g_nothing),
    FN("glGetGraphicsResetStatus", g_reset_status),
    FN("glGetGraphicsResetStatusEXT", g_reset_status),
    FN("glGetGraphicsResetStatusKHR", g_reset_status),
    FN("glPrimitiveBoundingBox", g_nothing),
    FN("glPrimitiveBoundingBoxEXT", g_nothing),
    FN("glDebugMessageCallback", g_nothing),
    FN("glDebugMessageControl", g_nothing),
    FN("glDebugMessageInsert", g_nothing),
    FN("glObjectLabel", g_nothing),
    FN("glObjectPtrLabel", g_nothing),
    FN("glPushDebugGroup", g_nothing),
    FN("glPopDebugGroup", g_nothing),
    FN("glDebugMessageInsertKHR", g_nothing),
    FN("glObjectPtrLabelKHR", g_nothing),
    FN("glLabelObjectEXT", g_nothing),
    FN("glDebugMessageCallbackKHR", g_nothing),
    FN("glDebugMessageControlKHR", g_nothing),
    FN("glObjectLabelKHR", g_nothing),
    FN("glPushDebugGroupKHR", g_nothing),
    FN("glPopDebugGroupKHR", g_nothing),
    {"", NULL, NULL},
};
