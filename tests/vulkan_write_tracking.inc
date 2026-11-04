/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The write tracking's contract, for any change to how it finds written pages or protects them again: what a consumer
   (a texture, a buffer, any check against a baseline of the tracking's clock) was told is unchanged must be unchanged.
   Each check makes a write the way a game or the runtime does and asks every consumer of those pages afterwards:
   - two consumers over the same pages, asking at different times: one's question must not hide the write from the other
   - a view of guest memory (the alias pool) written through one address and read through the other
   - the CPU writing while scans run and protect pages again
   - a write after a draw recorded its upload and before its submission
   - the runtime's own writes: a system call's, a decommit's, a GPU engine's, a landing's
   - a range the GPU owns (memory traps) written by the CPU in the middle of an epoch
   Each case runs in a 4 MiB leaf of the tracking's table of its own. SNX_TEST_WW_ONLY=1 runs them alone */
#if !defined(_WIN32)
#include <unistd.h>
#endif
#define WT_CPU  (HEAP_BASE + 0x6000000)
#define WT_GVA  0x910000000ull
#define WT_LEAF 0x400000ull

/* RGBA8 16 by 16 and 8 by 32, block linear, at gva: two textures over the same bytes (1 KiB) */
static void wt_tic(u32 tic[8], u64 gva, u32 w, u32 h) {
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    const u32 t[8] = {rgba8, (u32)gva, (u32)(gva >> 32) | 3u << 21, 0, (w - 1) | 1u << 23, h - 1, 0, 0};
    memcpy(tic, t, sizeof t);
}
/* the texture of tic as a draw takes it (vk_lock held), its texel (0, 0) as its image holds it; 0 without it */
static u32 wt_texel(const u32 tic[8]) {
    Surf* t = vk_tex_from_tic(tic);
    return t ? dma_texel(t, 0, 0) : 0;
}
static u32 wt_fill(u8 v) { return 0x01010101u * v; }
/* the tracking runs here (vk_lock held) */
static int wt_tracking(u64 cpu) { return vk_written_since(GP(cpu), 0x1000, 0) >= 0; }

/* Two consumers over the same pages, each with its own baseline of the tracking's clock. The first asks after a write
   and takes it; the second asks in a later epoch, after a submission with nothing written: the write is still news to
   it. A third, whose baseline came after the write was taken in, is told nothing. Then the same with the renderer's
   own consumers: two textures over the same bytes and a buffer over their pages, asked in that order, at different
   epochs */
static int check_wt_two_consumers(void) {
    const u64 cpu = WT_CPU, gva = WT_GVA, size = 0x40000;
    u32 tic_a[8], tic_b[8];
    u8 got = 0;
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    memset(GP(cpu), 0x11, size);
    g_renderer->mem_may_have_changed();
    vk_lock();
    if (!wt_tracking(cpu)) {
        vk_unlock();
        printf("skip: two consumers of one write need write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        return 0;
    }
    /* the tracking's own: [0, 4) pages and [2, 6) pages */
    const u8 *a = GP(cpu), *b = GP(cpu) + 0x2000;
    u32 since_a = vk_ww_now(), since_b;
    CHECK(vk_written_since(a, 0x4000, 0) >= 0 && vk_written_since(b, 0x4000, 0) >= 0); /* watched from now */
    since_a = since_b = vk_ww_now();
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_lock();
    CHECK(vk_written_since(a, 0x4000, since_a) == 0 && vk_written_since(b, 0x4000, since_b) == 0);
    vk_unlock();
    GP(cpu)[0x3000] = 0x12; /* page 3: both ranges */
    g_renderer->mem_may_have_changed();
    vk_lock();
    u32 now = vk_ww_now();
    int first = vk_written_since(a, 0x4000, since_a);
    since_a = now;
    u32 after = vk_ww_now(); /* the write is taken in: a baseline from here on predates nothing */
    vk_unlock();
    g_renderer->mem_may_have_changed(); /* a submission, nothing written */
    vk_lock();
    int second = vk_written_since(b, 0x4000, since_b), third = vk_written_since(b, 0x4000, after);
    vk_unlock();
    if (first != 1 || second != 1 || third != 0)
        fprintf(stderr, "two consumers: the first told %d, the second %d (a later epoch), one after the write %d\n",
                first, second, third);
    CHECK(first == 1 && second == 1 && third == 0);
    /* the renderer's: texture A, texture B (another size over the same bytes), and a buffer over their pages */
    wt_tic(tic_a, gva, 16, 16);
    wt_tic(tic_b, gva, 8, 32);
    memset(GP(cpu), 0x11, size);
    g_renderer->mem_may_have_changed();
    vk_lock();
    u32 ta = wt_texel(tic_a), tb = wt_texel(tic_b);
    CHECK(ta == wt_fill(0x11) && tb == wt_fill(0x11) && strict_bytes(gva, size, 0x11, &got));
    vk_unlock();
    memset(GP(cpu), 0x22, size); /* the CPU writes all of it */
    g_renderer->mem_may_have_changed();
    vk_lock();
    ta = wt_texel(tic_a); /* A takes the write */
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_frame_next();
    vk_lock();
    tb = wt_texel(tic_b); /* B asks in a later epoch and frame */
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_lock();
    int buffer = strict_bytes(gva, size, 0x22, &got); /* the buffer later still */
    vk_unlock();
    if (ta != wt_fill(0x22) || tb != wt_fill(0x22) || !buffer)
        fprintf(stderr, "two consumers: texture A %08x, texture B %08x, the buffer %#x after the CPU wrote 0x22\n", ta,
                tb, got);
    CHECK(ta == wt_fill(0x22) && tb == wt_fill(0x22) && buffer);
    CHECK(mem_unmap(cpu, size) == 0);
    puts("write tracking: two consumers of one write, asked at different times, each told of it");
    return 0;
}

/* Two guest addresses of the same pages, a and b, with a texture over each (at the GPU addresses ga and gb): the CPU
   writes through b, then through a, and each time both textures must hold the new bytes. 1 when they do; what each
   held when they don't, on stderr under what */
static int wt_both_ways(const char* what, u64 a, u64 b, u64 ga, u64 gb, u64 size) {
    u32 tic_a[8], tic_b[8];
    memset(GP(a), 0x11, size);
    gmmu_map(ga, a, size);
    gmmu_map(gb, b, size);
    wt_tic(tic_a, ga, 16, 16);
    wt_tic(tic_b, gb, 16, 16);
    g_renderer->mem_may_have_changed();
    vk_lock();
    u32 ta = wt_texel(tic_a), tb = wt_texel(tic_b);
    u32 since = vk_ww_now();
    vk_unlock();
    if (ta != wt_fill(0x11) || tb != wt_fill(0x11)) {
        fprintf(stderr, "%s: written 0x11 before either texture was made, they hold %08x and %08x\n", what, ta, tb);
        return 0;
    }
    memset(GP(b), 0x22, 0x400); /* through b: the texture over a */
    g_renderer->mem_may_have_changed();
    vk_lock();
    int told_a = vk_written_since(GP(a), 0x1000, since), told_b = vk_written_since(GP(b), 0x1000, since);
    ta = wt_texel(tic_a);
    tb = wt_texel(tic_b);
    vk_unlock();
    int one = ta == wt_fill(0x22) && tb == wt_fill(0x22);
    if (!one)
        fprintf(stderr,
                "%s: written 0x22 through the second address, the first's texture holds %08x and the second's %08x "
                "(the tracking told the first %d, the second %d)\n",
                what, ta, tb, told_a, told_b);
    memset(GP(a), 0x33, 0x400); /* through a: the texture over b */
    g_renderer->mem_may_have_changed();
    vk_lock();
    ta = wt_texel(tic_a);
    tb = wt_texel(tic_b);
    vk_unlock();
    int other = ta == wt_fill(0x33) && tb == wt_fill(0x33);
    if (!other)
        fprintf(stderr,
                "%s: written 0x33 through the first address, the first's texture holds %08x and the second's %08x\n",
                what, ta, tb);
    return one && other;
}

/* A view of guest memory and its source are the same pages (the alias pool): a write through either address reaches a
   texture over the other's */
static int check_wt_alias(void) {
    const u64 src = WT_CPU + WT_LEAF, view = WT_CPU + 2 * WT_LEAF, size = 0x10000;
    CHECK(mem_map(src, size, MS_NORMAL, PERM_RW) == 0);
    vk_lock();
    int tracking = wt_tracking(src);
    vk_unlock();
    if (!tracking || !mem_pool_active() || mem_alias_begin(src, size)) {
        printf("skip: a view's writes need write tracking and the alias pool\n");
        CHECK(mem_unmap(src, size) == 0);
        return 0;
    }
    CHECK(mem_alias_map(view, src, size, MS_NORMAL, PERM_RW) == 0);
    int both = wt_both_ways("alias", src, view, WT_GVA + 0x1000000, WT_GVA + 0x2000000, size);
    CHECK(mem_alias_unmap(view, size, src) == 0);
    mem_alias_end(src, size);
    CHECK(mem_unmap(src, size) == 0);
    CHECK(both);
    puts("write tracking: a view and its source, written through one and read through the other");
    return 0;
}

/* Shared memory mapped twice (a service's pages at two guest addresses, the alias pool's): the same, mapping to
   mapping */
static int check_wt_shared(void) {
    const u64 a = WT_CPU + 8 * WT_LEAF, b = WT_CPU + 9 * WT_LEAF, size = 0x10000;
    u64 off = 0;
    u8* host = mem_pool_host(size, &off);
    if (!host) {
        printf("skip: shared memory mapped twice needs the alias pool\n");
        return 0;
    }
    CHECK(mem_map_pool(a, off, size, MS_SHARED, PERM_RW) == 0);
    CHECK(mem_map_pool(b, off, size, MS_SHARED, PERM_RW) == 0);
    vk_lock();
    int tracking = wt_tracking(a);
    vk_unlock();
    int both = !tracking || wt_both_ways("shared", a, b, WT_GVA + 0x7000000, WT_GVA + 0x8000000, size);
    CHECK(mem_unmap(b, size) == 0);
    CHECK(mem_unmap(a, size) == 0);
    mem_pool_host_free(host, off, size);
    if (!tracking) {
        printf("skip: shared memory mapped twice needs write tracking\n");
        return 0;
    }
    CHECK(both);
    puts("write tracking: shared memory at two addresses, written through one and read through the other");
    return 0;
}

/* The CPU writes 16 pages while the tracking scans them and protects them again, the scanner thread among it: a
   consumer that takes each change as the textures do (the clock before the question as its baseline, the bytes read
   after it) must hold the bytes memory has whenever it is told nothing changed. Checked with the writer held between
   its writes, 300 times, each after 64 writes at least; the writer's thread is a game's, outside the renderer's lock */
#define WT_RACE_PAGES 16u
static u32 g_wt_hold, g_wt_held, g_wt_stop, g_wt_writes;
static u64 g_wt_race_cpu;
static void wt_writer(void* arg) {
    u32 x = 0x9e3779b9u;
    (void)arg;
    while (!__atomic_load_n(&g_wt_stop, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&g_wt_hold, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_wt_held, 1, __ATOMIC_RELEASE);
            while (__atomic_load_n(&g_wt_hold, __ATOMIC_ACQUIRE) && !__atomic_load_n(&g_wt_stop, __ATOMIC_ACQUIRE))
                plat_yield();
            __atomic_store_n(&g_wt_held, 0, __ATOMIC_RELEASE);
            continue;
        }
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        u32 n = __atomic_add_fetch(&g_wt_writes, 1, __ATOMIC_RELAXED);
        snx_st_le32(GP(g_wt_race_cpu) + (u64)(x % WT_RACE_PAGES) * 0x1000 + (x >> 20) % 1024 * 4, n);
    }
}
static int check_wt_concurrent(void) {
    const u64 cpu = WT_CPU + 3 * WT_LEAF, size = WT_RACE_PAGES * 0x1000;
    static u8 seen[WT_RACE_PAGES * 0x1000];
    u32 lost = 0, told = 0, rounds = 300;
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    memset(GP(cpu), 0, size);
    vk_lock();
    if (!wt_tracking(cpu)) {
        vk_unlock();
        printf("skip: writes during scans need write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        return 0;
    }
    (void)vk_written_since(GP(cpu), size, 0);
    u32 since = vk_ww_now();
    memcpy(seen, GP(cpu), size);
    vk_unlock();
    g_wt_race_cpu = cpu;
    __atomic_store_n(&g_wt_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wt_hold, 0, __ATOMIC_RELEASE);
    CHECK(plat_thread(wt_writer, NULL, 0) == 0);
    for (u32 r = 0; r < rounds; r++) {
        /* questions while it writes (at least 4, and until it wrote 64 times, which protection's faults slow): each
           answer the scans give is taken as a texture takes it */
        u32 from = __atomic_load_n(&g_wt_writes, __ATOMIC_RELAXED);
        for (u32 k = 0; k < 4 || (__atomic_load_n(&g_wt_writes, __ATOMIC_RELAXED) - from < 64 && k < 100000); k++) {
            g_renderer->mem_may_have_changed();
            vk_lock();
            u32 now = vk_ww_now();
            if (vk_written_since(GP(cpu), size, since) == 1) {
                since = now;
                memcpy(seen, GP(cpu), size);
            }
            vk_unlock();
        }
        /* held: told nothing changed, the bytes must be the ones taken */
        __atomic_store_n(&g_wt_hold, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_wt_held, __ATOMIC_ACQUIRE)) plat_yield();
        g_renderer->mem_may_have_changed();
        vk_lock();
        u32 now = vk_ww_now();
        int w = vk_written_since(GP(cpu), size, since);
        if (!w && memcmp(seen, GP(cpu), size)) lost++;
        if (w) {
            told++;
            since = now;
            memcpy(seen, GP(cpu), size);
        }
        vk_unlock();
        __atomic_store_n(&g_wt_hold, 0, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_wt_held, __ATOMIC_ACQUIRE)) plat_yield();
    }
    __atomic_store_n(&g_wt_stop, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wt_hold, 0, __ATOMIC_RELEASE);
    plat_sleep_ns(20000000);
    u32 writes = __atomic_load_n(&g_wt_writes, __ATOMIC_RELAXED);
    if (lost)
        fprintf(stderr, "concurrent writes: %u of %u rounds told nothing changed over changed bytes\n", lost, rounds);
    CHECK(!lost && told && writes >= 64 * rounds);
    CHECK(mem_unmap(cpu, size) == 0);
    printf("write tracking: %u writes during scans, %u rounds told of them, none told of nothing over changed bytes\n",
           writes, told);
    return 0;
}

/* A write after a draw's texture and buffer were taken (their bytes copied for the GPU, recorded) and before that
   submission: the next draw, in the next epoch, takes the new bytes. What the first one read is the GPU's business
   (the program wrote under it); the write must not be lost to the next */
static int check_wt_record_submit(void) {
    const u64 cpu = WT_CPU + 4 * WT_LEAF, gva = WT_GVA + 0x3000000, size = 0x40000;
    u32 tic[8];
    VkBuffer b;
    u64 off;
    u8 got = 0;
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    wt_tic(tic, gva, 16, 16);
    memset(GP(cpu), 0x11, size);
    g_renderer->mem_may_have_changed();
    vk_lock();
    if (!wt_tracking(cpu)) {
        vk_unlock();
        printf("skip: a write between a record and its submission needs write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        return 0;
    }
    CHECK(wt_texel(tic) == wt_fill(0x11) && strict_bytes(gva, size, 0x11, &got));
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_lock();
    u64 submits = vk_submit_number();
    CHECK(vk_tex_from_tic(tic) && vk_buf_get_strict(gva, size, &b, &off)); /* recorded, not submitted */
    memset(GP(cpu), 0x22, size);                                           /* the program writes */
    CHECK(vk_submit_number() == submits);
    vk_finish(); /* its submission */
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_lock();
    u32 t = wt_texel(tic);
    int buffer = strict_bytes(gva, size, 0x22, &got);
    vk_unlock();
    if (t != wt_fill(0x22) || !buffer)
        fprintf(stderr, "record and submit: the next draw's texture %08x, buffer %#x after the CPU wrote 0x22\n", t,
                got);
    CHECK(t == wt_fill(0x22) && buffer);
    CHECK(mem_unmap(cpu, size) == 0);
    puts("write tracking: a write between a draw's record and its submission reaches the next draw");
    return 0;
}

/* The runtime's own writes into a texture's memory, each reaching it in the next epoch: a system call's (a read(2)
   into guest memory, between plat_ww_open and plat_ww_close), a decommit's (unmapped and mapped again, it reads as
   zero), a GPU engine's (a DMA copy over it), the CPU's after the engine's, and a save state's restore (written through
   the pool's file, which no fault or watch sees, then the range named changed as state.c does) */
static int check_wt_runtime_writes(void) {
    const u64 cpu = WT_CPU + 5 * WT_LEAF, gva = WT_GVA + 0x4000000, size = 0x10000;
    u32 tic[8], t;
    u8 bytes[0x400];
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    wt_tic(tic, gva, 16, 16);
    memset(GP(cpu), 0x11, size);
    g_renderer->mem_may_have_changed();
    vk_lock();
    if (!wt_tracking(cpu)) {
        vk_unlock();
        printf("skip: the runtime's writes need write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        return 0;
    }
    CHECK(wt_texel(tic) == wt_fill(0x11));
    vk_unlock();
    /* a system call's: the kernel fills the texture's bytes */
#if !defined(_WIN32)
    int fds[2];
    memset(bytes, 0x22, sizeof bytes);
    CHECK(pipe(fds) == 0 && write(fds[1], bytes, sizeof bytes) == (ssize_t)sizeof bytes);
    plat_ww_open(GP(cpu), sizeof bytes);
    ssize_t n = read(fds[0], GP(cpu), sizeof bytes);
    plat_ww_close(GP(cpu), sizeof bytes);
    close(fds[0]);
    close(fds[1]);
    CHECK(n == (ssize_t)sizeof bytes);
#else
    memset(bytes, 0x22, sizeof bytes);
    plat_ww_open(GP(cpu), sizeof bytes);
    memcpy(GP(cpu), bytes, sizeof bytes);
    plat_ww_close(GP(cpu), sizeof bytes);
#endif
    g_renderer->mem_may_have_changed();
    vk_lock();
    t = wt_texel(tic);
    vk_unlock();
    if (t != wt_fill(0x22))
        fprintf(stderr, "runtime writes: after a system call wrote 0x22 the texture holds %08x\n", t);
    CHECK(t == wt_fill(0x22));
    /* a decommit's: the pages read as zero */
    CHECK(mem_unmap(cpu, size) == 0 && mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0 && GP(cpu)[0] == 0);
    g_renderer->mem_may_have_changed();
    vk_lock();
    t = wt_texel(tic);
    vk_unlock();
    if (t) fprintf(stderr, "runtime writes: unmapped and mapped again, the texture holds %08x\n", t);
    CHECK(t == 0);
    /* a GPU engine's: a DMA copy of 0x33 over it, then the CPU's 0x44 over that */
    memset(GP(cpu + 0x8000), 0x33, 0x400);
    GpuDmaOp o;
    memset(&o, 0, sizeof o);
    o.width = o.line_in = o.line_out = 0x400;
    o.lines = 1;
    o.src = pitch_side(gva + 0x8000, 0);
    o.dst = pitch_side(gva, 0);
    g_renderer->mem_may_have_changed();
    CHECK(g_renderer->dma_copy(&o));
    vk_lock();
    t = wt_texel(tic);
    vk_unlock();
    if (t != wt_fill(0x33)) fprintf(stderr, "runtime writes: after a DMA copy of 0x33 the texture holds %08x\n", t);
    CHECK(t == wt_fill(0x33));
    guest_done(cpu, 0x400);
    own_acquire(cpu, 0x400);
    memset(GP(cpu), 0x44, 0x400);
    g_renderer->mem_may_have_changed();
    vk_lock();
    t = wt_texel(tic);
    vk_unlock();
    if (t != wt_fill(0x44))
        fprintf(stderr, "runtime writes: the CPU's 0x44 after the copy, the texture holds %08x\n", t);
    CHECK(t == wt_fill(0x44));
    /* a save state's restore, where the alias pool holds the pages */
    if (mem_pool_active() && !mem_alias_begin(cpu, size)) {
        MemBlock blk;
        /* the move into the pool remaps the pages, which read as written: taken in first, twice (a check's own marks
           read as news to the next one), so only the restore's own report can carry its bytes */
        for (int k = 0; k < 2; k++) {
            g_renderer->mem_may_have_changed();
            vk_lock();
            CHECK(wt_texel(tic) == wt_fill(0x44));
            vk_unlock();
        }
        mem_query(cpu, &blk);
        memset(bytes, 0x66, sizeof bytes);
        CHECK(blk.pool && mem_restore_write(blk.pool - 1 + (cpu - blk.addr), bytes, sizeof bytes) == 0 &&
              GP(cpu)[0] == 0x66);
        gpu_mem_changed(cpu, size);
        g_renderer->mem_may_have_changed();
        vk_lock();
        t = wt_texel(tic);
        vk_unlock();
        mem_alias_end(cpu, size);
        if (t != wt_fill(0x66))
            fprintf(stderr, "runtime writes: a restore wrote 0x66 through the pool's file, the texture holds %08x\n",
                    t);
        CHECK(t == wt_fill(0x66));
    } else
        printf("skip: a save state's restore needs the alias pool\n");
    CHECK(mem_unmap(cpu, size) == 0);
    puts(
        "write tracking: a system call's, a decommit's, a DMA copy's, the CPU's after it and a restore's writes reach a "
        "texture");
    return 0;
}

/* A range the GPU owns (a result published over it, its pages trapped) and a texture over it: the texture takes the
   owner's bytes; the CPU then writes the range in the middle of an epoch, its first touch landing the result (the
   landing's bytes are what the tracking's next report of those pages must not count as news by themselves). The next
   epoch's texture holds the CPU's bytes, and an untouched page beside it keeps the landing's */
static int check_wt_owned(void) {
    const u64 cpu = WT_CPU + 6 * WT_LEAF, gva = WT_GVA + 0x5000000, size = 0x10000;
    u32 tic[8], tic2[8], t, t2;
    if (!own_traps() || vk_uma()) {
        printf("skip: a GPU-owned range needs memory traps, and no unified memory (which publishes in place)\n");
        return 0;
    }
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    wt_tic(tic, gva, 16, 16);
    wt_tic(tic2, gva + 0x1000, 16, 16);
    memset(GP(cpu), 0x11, size);
    g_renderer->mem_may_have_changed();
    vk_lock();
    if (!wt_tracking(cpu)) {
        vk_unlock();
        printf("skip: a GPU-owned range's writes need write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        return 0;
    }
    CHECK(wt_texel(tic) == wt_fill(0x11) && wt_texel(tic2) == wt_fill(0x11));
    VkOwnRes* owner = vk_own_res_new(0x2000);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 0x2000, 0x42));
    vk_own_publish_owned(cpu, 0x2000, owner, 0);
    CHECK(own_lookup(cpu, 0x2000, NULL, 0) == 1);
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_lock();
    t = wt_texel(tic);
    t2 = wt_texel(tic2);
    vk_unlock();
    CHECK(t == wt_fill(0x42) && t2 == wt_fill(0x42));
    /* the CPU writes page 0 within the epoch: its trap lands both pages' results, then the write goes on */
    memset(GP(cpu), 0x55, 0x400);
    vk_lock();
    CHECK(own_lookup(cpu, 0x2000, NULL, 0) == 0 && GP(cpu)[0x1000] == 0x42);
    vk_unlock();
    g_renderer->mem_may_have_changed();
    vk_lock();
    t = wt_texel(tic);
    t2 = wt_texel(tic2);
    vk_unlock();
    if (t != wt_fill(0x55) || t2 != wt_fill(0x42))
        fprintf(stderr,
                "owned: after the CPU wrote 0x55 over a GPU-owned page the texture holds %08x, the landed page's %08x "
                "(0x42)\n",
                t, t2);
    CHECK(t == wt_fill(0x55) && t2 == wt_fill(0x42));
    vk_own_res_unref(owner);
    CHECK(mem_unmap(cpu, size) == 0);
    puts(
        "write tracking: the CPU's write over a GPU-owned range reaches a texture, the landing's bytes beside it stay");
    return 0;
}

/* A leaf with pages the host's scan watches beside a GPU-owned span it can't (armed for its trap in minor mode, not
   registered for the write watch): each epoch's scan takes the CPU's writes on the watched pages, leaves the span
   owned, and the tracking stays on. Told by its answers, not the bytes: with the tracking off every cache compares
   contents and the textures still match (a split scan that took the span for written pages failed at the first) */
static int check_wt_mixed_leaf(void) {
    const u64 cpu = WT_CPU + 7 * WT_LEAF, gva = WT_GVA + 0x6000000, size = 0x10000, at = cpu + 0x8000;
    u32 tic[8];
    if (!own_traps() || vk_uma()) {
        printf("skip: a GPU-owned span beside watched pages needs memory traps, and no unified memory\n");
        return 0;
    }
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, at, 0x1000);
    wt_tic(tic, gva, 16, 16);
    memset(GP(cpu), 0x11, size);
    g_renderer->mem_may_have_changed();
    vk_lock();
    if (!wt_tracking(at)) {
        vk_unlock();
        printf("skip: a GPU-owned span beside watched pages needs write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        return 0;
    }
    CHECK(wt_texel(tic) == wt_fill(0x11));
    VkOwnRes* owner = vk_own_res_new(0x2000);
    CHECK(owner && !owned_test_fill(owner->buf, 0, 0x2000, 0x42));
    vk_own_publish_owned(cpu, 0x2000, owner, 0);
    CHECK(own_lookup(cpu, 0x2000, NULL, 0) == 1);
    vk_unlock();
    int ok = 1;
    for (u8 v = 0x22; v <= 0x33; v += 0x11) { /* two epochs: the scan, then the scan after its re-arm */
        vk_lock();
        u32 since = vk_ww_now();
        vk_unlock();
        memset(GP(at), v, 0x1000);
        g_renderer->mem_may_have_changed();
        vk_lock();
        int told = vk_written_since(GP(at), 0x1000, since), owned = own_lookup(cpu, 0x2000, NULL, 0);
        u32 t = wt_texel(tic);
        int on = wt_tracking(at);
        vk_unlock();
        if (told != 1 || owned != 1 || t != wt_fill(v) || !on) {
            fprintf(stderr,
                    "mixed leaf: after the CPU wrote %#x beside a GPU-owned span, the tracking told %d (1), the span "
                    "is %s, the texture holds %08x, the tracking is %s\n",
                    v, told, owned == 1 ? "owned" : "landed", t, on ? "on" : "off");
            ok = 0;
        }
    }
    vk_own_res_unref(owner);
    CHECK(mem_unmap(cpu, size) == 0);
    CHECK(ok);
    puts("write tracking: a leaf with a GPU-owned span beside written pages, scanned twice with the tracking on");
    return 0;
}

/* the cases by name, for SNX_TEST_WW_ONLY=NAME */
static const struct {
    const char* name;
    int (*check)(void);
} g_wt_cases[] = {
    {"two_consumers", check_wt_two_consumers},
    {"concurrent", check_wt_concurrent},
    {"record_submit", check_wt_record_submit},
    {"runtime_writes", check_wt_runtime_writes},
    {"owned", check_wt_owned},
    {"alias", check_wt_alias},
    {"shared", check_wt_shared},
    {"mixed_leaf", check_wt_mixed_leaf},
};
/* whether the tracking answers, asked at a leaf no case maps */
static int wt_on(void) {
    vk_lock();
    int on = wt_tracking(WT_CPU + 16 * WT_LEAF);
    vk_unlock();
    return on;
}
/* SNX_TEST_WW_ONLY: 1 every case, stopping at the first that fails; each, every case with a line of its
   own (for a mutant's run: which cases see it); or one case's name */
static int check_write_tracking(void) {
    const char* only = getenv("SNX_TEST_WW_ONLY");
    int failed = 0, ran = 0, each = only && !strcmp(only, "each");
    for (size_t i = 0; i < sizeof g_wt_cases / sizeof *g_wt_cases; i++) {
        int all = !only || !strcmp(only, "1");
        if (!all && !each && strcmp(only, g_wt_cases[i].name)) continue;
        ran++;
        int on = wt_on(), r = g_wt_cases[i].check();
        /* a case that failed under the renderer's lock returned holding it: the next would run as its holder (a trap's
           fault there is fatal), so it lets go first */
        while (vk_lock_owner() == plat_tid()) vk_unlock();
        /* a case passes on the bytes the caches compared once the tracking failed under it, and those after it skip */
        if (on && !wt_on()) {
            fprintf(stderr, "%s: the write tracking was on before the case and answers -1 after it (it failed)\n",
                    g_wt_cases[i].name);
            r = 1;
        }
        if (each) printf("write tracking case %s: %s\n", g_wt_cases[i].name, r ? "FAILED" : "passed");
        if (r && !each) return 1;
        failed += r != 0;
    }
    if (!ran) {
        fprintf(stderr,
                "SNX_TEST_WW_ONLY=%s names no case: 1, each, two_consumers, concurrent, record_submit, runtime_writes, "
                "owned, alias, shared or mixed_leaf\n",
                only);
        return 2;
    }
    return failed ? 1 : 0;
}
