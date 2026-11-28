/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Where the native back ends cut a long block into regions (mach_codegen.inc's instruction: a region near its room's
   end goes to the state and a new one starts). A function of n of one word and a RET is one block; the least n with one
   region more is where its first cut falls, the least with two more its second. The pieces of the function's entry
   wait at the first region's start (region_glue) and none waits at the second's, so both hold as many of the block's
   instructions only where the cut counts the block's own: the second cut at twice the first. Counting the waiting
   pieces too, the first region is cut short and the second cut is not at twice the first.
     snx-test-native-block-cuts [--print]
   --print: each row's two cuts, and nothing held */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 0x10000u
#define MOST 20000u /* words of the longest function: several regions of the plainest word */

typedef struct {
    const char* what;
    SnxU32 word;
    int x64; /* held on the x86-64 back end too */
} Row;

static const Row k_rows[] = {
    {"MOV X1, X2", 0xAA0203E1u, 1}, /* a region may hold three quarters of its room of these */
    /* and a quarter before a SIMD&FP word. x86-64's regions after the first begin with an instruction more of the
       block's own, so its second holds one FMOV fewer than its first whatever waits: arm64's are alike */
    {"FMOV D1, D2", 0x1E604041u, 0},
};

static struct {
    int failures;
} g_native_block_cuts;

/* the regions the back end made of a function of n of word and a RET: 0 where it made none */
static SnxU64 regions(int arm64, SnxU32 word, unsigned n) {
    static unsigned char image[4 * (MOST + 1)];
    unsigned char made[4] = {0}, *object = NULL;
    const SnxU64 hint = BASE;
    SnxSsaCount ssa = {0};
    SnxNativeOptions options = {.ssa = &ssa};
    SnxCodegen codegen;
    size_t size = 0;
    char error[256] = "";
    int rc;
    for (unsigned i = 0; i < n; ++i) snx_st_le32(image + 4 * i, word);
    snx_st_le32(image + 4 * n, 0xD65F03C0u);
    SnxCodeImage module = {.image = image, .base = BASE, .image_size = 4 * (n + 1), .text_size = 4 * (n + 1)};
    if (snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, SNX_A64_TARGET_MAX) || codegen.function_count != 1)
        return 0;
    rc = arm64 ? snx_arm64_object(&codegen, 0, 1, &options, SNX_ARM64_F_ALL, made, &object, &size, error, sizeof error)
               : snx_x64_object(&codegen, 0, 1, &options, made, &object, &size, error, sizeof error);
    free(object);
    snx_codegen_free(&codegen);
    return rc >= 0 && made[0] ? ssa.region_count : 0;
}

/* the least n whose function has at least want regions: 0 where MOST words have fewer */
static unsigned least(int arm64, SnxU32 word, SnxU64 want) {
    unsigned below = 1, at = MOST;
    if (regions(arm64, word, at) < want) return 0;
    while (at - below > 1) {
        const unsigned mid = below + (at - below) / 2;
        if (regions(arm64, word, mid) >= want)
            at = mid;
        else
            below = mid;
    }
    return at;
}

static void check(const char* machine, int arm64, const Row* row, int print) {
    const SnxU64 one = regions(arm64, row->word, 1);
    const unsigned first = one ? least(arm64, row->word, one + 1) : 0;
    const unsigned second = first ? least(arm64, row->word, one + 2) : 0;
    if (print)
        printf("%s, %s: %llu regions of one word, the cuts at %u and %u\n", machine, row->what, (unsigned long long)one,
               first, second);
    else if (!first || !second) {
        fprintf(stderr, "FAIL: %s, %s: %u words are not cut twice (%llu regions of one word, a cut at %u)\n", machine,
                row->what, MOST, (unsigned long long)one, first);
        ++g_native_block_cuts.failures;
    } else if (second != 2 * first) {
        fprintf(stderr,
                "FAIL: %s, %s: the block's first region holds %u of its instructions and its second %u: the cut "
                "counts what waits before the block\n",
                machine, row->what, first, second - first);
        ++g_native_block_cuts.failures;
    }
}

int main(int argc, char** argv) {
    int print = argc > 1 && !strcmp(argv[1], "--print");
    if (argc > 1 && !print) {
        fprintf(stderr, "%s: no such option\n", argv[1]);
        return 2;
    }
    for (size_t r = 0; r < sizeof k_rows / sizeof k_rows[0]; ++r) {
        if (snx_x64_available() && (k_rows[r].x64 || print)) check("x86-64", 0, &k_rows[r], print);
        check("arm64", 1, &k_rows[r], print);
    }
    printf("native block cuts: %zu rows, %d failures\n", sizeof k_rows / sizeof k_rows[0],
           g_native_block_cuts.failures);
    return print ? 0 : g_native_block_cuts.failures != 0;
}
