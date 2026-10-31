// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BC7 encoder's body (src/nx/gpu/texture/bc7_encode.glsl) run on the CPU through tests/glsl/glsl.hpp, dispatched
// as vk/astc.c dispatches bc7.comp: the expected blocks of tests/glsl/bc7_glsl.cpp and tests/astc_vulkan.c.
#include <cstdint>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "bc7_cpu.h"

namespace bc7_shader {
using namespace glsl;
// what bc7.comp declares, under its names
struct Encoder : Invocation {
    uint width, height, blocks_x, blocks_y;
    const leuint* rgba;
    leuvec4* bc7_blocks;
#include "nx/gpu/texture/bc7_encode.glsl"
};
} // namespace bc7_shader

extern "C" uint32_t bc7_weight_cpu(uint32_t bits, uint32_t index) {
    bc7_shader::Encoder e;
    e.sh_ib = bits;
    return e.Weight(index);
}

extern "C" uint32_t bc7_partition3_cpu(uint32_t partition) { return bc7_shader::Encoder().P3[partition]; }

extern "C" void bc7_encode_cpu(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t depth, uint8_t* out) {
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    std::vector<glsl::leuint> texels((size_t)w * h * depth);
    std::memcpy(texels.data(), rgba, texels.size() * 4);
    std::vector<glsl::leuvec4> blocks((size_t)bx * by * depth, glsl::uvec4(0xdeadbeefu));
    glsl::dispatch<bc7_shader::Encoder>(glsl::uvec3((bx + 63) / 64, by, depth), glsl::uvec3(64u, 1u, 1u),
                                        [&](bc7_shader::Encoder& e) {
                                            e.width = w;
                                            e.height = h;
                                            e.blocks_x = bx;
                                            e.blocks_y = by;
                                            e.rgba = texels.data();
                                            e.bc7_blocks = blocks.data();
                                        });
    std::memcpy(out, blocks.data(), blocks.size() * 16);
}
