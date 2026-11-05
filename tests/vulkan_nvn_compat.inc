/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic NVN array copies, through the real renderer. Distinct layer patterns catch wrong row/layer addressing;
   explicit image strides and untouched output padding catch a correct-looking but incorrectly packed copy. */
static int check_nvn_array_copies(void) {
    const u64 gpu = 0x4b0000000ull, cpu = HEAP_BASE + 0x740000;
    const u64 input = gpu + 0x1000, output = gpu + 0xc000;
    const u64 snap = cpu + 0x100, snap2 = cpu + 0x200, record = cpu + 0x400;
    NvnTexture texture = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu + 0x4000};
    texture.d.target = NVN_TEXTURE_TARGET_1D_ARRAY;
    texture.d.format = NVN_FORMAT_RGBA8;
    texture.d.width = 8;
    texture.d.height = 7;
    texture.d.depth = 1;
    texture.d.levels = 1;
    NvnTexture destination = texture;
    destination.gpu = gpu + 0x8000;
    NvnTexLayout layout;
    nvn_tex_layout(&texture.d, &layout);
    CHECK(layout.size <= 0x4000 && layout.layer_size);
    gmmu_map(gpu, cpu, 0x10000);
    memset(GP(cpu), 0xcd, 0x10000);
    memcpy(GP(snap), &texture, NVN_TEX_SNAP);
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    u32 pixels[7][8];
    for (u32 layer = 0; layer < 7; layer++)
        for (u32 x = 0; x < 8; x++) pixels[layer][x] = 0xff000000u | layer << 16 | x;
    CHECK(!gmem_write(input, pixels, sizeof pixels));
    i64 stride[2] = {0, 0};
    NvnPCopyB2T copy = {.buffer = input, .texture = snap, .r = {0, 0, 0, 8, 7, 1}};
    memcpy(GP(record), &copy, sizeof copy);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof copy, stride));
    /* Read the texture image itself, so a matching error in the upload and download cannot cancel out. */
    u32 tic[8];
    CHECK(nvn_tic_encode(&texture, NULL, 0, 0, tic));
    vk_lock();
    Surf* image = vk_tex_from_tic(tic);
    CHECK(image && image->h == 1 && image->layers == 7);
    VkBuffer buffer;
    u8* actual = vk_readback_buffer(sizeof pixels, &buffer);
    CHECK(actual);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 7}, .imageExtent = {8, 1, 1}};
    vk_surf_use(vk_cmd(), image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), image->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    vk_finish();
    vk_readback_ready();
    CHECK(!memcmp(actual, pixels, sizeof pixels));
    vk_unlock();
    NvnPCopyT2T between = {.src = snap,
                           .dst = snap2,
                           .sr = {0, 2, 0, 8, 3, 1},
                           .dr = {0, 1, 0, 8, 3, 1},
                           .flags = NVN_COPY_FLAGS_MIRROR_Z_BIT};
    memcpy(GP(record), &between, sizeof between);
    CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof between, stride));
    u8 expected[3 * 96];
    memset(expected, 0xcd, sizeof expected);
    for (u32 layer = 0; layer < 3; layer++) memcpy(expected + layer * 96, pixels[4 - layer], 32);
    copy = (NvnPCopyB2T){.buffer = output, .texture = snap2, .r = {0, 1, 0, 8, 3, 1}};
    stride[0] = 64;
    stride[1] = 96;
    memcpy(GP(record), &copy, sizeof copy);
    CHECK(nvn_copy_record(OP_COPY_T2B, record, sizeof copy, stride));
    vk_lock();
    vk_finish();
    vk_unlock();
    guest_done(cpu + 0xc000, sizeof expected);
    CHECK(!memcmp(GP(cpu + 0xc000), expected, sizeof expected));
    /* A view's layer base and public Y layer offset compose; MIRROR_Z reverses the buffer images on download. */
    copy.texture = snap;
    copy.has_view = 1;
    copy.view = (NvnView){.set = NVN_VIEW_LAYERS, .min_layer = 1, .layers = 6};
    copy.r.yoffset = 2;
    copy.flags = NVN_COPY_FLAGS_MIRROR_Z_BIT;
    memcpy(GP(record), &copy, sizeof copy);
    CHECK(nvn_copy_record(OP_COPY_T2B, record, sizeof copy, stride));
    for (u32 layer = 0; layer < 3; layer++) memcpy(expected + layer * 96, pixels[5 - layer], 32);
    vk_lock();
    vk_finish();
    vk_unlock();
    guest_done(cpu + 0xc000, sizeof expected);
    CHECK(!memcmp(GP(cpu + 0xc000), expected, sizeof expected));
    puts("PASS NVN array copies: image texels, mirrored layers, view offsets and output padding exact=1");
    return 0;
}

/* The independent aspects are copied directly from both array layers, without guest layout conversion. */
static int nvn_stencil_snapshot(Surf* target, u8 out[640]) {
    VkBuffer buffer;
    u8* result = vk_readback_buffer(640, &buffer);
    CHECK(result);
    VkBufferImageCopy copies[2] = {
        {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 2}, .imageExtent = {8, 8, 1}},
        {.bufferOffset = 512, .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 2}, .imageExtent = {8, 8, 1}}};
    vk_surf_use(vk_cmd(), target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), target->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 2, copies);
    vk_finish();
    vk_readback_ready();
    memcpy(out, result, 640);
    /* D24 depth copies use X8_D24_UNORM_PACK32. Its unused high byte has no defined value. */
    if (target->f.vk == VK_FORMAT_D24_UNORM_S8_UINT)
        for (u32 i = 3; i < 512; i += 4) out[i] = 0;
    return 0;
}

static int check_nvn_stencil_masks(void) {
    const u64 gpu = 0x4c0000000ull, cpu = HEAP_BASE + 0x700000;
    const u32 formats[2] = {0x14, 0x19}; /* D24S8 and D32FS8 */
    gmmu_map(gpu, cpu, 0x10000);
    memset(GP(cpu), 0xcd, 0x10000);
    DsState* state = calloc(1, sizeof *state);
    CHECK(state);
    DrawOwner owner = {0};
    state->dyn.clip_height = 8;
    state->vp.scissor_on = 1;
    state->vp.scissor_x[0] = 2u | 6u << 16;
    state->vp.scissor_y[0] = 1u | 5u << 16;
    vk_lock();
    for (u32 format = 0; format < 2; format++) {
        u64 address = gpu + format * 0x4000;
        state->tgt.depth = (DsSurf){
            .gva = address, .width = 8, .height = 8, .format = formats[format], .layers = 2, .layer_stride = 512};
        DrawClear clear = {.aspects = 3, .depth = 0.25f, .stencil = 0xaa};
        vk_render_clear(state, &clear, &owner);
        clear.layer = 1;
        vk_render_clear(state, &clear, &owner);
        vk_render_end();
        Surf* target = vk_rt_find(address);
        CHECK(target && target->layers == 2);
        u8 before[640], after[640];
        CHECK(!nvn_stencil_snapshot(target, before));
        clear = (DrawClear){.aspects = 2, .stencil = 5, .stencil_preserve = 0xf0, .scissored = 1, .layer = 1};
        vk_render_clear(state, &clear, &owner);
        vk_render_end();
        CHECK(!nvn_stencil_snapshot(target, after));
        CHECK(!memcmp(before, after, 512)); /* no depth writes */
        for (u32 layer = 0; layer < 2; layer++)
            for (u32 y = 0; y < 8; y++)
                for (u32 x = 0; x < 8; x++) {
                    u8 expected = layer == 1 && x >= 2 && x < 6 && y >= 1 && y < 5 ? 0xa5 : 0xaa;
                    if (after[512 + layer * 64 + y * 8 + x] != expected)
                        fprintf(stderr, "stencil format=%x layer=%u x=%u y=%u before=%02x after=%02x expected=%02x\n",
                                formats[format], layer, x, y, before[512 + layer * 64 + y * 8 + x],
                                after[512 + layer * 64 + y * 8 + x], expected);
                    CHECK(after[512 + layer * 64 + y * 8 + x] == expected);
                }
        clear.stencil_preserve = 0xff;
        clear.stencil = 0;
        vk_render_clear(state, &clear, &owner);
        vk_render_end();
        CHECK(!nvn_stencil_snapshot(target, before));
        CHECK(!memcmp(before, after, sizeof after)); /* zero write mask changes no aspect */
        clear = (DrawClear){.aspects = 3, .depth = 0.75f, .stencil = 0x30, .stencil_preserve = 0x0f};
        vk_render_clear(state, &clear, &owner);
        vk_render_end();
        CHECK(!nvn_stencil_snapshot(target, after));
        CHECK(memcmp(before, after, 256) && !memcmp(before + 256, after + 256, 256));
        for (u32 i = 0; i < 64; i++) CHECK(after[512 + i] == 0x3a);
    }
    vk_unlock();
    free(state);
    puts("PASS NVN stencil masks: preserved bits, depth isolation, scissor and array layers exact=1");
    return 0;
}
