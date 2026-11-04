/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 2 and 3's draws over Vulkan 1.3 (gles2.h), as zink makes them.

   The draw framebuffer is a render target: dynamic rendering into its attachments' images, open in the frame's
   command buffer from the first draw or clear into it till a copy, another target or the frame's end. The images go
   from their rest (gles2_object.c) to their attachment layouts at its start and back at its end. The window's
   framebuffer is the swapchain image, and a depth and stencil buffer of the surface. Its origin is GL's, the bottom
   left: the viewport has a negative height, so GL's coordinates are kept and the image holds the window as it is.
   A framebuffer object's images are drawn upright instead, with the front face turned, so a texture holds its rows
   in GL's order, row 0 the bottom. GL's clip depth is -1 to 1: VK_EXT_depth_clip_control takes it as it is.

   A pipeline is made at the first draw of its state (G2PipelineKey: the program, the target's formats, the vertex
   formats, blending) and kept in a cache by the key's hash. The rest of GL's state is Vulkan 1.3's dynamic state, set
   at each draw. Each draw writes a descriptor set of the program's layout: the default uniform blocks, the textures
   of the samplers' units (a black texture for an incomplete one), the uniform buffers of the blocks' binding points */
#include "android/bridge/gles2.h"
#include "android/bridge/hle_impl.h"
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>

#define V g_gles_vk

enum {
    GL_INVALID_ENUM_ = 0x0500,
    GL_INVALID_VALUE_ = 0x0501,
    GL_INVALID_OPERATION_ = 0x0502,
    GL_OUT_OF_MEMORY_ = 0x0505,
    GL_INVALID_FRAMEBUFFER_OPERATION_ = 0x0506,
    GL_POINTS_ = 0,
    GL_LINES_ = 1,
    GL_LINE_LOOP_ = 2,
    GL_LINE_STRIP_ = 3,
    GL_TRIANGLES_ = 4,
    GL_TRIANGLE_STRIP_ = 5,
    GL_TRIANGLE_FAN_ = 6,
    GL_BYTE_ = 0x1400,
    GL_UNSIGNED_BYTE_ = 0x1401,
    GL_SHORT_ = 0x1402,
    GL_UNSIGNED_SHORT_ = 0x1403,
    GL_INT_ = 0x1404,
    GL_UNSIGNED_INT_ = 0x1405,
    GL_FLOAT_ = 0x1406,
    GL_HALF_FLOAT_ = 0x140b,
    GL_HALF_FLOAT_OES_ = 0x8d61,
    GL_FIXED_ = 0x140c,
    GL_INT_2101010_ = 0x8d9f,
    GL_UNSIGNED_INT_2101010_ = 0x8368,
    GL_FRONT_ = 0x0404,
    GL_BACK_ = 0x0405,
    GL_CCW_ = 0x0901,
    GL_COLOR_BUFFER_BIT_ = 0x4000,
    GL_DEPTH_BUFFER_BIT_ = 0x0100,
    GL_STENCIL_BUFFER_BIT_ = 0x0400,
    GL_COLOR_ = 0x1800,
    GL_DEPTH_ = 0x1801,
    GL_STENCIL_ = 0x1802,
    GL_DEPTH_STENCIL_ = 0x84f9,
    GL_RGBA_ = 0x1908,
    GL_BGRA_EXT_ = 0x80e1,
    GL_NEAREST_ = 0x2600,
    GL_COLOR_ATTACHMENT0_ = 0x8ce0,
    GL_TEXTURE_2D_ = 0x0de1,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X_ = 0x8515,
};

/* the module's state: the rendering open in a frame, and the last draw's descriptor set */
typedef struct {
    VkExtent2D extent;
    VkFormat colors[G2_COLORS], depth;
    VkImage color_images[G2_COLORS], depth_image;
    VkImageView color_views[G2_COLORS], depth_view;
    VkImageLayout color_rest[G2_COLORS], depth_rest;
    VkImageAspectFlags depth_aspect;
    VkSampleCountFlagBits samples;
    bool flip;     /* the window's (or a pbuffer's): GL's origin with a negative viewport height */
    bool integer[G2_COLORS];
    /* what it was made from, to tell another apart */
    uint32_t fbo, fbo_generation, surface_generation, image, epoch;
} Target;

static struct {
    GlesSurface* surface; /* the surface whose frame has the rendering open */
    Gles2Context* gl;     /* the context that opened it, for its occlusion query */
    Target open;
    uint32_t epoch;       /* counts gles2_target_forget's */
    /* the last draw's descriptor set: used again while nothing in it changed */
    uint64_t set_hash, set_serial;
    VkDescriptorSet set;
    /* a zeroed buffer for a block without a buffer bound */
    G2Buffer zeros;
} g_draw;

void gles2_target_forget(void) {
    g_draw.epoch++;
}

/* ---- the render targets ---- */

/* the window's depth and stencil buffer, made at its first use with the bits the config asks for */
static bool surface_depth(GlesSurface* s) {
    if (s->depth || (!s->depth_bits && !s->stencil_bits)) return true;
    static const VkFormat candidates[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                          VK_FORMAT_D16_UNORM_S8_UINT};
    VkFormat format = VK_FORMAT_UNDEFINED;
    if (!s->stencil_bits && s->depth_bits <= 16) format = VK_FORMAT_D16_UNORM;
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates && !format; ++i) {
        VkFormatProperties p;
        V.vk.vkGetPhysicalDeviceFormatProperties(V.physical, candidates[i], &p);
        if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) format = candidates[i];
    }
    if (!format) return false;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {s->extent.width, s->extent.height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT | (format == VK_FORMAT_D16_UNORM ? 0 : VK_IMAGE_ASPECT_STENCIL_BIT);
    GlesImage image;
    if (!gles_vk_image(&info, VK_IMAGE_VIEW_TYPE_2D, aspect, &image)) return false;
    s->depth = image.image;
    s->depth_memory = image.memory;
    s->depth_view = image.view;
    s->depth_format = format;
    s->depth_ready = false;
    return true;
}

static VkImageAspectFlags depth_aspect_of(VkFormat f) {
    switch (f) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    }
}

/* an attachment's image, view, format and rest layout; false without an image */
static bool attachment(const G2Attachment* a, VkImage* image, VkImageView* view, VkFormat* format,
                       VkImageLayout* rest, VkExtent2D* extent, VkSampleCountFlagBits* samples, bool* integer) {
    if (a->kind == 1) {
        G2Texture* t = gles2_texture(a->name);
        if (!t || !t->image.image || a->level >= t->levels) return false;
        VkImageAspectFlags aspect = gles2_aspect(t->format);
        if (t->vk == VK_FORMAT_D24_UNORM_S8_UINT || t->vk == VK_FORMAT_D32_SFLOAT_S8_UINT)
            aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
        int layer = t->target == G2_T3D ? 0 : a->layer;
        *view = gles2_texture_view(t, VK_IMAGE_VIEW_TYPE_2D, a->level, 1, layer, 1, false, aspect);
        if (t->target == G2_T3D) gles2_warn(8, "a layer of a 3D texture as an attachment: its first is drawn");
        *image = t->image.image;
        *format = t->vk;
        *rest = t->layout;
        int w = t->width >> a->level, h = t->height >> a->level;
        *extent = (VkExtent2D){(uint32_t)(w > 0 ? w : 1), (uint32_t)(h > 0 ? h : 1)};
        *samples = t->samples;
        *integer = t->format->kind == G2_INT || t->format->kind == G2_UINT;
        return *view != VK_NULL_HANDLE;
    }
    G2Renderbuffer* r = gles2_renderbuffer(a->name);
    if (!r || !r->image.image) return false;
    *image = r->image.image;
    *view = r->image.view;
    *format = r->vk;
    *rest = r->ready ? gles2_rest_layout(r->format, false) : VK_IMAGE_LAYOUT_UNDEFINED;
    *extent = (VkExtent2D){(uint32_t)r->width, (uint32_t)r->height};
    *samples = r->samples;
    *integer = r->format->kind == G2_INT || r->format->kind == G2_UINT;
    return true;
}

/* the draw target of the context: the bound framebuffer's, or the surface's. False when incomplete */
static bool target_of(Gles2Context* gl, unsigned fbo_name, bool read, Target* t) {
    *t = (Target){.epoch = g_draw.epoch};
    GlesSurface* s = read ? gl->read : gl->draw;
    G2Framebuffer* fb = gles2_framebuffer(fbo_name);
    if (!fbo_name || !fb) {
        if (!s) return false;
        t->flip = true;
        t->extent = s->extent;
        t->samples = VK_SAMPLE_COUNT_1_BIT;
        t->surface_generation = s->generation;
        t->colors[0] = s->format;
        t->color_rest[0] = s->pbuffer ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        if (!surface_depth(s)) return false;
        if (s->depth) {
            t->depth = s->depth_format;
            t->depth_image = s->depth;
            t->depth_view = s->depth_view;
            t->depth_rest = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            t->depth_aspect = depth_aspect_of(s->depth_format);
        }
        return true;
    }
    t->fbo = fbo_name;
    t->fbo_generation = fb->generation;
    bool any = false;
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    for (int i = 0; i < G2_COLORS; ++i) {
        unsigned which = read ? (i == 0 ? fb->read : 0) : fb->draw[i];
        if (which < GL_COLOR_ATTACHMENT0_ || which >= GL_COLOR_ATTACHMENT0_ + G2_COLORS) continue;
        const G2Attachment* a = &fb->at[which - GL_COLOR_ATTACHMENT0_];
        if (!a->kind) continue;
        VkExtent2D e;
        if (!attachment(a, &t->color_images[i], &t->color_views[i], &t->colors[i], &t->color_rest[i], &e, &t->samples,
                        &t->integer[i]))
            return false;
        if (e.width < w) w = e.width;
        if (e.height < h) h = e.height;
        any = true;
    }
    const G2Attachment* d = fb->at[G2_DEPTH_ATTACHMENT].kind ? &fb->at[G2_DEPTH_ATTACHMENT] : &fb->at[G2_STENCIL_ATTACHMENT];
    if (d->kind) {
        VkExtent2D e;
        bool integer;
        if (!attachment(d, &t->depth_image, &t->depth_view, &t->depth, &t->depth_rest, &e, &t->samples, &integer))
            return false;
        t->depth_aspect = depth_aspect_of(t->depth);
        if (!fb->at[G2_STENCIL_ATTACHMENT].kind) t->depth_aspect &= VK_IMAGE_ASPECT_DEPTH_BIT;
        if (!fb->at[G2_DEPTH_ATTACHMENT].kind) t->depth_aspect &= VK_IMAGE_ASPECT_STENCIL_BIT;
        if (!t->depth_aspect) t->depth_aspect = depth_aspect_of(t->depth);
        if (e.width < w) w = e.width;
        if (e.height < h) h = e.height;
        any = true;
    }
    if (!any) return false;
    t->extent = (VkExtent2D){w, h};
    return true;
}

static bool same_target(const Target* a, const Target* b) {
    return a->fbo == b->fbo && a->fbo_generation == b->fbo_generation && a->epoch == b->epoch &&
           a->surface_generation == b->surface_generation && a->flip == b->flip &&
           !memcmp(a->color_views, b->color_views, sizeof a->color_views) && a->depth_view == b->depth_view;
}

static VkImageLayout attachment_layout(bool depth) {
    return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
static const VkPipelineStageFlags ATTACHMENT_STAGES = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
static const VkAccessFlags ATTACHMENT_ACCESS = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
static const VkPipelineStageFlags ALL_READERS = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;

void gles2_rendering_end(GlesSurface* s) {
    VkCommandBuffer cb = s->slots[s->slot].cb;
    if (g_draw.gl) gles2_queries_end(g_draw.gl, cb);
    V.vk.vkCmdEndRendering(cb);
    const Target* t = &g_draw.open;
    for (int i = 0; i < G2_COLORS; ++i) {
        VkImage image = t->color_images[i];
        if (!t->color_views[i] && !(i == 0 && t->flip)) continue;
        VkImageLayout rest = t->color_rest[i] == VK_IMAGE_LAYOUT_UNDEFINED ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                                                                            : t->color_rest[i];
        if (rest != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            gles_image_barrier(cb, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rest,
                               ATTACHMENT_ACCESS, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                               ATTACHMENT_STAGES, ALL_READERS);
    }
    if (t->depth_view) {
        VkImageLayout rest = t->depth_rest == VK_IMAGE_LAYOUT_UNDEFINED ? attachment_layout(true) : t->depth_rest;
        if (rest != attachment_layout(true))
            gles_image_barrier(cb, t->depth_image, depth_aspect_of(t->depth), attachment_layout(true), rest,
                               ATTACHMENT_ACCESS, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                               ATTACHMENT_STAGES, ALL_READERS);
    }
    g_draw.surface = NULL;
    g_draw.gl = NULL;
}

void gles2_surface_gone(GlesSurface* s) {
    if (g_draw.surface == s) g_draw.surface = NULL, g_draw.gl = NULL;
}

void gles2_rendering_stop(Gles2Context* gl) {
    GlesSurface* s = gl->draw;
    if (s && s->recording && s->open == GLES_OPEN_RENDERING) gles_pass_end(s);
}

/* the renderbuffers of the target that are new: their images are in UNDEFINED */
static void mark_ready(Gles2Context* gl) {
    G2Framebuffer* fb = gles2_framebuffer(gl->draw_fbo);
    if (!fb) return;
    for (int i = 0; i < G2_ATTACHMENTS; ++i)
        if (fb->at[i].kind == 2) {
            G2Renderbuffer* r = gles2_renderbuffer(fb->at[i].name);
            if (r && r->image.image) r->ready = true;
        }
}

bool gles2_rendering_begin(Gles2Context* gl) {
    GlesSurface* s = gles2_surface(gl);
    if (!s) return false;
    Target t;
    if (!target_of(gl, gl->draw_fbo, false, &t)) {
        gles2_error(gl, GL_INVALID_FRAMEBUFFER_OPERATION_);
        return false;
    }
    if (!gles_frame_begin(s)) return false;
    if (t.flip && !s->pbuffer) {
        if (!gles_acquire(s)) return false;
        t.image = s->image;
        t.color_images[0] = s->images[s->image];
        t.color_views[0] = s->views[s->image];
    } else if (t.flip) {
        t.color_images[0] = s->images[0];
        t.color_views[0] = s->views[0];
    }
    if (s->open == GLES_OPEN_RENDERING && g_draw.surface == s && same_target(&g_draw.open, &t) && g_draw.gl == gl)
        return true;
    gles_pass_end(s);
    VkCommandBuffer cb = s->slots[s->slot].cb;
    VkRenderingAttachmentInfo colors[G2_COLORS];
    uint32_t count = 0;
    bool clear_window = false;
    for (int i = 0; i < G2_COLORS; ++i) {
        colors[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                .imageView = t.color_views[i],
                                                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                                .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        if (!t.color_views[i]) continue;
        count = (uint32_t)i + 1;
        VkImageLayout from = t.color_rest[i];
        if (t.flip && !s->pbuffer && !s->touched) {
            from = VK_IMAGE_LAYOUT_UNDEFINED; /* the acquired image, which the frame clears to black first */
            clear_window = true;
        }
        if (t.flip && s->pbuffer && !s->own_ready) from = VK_IMAGE_LAYOUT_UNDEFINED;
        if (from != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            gles_image_barrier(cb, t.color_images[i], VK_IMAGE_ASPECT_COLOR_BIT, from,
                               VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ACCESS_SHADER_READ_BIT |
                               VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, ATTACHMENT_ACCESS,
                               ALL_READERS | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, ATTACHMENT_STAGES);
        if (clear_window) {
            colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            colors[i].clearValue.color = (VkClearColorValue){{0, 0, 0, 1}};
        }
    }
    VkRenderingAttachmentInfo depth = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = t.depth_view,
                                       .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                       .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
    if (t.depth_view) {
        VkImageLayout from = t.depth_rest;
        if (t.flip && !s->depth_ready) from = VK_IMAGE_LAYOUT_UNDEFINED;
        if (from == VK_IMAGE_LAYOUT_UNDEFINED) {
            depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
            depth.clearValue.depthStencil = (VkClearDepthStencilValue){1, 0};
        }
        if (from != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
            gles_image_barrier(cb, t.depth_image, depth_aspect_of(t.depth), from,
                               VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                               ATTACHMENT_ACCESS, ALL_READERS | ATTACHMENT_STAGES, ATTACHMENT_STAGES);
    }
    VkRenderingAttachmentInfo stencil = depth;
    bool has_depth = t.depth_view && (t.depth_aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    bool has_stencil = t.depth_view && (depth_aspect_of(t.depth) & VK_IMAGE_ASPECT_STENCIL_BIT);
    VkRenderingInfo info = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                            .renderArea = {{0, 0}, t.extent},
                            .layerCount = 1,
                            .colorAttachmentCount = count,
                            .pColorAttachments = colors,
                            .pDepthAttachment = has_depth || (t.depth_view && !has_stencil) ? &depth : NULL,
                            .pStencilAttachment = has_stencil ? &stencil : NULL};
    V.vk.vkCmdBeginRendering(cb, &info);
    s->open = GLES_OPEN_RENDERING;
    if (t.flip && !s->pbuffer) s->touched = true;
    if (t.flip && s->pbuffer) s->own_ready = true;
    if (t.flip && t.depth_view) s->depth_ready = true;
    s->drew = true;
    mark_ready(gl);
    /* the target's rest layouts as they are now: a renderbuffer new to this rendering rests in its attachment one */
    for (int i = 0; i < G2_COLORS; ++i)
        if (t.color_rest[i] == VK_IMAGE_LAYOUT_UNDEFINED) t.color_rest[i] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    if (t.depth_rest == VK_IMAGE_LAYOUT_UNDEFINED) t.depth_rest = attachment_layout(true);
    g_draw.open = t;
    g_draw.surface = s;
    g_draw.gl = gl;
    gles2_queries_begin(gl, cb);
    return true;
}

/* ---- pipelines ---- */

static VkBlendFactor blend_factor(unsigned f) {
    switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 0x300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blend_op(unsigned e) {
    switch (e) {
    case 0x800a: return VK_BLEND_OP_SUBTRACT;
    case 0x800b: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkPrimitiveTopology topology(unsigned mode) {
    switch (mode) {
    case GL_POINTS_: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case GL_LINES_: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case GL_LINE_LOOP_:
    case GL_LINE_STRIP_: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case GL_TRIANGLE_STRIP_: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN_: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
/* the topology class a pipeline is made for: 0 points, 1 lines, 2 triangles */
static uint8_t topology_class(unsigned mode) {
    return mode == GL_POINTS_ ? 0 : mode <= GL_LINE_STRIP_ ? 1 : 2;
}

static VkPipeline pipeline(const G2Program* p, const G2PipelineKey* k) {
    uint64_t hash = snx_fnv1a(SNX_FNV_BASIS, k, sizeof *k);
    if (g_gles2.pipeline_capacity) {
        for (size_t i = hash & (g_gles2.pipeline_capacity - 1);; i = (i + 1) & (g_gles2.pipeline_capacity - 1)) {
            G2PipelineEntry* e = &g_gles2.pipelines[i];
            if (!e->pipeline) break;
            if (e->hash == hash && !memcmp(&e->key, k, sizeof *k)) return e->pipeline;
        }
    }
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = p->modules[0], .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = p->modules[1], .pName = "main"}};
    VkVertexInputBindingDescription bindings[G2_ATTRIBS];
    VkVertexInputAttributeDescription attributes[G2_ATTRIBS];
    uint32_t n = 0;
    for (uint32_t loc = 0; loc < G2_ATTRIBS; ++loc) {
        if (!k->formats[loc]) continue;
        bindings[n] = (VkVertexInputBindingDescription){loc, 0, (k->rates >> loc) & 1 ? VK_VERTEX_INPUT_RATE_INSTANCE
                                                                                       : VK_VERTEX_INPUT_RATE_VERTEX};
        attributes[n] = (VkVertexInputAttributeDescription){loc, loc, (VkFormat)k->formats[loc], 0};
        ++n;
    }
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                  .vertexBindingDescriptionCount = n,
                                                  .pVertexBindingDescriptions = bindings,
                                                  .vertexAttributeDescriptionCount = n,
                                                  .pVertexAttributeDescriptions = attributes};
    static const VkPrimitiveTopology classes[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                   VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = classes[k->topology]};
    VkPipelineViewportDepthClipControlCreateInfoEXT clip = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_DEPTH_CLIP_CONTROL_CREATE_INFO_EXT, .negativeOneToOne = VK_TRUE};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .pNext = k->clip_control ? &clip : NULL,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL,
                                                     .lineWidth = 1};
    VkSampleMask mask = k->sample_mask;
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = (VkSampleCountFlagBits)k->samples,
                                               .pSampleMask = &mask,
                                               .alphaToCoverageEnable = k->alpha_to_coverage};
    VkPipelineDepthStencilStateCreateInfo ds = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blends[G2_COLORS];
    VkFormat colors[G2_COLORS];
    uint32_t color_count = 0;
    for (int i = 0; i < G2_COLORS; ++i) {
        colors[i] = (VkFormat)k->colors[i];
        if (colors[i]) color_count = (uint32_t)i + 1;
        bool integer = ((k->blend >> (4 + i)) & 1) != 0;
        blends[i] = (VkPipelineColorBlendAttachmentState){.blendEnable = (k->blend & 1) && !integer,
                                                          .colorWriteMask = k->write_mask};
        static const unsigned codes[16] = {0, 1, 0x300, 0x301, 0x302, 0x303, 0x304, 0x305,
                                           0x306, 0x307, 0x308, 0x8001, 0x8002, 0x8003, 0x8004, 1};
        blends[i].srcColorBlendFactor = blend_factor(codes[k->factors[0] & 15]);
        blends[i].dstColorBlendFactor = blend_factor(codes[k->factors[1] & 15]);
        blends[i].srcAlphaBlendFactor = blend_factor(codes[k->factors[2] & 15]);
        blends[i].dstAlphaBlendFactor = blend_factor(codes[k->factors[3] & 15]);
        static const unsigned eqs[5] = {0x8006, 0x800a, 0x800b, 0x8007, 0x8008};
        blends[i].colorBlendOp = blend_op(eqs[k->ops[0] % 5]);
        blends[i].alphaBlendOp = blend_op(eqs[k->ops[1] % 5]);
    }
    VkPipelineColorBlendStateCreateInfo blending = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = color_count,
                                                    .pAttachments = blends};
    static const VkDynamicState dynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
        VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE,
        VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    /* the strides, last, are dynamic only with vertex inputs: a draw without them binds no vertex buffer */
    uint32_t dynamic_count = sizeof dynamic / sizeof *dynamic - (n ? 0 : 1);
    VkPipelineDynamicStateCreateInfo dyn = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = dynamic_count,
                                            .pDynamicStates = dynamic};
    VkFormat depth = (VkFormat)k->depth;
    VkImageAspectFlags aspect = depth ? depth_aspect_of(depth) : 0;
    VkPipelineRenderingCreateInfo rendering = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = color_count,
        .pColorAttachmentFormats = colors,
        .depthAttachmentFormat = aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? depth : VK_FORMAT_UNDEFINED,
        .stencilAttachmentFormat = aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? depth : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .pNext = &rendering,
                                         .stageCount = 2,
                                         .pStages = stages,
                                         .pVertexInputState = &input,
                                         .pInputAssemblyState = &assembly,
                                         .pViewportState = &viewport,
                                         .pRasterizationState = &raster,
                                         .pMultisampleState = &ms,
                                         .pDepthStencilState = &ds,
                                         .pColorBlendState = &blending,
                                         .pDynamicState = &dyn,
                                         .layout = p->layout};
    VkPipeline made = VK_NULL_HANDLE;
    if (V.vk.vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &info, NULL, &made) != VK_SUCCESS) {
        gles2_warn(9, "Vulkan does not make a pipeline of a draw's state");
        return VK_NULL_HANDLE;
    }
    /* into the cache, which doubles at half full */
    if ((g_gles2.pipeline_count + 1) * 2 > g_gles2.pipeline_capacity) {
        size_t cap = g_gles2.pipeline_capacity ? g_gles2.pipeline_capacity * 2 : 256;
        G2PipelineEntry* table = calloc(cap, sizeof *table);
        if (!table) return made; /* not kept: it is made again at the next draw */
        for (size_t i = 0; i < g_gles2.pipeline_capacity; ++i) {
            G2PipelineEntry* e = &g_gles2.pipelines[i];
            if (!e->pipeline) continue;
            size_t j = e->hash & (cap - 1);
            while (table[j].pipeline) j = (j + 1) & (cap - 1);
            table[j] = *e;
        }
        free(g_gles2.pipelines);
        g_gles2.pipelines = table;
        g_gles2.pipeline_capacity = cap;
    }
    size_t j = hash & (g_gles2.pipeline_capacity - 1);
    while (g_gles2.pipelines[j].pipeline) j = (j + 1) & (g_gles2.pipeline_capacity - 1);
    g_gles2.pipelines[j] = (G2PipelineEntry){*k, made, hash};
    g_gles2.pipeline_count++;
    return made;
}

void gles2_pipelines_forget(const G2Program* p) {
    size_t cap = g_gles2.pipeline_capacity;
    if (!cap) return;
    uint32_t name = (uint32_t)(p - g_gles2.programs);
    /* the table without them, entries moved back to fill the gaps of open addressing */
    G2PipelineEntry* table = calloc(cap, sizeof *table);
    if (!table) return;
    size_t kept = 0;
    for (size_t i = 0; i < cap; ++i) {
        G2PipelineEntry* e = &g_gles2.pipelines[i];
        if (!e->pipeline) continue;
        if (e->key.program == name) {
            gles_vk_retire((GlesGarbage){.pipeline = e->pipeline});
            continue;
        }
        size_t j = e->hash & (cap - 1);
        while (table[j].pipeline) j = (j + 1) & (cap - 1);
        table[j] = *e;
        ++kept;
    }
    free(g_gles2.pipelines);
    g_gles2.pipelines = table;
    g_gles2.pipeline_count = kept;
}

/* ---- the vertex inputs ---- */

/* the Vulkan format of an attribute array, for a shader input of base; 0 for one Vulkan has no format of */
static VkFormat vertex_format(const G2Attrib* a, uint8_t base) {
    int n = a->size;
    bool integer = a->integer || base != SNX_GLSLC_BASE_FLOAT;
    static const VkFormat f32[4] = {VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT,
                                    VK_FORMAT_R32G32B32A32_SFLOAT};
    static const VkFormat f16[4] = {VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT,
                                    VK_FORMAT_R16G16B16A16_SFLOAT};
    /* each 8- and 16-bit kind: UNORM, SNORM, USCALED, SSCALED, UINT, SINT are 6 apart... by name instead */
    static const VkFormat u8[3][4] = {
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM},
        {VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED},
        {VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT}};
    static const VkFormat s8[3][4] = {
        {VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM},
        {VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED},
        {VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT}};
    static const VkFormat u16[3][4] = {
        {VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM},
        {VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED},
        {VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT}};
    static const VkFormat s16[3][4] = {
        {VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM},
        {VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED},
        {VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT}};
    static const VkFormat u32[4] = {VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT,
                                    VK_FORMAT_R32G32B32A32_UINT};
    static const VkFormat s32[4] = {VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT,
                                    VK_FORMAT_R32G32B32A32_SINT};
    int k = integer ? 2 : a->normalized ? 0 : 1;
    switch (a->type) {
    case GL_FLOAT_: return integer ? VK_FORMAT_UNDEFINED : f32[n - 1];
    case GL_HALF_FLOAT_:
    case GL_HALF_FLOAT_OES_: return integer ? VK_FORMAT_UNDEFINED : f16[n - 1];
    case GL_UNSIGNED_BYTE_: return u8[k][n - 1];
    case GL_BYTE_: return s8[k][n - 1];
    case GL_UNSIGNED_SHORT_: return u16[k][n - 1];
    case GL_SHORT_: return s16[k][n - 1];
    case GL_UNSIGNED_INT_: return integer ? u32[n - 1] : VK_FORMAT_UNDEFINED;
    case GL_INT_: return integer ? s32[n - 1] : VK_FORMAT_UNDEFINED;
    case GL_UNSIGNED_INT_2101010_:
        return n != 4 ? VK_FORMAT_UNDEFINED : a->normalized ? VK_FORMAT_A2B10G10R10_UNORM_PACK32
                                                            : VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case GL_INT_2101010_:
        return n != 4 ? VK_FORMAT_UNDEFINED : a->normalized ? VK_FORMAT_A2B10G10R10_SNORM_PACK32
                                                            : VK_FORMAT_A2B10G10R10_SSCALED_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}
static uint32_t type_bytes(unsigned type) {
    switch (type) {
    case GL_BYTE_:
    case GL_UNSIGNED_BYTE_: return 1;
    case GL_SHORT_:
    case GL_UNSIGNED_SHORT_:
    case GL_HALF_FLOAT_:
    case GL_HALF_FLOAT_OES_: return 2;
    default: return 4;
    }
}
static uint32_t attrib_bytes(const G2Attrib* a) {
    return gles2_vertex_bytes(a->type, a->size);
}

/* a draw's vertices: the bindings, their offsets and strides by location */
typedef struct {
    VkBuffer buffers[G2_ATTRIBS];
    VkDeviceSize offsets[G2_ATTRIBS], strides[G2_ATTRIBS];
    uint32_t used; /* the locations */
} Vertices;

/* the vertex inputs of p for the vertices [lo, hi] and the instances [0, instances): the key's formats and rates,
   and the buffers. A client array's range is copied into the frame's arena */
static bool vertex_inputs(Gles2Context* gl, GlesSlot* slot, const G2Program* p, uint32_t lo, uint32_t hi,
                          uint32_t instances, G2PipelineKey* key, Vertices* v) {
    G2VertexArray* vao = gles2_vertex_array(gl);
    *v = (Vertices){0};
    for (uint32_t i = 0; i < p->input_count; ++i) {
        const G2Input* in = &p->inputs[i];
        if (in->location < 0 || in->spirv < 0) continue;
        for (uint32_t e = 0; e < (uint32_t)in->count * in->columns; ++e) {
            uint32_t index = (uint32_t)in->location + e, loc = (uint32_t)in->spirv + e;
            if (index >= G2_ATTRIBS || loc >= G2_ATTRIBS) continue;
            const G2Attrib* a = &vao->attribs[index];
            VkFormat format = a->enabled ? vertex_format(a, in->base) : VK_FORMAT_UNDEFINED;
            if (a->enabled && !format) gles2_warn(10, "a vertex attribute of a type Vulkan has no format of");
            v->used |= 1u << loc;
            if (!format) {
                /* the current value of the generic attribute, every vertex the same */
                static const VkFormat generic[3] = {VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_UINT,
                                                    VK_FORMAT_R32G32B32A32_SFLOAT};
                VkDeviceSize at = gles_vk_arena_take(slot, &slot->vertices, 16, 16, 4u << 20, 0);
                if (at == VK_WHOLE_SIZE) return false;
                memcpy(slot->vertices.map + at, gl->generic[index].bits, 16);
                key->formats[loc] = (uint32_t)generic[in->base == SNX_GLSLC_BASE_INT    ? 0
                                                      : in->base == SNX_GLSLC_BASE_UINT ? 1
                                                                                        : 2];
                v->buffers[loc] = slot->vertices.buffer;
                v->offsets[loc] = at;
                v->strides[loc] = 0;
                continue;
            }
            key->formats[loc] = (uint32_t)format;
            const G2VertexBinding* binding = &vao->bindings[a->binding];
            uint32_t stride = binding->stride;
            uint64_t pointer = binding->offset + a->relative;
            v->strides[loc] = stride;
            bool instanced = binding->divisor != 0;
            if (instanced) key->rates |= (uint16_t)(1u << loc);
            if (binding->divisor > 1) gles2_warn(11, "a vertex attribute divisor past 1 steps each instance");
            G2Buffer* b = gles2_buffer(binding->buffer);
            if (b) {
                G2Ref ref;
                if (!gles2_buffer_ref(slot, b, false, &ref)) return false;
                v->buffers[loc] = ref.buffer;
                v->offsets[loc] = ref.offset + pointer;
                continue;
            }
            if (!pointer) return false; /* an array without memory: nothing to draw */
            /* the client's array: the range the draw reads, copied at its place from the start */
            uint32_t first = instanced ? 0 : lo, last = instanced ? (instances ? instances - 1 : 0) : hi;
            uint64_t from = (uint64_t)first * stride, end = (uint64_t)last * stride + attrib_bytes(a);
            if (end > (64u << 20)) {
                gles2_warn(12, "a client array of more than 64 MiB: the draw is left out");
                return false;
            }
            VkDeviceSize at = gles_vk_arena_take(slot, &slot->vertices, end, 16, 4u << 20, 0);
            if (at == VK_WHOLE_SIZE) return false;
            memcpy(slot->vertices.map + at + from, (const unsigned char*)(uintptr_t)pointer + from,
                   (size_t)(end - from));
            v->buffers[loc] = slot->vertices.buffer;
            v->offsets[loc] = at;
        }
    }
    return true;
}

/* ---- descriptor sets ---- */

/* the black textures of samplers without a complete texture, one of each dimension, at the first draw */
static bool missing_ready(Gles2Context* gl) {
    if (g_gles2.missing_ready) return true;
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (!slot) return false;
    static const VkImageViewType types[G2_TARGETS] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE,
                                                      VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                      VK_IMAGE_VIEW_TYPE_2D};
    for (int i = 0; i < G2_TARGETS; ++i) {
        VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                  .flags = i == G2_TCUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                                  .imageType = i == G2_T3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                                  .format = VK_FORMAT_R8G8B8A8_UNORM,
                                  .extent = {1, 1, 1},
                                  .mipLevels = 1,
                                  .arrayLayers = i == G2_TCUBE ? 6 : 1,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .tiling = VK_IMAGE_TILING_OPTIMAL,
                                  .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        if (!gles_vk_image(&info, types[i], VK_IMAGE_ASPECT_COLOR_BIT, &g_gles2.missing[i])) return false;
        VkImage image = g_gles2.missing[i].image;
        gles_image_barrier(slot->cb, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkClearColorValue black = {{0, 0, 0, 1}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, info.arrayLayers};
        V.vk.vkCmdClearColorImage(slot->cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        gles_image_barrier(slot->cb, image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ALL_READERS);
    }
    g_gles2.missing_ready = true;
    return true;
}

/* the zeros a block without a buffer reads */
static bool zeros_ref(GlesSlot* slot, G2Ref* out) {
    G2Buffer* z = &g_draw.zeros;
    if (!z->data) {
        z->data = calloc(1, 65536);
        if (!z->data) return false;
        z->size = 65536;
        z->stale = true;
        z->used = true;
    }
    return gles2_buffer_ref(slot, z, false, out);
}

enum { MAX_DESCRIPTORS = 96 };

/* the program's set for the draw: written, or the last draw's when nothing changed */
static VkDescriptorSet descriptor_set(Gles2Context* gl, GlesSlot* slot, G2Program* p) {
    if (!gles2_program_defaults(slot, p)) return VK_NULL_HANDLE;
    VkDescriptorImageInfo images[MAX_DESCRIPTORS];
    VkDescriptorBufferInfo buffers[MAX_DESCRIPTORS];
    VkWriteDescriptorSet writes[64];
    uint32_t ni = 0, nb = 0, nw = 0;
    for (uint32_t i = 0; i < p->binding_count && nw < 64; ++i) {
        const G2Binding* b = &p->bindings[i];
        VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = b->binding,
                                  .descriptorCount = b->count};
        if (b->kind == SNX_GLSLC_REFLECT_SAMPLER && !b->texel_buffer) {
            if (ni + b->count > MAX_DESCRIPTORS) return VK_NULL_HANDLE;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &images[ni];
            for (uint32_t k = 0; k < b->count; ++k) {
                int32_t unit = p->units[b->first + k];
                G2Texture* t = gles2_unit_texture(gl, (unsigned)unit, b->dim);
                int levels = 0;
                VkImageView view = VK_NULL_HANDLE;
                VkSampler sampler = VK_NULL_HANDLE;
                VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                if (gles2_texture_complete(t, &levels)) {
                    static const VkImageViewType types[G2_TARGETS] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE,
                                                                      VK_IMAGE_VIEW_TYPE_3D,
                                                                      VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                                      VK_IMAGE_VIEW_TYPE_2D};
                    VkImageAspectFlags aspect = gles2_aspect(t->format);
                    if (aspect & VK_IMAGE_ASPECT_DEPTH_BIT) aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
                    if ((t->vk == VK_FORMAT_D24_UNORM_S8_UINT && t->format->kind == G2_STENCIL) ||
                        (t->stencil_mode && (depth_aspect_of(t->vk) & VK_IMAGE_ASPECT_STENCIL_BIT)))
                        aspect = VK_IMAGE_ASPECT_STENCIL_BIT;
                    view = gles2_texture_view(t, types[b->dim], t->base_level, levels, 0, t->layers, true, aspect);
                    const G2SamplerState* st = gles2_unit_sampler(gl, (unsigned)unit, t);
                    bool integer = t->format->kind == G2_INT || t->format->kind == G2_UINT ||
                                   aspect == VK_IMAGE_ASPECT_STENCIL_BIT;
                    sampler = gles2_vk_sampler(st, integer);
                    layout = t->layout;
                }
                if (!view || !sampler) {
                    if (!missing_ready(gl)) return VK_NULL_HANDLE;
                    view = g_gles2.missing[b->dim].view;
                    G2SamplerState nearest = {.min = GL_NEAREST_, .mag = GL_NEAREST_, .wrap = {0x812f, 0x812f, 0x812f},
                                              .compare_func = 0x0203, .max_lod = 1000, .anisotropy = 1};
                    sampler = gles2_vk_sampler(&nearest, false);
                    layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                }
                images[ni++] = (VkDescriptorImageInfo){sampler, view, layout};
            }
        } else if (b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK || b->kind == SNX_GLSLC_REFLECT_BUFFER_BLOCK) {
            if (nb + b->count > MAX_DESCRIPTORS) return VK_NULL_HANDLE;
            w.descriptorType = b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                          : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &buffers[nb];
            for (uint32_t k = 0; k < b->count; ++k) {
                VkDeviceSize size = b->size ? b->size : 16;
                if (!b->name) { /* a stage's default block */
                    buffers[nb++] = (VkDescriptorBufferInfo){p->default_ref[b->stage].buffer,
                                                             p->default_ref[b->stage].offset, size};
                    continue;
                }
                bool uniform = b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK;
                unsigned point = uniform ? (b->first + k < G2_UBO_BINDINGS ? p->block_binding[b->first + k] : 0)
                                         : b->first + k;
                const G2IndexedBuffer* x = uniform && point < G2_UBO_BINDINGS        ? &gl->uniform_buffers[point]
                                           : !uniform && point < G2_SSBO_BINDINGS ? &gl->storage_buffers[point]
                                                                                  : NULL;
                G2Buffer* buffer = x ? gles2_buffer(x->buffer) : NULL;
                G2Ref ref;
                VkDeviceSize offset = 0;
                if (buffer && buffer->size && (uint64_t)x->offset < buffer->size &&
                    gles2_buffer_ref(slot, buffer, b->kind == SNX_GLSLC_REFLECT_BUFFER_BLOCK, &ref)) {
                    offset = ref.offset + (VkDeviceSize)x->offset;
                    VkDeviceSize left = buffer->size - (VkDeviceSize)x->offset;
                    if (x->size > 0 && (VkDeviceSize)x->size < left) left = (VkDeviceSize)x->size;
                    if (b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && left > V.props.limits.maxUniformBufferRange)
                        left = V.props.limits.maxUniformBufferRange;
                    size = left < size && b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK ? size : left;
                    if (size > left) {
                        /* a buffer smaller than the block: the block reads zeros instead */
                        if (!zeros_ref(slot, &ref)) return VK_NULL_HANDLE;
                        offset = ref.offset;
                        size = b->size ? b->size : 16;
                    }
                } else if (zeros_ref(slot, &ref)) {
                    offset = ref.offset;
                } else {
                    return VK_NULL_HANDLE;
                }
                buffers[nb++] = (VkDescriptorBufferInfo){ref.buffer, offset, size};
            }
        } else if (b->kind == SNX_GLSLC_REFLECT_IMAGE && !b->texel_buffer) {
            if (ni + b->count > MAX_DESCRIPTORS) return VK_NULL_HANDLE;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            w.pImageInfo = &images[ni];
            for (uint32_t k = 0; k < b->count; ++k) {
                VkImageView view = gles2_image_unit(gl, b->first + k, b->dim);
                if (!view) {
                    gles2_warn(13, "an image uniform without a texture of its image unit: the draw is left out");
                    return VK_NULL_HANDLE;
                }
                images[ni++] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            }
        } else {
            gles2_warn(20, "texel buffers in shaders are not bound yet: the draw is left out");
            return VK_NULL_HANDLE;
        }
        writes[nw++] = w;
    }
    /* the set as the last draw had it: the same contents of the same layout in this frame */
    uint64_t hash = snx_fnv1a(SNX_FNV_BASIS, &p->set_layout, sizeof p->set_layout);
    hash = snx_fnv1a(hash, images, ni * sizeof *images);
    hash = snx_fnv1a(hash, buffers, nb * sizeof *buffers);
    if (g_draw.set && g_draw.set_hash == hash && g_draw.set_serial == gles_vk_pending()) return g_draw.set;
    VkDescriptorSet set = gles_vk_set(slot, p->set_layout);
    if (!set) return VK_NULL_HANDLE;
    for (uint32_t i = 0; i < nw; ++i) writes[i].dstSet = set;
    V.vk.vkUpdateDescriptorSets(V.device, nw, writes, 0, NULL);
    g_draw.set = set;
    g_draw.set_hash = hash;
    g_draw.set_serial = gles_vk_pending();
    return set;
}

/* ---- the draws ---- */

static VkCompareOp compare(unsigned f) {
    return (VkCompareOp)((f - 0x0200) & 7);
}
static VkStencilOp stencil_op(unsigned op) {
    switch (op) {
    case 0: return VK_STENCIL_OP_ZERO;
    case 0x1e01: return VK_STENCIL_OP_REPLACE;
    case 0x1e02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1e03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150a: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

/* GL's rectangle (x, y from the bottom left) as Vulkan's on the target, clamped to it */
static VkRect2D rect(const Target* t, int x, int y, int w, int h) {
    int32_t ty = t->flip ? (int32_t)t->extent.height - y - h : y;
    int32_t x0 = x < 0 ? 0 : x, y0 = ty < 0 ? 0 : ty;
    int32_t x1 = x + w, y1 = ty + h;
    if (x1 > (int32_t)t->extent.width) x1 = (int32_t)t->extent.width;
    if (y1 > (int32_t)t->extent.height) y1 = (int32_t)t->extent.height;
    if (x1 < x0) x1 = x0;
    if (y1 < y0) y1 = y0;
    return (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}

/* the dynamic state of the context's fixed functions */
static void dynamic_state(Gles2Context* gl, VkCommandBuffer cb, const Target* t, unsigned mode) {
    const int* vp = gl->viewport;
    VkViewport viewport = {(float)vp[0], (float)vp[1], (float)vp[2], (float)vp[3], gl->depth_range[0],
                           gl->depth_range[1]};
    if (t->flip) viewport.y = (float)t->extent.height - (float)vp[1], viewport.height = -(float)vp[3];
    if (viewport.width < 1) viewport.width = 1;
    if (viewport.height == 0) viewport.height = 1;
    V.vk.vkCmdSetViewport(cb, 0, 1, &viewport);
    VkRect2D scissor = gl->scissor_test ? rect(t, gl->scissor[0], gl->scissor[1], gl->scissor[2], gl->scissor[3])
                                        : (VkRect2D){{0, 0}, t->extent};
    V.vk.vkCmdSetScissor(cb, 0, 1, &scissor);
    float width = gl->line_width;
    if (!V.features.wideLines) width = 1;
    V.vk.vkCmdSetLineWidth(cb, width);
    V.vk.vkCmdSetDepthBiasEnable(cb, gl->polygon_offset);
    V.vk.vkCmdSetDepthBias(cb, gl->offset_units, 0, gl->offset_factor);
    V.vk.vkCmdSetBlendConstants(cb, gl->blend_color);
    VkCullModeFlags cull = !gl->cull ? VK_CULL_MODE_NONE
                           : gl->cull_face == GL_FRONT_ ? VK_CULL_MODE_FRONT_BIT
                           : gl->cull_face == GL_BACK_  ? VK_CULL_MODE_BACK_BIT
                                                        : VK_CULL_MODE_FRONT_AND_BACK;
    V.vk.vkCmdSetCullMode(cb, cull);
    bool ccw = gl->front_face == GL_CCW_;
    if (!t->flip) ccw = !ccw; /* drawn upright, the winding turns */
    V.vk.vkCmdSetFrontFace(cb, ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    V.vk.vkCmdSetPrimitiveTopology(cb, topology(mode));
    bool depth = t->depth_view && (t->depth_aspect & VK_IMAGE_ASPECT_DEPTH_BIT);
    bool stencil = t->depth_view && (depth_aspect_of(t->depth) & VK_IMAGE_ASPECT_STENCIL_BIT);
    V.vk.vkCmdSetDepthTestEnable(cb, depth && gl->depth_test);
    V.vk.vkCmdSetDepthWriteEnable(cb, depth && gl->depth_test && gl->depth_mask);
    V.vk.vkCmdSetDepthCompareOp(cb, compare(gl->depth_func));
    V.vk.vkCmdSetStencilTestEnable(cb, stencil && gl->stencil_test);
    for (int f = 0; f < 2; ++f) {
        VkStencilFaceFlags face = f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT;
        /* the front face is set to keep GL's (dynamic_state's winding), so GL's front is Vulkan's front */
        V.vk.vkCmdSetStencilOp(cb, face, stencil_op(gl->stencil[f].fail), stencil_op(gl->stencil[f].zpass),
                               stencil_op(gl->stencil[f].zfail), compare(gl->stencil[f].func));
        V.vk.vkCmdSetStencilCompareMask(cb, face, gl->stencil[f].mask);
        V.vk.vkCmdSetStencilWriteMask(cb, face, gl->stencil[f].write);
        V.vk.vkCmdSetStencilReference(cb, face, (uint32_t)gl->stencil[f].ref);
    }
    V.vk.vkCmdSetPrimitiveRestartEnable(cb, gl->primitive_restart && mode != GL_POINTS_ && mode != GL_LINES_ &&
                                                mode != GL_TRIANGLES_);
    V.vk.vkCmdSetRasterizerDiscardEnable(cb, gl->rasterizer_discard);
}

/* the blend's code in the key: GL's factor as an index of codes */
static uint8_t factor_code(unsigned f) {
    static const unsigned codes[15] = {0, 1, 0x300, 0x301, 0x302, 0x303, 0x304, 0x305,
                                       0x306, 0x307, 0x308, 0x8001, 0x8002, 0x8003, 0x8004};
    for (uint8_t i = 0; i < 15; ++i)
        if (codes[i] == f) return i;
    return 1;
}
static uint8_t equation_code(unsigned e) {
    return e == 0x800a ? 1 : e == 0x800b ? 2 : e == 0x8007 ? 3 : e == 0x8008 ? 4 : 0;
}

/* the indices of a draw: their type's size, and where they are */
typedef struct {
    unsigned type;       /* 0: none */
    uint64_t pointer;    /* the client's, or an offset into buffer */
    G2Buffer* buffer;
} Indices;

/* index i of the indices */
static uint32_t index_at(const unsigned char* p, unsigned type, uint32_t i) {
    if (type == GL_UNSIGNED_BYTE_) return p[i];
    if (type == GL_UNSIGNED_SHORT_) {
        uint16_t v;
        memcpy(&v, p + 2 * (size_t)i, 2);
        return v;
    }
    uint32_t v;
    memcpy(&v, p + 4 * (size_t)i, 4);
    return v;
}

/* a draw of count vertices (indices, with idx) from first, instances times; or the one an indirect buffer holds at
   indirect_offset (GLES 3.1), which reads vertex and index buffers alone */
static void draw(Gles2Context* gl, unsigned mode, int32_t first, int32_t count, int32_t instances, Indices idx,
                 int32_t base_vertex, G2Buffer* indirect, uint64_t indirect_offset) {
    if (mode > GL_TRIANGLE_FAN_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    if (count < 0 || first < 0 || instances < 0) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    G2Program* p = gles2_program(gl->program);
    GlesSurface* s = gles2_surface(gl);
    if (!p || !p->linked || !p->modules[0] || (!indirect && (!count || !instances)) || !s) return;
    if (!V.v13) return;
    if (!gles_frame_begin(s)) return;
    GlesSlot* slot = &s->slots[s->slot];
    /* the indices: their range of vertices, and their buffer */
    const unsigned char* bytes = NULL;
    uint32_t lo = (uint32_t)first, hi = (uint32_t)(first + count - 1);
    unsigned restart = idx.type == GL_UNSIGNED_BYTE_ ? 0xff : idx.type == GL_UNSIGNED_SHORT_ ? 0xffff : 0xffffffffu;
    if (indirect && (mode == GL_LINE_LOOP_ || (idx.type && !idx.buffer) ||
                     (idx.type == GL_UNSIGNED_BYTE_ && !V.index_uint8))) {
        gles2_warn(21, "an indirect draw of a line loop, client indices or bytes the device has no index type of");
        return;
    }
    if (idx.type && !indirect) {
        bytes = idx.buffer ? gles2_buffer_bytes(idx.buffer) + idx.pointer : (const unsigned char*)(uintptr_t)idx.pointer;
        uint32_t size = type_bytes(idx.type);
        if (!bytes || (idx.buffer && idx.pointer + (uint64_t)count * size > idx.buffer->size)) {
            gles2_error(gl, GL_INVALID_OPERATION_);
            return;
        }
        /* the vertices a client array copies: the indices' range (a buffer's arrays need none) */
        bool clients = false;
        G2VertexArray* vao = gles2_vertex_array(gl);
        for (int i = 0; i < G2_ATTRIBS; ++i)
            clients |= vao->attribs[i].enabled && !vao->bindings[vao->attribs[i].binding].buffer;
        lo = 0, hi = 0;
        if (clients) {
            lo = UINT32_MAX;
            for (int32_t i = 0; i < count; ++i) {
                uint32_t v = index_at(bytes, idx.type, (uint32_t)i);
                if (gl->primitive_restart && v == restart) continue;
                if (v < lo) lo = v;
                if (v > hi) hi = v;
            }
            if (lo == UINT32_MAX) lo = 0;
            lo += (uint32_t)base_vertex, hi += (uint32_t)base_vertex;
        }
    }
    G2PipelineKey key;
    memset(&key, 0, sizeof key); /* the key's bytes are hashed: its padding too */
    Vertices vertices;
    if (!vertex_inputs(gl, slot, p, lo, hi, (uint32_t)instances, &key, &vertices)) return;
    /* the index buffer: the GL buffer's, the client's copied, a line loop's made, or bytes widened */
    VkBuffer index_buffer = VK_NULL_HANDLE;
    VkDeviceSize index_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    uint32_t draw_count = (uint32_t)count;
    bool loop = mode == GL_LINE_LOOP_ && count > 1;
    if (!indirect && (loop || (idx.type == GL_UNSIGNED_BYTE_ && !V.index_uint8) || (idx.type && !idx.buffer))) {
        uint32_t n = (uint32_t)count + (loop ? 1 : 0);
        bool wide = loop || idx.type == GL_UNSIGNED_INT_ || idx.type == 0;
        uint32_t size = wide ? 4 : idx.type == GL_UNSIGNED_BYTE_ && V.index_uint8 ? 1 : 2;
        VkDeviceSize at = gles_vk_arena_take(slot, &slot->vertices, (VkDeviceSize)n * size + 4, 16, 4u << 20, 0);
        if (at == VK_WHOLE_SIZE) return;
        unsigned char* out = slot->vertices.map + at;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t k = i < (uint32_t)count ? i : 0;
            uint32_t v = idx.type ? index_at(bytes, idx.type, k) : (uint32_t)first + k;
            if (idx.type && gl->primitive_restart && v == restart) v = size == 4 ? 0xffffffffu : size == 2 ? 0xffff : 0xff;
            if (size == 4) memcpy(out + 4 * (size_t)i, &v, 4);
            else if (size == 2) {
                uint16_t h = (uint16_t)v;
                memcpy(out + 2 * (size_t)i, &h, 2);
            } else {
                out[i] = (unsigned char)v;
            }
        }
        index_buffer = slot->vertices.buffer;
        index_offset = at;
        index_type = size == 4 ? VK_INDEX_TYPE_UINT32 : size == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT8_EXT;
        draw_count = n;
        if (!idx.type) base_vertex = 0; /* the made indices count from first already */
    } else if (idx.type) {
        G2Ref ref;
        if (!gles2_buffer_ref(slot, idx.buffer, false, &ref)) return;
        index_buffer = ref.buffer;
        index_offset = ref.offset + idx.pointer;
        index_type = idx.type == GL_UNSIGNED_BYTE_   ? VK_INDEX_TYPE_UINT8_EXT
                     : idx.type == GL_UNSIGNED_SHORT_ ? VK_INDEX_TYPE_UINT16
                                                      : VK_INDEX_TYPE_UINT32;
        if (index_offset % type_bytes(idx.type)) {
            gles2_warn(14, "indices at an offset that is not a multiple of their size: the draw is left out");
            return;
        }
    }
    G2Ref command = {0};
    if (indirect && !gles2_buffer_ref(slot, indirect, false, &command)) return;
    VkDescriptorSet set = descriptor_set(gl, slot, p);
    if (!set) return;
    if (!gles2_rendering_begin(gl)) return;
    const Target* t = &g_draw.open;
    /* the pipeline's state */
    key.program = (uint32_t)(p - g_gles2.programs);
    key.generation = p->generation;
    for (int i = 0; i < G2_COLORS; ++i) key.colors[i] = (uint32_t)t->colors[i];
    key.depth = (uint32_t)t->depth;
    key.samples = (uint8_t)t->samples;
    key.topology = topology_class(mode);
    key.alpha_to_coverage = gl->alpha_to_coverage;
    key.clip_control = V.depth_clip_control;
    key.blend = gl->blend;
    for (int i = 0; i < G2_COLORS; ++i)
        if (t->integer[i]) key.blend |= (uint8_t)(1u << (4 + i));
    key.write_mask = (uint8_t)(gl->color_mask[0] | gl->color_mask[1] << 1 | gl->color_mask[2] << 2 |
                               gl->color_mask[3] << 3);
    if (gl->blend) {
        key.factors[0] = factor_code(gl->blend_src[0]);
        key.factors[1] = factor_code(gl->blend_dst[0]);
        key.factors[2] = factor_code(gl->blend_src[1]);
        key.factors[3] = factor_code(gl->blend_dst[1]);
        key.ops[0] = equation_code(gl->blend_eq[0]);
        key.ops[1] = equation_code(gl->blend_eq[1]);
    }
    key.sample_mask = gl->sample_mask_on ? gl->sample_mask : 0xffffffffu;
    if (gl->sample_coverage && t->samples > 1) {
        uint32_t covered = (uint32_t)(gl->coverage_value * (float)t->samples + 0.5f);
        uint32_t coverage = covered >= 32 ? 0xffffffffu : (1u << covered) - 1;
        key.sample_mask &= gl->coverage_invert ? ~coverage : coverage;
    }
    if (!V.depth_clip_control) gles2_warn(15, "the device has no VK_EXT_depth_clip_control: depth is [0, 1] of GL's");
    VkPipeline pl = pipeline(p, &key);
    if (!pl) return;
    VkCommandBuffer cb = slot->cb;
    V.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
    dynamic_state(gl, cb, t, mode);
    V.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0, 1, &set, 0, NULL);
    /* gl_FragCoord.y and gl_PointCoord.y as GL has them: the window's from the bottom, a framebuffer's points' t
       from the top */
    float flip[4] = {-1, (float)t->extent.height, 1, 0};
    if (!t->flip) flip[0] = 1, flip[1] = 0, flip[2] = -1, flip[3] = 1;
    V.vk.vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof flip, flip);
    for (uint32_t loc = 0; loc < G2_ATTRIBS; ++loc)
        if (vertices.used & (1u << loc))
            V.vk.vkCmdBindVertexBuffers2(cb, loc, 1, &vertices.buffers[loc], &vertices.offsets[loc], NULL,
                                         &vertices.strides[loc]);
    if (indirect) {
        if (index_buffer) V.vk.vkCmdBindIndexBuffer(cb, index_buffer, index_offset, index_type);
        if (index_buffer) V.vk.vkCmdDrawIndexedIndirect(cb, command.buffer, command.offset + indirect_offset, 1, 20);
        else V.vk.vkCmdDrawIndirect(cb, command.buffer, command.offset + indirect_offset, 1, 16);
    } else if (index_buffer) {
        V.vk.vkCmdBindIndexBuffer(cb, index_buffer, index_offset, index_type);
        V.vk.vkCmdDrawIndexed(cb, draw_count, (uint32_t)instances, 0, base_vertex, 0);
    } else {
        V.vk.vkCmdDraw(cb, draw_count, (uint32_t)instances, (uint32_t)first, 0);
    }
}

static Indices indices_of(Gles2Context* gl, unsigned type, uint64_t pointer) {
    G2Buffer* b = gles2_buffer(gles2_vertex_array(gl)->elements);
    if (type != GL_UNSIGNED_BYTE_ && type != GL_UNSIGNED_SHORT_ && type != GL_UNSIGNED_INT_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return (Indices){0};
    }
    return (Indices){type, pointer, b};
}

static void g_draw_arrays(CPU* c, Gles2Context* gl) {
    draw(gl, (unsigned)X(0), (int32_t)X(1), (int32_t)X(2), 1, (Indices){0}, 0, NULL, 0);
}
static void g_draw_arrays_instanced(CPU* c, Gles2Context* gl) {
    draw(gl, (unsigned)X(0), (int32_t)X(1), (int32_t)X(2), (int32_t)X(3), (Indices){0}, 0, NULL, 0);
}
static void g_draw_elements(CPU* c, Gles2Context* gl) {
    Indices i = indices_of(gl, (unsigned)X(2), X(3));
    if (i.type) draw(gl, (unsigned)X(0), 0, (int32_t)X(1), 1, i, 0, NULL, 0);
}
static void g_draw_elements_instanced(CPU* c, Gles2Context* gl) {
    Indices i = indices_of(gl, (unsigned)X(2), X(3));
    if (i.type) draw(gl, (unsigned)X(0), 0, (int32_t)X(1), (int32_t)X(4), i, 0, NULL, 0);
}
/* glDrawRangeElements(mode, start, end, count, type, indices) */
static void g_draw_range_elements(CPU* c, Gles2Context* gl) {
    Indices i = indices_of(gl, (unsigned)X(4), X(5));
    if (i.type) draw(gl, (unsigned)X(0), 0, (int32_t)X(3), 1, i, 0, NULL, 0);
}

/* glDrawArraysIndirect(mode, indirect) and glDrawElementsIndirect(mode, type, indirect): the command in the draw
   indirect buffer, at the offset indirect */
static void g_draw_arrays_indirect(CPU* c, Gles2Context* gl) {
    G2Buffer* b = gles2_buffer(gl->draw_indirect);
    if (!b || X(1) + 16 > b->size || X(1) % 4) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    draw(gl, (unsigned)X(0), 0, 0, 0, (Indices){0}, 0, b, X(1));
}
static void g_draw_elements_indirect(CPU* c, Gles2Context* gl) {
    G2Buffer* b = gles2_buffer(gl->draw_indirect);
    if (!b || X(2) + 20 > b->size || X(2) % 4) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    Indices i = indices_of(gl, (unsigned)X(1), 0);
    if (i.type) draw(gl, (unsigned)X(0), 0, 0, 0, i, 0, b, X(2));
}

/* ---- compute ---- */

/* glDispatchCompute(x, y, z), or the counts the dispatch indirect buffer holds at offset: outside rendering, with
   the writes before visible to the dispatch and its writes to what follows (GLES 3.1 asks a glMemoryBarrier for
   those; one here costs little and keeps the GPU's order) */
static void dispatch(Gles2Context* gl, uint32_t x, uint32_t y, uint32_t z, G2Buffer* indirect, uint64_t offset) {
    G2Program* p = gles2_program(gl->program);
    if (!p || !p->linked || !p->modules[2]) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    if (!V.v13) return;
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (!slot) return;
    if (!p->compute) {
        VkComputePipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                      .module = p->modules[2],
                                                      .pName = "main"},
                                            .layout = p->layout};
        if (V.vk.vkCreateComputePipelines(V.device, VK_NULL_HANDLE, 1, &info, NULL, &p->compute) != VK_SUCCESS) {
            p->compute = VK_NULL_HANDLE;
            gles2_warn(22, "Vulkan does not make a compute pipeline of a program");
            return;
        }
    }
    G2Ref command = {0};
    if (indirect && !gles2_buffer_ref(slot, indirect, false, &command)) return;
    VkDescriptorSet set = descriptor_set(gl, slot, p);
    if (!set) return;
    /* descriptor_set may have recorded copies (a texture made for a sampler): the slot's frame is the same */
    VkCommandBuffer cb = slot->cb;
    gles2_memory_barrier(cb);
    V.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->compute);
    V.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
    if (indirect) V.vk.vkCmdDispatchIndirect(cb, command.buffer, command.offset + offset);
    else V.vk.vkCmdDispatch(cb, x, y, z);
    gles2_memory_barrier(cb);
}
static void g_dispatch_compute(CPU* c, Gles2Context* gl) {
    uint32_t x = (uint32_t)X(0), y = (uint32_t)X(1), z = (uint32_t)X(2);
    const uint32_t* max = V.props.limits.maxComputeWorkGroupCount;
    if (x > max[0] || y > max[1] || z > max[2]) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    if (x && y && z) dispatch(gl, x, y, z, NULL, 0);
}
static void g_dispatch_compute_indirect(CPU* c, Gles2Context* gl) {
    G2Buffer* b = gles2_buffer(gl->dispatch_indirect);
    if (!b || X(0) + 12 > b->size || X(0) % 4) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    dispatch(gl, 0, 0, 0, b, X(0));
}

/* ---- clears ---- */

/* the target's attachments cleared in the scissor's rectangle: colors (drawbuffer -1: every one), depth, stencil */
static void clear(Gles2Context* gl, int drawbuffer, const VkClearColorValue* color, bool depth, float depth_value,
                  bool stencil, int stencil_value) {
    if (!gles2_rendering_begin(gl)) return;
    const Target* t = &g_draw.open;
    GlesSurface* s = gl->draw;
    VkClearAttachment at[G2_COLORS + 1];
    uint32_t n = 0;
    if (color) {
        bool partial = !(gl->color_mask[0] && gl->color_mask[1] && gl->color_mask[2] && gl->color_mask[3]);
        if (partial && (gl->color_mask[0] || gl->color_mask[1] || gl->color_mask[2] || gl->color_mask[3]))
            gles2_warn(16, "glClear with a partial color mask clears every channel");
        for (int i = 0; i < G2_COLORS && (gl->color_mask[0] || gl->color_mask[1] || gl->color_mask[2] ||
                                          gl->color_mask[3]); ++i) {
            if (!t->color_views[i] || (drawbuffer >= 0 && i != drawbuffer)) continue;
            at[n++] = (VkClearAttachment){VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)i, {.color = *color}};
        }
    }
    VkImageAspectFlags aspect = 0;
    if (depth && gl->depth_mask && t->depth_view && (t->depth_aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (stencil && t->depth_view && (depth_aspect_of(t->depth) & VK_IMAGE_ASPECT_STENCIL_BIT) && gl->stencil[0].write)
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (aspect)
        at[n++] = (VkClearAttachment){aspect, 0, {.depthStencil = {depth_value, (uint32_t)stencil_value}}};
    VkClearRect r = {gl->scissor_test ? rect(t, gl->scissor[0], gl->scissor[1], gl->scissor[2], gl->scissor[3])
                                      : (VkRect2D){{0, 0}, t->extent},
                     0, 1};
    if (n && r.rect.extent.width && r.rect.extent.height)
        V.vk.vkCmdClearAttachments(s->slots[s->slot].cb, n, at, 1, &r);
}
static void g_clear(CPU* c, Gles2Context* gl) {
    unsigned mask = (unsigned)X(0);
    VkClearColorValue color = {{gl->clear_color[0], gl->clear_color[1], gl->clear_color[2], gl->clear_color[3]}};
    if (!(mask & (GL_COLOR_BUFFER_BIT_ | GL_DEPTH_BUFFER_BIT_ | GL_STENCIL_BUFFER_BIT_))) return;
    clear(gl, -1, mask & GL_COLOR_BUFFER_BIT_ ? &color : NULL, (mask & GL_DEPTH_BUFFER_BIT_) != 0, gl->clear_depth,
          (mask & GL_STENCIL_BUFFER_BIT_) != 0, gl->clear_stencil);
}
/* glClearBuffer{fv,iv,uiv}(buffer, drawbuffer, value) and glClearBufferfi(buffer, drawbuffer, depth, stencil) */
static void clear_buffer(CPU* c, Gles2Context* gl, char kind) {
    unsigned buffer = (unsigned)X(0);
    int drawbuffer = (int)X(1);
    const unsigned char* v = PTR(2);
    if (buffer == GL_COLOR_ && v) {
        VkClearColorValue color;
        memcpy(&color, v, 16); /* floats, ints or uints: the same 16 bytes */
        (void)kind;
        clear(gl, drawbuffer, &color, false, 0, false, 0);
    } else if (buffer == GL_DEPTH_ && v) {
        float d;
        memcpy(&d, v, 4);
        clear(gl, -1, NULL, true, d, false, 0);
    } else if (buffer == GL_STENCIL_ && v) {
        int32_t st;
        memcpy(&st, v, 4);
        clear(gl, -1, NULL, false, 0, true, st);
    }
}
static void g_clear_bufferfv(CPU* c, Gles2Context* gl) {
    clear_buffer(c, gl, 'f');
}
static void g_clear_bufferiv(CPU* c, Gles2Context* gl) {
    clear_buffer(c, gl, 'i');
}
static void g_clear_bufferuiv(CPU* c, Gles2Context* gl) {
    clear_buffer(c, gl, 'u');
}
static void g_clear_bufferfi(CPU* c, Gles2Context* gl) {
    if ((unsigned)X(0) == GL_DEPTH_STENCIL_) clear(gl, -1, NULL, true, hle_f(c, 0), true, (int)X(2));
}

/* ---- copies between images ---- */

/* the read framebuffer's color image as a transfer source: its image, aspect, layout at rest, format, size */
typedef struct {
    VkImage image;
    VkFormat format;
    VkImageLayout rest;
    VkExtent2D extent;
    bool flip;
    VkSampleCountFlagBits samples;
    uint32_t level, layer;
} Source;

static bool read_source(Gles2Context* gl, Source* out, bool depth) {
    *out = (Source){0};
    G2Framebuffer* fb = gles2_framebuffer(gl->read_fbo);
    if (!gl->read_fbo || !fb) {
        GlesSurface* s = gl->read;
        if (!s || !gles_frame_begin(s)) return false;
        if (depth) {
            if (!s->depth || !s->depth_ready) return false;
            *out = (Source){s->depth, s->depth_format, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, s->extent,
                            true, VK_SAMPLE_COUNT_1_BIT, 0, 0};
            return true;
        }
        if (s->pbuffer) {
            if (!s->own_ready) return false;
            *out = (Source){s->images[0], s->format, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, s->extent, true,
                            VK_SAMPLE_COUNT_1_BIT, 0, 0};
            return true;
        }
        if (!s->acquired || !s->touched) return false; /* nothing drawn into the frame's image yet */
        *out = (Source){s->images[s->image], s->format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, s->extent, true,
                        VK_SAMPLE_COUNT_1_BIT, 0, 0};
        return true;
    }
    const G2Attachment* a = depth ? &fb->at[G2_DEPTH_ATTACHMENT]
                                  : fb->read >= GL_COLOR_ATTACHMENT0_ && fb->read < GL_COLOR_ATTACHMENT0_ + G2_COLORS
                                        ? &fb->at[fb->read - GL_COLOR_ATTACHMENT0_]
                                        : NULL;
    if (!a || !a->kind) return false;
    VkImageView view;
    bool integer;
    if (!attachment(a, &out->image, &view, &out->format, &out->rest, &out->extent, &out->samples, &integer))
        return false;
    if (out->rest == VK_IMAGE_LAYOUT_UNDEFINED) return false;
    out->level = a->kind == 1 ? (uint32_t)a->level : 0;
    out->layer = a->kind == 1 ? (uint32_t)a->layer : 0;
    return true;
}

static VkImageAspectFlags aspect_of_format(VkFormat f) {
    return f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D32_SFLOAT ||
                   f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT ||
                   f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_S8_UINT
               ? depth_aspect_of(f)
               : VK_IMAGE_ASPECT_COLOR_BIT;
}

/* a region of the source blitted into an image region: GL's y from the bottom of a flipped source and target */
static void blit(VkCommandBuffer cb, const Source* src, int sx0, int sy0, int sx1, int sy1, VkImage dst,
                 VkImageLayout dst_from, VkImageLayout dst_rest, bool dst_flip, VkExtent2D dst_extent, uint32_t dst_level, uint32_t dst_layer,
                 int dx0, int dy0, int dx1, int dy1, VkFilter filter, VkImageAspectFlags aspect) {
    if (src->flip) sy0 = (int)src->extent.height - sy0, sy1 = (int)src->extent.height - sy1;
    if (dst_flip) dy0 = (int)dst_extent.height - dy0, dy1 = (int)dst_extent.height - dy1;
    VkImageLayout from_src = src->rest;
    gles_image_barrier(cb, src->image, aspect, from_src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                       VK_ACCESS_TRANSFER_READ_BIT, ATTACHMENT_STAGES | ALL_READERS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (dst != src->image)
        gles_image_barrier(cb, dst, aspect, dst_from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                           ATTACHMENT_STAGES | ALL_READERS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageBlit region = {.srcSubresource = {aspect, src->level, src->layer, 1},
                          .srcOffsets = {{sx0, sy0, 0}, {sx1, sy1, 1}},
                          .dstSubresource = {aspect, dst_level, dst_layer, 1},
                          .dstOffsets = {{dx0, dy0, 0}, {dx1, dy1, 1}}};
    bool same_size = sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0;
    if (src->samples > 1 && same_size) {
        VkImageResolve resolve = {.srcSubresource = region.srcSubresource,
                                  .srcOffset = {sx0 < sx1 ? sx0 : sx1, sy0 < sy1 ? sy0 : sy1, 0},
                                  .dstSubresource = region.dstSubresource,
                                  .dstOffset = {dx0 < dx1 ? dx0 : dx1, dy0 < dy1 ? dy0 : dy1, 0},
                                  .extent = {(uint32_t)abs(sx1 - sx0), (uint32_t)abs(sy1 - sy0), 1}};
        V.vk.vkCmdResolveImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                               dst == src->image ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &resolve);
    } else {
        V.vk.vkCmdBlitImage(cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                            dst == src->image ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                            1, &region, filter);
    }
    gles_image_barrier(cb, src->image, aspect, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, from_src,
                       VK_ACCESS_TRANSFER_READ_BIT,
                       ATTACHMENT_ACCESS | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, ATTACHMENT_STAGES | ALL_READERS);
    if (dst != src->image)
        gles_image_barrier(cb, dst, aspect, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_rest, VK_ACCESS_TRANSFER_WRITE_BIT,
                           ATTACHMENT_ACCESS | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, ATTACHMENT_STAGES | ALL_READERS);
}

/* glBlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter) */
static void g_blit_framebuffer(CPU* c, Gles2Context* gl) {
    int sx0 = (int)X(0), sy0 = (int)X(1), sx1 = (int)X(2), sy1 = (int)X(3);
    int dx0 = (int)X(4), dy0 = (int)X(5), dx1 = (int)X(6), dy1 = (int)X(7);
    unsigned mask = (unsigned)gles2_arg(c, 8), filter = (unsigned)gles2_arg(c, 9);
    Target dst;
    if (!target_of(gl, gl->draw_fbo, false, &dst)) {
        gles2_error(gl, GL_INVALID_FRAMEBUFFER_OPERATION_);
        return;
    }
    GlesSlot* slot = gles2_transfer_slot(gl);
    GlesSurface* s = gl->draw;
    if (!slot) return;
    if (dst.flip && !s->pbuffer) {
        if (!gles_acquire(s)) return;
        if (!s->touched) gles2_rendering_begin(gl), gles2_rendering_stop(gl); /* the image cleared, at its rest */
        dst.color_images[0] = s->images[s->image];
    } else if (dst.flip) {
        if (!s->own_ready) gles2_rendering_begin(gl), gles2_rendering_stop(gl);
        dst.color_images[0] = s->images[0];
    }
    VkCommandBuffer cb = s->slots[s->slot].cb;
    Source src;
    if ((mask & GL_COLOR_BUFFER_BIT_) && read_source(gl, &src, false)) {
        for (int i = 0; i < G2_COLORS; ++i) {
            if (!dst.color_images[i]) continue;
            G2Framebuffer* fb = gles2_framebuffer(gl->draw_fbo);
            uint32_t level = 0, layer = 0;
            if (fb && gl->draw_fbo) {
                unsigned which = fb->draw[i];
                const G2Attachment* a = &fb->at[which - GL_COLOR_ATTACHMENT0_];
                level = a->kind == 1 ? (uint32_t)a->level : 0;
                layer = a->kind == 1 ? (uint32_t)a->layer : 0;
            }
            VkImageLayout rest = dst.color_rest[i] ? dst.color_rest[i] : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            blit(cb, &src, sx0, sy0, sx1, sy1, dst.color_images[i], rest, rest, dst.flip, dst.extent, level, layer, dx0, dy0,
                 dx1, dy1, filter == GL_NEAREST_ ? VK_FILTER_NEAREST : VK_FILTER_LINEAR, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }
    if ((mask & (GL_DEPTH_BUFFER_BIT_ | GL_STENCIL_BUFFER_BIT_)) && dst.depth_image && read_source(gl, &src, true)) {
        VkImageAspectFlags aspect = aspect_of_format(src.format) & aspect_of_format(dst.depth);
        if (src.format == dst.depth && aspect)
            blit(cb, &src, sx0, sy0, sx1, sy1, dst.depth_image,
                 dst.depth_rest ? dst.depth_rest : attachment_layout(true),
                 dst.depth_rest ? dst.depth_rest : attachment_layout(true), dst.flip, dst.extent, 0, 0, dx0, dy0, dx1,
                 dy1, VK_FILTER_NEAREST, aspect);
    }
}

/* glReadPixels(x, y, width, height, format, type, pixels): the GPU blits the rectangle into an RGBA8 (or BGRA8)
   image upright, copies it into host-cached memory, and the CPU copies the rows from there into pixels */
static void g_read_pixels(CPU* c, Gles2Context* gl) {
    gles2_read_pixels(gl, (int)X(0), (int)X(1), (int)X(2), (int)X(3), (unsigned)X(4), (unsigned)X(5), X(6));
}
void gles2_read_pixels(Gles2Context* gl, int x, int y, int width, int height, unsigned format, unsigned type,
                       uint64_t pixels) {
    if (width < 0 || height < 0) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    if ((format != GL_RGBA_ && format != GL_BGRA_EXT_) || type != GL_UNSIGNED_BYTE_) {
        gles2_warn(17, "glReadPixels of a format that is not GL_RGBA or GL_BGRA_EXT of bytes");
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    if (!width || !height) return;
    G2Buffer* pack = gles2_buffer(gl->pixel_pack);
    if (!pixels && !pack) return;
    GlesSlot* slot = gles2_transfer_slot(gl);
    Source src;
    if (!slot || !read_source(gl, &src, false)) {
        gles2_warn(18, "glReadPixels before anything was drawn: zeros");
        if (!pack) memset((void*)(uintptr_t)pixels, 0, (size_t)width * (size_t)height * 4);
        return;
    }
    /* the rectangle inside the source; what is outside it stays as the guest had it */
    VkFormat format_vk = format == GL_RGBA_ ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format_vk,
                              .extent = {(uint32_t)width, (uint32_t)height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    GlesImage temp;
    if (!gles_vk_image(&info, VK_IMAGE_VIEW_TYPE_MAX_ENUM, 0, &temp)) {
        gles2_error(gl, GL_OUT_OF_MEMORY_);
        return;
    }
    VkDeviceSize bytes = (VkDeviceSize)width * (VkDeviceSize)height * 4;
    G2Buffer host = {0};
    host.size = (size_t)bytes;
    VkCommandBuffer cb = slot->cb;
    /* GL's rows from the bottom: the source's row y lands in the temporary image's row 0 */
    blit(cb, &src, x, y, x + width, y + height, temp.image, VK_IMAGE_LAYOUT_UNDEFINED,
         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, false, (VkExtent2D){(uint32_t)width, (uint32_t)height}, 0, 0, 0, 0,
         width, height, VK_FILTER_NEAREST, VK_IMAGE_ASPECT_COLOR_BIT);
    G2Ref ref;
    host.data = NULL;
    host.used = true;
    unsigned char* zero = calloc(1, (size_t)bytes);
    host.data = zero;
    bool ok = zero && gles2_buffer_ref(slot, &host, true, &ref);
    if (ok) {
        VkBufferImageCopy copy = {.bufferOffset = ref.offset,
                                  .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                  .imageExtent = {(uint32_t)width, (uint32_t)height, 1}};
        V.vk.vkCmdCopyImageToBuffer(cb, temp.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ref.buffer, 1, &copy);
        VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
        V.vk.vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                                  NULL);
        gles_flush(gl->draw);
        const unsigned char* rows = gles2_buffer_bytes(&host);
        /* GL's pack layout: the rows from the bottom, each at the pack alignment */
        size_t row = (size_t)width * 4;
        size_t length = (size_t)(gl->pixel[0].row_length ? gl->pixel[0].row_length : width) * 4;
        size_t align = (size_t)gl->pixel[0].alignment;
        size_t stride = (length + align - 1) / align * align;
        unsigned char* out = pack ? pack->data : (unsigned char*)(uintptr_t)pixels;
        size_t offset = pack ? (size_t)pixels : 0;
        offset += (size_t)gl->pixel[0].skip_rows * stride + (size_t)gl->pixel[0].skip_pixels * 4;
        if (!pack || offset + stride * (size_t)(height - 1) + row <= pack->size)
            for (int r = 0; r < height; ++r) memcpy(out + offset + (size_t)r * stride, rows + (size_t)r * row, row);
        if (pack) {
            pack->stale = true;
            pack->arena_serial = 0;
        }
    }
    if (host.buffer) gles_vk_retire((GlesGarbage){.buffer = host.buffer, .memory = host.memory});
    free(zero);
    gles_vk_retire((GlesGarbage){.image = temp.image, .memory = temp.memory});
}

/* glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height): the read framebuffer's rectangle
   blitted into the texture's level, upright */
static void copy_tex(Gles2Context* gl, unsigned target, int level, int dx, int dy, int layer, int x, int y, int w,
                     int h) {
    int face = 0;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X_ && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + 6) {
        face = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X_);
        target = 0x8513;
    }
    unsigned index = target == GL_TEXTURE_2D_ ? G2_T2D : target == 0x8513 ? G2_TCUBE : target == 0x806f ? G2_T3D : G2_TARRAY;
    G2Texture* t = gles2_unit_texture(gl, gl->active, index);
    if (!t || !t->image.image || level >= t->levels) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    GlesSlot* slot = gles2_transfer_slot(gl);
    Source src;
    if (!slot || !read_source(gl, &src, false) || w <= 0 || h <= 0) return;
    VkExtent2D extent = {(uint32_t)(t->width >> level), (uint32_t)(t->height >> level)};
    blit(slot->cb, &src, x, y, x + w, y + h, t->image.image, t->layout, t->layout, false, extent, (uint32_t)level,
         (uint32_t)(index == G2_TCUBE ? face : layer), dx, dy, dx + w, dy + h, VK_FILTER_NEAREST,
         VK_IMAGE_ASPECT_COLOR_BIT);
    t->defined |= 1u << level;
}
static void g_copy_tex_sub_image_2d(CPU* c, Gles2Context* gl) {
    copy_tex(gl, (unsigned)X(0), (int)X(1), (int)X(2), (int)X(3), 0, (int)X(4), (int)X(5), (int)X(6), (int)X(7));
}
static void g_copy_tex_sub_image_3d(CPU* c, Gles2Context* gl) {
    copy_tex(gl, (unsigned)X(0), (int)X(1), (int)X(2), (int)X(3), (int)X(4), (int)X(5), (int)X(6), (int)X(7),
             (int)gles2_arg(c, 8));
}
/* glCopyTexImage2D(target, level, internal, x, y, width, height, border): the level made, then copied into */
static void g_copy_tex_image_2d(CPU* c, Gles2Context* gl) {
    unsigned target = (unsigned)X(0);
    int level = (int)X(1), w = (int)X(5), h = (int)X(6);
    unsigned internal = (unsigned)X(2);
    int face = target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X_ && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + 6;
    unsigned index = face ? G2_TCUBE : G2_T2D;
    G2Texture* t = gles2_unit_texture(gl, gl->active, index);
    if (!t || w < 0 || h < 0) {
        gles2_error(gl, t ? GL_INVALID_VALUE_ : GL_INVALID_OPERATION_);
        return;
    }
    /* the read framebuffer's own format when the internal format is unsized: 4 bytes */
    const G2Format* f = gles2_format(internal, internal == 0x1907 ? 0x1907 : 0x1908, GL_UNSIGNED_BYTE_);
    if (!f) f = gles2_format(0x1908, 0x1908, GL_UNSIGNED_BYTE_);
    int bw = w << level, bh = h << level;
    if (!t->image.image || t->format != f || (t->width >> level) != w || (t->height >> level) != h) {
        int m = bw > bh ? bw : bh, levels = 1;
        while (m > 1) m >>= 1, ++levels;
        if (!gles2_texture_storage(gl, t, f, bw, bh, 1, levels)) return;
    }
    t->defined |= 1u << level;
    copy_tex(gl, target, level, 0, 0, 0, (int)X(3), (int)X(4), w, h);
}

const HleSymbol hle_gles2_draw[] = {
    FN("glDrawArrays", g_draw_arrays),
    FN("glDrawElements", g_draw_elements),
    FN("glDrawRangeElements", g_draw_range_elements),
    FN("glDrawArraysInstanced", g_draw_arrays_instanced),
    FN("glDrawArraysInstancedEXT", g_draw_arrays_instanced),
    FN("glDrawArraysInstancedANGLE", g_draw_arrays_instanced),
    FN("glDrawElementsInstanced", g_draw_elements_instanced),
    FN("glDrawElementsInstancedEXT", g_draw_elements_instanced),
    FN("glDrawElementsInstancedANGLE", g_draw_elements_instanced),
    FN("glDrawArraysIndirect", g_draw_arrays_indirect),
    FN("glDrawElementsIndirect", g_draw_elements_indirect),
    FN("glDispatchCompute", g_dispatch_compute),
    FN("glDispatchComputeIndirect", g_dispatch_compute_indirect),
    FN("glClear", g_clear),
    FN("glClearBufferfv", g_clear_bufferfv),
    FN("glClearBufferiv", g_clear_bufferiv),
    FN("glClearBufferuiv", g_clear_bufferuiv),
    FN("glClearBufferfi", g_clear_bufferfi),
    FN("glBlitFramebuffer", g_blit_framebuffer),
    FN("glBlitFramebufferANGLE", g_blit_framebuffer),
    FN("glReadPixels", g_read_pixels),
    FN("glCopyTexImage2D", g_copy_tex_image_2d),
    FN("glCopyTexSubImage2D", g_copy_tex_sub_image_2d),
    FN("glCopyTexSubImage3D", g_copy_tex_sub_image_3d),
    {"", NULL, NULL},
};
