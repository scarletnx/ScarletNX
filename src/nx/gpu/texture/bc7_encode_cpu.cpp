// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The BC7 encoder's fast build run on the CPU: bc7_encode.glsl's body through glsl.hpp, as tests/glsl/bc7_cpu.cpp runs
// it for the tests, an invocation (a block) at a time. The arithmetic is integer only, so the blocks are the GPU's
// byte for byte, and the background encodes may store them under the keys the GPU's encodes are stored under
// (vk/astc.c, docs/layers/gpu/astc.md). Each block is a fresh invocation, as on the GPU: the body's globals are its own
#include "nx/gpu/texture/glsl.hpp"
#include "nx/gpu/texture/bc7_encode_cpu.h"

namespace {
using namespace glsl;
#define SNX_BC7_FAST 1
// what bc7.comp declares, under its names
struct EncoderFast : Invocation {
    uint width, height, blocks_x, blocks_y;
    const leuint* rgba;
    leuvec4* bc7_blocks;
#include "nx/gpu/texture/bc7_encode.glsl"
};
} // namespace

extern "C" void bc7_encode_cpu_strip(const uint8_t* rgba, uint32_t w, uint32_t h, uint32_t slices, uint8_t* out) {
    const uint32_t bx = (w + 3) / 4, by = (h + 3) / 4;
    for (uint32_t z = 0; z < slices; z++)
        for (uint32_t y = 0; y < by; y++)
            for (uint32_t x = 0; x < bx; x++) {
                EncoderFast e;
                e.width = w;
                e.height = h;
                e.blocks_x = bx;
                e.blocks_y = by;
                e.rgba = reinterpret_cast<const leuint*>(rgba);
                e.bc7_blocks = reinterpret_cast<leuvec4*>(out);
                e.gl_NumWorkGroups = uvec3((bx + 63) / 64, by, slices);
                e.gl_WorkGroupSize = uvec3(64u, 1u, 1u);
                e.gl_WorkGroupID = uvec3(x / 64, y, z);
                e.gl_LocalInvocationID = uvec3(x % 64, 0u, 0u);
                e.gl_GlobalInvocationID = uvec3(x, y, z);
                e.gl_LocalInvocationIndex = x % 64;
                e.main();
            }
}
