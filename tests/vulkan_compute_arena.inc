/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The compute arena's space check (compute.c's need, before any binding is put in the arena): a dispatch that runs
   from the arena binds eight constant buffers, seven of them of 4 bytes, each bound as a whole checked step of the
   device's (vk_cb_range). The step is faked here to 4096 bytes, past the 256 a device may ask
   (robustUniformBufferAccessSizeAlignment), so that what it adds to a binding passes any alignment the check adds
   too: the check counts each binding's step, and the bindings take no more of the arena than it made room for
   (vk_cs_arena_short). Counted by the guest's 4 bytes, each took about 4 KiB past its count, which at the arena's end
   were written past it. The program stores the sum of the eight buffers' first words */
static int check_compute_arena_count(void) {
    const u64 gpu = 0x9c0000000ull, cpu = HEAP_BASE + 0x3c200000;
    const u64 code_at = gpu, constants = gpu + 0x1000, output = gpu + 0x2000;
    const u32 step = 4096;
    if (vk_uma()) {
        puts("compute arena: memory is shared with the CPU, where every dispatch binds in place: not checked");
        return 0;
    }
    CHECK(mem_map(cpu, 0x10000, MS_NORMAL, PERM_RW) == 0);
    gmmu_map(gpu, cpu, 0x10000);
    /* R2 to R9 the first word of buffers 0 to 7, summed into R2 and stored at the address constant buffer 0 gives */
    u64 ops[24];
    u32 nops = 0;
    for (u32 b = 0; b < 8; b++) ops[nops++] = 0x4c98078000070000ull | (u64)b << 34 | (2u + b); /* MOV R(2+b), c[b][0] */
    for (u32 r = 3; r < 10; r++) ops[nops++] = 0x5c10000000070202ull | (u64)r << 20;           /* IADD R2, R2, Rr */
    ops[nops++] = 0x4c98078004470000ull;                                                       /* MOV R0, c[0][0x110] */
    ops[nops++] = 0x4c98078004570001ull;                                                       /* MOV R1, c[0][0x114] */
    ops[nops++] = 0xeedc200000070002ull;                                                       /* STG.E [R0], R2 */
    ops[nops++] = 0xe30000000007000full;                                                       /* EXIT */
    u8 code[512] = {0};
    u32 at = 0;
    for (u32 i = 0; i < nops; i++) {
        if (!(at % 4)) snx_st_le64(code + 8 * at++, 0x001f8000fc0007e0ull);
        snx_st_le64(code + 8 * at++, ops[i]);
    }
    CHECK(at * 8 <= sizeof code);
    CHECK(!gmem_write(code_at, code, sizeof code));
    u32 qmd[64] = {0}, sum = 0;
    for (u32 b = 0; b < 8; b++) {
        const u64 cb = constants + b * 0x200;
        const u32 word = 0x1001u * (b + 1);
        CHECK(!gmem_write(cb, &word, 4));
        sum += word;
        qmd[QMD_CONST_BUFFER_CONFIG + b * QMD_CONST_BUFFER_CONFIG_WORDS] = (u32)cb;
        qmd[QMD_CONST_BUFFER_CONFIG + b * QMD_CONST_BUFFER_CONFIG_WORDS + 1] = (u32)(cb >> 32) | (b ? 4u : 0x200u)
                                                                                                     << 15;
    }
    const u32 descriptor[4] = {(u32)output, (u32)(output >> 32), 4, 0};
    CHECK(!gmem_write(constants + 0x110, descriptor, sizeof descriptor));
    CHECK(!gmem_write(output, (const u32[1]){0xcdcdcdcd}, 4));
    qmd[QMD_GRID_DIM_X] = 1;
    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_BLOCK_DIM_X] = 1u << 16;
    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
    qmd[QMD_CONST_BUFFER_ENABLE] = 0xff;
    GpuChannel* ch = calloc(1, sizeof *ch);
    CHECK(ch);
    const int robust = g_vk.robust_buffer2;
    const u32 ubo_align = g_vk.robust_ubo_align;
    const u64 shorts = vk_cs_arena_short(), dispatches = g_vk_dispatches;
    vk_lock();
    /* the CPU read the output lately: the dispatch runs on the compute queue from the arena, not kept on the GPU */
    vk_own_cpu_touched(cpu + 0x2000, 0x1000);
    g_vk.robust_buffer2 = 1;
    g_vk.robust_ubo_align = step;
    DispatchState d;
    mx_dispatch_state(qmd, ch->rcomp, code_at, &d);
    vk_dispatch(&d);
    vk_compute_land();
    g_vk.robust_buffer2 = robust;
    g_vk.robust_ubo_align = ubo_align;
    vk_unlock();
    free(ch);
    u32 got = 0;
    CHECK(!gmem_read(output, &got, 4));
    CHECK(mem_unmap(cpu, 0x10000) == 0);
    const u64 short_after = vk_cs_arena_short();
    if (g_vk_dispatches != dispatches + 1 || short_after != shorts || got != sum)
        fprintf(stderr,
                "compute arena: %llu dispatches recorded (1 expected), %llu took more of the arena than counted (none "
                "expected), the sum stored %#x, not %#x\n",
                (unsigned long long)(g_vk_dispatches - dispatches), (unsigned long long)(short_after - shorts), got,
                sum);
    CHECK(g_vk_dispatches == dispatches + 1 && short_after == shorts && got == sum);
    printf("compute arena: eight constant buffers bound as checked steps of %u bytes took no more than the space check "
           "counted\n",
           step);
    return 0;
}
