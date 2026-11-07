/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The heap API consumes create infos instead of handles. Retain their exact known chains, never opaque descriptor
   guesses. Native sampler objects share an explicitly registered custom-color index with heap descriptors.
   Hash buckets hold slot indices; snapshots hold no pointers to the caller's create infos. */
#include "nx/gpu/vk/descriptor_objects.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include "common/utilities/bits.h"
#include <stdlib.h>
#include <string.h>

#define OBJECT_BUCKETS 4096u
#define BORDER_BUCKETS 256u
#define CHAIN_LIMIT 64u
enum { OBJECT_VIEW, OBJECT_SAMPLER };
typedef struct { u32 slot, references; } BorderUse;
typedef struct {
    VkDevice device;
    u64 handle;
    u32 next, references, kind, supported, chain_bytes, border_count;
    union { VkImageViewCreateInfo view; VkSamplerCreateInfo sampler; } info;
    u8 data[]; /* aligned chain nodes with NULL links, followed by BorderUse entries */
} Object;
typedef struct { Object* object; u32 next; } ObjectSlot;
typedef struct {
    VkDevice device;
    VkClearColorValue color;
    VkFormat format;
    u32 index, references, next;
} Border;
static struct {
    ObjectSlot* objects;
    u32 object_count, object_capacity, object_free, object_bucket[OBJECT_BUCKETS];
    Border* borders;
    u32 border_count, border_capacity, border_free, border_bucket[BORDER_BUCKETS];
} g_descriptor_objects;

static u64 view_handle(VkImageView view) {
#if VK_USE_64_BIT_PTR_DEFINES
    return (u64)(uintptr_t)view;
#else
    return (u64)view;
#endif
}
static u64 sampler_handle(VkSampler sampler) {
#if VK_USE_64_BIT_PTR_DEFINES
    return (u64)(uintptr_t)sampler;
#else
    return (u64)sampler;
#endif
}
static u32 object_bucket(VkDevice device, u64 handle, u32 kind) {
    u64 hash = snx_fnv_step(SNX_FNV_BASIS, handle);
    hash = snx_fnv_step(hash, (u64)(uintptr_t)device | kind);
    return (u32)(hash ^ (hash >> 32)) & (OBJECT_BUCKETS - 1);
}
static u32 object_find(VkDevice device, u64 handle, u32 kind) {
    for (u32 slot = g_descriptor_objects.object_bucket[object_bucket(device, handle, kind)]; slot;
         slot = g_descriptor_objects.objects[slot].object->next) {
        const Object* object = g_descriptor_objects.objects[slot].object;
        if (object->device == device && object->handle == handle && object->kind == kind) return slot;
    }
    return 0;
}
static u32 object_slot(void) {
    if (g_descriptor_objects.object_free) {
        u32 slot = g_descriptor_objects.object_free;
        g_descriptor_objects.object_free = g_descriptor_objects.objects[slot].next;
        return slot;
    }
    if (g_descriptor_objects.object_count + 1 >= g_descriptor_objects.object_capacity) {
        u32 capacity = g_descriptor_objects.object_capacity ? g_descriptor_objects.object_capacity * 2 : 256;
        size_t bytes = (size_t)capacity * sizeof(ObjectSlot);
        if (capacity <= g_descriptor_objects.object_capacity || bytes / sizeof(ObjectSlot) != capacity) return 0;
        ObjectSlot* objects = realloc(g_descriptor_objects.objects, bytes);
        if (!objects) return 0;
        g_descriptor_objects.objects = objects;
        g_descriptor_objects.object_capacity = capacity;
    }
    return ++g_descriptor_objects.object_count;
}
static void object_slot_free(u32 slot) {
    g_descriptor_objects.objects[slot] = (ObjectSlot){NULL, g_descriptor_objects.object_free};
    g_descriptor_objects.object_free = slot;
}
static size_t chain_node_size(VkStructureType type, u32 kind) {
    if (kind == OBJECT_VIEW) {
        switch (type) {
            case VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO: return sizeof(VkImageViewUsageCreateInfo);
            case VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_2_CREATE_INFO_KHR: return sizeof(VkImageViewUsage2CreateInfoKHR);
            case VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT: return sizeof(VkImageViewMinLodCreateInfoEXT);
            case VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT: return sizeof(VkImageViewASTCDecodeModeEXT);
            case VK_STRUCTURE_TYPE_IMAGE_VIEW_SLICED_CREATE_INFO_EXT: return sizeof(VkImageViewSlicedCreateInfoEXT);
            case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO: return sizeof(VkSamplerYcbcrConversionInfo);
            default: return 0;
        }
    }
    switch (type) {
        case VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO: return sizeof(VkSamplerReductionModeCreateInfo);
        case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT: return sizeof(VkSamplerCustomBorderColorCreateInfoEXT);
        case VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_INDEX_CREATE_INFO_EXT: return sizeof(VkSamplerCustomBorderColorIndexCreateInfoEXT);
        case VK_STRUCTURE_TYPE_SAMPLER_BORDER_COLOR_COMPONENT_MAPPING_CREATE_INFO_EXT:
            return sizeof(VkSamplerBorderColorComponentMappingCreateInfoEXT);
        case VK_STRUCTURE_TYPE_SAMPLER_CUBIC_WEIGHTS_CREATE_INFO_QCOM: return sizeof(VkSamplerCubicWeightsCreateInfoQCOM);
        case VK_STRUCTURE_TYPE_SAMPLER_BLOCK_MATCH_WINDOW_CREATE_INFO_QCOM: return sizeof(VkSamplerBlockMatchWindowCreateInfoQCOM);
        case VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO: return sizeof(VkSamplerYcbcrConversionInfo);
        default: return 0;
    }
}
static int chain_size(const void* chain, u32 kind, u32* bytes) {
    *bytes = 0;
    u32 nodes = 0;
    for (const VkBaseInStructure* node = chain; node; node = node->pNext) {
        size_t size = chain_node_size(node->sType, kind);
        if (!size || ++nodes > CHAIN_LIMIT || size > UINT32_MAX - *bytes - 8) return 0;
        *bytes += (u32)snx_align_up(size, 8);
    }
    return 1;
}
static void chain_store(u8* destination, const void* chain, u32 kind) {
    for (const VkBaseInStructure* node = chain; node; node = node->pNext) {
        size_t size = chain_node_size(node->sType, kind);
        memcpy(destination, node, size);
        ((VkBaseOutStructure*)(void*)destination)->pNext = NULL;
        destination += snx_align_up(size, 8);
    }
}
static int chain_load(const Object* object, void* chain, size_t capacity, const void** first) {
    *first = NULL;
    if (!object->supported || object->chain_bytes > capacity ||
        (object->chain_bytes && (!chain || (uintptr_t)chain % sizeof(void*)))) return 0;
    u8* destination = chain;
    for (size_t at = 0; at < object->chain_bytes;) {
        const VkBaseInStructure* source = (const void*)(object->data + at);
        size_t size = chain_node_size(source->sType, object->kind), next = at + (size_t)snx_align_up(size, 8);
        if (!size || next > object->chain_bytes) return 0;
        /* YCbCr sampler create state is retained but vkWriteSamplerDescriptorsEXT forbids its replay (11204). */
        if (object->kind == OBJECT_SAMPLER && source->sType == VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO) return 0;
        memcpy(destination + at, source, size);
        ((VkBaseOutStructure*)(void*)(destination + at))->pNext =
            next < object->chain_bytes ? (VkBaseOutStructure*)(void*)(destination + next) : NULL;
        at = next;
    }
    if (object->chain_bytes) *first = chain;
    return 1;
}
static BorderUse* border_uses(Object* object) { return (BorderUse*)(void*)(object->data + object->chain_bytes); }
static u32 border_bucket(VkDevice device, const VkSamplerCustomBorderColorCreateInfoEXT* color) {
    u32 words[4];
    memcpy(words, &color->customBorderColor, sizeof words);
    u64 hash = snx_fnv_step(SNX_FNV_BASIS, (u64)(uintptr_t)device);
    for (u32 i = 0; i < 4; i++) hash = snx_fnv_step(hash, words[i]);
    return (u32)snx_fnv_step(hash, (u32)color->format) & (BORDER_BUCKETS - 1);
}
static VkResult border_acquire(VkDevice device, const VkSamplerCustomBorderColorCreateInfoEXT* color, u32* result) {
    if (!vkRegisterCustomBorderColorEXT || !vkUnregisterCustomBorderColorEXT) return VK_ERROR_EXTENSION_NOT_PRESENT;
    u32 bucket = border_bucket(device, color);
    for (u32 slot = g_descriptor_objects.border_bucket[bucket]; slot; slot = g_descriptor_objects.borders[slot].next) {
        Border* border = &g_descriptor_objects.borders[slot];
        if (border->device == device && border->format == color->format &&
            !memcmp(&border->color, &color->customBorderColor, sizeof border->color)) {
            if (border->references == UINT32_MAX) return VK_ERROR_OUT_OF_HOST_MEMORY;
            border->references++;
            *result = slot;
            return VK_SUCCESS;
        }
    }
    u32 slot = g_descriptor_objects.border_free;
    if (slot) g_descriptor_objects.border_free = g_descriptor_objects.borders[slot].next;
    else {
        if (g_descriptor_objects.border_count + 1 >= g_descriptor_objects.border_capacity) {
            u32 capacity = g_descriptor_objects.border_capacity ? g_descriptor_objects.border_capacity * 2 : 32;
            size_t bytes = (size_t)capacity * sizeof(Border);
            if (capacity <= g_descriptor_objects.border_capacity || bytes / sizeof(Border) != capacity)
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            Border* borders = realloc(g_descriptor_objects.borders, bytes);
            if (!borders) return VK_ERROR_OUT_OF_HOST_MEMORY;
            g_descriptor_objects.borders = borders;
            g_descriptor_objects.border_capacity = capacity;
        }
        slot = ++g_descriptor_objects.border_count;
    }
    VkSamplerCustomBorderColorCreateInfoEXT plain = *color;
    plain.pNext = NULL;
    u32 index = 0;
    VkResult status = vkRegisterCustomBorderColorEXT(device, &plain, VK_FALSE, &index);
    if (status != VK_SUCCESS) {
        g_descriptor_objects.borders[slot] = (Border){.next = g_descriptor_objects.border_free};
        g_descriptor_objects.border_free = slot;
        return status;
    }
    g_descriptor_objects.borders[slot] = (Border){device, color->customBorderColor, color->format, index, 1,
                                                   g_descriptor_objects.border_bucket[bucket]};
    g_descriptor_objects.border_bucket[bucket] = slot;
    *result = slot;
    return VK_SUCCESS;
}
static void border_release(u32 slot, u32 count) {
    Border* border = &g_descriptor_objects.borders[slot];
    border->references -= count;
    if (border->references) return;
    VkSamplerCustomBorderColorCreateInfoEXT color = {.customBorderColor = border->color, .format = border->format};
    u32* link = &g_descriptor_objects.border_bucket[border_bucket(border->device, &color)];
    while (*link != slot) link = &g_descriptor_objects.borders[*link].next;
    *link = border->next;
    vkUnregisterCustomBorderColorEXT(border->device, border->index);
    *border = (Border){.next = g_descriptor_objects.border_free};
    g_descriptor_objects.border_free = slot;
}
static Object* object_new(VkDevice device, u32 kind, const void* chain, u32 border) {
    u32 bytes = 0;
    int supported = chain_size(chain, kind, &bytes);
    if (!supported) bytes = 0;
    size_t allocation = sizeof(Object) + (size_t)bytes + (border ? sizeof(BorderUse) : 0);
    if (allocation < bytes) return NULL;
    Object* object = malloc(allocation);
    if (!object) return NULL;
    *object = (Object){.device = device, .references = 1, .kind = kind, .supported = supported,
                        .chain_bytes = bytes, .border_count = border != 0};
    if (supported) chain_store(object->data, chain, kind);
    if (border) border_uses(object)[0] = (BorderUse){border, 1};
    return object;
}
/* Drivers may return identical sampler handles for identical state. Keep every associated color registration
   until the final native object reference retires; descriptors encoded from an earlier reference may still use it. */
static int object_insert(u32 slot, Object* object) {
    u32 existing = object_find(object->device, object->handle, object->kind);
    if (existing) {
        Object* old = g_descriptor_objects.objects[existing].object;
        if (old->references == UINT32_MAX) return 0;
        if (object->border_count) {
            BorderUse use = border_uses(object)[0];
            u32 at = 0;
            while (at < old->border_count && border_uses(old)[at].slot != use.slot) at++;
            if (at == old->border_count) {
                if (old->border_count >= (SIZE_MAX - sizeof *old - old->chain_bytes) / sizeof(BorderUse)) return 0;
                Object* grown = realloc(old, sizeof *old + old->chain_bytes + (size_t)(old->border_count + 1) * sizeof(BorderUse));
                if (!grown) return 0;
                old = grown;
                g_descriptor_objects.objects[existing].object = old;
                border_uses(old)[old->border_count++] = use;
            } else border_uses(old)[at].references++;
        }
        old->references++;
        old->supported &= object->supported;
        free(object);
        object_slot_free(slot);
        return 1;
    }
    u32 bucket = object_bucket(object->device, object->handle, object->kind);
    object->next = g_descriptor_objects.object_bucket[bucket];
    g_descriptor_objects.objects[slot].object = object;
    g_descriptor_objects.object_bucket[bucket] = slot;
    return 1;
}
static void object_remove(VkDevice device, u64 handle, u32 kind) {
    u32* link = &g_descriptor_objects.object_bucket[object_bucket(device, handle, kind)];
    while (*link) {
        u32 slot = *link;
        Object* object = g_descriptor_objects.objects[slot].object;
        if (object->device != device || object->handle != handle || object->kind != kind) { link = &object->next; continue; }
        if (--object->references) return;
        *link = object->next;
        for (u32 i = 0; i < object->border_count; i++) border_release(border_uses(object)[i].slot, border_uses(object)[i].references);
        free(object);
        object_slot_free(slot);
        return;
    }
}
VkResult vk_image_view_create(VkDevice device, const VkImageViewCreateInfo* info,
                               const VkAllocationCallbacks* allocation, VkImageView* view) {
    if (!g_vk.has_descriptor_heap_support) return vkCreateImageView(device, info, allocation, view);
    Object* object = object_new(device, OBJECT_VIEW, info->pNext, 0);
    u32 slot = object ? object_slot() : 0;
    if (!slot) { free(object); *view = VK_NULL_HANDLE; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    object->info.view = *info;
    object->info.view.pNext = NULL;
    VkResult status = vkCreateImageView(device, info, allocation, view);
    if (status == VK_SUCCESS) {
        object->handle = view_handle(*view);
        if (object_insert(slot, object)) return status;
        vkDestroyImageView(device, *view, allocation);
        *view = VK_NULL_HANDLE;
        status = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    free(object);
    object_slot_free(slot);
    return status;
}
void vk_image_view_destroy(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocation) {
    vkDestroyImageView(device, view, allocation);
    if (g_vk.has_descriptor_heap_support && view) object_remove(device, view_handle(view), OBJECT_VIEW);
}
VkResult vk_sampler_create(VkDevice device, const VkSamplerCreateInfo* info,
                            const VkAllocationCallbacks* allocation, VkSampler* sampler) {
    if (!g_vk.has_descriptor_heap_support) return vkCreateSampler(device, info, allocation, sampler);
    const VkSamplerCustomBorderColorCreateInfoEXT* color = NULL;
    const VkSamplerCustomBorderColorIndexCreateInfoEXT* existing_index = NULL;
    u32 nodes = 0;
    for (const VkBaseInStructure* node = info->pNext; node && nodes++ < CHAIN_LIMIT; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT) color = (const void*)node;
        if (node->sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_INDEX_CREATE_INFO_EXT) existing_index = (const void*)node;
    }
    int custom = info->borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT || info->borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT;
    u32 border = 0;
    if (custom && color && !existing_index) {
        VkResult status = border_acquire(device, color, &border);
        if (status != VK_SUCCESS) { *sampler = VK_NULL_HANDLE; return status; }
    }
    VkSamplerCustomBorderColorIndexCreateInfoEXT index = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_INDEX_CREATE_INFO_EXT,
                                                           .pNext = info->pNext,
                                                           .index = border ? g_descriptor_objects.borders[border].index : 0};
    VkSamplerCreateInfo create = *info;
    if (border) create.pNext = &index;
    Object* object = object_new(device, OBJECT_SAMPLER, create.pNext, border);
    u32 slot = object ? object_slot() : 0;
    if (!slot) {
        free(object);
        if (border) border_release(border, 1);
        *sampler = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    object->info.sampler = create;
    object->info.sampler.pNext = NULL;
    VkResult status = vkCreateSampler(device, &create, allocation, sampler);
    if (status == VK_SUCCESS) {
        object->handle = sampler_handle(*sampler);
        if (object_insert(slot, object)) return status;
        vkDestroySampler(device, *sampler, allocation);
        *sampler = VK_NULL_HANDLE;
        status = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    free(object);
    object_slot_free(slot);
    if (border) border_release(border, 1);
    return status;
}
void vk_sampler_destroy(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* allocation) {
    vkDestroySampler(device, sampler, allocation);
    if (g_vk.has_descriptor_heap_support && sampler) object_remove(device, sampler_handle(sampler), OBJECT_SAMPLER);
}
int vk_descriptor_view_info(VkImageView view, VkImageViewCreateInfo* out, void* chain, size_t size) {
    if (!g_vk.has_descriptor_heap_support) return 0;
    u32 slot = object_find(g_vk.dev, view_handle(view), OBJECT_VIEW);
    if (!slot) return 0;
    const Object* object = g_descriptor_objects.objects[slot].object;
    const void* first;
    if (!chain_load(object, chain, size, &first)) return 0;
    *out = object->info.view;
    out->pNext = first;
    return 1;
}
int vk_descriptor_sampler_info(VkSampler sampler, VkSamplerCreateInfo* out, void* chain, size_t size) {
    if (!g_vk.has_descriptor_heap_support) return 0;
    u32 slot = object_find(g_vk.dev, sampler_handle(sampler), OBJECT_SAMPLER);
    if (!slot) return 0;
    const Object* object = g_descriptor_objects.objects[slot].object;
    const void* first;
    if (!chain_load(object, chain, size, &first)) return 0;
    *out = object->info.sampler;
    out->pNext = first;
    return 1;
}
