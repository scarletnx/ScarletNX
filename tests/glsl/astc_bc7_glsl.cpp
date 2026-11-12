// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// astc_bc7.comp's body run on the CPU (astc_bc7_cpu.cpp): ASTC blocks straight into BC7 blocks. Its blocks are held,
// byte for byte, to the BC7 encoder's (bc7_cpu.cpp) over the texels the CPU's ASTC decoder (tests/support/astc.c)
// makes of the same blocks, which is what the two shaders it took the place of wrote: every footprint in both decode
// modes, each of the encoder's builds, random blocks through all 2048 block modes, sizes on no block of either kind,
// and strips of several slices. Then the multiplication that stands for a coordinate's division by the footprint, for
// every footprint and coordinate. No GPU is needed: tests/astc_vulkan.c holds the SPIR-V build of the same files to
// the same blocks on a Vulkan device.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "astc_bc7_cpu.h"
#include "bc7_cpu.h"
extern "C" void astc_decode_image(const uint8_t* src, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint8_t* dst,
                                  int srgb);

static uint32_t rng = 0x2468ace1;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// random blocks with every block mode, void extents valid and not, and runs of one colour (as tests/astc_glsl.cpp's,
// stored as the device's little-endian words)
static std::vector<uint8_t> blocks_new(uint32_t count) {
    std::vector<uint8_t> blocks((size_t)count * 16);
    for (uint32_t i = 0; i < count; i++) {
        uint32_t b[4] = {random_word(), random_word(), random_word(), random_word()};
        b[0] = (b[0] & ~2047u) | (i & 2047u);
        if (i % 29 == 0 || i % 64 < 3) {
            b[0] = 0xfffffdfcu;
            b[1] = ~0u;
            if (i % 64 < 3) b[2] = 0x40008000u, b[3] = 0xffffc000u;
        } else if (i % 31 == 0) {
            b[0] = 0xfffff9fcu; // invalid reserved bit
        }
        for (int k = 0; k < 16; k++) blocks[(size_t)i * 16 + k] = uint8_t(b[k / 4] >> (k % 4 * 8));
    }
    return blocks;
}

static int failures;

static void check(uint32_t build, uint32_t bw, uint32_t bh, int srgb, uint32_t w, uint32_t h, uint32_t depth) {
    const uint32_t nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh, cx = (w + 3) / 4, cy = (h + 3) / 4;
    const std::vector<uint8_t> blocks = blocks_new(nx * ny * depth);
    std::vector<uint8_t> texels((size_t)w * h * depth * 4), want((size_t)cx * cy * depth * 16), got(want.size());
    for (uint32_t z = 0; z < depth; z++)
        astc_decode_image(blocks.data() + (size_t)z * nx * ny * 16, w, h, bw, bh, texels.data() + (size_t)z * w * h * 4,
                          srgb);
    // the fast build without the hints is the encoder's fast build of the texels; with them (as the shader is built)
    // it is held to its two passes below
    bc7_encode_cpu_build(build, texels.data(), w, h, depth, want.data());
    astc_bc7_cpu(build == 1 ? 1u | ASTC_BC7_PLAIN : build, blocks.data(), w, h, depth, bw, bh, srgb, got.data());
    for (size_t i = 0; i < want.size(); i += 16)
        if (std::memcmp(&got[i], &want[i], 16)) {
            std::fprintf(stderr,
                         "FAIL %ux%u srgb=%d %ux%ux%u build %u: BC7 block %zu is not the encoder's of the decoded "
                         "texels\n",
                         bw, bh, srgb, w, h, depth, build, i / 16);
            failures++;
            return;
        }
    // the two passes of the builds that search past mode 6: the same blocks, some of them the second pass's
    uint32_t listed = 0;
    if (build < 2) {
        astc_bc7_cpu(build, blocks.data(), w, h, depth, bw, bh, srgb, want.data());
        std::fill(got.begin(), got.end(), uint8_t(0xA5));
        astc_bc7_cpu_passes(build, blocks.data(), w, h, depth, bw, bh, srgb, got.data(), &listed);
        if (std::memcmp(got.data(), want.data(), want.size()) || !listed || listed >= cx * cy * depth) {
            std::fprintf(stderr,
                         "FAIL %ux%u srgb=%d %ux%ux%u build %u in two passes: not the encoder's blocks, or %u of "
                         "the %u left to the second pass\n",
                         bw, bh, srgb, w, h, depth, build, listed, cx * cy * depth);
            failures++;
            return;
        }
    }
    std::printf("PASS %ux%u srgb=%d %ux%ux%u build %u, %u blocks the second pass's\n", bw, bh, srgb, w, h, depth,
                build, listed);
}

// a texel's coordinate divided by a footprint's side as (v * ceil(2^18 / side)) >> 18, for every side and every
// coordinate of the largest texture (16384 a side)
static void footprint_division() {
    for (uint32_t side = 4; side <= 12; side++) {
        const uint32_t recip = (262144u + side - 1u) / side;
        for (uint32_t v = 0; v < 16384; v++)
            if ((v * recip) >> 18 != v / side) {
                std::fprintf(stderr, "FAIL %u / %u by multiplication\n", v, side);
                failures++;
                return;
            }
    }
    std::puts("PASS a texel's coordinate divided by the footprint's side, by multiplication");
}

// --measure file.astc...: what the hints cost and save on a game's own blocks (astc_dump's files). Each file's first
// slice, transcoded by the full build, the fast one, the fast one with each cut of its search and mode 6 alone, on the CPU: the PSNR of each against
// the ASTC blocks' own texels (all four channels), the seconds each took here (the CPU's, which count the same
// operations the GPU runs but none of its divergence), and how many blocks each build wrote in each mode
extern "C" void bcn_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
#define NB 6
static int measure(int n, char** files) {
    static const struct {
        uint32_t build;
        const char* name;
    } builds[NB] = {{0, "full"},
                    {1 | ASTC_BC7_PLAIN, "fast, no hints"},
                    {1, "fast"},
                    {1 | ASTC_BC7_PLAIN | ASTC_BC7_QUICK, "no hints, q64"},
                    {1 | ASTC_BC7_QUICK, "fast, q64"},
                    {2, "mode 6"}};
    double sse[NB] = {0}, secs[NB] = {0}, values = 0;
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
        std::vector<uint8_t> blocks((size_t)nx * ny * 16);
        if (bw < 4 || bw > 12 || bh < 4 || bh > 12 || !w || !h || w > 16384 || h > 16384 ||
            std::fread(blocks.data(), 16, (size_t)nx * ny, file) != (size_t)nx * ny) {
            std::fprintf(stderr, "%s is no .astc file of whole blocks\n", files[f]);
            return 1;
        }
        std::fclose(file);
        std::vector<uint8_t> texels((size_t)w * h * 4), bc7((size_t)cx * cy * 16), got((size_t)cx * cy * 64);
        astc_decode_image(blocks.data(), w, h, bw, bh, texels.data(), 0);
        std::printf("%s: %ux%u of %ux%u", files[f], w, h, bw, bh);
        for (int b = 0; b < NB; b++) {
            timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            astc_bc7_cpu(builds[b].build, blocks.data(), w, h, 1, bw, bh, 0, bc7.data());
            clock_gettime(CLOCK_MONOTONIC, &t1);
            const double s = double(t1.tv_sec - t0.tv_sec) + double(t1.tv_nsec - t0.tv_nsec) / 1e9;
            bcn_decode_image(bc7.data(), cx * 4, cy * 4, 9 /* BCN_BC7 */, got.data());
            double e = 0;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w * 4; x++) {
                    const double d = double(got[(size_t)y * cx * 16 + x]) - double(texels[(size_t)y * w * 4 + x]);
                    e += d * d;
                }
            uint32_t modes[8] = {0};
            for (size_t i = 0; i < bc7.size(); i += 16) {
                uint32_t m = 0;
                while (m < 8 && !(bc7[i] >> m & 1)) m++;
                modes[m & 7]++;
            }
            sse[b] += e;
            secs[b] += s;
            const double mse = e / (double(w) * h * 4);
            std::printf("\n  %-14s %6.2f dB %7.3f s  modes 1:%u 2:%u 3:%u 4:%u 5:%u 6:%u 7:%u", builds[b].name,
                        mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99.0, s, modes[1], modes[2], modes[3],
                        modes[4], modes[5], modes[6], modes[7]);
        }
        std::printf("\n");
        std::fflush(stdout);
        values += double(w) * h * 4;
    }
    std::printf("all %d files:", n);
    for (int b = 0; b < NB; b++)
        std::printf(" %s %.2f dB %.2f s;", builds[b].name,
                    sse[b] > 0 ? 10 * std::log10(255.0 * 255.0 * values / sse[b]) : 99.0, secs[b]);
    std::printf("\n");
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 2 && !std::strcmp(argv[1], "--measure")) return measure(argc - 2, argv + 2);
    footprint_division();
    const uint32_t footprints[][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                      {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    uint32_t n = 0;
    for (const auto& f : footprints) {
        // the builds and the decode modes in turn; sizes on no block of either kind
        for (uint32_t k = 0; k < 4; k++, n++) check(n % 4, f[0], f[1], int(n / 4 & 1), f[0] * 9 - 1, f[1] * 7 - 2, 1);
        // 2048 blocks or more, so every block mode, in the build a level takes by default, and a strip of slices
        check(1, f[0], f[1], 0, f[0] * 67 - 1, f[1] * 31 - 2, 1);
        check(0, f[0], f[1], 1, 17, 13, 3);
    }
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("ASTC blocks become the BC7 blocks the encoder makes of their decoded texels");
    return 0;
}
