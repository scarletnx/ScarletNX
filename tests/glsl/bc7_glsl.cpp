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

// every block is mode 6 (its first seven bits 0000001), the only one the encoder writes
static bool all_mode6(const std::vector<glsl::leuvec4>& blocks, const char* what) {
    for (size_t i = 0; i < blocks.size(); i++)
        if ((uint32_t(blocks[i].x) & 0x7Fu) != 0x40u) {
            std::fprintf(stderr, "FAIL %s: block %zu starts %08x, not mode 6\n", what, i, uint32_t(blocks[i].x));
            failures++;
            return false;
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

// the encoder's weights are the format's (the Khronos Data Format Specification's BPTC weight table for 4-bit
// indices): one off, and every error and fit it works out is off, while blocks still decode
static void weights() {
    static const uint32_t table[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
    for (uint32_t i = 0; i < 16; i++)
        if (bc7_weight_cpu(i) != table[i]) {
            std::fprintf(stderr, "FAIL weight %u is %u, the format's %u\n", i, bc7_weight_cpu(i), table[i]);
            failures++;
            return;
        }
    std::puts("PASS the 16 weights are the format's");
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
    all_mode6(blocks, "constant blocks");
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

// texels that are exactly what mode 6 interpolates between two ends it holds (even channels, p-bits nought), the
// sixteen weights each once in a scrambled order: the encoder finds those ends and weights again
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
            e0[c] = s & 0xFE;
            e1[c] = (s >> 8) & 0xFE;
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
    all_mode6(blocks, "exact lines");
    const double db = psnr(texels, got, 32, 0, 0, 32, 4);
    if (db < 99.0) {
        std::fprintf(stderr, "FAIL exact lines: %.2f dB, not exact\n", db);
        failures++;
        return;
    }
    std::puts("PASS 8 blocks that lie on a line mode 6 holds come back exact");
}

// the picture, region by region, held to what the encoder reached when it was written (gate values a little under)
static void picture_psnr() {
    std::vector<glsl::leuint> texels(128 * 128);
    for (uint32_t y = 0; y < 128; y++)
        for (uint32_t x = 0; x < 128; x++) texels[(size_t)y * 128 + x] = picture(x, y);
    const auto blocks = encode(texels, 128, 128, 1);
    const auto got = decode(blocks.data(), 128, 128);
    all_mode6(blocks, "picture");
    static const struct {
        const char* name;
        uint32_t x0, y0;
        double floor;
    } regions[4] = {{"gradients", 0, 0, 42.5}, {"edges", 64, 0, 23.0}, {"noise", 0, 64, 24.5}, {"alpha", 64, 64, 37.0}};
    for (const auto& r : regions) {
        const double db = psnr(texels, got, 128, r.x0, r.y0, r.x0 + 64, r.y0 + 64);
        std::printf("%s %s: %.2f dB, at least %.1f\n", db >= r.floor ? "PASS" : "FAIL", r.name, db, r.floor);
        if (db < r.floor) failures++;
    }
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
    all_mode6(strip, "shapes");
    std::puts("PASS a 13x7 image of 3 slices encodes slice by slice as each alone");
}

int main() {
    weights();
    constant_blocks();
    exact_lines();
    picture_psnr();
    shapes();
    if (failures) std::fprintf(stderr, "%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
