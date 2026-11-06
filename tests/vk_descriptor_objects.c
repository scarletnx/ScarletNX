/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Mock Vulkan object lifetimes and exact create-state snapshots. No Vulkan loader, device or GPU is used. */
#include "nx/gpu/vk/descriptor_objects.h"
#include "nx/gpu/vk/descriptor_heaps.h"
#include <stdio.h>
#include <string.h>

VkState g_vk;
PFN_vkCreateImageView vkCreateImageView;
PFN_vkDestroyImageView vkDestroyImageView;
PFN_vkCreateSampler vkCreateSampler;
PFN_vkDestroySampler vkDestroySampler;
PFN_vkRegisterCustomBorderColorEXT vkRegisterCustomBorderColorEXT;
PFN_vkUnregisterCustomBorderColorEXT vkUnregisterCustomBorderColorEXT;

static struct {
    unsigned checks, failures, creates, destroys, registrations, unregistrations;
    u32 next_view, next_sampler, alias_sampler, palette[64], sampler_references[1024];
    VkResult create_result, register_result;
    const void* native_chain;
    u32 native_index, serial, last_destroy, last_unregister;
} g_test;
static void check(int pass, const char* what) {
    g_test.checks++;
    if (!pass && g_test.failures++ < 12) fprintf(stderr, "FAIL: %s\n", what);
}
static VKAPI_ATTR VkResult VKAPI_CALL create_view(VkDevice device, const VkImageViewCreateInfo* info,
                                                  const VkAllocationCallbacks* allocation, VkImageView* view) {
    (void)device; (void)allocation;
    g_test.creates++;
    g_test.native_chain = info->pNext;
    if (g_test.create_result != VK_SUCCESS) return g_test.create_result;
    *view = (VkImageView)(uintptr_t)g_test.next_view++;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_view(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocation) {
    (void)device; (void)view; (void)allocation;
    g_test.destroys++;
}
static VKAPI_ATTR VkResult VKAPI_CALL create_sampler(VkDevice device, const VkSamplerCreateInfo* info,
                                                     const VkAllocationCallbacks* allocation, VkSampler* sampler) {
    (void)device; (void)allocation;
    g_test.creates++;
    g_test.native_chain = info->pNext;
    const VkSamplerCustomBorderColorCreateInfoEXT* color = NULL;
    const VkSamplerCustomBorderColorIndexCreateInfoEXT* index = NULL;
    for (const VkBaseInStructure* node = info->pNext; node; node = node->pNext) {
        if (node->sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT) color = (const void*)node;
        if (node->sType == VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_INDEX_CREATE_INFO_EXT) index = (const void*)node;
    }
    if (g_vk.has_descriptor_heap_support && (info->borderColor == VK_BORDER_COLOR_FLOAT_CUSTOM_EXT ||
                                     info->borderColor == VK_BORDER_COLOR_INT_CUSTOM_EXT)) {
        check(color && index, "native custom sampler retains color state and explicit index");
        check(index && index->index < 64 && g_test.palette[index->index], "custom index is registered before native create");
        if (index) g_test.native_index = index->index;
    }
    if (g_test.create_result != VK_SUCCESS) return g_test.create_result;
    u32 handle = g_test.alias_sampler ? g_test.alias_sampler : g_test.next_sampler++;
    check(handle < 1024, "mock sampler handle fits");
    g_test.sampler_references[handle]++;
    *sampler = (VkSampler)(uintptr_t)handle;
    return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL destroy_sampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* allocation) {
    (void)device; (void)allocation;
    u32 handle = (u32)(uintptr_t)sampler;
    check(handle < 1024 && g_test.sampler_references[handle], "native sampler reference exists until destruction");
    if (handle < 1024 && g_test.sampler_references[handle]) g_test.sampler_references[handle]--;
    g_test.destroys++;
    g_test.last_destroy = ++g_test.serial;
}
static VKAPI_ATTR VkResult VKAPI_CALL register_color(VkDevice device, const VkSamplerCustomBorderColorCreateInfoEXT* color,
                                                     VkBool32 requested, u32* index) {
    (void)device;
    check(!color->pNext && !requested, "palette receives exact color state without an unrelated sampler chain");
    if (g_test.register_result != VK_SUCCESS) return g_test.register_result;
    for (u32 i = 0; i < 64; i++)
        if (!g_test.palette[i]) {
            *index = i;
            g_test.palette[i] = 1;
            g_test.registrations++;
            return VK_SUCCESS;
        }
    return VK_ERROR_TOO_MANY_OBJECTS;
}
static VKAPI_ATTR void VKAPI_CALL unregister_color(VkDevice device, u32 index) {
    (void)device;
    check(index < 64 && g_test.palette[index], "only a live registration is released");
    if (index < 64) g_test.palette[index] = 0;
    g_test.unregistrations++;
    g_test.last_unregister = ++g_test.serial;
}
static VkImageViewCreateInfo view_info(void) {
    return (VkImageViewCreateInfo){.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = (VkImage)(uintptr_t)9, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 2, 3, 4, 5}};
}
static VkSamplerCreateInfo sampler_info(void) {
    return (VkSamplerCreateInfo){.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER, .mipLodBias = .25f, .maxLod = 7,
        .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE};
}
static void test_passthrough(void) {
    g_vk.has_descriptor_heap_support = 0;
    g_vk.has_descriptor_heaps = 0;
    VkImageViewCreateInfo view_create = view_info(), out;
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    view_create.pNext = &usage;
    VkImageView view;
    check(vk_image_view_create(g_vk.dev, &view_create, NULL, &view) == VK_SUCCESS && g_test.native_chain == &usage,
          "legacy view creation is passed through unchanged");
    check(!vk_descriptor_view_info(view, &out, NULL, 0), "legacy view is not registered");
    vk_image_view_destroy(g_vk.dev, view, NULL);
    VkSamplerCreateInfo sampler_create = sampler_info(), sampler_out;
    VkSampler sampler;
    check(vk_sampler_create(g_vk.dev, &sampler_create, NULL, &sampler) == VK_SUCCESS, "legacy sampler creation passes through");
    check(!vk_descriptor_sampler_info(sampler, &sampler_out, NULL, 0), "legacy sampler is not registered");
    vk_sampler_destroy(g_vk.dev, sampler, NULL);
}
static void test_views(void) {
    g_vk.has_descriptor_heap_support = 1;
    g_vk.has_descriptor_heaps = 1;
    VkImageViewSlicedCreateInfoEXT sliced = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_SLICED_CREATE_INFO_EXT,
                                             .sliceOffset = 3, .sliceCount = 7};
    VkImageViewASTCDecodeModeEXT decode = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_ASTC_DECODE_MODE_EXT,
                                           .pNext = &sliced, .decodeMode = VK_FORMAT_R16G16B16A16_SFLOAT};
    VkImageViewMinLodCreateInfoEXT lod = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_MIN_LOD_CREATE_INFO_EXT,
                                          .pNext = &decode, .minLod = .75f};
    VkImageViewUsageCreateInfo usage = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                        .pNext = &lod, .usage = VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo create = view_info(), out;
    create.pNext = &usage;
    VkImageView view;
    check(vk_image_view_create(g_vk.dev, &create, NULL, &view) == VK_SUCCESS, "tracked view creation");
    usage.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    sliced.sliceCount = 1;
    u64 chain[128];
    check(!vk_descriptor_view_info(view, &out, chain, 1), "small replay buffer refuses the exact chain");
    check(vk_descriptor_view_info(view, &out, chain, sizeof chain), "known view chain replays into caller storage");
    const VkImageViewUsageCreateInfo* copied_usage = out.pNext;
    const VkImageViewMinLodCreateInfoEXT* copied_lod = copied_usage ? copied_usage->pNext : NULL;
    const VkImageViewASTCDecodeModeEXT* copied_decode = copied_lod ? copied_lod->pNext : NULL;
    const VkImageViewSlicedCreateInfoEXT* copied_sliced = copied_decode ? copied_decode->pNext : NULL;
    check(copied_usage && copied_usage != &usage && copied_usage->usage == VK_IMAGE_USAGE_SAMPLED_BIT &&
              copied_lod && copied_lod->minLod == .75f && copied_decode && copied_sliced &&
              copied_sliced->sliceCount == 7 && !copied_sliced->pNext && out.subresourceRange.baseArrayLayer == 4,
          "snapshot fields and links survive original storage changes");
    u32 old_handle = (u32)(uintptr_t)view;
    vk_image_view_destroy(g_vk.dev, view, NULL);
    check(!vk_descriptor_view_info(view, &out, chain, sizeof chain), "destroyed view is absent");
    g_test.next_view = old_handle;
    create.format = VK_FORMAT_R32_UINT;
    create.pNext = NULL;
    check(vk_image_view_create(g_vk.dev, &create, NULL, &view) == VK_SUCCESS &&
              vk_descriptor_view_info(view, &out, NULL, 0) && out.format == VK_FORMAT_R32_UINT && !out.pNext,
          "recycled handle gets only its new metadata");
    vk_image_view_destroy(g_vk.dev, view, NULL);
    VkBaseInStructure unknown = {.sType = VK_STRUCTURE_TYPE_MAX_ENUM};
    create.pNext = &unknown;
    check(vk_image_view_create(g_vk.dev, &create, NULL, &view) == VK_SUCCESS &&
              !vk_descriptor_view_info(view, &out, chain, sizeof chain), "unknown view chain stays native but refuses heap replay");
    vk_image_view_destroy(g_vk.dev, view, NULL);
}
static void test_custom_samplers(void) {
    VkSamplerCustomBorderColorCreateInfoEXT color = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
        .customBorderColor.uint32 = {0x3e800000, 0x3f000000, 0x3f400000, 0x3f800000}, .format = VK_FORMAT_R8G8B8A8_UNORM};
    VkSamplerReductionModeCreateInfo reduction = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
                                                    .pNext = &color, .reductionMode = VK_SAMPLER_REDUCTION_MODE_MIN};
    VkSamplerCreateInfo create = sampler_info(), out;
    create.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    create.pNext = &reduction;
    VkSampler first, second;
    unsigned registrations = g_test.registrations, unregistrations = g_test.unregistrations;
    check(vk_sampler_create(g_vk.dev, &create, NULL, &first) == VK_SUCCESS, "custom sampler creation registers before native create");
    u32 index = g_test.native_index;
    create.magFilter = VK_FILTER_NEAREST;
    check(vk_sampler_create(g_vk.dev, &create, NULL, &second) == VK_SUCCESS && g_test.registrations == registrations + 1 &&
              g_test.native_index == index, "equal raw border color and format share a registration");
    u64 chain[128];
    check(vk_descriptor_sampler_info(first, &out, chain, sizeof chain), "custom sampler heap metadata is available");
    const VkSamplerCustomBorderColorIndexCreateInfoEXT* copied_index = out.pNext;
    const VkSamplerReductionModeCreateInfo* copied_reduction = copied_index ? copied_index->pNext : NULL;
    const VkSamplerCustomBorderColorCreateInfoEXT* copied_color = copied_reduction ? copied_reduction->pNext : NULL;
    check(copied_index && copied_index->index == index && copied_reduction && copied_color &&
              !memcmp(&copied_color->customBorderColor, &color.customBorderColor, 16),
          "heap replay has the same explicit index and exact native color/reduction state");
    vk_sampler_destroy(g_vk.dev, first, NULL);
    check(g_test.unregistrations == unregistrations && !vk_descriptor_sampler_info(first, &out, chain, sizeof chain),
          "other sampler retains shared palette index after first destruction");
    vk_sampler_destroy(g_vk.dev, second, NULL);
    check(g_test.unregistrations == unregistrations + 1 && g_test.last_unregister > g_test.last_destroy,
          "final sampler unregisters only after native destruction");
    g_test.alias_sampler = 500;
    check(vk_sampler_create(g_vk.dev, &create, NULL, &first) == VK_SUCCESS &&
              vk_sampler_create(g_vk.dev, &create, NULL, &second) == VK_SUCCESS && first == second,
          "mock driver can intern identical sampler handles");
    unregistrations = g_test.unregistrations;
    vk_sampler_destroy(g_vk.dev, first, NULL);
    check(g_test.unregistrations == unregistrations && vk_descriptor_sampler_info(second, &out, chain, sizeof chain),
          "duplicate handle keeps metadata and palette until its final native reference");
    vk_sampler_destroy(g_vk.dev, second, NULL);
    check(g_test.unregistrations == unregistrations + 1 && !vk_descriptor_sampler_info(second, &out, chain, sizeof chain),
          "final duplicate reference releases metadata and palette");
    g_test.alias_sampler = 0;
    g_test.create_result = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    unregistrations = g_test.unregistrations;
    check(vk_sampler_create(g_vk.dev, &create, NULL, &first) == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
              g_test.unregistrations == unregistrations + 1, "failed native create releases its unused registration");
    g_test.create_result = VK_SUCCESS;
    g_test.register_result = VK_ERROR_TOO_MANY_OBJECTS;
    unsigned creates = g_test.creates;
    check(vk_sampler_create(g_vk.dev, &create, NULL, &first) == VK_ERROR_TOO_MANY_OBJECTS && g_test.creates == creates,
          "failed palette registration creates no native sampler");
    g_test.register_result = VK_SUCCESS;
}
static void test_unsupported_sampler(void) {
    VkSamplerYcbcrConversionInfo conversion = {.sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
                                                 .conversion = (VkSamplerYcbcrConversion)(uintptr_t)7};
    VkSamplerCreateInfo create = sampler_info(), out;
    create.pNext = &conversion;
    VkSampler sampler;
    u64 chain[128];
    check(vk_sampler_create(g_vk.dev, &create, NULL, &sampler) == VK_SUCCESS &&
              !vk_descriptor_sampler_info(sampler, &out, chain, sizeof chain),
          "YCbCr sampler remains native but refuses forbidden heap-write replay");
    vk_sampler_destroy(g_vk.dev, sampler, NULL);
}
static void test_buffer_fallback(void) {
    g_vk.has_descriptor_heaps = 0;
    VkImageViewCreateInfo view_create = view_info(), view_out;
    VkImageView view;
    check(vk_image_view_create(g_vk.dev, &view_create, NULL, &view) == VK_SUCCESS &&
              vk_descriptor_view_info(view, &view_out, NULL, 0),
          "temporary buffer fallback still registers views on a heap-capable device");
    VkSamplerCustomBorderColorCreateInfoEXT color = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT,
        .customBorderColor.uint32 = {1, 2, 3, 4}, .format = VK_FORMAT_R32_UINT};
    VkSamplerCreateInfo create = sampler_info(), out;
    create.borderColor = VK_BORDER_COLOR_INT_CUSTOM_EXT;
    create.pNext = &color;
    VkSampler sampler;
    unsigned registrations = g_test.registrations, unregistrations = g_test.unregistrations;
    u64 chain[128];
    check(vk_sampler_create(g_vk.dev, &create, NULL, &sampler) == VK_SUCCESS &&
              g_test.registrations == registrations + 1 &&
              vk_descriptor_sampler_info(sampler, &out, chain, sizeof chain),
          "temporary fallback keeps explicit custom color registration and exact replay state");
    g_vk.has_descriptor_heaps = 1;
    check(vk_descriptor_view_info(view, &view_out, NULL, 0) &&
              vk_descriptor_sampler_info(sampler, &out, chain, sizeof chain),
          "objects created during fallback remain available when heap selection resumes");
    g_vk.has_descriptor_heaps = 0;
    vk_image_view_destroy(g_vk.dev, view, NULL);
    vk_sampler_destroy(g_vk.dev, sampler, NULL);
    check(!vk_descriptor_view_info(view, &view_out, NULL, 0) &&
              !vk_descriptor_sampler_info(sampler, &out, chain, sizeof chain) &&
              g_test.unregistrations == unregistrations + 1,
          "destruction during fallback retires metadata and owned palette after the native object");
    g_vk.has_descriptor_heaps = 1;
}
int main(void) {
    vkCreateImageView = create_view;
    vkDestroyImageView = destroy_view;
    vkCreateSampler = create_sampler;
    vkDestroySampler = destroy_sampler;
    vkRegisterCustomBorderColorEXT = register_color;
    vkUnregisterCustomBorderColorEXT = unregister_color;
    g_vk.dev = (VkDevice)(uintptr_t)1;
    g_test.next_view = 100;
    g_test.next_sampler = 200;
    test_passthrough();
    test_views();
    test_custom_samplers();
    test_unsupported_sampler();
    test_buffer_fallback();
    for (u32 i = 0; i < 64; i++) check(!g_test.palette[i], "all owned palette registrations retired");
    for (u32 i = 0; i < 1024; i++) check(!g_test.sampler_references[i], "all native mock sampler references retired");
    printf("descriptor object registry mock lifecycle: %u checks, %u failures\n", g_test.checks, g_test.failures);
    return g_test.failures != 0;
}
