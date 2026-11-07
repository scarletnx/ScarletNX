// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The Vita's 24-bit color shader body (src/vita/gpu/rgb24.glsl) run on the CPU through tests/glsl/glsl.hpp against
// memory written out per byte: random surfaces of both swizzles, source row strides in bytes, strides past the width,
// the surface at every offset in a word, unpacked to rows and packed back with other bytes unchanged.
#include <cstdio>
#include <cstring>
#include <vector>
#include "glsl.hpp"

namespace vita_rgb24_shader {
using namespace glsl;
struct Packer : Invocation {
    uint words, src_at, dst_at, width, height, stride, pack, bgr;
    const leuint* src;
    leuint* dst;
#include "vita/gpu/rgb24.glsl"
};
} // namespace vita_rgb24_shader
using vita_rgb24_shader::Packer;

static uint32_t rng = 0x85ebca6b;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static unsigned failures;
static void run(uint32_t count, uint32_t src_at, uint32_t dst_at, uint32_t width, uint32_t height, uint32_t stride,
                bool pack, bool bgr, const glsl::leuint* src, glsl::leuint* dst) {
    glsl::dispatch<Packer>(glsl::uvec3((count + 63) / 64, 1, 1), glsl::uvec3(64, 1, 1), [&](Packer& c) {
        c.words = count, c.src_at = src_at, c.dst_at = dst_at, c.width = width, c.height = height, c.stride = stride;
        c.pack = pack, c.bgr = bgr, c.src = src, c.dst = dst;
    });
}
static void one(uint32_t width, uint32_t height, uint32_t stride, uint32_t at, bool bgr) {
    size_t size = at + (size_t)stride * height * 3, words = (size + 3) / 4;
    std::vector<uint8_t> bytes(words * 4);
    for (auto& b : bytes) b = (uint8_t)random_word();
    std::vector<glsl::leuint> memory(words), rows(width * height);
    std::memcpy(memory.data(), bytes.data(), bytes.size());
    run(width * height, at, 0, width, height, stride * 3, false, bgr, memory.data(), rows.data());
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* p = &bytes[at + (y * stride + x) * 3];
            uint32_t want = bgr ? (p[2] | p[1] << 8 | p[0] << 16) : (p[0] | p[1] << 8 | p[2] << 16);
            want |= 0xff000000u;
            uint32_t got = rows[y * width + x];
            if (got != want && failures++ < 10)
                std::printf("unpack %ux%u stride %u at %u %s: (%u, %u) is %08x, not %08x\n", width, height, stride, at,
                            bgr ? "RGB" : "BGR", x, y, got, want);
        }
    // new rows packed over the memory: the surface's pixels take them, every other byte stays
    for (auto& t : rows) t = random_word();
    std::vector<uint8_t> want = bytes;
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++)
            for (uint32_t c = 0; c < 3; c++)
                want[at + (y * stride + x) * 3 + c] = (uint8_t)(rows[y * width + x] >> ((bgr ? 2 - c : c) * 8));
    run((uint32_t)words, 0, at, width, height, stride, true, bgr, rows.data(), memory.data());
    std::vector<uint8_t> got(words * 4);
    std::memcpy(got.data(), memory.data(), got.size());
    for (size_t i = 0; i < got.size(); i++)
        if (got[i] != want[i] && failures++ < 10)
            std::printf("pack %ux%u stride %u at %u %s: byte %zu is %02x, not %02x\n", width, height, stride, at,
                        bgr ? "RGB" : "BGR", i, got[i], want[i]);
}
static void one_unpacked_stride(uint32_t width, uint32_t height, uint32_t stride, uint32_t at, bool bgr) {
    size_t size = at + (size_t)stride * height, words = (size + 3) / 4;
    std::vector<uint8_t> bytes(words * 4);
    for (auto& b : bytes) b = (uint8_t)random_word();
    std::vector<glsl::leuint> memory(words), rows(width * height);
    std::memcpy(memory.data(), bytes.data(), bytes.size());
    run(width * height, at, 0, width, height, stride, false, bgr, memory.data(), rows.data());
    for (uint32_t y = 0; y < height; y++)
        for (uint32_t x = 0; x < width; x++) {
            const uint8_t* p = &bytes[at + (size_t)y * stride + x * 3];
            uint32_t want = bgr ? (p[2] | p[1] << 8 | p[0] << 16) : (p[0] | p[1] << 8 | p[2] << 16);
            want |= 0xff000000u;
            uint32_t got = rows[y * width + x];
            if (got != want && failures++ < 10)
                std::printf("unpack byte stride %u at %u: (%u, %u) is %08x, not %08x\n", stride, at, x, y, got,
                            want);
        }
}

int main() {
    for (int i = 0; i < 300; i++) {
        uint32_t width = 1 + random_word() % 70, height = 1 + random_word() % 40;
        uint32_t stride = width + (random_word() % 2 ? random_word() % 9 : 0);
        one(width, height, stride, random_word() % 4, random_word() % 2);
        one_unpacked_stride(width, height, width * 3 + random_word() % 9, random_word() % 4, random_word() % 2);
    }
    if (failures) {
        std::printf("vita_rgb24_glsl: %u failures\n", failures);
        return 1;
    }
    std::puts("vita_rgb24_glsl: the shader unpacks and packs every pixel's bytes and keeps the others");
    return 0;
}
