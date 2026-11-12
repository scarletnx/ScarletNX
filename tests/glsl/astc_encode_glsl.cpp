// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The ASTC 4x4 encoder (src/nx/gpu/texture/astc_encode.glsl) run on the CPU (astc_encode_cpu.cpp), its blocks decoded
// by the CPU's ASTC decoder (tests/support/astc.c): no block is one the decoder refuses, each decodes to the squared
// error the encoder worked out for it (so every layout's bits, colour encoding and weights are where the decoder reads
// them), blocks of one colour and blocks a layout holds come back exact, and a picture of gradients, edges, noise and
// alpha comes back no worse than each region is held to. No GPU is needed.
//
// snx-test-astc-encode-glsl --measure FILE.astc... gives the PSNR of a game's own textures (astc_dump's files, decoded
// and encoded again) beside the BC7 encoder's fast build of the same texels.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "astc_encode_cpu.h"
#include "bc7_cpu.h"
extern "C" void astc_decode_image(const uint8_t* src, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint8_t* dst,
                                  int srgb);
extern "C" void bcn_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);

static int failures;
// each region of the picture, half a dB under what the encoder reached when it was written
#define FLOOR_GRADIENTS 41.7
#define FLOOR_EDGES 33.3
#define FLOOR_NOISE 18.4
#define FLOOR_ALPHA 40.5
static uint32_t rng = 0x9E3779B9u;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static uint32_t sse(const uint8_t* a, const uint8_t* b, size_t n) {
    uint32_t e = 0;
    for (size_t i = 0; i < n; i++) e += uint32_t((int(a[i]) - int(b[i])) * (int(a[i]) - int(b[i])));
    return e;
}

// a block's texels as kind says: 0 random, 1 two colours across an edge, 2 a gradient, 3 one colour, 4 a gradient
// whose alpha goes its own way, 5 random with alpha 255; alpha 255 too for the odd kinds of 1 and 2
static void block_new(uint32_t kind, uint32_t n, uint32_t px[16]) {
    const uint32_t a = random_word(), b = random_word(), opaque = (n & 1) || kind == 5 ? 0xFF000000u : 0u;
    for (uint32_t k = 0; k < 16; k++) {
        const uint32_t x = k & 3, y = k >> 2;
        uint32_t c = 0;
        if (kind == 0 || kind == 5) {
            c = random_word();
        } else if (kind == 1) {
            c = (x * (a & 3) + y * (a >> 2 & 3) > (a >> 4 & 7)) ? a : b;
        } else if (kind == 3) {
            c = a;
        } else {
            for (uint32_t ch = 0; ch < 4; ch++) {
                const uint32_t lo = a >> (8 * ch) & 255, hi = b >> (8 * ch) & 255;
                const uint32_t t = kind == 4 && ch == 3 ? (3 - x) * 5 : x * 3 + y * 2;
                c |= ((lo * (15 - t) + hi * t + 7) / 15) << (8 * ch);
            }
        }
        px[k] = c | opaque;
    }
}

// every block decodes, to the error the encoder says
static void blocks() {
    uint32_t modes[4] = {0, 0, 0, 0}, exact = 0;
    for (uint32_t n = 0; n < 6000; n++) {
        uint32_t px[16];
        uint8_t block[16], got[64], want[64];
        block_new(n % 6, n / 6, px);
        const uint32_t err = astc_encode_block_cpu(px, block);
        for (int k = 0; k < 16; k++)
            for (int c = 0; c < 4; c++) want[k * 4 + c] = uint8_t(px[k] >> (8 * c));
        astc_decode_image(block, 4, 4, 4, 4, got, 0);
        const uint32_t e = sse(got, want, 64);
        bool magenta = true;
        for (int k = 0; k < 16; k++)
            magenta = magenta && got[k * 4] == 255 && got[k * 4 + 1] == 0 && got[k * 4 + 2] == 255 && got[k * 4 + 3] == 255;
        bool want_magenta = true;
        for (int k = 0; k < 16; k++) want_magenta = want_magenta && px[k] == 0xFFFF00FFu;
        if (e != err || (magenta && !want_magenta)) {
            std::fprintf(stderr, "FAIL block %u (kind %u): the decoder's error %u, the encoder's %u%s (block %02x %02x)\n",
                         n, n % 6, e, err, magenta ? ", an error block" : "", block[0], block[1]);
            failures++;
            return;
        }
        if (n % 6 == 3 && e != 0) {
            std::fprintf(stderr, "FAIL block %u of one colour comes back with an error of %u\n", n, e);
            failures++;
            return;
        }
        exact += e == 0;
        // void extent, two planes (bit 10), two partitions (bits 11 and 12), or one
        const uint32_t m = block[0] | block[1] << 8;
        modes[(m & 0x1FF) == 0x1FC ? 0 : (m >> 10 & 1) ? 2 : (m >> 11 & 3) ? 3 : 1]++;
    }
    std::printf("PASS 6000 blocks decode to the error the encoder worked out: %u void extents, %u of one partition, %u "
                "of two planes, %u of two partitions; %u exact\n",
                modes[0], modes[1], modes[2], modes[3], exact);
    if (!modes[0] || !modes[1] || !modes[2] || !modes[3]) {
        std::fprintf(stderr, "FAIL a kind of block was never written\n");
        failures++;
    }
}

static double psnr(const uint8_t* a, const uint8_t* b, size_t n) {
    double e = 0;
    for (size_t i = 0; i < n; i++) e += double(int(a[i]) - int(b[i])) * double(int(a[i]) - int(b[i]));
    return e > 0 ? 10 * std::log10(255.0 * 255.0 * double(n) / e) : 99.0;
}

// a 128x128 picture: gradients, edges of two colours, noise, and gradients with alpha, a quarter each
static uint32_t picture(uint32_t x, uint32_t y) {
    const uint32_t qx = x / 64, qy = y / 64, u = x % 64, v = y % 64;
    if (qx == 0 && qy == 0) return 0xFF000000u | (u * 4) | ((v * 4) << 8) | (((u + v) * 2) << 16);
    if (qx == 1 && qy == 0) return ((u * 3 + v * 5) / 37 % 2) ? 0xFF2040E0u : ((u / 9 + v / 7) % 2 ? 0xFFE0C020u : 0xFF103010u);
    if (qx == 0 && qy == 1) return 0xFF000000u | ((x * 2654435761u ^ y * 40503u) >> 8 & 0xFFFFFFu);
    return (u * 4) | ((255 - v * 4) << 8) | (128u << 16) | ((((u ^ v) & 32) ? u * 4 : 255 - v * 3) << 24);
}
static void picture_psnr() {
    const uint32_t w = 128, h = 128;
    std::vector<uint8_t> texels((size_t)w * h * 4), blocks((size_t)w * h), got(texels.size());
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t c = picture(x, y);
            std::memcpy(&texels[((size_t)y * w + x) * 4], &c, 4);
        }
    astc_encode_cpu(texels.data(), w, h, 1, blocks.data());
    astc_decode_image(blocks.data(), w, h, 4, 4, got.data(), 0);
    static const char* const names[4] = {"gradients", "edges", "noise", "alpha"};
    // what the encoder reached when it was written, half a dB under
    static const double floors[4] = {FLOOR_GRADIENTS, FLOOR_EDGES, FLOOR_NOISE, FLOOR_ALPHA};
    for (uint32_t q = 0; q < 4; q++) {
        std::vector<uint8_t> a, b;
        for (uint32_t y = 0; y < 64; y++)
            for (uint32_t x = 0; x < 64 * 4; x++) {
                const size_t at = ((size_t)(q / 2 * 64 + y) * w + q % 2 * 64) * 4 + x;
                a.push_back(texels[at]);
                b.push_back(got[at]);
            }
        const double d = psnr(a.data(), b.data(), a.size());
        std::printf("%s picture %s: %.2f dB (at least %.1f)\n", d >= floors[q] ? "PASS" : "FAIL", names[q], d, floors[q]);
        if (d < floors[q]) failures++;
    }
}

static int measure(int n, char** files) {
    double se[2] = {0, 0}, values = 0;
    for (int f = 0; f < n; f++) {
        FILE* file = std::fopen(files[f], "rb");
        uint8_t hdr[16];
        if (!file || std::fread(hdr, 1, 16, file) != 16) {
            std::fprintf(stderr, "cannot read %s\n", files[f]);
            return 1;
        }
        const uint32_t bw = hdr[4], bh = hdr[5], w = hdr[7] | hdr[8] << 8 | hdr[9] << 16,
                       h = hdr[10] | hdr[11] << 8 | hdr[12] << 16;
        const uint32_t nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh, cx = (w + 3) / 4, cy = (h + 3) / 4;
        std::vector<uint8_t> src((size_t)nx * ny * 16);
        if (bw < 4 || bw > 12 || bh < 4 || bh > 12 || !w || !h ||
            std::fread(src.data(), 16, (size_t)nx * ny, file) != (size_t)nx * ny) {
            std::fprintf(stderr, "%s is no .astc file of whole blocks\n", files[f]);
            return 1;
        }
        std::fclose(file);
        std::vector<uint8_t> texels((size_t)w * h * 4), blocks((size_t)cx * cy * 16), got((size_t)cx * cy * 64);
        astc_decode_image(src.data(), w, h, bw, bh, texels.data(), 0);
        double e[2] = {0, 0};
        for (int which = 0; which < 2; which++) {
            if (which == 0) {
                astc_encode_cpu(texels.data(), w, h, 1, blocks.data());
                astc_decode_image(blocks.data(), cx * 4, cy * 4, 4, 4, got.data(), 0);
            } else {
                bc7_encode_cpu_fast(texels.data(), w, h, 1, blocks.data());
                bcn_decode_image(blocks.data(), cx * 4, cy * 4, 9 /* BCN_BC7 */, got.data());
            }
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w * 4; x++) {
                    const double d = double(got[(size_t)y * cx * 16 + x]) - double(texels[(size_t)y * w * 4 + x]);
                    e[which] += d * d;
                }
            se[which] += e[which];
        }
        const double count = double(w) * h * 4;
        values += count;
        std::printf("%s: %ux%u of %ux%u: ASTC 4x4 %.2f dB, BC7 (the fast build) %.2f dB\n", files[f], w, h, bw, bh,
                    e[0] > 0 ? 10 * std::log10(255.0 * 255.0 * count / e[0]) : 99.0,
                    e[1] > 0 ? 10 * std::log10(255.0 * 255.0 * count / e[1]) : 99.0);
        std::fflush(stdout);
    }
    std::printf("all %d files: ASTC 4x4 %.2f dB, BC7 %.2f dB\n", n, 10 * std::log10(255.0 * 255.0 * values / se[0]),
                10 * std::log10(255.0 * 255.0 * values / se[1]));
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && !std::strcmp(argv[1], "--measure")) return measure(argc - 2, argv + 2);
    blocks();
    picture_psnr();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("The ASTC encoder's blocks decode to what it worked out, and hold the picture");
    return 0;
}
