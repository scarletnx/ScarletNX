/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A loop's turn on the arm64 back end with no poll pending: its backward branch falls through to LDR W16 of CPU.poll and
   a CBZ of it back to the loop's header (mach_codegen.inc's SGX_FN_JCC_BACK, arm64_codegen.c's poll_jz), the call of
   rt->poll out of line: one branch taken a turn, not a jump to a stub, its CBZ and a jump back. Each row's words and a
   RET are a function, made for a host of every optional feature; its code has at least the row's latches, a CBZ W16
   back to an earlier word right after that LDR. recompiler_arm64 runs such loops with a poll pending against LLVM.
     snx-test-arm64-loop-code [--print]
   --print: each row's latches, and nothing held */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 0x10000u

typedef struct {
    const char* what;
    unsigned latches;  /* the least CBZ W16 back right after LDR W16 of the poll */
    const char* words; /* the function's words before its RET, in hex */
} Row;

static const Row k_rows[] = {
    /* SUBS X10, X10, #1; B.NE back to it */
    {"a B.NE back", 1, "F100054A 54FFFFE1"},
    /* SUBS X10, X10, #1; B.EQ out; B back */
    {"a B back", 1, "F100054A 54000040 17FFFFFE"},
    /* SUB X10, X10, #1; CBNZ X10 back */
    {"a CBNZ back", 1, "D100054A B5FFFFEA"},
    /* two loops, one after the other: SUBS X10; B.NE; SUBS X11; B.NE */
    {"two loops", 2, "F100054A 54FFFFE1 F100056B 54FFFFE1"},
    /* a time loop of 100000 turns: eight of X1-X8 += X17, CLZ into X9, X21-X28 ^= X9 */
    {"a time loop of CLZ", 1,
     "D290D410 F2A00030 8B110021 DAC01029 CA0902B5 8B110042 DAC01049 CA0902D6 8B110063 DAC01069 CA0902F7 8B110084 "
     "DAC01089 CA090318 8B1100A5 DAC010A9 CA090339 8B1100C6 DAC010C9 CA09035A 8B1100E7 DAC010E9 CA09037B 8B110108 "
     "DAC01109 CA09039C F1000610 54FFFCE1"},
    /* FADD V0.4S (the guest's FPCR switched to around it); SUBS X10, X10, #1; B.NE back to the FADD */
    {"a B.NE back past FADD", 1, "4E22D420 F100054A 54FFFFC1"},
    /* FADD V0.4S; SUBS X10, X10, #1; B.EQ out; B back */
    {"a B back past FADD", 1, "4E22D420 F100054A 54000040 17FFFFFD"},
    /* a time loop of FADD: eight of V3-V10 += V31 (ADD .2D), FADD into V16-V23 and EOR into the accumulators */
    {"a time loop of FADD", 1,
     "D290D410 F2A00030 4EFF8463 4E22D470 6E301D6B 4EFF8484 4E22D491 6E311D8C 4EFF84A5 4E22D4B2 6E321DAD 4EFF84C6 "
     "4E22D4D3 6E331DCE 4EFF84E7 4E22D4F4 6E341DEF 4EFF8508 4E22D515 6E351F18 4EFF8529 4E22D536 6E361F39 4EFF854A "
     "4E22D557 6E371F5A F1000610 54FFFCE1"},
};

static struct {
    int failures;
} g_arm64_loop_code;

/* the code section of an ELF64 object, NULL where there is none */
static const unsigned char* text_of(const unsigned char* o, size_t size, size_t* length) {
    if (size < 64 || memcmp(o, "\177ELF", 4)) return NULL;
    uint64_t shoff = snx_ld_le64(o + 40);
    for (unsigned i = 0, n = snx_ld_le16(o + 60); i < n; ++i) {
        const unsigned char* sh = o + shoff + (size_t)i * 64;
        if (snx_ld_le32(sh + 4) != 1 || !(snx_ld_le64(sh + 8) & 4)) continue; /* PROGBITS, executable */
        *length = (size_t)snx_ld_le64(sh + 32);
        return o + snx_ld_le64(sh + 24);
    }
    return NULL;
}

/* CBZ W16 to an earlier word, right after LDR W16, [X19, #poll] */
static unsigned latches(const unsigned char* t, size_t n) {
    const SnxU32 ldr = 0xB9400000u | (SnxU32)((offsetof(CPU, poll) - SNX_ARM64_STATE_BIAS) / 4) << 10 | 19u << 5 | 16u;
    unsigned found = 0;
    for (size_t i = 4; i + 4 <= n; i += 4) {
        SnxU32 w = snx_ld_le32(t + i);
        if ((w & 0xFF00001Fu) == 0x34000010u && w >> 23 & 1 && snx_ld_le32(t + i - 4) == ldr) ++found;
    }
    return found;
}

int main(int argc, char** argv) {
    int print = argc > 1 && !strcmp(argv[1], "--print");
    if (argc > 1 && !print) {
        fprintf(stderr, "%s: no such option\n", argv[1]);
        return 2;
    }
    for (size_t r = 0; r < sizeof k_rows / sizeof k_rows[0]; ++r) {
        const Row* row = &k_rows[r];
        unsigned char image[256], *object = NULL, made[4] = {0};
        const unsigned char* text;
        const char* p = row->words;
        const SnxU64 hint = BASE;
        SnxNativeOptions options = {.stale = 1};
        SnxCodegen codegen;
        size_t count = 0, size = 0, length = 0;
        char error[256] = "", *end;
        for (;; p = end) {
            unsigned long w = strtoul(p, &end, 16);
            if (end == p) break;
            snx_st_le32(image + 4 * count++, (SnxU32)w);
        }
        snx_st_le32(image + 4 * count++, 0xD65F03C0u);
        SnxCodeImage module = {.image = image, .base = BASE, .image_size = 4 * count, .text_size = 4 * count};
        if (snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, SNX_A64_TARGET_MAX) || codegen.function_count != 1) {
            fprintf(stderr, "FAIL: %s: not one function\n", row->what);
            ++g_arm64_loop_code.failures;
            continue;
        }
        int made_it =
            snx_arm64_object(&codegen, 0, 1, &options, SNX_ARM64_F_ALL, made, &object, &size, error, sizeof error) >= 0;
        if (!made_it || !made[0] || !(text = text_of(object, size, &length))) {
            fprintf(stderr, "FAIL: %s: not made: %s\n", row->what, error);
            ++g_arm64_loop_code.failures;
        } else {
            unsigned got = latches(text, length);
            if (print)
                printf("%-24s %u latches\n", row->what, got);
            else if (got < row->latches) {
                fprintf(stderr, "FAIL: %s: %u loops' turns end in a CBZ of the poll back to the header, not %u\n",
                        row->what, got, row->latches);
                ++g_arm64_loop_code.failures;
            }
        }
        free(object);
        snx_codegen_free(&codegen);
    }
    printf("arm64 loop code: %zu rows, %d failures\n", sizeof k_rows / sizeof k_rows[0], g_arm64_loop_code.failures);
    return print ? 0 : g_arm64_loop_code.failures != 0;
}
