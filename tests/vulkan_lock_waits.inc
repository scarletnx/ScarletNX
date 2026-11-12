/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What waits for vk_lock and what waits under it. A change of guest memory that nothing of the renderer covers goes on
   while another thread holds vk_lock (gpu/vk/renderer.c's mapping bracket); one over a GPU mapping or GPU-owned
   memory waits for it. A landing waits for the GPU's work on what it lands with no lock held (own.c's wait,
   gpu/vk/owned.c), and so does the GPU thread for its next frame slot (vk_frame_ready). A gate semaphore holds the
   GPU's work back until the test lets it go */

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

/* work on threads of their own, which the test waits for at most a while: 0 running, 1 done */
static u32 g_lw_done[2];
static void (*g_lw_fn[2])(void);
static void lw_worker(void* arg) {
    u32 slot = (u32)(uintptr_t)arg;
    g_lw_fn[slot]();
    __atomic_store_n(&g_lw_done[slot], 1, __ATOMIC_RELEASE);
    plat_wake_word(&g_lw_done[slot]);
}
static int lw_start_on(u32 slot, void (*fn)(void)) {
    g_lw_fn[slot] = fn;
    __atomic_store_n(&g_lw_done[slot], 0, __ATOMIC_RELEASE);
    CHECK(plat_thread(lw_worker, (void*)(uintptr_t)slot, 0) == 0);
    return 0;
}
/* 1 when the work is done within ms milliseconds (a negative ms: until it is) */
static int lw_done_on(u32 slot, i64 ms) {
    u64 until = plat_ns() + (u64)(ms < 0 ? 0 : ms) * 1000000u;
    while (!__atomic_load_n(&g_lw_done[slot], __ATOMIC_ACQUIRE)) {
        u64 now = plat_ns();
        if (ms >= 0 && now >= until) return 0;
        plat_wait_word(&g_lw_done[slot], 0, ms < 0 ? -1 : (i64)(until - now));
    }
    return 1;
}
static int lw_start(void (*fn)(void)) { return lw_start_on(0, fn); }
static int lw_done_within(i64 ms) { return lw_done_on(0, ms); }

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
        VkOwnRes* owner = vk_own_res_new(256, VK_OWN_BY_DRAW);
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

/* a timeline semaphore the next submission waits for (value 1, which the test signals) and signals (2) */
static int lw_gate(VkSemaphore* gate) {
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    CHECK(VK_OK(vkCreateSemaphore(g_vk.dev, &ci, NULL, gate)));
    vk_submit_after(*gate, 1, 2);
    return 0;
}
static int lw_open(VkSemaphore gate) {
    VkSemaphoreSignalInfo signal = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = gate, .value = 1};
    CHECK(VK_OK(vkSignalSemaphore(g_vk.dev, &signal)));
    return 0;
}
/* vk_lock is free: taken and let go at once. Else who holds it is said */
static int lw_lock_free(void) {
    if (vk_trylock()) {
        vk_unlock();
        return 1;
    }
    char path[64], name[32] = "?";
    u32 tid = vk_lock_owner();
    snprintf(path, sizeof path, "/proc/self/task/%u/comm", tid);
    FILE* f = fopen(path, "r");
    if (f) {
        if (!fgets(name, sizeof name, f)) name[0] = 0;
        fclose(f);
    }
    fprintf(stderr, "lock waits: thread %u (%s) holds vk_lock\n", tid, name);
    return 0;
}

#define LW_OWN_CPU (HEAP_BASE + 0x6900000)
static void lw_acquire(void) { own_acquire(LW_OWN_CPU, 256); }
/* A landing on a thread without vk_lock, of a result whose submission the GPU holds back: vk_lock stays free while it
   waits, and the bytes land once the GPU ran it */
static int check_landing_wait(void) {
    VkSemaphore gate;
    if (!own_traps()) return 0;
    CHECK(mem_map(LW_OWN_CPU, 0x1000, MS_NORMAL, PERM_RW) == 0);
    memset(GP(LW_OWN_CPU), 0x11, 0x1000);
    VkOwnRes* owner = vk_own_res_new(256, VK_OWN_BY_DRAW);
    CHECK(owner);
    vk_lock();
    VkBuffer upload;
    u64 at;
    u8* data = vk_stage_upload(256, 16, &upload, &at);
    CHECK(data);
    memset(data, 0x5a, 256);
    VkBufferCopy copy = {at, 0, 256};
    rec_CmdCopyBuffer(vk_cmd(), upload, owner->buf, 1, &copy);
    vk_own_publish_owned(LW_OWN_CPU, 256, owner, 0); /* written by the submission being recorded */
    CHECK(!lw_gate(&gate));
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    u64 writer = vk_last_submit();
    vk_unlock();
    CHECK(own_lookup(LW_OWN_CPU, 256, NULL, 0) == 1);
    CHECK(!lw_start(lw_acquire));
    int waits = !lw_done_within(300); /* the GPU holds the writes back */
    int lock_free = lw_lock_free();   /* and the landing waits for them without vk_lock */
    CHECK(!lw_open(gate));            /* first: what holds the GPU's work back would keep the test from ending */
    CHECK(lw_done_within(-1));
    CHECK(waits);
    CHECK(lock_free);
    vk_wait_submit(writer);
    for (u32 i = 0; i < 256; i++) CHECK(GP(LW_OWN_CPU)[i] == 0x5a);
    for (u32 i = 256; i < 0x1000; i++) CHECK(GP(LW_OWN_CPU)[i] == 0x11);
    CHECK(own_lookup(LW_OWN_CPU, 256, NULL, 0) == 0);
    vk_own_res_unref(owner);
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    CHECK(mem_unmap(LW_OWN_CPU, 0x1000) == 0);
    puts("landing wait: a landing waits for the GPU with vk_lock free, then lands the result");
    return 0;
}

#define LW_LAND_CPU (HEAP_BASE + 0x6a00000)
#define LW_LAND_GVA 0x5d1000000ull
static void lw_unmap_landing(void) { mem_unmap(LW_LAND_CPU, LW_SIZE); }
static void lw_map_other(void) { mem_map(LW_CPU + 4 * LW_SIZE, LW_SIZE, MS_NORMAL, PERM_RW); }
/* An unmap of guest memory a storage result is pending on lands it first, with vk_lock held, waiting for the GPU. A
   change of other guest memory, which the renderer holds nothing of, goes on meanwhile: the landing holds no
   pend_lock */
static int check_mapping_landing(void) {
    VkSemaphore gate;
    VkBuffer output, upload;
    u64 offset, at;
    CHECK(mem_map(LW_LAND_CPU, LW_SIZE, MS_NORMAL, PERM_RW) == 0);
    memset(GP(LW_LAND_CPU), 0x11, LW_SIZE);
    gmmu_map(LW_LAND_GVA, LW_LAND_CPU, LW_SIZE);
    vk_lock();
    vk_finish();
    CHECK(vk_buf_copy_writable(LW_LAND_GVA, 256, &output, &offset, 1));
    u8* data = vk_stage_upload(256, 16, &upload, &at);
    CHECK(data);
    memset(data, 0x22, 256);
    VkBufferCopy copy = {at, offset, 256};
    rec_CmdCopyBuffer(vk_cmd(), upload, output, 1, &copy);
    vk_buf_writable_commit();
    CHECK(!lw_gate(&gate));
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_unlock();
    CHECK(!lw_start_on(0, lw_unmap_landing));
    int lands = !lw_done_on(0, 300); /* its landing waits for the GPU */
    CHECK(!lw_start_on(1, lw_map_other));
    int other = lw_done_on(1, vk_uma() ? 300 : 5000) == !vk_uma(); /* in place: every change waits (its imports) */
    CHECK(!lw_open(gate));
    CHECK(lw_done_on(0, -1) && lw_done_on(1, -1));
    CHECK(lands);
    CHECK(other);
    CHECK(mem_mapped_run(LW_LAND_CPU, LW_SIZE) == 0 && mem_mapped_run(LW_CPU + 4 * LW_SIZE, LW_SIZE) == LW_SIZE);
    gmmu_space_unmap(LW_LAND_GVA, LW_SIZE);
    CHECK(mem_unmap(LW_CPU + 4 * LW_SIZE, LW_SIZE) == 0);
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    puts("mapping landing: an unmap's landing waits for the GPU while other guest memory changes go on");
    return 0;
}

/* The GPU thread's wait for the slot its next frame records in: with that slot's last submission held back on the GPU,
   it waits with vk_lock free, and the slot's recycling then waits for nothing */
static int check_frame_ready(void) {
    VkSemaphore gate;
    vk_lock();
    vk_finish();
    VkBuffer upload;
    u64 at;
    CHECK(vk_stage_upload(16, 16, &upload, &at));
    vk_cmd(); /* something to submit in this slot */
    CHECK(!lw_gate(&gate));
    vk_frame_next(); /* submits it: this slot's last use, held back */
    u64 held = vk_last_submit();
    vk_frame_next(); /* back to it, fresh: its recycling waits for that submission */
    vk_unlock();
    CHECK(!lw_start(vk_frame_ready));
    int waits = !lw_done_within(300);
    int lock_free = lw_lock_free();
    CHECK(!lw_open(gate));
    CHECK(lw_done_within(-1));
    CHECK(waits);
    CHECK(lock_free);
    u64 submitted = 0, done = 0;
    vk_submit_progress(&submitted, &done);
    CHECK(done >= held);
    vk_lock();
    vk_cmd(); /* recycles the slot with its work done */
    vk_finish();
    vk_unlock();
    vkDestroySemaphore(g_vk.dev, gate, NULL);
    puts("frame ready: the wait for the next frame slot holds no vk_lock");
    return 0;
}
