/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The FXAA of the window's picture (the anti_aliasing setting) on the GPU, through the 3DS's pass
   (gpu/vk/shaders/fxaa.comp), against its CPU reference (support/fxaa_ref.c). The shader's function is the Switch's
   presenter's too (common/gpu/vk/fxaa.glsl). A picture with aliased edges of a triangle, a circle and thin lines, over
   flat colours and a gradient: a pixel with no edge in its taps keeps its bytes, alpha too; an edge pixel is the
   reference's colour within what the device's bilinear weights (subTexelPrecisionBits) can differ by; and the pass
   smooths the edges, changing a third of the edge pixels at least. */
#include "3ds/gpu/vk/shaders/fxaa_spv.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/memory/ctr_memory.h"
#include "support/fxaa_ref.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 96u
#define HEIGHT 64u
#define BYTES (WIDTH * HEIGHT * 4u)

static void fail(const char* what) {
    fprintf(stderr, "3ds fxaa: %s\n", what);
    exit(1);
}
/* the test picture, RGBA8, rows top down */
static void picture(uint8_t* p) {
    for (unsigned y = 0; y < HEIGHT; ++y)
        for (unsigned x = 0; x < WIDTH; ++x) {
            uint8_t* px = p + (y * WIDTH + x) * 4;
            /* the left half dark grey, the right a horizontal gradient */
            unsigned grey = x < WIDTH / 2 ? 40u : 60u + (x - WIDTH / 2) * 2u;
            px[0] = px[1] = px[2] = (uint8_t)grey;
            px[3] = (uint8_t)(255u - y); /* alpha the FXAA keeps */
            /* a white triangle of shallow and steep edges */
            if (y >= 8 && y < 56 && (int)x >= 8 + (int)(56 - y) / 3 && x < 8 + (y - 8) + 4) px[0] = px[1] = px[2] = 230;
            /* a red circle over the gradient */
            float dx = (float)x - 70.5f, dy = (float)y - 30.5f;
            if (dx * dx + dy * dy < 15.0f * 15.0f) px[0] = 220, px[1] = 30, px[2] = 30;
            /* a one-pixel line, nearly horizontal */
            if (y == 58 + x / 24) px[0] = px[1] = px[2] = 250;
        }
}
static VkDeviceMemory bind_image(SnxCtrVk* v, VkImage image) {
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(v->device, image, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex =
                                   snx_ctr_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = snx_ctr_vk_memory_type(req.memoryTypeBits, 0);
    VkDeviceMemory memory = VK_NULL_HANDLE;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &memory) != VK_SUCCESS ||
        vkBindImageMemory(v->device, image, memory, 0) != VK_SUCCESS)
        fail("no memory for an image");
    return memory;
}
static VkImage image_make(SnxCtrVk* v, VkImageUsageFlags usage, VkDeviceMemory* memory, VkImageView* view) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {WIDTH, HEIGHT, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image;
    if (vkCreateImage(v->device, &ii, NULL, &image) != VK_SUCCESS) fail("no image");
    *memory = bind_image(v, image);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = VK_FORMAT_R8G8B8A8_UNORM,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    if (vkCreateImageView(v->device, &vi, NULL, view) != VK_SUCCESS) fail("no image view");
    return image;
}
static void barrier(VkCommandBuffer cb, VkImage image, VkImageLayout old, VkImageLayout next) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                              .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                              .oldLayout = old,
                              .newLayout = next,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                         NULL, 1, &b);
}
static unsigned byte_of(float v) { return (unsigned)lrintf((v < 0 ? 0 : v > 1 ? 1 : v) * 255.0f); }
static unsigned distance(const uint8_t* got, const float* want) {
    unsigned most = 0;
    for (int i = 0; i < 3; ++i) {
        unsigned w = byte_of(want[i]), d = got[i] > w ? got[i] - w : w - got[i];
        most = d > most ? d : most;
    }
    return most;
}

int main(void) {
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[256];
    if (!m) fail("no memory");
    if (!snx_ctr_vk_init(m, why, sizeof why)) {
        printf("no Vulkan device (%s): GPU FXAA checks skipped\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    SnxCtrVk* v = snx_ctr_vk();
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(v->phys, &properties);
    /* a tap between pixel centres: its weights are steps of 2^-bits, so a colour may differ by a step of the largest
       difference of its texels (255), and one more for the rounding to bytes */
    unsigned bits = properties.limits.subTexelPrecisionBits, tolerance = 1u + (255u + (1u << bits) - 1u) / (1u << bits);

    static uint8_t source[BYTES], got[BYTES], edge[WIDTH * HEIGHT];
    static float want[BYTES], other[BYTES];
    picture(source);
    fxaa_ref(source, WIDTH, HEIGHT, want, other, edge);

    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = 2 * BYTES,
                             .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer staging;
    if (vkCreateBuffer(v->device, &bi, NULL, &staging) != VK_SUCCESS) fail("no buffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, staging, &req);
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = snx_ctr_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory staging_memory;
    void* mapped = NULL;
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(v->device, &ai, NULL, &staging_memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, staging, staging_memory, 0) != VK_SUCCESS ||
        vkMapMemory(v->device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        fail("no staging memory");
    memcpy(mapped, source, BYTES);

    VkDeviceMemory from_memory, to_memory;
    VkImageView from_view, to_view;
    VkImage from =
        image_make(v, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &from_memory, &from_view);
    VkImage to = image_make(v, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &to_memory, &to_view);
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR,
                              .minFilter = VK_FILTER_LINEAR,
                              .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkSampler sampler;
    if (vkCreateSampler(v->device, &si, NULL, &sampler) != VK_SUCCESS) fail("no sampler");
    static const VkDescriptorType types[2] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    SnxCtrVkCompute pass;
    if (!snx_ctr_vk_compute_types(snx_ctr_fxaa_spv, snx_ctr_fxaa_spv_size / 4, types, 2, 0, &pass)) fail("no pipeline");

    VkCommandBuffer cb = snx_ctr_vk_commands();
    barrier(cb, from, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {WIDTH, HEIGHT, 1}};
    vkCmdCopyBufferToImage(cb, staging, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier(cb, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL); /* as present.c samples it */
    barrier(cb, to, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorSet set = snx_ctr_vk_set(pass.set);
    VkDescriptorImageInfo in = {sampler, from_view, VK_IMAGE_LAYOUT_GENERAL}, out = {VK_NULL_HANDLE, to_view,
                                                                                    VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 0,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .pImageInfo = &in},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 1,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &out}};
    vkUpdateDescriptorSets(v->device, 2, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pass.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pass.layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, (WIDTH + 7) / 8, (HEIGHT + 7) / 8, 1);
    barrier(cb, to, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    copy.bufferOffset = BYTES;
    vkCmdCopyImageToBuffer(cb, to, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
    VkMemoryBarrier host = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                            .dstAccessMask = VK_ACCESS_HOST_READ_BIT};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    if (!snx_ctr_vk_finish()) fail("the GPU's work did not finish");
    memcpy(got, (const uint8_t*)mapped + BYTES, BYTES);

    unsigned edges = 0, changed = 0, worst = 0;
    for (unsigned i = 0; i < WIDTH * HEIGHT; ++i) {
        const uint8_t* g = got + i * 4;
        const uint8_t* s = source + i * 4;
        if (g[3] != s[3]) {
            fprintf(stderr, "pixel (%u, %u): alpha %u, the picture's %u\n", i % WIDTH, i / WIDTH, g[3], s[3]);
            return 1;
        }
        if (!edge[i]) {
            if (memcmp(g, s, 4)) {
                fprintf(stderr, "pixel (%u, %u), no edge: %u %u %u, the picture's %u %u %u\n", i % WIDTH, i / WIDTH,
                        g[0], g[1], g[2], s[0], s[1], s[2]);
                return 1;
            }
            continue;
        }
        ++edges;
        changed += memcmp(g, s, 3) != 0;
        unsigned d = distance(g, want + i * 4), alternative = distance(g, other + i * 4);
        d = alternative < d ? alternative : d;
        worst = d > worst ? d : worst;
        if (d > tolerance) {
            fprintf(stderr, "pixel (%u, %u), an edge: %u %u %u, the reference's %u %u %u (%u off, %u allowed)\n",
                    i % WIDTH, i / WIDTH, g[0], g[1], g[2], byte_of(want[i * 4]), byte_of(want[i * 4 + 1]),
                    byte_of(want[i * 4 + 2]), d, tolerance);
            return 1;
        }
    }
    /* the edges of the shapes are a few hundred pixels: the pass must have found them and blurred many. A pixel beside
       a straight edge (the triangle's top, the line's runs) is blurred along the edge, over its own colour, and keeps
       it: about half of them do */
    if (edges < 200 || changed * 3 < edges) {
        fprintf(stderr, "%u edge pixels, %u of them changed: the edges were not smoothed\n", edges, changed);
        return 1;
    }
    snx_ctr_vk_compute_free(&pass);
    vkDestroySampler(v->device, sampler, NULL);
    vkDestroyImageView(v->device, from_view, NULL);
    vkDestroyImageView(v->device, to_view, NULL);
    vkDestroyImage(v->device, from, NULL);
    vkDestroyImage(v->device, to, NULL);
    vkFreeMemory(v->device, from_memory, NULL);
    vkFreeMemory(v->device, to_memory, NULL);
    vkDestroyBuffer(v->device, staging, NULL);
    vkFreeMemory(v->device, staging_memory, NULL);
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    printf("3ds fxaa: %u of %u pixels at an edge, %u of them smoothed, each within %u of the CPU reference (%u "
           "allowed, %u sub-texel bits); the rest kept\n",
           edges, WIDTH * HEIGHT, changed, worst, tolerance, bits);
    return 0;
}
