/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce game's fragment program reads a perspective input as IPA, then FMUL.RZ.FTZ by MUFU.RCP of IPA a[0x7c], which
   the SM86 translator takes as the input itself (sm86_translate.c's fmul_over_w). One triangle drawn through the
   renderer with its vertices' w far apart, and what each fragment got against the perspective-correct value in doubles,
   from the pixel's centre: generic 0's x so folded, the same with MUFU.RCP of |a[0x7c]|, which the translator keeps
   as attr * w * (1 / w) rounded toward zero, a[0x7c] itself, and generic 0's y folded too. The largest and the mean
   relative difference of each over the pixels inside the triangle by more than one pixel; and in how many pixels, by
   how many ulp, the kept x is off the folded one, the same input with the round trip */
#define PF86_GPU 0xce0000000ull
#define PF86_CPU (HEAP_BASE + 0x3f400000)
enum { PF86_W = 64, PF86_H = 64 };

typedef struct {
    double folded[2], kept[2], w[2], y[2]; /* largest, mean */
    u32 pixels, differ, ulp;
} Pf86Error;
static u32 pf86_ulp(float a, float b) {
    i32 x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    if (x < 0) x = INT32_MIN - x; /* the floats in order as integers, -0 as +0 */
    if (y < 0) y = INT32_MIN - y;
    i64 d = (i64)x - y;
    return d < 0 ? (u32)-d : (u32)d;
}
/* an Ounce program at gva: its SPH, words 0, 6, 13 and 18 given, and its code */
static int pf86_program(u64 gva, const u32 sph[19], const Sm86Word* code, u32 n) {
    u8 bytes[SM86_SPH_BYTES + 16 * 16] = {0};
    for (u32 k = 0; k < 19; k++) snx_st_le32(bytes + 4 * k, sph[k]);
    for (u32 i = 0; i < n; i++) sm86_put(bytes + SM86_SPH_BYTES, i, code[i]);
    sm86_put(bytes + SM86_SPH_BYTES, n, sm86_bra(n, n, 7, 0)); /* the BRA to itself after the EXIT */
    return gmem_write(gva, bytes, SM86_SPH_BYTES + 16 * (n + 1));
}
static Sm86Word pf86_ipa(u32 dst, u32 addr) {
    Sm86Word x = sm86_fix(0x326);
    sm86_set(&x, 16, 8, dst);
    sm86_set(&x, 32, 8, 255);
    sm86_set(&x, 64, 8, addr >> 2);
    sm86_set(&x, 81, 3, 7);
    return x;
}
static Sm86Word pf86_rcp(u32 dst, u32 b, int absolute) {
    Sm86Word x = sm86_alu(0x108, 1, dst, 255);
    sm86_set(&x, 32, 8, b);
    sm86_set(&x, 62, 1, (u64)absolute);
    sm86_set(&x, 64, 8, 255);
    sm86_set(&x, 74, 6, 4);
    return x;
}
/* FMUL.RZ.FTZ Rd, Ra, Rb, as the game's programs interpolate */
static Sm86Word pf86_fmul(u32 dst, u32 a, u32 b) {
    Sm86Word x = sm86_alu(0x020, 1, dst, a);
    sm86_set(&x, 32, 8, b);
    sm86_set(&x, 64, 8, 255);
    sm86_set(&x, 78, 2, 3);
    sm86_set(&x, 80, 1, 1);
    sm86_set(&x, 84, 3, 4);
    return x;
}
static Sm86Word pf86_ald_ast(int store, u32 addr, u32 reg) { /* ALD R(reg)-R(reg + 3), a[addr], or AST of them */
    Sm86Word x = sm86_fix(store ? 0x322 : 0x321);
    sm86_set(&x, 16, 8, store ? 0 : reg);
    sm86_set(&x, 24, 8, 255);
    sm86_set(&x, 32, 8, store ? reg : 255);
    sm86_set(&x, 64, 8, 255);
    sm86_set(&x, 40, 10, addr);
    sm86_set(&x, 74, 2, 3);
    return x;
}
static int pf86_draw(GpuChannel* channel, u64 target, u64 vertices, const float position[3][4], const float attr[3][2],
                     Pf86Error* e) {
    u8 bytes[3 * 32];
    for (u32 v = 0; v < 3; v++) {
        const float values[8] = {
            position[v][0], position[v][1], position[v][2], position[v][3], attr[v][0], attr[v][1], 0, 1};
        for (u32 i = 0; i < 8; i++) {
            u32 word;
            memcpy(&word, values + i, 4);
            snx_st_le32(bytes + v * 32 + 4 * i, word);
        }
    }
    CHECK(!gmem_write(vertices, bytes, sizeof bytes));
    u32* r = channel->r3d;
    r[0x7c0] = (u32)((vertices + sizeof bytes - 1) >> 32);
    r[0x7c1] = (u32)(vertices + sizeof bytes - 1);
    u64 drawn = g_vk_drawn;
    const u32 size = PF86_W * PF86_H * 16;
    int sm86 = g_shader_sm86;
    g_shader_sm86 = 1;
    vk_lock();
    rt_clear_format(channel, target, PF86_W, PF86_H, 0, 0, 0xC0); /* RGBA32F */
    channel->r3d_dirty = ~0u;                                     /* the registers were set directly */
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    g_shader_sm86 = sm86;
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(size, &buffer);
    CHECK(surface && surface->w == PF86_W && surface->h == PF86_H && data);
    vk_surf_use(vk_cmd(), surface, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {PF86_W, PF86_H, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    /* the vertices in pixels, as the viewport transform places them */
    double px[3], py[3];
    for (u32 v = 0; v < 3; v++) {
        px[v] = ((double)position[v][0] / position[v][3] + 1) * PF86_W / 2;
        py[v] = ((double)position[v][1] / position[v][3] + 1) * PF86_H / 2;
    }
    double area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    *e = (Pf86Error){0};
    for (u32 y = 0; y < PF86_H; y++)
        for (u32 x = 0; x < PF86_W; x++) {
            double cx = x + 0.5, cy = y + 0.5, b[3];
            b[1] = ((cx - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (cy - py[0])) / area;
            b[2] = ((px[1] - px[0]) * (cy - py[0]) - (cx - px[0]) * (py[1] - py[0])) / area;
            b[0] = 1 - b[1] - b[2];
            int inside = 1; /* by more than a pixel: each weight past what one pixel moves it */
            for (u32 v = 0; v < 3; v++)
                inside &=
                    b[v] * fabs(area) / hypot(px[(v + 2) % 3] - px[(v + 1) % 3], py[(v + 2) % 3] - py[(v + 1) % 3]) >
                    1.0;
            if (!inside) continue;
            double over_w = 0, want_x = 0, want_y = 0;
            for (u32 v = 0; v < 3; v++) {
                over_w += b[v] / position[v][3];
                want_x += b[v] * attr[v][0] / position[v][3];
                want_y += b[v] * attr[v][1] / position[v][3];
            }
            want_x /= over_w;
            want_y /= over_w;
            float got[4];
            memcpy(got, data + ((size_t)y * PF86_W + x) * 16, 16);
            const double want[4] = {want_x, want_x, over_w, want_y};
            u32 apart = pf86_ulp(got[0], got[1]);
            e->differ += apart != 0;
            if (apart > e->ulp) e->ulp = apart;
            double* err[4] = {e->folded, e->kept, e->w, e->y};
            for (u32 k = 0; k < 4; k++) {
                double off = fabs((got[k] - want[k]) / want[k]);
                if (!(off <= 1e300)) off = 1e300; /* a NaN or an infinity counts as the largest */
                if (off > err[k][0]) err[k][0] = off;
                err[k][1] += off;
            }
            e->pixels++;
        }
    if (e->pixels) {
        e->folded[1] /= e->pixels;
        e->kept[1] /= e->pixels;
        e->w[1] /= e->pixels;
        e->y[1] /= e->pixels;
    }
    vk_unlock();
    return 0;
}

static int check_sm86_perspective_fold(void) {
    const u64 address = PF86_GPU, vertices = address + 0x12000, target = address + 0x100000;
    /* ALD R0-R7, a[0x80] and a[0x90], the vertex's attributes 0 and 1; AST a[0x70] from R0-R3, the position, and
       a[0x80] from R4-R7, generic 0 */
    const Sm86Word vertex[] = {pf86_ald_ast(0, 0x80, 0), pf86_ald_ast(0, 0x90, 4), pf86_ald_ast(1, 0x70, 0),
                               pf86_ald_ast(1, 0x80, 4), sm86_exit()};
    const Sm86Word fragment[] = {pf86_ipa(4, 0x7c),  pf86_rcp(5, 4, 0),  pf86_rcp(6, 4, 1),  pf86_ipa(0, 0x80),
                                 pf86_fmul(0, 0, 5), pf86_ipa(1, 0x80),  pf86_fmul(1, 1, 6), pf86_ipa(2, 0x7c),
                                 pf86_ipa(3, 0x84),  pf86_fmul(3, 3, 5), sm86_exit()};
    /* the vertex program writes the position and generic 0 (the output map's bits 428-435); the fragment program
       reads generic 0's components with perspective (byte 0x18) and writes render target 0 from R0-R3 */
    const u32 vsph[19] = {[0] = 0x000c0481u, [13] = 0xffu << 12};
    const u32 fsph[19] = {[0] = 2u | 4u << 5 | 5u << 10, [6] = 0xaa, [18] = 0xf};
    const float transform[6] = {PF86_W / 2.0f, PF86_H / 2.0f, 0.5f, PF86_W / 2.0f, PF86_H / 2.0f, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 128;
    CHECK(mem_map(PF86_CPU, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(address, PF86_CPU, 0x200000);
    CHECK(!pf86_program(address, vsph, vertex, 5));
    CHECK(!pf86_program(address + 0x10000, fsph, fragment, 11));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = PF86_W << 16;
    r[0x3fe] = PF86_H << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 0 | 1u << 21 | 7u << 27;            /* stream 0 at 0: RGBA32F */
    r[0x459] = 0 | 16u << 7 | 1u << 21 | 7u << 27; /* stream 0 at 16: RGBA32F */
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)(vertices >> 32);
    r[0x702] = (u32)vertices;
    /* a triangle across the target on whole pixels, with these w: four times apart near the eye (gl_FragCoord.w about
       1000), near 1 and far off (about 1 / 1000), then a millionfold apart */
    static const float places[3][2] = {{-28 / 32.0f, -28 / 32.0f}, {28 / 32.0f, -22 / 32.0f}, {-3 / 32.0f, 28 / 32.0f}};
    static const float spans[][3] = {{1, 2, 4}, {1e-3f, 2e-3f, 4e-3f}, {1e3f, 2e3f, 4e3f}, {1e-3f, 1, 1e3f}};
    static const double tolerance[] = {1e-4, 1e-4, 1e-4, 0};
    /* generic 0's x and y at each corner, none near 0, where a relative difference measures nothing */
    static const float attr[3][2] = {{0.1f, 3.0f}, {0.7f, 20.0f}, {0.35f, 0.5f}};
    for (u32 s = 0; s < sizeof spans / sizeof *spans; s++) {
        float position[3][4];
        for (u32 v = 0; v < 3; v++) {
            float w = spans[s][v];
            position[v][0] = places[v][0] * w;
            position[v][1] = places[v][1] * w;
            position[v][2] = 0.5f * w;
            position[v][3] = w;
        }
        Pf86Error e;
        CHECK(!pf86_draw(channel, target, vertices, position, attr, &e));
        printf("SM86 perspective fold: w %g, %g and %g, %u pixels: relative difference from the exact value, largest "
               "and mean: folded x %.3g %.3g, kept x %.3g %.3g, folded y %.3g %.3g, gl_FragCoord.w %.3g %.3g; the kept "
               "x differs from the folded one in %u pixels, by %u ulp at most\n",
               (double)spans[s][0], (double)spans[s][1], (double)spans[s][2], e.pixels, e.folded[0], e.folded[1],
               e.kept[0], e.kept[1], e.y[0], e.y[1], e.w[0], e.w[1], e.differ, e.ulp);
        CHECK(e.pixels > PF86_W * PF86_H / 8);
        /* the folded value is the device's perspective-correct input. A fold that dropped one multiply alone would be
           gl_FragCoord.w or its reciprocal off, 0.75 or more somewhere on each triangle. Over a millionfold of w the
           difference is the device's own interpolation's, folded or not */
        CHECK(!tolerance[s] || (e.folded[0] < tolerance[s] && e.y[0] < tolerance[s]));
        /* the kept one is that input with the round trip's roundings, toward zero: a few ulp off it, somewhere */
        CHECK(e.differ && e.ulp <= 8);
    }
    vk_lock();
    vk_render_end();
    vk_unlock();
    free(channel);
    return 0;
}
