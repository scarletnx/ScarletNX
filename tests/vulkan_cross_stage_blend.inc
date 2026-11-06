/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/gpu/nvn/nvn_blend.h"

/* TES/GS resolve their texture sets after the fragment set. Snapshotting RT0 there must not leave the ordered
   fragment blend image in TRANSFER_SRC layout, and each draw must get the target's newest contents. */
static int cross_stage_snapshot_program(u64 address, int stage) {
    u64 ops[32];
    u32 count = 0;
    ops[count++] = storage_mov(4, 1);
    ops[count++] = storage_mov(5, 1);
    ops[count++] = 0xdc00000000000000ull | 7ull << 51 | 74ull << 36 | 15ull << 31 | 2ull << 28 | 255ull << 20 |
                   7ull << 16 | 4ull << 8 | 8; /* TLD R8..R11, R4, RZ,2D */
    if (stage == ST_GS) {
        for (u32 vertex = 0; vertex < 3; vertex++) {
            ops[count++] = storage_mov(4, vertex);
            ops[count++] = 0xefd982000707ff00ull; /* ALD position of vertex R4 */
            ops[count++] = 0xeff1ff800707ff00ull;
            ops[count++] = 0xeff1ff800807ff08ull; /* AST generic color, R8..R11 */
            ops[count++] = 0xfbe000800ff70c0cull;
        }
        ops[count++] = 0xfbe001000ff70c0cull;
    } else {
        ops[count++] = 0xefd800000007ff00ull | 255ull << 39 | 0x2f0ull << 20 | 1ull << 47;
        for (u32 axis = 0; axis < 2; axis++) {
            ops[count++] = 0x1e00000000070000ull | 0x40800000ull << 20 | (u64)axis << 8 | axis; /*4*t*/
            ops[count++] = 0x0800000000070000ull | 0xbf800000ull << 20 | (u64)axis << 8 | axis; /*-1*/
        }
        ops[count++] = storage_mov(2, 0);
        ops[count++] = storage_mov(3, 0x3f800000);
        ops[count++] = 0xeff1ff800707ff00ull;
        ops[count++] = 0xeff1ff800807ff08ull;
    }
    ops[count++] = 0xe30000000007000full;
    u64 code[64] = {0};
    if (stage == ST_GS) {
        code[1] = 1u << 24 | 7ull << 56;
        code[2] = 3;
    }
    u32 at = 10;
    for (u32 i = 0; i < count; i++) {
        if (!(i % 3)) code[at++] = 0x001f8000fc0007e0ull;
        code[at++] = ops[i];
    }
    return invalidate_write_code(address, code, sizeof code / sizeof *code);
}

static int check_cross_stage_blend_snapshots(void) {
    if (!g_vk.has_fragment_interlock || !g_vk.feat.fragmentStoresAndAtomics || g_vk_stage_sets < 5 ||
        !g_vk.feat.shaderStorageImageReadWithoutFormat || !g_vk.feat.shaderStorageImageWriteWithoutFormat ||
        !g_vk.image_slots[ST_FS]) {
        puts("SKIP cross-stage blend snapshots: ordered fragment blending or stage descriptor sets unavailable");
        return 0;
    }
    const u64 gpu = 0x800000000ull, cpu = HEAP_BASE + 0x10700000;
    const u64 stream = gpu + 0x4000, pool = gpu + 0x5000, samplers = pool + 0x100;
    const u64 constants = gpu + 0x6000, target = gpu + 0x8000;
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x10000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 115;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const float positions[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    u64 fragment[5];
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(stage_image_program(gpu + 0x1000, ST_TCS, 0));
    CHECK(cross_stage_snapshot_program(gpu + 0x1800, ST_TES));
    CHECK(cross_stage_snapshot_program(gpu + 0x2000, ST_GS));
    CHECK(mesh_program(gpu + 0x3000, fragment, sizeof fragment / sizeof *fragment, 1));
    CHECK(!gmem_write(stream, positions, sizeof positions));
    CHECK(!storage_write_words(constants + 0x128, (const u32[1]){0}, 1));
    CHECK(!storage_write_words(samplers, (const u32[8]){0}, 8));
    NvnTexture texture = {.gpu = target};
    texture.d.target = NVN_TEXTURE_TARGET_2D;
    texture.d.format = NVN_FORMAT_RGBA32F;
    texture.d.width = texture.d.height = 4;
    texture.d.depth = texture.d.levels = 1;
    u32 tic[8];
    CHECK(nvn_tic_encode(&texture, NULL, 0, 0, tic));
    CHECK(!storage_write_words(pool, tic, 8));
    g_renderer->method_barrier(5);
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32), r[0x583] = (u32)gpu;
    r[0x55d] = (u32)(pool >> 32), r[0x55e] = (u32)pool, r[0x55f] = 0;
    r[0x557] = (u32)(samplers >> 32), r[0x558] = (u32)samplers, r[0x559] = 0;
    r[0x982] = 2;
    r[0x810] = r[0x850] = 1;
    r[0x851] = 0x3000;
    r[0x821] = 0x1000, r[0x831] = 0x1800, r[0x841] = 0x2000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(stream >> 32), r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof positions - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof positions - 1);
    channel->ds.bind.cb_addr[ST_GS][2] = channel->ds.bind.cb_addr[ST_TES][2] = constants;
    channel->ds.bind.cb_size[ST_GS][2] = channel->ds.bind.cb_size[ST_TES][2] = 0x200;
    u32 native = g_vk.advanced_blend_caps.available, tested = 0;
    g_vk.advanced_blend_caps.available = 0;
    for (int stage = ST_TES; stage <= ST_GS; stage++) {
        if (stage == ST_TES && !g_vk.feat.tessellationShader) continue;
        if (stage == ST_GS && !g_vk.feat.geometryShader) continue;
        int tessellation = stage == ST_TES;
        r[0x820] = r[0x830] = tessellation;
        r[0x840] = !tessellation;
        r[0xc8] = 0x301; /* triangle domain and triangle output */
        r[0x373] = 1;
        vk_lock();
        rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        DrawClear clear = {.color_mask = 15};
        memcpy(clear.color, (const float[4]){0.25f, 0.5f, 0.75f, 1}, sizeof clear.color);
        vk_render_clear(&channel->ds, &clear, &channel->owner);
        channel->ds.blend.en[0] = 1;
        channel->ds.blend.mask[0] = 15;
        channel->ds.gen[DS_BLEND] = ds_generation_next();
        DrawCall call = {.topology = tessellation ? 14 : 4, .count = tessellation ? 1 : 3, .instances = 1};
        call.advanced_blend[0] = nvn_set_field(
            nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV, NVN_BLEND_ADVANCED_MODE_DIFFERENCE), NVN_BLEND_ADV_PMS, 1),
            NVN_BLEND_ADV_NPM, 1);
        for (u32 draw = 0; draw < 2; draw++) {
            u64 before = g_vk_drawn;
            vk_render_draw(&channel->ds, &call, &channel->owner);
            CHECK(g_vk_drawn == before + 1);
        }
        vk_render_end();
        Surf* surface = vk_rt_find(target);
        VkBuffer buffer;
        u8* actual = vk_readback_buffer(16 * 16, &buffer);
        CHECK(surface && actual);
        vk_surf_use(vk_cmd(), surface, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                    VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy region = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
        rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &region);
        vk_finish();
        vk_readback_ready();
        for (u32 pixel = 0; pixel < 16; pixel++)
            for (u32 component = 0; component < 4; component++) {
                u32 expected = component == 3 ? 0x3f800000 : 0;
                if (snx_ld_le32(actual + pixel * 16 + component * 4) != expected)
                    fprintf(stderr, "cross-stage snapshot stage%d pixel%u component%u: %08x expected %08x\n", stage,
                            pixel, component, snx_ld_le32(actual + pixel * 16 + component * 4), expected);
                CHECK(snx_ld_le32(actual + pixel * 16 + component * 4) == expected);
            }
        vk_unlock();
        tested++;
    }
    g_vk.advanced_blend_caps.available = native;
    free(channel);
    printf("PASS cross-stage blend snapshots: %u TES/GS readers and consecutive Difference draws exact=1\n", tested);
    return 0;
}
