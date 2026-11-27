/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The kept draw states (render.c's g_draw): each case draws a sequence of draws into targets of their own, and the
   pixels must be what each draw must draw. The counts tell that the table was used where it may be, and not where it
   must not: a draw of the state an earlier draw of the same epoch had, with a draw of other state between them (A,
   B, A), takes the earlier one's shaders and pipeline from the table. Its own vertex format picks among entries of
   one key. A new write tracking epoch, or the program's code written and invalidated, makes it again. Each sequence
   is drawn a second time with draw_state_check=1, which makes the kept states again and compares: the same pixels,
   and no difference. Synthetic programs only. */
#include "nx/gpu/vk/descriptor_heaps.h" /* the lasting part's counts (last_cases) */
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

/* What a draw keeps past its submission, or in its frame (render.c's cb_get, tex_bind_view, tex_bind_sampler and
   desc_set_cached): each case draws a sequence as it is and with draw_state_check=1, with something that happens before
   each draw, and the pixels must be the same in the two runs and as the case's model says. A fragment program reads its
   color from constant buffer 3 (LIFE_CB), or samples a 2x1 R32F texture of 0 and 1 at 5/8 of its width, linearly (red
   0.75) */
#define LIFE_CB          (MEMO_GPU + 0x38000)
#define LIFE_POOL        (MEMO_GPU + 0x3a000) /* the texture header; its sampler at 0x100, the handle's buffer at
                                                 0x200, the texels at 0x400 */
#define LIFE_CB_PROGRAM  0x10800
#define LIFE_TEX_PROGRAM 0x10c00
#define LIFE_DRAWS       6
#define LIFE_JUNK        (256u << 10) /* the bytes a present's event writes into each ring of a frame */
enum {
    LIFE_NONE,
    LIFE_SUBMIT, /* what was recorded submitted, as cmd.c's frame_check does after each millisecond of work */
    LIFE_CPU,    /* the guest's CPU writes the constant buffer (blue), and its queue submission starts an epoch */
    LIFE_FRONT,  /* the 3D engine's constant buffer upload writes it (white), in the same epoch */
    LIFE_RESULT, /* a storage result over it (each byte 0x3f) is recorded and stays pending */
    LIFE_LAND,   /* everything done and landed (a finish) */
    LIFE_FRAMES, /* VK_FRAMES + 1 presents, each frame's rings written over with other bytes */
    LIFE_TIC,    /* the guest's CPU writes the texture header (its red read as one), and an epoch */
    LIFE_TSC,    /* the GPU writes the sampler header (nearest filtering: texel 1, red one), in the same epoch */
};
static const float g_life_green[4] = {0, 1, 0, 1}, g_life_blue[4] = {0, 0, 1, 1}, g_life_white[4] = {1, 1, 1, 1};
static void life_floats(u8* bytes, const float* f, u32 n) {
    for (u32 i = 0; i < n; i++) {
        u32 word;
        memcpy(&word, f + i, 4);
        snx_st_le32(bytes + 4 * i, word);
    }
}
/* the texture's headers as each run starts: red read from the texels, linear filtering */
static void life_tic(u32 tic[8], int one) {
    const u64 texels = LIFE_POOL + 0x400;
    tic[0] = 0x0f | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16 | (one ? 7u : 2u) << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    tic[1] = (u32)texels;
    tic[2] = (u32)(texels >> 32) | 3u << 21;
    tic[3] = 0;
    tic[4] = 1 | 1u << 23;
    tic[5] = 1u << 31;
    tic[6] = tic[7] = 0;
}
static int life_headers(int one, int nearest) {
    u32 tic[8], tsc[8] = {2u | 2u << 3 | 2u << 6, nearest ? 1u | 1u << 4 | 1u << 6 : 2u | 2u << 4 | 1u << 6};
    u8 bytes[32];
    life_tic(tic, one);
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(LIFE_POOL, bytes, 32));
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tsc[i]);
    CHECK(!gmem_write(LIFE_POOL + 0x100, bytes, 32));
    return 0;
}
/* the event before a draw */
static int life_event(GpuChannel* channel, int ev) {
    u8 bytes[16];
    switch (ev) {
        case LIFE_SUBMIT:
            vk_lock();
            vk_render_end();
            vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
            vk_unlock();
            break;
        case LIFE_CPU:
            life_floats(bytes, g_life_blue, 4);
            memcpy(GP(MEMO_CPU + (LIFE_CB - MEMO_GPU)), bytes, 16);
            vk_ww_new_epoch();
            break;
        case LIFE_FRONT:
            life_floats(bytes, g_life_white, 4);
            channel->r3d[0x8E3] = 0;
            for (u32 i = 0; i < 4; i++) r3d_const_upload(channel, snx_ld_le32(bytes + 4 * i), i == 3);
            break;
        case LIFE_RESULT: {
            VkBuffer out;
            u64 at;
            vk_lock();
            vk_render_end();
            CHECK(vk_buf_copy_writable(LIFE_CB, 16, &out, &at, 1));
            vk_buf_writable_commit();
            CHECK(!owned_test_fill(out, at, 16, 0x3f));
            VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                                  .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                                  .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                                  .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                  .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
            VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1,
                                  .pMemoryBarriers = &b};
            rec_CmdPipelineBarrier2(vk_cmd(), &d);
            vk_unlock();
            break;
        }
        case LIFE_LAND:
            vk_lock();
            vk_finish();
            vk_unlock();
            break;
        case LIFE_FRAMES:
            vk_lock();
            vk_render_end();
            for (u32 k = 0; k <= VK_FRAMES; k++) {
                VkBuffer b;
                u64 o;
                u8* junk = vk_stage(LIFE_JUNK, vk_stage_align(), &b, &o);
                CHECK(junk);
                memset(junk, 0x7f, LIFE_JUNK);
                CHECK((junk = vk_stage_upload(LIFE_JUNK, vk_stage_align(), &b, &o)));
                memset(junk, 0x7f, LIFE_JUNK);
                vk_frame_next();
            }
            vk_unlock();
            break;
        case LIFE_TIC: {
            u32 tic[8];
            life_tic(tic, 1);
            for (u32 i = 0; i < 8; i++) snx_st_le32(GP(MEMO_CPU + (LIFE_POOL - MEMO_GPU)) + i * 4, tic[i]);
            vk_ww_new_epoch();
            break;
        }
        case LIFE_TSC: {
            u32 tsc[8] = {2u | 2u << 3 | 2u << 6, 1u | 1u << 4 | 1u << 6};
            u8 words[32] = {0};
            for (u32 i = 0; i < 2; i++) snx_st_le32(words + i * 4, tsc[i]);
            CHECK(!gmem_write(LIFE_POOL + 0x100, words, 32));
            break;
        }
        default: break;
    }
    return 0;
}
/* the counts a run added */
typedef struct {
    u64 past, views, samplers, sets, differs;
} LifeCounts;
/* the sequence with draw_state_check as given, each draw into a target of its own cleared red: their pixels (0, 0) into
   px */
static int life_sequence(GpuChannel* channel, int tex, const u8* events, u32 n, int check, u8 px[LIFE_DRAWS][4],
                         LifeCounts* counts) {
    u32* r = channel->r3d;
    int saved_check = g_settings.draw_state_check;
    u8 bytes[256] = {0};
    life_floats(bytes, g_life_green, 4);
    CHECK(!gmem_write(LIFE_CB, bytes, 256));
    CHECK(!life_headers(0, 0));
    g_settings.draw_state_check = check;
    vk_lock();
    vk_finish(); /* nothing of the run before pending */
    vk_render_draws_forget();
    /* each frame slot's rings begun again, so what the run stages lies where LIFE_FRAMES's bytes go */
    for (u32 k = 0; k <= VK_FRAMES; k++) vk_frame_next();
    vk_unlock();
    vk_ww_new_epoch();
    VkDrawCounts before;
    vk_draw_counts(&before);
    for (u32 i = 0; i < n; i++) {
        const u64 target = MEMO_GPU + 0x20000 + i * 0x4000;
        CHECK(!life_event(channel, events[i]));
        u64 drawn = g_vk_drawn;
        vk_lock();
        rt_clear(channel, target, MEMO_SIDE, MEMO_SIDE, 0, 0);
        r[0x851] = tex ? LIFE_TEX_PROGRAM : LIFE_CB_PROGRAM;
        r[0x458] = 0 | 1u << 21 | 7u << 27; /* RGBA32F, the whole target's triangle */
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
        CHECK(!read_pixel_at(target, 0, 0, px[i]));
        vk_unlock();
    }
    VkDrawCounts after;
    vk_draw_counts(&after);
    counts->past = after.cb[VK_CB_PAST_SUBMIT] - before.cb[VK_CB_PAST_SUBMIT];
    counts->views = after.tex[VK_TEX_VIEW_KEPT] - before.tex[VK_TEX_VIEW_KEPT];
    counts->samplers = after.tex[VK_TEX_SAMPLER_KEPT] - before.tex[VK_TEX_SAMPLER_KEPT];
    counts->sets = after.sets_last - before.sets_last;
    counts->differs = after.differs - before.differs;
    g_settings.draw_state_check = saved_check;
    return 0;
}
/* a color as the RGBA8 target holds it, within one */
static int life_near(const u8* p, const u8* want) {
    for (int c = 0; c < 4; c++)
        if (abs((int)p[c] - (int)want[c]) > 1) return 0;
    return 1;
}
/* the case's two runs: the same pixels, each the model's (the constant buffer's color, or the texture's red), no
   difference found, and the keeps the case names: kept past a submission (keeps & 1), views and samplers kept
   (keeps & 2), a stage's last set taken again (keeps & 4) */
static int life_case(GpuChannel* channel, const char* what, int tex, const u8* events, u32 n, int keeps) {
    u8 px[2][LIFE_DRAWS][4];
    LifeCounts counts[2];
    for (int run = 0; run < 2; run++) CHECK(!life_sequence(channel, tex, events, n, run, px[run], &counts[run]));
    u8 want[4] = {0, 255, 0, 255};
    for (u32 i = 0; i < n; i++) {
        switch (events[i]) {
            case LIFE_CPU: memcpy(want, (const u8[4]){0, 0, 255, 255}, 4); break;
            case LIFE_FRONT: memcpy(want, (const u8[4]){255, 255, 255, 255}, 4); break;
            case LIFE_RESULT: memcpy(want, (const u8[4]){190, 190, 190, 190}, 4); break; /* 0x3f3f3f3f is 0.747 */
            case LIFE_TIC:
            case LIFE_TSC: memcpy(want, (const u8[4]){255, 0, 0, 255}, 4); break;
            default: break;
        }
        if (tex && i == 0) memcpy(want, (const u8[4]){191, 0, 0, 255}, 4); /* 0.75 of the way from 0 to 1 */
        for (int run = 0; run < 2; run++) {
            int ok = life_near(px[run][i], want) && !memcmp(px[run][i], px[0][i], 4);
            if (!ok)
                fprintf(stderr,
                        "draw memo, %s: draw %u%s: pixel %u,%u,%u,%u, expected %u,%u,%u,%u and the pixel of the first "
                        "run\n",
                        what, i, run ? " with draw_state_check=1" : "", px[run][i][0], px[run][i][1], px[run][i][2],
                        px[run][i][3], want[0], want[1], want[2], want[3]);
            CHECK(ok);
        }
    }
    int counted = !counts[0].differs && !counts[1].differs;
    for (int run = 0; run < 2; run++) {
        if (keeps & 1) counted &= counts[run].past != 0;
        if (keeps & 2) counted &= counts[run].views != 0 && counts[run].samplers != 0;
        if (keeps & 4) counted &= counts[run].sets != 0;
    }
    if (!counted)
        fprintf(stderr,
                "draw memo, %s: kept past a submission %llu and %llu; views %llu and %llu; samplers %llu and %llu; "
                "last sets %llu and %llu; differences %llu and %llu (keeps %d)\n",
                what, (unsigned long long)counts[0].past, (unsigned long long)counts[1].past,
                (unsigned long long)counts[0].views, (unsigned long long)counts[1].views,
                (unsigned long long)counts[0].samplers, (unsigned long long)counts[1].samplers,
                (unsigned long long)counts[0].sets, (unsigned long long)counts[1].sets,
                (unsigned long long)counts[0].differs, (unsigned long long)counts[1].differs, keeps);
    CHECK(counted);
    return 0;
}
static int life_cases(GpuChannel* channel) {
    const u64 cb_program[] = {0x4c98078000070000ull | 3ull << 34 | 0ull << 20 | 0, /* MOV R0, c[3][0x0] */
                              0x4c98078000070000ull | 3ull << 34 | 1ull << 20 | 1, /* MOV R1, c[3][0x4] */
                              0x4c98078000070000ull | 3ull << 34 | 2ull << 20 | 2, /* MOV R2, c[3][0x8] */
                              0x4c98078000070000ull | 3ull << 34 | 3ull << 20 | 3, /* MOV R3, c[3][0xc] */
                              0xe30000000007000full};
    const u64 tex_program[] = {0x0100000000071004ull | 0x3f200000ull << 20, /* MOV32I R4, 0.625 */
                               0x0100000000071005ull | 0x3f000000ull << 20, /* MOV32I R5, 0.5 */
                               /* TEX R0, R4, c[0x40], 2D, RGBA */
                               0xc000000000000000ull | 0x10ull << 36 | 7ull << 51 | 0xfull << 31 | 2ull << 28 |
                                   0xffull << 20 | 7ull << 16 | 4ull << 8,
                               0xe30000000007000full};
    CHECK(mesh_program(MEMO_GPU + LIFE_CB_PROGRAM, cb_program, 5, 1));
    CHECK(mesh_program(MEMO_GPU + LIFE_TEX_PROGRAM, tex_program, 4, 1));
    maxwell3d_method(channel, 0x369, 1, 1); /* the code cache invalidated */
    u8 bytes[256] = {0};
    snx_st_le32(bytes + 4, 0x3f800000); /* texel 1 is 1.0, texel 0 nought */
    CHECK(!gmem_write(LIFE_POOL + 0x400, bytes, 256));
    memset(bytes, 0, sizeof bytes);
    CHECK(!gmem_write(LIFE_POOL + 0x200, bytes, 256)); /* the handle: 0, the header above and sampler 0 */
    u32* r = channel->r3d;
    r[0x55D] = (u32)(LIFE_POOL >> 32), r[0x55E] = (u32)LIFE_POOL, r[0x55F] = 0;
    r[0x557] = (u32)((LIFE_POOL + 0x100) >> 32), r[0x558] = (u32)(LIFE_POOL + 0x100), r[0x559] = 0;
    r[0x48D] = 0;
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffers 2 (the handle) and 3 (the color) of the fragment stage */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    r[0x8E1] = (u32)(LIFE_CB >> 32);
    r[0x8E2] = (u32)LIFE_CB;
    r3d_cb_bind(channel, 4, 1 | 3u << 4);
    /* the same bytes through submissions: kept past them once the buffer cache has a buffer of its own for them (its
       third lookup), or at once from the frame's staging */
    const u8 submits[] = {LIFE_NONE, LIFE_SUBMIT, LIFE_SUBMIT, LIFE_SUBMIT, LIFE_SUBMIT, LIFE_SUBMIT};
    CHECK(!life_case(channel, "constant buffer through submissions", 0, submits, 6, 1 | 4));
    /* the guest's CPU writes them between two submissions, in a new epoch: the new bytes */
    const u8 cpu[] = {LIFE_NONE, LIFE_SUBMIT, LIFE_SUBMIT, LIFE_SUBMIT, LIFE_CPU, LIFE_SUBMIT};
    CHECK(!life_case(channel, "constant buffer written by the CPU", 0, cpu, 6, 1));
    /* the GPU's front end writes them in the same epoch (a GPU-side write's stamp) */
    const u8 front[] = {LIFE_NONE, LIFE_SUBMIT, LIFE_SUBMIT, LIFE_SUBMIT, LIFE_FRONT, LIFE_SUBMIT};
    CHECK(!life_case(channel, "constant buffer written by the 3D engine", 0, front, 6, 1));
    /* a storage result over them, read pending, then landed, then kept again */
    const u8 result[] = {LIFE_NONE, LIFE_SUBMIT, LIFE_SUBMIT, LIFE_RESULT, LIFE_LAND, LIFE_SUBMIT};
    CHECK(!life_case(channel, "constant buffer under a storage result that lands", 0, result, 6, 0));
    /* presents that use each frame slot's rings again, with other bytes */
    const u8 frames[] = {LIFE_NONE, LIFE_SUBMIT, LIFE_FRAMES, LIFE_NONE, LIFE_FRAMES, LIFE_SUBMIT};
    CHECK(!life_case(channel, "constant buffer through presents", 0, frames, 6, 0));
    /* a texture's view and sampler kept with its headers, then the header written by the CPU, then the sampler by the
       GPU */
    const u8 headers[] = {LIFE_NONE, LIFE_SUBMIT, LIFE_NONE, LIFE_TIC, LIFE_SUBMIT, LIFE_NONE};
    CHECK(!life_case(channel, "texture headers written", 1, headers, 6, 2 | 4));
    const u8 tex_frames[] = {LIFE_NONE, LIFE_NONE, LIFE_FRAMES, LIFE_NONE, LIFE_TSC, LIFE_NONE};
    CHECK(!life_case(channel, "texture through presents", 1, tex_frames, 6, 2));
    channel->ds.bind.cb_addr[4][2] = channel->ds.bind.cb_addr[4][3] = 0;
    channel->ds.bind.cb_size[4][2] = channel->ds.bind.cb_size[4][3] = 0;
    r[0x982] = 0;
    return 0;
}

/* A draw's fragment key kept from the draw before while compute results are pending (render.c's fs_key): a dispatch
   on the compute queue stores one word through a storage buffer, its result pending; the first draw after it makes its
   key again (a result was added), the second keeps it. A store of 1.0 into the texture's texel 0 makes the next draw's
   key again too, and its resolve lands the result: red 1.0, where the texture read 0.75. The draws into one target,
   cleared once (a clear is a GPU-side write, after which a key is made again). Drawn as it is and with
   draw_state_check=1, which makes a kept key again and finds no difference */
#define PENDING_CB   (MEMO_GPU + 0x3e800) /* the dispatch's constant buffer 0, its storage buffer at 0x110 */
#define PENDING_CODE (MEMO_GPU + 0x11400)
static int pending_dispatch(u64 word_at) {
    const u64 program[] = {0x4c98078004470000ull,                       /* MOV R0, c[0][0x110] */
                           0x4c98078004570001ull,                       /* MOV R1, c[0][0x114] */
                           0x0100000000071002ull | 0x3f800000ull << 20, /* MOV32I R2, 1.0 */
                           0xeedc200000070002ull,                       /* STG.E [R0], R2 */
                           0xe30000000007000full};
    u8 code[64] = {0}, descriptor[16] = {0};
    u32 at = 0;
    for (u32 i = 0; i < sizeof program / sizeof *program; i++) {
        if (!(i % 3)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, program[i]);
    }
    CHECK(!gmem_write(PENDING_CODE, code, sizeof code));
    snx_st_le32(descriptor, (u32)word_at);
    snx_st_le32(descriptor + 4, (u32)(word_at >> 32));
    snx_st_le32(descriptor + 8, 4);
    CHECK(!gmem_write(PENDING_CB + 0x110, descriptor, sizeof descriptor));
    u32 qmd[64] = {0};
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)PENDING_CB;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(PENDING_CB >> 32) | 0x200u << 15;
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    if (!ch) return 1;
    u64 dispatches = g_vk_dispatches;
    vk_lock();
    /* the CPU read the word lately: the dispatch runs on the compute queue, its result pending until it lands, not
       kept on the GPU */
    vk_own_cpu_touched(MEMO_CPU + (word_at - MEMO_GPU), 4);
    DispatchState d;
    mx_dispatch_state(qmd, ch->rcomp, PENDING_CODE, &d);
    vk_dispatch(&d);
    vk_unlock();
    free(ch);
    CHECK(g_vk_dispatches == dispatches + 1);
    return 0;
}
/* one draw of the texture program into the first target: its pixel, and whether its fragment key was the draw
   before's */
static int pending_draw(GpuChannel* channel, u8 px[4], int* kept) {
    u32* r = channel->r3d;
    const u64 target = MEMO_GPU + 0x20000;
    VkDrawCounts before, after;
    vk_draw_counts(&before);
    vk_lock();
    r[0x851] = LIFE_TEX_PROGRAM;
    r[0x458] = 0 | 1u << 21 | 7u << 27;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_render_end();
    Surf* t = vk_rt_find(target);
    CHECK(t);
    if (t) CHECK(!read_pixel_at(t, 0, 0, px));
    vk_unlock();
    vk_draw_counts(&after);
    *kept = after.fs[VK_FS_KEPT] == before.fs[VK_FS_KEPT] + 1;
    return 0;
}
static int compute_pending_cases(GpuChannel* channel) {
    if (vk_uma()) {
        puts("SKIP fragment keys under pending compute results: memory is shared with the CPU, where a dispatch writes "
             "in place");
        return 0;
    }
    u32* r = channel->r3d;
    CHECK(!life_headers(0, 0)); /* the 2x1 R32F texture of 0 and 1, sampled linearly at 5/8: red 0.75 */
    u8 texels[8] = {0};
    snx_st_le32(texels + 4, 0x3f800000);
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffer 2 of the fragment stage: the texture's handle, 0 */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    int saved_check = g_settings.draw_state_check;
    for (int check = 0; check < 2; check++) {
        g_settings.draw_state_check = check;
        CHECK(!gmem_write(LIFE_POOL + 0x400, texels, sizeof texels));
        vk_ww_new_epoch();
        vk_lock();
        rt_clear(channel, MEMO_GPU + 0x20000, MEMO_SIDE, MEMO_SIDE, 0, 0);
        vk_unlock();
        VkDrawCounts before, after;
        vk_draw_counts(&before);
        u8 px[4][4];
        int kept[4], pending[2];
        CHECK(!pending_draw(channel, px[0], &kept[0]));
        CHECK(!pending_dispatch(MEMO_GPU + 0x3f000)); /* a word no draw reads */
        CHECK(!pending_draw(channel, px[1], &kept[1]));
        CHECK(!pending_draw(channel, px[2], &kept[2]));
        pending[0] = atomic_load_explicit(&g_cs_any, memory_order_relaxed);
        CHECK(!pending_dispatch(LIFE_POOL + 0x400)); /* the texture's texel 0: 1.0 */
        pending[1] = atomic_load_explicit(&g_cs_any, memory_order_relaxed);
        CHECK(!pending_draw(channel, px[3], &kept[3]));
        vk_draw_counts(&after);
        static const u8 three[4] = {191, 0, 0, 255}, one[4] = {255, 0, 0, 255};
        int ok = !kept[1] && kept[2] && !kept[3] && pending[0] && pending[1] && after.differs == before.differs;
        for (int i = 0; i < 4; i++) ok &= life_near(px[i], i < 3 ? three : one);
        if (!ok)
            fprintf(stderr,
                    "fragment keys under pending compute results%s: kept %d %d %d (expected 0 1 0), pending %d %d, "
                    "pixels %u %u %u %u (expected 191 191 191 255 red), %llu differences\n",
                    check ? " with draw_state_check=1" : "", kept[1], kept[2], kept[3], pending[0], pending[1],
                    px[0][0], px[1][0], px[2][0], px[3][0], (unsigned long long)(after.differs - before.differs));
        CHECK(ok);
        vk_lock();
        vk_compute_land();
        vk_finish();
        vk_unlock();
    }
    CHECK(!gmem_write(LIFE_POOL + 0x400, texels, sizeof texels)); /* texel 0 back to 0 for the cases after */
    vk_ww_new_epoch();
    g_settings.draw_state_check = saved_check;
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    r[0x982] = 0;
    return 0;
}

/* The texture memo's checks by the generations of the addresses it read (render.c tex_memo_unwritten and
   tex_memo_unpended, vk/addr_gens.h). The texture program samples the 2x1 R32F texture of compute_pending_cases (red
   0.75). A write to other memory, in a granule of its own, by a dispatch and by the CPU, two times each: the second
   check keeps the texture by the generations alone, with no resolve and the same pixel. Then the writes that must
   miss or load again: the CPU writes the texture's texel 0 (red one, then 0.75 again), the CPU writes the header
   (its red read as one), and a dispatch stores 1.0 into texel 0 (red one) */
#include "nx/gpu/vk/addr_gens.h"
#define GENS_OTHER_GPU (MEMO_GPU + 0x200000)
#define GENS_OTHER_CPU (MEMO_CPU + 0x200000)
/* a draw of pending_draw's, its pixel's red against want, and the counts it added into *d */
static int gens_draw(GpuChannel* channel, u8 want, const char* what, VkDrawCounts* d) {
    VkDrawCounts before, after;
    u8 px[4];
    int kept;
    vk_draw_counts(&before);
    CHECK(!pending_draw(channel, px, &kept));
    vk_draw_counts(&after);
    for (int i = 0; i < VK_GENS_N; i++) d->gens[i] = after.gens[i] - before.gens[i];
    for (int i = 0; i < VK_TEX_N; i++) d->tex[i] = after.tex[i] - before.tex[i];
    const int within = px[0] + 2 >= want && px[0] <= want + 2;
    if (!within)
        fprintf(stderr, "memo generations, %s: red %u, expected %u (%llu resolved, gens %llu %llu %llu %llu)\n", what,
                px[0], want, (unsigned long long)d->tex[VK_TEX_RESOLVED],
                (unsigned long long)d->gens[VK_GENS_HEADERS_KEPT], (unsigned long long)d->gens[VK_GENS_HEADERS_CHECKED],
                (unsigned long long)d->gens[VK_GENS_RESULTS_KEPT], (unsigned long long)d->gens[VK_GENS_RESULTS_CHECKED]);
    CHECK(within);
    return 0;
}
/* the CPU writes word at cpu and a submission starts an epoch, as the guest's queue submission does */
static void gens_cpu_word(u64 cpu, u32 word) {
    snx_st_le32(GP(cpu), word);
    vk_ww_new_epoch();
}
/* the sampler pool of the fragment stage at gva, its sampler 0 the cases' (life_headers) */
static int gens_samplers(GpuChannel* channel, u64 gva) {
    u8 sampler[32];
    CHECK(!gmem_read(LIFE_POOL + 0x100, sampler, sizeof sampler) && !gmem_write(gva, sampler, sizeof sampler));
    channel->r3d[0x557] = (u32)(gva >> 32);
    channel->r3d[0x558] = (u32)gva;
    return 0;
}
/* A way of the memo that is kept anew for another key takes nothing of the generations it had: its first check reads
   the headers. The texture through five sampler pools, one more than a set has ways, so the last takes the way of the
   first, whose last check took the sum of its headers' granule (header_cpu's, two times). The last one's sampler is in
   a granule of its own: once the texture's header is written, that granule is moved until the sum of the two is the
   first one's. Taken for the last check's, it kept the header from before */
static int gens_way_again(GpuChannel* channel, u64 header_cpu) {
    const u64 last = GENS_OTHER_GPU + 0x100, last_cpu = GENS_OTHER_CPU + 0x100;
    VkDrawCounts d;
    u32 tic[8], g = 0, h = 0;
    vk_ww_new_epoch();
    CHECK(!gens_draw(channel, 191, "the first pool", &d));
    for (int i = 0; i < 64; i++) addr_gens_bump(ADDR_GENS_CPU, header_cpu, 32); /* room for the last one's sum */
    gens_cpu_word(GENS_OTHER_CPU, 0x5ca1ab1eu);
    CHECK(!gens_draw(channel, 191, "the first pool, its headers checked", &d));
    CHECK(d.gens[VK_GENS_HEADERS_CHECKED] && vk_ww_gen(GP(header_cpu), 32, &g));
    const u32 sum = 2 * g;
    for (u32 i = 1; i < 4; i++) {
        CHECK(!gens_samplers(channel, LIFE_POOL + 0x100 + i * 0x20));
        CHECK(!gens_draw(channel, 191, "a pool beside it", &d));
    }
    CHECK(!gens_samplers(channel, last));
    CHECK(!gens_draw(channel, 191, "the last pool", &d));
    CHECK(d.tex[VK_TEX_RESOLVED]);
    life_tic(tic, 1);
    for (u32 i = 0; i < 8; i++) snx_st_le32(GP(header_cpu) + i * 4, tic[i]);
    vk_ww_new_epoch();
    vk_lock();
    CHECK(!vk_ww_take(GP(header_cpu), 32) && !vk_ww_take(GP(last_cpu), 32));
    CHECK(vk_ww_gen(GP(header_cpu), 32, &g) && vk_ww_gen(GP(last_cpu), 32, &h));
    for (; g + h < sum; h++) addr_gens_bump(ADDR_GENS_CPU, last_cpu, 8);
    CHECK(vk_ww_gen(GP(last_cpu), 32, &h) && g + h == sum);
    vk_unlock();
    CHECK(!gens_draw(channel, 255, "the last pool, after the CPU wrote the header", &d));
    CHECK(d.tex[VK_TEX_RESOLVED] && !d.gens[VK_GENS_HEADERS_KEPT]);
    CHECK(!gens_samplers(channel, LIFE_POOL + 0x100) && !life_headers(0, 0));
    vk_ww_new_epoch();
    CHECK(!gens_draw(channel, 191, "the first pool again", &d));
    return 0;
}
static int memo_gens_cases(GpuChannel* channel) {
    if (vk_uma()) {
        puts("SKIP memo generations: memory is shared with the CPU, where a dispatch writes in place");
        return 0;
    }
    static int mapped;
    if (!mapped) {
        CHECK(mem_map(GENS_OTHER_CPU, 0x1000, MS_NORMAL, PERM_RW) == 0);
        gmmu_map(GENS_OTHER_GPU, GENS_OTHER_CPU, 0x1000);
        mapped = 1;
    }
    u32* r = channel->r3d;
    CHECK(!life_headers(0, 0));
    u8 texels[8] = {0};
    snx_st_le32(texels + 4, 0x3f800000);
    CHECK(!gmem_write(LIFE_POOL + 0x400, texels, sizeof texels));
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffer 2 of the fragment stage: the texture's handle, 0 */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    int saved_check = g_settings.draw_state_check;
    g_settings.draw_state_check = 0;
    vk_ww_new_epoch();
    vk_lock();
    rt_clear(channel, MEMO_GPU + 0x20000, MEMO_SIDE, MEMO_SIDE, 0, 0);
    const u64 header_cpu = MEMO_CPU + (LIFE_POOL - MEMO_GPU), texel_cpu = header_cpu + 0x400;
    const int tracking = vk_written_since(GP(header_cpu), 32, 0) >= 0;
    vk_unlock();
    VkDrawCounts d[2];
    CHECK(!gens_draw(channel, 191, "the first draw", &d[0]));
    /* a dispatch's result elsewhere, two times: the second check is kept by the generations, and nothing resolves */
    for (int k = 0; k < 2; k++) {
        CHECK(!pending_dispatch(GENS_OTHER_GPU));
        CHECK(!gens_draw(channel, 191, "after a result elsewhere", &d[k]));
        CHECK(!d[k].tex[VK_TEX_RESOLVED]);
    }
    if (!d[1].gens[VK_GENS_RESULTS_KEPT])
        fprintf(stderr, "memo generations: a second result elsewhere was checked again (%llu checked)\n",
                (unsigned long long)d[1].gens[VK_GENS_RESULTS_CHECKED]);
    CHECK(d[1].gens[VK_GENS_RESULTS_KEPT] && !d[1].gens[VK_GENS_RESULTS_CHECKED]);
    vk_lock();
    vk_compute_land();
    vk_finish();
    vk_unlock();
    /* the CPU writes elsewhere, two times, each with an epoch: the second check of the headers is kept by the
       generations, where the write tracking runs and reports no write on the headers' page. With hot pages
       (write_tracking_hot_pages=1), the cases before wrote that page often enough that it reads as written at each
       scan for a while, and the headers are checked again, as they must be */
    const u32 before = vk_ww_now();
    for (int k = 0; k < 2; k++) {
        gens_cpu_word(GENS_OTHER_CPU, 0x12345678u + (u32)k);
        CHECK(!gens_draw(channel, 191, "after a CPU write elsewhere", &d[k]));
        CHECK(!d[k].tex[VK_TEX_RESOLVED]);
    }
    vk_lock();
    const int header_quiet = vk_written_since(GP(header_cpu), 32, before) == 0;
    vk_unlock();
    if (tracking && !header_quiet)
        puts("memo generations: the headers' page read as written (hot pages), so the second check was not asked to be "
             "kept");
    if (tracking && header_quiet) {
        if (!d[1].gens[VK_GENS_HEADERS_KEPT])
            fprintf(stderr, "memo generations: a second CPU write elsewhere was checked again (%llu checked)\n",
                    (unsigned long long)d[1].gens[VK_GENS_HEADERS_CHECKED]);
        CHECK(d[1].gens[VK_GENS_HEADERS_KEPT] && !d[1].gens[VK_GENS_HEADERS_CHECKED]);
    }
    /* the CPU writes the texture memory: the new texel, then the old one */
    gens_cpu_word(texel_cpu, 0x3f800000);
    CHECK(!gens_draw(channel, 255, "after the CPU wrote texel 0", &d[0]));
    gens_cpu_word(texel_cpu, 0);
    CHECK(!gens_draw(channel, 191, "after the CPU wrote texel 0 back", &d[0]));
    /* the CPU writes the header between two draws: its red read as one, a miss */
    u32 tic[8];
    life_tic(tic, 1);
    for (u32 i = 0; i < 8; i++) snx_st_le32(GP(header_cpu) + i * 4, tic[i]);
    vk_ww_new_epoch();
    CHECK(!gens_draw(channel, 255, "after the CPU wrote the header", &d[0]));
    CHECK(d[0].tex[VK_TEX_RESOLVED] && !d[0].gens[VK_GENS_HEADERS_KEPT]);
    CHECK(!life_headers(0, 0));
    vk_ww_new_epoch();
    CHECK(!gens_draw(channel, 191, "after the header written back", &d[0]));
    /* a dispatch writes the texture memory: a miss, which lands the result. A result elsewhere first, whose check
       takes the generations again after the header's miss kept the way anew: the store is then asked of them */
    CHECK(!pending_dispatch(GENS_OTHER_GPU));
    CHECK(!gens_draw(channel, 191, "after a result elsewhere again", &d[0]));
    CHECK(!pending_dispatch(LIFE_POOL + 0x400));
    CHECK(!gens_draw(channel, 255, "after a dispatch wrote texel 0", &d[0]));
    CHECK(d[0].tex[VK_TEX_RESOLVED] && !d[0].gens[VK_GENS_RESULTS_KEPT]);
    vk_lock();
    vk_compute_land();
    vk_finish();
    vk_unlock();
    CHECK(!gmem_write(LIFE_POOL + 0x400, texels, sizeof texels)); /* texel 0 back to 0 for the cases after */
    if (tracking) CHECK(!gens_way_again(channel, header_cpu));
    vk_ww_new_epoch();
    g_settings.draw_state_check = saved_check;
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    r[0x982] = 0;
    printf("PASS memo generations: writes elsewhere by a dispatch and by the CPU kept by the generations%s; the "
           "texture's memory and header written by the CPU and by a dispatch drawn new\n",
           tracking ? "" : " (the CPU's not: no write tracking)");
    return 0;
}

/* A draw's fragment key kept from the draw before where its texture lies over a render target without being it
   (render.c's fs_key, TexOver): the texture's header reads the 4x4 RGBA8 target at OVER_TARGET as R32F, the same bytes
   in another format, so the draws sample a copy of the target. The second draw of a pair keeps the first one's key.
   Then another program draws into that target and the game's submission ends, after which a copy is made again:
   the next pair's first draw makes the copy and the key again (the target's write generation is all that moved), its
   second keeps the key. A key made again after another program's draw finds the texture without the memo, and a
   draw into the sampled target itself keeps no key. Drawn as it is and with draw_state_check=1, which makes a kept
   key again and finds no difference */
#define OVER_TARGET (MEMO_GPU + 0x34000)
/* the 4x4 RGBA8 target at gva bound as the first color target, as rt_clear binds it, with no clear */
static void over_bind(GpuChannel* channel, u64 gva) {
    u32* r = channel->r3d;
    r[0x487] = 1;
    r[0x200] = (u32)(gva >> 32);
    r[0x201] = (u32)gva;
    r[0x202] = r[0x203] = MEMO_SIDE;
    r[0x204] = 0xD5;
    r[0x205] = 0;
    r[0x206] = 1;
    r[0x207] = 0;
}
static int over_target_cases(GpuChannel* channel) {
    u32* r = channel->r3d;
    u32 tic[8];
    u8 bytes[32];
    CHECK(!life_headers(0, 0));
    life_tic(tic, 0);
    tic[1] = (u32)OVER_TARGET;
    tic[2] = (u32)(OVER_TARGET >> 32) | 3u << 21;
    tic[4] = (MEMO_SIDE - 1) | 1u << 23;
    tic[5] = (MEMO_SIDE - 1) | 1u << 31;
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(LIFE_POOL, bytes, 32));
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffer 2 of the fragment stage: the texture's handle, 0 */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    int saved_check = g_settings.draw_state_check;
    for (int check = 0; check < 2; check++) {
        g_settings.draw_state_check = check;
        VkDrawCounts before, after;
        vk_draw_counts(&before);
        u8 px[4][4];
        int kept[4];
        u64 copies[3];
        vk_ww_new_epoch();
        copies[0] = g_vk_rt_copy_writes;
        rt_clear(channel, OVER_TARGET, MEMO_SIDE, MEMO_SIDE, 0, 1);
        vk_release_submit(); /* a submission of the game's ended: the copy is made again in each run */
        rt_clear(channel, MEMO_GPU + 0x20000, MEMO_SIDE, MEMO_SIDE, 0, 0); /* the draws' target, bound again */
        CHECK(!pending_draw(channel, px[0], &kept[0]));
        CHECK(!pending_draw(channel, px[1], &kept[1]));
        copies[1] = g_vk_rt_copy_writes;
        /* a color program's draw into the sampled target, and the submission's end that lets a copy be made again */
        u64 drawn = g_vk_drawn;
        vk_lock();
        over_bind(channel, OVER_TARGET);
        r[0x851] = MEMO_GREEN;
        r[0x458] = 0 | 1u << 21 | 7u << 27;
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        DrawCall call = {.topology = 4, .count = 3, .instances = 1};
        vk_render_draw(&channel->ds, &call, &channel->owner);
        vk_render_end();
        over_bind(channel, MEMO_GPU + 0x20000);
        vk_unlock();
        vk_release_submit();
        CHECK(g_vk_drawn == drawn + 1);
        CHECK(!pending_draw(channel, px[2], &kept[2]));
        CHECK(!pending_draw(channel, px[3], &kept[3]));
        copies[2] = g_vk_rt_copy_writes;
        /* the color program's draw into the draws' own target: the texture program's next draw makes its key again
           (another program drew), and finds its texture with no lookup through the memo, as nothing of the texture's
           moved. With draw_state_check=1 every lookup goes through it */
        vk_lock();
        r[0x851] = MEMO_GREEN;
        channel->r3d_dirty = ~0u;
        mx_draw_state_sync(channel);
        vk_render_draw(&channel->ds, &call, &channel->owner);
        vk_render_end();
        vk_unlock();
        VkDrawCounts memo0, memo1;
        int kept_after, kept_own[2];
        u8 unused[4];
        vk_draw_counts(&memo0);
        CHECK(!pending_draw(channel, unused, &kept_after));
        vk_draw_counts(&memo1);
        u64 through = memo1.tex[VK_TEX_MEMO] - memo0.tex[VK_TEX_MEMO];
        /* the texture program's draws into the target they sample a copy of: each moves that target's write
           generation, so the second one's key is made again */
        vk_lock();
        over_bind(channel, OVER_TARGET);
        vk_unlock();
        CHECK(!pending_draw(channel, unused, &kept_own[0]));
        CHECK(!pending_draw(channel, unused, &kept_own[1]));
        vk_lock();
        over_bind(channel, MEMO_GPU + 0x20000);
        vk_unlock();
        vk_draw_counts(&after);
        int ok = !kept[0] && kept[1] && !kept[2] && kept[3] && copies[1] > copies[0] && copies[2] > copies[1] &&
                 after.differs == before.differs && !memcmp(px[0], px[1], 4) && !memcmp(px[2], px[3], 4) &&
                 !kept_after && (check ? through > 0 : !through) && !kept_own[1];
        if (kept_after || (check ? !through : through) || kept_own[1])
            fprintf(stderr,
                    "fragment keys over a copy of a target%s: after another program's draw the key was kept %d "
                    "(expected 0) with %llu lookups through the memo (expected %s); the second draw into the sampled "
                    "target kept its key %d (expected 0)\n",
                    check ? " with draw_state_check=1" : "", kept_after, (unsigned long long)through,
                    check ? "1 or more" : "0", kept_own[1]);
        if (!ok)
            fprintf(stderr,
                    "fragment keys over a copy of a target%s: kept %d %d %d %d (expected 0 1 0 1), %llu and %llu "
                    "copies of the target written (1 or more each expected), pixels' red %u %u and %u %u (each pair "
                    "the same expected), %llu differences\n",
                    check ? " with draw_state_check=1" : "", kept[0], kept[1], kept[2], kept[3],
                    (unsigned long long)(copies[1] - copies[0]), (unsigned long long)(copies[2] - copies[1]), px[0][0],
                    px[1][0], px[2][0], px[3][0], (unsigned long long)(after.differs - before.differs));
        CHECK(ok);
    }
    g_settings.draw_state_check = saved_check;
    CHECK(!life_headers(0, 0));
    vk_ww_new_epoch();
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    r[0x982] = 0;
    return 0;
}

/* The pool map in shadow mode (vk/pool_map.c, pool_map_shadow): the texture program's draws fill the record of TIC
   index 0, which stays valid and agrees with each resolve after it. Each event clears it, and the next draw fills it
   again: the CPU writes the header between two draws, the GPU writes it back, the texture's image is swapped, a view
   of the record is destroyed, and the target that a record's texture crops is drawn again. Pixels as without the map,
   and no difference, in each run, the second with draw_state_check=1 */
#include "nx/gpu/vk/pool_map.h"
/* whether the record of TIC index 0 of the draws' texture pool is valid now (the events taken in first) */
static int pm_valid(GpuChannel* channel) {
    vk_lock();
    const int valid = vk_pool_map_valid(channel->ds.bind.tex_pool, 0);
    vk_unlock();
    return valid;
}
/* one draw of the texture program: its pixel's red against want */
static int pm_draw(GpuChannel* channel, u8 want, const char* what) {
    u8 px[4];
    int kept;
    CHECK(!pending_draw(channel, px, &kept));
    if (px[0] + 2 < want || px[0] > want + 2)
        fprintf(stderr, "pool map, %s: red %u, expected %u\n", what, px[0], want);
    CHECK(px[0] + 2 >= want && px[0] <= want + 2);
    return 0;
}
/* event cleared the record of TIC index 0: valid after two draws, not valid after event, valid after the next draw */
#define PM_CLEARS(what, want, ...)                                                                                      \
    do {                                                                                                               \
        CHECK(!pm_draw(channel, want, what));                                                                          \
        CHECK(!pm_draw(channel, want, what));                                                                          \
        const int before_event = pm_valid(channel);                                                                    \
        __VA_ARGS__;                                                                                                   \
        const int after_event = pm_valid(channel);                                                                     \
        if (!before_event || after_event)                                                                              \
            fprintf(stderr, "pool map, %s: valid %d before the event and %d after it (expected 1 and 0)\n", what,      \
                    before_event, after_event);                                                                        \
        CHECK(before_event && !after_event);                                                                           \
    } while (0)
/* every view of the texture tic names destroyed, as the end of its frame destroys them (vk_image_view_destroy) */
static void pm_views_gone(const u32* tic) {
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    for (int i = 0; t && i < t->nsv; i++) vk_pool_map_view_gone(t->sv[i].v);
    vk_unlock();
}
static void pm_swapped(const u32* tic) {
    vk_lock();
    vk_pool_map_swapped(vk_tex_from_tic(tic));
    vk_unlock();
}
static int pool_map_cases(GpuChannel* channel) {
    u32* r = channel->r3d;
    const int saved_check = g_settings.draw_state_check, saved_map = g_settings.pool_map_shadow;
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffer 2 of the fragment stage: the texture's handle, 0 */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    for (int check = 0; check < 2; check++) {
        g_settings.draw_state_check = check;
        g_settings.pool_map_shadow = 1;
        vk_lock();
        vk_pool_map_forget();
        vk_unlock();
        CHECK(!life_headers(0, 0)); /* the 2x1 R32F texture of 0 and 1: red 0.75 */
        u8 texels[8] = {0};
        snx_st_le32(texels + 4, 0x3f800000);
        CHECK(!gmem_write(LIFE_POOL + 0x400, texels, sizeof texels));
        vk_ww_new_epoch();
        vk_lock();
        rt_clear(channel, MEMO_GPU + 0x20000, MEMO_SIDE, MEMO_SIDE, 0, 0);
        vk_unlock();
        PoolMapCounts c0, c1;
        vk_pool_map_counts(&c0);
        u32 tic[8], tic_one[8];
        life_tic(tic, 0);
        life_tic(tic_one, 1);
        /* the CPU writes the header between two draws (its red read as one) */
        PM_CLEARS("the CPU writes the header", 191, {
            for (u32 i = 0; i < 8; i++) snx_st_le32(GP(MEMO_CPU + (LIFE_POOL - MEMO_GPU)) + i * 4, tic_one[i]);
            vk_ww_new_epoch();
        });
        /* the GPU writes it back */
        PM_CLEARS("the GPU writes the header", 255, CHECK(!life_headers(0, 0)));
        /* the texture's image swapped, and a view of the record destroyed */
        PM_CLEARS("the image swapped", 191, pm_swapped(tic));
        PM_CLEARS("a view destroyed", 191, pm_views_gone(tic));
        CHECK(!pm_draw(channel, 191, "after the events"));
        CHECK(!pm_draw(channel, 191, "after the events"));
        vk_pool_map_counts(&c1);
        u64 differ = 0;
        for (int i = 0; i < POOL_DIFF_N; i++) differ += c1.differ[i] - c0.differ[i];
        if (differ || c1.agree - c0.agree < 5)
            fprintf(stderr, "pool map%s: %llu agreed (5 or more expected), %llu differed (none expected)\n",
                    check ? " with draw_state_check=1" : "", (unsigned long long)(c1.agree - c0.agree),
                    (unsigned long long)differ);
        CHECK(!differ && c1.agree - c0.agree >= 5);
        /* a texture over a target (a copy of it in another format), and the target drawn again */
        u32 over[8];
        u8 bytes[32];
        life_tic(over, 0);
        over[1] = (u32)OVER_TARGET;
        over[2] = (u32)(OVER_TARGET >> 32) | 3u << 21;
        over[4] = (MEMO_SIDE - 1) | 1u << 23;
        over[5] = (MEMO_SIDE - 1) | 1u << 31;
        for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, over[i]);
        CHECK(!gmem_write(LIFE_POOL, bytes, 32));
        vk_ww_new_epoch();
        vk_lock();
        rt_clear(channel, OVER_TARGET, MEMO_SIDE, MEMO_SIDE, 0, 1);
        vk_unlock();
        vk_release_submit();
        vk_lock();
        rt_clear(channel, MEMO_GPU + 0x20000, MEMO_SIDE, MEMO_SIDE, 0, 0);
        vk_unlock();
        u8 px[4];
        int kept;
        CHECK(!pending_draw(channel, px, &kept));
        const u8 crop_red = px[0];
        PM_CLEARS("the cropped target drawn again", crop_red, {
            vk_lock();
            over_bind(channel, OVER_TARGET);
            r[0x851] = MEMO_GREEN;
            channel->r3d_dirty = ~0u;
            mx_draw_state_sync(channel);
            DrawCall call = {.topology = 4, .count = 3, .instances = 1};
            vk_render_draw(&channel->ds, &call, &channel->owner);
            vk_render_end();
            over_bind(channel, MEMO_GPU + 0x20000);
            vk_unlock();
            vk_release_submit();
        });
        vk_pool_map_counts(&c0);
        CHECK(!pending_draw(channel, px, &kept));
        vk_pool_map_counts(&c1);
        differ = 0;
        for (int i = 0; i < POOL_DIFF_N; i++) differ += c1.differ[i] - c0.differ[i];
        CHECK(!differ);
        CHECK(pm_valid(channel));
    }
    g_settings.draw_state_check = saved_check;
    g_settings.pool_map_shadow = saved_map;
    vk_lock();
    vk_pool_map_forget();
    vk_unlock();
    CHECK(!life_headers(0, 0));
    vk_ww_new_epoch();
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    r[0x982] = 0;
    puts("PASS pool map: a record of a texture pool agrees with each resolve while valid, and a header written by the "
         "CPU or the GPU, an image swap, a view destroyed and a cropped target drawn again each clear it");
    return 0;
}

/* A header that NVN's nvnTexturePoolRegisterTexture writes between two draws, with no submission between them, as an
   Ounce game's menus register a new texture at an index while the GPU thread replays the work before (nvn_object.c
   register_texture). No write tracking epoch starts, so only the record of that write as a GPU-side one
   (gpu_mem_written_cpu) tells the pool map: the record of the index must be cleared before the next compare. The
   texture and sampler pools of the draws, as an NVN pool over their memory: texture A, the 2x1 R32F texture of 0 and
   1, then texture B, the same over 1 and 1 */
#include "gpu/nvn/nvn.hook.h"
#define PMN_OBJECTS (MEMO_CPU + 0x3c000) /* the memory pool, the texture pool and the two textures' objects */
static void pmn_texture(u64 at, u64 gpu) {
    NvnTexture tex = {0};
    tex.d.target = NVN_TEXTURE_TARGET_2D;
    tex.d.format = NVN_FORMAT_R32F;
    tex.d.levels = 1;
    tex.d.width = 2;
    tex.d.height = 1;
    tex.d.depth = 1;
    tex.gpu = gpu;
    memcpy(GP(at), &tex, sizeof tex);
}
static int pool_map_nvn_case(GpuChannel* channel) {
    u32* r = channel->r3d;
    const int saved_map = g_settings.pool_map_shadow;
    const u64 pool = PMN_OBJECTS, tp = PMN_OBJECTS + 0x100, a = PMN_OBJECTS + 0x200, b = PMN_OBJECTS + 0x300;
    NvnPool mem = {0};
    mem.memory = MEMO_CPU + (LIFE_POOL - MEMO_GPU);
    mem.gpu = LIFE_POOL;
    mem.size = 0x1000;
    memcpy(GP(pool), &mem, sizeof mem);
    CHECK(nvn_TexturePoolInitialize(NULL, tp, pool, 0, 2));
    pmn_texture(a, LIFE_POOL + 0x400);
    pmn_texture(b, LIFE_POOL + 0x800);
    u8 texels[16] = {0};
    snx_st_le32(texels + 4, 0x3f800000);
    snx_st_le32(texels + 8, 0x3f800000);
    snx_st_le32(texels + 12, 0x3f800000);
    CHECK(!gmem_write(LIFE_POOL + 0x400, texels, 8));
    CHECK(!gmem_write(LIFE_POOL + 0x800, texels + 8, 8));
    CHECK(!life_headers(0, 0)); /* the sampler */
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffer 2 of the fragment stage: the texture's handle, 0 */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    g_settings.pool_map_shadow = 1;
    vk_lock();
    vk_pool_map_forget();
    vk_unlock();
    nvn_TexturePoolRegisterTexture(NULL, tp, 0, a, 0);
    vk_ww_new_epoch(); /* the game's submission */
    vk_lock();
    rt_clear(channel, MEMO_GPU + 0x20000, MEMO_SIDE, MEMO_SIDE, 0, 0);
    vk_unlock();
    u8 px[4];
    int kept;
    CHECK(!pending_draw(channel, px, &kept));
    CHECK(!pending_draw(channel, px, &kept));
    const int before = pm_valid(channel);
    nvn_TexturePoolRegisterTexture(NULL, tp, 0, b, 0); /* no submission: the same epoch */
    const int after = pm_valid(channel);
    PoolMapCounts c0, c1;
    vk_pool_map_counts(&c0);
    CHECK(!pending_draw(channel, px, &kept));
    CHECK(!pending_draw(channel, px, &kept));
    vk_pool_map_counts(&c1);
    u64 differ = 0;
    for (int i = 0; i < POOL_DIFF_N; i++) differ += c1.differ[i] - c0.differ[i];
    if (!before || after || differ)
        fprintf(stderr,
                "pool map, a header NVN registers in the same epoch: valid %d before and %d after (expected 1 and 0), "
                "%llu differences after (none expected)\n",
                before, after, (unsigned long long)differ);
    CHECK(before && !after && !differ);
    g_settings.pool_map_shadow = saved_map;
    vk_lock();
    vk_pool_map_forget();
    vk_unlock();
    CHECK(!life_headers(0, 0));
    u8 zero[8] = {0};
    snx_st_le32(zero + 4, 0x3f800000);
    CHECK(!gmem_write(LIFE_POOL + 0x400, zero, 8));
    vk_ww_new_epoch();
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    r[0x982] = 0;
    puts("PASS pool map: a header NVN registers between two draws in one epoch clears its record");
    return 0;
}
/* Work submitted with texture A at index 0, which the GPU thread replays while the game registers B there: it draws
   with A, as the registration lands after it (nvn_object.c descriptor_write). The call on the GPU thread draws once it
   sees what the registration does: B's descriptor queued in the replay's order (nvn_replay_write), which lands after
   this call, B's descriptor in the slot, or the wait for the submitted work queued (nvn_replay_finish_all's job,
   NVN_QUEUES + 1), which ends after this call */
typedef struct {
    GpuChannel* channel;
    u64 slot;
    u32 a[8];
    u8 px[4];
    int failed, waited, late;
} TxwDraw;
static int txw_waits(void) {
    NvnReplayState* st = &g_nvn.replay;
    int n = 0;
    pm_lock(&st->lock);
    for (int i = 0; i < NVN_JOBS; i++) n += st->jobs[i].queue == NVN_QUEUES + 1;
    pm_unlock(&st->lock);
    return n != 0;
}
static int txw_draw(void* arg) {
    TxwDraw* d = (TxwDraw*)arg;
    const u64 end = plat_ns() + 10000000000ull;
    int kept;
    /* until the registration is in the replay's order (nvn_replay_write), waits for the replay, or wrote the slot */
    while (!(d->waited = txw_waits() || nvn_replay_writes_pending()) && !memcmp(GP(d->slot), d->a, sizeof d->a)) {
        if (plat_ns() > end) {
            d->late = 1;
            break;
        }
        plat_sleep_ns(20000);
    }
    d->failed = pending_draw(d->channel, d->px, &kept);
    return 1;
}
static int texture_write_case(GpuChannel* channel) {
    u32* r = channel->r3d;
    const u64 pool = PMN_OBJECTS, tp = PMN_OBJECTS + 0x100, a = PMN_OBJECTS + 0x200, b = PMN_OBJECTS + 0x300;
    NvnPool mem = {0};
    mem.memory = MEMO_CPU + (LIFE_POOL - MEMO_GPU);
    mem.gpu = LIFE_POOL;
    mem.size = 0x1000;
    memcpy(GP(pool), &mem, sizeof mem);
    CHECK(nvn_TexturePoolInitialize(NULL, tp, pool, 0, 2));
    pmn_texture(a, LIFE_POOL + 0x400);
    pmn_texture(b, LIFE_POOL + 0x800);
    u8 texels[16] = {0}, px[4];
    int kept;
    snx_st_le32(texels + 4, 0x3f800000);
    snx_st_le32(texels + 8, 0x3f800000);
    snx_st_le32(texels + 12, 0x3f800000);
    CHECK(!gmem_write(LIFE_POOL + 0x400, texels, 8));     /* A: 0 and 1, filtered to 191 of 255 */
    CHECK(!gmem_write(LIFE_POOL + 0x800, texels + 8, 8)); /* B: 1 and 1 */
    CHECK(!life_headers(0, 0));                           /* the sampler */
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffer 2 of the fragment stage: the texture's handle, 0 */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    nvn_TexturePoolRegisterTexture(NULL, tp, 0, a, 0);
    vk_ww_new_epoch(); /* the game's submission */
    vk_lock();
    rt_clear(channel, MEMO_GPU + 0x20000, MEMO_SIDE, MEMO_SIDE, 0, 0);
    vk_unlock();
    CHECK(!pending_draw(channel, px, &kept));
    const u8 alone = px[0];
    TxwDraw d = {.channel = channel, .slot = mem.memory};
    memcpy(d.a, GP(d.slot), sizeof d.a);
    const u32 q = nvn_replay_queue(); /* a live queue: with none, finish_all has nothing to wait for */
    gpu_call_poll(txw_draw, &d, q - 1);
    nvn_TexturePoolRegisterTexture(NULL, tp, 0, b, 0); /* the same epoch */
    nvn_replay_finish(q);
    const int written = memcmp(GP(d.slot), d.a, sizeof d.a) != 0;
    CHECK(!pending_draw(channel, px, &kept));
    nvn_replay_queue_free(q);
    if (alone != 191 || d.px[0] != 191 || px[0] != 255 || !written || d.late || d.failed)
        fprintf(stderr,
                "a texture registered over another while the work before it replays: that work reads red %u, the draw "
                "after %u, and A alone %u (expected 191, 255 and 191); the registration was queued or waited %d, wrote %d, the draw "
                "gave up on it %d, failed %d\n",
                d.px[0], px[0], alone, d.waited, written, d.late, d.failed);
    CHECK(alone == 191 && d.px[0] == 191 && px[0] == 255 && written && !d.late && !d.failed);
    CHECK(!life_headers(0, 0));
    u8 zero[8] = {0};
    snx_st_le32(zero + 4, 0x3f800000);
    CHECK(!gmem_write(LIFE_POOL + 0x400, zero, 8));
    vk_ww_new_epoch();
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    r[0x982] = 0;
    puts("PASS a texture NVN registers over another lands after the work that samples it");
    return 0;
}

/* The barriers at the writer (writer_barriers, gpu/vk/surface.c vk_surf_written_flush), with unified image layouts. A
   pass writes target T with the color program (white or green, as the cases before left its code), a pass on another
   target begins (its batch takes T's write), and the next pass samples T: the drawn color, and today's reader barrier
   for T is one a batch covered. Then an upload and a sample in one draw: the CPU
   writes the texture's texel 0, and the draw's upload and sample give red one, the reader barrier one that the next
   pass's batch covers. The pixels as without the setting */
#define WB_T (MEMO_GPU + 0x34000) /* 4x4 RGBA8, as OVER_TARGET */
#define WB_V (MEMO_GPU + 0x24000)
static int writer_cases(GpuChannel* channel) {
    if (!g_vk.has_unified_layouts) {
        puts("SKIP barriers at the writer: the device has no unified image layouts");
        return 0;
    }
    u32* r = channel->r3d;
    const int saved = g_settings.writer_barriers;
    /* SNX_TEST_WRITER_OFF=1: the same commands with the setting off, for the validation layer's messages to compare */
    const int on = !getenv("SNX_TEST_WRITER_OFF");
    g_settings.writer_barriers = on;
    VkWriterCounts c0, c1;
    /* a pass writes T, a pass on V begins, a pass samples T */
    u32 tic[8] = {0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28,
                  (u32)WB_T,
                  (u32)(WB_T >> 32) | 3u << 21,
                  0,
                  (MEMO_SIDE - 1) | 1u << 23,
                  (MEMO_SIDE - 1) | 1u << 31,
                  0,
                  0};
    u8 bytes[32];
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!life_headers(0, 0)); /* the sampler, linear */
    CHECK(!gmem_write(LIFE_POOL, bytes, 32));
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffer 2 of the fragment stage: the texture's handle, 0 */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    vk_ww_new_epoch();
    vk_surf_writer_counts(&c0);
    rt_clear(channel, WB_T, MEMO_SIDE, MEMO_SIDE, 0, 1);
    vk_lock();
    over_bind(channel, WB_T);
    r[0x851] = MEMO_GREEN;
    r[0x458] = 0 | 1u << 21 | 7u << 27;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_render_end();
    u8 drawn[4] = {0}; /* the color program's color (an earlier case made it white) */
    Surf* t = vk_rt_find(WB_T);
    CHECK(t && !read_pixel_at(t, 2, 2, drawn));
    vk_unlock();
    rt_clear(channel, WB_V, MEMO_SIDE, MEMO_SIDE, 0, 1); /* a pass on V: its batch takes T's write */
    rt_clear(channel, MEMO_GPU + 0x20000, MEMO_SIDE, MEMO_SIDE, 0, 0);
    u8 px[4];
    int kept;
    CHECK(!pending_draw(channel, px, &kept));
    vk_surf_writer_counts(&c1);
    const int same = !memcmp(px, drawn, 4) && drawn[0] + drawn[1] + drawn[2] > 0; /* drawn, not the clear's red */
    if (!on) c1.batched = c0.batched + 1, c1.covered = c0.covered + 1; /* no counts with the setting off */
    if (!same || drawn[1] != 255 || c1.batched == c0.batched || c1.covered == c0.covered)
        fprintf(stderr,
                "barriers at the writer, a pass's target sampled: pixel %u,%u,%u,%u (the target holds %u,%u,%u,%u), "
                "%llu surfaces batched, %llu reader barriers covered (1 or more each expected)\n",
                px[0], px[1], px[2], px[3], drawn[0], drawn[1], drawn[2], drawn[3],
                (unsigned long long)(c1.batched - c0.batched), (unsigned long long)(c1.covered - c0.covered));
    CHECK(same && drawn[1] == 255 && c1.batched > c0.batched && c1.covered > c0.covered);
    /* an upload and a sample: the 2x1 R32F texture, texel 0 written by the CPU */
    CHECK(!life_headers(0, 0));
    u8 texels[8] = {0};
    snx_st_le32(texels + 4, 0x3f800000);
    CHECK(!gmem_write(LIFE_POOL + 0x400, texels, sizeof texels));
    vk_ww_new_epoch();
    CHECK(!pending_draw(channel, px, &kept));
    CHECK(life_near(px, (const u8[4]){191, 0, 0, 255}));
    snx_st_le32(GP(MEMO_CPU + (LIFE_POOL + 0x400 - MEMO_GPU)), 0x3f800000);
    vk_ww_new_epoch();
    vk_surf_writer_counts(&c0);
    CHECK(!pending_draw(channel, px, &kept));
    vk_surf_writer_counts(&c1);
    if (!on) c1.covered = c0.covered + 1;
    if (!life_near(px, (const u8[4]){255, 0, 0, 255}) || c1.covered == c0.covered)
        fprintf(stderr,
                "barriers at the writer, an upload sampled: red %u (expected 255), %llu reader barriers covered (1 or "
                "more expected)\n",
                px[0], (unsigned long long)(c1.covered - c0.covered));
    CHECK(life_near(px, (const u8[4]){255, 0, 0, 255}) && c1.covered > c0.covered);
    CHECK(!gmem_write(LIFE_POOL + 0x400, texels, sizeof texels)); /* texel 0 back to 0 for the cases after */
    vk_ww_new_epoch();
    g_settings.writer_barriers = saved;
    channel->ds.bind.cb_addr[4][2] = 0;
    channel->ds.bind.cb_size[4][2] = 0;
    r[0x982] = 0;
    puts("PASS barriers at the writer: a pass's target and an upload sampled after them, their writes in batches or "
         "listed for the next pass's batch");
    return 0;
}

/* Inline constant blocks, as NVN's front end gives them (DsInline, its per-block generations DsState.inl_gen): the
   constant buffer program reads its color from slot 3, the fragment stage's bank 2 block. A draw's new base vertex
   moves every stage's bank 0 block and the inline generation, not bank 2: the next draw keeps bank 2's staged copy.
   A new color in bank 2 is staged again and drawn */
static void inline_set(DsState* ds, int bank0, const float* color) {
    if (bank0)
        for (int s = 0; s < DS_GFX_STAGES; s++) {
            snx_st_le32(ds->inl.bank0[s] + SH_CB0_BASE_VERTEX, (u32)(19 + s));
            ds->inl_gen[0][s] = ds_generation_next();
        }
    if (color) {
        life_floats(ds->inl.bank2[4], color, 4);
        ds->inl_gen[1][4] = ds_generation_next();
    }
    ds->gen[DS_INLINE] = ds_generation_next();
}
static int inline_draw(GpuChannel* channel, u8 px[4], u64* kept, u64* staged) {
    u32* r = channel->r3d;
    const u64 target = MEMO_GPU + 0x20000;
    VkDrawCounts before, after;
    vk_draw_counts(&before);
    vk_lock();
    rt_clear(channel, target, MEMO_SIDE, MEMO_SIDE, 0, 0);
    r[0x851] = LIFE_CB_PROGRAM;
    r[0x458] = 0 | 1u << 21 | 7u << 27;
    channel->r3d_dirty = ~0u;
    mx_draw_state_sync(channel);
    channel->ds.bind.cb_inline[4][3] = DS_CB_BANK2;
    channel->ds.gen[DS_BIND] = ds_generation_next();
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    u64 drawn = g_vk_drawn;
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_render_end();
    CHECK(g_vk_drawn == drawn + 1);
    Surf* t = vk_rt_find(target);
    CHECK(t);
    if (t) CHECK(!read_pixel_at(t, 0, 0, px));
    vk_unlock();
    vk_draw_counts(&after);
    *kept = after.inl[2][0] - before.inl[2][0];
    *staged = after.inl[2][1] - before.inl[2][1];
    return 0;
}
static int inline_block_cases(GpuChannel* channel) {
    DsState* ds = &channel->ds;
    int saved_check = g_settings.draw_state_check;
    static const u8 green[4] = {0, 255, 0, 255}, blue[4] = {0, 0, 255, 255};
    for (int check = 0; check < 2; check++) {
        g_settings.draw_state_check = check;
        VkDrawCounts before, after;
        vk_draw_counts(&before);
        u8 px[3][4];
        u64 kept[3], staged[3];
        inline_set(ds, 0, g_life_green);
        CHECK(!inline_draw(channel, px[0], &kept[0], &staged[0]));
        inline_set(ds, 1, NULL); /* a new base vertex in every bank 0 */
        CHECK(!inline_draw(channel, px[1], &kept[1], &staged[1]));
        inline_set(ds, 0, g_life_blue);
        CHECK(!inline_draw(channel, px[2], &kept[2], &staged[2]));
        vk_draw_counts(&after);
        int ok = staged[0] == 1 && kept[1] == 1 && !staged[1] && staged[2] == 1 && life_near(px[0], green) &&
                 life_near(px[1], green) && life_near(px[2], blue) && after.differs == before.differs;
        if (!ok)
            fprintf(stderr,
                    "inline blocks%s: bank 2 kept %llu %llu %llu, staged %llu %llu %llu (expected staged, kept, "
                    "staged); pixels %u,%u,%u %u,%u,%u %u,%u,%u (green, green, blue); %llu differences\n",
                    check ? " with draw_state_check=1" : "", (unsigned long long)kept[0], (unsigned long long)kept[1],
                    (unsigned long long)kept[2], (unsigned long long)staged[0], (unsigned long long)staged[1],
                    (unsigned long long)staged[2], px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2], px[2][0],
                    px[2][1], px[2][2], (unsigned long long)(after.differs - before.differs));
        CHECK(ok);
    }
    g_settings.draw_state_check = saved_check;
    /* the channel's slot 3 back in guest memory and its blocks zero, under generations nothing staged holds */
    ds->bind.cb_inline[4][3] = DS_CB_MEMORY;
    memset(&ds->inl, 0, sizeof ds->inl);
    ds_generations_renew(ds);
    return 0;
}

/* Headers that name no texture, sampled by the fragment program of the texture cases (slot 0, its handle in constant
   buffer 2): an empty one, all eight words nought; one whose word 0 alone is nought, a texture's address, size and
   layout after it; and one of a format the renderer has no Vulkan format for. Word 0's four component sources (bits 19
   to 30) of nought are ZERO on the console (envytools' gm200_texture.xml X_SOURCE to W_SOURCE, g80_defs.xml
   G80_TIC_SOURCE), so the first two read (0, 0, 0, 0) whatever the texture unit fetches. Each draw is drawn, its pixel
   (0, 0, 0, 0) over the red clear, and the log says once for each header its address, its word 0 and the null texture
   read in its place. Then a program of a dynamic handle table (render.c's tex_tables_set) reads a fifth, empty one */
#define EMPTY_INDEXED_PROGRAM 0x11000
static int empty_header_draw(GpuChannel* channel, u32 program, u32 handle, u8 px[4], u64* drawn) {
    u32* r = channel->r3d;
    const u64 target = MEMO_GPU + 0x20000;
    /* constant buffer 2's bytes 0x40 to 0x7f, written by the guest's CPU in a new epoch as LIFE_CPU writes: the TEX's
       handle, its first word, and the table's entries, one each 16 bytes */
    for (u32 k = 0; k < 4; k++) snx_st_le32(GP(MEMO_CPU + (LIFE_POOL + 0x240 + 16 * k - MEMO_GPU)), handle);
    vk_ww_new_epoch();
    const u64 before = g_vk_drawn;
    vk_lock();
    rt_clear(channel, target, MEMO_SIDE, MEMO_SIDE, 0, 0);
    r[0x851] = program;
    r[0x458] = 0 | 1u << 21 | 7u << 27; /* RGBA32F, the whole target's triangle */
    channel->r3d_dirty = ~0u;           /* the registers were set directly */
    mx_draw_state_sync(channel);
    DrawCall call = {.topology = 4, .count = 3, .instances = 1};
    vk_render_draw(&channel->ds, &call, &channel->owner);
    vk_render_end();
    Surf* t = vk_rt_find(target);
    CHECK(t);
    if (t) CHECK(!read_pixel_at(t, 0, 0, px));
    vk_unlock();
    *drawn = g_vk_drawn - before;
    return 0;
}
static int empty_header_cases(GpuChannel* channel) {
    u32* r = channel->r3d;
    u32 headers[3][8] = {{0}};
    life_tic(headers[1], 0);
    headers[1][0] = 0;
    life_tic(headers[2], 0);
    headers[2][0] = 0x7f | 7u << 7 | 7u << 10 | 7u << 13 | 7u << 16 | 2u << 19 | 3u << 22 | 4u << 25 | 5u << 28;
    for (u32 h = 0; h < 3; h++) { /* indices 1 to 3 of the pool, before its samplers at 0x100 */
        u8 bytes[32];
        for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + 4 * i, headers[h][i]);
        CHECK(!gmem_write(LIFE_POOL + 32 * (h + 1), bytes, 32));
    }
    CHECK(!life_headers(0, 0)); /* index 0 a texture, sampler 0 linear */
    r[0x55F] = 3;               /* the pool's highest index */
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffers 2 (the texture's handle) and 3 of the fragment stage */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    r[0x8E1] = (u32)(LIFE_CB >> 32);
    r[0x8E2] = (u32)LIFE_CB;
    r3d_cb_bind(channel, 4, 1 | 3u << 4);
    /* each header twice, the empty one's two draws one after the other */
    static const u32 handles[] = {1, 1, 2, 3, 2, 3};
    enum { DRAWS = sizeof handles / sizeof *handles };
    u8 px[DRAWS][4];
    u64 drawn[DRAWS];
    vk_null_headers_forget();
    FILE* log = tmpfile();
    CHECK(log);
    if (!log) return 1;
    t_plat.log_redirect = log;
    for (u32 i = 0; i < DRAWS; i++) CHECK(!empty_header_draw(channel, LIFE_TEX_PROGRAM, handles[i], px[i], &drawn[i]));
    t_plat.log_redirect = NULL;
    char text[4096] = {0};
    rewind(log);
    text[fread(text, 1, sizeof text - 1, log)] = 0;
    fclose(log);
    static const u8 zero[4] = {0, 0, 0, 0};
    for (u32 i = 0; i < DRAWS; i++) {
        if (drawn[i] != 1 || memcmp(px[i], zero, 4))
            fprintf(stderr,
                    "empty headers: draw %u (handle %u): %llu drawn, pixel %u,%u,%u,%u, expected 1 and 0,0,0,0\n", i,
                    handles[i], (unsigned long long)drawn[i], px[i][0], px[i][1], px[i][2], px[i][3]);
        CHECK(drawn[i] == 1 && !memcmp(px[i], zero, 4));
    }
    for (u32 h = 1; h <= 3; h++) {
        char want[256];
        snprintf(want, sizeof want,
                 "vulkan: texture 0 of stage %d (handle %08x) names %s at %llx (word 0 %08x): it reads the null "
                 "texture, zeros, and the draw goes on\n",
                 ST_FS, h, h == 3 ? "a header of no Vulkan format" : "an empty header",
                 (unsigned long long)(LIFE_POOL + 32 * h), headers[h - 1][0]);
        const char* at = strstr(text, want);
        if (!at || strstr(at + 1, want)) fprintf(stderr, "empty headers: not once in the log: %slog:\n%s", want, text);
        CHECK(at && !strstr(at + 1, want));
    }
    /* word 0 nought is no texture, not a format the runtime lacks */
    CHECK(!strstr(text, "unsupported texture format 0,"));
    /* a dynamic handle table: each fragment's column picks an entry (LDC R4, c[2][x * 16 + 0x40]), every entry handle
       4, an empty header the draws above did not read; TLD.B.LZ fetches texel (0, 0) through it */
    if (g_spirv_translate.texture_arrays) {
        const u64 indexed[] = {0xe000000000070002ull | 0x1cull << 30, /* IPA R2, fragment X */
                               0x5cb0000000070002ull | 3ull << 39 | 2ull << 20 | 2ull << 10 | 2ull << 8, /* F2I R2 */
                               0x3848000000470205ull, /* SHL R5, R2, 4 */
                               0xef94002004070504ull, /* LDC R4, c[2][R5 + 0x40] */
                               0x0100000000071002ull, /* MOV32I R2, 0 */
                               0x0100000000071003ull, /* MOV32I R3, 0 */
                               0xdd380007a0470200ull, /* TLD.B.LZ R0..3, R2, handle R4 */
                               0xe30000000007000full};
        CHECK(mesh_program(MEMO_GPU + EMPTY_INDEXED_PROGRAM, indexed, sizeof indexed / sizeof *indexed, 1));
        maxwell3d_method(channel, 0x369, 1, 1); /* the code cache invalidated */
        CHECK(!gmem_write(LIFE_POOL + 32 * 4, (const u8[32]){0}, 32));
        r[0x55F] = 4;
        u8 pixel[4];
        u64 count = 0;
        log = tmpfile();
        CHECK(log);
        if (!log) return 1;
        t_plat.log_redirect = log;
        for (int k = 0; k < 2; k++) CHECK(!empty_header_draw(channel, EMPTY_INDEXED_PROGRAM, 4, pixel, &count));
        t_plat.log_redirect = NULL;
        memset(text, 0, sizeof text);
        rewind(log);
        text[fread(text, 1, sizeof text - 1, log)] = 0;
        fclose(log);
        char want[256];
        snprintf(
            want, sizeof want,
            "vulkan: an indexed texture of stage %d (bank 2 handle 00000004) names an empty header at %llx (word 0 "
            "00000000): it reads the null texture, zeros, and the draw goes on\n",
            ST_FS, (unsigned long long)(LIFE_POOL + 32 * 4));
        const char* at = strstr(text, want);
        if (count != 1 || memcmp(pixel, zero, 4) || !at || strstr(at + 1, want))
            fprintf(stderr,
                    "empty headers, a handle table: %llu drawn, pixel %u,%u,%u,%u, expected 1 and 0,0,0,0 and "
                    "once in the log: %slog:\n%s",
                    (unsigned long long)count, pixel[0], pixel[1], pixel[2], pixel[3], want, text);
        CHECK(count == 1 && !memcmp(pixel, zero, 4) && at && !strstr(at + 1, want));
    } else
        puts("SKIP empty headers in a handle table: non-uniform sampled image arrays are unavailable");
    /* the first 64 headers are each said once, then one line says the later ones are not */
    vk_null_headers_forget();
    log = tmpfile();
    CHECK(log);
    if (!log) return 1;
    t_plat.log_redirect = log;
    int firsts = 0, again = 0;
    for (u64 k = 0; k < 66; k++) firsts += vk_null_header_first(0x10000 + 32 * k);
    for (u64 k = 0; k < 66; k++) again += vk_null_header_first(0x10000 + 32 * k);
    t_plat.log_redirect = NULL;
    memset(text, 0, sizeof text);
    rewind(log);
    text[fread(text, 1, sizeof text - 1, log)] = 0;
    fclose(log);
    static const char unlisted[] = "vulkan: 64 headers that name no texture were each said once: the ones after them "
                                   "read the null texture too, unlisted\n";
    const char* cap = strstr(text, unlisted);
    if (firsts != 64 || again || !cap || strstr(cap + 1, unlisted))
        fprintf(stderr, "empty headers: %d said of 66, %d said again, log:\n%s", firsts, again, text);
    CHECK(firsts == 64 && !again && cap && !strstr(cap + 1, unlisted));
    vk_null_headers_forget();
    for (u32 k = 0; k < 4; k++) /* handle 0 again, for the cases after */
        snx_st_le32(GP(MEMO_CPU + (LIFE_POOL + 0x240 + 16 * k - MEMO_GPU)), 0);
    vk_ww_new_epoch();
    channel->ds.bind.cb_addr[4][2] = channel->ds.bind.cb_addr[4][3] = 0;
    channel->ds.bind.cb_size[4][2] = channel->ds.bind.cb_size[4][3] = 0;
    r[0x982] = 0;
    r[0x55F] = 0;
    return 0;
}

/* The descriptor heaps' lasting part (descriptor_heaps.c): the descriptors of views and of the buffers that outlive a
   frame are kept from frame to frame, and a destroy forgets them. Each case draws as it is and with
   draw_state_check=1, each draw into a target of its own read at once (an eviction takes the targets too), and the
   pixels must be the same in the two runs and the model's. LAST_EVICT evicts the texture, lets its view be
   destroyed (the frames after it), and makes its texture 4 texels wide (0, 0, 1, 1: red one where the program samples):
   a view made later, often with the destroyed one's handle, must be drawn, not the kept descriptor of the old one
   (2 wide, red 0.75 or none). LAST_REBUFFER drops every buffer of the buffer cache, lets them be destroyed, and has the
   CPU write the constant buffer (blue) */
enum {
    LAST_NONE,
    LAST_FRAMES,   /* VK_FRAMES + 1 presents, each with commands: every slot recycled, its deferred objects destroyed */
    LAST_EVICT,    /* the texture evicted, destroyed, and made 4 texels wide */
    LAST_REBUFFER, /* the buffer cache's buffers dropped and destroyed, and new bytes in the constant buffer */
};
#define LAST_DRAWS 8
typedef struct {
    u64 made, hits, forgotten, differs, checked;
    int reused; /* a view after LAST_EVICT got the handle of one destroyed before it */
} LastCounts;
static int last_frames(void) {
    vk_lock();
    vk_render_end();
    for (u32 k = 0; k <= VK_FRAMES; k++) {
        CHECK(vk_cmd()); /* the slot recycled: the deferred destroys of its last use run */
        vk_frame_next();
    }
    vk_cmd();
    vk_unlock();
    return 0;
}
/* the views the texture at LIFE_POOL + 0x400 has, into views (up to 6), their count */
static u32 last_views(VkImageView* views) {
    u32 n = 0;
    for (Surf* s = vk_surf_first(LIFE_POOL + 0x400); s; s = vk_surf_next(s))
        if (!s->is_rt && s->gva == LIFE_POOL + 0x400)
            for (int i = 0; i < s->nsv && n < 6; i++) views[n++] = s->sv[i].v;
    return n;
}
static int last_texture(int wide) {
    u32 tic[8];
    u8 bytes[32];
    life_tic(tic, 0);
    tic[4] = (wide ? 3u : 1u) | 1u << 23; /* the width less one */
    for (u32 i = 0; i < 8; i++) snx_st_le32(bytes + i * 4, tic[i]);
    CHECK(!gmem_write(LIFE_POOL, bytes, 32));
    u8 texels[16] = {0};
    if (wide) {
        snx_st_le32(texels + 8, 0x3f800000);
        snx_st_le32(texels + 12, 0x3f800000);
    } else
        snx_st_le32(texels + 4, 0x3f800000);
    CHECK(!gmem_write(LIFE_POOL + 0x400, texels, 16));
    return 0;
}
static int last_sequence(GpuChannel* channel, int tex, const u8* events, u32 n, int check, u8 px[LAST_DRAWS][4],
                         LastCounts* counts) {
    u32* r = channel->r3d;
    int saved_check = g_settings.draw_state_check;
    u8 bytes[256] = {0};
    life_floats(bytes, g_life_green, 4);
    CHECK(!gmem_write(LIFE_CB, bytes, 256));
    CHECK(!life_headers(0, 0));
    CHECK(!last_texture(0));
    g_settings.draw_state_check = check;
    vk_lock();
    vk_finish();
    vk_render_draws_forget();
    vk_surf_forget_all(); /* nothing of the run before: the texture is made again from its 2-wide header */
    vk_buf_forget_all();
    vk_unlock();
    CHECK(!last_frames());
    vk_ww_new_epoch();
    VkDescriptorHeapStats before;
    vk_descriptor_heaps_get_stats(&before);
    VkImageView gone[64];
    u32 ngone = 0;
    *counts = (LastCounts){0};
    for (u32 i = 0; i < n; i++) {
        const u64 target = MEMO_GPU + 0x20000 + i * 0x4000;
        switch (events[i]) {
            case LAST_FRAMES: CHECK(!last_frames()); break;
            case LAST_EVICT:
                vk_lock();
                ngone += last_views(gone + ngone);
                vk_surf_forget_all();
                vk_unlock();
                CHECK(!last_frames());
                CHECK(!last_texture(1));
                vk_ww_new_epoch();
                break;
            case LAST_REBUFFER:
                vk_lock();
                vk_buf_forget_all();
                vk_unlock();
                CHECK(!last_frames());
                life_floats(bytes, g_life_blue, 4);
                memcpy(GP(MEMO_CPU + (LIFE_CB - MEMO_GPU)), bytes, 16);
                vk_ww_new_epoch();
                break;
            default: break;
        }
        u64 drawn = g_vk_drawn;
        vk_lock();
        rt_clear(channel, target, MEMO_SIDE, MEMO_SIDE, 0, 0);
        r[0x851] = tex ? LIFE_TEX_PROGRAM : LIFE_CB_PROGRAM;
        r[0x458] = 0 | 1u << 21 | 7u << 27; /* RGBA32F, the whole target's triangle */
        channel->r3d_dirty = ~0u; /* the registers were set directly */
        mx_draw_state_sync(channel);
        DrawCall call = {.topology = 4, .count = 3, .instances = 1};
        vk_render_draw(&channel->ds, &call, &channel->owner);
        vk_render_end();
        Surf* drawn_into = vk_rt_find(target);
        CHECK(drawn_into);
        CHECK(!read_pixel_at(drawn_into, 0, 0, px[i]));
        if (i && events[i] == LAST_EVICT) {
            VkImageView views[6];
            u32 nv = last_views(views);
            for (u32 a = 0; a < nv; a++)
                for (u32 b = 0; b < ngone; b++) counts->reused |= views[a] == gone[b];
        }
        vk_unlock();
        CHECK(g_vk_drawn == drawn + 1);
    }
    VkDescriptorHeapStats after;
    vk_descriptor_heaps_get_stats(&after);
    counts->made = after.lasting_made - before.lasting_made;
    counts->hits = after.lasting_hits - before.lasting_hits;
    counts->forgotten = after.lasting_forgotten - before.lasting_forgotten;
    counts->differs = after.lasting_differs - before.lasting_differs;
    counts->checked = after.lasting_checked - before.lasting_checked;
    g_settings.draw_state_check = saved_check;
    return 0;
}
/* the case's two runs: the same pixels, the model's, no difference found; descriptors kept and found again in a later
   frame, and (forgets) forgotten with their object. forgets 2: only where the bytes are in a buffer that lives past
   the frame (the buffer cache's own, not staging or a stream copy, as some buffer options give): there the pixels
   alone are checked */
static int last_case(GpuChannel* channel, const char* what, int tex, const u8* events, u32 n, int forgets) {
    u8 px[2][LAST_DRAWS][4];
    LastCounts counts[2];
    for (int run = 0; run < 2; run++) CHECK(!last_sequence(channel, tex, events, n, run, px[run], &counts[run]));
    u8 want[4] = {0, 255, 0, 255};
    if (tex) memcpy(want, (const u8[4]){191, 0, 0, 255}, 4); /* 0.75 of the way from 0 to 1 */
    for (u32 i = 0; i < n; i++) {
        if (events[i] == LAST_EVICT) memcpy(want, (const u8[4]){255, 0, 0, 255}, 4); /* texel 2 of 0, 0, 1, 1 */
        if (events[i] == LAST_REBUFFER) memcpy(want, (const u8[4]){0, 0, 255, 255}, 4);
        for (int run = 0; run < 2; run++) {
            int ok = life_near(px[run][i], want) && !memcmp(px[run][i], px[0][i], 4);
            if (!ok)
                fprintf(stderr,
                        "lasting descriptors, %s: draw %u%s: pixel %u,%u,%u,%u, expected %u,%u,%u,%u and the pixel of "
                        "the first run\n",
                        what, i, run ? " with draw_state_check=1" : "", px[run][i][0], px[run][i][1], px[run][i][2],
                        px[run][i][3], want[0], want[1], want[2], want[3]);
            CHECK(ok);
        }
    }
    int kept = forgets != 2 || counts[0].made || counts[1].made;
    int counted = !counts[0].differs && !counts[1].differs && (!kept || counts[1].checked != 0);
    for (int run = 0; run < 2 && kept; run++) counted &= counts[run].made != 0 && counts[run].hits != 0;
    if (forgets && kept)
        for (int run = 0; run < 2; run++) counted &= counts[run].forgotten != 0;
    printf("lasting descriptors, %s: made %llu and %llu, found %llu and %llu, forgotten %llu and %llu, checked %llu, "
           "different %llu and %llu%s\n",
           what, (unsigned long long)counts[0].made, (unsigned long long)counts[1].made,
           (unsigned long long)counts[0].hits, (unsigned long long)counts[1].hits,
           (unsigned long long)counts[0].forgotten, (unsigned long long)counts[1].forgotten,
           (unsigned long long)counts[1].checked, (unsigned long long)counts[0].differs,
           (unsigned long long)counts[1].differs,
           counts[0].reused || counts[1].reused ? "; a new view had a destroyed one's handle"
           : kept                               ? ""
                                                : "; the bytes were in no buffer that lives past the frame");
    CHECK(counted);
    return 0;
}
static int last_cases(GpuChannel* channel) {
    if (!g_vk.has_descriptor_heaps) {
        puts("SKIP lasting descriptors: the device draws without descriptor heaps");
        return 0;
    }
    u32* r = channel->r3d;
    r[0x982] = 2;
    r[0x8E0] = 256; /* constant buffers 2 (the texture's handle) and 3 (the color) of the fragment stage */
    r[0x8E1] = (u32)((LIFE_POOL + 0x200) >> 32);
    r[0x8E2] = (u32)(LIFE_POOL + 0x200);
    r3d_cb_bind(channel, 4, 1 | 2u << 4);
    r[0x8E1] = (u32)(LIFE_CB >> 32);
    r[0x8E2] = (u32)LIFE_CB;
    r3d_cb_bind(channel, 4, 1 | 3u << 4);
    /* a texture's view through frames: made once, found in each later frame */
    const u8 frames[] = {LAST_NONE, LAST_FRAMES, LAST_NONE, LAST_FRAMES, LAST_NONE};
    CHECK(!last_case(channel, "a texture through frames", 1, frames, 5, 0));
    /* the texture evicted, its view destroyed, and a new view (often at the same handle) of a texture loaded again */
    const u8 evict[] = {LAST_NONE, LAST_FRAMES, LAST_NONE, LAST_EVICT, LAST_FRAMES, LAST_NONE, LAST_EVICT, LAST_NONE};
    CHECK(!last_case(channel, "a texture evicted and its view made again", 1, evict, 8, 1));
    /* the constant buffer through frames, the buffer cache's buffers dropped and made again with new bytes */
    const u8 rebuffer[] = {LAST_NONE,     LAST_FRAMES, LAST_NONE,   LAST_FRAMES,
                           LAST_REBUFFER, LAST_FRAMES, LAST_NONE,   LAST_FRAMES};
    CHECK(!last_case(channel, "a buffer of the cache dropped and made again", 0, rebuffer, 8, 2));
    channel->ds.bind.cb_addr[4][2] = channel->ds.bind.cb_addr[4][3] = 0;
    channel->ds.bind.cb_size[4][2] = channel->ds.bind.cb_size[4][3] = 0;
    r[0x982] = 0;
    puts("PASS lasting descriptors: views and buffers kept from frame to frame, an evicted texture's view and the "
         "buffer cache's dropped buffers forgotten, draw_state_check=1 draws the same pixels and finds no "
         "difference");
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
    CHECK(!life_cases(channel));
    CHECK(!compute_pending_cases(channel));
    CHECK(!memo_gens_cases(channel));
    CHECK(!over_target_cases(channel));
    CHECK(!pool_map_cases(channel));
    CHECK(!pool_map_nvn_case(channel));
    CHECK(!texture_write_case(channel));
    CHECK(!writer_cases(channel));
    CHECK(!inline_block_cases(channel));
    CHECK(!empty_header_cases(channel));
    CHECK(!last_cases(channel));
    free(channel);
    puts("PASS draw memo: A, B, A and a state in two vertex formats take the table, a new epoch and new code make the "
         "state again; constant buffers kept past submissions, views, samplers and a stage's last set kept in the "
         "frame, and CPU, 3D engine and storage result writes, header writes and presents between draws drawn as "
         "without them; draw_state_check=1 finds no difference; an inline block kept while another changes, staged "
         "again when it does; a fragment key kept over a copy of a target and made again with the copy; headers that "
         "name no texture read zeros, their draws drawn and each header said once");
    return 0;
}
