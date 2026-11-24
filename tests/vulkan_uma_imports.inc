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
    VkOwnRes* r = vk_own_res_new(size, VK_OWN_BY_DRAW);
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

/* A lookup in a window that a trap holds a page of imports the range's own pages, not the window: a driver may read
   an import's pages on the CPU from the thread that holds vk_lock, which a trapped page's landing needs (PanVK on a
   Mali phone read one up to its trapped page, #1875). The owner stays, nothing lands, and the range reads the CPU's
   bytes */
#define UMA_TW  (HEAP_BASE + 0xd800000) /* a window, one page of it trapped */
#define UMA_GTW 0x4f1000000ull
static int check_uma_trapped_window(void) {
    const u64 window = 0x200000, page = 0x1000, held = UMA_TW + 0x10000; /* uma.c's UMA_WINDOW */
    VkBuffer b, rb;
    u64 o;
    if (uma_skip()) return 0;
    CHECK(mem_map(UMA_TW, window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GTW, UMA_TW, window);
    memset(GP(UMA_TW), 0x71, (size_t)window);
    vk_lock();
    u8* result = vk_readback_buffer(256, &rb);
    VkOwnRes* owner = uma_owner(page, 0x72);
    CHECK(result && owner);
    vk_own_publish_owned(held, page, owner, 0);
    CHECK(own_lookup(held, page, NULL, 0) == 1);
    u64 lands = own_landings();
    CHECK(vk_buf_get_input(UMA_GTW + 0x100, 256, 4, &b, &o));
    u32 over = vk_uma_imports(held, page), inplace = vk_uma_imports(UMA_TW, page);
    uma_read(b, o, rb, 0);
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 256; i++) CHECK(result[i] == 0x71);
    int kept = own_lookup(held, page, NULL, 0) == 1, landed = own_landings() != lands;
    vk_unlock();
    if (over || !kept || landed)
        fprintf(stderr, "in place, a window with a trapped page: %u imports over that page, owner %s, %s\n", over,
                kept ? "kept" : "gone", landed ? "landed" : "nothing landed");
    CHECK(!over && kept && !landed);
    CHECK(!uma_landed(held, page, 0x72));
    vk_own_res_unref(owner);
    CHECK(mem_unmap(UMA_TW, window) == 0);
    printf("in place: a lookup in a window with a trapped page imports none a trap holds (%s)\n",
           inplace ? "its own page imported" : "copied: the driver refused it");
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
#define UMA_CB_PROGRAMS 0x4dc000000ull
#define UMA_CB_TARGET   (UMA_CB_PROGRAMS + 0x18000)
/* a channel whose draw takes the green of its color from c[0][0x644] of the window of guest memory at window (2 MiB,
   GPU address gva), 1.0 there: NULL when it can't be set up. uma_constants_done undoes it */
static GpuChannel* uma_constants_draw(u64 window, u64 gva) {
    const u64 address = UMA_CB_PROGRAMS, cpu = HEAP_BASE + 0x740000;
    const u64 stream = address + 0x12000;
    const u64 vertex[] = {0xefd9ff800807ff00ull, /* ALD R0..3, a[0x80] */
                          0xeff1ff800707ff00ull, /* position */
                          0xefd9ff800907ff00ull, /* ALD R0..3, a[0x90] */
                          0x4c98078019170001ull, /* MOV R1, c[0x0][0x644] */
                          0xeff1ff800807ff00ull, /* color varying */
                          0xe30000000007000full};
    const float position[12] = {-1, -1, 0, 1, 3, -1, 0, 1, -1, 3, 0, 1}, red[4] = {1, 0, 0, 1};
    const float one = 1.0f;
    u64 fragment[5];
    u8 bytes[96];
    GpuChannel* channel = calloc(1, sizeof *channel);
    if (!channel) return NULL;
    channel->id = 125;
    if (mem_map(window, 0x200000, MS_NORMAL, PERM_RW) != 0) {
        free(channel);
        return NULL;
    }
    gmmu_map(gva, window, 0x200000);
    memset(GP(window), 0, 0x1000);
    memcpy(GP(window) + 0x644, &one, 4);
    gmmu_map(address, cpu, 0x20000);
    for (u32 i = 0; i < 4; i++) fragment[i] = 0xe000000000070000ull | (0x20ull + i) << 30 | i;
    fragment[4] = 0xe30000000007000full;
    for (u32 v = 0; v < 3; v++) {
        memcpy(bytes + v * 32, position + 4 * v, 16);
        memcpy(bytes + v * 32 + 16, red, 16);
    }
    if (!mesh_program(address, vertex, 6, 0) || !mesh_program(address + 0x10000, fragment, 5, 1) ||
        gmem_write(stream, bytes, sizeof bytes)) {
        free(channel);
        (void)mem_unmap(window, 0x200000);
        return NULL;
    }
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
    all_samples(r);
    const float transform[6] = {2, 2, 0.5f, 2, 2, 0.5f};
    memcpy(r + 0x280, transform, sizeof transform);
    r[0x458] = 0 | 0u << 7 | 1u << 21 | 7u << 27;  /* stream 0 at 0: RGBA32F */
    r[0x459] = 0 | 16u << 7 | 1u << 21 | 7u << 27; /* stream 0 at 16: RGBA32F */
    r[0x700] = 0x1000 | 32;
    r[0x701] = (u32)(stream >> 32);
    r[0x702] = (u32)stream;
    r[0x7c0] = (u32)((stream + sizeof bytes - 1) >> 32);
    r[0x7c1] = (u32)(stream + sizeof bytes - 1);
    channel->ds.bind.cb_addr[0][0] = gva;
    channel->ds.bind.cb_size[0][0] = 0x1000;
    return channel;
}
static int uma_constants_done(GpuChannel* channel, u64 window) {
    channel->ds.bind.cb_addr[0][0] = 0;
    channel->ds.bind.cb_size[0][0] = 0;
    free(channel);
    CHECK(mem_unmap(window, 0x200000) == 0);
    return 0;
}
static int check_uma_constants(void) {
    const float one = 1.0f, zero = 0.0f;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    GpuChannel* channel = uma_constants_draw(UMA_C, UMA_GC);
    CHECK(channel);
    CHECK(!mesh_pixel(channel, UMA_CB_TARGET, 0, (const u8[4]){255, 255, 0, 255}));
    int inplace = vk_uma_imports(UMA_C, 0x1000);
    memcpy(GP(UMA_C) + 0x644, &zero, 4); /* the guest CPU's write */
    g_renderer->mem_may_have_changed();
    CHECK(!mesh_pixel(channel, UMA_CB_TARGET, 0, (const u8[4]){255, 0, 0, 255}));
    CHECK(!gmem_write(UMA_GC + 0x644, &one, 4)); /* the runtime's: copied from now on */
    CHECK(!mesh_pixel(channel, UMA_CB_TARGET, 0, (const u8[4]){255, 255, 0, 255}));
    CHECK(!uma_constants_done(channel, UMA_C));
    if (!inplace) fprintf(stderr, "constant buffer: copied, not read in place\n");
    CHECK(inplace);
    printf("in place: a constant buffer read where the guest's CPU wrote it, and copied once the runtime wrote it\n");
    return 0;
}

/* Where uma_copies copies constant buffers (vk_uma_imports_dear), their bytes are copied straight from guest memory
   into staging (render.c's cb_get), as on a GPU with memory of its own: the draw reads the 1.0 there. A draw that
   writes the page in place, recorded and not yet run, keeps the next lookup off that copy, whose bytes would be from
   before the write: the constant buffer goes through the buffer cache, after the write, and the draw reads its 0.0.
   On a discrete GPU with host imports (vk_uma_storage_copied) the write is a pending result of the copies' path
   instead (buffer.c's copy_writable), with the same order */
#define UMA_CD  (HEAP_BASE + 0x4c00000)
#define UMA_GCD 0x4f5000000ull
static int check_uma_constants_copied(void) {
    const int was = g_settings.guest_buffers_copied;
    const u8 yellow[4] = {255, 255, 0, 255}, red[4] = {255, 0, 0, 255};
    VkDrawCounts before, after;
    VkBuffer w;
    u64 wo;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    GpuChannel* channel = uma_constants_draw(UMA_CD, UMA_GCD);
    CHECK(channel);
    g_settings.guest_buffers_copied = 2;
    vk_draw_counts(&before);
    CHECK(!mesh_pixel(channel, UMA_CB_TARGET, 0, yellow));
    vk_draw_counts(&after);
    const int copied = after.cb[VK_CB_COPIED] > before.cb[VK_CB_COPIED];
    /* the shader's write of zeros over c[0][0x600..0x6ff], in place and pending */
    vk_lock();
    const int staged = vk_uma_storage_copied();
    CHECK(vk_buf_copy_writable(UMA_GCD + 0x600, 256, &w, &wo, 1));
    const int inplace = staged || !vk_buf_pending(UMA_GCD + 0x600, 256); /* staged: pending once committed, below */
    CHECK(!owned_test_fill(w, wo, 256, 0));
    vk_buf_writable_commit();
    const int pending = staged ? vk_buf_pending(UMA_GCD + 0x644, 4) : vk_uma_pending(UMA_GCD + 0x644, 4);
    vk_unlock();
    vk_draw_counts(&before);
    CHECK(!mesh_pixel(channel, UMA_CB_TARGET, 0, red));
    vk_draw_counts(&after);
    const int cached =
        after.cb[VK_CB_PENDING] > before.cb[VK_CB_PENDING] && after.cb[VK_CB_COPIED] == before.cb[VK_CB_COPIED];
    CHECK(GP(UMA_CD)[0x644] == 0 && GP(UMA_CD)[0x647] == 0);
    g_settings.guest_buffers_copied = was;
    CHECK(!uma_constants_done(channel, UMA_CD));
    g_renderer->mem_may_have_changed();
    if (!copied || !inplace || !pending || !cached)
        fprintf(stderr, "constant buffer: copied straight %d, the write in place %d and pending %d, then cached %d\n",
                copied, inplace, pending, cached);
    CHECK(copied && inplace && pending && cached);
    printf("in place: a constant buffer copied straight into staging where the buffer cache copies, and through the "
           "cache after a write in place over it\n");
    return 0;
}

/* Storage the GPU writes is written in place (vk_buf_copy_writable): the range bound for writing is the import of its
   guest memory, with no result to land; a later reader on the GPU reads the write in place, after a barrier, and so
   does one that copied the range before the write; and the runtime's CPU reader waits for the submit. A later
   dispatch's read-only storage binding of it (vk_buf_get_strict), its whole and a part at a storage buffer's alignment,
   is that import, where the write is, with no copy while the write is pending */
#define UMA_V  (HEAP_BASE + 0x3400000)
#define UMA_GV 0x4dd000000ull
static int check_uma_writes(void) {
    VkBuffer w, r, c, rb, s, t;
    u64 wo, ro, co, so, to;
    u8 bytes[256];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_V, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GV, UMA_V, 0x200000);
    memset(GP(UMA_V), 0x91, 0x1000);
    vk_lock();
    u8* result = vk_readback_buffer(1024, &rb);
    CHECK(result && vk_buf_get(UMA_GV + 0x100, 256, &c, &co)); /* a vertex fetch's copy, before the write */
    CHECK(vk_buf_copy_writable(UMA_GV + 0x100, 256, &w, &wo, 1));
    int inplace = vk_uma_imports(UMA_V, 0x200000) == 1 && !vk_buf_pending(UMA_GV + 0x100, 256);
    CHECK(!owned_test_fill(w, wo, 256, 0x92)); /* the shader's write */
    vk_buf_writable_commit();
    CHECK(vk_buf_get_input(UMA_GV + 0x100, 256, 4, &r, &ro)); /* a later draw's read of it */
    uma_read(r, ro, rb, 0);
    CHECK(vk_buf_get(UMA_GV + 0x100, 256, &c, &co)); /* the same fetch after it: not the copy from before */
    uma_read(c, co, rb, 256);
    int pending = vk_uma_pending(UMA_GV + 0x100, 256);
    CHECK(vk_buf_get_strict(UMA_GV + 0x100, 256, &s, &so)); /* a later dispatch's read-only storage */
    CHECK(vk_buf_get_strict(UMA_GV + 0x120, 0xe0, &t, &to)); /* a part, 32 bytes in */
    int shared = s == w && so == wo && t == w && to == wo + 0x20;
    uma_read(s, so, rb, 512);
    VkBufferCopy part = {to, 768, 0xe0};
    rec_CmdCopyBuffer(vk_cmd(), t, rb, 1, &part);
    vk_unlock();
    CHECK(!gmem_read(UMA_GV + 0x100, bytes, sizeof bytes)); /* the runtime's CPU read: after the write */
    for (u32 i = 0; i < 256; i++) CHECK(bytes[i] == 0x92);
    vk_lock();
    vk_finish();
    vk_readback_ready();
    for (u32 i = 0; i < 768 + 0xe0; i++) CHECK(result[i] == 0x92);
    vk_unlock();
    CHECK(GP(UMA_V)[0x100] == 0x92 && GP(UMA_V)[0x1ff] == 0x92 && GP(UMA_V)[0xff] == 0x91 && GP(UMA_V)[0x200] == 0x91);
    CHECK(mem_unmap(UMA_V, 0x200000) == 0);
    if (!inplace) fprintf(stderr, "storage write: copied out and landed, not written in place\n");
    CHECK(inplace);
    if (!shared)
        fprintf(stderr, "storage read: the write pending %d, bound at %p+%llx and %p+%llx, the import at %p+%llx\n",
                pending, (void*)s, (unsigned long long)so, (void*)t, (unsigned long long)to, (void*)w,
                (unsigned long long)wo);
    CHECK(pending && shared);
    printf("in place: storage written where it is in guest memory, read after the write on the GPU and the CPU, and "
           "bound for a dispatch's read where it was written\n");
    return 0;
}

/* An aliasing draw's storage is one page atlas (buffer.c's vk_buf_draw_prepare, here with uma_copies=2, which binds no
   aliasing draw in place), which starts from guest memory. An earlier draw of the same submission wrote 0x32 in place
   over [0x100, 0x200) of a page of 0x31, recorded and not run yet: the atlas must have that write, as the GPU runs the
   two draws in order. The aliasing draw reads [0x100, 0x200) and writes 0x33 over [0x800, 0x900), and its landing
   writes back only what it wrote */
#define UMA_AT  (HEAP_BASE + 0x4e00000)
#define UMA_GAT 0x4f5400000ull
static int check_uma_atlas_pending(void) {
    VkBuffer w, rb;
    u64 wo;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    const int was = g_settings.guest_buffers_copied;
    g_settings.guest_buffers_copied = 2; /* the atlas: in place is check_uma_alias_in_place's */
    if (vk_uma_storage_copied()) { /* a discrete GPU with host imports: there the earlier write isn't in place */
        g_settings.guest_buffers_copied = was;
        printf("skip: storage written by draws is kept in the GPU's memory on this device (vk_uma_storage_copied)\n");
        return 0;
    }
    CHECK(mem_map(UMA_AT, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GAT, UMA_AT, 0x200000);
    memset(GP(UMA_AT), 0x31, 0x1000);
    vk_lock();
    u8* result = vk_readback_buffer(0x1000, &rb);
    CHECK(result);
    CHECK(vk_buf_copy_writable(UMA_GAT + 0x100, 256, &w, &wo, 1));
    const int inplace = vk_uma_imports(UMA_AT, 0x200000) == 1 && !vk_buf_pending(UMA_GAT + 0x100, 256);
    CHECK(!owned_test_fill(w, wo, 256, 0x32)); /* the earlier draw's write */
    vk_buf_writable_commit();
    const int pending = vk_uma_pending(UMA_GAT + 0x100, 256);
    VkDrawStorage draw = {0};
    draw.n[ST_VS] = 1;
    draw.b[ST_VS][0] = (VkDrawGbuf){.addr = UMA_GAT + 0x100, .size = 256};
    draw.n[ST_FS] = 1;
    draw.b[ST_FS][0] = (VkDrawGbuf){.addr = UMA_GAT + 0x800, .size = 256, .writable = 1};
    CHECK(vk_buf_draw_has_alias(&draw) == 1);
    CHECK(vk_buf_draw_prepare(&draw));
    const VkDrawGbuf* in = &draw.b[ST_VS][0];
    const VkDrawGbuf* out = &draw.b[ST_FS][0];
    CHECK(in->aliased && out->aliased && in->buf == out->buf && in->off == out->off);
    /* the atlas's one page, as the draw finds it; then the draw's write into it */
    VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                          .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                          .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
                          .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
    rec_CmdPipelineBarrier2(vk_cmd(), &d);
    VkBufferCopy page = {in->off, 0, 0x1000};
    rec_CmdCopyBuffer(vk_cmd(), in->buf, rb, 1, &page);
    CHECK(!owned_test_fill(out->buf, out->off + 0x800, 256, 0x33));
    vk_buf_writable_commit();
    vk_finish();
    vk_readback_ready();
    u32 stale = 0;
    for (u32 i = 0; i < 0x1000; i++)
        if (result[i] != (i >= 0x100 && i < 0x200 ? 0x32 : 0x31)) stale++;
    vk_unlock();
    u32 wrong = 0;
    for (u32 i = 0; i < 0x1000; i++)
        if (GP(UMA_AT)[i] != (i >= 0x100 && i < 0x200 ? 0x32 : i >= 0x800 && i < 0x900 ? 0x33 : 0x31)) wrong++;
    CHECK(mem_unmap(UMA_AT, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    g_settings.guest_buffers_copied = was;
    if (!inplace || !pending || stale || wrong)
        fprintf(stderr,
                "aliased draw: the earlier write in place %d and pending %d; the atlas had %u bytes from before "
                "it (atlas[0x100] %02x), guest memory %u wrong bytes after the landing\n",
                inplace, pending, stale, result[0x100], wrong);
    CHECK(inplace && pending && !stale && !wrong);
    printf(
        "in place: an aliasing draw's atlas has a write in place of the same submission, and its landing keeps it\n");
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
    all_samples(r);
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

/* A video picture over memory an import holds is copied there on the GPU (vk_uma_publish), neither owned nor trapped:
   the picture is in guest memory when its write returns, as the caller steps a syncpoint then that the guest reads it
   after (host1x.c's run_job, native_video.c's codec_output). The wait for it lets go of vk_lock: the submission waits
   for a semaphore that another thread signals once it held the lock after the submission was made, or after 5 s without
   it, so the wait cannot end first. Before, the decoder's thread held the lock through it, and the recording of the
   frame waited for each picture of a video */
#define UMA_NV  (HEAP_BASE + 0x4600000)
#define UMA_GNV 0x4e8000000ull
static struct {
    _Atomic int got, done;
    _Atomic u64 before; /* the submission before the picture's */
    VkSemaphore gate;
} g_nv;
static void uma_nvdec_thread(void* p) {
    (void)p;
    const u64 end = plat_ns() + 5000000000ull;
    while (plat_ns() < end && !atomic_load_explicit(&g_nv.got, memory_order_relaxed)) {
        if (vk_trylock()) {
            if (vk_last_submit() > atomic_load_explicit(&g_nv.before, memory_order_relaxed))
                atomic_store_explicit(&g_nv.got, 1, memory_order_release);
            vk_unlock();
        }
        plat_sleep_ns(100000);
    }
    VkSemaphoreSignalInfo signal = {
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, .semaphore = g_nv.gate, .value = 1};
    VK_OK(vkSignalSemaphore(g_vk.dev, &signal));
    atomic_store_explicit(&g_nv.done, 1, memory_order_release);
}
static int check_uma_nvdec(void) {
    enum { W = 64, H = 32, P = 256 };
    static u8 y[W * H], u[W / 2 * (H / 2)], v[W / 2 * (H / 2)];
    VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                      .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
    VkBuffer b;
    u64 o;
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_NV, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GNV, UMA_NV, 0x200000);
    memset(GP(UMA_NV), 0x11, 0x10000);
    for (u32 i = 0; i < W * H; i++) y[i] = (u8)(i * 7 + 3);
    for (u32 i = 0; i < sizeof u; i++) u[i] = (u8)(i * 3 + 1), v[i] = (u8)(i * 5 + 2);
    CHECK(VK_OK(vkCreateSemaphore(g_vk.dev, &ci, NULL, &g_nv.gate)));
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GNV, 0x10000, 4, &b, &o)); /* a draw reads it in place: an import holds it */
    int held = vk_uma_held(UMA_NV, 0x10000);
    vk_finish();
    vk_submit_after(g_nv.gate, 1, 2); /* the next submission, the picture's, waits for the gate */
    atomic_store_explicit(&g_nv.before, vk_last_submit(), memory_order_relaxed);
    vk_unlock();
    CHECK(!plat_thread(uma_nvdec_thread, NULL, 1u << 16));
    NvdecPic p = {{y, u, v}, {W, W / 2, W / 2}, W, H, UMA_NV, UMA_NV + 0x4000, P * H, P * (H / 2), P, P, H, 0};
    p.pitch_linear = 1;
    CHECK(g_renderer->nvdec_write(&p));
    int there = 1; /* as a guest load sees it */
    for (u32 j = 0; j < H && there; j++) there = !memcmp(GP(UMA_NV) + j * P, y + j * W, W);
    for (u32 j = 0; j < H / 2 && there; j++)
        for (u32 i = 0; i < W / 2 && there; i++)
            there = GP(UMA_NV + 0x4000)[j * P + 2 * i] == u[j * (W / 2) + i] &&
                    GP(UMA_NV + 0x4000)[j * P + 2 * i + 1] == v[j * (W / 2) + i];
    int owned = own_lookup(UMA_NV, P * H, NULL, 0) != 0;
    while (!atomic_load_explicit(&g_nv.done, memory_order_acquire)) plat_sleep_ns(1000000);
    vk_lock();
    vk_finish();
    vk_unlock();
    vkDestroySemaphore(g_vk.dev, g_nv.gate, NULL);
    CHECK(mem_unmap(UMA_NV, 0x200000) == 0);
    const int got = atomic_load_explicit(&g_nv.got, memory_order_acquire);
    if (!held || owned || !there || !got)
        fprintf(stderr,
                "a video picture in place: held %d, owned %d, %s guest memory when its write returned, vk_lock %s\n",
                held, owned, there ? "in" : "not in", got ? "let go during its wait" : "not let go in 5 s of its wait");
    CHECK(held && !owned && there && got);
    printf("in place: a video picture is in guest memory when its write returns, vk_lock let go during its wait\n");
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

/* A dispatch with nothing staged for the host stays in the submission being recorded, and the end of the game's GPU
   submission (stream_end) submits it. A CPU read of its store while it is still unsubmitted waits for all that was
   recorded (a finish), and the program's dispatches go to the GPU at once from then on. The program stores 0x5eef
   through an NVN descriptor in cbuf 0, as check_uma_dispatch's, so that a program of its own is learned here */
static int check_uma_dispatch_open(void) {
    const u64 code = 0x4e1000000ull + 0x100, code_cpu = HEAP_BASE + 0x780000;
    const u64 cbuf = UMA_GQ + 0x1000, stored = UMA_GQ + 0x2000;
    const u64 program[] = {
        0x001f8000fc0007e0ull,  0x4c98078004470000ull, /* MOV R0, c[0][0x110]: the buffer's address, NVN's descriptor */
        0x4c98078004570001ull,                         /* MOV R1, c[0][0x114] */
        storage_mov(2, 0x5eef),                        /* MOV32I R2, 0x5eef */
        0x001f8000fc0007e0ull,  0xeedc200000070002ull, /* STG.E [R0], R2 */
        0xe30000000007000full};
    const u32 descriptor[3] = {(u32)stored, (u32)(stored >> 32), 16};
    u32 qmd[64] = {0};
    DispatchState d;
    u8 word[4];
    int left[3], words[3];
    u64 finishes[3];
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    CHECK(mem_map(UMA_Q, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GQ, UMA_Q, 0x200000);
    gmmu_map(0x4e1000000ull, code_cpu, 0x10000);
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
    for (int i = 0; i < 3; i++) {
        memset(GP(UMA_Q) + 0x2000, 0, 4);
        vk_lock();
        const u64 was_left = g_vk_cs_left;
        mx_dispatch_state(qmd, channel->rcomp, code, &d);
        vk_dispatch(&d);
        /* left in the submission being recorded (a periodic submit of earlier work may have come before it) */
        left[i] = g_vk_cs_left == was_left + 1 && g_vk_cs_open == vk_submit_number();
        vk_unlock();
        if (i == 0) g_renderer->stream_end(); /* the stream's end submits the first: its read makes no finish */
        finishes[i] = g_vk_finishes;
        CHECK(!gmem_read(stored, word, sizeof word));
        finishes[i] = g_vk_finishes - finishes[i];
        words[i] = snx_ld_le32(word) == 0x5eef;
    }
    free(channel);
    CHECK(mem_unmap(UMA_Q, 0x200000) == 0);
    if (!left[0] || !left[1] || left[2] || finishes[0] || finishes[1] != 1 || finishes[2] || !words[0] || !words[1] ||
        !words[2])
        fprintf(stderr,
                "dispatch left open: left %d %d %d (want 1 1 0), finishes at the read %llu %llu %llu (want 0 1 0), "
                "stored %d %d %d\n",
                left[0], left[1], left[2], (unsigned long long)finishes[0], (unsigned long long)finishes[1],
                (unsigned long long)finishes[2], words[0], words[1], words[2]);
    CHECK(left[0] && left[1] && !left[2] && !finishes[0] && finishes[1] == 1 && !finishes[2]);
    CHECK(words[0] && words[1] && words[2]);
    printf("in place: a dispatch waits for the stream's end, and a program read before then goes at once after\n");
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
    const u64 waits = vk_uma_out_waits();
    Surf* again = vk_tex_recheck(t);
    int waited = vk_uma_out_waits() != waits;
    u8* texels = vk_readback_buffer(256, &rb);
    CHECK(again == t && texels);
    vk_surf_use(vk_cmd(), t, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {64, 1, 1}};
    rec_CmdCopyImageToBuffer(vk_cmd(), t->img, VK_LAYOUT(TRANSFER_SRC_OPTIMAL), rb, 1, &copy);
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
    const u64 waits = vk_uma_out_waits();
    mx_dispatch_state(qmd, channel->rcomp, code, &d);
    vk_dispatch(&d);
    int ran = g_vk_dispatches == dispatches + 1, waited = vk_uma_out_waits() != waits;
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

/* A compute program's pointer table (SH_GBUF_TABLE) in a constant buffer an engine's result is being written to in
   place: the windows the dispatch binds are made from the table's words (compute.c's table_window), so the bank is
   read after the write, as a word a translation depends on is. Made from what the last dispatch left of that bank,
   the windows were an older table's, and the program's stores through the pointers it loads went nowhere. An SM86
   program loads the pointer at c[4][0x20] with an LDC.64 at a register's offset and stores 0x5eed 8 bytes on: first
   with no pointer in the table, then with one the pending write puts there. Where guest memory is copied, the CPU
   writes the pointer and no write is pending */
#define UMA_TB  (HEAP_BASE + 0xd600000)
#define UMA_GTB 0x4d6000000ull
static int check_uma_dispatch_table(void) {
    const u64 table = UMA_GTB + 0x3000, result = UMA_GTB + 0x10000, code = UMA_GTB + 0x20000,
              pointer = UMA_GTB + 0x40100;
    u8 program[16 * 6] = {0}, word[4];
    u32 qmd[64] = {0}, n = 0;
    DispatchState d;
    VkBuffer r;
    u64 ro;
    int held = 1, pending = 1;
    Sm86Word x = sm86_fix(0xb82); /* LDC.64 R2, c[4][R0] */
    sm86_set(&x, 16, 8, 2);
    sm86_set(&x, 24, 8, 0);
    sm86_cbuf(&x, 4, 0);
    sm86_set(&x, 73, 3, 5);
    sm86_put(program, n++, sm86_mov_imm(0, 0x20));
    sm86_put(program, n++, x);
    sm86_put(program, n++, sm86_mov_imm(5, 0x5eed));
    x = sm86_fix(0x386); /* STG.E [R2:R3 + 8], R5 */
    sm86_set(&x, 24, 8, 2);
    sm86_set(&x, 32, 8, 5);
    sm86_set(&x, 40, 24, 8);
    sm86_set(&x, 72, 1, 1);
    sm86_set(&x, 73, 3, 4);
    sm86_put(program, n++, x);
    sm86_put(program, n++, sm86_exit());
    sm86_put(program, n, sm86_bra(n, n, 7, 0));
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    CHECK(mem_map(UMA_TB, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GTB, UMA_TB, 0x200000);
    memset(GP(UMA_TB), 0, 0x50000);
    CHECK(!gmem_write(code, program, sizeof program));
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1u << 4;
    qmd[QMD_CONST_BUFFER_CONFIG + 4 * QMD_CONST_BUFFER_CONFIG_WORDS] = (u32)table;
    qmd[QMD_CONST_BUFFER_CONFIG + 4 * QMD_CONST_BUFFER_CONFIG_WORDS + 1] = (u32)(table >> 32) | 0x100u << 15;
    int sm86 = g_shader_sm86;
    g_shader_sm86 = 1;
    u64 dispatches = g_vk_dispatches;
    vk_lock();
    mx_dispatch_state(qmd, channel->rcomp, code, &d);
    vk_dispatch(&d); /* the table with no pointer: the bank as this dispatch read it */
    vk_compute_land();
    vk_unlock();
    if (vk_uma()) {
        snx_st_le64(GP(UMA_TB) + 0x10020, pointer); /* the engine's result: the table with its pointer */
        vk_lock();
        CHECK(vk_buf_get_input(table, 256, 4, &r, &ro)); /* a draw reads it in place: an import holds it */
        held = vk_uma_held(UMA_TB + 0x3000, 256);
        vk_unlock();
        CHECK(!uma_dma(table, result)); /* written in place by a submit not made yet */
    } else {
        u8 bytes[8];
        snx_st_le64(bytes, pointer);
        CHECK(!gmem_write(table + 0x20, bytes, sizeof bytes));
        g_renderer->mem_may_have_changed();
    }
    vk_lock();
    if (vk_uma()) pending = vk_uma_pending(table, 256);
    mx_dispatch_state(qmd, channel->rcomp, code, &d);
    vk_dispatch(&d);
    int ran = g_vk_dispatches == dispatches + 2;
    vk_unlock();
    g_shader_sm86 = sm86;
    /* the store, needed where it went as a reader of that memory needs it: the compute queue's batch, or the graphics
       queue's results (the GPU mirror's, the writes in place) */
    g_renderer->cs_mem_need(pointer, 16);
    g_renderer->gfx_mem_need(pointer, 16);
    CHECK(!gmem_read(pointer + 8, word, sizeof word));
    free(channel);
    CHECK(mem_unmap(UMA_TB, 0x200000) == 0);
    if (!held || !pending || !ran || snx_ld_le32(word) != 0x5eed)
        fprintf(stderr,
                "dispatch table: held %d, pending %d, ran %d, the word 8 bytes past the table's pointer holds %08x "
                "(0x5eed wanted)\n",
                held, pending, ran, snx_ld_le32(word));
    CHECK(held && pending && ran && snx_ld_le32(word) == 0x5eed);
    printf("%s: a dispatch's pointer table %s binds the windows of its pointers, and a store through one lands\n",
           vk_uma() ? "in place" : "copied", vk_uma() ? "under a pending write in place" : "the CPU wrote");
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
    const u64 waits = vk_uma_out_waits();
    memcpy(now, GP(UMA_J) + 0x20, sizeof now);
    vk_gmem_words(UMA_GJ + 0x20, words, 8);
    int waited = vk_uma_out_waits() != waits, same = 1;
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
   a device, and 1 copies everywhere. uma_memory picks between the two ways where guest memory is in place: 1 only
   imports, so it copies where 0 would move, and 2 only moves, on a driver that imports and under auto as well; neither
   puts guest memory in place where vk_memory copies */
static int check_uma_way(void) {
    CHECK(vk_uma_way(0, 0, VK_MEM_UMA, 0, 1, 0, 0, 0) == VK_UMA_WITH_NONE);
    CHECK(vk_uma_way(2, 0, VK_MEM_UMA, 0, 1, 0, 0, 0) == VK_UMA_WITH_AHB);
    CHECK(vk_uma_way(0, 0, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_HOST &&
          vk_uma_way(2, 0, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_HOST);
    CHECK(vk_uma_way(0, 0, VK_MEM_REBAR, 1, 1, 0, 0, 0) == VK_UMA_WITH_NONE &&
          vk_uma_way(2, 0, VK_MEM_REBAR, 1, 0, 0, 0, 0) == VK_UMA_WITH_HOST);
    CHECK(vk_uma_way(1, 0, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_NONE &&
          vk_uma_way(2, 0, VK_MEM_UMA, 0, 0, 0, 0, 0) == VK_UMA_WITH_NONE);
    /* imported only */
    CHECK(vk_uma_way(2, 1, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_HOST &&
          vk_uma_way(0, 1, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_HOST);
    CHECK(vk_uma_way(2, 1, VK_MEM_UMA, 0, 1, 0, 0, 0) == VK_UMA_WITH_NONE);
    /* moved only */
    CHECK(vk_uma_way(2, 2, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_AHB &&
          vk_uma_way(0, 2, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_AHB);
    CHECK(vk_uma_way(0, 2, VK_MEM_UMA, 0, 1, 0, 0, 0) == VK_UMA_WITH_AHB &&
          vk_uma_way(2, 2, VK_MEM_REBAR, 1, 1, 0, 0, 0) == VK_UMA_WITH_AHB);
    CHECK(vk_uma_way(2, 2, VK_MEM_UMA, 1, 0, 0, 0, 0) == VK_UMA_WITH_NONE);
    /* Windows' discrete GPU where the mirror can be (host_discrete): auto imports there, as vk_memory=2 does; moved only
       and copied are as before, and without the mirror auto copies */
    CHECK(vk_uma_way(0, 0, VK_MEM_REBAR, 1, 0, 0, 0, 1) == VK_UMA_WITH_HOST &&
          vk_uma_way(0, 1, VK_MEM_REBAR, 1, 0, 0, 0, 1) == VK_UMA_WITH_HOST);
    CHECK(vk_uma_way(0, 0, VK_MEM_REBAR, 0, 0, 0, 0, 1) == VK_UMA_WITH_NONE &&
          vk_uma_way(1, 0, VK_MEM_REBAR, 1, 0, 0, 0, 1) == VK_UMA_WITH_NONE &&
          vk_uma_way(0, 2, VK_MEM_REBAR, 1, 0, 0, 0, 1) == VK_UMA_WITH_NONE &&
          vk_uma_way(0, 0, VK_MEM_REBAR, 1, 0, 0, 0, 0) == VK_UMA_WITH_NONE);
    /* Linux's dma-bufs of memfds (udmabuf): auto moves into them where imports are dear (amdgpu), else imports as before;
       imported only never moves, moved only takes them where there are no AHardwareBuffers */
    CHECK(vk_uma_way(0, 0, VK_MEM_UMA, 1, 0, 1, 1, 0) == VK_UMA_WITH_DMABUF &&
          vk_uma_way(2, 0, VK_MEM_UMA, 1, 0, 1, 1, 0) == VK_UMA_WITH_DMABUF);
    CHECK(vk_uma_way(0, 0, VK_MEM_UMA, 1, 0, 1, 0, 0) == VK_UMA_WITH_HOST &&
          vk_uma_way(0, 0, VK_MEM_UMA, 1, 0, 0, 1, 0) == VK_UMA_WITH_HOST);
    CHECK(vk_uma_way(0, 1, VK_MEM_UMA, 1, 0, 1, 1, 0) == VK_UMA_WITH_HOST &&
          vk_uma_way(0, 1, VK_MEM_UMA, 0, 0, 1, 1, 0) == VK_UMA_WITH_NONE);
    CHECK(vk_uma_way(0, 2, VK_MEM_UMA, 1, 0, 1, 0, 0) == VK_UMA_WITH_DMABUF &&
          vk_uma_way(0, 2, VK_MEM_UMA, 1, 1, 1, 1, 0) == VK_UMA_WITH_AHB);
    CHECK(vk_uma_way(1, 0, VK_MEM_UMA, 1, 0, 1, 1, 0) == VK_UMA_WITH_NONE);
    /* a discrete GPU: auto moves into dma-bufs where host imports are dear, and copies where they are not, where it has
       no udmabuf, or where uma_memory imports only; never a host pointer, which amdgpu takes again at each submission */
    CHECK(vk_uma_way(0, 0, VK_MEM_REBAR, 1, 0, 1, 1, 0) == VK_UMA_WITH_DMABUF &&
          vk_uma_way(0, 0, VK_MEM_SMALL_BAR, 1, 0, 1, 1, 0) == VK_UMA_WITH_DMABUF &&
          vk_uma_way(0, 2, VK_MEM_REBAR, 1, 0, 1, 1, 0) == VK_UMA_WITH_DMABUF);
    CHECK(vk_uma_way(0, 0, VK_MEM_REBAR, 1, 0, 1, 0, 0) == VK_UMA_WITH_NONE &&
          vk_uma_way(0, 0, VK_MEM_REBAR, 1, 0, 0, 1, 0) == VK_UMA_WITH_NONE &&
          vk_uma_way(0, 1, VK_MEM_REBAR, 1, 0, 1, 1, 0) == VK_UMA_WITH_NONE);
    /* vk_memory's copy stays a copy */
    CHECK(vk_uma_way(1, 1, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_NONE &&
          vk_uma_way(1, 2, VK_MEM_UMA, 1, 1, 0, 0, 0) == VK_UMA_WITH_NONE &&
          vk_uma_way(0, 2, VK_MEM_REBAR, 1, 1, 0, 0, 0) == VK_UMA_WITH_NONE);
    printf("in place: auto copies where only AHardwareBuffers would take guest memory; vk_memory=2 moves it there; "
           "uma_memory=1 only imports, uma_memory=2 only moves\n");
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
   doesn't (its user pointers take anonymous memory only). None where the device was made without
   VK_EXT_external_memory_host (the dma-buf way on amdgpu with uma_copies=0), whose function volk leaves NULL */
static int uma_imports_shared(void) {
    const u64 size = 0x200000;
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VkDeviceMemory mem;
    void* view;
    int fd, ok = 0;
    if (!vkGetMemoryHostPointerPropertiesEXT) return 0;
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
/* a draw's start whose frame check submits what was recorded (vk_submit_every at 1). A region's reads recorded
   after the last frame check keep it until a later submission is done, which settles them (uma.c's last_read): the
   checks below free a region at the present after its readers, so their reads are submitted at the draw before */
static void uma_draw_submits(void) {
    const int every = g_settings.submit_every;
    g_settings.submit_every = 1;
    vk_frame_check();
    g_settings.submit_every = every;
}
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
    /* regions the driver imports as host memory, else as dma-bufs (Linux's udmabuf: amdgpu takes no memfd's pages as
       host memory) */
    const int way = uma_imports_shared() ? 1 : vk_uma_can_dmabuf() ? 2 : 0;
    if (!way) {
        printf("skip: the driver imports no buffer's pages as host memory or as a dma-buf, which the migrate way "
               "needs here\n");
        return 0;
    }
    CHECK(mem_map(UMA_M, 4 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GM, UMA_M, 4 * window);
    u8* g = GP(UMA_M);
    for (u64 i = 0; i < 4 * window; i++) g[i] = uma_fill(i);
    vk_lock();
    CHECK(vk_uma_migrate(way));
    u8* result = vk_readback_buffer(768, &rb);
    CHECK(result);
    /* the fill taken by write tracking, in an epoch of its own: from here a page reads as written only by a write */
    int watched = 0;
    u32 quiet = 0;
    if (way == 2) {
        vk_unlock();
        g_renderer->mem_may_have_changed();
        vk_lock();
        watched = vk_written_since(g, 4 * window, 0) >= 0;
        quiet = vk_ww_now();
    }
    /* the first window moves, with its bytes; a range beside it is in the same region */
    CHECK(vk_uma_get(UMA_GM + 0x1000, 256, 1, &b, &off) && off == 0x1000);
    CHECK(vk_uma_imports(UMA_M, 4 * window) == 1 && vk_uma_imports(UMA_M + window, 3 * window) == 0);
    CHECK(uma_in_buffer(g) == 1 && uma_in_buffer(g + window) == 0);
    CHECK(vk_uma_get(UMA_GM + 0x100000, 256, 1, &b2, &off2) && b2 == b && off2 == 0x100000);
    /* a region made is no write: marked written whole by the move, a render target drawn there before, its pixels not
       yet in memory, read as older than memory, and a texture over it loaded memory's bytes */
    if (watched) {
        vk_unlock();
        g_renderer->mem_may_have_changed();
        vk_lock();
        CHECK(vk_written_since(g + 0x4000, 0x4000, quiet) == 0);
    }
    /* a dma-buf region's pages are watched for writes as the rest of guest memory (the driver leaves the CPU's page
       table alone): quiet, then a CPU write found. A host-memory region's aren't (vk_written_since -1) */
    if (way == 2 && vk_written_since(g + 0x4000, 0x4000, 0) >= 0) {
        u32 since = vk_ww_now();
        vk_unlock();
        g_renderer->mem_may_have_changed();
        vk_lock();
        CHECK(vk_written_since(g + 0x4000, 0x4000, since) == 0);
        vk_unlock();
        g[0x5000] = uma_fill(0x5000); /* a write of the byte it holds: a write all the same */
        g_renderer->mem_may_have_changed();
        vk_lock();
        CHECK(vk_written_since(g + 0x4000, 0x4000, since) == 1);
    } else /* a dma-buf region where tracking is on (a page outside any region answers) is watched */
        CHECK(way != 2 || vk_written_since(g + 3 * window, 0x1000, 0) < 0);
    g[0x1000] = 0x5a; /* after the lookup, before the GPU reads it */
    uma_read(b, off, rb, 0);
    /* over the edge: one region of both windows, the first one's going with its last reader */
    CHECK(vk_uma_get(UMA_GM + window - 0x80, 256, 1, &b2, &off2) && b2 != b && off2 == window - 0x80);
    CHECK(uma_in_buffer(g + window) == 1 && uma_in_buffer(g + 2 * window) == 0);
    uma_read(b2, off2, rb, 256);
    uma_read(b2, 0x1000, rb, 512);
    uma_draw_submits(); /* a draw begins: the copies are all recorded, and its frame check submits them */
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
    /* nor is a region gone, or moved on into a bigger one: a page of the first window that no one wrote */
    if (watched) {
        vk_unlock();
        g_renderer->mem_may_have_changed();
        vk_lock();
        CHECK(vk_written_since(g + 0x8000, 0x1000, quiet) == 0);
    }
    /* a range of a region the GPU owns next (a result published over it): nothing waits for the copy recorded before,
       which reads the bytes from before, and the region goes, its other pages back with their bytes */
    if (own_traps()) {
        const u64 at = 2 * window + 0x100;
        CHECK(vk_uma_get(UMA_GM + at, 256, 1, &b, &off) && uma_in_buffer(g + 2 * window) == 1);
        uma_read(b, off, rb, 0);
        uma_draw_submits();
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
    /* regions the driver imports as host memory, else as dma-bufs (Linux's udmabuf: amdgpu takes no memfd's pages as
       host memory) */
    const int way = uma_imports_shared() ? 1 : vk_uma_can_dmabuf() ? 2 : 0;
    if (!way) {
        printf("skip: the driver imports no buffer's pages as host memory or as a dma-buf, which the migrate way "
               "needs here\n");
        return 0;
    }
    CHECK(mem_map(UMA_RW, 3 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GRW, UMA_RW, 3 * window);
    u8* g = GP(UMA_RW);
    for (u32 i = 0; i < 3; i++) memset(g + i * window, 0x91, 0x6000);
    vk_lock();
    CHECK(vk_uma_migrate(way));
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
    CHECK(vk_uma_migrate(way));
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
    /* regions the driver imports as host memory, else as dma-bufs (Linux's udmabuf: amdgpu takes no memfd's pages as
       host memory) */
    const int way = uma_imports_shared() ? 1 : vk_uma_can_dmabuf() ? 2 : 0;
    if (!way) {
        printf("skip: the driver imports no buffer's pages as host memory or as a dma-buf, which the migrate way "
               "needs here\n");
        return 0;
    }
    CHECK(mem_map(UMA_RM, 2 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GRM, UMA_RM, 2 * window);
    u8* g = GP(UMA_RM);
    memset(g, 0x91, 0x1000);
    memset(g + window - 0x1000, 0x91, 0x2000);
    vk_lock();
    CHECK(vk_uma_migrate(way));
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
/* A region goes whole when any page of it is trapped or changes, its other pages moving back with the bytes its buffer
   holds then: the writes in place pending anywhere in it land first, not only those over the pages that change. A
   game's cube of render targets lay over the edge of two regions: at a release each target a region held was
   published there in place, the one over the edge was trapped, both regions went before the submit ran, and the
   cube loaded the bytes from before its targets from memory */
#define UMA_RT  (HEAP_BASE + 0x26800000)
#define UMA_GRT 0x4fa000000ull
static int check_uma_region_trap(void) {
    const u64 window = 0x200000;
    VkBuffer b;
    u64 bo;
#if defined(__ANDROID__)
    printf("skip: the migrate way runs here as the device chose it (vk_memory), not from this check\n");
    return 0;
#endif
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    const int way = uma_imports_shared() ? 1 : vk_uma_can_dmabuf() ? 2 : 0;
    if (!way) {
        printf("skip: the driver imports no buffer's pages as host memory or as a dma-buf, which the migrate way "
               "needs here\n");
        return 0;
    }
    CHECK(mem_map(UMA_RT, 3 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GRT, UMA_RT, 3 * window);
    u8* g = GP(UMA_RT);
    memset(g, 0x91, 0x1000);
    memset(g + window - 0x1000, 0x91, 0x4000);
    memset(g + 2 * window, 0x91, 0x4000);
    vk_lock();
    CHECK(vk_uma_migrate(way));
    /* a region of each of two windows, and an engine's result published in place in each, pending */
    CHECK(vk_buf_get_input(UMA_GRT + 0x100, 256, 4, &b, &bo));
    CHECK(vk_buf_get_input(UMA_GRT + window + 0x2100, 256, 4, &b, &bo));
    int regions = vk_uma_imports(UMA_RT, 2 * window) == 2 && uma_in_buffer(g) == 1 && uma_in_buffer(g + window) == 1;
    VkOwnRes *first = uma_owner(256, 0x96), *second = uma_owner(256, 0x97), *edge = uma_owner(256, 0x42);
    CHECK(first && second && edge);
    vk_own_publish(UMA_RT + 0x100, 256, first, 0);
    vk_own_publish(UMA_RT + window + 0x2100, 256, second, 0);
    int pending = vk_uma_pending_cpu(UMA_RT + 0x100, 256) && vk_uma_pending_cpu(UMA_RT + window + 0x2100, 256);
    /* a result over the edge between them, which no one region holds: GPU-owned, a trap on a page of each. What asks
       about the regions next (a release's next target) lets them go, before the submit */
    vk_own_publish(UMA_RT + window - 0x80, 256, edge, 0);
    int gone = !own_traps() || (!vk_uma_held(UMA_RT + 0x100, 256) && !vk_uma_held(UMA_RT + window + 0x2100, 256));
    vk_finish();
    vk_unlock();
    CHECK(!uma_landed(UMA_RT + window - 0x80, 256, 0x42));
    CHECK(g[0x100] == 0x96 && g[0x1ff] == 0x96 && g[0xff] == 0x91 && g[0x200] == 0x91);
    CHECK(g[window + 0x2100] == 0x97 && g[window + 0x21ff] == 0x97 && g[window + 0x20ff] == 0x91);
    vk_own_res_unref(first);
    vk_own_res_unref(second);
    vk_own_res_unref(edge);
    /* a result pending in a third window's region, another page of which a view then takes (mem_alias_begin moves it
       into the pool) */
    u8* g3 = g + 2 * window;
    vk_lock();
    CHECK(vk_buf_get_input(UMA_GRT + 2 * window + 0x100, 256, 4, &b, &bo));
    int region3 = vk_uma_imports(UMA_RT + 2 * window, window) == 1 && uma_in_buffer(g3) == 1;
    VkOwnRes* third = uma_owner(256, 0x98);
    CHECK(third);
    vk_own_publish(UMA_RT + 2 * window + 0x100, 256, third, 0);
    int pending3 = vk_uma_pending_cpu(UMA_RT + 2 * window + 0x100, 256);
    vk_unlock();
    CHECK(mem_alias_begin(UMA_RT + 2 * window + 0x3000, 0x1000) == 0);
    CHECK(g3[0x100] == 0x98 && g3[0x1ff] == 0x98 && g3[0xff] == 0x91 && g3[0x200] == 0x91);
    mem_alias_end(UMA_RT + 2 * window + 0x3000, 0x1000);
    vk_own_res_unref(third);
    vk_lock();
    CHECK(vk_uma_migrate(0));
    vk_finish();
    vk_unlock();
    CHECK(g[0x100] == 0x96 && g[window + 0x2100] == 0x97 && g3[0x100] == 0x98);
    CHECK(mem_unmap(UMA_RT, 3 * window) == 0);
    if (!regions || !pending || !gone || !region3 || !pending3)
        fprintf(stderr,
                "region trap: two regions %d with a result pending in each %d, gone at the trap %d, a third %d with "
                "one %d\n",
                regions, pending, gone, region3, pending3);
    CHECK(regions && pending && gone && region3 && pending3);
    printf("in place, the migrate way: a trap or a view on a page of a region lands the writes in place pending in "
           "the rest of it first\n");
    return 0;
}

/* A logged result the in-place path refuses, its range an import's in part and an owner's in part (buffer.c's
   back_record): written back on the GPU after its writer, the bytes its log gives and no others, into the import's
   part of guest memory, into the owner, and into the page neither held (an import made there, or an owner made for
   it from memory where the driver takes no import with a trapped page in its window). Nothing is left pending, so
   reading the range lands nothing */
#define UMA_LB  (HEAP_BASE + 0x7000000)
#define UMA_GLB 0x4e9000000ull
/* part 0 of log: runs [from[k], from[k] + n[k]) of a binding of windows windows written, as the GPU logs them: every
   window's flag and mask, each copied once (logged_mark gives one run a window) */
static int logged_marks(VkWriteLog* log, const u32* from, const u32* n, u32 runs, u32 windows) {
    static u32 masks[4][SH_WLOG_MASK];
    CHECK(windows <= 4);
    memset(masks, 0, sizeof masks);
    for (u32 k = 0; k < runs; k++)
        for (u32 b = from[k]; b < from[k] + n[k]; b++) masks[b / 4096][b % 4096 / 32] |= 1u << (b % 32);
    for (u32 w = 0; w < windows; w++) {
        VkBuffer src;
        u64 at;
        u8* data = vk_stage_upload(16 + sizeof masks[w], 16, &src, &at);
        CHECK(data);
        u32 flag = 0;
        for (u32 i = 0; i < SH_WLOG_MASK; i++) flag |= masks[w][i] != 0;
        memcpy(data, &flag, 4);
        memcpy(data + 16, masks[w], sizeof masks[w]);
        VkBufferCopy copy[3] = {{at, (u64)w * 4, 4},
                                {at + 16, ((u64)SH_WPAGE_SLOT + (u64)w * SH_WLOG_MASK) * 4, sizeof masks[w]},
                                {at, ((u64)SH_WPAGE_SLOT * (1u + SH_WLOG_MASK) + w / 64) * 4, 4}};
        /* the coarse word only from a window written: an unwritten one's 0 would clear another's */
        rec_CmdCopyBuffer(vk_cmd(), src, vk_wlog_buffer(log), flag ? 3 : 2, copy);
    }
    return 0;
}
static int check_uma_logged_back(void) {
    const u64 window = 0x200000, edge = UMA_LB + window, page = 0x1000;
    /* the binding: its first 0xfe0 bytes the import's, then the owner's page, then a page neither holds */
    const u64 cpu = edge - 0xfe0, gva = UMA_GLB + (cpu - UMA_LB);
    const u32 len = 0xfe0 + 2 * page;
    /* written by the program: [from, from + n) of the binding, of which logged bytes are logged */
    static const u32 from[5] = {0x20, 0xfd0, 0x1100, 0x1801, 0x2400}, n[5] = {0x40, 0x30, 0x10, 0x5, 0x20},
                     logged[5] = {0x40, 0x30, 0x8, 0x5, 0x20};
    static const u8 value[5] = {0xa1, 0xa2, 0xa3, 0xa4, 0xa5};
    static u8 want[0xfe0 + 2 * 0x1000];
    VkBuffer b, out;
    u64 bo, at;
    if (uma_skip()) return 0;
    CHECK(mem_map(UMA_LB, 2 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GLB, UMA_LB, 2 * window);
    for (u32 i = 0; i < 2 * window; i++) GP(UMA_LB)[i] = (u8)(i * 13 + 9);
    vk_lock();
    u32 validation = g_vk_validation_messages;
    CHECK(vk_buf_get_input(UMA_GLB + 0x100, 256, 4, &b, &bo) && vk_uma_imports(UMA_LB, window) == 1);
    VkOwnRes* owner = uma_owner(page, 0x63);
    CHECK(owner);
    vk_own_publish_owned(edge, page, owner, 0);
    for (u32 i = 0; i < len; i++) want[i] = i >= 0xfe0 && i < 0xfe0 + page ? 0x63 : GP(cpu)[i];
    VkWriteLog* log = vk_wlog_get(1);
    CHECK(log);
    CHECK(vk_buf_copy_writable_logged(gva, len, &out, &at, 1, log, 0, 1));
    for (u32 k = 0; k < 5; k++) { /* the program's stores, recorded before its commit as a dispatch's */
        CHECK(!owned_test_fill(out, at + from[k], n[k], value[k]));
        for (u32 i = from[k]; i < from[k] + logged[k]; i++) want[i] = value[k];
    }
    CHECK(!logged_marks(log, from, logged, 5, 3)); /* and their marks */
    vk_buf_writable_commit();
    vk_buf_log_retire(log);
    CHECK(!vk_buf_pending(gva, len));
    vk_finish();
    vk_unlock();
    own_acquire(cpu, len);
    for (u32 i = 0; i < len; i++)
        if (GP(cpu)[i] != want[i]) {
            fprintf(stderr, "vulkan_headless: a logged result written back: byte %#x is %#x, not %#x\n", i, GP(cpu)[i],
                    want[i]);
            return 1;
        }
    CHECK(GP(cpu)[-1] == (u8)((cpu - 1 - UMA_LB) * 13 + 9) && GP(cpu)[len] == (u8)((cpu + len - UMA_LB) * 13 + 9));
    CHECK(g_vk_validation_messages == validation);
    vk_own_res_unref(owner);
    CHECK(mem_unmap(UMA_LB, 2 * window) == 0);
    printf("in place: a logged result over an import and an owner is written back on the GPU, its logged bytes "
           "alone\n");
    return 0;
}

/* ------------------------------------------------------------ aliasing draws in place (buffer.c's alias_in_place) */
/* the aliasing draws counted so far (vk_buf_alias_counts) */
typedef struct {
    u64 atlas, pages, inplace, why[VK_ALIAS_N];
} AliasCounts;
static AliasCounts alias_counts(void) {
    AliasCounts c;
    vk_buf_alias_counts(&c.atlas, &c.pages, &c.inplace, c.why);
    return c;
}
/* a vertex stage that reads [read, read + 256) and a fragment stage that writes [write, write + 256) */
static VkDrawStorage alias_draw(u64 read, u64 write) {
    VkDrawStorage draw = {0};
    draw.n[ST_VS] = 1;
    draw.b[ST_VS][0] = (VkDrawGbuf){.addr = read, .size = 256};
    draw.n[ST_FS] = 1;
    draw.b[ST_FS][0] = (VkDrawGbuf){.addr = write, .size = 256, .writable = 1};
    return draw;
}

/* An aliasing draw with unified memory binds each of its bindings where it is in guest memory: the vertex stage's read
   at 0x100 and the fragment stage's write at 0x800 of one page are one region's buffer, 0x700 apart, with no page map
   (the shader's no-map branch), no output to land and no atlas. The write is pending in place, and in guest memory once
   its submit is done */
#define UMA_AI  (HEAP_BASE + 0x30000000)
#define UMA_GAI 0xc00000000ull
static int check_uma_alias_in_place(void) {
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_AI, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GAI, UMA_AI, 0x200000);
    memset(GP(UMA_AI), 0x31, 0x1000);
    vk_lock();
    const AliasCounts before = alias_counts();
    VkDrawStorage draw = alias_draw(UMA_GAI + 0x100, UMA_GAI + 0x800);
    CHECK(vk_buf_draw_has_alias(&draw) == 1);
    CHECK(vk_buf_draw_prepare(&draw));
    const AliasCounts after = alias_counts();
    const VkDrawGbuf* in = &draw.b[ST_VS][0];
    const VkDrawGbuf* out = &draw.b[ST_FS][0];
    const int inplace = after.inplace == before.inplace + 1 && after.atlas == before.atlas;
    const int bound = in->aliased && out->aliased && in->buf == out->buf && out->off - in->off == 0x700 &&
                      in->range == 256 && out->range == 256;
    const int nomap = draw.map_size[ST_VS] == 48 * 4 && draw.map_size[ST_FS] == 48 * 4;
    const int nooutput = !vk_buf_pending(UMA_GAI, 0x1000);
    CHECK(!owned_test_fill(out->buf, out->off, 256, 0x33)); /* the draw's write */
    vk_buf_writable_commit();
    const int pending = vk_uma_pending(UMA_GAI + 0x800, 256);
    vk_finish();
    vk_unlock();
    u32 wrong = 0;
    for (u32 i = 0; i < 0x1000; i++) wrong += GP(UMA_AI)[i] != (i >= 0x800 && i < 0x900 ? 0x33 : 0x31);
    CHECK(mem_unmap(UMA_AI, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    if (!inplace || !bound || !nomap || !nooutput || !pending || wrong)
        fprintf(stderr,
                "aliasing draw in place: counted %d, bound %d (%p+%llx, %p+%llx), no map %d (%llu, %llu), no output "
                "%d, pending %d, %u wrong bytes\n",
                inplace, bound, (void*)in->buf, (unsigned long long)in->off, (void*)out->buf,
                (unsigned long long)out->off, nomap, (unsigned long long)draw.map_size[ST_VS],
                (unsigned long long)draw.map_size[ST_FS], nooutput, pending, wrong);
    CHECK(inplace && bound && nomap && nooutput && pending && !wrong);
    printf("in place: an aliasing draw binds each of its bindings at its own offset in guest memory, with no atlas\n");
    return 0;
}

/* An aliasing draw goes to the atlas whenever one of its group can't be in place, and leaves no write in place behind:
   the vertex stage writes page p (and reads it), the fragment stage writes page q (and reads it), the writes bound in
   that order. q the runtime wrote (front), or q under an earlier atlas's output (a result pending): the write on p was
   bound in place before q's was refused, and is undone. A binding over two runs of guest memory, one off the storage
   alignment, and a group whose guest memory has a hole (no one region) go there before any write is bound */
#define UMA_AF  (HEAP_BASE + 0x30400000)
#define UMA_GAF 0xc10000000ull
#define UMA_AE  (HEAP_BASE + 0x30800000) /* two pages of a window mapped, a hole between */
#define UMA_GAE 0xc20000000ull
static VkDrawStorage alias_pair(u64 p, u64 q) {
    VkDrawStorage draw = {0};
    draw.n[ST_VS] = 2;
    draw.b[ST_VS][0] = (VkDrawGbuf){.addr = p + 0x800, .size = 256, .writable = 1};
    draw.b[ST_VS][1] = (VkDrawGbuf){.addr = p + 0x100, .size = 256};
    draw.n[ST_FS] = 2;
    draw.b[ST_FS][0] = (VkDrawGbuf){.addr = q + 0x800, .size = 256, .writable = 1};
    draw.b[ST_FS][1] = (VkDrawGbuf){.addr = q + 0x100, .size = 256};
    return draw;
}
/* draw prepared (vk_lock held): 0 when it went to the atlas for why, and left nothing written in place over [p, +len) */
static int alias_fell_back(const char* name, VkDrawStorage* draw, int why, u64 p, u64 len) {
    const AliasCounts before = alias_counts();
    CHECK(vk_buf_draw_has_alias(draw) == 1);
    CHECK(vk_buf_draw_prepare(draw));
    const AliasCounts after = alias_counts();
    vk_buf_writable_commit(); /* as the draw recorded */
    const int stray = vk_uma_pending(p, len);
    const int atlas = after.atlas == before.atlas + 1 && after.inplace == before.inplace &&
                      after.why[why] == before.why[why] + 1 && draw->map_size[ST_VS] > 48 * 4;
    vk_finish();
    if (!atlas || stray) {
        fprintf(stderr, "aliasing draw over %s: atlas %d (in place %llu, why", name, atlas,
                (unsigned long long)(after.inplace - before.inplace));
        for (int i = 0; i < VK_ALIAS_N; i++) fprintf(stderr, " %llu", (unsigned long long)(after.why[i] - before.why[i]));
        fprintf(stderr, "), a write in place left %d\n", stray);
    }
    return !atlas || stray;
}
static int check_uma_alias_fallback(void) {
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    const int was = g_settings.guest_buffers_copied;
    const u32 word = 0x12345678;
    CHECK(mem_map(UMA_AF, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GAF, UMA_AF, 0x200000);
    memset(GP(UMA_AF), 0x31, 0x10000);
    /* q the runtime wrote */
    CHECK(!gmem_write(UMA_GAF + 0x1c00, &word, sizeof word));
    VkDrawStorage draw = alias_pair(UMA_GAF, UMA_GAF + 0x1000);
    vk_lock();
    CHECK(!alias_fell_back("a page the runtime wrote", &draw, VK_ALIAS_WRITE, UMA_GAF, 0x1000));
    vk_unlock();
    /* q under an earlier aliasing draw's atlas (uma_copies=2 binds none in place), its output pending */
    g_settings.guest_buffers_copied = 2;
    VkDrawStorage earlier = alias_draw(UMA_GAF + 0x3100, UMA_GAF + 0x3800);
    vk_lock();
    CHECK(vk_buf_draw_has_alias(&earlier) == 1 && vk_buf_draw_prepare(&earlier));
    vk_buf_writable_commit();
    CHECK(vk_buf_pending(UMA_GAF + 0x3800, 256));
    g_settings.guest_buffers_copied = was;
    draw = alias_pair(UMA_GAF + 0x2000, UMA_GAF + 0x3000);
    CHECK(!alias_fell_back("an atlas's output", &draw, VK_ALIAS_WRITE, UMA_GAF + 0x2000, 0x1000));
    vk_unlock();
    /* a read over two runs of guest memory: GPU pages 0x104000 and 0x105000 map guest pages 0x4000 and 0x7000 */
    gmmu_map(UMA_GAF + 0x105000, UMA_AF + 0x7000, 0x1000);
    gmmu_map(UMA_GAF + 0x104000, UMA_AF + 0x4000, 0x1000);
    draw = alias_draw(UMA_GAF + 0x104f80, UMA_GAF + 0x104800);
    vk_lock();
    CHECK(!alias_fell_back("two runs", &draw, VK_ALIAS_RUN, UMA_GAF + 0x104000, 0x1000));
    vk_unlock();
    /* a write two bytes off the storage alignment */
    if (g_vk.props.limits.minStorageBufferOffsetAlignment > 2) {
        draw = alias_draw(UMA_GAF + 0x6100, UMA_GAF + 0x6802);
        vk_lock();
        CHECK(!alias_fell_back("an offset off the alignment", &draw, VK_ALIAS_ALIGN, UMA_GAF + 0x6000, 0x1000));
        vk_unlock();
    }
    /* two groups with a page of no guest memory between: no one region holds them */
    CHECK(mem_map(UMA_AE, 0x1000, MS_NORMAL, PERM_RW) == 0);
    CHECK(mem_map(UMA_AE + 0x2000, 0x1000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GAE, UMA_AE, 0x1000);
    gmmu_map(UMA_GAE + 0x2000, UMA_AE + 0x2000, 0x1000);
    draw = alias_pair(UMA_GAE, UMA_GAE + 0x2000);
    vk_lock();
    CHECK(!alias_fell_back("a hole", &draw, VK_ALIAS_REGION, UMA_GAE, 0x3000));
    vk_unlock();
    CHECK(mem_unmap(UMA_AE, 0x1000) == 0 && mem_unmap(UMA_AE + 0x2000, 0x1000) == 0);
    CHECK(mem_unmap(UMA_AF, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    printf("in place: an aliasing draw that can't be bound in place goes to the atlas, with no write in place left\n");
    return 0;
}

/* The migrate way: an aliasing draw over the edge of two regions makes one region of their union, as a lookup does,
   with no wait, and binds both bindings there. With a write in place pending in one of them, the region could only
   be made after a wait for it: the draw goes to the atlas, and nothing waits */
#define UMA_AM  (HEAP_BASE + 0x30c00000)
#define UMA_GAM 0xc30000000ull
static int alias_two_regions(int way) {
    VkBuffer a, b;
    u64 o;
    CHECK(vk_uma_migrate(way));
    CHECK(vk_uma_cover(UMA_AM + 0x1ff000, 0x1000, &a, &o) == 1);
    CHECK(vk_uma_cover(UMA_AM + 0x200000, 0x1000, &b, &o) == 1);
    CHECK(a != b);
    return 0;
}
static int check_uma_alias_merge(void) {
    const u64 window = 0x200000;
#if defined(__ANDROID__)
    printf("skip: the migrate way runs here as the device chose it (vk_memory), not from this check\n");
    return 0;
#endif
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    const int way = uma_imports_shared() ? 1 : vk_uma_can_dmabuf() ? 2 : 0;
    if (!way) {
        printf("skip: the driver imports no buffer's pages as host memory or as a dma-buf, which the migrate way "
               "needs here\n");
        return 0;
    }
    CHECK(mem_map(UMA_AM, 2 * window, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GAM, UMA_AM, 2 * window);
    u8* g = GP(UMA_AM);
    memset(g + window - 0x2000, 0x31, 0x4000);
    /* the write in the first window's last page, the read across the edge */
    VkDrawStorage draw = alias_draw(UMA_GAM + window - 0x100, UMA_GAM + window - 0x400);
    draw.b[ST_VS][0].size = 0x200;
    vk_lock();
    CHECK(!alias_two_regions(way));
    AliasCounts before = alias_counts();
    u64 waits = vk_uma_out_waits(), submit = vk_submit_number();
    CHECK(vk_buf_draw_has_alias(&draw) == 1 && vk_buf_draw_prepare(&draw));
    AliasCounts after = alias_counts();
    const VkDrawGbuf* in = &draw.b[ST_VS][0];
    const VkDrawGbuf* out = &draw.b[ST_FS][0];
    const int merged = after.inplace == before.inplace + 1 && in->buf == out->buf && in->off - out->off == 0x300 &&
                       draw.map_size[ST_FS] == 48 * 4;
    const int nowait = vk_uma_out_waits() == waits && vk_submit_number() == submit;
    CHECK(!owned_test_fill(out->buf, out->off, 256, 0x33));
    vk_buf_writable_commit();
    vk_finish();
    CHECK(vk_uma_migrate(0));
    vk_finish();
    vk_unlock();
    u32 wrong = 0;
    for (u32 i = window - 0x2000; i < window + 0x2000; i++)
        wrong += g[i] != (i >= window - 0x400 && i < window - 0x300 ? 0x33 : 0x31);
    /* again, a write in place pending in the first region */
    VkBuffer w;
    u64 wo;
    draw = alias_draw(UMA_GAM + window - 0x100, UMA_GAM + window - 0x400);
    draw.b[ST_VS][0].size = 0x200;
    vk_lock();
    CHECK(!alias_two_regions(way));
    CHECK(vk_buf_copy_writable(UMA_GAM + window - 0x1000, 256, &w, &wo, 1));
    CHECK(!owned_test_fill(w, wo, 256, 0x35));
    vk_buf_writable_commit();
    CHECK(vk_uma_pending(UMA_GAM + window - 0x1000, 256));
    before = alias_counts();
    waits = vk_uma_out_waits();
    submit = vk_submit_number();
    CHECK(vk_buf_draw_has_alias(&draw) == 1 && vk_buf_draw_prepare(&draw));
    after = alias_counts();
    const int refused = after.atlas == before.atlas + 1 && after.why[VK_ALIAS_WAIT] == before.why[VK_ALIAS_WAIT] + 1;
    const int nowait2 = vk_uma_out_waits() == waits && vk_submit_number() == submit;
    vk_buf_writable_commit();
    vk_finish();
    CHECK(vk_uma_migrate(0));
    vk_finish();
    vk_unlock();
    const int landed = g[window - 0x1000] == 0x35 && g[window - 0xf01] == 0x35;
    CHECK(mem_unmap(UMA_AM, 2 * window) == 0);
    g_renderer->mem_may_have_changed();
    if (!merged || !nowait || wrong || !refused || !nowait2 || !landed)
        fprintf(stderr,
                "aliasing draw over two regions: one region %d (%p+%llx, %p+%llx), no wait %d, %u wrong bytes; with a "
                "write pending: atlas for the wait %d, no wait %d, the write landed %d\n",
                merged, (void*)in->buf, (unsigned long long)in->off, (void*)out->buf, (unsigned long long)out->off,
                nowait, wrong, refused, nowait2, landed);
    CHECK(merged && nowait && !wrong && refused && nowait2 && landed);
    printf("in place, the migrate way: an aliasing draw over two regions makes one with no wait, and goes to the atlas "
           "where only a wait would make it\n");
    return 0;
}

/* What an aliasing draw wrote in place is pending until its submit is done (uma.c's out_add): another thread's read of
   the runtime (gmem_read) before then waits for it, and gets the new bytes */
#define UMA_AR  (HEAP_BASE + 0x31400000)
#define UMA_GAR 0xc40000000ull
typedef struct {
    u8 bytes[256];
    _Atomic u32 done;
    int failed;
} AliasRead;
static void alias_read_thread(void* arg) {
    AliasRead* r = arg;
    r->failed = gmem_read(UMA_GAR + 0x800, r->bytes, sizeof r->bytes) != 0;
    atomic_store_explicit(&r->done, 1u, memory_order_release);
    plat_wake_word(&r->done);
}
static int check_uma_alias_cpu_read(void) {
    if (!vk_uma()) {
        printf("skip: guest memory is copied on this device (vk_memory)\n");
        return 0;
    }
    CHECK(mem_map(UMA_AR, 0x200000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(UMA_GAR, UMA_AR, 0x200000);
    memset(GP(UMA_AR), 0x31, 0x1000);
    static AliasRead r;
    r = (AliasRead){.done = 0};
    VkDrawStorage draw = alias_draw(UMA_GAR + 0x100, UMA_GAR + 0x800);
    vk_lock();
    const AliasCounts before = alias_counts();
    CHECK(vk_buf_draw_has_alias(&draw) == 1 && vk_buf_draw_prepare(&draw));
    const int inplace = alias_counts().inplace == before.inplace + 1;
    CHECK(!owned_test_fill(draw.b[ST_FS][0].buf, draw.b[ST_FS][0].off, 256, 0x44));
    vk_buf_writable_commit();
    const u64 recorded = vk_submit_number();
    vk_unlock();
    CHECK(plat_thread(alias_read_thread, &r, 0) == 0);
    while (!atomic_load_explicit(&r.done, memory_order_acquire)) plat_wait_word(&r.done, 0, 100000000);
    u32 old = 0;
    for (u32 i = 0; i < 256; i++) old += r.bytes[i] != 0x44;
    vk_lock();
    vk_finish();
    vk_unlock();
    CHECK(mem_unmap(UMA_AR, 0x200000) == 0);
    g_renderer->mem_may_have_changed();
    if (!inplace || r.failed || old)
        fprintf(stderr, "aliasing draw's write read on another thread: in place %d, read failed %d, %u old bytes (the "
                "write recorded in submit %llu)\n", inplace, r.failed, old, (unsigned long long)recorded);
    CHECK(inplace && !r.failed && !old);
    printf("in place: another thread's read waits for an aliasing draw's write in place, and gets its bytes\n");
    return 0;
}
