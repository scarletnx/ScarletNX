/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Which BC2, BC3 and BC7 textures a GPU that samples ETC2 and ASTC but not BCn holds as ASTC 4x4 (texture.c makes it
   between frames, astc_encode.glsl encodes it): the ones whose alpha stays one value within every block. Their colours
   come out of an ASTC 4x4 block 5 to 16 dB nearer than out of ETC2's, and their alpha as it was (BC2's and BC3's
   exact; BC7's exact at 0 and 255, else the nearest of 192 levels, one off at most). A block whose alpha moves is
   another matter: BC2's, BC3's and ETC2's alpha is a block of its own, 8 or 16 levels a texel, where this encoder's
   block of two planes has four levels a plane, 7 to 17 dB behind ETC2's. So those stay ETC2 RGBA8, as they load.

   The answer comes from the blocks' own bytes, with no decoding: it may call a block whose alpha is one value moving
   (its indices pick one value out of two ends that differ), which only leaves a texture as ETC2; never the other way
   round, which bcn_astc_glsl holds against the reference decoder on every kind of block */
#ifndef SNX_TEXTURE_BCN_ASTC_H
#define SNX_TEXTURE_BCN_ASTC_H

/* the kinds of block (the order of tests/support/bcn.h's and of vk.h's VKC_BC2, VKC_BC3 and VKC_BC7) */
enum { SNX_ALPHA_BC2, SNX_ALPHA_BC3, SNX_ALPHA_BC7 };

/* bits at to at + n - 1 of a 16 byte block (n up to 32), its first byte's lowest bit bit 0 */
static inline unsigned snx_bcn_bits(const unsigned char* b, unsigned at, unsigned n) {
    unsigned long long v = 0;
    unsigned first = at >> 3;
    for (unsigned k = 0; k < 5 && first + k < 16; k++) v |= (unsigned long long)b[first + k] << (8 * k);
    return (unsigned)(v >> (at & 7)) & (n >= 32 ? 0xFFFFFFFFu : (1u << n) - 1);
}

/* 1 where the alpha of every texel of a block of kind is one value for sure */
static inline int snx_bcn_alpha_fixed(int kind, const unsigned char* b) {
    if (kind == SNX_ALPHA_BC2) {
        /* sixteen 4 bit alphas in the first 8 bytes */
        for (unsigned i = 0; i < 8; i++)
            if (b[i] != (unsigned char)((b[0] & 15) * 17)) return 0;
        return 1;
    }
    if (kind == SNX_ALPHA_BC3) {
        /* two ends, then sixteen 3 bit indices: 0 and 1 are the ends, 2 to 7 lie between them, but 6 and 7 are 0 and
           255 when the first end is no more than the second. One index throughout, or two equal ends and no 0 and
           255 */
        unsigned first = snx_bcn_bits(b, 16, 3), same = 1, between = 1;
        for (unsigned i = 0; i < 16; i++) {
            unsigned k = snx_bcn_bits(b, 16 + 3 * i, 3);
            same = same && k == first;
            between = between && k < 6;
        }
        return same || (b[0] == b[1] && between);
    }
    /* BC7: the mode is the first byte's lowest set bit; none is a block of transparent black */
    unsigned mode = 0;
    if (!b[0]) return 1;
    while (!(b[0] >> mode & 1)) mode++;
    switch (mode) {
        case 4: { /* 2 bits of rotation at 5, the index selection, three colours' two ends of 5 bits, alpha's of 6 */
            unsigned rot = snx_bcn_bits(b, 5, 2), at = rot ? 8 + 10 * (rot - 1) : 38, n = rot ? 5 : 6;
            /* a rotation puts a colour channel in alpha's place: its two ends, which its indices move between */
            return snx_bcn_bits(b, at, n) == snx_bcn_bits(b, at + n, n);
        }
        case 5: { /* rotation at 6, colours' ends of 7 bits, alpha's of 8 */
            unsigned rot = snx_bcn_bits(b, 6, 2), at = rot ? 8 + 14 * (rot - 1) : 50, n = rot ? 7 : 8;
            return snx_bcn_bits(b, at, n) == snx_bcn_bits(b, at + n, n);
        }
        case 6: /* one subset: alpha's two ends of 7 bits at 49 and a bit more each at 63 */
            return snx_bcn_bits(b, 49, 7) == snx_bcn_bits(b, 56, 7) && snx_bcn_bits(b, 63, 1) == snx_bcn_bits(b, 64, 1);
        case 7: { /* two subsets: alpha's four ends of 5 bits at 74 and a bit more each at 94 */
            unsigned a = snx_bcn_bits(b, 74, 5), p = snx_bcn_bits(b, 94, 1);
            for (unsigned k = 1; k < 4; k++)
                if (snx_bcn_bits(b, 74 + 5 * k, 5) != a || snx_bcn_bits(b, 94 + k, 1) != p) return 0;
            return 1;
        }
    }
    return 1; /* modes 0 to 3 hold no alpha: 255 */
}

/* 1 where every one of count blocks (16 bytes each, in any order) has one alpha for sure */
static inline int snx_bcn_alpha_all_fixed(int kind, const unsigned char* blocks, unsigned long long count) {
    for (unsigned long long i = 0; i < count; i++)
        if (!snx_bcn_alpha_fixed(kind, blocks + 16 * i)) return 0;
    return 1;
}

#endif
