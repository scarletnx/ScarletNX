/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2016-2026 melonDS team
   Copyright 2026 the ScarletNX contributors */
/* The 3D engine's picture, drawn on the GPU from the polygons the geometry engine swapped in (gpu/gpu3d.c). The way a
   frame is drawn is melonDS's OpenGL renderer's (GPU3D_OpenGL.cpp at 497aa6c, GPL-3.0-or-later): the polygons as
   triangles of vertices already on the screen, the opaque pixels first, then the translucent ones in the polygons'
   order, the stencil buffer holding each pixel's polygon ID so a translucent polygon does not blend with itself and a
   shadow falls only where its mask says, then edge marking and fog as passes over the picture. Where melonDS decodes
   textures on the CPU into a cache, the fragment shader here decodes each texel from a copy of the texture memory
   (shaders/texture.glsl), so the CPU's part of a texture is one copy of memory a frame. */
#include "nds/gpu/vk/vk.h"
#include "nds/gpu/gpu3d.h"
#include "nds/gpu/gpu3d_state.h"
#include "nds/gpu/vk/shaders/poly_spv.h"
#include "nds/gpu/vk/shaders/post_spv.h"
#include <stdio.h>
#include <string.h>

#define MAX_VERTICES 24576u
#define MAX_INDICES  65536u
#define VERTEX_BYTES 28u
/* a submission's staging: the texture memory, the registers, the vertices and the indices */
#define STAGE_CONFIG   NDS_VK_3D_BYTES
#define STAGE_VERTICES (STAGE_CONFIG + 0x1000u)
#define STAGE_INDICES  (STAGE_VERTICES + MAX_VERTICES * VERTEX_BYTES)
#define STAGE_BYTES    (STAGE_INDICES + MAX_INDICES * 2u)

typedef struct {
    float toon[32][4], edge[8][4], fog_color[4], fog_density[36];
    int32_t disp_cnt, fog_offset, fog_shift, alpha_ref;
} Config;

typedef struct {
    const NdsPolygon* poly;
    uint32_t first_index, index_count, key;
} Drawn;

enum { BLEND_NONE, BLEND_ALPHA, BLEND_REPLACE, BLEND_EDGE, BLEND_FOG, BLEND_FOG_ALPHA, BLEND_COUNT };
enum { PIPE_POLYGON, PIPE_STENCIL, PIPE_POST, PIPE_KINDS };

static struct {
    NdsVkImage color, attribute, depth;
    VkImageView depth_view; /* the depth alone, for the passes that read it */
    VkSampler sampler;
    NdsVkBuffer staging[NDS_VK_SLOTS], textures;
    VkDescriptorSetLayout set;
    VkPipelineLayout layout;
    VkShaderModule poly_vert, poly_frag, screen_vert, post_frag, stencil_frag;
    VkPipeline pipelines[PIPE_KINDS][BLEND_COUNT][2][16]; /* by kind, blend, the color's mask and the attributes' */
    Drawn drawn[2048];
    uint32_t drawn_count;
    int first_translucent;
    uint64_t frames;
    bool up, cleared, told_bitmap;
} g_3d;

NdsVkImage* nds_vk_render3d_image(void) {
    NdsVk* v = nds_vk();
    if (v && g_3d.up && !g_3d.cleared) { /* nothing drawn yet: a picture with nothing in it */
        VkCommandBuffer cb = snx_vk_ring_commands(&v->ring);
        VkClearColorValue clear = {{0, 0, 0, 0}};
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        nds_vk_layout(cb, &g_3d.color, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdClearColorImage(cb, g_3d.color.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        g_3d.cleared = true;
    }
    return &g_3d.color;
}

static VkPipeline pipeline(unsigned kind, unsigned blend, bool color_on, unsigned attribute_mask) {
    NdsVk* v = nds_vk();
    VkPipeline* slot = &g_3d.pipelines[kind][blend][color_on][attribute_mask & 15];
    if (*slot) return *slot;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = kind == PIPE_POLYGON ? g_3d.poly_vert : g_3d.screen_vert,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = kind == PIPE_POLYGON ? g_3d.poly_frag : kind == PIPE_POST ? g_3d.post_frag : g_3d.stencil_frag,
         .pName = "main"}};
    VkVertexInputBindingDescription binding = {0, VERTEX_BYTES, VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[4] = {{0, 0, VK_FORMAT_R16G16B16A16_UINT, 0},
                                                       {1, 0, VK_FORMAT_R8G8B8A8_UINT, 8},
                                                       {2, 0, VK_FORMAT_R16G16_SINT, 12},
                                                       {3, 0, VK_FORMAT_R32G32B32_SINT, 16}};
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    if (kind == PIPE_POLYGON) {
        input.vertexBindingDescriptionCount = 1, input.pVertexBindingDescriptions = &binding;
        input.vertexAttributeDescriptionCount = 4, input.pVertexAttributeDescriptions = attributes;
    }
    VkPipelineInputAssemblyStateCreateInfo assembly = {.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                       .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                     .polygonMode = VK_POLYGON_MODE_FILL,
                                                     .cullMode = VK_CULL_MODE_NONE,
                                                     .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                   .depthTestEnable = kind != PIPE_POST,
                                                   .stencilTestEnable = kind != PIPE_POST};
    VkPipelineColorBlendAttachmentState blends[2] = {{.colorWriteMask = color_on ? 0xF : 0},
                                                     {.colorWriteMask = attribute_mask & 15}};
    VkPipelineColorBlendAttachmentState* b = &blends[0];
    b->blendEnable = blend != BLEND_NONE;
    b->colorBlendOp = VK_BLEND_OP_ADD, b->alphaBlendOp = VK_BLEND_OP_ADD;
    switch (blend) {
        case BLEND_ALPHA:
        case BLEND_REPLACE:
            b->srcColorBlendFactor = blend == BLEND_ALPHA ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
            b->dstColorBlendFactor = blend == BLEND_ALPHA ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ZERO;
            b->srcAlphaBlendFactor = b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            b->alphaBlendOp = VK_BLEND_OP_MAX;
            break;
        case BLEND_EDGE:
            b->srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA, b->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            b->srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            break;
        case BLEND_FOG:
        case BLEND_FOG_ALPHA:
            b->srcColorBlendFactor = blend == BLEND_FOG ? VK_BLEND_FACTOR_CONSTANT_COLOR : VK_BLEND_FACTOR_ZERO;
            b->dstColorBlendFactor = blend == BLEND_FOG ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE;
            b->srcAlphaBlendFactor = VK_BLEND_FACTOR_CONSTANT_ALPHA;
            b->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        default: break;
    }
    VkPipelineColorBlendStateCreateInfo blending = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                    .attachmentCount = kind == PIPE_POST ? 1 : 2,
                                                    .pAttachments = blends};
    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                                 VK_DYNAMIC_STATE_BLEND_CONSTANTS,    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                                 VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,   VK_DYNAMIC_STATE_STENCIL_OP,
                                 VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                                 VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = sizeof dynamics / sizeof *dynamics,
                                                .pDynamicStates = dynamics};
    VkFormat formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                               .colorAttachmentCount = kind == PIPE_POST ? 1 : 2,
                                               .pColorAttachmentFormats = formats};
    if (kind != PIPE_POST) rendering.depthAttachmentFormat = rendering.stencilAttachmentFormat = v->depth_format;
    VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .pNext = &rendering,
                                         .stageCount = 2,
                                         .pStages = stages,
                                         .pVertexInputState = &input,
                                         .pInputAssemblyState = &assembly,
                                         .pViewportState = &viewport,
                                         .pRasterizationState = &raster,
                                         .pMultisampleState = &multisample,
                                         .pDepthStencilState = &depth,
                                         .pColorBlendState = &blending,
                                         .pDynamicState = &dynamic,
                                         .layout = g_3d.layout};
    if (vkCreateGraphicsPipelines(v->device, VK_NULL_HANDLE, 1, &info, NULL, slot) != VK_SUCCESS) *slot = VK_NULL_HANDLE;
    return *slot;
}

/* ---- the frame's state, as each draw sets it ---- */

static void use(VkCommandBuffer cb, unsigned kind, unsigned blend, bool color_on, unsigned attribute_mask) {
    VkPipeline p = pipeline(kind, blend, color_on, attribute_mask);
    if (p) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
}
static void mode(VkCommandBuffer cb, int32_t value) {
    vkCmdPushConstants(cb, g_3d.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &value);
}
static void depth(VkCommandBuffer cb, VkCompareOp op, bool write) {
    vkCmdSetDepthCompareOp(cb, op);
    vkCmdSetDepthWriteEnable(cb, write);
}
/* OpenGL's glStencilFunc, glStencilOp and glStencilMask */
static void stencil(VkCommandBuffer cb, VkCompareOp func, uint32_t reference, uint32_t compare, VkStencilOp fail,
                    VkStencilOp depth_fail, VkStencilOp pass, uint32_t write) {
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, fail, pass, depth_fail, func);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, compare);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, write);
}
static VkCompareOp depth_op(const NdsPolygon* p) {
    /* the "equal" test has a margin on the console: less or equal, as melonDS has it */
    return p->attr & (1u << 14) ? VK_COMPARE_OP_LESS_OR_EQUAL : VK_COMPARE_OP_LESS;
}
static int draw_one(VkCommandBuffer cb, uint32_t i) {
    const Drawn* d = &g_3d.drawn[i];
    if (d->index_count) vkCmdDrawIndexed(cb, d->index_count, 1, d->first_index, 0, 0);
    return 1;
}
/* the polygons from i on that are drawn the same way, in one draw */
static int draw_batch(VkCommandBuffer cb, uint32_t i) {
    uint32_t key = g_3d.drawn[i].key, count = 0, end = i;
    for (; end < g_3d.drawn_count && g_3d.drawn[end].key == key; ++end) count += g_3d.drawn[end].index_count;
    if (count) vkCmdDrawIndexed(cb, count, 1, g_3d.drawn[i].first_index, 0, 0);
    return (int)(end - i);
}

/* how a polygon is drawn: polygons with the same key can share a draw (melonDS's SetupPolygon) */
static uint32_t render_key(const NdsPolygon* p) {
    uint32_t key = (p->attr >> 14) & 1;
    if (p->is_shadow_mask) return key | 0x30000;
    if (p->translucent) {
        key |= p->is_shadow ? 0x20000 : 0x10000;
        key |= (p->attr >> 10) & 2;
        key |= (p->attr >> 13) & 4;
        key |= (p->attr & 0x3F000000) >> 16;
        if ((p->attr & 0x001F0000) == 0x001F0000) key |= 0x4000;
    } else {
        if (!(p->attr & 0x001F0000)) key |= 2;
        key |= (p->attr & 0x3F000000) >> 16;
    }
    return key;
}

static unsigned char* put_vertex(unsigned char* out, const NdsPolygon* p, unsigned n, uint32_t attribute) {
    const NdsVertex* vtx = p->vertices[n];
    uint32_t z = (uint32_t)p->final_z[n], w = (uint32_t)p->final_w[n], shift = 0, alpha = (p->attr >> 16) & 0x1F;
    while (z > 0xFFFF) z >>= 1, ++shift; /* 16 bits and a shift the vertex shader undoes */
    if (!alpha) alpha = 31;              /* a wireframe polygon: drawn filled */
    uint16_t position[4] = {(uint16_t)vtx->final_position[0], (uint16_t)vtx->final_position[1], (uint16_t)z, (uint16_t)w};
    uint8_t color[4] = {(uint8_t)(vtx->final_color[0] >> 1), (uint8_t)(vtx->final_color[1] >> 1),
                        (uint8_t)(vtx->final_color[2] >> 1), (uint8_t)alpha};
    int32_t attributes[3] = {(int32_t)(attribute | shift << 16), (int32_t)p->tex_param, (int32_t)p->tex_palette};
    memcpy(out, position, 8);
    memcpy(out + 8, color, 4);
    memcpy(out + 12, vtx->tex_coords, 4);
    memcpy(out + 16, attributes, 12);
    return out + VERTEX_BYTES;
}

/* the frame's polygons as triangles: each polygon a fan from its first vertex */
static void build(const NdsGpu3D* g, unsigned char* vertices, uint16_t* indices, uint32_t* vertex_count,
                  uint32_t* index_count) {
    uint32_t v = 0, i = 0;
    g_3d.drawn_count = 0;
    g_3d.first_translucent = -1;
    for (uint32_t n = 0; n < g->render_num_polygons; ++n) {
        const NdsPolygon* p = g->render_polygon_ram[n];
        /* a line (two of a triangle's vertices at one place) has no area: melonDS draws it as a line, which waits */
        if (p->degenerate || p->num_vertices < 3 || p->type == 1) continue;
        if (v + p->num_vertices > MAX_VERTICES || i + (p->num_vertices - 2) * 3 > MAX_INDICES) break;
        Drawn* d = &g_3d.drawn[g_3d.drawn_count];
        *d = (Drawn){.poly = p, .first_index = i, .key = render_key(p)};
        if (g_3d.first_translucent < 0 && p->translucent) g_3d.first_translucent = (int)g_3d.drawn_count;
        ++g_3d.drawn_count;
        uint32_t attribute = p->attr & 0x1F00C8F0;
        if (p->facing_view) attribute |= 1u << 8;
        if (p->w_buffer) attribute |= 1u << 9;
        uint32_t first = v;
        for (unsigned k = 0; k < p->num_vertices; ++k) {
            vertices = put_vertex(vertices, p, k, attribute);
            if (k >= 2) {
                indices[i++] = (uint16_t)first, indices[i++] = (uint16_t)(v - 1), indices[i++] = (uint16_t)v;
                d->index_count += 3;
            }
            ++v;
        }
    }
    *vertex_count = v, *index_count = i;
}

/* the scene, as melonDS's RenderSceneChunk draws it. Stencil values: 0xFF the clear plane, 0xFE a shadow mask against
   it, 00pppppp an opaque polygon's ID, 01pppppp a translucent one's, bit 7 a shadow mask */
static void draw_scene(VkCommandBuffer cb, const NdsGpu3D* g) {
    unsigned fog = g->render_disp_cnt & (1u << 7) ? VK_COLOR_COMPONENT_B_BIT : 0;
    unsigned opaque_attributes = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | fog;
    const VkStencilOp KEEP = VK_STENCIL_OP_KEEP, REPLACE = VK_STENCIL_OP_REPLACE, INVERT = VK_STENCIL_OP_INVERT;
    /* pass 1: the opaque pixels */
    use(cb, PIPE_POLYGON, BLEND_NONE, true, opaque_attributes);
    mode(cb, 0);
    for (uint32_t i = 0; i < g_3d.drawn_count;) {
        const NdsPolygon* p = g_3d.drawn[i].poly;
        if (p->is_shadow_mask || p->translucent) {
            ++i;
            continue;
        }
        depth(cb, depth_op(p), true);
        stencil(cb, VK_COMPARE_OP_ALWAYS, (p->attr >> 24) & 0x3F, 0xFF, KEEP, KEEP, REPLACE, 0xFF);
        i += (uint32_t)draw_batch(cb, i);
    }
    if (g_3d.first_translucent < 0) return;
    unsigned blended = g->render_disp_cnt & (1u << 3) ? BLEND_ALPHA : BLEND_REPLACE;
    /* pass 2: translucent pixels over a clear plane with no alpha, which are not blended with it */
    if (!(g->render_clear_attr1 & 0x001F0000)) {
        for (uint32_t i = 0; i < g_3d.drawn_count;) {
            const NdsPolygon* p = g_3d.drawn[i].poly;
            uint32_t id = (p->attr >> 24) & 0x3F;
            if (p->is_shadow_mask) {
                /* where the stencil is 0xFF, 0xFE where the mask fails the depth test */
                use(cb, PIPE_POLYGON, BLEND_NONE, false, 0);
                mode(cb, 2);
                depth(cb, VK_COMPARE_OP_LESS, false);
                stencil(cb, VK_COMPARE_OP_EQUAL, 0xFF, 0xFF, KEEP, INVERT, KEEP, 0x01);
                i += (uint32_t)draw_batch(cb, i);
            } else if (p->translucent) {
                bool need_opaque = (p->attr & 0x001F0000) == 0x001F0000;
                unsigned translucent_fog = p->attr & (1u << 15) ? 0 : fog;
                if (need_opaque) {
                    use(cb, PIPE_POLYGON, BLEND_NONE, true, opaque_attributes);
                    mode(cb, 0);
                    depth(cb, depth_op(p), true);
                    stencil(cb, VK_COMPARE_OP_ALWAYS, id, 0xFF, KEEP, KEEP, REPLACE, 0xFF);
                    draw_one(cb, i);
                }
                mode(cb, 1);
                if (p->is_shadow) {
                    /* a shadow over the clear plane passes only with the plane's polygon ID */
                    if (id != ((g->render_clear_attr1 >> 24) & 0x3F)) {
                        ++i;
                        continue;
                    }
                    use(cb, PIPE_POLYGON, blended, true, translucent_fog);
                    depth(cb, depth_op(p), (p->attr & (1u << 11)) != 0);
                    stencil(cb, VK_COMPARE_OP_EQUAL, 0xFE, 0xFF, KEEP, KEEP, INVERT, ~(0x40u | id) & 0xFF);
                } else {
                    /* over the clear plane (0xFF) or a shadow mask on it (0xFE) */
                    use(cb, PIPE_POLYGON, BLEND_NONE, true, translucent_fog);
                    depth(cb, depth_op(p), (p->attr & (1u << 11)) != 0);
                    stencil(cb, VK_COMPARE_OP_EQUAL, 0xFF, 0xFE, KEEP, KEEP, INVERT, ~(0x40u | id) & 0xFF);
                }
                i += (uint32_t)(need_opaque ? draw_one(cb, i) : draw_batch(cb, i));
            } else
                ++i;
        }
    }
    /* pass 3: the translucent pixels */
    for (uint32_t i = 0; i < g_3d.drawn_count;) {
        const NdsPolygon* p = g_3d.drawn[i].poly;
        uint32_t id = (p->attr >> 24) & 0x3F;
        if (p->is_shadow_mask) {
            /* the stencil's shadow bit cleared everywhere, then set where the mask fails the depth test */
            use(cb, PIPE_STENCIL, BLEND_NONE, false, 0);
            depth(cb, VK_COMPARE_OP_ALWAYS, false);
            stencil(cb, VK_COMPARE_OP_ALWAYS, 0, 0xFF, KEEP, KEEP, VK_STENCIL_OP_ZERO, 0x80);
            vkCmdDraw(cb, 3, 1, 0, 0);
            use(cb, PIPE_POLYGON, BLEND_NONE, false, 0);
            mode(cb, 2);
            depth(cb, VK_COMPARE_OP_LESS, false);
            stencil(cb, VK_COMPARE_OP_ALWAYS, 0x80, 0x80, KEEP, REPLACE, KEEP, 0x80);
            i += (uint32_t)draw_batch(cb, i);
        } else if (p->translucent) {
            bool need_opaque = (p->attr & 0x001F0000) == 0x001F0000;
            unsigned translucent_fog = p->attr & (1u << 15) ? 0 : fog;
            if (need_opaque) {
                use(cb, PIPE_POLYGON, BLEND_NONE, true, opaque_attributes);
                mode(cb, 0);
                depth(cb, depth_op(p), true);
                stencil(cb, VK_COMPARE_OP_ALWAYS, id, 0xFF, KEEP, KEEP, REPLACE, 0xFF);
                draw_one(cb, i);
            }
            mode(cb, 1);
            if (p->is_shadow) {
                /* the shadow bit cleared where the polygon's ID is the pixel's, then the shadow drawn where it is left */
                use(cb, PIPE_POLYGON, BLEND_NONE, false, 0);
                depth(cb, depth_op(p), false);
                stencil(cb, VK_COMPARE_OP_EQUAL, id, 0x3F, KEEP, KEEP, VK_STENCIL_OP_ZERO, 0x80);
                draw_one(cb, i);
                use(cb, PIPE_POLYGON, blended, true, translucent_fog);
                depth(cb, depth_op(p), (p->attr & (1u << 11)) != 0);
                stencil(cb, VK_COMPARE_OP_EQUAL, 0xC0 | id, 0x80, KEEP, KEEP, REPLACE, 0x7F);
                i += (uint32_t)draw_one(cb, i);
            } else {
                /* where the pixel is not this ID's translucent pixel already */
                use(cb, PIPE_POLYGON, blended, true, translucent_fog);
                depth(cb, depth_op(p), (p->attr & (1u << 11)) != 0);
                stencil(cb, VK_COMPARE_OP_NOT_EQUAL, 0x40 | id, 0x7F, KEEP, KEEP, REPLACE, 0x7F);
                i += (uint32_t)(need_opaque ? draw_one(cb, i) : draw_batch(cb, i));
            }
        } else
            ++i;
    }
}

static void fill_config(const NdsGpu3D* g, Config* c) {
    memset(c, 0, sizeof *c);
    for (unsigned i = 0; i < 32; ++i)
        for (unsigned k = 0; k < 3; ++k) c->toon[i][k] = (float)(g->render_toon_table[i] >> (k * 5) & 0x1F) / 31.0f;
    for (unsigned i = 0; i < 8; ++i)
        for (unsigned k = 0; k < 3; ++k) c->edge[i][k] = (float)(g->render_edge_table[i] >> (k * 5) & 0x1F) / 31.0f;
    for (unsigned k = 0; k < 3; ++k) c->fog_color[k] = (float)(g->render_fog_color >> (k * 5) & 0x1F) / 31.0f;
    c->fog_color[3] = (float)(g->render_fog_color >> 16 & 0x1F) / 31.0f;
    for (unsigned i = 0; i < 34; ++i) c->fog_density[i] = (float)g->render_fog_density_table[i] / 127.0f;
    c->disp_cnt = (int32_t)g->render_disp_cnt;
    c->fog_offset = (int32_t)g->render_fog_offset;
    c->fog_shift = (int32_t)g->render_fog_shift;
    c->alpha_ref = g->render_alpha_ref;
}

void nds_vk_render3d(NdsBoard* board) {
    NdsVk* v = nds_vk();
    const NdsGpu3D* g = nds_gpu3d_state(board);
    if (!v || !g_3d.up) return;
    /* the frame the geometry engine swapped in last, drawn once */
    if (g->frames_swapped == g_3d.frames) return;
    g_3d.frames = g->frames_swapped;
    nds_vk_capture_land(board); /* a captured frame may be this one's texture */
    VkCommandBuffer cb = snx_vk_ring_commands(&v->ring);
    NdsVkBuffer* stage = &g_3d.staging[v->ring.slot];
    nds_vram_copy(board, NDS_SPACE_TEXTURE, stage->map);
    nds_vram_copy(board, NDS_SPACE_TEXPAL, stage->map + NDS_VK_TEXPAL);
    Config config;
    fill_config(g, &config);
    memcpy(stage->map + STAGE_CONFIG, &config, sizeof config);
    uint32_t vertex_count = 0, index_count = 0;
    build(g, stage->map + STAGE_VERTICES, (uint16_t*)(void*)(stage->map + STAGE_INDICES), &vertex_count, &index_count);
    if ((g->render_disp_cnt & (1u << 14)) && !g_3d.told_bitmap) {
        g_3d.told_bitmap = true;
        fprintf(stderr, "scarletnx-nds: the game clears the 3D picture with a bitmap, which is drawn as a plain clear\n");
    }
    if (board->trace && !(g_3d.frames % 60))
        fprintf(stderr, "nds 3d: %u polygons (%u drawn), DISP3DCNT %04X, clear %08X %08X, fog %08X offset %X, alpha ref %u\n",
                g->render_num_polygons, g_3d.drawn_count, g->render_disp_cnt, g->render_clear_attr1,
                g->render_clear_attr2, g->render_fog_color, g->render_fog_offset, g->render_alpha_ref);
    if (board->trace && g_3d.frames == 300)
        for (uint32_t i = 0; i < g_3d.drawn_count; ++i) {
            const NdsPolygon* p = g_3d.drawn[i].poly;
            fprintf(stderr, "  poly %u: n %u attr %08X tex %08X pal %04X y %d..%d x %d w %d z %06X color %03X %03X %03X st %d %d%s\n",
                    i, p->num_vertices, p->attr, p->tex_param, p->tex_palette, p->y_top, p->y_bottom,
                    p->vertices[0]->final_position[0], p->final_w[0], p->final_z[0], p->vertices[0]->final_color[0],
                    p->vertices[0]->final_color[1], p->vertices[0]->final_color[2], p->vertices[0]->tex_coords[0],
                    p->vertices[0]->tex_coords[1], p->translucent ? " translucent" : "");
        }
    VkBufferCopy copy = {0, 0, NDS_VK_3D_BYTES};
    vkCmdCopyBuffer(cb, stage->buffer, g_3d.textures.buffer, 1, &copy);
    nds_vk_buffer_barrier(cb);
    VkDescriptorSet set = snx_vk_ring_set(&v->ring, g_3d.set);
    VkDescriptorBufferInfo config_info = {stage->buffer, STAGE_CONFIG, sizeof config};
    VkDescriptorBufferInfo texture_info = {g_3d.textures.buffer, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo images[2] = {
        {g_3d.sampler, g_3d.attribute.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {g_3d.sampler, g_3d.depth_view, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[3] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                       .pBufferInfo = &config_info},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 1,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = &texture_info},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 2,
                                       .descriptorCount = 2,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = images}};
    vkUpdateDescriptorSets(v->device, 3, writes, 0, NULL);
    /* the clear plane: its color and alpha, its polygon ID and fog bit, its depth, and the stencil's 0xFF */
    uint32_t a1 = g->render_clear_attr1;
    VkClearValue clear_color = {.color = {{(float)(a1 & 0x1F) / 31.0f, (float)(a1 >> 5 & 0x1F) / 31.0f,
                                           (float)(a1 >> 10 & 0x1F) / 31.0f, (float)(a1 >> 16 & 0x1F) / 31.0f}}};
    VkClearValue clear_attribute = {.color = {{(float)(a1 >> 24 & 0x3F) / 63.0f, 0, (float)(a1 >> 15 & 1), 1}}};
    VkClearValue clear_depth = {
        .depthStencil = {(float)(((g->render_clear_attr2 & 0x7FFF) * 0x200u) + 0x1FFu) / 16777216.0f, 0xFF}};
    nds_vk_layout(cb, &g_3d.color, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    nds_vk_layout(cb, &g_3d.attribute, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    nds_vk_layout(cb, &g_3d.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo colors[2] = {{.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                            .imageView = g_3d.color.view,
                                            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                            .clearValue = clear_color},
                                           {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                            .imageView = g_3d.attribute.view,
                                            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                            .clearValue = clear_attribute}};
    VkRenderingAttachmentInfo depth_attachment = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                                  .imageView = g_3d.depth.view,
                                                  .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                                  .clearValue = clear_depth};
    VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                                 .renderArea = {{0, 0}, {NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT}},
                                 .layerCount = 1,
                                 .colorAttachmentCount = 2,
                                 .pColorAttachments = colors,
                                 .pDepthAttachment = &depth_attachment,
                                 .pStencilAttachment = &depth_attachment};
    VkViewport viewport = {0, 0, NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT, 0, 1};
    VkRect2D scissor = {{0, 0}, {NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT}};
    float constants[4] = {config.fog_color[0], config.fog_color[1], config.fog_color[2], config.fog_color[3]};
    vkCmdBeginRendering(cb, &rendering);
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdSetBlendConstants(cb, constants);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_3d.layout, 0, 1, &set, 0, NULL);
    if (g_3d.drawn_count) {
        VkDeviceSize at = STAGE_VERTICES;
        vkCmdBindVertexBuffers(cb, 0, 1, &stage->buffer, &at);
        vkCmdBindIndexBuffer(cb, stage->buffer, STAGE_INDICES, VK_INDEX_TYPE_UINT16);
        draw_scene(cb, g);
    }
    vkCmdEndRendering(cb);
    /* edge marking and fog, over the picture */
    if (g->render_disp_cnt & 0xA0) {
        nds_vk_layout(cb, &g_3d.attribute, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        nds_vk_layout(cb, &g_3d.depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
        colors[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        rendering.colorAttachmentCount = 1;
        rendering.pDepthAttachment = rendering.pStencilAttachment = NULL;
        vkCmdBeginRendering(cb, &rendering);
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdSetBlendConstants(cb, constants);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g_3d.layout, 0, 1, &set, 0, NULL);
        depth(cb, VK_COMPARE_OP_ALWAYS, false);
        stencil(cb, VK_COMPARE_OP_ALWAYS, 0, 0, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, 0);
        if (g->render_disp_cnt & (1u << 5)) {
            use(cb, PIPE_POST, BLEND_EDGE, true, 0);
            mode(cb, 1);
            vkCmdDraw(cb, 3, 1, 0, 0);
        }
        if (g->render_disp_cnt & (1u << 7)) {
            use(cb, PIPE_POST, g->render_disp_cnt & (1u << 6) ? BLEND_FOG_ALPHA : BLEND_FOG, true, 0);
            mode(cb, 2);
            vkCmdDraw(cb, 3, 1, 0, 0);
        }
        vkCmdEndRendering(cb);
    }
    g_3d.cleared = true;
    /* sent now: the GPU draws while the game goes on to the next frame */
    (void)snx_vk_ring_submit(&v->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
}

bool nds_vk_render3d_init(char* error, size_t error_size) {
    NdsVk* v = nds_vk();
    VkImageUsageFlags attachment = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    bool ok = nds_vk_image(&g_3d.color, VK_FORMAT_R8G8B8A8_UNORM, NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT,
                           attachment | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                               VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
              nds_vk_image(&g_3d.attribute, VK_FORMAT_R8G8B8A8_UNORM, NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT, attachment,
                           VK_IMAGE_ASPECT_COLOR_BIT) &&
              nds_vk_image(&g_3d.depth, v->depth_format, NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT,
                           VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                           VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = g_3d.depth.image,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = v->depth_format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}};
    VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                   .magFilter = VK_FILTER_NEAREST,
                                   .minFilter = VK_FILTER_NEAREST,
                                   .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                   .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    ok = ok && vkCreateImageView(v->device, &view, NULL, &g_3d.depth_view) == VK_SUCCESS &&
         vkCreateSampler(v->device, &sampler, NULL, &g_3d.sampler) == VK_SUCCESS;
    for (unsigned i = 0; ok && i < NDS_VK_SLOTS; ++i)
        ok = nds_vk_buffer(&g_3d.staging[i], STAGE_BYTES,
                           VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                           true);
    ok = ok && nds_vk_buffer(&g_3d.textures, NDS_VK_3D_BYTES,
                             VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
    VkDescriptorSetLayoutBinding bindings[4] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 4,
                                           .pBindings = bindings};
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4};
    ok = ok && vkCreateDescriptorSetLayout(v->device, &set, NULL, &g_3d.set) == VK_SUCCESS;
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &g_3d.set,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &push};
    ok = ok && vkCreatePipelineLayout(v->device, &layout, NULL, &g_3d.layout) == VK_SUCCESS &&
         snx_vk_shader_module(v->device, snx_nds_poly_vert_spv, (size_t)snx_nds_poly_vert_spv_size, &g_3d.poly_vert) ==
             VK_SUCCESS &&
         snx_vk_shader_module(v->device, snx_nds_poly_frag_spv, (size_t)snx_nds_poly_frag_spv_size, &g_3d.poly_frag) ==
             VK_SUCCESS &&
         snx_vk_shader_module(v->device, snx_nds_screen_vert_spv, (size_t)snx_nds_screen_vert_spv_size,
                              &g_3d.screen_vert) == VK_SUCCESS &&
         snx_vk_shader_module(v->device, snx_nds_post_frag_spv, (size_t)snx_nds_post_frag_spv_size, &g_3d.post_frag) ==
             VK_SUCCESS &&
         snx_vk_shader_module(v->device, snx_nds_stencil_frag_spv, (size_t)snx_nds_stencil_frag_spv_size,
                              &g_3d.stencil_frag) == VK_SUCCESS;
    if (!ok) {
        snprintf(error, error_size, "cannot make the 3D engine's images, buffers and shaders on the Vulkan device");
        nds_vk_render3d_free();
        return false;
    }
    g_3d.up = true;
    return true;
}

void nds_vk_render3d_free(void) {
    NdsVk* v = nds_vk();
    if (!v) return;
    for (unsigned k = 0; k < PIPE_KINDS; ++k)
        for (unsigned b = 0; b < BLEND_COUNT; ++b)
            for (unsigned c = 0; c < 2; ++c)
                for (unsigned a = 0; a < 16; ++a)
                    if (g_3d.pipelines[k][b][c][a]) vkDestroyPipeline(v->device, g_3d.pipelines[k][b][c][a], NULL);
    VkShaderModule modules[] = {g_3d.poly_vert, g_3d.poly_frag, g_3d.screen_vert, g_3d.post_frag, g_3d.stencil_frag};
    for (size_t i = 0; i < sizeof modules / sizeof *modules; ++i)
        if (modules[i]) vkDestroyShaderModule(v->device, modules[i], NULL);
    if (g_3d.layout) vkDestroyPipelineLayout(v->device, g_3d.layout, NULL);
    if (g_3d.set) vkDestroyDescriptorSetLayout(v->device, g_3d.set, NULL);
    if (g_3d.sampler) vkDestroySampler(v->device, g_3d.sampler, NULL);
    if (g_3d.depth_view) vkDestroyImageView(v->device, g_3d.depth_view, NULL);
    for (unsigned i = 0; i < NDS_VK_SLOTS; ++i) nds_vk_buffer_free(&g_3d.staging[i]);
    nds_vk_buffer_free(&g_3d.textures);
    nds_vk_image_free(&g_3d.color);
    nds_vk_image_free(&g_3d.attribute);
    nds_vk_image_free(&g_3d.depth);
    memset(&g_3d, 0, sizeof g_3d);
}
