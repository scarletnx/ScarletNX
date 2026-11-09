// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BC7 encoder's body (src/nx/gpu/texture/bc7_encode.glsl) run on the CPU through tests/glsl/glsl.hpp, its blocks
// decoded by the CPU reference (tests/support/bcn.c): its least squares fit gives a line's ends back, a block of one
// colour comes back exact, so does a line of two endpoints the format holds and a block each mode holds, blocks with a
// channel apart come back closer than the encoder without modes 4 and 5 brings them, and a picture of gradients, hard
// edges, noise and alpha comes back no worse than the PSNR each region is held to; sizes that are not a multiple of 4
// and strips of several slices encode as vk/astc.c dispatches them. No GPU is needed.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "bc7_cpu.h"
// tests/support/bcn.h's one function (its header brings the recompiler's C helpers, which C++ warns about)
extern "C" void bcn_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
static const int BCN_BC7 = 9; // tests/support/bcn.h

static int failures;

// the texels of a w x h image of depth slices (RGBA8 words, R in the low byte) encoded as vk/astc.c dispatches them
static std::vector<glsl::leuvec4> encode(const std::vector<glsl::leuint>& texels, uint32_t w, uint32_t h,
                                         uint32_t depth) {
    std::vector<glsl::leuvec4> blocks((size_t)((w + 3) / 4) * ((h + 3) / 4) * depth);
    bc7_encode_cpu(reinterpret_cast<const uint8_t*>(texels.data()), w, h, depth,
                   reinterpret_cast<uint8_t*>(blocks.data()));
    return blocks;
}

// the same with the whole search for every block: a block a mode holds comes back exact only from that
static std::vector<glsl::leuvec4> encode_whole(const std::vector<glsl::leuint>& texels, uint32_t w, uint32_t h) {
    std::vector<glsl::leuvec4> blocks((size_t)((w + 3) / 4) * ((h + 3) / 4));
    bc7_encode_cpu_whole(reinterpret_cast<const uint8_t*>(texels.data()), w, h, 1,
                         reinterpret_cast<uint8_t*>(blocks.data()));
    return blocks;
}
static std::vector<glsl::leuvec4> encode_whole_without_45(const std::vector<glsl::leuint>& texels,
                                                         uint32_t w, uint32_t h) {
    std::vector<glsl::leuvec4> blocks((size_t)((w + 3) / 4) * ((h + 3) / 4));
    bc7_encode_cpu_whole_without_45(reinterpret_cast<const uint8_t*>(texels.data()), w, h, 1,
                                    reinterpret_cast<uint8_t*>(blocks.data()));
    return blocks;
}

// the same by the encoder without modes 4 and 5
static std::vector<glsl::leuvec4> encode_without_45(const std::vector<glsl::leuint>& texels, uint32_t w, uint32_t h) {
    std::vector<glsl::leuvec4> blocks((size_t)((w + 3) / 4) * ((h + 3) / 4));
    bc7_encode_cpu_without_45(reinterpret_cast<const uint8_t*>(texels.data()), w, h, 1,
                              reinterpret_cast<uint8_t*>(blocks.data()));
    return blocks;
}

// the same by the build vk/astc.c picks for a level (1 the fast build, 2 mode 6 alone, 3 the quick build)
static std::vector<glsl::leuvec4> encode_build(uint32_t build, const std::vector<glsl::leuint>& texels, uint32_t w,
                                               uint32_t h) {
    std::vector<glsl::leuvec4> blocks((size_t)((w + 3) / 4) * ((h + 3) / 4));
    bc7_encode_cpu_build(build, reinterpret_cast<const uint8_t*>(texels.data()), w, h, 1,
                         reinterpret_cast<uint8_t*>(blocks.data()));
    return blocks;
}

// one slice's blocks decoded back to RGBA8 bytes, w x h
static std::vector<uint8_t> decode(const glsl::leuvec4* blocks, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out((size_t)w * h * 4);
    bcn_decode_image(reinterpret_cast<const uint8_t*>(blocks), w, h, BCN_BC7, out.data());
    return out;
}

static uint8_t byte_of(const glsl::leuint& word, uint32_t c) { return uint8_t(uint32_t(word) >> (c * 8)); }

// every block has a mode (0 is the one the encoder never writes, three subsets of 4-bit ends), and how many of each,
// for the picture's line
static bool known_modes(const std::vector<glsl::leuvec4>& blocks, const char* what, uint32_t count[8]) {
    for (size_t i = 0; i < blocks.size(); i++) {
        const uint32_t first = uint32_t(blocks[i].x) & 0xFFu;
        uint32_t mode = 0;
        while (mode < 8 && !(first >> mode & 1)) mode++;
        if (mode == 8 || mode == 0) {
            std::fprintf(stderr, "FAIL %s: block %zu starts %08x, a mode the encoder does not write\n", what, i,
                         uint32_t(blocks[i].x));
            failures++;
            return false;
        }
        if (count) count[mode]++;
    }
    return true;
}

// the edenprobe astcimage suite's picture (tools/astc-images.py there), integer arithmetic, so the regions are the
// same content an emulator meets in that suite
static uint32_t clamp255(int v) { return uint32_t(v < 0 ? 0 : v > 255 ? 255 : v); }
static uint32_t rgba_of(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
static uint32_t picture(uint32_t x, uint32_t y) {
    const int u = int(x % 64), v = int(y % 64);
    if (y < 64 && x < 64) {
        const int d = (u - 32) * (u - 32) + (v - 32) * (v - 32);
        return rgba_of(clamp255(24 + 3 * u), clamp255(40 + 3 * v), clamp255(220 - u - v - (d >> 5)), 255);
    }
    if (y < 64) {
        if ((u - 20) * (u - 20) + (v - 20) * (v - 20) < 196) return rgba_of(220, 40, 30, 255);
        if (u >= 40 && v >= 40) {
            static const uint32_t three[3] = {rgba_of(250, 250, 250, 255), rgba_of(20, 20, 20, 255),
                                              rgba_of(40, 200, 60, 255)};
            return three[(u / 3 + v / 3) % 3];
        }
        if (u % 9 == 4) return rgba_of(255, 255, 255, 255);
        return u + v > 70 ? rgba_of(30, 60, 200, 255) : rgba_of(240, 200, 40, 255);
    }
    if (x < 64) {
        uint32_t s = uint32_t(v * 64 + u) + 0x9E3779B9u;
        s ^= s >> 16;
        s *= 0x7FEB352Du;
        s ^= s >> 15;
        s *= 0x846CA68Bu;
        s ^= s >> 16;
        return rgba_of(clamp255(110 + int(s & 0xFF) * 80 / 255 - 40),
                       clamp255(140 + int(s >> 8 & 0xFF) * 80 / 255 - 40),
                       clamp255(90 + int(s >> 16 & 0xFF) * 80 / 255 - 40), 255);
    }
    const uint32_t r = clamp255(255 - 3 * u), g = clamp255(128 + v), b = clamp255(4 * u);
    const uint32_t a = v < 32 ? clamp255(8 * v + 2 * u) : (u * u + 3 * v * v) % 23 < 12 ? 255u : 0u;
    return rgba_of(r, g, b, a);
}

// PSNR in dB of decoded bytes against the texels over the rectangle x0..x1, y0..y1 of a w-wide image
static double psnr(const std::vector<glsl::leuint>& texels, const std::vector<uint8_t>& got, uint32_t w, uint32_t x0,
                   uint32_t y0, uint32_t x1, uint32_t y1) {
    double sum = 0;
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = x0; x < x1; x++)
            for (uint32_t c = 0; c < 4; c++) {
                const double d = double(got[((size_t)y * w + x) * 4 + c]) - byte_of(texels[(size_t)y * w + x], c);
                sum += d * d;
            }
    const double mse = sum / ((x1 - x0) * (y1 - y0) * 4.0);
    return mse == 0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// the encoder's weights are the format's (the Khronos Data Format Specification's BPTC weight tables for 2, 3 and
// 4-bit indices): one off, and every error and fit it works out is off, while blocks still decode
static void weights() {
    static const uint32_t w2[4] = {0, 21, 43, 64}, w3[8] = {0, 9, 18, 27, 37, 46, 55, 64},
                          w4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    const uint32_t* tables[3] = {w2, w3, w4};
    for (uint32_t bits = 2; bits <= 4; bits++)
        for (uint32_t i = 0; i < (1u << bits); i++)
            if (bc7_weight_cpu(bits, i) != tables[bits - 2][i]) {
                std::fprintf(stderr, "FAIL %u-bit weight %u is %u, the format's %u\n", bits, i, bc7_weight_cpu(bits, i),
                             tables[bits - 2][i]);
                failures++;
                return;
            }
    std::puts("PASS the 2, 3 and 4-bit weights are the format's");
}

static uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

// the encoder's least squares fit to the weights its texels took. 16 texels on a line between two ends, texel k at the
// weight of an index of 2, 3 or 4 bits (each index as often as the others), give the ends back, within one where the
// texels were rounded; in each subset of every two and three subset partition, at random indices, a channel of one
// value (alpha 0 or 255, which a block keeps exactly) gets that value at both ends. Whole blocks do not show a wrong
// fit: the encoder keeps the bounding box's line where the fit's is worse
static void least_squares() {
    uint32_t s = 0x6C8E9CF5u, worst = 0;
    for (uint32_t bits = 2; bits <= 4; bits++)
        for (uint32_t n = 0; n < 32; n++) {
            const uint32_t e0 = xorshift(s), e1 = xorshift(s);
            uint32_t texels[16], ends[2];
            uint8_t index[16];
            for (uint32_t k = 0; k < 16; k++) {
                index[k] = uint8_t((k * 5 + n) & ((1u << bits) - 1));
                const uint32_t w = bc7_weight_cpu(bits, index[k]);
                texels[k] = 0;
                for (uint32_t c = 0; c < 4; c++)
                    texels[k] |= ((64 - w) * (e0 >> (c * 8) & 0xFF) + w * (e1 >> (c * 8) & 0xFF) + 32) >> 6 << (c * 8);
            }
            if (!bc7_least_squares_cpu(texels, index, bits, 1, 0, 0, ends)) {
                std::fprintf(stderr, "FAIL least squares, %u-bit indices: no fit of 16 texels at every weight\n", bits);
                failures++;
                return;
            }
            for (uint32_t c = 0; c < 4; c++)
                for (uint32_t side = 0; side < 2; side++) {
                    const int want = int((side ? e1 : e0) >> (c * 8) & 0xFF), got = int(ends[side] >> (c * 8) & 0xFF);
                    const uint32_t d = uint32_t(got > want ? got - want : want - got);
                    if (d > worst) worst = d;
                    if (d > 1) {
                        std::fprintf(stderr,
                                     "FAIL least squares, %u-bit indices: end %u channel %u is %d, the line's %d\n",
                                     bits, side, c, got, want);
                        failures++;
                        return;
                    }
                }
        }
    uint32_t fits = 0;
    for (uint32_t subsets = 2; subsets <= 3; subsets++)
        for (uint32_t part = 0; part < 64; part++)
            for (uint32_t subset = 0; subset < subsets; subset++)
                for (uint32_t bits = 2; bits <= 4; bits++) {
                    const uint32_t alpha = xorshift(s) & 1 ? 255 : 0;
                    uint32_t texels[16], ends[2];
                    uint8_t index[16];
                    for (uint32_t k = 0; k < 16; k++) {
                        texels[k] = (xorshift(s) & 0xFFFFFFu) | alpha << 24;
                        index[k] = uint8_t(xorshift(s) & ((1u << bits) - 1));
                    }
                    if (!bc7_least_squares_cpu(texels, index, bits, subsets, part, subset, ends)) continue;
                    fits++;
                    if (ends[0] >> 24 != alpha || ends[1] >> 24 != alpha) {
                        std::fprintf(
                            stderr,
                            "FAIL least squares, subset %u of %u subset partition %u, %u-bit indices: alpha %u "
                            "fits to ends of alpha %u and %u\n",
                            subset, subsets, part, bits, alpha, ends[0] >> 24, ends[1] >> 24);
                        failures++;
                        return;
                    }
                }
    std::printf("PASS the least squares fit gives 96 lines' ends back, at most %u away, and %u subsets' one alpha\n",
                worst, fits);
}

// a block of one colour, alpha included, whose four channels share their low bit: mode 6 holds it exactly (both ends
// the colour, each channel 2 e + p under the one p-bit an end has)
static void constant_blocks() {
    std::vector<glsl::leuint> texels(16 * 64);
    uint32_t s = 0x2545F491u;
    for (uint32_t b = 0; b < 64; b++) {
        s ^= s << 13;
        s ^= s >> 17;
        s ^= s << 5;
        const uint32_t colour = (s & 0xFEFEFEFEu) | (b & 1 ? 0x01010101u : 0u);
        for (uint32_t t = 0; t < 16; t++) texels[(size_t)(t / 4) * 256 + b * 4 + t % 4] = colour;
    }
    const auto blocks = encode(texels, 256, 4, 1);
    const auto got = decode(blocks.data(), 256, 4);
    known_modes(blocks, "constant blocks", nullptr);
    for (size_t i = 0; i < texels.size(); i++)
        for (uint32_t c = 0; c < 4; c++)
            if (got[i * 4 + c] != byte_of(texels[i], c)) {
                std::fprintf(stderr, "FAIL constant blocks: texel %zu channel %u is %u, not %u\n", i, c, got[i * 4 + c],
                             byte_of(texels[i], c));
                failures++;
                return;
            }
    std::puts("PASS 64 blocks of one colour each come back exact");
}

// texels that are exactly what mode 6 interpolates between two ends it holds (one end's channels even, the other's
// even or odd: p-bits 0 and 0, or 0 and 1), the sixteen weights each once in a scrambled order: the encoder finds
// those ends and weights again
static void exact_lines() {
    static const uint32_t order[16] = {3, 14, 0, 9, 6, 11, 15, 1, 12, 4, 8, 13, 2, 10, 7, 5};
    std::vector<glsl::leuint> texels(16 * 8);
    uint32_t s = 0x9E3779B9u;
    for (uint32_t b = 0; b < 8; b++) {
        uint32_t e0[4], e1[4];
        for (uint32_t c = 0; c < 4; c++) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            // half the blocks with the second end odd: p-bits 0 and 1, which a block takes only when it tries both
            e0[c] = s & 0xFE;
            e1[c] = ((s >> 8) & 0xFE) | (b & 1);
        }
        for (uint32_t t = 0; t < 16; t++) {
            const uint32_t w = (order[t] * 64 + 7) / 15;
            uint32_t word = 0;
            for (uint32_t c = 0; c < 4; c++) word |= ((64 - w) * e0[c] + w * e1[c] + 32) >> 6 << (c * 8);
            texels[(size_t)(t / 4) * 32 + b * 4 + t % 4] = word;
        }
    }
    const auto blocks = encode_whole(texels, 32, 4);
    const auto got = decode(blocks.data(), 32, 4);
    known_modes(blocks, "exact lines", nullptr);
    const double db = psnr(texels, got, 32, 0, 0, 32, 4);
    if (db < 99.0) {
        std::fprintf(stderr, "FAIL exact lines: %.2f dB, not exact\n", db);
        failures++;
        return;
    }
    std::puts("PASS 8 blocks that lie on a line mode 6 holds come back exact");
}

// a block of 16 texels whose subsets of BC7 partition `mask` (two bits a texel, as the format's three subset table)
// take colours[subset]: the mode that holds those colours exactly brings it back exact, which needs the partition, its
// anchors and the mode's layout right
static void partitioned(const char* what, const uint32_t masks[64], uint32_t (*colour)(uint32_t part, uint32_t s),
                        uint32_t mode) {
    std::vector<glsl::leuint> texels(16 * 64);
    for (uint32_t b = 0; b < 64; b++)
        for (uint32_t t = 0; t < 16; t++)
            texels[(size_t)(t / 4) * 256 + b * 4 + t % 4] = colour(b, masks[b] >> (t * 2) & 3);
    const auto blocks = encode_whole(texels, 256, 4);
    const auto got = decode(blocks.data(), 256, 4);
    uint32_t modes[8] = {0};
    known_modes(blocks, what, modes);
    if (modes[mode] != 64) {
        std::fprintf(stderr, "FAIL %s: %u of the 64 blocks in mode %u\n", what, modes[mode], mode);
        failures++;
        return;
    }
    for (size_t i = 0; i < texels.size(); i++)
        for (uint32_t c = 0; c < 4; c++)
            if (got[i * 4 + c] != byte_of(texels[i], c)) {
                std::fprintf(stderr, "FAIL %s: partition %zu texel %zu channel %u is %u, not %u (block starts %08x)\n",
                             what, (i % 256) / 4, (i / 256) * 4 + i % 4, c, got[i * 4 + c], byte_of(texels[i], c),
                             uint32_t(blocks[(i % 256) / 4].x));
                failures++;
                return;
            }
    std::printf("PASS %s come back exact\n", what);
}
// the format's partitions, two bits a texel: the two subset table (from tests/support/bcn.c's) and the three
static uint32_t two_subsets[64], three_subsets[64];
static void tables() {
    static const uint16_t part2[64] = {
        0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
        0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
        0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
        0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
        0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
    for (uint32_t p = 0; p < 64; p++)
        for (uint32_t t = 0; t < 16; t++) two_subsets[p] |= (uint32_t(part2[p]) >> t & 1) << (t * 2);
    // three subsets: the encoder's own table, checked against bcn.c's by every block decoding back exact
    for (uint32_t p = 0; p < 64; p++) three_subsets[p] = bc7_partition3_cpu(p);
}
// colours that widen exactly from 6 bits and a subset's p-bit (mode 1, which is tried first), from 7 bits and a
// p-bit of 0 that 6 bits and a p-bit cannot hold (mode 3), from 5 bits (mode 2), and RGBA from 5 bits and
// a p-bit (mode 7)
static uint32_t widen7(uint32_t v) { return v << 1 | v >> 6; }
static uint32_t opaque1(uint32_t part, uint32_t s) {
    const uint32_t p = s & 1, v = ((part * 5 + s * 17) & 0x7E) | p;
    return rgba_of(widen7(v), widen7((126 - (v & 0x7E)) | p), widen7(((v * 3) & 0x7E) | p), 255);
}
static uint32_t opaque2(uint32_t part, uint32_t s) {
    const uint32_t v = (part * 37 + s * 101) & 0xFE;
    return rgba_of(v, 254 - v, (v * 3) & 0xFE, 255);
}
static uint32_t widen5(uint32_t v) { return v << 3 | v >> 2; }
static uint32_t opaque3(uint32_t part, uint32_t s) {
    const uint32_t v = (part * 7 + s * 11) & 31;
    return rgba_of(widen5(v), widen5(31 - v), widen5((v * 5) & 31), 255);
}
static uint32_t widen6(uint32_t v) { return v << 2 | v >> 4; }
static uint32_t alpha2(uint32_t part, uint32_t s) {
    const uint32_t v = ((part * 13 + s * 29) & 31) << 1;
    return rgba_of(widen6(v), widen6(62 - v), widen6((v * 3) & 62), widen6(s ? 62 : 0));
}

// the picture, region by region, held to what the encoder reached when it was written (gate values a little under)
static void picture_psnr() {
    std::vector<glsl::leuint> texels(128 * 128);
    for (uint32_t y = 0; y < 128; y++)
        for (uint32_t x = 0; x < 128; x++) texels[(size_t)y * 128 + x] = picture(x, y);
    const auto blocks = encode(texels, 128, 128, 1);
    const auto got = decode(blocks.data(), 128, 128);
    const auto without = decode(encode_without_45(texels, 128, 128).data(), 128, 128);
    const auto fast_blocks = encode_build(1, texels, 128, 128), mode6_blocks = encode_build(2, texels, 128, 128);
    const auto fast = decode(fast_blocks.data(), 128, 128), mode6 = decode(mode6_blocks.data(), 128, 128);
    const auto quick_blocks = encode_build(3, texels, 128, 128);
    const auto quick = decode(quick_blocks.data(), 128, 128);
    uint32_t modes[8] = {0}, fast_modes[8] = {0}, mode6_modes[8] = {0}, quick_modes[8] = {0};
    known_modes(blocks, "picture", modes);
    known_modes(fast_blocks, "picture in the fast build", fast_modes);
    known_modes(mode6_blocks, "picture in mode 6 alone", mode6_modes);
    std::printf("picture modes: 1 %u, 2 %u, 3 %u, 4 %u, 5 %u, 6 %u, 7 %u; the fast build's 6 %u of 1024\n", modes[1],
                modes[2], modes[3], modes[4], modes[5], modes[6], modes[7], fast_modes[6]);
    // the fast build keeps mode 6 on the blocks it holds closely, where the full build looks on: on more of them
    if (fast_modes[6] <= modes[6]) {
        std::printf("FAIL the fast build keeps mode 6 on %u blocks of the picture, the full build on %u\n",
                    fast_modes[6], modes[6]);
        failures++;
    }
    if (mode6_modes[6] != 32 * 32) {
        std::printf("FAIL the build of mode 6 alone wrote %u blocks of the picture in mode 6 of 1024\n",
                    mode6_modes[6]);
        failures++;
    }
    known_modes(quick_blocks, "picture in the quick build", quick_modes);
    if (quick_modes[6] != 32 * 32) {
        std::printf("FAIL the quick build wrote %u blocks of the picture in mode 6 of 1024\n", quick_modes[6]);
        failures++;
    }
    static const struct {
        const char* name;
        uint32_t x0, y0;
        double floor, fast_floor, mode6_floor, quick_floor; /* the full build's, the fast build's, mode 6 alone's and
                                                               the quick build's (mode 6 from one line) */
    } regions[4] = {{"gradients", 0, 0, 50.8, 43.0, 43.0, 42.5},
                    {"edges", 64, 0, 48.7, 46.5, 26.0, 22.9},
                    {"noise", 0, 64, 28.7, 28.7, 25.0, 24.5},
                    {"alpha", 64, 64, 47.6, 47.4, 39.8, 32.2}};
    for (const auto& r : regions) {
        const double db = psnr(texels, got, 128, r.x0, r.y0, r.x0 + 64, r.y0 + 64);
        const double fast_db = psnr(texels, fast, 128, r.x0, r.y0, r.x0 + 64, r.y0 + 64);
        const double mode6_db = psnr(texels, mode6, 128, r.x0, r.y0, r.x0 + 64, r.y0 + 64);
        const double quick_db = psnr(texels, quick, 128, r.x0, r.y0, r.x0 + 64, r.y0 + 64);
        const bool pass =
            db >= r.floor && fast_db >= r.fast_floor && mode6_db >= r.mode6_floor && quick_db >= r.quick_floor;
        std::printf("%s %s: %.2f dB, at least %.1f (%.2f without modes 4 and 5); the fast build %.2f, at least %.1f; "
                    "mode 6 alone %.2f, at least %.1f; the quick build %.2f, at least %.1f\n",
                    pass ? "PASS" : "FAIL", r.name, db, r.floor,
                    psnr(texels, without, 128, r.x0, r.y0, r.x0 + 64, r.y0 + 64), fast_db, r.fast_floor, mode6_db,
                    r.mode6_floor, quick_db, r.quick_floor);
        if (!pass) failures++;
    }
}

// what a mode 4 or 5 block decodes to: each channel of the texel from the ends e0 and e1 (RGBA, as widened) at the
// colour's weight cw or alpha's aw, then alpha swapped with red, green or blue (rot 1 to 3)
static uint32_t dual_texel(const uint32_t e0[4], const uint32_t e1[4], uint32_t cw, uint32_t aw, uint32_t rot) {
    uint32_t v[4];
    for (uint32_t c = 0; c < 4; c++) {
        const uint32_t w = c < 3 ? cw : aw;
        v[c] = ((64 - w) * e0[c] + w * e1[c] + 32) >> 6;
    }
    if (rot) {
        const uint32_t t = v[3];
        v[3] = v[rot - 1];
        v[rot - 1] = t;
    }
    return rgba_of(v[0], v[1], v[2], v[3]);
}

// blocks that mode 5 (colour of 7 bits, alpha of 8, 2-bit weights each) or mode 4 (colour of 5 bits and alpha of 6,
// one with 3-bit weights and the other 2-bit, either way round) holds exactly, alpha's weights apart from colour's, in
// each rotation: they come back exact in the mode they were made for, which needs both modes' layout, the rotation and
// index selection bits, and the swap that keeps each weight set's first texel under its top bit
static void exact_dual() {
    std::vector<glsl::leuint> texels(16 * 96);
    uint32_t s = 0x1B873593u, want[96];
    for (uint32_t b = 0; b < 96; b++) {
        const uint32_t kind = b / 4 % 3, rot = b % 4; // mode 5, mode 4 with isb 0, mode 4 with isb 1
        const uint32_t cb = kind ? 5 : 7, ab = kind ? 6 : 8, cbits = kind == 2 ? 3 : 2, abits = kind == 1 ? 3 : 2;
        uint32_t e0[4], e1[4];
        for (uint32_t c = 0; c < 4; c++) {
            const uint32_t bits = c < 3 ? cb : ab, mask = (1u << bits) - 1;
            const uint32_t v0 = xorshift(s) & mask, v1 = xorshift(s) & mask;
            e0[c] = bits == 8 ? v0 : (v0 << (8 - bits) | v0 >> (2 * bits - 8));
            e1[c] = bits == 8 ? v1 : (v1 << (8 - bits) | v1 >> (2 * bits - 8));
        }
        // every colour weight and every alpha weight in the block, neither a function of the other (all 16 pairs of
        // 2-bit ones), each set's order changed by a random xor and the texels' by a random step
        const uint32_t ctop = (1u << cbits) - 1, atop = (1u << abits) - 1, r = xorshift(s);
        for (uint32_t t = 0; t < 16; t++) {
            const uint32_t three = cbits == 3 || abits == 3, lo = t & (three ? 7 : 3), hi = (t >> (three ? 3 : 2)) + t;
            const uint32_t ci = ((abits == 3 ? hi : lo) ^ r) & ctop, ai = ((abits == 3 ? lo : hi) ^ r >> 4) & atop;
            const uint32_t at = (t * 5 + (r >> 8)) & 15;
            texels[(size_t)(at / 4) * 384 + b * 4 + at % 4] =
                dual_texel(e0, e1, bc7_weight_cpu(cbits, ci), bc7_weight_cpu(abits, ai), rot);
        }
        want[b] = (kind ? 4u : 5u) | rot << 4 | (kind == 2 ? 1u : 0u) << 6;
    }
    const auto blocks = encode_whole(texels, 384, 4);
    const auto got = decode(blocks.data(), 384, 4);
    known_modes(blocks, "exact dual", nullptr);
    for (uint32_t b = 0; b < 96; b++) {
        // the mode, then modes 4 and 5's rotation (2 bits) and mode 4's index selection bit
        const uint32_t first = uint32_t(blocks[b].x);
        uint32_t mode = 0;
        while (mode < 8 && !(first >> mode & 1)) mode++;
        const uint32_t dual = mode == 4 || mode == 5, rot = dual ? first >> (mode + 1) & 3 : 0;
        const uint32_t isb = mode == 4 ? first >> 7 & 1 : 0;
        if ((mode | rot << 4 | isb << 6) != want[b]) {
            std::fprintf(stderr,
                         "FAIL exact dual: block %u is mode %u rotation %u isb %u, not mode %u rotation %u isb %u\n", b,
                         mode, rot, isb, want[b] & 15, want[b] >> 4 & 3, want[b] >> 6);
            failures++;
            return;
        }
    }
    for (size_t i = 0; i < texels.size(); i++)
        for (uint32_t c = 0; c < 4; c++)
            if (got[i * 4 + c] != byte_of(texels[i], c)) {
                std::fprintf(stderr,
                             "FAIL exact dual: block %zu texel %zu channel %u is %u, not %u (block starts %08x)\n",
                             (i % 384) / 4, (i / 384) * 4 + i % 4, c, got[i * 4 + c], byte_of(texels[i], c),
                             uint32_t(blocks[(i % 384) / 4].x));
                failures++;
                return;
            }
    std::puts("PASS 96 blocks of modes 4 and 5, each rotation and index selection, come back exact");
}

// blocks of a colour gradient across with one channel apart (alpha, or red, green or blue in each rotation): a
// gradient down or noise, as ASTC's second plane makes. Modes 4 and 5 fit that channel on its own: the worst channel
// error over the blocks and their PSNR, against the same encoder without them, and held to what the encoder reached
// when it was written (gate values a little over and under)
static void channel_apart() {
    const uint32_t w = 256, h = 16; // 64 x 4 blocks
    std::vector<glsl::leuint> texels((size_t)w * h);
    uint32_t s = 0x85EBCA6Bu;
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++) {
            const uint32_t c0 = xorshift(s), c1 = xorshift(s), apart = (bx + by) % 4, noise = bx / 4 % 2;
            const uint32_t a0 = xorshift(s) & 0xFF, a1 = xorshift(s) & 0xFF;
            for (uint32_t t = 0; t < 16; t++) {
                const uint32_t x = t % 4, y = t / 4;
                uint32_t v = 0;
                for (uint32_t c = 0; c < 4; c++) {
                    uint32_t ch = ((3 - x) * (c0 >> (c * 8) & 0xFF) + x * (c1 >> (c * 8) & 0xFF) + 1) / 3;
                    if (c == apart) ch = noise ? xorshift(s) & 0xFF : ((3 - y) * a0 + y * a1 + 1) / 3;
                    v |= ch << (c * 8);
                }
                texels[(size_t)(by * 4 + y) * w + bx * 4 + x] = v;
            }
        }
    const auto blocks = encode_whole(texels, w, h);
    const auto got = decode(blocks.data(), w, h);
    const auto without = decode(encode_without_45(texels, w, h).data(), w, h);
    uint32_t modes[8] = {0}, worst = 0, worst_without = 0;
    known_modes(blocks, "channel apart", modes);
    for (size_t i = 0; i < texels.size(); i++)
        for (uint32_t c = 0; c < 4; c++) {
            const int v = byte_of(texels[i], c);
            worst = std::max(worst, uint32_t(std::abs(int(got[i * 4 + c]) - v)));
            worst_without = std::max(worst_without, uint32_t(std::abs(int(without[i * 4 + c]) - v)));
        }
    const double db = psnr(texels, got, w, 0, 0, w, h), db_without = psnr(texels, without, w, 0, 0, w, h);
    const bool pass = worst <= 24 && worst < worst_without && db >= 37.9 && db > db_without;
    std::printf("%s a channel apart: worst channel %u away and %.2f dB in modes 4 and 5 (%u blocks of 256), %u and "
                "%.2f dB without them; at most 24 and at least 37.9\n",
                pass ? "PASS" : "FAIL", worst, db, modes[4] + modes[5], worst_without, db_without);
    if (!pass) failures++;
}

// Decode independently, then sum each block's actual RGBA error. The bounded stop may replace an exact block by
// error<=16; a harder block must retain the prior search's quality, not a reference with the same pruning.
static void bounded_quality(const char* what, const std::vector<glsl::leuint>& texels, uint32_t w, uint32_t h) {
    const auto blocks = encode(texels, w, h, 1), whole = encode_whole(texels, w, h);
    const auto got = decode(blocks.data(), w, h), best = decode(whole.data(), w, h);
    known_modes(blocks, what, nullptr);
    uint32_t over = 0;
    uint64_t worst = 0;
    for (uint32_t by = 0; by < (h + 3) / 4; by++)
        for (uint32_t bx = 0; bx < (w + 3) / 4; bx++) {
            uint64_t e = 0, b = 0;
            for (uint32_t y = by * 4; y < std::min(by * 4 + 4, h); y++)
                for (uint32_t x = bx * 4; x < std::min(bx * 4 + 4, w); x++)
                    for (uint32_t c = 0; c < 4; c++) {
                        const int v = byte_of(texels[y * w + x], c);
                        const int d = v - got[(y * w + x) * 4 + c], f = v - best[(y * w + x) * 4 + c];
                        e += uint64_t(d * d);
                        b += uint64_t(f * f);
                    }
            if (e > 16 && e > b) {
                if (over < 3)
                    std::fprintf(stderr, "FAIL %s block(%u,%u): error %llu exceeds16 and prior search %llu\n",
                                 what, bx, by, (unsigned long long)e, (unsigned long long)b);
                over++;
            }
            if (e > worst) worst = e;
        }
    const double db = psnr(texels, got, w, 0, 0, w, h), db_whole = psnr(texels, best, w, 0, 0, w, h);
    std::printf("%s: %.2f dB, prior search %.2f, %u blocks past both limits, worst block %llu\n", what, db,
                db_whole, over, (unsigned long long)worst);
    if (over || db < db_whole - 1.0) {
        std::fprintf(stderr, "FAIL %s: %u blocks past both limits, %.2f dB against %.2f\n", what, over, db, db_whole);
        failures++;
    }
}
static void good_enough() {
    std::vector<glsl::leuint> texels(128 * 128);
    for (uint32_t y = 0; y < 128; y++)
        for (uint32_t x = 0; x < 128; x++) texels[(size_t)y * 128 + x] = picture(x, y);
    bounded_quality("good enough picture", texels, 128, 128);

    // Different content in each block: independent/correlated channels, alpha separated from color, clusters,
    // outliers and near-constant colors. This corpus does not use the encoder's partitions, weights or errors.
    const uint32_t w = 128, h = 32;
    std::vector<glsl::leuint> corpus(w * h);
    uint32_t random = 0x9e3779b9u;
    for (uint32_t by = 0; by < h / 4; by++)
        for (uint32_t bx = 0; bx < w / 4; bx++) {
            const uint32_t block = by * (w / 4) + bx;
            const uint32_t ends[3] = {xorshift(random), xorshift(random), xorshift(random)};
            for (uint32_t k = 0; k < 16; k++) {
                uint32_t value = 0, noise = xorshift(random);
                for (uint32_t c = 0; c < 4; c++) {
                    const int lo = int(ends[0] >> (c * 8) & 255), hi = int(ends[1] >> (c * 8) & 255);
                    const int t = int((k * 13 + block) & 15);
                    int component = int(noise >> (c * 8) & 255);
                    switch (block & 7) {
                        case 0: if (c == 3) component = 255; break;
                        case 1: component = lo + ((hi - lo) * t + 7) / 15; break;
                        case 2: component = lo + (int(k & 3) - 1) * (c == 3 ? 0 : 3); break;
                        case 3: if (c != 3) component = lo + ((hi - lo) * t + 7) / 15; break;
                        case 4: component = int(ends[(k + block) % 3] >> (c * 8) & 255); break;
                        case 5: component = k == (block & 15) ? hi : lo + int(k & 1); break;
                        case 6: component = lo + int(noise >> (c * 8) & 3) - 1; break;
                        default: break;
                    }
                    value |= clamp255(component) << (c * 8);
                }
                corpus[(by * 4 + k / 4) * w + bx * 4 + k % 4] = value;
            }
        }
    bounded_quality("good enough mixed blocks", corpus, w, h);
}

// A block whose alpha is uniformly255 or0 retains it, whatever the colors.
static void alpha_kept() {
    std::vector<glsl::leuint> texels(128 * 128);
    for (uint32_t y = 0; y < 128; y++)
        for (uint32_t x = 0; x < 128; x++) {
            const uint32_t rgb = picture(x, y) & 0xFFFFFFu;
            texels[(size_t)y * 128 + x] = rgb | (x < 64 ? 0xFF000000u : 0u);
        }
    const auto blocks = encode(texels, 128, 128, 1);
    const auto got = decode(blocks.data(), 128, 128);
    for (size_t i = 0; i < texels.size(); i++)
        if (got[i * 4 + 3] != byte_of(texels[i], 3)) {
            std::fprintf(stderr, "FAIL texel %zu: alpha %u, every alpha of its block %u\n", i, got[i * 4 + 3],
                         byte_of(texels[i], 3));
            failures++;
            return;
        }
    std::puts("PASS opaque blocks stay 255 and transparent ones 0");
}

// sizes that are not a multiple of 4 (the last blocks repeat the edge texels) and a strip of three slices in one
// dispatch: each slice's blocks are the ones it gets encoded alone
static void shapes() {
    const uint32_t w = 13, h = 7, depth = 3;
    std::vector<glsl::leuint> texels((size_t)w * h * depth);
    for (uint32_t z = 0; z < depth; z++)
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) texels[((size_t)z * h + y) * w + x] = picture(x * 9 + z * 40, y * 9 + 3);
    const auto strip = encode(texels, w, h, depth);
    const uint32_t per = ((w + 3) / 4) * ((h + 3) / 4);
    for (uint32_t z = 0; z < depth; z++) {
        std::vector<glsl::leuint> slice(texels.begin() + (ptrdiff_t)z * w * h,
                                        texels.begin() + (ptrdiff_t)(z + 1) * w * h);
        const auto alone = encode(slice, w, h, 1);
        if (std::memcmp(alone.data(), strip.data() + (size_t)z * per, per * 16) != 0) {
            std::fprintf(stderr, "FAIL slice %u of a strip differs from it encoded alone\n", z);
            failures++;
            return;
        }
    }
    known_modes(strip, "shapes", nullptr);
    std::puts("PASS a 13x7 image of 3 slices encodes slice by slice as each alone");
}

// The encoder's blocks for the picture, random texels with alpha and opaque ones, with and without modes 4 and 5,
// against a digest of the bytes the encoder wrote before it was laid out for the GPU's compiler (a partition read once
// a shape, each body called from one place in a loop): that layout changes no byte it writes
static uint64_t fnv1a(uint64_t h, const void* p, size_t n) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}
static void same_blocks() {
    std::vector<glsl::leuint> pic((size_t)128 * 128), any((size_t)64 * 64), opaque((size_t)64 * 64);
    for (uint32_t y = 0; y < 128; y++)
        for (uint32_t x = 0; x < 128; x++) pic[(size_t)y * 128 + x] = picture(x, y);
    uint32_t s = 0x2545F491u;
    for (auto& t : any) t = xorshift(s);
    for (auto& t : opaque) t = xorshift(s) | 0xFF000000u;
    const std::vector<glsl::leuvec4> runs[4] = {encode(pic, 128, 128, 1), encode(any, 64, 64, 1),
                                                encode(opaque, 64, 64, 1), encode_without_45(any, 64, 64)};
    uint64_t h = 0xCBF29CE484222325ull;
    for (const auto& r : runs) h = fnv1a(h, r.data(), r.size() * sizeof r[0]);
    const uint64_t want = 0xCF713CD3243FCE94ull;
    std::printf("%s the encoder's blocks: digest %016llx, %016llx expected\n", h == want ? "PASS" : "FAIL",
                (unsigned long long)h, (unsigned long long)want);
    if (h != want) failures++;
    const std::vector<glsl::leuvec4> prior[4] = {encode_whole(pic, 128, 128), encode_whole(any, 64, 64),
        encode_whole(opaque, 64, 64), encode_whole_without_45(any, 64, 64)};
    uint64_t prior_hash = 0xCBF29CE484222325ull;
    for (const auto& r : prior) prior_hash = fnv1a(prior_hash, r.data(), r.size() * sizeof r[0]);
    const uint64_t prior_want = 0x97987257CCF58CADull; // The committed digest before the search optimizations.
    std::printf("%s prior search blocks: digest %016llx, %016llx expected\n", prior_hash == prior_want ? "PASS" : "FAIL",
                (unsigned long long)prior_hash, (unsigned long long)prior_want);
    if (prior_hash != prior_want) failures++;
}

int main() {
    tables();
    weights();
    least_squares();
    constant_blocks();
    exact_lines();
    partitioned("two colours on each two subset partition, in mode 1", two_subsets, opaque1, 1);
    partitioned("two colours on each two subset partition, in mode 3", two_subsets, opaque2, 3);
    partitioned("three colours on each three subset partition, in mode 2", three_subsets, opaque3, 2);
    partitioned("two colours with alpha on each two subset partition, in mode 7", two_subsets, alpha2, 7);
    exact_dual();
    channel_apart();
    picture_psnr();
    good_enough();
    alpha_kept();
    shapes();
    same_blocks();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
