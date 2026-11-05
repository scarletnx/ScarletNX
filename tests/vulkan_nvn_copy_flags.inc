/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Copy flags through NVN records and the real renderer, using independent image readback and exact row padding. */
static int nvn_copy_check_pixels(const NvnTexture* texture, const u32* expected, u32 width, u32 height) {
    u32 depth = texture->d.target == NVN_TEXTURE_TARGET_3D ? texture->d.depth : 1;
    u64 bytes = (u64)width * height * depth * 4;
    u32 tic[8];
    CHECK(nvn_tic_encode(texture, NULL, 0, 0, tic));
    vk_lock();
    Surf* image = vk_tex_from_tic(tic);
    CHECK(image && image->w == width && image->h == height);
    CHECK(depth == 1 || image->depth3d == depth);
    VkBuffer buffer;
    u8* actual = vk_readback_buffer(bytes, &buffer);
    CHECK(actual);
    VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                .imageExtent = {width, height, depth}};
    vk_surf_use(vk_cmd(), image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), image->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
    vk_finish();
    vk_readback_ready();
    CHECK(!memcmp(actual, expected, (size_t)bytes));
    vk_unlock();
    return 0;
}

static int nvn_copy_volume_flags(u64 gpu, u64 cpu) {
    enum { W = 4, H = 3, D = 3 };
    const u64 snap = cpu + 0x100, snap2 = cpu + 0x200, record = cpu + 0x600, input = gpu + 0x1000;
    NvnTexture source = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu + 0x2000};
    source.d = (NvnTexDesc){
        .target = NVN_TEXTURE_TARGET_3D, .format = NVN_FORMAT_RGBA8, .width = W, .height = H, .depth = D, .levels = 1};
    NvnTexture destination = source;
    destination.gpu = gpu + 0x6000;
    memcpy(GP(snap), &source, NVN_TEX_SNAP);
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    u32 pixels[D][H][W], expected[D][H][W];
    for (u32 z = 0; z < D; z++)
        for (u32 y = 0; y < H; y++)
            for (u32 x = 0; x < W; x++) pixels[z][y][x] = 0xff000000u | (z + 1) << 16 | y << 8 | x * 31;
    CHECK(!gmem_write(input, pixels, sizeof pixels));
    i64 stride[2] = {0};
    NvnPCopyB2T buffer = {.buffer = input, .texture = snap, .r = {0, 0, 0, W, H, D}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    CHECK(!nvn_copy_check_pixels(&source, &pixels[0][0][0], W, H));
    for (u32 flags = 0; flags < 8; flags++) {
        NvnPCopyT2T copy = {.src = snap,
                            .dst = snap2,
                            .sr = {0, 0, 0, W, H, D},
                            .dr = {0, 0, 0, W, H, D},
                            .flags = (flags & 1 ? NVN_COPY_FLAGS_MIRROR_X_BIT : 0) |
                                     (flags & 2 ? NVN_COPY_FLAGS_MIRROR_Y_BIT : 0) |
                                     (flags & 4 ? NVN_COPY_FLAGS_MIRROR_Z_BIT : 0)};
        memcpy(GP(record), &copy, sizeof copy);
        CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
        for (u32 z = 0; z < D; z++)
            for (u32 y = 0; y < H; y++)
                for (u32 x = 0; x < W; x++)
                    expected[z][y][x] =
                        pixels[flags & 4 ? D - 1 - z : z][flags & 2 ? H - 1 - y : y][flags & 1 ? W - 1 - x : x];
        CHECK(!nvn_copy_check_pixels(&destination, &expected[0][0][0], W, H));
    }
    destination.gpu = gpu + 0xa000;
    destination.d.width = 2 * W;
    destination.d.height = 2 * H;
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    NvnPCopyT2T copy = {.src = snap, .dst = snap2, .sr = {0, 0, 0, W, H, D}, .dr = {0, 0, 0, 2 * W, 2 * H, D}};
    memcpy(GP(record), &copy, sizeof copy);
    CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
    u32 scaled[D][2 * H][2 * W];
    for (u32 z = 0; z < D; z++)
        for (u32 y = 0; y < 2 * H; y++)
            for (u32 x = 0; x < 2 * W; x++) scaled[z][y][x] = pixels[z][y / 2][x / 2];
    CHECK(!nvn_copy_check_pixels(&destination, &scaled[0][0][0], 2 * W, 2 * H));
    return 0;
}

static int check_nvn_copy_flags(void) {
    enum { W = 4, H = 3, PITCH = 64 };
    const u64 gpu = 0x4cc000000ull, cpu = HEAP_BASE + 0x730000;
    const u64 snap = cpu + 0x100, snap2 = cpu + 0x200, record = cpu + 0x600;
    const u64 input = gpu + 0x1000, output = gpu + 0xc000;
    gmmu_map(gpu, cpu, 0x10000);
    memset(GP(cpu), 0xcd, 0x10000);
    NvnTexture source = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu + 0x4000};
    source.d = (NvnTexDesc){
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = W, .height = H, .depth = 1, .levels = 1};
    NvnTexture destination = source;
    destination.gpu = gpu + 0x8000;
    memcpy(GP(snap), &source, NVN_TEX_SNAP);
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    u8 rows[H * PITCH];
    memset(rows, 0xcd, sizeof rows);
    u32 pixels[H][W], expected[H][W];
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) {
            pixels[y][x] = 0xff000000u | (y + 1) << 16 | (x + 1) * 37;
            snx_st_le32(rows + y * PITCH + x * 4, pixels[y][x]);
        }
    CHECK(!gmem_write(input, rows, sizeof rows));
    i64 stride[2] = {PITCH, 0};
    NvnPCopyB2T buffer = {
        .buffer = input, .texture = snap, .r = {0, 0, 0, W, H, 1}, .flags = NVN_COPY_FLAGS_MIRROR_Y_BIT};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) expected[y][x] = pixels[H - 1 - y][x];
    CHECK(!nvn_copy_check_pixels(&source, &expected[0][0], W, H));
    buffer.flags |= NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_ENGINE_2D_BIT;
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) expected[y][x] = pixels[H - 1 - y][W - 1 - x];
    CHECK(!nvn_copy_check_pixels(&source, &expected[0][0], W, H));
    /* The download reverses the texture rows into buffer rows; row padding keeps its original bytes. */
    buffer.buffer = output;
    buffer.flags = NVN_COPY_FLAGS_MIRROR_Y_BIT;
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_T2B, record, sizeof buffer, stride));
    for (u32 y = 0; y < H; y++)
        for (u32 x = 0; x < W; x++) snx_st_le32(rows + y * PITCH + x * 4, pixels[y][W - 1 - x]);
    vk_lock();
    vk_finish();
    vk_unlock();
    guest_done(cpu + 0xc000, sizeof rows);
    CHECK(!memcmp(GP(cpu + 0xc000), rows, sizeof rows));
    for (u32 flags = 0; flags < 4; flags++) {
        NvnPCopyT2T copy = {.src = snap,
                            .dst = snap2,
                            .sr = {0, 0, 0, W, H, 1},
                            .dr = {0, 0, 0, W, H, 1},
                            .flags = (flags & 1 ? NVN_COPY_FLAGS_MIRROR_X_BIT : 0) |
                                     (flags & 2 ? NVN_COPY_FLAGS_MIRROR_Y_BIT : 0)};
        memcpy(GP(record), &copy, sizeof copy);
        CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
        for (u32 y = 0; y < H; y++)
            for (u32 x = 0; x < W; x++) {
                u32 sx = flags & 1 ? x : W - 1 - x, sy = flags & 2 ? y : H - 1 - y;
                expected[y][x] = pixels[sy][sx];
            }
        CHECK(!nvn_copy_check_pixels(&destination, &expected[0][0], W, H));
    }
    /* At a 2x horizontal scale, the center samples distinguish nearest from linear exactly (0,63,189,252). */
    source.gpu = gpu + 0xe000;
    source.d.width = source.d.height = 2;
    destination.gpu = gpu + 0xf000;
    destination.d.width = 4;
    destination.d.height = 2;
    memcpy(GP(snap), &source, NVN_TEX_SNAP);
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    const u32 gradient[2][2] = {{0xff000000u, 0xfffcfcfcu}, {0xff000000u, 0xfffcfcfcu}};
    CHECK(!gmem_write(input, gradient, sizeof gradient));
    stride[0] = stride[1] = 0;
    buffer = (NvnPCopyB2T){.buffer = input, .texture = snap, .r = {0, 0, 0, 2, 2, 1}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    /* A draw can leave a padded render target at the source address. Filtering must still clamp at texel two. */
    DsSurf padded;
    int is_depth;
    u32 samples;
    CHECK(nvn_draw_surface(snap, NULL, &padded, &is_depth, &samples));
    vk_lock();
    CHECK(vk_rt(padded.gva, padded.width, padded.height, padded.format, padded.mem, 0, 0));
    vk_unlock();
    const u32 nearest[2][4] = {{0xff000000u, 0xff000000u, 0xfffcfcfcu, 0xfffcfcfcu},
                               {0xff000000u, 0xff000000u, 0xfffcfcfcu, 0xfffcfcfcu}};
    const u32 linear[2][4] = {{0xff000000u, 0xff3f3f3fu, 0xffbdbdbdu, 0xfffcfcfcu},
                              {0xff000000u, 0xff3f3f3fu, 0xffbdbdbdu, 0xfffcfcfcu}};
    for (u32 filter = 0; filter < 2; filter++) {
        NvnPCopyT2T copy = {.src = snap,
                            .dst = snap2,
                            .sr = {0, 0, 0, 2, 2, 1},
                            .dr = {0, 0, 0, 4, 2, 1},
                            .flags = filter ? NVN_COPY_FLAGS_LINEAR_FILTER_BIT : 0};
        memcpy(GP(record), &copy, sizeof copy);
        CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
        CHECK(!nvn_copy_check_pixels(&destination, filter ? &linear[0][0] : &nearest[0][0], 4, 2));
    }
    CHECK(!nvn_copy_volume_flags(gpu, cpu));
    puts("PASS NVN copy flags: buffer rows, image mirrors and explicit nearest/linear sampling exact=1");
    return 0;
}
