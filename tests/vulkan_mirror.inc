/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU mirror of guest memory (mirror.c): a range bound into it holds guest memory's bytes; a byte the CPU changes
   since goes in when the range is bound again, and a byte the GPU wrote there and the CPU didn't stays */

/* len bytes of src at from, read back once the work recorded so far is done */
static int buffer_read(VkBuffer src, u64 from, u32 len, u8* out) {
    VkBuffer back;
    u64 at;
    DevMem mem;
    u8* p = vk_stage_back(len, &back, &at, &mem);
    CHECK(p);
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    vk_render_end();
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    VkBufferCopy copy = {from, at, len};
    rec_CmdCopyBuffer(vk_cmd(), src, back, 1, &copy);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    b.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    vk_finish();
    CHECK(VK_OK(vk_devmem_invalidate(mem, 0, VK_WHOLE_SIZE)));
    memcpy(out, p, len);
    return 0;
}
/* len bytes of the mirror at cpu */
static int mirror_read(u64 cpu, u32 len, u8* out) { return buffer_read(vk_mirror_buffer(), cpu, len, out); }
/* the mirror's bytes against guest memory's, but [from, from + n), which hold value */
static int mirror_expect(const u8* got, const u8* guest, u32 len, u32 from, u32 n, u8 value) {
    for (u32 i = 0; i < len; i++) {
        u8 want = i >= from && i < from + n ? value : guest[i];
        if (got[i] != want) {
            fprintf(stderr, "vulkan_headless: mirror: byte %#x is %#x, not %#x\n", i, got[i], want);
            return 1;
        }
    }
    return 0;
}

/* a program's write of [cpu, cpu + n) of the mirror, value in its bytes and their bits in the mask, as its stores
   would leave them (logged 0: the bytes alone, which no store of a logging program leaves) */
static int mirror_write(u64 cpu, u32 n, u8 value, int logged) {
    CHECK(!owned_test_fill(vk_mirror_buffer(), cpu, n, value));
    if (!logged) return 0;
    u64 w0 = cpu / 32, w1 = (cpu + n + 31) / 32;
    VkBuffer src;
    u64 at;
    u32* m = (u32*)(void*)vk_stage_upload((w1 - w0) * 4, 16, &src, &at);
    CHECK(m);
    memset(m, 0, (w1 - w0) * 4);
    for (u64 b = cpu; b < cpu + n; b++) m[b / 32 - w0] |= 1u << (b % 32);
    VkBufferCopy copy = {at, w0 * 4, (w1 - w0) * 4};
    rec_CmdCopyBuffer(vk_cmd(), src, vk_mirror_mask(), 1, &copy);
    return 0;
}

/* a log's words from from on that aren't zero: the first's index, or -1 with none */
static i64 log_dirty(const u32* w, u64 from, u64 n) {
    for (u64 i = from; i < from + n; i++)
        if (w[i]) return (i64)i;
    return -1;
}
/* check_mirror_fold's unflagged run, b's bytes 0x1100 to 0x1104 in part 1: still in the log after the folds, then
   taken out of it, so the rest can be checked clear. 0, or 1 where a bit of it went */
static int unflagged_kept(u32* w) {
    for (u32 x = 0x1100; x < 0x1104; x++) {
        u32* word = &w[SH_WLOG_PART + SH_WPAGE_SLOT + x / 32];
        if (!(*word >> (x % 32) & 1)) {
            fprintf(stderr, "vulkan_headless: mirror fold: the unflagged bit of b's byte %#x went from the log\n", x);
            return 1;
        }
        *word &= ~(1u << (x % 32));
    }
    return 0;
}
/* A dispatch's write log into the mirror's mask (vk_mirror_fold): two buffers of one dispatch, the first at a byte
   that is not the first of a mask word, the second over it. Only the windows flagged are taken, the bits past a
   buffer's length go, and the bits of the two buffers meet in the mask. The folds leave the parts they read clear,
   the windows past a buffer's length too (a store past the windows is logged in the last one, shader/write_log.c),
   and vk_wlog_clear fills the part no fold read (a slot not bound) alone: a part the folds left clear keeps what is
   there. With gpu, the log in the GPU's memory, as a dispatch in the mirror takes it (vk_wlog_get_gpu) */
static int check_mirror_fold(u64 cpu, u64 gva, int gpu) {
    const u64 a = cpu + 0x1005, b = a + 8; /* a: bit 5 of its mask word */
    const u32 alen = 0x1800, blen = 0x1ff0; /* b inside the 3 pages mapped */
    static u8 want[0x2100]; /* a byte for each guest byte from cpu + 0x1000 */
    static u32 words[3 * SH_WLOG_PART];
    memset(want, 0, sizeof want);
    memset(words, 0, sizeof words);
    /* the bits of part p for its buffer's bytes [from, to), the window flagged or not; part 2 is no fold's */
    u32* part[3] = {words, words + SH_WLOG_PART, words + 2 * SH_WLOG_PART};
    const struct {
        u32 p, from, to, flag;
    } runs[] = {
        {0, 0x10, 0x30, 1},         /* across a mask word of the log and two of the mirror's */
        {0, 0x17f0, 0x1810, 1},     /* past a's length from 0x1800: those go */
        {0, 0xfff000, 0xfff008, 1}, /* the last window, far past a's length: none taken, cleared all the same */
        {1, 0x0, 0x4, 1},           /* b's first bytes, a's 8 to 12 */
        {1, 0x1100, 0x1104, 0},     /* window 1 of b not flagged: not taken */
        {2, 0x7000, 0x7010, 1},     /* the part no fold reads */
    };
    for (u32 r = 0; r < sizeof runs / sizeof *runs; r++) {
        u32* flags = part[runs[r].p];
        for (u32 x = runs[r].from; x < runs[r].to; x++) {
            flags[SH_WPAGE_SLOT + x / 32] |= 1u << (x % 32);
            if (runs[r].flag) flags[x / 4096] = 1;
            if (runs[r].p == 2) continue;
            u64 at = (runs[r].p ? b : a) + x;
            u32 len = runs[r].p ? blen : alen;
            if (runs[r].flag && x < len) want[at - (cpu + 0x1000)] = 1;
        }
    }
    VkWriteLog* log = gpu ? vk_wlog_get_gpu(3) : vk_wlog_get(3);
    CHECK(log && vk_wlog_size(log) == sizeof words && vk_wlog_on_gpu(log) == gpu);
    VkBuffer src;
    u64 at;
    u8* p = vk_stage_upload(sizeof words, 16, &src, &at);
    CHECK(p);
    memcpy(p, words, sizeof words);
    vk_finish(); /* the mask clear of what came before, and a new log's */
    vk_render_end();
    vk_all_barrier(vk_cmd());
    VkBufferCopy copy = {at, 0, sizeof words};
    rec_CmdCopyBuffer(vk_cmd(), src, vk_wlog_buffer(log), 1, &copy);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                           .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                           .dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    VkMirFold f[2] = {{a, alen, 0}, {b, blen, 1}};
    const u32 cleared = vk_mirror_fold(vk_cmd(), vk_wlog_buffer(log), vk_wlog_size(log), f, 2);
    CHECK(cleared == 3);
    CHECK(vk_mirror_pending(gva + 0x1005, alen));
    /* the mask, read before the submit's harvest clears it */
    static u32 mask[0x2100 / 32];
    CHECK(!buffer_read(vk_mirror_mask(), (cpu + 0x1000) / 8, sizeof mask, (u8*)mask));
    for (u32 i = 0; i < sizeof want; i++)
        if ((mask[i / 32] >> (i % 32) & 1) != want[i]) {
            fprintf(stderr, "vulkan_headless: mirror fold: the bit of byte %#x is %u, not %u\n", i,
                    mask[i / 32] >> (i % 32) & 1, want[i]);
            return 1;
        }
    /* the folds' parts clear but for the bits of b's window left unflagged, which no fold reads (the log never holds
       a window's bits without its flag: shader/write_log.c); the other part as it was */
    static u32 after[3 * SH_WLOG_PART];
    CHECK(!buffer_read(vk_wlog_buffer(log), 0, sizeof after, (u8*)after));
    CHECK(!unflagged_kept(after));
    i64 dirty = log_dirty(after, 0, 2 * SH_WLOG_PART);
    if (dirty >= 0) {
        fprintf(stderr, "vulkan_headless: mirror fold: word %#llx of the log is %#x after the folds\n",
                (unsigned long long)dirty, after[dirty]);
        return 1;
    }
    CHECK(!memcmp(after + 2 * SH_WLOG_PART, part[2], SH_WLOG_PART * 4));
    if (gpu) { /* every word of the log zero again, for the next dispatch that takes it: part 2 filled, the folds'
                  parts not written (a word put in part 1 stays) */
        const u32 mark = 0x5eed;
        VkBuffer ms;
        u64 mat;
        u8* mp = vk_stage_upload(4, 16, &ms, &mat);
        CHECK(mp);
        memcpy(mp, &mark, 4);
        VkBufferCopy mc = {mat, (u64)SH_WLOG_PART * 4, 4};
        rec_CmdCopyBuffer(vk_cmd(), ms, vk_wlog_buffer(log), 1, &mc);
        vk_all_barrier(vk_cmd());
        vk_wlog_clear(vk_cmd(), log, cleared);
        CHECK(!buffer_read(vk_wlog_buffer(log), 0, sizeof after, (u8*)after));
        CHECK(after[SH_WLOG_PART] == mark);
        after[SH_WLOG_PART] = 0;
        CHECK(!unflagged_kept(after));
        dirty = log_dirty(after, 0, 3 * SH_WLOG_PART);
        if (dirty >= 0) {
            fprintf(stderr, "vulkan_headless: mirror fold: word %#llx of the GPU's log is %#x after its clear\n",
                    (unsigned long long)dirty, after[dirty]);
            return 1;
        }
        vk_wlog_clear(vk_cmd(), log, 0); /* the mark too: the log goes back to the pool clear */
    }
    vk_wlog_unref(log);
    /* the mirror's bytes there are guest memory's: landing the bits changes nothing */
    vk_mirror_publish();
    vk_mirror_mem_need(gva + 0x1000, 0x2000);
    own_acquire(cpu + 0x1000, 0x2000);
    CHECK(!vk_mirror_pending(gva + 0x1000, 0x2000));
    return 0;
}

/* Frames one after another, as a game's. In each a dispatch in the mirror writes its buffers and its log, taking the
   log the frame before cleared (vk_wlog_get_gpu gives it again), and its folds and the log's clear are recorded as
   compute.c records them; the CPU writes every byte of the buffers' guest memory while the frame's work is in flight;
   then the frame's harvest lands. Each frame lands the bytes its own log gives and no others. A bit an earlier frame
   left in the log would land the mirror's older byte over the CPU's newer one, a window written and not folded would
   lose the frame's bytes, and a bit past a buffer's length would change a byte no program wrote: the mask each frame's
   folds leave, the log after its clear and guest memory after the landing are checked. The stores are the words
   shader/write_log.c's leave, ORed into what the log holds: a flag for each window written, the bytes' bits in the
   words there, and a store past the windows the last window's flag and no bit */
#define MF_CPU    (HEAP_BASE + 0x24000000) /* the windows of a whole part, past every other check's memory */
#define MF_GVA    0xbc0000000ull
#define MF_LEN    0x1000000u
#define MF_B      0x3004u /* B in A, at a byte that is not a mask word's first */
#define MF_D      0x9000u
#define MF_DLEN   0x8000u
#define MF_FRAMES 4u /* of each check */
typedef struct {
    u32 part;     /* the log's part: 0 A, all 16 MiB; 1 B, inside it; 2 D, a buffer in some frames and in others a slot
                     with none, which no fold reads */
    u32 from, to; /* the bytes its stores wrote, from the buffer's first */
} MfRun;
typedef struct {
    u32 blen;   /* B's length */
    int dbound; /* D is a buffer: its part folded */
    MfRun run[8];
} MfFrame;
static const MfFrame mf_frames[MF_FRAMES] = {
    {0x1800,
     0,
     {{0, 0x10, 0x30},           /* window 0 */
      {0, 0x1000, 0x1010},       /* window 1, written in this frame alone */
      {0, 0x2080, 0x2090},       /* window 2: three windows of one workgroup's 128 */
      {0, 0x801020, 0x801040},   /* window 2049, past the first half */
      {0, 0x1000000, 0x1000004}, /* past the windows */
      {1, 0x17f0, 0x1810},       /* across B's length: from 0x1800 on, no byte of B */
      {2, 0x7000, 0x7010}}},
    {0x3000,
     0,
     {{0, 0x40, 0x48}, /* window 0 again, other bytes of it */
      {0, 0x801100, 0x801108},
      {1, 0x1820, 0x1828}, /* window 1 again with B longer: the bytes past its length before are B's now */
      {1, 0x2000, 0x2008},
      {2, 0x7100, 0x7104}}},
    {0x1800,
     1,
     {{0, 0x20, 0x22},
      {0, 0xfa0000, 0xfa0010}, /* window 4000 */
      {1, 0x17f8, 0x1808},
      {2, 0x100, 0x104}}}, /* D a buffer, after two frames whose stores to its part only the fills cleared */
    {0x3000, 0, {{0, 0x50, 0x52}, {1, 0x0, 0x2}}},
};
static u64 mf_base(u32 part) { return MF_CPU + (part == 1 ? MF_B : part == 2 ? MF_D : 0); }
static u32 mf_len(const MfFrame* fr, u32 part) { return part == 0 ? MF_LEN : part == 1 ? fr->blen : MF_DLEN; }
static int mf_folded(const MfFrame* fr, u32 part) { return part < 2 || fr->dbound; }
/* the 16 MiB both checks below work in, mapped by whichever runs first */
static int mf_map(void) {
    static int mapped;
    if (!mapped) {
        CHECK(mem_map(MF_CPU, MF_LEN, MS_NORMAL, PERM_RW) == 0);
        gmmu_map(MF_GVA, MF_CPU, MF_LEN);
        mapped = 1;
    }
    return 0;
}
/* a store's bytes [from, to) of part p ORed into the log's words, as shader/write_log.c's stores leave them */
static void mf_log(u32* log, u32 p, u32 from, u32 to) {
    u32* part = log + (size_t)p * SH_WLOG_PART;
    for (u32 x = from; x < to; x++) {
        u32 window = x >> 12;
        int past = window > SH_WPAGE_SLOT - 1;
        if (past) window = SH_WPAGE_SLOT - 1;
        part[window] = 1;
        if (!past) part[SH_WPAGE_SLOT + window * SH_WLOG_MASK + (x & 4095) / 32] |= 1u << (x % 32);
    }
}
/* what differed, by check (0 these frames, 1 check_mirror_dispatches'), frame and kind (0 a mask bit, 1 a log word,
   2 a guest byte): the first three of each kind are said, then each frame's counts, so that a bit left in the log is
   seen in the frames after it too */
static u32 mf_diff[2][MF_FRAMES][3];
static void mf_fail(u32 check, u32 frame, u32 kind, u64 at, u32 got, u32 want) {
    static const char* const what[3] = {"the mask's bit of byte", "the log's word", "guest byte"};
    u32 seen = 0;
    for (u32 f = 0; f < MF_FRAMES; f++) seen += mf_diff[check][f][kind];
    mf_diff[check][frame][kind]++;
    if (seen < 3)
        fprintf(stderr, "vulkan_headless: mirror %s: frame %u: %s %#llx is %#x, not %#x\n",
                check ? "dispatches" : "frames", frame, what[kind], (unsigned long long)at, got, want);
}
/* 1 where anything differed, each such frame's counts said */
static int mf_report(u32 check) {
    int any = 0;
    for (u32 f = 0; f < MF_FRAMES; f++) {
        const u32* d = mf_diff[check][f];
        if (!(d[0] | d[1] | d[2])) continue;
        any = 1;
        fprintf(stderr,
                "vulkan_headless: mirror %s: frame %u: %u mask bits, %u log words and %u guest bytes differed\n",
                check ? "dispatches" : "frames", f, d[0], d[1], d[2]);
    }
    return any;
}
static int check_mirror_frames(void) {
    static u8 want[MF_LEN];
    static u32 log[3 * SH_WLOG_PART], mask[MF_LEN / 32], mwant[MF_LEN / 32];
    const u32 present = g_vk_present;
    VkWriteLog* kept = NULL;
    VkBuffer buf;
    u64 off;
    memset(mf_diff[0], 0, sizeof mf_diff[0]);
    CHECK(!mf_map());
    /* the CPU's bytes under 0x80 and the programs' over it, and each frame's CPU bytes other than the frame before's:
       a byte landed where it shouldn't be differs from what is wanted there */
    for (u32 x = 0; x < MF_LEN; x++) GP(MF_CPU)[x] = (u8)((x * 7 + 1) & 0x7f);
    for (u32 f = 0; f < MF_FRAMES; f++) {
        const MfFrame* fr = &mf_frames[f];
        /* where the host traps guest memory, the even frames' pages were last touched by the CPU past
           vk_own_cpu_read's 1200 presents: they stay the mirror's once done and land whole at its touch. The odd
           frames' land with their submit, touched by the landing before */
        if (!(f & 1)) g_vk_present += 1300;
        vk_ww_new_epoch();
        for (u32 p = 0; p < 3; p++)
            if (mf_folded(fr, p))
                CHECK(vk_mirror_slice(MF_GVA + (mf_base(p) - MF_CPU), mf_len(fr, p), &buf, &off) &&
                      buf == vk_mirror_buffer() && off == mf_base(p));
        const u64 again = vk_wlog_again();
        VkWriteLog* l = vk_wlog_get_gpu(3);
        CHECK(l && vk_wlog_on_gpu(l) && vk_wlog_size(l) == sizeof log);
        if (f) CHECK(l == kept && vk_wlog_again() == again + 1); /* the one the frame before cleared */
        if (!f) { /* clear as the pool gives it; later frames read it after each clear */
            CHECK(!buffer_read(vk_wlog_buffer(l), 0, sizeof log, (u8*)log));
            for (u32 i = 0; i < 3 * SH_WLOG_PART; i++)
                if (log[i]) {
                    fprintf(stderr, "vulkan_headless: mirror frames: word %#x of the log the pool gave is %#x\n", i,
                            log[i]);
                    return 1;
                }
        }
        /* the program: its bytes in the mirror where a buffer holds them, and its stores in the log */
        memset(mwant, 0, sizeof mwant);
        for (u32 r = 0; r < 8; r++) {
            const MfRun* run = &fr->run[r];
            const u32 len = mf_len(fr, run->part), to = run->to < len ? run->to : len;
            if (run->from == run->to) continue;
            mf_log(log, run->part, run->from, run->to);
            if (!mf_folded(fr, run->part) || run->from >= to) continue;
            CHECK(!owned_test_fill(vk_mirror_buffer(), mf_base(run->part) + run->from, to - run->from,
                                   (u8)(0xc0 + f * 8 + r)));
            for (u32 x = run->from; x < to; x++) {
                const u64 b = mf_base(run->part) - MF_CPU + x;
                mwant[b / 32] |= 1u << (b % 32);
            }
        }
        VkBuffer src;
        u64 at;
        u8* staged = vk_stage_upload(sizeof log, 16, &src, &at);
        CHECK(staged);
        memcpy(staged, log, sizeof log);
        VkBufferCopy copy = {at, 0, sizeof log};
        vk_all_barrier(vk_cmd());
        rec_CmdCopyBuffer(vk_cmd(), src, vk_wlog_buffer(l), 1, &copy);
        vk_all_barrier(vk_cmd());
        /* as compute.c records them after the dispatch */
        VkMirFold fo[3];
        u32 nf = 0;
        for (u32 p = 0; p < 3; p++)
            if (mf_folded(fr, p)) fo[nf++] = (VkMirFold){mf_base(p), mf_len(fr, p), p};
        const u32 cleared = vk_mirror_fold(vk_cmd(), vk_wlog_buffer(l), vk_wlog_size(l), fo, nf);
        CHECK(cleared == (fr->dbound ? 7u : 3u));
        vk_wlog_clear(vk_cmd(), l, cleared);
        vk_wlog_ready(l);
        kept = l;
        /* the CPU's writes in flight */
        for (u32 x = 0; x < MF_LEN; x++) GP(MF_CPU)[x] = (u8)((x * 7 + (f + 2) * 0x35) & 0x7f);
        memcpy(want, GP(MF_CPU), MF_LEN);
        for (u32 r = 0; r < 8; r++) {
            const MfRun* run = &fr->run[r];
            const u32 len = mf_len(fr, run->part), to = run->to < len ? run->to : len;
            if (mf_folded(fr, run->part))
                for (u32 x = run->from; x < to; x++) want[mf_base(run->part) - MF_CPU + x] = (u8)(0xc0 + f * 8 + r);
        }
        /* the mask the folds left, read before the submit's harvest clears it: the frame's bytes and no others */
        const u64 lands = own_landings();
        CHECK(!buffer_read(vk_mirror_mask(), MF_CPU / 8, sizeof mask, (u8*)mask));
        for (u32 w = 0; w < MF_LEN / 32; w++)
            for (u32 k = 0; mask[w] != mwant[w] && k < 32; k++)
                if ((mask[w] ^ mwant[w]) >> k & 1) mf_fail(0, f, 0, w * 32 + k, mask[w] >> k & 1, mwant[w] >> k & 1);
        /* the log clear again for the next frame: its folds zeroed the windows they read, the fill the part no fold
           read */
        CHECK(!buffer_read(vk_wlog_buffer(l), 0, sizeof log, (u8*)log));
        for (u32 i = 0; i < 3 * SH_WLOG_PART; i++)
            if (log[i]) mf_fail(0, f, 1, i, log[i], 0);
        vk_wlog_unref(l); /* the dispatch's reference, which its work held */
        /* landed: the frame's bytes over the CPU's, and the CPU's everywhere else */
        vk_mirror_publish();
        vk_mirror_mem_need(MF_GVA, MF_LEN);
        own_acquire(MF_CPU, MF_LEN);
        CHECK(!vk_mirror_pending(MF_GVA, MF_LEN));
        CHECK(!own_traps() || (f & 1 ? own_landings() == lands : own_landings() > lands)); /* each path taken */
        for (u32 x = 0; x < MF_LEN; x++)
            if (GP(MF_CPU)[x] != want[x]) mf_fail(0, f, 2, x, GP(MF_CPU)[x], want[x]);
    }
    g_vk_present = present;
    if (mf_report(0)) return 1;
    printf("mirror frames: %u frames took one write log, each left clear by its folds and fills, and each landed the "
           "bytes its own stores logged over the CPU's writes in flight, and no others (%s)\n",
           MF_FRAMES,
           own_traps() ? "the even frames' pages the mirror's until the CPU touched them, the odd ones' landed with "
                         "their submit"
                       : "each frame's pages landed with its submit");
    return 0;
}

/* The same frames through a program's own stores: an SM86 program in the mirror, as compute.c dispatches it (its log,
   its folds and the log's clear), each invocation a byte store at the offset and of the value its constants give.
   Eight dispatches a frame, one after another in the frame's commands: each takes the log the one before cleared, and
   stores in the windows the one before stored in. The CPU writes the other bytes of those windows while the frame is
   in flight: each frame lands the bytes its dispatches stored, the last one's where two stored the same byte, and the
   CPU's everywhere else */
#define MD_CPU    (HEAP_BASE + 0x25000000) /* the program, then each dispatch's constants */
#define MD_GVA    0xbd0000000ull
#define MD_N      8u  /* dispatches a frame */
#define MD_STORES 32u /* a workgroup, an invocation a store */
static const u32 md_windows[] = {0, 1, 2, 2049, 3000, 4095};
static u32 md_offset(u32 frame, u32 d, u32 i) {
    return md_windows[(i + d) % 6] * 4096 + ((i * 131 + d * 17 + frame * 1031) & 4095);
}
static u8 md_byte(u32 frame, u32 d, u32 i) { return (u8)(0x80 | ((frame * 0x21 + d * 7 + i) & 0x7f)); }
static int check_mirror_dispatches(void) {
    static u8 want[MF_LEN];
    static u32 mask[MF_LEN / 32], mwant[MF_LEN / 32];
    const u64 code = MD_GVA, banks = MD_GVA + 0x1000; /* a constant bank of 0x1000 bytes for each dispatch */
    const u32 present = g_vk_present;
    const int sm86 = g_shader_sm86;
    u8 program[16 * 10] = {0};
    u32 n = 0, splits = 0;
    Sm86Word x = sm86_fix(0x919); /* S2R R0, TID.X */
    sm86_set(&x, 72, 8, 0x21);
    sm86_put(program, n++, x);
    x = sm86_fix(0x919); /* S2R R1, CTAID.X */
    sm86_set(&x, 16, 8, 1);
    sm86_set(&x, 72, 8, 0x25);
    sm86_put(program, n++, x);
    x = sm86_alu(0x024, 4, 2, 1); /* IMAD R2, R1, 32, R0 */
    sm86_set(&x, 32, 32, MD_STORES);
    sm86_set(&x, 64, 8, 0);
    sm86_put(program, n++, x);
    x = sm86_alu(0x024, 4, 3, 2); /* IMAD R3, R2, 4, RZ */
    sm86_set(&x, 32, 32, 4);
    sm86_set(&x, 64, 8, 255);
    sm86_put(program, n++, x);
    for (u32 k = 0; k < 2; k++) { /* LDC R6, c[0][R3 + 0x100], the store's offset; LDC R4, c[0][R3 + 0x200], its byte */
        x = sm86_fix(0xb82);
        sm86_set(&x, 16, 8, k ? 4 : 6);
        sm86_set(&x, 24, 8, 3);
        sm86_cbuf(&x, 0, k ? 0x200 : 0x100);
        sm86_set(&x, 73, 3, 4);
        sm86_put(program, n++, x);
    }
    x = sm86_fix(0xab9); /* ULDC.64 UR6, c[0][0x28]: the buffer */
    sm86_set(&x, 16, 6, 6);
    sm86_cbuf(&x, 0, 0x28);
    sm86_set(&x, 73, 3, 5);
    sm86_put(program, n++, x);
    x = sm86_fix(0x986); /* STG.E.U8 [R6.U32 + UR6], R4 */
    sm86_set(&x, 24, 8, 6);
    sm86_set(&x, 32, 8, 4);
    sm86_set(&x, 64, 6, 6);
    sm86_set(&x, 72, 1, 1);
    sm86_set(&x, 73, 3, 0);
    sm86_set(&x, 84, 3, 1);
    sm86_set(&x, 91, 1, 1);
    sm86_put(program, n++, x);
    sm86_put(program, n++, sm86_exit());
    sm86_put(program, n, sm86_bra(n, n, 7, 0));
    CHECK(!mf_map());
    CHECK(mem_map(MD_CPU, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(MD_GVA, MD_CPU, 0x10000);
    CHECK(!gmem_write(code, program, sizeof program));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    u32 qmd[64] = {0};
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = MD_STORES << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1;
    memset(mf_diff[1], 0, sizeof mf_diff[1]);
    g_shader_sm86 = 1;
    for (u32 k = 0; k < MF_FRAMES; k++) {
        if (!(k & 1)) g_vk_present += 1300; /* as check_mirror_frames: the even frames' pages GPU-owned once done */
        for (u32 d = 0; d < MD_N; d++) {
            u32 bank[0x300 / 4] = {0};
            bank[0x28 / 4] = (u32)MF_GVA;
            bank[0x2c / 4] = (u32)(MF_GVA >> 32);
            for (u32 i = 0; i < MD_STORES; i++) {
                bank[0x100 / 4 + i] = md_offset(k, d, i);
                bank[0x200 / 4 + i] = md_byte(k, d, i);
            }
            CHECK(!gmem_write(banks + d * 0x1000, bank, sizeof bank));
        }
        memset(mwant, 0, sizeof mwant);
        for (u32 d = 0; d < MD_N; d++)
            for (u32 i = 0; i < MD_STORES; i++) mwant[md_offset(k, d, i) / 32] |= 1u << (md_offset(k, d, i) % 32);
        vk_ww_new_epoch();
        const u64 ran = g_vk_dispatches, again = vk_wlog_again(), submits = vk_submit_number();
        for (u32 d = 0; d < MD_N; d++) {
            DispatchState ds;
            const u64 bank = banks + d * 0x1000;
            qmd[QMD_CONST_BUFFER_CONFIG] = (u32)bank;
            qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(bank >> 32) | 0x1000u << 15;
            mx_dispatch_state(qmd, channel->rcomp, code, &ds);
            vk_dispatch(&ds);
        }
        /* each in the mirror, after the first each with the log the one before cleared */
        if (g_vk_dispatches != ran + MD_N || vk_wlog_again() < again + MD_N - 1 || !vk_mirror_pending(MF_GVA, MF_LEN)) {
            fprintf(stderr,
                    "vulkan_headless: mirror dispatches: frame %u ran %llu of %u dispatches, %llu of them took a log "
                    "cleared before (%u at least wanted), the buffer pending %d (1 wanted)\n",
                    k, (unsigned long long)(g_vk_dispatches - ran), MD_N, (unsigned long long)(vk_wlog_again() - again),
                    MD_N - 1, vk_mirror_pending(MF_GVA, MF_LEN));
            CHECK(0);
        }
        /* a submit between them harvests the ones before it: their bits are gone from the mask by then, and they land
           once that submit is done, before the CPU's writes below or after them */
        const int split = vk_submit_number() != submits;
        splits += split;
        /* the CPU's writes in flight: every byte of the windows the frame's stores wrote, but those bytes. A bit an
           earlier frame left there lands the mirror's older byte over the CPU's */
        for (u32 w = 0; w < sizeof md_windows / sizeof *md_windows; w++)
            for (u32 a = md_windows[w] * 4096; a < md_windows[w] * 4096 + 4096; a++)
                if (!(mwant[a / 32] >> (a % 32) & 1)) GP(MF_CPU)[a] = (u8)((a * 7 + k * 0x35 + 0x11) & 0x7f);
        memcpy(want, GP(MF_CPU), MF_LEN);
        for (u32 d = 0; d < MD_N; d++)
            for (u32 i = 0; i < MD_STORES; i++) want[md_offset(k, d, i)] = md_byte(k, d, i); /* the last one's */
        const u64 lands = own_landings();
        CHECK(!buffer_read(vk_mirror_mask(), MF_CPU / 8, sizeof mask, (u8*)mask)); /* before the harvest clears it */
        for (u32 w = 0; w < MF_LEN / 32; w++)
            for (u32 b = 0; mask[w] != mwant[w] && b < 32; b++)
                if ((mask[w] & ~mwant[w]) >> b & 1 || (!split && (~mask[w] & mwant[w]) >> b & 1))
                    mf_fail(1, k, 0, w * 32 + b, mask[w] >> b & 1, mwant[w] >> b & 1);
        vk_mirror_publish();
        vk_mirror_mem_need(MF_GVA, MF_LEN);
        own_acquire(MF_CPU, MF_LEN);
        CHECK(!vk_mirror_pending(MF_GVA, MF_LEN));
        CHECK(!own_traps() || (k & 1 ? own_landings() == lands : own_landings() > lands));
        for (u32 a = 0; a < MF_LEN; a++)
            if (GP(MF_CPU)[a] != want[a]) mf_fail(1, k, 2, a, GP(MF_CPU)[a], want[a]);
    }
    g_shader_sm86 = sm86;
    g_vk_present = present;
    free(channel);
    if (mf_report(1)) return 1;
    printf("mirror dispatches: %u frames of %u dispatches of a program in the mirror, each taking the log the one "
           "before cleared (%u frames with a submit between them): each frame landed the bytes its stores wrote, and "
           "the CPU's writes in flight stayed\n",
           MF_FRAMES, MD_N, splits);
    return 0;
}

static int check_mirror(void) {
    const u64 cpu = HEAP_BASE + 0x7c0000, gva = 0xa600000, len = 0x3000;
    static u8 got[0x3000];
    if (!vk_mirror_on()) {
        puts("The GPU mirror is off on this device: not checked");
        return 0;
    }
    gmmu_map(gva, cpu, len);
    vk_lock();
    vk_finish();
    u32 validation = g_vk_validation_messages;
    for (u32 i = 0; i < len; i++) GP(cpu)[i] = (u8)(i * 7 + 3);
    vk_ww_new_epoch();
    VkBuffer buf;
    u64 off;
    /* bound: guest memory's bytes, at the guest address */
    CHECK(vk_mirror_slice(gva, len, &buf, &off));
    CHECK(buf == vk_mirror_buffer() && off == cpu);
    CHECK(!mirror_read(cpu, len, got));
    CHECK(!mirror_expect(got, GP(cpu), len, 0, 0, 0));
    /* a result landed in guest memory by the renderer in the same write epoch (a dispatch outside the mirror, which
       stamps it: gpu_mem_written): bound again in that epoch, the binding takes it, where it trusted its check of
       the epoch and kept the bytes from before */
    GP(cpu)[0x800] = 0xc3;
    gpu_mem_written(gva + 0x800, 1);
    CHECK(vk_mirror_slice(gva, len, &buf, &off));
    CHECK(!mirror_read(cpu, len, got));
    CHECK(!mirror_expect(got, GP(cpu), len, 0, 0, 0));
    /* a program writes 16 bytes at 0x100, pending on page 0; the CPU writes the byte after them, and one on page 2,
       where nothing is pending: bound again, the CPU's bytes go in, page 2 whole and page 0 the bytes it changed, and
       the program's stay */
    CHECK(!mirror_write(cpu + 0x100, 16, 0x77, 1));
    vk_mirror_wrote(cpu + 0x100, 16);
    GP(cpu)[0x110] = 0x5a;
    GP(cpu)[0x2003] = 0xa5;
    vk_ww_new_epoch();
    CHECK(vk_mirror_slice(gva, len, &buf, &off));
    {
        static u8 before_landing[0x3000];
        memcpy(before_landing, GP(cpu), len);
        CHECK(!mirror_read(cpu, len, got)); /* its finish takes the program's bytes too */
        CHECK(!mirror_expect(got, before_landing, len, 0x100, 16, 0x77));
        /* gmem_read itself, of the program's bytes while they are pending: the mirror's own flag makes it ask for them
           (g_mir_any), with no graphics result, storage image or write in place pending anywhere. Without the flag the
           read asked for nothing and took the bytes from before the program's writes */
        {
            u8 seen[16];
            CHECK(g_mir_any);
            CHECK(!gmem_read(gva + 0x100, seen, sizeof seen));
            for (u32 i = 0; i < sizeof seen; i++) CHECK(seen[i] == 0x77);
        }
        /* the CPU's read, as gmem_read makes it: what is pending first, then what the GPU owns lands */
        vk_mirror_mem_need(gva, len);
        own_acquire(cpu, len);
    }
    /* a range over the same guest memory at another GPU address is the same bytes */
    gmmu_map(gva + 0x100000, cpu + 0x1000, 0x1000);
    CHECK(vk_mirror_slice(gva + 0x100000, 0x1000, &buf, &off));
    CHECK(off == cpu + 0x1000);

    /* the CPU's touch above is old now (vk_own_cpu_read's 1200 presents): the page takes the published path below */
    g_vk_present += 1200;
    /* written by a program: pending, then once its submit is done the page it wrote stays the mirror's, GPU-owned,
       with the byte the CPU wrote beside the program's meanwhile taken into it; nothing lands until the CPU touches the
       page (where the host traps guest memory), then the page lands whole. The bytes the program didn't log are guest
       memory's either way (the 8 bytes at 0x400, changed without being logged, which no program's stores leave: the
       page landed before, so they were taken from guest memory into the mirror) */
    memcpy(got, GP(cpu), len);
    CHECK(!mirror_write(cpu + 0x200, 24, 0x66, 1));
    CHECK(!mirror_write(cpu + 0x400, 8, 0x99, 0));
    vk_mirror_wrote(cpu, len);
    CHECK(vk_mirror_pending(gva, len) && !vk_mirror_pending(gva + 0x10000, 0x100));
    GP(cpu)[0x219] = 0x11;
    got[0x219] = 0x11;
    u64 lands = own_landings();
    vk_finish();
    /* taken in once its submit is done, but not published inside the landing, which can run in a draw's setup:
       pending until a point where nothing is being recorded */
    {
        OwnRange o0[2];
        CHECK(!own_traps() || (vk_mirror_pending(gva, len) && !own_lookup(cpu, len, o0, 2)));
    }
    vk_mirror_publish();
    CHECK(!vk_mirror_pending(gva, len));
    if (own_traps()) {
        OwnRange o[4];
        CHECK(own_lookup(cpu, len, o, 4) == 1 && o[0].addr == cpu && o[0].size == 0x1000 &&
              ((const VkOwnRes*)o[0].res)->buf == vk_mirror_buffer());
        CHECK(own_landings() == lands); /* not landed: the CPU hasn't touched it */
        /* bound again while owned: the mirror keeps the program's bytes, guest memory under it isn't copied in */
        vk_ww_new_epoch();
        CHECK(vk_mirror_slice(gva, len, &buf, &off));
        {
            static u8 m[0x3000];
            CHECK(!mirror_read(cpu, 0x1000, m));
            for (u32 i = 0; i < 0x1000; i++) {
                u8 want = i >= 0x200 && i < 0x218 ? 0x66 : got[i];
                if (m[i] != want) {
                    fprintf(stderr, "vulkan_headless: mirror: owned byte %#x is %#x, not %#x\n", i, m[i], want);
                    CHECK(0);
                }
            }
        }
        own_acquire(cpu, len); /* the CPU's touch: it lands */
        CHECK(own_landings() > lands && !own_lookup(cpu, len, o, 4));
    }
    for (u32 i = 0; i < len; i++) {
        u8 want = i >= 0x200 && i < 0x218 ? 0x66 : got[i];
        if (GP(cpu)[i] != want) {
            fprintf(stderr, "vulkan_headless: mirror: landed byte %#x is %#x, not %#x\n", i, GP(cpu)[i], want);
            CHECK(0);
        }
    }
    /* bound again, nothing pending: the mirror is guest memory's bytes, the landed ones with them (the 8 bytes at
       0x400 that no store logged, which no program leaves, are guest memory's again) */
    vk_ww_new_epoch();
    CHECK(vk_mirror_slice(gva, len, &buf, &off));
    {
        static u8 m[0x3000];
        CHECK(!mirror_read(cpu, len, m));
        CHECK(!mirror_expect(m, GP(cpu), len, 0, 0, 0));
    }

    /* touched lately (the landing above): a program's write to the page comes back with the submit's harvest and lands
       in guest memory once it is done, the bytes it logged and no others, with no owner left over the page and no
       landing of a trap when the CPU reads it */
    {
        static u8 before[0x1000];
        memcpy(before, GP(cpu), sizeof before);
        CHECK(!mirror_write(cpu + 0x300, 8, 0x3c, 1));
        CHECK(!mirror_write(cpu + 0x308, 8, 0xc3, 0)); /* not logged: guest memory keeps its bytes */
        vk_mirror_wrote(cpu, 0x1000);
        u64 lands = own_landings();
        vk_finish();
        vk_mirror_publish();
        OwnRange o[2];
        CHECK(!vk_mirror_pending(gva, 0x1000) && !own_lookup(cpu, 0x1000, o, 2) && own_landings() == lands);
        for (u32 i = 0; i < 0x1000; i++) {
            u8 want = i >= 0x300 && i < 0x308 ? 0x3c : before[i];
            if (GP(cpu)[i] != want) {
                fprintf(stderr, "vulkan_headless: mirror: hot byte %#x is %#x, not %#x\n", i, GP(cpu)[i], want);
                CHECK(0);
            }
        }
        CHECK(own_landings() == lands);
    }

    /* more pages written than the harvest's room (64 at first): those past it land by a pass of their own */
    {
        const u64 c2 = HEAP_BASE + 0x5000000, g2 = 0xa700000, l2 = 0x100000; /* past the heap */
        CHECK(mem_map(c2, l2, MS_NORMAL, PERM_RW) == 0);
        gmmu_map(g2, c2, l2);
        for (u32 i = 0; i < l2; i++) GP(c2)[i] = (u8)(i * 5);
        vk_ww_new_epoch();
        CHECK(vk_mirror_slice(g2, l2, &buf, &off));
        for (u32 pg = 0; pg < 100; pg++) CHECK(!mirror_write(c2 + pg * 4096 + pg, 1, 0xe0 | (pg & 15), 1));
        vk_mirror_wrote(c2, l2);
        u64 gr, hv, pages, landed, overflows0, overflows;
        u32 room;
        vk_mirror_stats(&gr, &hv, &pages, &landed, &overflows0, &room);
        vk_finish();
        vk_mirror_stats(&gr, &hv, &pages, &landed, &overflows, &room);
        CHECK(overflows == overflows0 + 1 && room >= 128); /* past its room once, and more room from then on */
        vk_mirror_mem_need(g2, l2); /* those the output took are the mirror's until the CPU touches them */
        own_acquire(c2, l2);
        for (u32 i = 0; i < l2; i++) {
            u32 pg = i / 4096;
            u8 want = pg < 100 && i % 4096 == pg ? (u8)(0xe0 | (pg & 15)) : (u8)(i * 5);
            if (GP(c2)[i] != want) {
                fprintf(stderr, "vulkan_headless: mirror: landed byte %#x of the 100 pages is %#x, not %#x\n", i,
                        GP(c2)[i], want);
                CHECK(0);
            }
        }
    }
    /* a draw's constants over a program's pending write, at an address a uniform buffer doesn't bind at: copied on the
       GPU from the mirror into memory it binds at, the program's bytes, without waiting for the GPU */
    {
        static u8 u[20], guest[20];
        CHECK(!mirror_write(cpu + 0x208, 8, 0x3c, 1));
        vk_mirror_wrote(cpu + 0x208, 8);
        u64 finishes = g_vk_finishes;
        CHECK(vk_buf_get_uniform(gva + 0x204, 20, &buf, &off));
        CHECK(g_vk_finishes == finishes);
        CHECK(buf != vk_mirror_buffer() && off % vk_stage_align() == 0);
        memcpy(guest, GP(cpu + 0x204), 20);
        CHECK(!buffer_read(buf, off, 20, u));
        CHECK(!mirror_expect(u, guest, 20, 4, 8, 0x3c));
    }
    CHECK(!check_mirror_fold(cpu, gva, 0));
    CHECK(!check_mirror_fold(cpu, gva, 1));
    CHECK(!check_mirror_frames());
    CHECK(!check_mirror_dispatches());
    /* With the device's memory short, the granules no binding took for 600 presents give their memory back
       (vk_mirror_trim): a program's write there lands first, and the next binding has guest memory's bytes, that
       write among them. With memory to spare, or bound lately, they stay. Short is a budget of one byte, on any
       device. To spare is the device's to give: the use the trim reads is the driver's count of the heap, so that
       half is checked only where the count is under three quarters of the budget before the trims and after them
       (the trim acts at four fifths), and says the numbers where it is not */
    {
        u64 gr0, gr, hv, pages, landed, overflows;
        u32 room, present = g_vk_present;
        u8 byte = 0;
        CHECK(vk_mirror_slice(gva, len, &buf, &off));
        CHECK(!mirror_write(cpu + 0x310, 1, 0x6d, 1));
        vk_mirror_wrote(cpu + 0x310, 1);
        vk_finish();
        vk_mirror_stats(&gr0, &hv, &pages, &landed, &overflows, &room);
        CHECK(gr0);
        u64 usage, budget, usage_after, budget_after;
        vk_mem_use(&usage, &budget);
        g_vk_present += 601;
        for (u32 i = 0; i < 32; i++) vk_mirror_trim(); /* memory to spare: nothing goes */
        vk_mem_use(&usage_after, &budget_after);
        vk_mirror_stats(&gr, &hv, &pages, &landed, &overflows, &room);
        if (usage < budget / 4 * 3 && usage_after < budget_after / 4 * 3)
            CHECK(gr == gr0);
        else
            printf("mirror: memory to spare is not checked here: %llu of %llu MiB budgeted in use before the trims and "
                   "%llu of %llu after them, where the check needs under three quarters\n",
                   (unsigned long long)(usage >> 20), (unsigned long long)(budget >> 20),
                   (unsigned long long)(usage_after >> 20), (unsigned long long)(budget_after >> 20));
        u64 cap = vk_mem_budget_cap(1); /* a budget of one byte: short */
        g_vk_present += 601;
        CHECK(vk_mirror_slice(gva, len, &buf, &off)); /* this one bound now: it stays */
        u64 kept = 0;
        for (u32 i = 0; i < 32; i++) vk_mirror_trim(); /* each call a part of the granules, a landing or a release */
        vk_mirror_stats(&kept, &hv, &pages, &landed, &overflows, &room);
        g_vk_present += 601;
        for (u32 i = 0; i < 32; i++) vk_mirror_trim();
        vk_mirror_stats(&gr, &hv, &pages, &landed, &overflows, &room);
        vk_mem_budget_cap(cap);
        g_vk_present = present;
        if (!kept || kept >= gr0 || gr)
            fprintf(stderr, "vulkan_headless: mirror: of %llu granules %llu stayed with one bound lately (some, not "
                            "all, expected) and %llu once it was idle too (none expected)\n",
                    (unsigned long long)gr0, (unsigned long long)kept, (unsigned long long)gr);
        CHECK(kept && kept < gr0 && !gr);
        vk_finish();
        CHECK(!gmem_read(gva + 0x310, &byte, 1) && byte == 0x6d); /* landed before its granule went */
        CHECK(vk_mirror_slice(gva, len, &buf, &off)); /* bound again, from guest memory */
        static u8 again[0x400], guest[0x400];
        memcpy(guest, GP(cpu), sizeof guest);
        CHECK(!buffer_read(buf, off, sizeof again, again) && !memcmp(again, guest, sizeof again) && again[0x310] == 0x6d);
    }
    /* What keeps an idle granule with memory short (gran_release). One with a program's write that has not landed
       stays: its bytes are only there. Once the write is taken in and published as the mirror's own, the granule's
       pages land first, as the CPU's touch would land them, and it goes at a later call: guest memory has the byte,
       and no owner is left over memory that is gone */
    if (own_traps()) {
        u64 gr, hv, pages, landed, overflows;
        u32 room, present = g_vk_present;
        g_vk_present +=
            1300; /* past the presents a page the CPU touched keeps its writes off the GPU (the checks above) */
        CHECK(vk_mirror_slice(gva, len, &buf, &off));
        CHECK(!mirror_write(cpu + 0x320, 1, 0x7e, 1));
        vk_mirror_wrote(cpu + 0x320, 1);
        u64 cap = vk_mem_budget_cap(1);
        g_vk_present += 601;
        for (u32 i = 0; i < 32; i++) vk_mirror_trim();
        vk_mirror_stats(&gr, &hv, &pages, &landed, &overflows, &room);
        const u64 pending = gr;
        vk_finish();
        vk_mirror_publish();
        const u32 owned = own_lookup(cpu, 0x1000, NULL, 0);
        for (u32 i = 0; i < 32; i++) vk_mirror_trim();
        vk_mirror_stats(&gr, &hv, &pages, &landed, &overflows, &room);
        const u32 owned_after = own_lookup(cpu, 0x1000, NULL, 0);
        vk_mem_budget_cap(cap);
        g_vk_present = present;
        /* read only with no owner left: this thread holds vk_lock, and its read of an owned page could not land it */
        const u32 byte = owned_after ? 0x100 : GP(cpu)[0x320];
        if (!pending || !owned || owned_after || gr || byte != 0x7e) {
            fprintf(stderr,
                    "vulkan_headless: mirror: with memory short and a write not landed %llu idle granules stayed (its "
                    "own expected); published, %u owners were on its page (one expected), and after the trims %u "
                    "(none expected) with %llu granules (none expected) and the byte %#x in guest memory (0x7e "
                    "expected; 0x100: not read under an owner)\n",
                    (unsigned long long)pending, owned, owned_after, (unsigned long long)gr, byte);
            return 1;
        }
        CHECK(vk_mirror_slice(gva, len, &buf, &off)); /* bound again, for the checks after */
    }
    CHECK(g_vk_validation_messages == validation);
    vk_unlock();
    puts("The GPU mirror takes the bytes the CPU changed and keeps those the GPU wrote; the bytes its programs logged "
         "land, and no others; a draw reads pending bytes at any address without a wait");
    return 0;
}
