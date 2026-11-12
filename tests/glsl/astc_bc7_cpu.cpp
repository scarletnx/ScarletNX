// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// astc_bc7.comp's body run on the CPU through tests/glsl/glsl.hpp, dispatched as vk/astc.c dispatches it: ASTC blocks
// straight into BC7 blocks, in each of the encoder's builds.
#include <cstdint>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "astc_bc7_cpu.h"

namespace astc_bc7_shader {
using namespace glsl;
// what astc_bc7.comp declares, under its names
#define SNX_ASTC_NO_MAIN
#define TILE(field, slot) astc_block[(field) + (slot)]
#define ASTC_SLOT 0u
#define SNX_ASTC_BC7_DECLARATIONS                                                                                      \
    uint width, height, blocks_x, blocks_y, srgb, block_w, block_h, recip_w, recip_h, infill_ds, infill_dt, in_base;   \
    uvec2 block_dims;                                                                                                  \
    const leuvec4* astc_data;                                                                                          \
    leuvec4* bc7_blocks;                                                                                               \
    uint astc_block[39];
struct Full : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_FAST
#define SNX_BC7_FAST 1
struct Fast : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_FAST
#define SNX_BC7_FAST 2
struct Mode6 : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_FAST
#define SNX_BC7_FAST 3
#undef SNX_BC7_LINES
struct Quick : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef TILE
} // namespace astc_bc7_shader

template <typename T>
static void transcode(const uint8_t* src, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh, int srgb,
                      uint8_t* out) {
    const uint32_t nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh, cx = (w + 3) / 4, cy = (h + 3) / 4;
    std::vector<glsl::leuvec4> in((size_t)nx * ny * depth);
    std::memcpy(in.data(), src, in.size() * 16);
    std::vector<glsl::leuvec4> blocks((size_t)cx * cy * depth, glsl::uvec4(0xdeadbeefu));
    glsl::dispatch<T>(glsl::uvec3((cx + 63) / 64, cy, depth), glsl::uvec3(64u, 1u, 1u), [&](T& d) {
        d.width = w;
        d.height = h;
        d.blocks_x = nx;
        d.blocks_y = ny;
        d.srgb = uint32_t(srgb);
        d.block_w = bw;
        d.block_h = bh;
        d.block_dims = glsl::uvec2(bw, bh);
        d.recip_w = (262144u + bw - 1u) / bw;
        d.recip_h = (262144u + bh - 1u) / bh;
        d.infill_ds = (1024u + bw / 2u) / (bw - 1u);
        d.infill_dt = (1024u + bh / 2u) / (bh - 1u);
        d.in_base = 0u;
        d.astc_data = in.data();
        d.bc7_blocks = blocks.data();
    });
    std::memcpy(out, blocks.data(), blocks.size() * 16);
}

extern "C" void astc_bc7_cpu(uint32_t build, const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t depth,
                             uint32_t bw, uint32_t bh, int srgb, uint8_t* out) {
    if (build == 3)
        transcode<astc_bc7_shader::Quick>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == 2)
        transcode<astc_bc7_shader::Mode6>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == 1)
        transcode<astc_bc7_shader::Fast>(blocks, w, h, depth, bw, bh, srgb, out);
    else
        transcode<astc_bc7_shader::Full>(blocks, w, h, depth, bw, bh, srgb, out);
}
