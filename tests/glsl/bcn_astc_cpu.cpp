// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BCn to ASTC transcoder (src/nx/gpu/texture/texconv.glsl built with SNX_TC_ASTC, and its astc_encode.glsl) run
// on the CPU through tests/glsl/glsl.hpp, dispatched as vk/texconv.c dispatches it: the expected blocks of
// tests/glsl/bcn_astc_glsl.cpp and tests/astc_vulkan.c. The body is built twice, its floats 32 bit ones and halves
// (every operation rounded to 16 bits): the two are what a GPU's relaxed precision may be.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "bcn_astc_cpu.h"

// what texconv.comp declares, under its names, in its ASTC build, the floats of type F
#define SNX_TC_ASTC 1
#define highp
#define SNX_ASTC_TRANSCODER(F)                                                                                         \
    using namespace glsl;                                                                                              \
    /* the sRGB copy transfer (pixel_convert.glsl) is the body's other floating point, unused by this build */         \
    [[maybe_unused]] static float uintBitsToFloat(uint bits) {                                                         \
        float value;                                                                                                   \
        std::memcpy(&value, &bits, sizeof value);                                                                      \
        return value;                                                                                                  \
    }                                                                                                                  \
    [[maybe_unused]] static uint floatBitsToUint(float value) {                                                        \
        uint bits;                                                                                                     \
        std::memcpy(&bits, &value, sizeof bits);                                                                       \
        return bits;                                                                                                   \
    }                                                                                                                  \
    using std::pow;                                                                                                    \
    /* the body's 32 bit floats, whatever F is */                                                                      \
    using vec4 = vec<float, 4>;                                                                                        \
    [[maybe_unused]] static F ae_f(uint v) { return F(double(v)); }                                                    \
    [[maybe_unused]] static uint ae_u(F v) { return uint(v); }                                                         \
    [[maybe_unused]] static vec<F, 4> ae_v(F r, F g, F b, F a) { return vec<F, 4>(r, g, b, a); }                       \
    [[maybe_unused]] static vec<F, 4> ae_vs(F s) { return vec<F, 4>(s, s, s, s); }

#pragma GCC diagnostic push
// the encoding builds' bc_put keeps the output place the decoders hand it, which it doesn't use
#pragma GCC diagnostic ignored "-Wunused-parameter"
namespace transcode_float {
SNX_ASTC_TRANSCODER(float)
#define AE_F float
#define AE_V glsl::vec<float, 4>
struct Transcoder : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
#undef AE_F
#undef AE_V
} // namespace transcode_float
namespace transcode_half {
SNX_ASTC_TRANSCODER(glsl::half)
#define AE_F glsl::half
#define AE_V glsl::vec<glsl::half, 4>
struct Transcoder : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
#undef AE_F
#undef AE_V
} // namespace transcode_half
#pragma GCC diagnostic pop
#undef SNX_TC_ASTC
#undef highp

static const uint32_t TC_BCN_ASTC_MODE = 50; // texconv_modes.glsl's TC_BCN_ASTC

template <class T>
static void transcode(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, uint8_t* out) {
    uint32_t words = bw * 4;
    std::vector<glsl::leuint> in((size_t)words * bh * slices), res((size_t)words * bh * slices);
    std::memcpy(in.data(), blocks, in.size() * 4);
    glsl::dispatch<T>(glsl::uvec3((bw + 63) / 64, bh, slices), glsl::uvec3(64, 1, 1), [&](T& t) {
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
extern "C" void bcn_astc_cpu(int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh, uint32_t slices, int half,
                             uint8_t* out) {
    if (half)
        transcode<transcode_half::Transcoder>(kind, blocks, bw, bh, slices, out);
    else
        transcode<transcode_float::Transcoder>(kind, blocks, bw, bh, slices, out);
}

// a block's four words as ASTC stores them: the block's bit 0 the first byte's lowest
static void store(const glsl::uint w[4], uint8_t out[16]) {
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)(w[i / 4] >> (8 * (i % 4)));
}
template <class T>
static void block(const uint32_t texels[16], uint32_t part, uint32_t seed, uint32_t own, uint8_t out[16]) {
    T t;
    for (int i = 0; i < 16; i++) t.tx_px[i] = texels[i];
    t.ax_encode(part, seed, own);
    store(t.ax_w, out);
}
extern "C" void astc_block_cpu(const uint32_t texels[16], uint32_t part, uint32_t seed, uint32_t own, int half,
                               uint8_t out[16]) {
    if (half)
        block<transcode_half::Transcoder>(texels, part, seed, own, out);
    else
        block<transcode_float::Transcoder>(texels, part, seed, own, out);
}

template <class T>
static uint32_t candidate(const uint32_t texels[16], uint32_t kind, uint32_t part, uint32_t seed, uint32_t own,
                          uint8_t out[16]) {
    T t;
    for (int i = 0; i < 16; i++) {
        t.tx_px[i] = texels[i];
        t.ax_px[i] = t.ax_texel(texels[i]);
    }
    t.ax_build(kind, part, seed, own);
    store(t.ax_w, out);
    return uint32_t(t.ax_err);
}
extern "C" uint32_t astc_candidate_cpu(const uint32_t texels[16], uint32_t kind, uint32_t part, uint32_t seed,
                                       uint32_t own, int half, uint8_t out[16]) {
    return half ? candidate<transcode_half::Transcoder>(texels, kind, part, seed, own, out)
                : candidate<transcode_float::Transcoder>(texels, kind, part, seed, own, out);
}

extern "C" void astc_level_cpu(uint32_t v, uint32_t levels, int half, uint32_t* bits, uint32_t* digit,
                               uint32_t* value) {
    if (half) {
        transcode_half::Transcoder t;
        *value = uint32_t(t.ax_levels(transcode_half::ae_vs(double(v)), levels, 0)[0]);
        *bits = t.ax_m[0];
        *digit = t.ax_d[0];
    } else {
        transcode_float::Transcoder t;
        *value = uint32_t(t.ax_levels(transcode_float::ae_vs(float(v)), levels, 0)[0]);
        *bits = t.ax_m[0];
        *digit = t.ax_d[0];
    }
}

extern "C" uint32_t astc_partition_cpu(uint32_t subsets, uint32_t partition) {
    transcode_float::Transcoder t;
    return subsets == 2 ? t.AX_PART2[partition] : t.AX_PART3[partition];
}
