/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's Vulkan (gvk.h): the slots of the game's Vulkan loader, and the copies of the structures it passes with
   their pointers made host ones */
#include "nx/gpu/gvk/gvk.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/loader.h"
#include "nx/hle/hook/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <stdlib.h>
#include <string.h>

/* the host's structures have the guest's layout (gvk.h): 64-bit pointers, little-endian */
typedef char gvk_host_lp64[sizeof(void*) == 8 && sizeof(size_t) == 8 ? 1 : -1];

#define GVK_EXPORTS 512 /* the loader of one driver exports 246 */
#define GVK_CHUNKS  24  /* scratch chunks, each at least twice the one before */
#define GVK_CHUNK   (1u << 20)

static struct {
    PMutex lock; /* unknown_said */
    u32 nexports;
    const char* export_name[GVK_EXPORTS];
    u64 export_addr[GVK_EXPORTS];
    u32 unknown_said[64]; /* the pNext structure types said once */
    u32 nunknown;
} g_gvk = {.lock = PMUTEX_INIT};

typedef struct {
    u8* chunk[GVK_CHUNKS];
    size_t cap[GVK_CHUNKS];
    u32 n, cur;
    size_t used;
} GvkTls;
static _Thread_local GvkTls t_gvk;

/* ------------------------------------------------------------ scratch */
void gvk_scratch_reset(void) {
    t_gvk.cur = 0;
    t_gvk.used = 0;
}

void* gvk_scratch(size_t n) {
    GvkTls* t = &t_gvk;
    n = (n + 15) & ~(size_t)15;
    for (;;) {
        if (t->cur < t->n && n <= t->cap[t->cur] - t->used) {
            void* p = t->chunk[t->cur] + t->used;
            t->used += n;
            return p;
        }
        if (t->cur < t->n && t->used) { /* the next chunk, from its start */
            t->cur++;
            t->used = 0;
            continue;
        }
        /* chunk cur doesn't exist, or holds less than n when empty: made (again) at least twice the size before it */
        if (t->cur == GVK_CHUNKS) fatal("gvk: a call's structures take more than %d scratch chunks", GVK_CHUNKS);
        size_t cap = t->cur ? t->cap[t->cur - 1] * 2 : GVK_CHUNK;
        if (cap < n) cap = n;
        if (t->cur < t->n) free(t->chunk[t->cur]);
        u8* p = malloc(cap);
        if (!p) fatal("gvk: out of memory for %zu bytes of a call's structures", cap);
        t->chunk[t->cur] = p;
        t->cap[t->cur] = cap;
        if (t->cur == t->n) t->n++;
        t->used = 0;
    }
}

/* ------------------------------------------------------------ copies */
static u64 ld64(const u8* p) {
    u64 v;
    memcpy(&v, p, sizeof v);
    return v;
}
static void st_ptr(u8* p, const void* v) { memcpy(p, &v, sizeof v); }

/* whether the game mapped [g, g + n): a pointer a structure holds where Vulkan ignores it can be anything */
static int mapped(u64 g, u64 n) { return g && n && g < AS_SIZE && n <= AS_SIZE - g && mem_user_mapped(g, n); }

static const GvkType* chain_type(u32 stype);

/* plain data the host reads where the game wrote it */
static void* plain_in(u64 g, u64 n, const char* what) {
    if (!g || !n) return NULL;
    if (mapped(g, n)) return GP(g);
    LOG_LIMIT(16, "gvk: %llu bytes of %s at %llx aren't mapped: the host is given none\n", (unsigned long long)n, what,
              (unsigned long long)g);
    return NULL;
}
/* a string copied to scratch, up to its NUL within 256 bytes: the host would read to the NUL wherever it is */
static void* str_in(u64 g, const char* what) {
    if (!g) return NULL;
    char* s = gvk_scratch(256);
    if (mem_user_string(s, g, 256) > 0) return s;
    LOG_LIMIT(16, "gvk: a string of %s at %llx isn't mapped or ends past 256 bytes: the host is given none\n", what,
              (unsigned long long)g);
    return NULL;
}

static void fix(const GvkType* t, u8* h) {
    if (t->stype) st_ptr(h + 8, gvk_chain_in(ld64(h + 8)));
    for (const GvkField* f = t->f; f->kind; f++) {
        if (f->kind == GVK_EMBED) {
            fix(f->type, h + f->off);
            continue;
        }
        u64 g = ld64(h + f->off), count = 1;
        if (f->count_size == 4) {
            u32 c;
            memcpy(&c, h + f->count_off, sizeof c);
            count = c;
        } else if (f->count_size == 8) {
            count = ld64(h + f->count_off);
        }
        void* p = NULL;
        if (f->kind == GVK_STR)
            p = str_in(g, t->name);
        else if (f->kind == GVK_BYTES)
            p = plain_in(g, count, t->name);
        else if (f->type)
            p = gvk_in(f->type, g, count);
        else if (count <= UINT64_MAX / f->elem)
            p = plain_in(g, count * f->elem, t->name);
        st_ptr(h + f->off, p);
    }
}

void* gvk_in(const GvkType* t, u64 g, u64 count) {
    if (!g || !count) return NULL;
    if (count > AS_SIZE / t->size || !mapped(g, count * t->size)) {
        LOG_LIMIT(16, "gvk: %llu of %s at %llx aren't mapped: the host is given none\n", (unsigned long long)count,
                  t->name, (unsigned long long)g);
        return NULL;
    }
    size_t bytes = (size_t)(count * t->size);
    u8* h = gvk_scratch(bytes);
    memcpy(h, GP(g), bytes);
    for (u64 i = 0; i < count; i++) fix(t, h + i * t->size);
    return h;
}

/* a pNext chain: the structures gvk knows copied, the others left out of it (each type said once) */
void* gvk_chain_in(u64 g) {
    for (u32 hops = 0; mapped(g, 16) && hops < 64; hops++) {
        u32 st = gr32(g);
        const GvkType* t = chain_type(st);
        if (t) return gvk_in(t, g, 1);
        pm_lock(&g_gvk.lock);
        u32 i = 0;
        while (i < g_gvk.nunknown && g_gvk.unknown_said[i] != st) i++;
        if (i == g_gvk.nunknown && i < 64) {
            g_gvk.unknown_said[g_gvk.nunknown++] = st;
            WARN("gvk: a pNext structure of type %u isn't known: it is left out of the chain\n", st);
        }
        pm_unlock(&g_gvk.lock);
        g = gr64(g + 8);
    }
    return NULL;
}

void gvk_out(const GvkType* t, u64 g, void* host) {
    u8* h = host;
    while (t && h && mapped(g, t->size)) {
        memcpy(GP(g) + 16, h + 16, t->size - 16);
        u8* hn;
        memcpy(&hn, h + 8, sizeof hn);
        if (!hn) return;
        u32 st;
        memcpy(&st, hn, sizeof st);
        u64 gn = gr64(g + 8);
        while (mapped(gn, 16) && gr32(gn) != st) gn = gr64(gn + 8);
        g = gn;
        h = hn;
        t = chain_type(st);
    }
}

/* ------------------------------------------------------------ the structures */
#define SZ(S, m)               ((u8)sizeof(((S*)0)->m))
#define ARR(S, p, n, T)        {offsetof(S, p), GVK_ARRAY, SZ(S, n), offsetof(S, n), 0, &(T)}
#define PLAIN(S, p, n, E)      {offsetof(S, p), GVK_ARRAY, SZ(S, n), offsetof(S, n), sizeof(E), NULL}
#define ONE(S, p, T)           {offsetof(S, p), GVK_ONE, 0, 0, 0, &(T)}
#define ONE_PLAIN(S, p, E)     {offsetof(S, p), GVK_ONE, 0, 0, sizeof(E), NULL}
#define BYTES(S, p, n)         {offsetof(S, p), GVK_BYTES, SZ(S, n), offsetof(S, n), 1, NULL}
#define STR(S, p)              {offsetof(S, p), GVK_STR, 0, 0, 1, NULL}
#define EMBED(S, m, T)         {offsetof(S, m), GVK_EMBED, 0, 0, 0, &(T)}
#define CHAINED(N, S, ST, ...) const GvkType N = {#S, sizeof(S), ST, {__VA_ARGS__}}
#define STRUCT(N, S, ...)      static const GvkType N = {#S, sizeof(S), 0, {__VA_ARGS__}}

/* a pNext chain's structures, and the structures inside others */
static const GvkType t_spec_info, t_stage, t_vertex_input, t_input_assembly, t_tess, t_viewport, t_raster,
    t_multisample, t_depth_stencil, t_blend, t_dynamic, t_subpass, t_dsbinding, t_inherit, t_dedicated_req,
    t_dedicated_alloc, t_format_list, t_reduction, t_view_usage, t_depth_clip, t_provoking, t_color_write;

STRUCT(t_spec_info, VkSpecializationInfo,
       PLAIN(VkSpecializationInfo, pMapEntries, mapEntryCount, VkSpecializationMapEntry),
       BYTES(VkSpecializationInfo, pData, dataSize));
static CHAINED(t_stage, VkPipelineShaderStageCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
               STR(VkPipelineShaderStageCreateInfo, pName),
               ONE(VkPipelineShaderStageCreateInfo, pSpecializationInfo, t_spec_info));
static CHAINED(t_vertex_input, VkPipelineVertexInputStateCreateInfo,
               VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
               PLAIN(VkPipelineVertexInputStateCreateInfo, pVertexBindingDescriptions, vertexBindingDescriptionCount,
                     VkVertexInputBindingDescription),
               PLAIN(VkPipelineVertexInputStateCreateInfo, pVertexAttributeDescriptions,
                     vertexAttributeDescriptionCount, VkVertexInputAttributeDescription));
static CHAINED(t_input_assembly, VkPipelineInputAssemblyStateCreateInfo,
               VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, {0});
static CHAINED(t_tess, VkPipelineTessellationStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
               {0});
static CHAINED(t_viewport, VkPipelineViewportStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
               PLAIN(VkPipelineViewportStateCreateInfo, pViewports, viewportCount, VkViewport),
               PLAIN(VkPipelineViewportStateCreateInfo, pScissors, scissorCount, VkRect2D));
static CHAINED(t_raster, VkPipelineRasterizationStateCreateInfo,
               VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, {0});
/* the sample mask of a 64-sample pipeline is two words: two are read, as a check of one would let the host past it */
static CHAINED(t_multisample, VkPipelineMultisampleStateCreateInfo,
               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
               {offsetof(VkPipelineMultisampleStateCreateInfo, pSampleMask), GVK_ONE, 0, 0, 2 * sizeof(VkSampleMask),
                NULL});
static CHAINED(t_depth_stencil, VkPipelineDepthStencilStateCreateInfo,
               VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, {0});
static CHAINED(t_blend, VkPipelineColorBlendStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
               PLAIN(VkPipelineColorBlendStateCreateInfo, pAttachments, attachmentCount,
                     VkPipelineColorBlendAttachmentState));
static CHAINED(t_dynamic, VkPipelineDynamicStateCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
               PLAIN(VkPipelineDynamicStateCreateInfo, pDynamicStates, dynamicStateCount, VkDynamicState));
STRUCT(t_subpass, VkSubpassDescription,
       PLAIN(VkSubpassDescription, pInputAttachments, inputAttachmentCount, VkAttachmentReference),
       PLAIN(VkSubpassDescription, pColorAttachments, colorAttachmentCount, VkAttachmentReference),
       PLAIN(VkSubpassDescription, pResolveAttachments, colorAttachmentCount, VkAttachmentReference),
       ONE_PLAIN(VkSubpassDescription, pDepthStencilAttachment, VkAttachmentReference),
       PLAIN(VkSubpassDescription, pPreserveAttachments, preserveAttachmentCount, u32));
STRUCT(t_dsbinding, VkDescriptorSetLayoutBinding,
       PLAIN(VkDescriptorSetLayoutBinding, pImmutableSamplers, descriptorCount, VkSampler));
static CHAINED(t_inherit, VkCommandBufferInheritanceInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, {0});
static CHAINED(t_dedicated_req, VkMemoryDedicatedRequirements, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS, {0});
static CHAINED(t_dedicated_alloc, VkMemoryDedicatedAllocateInfo, VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, {0});
static CHAINED(t_format_list, VkImageFormatListCreateInfo, VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO,
               PLAIN(VkImageFormatListCreateInfo, pViewFormats, viewFormatCount, VkFormat));
static CHAINED(t_reduction, VkSamplerReductionModeCreateInfo, VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
               {0});
static CHAINED(t_view_usage, VkImageViewUsageCreateInfo, VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, {0});
static CHAINED(t_depth_clip, VkPipelineRasterizationDepthClipStateCreateInfoEXT,
               VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE_CREATE_INFO_EXT, {0});
static CHAINED(t_provoking, VkPipelineRasterizationProvokingVertexStateCreateInfoEXT,
               VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT, {0});
static CHAINED(t_color_write, VkPipelineColorWriteCreateInfoEXT, VK_STRUCTURE_TYPE_PIPELINE_COLOR_WRITE_CREATE_INFO_EXT,
               PLAIN(VkPipelineColorWriteCreateInfoEXT, pColorWriteEnables, attachmentCount, VkBool32));

/* the ones gvk_api.c passes */
CHAINED(gvk_t_submit, VkSubmitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO,
        PLAIN(VkSubmitInfo, pWaitSemaphores, waitSemaphoreCount, VkSemaphore),
        PLAIN(VkSubmitInfo, pWaitDstStageMask, waitSemaphoreCount, VkPipelineStageFlags),
        PLAIN(VkSubmitInfo, pCommandBuffers, commandBufferCount, VkCommandBuffer),
        PLAIN(VkSubmitInfo, pSignalSemaphores, signalSemaphoreCount, VkSemaphore));
CHAINED(gvk_t_buffer, VkBufferCreateInfo, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        PLAIN(VkBufferCreateInfo, pQueueFamilyIndices, queueFamilyIndexCount, u32));
CHAINED(gvk_t_image, VkImageCreateInfo, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        PLAIN(VkImageCreateInfo, pQueueFamilyIndices, queueFamilyIndexCount, u32));
CHAINED(gvk_t_image_view, VkImageViewCreateInfo, VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, {0});
CHAINED(gvk_t_buffer_view, VkBufferViewCreateInfo, VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, {0});
CHAINED(gvk_t_sampler, VkSamplerCreateInfo, VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, {0});
CHAINED(gvk_t_shader, VkShaderModuleCreateInfo, VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        BYTES(VkShaderModuleCreateInfo, pCode, codeSize));
CHAINED(gvk_t_cache, VkPipelineCacheCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        BYTES(VkPipelineCacheCreateInfo, pInitialData, initialDataSize));
CHAINED(gvk_t_playout, VkPipelineLayoutCreateInfo, VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        PLAIN(VkPipelineLayoutCreateInfo, pSetLayouts, setLayoutCount, VkDescriptorSetLayout),
        PLAIN(VkPipelineLayoutCreateInfo, pPushConstantRanges, pushConstantRangeCount, VkPushConstantRange));
CHAINED(gvk_t_dslayout, VkDescriptorSetLayoutCreateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        ARR(VkDescriptorSetLayoutCreateInfo, pBindings, bindingCount, t_dsbinding));
CHAINED(gvk_t_dspool, VkDescriptorPoolCreateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        PLAIN(VkDescriptorPoolCreateInfo, pPoolSizes, poolSizeCount, VkDescriptorPoolSize));
CHAINED(gvk_t_dsalloc, VkDescriptorSetAllocateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        PLAIN(VkDescriptorSetAllocateInfo, pSetLayouts, descriptorSetCount, VkDescriptorSetLayout));
/* which of its three arrays a write reads goes by its descriptor type: gvk_api.c clears the others */
CHAINED(gvk_t_write, VkWriteDescriptorSet, VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        PLAIN(VkWriteDescriptorSet, pImageInfo, descriptorCount, VkDescriptorImageInfo),
        PLAIN(VkWriteDescriptorSet, pBufferInfo, descriptorCount, VkDescriptorBufferInfo),
        PLAIN(VkWriteDescriptorSet, pTexelBufferView, descriptorCount, VkBufferView));
CHAINED(gvk_t_copy, VkCopyDescriptorSet, VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, {0});
CHAINED(gvk_t_rpass, VkRenderPassCreateInfo, VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        PLAIN(VkRenderPassCreateInfo, pAttachments, attachmentCount, VkAttachmentDescription),
        ARR(VkRenderPassCreateInfo, pSubpasses, subpassCount, t_subpass),
        PLAIN(VkRenderPassCreateInfo, pDependencies, dependencyCount, VkSubpassDependency));
CHAINED(gvk_t_fb, VkFramebufferCreateInfo, VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        PLAIN(VkFramebufferCreateInfo, pAttachments, attachmentCount, VkImageView));
CHAINED(gvk_t_gpipe, VkGraphicsPipelineCreateInfo, VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        ARR(VkGraphicsPipelineCreateInfo, pStages, stageCount, t_stage),
        ONE(VkGraphicsPipelineCreateInfo, pVertexInputState, t_vertex_input),
        ONE(VkGraphicsPipelineCreateInfo, pInputAssemblyState, t_input_assembly),
        ONE(VkGraphicsPipelineCreateInfo, pTessellationState, t_tess),
        ONE(VkGraphicsPipelineCreateInfo, pViewportState, t_viewport),
        ONE(VkGraphicsPipelineCreateInfo, pRasterizationState, t_raster),
        ONE(VkGraphicsPipelineCreateInfo, pMultisampleState, t_multisample),
        ONE(VkGraphicsPipelineCreateInfo, pDepthStencilState, t_depth_stencil),
        ONE(VkGraphicsPipelineCreateInfo, pColorBlendState, t_blend),
        ONE(VkGraphicsPipelineCreateInfo, pDynamicState, t_dynamic));
CHAINED(gvk_t_cpipe, VkComputePipelineCreateInfo, VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        EMBED(VkComputePipelineCreateInfo, stage, t_stage));
CHAINED(gvk_t_begin, VkCommandBufferBeginInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        ONE(VkCommandBufferBeginInfo, pInheritanceInfo, t_inherit));
CHAINED(gvk_t_rpbegin, VkRenderPassBeginInfo, VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        PLAIN(VkRenderPassBeginInfo, pClearValues, clearValueCount, VkClearValue));
CHAINED(gvk_t_mbar, VkMemoryBarrier, VK_STRUCTURE_TYPE_MEMORY_BARRIER, {0});
CHAINED(gvk_t_bbar, VkBufferMemoryBarrier, VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, {0});
CHAINED(gvk_t_ibar, VkImageMemoryBarrier, VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, {0});
CHAINED(gvk_t_pool, VkCommandPoolCreateInfo, VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, {0});
CHAINED(gvk_t_cballoc, VkCommandBufferAllocateInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, {0});
CHAINED(gvk_t_fence, VkFenceCreateInfo, VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, {0});
CHAINED(gvk_t_sem, VkSemaphoreCreateInfo, VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, {0});
CHAINED(gvk_t_query, VkQueryPoolCreateInfo, VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, {0});
CHAINED(gvk_t_event, VkEventCreateInfo, VK_STRUCTURE_TYPE_EVENT_CREATE_INFO, {0});
CHAINED(gvk_t_range, VkMappedMemoryRange, VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, {0});
CHAINED(gvk_t_imreq2, VkImageMemoryRequirementsInfo2, VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, {0});
CHAINED(gvk_t_bufreq2, VkBufferMemoryRequirementsInfo2, VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, {0});
CHAINED(gvk_t_req2, VkMemoryRequirements2, VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, {0});

static const GvkType* const g_chain_types[] = {
    &t_stage,           &t_vertex_input,  &t_input_assembly, &t_tess,        &t_viewport,       &t_raster,
    &t_multisample,     &t_depth_stencil, &t_blend,          &t_dynamic,     &t_inherit,        &t_dedicated_req,
    &t_dedicated_alloc, &t_format_list,   &t_reduction,      &t_view_usage,  &t_depth_clip,     &t_provoking,
    &t_color_write,     &gvk_t_submit,    &gvk_t_buffer,     &gvk_t_image,   &gvk_t_image_view, &gvk_t_buffer_view,
    &gvk_t_sampler,     &gvk_t_shader,    &gvk_t_cache,      &gvk_t_playout, &gvk_t_dslayout,   &gvk_t_dspool,
    &gvk_t_dsalloc,     &gvk_t_write,     &gvk_t_copy,       &gvk_t_rpass,   &gvk_t_fb,         &gvk_t_gpipe,
    &gvk_t_cpipe,       &gvk_t_begin,     &gvk_t_rpbegin,    &gvk_t_mbar,    &gvk_t_bbar,       &gvk_t_ibar,
    &gvk_t_pool,        &gvk_t_cballoc,   &gvk_t_fence,      &gvk_t_sem,     &gvk_t_query,      &gvk_t_event,
    &gvk_t_range,       &gvk_t_imreq2,    &gvk_t_bufreq2,    &gvk_t_req2};

static const GvkType* chain_type(u32 stype) {
    for (size_t i = 0; i < sizeof g_chain_types / sizeof *g_chain_types; i++)
        if (g_chain_types[i]->stype == stype) return g_chain_types[i];
    return NULL;
}

/* ------------------------------------------------------------ the slots */
int gvk_active(void) { return g_gvk.nexports != 0; }

u64 gvk_export(const char* name) {
    for (u32 i = 0; i < g_gvk.nexports; i++)
        if (!strcmp(g_gvk.export_name[i], name)) return g_gvk.export_addr[i];
    return 0;
}

/* a Vulkan function the game exports and gvk doesn't implement: the game stops, naming it */
static void missing(CPU* RESTRICT c, u64 entry) {
    (void)c;
    for (u32 i = 0; i < g_gvk.nexports; i++)
        if (g_gvk.export_addr[i] == entry)
            fatal("gvk: the game called %s, which the guest's Vulkan on the host (guest_vulkan) doesn't implement",
                  g_gvk.export_name[i]);
    fatal("gvk: the game called a Vulkan function at %llx that gvk doesn't know", (unsigned long long)entry);
}

void gvk_install(void) {
    if (!g_settings.guest_vulkan) return;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    /* the structures are passed in the guest's layout and byte order (gvk.h): a big-endian host would need each
       number in them converted */
    WARN("gvk: guest_vulkan is on, but this host is big-endian, where the game's Vulkan structures can't be passed "
         "as they are: its own driver runs\n");
    return;
#endif
    if (g_arm_mod) {
        WARN("gvk: guest_vulkan is on, but a 32-bit game's Vulkan isn't taken: its own driver runs\n");
        return;
    }
    /* the driver's module: the one that exports the loader's vkGetInstanceProcAddr. A vk function of another module
       (the game's own wrapper of one) is left alone */
    u64 getproc = kernel_find_symbol_exact("vkGetInstanceProcAddr");
    const SnxModInfo* driver = NULL;
    for (u32 i = 0; getproc && i < g_mod->nmods; i++)
        if (getproc - g_mod->mods[i].base < g_mod->mods[i].size) driver = &g_mod->mods[i];
    if (!driver || getproc < g_code_lo || getproc >= g_code_hi) {
        LOG("gvk: guest_vulkan is on, but no module of the game exports vkGetInstanceProcAddr in its code: nothing is "
            "taken\n");
        return;
    }
    /* a module recompiled before gvk.hook calls the driver directly, beside the host's Vulkan on the same objects */
    if (!hook_routed("vkQueueSubmit"))
        fatal("gvk: guest_vulkan=1 needs the game recompiled with this runtime's route list, which sends its direct "
              "Vulkan calls to gvk: recompile the game, or set guest_vulkan=0");
    u64 addr;
    const char* name;
    u32 taken = 0, unrouted = 0;
    for (u32 i = 0; kernel_symbol(i, &addr, &name); i++) {
        if (!name || name[0] != 'v' || name[1] != 'k' || name[2] < 'A' || name[2] > 'Z') continue;
        if (addr - driver->base >= driver->size || addr < g_code_lo || addr >= g_code_hi || (addr & 3) ||
            gvk_export(name))
            continue;
        if (g_gvk.nexports == GVK_EXPORTS) fatal("gvk: the game exports more than %d Vulkan functions", GVK_EXPORTS);
        g_gvk.export_name[g_gvk.nexports] = name;
        g_gvk.export_addr[g_gvk.nexports++] = addr;
        GuestFn fn = gvk_api_find(name);
        taken += fn != NULL;
        if (!fn) fn = missing;
        GuestFn* slot = &g_disp[(addr - g_code_lo) >> 2];
        __atomic_store_n(slot, fn, __ATOMIC_RELEASE);
        nce_slot_taken(addr, "gvk");
        jit_slot_taken(addr, fn);
        if (!hook_routed(name)) {
            unrouted++;
            LOG_LIMIT(16, "gvk: the route list doesn't name %s: a direct call of the game to it runs its driver\n",
                      name);
        }
    }
    LOG("gvk: the %u Vulkan functions of the game's driver (%s) call the host's Vulkan, %u of them implemented (the "
        "others stop the game when called), %u not on the route list\n",
        g_gvk.nexports, driver->name ? driver->name : "?", taken, unrouted);
}
