/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's Vulkan (gvk.h): the slots of the game's Vulkan loader, and the copies of the structures it passes with
   their pointers made host ones */
#include "nx/gpu/gvk/gvk.h"
#include "nx/runtime/platform/plat.h"
#include <vulkan/vulkan_core.h>
#include "nx/runtime/loader.h"
#include "nx/nn/hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* the host's structures have the guest's layout (gvk.h): 64-bit pointers, little-endian */
static_assert(sizeof(void*) == 8 && sizeof(size_t) == 8, "a 64-bit host");

#define GVK_CHUNKS     24 /* scratch chunks, each at least twice the one before */
#define GVK_CHUNK      (1u << 20)
#define GVK_CHAIN_HOPS 64

typedef struct {
    const char* name;
    u64 addr;
    GuestFn fn;
    GuestFn orig;
} GvkExport;

static struct {
    PMutex lock; /* unknown_said */
    u32 nexports;
    GvkExport* exports;
    u64 driver_base, driver_size;
    u64 proc_base;
    GuestFn proc[3];
    const char* unrouted_impl; /* an implemented function the recompile has no route for (gvk_check_device) */
    u32 unknown_said[64];      /* the pNext structure types said once */
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

void gvk_scratch_free(void) {
    for (u32 i = 0; i < t_gvk.n; i++) free(t_gvk.chunk[i]);
    t_gvk = (GvkTls){0};
}

void* gvk_scratch(size_t n) {
    GvkTls* t = &t_gvk;
    n = (size_t)snx_align_up(n, 16);
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

int gvk_writable(u64 g, u64 size) {
    if (!g || g >= AS_END || size > AS_END - g) return 0;
    u64 end = g + size;
    while (g < end) {
        MemBlock b;
        mem_query(g, &b);
        if (b.state == MS_FREE || b.state == MS_INACCESSIBLE || !(b.perm & PERM_W)) return 0;
        g = b.addr + b.size;
    }
    return 1;
}

int gvk_unlisted(u64 names, u32 n, const char* listed, size_t step, u32 nlisted, char* name, size_t size) {
    name[0] = 0;
    if (!n) return 0;
    if (!mapped(names, 8ull * n)) return 1;
    for (u32 i = 0; i < n; i++) {
        u32 k = 0;
        if (mem_user_string(name, gr64(names + 8ull * i), size) <= 0) {
            name[0] = 0;
            return 1;
        }
        while (k < nlisted && strcmp(listed + k * step, name)) k++;
        if (k == nlisted) return 1;
    }
    name[0] = 0;
    return 0;
}

static const GvkType* chain_type(u32 parent, u32 stype);
static int chain_in(u32 parent, u64 g, const void** out);
static void* copy_in(const GvkType* t, u64 g, u64 count);

/* a structure the host can't be given: said, with the values that make it so, and the call fails */
#define BAD(...)                                                                                                       \
    do {                                                                                                               \
        WARN_LIMIT(16, "gvk: " __VA_ARGS__);                                                                           \
        return 0;                                                                                                      \
    } while (0)

static int fix(const GvkType* t, u8* h, int fix_chain) {
    if (t->stype && fix_chain) {
        const void* next;
        if (!chain_in(t->stype, ld64(h + 8), &next)) return 0;
        st_ptr(h + 8, next);
    }
    for (const GvkField* f = t->f; f->kind; f++) {
        if (f->kind == GVK_EMBED) {
            if (!fix(f->type, h + f->off, 1)) return 0;
            continue;
        }
        u64 g = ld64(h + f->off), count = 1;
        if (f->kind == GVK_DRAW_ONE) {
            const VkPipelineRasterizationStateCreateInfo* rs;
            memcpy(&rs, h + f->count_off, sizeof rs);
            if (rs && rs->rasterizerDiscardEnable) g = 0;
        }
        if (f->count_size == 4) {
            u32 c;
            memcpy(&c, h + f->count_off, sizeof c);
            count = c;
        } else if (f->count_size == 8) {
            count = ld64(h + f->count_off);
        }
        if (f->kind == GVK_SAMPLE_MASK) {
            u32 samples;
            memcpy(&samples, h + f->count_off, sizeof samples);
            if (samples == 0 || samples > VK_SAMPLE_COUNT_64_BIT || (samples & (samples - 1)))
                BAD("%s has rasterizationSamples %u, which is no sample count\n", t->name, samples);
            count = (samples + 31) / 32;
        }
        void* p = NULL;
        if (g && count) {
            if (f->kind == GVK_STR) {
                char* s = gvk_scratch(256);
                if (mem_user_string(s, g, 256) > 0) p = s;
            } else if (f->kind == GVK_OPTIONAL_ONE && f->type) {
                if (mapped(g, f->type->size) && (!f->type->stype || gr32(g) == f->type->stype))
                    p = copy_in(f->type, g, count);
            } else if (f->type) {
                p = copy_in(f->type, g, count);
            } else if (count <= AS_SIZE / f->elem && mapped(g, count * f->elem)) {
                p = GP(g);
            }
        }
        if (count && !p && f->kind != GVK_OPTIONAL_ARRAY && f->kind != GVK_OPTIONAL_ONE &&
            (g || f->kind == GVK_ARRAY || f->kind == GVK_BYTES || f->kind == GVK_STR))
            BAD("%s has %llu elements at %llx by the pointer at its byte %u, which the host can't be given\n", t->name,
                (unsigned long long)count, (unsigned long long)g, (unsigned)f->off);
        st_ptr(h + f->off, p);
    }
    return 1;
}

static void* copy_in(const GvkType* t, u64 g, u64 count) {
    if (!g || !count) return NULL;
    if (count > AS_SIZE / t->size || !mapped(g, count * t->size)) {
        LOG_LIMIT(16, "gvk: %llu of %s at %llx aren't mapped: the host is given none\n", (unsigned long long)count,
                  t->name, (unsigned long long)g);
        return NULL;
    }
    size_t bytes = (size_t)(count * t->size);
    u8* h = gvk_scratch(bytes);
    memcpy(h, GP(g), bytes);
    for (u64 i = 0; i < count; i++) {
        u8* item = h + i * t->size;
        if (t->stype && snx_ld_le32(item) != t->stype) {
            WARN_LIMIT(16, "gvk: %s %llu of %llu at %llx has the sType %u, not %u\n", t->name, (unsigned long long)i,
                       (unsigned long long)count, (unsigned long long)g, snx_ld_le32(item), t->stype);
            return NULL;
        }
        if (!fix(t, item, 1)) return NULL;
    }
    return h;
}

void* gvk_in(const GvkType* t, u64 g, u64 count) { return copy_in(t, g, count); }

/* a pNext chain: the structures gvk knows copied, the others left out of it (each type said once) */
static int chain_in(u32 parent, u64 g, const void** out) {
    u8* tail = NULL;
    u64 seen[GVK_CHAIN_HOPS];
    u32 hops = 0;
    *out = NULL;
    for (; g; g = gr64(g + 8)) {
        if (hops == GVK_CHAIN_HOPS)
            BAD("a pNext chain of the structure of type %u is longer than %u\n", parent, GVK_CHAIN_HOPS);
        if (!mapped(g, 16))
            BAD("a pNext chain of the structure of type %u points to %llx, which isn't mapped\n", parent,
                (unsigned long long)g);
        for (u32 i = 0; i < hops; i++)
            if (seen[i] == g)
                BAD("a pNext chain of the structure of type %u comes back to %llx\n", parent, (unsigned long long)g);
        seen[hops++] = g;
        u32 st = gr32(g);
        const GvkType* t = chain_type(parent, st);
        if (t) {
            if (!mapped(g, t->size))
                BAD("the %s at %llx in a pNext chain isn't all mapped\n", t->name, (unsigned long long)g);
            u8* copy = gvk_scratch(t->size);
            memcpy(copy, GP(g), t->size);
            if (snx_ld_le32(copy) != st || !fix(t, copy, 0)) return 0;
            for (const u8* p = *out; p;) {
                if (snx_ld_le32(p) == st)
                    BAD("a pNext chain of the structure of type %u has two %s\n", parent, t->name);
                memcpy(&p, p + 8, sizeof p);
            }
            st_ptr(copy + 8, NULL);
            if (tail)
                st_ptr(tail + 8, copy);
            else
                *out = copy;
            tail = copy;
            continue;
        }
        pm_lock(&g_gvk.lock);
        u32 i = 0;
        while (i < g_gvk.nunknown && g_gvk.unknown_said[i] != st) i++;
        if (i == g_gvk.nunknown && i < 64) {
            g_gvk.unknown_said[g_gvk.nunknown++] = st;
            WARN("gvk: a pNext structure of type %u isn't known: it is left out of the chain\n", st);
        }
        pm_unlock(&g_gvk.lock);
    }
    return 1;
}

int gvk_chain_in(u32 parent, u64 g, const void** out) { return chain_in(parent, g, out); }

void gvk_out(const GvkType* t, u64 g, void* host) {
    u8* h = host;
    u32 hops = GVK_CHAIN_HOPS;
    u32 parent = t->stype;
    while (t && h && gvk_writable(g, t->size)) {
        memcpy(GP(g) + 16, h + 16, t->size - 16);
        u8* hn;
        memcpy(&hn, h + 8, sizeof hn);
        if (!hn) return;
        u32 st;
        memcpy(&st, hn, sizeof st);
        u64 gn = gr64(g + 8);
        while (mapped(gn, 16)) {
            if (!hops) return;
            hops--;
            if (gr32(gn) == st) break;
            gn = gr64(gn + 8);
        }
        if (!mapped(gn, 16)) return;
        g = gn;
        h = hn;
        t = chain_type(parent, st);
    }
}

/* ------------------------------------------------------------ the structures */
#define SZ(S, m)               ((u8)sizeof(((S*)0)->m))
#define ARR(S, p, n, T)        {offsetof(S, p), GVK_ARRAY, SZ(S, n), offsetof(S, n), 0, &(T)}
#define PLAIN(S, p, n, E)      {offsetof(S, p), GVK_ARRAY, SZ(S, n), offsetof(S, n), sizeof(E), NULL}
#define OPT(S, p, n, E)        {offsetof(S, p), GVK_OPTIONAL_ARRAY, SZ(S, n), offsetof(S, n), sizeof(E), NULL}
#define ONE(S, p, T)           {offsetof(S, p), GVK_ONE, 0, 0, 0, &(T)}
#define OPT_ONE(S, p, T)       {offsetof(S, p), GVK_OPTIONAL_ONE, 0, 0, 0, &(T)}
#define DRAW_ONE(S, p, T)      {offsetof(S, p), GVK_DRAW_ONE, 0, offsetof(S, pRasterizationState), 0, &(T)}
#define ONE_PLAIN(S, p, E)     {offsetof(S, p), GVK_ONE, 0, 0, sizeof(E), NULL}
#define BYTES(S, p, n)         {offsetof(S, p), GVK_BYTES, SZ(S, n), offsetof(S, n), 1, NULL}
#define STR(S, p)              {offsetof(S, p), GVK_STR, 0, 0, 1, NULL}
#define EMBED(S, m, T)         {offsetof(S, m), GVK_EMBED, 0, 0, 0, &(T)}
#define CHAINED(N, S, ST, ...) const GvkType N = {#S, sizeof(S), ST, {__VA_ARGS__}}
#define STRUCT(N, S, ...)      static const GvkType N = {#S, sizeof(S), 0, {__VA_ARGS__}}

/* a pNext chain's structures, and the structures inside others */
static const GvkType t_spec_info, t_stage, t_vertex_input, t_input_assembly, t_tess, t_viewport, t_raster,
    t_multisample, t_depth_stencil, t_blend, t_dynamic, t_subpass, t_dsbinding, t_inherit, t_dedicated_req,
    t_dedicated_alloc, t_format_list, t_reduction, t_view_usage, t_depth_clip, t_provoking, t_color_write,
    t_domain_origin, t_input_aspect;

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
               OPT(VkPipelineViewportStateCreateInfo, pViewports, viewportCount, VkViewport),
               OPT(VkPipelineViewportStateCreateInfo, pScissors, scissorCount, VkRect2D));
static CHAINED(t_raster, VkPipelineRasterizationStateCreateInfo,
               VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, {0});
static CHAINED(t_multisample, VkPipelineMultisampleStateCreateInfo,
               VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
               {offsetof(VkPipelineMultisampleStateCreateInfo, pSampleMask), GVK_SAMPLE_MASK, 0,
                offsetof(VkPipelineMultisampleStateCreateInfo, rasterizationSamples), sizeof(VkSampleMask), NULL});
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
       OPT(VkSubpassDescription, pResolveAttachments, colorAttachmentCount, VkAttachmentReference),
       ONE_PLAIN(VkSubpassDescription, pDepthStencilAttachment, VkAttachmentReference),
       PLAIN(VkSubpassDescription, pPreserveAttachments, preserveAttachmentCount, u32));
STRUCT(t_dsbinding, VkDescriptorSetLayoutBinding,
       OPT(VkDescriptorSetLayoutBinding, pImmutableSamplers, descriptorCount, VkSampler));
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
static CHAINED(t_domain_origin, VkPipelineTessellationDomainOriginStateCreateInfo,
               VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_DOMAIN_ORIGIN_STATE_CREATE_INFO, {0});
static CHAINED(t_input_aspect, VkRenderPassInputAttachmentAspectCreateInfo,
               VK_STRUCTURE_TYPE_RENDER_PASS_INPUT_ATTACHMENT_ASPECT_CREATE_INFO,
               PLAIN(VkRenderPassInputAttachmentAspectCreateInfo, pAspectReferences, aspectReferenceCount,
                     VkInputAttachmentAspectReference));

/* the ones gvk_api.c passes */
CHAINED(gvk_t_submit, VkSubmitInfo, VK_STRUCTURE_TYPE_SUBMIT_INFO,
        PLAIN(VkSubmitInfo, pWaitSemaphores, waitSemaphoreCount, VkSemaphore),
        PLAIN(VkSubmitInfo, pWaitDstStageMask, waitSemaphoreCount, VkPipelineStageFlags),
        PLAIN(VkSubmitInfo, pCommandBuffers, commandBufferCount, VkCommandBuffer),
        PLAIN(VkSubmitInfo, pSignalSemaphores, signalSemaphoreCount, VkSemaphore));
CHAINED(gvk_t_buffer, VkBufferCreateInfo, VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        OPT(VkBufferCreateInfo, pQueueFamilyIndices, queueFamilyIndexCount, u32));
CHAINED(gvk_t_image, VkImageCreateInfo, VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        OPT(VkImageCreateInfo, pQueueFamilyIndices, queueFamilyIndexCount, u32));
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
        OPT(VkWriteDescriptorSet, pImageInfo, descriptorCount, VkDescriptorImageInfo),
        OPT(VkWriteDescriptorSet, pBufferInfo, descriptorCount, VkDescriptorBufferInfo),
        OPT(VkWriteDescriptorSet, pTexelBufferView, descriptorCount, VkBufferView));
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
        OPT_ONE(VkGraphicsPipelineCreateInfo, pTessellationState, t_tess),
        ONE(VkGraphicsPipelineCreateInfo, pRasterizationState, t_raster), /* before the four it can have ignored */
        DRAW_ONE(VkGraphicsPipelineCreateInfo, pViewportState, t_viewport),
        DRAW_ONE(VkGraphicsPipelineCreateInfo, pMultisampleState, t_multisample),
        /* and these two where the subpass has no such attachment: what is there can be anything */
        OPT_ONE(VkGraphicsPipelineCreateInfo, pDepthStencilState, t_depth_stencil),
        OPT_ONE(VkGraphicsPipelineCreateInfo, pColorBlendState, t_blend),
        ONE(VkGraphicsPipelineCreateInfo, pDynamicState, t_dynamic));
CHAINED(gvk_t_cpipe, VkComputePipelineCreateInfo, VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        EMBED(VkComputePipelineCreateInfo, stage, t_stage));
CHAINED(gvk_t_begin, VkCommandBufferBeginInfo, VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        OPT_ONE(VkCommandBufferBeginInfo, pInheritanceInfo, t_inherit));
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
CHAINED(gvk_t_template, VkDescriptorUpdateTemplateCreateInfo, VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        PLAIN(VkDescriptorUpdateTemplateCreateInfo, pDescriptorUpdateEntries, descriptorUpdateEntryCount,
              VkDescriptorUpdateTemplateEntry));
CHAINED(gvk_t_bind_buffer, VkBindBufferMemoryInfo, VK_STRUCTURE_TYPE_BIND_BUFFER_MEMORY_INFO, {0});
CHAINED(gvk_t_bind_image, VkBindImageMemoryInfo, VK_STRUCTURE_TYPE_BIND_IMAGE_MEMORY_INFO, {0});
CHAINED(gvk_t_imreq2, VkImageMemoryRequirementsInfo2, VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2, {0});
CHAINED(gvk_t_bufreq2, VkBufferMemoryRequirementsInfo2, VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, {0});
CHAINED(gvk_t_req2, VkMemoryRequirements2, VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, {0});

int gvk_writes_take(void* writes, u32 n) {
    VkWriteDescriptorSet* w = writes;
    for (u32 i = 0; i < n; i++) {
        const VkDescriptorImageInfo* image = NULL;
        const VkDescriptorBufferInfo* buffer = NULL;
        const VkBufferView* texel = NULL;
        const char* array = NULL;
        if (w[i].descriptorCount) {
            switch (w[i].descriptorType) {
                case VK_DESCRIPTOR_TYPE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
                case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
                case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
                    if (!(image = w[i].pImageInfo)) array = "pImageInfo";
                    break;
                case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
                case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
                    if (!(texel = w[i].pTexelBufferView)) array = "pTexelBufferView";
                    break;
                default:
                    if (!(buffer = w[i].pBufferInfo)) array = "pBufferInfo";
                    break;
            }
        }
        if (array)
            BAD("write %u of %u of descriptors (binding %u, type %d, %u of them) has no %s: none of the writes and copies "
                "of the call is done\n",
                i, n, w[i].dstBinding, (int)w[i].descriptorType, w[i].descriptorCount, array);
        w[i].pImageInfo = image;
        w[i].pBufferInfo = buffer;
        w[i].pTexelBufferView = texel;
    }
    return 1;
}

static const GvkType* chain_type(u32 parent, u32 stype) {
    static const struct {
        u32 parent;
        const GvkType* type;
    } ext[] = {
        {VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, &t_dedicated_req},
        {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &t_dedicated_alloc},
        {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &t_format_list},
        {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, &t_reduction},
        {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &t_view_usage},
        {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, &t_depth_clip},
        {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, &t_provoking},
        {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, &t_color_write},
        {VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, &t_domain_origin},
        {VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, &t_input_aspect},
    };
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++)
        if (ext[i].parent == parent && ext[i].type->stype == stype) return ext[i].type;
    return NULL;
}

/* Older loaders return these extension functions from GetInstanceProcAddr without exporting their names. */
static const char* const debug_procs[] = {"vkCreateDebugReportCallbackEXT", "vkDestroyDebugReportCallbackEXT",
                                          "vkDebugReportMessageEXT"};

u64 gvk_proc_space(u64 code_hi) {
    if (!g_settings.guest_vulkan || g_arm_mod || nce_wanted() || !code_hi) return code_hi;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return code_hi;
#endif
    u64 end = code_hi;
    for (u32 i = 0; i < g_mod->nmods; i++) {
        const SnxModInfo* m = &g_mod->mods[i];
        if (m->base + m->size > end) end = m->base + m->size;
    }
    u64 page = plat_vm_page();
    g_gvk.proc_base = snx_align_up(end, page);
    return g_gvk.proc_base + page;
}

/* ------------------------------------------------------------ the slots */
int gvk_active(void) { return g_gvk.nexports != 0; }

u64 gvk_export(const char* name) {
    for (u32 i = 0; i < g_gvk.nexports; i++)
        if (!strcmp(g_gvk.exports[i].name, name)) return g_gvk.exports[i].addr;
    if (g_gvk.proc_base)
        for (u32 i = 0; i < sizeof debug_procs / sizeof *debug_procs; i++)
            if (g_gvk.proc[i] && !strcmp(debug_procs[i], name)) return g_gvk.proc_base + 4 * i;
    return 0;
}

/* a Vulkan function the game exports and gvk doesn't implement: the game stops, naming it */
static void missing(CPU* RESTRICT c, u64 entry) {
    (void)c;
    for (u32 i = 0; i < g_gvk.nexports; i++)
        if (g_gvk.exports[i].addr == entry)
            fatal("gvk: the game called %s, which the guest's Vulkan on the host (guest_vulkan) doesn't implement",
                  g_gvk.exports[i].name);
    fatal("gvk: the game called a Vulkan function at %llx that gvk doesn't know", (unsigned long long)entry);
}

static void set_slot(u64 addr, GuestFn fn) {
    atomic_store_explicit(&g_disp[(addr - g_code_lo) >> 2], fn, memory_order_release);
    jit_slot_taken(addr, fn);
}

GuestFn gvk_invalidation_slot(u64 addr) {
    if (g_gvk.proc_base && addr - g_gvk.proc_base < sizeof debug_procs / sizeof *debug_procs * 4 && !(addr & 3)) {
        MemBlock b;
        mem_query(addr, &b);
        return (b.perm & PERM_X) ? g_gvk.proc[(addr - g_gvk.proc_base) / 4] : NULL;
    }
    if (addr - g_gvk.driver_base >= g_gvk.driver_size) return NULL;
    /* kernel_symbol enumerates exports in address order. */
    u32 lo = 0, hi = g_gvk.nexports;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (g_gvk.exports[mid].addr < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == g_gvk.nexports || g_gvk.exports[lo].addr != addr) return NULL;
    MemBlock b;
    mem_query(addr, &b);
    return b.state != MS_FREE && (b.perm & PERM_X) ? g_gvk.exports[lo].fn : NULL;
}

static int driver_export(const SnxModInfo* driver, u64 addr, const char* name) {
    return addr - driver->base < driver->size && addr >= g_code_lo && addr < g_code_hi && !(addr & 3) && name &&
           name[0] == 'v' && name[1] == 'k' && name[2] >= 'A' && name[2] <= 'Z';
}

/* gives each slot that gvk_install took back to the driver's code, with why in the log: the game's own driver runs */
static void release(const char* why) {
    WARN("%s", why);
    for (u32 i = 0; i < g_gvk.nexports; i++) {
        const GvkExport* e = &g_gvk.exports[i];
        if (i && e[-1].addr == e->addr) continue;
        set_slot(e->addr, e->orig);
    }
    if (g_gvk.proc_base) {
        for (u32 i = 0; i < sizeof debug_procs / sizeof *debug_procs; i++) {
            if (!g_gvk.proc[i]) continue;
            set_slot(g_gvk.proc_base + 4 * i, NULL);
            g_gvk.proc[i] = NULL;
        }
    }
    free(g_gvk.exports);
    g_gvk.exports = NULL;
    g_gvk.nexports = 0;
    g_gvk.driver_base = 0;
    g_gvk.driver_size = 0;
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
    u64 addr;
    const char* name;
    u32 cap = 0;
    for (u32 i = 0; kernel_symbol(i, &addr, &name); i++) cap += driver_export(driver, addr, name);
    if (!cap) return;
    g_gvk.exports = malloc(cap * sizeof *g_gvk.exports);
    if (!g_gvk.exports) fatal("gvk: out of memory for %u Vulkan exports", cap);
    u32 taken = 0, unrouted = 0;
    const char* unrouted_first = NULL;
    for (u32 i = 0; kernel_symbol(i, &addr, &name); i++) {
        if (!driver_export(driver, addr, name)) continue;
        GuestFn fn = gvk_api_find(name);
        /* a direct call the recompiler didn't route runs the driver's own code. gvk.hook routes each export of the
           loader. Another export of the driver (one SDK's vkDevExtError) has no function here, which a call through
           its slot finds stopping the game, and only its direct calls would run the driver */
        if (!hook_routed(name)) {
            if (fn && !g_gvk.unrouted_impl) g_gvk.unrouted_impl = name;
            if (!fn && !unrouted++) unrouted_first = name;
        }
        taken += fn != NULL;
        g_gvk.exports[g_gvk.nexports++] = (GvkExport){name, addr, fn, NULL};
    }
    if (unrouted)
        LOG("gvk: %u functions the driver exports beside the loader's (%s, ...) have no route: a direct call to one "
            "runs the driver's own code\n",
            unrouted, unrouted_first);
    g_gvk.driver_base = driver->base;
    g_gvk.driver_size = driver->size;
    for (u32 i = 0; i < g_gvk.nexports;) {
        const GvkExport* e = &g_gvk.exports[i];
        GuestFn fn = e->fn;
        u32 end = i + 1;
        while (end < g_gvk.nexports && g_gvk.exports[end].addr == e->addr) {
            GuestFn alias = g_gvk.exports[end++].fn;
            if (fn && alias && fn != alias) fatal("gvk: conflicting Vulkan aliases for %s", e->name);
            if (alias) fn = alias;
        }
        if (!fn) fn = missing;
        for (; i < end; i++) g_gvk.exports[i].fn = fn;
    }
    for (u32 i = 0; i < g_gvk.nexports; i++) {
        GvkExport* e = &g_gvk.exports[i];
        if (i && e[-1].addr == e->addr) continue;
        e->orig = atomic_load_explicit(&g_disp[(e->addr - g_code_lo) >> 2], memory_order_relaxed);
        set_slot(e->addr, e->fn);
        /* under NCE a function whose entry no stub reaches keeps running the driver's code for native calls, and the
           driver would get the host's handles that the other functions return: all the functions or none */
        if (nce_slot_taken(e->addr, "gvk")) {
            g_gvk.nexports = i + 1;
            release("gvk: guest_vulkan is on, but a Vulkan function of the driver can't be taken from native code: "
                    "the game's own driver runs\n");
            return;
        }
    }
    if (g_gvk.proc_base) {
        int needed = 0;
        for (u32 i = 0; i < sizeof debug_procs / sizeof *debug_procs; i++) needed |= !gvk_export(debug_procs[i]);
        if (needed) {
            u64 page = plat_vm_page();
            if (mem_map(g_gvk.proc_base, page, MS_CODE, PERM_RW))
                fatal("gvk: could not map space for private Vulkan entry points");
            for (u32 i = 0; i < sizeof debug_procs / sizeof *debug_procs; i++)
                gw32(g_gvk.proc_base + 4 * i, 0xd4200000u); /* BRK if entered without dispatch */
            if (mem_set_state(g_gvk.proc_base, page, MS_CODE, PERM_RX))
                fatal("gvk: could not protect private Vulkan entry points");
            for (u32 i = 0; i < sizeof debug_procs / sizeof *debug_procs; i++) {
                if (gvk_export(debug_procs[i])) continue;
                GuestFn fn = gvk_api_find(debug_procs[i]);
                if (!fn) continue;
                g_gvk.proc[i] = fn;
                set_slot(g_gvk.proc_base + 4 * i, fn);
            }
        }
    }
    LOG("gvk: the %u Vulkan functions of the game's driver (%s) call the host's Vulkan, %u of them implemented (the "
        "others stop the game when called)\n",
        g_gvk.nexports, driver->name ? driver->name : "?", taken);
}

void gvk_check_device(void) {
    if (!g_gvk.nexports) return;
    if (!gvk_api_supported()) {
        release("gvk: guest_vulkan is on, but the host device neither imports guest memory as host memory "
                "(VK_EXT_external_memory_host) nor exports its own as a dma-buf (VK_EXT_external_memory_dma_buf): "
                "the game's own driver runs\n");
        return;
    }
    /* after the check above, so that a host where gvk can't run falls back to the driver whatever the recompile */
    if (g_gvk.unrouted_impl)
        fatal("gvk: guest_vulkan=1 needs a Vulkan route missing from the recompiled game (%s). "
              "Recompile with --hook-direct-calls or set guest_vulkan=0",
              g_gvk.unrouted_impl);
}
