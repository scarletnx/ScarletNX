/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx-shaderperf's texel formats (texfmt.h): the format table, the conversions of the plain, packed and depth formats
   and the PPM/PGM/PFM images. Self-contained: the C99 library and vulkan_core.h only, no file of the repository */
#include "texfmt.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the table ----------------------------------------------------------------------------------------------- */

/* X(short name, Vulkan name without VK_FORMAT_, kind, numeric, block width, block height, bytes, channels) */
#define SP_F8(X, s, V, ch, kind, sfx)                                                                                  \
    X(s "_unorm", V##_UNORM##sfx, kind, SP_UNORM, 1, 1, ch, ch)                                                        \
    X(s "_snorm", V##_SNORM##sfx, kind, SP_SNORM, 1, 1, ch, ch)                                                        \
    X(s "_uscaled", V##_USCALED##sfx, kind, SP_USCALED, 1, 1, ch, ch)                                                  \
    X(s "_sscaled", V##_SSCALED##sfx, kind, SP_SSCALED, 1, 1, ch, ch)                                                  \
    X(s "_uint", V##_UINT##sfx, kind, SP_UINT, 1, 1, ch, ch)                                                           \
    X(s "_sint", V##_SINT##sfx, kind, SP_SINT, 1, 1, ch, ch)                                                           \
    X(s "_srgb", V##_SRGB##sfx, kind, SP_SRGB, 1, 1, ch, ch)
#define SP_F10(X, s, V)                                                                                                \
    X(s "_unorm", V##_UNORM_PACK32, SP_KIND_PACKED, SP_UNORM, 1, 1, 4, 4)                                              \
    X(s "_snorm", V##_SNORM_PACK32, SP_KIND_PACKED, SP_SNORM, 1, 1, 4, 4)                                              \
    X(s "_uscaled", V##_USCALED_PACK32, SP_KIND_PACKED, SP_USCALED, 1, 1, 4, 4)                                        \
    X(s "_sscaled", V##_SSCALED_PACK32, SP_KIND_PACKED, SP_SSCALED, 1, 1, 4, 4)                                        \
    X(s "_uint", V##_UINT_PACK32, SP_KIND_PACKED, SP_UINT, 1, 1, 4, 4)                                                 \
    X(s "_sint", V##_SINT_PACK32, SP_KIND_PACKED, SP_SINT, 1, 1, 4, 4)
#define SP_F16(X, s, V, ch)                                                                                            \
    X(s "_unorm", V##_UNORM, SP_KIND_PLAIN, SP_UNORM, 1, 1, 2 * ch, ch)                                                \
    X(s "_snorm", V##_SNORM, SP_KIND_PLAIN, SP_SNORM, 1, 1, 2 * ch, ch)                                                \
    X(s "_uscaled", V##_USCALED, SP_KIND_PLAIN, SP_USCALED, 1, 1, 2 * ch, ch)                                          \
    X(s "_sscaled", V##_SSCALED, SP_KIND_PLAIN, SP_SSCALED, 1, 1, 2 * ch, ch)                                          \
    X(s "_uint", V##_UINT, SP_KIND_PLAIN, SP_UINT, 1, 1, 2 * ch, ch)                                                   \
    X(s "_sint", V##_SINT, SP_KIND_PLAIN, SP_SINT, 1, 1, 2 * ch, ch)                                                   \
    X(s "_sfloat", V##_SFLOAT, SP_KIND_PLAIN, SP_SFLOAT, 1, 1, 2 * ch, ch)
#define SP_F32(X, s, V, ch)                                                                                            \
    X(s "_uint", V##_UINT, SP_KIND_PLAIN, SP_UINT, 1, 1, 4 * ch, ch)                                                   \
    X(s "_sint", V##_SINT, SP_KIND_PLAIN, SP_SINT, 1, 1, 4 * ch, ch)                                                   \
    X(s "_sfloat", V##_SFLOAT, SP_KIND_PLAIN, SP_SFLOAT, 1, 1, 4 * ch, ch)
#define SP_FASTC(X, w, h)                                                                                              \
    X("astc_" #w "x" #h "_unorm", ASTC_##w##x##h##_UNORM_BLOCK, SP_KIND_ASTC, SP_UNORM, w, h, 16, 4)                   \
    X("astc_" #w "x" #h "_srgb", ASTC_##w##x##h##_SRGB_BLOCK, SP_KIND_ASTC, SP_SRGB, w, h, 16, 4)

#define SP_FORMAT_LIST(X)                                                                                              \
    X("rg4_unorm", R4G4_UNORM_PACK8, SP_KIND_PACKED, SP_UNORM, 1, 1, 1, 2)                                             \
    X("rgba4_unorm", R4G4B4A4_UNORM_PACK16, SP_KIND_PACKED, SP_UNORM, 1, 1, 2, 4)                                      \
    X("bgra4_unorm", B4G4R4A4_UNORM_PACK16, SP_KIND_PACKED, SP_UNORM, 1, 1, 2, 4)                                      \
    X("r5g6b5_unorm", R5G6B5_UNORM_PACK16, SP_KIND_PACKED, SP_UNORM, 1, 1, 2, 3)                                       \
    X("b5g6r5_unorm", B5G6R5_UNORM_PACK16, SP_KIND_PACKED, SP_UNORM, 1, 1, 2, 3)                                       \
    X("rgb5a1_unorm", R5G5B5A1_UNORM_PACK16, SP_KIND_PACKED, SP_UNORM, 1, 1, 2, 4)                                     \
    X("bgr5a1_unorm", B5G5R5A1_UNORM_PACK16, SP_KIND_PACKED, SP_UNORM, 1, 1, 2, 4)                                     \
    X("a1rgb5_unorm", A1R5G5B5_UNORM_PACK16, SP_KIND_PACKED, SP_UNORM, 1, 1, 2, 4)                                     \
    SP_F8(X, "r8", R8, 1, SP_KIND_PLAIN, )                                                                             \
    SP_F8(X, "rg8", R8G8, 2, SP_KIND_PLAIN, )                                                                          \
    SP_F8(X, "rgb8", R8G8B8, 3, SP_KIND_PLAIN, )                                                                       \
    SP_F8(X, "bgr8", B8G8R8, 3, SP_KIND_PLAIN, )                                                                       \
    SP_F8(X, "rgba8", R8G8B8A8, 4, SP_KIND_PLAIN, )                                                                    \
    SP_F8(X, "bgra8", B8G8R8A8, 4, SP_KIND_PLAIN, )                                                                    \
    SP_F8(X, "abgr8", A8B8G8R8, 4, SP_KIND_PACKED, _PACK32)                                                            \
    SP_F10(X, "a2rgb10", A2R10G10B10)                                                                                  \
    SP_F10(X, "a2bgr10", A2B10G10R10)                                                                                  \
    SP_F16(X, "r16", R16, 1)                                                                                           \
    SP_F16(X, "rg16", R16G16, 2)                                                                                       \
    SP_F16(X, "rgb16", R16G16B16, 3)                                                                                   \
    SP_F16(X, "rgba16", R16G16B16A16, 4)                                                                               \
    SP_F32(X, "r32", R32, 1)                                                                                           \
    SP_F32(X, "rg32", R32G32, 2)                                                                                       \
    SP_F32(X, "rgb32", R32G32B32, 3)                                                                                   \
    SP_F32(X, "rgba32", R32G32B32A32, 4)                                                                               \
    X("b10g11r11_ufloat", B10G11R11_UFLOAT_PACK32, SP_KIND_PACKED, SP_UFLOAT, 1, 1, 4, 3)                              \
    X("e5b9g9r9_ufloat", E5B9G9R9_UFLOAT_PACK32, SP_KIND_PACKED, SP_UFLOAT, 1, 1, 4, 3)                                \
    X("d16_unorm", D16_UNORM, SP_KIND_DEPTH, SP_UNORM, 1, 1, 2, 1)                                                     \
    X("x8_d24_unorm", X8_D24_UNORM_PACK32, SP_KIND_DEPTH, SP_UNORM, 1, 1, 4, 1)                                        \
    X("d32_sfloat", D32_SFLOAT, SP_KIND_DEPTH, SP_SFLOAT, 1, 1, 4, 1)                                                  \
    X("s8_uint", S8_UINT, SP_KIND_DEPTH, SP_UINT, 1, 1, 1, 1)                                                          \
    X("d24_unorm_s8_uint", D24_UNORM_S8_UINT, SP_KIND_DEPTH, SP_UNORM, 1, 1, 4, 2)                                     \
    X("d32_sfloat_s8_uint", D32_SFLOAT_S8_UINT, SP_KIND_DEPTH, SP_SFLOAT, 1, 1, 4, 2)                                  \
    X("bc1_rgb_unorm", BC1_RGB_UNORM_BLOCK, SP_KIND_BC, SP_UNORM, 4, 4, 8, 3)                                          \
    X("bc1_rgb_srgb", BC1_RGB_SRGB_BLOCK, SP_KIND_BC, SP_SRGB, 4, 4, 8, 3)                                             \
    X("bc1_rgba_unorm", BC1_RGBA_UNORM_BLOCK, SP_KIND_BC, SP_UNORM, 4, 4, 8, 4)                                        \
    X("bc1_rgba_srgb", BC1_RGBA_SRGB_BLOCK, SP_KIND_BC, SP_SRGB, 4, 4, 8, 4)                                           \
    X("bc2_unorm", BC2_UNORM_BLOCK, SP_KIND_BC, SP_UNORM, 4, 4, 16, 4)                                                 \
    X("bc2_srgb", BC2_SRGB_BLOCK, SP_KIND_BC, SP_SRGB, 4, 4, 16, 4)                                                    \
    X("bc3_unorm", BC3_UNORM_BLOCK, SP_KIND_BC, SP_UNORM, 4, 4, 16, 4)                                                 \
    X("bc3_srgb", BC3_SRGB_BLOCK, SP_KIND_BC, SP_SRGB, 4, 4, 16, 4)                                                    \
    X("bc4_unorm", BC4_UNORM_BLOCK, SP_KIND_BC, SP_UNORM, 4, 4, 8, 1)                                                  \
    X("bc4_snorm", BC4_SNORM_BLOCK, SP_KIND_BC, SP_SNORM, 4, 4, 8, 1)                                                  \
    X("bc5_unorm", BC5_UNORM_BLOCK, SP_KIND_BC, SP_UNORM, 4, 4, 16, 2)                                                 \
    X("bc5_snorm", BC5_SNORM_BLOCK, SP_KIND_BC, SP_SNORM, 4, 4, 16, 2)                                                 \
    X("bc6h_ufloat", BC6H_UFLOAT_BLOCK, SP_KIND_BC, SP_UFLOAT, 4, 4, 16, 3)                                            \
    X("bc6h_sfloat", BC6H_SFLOAT_BLOCK, SP_KIND_BC, SP_SFLOAT, 4, 4, 16, 3)                                            \
    X("bc7_unorm", BC7_UNORM_BLOCK, SP_KIND_BC, SP_UNORM, 4, 4, 16, 4)                                                 \
    X("bc7_srgb", BC7_SRGB_BLOCK, SP_KIND_BC, SP_SRGB, 4, 4, 16, 4)                                                    \
    X("etc2_rgb8_unorm", ETC2_R8G8B8_UNORM_BLOCK, SP_KIND_ETC2, SP_UNORM, 4, 4, 8, 3)                                  \
    X("etc2_rgb8_srgb", ETC2_R8G8B8_SRGB_BLOCK, SP_KIND_ETC2, SP_SRGB, 4, 4, 8, 3)                                     \
    X("etc2_rgb8a1_unorm", ETC2_R8G8B8A1_UNORM_BLOCK, SP_KIND_ETC2, SP_UNORM, 4, 4, 8, 4)                              \
    X("etc2_rgb8a1_srgb", ETC2_R8G8B8A1_SRGB_BLOCK, SP_KIND_ETC2, SP_SRGB, 4, 4, 8, 4)                                 \
    X("etc2_rgba8_unorm", ETC2_R8G8B8A8_UNORM_BLOCK, SP_KIND_ETC2, SP_UNORM, 4, 4, 16, 4)                              \
    X("etc2_rgba8_srgb", ETC2_R8G8B8A8_SRGB_BLOCK, SP_KIND_ETC2, SP_SRGB, 4, 4, 16, 4)                                 \
    X("eac_r11_unorm", EAC_R11_UNORM_BLOCK, SP_KIND_ETC2, SP_UNORM, 4, 4, 8, 1)                                        \
    X("eac_r11_snorm", EAC_R11_SNORM_BLOCK, SP_KIND_ETC2, SP_SNORM, 4, 4, 8, 1)                                        \
    X("eac_rg11_unorm", EAC_R11G11_UNORM_BLOCK, SP_KIND_ETC2, SP_UNORM, 4, 4, 16, 2)                                   \
    X("eac_rg11_snorm", EAC_R11G11_SNORM_BLOCK, SP_KIND_ETC2, SP_SNORM, 4, 4, 16, 2)                                   \
    SP_FASTC(X, 4, 4)                                                                                                  \
    SP_FASTC(X, 5, 4)                                                                                                  \
    SP_FASTC(X, 5, 5)                                                                                                  \
    SP_FASTC(X, 6, 5)                                                                                                  \
    SP_FASTC(X, 6, 6)                                                                                                  \
    SP_FASTC(X, 8, 5)                                                                                                  \
    SP_FASTC(X, 8, 6)                                                                                                  \
    SP_FASTC(X, 8, 8)                                                                                                  \
    SP_FASTC(X, 10, 5)                                                                                                 \
    SP_FASTC(X, 10, 6)                                                                                                 \
    SP_FASTC(X, 10, 8)                                                                                                 \
    SP_FASTC(X, 10, 10)                                                                                                \
    SP_FASTC(X, 12, 10)                                                                                                \
    SP_FASTC(X, 12, 12)

#define SP_ROW(name, vk, kind, num, bw, bh, bytes, ch) {name, VK_FORMAT_##vk, kind, num, bw, bh, bytes, ch},
#define SP_VKNAME(name, vk, kind, num, bw, bh, bytes, ch) #vk,
static const SpFormat k_formats[] = {SP_FORMAT_LIST(SP_ROW)};
static const char* const k_vk_names[] = {SP_FORMAT_LIST(SP_VKNAME)};
#define SP_FORMAT_COUNT (sizeof k_formats / sizeof k_formats[0])

static const struct {
    const char *alias, *name;
} k_aliases[] = {
    {"rgba8", "rgba8_unorm"},   {"bgra8", "bgra8_unorm"},   {"rgba16f", "rgba16_sfloat"},
    {"rgba32f", "rgba32_sfloat"}, {"r8", "r8_unorm"},       {"rg8", "rg8_unorm"},
    {"r32f", "r32_sfloat"},     {"rg32f", "rg32_sfloat"},   {"r16f", "r16_sfloat"},
    {"rg16f", "rg16_sfloat"},   {"r11g11b10f", "b10g11r11_ufloat"}, {"rgb10a2", "a2bgr10_unorm"},
    {"srgba8", "rgba8_srgb"},
};

const SpFormat* sp_formats(size_t* count) {
    *count = SP_FORMAT_COUNT;
    return k_formats;
}

static char ascii_upper(char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; }

static bool ascii_ieq(const char* a, const char* b) {
    for (; *a && *b; a++, b++)
        if (ascii_upper(*a) != ascii_upper(*b)) return false;
    return *a == *b;
}

static bool ascii_iprefix(const char* s, const char* prefix) {
    for (; *prefix; s++, prefix++)
        if (ascii_upper(*s) != ascii_upper(*prefix)) return false;
    return true;
}

/* a Vulkan name upper-cased, without "VK_FORMAT_" and without a "_BLOCK" or "_PACKn" suffix. False when too long */
static bool vk_name_key(const char* s, char* out, size_t size) {
    static const char* const suffixes[] = {"_BLOCK", "_PACK8", "_PACK16", "_PACK32"};
    if (ascii_iprefix(s, "VK_FORMAT_")) s += 10;
    size_t n = strlen(s);
    if (n >= size) return false;
    for (size_t i = 0; i < n; i++) out[i] = ascii_upper(s[i]);
    out[n] = 0;
    for (size_t i = 0; i < sizeof suffixes / sizeof suffixes[0]; i++) {
        size_t sn = strlen(suffixes[i]);
        if (n > sn && strcmp(out + n - sn, suffixes[i]) == 0) {
            out[n - sn] = 0;
            break;
        }
    }
    return true;
}

const SpFormat* sp_format_find(const char* name) {
    if (!name || !*name) return NULL;
    for (size_t i = 0; i < SP_FORMAT_COUNT; i++)
        if (ascii_ieq(name, k_formats[i].name)) return &k_formats[i];
    for (size_t i = 0; i < sizeof k_aliases / sizeof k_aliases[0]; i++)
        if (ascii_ieq(name, k_aliases[i].alias)) return sp_format_find(k_aliases[i].name);
    char key[64], other[64];
    if (!vk_name_key(name, key, sizeof key)) return NULL;
    for (size_t i = 0; i < SP_FORMAT_COUNT; i++)
        if (vk_name_key(k_vk_names[i], other, sizeof other) && strcmp(key, other) == 0) return &k_formats[i];
    return NULL;
}

const SpFormat* sp_format_vk(VkFormat vk) {
    for (size_t i = 0; i < SP_FORMAT_COUNT; i++)
        if (k_formats[i].vk == vk) return &k_formats[i];
    return NULL;
}

size_t sp_format_level_bytes(const SpFormat* fmt, uint32_t w, uint32_t h, uint32_t d) {
    size_t bx = ((size_t)w + fmt->bw - 1) / fmt->bw, by = ((size_t)h + fmt->bh - 1) / fmt->bh;
    return bx * by * (size_t)d * fmt->bytes;
}

/* ---- texel layouts ------------------------------------------------------------------------------------------- */

/* One component of a texel: its image channel (0 = r ... 3 = a; depth is 0 and stencil 1), the bit offset in the
   texel's little-endian bytes, its width and its SpNumeric */
typedef struct {
    uint8_t chan, off, width, num;
} SpField;

typedef struct {
    unsigned count;
    SpField f[4];
    bool shared_exp; /* E5B9G9R9: the fields are not used */
} SpLayout;

/* The layout from the Vulkan name. A plain format lists its components from the lowest byte up. A _PACKn format
   lists them from the most significant bit down. D24_UNORM_S8_UINT is the packed 32-bit word (depth in bits 0-23,
   stencil in 24-31): a copy of its depth aspect to a buffer reads the same word and ignores the top byte.
   D32_SFLOAT_S8_UINT is the layout of a copy of its depth aspect only: one float, no stencil */
static bool texel_layout(const SpFormat* fmt, SpLayout* out) {
    *out = (SpLayout){0};
    if (fmt->kind != SP_KIND_PLAIN && fmt->kind != SP_KIND_PACKED && fmt->kind != SP_KIND_DEPTH) return false;
    const SpFormat* row = sp_format_vk(fmt->vk);
    if (!row) return false;
    const char* name = k_vk_names[row - k_formats];
    switch (fmt->vk) {
    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32: out->shared_exp = true; return true;
    case VK_FORMAT_X8_D24_UNORM_PACK32:
        out->count = 1;
        out->f[0] = (SpField){0, 0, 24, SP_UNORM};
        return true;
    case VK_FORMAT_D24_UNORM_S8_UINT:
        out->count = 2;
        out->f[0] = (SpField){0, 0, 24, SP_UNORM};
        out->f[1] = (SpField){1, 24, 8, SP_UINT};
        return true;
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
        out->count = 1;
        out->f[0] = (SpField){0, 0, 32, SP_SFLOAT};
        return true;
    default: break;
    }
    bool packed = strstr(name, "_PACK") != NULL;
    unsigned total = 0;
    const char* p = name;
    while (*p && *p != '_') {
        unsigned chan;
        switch (*p) {
        case 'R': case 'D': chan = 0; break;
        case 'G': case 'S': chan = 1; break;
        case 'B': chan = 2; break;
        case 'A': chan = 3; break;
        default: return false;
        }
        p++;
        unsigned width = 0;
        while (*p >= '0' && *p <= '9') width = width * 10 + (unsigned)(*p++ - '0');
        if (width == 0 || width > 32 || out->count == 4) return false;
        unsigned num = fmt->numeric;
        if (num == SP_SRGB && chan == 3) num = SP_UNORM;
        out->f[out->count++] = (SpField){(uint8_t)chan, (uint8_t)total, (uint8_t)width, (uint8_t)num};
        total += width;
    }
    if (total != (unsigned)fmt->bytes * 8) return false;
    if (packed)
        for (unsigned i = 0; i < out->count; i++) out->f[i].off = (uint8_t)(total - out->f[i].off - out->f[i].width);
    return true;
}

/* bits at off, width up to 32, of little-endian bytes */
static void put_bits(uint8_t* p, unsigned off, unsigned width, uint32_t v) {
    for (unsigned i = 0; i < width; i++) {
        unsigned b = off + i;
        uint8_t m = (uint8_t)(1u << (b & 7));
        if ((v >> i) & 1u)
            p[b >> 3] |= m;
        else
            p[b >> 3] &= (uint8_t)~m;
    }
}

static uint32_t get_bits(const uint8_t* p, unsigned off, unsigned width) {
    uint32_t v = 0;
    for (unsigned i = 0; i < width; i++) {
        unsigned b = off + i;
        v |= (uint32_t)((p[b >> 3] >> (b & 7)) & 1u) << i;
    }
    return v;
}

/* ---- conversions --------------------------------------------------------------------------------------------- */

static uint32_t f32_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

static float bits_f32(uint32_t u) {
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* v >> s rounded to nearest, ties to even */
static uint32_t shr_rne(uint32_t v, unsigned s) {
    if (s == 0) return v;
    if (s > 31) return 0;
    uint32_t q = v >> s, rem = v & ((1u << s) - 1), half = 1u << (s - 1);
    if (rem > half || (rem == half && (q & 1u))) q++;
    return q;
}

/* a float to a small float of 5 exponent bits (bias 15) and mbits mantissa bits, with a sign bit above them when
   is_signed (half: mbits 10), rounded to nearest even. Overflow gives infinity; an unsigned format clamps negatives
   to 0 (B10G11R11: mbits 6 and 5) */
static uint32_t float_to_small(float f, unsigned mbits, bool is_signed) {
    uint32_t x = f32_bits(f), sign = x >> 31, inf = 31u << mbits, out;
    x &= 0x7FFFFFFFu;
    if (x > 0x7F800000u) out = inf | (1u << (mbits - 1));
    else if (sign && !is_signed) return 0;
    else if (x == 0x7F800000u) out = inf;
    else {
        int e = (int)(x >> 23) - 127 + 15;
        uint32_t m = x & 0x7FFFFFu;
        if (e >= 31) out = inf;
        else if (e <= 0) out = shr_rne(m | 0x800000u, (unsigned)(24 - (int)mbits - e));
        else out = shr_rne((uint32_t)e << 23 | m, 23 - mbits);
    }
    return is_signed ? out | sign << (mbits + 5) : out;
}

static float small_to_float(uint32_t v, unsigned mbits, bool is_signed) {
    uint32_t m = v & ((1u << mbits) - 1), e = (v >> mbits) & 31u;
    float f;
    if (e == 0) f = ldexpf((float)m, -14 - (int)mbits);
    else if (e == 31) f = m ? NAN : INFINITY;
    else f = ldexpf((float)(m | 1u << mbits), (int)e - 15 - (int)mbits);
    return is_signed && ((v >> (mbits + 5)) & 1u) ? -f : f;
}

static float srgb_oetf(float v) {
    return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static float srgb_eotf(float c) { return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f); }

/* round to nearest and clamp to lo..hi; NaN gives 0 */
static double round_clamp(double v, double lo, double hi) {
    if (v != v) return 0;
    v = floor(v + 0.5);
    return v < lo ? lo : v > hi ? hi : v;
}

static uint32_t encode_field(float v, unsigned num, unsigned width) {
    double umax = ldexp(1.0, (int)width) - 1, smax = ldexp(1.0, (int)width - 1) - 1;
    uint32_t mask = (uint32_t)umax;
    switch (num) {
    case SP_SRGB: {
        float c = v != v ? 0.0f : v < 0 ? 0.0f : v > 1 ? 1.0f : v;
        return (uint32_t)round_clamp((double)srgb_oetf(c) * umax, 0, umax);
    }
    case SP_UNORM: {
        double c = v != v ? 0.0 : v < 0 ? 0.0 : v > 1 ? 1.0 : (double)v;
        return (uint32_t)round_clamp(c * umax, 0, umax);
    }
    case SP_SNORM: {
        double c = v != v ? 0.0 : v < -1 ? -1.0 : v > 1 ? 1.0 : (double)v;
        return (uint32_t)(uint64_t)(int64_t)round_clamp(c * smax, -smax, smax) & mask;
    }
    case SP_UINT:
    case SP_USCALED: return (uint32_t)round_clamp(v, 0, umax);
    case SP_SINT:
    case SP_SSCALED: return (uint32_t)(uint64_t)(int64_t)round_clamp(v, -smax - 1, smax) & mask;
    case SP_SFLOAT: return width == 32 ? f32_bits(v) : float_to_small(v, width - 6, true);
    case SP_UFLOAT: return float_to_small(v, width - 5, false);
    default: return 0;
    }
}

static float decode_field(uint32_t q, unsigned num, unsigned width) {
    double umax = ldexp(1.0, (int)width) - 1, smax = ldexp(1.0, (int)width - 1) - 1;
    int64_t s = (int64_t)q;
    if (width < 64 && (q >> (width - 1)) & 1u) s -= (int64_t)1 << width;
    switch (num) {
    case SP_SRGB: return srgb_eotf((float)(q / umax));
    case SP_UNORM: return (float)(q / umax);
    case SP_SNORM: {
        double f = (double)s / smax;
        return (float)(f < -1 ? -1 : f);
    }
    case SP_UINT:
    case SP_USCALED: return (float)q;
    case SP_SINT:
    case SP_SSCALED: return (float)s;
    case SP_SFLOAT: return width == 32 ? bits_f32(q) : small_to_float(q, width - 6, true);
    case SP_UFLOAT: return small_to_float(q, width - 5, false);
    default: return 0;
    }
}

/* E5B9G9R9 as the Vulkan specification's "Shared Exponent" section gives it: N = 9, B = 15, Emax = 31 */
static uint32_t encode_rgb9e5(const float* rgb) {
    const double max_val = 511.0 / 512.0 * 65536.0;
    double c[3], maxc = 0;
    for (int i = 0; i < 3; i++) {
        double v = rgb[i];
        c[i] = v != v || v < 0 ? 0 : v > max_val ? max_val : v;
        if (c[i] > maxc) maxc = c[i];
    }
    int e2 = 0;
    int floor_log2 = maxc > 0 ? (frexp(maxc, &e2), e2 - 1) : -16;
    int exp_shared = (floor_log2 < -16 ? -16 : floor_log2) + 1 + 15;
    double max_s = floor(maxc / ldexp(1.0, exp_shared - 15 - 9) + 0.5);
    if (max_s == 512.0) exp_shared++;
    uint32_t out = (uint32_t)exp_shared << 27;
    for (int i = 0; i < 3; i++) {
        double q = floor(c[i] / ldexp(1.0, exp_shared - 15 - 9) + 0.5);
        out |= (uint32_t)(q > 511 ? 511 : q) << (9 * i);
    }
    return out;
}

static void decode_rgb9e5(uint32_t v, float* rgb) {
    int e = (int)(v >> 27);
    for (int i = 0; i < 3; i++) rgb[i] = (float)ldexp((double)((v >> (9 * i)) & 511u), e - 15 - 9);
}

bool sp_encode(const SpFormat* fmt, const SpImage* img, uint8_t* out) {
    SpLayout lay;
    if (!texel_layout(fmt, &lay)) return false;
    size_t n = (size_t)img->w * img->h;
    for (size_t i = 0; i < n; i++) {
        const float* px = img->rgba + i * 4;
        uint8_t* t = out + i * fmt->bytes;
        memset(t, 0, fmt->bytes);
        if (lay.shared_exp) {
            uint32_t v = encode_rgb9e5(px);
            for (int b = 0; b < 4; b++) t[b] = (uint8_t)(v >> (8 * b));
            continue;
        }
        for (unsigned f = 0; f < lay.count; f++) {
            const SpField* fd = &lay.f[f];
            put_bits(t, fd->off, fd->width, encode_field(px[fd->chan], fd->num, fd->width));
        }
    }
    return true;
}

bool sp_decode(const SpFormat* fmt, const uint8_t* src, uint32_t w, uint32_t h, SpImage* img) {
    SpLayout lay;
    if (!texel_layout(fmt, &lay) || w == 0 || h == 0) return false;
    size_t n = (size_t)w * h;
    float* rgba = malloc(n * 4 * sizeof *rgba);
    if (!rgba) return false;
    for (size_t i = 0; i < n; i++) {
        float* px = rgba + i * 4;
        const uint8_t* t = src + i * fmt->bytes;
        px[0] = px[1] = px[2] = 0.0f;
        px[3] = 1.0f;
        if (lay.shared_exp) {
            decode_rgb9e5(get_bits(t, 0, 32), px);
            continue;
        }
        for (unsigned f = 0; f < lay.count; f++) {
            const SpField* fd = &lay.f[f];
            px[fd->chan] = decode_field(get_bits(t, fd->off, fd->width), fd->num, fd->width);
        }
    }
    img->w = w;
    img->h = h;
    img->rgba = rgba;
    return true;
}

/* ---- images -------------------------------------------------------------------------------------------------- */

void sp_image_free(SpImage* img) {
    free(img->rgba);
    *img = (SpImage){0};
}

static bool image_alloc(SpImage* img, uint32_t w, uint32_t h) {
    *img = (SpImage){0};
    if (w == 0 || h == 0 || (uint64_t)w * h > ((uint64_t)1 << 28)) return false;
    img->rgba = calloc((size_t)w * h * 4, sizeof *img->rgba);
    if (!img->rgba) return false;
    img->w = w;
    img->h = h;
    return true;
}

/* a header token: skips white space and '#' comments */
static bool read_token(FILE* f, char* buf, size_t size) {
    int c = fgetc(f);
    for (;;) {
        while (c == ' ' || c == '\t' || c == '\r' || c == '\n') c = fgetc(f);
        if (c != '#') break;
        while (c != '\n' && c != EOF) c = fgetc(f);
    }
    size_t n = 0;
    while (c != EOF && c != ' ' && c != '\t' && c != '\r' && c != '\n') {
        if (n + 1 >= size) return false;
        buf[n++] = (char)c;
        c = fgetc(f);
    }
    buf[n] = 0;
    return n > 0; /* the white space after the token is consumed */
}

static bool read_uint(FILE* f, uint32_t* out) {
    char buf[32], *end;
    if (!read_token(f, buf, sizeof buf) || buf[0] < '0' || buf[0] > '9') return false;
    unsigned long v = strtoul(buf, &end, 10);
    if (*end || v > 0xFFFFFFFFul) return false;
    *out = (uint32_t)v;
    return true;
}

bool sp_image_load(const char* path, SpImage* out, char* err, size_t err_size) {
    *out = (SpImage){0};
    FILE* f = fopen(path, "rb");
    if (!f) {
        snprintf(err, err_size, "%s: can't open", path);
        return false;
    }
    char magic[3] = {0};
    uint32_t w = 0, h = 0, maxval = 1;
    bool ok = false;
    if (fread(magic, 1, 2, f) != 2 || magic[0] != 'P') {
        snprintf(err, err_size, "%s: not a PPM, PGM or PFM", path);
        goto done;
    }
    char t = magic[1];
    bool pfm = t == 'F' || t == 'f';
    if (!pfm && t != '2' && t != '3' && t != '5' && t != '6') {
        snprintf(err, err_size, "%s: unsupported type P%c", path, t);
        goto done;
    }
    unsigned ch = (t == '3' || t == '6' || t == 'F') ? 3 : 1;
    double scale = 1;
    if (!read_uint(f, &w) || !read_uint(f, &h)) {
        snprintf(err, err_size, "%s: bad header", path);
        goto done;
    }
    if (pfm) {
        char buf[64], *end;
        if (!read_token(f, buf, sizeof buf) || (scale = strtod(buf, &end), *end) || scale == 0) {
            snprintf(err, err_size, "%s: bad PFM scale", path);
            goto done;
        }
    } else if (!read_uint(f, &maxval) || maxval == 0 || maxval > 65535) {
        snprintf(err, err_size, "%s: bad maxval", path);
        goto done;
    }
    if (!image_alloc(out, w, h)) {
        snprintf(err, err_size, "%s: bad size %ux%u", path, (unsigned)w, (unsigned)h);
        goto done;
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t row = pfm ? h - 1 - y : y; /* a PFM's rows go bottom to top */
        for (uint32_t x = 0; x < w; x++) {
            float* px = out->rgba + ((size_t)row * w + x) * 4;
            float v[3];
            for (unsigned c = 0; c < ch; c++) {
                if (pfm) {
                    uint8_t b[4];
                    if (fread(b, 1, 4, f) != 4) goto truncated;
                    uint32_t u = scale < 0 ? (uint32_t)b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 |
                                                 (uint32_t)b[3] << 24
                                           : (uint32_t)b[3] | (uint32_t)b[2] << 8 | (uint32_t)b[1] << 16 |
                                                 (uint32_t)b[0] << 24;
                    v[c] = bits_f32(u);
                } else if (t == '2' || t == '3') {
                    uint32_t s;
                    if (!read_uint(f, &s)) goto truncated;
                    v[c] = (float)(s > maxval ? 1.0 : (double)s / maxval);
                } else {
                    uint8_t b[2];
                    uint32_t s;
                    if (maxval > 255) {
                        if (fread(b, 1, 2, f) != 2) goto truncated;
                        s = (uint32_t)b[0] << 8 | b[1];
                    } else {
                        if (fread(b, 1, 1, f) != 1) goto truncated;
                        s = b[0];
                    }
                    v[c] = (float)(s > maxval ? 1.0 : (double)s / maxval);
                }
            }
            px[0] = v[0];
            px[1] = ch == 3 ? v[1] : v[0];
            px[2] = ch == 3 ? v[2] : v[0];
            px[3] = 1.0f;
        }
    }
    ok = true;
    goto done;
truncated:
    snprintf(err, err_size, "%s: truncated or bad pixel data", path);
    sp_image_free(out);
done:
    fclose(f);
    return ok;
}

bool sp_image_save(const char* path, const SpImage* img, char* err, size_t err_size) {
    size_t len = strlen(path);
    bool pfm = len >= 4 && ascii_ieq(path + len - 4, ".pfm");
    FILE* f = fopen(path, "wb");
    if (!f) {
        snprintf(err, err_size, "%s: can't create", path);
        return false;
    }
    bool ok = fprintf(f, pfm ? "PF\n%u %u\n-1.0\n" : "P6\n%u %u\n255\n", (unsigned)img->w, (unsigned)img->h) > 0;
    for (uint32_t y = 0; ok && y < img->h; y++) {
        uint32_t row = pfm ? img->h - 1 - y : y;
        for (uint32_t x = 0; ok && x < img->w; x++) {
            const float* px = img->rgba + ((size_t)row * img->w + x) * 4;
            uint8_t b[12];
            size_t n = 0;
            for (int c = 0; c < 3; c++) {
                if (pfm) {
                    uint32_t u = f32_bits(px[c]);
                    for (int k = 0; k < 4; k++) b[n++] = (uint8_t)(u >> (8 * k));
                } else
                    b[n++] = (uint8_t)encode_field(px[c], SP_UNORM, 8);
            }
            ok = fwrite(b, 1, n, f) == n;
        }
    }
    if (fclose(f) != 0) ok = false;
    if (!ok) snprintf(err, err_size, "%s: write error", path);
    return ok;
}

/* one axis: dn samples of 4 floats at dst (stride dstride floats) from sn at src (stride sstride). A box filter of
   the exact source footprint when dn < sn, bilinear between source centers otherwise */
static void resample_1d(const float* src, size_t sstride, uint32_t sn, float* dst, size_t dstride, uint32_t dn) {
    for (uint32_t i = 0; i < dn; i++) {
        float acc[4] = {0, 0, 0, 0};
        if (dn < sn) {
            double lo = (double)i * sn / dn, hi = (double)(i + 1) * sn / dn, total = 0;
            for (uint32_t s = (uint32_t)lo; s < sn && s < hi; s++) {
                double a = s < lo ? lo : s, b = s + 1.0 > hi ? hi : s + 1.0, wgt = b - a;
                if (wgt <= 0) continue;
                for (int c = 0; c < 4; c++) acc[c] += (float)(wgt * src[s * sstride + (size_t)c]);
                total += wgt;
            }
            for (int c = 0; c < 4; c++) acc[c] = (float)(acc[c] / total);
        } else {
            double x = ((double)i + 0.5) * sn / dn - 0.5;
            if (x < 0) x = 0;
            uint32_t s0 = (uint32_t)x, s1 = s0 + 1 < sn ? s0 + 1 : s0;
            float fr = (float)(x - s0);
            for (int c = 0; c < 4; c++)
                acc[c] = src[s0 * sstride + (size_t)c] * (1 - fr) + src[s1 * sstride + (size_t)c] * fr;
        }
        for (int c = 0; c < 4; c++) dst[i * dstride + (size_t)c] = acc[c];
    }
}

bool sp_image_resize(const SpImage* img, uint32_t w, uint32_t h, SpImage* out) {
    if (!img->rgba || img->w == 0 || img->h == 0) return false;
    if (!image_alloc(out, w, h)) return false;
    float* tmp = malloc((size_t)w * img->h * 4 * sizeof *tmp);
    if (!tmp) {
        sp_image_free(out);
        return false;
    }
    for (uint32_t y = 0; y < img->h; y++)
        resample_1d(img->rgba + (size_t)y * img->w * 4, 4, img->w, tmp + (size_t)y * w * 4, 4, w);
    for (uint32_t x = 0; x < w; x++)
        resample_1d(tmp + (size_t)x * 4, (size_t)w * 4, img->h, out->rgba + (size_t)x * 4, (size_t)w * 4, h);
    free(tmp);
    return true;
}

/* splitmix64 */
static uint64_t rng_next(uint64_t* state) {
    uint64_t z = (*state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

bool sp_image_pattern(const char* pattern, uint32_t w, uint32_t h, uint64_t seed, SpImage* out) {
    float solid[4] = {0, 0, 0, 1};
    enum { NOISE, GRADIENT, CHECKER, SOLID, ZERO } kind;
    if (ascii_ieq(pattern, "noise")) kind = NOISE;
    else if (ascii_ieq(pattern, "gradient")) kind = GRADIENT;
    else if (ascii_ieq(pattern, "checker")) kind = CHECKER;
    else if (ascii_ieq(pattern, "zero")) kind = ZERO;
    else if (ascii_iprefix(pattern, "solid:")) {
        kind = SOLID;
        const char* p = pattern + 6;
        for (int c = 0; c < 4; c++) {
            char* end;
            solid[c] = strtof(p, &end);
            if (end == p) return false;
            p = end;
            if (*p == 0) break;
            if (*p != ',' || c == 3) return false;
            p++;
        }
    } else
        return false;
    if (!image_alloc(out, w, h)) return false;
    uint64_t state = seed;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            float* px = out->rgba + ((size_t)y * w + x) * 4;
            float fx = w > 1 ? (float)x / (float)(w - 1) : 0.0f, fy = h > 1 ? (float)y / (float)(h - 1) : 0.0f;
            switch (kind) {
            case NOISE:
                for (int c = 0; c < 4; c++) px[c] = (float)(rng_next(&state) >> 40) * (1.0f / 16777216.0f);
                break;
            case GRADIENT: px[0] = fx; px[1] = fy; px[2] = 1.0f - fx; px[3] = 1.0f; break;
            case CHECKER: {
                float v = ((x / 8 + y / 8) & 1u) ? 1.0f : 0.0f;
                px[0] = px[1] = px[2] = v;
                px[3] = 1.0f;
                break;
            }
            case SOLID: memcpy(px, solid, sizeof solid); break;
            case ZERO: px[0] = px[1] = px[2] = px[3] = 0.0f; break;
            }
        }
    return true;
}
