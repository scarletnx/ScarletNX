// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The Vita's texture deswizzle shader's body (src/vita/gpu/swizzle.glsl) run on the CPU through tests/glsl/glsl.hpp
// against the loops Vita3K's renderer runs (renderer/src/texture/format.cpp, GPL-2.0-or-later, Copyright (C) 2026
// Vita3K team: swizzled_texture_to_linear_texture and resolve_z_order_compressed_image, which are the same walk over
// texels or 4x4 blocks, and tiled_texture_to_linear_texture), on random textures: swizzled ones of every power of two
// shape up to 512 on a side, tiled ones of any size, elements of 1, 2, 3, 4, 8 or 16 bytes, the source at an offset.
// Every byte of the linear rows is checked, the last word's bytes past them zero, and the word after it left alone.
#include <bit>
#include <cstdio>
#include <cstring>
#include <vector>
#include "glsl.hpp"

namespace vita_swizzle_shader {
using namespace glsl;
struct Deswizzler : Invocation {
    uint words, elements, src_base, src_at, es, width, wl, k, tiled, tiles_x, reverse, dst_base;
    const leuint* src;
    leuint* dst;
#include "vita/gpu/swizzle.glsl"
};
} // namespace vita_swizzle_shader
using vita_swizzle_shader::Deswizzler;

static uint32_t rng = 0x2545f491;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
// Vita3K's decode_morton2_x and decode_morton2_y (compact_one_by_one)
static uint32_t compact(uint32_t x) {
    x &= 0x55555555;
    x = (x ^ (x >> 1)) & 0x33333333;
    x = (x ^ (x >> 2)) & 0x0f0f0f0f;
    x = (x ^ (x >> 4)) & 0x00ff00ff;
    x = (x ^ (x >> 8)) & 0x0000ffff;
    return x;
}
// the reference: element i of the source to its place in the linear rows
static void swizzled_reference(uint8_t* dest, const uint8_t* src, uint32_t width, uint32_t height, uint32_t bytes) {
    uint32_t min = width < height ? width : height, k = std::bit_width(min) - 1;
    for (uint32_t i = 0; i < width * height; i++) {
        uint32_t x = compact(i >> 1) & (min - 1), y = compact(i) & (min - 1), upper = (i >> (2 * k)) << k;
        if (width >= height)
            x |= upper;
        else
            y |= upper;
        std::memcpy(dest + (y * width + x) * bytes, src + i * bytes, bytes);
    }
}
static void tiled_reference(uint8_t* dest, const uint8_t* src, uint32_t width, uint32_t height, uint32_t bytes) {
    uint32_t tiles_x = (width + 31) >> 5;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            uint32_t in_tile = (x & 31) | ((y & 31) << 5), tile = (x >> 5) + tiles_x * (y >> 5);
            std::memcpy(dest + (y * width + x) * bytes, src + ((tile << 10) | in_tile) * bytes, bytes);
        }
}

static unsigned failures;
static void one(bool tiled, uint32_t width, uint32_t height, uint32_t bytes) {
    uint32_t es = bytes == 3 ? 5 : std::countr_zero(bytes);
    uint32_t elements = width * height, words = (elements * bytes + 3) / 4;
    uint32_t base = random_word() % 7;
    uint32_t src_at = bytes == 3 ? random_word() % 4 : 0;
    // the source as the guest holds it: tiled textures span whole tiles
    uint32_t span = tiled ? (((width + 31) >> 5) * ((height + 31) >> 5)) << 10 : elements;
    std::vector<glsl::leuint> src(base + (src_at + span * bytes + 3) / 4 + 1), dst(words + 1);
    for (auto& w : src) w = random_word();
    uint32_t guard = random_word();
    dst[words] = guard;
    glsl::dispatch<Deswizzler>(glsl::uvec3((words + 63) / 64, 1, 1), glsl::uvec3(64, 1, 1), [&](Deswizzler& d) {
        d.words = words, d.elements = elements, d.src_base = base, d.src_at = src_at, d.es = es, d.width = width;
        d.wl = std::bit_width(width) - 1;
        d.k = std::bit_width(width < height ? width : height) - 1;
        d.tiled = tiled, d.tiles_x = (width + 31) >> 5, d.reverse = 0, d.dst_base = 0;
        d.src = src.data(), d.dst = dst.data();
    });
    // reversed (a color surface's store, elements of four bytes or more): the rows back where they came from
    if (bytes >= 4) {
        std::vector<glsl::leuint> back(base + (span * bytes + 3) / 4 + 1);
        back[back.size() - 1] = guard;
        glsl::dispatch<Deswizzler>(glsl::uvec3((words + 63) / 64, 1, 1), glsl::uvec3(64, 1, 1), [&](Deswizzler& d) {
            d.words = words, d.elements = elements, d.src_base = 0, d.src_at = 0, d.es = es, d.width = width;
            d.wl = std::bit_width(width) - 1;
            d.k = std::bit_width(width < height ? width : height) - 1;
            d.tiled = tiled, d.tiles_x = (width + 31) >> 5, d.reverse = 1, d.dst_base = base;
            d.src = dst.data(), d.dst = back.data();
        });
        for (uint32_t y = 0; y < height; y++)
            for (uint32_t x = 0; x < width; x++) {
                // the source element linear element (x, y) came from, as the references place it
                uint32_t e = 0;
                if (tiled)
                    e = (((x >> 5) + ((width + 31) >> 5) * (y >> 5)) << 10) | (x & 31) | ((y & 31) << 5);
                else {
                    uint32_t min = width < height ? width : height, k = std::bit_width(min) - 1, m = min - 1;
                    uint32_t mx = x & m, my = y & m, morton = 0;
                    for (uint32_t b = 0; b < k; b++) morton |= ((my >> b) & 1) << (2 * b) | ((mx >> b) & 1) << (2 * b + 1);
                    e = (((x >> k) | (y >> k)) << (2 * k)) | morton;
                }
                for (uint32_t w = 0; w < bytes / 4; w++)
                    if ((uint32_t)back[base + e * (bytes / 4) + w] != (uint32_t)src[base + e * (bytes / 4) + w] &&
                        failures++ < 10)
                        std::printf("%s %ux%u reversed: element (%u, %u) isn't back\n", tiled ? "tiled" : "swizzled",
                                    width, height, x, y);
            }
        if ((uint32_t)back[back.size() - 1] != guard && failures++ < 10)
            std::printf("%s %ux%u reversed: the word past the texture was written\n", tiled ? "tiled" : "swizzled",
                        width, height);
    }
    std::vector<uint8_t> want(elements * bytes);
    const uint8_t* s = (const uint8_t*)(src.data() + base) + src_at;
    if (tiled)
        tiled_reference(want.data(), s, width, height, bytes);
    else
        swizzled_reference(want.data(), s, width, height, bytes);
    const uint8_t* got = (const uint8_t*)dst.data();
    for (uint32_t i = 0; i < elements * bytes; i++)
        if (got[i] != want[i] && failures++ < 10)
            std::printf("%s %ux%u, %u bytes an element: byte %u is %02x, not %02x\n", tiled ? "tiled" : "swizzled",
                        width, height, bytes, i, got[i], want[i]);
    for (uint32_t i = elements * bytes; i < words * 4; i++) /* the last word's bytes past the texture: zero */
        if (got[i] && failures++ < 10)
            std::printf("%s %ux%u: byte %u past the texture isn't zero\n", tiled ? "tiled" : "swizzled", width, height, i);
    if ((uint32_t)dst[words] != guard && failures++ < 10)
        std::printf("%s %ux%u: the word past the rows was written\n", tiled ? "tiled" : "swizzled", width, height);
}

int main() {
    static const uint32_t sizes[] = {1, 2, 3, 4, 8, 16};
    for (uint32_t wl = 0; wl <= 9; wl++)
        for (uint32_t hl = 0; hl <= 9; hl++) one(false, 1u << wl, 1u << hl, sizes[random_word() % 6]);
    for (int i = 0; i < 200; i++) {
        uint32_t bytes = sizes[random_word() % 6];
        one(false, 1u << (random_word() % 10), 1u << (random_word() % 10), bytes);
        one(true, 1 + random_word() % 150, 1 + random_word() % 150, bytes);
    }
    if (failures) {
        std::printf("vita_swizzle_glsl: %u failures\n", failures);
        return 1;
    }
    std::puts("vita_swizzle_glsl: every byte, including 24-bit elements, matches Vita3K's layouts");
    return 0;
}
