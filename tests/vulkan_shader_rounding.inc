/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Execute actual Maxwell F2F instructions on64 source values for each narrowing pair and rounding direction.
   The CPU oracle uses exact long-double values and searches adjacent representable half values. */
#include <float.h>
#include <math.h>

static long double rounding_half_value(u32 bits) {
    u32 exponent = bits >> 10, fraction = bits & 1023;
    return exponent ? ldexpl((long double)(1024 + fraction), (int)exponent - 25) : ldexpl((long double)fraction, -24);
}
static u32 rounding_half_reference(long double value, u32 mode) {
    int negative = signbit(value) != 0;
    long double magnitude = fabsl(value);
    u32 sign = negative ? 0x8000 : 0;
    if (!isfinite(value)) return sign | 0x7c00;
    u32 lo = 0, hi = 0x7c00;
    while (lo + 1 < hi) {
        u32 mid = (lo + hi) / 2;
        if (rounding_half_value(mid) <= magnitude)
            lo = mid;
        else
            hi = mid;
    }
    if (magnitude == rounding_half_value(lo)) return sign | lo;
    if (mode) return sign | ((mode == 1 ? negative : mode == 2 ? !negative : 0) ? hi : lo);
    if (hi == 0x7c00) return sign | (magnitude >= 65520.0L ? hi : lo);
    long double down = magnitude - rounding_half_value(lo), up = rounding_half_value(hi) - magnitude;
    return sign | (down < up || (down == up && !(lo & 1)) ? lo : hi);
}
static u32 rounding_float_reference(long double value, u32 mode) {
    float result = (float)value;
    if (mode == 1 && (long double)result > value) result = nextafterf(result, -INFINITY);
    if (mode == 2 && (long double)result < value) result = nextafterf(result, INFINITY);
    if (mode == 3 && ((value > 0 && (long double)result > value) || (value < 0 && (long double)result < value)))
        result = nextafterf(result, 0);
    u32 bits;
    memcpy(&bits, &result, sizeof bits);
    return bits;
}
static int check_shader_float_rounding(void) {
    if (!g_vk.feat.shaderFloat64) {
        puts("SKIP shader float narrowing: shaderFloat64 is unavailable");
        return 0;
    }
    const u64 gpu = 0x4f6000000ull, cpu = HEAP_BASE + 0x710000;
    const u64 constants = gpu + 0x1000, values = gpu + 0x2000;
    GpuChannel* channel = calloc(1, sizeof *channel);
    CHECK(channel);
    gmmu_map(gpu, cpu, 0x10000);
    const long double sources[32] = {0,
                                     -0.0L,
                                     1,
                                     -1,
                                     1.00048828125L,
                                     1.00146484375L,
                                     1.0004882812500002220446049250313080847263336181640625L,
                                     1.0004882812499997779553950749686919152736663818359375L,
                                     0x1p-25L,
                                     -0x1p-25L,
                                     0x1p-24L,
                                     -0x1p-24L,
                                     0x1.ffcp-15L,
                                     -0x1.ffcp-15L,
                                     65504,
                                     -65504,
                                     65519,
                                     -65519,
                                     65520,
                                     -65520,
                                     65521,
                                     -65521,
                                     0x1p-149L,
                                     -0x1p-149L,
                                     0x1p-150L,
                                     -0x1p-150L,
                                     0x1.000001p0L,
                                     -0x1.000001p0L,
                                     0x1.ffffffp127L,
                                     -0x1.ffffffp127L,
                                     0x1p128L,
                                     -0x1p128L};
    for (u32 pair = 0; pair < 3; pair++)
        for (u32 mode = 0; mode < 4; mode++) {
            u32 source_format = pair == 0 ? 2 : 3, destination_format = pair == 2 ? 2 : 1;
            u32 input[64][4], expected[64];
            for (u32 i = 0; i < 64; i++) {
                long double value = i < 32 ? sources[i] : ldexpl((long double)(i * 137 + 1), (int)i - 52);
                if (source_format == 2) {
                    float f = (float)value;
                    memcpy(input[i], &f, sizeof f);
                    input[i][1] = 0;
                    value = f;
                } else {
                    double d = (double)value;
                    u64 bits;
                    memcpy(&bits, &d, sizeof bits);
                    input[i][0] = (u32)bits;
                    input[i][1] = (u32)(bits >> 32);
                    value = d;
                }
                input[i][2] = input[i][3] = 0xcdcdcdcd;
                expected[i] = destination_format == 1 ? rounding_half_reference(value, mode)
                                                      : rounding_float_reference(value, mode);
            }
            u64 instructions[] = {0x4c98078004470000ull, /* MOV R0, c[0][0x110] */
                                  0x4c98078004570001ull, /* MOV R1, c[0][0x114] */
                                  0xf0c8000002170002ull, /* S2R R2, TID.X */
                                  0x3848000000470202ull, /* SHL R2, R2,4 */
                                  0x5c10000000270000ull, /* IADD R0, R0,R2 */
                                  0xeed4200000070004ull, /* LDG.E R4, [R0] */
                                  0xeed4200000470005ull, /* LDG.E R5, [R0+4] */
                                  0x5ca8000000470006ull | (u64)source_format << 10 | (u64)destination_format << 8 |
                                      (u64)mode << 39,
                                  0xeedc200000870006ull, /* STG.E [R0+8],R6 */
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
                    fprintf(stderr, "F2F source%u destination%u mode%u sample%u: %08x expected %08x\n", source_format,
                            destination_format, mode, i, input[i][2], expected[i]);
                CHECK(input[i][2] == expected[i] && input[i][3] == 0xcdcdcdcd);
            }
        }
    free(channel);
    puts("PASS shader float narrowing:768 finite, subnormal, overflow, tie and directed-rounding cases exact=1");
    return 0;
}
