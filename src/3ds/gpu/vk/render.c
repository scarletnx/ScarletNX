/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's draws on the GPU (vk_render.h). The register meanings and their Vulkan equivalents follow Azahar's
   Vulkan renderer (video_core/renderer_vulkan/vk_rasterizer.cpp, vk_pipeline_cache.cpp, pica_to_vk.h,
   GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project, at azahar-emu/azahar
   a4b335e); the targets and textures live in the guest's memory, as gpu/raster.c and gpu/texture.c (the CPU
   reference) read and write them. An image's row r is the rasterizer's y = r (and a texture's row t its coordinate
   t), so the viewport maps the PICA's window coordinates as they are, and the guest's rows (from the top, y =
   height - 1, unless the framebuffer is flipped) are the compute shaders' business. The triangles the CPU makes for a
   draw with a geometry shader turn their quaternions as video_core/rasterizer_accelerated.cpp's AddTriangle does. */
#include "3ds/gpu/vk/vk_render.h"
#include "3ds/gpu/transfer.h"
#include "3ds/gpu/vk/fsgen.h"
#include "3ds/gpu/vk/shaders/color_spv.h"
#include "3ds/gpu/vk/shaders/depth_spv.h"
#include "3ds/gpu/vk/shaders/standin_spv.h"
#include "3ds/gpu/vk/shaders/texture_spv.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vsgen.h"
#include "common/gpu/vk/shader_object.h"
#include "common/gpu/vk/vk_common.h"
#include "common/platform/file.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "snx_settings.h"
#include <SDL3/SDL_cpuinfo.h>
#include <SDL3/SDL_mutex.h>
#include <SDL3/SDL_thread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RING_SIZE (32u << 20) /* the uploads, a part for each submission slot: uniforms, LUTs, default and immediate
                                 vertices */
#define RING_PART (RING_SIZE / SNX_CTR_VK_SLOTS)
/* the triangles a draw with a geometry shader made that one recording takes: a quarter of its uploads, so that the
   draw's uniforms, LUTs and default attributes fit beside them */
#define TRIANGLES_AT_ONCE (RING_PART / 4 / sizeof(SnxCtrVertex[3]))
#define TEXTURES  512 /* textures kept decoded */
#define TARGETS   32  /* render targets kept */
#define SPANS     4096 /* the memory spans a submission reads or writes, re-levelled when it is done */
#define PIPE_JOBS         64   /* the pipelines or shader objects the workers have in hand at once (async_pipelines) */
#define WORKERS           32   /* pipeline_threads's most */

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
/* the open rendering pass: a target's attachments, or a shadow pass with none (its draws write the target's colour
   image as storage) */
enum { RENDERING_TARGET = 1, RENDERING_SHADOW = 2 };
/* texture unit 0's descriptors: a shadow cube's six faces (the other kinds repeat their one image) */
enum { TEX0_DESCRIPTORS = 6 };
/* a draw that ran out of the recording's descriptors, uploads or textures: finished, it is recorded again */
#define AGAIN ((SnxCtrPicaResult)-1)
/* where a draw's vertices come from: the arrays the loaders name, the immediate vertices kept, or the triangles the CPU
   made */
typedef enum { ARRAYS, IMMEDIATE, TRIANGLES } Source;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;       /* every level and layer: sampled (2D or cube) or attached */
    VkImageView levels[11]; /* textures: each level as an array, for the decoder to store into */
    VkImageView words[6];   /* colour images: each layer's level 0 as R32_UINT, a shadow map's words */
    uint32_t width, height, level_count, layers;
} Image;

typedef struct {
    uint32_t color_pa, depth_pa, width, height;
    uint8_t color_format, depth_format, flip, scale; /* color in transfer.h's order; depth 0 D16, 2 D24, 3 D24S8 */
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
    uint8_t pass;  /* the kind of the last pass into its colour image (RENDERING_*), 0 before the first */
    uint64_t used; /* the last submission's serial that drew into it or read it */
} Target;

/* A texture kept decoded while its memory is unchanged (valid); an invalid one's image is reused */
typedef struct {
    uint32_t pa[6], format, width, height, levels, bytes;
    bool cube, valid;
    Image image;
    uint64_t used;
} Texture;
/* Where the GPU's work is on a copy of the RAM (vk.h), how a page's copy stands against the RAM: stale (the CPU wrote
   the RAM since, or nothing was copied yet), the same, or newer (the GPU wrote the copy since). The work is only ever on
   the copy: a stale page is copied in before the GPU reads or writes it, a newer one copied out before the CPU touches
   it. A write copies its stale pages in too, as one need not cover all of a page, and a page goes back whole */
enum { COPY_STALE, COPY_SAME, COPY_NEWER };
/* What the GPU keeps of a RAM page and does with it: the submissions that last read and wrote it, the targets and
   textures kept of it, the dirty targets over it, its copy. Its write-tracking level follows (levels()) */
typedef struct {
    uint64_t read, write;
    uint16_t kept, dirty;
    uint8_t copy;
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
    uint8_t topology, cull, depth, shadow;
    uint16_t binding_mask, pad2;
    uint32_t stride[13];
    struct {
        uint8_t binding, format;
        uint16_t offset;
    } attribute[16];
} PipeKey;

/* A shader as a shader object: the object once made. With async_pipelines a worker makes it, job its PipeJob's index
   plus 1 meanwhile; words holds its SPIR-V while every job is in hand, until one is free */
typedef struct {
    VkShaderEXT object;
    uint32_t job;
    uint32_t* words;
    size_t count;
    uint64_t key; /* its binary's (snx_vk_shader_key) */
    VkShaderStageFlagBits stage;
} ShaderObject;
/* a shader: its module for the pipelines, or with shader objects its object */
typedef struct {
    uint64_t hash;
    SnxCtrVsConfig config;
    VkShaderModule module;
    ShaderObject so;
    const char* why;
} VsShader;
typedef struct {
    uint64_t hash;
    SnxCtrFsConfig config;
    VkShaderModule module;
    ShaderObject so;
    const char* why;
} FsShader;
typedef struct {
    uint64_t hash;
    PipeKey key;
    VkPipeline pipeline;
    uint32_t job; /* while a worker makes it (async_pipelines), its PipeJob's index plus 1; else 0 */
} Pipe;
/* a pipeline a worker makes: its key and the shader modules the draw found, then the pipeline. Or a shader object:
   its stage, its SPIR-V (the job's until it is collected) and its binary's key, then the object */
enum { JOB_FREE, JOB_QUEUED, JOB_MAKING, JOB_DONE };
typedef struct {
    PipeKey key;
    VkShaderModule vs, fs;
    VkPipeline made;
    VkShaderStageFlagBits stage;
    uint32_t* words;
    size_t count;
    uint64_t binary_key;
    VkShaderEXT object;
    uint8_t state; /* under g.job_lock */
} PipeJob;
/* The stand-in fragment shader's push constants (shaders/standin.frag): a fragment config's stages, switches and
   lighting, packed as the shader's comment says, for a draw whose own shader is still being made */
typedef struct {
    uint32_t sources[6], modifiers[6], ops[6], scales[6];
    uint32_t flags, borders;
    uint32_t lights[3], luts[2], lighting;
} StandIn;
_Static_assert(sizeof(StandIn) == 128, "standin.frag's push constants");
/* The vertex stand-in's push constants (shaders/standin.vert), after the fragment one's: a vertex config's entry, its
   output registers' attribute indices (a byte each), its semantic map, its inputs' sizes (four bits each) and whether
   the CPU made the vertex */
typedef struct {
    uint32_t main_offset, num_outputs, output_map[4], attributes[7], total, input_sizes[2], switches;
} StandInVertex;
_Static_assert(sizeof(StandInVertex) == 68, "standin.vert's push constants");
#define PROGRAM_WORDS (4096 + 128) /* standin.vert's program: the code, then the operand descriptors */
_Static_assert(offsetof(SnxCtrFsUniforms, fog_lut_offset) == 40 && offsetof(SnxCtrFsUniforms, fog_color) == 176 &&
                   offsetof(SnxCtrFsUniforms, lighting_lut_offset) == 80 &&
                   offsetof(SnxCtrFsUniforms, lighting_global_ambient) == 224 &&
                   offsetof(SnxCtrFsUniforms, light_src) == 240 && sizeof(SnxCtrFsLight) == 112,
               "standin.frag's fs_data offsets");
_Static_assert(offsetof(SnxCtrFsUniforms, alphatest_ref) == 4 && offsetof(SnxCtrFsUniforms, depth_scale) == 8 &&
                   offsetof(SnxCtrFsUniforms, scissor_x1) == 24 && offsetof(SnxCtrFsUniforms, const_color) == 1136 &&
                   offsetof(SnxCtrFsUniforms, tev_combiner_buffer_color) == 1232,
               "standin.frag's fs_data offsets");
/* the state a draw with shader objects sets: what a pipeline would hold of its key (vs and fs 0, as the shaders are
   bound apart), and the viewport, scissor and blend constants. The next draw of the recording sets only the groups
   that differ */
typedef struct {
    PipeKey key;
    VkViewport viewport;
    VkRect2D scissor;
    float constants[4];
    uint8_t scaled, pad[3]; /* the vertex stand-in's: integer attributes in scaled formats, which it reads as floats */
} ObjectState;

typedef struct {
    uint32_t lighting[24][256], fog[128], noise[128], color_map[128], alpha_map[128], color[256], diff[256];
} LutSource;

static struct {
    bool made, failed;
    SnxCtrVkCompute color, depth, texture;
    VkDescriptorSetLayout set0, set1, set2;
    VkDescriptorSetLayout set_layouts[3]; /* the three, for the shader objects that take them */
    /* the stand-ins' push constants (the fragment one's, and the vertex one's where the device has room for them),
       in the layout and every shader object, for their sets to agree */
    VkPushConstantRange push[2];
    uint32_t push_count;
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
    /* the target the draws go to, its index plus 1 (0: none), and its open rendering pass (0: none) */
    unsigned open;
    uint8_t rendering;
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
    /* the triangles the CPU made for a draw with a geometry shader, waiting for their draw, from triangle_first on */
    SnxCtrVertex (*triangles)[3];
    size_t triangle_count, triangle_capacity, triangle_first;
    SnxCtrVkTraffic traffic;
    /* async_pipelines: the threads that make the new pipelines while the draws that need them are skipped, started
       at the first one and stopped with the renderer, their jobs, and the lock and conditions over those */
    SDL_Thread* workers[WORKERS];
    unsigned worker_count;
    bool workers_tried, jobs_stop;
    SDL_Mutex* job_lock;
    SDL_Condition *job_queued, *job_done;
    PipeJob jobs[PIPE_JOBS];
    uint64_t skipped; /* the draws skipped while their pipeline or shader objects were made */
    /* with shader objects: the stand-in fragment and vertex shaders (VK_NULL_HANDLE when ctr_stand_in leaves them out
       or the device refuses them), and the draws each drew while their own shaders were made */
    VkShaderEXT stand_in, stand_in_vertex;
    uint64_t stood_in, stood_in_vertex;
    /* the device takes the scaled formats of every integer attribute as vertex input: the vertex stand-in can run */
    bool scaled_inputs;
    /* the program the vertex stand-in runs, uploaded into the open recording's part of the ring (program_hash's) */
    bool program_uploaded;
    uint64_t program_uploaded_hash;
    VkDeviceSize program_at;
    int low_refused;  /* a worker's low priority was refused, which the first one refused says (atomic) */
    /* the driver's pipelines kept between runs (snx_ctr_vk_pipeline_cache_open): the cache every pipeline is made
       with, its file, whether a pipeline went into it since it was written and when the last one did (atomic: the
       workers make pipelines too) */
    VkPipelineCache cache;
    char cache_path[1200];
    int cache_dirty;
    uint64_t cache_used_ms;
    /* with shader objects: the driver's binaries of the shaders kept between runs, and their file (binaries_open once
       read); and in the open recording, the shaders bound, the state set (object_state, once object_set) */
    SnxVkShaderBinaries binaries;
    char binaries_path[1200];
    bool binaries_open;
    VkShaderEXT bound[2];
    ObjectState object_state;
    bool object_set;
} g;

static VkDevice device(void) { return snx_ctr_vk()->device; }
static void stand_in_make(void);

/* ---- images ---- */

static void image_free(Image* m) {
    VkDevice d = device();
    if (m->view) vkDestroyImageView(d, m->view, NULL);
    for (unsigned i = 0; i < 11; ++i)
        if (m->levels[i]) vkDestroyImageView(d, m->levels[i], NULL);
    for (unsigned i = 0; i < 6; ++i)
        if (m->words[i]) vkDestroyImageView(d, m->words[i], NULL);
    if (m->image) vkDestroyImage(d, m->image, NULL);
    if (m->memory) vkFreeMemory(d, m->memory, NULL);
    *m = (Image){0};
}
/* an image in the GENERAL layout, which every use of it here takes: color (RGBA8) or the depth format, with layers 1
   or 6 (a cube), storage views of each level for a texture, and each layer as R32_UINT words for a colour one */
static bool image_new(VkFormat format, uint32_t width, uint32_t height, uint32_t levels, uint32_t layers,
                      VkImageUsageFlags usage, Image* m) {
    VkDevice d = device();
    bool depth = format != VK_FORMAT_R8G8B8A8_UNORM;
    VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                                      : VK_IMAGE_ASPECT_COLOR_BIT;
    *m = (Image){0};
    /* the formats a colour image's views take, so that a driver can keep its compression on */
    VkFormat view_formats[2] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32_UINT};
    VkImageFormatListCreateInfo format_list = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = view_formats};
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .pNext = depth ? NULL : &format_list,
                            .flags = (layers == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                                     (depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT),
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
    if (!depth)
        for (uint32_t i = 0; i < layers; ++i) {
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = VK_FORMAT_R32_UINT;
            vi.subresourceRange = (VkImageSubresourceRange){aspect, 0, 1, i, 1};
            if (vkCreateImageView(d, &vi, NULL, &m->words[i]) != VK_SUCCESS) {
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
    if (!snx_ctr_vk_compute(snx_ctr_color_spv, snx_ctr_color_spv_size / 4, 1, 1, 28, &g.color) ||
        !snx_ctr_vk_compute(snx_ctr_depth_spv, snx_ctr_depth_spv_size / 4, 3, 0, 32, &g.depth) ||
        !snx_ctr_vk_compute(snx_ctr_texture_spv, snx_ctr_texture_spv_size / 4, 1, 1, 20, &g.texture))
        return false;
    if (depth_usable(VK_FORMAT_D24_UNORM_S8_UINT))
        g.depth_format = VK_FORMAT_D24_UNORM_S8_UINT;
    else if (depth_usable(VK_FORMAT_D32_SFLOAT_S8_UINT)) {
        g.depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
        g.depth_float = true;
    } else
        return false;
    VkDescriptorSetLayoutBinding set0[7] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL}}; /* the vertex stand-in's */
    VkDescriptorSetLayoutBinding set1[3];
    for (uint32_t i = 0; i < 3; ++i)
        set1[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                 VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    set1[0].descriptorCount = TEX0_DESCRIPTORS;
    VkDescriptorSetLayoutBinding set2 = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 7, .pBindings = set0};
    if (vkCreateDescriptorSetLayout(v->device, &si, NULL, &g.set0) != VK_SUCCESS) return false;
    si.bindingCount = 3;
    si.pBindings = set1;
    if (vkCreateDescriptorSetLayout(v->device, &si, NULL, &g.set1) != VK_SUCCESS) return false;
    si.bindingCount = 1;
    si.pBindings = &set2;
    if (vkCreateDescriptorSetLayout(v->device, &si, NULL, &g.set2) != VK_SUCCESS) return false;
    g.set_layouts[0] = g.set0;
    g.set_layouts[1] = g.set1;
    g.set_layouts[2] = g.set2;
    g.push[0] = (VkPushConstantRange){VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(StandIn)};
    g.push_count = 1;
    if (v->properties.limits.maxPushConstantsSize >= sizeof(StandIn) + sizeof(StandInVertex))
        g.push[g.push_count++] =
            (VkPushConstantRange){VK_SHADER_STAGE_VERTEX_BIT, sizeof(StandIn), sizeof(StandInVertex)};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 3,
                                     .pSetLayouts = g.set_layouts,
                                     .pushConstantRangeCount = g.push_count,
                                     .pPushConstantRanges = g.push};
    if (vkCreatePipelineLayout(v->device, &li, NULL, &g.layout) != VK_SUCCESS) return false;
    if (!buffer_new(RING_SIZE,
                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &g.ring,
                    &g.ring_memory) ||
        vkMapMemory(v->device, g.ring_memory, 0, VK_WHOLE_SIZE, 0, (void**)&g.ring_map) != VK_SUCCESS)
        return false;
    const VkPhysicalDeviceLimits* l = &v->properties.limits;
    g.align = 256;
    if (l->minUniformBufferOffsetAlignment > g.align) g.align = l->minUniformBufferOffsetAlignment;
    if (l->minTexelBufferOffsetAlignment > g.align) g.align = l->minTexelBufferOffsetAlignment;
    if (l->minStorageBufferOffsetAlignment > g.align) g.align = l->minStorageBufferOffsetAlignment;
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
    stand_in_make();
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
/* The 3DS draws a screen's framebuffer into one 240x400 or 240x320 target: the top screen is 400 rows, the bottom
   320. Its 3D rendering is scaled by the setting of that screen, as far as the device's largest image takes. */
uint8_t snx_ctr_vk_target_scale(unsigned width, unsigned height) {
    unsigned scale = 1;
    if (height == 400)
        scale = (unsigned)g_settings.ctr_top_resolution;
    else if (height == 320)
        scale = (unsigned)g_settings.ctr_bottom_resolution;
    if (scale < 1) scale = 1;
    if (scale > 16) scale = 16;
    const SnxCtrVk* v = snx_ctr_vk();
    const uint32_t largest = v ? v->properties.limits.maxImageDimension2D : 0;
    if (largest && width && scale > largest / width) scale = largest / width;
    if (largest && height && scale > largest / height) scale = largest / height;
    return (uint8_t)(scale ? scale : 1);
}
static uint8_t target_scale(const TargetKey* k) { return snx_ctr_vk_target_scale(k->width, k->height); }
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
/* The level write tracking gives each page of [pa, pa + size): the GPU's (OWN) under a dirty target, a write not
   done or a newer copy, watched under something kept, a read not done (a write waits for that read) or a copy the
   same as the RAM, the CPU's otherwise */
static void levels(uint32_t pa, uint64_t size) {
    SnxCtrVk* v = snx_ctr_vk();
    if (!v || !v->memory || !size) return;
    for (uint64_t at = pa & ~4095u; at < (uint64_t)pa + size; at += 4096) {
        PageState* page = page_state((uint32_t)at);
        if (!page) continue;
        SnxCtrTrackLevel l =
            page->dirty || page->write > v->ring.completed || page->copy == COPY_NEWER ? SNX_CTR_TRACK_OWN
            : page->kept || page->read > v->ring.completed || page->copy == COPY_SAME  ? SNX_CTR_TRACK_WATCH
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
/* The pages of [pa, pa + size) whose copy is stale copied in from the RAM, in the open recording's uploads (which run
   before every command it holds), the same from then on. The copy reads the RAM: a CPU write waits for serial. Nothing
   recorded or in flight reads or writes a stale page's copy: it went stale on a CPU write, which waited for them */
static void copy_in(uint32_t pa, uint64_t size, uint64_t serial) {
    VkCommandBuffer uploads = VK_NULL_HANDLE;
    bool shadow = snx_ctr_vk_shadow();
    uint64_t end = (uint64_t)pa + size;
    for (uint64_t at = pa & ~4095u; at < end;) {
        PageState* page = page_state((uint32_t)at);
        if (!page || page->copy != COPY_STALE) {
            at += 4096;
            continue;
        }
        VkDeviceSize offset;
        const SnxCtrVkRam* ram = snx_ctr_vk_ram((uint32_t)at, 4096, &offset);
        uint64_t run = at; /* the stale pages from here on, in this region */
        for (; run < end && run < (uint64_t)ram->base + ram->size; run += 4096) {
            page = page_state((uint32_t)run);
            if (page->copy != COPY_STALE) break;
            page->copy = COPY_SAME;
            page->read = serial;
        }
        if (shadow)
            memcpy(ram->map + (at - ram->base), ram->guest + (at - ram->base), (size_t)(run - at));
        else {
            if (!uploads) uploads = snx_vk_ring_uploads(&snx_ctr_vk()->ring);
            VkBufferCopy c = {offset, offset, run - at};
            vkCmdCopyBuffer(uploads, ram->buffer, ram->copy, 1, &c);
        }
        g.traffic.copied_in += run - at;
        at = run;
    }
}
/* the open recording reads [pa, pa + size), or writes it: its pages wait for its submission (a CPU write for a read, any
   access for a write), and are levelled again when it is done. On a copy of the RAM, its stale pages are copied in
   first, and what it writes is newer than the RAM from then on */
static void gpu_uses(uint32_t pa, uint64_t size, bool write) {
    if (!size || !snx_ctr_vk()) return;
    snx_ctr_vk_commands(); /* the open recording's slot */
    uint64_t serial = snx_ctr_vk_serial();
    bool copies = snx_ctr_vk_copies(), shadow = snx_ctr_vk_shadow();
    if (write)
        g.traffic.written += size;
    else
        g.traffic.read += size;
    if (copies || shadow) copy_in(pa, size, serial);
    for (uint64_t at = pa & ~4095u; at < (uint64_t)pa + size; at += 4096) {
        PageState* page = page_state((uint32_t)at);
        if (!page) continue;
        if (write) {
            page->write = serial;
            if (copies || shadow) page->copy = COPY_NEWER;
        } else
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
    g.rendering = 0;
}
/* the target's colour, and its depth and stencil, moved between the guest's memory and its images (store: to memory,
   which is then the GPU's until the recording is done) */
static bool target_move(Target* t, bool store, bool color, bool depth) {
    const TargetKey* k = &t->key;
    VkCommandBuffer cb = snx_ctr_vk_commands();
    uint32_t width = k->width * k->scale, height = k->height * k->scale;
    VkDeviceSize pixels = (VkDeviceSize)width * height;
    SnxCtrVkWindow w[3];
    uint32_t inside;
    if (store) end_rendering(); /* its barrier and dispatch cannot be recorded inside a target's pass */
    snx_ctr_vk_barrier(cb);
    if (color && snx_ctr_vk_window(k->color_pa, color_span(k), &w[0], &inside)) {
        uint32_t push[7] = {inside, k->width, k->height, k->color_format, k->flip, store, k->scale};
        if (!dispatch(&g.color, w, 1, t->color.view, push, sizeof push, store ? k->width : width,
                      store ? k->height : height))
            return false;
        gpu_uses(k->color_pa, color_span(k), store);
    }
    if (depth && k->depth_format != NO_DEPTH && snx_ctr_vk_window(k->depth_pa, depth_span(k), &w[0], &inside)) {
        VkBufferImageCopy copies[2] = {
            {0, width, height, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {0, 0, 0}, {width, height, 1}},
            {t->stencil_at, width, height, {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1}, {0, 0, 0}, {width, height, 1}}};
        w[1] = (SnxCtrVkWindow){t->scratch, 0, pixels * 4};
        w[2] = (SnxCtrVkWindow){t->scratch, t->stencil_at, (pixels + 3) / 4 * 4};
        uint32_t push[8] = {inside, k->width, k->height, k->depth_format, k->flip, store, g.depth_float, k->scale};
        if (store) {
            vkCmdCopyImageToBuffer(cb, t->depth.image, VK_IMAGE_LAYOUT_GENERAL, t->scratch, 2, copies);
            snx_ctr_vk_barrier(cb);
        }
        if (!dispatch(&g.depth, w, 3, VK_NULL_HANDLE, push, sizeof push, store ? k->width : width,
                      store ? k->height : height))
            return false;
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
        if ((color || depth) && g.open == i + 1) { /* the draws load it again first */
            end_rendering();
            g.open = 0;
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
            if (g.open != i + 1 && (!t || g.targets[i].used < t->used)) t = &g.targets[i];
        if (!target_store(t)) return NULL;
        set_color(t, false, false);
        set_depth(t, false, false);
        /* its images may be in the recordings in flight: they go when those are done */
        bury(&t->color, NULL, NULL);
        bury(&t->depth, &t->scratch, &t->scratch_memory);
    }
    *t = (Target){0};
    if (!target_images(t, k->width * k->scale, k->height * k->scale)) return NULL;
    t->key = *k;
    if (t == &g.targets[g.target_count]) ++g.target_count;
    return t;
}
/* the draw's target open: the one kept for key, loaded from memory unless its images hold what memory does. Other
   targets over the same memory are stored first, and are stale from now on */
static SnxCtrPicaResult target_open(const TargetKey* k) {
    if (g.open && !memcmp(k, &g.targets[g.open - 1].key, sizeof *k)) return SNX_CTR_PICA_OK;
    end_rendering();
    g.open = 0;
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
    g.open = (unsigned)(t - g.targets) + 1;
    return SNX_CTR_PICA_OK;
}
/* A dirty colour target kept over the shown framebuffer: the presenter draws from its image, at the render scale,
   instead of decoding the framebuffer the target has not stored yet. format is transfer.h's colour format, which the
   target must have; a target at the console's scale is not used, because its image holds the draws' unquantised
   colours. Null when no such target is kept. */
bool snx_ctr_vk_target_image(uint32_t pa, unsigned width, unsigned height, unsigned format, VkImageView* view,
                             unsigned* scale, unsigned* flip) {
    if (!g.made) return false;
    for (unsigned i = 0; i < g.target_count; ++i) {
        Target* t = &g.targets[i];
        if (t->key.color_pa == pa && t->key.width == width && t->key.height == height &&
            t->key.color_format == format && t->key.scale > 1 && (t->color_dirty || t->color_valid) && t->color.view) {
            *view = t->color.view;
            *scale = t->key.scale;
            *flip = t->key.flip;
            t->used = snx_ctr_vk_serial();
            return true;
        }
    }
    return false;
}
/* The run of pages around pa (256 at most on each side, in its region) whose copy is newer than the RAM copied back to
   it, and waited for: the same from then on. One CPU access to what the GPU wrote takes what is around it too, a frame
   or a buffer, rather than a submission and a wait for each of its pages */
static void copy_out(uint32_t pa) {
    VkDeviceSize offset;
    const SnxCtrVkRam* ram = snx_ctr_vk_ram(pa, 4096, &offset);
    bool shadow = snx_ctr_vk_shadow();
    uint32_t first = pa, last = pa;
    for (unsigned n = 0; n < 256 && first > ram->base && page_state(first - 4096)->copy == COPY_NEWER; ++n)
        first -= 4096;
    for (unsigned n = 0; n < 256 && (uint64_t)last + 4096 < (uint64_t)ram->base + ram->size &&
                         page_state(last + 4096)->copy == COPY_NEWER;
         ++n)
        last += 4096;
    VkDeviceSize bytes = (VkDeviceSize)last + 4096 - first;
    if (shadow) {
        /* the GPU's writes done, then the guest's own memory gets them */
        (void)snx_ctr_vk_finish();
        memcpy(ram->guest + (first - ram->base), ram->map + (first - ram->base), (size_t)bytes);
        g.traffic.copied_out += bytes;
    } else {
        VkCommandBuffer cb = snx_ctr_vk_commands();
        end_rendering(); /* a copy is not recorded inside a target's pass */
        snx_ctr_vk_barrier(cb);
        VkBufferCopy c = {first - ram->base, first - ram->base, bytes};
        vkCmdCopyBuffer(cb, ram->copy, ram->buffer, 1, &c);
        (void)snx_ctr_vk_finish();
        g.traffic.copied_out += c.size;
    }
    for (uint64_t at = first; at <= last; at += 4096) page_state((uint32_t)at)->copy = COPY_SAME;
    levels(first, bytes);
}
void snx_ctr_vk_sync(uint32_t pa, uint32_t size, bool write) {
    if (!size || !snx_ctr_vk()) return;
    if (g.made && !store_over(pa, size)) fprintf(stderr, "3ds gpu: a render target could not be stored\n");
    if (write) stale(pa, size);
    gpu_uses(pa, size, write);
}
void snx_ctr_vk_track(void* context, uint32_t pa, SnxCtrTrackLevel level) {
    (void)context;
    ++g.traffic.traps;
    if (level == SNX_CTR_TRACK_OWN) {
        /* the page's newer contents: its targets stored, then every GPU write to it waited for, or its copy copied
           back with what the GPU wrote around it */
        if (g.made) (void)store_over(pa, 4096);
        PageState* page = page_state(pa);
        if (page && page->copy == COPY_NEWER)
            copy_out(pa);
        else if (page && page->write)
            (void)snx_ctr_vk_wait(page->write, UINT64_MAX);
    } else { /* the CPU writes it: after the GPU's reads of it, and what the GPU made of it is stale, its copy too */
        PageState* page = page_state(pa);
        if (page && page->read) (void)snx_ctr_vk_wait(page->read, UINT64_MAX);
        if (page && page->copy == COPY_NEWER) /* the GPU wrote it and its level is not raised yet: its bytes land */
            copy_out(pa);
        stale(pa, 4096);
        if (page && page->copy == COPY_SAME) page->copy = COPY_STALE;
    }
    levels(pa, 4096);
}
SnxCtrVkTraffic snx_ctr_vk_traffic(void) { return g.traffic; }

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
    /* the Switch's anisotropy raises a mipmapped texture's linear filtering, a 3D scene's surfaces; nearest filtering
       (2D art) and a texture without levels keep the game's */
    float anisotropy = (key & 3) == 3 && (key >> 13 & 15) ? snx_ctr_vk()->anisotropy : 0;
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
                              .anisotropyEnable = anisotropy > 0,
                              .maxAnisotropy = anisotropy,
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
/* the image and sampler of texture unit: the texture kept, or decoded from guest memory on the GPU and kept */
static SnxCtrPicaResult texture(const SnxCtrPica* p, unsigned unit, unsigned type, const Image** image,
                                VkSampler* sampled) {
    const uint32_t* r = p->regs;
    const uint32_t* cfg = &r[tex_config[unit]];
    bool cube = unit == 0 && (type == SNX_CTR_FS_TEX_CUBE || type == SNX_CTR_FS_TEX_SHADOW_CUBE);
    bool shadow = unit == 0 && (type == SNX_CTR_FS_TEX_SHADOW_2D || type == SNX_CTR_FS_TEX_SHADOW_CUBE);
    uint32_t width = cfg[1] >> 16 & 0x7ff, height = cfg[1] & 0x7ff, format = r[tex_format[unit]] & 15;
    uint32_t max_level = cfg[3] >> 16 & 15, min_level = cfg[3] >> 24 & 15;
    *image = cube ? &g.dummy_cube : &g.dummy;
    uint32_t key = (cfg[2] >> 1 & 1) | (cfg[2] >> 2 & 1) << 1 | (cfg[2] >> 24 & 1) << 2 | (cfg[2] >> 12 & 7) << 3 |
                   (cfg[2] >> 8 & 7) << 6 | min_level << 9 | max_level << 13;
    if (shadow) key &= ~7u; /* a map's words are read texel by texel: integer texels take no linear filter */
    *sampled = sampler(key);
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
            *image = &t->image;
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
    *image = &t->image;
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
static unsigned pipe_workers(void);
/* a shader object's create info of words: a vertex shader's next stage the fragment's, both with the draws' sets */
static VkShaderCreateInfoEXT object_info(VkShaderStageFlagBits stage, const uint32_t* words, size_t count) {
    return (VkShaderCreateInfoEXT){.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT,
                                   .stage = stage,
                                   .nextStage = stage == VK_SHADER_STAGE_VERTEX_BIT ? VK_SHADER_STAGE_FRAGMENT_BIT : 0,
                                   .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
                                   .codeSize = count * 4,
                                   .pCode = words,
                                   .pName = "main",
                                   .setLayoutCount = 3,
                                   .pSetLayouts = g.set_layouts,
                                   .pushConstantRangeCount = g.push_count,
                                   .pPushConstantRanges = g.push};
}
/* a shader object made: when the cache was last used, for its write (snx_ctr_vk_pipeline_cache_save) */
static void compiled_now(void) {
    __atomic_store_n(&g.cache_used_ms, (uint64_t)(snx_seconds() * 1000), __ATOMIC_RELAXED);
}
/* o's words handed to a free job of the workers, which then owns them: whether one was free */
static bool object_queue(ShaderObject* o) {
    SDL_LockMutex(g.job_lock);
    for (unsigned i = 0; i < PIPE_JOBS && !o->job; ++i)
        if (g.jobs[i].state == JOB_FREE) {
            g.jobs[i] = (PipeJob){
                .stage = o->stage, .words = o->words, .count = o->count, .binary_key = o->key, .state = JOB_QUEUED};
            o->job = i + 1;
            o->words = NULL;
            SDL_SignalCondition(g.job_queued);
        }
    SDL_UnlockMutex(g.job_lock);
    return o->job != 0;
}
/* The shader object of generated words, which it frees or keeps: made now from the binary an earlier run kept, which
   costs no compile; with async_pipelines handed to the workers (o->job), or kept until a job is free (o->words), so
   that no compile holds up the draw; else compiled now, its binary kept for the next run. *why when the device refuses
   it */
static void object_new(ShaderObject* o, VkShaderStageFlagBits stage, uint32_t* words, size_t count, const char** why) {
    VkShaderCreateInfoEXT info = object_info(stage, words, count);
    *o = (ShaderObject){.stage = stage, .key = snx_vk_shader_key(&info)};
    size_t size;
    if (!snx_vk_shader_binary(&g.binaries, o->key, &size) && g_settings.async_pipelines && pipe_workers()) {
        o->words = words;
        o->count = count;
        (void)object_queue(o);
        return;
    }
    bool compiled = false;
    if (snx_vk_shader_object(device(), g.binaries_open ? &g.binaries : NULL, o->key, &info, &o->object, &compiled) !=
        VK_SUCCESS)
        *why = "the device refuses a shader object";
    if (compiled) compiled_now();
    free(words);
}
/* Whether o is still to be made: handed to a job now if it waits for one, or collected if a worker made it, its binary
   kept for the next run. *why when the device refused it */
static bool object_pending(ShaderObject* o, const char** why) {
    if (o->words && !object_queue(o)) return true;
    if (!o->job) return false;
    PipeJob* j = &g.jobs[o->job - 1];
    SDL_LockMutex(g.job_lock);
    bool done = j->state == JOB_DONE;
    if (done) {
        o->object = j->object;
        free(j->words);
        *j = (PipeJob){0};
        o->job = 0;
    }
    SDL_UnlockMutex(g.job_lock);
    if (done && o->object && g.binaries_open)
        (void)snx_vk_shader_binaries_keep(&g.binaries, device(), o->key, o->object);
    if (done && !o->object) *why = "the device refuses a shader object";
    return !done;
}
/* whether a shader object is made or on its way */
static bool object_usable(const ShaderObject* o) { return o->object || o->job || o->words; }
static bool vs_shader(const SnxCtrPica* p, const SnxCtrVsConfig* c, uint32_t* index, const char** why) {
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, c, sizeof *c);
    for (size_t i = 0; i < g.vs_count; ++i)
        if (g.vs[i].hash == h && !memcmp(&g.vs[i].config, c, sizeof *c)) {
            *index = (uint32_t)i;
            *why = g.vs[i].why;
            return g.vs[i].module || object_usable(&g.vs[i].so);
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
    if (snx_ctr_vs_spirv(&p->vs, c, snx_ctr_vk()->preserve_infnan, &words, &count, &s->why)) {
        if (snx_ctr_vk()->shader_object)
            object_new(&s->so, VK_SHADER_STAGE_VERTEX_BIT, words, count, &s->why);
        else
            s->module = module(words, count, &s->why);
    }
    bool usable = s->module || object_usable(&s->so);
    if (!usable) fprintf(stderr, "3ds gpu: a vertex program at 0x%x cannot be drawn: %s\n", c->main_offset, s->why);
    *index = (uint32_t)g.vs_count++;
    *why = s->why;
    return usable;
}
static bool fs_shader(const SnxCtrFsConfig* c, uint32_t* index, const char** why) {
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, c, sizeof *c);
    for (size_t i = 0; i < g.fs_count; ++i)
        if (g.fs[i].hash == h && !memcmp(&g.fs[i].config, c, sizeof *c)) {
            *index = (uint32_t)i;
            *why = g.fs[i].why;
            return g.fs[i].module || object_usable(&g.fs[i].so);
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
    if (snx_ctr_vk()->shader_object)
        object_new(&s->so, VK_SHADER_STAGE_FRAGMENT_BIT, words, count, &s->why);
    else
        s->module = module(words, count, &s->why);
    *index = (uint32_t)g.fs_count++;
    *why = s->why;
    return s->module || object_usable(&s->so);
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
/* the same, with the integer formats scaled (read as floats), for the vertex stand-in */
static VkFormat attribute_format_scaled(unsigned code) {
    static const VkFormat formats[12] = {
        VK_FORMAT_R8_SSCALED,       VK_FORMAT_R8G8_SSCALED,         VK_FORMAT_R8G8B8A8_SSCALED,
        VK_FORMAT_R8G8B8A8_SSCALED, VK_FORMAT_R8_USCALED,           VK_FORMAT_R8G8_USCALED,
        VK_FORMAT_R8G8B8A8_USCALED, VK_FORMAT_R8G8B8A8_USCALED,     VK_FORMAT_R16_SSCALED,
        VK_FORMAT_R16G16_SSCALED,   VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED};
    return code < 12 ? formats[code] : attribute_format(code);
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

/* k's pipeline, of the shader modules vs and fs, which a worker passes as the draw found them: g.vs and g.fs move as
   they grow. It reads only what made() set */
static VkPipeline make_pipeline(const PipeKey* k, VkShaderModule vs, VkShaderModule fs) {
    SnxCtrVk* v = snx_ctr_vk();
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs,
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
                                                 .attachmentCount = k->shadow ? 0u : 1u,
                                                 .pAttachments = &attachment};
    VkDynamicState dynamic_states[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                        VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 3,
                                                .pDynamicStates = dynamic_states};
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                               .colorAttachmentCount = k->shadow ? 0u : 1u,
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
    if (vkCreateGraphicsPipelines(v->device, g.cache, 1, &pi, NULL, &pipeline) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (g.cache) {
        __atomic_store_n(&g.cache_used_ms, (uint64_t)(snx_seconds() * 1000), __ATOMIC_RELAXED);
        __atomic_store_n(&g.cache_dirty, 1, __ATOMIC_RELAXED);
    }
    return pipeline;
}
/* A worker: makes the queued pipelines one by one, until the renderer stops them. One past the first two runs at the
   system's low priority (low, snx_vk_pipeline_low_workers) */
static int SDLCALL pipe_worker(void* low) {
    char why[256];
    if (low && snx_thread_priority_low(why, sizeof why) && !__atomic_exchange_n(&g.low_refused, 1, __ATOMIC_RELAXED))
        fprintf(
            stderr,
            "3ds gpu: a pipeline worker asked for %s, the system's low priority: %s (said for the first one refused)\n",
            snx_thread_priority_low_name(), why);
    SDL_LockMutex(g.job_lock);
    while (!g.jobs_stop) {
        PipeJob* j = NULL;
        for (unsigned i = 0; i < PIPE_JOBS && !j; ++i)
            if (g.jobs[i].state == JOB_QUEUED) j = &g.jobs[i];
        if (!j) {
            SDL_WaitCondition(g.job_queued, g.job_lock);
            continue;
        }
        j->state = JOB_MAKING;
        SDL_UnlockMutex(g.job_lock);
        VkPipeline made_pipeline = VK_NULL_HANDLE;
        VkShaderEXT made_object = VK_NULL_HANDLE;
        if (j->words) {
            /* no binary of it was kept: the draw thread makes those, and keeps this one's once it collects it */
            VkShaderCreateInfoEXT info = object_info(j->stage, j->words, j->count);
            bool compiled;
            if (snx_vk_shader_object(device(), NULL, j->binary_key, &info, &made_object, &compiled) == VK_SUCCESS)
                compiled_now();
        } else
            made_pipeline = make_pipeline(&j->key, j->vs, j->fs);
        SDL_LockMutex(g.job_lock);
        j->made = made_pipeline;
        j->object = made_object;
        j->state = JOB_DONE;
        SDL_BroadcastCondition(g.job_done);
    }
    SDL_UnlockMutex(g.job_lock);
    return 0;
}
/* the workers, started at the first pipeline async_pipelines hands them: pipeline_threads of them, or the common count
   (snx_vk_pipeline_workers). How many there are; with none the draws wait for their pipelines, as with
   async_pipelines=0, which the log says once */
static unsigned pipe_workers(void) {
    if (g.workers_tried) return g.worker_count;
    g.workers_tried = true;
    unsigned n = (unsigned)snx_vk_pipeline_workers(g_settings.pipeline_threads, SDL_GetNumLogicalCPUCores());
    if (n > WORKERS) n = WORKERS;
    unsigned low = (unsigned)snx_vk_pipeline_low_workers((int)n);
    if ((g.job_lock = SDL_CreateMutex()) && (g.job_queued = SDL_CreateCondition()) &&
        (g.job_done = SDL_CreateCondition()))
        while (g.worker_count < n) {
            SDL_Thread* t =
                SDL_CreateThread(pipe_worker, "3DS pipelines", (void*)(uintptr_t)(g.worker_count >= n - low));
            if (!t) break;
            g.workers[g.worker_count++] = t;
        }
    if (!g.worker_count)
        fprintf(stderr,
                "3ds gpu: async_pipelines=1, but no pipeline worker could start (%s): the draws wait for "
                "their pipelines, as with async_pipelines=0\n",
                SDL_GetError());
    return g.worker_count;
}
/* The pipeline for k. Without async_pipelines it is made now. With it a worker makes a new one while the draws that
   need it are skipped: *pending, and VK_NULL_HANDLE, until it is done. VK_NULL_HANDLE alone when the device refuses
   it */
static VkPipeline pipeline(const PipeKey* k, bool* pending) {
    *pending = false;
    uint64_t h = snx_fnv1a(SNX_FNV_BASIS, k, sizeof *k);
    for (size_t i = 0; i < g.pipe_count; ++i) {
        Pipe* e = &g.pipes[i];
        if (e->hash != h || memcmp(&e->key, k, sizeof *k)) continue;
        if (e->job) {
            PipeJob* j = &g.jobs[e->job - 1];
            SDL_LockMutex(g.job_lock);
            if (j->state == JOB_DONE) {
                e->pipeline = j->made;
                *j = (PipeJob){0};
                e->job = 0;
            }
            SDL_UnlockMutex(g.job_lock);
            *pending = e->job != 0;
        }
        g.skipped += *pending;
        return e->pipeline;
    }
    if (!grow((void**)&g.pipes, &g.pipe_capacity, g.pipe_count, sizeof *g.pipes)) return VK_NULL_HANDLE;
    Pipe* e = &g.pipes[g.pipe_count];
    *e = (Pipe){.hash = h, .key = *k};
    if (g_settings.async_pipelines && pipe_workers()) {
        /* with every job in hand the draw is skipped all the same, and its pipeline asked for again at the next */
        *pending = true;
        ++g.skipped;
        SDL_LockMutex(g.job_lock);
        for (unsigned i = 0; i < PIPE_JOBS && !e->job; ++i)
            if (g.jobs[i].state == JOB_FREE) {
                g.jobs[i] =
                    (PipeJob){.key = *k, .vs = g.vs[k->vs].module, .fs = g.fs[k->fs].module, .state = JOB_QUEUED};
                e->job = i + 1;
                SDL_SignalCondition(g.job_queued);
            }
        SDL_UnlockMutex(g.job_lock);
        if (e->job) ++g.pipe_count;
        return VK_NULL_HANDLE;
    }
    e->pipeline = make_pipeline(k, g.vs[k->vs].module, g.fs[k->fs].module);
    if (e->pipeline) ++g.pipe_count;
    return e->pipeline;
}
/* async_pipelines: waits until every pipeline asked for is made. Stopping, the ones not yet begun are dropped first */
static void pipes_wait(bool stopping) {
    if (!g.worker_count) return;
    SDL_LockMutex(g.job_lock);
    for (unsigned i = 0; i < PIPE_JOBS; ++i) {
        if (stopping && g.jobs[i].state == JOB_QUEUED) {
            free(g.jobs[i].words);
            g.jobs[i] = (PipeJob){0};
        }
        while (g.jobs[i].state == JOB_QUEUED || g.jobs[i].state == JOB_MAKING)
            SDL_WaitCondition(g.job_done, g.job_lock);
    }
    SDL_UnlockMutex(g.job_lock);
}
uint64_t snx_ctr_vk_pipelines_wait(void) {
    pipes_wait(false);
    return g.skipped;
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
typedef char snx_ctr_vertex_is_six_vec4s[sizeof(SnxCtrVertex) == 6 * 16 ? 1 : -1];
/* triangles the CPU made: binding 0 holds each vertex as pica.h's SnxCtrVertex, locations 0-5 its six vec4s, which
   the vertex shader passes on */
static void triangle_layout(PipeKey* k, SnxCtrVsConfig* vs) {
    for (unsigned i = 0; i < 16; ++i) {
        k->attribute[i].binding = i < 6 ? 0 : DEFAULTS;
        k->attribute[i].format = 16;
        k->attribute[i].offset = (uint16_t)(i < 6 ? i * 16 : ZERO_SLOT * 16);
    }
    k->binding_mask = 1u | 1u << DEFAULTS;
    k->stride[0] = sizeof(SnxCtrVertex);
    vs->passthrough = 1;
}
/* the triangles made that the next draw takes */
static size_t triangles_next(void) {
    size_t left = g.triangle_count - g.triangle_first;
    return left < TRIANGLES_AT_ONCE ? left : TRIANGLES_AT_ONCE;
}

static SnxCtrPicaResult stop(SnxCtrPica* p, const char* why) {
    p->stop_reason = why;
    return SNX_CTR_PICA_UNSUPPORTED;
}
/* the uploads of a draw: the uniform blocks and LUTs (the last ones again when they hold the same), the default
   attributes; the descriptor sets that point at them */
static SnxCtrPicaResult descriptors(const SnxCtrPica* p, const SnxCtrFsConfig* fs, unsigned scale,
                                    VkDescriptorSet sets[2], VkDeviceSize* defaults) {
    SnxCtrVsPica vs_pica;
    SnxCtrVsData vs_data;
    SnxCtrFsUniforms* fs_data = (SnxCtrFsUniforms*)malloc(sizeof *fs_data);
    if (!fs_data) return SNX_CTR_PICA_UNSUPPORTED;
    snx_ctr_vs_uniforms(p, &vs_pica, &vs_data);
    snx_ctr_fs_uniforms(p, fs_data);
    /* FragCoord is the scaled image's: the PICA's scissor and framebuffer scale move with it */
    fs_data->framebuffer_scale = (int32_t)scale;
    fs_data->scissor_x1 *= (int32_t)scale;
    fs_data->scissor_y1 *= (int32_t)scale;
    fs_data->scissor_x2 *= (int32_t)scale;
    fs_data->scissor_y2 *= (int32_t)scale;
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
    /* tex0's six (a shadow cube's faces as words, a shadow map's words six times, else its image six times), then
       tex1 and tex2 */
    VkDescriptorImageInfo images[TEX0_DESCRIPTORS + 2];
    VkWriteDescriptorSet image_writes[3];
    bool shadow = fs->texture0_type == SNX_CTR_FS_TEX_SHADOW_2D || fs->texture0_type == SNX_CTR_FS_TEX_SHADOW_CUBE;
    for (unsigned unit = 0; unit < 3; ++unit) {
        const Image* image;
        VkSampler sampled;
        SnxCtrPicaResult r = texture(p, unit, fs->texture0_type, &image, &sampled);
        if (r != SNX_CTR_PICA_OK) return r;
        uint32_t first = unit ? TEX0_DESCRIPTORS + unit - 1 : 0, count = unit ? 1 : TEX0_DESCRIPTORS;
        for (uint32_t f = 0; f < count; ++f) {
            VkImageView view = !unit && shadow ? image->words[f < image->layers ? f : 0] : image->view;
            images[first + f] = (VkDescriptorImageInfo){sampled, view, VK_IMAGE_LAYOUT_GENERAL};
        }
        image_writes[unit] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                    .dstSet = sets[1],
                                                    .dstBinding = unit,
                                                    .descriptorCount = count,
                                                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                    .pImageInfo = &images[first]};
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
/* Whether the stand-in draws a fragment config as its own shader would: no gas, procedural texture or shadow
   rendering, and on unit 0 a 2D or projection texture or none */
static bool stand_in_draws(const SnxCtrFsConfig* c) {
    return !c->proctex.enable && c->fog_mode != 7 && !c->shadow_rendering &&
           (c->texture0_type == SNX_CTR_FS_TEX_2D || c->texture0_type == SNX_CTR_FS_TEX_PROJECTION ||
            c->texture0_type == SNX_CTR_FS_TEX_DISABLED);
}
/* c as the stand-in's push constants */
static StandIn stand_in_of(const SnxCtrFsConfig* c) {
    StandIn s = {0};
    memcpy(s.sources, c->tev_sources, sizeof s.sources);
    memcpy(s.modifiers, c->tev_modifiers, sizeof s.modifiers);
    memcpy(s.ops, c->tev_ops, sizeof s.ops);
    memcpy(s.scales, c->tev_scales, sizeof s.scales);
    s.flags = (uint32_t)(c->alpha_test_func & 7) | (uint32_t)(c->texture0_type & 7) << 3 |
              (uint32_t)(c->texture2_use_coord1 & 1) << 6 | (uint32_t)c->combiner_buffer_input << 8 |
              (uint32_t)(c->depthmap_enable & 1) << 16 | (uint32_t)(c->scissor_mode & 3) << 17 |
              (uint32_t)(c->fog_mode == 5) << 19 | (uint32_t)(c->fog_flip & 1) << 20;
    for (unsigned i = 0; i < 3; ++i)
        s.borders |= (uint32_t)(c->border_s[i] & 1) << i | (uint32_t)(c->border_t[i] & 1) << (3 + i);
    if (!c->lighting.enable) return s;
    for (unsigned i = 0; i < 8; ++i) {
        uint32_t bits = (uint32_t)(c->lighting.lights[i].num & 7) |
                        (uint32_t)(c->lighting.lights[i].directional & 1) << 3 |
                        (uint32_t)(c->lighting.lights[i].two_sided_diffuse & 1) << 4 |
                        (uint32_t)(c->lighting.lights[i].dist_atten & 1) << 5 |
                        (uint32_t)(c->lighting.lights[i].spot_atten & 1) << 6 |
                        (uint32_t)(c->lighting.lights[i].geometric_factor_0 & 1) << 7 |
                        (uint32_t)(c->lighting.lights[i].geometric_factor_1 & 1) << 8 |
                        (uint32_t)(c->lighting.lights[i].shadow & 1) << 9;
        s.lights[i / 3] |= bits << (i % 3 * 10);
    }
    for (unsigned i = 0; i < 7; ++i) {
        uint32_t bits = (uint32_t)(c->lighting.lut[i].enable & 1) | (uint32_t)(c->lighting.lut[i].abs_input & 1) << 1 |
                        (uint32_t)(c->lighting.lut[i].type & 7) << 2 | (uint32_t)(c->lighting.lut[i].scale & 7) << 5;
        s.luts[i / 4] |= bits << (i % 4 * 8);
    }
    s.lighting = 1u | (uint32_t)(c->lighting.src_num & 15) << 1 | (uint32_t)(c->lighting.bump_mode & 3) << 5 |
                 (uint32_t)(c->lighting.bump_selector & 3) << 7 | (uint32_t)(c->lighting.bump_renorm & 1) << 9 |
                 (uint32_t)(c->lighting.clamp_highlights & 1) << 10 | (uint32_t)(c->lighting.config & 15) << 11 |
                 (uint32_t)(c->lighting.enable_primary_alpha & 1) << 15 |
                 (uint32_t)(c->lighting.enable_secondary_alpha & 1) << 16 |
                 (uint32_t)(c->lighting.enable_shadow & 1) << 17 | (uint32_t)(c->lighting.shadow_primary & 1) << 18 |
                 (uint32_t)(c->lighting.shadow_secondary & 1) << 19 | (uint32_t)(c->lighting.shadow_invert & 1) << 20 |
                 (uint32_t)(c->lighting.shadow_alpha & 1) << 21 | (uint32_t)(c->lighting.shadow_selector & 3) << 22;
    return s;
}
/* The stand-in made, once, where ctr_stand_in asks for it: with async_pipelines=1 (a draw waits for its shaders
   otherwise), or for every draw it draws with ctr_stand_in=2. A device that refuses it skips the draws instead */
/* a stand-in shader object of glslang's words, SignedZeroInfNanPreserve declared where the device keeps them as the
   generated shaders declare it. VK_NULL_HANDLE when the device refuses it */
static VkShaderEXT stand_in_object(VkShaderStageFlagBits stage, const uint32_t* words, size_t count) {
    const SnxCtrVk* v = snx_ctr_vk();
    uint32_t* declared = NULL;
    if (v->preserve_infnan) {
        declared = snx_vk_spirv_preserve_infnan(words, count, &count);
        if (!declared) return VK_NULL_HANDLE;
        words = declared;
    }
    VkShaderCreateInfoEXT info = object_info(stage, words, count);
    VkShaderEXT object = VK_NULL_HANDLE;
    bool compiled;
    if (snx_vk_shader_object(device(), g.binaries_open ? &g.binaries : NULL, snx_vk_shader_key(&info), &info, &object,
                             &compiled) != VK_SUCCESS)
        object = VK_NULL_HANDLE;
    free(declared);
    return object;
}
/* whether the device takes the scaled format of each integer attribute format as vertex input */
static bool scaled_inputs(void) {
    static const VkFormat formats[9] = {
        VK_FORMAT_R8_SSCALED,  VK_FORMAT_R8G8_SSCALED,   VK_FORMAT_R8G8B8A8_SSCALED,
        VK_FORMAT_R8_USCALED,  VK_FORMAT_R8G8_USCALED,   VK_FORMAT_R8G8B8A8_USCALED,
        VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED};
    for (unsigned i = 0; i < 9; ++i) {
        VkFormatProperties p;
        vkGetPhysicalDeviceFormatProperties(snx_ctr_vk()->phys, formats[i], &p);
        if (!(p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)) return false;
    }
    return true;
}
static void stand_in_make(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (!v->shader_object || g.stand_in ||
        !(g_settings.ctr_stand_in == 2 || (g_settings.ctr_stand_in == 1 && g_settings.async_pipelines)))
        return;
    g.stand_in = stand_in_object(VK_SHADER_STAGE_FRAGMENT_BIT, snx_ctr_standin_frag_spv,
                                 (size_t)snx_ctr_standin_frag_spv_size / 4);
    if (!g.stand_in) fprintf(stderr, "3ds gpu: the device refuses the fragment stand-in: draws wait for their own\n");
    /* the vertex stand-in: its config in push constants after the fragment one's, its inputs as floats */
    g.scaled_inputs = scaled_inputs();
    if (g.push_count < 2 || !g.scaled_inputs) {
        fprintf(stderr, "3ds gpu: no vertex stand-in: the device has %s\n",
                g.push_count < 2 ? "too few push constants" : "no scaled vertex formats");
        return;
    }
    g.stand_in_vertex = v->clip_distance ? stand_in_object(VK_SHADER_STAGE_VERTEX_BIT, snx_ctr_standin_vert_clip_spv,
                                                           (size_t)snx_ctr_standin_vert_clip_spv_size / 4)
                                         : stand_in_object(VK_SHADER_STAGE_VERTEX_BIT, snx_ctr_standin_vert_spv,
                                                           (size_t)snx_ctr_standin_vert_spv_size / 4);
    if (!g.stand_in_vertex)
        fprintf(stderr, "3ds gpu: the device refuses the vertex stand-in: draws wait for their own\n");
}
/* vs as the vertex stand-in's push constants */
static StandInVertex stand_in_vertex_of(const SnxCtrVsConfig* vs) {
    StandInVertex s = {0};
    s.main_offset = vs->main_offset;
    s.num_outputs = vs->num_outputs;
    for (unsigned r = 0; r < 16; ++r) s.output_map[r / 4] |= (uint32_t)vs->output_map[r] << (r % 4 * 8);
    memcpy(s.attributes, vs->vs_output_attributes, sizeof s.attributes);
    s.total = vs->vs_output_total;
    for (unsigned r = 0; r < 16; ++r) s.input_sizes[r / 8] |= (uint32_t)(vs->input_size[r] & 15) << (r % 8 * 4);
    s.switches = vs->passthrough;
    return s;
}
/* the program the vertex stand-in runs, in the ring, and set 0's binding 6 pointing at it: uploaded again only when
   another program is drawn in the recording */
static bool stand_in_program(const SnxCtrPica* p, uint64_t hash, VkDescriptorSet set) {
    if (!g.program_uploaded || g.program_uploaded_hash != hash) {
        VkDeviceSize at;
        if (!ring(PROGRAM_WORDS * 4, &at)) return false;
        memcpy(g.ring_map + at, p->vs.code, 4096 * 4);
        memcpy(g.ring_map + at + 4096 * 4, p->vs.swizzle, 128 * 4);
        g.program_at = at;
        g.program_uploaded_hash = hash;
        g.program_uploaded = true;
    }
    VkDescriptorBufferInfo program = {g.ring, g.program_at, PROGRAM_WORDS * 4};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 6,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &program};
    vkUpdateDescriptorSets(device(), 1, &write, 0, NULL);
    return true;
}
/* A draw with shader objects: vs and fs bound, and the state a pipeline of s's key would hold set, with the viewport,
   scissor and blend constants. In the open recording only what differs from the last draw's is set: the vertex input,
   the rasterizer's, the depth and stencil's, the colour's, the viewport and scissor, and the constants, each a group */
static void object_bind(VkCommandBuffer cb, VkShaderEXT vs, VkShaderEXT fs, const ObjectState* s) {
    const SnxCtrVk* v = snx_ctr_vk();
    const PipeKey *k = &s->key, *was = g.object_set ? &g.object_state.key : NULL;
    if (!g.object_set) snx_vk_shader_object_defaults(cb);
    if (g.bound[0] != vs || g.bound[1] != fs) {
        static const VkShaderStageFlagBits stages[2] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
        VkShaderEXT shaders[2] = {vs, fs};
        vkCmdBindShadersEXT(cb, 2, stages, shaders);
        g.bound[0] = vs;
        g.bound[1] = fs;
    }
    if (!was || g.object_state.scaled != s->scaled || was->binding_mask != k->binding_mask ||
        memcmp(was->stride, k->stride, sizeof k->stride) || memcmp(was->attribute, k->attribute, sizeof k->attribute)) {
        VkVertexInputBindingDescription2EXT bindings[13];
        VkVertexInputAttributeDescription2EXT attributes[16];
        uint32_t binding_count = 0;
        for (uint32_t b = 0; b < 13; ++b)
            if (k->binding_mask >> b & 1)
                bindings[binding_count++] = (VkVertexInputBindingDescription2EXT){
                    .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
                    .binding = b,
                    .stride = k->stride[b],
                    .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
                    .divisor = 1};
        for (uint32_t i = 0; i < 16; ++i)
            attributes[i] = (VkVertexInputAttributeDescription2EXT){
                .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
                .location = i,
                .binding = k->attribute[i].binding,
                .format = s->scaled ? attribute_format_scaled(k->attribute[i].format)
                                    : attribute_format(k->attribute[i].format),
                .offset = k->attribute[i].offset};
        vkCmdSetVertexInputEXT(cb, binding_count, bindings, 16, attributes);
    }
    if (!was || was->topology != k->topology || was->cull != k->cull) {
        static const VkPrimitiveTopology topologies[4] = {
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
            VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        vkCmdSetPrimitiveTopology(cb, topologies[k->topology & 3]);
        /* as make_pipeline: the PICA's counter-clockwise is Vulkan's clockwise */
        vkCmdSetCullMode(cb, k->cull == 1 || k->cull == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
        vkCmdSetFrontFace(cb, k->cull == 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    }
    if (!was || was->depth != k->depth || was->depth_color != k->depth_color || was->depth_write != k->depth_write ||
        was->stencil_on != k->stencil_on || was->stencil != k->stencil || was->stencil_op != k->stencil_op) {
        /* as make_pipeline */
        vkCmdSetDepthTestEnable(cb, k->depth && ((k->depth_color & 1) || k->depth_write));
        vkCmdSetDepthWriteEnable(cb, k->depth && k->depth_write && (k->depth_color >> 12 & 1));
        vkCmdSetDepthCompareOp(cb, k->depth_color & 1 ? compare_op[k->depth_color >> 4 & 7] : VK_COMPARE_OP_ALWAYS);
        vkCmdSetStencilTestEnable(cb, k->depth && k->stencil_on);
        vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_op[k->stencil_op & 7],
                          stencil_op[k->stencil_op >> 8 & 7], stencil_op[k->stencil_op >> 4 & 7],
                          compare_op[k->stencil >> 4 & 7]);
        vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, k->stencil >> 24 & 0xff);
        vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_AND_BACK, k->depth_write ? k->stencil >> 8 & 0xff : 0);
        vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, k->stencil >> 16 & 0xff);
    }
    if (!was || was->blend_on != k->blend_on || was->blend != k->blend || was->logic != k->logic ||
        was->color_mask != k->color_mask) {
        VkBool32 blend = k->blend_on;
        VkColorBlendEquationEXT equation = {
            blend_factor(k->blend >> 16 & 15), blend_factor(k->blend >> 20 & 15), blend_op(k->blend & 7),
            blend_factor(k->blend >> 24 & 15), blend_factor(k->blend >> 28 & 15), blend_op(k->blend >> 8 & 7)};
        VkColorComponentFlags mask = k->color_mask;
        vkCmdSetColorBlendEnableEXT(cb, 0, 1, &blend);
        vkCmdSetColorBlendEquationEXT(cb, 0, 1, &equation);
        vkCmdSetColorWriteMaskEXT(cb, 0, 1, &mask);
        /* the logicOp feature on: its enable is the draw's to set, and the operation with it */
        if (v->logic_op) {
            bool logic = !k->blend_on && (k->logic & 15) != 3;
            vkCmdSetLogicOpEnableEXT(cb, logic);
            if (logic) vkCmdSetLogicOpEXT(cb, logic_op[k->logic & 15]);
        }
    }
    if (!g.object_set || memcmp(&g.object_state.viewport, &s->viewport, sizeof s->viewport) ||
        memcmp(&g.object_state.scissor, &s->scissor, sizeof s->scissor)) {
        vkCmdSetViewportWithCount(cb, 1, &s->viewport);
        vkCmdSetScissorWithCount(cb, 1, &s->scissor);
    }
    if (!g.object_set || memcmp(g.object_state.constants, s->constants, sizeof s->constants))
        vkCmdSetBlendConstants(cb, s->constants);
    g.object_state = *s;
    g.object_set = true;
}
/* one draw: array (indexed or not), the immediate vertices kept or the triangles the CPU made */
static SnxCtrPicaResult record(SnxCtrPica* p, Source source, bool indexed) {
    const uint32_t* r = p->regs;
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
    key.scale = target_scale(&key);
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
    const char* why = NULL;
    if (fs.shadow_rendering) {
        /* a shadow pass (Azahar's IsShadowRendering): the colour buffer's words only, no depth buffer, and no render
           scale, since a store averages a scaled image's blocks, which would mix the words */
        if (!snx_ctr_vk()->fragment_stores) return stop(p, "shadow rendering needs fragmentStoresAndAtomics");
        if (key.color_format != SNX_CTR_PIX_RGBA8) return stop(p, "shadow rendering into a colour buffer not RGBA8");
        key.depth_format = NO_DEPTH;
        key.depth_pa = 0;
        key.scale = 1;
    }
    PipeKey k;
    SnxCtrVsConfig vs;
    uint32_t binding_pa[12] = {0};
    memset(&k, 0, sizeof k);
    memset(&vs, 0, sizeof vs);
    if (source == TRIANGLES)
        triangle_layout(&k, &vs);
    else if (source == IMMEDIATE)
        immediate_layout(r, &k, &vs);
    else
        array_layout(r, &k, &vs, binding_pa);
    if (source != TRIANGLES) {
        if (!g.hashed || g.hashed_pica != p || g.hashed_version != p->vs_version) {
            g.program_hash = snx_ctr_vs_hash(&p->vs);
            g.hashed_pica = p;
            g.hashed_version = p->vs_version;
            g.hashed = true;
        }
        snx_ctr_vs_config(p, &vs);
        vs.program_hash = g.program_hash;
    }
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
    k.topology = (uint8_t)(source == TRIANGLES ? 0 : r[SNX_PICA_TOPOLOGY] >> 8 & 3); /* the triangles made: a list */
    k.cull = (uint8_t)(r[CULL_MODE] & 3);
    k.shadow = fs.shadow_rendering;
    bool objects = snx_ctr_vk()->shader_object, stand_in = false, stand_in_vertex = false;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (objects) {
        /* async_pipelines: drawn once the workers have made both shaders */
        VsShader* vsh = &g.vs[k.vs];
        FsShader* fsh = &g.fs[k.fs];
        bool vs_pending = object_pending(&vsh->so, &vsh->why), fs_pending = object_pending(&fsh->so, &fsh->why);
        /* a shader still being made: a stand-in draws in its place, where it draws it as it would (ctr_stand_in=2: in
           place of every shader it draws) */
        bool every = g_settings.ctr_stand_in == 2;
        stand_in_vertex = g.stand_in_vertex && (vs_pending || every);
        stand_in = g.stand_in && (fs_pending || every) && stand_in_draws(&fs);
        if ((vs_pending && !stand_in_vertex) || (fs_pending && !stand_in)) {
            ++g.skipped;
            return SNX_CTR_PICA_OK;
        }
        if ((!stand_in_vertex && !vsh->so.object) || (!stand_in && !fsh->so.object))
            return stop(p, "the device refuses a draw's shader object");
    } else {
        bool pending;
        pipe = pipeline(&k, &pending);
        if (pending) return SNX_CTR_PICA_OK; /* async_pipelines: drawn once a worker has made its pipeline */
        if (!pipe) return stop(p, "the device refuses a draw's pipeline");
    }
    /* the vertices: an index buffer, the loaders' bindings, the immediate vertices or the triangles made */
    uint32_t count = source == ARRAYS      ? r[NUM_VERTICES]
                     : source == IMMEDIATE ? (uint32_t)g.immediate_count
                                           : (uint32_t)triangles_next() * 3;
    uint32_t index_pa = 0, index_bytes = 0, index_max = 0;
    bool wide = r[INDEX_ARRAY] >> 31;
    VkBuffer buffers[13];
    VkDeviceSize offsets[13];
    if (!count) return SNX_CTR_PICA_OK;
    if (source == ARRAYS) {
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
                buffers[n] = snx_ctr_vk_work(ram);
            }
    }
    SnxCtrPicaResult result = target_open(&key);
    if (result != SNX_CTR_PICA_OK) return result;
    VkDescriptorSet sets[3];
    VkDeviceSize defaults;
    result = descriptors(p, &fs, key.scale, sets, &defaults);
    if (result != SNX_CTR_PICA_OK) return result;
    if (stand_in_vertex && !stand_in_program(p, source == TRIANGLES ? 0 : g.program_hash, sets[0])) return AGAIN;
    if (k.shadow) {
        sets[2] = snx_ctr_vk_set(g.set2);
        if (!sets[2]) return AGAIN;
        VkDescriptorImageInfo words = {VK_NULL_HANDLE, g.targets[g.open - 1].color.words[0], VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                      .dstSet = sets[2],
                                      .dstBinding = 0,
                                      .descriptorCount = 1,
                                      .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                      .pImageInfo = &words};
        vkUpdateDescriptorSets(device(), 1, &write, 0, NULL);
    }
    if (source == IMMEDIATE) {
        if (!upload(g.immediate, g.immediate_count * sizeof *g.immediate, &offsets[0])) return AGAIN;
        buffers[0] = g.ring;
    } else if (source == TRIANGLES) {
        if (!upload(g.triangles + g.triangle_first, triangles_next() * sizeof *g.triangles, &offsets[0])) return AGAIN;
        buffers[0] = g.ring;
    }
    buffers[DEFAULTS] = g.ring;
    offsets[DEFAULTS] = defaults;
    /* a texture decoded may have stored the target, which then needs loading no more: it is still open */
    VkCommandBuffer cb = snx_ctr_vk_commands();
    Target* t = &g.targets[g.open - 1];
    uint8_t kind = k.shadow ? RENDERING_SHADOW : RENDERING_TARGET;
    if (g.rendering != kind) end_rendering();
    if (!g.rendering) {
        /* the two kinds of pass write the colour image as an attachment and as storage: a barrier between them, also
           when passes into other targets or a submission came between */
        if (t->pass && t->pass != kind) snx_ctr_vk_barrier(cb);
        t->pass = kind;
        VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                           .imageView = t->color.view,
                                           .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                           .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                           .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        VkRenderingAttachmentInfo ds = color;
        ds.imageView = t->depth.view;
        VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                              .renderArea = {{0, 0}, {t->key.width * t->key.scale, t->key.height * t->key.scale}},
                              .layerCount = 1,
                              .colorAttachmentCount = k.shadow ? 0u : 1u,
                              .pColorAttachments = &color,
                              .pDepthAttachment = t->key.depth_format != NO_DEPTH ? &ds : NULL,
                              .pStencilAttachment = t->key.depth_format != NO_DEPTH ? &ds : NULL};
        vkCmdBeginRendering(cb, &ri);
        g.rendering = kind;
    }
    int32_t cx = (int32_t)(r[VIEWPORT_CORNER] << 22) >> 22, cy = (int32_t)(r[VIEWPORT_CORNER] << 6) >> 22;
    float scale = (float)key.scale;
    VkViewport viewport = {(float)cx * scale, (float)cy * scale, 2.0f * half_x * scale, 2.0f * half_y * scale, 0.0f,
                           1.0f};
    VkRect2D scissor = {{0, 0}, {key.width * key.scale, key.height * key.scale}};
    float constants[4];
    for (unsigned c = 0; c < 4; ++c) constants[c] = (float)(r[OM_BLEND_CONST] >> (c * 8) & 0xff) / 255.0f;
    if (objects) {
        ObjectState state;
        memset(&state, 0, sizeof state); /* its padding too: object_bind compares it */
        state.key = k;
        state.key.vs = state.key.fs = 0;
        state.viewport = viewport;
        state.scissor = scissor;
        memcpy(state.constants, constants, sizeof constants);
        state.scaled = stand_in_vertex;
        object_bind(cb, stand_in_vertex ? g.stand_in_vertex : g.vs[k.vs].so.object,
                    stand_in ? g.stand_in : g.fs[k.fs].so.object, &state);
        if (stand_in) {
            StandIn constants = stand_in_of(&fs);
            vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof constants, &constants);
            ++g.stood_in;
        }
        if (stand_in_vertex) {
            StandInVertex constants = stand_in_vertex_of(&vs);
            vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_VERTEX_BIT, sizeof(StandIn), sizeof constants, &constants);
            ++g.stood_in_vertex;
        }
    } else {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &scissor);
        vkCmdSetBlendConstants(cb, constants);
    }
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, k.shadow ? 3u : 2u, sets, 0, NULL);
    for (uint32_t b = 0; b < 13; ++b)
        if (k.binding_mask >> b & 1) vkCmdBindVertexBuffers(cb, b, 1, &buffers[b], &offsets[b]);
    if (source == ARRAYS) draw_reads(&k, binding_pa, index_pa, index_bytes, count, r[VERTEX_OFFSET], index_max);
    if (indexed && source == ARRAYS) {
        VkDeviceSize at;
        const SnxCtrVkRam* ram = snx_ctr_vk_ram(index_pa, 1, &at);
        vkCmdBindIndexBuffer(cb, snx_ctr_vk_work(ram), at, wide ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT8_EXT);
        vkCmdDrawIndexed(cb, count, 1, 0, 0, 0);
    } else
        vkCmdDraw(cb, count, 1, source == ARRAYS ? r[VERTEX_OFFSET] : 0, 0);
    /* drawn into: newer than memory, which is the GPU's until the target is stored, and the textures made from that
       memory stale */
    t->used = snx_ctr_vk_serial();
    if ((k.color_mask || k.shadow) && !t->color_dirty) {
        drop_textures(t->key.color_pa, color_span(&t->key));
        set_color(t, t->color_valid, true);
    }
    if (depth && k.depth_write && ((om >> 12 & 1) || k.stencil_on) && !t->depth_dirty) {
        drop_textures(t->key.depth_pa, depth_span(&t->key));
        set_depth(t, t->depth_valid, true);
    }
    return SNX_CTR_PICA_OK;
}
static SnxCtrPicaResult draw(SnxCtrPica* p, Source source, bool indexed) {
    SnxCtrPicaResult r = record(p, source, indexed);
    if (r != AGAIN) return r;
    /* out of descriptors or uploads: what is recorded is submitted (no wait), and the draw starts over in the next
       slot */
    snx_ctr_vk_submit();
    r = record(p, source, indexed);
    return r == AGAIN ? stop(p, "a draw needs more than one recording holds") : r;
}

/* a triangle a draw with a geometry shader made, kept for the draw, its quaternions turned to the first one's side as
   Azahar's AddTriangle does, so they interpolate the short way. *context is set when the host runs out of memory */
static void keep(void* context, const SnxCtrVertex* v0, const SnxCtrVertex* v1, const SnxCtrVertex* v2) {
    if (!grow((void**)&g.triangles, &g.triangle_capacity, g.triangle_count, sizeof *g.triangles)) {
        *(bool*)context = true;
        return;
    }
    SnxCtrVertex* t = g.triangles[g.triangle_count++];
    t[0] = *v0;
    t[1] = *v1;
    t[2] = *v2;
    for (unsigned i = 1; i < 3; ++i) {
        float dot = 0.0f;
        for (unsigned c = 0; c < 4; ++c) dot += t[0].quat[c] * t[i].quat[c];
        if (dot < 0.0f)
            for (unsigned c = 0; c < 4; ++c) t[i].quat[c] = -t[i].quat[c];
    }
}
/* A draw with a geometry shader runs its vertex and geometry shaders on the CPU (draw.c), as Azahar does: the geometry
   shader keeps its registers from one primitive to the next, which the GPU's invocations cannot. The triangles they
   make are drawn with a vertex shader that passes them on, TRIANGLES_AT_ONCE at a time */
static SnxCtrPicaResult draw_triangles(SnxCtrPica* p) {
    SnxCtrPicaResult r = SNX_CTR_PICA_OK;
    for (g.triangle_first = 0; r == SNX_CTR_PICA_OK && g.triangle_first < g.triangle_count;
         g.triangle_first += TRIANGLES_AT_ONCE)
        r = draw(p, TRIANGLES, false);
    g.triangle_count = 0;
    return r;
}

SnxCtrPicaResult snx_ctr_vk_draw(SnxCtrPica* p, SnxCtrMemory* memory, bool indexed) {
    if (!(p->regs[SNX_PICA_USE_GS] & 3)) return draw(p, ARRAYS, indexed);
    bool out_of_memory = false;
    SnxCtrPicaResult r = snx_ctr_pica_triangles(p, memory, indexed, keep, &out_of_memory);
    if (r == SNX_CTR_PICA_OK && out_of_memory) r = stop(p, "the host is out of memory");
    if (r == SNX_CTR_PICA_OK) return draw_triangles(p);
    g.triangle_count = 0;
    return r;
}
SnxCtrPicaResult snx_ctr_vk_vertex(SnxCtrPica* p, SnxCtrMemory* memory) {
    (void)memory;
    if (p->regs[SNX_PICA_USE_GS] & 3) { /* its triangles are drawn with the others of the batch, at vertices_end */
        bool out_of_memory = false;
        SnxCtrPicaResult r = snx_ctr_pica_triangles_immediate(p, keep, &out_of_memory);
        if (r == SNX_CTR_PICA_OK && out_of_memory) r = stop(p, "the host is out of memory");
        if (r != SNX_CTR_PICA_OK) g.triangle_count = 0;
        return r;
    }
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
    if (g.triangle_count) return draw_triangles(p);
    if (!g.immediate_count) return SNX_CTR_PICA_OK;
    SnxCtrPicaResult r = draw(p, IMMEDIATE, false);
    g.immediate_count = 0;
    return r;
}

void snx_ctr_vk_render_begin(unsigned slot) {
    g.slot = slot;
    g.ring_at = 0;
    g.vs_uploaded = g.fs_uploaded = g.lut_uploaded = false; /* the last uploads were the other slot's */
    /* a new recording has no shaders bound and no state set */
    g.bound[0] = g.bound[1] = VK_NULL_HANDLE;
    g.object_set = false;
    g.program_uploaded = false;
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
/* every page whose copy is newer than the RAM copied back, and waited for: the RAM holds what the GPU wrote once the
   device is gone */
static void copy_every_page_back(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    bool shadow = snx_ctr_vk_shadow();
    if (!g.pages || !(snx_ctr_vk_copies() || shadow)) return;
    uint32_t index = 0;
    bool copied = false;
    /* The GPU's submissions done, without recording what is open: nothing writes the copy the CPU reads. Work the
       open recording holds and never submitted is dropped, and a page it wrote is copied back from the copy, which
       already holds the guest's bytes (copy_in put them there, or the page was the same): only that work is lost */
    if (shadow) {
        uint64_t last = snx_ctr_vk_serial();
        if (last > 1) (void)snx_ctr_vk_wait(last - 1, UINT64_MAX);
    }
    for (unsigned i = 0; i < v->ram_count; ++i) {
        const SnxCtrVkRam* ram = &v->ram[i];
        for (uint32_t at = 0; at < ram->size;) {
            uint32_t run = at;
            while (run < ram->size && g.pages[index + run / 4096].copy == COPY_NEWER) {
                g.pages[index + run / 4096].copy = COPY_SAME;
                run += 4096;
            }
            if (run == at) {
                at += 4096;
                continue;
            }
            if (shadow) {
                memcpy(ram->guest + at, ram->map + at, run - at);
                g.traffic.copied_out += run - at;
                at = run;
                continue;
            }
            VkCommandBuffer cb = snx_ctr_vk_commands();
            if (!copied) {
                end_rendering();
                snx_ctr_vk_barrier(cb);
            }
            VkBufferCopy c = {at, at, run - at};
            vkCmdCopyBuffer(cb, ram->copy, ram->buffer, 1, &c);
            g.traffic.copied_out += run - at;
            copied = true;
            at = run;
        }
        index += ram->size / 4096;
    }
    if (copied) (void)snx_ctr_vk_finish();
}
/* ---- the pipeline cache kept between runs ---- */

void snx_ctr_vk_pipeline_cache_open(const char* directory) {
    const SnxCtrVk* v = snx_ctr_vk();
    if (!v || g.cache || !g_settings.vulkan_pipeline_cache) return;
    char dir[1100], tag[SNX_VK_DRIVER_TAG];
    snprintf(dir, sizeof dir, "%s/shader_cache", directory);
    snx_path_native(dir);
    if (!snx_is_dir(dir) && plat_mkdir(dir)) {
        char why[256];
        fprintf(stderr, "3ds gpu: no pipeline cache kept between runs: cannot create %s: %s\n", dir,
                snx_os_error(why, sizeof why));
        return;
    }
    /* each driver's file apart, as the Switch's: d<driver ID>-<vendor>-<device> */
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(v->phys, &props);
    snx_vk_driver_tag(tag, sizeof tag, (uint32_t)driver.driverID, props.properties.vendorID, props.properties.deviceID,
                      NULL);
    snx_vk_driver_path(g.cache_path, sizeof g.cache_path, dir, "vk_pipelines", tag, ".bin");
    snx_path_native(g.cache_path);
    size_t loaded = 0;
    if (v->shader_object) {
        /* the draws make no pipelines: the shaders' binaries are what an earlier run keeps */
        snx_vk_driver_path(g.binaries_path, sizeof g.binaries_path, dir, "vk_shaders", tag, ".bin");
        snx_path_native(g.binaries_path);
        snx_vk_shader_binaries_init(&g.binaries, v->shader_binary_uuid, v->shader_binary_version);
        snx_vk_shader_binaries_load(&g.binaries, g.binaries_path, &loaded);
        g.binaries_open = true;
        if (loaded)
            fprintf(stderr, "3ds gpu: the binaries of %zu shaders of earlier runs loaded (%zu KB)\n", g.binaries.count,
                    loaded >> 10);
        return;
    }
    char why[512];
    g.cache = snx_vk_pipeline_cache_load(v->device, &props.properties, g.cache_path, &loaded, why, sizeof why);
    if (why[0]) fprintf(stderr, "3ds gpu: %s: %s\n", g.cache_path, why);
    if (loaded) fprintf(stderr, "3ds gpu: the pipeline cache of earlier runs loaded (%zu KB)\n", loaded >> 10);
}
/* No pipeline for this long, and none queued or being made, before a write while the game runs: the driver holds the
   cache while it copies it out, and a pipeline made meanwhile would wait for the whole copy (the Switch's renderer
   once waited 3.2 s so) */
#define CACHE_QUIET_MS 20000u
void snx_ctr_vk_pipeline_cache_save(bool at_exit) {
    bool pipelines = g.cache && __atomic_load_n(&g.cache_dirty, __ATOMIC_RELAXED);
    bool shaders = g.binaries_open && g.binaries.dirty;
    if (!pipelines && !shaders) return;
    if (!at_exit) {
        uint64_t now = (uint64_t)(snx_seconds() * 1000);
        if (now - __atomic_load_n(&g.cache_used_ms, __ATOMIC_RELAXED) < CACHE_QUIET_MS) return;
        bool busy = false;
        if (g.job_lock) {
            SDL_LockMutex(g.job_lock);
            for (unsigned i = 0; i < PIPE_JOBS && !busy; ++i)
                busy = g.jobs[i].state == JOB_QUEUED || g.jobs[i].state == JOB_MAKING;
            SDL_UnlockMutex(g.job_lock);
        }
        if (busy) return;
    }
    size_t saved = 0;
    char why[512];
    if (shaders) {
        if (snx_vk_shader_binaries_save(&g.binaries, g.binaries_path, &saved, why, sizeof why))
            fprintf(stderr, "3ds gpu: the shaders' binaries could not be written to %s: %s\n", g.binaries_path, why);
        else if (at_exit)
            fprintf(stderr, "3ds gpu: the binaries of %zu shaders written (%zu KB)\n", g.binaries.count, saved >> 10);
    }
    if (!pipelines) return;
    __atomic_store_n(&g.cache_dirty, 0, __ATOMIC_RELAXED);
    if (snx_vk_pipeline_cache_save(device(), g.cache, g.cache_path, &saved, why, sizeof why)) {
        fprintf(stderr, "3ds gpu: the pipeline cache could not be written to %s: %s\n", g.cache_path, why);
        /* written again after another quiet time, or at the end */
        __atomic_store_n(&g.cache_used_ms, (uint64_t)(snx_seconds() * 1000), __ATOMIC_RELAXED);
        __atomic_store_n(&g.cache_dirty, 1, __ATOMIC_RELAXED);
    } else if (at_exit)
        fprintf(stderr, "3ds gpu: the pipeline cache written (%zu KB)\n", saved >> 10);
}

void snx_ctr_vk_render_free(void) {
    if (!snx_ctr_vk()) {
        memset(&g, 0, sizeof g);
        return;
    }
    copy_every_page_back();
    VkDevice d = device();
    if (g.worker_count) {
        pipes_wait(true);
        SDL_LockMutex(g.job_lock);
        g.jobs_stop = true;
        SDL_BroadcastCondition(g.job_queued);
        SDL_UnlockMutex(g.job_lock);
        for (unsigned i = 0; i < g.worker_count; ++i) SDL_WaitThread(g.workers[i], NULL);
        for (unsigned i = 0; i < PIPE_JOBS; ++i)
            if (g.jobs[i].state == JOB_DONE) {
                vkDestroyPipeline(d, g.jobs[i].made, NULL);
                /* a shader no draw collected: its binary kept all the same */
                if (g.jobs[i].object && g.binaries_open)
                    (void)snx_vk_shader_binaries_keep(&g.binaries, d, g.jobs[i].binary_key, g.jobs[i].object);
                if (g.jobs[i].object) vkDestroyShaderEXT(d, g.jobs[i].object, NULL);
                free(g.jobs[i].words);
            }
    }
    snx_ctr_vk_pipeline_cache_save(true); /* every pipeline made, the workers stopped */
    if (g.cache) vkDestroyPipelineCache(d, g.cache, NULL);
    if (g.job_done) SDL_DestroyCondition(g.job_done);
    if (g.job_queued) SDL_DestroyCondition(g.job_queued);
    if (g.job_lock) SDL_DestroyMutex(g.job_lock);
    if (g.skipped)
        fprintf(stderr, "3ds gpu: async_pipelines=%d skipped %llu draw%s while their %s made\n",
                g_settings.async_pipelines, (unsigned long long)g.skipped, g.skipped == 1 ? "" : "s",
                snx_ctr_vk()->shader_object ? "shaders were" : "pipelines were");
    if (g.stood_in || g.stood_in_vertex)
        fprintf(stderr,
                "3ds gpu: the stand-ins drew %llu draw%s with their fragment shader and %llu with their vertex "
                "shader while %s\n",
                (unsigned long long)g.stood_in, g.stood_in == 1 ? "" : "s", (unsigned long long)g.stood_in_vertex,
                g_settings.ctr_stand_in == 2 ? "ctr_stand_in=2 gave them every draw they draw" : "their own were made");
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
    for (size_t i = 0; i < g.vs_count; ++i) {
        if (g.vs[i].module) vkDestroyShaderModule(d, g.vs[i].module, NULL);
        if (g.vs[i].so.object) vkDestroyShaderEXT(d, g.vs[i].so.object, NULL);
        free(g.vs[i].so.words);
    }
    for (size_t i = 0; i < g.fs_count; ++i) {
        if (g.fs[i].module) vkDestroyShaderModule(d, g.fs[i].module, NULL);
        if (g.fs[i].so.object) vkDestroyShaderEXT(d, g.fs[i].so.object, NULL);
        free(g.fs[i].so.words);
    }
    if (g.stand_in) vkDestroyShaderEXT(d, g.stand_in, NULL);
    if (g.stand_in_vertex) vkDestroyShaderEXT(d, g.stand_in_vertex, NULL);
    snx_vk_shader_binaries_free(&g.binaries);
    for (unsigned i = 0; i < g.sampler_count; ++i) vkDestroySampler(d, g.samplers[i].sampler, NULL);
    image_free(&g.dummy);
    image_free(&g.dummy_cube);
    if (g.ring) vkDestroyBuffer(d, g.ring, NULL);
    if (g.ring_memory) vkFreeMemory(d, g.ring_memory, NULL);
    if (g.layout) vkDestroyPipelineLayout(d, g.layout, NULL);
    if (g.set0) vkDestroyDescriptorSetLayout(d, g.set0, NULL);
    if (g.set1) vkDestroyDescriptorSetLayout(d, g.set1, NULL);
    if (g.set2) vkDestroyDescriptorSetLayout(d, g.set2, NULL);
    snx_ctr_vk_compute_free(&g.color);
    snx_ctr_vk_compute_free(&g.depth);
    snx_ctr_vk_compute_free(&g.texture);
    free(g.vs);
    free(g.fs);
    free(g.pipes);
    free(g.immediate);
    free(g.triangles);
    memset(&g, 0, sizeof g);
}
