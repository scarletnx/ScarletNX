/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Execute I2F.F16 for every integer width, signedness, rounding direction and abs/neg combination. The source
   remains an integer in the oracle; all integers at a finite-half boundary are represented exactly by long double. */
static int check_shader_integer_half_rounding(void) {
    const u64 gpu = 0x4f7000000ull, cpu = HEAP_BASE + 0x720000;
    const u64 constants = gpu + 0x1000, values = gpu + 0x2000;
    static const u64 sources[32] = {0,
                                    1,
                                    2,
                                    127,
                                    128,
                                    255,
                                    256,
                                    1023,
                                    1024,
                                    2048,
                                    2049,
                                    2050,
                                    2051,
                                    4095,
                                    4096,
                                    4097,
                                    32767,
                                    32768,
                                    65503,
                                    65504,
                                    65505,
                                    65519,
                                    65520,
                                    65521,
                                    65535,
                                    65536,
                                    0xffffffffu,
                                    0x7fffffffu,
                                    0x80000000u,
                                    0xffffffffffffffffull,
                                    0x7fffffffffffffffull,
                                    0x8000000000000000ull};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    gmmu_map(gpu, cpu, 0x10000);
    u32 checks = 0;
    for (u32 size = 0; size < 4; size++) {
        if (size == 3 && !g_vk.feat.shaderInt64) continue;
        u32 width = 8u << size;
        u64 mask = width == 64 ? ~0ull : (1ull << width) - 1;
        for (u32 is_signed = 0; is_signed < 2; is_signed++)
            for (u32 modifiers = 0; modifiers < 4; modifiers++)
                for (u32 mode = 0; mode < 4; mode++) {
                    u32 input[64][4], expected[64];
                    for (u32 i = 0; i < 64; i++) {
                        u64 raw = (i < 32 ? sources[i] : 0ull - sources[i - 32]) & mask;
                        int negative = is_signed && (raw >> (width - 1));
                        int minimum = is_signed && raw == (1ull << (width - 1));
                        u64 magnitude = negative ? (0ull - raw) & mask : raw;
                        if ((modifiers & 1) && !minimum) negative = 0;
                        if ((modifiers & 2) && !(minimum && !(modifiers & 1))) negative = !negative;
                        long double value = (long double)magnitude;
                        if (negative) value = -value;
                        input[i][0] = (u32)raw;
                        input[i][1] = (u32)(raw >> 32);
                        input[i][2] = input[i][3] = 0xcdcdcdcd;
                        expected[i] = rounding_half_reference(value, mode);
                    }
                    u64 instructions[] = {0x4c98078004470000ull, /* MOV R0, c[0][0x110] */
                                          0x4c98078004570001ull, /* MOV R1, c[0][0x114] */
                                          0xf0c8000002170002ull, /* S2R R2, TID.X */
                                          0x3848000000470202ull, /* SHL R2, R2,4 */
                                          0x5c10000000270000ull, /* IADD R0, R0,R2 */
                                          0xeed4200000070004ull, /* LDG.E R4, [R0] */
                                          0xeed4200000470005ull, /* LDG.E R5, [R0+4] */
                                          0x5cb8000000470106ull | (u64)size << 10 | (u64)is_signed << 13 |
                                              (u64)mode << 39 | (u64)(modifiers & 1) << 49 |
                                              (u64)(modifiers >> 1) << 45,
                                          0xeedc200000870006ull, /* STG.E [R0+8], R6 */
                                          0xe30000000007000full};
                    u64 program[16] = {0};
                    u32 count = 0;
                    for (u32 i = 0; i < sizeof instructions / sizeof *instructions; i++) {
                        if (!(count & 3)) program[count++] = 0x001f8000fc0007e0ull;
                        program[count++] = instructions[i];
                    }
                    u8 bytes[sizeof program];
                    for (u32 i = 0; i < count; i++) snx_st_le64(bytes + i * 8, program[i]);
                    CHECK(!gmem_write(gpu, bytes, count * 8));
                    CHECK(!gmem_write(values, input, sizeof input));
                    const u32 descriptor[3] = {(u32)values, (u32)(values >> 32), sizeof input};
                    CHECK(!gmem_write(constants + 0x110, descriptor, sizeof descriptor));
                    g_renderer->method_barrier(5);
                    u32 qmd[64] = {0};
                    qmd[QMD_GRID_DIM_X] = 1;
                    qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
                    qmd[QMD_BLOCK_DIM_X] = 64u << 16;
                    qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
                    qmd[QMD_CONST_BUFFER_ENABLE] = 1;
                    qmd[QMD_CONST_BUFFER_CONFIG] = (u32)constants;
                    qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(constants >> 32) | 0x200u << 15;
                    DispatchState dispatch;
                    vk_lock();
                    mx_dispatch_state(qmd, channel->rcomp, gpu, &dispatch);
                    u64 before = g_vk_dispatches;
                    vk_dispatch(&dispatch);
                    CHECK(g_vk_dispatches == before + 1);
                    vk_compute_land();
                    vk_unlock();
                    CHECK(!gmem_read(values, input, sizeof input));
                    for (u32 i = 0; i < 64; i++) {
                        if (input[i][2] != expected[i])
                            fprintf(stderr,
                                    "I2F.F16 width%u signed%u modifiers%u mode%u sample%u: %08x expected %08x\n", width,
                                    is_signed, modifiers, mode, i, input[i][2], expected[i]);
                        CHECK(input[i][2] == expected[i] && input[i][3] == 0xcdcdcdcd);
                        checks++;
                    }
                }
    }
    free(channel);
    printf("PASS shader integer-to-half: %u width, sign, abs/neg, finite, overflow and directed cases exact=1\n",
           checks);
    return 0;
}

/* Execute I2F.F32 of 32-bit integers, signed and unsigned, with each abs/neg combination and rounding mode (RN, RM,
   RP, RZ), converted whole and in halves (g_shader_i2f_nearest, for a driver whose conversions truncate): the mode
   rounds the value after abs and neg, as for an F16 result. The sources need rounding, sit on ties either way, are
   exact, or round to 2^31 or 2^32 */
static int check_shader_integer_float(void) {
    const u64 gpu = 0x4f7000000ull, cpu = HEAP_BASE + 0x720000; /* the region of the I2F.F16 check, done with */
    const u64 constants = gpu + 0x1000, values = gpu + 0x2000;
    static const u32 sources[32] = {0,          1,          0x00800001, 0x00ffffff, 0x01000000, 0x01000001, 0x01000003,
                                    0x01000005, 0x01000007, 0x02000002, 0x02000006, 0x01176c8f, 0x12345678, 0x55555555,
                                    0x7fffff3f, 0x7fffff40, 0x7fffff41, 0x7fffffbf, 0x7fffffc0, 0x7fffffff, 0x80000000,
                                    0x80000001, 0x80000040, 0x80000080, 0x80000180, 0x87654321, 0xaaaaaaab, 0xcafebabe,
                                    0xdeadbeef, 0xffffff7f, 0xffffff80, 0xffffffff};
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    gmmu_map(gpu, cpu, 0x10000);
    int was = g_shader_i2f_nearest;
    u32 checks = 0;
    for (u32 run = 0; run < 8; run++) {
        u32 nearest = run & 1, mode = run >> 1;
        g_shader_i2f_nearest = nearest ? 3 : 0;
        for (u32 is_signed = 0; is_signed < 2; is_signed++)
            for (u32 modifiers = 0; modifiers < 4; modifiers++) {
                u32 input[64][4], expected[64];
                for (u32 i = 0; i < 64; i++) {
                    u32 raw = i < 32 ? sources[i] : 0u - sources[i - 32];
                    int negative = is_signed && (raw >> 31);
                    int minimum = is_signed && raw == 0x80000000u;
                    u32 magnitude = negative ? 0u - raw : raw;
                    if ((modifiers & 1) && !minimum) negative = 0;
                    if ((modifiers & 2) && !(minimum && !(modifiers & 1))) negative = !negative;
                    long double value = (long double)magnitude;
                    if (negative) value = -value;
                    input[i][0] = raw;
                    input[i][1] = 0;
                    input[i][2] = input[i][3] = 0xcdcdcdcd;
                    expected[i] = rounding_float_reference(value, mode);
                }
                u64 instructions[] = {0x4c98078004470000ull,    /* MOV R0, c[0][0x110] */
                                      0x4c98078004570001ull,    /* MOV R1, c[0][0x114] */
                                      0xf0c8000002170002ull,    /* S2R R2, TID.X */
                                      0x3848000000470202ull,    /* SHL R2, R2,4 */
                                      0x5c10000000270000ull,    /* IADD R0, R0,R2 */
                                      0xeed4200000070004ull,    /* LDG.E R4, [R0] */
                                      storage_mov(10, nearest), /* each path its own program hash */
                                      /* I2F.F32.{U,S}32 R6, R4 with abs (49), neg (45) and the mode (39) */
                                      0x5cb8000000470a06ull | (u64)mode << 39 | (u64)is_signed << 13 |
                                          (u64)(modifiers & 1) << 49 | (u64)(modifiers >> 1) << 45,
                                      0xeedc200000870006ull, /* STG.E [R0+8], R6 */
                                      0xe30000000007000full};
                u64 program[16] = {0};
                u32 count = 0;
                for (u32 i = 0; i < sizeof instructions / sizeof *instructions; i++) {
                    if (!(count & 3)) program[count++] = 0x001f8000fc0007e0ull;
                    program[count++] = instructions[i];
                }
                u8 bytes[sizeof program];
                for (u32 i = 0; i < count; i++) snx_st_le64(bytes + i * 8, program[i]);
                CHECK(!gmem_write(gpu, bytes, count * 8));
                CHECK(!gmem_write(values, input, sizeof input));
                const u32 descriptor[3] = {(u32)values, (u32)(values >> 32), sizeof input};
                CHECK(!gmem_write(constants + 0x110, descriptor, sizeof descriptor));
                g_renderer->method_barrier(5);
                u32 qmd[64] = {0};
                qmd[QMD_GRID_DIM_X] = 1;
                qmd[QMD_GRID_DIM_YZ] = 1 | 1u << 16;
                qmd[QMD_BLOCK_DIM_X] = 64u << 16;
                qmd[QMD_BLOCK_DIM_YZ] = 1 | 1u << 16;
                qmd[QMD_CONST_BUFFER_ENABLE] = 1;
                qmd[QMD_CONST_BUFFER_CONFIG] = (u32)constants;
                qmd[QMD_CONST_BUFFER_CONFIG + 1] = (u32)(constants >> 32) | 0x200u << 15;
                DispatchState dispatch;
                vk_lock();
                mx_dispatch_state(qmd, channel->rcomp, gpu, &dispatch);
                u64 before = g_vk_dispatches;
                vk_dispatch(&dispatch);
                CHECK(g_vk_dispatches == before + 1);
                vk_compute_land();
                vk_unlock();
                CHECK(!gmem_read(values, input, sizeof input));
                for (u32 i = 0; i < 64; i++) {
                    if (input[i][2] != expected[i])
                        fprintf(stderr,
                                "I2F.F32.%c32 mode%u i2f_nearest %d modifiers%u source %08x: %08x expected %08x\n",
                                is_signed ? 'S' : 'U', mode, g_shader_i2f_nearest, modifiers,
                                i < 32 ? sources[i] : 0u - sources[i - 32], input[i][2], expected[i]);
                    CHECK(input[i][2] == expected[i] && input[i][3] == 0xcdcdcdcd);
                    checks++;
                }
            }
    }
    g_shader_i2f_nearest = was;
    free(channel);
    printf("PASS shader integer-to-float: %u signed and unsigned, abs/neg, RN/RM/RP/RZ, whole and in halves exact=1\n",
           checks);
    return 0;
}
