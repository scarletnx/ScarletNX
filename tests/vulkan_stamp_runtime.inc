/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game that sizes its frame by its GPU timer reads the span between two timestamp reports, and the reports leave out
   the runtime's own work on the graphics queue (stamp.c): what the console's GPU does not do, as it samples and draws
   guest memory where it is. Each of these, done between two reports, is a stretch left out, and none loses its mark:
   a texture's upload and a drawn target taken into a texture. A racing game whose memory ran short read those as its own GPU time, lowered its render
   size, made every target again at the new size each frame, and never got out of it */
#define STAMP_RT_GVA 0xCD0000000ull
#define STAMP_RT_CPU (HEAP_BASE + 0x40C00000)

/* the stretches marked so far */
static u64 stamp_rt_marks(void) {
    u64 marks, lost, ns;
    vk_stamp_runtime_stats(&marks, &lost, &ns);
    return marks;
}
/* a 16 by 16 block linear RGBA8 header at gva, blocks one GOB high, of w by 16 texels and `levels` levels */
static void stamp_rt_tic(u32* tic, u64 gva, u32 w, u32 levels) {
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    const u32 words[8] = {
        rgba8, (u32)gva, (u32)(gva >> 32) | 3u << 21, (levels - 1) << 28, (w - 1) | 1u << 23, 15, 0, (levels - 1) << 4};
    memcpy(tic, words, sizeof words);
}

static int check_stamp_runtime(void) {
    const u64 up = STAMP_RT_GVA, take = STAMP_RT_GVA + 0x100000;
    u64 marks, lost, ns, before;
    u32 tic[8];
    if (!g_vk.gfx_timestamp_bits || !g_vk.has_host_query_reset) {
        printf("runtime work in report spans: not checked, the graphics queue has no timestamps (%u valid bits) or "
               "the device no host query reset (%d)\n",
               g_vk.gfx_timestamp_bits, g_vk.has_host_query_reset);
        return 0;
    }
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    CHECK(mem_map(STAMP_RT_CPU, 0x400000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(STAMP_RT_GVA, STAMP_RT_CPU, 0x400000);
    /* the first report starts the timeline: the command buffers from the next one on have timestamps */
    vk_lock();
    const u64 first = vk_stamp_report(1);
    vk_finish();
    vk_unlock();
    (void)first;
    vk_stamp_runtime_stats(&marks, &lost, &ns);
    const u64 lost_before = lost, ns_before = ns;
    /* a texture's upload */
    stamp_rt_tic(tic, up, 16, 1);
    vk_lock();
    before = stamp_rt_marks();
    Surf* loaded = vk_tex_from_tic(tic);
    const u64 by_upload = stamp_rt_marks() - before;
    vk_unlock();
    CHECK(loaded && !loaded->is_rt && by_upload >= 1);
    /* a drawn target taken into a texture of two levels: the texture is made (an upload and a take), then the target
       is cleared again and the texture takes it again, a take alone */
    stamp_rt_tic(tic, take, 16, 2);
    rt_clear(ch, take, 16, 16, 0, 0);
    vk_lock();
    Surf* taker = vk_tex_from_tic(tic);
    vk_unlock();
    rt_clear(ch, take, 16, 16, 0, 1);
    vk_lock();
    before = stamp_rt_marks();
    const int again = vk_tex_from_tic(tic) == taker;
    const u64 by_take = stamp_rt_marks() - before;
    /* where the target draws into the texture's own image (docs/dev/one-image.md) there is nothing to copy */
    const Surf* drawn = vk_rt_find(take);
    const int hosted = drawn && drawn->host == taker;
    vk_unlock();
    CHECK(taker && !taker->is_rt && again && (by_take >= 1 || hosted));
    /* the second report ends the span; no stretch lost its mark */
    vk_lock();
    const u64 last = vk_stamp_report(0);
    vk_finish();
    vk_unlock();
    (void)last;
    vk_stamp_runtime_stats(&marks, &lost, &ns);
    CHECK(lost == lost_before);
    printf("runtime work in report spans: stretches left out between two reports: an upload %llu",
           (unsigned long long)by_upload);
    printf(", a take %llu", (unsigned long long)by_take);
    printf("; %.3f ms of GPU time left out, no mark lost\n", (double)(ns - ns_before) / 1e6);
    free(ch);
    return 0;
}
