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
        vk_own_publish(UMA_M + at, 256, owner, 0);
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
