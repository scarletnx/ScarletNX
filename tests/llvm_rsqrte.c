/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Real emitted FRSQRTE steps against the exact helper: every normal exponent and table-bin boundary, mixed
   exceptional lanes, FPCR modes and sticky FPSR. QEMU differential tests independently check the helper's semantics.
   Both optimization levels and loaders run, with host features and with the portable fallback. */
#include "llvm_steps.h"
#include <stdio.h>
#include <string.h>

static unsigned failures;
static unsigned long long checked;

static void check(LlvmStep step, unsigned lanes, const u32 input[4], u32 fpcr, u32 fpsr) {
    CPU expected = {0}, actual = {0};
    for (unsigned v = 0; v < 32; v++)
        for (unsigned i = 0; i < 4; i++) expected.v[v].s[i] = 0x12345678u + v * 4 + i;
    memcpy(expected.v[1].s, input, sizeof expected.v[1].s);
    expected.fpcr = fpcr;
    expected.fpsr = fpsr;
    expected.pc = 0x1000;
    actual = expected;
    for (unsigned i = 0; i < lanes; i++)
        expected.v[0].s[i] = (u32)snx_fp_unary(&expected, input[i], 32, SNX_FP_RSQRTE);
    if (lanes == 2) expected.v[0].d[1] = 0;
    u64 next = step(&actual);
    checked++;
    if (next != 0x1004 || memcmp(&actual, &expected, sizeof actual)) {
        if (failures++ < 8)
            fprintf(stderr, "FRSQRTE %u lanes, FPCR %08x FPSR %08x: %08x %08x %08x %08x, "
                            "got %08x/%08x expected %08x/%08x (next %llx)\n",
                    lanes, fpcr, fpsr, input[0], input[1], input[2], input[3],
                    actual.v[0].s[0], actual.fpsr, expected.v[0].s[0], expected.fpsr,
                    (unsigned long long)next);
    }
}

static int run(int jit, int portable, char opt) {
    char error[1024];
    LlvmSteps s;
    if (llvm_steps_begin_opt(&s, jit, portable, opt, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        return 1;
    }
    llvm_steps_add(&s, 0x6EA1D820u, 0x1000, 0); /* frsqrte v0.4s, v1.4s */
    llvm_steps_add(&s, 0x2EA1D820u, 0x1000, 0); /* frsqrte v0.2s, v1.2s */
    if (llvm_steps_load(&s, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        llvm_steps_end(&s);
        return 1;
    }
    for (unsigned form = 0; form < 2; form++) {
        unsigned lanes = form ? 2 : 4;
        for (unsigned exponent = 1; exponent < 255; exponent++)
            for (unsigned bin = 0; bin < 256; bin++) {
                u32 first = exponent << 23 | bin << 15;
                u32 input[4] = {first, first | 0x7fffu, first | 1, first | 0x4000u};
                check(s.steps[form], lanes, input, 0, 0);
                /* Every edge reaches a live lane of the short form too. */
                if (form) {
                    input[0] |= 1;
                    input[1] = first | 0x4000u;
                    check(s.steps[form], lanes, input, 0, 0);
                }
            }
        static const u32 edges[] = {0, 0x80000000u, 1, 0x80000001u, 0x007fffffu, 0x807fffffu,
                                    0x00800000u, 0x7f7fffffu, 0xbf800000u, 0xff7fffffu,
                                    0x7f800000u, 0xff800000u, 0x7fc12345u, 0xffc54321u,
                                    0x7f812345u, 0xff854321u};
        static const u32 sticky[] = {0, 0x1fu, 0x80u, 0x0800009fu};
        for (unsigned mode = 0; mode < 32; mode++) {
            u32 fpcr = (mode & 3) << 22 | ((mode >> 2) & 1) << 24 |
                       ((mode >> 3) & 1) << 25 | ((mode >> 4) & 1) << 19;
            for (unsigned flags = 0; flags < sizeof sticky / sizeof sticky[0]; flags++) {
                u32 normal[4] = {0x00800000u, 0x3fc00000u, 0x40000000u, 0x7f7fffffu};
                check(s.steps[form], lanes, normal, fpcr, sticky[flags]);
                for (unsigned edge = 0; edge < sizeof edges / sizeof edges[0]; edge++)
                    for (unsigned lane = 0; lane < lanes; lane++) {
                        u32 input[4] = {0x3f800000u, 0x40000000u, 0x40400000u, 0x40800000u};
                        input[lane] = edges[edge];
                        check(s.steps[form], lanes, input, fpcr, sticky[flags]);
                        input[(lane + 1) % lanes] = edges[(edge + 1) % (sizeof edges / sizeof edges[0])];
                        check(s.steps[form], lanes, input, fpcr, sticky[flags]);
                    }
            }
        }
    }
    llvm_steps_end(&s);
    return 0;
}

int main(void) {
    for (int jit = 0; jit < 2; jit++)
        for (int portable = 0; portable < 2; portable++)
            for (unsigned level = 0; level < 2; level++)
                if (run(jit, portable, level ? '2' : '0')) return 1;
    printf("FRSQRTE: %llu cases, %u failures\n", checked, failures);
    return failures != 0;
}
