// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BCn to ASTC transcoder (texconv.glsl's ASTC build, astc_encode.glsl) run on the CPU through tests/glsl/glsl.hpp,
// its blocks decoded by the runtime's ASTC decoder (src/nx/gpu/texture/astc_cpu.c) and the BCn ones by
// tests/support/bcn.c. Its floats are run 32 bits wide and with every operation rounded to 16 (a GPU's relaxed
// precision is either), and every block below has to come out the same bytes both ways. Held:
// - the halves round as IEEE binary16 does;
// - each level it stores for a value is the range's nearest, and decodes to what the encoder reckons with (every
//   value of each range, against the specification's formula, and through the decoder);
// - a block of one colour comes back exact; texels on a line between two levels, a line with alpha, a channel with a
//   line of its own and two partitions' lines each store the kind of block that holds them, and most come back as
//   they were;
// - the partition tables: the texels a table gives a seed are the ones the decoder draws for it, 30 of BC7's two
//   subset shapes are an ASTC seed's and the rest as near as the tables say;
// - random BC2, BC3 and BC7 blocks (every mode, partition and rotation) decode near what they held, none to the
//   decoder's error colour, and a BC7 block stored the source's way has the source's own plane or partition;
// - which textures are made ASTC (bcn_astc.h): a block said to have one alpha has one by the reference decoder, in
//   every kind and mode, the encoder keeps that alpha exact, and the picture's see-through region is no such texture;
// - images whose sides aren't multiples of 4, of several slices, transcode block for block as one block alone does;
// - the picture of bcn_picture.hpp, made BC2, BC3 and BC7, comes back no worse than the PSNR each is held to (against
//   the BCn blocks decoded, which is what the GPU showed before). No GPU is needed.
//
// snx-test-bcn-astc-glsl --image FILE W H measures a picture of one's own (W x H RGBA8 bytes) the same way, beside the
// ETC2 transcoder's: each kind's PSNR against the BCn decoded, and, with --dump DIR, the decoded pictures as raw
// RGBA8 files (KIND.bcn.raw, KIND.astc.raw, KIND.etc2.raw) and the blocks themselves, 16 bytes each (KIND.bcn.blocks,
// KIND.astc.blocks, KIND.etc2.blocks), which a run of the shader on a device is compared with.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "bcn_astc_cpu.h"
#include "bcn_etc2_cpu.h"
#include "bcn_picture.hpp"
#include "glsl.hpp"
#include "nx/gpu/texture/bcn_astc.h"
// astc_cpu.h's and tests/support/etc2.h's (their headers bring the recompiler's C helpers, which C++ warns about)
extern "C" void astc_decode_image(const uint8_t* src, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint8_t* dst,
                                  int srgb);
extern "C" void etc2_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            if (failures++ < 20) {                                                                                     \
                std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                       \
                std::printf(__VA_ARGS__);                                                                              \
                std::printf("\n");                                                                                     \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

// ---- the encoder, both widths

// sixteen texels as a block, with what the source says (astc_block_cpu): the bytes have to be the same with 32 bit
// floats and with halves
static void encode(const uint32_t px[16], uint32_t part, uint32_t seed, uint32_t own, uint8_t out[16]) {
    uint8_t narrow[16];
    astc_block_cpu(px, part, seed, own, 0, out);
    astc_block_cpu(px, part, seed, own, 1, narrow);
    CHECK(!std::memcmp(out, narrow, 16), "a block differs between 32 bit floats and halves (texel 0 %08x, part %04x)",
          px[0], part);
}
// the encoder's block of one kind (astc_candidate_cpu) and how far it reckons it from the texels, the same both widths
static uint32_t candidate(const uint32_t px[16], uint32_t kind, uint32_t part, uint32_t seed, uint32_t own,
                          uint8_t out[16]) {
    uint8_t narrow[16];
    uint32_t err = astc_candidate_cpu(px, kind, part, seed, own, 0, out);
    uint32_t err_narrow = astc_candidate_cpu(px, kind, part, seed, own, 1, narrow);
    CHECK(
        !std::memcmp(out, narrow, 16) && err == err_narrow,
        "a block of kind %u differs between 32 bit floats and halves (texel 0 %08x, part %04x; reckoned %u and %u off)",
        kind, px[0], part, err, err_narrow);
    return err;
}
// BCn blocks as ASTC ones, both widths the same bytes
static void transcode(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out) {
    std::vector<uint8_t> narrow((size_t)bw * bh * slices * 16);
    bcn_astc_cpu(kind, blocks, bw, bh, slices, 0, out);
    bcn_astc_cpu(kind, blocks, bw, bh, slices, 1, narrow.data());
    size_t at = 0;
    while (at < narrow.size() && out[at] == narrow[at]) at++;
    CHECK(at == narrow.size(), "%s block %zu of %ux%ux%u differs between 32 bit floats and halves", kind_name(kind),
          at / 16, bw, bh, slices);
}
static void decode(const uint8_t block[16], uint32_t px[16]) {
    uint8_t dec[64];
    astc_decode_image(block, 4, 4, 4, 4, dec, 0);
    for (int i = 0; i < 16; i++) px[i] = rgba(dec[4 * i], dec[4 * i + 1], dec[4 * i + 2], dec[4 * i + 3]);
}
// bits lo to hi of a block as stored
static uint32_t bits(const uint8_t b[16], int lo, int hi) {
    uint32_t v = 0;
    for (int i = hi; i >= lo; i--) v = v << 1 | (b[i / 8] >> (i % 8) & 1);
    return v;
}
// what a block stores, read from its header: the kinds of astc_encode.glsl, then a void extent block
enum { KIND_RGB, KIND_RGBA, KIND_DUAL, KIND_TWO, KIND_RGBA2, KIND_VOID, KIND_OTHER };
static int stored_kind(const uint8_t b[16]) {
    if (bits(b, 0, 8) == 0x1FC) return KIND_VOID;
    uint32_t mode = bits(b, 0, 10), parts = bits(b, 11, 12) + 1, cem = parts == 1 ? bits(b, 13, 16) : bits(b, 25, 28);
    if (parts == 1 && mode == 0x242 && cem == 8) return KIND_RGB;
    if (parts == 1 && mode == 0x53 && cem == 12) return KIND_RGBA;
    if (parts == 1 && mode == 0x442 && cem == 12) return KIND_DUAL;
    if (parts == 1 && mode == 0x42 && cem == 12) return KIND_RGBA2;
    if (parts == 2 && mode == 0x42 && cem == 8 && bits(b, 23, 24) == 0) return KIND_TWO;
    return KIND_OTHER;
}

// ---- the halves the shader's floats are run as

// glsl.hpp's half rounds as IEEE binary16 does, to nearest and ties to even: what the equal bytes of the two runs
// rest on
static void halves() {
    static const struct {
        double in, out;
    } cases[] = {{2047, 2047},
                 {2049, 2048},
                 {2051, 2052},
                 {2050, 2050},
                 {4098, 4096},
                 {4102, 4104},
                 {0.1, 0.0999755859375},
                 {65519, 65504},
                 {1.0 / 3, 0.333251953125},
                 {129.875, 129.875},
                 {-2049, -2048},
                 {5.9604644775390625e-8, 5.9604644775390625e-8},
                 {2.98e-8, 0}};
    for (const auto& c : cases) {
        double got = double(glsl::half(c.in));
        CHECK(got == c.out, "half(%.17g) is %.17g, not %.17g", c.in, got, c.out);
    }
    CHECK(std::isinf(double(glsl::half(65520.0))), "half(65520) is %g, not inf", double(glsl::half(65520.0)));
    // an operation rounds once: 3 * (1/3 as a half) is 4095/4096, a tie that goes to 1; and 2047 + 2 is 2049, to 2048
    glsl::half third(1.0 / 3), three(3.0), sum = glsl::half(2047.0) + glsl::half(2.0);
    CHECK(double(three * third) == 1.0 && double(sum) == 2048.0, "3 * half(1/3) is %.17g and 2047 + 2 is %.17g",
          double(three * third), double(sum));
}

// ---- the levels

// the value a symbol of a range of levels decodes to, by the specification's unquantization (its table of bit
// patterns for a trit or a quint above n bits)
static uint32_t spec_value(uint32_t levels, uint32_t bits_, uint32_t digit) {
    uint32_t n = levels == 192 ? 6 : levels == 48 ? 4 : 3, m = bits_, a = m & 1, A = a ? 0x1FF : 0;
    uint32_t b = m >> 1 & 1, c = m >> 2 & 1, d = m >> 3 & 1, e = m >> 4 & 1, f = m >> 5 & 1, B, C;
    if (levels == 192)
        B = f << 8 | e << 7 | d << 6 | c << 5 | b << 4 | f, C = 5;
    else if (levels == 48)
        B = d << 8 | c << 7 | b << 6 | d << 2 | c << 1 | b, C = 22;
    else
        B = c << 8 | b << 7 | c << 2 | b << 1 | c, C = 26;
    (void)n;
    uint32_t t = (digit * C + B) ^ A;
    return (A & 0x80) | t >> 2;
}
static void levels() {
    for (uint32_t range : {192u, 48u, 40u}) {
        uint32_t nbits = range == 192 ? 6 : range == 48 ? 4 : 3, digits = range == 40 ? 5 : 3;
        std::vector<uint32_t> all;
        for (uint32_t m = 0; m < 1u << nbits; m++)
            for (uint32_t d = 0; d < digits; d++) all.push_back(spec_value(range, m, d));
        CHECK(all.size() == range, "a range of %u levels has %zu symbols", range, all.size());
        uint32_t worst = 0;
        for (uint32_t v = 0; v < 256; v++) {
            uint32_t m[2], d[2], u[2];
            for (int half = 0; half < 2; half++) astc_level_cpu(v, range, half, &m[half], &d[half], &u[half]);
            CHECK(m[0] == m[1] && d[0] == d[1] && u[0] == u[1],
                  "%u of %u levels: bits %u, digit %u, value %u with 32 bit floats, %u, %u, %u with halves", v, range,
                  m[0], d[0], u[0], m[1], d[1], u[1]);
            CHECK(m[0] < 1u << nbits && d[0] < digits, "%u of %u levels: bits %u, digit %u", v, range, m[0], d[0]);
            CHECK(spec_value(range, m[0], d[0]) == u[0],
                  "%u of %u levels: the symbol (bits %u, digit %u) decodes to %u, "
                  "not the %u the encoder reckons with",
                  v, range, m[0], d[0], spec_value(range, m[0], d[0]), u[0]);
            uint32_t best = 255, off = u[0] > v ? u[0] - v : v - u[0];
            for (uint32_t x : all) best = std::min(best, x > v ? x - v : v - x);
            CHECK(off == best, "%u of %u levels: stored as %u, %u away, where a level is %u away", v, range, u[0], off,
                  best);
            worst = std::max(worst, off);
        }
        // the steps: 192 levels are 1 or 2 apart, 48 are 5 to 7, 40 are 6 to 9
        CHECK(worst == (range == 192  ? 1u
                        : range == 48 ? 3u
                                      : 4u),
              "the farthest value from a level of %u is %u away", range, worst);
    }
}
// through the decoder: two greys a block, built as each kind of block, decode to the levels the encoder reckons with
static void levels_decoded() {
    static const struct {
        uint32_t range, part, seed, own;
        int kind;
    } cases[] = {
        {192, 0, 0, 4, KIND_RGB}, {192, 0, 0, 4, KIND_RGBA}, {48, 0, 0, 3, KIND_DUAL}, {40, 0, 0, 4, KIND_TWO}};
    for (const auto& k : cases) {
        uint32_t t = astc_partition_cpu(2, 13); // the top half and the bottom half
        uint32_t part = k.kind == KIND_TWO ? t >> 10 : 0, seed = k.kind == KIND_TWO ? t & 1023 : 0;
        for (uint32_t v = 0; v < 256; v++) {
            // the second grey far enough for the two to be two levels
            uint32_t w = v < 128 ? v + 100 : v - 100, px[16], dec[16], m, d, uv, uw;
            astc_level_cpu(v, k.range, 0, &m, &d, &uv);
            astc_level_cpu(w, k.range, 0, &m, &d, &uw);
            for (int i = 0; i < 16; i++) {
                bool second = k.kind == KIND_TWO ? (part >> i & 1) != 0 : i >= 8;
                uint32_t g = second ? w : v, a = k.kind == KIND_RGB || k.kind == KIND_TWO ? 255 : g;
                // two planes: the colours one way, alpha the other
                px[i] = k.kind == KIND_DUAL ? rgba(g, g, g, i % 2 ? v : w) : rgba(g, g, g, a);
            }
            uint8_t block[16];
            candidate(px, (uint32_t)k.kind, part, seed, k.own, block);
            CHECK(stored_kind(block) == k.kind, "two greys %u and %u: a block of kind %d, not %d", v, w,
                  stored_kind(block), k.kind);
            decode(block, dec);
            for (int i = 0; i < 16; i++) {
                bool second = k.kind == KIND_TWO ? (part >> i & 1) != 0 : i >= 8;
                uint32_t want = second ? uw : uv;
                CHECK(ch(dec[i], 0) == want && ch(dec[i], 1) == want && ch(dec[i], 2) == want,
                      "two greys %u and %u in a block of kind %d: texel %d decodes to %08x, not the level %u", v, w,
                      k.kind, i, dec[i], want);
                if (k.kind == KIND_DUAL)
                    CHECK(ch(dec[i], 3) == (i % 2 ? uv : uw), "alpha %u and %u in two planes: texel %d decodes to %u",
                          v, w, i, ch(dec[i], 3));
            }
        }
    }
}

// ---- blocks that come back as they were

// the decoder's weight of index k of top + 1 levels (the specification's unquantization: 2, 3 and 4 bit weights)
static uint32_t weight(uint32_t top, uint32_t k) {
    uint32_t w = top == 3 ? k * 21 : top == 7 ? k * 9 : k * 4 + (k >> 2);
    return w > 32 ? w + 1 : w;
}
// the decoder's texel between two ends (8 bit levels) at a weight: 16 bits interpolated, to the nearest of 8
static uint32_t between(uint32_t e0, uint32_t e1, uint32_t w) {
    return (((e0 * 257 * (64 - w) + e1 * 257 * w + 32) >> 6) * 255 + 32767) / 65535;
}
static void exact_blocks() {
    // one colour: a void extent block, exact
    for (int n = 0; n < 4096; n++) {
        uint32_t px[16], dec[16], c = random_word();
        if (n % 3 == 0) c |= 0xFF000000u;
        for (auto& p : px) p = c;
        uint8_t block[16];
        encode(px, 0, 0, n % 5, block);
        decode(block, dec);
        bool same = true;
        for (int i = 0; i < 16; i++) same = same && dec[i] == c;
        CHECK(stored_kind(block) == KIND_VOID && same, "a block of %08x comes back %08x, kind %d", c, dec[0],
              stored_kind(block));
    }
    // texels the decoder makes of two levels and sixteen weights: the block that holds them, and most as they were
    // (a weight a texel's rounding moved, or two ends too near to tell the weights apart, is the rest)
    static const struct {
        int kind;
        uint32_t range, top, own, at_least;
        const char* name;
    } lines[] = {{KIND_RGB, 192, 15, 4, 950, "a line of colours"},
                 {KIND_RGBA, 192, 7, 4, 950, "a line of colours and alpha"},
                 {KIND_DUAL, 48, 3, 3, 950, "a line of colours and alpha's own"},
                 {KIND_DUAL, 48, 3, 1, 950, "a line and green's own"},
                 {KIND_TWO, 40, 3, 4, 950, "two partitions' lines"},
                 {KIND_RGBA2, 256, 3, 3, 950, "a line of colours under one alpha, kept apart"}};
    for (const auto& l : lines) {
        uint32_t exact = 0;
        for (int n = 0; n < 1000; n++) {
            uint32_t t = astc_partition_cpu(2 + n % 2, random_word() % 64);
            uint32_t part = l.kind == KIND_TWO ? t >> 10 : 0, seed = l.kind == KIND_TWO ? t & 1023 : 0;
            // each subset's two ends, of levels, far apart in one channel at least
            uint32_t ends[2][2][4];
            for (int s = 0; s < 2; s++)
                for (int c = 0; c < 4; c++) {
                    uint32_t m, d, lo = random_word() % 100, hi = 155 + random_word() % 100;
                    if (c != 0 && random_word() % 2) std::swap(lo, hi); // other channels fall as red rises
                    astc_level_cpu(lo, l.range, 0, &m, &d, &ends[s][0][c]);
                    astc_level_cpu(hi, l.range, 0, &m, &d, &ends[s][1][c]);
                    if (c == 3 && (l.kind == KIND_RGB || l.kind == KIND_TWO)) ends[s][0][c] = ends[s][1][c] = 255;
                    if (c == 3 && l.kind == KIND_RGBA2) ends[s][0][c] = ends[s][1][c] = 1 + (uint32_t)n % 254;
                }
            uint32_t px[16], dec[16], met[2] = {0, 0};
            for (int i = 0; i < 16; i++) {
                int s = part >> i & 1;
                // every weight in turn, a subset's first two texels its two ends (a subset of one texel is one end)
                uint32_t k = (uint32_t)(i * 7 + n) % (l.top + 1), k2 = (uint32_t)(i * 5 + n / 3) % (l.top + 1), c[4];
                if (met[s] < 2) k = met[s] ? l.top : 0, k2 = met[s] ? 0 : l.top;
                met[s]++;
                for (uint32_t j = 0; j < 4; j++)
                    c[j] = between(ends[s][0][j], ends[s][1][j],
                                   weight(l.top, l.kind == KIND_DUAL && j == l.own ? k2 : k));
                px[i] = rgba(c[0], c[1], c[2], c[3]);
            }
            uint8_t block[16];
            encode(px, part, seed, l.own, block);
            CHECK(stored_kind(block) == l.kind, "%s: a block of kind %d, not %d", l.name, stored_kind(block), l.kind);
            if (l.kind == KIND_DUAL)
                CHECK(bits(block, 62, 63) == l.own, "%s: the second plane is channel %u", l.name, bits(block, 62, 63));
            if (l.kind == KIND_TWO)
                CHECK(bits(block, 13, 22) == seed, "%s: seed %u, not %u", l.name, bits(block, 13, 22), seed);
            decode(block, dec);
            bool same = true;
            for (int i = 0; i < 16; i++) same = same && dec[i] == px[i];
            exact += same;
        }
        std::printf("%s %s: %u of 1000 blocks come back as they were (at least %u)\n",
                    exact >= l.at_least ? "PASS" : "FAIL", l.name, exact, l.at_least);
        if (exact < l.at_least) failures++;
    }
}

// ---- the partition tables

// the texels of BC7's second (and third) subset of a partition, read off the reference decoder: a block of mode 3
// (two subsets, 7 bit colours) or 2 (three, 5 bit colours) whose subsets are black, white and (the third) black
static void bc7_shape(uint32_t subsets, uint32_t partition, uint32_t sub[16]) {
    uint8_t b[16] = {0}, dec[64];
    auto put = [&](int at, int n, uint32_t v) {
        for (int i = 0; i < n; i++)
            if (v >> i & 1) b[(at + i) / 8] |= (uint8_t)(1 << ((at + i) % 8));
    };
    if (subsets == 2) {
        put(3, 1, 1), put(4, 6, partition);
        // R, G and B of ends 0 to 3, 7 bits each: the second subset's (2 and 3) all ones
        for (int c = 0; c < 3; c++) put(10 + c * 28 + 14, 14, 0x3FFF);
        put(94 + 2, 2, 3); // its p-bits
        bcn_decode_image(b, 4, 4, BC7, dec);
        for (int i = 0; i < 16; i++) sub[i] = dec[4 * i] > 127;
    } else {
        // three blocks: each subset white in turn
        for (int i = 0; i < 16; i++) sub[i] = 0;
        for (uint32_t s = 1; s < 3; s++) {
            std::memset(b, 0, 16);
            put(2, 1, 1), put(3, 6, partition);
            for (int c = 0; c < 3; c++) put(9 + c * 30 + (int)s * 10, 10, 0x3FF);
            bcn_decode_image(b, 4, 4, BC7, dec);
            for (int i = 0; i < 16; i++)
                if (dec[4 * i] > 127) sub[i] = s;
        }
    }
}
static void partitions() {
    uint32_t exact = 0, off2 = 0, off3 = 0;
    for (uint32_t subsets = 2; subsets <= 3; subsets++)
        for (uint32_t p = 0; p < 64; p++) {
            uint32_t t = astc_partition_cpu(subsets, p), part = t >> 10, seed = t & 1023, px[16], dec[16];
            CHECK(part != 0 && part != 0xFFFF && t >> 26 == 0, "partition %u of %u subsets: texels %04x", p, subsets,
                  part);
            // the table's texels are the seed's: a colour a partition, levels both, built as two partitions, comes
            // back in its place
            for (int i = 0; i < 16; i++) px[i] = part >> i & 1 ? rgba(255, 0, 0, 255) : rgba(0, 0, 255, 255);
            uint8_t block[16];
            candidate(px, KIND_TWO, part, seed, 4, block);
            decode(block, dec);
            bool same = stored_kind(block) == KIND_TWO;
            for (int i = 0; i < 16; i++) same = same && dec[i] == px[i];
            CHECK(same, "partition %u of %u subsets: seed %u doesn't draw the texels %04x the table gives it", p,
                  subsets, seed, part);
            // and near BC7's: its second subset, or one of three alone, either way round
            uint32_t sub[16], best = 16;
            bc7_shape(subsets, p, sub);
            for (uint32_t alone = subsets == 2 ? 1 : 0; alone < subsets; alone++) {
                uint32_t off = 0;
                for (int i = 0; i < 16; i++) off += (sub[i] == alone) != ((part >> i & 1) != 0);
                best = std::min(best, std::min(off, 16 - off));
            }
            if (subsets == 2)
                exact += best == 0, off2 += best;
            else
                off3 += best;
        }
    CHECK(
        exact == 30 && off2 == 68 && off3 == 17,
        "the tables: %u of BC7's two subset shapes are a seed's (30), the others %u texels off in all (68), the three "
        "subset ones %u (17)",
        exact, off2, off3);
}

// ---- random blocks

// random BCn blocks: every one decodes to something near what it held, none to the decoder's error colour, and the
// kinds of block the encoder stores are all met
static void random_blocks() {
    const uint32_t n = 12000;
    for (int kind : {BC2, BC3, BC7}) {
        // BC7's subsets of random colours are what two partitions hold worst: three subsets in two
        std::vector<uint8_t> blocks((size_t)n * 16), astc((size_t)n * 16), a((size_t)n * 64), b((size_t)n * 64);
        for (auto& x : blocks) x = (uint8_t)random_word();
        transcode(kind, blocks.data(), n, 1, 1, astc.data());
        bcn_decode_image(blocks.data(), n * 4, 4, kind, a.data());
        astc_decode_image(astc.data(), n * 4, 4, 4, 4, b.data(), 0);
        uint32_t seen[7] = {0, 0, 0, 0, 0, 0, 0}, error = 0;
        double sum = 0;
        for (uint32_t i = 0; i < n; i++) {
            int k = stored_kind(&astc[(size_t)i * 16]);
            seen[k]++;
            for (uint32_t t = 0; t < 16; t++) {
                // texel t of block i: the blocks are side by side
                size_t at = ((size_t)(t / 4) * n * 4 + (size_t)i * 4 + t % 4) * 4;
                if (b[at] == 255 && b[at + 1] == 0 && b[at + 2] == 255 && b[at + 3] == 255 &&
                    !(a[at] > 200 && a[at + 1] < 60 && a[at + 2] > 200))
                    error++;
                for (int c = 0; c < 4; c++) sum += double(a[at + c] - b[at + c]) * double(a[at + c] - b[at + c]);
            }
        }
        double db = 10.0 * std::log10(255.0 * 255.0 / (sum / (double(n) * 64)));
        // random blocks' texels are as far apart as texels get: what holds them is that none is lost
        double floor = kind == BC2 ? 26.8 : kind == BC3 ? 29.6 : 21.2;
        bool kinds = seen[KIND_OTHER] == 0 && seen[KIND_DUAL] > 0 &&
                     (kind != BC7 || (seen[KIND_RGB] > 0 && seen[KIND_RGBA] > 0 && seen[KIND_TWO] > 0));
        bool pass = error == 0 && kinds && db >= floor;
        std::printf(
            "%s %u random %s blocks: %.2f dB (at least %.1f); stored as RGB %u, RGBA %u, RGBA of 2 bit weights %u, "
            "two planes %u, two partitions %u, one colour %u; %u texels the error colour\n",
            pass ? "PASS" : "FAIL", n, kind_name(kind), db, floor, seen[KIND_RGB], seen[KIND_RGBA], seen[KIND_RGBA2],
            seen[KIND_DUAL], seen[KIND_TWO], seen[KIND_VOID], error);
        if (!pass) failures++;
    }
}

// A block that keeps alpha apart, under one alpha that isn't 255: its colours are held as an opaque block's are and
// its alpha exact (ends of 256 levels, 2 bit weights). Random BC3 colour blocks under one alpha, transparent, half
// and nearly opaque, against the same blocks opaque
static void one_alpha_colours() {
    for (uint32_t alpha : {0u, 128u, 254u}) {
        const uint32_t n = 2000;
        std::vector<uint8_t> blocks((size_t)n * 16), astc((size_t)n * 16), a((size_t)n * 64), b((size_t)n * 64);
        double sums[2] = {0, 0};
        for (int opaque = 0; opaque < 2; opaque++) {
            rng = 0x1234567u + alpha;
            for (auto& x : blocks) x = (uint8_t)random_word();
            for (uint32_t i = 0; i < n; i++) {
                uint8_t* blk = &blocks[(size_t)i * 16];
                blk[0] = blk[1] = (uint8_t)(opaque ? 255 : alpha);
                std::memset(blk + 2, 0, 6); // index 0 throughout: the first end
            }
            transcode(BC3, blocks.data(), n, 1, 1, astc.data());
            bcn_decode_image(blocks.data(), n * 4, 4, BC3, a.data());
            astc_decode_image(astc.data(), n * 4, 4, 4, 4, b.data(), 0);
            uint32_t off = 0;
            for (size_t i = 0; i < a.size(); i += 4) {
                for (int c = 0; c < 3; c++) sums[opaque] += double(a[i + c] - b[i + c]) * double(a[i + c] - b[i + c]);
                off += a[i + 3] != b[i + 3];
            }
            CHECK(off == 0, "random BC3 blocks under one alpha of %u: %u texels' alpha changed", opaque ? 255 : alpha,
                  off);
        }
        double db[2];
        for (int k = 0; k < 2; k++) db[k] = 10.0 * std::log10(255.0 * 255.0 / (sums[k] / (double(n) * 48)));
        // both hold BC1's four colours at their weights: an opaque block's 16, these blocks' 4
        bool pass = db[0] >= db[1] - 3.0 && db[1] >= 48.9;
        std::printf("%s random BC3 colours under one alpha of %u: %.2f dB, opaque %.2f dB (within 3 dB of it)\n",
                    pass ? "PASS" : "FAIL", alpha, db[0], db[1]);
        if (!pass) failures++;
    }
}

// ---- which textures are made ASTC: the ones whose alpha is one value within every block (bcn_astc.h)

// the alpha of a block's sixteen texels, by the reference decoder: one value?
static bool alpha_one(int kind, const uint8_t b[16]) {
    uint8_t dec[64];
    bcn_decode_image(b, 4, 4, kind, dec);
    for (int i = 1; i < 16; i++)
        if (dec[4 * i + 3] != dec[3]) return false;
    return true;
}
static int alpha_kind(int kind) { return kind == BC2 ? SNX_ALPHA_BC2 : kind == BC3 ? SNX_ALPHA_BC3 : SNX_ALPHA_BC7; }
// Blocks of every shape, random and made to have one alpha in each way a block can: a block said to have one alpha
// has one (never the other way round, which would send a texture of soft alpha to ASTC), the ways the blocks' bytes
// show are all found, and the encoder's block of such a block keeps its alpha: exact for BC2 and BC3, and for BC7 at 0
// and 255, which are levels of every range, else the nearest of 192 levels, one off at most (of 48, three off, in a
// block whose second plane is a colour channel's, a BC7 mode 4 or 5 block that rotates one in)
static void alpha_fixed() {
    for (int kind : {BC2, BC3, BC7}) {
        uint32_t said = 0, wrong = 0, missed = 0, one = 0, lost = 0, shown_missed = 0, seen[9] = {0}, off = 0;
        const uint32_t n = 24000;
        for (uint32_t i = 0; i < n; i++) {
            uint8_t b[16];
            for (auto& x : b) x = (uint8_t)random_word();
            uint32_t way = i % 6;
            bool shown = false; // made so that the bytes show its one alpha
            auto put = [&](int at, int bits_, uint32_t v) {
                for (int k = 0; k < bits_; k++) {
                    b[(at + k) / 8] &= (uint8_t)~(1 << ((at + k) % 8));
                    if (v >> k & 1) b[(at + k) / 8] |= (uint8_t)(1 << ((at + k) % 8));
                }
            };
            auto get = [&](int at, int bits_) { return bits(b, at, at + bits_ - 1); };
            if (kind == BC2 && way >= 3) {
                for (int k = 0; k < 8; k++) b[k] = (uint8_t)((b[0] & 15) * 17);
                shown = true;
            } else if (kind == BC3 && way == 3) { // one index throughout
                for (int k = 1; k < 16; k++) put(16 + 3 * k, 3, get(16, 3));
                shown = true;
            } else if (kind == BC3 && way == 4) { // equal ends, indices between them
                b[1] = b[0];
                for (int k = 0; k < 16; k++) put(16 + 3 * k, 3, get(16 + 3 * k, 3) % 6);
                shown = true;
            } else if (kind == BC3 && way == 5) { // equal ends, but an index that is 0 or 255: not one alpha
                b[1] = b[0] = (uint8_t)(1 + b[0] % 254);
                put(16 + 3 * (int)(i % 16), 3, 6 + i % 2);
                put(16 + 3 * (int)((i + 1) % 16), 3, 2);
            } else if (kind == BC7) {
                uint32_t mode = i / 6 % 9; // 8: a block without a mode
                b[0] = mode == 8 ? 0 : (uint8_t)((b[0] | 1) << mode);
                if (way >= 3 && mode == 4) {
                    uint32_t rot = get(5, 2), at = rot ? 8 + 10 * (rot - 1) : 38, w = rot ? 5 : 6;
                    put((int)(at + w), (int)w, get((int)at, (int)w));
                } else if (way >= 3 && mode == 5) {
                    uint32_t rot = get(6, 2), at = rot ? 8 + 14 * (rot - 1) : 50, w = rot ? 7 : 8;
                    put((int)(at + w), (int)w, get((int)at, (int)w));
                } else if (way >= 3 && mode == 6) {
                    put(56, 7, get(49, 7)), put(64, 1, get(63, 1));
                } else if (way >= 3 && mode == 7) {
                    for (int k = 1; k < 4; k++) put(74 + 5 * k, 5, get(74, 5)), put(94 + k, 1, get(94, 1));
                }
                shown = way >= 3 || mode < 4 || mode == 8;
                if (way == 5 && mode >= 4 && mode < 8) {
                    // one bit of the ends off again: alpha's (or the rotated channel's) second end, or a p-bit
                    uint32_t rot = mode == 4 ? get(5, 2) : mode == 5 ? get(6, 2) : 0;
                    int at = mode == 4   ? (int)(rot ? 8 + 10 * (rot - 1) + 5 : 44)
                             : mode == 5 ? (int)(rot ? 8 + 14 * (rot - 1) + 7 : 58)
                             : mode == 6 ? (i % 12 < 6 ? 56 : 64)
                                         : (i % 12 < 6 ? 79 + 5 * (int)(i % 3) : 95 + (int)(i % 3));
                    put(at, 1, get(at, 1) ^ 1);
                    shown = false;
                }
                seen[mode] += snx_bcn_alpha_fixed(SNX_ALPHA_BC7, b);
            }
            bool fixed = snx_bcn_alpha_fixed(alpha_kind(kind), b) != 0, truth = alpha_one(kind, b);
            said += fixed, one += truth, wrong += fixed && !truth, missed += truth && !fixed;
            shown_missed += shown && !fixed;
            if (fixed) {
                // the encoder's block of it: its alpha as it was
                uint8_t astc[16], a[64], d[64];
                transcode(kind, b, 1, 1, 1, astc);
                bcn_decode_image(b, 4, 4, kind, a);
                astc_decode_image(astc, 4, 4, 4, 4, d, 0);
                // 256 levels where the block keeps alpha apart, which BC2 and BC3 do: exact
                uint32_t most = stored_kind(astc) == KIND_DUAL ? 3 : kind == BC7 ? 1 : 0;
                for (int t = 0; t < 16; t++) {
                    uint32_t x = a[4 * t + 3], y = d[4 * t + 3], gap = x > y ? x - y : y - x;
                    lost += gap > ((x == 0 || x == 255) ? 0 : most);
                    off = std::max(off, gap);
                }
            }
        }
        bool modes = kind != BC7 ||
                     (seen[0] && seen[1] && seen[2] && seen[3] && seen[4] && seen[5] && seen[6] && seen[7] && seen[8]);
        bool pass = wrong == 0 && shown_missed == 0 && lost == 0 && said > n / 4 && modes;
        std::printf("%s %s one alpha a block: %u of %u blocks said so, %u of them wrongly, %u made so missed; the "
                    "encoder's blocks of them: %u texels' alpha past its level (the farthest %u off); %u more have one "
                    "alpha that the bytes don't show\n",
                    pass ? "PASS" : "FAIL", kind_name(kind), said, n, wrong, shown_missed, lost, off, missed);
        if (!pass) failures++;
    }
    // the picture's opaque regions are textures of one alpha a block, its see-through one (a soft ramp and a hard
    // shape) is not, in any kind: that one stays ETC2
    for (int kind : {BC2, BC3, BC7})
        for (int r = 0; r < 4; r++) {
            std::vector<uint32_t> pic(64 * 64);
            for (uint32_t y = 0; y < 64; y++)
                for (uint32_t x = 0; x < 64; x++) pic[y * 64 + x] = picture((r & 1) * 64 + x, (r >> 1) * 64 + y);
            std::vector<uint8_t> blocks = bc_image(kind, pic, 64, 64);
            int fixed = snx_bcn_alpha_all_fixed(alpha_kind(kind), blocks.data(), blocks.size() / 16);
            uint32_t moving = 0;
            for (size_t i = 0; i < blocks.size(); i += 16) moving += !snx_bcn_alpha_fixed(alpha_kind(kind), &blocks[i]);
            CHECK(fixed == (r != 3) && (r == 3) == (moving != 0),
                  "%s picture region %d: every block of one alpha %d, %u blocks whose alpha moves", kind_name(kind), r,
                  fixed, moving);
            // the soft ramp's blocks are found, not a stray one: half the region is ramp
            if (r == 3)
                CHECK(moving >= 64, "%s see-through region: only %u blocks' alpha moves", kind_name(kind), moving);
        }
}

// what the transcoder tells the encoder of a BC7 block: random blocks of each mode, and where the block it stores
// follows the source's way, it is the source's own: the plane of the channel modes 4 and 5 rotate out (alpha, or
// red, green or blue), and for two and three subsets the seed the tables give the block's partition
static void bc7_said() {
    static const uint32_t subsets[8] = {3, 2, 3, 2, 1, 1, 1, 2}, partition_bits[8] = {4, 6, 6, 6, 0, 0, 0, 6};
    uint32_t planes[4] = {0, 0, 0, 0}, seeds[2] = {0, 0};
    for (uint32_t i = 0; i < 12000; i++) {
        uint8_t b[16], astc[16];
        for (auto& x : b) x = (uint8_t)random_word();
        uint32_t mode = i % 8;
        b[0] = (uint8_t)((b[0] | 1) << mode);
        transcode(BC7, b, 1, 1, 1, astc);
        int kind = stored_kind(astc);
        if ((mode == 4 || mode == 5) && kind == KIND_DUAL) {
            uint32_t rot = bits(b, (int)mode + 1, (int)mode + 2), own = rot ? rot - 1 : 3;
            CHECK(bits(astc, 62, 63) == own, "a BC7 block of mode %u, rotation %u: the second plane is channel %u",
                  mode, rot, bits(astc, 62, 63));
            planes[own]++;
        }
        if (subsets[mode] > 1 && kind == KIND_TWO) {
            uint32_t p = bits(b, (int)mode + 1, (int)(mode + partition_bits[mode]));
            uint32_t seed = astc_partition_cpu(subsets[mode], p) & 1023;
            CHECK(bits(astc, 13, 22) == seed, "a BC7 block of mode %u, partition %u: seed %u, not the table's %u", mode,
                  p, bits(astc, 13, 22), seed);
            seeds[subsets[mode] - 2]++;
        }
        // mode 7's two subsets carry alpha: never two partitions, whose ends hold none
        if (mode == 7) CHECK(kind != KIND_TWO || bits(b, 74, 93) == 0xFFFFF, "a BC7 block of mode 7 as two partitions");
    }
    CHECK(planes[0] > 50 && planes[1] > 50 && planes[2] > 50 && planes[3] > 50 && seeds[0] > 500 && seeds[1] > 500,
          "BC7 blocks stored the source's way: planes of R %u, G %u, B %u, A %u; seeds of two subsets %u, of three %u",
          planes[0], planes[1], planes[2], planes[3], seeds[0], seeds[1]);
}

// an image whose sides aren't multiples of 4, of three slices, transcodes block for block as each block alone: the
// dispatch reads and writes the blocks it should
static void layout() {
    const uint32_t bw = 67, bh = 5, slices = 3;
    for (int kind : {BC2, BC3, BC7}) {
        uint32_t n = bw * bh * slices;
        std::vector<uint8_t> blocks((size_t)n * 16), astc((size_t)n * 16);
        for (auto& x : blocks) x = (uint8_t)random_word();
        transcode(kind, blocks.data(), bw, bh, slices, astc.data());
        for (uint32_t i = 0; i < n; i++) {
            uint8_t one[16];
            bcn_astc_cpu(kind, &blocks[(size_t)i * 16], 1, 1, 1, 0, one);
            CHECK(!std::memcmp(one, &astc[(size_t)i * 16], 16), "%s block %u of a %ux%ux%u image differs from it alone",
                  kind_name(kind), i, bw, bh, slices);
        }
    }
}

// ---- the picture

struct Measure {
    double rgb, alpha;
};
// kind's blocks of a w x h picture transcoded to ASTC (or, etc2, to ETC2 RGBA8), against them decoded: the PSNR of
// colour and alpha apart; the decoded pictures into dec_bc and dec_out (RGBA8)
static Measure measure(int kind, bool etc2, const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h,
                       std::vector<uint8_t>* dec_bc, std::vector<uint8_t>* dec_out) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> out(blocks.size()), a((size_t)w * h * 4), b((size_t)bw * bh * 64);
    bcn_decode_image(blocks.data(), w, h, kind, a.data());
    if (etc2) {
        bcn_etc2_cpu(kind, blocks.data(), bw, bh, 1, out.data());
        b.resize((size_t)w * h * 4);
        etc2_decode_image(out.data(), w, h, 2 /* etc2.h's ETC2_RGBA */, b.data());
    } else {
        transcode(kind, blocks.data(), bw, bh, 1, out.data());
        // whole blocks, then the picture's part of them
        astc_decode_image(out.data(), bw * 4, bh * 4, 4, 4, b.data(), 0);
        std::vector<uint8_t> cut((size_t)w * h * 4);
        for (uint32_t y = 0; y < h; y++) std::memcpy(&cut[(size_t)y * w * 4], &b[(size_t)y * bw * 16], (size_t)w * 4);
        b = cut;
    }
    std::vector<uint8_t> ca, cb, aa, ab;
    for (size_t i = 0; i < (size_t)w * h; i++) {
        for (int c = 0; c < 3; c++) ca.push_back(a[4 * i + c]), cb.push_back(b[4 * i + c]);
        aa.push_back(a[4 * i + 3]), ab.push_back(b[4 * i + 3]);
    }
    Measure m = {psnr(ca.data(), cb.data(), ca.size(), 1, false), psnr(aa.data(), ab.data(), aa.size(), 1, false)};
    if (dec_bc) *dec_bc = a;
    if (dec_out) *dec_out = b;
    return m;
}
// each kind's PSNR on each region of the picture (colour, and alpha in the see-through region), held to what the
// encoder reached when it was written, half a dB under
static void picture_psnr() {
    static const char* regions[4] = {"gradients", "edges", "noise", "see-through"};
    static const struct {
        int kind;
        double rgb[4], alpha[4];
    } floors[] = {{BC2, {52.6, 58.4, 45.1, 43.4}, {0, 0, 0, 42.9}},
                  {BC3, {52.6, 58.4, 45.1, 42.9}, {0, 0, 0, 44.9}},
                  {BC7, {44.9, 53.1, 20.4, 45.1}, {0, 0, 0, 49.0}}};
    for (const auto& f : floors) {
        bool pass = true;
        std::string line;
        char part[96];
        for (int r = 0; r < 4; r++) {
            std::vector<uint32_t> pic(64 * 64);
            for (uint32_t y = 0; y < 64; y++)
                for (uint32_t x = 0; x < 64; x++) pic[y * 64 + x] = picture((r & 1) * 64 + x, (r >> 1) * 64 + y);
            Measure m = measure(f.kind, false, bc_image(f.kind, pic, 64, 64), 64, 64, nullptr, nullptr);
            pass = pass && m.rgb >= f.rgb[r] && m.alpha >= f.alpha[r];
            std::snprintf(part, sizeof part, "%s%s %.2f (%.1f)", r ? ", " : "", regions[r], m.rgb, f.rgb[r]);
            line += part;
            if (r == 3) {
                std::snprintf(part, sizeof part, " alpha %.2f (%.1f)", m.alpha, f.alpha[r]);
                line += part;
            }
        }
        std::printf("%s picture %s to ASTC 4x4, dB (at least): %s\n", pass ? "PASS" : "FAIL", kind_name(f.kind),
                    line.c_str());
        if (!pass) failures++;
    }
}

// --image FILE W H [--dump DIR]: a picture of one's own measured as the test picture is, beside the ETC2 transcoder
static int image(const char* file, uint32_t w, uint32_t h, const char* dump) {
    FILE* f = std::fopen(file, "rb");
    if (!f) {
        std::printf("cannot open %s\n", file);
        return 2;
    }
    std::vector<uint32_t> pic((size_t)w * h);
    std::vector<uint8_t> bytes((size_t)w * h * 4);
    size_t got = std::fread(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    if (got != bytes.size()) {
        std::printf("%s holds %zu bytes, not the %zu of %ux%u RGBA8\n", file, got, bytes.size(), w, h);
        return 2;
    }
    for (size_t i = 0; i < pic.size(); i++)
        pic[i] = rgba(bytes[4 * i], bytes[4 * i + 1], bytes[4 * i + 2], bytes[4 * i + 3]);
    for (int kind : {BC2, BC3, BC7}) {
        std::vector<uint8_t> blocks = bc_image(kind, pic, w, h), a, b[2];
        Measure m[2];
        for (int etc2 = 0; etc2 < 2; etc2++) m[etc2] = measure(kind, etc2 != 0, blocks, w, h, &a, &b[etc2]);
        uint32_t seen[7] = {0, 0, 0, 0, 0, 0, 0}, bw = (w + 3) / 4, bh = (h + 3) / 4;
        std::vector<uint8_t> astc(blocks.size());
        bcn_astc_cpu(kind, blocks.data(), bw, bh, 1, 0, astc.data());
        for (size_t i = 0; i < astc.size(); i += 16) seen[stored_kind(&astc[i])]++;
        uint32_t moving = 0;
        for (size_t i = 0; i < blocks.size(); i += 16) moving += !snx_bcn_alpha_fixed(alpha_kind(kind), &blocks[i]);
        std::printf("%s %s: %s (%u blocks' alpha moves)\n", file, kind_name(kind),
                    moving ? "stays ETC2 RGBA8, its alpha moves within a block" : "made ASTC 4x4, one alpha a block",
                    moving);
        std::printf("%s %s: ASTC 4x4 %.2f dB, alpha %.2f dB; ETC2 RGBA8 %.2f dB, alpha %.2f dB; of %u blocks RGB %u, "
                    "RGBA %u, RGBA of 2 bit weights %u, two planes %u, two partitions %u, one colour %u\n",
                    file, kind_name(kind), m[0].rgb, m[0].alpha, m[1].rgb, m[1].alpha, bw * bh, seen[KIND_RGB],
                    seen[KIND_RGBA], seen[KIND_RGBA2], seen[KIND_DUAL], seen[KIND_TWO], seen[KIND_VOID]);
        if (dump) {
            // the pictures, then the blocks: the game's, and what each transcoder makes of them
            std::vector<uint8_t> etc2(blocks.size());
            bcn_etc2_cpu(kind, blocks.data(), bw, bh, 1, etc2.data());
            static const char* const names[6] = {".bcn.raw",    ".astc.raw",    ".etc2.raw",
                                                 ".bcn.blocks", ".astc.blocks", ".etc2.blocks"};
            const std::vector<uint8_t>* files[6] = {&a, &b[0], &b[1], &blocks, &astc, &etc2};
            for (int which = 0; which < 6; which++) {
                std::string path = std::string(dump) + "/" + kind_name(kind) + names[which];
                FILE* o = std::fopen(path.c_str(), "wb");
                if (!o) {
                    std::printf("cannot write %s\n", path.c_str());
                    return 2;
                }
                std::fwrite(files[which]->data(), 1, files[which]->size(), o);
                std::fclose(o);
            }
        }
    }
    return failures != 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && !std::strcmp(argv[1], "--image"))
        return image(argv[2], (uint32_t)std::atoi(argv[3]), (uint32_t)std::atoi(argv[4]),
                     argc >= 7 && !std::strcmp(argv[5], "--dump") ? argv[6] : nullptr);
    if (argc > 1) {
        std::printf("usage: %s [--image FILE W H [--dump DIR]]\n", argv[0]);
        return 2;
    }
    halves();
    levels();
    levels_decoded();
    exact_blocks();
    partitions();
    random_blocks();
    one_alpha_colours();
    alpha_fixed();
    bc7_said();
    layout();
    picture_psnr();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::puts(
        "BCn to ASTC: the levels, exact blocks, the partition tables, random blocks, the layout and the picture's "
        "PSNR as held, the same bytes with 32 bit floats and with halves");
    return 0;
}
