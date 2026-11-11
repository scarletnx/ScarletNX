// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BCn to ETC2 and EAC transcoder (src/nx/gpu/texture/texconv.glsl built with SNX_TC_ETC2, and its
// etc2_encode.glsl) run on the CPU through tests/glsl/glsl.hpp, dispatched as vk/texconv.c dispatches it: the
// expected blocks of tests/glsl/bcn_etc2_glsl.cpp and tests/astc_vulkan.c.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "bcn_etc2_cpu.h"

namespace transcode_shader {
using namespace glsl;
// the sRGB copy transfer (pixel_convert.glsl) is the body's only floating point, unused by the ETC2 build
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
// what texconv.comp declares, under its names, in its ETC2 build
#define SNX_TC_ETC2 1
struct Transcoder : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
// the ETC2 build's bc_put keeps the output place the decoders hand it, which it doesn't use
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "nx/gpu/texture/texconv.glsl"
#pragma GCC diagnostic pop
};
#undef SNX_TC_ETC2
} // namespace transcode_shader
using transcode_shader::Transcoder;

static const uint32_t TC_BCN_ETC2_MODE = 40; // texconv_modes.glsl's TC_BCN_ETC2

static uint32_t block_bytes(int kind) { return kind == 0 || kind == 3 || kind == 4 ? 8 : 16; }

extern "C" void bcn_etc2_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out) {
    uint32_t bb = block_bytes(kind), words = bw * bb / 4;
    std::vector<glsl::leuint> in((size_t)words * bh * slices), res((size_t)words * bh * slices);
    std::memcpy(in.data(), blocks, in.size() * 4);
    glsl::dispatch<Transcoder>(glsl::uvec3((bw + 63) / 64, bh, slices), glsl::uvec3(64, 1, 1), [&](Transcoder& t) {
        t.mode = TC_BCN_ETC2_MODE + (uint32_t)kind;
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

// a block's two words as ETC2 and EAC store them: big-endian
static void store(uint32_t hi, uint32_t lo, uint8_t out[8]) {
    for (int i = 0; i < 4; i++) {
        out[i] = (uint8_t)(hi >> (24 - 8 * i));
        out[4 + i] = (uint8_t)(lo >> (24 - 8 * i));
    }
}

extern "C" void etc2_rgb_cpu(const uint32_t texels[16], int punch, uint8_t out[8]) {
    Transcoder t;
    for (int i = 0; i < 16; i++) t.tx_px[i] = texels[i];
    t.et_encode(punch != 0);
    store(t.tx_hi, t.tx_lo, out);
}

extern "C" uint32_t etc2_candidate_cpu(const uint32_t texels[16], int punch, uint32_t c, uint8_t out[8]) {
    Transcoder t;
    bool see = false;
    for (int i = 0; i < 16; i++) {
        t.tx_px[i] = texels[i];
        see = see || (punch && t.et_clear((uint32_t)i));
    }
    t.et_cluster(see);
    t.et_build(c, punch != 0, see);
    store(t.tx_hi, t.tx_lo, out);
    return t.et_err;
}

extern "C" int etc2_tries_cpu(const uint32_t texels[16], int punch, uint32_t c) {
    Transcoder t;
    bool see = false;
    for (int i = 0; i < 16; i++) {
        t.tx_px[i] = texels[i];
        see = see || (punch && t.et_clear((uint32_t)i));
    }
    return !((c < 2 && (punch || t.et1_diff_holds(c))) || (see && c == 7));
}

extern "C" void eac_cpu(int kind, const int32_t values[16], uint8_t out[8]) {
    Transcoder t;
    for (int i = 0; i < 16; i++) t.tx_v[i] = values[i];
    t.ex_encode((uint32_t)kind);
    store(t.tx_hi, t.tx_lo, out);
}
