/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Guest memory the renderer reads with its lock held, on a page the GPU owns bytes of (a result kept on the GPU, as a
   dispatch's storage buffer is: compute.c res_publish). The CPU touching a trapped page there can't be answered and ends
   the process (memory/own.c own_fault): a read takes the owned bytes from their owner on the GPU, the CPU's on such a
   page through the pool's mirror, and lands nothing. No extracted shaders or game data are used */

/* A draw's constant buffer. Two buffers: one whose bound bytes hold an owner's 16, one with an owner on its page past
   the bytes the draw reads. A vertex program stores c[0][0x100] where the descriptor at c[0][0x130] points */
#define CBOWN_GVA 0x8da000000ull
#define CBOWN_CPU (HEAP_BASE + 0x11f00000)

/* the vertex program at the code's base: c[0][0x100] stored through c[0][0x130]'s descriptor, one point at the origin */
static int cbown_program(void) {
    const u64 ops[] = {
        0x4C98078004C70006ull,       /* MOV R6, c[0x0][0x130] */
        0x4C98078004D70007ull,       /* MOV R7, c[0x0][0x134] */
        0x4C9807800407000Aull,       /* MOV R10, c[0x0][0x100] */
        0xEEDC20000007060Aull,       /* STG.E [R6], R10 */
        storage_mov(16, 0),          /* the position: x */
        storage_mov(17, 0),          /* y */
        storage_mov(18, 0),          /* z */
        storage_mov(19, 0x3f800000), /* w */
        0xEFF1FF800707FF10ull,       /* AST a[0x70], R16..R19 */
        0xE30000000007000Full,       /* EXIT */
    };
    return mesh_program(CBOWN_GVA, ops, sizeof ops / sizeof *ops, 0);
}

/* a draw reading the buffer at cb (an offset in the mapped area), whose word at 0x100 the CPU wrote as cpu_word, with
   an owner of 16 bytes of fill at own: 0 when it stored want, the owner is still there and nothing landed */
static int cbown_case(GpuChannel* channel, u32 cb, u32 own, u32 cpu_word, u8 fill, u32 want, const char* what) {
    const u64 result = CBOWN_GVA + 0x5000;
    const u32 desc[3] = {(u32)result, (u32)(result >> 32), 4};
    u8 bytes[4] = {0};
    CHECK(!gmem_write(result, bytes, sizeof bytes));
    CHECK(!storage_write_words(CBOWN_GVA + cb + 0x100, &cpu_word, 1));
    CHECK(!storage_write_words(CBOWN_GVA + cb + 0x130, desc, 3));
    channel->ds.bind.cb_addr[0][0] = CBOWN_GVA + cb;
    channel->ds.bind.cb_size[0][0] = 0x200;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    vk_lock();
    VkOwnRes* owner = vk_own_res_new(16);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 16, fill));
    vk_own_publish_owned(CBOWN_CPU + own, 16, owner, 0);
    CHECK(own_lookup(CBOWN_CPU + own, 16, NULL, 0) == 1);
    u64 lands = own_landings(), drawn = g_vk_drawn, skipped = g_vk_skipped;
    DrawCall call = {.topology = 0, .count = 1, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_finish();
    int drew = g_vk_drawn == drawn + 1 && g_vk_skipped == skipped;
    int kept = own_lookup(CBOWN_CPU + own, 16, NULL, 0) == 1, landed = own_landings() != lands;
    vk_unlock();
    CHECK(!gmem_read(result, bytes, sizeof bytes));
    u32 got = snx_ld_le32(bytes);
    if (!drew || got != want || !kept || landed)
        fprintf(stderr, "constant buffer, %s: drawn %d, stored %08x (expected %08x), owner %s, %s\n", what, drew, got,
                want, kept ? "kept" : "gone", landed ? "landed" : "nothing landed");
    CHECK(drew && got == want && kept && !landed);
    vk_lock();
    own_acquire(CBOWN_CPU + own, 16);
    vk_own_res_unref(owner);
    vk_unlock();
    return 0;
}

static int owned_constant_buffers(void) {
    if (!g_vk.feat.vertexPipelineStoresAndAtomics) {
        puts("owned page reads: no constant buffer case, as the device has no stores in vertex programs");
        return 0;
    }
    const u64 fragment[] = {0xE30000000007000Full};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    CHECK(!mem_map(CBOWN_CPU, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(CBOWN_GVA, CBOWN_CPU, 0x10000);
    CHECK(cbown_program());
    CHECK(mesh_program(CBOWN_GVA + 0x2000, fragment, 1, 1));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 154;
    u32* r = channel->r3d;
    r[0x582] = (u32)(CBOWN_GVA >> 32);
    r[0x583] = (u32)CBOWN_GVA;
    r[0x850] = 1;
    r[0x851] = 0x2000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 0;
    memcpy(r + 0x280, transform, sizeof transform);
    channel->ds.owner = channel->id;
    CHECK(!cbown_case(channel, 0x3000, 0x3100, 0x11111111, 0x5a, 0x5a5a5a5a, "an owner in the bytes it binds"));
    CHECK(!cbown_case(channel, 0x6000, 0x6200, 0x22222222, 0x6b, 0x22222222, "an owner past them on their page"));
    channel->ds.bind.cb_addr[0][0] = 0;
    channel->ds.bind.cb_size[0][0] = 0;
    free(channel);
    puts("owned page reads: a draw's constant buffers took an owner's bytes and the CPU's beside another");
    return 0;
}

static int check_owned_page_reads(void) {
    if (!own_traps()) {
        puts("SKIP owned page reads: the host has no memory traps, so nothing is owned");
        return 0;
    }
    CHECK(!owned_constant_buffers());
    printf("PASS owned page reads, with %s guest buffers: nothing landed\n", vk_uma() ? "unified" : "copied");
    return 0;
}
