/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A draw's texture over a render target that is not the target itself (render.c tex_resolve): a 4x4 header at the
   address of an 8x8 target, sampled through a copy of its corner. The memo keeps it while the target holds the same
   pixels: a draw into the target after the memo kept the texture must give the new texels. The sequence runs as it
   is, and with draw_state_check=1, whose full lookup of each kept texture must find the same one
   (SNX_TEST_NVN_CASE=memo_over_target) */
#define MO_GVA    0xdc0000000ull
#define MO_CPU    (HEAP_BASE + 0x2fc00000)
#define MO_SPAN   0x100000ull
#define MO_TIC    (MO_GVA)
#define MO_TSC    (MO_GVA + 0x1000)
#define MO_SOURCE (MO_GVA + 0x10000) /* the 8x8 target the texture lies over */
#define MO_DEST   (MO_GVA + 0x20000) /* the draws' 4x4 target */
static void mo_target(GpuChannel* ch, u64 gva, u32 side, const float rgba[4]) {
    u32* r = ch->r3d;
    r[0x487] = 1;
    r[0x200] = (u32)(gva >> 32);
    r[0x201] = (u32)gva;
    r[0x202] = side;
    r[0x203] = side;
    r[0x204] = 0xD5;
    r[0x205] = 0;
    r[0x206] = 1;
    r[0x207] = 0;
    memcpy(&r[0x360], rgba, 16);
    engine_clear(ch, 15 << 2);
    vk_lock();
    vk_render_end();
    vk_unlock();
    /* the texture barrier a game puts between drawing a target and sampling it: rt_crop keeps a copy made since the
       last one (g_vk_texture_epoch), as the console's texture cache keeps the old texels until it */
    atomic_fetch_add_explicit(&g_vk_texture_epoch, 1, memory_order_relaxed);
}
/* n DrawTexture draws of the texture's texel (0, 0) into the 4x4 target: its pixel (0, 0), and the textures the memo
   kept in *kept */
static int mo_draws(GpuChannel* ch, u32 n, u32* px, u64* kept) {
    VkDrawCounts c0, c1;
    vk_lock();
    ch->r3d[0x574] = 0;
    rt_clear(ch, MO_DEST, 4, 4, 0, 0);
    ch->r3d_dirty = ~0u;
    mx_draw_state_sync(ch);
    DsState* state = &ch->ds;
    state->bind.tex_pool = MO_TIC;
    state->bind.smp_pool = MO_TSC;
    state->bind.tex_max = state->bind.smp_max = 0;
    state->blend = (DsBlend){0};
    state->blend.mask[0] = 15;
    state->rkey.alpha_to_coverage = state->rkey.alpha_to_one = 0;
    state->rkey.sample_mask = ~0u;
    state->dyn.alpha_test = 0;
    vk_draw_counts(&c0);
    for (u32 i = 0; i < n; i++) {
        DrawTexture rectangle = {.w = 4, .h = 4, .sx0 = 0.5f, .sy0 = 0.5f};
        rectangle.state.multisample_flags = DS_MS_A2C_DITHER;
        vk_render_draw_texture(state, &rectangle);
    }
    vk_draw_counts(&c1);
    vk_render_end();
    Surf* target = vk_rt_find(MO_DEST);
    u8 p[4] = {0};
    int ok = target && !read_pixel_at(target, 0, 0, p);
    vk_unlock();
    *px = snx_ld_le32(p);
    *kept = c1.tex[VK_TEX_KEPT] - c0.tex[VK_TEX_KEPT];
    return ok ? 0 : 1;
}
static int check_memo_over_target(void) {
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    ch->id = 149;
    CHECK(mem_map(MO_CPU, MO_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(MO_GVA, MO_CPU, MO_SPAN);
    NvnTexture tex = {0};
    tex.d.target = NVN_TEXTURE_TARGET_2D;
    tex.d.format = NVN_FORMAT_RGBA8;
    tex.d.levels = 1;
    tex.d.width = tex.d.height = 4;
    tex.d.depth = 1;
    tex.gpu = MO_SOURCE;
    u32 tic[8];
    CHECK(nvn_tic_encode(&tex, NULL, 0, 0, tic));
    CHECK(!storage_write_words(MO_TIC, tic, 8));
    CHECK(!storage_write_words(MO_TSC, (const u32[8]){2u | 2u << 3 | 2u << 6}, 8));
    int saved_check = g_settings.draw_state_check, failed = 0;
    static const float red[4] = {1, 0, 0, 1}, blue[4] = {0, 0, 1, 1}, green[4] = {0, 1, 0, 1};
    for (int run = 0; run < 2; run++) {
        g_settings.draw_state_check = run == 1;
        g_vk_present++;
        u32 px;
        u64 kept;
        mo_target(ch, MO_SOURCE, 8, run == 0 ? green : red);
        CHECK(!mo_draws(ch, 3, &px, &kept));
        const u32 first = run == 0 ? 0xff00ff00u : 0xff0000ffu;
        if (px != first || (run == 0 && kept < 2)) {
            fprintf(stderr, "memo over target%s: pixel %08x (drawn %08x), %llu kept\n",
                    run == 1 ? " with draw_state_check=1" : "", px, first, (unsigned long long)kept);
            failed = 1;
        }
        /* a draw into the target after the memo kept the texture */
        mo_target(ch, MO_SOURCE, 8, blue);
        CHECK(!mo_draws(ch, 2, &px, &kept));
        if (px != 0xffff0000u) {
            fprintf(stderr, "memo over target%s: after a draw into the target the pixel is %08x, drawn ffff0000\n",
                    run == 1 ? " with draw_state_check=1" : "", px);
            failed = 1;
        } else
            printf("memo over target%s: the target's new pixels sampled, %llu lookups kept\n",
                   run == 1 ? " with draw_state_check=1" : "", (unsigned long long)kept);
    }
    g_settings.draw_state_check = saved_check;
    free(ch);
    if (!failed) puts("PASS memo over target: a texture kept over a target takes the target's new draws");
    return failed;
}
