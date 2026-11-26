/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How the arm64 back end's functions are entered in its ELF objects: each compares its target (X1) with its start
   before the frame (MOVZ/MOVK X16; CMP X1, X16; B.NE to an entry of its own frame), and a call of a function of the
   unit at its start comes in past that check, with nothing in X1 where the callee was made before the call
   (mach_codegen.inc's past_check). The program: F1 a leaf; F2 calls F1, made before it, and F3, made after it; F3
   calls itself. The stale check past the frame stores the start as the pc, X1 holding none there. In an AOT unit and
   a JIT one. recompiler_arm64_calls and _calls_back hold what such calls compute to LLVM's; on Windows the check stays
   past the prologue, which recompiler_arm64win's unwinder reads at the function's first word.
     snx-test-arm64-entry-code [--print]
   --print: each function's entry and the calls, and nothing held */
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

#define BASE  0x10000u
#define FUNCS 3u

static const uint32_t k_words[] = {
    0x91000400u, 0xD65F03C0u,                           /* F1: ADD X0, X0, #1; RET */
    0x97FFFFFEu, 0x91000800u, 0x94000002u, 0xD65F03C0u, /* F2: BL F1; ADD X0, X0, #2; BL F3; RET */
    0xB4000060u, 0xD1000400u, 0x97FFFFFEu, 0xD65F03C0u, /* F3: CBZ X0, the RET; SUB X0, X0, #1; BL F3; RET */
};
static const uint64_t k_start[FUNCS] = {BASE, BASE + 8, BASE + 24};
/* the MOVZs of X1 each function has: F2's of F3, made after it */
static const unsigned k_x1[FUNCS] = {0, 1, 0};

static struct {
    const char* unit; /* the one being checked, for the messages */
    int failures;
} g_arm64_entry_code;
#define FAIL(...)                                                                                                      \
    do {                                                                                                               \
        fprintf(stderr, "FAIL: %s: ", g_arm64_entry_code.unit);                                                        \
        fprintf(stderr, __VA_ARGS__);                                                                                  \
        fputc('\n', stderr);                                                                                           \
        ++g_arm64_entry_code.failures;                                                                                 \
    } while (0)

typedef struct {
    uint64_t at, size; /* in the code section */
    size_t entry;      /* the frame's first word past the check, from at; 0 where there is no check */
} Fn;

/* the code section of an ELF64 object and each F_<start>'s place in it: 0, -1 where there is no such object */
static int read_object(const unsigned char* o, size_t size, const unsigned char** text, size_t* length, Fn* fn) {
    uint64_t shoff;
    unsigned n, i, code = 0;
    const unsigned char* symtab = NULL;
    size_t nsyms = 0;
    const char* names = NULL;
    if (size < 64 || memcmp(o, "\177ELF", 4)) return -1;
    shoff = snx_ld_le64(o + 40), n = snx_ld_le16(o + 60);
    for (i = 0; i < n; ++i) {
        const unsigned char* sh = o + shoff + (size_t)i * 64;
        if (snx_ld_le32(sh + 4) == 1 && snx_ld_le64(sh + 8) & 4) { /* PROGBITS, executable */
            code = i;
            *length = (size_t)snx_ld_le64(sh + 32);
            *text = o + snx_ld_le64(sh + 24);
        } else if (snx_ld_le32(sh + 4) == 2) { /* SYMTAB, its names in the section sh_link names */
            const unsigned char* str = o + shoff + (size_t)snx_ld_le32(sh + 40) * 64;
            symtab = o + snx_ld_le64(sh + 24);
            nsyms = (size_t)(snx_ld_le64(sh + 32) / 24);
            names = (const char*)o + snx_ld_le64(str + 24);
        }
    }
    if (!code || !symtab) return -1;
    for (size_t k = 0; k < nsyms; ++k) {
        const unsigned char* s = symtab + 24 * k;
        const char* name = names + snx_ld_le32(s);
        if (snx_ld_le16(s + 6) != code || strncmp(name, "F_", 2)) continue;
        uint64_t start = strtoull(name + 2, NULL, 16);
        for (unsigned f = 0; f < FUNCS; ++f)
            if (k_start[f] == start) fn[f].at = snx_ld_le64(s + 8), fn[f].size = snx_ld_le64(s + 16);
    }
    return 0;
}

static int movz_x(uint32_t w, unsigned rd) { return (w & 0xFF80001Fu) == (0xD2800000u | rd); }
/* MOVZ of Wrd or Xrd */
static int movz(uint32_t w, unsigned rd) { return (w & 0x7F80001Fu) == (0x52800000u | rd); }
static int movk_x(uint32_t w, unsigned rd) { return (w & 0xFF80001Fu) == (0xF2800000u | rd); }
/* the value a MOVZ and its MOVKs of X16 make from word k on, k past them */
static uint64_t x16_value(const unsigned char* t, size_t n, size_t* k) {
    uint64_t v = 0;
    for (; *k + 4 <= n; *k += 4) {
        uint32_t w = snx_ld_le32(t + *k);
        if (!movz_x(w, 16) && !movk_x(w, 16)) break;
        if (movz_x(w, 16)) v = 0;
        v |= (uint64_t)(w >> 5 & 0xFFFF) << (16 * (w >> 21 & 3));
    }
    return v;
}
#define STP_FRAME 0xA9BA7BFDu /* STP X29, X30, [SP, #-96]! */
#define CMP_X1    0xEB10003Fu /* CMP X1, X16 */
#define CMP_W16   0x7100021Fu /* CMP W16, #0, the stale byte's */

/* function f's entry: MOVZ/MOVK X16 = start, CMP X1, X16, B.NE to a frame of its own past the one that follows */
static void check_entry(const unsigned char* t, Fn* fn, unsigned f) {
    size_t k = (size_t)fn[f].at, end = (size_t)(fn[f].at + fn[f].size);
    uint64_t v = x16_value(t, end, &k);
    uint32_t bne;
    int64_t d;
    if (k == fn[f].at || v != k_start[f] || snx_ld_le32(t + k) != CMP_X1) {
        FAIL("F_%llX: its first words are no check of X1 with %llx before the frame (%08x)",
             (unsigned long long)k_start[f], (unsigned long long)k_start[f], (unsigned)snx_ld_le32(t + fn[f].at));
        return;
    }
    bne = snx_ld_le32(t + k + 4);
    if ((bne & 0xFF00001Fu) != 0x54000001u || snx_ld_le32(t + k + 8) != STP_FRAME) {
        FAIL("F_%llX: its check is not a B.NE then the frame (%08x %08x)", (unsigned long long)k_start[f],
             (unsigned)bne, (unsigned)snx_ld_le32(t + k + 8));
        return;
    }
    fn[f].entry = k + 8 - (size_t)fn[f].at;
    d = (int64_t)(bne >> 5 & 0x7FFFF);
    d = (d & 0x40000 ? d - 0x80000 : d) * 4;
    d += (int64_t)(k + 4);
    if (d <= (int64_t)(k + 8) || d + 4 > (int64_t)end || snx_ld_le32(t + d) != STP_FRAME)
        FAIL("F_%llX: another target's way is no frame of its own past the start's", (unsigned long long)k_start[f]);
}
/* past the frame, the stale check's exit stores the start as the pc: MOVZ/MOVK X16 = start, STR X16 there */
static void check_stale(const unsigned char* t, const Fn* fn, unsigned f) {
    size_t k = (size_t)(fn[f].at + fn[f].entry), end = (size_t)(fn[f].at + fn[f].size);
    const long long bias = SNX_ARM64_STATE_BIAS, pc = offsetof(CPU, pc);
    for (; k + 12 <= end && snx_ld_le32(t + k) != CMP_W16; k += 4) {}
    if (k + 12 > end) {
        FAIL("F_%llX: no stale check past the frame", (unsigned long long)k_start[f]);
        return;
    }
    k += 8; /* the CMP, the B.EQ */
    uint64_t v = x16_value(t, end, &k);
    uint32_t w = k + 4 <= end ? snx_ld_le32(t + k) : 0;
    long long off = -1;
    if ((w & 0xFFC003FFu) == (0xF9000000u | 19u << 5 | 16u)) off = (w >> 10 & 0xFFF) * 8 + bias; /* STR */
    if ((w & 0xFFE00C1Fu) == (0xF8000000u | 16u) && (w >> 5 & 31) == 19) {                       /* STUR */
        long long i9 = w >> 12 & 0x1FF;
        off = (i9 & 0x100 ? i9 - 0x200 : i9) + bias;
    }
    if (v != k_start[f] || off != pc)
        FAIL("F_%llX: the stale check's exit stores %llx at %lld, not the start at the pc's %lld",
             (unsigned long long)k_start[f], (unsigned long long)v, off, pc);
}

static void check(int jit, int print) {
    unsigned char image[sizeof k_words], made[FUNCS + 1] = {0}, *object = NULL;
    const unsigned char* text = NULL;
    SnxCodeImage module = {.image = image, .base = BASE};
    SnxNativeOptions options = {.stale = 1, .jit = jit, .table_name = "UNIT"};
    size_t size = 0, length = 0;
    char error[256] = "";
    SnxCodegen codegen;
    Fn fn[FUNCS] = {{0}};
    unsigned past = 0, at_check = 0;
    g_arm64_entry_code.unit = jit ? "a JIT unit" : "an AOT unit";
    for (size_t i = 0; i < sizeof k_words / 4; ++i) snx_st_le32(image + 4 * i, k_words[i]);
    module.image_size = module.text_size = sizeof image;
    if (snx_codegen_analyze(&codegen, &module, 1, k_start, FUNCS, 1, SNX_A64_TARGET_SWITCH) ||
        codegen.function_count != FUNCS) {
        FAIL("the analysis found %zu functions, not %u", codegen.function_count, FUNCS);
        return;
    }
    if (snx_arm64_object(&codegen, 0, FUNCS, &options, 0, made, &object, &size, error, sizeof error) < 0 || !made[0] ||
        !made[1] || !made[2])
        FAIL("not made: %s", error);
    else if (read_object(object, size, &text, &length, fn) || !fn[0].size || !fn[1].size || !fn[2].size)
        FAIL("no code or no F_ symbols in its object of %zu bytes", size);
    else {
        for (unsigned f = 0; f < FUNCS; ++f) {
            check_entry(text, fn, f);
            if (fn[f].entry) check_stale(text, fn, f);
        }
        for (unsigned f = 0; f < FUNCS; ++f) {
            unsigned x1 = 0;
            for (size_t k = (size_t)fn[f].at; k < fn[f].at + fn[f].size; k += 4) {
                uint32_t w = snx_ld_le32(text + k);
                x1 += movz(w, 1);
                if ((w & 0xFC000000u) != 0x94000000u) continue; /* BL */
                int64_t d = (int64_t)(w & 0x3FFFFFFu);
                uint64_t to = (uint64_t)((int64_t)k + (d & 0x2000000 ? d - 0x4000000 : d) * 4);
                for (unsigned g = 0; g < FUNCS; ++g) {
                    if (fn[g].entry && to == fn[g].at + fn[g].entry) ++past;
                    if (to == fn[g].at) ++at_check;
                    if (print && (to == fn[g].at || to == fn[g].at + fn[g].entry))
                        printf("%s: F_%llX calls F_%llX at +%llu\n", g_arm64_entry_code.unit,
                               (unsigned long long)k_start[f], (unsigned long long)k_start[g],
                               (unsigned long long)(to - fn[g].at));
                }
            }
            if (print)
                printf("%s: F_%llX: %llu bytes, entered past its check at +%zu, %u MOVZ of X1\n",
                       g_arm64_entry_code.unit, (unsigned long long)k_start[f], (unsigned long long)fn[f].size,
                       fn[f].entry, x1);
            else if (x1 != k_x1[f])
                FAIL("F_%llX sets X1 %u times, not %u (a call of a function made before it gives none)",
                     (unsigned long long)k_start[f], x1, k_x1[f]);
        }
        if (!print && (past != 3 || at_check))
            FAIL("%u calls past the callee's check and %u at its start, not 3 and 0", past, at_check);
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
    check(0, print);
    check(1, print);
    printf("arm64 entry code: 2 units, %d failures\n", g_arm64_entry_code.failures);
    return g_arm64_entry_code.failures != 0;
}
