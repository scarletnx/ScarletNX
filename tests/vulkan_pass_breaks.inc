/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Copies that fill a buffer a draw reads, and the rendering pass the draws before them are in. A 4x4 target, quads over
   all of it or over its left or right half, their colors a vertex stream:
   - a range that held still for three checks gets a buffer of its own (buffer.c make_own) at the second draw of a
     pass: a copy of the CPU's bytes into a buffer nothing before it reads, so it need not end the pass
   - a range two copy engine results hold, read by the second draw (buffer.c buf_get_owned): written in an earlier
     submission, the copy of them need not end the pass either, whether the test submitted them or the first draw's
     frame check did after a millisecond of recording; written in this one, it has to come after their writes, and
     the pass ends there
   - a write of the guest between two draws over a big buffer of its own, which takes the written pages (patch_own):
     the first draw reads the old bytes and the second the new, so that copy stays between them and the pass ends */
#define PB_GVA   0x4f4000000ull
#define PB_CPU   (HEAP_BASE + 0x5800000)
#define PB_SPAN  0x800000ull /* the checks' 2 MiB, then the benchmark's ranges */
#define PB_VERTS 12u         /* four corners each: the whole target, its left half, its right half */
#define PB_LEN   (PB_VERTS * 16ull)
enum { PB_ALL = 0, PB_LEFT = 4, PB_RIGHT = 8 };
enum { PB_RED, PB_GREEN, PB_YELLOW };

/* vertices first to first + n of the colors at gva, one color */
static int pb_colors(u64 gva, u32 color, u32 first, u32 n) {
    static const float rgba[3][4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {1, 1, 0, 1}};
    u8 bytes[PB_LEN];
    for (u32 v = 0; v < n; v++)
        for (u32 i = 0; i < 4; i++) {
            u32 word;
            memcpy(&word, &rgba[color][i], 4);
            snx_st_le32(bytes + v * 16 + i * 4, word);
        }
    CHECK(!gmem_write(gva + first * 16ull, bytes, n * 16ull));
    return 0;
}
/* the colors stream: len bytes at gva */
static void pb_stream(GpuChannel* ch, u64 gva, u64 len) {
    u32* r = ch->r3d;
    r[0x705] = (u32)(gva >> 32);
    r[0x706] = (u32)gva;
    r[0x7c2] = (u32)((gva + len - 1) >> 32);
    r[0x7c3] = (u32)(gva + len - 1);
    ch->r3d_dirty = ~0u;
    mx_draw_state_sync(ch);
}
static void pb_draw(GpuChannel* ch, u32 where) {
    DrawCall call = {.topology = 7, .first = where, .count = 4, .instances = 1};
    vk_render_draw(&ch->ds, &call, &ch->owner);
}
/* the color of the target's left half (columns 0 and 1) and right half once the work so far is done: r, g, y or b (the
   clear) when all eight pixels are it, ? otherwise. Under vk_lock */
static int pb_halves(u64 target, char half[2]) {
    vk_render_end();
    Surf* s = vk_rt_find(target);
    VkBuffer buffer;
    u8* d = vk_readback_buffer(64, &buffer);
    CHECK(s && d);
    vk_surf_use(vk_cmd(), s, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {4, 4, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), s->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    vk_finish();
    vk_readback_ready();
    for (u32 h = 0; h < 2; h++) {
        char c = 0;
        for (u32 y = 0; y < 4; y++)
            for (u32 x = h * 2; x < h * 2 + 2; x++) {
                const u8* p = d + (y * 4 + x) * 4;
                char k = p[0] == 255 && !p[1] && !p[2]         ? 'r'
                         : !p[0] && p[1] == 255 && !p[2]       ? 'g'
                         : p[0] == 255 && p[1] == 255 && !p[2] ? 'y'
                         : !p[0] && !p[1] && p[2] == 255       ? 'b'
                                                               : '?';
                c = !c || c == k ? k : '?';
            }
        half[h] = c;
        if (c == '?')
            for (u32 y = 0; y < 4; y++)
                fprintf(stderr,
                        "pass breaks: row %u: %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n", y,
                        d[y * 16 + 0], d[y * 16 + 1], d[y * 16 + 2], d[y * 16 + 3], d[y * 16 + 4], d[y * 16 + 5],
                        d[y * 16 + 6], d[y * 16 + 7], d[y * 16 + 8], d[y * 16 + 9], d[y * 16 + 10], d[y * 16 + 11],
                        d[y * 16 + 12], d[y * 16 + 13], d[y * 16 + 14], d[y * 16 + 15]);
    }
    return 0;
}
/* a frame: a new epoch (the guest's submission), the target cleared, red over its left half and the colors at second
   over its right half; the halves' colors, the passes the two draws took, and the submission the first was recorded
   in (first, unless NULL). 0 passes when a submit came between the draws, which ends the pass whatever they do: the
   GPU thread's own after a millisecond of recording (cmd.c frame_check), which a slow driver or a validation layer
   reaches */
static int pb_frame(GpuChannel* ch, u64 target, u64 red, u64 second, u64* begun, u64* first, char half[2]) {
    g_renderer->mem_may_have_changed();
    rt_clear(ch, target, 4, 4, 0, 1);
    vk_lock();
    u64 passes = g_vk_passes, drawn = g_vk_drawn, skipped = g_vk_skipped;
    pb_stream(ch, red, PB_LEN);
    pb_draw(ch, PB_LEFT);
    u64 submit = vk_submit_number();
    if (first) *first = submit;
    pb_stream(ch, second, PB_LEN);
    pb_draw(ch, PB_RIGHT);
    *begun = vk_submit_number() == submit ? g_vk_passes - passes : 0;
    if (g_vk_drawn != drawn + 2)
        fprintf(stderr, "pass breaks: %llu of the frame's 2 draws recorded, %llu skipped\n",
                (unsigned long long)(g_vk_drawn - drawn), (unsigned long long)(g_vk_skipped - skipped));
    CHECK(g_vk_drawn == drawn + 2);
    CHECK(!pb_halves(target, half));
    vk_unlock();
    return 0;
}

/* The green range gets a buffer of its own at its third check, after the red draw began the pass: the copy into it is
   recorded ahead of the pass, and the green draw reads it */
static int pb_own_case(GpuChannel* ch, u64 target, u64 red, u64 green) {
    char half[2];
    u64 begun = 0, bytes0, bytes1;
    u32 own0, own1;
    CHECK(!pb_colors(red, PB_RED, 0, PB_VERTS) && !pb_colors(green, PB_GREEN, 0, PB_VERTS));
    /* the red range and the positions their own buffers first: red over both halves */
    for (u32 f = 0; f < 3; f++) CHECK(!pb_frame(ch, target, red, red, &begun, NULL, half));
    for (u32 f = 0; f < 2; f++) { /* the green range's first two checks: read from the frame's staging */
        CHECK(!pb_frame(ch, target, red, green, &begun, NULL, half));
        if (half[0] != 'r' || half[1] != 'g')
            fprintf(stderr, "own buffer, check %u from staging: left %c, right %c (r, g wanted)\n", f + 1, half[0],
                    half[1]);
        CHECK(half[0] == 'r' && half[1] == 'g');
    }
    vk_lock();
    vk_buf_stats(&own0, &bytes0);
    vk_unlock();
    CHECK(!pb_frame(ch, target, red, green, &begun, NULL, half));
    vk_lock();
    vk_buf_stats(&own1, &bytes1);
    vk_unlock();
    if (half[0] != 'r' || half[1] != 'g' || begun > 1)
        fprintf(stderr, "own buffer mid-pass: left %c, right %c (r, g wanted), %llu passes (1 wanted)\n", half[0],
                half[1], (unsigned long long)begun);
    CHECK(half[0] == 'r' && half[1] == 'g' && begun <= 1);
    printf("pass breaks: a range %s at the second draw of a pass, %s\n",
           own1 > own0 ? "took a buffer of its own" : "read in place",
           begun ? "which kept the pass" : "a submit between the draws (their passes not counted)");
    return 0;
}

/* how pb_owners_case's copies reach the GPU: with the draws' submission, submitted before the draws, or sent by the
   first draw's frame check after a wait of over a millisecond of recording (cmd.c frame_check) */
enum { PB_WITH_DRAWS, PB_SUBMITTED, PB_FRAME_CHECK };
#define PB_WAIT_NS 1500000 /* over the millisecond of recording a submission holds at most (cmd.c SUBMIT_EVERY_NS) */
/* Green colors a copy engine wrote in two halves, two owners the GPU holds, drawn over the right half after the red
   draw began the pass. Copies submitted before the draws were recorded, whatever submitted them, let the copy of their
   bytes for the draw run before the pass; otherwise it follows them, and the pass ends before the green draw */
static int pb_owners_case(GpuChannel* ch, u64 target, u64 red, u64 staged, u64 copy, u64 copy_cpu, int how) {
    GpuDmaOp o;
    char half[2];
    u64 begun = 0, copied, first;
    CHECK(!pb_colors(staged, PB_GREEN, 0, PB_VERTS));
    vk_lock();
    int inplace = vk_uma_held(copy_cpu, PB_LEN), owned = own_traps() && !inplace;
    vk_unlock();
    for (u32 k = 0; k < 2; k++) {
        memset(&o, 0, sizeof o);
        o.width = o.line_in = o.line_out = (u32)(PB_LEN / 2);
        o.lines = 1;
        o.src = pitch_side(staged + k * (PB_LEN / 2), 0);
        o.dst = pitch_side(copy + k * (PB_LEN / 2), 0);
        CHECK(g_renderer->dma_copy(&o));
    }
    if (owned) CHECK(own_lookup(copy_cpu, PB_LEN, NULL, 0) == 2);
    vk_lock();
    copied = vk_submit_number(); /* the submission the copies were recorded in */
    if (how == PB_SUBMITTED) vk_submit_recorded();
    vk_unlock();
    if (how == PB_FRAME_CHECK) plat_sleep_ns(PB_WAIT_NS);
    u64 landings = own_landings();
    CHECK(!pb_frame(ch, target, red, copy, &begun, &first, half));
    /* the first draw's frame check sends the copies without the wait too, where a slow driver or a validation layer
       takes a millisecond from the last submit to it */
    int before = first != copied;
    if (owned && how == PB_SUBMITTED && !before)
        fprintf(stderr,
                "two owners: the copies' submission %llu was still recording at the first draw, after "
                "vk_submit_recorded\n",
                (unsigned long long)copied);
    if (owned && how == PB_FRAME_CHECK && !before)
        fprintf(stderr,
                "two owners: the copies' submission %llu was still recording at the first draw, after a wait "
                "of %u us (cmd.c frame_check submits after 1000)\n",
                (unsigned long long)copied, PB_WAIT_NS / 1000);
    CHECK(!owned || how == PB_WITH_DRAWS || before);
    u64 want = owned && !before ? 2 : 1;
    if (half[0] != 'r' || half[1] != 'g' || (owned && begun && begun != want))
        fprintf(stderr, "two owners%s: left %c, right %c (r, g wanted), %llu passes (%llu wanted)\n",
                before ? " submitted before the draws" : "", half[0], half[1], (unsigned long long)begun,
                (unsigned long long)want);
    CHECK(half[0] == 'r' && half[1] == 'g');
    if (owned) CHECK((!begun || begun == want) && own_landings() == landings);
    printf("pass breaks: a range two copy engine results hold, %s, read %s%s\n",
           how == PB_SUBMITTED ? "written in an earlier submission"
           : before            ? "written in a submission the first draw's frame check sent"
                               : "written in this one",
           !owned   ? (inplace ? "in guest memory" : "landed (no traps here)")
           : before ? "inside the pass"
                    : "after their writes, in a pass of its own",
           begun ? "" : " (a submit between the draws: their passes not counted)");
    return 0;
}

/* A big buffer of its own, read by two indexed draws: the left half drawn red from it, then the guest writes the
   colors of both halves green and submits again, and the right half drawn. Where writes are tracked the buffer takes
   the written pages, a copy over bytes the first draw reads: it stays after that draw, and the pass ends between them.
   Both draws reach the last vertex (a triangle of it alone draws nothing), so they read one range of PATCH_MIN */
static int pb_patch_case(GpuChannel* ch, u64 target) {
    const u32 count = 0x4000, last = count - 1; /* 16 bytes a vertex: buffer.c PATCH_MIN */
    const u64 positions = PB_GVA + 0x80000, colors = PB_GVA + 0xC0000, indices = PB_GVA + 0x19000;
    const u32 left[9] = {0, 1, 2, 0, 2, 3, last, last, last}, right[9] = {4, 5, 6, 4, 6, 7, last, last, last};
    u8 bytes[16 * 8];
    char half[2];
    u32* r = ch->r3d;
    if (vk_uma()) { /* read in place: the GPU reads what the guest wrote by the time it draws */
        puts("pass breaks: a guest write between two draws needs buffers of their own (here ranges are read in place)");
        return 0;
    }
    vk_lock();
    int tracked = vk_written_since(GP(PB_CPU + (colors - PB_GVA)), count * 16ull, 0) >= 0;
    vk_unlock();
    CHECK(!gmem_read(PB_GVA + 0x12000 + 4 * 16, bytes, sizeof bytes)); /* the harness's two halves, vertices 0 to 7 */
    CHECK(!gmem_write(positions, bytes, sizeof bytes) && !pb_colors(colors, PB_RED, 0, 8));
    r[0x701] = (u32)(positions >> 32);
    r[0x702] = (u32)positions;
    r[0x7c0] = (u32)((positions + count * 16ull - 1) >> 32);
    r[0x7c1] = (u32)(positions + count * 16ull - 1);
    pb_stream(ch, colors, count * 16ull);
    DrawCall call = {.topology = 4, .indexed = 1, .count = 9, .instances = 1};
    for (u32 f = 0; f < 2; f++) { /* the first frame makes the buffer, the second reads and patches it */
        g_renderer->mem_may_have_changed();
        rt_clear(ch, target, 4, 4, 0, 1);
        CHECK(!index_buffer(ch, indices, left, 9, 4, ~0u));
        vk_lock();
        ch->r3d_dirty = ~0u;
        mx_draw_state_sync(ch);
        u64 passes = g_vk_passes;
        vk_render_draw(&ch->ds, &call, &ch->owner);
        vk_unlock();
        if (!f) {
            vk_lock();
            vk_finish();
            vk_unlock();
            continue;
        }
        CHECK(!pb_colors(colors, PB_GREEN, 0, 8));
        CHECK(!index_buffer(ch, indices + 0x100, right, 9, 4, ~0u));
        g_renderer->mem_may_have_changed();
        vk_lock();
        u64 submit = vk_submit_number();
        ch->r3d_dirty = ~0u;
        mx_draw_state_sync(ch);
        vk_render_draw(&ch->ds, &call, &ch->owner);
        u64 begun = vk_submit_number() == submit ? g_vk_passes - passes : 0; /* 0: a submit came between (pb_frame) */
        CHECK(!pb_halves(target, half));
        vk_unlock();
        if (half[0] != 'r' || half[1] != 'g' || (tracked && begun && begun != 2))
            fprintf(stderr, "guest write between draws: left %c, right %c (r, g wanted), %llu passes (2 wanted)\n",
                    half[0], half[1], (unsigned long long)begun);
        CHECK(half[0] == 'r' && half[1] == 'g');
        if (tracked) CHECK(!begun || begun == 2);
        printf("pass breaks: a guest write between two draws is read by the second and not the first (%s%s)\n",
               tracked ? "its pages copied over the buffer between them" : "no write tracking: hashed again",
               begun ? "" : "; a submit between the draws, their passes not counted");
    }
    r[0x701] = (u32)((PB_GVA + 0x12000) >> 32); /* the harness's streams again */
    r[0x702] = (u32)(PB_GVA + 0x12000);
    r[0x7c0] = (u32)((PB_GVA + 0x12000 + PB_LEN - 1) >> 32);
    r[0x7c1] = (u32)(PB_GVA + 0x12000 + PB_LEN - 1);
    r[0x5F6] = 0;
    return 0;
}

/* the harness: vertex positions (stream 0), a colors stream (stream 1), and the programs that pass the color on */
static GpuChannel* pb_channel(void) {
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    const float corner[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
    const float x0[3] = {-1, -1, 0}, x1[3] = {1, 0, 1}; /* the whole target, its left half, its right half */
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    const u64 positions = PB_GVA + 0x12000;
    u64 fragment[5];
    u8 bytes[PB_LEN];
    GpuChannel* ch = calloc(1, sizeof *ch);
    if (!ch) return NULL;
    ch->id = 128;
    for (u32 v = 0; v < PB_VERTS; v++) {
        const float p[4] = {corner[(v & 3) * 2] < 0 ? x0[v / 4] : x1[v / 4], corner[(v & 3) * 2 + 1], 0, 1};
        for (u32 i = 0; i < 4; i++) {
            u32 word;
            memcpy(&word, p + i, 4);
            snx_st_le32(bytes + v * 16 + i * 4, word);
        }
    }
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    if (gmem_write(positions, bytes, PB_LEN) || !index_program(PB_GVA, vertex, 5, 0) ||
        !index_program(PB_GVA + 0x10000, fragment, 5, 1)) {
        free(ch);
        return NULL;
    }
    u32* r = ch->r3d;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1; /* as reset_3d starts a channel */
    r[0x582] = (u32)(PB_GVA >> 32);
    r[0x583] = (u32)PB_GVA;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    memcpy(r + 0x280, transform, sizeof transform);
    for (u32 stream = 0; stream < 2; stream++) {
        r[0x458 + stream] = stream | 1u << 21 | 7u << 27; /* RGBA32F */
        r[0x700 + stream * 4] = 0x1000 | 16;
    }
    r[0x701] = (u32)(positions >> 32);
    r[0x702] = (u32)positions;
    r[0x7c0] = (u32)((positions + PB_LEN - 1) >> 32);
    r[0x7c1] = (u32)(positions + PB_LEN - 1);
    return ch;
}

static int check_pass_breaks(void) {
    const u64 target = PB_GVA + 0x18000;
    CHECK(mem_map(PB_CPU, PB_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(PB_GVA, PB_CPU, PB_SPAN);
    GpuChannel* ch = pb_channel();
    CHECK(ch);
    CHECK(!pb_own_case(ch, target, PB_GVA + 0x13000, PB_GVA + 0x14000));
    CHECK(!pb_owners_case(ch, target, PB_GVA + 0x13000, PB_GVA + 0x17000, PB_GVA + 0x15000, PB_CPU + 0x15000,
                          PB_SUBMITTED));
    CHECK(!pb_owners_case(ch, target, PB_GVA + 0x13000, PB_GVA + 0x17000, PB_GVA + 0x16000, PB_CPU + 0x16000,
                          PB_WITH_DRAWS));
    CHECK(!pb_owners_case(ch, target, PB_GVA + 0x13000, PB_GVA + 0x17000, PB_GVA + 0x1A000, PB_CPU + 0x1A000,
                          PB_FRAME_CHECK));
    CHECK(!pb_patch_case(ch, target));
    vk_lock();
    vk_finish();
    vk_unlock();
    free(ch);
    return 0;
}

/* Frames of a scene where ranges keep taking buffers of their own between draws (n a frame, each drawn in three frames
   in a row, own at the third), and n / 4 draws read ranges two copy engine results of the frame's earlier submission
   hold. Prints the passes, where they ended, the barriers recorded, and each frame's time split into its recording and
   the wait for the GPU to finish it: the numbers a change to the pass ends is judged by */
static int pb_ns_cmp(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return x < y ? -1 : x > y;
}
static int bench_pass_breaks(u32 frames, u32 n) {
    const u64 target = PB_GVA + 0x18000, ranges = PB_GVA + 0x200000, owners = PB_GVA + 0x600000,
              staged = PB_GVA + 0x17000;
    const u32 dmas = n / 4, warm = 3; /* the first frames make no buffers of their own yet */
    CHECK(frames > warm && frames <= 1000 && n && (u64)frames * n * 256 <= 0x400000 &&
          (u64)frames * dmas * 256 <= 0x200000);
    CHECK(mem_map(PB_CPU, PB_SPAN, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(PB_GVA, PB_CPU, PB_SPAN);
    GpuChannel* ch = pb_channel();
    u64* ns = calloc(frames * 3ull, sizeof *ns); /* each frame's time, recording and wait */
    CHECK(ch && ns);
    for (u32 k = 0; k < frames * n; k++)
        CHECK(!pb_colors(ranges + k * 256ull, k & 1 ? PB_GREEN : PB_YELLOW, 0, PB_VERTS));
    CHECK(!pb_colors(staged, PB_GREEN, 0, PB_VERTS));
    vk_lock();
    vk_pass_ends_log(); /* the sites counted from here */
    vk_unlock();
    u32 own0, own1;
    u64 bytes0, bytes1, passes = 0, barriers = 0, landings0 = own_landings();
    vk_lock();
    vk_buf_stats(&own0, &bytes0);
    vk_unlock();
    for (u32 f = 0; f < frames; f++) {
        u64 t0 = plat_ns(), p0 = g_vk_passes, b0 = g_rec_barriers;
        for (u32 d = 0; d < dmas; d++) /* each range in two halves, two owners */
            for (u32 k = 0; k < 2; k++) {
                GpuDmaOp o;
                memset(&o, 0, sizeof o);
                o.width = o.line_in = o.line_out = (u32)(PB_LEN / 2);
                o.lines = 1;
                o.src = pitch_side(staged + k * (PB_LEN / 2), 0);
                o.dst = pitch_side(owners + (f * dmas + d) * 256ull + k * (PB_LEN / 2), 0);
                CHECK(g_renderer->dma_copy(&o));
            }
        g_renderer->mem_may_have_changed();
        rt_clear(ch, target, 4, 4, 0, 1);
        vk_lock();
        vk_submit_recorded(); /* the copies' writes in an earlier submission than the draws */
        for (u32 g = 0; g < 3 && g <= f; g++)
            for (u32 k = 0; k < n; k++) {
                pb_stream(ch, ranges + ((f - g) * n + k) * 256ull, PB_LEN);
                pb_draw(ch, PB_ALL);
            }
        for (u32 d = 0; d < dmas; d++) {
            pb_stream(ch, owners + (f * dmas + d) * 256ull, PB_LEN);
            pb_draw(ch, PB_ALL);
        }
        u64 t1 = plat_ns();
        vk_finish();
        vk_frame_next(); /* a present's end of the frame: the next one records in the other slot */
        vk_unlock();
        u64 t2 = plat_ns();
        if (f >= warm) {
            ns[f] = t2 - t0;
            ns[frames + f] = t1 - t0;
            ns[2 * frames + f] = t2 - t1;
            passes += g_vk_passes - p0;
            barriers += g_rec_barriers - b0;
        }
    }
    vk_lock();
    vk_buf_stats(&own1, &bytes1);
    vk_unlock();
    u32 steady = frames - warm;
    double mean[3] = {0, 0, 0};
    for (u32 k = 0; k < 3; k++) {
        for (u32 f = warm; f < frames; f++) mean[k] += (double)ns[k * frames + f];
        mean[k] /= steady * 1e6;
    }
    qsort(ns + warm, steady, sizeof *ns, pb_ns_cmp);
    u64 p99 = ns[warm + (steady * 99) / 100 < frames ? warm + (steady * 99) / 100 : frames - 1];
    printf("pass breaks bench: %u frames, %u draws taking buffers of their own and %u over two owners' bytes each; "
           "from frame %u on: %.2f passes and %.2f barriers a frame, %.2f own buffers made a frame, %llu landings; "
           "frame %.3f ms mean (recording %.3f, waiting for the GPU %.3f), %.3f ms at the 99th percentile\n",
           frames, n, dmas, warm + 1, (double)passes / steady, (double)barriers / steady,
           (double)(own1 - own0) / frames, (unsigned long long)(own_landings() - landings0), mean[0], mean[1], mean[2],
           (double)p99 / 1e6);
    vk_lock();
    vk_pass_ends_log();
    vk_unlock();
    free(ns);
    free(ch);
    return 0;
}
