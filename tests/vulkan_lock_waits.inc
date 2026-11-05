/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What waits for vk_lock and what waits under it. A change of guest memory that nothing of the renderer covers goes on
   while another thread holds vk_lock (gpu/vk/renderer.c's mapping bracket); one over a GPU mapping or GPU-owned
   memory waits for it. */

/* a thread holding vk_lock until let go, as the GPU thread did while it waited for the GPU. 1: to take it, 2: held,
   3: to let it go, 0: let go */
static u32 g_lw_hold;
static void lw_holder(void* arg) {
    (void)arg;
    vk_lock();
    __atomic_store_n(&g_lw_hold, 2, __ATOMIC_RELEASE);
    plat_wake_word(&g_lw_hold);
    while (__atomic_load_n(&g_lw_hold, __ATOMIC_ACQUIRE) != 3) plat_wait_word(&g_lw_hold, 2, -1);
    vk_unlock();
    __atomic_store_n(&g_lw_hold, 0, __ATOMIC_RELEASE);
    plat_wake_word(&g_lw_hold);
}
static int lw_hold(void) {
    u32 v;
    __atomic_store_n(&g_lw_hold, 1, __ATOMIC_RELEASE);
    CHECK(plat_thread(lw_holder, NULL, 0) == 0);
    while ((v = __atomic_load_n(&g_lw_hold, __ATOMIC_ACQUIRE)) != 2) plat_wait_word(&g_lw_hold, v, -1);
    return 0;
}
static void lw_let_go(void) {
    u32 v;
    __atomic_store_n(&g_lw_hold, 3, __ATOMIC_RELEASE);
    plat_wake_word(&g_lw_hold);
    while ((v = __atomic_load_n(&g_lw_hold, __ATOMIC_ACQUIRE)) != 0) plat_wait_word(&g_lw_hold, v, -1);
}

/* work on a thread of its own, which the test waits for at most a while: 0 running, 1 done */
static u32 g_lw_done;
static void (*g_lw_fn)(void);
static void lw_worker(void* arg) {
    (void)arg;
    g_lw_fn();
    __atomic_store_n(&g_lw_done, 1, __ATOMIC_RELEASE);
    plat_wake_word(&g_lw_done);
}
static int lw_start(void (*fn)(void)) {
    g_lw_fn = fn;
    __atomic_store_n(&g_lw_done, 0, __ATOMIC_RELEASE);
    CHECK(plat_thread(lw_worker, NULL, 0) == 0);
    return 0;
}
/* 1 when the work is done within ms milliseconds (a negative ms: until it is) */
static int lw_done_within(i64 ms) {
    u64 until = plat_ns() + (u64)(ms < 0 ? 0 : ms) * 1000000u;
    while (!__atomic_load_n(&g_lw_done, __ATOMIC_ACQUIRE)) {
        u64 now = plat_ns();
        if (ms >= 0 && now >= until) return 0;
        plat_wait_word(&g_lw_done, 0, ms < 0 ? -1 : (i64)(until - now));
    }
    return 1;
}

#define LW_CPU  (HEAP_BASE + 0x6800000)
#define LW_SIZE 0x10000ull
#define LW_GVA  0x5d0000000ull
static u64 g_lw_cpu;
static void lw_map(void) { mem_map(g_lw_cpu, LW_SIZE, MS_NORMAL, PERM_RW); }
static void lw_unmap(void) { mem_unmap(g_lw_cpu, LW_SIZE); }
/* the change on its own thread while another holds vk_lock: whether it ended within ms, before that one let go */
static int lw_change_held(u64 cpu, void (*change)(void), i64 ms, int* went_on) {
    g_lw_cpu = cpu;
    CHECK(!lw_hold());
    CHECK(!lw_start(change));
    *went_on = lw_done_within(ms);
    lw_let_go();
    CHECK(lw_done_within(-1));
    return 0;
}
static int check_mapping_lock(void) {
    int went_on = 0, quick = !vk_uma(); /* guest memory read in place: every change waits (its imports) */
    /* nothing of the renderer's there: mapped and unmapped with vk_lock held elsewhere */
    CHECK(!lw_change_held(LW_CPU, lw_map, quick ? 5000 : 300, &went_on));
    CHECK(went_on == quick);
    CHECK(mem_mapped_run(LW_CPU, LW_SIZE) == LW_SIZE);
    CHECK(!lw_change_held(LW_CPU, lw_unmap, quick ? 5000 : 300, &went_on));
    CHECK(went_on == quick);
    CHECK(mem_mapped_run(LW_CPU, LW_SIZE) == 0);
    /* a GPU mapping shows it: the renderer reads it through that with vk_lock held, and the change waits */
    CHECK(mem_map(LW_CPU + LW_SIZE, LW_SIZE, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(LW_GVA, LW_CPU + LW_SIZE, LW_SIZE);
    CHECK(!lw_change_held(LW_CPU + LW_SIZE, lw_unmap, 300, &went_on));
    CHECK(!went_on);
    gmmu_space_unmap(LW_GVA, LW_SIZE);
    /* GPU-owned memory on its pages: what lands or drops it needs vk_lock, and the change waits */
    if (own_traps()) {
        const u64 cpu = LW_CPU + 2 * LW_SIZE;
        CHECK(mem_map(cpu, LW_SIZE, MS_NORMAL, PERM_RW) == 0);
        VkOwnRes* owner = vk_own_res_new(256);
        CHECK(owner);
        vk_lock();
        vk_own_publish_owned(cpu + 0x1000, 256, owner, 0);
        vk_unlock();
        CHECK(own_lookup(cpu, LW_SIZE, NULL, 0) == 1);
        CHECK(!lw_change_held(cpu, lw_unmap, 300, &went_on));
        CHECK(!went_on);
        CHECK(own_lookup(cpu, LW_SIZE, NULL, 0) == 0); /* dropped with the mapping */
        vk_own_res_unref(owner);
    }
    puts("mapping lock: a change of guest memory the renderer holds nothing of waits for no vk_lock, others do");
    return 0;
}
