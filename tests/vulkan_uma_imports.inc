/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */

/* How long an import of guest memory stays in the driver (gpu/vk/uma.c) around the GPU's work on it. Each change of its
   pages is followed by a submission: amdgpu takes every import's pages again at each one and refuses all of it on
   pages gone or trapped (EFAULT: the device is lost), and a driver that takes no pages again (lavapipe) shows it in the
   imports held. Without traps (memory_traps=2, or a host with none) a publish lands at once and these skip */
#define UMA_F  (HEAP_BASE + 0x2000000) /* read on the GPU while it is published over */
#define UMA_GF 0x4d2000000ull
#define UMA_W  (HEAP_BASE + 0x2400000) /* two whole windows of imports */
#define UMA_GW 0x4d4000000ull
#define UMA_D  (HEAP_BASE + 0x2a00000) /* an import elsewhere */
#define UMA_GD 0x4d8000000ull
#define UMA_S  (HEAP_BASE + 0x2c00000) /* looked up before a submission, read after it */
#define UMA_GS 0x4d9000000ull

static int uma_skip(void) {
    if (vk_uma() && own_traps()) return 0;
    printf("skip: %s\n", !vk_uma() ? "guest memory is copied on this device (vk_memory)"
                                   : "no traps (memory_traps): a publish lands at once");
    return 1;
}
/* a GPU result of size bytes of value, its writes recorded */
static VkOwnRes* uma_owner(u64 size, u8 value) {
    VkOwnRes* r = vk_own_res_new(size);
    if (r && owned_test_fill(r->buf, 0, (u32)size, value)) {
        vk_own_res_unref(r);
        r = NULL;
    }
    return r;
}
/* guest memory [cpu, cpu + size) as the CPU reads it, the GPU's result landed: all value */
static int uma_landed(u64 cpu, u64 size, u8 value) {
    own_acquire(cpu, size);
    for (u64 i = 0; i < size; i++) CHECK(GP(cpu)[i] == value);
    return 0;
}
/* a copy of 256 bytes at off of b into the readback at at, for a submission of its own */
static void uma_read(VkBuffer b, u64 off, VkBuffer rb, u64 at) {
    VkBufferCopy c = {off, at, 256};
    rec_CmdCopyBuffer(vk_cmd(), b, rb, 1, &c);
}

/* An import goes once the submission reading it is done, one still running included: the read takes the bytes from
   before the publish, and the publish returns after it */
static int check_uma_in_flight(void) {
    const u64 span = 0x100000;
    VkBuffer imp, rb, fa, fb;
    u64 io, ao, bo, submitted, done;
    if (uma_skip()) return 0;
    CHECK(mem_map(UMA_F, span, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GF, UMA_F, span);
    memset(GP(UMA_F), 0x5a, (size_t)span);
    vk_lock();
    u8* result = vk_readback_buffer(span, &rb);
    u8* a = vk_stage_upload(span, 16, &fa, &ao);
    u8* b = vk_stage_upload(span, 16, &fb, &bo);
    CHECK(result && a && b && vk_buf_get_input(UMA_GF, span, 4, &imp, &io) && vk_uma_imports(UMA_F, span) == 1);
    memset(a, 0x11, (size_t)span);
    VkBufferCopy read = {io, 0, span};
    rec_CmdCopyBuffer(vk_cmd(), imp, rb, 1, &read);
    for (u32 k = 0; k < 64; k++) { /* 64 MiB more after it: the submission still runs at the publish */
        VkBufferCopy c = {k & 1 ? bo : ao, k & 1 ? ao : bo, span};
        rec_CmdCopyBuffer(vk_cmd(), k & 1 ? fb : fa, k & 1 ? fa : fb, 1, &c);
    }
    u64 reading = vk_submit_number();
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    vk_submit_progress(&submitted, &done);
    int running = done < reading;
    VkOwnRes* owner = uma_owner(span, 0x42);
    CHECK(owner);
    vk_own_publish(UMA_F, span, owner, 0); /* its trap: the import goes once the read is done */
    vk_submit_progress(&submitted, &done);
    CHECK(done >= reading && vk_uma_imports(UMA_F, span) == 0);
    vk_finish();
    vk_readback_ready();
    for (u64 i = 0; i < span; i++) CHECK(result[i] == 0x5a);
    vk_unlock();
    CHECK(!uma_landed(UMA_F, span, 0x42));
    vk_own_res_unref(owner);
    CHECK(mem_unmap(UMA_F, span) == 0);
    printf("in place: an import goes once the read of it is done (%s at the publish)\n",
           running ? "running" : "done already");
    return 0;
}

/* A publish over the edge between two windows takes both imports from the driver, and an import elsewhere stays the
   same one; a page trapped again after it landed takes the import made since; and a range unmapped under its import,
   with a submission while it is, mapped again and trapped, takes each */
static int check_uma_trap_ranges(void) {
    const u64 window = 0x200000, span = 2 * window, edge = UMA_W + window, page = 0x1000; /* uma.c's UMA_WINDOW */
    VkBuffer wa, wb, d, again, rb;
    u64 oa, ob, od, again_off;
    if (uma_skip()) return 0;
    CHECK(mem_map(UMA_W, span, MS_NORMAL, PERM_RW) == 0 && mem_map(UMA_D, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GW, UMA_W, span);
    gmmu_map(UMA_GD, UMA_D, 0x10000);
    memset(GP(UMA_W), 0x61, (size_t)span);
    memset(GP(UMA_D), 0x62, 0x10000);
    vk_lock();
    u8* result = vk_readback_buffer(512, &rb);
    CHECK(result && vk_buf_get_input(UMA_GW + 0x100, 256, 4, &wa, &oa) &&
          vk_buf_get_input(UMA_GW + window + 0x100, 256, 4, &wb, &ob) && wa != wb &&
          vk_buf_get_input(UMA_GD + 0x100, 256, 4, &d, &od));
    CHECK(vk_uma_imports(UMA_W, window) == 1 && vk_uma_imports(edge, window) == 1 &&
          vk_uma_imports(UMA_D, 0x10000) == 1);
    uma_read(wb, ob, rb, 0);
    VkOwnRes* owner = uma_owner(2 * page, 0x63);
    CHECK(owner);
    vk_own_publish(edge - page, 2 * page, owner, 0); /* a page of each window: both imports go */
    uma_read(d, od, rb, 256);                        /* work of its own for a submission after the trap */
    vk_finish();
    vk_readback_ready();
    CHECK(vk_uma_imports(UMA_W, span) == 0 && vk_uma_imports(UMA_D, 0x10000) == 1);
    for (u32 i = 0; i < 512; i++) CHECK(result[i] == (i < 256 ? 0x61 : 0x62));
    CHECK(vk_buf_get_input(UMA_GD + 0x100, 256, 4, &again, &again_off) && again == d && again_off == od);
    vk_unlock();
    CHECK(!uma_landed(edge - page, 2 * page, 0x63));
    vk_own_res_unref(owner);
    /* the page trapped again under the import a lookup made since (or a copy, where the driver refuses the pool's
       pages it is in now) */
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GW + window - 0x100, 256, 4, &wa, &oa));
    int inplace = vk_uma_imports(UMA_W, window);
    uma_read(wa, oa, rb, 0);
    owner = uma_owner(page, 0x64);
    CHECK(owner);
    vk_own_publish(edge - page, page, owner, 0);
    uma_read(d, od, rb, 256);
    vk_finish();
    vk_readback_ready();
    CHECK(vk_uma_imports(UMA_W, span) == 0);
    for (u32 i = 0; i < 512; i++) CHECK(result[i] == (i < 256 ? 0x63 : 0x62));
    vk_unlock();
    CHECK(!uma_landed(edge - page, page, 0x64));
    vk_own_res_unref(owner);
    /* the other range unmapped under its import, a submission while it is, and mapped again: a new import of the new
       pages, which a trap takes as it took the first */
    CHECK(mem_unmap(UMA_D, 0x10000) == 0);
    vk_lock();
    CHECK(vk_stage_upload(256, 16, &again, &again_off));
    uma_read(again, again_off, rb, 0); /* work of its own for a submission while it is unmapped */
    vk_finish();
    CHECK(vk_uma_imports(UMA_D, 0x10000) == 0);
    vk_unlock();
    CHECK(mem_map(UMA_D, 0x10000, MS_NORMAL, PERM_RW) == 0);
    memset(GP(UMA_D), 0x65, 0x10000);
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GD + 0x100, 256, 4, &d, &od) && vk_uma_imports(UMA_D, 0x10000) == 1);
    uma_read(d, od, rb, 256);
    owner = uma_owner(256, 0x66);
    CHECK(owner);
    vk_own_publish(UMA_D + 0x100, 256, owner, 0);
    vk_finish();
    vk_readback_ready();
    CHECK(vk_uma_imports(UMA_D, 0x10000) == 0);
    for (u32 i = 256; i < 512; i++) CHECK(result[i] == 0x65);
    vk_unlock();
    CHECK(!uma_landed(UMA_D + 0x100, 256, 0x66));
    vk_own_res_unref(owner);
    CHECK(mem_unmap(UMA_W, span) == 0 && mem_unmap(UMA_D, 0x10000) == 0);
    printf("in place: a publish over two imports takes both and one elsewhere stays, a page trapped again takes the "
           "import made since (%s), and a range unmapped under its import, mapped again and trapped takes each\n",
           inplace ? "in place" : "copied: the driver refused the pool's page");
    return 0;
}

/* A lookup whose use is recorded after a submission since the lookup (a draw's setup that waits for a landing, as
   buffer.c's wait_finish and surface.c's flush_wait do) is read by that later submission: a trap of its pages waits
   for that one too, and the import goes after it. Also when the next draw has begun by the trap (vk_frame_check) */
static int check_uma_lookup_then_submit(void) {
    VkBuffer b, rb;
    u64 o;
    if (uma_skip()) return 0;
    CHECK(mem_map(UMA_S, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GS, UMA_S, 0x10000);
    for (int next_draw = 0; next_draw < 2; next_draw++) {
        const u64 at = 0x100 + (u64)next_draw * 0x2000; /* a page the first round's trap left alone */
        memset(GP(UMA_S + at), 0x71 + next_draw, 256);
        vk_lock();
        u8* result = vk_readback_buffer(256, &rb);
        CHECK(result && vk_buf_get_input(UMA_GS + at, 256, 4, &b, &o) && vk_uma_imports(UMA_S, 0x10000) == 1);
        vk_finish();           /* the landing's wait, between the lookup and its use */
        uma_read(b, o, rb, 0); /* the use, in the next submission */
        if (next_draw) vk_frame_check();
        VkOwnRes* owner = uma_owner(256, 0x73);
        CHECK(owner);
        vk_own_publish(UMA_S + at, 256, owner, 0);
        CHECK(vk_uma_imports(UMA_S, 0x10000) == 0);
        vk_finish();
        vk_readback_ready();
        for (u32 i = 0; i < 256; i++) CHECK(result[i] == 0x71 + next_draw);
        vk_unlock();
        CHECK(!uma_landed(UMA_S + at, 256, 0x73));
        vk_own_res_unref(owner);
    }
    CHECK(mem_unmap(UMA_S, 0x10000) == 0);
    printf("in place: a lookup used after a submission since it keeps its import until that use ran, also once the "
           "next draw began\n");
    return 0;
}
