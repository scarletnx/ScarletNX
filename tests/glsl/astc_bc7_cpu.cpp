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
#define work_groups work[0]
#define work_count work[3]
#define work_list (work + 4)
#define SNX_ASTC_BC7_DECLARATIONS                                                                                      \
    uint width, height, blocks_x, blocks_y, srgb, block_w, block_h, recip_w, recip_h, infill_ds, infill_dt, in_base;   \
    uvec2 block_dims;                                                                                                  \
    const leuvec4* astc_data;                                                                                          \
    leuvec4* bc7_blocks;                                                                                               \
    leuint* work;                                                                                                      \
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
// the full and the fast build in two passes, as vk/astc.c runs them (astc_bc7.glsl's SNX_BC7_PASS)
#undef SNX_BC7_LINES
#undef SNX_BC7_FAST
#define SNX_BC7_FAST 0
#undef SNX_BC7_PASS
#define SNX_BC7_PASS 1
struct FullA : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_FAST
#define SNX_BC7_FAST 0
#undef SNX_BC7_PASS
#define SNX_BC7_PASS 2
struct FullB : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_FAST
#define SNX_BC7_FAST 1
#undef SNX_BC7_PASS
#define SNX_BC7_PASS 1
struct FastA : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_FAST
#define SNX_BC7_FAST 1
#undef SNX_BC7_PASS
#define SNX_BC7_PASS 2
struct FastB : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_PASS
#define SNX_BC7_PASS 0
// the fast build's cuts of the search, for --measure: the ASTC blocks' hints (astc_bc7.glsl's SNX_BC7_HINTS) and the
// quick line of mode 6 kept under an error (bc7_encode.glsl's SNX_BC7_QUICK)
#undef SNX_BC7_LINES
#undef SNX_BC7_FAST
#define SNX_BC7_FAST 1
#undef SNX_BC7_HINTS
#define SNX_BC7_HINTS 0
#undef SNX_BC7_QUICK
#define SNX_BC7_QUICK 16
struct FastH0Q16 : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_HINTS
#define SNX_BC7_HINTS 0
#undef SNX_BC7_QUICK
#define SNX_BC7_QUICK 64
struct FastH0Q64 : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_HINTS
#define SNX_BC7_HINTS 0
#undef SNX_BC7_QUICK
#define SNX_BC7_QUICK 256
struct FastH0Q256 : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_HINTS
#define SNX_BC7_HINTS 1
#undef SNX_BC7_QUICK
#define SNX_BC7_QUICK 0
struct FastH1Q0 : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_HINTS
#define SNX_BC7_HINTS 1
#undef SNX_BC7_QUICK
#define SNX_BC7_QUICK 16
struct FastH1Q16 : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_HINTS
#define SNX_BC7_HINTS 1
#undef SNX_BC7_QUICK
#define SNX_BC7_QUICK 64
struct FastH1Q64 : Invocation {
    SNX_ASTC_BC7_DECLARATIONS
#include "nx/gpu/texture/astc_decode.glsl"
#include "nx/gpu/texture/bc7_encode.glsl"
#include "nx/gpu/texture/astc_bc7.glsl"
};
#undef SNX_BC7_HINTS
#define SNX_BC7_HINTS 1
#undef SNX_BC7_QUICK
#define SNX_BC7_QUICK 256
struct FastH1Q256 : Invocation {
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
        d.work = nullptr;
    });
    std::memcpy(out, blocks.data(), blocks.size() * 16);
}

// the two passes: the first over the strip's blocks, the second over the blocks it listed
template <typename A, typename B>
static void transcode2(const uint8_t* src, uint32_t w, uint32_t h, uint32_t depth, uint32_t bw, uint32_t bh, int srgb,
                       uint8_t* out, uint32_t* listed) {
    const uint32_t nx = (w + bw - 1) / bw, ny = (h + bh - 1) / bh, cx = (w + 3) / 4, cy = (h + 3) / 4;
    std::vector<glsl::leuvec4> in((size_t)nx * ny * depth);
    std::memcpy(in.data(), src, in.size() * 16);
    std::vector<glsl::leuvec4> blocks((size_t)cx * cy * depth, glsl::uvec4(0xdeadbeefu));
    std::vector<glsl::leuint> work(4 + blocks.size(), 0u);
    work[1] = work[2] = 1u;
    const auto setup = [&](auto& d) {
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
        d.work = work.data();
    };
    glsl::dispatch<A>(glsl::uvec3((cx + 63) / 64, cy, depth), glsl::uvec3(64u, 1u, 1u), setup);
    glsl::dispatch<B>(glsl::uvec3(uint32_t(work[0]), uint32_t(work[1]), uint32_t(work[2])), glsl::uvec3(64u, 1u, 1u),
                      setup);
    if (listed) *listed = uint32_t(work[3]);
    std::memcpy(out, blocks.data(), blocks.size() * 16);
}

extern "C" void astc_bc7_cpu_passes(uint32_t build, const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t depth,
                                    uint32_t bw, uint32_t bh, int srgb, uint8_t* out, uint32_t* listed) {
    if (build == 1)
        transcode2<astc_bc7_shader::FastA, astc_bc7_shader::FastB>(blocks, w, h, depth, bw, bh, srgb, out, listed);
    else
        transcode2<astc_bc7_shader::FullA, astc_bc7_shader::FullB>(blocks, w, h, depth, bw, bh, srgb, out, listed);
}

extern "C" void astc_bc7_cpu(uint32_t build, const uint8_t* blocks, uint32_t w, uint32_t h, uint32_t depth,
                             uint32_t bw, uint32_t bh, int srgb, uint8_t* out) {
    if (build == (1u | 0u | 16u << 8))
        transcode<astc_bc7_shader::FastH0Q16>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == (1u | 0u | 64u << 8))
        transcode<astc_bc7_shader::FastH0Q64>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == (1u | 0u | 256u << 8))
        transcode<astc_bc7_shader::FastH0Q256>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == (1u | 8u | 0u << 8))
        transcode<astc_bc7_shader::FastH1Q0>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == (1u | 8u | 16u << 8))
        transcode<astc_bc7_shader::FastH1Q16>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == (1u | 8u | 64u << 8))
        transcode<astc_bc7_shader::FastH1Q64>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == (1u | 8u | 256u << 8))
        transcode<astc_bc7_shader::FastH1Q256>(blocks, w, h, depth, bw, bh, srgb, out);
    else if ((build & 3) == 3)
        transcode<astc_bc7_shader::Quick>(blocks, w, h, depth, bw, bh, srgb, out);
    else if ((build & 3) == 2)
        transcode<astc_bc7_shader::Mode6>(blocks, w, h, depth, bw, bh, srgb, out);
    else if (build == 1)
        transcode<astc_bc7_shader::Fast>(blocks, w, h, depth, bw, bh, srgb, out);
    else
        transcode<astc_bc7_shader::Full>(blocks, w, h, depth, bw, bh, srgb, out);
}
