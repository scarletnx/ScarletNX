/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a read of guest memory over a program's write in the GPU mirror waits for (mirror.c's vk_mirror_mem_need), with a
   program of one store */
#define MP_CPU  (HEAP_BASE + 0x25800000) /* a granule of the mirror no other check binds, and the program's after it */
#define MP_GVA  0xbe0000000ull
#define MP_LEN  0x10000u            /* the buffer: the memory mapped from its address on */
#define MP_COPY (MP_GVA + 0x200000) /* the program */
#define MP_BANK (MP_COPY + 0x1000)  /* its constants */
static u8 mp_guest(u32 i) { return (u8)((i * 7 + 1) & 0x7f); }
/* the program: the byte at c[0][0x100] of the buffer stored at c[0][0x200] of it */
static int mp_program(void) {
    u8 program[16 * 8] = {0};
    u32 n = 0;
    Sm86Word x;
    for (u32 k = 0; k < 2; k++) { /* LDC R5, c[0][0x100], the load's offset; LDC R6, c[0][0x200], the store's */
        x = sm86_fix(0xb82);
        sm86_set(&x, 16, 8, 5 + k);
        sm86_set(&x, 24, 8, 255);
        sm86_cbuf(&x, 0, k ? 0x200 : 0x100);
        sm86_set(&x, 73, 3, 4);
        sm86_put(program, n++, x);
    }
    x = sm86_fix(0xab9); /* ULDC.64 UR6, c[0][0x28]: the buffer */
    sm86_set(&x, 16, 6, 6);
    sm86_cbuf(&x, 0, 0x28);
    sm86_set(&x, 73, 3, 5);
    sm86_put(program, n++, x);
    x = sm86_fix(0x981); /* LDG.E.U8 R4, [R5.U32 + UR6] */
    sm86_set(&x, 16, 8, 4);
    sm86_set(&x, 24, 8, 5);
    sm86_set(&x, 32, 6, 6);
    sm86_set(&x, 72, 1, 1);
    sm86_set(&x, 73, 3, 0);
    sm86_set(&x, 81, 3, 7);
    sm86_set(&x, 84, 3, 1);
    sm86_set(&x, 91, 1, 1);
    sm86_put(program, n++, x);
    x = sm86_fix(0x986); /* STG.E.U8 [R6.U32 + UR6], R4 */
    sm86_set(&x, 24, 8, 6);
    sm86_set(&x, 32, 8, 4);
    sm86_set(&x, 64, 6, 6);
    sm86_set(&x, 72, 1, 1);
    sm86_set(&x, 73, 3, 0);
    sm86_set(&x, 84, 3, 1);
    sm86_set(&x, 91, 1, 1);
    sm86_put(program, n++, x);
    sm86_put(program, n++, sm86_exit());
    sm86_put(program, n, sm86_bra(n, n, 7, 0));
    CHECK(!gmem_write(MP_COPY, program, sizeof program));
    return 0;
}
/* a dispatch of it, recorded: the finishes it made, or -1 where it did not run */
static int mp_dispatch(GpuChannel* ch, u32 from, u32 to) {
    u32 bank[0x300 / 4] = {0}, qmd[64] = {0};
    DispatchState ds;
    bank[0x28 / 4] = (u32)MP_GVA;
    bank[0x2c / 4] = (u32)(MP_GVA >> 32);
    bank[0x100 / 4] = from;
    bank[0x200 / 4] = to;
    if (gmem_write(MP_BANK, bank, sizeof bank)) return -1;
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)MP_BANK;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(MP_BANK >> 32) | 0x1000u << 15;
    const u64 ran = g_vk_dispatches, finishes = g_vk_finishes;
    mx_dispatch_state(qmd, ch->rcomp, MP_COPY, &ds);
    vk_dispatch(&ds);
    return g_vk_dispatches == ran + 1 ? (int)(g_vk_finishes - finishes) : -1;
}

/* A read of guest memory over a byte a program wrote in the mirror, in a submission made and not done: it waits for that
   submission alone, letting go of vk_lock, and takes the byte. What is being recorded meanwhile stays so, and no finish
   is made. The submission waits for a semaphore that another thread signals once it held the lock */
static int check_mirror_batch(void) {
    if (!vk_mirror_on()) {
        puts("The GPU mirror is off on this device: reads over its batches not checked");
        return 0;
    }
    CHECK(mem_map(MP_CPU, MP_LEN, MS_NORMAL, PERM_RW) == 0);
    CHECK(mem_map(MP_CPU + 0x200000, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(MP_GVA, MP_CPU, MP_LEN);
    gmmu_map(MP_COPY, MP_CPU + 0x200000, 0x10000);
    CHECK(!mp_program());
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    vk_lock();
    vk_finish();
    const int sm86 = g_shader_sm86;
    g_shader_sm86 = 1;
    for (u32 i = 0; i < MP_LEN; i++) GP(MP_CPU)[i] = mp_guest(i);
    results_copied(MP_CPU, MP_LEN); /* touched lately: the pages land with their submission, none is published */
    vk_ww_new_epoch();
    const int made = mp_dispatch(ch, 0x10, 0xc000);
    g_shader_sm86 = sm86;
    const int pending = vk_mirror_pending(MP_GVA + 0xc000, 1);
    CHECK(!fl_gate());
    vk_submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE);
    const u64 at = vk_last_submit();
    (void)vk_cmd(); /* something being recorded when the read comes */
    const u64 recording = vk_submit_number(), finishes = g_vk_finishes;
    vk_unlock();
    CHECK(!plat_thread(fl_thread, NULL, 1u << 16));
    u8 byte = 0;
    CHECK(!gmem_read(MP_GVA + 0xc000, &byte, 1));
    vk_lock();
    const u64 finished = g_vk_finishes - finishes, still = vk_submit_number();
    u64 submitted, done;
    vk_submit_progress(&submitted, &done);
    vk_unlock();
    const int locked = fl_ended("a read over the mirror's batch");
    free(ch);
    if (made || !pending || locked || finished || still != recording || done < at || byte != mp_guest(0x10)) {
        fprintf(stderr,
                "vulkan_headless: mirror batch: the dispatch made %d finishes (none) and left the byte pending %d (1); "
                "the read made %llu finishes (none), submission %llu was being recorded after it (%llu), %llu were "
                "done (%llu at least), the lock %s, and it took %#x (%#x)\n",
                made, pending, (unsigned long long)finished, (unsigned long long)still, (unsigned long long)recording,
                (unsigned long long)done, (unsigned long long)at, locked ? "held" : "let go of", byte, mp_guest(0x10));
        return 1;
    }
    puts("mirror batch: a read over a submitted program's write waited for its submission alone, with vk_lock let go");
    return 0;
}
