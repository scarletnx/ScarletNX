// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// The texture conversion shader's body (src/nx/gpu/texture/texconv.glsl) run on the CPU through tests/glsl/glsl.hpp,
// compared byte for byte with the CPU's conversions: every 2 byte texel of the three 2 byte formats, every 24 bit
// depth as a float against the CPU's float division, RGB32F, packed depth and stencil in rows not a multiple of the
// four texels an invocation owns, and random ETC2 and EAC blocks of every kind (with the T, H and planar modes made
// often) against tests/support/etc2.c and random BC1 to BC7 blocks of every kind (every BC6H and BC7 mode
// made often) against tests/support/bcn.c, in images whose sides aren't multiples of 4, and vi's pixel formats
// against present.c's former CPU conversion, and the 2D blit's half floats against tests/support/half.c.
// tests/astc_vulkan.c runs the SPIR-V build of the same file on a Vulkan device.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "glsl.hpp"
// tests/support/etc2.h's functions and kinds (its header brings the recompiler's C helpers, which C++ warns about)
extern "C" void etc2_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
extern "C" uint32_t etc2_block_bytes(int kind);
extern "C" uint32_t etc2_pixel_bytes(int kind);
// tests/support/bcn.h's, in the order of texconv's TC_BC1 to TC_BC7
extern "C" void bcn_decode_image(const uint8_t* src, uint32_t w, uint32_t h, int kind, uint8_t* dst);
extern "C" uint32_t bcn_block_bytes(int kind);
extern "C" uint32_t bcn_pixel_bytes(int kind);
// tests/support/half.h's
extern "C" float half_to_float(uint16_t h);
extern "C" uint16_t float_to_half(float f);

namespace texconv_shader {
using namespace glsl;
// what texconv.comp declares, under its names
struct Conv : Invocation {
    uint mode, units, rows, in_pitch, out_pitch, st_pitch, in_bias, out_bias, st_bias, zfloat;
    uint remap, const_a, const_b, line_out;
    const leuint* src;
    leuint* dst;
#include "nx/gpu/texture/texconv_modes.glsl"
#include "nx/gpu/texture/texconv.glsl"
};
} // namespace texconv_shader
using texconv_shader::Conv;

static unsigned failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            if (failures++ < 10) {                                                                                     \
                std::printf("%s:%d: ", __FILE__, __LINE__);                                                            \
                std::printf(__VA_ARGS__);                                                                              \
                std::printf("\n");                                                                                     \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

static uint32_t rng = 0x2545f491;
static uint32_t random_word() {
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return rng;
}

// vk/texconv.c's shape and push constants for a w x rows x slices conversion from rows of in_pitch bytes; the output's
// row is out_texels long, the stencil's rows follow the depth's
struct Shape {
    uint32_t units, out_texels, out_bpp, out_rows, stencil;
};
static Shape shape(uint32_t mode, uint32_t w) {
    if (mode <= 2 || (mode >= 15 && mode <= 17)) return {(w + 1) / 2, (w + 1) / 2 * 2, 4, 1, 0};
    if (mode >= 23 && mode <= 32) // BCn: BC4 2 bytes a texel, BC6H 8
        return {(w + 3) / 4, (w + 3) / 4 * 4, mode == 26 || mode == 27 ? 2u : mode == 30 || mode == 31 ? 8u : 4u, 4, 0};
    if (mode >= 18) return {w, w, 4, 1, 0};
    if (mode == 3) return {w, w, 16, 1, 0};
    if (mode <= 7) return {(w + 3) / 4, (w + 3) / 4 * 4, 4, 1, 1};
    return {(w + 3) / 4, (w + 3) / 4 * 4, mode == 11 || mode == 12 ? 2u : 4u, 4, 0};
}
// the buffers are the device's words, little-endian (glsl.hpp): the bytes a test hands a reference are the same
// on any host
static std::vector<glsl::leuint> convert(uint32_t mode, uint32_t zfloat, uint32_t w, uint32_t rows, uint32_t slices,
                                         const std::vector<glsl::leuint>& in, uint32_t in_pitch) {
    Shape s = shape(mode, w);
    uint64_t texels = (uint64_t)s.out_texels * s.out_rows * rows * slices;
    std::vector<glsl::leuint> out((texels * s.out_bpp + (s.stencil ? texels : 0)) / 4, 0xDEADBEEFu);
    glsl::dispatch<Conv>(glsl::uvec3((s.units + 63) / 64, rows, slices), glsl::uvec3(64, 1, 1), [&](Conv& c) {
        c.mode = mode;
        c.units = s.units;
        c.rows = rows;
        c.in_pitch = in_pitch / 4;
        c.out_pitch = s.out_texels * s.out_bpp / 4;
        c.st_pitch = s.out_texels / 4;
        c.in_bias = 0;
        c.out_bias = 0;
        c.st_bias = (uint32_t)(texels * s.out_bpp / 4);
        c.zfloat = zfloat;
        c.src = in.data();
        c.dst = out.data();
    });
    return out;
}

// the n byte little-endian number at p: a texel as the device's memory holds it
static uint32_t le_bytes(const uint8_t* p, uint32_t n) {
    uint32_t v = 0;
    for (uint32_t i = 0; i < n; i++) v |= (uint32_t)p[i] << (8 * i);
    return v;
}

// texture.c's conversion of a 2 byte texel before it moved to the GPU
static uint32_t ref16(uint32_t mode, uint32_t v) {
    uint32_t r, g, b, a;
    if (mode == 0) {
        r = (v & 15) * 17;
        g = ((v >> 4) & 15) * 17;
        b = ((v >> 8) & 15) * 17;
        a = ((v >> 12) & 15) * 17;
    } else if (mode == 1) {
        r = (v & 31) * 255 / 31;
        g = ((v >> 5) & 31) * 255 / 31;
        b = ((v >> 10) & 31) * 255 / 31;
        a = (v >> 15) ? 255 : 0;
    } else {
        r = (v & 1) ? 255 : 0;
        g = ((v >> 1) & 31) * 255 / 31;
        b = ((v >> 6) & 31) * 255 / 31;
        a = ((v >> 11) & 31) * 255 / 31;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static void two_byte_texels() {
    for (uint32_t mode = 0; mode < 3; mode++) {
        Conv c;
        c.mode = mode;
        for (uint32_t v = 0; v < 65536; v++)
            CHECK(c.tc_16to32(v) == ref16(mode, v), "mode %u texel %04x: %08x, not %08x", mode, v, c.tc_16to32(v),
                  ref16(mode, v));
        // an odd row in rows padded to 16 bytes: every texel where it goes, two slices
        uint32_t w = 13, rows = 3, pitch = 32;
        std::vector<glsl::leuint> in(pitch / 4 * rows * 2);
        for (auto& x : in) x = random_word();
        std::vector<glsl::leuint> out = convert(mode, 0, w, rows, 2, in, pitch);
        for (uint32_t y = 0; y < rows * 2; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t v = le_bytes((const uint8_t*)in.data() + y * pitch + x * 2, 2);
                CHECK(out[y * 14 + x] == ref16(mode, v), "mode %u (%u, %u)", mode, x, y);
            }
    }
}

// present.c's conversion of vi's pixel formats before it moved to the GPU
static uint32_t expand(uint32_t v, unsigned bits) { return bits == 4 ? v * 17 : bits == 5 ? v << 3 | v >> 2 : v << 2 | v >> 4; }
static uint32_t to_rgba8(uint32_t fmt, uint32_t p) {
    switch (fmt) {
        case 2: return p | 0xFF000000u;
        case 4: return expand(p >> 11 & 31, 5) | expand(p >> 5 & 63, 6) << 8 | expand(p & 31, 5) << 16 | 0xFF000000u;
        case 5: return (p & 0xFF00FF00u) | (p >> 16 & 0xFFu) | (p & 0xFFu) << 16;
        case 6:
            return expand(p >> 11 & 31, 5) | expand(p >> 6 & 31, 5) << 8 | expand(p >> 1 & 31, 5) << 16 |
                   (p & 1 ? 0xFF000000u : 0);
        case 7:
            return expand(p >> 12 & 15, 4) | expand(p >> 8 & 15, 4) << 8 | expand(p >> 4 & 15, 4) << 16 |
                   expand(p & 15, 4) << 24;
    }
    return p;
}
// every 2 byte pixel of the three 2 byte formats, random 4 byte ones, and rows of each through a dispatch
static void vi_frames() {
    const uint32_t fmts[] = {4, 6, 7, 2, 5}, modes[] = {15, 16, 17, 18, 19};
    for (int i = 0; i < 5; i++) {
        uint32_t fmt = fmts[i], mode = modes[i], bpp = i < 3 ? 2 : 4;
        Conv c;
        c.mode = mode;
        if (bpp == 2)
            for (uint32_t v = 0; v < 65536; v++)
                CHECK(c.tc_16to32(v) == to_rgba8(fmt, v), "vi format %u pixel %04x", fmt, v);
        uint32_t w = 15, rows = 4, pitch = 64;
        std::vector<glsl::leuint> in(pitch / 4 * rows);
        for (auto& x : in) x = random_word();
        std::vector<glsl::leuint> out = convert(mode, 0, w, rows, 1, in, pitch);
        uint32_t ow = shape(mode, w).out_texels;
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < w; x++) {
                uint32_t p = le_bytes((const uint8_t*)in.data() + y * pitch + x * bpp, bpp);
                CHECK(out[y * ow + x] == to_rgba8(fmt, p), "vi format %u (%u, %u)", fmt, x, y);
            }
    }
}

// NVDEC's chroma: U and V rows interleaved, U first, in rows of whole words
static void nv12_uv() {
    const uint32_t pairs = 13, rows = 5, in_pitch = 16, out_pitch = 32;
    std::vector<glsl::leuint> in(in_pitch / 4 * rows * 2), out(out_pitch / 4 * rows, 0xDEADBEEFu);
    for (auto& x : in) x = random_word();
    const uint8_t* ub = (const uint8_t*)in.data();
    const uint8_t* vb = ub + in_pitch * rows;
    glsl::dispatch<Conv>(glsl::uvec3(1, rows, 1), glsl::uvec3(64, 1, 1), [&](Conv& c) {
        c.mode = 20;
        c.units = (pairs + 3) / 4;
        c.rows = rows;
        c.in_pitch = in_pitch / 4;
        c.st_pitch = in_pitch / 4;
        c.out_pitch = out_pitch / 4;
        c.in_bias = 0;
        c.st_bias = in_pitch * rows / 4;
        c.out_bias = 0;
        c.zfloat = 0;
        c.src = in.data();
        c.dst = out.data();
    });
    const uint8_t* ob = (const uint8_t*)out.data();
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < pairs; x++) {
            CHECK(ob[y * out_pitch + 2 * x] == ub[y * in_pitch + x], "nv12 u (%u, %u)", x, y);
            CHECK(ob[y * out_pitch + 2 * x + 1] == vb[y * in_pitch + x], "nv12 v (%u, %u)", x, y);
        }
}

// the DMA engine's remap against dma.c's on the CPU: random component sizes and counts, every source (components,
// constants, keeping the destination's bytes), lines starting anywhere in a word, lines that end inside one
static void dma_remap_case(uint32_t cs, uint32_t ns, uint32_t nd, const uint32_t sel[4], uint32_t width, uint32_t lines,
                           uint32_t in_at) {
    uint32_t ca = random_word(), cb = random_word(), line_in = width * cs * ns, line_out = width * cs * nd;
    uint32_t in_pitch = line_in + 5, out_pitch = (line_out + 3) / 4 * 4 + 8;
    std::vector<uint8_t> inb(in_at + in_pitch * lines + 8), old(out_pitch * lines), want;
    for (auto& b : inb) b = (uint8_t)random_word();
    for (auto& b : old) b = (uint8_t)random_word();
    want = old;
    for (uint32_t y = 0; y < lines; y++)
        for (uint32_t e = 0; e < width; e++)
            for (uint32_t k = 0; k < nd; k++) {
                uint8_t* d = &want[y * out_pitch + (e * nd + k) * cs];
                const uint8_t* src = &inb[in_at + y * in_pitch + e * cs * ns];
                if (sel[k] == 4 || sel[k] == 5) // the constant's low bytes, as the device's memory holds them
                    for (uint32_t i = 0; i < cs; i++) d[i] = (uint8_t)((sel[k] == 4 ? ca : cb) >> (8 * i));
                else if (sel[k] < 4) {
                    if (sel[k] < ns)
                        std::memcpy(d, src + sel[k] * cs, cs);
                    else
                        std::memset(d, 0, cs);
                }
            }
    std::vector<glsl::leuint> in((inb.size() + 3) / 4), out(old.size() / 4);
    std::memcpy(in.data(), inb.data(), inb.size());
    std::memcpy(out.data(), old.data(), old.size());
    uint32_t remap = cs | ns << 4 | nd << 8;
    for (int k = 0; k < 4; k++) remap |= (sel[k] & 7) << (12 + 4 * k);
    glsl::dispatch<Conv>(glsl::uvec3(((line_out + 3) / 4 + 63) / 64, lines, 1), glsl::uvec3(64, 1, 1), [&](Conv& c) {
        c.mode = 21;
        c.units = (line_out + 3) / 4;
        c.rows = lines;
        c.in_pitch = in_pitch;
        c.out_pitch = out_pitch / 4;
        c.in_bias = in_at;
        c.out_bias = 0;
        c.remap = remap;
        c.const_a = ca;
        c.const_b = cb;
        c.line_out = line_out;
        c.src = in.data();
        c.dst = out.data();
    });
    CHECK(!std::memcmp(out.data(), want.data(), want.size()), "dma remap cs %u ns %u nd %u", cs, ns, nd);
}
static void dma_remaps() {
    for (int i = 0; i < 400; i++) {
        uint32_t cs = 1 + random_word() % 4, ns = 1 + random_word() % 4, nd = 1 + random_word() % 4, sel[4];
        for (int k = 0; k < 4; k++) sel[k] = random_word() % 8;
        dma_remap_case(cs, ns, nd, sel, 1 + random_word() % 9, 1 + random_word() % 4, random_word() % 7);
    }
}

// the 3D engine's index buffers Vulkan can't read (vk/render.c's index_convert) against render.c's CPU conversions
// before they moved to the GPU: quads from 8, 16 and 32-bit indices and from vertex numbers, 8-bit indices widened
// with and without their restart index, 16 and 32-bit restart indices made all ones; odd counts, inputs and outputs
// that start past the bound offset, and more elements than a row of units (gl_GlobalInvocationID.y counts rows; 0: n)
static void index_case(uint32_t mode, uint32_t remap, uint32_t count, uint32_t units) {
    uint32_t isz = mode == 34 ? 1 : mode == 35 ? 2 : mode == 36 ? 4 : remap ? remap : 4, in_bias = 4 * (random_word() % 4);
    uint32_t n = mode == 37 ? count / 4 : mode == 36 ? count : (count + 1) / 2, out_bias = random_word() % 5;
    std::vector<uint8_t> inb(in_bias + (count * isz + 3) / 4 * 4);
    for (auto& b : inb) b = (uint8_t)(random_word() % 6 == 0 ? 0xFF : random_word() % 12); // restart indices often
    uint32_t ri = mode == 37 ? 0 : inb[in_bias] | (isz > 1 ? 0xFFu << 8 : 0) | (isz > 2 ? 0xFFFF0000u : 0);
    if (mode == 34 && random_word() % 3 == 0) ri = 0xFF;
    if (isz > 1 && mode != 37) // the restart index somewhere in the buffer
        std::memcpy(&inb[in_bias + isz * (random_word() % count)], &ri, isz);
    std::vector<uint32_t> want(out_bias + (mode == 37 ? 6 * n : n), 0xDEADBEEFu);
    const uint8_t* ib = &inb[in_bias];
    for (uint32_t j = 0; j < count; j++) {
        uint32_t v = mode == 37 && !remap ? j : le_bytes(ib + j * isz, isz);
        if (mode == 37) {
            if (j / 4 >= n) continue;
            static const uint32_t at[4][2] = {{0, 3}, {1, 1}, {2, 4}, {5, 5}}; // where each corner goes: 0 1 2 0 2 3
            want[out_bias + 6 * (j / 4) + at[j & 3][0]] = v;
            want[out_bias + 6 * (j / 4) + at[j & 3][1]] = v;
        } else if (mode == 36)
            want[out_bias + j] = v == ri ? 0xFFFFFFFFu : v;
        else {
            uint32_t w = (mode == 34 ? remap && v == (ri & 0xFF) : v == (ri & 0xFFFF)) ? 0xFFFFu : v;
            uint32_t& d = want[out_bias + j / 2];
            d = j & 1 ? (d & 0xFFFFu) | w << 16 : w;
        }
    }
    std::vector<glsl::leuint> in(inb.size() / 4), out(want.size(), 0xDEADBEEFu);
    std::memcpy(in.data(), inb.data(), inb.size());
    if (!units) units = n; // one row
    uint32_t rows = (n + units - 1) / units;
    glsl::dispatch<Conv>(glsl::uvec3((units + 63) / 64, rows, 1), glsl::uvec3(64, 1, 1), [&](Conv& c) {
        c.mode = mode;
        c.units = units;
        c.rows = rows;
        c.in_bias = in_bias;
        c.out_bias = out_bias;
        c.remap = remap;
        c.const_a = mode == 34 ? ri & 0xFF : mode == 35 ? ri & 0xFFFF : ri;
        c.line_out = n;
        c.src = in.data();
        c.dst = out.data();
    });
    for (uint32_t i = 0; i < want.size(); i++) {
        // an odd count's last pair has a high half that no index is drawn from: only its low half counts
        uint32_t mask = mode != 36 && mode != 37 && count % 2 && i == want.size() - 1 ? 0xFFFFu : ~0u;
        CHECK(((uint32_t)out[i] & mask) == (want[i] & mask), "index mode %u remap %u count %u word %u: %08x, not %08x",
              mode, remap, count, i, (uint32_t)out[i], want[i]);
    }
}
static void index_buffers() {
    for (int i = 0; i < 300; i++) {
        uint32_t count = 1 + random_word() % 300, units = random_word() % 2 ? 0 : 1 + random_word() % 70;
        index_case(34, random_word() % 2, count, units);
        index_case(35, 0, count, units);
        index_case(36, 0, count, units);
        if (count >= 4) {
            static const uint32_t sizes[4] = {0, 1, 2, 4};
            index_case(37, sizes[random_word() % 4], count, units);
        }
    }
}

static void depth_float() {
    Conv c;
    for (uint32_t z = 0; z < 1u << 24; z++) {
        float f = (float)z / 16777215.0f;
        uint32_t want;
        std::memcpy(&want, &f, 4);
        CHECK(c.tc_d24_float(z) == want, "depth %06x: %08x, not %08x", z, c.tc_d24_float(z), want);
    }
}

// the 2D blit's half floats (TC_BLIT_FLOAT) against half.c: every half to a float, and to a half every float near
// one, the ties between neighbors and a step off them, and random words
static uint32_t float_bits(float f) {
    uint32_t x;
    std::memcpy(&x, &f, 4);
    return x;
}
static void half_check(Conv& c, uint32_t x) {
    float f;
    std::memcpy(&f, &x, 4);
    CHECK(c.tc_float_half(x) == float_to_half(f), "float %08x: half %04x, not %04x", x, c.tc_float_half(x),
          float_to_half(f));
}
static void halves() {
    Conv c;
    for (uint32_t h = 0; h < 0x10000; h++) {
        uint32_t x = float_bits(half_to_float((uint16_t)h));
        CHECK(c.tc_half_float(h) == x, "half %04x: %08x, not %08x", h, c.tc_half_float(h), x);
        for (uint32_t d = 0; d < 5; d++) half_check(c, x + d - 2);
        if ((h & 0x7FFF) < 0x7C00 && (h & 0x7FFF) != 0x7BFF) { // finite, with a finite neighbor away from zero
            uint32_t tie = float_bits((half_to_float((uint16_t)h) + half_to_float((uint16_t)(h + 1))) / 2);
            for (uint32_t d = 0; d < 3; d++) half_check(c, tie + d - 1);
        }
    }
    half_check(c, 0x477FF000u); // 65520, the tie past the largest half: infinity
    for (int i = 0; i < 1 << 20; i++) half_check(c, random_word());
}

static void rgb32f() {
    uint32_t w = 7, rows = 5, pitch = 96; // 84 bytes a row, padded to 48
    std::vector<glsl::leuint> in(pitch / 4 * rows);
    for (auto& x : in) x = random_word();
    std::vector<glsl::leuint> out = convert(3, 0, w, rows, 1, in, pitch);
    for (uint32_t y = 0; y < rows; y++)
        for (uint32_t x = 0; x < w; x++)
            for (uint32_t k = 0; k < 4; k++)
                CHECK(out[(y * w + x) * 4 + k] == (k < 3 ? uint32_t(in[y * pitch / 4 + x * 3 + k]) : 0x3F800000u), "rgb32f");
}

// format.c's vk_depth_unpack before it moved to the GPU: the texel's depth and stencil
static void ref_depth(uint32_t mode, int zfloat, const uint8_t* t, uint32_t* depth, uint8_t* stencil) {
    uint32_t v = le_bytes(t, 4);
    if (mode == 7) {
        *depth = v;
        *stencil = t[4];
        return;
    }
    uint32_t z = mode == 5 ? v >> 8 : v & 0xFFFFFF;
    *stencil = mode == 6 ? 0 : mode == 5 ? v & 0xFF : v >> 24;
    if (zfloat) {
        float f = (float)z / 16777215.0f;
        std::memcpy(depth, &f, 4);
    } else
        *depth = z;
}
static void depth_stencil() {
    for (uint32_t mode = 4; mode <= 7; mode++)
        for (int zfloat = 0; zfloat < 2; zfloat++) {
            uint32_t bpp = mode == 7 ? 8 : 4, w = 9, rows = 4, slices = 2;
            uint32_t pitch = (w * bpp + 31) / 32 * 32;
            std::vector<glsl::leuint> in(pitch / 4 * rows * slices);
            for (auto& x : in) x = random_word();
            std::vector<glsl::leuint> out = convert(mode, mode == 7 ? 0 : (uint32_t)zfloat, w, rows, slices, in, pitch);
            uint32_t ow = 12, st = ow * rows * slices;
            const uint8_t* stencil = (const uint8_t*)(out.data() + st);
            for (uint32_t y = 0; y < rows * slices; y++)
                for (uint32_t x = 0; x < w; x++) {
                    uint32_t d;
                    uint8_t s;
                    ref_depth(mode, mode != 7 && zfloat, (const uint8_t*)in.data() + y * pitch + x * bpp, &d, &s);
                    CHECK(out[y * ow + x] == d, "depth mode %u zfloat %d (%u, %u)", mode, zfloat, x, y);
                    CHECK(stencil[y * ow + x] == s, "stencil mode %u (%u, %u)", mode, x, y);
                }
        }
}

// random ETC2 blocks, differential, half of them made to overflow a differential color into the T, H or planar
// mode: a base of 31 plus 3 (0xFB in the color's byte) overflows, a base of 2 plus 0 (0x10) doesn't
static void random_block(uint8_t* b) {
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)random_word();
    b[3] |= 2;
    uint32_t m = random_word() % 6;
    if (m == 0) b[0] = 0xFB; // red overflows: T
    if (m == 1) b[0] = 0x10, b[1] = 0xFB; // green: H
    if (m == 2) b[0] = 0x10, b[1] = 0x10, b[2] = 0xFB; // blue: planar
}
static void etc2_kinds() {
    const uint32_t sizes[][2] = {{4, 4}, {13, 9}, {1, 1}, {18, 7}};
    for (int kind = 0; kind < 7; kind++)
        for (const auto& sz : sizes) {
            uint32_t w = sz[0], h = sz[1], bw = (w + 3) / 4, bh = (h + 3) / 4, bb = etc2_block_bytes(kind);
            uint32_t pb = etc2_pixel_bytes(kind), pitch = (bw * bb + 15) / 16 * 16;
            std::vector<uint8_t> tight((size_t)bw * bh * bb);
            for (uint32_t i = 0; i < bw * bh * bb / 8; i++) {
                random_block(&tight[i * 8]);
                if (random_word() % 4 == 0) tight[i * 8 + 3] &= ~2; // individual, or punch-through's see-through
            }
            std::vector<glsl::leuint> in(pitch / 4 * bh);
            for (uint32_t y = 0; y < bh; y++) std::memcpy((uint8_t*)in.data() + y * pitch, &tight[y * bw * bb], bw * bb);
            std::vector<uint8_t> want((size_t)w * h * pb);
            etc2_decode_image(tight.data(), w, h, kind, want.data());
            std::vector<glsl::leuint> out = convert(8 + kind, 0, w, bh, 1, in, pitch);
            uint32_t ow = bw * 4;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    CHECK(!std::memcmp((const uint8_t*)out.data() + ((size_t)y * ow + x) * pb, &want[((size_t)y * w + x) * pb],
                                       pb),
                          "kind %d %ux%u (%u, %u)", kind, w, h, x, y);
        }
}

// random BCn blocks: BC6H's codes and BC7's modes each made as often as the others, a reserved one now and then
static void random_bc_block(int kind, uint8_t* b) {
    for (int i = 0; i < 16; i++) b[i] = (uint8_t)random_word();
    if (random_word() % 16 == 0) return;
    if (kind == 7 || kind == 8) {
        static const uint8_t codes[14] = {0, 1, 2, 6, 10, 14, 18, 22, 26, 30, 3, 7, 11, 15};
        uint8_t c = codes[random_word() % 14];
        b[0] = (uint8_t)((b[0] & (c < 2 ? ~3 : ~31)) | c);
    }
    if (kind == 9) {
        uint32_t m = random_word() % 8;
        b[0] = (uint8_t)((b[0] & ~((2u << m) - 1)) | 1u << m);
    }
}
static void bcn_kinds() {
    const uint32_t sizes[][2] = {{4, 4}, {13, 9}, {1, 1}, {18, 7}};
    for (int kind = 0; kind < 10; kind++)
        for (const auto& sz : sizes) {
            uint32_t w = sz[0], h = sz[1], bw = (w + 3) / 4, bh = (h + 3) / 4, bb = bcn_block_bytes(kind);
            uint32_t pb = bcn_pixel_bytes(kind), pitch = (bw * bb + 15) / 16 * 16;
            std::vector<uint8_t> tight((size_t)bw * bh * bb + 16);
            for (uint32_t i = 0; i < bw * bh; i++) random_bc_block(kind, &tight[i * bb]);
            std::vector<glsl::leuint> in(pitch / 4 * bh);
            for (uint32_t y = 0; y < bh; y++) std::memcpy((uint8_t*)in.data() + y * pitch, &tight[y * bw * bb], bw * bb);
            std::vector<uint8_t> want((size_t)w * h * pb);
            bcn_decode_image(tight.data(), w, h, kind, want.data());
            std::vector<glsl::leuint> out = convert(23 + kind, 0, w, bh, 1, in, pitch);
            uint32_t ow = bw * 4;
            for (uint32_t y = 0; y < h; y++)
                for (uint32_t x = 0; x < w; x++)
                    CHECK(!std::memcmp((const uint8_t*)out.data() + ((size_t)y * ow + x) * pb, &want[((size_t)y * w + x) * pb],
                                       pb),
                          "BCn kind %d %ux%u (%u, %u)", kind, w, h, x, y);
        }
}

int main() {
    two_byte_texels();
    vi_frames();
    nv12_uv();
    dma_remaps();
    index_buffers();
    depth_float();
    halves();
    rgb32f();
    depth_stencil();
    for (int i = 0; i < 200; i++) etc2_kinds();
    for (int i = 0; i < 200; i++) bcn_kinds();
    if (failures) {
        std::printf("texconv_glsl: %u failures\n", failures);
        return 1;
    }
    std::puts("texconv_glsl: the shader's conversions are the CPU's, byte for byte");
    return 0;
}
