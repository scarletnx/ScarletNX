// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The block linear shader's body (src/nx/gpu/texture/blocklinear.glsl) run on the CPU through tests/glsl/glsl.hpp,
// both ways, against gpu.c's bl_offset walked byte by byte: rows not a multiple of 16 or 64 bytes, blocks of 1 to 32
// GOBs high and 1 to 4 deep, moves that start inside a GOB and a block, volumes, and bound ranges that start before
// or after the layout's first byte (the biases). A deswizzle writes every linear byte of the rows it moves, a swizzle
// only valid row bytes, preserving a partial run's padding. The BL_RUNS build (blocklinear.comp) deswizzles the same
// with runs of its source read from results of their own (tab), and junk in the source there.
// tests/astc_vulkan.c runs the SPIR-V build on Vulkan.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "glsl.hpp"

namespace blocklinear_shader {
using namespace glsl;
// what blocklinear.comp declares, under its names
struct Mover : Invocation {
    uint runs, rows, lin_runs, y0, gob_y0, z0, bh, hmask, bd, dmask, bsh, block_row, slab, bl_bias, lin_bias, to_bl;
    uvec4 tail_mask;
    uint native;
    const uvec4* src;
    uvec4* dst;
#include "nx/gpu/texture/blocklinear.glsl"
};
#undef BL_READ
// the BL_RUNS build: blocklinear.comp's Tab, and a run's address as the host pointer to its result's words
struct InRun {
    uint start, end;
    uvec2 addr;
};
#define BL_RUNS
#define BL_RUN_WORD(a, k) (((const uvec4*)(uintptr_t)((uint64_t)(a).x | (uint64_t)(a).y << 32))[k])
struct RunsMover : Invocation {
    uint runs, rows, lin_runs, y0, gob_y0, z0, bh, hmask, bd, dmask, bsh, block_row, slab, bl_bias, lin_bias, to_bl;
    uvec4 tail_mask;
    uint native, tab_bias, nruns;
    const InRun* tab;
    const uvec4* src;
    uvec4* dst;
#include "nx/gpu/texture/blocklinear.glsl"
};
#undef BL_RUNS
#undef BL_READ
} // namespace blocklinear_shader
using blocklinear_shader::InRun;
using blocklinear_shader::Mover;
using blocklinear_shader::RunsMover;

static uint32_t rng = 0x9e3779b9;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

static int g_native; // an Ounce game's GOB, bits 5 and 6 of the offset swapped (blocklinear.h)

// gpu.c's bl_offset
static uint64_t bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t wb, uint32_t h, uint32_t bh, uint32_t bd) {
    uint64_t gobs_x = (wb + 63) >> 6, gy = y >> 3;
    uint64_t rows = ((((uint64_t)h + 7) >> 3) + (1u << bh) - 1) >> bh, block = (uint64_t)512 << (bh + bd);
    uint64_t in_block = ((((uint64_t)z & ((1u << bd) - 1)) << bh) + (gy & ((1u << bh) - 1))) << 9;
    uint64_t gx = g_native ? ((x & 32) << 3) + ((x & 16) << 2) : ((x & 32) << 3) + ((x & 16) << 1);
    uint64_t gyo = g_native ? ((y & 4) << 5) + ((y & 2) << 4) : ((y & 6) >> 1) << 6;
    return ((uint64_t)(x >> 6) << (9 + bh + bd)) + gx + (x & 15) +
           (((uint64_t)(z >> bd) * rows + (gy >> bh)) * gobs_x) * block + in_block + gyo + ((y & 1) << 4);
}

static unsigned failures;

// A deswizzle of the whole of a row_bytes x h surface with the BL_RUNS build: the source bound from pad runs on, with
// up to 6 runs of it held by results of their own (its words there in a vector apart, junk in the source), and the
// table starting at the first of them (tab_bias = pad). Every linear byte is the layout's, as bl_offset has it
static void one_runs(uint32_t row_bytes, uint32_t h, uint32_t bh) {
    uint32_t runs = (row_bytes + 15) / 16, lin_runs = runs, pad = random_word() % 5;
    uint64_t block_row = (uint64_t)((row_bytes + 63) >> 6) << (9 + bh);
    uint64_t size = (((((uint64_t)h + 7) >> 3) + (1u << bh) - 1) >> bh) * block_row;
    uint32_t units = (uint32_t)(size / 16);
    std::vector<glsl::uvec4> truth(units), bl(units + pad), lin((size_t)lin_runs * h);
    for (auto& v : truth) v = glsl::uvec4(random_word(), random_word(), random_word(), random_word());
    for (uint32_t i = 0; i < units; i++) bl[pad + i] = truth[i];
    // the results: sorted and apart, the first maybe at run 0 and the last maybe at the range's end
    std::vector<std::vector<glsl::uvec4>> held;
    std::vector<InRun> tab;
    uint32_t at = random_word() % 2 ? 0 : random_word() % (units / 4 + 1), n = 1 + random_word() % 6;
    for (uint32_t k = 0; k < n && at < units; k++) {
        uint32_t len = 1 + random_word() % (units / 4 + 1);
        if (at + len > units || k + 1 == n) len = units - at < len ? units - at : len;
        held.emplace_back(truth.begin() + at, truth.begin() + at + len);
        for (uint32_t i = 0; i < len; i++) bl[pad + at + i] = glsl::uvec4(0xEEEEEEEEu, 0xEEEEEEEEu, 0xEEEEEEEEu, 0xEEEEEEEEu);
        tab.push_back(InRun{at, at + len, glsl::uvec2(0, 0)});
        at += len + 1 + random_word() % 4;
    }
    for (size_t k = 0; k < tab.size(); k++) {
        uint64_t a = (uint64_t)(uintptr_t)held[k].data();
        tab[k].addr = glsl::uvec2((uint32_t)a, (uint32_t)(a >> 32));
    }
    glsl::dispatch<RunsMover>(glsl::uvec3((runs + 7) / 8, (h + 7) >> 3, 1), glsl::uvec3(32, 2, 1), [&](RunsMover& m) {
        m.runs = runs, m.rows = h, m.lin_runs = lin_runs, m.y0 = 0, m.gob_y0 = 0, m.z0 = 0, m.bh = bh;
        m.hmask = (1u << bh) - 1, m.bd = 0, m.dmask = 0, m.bsh = 5 + bh, m.block_row = (uint32_t)(block_row / 16);
        m.slab = (uint32_t)(size / 16), m.bl_bias = pad, m.lin_bias = 0, m.to_bl = 0, m.native = (uint32_t)g_native;
        m.tail_mask = glsl::uvec4(~0u, ~0u, ~0u, ~0u);
        m.tab_bias = pad, m.nruns = (uint32_t)tab.size(), m.tab = tab.data();
        m.src = bl.data(), m.dst = lin.data();
    });
    const uint8_t* b = (const uint8_t*)truth.data();
    const uint8_t* l = (const uint8_t*)lin.data();
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < runs * 16; x++)
            if (b[bl_offset(x, y, 0, row_bytes, h, bh, 0)] != l[(uint64_t)y * lin_runs * 16 + x] && failures++ < 10)
                std::printf("runs deswizzle %ux%u bh %u, %zu runs held: (%u, %u)\n", row_bytes, h, bh, tab.size(), x, y);
}

// one move of rows y0 to y0 + rows of slices z0 to z0 + slices of a row_bytes x h x d surface, as vk/blocklinear.c sets
// it up: the block linear side bound from its byte lo, pad runs before it, the linear side from lin_pad runs on
static void one(uint32_t row_bytes, uint32_t h, uint32_t d, uint32_t bh, uint32_t bd, uint32_t y0, uint32_t rows,
                uint32_t z0, uint32_t slices) {
    uint32_t runs = (row_bytes + 15) / 16, lin_runs = runs + random_word() % 3, pad = random_word() % 5;
    uint32_t lin_pad = random_word() % 5;
    uint64_t block_row = (uint64_t)((row_bytes + 63) >> 6) << (9 + bh + bd);
    uint64_t slab = (((((uint64_t)h + 7) >> 3) + (1u << bh) - 1) >> bh) * block_row;
    uint64_t size = ((d + (1u << bd) - 1) >> bd) * slab;
    uint64_t lo = (uint64_t)(z0 >> bd) * slab + (uint64_t)((y0 >> 3) >> bh) * block_row;
    std::vector<glsl::uvec4> bl(size / 16 + pad), lin((size_t)lin_runs * rows * slices + lin_pad);
    for (int to_bl = 0; to_bl < 2; to_bl++) {
        for (auto& v : bl) v = glsl::uvec4(random_word(), random_word(), random_word(), random_word());
        for (auto& v : lin) v = glsl::uvec4(random_word(), random_word(), random_word(), random_word());
        std::vector<glsl::uvec4> bl0 = bl, lin0 = lin;
        // the bound block linear range starts at lo, pad runs into the buffer: run 0 of the layout is pad - lo / 16
        uint32_t bl_bias = pad - (uint32_t)(lo / 16);
        glsl::uvec4* bl_bound = bl.data() + 0;
        glsl::dispatch<Mover>(glsl::uvec3((runs + 7) / 8, ((y0 + rows + 7) >> 3) - (y0 >> 3), slices),
                              glsl::uvec3(32, 2, 1), [&](Mover& m) {
                                  m.runs = runs, m.rows = rows, m.lin_runs = lin_runs, m.y0 = y0, m.gob_y0 = y0 >> 3;
                                  m.z0 = z0, m.bh = bh, m.hmask = (1u << bh) - 1, m.bd = bd, m.dmask = (1u << bd) - 1;
                                  m.bsh = 5 + bh + bd, m.block_row = (uint32_t)(block_row / 16);
                                  m.slab = (uint32_t)(slab / 16), m.to_bl = (uint32_t)to_bl, m.native = (uint32_t)g_native;
                                  // the block linear buffer starts lo - pad * 16 bytes into the layout
                                  m.bl_bias = bl_bias, m.lin_bias = lin_pad;
                                  m.src = to_bl ? lin.data() : bl_bound;
                                  m.dst = to_bl ? bl_bound : lin.data();
                                  for (uint32_t c = 0; c < 4; c++) {
                                      uint32_t tail = row_bytes % 16;
                                      uint32_t bytes = !tail ? 4 : tail > 4 * c ? tail - 4 * c : 0;
                                      m.tail_mask[c] = bytes >= 4 ? ~0u : (1u << (bytes * 8)) - 1;
                                  }
                              });
        const uint8_t* b = (const uint8_t*)bl.data() + pad * 16 - lo;
        const uint8_t* l = (const uint8_t*)(lin.data() + lin_pad);
        std::vector<uint8_t> moved(bl.size() * 16, 0), lmoved(lin.size() * 16, 0);
        for (uint32_t z = 0; z < slices; z++)
            for (uint32_t y = 0; y < rows; y++)
                for (uint32_t x = 0; x < (to_bl ? row_bytes : runs * 16); x++) {
                    uint64_t o = bl_offset(x, y0 + y, z0 + z, row_bytes, h, bh, bd);
                    uint64_t li = ((uint64_t)z * rows + y) * lin_runs * 16 + x;
                    moved[pad * 16 - lo + o] = 1;
                    lmoved[lin_pad * 16 + li] = 1;
                    if (b[o] != l[li] && failures++ < 10)
                        std::printf("%s %ux%ux%u bh %u bd %u rows %u+%u slices %u+%u: (%u, %u, %u)\n",
                                    to_bl ? "swizzle" : "deswizzle", row_bytes, h, d, bh, bd, y0, rows, z0, slices, x,
                                    y0 + y, z0 + z);
                }
        // nothing else written: the side written is as it was outside the moved runs
        const uint8_t* w = to_bl ? (const uint8_t*)bl.data() : (const uint8_t*)lin.data();
        const uint8_t* w0 = to_bl ? (const uint8_t*)bl0.data() : (const uint8_t*)lin0.data();
        const std::vector<uint8_t>& mask = to_bl ? moved : lmoved;
        for (size_t i = 0; i < mask.size(); i++)
            if (!mask[i] && w[i] != w0[i] && failures++ < 10)
                std::printf("%s %ux%ux%u bh %u bd %u: byte %zu outside the moved runs written\n",
                            to_bl ? "swizzle" : "deswizzle", row_bytes, h, d, bh, bd, i);
    }
}

int main() {
    for (g_native = 0; g_native < 2; g_native++) { // the Maxwell GOB, then an Ounce game's
    for (uint32_t width = 1; width <= 33; width++) one(width, 3, 2, 0, 0, 0, 3, 0, 2);
    for (int i = 0; i < 400; i++) {
        uint32_t row_bytes = 1 + random_word() % 400, h = 1 + random_word() % 90, bh = random_word() % 6;
        uint32_t d = 1, bd = 0;
        if (i % 3 == 0) {
            d = 1 + random_word() % 9;
            bd = random_word() % 3;
        }
        uint32_t y0 = random_word() % h, rows = 1 + random_word() % (h - y0);
        uint32_t z0 = random_word() % d, slices = 1 + random_word() % (d - z0);
        if (i % 4 == 0) y0 = 0, rows = h, z0 = 0, slices = d;
        one(row_bytes, h, d, bh, bd, y0, rows, z0, slices);
        one_runs(row_bytes, h, bh);
    }
    }
    if (failures) {
        std::printf("blocklinear_glsl: %u failures\n", failures);
        return 1;
    }
    std::puts("blocklinear_glsl: the shader moves every run where bl_offset puts it, and nothing else, in both GOBs, "
              "and its BL_RUNS build reads the runs that results hold from them");
    return 0;
}
