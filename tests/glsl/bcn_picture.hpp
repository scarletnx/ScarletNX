// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// What the tests of the BCn transcoders (bcn_etc2_glsl.cpp, bcn_astc_glsl.cpp) share: simple BCn encoders, which the
// pictures they hold a transcoder to are made with, the test picture and the PSNR. Static, a copy a test.
#ifndef SNX_TESTS_BCN_PICTURE_HPP
#define SNX_TESTS_BCN_PICTURE_HPP
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "bc7_cpu.h"
// tests/support/bcn.h's (its header brings the recompiler's C helpers, which C++ warns about)
extern "C" void bcn_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
enum { BC1, BC2, BC3, BC4, BC4S, BC5, BC5S, BC6U, BC6S, BC7 }; // bcn.h's order

static uint32_t rng = 0x9e3779b9u;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static uint32_t rgba(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 8 | b << 16 | a << 24; }
static uint32_t ch(uint32_t p, int c) { return p >> (8 * c) & 255; }
static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static uint32_t block_bytes(int kind) { return kind == BC1 || kind == BC4 || kind == BC4S ? 8 : 16; }
static const char* kind_name(int kind) {
    static const char* names[10] = {"BC1", "BC2", "BC3", "BC4", "BC4 snorm", "BC5", "BC5 snorm", "BC6H", "BC6H", "BC7"};
    return names[kind];
}

// ---- simple BCn encoders, what the test pictures are made of (a game's are better; the transcoder sees blocks)

// a BC1 colour block of the texels, transparent (alpha < 128) ones as the three colour mode's index 3 when alpha
// says so: ends the extremes of the texels along their principal axis, quantized to 565
static void bc1_colour(const uint32_t px[16], bool alpha, uint8_t out[8]) {
    double mean[3] = {0, 0, 0};
    int n = 0;
    bool clear[16];
    for (int i = 0; i < 16; i++) {
        clear[i] = alpha && ch(px[i], 3) < 128;
        if (clear[i]) continue;
        for (int c = 0; c < 3; c++) mean[c] += ch(px[i], c);
        n++;
    }
    std::memset(out, 0, 8);
    if (n == 0) {            // every texel transparent: three colours, index 3 throughout
        out[2] = out[3] = 0; // c0 = c1 = 0 is the three colour mode
        std::memset(out + 4, 0xFF, 4);
        return;
    }
    for (int c = 0; c < 3; c++) mean[c] /= n;
    double cov[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 16; i++) {
        if (clear[i]) continue;
        double d[3] = {ch(px[i], 0) - mean[0], ch(px[i], 1) - mean[1], ch(px[i], 2) - mean[2]};
        cov[0] += d[0] * d[0], cov[1] += d[0] * d[1], cov[2] += d[0] * d[2];
        cov[3] += d[1] * d[1], cov[4] += d[1] * d[2], cov[5] += d[2] * d[2];
    }
    double ax[3] = {1, 1, 1};
    for (int k = 0; k < 8; k++) {
        double v[3] = {cov[0] * ax[0] + cov[1] * ax[1] + cov[2] * ax[2],
                       cov[1] * ax[0] + cov[3] * ax[1] + cov[4] * ax[2],
                       cov[2] * ax[0] + cov[4] * ax[1] + cov[5] * ax[2]};
        double m = std::max(std::fabs(v[0]), std::max(std::fabs(v[1]), std::fabs(v[2])));
        if (m < 1e-9) break;
        for (int c = 0; c < 3; c++) ax[c] = v[c] / m;
    }
    double lo = 1e9, hi = -1e9;
    for (int i = 0; i < 16; i++) {
        if (clear[i]) continue;
        double t =
            (ch(px[i], 0) - mean[0]) * ax[0] + (ch(px[i], 1) - mean[1]) * ax[1] + (ch(px[i], 2) - mean[2]) * ax[2];
        lo = std::min(lo, t), hi = std::max(hi, t);
    }
    double norm = ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2];
    uint32_t e[2];
    for (int k = 0; k < 2; k++) {
        double t = (k ? lo : hi) / norm;
        int r = clamp255((int)std::lround(mean[0] + ax[0] * t)), g = clamp255((int)std::lround(mean[1] + ax[1] * t));
        int b = clamp255((int)std::lround(mean[2] + ax[2] * t));
        e[k] = (uint32_t)((r * 31 + 127) / 255) << 11 | (uint32_t)((g * 63 + 127) / 255) << 5 |
               (uint32_t)((b * 31 + 127) / 255);
    }
    bool three = false;
    for (int i = 0; i < 16; i++) three = three || clear[i];
    // four colours want the first end the larger, three the smaller
    if (three ? e[0] > e[1] : e[0] < e[1]) std::swap(e[0], e[1]);
    if (!three && e[0] == e[1]) { // one colour: every index 0
        out[0] = (uint8_t)e[0], out[1] = (uint8_t)(e[0] >> 8), out[2] = (uint8_t)e[1], out[3] = (uint8_t)(e[1] >> 8);
        return;
    }
    out[0] = (uint8_t)e[0], out[1] = (uint8_t)(e[0] >> 8), out[2] = (uint8_t)e[1], out[3] = (uint8_t)(e[1] >> 8);
    // the palette as bcn.c decodes it, and each texel's nearest
    uint8_t pal[16 * 4];
    for (uint32_t k = 0; k < 4; k++) {
        uint8_t one[8];
        std::memcpy(one, out, 4);
        uint32_t idx = 0;
        for (int i = 0; i < 16; i++) idx |= k << (2 * i);
        for (int j = 0; j < 4; j++) one[4 + j] = (uint8_t)(idx >> (8 * j));
        uint8_t dec[64];
        bcn_decode_image(one, 4, 4, BC1, dec);
        std::memcpy(pal + 4 * k, dec, 4);
    }
    uint32_t idx = 0;
    for (int i = 0; i < 16; i++) {
        uint32_t best = 0, err = ~0u;
        for (uint32_t k = 0; k < 4; k++) {
            if (three && k == 3) continue;
            uint32_t e2 = 0;
            for (int c = 0; c < 3; c++) {
                int d = (int)pal[4 * k + c] - (int)ch(px[i], c);
                e2 += (uint32_t)(d * d);
            }
            if (e2 < err) err = e2, best = k;
        }
        idx |= (clear[i] ? 3u : best) << (2 * i);
    }
    for (int j = 0; j < 4; j++) out[4 + j] = (uint8_t)(idx >> (8 * j));
}
// a BC4 block of the values (0 to 255, or -127 to 127 signed): the extremes as ends, eight values between
static void bc4_values(const int v[16], bool sn, uint8_t out[8]) {
    int lo = v[0], hi = v[0];
    for (int i = 1; i < 16; i++) lo = std::min(lo, v[i]), hi = std::max(hi, v[i]);
    std::memset(out, 0, 8);
    out[0] = (uint8_t)hi, out[1] = (uint8_t)lo;
    if (hi == lo) return;
    uint64_t idx = 0;
    for (int i = 0; i < 16; i++) {
        uint64_t best = 0;
        double err = 1e9;
        for (int k = 0; k < 8; k++) {
            double p = k == 0 ? hi : k == 1 ? lo : ((8 - k) * hi + (k - 1) * lo) / 7.0;
            if (std::fabs(p - v[i]) < err) err = std::fabs(p - v[i]), best = (uint64_t)k;
        }
        idx |= best << (3 * i);
    }
    (void)sn;
    for (int j = 0; j < 6; j++) out[2 + j] = (uint8_t)(idx >> (8 * j));
}
// one block of kind from sixteen texels: BC4 and BC5 from red and green (snorm: centred on 128)
static void bc_block(int kind, const uint32_t px[16], uint8_t* out) {
    int v[16];
    switch (kind) {
        case BC1: bc1_colour(px, true, out); return;
        case BC2:
            std::memset(out, 0, 8);
            for (int i = 0; i < 16; i++) out[i / 2] |= (uint8_t)(((ch(px[i], 3) * 15 + 127) / 255) << (4 * (i & 1)));
            bc1_colour(px, false, out + 8);
            return;
        case BC3:
            for (int i = 0; i < 16; i++) v[i] = (int)ch(px[i], 3);
            bc4_values(v, false, out);
            bc1_colour(px, false, out + 8);
            return;
        case BC4:
        case BC4S:
        case BC5:
        case BC5S: {
            bool sn = kind == BC4S || kind == BC5S;
            for (int c = 0; c < (kind == BC5 || kind == BC5S ? 2 : 1); c++) {
                for (int i = 0; i < 16; i++) v[i] = sn ? std::max((int)ch(px[i], c) - 128, -127) : (int)ch(px[i], c);
                bc4_values(v, sn, out + 8 * c);
            }
            return;
        }
    }
}
// a w x h picture as blocks of kind, rows of blocks after rows
static std::vector<uint8_t> bc_image(int kind, const std::vector<uint32_t>& pic, uint32_t w, uint32_t h) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, bb = block_bytes(kind);
    std::vector<uint8_t> blocks((size_t)bw * bh * bb);
    if (kind == BC7) {
        bc7_encode_cpu(reinterpret_cast<const uint8_t*>(pic.data()), w, h, 1, blocks.data());
        return blocks;
    }
    for (uint32_t by = 0; by < bh; by++)
        for (uint32_t bx = 0; bx < bw; bx++) {
            uint32_t px[16];
            for (uint32_t i = 0; i < 16; i++) {
                uint32_t x = std::min(bx * 4 + (i & 3), w - 1), y = std::min(by * 4 + (i >> 2), h - 1);
                px[i] = pic[(size_t)y * w + x];
            }
            bc_block(kind, px, &blocks[((size_t)by * bw + bx) * bb]);
        }
    return blocks;
}

// ---- measures

// PSNR in dB of b against a (n samples of bytes bytes each, little-endian; snorm: signed), the peak the format's
static double psnr(const uint8_t* a, const uint8_t* b, size_t n, int bytes, bool sn) {
    double sum = 0, peak = bytes == 1 ? 255.0 : sn ? 65534.0 : 65535.0;
    for (size_t i = 0; i < n; i++) {
        int x, y;
        if (bytes == 1) {
            x = a[i], y = b[i];
        } else {
            x = a[2 * i] | a[2 * i + 1] << 8, y = b[2 * i] | b[2 * i + 1] << 8;
            if (sn) x = (int16_t)x, y = (int16_t)y;
        }
        sum += double(x - y) * double(x - y);
    }
    double mse = sum / double(n);
    return mse == 0 ? 99.0 : 10.0 * std::log10(peak * peak / mse);
}

// the test picture: gradients, hard edges, noise and alpha, a 64x64 region each
static uint32_t picture(uint32_t x, uint32_t y) {
    uint32_t u = x & 63, v = y & 63;
    if (y < 64 && x < 64) // smooth gradients
        return rgba(u * 4, v * 4, (u + v) * 2, 255);
    if (y < 64) { // two and three colour shapes with hard edges
        int dx = (int)u - 32, dy = (int)v - 32;
        bool disc = dx * dx + dy * dy < 400, stripe = (u / 6 + v / 9) % 2;
        return disc ? rgba(220, 40, 30, 255) : stripe ? rgba(30, 90, 200, 255) : rgba(240, 230, 120, 255);
    }
    if (x < 64) { // noise
        uint32_t s = (u * 73856093u) ^ (v * 19349663u);
        s ^= s >> 13, s *= 0x5bd1e995u, s ^= s >> 15;
        return rgba(s & 255, s >> 8 & 255, s >> 16 & 255, 255);
    }
    // see-through: alpha a soft ramp in the top half, a hard shape below, over a gradient
    uint32_t a = v < 32 ? (uint32_t)clamp255((int)(8 * v + 2 * u)) : (u * u + 3 * v * v) % 23 < 12 ? 255u : 0u;
    return rgba((uint32_t)clamp255(255 - 3 * (int)u), 128 + v, 4 * u, a);
}
#endif
