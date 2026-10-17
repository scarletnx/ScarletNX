/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Opt-in integration test: real renderer, no game data. Works with Mesa lavapipe and no display server. */
#include "common/utilities/common.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/gpu/renderer.h"
#include "nx/gpu/vk/vk.h"
#include "nx/memory/mem.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/runtime/platform/backend.h"
#include "spirv_emitter_fixture.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vulkan_headless:%d: %s\n", __LINE__, #x);                                                 \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static int check_viewport_orientation(void) {
    u32 r[0xE00] = {0};
    for (u32 mode = 0; mode <= 6; mode++) {
        r[0x64F] = mode << 11;
        CHECK(vk_render_depth_clamp(r) == (mode == 0 || mode == 2 || mode == 4 || mode == 6));
    }
    r[0x64F] = 0;
    float transform[6] = {32, 17, 0.5f, 40, 37, 0.5f};
    memcpy(&r[0x280], transform, sizeof transform);
    r[0x64B] = 1;
    r[0x286] = 0x6420; /* identity x/y/z/w swizzle */
    r[0x647] = 0x900;  /* guest clockwise */
    VkViewport vp = vk_render_viewport(r);
    CHECK(vp.x == 8 && vp.width == 64 && vp.y == 20 && vp.height == 34);
    CHECK(vp.minDepth == 0 && vp.maxDepth == 1);
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    /* NegativeY reverses the endpoints about translate_y, keeping the same viewport rectangle. */
    r[0x286] = 0x6430;
    vp = vk_render_viewport(r);
    CHECK(vp.y == 54 && vp.height == -34 && vp.y + vp.height == 20);
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    /* Negative scale and negative coordinate swizzle cancel in the viewport. */
    transform[1] = -17;
    memcpy(&r[0x280], transform, sizeof transform);
    vp = vk_render_viewport(r);
    CHECK(vp.y == 20 && vp.height == 34);
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    r[0x4EB] = 0x10;
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_COUNTER_CLOCKWISE);
    /* The origin facing flip and a negative viewport cancel. */
    transform[1] = 17;
    memcpy(&r[0x280], transform, sizeof transform);
    CHECK(vk_render_viewport(r).height == -34);
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    transform[1] = -17;
    memcpy(&r[0x280], transform, sizeof transform);
    r[0x647] = 0x901;
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    transform[1] = 17;
    memcpy(&r[0x280], transform, sizeof transform);
    r[0x286] = 0x6420;
    r[0x647] = 0x900;
    r[0x3FE] = 100u << 16;
    r[0x4EB] = 1; /* lower-left window origin flips the viewport */
    vp = vk_render_viewport(r);
    CHECK(vp.y == 120 && vp.height == -34);
    CHECK(vk_render_front_face(r) == VK_FRONT_FACE_CLOCKWISE);
    return 0;
}

static int check_packed_depth(void) {
    const u32 formats[] = {0x0D, 0x0E, 0x29, 0x2A, 0x2B, 0x30};
    for (u32 i = 0; i < sizeof formats / sizeof *formats; i++) {
        VkFmt f = vk_tic_format(formats[i], 0);
        CHECK(f.depth && f.stencil);
        const int reversed = formats[i] == 0x0E || formats[i] == 0x29;
        const int floating = formats[i] == 0x30;
        CHECK(f.bpp == (floating ? 8 : 4));
        u32 src[6] = {0}, depth[3];
        u8 stencil[3];
        const u32 z[] = {0, 0x800000, 0xffffff};
        for (u32 k = 0; k < 3; k++) {
            if (floating) {
                float v = (float)k * 0.5f;
                memcpy(&src[k * 2], &v, 4);
                src[k * 2 + 1] = 0x12340000 | (0x51 + k);
            } else
                src[k] = reversed ? (z[k] << 8) | (0x51 + k) : z[k] | ((0x51 + k) << 24);
        }
        for (int fallback = 0; fallback < 2; fallback++) {
            f.vk = fallback || floating ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
            vk_depth_unpack(&f, (const u8*)src, 3, (u8*)depth, stencil);
            for (u32 k = 0; k < 3; k++) {
                CHECK(stencil[k] == (formats[i] == 0x2A ? 0 : 0x51 + k));
                if (floating)
                    CHECK(depth[k] == src[k * 2]);
                else if (fallback) {
                    float value;
                    memcpy(&value, &depth[k], 4);
                    CHECK(value == (float)z[k] / 16777215.0f);
                } else
                    CHECK(depth[k] == z[k]);
            }
        }
    }
    return 0;
}

/* Clear separate shadow cascades through the real register path, then read each depth layer back. */
static int check_depth_array(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    u32* r = ch->r3d;
    r[0x54E] = 1;
    r[0x3F9] = 0x100000;
    r[0x3FA] = 0x0A;   /* Z32F */
    r[0x3FB] = 0x1000; /* pitch linear */
    r[0x3FC] = 1024;   /* 4096 byte layer stride */
    r[0x48A] = r[0x48B] = 16;
    r[0x48C] = 3;
    for (u32 layer = 0; layer < 3; layer++) {
        float depth = 0.25f * (layer + 1);
        memcpy(&r[0x364], &depth, 4);
        vk_render_clear(ch, 1 | (layer << 10));
    }
    vk_render_end();
    Surf* target = vk_rt_find(r[0x3F9]);
    CHECK(target && target->layers == 3 && target->layer_size == 4096);
    Surf* drawn[4];
    CHECK(vk_rt_drawn_in(target->gva, target->gva + target->layer_size * target->layers, drawn, 4) >= 1);
    CHECK(drawn[0] == target && vk_rt_drawn_fresh(target));
    VkBuffer buffer;
    u8* data = vk_readback_buffer(12, &buffer);
    CHECK(data);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copies[3] = {0};
    for (u32 layer = 0; layer < 3; layer++) {
        copies[layer].bufferOffset = layer * 4;
        copies[layer].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_DEPTH_BIT, 0, layer, 1};
        copies[layer].imageExtent = (VkExtent3D){1, 1, 1};
    }
    vkCmdCopyImageToBuffer(cb, target->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 3, copies);
    vk_finish();
    vk_readback_ready();
    for (u32 layer = 0; layer < 3; layer++) {
        float depth;
        memcpy(&depth, data + layer * 4, 4);
        CHECK(depth == 0.25f * (layer + 1));
    }
    r[0x3F9] = 0x200000;
    r[0x48C] = 3 | (1u << 16);
    vk_render_clear(ch, 1);
    vk_render_end();
    CHECK(vk_rt_find(r[0x3F9])->layers == 1);
    /* Sampling a packed-depth array must retain the rendered image, including all cascade layers. */
    r[0x3F9] = 0x300000;
    r[0x3FA] = 0x14;
    r[0x48C] = 3;
    vk_render_clear(ch, 3);
    vk_render_end();
    u32 tic[8] = {0x2B, r[0x3F9], 2u << 21, 2, 15 | (7u << 23), 15 | (2u << 16), 0, 0};
    CHECK(vk_tex_from_tic(tic) == vk_rt_find(r[0x3F9]));
    free(ch);
    return 0;
}

/* Switch sRGB writes on one image without losing its contents. Both full clears and the masked-clear draw
   must use the selected attachment format, including when it changes in an already-open pass. */
static int check_srgb_write(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    u32* r = ch->r3d;
    r[0x487] = 1;
    r[0x201] = 0x400000;
    r[0x202] = 64; /* 16 RGBA pixels, pitch linear */
    r[0x203] = 16;
    r[0x204] = 0xD6; /* A8B8G8R8 sRGB */
    r[0x205] = 0x1000;
    r[0x206] = 2;
    r[0x207] = 1024;
    float color[4] = {0.25f, 0.25f, 0.25f, 1};
    memcpy(&r[0x360], color, sizeof color);
    r[0x56E] = 1;
    vk_render_clear(ch, 15 << 2);
    r[0x56E] = 0;
    vk_render_clear(ch, (15 << 2) | (1 << 10));
    color[0] = 0.5f;
    memcpy(&r[0x360], color, sizeof color);
    r[0x56E] = 1;
    vk_render_clear(ch, (1 << 2) | (1 << 10));
    r[0x56E] = 0;
    vk_render_clear(ch, 1 << 2);
    vk_render_end();
    Surf* target = vk_rt_find(r[0x201]);
    CHECK(target && target->f.vk == VK_FORMAT_R8G8B8A8_SRGB && target->layers == 2);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(8, &buffer);
    CHECK(data);
    VkCommandBuffer cb = vk_cmd();
    vk_surf_use(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copies[2] = {0};
    for (u32 i = 0; i < 2; i++) {
        copies[i].bufferOffset = i * 4;
        copies[i].imageSubresource = (VkImageSubresourceLayers){VK_IMAGE_ASPECT_COLOR_BIT, 0, i, 1};
        copies[i].imageExtent = (VkExtent3D){1, 1, 1};
    }
    vkCmdCopyImageToBuffer(cb, target->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 2, copies);
    vk_finish();
    vk_readback_ready();
    const u8 expected[8] = {128, 137, 137, 255, 188, 64, 64, 255};
    for (u32 i = 0; i < 8; i++) CHECK(abs((int)data[i] - expected[i]) <= 1);
    free(ch);
    return 0;
}

static int check_multisample_shader_gate(void) {
    u64 guest[14] = {0};
    ShaderInfo info = {0};
    size_t words = 0;
    guest[10] = 0x001f8000fc0007e0ull; /* scheduler */
    guest[11] = 0xd000000000000000ull | 6ull << 53 | 1ull << 57 | 0x10ull << 36 | 0xffull << 28 | 4ull << 20 |
                7ull << 16 | 2ull << 8; /* TLDS.MS */
    guest[12] = 0xe30000000007000full;  /* EXIT */
    u32* spirv = vk_translate_spirv((const u8*)guest, sizeof guest, ST_FS, &info, &words, NULL, NULL);
    CHECK(!spirv && info.unsupported);
    CHECK(strstr(info.first_unsupported, "multisample texture binding unavailable"));
    free(spirv);
    guest[11] = 0xdc00000000000000ull | 7ull << 51 | 1ull << 50 | 0xfull << 31 | 2ull << 28 | 0x10ull << 36 |
                4ull << 20 | 7ull << 16 | 2ull << 8; /* bound TLD.MS */
    memset(&info, 0, sizeof info);
    words = 0;
    spirv = vk_translate_spirv((const u8*)guest, sizeof guest, ST_FS, &info, &words, NULL, NULL);
    CHECK(!spirv && info.unsupported);
    CHECK(strstr(info.first_unsupported, "multisample texture binding unavailable"));
    free(spirv);
    return 0;
}

/* Execute the direct writer's module, not only spirv-val's structural check. */
static int check_direct_spirv(void) {
    size_t words = 0;
    u32* code = snx_test_spirv_compute(&words);
    VkShaderModuleCreateInfo smi = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = words * sizeof(u32), .pCode = code};
    VkShaderModule module;
    CHECK(vkCreateShaderModule(g_vk.dev, &smi, NULL, &module) == VK_SUCCESS);
    free(code);
    VkDescriptorSetLayoutBinding binding = {.binding = 0,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .descriptorCount = 1,
                                            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
    VkDescriptorSetLayoutCreateInfo dli = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &binding};
    VkDescriptorSetLayout dlayout;
    CHECK(vkCreateDescriptorSetLayout(g_vk.dev, &dli, NULL, &dlayout) == VK_SUCCESS);
    VkPipelineLayoutCreateInfo pli = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dlayout};
    VkPipelineLayout playout;
    CHECK(vkCreatePipelineLayout(g_vk.dev, &pli, NULL, &playout) == VK_SUCCESS);
    VkComputePipelineCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                 .module = module,
                                                 .pName = "main"},
                                       .layout = playout};
    VkPipeline pipeline;
    CHECK(vkCreateComputePipelines(g_vk.dev, VK_NULL_HANDLE, 1, &pci, NULL, &pipeline) == VK_SUCCESS);
    vkDestroyShaderModule(g_vk.dev, module, NULL);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = sizeof(u32),
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT};
    VmaAllocationCreateInfo ai = {.flags =
                                      VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer;
    VmaAllocation memory;
    VmaAllocationInfo allocation;
    CHECK(vmaCreateBuffer(g_vk.vma, &bi, &ai, &buffer, &memory, &allocation) == VK_SUCCESS);
    *(u32*)allocation.pMappedData = 0;
    CHECK(vmaFlushAllocation(g_vk.vma, memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    VkDescriptorPoolSize pool_size = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &pool_size};
    VkDescriptorPool pool;
    CHECK(vkCreateDescriptorPool(g_vk.dev, &dpi, NULL, &pool) == VK_SUCCESS);
    VkDescriptorSetAllocateInfo dsi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = pool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &dlayout};
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(g_vk.dev, &dsi, &set) == VK_SUCCESS);
    VkDescriptorBufferInfo dbi = {buffer, 0, sizeof(u32)};
    VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                  .dstSet = set,
                                  .dstBinding = 0,
                                  .descriptorCount = 1,
                                  .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &dbi};
    vkUpdateDescriptorSets(g_vk.dev, 1, &write, 0, NULL);
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .queueFamilyIndex = g_vk.gfx_family};
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(g_vk.dev, &cpi, NULL, &cpool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = cpool,
                                       .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1};
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(g_vk.dev, &cbi, &cmd) == VK_SUCCESS);
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(vkBeginCommandBuffer(cmd, &begin) == VK_SUCCESS);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                                .srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                                .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                                .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    vkCmdPipelineBarrier2(cmd, &dependency);
    CHECK(vkEndCommandBuffer(cmd) == VK_SUCCESS);
    VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd};
    CHECK(vkQueueSubmit(g_vk.gfx, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS);
    CHECK(vkQueueWaitIdle(g_vk.gfx) == VK_SUCCESS);
    CHECK(vmaInvalidateAllocation(g_vk.vma, memory, 0, VK_WHOLE_SIZE) == VK_SUCCESS);
    CHECK(*(u32*)allocation.pMappedData == 42);
    vkDestroyCommandPool(g_vk.dev, cpool, NULL);
    vkDestroyDescriptorPool(g_vk.dev, pool, NULL);
    vmaDestroyBuffer(g_vk.vma, buffer, memory);
    vkDestroyPipeline(g_vk.dev, pipeline, NULL);
    vkDestroyPipelineLayout(g_vk.dev, playout, NULL);
    vkDestroyDescriptorSetLayout(g_vk.dev, dlayout, NULL);
    return 0;
}

/* VIC through the compute shader: raw YUV and alpha, odd starts kept, luma-only NV12, bytes outside untouched */
static int check_vic(void) {
    enum { W = 32, H = 16, SENTINEL = 0xA5 };
    static u8 y[W * H], uv[W * H / 2], out[W * 4 * H];
    for (u32 j = 0; j < H; ++j)
        for (u32 i = 0; i < W; ++i) y[j * W + i] = (u8)(16 + i + 4 * j);
    for (u32 j = 0; j < H / 2; ++j)
        for (u32 i = 0; i < W / 2; ++i) {
            uv[j * W + i * 2] = (u8)(40 + i + 8 * j);
            uv[j * W + i * 2 + 1] = (u8)(240 - i - 8 * j);
        }
    static const struct {
        u32 fmt, sx0, sy0, dx0, dy0, w, h;
    } cases[] = {{0x1F, 0, 0, 0, 0, W, H}, {0x1F, 3, 1, 5, 3, 10, 7}, {0x44, 0, 0, 0, 0, W, H}};
    for (u32 c = 0; c < sizeof cases / sizeof cases[0]; ++c) {
        const int rgba = cases[c].fmt != 0x44;
        const u32 bpp = rgba ? 4 : 1;
        VicJob job;
        memset(&job, 0, sizeof job);
        memset(out, SENTINEL, sizeof out);
        job.fmt = cases[c].fmt;
        job.sw = W;
        job.sh = H;
        job.oy = (VicPlane){out, (u64)W * bpp * H, W * bpp, H, 0, 0};
        VicSlot* sl = &job.slots[0];
        sl->on = 1;
        sl->sx0 = cases[c].sx0;
        sl->sy0 = cases[c].sy0;
        sl->dx0 = cases[c].dx0;
        sl->dy0 = cases[c].dy0;
        sl->w = cases[c].w;
        sl->h = cases[c].h;
        sl->alpha = 0x3FF;
        sl->clamp_hi = 0x3FF;
        sl->iy = (VicPlane){y, sizeof y, W, H, 0, 0};
        sl->ic = (VicPlane){uv, sizeof uv, W, H / 2, 0, 0};
        CHECK(g_renderer->vic_compose(&job));
        for (u32 j = 0; j < H; ++j)
            for (u32 i = 0; i < W; ++i) {
                const u8* o = out + (j * W + i) * bpp;
                const int in = i >= sl->dx0 && i < sl->dx0 + sl->w && j >= sl->dy0 && j < sl->dy0 + sl->h;
                const u32 sx = sl->sx0 + i - sl->dx0, sy = sl->sy0 + j - sl->dy0;
                if (!in) {
                    CHECK(o[0] == SENTINEL);
                } else if (!rgba) {
                    CHECK(o[0] == y[sy * W + sx]);
                } else {
                    const u8* p = uv + (sy / 2) * W + (sx & ~1u);
                    CHECK(o[0] == y[sy * W + sx] && o[1] == p[0] && o[2] == p[1] && o[3] == 255);
                }
            }
    }
    return 0;
}
int main(int argc, char** argv) {
    const u32 w = 32, h = 16;
    const u64 address = 4096;
    const i32 full[4] = {0, 0, 0, 0}, crop[4] = {4, 2, 20, 10};
    char *root = snx_temp_dir(NULL), *shot, *ppm;
    CHECK(root);
    CHECK(!snx_startup_options(argc, argv));
    snprintf(g_exedir, 1024, "%s/", root);
    shot = snx_path_join(root, "frame");
    ppm = snx_format("%s.ppm", shot);
    g_mem = calloc(1, (size_t)address + w * h * 4);
    CHECK(g_mem);
    nv_init(); /* color targets may import guest memory through the GPU page table */
    video_init("Headless Vulkan fixture");
    CHECK(g_opt_headless && g_renderer_up && g_vk.dev && !g_vk.surface && !g_vk.swapchain && !g_vk.sc_count);
    CHECK(!check_viewport_orientation());
    CHECK(!check_packed_depth());
    CHECK(!check_depth_array());
    CHECK(!check_srgb_write());
    CHECK(!check_multisample_shader_gate());
    CHECK(!check_direct_spirv());
    CHECK(!check_vic());
    CHECK(g_renderer->method_barrier);
    u32 epoch = g_vk_epoch;
    u64 finishes = g_vk_finishes;
    for (int kind = 0; kind <= 4; kind++) g_renderer->method_barrier(kind);
    CHECK(g_vk_epoch == epoch + 2);   /* data and descriptor invalidation each expire cached reads */
    CHECK(g_vk_finishes == finishes); /* method barriers order GPU work without forcing a host wait */
    printf("Headless Vulkan device: %s\n", g_vk.props.deviceName);
    /* Make any accidental WSI use during presents/recreation fail the test immediately. */
    vkCreateSwapchainKHR = NULL;
    vkAcquireNextImageKHR = NULL;
    vkQueuePresentKHR = NULL;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR = NULL;
    CHECK(!vk_swapchain_create(0));
    u32* pixels = (u32*)(g_mem + address);
    for (u32 frame = 0; frame < 8; ++frame) {
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x) pixels[y * w + x] = 0xff000000u | x | (y << 8) | (frame << 16);
        const i32* region = frame & 1 ? crop : full;
        u32 x0 = frame & 1 ? 4 : 0, y0 = frame & 1 ? 2 : 0;
        u32 width = frame & 1 ? 16 : w, height = frame & 1 ? 8 : h;
        u32 transform = frame >> 1; /* none, horizontal, vertical, both; each with a full image and a crop */
        u64 expected = 1469598103934665603ull;
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                u32 sx = x0 + ((transform & 1) ? width - 1 - x : x);
                u32 sy = y0 + ((transform & 2) ? height - 1 - y : y);
                expected = (expected ^ pixels[sy * w + sx]) * 1099511628211ull;
            }
        u64 before = vk_frame_id();
        g_renderer->present_buffer(address, w, h, w * 4, 0, 0, 1, region, transform);
        CHECK(vk_frame_id() == before + 1);
        CHECK(!g_vk.surface && !g_vk.swapchain);
        CHECK(g_renderer->screenshot(shot) == expected);
        size_t size;
        unsigned char* data = snx_read_file(ppm, &size);
        char header[64];
        int n = snprintf(header, sizeof header, "P6\n%u %u\n255\n", width, height);
        CHECK(data && size == (size_t)n + width * height * 3 && !memcmp(data, header, (size_t)n));
        for (u32 y = 0; y < height; ++y)
            for (u32 x = 0; x < width; ++x) {
                unsigned char* p = data + n + (y * width + x) * 3;
                CHECK(p[0] == x0 + ((transform & 1) ? width - 1 - x : x));
                CHECK(p[1] == y0 + ((transform & 2) ? height - 1 - y : y));
                CHECK(p[2] == frame);
            }
        free(data);
    }
    /* Even a present without an image submits and retires the current frame slot. */
    u64 before = vk_frame_id();
    g_renderer->present_buffer(0, 0, 0, 0, 0, 0, 1, full, 0);
    CHECK(vk_frame_id() == before + 1);
    vk_finish();
    free(g_mem);
    g_mem = NULL;
    CHECK(!snx_remove_tree(root));
    free(root);
    free(shot);
    free(ppm);
    puts("PASS headless Vulkan: viewport orientation, 3 VIC compositions, 8 cropped/transformed readbacks and PPMs, "
         "no WSI");
    return 0;
}
