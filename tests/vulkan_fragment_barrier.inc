/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN's fragment barrier (the renderer's barrier kind 0) and the texture data cache invalidation (kind 2). A clear on
   each side of the barrier shows what it did: a barrier from all work ends the first clear's pass, so the second clear
   begins another, and a barrier in the pass leaves both clears in one pass.

   With nothing before it that the renderer does not order where it is bound, each kind stays in the pass on every
   descriptor form. After a storage write that is still pending, a device that draws with descriptor sets ends the pass
   for both kinds. One with descriptor buffers or heaps keeps a fragment barrier in the pass as before, and ends it for
   an invalidation. A wait for idle (kind 4) just after a barrier from all work, with no command between them, records
   no second barrier: the first orders all that it would. With a clear between them, it records its own. */
static void fragment_barrier_clear(GpuChannel* channel, u64 target) {
    u32* r = channel->r3d;
    const float color[4] = {0.25f, 0.5f, 0.75f, 1};
    r[0x487] = 1;
    r[0x200] = (u32)(target >> 32);
    r[0x201] = (u32)target;
    r[0x202] = r[0x203] = 4;
    r[0x204] = 0xD5;
    r[0x205] = 0;
    r[0x206] = 1;
    r[0x207] = 0;
    memcpy(&r[0x360], color, sizeof color);
    engine_clear(channel, 15 << 2);
}
/* a storage write still pending, as a draw that writes a storage buffer leaves it: 0 when it could not be made */
static int fragment_barrier_write(u64 stored) {
    vk_lock();
    VkBuffer output;
    u64 at;
    int made = vk_buf_copy_writable(stored, 32, &output, &at, 0);
    if (made) vk_buf_writable_commit();
    made = made && vk_buf_order_pending(1);
    vk_unlock();
    return made;
}
/* the passes that two clears around a barrier of kind take, with a storage write pending before it or not. ~0 when
   the write was not pending at the barrier */
static u64 fragment_barrier_passes(GpuChannel* channel, u64 target, u64 stored, int kind, int pending) {
    vk_lock();
    vk_render_end();
    vk_unlock();
    if (pending && !fragment_barrier_write(stored)) return ~0ull;
    u64 before = g_vk_passes;
    fragment_barrier_clear(channel, target);
    vk_lock();
    int still = !pending || vk_buf_order_pending(1);
    vk_unlock();
    g_renderer->method_barrier(kind);
    fragment_barrier_clear(channel, target);
    vk_lock();
    vk_render_end();
    vk_unlock();
    return still ? g_vk_passes - before : ~0ull;
}
static int check_fragment_barrier_backend(void) {
    const u64 gpu = 0x8c0000000ull, cpu = HEAP_BASE + 0x11900000, stored = gpu + 0x8000;
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 152;
    int buffers = g_vk.has_descriptor_buffers, heaps = g_vk.has_descriptor_heap_support;
    int setting = g_settings.descriptor_buffers;
    g_settings.descriptor_buffers = 3;
    int failed = 0;
    u64 merged = 0, kept = 0;
    for (u32 sets = 0; sets < 2; sets++) {
        /* The descriptor-set device: what device.c leaves when the device has neither buffers nor heaps */
        vk_lock();
        if (sets) g_vk.has_descriptor_buffers = g_vk.has_descriptor_heap_support = 0;
        int in_heaps = g_vk.has_descriptor_buffers || g_vk.has_descriptor_heap_support;
        vk_unlock();
        static const int kinds[2] = {0, 2};
        for (u32 k = 0; k < 2; k++)
            for (int pending = 0; pending < 2; pending++) {
                u64 want = pending && !(kinds[k] == 0 && in_heaps) ? 2 : 1;
                u64 got = fragment_barrier_passes(channel, gpu, stored, kinds[k], pending);
                if (got != want) {
                    fprintf(stderr,
                            "barrier kind %d on %s, %s: %llu passes (%llu expected; ~0: the write was not pending)\n",
                            kinds[k], sets ? "descriptor sets" : "this device's descriptors",
                            pending ? "a storage write pending" : "nothing pending", (unsigned long long)got,
                            (unsigned long long)want);
                    failed = 1;
                }
            }
        if (sets) {
            vk_lock();
            vk_render_end();
            vk_unlock();
            if (!fragment_barrier_write(stored)) failed = 1;
            fragment_barrier_clear(channel, gpu);
            g_renderer->method_barrier(0);
            merged = g_vk_barriers_merged;
            g_renderer->method_barrier(4);
            merged = g_vk_barriers_merged - merged;
            fragment_barrier_clear(channel, gpu);
            kept = g_vk_barriers_merged;
            g_renderer->method_barrier(4);
            kept = g_vk_barriers_merged - kept;
        }
        vk_lock();
        vk_render_end();
        g_vk.has_descriptor_buffers = buffers;
        g_vk.has_descriptor_heap_support = heaps;
        vk_unlock();
    }
    g_settings.descriptor_buffers = setting;
    free(channel);
    if (merged != 1 || kept != 0)
        fprintf(stderr,
                "wait for idle after a fragment barrier on descriptor sets: %llu merged (1 expected), %llu after a "
                "clear (0 expected)\n",
                (unsigned long long)merged, (unsigned long long)kept);
    CHECK(merged == 1 && kept == 0);
    CHECK(!failed);
    return 0;
}
