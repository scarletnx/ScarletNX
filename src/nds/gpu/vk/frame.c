/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 2D engines' frame on the GPU. When the board has drawn its last visible line it has 192 lines' registers for
   each engine and the video memory as the frame left it: both are copied whole into the device's buffers, and one
   compute pass (shaders/compose.comp) makes every pixel of both screens from them, over the 3D engine's picture. */
#include "nds/gpu/vk/vk.h"
#include "nds/gpu/vk/shaders/compose_spv.h"
#include <stdio.h>
#include <string.h>

static struct {
    NdsVkBuffer staging[NDS_VK_SLOTS]; /* each submission's copy of the video memory and the lines, the CPU's side */
    NdsVkBuffer vram, lines;           /* the device's */
    NdsVkBuffer readback;
    NdsVkImage frame;
    /* the display capture: its pixels as the compositor wrote them, read back into the bank DISPCAPCNT names once
       the GPU has made them. The game's CPU and the next frame both read that bank, so it is the one thing of a frame
       that comes back from the GPU */
    NdsVkImage capture;
    NdsVkBuffer capture_read;
    struct {
        bool pending;
        uint64_t serial;
        unsigned bank;
        uint32_t offset, bytes;
    } captured;
    VkDescriptorSetLayout set;
    VkPipelineLayout layout;
    VkPipeline pipeline;
    bool up;
} g_frame;

NdsVkImage* nds_vk_frame_image(void) { return &g_frame.frame; }

bool nds_vk_frame_init(char* error, size_t error_size) {
    NdsVk* v = nds_vk();
    bool ok = true;
    for (unsigned i = 0; ok && i < NDS_VK_SLOTS; ++i)
        ok = nds_vk_buffer(&g_frame.staging[i], NDS_VK_2D_BYTES + NDS_VK_LINES_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           true);
    ok = ok &&
         nds_vk_buffer(&g_frame.vram, NDS_VK_2D_BYTES,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
         nds_vk_buffer(&g_frame.lines, NDS_VK_LINES_BYTES,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) &&
         nds_vk_buffer(&g_frame.readback, NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2 * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       true) &&
         nds_vk_image(&g_frame.frame, VK_FORMAT_R8G8B8A8_UNORM, NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT * 2,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                      VK_IMAGE_ASPECT_COLOR_BIT) &&
         nds_vk_image(&g_frame.capture, VK_FORMAT_R16_UINT, NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT) &&
         nds_vk_buffer(&g_frame.capture_read, NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                       true) &&
         snx_vk_compute_layouts(v->device, 2, 3, 0, &g_frame.set, &g_frame.layout) == VK_SUCCESS &&
         snx_vk_compute_pipeline(v->device, VK_NULL_HANDLE, g_frame.layout, snx_nds_compose_spv,
                                 (size_t)snx_nds_compose_spv_size, &g_frame.pipeline) == VK_SUCCESS;
    if (!ok) {
        snprintf(error, error_size, "cannot make the 2D engines' buffers and pipeline on the Vulkan device");
        nds_vk_frame_free();
        return false;
    }
    g_frame.up = true;
    return true;
}

void nds_vk_frame_free(void) {
    NdsVk* v = nds_vk();
    if (!v) return;
    if (g_frame.pipeline) vkDestroyPipeline(v->device, g_frame.pipeline, NULL);
    if (g_frame.layout) vkDestroyPipelineLayout(v->device, g_frame.layout, NULL);
    if (g_frame.set) vkDestroyDescriptorSetLayout(v->device, g_frame.set, NULL);
    for (unsigned i = 0; i < NDS_VK_SLOTS; ++i) nds_vk_buffer_free(&g_frame.staging[i]);
    nds_vk_buffer_free(&g_frame.vram);
    nds_vk_buffer_free(&g_frame.lines);
    nds_vk_buffer_free(&g_frame.readback);
    nds_vk_buffer_free(&g_frame.capture_read);
    nds_vk_image_free(&g_frame.frame);
    nds_vk_image_free(&g_frame.capture);
    memset(&g_frame, 0, sizeof g_frame);
}

void nds_vk_capture_land(NdsBoard* board) {
    NdsVk* v = nds_vk();
    if (!v || !g_frame.captured.pending) return;
    g_frame.captured.pending = false;
    if (!snx_vk_ring_wait(&v->ring, g_frame.captured.serial, UINT64_MAX)) return;
    /* into the bank from the offset on, around its end */
    unsigned char* bank = board->mem.vram[g_frame.captured.bank].view;
    uint32_t offset = g_frame.captured.offset, bytes = g_frame.captured.bytes;
    uint32_t first = bytes < 0x20000 - offset ? bytes : 0x20000 - offset;
    memcpy(bank + offset, g_frame.capture_read.map, first);
    memcpy(bank, g_frame.capture_read.map + first, bytes - first);
}

void nds_vk_frame_compose(NdsBoard* board) {
    NdsVk* v = nds_vk();
    if (!v || !g_frame.up) return;
    nds_vk_capture_land(board); /* the last frame's capture is in memory before this frame's copy of it */
    VkCommandBuffer cb = snx_vk_ring_commands(&v->ring);
    unsigned char* stage = g_frame.staging[v->ring.slot].map;
    /* the video memory as the engines address it, each space where the shader has it */
    static const struct {
        NdsVramSpace space;
        uint32_t at;
    } spaces[] = {{NDS_SPACE_BG_A, NDS_VK_BG_A},         {NDS_SPACE_BG_B, NDS_VK_BG_B},
                  {NDS_SPACE_OBJ_A, NDS_VK_OBJ_A},       {NDS_SPACE_OBJ_B, NDS_VK_OBJ_B},
                  {NDS_SPACE_BG_EXTPAL_A, NDS_VK_BGEXT_A}, {NDS_SPACE_BG_EXTPAL_B, NDS_VK_BGEXT_B},
                  {NDS_SPACE_OBJ_EXTPAL_A, NDS_VK_OBJEXT_A}, {NDS_SPACE_OBJ_EXTPAL_B, NDS_VK_OBJEXT_B}};
    for (size_t i = 0; i < sizeof spaces / sizeof *spaces; ++i) nds_vram_copy(board, spaces[i].space, stage + spaces[i].at);
    memcpy(stage + NDS_VK_PALETTE, board->mem.palette.view, 0x800);
    memcpy(stage + NDS_VK_OAM, board->mem.oam.view, 0x800);
    /* the four banks a display mode shows as they are, A to D, when they are the ARM9's own */
    for (unsigned bank = 0; bank < 4; ++bank) {
        if ((board->vramcnt[bank] & 0x87) == 0x80)
            memcpy(stage + NDS_VK_LCDC + bank * 0x20000u, board->mem.vram[bank].view, 0x20000);
        else
            memset(stage + NDS_VK_LCDC + bank * 0x20000u, 0, 0x20000);
    }
    memcpy(stage + NDS_VK_MAIN, board->main_display, sizeof board->main_display);
    memcpy(stage + NDS_VK_2D_BYTES, board->lines, NDS_VK_LINES_BYTES);
    VkBufferCopy copies[2] = {{0, 0, NDS_VK_2D_BYTES}, {NDS_VK_2D_BYTES, 0, NDS_VK_LINES_BYTES}};
    vkCmdCopyBuffer(cb, g_frame.staging[v->ring.slot].buffer, g_frame.vram.buffer, 1, &copies[0]);
    vkCmdCopyBuffer(cb, g_frame.staging[v->ring.slot].buffer, g_frame.lines.buffer, 1, &copies[1]);
    nds_vk_buffer_barrier(cb);
    NdsVkImage* layer = nds_vk_render3d_image();
    nds_vk_layout(cb, layer, VK_IMAGE_LAYOUT_GENERAL);
    nds_vk_layout(cb, &g_frame.frame, VK_IMAGE_LAYOUT_GENERAL);
    nds_vk_layout(cb, &g_frame.capture, VK_IMAGE_LAYOUT_GENERAL);
    VkDescriptorSet set = snx_vk_ring_set(&v->ring, g_frame.set);
    VkDescriptorBufferInfo buffers[2] = {{g_frame.vram.buffer, 0, VK_WHOLE_SIZE}, {g_frame.lines.buffer, 0, VK_WHOLE_SIZE}};
    VkDescriptorImageInfo images[3] = {{VK_NULL_HANDLE, layer->view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, g_frame.frame.view, VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, g_frame.capture.view, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 0,
                                       .descriptorCount = 2,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .pBufferInfo = buffers},
                                      {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = set,
                                       .dstBinding = 2,
                                       .descriptorCount = 3,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = images}};
    vkUpdateDescriptorSets(v->device, 2, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_frame.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_frame.layout, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, NDS_SCREEN_WIDTH / 8, NDS_SCREEN_HEIGHT / 8, 2);
    /* a capture this frame: its pixels back, for the bank and the place DISPCAPCNT names, when the GPU has them */
    uint32_t cap = board->lines[0][0].words[20];
    if (cap >> 31) {
        unsigned size = cap >> 20 & 3;
        uint32_t width = size ? 256 : 128, height = size == 0 ? 128 : size == 1 ? 64 : size == 2 ? 128 : 192;
        nds_vk_layout(cb, &g_frame.capture, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {width, height, 1}};
        vkCmdCopyImageToBuffer(cb, g_frame.capture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               g_frame.capture_read.buffer, 1, &region);
        g_frame.captured.pending = true;
        g_frame.captured.serial = v->ring.submitted + 1; /* the open recording's */
        g_frame.captured.bank = cap >> 16 & 3;
        g_frame.captured.offset = (cap >> 18 & 3) * 0x8000u;
        g_frame.captured.bytes = width * height * 2;
    }
}

bool nds_vk_frame_read(unsigned char* rgba) {
    NdsVk* v = nds_vk();
    if (!v || !g_frame.up) return false;
    VkCommandBuffer cb = snx_vk_ring_commands(&v->ring);
    nds_vk_layout(cb, &g_frame.frame, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {NDS_SCREEN_WIDTH, NDS_SCREEN_HEIGHT * 2, 1}};
    vkCmdCopyImageToBuffer(cb, g_frame.frame.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_frame.readback.buffer, 1,
                           &region);
    uint64_t serial = snx_vk_ring_submit(&v->ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
    if (!snx_vk_ring_wait(&v->ring, serial, UINT64_MAX)) return false;
    memcpy(rgba, g_frame.readback.map, NDS_SCREEN_WIDTH * NDS_SCREEN_HEIGHT * 2 * 4);
    return true;
}
