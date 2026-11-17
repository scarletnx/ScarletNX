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
    CHECK(!last_cases(channel));
    free(channel);
    puts("PASS draw memo: A, B, A and a state in two vertex formats take the table, a new epoch and new code make the "
         "state again; constant buffers kept past submissions, views, samplers and a stage's last set kept in the "
         "frame, and CPU, 3D engine and storage result writes, header writes and presents between draws drawn as "
         "without them; draw_state_check=1 finds no difference");
    return 0;
}
