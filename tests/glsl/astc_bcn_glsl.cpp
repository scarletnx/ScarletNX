// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// astc_bcn.comp's body run on the CPU (astc_bcn_cpu.cpp): ASTC blocks straight into BC4, BC5 and BC1 blocks, for the
// textures whose blocks hold no more than those (gpu/texture/astc_kind.h). What each loses against the texels the CPU's
// ASTC decoder makes of the same blocks (tests/support/astc.c, the GPU decoder's reference), with the formats decoded
// as the specification has them (tests/support/bcn.c) and sampled as the view gives them back (BC4 as R, R, R, 1, BC5
// as R, R, R, G): PSNR over the four channels and the largest error of each, on levels of random blocks of each kind,
// every footprint, held to the floor measured on them. No GPU is needed: tests/astc_vulkan.c holds the SPIR-V build of
// the same files to the same blocks on a Vulkan device.
//
// --measure [--srgb] FILE.astc...: each file's kind, the format it takes, and that format's loss and BC7's (the
// encoder's fast build, astc_bc7_cpu) on it, for the quality table in docs/layers/gpu/astc.md.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <vector>
#include "astc_bc7_cpu.h"
#include "astc_bcn_cpu.h"
#include "nx/gpu/texture/astc_kind.h"
extern "C" {
void astc_decode_image(const uint8_t* src, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint8_t* dst, int srgb);
void bcn_decode_image_spec(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
uint32_t bcn_pixel_bytes(int kind);
}
// tests/support/bcn.h's kinds
enum { BCN_BC1 = 0, BCN_BC4 = 3, BCN_BC5 = 5, BCN_BC7 = 9 };

static uint32_t rng = 0x2468ace1;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// a random block, one in two of them one partition of an LDR endpoint mode without HDR (as tests/astc_kind.c)
static void block_new(uint8_t b[16]) {
    for (int k = 0; k < 16; k++) b[k] = uint8_t(random_word());
    if (random_word() % 2) {
        static const uint8_t cems[7] = {0, 1, 4, 5, 6, 8, 9};
        const uint32_t cem = cems[random_word() % 7];
        b[1] = uint8_t((b[1] & 0x80) | (cem << 5) | (b[1] & 7));
        b[2] = uint8_t((b[2] & 0xFE) | (cem >> 3));
    }
}

// the loss of a format against the exact texels: squared error over RGBA, and the largest error of each channel
struct Loss {
    double sse = 0, values = 0;
    uint32_t most[4] = {0, 0, 0, 0};
    double psnr() const { return sse > 0 ? 10 * std::log10(255.0 * 255.0 * values / sse) : 99.0; }
    void add(const Loss& o) {
        sse += o.sse;
        values += o.values;
        for (int c = 0; c < 4; c++) most[c] = std::max(most[c], o.most[c]);
    }
};

// what the view samples from blocks of format as (SNX_ASTC_AS_*, BC7 for SNX_ASTC_AS_USUAL), w x h texels, RGBA8
static void sampled(int as, const uint8_t* blocks, uint32_t w, uint32_t h, uint8_t* out) {
    const uint32_t cx = (w + 3) / 4, cy = (h + 3) / 4, pw = cx * 4;
    const int kind = as == SNX_ASTC_AS_BC4   ? BCN_BC4
                     : as == SNX_ASTC_AS_BC5 ? BCN_BC5
                     : as == SNX_ASTC_AS_BC1 ? BCN_BC1
                                             : BCN_BC7;
    std::vector<uint8_t> px((size_t)pw * cy * 4 * bcn_pixel_bytes(kind));
    bcn_decode_image_spec(blocks, pw, cy * 4, kind, px.data());
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = px.data() + ((size_t)y * pw + x) * bcn_pixel_bytes(kind);
            uint8_t* o = out + ((size_t)y * w + x) * 4;
            if (kind == BCN_BC4 || kind == BCN_BC5) {
                // 16 bit unorm channels to 8 bits, nearest
                const uint32_t r = (uint32_t)(p[0] | p[1] << 8), g = kind == BCN_BC5 ? (uint32_t)(p[2] | p[3] << 8) : 0;
                const uint8_t r8 = uint8_t((r * 255 + 32767) / 65535), g8 = uint8_t((g * 255 + 32767) / 65535);
                o[0] = o[1] = o[2] = r8;
                o[3] = kind == BCN_BC5 ? g8 : 255;
            } else {
                std::memcpy(o, p, 4);
            }
        }
}

static Loss loss_of(const uint8_t* want, const uint8_t* got, size_t texels) {
    Loss l;
    for (size_t i = 0; i < texels * 4; i++) {
        const int d = int(got[i]) - int(want[i]);
        l.sse += double(d) * d;
        l.most[i % 4] = std::max(l.most[i % 4], uint32_t(std::abs(d)));
    }
    l.values = double(texels) * 4;
    return l;
}

// the blocks of format as (BC7's fast build for SNX_ASTC_AS_USUAL) of a w x h level of ASTC blocks, and what sampling
// them gives
static std::vector<uint8_t> encode(int as, const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh,
                                   int srgb) {
    const uint32_t cx = (w + 3) / 4, cy = (h + 3) / 4;
    std::vector<uint8_t> out((size_t)cx * cy * 16);
    if (as == SNX_ASTC_AS_USUAL)
        astc_bc7_cpu(1, blocks, w, h, 1, bw, bh, srgb, out.data());
    else
        astc_bcn_cpu(as == SNX_ASTC_AS_BC1   ? 1
                     : as == SNX_ASTC_AS_BC4 ? 4
                                             : 5,
                     blocks, w, h, 1, bw, bh, srgb, out.data());
    return out;
}

static int failures;

// BC1's blocks are four colours, or every index 0 where both ends are one: none decodes transparent
static void bc1_opaque(const std::vector<uint8_t>& b, const char* what) {
    for (size_t i = 0; i + 8 <= b.size(); i += 8) {
        const uint32_t c0 = b[i] | b[i + 1] << 8, c1 = b[i + 2] | b[i + 3] << 8;
        const uint32_t idx = b[i + 4] | b[i + 5] << 8 | b[i + 6] << 16 | uint32_t(b[i + 7]) << 24;
        if (c0 < c1 || (c0 == c1 && idx)) {
            std::fprintf(stderr, "FAIL %s: BC1 block %zu is of three colours, ends %04x %04x indices %08x\n", what,
                         i / 8, c0, c1, idx);
            failures++;
            return;
        }
    }
}

// a level of random blocks of one kind (want, as snx_astc_block_kind gives it) and footprint, its format's loss
static Loss level_loss(uint32_t want, int as, uint32_t bw, uint32_t bh, int srgb) {
    if (snx_astc_kind_as(want | (want & SNX_ASTC_CHROMA && (bw % 4 || bh % 4) ? SNX_ASTC_LINES : 0u), srgb) != as)
        return Loss(); /* colour at a footprint whose 4x4 blocks take two ASTC blocks or more: not BC1 */
    const uint32_t w = bw * 13 - 1, h = bh * 11 - 2, nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
    std::vector<uint8_t> blocks((size_t)nx * ny * 16);
    for (uint32_t i = 0; i < nx * ny;) {
        uint8_t b[16];
        block_new(b);
        if (snx_astc_block_kind(b, bw, bh, srgb) != want) continue;
        std::memcpy(&blocks[(size_t)i++ * 16], b, 16);
    }
    if (snx_astc_level_kind(blocks.data(), blocks.size(), nx, ny, 1, nx * 16, 0, 0, 0, bw, bh, srgb) != want ||
        snx_astc_kind_as(want, srgb) != as) {
        std::fprintf(stderr, "FAIL %ux%u srgb=%d: a level of blocks of kind %u is not of that kind, or format %d\n", bw,
                     bh, srgb, want, as);
        failures++;
    }
    std::vector<uint8_t> texels((size_t)w * h * 4), got(texels.size());
    astc_decode_image(blocks.data(), w, h, bw, bh, texels.data(), srgb);
    const std::vector<uint8_t> enc = encode(as, blocks.data(), w, h, bw, bh, srgb);
    if (as == SNX_ASTC_AS_BC1) bc1_opaque(enc, "random blocks");
    sampled(as, enc.data(), w, h, got.data());
    return loss_of(texels.data(), got.data(), (size_t)w * h);
}

// The floor of each format, measured on these levels (random weights: the encoders' worst case, a texel apart from its
// neighbours; BC1's colour at 4x4, 8x8 and 12x12 alone, astc_kind.h's SNX_ASTC_LINES); a change that loses more fails.
// PSNR over RGBA, then the largest error of R, G, B and A. On astcenc's encodings of public pictures they lose far less
// (docs/layers/gpu/astc.md, "By what the blocks hold")
static const struct {
    uint32_t kind;
    int srgb, as;
    const char* name;
    double psnr;
    uint32_t most[4];
} floors[4] = {{0, 0, SNX_ASTC_AS_BC4, "luminance as BC4", 41.07, {17, 17, 17, 0}},
               {SNX_ASTC_ALPHA, 0, SNX_ASTC_AS_BC5, "luminance and alpha as BC5", 40.19, {17, 17, 17, 19}},
               {SNX_ASTC_CHROMA, 0, SNX_ASTC_AS_BC1, "colour on one line as BC1", 36.74, {39, 32, 33, 0}},
               {0, 1, SNX_ASTC_AS_BC1, "sRGB luminance as BC1", 34.35, {40, 43, 40, 0}}};

static int measure(int n, char** files, int srgb) {
    static const char* names[4] = {"as it is (BC7 fast build)", "BC4", "BC5", "BC1"};
    std::printf("file | footprint | size | kind | format | its PSNR | its most R/G/B/A | BC7 PSNR | BC7 most R/G/B/A | "
                "its CPU s | BC7 CPU s\n");
    for (int f = 0; f < n; f++) {
        FILE* file = std::fopen(files[f], "rb");
        uint8_t hdr[16];
        if (!file || std::fread(hdr, 1, 16, file) != 16) {
            std::fprintf(stderr, "cannot read %s\n", files[f]);
            return 1;
        }
        const uint32_t bw = hdr[4], bh = hdr[5], w = hdr[7] | hdr[8] << 8 | hdr[9] << 16,
                       h = hdr[10] | hdr[11] << 8 | hdr[12] << 16;
        const uint32_t nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
        std::vector<uint8_t> blocks((size_t)nx * ny * 16);
        if (bw < 4 || bw > 12 || bh < 4 || bh > 12 || !w || !h || w > 16384 || h > 16384 ||
            std::fread(blocks.data(), 16, (size_t)nx * ny, file) != (size_t)nx * ny) {
            std::fprintf(stderr, "%s is no .astc file of whole blocks\n", files[f]);
            return 1;
        }
        std::fclose(file);
        const uint32_t kind =
            snx_astc_level_kind(blocks.data(), blocks.size(), nx, ny, 1, nx * 16, 0, 0, 0, bw, bh, srgb);
        const int as = snx_astc_kind_as(kind, srgb);
        std::vector<uint8_t> texels((size_t)w * h * 4), got(texels.size());
        astc_decode_image(blocks.data(), w, h, bw, bh, texels.data(), srgb);
        Loss l[2];
        double secs[2] = {0, 0};
        for (int k = 0; k < 2; k++) {
            if (k == 0 && as == SNX_ASTC_AS_USUAL) continue;
            timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            const std::vector<uint8_t> enc = encode(k ? SNX_ASTC_AS_USUAL : as, blocks.data(), w, h, bw, bh, srgb);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            secs[k] = double(t1.tv_sec - t0.tv_sec) + double(t1.tv_nsec - t0.tv_nsec) / 1e9;
            sampled(k ? SNX_ASTC_AS_USUAL : as, enc.data(), w, h, got.data());
            l[k] = loss_of(texels.data(), got.data(), (size_t)w * h);
        }
        const char* slash = std::strrchr(files[f], '/');
        std::printf("%s | %ux%u | %ux%u | %u | %s | ", slash ? slash + 1 : files[f], bw, bh, w, h, kind, names[as]);
        if (as == SNX_ASTC_AS_USUAL)
            std::printf("- | - | ");
        else
            std::printf("%.2f | %u/%u/%u/%u | ", l[0].psnr(), l[0].most[0], l[0].most[1], l[0].most[2], l[0].most[3]);
        std::printf("%.2f | %u/%u/%u/%u | %.3f | %.3f\n", l[1].psnr(), l[1].most[0], l[1].most[1], l[1].most[2],
                    l[1].most[3], secs[0], secs[1]);
        std::fflush(stdout);
    }
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && !std::strcmp(argv[1], "--measure")) {
        const int srgb = !std::strcmp(argv[2], "--srgb");
        return measure(argc - 2 - srgb, argv + 2 + srgb, srgb);
    }
    static const uint32_t footprints[][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                             {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    for (const auto& fl : floors) {
        Loss all;
        for (const auto& f : footprints) {
            const Loss l = level_loss(fl.kind, fl.as, f[0], f[1], fl.srgb);
            if (getenv("SNX_BCN_EACH"))
                std::printf("  %s %ux%u: %.2f dB, most %u %u %u %u\n", fl.name, f[0], f[1], l.psnr(), l.most[0],
                            l.most[1], l.most[2], l.most[3]);
            all.add(l);
        }
        int worse = all.psnr() < fl.psnr;
        for (int c = 0; c < 4; c++) worse |= all.most[c] > fl.most[c];
        std::printf(
            "%s %s: %.3f dB (floor %.2f), most error R %u G %u B %u A %u (floor %u %u %u %u), every footprint\n",
            worse ? "FAIL" : "PASS", fl.name, all.psnr(), fl.psnr, all.most[0], all.most[1], all.most[2], all.most[3],
            fl.most[0], fl.most[1], fl.most[2], fl.most[3]);
        failures += worse;
    }
    // a block of one value or one colour: BC4 and BC5 hold it exactly, BC1 within the error of a third of the way
    // between two 565 ends (one level of 256 at most)
    for (uint32_t v = 0; v < 256; v++) {
        uint8_t blk[16] = {0xFC, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        const uint32_t r = v * 257, g = (255 - v) * 257, b = (v * 7 % 256) * 257, a = 0xFFFF;
        const uint32_t grey[4] = {r, r, r, (v ^ 0x5A) * 257u}, colour[4] = {r, g, b, a};
        for (int k = 0; k < 3; k++) {
            const uint32_t* c = k == 2 ? colour : grey;
            for (int i = 0; i < 4; i++) blk[8 + 2 * i] = uint8_t(c[i]), blk[9 + 2 * i] = uint8_t(c[i] >> 8);
            if (k == 0) blk[14] = blk[15] = 0xFF;
            const int as = k == 0 ? SNX_ASTC_AS_BC4 : k == 1 ? SNX_ASTC_AS_BC5 : SNX_ASTC_AS_BC1;
            uint8_t texels[16 * 4], got[16 * 4];
            astc_decode_image(blk, 4, 4, 4, 4, texels, 0);
            const std::vector<uint8_t> enc = encode(as, blk, 4, 4, 4, 4, 0);
            sampled(as, enc.data(), 4, 4, got);
            const Loss l = loss_of(texels, got, 16);
            if (l.most[0] > (k == 2 ? 1u : 0u) || l.most[1] > (k == 2 ? 1u : 0u) || l.most[2] > (k == 2 ? 1u : 0u) ||
                l.most[3]) {
                std::fprintf(stderr, "FAIL one colour %u as format %d: errors %u %u %u %u\n", v, as, l.most[0],
                             l.most[1], l.most[2], l.most[3]);
                failures++;
            }
        }
    }
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("PASS one value or colour a block: BC4 and BC5 exact, BC1 within a level");
    std::puts("ASTC blocks of luminance, luminance and alpha, and colour on one line become BC4, BC5 and BC1 within "
              "the floors measured");
    return 0;
}
