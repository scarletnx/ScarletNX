// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The ASTC 4x4 encoder's body (src/nx/gpu/texture/astc_encode.glsl, over bc7_encode.glsl's fitter built for it) run
// on the CPU through tests/glsl/glsl.hpp: the expected blocks of tests/glsl/astc_encode_glsl.cpp.
#include <cstdint>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "astc_encode_cpu.h"

namespace astc_encode_shader {
using namespace glsl;
#define SNX_ASTC_ENC 1
struct Encoder : Invocation {
    uint width, height, blocks_x, blocks_y;
    const leuint* rgba;
    leuvec4* astc_blocks;
#include "nx/gpu/texture/astc_encode_tables.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_encode.glsl"
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
#undef SNX_ASTC_ENC
} // namespace astc_encode_shader

extern "C" void astc_encode_cpu(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t depth, uint8_t* out) {
    using astc_encode_shader::Encoder;
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    std::vector<glsl::leuint> texels((size_t)w * h * depth);
    std::memcpy(texels.data(), rgba, texels.size() * 4);
    std::vector<glsl::leuvec4> blocks((size_t)bx * by * depth, glsl::uvec4(0xdeadbeefu));
    glsl::dispatch<Encoder>(glsl::uvec3((bx + 63) / 64, by, depth), glsl::uvec3(64u, 1u, 1u), [&](Encoder& e) {
        e.width = w;
        e.height = h;
        e.blocks_x = bx;
        e.blocks_y = by;
        e.rgba = texels.data();
        e.astc_blocks = blocks.data();
    });
    std::memcpy(out, blocks.data(), blocks.size() * 16);
}

extern "C" uint32_t astc_encode_block_cpu(const uint32_t texels[16], uint8_t out[16]) {
    astc_encode_shader::Encoder e;
    e.hint_on = false;
    e.as_hint = 0u;
    for (int k = 0; k < 16; k++) e.px[k] = texels[k];
    const glsl::leuvec4 block = e.AstcEncode();
    std::memcpy(out, &block, 16);
    return e.as_err;
}
