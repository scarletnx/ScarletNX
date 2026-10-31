/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The transfer engine on the GPU: memory fills, display transfers and texture copies as compute shaders over the
   guest's RAM in place (the shaders folder's .comp files), with no wait: the memory they write is the GPU's until
   their submission is done (snx_ctr_vk_sync). gpu/transfer.c is their CPU reference, which the tests hold them to; its
   span functions bound what each dispatch may reach, checked here before it is recorded. */
#include "3ds/gpu/vk/vk_transfer.h"
#include "3ds/gpu/vk/vk_render.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/shaders/display_transfer_spv.h"
#include "3ds/gpu/vk/shaders/fill_spv.h"
#include "3ds/gpu/vk/shaders/texture_copy_spv.h"
#include <string.h>

static struct {
    SnxCtrVkCompute fill, display, copy;
    bool made, failed;
} g_transfer;

static bool pipelines(void) {
    if (g_transfer.made || g_transfer.failed) return g_transfer.made;
    g_transfer.failed = !snx_ctr_vk_compute(snx_ctr_fill_spv, snx_ctr_fill_spv_size / 4, 1, 0, 16, &g_transfer.fill) ||
                        !snx_ctr_vk_compute(snx_ctr_display_transfer_spv, snx_ctr_display_transfer_spv_size / 4, 2, 0,
                                            40, &g_transfer.display) ||
                        !snx_ctr_vk_compute(snx_ctr_texture_copy_spv, snx_ctr_texture_copy_spv_size / 4, 2, 0, 28,
                                            &g_transfer.copy);
    g_transfer.made = !g_transfer.failed;
    return g_transfer.made;
}
void snx_ctr_vk_transfer_free(void) {
    snx_ctr_vk_compute_free(&g_transfer.fill);
    snx_ctr_vk_compute_free(&g_transfer.display);
    snx_ctr_vk_compute_free(&g_transfer.copy);
    memset(&g_transfer, 0, sizeof g_transfer);
}

static void bind(VkCommandBuffer cb, const SnxCtrVkCompute* c, const SnxCtrVkWindow* windows, unsigned count,
                 const void* push, uint32_t push_size) {
    VkDescriptorSet set = snx_ctr_vk_set(c->set);
    VkDescriptorBufferInfo infos[2];
    VkWriteDescriptorSet writes[2];
    for (unsigned i = 0; i < count; ++i) {
        infos[i] = (VkDescriptorBufferInfo){windows[i].buffer, windows[i].offset, windows[i].range};
        writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = set,
                                           .dstBinding = i,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                           .pBufferInfo = &infos[i]};
    }
    vkUpdateDescriptorSets(snx_ctr_vk()->device, count, writes, 0, NULL);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->layout, 0, 1, &set, 0, NULL);
    vkCmdPushConstants(cb, c->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size, push);
}

SnxCtrXferResult snx_ctr_vk_fill(uint32_t pa, uint32_t size, uint32_t value, uint32_t control) {
    SnxCtrVkWindow w;
    uint32_t inside;
    if (!size) return SNX_CTR_XFER_EMPTY;
    if (!snx_ctr_vk() || !pipelines()) return SNX_CTR_XFER_UNSUPPORTED;
    if (!snx_ctr_vk_window(pa, size, &w, &inside)) return SNX_CTR_XFER_BOUNDS;
    uint32_t width = control & SNX_CTR_FILL_24BIT ? 3 : control & SNX_CTR_FILL_32BIT ? 4 : 2;
    uint32_t push[4] = {inside, size, value, width};
    snx_ctr_vk_sync(pa, size, true); /* what the GPU kept of it stored or let go of, then written */
    VkCommandBuffer cb = snx_ctr_vk_commands();
    snx_ctr_vk_compute_barrier(cb);
    bind(cb, &g_transfer.fill, &w, 1, push, sizeof push);
    vkCmdDispatch(cb, (size + 63) / 64, 1, 1);
    return SNX_CTR_XFER_OK;
}

SnxCtrXferResult snx_ctr_vk_display_transfer(const SnxCtrDisplayTransfer* t, uint32_t input, uint32_t output) {
    size_t in_bytes = 0, out_bytes = 0;
    if (!snx_ctr_display_spans(t, &in_bytes, &out_bytes)) /* empty, or a mode the reference has not either */
        return snx_ctr_display_transfer(t, NULL, 0, NULL, 0);
    if (!snx_ctr_vk() || !pipelines()) return SNX_CTR_XFER_UNSUPPORTED;
    SnxCtrVkWindow w[2];
    uint32_t in_at, out_at;
    if (!snx_ctr_vk_window(input, in_bytes, &w[0], &in_at) || !snx_ctr_vk_window(output, out_bytes, &w[1], &out_at))
        return SNX_CTR_XFER_BOUNDS;
    uint32_t out_w = t->output_size & 0xffff, out_h = t->output_size >> 16, in_w = t->input_size & 0xffff;
    uint32_t flags = t->flags, scaling = (flags >> SNX_CTR_XFER_SCALE_SHIFT) & 3;
    uint32_t in_fmt = (flags >> SNX_CTR_XFER_IN_FORMAT) & 7, out_fmt = (flags >> SNX_CTR_XFER_OUT_FORMAT) & 7;
    uint32_t skew = 0;
    if ((flags & SNX_CTR_XFER_FLIP) && (flags & SNX_CTR_XFER_CROP))
        skew = (in_w - out_w) * (out_h - 1) * snx_ctr_pixel_bytes(out_fmt); /* the spans refused a negative one */
    uint32_t push[10] = {in_at,
                         out_at,
                         in_w,
                         out_w >> (scaling != 0),
                         out_h >> (scaling == 2),
                         in_fmt,
                         out_fmt,
                         scaling,
                         (flags & SNX_CTR_XFER_FLIP ? 1u : 0u) | (flags & SNX_CTR_XFER_LINEAR ? 2u : 0u) |
                             (flags & SNX_CTR_XFER_NO_SWIZZLE ? 4u : 0u),
                         skew};
    snx_ctr_vk_sync(input, (uint32_t)in_bytes, false); /* the draws over its input stored first */
    snx_ctr_vk_sync(output, (uint32_t)out_bytes, true);
    VkCommandBuffer cb = snx_ctr_vk_commands();
    snx_ctr_vk_compute_barrier(cb);
    bind(cb, &g_transfer.display, w, 2, push, sizeof push);
    vkCmdDispatch(cb, (push[3] + 7) / 8, (push[4] + 7) / 8, 1);
    return SNX_CTR_XFER_OK;
}

SnxCtrXferResult snx_ctr_vk_texture_copy(const SnxCtrTextureCopy* t, uint32_t input, uint32_t output) {
    size_t in_bytes = 0, out_bytes = 0;
    if (!snx_ctr_texture_spans(t, &in_bytes, &out_bytes)) return snx_ctr_texture_copy(t, NULL, 0, NULL, 0);
    if (!snx_ctr_vk() || !pipelines()) return SNX_CTR_XFER_UNSUPPORTED;
    SnxCtrVkWindow w[2];
    uint32_t in_at, out_at;
    if (!snx_ctr_vk_window(input, in_bytes, &w[0], &in_at) || !snx_ctr_vk_window(output, out_bytes, &w[1], &out_at))
        return SNX_CTR_XFER_BOUNDS;
    uint32_t size = t->size & ~15u, in_gap = (t->input_line >> 16) * 16, out_gap = (t->output_line >> 16) * 16;
    uint32_t push[7] = {in_at,
                        out_at,
                        size / 16,
                        in_gap ? (t->input_line & 0xffff) * 16 : size,
                        in_gap,
                        out_gap ? (t->output_line & 0xffff) * 16 : size,
                        out_gap};
    snx_ctr_vk_sync(input, (uint32_t)in_bytes, false);
    snx_ctr_vk_sync(output, (uint32_t)out_bytes, true);
    VkCommandBuffer cb = snx_ctr_vk_commands();
    snx_ctr_vk_compute_barrier(cb);
    bind(cb, &g_transfer.copy, w, 2, push, sizeof push);
    vkCmdDispatch(cb, (push[2] + 63) / 64, 1, 1);
    return SNX_CTR_XFER_OK;
}
