// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The ASTC 4x4 encoder of each GPU vendor (src/nx/gpu/texture/astc_encode.glsl, astc_encode_arm.glsl and
// astc_encode_qcom.glsl, over bc7_encode.glsl's fitter) run on the CPU through tests/glsl/glsl.hpp, alone and in the
// BCn to ASTC transcoder as vk/texconv.c dispatches it: the expected blocks of tests/glsl/astc_encode_glsl.cpp and
// tests/astc_vulkan.c for each vendor's build.
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "astc_encode_cpu.h"
#include "astc_vendor_cpu.h"
#include "bcn_etc2_cpu.h"

#define SNX_ASTC_VENDOR 0
#define ASTC_VENDOR_NS  astc_generic_shader
#include "astc_vendor_cpu.inc"
#undef ASTC_VENDOR_NS
#undef SNX_ASTC_VENDOR
#define SNX_ASTC_VENDOR 1
#define ASTC_VENDOR_NS  astc_arm_shader
#include "astc_vendor_cpu.inc"
#undef ASTC_VENDOR_NS
#undef SNX_ASTC_VENDOR
#define SNX_ASTC_VENDOR 2
#define ASTC_VENDOR_NS  astc_qcom_shader
#include "astc_vendor_cpu.inc"
#undef ASTC_VENDOR_NS
#undef SNX_ASTC_VENDOR

static const uint32_t TC_BCN_ASTC_MODE = 50; // texconv_modes.glsl's TC_BCN_ASTC

template <typename E> static void encode(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t depth, uint8_t* out) {
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    std::vector<glsl::leuint> texels((size_t)w * h * depth);
    std::memcpy(texels.data(), rgba, texels.size() * 4);
    std::vector<glsl::leuvec4> blocks((size_t)bx * by * depth, glsl::uvec4(0xdeadbeefu));
    glsl::dispatch<E>(glsl::uvec3((bx + 63) / 64, by, depth), glsl::uvec3(64u, 1u, 1u), [&](E& e) {
        e.width = w;
        e.height = h;
        e.blocks_x = bx;
        e.blocks_y = by;
        e.rgba = texels.data();
        e.astc_blocks = blocks.data();
    });
    std::memcpy(out, blocks.data(), blocks.size() * 16);
}
extern "C" void astc_encode_vendor_cpu(int vendor, const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t depth,
                                       uint8_t* out) {
    if (vendor == 1)
        encode<astc_arm_shader::Encoder>(rgba, w, h, depth, out);
    else if (vendor == 2)
        encode<astc_qcom_shader::Encoder>(rgba, w, h, depth, out);
    else
        encode<astc_generic_shader::Encoder>(rgba, w, h, depth, out);
}

template <typename E> static uint32_t encode_block(const uint32_t texels[16], uint8_t out[16]) {
    E e;
    e.hint_on = false;
    e.as_hint = 0u;
    for (int k = 0; k < 16; k++) e.px[k] = texels[k];
    const glsl::leuvec4 block = e.AstcEncode();
    std::memcpy(out, &block, 16);
    return e.as_err;
}
extern "C" uint32_t astc_encode_block_vendor_cpu(int vendor, const uint32_t texels[16], uint8_t out[16]) {
    return vendor == 1   ? encode_block<astc_arm_shader::Encoder>(texels, out)
           : vendor == 2 ? encode_block<astc_qcom_shader::Encoder>(texels, out)
                         : encode_block<astc_generic_shader::Encoder>(texels, out);
}

template <typename E> static void pack(const AstcCandidate* c, uint8_t out[16]) {
    E e;
    e.as_np = c->np;
    e.as_cem = c->cem;
    e.as_enc = c->enc;
    e.as_ib = c->ib;
    e.as_s0a = c->s0a;
    e.as_s1a = c->s1a;
    e.as_s0b = c->s0b;
    e.as_s1b = c->s1b;
    e.as_lo = c->lo;
    e.as_hi = c->hi;
    e.as_dual = c->dual;
    e.as_plane = c->plane;
    e.as_lo2 = c->lo2;
    e.as_hi2 = c->hi2;
    e.as_part = c->part;
    const glsl::leuvec4 block = e.AsPack();
    std::memcpy(out, &block, 16);
}
extern "C" void astc_pack_vendor_cpu(int vendor, const AstcCandidate* c, uint8_t out[16]) {
    if (vendor == 1)
        pack<astc_arm_shader::Encoder>(c, out);
    else if (vendor == 2)
        pack<astc_qcom_shader::Encoder>(c, out);
    else
        pack<astc_generic_shader::Encoder>(c, out);
}

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
extern "C" void bcn_astc_vendor_cpu(int vendor, int kind, const uint8_t* blocks, uint32_t bw, uint32_t bh,
                                    uint32_t slices, uint8_t* out) {
    if (vendor == 1)
        transcode<astc_arm_shader::Transcoder>(kind, blocks, bw, bh, slices, out);
    else if (vendor == 2)
        transcode<astc_qcom_shader::Transcoder>(kind, blocks, bw, bh, slices, out);
    else
        bcn_astc_cpu(kind, blocks, bw, bh, slices, out);
}

template <typename T>
static void pass(int kind, uint32_t n, uint32_t groups, const glsl::leuint* in, glsl::leuint* res, glsl::leuint* work) {
    glsl::dispatch<T>(glsl::uvec3(groups, 1, 1), glsl::uvec3(64, 1, 1), [&](T& t) {
        t.mode = TC_BCN_ASTC_MODE + (uint32_t)kind;
        t.units = n;
        t.rows = 1;
        t.in_pitch = n * (kind == 0 ? 2 : 4);
        t.out_pitch = n * 4;
        t.in_bias = 0;
        t.out_bias = 0;
        t.const_a = n;
        t.src = in;
        t.dst = res;
        t.work = work;
    });
}
template <typename First, typename Second, typename Third>
static void passes(int kind, const uint8_t* blocks, uint32_t n, uint8_t* out, uint32_t listed[2]) {
    std::vector<glsl::leuint> in((size_t)n * (kind == 0 ? 2 : 4)), res((size_t)n * 4), work(8 + 2 * (size_t)n, 0u);
    std::memcpy(in.data(), blocks, in.size() * 4);
    pass<First>(kind, n, (n + 63) / 64, in.data(), res.data(), work.data());
    const uint32_t before = uint32_t(work[7]);
    pass<Second>(kind, n, uint32_t(work[0]), in.data(), res.data(), work.data());
    if (listed) {
        listed[0] = uint32_t(work[3]);
        listed[1] = uint32_t(work[7]) - before;
    }
    pass<Third>(kind, n, uint32_t(work[4]), in.data(), res.data(), work.data());
    std::memcpy(out, res.data(), res.size() * 4);
}
extern "C" void bcn_astc_vendor_passes_cpu(int vendor, int kind, const uint8_t* blocks, uint32_t n, uint8_t* out,
                                           uint32_t listed[2]) {
    if (vendor == 1)
        passes<astc_arm_shader::TranscoderFirst, astc_arm_shader::TranscoderSecond, astc_arm_shader::TranscoderThird>(
            kind, blocks, n, out, listed);
    else if (vendor == 2)
        passes<astc_qcom_shader::TranscoderFirst, astc_qcom_shader::TranscoderSecond,
               astc_qcom_shader::TranscoderThird>(kind, blocks, n, out, listed);
    else
        bcn_astc_passes_cpu(kind, blocks, n, out, listed);
}
