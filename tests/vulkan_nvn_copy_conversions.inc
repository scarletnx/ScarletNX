/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Independent generated texels through NVN uploads, conversion/transformation and downloads. Compare every row,
   slice and padding byte. Rendered depth/stencil inputs use separate aspect readbacks, not matching upload paths. */
#include <math.h>
static int nvn_conversion_volume(u64 gpu, u64 cpu, int floating) {
    enum { W = 3, H = 2, D = 3, DW = 6, DH = 4, PITCH = 64 };
    const u64 snap = cpu + 0x100, snap2 = cpu + 0x200, record = cpu + 0x600;
    const u64 input = gpu + 0x1000, output = gpu + 0xc000;
    NvnTexture source = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu + 0x4000};
    source.d = (NvnTexDesc){.target = NVN_TEXTURE_TARGET_3D,
                            .format = floating ? NVN_FORMAT_RGBA32F : NVN_FORMAT_RGBA8,
                            .width = W,
                            .height = H,
                            .depth = D,
                            .levels = 1};
    NvnTexture destination = source;
    destination.gpu = gpu + 0x8000;
    destination.d.format = floating ? NVN_FORMAT_RGBA16F : NVN_FORMAT_RGBA16;
    destination.d.width = DW;
    destination.d.height = DH;
    memcpy(GP(snap), &source, NVN_TEX_SNAP);
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    const u32 floats[5] = {0, 0x3e800000, 0x3f000000, 0x3f800000, 0x40000000};
    const u16 halves[5] = {0, 0x3400, 0x3800, 0x3c00, 0x4000};
    u8 pixels[W * H * D * 16], expected[PITCH * DH * D];
    u32 input_bpp = floating ? 16 : 4;
    for (u32 i = 0; i < W * H * D; i++)
        for (u32 c = 0; c < 4; c++) {
            if (floating)
                snx_st_le32(pixels + i * input_bpp + c * 4, floats[(i + c) % 5]);
            else
                pixels[i * input_bpp + c] = (u8)(i * 37 + c * 53);
        }
    CHECK(!gmem_write(input, pixels, W * H * D * input_bpp));
    i64 stride[2] = {0};
    NvnPCopyB2T buffer = {.buffer = input, .texture = snap, .r = {0, 0, 0, W, H, D}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    NvnPCopyT2T copy = {.src = snap,
                        .dst = snap2,
                        .sr = {0, 0, 0, W, H, D},
                        .dr = {0, 0, 0, DW, DH, D},
                        .flags =
                            NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT};
    memcpy(GP(record), &copy, sizeof copy);
    CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
    memset(expected, 0xcd, sizeof expected);
    for (u32 z = 0; z < D; z++)
        for (u32 y = 0; y < DH; y++)
            for (u32 x = 0; x < DW; x++) {
                u32 source_index = ((D - 1 - z) * H + H - 1 - y / 2) * W + W - 1 - x / 2;
                for (u32 c = 0; c < 4; c++) {
                    u16 value =
                        floating ? halves[(source_index + c) % 5] : (u16)((u8)(source_index * 37 + c * 53) * 257u);
                    snx_st_le16(expected + (z * DH + y) * PITCH + x * 8 + c * 2, value);
                }
            }
    u8 sentinels[sizeof expected];
    memset(sentinels, 0xcd, sizeof sentinels);
    CHECK(!gmem_write(output, sentinels, sizeof sentinels));
    stride[0] = PITCH;
    buffer = (NvnPCopyB2T){.buffer = output, .texture = snap2, .r = {0, 0, 0, DW, DH, D}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_T2B, record, sizeof buffer, stride));
    vk_lock();
    vk_finish();
    vk_unlock();
    guest_done(cpu + 0xc000, sizeof expected);
    if (memcmp(GP(cpu + 0xc000), expected, sizeof expected)) {
        const u8* actual = GP(cpu + 0xc000);
        for (u32 i = 0; i < sizeof expected; i++)
            if (actual[i] != expected[i]) {
                fprintf(stderr, "NVN volume conversion float=%d byte%u: %02x expected %02x\n", floating, i, actual[i],
                        expected[i]);
                break;
            }
        CHECK(0);
    }
    return 0;
}

static int nvn_conversion_packed(u64 gpu, u64 cpu, int convert) {
    enum { W = 5, H = 3, DW = 7, DH = 5, PITCH = 32 };
    const u64 snap = cpu + 0x100, snap2 = cpu + 0x200, record = cpu + 0x600;
    const u64 input = gpu + 0x1000, initial = gpu + 0x2000, output = gpu + 0xc000;
    NvnTexture source = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu + 0x4000};
    source.d = (NvnTexDesc){.target = NVN_TEXTURE_TARGET_2D,
                            .format = convert ? NVN_FORMAT_RGB5A1 : NVN_FORMAT_RGBA4,
                            .width = W,
                            .height = H,
                            .depth = 1,
                            .levels = 1};
    NvnTexture destination = source;
    destination.gpu = gpu + 0x8000;
    destination.d.format = convert ? NVN_FORMAT_RGB565 : NVN_FORMAT_RGBA4;
    destination.d.width = DW;
    destination.d.height = DH;
    memcpy(GP(snap), &source, NVN_TEX_SNAP);
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    u8 pixels[W * H * 2], untouched[DW * DH * 2], expected[PITCH * DH];
    memset(untouched, 0xa5, sizeof untouched);
    for (u32 i = 0; i < W * H; i++)
        snx_st_le16(pixels + i * 2,
                    convert ? (i % 32) | ((i * 3 % 32) << 5) | ((i * 7 % 32) << 10) | 0x8000 : 0x1000u + i * 157u);
    CHECK(!gmem_write(input, pixels, sizeof pixels));
    CHECK(!gmem_write(initial, untouched, sizeof untouched));
    i64 stride[2] = {0};
    NvnPCopyB2T buffer = {.buffer = input, .texture = snap, .r = {0, 0, 0, W, H, 1}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    buffer = (NvnPCopyB2T){.buffer = initial, .texture = snap2, .r = {0, 0, 0, DW, DH, 1}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    NvnPCopyT2T copy = {.src = snap,
                        .dst = snap2,
                        .sr = {0, 0, 0, W, H, 1},
                        .dr = {1, 1, 0, W, H, 1},
                        .flags = NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT};
    memcpy(GP(record), &copy, sizeof copy);
    CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
    memset(expected, 0xcd, sizeof expected);
    for (u32 y = 0; y < DH; y++)
        for (u32 x = 0; x < DW; x++) {
            u16 value = 0xa5a5;
            if (x >= 1 && x <= W && y >= 1 && y <= H) {
                u32 index = (H - y) * W + W - x;
                value = snx_ld_le16(pixels + index * 2);
                if (convert)
                    value =
                        (value & 31u) | ((((value >> 5) & 31u) * 63u + 15u) / 31u << 5) | (((value >> 10) & 31u) << 11);
            }
            snx_st_le16(expected + y * PITCH + x * 2, value);
        }
    u8 sentinels[sizeof expected];
    memset(sentinels, 0xcd, sizeof sentinels);
    CHECK(!gmem_write(output, sentinels, sizeof sentinels));
    stride[0] = PITCH;
    buffer = (NvnPCopyB2T){.buffer = output, .texture = snap2, .r = {0, 0, 0, DW, DH, 1}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_T2B, record, sizeof buffer, stride));
    vk_lock();
    vk_finish();
    vk_unlock();
    guest_done(cpu + 0xc000, sizeof expected);
    CHECK(!memcmp(GP(cpu + 0xc000), expected, sizeof expected));
    return 0;
}

static int nvn_conversion_depth(u64 gpu, u64 cpu, u32 width, u32 scale, u32 offset) {
    enum { H = 4 };
    u32 W = width;
    const u64 snap = cpu + 0x100, snap2 = cpu + 0x200, record = cpu + 0x600;
    NvnTexture source = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu + 0x4000 + offset};
    source.d = (NvnTexDesc){.target = NVN_TEXTURE_TARGET_2D,
                            .format = NVN_FORMAT_DEPTH32F_STENCIL8,
                            .width = W,
                            .height = H,
                            .depth = 1,
                            .levels = 1};
    NvnTexture destination = source;
    destination.gpu = gpu + 0x8000 + offset;
    memcpy(GP(snap), &source, NVN_TEX_SNAP);
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    DsState* state = calloc(1, sizeof *state);
    CHECK(state);
    DrawOwner owner = {0};
    u32 samples;
    int is_depth;
    CHECK(nvn_draw_surface(snap, NULL, &state->tgt.depth, &is_depth, &samples) && is_depth);
    state->dyn.clip_height = H;
    state->vp.scissor_on = 1;
    const float depth[4] = {0.125f, 0.375f, 0.625f, 0.875f};
    const u32 stencil[4] = {0x11, 0x33, 0x55, 0x77};
    vk_lock();
    float previous_scale = g_vk_scale;
    g_vk_scale = (float)scale;
    for (u32 i = 0; i < 4; i++) {
        u32 x = (i & 1) * (W / 2), y = (i / 2) * (H / 2);
        state->vp.scissor_x[0] = x | (x + W / 2) << 16;
        state->vp.scissor_y[0] = y | (y + H / 2) << 16;
        DrawClear clear = {.aspects = 3, .depth = depth[i], .stencil = stencil[i], .scissored = 1};
        vk_render_clear(state, &clear, &owner);
    }
    vk_render_end();
    vk_unlock();
    NvnPCopyT2T copy = {.src = snap,
                        .dst = snap2,
                        .sr = {0, 0, 0, W, H, 1},
                        .dr = {0, 0, 0, W, H, 1},
                        .flags = NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT};
    i64 stride[2] = {0};
    memcpy(GP(record), &copy, sizeof copy);
    CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
    vk_lock();
    Surf* image = vk_rt_find(destination.gpu);
    CHECK(image && image->f.depth && image->f.stencil);
    u32 physical_width = W * scale, physical_height = H * scale, pixels = physical_width * physical_height;
    CHECK(image->sw >= physical_width && image->sh == physical_height);
    VkBuffer buffer;
    u8* actual = vk_readback_buffer(pixels * 5, &buffer);
    CHECK(actual);
    VkBufferImageCopy regions[2] = {
        {.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, .imageExtent = {physical_width, physical_height, 1}},
        {.bufferOffset = pixels * 4,
         .imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1},
         .imageExtent = {physical_width, physical_height, 1}},
    };
    vk_surf_use(vk_cmd(), image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    rec_CmdCopyImageToBuffer(vk_cmd(), image->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 2, regions);
    vk_finish();
    vk_readback_ready();
    for (u32 y = 0; y < physical_height; y++)
        for (u32 x = 0; x < physical_width; x++) {
            u32 quadrant = (1 - y / (physical_height / 2)) * 2 + 1 - x / (physical_width / 2), expected;
            memcpy(&expected, depth + quadrant, sizeof expected);
            CHECK(snx_ld_le32(actual + (y * physical_width + x) * 4) == expected);
            CHECK(actual[pixels * 4 + y * physical_width + x] == stencil[quadrant]);
        }
    g_vk_scale = previous_scale;
    vk_unlock();
    free(state);
    return 0;
}

/* sRGB's transfer is approximate on a shader device: allow 1e-6 in the linear float and one encoded UNORM code.
   The alpha, untouched row padding and integer source selection remain exact. */
static int nvn_conversion_srgb(u64 gpu, u64 cpu, int encode) {
    enum { W = 16, H = 4, D = 4, PIXELS = W * H * D, MAX_PITCH = W * 16 + 16 };
    const u64 snap = cpu + 0x100, snap2 = cpu + 0x200, record = cpu + 0x600;
    const u64 input = gpu + 0x1000, output = gpu + 0xc000;
    u32 source_bpp = encode ? 16 : 4, destination_bpp = encode ? 4 : 16, pitch = W * destination_bpp + 16;
    NvnTexture source = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu + 0x4000};
    source.d = (NvnTexDesc){.target = NVN_TEXTURE_TARGET_3D,
                            .format = encode ? NVN_FORMAT_RGBA32F : NVN_FORMAT_RGBA8_SRGB,
                            .width = W,
                            .height = H,
                            .depth = D,
                            .levels = 1};
    NvnTexture destination = source;
    destination.gpu = gpu + 0x8000;
    destination.d.format = encode ? NVN_FORMAT_RGBA8_SRGB : NVN_FORMAT_RGBA32F;
    memcpy(GP(snap), &source, NVN_TEX_SNAP);
    memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
    u8 pixels[PIXELS * 16], sentinels[MAX_PITCH * H * D];
    for (u32 i = 0; i < PIXELS; i++)
        for (u32 c = 0; c < 4; c++) {
            u8 value = (u8)(i + c * 73);
            if (encode) {
                double encoded = (double)value / 255.0;
                float linear = (float)(c == 3               ? encoded
                                       : encoded <= 0.04045 ? encoded / 12.92
                                                            : pow((encoded + 0.055) / 1.055, 2.4));
                u32 bits;
                memcpy(&bits, &linear, sizeof bits);
                snx_st_le32(pixels + i * source_bpp + c * 4, bits);
            } else
                pixels[i * source_bpp + c] = value;
        }
    CHECK(!gmem_write(input, pixels, PIXELS * source_bpp));
    memset(sentinels, 0xcd, sizeof sentinels);
    CHECK(!gmem_write(output, sentinels, pitch * H * D));
    i64 stride[2] = {0};
    NvnPCopyB2T buffer = {.buffer = input, .texture = snap, .r = {0, 0, 0, W, H, D}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    NvnPCopyT2T copy = {.src = snap,
                        .dst = snap2,
                        .sr = {0, 0, 0, W, H, D},
                        .dr = {0, 0, 0, W, H, D},
                        .flags =
                            NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT | NVN_COPY_FLAGS_MIRROR_Z_BIT};
    memcpy(GP(record), &copy, sizeof copy);
    CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
    stride[0] = pitch;
    buffer = (NvnPCopyB2T){.buffer = output, .texture = snap2, .r = {0, 0, 0, W, H, D}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_T2B, record, sizeof buffer, stride));
    vk_lock();
    vk_finish();
    vk_unlock();
    guest_done(cpu + 0xc000, pitch * H * D);
    const u8* actual = GP(cpu + 0xc000);
    for (u32 z = 0; z < D; z++)
        for (u32 y = 0; y < H; y++) {
            const u8* row = actual + (z * H + y) * pitch;
            for (u32 x = 0; x < W; x++) {
                u32 source_index = ((D - 1 - z) * H + H - 1 - y) * W + W - 1 - x;
                for (u32 c = 0; c < 4; c++) {
                    u8 value = (u8)(source_index + c * 73);
                    if (encode)
                        CHECK(abs((int)row[x * 4 + c] - value) <= (c == 3 ? 0 : 1));
                    else {
                        double encoded = (double)value / 255.0;
                        double expected = c == 3               ? encoded
                                          : encoded <= 0.04045 ? encoded / 12.92
                                                               : pow((encoded + 0.055) / 1.055, 2.4);
                        u32 bits = snx_ld_le32(row + x * 16 + c * 4);
                        float result;
                        memcpy(&result, &bits, sizeof result);
                        CHECK(fabs((double)result - expected) <= 1e-6);
                    }
                }
            }
            for (u32 byte = W * destination_bpp; byte < pitch; byte++) CHECK(row[byte] == 0xcd);
        }
    return 0;
}

static int nvn_conversion_packed_linear(u64 gpu, u64 cpu) {
    enum { W = 2, H = 2, DW = 6, DH = 6, PITCH = 32 };
    const u64 snap = cpu + 0x100, snap2 = cpu + 0x200, record = cpu + 0x600;
    const u64 input = gpu + 0x1000, initial = gpu + 0x2000, output = gpu + 0xc000;
    NvnTexture source = {.magic = NVN_MAGIC('T', 'E', 'X', 0), .gpu = gpu + 0x4000};
    source.d = (NvnTexDesc){
        .target = NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGB5A1, .width = W, .height = H, .depth = 1, .levels = 1};
    const u16 packed[4] = {31, (31u << 5) | 0x8000u, (31u << 10) | 0x8000u, 0x7fffu};
    const double colors[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 0}};
    u8 pixels[sizeof packed], untouched[DW * DH * 2], sentinels[PITCH * DH], expected[PITCH * DH];
    for (u32 i = 0; i < 4; i++) snx_st_le16(pixels + i * 2, packed[i]);
    memset(untouched, 0xa5, sizeof untouched);
    memset(sentinels, 0xcd, sizeof sentinels);
    memcpy(GP(snap), &source, NVN_TEX_SNAP);
    CHECK(!gmem_write(input, pixels, sizeof pixels));
    CHECK(!gmem_write(initial, untouched, sizeof untouched));
    i64 stride[2] = {0};
    NvnPCopyB2T buffer = {.buffer = input, .texture = snap, .r = {0, 0, 0, W, H, 1}};
    memcpy(GP(record), &buffer, sizeof buffer);
    CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
    for (u32 conversion = 0; conversion < 2; conversion++)
        for (u32 flags = 0; flags < 4; flags++) {
            NvnTexture destination = source;
            destination.gpu = gpu + 0x8000;
            destination.d.width = DW;
            destination.d.height = DH;
            destination.d.format = conversion ? NVN_FORMAT_RGB565 : NVN_FORMAT_RGB5A1;
            memcpy(GP(snap2), &destination, NVN_TEX_SNAP);
            stride[0] = stride[1] = 0;
            buffer = (NvnPCopyB2T){.buffer = initial, .texture = snap2, .r = {0, 0, 0, DW, DH, 1}};
            memcpy(GP(record), &buffer, sizeof buffer);
            CHECK(nvn_copy_record(OP_COPY_B2T, record, sizeof buffer, stride));
            NvnPCopyT2T copy = {.src = snap,
                                .dst = snap2,
                                .sr = {0, 0, 0, W, H, 1},
                                .dr = {1, 1, 0, 4, 4, 1},
                                .flags = NVN_COPY_FLAGS_LINEAR_FILTER_BIT |
                                         (flags & 1 ? NVN_COPY_FLAGS_MIRROR_X_BIT : 0) |
                                         (flags & 2 ? NVN_COPY_FLAGS_MIRROR_Y_BIT : 0)};
            memcpy(GP(record), &copy, sizeof copy);
            CHECK(nvn_copy_record(OP_COPY_T2T, record, sizeof copy, stride));
            memset(expected, 0xcd, sizeof expected);
            for (u32 y = 0; y < DH; y++)
                for (u32 x = 0; x < DW; x++) {
                    u16 value = 0xa5a5;
                    if (x >= 1 && x <= 4 && y >= 1 && y <= 4) {
                        double tx = ((double)x - 0.5) * 0.5 - 0.5;
                        double ty = ((double)y - 0.5) * 0.5 - 0.5;
                        if (flags & 1) tx = 1 - tx;
                        if (flags & 2) ty = 1 - ty;
                        tx = tx < 0 ? 0 : tx > 1 ? 1 : tx;
                        ty = ty < 0 ? 0 : ty > 1 ? 1 : ty;
                        value = 0;
                        for (u32 c = 0; c < (conversion ? 3u : 4u); c++) {
                            double color = colors[0][c] * (1 - tx) * (1 - ty) + colors[1][c] * tx * (1 - ty) +
                                           colors[2][c] * (1 - tx) * ty + colors[3][c] * tx * ty;
                            u32 bits = conversion ? (c == 1 ? 6 : 5) : (c == 3 ? 1 : 5), maximum = (1u << bits) - 1;
                            double scaled = color * maximum;
                            u32 low = (u32)scaled;
                            double remainder = scaled - low;
                            u32 rounded = low + (u32)(remainder > 0.5 || (remainder == 0.5 && (low & 1)));
                            u32 shift = conversion ? (c == 0 ? 0 : c == 1 ? 5 : 11) : c * 5;
                            value |= (u16)(rounded << shift);
                        }
                    }
                    snx_st_le16(expected + y * PITCH + x * 2, value);
                }
            CHECK(!gmem_write(output, sentinels, sizeof sentinels));
            stride[0] = PITCH;
            buffer = (NvnPCopyB2T){.buffer = output, .texture = snap2, .r = {0, 0, 0, DW, DH, 1}};
            memcpy(GP(record), &buffer, sizeof buffer);
            CHECK(nvn_copy_record(OP_COPY_T2B, record, sizeof buffer, stride));
            vk_lock();
            vk_finish();
            vk_unlock();
            guest_done(cpu + 0xc000, sizeof expected);
            CHECK(!memcmp(GP(cpu + 0xc000), expected, sizeof expected));
        }
    return 0;
}

static int check_nvn_copy_conversions(void) {
    const u64 gpu = 0x4e8000000ull, cpu = HEAP_BASE + 0x760000;
    gmmu_map(gpu, cpu, 0x10000);
    for (u32 kind = 0; kind < 2; kind++) {
        CHECK(!nvn_conversion_volume(gpu, cpu, (int)kind));
        CHECK(!nvn_conversion_packed(gpu, cpu, (int)kind));
        CHECK(!nvn_conversion_srgb(gpu, cpu, (int)kind));
    }
    CHECK(!nvn_conversion_depth(gpu, cpu, 8, 1, 0));
    CHECK(!nvn_conversion_depth(gpu, cpu, 6, 1, 0x1000));
    CHECK(!nvn_conversion_depth(gpu, cpu, 6, 2, 0x2000));
    CHECK(!nvn_conversion_packed_linear(gpu, cpu));
    puts(
        "PASS NVN copy conversions: volume float/normalized formats, packed texels, depth/stencil mirrors and padding exact=1");
    return 0;
}
