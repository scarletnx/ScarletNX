/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* libGLESv1_CM as GLES 1.1's fixed function over Vulkan (docs/platforms/android-apps/native.md, "EGL and GLES"), on the device and
   the surfaces that it shares with GLES 2 and 3 (gles_vk.h).

   Each draw goes into a render pass on the surface's image, and a texture upload ends the pass, copies from a
   staging buffer and starts it again (loading what it drew), so each draw reads the texture as it was at the call.
   The client arrays of a draw are converted to one vertex format (position, texture coordinate, color, all floats)
   in a buffer of the frame, as triangle, line or point lists. GL's window origin is the bottom left: the viewport has
   a negative height (Vulkan 1.1), so GL's coordinates, the scissor's and glDrawTexOES's are GL's. What a frame used
   (a replaced texture's image, a staging buffer that grew) is destroyed when the frame's fence is signaled.

   What is here: textures (2D, the base level, the formats that Vulkan samples as they are), the texture environment
   of unit 0, the matrix stacks, the client arrays, glDrawArrays, glDrawElements, glDrawTexOES, blending, the
   scissor, glClear. Depth, stencil, lighting, fog, alpha test, mipmaps and the second texture unit are not: their
   state is kept and has no effect */
#include "android/bridge/hle_gles.h"
#include "android/bridge/hle.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/gles_vk.h"
#include "android/bridge/gles_frag_spv.h"
#include "android/bridge/gles_rgb_comp_spv.h"
#include "android/bridge/gles_vert_spv.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/* ---- GL's numbers ---- */
enum {
    GL_NO_ERROR_ = 0,
    GL_INVALID_ENUM_ = 0x0500,
    GL_INVALID_VALUE_ = 0x0501,
    GL_INVALID_OPERATION_ = 0x0502,
    GL_STACK_OVERFLOW_ = 0x0503,
    GL_STACK_UNDERFLOW_ = 0x0504,
    GL_OUT_OF_MEMORY_ = 0x0505,
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
    GL_UNSIGNED_INT_ = 0x1405,
    GL_FLOAT_ = 0x1406,
    GL_FIXED_ = 0x140c,
    GL_TEXTURE_2D_ = 0x0de1,
    GL_BLEND_ = 0x0be2,
    GL_SCISSOR_TEST_ = 0x0c11,
    GL_VERTEX_ARRAY_ = 0x8074,
    GL_NORMAL_ARRAY_ = 0x8075,
    GL_COLOR_ARRAY_ = 0x8076,
    GL_TEXTURE_COORD_ARRAY_ = 0x8078,
    GL_MODELVIEW_ = 0x1700,
    GL_PROJECTION_ = 0x1701,
    GL_TEXTURE_ = 0x1702,
    GL_ALPHA_ = 0x1906,
    GL_RGB_ = 0x1907,
    GL_RGBA_ = 0x1908,
    GL_LUMINANCE_ = 0x1909,
    GL_LUMINANCE_ALPHA_ = 0x190a,
    GL_BGRA_EXT_ = 0x80e1,
    GL_UNSIGNED_SHORT_4_4_4_4_ = 0x8033,
    GL_UNSIGNED_SHORT_5_5_5_1_ = 0x8034,
    GL_UNSIGNED_SHORT_5_6_5_ = 0x8363,
    GL_TEXTURE_MAG_FILTER_ = 0x2800,
    GL_TEXTURE_MIN_FILTER_ = 0x2801,
    GL_TEXTURE_WRAP_S_ = 0x2802,
    GL_TEXTURE_WRAP_T_ = 0x2803,
    GL_NEAREST_ = 0x2600,
    GL_LINEAR_ = 0x2601,
    GL_NEAREST_MIPMAP_NEAREST_ = 0x2700,
    GL_REPEAT_ = 0x2901,
    GL_CLAMP_TO_EDGE_ = 0x812f,
    GL_MIRRORED_REPEAT_ = 0x8370,
    GL_TEXTURE_CROP_RECT_OES_ = 0x8b9d,
    GL_TEXTURE_ENV_ = 0x2300,
    GL_TEXTURE_ENV_MODE_ = 0x2200,
    GL_MODULATE_ = 0x2100,
    GL_DECAL_ = 0x2101,
    GL_REPLACE_ = 0x1e01,
    GL_ADD_ = 0x0104,
    GL_UNPACK_ALIGNMENT_ = 0x0cf5,
    GL_PACK_ALIGNMENT_ = 0x0d05,
    GL_COLOR_BUFFER_BIT_ = 0x4000,
    GL_TEXTURE0_ = 0x84c0,
    GL_VIEWPORT_ = 0x0ba2,
    GL_MAX_TEXTURE_SIZE_ = 0x0d33,
    GL_MAX_TEXTURE_UNITS_ = 0x84e2,
    GL_TEXTURE_BINDING_2D_ = 0x8069,
    GL_MATRIX_MODE_ = 0x0ba0,
    GL_MAX_MODELVIEW_STACK_DEPTH_ = 0x0d36,
    GL_MAX_PROJECTION_STACK_DEPTH_ = 0x0d38,
    GL_MAX_TEXTURE_STACK_DEPTH_ = 0x0d39,
    GL_MAX_VIEWPORT_DIMS_ = 0x0d3a,
    GL_RED_BITS_ = 0x0d52,
    GL_GREEN_BITS_ = 0x0d53,
    GL_BLUE_BITS_ = 0x0d54,
    GL_ALPHA_BITS_ = 0x0d55,
    GL_DEPTH_BITS_ = 0x0d56,
    GL_STENCIL_BITS_ = 0x0d57,
    GL_IMPLEMENTATION_COLOR_READ_TYPE_OES_ = 0x8b9a,
    GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES_ = 0x8b9b,
};

enum { STACK = 32, PIPELINE_KEYS = 512 };

/* a vertex as the shaders take it */
typedef struct {
    float position[4];
    float uv[2];
    float color[4];
} Vertex;

typedef struct {
    bool on;
    int size;
    unsigned type;
    int stride;
    uint64_t pointer;
} ClientArray;

struct GlesContext {
    unsigned error;
    int mode; /* the matrix mode: 0 modelview, 1 projection, 2 texture */
    float stacks[3][STACK][16];
    int depth[3];
    float color[4], clear[4];
    bool blend, texture2d, scissor_test, sized;
    unsigned src, dst;
    ClientArray vertex, texcoord, colors;
    unsigned bound[2]; /* the texture of each unit */
    int active;
    unsigned env;
    int viewport[4], scissor[4];
    int unpack;
    GlesSurface* draw;
};

typedef struct {
    bool used, ready; /* ready: the image holds data, in SHADER_READ_ONLY_OPTIMAL */
    bool expand;      /* 3-byte texels that gles_rgb.comp writes into an RGBA8 image */
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    int width, height, base, bpp;
    VkFormat format;
    unsigned min, mag, wrap_s, wrap_t;
    int crop[4];
} Texture;

/* GLES 1.1's own: its pipelines, samplers and textures. The device, surfaces and lock are gles_vk.c's */
static struct {
    bool tried, up; /* the layouts, shaders and pipelines below, made at the first draw or texture */
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkShaderModule vert, frag;
    /* the expansion of 3-byte texels (gles_rgb.comp): its set's layout, its layout and pipeline */
    VkDescriptorSetLayout expand_set;
    VkPipelineLayout expand_layout;
    VkPipeline expand;
    VkDeviceSize storage_align;
    VkSampler samplers[32];
    VkPipeline pipelines[PIPELINE_KEYS]; /* made for g_gles_vk.pass_clear */
    Texture* textures;
    size_t texture_count;
    unsigned white; /* a 1x1 white texture, for a draw without one */
    bool warned_read;
} g;
#define V g_gles_vk
#undef GLES_DECLARE

static _Thread_local GlesContext* t_gl;

/* ---- the device ---- */

/* GLES 1.1's layouts, shaders and pipelines on the shared device: once, at the first draw or texture */
static bool gles1_up(void) {
    if (g.tried) return g.up;
    if (!V.up) return false;
    g.tried = true;
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
    VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1,
                                           .pBindings = &binding};
    V.vk.vkCreateDescriptorSetLayout(V.device, &set, NULL, &g.set_layout);
    VkPushConstantRange push = {.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                .size = 72};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &g.set_layout,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &push};
    V.vk.vkCreatePipelineLayout(V.device, &layout, NULL, &g.layout);
    g.vert = gles_vk_shader(android_gles_vert_spv, (size_t)android_gles_vert_spv_size);
    g.frag = gles_vk_shader(android_gles_frag_spv, (size_t)android_gles_frag_spv_size);
    VkDeviceSize align = V.props.limits.minStorageBufferOffsetAlignment;
    g.storage_align = align < 16 ? 16 : align;
    VkDescriptorSetLayoutBinding expand[2] = {{.binding = 0,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                               .descriptorCount = 1,
                                               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT},
                                              {.binding = 1,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                               .descriptorCount = 1,
                                               .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT}};
    VkDescriptorSetLayoutCreateInfo expand_set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                  .bindingCount = 2,
                                                  .pBindings = expand};
    V.vk.vkCreateDescriptorSetLayout(V.device, &expand_set, NULL, &g.expand_set);
    VkPushConstantRange expand_push = {.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 20};
    VkPipelineLayoutCreateInfo expand_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                .setLayoutCount = 1,
                                                .pSetLayouts = &g.expand_set,
                                                .pushConstantRangeCount = 1,
                                                .pPushConstantRanges = &expand_push};
    V.vk.vkCreatePipelineLayout(V.device, &expand_layout, NULL, &g.expand_layout);
    VkShaderModule comp = gles_vk_shader(android_gles_rgb_comp_spv, (size_t)android_gles_rgb_comp_spv_size);
    VkComputePipelineCreateInfo cinfo = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = comp,
                                                   .pName = "main"},
                                         .layout = g.expand_layout};
    if (comp) V.vk.vkCreateComputePipelines(V.device, VK_NULL_HANDLE, 1, &cinfo, NULL, &g.expand);
    g.up = g.set_layout && g.layout && g.vert && g.frag;
    return g.up;
}

/* the surfaces' format changed: the pipelines, made for the old one, made again at their next draw */
void gles1_format_changed(void) {
    for (size_t i = 0; i < PIPELINE_KEYS; ++i)
        if (g.pipelines[i]) V.vk.vkDestroyPipeline(V.device, g.pipelines[i], NULL);
    memset(g.pipelines, 0, sizeof g.pipelines);
}

/* GL's blend factors as Vulkan's, by their order: ZERO, ONE, then SRC_COLOR (0x300) to SRC_ALPHA_SATURATE (0x308) */
static int factor_index(unsigned f) {
    if (f <= 1) return (int)f;
    if (f >= 0x300 && f <= 0x308) return (int)(f - 0x300) + 2;
    return -1;
}
static VkBlendFactor factor(int index) {
    static const VkBlendFactor table[] = {
        VK_BLEND_FACTOR_ZERO,      VK_BLEND_FACTOR_ONE,       VK_BLEND_FACTOR_SRC_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA, VK_BLEND_FACTOR_DST_COLOR,
        VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_SRC_ALPHA_SATURATE};
    return table[index];
}

/* the pipeline of a primitive kind (0 triangles, 1 lines, 2 points) and a blend (off, or its two factors) */
static VkPipeline pipeline(int kind, bool blend, int src, int dst) {
    unsigned key = blend ? (unsigned)(1 + src * 11 + dst) : 0;
    key = key * 3 + (unsigned)kind;
    if (key >= PIPELINE_KEYS) return VK_NULL_HANDLE;
    if (g.pipelines[key]) return g.pipelines[key];
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = g.vert,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = g.frag,
         .pName = "main"}};
    VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[3] = {
        {0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}};
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                  .vertexBindingDescriptionCount = 1,
                                                  .pVertexBindingDescriptions = &binding,
                                                  .vertexAttributeDescriptionCount = 3,
                                                  .pVertexAttributeDescriptions = attributes};
    static const VkPrimitiveTopology topologies[] = {VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                     VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                     VK_PRIMITIVE_TOPOLOGY_POINT_LIST};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = topologies[kind]};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL,
                                                     .cullMode = VK_CULL_MODE_NONE,
                                                     .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo ms = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineColorBlendAttachmentState attachment = {.blendEnable = blend,
                                                      .srcColorBlendFactor = factor(src),
                                                      .dstColorBlendFactor = factor(dst),
                                                      .colorBlendOp = VK_BLEND_OP_ADD,
                                                      .srcAlphaBlendFactor = factor(src),
                                                      .dstAlphaBlendFactor = factor(dst),
                                                      .alphaBlendOp = VK_BLEND_OP_ADD,
                                                      .colorWriteMask = 0xf};
    VkPipelineColorBlendStateCreateInfo blending = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = 1,
                                                    .pAttachments = &attachment};
    static const VkDynamicState dynamic[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2,
                                            .pDynamicStates = dynamic};
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .stageCount = 2,
                                         .pStages = stages,
                                         .pVertexInputState = &input,
                                         .pInputAssemblyState = &assembly,
                                         .pViewportState = &viewport,
                                         .pRasterizationState = &raster,
                                         .pMultisampleState = &ms,
                                         .pColorBlendState = &blending,
                                         .pDynamicState = &dyn,
                                         .layout = g.layout,
                                         .renderPass = V.pass_clear};
    V.vk.vkCreateGraphicsPipelines(V.device, VK_NULL_HANDLE, 1, &info, NULL, &g.pipelines[key]);
    return g.pipelines[key];
}

/* the sampler of a texture's filters and wraps (mipmapped filters as their base level's) */
static VkSampler sampler(const Texture* t) {
    bool mag = t->mag == GL_LINEAR_;
    bool min = t->min == GL_LINEAR_ || t->min == 0x2701 || t->min == 0x2703;
    int ws = t->wrap_s == GL_CLAMP_TO_EDGE_ ? 0 : t->wrap_s == GL_MIRRORED_REPEAT_ ? 2 : 1;
    int wt = t->wrap_t == GL_CLAMP_TO_EDGE_ ? 0 : t->wrap_t == GL_MIRRORED_REPEAT_ ? 2 : 1;
    unsigned key = (unsigned)((mag * 2 + min) * 9 + ws * 3 + wt);
    if (key >= 32) key = 0;
    if (g.samplers[key]) return g.samplers[key];
    static const VkSamplerAddressMode modes[] = {VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                                 VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                .magFilter = mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                .minFilter = min ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                .addressModeU = modes[ws],
                                .addressModeV = modes[wt],
                                .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                .maxLod = 0};
    V.vk.vkCreateSampler(V.device, &info, NULL, &g.samplers[key]);
    return g.samplers[key];
}

/* ---- contexts ---- */

static void identity(float m[16]) {
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = 1;
}
/* m = m * n, GL's column-major order */
static void multiply(float m[16], const float n[16]) {
    float r[16];
    for (int c = 0; c < 4; ++c)
        for (int row = 0; row < 4; ++row) {
            float v = 0;
            for (int k = 0; k < 4; ++k) v += m[k * 4 + row] * n[c * 4 + k];
            r[c * 4 + row] = v;
        }
    memcpy(m, r, sizeof r);
}

GlesContext* gles_context_new(void) {
    GlesContext* gl = calloc(1, sizeof *gl);
    if (!gl) return NULL;
    for (int i = 0; i < 3; ++i) identity(gl->stacks[i][0]);
    for (int i = 0; i < 4; ++i) gl->color[i] = 1;
    gl->src = 1;
    gl->dst = 0;
    gl->env = GL_MODULATE_;
    gl->unpack = 4;
    gl->vertex.size = gl->texcoord.size = 4;
    gl->colors.size = 4;
    gl->vertex.type = gl->texcoord.type = gl->colors.type = GL_FLOAT_;
    return gl;
}

void gles_context_free(GlesContext* gl) {
    if (t_gl == gl) t_gl = NULL;
    free(gl);
}

void gles_make_current(GlesContext* gl, GlesSurface* draw) {
    t_gl = gl;
    if (!gl) return;
    gl->draw = draw;
    if (draw && !gl->sized) {
        int w, h;
        gles_surface_size(draw, &w, &h);
        gl->viewport[2] = gl->scissor[2] = w;
        gl->viewport[3] = gl->scissor[3] = h;
        gl->sized = true;
    }
}

static void set_error(GlesContext* gl, unsigned e) {
    if (gl && !gl->error) gl->error = e;
}

/* ---- textures ---- */

static Texture* texture(unsigned name) {
    if (name >= g.texture_count) {
        size_t count = name + 64;
        Texture* grown = realloc(g.textures, count * sizeof *grown);
        if (!grown) return NULL;
        memset(grown + g.texture_count, 0, (count - g.texture_count) * sizeof *grown);
        g.textures = grown;
        g.texture_count = count;
    }
    Texture* t = &g.textures[name];
    if (!t->used) {
        *t = (Texture){.used = true,
                       .min = 0x2702, /* NEAREST_MIPMAP_LINEAR, GL's default */
                       .mag = GL_LINEAR_,
                       .wrap_s = GL_REPEAT_,
                       .wrap_t = GL_REPEAT_};
    }
    return t;
}

/* a GL format and type as a Vulkan format the device samples and copies into, its view's swizzle, the base format
   (gles.frag's) and the bytes of a texel. 0 for a pair GLES 1.1 does not have, or the device cannot take */
static VkFormat texture_format(unsigned format, unsigned type, VkComponentMapping* swizzle, int* base, int* bpp,
                               bool* expand) {
    *expand = false;
    enum { I = VK_COMPONENT_SWIZZLE_IDENTITY, R = VK_COMPONENT_SWIZZLE_R, G = VK_COMPONENT_SWIZZLE_G,
           B = VK_COMPONENT_SWIZZLE_B, O = VK_COMPONENT_SWIZZLE_ONE };
    struct Candidate {
        VkFormat format;
        VkComponentMapping swizzle;
    };
    /* two candidates each: the format of GL's own layout, then one whose red and blue swap places in the bits */
    struct Candidate c[2] = {{0}, {0}};
    switch (format * 0x10000u + type) {
    case GL_RGBA_ * 0x10000u + GL_UNSIGNED_BYTE_:
        c[0] = (struct Candidate){VK_FORMAT_R8G8B8A8_UNORM, {I, I, I, I}}, *base = 0, *bpp = 4;
        break;
    case GL_BGRA_EXT_ * 0x10000u + GL_UNSIGNED_BYTE_:
        c[0] = (struct Candidate){VK_FORMAT_B8G8R8A8_UNORM, {I, I, I, I}}, *base = 0, *bpp = 4;
        break;
    case GL_RGB_ * 0x10000u + GL_UNSIGNED_BYTE_:
        c[0] = (struct Candidate){VK_FORMAT_R8G8B8_UNORM, {I, I, I, O}}, *base = 1, *bpp = 3;
        break;
    case GL_RGB_ * 0x10000u + GL_UNSIGNED_SHORT_5_6_5_:
        c[0] = (struct Candidate){VK_FORMAT_R5G6B5_UNORM_PACK16, {I, I, I, O}};
        c[1] = (struct Candidate){VK_FORMAT_B5G6R5_UNORM_PACK16, {B, G, R, O}}, *base = 1, *bpp = 2;
        break;
    case GL_RGBA_ * 0x10000u + GL_UNSIGNED_SHORT_4_4_4_4_:
        c[0] = (struct Candidate){VK_FORMAT_R4G4B4A4_UNORM_PACK16, {I, I, I, I}};
        c[1] = (struct Candidate){VK_FORMAT_B4G4R4A4_UNORM_PACK16, {B, G, R, I}}, *base = 0, *bpp = 2;
        break;
    case GL_RGBA_ * 0x10000u + GL_UNSIGNED_SHORT_5_5_5_1_:
        c[0] = (struct Candidate){VK_FORMAT_R5G5B5A1_UNORM_PACK16, {I, I, I, I}};
        c[1] = (struct Candidate){VK_FORMAT_B5G5R5A1_UNORM_PACK16, {B, G, R, I}}, *base = 0, *bpp = 2;
        break;
    case GL_ALPHA_ * 0x10000u + GL_UNSIGNED_BYTE_:
        c[0] = (struct Candidate){VK_FORMAT_R8_UNORM, {O, O, O, R}}, *base = 2, *bpp = 1;
        break;
    case GL_LUMINANCE_ * 0x10000u + GL_UNSIGNED_BYTE_:
        c[0] = (struct Candidate){VK_FORMAT_R8_UNORM, {R, R, R, O}}, *base = 3, *bpp = 1;
        break;
    case GL_LUMINANCE_ALPHA_ * 0x10000u + GL_UNSIGNED_BYTE_:
        c[0] = (struct Candidate){VK_FORMAT_R8G8_UNORM, {R, R, R, G}}, *base = 4, *bpp = 2;
        break;
    default: return VK_FORMAT_UNDEFINED;
    }
    for (int i = 0; i < 2 && c[i].format; ++i) {
        VkFormatProperties p;
        V.vk.vkGetPhysicalDeviceFormatProperties(V.physical, c[i].format, &p);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if ((p.optimalTilingFeatures & need) == need) {
            *swizzle = c[i].swizzle;
            return c[i].format;
        }
    }
    /* 3-byte texels on a device that has no 3-byte format: RGBA8, which every device stores to, written by
       gles_rgb.comp */
    if (*bpp == 3 && g.expand) {
        *expand = true;
        *swizzle = (VkComponentMapping){I, I, I, I};
        return VK_FORMAT_R8G8B8A8_UNORM;
    }
    return VK_FORMAT_UNDEFINED;
}

static void texture_release(GlesSurface* s, Texture* t) {
    GlesGarbage x = {.image = t->image, .view = t->view, .memory = t->memory};
    if (s && s->recording) gles_vk_garbage(&s->slots[s->slot], x);
    else if (x.image) {
        V.vk.vkDeviceWaitIdle(V.device);
        gles_vk_destroy(x);
    }
    t->image = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->memory = VK_NULL_HANDLE;
    t->ready = false;
}

/* a new image for the texture: width by height of a format */
static bool texture_image(GlesSurface* s, Texture* t, int width, int height, VkFormat format, VkComponentMapping swizzle,
                          bool expand) {
    texture_release(s, t);
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {(uint32_t)width, (uint32_t)height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       (expand ? VK_IMAGE_USAGE_STORAGE_BIT : 0)};
    if (V.vk.vkCreateImage(V.device, &info, NULL, &t->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    V.vk.vkGetImageMemoryRequirements(V.device, t->image, &req);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = gles_vk_memory_type(req.memoryTypeBits,
                                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (alloc.memoryTypeIndex == UINT32_MAX) alloc.memoryTypeIndex = gles_vk_memory_type(req.memoryTypeBits, 0);
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = format,
                                  .components = swizzle,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (alloc.memoryTypeIndex == UINT32_MAX || V.vk.vkAllocateMemory(V.device, &alloc, NULL, &t->memory) != VK_SUCCESS ||
        V.vk.vkBindImageMemory(V.device, t->image, t->memory, 0) != VK_SUCCESS) {
        texture_release(NULL, t);
        return false;
    }
    view.image = t->image;
    if (V.vk.vkCreateImageView(V.device, &view, NULL, &t->view) != VK_SUCCESS) {
        texture_release(NULL, t);
        return false;
    }
    t->width = width;
    t->height = height;
    t->format = format;
    t->expand = expand;
    return true;
}

/* the packed 3-byte rows at offset of the staging buffer into the texture's RGBA8 image (gles_rgb.comp). The image
   is in TRANSFER_DST_OPTIMAL, and is left in GENERAL whether the dispatch is recorded or not */
static bool expand_rows(GlesSlot* slot, Texture* t, VkDeviceSize at, VkDeviceSize size, int x, int y, int width,
                        int height, uint32_t row) {
    VkCommandBuffer cb = slot->cb;
    /* the host's writes to the staging buffer are visible to the queue at the submit: only the layout changes */
    gles_image_barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    VkImageView view = VK_NULL_HANDLE;
    VkImageViewCreateInfo vinfo = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                   .image = t->image,
                                   .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                   .format = VK_FORMAT_R8G8B8A8_UNORM,
                                   .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = slot->pool,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &g.expand_set};
    if (V.vk.vkCreateImageView(V.device, &vinfo, NULL, &view) != VK_SUCCESS) return false;
    gles_vk_garbage(slot, (GlesGarbage){.view = view}); /* the frame's commands use it */
    if (V.vk.vkAllocateDescriptorSets(V.device, &alloc, &set) != VK_SUCCESS) return false;
    VkDescriptorBufferInfo buffer = {slot->staging.buffer, at, size};
    VkDescriptorImageInfo image = {VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &buffer},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 1,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &image}};
    V.vk.vkUpdateDescriptorSets(V.device, 2, writes, 0, NULL);
    V.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.expand);
    V.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.expand_layout, 0, 1, &set, 0, NULL);
    int32_t push[5] = {x, y, width, height, (int32_t)row};
    V.vk.vkCmdPushConstants(cb, g.expand_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    V.vk.vkCmdDispatch(cb, ((uint32_t)width + 7) / 8, ((uint32_t)height + 7) / 8, 1);
    return true;
}

/* rows of pixels (GL's unpack alignment between rows) into a rectangle of the texture, or zeros for no pixels: a
   copy in the frame's commands, outside its render pass */
static bool texture_upload(GlesContext* gl, Texture* t, int x, int y, int width, int height, const unsigned char* pixels) {
    GlesSurface* s = gl->draw;
    if (!s || !gles_frame_begin(s)) return false;
    gles_pass_end(s);
    GlesSlot* slot = &s->slots[s->slot];
    VkCommandBuffer cb = slot->cb;
    VkImageLayout from = t->ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    gles_image_barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (!t->ready && (pixels == NULL || width != t->width || height != t->height)) {
        /* GL leaves the rest undefined: zeros */
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        V.vk.vkCmdClearColorImage(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    }
    bool ok = true, general = false;
    if (pixels && width > 0 && height > 0) {
        size_t row = (size_t)width * (size_t)t->bpp;
        size_t stride = (row + (size_t)gl->unpack - 1) / (size_t)gl->unpack * (size_t)gl->unpack;
        VkDeviceSize size = (VkDeviceSize)row * (VkDeviceSize)height;
        VkDeviceSize at = gles_vk_arena_take(slot, &slot->staging, (size + 3) & ~(VkDeviceSize)3, g.storage_align, 8u << 20,
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        ok = at != VK_WHOLE_SIZE;
        if (ok) {
            /* the guest's rows, packed: the one copy of guest memory, whole rows at a time */
            if (stride == row) memcpy(slot->staging.map + at, pixels, (size_t)size);
            else
                for (int r = 0; r < height; ++r)
                    memcpy(slot->staging.map + at + (size_t)r * row, pixels + (size_t)r * stride, row);
            if (t->expand) {
                general = true;
                ok = expand_rows(slot, t, at, (VkDeviceSize)((size + 3) & ~(VkDeviceSize)3), x, y, width, height,
                                 (uint32_t)row);
            } else {
                VkBufferImageCopy copy = {.bufferOffset = at,
                                          .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                          .imageOffset = {x, y, 0},
                                          .imageExtent = {(uint32_t)width, (uint32_t)height, 1}};
                V.vk.vkCmdCopyBufferToImage(cb, slot->staging.buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                            1, &copy);
            }
        }
    }
    if (general) {
        gles_image_barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    } else {
        gles_image_barrier(cb, t->image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    t->ready = true;
    return ok;
}

/* ---- draws ---- */

/* element i of a client array as up to 4 floats (the rest of out kept); GL_FIXED is 16.16 */
static void fetch(const ClientArray* a, uint32_t i, bool normalize, float out[4]) {
    int size = a->size < 1 ? 1 : a->size > 4 ? 4 : a->size;
    size_t bytes = a->type == GL_BYTE_ || a->type == GL_UNSIGNED_BYTE_ ? 1
                   : a->type == GL_SHORT_ || a->type == GL_UNSIGNED_SHORT_ ? 2 : 4;
    size_t stride = a->stride ? (size_t)a->stride : bytes * (size_t)size;
    const unsigned char* p = (const unsigned char*)(uintptr_t)a->pointer + stride * i;
    for (int k = 0; k < size; ++k, p += bytes) {
        switch (a->type) {
        case GL_BYTE_: out[k] = normalize ? (float)(int8_t)p[0] / 127.0f : (float)(int8_t)p[0]; break;
        case GL_UNSIGNED_BYTE_: out[k] = normalize ? (float)p[0] / 255.0f : (float)p[0]; break;
        case GL_SHORT_: {
            int16_t v;
            memcpy(&v, p, 2);
            out[k] = (float)v;
            break;
        }
        case GL_UNSIGNED_SHORT_: {
            uint16_t v;
            memcpy(&v, p, 2);
            out[k] = (float)v;
            break;
        }
        case GL_FIXED_: {
            int32_t v;
            memcpy(&v, p, 4);
            out[k] = (float)v / 65536.0f;
            break;
        }
        default: memcpy(&out[k], p, 4); break;
        }
    }
}

static void vertex_of(const GlesContext* gl, uint32_t i, Vertex* v) {
    float p[4] = {0, 0, 0, 1};
    fetch(&gl->vertex, i, false, p);
    memcpy(v->position, p, sizeof p);
    float uv[4] = {0, 0, 0, 1};
    if (gl->texcoord.on) {
        fetch(&gl->texcoord, i, false, uv);
        static const float unit[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        if (gl->depth[2] || memcmp(gl->stacks[2][0], unit, sizeof unit)) {
            /* the texture matrix, on the CPU: it is the identity almost always */
            const float* m = gl->stacks[2][gl->depth[2]];
            float u = m[0] * uv[0] + m[4] * uv[1] + m[8] * uv[2] + m[12] * uv[3];
            float w = m[1] * uv[0] + m[5] * uv[1] + m[9] * uv[2] + m[13] * uv[3];
            float q = m[3] * uv[0] + m[7] * uv[1] + m[11] * uv[2] + m[15] * uv[3];
            uv[0] = q != 0 ? u / q : u;
            uv[1] = q != 0 ? w / q : w;
        }
    }
    v->uv[0] = uv[0];
    v->uv[1] = uv[1];
    if (gl->colors.on) {
        float c[4] = {0, 0, 0, 1};
        fetch(&gl->colors, i, true, c);
        memcpy(v->color, c, sizeof c);
    } else {
        memcpy(v->color, gl->color, sizeof v->color);
    }
}

/* the vertices of a draw, as the list its primitive kind draws: their count, the kind in *kind */
static uint32_t assemble(const GlesContext* gl, unsigned mode, uint32_t count, const uint32_t* indices, uint32_t first,
                         Vertex* out, int* kind) {
#define AT(k) (indices ? indices[k] : first + (k))
    uint32_t n = 0;
    switch (mode) {
    case GL_TRIANGLES_:
        *kind = 0;
        for (uint32_t i = 0; i + 2 < count; i += 3)
            for (uint32_t k = 0; k < 3; ++k) vertex_of(gl, AT(i + k), &out[n++]);
        break;
    case GL_TRIANGLE_STRIP_:
        *kind = 0;
        for (uint32_t i = 2; i < count; ++i) {
            vertex_of(gl, AT(i - 2), &out[n++]);
            vertex_of(gl, AT(i - 1), &out[n++]);
            vertex_of(gl, AT(i), &out[n++]);
        }
        break;
    case GL_TRIANGLE_FAN_:
        *kind = 0;
        for (uint32_t i = 2; i < count; ++i) {
            vertex_of(gl, AT(0), &out[n++]);
            vertex_of(gl, AT(i - 1), &out[n++]);
            vertex_of(gl, AT(i), &out[n++]);
        }
        break;
    case GL_LINES_:
        *kind = 1;
        for (uint32_t i = 0; i + 1 < count; i += 2) {
            vertex_of(gl, AT(i), &out[n++]);
            vertex_of(gl, AT(i + 1), &out[n++]);
        }
        break;
    case GL_LINE_STRIP_:
    case GL_LINE_LOOP_:
        *kind = 1;
        for (uint32_t i = 1; i < count; ++i) {
            vertex_of(gl, AT(i - 1), &out[n++]);
            vertex_of(gl, AT(i), &out[n++]);
        }
        if (mode == GL_LINE_LOOP_ && count > 2) {
            vertex_of(gl, AT(count - 1), &out[n++]);
            vertex_of(gl, AT(0), &out[n++]);
        }
        break;
    default:
        *kind = 2;
        for (uint32_t i = 0; i < count; ++i) vertex_of(gl, AT(i), &out[n++]);
        break;
    }
#undef AT
    return n;
}

/* the most vertices a draw of count elements assembles into */
static uint64_t assembled_size(unsigned mode, uint32_t count) {
    return mode == GL_TRIANGLE_STRIP_ || mode == GL_TRIANGLE_FAN_ ? (uint64_t)count * 3
           : mode == GL_LINE_STRIP_ || mode == GL_LINE_LOOP_      ? (uint64_t)count * 2 + 2
                                                                  : count;
}

/* the texture a draw reads: unit 0's when GL_TEXTURE_2D is on and it has an image */
static Texture* draw_texture(const GlesContext* gl) {
    if (!gl->texture2d || gl->bound[0] >= g.texture_count) return NULL;
    Texture* t = &g.textures[gl->bound[0]];
    return t->used && t->ready ? t : NULL;
}

/* a draw of vertices already in the frame's vertex buffer at offset, with the matrix mvp */
static void record_draw(GlesContext* gl, VkDeviceSize offset, uint32_t n, int kind, const float mvp[16], bool full) {
    GlesSurface* s = gl->draw;
    GlesSlot* slot = &s->slots[s->slot];
    VkCommandBuffer cb = slot->cb;
    int src = factor_index(gl->src), dst = factor_index(gl->dst);
    bool blend = gl->blend && src >= 0 && dst >= 0 && !(src == 1 && dst == 0);
    VkPipeline p = pipeline(kind, blend, blend ? src : 1, blend ? dst : 0);
    Texture* t = draw_texture(gl);
    if (!t && !g.white) return;
    Texture* bound = t ? t : &g.textures[g.white];
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                         .descriptorPool = slot->pool,
                                         .descriptorSetCount = 1,
                                         .pSetLayouts = &g.set_layout};
    if (!p || !bound->view || V.vk.vkAllocateDescriptorSets(V.device, &alloc, &set) != VK_SUCCESS) return;
    VkDescriptorImageInfo image = {sampler(bound), bound->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  .pImageInfo = &image};
    V.vk.vkUpdateDescriptorSets(V.device, 1, &write, 0, NULL);
    V.vk.vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    float h = (float)s->extent.height;
    int vp[4] = {gl->viewport[0], gl->viewport[1], gl->viewport[2], gl->viewport[3]};
    if (full) vp[0] = vp[1] = 0, vp[2] = (int)s->extent.width, vp[3] = (int)s->extent.height;
    VkViewport viewport = {(float)vp[0], h - (float)vp[1], (float)vp[2], -(float)vp[3], 0, 1};
    V.vk.vkCmdSetViewport(cb, 0, 1, &viewport);
    int32_t sx = 0, sy = 0;
    int32_t sw = (int32_t)s->extent.width, sh = (int32_t)s->extent.height;
    if (gl->scissor_test) {
        sx = gl->scissor[0];
        sy = (int32_t)s->extent.height - gl->scissor[1] - gl->scissor[3];
        sw = gl->scissor[2];
        sh = gl->scissor[3];
        if (sx < 0) sw += sx, sx = 0;
        if (sy < 0) sh += sy, sy = 0;
        if (sw < 0) sw = 0;
        if (sh < 0) sh = 0;
    }
    VkRect2D scissor = {{sx, sy}, {(uint32_t)sw, (uint32_t)sh}};
    V.vk.vkCmdSetScissor(cb, 0, 1, &scissor);
    struct {
        float mvp[16];
        int32_t mode, base;
    } push;
    memcpy(push.mvp, mvp, sizeof push.mvp);
    push.mode = !t ? 0 : gl->env == GL_REPLACE_ ? 2 : gl->env == GL_DECAL_ ? 3 : gl->env == GL_ADD_ ? 4 : 1;
    push.base = t ? t->base : 0;
    V.vk.vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push,
                            &push);
    V.vk.vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, NULL);
    V.vk.vkCmdBindVertexBuffers(cb, 0, 1, &slot->vertices.buffer, &offset);
    V.vk.vkCmdDraw(cb, n, 1, 0, 0);
    s->drew = true;
}

/* the 1x1 white texture of draws without one, made at the first draw: the shaders' set must hold an image */
static void white_texture(GlesContext* gl) {
    if (g.white) return;
    unsigned name = 1;
    while (name < g.texture_count && g.textures[name].used) ++name;
    Texture* w = texture(name);
    VkComponentMapping swizzle;
    int base = 0, bpp = 0;
    bool expand;
    VkFormat f = texture_format(GL_RGBA_, GL_UNSIGNED_BYTE_, &swizzle, &base, &bpp, &expand);
    if (!w || !f || !texture_image(gl->draw, w, 1, 1, f, swizzle, false)) return;
    w->base = base;
    w->bpp = bpp;
    static const unsigned char white[4] = {255, 255, 255, 255};
    int unpack = gl->unpack;
    gl->unpack = 4;
    if (texture_upload(gl, w, 0, 0, 1, 1, white)) g.white = name;
    gl->unpack = unpack;
}

/* glDrawArrays and glDrawElements: the elements first .. first + count, or by indices */
static void draw_elements(GlesContext* gl, unsigned mode, uint32_t count, const uint32_t* indices, uint32_t first) {
    if (!gl->vertex.on || !count || !gl->draw) return;
    pthread_mutex_lock(&V.lock);
    if (!gles1_up()) {
        pthread_mutex_unlock(&V.lock);
        return;
    }
    GlesSurface* s = gl->draw;
    uint64_t most = assembled_size(mode, count);
    white_texture(gl);
    if (most * sizeof(Vertex) > (256u << 20) || !gles_pass_begin(s)) {
        pthread_mutex_unlock(&V.lock);
        return;
    }
    GlesSlot* slot = &s->slots[s->slot];
    VkDeviceSize at = gles_vk_arena_take(slot, &slot->vertices, most * sizeof(Vertex), 16, 1u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (at == VK_WHOLE_SIZE) {
        set_error(gl, GL_OUT_OF_MEMORY_);
        pthread_mutex_unlock(&V.lock);
        return;
    }
    int kind = 0;
    uint32_t n = assemble(gl, mode, count, indices, first, (Vertex*)(slot->vertices.map + at), &kind);
    slot->vertices.used = at + n * sizeof(Vertex);
    float mvp[16];
    memcpy(mvp, gl->stacks[1][gl->depth[1]], sizeof mvp);
    multiply(mvp, gl->stacks[0][gl->depth[0]]);
    if (n) record_draw(gl, at, n, kind, mvp, false);
    pthread_mutex_unlock(&V.lock);
}

/* glDrawTexOES(x, y, z, width, height): the bound texture's crop rectangle onto the window's rectangle at x, y */
static void draw_texture_rect(GlesContext* gl, float x, float y, float width, float height) {
    if (!gl->draw || width <= 0 || height <= 0) return;
    pthread_mutex_lock(&V.lock);
    if (!gles1_up()) {
        pthread_mutex_unlock(&V.lock);
        return;
    }
    Texture* t = draw_texture(gl);
    GlesSurface* s = gl->draw;
    if (!t || !gles_pass_begin(s)) {
        pthread_mutex_unlock(&V.lock);
        return;
    }
    GlesSlot* slot = &s->slots[s->slot];
    VkDeviceSize at = gles_vk_arena_take(slot, &slot->vertices, 6 * sizeof(Vertex), 16, 1u << 20, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (at == VK_WHOLE_SIZE) {
        pthread_mutex_unlock(&V.lock);
        return;
    }
    float w = (float)s->extent.width, h = (float)s->extent.height;
    float x0 = x / w * 2 - 1, x1 = (x + width) / w * 2 - 1, y0 = y / h * 2 - 1, y1 = (y + height) / h * 2 - 1;
    float u0 = (float)t->crop[0] / (float)t->width, u1 = (float)(t->crop[0] + t->crop[2]) / (float)t->width;
    float v0 = (float)t->crop[1] / (float)t->height, v1 = (float)(t->crop[1] + t->crop[3]) / (float)t->height;
    Vertex q[4] = {{{x0, y0, 0, 1}, {u0, v0}, {0}},
                   {{x1, y0, 0, 1}, {u1, v0}, {0}},
                   {{x0, y1, 0, 1}, {u0, v1}, {0}},
                   {{x1, y1, 0, 1}, {u1, v1}, {0}}};
    for (int i = 0; i < 4; ++i) memcpy(q[i].color, gl->color, sizeof q[i].color);
    Vertex* out = (Vertex*)(slot->vertices.map + at);
    static const int order[6] = {0, 1, 2, 2, 1, 3};
    for (int i = 0; i < 6; ++i) out[i] = q[order[i]];
    float ident[16];
    identity(ident);
    record_draw(gl, at, 6, 0, ident, true);
    pthread_mutex_unlock(&V.lock);
}

/* ---- the GLES 1.1 calls ---- */

/* the calling thread's context, or a return: a call without one does nothing, as GL's */
#define GL_OR_RETURN()                                                                                                 \
    GlesContext* gl = t_gl;                                                                                            \
    if (!gl) return

static void g_get_error(CPU* c, void* data) {
    (void)data;
    GlesContext* gl = t_gl;
    X(0) = gl ? gl->error : GL_NO_ERROR_;
    if (gl) gl->error = GL_NO_ERROR_;
}

static bool* capability(GlesContext* gl, unsigned cap) {
    switch (cap) {
    case GL_TEXTURE_2D_: return &gl->texture2d;
    case GL_BLEND_: return &gl->blend;
    case GL_SCISSOR_TEST_: return &gl->scissor_test;
    default: return NULL; /* the others' state has no effect here (the header's list) */
    }
}
static void g_enable(CPU* c, void* data) {
    GL_OR_RETURN();
    bool* on = capability(gl, (unsigned)X(0));
    if (on) *on = data != NULL;
}
static bool* client_array(GlesContext* gl, unsigned array) {
    switch (array) {
    case GL_VERTEX_ARRAY_: return &gl->vertex.on;
    case GL_TEXTURE_COORD_ARRAY_: return &gl->texcoord.on;
    case GL_COLOR_ARRAY_: return &gl->colors.on;
    case GL_NORMAL_ARRAY_: return NULL;
    default: set_error(gl, GL_INVALID_ENUM_); return NULL;
    }
}
static void g_client_state(CPU* c, void* data) {
    GL_OR_RETURN();
    bool* on = client_array(gl, (unsigned)X(0));
    if (on) *on = data != NULL;
}
static void set_pointer(CPU* c, ClientArray* a) {
    a->size = (int)X(0);
    a->type = (unsigned)X(1);
    a->stride = (int)X(2);
    a->pointer = X(3);
}
static void g_vertex_pointer(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    set_pointer(c, &gl->vertex);
}
static void g_tex_coord_pointer(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    set_pointer(c, &gl->texcoord);
}
static void g_color_pointer(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    set_pointer(c, &gl->colors);
}
static void g_color4f(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    for (unsigned i = 0; i < 4; ++i) gl->color[i] = hle_f(c, i);
}
static void g_color4ub(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    for (unsigned i = 0; i < 4; ++i) gl->color[i] = (float)(X(i) & 0xff) / 255.0f;
}
static void g_color4x(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    for (unsigned i = 0; i < 4; ++i) gl->color[i] = (float)(int32_t)X(i) / 65536.0f;
}
static void g_clear_color(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    for (unsigned i = 0; i < 4; ++i) gl->clear[i] = hle_f(c, i);
}
static void g_clear(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    if (!((unsigned)X(0) & GL_COLOR_BUFFER_BIT_) || !gl->draw) return;
    pthread_mutex_lock(&V.lock);
    GlesSurface* s = gl->draw;
    if (gles_pass_begin(s)) {
        VkClearAttachment clear = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0};
        for (int i = 0; i < 4; ++i) clear.clearValue.color.float32[i] = gl->clear[i];
        VkClearRect rect = {{{0, 0}, s->extent}, 0, 1};
        if (gl->scissor_test) {
            int32_t x = gl->scissor[0], y = (int32_t)s->extent.height - gl->scissor[1] - gl->scissor[3];
            int32_t w = gl->scissor[2], h = gl->scissor[3];
            if (x < 0) w += x, x = 0;
            if (y < 0) h += y, y = 0;
            if (x + w > (int32_t)s->extent.width) w = (int32_t)s->extent.width - x;
            if (y + h > (int32_t)s->extent.height) h = (int32_t)s->extent.height - y;
            rect.rect = (VkRect2D){{x, y}, {(uint32_t)(w > 0 ? w : 0), (uint32_t)(h > 0 ? h : 0)}};
        }
        if (rect.rect.extent.width && rect.rect.extent.height)
            V.vk.vkCmdClearAttachments(s->slots[s->slot].cb, 1, &clear, 1, &rect);
    }
    pthread_mutex_unlock(&V.lock);
}
static void g_blend_func(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    gl->src = (unsigned)X(0);
    gl->dst = (unsigned)X(1);
}
static void g_viewport(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    for (int i = 0; i < 4; ++i) gl->viewport[i] = (int)X(i);
}
static void g_scissor(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    for (int i = 0; i < 4; ++i) gl->scissor[i] = (int)X(i);
}
static void g_tex_envf(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    if ((unsigned)X(0) == GL_TEXTURE_ENV_ && (unsigned)X(1) == GL_TEXTURE_ENV_MODE_) gl->env = (unsigned)hle_f(c, 0);
}
static void g_tex_envi(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    if ((unsigned)X(0) == GL_TEXTURE_ENV_ && (unsigned)X(1) == GL_TEXTURE_ENV_MODE_) gl->env = (unsigned)X(2);
}
static void g_pixel_store(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int v = (int)X(1);
    if (v != 1 && v != 2 && v != 4 && v != 8) {
        set_error(gl, GL_INVALID_VALUE_);
        return;
    }
    if ((unsigned)X(0) == GL_UNPACK_ALIGNMENT_) gl->unpack = v;
}

/* the matrices */
static float* current(GlesContext* gl) {
    return gl->stacks[gl->mode][gl->depth[gl->mode]];
}
static void g_matrix_mode(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    unsigned m = (unsigned)X(0);
    if (m >= GL_MODELVIEW_ && m <= GL_TEXTURE_) gl->mode = (int)(m - GL_MODELVIEW_);
    else set_error(gl, GL_INVALID_ENUM_);
}
static void g_load_identity(CPU* c, void* data) {
    (void)c, (void)data;
    GL_OR_RETURN();
    identity(current(gl));
}
static void g_push_matrix(CPU* c, void* data) {
    (void)c, (void)data;
    GL_OR_RETURN();
    int* d = &gl->depth[gl->mode];
    if (*d + 1 >= STACK) {
        set_error(gl, GL_STACK_OVERFLOW_);
        return;
    }
    memcpy(gl->stacks[gl->mode][*d + 1], gl->stacks[gl->mode][*d], sizeof(float[16]));
    ++*d;
}
static void g_pop_matrix(CPU* c, void* data) {
    (void)c, (void)data;
    GL_OR_RETURN();
    int* d = &gl->depth[gl->mode];
    if (*d == 0) set_error(gl, GL_STACK_UNDERFLOW_);
    else --*d;
}
static void g_load_matrixf(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    memcpy(current(gl), PTR(0), sizeof(float[16]));
}
static void g_mult_matrixf(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    float m[16];
    memcpy(m, PTR(0), sizeof m);
    multiply(current(gl), m);
}
static void ortho(GlesContext* gl, float l, float r, float b, float t, float n, float f) {
    if (l == r || b == t || n == f) {
        set_error(gl, GL_INVALID_VALUE_);
        return;
    }
    float m[16] = {2 / (r - l), 0, 0, 0, 0, 2 / (t - b), 0, 0, 0, 0, -2 / (f - n), 0,
                   -(r + l) / (r - l), -(t + b) / (t - b), -(f + n) / (f - n), 1};
    multiply(current(gl), m);
}
static void g_orthof(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    ortho(gl, hle_f(c, 0), hle_f(c, 1), hle_f(c, 2), hle_f(c, 3), hle_f(c, 4), hle_f(c, 5));
}
static void g_orthox(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    float a[6];
    for (int i = 0; i < 6; ++i) a[i] = (float)(int32_t)X(i) / 65536.0f;
    ortho(gl, a[0], a[1], a[2], a[3], a[4], a[5]);
}
static void g_translatef(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    float m[16];
    identity(m);
    m[12] = hle_f(c, 0), m[13] = hle_f(c, 1), m[14] = hle_f(c, 2);
    multiply(current(gl), m);
}
static void g_scalef(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    float m[16];
    identity(m);
    m[0] = hle_f(c, 0), m[5] = hle_f(c, 1), m[10] = hle_f(c, 2);
    multiply(current(gl), m);
}
static void g_rotatef(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    float angle = hle_f(c, 0) * 3.14159265358979f / 180.0f, x = hle_f(c, 1), y = hle_f(c, 2), z = hle_f(c, 3);
    float len = sqrtf(x * x + y * y + z * z);
    if (len == 0) return;
    x /= len, y /= len, z /= len;
    float s = sinf(angle), co = cosf(angle), k = 1 - co;
    float m[16] = {x * x * k + co,     y * x * k + z * s, x * z * k - y * s, 0, x * y * k - z * s, y * y * k + co,
                   y * z * k + x * s, 0,                 x * z * k + y * s, y * z * k - x * s, z * z * k + co, 0,
                   0,                 0,                 0,                 1};
    multiply(current(gl), m);
}

/* the textures */
static void g_gen_textures(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int n = (int)X(0);
    uint32_t* out = PTR(1);
    if (n < 0) {
        set_error(gl, GL_INVALID_VALUE_);
        return;
    }
    pthread_mutex_lock(&V.lock);
    unsigned name = 1;
    for (int i = 0; i < n; ++i) {
        while (name < g.texture_count && g.textures[name].used) ++name;
        Texture* t = texture(name);
        uint32_t v = t ? name : 0;
        memcpy(&out[i], &v, sizeof v);
        ++name;
    }
    pthread_mutex_unlock(&V.lock);
}
static void g_delete_textures(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int n = (int)X(0);
    const uint32_t* names = PTR(1);
    pthread_mutex_lock(&V.lock);
    for (int i = 0; i < n; ++i) {
        uint32_t name;
        memcpy(&name, &names[i], sizeof name);
        if (!name || name >= g.texture_count || !g.textures[name].used || name == g.white) continue;
        texture_release(gl->draw, &g.textures[name]);
        g.textures[name].used = false;
        for (int u = 0; u < 2; ++u)
            if (gl->bound[u] == name) gl->bound[u] = 0;
    }
    pthread_mutex_unlock(&V.lock);
}
static void g_bind_texture(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    if ((unsigned)X(0) != GL_TEXTURE_2D_) {
        set_error(gl, GL_INVALID_ENUM_);
        return;
    }
    pthread_mutex_lock(&V.lock);
    unsigned name = (unsigned)X(1);
    if (texture(name)) gl->bound[gl->active] = name;
    pthread_mutex_unlock(&V.lock);
}
static void g_active_texture(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    unsigned unit = (unsigned)X(0) - GL_TEXTURE0_;
    if (unit < 2) gl->active = (int)unit;
    else set_error(gl, GL_INVALID_ENUM_);
}
static void tex_parameter(GlesContext* gl, unsigned name, int value) {
    if (gl->bound[gl->active] >= g.texture_count) return;
    Texture* t = &g.textures[gl->bound[gl->active]];
    switch (name) {
    case GL_TEXTURE_MIN_FILTER_: t->min = (unsigned)value; break;
    case GL_TEXTURE_MAG_FILTER_: t->mag = (unsigned)value; break;
    case GL_TEXTURE_WRAP_S_: t->wrap_s = (unsigned)value; break;
    case GL_TEXTURE_WRAP_T_: t->wrap_t = (unsigned)value; break;
    default: break; /* GL_GENERATE_MIPMAP: no mipmaps here */
    }
}
static void g_tex_parameteri(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    pthread_mutex_lock(&V.lock);
    tex_parameter(gl, (unsigned)X(1), (int)X(2));
    pthread_mutex_unlock(&V.lock);
}
static void g_tex_parameterf(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    pthread_mutex_lock(&V.lock);
    tex_parameter(gl, (unsigned)X(1), (int)hle_f(c, 0));
    pthread_mutex_unlock(&V.lock);
}
static void g_tex_parameteriv(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    const int32_t* v = PTR(2);
    pthread_mutex_lock(&V.lock);
    if ((unsigned)X(1) == GL_TEXTURE_CROP_RECT_OES_) {
        if (gl->bound[gl->active] < g.texture_count) memcpy(g.textures[gl->bound[gl->active]].crop, v, 4 * sizeof *v);
    } else {
        int32_t first;
        memcpy(&first, v, sizeof first);
        tex_parameter(gl, (unsigned)X(1), first);
    }
    pthread_mutex_unlock(&V.lock);
}
/* glTexImage2D(target, level, internalformat, width, height, border, format, type, pixels): the base level only */
static void g_tex_image(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int level = (int)X(1), width = (int)X(3), height = (int)X(4);
    unsigned format = (unsigned)X(6), type = (unsigned)X(7);
    uint64_t pixels;
    memcpy(&pixels, (const void*)(uintptr_t)X(31), sizeof pixels); /* the ninth argument: the guest's stack */
    if ((unsigned)X(0) != GL_TEXTURE_2D_) {
        set_error(gl, GL_INVALID_ENUM_);
        return;
    }
    if (level != 0) return; /* no mipmaps: the base level is the one sampled */
    pthread_mutex_lock(&V.lock);
    VkComponentMapping swizzle;
    int base = 0, bpp = 0;
    bool expand = false;
    VkFormat vf = gles1_up() ? texture_format(format, type, &swizzle, &base, &bpp, &expand) : VK_FORMAT_UNDEFINED;
    Texture* t = gl->bound[gl->active] < g.texture_count ? &g.textures[gl->bound[gl->active]] : NULL;
    if (!t || !vf || width < 0 || height < 0 || (uint32_t)width > V.max_texture || (uint32_t)height > V.max_texture) {
        static bool said;
        if (!vf && !said) {
            fprintf(stderr, "scarletnx-android: GLES: no texture format for 0x%x/0x%x\n", format, type);
            said = true;
        }
        set_error(gl, vf ? GL_INVALID_VALUE_ : GL_INVALID_OPERATION_);
        pthread_mutex_unlock(&V.lock);
        return;
    }
    if (width && height) {
        if ((t->width != width || t->height != height || t->format != vf || t->expand != expand || !t->image) &&
            !texture_image(gl->draw, t, width, height, vf, swizzle, expand)) {
            set_error(gl, GL_OUT_OF_MEMORY_);
            pthread_mutex_unlock(&V.lock);
            return;
        }
        t->base = base;
        t->bpp = bpp;
        texture_upload(gl, t, 0, 0, width, height, (const unsigned char*)(uintptr_t)pixels);
    }
    pthread_mutex_unlock(&V.lock);
}
/* glTexSubImage2D(target, level, x, y, width, height, format, type, pixels) */
static void g_tex_sub_image(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int level = (int)X(1), x = (int)X(2), y = (int)X(3), width = (int)X(4), height = (int)X(5);
    uint64_t pixels;
    memcpy(&pixels, (const void*)(uintptr_t)X(31), sizeof pixels);
    if (level != 0 || width <= 0 || height <= 0) return;
    pthread_mutex_lock(&V.lock);
    Texture* t = gl->bound[gl->active] < g.texture_count ? &g.textures[gl->bound[gl->active]] : NULL;
    if (!t || !t->image || x < 0 || y < 0 || x + width > t->width || y + height > t->height) set_error(gl, GL_INVALID_VALUE_);
    else texture_upload(gl, t, x, y, width, height, (const unsigned char*)(uintptr_t)pixels);
    pthread_mutex_unlock(&V.lock);
}

/* the draws */
static void g_draw_arrays(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int first = (int)X(1), count = (int)X(2);
    if (first < 0 || count < 0) {
        set_error(gl, GL_INVALID_VALUE_);
        return;
    }
    draw_elements(gl, (unsigned)X(0), (uint32_t)count, NULL, (uint32_t)first);
}
static void g_draw_elements(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int count = (int)X(1);
    unsigned type = (unsigned)X(2);
    const unsigned char* p = PTR(3);
    if (count <= 0 || !p) return;
    uint32_t* indices = malloc((size_t)count * sizeof *indices);
    if (!indices) {
        set_error(gl, GL_OUT_OF_MEMORY_);
        return;
    }
    for (int i = 0; i < count; ++i) {
        if (type == GL_UNSIGNED_BYTE_) indices[i] = p[i];
        else if (type == GL_UNSIGNED_SHORT_) {
            uint16_t v;
            memcpy(&v, p + 2 * i, 2);
            indices[i] = v;
        } else {
            memcpy(&indices[i], p + 4 * i, 4);
        }
    }
    draw_elements(gl, (unsigned)X(0), (uint32_t)count, indices, 0);
    free(indices);
}
static void g_draw_texi(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    draw_texture_rect(gl, (float)(int32_t)X(0), (float)(int32_t)X(1), (float)(int32_t)X(3), (float)(int32_t)X(4));
}
static void g_draw_texf(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    draw_texture_rect(gl, hle_f(c, 0), hle_f(c, 1), hle_f(c, 3), hle_f(c, 4));
}

/* the state the app reads back */
static void g_get_integerv(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int32_t v[4] = {0, 0, 0, 0};
    int n = 1;
    switch ((unsigned)X(0)) {
    case GL_VIEWPORT_: memcpy(v, gl->viewport, sizeof v), n = 4; break;
    case GL_MAX_TEXTURE_SIZE_: v[0] = (int32_t)(V.max_texture ? V.max_texture : 4096); break;
    case GL_MAX_VIEWPORT_DIMS_: v[0] = v[1] = (int32_t)(V.max_texture ? V.max_texture : 4096), n = 2; break;
    case GL_MAX_TEXTURE_UNITS_: v[0] = 2; break;
    case GL_TEXTURE_BINDING_2D_: v[0] = (int32_t)gl->bound[gl->active]; break;
    case GL_MATRIX_MODE_: v[0] = GL_MODELVIEW_ + gl->mode; break;
    case GL_MAX_MODELVIEW_STACK_DEPTH_:
    case GL_MAX_PROJECTION_STACK_DEPTH_:
    case GL_MAX_TEXTURE_STACK_DEPTH_: v[0] = STACK; break;
    case GL_UNPACK_ALIGNMENT_: v[0] = gl->unpack; break;
    case GL_PACK_ALIGNMENT_: v[0] = 4; break;
    case GL_RED_BITS_:
    case GL_GREEN_BITS_:
    case GL_BLUE_BITS_:
    case GL_ALPHA_BITS_: v[0] = 8; break;
    case GL_DEPTH_BITS_:
    case GL_STENCIL_BITS_: v[0] = 0; break;
    case GL_IMPLEMENTATION_COLOR_READ_TYPE_OES_: v[0] = GL_UNSIGNED_BYTE_; break;
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT_OES_: v[0] = GL_RGBA_; break;
    default: set_error(gl, GL_INVALID_ENUM_); return;
    }
    memcpy(PTR(1), v, (size_t)n * sizeof *v);
}
/* glReadPixels: what the window shows is the GPU's, read back only for a CPU that must see it. Not yet: zeros */
static void g_read_pixels(CPU* c, void* data) {
    (void)data;
    GL_OR_RETURN();
    int width = (int)X(2), height = (int)X(3);
    if (width <= 0 || height <= 0 || !X(6)) return;
    if (!g.warned_read) {
        fprintf(stderr, "scarletnx-android: GLES: glReadPixels reads zeros (no readback of the window yet)\n");
        g.warned_read = true;
    }
    size_t bpp = (unsigned)X(5) == GL_UNSIGNED_BYTE_ ? ((unsigned)X(4) == GL_RGB_ ? 3 : 4) : 2;
    memset(PTR(6), 0, (size_t)width * (size_t)height * bpp);
}
static void g_nothing(CPU* c, void* data) {
    (void)c, (void)data;
}

#define ON ((void*)1)
const HleSymbol hle_gles[] = {
    {"glGetError", g_get_error, NULL},
    {"glEnable", g_enable, ON},
    {"glDisable", g_enable, NULL},
    {"glEnableClientState", g_client_state, ON},
    {"glDisableClientState", g_client_state, NULL},
    {"glVertexPointer", g_vertex_pointer, NULL},
    {"glTexCoordPointer", g_tex_coord_pointer, NULL},
    {"glColorPointer", g_color_pointer, NULL},
    {"glColor4f", g_color4f, NULL},
    {"glColor4ub", g_color4ub, NULL},
    {"glColor4x", g_color4x, NULL},
    {"glClearColor", g_clear_color, NULL},
    {"glClear", g_clear, NULL},
    {"glBlendFunc", g_blend_func, NULL},
    {"glViewport", g_viewport, NULL},
    {"glScissor", g_scissor, NULL},
    {"glTexEnvf", g_tex_envf, NULL},
    {"glTexEnvi", g_tex_envi, NULL},
    {"glPixelStorei", g_pixel_store, NULL},
    {"glMatrixMode", g_matrix_mode, NULL},
    {"glLoadIdentity", g_load_identity, NULL},
    {"glPushMatrix", g_push_matrix, NULL},
    {"glPopMatrix", g_pop_matrix, NULL},
    {"glLoadMatrixf", g_load_matrixf, NULL},
    {"glMultMatrixf", g_mult_matrixf, NULL},
    {"glOrthof", g_orthof, NULL},
    {"glOrthox", g_orthox, NULL},
    {"glTranslatef", g_translatef, NULL},
    {"glScalef", g_scalef, NULL},
    {"glRotatef", g_rotatef, NULL},
    {"glGenTextures", g_gen_textures, NULL},
    {"glDeleteTextures", g_delete_textures, NULL},
    {"glBindTexture", g_bind_texture, NULL},
    {"glActiveTexture", g_active_texture, NULL},
    {"glTexParameteri", g_tex_parameteri, NULL},
    {"glTexParameterf", g_tex_parameterf, NULL},
    {"glTexParameteriv", g_tex_parameteriv, NULL},
    {"glTexImage2D", g_tex_image, NULL},
    {"glTexSubImage2D", g_tex_sub_image, NULL},
    {"glDrawArrays", g_draw_arrays, NULL},
    {"glDrawElements", g_draw_elements, NULL},
    {"glDrawTexiOES", g_draw_texi, NULL},
    {"glDrawTexfOES", g_draw_texf, NULL},
    {"glGetIntegerv", g_get_integerv, NULL},
    {"glReadPixels", g_read_pixels, NULL},
    /* state that has no effect here (the header's list), and the calls that wait for the GPU, which the next swap
       does in order anyway */
    {"glShadeModel", g_nothing, NULL},
    {"glHint", g_nothing, NULL},
    {"glDepthMask", g_nothing, NULL},
    {"glDepthFunc", g_nothing, NULL},
    {"glColorMask", g_nothing, NULL},
    {"glAlphaFunc", g_nothing, NULL},
    {"glCullFace", g_nothing, NULL},
    {"glFrontFace", g_nothing, NULL},
    {"glFlush", g_nothing, NULL},
    {"glFinish", g_nothing, NULL},
    {"glClientActiveTexture", g_nothing, NULL},
    {"", NULL, NULL},
};
#undef ON
