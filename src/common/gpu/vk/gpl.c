/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/gpl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/utilities/bits.h"

const char* snx_vk_gpl_off(int setting, bool has_ext, bool library, bool fast_link, bool independent_interpolation) {
    return !setting     ? "pipeline_libraries=0"
           : !has_ext   ? "the device has no VK_EXT_graphics_pipeline_library"
           : !library   ? "the device has VK_EXT_graphics_pipeline_library without graphicsPipelineLibrary"
           : !fast_link ? "the device links pipeline libraries without fast linking"
           : !independent_interpolation
               ? "the device has no independent interpolation decorations for pipeline libraries"
               : NULL;
}

void* snx_vk_gpl_choose(SnxVkGpl* g, SnxVkChoice* c, int setting, void* next, char* line, size_t n) {
    *g = (SnxVkGpl){0}; /* asked anew of each device choose is called for */
    const bool has_ext = snx_vk_choice_has(c, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME) &&
                         snx_vk_choice_has(c, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME);
    VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT feature = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT};
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    if (has_ext && setting) {
        VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feature};
        VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &props};
        vkGetPhysicalDeviceFeatures2(c->phys, &f2);
        vkGetPhysicalDeviceProperties2(c->phys, &p2);
    }
    const char* off =
        snx_vk_gpl_off(setting, has_ext, feature.graphicsPipelineLibrary, props.graphicsPipelineLibraryFastLinking,
                       props.graphicsPipelineLibraryIndependentInterpolationDecoration);
    if (!off && (!snx_vk_choice_enable(c, VK_KHR_PIPELINE_LIBRARY_EXTENSION_NAME) ||
                 !snx_vk_choice_enable(c, VK_EXT_GRAPHICS_PIPELINE_LIBRARY_EXTENSION_NAME)))
        off = "a setting took VK_EXT_graphics_pipeline_library or VK_KHR_pipeline_library out";
    if (off) {
        snprintf(line, n, "graphics pipeline libraries off (%s): each pipeline compiles its shaders whole", off);
        return next;
    }
    snprintf(line, n,
             "graphics pipeline libraries on: a new pipeline links the parts of its shaders, each compiled once, and "
             "is optimized in the background");
    g->on = true;
    g->feature = (VkPhysicalDeviceGraphicsPipelineLibraryFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_FEATURES_EXT,
        .pNext = next,
        .graphicsPipelineLibrary = VK_TRUE};
    return &g->feature;
}

void snx_vk_gpl_part_info(VkGraphicsPipelineCreateInfo* gi, VkGraphicsPipelineLibraryCreateInfoEXT* gl,
                          VkGraphicsPipelineLibraryFlagsEXT part, VkPipelineCreateFlags flags) {
    *gl = (VkGraphicsPipelineLibraryCreateInfoEXT){
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT, .pNext = gi->pNext, .flags = part};
    gi->pNext = gl;
    gi->flags =
        VK_PIPELINE_CREATE_LIBRARY_BIT_KHR | VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT | flags;
}

void snx_vk_gpl_link_info(VkGraphicsPipelineCreateInfo* gi, VkPipelineLibraryCreateInfoKHR* li, const VkPipeline* parts,
                          uint32_t count, bool optimize, VkPipelineCreateFlags flags, VkPipelineLayout layout,
                          const void* next) {
    *li = (VkPipelineLibraryCreateInfoKHR){.sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                           .pNext = next,
                                           .libraryCount = count,
                                           .pLibraries = parts};
    *gi = (VkGraphicsPipelineCreateInfo){.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                         .pNext = li,
                                         .flags =
                                             (optimize ? VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT : 0) | flags,
                                         .layout = layout};
}

VkGraphicsPipelineLibraryFlagsEXT snx_vk_gpl_part(unsigned i) {
    static const VkGraphicsPipelineLibraryFlagsEXT parts[SNX_VK_GPL_PARTS] = {
        VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
        VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT};
    return i < SNX_VK_GPL_PARTS ? parts[i] : 0;
}

void snx_vk_gpl_split(const VkGraphicsPipelineCreateInfo* whole, SnxVkGplSplit* out) {
    const VkGraphicsPipelineCreateInfo base = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                               .flags = whole->flags,
                                               .pDynamicState = whole->pDynamicState};
    for (unsigned i = 0; i < SNX_VK_GPL_PARTS; ++i) out->info[i] = base;
    VkGraphicsPipelineCreateInfo *vi = &out->info[0], *pr = &out->info[1], *fs = &out->info[2], *fo = &out->info[3];
    vi->pVertexInputState = whole->pVertexInputState;
    vi->pInputAssemblyState = whole->pInputAssemblyState;
    /* the stages before the fragment one, and that one, which a request lists last */
    uint32_t first_fragment = whole->stageCount;
    for (uint32_t s = 0; s < whole->stageCount; ++s)
        if (whole->pStages[s].stage == VK_SHADER_STAGE_FRAGMENT_BIT) first_fragment = s;
    pr->pNext = whole->pNext;
    pr->stageCount = first_fragment < whole->stageCount ? first_fragment : whole->stageCount;
    pr->pStages = whole->pStages;
    pr->pViewportState = whole->pViewportState;
    pr->pRasterizationState = whole->pRasterizationState;
    pr->pTessellationState = whole->pTessellationState;
    pr->layout = whole->layout;
    fs->pNext = whole->pNext;
    fs->stageCount = first_fragment < whole->stageCount ? 1 : 0;
    fs->pStages = first_fragment < whole->stageCount ? &whole->pStages[first_fragment] : NULL;
    fs->pDepthStencilState = whole->pDepthStencilState;
    fs->pMultisampleState = whole->pMultisampleState;
    fs->layout = whole->layout;
    fo->pNext = whole->pNext;
    fo->pColorBlendState = whole->pColorBlendState;
    fo->pMultisampleState = whole->pMultisampleState;
}

/* ---- the parts made so far ---- */

#define PART_BYTES 4096 /* the most a part's state writes out; a larger one is made every time */
struct SnxVkGplPart {
    uint64_t hash;
    size_t n;
    uint8_t* state;
    VkPipeline part;
};
/* a part's state written out, value by value */
typedef struct {
    uint8_t bytes[PART_BYTES];
    size_t n;
    bool full;
} State;
static void put(State* s, const void* p, size_t n) {
    if (s->full || n > PART_BYTES - s->n) {
        s->full = true;
        return;
    }
    if (n) memcpy(s->bytes + s->n, p, n);
    s->n += n;
}
#define PUT(s, v)                                                                                                      \
    do {                                                                                                               \
        const typeof(v) put_v = (v);                                                                                   \
        put(s, &put_v, sizeof put_v);                                                                                  \
    } while (0)
static void put_string(State* s, const char* text) {
    const size_t n = text ? strlen(text) : 0;
    PUT(s, (uint32_t)n);
    put(s, text, n);
}
/* the state of part i of the split, which the four's requests hold: what a driver makes it from */
static void write_part(State* s, const VkGraphicsPipelineCreateInfo* gi, unsigned i) {
    PUT(s, i);
    PUT(s, (uint32_t)gi->flags);
    PUT(s, (uint64_t)(uintptr_t)gi->layout);
    PUT(s, gi->stageCount);
    for (uint32_t k = 0; k < gi->stageCount; ++k) {
        const VkPipelineShaderStageCreateInfo* st = &gi->pStages[k];
        PUT(s, (uint32_t)st->stage);
        PUT(s, (uint64_t)(uintptr_t)st->module);
        put_string(s, st->pName);
        const VkSpecializationInfo* sp = st->pSpecializationInfo;
        PUT(s, sp ? sp->mapEntryCount : 0u);
        if (sp) {
            put(s, sp->pMapEntries, sp->mapEntryCount * sizeof *sp->pMapEntries);
            PUT(s, (uint64_t)sp->dataSize);
            put(s, sp->pData, sp->dataSize);
        }
    }
    const VkPipelineVertexInputStateCreateInfo* vi = gi->pVertexInputState;
    PUT(s, vi != NULL);
    if (vi) {
        PUT(s, vi->vertexBindingDescriptionCount);
        put(s, vi->pVertexBindingDescriptions,
            vi->vertexBindingDescriptionCount * sizeof *vi->pVertexBindingDescriptions);
        PUT(s, vi->vertexAttributeDescriptionCount);
        put(s, vi->pVertexAttributeDescriptions,
            vi->vertexAttributeDescriptionCount * sizeof *vi->pVertexAttributeDescriptions);
    }
    const VkPipelineInputAssemblyStateCreateInfo* ia = gi->pInputAssemblyState;
    PUT(s, ia != NULL);
    if (ia) {
        PUT(s, (uint32_t)ia->topology);
        PUT(s, (uint32_t)ia->primitiveRestartEnable);
    }
    const VkPipelineTessellationStateCreateInfo* ts = gi->pTessellationState;
    PUT(s, ts ? ts->patchControlPoints : 0u);
    const VkPipelineViewportStateCreateInfo* vp = gi->pViewportState;
    PUT(s, vp != NULL);
    if (vp) {
        PUT(s, vp->viewportCount);
        PUT(s, vp->scissorCount);
        PUT(s, vp->pViewports != NULL);
        if (vp->pViewports) put(s, vp->pViewports, vp->viewportCount * sizeof *vp->pViewports);
        PUT(s, vp->pScissors != NULL);
        if (vp->pScissors) put(s, vp->pScissors, vp->scissorCount * sizeof *vp->pScissors);
    }
    const VkPipelineRasterizationStateCreateInfo* rs = gi->pRasterizationState;
    PUT(s, rs != NULL);
    if (rs) {
        PUT(s, (uint32_t)rs->depthClampEnable);
        PUT(s, (uint32_t)rs->rasterizerDiscardEnable);
        PUT(s, (uint32_t)rs->polygonMode);
        PUT(s, (uint32_t)rs->cullMode);
        PUT(s, (uint32_t)rs->frontFace);
        PUT(s, (uint32_t)rs->depthBiasEnable);
        PUT(s, rs->depthBiasConstantFactor);
        PUT(s, rs->depthBiasClamp);
        PUT(s, rs->depthBiasSlopeFactor);
        PUT(s, rs->lineWidth);
    }
    const VkPipelineMultisampleStateCreateInfo* ms = gi->pMultisampleState;
    PUT(s, ms != NULL);
    if (ms) {
        PUT(s, (uint32_t)ms->rasterizationSamples);
        PUT(s, (uint32_t)ms->sampleShadingEnable);
        PUT(s, ms->minSampleShading);
        PUT(s, ms->pSampleMask ? *ms->pSampleMask : ~0u);
        PUT(s, (uint32_t)ms->alphaToCoverageEnable);
        PUT(s, (uint32_t)ms->alphaToOneEnable);
    }
    const VkPipelineDepthStencilStateCreateInfo* ds = gi->pDepthStencilState;
    PUT(s, ds != NULL);
    if (ds) {
        PUT(s, (uint32_t)ds->depthTestEnable);
        PUT(s, (uint32_t)ds->depthWriteEnable);
        PUT(s, (uint32_t)ds->depthCompareOp);
        PUT(s, (uint32_t)ds->depthBoundsTestEnable);
        PUT(s, (uint32_t)ds->stencilTestEnable);
        PUT(s, ds->front);
        PUT(s, ds->back);
        PUT(s, ds->minDepthBounds);
        PUT(s, ds->maxDepthBounds);
    }
    const VkPipelineColorBlendStateCreateInfo* cb = gi->pColorBlendState;
    PUT(s, cb != NULL);
    if (cb) {
        PUT(s, (uint32_t)cb->flags);
        PUT(s, (uint32_t)cb->logicOpEnable);
        PUT(s, (uint32_t)cb->logicOp);
        PUT(s, cb->attachmentCount);
        put(s, cb->pAttachments, cb->attachmentCount * sizeof *cb->pAttachments);
        put(s, cb->blendConstants, sizeof cb->blendConstants);
    }
    const VkPipelineDynamicStateCreateInfo* dy = gi->pDynamicState;
    PUT(s, dy ? dy->dynamicStateCount : 0u);
    if (dy) put(s, dy->pDynamicStates, dy->dynamicStateCount * sizeof *dy->pDynamicStates);
    /* the chain: the rendering's view mask and formats; any other structure by its type alone */
    for (const VkBaseInStructure* b = gi->pNext; b; b = b->pNext) {
        PUT(s, (int32_t)b->sType);
        if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            const VkPipelineRenderingCreateInfo* r = (const VkPipelineRenderingCreateInfo*)b;
            PUT(s, r->viewMask);
            PUT(s, r->colorAttachmentCount);
            put(s, r->pColorAttachmentFormats, r->colorAttachmentCount * sizeof *r->pColorAttachmentFormats);
            PUT(s, (int32_t)r->depthAttachmentFormat);
            PUT(s, (int32_t)r->stencilAttachmentFormat);
        }
    }
}
/* the state written out of the part being made on this thread */
static thread_local State t_gpl;
/* part i of whole's split, made where parts has it not: kept in parts either way, which destroys it at its end */
static VkResult part_get(SnxVkGplParts* parts, VkDevice device, VkPipelineCache cache, VkGraphicsPipelineCreateInfo* gi,
                         unsigned i, VkPipeline* out) {
    State* s = &t_gpl;
    s->n = 0, s->full = false;
    write_part(s, gi, i);
    const uint64_t hash = s->full ? 0 : snx_fnv1a(SNX_FNV_BASIS, s->bytes, s->n);
    SDL_LockMutex(parts->lock);
    for (size_t k = 0; !s->full && k < parts->count; ++k)
        if (parts->made[k].state && parts->made[k].hash == hash && parts->made[k].n == s->n &&
            !memcmp(parts->made[k].state, s->bytes, s->n)) {
            *out = parts->made[k].part;
            ++parts->reused;
            SDL_UnlockMutex(parts->lock);
            return VK_SUCCESS;
        }
    SDL_UnlockMutex(parts->lock);
    VkGraphicsPipelineLibraryCreateInfoEXT gl;
    snx_vk_gpl_part_info(gi, &gl, snx_vk_gpl_part(i), 0);
    VkResult r = vkCreateGraphicsPipelines(device, cache, 1, gi, NULL, out);
    if (r != VK_SUCCESS) return r;
    /* another thread may have made the same part meanwhile: either serves, and both are kept */
    uint8_t* state = s->full ? NULL : malloc(s->n);
    if (state) memcpy(state, s->bytes, s->n);
    SDL_LockMutex(parts->lock);
    if (parts->count == parts->room) {
        const size_t room = parts->room ? parts->room * 2 : 64;
        struct SnxVkGplPart* grown = realloc(parts->made, room * sizeof *grown);
        if (grown) parts->made = grown, parts->room = room;
    }
    const bool kept = parts->count < parts->room;
    if (kept) parts->made[parts->count++] = (struct SnxVkGplPart){hash, state ? s->n : 0, state, *out};
    SDL_UnlockMutex(parts->lock);
    if (kept) return VK_SUCCESS;
    free(state);
    vkDestroyPipeline(device, *out, NULL);
    *out = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

VkResult snx_vk_gpl_make(SnxVkGplParts* parts, VkDevice device, VkPipelineCache cache,
                         const VkGraphicsPipelineCreateInfo* whole, bool optimize, VkPipeline linked[SNX_VK_GPL_PARTS],
                         VkPipeline* out) {
    *out = VK_NULL_HANDLE;
    if (!parts->lock && !(parts->lock = SDL_CreateMutex())) return VK_ERROR_OUT_OF_HOST_MEMORY;
    SnxVkGplSplit split;
    snx_vk_gpl_split(whole, &split);
    for (unsigned i = 0; i < SNX_VK_GPL_PARTS; ++i) {
        linked[i] = VK_NULL_HANDLE;
        VkResult r = part_get(parts, device, cache, &split.info[i], i, &linked[i]);
        if (r != VK_SUCCESS) return r;
    }
    VkPipelineLibraryCreateInfoKHR li;
    VkGraphicsPipelineCreateInfo gi;
    snx_vk_gpl_link_info(&gi, &li, linked, SNX_VK_GPL_PARTS, optimize, 0, whole->layout, NULL);
    return vkCreateGraphicsPipelines(device, optimize ? cache : VK_NULL_HANDLE, 1, &gi, NULL, out);
}

VkResult snx_vk_gpl_optimize(VkDevice device, VkPipelineCache cache, const VkPipeline linked[SNX_VK_GPL_PARTS],
                             VkPipelineLayout layout, VkPipeline* out) {
    VkPipelineLibraryCreateInfoKHR li;
    VkGraphicsPipelineCreateInfo gi;
    snx_vk_gpl_link_info(&gi, &li, linked, SNX_VK_GPL_PARTS, true, 0, layout, NULL);
    *out = VK_NULL_HANDLE;
    return vkCreateGraphicsPipelines(device, cache, 1, &gi, NULL, out);
}

void snx_vk_gpl_parts_free(SnxVkGplParts* parts, VkDevice device) {
    for (size_t k = 0; k < parts->count; ++k) {
        vkDestroyPipeline(device, parts->made[k].part, NULL);
        free(parts->made[k].state);
    }
    free(parts->made);
    if (parts->lock) SDL_DestroyMutex(parts->lock);
    *parts = (SnxVkGplParts){0};
}
