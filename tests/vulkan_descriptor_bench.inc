/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* CPU submission benchmark. Pixel and buffer correctness is checked by the ordinary fixture first. No game data,
   AI, camera, frame limiter, shader compilation, or GPU shader work participates in these measurements. */
static int bench_descriptors(u32 sets, u32 count, u32 images) {
    CHECK(count && count <= 34 && sets && sets <= 16384 && images <= 16);
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorImageInfo image = {0};
    if (images) {
        VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
        CHECK(vkCreateSampler(g_vk.dev, &info, NULL, &sampler) == VK_SUCCESS);
        image = (VkDescriptorImageInfo){sampler, vk_null_view(VK_IMAGE_VIEW_TYPE_2D, 0, 0),
                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        CHECK(image.imageView);
    }
    u64 elapsed[3] = {0};
    for (u32 round = 0; round < 5; round++) {
        vk_lock();
        VkCommandBuffer cb = vk_cmd();
        VkBuffer buffer;
        u64 offset;
        u64 align = vk_stage_align();
        u8* bytes = vk_stage(count * align, align, &buffer, &offset);
        CHECK(bytes);
        memset(bytes, 0, (size_t)(count * align)); /* guest uniform bytes, never an object with padding */
        VkWriteDescriptorSet writes[50];
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
        u64 start = plat_ns();
        for (u32 i = 0; i < sets; i++) {
            for (u32 j = 0; j < count; j++) info[j].offset = offset + ((i + j) % count) * align;
            VkDescriptorSet set = g_vk.has_descriptor_buffers
                ? vk_descriptors_write(g_vk_dslayout[0][0], count + images, writes)
                : vk_rec_new_set(g_vk_dslayout[0][0], count + images, writes);
            CHECK(set);
            cb = vk_cmd(); /* an arena overflow can start another frame during descriptor allocation */
            vk_descriptors_bind(cb, g_vk_playout, 0, 1, &set);
        }
        vk_finish(); /* include the recording thread's allocation, descriptor updates and command recording */
        if (round >= 2) elapsed[round - 2] = plat_ns() - start;
        vk_frame_next();
        vk_unlock();
    }
    /* The median of three measured batches, after priming both frame slots. */
    if (elapsed[0] > elapsed[1]) { u64 v = elapsed[0]; elapsed[0] = elapsed[1]; elapsed[1] = v; }
    if (elapsed[1] > elapsed[2]) { u64 v = elapsed[1]; elapsed[1] = elapsed[2]; elapsed[2] = v; }
    if (elapsed[0] > elapsed[1]) { u64 v = elapsed[0]; elapsed[0] = elapsed[1]; elapsed[1] = v; }
    if (sampler) vkDestroySampler(g_vk.dev, sampler, NULL);
    printf("DESCRIPTOR_BENCH {\"sets\":%u,\"descriptors\":%u,\"images\":%u,\"elapsed_ns\":%llu,\"vk_descriptors\":%u,\"vk_record\":%u}\n",
           sets, count, images, (unsigned long long)elapsed[1], (u32)g_vk.has_descriptor_buffers,
           (u32)g_settings.vulkan_recording_thread);
    return 0;
}
