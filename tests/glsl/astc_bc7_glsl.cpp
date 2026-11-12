// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// astc_bc7.comp's body run on the CPU (astc_bc7_cpu.cpp): ASTC blocks straight into BC7 blocks. Its blocks are held,
// byte for byte, to the BC7 encoder's (bc7_cpu.cpp) over the texels the CPU's ASTC decoder (tests/support/astc.c)
// makes of the same blocks, which is what the two shaders it took the place of wrote: every footprint in both decode
// modes, each of the encoder's builds, random blocks through all 2048 block modes, sizes on no block of either kind,
// and strips of several slices. Then the multiplication that stands for a coordinate's division by the footprint, for
// every footprint and coordinate. No GPU is needed: tests/astc_vulkan.c holds the SPIR-V build of the same files to
// the same blocks on a Vulkan device.
#include <cstdio>
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
    bc7_encode_cpu_build(build, texels.data(), w, h, depth, want.data());
    astc_bc7_cpu(build, blocks.data(), w, h, depth, bw, bh, srgb, got.data());
    for (size_t i = 0; i < want.size(); i += 16)
        if (std::memcmp(&got[i], &want[i], 16)) {
            std::fprintf(stderr,
                         "FAIL %ux%u srgb=%d %ux%ux%u build %u: BC7 block %zu is not the encoder's of the decoded "
                         "texels\n",
                         bw, bh, srgb, w, h, depth, build, i / 16);
            failures++;
            return;
        }
    std::printf("PASS %ux%u srgb=%d %ux%ux%u build %u\n", bw, bh, srgb, w, h, depth, build);
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

int main() {
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
