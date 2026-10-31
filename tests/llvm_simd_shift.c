/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Every signed low-byte count, each lane size, rounding/saturation and register aliasing against the exact helper. */
#include "llvm_steps.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const u32 words[] = {0x4E225420u, 0x6E225420u, 0x4E224C20u, 0x6E224C20u, 0x4E225C20u, 0x6E225C20u};
static const unsigned flags[] = {2, 3, 4, 5, 6, 7};
static u64 random_next(u64* rng) { *rng ^= *rng << 13; *rng ^= *rng >> 7; *rng ^= *rng << 17; return *rng; }

static int run(int jit, int portable, char opt, unsigned long long* checked) {
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, jit, portable, opt, error, sizeof error)) return snx_error("%s", error);
    for (unsigned op = 0; op < 6; ++op)
        for (unsigned esz = 0; esz < 4; ++esz)
            for (unsigned form = 0; form < 2; ++form)
                for (unsigned alias = 0; alias < 3; ++alias) {
                    u32 word = words[op] | esz << 22 | alias;
                    if (form) word = esz == 3 ? word | 0x10000000u : word & ~0x40000000u;
                    llvm_steps_add(&steps, word, 0x1000, 0);
                }
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    u64 rng = 0x7af34b06c92815deull;
    size_t index = 0;
    int failed = 0;
    for (unsigned op = 0; op < 6 && !failed; ++op)
        for (unsigned esz = 0; esz < 4 && !failed; ++esz)
            for (unsigned form = 0; form < 2 && !failed; ++form)
                for (unsigned alias = 0; alias < 3 && !failed; ++alias, ++index) {
                    unsigned bits = 8u << esz, lanes = esz == 3 && form ? 1 : (form ? 8u : 16u) >> esz;
                    u64 top = bits == 64 ? ~0ull : (1ull << bits) - 1;
                    const u64 edges[] = {0, 1, top, top >> 1, (top >> 1) + 1, 3, top - 1};
                    for (unsigned shift = 0; shift < 256 && !failed; ++shift)
                        for (unsigned pattern = 0; pattern < 9 && !failed; ++pattern) {
                            CPU actual = {0};
                            actual.pc = 0x1000;
                            actual.fpcr = 0x07c00000u;
                            actual.fpsr = pattern & 1 ? 0x0800009fu : 0x9f;
                            for (unsigned v = 0; v < 32; ++v)
                                for (unsigned h = 0; h < 2; ++h) actual.v[v].d[h] = random_next(&rng);
                            for (unsigned lane = 0; lane < lanes; ++lane) {
                                u64 value = pattern < 7 ? edges[pattern] : random_next(&rng);
                                u64 count = shift | (random_next(&rng) & ~255ull);
                                u8* a = actual.v[1].b + lane * (1u << esz);
                                u8* b = actual.v[2].b + lane * (1u << esz);
                                for (unsigned byte = 0; byte < (1u << esz); ++byte) {
                                    a[byte] = (u8)(value >> (byte * 8));
                                    b[byte] = (u8)(count >> (byte * 8));
                                }
                            }
                            CPU expected = actual;
                            snx_vec_shl(&expected, alias, 1, 2, esz | lanes << 2 | flags[op] << 8);
                            if (steps.steps[index](&actual) != 0x1004 || memcmp(&actual, &expected, sizeof actual)) {
                                fprintf(stderr, "SIMD shift op%u bits%u form%u alias%u count%u pattern%u O%c jit%d portable%d\n",
                                        op, bits, form, alias, shift, pattern, opt, jit, portable);
                                failed = 1;
                            }
                            ++*checked;
                        }
                }
    llvm_steps_end(&steps);
    return failed;
}

static int bench(unsigned esz, char opt, int reference, const char* dump) {
    if (esz > 3) return 2;
    LlvmSteps steps;
    char error[1024];
    if (llvm_steps_begin_opt(&steps, 1, reference, opt, error, sizeof error)) return snx_error("%s", error);
    llvm_steps_add(&steps, words[5] | esz << 22, 0x1000, 0);
    if (dump) {
        int failed=llvm_steps_dump(&steps,dump,error,sizeof error);
        if(failed)fprintf(stderr,"%s\n",error);
        llvm_steps_end(&steps);return failed!=0;
    }
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);
    CPU cpu = {0};
    cpu.pc = 0x1000;
    for (unsigned i = 0; i < 16; ++i) { cpu.v[1].b[i] = (u8)(i * 37 + 73); cpu.v[2].b[i] = (u8)(i & 1 ? 3 : -3); }
    u64 checksum = 0;
    double start = snx_seconds();
    for (unsigned i = 0; i < 1000000; ++i) {
        steps.steps[0](&cpu);
        checksum += cpu.v[0].d[0] + (cpu.v[0].d[1] ^ cpu.fpsr);
    }
    double elapsed = snx_seconds() - start;
    printf("{\"elapsed_ns\":%.0f,\"checksum\":\"%016llx\"}\n", elapsed * 1e9, (unsigned long long)checksum);
    llvm_steps_end(&steps);
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 3 && !strcmp(argv[1], "--bench"))
        return bench((unsigned)strtoul(argv[2], NULL, 10), argc > 3 ? argv[3][0] : '0',
                     argc > 4 && !strcmp(argv[4], "--reference"), argc > 5 ? argv[5] : NULL);
    unsigned long long checked = 0;
    for (int jit = 0; jit < 2; ++jit)
        for (int portable = 0; portable < 2; ++portable)
            for (unsigned level = 0; level < 2; ++level)
                if (run(jit, portable, level ? '2' : '0', &checked)) return 1;
    printf("SIMD shifts: %llu exact cases\n", checked);
    return 0;
}
