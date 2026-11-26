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
#define MOST          32u
#define NEEDS_AVX2    32768u
#define NEEDS_GFNI    65536u
#define NEEDS_SHA     8192u
#define NEEDS_AES     4096u
#define NEEDS_AVX     1024u
#define NEEDS_FMA     2048u
/* no feature: the function is made with the stale check a game's have (SnxNativeOptions.stale) */
#define WITH_STALE    0x80000000u
/* ROUNDPS and ROUNDPD (66 0F 3A 08 and 09) and their VEX.128 forms (C4, two bytes, 08 or 09) */
#define ROUNDPS       "66 0F 3A 08|66 0F 3A 09|C4 ?? ?? 08|C4 ?? ?? 09"
/* MOV R9D, the descriptor of FMULX of four singles (x64_vec.c's FDESC): the vector helper's, on the cold path */
#define FMULX_4S      "41 B9 40 44 00 00"
/* MOVMSKPS and MOVMSKPD r32, xmm (0F 50, 66 0F 50): a lane mask the hot code tests */
#define MOVMSKPS      "0F 50"
#define MOVMSKPD      "66 0F 50"
/* SETNE r/m8 (0F 95): a condition made a value */
#define SETNE         "0F 95"
/* JP rel32 (0F 8A); PUSH qword [rsp + disp32] (FF B4 24) and its POPFQ (9D) */
#define JP_REL32       "0F 8A"
#define PUSH_RSP_POPFQ "FF B4 24 ?? ?? ?? ?? 9D"
/* PADDD (66 0F FE) and PADDQ (66 0F D4), or their VEX forms */
#define PADDD         "66 0F FE|C5 ?? FE|C4 ?? ?? FE"
#define PADDQ         "66 0F D4|C5 ?? D4|C4 ?? ?? D4"
/* INSERTPS (66 0F 3A 21) and MOVQ xmm, xmm/m64 (F3 0F 7E), or their VEX forms: a scalar's upper bits made 0 in XMM */
#define INSERTPS      "66 0F 3A 21|C4 ?? ?? 21"
#define MOVQ_XMM      "F3 0F 7E|C5 FA 7E|C5 7A 7E"
/* MOVDQU of V30's field (disp32 0x2F8 off RBX) to XMM8-XMM15 (REX.R, or VEX's inverted R): a pin's load */
#define V30_PIN_LOAD  "F3 44 0F 6F ?? F8 02 00 00|C5 7A 6F ?? F8 02 00 00"
/* MOVDQU xmm, m128: a V register's load from the state, or a constant's from the pool */
#define MOVDQU_LOAD   "F3 0F 6F"
/* GF2P8AFFINEQB xmm, xmm/m128, imm8 (66 0F 3A CE) and its VEX.128 form (C4, two bytes, CE); SHA1RNDS4 (0F 3A CC),
   AESDECLAST (66 0F 38 DF) and AESDEC (66 0F 38 DE) below */
#define GF2P8AFFINEQB "66 0F 3A CE|C4 ?? ?? CE"

/* PEXTRB, PEXTRW, PEXTRD/Q, PINSRB, PINSRD/Q (66 [REX] 0F 3A 14, 15, 16, 20, 22) and PINSRW, PEXTRW (66 [REX] 0F C4, C5) */
#define ELEMENT       "0F 3A 14|0F 3A 15|0F 3A 16|0F 3A 20|0F 3A 22|66 0F C4|66 0F C5"
/* CMP byte [rip + disp32], 0, the stale check; CMP RSI, imm32, the entry against the function's start */
#define STALE_TEST    "80 3D ?? ?? ?? ?? 00"
#define ENTRY_COMPARE "48 81 FE 00 00 01 00"

typedef struct {
    const char* what;
    unsigned needs, without; /* the host's features (and WITH_STALE); those it goes without */
    unsigned bytes;          /* the most the function takes */
    int fpsr;                /* the writes of FPSR, -1 not counted */
    const char* has;         /* bytes the code has (hex, ?? any), one form's encodings or another's (|); after a !
                                bytes it has not, after =N bytes it has N times; NULL for none */
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
    {"crc32w w0, w0, w2", 0, 0, 226, -1, NULL, "1AC24800"},
    {"crc32x w0, w0, x2", 0, 0, 282, -1, NULL, "9AC24C00"},
    {"sha1c q0, s1, v2.4s", NEEDS_SHA, 0, 256, -1, "0F 3A CC", "5E020020"},
    {"sha1h s0, s1", 0, 0, 224, -1, NULL, "5E280820"},
    {"sha1su0 v0.4s, v1.4s, v2.4s", 0, 0, 240, -1, NULL, "5E023020"},
    {"sha1su1 v0.4s, v1.4s", 0, 0, 234, -1, NULL, "5E281820"},
    {"aese v0.16b, v1.16b; aesmc v0.16b, v0.16b", NEEDS_AES, 0, 224, -1, "!66 0F 38 DF", "4E284820 4E286800"},
    {"aesd v0.16b, v1.16b; aesimc v0.16b, v0.16b", NEEDS_AES, 0, 224, -1, "66 0F 38 DE", "4E285820 4E287800"},
    {"zip1 v0.4s, v1.4s, v2.4s", 0, 0, 208, -1, NULL, "4E823820"},
    {"zip2 v0.16b, v1.16b, v2.16b", 0, 0, 208, -1, NULL, "4E027820"},
    {"uzp1 v0.4s, v1.4s, v2.4s", 0, 0, 208, -1, NULL, "4E821820"},
    {"trn1 v0.8h, v1.8h, v2.8h", 0, 0, 208, -1, NULL, "4E422820"},
    {"ext v0.16b, v1.16b, v2.16b, #5", 0, 0, 208, -1, NULL, "6E022820"},
    {"ext v0.8b, v1.8b, v2.8b, #5", 0, 0, 208, -1, NULL, "2E022820"},
    {"rev64 v0.4s, v1.4s", 0, 0, 192, -1, NULL, "4EA00820"},
    {"addv s0, v1.4s", 0, 0, 224, -1, NULL, "4EB1B820"},
    {"mul v0.4s, v1.4s, v2.s[1]", 0, 0, 208, -1, NULL, "4FA28020"},
    {"smaxp v0.4s, v1.4s, v2.4s", 0, 0, 224, -1, NULL, "4EA2A420"},
    {"add v0.16b, v1.16b, v2.16b (one operand from the state)", NEEDS_AVX, 0, 208, -1, "=1 " MOVDQU_LOAD, "4E228420"},
    {"uzp2 v0.16b, v1.16b, v2.16b (the masks from the pool)", NEEDS_AVX, 0, 272, -1, "=2 " MOVDQU_LOAD, "4E025820"},
    {"frinta v0.4s, v1.4s", 0, 0, 672, -1, ROUNDPS, "6E218820"},
    {"frinta s0, s1", 0, 0, 448, -1, ROUNDPS, "1E264020"},
    {"fcvtas v0.4s, v1.4s", 0, 0, 704, -1, ROUNDPS, "4E21C820"},
    {"fcvtas s0, s1", 0, 0, 512, -1, ROUNDPS, "5E21C820"},
    {"fmulx v0.4s, v1.4s, v2.4s", 0, 0, 880, -1, FMULX_4S, "4E22DC20"},
    {"fmulx v0.4s, v1.4s, v2.s[1]", 0, 0, 880, -1, FMULX_4S, "6FA29020"},
    {"fadd v0.4s, v1.4s, v2.4s (the hot code's one lane mask)", 0, 0, 816, -1, "=1 " MOVMSKPS, "4E22D420"},
    {"fmul v0.2d, v1.2d, v2.2d (the hot code's one lane mask)", 0, 0, 864, -1, "=1 " MOVMSKPD, "6E62DC20"},
    {"fmla v0.4s, v1.4s, v2.4s (the hot code's one lane mask)", NEEDS_FMA, 0, 928, -1, "=1 " MOVMSKPS, "4E22CC20"},
    {"fmul v0.4s, v1.4s, v2.4s (the hot test with no SETNE)", 0, 0, 832, -1, "!" SETNE, "6E22DC20"},
    {"fcmp s1, s2 (NZCV off UCOMISS's flags: JP out where unordered)", 0, 0, 448, -1, JP_REL32, "1E222020"},
    {"fcmpe d1, d2 (its cold path's flags back by PUSH and POPFQ)", 0, 0, 400, -1, PUSH_RSP_POPFQ, "1E622030"},
    {"frecps v0.4s, v1.4s, v2.4s (the warm path: its hot test with no SETNE)", 0, 0, 768, -1, "!" SETNE, "4E22FC20"},
    {"frsqrts d0, d1, d2 (the warm path: its hot test with no SETNE)", 0, 0, 800, -1, "!" SETNE, "5EE2FC20"},
    {"ucvtf s0, x1 (from 2^63 up inline: the exponent's PADDD)", 0, 0, 384, -1, PADDD, "9E230020"},
    {"ucvtf d0, x1, #3 (from 2^63 up inline: the exponent's PADDQ)", 0, 0, 416, -1, PADDQ, "9E43F420"},
    {"fneg s0, s1 (by the vector's lanes)", 0, 0, 240, -1, INSERTPS, "1E214020"},
    {"fabs d0, d1 (by the vector's lanes)", 0, 0, 240, -1, MOVQ_XMM, "1E60C020"},
    {"fmax v0.4s, v1.4s, v2.4s (the hot code's one lane mask)", 0, 0, 656, -1, "=1 " MOVMSKPS, "4E22F420"},
    {"fcmeq v0.4s, v1.4s, v2.4s (the hot code's one lane mask)", 0, 0, 640, -1, "=1 " MOVMSKPS, "4E22E420"},
    {"fsqrt v0.4s, v1.4s (the hot code's one lane mask)", 0, 0, 784, -1, "=1 " MOVMSKPS, "6EA1F820"},
    {"frintm v0.4s, v1.4s (the hot code's one lane mask)", 0, 0, 576, -1, "=1 " MOVMSKPS, "4E219820"},
    {"fcvtzs v0.4s, v1.4s (the hot code's one lane mask)", 0, 0, 608, -1, "=1 " MOVMSKPS, "4EA1B820"},
    /* a loop of eight ADD, EOR, EOR into V30 (simd_ab's): V3 to V10 and V30 each save a load and a store a turn, so
       of the nine that tie, V30, read and written eight times a turn, takes a pin (else its chain goes through memory
       at each turn) */
    {"a loop's accumulator V30 among nine V registers that tie", 0, 0, 1216, -1, V30_PIN_LOAD,
     "D2800C90 4EFF8463 6E221C70 6E301FDE 4EFF8484 6E221C91 6E311FDE 4EFF84A5 6E221CB2 6E321FDE 4EFF84C6 6E221CD3 "
     "6E331FDE 4EFF84E7 6E221CF4 6E341FDE 4EFF8508 6E221D15 6E351FDE 4EFF8529 6E221D36 6E361FDE 4EFF854A 6E221D57 "
     "6E371FDE F1000610 54FFFCE1"},
    /* the bytes' carryless products on whole vectors, where a lane at a time was thousands of bytes */
    {"pmul v0.16b, v1.16b, v2.16b", 0, 0, 304, -1, NULL, "6E229C20"},
    {"pmull v0.8h, v1.8b, v2.8b", 0, 0, 368, -1, NULL, "0E22E020"},
    /* whole structures in 16-byte chunks: no PEXTR or PINSR of an element (66 0F 3A 14 to 16, 20 and 22; 66 0F C4, C5) */
    {"ld2 {v0.16b, v1.16b}, [x1]", 0, 0, 160, -1, "!" ELEMENT, "4C408020"},
    {"ld3 {v0.8b, v1.8b, v2.8b}, [x1]", 0, 0, 272, -1, "!" ELEMENT, "0C404020"},
    {"st2 {v0.8b, v1.8b}, [x1]", 0, 0, 80, -1, "!" ELEMENT, "0C008020"},
    {"st4 {v0.16b, v1.16b, v2.16b, v3.16b}, [x1]", 0, 0, 144, -1, "!" ELEMENT, "4C000020"},
    /* INS of an element: no general-purpose register between the two (MOVQ r64, xmm: 66 REX.W 0F 7E) */
    {"ins v0.s[1], v1.s[0]", 0, 0, 80, -1, "!66 48 0F 7E|66 49 0F 7E|66 4C 0F 7E|66 4D 0F 7E", "6E0C0420"},
    {"ins v0.b[3], w1", 0, 0, 64, -1, NULL, "4E071C20"},
    /* a function nothing enters but at its start keeps its stale check (CMP byte [rip + snx_stale], 0), without the
       compare of the entry with its start (CMP RSI, imm32); one of two leaders (a B.NE back to its second word) has both */
    {"add x0, x1, x2 with a stale check: the check", WITH_STALE, 0, 80, -1, "=1 " STALE_TEST, "8B020020"},
    {"add x0, x1, x2 with a stale check: no compare of the entry", WITH_STALE, 0, 80, -1, "!" ENTRY_COMPARE,
     "8B020020"},
    {"add, subs, b.ne back to the subs, with a stale check: the entry's compare", WITH_STALE, 0, 416, -1, ENTRY_COMPARE,
     "8B020020 F1000610 54FFFFE1"},
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
/* how many times t has one of has's byte strings */
static int has_bytes(const unsigned char* t, size_t n, const char* has) {
    int times = 0;
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
            times += j == k;
        }
        if (*has == '|') ++has;
    }
    return times;
}

static void check(const Row* row, int print) {
    unsigned char image[(MOST + 1) * 4], *object = NULL;
    const unsigned char* text;
    const SnxU64 hint = BASE;
    SnxCodeImage module = {.image = image, .base = BASE};
    SnxNativeOptions options = {.stale = (row->needs & WITH_STALE) != 0};
    unsigned char made[4] = {0};
    uint32_t words[MOST];
    size_t size = 0, length = 0;
    char error[256] = "", *at = (char*)row->words;
    SnxCodegen codegen;
    unsigned count = 0;
    g_x64_vec_code.row = row;
    while (*at && count < MOST) words[count++] = (uint32_t)strtoul(at, &at, 16);
    while (*at == ' ') ++at;
    if (*at) { /* a row cut short would test other code */
        fprintf(stderr, "FAIL: %s: words past the %u a row holds\n", row->what, MOST);
        ++g_x64_vec_code.failures;
        return;
    }
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
        if (row->has && row->has[0] == '=') {
            char* rest;
            int want = (int)strtol(row->has + 1, &rest, 10);
            while (*rest == ' ') ++rest;
            int got = has_bytes(text, length, rest);
            if (got != want) {
                fprintf(stderr, "FAIL: %s: %d times %s in its code, not %d\n", row->what, got, rest, want);
                ++g_x64_vec_code.failures;
            }
        } else if (row->has && !has_bytes(text, length, row->has + (row->has[0] == '!')) != (row->has[0] == '!')) {
            fprintf(stderr, "FAIL: %s: %s %s in its code\n", row->what, row->has[0] == '!' ? "has" : "no", row->has);
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
        if ((snx_x64_features() & k_rows[i].needs & ~WITH_STALE) != (k_rows[i].needs & ~WITH_STALE)) continue;
        check(&k_rows[i], print);
        ++done;
    }
    printf("x64 vec code: %u of %zu rows, %d failures\n", done, sizeof k_rows / sizeof *k_rows,
           g_x64_vec_code.failures);
    return g_x64_vec_code.failures != 0;
}
