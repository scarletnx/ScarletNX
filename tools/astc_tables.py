#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The ASTC encoder's table of partitions (src/nx/gpu/texture/astc_encode_tables.glsl).

    python3 tools/astc_tables.py > src/nx/gpu/texture/astc_encode_tables.glsl

ASTC names a partition of a block by a seed, which the decoder hashes into a texel's partition (astc_decode.glsl's
SlotPartition, ported here line for line). The encoder's fitter knows its way around BC7's 64 partitions of two
subsets, so for each of those this finds the seed whose partition is nearest. The colour encodings need no table:
astc_quant.glsl works them out.
"""


def bits(v, off, n):
    return (v >> off) & ((1 << n) - 1)


def hash52(p):
    p = ((p ^ (p >> 15)) * 4007528593) & 0xFFFFFFFF
    p ^= p >> 5
    p = (p + (p << 16)) & 0xFFFFFFFF
    p ^= p >> 7
    p ^= p >> 3
    p = (p ^ (p << 6)) & 0xFFFFFFFF
    p ^= p >> 17
    return p


def partition2(seed10):
    """astc_decode.glsl's partition of a 4x4 block's texels in two, by seed: a bit a texel, texel y * 4 + x."""
    seed = seed10 + 1024
    r = hash52(seed)
    odd = (4 if seed & 2 else 5) if seed & 1 else 5
    even = 5 if seed & 1 else (4 if seed & 2 else 5)
    n = [bits(r, 4 * k, 4) for k in range(8)]
    step = [((n[0] * n[0]) >> odd) * 2, ((n[2] * n[2]) >> odd) * 2]
    rowstep = [((n[1] * n[1]) >> even) * 2, ((n[3] * n[3]) >> even) * 2]
    row = [bits(r, 14, 6), bits(r, 10, 6)]
    mask = 0
    for j in range(4):
        for i in range(4):
            a = (row[0] + rowstep[0] * j + step[0] * i) & 63
            b = (row[1] + rowstep[1] * j + step[1] * i) & 63
            if a < b:
                mask |= 1 << (j * 4 + i)
    return mask


# BC7's 64 partitions of two subsets (bc7_encode.glsl's P2), which the encoder's search already knows its way around
BC7_P2 = [
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8, 0xFF00,
    0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C,
    0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8,
    0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6, 0x639C,
    0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22]


def main():
    # for each of BC7's shapes the seed whose partition is nearest it, either way round (the earliest of equal ones),
    # as a mask with texel 0 in subset 0
    seeds = [partition2(k) for k in range(1024)]
    words, exact = [], 0
    for shape in BC7_P2:
        best = min(range(1024), key=lambda k: (min(bin(seeds[k] ^ shape).count("1"),
                                                   bin(seeds[k] ^ shape ^ 0xFFFF).count("1")), k))
        m = seeds[best]
        flip = m & 1  # texel 0 in ASTC's partition 1: the mask's subset 0 is that one
        m = m ^ 0xFFFF if flip else m
        assert 0 < m < 0xFFFF and not m & 1
        words.append(m | (best << 16) | (flip << 26))
        exact += m == shape
    print("/* SPDX-License-Identifier: GPL-3.0-or-later")
    print("   Copyright 2026 the ScarletNX contributors */")
    print("/* astc_encode.glsl's table of partitions, written by tools/astc_tables.py from the decoder's own rule")
    print("   (astc_decode.glsl): do not edit. ASTC's partitions of a 4x4 block in two nearest BC7's 64 (%d of them the" % exact)
    print("   same), a word each: in the low 16 bits a bit a texel, its subset, texel 0 in subset 0 (AS_P2); above them the")
    print("   partition's seed, 10 bits, and in bit 10 of that whether subset 0 is ASTC's partition 1 (AS_SEED2). Only the")
    print("   passes that fit a block of two partitions read it */")
    print("#define AS_P2(i) (ASTC_PART[i] & 0xFFFFu)")
    print("#define AS_SEED2(i) (ASTC_PART[i] >> 16u)")
    print("const uint ASTC_PART[64] = {")
    for k in range(0, 64, 8):
        print("    " + ", ".join("0x%08Xu" % w for w in words[k : k + 8]) + ("," if k + 8 < 64 else "};"))


main()
