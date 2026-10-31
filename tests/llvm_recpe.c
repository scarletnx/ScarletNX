/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Real emitted FRECPE steps against the exact helper: every normal exponent and table-bin boundary, mixed
   exceptional lanes, FPCR modes and sticky FPSR. QEMU differential tests independently check the helper's semantics.
   Both optimization levels and loaders run, with host features and with the portable fallback. */
#include "llvm_steps.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static unsigned failures;
static unsigned long long checked;

static u64 random_next(u64* state) {
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

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
        expected.v[0].s[i] = (u32)snx_fp_unary(&expected, input[i], 32, SNX_FP_RECPE);
    if (lanes == 2) expected.v[0].d[1] = 0;
    u64 next = step(&actual);
    checked++;
    if (next != 0x1004 || memcmp(&actual, &expected, sizeof actual)) {
        if (failures++ < 8)
            fprintf(stderr, "FRECPE %u lanes, FPCR %08x FPSR %08x: %08x %08x %08x %08x, "
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
    llvm_steps_add(&s, 0x4EA1D820u, 0x1000, 0); /* frecpe v0.4s, v1.4s */
    llvm_steps_add(&s, 0x0EA1D820u, 0x1000, 0); /* frecpe v0.2s, v1.2s */
    if (!portable && s.ir->features->x86 && s.ir->features->avx2 &&
        LLVMGetNamedFunction(s.ir->mod, "snx_fp_unary")) {
        fprintf(stderr, "inline reciprocal still contains a lane helper\n");
        llvm_steps_end(&s);
        return 1;
    }
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
                for (unsigned i = 0; i < 4; ++i) input[i] |= 0x80000000u;
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
                                    0x7f812345u, 0xff854321u,
                                    0x001fffffu, 0x00200000u, 0x00200001u, 0x003fffffu, 0x00400000u,
                                    0x801fffffu, 0x80200000u, 0x803fffffu, 0x80400000u,
                                    0x7e7fffffu, 0x7e800000u, 0x7effffffu, 0x7f000000u,
                                    0xfe800000u, 0xfeffffffu, 0xff000000u};
        static const u32 sticky[] = {0, 0x1fu, 0x80u, 0x0800009fu};
        for (unsigned mode = 0; mode < 32; mode++) {
            u32 fpcr = (mode & 3) << 22 | ((mode >> 2) & 1) << 24 |
                       ((mode >> 3) & 1) << 25 | ((mode >> 4) & 1) << 19;
            for (unsigned flags = 0; flags < sizeof sticky / sizeof sticky[0]; flags++) {
                u32 normal[4] = {0x00800000u, 0xbfc00000u, 0x40000000u, 0x7e7fffffu};
                check(s.steps[form], lanes, normal, fpcr, sticky[flags]);
                u32 exceptional[4] = {0x7f812345u, 0x80000001u, 0x7f800000u, 0};
                check(s.steps[form], lanes, exceptional, fpcr, sticky[flags]);
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
        u64 rng = 0x8c3c010cb4754c9dull;
        for (unsigned sample = 0; sample < 32768; ++sample) {
            u32 input[4];
            for (unsigned lane = 0; lane < 4; ++lane) input[lane] = (u32)random_next(&rng);
            u32 fpcr = (sample & 3) << 22 | ((sample >> 2) & 1) << 24 | ((sample >> 3) & 1) << 25;
            check(s.steps[form], lanes, input, fpcr, (sample & 16) ? 0x0800009fu : 0);
        }
    }
    llvm_steps_end(&s);
    return 0;
}

static int bench(const char* workload, char opt, const char* dump, int reference) {
    char error[1024];
    LlvmSteps s;
    if (llvm_steps_begin_opt(&s, 1, reference, opt, error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add(&s, 0x4EA1D820u, 0x1000, 0);
    if (dump) {
        int failed = llvm_steps_dump(&s, dump, error, sizeof error);
        if (failed) fprintf(stderr, "%s\n", error);
        llvm_steps_end(&s);
        return failed != 0;
    }
    if (llvm_steps_load(&s, error, sizeof error)) return snx_error("%s", error);
    CPU c = {0};
    c.pc = 0x1000;
    c.fpsr = 0x0800009fu;
    c.v[1] = (V128){.s = {0x3fc00000u, 0xc0000000u, 0x40400000u, 0x3eaaaaabu}};
    if (!strcmp(workload, "mixed")) c.v[1].s[2] = 0x7f812345u;
    else if (!strcmp(workload, "underflow")) c.v[1].s[2] = 0x7f000000u;
    else if (!strcmp(workload, "all-exceptional"))
        c.v[1] = (V128){.s = {0, 0x80000001u, 0x7f800000u, 0x7f812345u}};
    else if (strcmp(workload, "normal")) { llvm_steps_end(&s); return 2; }
    u64 checksum = 0;
    double start = snx_seconds();
    for (unsigned i = 0; i < 1000000; ++i) {
        s.steps[0](&c);
        checksum += c.v[0].d[0] + (c.v[0].d[1] ^ c.fpsr);
    }
    double elapsed = snx_seconds() - start;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n", elapsed * 1e9, (unsigned long long)checksum);
    llvm_steps_end(&s);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--bench"))
        return bench(argv[2], argc > 3 ? argv[3][0] : '0',
                     argc > 4 && strcmp(argv[4], "--reference") ? argv[4] : NULL,
                     argc > 4 && !strcmp(argv[4], "--reference"));
    for (int jit = 0; jit < 2; jit++)
        for (int portable = 0; portable < 2; portable++)
            for (unsigned level = 0; level < 2; level++)
                if (run(jit, portable, level ? '2' : '0')) return 1;
    printf("FRECPE: %llu cases, %u failures\n", checked, failures);
    return failures != 0;
}
