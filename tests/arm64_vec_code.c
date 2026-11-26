/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the arm64 back end makes of SIMD&FP words whose results recompiler_arm64 holds to LLVM's, by the size of its
   code and its calls: each row's words and a RET are a function, whose code takes at most the row's bytes and makes
   at most the row's calls (BL, BLR). A word the host runs as its own is one instruction and no call; the same made by a
   helper's call is some times over.
     snx-test-arm64-vec-code [--print]
   --print: each row's bytes and calls, and nothing held */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/aarch64/a64_decoder.h"
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
    unsigned bytes, calls; /* the most the function takes, and makes */
    const char* words;     /* the function's words before its RET, in hex */
} Row;

static const Row k_rows[] = {
    /* FCVTNS W1, S2; FCVTNU X3, D4; FCVTAS W5, D6; FCVTAU X7, S16; FCVTPS X9, D10; FCVTPU W8, S0; FCVTMS W11, S12;
       FCVTMU X13, D14; FCVTZS W15, D2; FCVTZU X19, S4: a float to a W or X by each rounding */
    {"the conversions to a W or X", 360, 0,
     "1E200041 9E610083 1E6400C5 9E250207 9E680149 1E290008 1E30018B 9E7101CD 1E78004F 9E390093"},
    /* FCVTZS X12, D3, #8; UCVTF S14, W6, #16: of a fixed point */
    {"a fixed point's", 240, 0, "9E58E06C 1E03C0CE"},
    /* SCVTF S20, W1; UCVTF D21, X3; SCVTF D22, XZR: a W or X to a float */
    {"the conversions from a W or X", 260, 0, "1E220034 9E630075 9E6203F6"},
};

static struct {
    const Row* row; /* the one being checked, for the messages */
    int failures;
} g_arm64_vec_code;
static void fail(const char* what, size_t got, size_t most) {
    fprintf(stderr, "FAIL: %s: %zu %s, at most %zu\n", g_arm64_vec_code.row->what, got, what, most);
    ++g_arm64_vec_code.failures;
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
/* the code's calls: BL and BLR */
static size_t calls_of(const unsigned char* t, size_t n) {
    size_t calls = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w = snx_ld_le32(t + i);
        calls += (w & 0xFC000000u) == 0x94000000u || (w & 0xFFFFFC1Fu) == 0xD63F0000u;
    }
    return calls;
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
    unsigned count = 0;
    g_arm64_vec_code.row = row;
    while (*at && count < MOST) words[count++] = (uint32_t)strtoul(at, &at, 16);
    for (unsigned i = 0; i <= count; ++i)
        for (unsigned b = 0; b < 4; ++b)
            image[4 * i + b] = (unsigned char)((i < count ? words[i] : 0xD65F03C0u) >> (8 * b));
    module.image_size = module.text_size = (count + 1) * 4;
    if (snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, SNX_A64_TARGET_SWITCH) || codegen.function_count != 1) {
        fail("functions the analysis found in its words", codegen.function_count, 1);
        return;
    }
    if (snx_arm64_object(&codegen, 0, 1, &options, 0, made, &object, &size, error, sizeof error) < 0 || !made[0]) {
        fprintf(stderr, "FAIL: %s: not made: %s\n", row->what, error);
        ++g_arm64_vec_code.failures;
    } else if (!(text = text_of(object, size, &length))) {
        fprintf(stderr, "FAIL: %s: no code in its object of %zu bytes\n", row->what, size);
        ++g_arm64_vec_code.failures;
    } else if (print)
        printf("%s: %zu bytes, %zu calls\n", row->what, length, calls_of(text, length));
    else {
        if (length > row->bytes) fail("bytes of code", length, row->bytes);
        if (calls_of(text, length) > row->calls) fail("calls", calls_of(text, length), row->calls);
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
    printf("arm64 vec code: %zu rows, %d failures\n", sizeof k_rows / sizeof *k_rows, g_arm64_vec_code.failures);
    return g_arm64_vec_code.failures != 0;
}
