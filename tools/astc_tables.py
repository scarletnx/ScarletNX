#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
"""The ASTC encoder's tables (src/nx/gpu/texture/astc_encode_tables.glsl), made by turning the decoder round.

    python3 tools/astc_tables.py > src/nx/gpu/texture/astc_encode_tables.glsl

astc_decode.glsl decodes a block of five trits from 8 bits and one of three quints from 7, and unquantizes a
color value of a trit or a quint and some bits to 8 bits (its DecodeSequence, ColorParams and UnquantizeTQ, ported
here line for line). The encoder needs the other way: the bits of five trits or three quints, and for a color
0 to 255 the nearest value an encoding holds with the trit or quint and the bits that give it. tests/glsl/
astc_encode_glsl.cpp holds the encoder's blocks to the reference decoder, which catches a wrong entry here.
"""


def bits(v, off, n):
    return (v >> off) & ((1 << n) - 1)


def trits_of(T):
    c7 = bits(T, 2, 3) == 7
    h3 = bits(T, 5, 2) == 3
    C = (bits(T, 5, 3) << 2) | bits(T, 0, 2) if c7 else bits(T, 0, 5)
    t4 = 2 if c7 or h3 else bits(T, 7, 1)
    t3 = 2 if c7 else bits(T, 7, 1) if h3 else bits(T, 5, 2)
    l3 = bits(C, 0, 2) == 3
    m3 = bits(C, 2, 2) == 3
    t2 = 2 if l3 or m3 else bits(C, 4, 1)
    t1 = bits(C, 4, 1) if l3 else 2 if m3 else bits(C, 2, 2)
    if l3:
        t0 = (bits(C, 3, 1) << 1) | (bits(C, 2, 1) & ~bits(C, 3, 1) & 1)
    elif m3:
        t0 = bits(C, 0, 2)
    else:
        t0 = (bits(C, 1, 1) << 1) | (bits(C, 0, 1) & ~bits(C, 1, 1) & 1)
    return (t0, t1, t2, t3, t4)


def quints_of(Q):
    high = bits(Q, 1, 2) == 3
    both4 = high and bits(Q, 5, 2) == 0
    C = (bits(Q, 3, 2) << 3) | ((~bits(Q, 5, 2) & 3) << 1) | bits(Q, 0, 1) if high else bits(Q, 0, 5)
    five = bits(C, 0, 3) == 5
    q0 = 4 if both4 else bits(C, 3, 2) if five else bits(C, 0, 3)
    q1 = 4 if both4 or five else bits(C, 3, 2)
    if both4:
        n0 = ~bits(Q, 0, 1) & 1
        q2 = (bits(Q, 0, 1) << 2) | ((bits(Q, 4, 1) & n0) << 1) | (bits(Q, 3, 1) & n0)
    elif high:
        q2 = 4
    else:
        q2 = bits(Q, 5, 2)
    return (q0, q1, q2)


def unquant(trit, b, v, d):
    """astc_decode.glsl's ColorParams and UnquantizeTQ: a trit or quint d over b bits v, to 8 bits."""
    i = b - 1
    bmask = ((1 << b) - 1) >> 1
    if trit:
        c = bits(0x162C5DCC if b <= 4 else 0x050B, (i & 3) * 8, 8)
        mb = bits(0x01160000 if b <= 2 else 0x00410085 if b <= 4 else 0x01010081, (i & 1) * 16, 16)
        sb = bits(0x420000, (i & 7) * 4, 4)
    else:
        c = bits(0x0D1A3671 if b <= 4 else 0x06, (i & 3) * 8, 8)
        mb = bits(0x010C0000 if b <= 2 else 0x00810105 if b <= 4 else 0x0101, (i & 1) * 16, 16)
        sb = bits(0x31100, (i & 7) * 4, 4)
    A = 0x1FF if v & 1 else 0
    B = (((v >> 1) & bmask) * mb) >> sb
    return (A & 0x80) | (((d * c + B) ^ A) >> 2)


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


def words16(name, values, comment):
    out = ["/* %s */" % comment, "const uint %s[%d] = {" % (name, len(values))]
    for k in range(0, len(values), 8):
        out.append("    " + ", ".join("0x%04Xu" % w for w in values[k : k + 8]) + ("," if k + 8 < len(values) else "};"))
    return "\n".join(out)


def packed(name, values, comment):
    words = []
    for k in range(0, len(values), 4):
        w = 0
        for j, v in enumerate(values[k : k + 4]):
            assert 0 <= v < 256
            w |= v << (8 * j)
        words.append(w)
    out = ["/* %s */" % comment, "const uint %s[%d] = {" % (name, len(words))]
    for k in range(0, len(words), 8):
        out.append("    " + ", ".join("0x%08Xu" % w for w in words[k : k + 8]) + ("," if k + 8 < len(words) else "};"))
    return "\n".join(out)


def main():
    # the smallest code of each set of digits: the digits past a sequence's end are 0, and its cut-off top bits too
    trit = {}
    for T in range(255, -1, -1):
        trit[trits_of(T)] = T
    assert len(trit) == 243
    quint = {}
    for Q in range(127, -1, -1):
        quint[quints_of(Q)] = Q
    assert len(quint) == 125
    # a sequence's last block of fewer values keeps only the low bits of its code: ceil(8 n / 5) or ceil(7 n / 3)
    for n in range(1, 5):
        keep = (8 * n + 4) // 5
        for t in trit:
            if all(d == 0 for d in t[n:]):
                assert trit[t] < 1 << keep, (n, t)
    for n in range(1, 3):
        keep = (7 * n + 2) // 3
        for q in quint:
            if all(d == 0 for d in q[n:]):
                assert quint[q] < 1 << keep, (n, q)
    print("/* SPDX-License-Identifier: GPL-3.0-or-later")
    print("   Copyright 2026 the ScarletNX contributors */")
    print("/* astc_encode.glsl's tables, written by tools/astc_tables.py from the decoder's own rules (astc_decode.glsl):")
    print("   do not edit. A byte an entry, four to a word, the first the lowest */")
    print(packed("ASTC_TRITS", [trit[(i % 3, i // 3 % 3, i // 9 % 3, i // 27 % 3, i // 81)] for i in range(243)],
                 "the 8 bits of five trits t0 to t4, at t0 + 3 t1 + 9 t2 + 27 t3 + 81 t4"))
    print(packed("ASTC_QUINTS", [quint[(i % 5, i // 5 % 5, i // 25)] for i in range(125)],
                 "the 7 bits of three quints q0 to q2, at q0 + 5 q1 + 25 q2"))
    # the color encodings with a trit or a quint that the encoder's block layouts come to: (trit, bits)
    ranges = [(1, 6), (0, 5), (1, 5), (0, 4), (1, 4), (0, 3)]
    near_all, code_all = [], []
    for trit_kind, b in ranges:
        digits = 3 if trit_kind else 5
        held = {}
        for d in range(digits):
            for v in range(1 << b):
                held.setdefault(unquant(trit_kind, b, v, d), (d << b) | v)
        assert len(held) == digits << b and 0 in held and 255 in held
        vals = sorted(held)
        near = [min(vals, key=lambda u: (abs(u - t), u)) for t in range(256)]
        near_all += near
        code_all += [held.get(u, 0) for u in range(256)]
    # ASTC's partitions of a 4x4 block in two, by seed, a texel's bit its partition; for each of BC7's shapes the seed
    # whose partition is nearest it, either way round (the earliest of equal ones), as a mask with texel 0 in subset 0
    seeds = [partition2(k) for k in range(1024)]
    masks, picks, exact = [], [], 0
    for shape in BC7_P2:
        best = min(range(1024), key=lambda k: (min(bin(seeds[k] ^ shape).count("1"),
                                                   bin(seeds[k] ^ shape ^ 0xFFFF).count("1")), k))
        m = seeds[best]
        flip = m & 1  # texel 0 in ASTC's partition 1: the mask's subset 0 is that one
        masks.append(m ^ 0xFFFF if flip else m)
        picks.append(best | (flip << 10))
        exact += masks[-1] == shape
    assert all(0 < m < 0xFFFF and not m & 1 for m in masks)
    print(words16("ASTC_P2", masks,
                  "ASTC's partitions of a 4x4 block in two nearest BC7's 64 (%d of them the same): a bit a texel, its "
                  "subset, texel 0 in subset 0" % exact))
    print(words16("ASTC_SEED2", picks,
                  "each one's partition seed, and in bit 10 whether subset 0 is ASTC's partition 1"))
    print("/* the color encodings with a trit or a quint, in ASTC_RANGE_ order: trits over 6 bits (192 values), quints")
    print("   over 5 (160), trits over 5 (96), quints over 4 (80), trits over 4 (48), quints over 3 (40) */")
    print(packed("ASTC_NEAR", near_all,
                 "the value an encoding holds nearest a color 0 to 255 (the lower of two as near), at range * 256 + color"))
    print(packed("ASTC_CODE", code_all,
                 "the trit or quint and bits of a value an encoding holds, digit << bits | bits, at range * 256 + value"))


main()
