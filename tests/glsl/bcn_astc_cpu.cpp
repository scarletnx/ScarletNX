// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BCn to ASTC transcoder (src/nx/gpu/texture/texconv.glsl built with SNX_TC_ASTC: its BCn decoders, then
// astc_encode.glsl) run on the CPU through tests/glsl/glsl.hpp, dispatched as vk/texconv.c dispatches it: the expected
// blocks of tests/astc_vulkan.c.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "bcn_etc2_cpu.h"

namespace astc_transcode_shader {
using namespace glsl;
// the sRGB copy transfer (pixel_convert.glsl) is the body's floating point beside the fitter's reciprocal
static float uintBitsToFloat(uint bits) {
    float value;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}
static uint floatBitsToUint(float value) {
    uint bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}
using std::pow;
// what texconv.comp declares, under its names, in its ASTC build
#define SNX_TC_ASTC 1
#define SNX_ASTC_ENC 1
struct Transcoder : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
// the ASTC build's bc_put keeps the output place the decoders hand it, which it doesn't use
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "nx/gpu/texture/texconv.glsl"
#pragma GCC diagnostic pop
};
// the same without the source block's hints (astc_encode.glsl's SNX_ASTC_HINTS), for --measure: what they cost
#undef SNX_ASTC_HINTS
#define SNX_ASTC_HINTS 0
struct TranscoderPlain : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "nx/gpu/texture/texconv.glsl"
#pragma GCC diagnostic pop
};
#undef SNX_ASTC_HINTS
// the first of the two passes vk/texconv.c runs (astc_encode.glsl's SNX_ASTC_PASS 1), for the count of the blocks
// it leaves to the second
#undef SNX_ASTC_HINTS
#define SNX_ASTC_HINTS 1
#undef SNX_ASTC_PASS
#define SNX_ASTC_PASS 1
#define work_groups work[0]
#define work_count work[3]
#define work_list (work + 4)
struct TranscoderFirst : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
    leuint* work;
#include "nx/gpu/texture/texconv_modes.glsl"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "nx/gpu/texture/texconv.glsl"
#pragma GCC diagnostic pop
};
#undef SNX_ASTC_PASS
#undef SNX_ASTC_HINTS
#undef SNX_TC_ASTC
#undef SNX_ASTC_ENC
} // namespace astc_transcode_shader

static const uint32_t TC_BCN_ASTC_MODE = 50; // texconv_modes.glsl's TC_BCN_ASTC

template <typename T>
static void transcode(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out) {
    const uint32_t words = bw * 4, in_words = bw * (kind == 0 ? 2 : 4); // BC1's blocks are 8 bytes
    std::vector<glsl::leuint> in((size_t)in_words * bh * slices), res((size_t)words * bh * slices);
    std::memcpy(in.data(), blocks, in.size() * 4);
    glsl::dispatch<T>(glsl::uvec3((bw + 63) / 64, bh, slices), glsl::uvec3(64, 1, 1), [&](T& t) {
        t.mode = TC_BCN_ASTC_MODE + (uint32_t)kind;
        t.units = bw;
        t.rows = bh;
        t.in_pitch = in_words;
        t.out_pitch = words;
        t.in_bias = 0;
        t.out_bias = 0;
        t.src = in.data();
        t.dst = res.data();
    });
    std::memcpy(out, res.data(), res.size() * 4);
}

extern "C" void bcn_astc_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out) {
    transcode<astc_transcode_shader::Transcoder>(kind, blocks, bw, bh, slices, out);
}
extern "C" void bcn_astc_plain_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices,
                                   uint8_t* out) {
    transcode<astc_transcode_shader::TranscoderPlain>(kind, blocks, bw, bh, slices, out);
}

// the blocks the first pass leaves to the second, of n BC7 blocks in a row: how many, and of which BC7 mode each
// (left[mode], 8 of them)
extern "C" uint32_t bcn_astc_left_cpu(const uint8_t* blocks, uint32_t n, uint32_t left[8]) {
    using astc_transcode_shader::TranscoderFirst;
    std::vector<glsl::leuint> in((size_t)n * 4), res((size_t)n * 4), work(4 + (size_t)n, 0u);
    std::memcpy(in.data(), blocks, in.size() * 4);
    glsl::dispatch<TranscoderFirst>(glsl::uvec3((n + 63) / 64, 1, 1), glsl::uvec3(64, 1, 1), [&](TranscoderFirst& t) {
        t.mode = TC_BCN_ASTC_MODE + 9u;
        t.units = n;
        t.rows = 1;
        t.in_pitch = n * 4;
        t.out_pitch = n * 4;
        t.in_bias = 0;
        t.out_bias = 0;
        t.src = in.data();
        t.dst = res.data();
        t.work = work.data();
    });
    for (int m = 0; m < 8; m++) left[m] = 0;
    for (uint32_t k = 0; k < uint32_t(work[3]); k++) {
        const uint32_t b = blocks[(size_t)uint32_t(work[4 + k]) * 16];
        uint32_t m = 0;
        while (m < 7 && !(b >> m & 1)) m++;
        left[m]++;
    }
    return uint32_t(work[3]);
}
