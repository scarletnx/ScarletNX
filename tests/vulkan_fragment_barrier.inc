/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* NVN's fragment barrier (the renderer's barrier kind 0) and the texture data cache invalidation (kind 2). A clear on
   each side of the barrier shows what it did: a barrier from all work ends the first clear's pass, so the second clear
   begins another, and a barrier in the pass leaves both clears in one pass.

   Each case begins after a wait for idle, with nothing pending: a write that an earlier case or check left, which a
   barrier in the pass only asks to order, is not this case's. With nothing pending, each kind stays in the pass on
   every descriptor form.

   A storage write before the barrier (vk_buf_copy_writable, as a draw binds a storage buffer it writes) takes the way
   this device's memory gives it: a result or page cache write that the order counts, or a write in place in guest
   memory (unified memory, which uma.c orders at the next use of its bytes). Where the order counts the write, a device
   that draws with descriptor sets ends the pass for both kinds, and one with descriptor buffers or heaps for an
   invalidation only. On every way, a draw that reads or writes the bytes after the barrier (a third clear after a
   binding of them) is in another pass than the draw before the barrier: the barrier's own ordered them, or the
   binding's (a barrier in the pass asks the order for the writers after it, vk_buf_order_request).

   A wait for idle just after a barrier from all work, with no command between them, records no second barrier: the
   first orders all that it would. With a clear between them, it records its own. */
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
/* a draw with the storage bindings made since the last one was recorded, as render.c counts it: its writes are
   committed, and it is a draw of its own (g_vk_drawn), which the order tells from the draws after it (vk_lock) */
static void fragment_barrier_drawn(void) {
    vk_buf_writable_commit();
    g_vk_drawn++;
}
/* what a barrier from all work orders that the renderer keeps count of (vk_lock) */
static int fragment_barrier_counted(void) { return vk_buf_order_pending(0) || vk_pages_order_pending(); }
enum { FB_NONE, FB_COUNTED, FB_IN_PLACE };
/* a storage write still pending, as a draw that writes a storage buffer leaves it: FB_COUNTED or FB_IN_PLACE, FB_NONE
   when it could not be made */
static int fragment_barrier_write(u64 stored) {
    vk_lock();
    VkBuffer output;
    u64 at;
    int made = vk_buf_copy_writable(stored, 32, &output, &at, 0);
    if (made) fragment_barrier_drawn();
    int way = !made                        ? FB_NONE
              : fragment_barrier_counted() ? FB_COUNTED
              : vk_uma_pending(stored, 32) ? FB_IN_PLACE
                                           : FB_NONE;
    vk_unlock();
    return way;
}
enum { FB_AFTER_NONE, FB_AFTER_READ, FB_AFTER_WRITE };
/* The passes of two clears around a barrier of kind, from a wait for idle on, after a storage write to stored or not
   (*way). With after, a binding of the written bytes to read or to write after the second clear, and a third clear as
   its draw. *counted: the order counted something at the barrier. ~0 when the case could not be set up */
static u64 fragment_barrier_passes(GpuChannel* channel, u64 target, u64 stored, int kind, int pending, int after,
                                   int* way, int* counted) {
    *way = FB_NONE;
    *counted = 0;
    vk_lock();
    vk_render_end();
    vk_unlock();
    g_renderer->method_barrier(4);
    vk_lock();
    int clean = !fragment_barrier_counted();
    vk_unlock();
    if (!clean || (pending && !(*way = fragment_barrier_write(stored)))) return ~0ull;
    u64 before = g_vk_passes;
    fragment_barrier_clear(channel, target);
    vk_lock();
    *counted = fragment_barrier_counted();
    vk_unlock();
    g_renderer->method_barrier(kind);
    fragment_barrier_clear(channel, target);
    int bound = 1;
    if (after) {
        VkBuffer buffer;
        u64 at;
        vk_lock();
        bound = after == FB_AFTER_READ ? vk_buf_get_strict(stored, 32, &buffer, &at)
                                       : vk_buf_copy_writable(stored, 32, &buffer, &at, 0);
        vk_unlock();
        fragment_barrier_clear(channel, target);
        vk_lock();
        if (after == FB_AFTER_WRITE && bound)
            fragment_barrier_drawn();
        else if (after == FB_AFTER_WRITE)
            vk_buf_writable_abort();
        vk_unlock();
    }
    vk_lock();
    vk_render_end();
    vk_unlock();
    return bound ? g_vk_passes - before : ~0ull;
}
static int check_fragment_barrier_backend(void) {
    const u64 gpu = 0x8c0000000ull, cpu = HEAP_BASE + 0x11900000, stored = gpu + 0x8000;
    static const char* const ways[3] = {"nothing pending", "a storage write the order counts",
                                        "a storage write in place in guest memory"};
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
        const char* form = sets ? "descriptor sets" : "this device's descriptors";
        static const int kinds[2] = {0, 2};
        for (u32 k = 0; k < 2; k++)
            for (int pending = 0; pending < 2; pending++) {
                int way, counted;
                u64 got =
                    fragment_barrier_passes(channel, gpu, stored, kinds[k], pending, FB_AFTER_NONE, &way, &counted);
                u64 want = counted && !(kinds[k] == 0 && in_heaps) ? 2 : 1;
                if (got != want) {
                    fprintf(stderr,
                            "barrier kind %d on %s, %s, %s at the barrier: %llu passes (%llu expected; ~0: the case "
                            "could not be set up)\n",
                            kinds[k], form, pending ? ways[way] : ways[FB_NONE],
                            counted ? "something counted" : "nothing counted", (unsigned long long)got,
                            (unsigned long long)want);
                    failed = 1;
                }
                if (!pending) continue;
                /* a draw after the barrier that reads or writes the bytes: in another pass than the one before it */
                for (int after = FB_AFTER_READ; after <= FB_AFTER_WRITE; after++) {
                    got = fragment_barrier_passes(channel, gpu, stored, kinds[k], 1, after, &way, &counted);
                    if (got < 2 || got == ~0ull) {
                        fprintf(stderr,
                                "barrier kind %d on %s, %s: a draw %s its bytes after the barrier had %llu passes "
                                "with the draw before it (2 or more expected; ~0: the case could not be set up)\n",
                                kinds[k], form, ways[way], after == FB_AFTER_READ ? "reading" : "writing",
                                (unsigned long long)got);
                        failed = 1;
                    }
                }
            }
        if (sets) {
            vk_lock();
            vk_render_end();
            vk_unlock();
            fragment_barrier_clear(channel, gpu);
            g_renderer->method_barrier(4);
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
                "wait for idle just after a barrier from all work on descriptor sets: %llu merged (1 expected), %llu "
                "after a clear (0 expected)\n",
                (unsigned long long)merged, (unsigned long long)kept);
    CHECK(merged == 1 && kept == 0);
    CHECK(!failed);
    return 0;
}
