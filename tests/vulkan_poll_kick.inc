/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* A poll of the GPU thread (gpu.c's gpu_call_poll, as a replayed event wait of nvn_replay.c) that waits for a word a
   release writes (vk/release.c): the release kicks the GPU thread (gpu_kick_polls), and the sleep before the poll
   finds the word ends with that kick, not with WAIT_POLL_NS. No extracted shaders or game data are used */
#define PK_CPU  (HEAP_BASE + 0x3e000000)
#define PK_GVA  0x6f8000000ull
#define PK_LANE 60 /* a lane no queue of nvn_replay.c uses */

static int g_pk_seen; /* atomic: the poll found the word */
static u64 g_pk_ns;   /* when, set before g_pk_seen */
static u32 g_pk_end;  /* what ended the GPU thread's last sleep then (GpuPollStats.last) */
static int pk_poll(void* arg) {
    (void)arg;
    if (!__atomic_load_n((u32*)GP(PK_CPU), __ATOMIC_ACQUIRE)) {
        gpu_poll_why(GPU_POLL_EVENT);
        return 0;
    }
    GpuPollStats s;
    gpu_poll_stats(&s);
    g_pk_ns = plat_ns();
    g_pk_end = s.last;
    __atomic_store_n(&g_pk_seen, 1, __ATOMIC_RELEASE);
    return 1;
}
/* trials polls, each ended by a release: how many of them the last sleep's kick ended, and the median ns from the
   release to the poll that found it (*median). -1 when a poll never found its word */
static int pk_trials(u32 trials, u64* median) {
    u64 ns[32];
    int kicked = 0;
    if (trials > 32) trials = 32;
    for (u32 t = 0; t < trials; t++) {
        vk_lock();
        vk_finish(); /* nothing in flight: the release has no gate, and is made at vk_release_submit */
        vk_unlock();
        __atomic_store_n((u32*)GP(PK_CPU), 0, __ATOMIC_RELEASE);
        __atomic_store_n(&g_pk_seen, 0, __ATOMIC_RELEASE);
        GpuPollStats s0, s;
        gpu_poll_stats(&s0);
        gpu_call_poll(pk_poll, NULL, PK_LANE);
        /* the GPU thread found the wait not over and sleeps: two looks at it, then a while into the sleep */
        const u64 until = plat_ns() + 2000000000ull;
        do gpu_poll_stats(&s);
        while (s.misses[GPU_POLL_EVENT] < s0.misses[GPU_POLL_EVENT] + 2 && plat_ns() < until);
        plat_sleep_ns(200000 + (u64)(t % 5) * 50000);
        const u32 one = 1;
        const u64 t0 = plat_ns();
        vk_release_write(PK_GVA, &one, 4);
        vk_release_submit();
        while (!__atomic_load_n(&g_pk_seen, __ATOMIC_ACQUIRE) && plat_ns() < until + 2000000000ull) plat_yield();
        if (!__atomic_load_n(&g_pk_seen, __ATOMIC_ACQUIRE)) return -1;
        ns[t] = g_pk_ns - t0;
        kicked += g_pk_end == 0;
    }
    qsort(ns, trials, sizeof *ns, snx_compare_u64);
    *median = ns[trials / 2];
    return kicked;
}
static int check_poll_kick(void) {
    const u32 trials = 20;
    u64 med = 0;
    CHECK(mem_map(PK_CPU, 0x1000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(PK_GVA, PK_CPU, 0x1000);
    memset(GP(PK_CPU), 0, 0x1000);
    g_renderer->mem_may_have_changed();
    const int on = pk_trials(trials, &med);
    CHECK(mem_unmap(PK_CPU, 0x1000) == 0);
    g_renderer->mem_may_have_changed();
    if (on < (int)trials * 4 / 5)
        fprintf(stderr,
                "poll kick: a kick ended the sleep before %d of %u polls that a release ended, at least %u wanted\n", on,
                trials, trials * 4 / 5);
    CHECK(on >= (int)trials * 4 / 5);
    printf("poll kick: a release ended %d of %u polls' sleeps with a kick; median %.1f us from the release to the poll "
           "that found it\n",
           on, trials, (double)med / 1e3);
    return 0;
}
