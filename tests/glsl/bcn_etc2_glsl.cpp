// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BCn to ETC2 and EAC transcoder (texconv.glsl's ETC2 build, etc2_encode.glsl) run on the CPU through
// tests/glsl/glsl.hpp, its blocks decoded by the CPU references (tests/support/etc2.c, bcn.c): every candidate mode
// stores the block it means (the reference decodes it in that mode, to the error the encoder worked out); a block of
// one 4 bit colour, a gradient the planar mode holds, an EAC block of every table and an alpha the same throughout come
// back exact, and so do most random ETC1 blocks and most gradients the decoder clamps; a BC1 block's transparent
// texels stay transparent and no opaque one turns transparent; images whose sides aren't multiples of 4, of several
// slices, transcode block for block as one block alone does; and a picture of gradients, hard edges, noise and alpha,
// made BC1 to BC7 by simple encoders below, comes back from ETC2 and EAC no worse than the PSNR each kind is held to
// (against the BCn blocks decoded, which is what the GPU showed before). No GPU is needed.
//
// snx-test-bcn-etc2-glsl --image FILE W H measures a picture of one's own (W x H RGBA8 bytes) the same way: each kind's
// PSNR against the BCn decoded, and, with --dump DIR, the decoded BCn and transcoded pictures as raw RGBA8 files.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "bcn_etc2_cpu.h"
#include "bc7_cpu.h"
// tests/support/etc2.h's and bcn.h's (their headers bring the recompiler's C helpers, which C++ warns about)
extern "C" void etc2_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
extern "C" void bcn_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
enum { BC1, BC2, BC3, BC4, BC4S, BC5, BC5S, BC6U, BC6S, BC7 };                        // bcn.h's order
enum { ETC2_RGB, ETC2_RGB_PTA, ETC2_RGBA, EAC_R, EAC_R_SNORM, EAC_RG, EAC_RG_SNORM }; // etc2.h's

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
// the ETC2 or EAC kind a BCn kind becomes
static int etc_kind(int kind) {
    switch (kind) {
        case BC1: return ETC2_RGB_PTA;
        case BC4: return EAC_R;
        case BC4S: return EAC_R_SNORM;
        case BC5: return EAC_RG;
        case BC5S: return EAC_RG_SNORM;
    }
    return ETC2_RGBA;
}
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
struct Measure {
    double rgb, alpha; // RGBA kinds: colour and alpha apart; BC4 and BC5: rgb is the channels'
};
// kind's blocks of a w x h picture transcoded, against them decoded: the PSNR; the decoded pictures into dec_bc and
// dec_etc (RGBA8, or the 16 bit channels)
static Measure measure(int kind, const std::vector<uint8_t>& blocks, uint32_t w, uint32_t h,
                       std::vector<uint8_t>* dec_bc, std::vector<uint8_t>* dec_etc) {
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    std::vector<uint8_t> etc(blocks.size());
    bcn_etc2_cpu(kind, blocks.data(), bw, bh, 1, etc.data());
    bool chans = kind >= BC4 && kind <= BC5S, sn = kind == BC4S || kind == BC5S;
    uint32_t pb = !chans ? 4 : kind == BC4 || kind == BC4S ? 2 : 4;
    std::vector<uint8_t> a((size_t)w * h * pb), b((size_t)w * h * pb);
    bcn_decode_image(blocks.data(), w, h, kind, a.data());
    etc2_decode_image(etc.data(), w, h, etc_kind(kind), b.data());
    Measure m;
    if (chans) {
        m.rgb = psnr(a.data(), b.data(), (size_t)w * h * pb / 2, 2, sn);
        m.alpha = 0;
    } else {
        std::vector<uint8_t> ca, cb, aa, ab;
        for (size_t i = 0; i < (size_t)w * h; i++) {
            for (int c = 0; c < 3; c++) ca.push_back(a[4 * i + c]), cb.push_back(b[4 * i + c]);
            aa.push_back(a[4 * i + 3]), ab.push_back(b[4 * i + 3]);
        }
        m.rgb = psnr(ca.data(), cb.data(), ca.size(), 1, false);
        m.alpha = psnr(aa.data(), ab.data(), aa.size(), 1, false);
    }
    if (dec_bc) *dec_bc = a;
    if (dec_etc) *dec_etc = b;
    return m;
}

// ---- the checks

// the mode an ETC2 RGB block (as stored) is in, by the specification's overflow rules: 0 individual, 1 differential,
// 2 T, 3 H, 4 planar
static int etc2_mode(const uint8_t b[8], bool punch) {
    if (!punch && !(b[3] & 2)) return 0;
    int r = b[0] >> 3, dr = (b[0] & 7) ^ 4, g = b[1] >> 3, dg = (b[1] & 7) ^ 4, bl = b[2] >> 3, db = (b[2] & 7) ^ 4;
    if (r + dr - 4 < 0 || r + dr - 4 > 31) return 2;
    if (g + dg - 4 < 0 || g + dg - 4 > 31) return 3;
    if (bl + db - 4 < 0 || bl + db - 4 > 31) return 4;
    return 1;
}
// the squared error of an ETC2 RGB block (as stored) against the texels, decoded by the reference
static uint32_t block_error(const uint8_t b[8], const uint32_t px[16], bool punch, bool* clear_ok) {
    uint8_t dec[64];
    etc2_decode_image(b, 4, 4, punch ? ETC2_RGB_PTA : ETC2_RGB, dec);
    uint32_t e = 0;
    *clear_ok = true;
    for (int i = 0; i < 16; i++) {
        bool want_clear = punch && ch(px[i], 3) < 128, got_clear = punch && dec[4 * i + 3] == 0;
        if (want_clear != got_clear) *clear_ok = false;
        if (want_clear) continue;
        for (int c = 0; c < 3; c++) {
            int d = (int)dec[4 * i + c] - (int)ch(px[i], c);
            e += (uint32_t)(d * d);
        }
    }
    return e;
}
// a block of random texels of up to four colours, some of them transparent (punch), as the candidates meet them
static void random_block(uint32_t px[16], bool punch, int colours) {
    uint32_t pal[4];
    for (int k = 0; k < 4; k++) pal[k] = random_word() | 0xFF000000u;
    for (int i = 0; i < 16; i++) {
        uint32_t p = pal[random_word() % (uint32_t)colours];
        int noise = (int)(random_word() % 9) - 4;
        p = rgba((uint32_t)clamp255((int)ch(p, 0) + noise), (uint32_t)clamp255((int)ch(p, 1) + noise),
                 (uint32_t)clamp255((int)ch(p, 2) - noise), 255);
        if (punch && random_word() % 5 == 0) p &= 0x00FFFFFFu;
        px[i] = p;
    }
}
// every candidate stores the mode it means, the reference decodes it to the error the encoder worked out, and the
// block the encoder writes is the best of those it tries; a see-through block's transparent texels and only those
// decode transparent
static void candidates() {
    static const int mode_of[8] = {0, 0, 1, 1, 3, 2, 2, 4};
    uint32_t seen[5] = {0, 0, 0, 0, 0};
    for (int n = 0; n < 3000; n++) {
        bool punch = n % 3 == 2;
        uint32_t px[16];
        random_block(px, punch, 1 + n % 4);
        bool see = false;
        for (int i = 0; i < 16; i++) see = see || (punch && ch(px[i], 3) < 128);
        uint32_t least = ~0u;
        int best_mode = 0;
        for (uint32_t c = 0; c < 8; c++) {
            if ((punch && c < 2) || (see && c == 7)) continue;
            uint8_t b[8];
            uint32_t scored = etc2_candidate_cpu(px, punch, c, b);
            bool clear_ok;
            uint32_t e = block_error(b, px, punch, &clear_ok);
            int mode = etc2_mode(b, punch);
            CHECK(mode == mode_of[c], "block %d candidate %u stores mode %d, not %d", n, c, mode, mode_of[c]);
            CHECK(e == scored, "block %d candidate %u: the reference's error %u, the encoder's %u", n, c, e, scored);
            CHECK(clear_ok, "block %d candidate %u: transparent texels don't come back as they were", n, c);
            CHECK(!punch || (b[3] & 2) == (see ? 0 : 2),
                  "block %d candidate %u: opaque bit %d with%s transparent texels", n, c, b[3] >> 1 & 1,
                  see ? "" : "out");
            if (e < least && etc2_tries_cpu(px, punch, c)) least = e, best_mode = mode;
        }
        seen[best_mode]++;
        uint8_t b[8];
        etc2_rgb_cpu(px, punch, b);
        bool clear_ok;
        uint32_t e = block_error(b, px, punch, &clear_ok);
        CHECK(e == least && clear_ok, "block %d: the encoder wrote an error %u, its best candidate's is %u", n, e,
              least);
    }
    // every mode wins some of these blocks: one that never does is a candidate that never helps
    for (int m = 0; m < 5; m++) CHECK(seen[m] > 0, "mode %d is never the best of 3000 blocks", m);
    std::printf("candidates: 3000 blocks, best modes individual %u, differential %u, T %u, H %u, planar %u\n", seen[0],
                seen[1], seen[2], seen[3], seen[4]);
}
// blocks ETC2 holds come back exact: one 4 bit colour (T's lone colour), a planar gradient
static void exact_blocks() {
    for (int n = 0; n < 200; n++) {
        uint32_t c = random_word();
        uint32_t px[16];
        uint32_t col = rgba((c & 15) * 17, (c >> 4 & 15) * 17, (c >> 8 & 15) * 17, 255);
        for (int i = 0; i < 16; i++) px[i] = col;
        uint8_t b[8];
        bool clear_ok;
        etc2_rgb_cpu(px, n & 1, b);
        CHECK(block_error(b, px, n & 1, &clear_ok) == 0, "one colour %06x isn't exact", col & 0xFFFFFF);
        // a gradient of three corners of 6, 7 and 6 bits, as the planar mode interpolates them, none of its texels
        // past 0 or 255 (the corner at (3, 3) lies beyond the three: one the decoder clamps can leave the fit a
        // little off, the levels nearest the clamped texels anything that clamps them)
        int o[3], hz[3], vt[3];
        bool clamps = false;
        for (int k = 0; k < 3; k++) {
            int top = k == 1 ? 127 : 63, bits = k == 1 ? 7 : 6;
            int q[3] = {(int)(random_word() % (uint32_t)(top + 1)), (int)(random_word() % (uint32_t)(top + 1)),
                        (int)(random_word() % (uint32_t)(top + 1))};
            o[k] = q[0] << (8 - bits) | q[0] >> (2 * bits - 8);
            hz[k] = q[1] << (8 - bits) | q[1] >> (2 * bits - 8);
            vt[k] = q[2] << (8 - bits) | q[2] >> (2 * bits - 8);
        }
        for (int i = 0; i < 16; i++) {
            int x = i & 3, y = i >> 2, v[3];
            for (int k = 0; k < 3; k++) {
                v[k] = (x * (hz[k] - o[k]) + y * (vt[k] - o[k]) + 4 * o[k] + 2) >> 2;
                clamps = clamps || v[k] != clamp255(v[k]);
            }
            px[i] = rgba((uint32_t)clamp255(v[0]), (uint32_t)clamp255(v[1]), (uint32_t)clamp255(v[2]), 255);
        }
        if (clamps) {
            n--;
            continue;
        }
        etc2_rgb_cpu(px, 0, b);
        CHECK(block_error(b, px, false, &clear_ok) == 0, "gradient %d isn't exact", n);
    }
    // an alpha the same throughout, any of them, is exact (an opaque block's 255 most often)
    for (int a = 0; a < 256; a++) {
        int32_t v[16];
        for (int i = 0; i < 16; i++) v[i] = a;
        uint8_t blk[16] = {0}, dec[64];
        eac_cpu(0, v, blk);
        etc2_decode_image(blk, 4, 4, ETC2_RGBA, dec);
        bool same = true;
        for (int i = 0; i < 16; i++) same = same && dec[4 * i + 3] == a;
        CHECK(same, "alpha %d throughout isn't exact", a);
    }
    // EAC: every table's values at a multiplier, around the middle, come back exact, in each kind
    static const int unit[3] = {1, 8, 8}, off[3] = {0, 4, 0}, base[3] = {128, 128, 0};
    for (int kind = 0; kind < 3; kind++)
        for (int t = 0; t < 16; t++)
            for (int m = kind ? 0 : 1; m < 16; m += 3) {
                static const int tab[16][8] = {
                    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
                    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
                    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
                    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
                    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
                    {-3, -5, -7, -9, 2, 4, 6, 8}};
                int32_t v[16];
                bool inside = true;
                for (int i = 0; i < 16; i++) {
                    int mod = tab[t][i % 8];
                    v[i] = base[kind] * unit[kind] + off[kind] + (m ? mod * m * unit[kind] : mod);
                    // values the decoder would clamp aren't the table's
                    inside = inside && v[i] >= (kind == 2 ? -1023 : 0) &&
                             v[i] <= (kind == 0   ? 255
                                      : kind == 1 ? 2047
                                                  : 1023);
                }
                if (!inside) continue;
                uint8_t b[8], dec[64];
                eac_cpu(kind, v, b);
                bool same = true;
                if (kind == 0) {
                    uint8_t blk[16] = {0};
                    std::memcpy(blk, b, 8);
                    etc2_decode_image(blk, 4, 4, ETC2_RGBA, dec);
                    for (int i = 0; i < 16; i++) same = same && dec[4 * i + 3] == v[i];
                } else {
                    etc2_decode_image(b, 4, 4, kind == 1 ? EAC_R : EAC_R_SNORM, dec);
                    for (int i = 0; i < 16; i++) {
                        int got = dec[2 * i] | dec[2 * i + 1] << 8;
                        // 11 bits widened to 16 as the decoder widens them
                        int a = v[i] < 0 ? -v[i] : v[i];
                        int want =
                            kind == 1 ? (v[i] << 5 | v[i] >> 6) : ((a << 5 | a >> 5) * (v[i] < 0 ? -1 : 1)) & 0xFFFF;
                        same = same && got == want;
                    }
                }
                CHECK(same, "EAC kind %d table %d multiplier %d isn't exact", kind, t, m);
            }
}
// random ETC1 blocks (individual and differential, both flips) decoded come back exact most of the time: the base each
// subblock's indices want, not its texels' mean (a table's modifiers aren't used evenly), is what makes them exact.
// Blocks the decoder doesn't clamp: base colours from 64 to 191 and the four smaller tables (modifiers up to 42)
static void etc1_blocks() {
    uint32_t exact = 0, n = 2000;
    for (uint32_t k = 0; k < n; k++) {
        uint8_t b[8], dec[64];
        for (int i = 0; i < 8; i++) b[i] = (uint8_t)random_word();
        b[3] &= 0x93; // tables 0 to 3: their top bits (7 and 4) clear
        if (k & 1) {  // differential: 5 bit colours 8 to 23 and a difference keeping the second there
            b[3] |= 2;
            for (int c = 0; c < 3; c++) {
                int base = 8 + (b[c] >> 3) % 16, d = (int)(b[c] & 7) - ((b[c] & 4) ? 8 : 0);
                if (base + d < 8 || base + d > 23) d = 0;
                b[c] = (uint8_t)(base << 3 | (d & 7));
            }
        } else { // individual: 4 bit colours 4 to 11
            b[3] &= (uint8_t)~2;
            for (int c = 0; c < 3; c++) b[c] = (uint8_t)((4 + (b[c] >> 4) % 8) << 4 | (4 + (b[c] & 15) % 8));
        }
        etc2_decode_image(b, 4, 4, ETC2_RGB, dec);
        uint32_t px[16];
        for (int i = 0; i < 16; i++) px[i] = rgba(dec[4 * i], dec[4 * i + 1], dec[4 * i + 2], 255);
        uint8_t got[8];
        bool clear_ok;
        etc2_rgb_cpu(px, 0, got);
        exact += block_error(got, px, false, &clear_ok) == 0;
    }
    // 1639 of 2000 when it was written (1699 with the individual mode tried where the differential one holds the
    // subblocks too); 744 with each subblock's base its texels' mean
    std::printf("ETC1 blocks: %u of %u come back exact\n", exact, n);
    CHECK(exact >= 1600, "%u of %u random ETC1 blocks come back exact, at least 1600 should", exact, n);
}
// gradients the decoder clamps (their corner at (3, 3) past 0 or 255) still come back exact from the planar mode
// most of the time: its levels walk from the least squares fit, which the clamped texels pull off by more than one
static void clamped_gradients() {
    uint32_t exact = 0, n = 0;
    while (n < 500) {
        int o[3], hz[3], vt[3];
        for (int k = 0; k < 3; k++) {
            int top = k == 1 ? 127 : 63, bits = k == 1 ? 7 : 6, q[3];
            for (int j = 0; j < 3; j++) q[j] = (int)(random_word() % (uint32_t)(top + 1));
            o[k] = q[0] << (8 - bits) | q[0] >> (2 * bits - 8);
            hz[k] = q[1] << (8 - bits) | q[1] >> (2 * bits - 8);
            vt[k] = q[2] << (8 - bits) | q[2] >> (2 * bits - 8);
        }
        uint32_t px[16];
        bool clamps = false;
        for (int i = 0; i < 16; i++) {
            int x = i & 3, y = i >> 2, v[3];
            for (int k = 0; k < 3; k++) {
                v[k] = (x * (hz[k] - o[k]) + y * (vt[k] - o[k]) + 4 * o[k] + 2) >> 2;
                clamps = clamps || v[k] != clamp255(v[k]);
            }
            px[i] = rgba((uint32_t)clamp255(v[0]), (uint32_t)clamp255(v[1]), (uint32_t)clamp255(v[2]), 255);
        }
        if (!clamps) continue;
        uint8_t b[8];
        exact += etc2_candidate_cpu(px, 0, 7, b) == 0;
        n++;
    }
    // 422 of 500 when it was written; 241 in 500 others with one walk of the 27 levels around the fit
    std::printf("clamped gradients: %u of %u come back exact\n", exact, n);
    CHECK(exact >= 400, "%u of %u clamped gradients come back exact from the planar mode, at least 400 should", exact,
          n);
}
// BC1's transparent texels stay transparent and its opaque ones opaque, in random three colour blocks
static void bc1_alpha() {
    const uint32_t n = 4096;
    std::vector<uint8_t> blocks(n * 8), etc(n * 8);
    for (uint32_t i = 0; i < n; i++) {
        uint16_t c0 = (uint16_t)random_word(), c1 = (uint16_t)random_word();
        if (i % 2 && c0 > c1) std::swap(c0, c1); // half three colour blocks
        uint32_t idx = random_word();
        uint8_t* b = &blocks[i * 8];
        b[0] = (uint8_t)c0, b[1] = (uint8_t)(c0 >> 8), b[2] = (uint8_t)c1, b[3] = (uint8_t)(c1 >> 8);
        for (int j = 0; j < 4; j++) b[4 + j] = (uint8_t)(idx >> (8 * j));
    }
    bcn_etc2_cpu(BC1, blocks.data(), 64, n / 64, 1, etc.data());
    std::vector<uint8_t> a(n * 64), b(n * 64);
    bcn_decode_image(blocks.data(), 256, n / 16, BC1, a.data());
    etc2_decode_image(etc.data(), 256, n / 16, ETC2_RGB_PTA, b.data());
    uint32_t wrong = 0, clear = 0;
    for (size_t i = 0; i < (size_t)n * 16; i++) {
        clear += a[4 * i + 3] == 0;
        if ((a[4 * i + 3] == 0) != (b[4 * i + 3] == 0) ||
            (b[4 * i + 3] == 0 && (b[4 * i] | b[4 * i + 1] | b[4 * i + 2])))
            wrong++;
    }
    CHECK(wrong == 0 && clear > 0, "BC1: %u of %u texels change transparency (%u transparent)", wrong, n * 16, clear);
}
// an image whose sides aren't multiples of 4, of three slices, transcodes block for block as each block alone: the
// dispatch reads and writes the blocks it should
static void layout() {
    const uint32_t bw = 67, bh = 5, slices = 3;
    for (int kind : {BC1, BC3, BC4S, BC5, BC7}) {
        uint32_t bb = block_bytes(kind), n = bw * bh * slices;
        std::vector<uint8_t> blocks(n * bb), etc(n * bb);
        for (auto& x : blocks) x = (uint8_t)random_word();
        if (kind == BC7)
            for (uint32_t i = 0; i < n; i++) blocks[i * 16] |= 0x40; // mode 6 at least, never a block without a mode
        bcn_etc2_cpu(kind, blocks.data(), bw, bh, slices, etc.data());
        for (uint32_t i = 0; i < n; i++) {
            uint8_t one[16];
            bcn_etc2_cpu(kind, &blocks[i * bb], 1, 1, 1, one);
            CHECK(!std::memcmp(one, &etc[i * bb], bb), "%s block %u of a %ux%ux%u image differs from it alone",
                  kind_name(kind), i, bw, bh, slices);
        }
    }
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
// each kind's PSNR on each region of the picture (colour, and alpha in the see-through region), held to what the
// encoder reached when it was written, half a dB under (BC1's alpha exact). The noise region is what neither format
// holds well: BCn's own blocks of it are already far from the picture
static void picture_psnr() {
    static const char* regions[4] = {"gradients", "edges", "noise", "see-through"};
    static const struct {
        int kind;
        double rgb[4], alpha[4];
    } floors[] = {{BC1, {45.4, 36.1, 21.3, 43.7}, {0, 0, 0, 99.0}}, {BC2, {45.4, 36.3, 21.3, 46.7}, {0, 0, 0, 51.6}},
                  {BC3, {45.4, 36.3, 21.3, 46.7}, {0, 0, 0, 49.2}}, {BC4, {54.2, 59.1, 36.7, 55.7}, {0, 0, 0, 0}},
                  {BC4S, {54.6, 53.8, 36.4, 54.4}, {0, 0, 0, 0}},   {BC5, {54.2, 52.3, 36.6, 58.6}, {0, 0, 0, 0}},
                  {BC5S, {54.6, 50.1, 36.4, 57.2}, {0, 0, 0, 0}},   {BC7, {46.8, 32.5, 15.5, 51.7}, {0, 0, 0, 54.8}}};
    for (const auto& f : floors) {
        bool pass = true;
        std::string line;
        char part[96];
        for (int r = 0; r < 4; r++) {
            std::vector<uint32_t> pic(64 * 64);
            for (uint32_t y = 0; y < 64; y++)
                for (uint32_t x = 0; x < 64; x++) pic[y * 64 + x] = picture((r & 1) * 64 + x, (r >> 1) * 64 + y);
            Measure m = measure(f.kind, bc_image(f.kind, pic, 64, 64), 64, 64, nullptr, nullptr);
            pass = pass && m.rgb >= f.rgb[r] && m.alpha >= f.alpha[r];
            std::snprintf(part, sizeof part, "%s%s %.2f (%.1f)", r ? ", " : "", regions[r], m.rgb, f.rgb[r]);
            line += part;
            if (f.alpha[r] > 0) {
                std::snprintf(part, sizeof part, " alpha %.2f (%.1f)", m.alpha, f.alpha[r]);
                line += part;
            }
        }
        std::printf("%s picture %s to %s, dB (at least): %s\n", pass ? "PASS" : "FAIL", kind_name(f.kind),
                    f.kind == BC1                     ? "ETC2 RGB8A1"
                    : f.kind >= BC4 && f.kind <= BC5S ? "EAC"
                                                      : "ETC2 RGBA8",
                    line.c_str());
        if (!pass) failures++;
    }
}

// --image FILE W H [--dump DIR]: a picture of one's own measured as the test picture is
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
    for (int kind : {BC1, BC2, BC3, BC4, BC5, BC7}) {
        std::vector<uint8_t> a, b;
        Measure m = measure(kind, bc_image(kind, pic, w, h), w, h, &a, &b);
        std::printf("%s %s: %.2f dB", file, kind_name(kind), m.rgb);
        if (kind < BC4 || kind == BC7) std::printf(", alpha %.2f dB", m.alpha);
        std::printf("\n");
        if (dump) {
            for (int which = 0; which < 2; which++) {
                std::string path = std::string(dump) + "/" + kind_name(kind) + (which ? ".etc2.raw" : ".bcn.raw");
                FILE* o = std::fopen(path.c_str(), "wb");
                if (!o) {
                    std::printf("cannot write %s\n", path.c_str());
                    return 2;
                }
                const auto& v = which ? b : a;
                std::fwrite(v.data(), 1, v.size(), o);
                std::fclose(o);
            }
        }
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 5 && !std::strcmp(argv[1], "--image"))
        return image(argv[2], (uint32_t)std::atoi(argv[3]), (uint32_t)std::atoi(argv[4]),
                     argc >= 7 && !std::strcmp(argv[5], "--dump") ? argv[6] : nullptr);
    if (argc > 1) {
        std::printf("usage: %s [--image FILE W H [--dump DIR]]\n", argv[0]);
        return 2;
    }
    candidates();
    exact_blocks();
    etc1_blocks();
    clamped_gradients();
    bc1_alpha();
    layout();
    picture_psnr();
    if (failures) {
        std::printf("%d failures\n", failures);
        return 1;
    }
    std::puts("BCn to ETC2 and EAC: every candidate stores its mode, exact blocks, BC1's transparency, the layout and "
              "the picture's PSNR as held");
    return 0;
}
