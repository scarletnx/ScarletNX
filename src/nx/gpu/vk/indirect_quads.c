/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Indirect draws of quads: their records stay on the GPU, where a compute pass makes them the indexed records of the
   triangles (indirect_quads.comp), and for quads of indices makes those too. */
#include "nx/gpu/vk/indirect_quads_spv.h"
#include "nx/gpu/vk/vk.h"

static struct {
    VkDescriptorSetLayout descriptors;
    VkPipelineLayout layout;
    VkPipeline pipeline;
} g_indirect_quads;

static void quads_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 source, VkAccessFlags2 source_access,
                          VkPipelineStageFlags2 destination, VkAccessFlags2 destination_access) {
    VkMemoryBarrier2 barrier = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                .srcStageMask = source,
                                .srcAccessMask = source_access,
                                .dstStageMask = destination,
                                .dstAccessMask = destination_access};
    VkDependencyInfo dependency = {
        .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &barrier};
    rec_CmdPipelineBarrier2(cb, &dependency);
}

static int quads_init(void) {
    if (g_indirect_quads.pipeline) return 1;
    /* both layouts or neither: a pipeline that could not be made is tried again with the ones it has */
    if (!g_indirect_quads.layout &&
        !VK_OK(snx_vk_compute_layouts(g_vk.dev, 3, 0, 40, &g_indirect_quads.descriptors, &g_indirect_quads.layout)))
        return 0;
    return VK_OK(snx_vk_compute_pipeline(g_vk.dev, vk_pipeline_cache(), g_indirect_quads.layout, indirect_quads_spv,
                                         indirect_quads_spv_size, &g_indirect_quads.pipeline));
}

int vk_indirect_quads(VkBuffer record, u64 record_at, u32 stride, u32 records, u32 size, VkBuffer indices,
                      u64 indices_at, u32 limit, int last, VkBuffer* arguments, u64* arguments_at, u64* made_at) {
    u64 alignment = g_vk.props.limits.minStorageBufferOffsetAlignment;
    if (alignment < 4) alignment = 4;
    const u32 record_bytes = size ? 20 : 16, quads = size ? (limit + 3) / 4 : 0;
    if ((record_at & 3) || (stride & 3) || stride < record_bytes || !records || (size && (indices_at & (size - 1))) ||
        !quads_init())
        return 0;
    /* the records of the triangles, then from a multiple of 32 bytes the four tables of the indices made */
    const u64 made = ((u64)records * 20 + 31) & ~31ull, bytes = made + (u64)quads * 4 * 24;
    const u64 read = (u64)(records - 1) * stride + record_bytes,
              work = records > (u64)quads * 4 ? records : (u64)quads * 4;
    if (made / 4 + (u64)quads * 24 > 0xFFFFFFFFu) return 0; /* past the words a 32-bit index reaches */
    if (!vk_stage_output(bytes, alignment, arguments, arguments_at, NULL)) return 0;
    VkDescriptorSet set = vk_desc_alloc(g_indirect_quads.descriptors);
    if (!set) return 0;
    const u64 record_base = record_at / alignment * alignment, indices_base = indices_at / alignment * alignment;
    /* with no indices the records bind in their place: the program reads none */
    VkDescriptorBufferInfo buffers[3] = {
        {record, record_base, record_at - record_base + read},
        {size ? indices : record, size ? indices_base : record_base,
         size ? (indices_at - indices_base + (u64)limit * size + 3) & ~3ull : record_at - record_base + read},
        {*arguments, *arguments_at, bytes}};
    snx_vk_storage_buffers(g_vk.dev, set, buffers, 3);
    vk_render_end(); /* dispatches happen outside rendering passes */
    VkCommandBuffer cb = vk_cmd();
    quads_barrier(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    /* rows of at most 65,535 workgroups, the least maxComputeWorkGroupCount[0] the specification allows */
    const u32 units = work < 65535u * 64 ? (u32)work : 65535u * 64, rows = (u32)((work + units - 1) / units);
    const u32 push[10] = {(u32)((record_at - record_base) / 4),
                          stride / 4,
                          records,
                          size ? (u32)(indices_at - indices_base) : 0,
                          size,
                          last ? 1u : 0u,
                          limit,
                          quads,
                          (u32)(made / 4),
                          units};
    rec_CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_indirect_quads.pipeline);
    rec_CmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_indirect_quads.layout, 0, 1, &set, 0, NULL);
    rec_CmdPushConstants(cb, g_indirect_quads.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    vk_gpuprof_tag(VK_PROF_INDIRECT_QUADS);
    rec_CmdDispatch(cb, (units + 63) / 64, rows, 1);
    quads_barrier(cb, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT,
                  VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
    *made_at = *arguments_at + made;
    return 1;
}
