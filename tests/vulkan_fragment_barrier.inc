/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN's fragment barrier (the renderer's barrier kind 0) and the texture data cache invalidation (kind 2) keep the
   pass open on every device: the renderer's tracking of targets and buffers orders what they order, with descriptor
   sets as with descriptor buffers or heaps. A clear on each side of the barrier shows it: an ended pass makes the
   second clear begin another; a kept one leaves both clears in one pass. A wait for idle (kind 4) still ends it. */
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
static int check_fragment_barrier_backend(void) {
    const u64 gpu = 0x8c0000000ull, cpu = HEAP_BASE + 0x11900000;
    CHECK(!mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW));
    gmmu_map(gpu, cpu, 0x10000);
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 152;
    int buffers = g_vk.has_descriptor_buffers, heaps = g_vk.has_descriptor_heap_support;
    static const int kinds[3] = {0, 2, 4};
    static const u64 want[3] = {1, 1, 2};
    int failed = 0;
    for (u32 sets = 0; sets < 2; sets++)
        for (u32 k = 0; k < 3; k++) {
            vk_lock();
            vk_render_end();
            /* the descriptor-set device: what device.c leaves when the device has neither buffers nor heaps */
            if (sets) g_vk.has_descriptor_buffers = g_vk.has_descriptor_heap_support = 0;
            vk_unlock();
            u64 before = g_vk_passes;
            fragment_barrier_clear(channel, gpu);
            g_renderer->method_barrier(kinds[k]);
            fragment_barrier_clear(channel, gpu);
            vk_lock();
            vk_render_end();
            u64 passes = g_vk_passes - before;
            g_vk.has_descriptor_buffers = buffers;
            g_vk.has_descriptor_heap_support = heaps;
            vk_unlock();
            if (passes != want[k]) {
                fprintf(stderr, "barrier kind %d on %s: %llu passes (%llu expected)\n", kinds[k],
                        sets ? "descriptor sets" : "this device's descriptors", (unsigned long long)passes,
                        (unsigned long long)want[k]);
                failed = 1;
            }
        }
    free(channel);
    CHECK(!failed);
    return 0;
}
