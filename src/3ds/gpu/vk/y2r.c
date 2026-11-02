/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The Y2R engine on the GPU: shaders/y2r.comp over the guest's RAM in place, gpu/y2r.c the CPU reference the tests
   hold it to. Its streams are virtual memory: each one is bound as the window of RAM that holds its pages, and a
   table the host writes gives each page's offset in it. */
#include "3ds/gpu/vk/vk_y2r.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include "3ds/gpu/vk/shaders/y2r_spv.h"
#include <string.h>

#define PAGE_SLOTS 16384u /* the table's entries, a part for each submission slot */
#define PAGE_PART  (PAGE_SLOTS / SNX_CTR_VK_SLOTS) /* pages the conversions of one submission may span together */

static struct {
    SnxCtrVkCompute convert;
    VkBuffer pages;
    VkDeviceMemory pages_memory;
    uint32_t* table;
    uint32_t used[SNX_CTR_VK_SLOTS];        /* entries each slot's part has taken in its recording */
    uint64_t slot_serial[SNX_CTR_VK_SLOTS]; /* the recording they are for */
    bool made, failed;
} g_y2r;

static bool made(void) {
    if (g_y2r.made || g_y2r.failed) return g_y2r.made;
    const SnxCtrVk* v = snx_ctr_vk();
    g_y2r.failed = true;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = PAGE_SLOTS * 4,
                             .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                             .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    if (!snx_ctr_vk_compute(snx_ctr_y2r_spv, snx_ctr_y2r_spv_size / 4, 5, 0, 96, &g_y2r.convert) ||
        vkCreateBuffer(v->device, &bi, NULL, &g_y2r.pages) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(v->device, g_y2r.pages, &req);
    /* the host writes the table, the GPU reads it: no readback */
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = snx_ctr_vk_memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    void* mapped = NULL;
    if (ai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(v->device, &ai, NULL, &g_y2r.pages_memory) != VK_SUCCESS ||
        vkBindBufferMemory(v->device, g_y2r.pages, g_y2r.pages_memory, 0) != VK_SUCCESS ||
        vkMapMemory(v->device, g_y2r.pages_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        return false;
    g_y2r.table = mapped;
    g_y2r.failed = false;
    g_y2r.made = true;
    return true;
}
void snx_ctr_vk_y2r_free(void) {
    const SnxCtrVk* v = snx_ctr_vk();
    snx_ctr_vk_compute_free(&g_y2r.convert);
    if (v && g_y2r.pages) vkDestroyBuffer(v->device, g_y2r.pages, NULL);
    if (v && g_y2r.pages_memory) vkFreeMemory(v->device, g_y2r.pages_memory, NULL);
    memset(&g_y2r, 0, sizeof g_y2r);
}
SnxCtrXferResult snx_ctr_vk_y2r(const SnxCtrY2rConfig* c, const SnxCtrY2rBuffer streams[SNX_CTR_Y2R_STREAMS],
                                const uint32_t* const pages[SNX_CTR_Y2R_STREAMS],
                                const uint32_t page_count[SNX_CTR_Y2R_STREAMS]) {
    if (!snx_ctr_vk() || !made()) return SNX_CTR_XFER_UNSUPPORTED;
    uint64_t total = 0;
    for (unsigned s = 0; s < SNX_CTR_Y2R_STREAMS; ++s) total += page_count[s];
    if (total > PAGE_PART || !page_count[SNX_CTR_Y2R_Y] || !page_count[SNX_CTR_Y2R_DST]) return SNX_CTR_XFER_BOUNDS;
    /* the RAM window of each stream's pages, and each page's offset in it, in the open slot's part of the table (a
       slot's part is free again with it) */
    SnxCtrVkWindow windows[SNX_CTR_Y2R_STREAMS];
    VkCommandBuffer cb = snx_ctr_vk_commands();
    unsigned part = snx_ctr_vk_slot();
    if (g_y2r.slot_serial[part] != snx_ctr_vk_serial()) g_y2r.used[part] = 0; /* a new recording's */
    if (g_y2r.used[part] + total > PAGE_PART) { /* the recording's part is full: it goes, the next has room */
        snx_ctr_vk_submit();
        cb = snx_ctr_vk_commands();
        part = snx_ctr_vk_slot();
        g_y2r.used[part] = 0;
    }
    g_y2r.slot_serial[part] = snx_ctr_vk_serial();
    uint32_t push[24] = {0}, slot = part * PAGE_PART + g_y2r.used[part];
    for (unsigned s = 0; s < SNX_CTR_Y2R_STREAMS; ++s) {
        unsigned from = page_count[s] ? s : SNX_CTR_Y2R_Y; /* an unused input reads nothing: Y's window stands in */
        uint32_t low = UINT32_MAX, high = 0, inside;
        for (uint32_t i = 0; i < page_count[from]; ++i) {
            if (pages[from][i] < low) low = pages[from][i];
            if (pages[from][i] > high) high = pages[from][i];
        }
        if (!snx_ctr_vk_window(low, (uint64_t)high - low + 4096, &windows[s], &inside)) return SNX_CTR_XFER_BOUNDS;
        push[s * 4] = slot;
        push[s * 4 + 1] = streams[s].address & 4095u;
        push[s * 4 + 2] = streams[s].transfer_unit ? streams[s].transfer_unit : 1;
        push[s * 4 + 3] = streams[s].gap;
        for (uint32_t i = 0; i < page_count[s]; ++i) g_y2r.table[slot++] = inside + (pages[s][i] - low);
    }
    g_y2r.used[part] = slot - part * PAGE_PART;
    /* the inputs' pages with the draws over them stored, the output's the GPU's until the conversion is done */
    for (unsigned s = 0; s < SNX_CTR_Y2R_STREAMS; ++s)
        for (uint32_t i = 0; i < page_count[s]; ++i) snx_ctr_vk_sync(pages[s][i], 4096, s == SNX_CTR_Y2R_DST);
    push[16] = c->input_line_width;
    push[17] = c->input_lines;
    push[18] = c->input_format | (uint32_t)c->output_format << 8 | (uint32_t)c->rotation << 16 |
               (uint32_t)c->block_alignment << 24;
    push[19] = c->alpha;
    for (unsigned i = 0; i < 4; ++i)
        push[20 + i] = (uint16_t)c->coefficients[i * 2] | (uint32_t)(uint16_t)c->coefficients[i * 2 + 1] << 16;
    snx_ctr_vk_compute_barrier(cb); /* after what the GPU wrote to the inputs */
    VkDescriptorSet set = snx_ctr_vk_set(g_y2r.convert.set);
    VkDescriptorBufferInfo infos[5];
    VkWriteDescriptorSet writes[5];
    for (unsigned i = 0; i < 5; ++i) {
        infos[i] = i < 4 ? (VkDescriptorBufferInfo){windows[i].buffer, windows[i].offset, windows[i].range}
                         : (VkDescriptorBufferInfo){g_y2r.pages, 0, VK_WHOLE_SIZE};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &infos[i]};
    }
    vkUpdateDescriptorSets(snx_ctr_vk()->device, 5, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_y2r.convert.pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_y2r.convert.layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, g_y2r.convert.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vkCmdDispatch(cb, ((uint32_t)c->input_line_width * c->input_lines + 63) / 64, 1, 1);
    snx_ctr_vk_barrier(cb);
    return SNX_CTR_XFER_OK;
}
