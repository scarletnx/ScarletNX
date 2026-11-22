/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Dispatches of an Ounce program that logs its writes over a buffer part of which a draw's result is still pending
   (compute.c's mirror_plan and the rule after it). The result is read where it is on the GPU: no vk_finish for it. And
   every byte read is the newest one, whoever wrote it last: the draw, a dispatch before it that is pending in the
   mirror or published as its owner, or a dispatch that left the mirror */
#define MP_CPU   (HEAP_BASE + 0x25800000) /* a granule of the mirror no other check binds, and the program's after it */
#define MP_GVA   0xbe0000000ull
#define MP_LEN   0x10000u            /* the buffer: the memory mapped from its address on */
#define MP_COPY  (MP_GVA + 0x200000) /* the programs */
#define MP_STORE (MP_COPY + 0x200)
#define MP_BANKS (MP_COPY + 0x1000) /* a constant bank of 0x1000 bytes for each dispatch */
#define MP_NBANKS 32u
static u32 mp_bank; /* the banks used so far */
static u8 mp_guest(u32 i) { return (u8)((i * 7 + 1) & 0x7f); }
/* MP_COPY: the byte at c[0][0x100] of the buffer stored at c[0][0x200] of it. MP_STORE: the byte c[0][0x100] stored at
   c[0][0x200], the buffer never read */
static int mp_program(u64 at, int load) {
    u8 program[16 * 8] = {0};
    u32 n = 0;
    Sm86Word x;
    for (u32 k = 0; k < 2; k++) { /* LDC R5 (the load's offset) or R4 (the byte), c[0][0x100]; LDC R6, c[0][0x200] */
        x = sm86_fix(0xb82);
        sm86_set(&x, 16, 8, k ? 6 : load ? 5 : 4);
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
    if (load) {
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
    }
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
    CHECK(!gmem_write(at, program, sizeof program));
    return 0;
}
/* a dispatch of program, recorded: the program's buffer starts base bytes into the buffer, and what it loads and where
   it stores count from the buffer's start. Returns the finishes it made, or -1 */
static int mp_dispatch(GpuChannel* ch, u64 program, u32 base, u32 arg, u32 to) {
    if (mp_bank == MP_NBANKS) return -1;
    const u64 at = MP_BANKS + (u64)mp_bank++ * 0x1000;
    u32 bank[0x300 / 4] = {0}, qmd[64] = {0};
    DispatchState ds;
    bank[0x28 / 4] = (u32)(MP_GVA + base);
    bank[0x2c / 4] = (u32)((MP_GVA + base) >> 32);
    bank[0x100 / 4] = program == MP_COPY ? arg - base : arg;
    bank[0x200 / 4] = to - base;
    if (gmem_write(at, bank, sizeof bank)) return -1;
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 1;
    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)at;
    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(at >> 32) | 0x1000u << 15;
    const u64 ran = g_vk_dispatches, finishes = g_vk_finishes;
    mx_dispatch_state(qmd, ch->rcomp, program, &ds);
    vk_dispatch(&ds);
    return g_vk_dispatches == ran + 1 ? (int)(g_vk_finishes - finishes) : -1;
}
/* a draw's storage result over [at, at + n) of the buffer, its bytes value: pending in the commands being recorded */
static int mp_draw(u32 at, u32 n, u8 value) {
    VkBuffer out;
    u64 off;
    results_copied(MP_CPU + at, n);
    CHECK(vk_buf_copy_writable(MP_GVA + at, n, &out, &off, 1));
    CHECK(!owned_test_fill(out, off, n, value));
    vk_buf_writable_commit();
    CHECK(vk_buf_pending(MP_GVA + at, n));
    return 0;
}
/* guest memory of the buffer once everything landed, against want: 0, or 1 with the first bytes that differ said */
static int mp_expect(const char* what, const u8* want) {
    static u8 got[MP_LEN];
    vk_finish();
    vk_mirror_publish();
    vk_mirror_mem_need(MP_GVA, MP_LEN);
    own_acquire(MP_CPU, MP_LEN);
    CHECK(!vk_buf_pending(MP_GVA, MP_LEN) && !vk_mirror_pending(MP_GVA, MP_LEN));
    memcpy(got, GP(MP_CPU), MP_LEN);
    u32 bad = 0;
    for (u32 i = 0; i < MP_LEN; i++)
        if (got[i] != want[i] && bad++ < 4)
            fprintf(stderr, "vulkan_headless: mirror, draw results: %s: byte %#x is %#x, not %#x\n", what, i, got[i],
                    want[i]);
    return bad != 0;
}
/* the bytes [at, at + n) are pending in the mirror, the programs' writes there not in guest memory yet */
static int mp_in_mirror(u32 at) { return vk_mirror_pending(MP_GVA + at, 1) && !vk_buf_pending(MP_GVA + at, 1); }
/* the buffer and the programs, by whichever check runs first */
static int mp_map(void) {
    static int mapped;
    if (mapped) return 0;
    CHECK(mem_map(MP_CPU, MP_LEN, MS_NORMAL, PERM_RW) == 0);
    CHECK(mem_map(MP_CPU + 0x200000, 0x40000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(MP_GVA, MP_CPU, MP_LEN);
    gmmu_map(MP_COPY, MP_CPU + 0x200000, 0x40000);
    CHECK(!mp_program(MP_COPY, 1) && !mp_program(MP_STORE, 0));
    mapped = 1;
    return 0;
}
static int check_mirror_draw_results(void) {
    static u8 want[MP_LEN];
    int wrong = 0;
    if (!vk_mirror_on()) {
        puts("The GPU mirror is off on this device: draw results under its dispatches not checked");
        return 0;
    }
    CHECK(!mp_map());
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    vk_lock();
    vk_finish();
    const u32 validation = g_vk_validation_messages;
    const int sm86 = g_shader_sm86;
    g_shader_sm86 = 1;
    for (u32 i = 0; i < MP_LEN; i++) GP(MP_CPU)[i] = want[i] = mp_guest(i);

    /* a draw's result read by the dispatch after it, and that dispatch's byte by the one after it */
    vk_ww_new_epoch();
    CHECK(!mp_draw(0x800, 0x100, 0xb5));
    const int f0 = mp_dispatch(ch, MP_COPY, 0, 0x810, 0x2000), f1 = mp_dispatch(ch, MP_COPY, 0, 0x2000, 0x2800);
    memset(want + 0x800, 0xb5, 0x100);
    want[0x2000] = want[0x2800] = 0xb5;
    wrong |= mp_expect("a draw's result read by two dispatches", want);

    /* another draw's result, and a dispatch over the memory after it, whose byte is pending in the mirror. The next one
       reads that byte, its buffer over both: it stays in the mirror, and the draw's result lands for it */
    vk_ww_new_epoch();
    CHECK(!mp_draw(0xa00, 0x100, 0xd7));
    const int f2 = mp_dispatch(ch, MP_COPY, 0x8000, 0x8010, 0x9000), in2 = mp_in_mirror(0x9000);
    const int f3 = mp_dispatch(ch, MP_COPY, 0, 0x9000, 0x9800), in3 = mp_in_mirror(0x9800);
    want[0x9000] = want[0x9800] = mp_guest(0x8010);
    memset(want + 0xa00, 0xd7, 0x100);
    wrong |= mp_expect("a byte pending in the mirror read beside a draw's result", want);

    /* the same with the mirror owning the pages of the first one's byte, published: not landed, as nothing read it */
    const u32 present = g_vk_present;
    g_vk_present += 1300; /* past the presents a page the CPU touched lands with its submit */
    vk_ww_new_epoch();
    const int f4 = mp_dispatch(ch, MP_COPY, 0x8000, 0x8020, 0xa000);
    vk_finish();
    vk_mirror_publish();
    const int owned = !own_traps() || own_lookup(MP_CPU + 0xa000, 1, NULL, 0) != 0;
    g_vk_present = present;
    CHECK(!mp_draw(0xc00, 0x100, 0xe8));
    const int f5 = mp_dispatch(ch, MP_COPY, 0, 0xa000, 0xa800), in5 = mp_in_mirror(0xa800);
    want[0xa000] = want[0xa800] = mp_guest(0x8020);
    memset(want + 0xc00, 0xe8, 0x100);
    wrong |= mp_expect("a byte the mirror owns read beside a draw's result", want);

    /* a dispatch that only stores leaves the mirror all the same: its byte over one pending in the mirror is the newer,
       and the byte pending beside it stays */
    vk_ww_new_epoch();
    CHECK(!mp_draw(0xe00, 0x100, 0xf9));
    const int f6 = mp_dispatch(ch, MP_COPY, 0x8000, 0x8030, 0xb000) + mp_dispatch(ch, MP_COPY, 0x8000, 0x8031, 0xb001);
    const int in6 = mp_in_mirror(0xb000);
    const int f7 = mp_dispatch(ch, MP_STORE, 0, 0x5e, 0xb000);
    want[0xb000] = 0x5e;
    want[0xb001] = mp_guest(0x8031);
    memset(want + 0xe00, 0xf9, 0x100);
    wrong |= mp_expect("a byte stored over one pending in the mirror", want);

    g_shader_sm86 = sm86;
    const u32 messages = g_vk_validation_messages - validation;
    vk_unlock();
    free(ch);
    if (wrong || messages || f0 || f1 || f2 || !in2 || f3 != 1 || !in3 || f4 || !owned || f5 != 1 || !in5 || f6 ||
        !in6 || f7)
        fprintf(stderr,
                "vulkan_headless: mirror, draw results: finishes %d and %d (none expected); %d and in the mirror %d "
                "(0 and 1), %d and %d (1 and 1); %d with the mirror owning %d (0 and 1), %d and in the mirror %d (1 "
                "and 1); %d and in the mirror %d (0 and 1), %d (none expected); %u validation messages\n",
                f0, f1, f2, in2, f3, in3, f4, owned, f5, in5, f6, in6, f7, messages);
    CHECK(!wrong && !messages);
    CHECK(!f0 && !f1 && !f2 && in2 && f3 == 1 && in3 && !f4 && owned && f5 == 1 && in5 && !f6 && in6 && !f7);
    puts("mirror, draw results: a dispatch of a logging program read a draw's pending result on the GPU without a "
         "finish, and one whose buffer the mirror holds bytes of stayed in the mirror, each byte the newest");
    return 0;
}
