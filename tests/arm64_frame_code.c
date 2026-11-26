/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The arm64 back end's frame in its ELF objects: the ten registers the prologue saves, then the spill slots above
   them. Slots of up to 400 bytes take no instruction of their own: STP X29, X30, [SP, #-(96 + slots)]! makes the
   whole frame and each LDP X29, X30, [SP], #(96 + slots) undoes it. Larger ones (past the pre-index's reach) are a
   SUB SP, SP, #slots before that STP and an ADD SP after each LDP, the function made again so. Each row is a function
   of its words and a RET, made for a host of no optional feature (its AES and CRC32 helpers' calls keep values in
   slots): its first word, and whether a SUB or ADD of SP is in it. recompiler_arm64_frame and _plain_crc32 hold such
   code to LLVM's; Windows' frame stays as x64_elf.c's unwinder reads it (recompiler_arm64win).
     snx-test-arm64-frame-code [--print]
   --print: each row's frame, and nothing held */
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BASE 0x10000u
#define MOST 96u

typedef struct {
    const char* what;
    int frame;         /* 0 no slot, 1 slots in the pre-index, 2 past its reach: a SUB and an ADD of SP */
    const char* words; /* the function's words before its RET, in hex */
} Row;

static const Row k_rows[] = {
    /* ADD X0, X0, #1 */
    {"a leaf of no slot", 0, "91000400"},
    /* the X pins row of arm64_pins_code.c: a loop and a call */
    {"a loop, then a call", 0, "F94000C1 8B070021 8B080129 F100054A 54FFFF81 97FFFFFB 8B010120"},
    /* AESE V0, V1: on a host of no AES (host 0) a helper's call, the values made before it kept in slots across it */
    {"an AES round", 1, "4E284820"},
    /* CRC32B, H, W and X of eight registers, each a helper's call on a host of no CRC32: some 300 bytes of slots, still
       in the pre-index */
    {"eight CRC32s", 1, "1AC14000 1AC24421 1AC34842 9AC44C63 1AC55084 1AC654A5 1AC758C6 9AC85CE7"},
    /* AESE of V0-V7, each from the next one: past the pre-index's reach (recompiler_arm64_frame runs these words) */
    {"eight AES rounds", 2, "4E284820 4E284841 4E284862 4E284883 4E2848A4 4E2848C5 4E2848E6 4E284907"},
};

static struct {
    int failures;
} g_arm64_frame_code;

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

static int stp_frame(uint32_t w) { return (w & 0xFFC07FFFu) == 0xA9807BFDu; } /* STP X29, X30, [SP, #-n]! */
static int ldp_frame(uint32_t w) { return (w & 0xFFC07FFFu) == 0xA8C07BFDu; } /* LDP X29, X30, [SP], #n */
static int sp_imm(uint32_t w) { return (w & 0xBF8003FFu) == 0x910003FFu; }    /* ADD or SUB SP, SP, #n */
/* the bytes an STP's pre-index or an LDP's post-index moves SP by */
static int pair_bytes(uint32_t w) {
    int imm7 = (int)(w >> 15 & 0x7F);
    return 8 * (imm7 & 0x40 ? imm7 - 0x80 : imm7);
}

static void check(const Row* row, int print) {
    unsigned char image[(MOST + 1) * 4], *object = NULL;
    const unsigned char* t;
    const SnxU64 hint = BASE;
    SnxCodeImage module = {.image = image, .base = BASE};
    SnxNativeOptions options = {.stale = 1};
    unsigned char made[4] = {0};
    uint32_t words[MOST];
    size_t size = 0, n = 0;
    char error[256] = "", *at = (char*)row->words;
    SnxCodegen codegen;
    unsigned count = 0, sps = 0, stps = 0, ldps = 0, bad = 0;
    int first = 0, sub = 0;
    while (*at && count < MOST) words[count++] = (uint32_t)strtoul(at, &at, 16);
    for (unsigned i = 0; i <= count; ++i) snx_st_le32(image + 4 * i, i < count ? words[i] : 0xD65F03C0u);
    module.image_size = module.text_size = (count + 1) * 4;
    if (snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, SNX_A64_TARGET_SWITCH) || !codegen.function_count) {
        fprintf(stderr, "FAIL: %s: the analysis found no function\n", row->what);
        ++g_arm64_frame_code.failures;
        return;
    }
    if (snx_arm64_object(&codegen, 0, 1, &options, 0, made, &object, &size, error, sizeof error) < 0 || !made[0] ||
        !(t = text_of(object, size, &n))) {
        fprintf(stderr, "FAIL: %s: not made: %s\n", row->what, error);
        ++g_arm64_frame_code.failures;
        free(object);
        snx_codegen_free(&codegen);
        return;
    }
    if (n >= 8) {
        uint32_t w0 = snx_ld_le32(t), w1 = snx_ld_le32(t + 4);
        if (sp_imm(w0) && w0 >> 30 & 1 && stp_frame(w1)) /* SUB SP, SP, #slots; STP X29, X30, [SP, #-96]! */
            sub = (int)(w0 >> 10 & 0xFFF) << (w0 >> 22 & 1 ? 12 : 0), first = pair_bytes(w1);
        else if (stp_frame(w0))
            first = pair_bytes(w0);
    }
    for (size_t k = 0; k + 4 <= n; k += 4) {
        uint32_t w = snx_ld_le32(t + k);
        sps += sp_imm(w);
        stps += stp_frame(w);
        if (ldp_frame(w)) {
            uint32_t next = k + 8 <= n ? snx_ld_le32(t + k + 4) : 0;
            ++ldps;
            /* each LDP undoes the STP: with the slots folded in, its post-index is the STP's; else the ADD follows */
            if (sub ? pair_bytes(w) != 96 || !sp_imm(next) || next >> 30 & 1 : pair_bytes(w) != -first) ++bad;
        }
    }
    if (print)
        printf("%s: %zu bytes; STP X29, X30 by %d, SUB SP by %d; %u ADD or SUB of SP, %u LDPs of X29, X30 (%u not "
               "undoing it)\n",
               row->what, n, first, sub, sps, ldps, bad);
    else if (!first || stps != 1 || bad)
        fprintf(stderr,
                "FAIL: %s: its frame is no STP X29, X30 at the start undone by each LDP (%u STPs, %u LDPs, %u "
                "not undoing it)\n",
                row->what, stps, ldps, bad),
            ++g_arm64_frame_code.failures;
    else if (row->frame < 2 && (sub || sps || (row->frame ? first >= -96 : first != -96) || (-first - 96) % 16))
        fprintf(stderr,
                "FAIL: %s: its slots are not in the STP's pre-index: STP by %d, SUB of SP by %d, %u ADD or SUB "
                "of SP\n",
                row->what, first, sub, sps),
            ++g_arm64_frame_code.failures;
    else if (row->frame == 2 && (first != -96 || sub <= 504 - 96 || sps != 1 + ldps))
        fprintf(stderr,
                "FAIL: %s: %d bytes of slots, not past the pre-index's reach in a SUB of SP and an ADD after each "
                "LDP (STP by %d, %u ADD or SUB of SP, %u LDPs)\n",
                row->what, sub, first, sps, ldps),
            ++g_arm64_frame_code.failures;
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
    printf("arm64 frame code: %zu rows, %d failures\n", sizeof k_rows / sizeof *k_rows, g_arm64_frame_code.failures);
    return g_arm64_frame_code.failures != 0;
}
