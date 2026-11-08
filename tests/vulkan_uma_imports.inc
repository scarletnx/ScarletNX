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
    vk_own_publish_owned(UMA_F, span, owner, 0); /* its trap: the import goes once the read is done */
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
    vk_own_publish_owned(edge - page, 2 * page, owner, 0); /* a page of each window: both imports go */
    uma_read(d, od, rb, 256);                              /* work of its own for a submission after the trap */
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
    vk_own_publish_owned(edge - page, page, owner, 0);
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
    vk_own_publish_owned(UMA_D + 0x100, 256, owner, 0);
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
        vk_own_publish_owned(UMA_S + at, 256, owner, 0);
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

/* A write of the runtime's own (the front end's inline write to memory, gmem_write) to a page a recorded, unsubmitted
   command reads in place waits for that command: the GPU reads the bytes from before the write, as the console's GPU
   ran it before the write. The page is copied from then on (the runtime wrote it), so a next write there waits for
   nothing, and a guest CPU write elsewhere in the import still reaches the GPU in place */
#define UMA_T  (HEAP_BASE + 0x2e00000)
#define UMA_GT 0x4da000000ull
static int check_uma_runtime_write(void) {
    VkBuffer b, b2, rb;
    u64 o, o2;
    u8 bytes[256];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_T, 0x200000, MS_NORMAL, PERM_RW) == 0); /* a whole window: one import for every page of it */
    gmmu_map(UMA_GT, UMA_T, 0x200000);
    memset(GP(UMA_T), 0x81, 0x10000);
    vk_lock();
    u8* result = vk_readback_buffer(768, &rb);
    CHECK(result && vk_buf_get_input(UMA_GT + 0x100, 256, 4, &b, &o) && vk_uma_imports(UMA_T, 0x10000) == 1);
    uma_read(b, o, rb, 0); /* recorded, not submitted */
    vk_unlock();
    memset(bytes, 0x82, sizeof bytes);
    CHECK(!gmem_write(UMA_GT + 0x100, bytes, sizeof bytes)); /* the runtime's write: after the copy reads */
    vk_lock();
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 256; i++) CHECK(result[i] == 0x81);
    /* the written page copied from now on, with the runtime's bytes; a page beside it still read in place */
    CHECK(vk_buf_get_input(UMA_GT + 0x100, 256, 4, &b2, &o2) && b2 != b);
    uma_read(b2, o2, rb, 256);
    CHECK(vk_buf_get_input(UMA_GT + 0x2100, 256, 4, &b2, &o2) && b2 == b);
    memset(GP(UMA_T) + 0x2100, 0x83, 256); /* the guest CPU's, after the binding: in place, the GPU reads it */
    uma_read(b2, o2, rb, 512);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 256; i < 768; i++) CHECK(result[i] == (i < 512 ? 0x82 : 0x83));
    vk_unlock();
    CHECK(mem_unmap(UMA_T, 0x200000) == 0);
    printf("in place: a runtime write to a page an unsubmitted command reads waits for it, and the page is copied "
           "from then on\n");
    return 0;
}

/* A constant buffer is read in place, as vertices are, but the pages the runtime wrote (front, copied as before): a
   vertex program takes the green of its color from c[0][0x644], 1.0 then 0.0 written by the guest's CPU, then 1.0 by
   the runtime (gmem_write) */
#define UMA_C  (HEAP_BASE + 0x3200000) /* a window of its own: the constant buffer */
#define UMA_GC 0x4db000000ull
static int check_uma_constants(void) {
    const u64 address = 0x4dc000000ull, cpu = HEAP_BASE + 0x740000;
    const u64 stream = address + 0x12000, target = address + 0x18000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0x4c98078019170001ull, /* MOV R1, c[0x0][0x644] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1}, red[4] = {1, 0, 0, 1};
    const float one = 1.0f, zero = 0.0f;
    u64 fragment[5];
    u8 bytes[96];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 125;
    CHECK(mem_map(UMA_C, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GC, UMA_C, 0x200000);
    memset(GP(UMA_C), 0, 0x1000);
    memcpy(GP(UMA_C) + 0x644, &one, 4);
    gmmu_map(address, cpu, 0x20000);
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(address, vertex, 6, 0));
    CHECK(mesh_program(address + 0x10000, fragment, 5, 1));
    for (u32 v = 0; v < 3; v++) {
        memcpy(bytes + v * 32, position + 4 * v, 16);
        memcpy(bytes + v * 32 + 16, red, 16);
    }
    CHECK(!gmem_write(stream, bytes, sizeof bytes));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 0 | 0u << 7 | 1u << 21 | 7u << 27;  /* stream 0 at 0: RGBA32F */
    r[0x459] = 0 | 16u << 7 | 1u << 21 | 7u << 27; /* stream 0 at 16: RGBA32F */
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof bytes - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof bytes - 1);
    channel->ds.bind.cb_addr[0][0] = UMA_GC;
    channel->ds.bind.cb_size[0][0] = 0x1000;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){255, 255, 0, 255}));
    int inplace = vk_uma_imports(UMA_C, 0x1000);
    memcpy(GP(UMA_C) + 0x644, &zero, 4); /* the guest CPU's write */
    g_renderer->mem_may_have_changed();
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){255, 0, 0, 255}));
    CHECK(!gmem_write(UMA_GC + 0x644, &one, 4)); /* the runtime's: copied from now on */
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){255, 255, 0, 255}));
    channel->ds.bind.cb_addr[0][0] = 0;
    channel->ds.bind.cb_size[0][0] = 0;
    free(channel);
    CHECK(mem_unmap(UMA_C, 0x200000) == 0);
    if (!inplace) fprintf(stderr, "constant buffer: copied, not read in place\n");
    CHECK(inplace);
    printf("in place: a constant buffer read where the guest's CPU wrote it, and copied once the runtime wrote it\n");
    return 0;
}

/* Storage the GPU writes is written in place (vk_buf_copy_writable): the range bound for writing is the import of its
   guest memory, with no result to land; a later reader on the GPU reads the write in place, after a barrier, and so
   does one that copied the range before the write; and the runtime's CPU reader waits for the submit */
#define UMA_V  (HEAP_BASE + 0x3400000)
#define UMA_GV 0x4dd000000ull
static int check_uma_writes(void) {
    VkBuffer w, r, c, rb;
    u64 wo, ro, co;
    u8 bytes[256];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_V, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GV, UMA_V, 0x200000);
    memset(GP(UMA_V), 0x91, 0x1000);
    vk_lock();
    u8* result = vk_readback_buffer(512, &rb);
    CHECK(result && vk_buf_get(UMA_GV + 0x100, 256, &c, &co)); /* a vertex fetch's copy, before the write */
    CHECK(vk_buf_copy_writable(UMA_GV + 0x100, 256, &w, &wo, 1));
    int inplace = vk_uma_imports(UMA_V, 0x200000) == 1 && !vk_buf_pending(UMA_GV + 0x100, 256);
    CHECK(!owned_test_fill(w, wo, 256, 0x92)); /* the shader's write */
    vk_buf_writable_commit();
    CHECK(vk_buf_get_input(UMA_GV + 0x100, 256, 4, &r, &ro)); /* a later draw's read of it */
    uma_read(r, ro, rb, 0);
    CHECK(vk_buf_get(UMA_GV + 0x100, 256, &c, &co)); /* the same fetch after it: not the copy from before */
    uma_read(c, co, rb, 256);
    vk_unlock();
    CHECK(!gmem_read(UMA_GV + 0x100, bytes, sizeof bytes)); /* the runtime's CPU read: after the write */
    for (u32 i = 0; i < 256; i++) CHECK(bytes[i] == 0x92);
    vk_lock();
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 512; i++) CHECK(result[i] == 0x92);
    vk_unlock();
    CHECK(GP(UMA_V)[0x100] == 0x92 && GP(UMA_V)[0x1ff] == 0x92 && GP(UMA_V)[0xff] == 0x91 && GP(UMA_V)[0x200] == 0x91);
    CHECK(mem_unmap(UMA_V, 0x200000) == 0);
    if (!inplace) fprintf(stderr, "storage write: copied out and landed, not written in place\n");
    CHECK(inplace);
    printf("in place: storage written where it is in guest memory, read after the write on the GPU and the CPU\n");
    return 0;
}

/* Transform feedback is written in place too (vk_buf_copy_writable): a draw captures its three positions into guest
   memory where it is, the window imported, and the runtime's CPU reads them */
#define UMA_X  (HEAP_BASE + 0x3a00000)
#define UMA_GX 0x4e2000000ull
static int check_uma_feedback(void) {
    const u64 address = 0x4e3000000ull, cpu = HEAP_BASE + 0x740000;
    const u64 stream = address + 0x12000, target = address + 0x18000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    /* z as w: the translator's depth range fix-up ((z + w) / 2, from the guest's -1 to 1) keeps it, which the capture
       takes after it */
    const float position[12] = {-1, -1, 1, 1, 3, -1, 1, 1, -1, 3, 1, 1}, green[4] = {0, 1, 0, 1};
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    u64 fragment[5];
    u8 bytes[96], captured[48];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    if (!g_vk.has_xfb) {
        printf("skip: the device has no transform feedback (VK_EXT_transform_feedback)\n");
        return 0;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    channel->id = 124;
    CHECK(mem_map(UMA_X, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GX, UMA_X, 0x200000);
    memset(GP(UMA_X), 0x77, 0x1000);
    gmmu_map(address, cpu, 0x20000);
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    CHECK(mesh_program(address, vertex, 5, 0));
    CHECK(mesh_program(address + 0x10000, fragment, 5, 1));
    for (u32 v = 0; v < 3; v++) {
        memcpy(bytes + v * 32, position + 4 * v, 16);
        memcpy(bytes + v * 32 + 16, green, 16);
    }
    CHECK(!gmem_write(stream, bytes, sizeof bytes));
    u32* r = channel->r3d;
    r[0x582] = (u32)(address >> 32);
    r[0x583] = (u32)address;
    r[0x850] = 1;
    r[0x851] = 0x10000;
    r[0x64b] = 1;
    r[0x286] = 0x6420;
    r[0x3fd] = r[0x3fe] = 4u << 16;
    r[0x680] = 0x1111;
    r[MAXWELL3D_RASTERIZE_ENABLE] = 1;
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 0 | 0u << 7 | 1u << 21 | 7u << 27;  /* stream 0 at 0: RGBA32F */
    r[0x459] = 0 | 16u << 7 | 1u << 21 | 7u << 27; /* stream 0 at 16: RGBA32F */
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof bytes - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof bytes - 1);
    /* buffer 0 captures the position's four words (attribute 0x70, words 0x1c to 0x1f), 16 bytes a vertex */
    r[0x1D1] = 1;
    r[0xE0] = 1;
    r[0xE1] = (u32)((UMA_GX + 0x100) >> 32);
    r[0xE2] = (u32)(UMA_GX + 0x100);
    r[0xE3] = sizeof captured;
    r[0x1C1] = 4;
    r[0x1C2] = 16;
    r[0xA00] = 0x1f1e1d1c;
    CHECK(!mesh_pixel(channel, target, 0, (const u8[4]){0, 255, 0, 255}));
    int inplace = vk_uma_imports(UMA_X, 0x200000) == 1;
    CHECK(!gmem_read(UMA_GX + 0x100, captured, sizeof captured));
    free(channel);
    CHECK(GP(UMA_X)[0xff] == 0x77 && GP(UMA_X)[0x130] == 0x77);
    CHECK(mem_unmap(UMA_X, 0x200000) == 0);
    if (memcmp(captured, position, sizeof captured)) {
        float got[12];
        memcpy(got, captured, sizeof got);
        fprintf(stderr, "feedback: captured %g,%g,%g,%g %g,%g,%g,%g %g,%g,%g,%g\n", got[0], got[1], got[2], got[3],
                got[4], got[5], got[6], got[7], got[8], got[9], got[10], got[11]);
    }
    CHECK(!memcmp(captured, position, sizeof captured));
    if (!inplace) fprintf(stderr, "feedback: copied out and landed, not written in place\n");
    CHECK(inplace);
    printf("in place: transform feedback captured where it is in guest memory\n");
    return 0;
}

/* An engine's result (the copy engine's here) over memory the GPU reads in place, which an import holds, is published
   in place: copied there on the GPU in stream order, not GPU-owned or trapped, the import kept, and a CPU read after it
   waits for the submit. Over memory no import holds it stays GPU-owned where the host traps, and no import is made
   for it (one would take its pages from write tracking) */
#define UMA_P  (HEAP_BASE + 0x3600000)
#define UMA_GP 0x4de000000ull
#define UMA_N  (HEAP_BASE + 0x3c00000) /* no import over it */
#define UMA_GN 0x4e4000000ull
static int uma_dma(u64 dst, u64 src) {
    GpuDmaOp o = {.width = 256, .line_in = 256, .line_out = 256, .lines = 1};
    o.src = pitch_side(src, 0);
    o.dst = pitch_side(dst, 0);
    return g_renderer->dma_copy(&o) ? 0 : 1;
}
static int check_uma_publish(void) {
    const u64 src = 0x4df000000ull, src_cpu = HEAP_BASE + 0x700000; /* in the first heap mapping */
    u8 bytes[256];
    VkBuffer r;
    u64 ro;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_P, 0x200000, MS_NORMAL, PERM_RW) == 0 && mem_map(UMA_N, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GP, UMA_P, 0x200000);
    gmmu_map(UMA_GN, UMA_N, 0x200000);
    gmmu_map(src, src_cpu, 0x10000);
    memset(GP(UMA_P), 0xa0, 0x1000);
    memset(GP(UMA_N), 0xa0, 0x1000);
    memset(GP(src_cpu), 0xa1, 0x1000);
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GP + 0x100, 256, 4, &r, &ro)); /* a draw reads it in place: an import holds it */
    int held = vk_uma_held(UMA_P + 0x100, 256);
    vk_unlock();
    u64 lands = own_landings();
    CHECK(!uma_dma(UMA_GP + 0x100, src + 0x100));
    int owned = own_lookup(UMA_P + 0x100, 256, NULL, 0) != 0, imported = vk_uma_imports(UMA_P, 0x200000) != 0;
    CHECK(!gmem_read(UMA_GP + 0x100, bytes, sizeof bytes));
    for (u32 i = 0; i < 256; i++) CHECK(bytes[i] == 0xa1);
    u64 landed = own_landings() - lands; /* in place: none */
    /* over memory nothing imported */
    CHECK(!uma_dma(UMA_GN + 0x100, src + 0x100));
    int owned_n = own_lookup(UMA_N + 0x100, 256, NULL, 0) != 0, imported_n = vk_uma_imports(UMA_N, 0x200000) != 0;
    CHECK(!gmem_read(UMA_GN + 0x100, bytes, sizeof bytes));
    for (u32 i = 0; i < 256; i++) CHECK(bytes[i] == 0xa1);
    vk_lock();
    vk_finish();
    vk_unlock();
    CHECK(GP(UMA_P)[0x100] == 0xa1 && GP(UMA_P)[0x1ff] == 0xa1 && GP(UMA_P)[0x200] == 0xa0);
    CHECK(GP(UMA_N)[0x100] == 0xa1 && GP(UMA_N)[0x1ff] == 0xa1 && GP(UMA_N)[0x200] == 0xa0);
    CHECK(mem_unmap(UMA_P, 0x200000) == 0 && mem_unmap(UMA_N, 0x200000) == 0);
    if (!held || owned || !imported || landed || owned_n != own_traps() || imported_n)
        fprintf(
            stderr,
            "publish: over an import (held %d): owned %d, imported %d, %llu landings; elsewhere: owned %d, imported "
            "%d\n",
            held, owned, imported, (unsigned long long)landed, owned_n, imported_n);
    CHECK(held && !owned && imported && !landed);
    CHECK(owned_n == own_traps() && !imported_n);
    printf("in place: an engine's result over an import copied there on the GPU, nothing GPU-owned; elsewhere owned "
           "(traps %s), no import made for it\n",
           own_traps() ? "on" : "off");
    return 0;
}

/* A dispatch runs on the graphics queue and writes its storage in place: a compute program stores 0x5eed through an NVN
   descriptor in cbuf 0; no compute result is pending after it, the range is imported, and the CPU reads the store */
#define UMA_Q  (HEAP_BASE + 0x3800000)
#define UMA_GQ 0x4e3000000ull
static int check_uma_dispatch(void) {
    const u64 code = 0x4e1000000ull, code_cpu = HEAP_BASE + 0x780000;
    const u64 cbuf = UMA_GQ + 0x1000, stored = UMA_GQ + 0x2000;
    const u64 program[] = {
        0x001f8000fc0007e0ull,  0x4c98078004470000ull, /* MOV R0, c[0][0x110]: the buffer's address, NVN's descriptor */
        0x4c98078004570001ull,                         /* MOV R1, c[0][0x114] */
        storage_mov(2, 0x5eed),                        /* MOV32I R2, 0x5eed */
        0x001f8000fc0007e0ull,  0xeedc200000070002ull, /* STG.E [R0], R2 */
        0xe30000000007000full};
    const u32 descriptor[3] = {(u32)stored, (u32)(stored >> 32), 16};
    u32 qmd[64] = {0};
    DispatchState d;
    u8 word[4];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    CHECK(mem_map(UMA_Q, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GQ, UMA_Q, 0x200000);
    gmmu_map(code, code_cpu, 0x10000);
    memset(GP(UMA_Q), 0, 0x3000);
    memcpy(GP(UMA_Q) + 0x1110, descriptor, sizeof descriptor);
    CHECK(invalidate_write_code(code, program, sizeof program / sizeof *program));
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)cbuf;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(cbuf >> 32) | 0x200u << 15;
    u64 dispatches = g_vk_dispatches;
    vk_lock();
    mx_dispatch_state(qmd, channel->rcomp, code, &d);
    vk_dispatch(&d);
    int ran = g_vk_dispatches == dispatches + 1, queued = vk_compute_pending(stored, 4) != 0,
        imported = vk_uma_imports(UMA_Q + 0x2000, 4) != 0;
    vk_unlock();
    CHECK(!gmem_read(stored, word, sizeof word));
    free(channel);
    CHECK(mem_unmap(UMA_Q, 0x200000) == 0);
    if (!ran || queued || !imported || snx_ld_le32(word) != 0x5eed)
        fprintf(stderr, "dispatch: ran %d, a compute result pending %d, imported %d, the buffer holds %08x\n", ran,
                queued, imported, snx_ld_le32(word));
    CHECK(ran && !queued && imported && snx_ld_le32(word) == 0x5eed);
    printf("in place: a dispatch on the graphics queue stores into guest memory where it is\n");
    return 0;
}

/* A linear texture over memory an engine's result is being written to in place (one game's exposure histogram, a
   64x1 texture one compute program fills and the next reads, in each frame): checked again, the texture takes the
   result from the import on the GPU, after the write, and nothing waits for the submit that writes it */
#define UMA_H  (HEAP_BASE + 0xd000000)
#define UMA_GH 0x4e9000000ull
static int check_uma_texture_pending(void) {
    const u32 rgba8 = 0x08 | 2u << 7 | 2u << 10 | 2u << 13 | 2u << 16;
    /* 64x1 2D, one level, pitch linear rows of 256 bytes */
    const u32 tic[8] = {rgba8, (u32)UMA_GH, (u32)(UMA_GH >> 32) | 2u << 21, 256 >> 5, 63 | 1u << 23, 0, 0, 0};
    VkBuffer r, rb;
    u64 ro;
    u8 result[256];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_H, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GH, UMA_H, 0x200000);
    memset(GP(UMA_H), 0x11, 0x1000);
    for (u32 i = 0; i < 256; i++) result[i] = GP(UMA_H)[0x10000 + i] = (u8)(i * 3 + 5); /* the engine's result */
    vk_lock();
    Surf* t = vk_tex_from_tic(tic);
    CHECK(t && !t->bl && t->size == 256);
    CHECK(vk_buf_get_input(UMA_GH, 256, 4, &r, &ro)); /* a draw reads it in place: an import holds it */
    int held = vk_uma_held(UMA_H, 256);
    vk_unlock();
    CHECK(!uma_dma(UMA_GH, UMA_GH + 0x10000)); /* written in place by a submit not made yet */
    vk_lock();
    int pending = vk_uma_pending(UMA_GH, 256);
    Surf* again = vk_tex_recheck(t);
    int waited = !vk_uma_pending(UMA_GH, 256);
    u8* texels = vk_readback_buffer(256, &rb);
    CHECK(again == t && texels);
    vk_surf_use(vk_cmd(), t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {64, 1, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &copy);
    vk_finish();
    vk_readback_ready();
    u32 differ = 0;
    for (u32 i = 0; i < 256; i++) differ += texels[i] != result[i];
    vk_unlock();
    CHECK(mem_unmap(UMA_H, 0x200000) == 0);
    if (!held || !pending || waited || differ)
        fprintf(stderr,
                "texture over a write in place: held %d, pending %d, waited for it %d, %u of 256 bytes not the "
                "result's (first: %u, want %u)\n",
                held, pending, waited, differ, texels[0], result[0]);
    CHECK(held && pending && !waited && !differ);
    printf("in place: a linear texture over a pending write in place takes it on the GPU, with no wait\n");
    return 0;
}

/* A dispatch whose constant buffer an engine's result is being written to in place (one game's dispatches bound 256
   bytes of constants such a write covered once a frame): the buffer is copied for the dispatch on the GPU, after the
   write, and nothing waits for the submit that writes it. A compute program stores c[1][0], the result's first word,
   through an NVN descriptor in cbuf 0, which no write covers */
#define UMA_K  (HEAP_BASE + 0xd200000)
#define UMA_GK 0x4ea000000ull
static int check_uma_dispatch_constants(void) {
    const u64 cbuf = UMA_GK + 0x1000, later = UMA_GK + 0x3000, stored = UMA_GK + 0x2000, code = UMA_GK + 0x20000;
    const u64 program[] = {
        0x001f8000fc0007e0ull,  0x4c98078004470000ull, /* MOV R0, c[0][0x110]: the buffer's address, NVN's descriptor */
        0x4c98078004570001ull,                         /* MOV R1, c[0][0x114] */
        0x4c98078400070002ull,                         /* MOV R2, c[1][0x0] */
        0x001f8000fc0007e0ull,  0xeedc200000070002ull, /* STG.E [R0], R2 */
        0xe30000000007000full};
    const u32 descriptor[3] = {(u32)stored, (u32)(stored >> 32), 16};
    u32 qmd[64] = {0};
    DispatchState d;
    VkBuffer r;
    u64 ro;
    u8 word[4];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    CHECK(mem_map(UMA_K, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GK, UMA_K, 0x200000);
    memset(GP(UMA_K), 0, 0x4000);
    memcpy(GP(UMA_K) + 0x1110, descriptor, sizeof descriptor);
    memset(GP(UMA_K) + 0x3000, 0x11, 0x100);                         /* cbuf 1 before the write */
    for (u32 i = 0; i < 256; i++) GP(UMA_K)[0x10000 + i] = (u8)(0x40 + i); /* the engine's result */
    CHECK(invalidate_write_code(code, program, sizeof program / sizeof *program));
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u | 2u;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)cbuf;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(cbuf >> 32) | 0x200u << 15;
    qmd[QMD_CONST_BUFFER_CONFIG + 2] = (u32)later;
    qmd[QMD_CONST_BUFFER_CONFIG + 3] = (u32)(later >> 32) | 0x100u << 15;
    vk_lock();
    CHECK(vk_buf_get_input(later, 256, 4, &r, &ro)); /* a draw reads it in place: an import holds it */
    int held = vk_uma_held(UMA_K + 0x3000, 256);
    vk_unlock();
    CHECK(!uma_dma(later, UMA_GK + 0x10000)); /* written in place by a submit not made yet */
    u64 dispatches = g_vk_dispatches;
    vk_lock();
    int pending = vk_uma_pending(later, 256);
    mx_dispatch_state(qmd, channel->rcomp, code, &d);
    vk_dispatch(&d);
    int ran = g_vk_dispatches == dispatches + 1, waited = !vk_uma_pending(later, 256);
    vk_unlock();
    CHECK(!gmem_read(stored, word, sizeof word));
    free(channel);
    CHECK(mem_unmap(UMA_K, 0x200000) == 0);
    if (!held || !pending || !ran || waited || snx_ld_le32(word) != 0x43424140)
        fprintf(stderr, "dispatch constants: held %d, pending %d, ran %d, waited for the write %d, stored %08x\n", held,
                pending, ran, waited, snx_ld_le32(word));
    CHECK(held && pending && ran && !waited && snx_ld_le32(word) == 0x43424140);
    printf("in place: a dispatch's constant buffer under a pending write in place is copied on the GPU, with no "
           "wait\n");
    return 0;
}

/* Texture and sampler headers are read without waiting for a write in place over them, as no program writes headers
   (one game's dispatches found such a write over theirs once a frame, and the GPU thread waited 20 ms for it):
   vk_gmem_words reads the words memory holds as it reads, and the write stays pending */
#define UMA_J  (HEAP_BASE + 0xd400000)
#define UMA_GJ 0x4eb000000ull
static int check_uma_header_words(void) {
    VkBuffer r;
    u64 ro;
    u32 words[8];
    u8 now[32];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_J, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GJ, UMA_J, 0x200000);
    for (u32 i = 0; i < 0x100; i++) GP(UMA_J)[i] = (u8)(0x80 + i);           /* the header pool's entries */
    for (u32 i = 0; i < 0x100; i++) GP(UMA_J)[0x10000 + i] = (u8)(0x20 + i); /* the engine's result */
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GJ, 256, 4, &r, &ro)); /* a draw reads it in place: an import holds it */
    int held = vk_uma_held(UMA_J, 256);
    vk_unlock();
    CHECK(!uma_dma(UMA_GJ, UMA_GJ + 0x10000)); /* written in place by a submit not made yet */
    vk_lock();
    int pending = vk_uma_pending(UMA_GJ, 256);
    memcpy(now, GP(UMA_J) + 0x20, sizeof now);
    vk_gmem_words(UMA_GJ + 0x20, words, 8);
    int waited = !vk_uma_pending(UMA_GJ, 256), same = 1;
    for (u32 i = 0; i < 8; i++) same &= words[i] == snx_ld_le32(now + 4 * i);
    vk_finish();
    vk_unlock();
    CHECK(GP(UMA_J)[0] == 0x20 && GP(UMA_J)[0xff] == 0x1f); /* the write, done after the finish */
    CHECK(mem_unmap(UMA_J, 0x200000) == 0);
    if (!held || !pending || waited || !same)
        fprintf(stderr, "header words: held %d, pending %d, waited for the write %d, the words memory's %d\n", held,
                pending, waited, same);
    CHECK(held && pending && !waited && same);
    printf("in place: texture headers under a pending write in place are read with no wait\n");
    return 0;
}

/* Physical memory as GPU memory where it is (placed maps, VK_EXT_map_memory_placed): a window mapped whole is a chunk
   of device memory at its guest address, which the GPU reads and writes in place with no import, the CPU's and the
   GPU's writes each seen by the other; given back and mapped again it reads zero, a new chunk. Write tracking leaves it
   alone (userfaultfd can't register the driver's mapping) */
#define UMA_L  (HEAP_BASE + 0x3e00000)
#define UMA_GL 0x4e5000000ull
static int check_uma_placed(void) {
    VkBuffer r, w, rb;
    u64 ro, wo;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    if (!vk_uma_can_place()) {
        printf("skip: the device can't map memory where it is told (VK_EXT_map_memory_placed, its unmap reserve)\n");
        return 0;
    }
    int was = vk_uma_placed(1);
    CHECK(mem_map_physical(UMA_L, 0x200000, ~0ull) == 0);
    gmmu_map(UMA_GL, UMA_L, 0x200000);
    CHECK(GP(UMA_L)[0] == 0 && GP(UMA_L)[0x1fffff] == 0);
    memset(GP(UMA_L) + 0x100, 0x5c, 256); /* the guest CPU's write */
    vk_lock();
    u8* result = vk_readback_buffer(256, &rb);
    CHECK(result && vk_buf_get_input(UMA_GL + 0x100, 256, 4, &r, &ro)); /* a draw's read in place */
    int placed = vk_uma_placed_at(UMA_L + 0x100, 256), imports = (int)vk_uma_imports(UMA_L, 0x200000);
    uma_read(r, ro, rb, 0);
    CHECK(vk_buf_copy_writable(UMA_GL + 0x400, 256, &w, &wo, 1)); /* a shader's write in place */
    CHECK(!owned_test_fill(w, wo, 256, 0x6d));
    vk_buf_writable_commit();
    int written = vk_uma_placed_at(UMA_L + 0x400, 256) && !vk_buf_pending(UMA_GL + 0x400, 256);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 256; i++) CHECK(result[i] == 0x5c);
    int tracked = vk_written_since(GP(UMA_L), 0x1000, 0);
    vk_unlock();
    CHECK(GP(UMA_L)[0x400] == 0x6d && GP(UMA_L)[0x4ff] == 0x6d && GP(UMA_L)[0x500] == 0);
    /* given back, then mapped again: zero, in a new chunk */
    CHECK(mem_unmap_physical(UMA_L, 0x200000) == 0);
    vk_lock();
    int gone = !vk_uma_placed_at(UMA_L, 0x1000);
    vk_unlock();
    CHECK(mem_map_physical(UMA_L, 0x200000, ~0ull) == 0);
    CHECK(GP(UMA_L)[0x100] == 0 && GP(UMA_L)[0x400] == 0);
    vk_lock();
    int again = vk_uma_placed_at(UMA_L, 0x200000);
    vk_unlock();
    CHECK(mem_unmap_physical(UMA_L, 0x200000) == 0);
    vk_uma_placed(was);
    if (!placed || imports || !written || tracked != -1 || !gone || !again)
        fprintf(stderr,
                "placed: read in a chunk %d with %d imports, written in a chunk %d, write tracking %d (-1: leaves it "
                "alone), gone after the unmap %d, a chunk again %d\n",
                placed, imports, written, tracked, gone, again);
    CHECK(placed && !imports && written && tracked == -1 && gone && again);
    printf("in place: physical memory as GPU memory where it is, read and written there with no import, given back and "
           "zero when mapped again\n");
    return 0;
}

/* A view of GPU memory where it is (MapMemory's, at a free address): the chunk's own pages mapped there again through
   its dma-buf, so a write through either address is the other's at once, and the GPU reads the view in place with no
   import. Unmapped, the view goes and the chunk stays */
#define UMA_VIEW  (HEAP_BASE + 0x4800000)
#define UMA_GVIEW 0x4e6000000ull
static int check_uma_placed_view(void) {
    VkBuffer r, rb;
    u64 ro;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    if (!vk_uma_can_view()) {
        printf("skip: the device can't map memory where it is told or export it as a dma-buf\n");
        return 0;
    }
    int was = vk_uma_placed(1);
    const u64 src = UMA_L + 0x10000, size = 0x4000;
    CHECK(mem_map_physical(UMA_L, 0x200000, ~0ull) == 0);
    memset(GP(src), 0x3a, size);
    CHECK(mem_alias_begin(src, size) == 0);
    CHECK(mem_alias_map(UMA_VIEW, src, size, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GVIEW, UMA_VIEW, size);
    int shared_in = GP(UMA_VIEW)[0] == 0x3a && GP(UMA_VIEW)[size - 1] == 0x3a;
    GP(UMA_VIEW)[0x100] = 0x3b; /* through the view: the source's at once */
    GP(src)[0x200] = 0x3c;      /* and the other way */
    int shared = GP(src)[0x100] == 0x3b && GP(UMA_VIEW)[0x200] == 0x3c;
    vk_lock();
    u8* result = vk_readback_buffer(256, &rb);
    CHECK(result && vk_buf_get_input(UMA_GVIEW + 0x100, 256, 4, &r, &ro)); /* the GPU reads the view in place */
    int placed = vk_uma_placed_at(UMA_VIEW + 0x100, 256), imports = (int)vk_uma_imports(UMA_VIEW, size);
    uma_read(r, ro, rb, 0);
    vk_finish();
    vk_readback_ready();
    int gpu = result[0] == 0x3b && result[1] == 0x3a && result[0xff] == 0x3a;
    vk_unlock();
    CHECK(mem_alias_unmap(UMA_VIEW, size, src) == 0);
    mem_alias_end(src, size);
    vk_lock();
    int gone = !vk_uma_placed_at(UMA_VIEW, 0x1000), kept = vk_uma_placed_at(src, size);
    vk_unlock();
    CHECK(GP(src)[0x100] == 0x3b && GP(src)[0x200] == 0x3c && GP(src)[0] == 0x3a);
    CHECK(mem_unmap_physical(UMA_L, 0x200000) == 0);
    vk_uma_placed(was);
    if (!shared_in || !shared || !placed || imports || !gpu || !gone || !kept)
        fprintf(stderr,
                "placed view: the source's bytes at the view %d, writes shared %d, read in a chunk %d with %d imports, "
                "the GPU's read %d, the view gone %d, the chunk kept %d\n",
                shared_in, shared, placed, imports, gpu, gone, kept);
    CHECK(shared_in && shared && placed && !imports && gpu && gone && kept);
    printf(
        "in place: a view of GPU memory where it is maps the same pages, which the GPU reads there with no import\n");
    return 0;
}

/* vk_memory's auto (0) never moves guest memory into AHardwareBuffers: on a device that shares memory, takes them and
   has no VK_EXT_external_memory_host, it copies, and only vk_memory=2 moves. An import where it is stays auto's on such
   a device, and 1 copies everywhere */
static int check_uma_way(void) {
    CHECK(vk_uma_way(0, VK_MEM_UMA, 0, 1) == VK_UMA_WITH_NONE);
    CHECK(vk_uma_way(2, VK_MEM_UMA, 0, 1) == VK_UMA_WITH_AHB);
    CHECK(vk_uma_way(0, VK_MEM_UMA, 1, 1) == VK_UMA_WITH_HOST && vk_uma_way(2, VK_MEM_UMA, 1, 1) == VK_UMA_WITH_HOST);
    CHECK(vk_uma_way(0, VK_MEM_REBAR, 1, 1) == VK_UMA_WITH_NONE &&
          vk_uma_way(2, VK_MEM_REBAR, 1, 0) == VK_UMA_WITH_HOST);
    CHECK(vk_uma_way(1, VK_MEM_UMA, 1, 1) == VK_UMA_WITH_NONE && vk_uma_way(2, VK_MEM_UMA, 0, 0) == VK_UMA_WITH_NONE);
    printf("in place: auto copies where only AHardwareBuffers would take guest memory; vk_memory=2 moves it there\n");
    return 0;
}
/* The migrate way (Android's: guest memory moves into AHardwareBuffers; elsewhere into memfd regions the driver imports
   as host memory). A range moves into a region of the whole 2 MiB windows it touches, with its bytes, where a range
   beside it is found too and what the guest writes is what the GPU reads; a range over the edge makes one region of
   both windows. A region's buffer is memory of its own: unmapping or trapping its pages waits for nothing, a copy
   recorded before reads the bytes it had, and the rest of the region goes back to the reservation's own pages with
   its bytes */
#define UMA_M  (HEAP_BASE + 0x4000000)
#define UMA_GM 0x4e0000000ull
/* whether the driver imports a buffer's pages as host memory, as the regions are off Android: lavapipe does, amdgpu
   doesn't (its user pointers take anonymous memory only) */
static int uma_imports_shared(void) {
    const u64 size = 0x200000;
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkDeviceMemory mem;
    void* view;
    int fd, ok = 0;
    if (plat_gpu_buffer(size, &fd, &view)) return 0;
    if (vkGetMemoryHostPointerPropertiesEXT(g_vk.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, view,
                                            &hp) == VK_SUCCESS &&
        hp.memoryTypeBits) {
        VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                                .pHostPointer = view};
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size};
        while (!((hp.memoryTypeBits >> ai.memoryTypeIndex) & 1)) ai.memoryTypeIndex++;
        ok = vkAllocateMemory(g_vk.dev, &ai, NULL, &mem) == VK_SUCCESS;
        if (ok) vkFreeMemory(g_vk.dev, mem, NULL);
    }
    plat_gpu_buffer_free(fd, view, size);
    return ok;
}
/* whether the host page at p is a buffer's (plat_gpu_buffer's memfd), as /proc/self/maps names it: -1 when it can't
   tell */
static int uma_in_buffer(const u8* p) {
#if defined(__linux__)
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512];
    int r = 0;
    if (!f) return -1;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi;
        if (sscanf(line, "%llx-%llx", &lo, &hi) == 2 && (u64)(uintptr_t)p >= lo && (u64)(uintptr_t)p < hi) {
            r = strstr(line, "snx-gpu-buffer") != NULL;
            break;
        }
    }
    fclose(f);
    return r;
#else
    (void)p;
    return -1;
#endif
}
static u8 uma_fill(u64 i) { return (u8)(i * 7 + 3); }
static int check_uma_migrate(void) {
    const u64 window = 0x200000;
    VkBuffer b, b2, rb;
    u64 off, off2;
#if defined(__ANDROID__)
    printf("skip: the migrate way runs here as the device chose it (vk_memory), not from this check\n");
    return 0;
#endif
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    if (!uma_imports_shared()) {
        printf("skip: the driver imports no buffer's pages as host memory, which the migrate way needs here\n");
        return 0;
    }
    CHECK(mem_map(UMA_M, 4 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GM, UMA_M, 4 * window);
    u8* g = GP(UMA_M);
    for (u64 i = 0; i < 4 * window; i++) g[i] = uma_fill(i);
    vk_lock();
    CHECK(vk_uma_migrate(1));
    u8* result = vk_readback_buffer(768, &rb);
    CHECK(result);
    /* the first window moves, with its bytes; a range beside it is in the same region */
    CHECK(vk_uma_get(UMA_GM + 0x1000, 256, 1, &b, &off) && off == 0x1000);
    CHECK(vk_uma_imports(UMA_M, 4 * window) == 1 && vk_uma_imports(UMA_M + window, 3 * window) == 0);
    CHECK(uma_in_buffer(g) == 1 && uma_in_buffer(g + window) == 0);
    CHECK(vk_uma_get(UMA_GM + 0x100000, 256, 1, &b2, &off2) && b2 == b && off2 == 0x100000);
    g[0x1000] = 0x5a; /* after the lookup, before the GPU reads it */
    uma_read(b, off, rb, 0);
    /* over the edge: one region of both windows, the first one's going with its last reader */
    CHECK(vk_uma_get(UMA_GM + window - 0x80, 256, 1, &b2, &off2) && b2 != b && off2 == window - 0x80);
    CHECK(uma_in_buffer(g + window) == 1 && uma_in_buffer(g + 2 * window) == 0);
    uma_read(b2, off2, rb, 256);
    uma_read(b2, 0x1000, rb, 512);
    vk_frame_check(); /* a draw begins: the copies are all recorded */
    u64 recorded = vk_submit_number();
    vk_unlock();
    /* the second window unmapped while the copies are only recorded: nothing waits, nothing is submitted */
    CHECK(mem_unmap(UMA_M + window, window) == 0);
    vk_lock();
    CHECK(vk_submit_number() == recorded);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 256; i++) CHECK(result[i] == (i ? uma_fill(0x1000 + i) : 0x5a));
    for (u32 i = 0; i < 256; i++) CHECK(result[256 + i] == uma_fill(window - 0x80 + i)); /* as they were */
    for (u32 i = 0; i < 256; i++) CHECK(result[512 + i] == (i ? uma_fill(0x1000 + i) : 0x5a));
    /* the region went with the unmap, its first window back in the reservation's own pages, and once their readers are
       done (a present) neither region is the driver's */
    CHECK(uma_in_buffer(g) == 0 && g[0x1000] == 0x5a && g[0x40] == uma_fill(0x40));
    vk_uma_frame();
    CHECK(vk_uma_imports(UMA_M, 4 * window) == 0);
    /* a range of a region the GPU owns next (a result published over it): nothing waits for the copy recorded before,
       which reads the bytes from before, and the region goes, its other pages back with their bytes */
    if (own_traps()) {
        const u64 at = 2 * window + 0x100;
        CHECK(vk_uma_get(UMA_GM + at, 256, 1, &b, &off) && uma_in_buffer(g + 2 * window) == 1);
        uma_read(b, off, rb, 0);
        vk_frame_check();
        VkOwnRes* owner = uma_owner(256, 0x42);
        CHECK(owner);
        vk_own_publish_owned(UMA_M + at, 256, owner, 0);
        vk_finish();
        vk_readback_ready();
        for (u32 i = 0; i < 256; i++) CHECK(result[i] == uma_fill(at + i));
        vk_uma_frame();
        CHECK(vk_uma_imports(UMA_M + 2 * window, 2 * window) == 0 && uma_in_buffer(g + 2 * window + 0x2000) == 0);
        CHECK(g[2 * window + 0x2007] == uma_fill(2 * window + 0x2007));
        vk_unlock();
        CHECK(!uma_landed(UMA_M + at, 256, 0x42));
        vk_own_res_unref(owner);
        vk_lock();
    }
    CHECK(vk_uma_migrate(0));
    vk_unlock();
    CHECK(mem_unmap(UMA_M, 4 * window) == 0);
    printf(
        "in place, the migrate way: a range moves into a region of its windows and the GPU reads what the guest "
        "writes there; an unmap%s of a region waits for nothing, a copy recorded before reads its bytes, and the rest "
        "of it moves back\n",
        own_traps() ? " or a trap" : "");
    return 0;
}
/* A region of the migrate way written in place (memfd regions here, AHardwareBuffers on Android): the write lands in
   guest memory when the region's pages move on before its submit (a trap moves them into the pool) and when the region
   moves back (idleness, room, the way changed): both wait for it, as the region's buffer is the guest's memory until
   then and a write after the move would land in the buffer alone */
#define UMA_RW  (HEAP_BASE + 0x4a00000)
#define UMA_GRW 0x4e7000000ull
static int check_uma_region_writes(void) {
    const u64 window = 0x200000;
    VkBuffer w;
    u64 wo;
#if defined(__ANDROID__)
    printf("skip: the migrate way runs here as the device chose it (vk_memory), not from this check\n");
    return 0;
#endif
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    if (!uma_imports_shared()) {
        printf("skip: the driver imports no buffer's pages as host memory, which the migrate way needs here\n");
        return 0;
    }
    CHECK(mem_map(UMA_RW, 3 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GRW, UMA_RW, 3 * window);
    u8* g = GP(UMA_RW);
    for (u32 i = 0; i < 3; i++) memset(g + i * window, 0x91, 0x6000);
    vk_lock();
    CHECK(vk_uma_migrate(1));
    /* written in place in a region, then its page trapped before anything waited for the write */
    CHECK(vk_buf_copy_writable(UMA_GRW + 0x100, 256, &w, &wo, 1));
    int region = vk_uma_imports(UMA_RW, window) == 1 && uma_in_buffer(g) == 1;
    CHECK(!owned_test_fill(w, wo, 256, 0x92));
    vk_buf_writable_commit();
    VkOwnRes* owner = uma_owner(256, 0x42);
    CHECK(owner);
    vk_own_publish_owned(UMA_RW + 0x800, 256, owner, 0); /* the same host page, GPU-owned: a trap */
    vk_finish();
    vk_unlock();
    CHECK(!uma_landed(UMA_RW + 0x800, 256, 0x42));
    vk_own_res_unref(owner);
    CHECK(g[0x100] == 0x92 && g[0x1ff] == 0x92 && g[0xff] == 0x91 && g[0x200] == 0x91);
    /* written in place in a region of the next window (the trap left the first one's pages in the pool), which then
       moves back with the way before anything waited for the write */
    u8* g2 = g + window;
    vk_lock();
    CHECK(vk_buf_copy_writable(UMA_GRW + window + 0x2100, 256, &w, &wo, 1));
    int region2 = vk_uma_imports(UMA_RW + window + 0x2000, 0x1000) == 1 && uma_in_buffer(g2 + 0x2000) == 1;
    CHECK(!owned_test_fill(w, wo, 256, 0x93));
    vk_buf_writable_commit();
    CHECK(vk_uma_migrate(0)); /* every region moves back first */
    vk_finish();
    vk_unlock();
    CHECK(g2[0x2100] == 0x93 && g2[0x21ff] == 0x93 && g2[0x20ff] == 0x91 && uma_in_buffer(g2 + 0x2000) == 0);
    /* an engine's result over a region: a region is an import (vk_uma_held), so the result is copied there in place */
    const u64 src = 0x4df000000ull, src_cpu = HEAP_BASE + 0x700000;
    u8 bytes[256];
    VkBuffer r;
    u64 ro;
    gmmu_map(src, src_cpu, 0x10000);
    memset(GP(src_cpu), 0xa1, 0x1000);
    vk_lock();
    CHECK(vk_uma_migrate(1));
    CHECK(vk_buf_get_input(UMA_GRW + 0x4100, 256, 4, &r, &ro));
    int held = vk_uma_held(UMA_RW + 0x4100, 256) && uma_in_buffer(g + 0x4000) == 1;
    vk_unlock();
    CHECK(!uma_dma(UMA_GRW + 0x4100, src + 0x100));
    int owned = own_lookup(UMA_RW + 0x4100, 256, NULL, 0) != 0;
    CHECK(!gmem_read(UMA_GRW + 0x4100, bytes, sizeof bytes));
    for (u32 i = 0; i < 256; i++) CHECK(bytes[i] == 0xa1);
    /* written in place in a region of a third window, whose page a view then takes before anything waited for the
       write: mem_alias_begin moves the source's pages into the pool */
    u8* g3 = g + 2 * window;
    vk_lock();
    CHECK(vk_buf_copy_writable(UMA_GRW + 2 * window + 0x100, 256, &w, &wo, 1));
    int region3 = vk_uma_imports(UMA_RW + 2 * window, 0x1000) == 1 && uma_in_buffer(g3) == 1;
    CHECK(!owned_test_fill(w, wo, 256, 0x94));
    vk_buf_writable_commit();
    vk_unlock();
    CHECK(mem_alias_begin(UMA_RW + 2 * window, 0x1000) == 0);
    CHECK(g3[0x100] == 0x94 && g3[0x1ff] == 0x94 && g3[0xff] == 0x91 && g3[0x200] == 0x91);
    mem_alias_end(UMA_RW + 2 * window, 0x1000);
    vk_lock();
    CHECK(vk_uma_migrate(0));
    vk_finish();
    vk_unlock();
    CHECK(g[0x4100] == 0xa1 && g[0x41ff] == 0xa1 && g[0x4200] == 0x91);
    if (!held || owned) fprintf(stderr, "region publish: held %d, owned %d\n", held, owned);
    CHECK(held && !owned);
    CHECK(mem_unmap(UMA_RW, 3 * window) == 0);
    if (!region || !region2 || !region3)
        fprintf(stderr, "region writes: no region made (%d, %d, %d)\n", region, region2, region3);
    CHECK(region && region2 && region3);
    printf("in place, the migrate way: a write in place in a region lands in guest memory when its pages move on (a "
           "trap, a view) or the region moves back before the write's submit, and an engine's result over a region is "
           "copied there, nothing GPU-owned\n");
    return 0;
}
/* A range over the edge between two regions makes one region of their union, whose buffer takes the bytes the pages
   hold (mem_migrate): a write in place pending in one of them (its submit not done, the copy engine's result over a
   region, say) lands in that region's buffer first, or it would land in a buffer no page maps any more, and the guest
   would read the bytes from before it */
#define UMA_RM  (HEAP_BASE + 0x5000000)
#define UMA_GRM 0x4e8000000ull
static int check_uma_region_merge(void) {
    const u64 window = 0x200000;
    VkBuffer w, b;
    u64 wo, bo;
#if defined(__ANDROID__)
    printf("skip: the migrate way runs here as the device chose it (vk_memory), not from this check\n");
    return 0;
#endif
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    if (!uma_imports_shared()) {
        printf("skip: the driver imports no buffer's pages as host memory, which the migrate way needs here\n");
        return 0;
    }
    CHECK(mem_map(UMA_RM, 2 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GRM, UMA_RM, 2 * window);
    u8* g = GP(UMA_RM);
    memset(g, 0x91, 0x1000);
    memset(g + window - 0x1000, 0x91, 0x2000);
    vk_lock();
    CHECK(vk_uma_migrate(1));
    /* a region of each window: the second's for a range read in place, the first's for a write in place */
    CHECK(vk_buf_get_input(UMA_GRM + window + 0x100, 256, 4, &b, &bo));
    CHECK(vk_buf_copy_writable(UMA_GRM + 0x100, 256, &w, &wo, 1));
    int regions = vk_uma_imports(UMA_RM, 2 * window) == 2 && uma_in_buffer(g) == 1 && uma_in_buffer(g + window) == 1;
    CHECK(!owned_test_fill(w, wo, 256, 0x95));
    vk_buf_writable_commit();
    /* a range over the edge, before anything waited for the write: one region of both windows holds it */
    CHECK(vk_buf_get_input(UMA_GRM + window - 0x100, 0x200, 4, &b, &bo));
    int merged = vk_uma_held(UMA_RM + window - 0x100, 0x200) && uma_in_buffer(g) == 1;
    vk_finish();
    vk_unlock();
    CHECK(g[0x100] == 0x95 && g[0x1ff] == 0x95 && g[0xff] == 0x91 && g[0x200] == 0x91);
    vk_lock();
    CHECK(vk_uma_migrate(0)); /* the region moves back with its bytes */
    vk_finish();
    vk_unlock();
    CHECK(uma_in_buffer(g) == 0 && g[0x100] == 0x95 && g[0x1ff] == 0x95 && g[0xff] == 0x91);
    CHECK(mem_unmap(UMA_RM, 2 * window) == 0);
    if (!regions || !merged) fprintf(stderr, "region merge: two regions %d, one of their union %d\n", regions, merged);
    CHECK(regions && merged);
    printf("in place, the migrate way: a region that takes over another lands the writes in place pending there "
           "first\n");
    return 0;
}
