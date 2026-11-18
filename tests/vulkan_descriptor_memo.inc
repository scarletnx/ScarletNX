/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The words a stage's descriptor table keeps from its last table (gpu/vk/descriptor_heaps.c memo_get). A table of four
   buffers (one batch), a view and a sampler is written again and again through the descriptor heaps: a binding that
   names what the last table named takes its word with no lookup, one whose range changed is looked up, and nothing is
   kept after a forget or into a new frame. The sequence runs with draw_state_check=0, where the counts of kept words
   must be these, and with draw_state_check=1, where every word the memo had is found again and must be the same. Only
   with descriptor heaps (vk_descriptors_mode=2) */
#include "nx/gpu/vk/descriptors.h"
#include "nx/gpu/vk/ledger.h"
typedef struct {
    VkBuffer buffer;
    DevMem memory;
    VkSampler sampler[4];
    VkImageView view;
    u64 align;
} DmRes;
static int dm_res_make(DmRes* r, u64 bytes) {
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = bytes,
                               .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    DevMemWant want = {
        .at = DM_HOST, .mapped = 1, .need = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    DevMemInfo got;
    CHECK(vk_buffer_create(&info, &want, &r->buffer, &r->memory, &got) == VK_SUCCESS);
    vk_mem_track(r->memory, VK_OWN_MISC);
    for (u32 i = 0; i < 4; i++) {
        VkSamplerCreateInfo s = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                 .magFilter = VK_FILTER_NEAREST,
                                 .minFilter = VK_FILTER_NEAREST,
                                 .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                 .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                 .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                 .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                 .minLod = (float)i / 4,
                                 .maxLod = 1};
        CHECK(vk_sampler_create(g_vk.dev, &s, NULL, &r->sampler[i]) == VK_SUCCESS);
    }
    r->view = vk_null_view(VK_IMAGE_VIEW_TYPE_2D, 0, 0);
    CHECK(r->view);
    r->align = vk_stage_align();
    return 0;
}
static void dm_res_free(DmRes* r) {
    for (u32 i = 0; i < 4; i++) vk_sampler_destroy(g_vk.dev, r->sampler[i], NULL);
    vk_mem_untrack(r->memory);
    vk_devmem_buffer_destroy(r->buffer, r->memory);
}
/* a table of `ubos` constant buffers at the offsets (in alignments) of at[], two storage buffers and `images`
   combined image samplers, written through the heaps: 1 when it was */
static int dm_table(const DmRes* r, u32 ubos, const u32* at, u32 images) {
    VkWriteDescriptorSet w[16];
    VkDescriptorBufferInfo b[8];
    VkDescriptorImageInfo im[4];
    u32 n = 0;
    for (u32 i = 0; i < ubos + 2; i++) {
        b[i] = (VkDescriptorBufferInfo){r->buffer, i < ubos ? at[i] * r->align : (64 + i) * r->align, 256};
        w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstBinding = i < ubos ? SNX_VKB_CB + i : SNX_VKB_SSBO + i - ubos,
                                        .descriptorCount = 1,
                                        .descriptorType = i < ubos ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                                                   : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                        .pBufferInfo = &b[i]};
    }
    for (u32 i = 0; i < images; i++) {
        im[i] = (VkDescriptorImageInfo){r->sampler[i], r->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        w[n++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstBinding = SNX_VKB_TEX + i,
                                        .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        .pImageInfo = &im[i]};
    }
    vk_cmd();
    return vk_descriptors_write(g_vk_dslayout[0][0], n, w, 0) != VK_NULL_HANDLE;
}
/* one step of the sequence: its table, and the words kept (draw_state_check=0) or found again and compared (=1) */
typedef struct {
    const char* what;
    int frame;  /* a new frame first */
    int forget; /* a forget of another object first (it empties the memo) */
    u32 at[2];
    u32 kept;
} DmStep;
static int dm_sequence(const DmRes* r, int check, u64* differs) {
    static const DmStep steps[] = {
        {"the first table of a frame", 1, 0, {0, 1}, 0},         {"the same table again", 0, 0, {0, 1}, 6},
        {"one constant buffer offset changed", 0, 0, {2, 1}, 5}, {"after a forget of another view", 0, 1, {2, 1}, 0},
        {"the same table after the forget", 0, 0, {2, 1}, 6},    {"the first table of the next frame", 1, 0, {2, 1}, 0},
        {"its two constant buffers changed", 0, 0, {3, 4}, 4},
    };
    int saved = g_settings.draw_state_check;
    g_settings.draw_state_check = check;
    VkDescriptorHeapStats s0, s1;
    vk_descriptor_heaps_get_stats(&s0);
    int failed = 0;
    for (u32 i = 0; i < sizeof steps / sizeof *steps; i++) {
        const DmStep* st = &steps[i];
        vk_lock();
        if (st->frame) vk_frame_next();
        if (st->forget) vk_descriptor_heaps_forget_view((VkImageView)(uintptr_t)0x5eed0000u);
        VkDescriptorHeapStats a, b;
        vk_descriptor_heaps_get_stats(&a);
        int wrote = dm_table(r, 2, st->at, 1);
        vk_descriptor_heaps_get_stats(&b);
        vk_unlock();
        u64 got = check ? b.memo_checked - a.memo_checked : b.memo_kept - a.memo_kept;
        if (!wrote || got != st->kept || (check && b.memo_kept != a.memo_kept)) {
            fprintf(stderr, "descriptor memo, %s with draw_state_check=%d: written %d, %llu words %s (expected %u)\n",
                    st->what, check, wrote, (unsigned long long)got, check ? "found again" : "kept", st->kept);
            failed = 1;
        }
    }
    vk_descriptor_heaps_get_stats(&s1);
    *differs = s1.memo_differs - s0.memo_differs;
    g_settings.draw_state_check = saved;
    return failed;
}
static int check_descriptor_memo(void) {
    if (!g_vk.has_descriptor_heaps) {
        puts("SKIP descriptor memo: no descriptor heaps");
        return 0;
    }
    DmRes r = {0};
    CHECK(!dm_res_make(&r, 256 * vk_stage_align()));
    u64 differs[2] = {0, 0};
    int failed = dm_sequence(&r, 0, &differs[0]) | dm_sequence(&r, 1, &differs[1]);
    if (differs[0] || differs[1]) {
        fprintf(stderr, "descriptor memo: %llu kept words differ from those found again\n",
                (unsigned long long)(differs[0] + differs[1]));
        failed = 1;
    }
    vk_lock();
    vk_finish();
    vk_unlock();
    dm_res_free(&r);
    if (!failed) puts("PASS descriptor memo: kept words where the binding is the same, none after a forget or a frame");
    return failed;
}
