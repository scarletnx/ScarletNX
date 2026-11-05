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
   - a render target's write-back and freshness over pages a scan reports written at every scan (hot pages)
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
/* the race's state: the writer's pause (hold, held), its end (stop), its count of writes, its pages, and the bytes a
   consumer took (seen). The writer writes held and writes; the main thread the rest, twice a round at most, so they
   share a cache line */
static struct {
    u32 hold, held, stop, writes; /* atomic */
    u64 race_cpu;
    u32 race_pages; /* the writer's pages from race_cpu on */
    u8 seen[WT_RACE_PAGES * 0x1000];
} g_wt;
static void wt_writer(void* arg) {
    u32 x = 0x9e3779b9u;
    (void)arg;
    while (!__atomic_load_n(&g_wt.stop, __ATOMIC_ACQUIRE)) {
        if (__atomic_load_n(&g_wt.hold, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_wt.held, 1, __ATOMIC_RELEASE);
            while (__atomic_load_n(&g_wt.hold, __ATOMIC_ACQUIRE) && !__atomic_load_n(&g_wt.stop, __ATOMIC_ACQUIRE))
                plat_yield();
            __atomic_store_n(&g_wt.held, 0, __ATOMIC_RELEASE);
            continue;
        }
        x ^= x << 13, x ^= x >> 17, x ^= x << 5;
        u32 n = __atomic_add_fetch(&g_wt.writes, 1, __ATOMIC_RELAXED);
        snx_st_le32(GP(g_wt.race_cpu) + (u64)(x % g_wt.race_pages) * 0x1000 + (x >> 20) % 1024 * 4, n);
    }
}
static int check_wt_concurrent(void) {
    const u64 cpu = WT_CPU + 3 * WT_LEAF, size = WT_RACE_PAGES * 0x1000;
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
    memcpy(g_wt.seen, GP(cpu), size);
    vk_unlock();
    g_wt.race_cpu = cpu;
    g_wt.race_pages = WT_RACE_PAGES;
    __atomic_store_n(&g_wt.stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wt.hold, 0, __ATOMIC_RELEASE);
    CHECK(plat_thread(wt_writer, NULL, 0) == 0);
    for (u32 r = 0; r < rounds; r++) {
        /* questions while it writes (at least 4, and until it wrote 64 times, which protection's faults slow): each
           answer the scans give is taken as a texture takes it */
        u32 from = __atomic_load_n(&g_wt.writes, __ATOMIC_RELAXED);
        for (u32 k = 0; k < 4 || (__atomic_load_n(&g_wt.writes, __ATOMIC_RELAXED) - from < 64 && k < 100000); k++) {
            g_renderer->mem_may_have_changed();
            vk_lock();
            u32 now = vk_ww_now();
            if (vk_written_since(GP(cpu), size, since) == 1) {
                since = now;
                memcpy(g_wt.seen, GP(cpu), size);
            }
            vk_unlock();
        }
        /* held: told nothing changed, the bytes must be the ones taken */
        __atomic_store_n(&g_wt.hold, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_wt.held, __ATOMIC_ACQUIRE)) plat_yield();
        g_renderer->mem_may_have_changed();
        vk_lock();
        u32 now = vk_ww_now();
        int w = vk_written_since(GP(cpu), size, since);
        if (!w && memcmp(g_wt.seen, GP(cpu), size)) lost++;
        if (w) {
            told++;
            since = now;
            memcpy(g_wt.seen, GP(cpu), size);
        }
        vk_unlock();
        __atomic_store_n(&g_wt.hold, 0, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_wt.held, __ATOMIC_ACQUIRE)) plat_yield();
    }
    __atomic_store_n(&g_wt.stop, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wt.hold, 0, __ATOMIC_RELEASE);
    plat_sleep_ns(20000000);
    u32 writes = __atomic_load_n(&g_wt.writes, __ATOMIC_RELAXED);
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
    GpuDmaOp o = {0};
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

/* A render target's write-back keeps memory's bytes on the pages the CPU wrote since the target's baseline, and the
   GPU's pixels on the rest: so a page the tracking reports written when it wasn't loses the GPU's pixels. The CPU
   writes one page of the target's memory in every epoch first (hot under any scheme that leaves such a page writable
   and reports it at each scan), then the target is drawn over it all, then the CPU writes another page only, and the
   write-back must hold the GPU's blue everywhere but that page (a prototype that reported hot pages at every scan kept
   the CPU's old bytes there) */
static int check_wt_hot_page(void) {
    const u64 cpu = WT_CPU + 10 * WT_LEAF, gva = WT_GVA + 0x9000000, size = 0x10000; /* 128 by 128, 4 bytes a pixel */
    const u64 hot = cpu + 0x3000, later = cpu + 0x8000;
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    memset(GP(cpu), 0x11, size);
    vk_lock();
    int tracking = wt_tracking(cpu);
    vk_unlock();
    if (!tracking) {
        printf("skip: a render target's write-back over a hot page needs write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        free(ch);
        return 0;
    }
    for (u8 v = 0x20; v < 0x28; v++) { /* eight epochs, the page written and scanned in each */
        memset(GP(hot), v, 0x1000);
        g_renderer->mem_may_have_changed();
        vk_lock();
        (void)vk_written_since(GP(cpu), size, 0);
        vk_unlock();
    }
    rt_clear(ch, gva, 128, 128, 0, 1);
    for (int i = 0; i < 2; i++) { /* epochs with scans, the hot page left alone */
        g_renderer->mem_may_have_changed();
        vk_lock();
        (void)vk_written_since(GP(cpu), size, 0);
        vk_unlock();
    }
    memset(GP(later), 0x77, 0x1000);
    g_renderer->mem_may_have_changed();
    g_renderer->rt_flush(gva);
    guest_done(cpu, size);
    int blue =
        all_blue(GP(cpu), (u32)(later - cpu)) && all_blue(GP(later + 0x1000), (u32)(cpu + size - later - 0x1000));
    int kept = all_bytes(GP(later), 0x1000, 0x77);
    if (!blue || !kept)
        fprintf(stderr,
                "hot page: after the write-back the page written in every epoch before the target holds %02x %02x %02x "
                "%02x (blue: 00 00 ff ff), the rest of the target is %s, and the page the CPU wrote after it %s\n",
                GP(hot)[0], GP(hot)[1], GP(hot)[2], GP(hot)[3], blue ? "blue" : "not all blue",
                kept ? "keeps 0x77" : "lost 0x77");
    CHECK(mem_unmap(cpu, size) == 0);
    free(ch);
    CHECK(blue && kept);
    puts("write tracking: a render target over a page written in every epoch keeps its pixels there at its write-back");
    return 0;
}

/* Hot pages (write_tracking_hot=1, vulkan_write_tracking_hot): a page the scans found written in a row stays writable
   and reads as written at each scan, and a render target protects its hot pages again before each baseline
   (vk_ww_cool). The cases below fail without that step, each for one way a target meets a hot page: under two
   targets, at the target's own baseline, through another address, with the CPU writing beside the GPU in one frame,
   and going hot in the draw's own take-in. Without hot pages they hold as well: a scan reports only what was written */

/* n epochs, each with a check's scan of [cpu, cpu + size) */
static void wt_scans(u64 cpu, u64 size, int n) {
    for (int i = 0; i < n; i++) {
        g_renderer->mem_may_have_changed();
        vk_lock();
        (void)vk_written_since(GP(cpu), size, 0);
        vk_unlock();
    }
}
/* pages[0..n) of the memory at cpu written in each of epochs epochs, all of a page byte v + the epoch, and scanned
   over [cpu, cpu + size): hot after the second */
static void wt_heat(u64 cpu, u64 size, const u32* pages, int n, u8 v, int epochs) {
    for (int e = 0; e < epochs; e++) {
        for (int i = 0; i < n; i++) memset(GP(cpu + (u64)pages[i] * 0x1000), v + e, 0x1000);
        wt_scans(cpu, size, 1);
    }
}
#define WT_BLUE (-1) /* a page of blue pixels (rt_clear's) */
#define WT_RED  (-2)
#define WT_KEPT (-3) /* the page as it was before the write-back: the CPU's bytes */
/* each of the pages pages at cpu as want has it (WT_BLUE, WT_RED, WT_KEPT against kept, or every byte that value): 1
   when they all are, else each one that isn't on stderr under what (none when what is NULL) */
static int wt_expect(const char* what, u64 cpu, u32 pages, const int* want, const u8* kept) {
    static const u8 red[4] = {255, 0, 0, 255};
    int ok = 1;
    for (u32 pg = 0; pg < pages; pg++) {
        const u8* p = GP(cpu + (u64)pg * 0x1000);
        int good;
        if (want[pg] == WT_BLUE)
            good = all_blue(p, 0x1000);
        else if (want[pg] == WT_RED) {
            good = 1;
            for (u32 i = 0; i < 0x1000 && good; i += 4) good = !memcmp(p + i, red, 4);
        } else if (want[pg] == WT_KEPT)
            good = !memcmp(p, kept + (u64)pg * 0x1000, 0x1000);
        else
            good = all_bytes(p, 0x1000, (u8)want[pg]);
        if (good) continue;
        ok = 0;
        if (!what) continue;
        if (want[pg] >= 0)
            fprintf(stderr, "%s: page %u should be all %02x, and starts %02x %02x %02x %02x\n", what, pg, want[pg],
                    p[0], p[1], p[2], p[3]);
        else
            fprintf(stderr, "%s: page %u should be %s, and starts %02x %02x %02x %02x\n", what, pg,
                    want[pg] == WT_BLUE  ? "blue (00 00 ff ff)"
                    : want[pg] == WT_RED ? "red (ff 00 00 ff)"
                                         : "the CPU's bytes it held before the write-back",
                    p[0], p[1], p[2], p[3]);
    }
    return ok;
}
/* the rows [y0, y1) of the target at gva cleared blue: a draw over part of it, its first baseline of the present kept */
static void rt_clear_rows(GpuChannel* ch, u64 gva, u32 w, u32 h, u32 y0, u32 y1) {
    u32* r = ch->r3d;
    r[0x43E] |= 0x100; /* the clear limited to viewport 0's scissor */
    r[0x380] = 1;
    r[0x381] = w << 16;
    r[0x382] = y0 | y1 << 16;
    rt_clear(ch, gva, w, h, 0, 1);
    r[0x43E] &= ~0x100u;
    r[0x380] = 0;
}

/* Two targets over hot pages, one over part of the other (A over pages [0, 16), B over [8, 24)), each drawn after
   the pages went hot, B after the CPU wrote one of A's pages again: each write-back keeps the GPU's pixels but on the
   pages the CPU wrote after that target's baseline, and B's cooling of the pages they share is no write to A */
static int check_wt_hot_rt_overlap(void) {
    const u64 cpu = WT_CPU + 11 * WT_LEAF, ga = WT_GVA + 0xA000000, gb = ga + 0x8000, size = 0x18000;
    const u32 hot[3] = {3, 12, 20}, again = 5; /* A's alone, both's, B's alone; and A's, after A's draw */
    int want[24];
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(ga, cpu, size);
    memset(GP(cpu), 0x11, size);
    vk_lock();
    int tracking = wt_tracking(cpu);
    vk_unlock();
    if (!tracking) {
        printf("skip: two targets over hot pages need write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        free(ch);
        return 0;
    }
    wt_heat(cpu, size, hot, 3, 0x20, 8);
    rt_clear(ch, ga, 128, 128, 0, 0);
    wt_heat(cpu, size, &again, 1, 0x55, 3); /* after A's baseline: A keeps the last of them, 0x57 */
    rt_clear(ch, gb, 128, 128, 0, 1);
    wt_scans(cpu, size, 2);
    memset(GP(cpu + 22 * 0x1000), 0x77, 0x1000); /* after B's baseline: B keeps it */
    g_renderer->mem_may_have_changed();
    g_renderer->rt_flush(gb);
    g_renderer->rt_flush(ga);
    guest_done(cpu, size);
    for (u32 pg = 0; pg < 24; pg++) want[pg] = pg < 8 ? WT_RED : WT_BLUE;
    want[again] = 0x57;
    want[22] = 0x77;
    int ok = wt_expect("hot pages under two targets", cpu, 24, want, NULL);
    CHECK(mem_unmap(cpu, size) == 0);
    free(ch);
    CHECK(ok);
    puts("write tracking: two targets over hot pages, one over part of the other, keep their pixels at their "
         "write-backs");
    return 0;
}

/* The target's own baseline makes a page hot: the CPU wrote it in the epoch before, reported once, and again in the
   epoch of the draw, so the draw's own scan of the target reports it a second time in a row. The pages are cooled
   after that scan, or the page reads as written at every scan after the draw: the target isn't fresh (a texture over
   it would take memory's old bytes over its pixels, and it could go while it holds pixels memory hasn't got) and its
   write-back keeps the CPU's bytes there. A page hot long before is the same. Checked by the target's freshness two
   epochs later, then a write the CPU does make, then the write-back */
static int check_wt_hot_rt_fresh(void) {
    const u64 cpu = WT_CPU + 12 * WT_LEAF, gva = WT_GVA + 0xB000000, size = 0x10000;
    const u32 hot = 3, late = 5;
    int want[16];
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    memset(GP(cpu), 0x11, size);
    vk_lock();
    int tracking = wt_tracking(cpu);
    vk_unlock();
    if (!tracking) {
        printf("skip: a target's baseline over hot pages needs write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        free(ch);
        return 0;
    }
    rt_clear(ch, gva, 128, 128, 0, 0); /* the target, made red */
    wt_heat(cpu, size, &hot, 1, 0x20, 8);
    wt_heat(cpu, size, &late, 1, 0x50, 1);
    memset(GP(cpu + late * 0x1000), 0x51, 0x1000);
    g_renderer->mem_may_have_changed(); /* the draw's epoch, not scanned yet */
    rt_clear(ch, gva, 128, 128, 0, 1);
    wt_scans(cpu, size, 2);
    vk_lock();
    Surf* s = vk_rt_find(gva);
    int fresh = s && vk_rt_drawn_fresh(s);
    vk_unlock();
    memset(GP(cpu + 0x8000), 0x77, 0x1000);
    g_renderer->mem_may_have_changed();
    vk_lock();
    s = vk_rt_find(gva);
    int stale = s && !vk_rt_drawn_fresh(s);
    vk_unlock();
    g_renderer->rt_flush(gva);
    guest_done(cpu, size);
    for (u32 pg = 0; pg < 16; pg++) want[pg] = WT_BLUE;
    want[8] = 0x77;
    int ok = wt_expect("hot pages at a target's baseline", cpu, 16, want, NULL);
    if (!fresh || !stale)
        fprintf(stderr,
                "hot pages at a target's baseline: two epochs after its draw, with nothing written, the target %s; "
                "after the CPU wrote one of its pages it %s\n",
                fresh ? "was fresh" : "was not fresh (written since its draw, it said)",
                stale ? "was not fresh" : "was still fresh");
    CHECK(mem_unmap(cpu, size) == 0);
    free(ch);
    CHECK(ok && fresh && stale);
    puts("write tracking: a page that went hot at a target's own baseline, or long before, is no write after it");
    return 0;
}

/* a target at ga over the memory at over, a page of it hot through the other address through */
static int wt_hot_alias_way(const char* what, u64 over, u64 through, u64 ga, u64 size) {
    const u32 hot = 3;
    int want[16];
    GpuChannel* ch = calloc(1, sizeof *ch);
    if (!ch) return 0;
    gmmu_map(ga, over, size);
    memset(GP(over), 0x11, size);
    for (int e = 0; e < 8; e++) { /* written through the other address, scanned as the target's pages are */
        memset(GP(through + hot * 0x1000), 0x20 + e, 0x1000);
        wt_scans(over, size, 1);
    }
    rt_clear(ch, ga, 128, 128, 0, 1);
    wt_scans(over, size, 2);
    memset(GP(over + 0x8000), 0x77, 0x1000);
    g_renderer->mem_may_have_changed();
    g_renderer->rt_flush(ga);
    guest_done(over, size);
    for (u32 pg = 0; pg < 16; pg++) want[pg] = WT_BLUE;
    want[8] = 0x77;
    int ok = wt_expect(what, over, 16, want, NULL);
    free(ch);
    return ok;
}
/* A view and its source are the same pages (the alias pool), each with its own protection and hot count: a page hot
   at one address reads as written at the other too (the tracking marks a write at every address), so a target over
   either cools its pages at the other as well */
static int check_wt_hot_alias(void) {
    const u64 src = WT_CPU + 13 * WT_LEAF, view = WT_CPU + 14 * WT_LEAF, size = 0x10000;
    CHECK(mem_map(src, size, MS_NORMAL, PERM_RW) == 0);
    vk_lock();
    int tracking = wt_tracking(src);
    vk_unlock();
    if (!tracking || !mem_pool_active() || mem_alias_begin(src, size)) {
        printf("skip: hot pages at a view's address need write tracking and the alias pool\n");
        CHECK(mem_unmap(src, size) == 0);
        return 0;
    }
    CHECK(mem_alias_map(view, src, size, MS_NORMAL, PERM_RW) == 0);
    int one = wt_hot_alias_way("hot pages written through a view, a target over its source", src, view,
                               WT_GVA + 0xC000000, size);
    int other = wt_hot_alias_way("hot pages written through the source, a target over its view", view, src,
                                 WT_GVA + 0xD000000, size);
    CHECK(mem_alias_unmap(view, size, src) == 0);
    mem_alias_end(src, size);
    CHECK(mem_unmap(src, size) == 0);
    CHECK(one && other);
    puts("write tracking: a target over a view or its source keeps its pixels over pages hot at the other address");
    return 0;
}

/* The CPU and the GPU write the same target's pages in one frame: a game thread writes two of its pages (4 and 5)
   before the target's draw, through it and after it, while the scans run and a second draw covers part of the target
   again; four of its pages (2 to 5) went hot before the frame. The write-back keeps the CPU's bytes on the pages it
   wrote after the draw's baseline and the GPU's pixels on every other, hot before or not. 20 frames */
static int check_wt_hot_same_frame(void) {
    const u64 cpu = WT_CPU + 15 * WT_LEAF, gva = WT_GVA + 0xE000000, size = 0x10000;
    const u32 hot[4] = {2, 3, 4, 5}, frames = 20;
    u32 failed = 0;
    int want[16];
    u8* kept = malloc(size);
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch && kept);
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    memset(GP(cpu), 0x11, size);
    vk_lock();
    int tracking = wt_tracking(cpu);
    vk_unlock();
    if (!tracking) {
        printf("skip: the CPU and the GPU on one target's hot pages need write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        free(ch);
        free(kept);
        return 0;
    }
    for (u32 pg = 0; pg < 16; pg++) want[pg] = pg == 4 || pg == 5 ? WT_KEPT : WT_BLUE;
    g_wt.race_cpu = cpu + 4 * 0x1000;
    g_wt.race_pages = 2;
    __atomic_store_n(&g_wt.stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wt.hold, 1, __ATOMIC_RELEASE);
    CHECK(plat_thread(wt_writer, NULL, 0) == 0);
    while (!__atomic_load_n(&g_wt.held, __ATOMIC_ACQUIRE)) plat_yield();
    for (u32 f = 0; f < frames; f++) {
        wt_heat(cpu, size, hot, 4, 0x20, 4); /* the writer held */
        u32 from = __atomic_load_n(&g_wt.writes, __ATOMIC_RELAXED);
        __atomic_store_n(&g_wt.hold, 0, __ATOMIC_RELEASE);
        while (__atomic_load_n(&g_wt.writes, __ATOMIC_RELAXED) - from < 16) plat_yield();
        rt_clear(ch, gva, 128, 128, 0, 1);
        from = __atomic_load_n(&g_wt.writes, __ATOMIC_RELAXED);
        for (int k = 0; k < 4; k++) {
            wt_scans(cpu, size, 1);
            if (k == 1) rt_clear_rows(ch, gva, 128, 128, 0, 48); /* pages 0 to 5 drawn again */
        }
        while (__atomic_load_n(&g_wt.writes, __ATOMIC_RELAXED) - from < 64) plat_yield(); /* both pages, surely */
        __atomic_store_n(&g_wt.hold, 1, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&g_wt.held, __ATOMIC_ACQUIRE)) plat_yield();
        memcpy(kept, GP(cpu), size);
        g_renderer->mem_may_have_changed();
        g_renderer->rt_flush(gva);
        guest_done(cpu, size);
        if (!wt_expect(failed ? NULL : "the CPU and the GPU on one target's hot pages", cpu, 16, want, kept) &&
            !failed++)
            fprintf(stderr, "the CPU and the GPU on one target's hot pages: that was frame %u\n", f);
    }
    __atomic_store_n(&g_wt.stop, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&g_wt.hold, 0, __ATOMIC_RELEASE);
    plat_sleep_ns(20000000);
    if (failed)
        fprintf(stderr, "the CPU and the GPU on one target's hot pages: %u of %u frames wrong\n", failed, frames);
    CHECK(mem_unmap(cpu, size) == 0);
    free(ch);
    free(kept);
    CHECK(!failed);
    printf("write tracking: %u frames of the CPU and the GPU on one target's hot pages, each page whose it should be\n",
           frames);
    return 0;
}

/* A page reported once and written again goes hot at the next scan that reports it: when that scan is the target
   draw's own take-in, only a cool after the take-in protects it again, and a cool before it leaves it hot past the
   baseline (the target not fresh, nothing written since). The scanner thread scans the leaf at the epoch's start too,
   and it would make the page hot first: 8 GiB of leaves below the target's, wanted, keep it busy (it scans from the
   lowest leaf up). 40 draws; with the cool before the take-in 39 of 40 lose their freshness here */
static int check_wt_hot_rt_take_in(void) {
    const u64 cpu = WT_CPU + 2100 * WT_LEAF, gva = WT_GVA + 0xF000000, size = 0x10000;
    const u64 busy = WT_CPU + 32 * WT_LEAF, busy_len = 2048 * WT_LEAF;
    const u32 page = 5;
    int draws = 40, stale = 0;
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(mem_map(cpu, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gva, cpu, size);
    memset(GP(cpu), 0x11, size);
    vk_lock();
    int tracking = wt_tracking(cpu);
    vk_unlock();
    if (!tracking) {
        printf("skip: a page going hot at a draw's take-in needs write tracking\n");
        CHECK(mem_unmap(cpu, size) == 0);
        free(ch);
        return 0;
    }
    rt_clear(ch, gva, 128, 128, 0, 0); /* the target, made red */
    for (int d = 0; d < draws; d++) {
        memset(GP(cpu + page * 0x1000), 0x50, 0x1000);
        g_renderer->mem_may_have_changed();
        vk_lock();
        (void)vk_written_since(GP(cpu), size, 0);      /* reported once */
        (void)vk_written_since(GP(busy), busy_len, 0); /* the scanner's work at the next epoch, first */
        vk_unlock();
        memset(GP(cpu + page * 0x1000), 0x51, 0x1000);
        g_renderer->mem_may_have_changed();
        rt_clear(ch, gva, 128, 128, 0, 1);
        wt_scans(cpu, size, 2);
        vk_lock();
        Surf* s = vk_rt_find(gva);
        if (!s || !vk_rt_drawn_fresh(s)) stale++;
        vk_unlock();
    }
    if (stale)
        fprintf(
            stderr,
            "hot pages at a draw's take-in: %d of %d draws left the target not fresh two epochs later, with nothing "
            "written since\n",
            stale, draws);
    CHECK(mem_unmap(cpu, size) == 0);
    free(ch);
    CHECK(!stale);
    printf("write tracking: %d draws over a page that went hot at their own take-in, the target fresh after each\n",
           draws);
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
    {"hot_page", check_wt_hot_page},
    {"hot_rt_overlap", check_wt_hot_rt_overlap},
    {"hot_rt_fresh", check_wt_hot_rt_fresh},
    {"hot_alias", check_wt_hot_alias},
    {"hot_same_frame", check_wt_hot_same_frame},
    {"hot_rt_take_in", check_wt_hot_rt_take_in},
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
        fprintf(
            stderr,
            "SNX_TEST_WW_ONLY=%s names no case: 1, each, two_consumers, concurrent, record_submit, runtime_writes, "
            "owned, alias, shared, mixed_leaf, hot_page, hot_rt_overlap, hot_rt_fresh, hot_alias, hot_same_frame or "
            "hot_rt_take_in\n",
            only);
        return 2;
    }
    return failed ? 1 : 0;
}
