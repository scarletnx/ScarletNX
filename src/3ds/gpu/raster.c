/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/renderer_software/sw_rasterizer.cpp (AddTriangle, MakeScreenCoords,
   ProcessTriangle, TextureColor, PixelColor, WriteTevConfig, WriteFog, DoAlphaTest, DoDepthStencilTest),
   sw_clipper.cpp, sw_texturing.cpp and sw_framebuffer.cpp (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator
   Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e. Arithmetic on float24 values is host float with
   PICA multiplication, as Azahar's f24. The framebuffer is read and written pixel by pixel in physical memory. */
#include "3ds/gpu/raster.h"
#include "3ds/gpu/pica_internal.h"
#include "3ds/gpu/texture.h"
#include "3ds/gpu/transfer.h"
#include "common/utilities/bits.h"
#include <math.h>
#include <string.h>

#define R(i) (p->regs[i])

/* register indices (RegsInternal) */
enum {
    CULL_MODE = 0x040,
    VIEWPORT_X = 0x041,
    VIEWPORT_Y = 0x043,
    CLIP_ENABLE = 0x047,
    CLIP_COEF = 0x048,
    DEPTH_RANGE = 0x04d,
    DEPTH_NEAR = 0x04e,
    SCISSOR_MODE = 0x065,
    SCISSOR_1 = 0x066,
    SCISSOR_2 = 0x067,
    VIEWPORT_CORNER = 0x068,
    DEPTHMAP_ENABLE = 0x06d,
    TEX_MAIN = 0x080,
    TEX_CUBE = 0x086, /* 5 face addresses after +X */
    TEX0_FORMAT = 0x08e,
    TEV_STAGES = 0x0c0,
    FOG_MODE = 0x0e0,
    FOG_COLOR = 0x0e1,
    TEV_BUFFER_COLOR = 0x0fd,
    OM_MODE = 0x100,
    OM_BLEND = 0x101,
    OM_LOGIC = 0x102,
    OM_BLEND_CONST = 0x103,
    OM_ALPHA_TEST = 0x104,
    OM_STENCIL = 0x105,
    OM_STENCIL_OP = 0x106,
    OM_DEPTH_COLOR = 0x107,
    FB_COLOR_WRITE = 0x113,
    FB_DEPTH_WRITE = 0x115,
    FB_DEPTH_FORMAT = 0x116,
    FB_COLOR_FORMAT = 0x117,
    FB_DEPTH_ADDR = 0x11c,
    FB_COLOR_ADDR = 0x11d,
    FB_SIZE = 0x11e,
    LIGHTING_DISABLE = 0x1c6
};
static const uint32_t tex_config[3] = {0x081, 0x091, 0x099}, tex_format[3] = {0x08e, 0x096, 0x09e};
static const uint32_t tev_stage[6] = {0x0c0, 0x0c8, 0x0d0, 0x0d8, 0x0f0, 0x0f8};

static float mul(float a, float b) { return snx_ctr_pica_mul(a, b); }
static float f24(uint32_t raw) { return snx_ctr_f24(raw & 0xffffff); }
static float dot4(const float* a, const float* b) {
    return mul(a[0], b[0]) + mul(a[1], b[1]) + mul(a[2], b[2]) + mul(a[3], b[3]);
}
static float dot3(const float* a, const float* b) { return mul(a[0], b[0]) + mul(a[1], b[1]) + mul(a[2], b[2]); }
static uint8_t to_u8(float f) { return (uint8_t)to_s32(f); }

/* the rasterizer's vertex: an OutputVertex and its screen position after the perspective divide */
typedef struct {
    SnxCtrVertex v;
    float screen[3];
} Vertex;
#define VERTEX_FLOATS (sizeof(SnxCtrVertex) / sizeof(float))

/* Vertex::Lerp: every attribute, factor of a and 1 - factor of b */
static void lerp(float factor, const Vertex* a, const Vertex* b, Vertex* out) {
    const float* x = (const float*)&a->v;
    const float* y = (const float*)&b->v;
    float* o = (float*)&out->v;
    *out = *a;
    for (unsigned i = 0; i < VERTEX_FLOATS; ++i) {
        if (i == 17 || i == 21) continue; /* the two padding words */
        o[i] = mul(x[i], factor) + mul(y[i], 1.0f - factor);
    }
}

/* ClippingEdge: inside when dot(pos + bias, coefficients) >= -EPSILON_Z, a margin some 2D games need */
typedef struct {
    float coeffs[4], bias[4];
} Edge;
static float edge_distance(const Edge* e, const Vertex* v) {
    float p4[4];
    for (unsigned i = 0; i < 4; ++i) p4[i] = v->v.pos[i] + e->bias[i];
    return dot4(p4, e->coeffs);
}
static bool inside(const Edge* e, const Vertex* v) { return edge_distance(e, v) >= -0.00000001f; }

/* Sutherland-Hodgman against one edge; at most one vertex more per edge, 9 after seven edges and a custom plane */
#define MAX_VERTICES 12
static unsigned clip(const Edge* e, const Vertex* in, unsigned n, Vertex* out) {
    unsigned count = 0;
    const Vertex* reference = &in[n - 1];
    for (unsigned i = 0; i < n; ++i) {
        const Vertex* v = &in[i];
        if (inside(e, v)) {
            if (!inside(e, reference) && count < MAX_VERTICES) {
                float dp = edge_distance(e, v), dp_prev = edge_distance(e, reference);
                lerp(dp_prev / (dp_prev - dp), v, reference, &out[count++]);
            }
            if (count < MAX_VERTICES) out[count++] = *v;
        } else if (inside(e, reference) && count < MAX_VERTICES) {
            float dp = edge_distance(e, v), dp_prev = edge_distance(e, reference);
            lerp(dp_prev / (dp_prev - dp), v, reference, &out[count++]);
        }
        reference = v;
    }
    return count;
}

static void make_screen(const SnxCtrPica* p, Vertex* v) {
    float half_x = f24(R(VIEWPORT_X)), half_y = f24(R(VIEWPORT_Y));
    /* viewport_corner: signed 10-bit x and y */
    int32_t cx = (int32_t)(R(VIEWPORT_CORNER) << 22) >> 22, cy = (int32_t)(R(VIEWPORT_CORNER) << 6) >> 22;
    float inv_w = 1.0f / v->v.pos[3];
    v->v.pos[3] = inv_w;
    float* attrs[] = {v->v.quat, v->v.color};
    for (unsigned a = 0; a < 2; ++a)
        for (unsigned i = 0; i < 4; ++i) attrs[a][i] = mul(attrs[a][i], inv_w);
    for (unsigned i = 0; i < 2; ++i) {
        v->v.tc0[i] = mul(v->v.tc0[i], inv_w);
        v->v.tc1[i] = mul(v->v.tc1[i], inv_w);
        v->v.tc2[i] = mul(v->v.tc2[i], inv_w);
    }
    v->v.tc0_w = mul(v->v.tc0_w, inv_w);
    for (unsigned i = 0; i < 3; ++i) v->v.view[i] = mul(v->v.view[i], inv_w);
    v->screen[0] = mul(mul(v->v.pos[0], inv_w) + 1.0f, half_x) + (float)cx;
    v->screen[1] = mul(mul(v->v.pos[1], inv_w) + 1.0f, half_y) + (float)cy;
    v->screen[2] = mul(v->v.pos[2], inv_w);
}

/* Fix12P4: screen coordinates in 12.4 fixed point, as u16 (a negative coordinate wraps, as the cast does on x86) */
static uint16_t fix12p4(float f) { return (uint16_t)to_s32(roundf(f * 16.0f)); }
static int signed_area(const uint16_t* a, const uint16_t* b, const uint16_t* c) {
    int x1 = b[0] - a[0], y1 = b[1] - a[1], x2 = c[0] - a[0], y2 = c[1] - a[1];
    return x1 * y2 - y1 * x2;
}
static bool right_or_flat_bottom(const uint16_t* v, const uint16_t* l1, const uint16_t* l2) {
    if (l1[1] == l2[1]) return v[1] < l1[1];
    return (int32_t)v[0] <
           (int32_t)l1[0] + ((int32_t)l2[0] - l1[0]) * ((int32_t)v[1] - l1[1]) / ((int32_t)l2[1] - l1[1]);
}

/* ---- textures ---- */

static int32_t wrap(unsigned mode, int32_t v, uint32_t size) {
    switch (mode) {
        case 4: /* ClampToEdge2: a negative coordinate repeats */
            if (v < 0) return (int32_t)((uint32_t)v % size);
            /* fallthrough */
        case 0: return v<0 ? 0 : v>(int32_t) size - 1 ? (int32_t)size - 1 : v; /* ClampToEdge */
        case 1: return v;                                                      /* ClampToBorder */
        case 3: {                                                              /* MirroredRepeat */
            uint32_t c = (uint32_t)v % (2 * size);
            if (c >= size) c = 2 * size - 1 - c;
            return (int32_t)c;
        }
        default: return (int32_t)((uint32_t)v % size); /* ClampToBorder2, Repeat and its aliases */
    }
}

/* ConvertCubeCoord: the face and its coordinates for a cube map's direction */
static void cube(const SnxCtrPica* p, float u, float v, float w, float* s, float* t, uint32_t* address) {
    float au = fabsf(u), av = fabsf(v), aw = fabsf(w), x, y, z;
    unsigned face;
    if (au > av && au > aw) {
        face = u > 0 ? 0 : 1;
        y = u > 0 ? -v : v;
        x = -w;
        z = u;
    } else if (av > aw) {
        face = v > 0 ? 2 : 3;
        x = v > 0 ? u : -u;
        y = w;
        z = v;
    } else {
        face = w > 0 ? 4 : 5;
        y = w > 0 ? -v : v;
        x = u;
        z = w;
    }
    /* GetCubePhysicalAddress: the other faces replace the low 22 bits of +X's address */
    uint32_t a = R(tex_config[0] + 4) & 0x0fffffff;
    if (face) a = (a & ~0x3fffffu) | (R(TEX_CUBE + face - 1) & 0x3fffff);
    *address = a * 8;
    *s = mul(x / z, 0.5f) + 0.5f;
    *t = mul(y / z, 0.5f) + 0.5f;
}

static void texture_color(const SnxCtrPica* p, SnxCtrMemory* m, float uv[3][2], float tc0_w, uint8_t out[4][4]) {
    memset(out, 0, 16);
    for (unsigned i = 0; i < 3; ++i) {
        if (!((R(TEX_MAIN) >> i) & 1)) continue;
        const uint32_t* c = &p->regs[tex_config[i]];
        uint32_t address = (c[4] & 0x0fffffff) * 8, width = (c[1] >> 16) & 0x7ff, height = c[1] & 0x7ff;
        unsigned type = (c[2] >> 28) & 7, wrap_t = (c[2] >> 8) & 7, wrap_s = (c[2] >> 12) & 7;
        if (!address) {
            out[i][0] = out[i][1] = out[i][2] = 0, out[i][3] = 255;
            continue;
        }
        unsigned coord = i == 2 && ((R(TEX_MAIN) >> 13) & 1) ? 1 : i;
        float u = uv[coord][0], v = uv[coord][1];
        if (i == 0) {      /* only unit 0 has a texture type */
            if (type == 1) /* TextureCube */
                cube(p, u, v, tc0_w, &u, &v, &address);
            else if (type == 3) { /* Projection2D */
                u /= tc0_w;
                v /= tc0_w;
            } else if (type == 5) /* Disabled */
                continue;
        }
        int32_t s = to_s32(mul(u, (float)width)), t = to_s32(mul(v, (float)height));
        bool border = (wrap_s == 1 && (s < 0 || s >= (int32_t)width)) || (wrap_s == 5 && s >= (int32_t)width) ||
                      (wrap_t == 1 && (t < 0 || t >= (int32_t)height)) || (wrap_t == 5 && t >= (int32_t)height);
        if (border) {
            for (unsigned k = 0; k < 4; ++k) out[i][k] = (uint8_t)(c[0] >> (k * 8));
            continue;
        }
        if (!width || !height) continue; /* a modulo by zero in Azahar: nothing sensible to sample */
        /* textures are stored bottom to top */
        s = wrap(wrap_s, s, width);
        t = (int32_t)height - 1 - wrap(wrap_t, t, height);
        (void)snx_ctr_texel(m, address, width, R(tex_format[i]) & 15, (unsigned)s, (unsigned)t, out[i]);
    }
}

/* ---- texture combiners (sw_texturing.cpp) ---- */

static void color_modifier(unsigned f, const uint8_t* v, int out[3]) {
    for (unsigned i = 0; i < 3; ++i) {
        int x;
        switch (f) {
            case 0x0: x = v[i]; break;
            case 0x1: x = 255 - v[i]; break;
            case 0x2: x = v[3]; break;
            case 0x3: x = 255 - v[3]; break;
            case 0x4: x = v[0]; break;
            case 0x5: x = 255 - v[0]; break;
            case 0x8: x = v[1]; break;
            case 0x9: x = 255 - v[1]; break;
            case 0xc: x = v[2]; break;
            case 0xd: x = 255 - v[2]; break;
            default: x = 0; break; /* unassigned modifiers: UNREACHABLE in Azahar */
        }
        out[i] = x;
    }
}
static int alpha_modifier(unsigned f, const uint8_t* v) {
    static const unsigned source[8] = {3, 3, 0, 0, 1, 1, 2, 2};
    int x = v[source[f & 7]];
    return f & 1 ? 255 - x : x;
}
static void color_combine(unsigned op, const int in[3][3], int out[3]) {
    for (unsigned i = 0; i < 3; ++i) {
        int a = in[0][i], b = in[1][i], c = in[2][i], r;
        switch (op) {
            case 0: r = a; break;
            case 1: r = a * b / 255; break;
            case 2: r = a + b < 255 ? a + b : 255; break;
            case 3: r = snx_clamp_int(a + b - 128, 0, 255); break;
            case 4: r = (a * c + b * (255 - c)) / 255; break;
            case 5: r = a - b > 0 ? a - b : 0; break;
            case 8: r = (a * b + 255 * c) / 255, r = r < 255 ? r : 255; break;
            case 9: r = a + b < 255 ? a + b : 255, r = r * c / 255; break;
            case 6:
            case 7: { /* Dot3: the same value in every component */
                int d = ((in[0][0] * 2 - 255) * (in[1][0] * 2 - 255) + 128) / 256 +
                        ((in[0][1] * 2 - 255) * (in[1][1] * 2 - 255) + 128) / 256 +
                        ((in[0][2] * 2 - 255) * (in[1][2] * 2 - 255) + 128) / 256;
                r = snx_clamp_int(d, 0, 255);
                break;
            }
            default: r = 0; break;
        }
        out[i] = (uint8_t)r;
    }
}
static int alpha_combine(unsigned op, const int in[3]) {
    int a = in[0], b = in[1], c = in[2];
    switch (op) {
        case 0: return a;
        case 1: return a * b / 255;
        case 2: return a + b < 255 ? a + b : 255;
        case 3: return snx_clamp_int(a + b - 128, 0, 255);
        case 4: return (a * c + b * (255 - c)) / 255;
        case 5: return a - b > 0 ? a - b : 0;
        case 8: return (a * b + 255 * c) / 255 < 255 ? (a * b + 255 * c) / 255 : 255;
        case 9: return (a + b < 255 ? a + b : 255) * c / 255;
        default: return 0;
    }
}

/* WriteTevConfig: six stages; stage 0's "previous" in sources 1 and 2 reads source 3 instead */
static void tev(const SnxCtrPica* p, uint8_t textures[4][4], const uint8_t primary[4], const uint8_t fragment[4],
                const uint8_t secondary[4], uint8_t out[4]) {
    static const uint8_t zero[4] = {0, 0, 0, 0};
    uint8_t buffer[4] = {0, 0, 0, 0}, next[4];
    uint32_t config = R(FOG_MODE);
    for (unsigned k = 0; k < 4; ++k) next[k] = (uint8_t)(R(TEV_BUFFER_COLOR) >> (k * 8));
    memset(out, 0, 4);
    for (unsigned s = 0; s < 6; ++s) {
        const uint32_t* st = &p->regs[tev_stage[s]];
        uint8_t constant[4], previous[4];
        for (unsigned k = 0; k < 4; ++k) constant[k] = (uint8_t)(st[3] >> (k * 8));
        memcpy(previous, out, 4);
        /* the vertex colour, fragment lighting's primary and secondary colours (zero with lighting off) */
        const uint8_t* sources[16] = {primary, fragment, secondary, textures[0], textures[1], textures[2], textures[3]};
        sources[13] = buffer;
        sources[14] = constant;
        sources[15] = previous;
        for (unsigned k = 7; k < 13; ++k) sources[k] = zero; /* unknown sources: UNIMPLEMENTED in Azahar */
        unsigned cs[3] = {st[0] & 15, (st[0] >> 4) & 15, (st[0] >> 8) & 15};
        if (s == 0 && cs[0] == 15) cs[0] = cs[2];
        if (s == 0 && cs[1] == 15) cs[1] = cs[2];
        int color[3][3], result[3], alpha;
        for (unsigned k = 0; k < 3; ++k) color_modifier((st[1] >> (k * 4)) & 15, sources[cs[k]], color[k]);
        unsigned color_op = st[2] & 15, alpha_op = (st[2] >> 16) & 15;
        color_combine(color_op, (const int (*)[3])color, result);
        if (color_op == 7) /* Dot3_RGBA puts its result in alpha too */
            alpha = result[0];
        else {
            int a[3];
            for (unsigned k = 0; k < 3; ++k)
                a[k] = alpha_modifier((st[1] >> (12 + k * 4)) & 7, sources[(st[0] >> (16 + k * 4)) & 15]);
            alpha = (uint8_t)alpha_combine(alpha_op, a);
        }
        unsigned cscale = st[4] & 3, ascale = (st[4] >> 16) & 3;
        unsigned cm = cscale < 3 ? 1u << cscale : 1, am = ascale < 3 ? 1u << ascale : 1;
        for (unsigned k = 0; k < 3; ++k)
            out[k] = (uint8_t)((unsigned)result[k] * cm < 255 ? (unsigned)result[k] * cm : 255);
        out[3] = (uint8_t)((unsigned)alpha * am < 255 ? (unsigned)alpha * am : 255);
        memcpy(buffer, next, 4);
        /* stages 0-3 may update the combiner buffer for the next stage */
        if (s < 4 && ((config >> 8) & (1u << s))) memcpy(next, out, 3);
        if (s < 4 && ((config >> 12) & (1u << s))) next[3] = out[3];
    }
}

static void fog(const SnxCtrPica* p, float depth, uint8_t c[4]) {
    if ((R(FOG_MODE) & 7) != 5) return;
    float index = (R(FOG_MODE) >> 16) & 1 ? (1.0f - depth) * 128.0f : depth * 128.0f;
    float i = floorf(index);
    i = i < 0 ? 0 : i > 127 ? 127 : i;
    float f = index - i;
    uint32_t e = p->fog_lut[(unsigned)i];
    int32_t difference = (int32_t)(e << 19) >> 19; /* 1.1.11, bits 0..12 */
    float factor = (float)((e >> 13) & 0x7ff) / 2047.0f + (float)difference / 2047.0f * f;
    factor = factor < 0 ? 0 : factor > 1 ? 1 : factor;
    for (unsigned k = 0; k < 3; ++k)
        c[k] = (uint8_t)(factor * c[k] + (1.0f - factor) * (float)((R(FOG_COLOR) >> (k * 8)) & 0xff));
}

static bool compare(unsigned func, unsigned a, unsigned b) {
    switch (func & 7) {
        case 0: return false;
        case 1: return true;
        case 2: return a == b;
        case 3: return a != b;
        case 4: return a < b;
        case 5: return a <= b;
        case 6: return a > b;
        default: return a >= b;
    }
}

/* ---- framebuffer (sw_framebuffer.cpp): tiled, rows bottom to top ---- */

/* the framebuffer's colour formats in transfer.h's order (RGB5A1 and RGB565 swap) */
static unsigned color_format(const SnxCtrPica* p) {
    static const unsigned map[8] = {
        SNX_CTR_PIX_RGBA8, SNX_CTR_PIX_RGB8, SNX_CTR_PIX_RGB5A1, SNX_CTR_PIX_RGB565, SNX_CTR_PIX_RGBA4, 5, 6, 7};
    return map[(R(FB_COLOR_FORMAT) >> 16) & 7];
}
static unsigned depth_bytes(unsigned format) { return format == 0 ? 2 : format == 2 ? 3 : format == 3 ? 4 : 0; }
static uint32_t pixel_offset(const SnxCtrPica* p, unsigned x, unsigned y, unsigned bpp) {
    uint32_t width = R(FB_SIZE) & 0x7ff, height = (R(FB_SIZE) >> 12) & 0x3ff; /* the register holds height - 1 */
    y = height - y;
    return snx_ctr_morton_offset(x, y, bpp) + (y & ~7u) * width * bpp;
}
static bool get_pixel(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y, uint8_t rgba[4]) {
    unsigned format = color_format(p), bpp = snx_ctr_pixel_bytes(format);
    unsigned char b[4];
    memset(rgba, 0, 4);
    if (!bpp) return false;
    uint32_t at = (R(FB_COLOR_ADDR) & 0x0fffffff) * 8 + pixel_offset(p, x, y, bpp);
    if (snx_ctr_phys_read(m, at, b, bpp) != SNX_CTR_MEM_OK) return false;
    snx_ctr_pixel_decode(format, b, rgba);
    return true;
}
static void draw_pixel(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y, const uint8_t rgba[4]) {
    unsigned format = color_format(p), bpp = snx_ctr_pixel_bytes(format);
    unsigned char b[4];
    if (!bpp) return;
    uint32_t at = (R(FB_COLOR_ADDR) & 0x0fffffff) * 8 + pixel_offset(p, x, y, bpp);
    snx_ctr_pixel_encode(format, rgba, b);
    (void)snx_ctr_phys_write(m, at, b, bpp);
}
/* depth and stencil: D16, D24 and D24S8, the stencil in the fourth byte */
static bool depth_read(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y, unsigned char b[4]) {
    unsigned bpp = depth_bytes(R(FB_DEPTH_FORMAT) & 3);
    memset(b, 0, 4);
    if (!bpp) return false;
    uint32_t at = (R(FB_DEPTH_ADDR) & 0x0fffffff) * 8 + pixel_offset(p, x, y, bpp);
    return snx_ctr_phys_read(m, at, b, bpp) == SNX_CTR_MEM_OK;
}
static void depth_write(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y, const unsigned char b[4],
                        unsigned first, unsigned count) {
    unsigned bpp = depth_bytes(R(FB_DEPTH_FORMAT) & 3);
    if (!bpp) return;
    uint32_t at = (R(FB_DEPTH_ADDR) & 0x0fffffff) * 8 + pixel_offset(p, x, y, bpp);
    (void)snx_ctr_phys_write(m, at + first, b + first, count);
}
static uint32_t get_depth(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y) {
    unsigned char b[4];
    if (!depth_read(p, m, x, y, b)) return 0;
    return (R(FB_DEPTH_FORMAT) & 3) == 0 ? (uint32_t)(b[0] | (b[1] << 8))
                                         : (uint32_t)(b[0] | (b[1] << 8) | (b[2] << 16));
}
static void set_depth(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y, uint32_t z) {
    unsigned char b[4] = {(unsigned char)z, (unsigned char)(z >> 8), (unsigned char)(z >> 16), 0};
    unsigned format = R(FB_DEPTH_FORMAT) & 3;
    if (format == 0 || format == 2 || format == 3) depth_write(p, m, x, y, b, 0, format == 0 ? 2 : 3);
}
static uint8_t get_stencil(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y) {
    unsigned char b[4];
    if ((R(FB_DEPTH_FORMAT) & 3) != 3 || !depth_read(p, m, x, y, b)) return 0;
    return b[3];
}
static void set_stencil(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y, uint8_t s) {
    unsigned char b[4] = {0, 0, 0, s};
    if ((R(FB_DEPTH_FORMAT) & 3) == 3) depth_write(p, m, x, y, b, 3, 1);
}

static uint8_t stencil_action(unsigned action, uint8_t old, uint8_t ref) {
    switch (action & 7) {
        case 0: return old;
        case 1: return 0;
        case 2: return ref;
        case 3: return (uint8_t)((old < 254 ? old : 254) + 1);
        case 4: return (uint8_t)((old > 1 ? old : 1) - 1);
        case 5: return (uint8_t)~old;
        case 6: return (uint8_t)(old + 1);
        default: return (uint8_t)(old - 1);
    }
}

static bool depth_stencil(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y, float depth) {
    uint32_t st = R(OM_STENCIL), op = R(OM_STENCIL_OP), om = R(OM_DEPTH_COLOR);
    bool write = (R(FB_DEPTH_WRITE) & 3) != 0;
    bool stencil = (st & 1) && (R(FB_DEPTH_FORMAT) & 3) == 3;
    uint8_t old = 0, ref = (uint8_t)(st >> 16), write_mask = (uint8_t)(st >> 8), input_mask = (uint8_t)(st >> 24);
#define UPDATE_STENCIL(action)                                                                                         \
    do {                                                                                                               \
        uint8_t n = stencil_action((action), old, ref);                                                                \
        if (write) set_stencil(p, m, x, y, (uint8_t)((n & write_mask) | (old & ~write_mask)));                         \
    } while (0)
    if (stencil) {
        old = get_stencil(p, m, x, y);
        if (!compare(st >> 4, ref & input_mask, old & input_mask)) {
            UPDATE_STENCIL(op);
            return false;
        }
    }
    unsigned bits = (R(FB_DEPTH_FORMAT) & 3) == 0 ? 16 : 24;
    uint32_t z = (uint32_t)(depth * (float)((1u << bits) - 1));
    if (om & 1) {
        if (!compare(om >> 4, z, get_depth(p, m, x, y))) {
            if (stencil) UPDATE_STENCIL(op >> 4);
            return false;
        }
    }
    if (write && ((om >> 12) & 1)) set_depth(p, m, x, y, z);
    /* the depth-pass action runs even with the depth test off */
    if (stencil) UPDATE_STENCIL(op >> 8);
#undef UPDATE_STENCIL
    return true;
}

static uint8_t logic_op(uint8_t s, uint8_t d, unsigned op) {
    switch (op & 15) {
        case 0: return 0;
        case 1: return s & d;
        case 2: return (uint8_t)(s & ~d);
        case 3: return s;
        case 4: return 255;
        case 5: return (uint8_t)~s;
        case 6: return d;
        case 7: return (uint8_t)~d;
        case 8: return (uint8_t)~(s & d);
        case 9: return s | d;
        case 10: return (uint8_t)~(s | d);
        case 11: return s ^ d;
        case 12: return (uint8_t)~(s ^ d);
        case 13: return (uint8_t)(~s & d);
        case 14: return (uint8_t)(s | ~d);
        default: return (uint8_t)(~s | d);
    }
}

static uint8_t blend_factor(unsigned channel, unsigned factor, const uint8_t* src, const uint8_t* dst,
                            const uint8_t* constant) {
    switch (factor) {
        case 0: return 0;
        case 1: return 255;
        case 2: return src[channel];
        case 3: return (uint8_t)(255 - src[channel]);
        case 4: return dst[channel];
        case 5: return (uint8_t)(255 - dst[channel]);
        case 6: return src[3];
        case 7: return (uint8_t)(255 - src[3]);
        case 8: return dst[3];
        case 9: return (uint8_t)(255 - dst[3]);
        case 10: return constant[channel];
        case 11: return (uint8_t)(255 - constant[channel]);
        case 12: return constant[3];
        case 13: return (uint8_t)(255 - constant[3]);
        case 14: /* SourceAlphaSaturate: 1 for alpha */
            if (channel == 3) return 255;
            return src[3] < 255 - dst[3] ? src[3] : (uint8_t)(255 - dst[3]);
        default: return src[channel]; /* unknown factors: UNIMPLEMENTED in Azahar, which then uses the source */
    }
}
static int blend_equation(unsigned eq, int s, int d) {
    switch (eq & 7) {
        case 0: return (s + d) / 255;
        case 1: return (s - d) / 255;
        case 2: return (d - s) / 255;
        case 3: return (s < d ? s : d) / 255;
        case 4: return (s > d ? s : d) / 255;
        default: return 0; /* undefined equations leave the result zeroed */
    }
}

static void pixel_color(const SnxCtrPica* p, SnxCtrMemory* m, unsigned x, unsigned y, const uint8_t src[4],
                        uint8_t result[4]) {
    uint8_t dst[4], out[4];
    uint32_t om = R(OM_DEPTH_COLOR);
    (void)get_pixel(p, m, x, y, dst);
    if ((R(OM_MODE) >> 8) & 1) {
        uint32_t params = R(OM_BLEND);
        uint8_t constant[4], sf[4], df[4];
        for (unsigned k = 0; k < 4; ++k) constant[k] = (uint8_t)(R(OM_BLEND_CONST) >> (k * 8));
        for (unsigned k = 0; k < 4; ++k) {
            sf[k] = blend_factor(k, k < 3 ? (params >> 16) & 15 : (params >> 24) & 15, src, dst, constant);
            df[k] = blend_factor(k, k < 3 ? (params >> 20) & 15 : (params >> 28) & 15, src, dst, constant);
        }
        for (unsigned k = 0; k < 4; ++k) {
            unsigned eq = k < 3 ? params & 7 : (params >> 8) & 7;
            out[k] = (uint8_t)snx_clamp_int(blend_equation(eq, src[k] * sf[k], dst[k] * df[k]), 0, 255);
        }
    } else
        for (unsigned k = 0; k < 4; ++k) out[k] = logic_op(src[k], dst[k], R(OM_LOGIC));
    for (unsigned k = 0; k < 4; ++k) result[k] = (om >> (8 + k)) & 1 ? out[k] : dst[k];
}

/* ProcessTriangle: counter-clockwise after culling, rasterized at pixel centres in 12.4 fixed point */
static void process(const SnxCtrPica* p, SnxCtrMemory* m, const Vertex* v0, const Vertex* v1, const Vertex* v2,
                    bool reversed) {
    const Vertex* v[3] = {v0, v1, v2};
    uint16_t pos[3][3];
    for (unsigned i = 0; i < 3; ++i)
        for (unsigned k = 0; k < 3; ++k) pos[i][k] = fix12p4(v[i]->screen[k]);
    unsigned cull = R(CULL_MODE) & 3;
    if (cull == 0 || cull == 3) {
        if (!reversed && signed_area(pos[0], pos[1], pos[2]) <= 0) {
            process(p, m, v0, v2, v1, true);
            return;
        }
    } else {
        if (!reversed && cull == 1) { /* KeepClockWise: reverse, then the counter-clockwise path */
            process(p, m, v0, v2, v1, true);
            return;
        }
        if (signed_area(pos[0], pos[1], pos[2]) <= 0) return;
    }
    uint16_t min_x = pos[0][0], min_y = pos[0][1], max_x = pos[0][0], max_y = pos[0][1];
    for (unsigned i = 1; i < 3; ++i) {
        if (pos[i][0] < min_x) min_x = pos[i][0];
        if (pos[i][1] < min_y) min_y = pos[i][1];
        if (pos[i][0] > max_x) max_x = pos[i][0];
        if (pos[i][1] > max_y) max_y = pos[i][1];
    }
    unsigned scissor = R(SCISSOR_MODE) & 3;
    uint16_t sx1 = (uint16_t)((R(SCISSOR_1) & 0x3ff) << 4), sy1 = (uint16_t)(((R(SCISSOR_1) >> 16) & 0x3ff) << 4);
    /* x2 and y2 cover their whole pixel */
    uint16_t sx2 = (uint16_t)(((R(SCISSOR_2) & 0x3ff) + 1) << 4),
             sy2 = (uint16_t)((((R(SCISSOR_2) >> 16) & 0x3ff) + 1) << 4);
    if (scissor == 3) { /* Include */
        if (sx1 > min_x) min_x = sx1;
        if (sy1 > min_y) min_y = sy1;
        if (sx2 < max_x) max_x = sx2;
        if (sy2 < max_y) max_y = sy2;
    }
    min_x &= 0xfff0, min_y &= 0xfff0;
    max_x = (uint16_t)((max_x + 0xf) & 0xfff0);
    max_y = (uint16_t)((max_y + 0xf) & 0xfff0);
    int bias0 = right_or_flat_bottom(pos[0], pos[1], pos[2]) ? -1 : 0;
    int bias1 = right_or_flat_bottom(pos[1], pos[2], pos[0]) ? -1 : 0;
    int bias2 = right_or_flat_bottom(pos[2], pos[0], pos[1]) ? -1 : 0;
    float w_inverse[3] = {v0->v.pos[3], v1->v.pos[3], v2->v.pos[3]};
    float depth_scale = f24(R(DEPTH_RANGE)), depth_offset = f24(R(DEPTH_NEAR));
    for (uint16_t y = (uint16_t)(min_y + 8); y < max_y; y = (uint16_t)(y + 0x10)) {
        for (uint16_t x = (uint16_t)(min_x + 8); x < max_x; x = (uint16_t)(x + 0x10)) {
            if (scissor == 1 && x >= sx1 && x < sx2 && y >= sy1 && y < sy2) continue; /* Exclude */
            uint16_t at[2] = {x, y};
            int w0 = bias0 + signed_area(pos[1], pos[2], at), w1 = bias1 + signed_area(pos[2], pos[0], at),
                w2 = bias2 + signed_area(pos[0], pos[1], at), wsum = w0 + w1 + w2;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float bary[3] = {(float)w0, (float)w1, (float)w2};
            float w_interp = 1.0f / dot3(w_inverse, bary);
            float z_over_w =
                (v0->screen[2] * (float)w0 + v1->screen[2] * (float)w1 + v2->screen[2] * (float)w2) / (float)wsum;
            /* not fully accurate: about 3 bits of precision are missing */
            float depth = z_over_w * depth_scale + depth_offset;
            if (!(R(DEPTHMAP_ENABLE) & 1)) depth *= w_interp * (float)wsum; /* W-buffering */
            depth = depth < 0.0f ? 0.0f : depth > 1.0f ? 1.0f : depth;
#define INTERP(field) mul(dot3((float[3]){v0->v.field, v1->v.field, v2->v.field}, bary), w_interp)
            uint8_t primary[4];
            for (unsigned k = 0; k < 4; ++k) primary[k] = to_u8(roundf(INTERP(color[k]) * 255.0f));
            float uv[3][2] = {
                {INTERP(tc0[0]), INTERP(tc0[1])}, {INTERP(tc1[0]), INTERP(tc1[1])}, {INTERP(tc2[0]), INTERP(tc2[1])}};
            float tc0_w = INTERP(tc0_w);
            uint8_t textures[4][4], combined[4], fragment[4] = {0, 0, 0, 0}, secondary[4] = {0, 0, 0, 0};
            texture_color(p, m, uv, tc0_w, textures);
            if (!(R(LIGHTING_DISABLE) & 1)) {
                float q[4] = {INTERP(quat[0]), INTERP(quat[1]), INTERP(quat[2]), INTERP(quat[3])};
                float view[3] = {INTERP(view[0]), INTERP(view[1]), INTERP(view[2])};
                float length = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                for (unsigned k = 0; k < 4; ++k) q[k] /= length;
                snx_ctr_lighting(p, q, view, textures, fragment, secondary);
            }
#undef INTERP
            tev(p, textures, primary, fragment, secondary, combined);
            uint32_t at_reg = R(OM_ALPHA_TEST);
            if ((at_reg & 1) && !compare(at_reg >> 4, combined[3], (at_reg >> 8) & 0xff)) continue;
            fog(p, depth, combined);
            if (!depth_stencil(p, m, x >> 4, y >> 4, depth)) continue;
            uint8_t result[4];
            pixel_color(p, m, x >> 4, y >> 4, combined, result);
            if (R(FB_COLOR_WRITE) & 15) draw_pixel(p, m, x >> 4, y >> 4, result);
        }
    }
}

const char* snx_ctr_raster_check(const SnxCtrPica* p) {
    if ((R(TEX_MAIN) >> 10) & 1) return "procedural textures are not ported yet";
    unsigned type0 = (R(tex_config[0] + 2) >> 28) & 7;
    if ((R(TEX_MAIN) & 1) && (type0 == 2 || type0 == 4)) return "shadow textures are not ported yet";
    if ((R(OM_MODE) & 3) == 3) return "shadow rendering is not ported yet";
    return NULL;
}

static const SnxCtrTriangleSink sink = {snx_ctr_raster_triangle, snx_ctr_raster_check};
void snx_ctr_raster_attach(SnxCtrPica* p) { p->sink = &sink; }

void snx_ctr_raster_triangle(const SnxCtrPica* p, SnxCtrMemory* m, const SnxCtrVertex* a, const SnxCtrVertex* b,
                             const SnxCtrVertex* c) {
    Vertex buffers[2][MAX_VERTICES] = {0};
    Vertex* in = buffers[0];
    Vertex* out = buffers[1];
    out[0].v = *a, out[1].v = *b, out[2].v = *c;
    unsigned n = 3;
    /* FlipQuaternionIfOpposite against the first vertex */
    for (unsigned i = 1; i < 3; ++i)
        if (dot4(out[i].v.quat, out[0].v.quat) < 0.0f)
            for (unsigned k = 0; k < 4; ++k) out[i].v.quat[k] = mul(out[i].v.quat[k], -1.0f);
    /* x = +w, x = -w, y = +w, y = -w, z = 0, z = -w, and w = epsilon so that w stays positive */
    static const Edge edges[7] = {
        {{-1, 0, 0, 1}, {0, 0, 0, 0}},       {{1, 0, 0, 1}, {0, 0, 0, 0}},  {{0, -1, 0, 1}, {0, 0, 0, 0}},
        {{0, 1, 0, 1}, {0, 0, 0, 0}},        {{0, 0, -1, 0}, {0, 0, 0, 0}}, {{0, 0, 1, 1}, {0, 0, 0, 0}},
        {{0, 0, 0, 1}, {0, 0, 0, 0.00001f}},
    };
    for (unsigned e = 0; e < 8; ++e) {
        Edge custom;
        const Edge* edge = &edges[e < 7 ? e : 0];
        if (e == 7) {
            if (!(R(CLIP_ENABLE) & 1)) break;
            for (unsigned k = 0; k < 4; ++k) custom.coeffs[k] = f24(R(CLIP_COEF + k)), custom.bias[k] = 0;
            edge = &custom;
        }
        Vertex* t = in;
        in = out;
        out = t;
        n = clip(edge, in, n, out);
        if (n < 3) return;
    }
    make_screen(p, &out[0]);
    make_screen(p, &out[1]);
    for (unsigned i = 0; i + 2 < n; ++i) {
        make_screen(p, &out[i + 2]);
        process(p, m, &out[0], &out[i + 1], &out[i + 2], false);
    }
}
