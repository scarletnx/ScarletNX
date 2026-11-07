/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The A32 and Thumb decoders that tools/decodetree makes from QEMU's .decode files (src/common/cpu/arm/a32.c,
   thumb.c), against the hand-written decoders they replaced (support/arm_decode_ref_a32.c, arm_decode_ref_thumb.c):
   every T16 halfword, every T32 pair, and the A32 words at a stride of 13, which meets every class of encoding
   (--full: every A32 word, about two minutes). A refused encoding agrees on its length 0 alone; a decoded one on
   every field the emitter reads.

   One difference is the new decoder's on purpose: T32 CLREX with an option other than 1111 was the A32 word 0 (AND
   r0, r0, r0) to the old decoder, and is UNDEFINED, as the A32 decoder has CLREX with such an option */
#include "support/arm_decode_ref.h"
#include <stdio.h>
#include <string.h>

static unsigned long g_failures;

static void fail(const char* what, uint32_t word) {
    if (g_failures++ < 20) fprintf(stderr, "%s 0x%08x: the decoders differ\n", what, (unsigned)word);
}

/* the old decoder's misdecode of T32 CLREX: op 0x3B, Rn 15, option 0010 with an option other than 1111 */
static int clrex_misdecode(uint16_t a, uint16_t b) {
    return a == 0xF3BF && (b & 0xD000) == 0x8000 && (b & 0xF00) == 0xF00 && ((b >> 4) & 15) == 2 && (b & 15) != 15;
}

static void thumb(uint16_t a, uint16_t b) {
    SnxThumb x, y;
    memset(&x, 0, sizeof x);
    memset(&y, 0, sizeof y);
    unsigned lx = snx_thumb_decode(a, b, &x), ly = snx_thumb_decode_ref(a, b, &y);
    if (lx == 0 && clrex_misdecode(a, b)) return;
    if (lx != ly || (lx && (x.shape != y.shape || x.op != y.op || x.rd != y.rd || x.rn != y.rn || x.rm != y.rm ||
                            x.ra != y.ra || x.imm != y.imm || x.flags != y.flags || x.len != y.len)))
        fail(lx == 1 || ly == 1 ? "T16" : "T32", (uint32_t)a << 16 | b);
}

int main(int argc, char** argv) {
    int full = argc > 1 && !strcmp(argv[1], "--full");
    unsigned long words = 0;
    uint64_t w;
    for (w = 0; w < 0xE800; w++, words++) thumb((uint16_t)w, 0);
    for (w = 0xE8000000u; w <= 0xFFFFFFFFu; w++, words++) thumb((uint16_t)(w >> 16), (uint16_t)w);
    for (w = 0; w <= 0xFFFFFFFFu; w += full ? 1 : 13, words++)
        if (snx_a32_decode((uint32_t)w) != snx_a32_decode_ref((uint32_t)w)) fail("A32", (uint32_t)w);
    /* the stride's own check: these encodings of each class are all met */
    {
        static const uint32_t a32[] = {0xE1A00000u, 0xE59F0004u, 0xF57FF01Fu, 0xE7F000F0u, 0xEE100E10u, 0xF2000000u};
        static const uint32_t t32[] = {0xF3BF8F2Fu, 0xE8BD8001u, 0xFB00F000u, 0xEC400A10u};
        for (size_t i = 0; i < sizeof a32 / sizeof a32[0]; i++)
            if (snx_a32_decode(a32[i]) != snx_a32_decode_ref(a32[i])) fail("A32", a32[i]);
        for (size_t i = 0; i < sizeof t32 / sizeof t32[0]; i++) thumb((uint16_t)(t32[i] >> 16), (uint16_t)t32[i]);
    }
    printf("arm_decode_tree: %lu instructions, %lu differ\n", words, g_failures);
    return g_failures != 0;
}
