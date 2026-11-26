/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the x86-64 back end makes of Advanced SIMD words whose results recompiler_backends holds to LLVM's, by the size
   of its code: each row's words and a RET are a function, whose code takes at most the row's bytes. A form that works
   on whole vectors is under it; the same made a lane at a time, or by a helper's call, is some times over. And how
   often the code writes FPSR, where the row says: once for all the saturating words of a block; and an instruction it
   has, where the row says, for a form no longer than the one it replaced (GF2P8AFFINEQB). Each row says the features
   it needs (sailgen's SGX_F_*); without them on this host it is left out, and goes without the ones it names.
     snx-test-x64-vec-code [--print]
   --print: each row's bytes and FPSR writes, and nothing held. On a host without the back end it passes, saying so */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE          0x10000u
#define MOST          12u
#define NEEDS_AVX2    32768u
#define NEEDS_GFNI    65536u
/* GF2P8AFFINEQB xmm, xmm/m128, imm8 (66 0F 3A CE) and its VEX.128 form (C4, two bytes, CE) */
#define GF2P8AFFINEQB "66 0F 3A CE|C4 ?? ?? CE"

typedef struct {
    const char* what;
    unsigned needs, without; /* the host's features; those it goes without */
    unsigned bytes;          /* the most the function takes */
    int fpsr;                /* the writes of FPSR, -1 not counted */
    const char* has;         /* bytes the code has (hex, ?? any), one form's encodings or another's (|), or NULL */
    const char* words;       /* the function's words before its RET, in hex */
} Row;

static const Row k_rows[] = {
    /* FPSR.QC of a block's saturating words: one write of the state, at the block's end, before a compare's flags, an
       MRS of FPSR, and a branch out (SQADD, UQSUB, SQXTN, SQRDMULH; SQADD, MRS x0, FPSR, SQADD; SQADD, SUBS, B.NE) */
    {"SQADD, UQSUB, SQXTN, SQRDMULH of one block", 0, 0, 576, 1, NULL, "4E220C20 6EA52C83 0E2148E6 6E6AB528"},
    {"SQADD, MRS FPSR, SQADD", 0, 0, 432, 2, NULL, "4E220C20 D53B4420 4EA50C83"},
    {"SQADD, SUBS, B.NE back to the SQADD", 0, 0, 480, 1, NULL, "4E220C20 F1000610 54FFFFC1"},
    {"suqadd v0.16b, v1.16b", 0, 0, 304, -1, NULL, "4E203820"},
    {"usqadd v0.8h, v1.8h", 0, 0, 304, -1, NULL, "6E603820"},
    {"sqabs v0.16b, v1.16b", 0, 0, 288, -1, NULL, "4E207820"},
    {"sqneg v0.4s, v1.4s", 0, 0, 304, -1, NULL, "6EA07820"},
    {"sqabs v0.2d, v1.2d", 0, 0, 304, -1, NULL, "4EE07820"},
    {"sshr v0.16b, v1.16b, #3", NEEDS_GFNI, 0, 240, -1, GF2P8AFFINEQB, "4F0D0420"},
    {"srshr v0.16b, v1.16b, #3", NEEDS_GFNI, 0, 288, -1, NULL, "4F0D2420"},
    {"rbit v0.16b, v1.16b", NEEDS_GFNI, 0, 240, -1, NULL, "6E605820"},
    {"rbit v0.16b, v1.16b", 0, NEEDS_GFNI, 320, -1, NULL, "6E605820"},
    {"rbit w0, w1", NEEDS_GFNI, 0, 240, -1, NULL, "5AC00020"},
    {"rbit x0, x1", NEEDS_GFNI, 0, 240, -1, NULL, "DAC00020"},
    {"rbit w0, w1", 0, NEEDS_GFNI, 272, -1, NULL, "5AC00020"},
    {"rbit x0, x1", 0, NEEDS_GFNI, 336, -1, NULL, "DAC00020"},
    {"rev w0, w1", 0, NEEDS_GFNI, 192, -1, NULL, "5AC00820"},
    {"rev x0, x1", 0, NEEDS_GFNI, 192, -1, NULL, "DAC00C20"},
    {"rev16 x0, x1", 0, NEEDS_GFNI, 240, -1, NULL, "DAC00420"},
    {"rev16 w0, w1", 0, NEEDS_GFNI, 208, -1, NULL, "5AC00420"},
    {"ushl v0.16b, v1.16b, v2.16b", 0, 0, 512, -1, NULL, "6E224420"},
    {"sshl v0.8h, v1.8h, v2.8h", 0, 0, 576, -1, NULL, "4E624420"},
    {"srshl v0.16b, v1.16b, v2.16b", 0, 0, 560, -1, NULL, "4E225420"},
    {"uqshl v0.16b, v1.16b, v2.16b", 0, 0, 640, -1, NULL, "6E224C20"},
    {"sqshl v0.8h, v1.8h, v2.8h", 0, 0, 784, -1, NULL, "4E624C20"},
    {"sqrshl v0.16b, v1.16b, v2.16b", 0, 0, 768, -1, NULL, "4E225C20"},
    {"ushl v0.4s, v1.4s, v2.4s", NEEDS_AVX2, 0, 288, -1, NULL, "6EA24420"},
    {"sshl v0.2d, v1.2d, v2.2d", NEEDS_AVX2, 0, 320, -1, NULL, "4EE24420"},
    {"sqrshl v0.4s, v1.4s, v2.4s", NEEDS_AVX2, 0, 480, -1, NULL, "4EA25C20"},
    {"uqrshl v0.2d, v1.2d, v2.2d", NEEDS_AVX2, 0, 448, -1, NULL, "6EE25C20"},
    {"sqshl v0.16b, v1.16b, #3", 0, 0, 416, -1, NULL, "4F0B7420"},
    {"uqshl v0.8h, v1.8h, #3", 0, 0, 304, -1, NULL, "6F137420"},
    {"sqshlu v0.4s, v1.4s, #3", 0, 0, 320, -1, NULL, "6F236420"},
    {"srhadd v0.16b, v1.16b, v2.16b", 0, 0, 272, -1, NULL, "4E221420"},
    {"raddhn v0.8b, v1.8h, v2.8h", 0, 0, 304, -1, NULL, "2E224020"},
    {"sqdmull v0.4s, v1.4h, v2.4h", 0, 0, 320, -1, NULL, "0E62D020"},
    {"sqdmlal v0.2d, v1.2s, v2.2s", 0, 0, 400, -1, NULL, "0EA29020"},
};

static struct {
    const Row* row; /* the one being checked, for the messages */
    int failures;
} g_x64_vec_code;
static void fail(const char* what, size_t got, size_t most) {
    fprintf(stderr, "FAIL: %s: %s %zu, not %zu\n", g_x64_vec_code.row->what, what, got, most);
    ++g_x64_vec_code.failures;
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
/* the stores of a 32-bit register in FPSR's field: mov [rbx + disp32], r32 (REX.R for the upper ones) */
static int fpsr_writes(const unsigned char* t, size_t n) {
    int writes = 0;
    for (size_t i = 0; i + 6 <= n; ++i)
        if (t[i] == 0x89 && (t[i + 1] & 0xC7) == 0x83 && snx_ld_le32(t + i + 2) == offsetof(CPU, fpsr)) ++writes;
    return writes;
}
/* where t has one of has's byte strings */
static int has_bytes(const unsigned char* t, size_t n, const char* has) {
    while (*has) {
        int want[16], k = 0;
        for (; *has && *has != '|' && k < 16; ++k) {
            while (*has == ' ') ++has;
            want[k] = has[0] == '?' ? -1 : (int)strtoul((char[3]){has[0], has[1], 0}, NULL, 16);
            has += 2;
            while (*has == ' ') ++has;
        }
        for (size_t i = 0; i + (size_t)k <= n; ++i) {
            int j = 0;
            while (j < k && (want[j] < 0 || t[i + (size_t)j] == want[j])) ++j;
            if (j == k) return 1;
        }
        if (*has == '|') ++has;
    }
    return 0;
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
    g_x64_vec_code.row = row;
    while (*at && count < MOST) words[count++] = (uint32_t)strtoul(at, &at, 16);
    for (unsigned i = 0; i <= count; ++i)
        for (unsigned b = 0; b < 4; ++b)
            image[4 * i + b] = (unsigned char)((i < count ? words[i] : 0xD65F03C0u) >> (8 * b));
    module.image_size = module.text_size = (count + 1) * 4;
    if (snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, SNX_A64_TARGET_SWITCH) || codegen.function_count != 1) {
        fail("functions the analysis found in its words", 0, 1);
        return;
    }
    snx_x64_without(row->without);
    if (snx_x64_object(&codegen, 0, 1, &options, made, &object, &size, error, sizeof error) < 0 || !made[0]) {
        fprintf(stderr, "FAIL: %s: not made: %s\n", row->what, error);
        ++g_x64_vec_code.failures;
    } else if (!(text = text_of(object, size, &length))) {
        fail("bytes of its object, and no code in them", size, 0);
    } else if (print)
        printf("%s: %zu bytes, %d writes of FPSR\n", row->what, length, fpsr_writes(text, length));
    else {
        if (length > row->bytes) fail("bytes of code", length, row->bytes);
        if (row->fpsr >= 0 && fpsr_writes(text, length) != row->fpsr)
            fail("writes of FPSR", (size_t)fpsr_writes(text, length), (size_t)row->fpsr);
        if (row->has && !has_bytes(text, length, row->has)) {
            fprintf(stderr, "FAIL: %s: no %s in its code\n", row->what, row->has);
            ++g_x64_vec_code.failures;
        }
    }
    snx_x64_without(0);
    free(object);
    snx_codegen_free(&codegen);
}

int main(int argc, char** argv) {
    int print = argc > 1 && !strcmp(argv[1], "--print");
    unsigned done = 0;
    if (argc > 1 && !print) {
        fprintf(stderr, "%s: no such option\n", argv[1]);
        return 2;
    }
    if (!snx_x64_available()) {
        printf("SKIP: no x86-64 back end on this host\n");
        return 77;
    }
    for (size_t i = 0; i < sizeof k_rows / sizeof *k_rows; ++i) {
        if ((snx_x64_features() & k_rows[i].needs) != k_rows[i].needs) continue;
        check(&k_rows[i], print);
        ++done;
    }
    printf("x64 vec code: %u of %zu rows, %d failures\n", done, sizeof k_rows / sizeof *k_rows,
           g_x64_vec_code.failures);
    return g_x64_vec_code.failures != 0;
}
