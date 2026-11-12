// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The ASTC compute shader's body (src/nx/gpu/texture/astc_decode.glsl) run on the CPU through tests/glsl/glsl.hpp,
// compared byte for byte with the CPU reference decoder (tests/support/astc.c): every footprint in both decode
// modes, random blocks through all 2048 block modes, sizes that are not a multiple of the footprint, and strips of
// several slices in one dispatch, as vk/astc.c batches a 3D texture's. No GPU is needed: tests/astc_vulkan.c compares
// the SPIR-V build of the same file with the same reference on a Vulkan device.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "glsl.hpp"
// tests/support/astc.h's one function (its header brings the recompiler's C helpers, which C++ warns about)
extern "C" void astc_decode_image(const uint8_t* src, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh, uint8_t* dst,
                                  int srgb);

namespace astc_shader {
using namespace glsl;
// what astc.comp declares, under its names. The workgroup's tile is zeroed here (a GPU's shared memory holds
// whatever was there, which the decoder only reads where it weighs it 0)
struct Tile {
    uint words[39 * 64];
};
struct Decoder : WorkgroupInvocation {
    uint width, height, blocks_x, blocks_y, srgb, tile_shift;
    uvec2 block_dims;
    const leuvec4* astc_data;
    leuint* texels;
    uint* astc_tile;
#define TILE(field, slot) astc_tile[(field) * 64u + (slot)]
#include "nx/gpu/texture/astc_decode.glsl"
#undef TILE
};
} // namespace astc_shader

static uint32_t rng = 0x12345678;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// random blocks with every block mode, and void extents valid and not (as tests/astc_vulkan.c makes them)
static std::vector<glsl::leuvec4> blocks_new(uint32_t count) {
    std::vector<glsl::leuvec4> blocks(count);
    for (uint32_t i = 0; i < count; i++) {
        glsl::uvec4 b = glsl::uvec4(random_word(), random_word(), random_word(), random_word());
        b.x = (b.x & ~2047u) | (i & 2047u);
        if (i % 29 == 0) {
            b.x = 0xfffffdfcu;
            b.y = ~0u;
        } else if (i % 31 == 0) {
            b.x = 0xfffff9fcu; // invalid reserved bit
        }
        blocks[i] = b;
    }
    return blocks;
}

static int failures;

// a w x h image of depth slices, decoded in one dispatch as one strip of depth slices, in tiles 1 << tile_shift
// blocks wide, as vk/astc.c dispatches it
static void check(uint32_t bw, uint32_t bh, int srgb, uint32_t w, uint32_t h, uint32_t depth, uint32_t tile_shift) {
    const uint32_t nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh;
    const uint32_t tw = 1u << tile_shift, th = 64u >> tile_shift;
    std::vector<glsl::leuvec4> blocks = blocks_new(nx * ny * depth);
    // the reference decodes into bytes: both images read as the device's little-endian words
    std::vector<glsl::leuint> gpu((size_t)w * h * depth, 0xdeadbeefu), cpu((size_t)w * h * depth);
    glsl::dispatch_workgroups<astc_shader::Decoder, astc_shader::Tile>(
        glsl::uvec3((nx + tw - 1) / tw, (ny + th - 1) / th, depth), glsl::uvec3(64u, 1u, 1u),
        [&](astc_shader::Decoder& d, astc_shader::Tile& tile) {
            d.width = w;
            d.height = h;
            d.blocks_x = nx;
            d.blocks_y = ny;
            d.block_dims = glsl::uvec2(bw, bh);
            d.srgb = uint32_t(srgb);
            d.tile_shift = tile_shift;
            d.astc_data = blocks.data();
            d.texels = gpu.data();
            d.astc_tile = tile.words;
        });
    for (uint32_t z = 0; z < depth; z++)
        astc_decode_image(reinterpret_cast<const uint8_t*>(blocks.data() + (size_t)z * nx * ny), w, h, bw, bh,
                          reinterpret_cast<uint8_t*>(cpu.data() + (size_t)z * w * h), srgb);
    for (size_t i = 0; i < cpu.size(); i++)
        if (gpu[i] != cpu[i]) {
            std::fprintf(stderr, "FAIL %ux%u srgb=%d %ux%ux%u tiles of %u: texel %zu is %08x, the reference %08x\n",
                         bw, bh, srgb, w, h, depth, 1u << tile_shift, i, uint32_t(gpu[i]), uint32_t(cpu[i]));
            failures++;
            return;
        }
    std::printf("PASS %ux%u srgb=%d %ux%ux%u tiles of %u\n", bw, bh, srgb, w, h, depth, 1u << tile_shift);
}

// the layer's built-ins against values worked out by hand from the GLSL specification
static void builtins() {
    using namespace glsl;
    struct {
        bool ok;
        const char* what;
    } cases[] = {
        {bitfieldExtract(0xF0F0F0F0u, 4, 8) == 0x0Fu, "bitfieldExtract uint"},
        {bitfieldExtract(0x80000000u, 0, 32) == 0x80000000u, "bitfieldExtract of all 32 bits"},
        {bitfieldExtract(0x12345678u, 7, 0) == 0u, "bitfieldExtract of no bits"},
        {bitfieldExtract(int(0x00000F00), 8, 4) == -1, "bitfieldExtract int sign-extends"},
        {bitfieldInsert(0xFFFFFFFFu, 0u, 8, 8) == 0xFFFF00FFu, "bitfieldInsert"},
        {bitfieldInsert(0u, 0xFFFFFFFFu, 0, 32) == 0xFFFFFFFFu, "bitfieldInsert of all 32 bits"},
        {bitfieldReverse(1u) == 0x80000000u && bitfieldReverse(0x12345678u) == 0x1E6A2C48u, "bitfieldReverse"},
        {bitCount(0xF00Fu) == 8, "bitCount"},
        {findLSB(0u) == -1 && findLSB(0x100u) == 8, "findLSB"},
        {findMSB(0u) == -1 && findMSB(0x80000000u) == 31 && findMSB(-1) == -1 && findMSB(-2) == 0, "findMSB"},
        {bitfieldReverse(uvec4(1u, 2u, 0u, 0x80000000u)) == uvec4(0x80000000u, 0x40000000u, 0u, 1u),
         "bitfieldReverse per component"},
        {clamp(ivec4(-5, 0, 300, 255), 0, 255) == ivec4(0, 0, 255, 255), "clamp of a vector"},
        {uvec4(ivec4(-1)) == uvec4(0xFFFFFFFFu), "int to uint vector conversion"},
        {(uvec2(3u, 5u) * uvec2(2u, 4u)) == uvec2(6u, 20u) && (uvec2(1u, 2u) << 3u) == uvec2(8u, 16u),
         "vector arithmetic"},
    };
    for (const auto& c : cases)
        if (!c.ok) {
            std::fprintf(stderr, "FAIL built-in: %s\n", c.what);
            failures++;
        }
    std::puts("PASS built-ins");
}

// the shader's functions are members, callable one by one: its UNORM16 to 8 bit conversion without a division, for
// every input, against the division it replaces (random blocks reach few of the inputs where they could differ)
static void unorm16() {
    astc_shader::Decoder d;
    for (uint32_t c = 0; c <= 0xFFFF; c++)
        if (d.Unorm16To8(c) != (c * 255 + 32767) / 65535) {
            std::fprintf(stderr, "FAIL Unorm16To8(%u) is %u\n", c, d.Unorm16To8(c));
            failures++;
            return;
        }
    std::puts("PASS Unorm16To8 for every UNORM16 value");
}

int main() {
    builtins();
    unorm16();
    const uint32_t footprints[][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                      {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    /* every tile shape, each footprint and decode mode taking a different one, past the image's edges */
    uint32_t shift = 0;
    for (const auto& f : footprints)
        for (int srgb = 0; srgb < 2; srgb++) {
            check(f[0], f[1], srgb, f[0] * 67 - 1, f[1] * 31 - 2, 1, shift++ % 7); // 2048 blocks or more: every mode
            check(f[0], f[1], srgb, 17, 13, 3, shift++ % 7);                       // a strip of several slices
        }
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("The shader's body decodes as the CPU reference does");
    return 0;
}
