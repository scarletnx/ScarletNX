/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The kept draw states (render.c's g_draw): each case draws a sequence of draws into targets of their own, and the
   pixels must be what each draw must draw. The counts tell that the table was used where it may be, and not where it
   must not: a draw of the state an earlier draw of the same epoch had, with a draw of other state between them (A,
   B, A), takes the earlier one's shaders and pipeline from the table. Its own vertex format picks among entries of
   one key. A new write tracking epoch, or the program's code written and invalidated, makes it again. Each sequence
   is drawn a second time with draw_state_check=1, which makes the kept states again and compares: the same pixels,
   and no difference. Synthetic programs only. */
#define MEMO_GPU    0x9d4000000ull
#define MEMO_CPU    (HEAP_BASE + 0x13400000)
#define MEMO_SIDE   4
#define MEMO_DRAWS  4
#define MEMO_GREEN  0x10000 /* the fragment programs' offsets: each writes its color */
#define MEMO_BLUE   0x10100

/* a fragment program at offset that writes (r, g, b, 1) */
static int memo_fragment(u32 offset, u32 r, u32 g, u32 b) {
    const u64 ops[] = {invalidate_mov(0, r), invalidate_mov(1, g), invalidate_mov(2, b), invalidate_mov(3, 0x3f800000),
                       0xe30000000007000full};
    return mesh_program(MEMO_GPU + offset, ops, sizeof ops / sizeof *ops, 1);
}

/* what one draw of a sequence sets: its fragment program, the stream its position attribute reads (0: a triangle over
   the whole target, 1: one over its top left quarter), and what happens before it */
typedef struct {
    u32 fragment, stream;
    int epoch;  /* a new write tracking epoch first */
    int recode; /* MEMO_GREEN's program made white first, and the 3D engine's code invalidated */
} MemoDraw;

/* the sequence drawn with draw_state_check as given, each draw into a target of its own cleared red: its pixels (0, 0)
   and (3, 3) into px, and the counts it added into *kept, *table and *differs */
static int memo_sequence(GpuChannel* channel, const MemoDraw* draws, u32 n, int check, u8 px[][2][4], u64* kept,
                         u64* table, u64* differs) {
    u32* r = channel->r3d;
    int saved_check = g_settings.draw_state_check;
    CHECK(memo_fragment(MEMO_GREEN, 0, 0x3f800000, 0));
    CHECK(memo_fragment(MEMO_BLUE, 0, 0, 0x3f800000));
    g_settings.draw_state_check = check;
    vk_lock();
    vk_render_draws_forget(); /* nothing of an earlier sequence */
    vk_unlock();
    maxwell3d_method(channel, 0x369, 1, 1); /* and no translation of an earlier sequence's programs */
    VkDrawCounts before;
    vk_draw_counts(&before);
    u64 kept_before = g_vk_draw_kept;
    for (u32 i = 0; i < n; i++) {
        const u64 target = MEMO_GPU + 0x20000 + i * 0x4000;
        if (draws[i].recode) {
            CHECK(memo_fragment(MEMO_GREEN, 0x3f800000, 0x3f800000, 0x3f800000));
            maxwell3d_method(channel, 0x369, 1, 1);
        }
        if (draws[i].epoch) vk_ww_new_epoch();
        u64 drawn = g_vk_drawn;
        vk_lock();
        rt_clear(channel, target, MEMO_SIDE, MEMO_SIDE, 0, 0);
        r[0x851] = draws[i].fragment;
        r[0x458] = draws[i].stream | 1u << 21 | 7u << 27; /* RGBA32F */
        channel->r3d_dirty = ~0u; /* the registers were set directly */
        mx_draw_state_sync(channel);
        DrawCall call = {.topology = 4, .count = 3, .instances = 1};
        vk_render_draw(&channel->ds, &call, &channel->owner);
        vk_render_end();
        vk_unlock();
        CHECK(g_vk_drawn == drawn + 1);
    }
    for (u32 i = 0; i < n; i++) {
        vk_lock();
        Surf* target = vk_rt_find(MEMO_GPU + 0x20000 + i * 0x4000);
        CHECK(target);
        CHECK(!read_pixel_at(target, 0, 0, px[i][0]));
        CHECK(!read_pixel_at(target, MEMO_SIDE - 1, MEMO_SIDE - 1, px[i][1]));
        vk_unlock();
    }
    VkDrawCounts after;
    vk_draw_counts(&after);
    *kept = g_vk_draw_kept - kept_before;
    *table = after.table - before.table;
    *differs = after.differs - before.differs;
    g_settings.draw_state_check = saved_check;
    return 0;
}

/* the sequence three times: first to make its pipelines (a pipeline still being made is kept by no draw), then as it
   is and with draw_state_check=1. The same pixels, each draw's pixels its program's color where its triangle covers
   them (red, the clear, elsewhere), no difference found, and the table taken by `table` draws in the last two runs */
static int memo_case(GpuChannel* channel, const char* what, const MemoDraw* draws, u32 n, u64 table) {
    u8 px[3][MEMO_DRAWS][2][4];
    u64 kept[3], taken[3], differs[3];
    for (int run = 0; run < 3; run++)
        CHECK(!memo_sequence(channel, draws, n, run == 2, px[run], &kept[run], &taken[run], &differs[run]));
    int recoded = 0;
    for (u32 i = 0; i < n; i++) {
        recoded |= draws[i].recode;
        const u8* color = draws[i].fragment == MEMO_BLUE ? (const u8[4]){0, 0, 255, 255}
                          : recoded                      ? (const u8[4]){255, 255, 255, 255}
                                                         : (const u8[4]){0, 255, 0, 255};
        static const u8 red[4] = {255, 0, 0, 255};
        for (int run = 0; run < 3; run++) {
            int ok = !memcmp(px[run][i][0], color, 4) && !memcmp(px[run][i][1], draws[i].stream ? red : color, 4);
            if (!ok || memcmp(px[run][i], px[0][i], sizeof px[0][i]))
                fprintf(stderr,
                        "draw memo, %s: draw %u%s: pixels %u,%u,%u,%u and %u,%u,%u,%u, expected %u,%u,%u,%u and %s\n",
                        what, i, run == 2 ? " with draw_state_check=1" : "", px[run][i][0][0], px[run][i][0][1],
                        px[run][i][0][2], px[run][i][0][3], px[run][i][1][0], px[run][i][1][1], px[run][i][1][2],
                        px[run][i][1][3], color[0], color[1], color[2], color[3], draws[i].stream ? "red" : "the same");
            CHECK(ok && !memcmp(px[run][i], px[0][i], sizeof px[0][i]));
        }
    }
    if (taken[1] != table || taken[2] != table || differs[1] || differs[2] || kept[1] < table)
        fprintf(stderr,
                "draw memo, %s: %llu and %llu draws took the table (expected %llu), %llu and %llu differences, %llu "
                "kept\n",
                what, (unsigned long long)taken[1], (unsigned long long)taken[2], (unsigned long long)table,
                (unsigned long long)differs[1], (unsigned long long)differs[2], (unsigned long long)kept[1]);
    CHECK(taken[1] == table && taken[2] == table && !differs[1] && !differs[2]);
    CHECK(kept[1] >= table);
    return 0;
}

static int check_draw_memo(void) {
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* the position */
                          0xe30000000007000full};
    /* stream 0: a triangle over the whole target; stream 1: one over its top left quarter */
    const float whole[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1};
    const float quarter[12] = {-1, -1, 0, 1, 0, -1, 0, 1, -1, 0, 0, 1};
    const float transform[6] = {MEMO_SIDE / 2, MEMO_SIDE / 2, 0.5f, MEMO_SIDE / 2, MEMO_SIDE / 2, 0.5f};
    CHECK(mem_map(MEMO_CPU, 0x40000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(MEMO_GPU, MEMO_CPU, 0x40000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 159;
    for (u32 stream = 0; stream < 2; stream++) {
        u8 bytes[48];
        for (u32 i = 0; i < 12; i++) {
            u32 word;
            memcpy(&word, (stream ? quarter : whole) + i, 4);
            snx_st_le32(bytes + 4 * i, word);
        }
        CHECK(!gmem_write(MEMO_GPU + 0x1000 + stream * 0x100, bytes, sizeof bytes));
    }
    CHECK(mesh_program(MEMO_GPU, vertex, sizeof vertex / sizeof *vertex, 0));
    u32* r = channel->r3d;
    r[0x582] = (u32)(MEMO_GPU >> 32);
    r[0x583] = (u32)MEMO_GPU;
    r[0x810] = r[0x850] = 1;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = MEMO_SIDE << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1; /* as reset_3d starts a channel */
    all_samples(r);
    memcpy(r + 0x280, transform, sizeof transform);
    for (u32 stream = 0; stream < 2; stream++) {
        u64 source = MEMO_GPU + 0x1000 + stream * 0x100;
        r[0x700 + stream * 4] = 0x1000 | 16;
        r[0x701 + stream * 4] = (u32)(source >> 32);
        r[0x702 + stream * 4] = (u32)source;
        r[0x7c0 + stream * 2] = (u32)((source + 47) >> 32);
        r[0x7c1 + stream * 2] = (u32)(source + 47);
    }
    /* A, B, A and B, A, B: the third draw takes the first one's state from the table */
    const MemoDraw aba[] = {{MEMO_GREEN, 0, 0, 0}, {MEMO_BLUE, 0, 0, 0}, {MEMO_GREEN, 0, 0, 0}, {MEMO_BLUE, 0, 0, 0}};
    CHECK(!memo_case(channel, "A, B, A, B", aba, 4, 2));
    /* one key, two vertex formats (the position from another stream): two entries of one set, each draw taking its
       own format's */
    const MemoDraw formats[] = {{MEMO_GREEN, 0, 0, 0}, {MEMO_GREEN, 1, 0, 0}, {MEMO_GREEN, 0, 0, 0},
                                {MEMO_GREEN, 1, 0, 0}};
    CHECK(!memo_case(channel, "one state in two vertex formats", formats, 4, 2));
    /* a new epoch between: made again (the table keeps within an epoch) */
    const MemoDraw epoch[] = {{MEMO_GREEN, 0, 0, 0}, {MEMO_BLUE, 0, 0, 0}, {MEMO_GREEN, 0, 1, 0}};
    CHECK(!memo_case(channel, "A, B, a new epoch, A", epoch, 3, 0));
    /* the program's code written and invalidated between: made again, and the new code drawn */
    const MemoDraw recode[] = {{MEMO_GREEN, 0, 0, 0}, {MEMO_BLUE, 0, 0, 0}, {MEMO_GREEN, 0, 0, 1}};
    CHECK(!memo_case(channel, "A, B, A's code written, A", recode, 3, 0));
    free(channel);
    puts("PASS draw memo: A, B, A and a state in two vertex formats take the table, a new epoch and new code make the "
         "state again, and draw_state_check=1 finds no difference");
    return 0;
}
