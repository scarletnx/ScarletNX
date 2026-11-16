/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/gpu/vk/swapchain.h"
#include <stdlib.h>

bool snx_vk_surface_format(VkPhysicalDevice phys, VkSurfaceKHR surface, const VkFormat* wanted, uint32_t count,
                           VkSurfaceFormatKHR* out) {
    *out = (VkSurfaceFormatKHR){0};
    uint32_t n = 0;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, NULL) != VK_SUCCESS || !n) return false;
    VkSurfaceFormatKHR* formats = calloc(n, sizeof *formats);
    if (!formats) return false;
    bool found = false;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats) >= 0 && n) {
        *out = formats[0];
        for (uint32_t i = 0; i < n && !found; ++i)
            for (uint32_t w = 0; w < count && !found; ++w)
                if (formats[i].format == wanted[w] && formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                    *out = formats[i];
                    found = true;
                }
    }
    free(formats);
    return found;
}

int snx_vk_pick_format(const VkSurfaceFormatKHR* formats, uint32_t n, const VkFormat* wanted, uint32_t count,
                       VkColorSpaceKHR cs, VkSurfaceFormatKHR* out) {
    for (uint32_t i = 0; i < n; ++i)
        for (uint32_t w = 0; w < count; ++w)
            if (formats[i].format == wanted[w] && formats[i].colorSpace == cs) {
                *out = formats[i];
                return 1;
            }
    return 0;
}

bool snx_vk_surface_format_cs(VkPhysicalDevice phys, VkSurfaceKHR surface, const VkFormat* wanted, uint32_t count,
                              VkColorSpaceKHR cs, VkSurfaceFormatKHR* out) {
    uint32_t n = 0;
    int found = 0;
    VkSurfaceFormatKHR* formats;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, NULL) != VK_SUCCESS || !n) return false;
    formats = calloc(n, sizeof *formats);
    if (!formats) return false;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, formats) >= 0)
        found = snx_vk_pick_format(formats, n, wanted, count, cs, out);
    free(formats);
    return found != 0;
}

VkExtent2D snx_vk_swap_extent(const VkSurfaceCapabilitiesKHR* caps, VkExtent2D window) {
    VkExtent2D e = caps->currentExtent.width == UINT32_MAX ? window : caps->currentExtent;
    if (e.width < caps->minImageExtent.width) e.width = caps->minImageExtent.width;
    if (e.height < caps->minImageExtent.height) e.height = caps->minImageExtent.height;
    if (e.width > caps->maxImageExtent.width) e.width = caps->maxImageExtent.width;
    if (e.height > caps->maxImageExtent.height) e.height = caps->maxImageExtent.height;
    return e;
}

uint32_t snx_vk_swap_image_count(const VkSurfaceCapabilitiesKHR* caps, uint32_t extra, uint32_t most) {
    uint32_t count = caps->minImageCount + extra;
    if (caps->maxImageCount && count > caps->maxImageCount) count = caps->maxImageCount;
    /* never fewer than the surface takes, whatever most says */
    if (count > most) count = most > caps->minImageCount ? most : caps->minImageCount;
    return count;
}

void snx_vk_swapchain_free(VkDevice device, SnxVkSwapchain* sc) {
    for (uint32_t i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
        if (sc->views[i]) vkDestroyImageView(device, sc->views[i], NULL);
    if (sc->swapchain) vkDestroySwapchainKHR(device, sc->swapchain, NULL);
    *sc = (SnxVkSwapchain){0};
}

static void swap_barrier(VkCommandBuffer cb, VkImage image, VkImageLayout from, VkImageLayout to,
                         VkPipelineStageFlags2 src_stage, VkAccessFlags2 src, VkPipelineStageFlags2 dst_stage,
                         VkAccessFlags2 dst) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

void snx_vk_swapchain_begin(VkCommandBuffer cb, const SnxVkSwapchain* sc, uint32_t index) {
    /* the acquire's semaphore is waited for before the clear's writes, at the attachment's output */
    swap_barrier(cb, sc->images[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo at = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                    .imageView = sc->views[index],
                                    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                    .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                    .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                    .clearValue = {.color = {.float32 = {0, 0, 0, 1}}}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, sc->extent},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &at};
    vkCmdBeginRendering(cb, &ri);
}

void snx_vk_swapchain_end(VkCommandBuffer cb, const SnxVkSwapchain* sc, uint32_t index) {
    vkCmdEndRendering(cb);
    swap_barrier(cb, sc->images[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_NONE, 0);
}

VkPresentModeKHR snx_vk_present_mode(const VkPresentModeKHR* modes, uint32_t count, bool vsync) {
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (uint32_t i = 0; i < count && !vsync; ++i)
        if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) return modes[i];
    for (uint32_t i = 0; i < count && !vsync; ++i)
        if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = modes[i];
    return mode;
}
/* the surface's present mode for unsynced (vsync off), FIFO with vsync */
static VkPresentModeKHR present_mode(VkPhysicalDevice phys, VkSurfaceKHR surface, bool unsynced) {
    VkPresentModeKHR modes[16];
    uint32_t count = sizeof modes / sizeof *modes;
    if (!unsynced || vkGetPhysicalDeviceSurfacePresentModesKHR(phys, surface, &count, modes) < 0) count = 0;
    return snx_vk_present_mode(modes, count, !unsynced);
}
bool snx_vk_swapchain_make(VkPhysicalDevice phys, VkDevice device, const SnxVkSwapchainInfo* info,
                           SnxVkSwapchain* sc) {
    VkSwapchainKHR old = sc->swapchain;
    /* the old one's images may still be in use by work in flight */
    if (old) vkDeviceWaitIdle(device);
    VkSurfaceCapabilitiesKHR caps;
    VkSurfaceFormatKHR format = {0};
    bool ok = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, info->surface, &caps) == VK_SUCCESS;
    if (ok) {
        snx_vk_surface_format(phys, info->surface, info->formats, info->format_count, &format);
        ok = format.format != VK_FORMAT_UNDEFINED;
    }
    VkExtent2D extent = ok ? snx_vk_swap_extent(&caps, info->window) : (VkExtent2D){0, 0};
    /* Frames are drawn upright: a display held other than its own way (a phone held wide) has its compositor turn
       them, where the surface's current transform would say they come turned already. The frames are landscape in a
       landscape window, which is what the consoles' compositors show */
    VkSurfaceTransformFlagBitsKHR transform =
        ok && (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                                                                                : caps.currentTransform;
    VkPresentModeKHR mode = present_mode(phys, info->surface, info->unsynced);
    VkSwapchainKHR made = VK_NULL_HANDLE;
    if (ok && extent.width && extent.height) {
        VkSwapchainCreateInfoKHR ci = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
                                       .surface = info->surface,
                                       .minImageCount = snx_vk_swap_image_count(&caps, 1, SNX_VK_SWAP_IMAGES),
                                       .imageFormat = format.format,
                                       .imageColorSpace = format.colorSpace,
                                       .imageExtent = extent,
                                       .imageArrayLayers = 1,
                                       .imageUsage = info->usage,
                                       .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
                                       .preTransform = transform,
                                       .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
                                       .presentMode = mode,
                                       .clipped = VK_TRUE,
                                       .oldSwapchain = old};
        if (vkCreateSwapchainKHR(device, &ci, NULL, &made) != VK_SUCCESS) made = VK_NULL_HANDLE;
    }
    /* retired by the create, made or not */
    snx_vk_swapchain_free(device, sc);
    if (!made) return false;
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device, made, &count, NULL);
    if (count > SNX_VK_SWAP_IMAGES) count = SNX_VK_SWAP_IMAGES;
    bool listed = vkGetSwapchainImagesKHR(device, made, &count, sc->images) >= 0 && count;
    for (uint32_t i = 0; listed && info->views && i < count; ++i) {
        VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                    .image = sc->images[i],
                                    .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                    .format = format.format,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        VkImageView view = VK_NULL_HANDLE;
        listed = vkCreateImageView(device, &vi, NULL, &view) == VK_SUCCESS;
        sc->views[i] = listed ? view : VK_NULL_HANDLE;
    }
    if (!listed) {
        sc->swapchain = made; /* freed with the views made */
        snx_vk_swapchain_free(device, sc);
        return false;
    }
    sc->swapchain = made;
    sc->format = format;
    sc->extent = extent;
    sc->count = count;
    sc->mode = mode;
    return true;
}
