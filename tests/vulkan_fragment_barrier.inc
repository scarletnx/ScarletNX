/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN's fragment barrier (the renderer's barrier kind 0) is skipped only where the device draws with descriptor
   buffers or heaps. A device that has neither draws with descriptor sets and keeps every one, whatever vk_descriptors
   asks for. A clear on each side of the barrier shows which: a kept barrier ends the first clear's pass, so the second
   clear begins another; a skipped one leaves both clears in one pass. */
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
    int setting = g_settings.descriptor_buffers;
    g_settings.descriptor_buffers = 3;
    u64 passes[2];
    for (u32 sets = 0; sets < 2; sets++) {
        vk_lock();
        vk_render_end();
        /* The descriptor-set device: what device.c leaves when the device has neither buffers nor heaps */
        if (sets) g_vk.has_descriptor_buffers = g_vk.has_descriptor_heap_support = 0;
        vk_unlock();
        u64 before = g_vk_passes;
        fragment_barrier_clear(channel, gpu);
        g_renderer->method_barrier(0);
        fragment_barrier_clear(channel, gpu);
        vk_lock();
        vk_render_end();
        passes[sets] = g_vk_passes - before;
        g_vk.has_descriptor_buffers = buffers;
        g_vk.has_descriptor_heap_support = heaps;
        vk_unlock();
    }
    g_settings.descriptor_buffers = setting;
    free(channel);
    if (passes[1] != 2 || passes[0] != (buffers || heaps ? 1u : 2u))
        fprintf(stderr,
                "fragment barrier with vk_descriptors=3: %llu passes on descriptor sets (2 expected), %llu on "
                "this device's %s (%u expected)\n",
                (unsigned long long)passes[1], (unsigned long long)passes[0],
                heaps     ? "heaps"
                : buffers ? "buffers"
                          : "sets",
                buffers || heaps ? 1u : 2u);
    CHECK(passes[1] == 2);
    CHECK(passes[0] == (buffers || heaps ? 1u : 2u));
    return 0;
}
