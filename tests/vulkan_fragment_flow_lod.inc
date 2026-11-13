/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A fragment program that samples with the level of detail of its derivatives after its lanes took different ways
   through its flow, on a device whose subgroups leave the fragment stage out (Turnip on an Adreno 610 lists compute
   only): the program is translated as structured flow, where a quad's lanes meet again at the join, and the sample
   takes the level the console takes. Such a program was not translated there, and its draws were skipped.

   A triangle over all of a 4x4 target samples a 16x16 texture of five levels, each texel its level and place
   (texture_limit_value), at (x / 8, y / 8): two texels a pixel, so level 1, whose texel (x, y) lies under pixel
   (x, y). The triangle is half of a viewport twice the target's size, drawn whole: one clipped to the viewport is
   drawn as triangles that meet inside the target, and lavapipe (Mesa 26.2.6) leaves a quad's lanes outside a triangle
   with the values they had when its last lane inside left a loop.

   The x coordinate is too high until the join, and the columns take it down in steps, in two forms: a branch to one
   block in the odd columns and to three in the even ones, and a loop that runs once in the even columns and three
   times in the odd ones. A lane that sampled before its quad's others arrived, two steps ahead of them, would take
   their coordinate still too high, and the smallest level with it. Each form is drawn with the device's subgroups,
   with the fragment stage taken out of them, and then without the device's MaximallyReconvergesKHR too: every pixel
   must hold its texel of level 1. No extracted shaders or game data are used. */

/* the word of a program's op, counted as mesh_program lays them out: a scheduling word before every third */
static u32 flow_lod_word(u32 op) { return 11 + op + op / 3; }
static u64 flow_lod_bra(u32 from, u32 to, u32 predicate) {
    u32 bytes = (flow_lod_word(to) - (flow_lod_word(from) + 1)) * 8;
    return 0xe24000000000000full | (u64)(bytes & 0xffffff) << 20 | (u64)predicate << 16;
}
static u64 flow_lod_add(u32 dst, u32 src, float value, u32 predicate) {
    return 0x0800000000000000ull | (u64)texture_limit_bits(value) << 20 | (u64)predicate << 16 | src << 8 | dst;
}
static u64 flow_lod_scale(u32 dst, u32 src, float value) {
    return 0x1e00000000070000ull | (u64)texture_limit_bits(value) << 20 | src << 8 | dst;
}

/* the program at address: marker makes each pass's program a program of its own */
static int flow_lod_program(u64 address, int loop, u32 marker) {
    u64 ops[32];
    u32 n = 0;
    ops[n++] = 0xe000000000070000ull | 0x1cull << 30 | 8; /* IPA R8, a[0x70]: the fragment's x */
    ops[n++] = 0xe000000000070000ull | 0x1dull << 30 | 9; /* IPA R9, a[0x74]: its y */
    ops[n++] = flow_lod_add(10, 8, -1.0f, 7);
    ops[n++] = flow_lod_add(11, 8, -2.0f, 7);
    ops[n++] = flow_lod_add(12, 8, -3.0f, 7);
    ops[n++] = 0x5c68000000070000ull | 11ull << 20 | 10 << 8 | 10; /* FMUL R10, R10, R11 */
    ops[n++] = 0x5c68000000070000ull | 12ull << 20 | 10 << 8 | 10; /* FMUL R10, R10, R12: above 0 in odd columns */
    ops[n++] = 0x5bb4038000070007ull | 0xffull << 20 | 10 << 8;    /* FSETP.GT P0, R10, RZ */
    ops[n++] = flow_lod_scale(4, 8, 0.125f);                       /* the coordinates: x / 8 */
    ops[n++] = flow_lod_scale(5, 9, 0.125f);                       /* and y / 8 */
    ops[n++] = storage_mov(1, 0);
    ops[n++] = storage_mov(2, 0);
    ops[n++] = storage_mov(3, 0x3f800000);
    ops[n++] = storage_mov(13, marker);
    if (!loop) {
        ops[n++] = flow_lod_add(4, 4, 64.0f, 7);
        u32 branch = n++, odd = branch + 7;
        ops[branch] = flow_lod_bra(branch, odd, 0); /* @P0 BRA: an odd column */
        for (u32 block = 0; block < 3; block++) {   /* an even one: three blocks, each ended by a BRA to the next */
            ops[n++] = flow_lod_add(4, 4, block < 2 ? -16.0f : -32.0f, 7);
            ops[n] = flow_lod_bra(n, block < 2 ? n + 1 : odd + 1, 7); /* the last to the join */
            n++;
        }
        ops[n++] = flow_lod_add(4, 4, -64.0f, 7); /* the odd column's one block, which runs into the join */
    } else {
        ops[n++] = storage_mov(6, 0x3f800000);                 /* the loop's count: 1 */
        ops[n++] = storage_mov(6, 0x40400000) & ~(7ull << 16); /* @P0 MOV32I R6, 3.0: three in an odd column */
        ops[n++] = flow_lod_add(4, 4, 32.0f, 7);
        ops[n++] = flow_lod_add(4, 4, 64.0f, 0); /* @P0 */
        u32 head = n;
        ops[n++] = flow_lod_add(4, 4, -32.0f, 7);
        ops[n++] = flow_lod_add(6, 6, -1.0f, 7);
        ops[n++] = 0x5bb403800007000full | 0xffull << 20 | 6 << 8; /* FSETP.GT P1, R6, RZ */
        ops[n] = flow_lod_bra(n, head, 1);                         /* @P1 BRA back */
        n++;
    }
    /* TEX R0, R4, R5: a 2D texture by the handle at c[2][0x128], the level its derivatives give */
    ops[n++] = 0xc038000000000000ull | 74ull << 36 | 7ull << 51 | 1ull << 31 | 2ull << 28 | 16ull << 20 | 7ull << 16 |
               4ull << 8;
    ops[n++] = 0xe30000000007000full;
    return mesh_program(address, ops, n, 1);
}

/* one draw of the program at offset with stages as the stages that have subgroups: 0 when every pixel holds its
   texel of level 1 */
static int flow_lod_pass(GpuChannel* channel, u64 gpu, u32 offset, int loop, VkShaderStageFlags stages, int reconverges,
                         const char* what) {
    const u64 constants = gpu + 0xb000, target = gpu + 0x40000;
    CHECK(flow_lod_program(gpu + offset, loop, 0x7100 + offset));
    g_renderer->method_barrier(5);
    channel->r3d[0x851] = offset;
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0); /* RGBA32F */
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.cb_addr[ST_FS][2] = constants;
    state->bind.cb_size[ST_FS][2] = 0x200;
    state->blend.en[0] = 0;
    state->blend.mask[0] = 15;
    VkShaderStageFlags device_stages = g_vk_subgroup_stages;
    int device_reconverges = g_shader_maximal_reconvergence;
    g_vk_subgroup_stages = stages;
    g_shader_maximal_reconvergence = reconverges;
    u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
    DrawCall draw = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(state, &draw, &channel->owner);
    vk_render_end();
    g_vk_subgroup_stages = device_stages;
    g_shader_maximal_reconvergence = device_reconverges;
    int drew = g_vk_drawn == drawn + 1 && g_vk_skipped == skipped;
    if (!drew) fprintf(stderr, "fragment flow lod, %s, %s: the draw was skipped\n", loop ? "loop" : "branch", what);
    CHECK(drew);
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
    u32 wrong = 0;
    for (u32 y = 0; y < 4; y++)
        for (u32 x = 0; x < 4; x++) {
            float value, expected = texture_limit_value(1, 0, x, y, 0);
            memcpy(&value, actual + (y * 4 + x) * 16, 4);
            if (value == expected) continue;
            if (!wrong++)
                fprintf(stderr, "fragment flow lod, %s, %s: pixel (%u, %u) sampled %g, expected %g (level 1's texel)\n",
                        loop ? "loop" : "branch", what, x, y, value, expected);
        }
    vk_unlock();
    if (wrong)
        fprintf(stderr, "fragment flow lod, %s, %s: %u of 16 pixels wrong\n", loop ? "loop" : "branch", what, wrong);
    CHECK(!wrong);
    return 0;
}

static int check_fragment_flow_lod(void) {
    const u64 gpu = 0x8e8000000ull, cpu = HEAP_BASE + 0x12800000;
    const u64 stream = gpu + 0x8000, pool = gpu + 0x9000, samplers = gpu + 0xa000, constants = gpu + 0xb000;
    CHECK(mem_map(cpu, 0x80000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x80000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 149;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 1, -1, 0, 1, -1, 1, 0, 1};
    CHECK(!gmem_write(stream, position, sizeof position));
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(!storage_write_words(constants + 0x128, (const u32[1]){0}, 1));
    /* clamped to the edge, the nearest texel of the nearest level, every level allowed */
    CHECK(!storage_write_words(samplers, (const u32[8]){2u | 2u << 3 | 2u << 6, 1u | 1u << 4 | 2u << 6, 0xfffu << 12},
                               8));
    NvnTexture texture = {.gpu = gpu + 0x10000};
    texture.d.target = NVN_TEXTURE_TARGET_2D;
    texture.d.format = NVN_FORMAT_R32F;
    texture.d.width = texture.d.height = 16;
    texture.d.depth = 1;
    texture.d.levels = 5;
    CHECK(!texture_limit_seed(&texture));
    u32 tic[8];
    CHECK(nvn_tic_encode(&texture, NULL, 0, 0, tic));
    CHECK(!storage_write_words(pool, tic, 8));
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32);
    r[0x583] = (u32)gpu;
    r[0x55d] = (u32)(pool >> 32);
    r[0x55e] = (u32)pool;
    r[0x55f] = 0;
    r[0x557] = (u32)(samplers >> 32);
    r[0x558] = (u32)samplers;
    r[0x559] = 0;
    r[0x810] = r[0x850] = 1;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    all_samples(r);
    const float transform[6] = {4, 4, 0.5f, 4, 4, 0.5f}; /* an 8x8 viewport, the target its first quarter */
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 1u << 21 | 7u << 27;
    r[0x700] = 0x1000 | 16;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof position - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof position - 1);
    const VkShaderStageFlags without = g_vk_subgroup_stages & ~(VkShaderStageFlags)VK_SHADER_STAGE_FRAGMENT_BIT;
    int has_fragment = (g_vk_subgroup_stages & VK_SHADER_STAGE_FRAGMENT_BIT) != 0;
    for (int loop = 0; loop < 2; loop++) {
        u32 offset = 0x1000 + (u32)loop * 0x1000;
        CHECK(!flow_lod_pass(channel, gpu, offset, loop, g_vk_subgroup_stages, g_shader_maximal_reconvergence,
                             has_fragment ? "the device's fragment subgroups" : "a device without fragment subgroups"));
        CHECK(!flow_lod_pass(channel, gpu, offset + 0x400, loop, without, g_shader_maximal_reconvergence,
                             "the fragment stage taken out of the subgroups"));
        CHECK(!flow_lod_pass(channel, gpu, offset + 0x800, loop, without, 0,
                             "no fragment subgroups and no MaximallyReconvergesKHR"));
    }
    free(channel);
    printf("PASS fragment flow lod: a sample after a branch and after a loop took level 1 in all 16 pixels, %s, and as "
           "structured flow without them, with MaximallyReconvergesKHR %s and without it\n",
           has_fragment ? "with the device's fragment subgroups" : "on a device without fragment subgroups",
           g_shader_maximal_reconvergence ? "declared" : "not on this device");
    return 0;
}
