/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native capture pass owns depth/stencil/discard. This test supplies its color/validity images, including
   source alpha0, and checks that each ordered resolve consumes only the surviving samples. */
#include "nx/gpu/shader/advanced_blend_spirv.h"
#include "support/nvn_advanced_blend_ref.h"

typedef struct {
    VkImage image;
    DevMem memory;
    VkImageView view;
    VkImageLayout layout;
    u32 layers;
} BlendResolveImage;

static int blend_resolve_image(BlendResolveImage* image, VkFormat format, u32 layers) {
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {5, 3, 1},
                              .mipLevels = 1,
                              .arrayLayers = layers,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (!vk_image_create(&info, &image->image, &image->memory)) return 0;
    image->layers = layers;
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = image->image,
                                  .viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
                                  .format = format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers}};
    return VK_OK(vk_image_view_create(g_vk.dev, &view, NULL, &image->view));
}
static void blend_resolve_use(BlendResolveImage* image, VkImageLayout layout, VkPipelineStageFlags2 stage,
                               VkAccessFlags2 access) {
    VkImageMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                      .srcStageMask = image->layout ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : 0,
                                      .srcAccessMask = image->layout ? VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT : 0,
                                      .dstStageMask = stage,
                                      .dstAccessMask = access,
                                      .oldLayout = image->layout,
                                      .newLayout = layout,
                                      .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                      .image = image->image,
                                      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image->layers}};
    VkDependencyInfo dependency = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                                    .imageMemoryBarrierCount = 1,
                                    .pImageMemoryBarriers = &barrier};
    rec_CmdPipelineBarrier2(vk_cmd(), &dependency);
    image->layout = layout;
}
static int blend_resolve_upload(BlendResolveImage* image, const void* bytes, u32 size) {
    VkBuffer buffer;
    u64 offset;
    u8* mapped = vk_stage(size, 4, &buffer, &offset);
    if (!mapped) return 0;
    memcpy(mapped, bytes, size);
    blend_resolve_use(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                       VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkBufferImageCopy copy = {.bufferOffset = offset,
                               .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, image->layers},
                               .imageExtent = {5, 3, 1}};
    rec_CmdCopyBufferToImage(vk_cmd(), buffer, image->image, image->layout, 1, &copy);
    return 1;
}

static int check_nvn_blend_resolve(void) {
    if (!g_vk.feat.shaderStorageImageReadWithoutFormat || !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("SKIP NVN late blend resolve: unformatted storage image access is unavailable");
        return 0;
    }
    VkDescriptorSetLayoutBinding bindings[3];
    for (u32 i = 0; i < 3; i++)
        bindings[i] = (VkDescriptorSetLayoutBinding){i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                                     VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo descriptor = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                   .bindingCount = 3,
                                                   .pBindings = bindings};
    VkDescriptorSetLayout descriptors;
    CHECK(VK_OK(vkCreateDescriptorSetLayout(g_vk.dev, &descriptor, NULL, &descriptors)));
    VkPipelineLayoutCreateInfo pipeline_layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                                   .setLayoutCount = 1,
                                                   .pSetLayouts = &descriptors};
    VkPipelineLayout layout;
    CHECK(VK_OK(vkCreatePipelineLayout(g_vk.dev, &pipeline_layout, NULL, &layout)));
    u32 checks = 0;
    vk_lock();
    vk_render_end();
    for (u32 shape = 0; shape < 2; shape++) {
        u32 layers = shape ? 2 : 1, pixels = layers * 15;
        BlendResolveImage images[3] = {0};
        CHECK(blend_resolve_image(&images[0], VK_FORMAT_R32G32B32A32_SFLOAT, layers));
        CHECK(blend_resolve_image(&images[1], VK_FORMAT_R8G8B8A8_UNORM, layers));
        CHECK(blend_resolve_image(&images[2], VK_FORMAT_R32G32B32A32_SFLOAT, layers));
        VkDescriptorSet set = vk_desc_alloc(descriptors);
        CHECK(set);
        VkDescriptorImageInfo image_info[3];
        VkWriteDescriptorSet writes[3];
        for (u32 i = 0; i < 3; i++) {
            image_info[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, images[i].view, VK_IMAGE_LAYOUT_GENERAL};
            writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                               .dstSet = set,
                                               .dstBinding = i,
                                               .descriptorCount = 1,
                                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                               .pImageInfo = &image_info[i]};
        }
        vkUpdateDescriptorSets(g_vk.dev, 3, writes, 0, NULL);
        for (u32 mode = 5; mode <= 50; mode++)
            for (u32 pm = 0; pm < 2; pm++)
                for (u32 overlap = 0; overlap < 3; overlap++) {
                    u64 packed = (u64)mode << 37 | (u64)pm << 46 | (u64)overlap << 44;
                    u32 mask = mode & 1 ? 15 : 5;
                    u32 flags = mask | (shape ? SH_BLEND_ARRAY : 0);
                    size_t words;
                    u32* code = shader_blend_resolve_spirv(packed, flags, &words);
                    CHECK(code);
                    VkShaderModuleCreateInfo module_info = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                                             .codeSize = words * 4,
                                                             .pCode = code};
                    VkShaderModule module;
                    CHECK(VK_OK(vkCreateShaderModule(g_vk.dev, &module_info, NULL, &module)));
                    free(code);
                    VkComputePipelineCreateInfo pipeline_info = {
                        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                  .module = module,
                                  .pName = "main"},
                        .layout = layout};
                    VkPipeline pipeline;
                    CHECK(VK_OK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline)));
                    vkDestroyShaderModule(g_vk.dev, module, NULL);
                    float source[30][4], expected[30][4];
                    u8 valid[30][4] = {0};
                    for (u32 i = 0; i < pixels; i++) {
                        expected[i][0] = 0.1875f;
                        expected[i][1] = 0.375f;
                        expected[i][2] = 0.5625f;
                        expected[i][3] = 0.75f;
                    }
                    CHECK(blend_resolve_upload(&images[2], expected, pixels * 16));
                    /* Each capture is resolved before the next. Different validity masks represent late-test and
                       discard rejection without making source alpha act as a validity marker. */
                    for (u32 primitive = 0; primitive < 3; primitive++) {
                        for (u32 i = 0; i < pixels; i++) {
                            float alpha = primitive == 1 ? 0 : 0.5f;
                            source[i][0] = pm ? alpha * 0.25f : 0.25f;
                            source[i][1] = pm ? alpha * 0.5f : 0.5f;
                            source[i][2] = pm ? alpha * 0.75f : 0.75f;
                            source[i][3] = alpha;
                            valid[i][0] = (i + primitive) % 3 ? 255 : 0;
                            if (valid[i][0]) {
                                float result[4];
                                nvn_advanced_blend_reference(mode, (int)pm, overlap, source[i], expected[i], result);
                                for (u32 c = 0; c < 4; c++)
                                    if (mask & (1u << c)) expected[i][c] = result[c];
                            }
                        }
                        CHECK(blend_resolve_upload(&images[0], source, pixels * 16));
                        CHECK(blend_resolve_upload(&images[1], valid, pixels * 4));
                        for (u32 i = 0; i < 3; i++)
                            blend_resolve_use(&images[i], VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                               VK_ACCESS_2_SHADER_STORAGE_READ_BIT |
                                                   (i == 2 ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : 0));
                        rec_CmdBindPipeline(vk_cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
                        rec_CmdBindDescriptorSets(vk_cmd(), VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
                        rec_CmdDispatch(vk_cmd(), 1, 1, layers);
                    }
                    VkBuffer readback;
                    u8* actual = vk_readback_buffer(pixels * 16, &readback);
                    CHECK(actual);
                    blend_resolve_use(&images[2], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                                       VK_ACCESS_2_TRANSFER_READ_BIT);
                    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers},
                                               .imageExtent = {5, 3, 1}};
                    rec_CmdCopyImageToBuffer(vk_cmd(), images[2].image, images[2].layout, readback, 1, &copy);
                    vk_finish();
                    vk_readback_ready();
                    for (u32 i = 0; i < pixels; i++) {
                        float got[4];
                        memcpy(got, actual + i * 16, 16);
                        for (u32 c = 0; c < 4; c++) {
                            if (!(fabsf(got[c] - expected[i][c]) < 0.00005f))
                                fprintf(stderr, "late resolve mode%u pm%u overlap%u pixel%u component%u: %g expected %g\n",
                                        mode, pm, overlap, i, c, got[c], expected[i][c]);
                            CHECK(fabsf(got[c] - expected[i][c]) < 0.00005f);
                            checks++;
                        }
                    }
                    vkDestroyPipeline(g_vk.dev, pipeline, NULL);
                    vk_render_forget_state();
                }
        for (u32 i = 0; i < 3; i++) {
            vk_image_view_destroy(g_vk.dev, images[i].view, NULL);
            vk_image_destroy(images[i].image, images[i].memory);
        }
    }
    vk_unlock();
    vkDestroyPipelineLayout(g_vk.dev, layout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, descriptors, NULL);
    printf("PASS NVN late blend resolve: all46 modes, validity, alpha0, ordered overlaps, masks and layers checked=%u\n", checks);
    return 0;
}
