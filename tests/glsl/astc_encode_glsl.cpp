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

// The blocks made with no texel decoded (astc_direct.glsl) are not measured as they are made, so they are held here
// to what the BCn blocks themselves decode to: random BC7 blocks of mode 6, of mode 5 with one alpha and no rotation,
// and BC1 blocks of four colours, every one of which is such a block. A channel may differ by what the layout costs:
// nothing where the ends and weights are the block's own (mode 5); for BC1 its thirds, which the reference decodes as
// the Switch's GPU rounds them, up to 4 from the 21 and 43 of 64 that ASTC has; and for mode 6 an end's step (192 values, or 48 with alpha) and a 64th of the line (ASTC's 4 bit weights are
// BC7's within 1, and 3 bit ones their top bits)
static void direct_blocks() {
    static const struct {
        const char* name;
        int kind; // bcn.h's: 0 BC1, 9 BC7
        uint32_t most;
    } cases[3] = {{"BC7 mode 6", 9, 8}, {"BC7 mode 5, one alpha, no rotation", 9, 0}, {"BC1, four colours", 0, 4}};
    for (int c = 0; c < 3; c++) {
        const uint32_t n = 4096, bb = cases[c].kind == 0 ? 8 : 16;
        std::vector<uint8_t> src((size_t)n * bb), astc((size_t)n * 16), was((size_t)n * 64), is((size_t)n * 64);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t w[4] = {random_word(), random_word(), random_word(), random_word()};
            if (c == 0) {
                w[0] = (w[0] & ~0x7Fu) | 0x40u;
                if (i & 1) w[1] |= 0xFFFE0000u, w[2] |= 1u; // opaque: both alpha ends and their p-bits all ones
            } else if (c == 1) {
                w[0] = (w[0] & ~0xFFu) | 0x20u; // mode 5, rotation 0
                // alpha's ends, 8 bits each from bits 50 and 58, the same: 255 for every other block
                const uint32_t a = i & 1 ? 255u : w[3] & 255u;
                w[1] = (w[1] & ~0xFFFC0000u) | (a << 18) | ((a & 0x3Fu) << 26);
                w[2] = (w[2] & ~3u) | (a >> 6);
            } else if ((w[0] & 0xFFFFu) <= (w[0] >> 16)) {
                w[0] = (w[0] >> 16) | (w[0] << 16);
                if ((w[0] & 0xFFFFu) == (w[0] >> 16)) w[0] ^= 1u;
            }
            for (uint32_t k = 0; k < bb; k++) src[(size_t)i * bb + k] = uint8_t(w[k / 4] >> (k % 4 * 8));
        }
        bcn_astc_cpu(cases[c].kind, src.data(), n, 1, 1, astc.data());
        bcn_decode_image(src.data(), n * 4, 4, cases[c].kind, was.data());
        astc_decode_image(astc.data(), n * 4, 4, 4, 4, is.data(), 0);
        uint32_t most = 0;
        double sum = 0;
        for (size_t k = 0; k < was.size(); k++) {
            const uint32_t d = uint32_t(std::abs(int(was[k]) - int(is[k])));
            most = d > most ? d : most;
            sum += d;
        }
        const bool ok = most <= cases[c].most;
        std::printf("%s %s, 4096 blocks made ASTC with no texel decoded: a channel %u off at most (%u allowed), %.3f "
                    "on average\n",
                    ok ? "PASS" : "FAIL", cases[c].name, most, cases[c].most, sum / double(was.size()));
        if (!ok) failures++;
    }
}

// astc_quant.glsl works the colour encodings out where a table would do, so every answer it has is held here to the
// decoder's rules (src/nx/gpu/texture/astc_decode.glsl's DecodeSequence, ColorParams and UnquantizeTQ, written out
// again below) turned round by search: for each encoding and each colour a value it holds, none nearer, under a
// code that decodes to it, and for every five trits and three quints bits that decode to them,
// with nothing set in the bits a shorter last group lacks.
static uint32_t field(uint32_t v, uint32_t off, uint32_t n) {
    return (v >> off) & ((1u << n) - 1u);
}
static void trits_of(uint32_t T, uint32_t t[5]) {
    const bool c7 = field(T, 2, 3) == 7, h3 = field(T, 5, 2) == 3;
    const uint32_t C = c7 ? (field(T, 5, 3) << 2) | field(T, 0, 2) : field(T, 0, 5);
    t[4] = c7 || h3 ? 2 : field(T, 7, 1);
    t[3] = c7 ? 2 : h3 ? field(T, 7, 1) : field(T, 5, 2);
    const bool l3 = field(C, 0, 2) == 3, m3 = field(C, 2, 2) == 3;
    t[2] = l3 || m3 ? 2 : field(C, 4, 1);
    t[1] = l3 ? field(C, 4, 1) : m3 ? 2 : field(C, 2, 2);
    t[0] = l3 ? (field(C, 3, 1) << 1) | (field(C, 2, 1) & ~field(C, 3, 1) & 1)
              : m3 ? field(C, 0, 2) : (field(C, 1, 1) << 1) | (field(C, 0, 1) & ~field(C, 1, 1) & 1);
}
static void quints_of(uint32_t Q, uint32_t q[3]) {
    const bool high = field(Q, 1, 2) == 3, both4 = high && field(Q, 5, 2) == 0;
    const uint32_t C = high ? (field(Q, 3, 2) << 3) | ((~field(Q, 5, 2) & 3) << 1) | field(Q, 0, 1) : field(Q, 0, 5);
    const bool five = field(C, 0, 3) == 5;
    q[0] = both4 ? 4 : five ? field(C, 3, 2) : field(C, 0, 3);
    q[1] = both4 || five ? 4 : field(C, 3, 2);
    const uint32_t n0 = ~field(Q, 0, 1) & 1;
    q[2] = both4 ? (field(Q, 0, 1) << 2) | ((field(Q, 4, 1) & n0) << 1) | (field(Q, 3, 1) & n0)
                 : high ? 4 : field(Q, 5, 2);
}
// a trit or quint d over b bits v, to 8 bits
static uint32_t unquant(bool trit, uint32_t b, uint32_t v, uint32_t d) {
    const uint32_t i = b - 1, bmask = ((1u << b) - 1) >> 1;
    uint32_t c, mb, sb;
    if (trit) {
        c = field(b <= 4 ? 0x162C5DCCu : 0x050Bu, (i & 3) * 8, 8);
        mb = field(b <= 2 ? 0x01160000u : b <= 4 ? 0x00410085u : 0x01010081u, (i & 1) * 16, 16);
        sb = field(0x420000u, (i & 7) * 4, 4);
    } else {
        c = field(b <= 4 ? 0x0D1A3671u : 0x06u, (i & 3) * 8, 8);
        mb = field(b <= 2 ? 0x010C0000u : b <= 4 ? 0x00810105u : 0x0101u, (i & 1) * 16, 16);
        sb = field(0x31100u, (i & 7) * 4, 4);
    }
    const uint32_t A = v & 1 ? 0x1FFu : 0, B = (((v >> 1) & bmask) * mb) >> sb;
    return (A & 0x80) | (((d * c + B) ^ A) >> 2);
}
static void quant_formulas() {
    for (uint32_t range = 0; range < 6; range++) {
        const bool trit = !(range & 1);
        const uint32_t b = 6 - ((range + 1) >> 1), digits = trit ? 3 : 5;
        bool held[256] = {};
        for (uint32_t d = 0; d < digits; d++)
            for (uint32_t v = 0; v < 1u << b; v++) held[unquant(trit, b, v, d)] = true;
        for (uint32_t t = 0; t < 256; t++) {
            int least = 256;
            for (int u = 0; u < 256; u++)
                if (held[u] && std::abs(u - (int)t) < least) least = std::abs(u - (int)t);
            const uint32_t got = astc_quant_cpu(range, t), code = got >> 8, v = got & 255;
            if (std::abs((int)v - (int)t) != least || code >> b >= digits ||
                unquant(trit, b, code & ((1u << b) - 1), code >> b) != v) {
                std::fprintf(stderr, "encoding %u, colour %u: value %u under code 0x%X, the nearest held is %d away\n",
                             range, t, v, code, least);
                failures++;
            }
        }
    }
    for (uint32_t i = 0; i < 243; i++) {
        const uint32_t t[5] = {i % 3, i / 3 % 3, i / 9 % 3, i / 27 % 3, i / 81};
        uint32_t back[5], n = 5;
        const uint32_t T = astc_trit_bits_cpu(t);
        trits_of(T & 255, back);
        while (n > 1 && t[n - 1] == 0) n--; // a group of n values keeps ceil(8 n / 5) bits
        if (T > 255 || std::memcmp(t, back, sizeof back) || T >> ((8 * n + 4) / 5)) {
            std::fprintf(stderr, "trits %u %u %u %u %u: bits 0x%X\n", t[0], t[1], t[2], t[3], t[4], T);
            failures++;
        }
    }
    for (uint32_t i = 0; i < 125; i++) {
        const uint32_t q[3] = {i % 5, i / 5 % 5, i / 25};
        uint32_t back[3], n = 3;
        const uint32_t Q = astc_quint_bits_cpu(q);
        quints_of(Q & 127, back);
        while (n > 1 && q[n - 1] == 0) n--; // a group of n values keeps ceil(7 n / 3) bits
        if (Q > 127 || std::memcmp(q, back, sizeof back) || Q >> ((7 * n + 2) / 3)) {
            std::fprintf(stderr, "quints %u %u %u: bits 0x%X\n", q[0], q[1], q[2], Q);
            failures++;
        }
    }
}

// The transcoder's three passes (the second decodes with no arrays, bcn_regs.glsl) against the one invocation that
// does it all: the same blocks, for random blocks of each kind (a random BC7 block is of every mode) and for the BC7
// encoder's blocks of the picture below, many of which their own lines hold, so that the second pass writes them.
static uint32_t picture(uint32_t x, uint32_t y);
static void passes_case(const char* what, int kind, const std::vector<uint8_t>& in, uint32_t n, bool settles) {
    std::vector<uint8_t> one((size_t)n * 16), three((size_t)n * 16);
    uint32_t listed[2], differ = 0;
    bcn_astc_cpu(kind, in.data(), n, 1, 1, one.data());
    bcn_astc_passes_cpu(kind, in.data(), n, three.data(), listed);
    for (uint32_t k = 0; k < n; k++) differ += std::memcmp(&one[(size_t)k * 16], &three[(size_t)k * 16], 16) != 0;
    const bool ok = !differ && listed[0] && (!settles || listed[1] < listed[0]);
    std::printf("%s %s, %u blocks in three passes as in one: %u differ; the second pass took %u and wrote %u\n",
                ok ? "PASS" : "FAIL", what, n, differ, listed[0], listed[0] - listed[1]);
    if (!ok) failures++;
}
static void passes() {
    static const char* const names[4] = {"random BC1", "random BC2", "random BC3", "random BC7"};
    static const int kinds[4] = {0, 1, 2, 9};
    for (int j = 0; j < 4; j++) {
        const uint32_t n = 4096;
        std::vector<uint8_t> in((size_t)n * (kinds[j] == 0 ? 8 : 16));
        for (auto& b : in) b = (uint8_t)(random_word() >> 24);
        passes_case(names[j], kinds[j], in, n, false);
    }
    const uint32_t w = 128, h = 128;
    std::vector<uint8_t> texels((size_t)w * h * 4), bc7((size_t)w * h);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t c = picture(x, y);
            std::memcpy(&texels[((size_t)y * w + x) * 4], &c, 4);
        }
    bc7_encode_cpu(texels.data(), w, h, 1, bc7.data());
    passes_case("the picture's BC7", 9, bc7, w * h / 16, true);
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
        std::printf("  decoded (no ASTC layout as they stand): %u of %u blocks; of BC7's modes 0 to 7", listed, cx * cy);
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
    quant_formulas();
    passes();
    blocks();
    direct_blocks();
    picture_psnr();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("The ASTC encoder's blocks decode to what it worked out, and hold the picture");
    return 0;
}
