// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// One vendor's ASTC encoder and, but for the generic one, which bcn_astc_cpu.cpp runs, its transcoder: astc_vendor_cpu.cpp
// includes this once a vendor, with SNX_ASTC_VENDOR and ASTC_VENDOR_NS defined. The encoder is astc_encode_cpu.cpp's,
// the transcoder's four builds bcn_astc_cpu.cpp's, each with the vendor's encoder (texconv.glsl's SNX_ASTC_VENDOR)
namespace ASTC_VENDOR_NS {
using namespace glsl;
#define SNX_ASTC_ENC 1
struct Encoder : Invocation {
    uint width, height, blocks_x, blocks_y;
    const leuint* rgba;
    leuvec4* astc_blocks;
#include "nx/gpu/texture/astc_encode_tables.glsl"
#include "nx/gpu/texture/astc_quant.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#if SNX_ASTC_VENDOR == 1
#include "nx/gpu/texture/astc_encode_arm.glsl"
#elif SNX_ASTC_VENDOR == 2
#include "nx/gpu/texture/astc_encode_qcom.glsl"
#else
#include "nx/gpu/texture/astc_encode.glsl"
#endif
    void main() {
        const uint bx = gl_GlobalInvocationID.x, by = gl_GlobalInvocationID.y, z = gl_WorkGroupID.z;
        if (bx >= blocks_x || by >= blocks_y) return;
        hint_on = false;
        as_hint = 0u; // a picture says nothing of its blocks
        for (uint k = 0u; k < 16u; k++) {
            const uint x = min(bx * 4u + (k & 3u), width - 1u), y = min(by * 4u + (k >> 2u), height - 1u);
            px[k] = rgba[(z * height + y) * width + x];
        }
        astc_blocks[(z * blocks_y + by) * blocks_x + bx] = AstcEncode();
    }
};
#if SNX_ASTC_VENDOR != 0
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
#define SNX_TC_ASTC 1
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
// all of it in one invocation
struct Transcoder : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
// the three passes vk/texconv.c dispatches
#undef SNX_ASTC_PASS
#define SNX_ASTC_PASS 1
#define work_head work
#define work_list (work + 8)
struct TranscoderFirst : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
    leuint* work;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
#undef SNX_ASTC_PASS
#define SNX_ASTC_PASS 2
struct TranscoderSecond : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
    leuint* work;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
#undef SNX_ASTC_STOP
#undef SNX_NO_ARRAYS
#undef SNX_ASTC_PASS
#define SNX_ASTC_PASS 3
struct TranscoderThird : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
    leuint* work;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
#pragma GCC diagnostic pop
#undef work_head
#undef work_list
#undef SNX_ASTC_PASS
#undef SNX_TC_ASTC
#endif
#undef SNX_ASTC_ENC
} // namespace ASTC_VENDOR_NS
