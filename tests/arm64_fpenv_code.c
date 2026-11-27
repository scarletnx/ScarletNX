/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Which SIMD&FP words the arm64 back end runs under the guest's FPCR (arm64_vec.c's SGX_M_FPENTER and SGX_M_FPLEAVE:
   MSR FPCR and FPSR, MRS FPSR), held to the A64 Sail: a word the host runs as its own instruction switches to the
   guest's FPCR where the Sail's own code of it (SnxNativeOptions.sail_alone) reads FPCR or writes FPSR, and only there.
   Each data-processing clause of the SIMD&FP family is tried with words of fixed seeds (its fixed bits, the rest
   drawn), each a function of that word and a RET, made both ways for a host of every optional feature, the guest's the
   decoder's every one: the Sail's code reaches FPCR or FPSR where a load or a store of X19's names them. A word with no
   switch where the Sail's code reaches them is a wrong result, and fails. One with a switch where it doesn't is a cost,
   and fails too, but where the table can't tell: a word of more than one clause's fixed bits (the first's decode may
   send it to a later one: SEE), and a clause whose fp is 2, FPCR given to a function the back end lowers, which may not
   read it (BFDOT's step). A clause the arm64 Sail makes no code for (a64_x64.roots leaves it to the host's own
   instruction) has nothing to be held to here: sailgen_encoders holds sailgen's fp of some of those.
     snx-test-arm64-fpenv-code [--print]
   --print: each clause's words, those the host runs as its own, and how many of them the Sail reaches FPCR or FPSR in
   and the back end switches for, and nothing held */
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

#include "a64_enc.inc"

#define BASE    0x10000u
#define SAMPLES 24u

static struct {
    int failures;
    SnxU64 seed;
} g_arm64_fpenv_code;

static SnxU32 draw(void) {
    g_arm64_fpenv_code.seed = g_arm64_fpenv_code.seed * 6364136223846793005ull + 1442695040888963407ull;
    return (SnxU32)(g_arm64_fpenv_code.seed >> 32);
}

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

/* the function of word and a RET, made whole for a host of every optional feature: malloc'd code, NULL where not made
   or where word has no code there (the back end makes it raising an undefined instruction, which is none of the Sail's
   code to hold the switch to) */
static unsigned char* make(SnxU32 word, int sail_alone, size_t* length) {
    unsigned char image[8], *object = NULL, *code = NULL, made[4] = {0};
    const unsigned char* text;
    const SnxU64 hint = BASE;
    SnxCodeImage module = {.image = image, .base = BASE, .image_size = 8, .text_size = 8};
    size_t refused = 0;
    SnxNativeOptions options = {.stale = 1, .sail_alone = sail_alone, .refused = &refused};
    SnxCodegen codegen;
    size_t size = 0;
    char error[256] = "";
    snx_st_le32(image, word);
    snx_st_le32(image + 4, 0xD65F03C0u);
    if (snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, SNX_A64_TARGET_MAX) || codegen.function_count != 1)
        return NULL;
    if (snx_arm64_object(&codegen, 0, 1, &options, SNX_ARM64_F_ALL, made, &object, &size, error, sizeof error) >= 0 &&
        made[0] && !refused && (text = text_of(object, size, length)) && (code = (unsigned char*)malloc(*length)))
        memcpy(code, text, *length);
    free(object);
    snx_codegen_free(&codegen);
    return code;
}

/* a load or a store of X19's at FPCR's or FPSR's place in the state (X19 its address SNX_ARM64_STATE_BIAS past it):
   W or X, an unsigned offset or an unscaled one */
static int reaches_fp_state(const unsigned char* t, size_t n) {
    const long long bias = SNX_ARM64_STATE_BIAS, fpcr = offsetof(CPU, fpcr), fpsr = offsetof(CPU, fpsr);
    for (size_t i = 0; i + 4 <= n; i += 4) {
        SnxU32 w = snx_ld_le32(t + i);
        long long off = -1;
        if ((w >> 5 & 31) != 19) continue;
        if ((w & 0xBF800000u) == 0xB9000000u) /* STR or LDR W or X, unsigned offset */
            off = (long long)(w >> 10 & 0xFFF) * (w >> 30 & 1 ? 8 : 4) + bias;
        else if ((w & 0xBFA00C00u) == 0xB8000000u) { /* STUR or LDUR W or X */
            long long i9 = w >> 12 & 0x1FF;
            off = (i9 & 0x100 ? i9 - 0x200 : i9) + bias;
        }
        if (off == fpcr || off == fpsr) return 1;
    }
    return 0;
}
/* MSR FPCR or FPSR, or MRS FPSR: the switch to the guest's FPCR and back */
static int switches(const unsigned char* t, size_t n) {
    for (size_t i = 0; i + 4 <= n; i += 4) {
        SnxU32 w = snx_ld_le32(t + i) & ~31u;
        if (w == 0xD51B4400u || w == 0xD51B4420u || w == 0xD53B4420u) return 1;
    }
    return 0;
}

static int clause_of(SnxU32 word) {
    for (unsigned k = 0; k < sge_a64_count; ++k)
        if ((word & sge_a64_table[k].mask) == sge_a64_table[k].value) return (int)k;
    return -1;
}
/* how many clauses have word's fixed bits */
static unsigned clauses_of(SnxU32 word) {
    unsigned n = 0;
    for (unsigned k = 0; k < sge_a64_count; ++k) n += (word & sge_a64_table[k].mask) == sge_a64_table[k].value;
    return n;
}

int main(int argc, char** argv) {
    int print = argc > 1 && !strcmp(argv[1], "--print");
    unsigned clauses = 0, words = 0, through = 0, wrong = 0, costly = 0, see = 0, lowered = 0, no_sail = 0;
    if (argc > 1 && !print) {
        fprintf(stderr, "%s: no such option\n", argv[1]);
        return 2;
    }
    for (unsigned k = 0; k < sge_a64_count; ++k) {
        unsigned tried = 0, own = 0, reach = 0, sw = 0, host_only = 0;
        if (strcmp(sge_a64_table[k].family, "DataProcFPSIMD")) continue;
        g_arm64_fpenv_code.seed = 0x5EED0000u + k;
        for (unsigned s = 0; s < SAMPLES * 8 && tried < SAMPLES; ++s) {
            SnxU32 word = sge_a64_table[k].value | (draw() & ~sge_a64_table[k].mask);
            SnxA64Insn insn;
            size_t na = 0, nb = 0;
            if (clause_of(word) != (int)k || snx_a64_decode_word(word, SNX_A64_TARGET_MAX, &insn) == SNX_A64_OP_UNDEF)
                continue;
            unsigned char *a = make(word, 0, &na), *b = make(word, 1, &nb);
            if (a && b) {
                int is_own = na != nb || memcmp(a, b, na), r = reaches_fp_state(b, nb), w = switches(a, na);
                ++tried;
                if (is_own) {
                    ++own, reach += r, sw += w;
                    if (r && !w) {
                        if (!print)
                            fprintf(stderr,
                                    "FAIL: %s %08X: the host runs it without the guest's FPCR, which the Sail's "
                                    "code of it reads or whose FPSR it writes\n",
                                    sge_a64_table[k].name, (unsigned)word);
                        ++wrong;
                    } else if (!r && w && clauses_of(word) > 1)
                        ++see;
                    else if (!r && w && sge_a64_table[k].fp == 2)
                        ++lowered;
                    else if (!r && w) {
                        if (!print)
                            fprintf(stderr,
                                    "FAIL: %s %08X: the guest's FPCR switched to for a word whose Sail code "
                                    "reads no FPCR and writes no FPSR\n",
                                    sge_a64_table[k].name, (unsigned)word);
                        ++costly;
                    }
                }
            } else if (a)
                ++host_only;
            free(a), free(b);
        }
        if (!tried) {
            no_sail += host_only > 0;
            continue;
        }
        ++clauses, words += tried, through += own;
        if (print)
            printf("%-28s %2u words, %2u the host's own: the Sail reaches FPCR/FPSR in %2u, switched %2u\n",
                   sge_a64_table[k].name, tried, own, reach, sw);
    }
    g_arm64_fpenv_code.failures = (int)(wrong + costly);
    printf("arm64 fpenv code: %u clauses, %u words, %u the host's own; %u without the switch they need, %u with one "
           "they don't, and %u with one the table can't tell from the clause (%u of words of more than one clause, %u "
           "given to a lowered function); %u clauses the Sail makes no code for\n",
           clauses, words, through, wrong, costly, see + lowered, see, lowered, no_sail);
    return print ? 0 : g_arm64_fpenv_code.failures != 0;
}
