/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/vk/descriptor_heaps.h"
#include "nx/gpu/vk/ledger.h"
/* CPU submission benchmark. Pixel and buffer correctness is checked by the ordinary fixture first. No game data,
   AI, camera, frame limiter, shader compilation, or GPU shader work participates in these measurements. */
static int bench_descriptor_pattern(u32 sets, u32 count, u32 images, u32 indexed, int unique) {
    CHECK(count && count <= 34 && sets && sets <= 16384 && images <= 16 && indexed <= SNX_TEX_ARRAY);
    if (indexed && !g_shader_texture_arrays) {
        puts("SKIP DESCRIPTOR_PATTERN_BENCH indexed: descriptor arrays are unavailable");
        return 0;
    }
    u64 alignment = vk_stage_align(), staged_bytes = (u64)count * alignment * (unique ? sets : 1);
    if (unique && staged_bytes > g_vk.alloc_cap) {
        printf("SKIP DESCRIPTOR_PATTERN_BENCH unique: %llu staging bytes exceed the allocation cap\n",
               (unsigned long long)staged_bytes);
        return 0;
    }
    /* A frame-owned staging range would retire while later benchmark operations still reference it. This buffer
       owns its allocation for the complete batch, including all measured frame transitions. */
    VkBuffer buffer;
    VmaAllocation memory;
    VkBufferCreateInfo buffer_info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = staged_bytes,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    VmaAllocationCreateInfo allocation_info = {.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
        .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo allocation;
    CHECK(vk_buffer_create(g_vk.vma, &buffer_info, &allocation_info, &buffer, &memory, &allocation) == VK_SUCCESS);
    vk_mem_track(memory, VK_OWN_MISC);
    CHECK(allocation.pMappedData);
    memset(allocation.pMappedData, 0, (size_t)staged_bytes);
    VkSampler samplers[SNX_TEX_ARRAY] = {0};
    u32 sampler_count = indexed ? indexed : images ? 1 : 0;
    VkDescriptorImageInfo image = {0}, array[SNX_TEX_ARRAY];
    for (u32 i = 0; i < sampler_count; i++) {
        VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .minLod = (float)i / (float)sampler_count, .maxLod = 1};
        CHECK(vk_sampler_create(g_vk.dev, &info, NULL, &samplers[i]) == VK_SUCCESS);
    }
    if (sampler_count) {
        image = (VkDescriptorImageInfo){samplers[0], vk_null_view(VK_IMAGE_VIEW_TYPE_2D, 0, 0),
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        CHECK(image.imageView);
    }
    if (indexed)
        for (u32 i = 0; i < SNX_TEX_ARRAY; i++) {
            array[i] = image;
            array[i].sampler = samplers[i % indexed];
        }
    u64 elapsed[3] = {0};
    VkDescriptorStats counters[3][2];
    VkDescriptorHeapStats heap_counters[3][2];
    u64 frames[3] = {0};
    for (u32 round = 0; round < 5; round++) {
        vk_lock();
        VkCommandBuffer cb = vk_cmd();
        u64 offset = 0, align = alignment;
        VkWriteDescriptorSet writes[52];
        VkDescriptorBufferInfo info[34];
        for (u32 i = 0; i < count; i++) {
            info[i] = (VkDescriptorBufferInfo){buffer, offset + i * align, 16};
            writes[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = i, .descriptorCount = 1,
                .descriptorType = i < 18 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &info[i]};
        }
        for (u32 i = 0; i < images; i++)
            writes[count + i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstBinding = SNX_VKB_TEX + i, .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = &image};
        if (indexed)
            for (u32 i = 0; i < 2; i++)
                writes[count + images + i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                    .dstBinding = i ? SNX_VKB_TEXDEPTH : SNX_VKB_TEXARRAY, .descriptorCount = SNX_TEX_ARRAY,
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .pImageInfo = array};
        VkDescriptorStats before, after;
        VkDescriptorHeapStats heap_before = {0}, heap_after = {0};
        vk_descriptors_get_stats(&before);
        if (g_vk.has_descriptor_heaps) vk_descriptor_heaps_get_stats(&heap_before);
        u64 frame_before = vk_frame_id();
        u64 start = plat_ns();
        for (u32 i = 0; i < sets; i++) {
            /* Retirement is allowed before a complete operation, and its wait/recording cost is timed. */
            vk_frame_check();
            cb = vk_cmd();
            for (u32 j = 0; j < count; j++)
                info[j].offset = offset + (unique ? (u64)i * count + j : (i + j) % count) * align;
            VkDescriptorSet set = (g_vk.has_descriptor_buffers || g_vk.has_descriptor_heaps)
                ? vk_descriptors_write(g_vk_dslayout[indexed != 0][0], count + images + (indexed ? 2 : 0), writes)
                : vk_rec_new_set(g_vk_dslayout[indexed != 0][0], count + images + (indexed ? 2 : 0), writes);
            CHECK(set);
            cb = vk_cmd();
            vk_descriptors_bind(cb, vk_playout(indexed ? 1 : 0), 0, 1, &set);
        }
        vk_finish(); /* include the recording thread's allocation, descriptor updates and command recording */
        u64 took = plat_ns() - start;
        vk_descriptors_get_stats(&after);
        if (g_vk.has_descriptor_heaps) vk_descriptor_heaps_get_stats(&heap_after);
        if (round >= 2) {
            elapsed[round - 2] = took;
            counters[round - 2][0] = before;
            counters[round - 2][1] = after;
            heap_counters[round - 2][0] = heap_before;
            heap_counters[round - 2][1] = heap_after;
            frames[round - 2] = vk_frame_id() - frame_before;
        }
        vk_frame_next();
        vk_unlock();
    }
    /* The median of three measured batches, after priming both frame slots. */
    u32 order[3] = {0, 1, 2};
    if (elapsed[order[0]] > elapsed[order[1]]) { u32 v = order[0]; order[0] = order[1]; order[1] = v; }
    if (elapsed[order[1]] > elapsed[order[2]]) { u32 v = order[1]; order[1] = order[2]; order[2] = v; }
    if (elapsed[order[0]] > elapsed[order[1]]) { u32 v = order[0]; order[0] = order[1]; order[1] = v; }
    u32 median = order[1];
    for (u32 i = 0; i < sampler_count; i++) vk_sampler_destroy(g_vk.dev, samplers[i], NULL);
    vk_mem_untrack(memory);
    vmaDestroyBuffer(g_vk.vma, buffer, memory);
    if (!unique && !indexed)
        printf("DESCRIPTOR_BENCH {\"sets\":%u,\"descriptors\":%u,\"images\":%u,\"elapsed_ns\":%llu,\"vk_descriptors\":%u,\"vk_descriptor_heaps\":%u,\"vk_record\":%u}\n",
               sets, count, images, (unsigned long long)elapsed[median], (u32)g_vk.has_descriptor_buffers,
               (u32)g_vk.has_descriptor_heaps, (u32)g_settings.vulkan_recording_thread);
    const VkDescriptorStats* first = &counters[median][0];
    const VkDescriptorStats* last = &counters[median][1];
    const VkDescriptorHeapStats* heap_first = &heap_counters[median][0];
    const VkDescriptorHeapStats* heap_last = &heap_counters[median][1];
    printf("DESCRIPTOR_PATTERN_BENCH {\"pattern\":\"%s\",\"sets\":%u,\"descriptors\":%u,\"images\":%u,"
           "\"elapsed_ns\":%llu,\"vk_descriptors\":%u,\"vk_descriptor_heaps\":%u,\"vk_record\":%u,\"buffer_requests\":%llu,"
           "\"buffer_driver_calls\":%llu,\"buffer_hits\":%llu,\"buffer_evictions\":%llu,\"image_requests\":%llu,"
           "\"image_driver_calls\":%llu,\"image_hits\":%llu,\"image_replicas\":%llu,\"full_layout_bytes\":%llu,"
           "\"arena_bytes\":%llu,\"payload_bytes\":%llu,\"frame_advances\":%llu,\"arena_allocated_bytes\":%llu,"
           "\"cache_allocated_bytes\":%llu,\"backend\":%u,\"counters_available\":%u,\"fallbacks\":0,"
           "\"address_queries\":%llu,\"peak_used_bytes\":%llu,\"workload_buffer_bytes\":%llu,"
           "\"heap_table_bytes\":%llu,\"heap_image_lookups\":%llu,\"heap_sampler_requests\":%llu,"
           "\"heap_sampler_lookups\":%llu,\"heap_sampler_replicas\":%llu,\"heap_sampler_driver_calls\":%llu,"
           "\"heap_sampler_hits\":%llu,\"heap_reserved_bytes\":%llu,\"heap_resource_range_bytes\":%llu,"
           "\"heap_sampler_range_bytes\":%llu,\"heap_buffer_native_calls\":%llu,\"indexed\":%u}\n",
           unique ? "unique" : "repeated", sets, count, images, (unsigned long long)elapsed[median],
           (u32)g_vk.has_descriptor_buffers, (u32)g_vk.has_descriptor_heaps, (u32)g_settings.vulkan_recording_thread,
           (unsigned long long)(last->buffer_requests - first->buffer_requests),
           (unsigned long long)(last->buffer_driver_calls - first->buffer_driver_calls),
           (unsigned long long)(last->buffer_hits - first->buffer_hits),
           (unsigned long long)(last->buffer_evictions - first->buffer_evictions),
           (unsigned long long)(last->image_requests - first->image_requests),
           (unsigned long long)(last->image_driver_calls - first->image_driver_calls),
           (unsigned long long)(last->image_hits - first->image_hits),
           (unsigned long long)(last->image_replicas - first->image_replicas),
           (unsigned long long)(last->full_layout_bytes - first->full_layout_bytes),
           (unsigned long long)(last->arena_bytes - first->arena_bytes),
           (unsigned long long)(last->payload_bytes - first->payload_bytes),
           (unsigned long long)frames[median],
           (unsigned long long)last->arena_allocated_bytes, (unsigned long long)last->cache_allocated_bytes,
           g_vk.has_descriptor_heaps ? 2u : g_vk.has_descriptor_buffers ? 1u : 0u,
           (u32)(g_vk.has_descriptor_heaps || g_vk.has_descriptor_buffers),
           (unsigned long long)(last->address_queries - first->address_queries),
           (unsigned long long)last->peak_used_bytes, (unsigned long long)allocation.size,
           (unsigned long long)(heap_last->table_bytes - heap_first->table_bytes),
           (unsigned long long)(heap_last->image_lookups - heap_first->image_lookups),
           (unsigned long long)(heap_last->sampler_requests - heap_first->sampler_requests),
           (unsigned long long)(heap_last->sampler_lookups - heap_first->sampler_lookups),
           (unsigned long long)(heap_last->sampler_replicas - heap_first->sampler_replicas),
           (unsigned long long)(heap_last->sampler_driver_calls - heap_first->sampler_driver_calls),
           (unsigned long long)(heap_last->sampler_hits - heap_first->sampler_hits),
           (unsigned long long)heap_last->reserved_bytes, (unsigned long long)heap_last->resource_range_bytes,
           (unsigned long long)heap_last->sampler_range_bytes,
           (unsigned long long)(heap_last->buffer_native_calls - heap_first->buffer_native_calls), indexed);
    return 0;
}
static int bench_descriptors(u32 sets, u32 count, u32 images, u32 indexed) {
    CHECK(!bench_descriptor_pattern(sets, count, images, indexed, 0));
    CHECK(!bench_descriptor_pattern(sets, count, images, indexed, 1));
    return 0;
}
