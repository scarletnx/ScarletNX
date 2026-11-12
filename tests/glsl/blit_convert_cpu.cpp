// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The 2D engine's blit between pixel formats (src/nx/gpu/texture/texconv.glsl built with SNX_TC_CONVERT, and its
// pixel_convert.glsl) run on the CPU through tests/glsl/glsl.hpp, dispatched as vk/texconv.c's vk_tc_blit dispatches
// it: the expected rows of tests/astc_vulkan.c.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "blit_convert_cpu.h"

namespace convert_shader {
using namespace glsl;
// the sRGB transfer and the filtered blit's interpolation (pixel_convert.glsl) are the build's floating point
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
// what texconv.comp declares, under its names, in its conversion build
#define SNX_TC_CONVERT 1
struct Converter : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
#undef SNX_TC_CONVERT
} // namespace convert_shader
using convert_shader::Converter;

extern "C" void blit_convert_cpu(uint32_t fmt, uint32_t nb, uint32_t rows, const uint8_t* in, uint32_t in_size,
                                 uint32_t src_at, uint32_t in_pitch, uint32_t col_at, uint32_t row_at, uint8_t* out,
                                 uint32_t out_pitch) {
    uint32_t units = (nb + 3) / 4;
    std::vector<glsl::leuint> words((in_size + 3) / 4), res((size_t)out_pitch / 4 * rows);
    std::memcpy(words.data(), in, in_size);
    std::memcpy(res.data(), out, res.size() * 4);
    glsl::dispatch<Converter>(glsl::uvec3((units + 63) / 64, rows, 1), glsl::uvec3(64, 1, 1), [&](Converter& c) {
        c.mode = TC_BLIT_CONVERT;
        c.units = units;
        c.rows = rows;
        c.in_pitch = in_pitch;
        c.out_pitch = out_pitch / 4;
        c.in_bias = src_at;
        c.out_bias = 0;
        c.remap = fmt;
        c.const_a = col_at / 4;
        c.const_b = row_at / 4;
        c.line_out = nb;
        c.src = words.data();
        c.dst = res.data();
    });
    std::memcpy(out, res.data(), res.size() * 4);
}
