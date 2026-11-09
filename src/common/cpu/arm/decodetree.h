/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported in part from QEMU (target/arm/tcg/translate.h and translate.c), LGPL-2.1-or-later:
   Copyright (c) 2003 Fabrice Bellard
   Copyright (c) 2005-2007 CodeSourcery
   Copyright (c) 2007 OpenedHand, Ltd. */
/* What the decoders tools/decodetree generates from the .decode files of decode/ need from the file that includes them: QEMU's
   bit operations (common/utilities/decodetree.h) and the !function= helpers of the .decode files. That file typedefs
   DisasContext first */
#ifndef SNX_ARM_DECODETREE_H
#define SNX_ARM_DECODETREE_H

#include "common/utilities/decodetree.h"

/* the arithmetic is unsigned, converted back: the fields are small, and a negated one is a two's complement int */
static inline int times_2(DisasContext* s, int x) { return (void)s, (int)((unsigned)x * 2); }
static inline int times_4(DisasContext* s, int x) { return (void)s, (int)((unsigned)x * 4); }
static inline int times_2_plus_1(DisasContext* s, int x) { return (void)s, (int)((unsigned)x * 2 + 1); }
static inline int plus_2(DisasContext* s, int x) { return (void)s, (int)((unsigned)x + 2); }
static inline int negate(DisasContext* s, int x) { return (void)s, (int)(0u - (unsigned)x); }

/* T32ExpandImm's rotation, and its value before the rotation */
static inline int t32_expandimm_rot(DisasContext* s, int x) {
    return (void)s, x & 0xC00 ? (int)extract32((uint32_t)x, 7, 5) : 0;
}
static inline int t32_expandimm_imm(DisasContext* s, int x) {
    uint32_t imm = extract32((uint32_t)x, 0, 8);
    (void)s;
    switch (extract32((uint32_t)x, 8, 4)) {
        case 0: break;
        case 1: imm *= 0x00010001u; break;
        case 2: imm *= 0x01000100u; break;
        case 3: imm *= 0x01010101u; break;
        default: imm |= 0x80; break;
    }
    return (int)imm;
}

/* J1:J2 at x[22:21] to I1:I2 (I = NOT(J XOR S)), and the final zero */
static inline int t32_branch24(DisasContext* s, int x) {
    unsigned u = (unsigned)x ^ (x < 0 ? 0 : 3u << 21);
    return (void)s, (int)(u << 1);
}

static inline int t16_push_list(DisasContext* s, int x) { return (void)s, (x & 0xFF) | (x & 0x100) << (14 - 8); }
static inline int t16_pop_list(DisasContext* s, int x) { return (void)s, (x & 0xFF) | (x & 0x100) << (15 - 8); }

#endif
