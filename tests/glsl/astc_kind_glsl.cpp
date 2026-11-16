// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// astc_kind.comp's body run on the CPU (tests/glsl/glsl.hpp): what an ASTC block can hold, read on the GPU from the
// blocks a texture keeps (vk/astc.c's vk_astc_kind_read), against gpu/texture/astc_kind.h's CPU reading. No GPU is
// needed.
//
// - Block for block: BlockKind gives snx_astc_block_kind's bits, every footprint, in the sRGB decode mode and not, on
//   the block sets of the other tests here (random bytes, one in two of one LDR endpoint mode, as astc_bcn_glsl.cpp;
//   every block mode with void extents valid and not, as astc_glsl.cpp; runs of one colour, as astc_bc7_glsl.cpp), on
//   void extents of random colours, grey or not, on RGBA direct blocks whose ends lie up to 3 levels apart, where
//   SNX_ASTC_GREY_SPREAD decides, and on base and offset blocks whose ends the decoders clamp at white.
// - Level for level: the workgroups' first block of each bit (main), through snx_astc_level_stop and
//   snx_astc_level_kind_firsts, give snx_astc_level_kind's kind, its early stop too, on levels of mostly one kind
//   with blocks of others among them, pitch and block linear, in layouts cut short of their blocks, a few layers in
//   one dispatch.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "glsl.hpp"
#include "support/astc_kind_ref.h"

namespace astc_kind_shader {
using namespace glsl;
#define KIND_FIRST(i) shared_first[i]
struct Kind : WorkgroupInvocation {
    uint blocks_x = 0, blocks_y = 0, block_w = 0, block_h = 0, srgb = 0, in_base = 0, walk_bl = 0, out_base = 0,
         out_layer = 0;
    const leuvec4* astc_data = nullptr;
    leuint* first_words = nullptr;
    uint* shared_first = nullptr;
#include "nx/gpu/texture/astc_kind.glsl"
};
#undef KIND_FIRST
struct Shared {
    uint first[4];
};
} // namespace astc_kind_shader
static_assert(SNX_ASTC_KIND_GREY_SPREAD == SNX_ASTC_GREY_SPREAD, "astc_kind.glsl's grey is astc_kind.h's");

static uint32_t rng = 0x13579bdf;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}
static void put_words(uint8_t* b, const uint32_t w[4]) {
    for (int k = 0; k < 16; k++) b[k] = uint8_t(w[k / 4] >> (k % 4 * 8));
}

// block i of set s (0 to 5)
static void block_of_set(int s, uint32_t i, uint8_t b[16]) {
    uint32_t w[4] = {random_word(), random_word(), random_word(), random_word()};
    if (s == 0) { // astc_bcn_glsl.cpp's: random bytes, one in two of one partition of an LDR endpoint mode
        put_words(b, w);
        if (random_word() % 2) {
            static const uint8_t cems[7] = {0, 1, 4, 5, 6, 8, 9};
            const uint32_t cem = cems[random_word() % 7];
            b[1] = uint8_t((b[1] & 0x80) | (cem << 5) | (b[1] & 7));
            b[2] = uint8_t((b[2] & 0xFE) | (cem >> 3));
        }
        return;
    }
    if (s == 1 || s == 2) { // astc_glsl.cpp's every block mode, and astc_bc7_glsl.cpp's runs of one colour too
        w[0] = (w[0] & ~2047u) | (i & 2047u);
        if (i % 29 == 0 || (s == 2 && i % 64 < 3)) {
            w[0] = 0xfffffdfcu;
            w[1] = ~0u;
            if (s == 2 && i % 64 < 3) w[2] = 0x40008000u, w[3] = 0xffffc000u;
        } else if (i % 31 == 0) {
            w[0] = 0xfffff9fcu; // invalid reserved bit
        }
        put_words(b, w);
        return;
    }
    if (s == 3) { // void extents of random colours, one in three grey, one in three opaque, some HDR or reserved
        w[0] = 0xfffffdfcu;
        w[1] = ~0u;
        const uint32_t r = w[2] & 0xFFFF, g = random_word() % 3 ? w[2] >> 16 : r,
                       bl = random_word() % 3 ? w[3] & 0xFFFF : r, a = random_word() % 3 ? w[3] >> 16 : 0xFFFF;
        w[2] = r | (random_word() % 4 ? g : r) << 16;
        w[3] = bl | a << 16;
        if (random_word() % 16 == 0) w[0] ^= 1u << (9 + random_word() % 3); // HDR, or the reserved bits
        put_words(b, w);
        return;
    }
    // one partition, a 4x4 grid of 2 bit weights (block mode 0x42), 8 bits a colour value, one in eight dual plane.
    // Set 4: RGBA direct (mode 12), each end's G and B up to 3 levels over R in their values
    // (tests/vulkan_astc_kind.inc's normal maps). Set 5: RGB and RGBA base and offset (modes 9 and 13), bases near
    // white and offsets that take ends past it, which the decoders clamp, a channel or two a level or three below the
    // others
    uint32_t v[8], cem = 12;
    for (uint32_t k = 0; k < 8; k++) v[k] = random_word() & 255;
    if (s == 4) {
        const uint32_t apart = random_word() % 4;
        for (uint32_t e = 0; e < 2; e++) {
            v[2 + e] = v[e] + apart > 255 ? 255 : v[e] + apart;
            v[4 + e] = v[e] + 2 * apart > 255 ? 255 : v[e] + 2 * apart;
        }
    } else {
        cem = random_word() % 2 ? 9 : 13;
        const uint32_t top = 236 + random_word() % 20;
        const int off = int(random_word() % 24) - 6;
        for (uint32_t c = 0; c < 3; c++) {
            // a base of 8 bits and a signed offset of 6 (snx_astc_rgb_spread's bit transfer, undone)
            const uint32_t base = top - (random_word() % 3 == 0 ? random_word() % 4 : 0);
            const uint32_t o = uint32_t(off + int(random_word() % 3 == 0 ? random_word() % 3 : 0)) & 0x3F;
            v[2 * c] = (base & 0x7F) << 1 | (random_word() & 1);
            v[2 * c + 1] = (base & 0x80) | o << 1 | (random_word() & 1);
        }
    }
    uint64_t lo = 0x42u | (uint64_t)cem << 13 | (random_word() % 8 == 0 ? 0x400u : 0u), hi = 0;
    for (uint32_t k = 0; k < 8; k++) {
        const uint32_t at = 17 + 8 * k;
        if (at < 64) lo |= (uint64_t)v[k] << at;
        if (at + 8 > 64) hi |= at >= 64 ? (uint64_t)v[k] << (at - 64) : (uint64_t)v[k] >> (64 - at);
    }
    hi |= (uint64_t)random_word() << 32;
    for (int k = 0; k < 8; k++) b[k] = uint8_t(lo >> (8 * k)), b[8 + k] = uint8_t(hi >> (8 * k));
}

static int failures;
static const uint32_t footprints[][2] = {{4, 4}, {5, 4},  {5, 5},  {6, 5},  {6, 6},   {8, 5},   {8, 6},
                                         {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};

static uint32_t glsl_kind(const uint8_t b[16], uint32_t bw, uint32_t bh, int srgb) {
    astc_kind_shader::Kind k;
    glsl::uvec4 v;
    for (int c = 0; c < 4; c++)
        v[c] = uint32_t(b[4 * c] | b[4 * c + 1] << 8 | b[4 * c + 2] << 16) | uint32_t(b[4 * c + 3]) << 24;
    k.kind_blk = v;
    return k.BlockKind(bw, bh, uint32_t(srgb));
}

// every block of each set at each footprint and decode mode: the same bits; and how many had each, which shows the
// sets reach every bit and kind 0
static void blocks() {
    static const char* names[6] = {"random, one in two of an LDR mode",
                                   "every block mode",
                                   "runs of one colour",
                                   "void extents",
                                   "RGBA ends near grey",
                                   "base and offset ends past white"};
    for (int s = 0; s < 6; s++) {
        uint64_t seen[16] = {0}, n = 0;
        const int before = failures;
        for (const auto& f : footprints)
            for (int srgb = 0; srgb < 2; srgb++)
                for (uint32_t i = 0; i < 12000; i++) {
                    uint8_t b[16];
                    block_of_set(s, i, b);
                    const uint32_t want = snx_astc_block_kind(b, f[0], f[1], srgb),
                                   got = glsl_kind(b, f[0], f[1], srgb);
                    seen[want & 15]++;
                    n++;
                    if (got != want && failures++ < 20)
                        std::fprintf(stderr,
                                     "FAIL %s, %ux%u srgb=%d: block %02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
                                     "%02x%02x%02x%02x is kind %u, the GLSL says %u\n",
                                     names[s], f[0], f[1], srgb, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8],
                                     b[9], b[10], b[11], b[12], b[13], b[14], b[15], want, got);
                }
        uint64_t bits[5] = {seen[0], 0, 0, 0, 0};
        for (uint32_t k = 0; k < 16; k++)
            for (uint32_t b = 0; b < 4; b++) bits[b + 1] += k >> b & 1 ? seen[k] : 0;
        std::printf("%s %s: %llu blocks the same kind, every footprint and decode mode (kind 0 %llu, chroma %llu, "
                    "alpha %llu, lines %llu, other %llu)\n",
                    failures == before ? "PASS" : "FAIL", names[s], (unsigned long long)n, (unsigned long long)bits[0],
                    (unsigned long long)bits[1], (unsigned long long)bits[2], (unsigned long long)bits[3],
                    (unsigned long long)bits[4]);
    }
}

// blocks of each kind (0 to 15) at a footprint and decode mode, found among random ones
struct Pools {
    std::vector<uint8_t> kind[16];
    void fill(uint32_t bw, uint32_t bh, int srgb) {
        for (uint32_t i = 0; i < 40000; i++) {
            uint8_t b[16];
            block_of_set(int(i % 6), i, b);
            std::vector<uint8_t>& p = kind[snx_astc_block_kind(b, bw, bh, srgb)];
            if (p.size() < 64 * 16) p.insert(p.end(), b, b + 16);
        }
    }
    // a block of kind k, or of kind 0 where there is none of it
    const uint8_t* pick(uint32_t k) const {
        const std::vector<uint8_t>& p = kind[k].empty() ? kind[0] : kind[k];
        return p.data() + (size_t)(random_word() % (p.size() / 16)) * 16;
    }
};

// a level of nx x ny blocks in each of layers layers, mostly of one kind, block linear (lbh, gobs_x past its own) or
// pitch linear, its memory cut short of its blocks one time in four: the kind the GPU's first blocks make, against the
// CPU's walk of each layer
static void level(const Pools& pools, uint32_t bw, uint32_t bh, int srgb, int bl, uint32_t nx, uint32_t ny,
                  uint32_t layers) {
    static const uint32_t usual[5] = {0, SNX_ASTC_ALPHA, SNX_ASTC_CHROMA, SNX_ASTC_CHROMA | SNX_ASTC_LINES,
                                      SNX_ASTC_CHROMA | SNX_ASTC_ALPHA};
    const uint32_t base = usual[random_word() % 5], rare = 4 + random_word() % 60;
    std::vector<glsl::leuvec4> kept((size_t)nx * ny * layers);
    for (size_t i = 0; i < kept.size(); i++) {
        const uint32_t k = random_word() % rare ? base : random_word() % 16;
        const uint8_t* b = pools.pick(k);
        glsl::uvec4 v;
        for (int c = 0; c < 4; c++)
            v[c] = uint32_t(b[4 * c] | b[4 * c + 1] << 8 | b[4 * c + 2] << 16) | uint32_t(b[4 * c + 3]) << 24;
        kept[i] = v;
    }
    const uint32_t lbh = bl ? random_word() % 4 : 0, gxn = (nx + 3) / 4,
                   gobs_x = bl ? gxn + random_word() % 3 - (random_word() % 16 == 0 ? 1 : 0) : 0,
                   pitch = bl ? 0 : nx * 16 + 16 * (random_word() % 3);
    const uint64_t rows = ((((uint64_t)ny + 7) >> 3) + (1u << lbh) - 1) >> lbh;
    const uint64_t full = bl ? rows * (gobs_x ? gobs_x : 1) * (512ull << lbh) : (uint64_t)pitch * ny;
    std::vector<glsl::leuint> first((size_t)layers * 4, glsl::leuint(~0u));
    glsl::dispatch_workgroups<astc_kind_shader::Kind, astc_kind_shader::Shared>(
        glsl::uvec3((nx + 63) / 64, ny, layers), glsl::uvec3(64u, 1u, 1u),
        [&](astc_kind_shader::Kind& k, astc_kind_shader::Shared& sh) {
            k.blocks_x = nx;
            k.blocks_y = ny;
            k.block_w = bw;
            k.block_h = bh;
            k.srgb = uint32_t(srgb);
            k.in_base = 0;
            k.walk_bl = uint32_t(bl);
            k.out_base = 0;
            k.out_layer = 4;
            k.astc_data = kept.data();
            k.first_words = first.data();
            k.shared_first = sh.first;
        });
    for (uint32_t z = 0; z < layers; z++) {
        std::vector<uint8_t> mem(full + 512);
        for (auto& c : mem) c = uint8_t(random_word());
        for (uint32_t y = 0; y < ny; y++)
            for (uint32_t x = 0; x < nx; x++) {
                const glsl::uvec4 v = kept[((size_t)z * ny + y) * nx + x];
                uint8_t b[16];
                const uint32_t w[4] = {v.x, v.y, v.z, v.w};
                put_words(b, w);
                const uint64_t at = bl ? (gobs_x >= gxn ? snx_astc_gob_at(0, y / 8, x / 4, rows, gobs_x, lbh, 0) +
                                                              gob_x_offset((x & 3) * 16) + gob_y_offset(y & 7)
                                                        : 0)
                                       : (uint64_t)y * pitch + x * 16;
                if (!bl || gobs_x >= gxn) std::memcpy(&mem[at], b, 16);
            }
        const uint64_t size = random_word() % 4 ? full : full - random_word() % (full + 1);
        const uint32_t want = snx_astc_level_kind(mem.data(), size, nx, ny, 1, pitch, gobs_x, lbh, 0, bw, bh, srgb);
        const uint32_t firsts[4] = {first[z * 4], first[z * 4 + 1], first[z * 4 + 2], first[z * 4 + 3]};
        const uint32_t got =
            snx_astc_level_kind_firsts(firsts, snx_astc_level_stop(size, nx, ny, 1, pitch, gobs_x, lbh, 0), srgb);
        if (got != want && failures++ < 20)
            std::fprintf(stderr,
                         "FAIL a %ux%u level of %ux%u blocks, layer %u of %u, %s (lbh %u, %u GOBs across), %llu of "
                         "%llu bytes, srgb=%d, mostly kind %u: the walk's kind %u, the GPU's first blocks %u %u %u %u "
                         "say %u\n",
                         nx, ny, bw, bh, z, layers, bl ? "block linear" : "pitch linear", lbh, gobs_x,
                         (unsigned long long)size, (unsigned long long)full, srgb, base, want, firsts[0], firsts[1],
                         firsts[2], firsts[3], got);
    }
}

static void levels() {
    const int before = failures;
    uint32_t n = 0;
    for (const auto& f : footprints)
        for (int srgb = 0; srgb < 2; srgb++) {
            Pools pools;
            pools.fill(f[0], f[1], srgb);
            for (int i = 0; i < 6; i++, n++)
                level(pools, f[0], f[1], srgb, i % 2, 1 + random_word() % 40, 1 + random_word() % 30,
                      1 + random_word() % 3);
        }
    std::printf("%s %u levels: the GPU's first blocks give the CPU walk's kind, its early stop and its layout's end "
                "too\n",
                failures == before ? "PASS" : "FAIL", n);
}

int main() {
    blocks();
    levels();
    if (failures) {
        std::fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    std::puts("astc_kind.glsl reads every block as gpu/texture/astc_kind.h does, and its levels' kinds are the walk's");
    return 0;
}
