/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ordered capture fallback (render.c capture_draw) records a clear, a draw and a resolve for every primitive of
   every instance. With no fragment interlock and no native advanced blending, an advanced-blend draw takes it: a draw
   of up to VK_CAPTURE_PRIMITIVES_MAX primitives is captured and counted; one past it is skipped with a log line naming
   its primitives and the cap. SNX_TEST_CAPTURE_COST=N also times one draw of N primitives through the fallback. */
static int capture_cap_draw(GpuChannel* channel, u32 primitives, u32 instances, u64* ns) {
    DrawCall call = {.topology = 4, .count = primitives * 3, .instances = instances};
    call.advanced_blend[0] = nvn_set_field(
        nvn_set_field(nvn_set_field(0, NVN_BLEND_ADV, NVN_BLEND_ADVANCED_MODE_OVERLAY), NVN_BLEND_ADV_PMS, 1),
        NVN_BLEND_ADV_NPM, 1);
    DsState* state = &channel->ds;
    state->gen[DS_PROGRAMS] = state->gen[DS_TARGETS] = state->gen[DS_TARGET_FORMATS] = state->gen[DS_DYNAMIC] =
        state->gen[DS_BLEND] = state->gen[DS_RASTER_KEY] = ds_generation_next();
    vk_lock();
    vk_finish();
    u64 start = plat_ns();
    vk_render_draw(state, &call, &channel->owner);
    vk_render_end();
    vk_finish();
    if (ns) *ns = plat_ns() - start;
    vk_unlock();
    return 0;
}
static int check_capture_cap(void) {
    if (!g_vk.feat.fragmentStoresAndAtomics || !g_vk.feat.shaderStorageImageReadWithoutFormat ||
        !g_vk.feat.shaderStorageImageWriteWithoutFormat) {
        puts("SKIP capture cap: fragment image writes are unavailable");
        return 0;
    }
    const u64 gpu = 0x8c8000000ull, cpu = HEAP_BASE + 0x11a00000;
    const u64 stream = gpu + 0x2000, target = gpu + 0x8000;
    const u32 streamed = 512; /* triangles the vertex stream holds: the timed draw's most */
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 153;
    const u64 vertex[] = {0xefd9ff800807ff00ull, 0xeff1ff800707ff00ull, 0xe30000000007000full};
    const u32 color[4] = {0x3e000000, 0x3d800000, 0x3e400000, 0x3e800000};
    u64 fragment[5];
    for (u32 component = 0; component < 4; component++)
        fragment[component] = 0x0100000000071000ull | (u64)color[component] << 20 | component; /* MOV32I */
    fragment[4] = 0xe30000000007000full;                                                       /* EXIT */
    CHECK(mesh_program(gpu, vertex, sizeof vertex / sizeof *vertex, 0));
    CHECK(mesh_program(gpu + 0x800, fragment, 5, 1));
    const float triangle[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    for (u32 i = 0; i < streamed; i++) CHECK(!gmem_write(stream + i * sizeof triangle, triangle, sizeof triangle));
    const u64 stream_end = stream + streamed * sizeof triangle - 1;
    u32* r = channel->r3d;
    r[0x582] = (u32)(gpu >> 32), r[0x583] = (u32)gpu;
    r[0x850] = 1, r[0x851] = 0x800;
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
    r[0x7c0] = (u32)(stream_end >> 32), r[0x7c1] = (u32)stream_end;
    vk_lock();
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xc0);
    vk_unlock();
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->prog.early_z = 0;
    state->blend.en[0] = 1;
    state->blend.mask[0] = 15;
    state->tgt.count = 1;
    state->rkey.sample_mask = ~0u;
    /* Neither native advanced blending nor the interlock path: the capture fallback is what is left */
    u32 native = g_vk.advanced_blend_caps.available;
    int interlock = g_vk.has_fragment_interlock;
    g_vk.advanced_blend_caps.available = 0;
    g_vk.has_fragment_interlock = 0;
    FILE* log = tmpfile();
    CHECK(log);
    u64 draws = g_vk_capture_draws, primitives = g_vk_capture_primitives, capped = g_vk_capture_capped;
    u64 drawn = g_vk_drawn, skipped = g_vk_skipped;
    t_plat.log_redirect = log;
    int failed = capture_cap_draw(channel, 2, 1, NULL);
    failed |= capture_cap_draw(channel, VK_CAPTURE_PRIMITIVES_MAX + 1, 1, NULL);
    failed |= capture_cap_draw(channel, VK_CAPTURE_PRIMITIVES_MAX / 2 + 1, 2, NULL);
    t_plat.log_redirect = NULL;
    char text[2048] = {0};
    rewind(log);
    size_t got = fread(text, 1, sizeof text - 1, log);
    fclose(log);
    text[got] = 0;
    const char* cost = getenv("SNX_TEST_CAPTURE_COST");
    u64 ns = 0;
    u32 timed = cost ? (u32)strtoul(cost, NULL, 10) : 0;
    if (timed && timed <= streamed && timed <= VK_CAPTURE_PRIMITIVES_MAX) {
        u64 before = g_vk_capture_primitives;
        failed |= capture_cap_draw(channel, timed, 1, &ns);
        CHECK(g_vk_capture_primitives == before + timed);
        printf("CAPTURE_COST primitives=%u ns=%llu ns_per_primitive=%llu\n", timed, (unsigned long long)ns,
               (unsigned long long)(ns / timed));
    }
    g_vk.advanced_blend_caps.available = native;
    g_vk.has_fragment_interlock = interlock;
    free(channel);
    CHECK(!failed);
    char expected[2][160];
    snprintf(expected[0], sizeof expected[0],
             "a draw of %u primitives (%u per instance, 1 instance) needs the ordered "
             "capture path, which takes %u at most: draw skipped",
             VK_CAPTURE_PRIMITIVES_MAX + 1, VK_CAPTURE_PRIMITIVES_MAX + 1, VK_CAPTURE_PRIMITIVES_MAX);
    snprintf(expected[1], sizeof expected[1],
             "a draw of %u primitives (%u per instance, 2 instances) needs the "
             "ordered capture path, which takes %u at most: draw skipped",
             VK_CAPTURE_PRIMITIVES_MAX + 2, VK_CAPTURE_PRIMITIVES_MAX / 2 + 1, VK_CAPTURE_PRIMITIVES_MAX);
    int said = strstr(text, expected[0]) && strstr(text, expected[1]);
    if (!said) fprintf(stderr, "capture cap log:\n%s", text);
    CHECK(said);
    /* The two-triangle draw is captured and counts as one draw; the two past the cap are skipped as before */
    CHECK(g_vk_capture_draws == draws + 1 + (timed ? 1 : 0));
    CHECK(g_vk_capture_primitives == primitives + 2 + timed);
    CHECK(g_vk_capture_capped == capped + 2);
    CHECK(g_vk_skipped == skipped + 2);
    CHECK(g_vk_drawn == drawn + 1 + (timed ? 1 : 0));
    return 0;
}
