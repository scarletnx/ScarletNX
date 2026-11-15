/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ASTC 4x4 encoding for Adreno (Qualcomm, Vulkan vendor 0x5143): astc_encode.glsl's encoder, block for block the same
   bytes, written for how Adreno's compilers lower GLSL. texconv.glsl includes it in place of astc_encode.glsl in the
   ASTC build's Adreno passes (SNX_ASTC_VENDOR 2), which vk/texconv.c runs on a GPU of that vendor; the first pass, which
   moves blocks and encodes none, is the generic one. Included after astc_encode_tables.glsl, astc_quant.glsl and
   bc7_encode.glsl built with SNX_ASTC_ENC 1; the fitting and the candidates are those of astc_encode.glsl, which says
   what each layout holds.

   What differs is how a block is put together, the work every block written by the second and third passes does once:
   - a 32 bit multiply is three instructions on Adreno (ir3 makes it umul_16x16 and two imadsh_mix16, Mesa's
     ir3_nir_imul.py; the stock compiler carries it as a pseudo-instruction, PSEUDOIMUL32, texconv.comp), so the void
     extent's c * 257 is c | c << 8, two channels a word, and the positions of a sequence's fields add a step a group;
   - an array, and a vector, indexed by a value go to memory or to a relative register move: the weights are packed
     as words, a nibble a weight already, into the block's fields of 2, 3 or 4 bits by halving the gaps between them
     (AsPack2, AsPack3), a turned subset's weights flipped with a mask of whole nibbles, where astc_encode.glsl puts
     each of 16 or 32 weights at a position worked out; the colour values go into two words in registers
     (AsSeqPut), a run of fields at a time, and land in the block with three shifts;
   - a loop over the 16 texels is 15 selects a read in the array-free second pass wherever the driver does not unroll
     it (bc7_encode.glsl's PX): there the two the block's own code has (opaque and one colour, a channel's least and
     greatest) read each texel by its number, an and and an or for the first;
   - what is the same for the whole block is worked out once: the colour encoding's constants (AsKey) before its
     values, not for each;
   - each candidate goes through the one call there is of the function that tries it (AsCandidate, and AsTryDual's two
     lines through one call of Try and one of FitSubset), the compiler inlining every call: a quarter less code in the
     third pass.
   The passes' builds for the Adreno parts with packed dot products (dp2acc and dp4acc from a6xx's third generation on,
   Mesa's freedreno_devices.py) take SPIR-V's, where the driver says it has them: the fitter's inner loop is dot
   products of packed bytes. The bytes are held to astc_encode.glsl's by tests/glsl/astc_encode_glsl.cpp, every block
   of its sets and every layout's packing alone, and on the GPU by tests/astc_vulkan.c. Integer arithmetic but for the
   fitter's reciprocal. */

/* A block stops trying candidates once its squared error over its texels and channels is this or less (bc7_encode.
   glsl's SNX_BC7_GOOD_ENOUGH: a mean squared error of a quarter, 54 dB) */
#ifndef SNX_ASTC_GOOD
#define SNX_ASTC_GOOD 16
#endif

/* What the block's source says of it, where it has one (texconv.glsl's ASTC build: the BCn block the texels were
   decoded from), which AstcEncode tries before any search (SNX_ASTC_HINTS). A BCn block is lines already: BC7's mode
   6 one through RGBA, its modes 4 and 5 one for three channels and one for the fourth, its modes 1 and 3 one a subset
   of a partition, and BC2 and BC3 a colour line beside their alpha. as_hint is 1 for one line, 2 for two planes (the
   channel as_hplane apart, alpha 3), 3 for two partitions (as_hpart of ASTC_P2's, BC7's own number), 0 for none (BC7's
   three subsets and its two with alpha, which no layout here holds). as_h0 and as_h1 are each subset's ends, RGBA8
   words, and as_hib the bits of the source's weights, which picks the layout of one line of an opaque block.

   A hinted line is taken as it is: its ends quantized to the layout's colour encoding and each texel given the
   weight nearest its place, one pass of the texels (bc7_encode.glsl's Try, quick), where the search fits a box and
   least squares over several. Its error is still the texels' own, so a hint that is wrong, or that the layout holds
   badly (ASTC's partition is not always BC7's, and two partitions leave an end 40 values), costs time and little
   else: past SNX_ASTC_HINT_ENOUGH the search runs as without one, and keeps the hinted block if it finds no better */
#ifndef SNX_ASTC_HINTS
#define SNX_ASTC_HINTS 1
#endif
/* 1024 is a squared error of 4 a channel as a root mean square. The search is a hundred times a hinted line's cost
   and what it finds for such a block is seldom much nearer, the layouts being what they are: at 256 the blocks
   searched for were 6% of one game's level and nearly all of its time, and at 1024 the level takes half the time for
   a quarter of a dB (docs/layers/gpu/bcn.md) */
#ifndef SNX_ASTC_HINT_ENOUGH
#define SNX_ASTC_HINT_ENOUGH 1024
#endif
/* SNX_ASTC_STOP 1: AstcEncode writes the blocks of one colour and those a hint settles, and stops at the others with
   as_more set, where it would search: texconv.glsl's ASTC build runs in passes (its main), as astc_bc7.glsl's do,
   since a GPU runs invocations abreast at the pace of the slowest and a block a hint settles is done in a tenth of
   the time of one that is searched for. AstcEncode clears as_more otherwise */
#ifndef SNX_ASTC_STOP
#define SNX_ASTC_STOP 0
#endif
bool as_more;
uint as_hint;
uint as_h0a; /* subset 0's ends */
uint as_h1a;
uint as_h0b; /* subset 1's */
uint as_h1b;
uint as_hplane;
uint as_hpart;
uint as_hib;

/* the best candidate so far: its error; partitions, endpoint mode, colour encoding and weight bits; each subset's
   ends as they decode (RGBA8 words); the weights, a nibble a texel; with two planes (as_dual) alpha's weights apart;
   with two partitions which of ASTC_P2's */
uint as_err;
uint as_np;
uint as_cem;
uint as_enc;
uint as_ib;
uint as_s0a; /* subset 0's ends */
uint as_s1a;
uint as_s0b; /* subset 1's */
uint as_s1b;
uint as_lo;
uint as_hi;
uint as_dual;
uint as_plane;
uint as_lo2;
uint as_hi2;
uint as_part;

/* the fitter's shape for a candidate: ns subsets of partition part, weights of ib bits, ends of colour encoding
   range, alpha fitted or 255, the channels of mask weighed. An end is the 8 bits it decodes to, with no p-bit */
void AsShape(uint ns, uint part, uint ib, uint range, uint alpha, uint mask) {
    SetShape(ns, part);
    sh_cb = 8u;
    sh_pk = 0u;
    sh_ib = ib;
    sh_alpha = alpha;
    sh_mask = mask;
    as_range = range;
    as_key = AsKey(range);
}

/* one end of a line as Try takes it, a channel at a time */
int AsC(uint word, uint c) {
    return int(Ch(word, c));
}

/* the fitted or tried line in fit_ kept as a candidate of one partition when it is the best so far */
void AsKeepOne(uint cem, uint ib, uint range) {
    if (fit_err >= as_err) return;
    as_err = fit_err;
    as_np = 1u;
    as_cem = cem;
    as_enc = range;
    as_ib = ib;
    as_s0a = fit_s0;
    as_s1a = fit_s1;
    as_lo = fit_lo;
    as_hi = fit_hi;
    as_dual = 0u;
}

/* one partition, RGB ends (cem 8) or RGBA (12): kept when it is the best so far */
void AsTryOne(uint cem, uint ib, uint range) {
    AsShape(1u, 0u, ib, range, cem == 12u ? 1u : 0u, 15u);
    FitSubset(0u);
    AsKeepOne(cem, ib, range);
}

/* the least and the greatest of one channel over the 16 texels, the channel at bit sh. In the array-free build each
   texel is read by its number, where PX's selects fold away (a loop of PX reads there is 15 selects a read wherever the
   driver's compiler does not unroll it); with the texels an array the loop stays, a sixth of the code */
uint as_least;
uint as_most;
void AsSpanOf(uint p, uint sh) {
    const uint v = (p >> sh) & 255u;
    as_least = min(as_least, v);
    as_most = max(as_most, v);
}
void AsSpan(uint sh) {
    as_least = 255u;
    as_most = 0u;
#if SNX_NO_ARRAYS
    AsSpanOf(PX(0u), sh);
    AsSpanOf(PX(1u), sh);
    AsSpanOf(PX(2u), sh);
    AsSpanOf(PX(3u), sh);
    AsSpanOf(PX(4u), sh);
    AsSpanOf(PX(5u), sh);
    AsSpanOf(PX(6u), sh);
    AsSpanOf(PX(7u), sh);
    AsSpanOf(PX(8u), sh);
    AsSpanOf(PX(9u), sh);
    AsSpanOf(PX(10u), sh);
    AsSpanOf(PX(11u), sh);
    AsSpanOf(PX(12u), sh);
    AsSpanOf(PX(13u), sh);
    AsSpanOf(PX(14u), sh);
    AsSpanOf(PX(15u), sh);
#else
    for (uint k = 0u; k < 16u; k++) AsSpanOf(PX(k), sh);
#endif
}

/* two planes, channel plane with weights of its own (rgb: of an opaque block, the plane red, green or blue): the
   other channels' line, then its own unless their error already passes the best. With hinted (as_h0 and as_h1 of
   subset 0) the three's line is the source's, tried quickly, and the fourth's runs from its least value in the block to
   its greatest, which is the line a single channel wants. The two lines go through one call of Try and one of
   FitSubset, a pass each, as the compiler inlines each call (astc_encode.glsl has two of each) */
void AsTryDual(uint plane, bool hinted, bool rgb) {
    /* an opaque block's ends are RGB (mode 8), which leaves them 160 values where RGBA ends (mode 12) have 48 */
    const uint own = 0xFFu << (plane * 8u), cem = rgb ? 8u : 12u, range = rgb ? AS_Q5 : AS_T4;
    AsShape(1u, 0u, 2u, range, rgb ? 0u : 1u, 15u ^ (1u << plane));
    uint err = 0u, s0 = 0u, s1 = 0u, lo = 0u, hi = 0u;
    for (uint pass = 0u; pass < 2u; pass++) {
        if (pass == 1u) {
            sh_mask = 1u << plane;
            if (hinted) AsSpan(plane * 8u);
        }
        if (hinted) {
            /* the second pass's ends, every channel its least and its greatest (read only then) */
            const bool own_line = pass == 1u;
            fit_err = 0xFFFFFFFFu;
            Try(0u, 1u, true, own_line ? int(as_least) : AsC(as_h0a, 0u), own_line ? int(as_least) : AsC(as_h0a, 1u),
                own_line ? int(as_least) : AsC(as_h0a, 2u), own_line ? int(as_least) : AsC(as_h0a, 3u),
                own_line ? int(as_most) : AsC(as_h1a, 0u), own_line ? int(as_most) : AsC(as_h1a, 1u),
                own_line ? int(as_most) : AsC(as_h1a, 2u), own_line ? int(as_most) : AsC(as_h1a, 3u));
        } else {
            FitSubset(0u);
        }
        if (pass == 0u) {
            if (fit_err >= as_err) return;
            err = fit_err;
            s0 = fit_s0;
            s1 = fit_s1;
            lo = fit_lo;
            hi = fit_hi;
        }
    }
    if (err + fit_err >= as_err) return;
    as_err = err + fit_err;
    as_np = 1u;
    as_cem = cem;
    as_enc = range;
    as_ib = 2u;
    as_s0a = (s0 & ~own) | (fit_s0 & own);
    as_s1a = (s1 & ~own) | (fit_s1 & own);
    as_lo = lo;
    as_hi = hi;
    as_dual = 1u;
    as_plane = plane;
    as_lo2 = fit_lo;
    as_hi2 = fit_hi;
}

/* two partitions of an opaque block, part of ASTC_P2's: each subset's line (with hinted the source's, tried
   quickly), left once their error passes the best */
void AsTryTwo(uint part, bool hinted) {
    AsShape(2u, part, 2u, AS_Q3, 0u, 15u);
    uint err = 0u, lo = 0u, hi = 0u, s00 = 0u, s01 = 0u;
    for (uint s = 0u; s < 2u; s++) {
        if (hinted) {
            const uint a = s == 0u ? as_h0a : as_h0b, b = s == 0u ? as_h1a : as_h1b;
            fit_err = 0xFFFFFFFFu;
            Try(s, 1u, true, AsC(a, 0u), AsC(a, 1u), AsC(a, 2u), 255, AsC(b, 0u), AsC(b, 1u), AsC(b, 2u), 255);
        } else {
            FitSubset(s);
        }
        err += fit_err;
        if (err >= as_err) return;
        lo |= fit_lo;
        hi |= fit_hi;
        if (s == 0u) {
            s00 = fit_s0;
            s01 = fit_s1;
        }
    }
    as_err = err;
    as_np = 2u;
    as_cem = 8u;
    as_enc = AS_Q3;
    as_ib = 2u;
    as_s0a = s00;
    as_s1a = s01;
    as_s0b = fit_s0;
    as_s1b = fit_s1;
    as_lo = lo;
    as_hi = hi;
    as_dual = 0u;
    as_part = part;
}

/* the colour values of the block, in the order the endpoint modes read them, a byte each of three words (12 at most:
   two partitions of RGB ends), and how many */
uint as_v0;
uint as_v1;
uint as_v2;
uint as_nv;
uint AsV(uint k) {
    return ((k < 4u ? as_v0 : k < 8u ? as_v1 : as_v2) >> ((k & 3u) * 8u)) & 255u;
}
/* a subset's ends as mode 8's or 12's values: red of both ends, then green, blue and (12) alpha. The decoder takes
   the second end for the one whose red, green and blue sum higher, and swaps them (with a blue contraction this
   encoder has no use for) otherwise: 1 when the ends go in the other way round, the subset's weights then turned */
bool AsEnds(uint s) {
    const uint a = s == 0u ? as_s0a : as_s0b, b = s == 0u ? as_s1a : as_s1b;
    const bool turn = Ch(b, 0u) + Ch(b, 1u) + Ch(b, 2u) < Ch(a, 0u) + Ch(a, 1u) + Ch(a, 2u);
    const uint e0 = turn ? b : a, e1 = turn ? a : b;
    /* red of both ends, then green, blue and alpha: 6 or 8 bytes, after the as_nv there are (0 or 6) */
    const uint lo = (e0 & 255u) | ((e1 & 255u) << 8u) | ((e0 & 0xFF00u) << 8u) | ((e1 & 0xFF00u) << 16u);
    const uint hi = ((e0 >> 16u) & 255u) | ((e1 >> 8u) & 0xFF00u) |
                    (as_cem == 12u ? ((e0 >> 8u) & 0xFF0000u) | (e1 & 0xFF000000u) : 0u);
    if (as_nv == 0u) {
        as_v0 = lo;
        as_v1 = hi;
    } else {
        as_v1 |= lo << 16u;
        as_v2 = (lo >> 16u) | (hi << 16u);
    }
    as_nv += as_cem == 12u ? 8u : 6u;
    return turn;
}

/* The integer sequence being written, 64 bits from bit 0 up (the longest, twelve values of 40, is 64 bits): v, whose
   bits end under bit 64, at bit pos. Two words in registers and two selects, where Put's word is an index into the
   block's vector, which a mobile compiler can leave in memory */
uint as_sq_lo;
uint as_sq_hi;
void AsSeqPut(uint pos, uint v) {
    const bool low = pos < 32u;
    const uint s = pos & 31u;
    as_sq_lo |= low ? v << s : 0u;
    as_sq_hi |= low ? (v >> 1u) >> (31u - s) : v << s;
}
/* value k's code under the encoding of key (AsQuantKey's d << b | m), nought past the last value */
uint AsCode(uvec2 key, uint k) {
    return k < as_nv ? AsQuantKey(key, AsV(k)) >> 8u : 0u;
}

/* as_v's values as an integer sequence of the block's colour encoding, from bit pos (17 for one partition, 29 for
   two). With a trit or a quint a value is that and some bits (AsQuant's code of a value the encoding holds); five
   trits share 8 bits and three quints 7, laid between their values' bits as the decoder reads them (astc_decode.glsl's
   DecodeSequence), as runs of fields written at once (astc_direct.glsl's DxEnds has the same three and two). A last
   group of fewer values is cut short: its missing digits are 0, which leaves the cut bits 0. The encoding's constants
   are worked out once a block (AsKey), not once a value */
void AsColours(uint pos) {
    /* 8 bits a value, as they are: only one partition comes to it, so at most 8 values, the two words as_v0 and as_v1 */
    as_sq_lo = as_v0;
    as_sq_hi = as_v1;
    if (as_enc != AS_B8) {
        const uvec2 key = AsKey(as_enc);
        const uint b = 6u - ((as_enc + 1u) >> 1u), low = (1u << b) - 1u;
        as_sq_lo = 0u;
        as_sq_hi = 0u;
        if ((as_enc & 1u) == 0u) {
            for (uint g = 0u, at = 0u; g * 5u < as_nv; g++, at += 5u * b + 8u) {
                const uint k = g * 5u;
                const uint c0 = AsCode(key, k), c1 = AsCode(key, k + 1u), c2 = AsCode(key, k + 2u);
                const uint c3 = AsCode(key, k + 3u), c4 = AsCode(key, k + 4u);
                const uint T = AsTritBits(c0 >> b, c1 >> b, c2 >> b, c3 >> b, c4 >> b);
                AsSeqPut(at, (c0 & low) | ((T & 3u) << b) | ((c1 & low) << (b + 2u)));
                AsSeqPut(at + 2u * b + 2u, ((T >> 2u) & 3u) | ((c2 & low) << 2u) | (((T >> 4u) & 1u) << (b + 2u)));
                AsSeqPut(at + 3u * b + 5u, (c3 & low) | (((T >> 5u) & 3u) << b) | ((c4 & low) << (b + 2u)) |
                                               ((T >> 7u) << (2u * b + 2u)));
            }
        } else {
            for (uint g = 0u, at = 0u; g * 3u < as_nv; g++, at += 3u * b + 7u) {
                const uint k = g * 3u;
                const uint c0 = AsCode(key, k), c1 = AsCode(key, k + 1u), c2 = AsCode(key, k + 2u);
                const uint Q = AsQuintBits(c0 >> b, c1 >> b, c2 >> b);
                AsSeqPut(at, (c0 & low) | ((Q & 7u) << b) | ((c1 & low) << (b + 3u)));
                AsSeqPut(at + 2u * b + 3u, ((Q >> 3u) & 3u) | ((c2 & low) << 2u) | ((Q >> 5u) << (b + 2u)));
            }
        }
    }
    /* pos is 17 or 29: the sequence's 64 bits land in the block's first three words */
    enc.x |= as_sq_lo << pos;
    enc.y |= (as_sq_lo >> (32u - pos)) | (as_sq_hi << pos);
    enc.z |= as_sq_hi >> (32u - pos);
}

/* Weights a nibble each (texel k's at bit 4k, each under 2^ib) as the block's fields of ib bits from bit 0 up: 2 bits
   of each nibble into 16, and 3 of each into 24, a halving of the gaps a step */
uint AsPack2(uint v) {
    v = (v | (v >> 2u)) & 0x0F0F0F0Fu;
    v = (v | (v >> 4u)) & 0x00FF00FFu;
    return (v | (v >> 8u)) & 0xFFFFu;
}
uint AsPack3(uint v) {
    v = (v & 0x07070707u) | ((v >> 1u) & 0x38383838u);
    v = (v & 0x003F003Fu) | ((v >> 2u) & 0x0FC00FC0u);
    return (v & 0xFFFu) | ((v >> 4u) & 0xFFF000u);
}
/* texel k's bit 2k of v (k < 8, the partition's subsets as SetShape spreads them) as all of nibble k: bit i to bit 2i,
   then 15 times it, the nibbles apart so nothing carries */
uint AsNibbles(uint v) {
    v = (v | (v << 8u)) & 0x00FF00FFu;
    v = (v | (v << 4u)) & 0x0F0F0F0Fu;
    v = (v | (v << 2u)) & 0x33333333u;
    v = (v | (v << 1u)) & 0x55555555u;
    return (v << 4u) - v;
}

/* the block the best candidate is */
uvec4 AsPack() {
    enc = uvec4(0u);
    as_nv = 0u;
    as_v0 = 0u;
    as_v1 = 0u;
    as_v2 = 0u;
    /* the block mode (a 4x4 grid of weights, of 2, 3 or 4 bits, the last with the mode's precision bit; two planes)
       and the partitions from bit 0, then one partition's endpoint mode from bit 13, or two's seed from 13 and their
       endpoint mode from 23: all in the first word */
    uint head = (as_ib == 2u ? 0x042u : as_ib == 3u ? 0x053u : 0x242u) | (as_dual << 10u) | ((as_np - 1u) << 11u);
    uint turned = 0u, pos = 17u, subs_lo = 0u, subs_hi = 0u;
    if (as_np == 1u) {
        head |= as_cem << 13u;
        turned = AsEnds(0u) ? 1u : 0u;
    } else {
        /* the partition's seed, then the endpoint mode of both; ASTC's partition 0 first, which is subset 1 where the
           table says so */
        const uint seed = AS_SEED2(as_part), flip = seed >> 10u;
        head |= ((seed & 1023u) << 13u) | (as_cem << 25u);
        pos = 29u;
        SetShape(2u, as_part);
        const bool t_first = AsEnds(flip), t_second = AsEnds(1u - flip);
        turned = flip != 0u ? (t_first ? 2u : 0u) | (t_second ? 1u : 0u) : (t_first ? 1u : 0u) | (t_second ? 2u : 0u);
        /* which texels are subset 1's, a whole nibble each */
        subs_lo = AsNibbles(sh_pbits & 0xFFFFu);
        subs_hi = AsNibbles(sh_pbits >> 16u);
    }
    enc.x = head;
    AsColours(pos);
    /* the weights, texel after texel (the two planes' side by side), a turned subset's each top less itself, which is
       itself xor top, a nibble mask; and with two planes the channel that has the second in the two bits under them */
    const uint top4 = as_ib == 2u ? 0x33333333u : as_ib == 3u ? 0x77777777u : 0xFFFFFFFFu;
    const uint keep0 = (turned & 1u) != 0u ? 0xFFFFFFFFu : 0u, keep1 = (turned & 2u) != 0u ? 0xFFFFFFFFu : 0u;
    const uint flip_lo = ((subs_lo & keep1) | (~subs_lo & keep0)) & top4;
    const uint flip_hi = ((subs_hi & keep1) | (~subs_hi & keep0)) & top4;
    const uint lo = as_lo ^ flip_lo, hi = as_hi ^ flip_hi;
    uint w0 = lo, w1 = hi;
    if (as_dual != 0u) {
        w0 = lo | ((as_lo2 ^ flip_lo) << 2u);
        w1 = hi | ((as_hi2 ^ flip_hi) << 2u);
        enc.y |= as_plane << 30u;
    } else if (as_ib == 3u) {
        const uint a = AsPack3(lo), c = AsPack3(hi);
        w0 = a | (c << 24u);
        w1 = c >> 8u;
    } else if (as_ib == 2u) {
        w0 = AsPack2(lo) | (AsPack2(hi) << 16u);
        w1 = 0u;
    }
    /* 64 bits of weights at most, from the block's top bit down */
    enc.z |= bitfieldReverse(w1);
    enc.w |= bitfieldReverse(w0);
    return enc;
}

/* whether the block's 16 texels are all opaque and all one colour: an and of every texel (its alpha 255 where each
   is) and an or of each one's difference from the first, every texel read by its number in the array-free build */
bool as_opaque;
bool as_solid;
void AsLook() {
#if SNX_NO_ARRAYS
    const uint p0 = PX(0u);
    uint all = p0, diff = 0u;
    uint p = PX(1u);
    all &= p;
    diff |= p ^ p0;
    p = PX(2u);
    all &= p;
    diff |= p ^ p0;
    p = PX(3u);
    all &= p;
    diff |= p ^ p0;
    p = PX(4u);
    all &= p;
    diff |= p ^ p0;
    p = PX(5u);
    all &= p;
    diff |= p ^ p0;
    p = PX(6u);
    all &= p;
    diff |= p ^ p0;
    p = PX(7u);
    all &= p;
    diff |= p ^ p0;
    p = PX(8u);
    all &= p;
    diff |= p ^ p0;
    p = PX(9u);
    all &= p;
    diff |= p ^ p0;
    p = PX(10u);
    all &= p;
    diff |= p ^ p0;
    p = PX(11u);
    all &= p;
    diff |= p ^ p0;
    p = PX(12u);
    all &= p;
    diff |= p ^ p0;
    p = PX(13u);
    all &= p;
    diff |= p ^ p0;
    p = PX(14u);
    all &= p;
    diff |= p ^ p0;
    p = PX(15u);
    all &= p;
    diff |= p ^ p0;
#else
    const uint p0 = PX(0u);
    uint all = p0, diff = 0u;
    for (uint k = 1u; k < 16u; k++) {
        all &= PX(k);
        diff |= PX(k) ^ p0;
    }
#endif
    as_opaque = (all >> 24u) == 255u;
    as_solid = diff == 0u;
}

/* candidate step of AstcEncode's, kept in as_ when it is the best so far: 0 the source's lines as they are (hinted),
   1 to 4 the search's, one partition in each layout the block's alpha allows, then two planes, or two partitions for
   an opaque block. The same candidates in the same order as astc_encode.glsl's, so the same block */
void AsCandidate(uint step, bool all_opaque, bool hinted) {
    const uint j = step - 1u;
    if (step == 0u ? as_hint == 3u && all_opaque : j == 3u && all_opaque) {
        /* two partitions: the source's own partition where it had two subsets, tried as it is and then fitted,
           and in the search none where it was one line or two planes, which a partition has nothing to add to;
           else the one of the 64 whose subsets spread least */
        if (step != 0u && hinted && as_hint != 3u) return;
        uint part = as_hpart;
        if (!hinted) {
            BlockSums();
            uint cost = 0xFFFFFFFFu;
            for (uint p = 0u; p < 64u; p++) {
                const uint c = Spread(2u, p, 0u);
                if (c < cost) {
                    part = p;
                    cost = c;
                }
            }
        }
        AsTryTwo(part, step == 0u);
    } else if (step == 0u ? as_hint == 2u && !(all_opaque && as_hplane == 3u) : j == 3u) {
        /* two planes: the source's, its rotated channel apart; in the search alpha apart */
        AsTryDual(step == 0u ? as_hplane : 3u, step == 0u, step == 0u && all_opaque);
    } else if (step == 0u) {
        if (as_hint == 3u) return; /* two partitions with alpha: no layout here */
        /* one line: an opaque block's as RGB ends in the layout with the source's weights (2 or 3 bits with 8 bit
           ends, which hold BC7's and BC1's ends as they are, or 4 bits with ends of 192 values); any other's as
           RGBA ends with 3 bit weights and ends of 192 values, then with 4 bit weights and ends of 48, the better
           of the two kept (one call of Try for both) */
        for (uint v = 0u; v < (all_opaque ? 1u : 2u); v++) {
            const uint ib = all_opaque ? as_hib : 3u + v;
            const uint range = all_opaque ? (as_hib < 4u ? AS_B8 : AS_T6) : v == 0u ? AS_T6 : AS_T4;
            AsShape(1u, 0u, ib, range, all_opaque ? 0u : 1u, 15u);
            fit_err = 0xFFFFFFFFu;
            Try(0u, 1u, true, AsC(as_h0a, 0u), AsC(as_h0a, 1u), AsC(as_h0a, 2u), AsC(as_h0a, 3u),
                AsC(as_h1a, 0u), AsC(as_h1a, 1u), AsC(as_h1a, 2u), AsC(as_h1a, 3u));
            AsKeepOne(all_opaque ? 8u : 12u, ib, range);
        }
    } else {
        if (all_opaque && j == 2u) return;
        AsTryOne(all_opaque ? 8u : 12u, all_opaque ? (j == 0u ? 4u : 3u) : j == 0u ? 3u : j == 1u ? 4u : 2u,
                 j == 0u ? AS_T6 : all_opaque || j == 2u ? AS_B8 : AS_T4);
    }
}

uvec4 AstcEncode() {
    AsLook();
    const bool all_opaque = as_opaque;
    as_err = 0xFFFFFFFFu;
    as_more = false;
    if (as_solid) {
        /* a void extent with no extent (its coordinates all ones): the colour, each channel 16 bits, a byte c as
           c * 257 = c | c << 8, two channels a word with no multiply */
        const uint c = PX(0u);
        const uint rg = (c & 0xFFu) | ((c << 8u) & 0xFF0000u), ba = ((c >> 16u) & 0xFFu) | ((c >> 8u) & 0xFF0000u);
        as_err = 0u;
        return uvec4(0xFFFFFDFCu, 0xFFFFFFFFu, rg | (rg << 8u), ba | (ba << 8u));
    }
    /* The candidates in turn (AsCandidate), each through the one call there is of the function that tries it, as the
       compiler inlines every call: astc_encode.glsl tries the source's lines in calls of their own beside the
       search's, which makes its third pass 326 KB of SPIR-V where this one is 255 (a compile on a phone's GPU). Step
       0, what the source says, is kept at once when the texels lie near enough; the second pass stops after it
       (SNX_ASTC_STOP), its call of AsCandidate the step a constant. Then the search, until one is good enough */
    const bool hinted = SNX_ASTC_HINTS != 0 && as_hint != 0u;
#if SNX_ASTC_STOP
    if (hinted) AsCandidate(0u, all_opaque, true);
    if (!hinted || as_err > uint(SNX_ASTC_HINT_ENOUGH)) { /* searched for in the next pass */
        as_more = true;
        return uvec4(0u);
    }
#else
    for (uint step = hinted ? 0u : 1u; step < 5u; step++) {
        if (step == 1u && hinted && as_err <= uint(SNX_ASTC_HINT_ENOUGH)) break;
        if (step >= 1u && as_err <= uint(SNX_ASTC_GOOD)) break;
        AsCandidate(step, all_opaque, hinted);
    }
#endif
    return AsPack();
}
