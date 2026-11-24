/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pool map that SM86 programs read (gpu/vk/pool_map_gpu.c, the setting pool_map): Ounce fragment programs fetch
   texel (1, 0) of the texture of a handle in c[2] into a 4x4 RGBA8 target, through the texture table and the map.
   Each case draws as without the map, and the counts tell which way each slot went:
   - a texture: its first draw teaches the map its record, the second takes the slot from the map
   - its header written by the CPU between two draws: the next draw shows the new texture, and the map the one after
   - a handle past the pools' limits: zeros, through the draw's word
   - a draw that fetches the target it draws into: the feedback loop's copy, through the draw's word
   - a texture that crops a target, and the target drawn again: the new pixels, the map's slot still the crop's
   - an ASTC texture held as BC7 later (astc_mode 2): its BC7 image takes the RGBA8 one's place, and the draw after the
     swap takes its word and teaches the map the new view, with texels near the RGBA8 ones
   The renderer starts with the SM86 frontend (SNX_TEST_SM86_TABLE=1), so that it takes the texture table, and with
   --pool-map=1 (SNX_TEST_NVN_CASE=pool_map alone) */
#define PM_GPU 0x8b0000000ull
#define PM_CPU (HEAP_BASE + 0x3f000000)
enum { PM_TARGET = 0x4000, PM_A = 0x5000, PM_B = 0x6000, PM_CROP = 0x7000, PM_POOL = 0x8000, PM_SAMPLERS = 0x9000,
       PM_CONSTANTS = 0xa000 };
enum { PM_HANDLE_TEX = 0x40, PM_HANDLE_PAST = 0x44, PM_HANDLE_SELF = 0x48, PM_HANDLE_CROP = 0x4c, PM_HANDLE_ASTC = 0x50 };

/* the RGBA8 header of a w x h block linear texture at gva */
static void pm_tic(u32 tic[8], u64 gva, u32 w, u32 h) {
    NvnTexture image = {.gpu = gva};
    image.d.target = NVN_TEXTURE_TARGET_2D;
    image.d.format = NVN_FORMAT_RGBA8;
    image.d.width = w;
    image.d.height = h;
    image.d.depth = image.d.levels = 1;
    nvn_tic_encode(&image, NULL, 0, 0, tic);
}
/* an 8x8 RGBA8 texture at gva of texel value v */
static int pm_texture(u64 gva, u32 v) {
    u8 texels[256];
    for (u32 i = 0; i < 64; i++) snx_st_le32(texels + 4 * i, v);
    return gmem_write(gva, texels, sizeof texels);
}
/* the draw of the fragment program at PM_GPU + program into the target, cleared red first: its pixel (0, 0) into px,
   and the counts of the map's ways it added into d */
static int pmg_draw(GpuChannel* channel, u32 program, u8 px[4], VkPoolGpuCounts* d) {
    VkPoolGpuCounts a, b;
    const u64 target = PM_GPU + PM_TARGET;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    u32* r = channel->r3d;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x851] = program;
    rt_clear_format(channel, target, 4, 4, 0, 0, 0xD5);
    vk_lock();
    channel->r3d_dirty = ~0u; /* the registers were set directly */
    mx_draw_state_sync(channel);
    DsState* state = &channel->ds;
    state->bind.tex_pool = PM_GPU + PM_POOL;
    state->bind.smp_pool = PM_GPU + PM_SAMPLERS;
    state->bind.tex_max = 6;
    state->bind.smp_max = 0;
    state->bind.cb_addr[ST_FS][2] = state->bind.cb_addr[ST_VS][2] = PM_GPU + PM_CONSTANTS;
    state->bind.cb_size[ST_FS][2] = state->bind.cb_size[ST_VS][2] = 256;
    state->blend.en[0] = 0;
    state->blend.mask[0] = 15;
    state->dyn.depth_test = state->dyn.depth_write = state->dyn.stencil_test = state->dyn.alpha_test = 0;
    state->rkey.sample_mask = ~0u;
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    const u64 drawn = g_vk_drawn;
    vk_pool_gpu_counts(&a);
    vk_render_draw(state, &call, &channel->owner);
    vk_render_end();
    vk_pool_gpu_counts(&b);
    Surf* surface = vk_rt_find(target);
    const int ok = g_vk_drawn == drawn + 1 && surface && !read_pixel_at(surface, 0, 0, px);
    vk_unlock();
    for (int k = 0; k < VK_POOL_GPU_WHY_N; k++) d->why[k] = b.why[k] - a.why[k];
    d->learned = b.learned - a.learned;
    return ok ? 0 : 1;
}
/* a fragment program at PM_GPU + at, render target 0 from R0-R3: MOV R4, 1; MOV R5, 0; TLD.LZ R0-R3, R4, R5,
   c[2][handle], texel (1, 0) for every fragment (as sf_table_fragment) */
static int pm_fragment(u32 at, u32 handle) {
    Sm86Word code[4];
    code[0] = sm86_mov_imm(4, 1);
    code[1] = sm86_mov_imm(5, 0);
    code[2] = sm86_fix(0xb66);
    sm86_set(&code[2], 16, 8, 0);
    sm86_set(&code[2], 24, 8, 4);
    sm86_set(&code[2], 32, 8, 255);
    sm86_set(&code[2], 40, 19, 2u << 14 | handle >> 2);
    sm86_set(&code[2], 61, 3, 1);
    sm86_set(&code[2], 64, 8, 255);
    sm86_set(&code[2], 72, 4, 0xf);
    sm86_set(&code[2], 87, 3, 1);
    code[3] = sm86_exit();
    return sf_program(PM_GPU + at, 2u | 4u << 5 | 5u << 10, 18, 0xf, code, 4);
}
/* px is v's bytes (RGBA8, R first), each within tolerance */
static int pm_near(const u8 px[4], u32 v, u32 tolerance) {
    for (u32 k = 0; k < 4; k++) {
        const int want = (int)(v >> (8 * k) & 0xff), got = px[k];
        if (got - want > (int)tolerance || want - got > (int)tolerance) return 0;
    }
    return 1;
}
static int pm_case(const char* what, int ok, const u8 px[4], const VkPoolGpuCounts* d) {
    if (!ok)
        fprintf(stderr,
                "pool map, %s: pixel %02x%02x%02x%02x; %llu through the map, %llu missed, %llu past, %llu another view, "
                "%llu no map, %llu learned\n",
                what, px[3], px[2], px[1], px[0], (unsigned long long)d->why[VK_POOL_GPU_MAP],
                (unsigned long long)d->why[VK_POOL_GPU_MISS], (unsigned long long)d->why[VK_POOL_GPU_PAST],
                (unsigned long long)d->why[VK_POOL_GPU_VIEW], (unsigned long long)d->why[VK_POOL_GPU_NONE],
                (unsigned long long)d->learned);
    return ok ? 0 : 1;
}
static int check_pool_map_draws(void) {
    if (!g_spirv_translate.pool_map) {
        printf("SKIP pool map: the renderer reads no pool map (SM86 %d, texture table %d, device addresses %d, unified "
               "layouts %d)\n",
               g_shader_sm86, g_spirv_translate.tex_table, g_spirv_translate.gbuf_address, g_vk.has_unified_layouts);
        return 0;
    }
    GpuChannel* channel = sample_raster_channel(PM_GPU, PM_CPU, 0);
    CHECK(channel);
    if (!channel) return 1;
    CHECK(mem_map(PM_CPU + 0x10000, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(PM_GPU + 0x10000, PM_CPU + 0x10000, 0x10000);
    Sm86Word code[3];
    /* the vertex program: ALD R0-R3, a[0x80]; AST a[0x70], R0-R3 */
    code[0] = sm86_fix(0x321);
    sm86_set(&code[0], 16, 8, 0);
    sm86_set(&code[0], 24, 8, 255);
    sm86_set(&code[0], 32, 8, 255);
    sm86_set(&code[0], 40, 10, 0x80);
    sm86_set(&code[0], 74, 2, 3);
    code[1] = sm86_fix(0x322);
    sm86_set(&code[1], 24, 8, 255);
    sm86_set(&code[1], 32, 8, 0);
    sm86_set(&code[1], 64, 8, 255);
    sm86_set(&code[1], 40, 10, 0x70);
    sm86_set(&code[1], 74, 2, 3);
    code[2] = sm86_exit();
    int failed = sf_program(PM_GPU, 0x000c0481u, 13, 0xfu << 12, code, 3); /* the output map's position */
    /* the fragment programs, one a handle: texel (1, 0) */
    static const u32 handles[] = {PM_HANDLE_TEX, PM_HANDLE_PAST, PM_HANDLE_SELF, PM_HANDLE_CROP, PM_HANDLE_ASTC};
    for (u32 k = 0; k < 5; k++) failed |= pm_fragment(0x800 + 0x400 * k, handles[k]);
    g_renderer->method_barrier(5); /* other code at the addresses */
    CHECK(!failed);
    u32 tic[8], tic_b[8];
    const u32 va = 0xff443322u, vb = 0xff887766u, vc = 0xffccbbaau;
    CHECK(!pm_texture(PM_GPU + PM_A, va) && !pm_texture(PM_GPU + PM_B, vb));
    pm_tic(tic, PM_GPU + PM_A, 8, 8);
    pm_tic(tic_b, PM_GPU + PM_B, 8, 8);
    CHECK(!storage_write_words(PM_GPU + PM_POOL + 32, tic, 8)); /* header 1 */
    u32 self[8];
    pm_tic(self, PM_GPU + PM_TARGET, 4, 4); /* header 2: the target the draws draw into */
    CHECK(!storage_write_words(PM_GPU + PM_POOL + 64, self, 8));
    u32 crop[8];
    pm_tic(crop, PM_GPU + PM_CROP, 4, 4); /* header 3: the top-left 4x4 of the 8x8 target at PM_CROP */
    CHECK(!storage_write_words(PM_GPU + PM_POOL + 96, crop, 8));
    CHECK(!storage_write_words(PM_GPU + PM_SAMPLERS, (const u32[8]){2u | 2u << 3 | 2u << 6}, 8));
    u8 constants[256] = {0};
    snx_st_le32(constants + PM_HANDLE_TEX, 1);
    snx_st_le32(constants + PM_HANDLE_PAST, 40); /* past the limit of 6 */
    snx_st_le32(constants + PM_HANDLE_SELF, 2);
    snx_st_le32(constants + PM_HANDLE_CROP, 3);
    snx_st_le32(constants + PM_HANDLE_ASTC, 4);
    CHECK(!gmem_write(PM_GPU + PM_CONSTANTS, constants, sizeof constants));
    const int sm86 = g_shader_sm86;
    g_shader_sm86 = 1;
    u8 px[4];
    VkPoolGpuCounts d[2];
    /* a texture: the second draw through the map */
    CHECK(!pmg_draw(channel, 0x800, px, &d[0]) && !pmg_draw(channel, 0x800, px, &d[1]));
    failed |= pm_case("a texture", pm_near(px, va, 0) && d[0].learned && d[1].why[VK_POOL_GPU_MAP], px, &d[1]);
    /* the header written by the CPU between two draws */
    for (u32 i = 0; i < 8; i++) snx_st_le32(GP(PM_CPU + PM_POOL + 32) + 4 * i, tic_b[i]);
    vk_ww_new_epoch();
    CHECK(!pmg_draw(channel, 0x800, px, &d[0]));
    failed |= pm_case("the header written", pm_near(px, vb, 0) && !d[0].why[VK_POOL_GPU_MAP], px, &d[0]);
    CHECK(!pmg_draw(channel, 0x800, px, &d[1]));
    failed |= pm_case("the header written, the draw after", pm_near(px, vb, 0) && d[1].why[VK_POOL_GPU_MAP], px, &d[1]);
    /* a handle past the limits: zeros */
    CHECK(!pmg_draw(channel, 0xc00, px, &d[0]));
    failed |= pm_case("a handle past the limits", pm_near(px, 0, 0) && d[0].why[VK_POOL_GPU_PAST], px, &d[0]);
    /* the target the draw draws into: what it held before the draw, red */
    CHECK(!pmg_draw(channel, 0x1000, px, &d[0]));
    failed |= pm_case("a feedback draw", pm_near(px, 0xff0000ffu, 0) && d[0].why[VK_POOL_GPU_VIEW], px, &d[0]);
    /* a texture that crops a target, and the target drawn again (a clear, then the texture barrier of the game) */
    rt_clear_format(channel, PM_GPU + PM_CROP, 8, 8, 0, 1, 0xD5); /* blue */
    atomic_fetch_add_explicit(&g_vk_texture_epoch, 1, memory_order_relaxed);
    CHECK(!pmg_draw(channel, 0x1400, px, &d[0]) && !pmg_draw(channel, 0x1400, px, &d[1]));
    failed |= pm_case("a crop of a target", pm_near(px, 0xffff0000u, 0) && d[1].why[VK_POOL_GPU_MAP], px, &d[1]);
    rt_clear_format(channel, PM_GPU + PM_CROP, 8, 8, 0, 0, 0xD5); /* red */
    atomic_fetch_add_explicit(&g_vk_texture_epoch, 1, memory_order_relaxed);
    CHECK(!pmg_draw(channel, 0x1400, px, &d[0]));
    failed |= pm_case("the cropped target drawn again", pm_near(px, 0xff0000ffu, 0), px, &d[0]);
    /* an ASTC texture held as BC7 later: header 4 */
    static const AlCase astc = {"ASTC 8x8 64x64", NVN_FORMAT_RGBA_ASTC_8x8, 64, 64, 1, 0};
    AlTex x;
    const int astc_mode = g_settings.astc_mode;
    g_settings.astc_mode = 2;
    CHECK(!al_place(&x, &astc, 0, 0));
    al_fill(&x);
    CHECK(!storage_write_words(PM_GPU + PM_POOL + 128, x.tic, 8));
    CHECK(!pmg_draw(channel, 0x1800, px, &d[0]) && !pmg_draw(channel, 0x1800, px, &d[1]));
    u8 before[4];
    memcpy(before, px, 4);
    failed |= pm_case("an ASTC texture as RGBA8", d[1].why[VK_POOL_GPU_MAP] > 0, px, &d[1]);
    vk_lock();
    Surf* t = vk_tex_from_tic(x.tic);
    vk_unlock();
    CHECK(t && t->bc7_later);
    int over = 0;
    const VkImage rgba = t ? t->img : VK_NULL_HANDLE;
    if (t) al_until_done(t, &over);
    CHECK(t && !t->bc7.queued && t->img != rgba);
    CHECK(!pmg_draw(channel, 0x1800, px, &d[0]));
    failed |= pm_case("the ASTC texture as BC7", pm_near(px, snx_ld_le32(before), 64) && !d[0].why[VK_POOL_GPU_MAP] &&
                                                     d[0].learned,
                      px, &d[0]);
    CHECK(!pmg_draw(channel, 0x1800, px, &d[1]));
    failed |= pm_case("the ASTC texture as BC7, the draw after",
                      pm_near(px, snx_ld_le32(before), 64) && d[1].why[VK_POOL_GPU_MAP], px, &d[1]);
    g_settings.astc_mode = astc_mode;
    vk_lock();
    vk_finish();
    vk_unlock();
    al_free(&x);
    g_shader_sm86 = sm86;
    free(channel);
    CHECK(!failed);
    VkPoolGpuCounts all;
    vk_pool_gpu_counts(&all);
    printf("PASS pool map: a texture, its header written, a handle past the limits, a feedback draw, a crop of a target "
           "drawn again and an ASTC texture made BC7 drawn as without the map (%llu slots through it, %llu records "
           "learned, %llu pages written)\n",
           (unsigned long long)all.why[VK_POOL_GPU_MAP], (unsigned long long)all.learned,
           (unsigned long long)all.pages_written);
    return 0;
}
