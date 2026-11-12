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
#undef SNX_TC_ASTC
#undef SNX_ASTC_ENC
} // namespace astc_transcode_shader

static const uint32_t TC_BCN_ASTC_MODE = 50; // texconv_modes.glsl's TC_BCN_ASTC

extern "C" void bcn_astc_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out) {
    using astc_transcode_shader::Transcoder;
    const uint32_t words = bw * 4;
    std::vector<glsl::leuint> in((size_t)words * bh * slices), res((size_t)words * bh * slices);
    std::memcpy(in.data(), blocks, in.size() * 4);
    glsl::dispatch<Transcoder>(glsl::uvec3((bw + 63) / 64, bh, slices), glsl::uvec3(64, 1, 1), [&](Transcoder& t) {
        t.mode = TC_BCN_ASTC_MODE + (uint32_t)kind;
        t.units = bw;
        t.rows = bh;
        t.in_pitch = words;
        t.out_pitch = words;
        t.in_bias = 0;
        t.out_bias = 0;
        t.src = in.data();
        t.dst = res.data();
    });
    std::memcpy(out, res.data(), res.size() * 4);
}
