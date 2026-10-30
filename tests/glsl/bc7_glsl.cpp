// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BC7 encoder's body (src/nx/gpu/texture/bc7_encode.glsl) run on the CPU through tests/glsl/glsl.hpp, its blocks
// decoded by the CPU reference (tests/support/bcn.c): a block of one colour comes back exact, so does a line of
// two endpoints the format holds, and a picture of gradients, hard edges, noise and alpha comes back no worse than
// the PSNR each region is held to; sizes that are not a multiple of 4 and strips of several slices encode as vk/astc.c
// dispatches them. No GPU is needed.
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

// one slice's blocks decoded back to RGBA8 bytes, w x h
static std::vector<uint8_t> decode(const glsl::leuvec4* blocks, uint32_t w, uint32_t h) {
    std::vector<uint8_t> out((size_t)w * h * 4);
    bcn_decode_image(reinterpret_cast<const uint8_t*>(blocks), w, h, BCN_BC7, out.data());
    return out;
}

static uint8_t byte_of(const glsl::leuint& word, uint32_t c) { return uint8_t(uint32_t(word) >> (c * 8)); }

// every block is one of the modes the encoder writes (1, 2, 3, 6 or 7), and how many of each, for the picture's line
static bool known_modes(const std::vector<glsl::leuvec4>& blocks, const char* what, uint32_t count[8]) {
    for (size_t i = 0; i < blocks.size(); i++) {
        const uint32_t first = uint32_t(blocks[i].x) & 0xFFu;
        uint32_t mode = 0;
        while (mode < 8 && !(first >> mode & 1)) mode++;
        if (mode == 8 || mode == 0 || mode == 4 || mode == 5) {
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
    const auto blocks = encode(texels, 32, 4, 1);
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
    const auto blocks = encode(texels, 256, 4, 1);
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
    uint32_t modes[8] = {0};
    known_modes(blocks, "picture", modes);
    std::printf("picture modes: 1 %u, 2 %u, 3 %u, 6 %u, 7 %u\n", modes[1], modes[2], modes[3], modes[6], modes[7]);
    static const struct {
        const char* name;
        uint32_t x0, y0;
        double floor;
    } regions[4] = {{"gradients", 0, 0, 47.0}, {"edges", 64, 0, 40.0}, {"noise", 0, 64, 27.0}, {"alpha", 64, 64, 38.5}};
    for (const auto& r : regions) {
        const double db = psnr(texels, got, 128, r.x0, r.y0, r.x0 + 64, r.y0 + 64);
        std::printf("%s %s: %.2f dB, at least %.1f\n", db >= r.floor ? "PASS" : "FAIL", r.name, db, r.floor);
        if (db < r.floor) failures++;
    }
}

// a block whose every alpha is 255 comes back with every alpha 255, and one whose every alpha is 0 with every alpha 0,
// whatever its colours: the p-bit a mode 6 end shares with its colours would otherwise make 254 or 1 of them where
// that is nearer for the colours
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

int main() {
    tables();
    weights();
    constant_blocks();
    exact_lines();
    partitioned("two colours on each two subset partition, in mode 1", two_subsets, opaque1, 1);
    partitioned("two colours on each two subset partition, in mode 3", two_subsets, opaque2, 3);
    partitioned("three colours on each three subset partition, in mode 2", three_subsets, opaque3, 2);
    partitioned("two colours with alpha on each two subset partition, in mode 7", two_subsets, alpha2, 7);
    picture_psnr();
    alpha_kept();
    shapes();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
