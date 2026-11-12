// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The ASTC 4x4 encoder (src/nx/gpu/texture/astc_encode.glsl) run on the CPU (astc_encode_cpu.cpp), its blocks decoded
// by the CPU's ASTC decoder (tests/support/astc.c): no block is one the decoder refuses, each decodes to the squared
// error the encoder worked out for it (so every layout's bits, colour encoding and weights are where the decoder reads
// them), blocks of one colour and blocks a layout holds come back exact, and a picture of gradients, edges, noise and
// alpha comes back no worse than each region is held to. No GPU is needed.
//
// snx-test-astc-encode-glsl --measure FILE.astc... gives the PSNR of a game's own textures (astc_dump's files) made
// ASTC 4x4 from their texels and, as the runtime does, from BC7 blocks of them, with the BCn block's hints and without.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include "astc_encode_cpu.h"
#include "bc7_cpu.h"
#include "bcn_etc2_cpu.h"
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

// --measure: each file's first slice decoded, and its texels made ASTC 4x4 three ways: by the encoder alone, and as
// the runtime makes them, from BC7 blocks (the BC7 encoder's fast build of the texels), the BCn block's hints taken and
// not. The PSNR of each against the texels, and of the two from BC7 against what the BC7 blocks decode to, which is
// what the game would have shown; the CPU's seconds for the two from BC7; and how the hinted blocks were written
static int measure(int n, char** files) {
    double se[5] = {0, 0, 0, 0, 0}, secs[2] = {0, 0}, values = 0;
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
        const size_t padded = (size_t)cx * cy * 64;
        std::vector<uint8_t> texels((size_t)w * h * 4), bc7((size_t)cx * cy * 16), blocks(bc7.size()), got(padded),
            shown(padded);
        astc_decode_image(src.data(), w, h, bw, bh, texels.data(), 0);
        bc7_encode_cpu_fast(texels.data(), w, h, 1, bc7.data());
        bcn_decode_image(bc7.data(), cx * 4, cy * 4, 9 /* BCN_BC7 */, shown.data());
        double e[5] = {0, 0, 0, 0, 0}, s[2] = {0, 0};
        uint32_t kinds[4] = {0, 0, 0, 0};
        for (int which = 0; which < 3; which++) {
            timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            if (which == 0)
                astc_encode_cpu(texels.data(), w, h, 1, blocks.data());
            else if (which == 1)
                bcn_astc_plain_cpu(9, bc7.data(), cx, cy, 1, blocks.data());
            else
                bcn_astc_cpu(9, bc7.data(), cx, cy, 1, blocks.data());
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (which) s[which - 1] = double(t1.tv_sec - t0.tv_sec) + double(t1.tv_nsec - t0.tv_nsec) / 1e9;
            astc_decode_image(blocks.data(), cx * 4, cy * 4, 4, 4, got.data(), 0);
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w * 4; x++) {
                    const size_t at = (size_t)y * cx * 16 + x;
                    const double d = double(got[at]) - double(texels[(size_t)y * w * 4 + x]);
                    e[which] += d * d;
                    if (which) {
                        const double d2 = double(got[at]) - double(shown[at]);
                        e[2 + which] += d2 * d2;
                    }
                }
            if (which == 2)
                for (size_t i = 0; i < blocks.size(); i += 16) {
                    const uint32_t m = blocks[i] | blocks[i + 1] << 8;
                    kinds[(m & 0x1FF) == 0x1FC ? 0 : (m >> 10 & 1) ? 2 : (m >> 11 & 3) ? 3 : 1]++;
                }
        }
        uint32_t left[8], had[8] = {0};
        const uint32_t listed = bcn_astc_left_cpu(bc7.data(), cx * cy, left);
        for (size_t i = 0; i < bc7.size(); i += 16) {
            uint32_t m = 0;
            while (m < 7 && !(bc7[i] >> m & 1)) m++;
            had[m]++;
        }
        std::printf("  the second pass's: %u of %u blocks; of BC7's modes 0 to 7", listed, cx * cy);
        for (int m = 0; m < 8; m++) std::printf(" %u/%u", left[m], had[m]);
        std::printf("\n");
        const double count = double(w) * h * 4;
        values += count;
        for (int k = 0; k < 5; k++) se[k] += e[k];
        secs[0] += s[0];
        secs[1] += s[1];
#define DB(x) ((x) > 0 ? 10 * std::log10(255.0 * 255.0 * count / (x)) : 99.0)
        std::printf("%s: %ux%u of %ux%u: from texels %.2f dB; from BC7 %.2f dB (%.2f to BC7's own) in %.3f s, with "
                    "hints %.2f dB (%.2f) in %.3f s: %u void extents, %u of one partition, %u of two planes, %u of two "
                    "partitions\n",
                    files[f], w, h, bw, bh, DB(e[0]), DB(e[1]), DB(e[3]), s[0], DB(e[2]), DB(e[4]), s[1], kinds[0],
                    kinds[1], kinds[2], kinds[3]);
#undef DB
        std::fflush(stdout);
    }
#define DB(x) (10 * std::log10(255.0 * 255.0 * values / (x)))
    std::printf("all %d files: from texels %.2f dB; from BC7 %.2f dB (%.2f to BC7's own) in %.2f s, with hints %.2f dB "
                "(%.2f) in %.2f s\n",
                n, DB(se[0]), DB(se[1]), DB(se[3]), secs[0], DB(se[2]), DB(se[4]), secs[1]);
#undef DB
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
