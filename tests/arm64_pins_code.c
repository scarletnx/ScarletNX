/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What the arm64 back end stores and loads of the guest registers it keeps in host ones (its pins), in functions of
   a call: each row's words and a RET are a function, whose code a call of its start runs (every branch both ways but
   the entry's check, CMP X1, X16 then B.NE: the way of another target, which takes a frame of its own) has at most the
   row's stores of the pins to the state and loads of them back, and of ADDs of X19 to reach a field (X19 holds the state's address SNX_ARM64_STATE_BIAS past
   it, so the V registers' 16 bytes take LDR's scaled offset). A call stores the pins written since the state was last in step with them, and reloads the
   ones live where it returns; the prologue loads those live at the entry (mach_codegen.inc's pin_dirt and live sets).
   Every pin stored and loaded at every call and exit is some times over. recompiler_arm64's br_pins, undef_pins and
   calls hold what such code computes to LLVM's.
     snx-test-arm64-pins-code [--print]
   --print: each row's stores and loads of pins on that path and in the whole code, and nothing held */
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
#define MOST 96u

typedef struct {
    const char* what;
    unsigned xstores, xloads; /* the most stores and loads of X pins (X20-X28 to or from the state's X) */
    unsigned vstores, vloads; /* the same of V pins (V24-V31 to or from the state's V) */
    unsigned adds;            /* the most ADDs of X16 = X19 + an immediate, the address of a field made first */
    unsigned tables;          /* the least jumps through a table (LDRSW X16, [X17, X16]; ADD; BR X16) */
    const char* words;        /* the function's words before its RET, in hex */
} Row;

static const Row k_rows[] = {
    /* a loop (pins are a loop's) writing x1, x9 and x10 and reading x6, x7 and x8, then a call, then x0 from x9 and x1:
       the stores are of the three written (at the call and the loop's poll), none at the exits */
    {"X pins: a loop, then a call", 6, 11, 0, 0, 0, 0,
     "F94000C1 8B070021 8B080129 F100054A 54FFFF81 97FFFFFB 8B010120"},
    /* the same of V: v2 and v3 written in the loop, v0 and v1 only read, then a call and v4 from v2 and v3 */
    {"V pins: a loop, then a call", 2, 2, 4, 8, 0, 0,
     "4E20D442 6E21DC63 F100054A 54FFFFA1 97FFFFFC 4E23D444 3D800084"},
    /* a switch of 32 cases through a compressed byte table (recompiler_library.c's switch64): CMP W0, #31; B.HI the
       default; ADR, LDRB, ADD, BR; each case MOVZ X0 and RET; then the table's bytes. Its BR jumps through a table of
       the function's own, past the poll and the range check, not along the dispatch block's 34 compares */
    {"a switch of 32 cases", 99, 99, 99, 99, 99, 1,
     "71007C1F 540008E8 2A0003E8 100008E9 1000008A 3868692B 8B0B094A D61F0140 D2800060 D65F03C0 D2800140 D65F03C0 "
     "D2800220 D65F03C0 D2800300 D65F03C0 D28003E0 D65F03C0 D28004C0 D65F03C0 D28005A0 D65F03C0 D2800680 D65F03C0 "
     "D2800760 D65F03C0 D2800840 D65F03C0 D2800920 D65F03C0 D2800A00 D65F03C0 D2800AE0 D65F03C0 D2800BC0 D65F03C0 "
     "D2800CA0 D65F03C0 D2800D80 D65F03C0 D2800E60 D65F03C0 D2800F40 D65F03C0 D2801020 D65F03C0 D2801100 D65F03C0 "
     "D28011E0 D65F03C0 D28012C0 D65F03C0 D28013A0 D65F03C0 D2801480 D65F03C0 D2801560 D65F03C0 D2801640 D65F03C0 "
     "D2801720 D65F03C0 D2801800 D65F03C0 D28018E0 D65F03C0 D28019C0 D65F03C0 D2801AA0 D65F03C0 D2801B80 D65F03C0 "
     "D2807CE0 D65F03C0 1E140A00 063C3228 2E241A10 160C0238 3E342A20 261C1208 0E043A30 362C2218"},
};

/* rows whose code also loads V register vfield's field into a V pin (V24-V31) at least vpins times */
static const struct {
    Row row;
    unsigned vfield, vpins;
} k_field_rows[] = {
    /* a loop of eight ADD, EOR, EOR into V30 (the native timing bench's): V3 to V10 and V30 each save a load and a
       store a turn, so of the nine that tie V30, read and written eight times a turn, takes a pin (else its chain goes
       through memory at each turn) */
    {{"a loop's accumulator V30 among nine V registers that tie", 2, 0, 16, 8, 0, 0,
      "D2800C90 4EFF8463 6E221C70 6E301FDE 4EFF8484 6E221C91 6E311FDE 4EFF84A5 6E221CB2 6E321FDE 4EFF84C6 6E221CD3 "
      "6E331FDE 4EFF84E7 6E221CF4 6E341FDE 4EFF8508 6E221D15 6E351FDE 4EFF8529 6E221D36 6E361FDE 4EFF854A 6E221D57 "
      "6E371FDE F1000610 54FFFCE1"},
     .vfield = 30,
     .vpins = 1},
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
/* the words a call of the function's start runs, from its first: mark[i] 1 for word i. Every branch is followed both
   ways (a BR's table not at all: the table's jumps are counted alone) but the entry's check's B.NE, right after CMP X1,
   X16: X1 is the start there */
static void call_path(const unsigned char* t, size_t n, unsigned char* mark) {
    size_t words = n / 4, top = 0, *stack = (size_t*)malloc((words + 1) * sizeof *stack);
    if (!stack) return;
    stack[top++] = 0;
    while (top) {
        size_t i = stack[--top];
        while (i < words && !mark[i]) {
            uint32_t w = snx_ld_le32(t + 4 * i);
            long long to = -1;
            int on = 1;
            mark[i] = 1;
            if ((w & 0xFC000000u) == 0x14000000u) /* B */
                to = (long long)i + ((long long)(w & 0x3FFFFFFu) ^ 0x2000000) - 0x2000000, on = 0;
            else if ((w & 0xFF000010u) == 0x54000000u) { /* B.cond */
                if ((w & 15) != 1 || !i || snx_ld_le32(t + 4 * (i - 1)) != 0xEB10003Fu)
                    to = (long long)i + ((long long)(w >> 5 & 0x7FFFF) ^ 0x40000) - 0x40000;
            } else if ((w & 0x7E000000u) == 0x34000000u) /* CBZ, CBNZ */
                to = (long long)i + ((long long)(w >> 5 & 0x7FFFF) ^ 0x40000) - 0x40000;
            else if ((w & 0x7E000000u) == 0x36000000u) /* TBZ, TBNZ */
                to = (long long)i + ((long long)(w >> 5 & 0x3FFF) ^ 0x2000) - 0x2000;
            else if ((w & 0xFFFFFC1Fu) == 0xD65F0000u || (w & 0xFFFFFC1Fu) == 0xD61F0000u) /* RET, BR */
                on = 0;
            if (to >= 0 && (size_t)to < words && !mark[to] && top < words) stack[top++] = (size_t)to;
            if (!on) break;
            ++i;
        }
    }
    free(stack);
}
/* the stores (load 0) or loads of pins in the code's words (those mark has, all where it is NULL), by the state's field each reaches (X19 holds its address
   SNX_ARM64_STATE_BIAS past it): STR or LDR Xt of X20-X28, unsigned offset or unscaled (STUR, LDUR), at [X19] in the
   state's X; STR or LDR Qt (unsigned offset) of V24-V31 at [X19] in the state's V, or at [X16] an ADD X16, X19, #off
   just before makes. *adds: the ADDs of X16 = X19 + an immediate */
/* the code's jumps through a table: LDRSW X16, [X17, X16], then ADD X16, X17, X16 and BR X16 */
static unsigned table_jumps(const unsigned char* t, size_t n) {
    unsigned jumps = 0;
    for (size_t i = 0; i + 12 <= n; i += 4)
        jumps += snx_ld_le32(t + i) == 0xB8B06A30u && snx_ld_le32(t + i + 4) == 0x8B100230u &&
                 snx_ld_le32(t + i + 8) == 0xD61F0200u;
    return jumps;
}
static void pin_moves(const unsigned char* t, size_t n, const unsigned char* mark, int load, unsigned* x, unsigned* v,
                      unsigned* adds) {
    const uint32_t xop = load ? 0xF9400000u : 0xF9000000u, uop = load ? 0xF8400000u : 0xF8000000u,
                   qop = load ? 0x3DC00000u : 0x3D800000u;
    const long long bias = SNX_ARM64_STATE_BIAS, xs = offsetof(CPU, x), vs = offsetof(CPU, v);
    uint32_t prev = 0;
    *x = *v = *adds = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w = snx_ld_le32(t + i), rt = w & 31, rn = w >> 5 & 31;
        long long off = -1;
        if (mark && !mark[i / 4]) {
            prev = w;
            continue;
        }
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

/* the LDR Q (unsigned offset) of a V pin (V24-V31) from V register r's field in the words mark has: at [X19], or at
   [X16] an ADD X16, X19, #off just before makes */
static unsigned field_pins(const unsigned char* t, size_t n, const unsigned char* mark, unsigned r) {
    const long long bias = SNX_ARM64_STATE_BIAS, want = (long long)(offsetof(CPU, v) + 16 * (size_t)r);
    unsigned pins = 0;
    uint32_t prev = 0;
    for (size_t i = 0; i + 4 <= n; i += 4) {
        uint32_t w = snx_ld_le32(t + i), rt = w & 31, rn = w >> 5 & 31;
        if ((w & 0xFFC00000u) == 0x3DC00000u && rt >= 24 && mark[i / 4]) {
            long long off = -1;
            if (rn == 19)
                off = (w >> 10 & 0xFFF) * 16 + bias;
            else if (rn == 16 && !(w >> 10 & 0xFFF) && (prev & 0xFFC003FFu) == (0x91000000u | 19u << 5 | 16u))
                off = (prev >> 10 & 0xFFF) + bias;
            pins += off == want;
        }
        prev = w;
    }
    return pins;
}

static void check(const Row* row, unsigned vfield, unsigned vpins, int print) {
    unsigned char image[(MOST + 1) * 4], *object = NULL;
    const unsigned char* text;
    const SnxU64 hint = BASE;
    SnxCodeImage module = {.image = image, .base = BASE};
    SnxNativeOptions options = {0};
    unsigned char made[4] = {0}; /* the row's function, and a RET past a switch's table */
    uint32_t words[MOST];
    size_t size = 0, length = 0;
    char error[256] = "", *at = (char*)row->words;
    SnxCodegen codegen;
    unsigned count = 0, xs, vs, xl, vl, adds, tables, fpins = 0, all[5];
    unsigned char* mark = NULL;
    g_arm64_pins_code.row = row;
    while (*at && count < MOST) words[count++] = (uint32_t)strtoul(at, &at, 16);
    for (unsigned i = 0; i <= count; ++i)
        for (unsigned b = 0; b < 4; ++b)
            image[4 * i + b] = (unsigned char)((i < count ? words[i] : 0xD65F03C0u) >> (8 * b));
    module.image_size = module.text_size = (count + 1) * 4;
    if (snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, SNX_A64_TARGET_SWITCH) || !codegen.function_count ||
        codegen.function_count > 4) {
        fail("functions the analysis found in its words", (unsigned)codegen.function_count, 4);
        return;
    }
    options.stale = 1;
    if (snx_arm64_object(&codegen, 0, codegen.function_count, &options, 0, made, &object, &size, error, sizeof error) <
            0 ||
        !made[0]) {
        fprintf(stderr, "FAIL: %s: not made: %s\n", row->what, error);
        ++g_arm64_pins_code.failures;
    } else if (!(text = text_of(object, size, &length))) {
        fprintf(stderr, "FAIL: %s: no code in its object of %zu bytes\n", row->what, size);
        ++g_arm64_pins_code.failures;
    } else if (!(mark = (unsigned char*)calloc(length / 4 + 1, 1))) {
        fprintf(stderr, "FAIL: %s: no memory for its %zu words' marks\n", row->what, length / 4);
        ++g_arm64_pins_code.failures;
    } else {
        call_path(text, length, mark);
        pin_moves(text, length, NULL, 0, &all[0], &all[2], &all[4]);
        pin_moves(text, length, NULL, 1, &all[1], &all[3], &all[4]);
        pin_moves(text, length, mark, 0, &xs, &vs, &adds);
        pin_moves(text, length, mark, 1, &xl, &vl, &adds);
        tables = table_jumps(text, length);
        if (vfield) fpins = field_pins(text, length, mark, vfield);
        if (print && vfield) printf("%s: V%u's field into a pin %u times\n", row->what, vfield, fpins);
        if (print)
            printf("%s: %zu bytes; a call's path: X pins %u stored, %u loaded; V pins %u stored, %u loaded; %u ADDs of "
                   "X19; the whole code: X %u, %u; V %u, %u; %u table jumps\n",
                   row->what, length, xs, xl, vs, vl, adds, all[0], all[1], all[2], all[3], tables);
        else {
            if (tables < row->tables) {
                fprintf(stderr, "FAIL: %s: %u jumps through a table, at least %u\n", row->what, tables, row->tables);
                ++g_arm64_pins_code.failures;
            }
            if (xs > row->xstores) fail("stores of X pins", xs, row->xstores);
            if (xl > row->xloads) fail("loads of X pins", xl, row->xloads);
            if (vs > row->vstores) fail("stores of V pins", vs, row->vstores);
            if (vl > row->vloads) fail("loads of V pins", vl, row->vloads);
            if (adds > row->adds) fail("ADDs of X19 before a field's load or store", adds, row->adds);
            if (vfield && fpins < vpins) {
                fprintf(stderr, "FAIL: %s: V%u's field into a pin %u times, at least %u\n", row->what, vfield, fpins,
                        vpins);
                ++g_arm64_pins_code.failures;
            }
        }
    }
    free(mark);
    free(object);
    snx_codegen_free(&codegen);
}

int main(int argc, char** argv) {
    int print = argc > 1 && !strcmp(argv[1], "--print");
    if (argc > 1 && !print) {
        fprintf(stderr, "%s: no such option\n", argv[1]);
        return 2;
    }
    for (size_t i = 0; i < sizeof k_rows / sizeof *k_rows; ++i) check(&k_rows[i], 0, 0, print);
    for (size_t i = 0; i < sizeof k_field_rows / sizeof *k_field_rows; ++i)
        check(&k_field_rows[i].row, k_field_rows[i].vfield, k_field_rows[i].vpins, print);
    printf("arm64 pins code: %zu rows, %d failures\n",
           sizeof k_rows / sizeof *k_rows + sizeof k_field_rows / sizeof *k_field_rows, g_arm64_pins_code.failures);
    return g_arm64_pins_code.failures != 0;
}
