/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the arm64 back end stores and loads of the guest registers it keeps in host ones (its pins), in functions of
   a call: each row's words and a RET are a function, whose code has at most the row's stores of the pins to the state
   and loads of them back, and of ADDs of X19 to reach a field (X19 holds the state's address SNX_ARM64_STATE_BIAS past
   it, so the V registers' 16 bytes take LDR's scaled offset). A call stores the pins written since the state was last in step with them, and reloads the
   ones live where it returns; the prologue loads those live at the entry (mach_codegen.inc's pin_dirt and live sets).
   Every pin stored and loaded at every call and exit is some times over. recompiler_arm64's br_pins, undef_pins and
   calls hold what such code computes to LLVM's.
     snx-test-arm64-pins-code [--print]
   --print: each row's stores and loads of pins, and nothing held */
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
#define MOST 16u

typedef struct {
    const char* what;
    unsigned xstores, xloads; /* the most stores and loads of X pins (X20-X28 to or from the state's X) */
    unsigned vstores, vloads; /* the same of V pins (V24-V31 to or from the state's V) */
    unsigned adds;            /* the most ADDs of X16 = X19 + an immediate, the address of a field made first */
    const char* words;        /* the function's words before its RET, in hex */
} Row;

static const Row k_rows[] = {
    /* a loop (pins are a loop's) writing x1, x9 and x10 and reading x6, x7 and x8, then a call, then x0 from x9 and x1:
       the stores are of the three written (at the call, the loop's poll, the dispatch block), none at the exits; every
       pin at each of those is 42 stores and 24 loads */
    {"X pins: a loop, then a call", 12, 18, 0, 0, 0, "F94000C1 8B070021 8B080129 F100054A 54FFFF81 97FFFFFB 8B010120"},
    /* the same of V: v2 and v3 written in the loop, v0 and v1 only read, then a call and v4 from v2 and v3; every pin
       is 28 stores and 16 loads of V */
    {"V pins: a loop, then a call", 4, 3, 8, 12, 0, "4E20D442 6E21DC63 F100054A 54FFFFA1 97FFFFFC 4E23D444 3D800084"},
};

static struct {
    const Row* row; /* the one being checked, for the messages */
    int failures;
} g_arm64_pins_code;
static void fail(const char* what, unsigned got, unsigned most) {
    fprintf(stderr, "FAIL: %s: %u %s, at most %u\n", g_arm64_pins_code.row->what, got, what, most);
    ++g_arm64_pins_code.failures;
}

/* the function's code in an ELF64 object of one: its executable section, whose size the caller takes; NULL where
   there is none */
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
/* the stores (load 0) or loads of pins in the code's words, by the state's field each reaches (X19 holds its address
   SNX_ARM64_STATE_BIAS past it): STR or LDR Xt of X20-X28, unsigned offset or unscaled (STUR, LDUR), at [X19] in the
   state's X; STR or LDR Qt (unsigned offset) of V24-V31 at [X19] in the state's V, or at [X16] an ADD X16, X19, #off
   just before makes. *adds: the ADDs of X16 = X19 + an immediate */
static void pin_moves(const unsigned char* t, size_t n, int load, unsigned* x, unsigned* v, unsigned* adds) {
    const uint32_t xop = load ? 0xF9400000u : 0xF9000000u, uop = load ? 0xF8400000u : 0xF8000000u,
                   qop = load ? 0x3DC00000u : 0x3D800000u;
    const long long bias = SNX_ARM64_STATE_BIAS, xs = offsetof(CPU, x), vs = offsetof(CPU, v);
    uint32_t prev = 0;
    *x = *v = *adds = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w = snx_ld_le32(t + i), rt = w & 31, rn = w >> 5 & 31;
        long long off = -1;
        if ((w & 0xFFC003FFu) == (0x91000000u | 19u << 5 | 16u)) ++*adds;
        if ((w & 0xFFC00000u) == xop && rn == 19 && rt >= 20 && rt <= 28)
            off = (w >> 10 & 0xFFF) * 8 + bias;
        else if ((w & 0xFFE00C00u) == uop && rn == 19 && rt >= 20 && rt <= 28) {
            long long i9 = w >> 12 & 0x1FF;
            off = (i9 & 0x100 ? i9 - 0x200 : i9) + bias;
        }
        if (off >= xs && off < xs + 31 * 8) ++*x;
        if ((w & 0xFFC00000u) == qop && rt >= 24) {
            off = -1;
            if (rn == 19)
                off = (w >> 10 & 0xFFF) * 16 + bias;
            else if (rn == 16 && !(w >> 10 & 0xFFF) && (prev & 0xFFC003FFu) == (0x91000000u | 19u << 5 | 16u))
                off = (prev >> 10 & 0xFFF) + bias;
            if (off >= vs && off < vs + 32 * 16) ++*v;
        }
        prev = w;
    }
}

static void check(const Row* row, int print) {
    unsigned char image[(MOST + 1) * 4], *object = NULL;
    const unsigned char* text;
    const SnxU64 hint = BASE;
    SnxCodeImage module = {.image = image, .base = BASE};
    SnxNativeOptions options = {0};
    unsigned char made[4] = {0};
    uint32_t words[MOST];
    size_t size = 0, length = 0;
    char error[256] = "", *at = (char*)row->words;
    SnxCodegen codegen;
    unsigned count = 0, xs, vs, xl, vl, adds;
    g_arm64_pins_code.row = row;
    while (*at && count < MOST) words[count++] = (uint32_t)strtoul(at, &at, 16);
    for (unsigned i = 0; i <= count; ++i)
        for (unsigned b = 0; b < 4; ++b)
            image[4 * i + b] = (unsigned char)((i < count ? words[i] : 0xD65F03C0u) >> (8 * b));
    module.image_size = module.text_size = (count + 1) * 4;
    if (snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, SNX_A64_TARGET_SWITCH) || codegen.function_count != 1) {
        fail("functions the analysis found in its words", (unsigned)codegen.function_count, 1);
        return;
    }
    options.stale = 1;
    if (snx_arm64_object(&codegen, 0, 1, &options, 0, made, &object, &size, error, sizeof error) < 0 || !made[0]) {
        fprintf(stderr, "FAIL: %s: not made: %s\n", row->what, error);
        ++g_arm64_pins_code.failures;
    } else if (!(text = text_of(object, size, &length))) {
        fprintf(stderr, "FAIL: %s: no code in its object of %zu bytes\n", row->what, size);
        ++g_arm64_pins_code.failures;
    } else {
        pin_moves(text, length, 0, &xs, &vs, &adds);
        pin_moves(text, length, 1, &xl, &vl, &adds);
        if (print)
            printf("%s: %zu bytes; X pins %u stored, %u loaded; V pins %u stored, %u loaded; %u ADDs of X19\n",
                   row->what, length, xs, xl, vs, vl, adds);
        else {
            if (xs > row->xstores) fail("stores of X pins", xs, row->xstores);
            if (xl > row->xloads) fail("loads of X pins", xl, row->xloads);
            if (vs > row->vstores) fail("stores of V pins", vs, row->vstores);
            if (vl > row->vloads) fail("loads of V pins", vl, row->vloads);
            if (adds > row->adds) fail("ADDs of X19 before a field's load or store", adds, row->adds);
        }
    }
    free(object);
    snx_codegen_free(&codegen);
}

int main(int argc, char** argv) {
    int print = argc > 1 && !strcmp(argv[1], "--print");
    if (argc > 1 && !print) {
        fprintf(stderr, "%s: no such option\n", argv[1]);
        return 2;
    }
    for (size_t i = 0; i < sizeof k_rows / sizeof *k_rows; ++i) check(&k_rows[i], print);
    printf("arm64 pins code: %zu rows, %d failures\n", sizeof k_rows / sizeof *k_rows, g_arm64_pins_code.failures);
    return g_arm64_pins_code.failures != 0;
}
