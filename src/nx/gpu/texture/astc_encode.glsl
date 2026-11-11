/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC 4x4 encoding: texconv.comp built with SNX_TC_ASTC (vk/texconv.c runs it) decodes a BC2, BC3 or BC7 block with
   texconv.glsl's decoders and encodes it again as an ASTC 4x4 block, for a GPU that samples ASTC but not BCn (Mali,
   Adreno's own driver): the same bytes a texel as the game's blocks. It is made to run as textures load: one
   or two candidates a block, taken from what the source block says of itself.

   What the source says: a BC7 block's mode names its subsets and whether a channel has indices of its own, and a BC2
   or BC3 block always keeps alpha apart from its colours. The ASTC block follows it:
   - every texel the same: a void extent block, exact;
   - colours on one line, alpha 255: one partition, RGB ends of 192 levels, 4 bit weights (AX_RGB);
   - colours and alpha on one line (BC7's mode 6 with alpha, a BC3 block of one colour): one partition, RGBA ends of
     192 levels, 3 bit weights (AX_RGBA);
   - colours on one line under one alpha that is not 255, in a block that keeps alpha apart (BC2's and BC3's colours
     are BC1's four, which 2 bit weights hold where 3 bit ones fall between them): RGBA ends of 256 levels, the
     alpha exact, 2 bit weights (AX_RGBA2);
   - a channel with indices of its own that moves while the others do (BC2's and BC3's alpha, the channel BC7's modes
     4 and 5 rotate out, mode 7's alpha): two planes, RGBA ends of 48 levels, 2 bit weights each (AX_DUAL);
   - two or three subsets without alpha (BC7's modes 0 to 3): two partitions, RGB ends of 40 levels, 2 bit weights
     (AX_TWO). ASTC's partitions come from a hash of a 10 bit seed, not BC7's tables: AX_PART2 and AX_PART3 name, for
     each BC7 shape, the seed whose shape is nearest (30 of BC7's 64 two subset shapes exist as they are; three
     subsets become one alone and two together) and the texels of its second partition.
   The ends' ranges are not a choice: the format gives the bits the weights leave to the largest range that fits.

   A subset's ends are the corners of its texels' bounding box, which a BCn subset's texels lie between (they are
   interpolated from two ends): the channel that moves most says which texels are the line's first and last, and each
   other channel takes its low or its high value at the first as it has there. Each end is stored as its nearest level
   and the texels' weights are their places between the two as stored, measured along the channels that move (each
   counting 0, 1 or 2 by how much it moves beside the one that moves most), so a texel on the line gets its place
   whatever the scale.

   Arithmetic. The values are small integers held in floats of relaxed precision (AE_F, AE_V: mediump, which a mobile
   GPU computes in 16 bits, several at a time): a 16 bit float holds every integer up to 2048, so sums and products
   that stay within it are exact in 16 bits as in 32, and those are all there is but three multiplications by a
   constant whose product is floored (ax_levels), each proved to give the integer result on every input in both
   widths (ucas equiv over f16 and f32: the statements are in docs/layers/gpu/bcn.md). So the blocks do not depend
   on the width the GPU picks, and tests/glsl/bcn_astc_glsl.cpp gets the same bytes running this file with 32 bit
   floats and with every operation rounded to 16. A texel's weight is one 16 bit integer multiplication. */

/* the block's texels are texconv.glsl's tx_px, RGBA8 words (R in the low byte), row by row: texel (x, y) is y * 4 + x */

/* the block being built: word 0's bit 0 is the block's bit 0 */
uint ax_w[4];
/* the texels as floats, R G B A */
AE_V ax_px[16];
/* the ends' stored symbols, in the order the block stores them: the bits (the lowest the half of the range the value
   is in) and the trit or quint above them. An end's alpha is made with its colours, so two partitions' RGB ends, six
   values each, leave the second's alpha past the twelve */
uint ax_m[14];
uint ax_d[14];
/* the ends ax_ends made last, as the decoder reads them back */
AE_V ax_u0;
AE_V ax_u1;
/* how far the block being built is from the texels (ax_weights) */
highp float ax_err;

/* how far from the texels (ax_err: 105ths of a level, summed over the texels' channels) a block of one line is near
   enough for the source's own way not to be tried: a level a channel of an opaque block's 48 */
#ifndef AX_NEAR
#define AX_NEAR 5040.0
#endif
/* the kinds of block (ax_encode's choice) */
#define AX_RGB 0u
#define AX_RGBA 1u
#define AX_DUAL 2u
#define AX_TWO 3u
#define AX_RGBA2 4u

/* five trits t0 + 3 t1 + 9 t2 + 27 t3 + 81 t4 as the byte the format packs them in, four a word */
const uint AX_TRITS[61] = {
    0x04020100u, 0x09080605u, 0x1211100Au, 0x18161514u, 0x07031A19u, 0x1B17130Bu, 0x200E0D0Cu, 0x25242221u,
    0x2A292826u, 0x34323130u, 0x39383635u, 0x2B27233Au, 0x2C3B3733u, 0x41402E2Du, 0x46454442u, 0x504A4948u,
    0x55545251u, 0x5A595856u, 0x534B4743u, 0x4D4C5B57u, 0x8281804Eu, 0x88868584u, 0x91908A89u, 0x96959492u,
    0x839A9998u, 0x97938B87u, 0x8E8D8C9Bu, 0xA4A2A1A0u, 0xA9A8A6A5u, 0xB2B1B0AAu, 0xB8B6B5B4u, 0xA7A3BAB9u,
    0xBBB7B3ABu, 0xC0AEADACu, 0xC5C4C2C1u, 0xCAC9C8C6u, 0xD4D2D1D0u, 0xD9D8D6D5u, 0xCBC7C3DAu, 0xCCDBD7D3u,
    0x6160CECDu, 0x66656462u, 0x706A6968u, 0x75747271u, 0x7A797876u, 0x736B6763u, 0x6D6C7B77u, 0xE2E1E06Eu,
    0xE8E6E5E4u, 0xF1F0EAE9u, 0xF6F5F4F2u, 0xE3FAF9F8u, 0xF7F3EBE7u, 0xEEEDECFBu, 0x3C1E1D1Cu, 0x5D5C3E3Du,
    0x9E9D9C5Eu, 0xDCBEBDBCu, 0x3F1FDEDDu, 0xDFBF9F5Fu, 0x007E7D7Cu};
/* three quints q0 + 5 q1 + 25 q2 as the 7 bits the format packs them in, four a word */
const uint AX_QUINTS[32] = {
    0x03020100u, 0x0A090804u, 0x11100C0Bu, 0x18141312u, 0x1C1B1A19u, 0x1D150D05u, 0x22212006u, 0x29282423u,
    0x302C2B2Au, 0x34333231u, 0x3B3A3938u, 0x352D253Cu, 0x41400E3Du, 0x48444342u, 0x4C4B4A49u, 0x53525150u,
    0x5A595854u, 0x4D455C5Bu, 0x60165D55u, 0x64636261u, 0x6B6A6968u, 0x7271706Cu, 0x79787473u, 0x657C7B7Au,
    0x1E7D756Du, 0x47466766u, 0x4E6F6E26u, 0x77762E4Fu, 0x7E365756u, 0x3E5F5E7Fu, 0x3F372F27u, 0x00000007u};
/* a BC7 two subset partition's ASTC seed (bits 0 to 9) and the texels of that seed's second partition (bit y * 4 + x
   from bit 10) */
const uint AX_PART2[64] = {
    0x0333301Cu, 0x02222014u, 0x00444410u, 0x03B3201Du, 0x00DDFC5Bu, 0x03FBB009u, 0x0004DC6Bu, 0x004DFC48u,
    0x03200095u, 0x00004CCCu, 0x03FA0032u, 0x005FFC72u, 0x00005DF0u, 0x0003FC11u, 0x03FFC04Eu, 0x003FFC27u,
    0x0003BFE1u, 0x03FDC4FCu, 0x023BFF3Cu, 0x0023382Bu, 0x0002309Cu, 0x01CC4074u, 0x033BFCD2u, 0x01CCC5DCu,
    0x00223111u, 0x033BBEACu, 0x01999967u, 0x03364C9Fu, 0x03A04C31u, 0x03C03CF6u, 0x01C23925u, 0x01199863u,
    0x015554C3u, 0x003C3EB6u, 0x03B607C6u, 0x0324CC0Eu, 0x030FC7AAu, 0x02891623u, 0x03A1A382u, 0x036EDEC9u,
    0x02204555u, 0x004D20AEu, 0x03364C9Fu, 0x00FFF2DAu, 0x01224B2Bu, 0x00F00D2Eu, 0x019226D7u, 0x00090090u,
    0x0001C146u, 0x0003B04Cu, 0x003B01A2u, 0x03777E8Fu, 0x00DB260Cu, 0x020DB3BEu, 0x0313E6D4u, 0x0273B873u,
    0x03B325D7u, 0x0273B873u, 0x01FB0417u, 0x0003DCCFu, 0x00C43E42u, 0x003F23C4u, 0x02222C19u, 0x0047F4E7u};
/* the same for a three subset partition: the seed whose two partitions are nearest one subset alone and the other
   two together */
const uint AX_PART3[64] = {
    0x0004CC3Du, 0x03FF20E2u, 0x03320089u, 0x00033A51u, 0x0003FC11u, 0x0333301Cu, 0x0003FC11u, 0x0333301Cu,
    0x0003FC11u, 0x03FFC04Eu, 0x03FFC04Eu, 0x0333301Cu, 0x00444410u, 0x00444410u, 0x00004CCCu, 0x0002309Cu,
    0x0004DC6Bu, 0x0023382Bu, 0x03FFC04Eu, 0x003FFC27u, 0x00444410u, 0x02222014u, 0x0044DC05u, 0x01DDC03Eu,
    0x0233B849u, 0x0004CC3Du, 0x02602CAFu, 0x00007DDBu, 0x03FB3DAAu, 0x03FEE6B8u, 0x02C44D70u, 0x0003BFE1u,
    0x03FE0252u, 0x03333919u, 0x004CCF1Fu, 0x01999967u, 0x03C03CF6u, 0x020907AFu, 0x000408B1u, 0x00C00DEFu,
    0x00F00D2Eu, 0x0003FC11u, 0x0003FC11u, 0x0333301Cu, 0x0333301Cu, 0x01999967u, 0x03C03CF6u, 0x03FFC04Eu,
    0x003FFC27u, 0x00444410u, 0x02222014u, 0x03FFC04Eu, 0x00444410u, 0x02222014u, 0x003FFC27u, 0x0003FC11u,
    0x0003FC11u, 0x0333301Cu, 0x00C00DEFu, 0x003FFC27u, 0x02222014u, 0x00444410u, 0x03FFC04Eu, 0x0023382Bu};

/* an RGBA8 word's four channels */
AE_V ax_texel(uint p) { return ae_v(ae_f(p & 255u), ae_f((p >> 8) & 255u), ae_f((p >> 16) & 255u), ae_f(p >> 24)); }
/* n bits of v (v below 1 << n) into the block from bit at up */
void ax_put(uint at, uint n, uint v) {
    uint k = at >> 5, s = at & 31u;
    ax_w[k] |= v << s;
    if (s + n > 32u) ax_w[k + 1u] |= v >> (32u - s);
}

/* The levels nearest v's four values (integers, 0 to 255) of a range of levels levels (256, 192, 48 or 40): their
   symbols into ax_m and ax_d at at, at + 2, at + 4 and at + 6 (an end's channels are stored every other value), and
   the values they decode to, which it returns. 256 levels are the values themselves, 8 bits each.

   A range's levels are a trit or a quint above bits whose lowest says which half of 0 to 255 the level is in, the
   upper half the lower mirrored (255 less it). In the lower half, with n the other bits and d the trit or quint:
   192 levels are 4 n + d; 48 are (65 n + 22 d) / 4; 40 are (130 n + n / 2 + 26 d) / 4, each rounded down. The
   nearest is found by its rank k = 3 n + d (or 5 n + d): w less its remainder's excess for 192; for the others
   k = floor((w + c1) * c2) and n = floor(k * c3), the three products whose rounding ucas proved harmless (a value of
   192 one off a level, w = 4 n + 3, takes the level below) */
AE_V ax_levels(AE_V v, uint levels, uint at) {
    if (levels == 256u) {
        for (uint c = 0u; c < 4u; c++) {
            ax_m[at + 2u * c] = ae_u(v[c]);
            ax_d[at + 2u * c] = 0u;
        }
        return v;
    }
    /* a: 1 in the upper half. w: v in the lower half's terms */
    AE_V a = step(ae_vs(128.0), v), w = v + a * (ae_vs(255.0) - v - v), n, d, u;
    if (levels == 192u) {
        n = floor(w * 0.25);
        d = min(w - n * 4.0, ae_vs(2.0));
        u = n * 4.0 + d;
    } else if (levels == 48u) {
        AE_V k = floor((w + ae_vs(2.875)) * 0.1845703125);
        n = floor(k * 0.33349609375);
        d = k - n * 3.0;
        u = floor((n * 65.0 + d * 22.0) * 0.25);
    } else {
        AE_V k = floor((w + ae_vs(4.0)) * 0.152587890625);
        n = floor(k * 0.2001953125);
        d = k - n * 5.0;
        u = floor((n * 130.0 + floor(n * 0.5) + d * 26.0) * 0.25);
    }
    for (uint c = 0u; c < 4u; c++) {
        ax_m[at + 2u * c] = (ae_u(n[c]) << 1) | ae_u(a[c]);
        ax_d[at + 2u * c] = ae_u(d[c]);
    }
    return u + a * (ae_vs(255.0) - u - u);
}

/* The two ends of the texels of set (a bit a texel) in the channels of use (1.0 for a channel on the line, 0.0 for
   one that is not: it takes its lowest value at both ends, which is its only one, or another plane's), as levels of
   levels: their symbols into ax_m and ax_d from at, the first end's channels at at, at + 2, at + 4, at + 6 and the
   second's one above each, and what they decode to into ax_u0 and ax_u1. own is a channel with a plane of its own
   (4: none), which runs from its lowest to its highest. lead is a channel the line follows alone (4: none): the ends
   are then the texels where it is lowest and highest, as they are, where the others' ends are the corners of the
   texels' bounding box. The ends go the way that keeps the decoder from reading them as the format's other colour
   encoding (blue contraction: the second's R, G and B sum below the first's) */
void ax_ends(uint set, AE_V use, uint own, uint lead, uint levels, uint at) {
    AE_V lo = ae_vs(255.0), hi = ae_vs(0.0);
    for (uint i = 0u; i < 16u; i++) {
        if (((set >> i) & 1u) == 0u) continue;
        lo = min(lo, ax_px[i]);
        hi = max(hi, ax_px[i]);
    }
    /* the channel that moves most, the first of them */
    AE_V range = (hi - lo) * use;
    uint ref = 0u;
    for (uint c = 1u; c < 4u; c++)
        if (range[c] > range[ref]) ref = c;
    if (lead < 4u) ref = lead;
    /* the texels where it is lowest and highest, the first of each */
    uint first = 16u, last = 16u;
    for (uint i = 0u; i < 16u; i++) {
        if (((set >> i) & 1u) == 0u) continue;
        if (first == 16u || ax_px[i][ref] < ax_px[first][ref]) first = i;
        if (last == 16u || ax_px[i][ref] > ax_px[last][ref]) last = i;
    }
    AE_V e0 = ax_px[first], e1 = ax_px[last];
    if (lead >= 4u) {
        /* up: 1 for a channel that is no lower at the last texel than at the first */
        AE_V up = step(ax_px[first], ax_px[last]);
        if (own < 4u) up[own] = 1.0;
        e0 = hi + (lo - hi) * up;
        e1 = lo + hi - e0;
        /* a channel off the line holds one value */
        e0 = lo + (e0 - lo) * use;
        e1 = lo + (e1 - lo) * use;
        if (own < 4u) {
            e0[own] = lo[own];
            e1[own] = hi[own];
        }
    }
    AE_V u0 = e0, u1 = e1;
    for (uint e = 0u; e < 2u; e++) {
        AE_V u = ax_levels(e == 0u ? e0 : e1, levels, at + e);
        if (e == 0u)
            u0 = u;
        else
            u1 = u;
    }
    if (u1[0] + u1[1] + u1[2] < u0[0] + u0[1] + u0[2]) {
        for (uint c = 0u; c < 4u; c++) {
            uint m = ax_m[at + 2u * c], d = ax_d[at + 2u * c];
            ax_m[at + 2u * c] = ax_m[at + 2u * c + 1u];
            ax_d[at + 2u * c] = ax_d[at + 2u * c + 1u];
            ax_m[at + 2u * c + 1u] = m;
            ax_d[at + 2u * c + 1u] = d;
        }
        ax_u0 = u1;
        ax_u1 = u0;
    } else {
        ax_u0 = u0;
        ax_u1 = u1;
    }
}

/* The weights of the texels of set between ax_u0 and ax_u1 along the channels of use, top the highest weight (3, 7
   or 15): each of bits bits into the block's weights, texel i's at place i * stride + slot (two planes store theirs
   in turn). A texel's place is its distance from the first end along the channels that move, each counting twice,
   once or not at all as it moves three quarters, a quarter or less of what the one that moves most does: at most
   2040 in all, which a 16 bit float holds, and a texel on the line between the ends gets its place on it. The weight
   is that place over the whole distance, times top, rounded: (place * m + 2048) >> 12 with m = top * 4096 /
   distance, below 65536 throughout.

   How far the texels are from what their weights give is added to ax_err, over the channels of held: each channel's
   top * (texel - first end) - (second end - first) * weight, as the weights were evenly spaced, in 105ths of a level
   (105 is 3 * 5 * 7: every top divides it), so the kinds of block compare. These pass 16 bits and are 32 bit floats,
   whole numbers below 2^24 */
void ax_weights(uint set, AE_V use, AE_V held, uint top, uint bits, uint stride, uint slot) {
    AE_V d = (ax_u1 - ax_u0) * use, size = abs(d);
    AE_F most = max(max(size[0], size[1]), max(size[2], size[3]));
    /* 0, 1 or 2 with d's sign: 4 * size is at most 1020 and 3 * most 765 */
    AE_V dir = (step(ae_vs(most), size * 4.0) + step(ae_vs(most * 3.0), size * 4.0)) * sign(d);
    uint whole = ae_u(dot(d, dir));
    uint m = whole != 0u ? (top << 12) / whole : 0u;
    highp vec4 from = vec4(ax_u0), span = (vec4(ax_u1) - from) * vec4(held), sum = vec4(0.0);
    for (uint i = 0u; i < 16u; i++) {
        if (((set >> i) & 1u) == 0u) continue;
        uint place = ae_u(clamp(dot(ax_px[i] - ax_u0, dir), ae_f(0u), ae_f(whole)));
        uint k = (place * m + 2048u) >> 12;
        /* the weights are stored from the block's last bit down, each one's bits reversed with them */
        uint at = (i * stride + slot + 1u) * bits;
        ax_put(128u - at, bits, bitfieldReverse(k) >> (32u - bits));
        highp vec4 off = (vec4(ax_px[i]) - from) * vec4(held);
        sum += abs(off * float(top) - span * float(k));
    }
    ax_err += (sum[0] + sum[1] + sum[2] + sum[3]) * float(105u / top);
}

/* count values of bits bits and a trit each, from ax_m and ax_d, into the block from bit at: five values share the
   byte their trits pack into, its bits between theirs (two after the first, second and fourth, one after the third
   and fifth); a last group of fewer stops after its own */
void ax_put_trits(uint at, uint count, uint bits) {
    for (uint g = 0u; g < count; g += 5u) {
        uint t = 0u, scale = 1u;
        for (uint j = 0u; j < 5u && g + j < count; j++) {
            t += ax_d[g + j] * scale;
            scale *= 3u;
        }
        uint code = (AX_TRITS[t >> 2] >> ((t & 3u) * 8u)) & 255u;
        for (uint j = 0u; j < 5u && g + j < count; j++) {
            uint n = j == 2u || j == 4u ? 1u : 2u, from = j == 0u ? 0u : j == 1u ? 2u : j == 2u ? 4u : j == 3u ? 5u : 7u;
            ax_put(at, bits, ax_m[g + j]);
            ax_put(at + bits, n, (code >> from) & ((1u << n) - 1u));
            at += bits + n;
        }
    }
}
/* count (a multiple of 3) values of bits bits and a quint each: three share 7 packed bits, three after the first,
   two after the second and the third */
void ax_put_quints(uint at, uint count, uint bits) {
    for (uint g = 0u; g < count; g += 3u) {
        uint q = ax_d[g] + ax_d[g + 1u] * 5u + ax_d[g + 2u] * 25u;
        uint code = (AX_QUINTS[q >> 2] >> ((q & 3u) * 8u)) & 127u;
        for (uint j = 0u; j < 3u; j++) {
            uint n = j == 0u ? 3u : 2u, from = j == 0u ? 0u : j == 1u ? 3u : 5u;
            ax_put(at, bits, ax_m[g + j]);
            ax_put(at + bits, n, (code >> from) & ((1u << n) - 1u));
            at += bits + n;
        }
    }
}

/* tx_px (ax_px) as a block of kind into ax_w, and how far it is from the texels into ax_err. part and seed: the
   second partition's texels and its seed (AX_TWO). own: the channel with a plane of its own (AX_DUAL), or the one
   an AX_RGBA line follows alone (4: none, the line is the texels' bounding box's; AX_RGBA2's always is).

   The kinds differ in a few numbers, and the work is one loop of two passes at most, the ends then the weights of
   each: a block's one line; two planes' lines, whose ends are made together; two partitions' lines. Each of ax_ends,
   ax_weights and ax_levels is called from one place, so the compiler that inlines them all has one copy of each to
   compile (an Intel driver took 90 s over the encoder written a call a kind) */
void ax_build(uint kind, uint part, uint seed, uint own) {
    AE_V every = ae_vs(1.0), colour = ae_v(1.0, 1.0, 1.0, 0.0), rest = every, alone = ae_vs(0.0);
    if (own < 4u) {
        rest[own] = 0.0;
        alone[own] = 1.0;
    }
    bool two = kind == AX_TWO, dual = kind == AX_DUAL, rgb = kind == AX_RGB || two;
    /* the ends' range (what the bits the weights leave hold), the highest weight and its bits */
    uint levels = kind == AX_RGB || kind == AX_RGBA ? 192u : dual ? 48u : two ? 40u : 256u;
    uint top = kind == AX_RGB ? 15u : kind == AX_RGBA ? 7u : 3u, bits = kind == AX_RGB ? 4u : kind == AX_RGBA ? 3u : 2u;
    for (uint k = 0u; k < 4u; k++) ax_w[k] = 0u;
    ax_err = 0.0;
    /* bits 0 to 10: a 4x4 grid of weights of 16, 8 or 4 levels, in two planes for AX_DUAL. Then one partition (bits
       11 and 12) and the ends' kind (13 to 16: 8 RGB, 12 RGBA); or two partitions of seed (13 to 22), both RGB ends
       (23 to 28) */
    ax_w[0] = (kind == AX_RGB ? 0x242u : kind == AX_RGBA ? 0x53u : dual ? 0x442u : 0x42u) |
              (two ? (1u << 11) | (seed << 13) | (8u << 25) : (kind == AX_RGB ? 8u : 12u) << 13);
    for (uint pass = 0u; pass < (two || dual ? 2u : 1u); pass++) {
        /* a partition's texels, the second partition's six values after the first's; or the block's */
        uint set = !two ? 0xFFFFu : pass == 0u ? ~part & 0xFFFFu : part;
        /* the channels the line runs along, and those the texels are held to: a plane's own; or the colours; or all
           four, the line of AX_RGBA following own alone where it is told to */
        AE_V on = rgb ? colour : dual ? rest : every;
        AE_V along = dual ? (pass == 0u ? rest : alone) : kind == AX_RGBA && own < 4u ? alone : on;
        AE_V held = dual ? along : on;
        /* two planes' ends are one pair, made with the first plane */
        if (!(dual && pass == 1u)) ax_ends(set, on, dual ? own : 4u, kind == AX_RGBA ? own : 4u, levels, pass * 6u);
        ax_weights(set, along, held, top, bits, dual ? 2u : 1u, dual ? pass : 0u);
    }
    /* the ends' values from bit 17 (29 after two partitions' seed): twelve of 40 levels; eight of 256, 8 bits each;
       or six (RGB) or eight of 192 or 48, a trit above 6 or 4 bits */
    if (two) {
        ax_put_quints(29u, 12u, 3u);
    } else if (levels == 256u) {
        for (uint k = 0u; k < 8u; k++) ax_put(17u + 8u * k, 8u, ax_m[k]);
    } else {
        ax_put_trits(17u, kind == AX_RGB ? 6u : 8u, levels == 192u ? 6u : 4u);
    }
    /* the second plane's channel in the two bits below the weights */
    if (dual) ax_put(62u, 2u, own);
}

/* tx_px as an ASTC 4x4 block into ax_w. What the source block says of itself: part, the texels of its second
   subset as the ASTC seed seed draws them (0: one subset), and own, the channel it keeps indices of its own for (4:
   none). A block that says either is built as it says and as one line, and the nearer of the two is kept (the
   source's on a tie): an encoder takes subsets or a second plane for precision too, on texels one ASTC line of more
   levels holds better */
void ax_encode(uint part, uint seed, uint own) {
    AE_V lo = ae_vs(255.0), hi = ae_vs(0.0);
    for (uint i = 0u; i < 16u; i++) {
        ax_px[i] = ax_texel(tx_px[i]);
        lo = min(lo, ax_px[i]);
        hi = max(hi, ax_px[i]);
    }
    if (lo == hi) {
        /* a void extent block of no extent: the colour's 16 bits a channel in the top half, each byte twice */
        uint p = tx_px[0];
        ax_w[0] = 0xFFFFFDFCu;
        ax_w[1] = 0xFFFFFFFFu;
        ax_w[2] = ((p & 255u) * 257u) | ((((p >> 8) & 255u) * 257u) << 16);
        ax_w[3] = (((p >> 16) & 255u) * 257u) | (((p >> 24) * 257u) << 16);
        return;
    }
    bool opaque = lo[3] == 255.0;
    /* the source's way: a plane of its own for a channel that moves while another does, else its two subsets where
       two partitions' ends, which hold no alpha, do */
    uint said = 4u;
    if (own < 4u && lo[own] != hi[own]) {
        AE_V rest = ae_vs(1.0);
        rest[own] = 0.0;
        if ((hi - lo) * rest != ae_vs(0.0)) said = AX_DUAL;
    }
    if (said == 4u && part != 0u && opaque) said = AX_TWO;
    /* one line: through the colours, or through colours and alpha (with 2 bit weights and the alpha exact where the
       source keeps alpha apart and it is one value); where the source gives a channel a plane and alpha moves, the
       line follows alpha alone (its ends the texels of lowest and highest alpha), which is what such a
       block's one line is for: a colour that goes with its alpha, at 8 levels of it */
    uint line = opaque ? AX_RGB : own == 3u && lo[3] == hi[3] ? AX_RGBA2 : AX_RGBA;
    uint lead = said == AX_DUAL && lo[3] != hi[3] ? 3u : 4u;
    /* the line first: where it holds the texels within AX_NEAR the block is done, which is most blocks of most
       textures; else the source's way too, kept unless the line is nearer */
    uint w0 = 0u, w1 = 0u, w2 = 0u, w3 = 0u;
    highp float err = 0.0;
    for (uint c = 0u; c < 2u; c++) {
        if (c == 1u && (said == 4u || err <= AX_NEAR)) break;
        ax_build(c == 0u ? line : said, part, seed, c == 0u ? lead : own);
        if (c == 0u || ax_err <= err) {
            w0 = ax_w[0];
            w1 = ax_w[1];
            w2 = ax_w[2];
            w3 = ax_w[3];
            err = ax_err;
        }
    }
    ax_w[0] = w0;
    ax_w[1] = w1;
    ax_w[2] = w2;
    ax_w[3] = w3;
}
