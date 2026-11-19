/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

enum { FOLD_W = 64, FOLD_H = 64 };

/* One triangle drawn through the renderer with its vertices' w far apart, and what each fragment got against the
   perspective-correct value in doubles, from the pixel's centre: generic 0's x by IPA.MUL by MUFU.RCP of a[0x7c],
   which the translator takes as the input itself (shader_direct_ipa_over_w), the same IPA.MUL by MUFU.RCP of
   |a[0x7c]|, which it keeps as attr * w * (1 / w), a[0x7c] itself, and generic 0's y folded too. The largest and the
   mean relative difference of each, over the pixels inside the triangle by more than one pixel; and how far the kept
   x is from the folded one, the same input with the round trip: in how many pixels it differs, by how many ulp */
typedef struct {
    double folded[2], kept[2], w[2], y[2]; /* largest, mean */
    u32 pixels, differ, ulp;
} FoldError;
static u32 fold_ulp(float a, float b) {
    i32 x, y;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    if (x < 0) x = INT32_MIN - x; /* the floats in order as integers, -0 as +0 */
    if (y < 0) y = INT32_MIN - y;
    i64 d = (i64)x - y;
    return d < 0 ? (u32)-d : (u32)d;
}
static int fold_draw(GpuChannel* channel, u64 target, u64 vertices, const float position[3][4], const float attr[3][2],
                     FoldError* e) {
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
    const u32 size = FOLD_W * FOLD_H * 16;
    vk_lock();
    rt_clear_format(channel, target, FOLD_W, FOLD_H, 0, 0, 0xC0); /* RGBA32F */
    channel->r3d_dirty = ~0u;                                     /* the registers were set directly */
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    CHECK(g_vk_drawn == drawn + 1);
    vk_render_end();
    Surf* surface = vk_rt_find(target);
    VkBuffer buffer;
    u8* data = vk_readback_buffer(size, &buffer);
    CHECK(surface && surface->w == FOLD_W && surface->h == FOLD_H && data);
    vk_surf_use(vk_cmd(), surface, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {FOLD_W, FOLD_H, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), surface->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    /* the vertices in pixels, as the viewport transform places them */
    double px[3], py[3];
    for (u32 v = 0; v < 3; v++) {
        px[v] = ((double)position[v][0] / position[v][3] + 1) * FOLD_W / 2;
        py[v] = ((double)position[v][1] / position[v][3] + 1) * FOLD_H / 2;
    }
    double area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
    *e = (FoldError){0};
    for (u32 y = 0; y < FOLD_H; y++)
        for (u32 x = 0; x < FOLD_W; x++) {
            double cx = x + 0.5, cy = y + 0.5, b[3];
            b[1] = ((cx - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (cy - py[0])) / area;
            b[2] = ((px[1] - px[0]) * (cy - py[0]) - (cx - px[0]) * (py[1] - py[0])) / area;
            b[0] = 1 - b[1] - b[2];
            /* inside by more than a pixel: each weight past what one pixel moves it */
            int inside = 1;
            for (u32 v = 0; v < 3; v++) {
                double edge = fabs(area) / hypot(px[(v + 2) % 3] - px[(v + 1) % 3], py[(v + 2) % 3] - py[(v + 1) % 3]);
                inside &= b[v] * edge > 1.0;
            }
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
            memcpy(got, data + ((size_t)y * FOLD_W + x) * 16, 16);
            const double want[4] = {want_x, want_x, over_w, want_y};
            u32 apart = fold_ulp(got[0], got[1]);
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

static int check_perspective_fold(void) {
    const u64 address = 0xcc0000000ull, cpu = HEAP_BASE + 0x3f000000;
    const u64 vertices = address + 0x12000, target = address + 0x100000;
/* ALD Rd, a[0x80 + 4 d]: a word of the vertex's attributes 0 and 1, a register each */
#define FOLD_ALD(d) (0xefd87f8000070000ull | (0x80ull + 4 * (d)) << 20 | 0xff00ull | (u64)(d))
    /* then AST a[0x70] from R0..3, the position, and a[0x80] from R4..7, generic 0 */
    const u64 vertex[] = {FOLD_ALD(0),           FOLD_ALD(1),           FOLD_ALD(2),          FOLD_ALD(3),
                          FOLD_ALD(4),           FOLD_ALD(5),           FOLD_ALD(6),          FOLD_ALD(7),
                          0xeff1ff800707ff00ull, 0xeff1ff800807ff04ull, 0xe30000000007000full};
#undef FOLD_ALD
#define FOLD_IPA(d, addr) (0xe000000000070000ull | (u64)((addr) / 4) << 30 | 0xff00ull | (u64)(d))
    const u64 fragment[] = {FOLD_IPA(4, 0x7c),                                  /* IPA R4, a[0x7c] */
                            0x5080000000470000ull | 4ull << 8 | 5,              /* MUFU.RCP R5, R4 */
                            0x5080000000470000ull | 1ull << 46 | 4ull << 8 | 6, /* MUFU.RCP R6, |R4| */
                            FOLD_IPA(0, 0x80) | 1ull << 54 | 5ull << 20,        /* IPA.MUL R0, a[0x80], R5 */
                            FOLD_IPA(1, 0x80) | 1ull << 54 | 6ull << 20,        /* IPA.MUL R1, a[0x80], R6 */
                            FOLD_IPA(2, 0x7c),                                  /* IPA R2, a[0x7c] */
                            FOLD_IPA(3, 0x84) | 1ull << 54 | 5ull << 20,        /* IPA.MUL R3, a[0x84], R5 */
                            0xe30000000007000full};
#undef FOLD_IPA
    const float transform[6] = {FOLD_W / 2.0f, FOLD_H / 2.0f, 0.5f, FOLD_W / 2.0f, FOLD_H / 2.0f, 0.5f};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 127;
    CHECK(mem_map(cpu, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(address, cpu, 0x200000);
    CHECK(varying_program(address, vertex, 11, 0));
    CHECK(varying_program(address + 0x10000, fragment, 8, 1));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = FOLD_W << 16;
    r[0x3fe] = FOLD_H << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 0 | 1u << 21 | 7u << 27;            /* stream 0 at 0: RGBA32F */
    r[0x459] = 0 | 16u << 7 | 1u << 21 | 7u << 27; /* stream 0 at 16: RGBA32F */
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)(vertices >> 32);
    r[0x702] = (u32)vertices;
    /* a triangle across the target, its corners at the places given (x / w, y / w) with these w: four times apart
       near the eye (gl_FragCoord.w about 1000: attr * w a thousand times the input), near 1 and far off (about 1 /
       1000), then a millionfold apart */
    /* on whole pixels, where the device's grid of sub-pixel positions puts them too */
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
        FoldError e;
        CHECK(!fold_draw(channel, target, vertices, position, attr, &e));
        printf("perspective fold: w %g, %g and %g, %u pixels: relative difference from the exact value, largest and "
               "mean: folded x %.3g %.3g, kept x %.3g %.3g, folded y %.3g %.3g, gl_FragCoord.w %.3g %.3g; the kept x "
               "differs from the folded one in %u pixels, by %u ulp at most\n",
               (double)spans[s][0], (double)spans[s][1], (double)spans[s][2], e.pixels, e.folded[0], e.folded[1],
               e.kept[0], e.kept[1], e.y[0], e.y[1], e.w[0], e.w[1], e.differ, e.ulp);
        CHECK(e.pixels > FOLD_W * FOLD_H / 8);
        /* the folded value is the device's perspective-correct input, as near the exact one as the device's
           interpolation is (lavapipe 5e-7; RADV 7e-5 with the corners off its sub-pixel grid). A fold that dropped
           one multiply alone would be gl_FragCoord.w or its reciprocal off, 0.75 or more somewhere on each triangle.
           Over a millionfold of w the difference is the device's own: a plane of 1 / w through the corners loses
           digits where it is small, folded or not */
        CHECK(!tolerance[s] || (e.folded[0] < tolerance[s] && e.y[0] < tolerance[s]));
        /* the kept one is that input with the round trip's roundings: a few ulp off it, and off it somewhere */
        CHECK(e.differ && e.ulp <= 8);
    }
    vk_lock();
    vk_render_end();
    vk_unlock();
    free(channel);
    return 0;
}
