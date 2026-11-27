/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A big range a draw binds over GPU-owned bytes that no one owner holds whole (buffer.c ocopy_get): copied on the GPU
   into a buffer of its own, which a later submission binds again with nothing read, until an owner is written or the
   CPU writes the range's other pages. Copied into the frame's ring at each submission instead, one game's 64 MiB range
   took 95% of the GPU's time */

#define OKC_CPU  (HEAP_BASE + 0x3f800000)
#define OKC_GVA  0xe20000000ull
#define OKC_SIZE 0x100000u

/* the range's bytes as the draws read them: the CPU's cpu, the first owner's first, the second's second */
static int okc_expect(VkBuffer b, u64 off, u8 cpu, u8 first, u8 second, u32 cpu_at, u8 cpu_written, const char* what) {
    u8* got = malloc(OKC_SIZE);
    CHECK(got);
    CHECK(!buffer_read(b, off, OKC_SIZE, got));
    for (u32 i = 0; i < OKC_SIZE; i++) {
        const u8 want = i >= 0x10000 && i < 0x20000   ? first
                      : i >= 0x80000 && i < 0x90000   ? second
                      : i >= cpu_at && i < cpu_at + 0x1000 ? cpu_written
                                                      : cpu;
        if (got[i] != want) {
            fprintf(stderr, "owned range kept, %s: byte %#x is %#x, not %#x\n", what, i, got[i], want);
            free(got);
            return 1;
        }
    }
    free(got);
    return 0;
}

static int check_owned_kept_copy(void) {
    if (!own_traps()) {
        puts("skip: owned range kept: no traps to own guest memory by");
        return 0;
    }
    CHECK(mem_map(OKC_CPU, OKC_SIZE, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(OKC_GVA, OKC_CPU, OKC_SIZE);
    memset(GP(OKC_CPU), 0x11, OKC_SIZE);
    g_renderer->mem_may_have_changed();
    vk_lock();
    vk_finish();
    VkOwnRes* first = vk_own_res_new(0x10000, VK_OWN_BY_DRAW);
    VkOwnRes* second = vk_own_res_new(0x10000, VK_OWN_BY_DRAW);
    CHECK(first && second);
    CHECK(!owned_test_fill(first->buf, 0, 0x10000, 0x5a) && !owned_test_fill(second->buf, 0, 0x10000, 0x6b));
    vk_own_publish_owned(OKC_CPU + 0x10000, 0x10000, first, 0);
    vk_own_publish_owned(OKC_CPU + 0x80000, 0x10000, second, 0);
    CHECK(own_lookup(OKC_CPU, OKC_SIZE, NULL, 0) == 2);

    VkBuffer b, again;
    u64 off, again_off;
    CHECK(vk_buf_get(OKC_GVA, OKC_SIZE, &b, &off));
    CHECK(!okc_expect(b, off, 0x11, 0x5a, 0x6b, 0, 0x11, "made"));

    /* a later submission, nothing written: the same buffer, and nothing read from guest memory or the owners */
    u64 was[2][VK_GREAD_N], now[2][VK_GREAD_N];
    vk_finish();
    vk_guest_read_counts(was);
    CHECK(vk_buf_get(OKC_GVA, OKC_SIZE, &again, &again_off));
    vk_guest_read_counts(now);
    const u64 read = now[1][VK_GREAD_CPU] - was[1][VK_GREAD_CPU] + now[1][VK_GREAD_OWNERS] - was[1][VK_GREAD_OWNERS];
    if (again != b || again_off != off || read)
        fprintf(stderr, "owned range kept, a later submission: %s buffer, %llu bytes read again\n",
                again != b || again_off != off ? "another" : "the same", (unsigned long long)read);
    CHECK(again == b && again_off == off && !read);

    /* an owner written, with no GPU-side write stamped on its pages: its next use in a later submission copies the
       range again, with its new bytes, by the owner's write serial (owners_sig) */
    CHECK(!owned_test_fill(first->buf, 0, 0x10000, 0x7c));
    vk_own_res_wrote(first);
    vk_finish();
    CHECK(vk_buf_get(OKC_GVA, OKC_SIZE, &b, &off));
    CHECK(!okc_expect(b, off, 0x11, 0x7c, 0x6b, 0, 0x11, "an owner written"));

    /* the CPU's write to a page no owner is on, where writes are tracked */
    const int tracked = vk_written_since(GP(OKC_CPU + 0x40000), 0x1000, 0) >= 0;
    if (tracked) {
        vk_unlock();
        memset(GP(OKC_CPU + 0x40000), 0x22, 0x1000);
        g_renderer->mem_may_have_changed();
        vk_lock();
        CHECK(vk_buf_get(OKC_GVA, OKC_SIZE, &b, &off));
        CHECK(!okc_expect(b, off, 0x11, 0x7c, 0x6b, 0x40000, 0x22, "the CPU wrote a page"));
    }
    own_acquire(OKC_CPU, OKC_SIZE);
    vk_own_res_unref(first);
    vk_own_res_unref(second);
    vk_finish();
    vk_unlock();
    printf("owned range kept: a range over two owners bound again with nothing read, copied again after an owner's "
           "write%s\n",
           tracked ? " and the CPU's" : " (no write tracking for the CPU's)");
    return 0;
}
