/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's draws on the GPU (vk_render.h). The register meanings and their Vulkan equivalents follow Azahar's
   Vulkan renderer (video_core/renderer_vulkan/vk_rasterizer.cpp, vk_pipeline_cache.cpp, pica_to_vk.h,
   GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project, at azahar-emu/azahar
   a4b335e); the targets and textures live in the guest's memory, as gpu/raster.c and gpu/texture.c (the CPU
   reference) read and write them. An image's row r is the rasterizer's y = r (and a texture's row t its coordinate
   t), so the viewport maps the PICA's window coordinates as they are, and the guest's rows (from the top, y =
   height - 1, unless the framebuffer is flipped) are the compute shaders' business. */
#include "3ds/gpu/vk/vk_render.h"
#include "3ds/gpu/vk/fsgen.h"
#include "3ds/gpu/vk/shaders/color_spv.h"
#include "3ds/gpu/vk/shaders/depth_spv.h"
#include "3ds/gpu/vk/shaders/texture_spv.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vsgen.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_SIZE (32u << 20) /* the uploads, a part for each submission slot: uniforms, LUTs, default and immediate
                                 vertices */
#define RING_PART (RING_SIZE / SNX_CTR_VK_SLOTS)
#define TEXTURES  512 /* textures kept decoded */
#define TARGETS   32  /* render targets kept */
#define SPANS     4096 /* the memory spans a submission reads or writes, re-levelled when it is done */

enum {
    CULL_MODE = 0x040,
    VIEWPORT_X = 0x041,
    VIEWPORT_Y = 0x043,
    VIEWPORT_CORNER = 0x068,
    TEX_MAIN = 0x080,
    TEX_CUBE = 0x086, /* the other five faces' addresses */
    OM_MODE = 0x100,
    OM_BLEND = 0x101,
    OM_LOGIC = 0x102,
    OM_BLEND_CONST = 0x103,
    OM_STENCIL = 0x105,
    OM_STENCIL_OP = 0x106,
    OM_DEPTH_COLOR = 0x107,
    FB_COLOR_WRITE = 0x113,
    FB_DEPTH_WRITE = 0x115,
    FB_DEPTH_FORMAT = 0x116,
    FB_COLOR_FORMAT = 0x117,
    FB_DEPTH_ADDR = 0x11c,
    FB_COLOR_ADDR = 0x11d,
    FB_SIZE = 0x11e,
    ATTR_BASE = 0x200,
    ATTR_FORMAT_LO = 0x201,
    ATTR_FORMAT_HI = 0x202,
    ATTR_LOADERS = 0x203,
    INDEX_ARRAY = 0x227,
    NUM_VERTICES = 0x228,
    VERTEX_OFFSET = 0x22a
};
static const uint32_t tex_config[3] = {0x081, 0x091, 0x099}, tex_format[3] = {0x08e, 0x096, 0x09e};
enum { FMT_BYTE, FMT_UBYTE, FMT_SHORT, FMT_FLOAT };
enum { DEFAULTS = 12, ZERO_SLOT = 16 }; /* the binding of the default attributes, and its slot of zeros */
enum { NO_DEPTH = 0xff };
/* a draw that ran out of the recording's descriptors, uploads or textures: finished, it is recorded again */
#define AGAIN ((SnxCtrPicaResult)-1)

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;       /* every level and layer: sampled (2D or cube) or attached */
    VkImageView levels[11]; /* textures: each level as an array, for the decoder to store into */
    uint32_t width, height, level_count, layers;
} Image;

typedef struct {
    uint32_t color_pa, depth_pa, width, height;
    uint8_t color_format, depth_format, flip, pad; /* color in transfer.h's order; depth 0 D16, 2 D24, 3 D24S8 */
} TargetKey;
/* A render target kept: its images hold memory's contents when valid, and newer ones when dirty (drawn into since they
   were loaded or stored), whose memory is the GPU's until they are stored */
typedef struct {
    TargetKey key;
    Image color, depth;
    VkBuffer scratch; /* depth words, then stencil bytes from stencil_at, between the image and the guest's layout */
    VkDeviceMemory scratch_memory;
    VkDeviceSize stencil_at;
    bool color_valid, depth_valid, color_dirty, depth_dirty;
    uint64_t used; /* the last submission's serial that drew into it or read it */
} Target;

/* A texture kept decoded while its memory is unchanged (valid); an invalid one's image is reused */
typedef struct {
    uint32_t pa[6], format, width, height, levels, bytes;
    bool cube, valid;
    Image image;
    uint64_t used;
} Texture;
/* What the GPU keeps of a RAM page and does with it: the submissions that last read and wrote it, the targets and
   textures kept of it, the dirty targets over it. Its write-tracking level follows (levels()) */
typedef struct {
    uint64_t read, write;
    uint16_t kept, dirty;
} PageState;
typedef struct {
    uint32_t pa, size;
} Span;
/* an image (or a target's scratch buffer) no longer kept, destroyed once the submissions that may use it are done */
typedef struct {
    Image image;
    VkBuffer buffer;
    VkDeviceMemory memory;
    uint64_t serial;
} Dead;

/* what picks a pipeline besides its shaders: plain data, zeroed before it is filled */
typedef struct {
    uint32_t vs, fs; /* indices into the shader caches */
    uint32_t blend, logic, depth_color, stencil, stencil_op;
    uint8_t blend_on, color_mask, depth_write, stencil_on;
    uint8_t topology, cull, depth, pad;
    uint16_t binding_mask, pad2;
    uint32_t stride[13];
    struct {
        uint8_t binding, format;
        uint16_t offset;
    } attribute[16];
} PipeKey;

typedef struct {
    uint64_t hash;
    SnxCtrVsConfig config;
    VkShaderModule module;
    const char* why;
} VsShader;
typedef struct {
    uint64_t hash;
    SnxCtrFsConfig config;
    VkShaderModule module;
    const char* why;
} FsShader;
typedef struct {
    uint64_t hash;
    PipeKey key;
    VkPipeline pipeline;
} Pipe;

typedef struct {
    uint32_t lighting[24][256], fog[128], noise[128], color_map[128], alpha_map[128], color[256], diff[256];
} LutSource;

static struct {
    bool made, failed;
    SnxCtrVkCompute color, depth, texture;
    VkDescriptorSetLayout set0, set1;
    VkPipelineLayout layout;
    VkFormat depth_format;
    bool depth_float;
    VkBuffer ring;
    VkDeviceMemory ring_memory;
    unsigned char* ring_map;
    VkDeviceSize ring_at, align; /* ring_at: in the open slot's part */
    unsigned slot;               /* the open recording's */
    Image dummy, dummy_cube;
    struct {
        uint32_t key;
        VkSampler sampler;
    } samplers[128];
    unsigned sampler_count;
    VsShader* vs;
    size_t vs_count, vs_capacity;
    FsShader* fs;
    size_t fs_count, fs_capacity;
    Pipe* pipes;
    size_t pipe_count, pipe_capacity;
    Target targets[TARGETS];
    unsigned target_count;
    /* the target the draws go to, and whether its rendering pass is open */
    unsigned open;
    bool opened, rendering;
    Texture textures[TEXTURES];
    unsigned texture_count;
    PageState* pages; /* every RAM page of the device's regions, in their order */
    uint32_t page_count;
    Span* spans[SNX_CTR_VK_SLOTS]; /* what each slot's submission reads or writes */
    size_t span_count[SNX_CTR_VK_SLOTS], span_capacity[SNX_CTR_VK_SLOTS];
    Dead* dead;
    size_t dead_count, dead_capacity;
    /* each slot's buffer views, destroyed when its submission is done */
    VkBufferView* views[SNX_CTR_VK_SLOTS];
    size_t view_count[SNX_CTR_VK_SLOTS], view_capacity[SNX_CTR_VK_SLOTS];
    /* the recording's last uploads, reused while they hold the same */
    bool vs_uploaded, fs_uploaded, lut_uploaded;
    SnxCtrVsPica vs_pica;
    SnxCtrVsData vs_data;
    VkDeviceSize vs_at, vs_data_at;
    SnxCtrFsUniforms fs_data;
    VkDeviceSize fs_at;
    LutSource lut_source;
    VkBufferView lut_views[3];
    /* the vertex program's hash, kept until another upload */
    const SnxCtrPica* hashed_pica;
    uint32_t hashed_version;
    uint64_t program_hash;
    bool hashed;
    /* immediate-mode vertices waiting for their draw */
    float (*immediate)[16][4];
    size_t immediate_count, immediate_capacity;
} g;

static VkDevice device(void) { return snx_ctr_vk()->device; }

/* ---- images ---- */

static void image_free(Image* m) {
    VkDevice d = device();
    if (m->view) vkDestroyImageView(d, m->view, NULL);
    for (unsigned i = 0; i < 11; ++i)
        if (m->levels[i]) vkDestroyImageView(d, m->levels[i], NULL);
    if (m->image) vkDestroyImage(d, m->image, NULL);
    if (m->memory) vkFreeMemory(d, m->memory, NULL);
    *m = (Image){0};
}
/* an image in the GENERAL layout, which every use of it here takes: color (RGBA8) or the depth format, with layers 1
   or 6 (a cube), and storage views of each level for a texture */
static bool image_new(VkFormat format, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers,
                      VkImageUsageFlags usage, Image* m) {
    VkDevice d = device();
    bool depth = format != VK_FORMAT_R8G8B8A8_UNORM;
    VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                      : VK_IMAGE_ASPECT_COLOR_BIT;
    *m = (Image){0};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .flags = layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {width, height, 1},
                            .mipLevels = levels,
                            .arrayLayers = layers,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(d, &ii, NULL, &m->image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(d, m->image, &req);
    uint32_t type = snx_ctr_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = snx_ctr_vk_memory_type(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(d, &ai, NULL, &m->memory) != VK_SUCCESS ||
        vkBindImageMemory(d, m->image, m->memory, 0) != VK_SUCCESS) {
        image_free(m);
        return false;
    }
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = m->image,
                                .viewType = layers == 6 ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {aspect, 0, levels, 0, layers}};
    if (vkCreateImageView(d, &vi, NULL, &m->view) != VK_SUCCESS) {
        image_free(m);
        return false;
    }
    if (usage & VK_IMAGE_USAGE_SAMPLED_BIT)
        for (uint32_t i = 0; i < levels && i < 11; ++i) {
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
            vi.subresourceRange = (VkImageSubresourceRange){aspect, i, 1, 0, layers};
            if (vkCreateImageView(d, &vi, NULL, &m->levels[i]) != VK_SUCCESS) {
                image_free(m);
                return false;
            }
        }
    m->width = width;
    m->height = height;
    m->level_count = levels;
    m->layers = layers;
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                              .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = m->image,
                              .subresourceRange = {aspect, 0, levels, 0, layers}};
    vkCmdPipelineBarrier(snx_ctr_vk_commands(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    return true;
}

static bool buffer_new(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer* buffer,
                       VkDeviceMemory* memory) {
    VkDevice d = device();
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = usage,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (vkCreateBuffer(d, &bi, NULL, buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(d, *buffer, &req);
    uint32_t type = snx_ctr_vk_memory_type(req.memoryTypeBits, properties);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(d, &ai, NULL, memory) != VK_SUCCESS ||
        vkBindBufferMemory(d, *buffer, *memory, 0) != VK_SUCCESS) {
        vkDestroyBuffer(d, *buffer, NULL);
        *buffer = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

/* ---- setup ---- */

static bool depth_usable(VkFormat format) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(snx_ctr_vk()->phys, format, &p);
    VkFormatFeatureFlags want = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                                VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return (p.optimalTilingFeatures & want) == want;
}
static bool made(void) {
    if (g.made || g.failed) return g.made;
    SnxCtrVk* v = snx_ctr_vk();
    if (!v) return false;
    g.failed = true;
    if (!snx_ctr_vk_compute(snx_ctr_color_spv, snx_ctr_color_spv_size / 4, 1, 1, 24, &g.color) ||
        !snx_ctr_vk_compute(snx_ctr_depth_spv, snx_ctr_depth_spv_size / 4, 3, 0, 28, &g.depth) ||
        !snx_ctr_vk_compute(snx_ctr_texture_spv, snx_ctr_texture_spv_size / 4, 1, 1, 20, &g.texture))
        return false;
    if (depth_usable(VK_FORMAT_D24_UNORM_S8_UINT))
        g.depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    else if (depth_usable(VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        g.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        g.depth_float = true;
    } else
        return false;
    VkDescriptorSetLayoutBinding set0[6] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorSetLayoutBinding set1[3];
    for (uint32_t i = 0; i < 3; ++i)
        set1[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .bindingCount = 6,
                                          .pBindings = set0};
    if (vkCreateDescriptorSetLayout(v->device, &si, NULL, &g.set0) != VK_SUCCESS) return false;
    si.bindingCount = 3;
    si.pBindings = set1;
    if (vkCreateDescriptorSetLayout(v->device, &si, NULL, &g.set1) != VK_SUCCESS) return false;
    VkDescriptorSetLayout sets[2] = {g.set0, g.set1};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 2,
                                     .pSetLayouts = sets};
    if (vkCreatePipelineLayout(v->device, &li, NULL, &g.layout) != VK_SUCCESS) return false;
    if (!buffer_new(RING_SIZE,
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g.ring,
                    &g.ring_memory) ||
        vkMapMemory(v->device, g.ring_memory, 0, VK_WHOLE_SIZE, 0, (void**)&g.ring_map) != VK_SUCCESS)
        return false;
    const VkPhysicalDeviceLimits* l = &v->properties.limits;
    g.align = 256;
    if (l->minUniformBufferOffsetAlignment > g.align) g.align = l->minUniformBufferOffsetAlignment;
    if (l->minTexelBufferOffsetAlignment > g.align) g.align = l->minTexelBufferOffsetAlignment;
    /* the samplers of disabled textures: transparent black */
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image_new(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 1, usage, &g.dummy) ||
        !image_new(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 6, usage, &g.dummy_cube))
        return false;
    VkClearColorValue black = {{0}};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 6};
    vkCmdClearColorImage(snx_ctr_vk_commands(), g.dummy_cube.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
    all.layerCount = 1;
    vkCmdClearColorImage(snx_ctr_vk_commands(), g.dummy.image, VK_IMAGE_LAYOUT_GENERAL, &black, 1, &all);
    snx_ctr_vk_barrier(snx_ctr_vk_commands());
    g.failed = false;
    g.made = true;
    return true;
}

/* room in the open slot's part of the ring: its offset in the ring */
static bool ring(VkDeviceSize size, VkDeviceSize* at) {
    snx_ctr_vk_commands(); /* the open recording's slot */
    VkDeviceSize start = (g.ring_at + g.align - 1) / g.align * g.align;
    if (start + size > RING_PART) return false;
    *at = (VkDeviceSize)g.slot * RING_PART + start;
    g.ring_at = start + size;
    return true;
}
static bool upload(const void* data, VkDeviceSize size, VkDeviceSize* at) {
    if (!ring(size, at)) return false;
    memcpy(g.ring_map + *at, data, size);
    return true;
}
static bool texel_view(VkDeviceSize at, VkDeviceSize size, VkFormat format, VkBufferView* view) {
    unsigned s = g.slot;
    if (g.view_count[s] == g.view_capacity[s]) {
        size_t n = g.view_capacity[s] ? g.view_capacity[s] * 2 : 64;
        VkBufferView* views = (VkBufferView*)realloc(g.views[s], n * sizeof *views);
        if (!views) return false;
        g.views[s] = views;
        g.view_capacity[s] = n;
    }
    VkBufferViewCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
                                 .buffer = g.ring,
                                 .format = format,
                                 .offset = at,
                                 .range = size};
    if (vkCreateBufferView(device(), &bi, NULL, view) != VK_SUCCESS) return false;
    g.views[s][g.view_count[s]++] = *view;
    return true;
}

/* a compute dispatch over width x height with storage buffers (windows) and an optional storage image */
static bool dispatch(const SnxCtrVkCompute* c, const SnxCtrVkWindow* windows, unsigned count, VkImageView image,
                     const void* push, uint32_t push_size, uint32_t width, uint32_t height) {
    VkDescriptorSet set = snx_ctr_vk_set(c->set);
    if (!set) return false;
    VkDescriptorBufferInfo infos[3];
    VkDescriptorImageInfo image_info = {VK_NULL_HANDLE, image, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[4];
    unsigned n = 0;
    for (unsigned i = 0; i < count; ++i) {
        infos[i] = (VkDescriptorBufferInfo){windows[i].buffer, windows[i].offset, windows[i].range};
        writes[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = i,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .pBufferInfo = &infos[i]};
    }
    if (image)
        writes[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = count,
                                             .descriptorCount = 1,
                                             .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                             .pImageInfo = &image_info};
    vkUpdateDescriptorSets(device(), n, writes, 0, NULL);
    VkCommandBuffer cb = snx_ctr_vk_commands();
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
    vkCmdDispatch(cb, (width + 7) / 8, (height + 7) / 8, 1);
    return true;
}

/* ---- render targets and what the GPU keeps of memory ---- */

static unsigned color_bytes(unsigned format) { return format == 0 ? 4 : format == 1 ? 3 : 2; }
static unsigned depth_bytes(unsigned format) { return format == 0 ? 2 : format == 2 ? 3 : 4; }
static bool overlaps(uint32_t a, uint64_t a_size, uint32_t b, uint64_t b_size) {
    return a < (uint64_t)b + b_size && b < (uint64_t)a + a_size;
}
/* a target's colour and depth spans in memory (depth's 0 bytes without one) */
static uint64_t color_span(const TargetKey* k) {
    return (uint64_t)k->width * k->height * color_bytes(k->color_format);
}
static uint64_t depth_span(const TargetKey* k) {
    return k->depth_format == NO_DEPTH ? 0 : (uint64_t)k->width * k->height * depth_bytes(k->depth_format);
}
/* the state of the RAM page holding pa, NULL outside RAM (the pages made at the first use) */
static PageState* page_state(uint32_t pa) {
    SnxCtrVk* v = snx_ctr_vk();
    if (!v) return NULL;
    if (!g.pages) {
        uint32_t n = 0;
        for (unsigned i = 0; i < v->ram_count; ++i) n += v->ram[i].size / 4096;
        g.pages = (PageState*)calloc(n ? n : 1, sizeof *g.pages);
        if (!g.pages) return NULL;
        g.page_count = n;
    }
    uint32_t index = 0;
    for (unsigned i = 0; i < v->ram_count; ++i) {
        if (pa >= v->ram[i].base && pa - v->ram[i].base < v->ram[i].size)
            return &g.pages[index + (pa - v->ram[i].base) / 4096];
        index += v->ram[i].size / 4096;
    }
    return NULL;
}
/* The level write tracking gives each page of [pa, pa + size): the GPU's (OWN) under a dirty target or a write not
   done, watched under something kept or a read not done (a write waits for that read), the CPU's otherwise */
static void levels(uint32_t pa, uint64_t size) {
    SnxCtrVk* v = snx_ctr_vk();
    if (!v || !v->memory || !size) return;
    for (uint64_t at = pa & ~4095u; at < (uint64_t)pa + size; at += 4096) {
        PageState* page = page_state((uint32_t)at);
        if (!page) continue;
        SnxCtrTrackLevel l = page->dirty || page->write > v->ring.completed ? SNX_CTR_TRACK_OWN
                             : page->kept || page->read > v->ring.completed ? SNX_CTR_TRACK_WATCH
                                                                       : SNX_CTR_TRACK_NONE;
        snx_ctr_track_set(v->memory, (uint32_t)at, 4096, l);
    }
}
/* targets and textures kept of [pa, pa + size) (kept), and dirty targets over it (dirty), counted in or out */
static void count(uint32_t pa, uint64_t size, int kept, int dirty) {
    if (!size) return;
    for (uint64_t at = pa & ~4095u; at < (uint64_t)pa + size; at += 4096) {
        PageState* page = page_state((uint32_t)at);
        if (!page) continue;
        page->kept = (uint16_t)(page->kept + kept);
        page->dirty = (uint16_t)(page->dirty + dirty);
    }
    levels(pa, size);
}
/* the open recording reads [pa, pa + size), or writes it: its pages wait for its submission (a CPU write for a read, any
   access for a write), and are levelled again when it is done */
static void gpu_uses(uint32_t pa, uint64_t size, bool write) {
    if (!size || !snx_ctr_vk()) return;
    snx_ctr_vk_commands(); /* the open recording's slot */
    uint64_t serial = snx_ctr_vk_serial();
    for (uint64_t at = pa & ~4095u; at < (uint64_t)pa + size; at += 4096) {
        PageState* page = page_state((uint32_t)at);
        if (!page) continue;
        if (write)
            page->write = serial;
        else
            page->read = serial;
    }
    unsigned slot = g.slot;
    Span* last = g.span_count[slot] ? &g.spans[slot][g.span_count[slot] - 1] : NULL;
    if (last && last->pa <= pa && (uint64_t)last->pa + last->size >= (uint64_t)pa + size) {
        /* within the last one */
    } else if (last && (uint64_t)last->pa + last->size == pa && last->size + size <= UINT32_MAX)
        last->size += (uint32_t)size;
    else {
        if (g.span_count[slot] == g.span_capacity[slot] && g.span_capacity[slot] < SPANS) {
            size_t n = g.span_capacity[slot] ? g.span_capacity[slot] * 2 : 256;
            Span* grown = (Span*)realloc(g.spans[slot], n * sizeof *grown);
            if (grown) {
                g.spans[slot] = grown;
                g.span_capacity[slot] = n;
            }
        }
        if (g.span_count[slot] < g.span_capacity[slot])
            g.spans[slot][g.span_count[slot]++] = (Span){pa, (uint32_t)(size > UINT32_MAX ? UINT32_MAX : size)};
        else if (last) { /* no room: the last one grows over this one too, and more is levelled again than needed */
            uint64_t start = pa < last->pa ? pa : last->pa;
            uint64_t end = (uint64_t)pa + size > (uint64_t)last->pa + last->size ? (uint64_t)pa + size
                                                                                   : (uint64_t)last->pa + last->size;
            last->pa = (uint32_t)start;
            last->size = (uint32_t)(end - start > UINT32_MAX ? UINT32_MAX : end - start);
        }
    }
    levels(pa, size);
}
/* A target's colour (or depth) images as kept: holding what memory does (valid), or newer (dirty). Every change goes
   through here, so the pages count them */
static void set_color(Target* t, bool valid, bool dirty) {
    if (valid != t->color_valid) count(t->key.color_pa, color_span(&t->key), valid ? 1 : -1, 0);
    if (dirty != t->color_dirty) count(t->key.color_pa, color_span(&t->key), 0, dirty ? 1 : -1);
    t->color_valid = valid;
    t->color_dirty = dirty;
}
static void set_depth(Target* t, bool valid, bool dirty) {
    if (valid != t->depth_valid) count(t->key.depth_pa, depth_span(&t->key), valid ? 1 : -1, 0);
    if (dirty != t->depth_dirty) count(t->key.depth_pa, depth_span(&t->key), 0, dirty ? 1 : -1);
    t->depth_valid = valid;
    t->depth_dirty = dirty;
}
/* a texture kept decoded, or not: its faces' pages count it */
static void set_texture(Texture* t, bool valid) {
    if (valid == t->valid) return;
    t->valid = valid;
    for (unsigned f = 0; f < (t->cube ? 6u : 1u); ++f) count(t->pa[f], t->bytes, valid ? 1 : -1, 0);
}
static void dead_free(Dead* d) {
    image_free(&d->image);
    if (d->buffer) vkDestroyBuffer(device(), d->buffer, NULL);
    if (d->memory) vkFreeMemory(device(), d->memory, NULL);
}
/* an image or buffer the recordings in flight, the open one included, may still use: destroyed when they are done */
static void bury(Image* image, VkBuffer* buffer, VkDeviceMemory* memory) {
    Dead d = {*image, buffer ? *buffer : VK_NULL_HANDLE, memory ? *memory : VK_NULL_HANDLE, snx_ctr_vk_serial()};
    *image = (Image){0};
    if (buffer) *buffer = VK_NULL_HANDLE;
    if (memory) *memory = VK_NULL_HANDLE;
    if (!d.image.image && !d.buffer) return;
    if (g.dead_count == g.dead_capacity) {
        size_t n = g.dead_capacity ? g.dead_capacity * 2 : 32;
        Dead* grown = (Dead*)realloc(g.dead, n * sizeof *grown);
        if (!grown) { /* no room to keep it: wait for the device instead */
            vkDeviceWaitIdle(device());
            dead_free(&d);
            return;
        }
        g.dead = grown;
        g.dead_capacity = n;
    }
    g.dead[g.dead_count++] = d;
}
static void end_rendering(void) {
    if (!g.rendering) return;
    vkCmdEndRendering(snx_ctr_vk_commands());
    g.rendering = false;
}
/* the target's colour, and its depth and stencil, moved between the guest's memory and its images (store: to memory,
   which is then the GPU's until the recording is done) */
static bool target_move(Target* t, bool store, bool color, bool depth) {
    const TargetKey* k = &t->key;
    VkCommandBuffer cb = snx_ctr_vk_commands();
    VkDeviceSize pixels = (VkDeviceSize)k->width * k->height;
    SnxCtrVkWindow w[3];
    uint32_t inside;
    if (store) end_rendering(); /* its barrier and dispatch cannot be recorded inside a target's pass */
    snx_ctr_vk_barrier(cb);
    if (color && snx_ctr_vk_window(k->color_pa, color_span(k), &w[0], &inside)) {
        uint32_t push[6] = {inside, k->width, k->height, k->color_format, k->flip, store};
        if (!dispatch(&g.color, w, 1, t->color.view, push, sizeof push, k->width, k->height)) return false;
        gpu_uses(k->color_pa, color_span(k), store);
    }
    if (depth && k->depth_format != NO_DEPTH && snx_ctr_vk_window(k->depth_pa, depth_span(k), &w[0], &inside)) {
        VkBufferImageCopy copies[2] = {
            {0, k->width, k->height, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {k->width, k->height, 1}},
            {t->stencil_at, k->width, k->height, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, {0, 0, 0},
             {k->width, k->height, 1}}};
        w[1] = (SnxCtrVkWindow){t->scratch, 0, pixels * 4};
        w[2] = (SnxCtrVkWindow){t->scratch, t->stencil_at, (pixels + 3) / 4 * 4};
        uint32_t push[7] = {inside, k->width, k->height, k->depth_format, k->flip, store, g.depth_float};
        if (store) {
            vkCmdCopyImageToBuffer(cb, t->depth.image, VK_IMAGE_LAYOUT_GENERAL, t->scratch, 2, copies);
            snx_ctr_vk_barrier(cb);
        }
        if (!dispatch(&g.depth, w, 3, VK_NULL_HANDLE, push, sizeof push, k->width, k->height)) return false;
        if (!store) {
            snx_ctr_vk_barrier(cb);
            vkCmdCopyBufferToImage(cb, t->scratch, t->depth.image, VK_IMAGE_LAYOUT_GENERAL, 2, copies);
        }
        gpu_uses(k->depth_pa, depth_span(k), store);
    }
    snx_ctr_vk_barrier(cb);
    return true;
}
/* a dirty target's newer contents written to memory, on the GPU: the target is clean, its memory the GPU's until the
   recording is done */
static bool target_store(Target* t) {
    if (!t->color_dirty && !t->depth_dirty) return true;
    if (!target_move(t, true, t->color_dirty, t->depth_dirty)) return false;
    set_color(t, t->color_valid, false);
    set_depth(t, t->depth_valid, false);
    return true;
}
/* every dirty target over [pa, pa + size) stored */
static bool store_over(uint32_t pa, uint64_t size) {
    for (unsigned i = 0; i < g.target_count; ++i) {
        Target* t = &g.targets[i];
        if (((t->color_dirty && overlaps(pa, size, t->key.color_pa, color_span(&t->key))) ||
             (t->depth_dirty && overlaps(pa, size, t->key.depth_pa, depth_span(&t->key)))) &&
            !target_store(t))
            return false;
    }
    return true;
}
/* the textures decoded from [pa, pa + size) dropped: its memory changed, or is about to */
static void drop_textures(uint32_t pa, uint64_t size) {
    for (unsigned i = 0; i < g.texture_count; ++i) {
        Texture* t = &g.textures[i];
        bool over = false;
        for (unsigned f = 0; t->valid && f < (t->cube ? 6u : 1u); ++f) over = over || overlaps(pa, size, t->pa[f], t->bytes);
        if (over) set_texture(t, false);
    }
}
/* what was kept of [pa, pa + size) is stale (memory changed under it): textures dropped, targets' images to be loaded
   again. A dirty one was stored before */
static void stale(uint32_t pa, uint64_t size) {
    drop_textures(pa, size);
    for (unsigned i = 0; i < g.target_count; ++i) {
        Target* t = &g.targets[i];
        bool color = overlaps(pa, size, t->key.color_pa, color_span(&t->key));
        bool depth = overlaps(pa, size, t->key.depth_pa, depth_span(&t->key));
        if (color) set_color(t, false, t->color_dirty);
        if (depth) set_depth(t, false, t->depth_dirty);
        if ((color || depth) && g.opened && g.open == i) { /* the draws load it again first */
            end_rendering();
            g.opened = false;
        }
    }
}
static bool target_images(Target* t, uint32_t width, uint32_t height) {
    VkDeviceSize pixels = (VkDeviceSize)width * height;
    VkDeviceSize alignment = snx_ctr_vk()->properties.limits.minStorageBufferOffsetAlignment;
    if (alignment < 4) alignment = 4;
    t->stencil_at = (pixels * 4 + alignment - 1) / alignment * alignment;
    if (!image_new(VK_FORMAT_R8G8B8A8_UNORM, width, height, 1, 1,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT, &t->color))
        return false;
    if (!image_new(g.depth_format, width, height, 1, 1,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                       VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                   &t->depth) ||
        !buffer_new(t->stencil_at + (pixels + 3) / 4 * 4,
                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &t->scratch, &t->scratch_memory)) {
        image_free(&t->color);
        image_free(&t->depth);
        return false;
    }
    return true;
}
static void target_free(Target* t) {
    image_free(&t->color);
    image_free(&t->depth);
    if (t->scratch) vkDestroyBuffer(device(), t->scratch, NULL);
    if (t->scratch_memory) vkFreeMemory(device(), t->scratch_memory, NULL);
    t->scratch = VK_NULL_HANDLE;
    t->scratch_memory = VK_NULL_HANDLE;
}
/* a target for key: the one kept, or a new one (the least used one stored and let go of when they are all taken) */
static Target* target_find(const TargetKey* k) {
    for (unsigned i = 0; i < g.target_count; ++i)
        if (!memcmp(&g.targets[i].key, k, sizeof *k)) return &g.targets[i];
    Target* t = NULL;
    if (g.target_count < TARGETS)
        t = &g.targets[g.target_count];
    else {
        for (unsigned i = 0; i < TARGETS; ++i)
            if ((!g.opened || g.open != i) && (!t || g.targets[i].used < t->used)) t = &g.targets[i];
        if (!target_store(t)) return NULL;
        set_color(t, false, false);
        set_depth(t, false, false);
        /* its images may be in the recordings in flight: they go when those are done */
        bury(&t->color, NULL, NULL);
        bury(&t->depth, &t->scratch, &t->scratch_memory);
    }
    *t = (Target){0};
    if (!target_images(t, k->width, k->height)) return NULL;
    t->key = *k;
    if (t == &g.targets[g.target_count]) ++g.target_count;
    return t;
}
/* the draw's target open: the one kept for key, loaded from memory unless its images hold what memory does. Other
   targets over the same memory are stored first, and are stale from now on */
static SnxCtrPicaResult target_open(const TargetKey* k) {
    if (g.opened && !memcmp(k, &g.targets[g.open].key, sizeof *k)) return SNX_CTR_PICA_OK;
    end_rendering();
    g.opened = false;
    Target* t = target_find(k);
    if (!t) return SNX_CTR_PICA_UNSUPPORTED;
    for (unsigned i = 0; i < g.target_count; ++i) {
        Target* u = &g.targets[i];
        if (u == t) continue;
        bool color = overlaps(u->key.color_pa, color_span(&u->key), k->color_pa, color_span(k)) ||
                     overlaps(u->key.color_pa, color_span(&u->key), k->depth_pa, depth_span(k));
        bool depth = overlaps(u->key.depth_pa, depth_span(&u->key), k->color_pa, color_span(k)) ||
                     overlaps(u->key.depth_pa, depth_span(&u->key), k->depth_pa, depth_span(k));
        if (!color && !depth) continue;
        if (!target_store(u)) return AGAIN;
        if (color) set_color(u, false, u->color_dirty);
        if (depth) set_depth(u, false, u->depth_dirty);
    }
    bool color = !t->color_valid, depth = !t->depth_valid && k->depth_format != NO_DEPTH;
    if ((color || depth) && !target_move(t, false, color, depth)) return AGAIN;
    set_color(t, true, t->color_dirty);
    set_depth(t, true, t->depth_dirty);
    t->used = snx_ctr_vk_serial();
    g.open = (unsigned)(t - g.targets);
    g.opened = true;
    return SNX_CTR_PICA_OK;
}
void snx_ctr_vk_sync(uint32_t pa, uint32_t size, bool write) {
    if (!size || !snx_ctr_vk()) return;
    if (g.made && !store_over(pa, size)) fprintf(stderr, "3ds gpu: a render target could not be stored\n");
    if (write) stale(pa, size);
    gpu_uses(pa, size, write);
}
void snx_ctr_vk_track(void* context, uint32_t pa, SnxCtrTrackLevel level) {
    (void)context;
    if (level == SNX_CTR_TRACK_OWN) {
        /* the page's newer contents: its targets stored, then every GPU write to it waited for */
        if (g.made) (void)store_over(pa, 4096);
        PageState* page = page_state(pa);
        if (page && page->write) (void)snx_ctr_vk_wait(page->write, UINT64_MAX);
    } else { /* the CPU writes it: after the GPU's reads of it, and what the GPU made of it is stale */
        PageState* page = page_state(pa);
        if (page && page->read) (void)snx_ctr_vk_wait(page->read, UINT64_MAX);
        stale(pa, 4096);
    }
    levels(pa, 4096);
}

/* ---- textures ---- */

static uint32_t tile_bytes(unsigned format) {
    static const uint32_t tiles[14] = {256, 192, 128, 128, 128, 128, 128, 64, 64, 64, 32, 32, 32, 64};
    return tiles[format];
}
static VkSampler sampler(uint32_t key) {
    for (unsigned i = 0; i < g.sampler_count; ++i)
        if (g.samplers[i].key == key) return g.samplers[i].sampler;
    if (g.sampler_count == sizeof g.samplers / sizeof *g.samplers) return g.samplers[0].sampler;
    /* ClampToBorder is the fragment shader's: the sampler clamps to the edge (the 2 variants repeat below zero) */
    static const VkSamplerAddressMode wrap[8] = {
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_REPEAT,        VK_SAMPLER_ADDRESS_MODE_REPEAT};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = key & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .minFilter = key >> 1 & 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                              .mipmapMode = key >> 2 & 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                         : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = wrap[key >> 3 & 7],
                              .addressModeV = wrap[key >> 6 & 7],
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .minLod = (float)(key >> 9 & 15),
                              .maxLod = (float)(key >> 13 & 15),
                              .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    VkSampler s;
    if (vkCreateSampler(device(), &si, NULL, &s) != VK_SUCCESS) return g.sampler_count ? g.samplers[0].sampler : 0;
    g.samplers[g.sampler_count].key = key;
    g.samplers[g.sampler_count++].sampler = s;
    return s;
}
/* texture slot t's image for this shape: its own when it has it, else a new one (the old one buried) */
static bool texture_image(Texture* t, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers) {
    Image* m = &t->image;
    if (m->image && m->width == width && m->height == height && m->level_count == levels && m->layers == layers)
        return true;
    bury(m, NULL, NULL);
    return image_new(VK_FORMAT_R8G8B8A8_UNORM, width, height, levels, layers,
                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT, m);
}
/* a slot for a new texture: an invalid one of the same shape, an empty one, any invalid one, or the least used */
static Texture* texture_slot(uint32_t width, uint32_t height, uint32_t levels, uint32_t layers) {
    Texture* any = NULL;
    for (unsigned i = 0; i < g.texture_count; ++i) {
        Texture* t = &g.textures[i];
        if (t->valid) continue;
        if (t->image.width == width && t->image.height == height && t->image.level_count == levels &&
            t->image.layers == layers)
            return t;
        if (!any) any = t;
    }
    if (g.texture_count < TEXTURES) {
        Texture* t = &g.textures[g.texture_count++];
        *t = (Texture){0};
        return t;
    }
    if (any) return any;
    Texture* least = &g.textures[0];
    for (unsigned i = 1; i < TEXTURES; ++i)
        if (g.textures[i].used < least->used) least = &g.textures[i];
    set_texture(least, false);
    return least;
}
/* the view and sampler of texture unit: the texture kept, or decoded from guest memory on the GPU and kept */
static SnxCtrPicaResult texture(const SnxCtrPica* p, unsigned unit, unsigned type, VkDescriptorImageInfo* out) {
    const uint32_t* r = p->regs;
    const uint32_t* cfg = &r[tex_config[unit]];
    bool cube = unit == 0 && type == SNX_CTR_FS_TEX_CUBE;
    uint32_t width = cfg[1] >> 16 & 0x7ff, height = cfg[1] & 0x7ff, format = r[tex_format[unit]] & 15;
    uint32_t max_level = cfg[3] >> 16 & 15, min_level = cfg[3] >> 24 & 15;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out->imageView = cube ? g.dummy_cube.view : g.dummy.view;
    uint32_t key = (cfg[2] >> 1 & 1) | (cfg[2] >> 2 & 1) << 1 | (cfg[2] >> 24 & 1) << 2 | (cfg[2] >> 12 & 7) << 3 |
                   (cfg[2] >> 8 & 7) << 6 | min_level << 9 | max_level << 13;
    out->sampler = sampler(key);
    if (!(r[TEX_MAIN] >> unit & 1) || (unit == 0 && type == SNX_CTR_FS_TEX_DISABLED) || format > 13 || width < 8 ||
        height < 8)
        return SNX_CTR_PICA_OK;
    uint32_t levels = 1;
    while (levels <= max_level && levels < 11 && (width >> levels) >= 8 && (height >> levels) >= 8) ++levels;
    uint32_t bytes = 0;
    for (uint32_t i = 0; i < levels; ++i) bytes += (width >> i) * (height >> i) / 64 * tile_bytes(format);
    uint32_t pa[6] = {(cfg[4] & 0x0fffffff) * 8};
    for (unsigned f = 1; f < 6; ++f)
        pa[f] = cube ? (((cfg[4] & 0x0fffffff) & ~0x3fffffu) | (r[TEX_CUBE + f - 1] & 0x3fffff)) * 8 : 0;
    /* a dirty target over the texture: stored first, so the texture is decoded from what its draws wrote (rendering to
       a texture); the textures made from that memory before went stale when the target was drawn into */
    for (unsigned f = 0; f < (cube ? 6u : 1u); ++f)
        if (g.made && !store_over(pa[f], bytes)) return AGAIN;
    uint64_t serial = snx_ctr_vk_serial();
    for (unsigned i = 0; i < g.texture_count; ++i) {
        Texture* t = &g.textures[i];
        if (t->valid && t->cube == cube && t->format == format && t->width == width && t->height == height &&
            t->levels == levels && !memcmp(t->pa, pa, sizeof pa)) {
            t->used = serial;
            out->imageView = t->image.view;
            return SNX_CTR_PICA_OK;
        }
    }
    SnxCtrVkWindow windows[6];
    uint32_t inside[6];
    for (unsigned f = 0; f < (cube ? 6u : 1u); ++f)
        if (!snx_ctr_vk_window(pa[f], bytes, &windows[f], &inside[f])) return SNX_CTR_PICA_OK; /* outside RAM */
    Texture* t = texture_slot(width, height, levels, cube ? 6 : 1);
    /* the image's layout transition and the decode below cannot be recorded inside a target's pass */
    end_rendering();
    if (!texture_image(t, width, height, levels, cube ? 6 : 1)) return SNX_CTR_PICA_UNSUPPORTED;
    memcpy(t->pa, pa, sizeof pa);
    t->format = format;
    t->width = width;
    t->height = height;
    t->levels = levels;
    t->bytes = bytes;
    t->cube = cube;
    t->used = serial;
    VkCommandBuffer cb = snx_ctr_vk_commands();
    snx_ctr_vk_barrier(cb);
    for (unsigned f = 0; f < (cube ? 6u : 1u); ++f) {
        uint32_t at = inside[f];
        for (uint32_t i = 0; i < levels; ++i) {
            uint32_t w = width >> i, h = height >> i, push[5] = {at, w, h, format, f};
            if (!dispatch(&g.texture, &windows[f], 1, t->image.levels[i], push, sizeof push, w, h)) return AGAIN;
            at += w * h / 64 * tile_bytes(format);
        }
    }
    snx_ctr_vk_barrier(cb);
    for (unsigned f = 0; f < (cube ? 6u : 1u); ++f) gpu_uses(pa[f], bytes, false);
    set_texture(t, true); /* watched from now on */
    out->imageView = t->image.view;
    return SNX_CTR_PICA_OK;
}

/* ---- shaders and pipelines ---- */

/* a shader module of generated words, which it frees */
static VkShaderModule module(uint32_t* words, size_t count, const char** why) {
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = count * 4,
                                   .pCode = words};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device(), &mi, NULL, &m) != VK_SUCCESS) *why = "the device refuses a shader module";
    free(words);
    return m;
}
static bool grow(void** items, size_t* capacity, size_t count, size_t size) {
    if (count < *capacity) return true;
    size_t n = *capacity ? *capacity * 2 : 16;
    void* p = realloc(*items, n * size);
    if (!p) return false;
    *items = p;
    *capacity = n;
    return true;
}
static bool vs_shader(const SnxCtrPica* p, const SnxCtrVsConfig* c, uint32_t* index, const char** why) {
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, c, sizeof *c);
    for (size_t i = 0; i < g.vs_count; ++i)
        if (g.vs[i].hash == h && !memcmp(&g.vs[i].config, c, sizeof *c)) {
            *index = (uint32_t)i;
            *why = g.vs[i].why;
            return g.vs[i].module != VK_NULL_HANDLE;
        }
    if (!grow((void**)&g.vs, &g.vs_capacity, g.vs_count, sizeof *g.vs)) {
        *why = "the host is out of memory";
        return false;
    }
    VsShader* s = &g.vs[g.vs_count];
    memset(s, 0, sizeof *s);
    s->hash = h;
    memcpy(&s->config, c, sizeof *c); /* its padding too: lookups memcmp it */
    uint32_t* words;
    size_t count;
    if (snx_ctr_vs_spirv(&p->vs, c, snx_ctr_vk()->preserve_infnan, &words, &count, &s->why))
        s->module = module(words, count, &s->why);
    if (!s->module) fprintf(stderr, "3ds gpu: a vertex program at 0x%x cannot be drawn: %s\n", c->main_offset, s->why);
    *index = (uint32_t)g.vs_count++;
    *why = s->why;
    return s->module != VK_NULL_HANDLE;
}
static bool fs_shader(const SnxCtrFsConfig* c, uint32_t* index, const char** why) {
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, c, sizeof *c);
    for (size_t i = 0; i < g.fs_count; ++i)
        if (g.fs[i].hash == h && !memcmp(&g.fs[i].config, c, sizeof *c)) {
            *index = (uint32_t)i;
            *why = g.fs[i].why;
            return g.fs[i].module != VK_NULL_HANDLE;
        }
    if (!grow((void**)&g.fs, &g.fs_capacity, g.fs_count, sizeof *g.fs)) {
        *why = "the host is out of memory";
        return false;
    }
    FsShader* s = &g.fs[g.fs_count];
    memset(s, 0, sizeof *s);
    s->hash = h;
    memcpy(&s->config, c, sizeof *c); /* its padding too: lookups memcmp it */
    uint32_t* words;
    size_t count;
    snx_ctr_fs_spirv(c, snx_ctr_vk()->preserve_infnan, &words, &count);
    s->module = module(words, count, &s->why);
    *index = (uint32_t)g.fs_count++;
    *why = s->why;
    return s->module != VK_NULL_HANDLE;
}

static VkFormat attribute_format(unsigned code) {
    static const VkFormat formats[17] = {
        VK_FORMAT_R8_SINT,       VK_FORMAT_R8G8_SINT,       VK_FORMAT_R8G8B8A8_SINT,       VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R8_UINT,       VK_FORMAT_R8G8_UINT,       VK_FORMAT_R8G8B8A8_UINT,       VK_FORMAT_R8G8B8A8_UINT,
        VK_FORMAT_R16_SINT,      VK_FORMAT_R16G16_SINT,     VK_FORMAT_R16G16B16A16_SINT,   VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R32_SFLOAT,    VK_FORMAT_R32G32_SFLOAT,   VK_FORMAT_R32G32B32_SFLOAT,    VK_FORMAT_R32G32B32A32_SFLOAT,
        VK_FORMAT_R32G32B32A32_SFLOAT};
    return formats[code];
}
static VkBlendFactor blend_factor(uint32_t f) {
    static const VkBlendFactor factors[16] = {
        VK_BLEND_FACTOR_ZERO,           VK_BLEND_FACTOR_ONE,
        VK_BLEND_FACTOR_SRC_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR,
        VK_BLEND_FACTOR_DST_COLOR,      VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR,
        VK_BLEND_FACTOR_SRC_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
        VK_BLEND_FACTOR_DST_ALPHA,      VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA,
        VK_BLEND_FACTOR_CONSTANT_COLOR, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR,
        VK_BLEND_FACTOR_CONSTANT_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA,
        VK_BLEND_FACTOR_SRC_ALPHA_SATURATE, VK_BLEND_FACTOR_ONE}; /* an unknown factor: the source's, as raster.c */
    return factors[f & 15];
}
static VkBlendOp blend_op(uint32_t e) {
    static const VkBlendOp ops[8] = {VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT,
                                     VK_BLEND_OP_MIN, VK_BLEND_OP_MAX,      VK_BLEND_OP_ADD,
                                     VK_BLEND_OP_ADD, VK_BLEND_OP_ADD};
    return ops[e & 7];
}
static const VkCompareOp compare_op[8] = {VK_COMPARE_OP_NEVER,         VK_COMPARE_OP_ALWAYS,
                                          VK_COMPARE_OP_EQUAL,         VK_COMPARE_OP_NOT_EQUAL,
                                          VK_COMPARE_OP_LESS,          VK_COMPARE_OP_LESS_OR_EQUAL,
                                          VK_COMPARE_OP_GREATER,       VK_COMPARE_OP_GREATER_OR_EQUAL};
static const VkStencilOp stencil_op[8] = {VK_STENCIL_OP_KEEP,
                                          VK_STENCIL_OP_ZERO,
                                          VK_STENCIL_OP_REPLACE,
                                          VK_STENCIL_OP_INCREMENT_AND_CLAMP,
                                          VK_STENCIL_OP_DECREMENT_AND_CLAMP,
                                          VK_STENCIL_OP_INVERT,
                                          VK_STENCIL_OP_INCREMENT_AND_WRAP,
                                          VK_STENCIL_OP_DECREMENT_AND_WRAP};
static const VkLogicOp logic_op[16] = {
    VK_LOGIC_OP_CLEAR,  VK_LOGIC_OP_AND,         VK_LOGIC_OP_AND_REVERSE, VK_LOGIC_OP_COPY,
    VK_LOGIC_OP_SET,    VK_LOGIC_OP_COPY_INVERTED, VK_LOGIC_OP_NO_OP,     VK_LOGIC_OP_INVERT,
    VK_LOGIC_OP_NAND,   VK_LOGIC_OP_OR,          VK_LOGIC_OP_NOR,         VK_LOGIC_OP_XOR,
    VK_LOGIC_OP_EQUIVALENT, VK_LOGIC_OP_AND_INVERTED, VK_LOGIC_OP_OR_REVERSE, VK_LOGIC_OP_OR_INVERTED};

static VkPipeline make_pipeline(const PipeKey* k) {
    SnxCtrVk* v = snx_ctr_vk();
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = g.vs[k->vs].module,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = g.fs[k->fs].module,
         .pName = "main"}};
    VkVertexInputBindingDescription bindings[13];
    VkVertexInputAttributeDescription attributes[16];
    uint32_t binding_count = 0;
    for (uint32_t b = 0; b < 13; ++b)
        if (k->binding_mask >> b & 1)
            bindings[binding_count++] = (VkVertexInputBindingDescription){b, k->stride[b], VK_VERTEX_INPUT_RATE_VERTEX};
    for (uint32_t i = 0; i < 16; ++i)
        attributes[i] = (VkVertexInputAttributeDescription){i, k->attribute[i].binding,
                                                            attribute_format(k->attribute[i].format),
                                                            k->attribute[i].offset};
    VkPipelineVertexInputStateCreateInfo vertex = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                   .vertexBindingDescriptionCount = binding_count,
                                                   .pVertexBindingDescriptions = bindings,
                                                   .vertexAttributeDescriptionCount = 16,
                                                   .pVertexAttributeDescriptions = attributes};
    static const VkPrimitiveTopology topologies[4] = {
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
        VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = topologies[k->topology & 3]};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    /* the image's y is the PICA's window y, which grows upwards: its counter-clockwise is Vulkan's clockwise.
       KeepClockWise (1) and KeepCounterClockWise (2) keep those; 0 and 3 keep both */
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = k->cull == 1 || k->cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE,
        .frontFace = k->cull == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    /* depth writes need the test on: a write without a test is a test that always passes */
    bool depth_test = k->depth && ((k->depth_color & 1) || k->depth_write);
    VkStencilOpState stencil = {stencil_op[k->stencil_op & 7],
                                stencil_op[k->stencil_op >> 8 & 7],
                                stencil_op[k->stencil_op >> 4 & 7],
                                compare_op[k->stencil >> 4 & 7],
                                k->stencil >> 24 & 0xff,
                                k->depth_write ? k->stencil >> 8 & 0xff : 0,
                                k->stencil >> 16 & 0xff};
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = depth_test,
        .depthWriteEnable = k->depth && k->depth_write && (k->depth_color >> 12 & 1),
        .depthCompareOp = k->depth_color & 1 ? compare_op[k->depth_color >> 4 & 7] : VK_COMPARE_OP_ALWAYS,
        .stencilTestEnable = k->depth && k->stencil_on,
        .front = stencil,
        .back = stencil};
    VkPipelineColorBlendAttachmentState attachment = {
        .blendEnable = k->blend_on,
        .srcColorBlendFactor = blend_factor(k->blend >> 16 & 15),
        .dstColorBlendFactor = blend_factor(k->blend >> 20 & 15),
        .colorBlendOp = blend_op(k->blend & 7),
        .srcAlphaBlendFactor = blend_factor(k->blend >> 24 & 15),
        .dstAlphaBlendFactor = blend_factor(k->blend >> 28 & 15),
        .alphaBlendOp = blend_op(k->blend >> 8 & 7),
        .colorWriteMask = k->color_mask};
    VkPipelineColorBlendStateCreateInfo blend = {.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                 .logicOpEnable = !k->blend_on && v->logic_op &&
                                                                  (k->logic & 15) != 3,
                                                 .logicOp = logic_op[k->logic & 15],
                                                 .attachmentCount = 1,
                                                 .pAttachments = &attachment};
    VkDynamicState dynamic_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 3,
                                                .pDynamicStates = dynamic_states};
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                               .colorAttachmentCount = 1,
                                               .pColorAttachmentFormats = &color_format,
                                               .depthAttachmentFormat =
                                                   k->depth ? g.depth_format : VK_FORMAT_UNDEFINED,
                                               .stencilAttachmentFormat =
                                                   k->depth ? g.depth_format : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                       .pNext = &rendering,
                                       .stageCount = 2,
                                       .pStages = stages,
                                       .pVertexInputState = &vertex,
                                       .pInputAssemblyState = &assembly,
                                       .pViewportState = &viewport,
                                       .pRasterizationState = &raster,
                                       .pMultisampleState = &multisample,
                                       .pDepthStencilState = &depth,
                                       .pColorBlendState = &blend,
                                       .pDynamicState = &dynamic,
                                       .layout = g.layout};
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(v->device, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}
static VkPipeline pipeline(const PipeKey* k) {
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, k, sizeof *k);
    for (size_t i = 0; i < g.pipe_count; ++i)
        if (g.pipes[i].hash == h && !memcmp(&g.pipes[i].key, k, sizeof *k)) return g.pipes[i].pipeline;
    if (!grow((void**)&g.pipes, &g.pipe_capacity, g.pipe_count, sizeof *g.pipes)) return VK_NULL_HANDLE;
    VkPipeline made_pipeline = make_pipeline(k);
    if (!made_pipeline) return VK_NULL_HANDLE;
    g.pipes[g.pipe_count].hash = h;
    g.pipes[g.pipe_count].key = *k;
    g.pipes[g.pipe_count++].pipeline = made_pipeline;
    return made_pipeline;
}

/* ---- draws ---- */

/* the vertex layout of an array draw (VertexLoader): each loader a binding, each attribute's input register a
   location; attributes from the default attributes, and registers no attribute loads, read the defaults' slots */
static void array_layout(const uint32_t* r, PipeKey* k, SnxCtrVsConfig* vs, uint32_t binding_pa[12]) {
    uint32_t lo = r[ATTR_FORMAT_LO], hi = r[ATTR_FORMAT_HI], base = (r[ATTR_BASE] >> 1 & 0x0fffffff) * 16;
    unsigned format[12], elements[12], total = (hi >> 28) + 1, max = r[SNX_PICA_VS + SNX_PICA_SH_INPUT_CONFIG] & 15;
    uint64_t map = (uint64_t)r[SNX_PICA_VS + SNX_PICA_SH_ATTR_MAP_HIGH] << 32 | r[SNX_PICA_VS + SNX_PICA_SH_ATTR_MAP_LOW];
    struct {
        uint8_t binding, format;
        uint16_t offset;
    } source[16];
    for (unsigned n = 0; n < 12; ++n) {
        uint32_t word = n < 8 ? lo : hi, at = (n % 8) * 4;
        format[n] = word >> at & 3;
        elements[n] = (word >> (at + 2) & 3) + 1;
    }
    for (unsigned i = 0; i < 16; ++i) source[i].binding = DEFAULTS, source[i].format = 16, source[i].offset = (uint16_t)(i * 16);
    for (unsigned n = 0; n < 12; ++n) {
        const uint32_t* config = &r[ATTR_LOADERS + n * 3];
        uint64_t components = config[1] | (uint64_t)(config[2] & 0xffff) << 32;
        uint32_t offset = 0, count = config[2] >> 28;
        for (unsigned c = 0; c < count && c < 12; ++c) {
            unsigned attr = components >> (c * 4) & 15;
            if (attr < 12) {
                uint32_t align = format[attr] == FMT_FLOAT ? 4 : format[attr] == FMT_SHORT ? 2 : 1;
                offset = (offset + align - 1) / align * align;
                if (!(hi >> 16 & 1u << attr)) {
                    source[attr].binding = (uint8_t)n;
                    source[attr].format = (uint8_t)(format[attr] * 4 + elements[attr] - 1);
                    source[attr].offset = (uint16_t)offset;
                    k->binding_mask |= (uint16_t)(1u << n);
                    k->stride[n] = config[2] >> 16 & 0xff;
                    binding_pa[n] = base + (config[0] & 0x0fffffff);
                }
                offset += elements[attr] * align;
            } else
                offset = (offset + 3) / 4 * 4 + (attr - 11) * 4;
        }
    }
    static const uint8_t types[4] = {SNX_CTR_VS_SINT, SNX_CTR_VS_UINT, SNX_CTR_VS_SINT, SNX_CTR_VS_FLOAT};
    for (unsigned i = 0; i < 16; ++i) {
        k->attribute[i].binding = DEFAULTS;
        k->attribute[i].format = 16;
        k->attribute[i].offset = ZERO_SLOT * 16;
        vs->input_type[i] = SNX_CTR_VS_FLOAT;
        vs->input_size[i] = 4;
    }
    for (unsigned i = 0; i < total && i <= max; ++i) {
        unsigned reg = map >> (i * 4) & 15;
        k->attribute[reg].binding = source[i].binding;
        k->attribute[reg].format = source[i].format;
        k->attribute[reg].offset = source[i].offset;
        vs->input_type[reg] = source[i].format == 16 ? SNX_CTR_VS_FLOAT : types[source[i].format / 4];
        vs->input_size[reg] = (uint8_t)(source[i].format == 16 ? 4 : source[i].format % 4 + 1);
    }
    k->binding_mask |= 1u << DEFAULTS;
    k->stride[DEFAULTS] = 0;
}
/* immediate mode: binding 0 holds each vertex's attributes as 16 vec4s */
static void immediate_layout(const uint32_t* r, PipeKey* k, SnxCtrVsConfig* vs) {
    unsigned max = r[SNX_PICA_VS + SNX_PICA_SH_INPUT_CONFIG] & 15, total = (r[SNX_PICA_MAX_INPUT_ATTR] & 15) + 1;
    uint64_t map = (uint64_t)r[SNX_PICA_VS + SNX_PICA_SH_ATTR_MAP_HIGH] << 32 | r[SNX_PICA_VS + SNX_PICA_SH_ATTR_MAP_LOW];
    for (unsigned i = 0; i < 16; ++i) {
        k->attribute[i].binding = DEFAULTS;
        k->attribute[i].format = 16;
        k->attribute[i].offset = ZERO_SLOT * 16;
        vs->input_type[i] = SNX_CTR_VS_FLOAT;
        vs->input_size[i] = 4;
    }
    for (unsigned i = 0; i < total && i <= max; ++i) {
        unsigned reg = map >> (i * 4) & 15;
        k->attribute[reg].binding = 0;
        k->attribute[reg].offset = (uint16_t)(i * 16);
    }
    k->binding_mask = 1u | 1u << DEFAULTS;
    k->stride[0] = 256;
}

static SnxCtrPicaResult stop(SnxCtrPica* p, const char* why) {
    p->stop_reason = why;
    return SNX_CTR_PICA_UNSUPPORTED;
}
/* the uploads of a draw: the uniform blocks and LUTs (the last ones again when they hold the same), the default
   attributes; the descriptor sets that point at them */
static SnxCtrPicaResult descriptors(const SnxCtrPica* p, const SnxCtrFsConfig* fs, VkDescriptorSet sets[2],
                                    VkDeviceSize* defaults) {
    SnxCtrVsPica vs_pica;
    SnxCtrVsData vs_data;
    SnxCtrFsUniforms* fs_data = (SnxCtrFsUniforms*)malloc(sizeof *fs_data);
    if (!fs_data) return SNX_CTR_PICA_UNSUPPORTED;
    snx_ctr_vs_uniforms(p, &vs_pica, &vs_data);
    snx_ctr_fs_uniforms(p, fs_data);
    bool ok = true;
    if (!g.vs_uploaded || memcmp(&vs_pica, &g.vs_pica, sizeof vs_pica) || memcmp(&vs_data, &g.vs_data, sizeof vs_data)) {
        VkDeviceSize pica_at, data_at;
        ok = upload(&vs_pica, sizeof vs_pica, &pica_at) && upload(&vs_data, sizeof vs_data, &data_at);
        if (ok) {
            g.vs_pica = vs_pica;
            g.vs_data = vs_data;
            g.vs_at = pica_at;
            g.vs_data_at = data_at;
            g.vs_uploaded = true;
        }
    }
    if (ok && (!g.fs_uploaded || memcmp(fs_data, &g.fs_data, sizeof *fs_data))) {
        VkDeviceSize at;
        ok = upload(fs_data, sizeof *fs_data, &at);
        if (ok) {
            g.fs_data = *fs_data;
            g.fs_at = at;
            g.fs_uploaded = true;
        }
    }
    free(fs_data);
    LutSource* source = (LutSource*)malloc(sizeof *source);
    if (!source) return SNX_CTR_PICA_UNSUPPORTED;
    memcpy(source->lighting, p->lighting_lut, sizeof source->lighting);
    memcpy(source->fog, p->fog_lut, sizeof source->fog);
    memcpy(source->noise, p->proctex_noise, sizeof source->noise);
    memcpy(source->color_map, p->proctex_color_map, sizeof source->color_map);
    memcpy(source->alpha_map, p->proctex_alpha_map, sizeof source->alpha_map);
    memcpy(source->color, p->proctex_color, sizeof source->color);
    memcpy(source->diff, p->proctex_color_diff, sizeof source->diff);
    if (ok && (!g.lut_uploaded || memcmp(source, &g.lut_source, sizeof *source))) {
        VkDeviceSize lf, rg, rgba;
        ok = ring(SNX_CTR_FS_LUT_LF * 8, &lf) && ring(SNX_CTR_FS_LUT_RG * 8, &rg) &&
             ring(SNX_CTR_FS_LUT_RGBA * 16, &rgba);
        if (ok) {
            snx_ctr_fs_luts(p, (float (*)[2])(g.ring_map + lf), (float (*)[2])(g.ring_map + rg),
                            (float (*)[4])(g.ring_map + rgba));
            ok = texel_view(lf, SNX_CTR_FS_LUT_LF * 8, VK_FORMAT_R32G32_SFLOAT, &g.lut_views[0]) &&
                 texel_view(rg, SNX_CTR_FS_LUT_RG * 8, VK_FORMAT_R32G32_SFLOAT, &g.lut_views[1]) &&
                 texel_view(rgba, SNX_CTR_FS_LUT_RGBA * 16, VK_FORMAT_R32G32B32A32_SFLOAT, &g.lut_views[2]);
        }
        if (ok) {
            g.lut_source = *source;
            g.lut_uploaded = true;
        }
    }
    free(source);
    float attributes[17][4];
    memcpy(attributes, p->default_attributes, sizeof p->default_attributes);
    memset(attributes[ZERO_SLOT], 0, sizeof attributes[ZERO_SLOT]);
    if (!ok || !upload(attributes, sizeof attributes, defaults)) return AGAIN;
    sets[0] = snx_ctr_vk_set(g.set0);
    sets[1] = snx_ctr_vk_set(g.set1);
    if (!sets[0] || !sets[1]) return AGAIN;
    VkDescriptorBufferInfo blocks[3] = {{g.ring, g.vs_at, sizeof(SnxCtrVsPica)},
                                        {g.ring, g.vs_data_at, sizeof(SnxCtrVsData)},
                                        {g.ring, g.fs_at, sizeof(SnxCtrFsUniforms)}};
    VkWriteDescriptorSet writes[6];
    for (uint32_t i = 0; i < 6; ++i)
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = sets[0],
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                   : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                           .pBufferInfo = i < 3 ? &blocks[i] : NULL,
                                           .pTexelBufferView = i < 3 ? NULL : &g.lut_views[i - 3]};
    vkUpdateDescriptorSets(device(), 6, writes, 0, NULL);
    VkDescriptorImageInfo images[3];
    VkWriteDescriptorSet image_writes[3];
    for (unsigned unit = 0; unit < 3; ++unit) {
        SnxCtrPicaResult r = texture(p, unit, fs->texture0_type, &images[unit]);
        if (r != SNX_CTR_PICA_OK) return r;
        image_writes[unit] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                    .dstSet = sets[1],
                                                    .dstBinding = unit,
                                                    .descriptorCount = 1,
                                                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                    .pImageInfo = &images[unit]};
    }
    vkUpdateDescriptorSets(device(), 3, image_writes, 0, NULL);
    return SNX_CTR_PICA_OK;
}

/* Reading indices can wait for a recording that wrote their memory, so scan them before preparing the draw: a scan
   inside it would submit the recording half-recorded. Returns the highest index drawn, or 0xffff when they cannot
   be read. */
static uint32_t draw_index_max(uint32_t index_pa, bool wide, uint32_t bytes) {
    uint32_t top = 0;
    unsigned char chunk[4096];
    for (uint32_t at = 0; at < bytes; at += sizeof chunk) {
        uint32_t n = bytes - at < sizeof chunk ? bytes - at : (uint32_t)sizeof chunk;
        if (snx_ctr_phys_read(snx_ctr_vk()->memory, index_pa + at, chunk, n) != SNX_CTR_MEM_OK) {
            top = 0xffff; /* unreadable: every index there may be */
            break;
        }
        for (uint32_t i = 0; i < n; i += wide ? 2u : 1u) {
            uint32_t index = wide ? snx_ld_le16(chunk + i) : chunk[i];
            if (index > top) top = index;
        }
    }
    return top;
}
/* What an array draw reads of memory, watched until its submission is done (a CPU write waits for it): the index
   buffer, and each loader's binding up to the highest vertex drawn (scanned before the draw is recorded) */
static void draw_reads(const PipeKey* k, const uint32_t binding_pa[12], uint32_t index_pa, uint32_t index_bytes,
                       uint32_t count, uint32_t first, uint32_t index_max) {
    uint64_t last = index_bytes ? index_max : (uint64_t)first + count - 1;
    if (index_bytes) gpu_uses(index_pa, index_bytes, false);
    for (unsigned n = 0; n < 12; ++n)
        if (k->binding_mask >> n & 1) {
            uint64_t size = (uint64_t)k->stride[n] * (last + 1);
            gpu_uses(binding_pa[n], size < 64 ? 64 : size, false);
        }
}
/* one draw: array (indexed or not) or the immediate vertices kept */
static SnxCtrPicaResult record(SnxCtrPica* p, bool immediate, bool indexed) {
    const uint32_t* r = p->regs;
    if (r[SNX_PICA_USE_GS] & 3) return stop(p, "geometry shaders are not ported yet");
    if (!made()) return stop(p, "the Vulkan renderer could not be set up");
    /* the target: a colour buffer outside RAM draws nothing */
    TargetKey key = {.color_pa = (r[FB_COLOR_ADDR] & 0x0fffffff) * 8,
                     .depth_pa = (r[FB_DEPTH_ADDR] & 0x0fffffff) * 8,
                     .width = r[FB_SIZE] & 0x7ff,
                     .height = (r[FB_SIZE] >> 12 & 0x3ff) + 1,
                     /* Azahar's IsFlipped: the rows go from y = 0 up when bit 24 is clear (games set it) */
                     .flip = (uint8_t)!(r[FB_SIZE] >> 24 & 1),
                     .depth_format = (uint8_t)(r[FB_DEPTH_FORMAT] & 3)};
    static const uint8_t color_formats[8] = {0, 1, 3, 2, 4, 0xff, 0xff, 0xff}; /* PICA's to transfer.h's */
    key.color_format = color_formats[r[FB_COLOR_FORMAT] >> 16 & 7];
    uint64_t pixels = (uint64_t)key.width * key.height;
    SnxCtrVkWindow probe;
    uint32_t inside;
    if (!key.width || key.color_format == 0xff ||
        !snx_ctr_vk_window(key.color_pa, pixels * color_bytes(key.color_format), &probe, &inside))
        return SNX_CTR_PICA_OK;
    if (key.depth_format == 1 || !key.depth_pa ||
        !snx_ctr_vk_window(key.depth_pa, pixels * depth_bytes(key.depth_format), &probe, &inside)) {
        key.depth_format = NO_DEPTH;
        key.depth_pa = 0;
    }
    float half_x = snx_ctr_f24(r[VIEWPORT_X] & 0xffffff), half_y = snx_ctr_f24(r[VIEWPORT_Y] & 0xffffff);
    if (!(half_x > 0.0f) || !(half_y > 0.0f) || isinf(half_x) || isinf(half_y)) return SNX_CTR_PICA_OK;
    /* the shaders */
    SnxCtrFsConfig fs;
    snx_ctr_fs_config(p, &fs);
    const char* why = snx_ctr_fs_unsupported(&fs);
    if (!why && (fs.texture0_type == SNX_CTR_FS_TEX_SHADOW_2D || fs.texture0_type == SNX_CTR_FS_TEX_SHADOW_CUBE) &&
        (r[TEX_MAIN] & 1))
        why = "shadow textures are not ported yet";
    if (why) return stop(p, why);
    PipeKey k;
    SnxCtrVsConfig vs;
    uint32_t binding_pa[12] = {0};
    memset(&k, 0, sizeof k);
    memset(&vs, 0, sizeof vs);
    if (immediate)
        immediate_layout(r, &k, &vs);
    else
        array_layout(r, &k, &vs, binding_pa);
    if (!g.hashed || g.hashed_pica != p || g.hashed_version != p->vs_version) {
        g.program_hash = snx_ctr_vs_hash(&p->vs);
        g.hashed_pica = p;
        g.hashed_version = p->vs_version;
        g.hashed = true;
    }
    snx_ctr_vs_config(p, &vs);
    vs.program_hash = g.program_hash;
    vs.clip_planes = snx_ctr_vk()->clip_distance;
    if (!vs_shader(p, &vs, &k.vs, &why) || !fs_shader(&fs, &k.fs, &why)) return stop(p, why);
    /* the fixed state */
    uint32_t om = r[OM_DEPTH_COLOR];
    bool depth = key.depth_format != NO_DEPTH;
    k.blend_on = (uint8_t)(r[OM_MODE] >> 8 & 1);
    k.blend = k.blend_on ? r[OM_BLEND] : 0;
    k.logic = k.blend_on ? 0 : r[OM_LOGIC] & 15;
    k.color_mask = (uint8_t)(r[FB_COLOR_WRITE] & 15 ? om >> 8 & 15 : 0);
    k.depth_write = (uint8_t)((r[FB_DEPTH_WRITE] & 3) != 0);
    k.depth = depth;
    k.depth_color = depth ? om & 0x1071 : 0; /* test, function, write */
    k.stencil_on = (uint8_t)(depth && (r[OM_STENCIL] & 1) && key.depth_format == 3);
    k.stencil = k.stencil_on ? r[OM_STENCIL] & 0xffffff71u : 0;
    k.stencil_op = k.stencil_on ? r[OM_STENCIL_OP] & 0x777 : 0;
    k.topology = (uint8_t)(r[SNX_PICA_TOPOLOGY] >> 8 & 3);
    k.cull = (uint8_t)(r[CULL_MODE] & 3);
    VkPipeline pipe = pipeline(&k);
    if (!pipe) return stop(p, "the device refuses a draw's pipeline");
    /* the vertices: an index buffer, the loaders' bindings, or the immediate vertices */
    uint32_t count = immediate ? (uint32_t)g.immediate_count : r[NUM_VERTICES];
    uint32_t index_pa = 0, index_bytes = 0, index_max = 0;
    bool wide = r[INDEX_ARRAY] >> 31;
    VkBuffer buffers[13];
    VkDeviceSize offsets[13];
    if (!count) return SNX_CTR_PICA_OK;
    if (!immediate) {
        uint32_t base = (r[ATTR_BASE] >> 1 & 0x0fffffff) * 16;
        index_pa = base + (r[INDEX_ARRAY] & 0x0fffffff);
        VkDeviceSize at;
        /* an invalid base address draws nothing */
        if (!snx_ctr_vk_ram(index_pa, 1, &at)) return SNX_CTR_PICA_OK;
        if (indexed && ((!wide && !snx_ctr_vk()->index8) || (wide && index_pa % 2)))
            return stop(p, wide ? "16-bit indices at an odd address" : "8-bit indices without VK_EXT_index_type_uint8");
        if (indexed) {
            uint64_t bytes = (uint64_t)count * (wide ? 2u : 1u);
            if (bytes > UINT32_MAX || !snx_ctr_vk_ram(index_pa, (uint32_t)bytes, &at)) return SNX_CTR_PICA_OK;
            index_bytes = (uint32_t)bytes;
            index_max = draw_index_max(index_pa, wide, index_bytes);
        }
        for (unsigned n = 0; n < 12; ++n)
            if (k.binding_mask >> n & 1) {
                const SnxCtrVkRam* ram = snx_ctr_vk_ram(binding_pa[n], 1, &offsets[n]);
                if (!ram) return SNX_CTR_PICA_OK;
                buffers[n] = ram->buffer;
            }
    }
    SnxCtrPicaResult result = target_open(&key);
    if (result != SNX_CTR_PICA_OK) return result;
    VkDescriptorSet sets[2];
    VkDeviceSize defaults;
    result = descriptors(p, &fs, sets, &defaults);
    if (result != SNX_CTR_PICA_OK) return result;
    if (immediate) {
        if (!upload(g.immediate, g.immediate_count * sizeof *g.immediate, &offsets[0])) return AGAIN;
        buffers[0] = g.ring;
    }
    buffers[DEFAULTS] = g.ring;
    offsets[DEFAULTS] = defaults;
    /* a texture decoded may have stored the target, which then needs loading no more: it is still open */
    VkCommandBuffer cb = snx_ctr_vk_commands();
    Target* t = &g.targets[g.open];
    if (!g.rendering) {
        VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                           .imageView = t->color.view,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                           .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingAttachmentInfo ds = color;
        ds.imageView = t->depth.view;
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, {t->key.width, t->key.height}},
                              .layerCount = 1,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color,
                              .pDepthAttachment = t->key.depth_format != NO_DEPTH ? &ds : NULL,
                              .pStencilAttachment = t->key.depth_format != NO_DEPTH ? &ds : NULL};
        vkCmdBeginRendering(cb, &ri);
        g.rendering = true;
    }
    int32_t cx = (int32_t)(r[VIEWPORT_CORNER] << 22) >> 22, cy = (int32_t)(r[VIEWPORT_CORNER] << 6) >> 22;
    VkViewport viewport = {(float)cx, (float)cy, 2.0f * half_x, 2.0f * half_y, 0.0f, 1.0f};
    VkRect2D scissor = {{0, 0}, {key.width, key.height}};
    float constants[4];
    for (unsigned c = 0; c < 4; ++c) constants[c] = (float)(r[OM_BLEND_CONST] >> (c * 8) & 0xff) / 255.0f;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    vkCmdSetBlendConstants(cb, constants);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 2, sets, 0, NULL);
    for (uint32_t b = 0; b < 13; ++b)
        if (k.binding_mask >> b & 1) vkCmdBindVertexBuffers(cb, b, 1, &buffers[b], &offsets[b]);
    if (!immediate) draw_reads(&k, binding_pa, index_pa, index_bytes, count, r[VERTEX_OFFSET], index_max);
    if (indexed && !immediate) {
        VkDeviceSize at;
        const SnxCtrVkRam* ram = snx_ctr_vk_ram(index_pa, 1, &at);
        vkCmdBindIndexBuffer(cb, ram->buffer, at, wide ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT8_EXT);
        vkCmdDrawIndexed(cb, count, 1, 0, 0, 0);
    } else
        vkCmdDraw(cb, count, 1, immediate ? 0 : r[VERTEX_OFFSET], 0);
    /* drawn into: newer than memory, which is the GPU's until the target is stored, and the textures made from that
       memory stale */
    t->used = snx_ctr_vk_serial();
    if (k.color_mask && !t->color_dirty) {
        drop_textures(t->key.color_pa, color_span(&t->key));
        set_color(t, t->color_valid, true);
    }
    if (depth && k.depth_write && ((om >> 12 & 1) || k.stencil_on) && !t->depth_dirty) {
        drop_textures(t->key.depth_pa, depth_span(&t->key));
        set_depth(t, t->depth_valid, true);
    }
    return SNX_CTR_PICA_OK;
}
static SnxCtrPicaResult draw(SnxCtrPica* p, bool immediate, bool indexed) {
    SnxCtrPicaResult r = record(p, immediate, indexed);
    if (r != AGAIN) return r;
    /* out of descriptors or uploads: what is recorded is submitted (no wait), and the draw starts over in the next
       slot */
    snx_ctr_vk_submit();
    r = record(p, immediate, indexed);
    return r == AGAIN ? stop(p, "a draw needs more than one recording holds") : r;
}

SnxCtrPicaResult snx_ctr_vk_draw(SnxCtrPica* p, SnxCtrMemory* memory, bool indexed) {
    (void)memory;
    return draw(p, false, indexed);
}
SnxCtrPicaResult snx_ctr_vk_vertex(SnxCtrPica* p, SnxCtrMemory* memory) {
    (void)memory;
    if (g.immediate_count == g.immediate_capacity) {
        size_t n = g.immediate_capacity ? g.immediate_capacity * 2 : 64;
        float (*v)[16][4] = (float (*)[16][4])realloc(g.immediate, n * sizeof *v);
        if (!v) return stop(p, "the host is out of memory");
        g.immediate = v;
        g.immediate_capacity = n;
    }
    memcpy(g.immediate[g.immediate_count++], p->immediate_vertex, sizeof *g.immediate);
    return SNX_CTR_PICA_OK;
}
SnxCtrPicaResult snx_ctr_vk_vertices_end(SnxCtrPica* p, SnxCtrMemory* memory) {
    (void)memory;
    if (!g.immediate_count) return SNX_CTR_PICA_OK;
    SnxCtrPicaResult r = draw(p, true, false);
    g.immediate_count = 0;
    return r;
}

void snx_ctr_vk_render_begin(unsigned slot) {
    g.slot = slot;
    g.ring_at = 0;
    g.vs_uploaded = g.fs_uploaded = g.lut_uploaded = false; /* the last uploads were the other slot's */
}
void snx_ctr_vk_render_close(void) { end_rendering(); }
void snx_ctr_vk_render_retire(unsigned slot, uint64_t serial) {
    if (!snx_ctr_vk()) return;
    for (size_t i = 0; i < g.view_count[slot]; ++i) vkDestroyBufferView(device(), g.views[slot][i], NULL);
    g.view_count[slot] = 0;
    for (size_t i = 0; i < g.dead_count;)
        if (g.dead[i].serial <= serial) {
            dead_free(&g.dead[i]);
            g.dead[i] = g.dead[--g.dead_count];
        } else
            ++i;
    /* what it read and wrote is done with: the CPU's again, or watched where something is kept of it */
    for (size_t i = 0; i < g.span_count[slot]; ++i) levels(g.spans[slot][i].pa, g.spans[slot][i].size);
    g.span_count[slot] = 0;
}
void snx_ctr_vk_render_free(void) {
    if (!snx_ctr_vk()) {
        memset(&g, 0, sizeof g);
        return;
    }
    VkDevice d = device();
    vkDeviceWaitIdle(d);
    for (unsigned s = 0; s < SNX_CTR_VK_SLOTS; ++s) {
        for (size_t i = 0; i < g.view_count[s]; ++i) vkDestroyBufferView(d, g.views[s][i], NULL);
        free(g.views[s]);
    }
    for (size_t i = 0; i < g.dead_count; ++i) dead_free(&g.dead[i]);
    free(g.dead);
    for (unsigned s = 0; s < SNX_CTR_VK_SLOTS; ++s) free(g.spans[s]);
    free(g.pages);
    for (unsigned i = 0; i < g.texture_count; ++i) image_free(&g.textures[i].image);
    for (unsigned i = 0; i < g.target_count; ++i) target_free(&g.targets[i]);
    for (size_t i = 0; i < g.pipe_count; ++i) vkDestroyPipeline(d, g.pipes[i].pipeline, NULL);
    for (size_t i = 0; i < g.vs_count; ++i)
        if (g.vs[i].module) vkDestroyShaderModule(d, g.vs[i].module, NULL);
    for (size_t i = 0; i < g.fs_count; ++i)
        if (g.fs[i].module) vkDestroyShaderModule(d, g.fs[i].module, NULL);
    for (unsigned i = 0; i < g.sampler_count; ++i) vkDestroySampler(d, g.samplers[i].sampler, NULL);
    image_free(&g.dummy);
    image_free(&g.dummy_cube);
    if (g.ring) vkDestroyBuffer(d, g.ring, NULL);
    if (g.ring_memory) vkFreeMemory(d, g.ring_memory, NULL);
    if (g.layout) vkDestroyPipelineLayout(d, g.layout, NULL);
    if (g.set0) vkDestroyDescriptorSetLayout(d, g.set0, NULL);
    if (g.set1) vkDestroyDescriptorSetLayout(d, g.set1, NULL);
    snx_ctr_vk_compute_free(&g.color);
    snx_ctr_vk_compute_free(&g.depth);
    snx_ctr_vk_compute_free(&g.texture);
    free(g.vs);
    free(g.fs);
    free(g.pipes);
    free(g.immediate);
    memset(&g, 0, sizeof g);
}
