/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's draws on the GPU (src/3ds/gpu/vk/render.c, with the shaders vsgen.c and fsgen.c generate) against
   the software renderer they replaced (draw.c, raster.c, lighting.c, texture.c, ported from Azahar's), which stays as
   their CPU reference: each scene's command list runs twice from the same guest memory, once through each, and the
   colour and depth buffers they leave must agree. The two rasterize differently on edges (the PICA's 12.4 fixed point
   against the host's rules) and round interpolated values differently, so a pixel may differ by a few steps, a depth
   by a thousandth of its range, and a few pixels on edges by more. */
#include "3ds/gpu/pica.h"
#include "3ds/gpu/present.h"
#include "3ds/gpu/texture.h"
#include "3ds/gpu/transfer.h"
#include "3ds/gpu/vk/vk.h"
#include "3ds/gpu/vk/vk_render.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FCRAM 0x20000000u
#define VRAM 0x18000000u
#define LIST (FCRAM + 0x100000u)
#define VERTICES (FCRAM + 0x110000u)
#define INDICES (FCRAM + 0x118000u)
#define TEXTURE (FCRAM + 0x120000u)
#define COLOR VRAM
#define DEPTH (VRAM + 0x100000u)
#define W 64
#define H 64

static int failures;
static uint32_t seed = 0x3d5u;
static uint32_t next(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}

typedef struct {
    uint32_t w[4096];
    unsigned n;
} List;
static void put(List* l, uint32_t id, uint32_t value) {
    l->w[l->n++] = value;
    l->w[l->n++] = id | 15u << 16;
}
static uint32_t op1(unsigned op, unsigned dest, unsigned src1, unsigned src2, unsigned desc) {
    return (uint32_t)op << 26 | dest << 21 | src1 << 12 | src2 << 7 | desc;
}
#define XYZW 0x6c36fu
#define END (0x22u << 26)
/* float24 of a float with an exact float24 (no rounding here) */
static uint32_t f24(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    if (!(b & 0x7fffffffu)) return (b >> 31) << 23;
    uint32_t sign = b >> 31, exponent = (b >> 23 & 0xff) - 127 + 63, mantissa = (b >> 7) & 0xffff;
    return sign << 23 | exponent << 16 | mantissa;
}

/* a vertex: position, colour, texture coordinate 0 */
typedef struct {
    float pos[4], color[4], tc[2];
} Vertex;

/* what every scene draws with: the program (o0 = v0 * c0, o1 = v1, o2 = v2), the outputs' semantics, the viewport,
   TEV stages 1-5 passing the previous stage on, the colour target, a loader of three float attributes */
static void base(List* l, unsigned color_format) {
    const uint32_t code[5] = {op1(0x08, 0x10, 0x20, 0x00, 0), /* mul r0, c0, v0 */
                              op1(0x13, 0x00, 0x10, 0x00, 0), /* mov o0, r0 */
                              op1(0x13, 0x01, 0x01, 0x00, 0), /* mov o1, v1 */
                              op1(0x13, 0x02, 0x02, 0x00, 0), /* mov o2, v2 */
                              END};
    put(l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    for (unsigned i = 0; i < 5; ++i) put(l, SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD, code[i]);
    put(l, SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_OFFSET, 0);
    put(l, SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_WORD, XYZW);
    /* c0 = (1, 1, 1, 1) */
    put(l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_SETUP, 0);
    put(l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, f24(1.0f) << 8 | f24(1.0f) >> 16);
    put(l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, (f24(1.0f) & 0xffff) << 16 | f24(1.0f) >> 8);
    put(l, SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE, (f24(1.0f) & 0xff) << 24 | f24(1.0f));
    put(l, SNX_PICA_VS + SNX_PICA_SH_INPUT_CONFIG, 2 | 0xa0u << 24);
    put(l, SNX_PICA_VS + SNX_PICA_SH_ATTR_MAP_LOW, 0x210);
    put(l, SNX_PICA_VS + SNX_PICA_SH_OUTPUT_MASK, 7);
    put(l, SNX_PICA_MAX_INPUT_ATTR, 2);
    put(l, 0x04f, 3);
    put(l, 0x050, 0x03020100u);
    put(l, 0x051, 0x0b0a0908u);
    put(l, 0x052, 0x1f1f0d0cu);
    put(l, 0x041, f24(W / 2.0f));
    put(l, 0x043, f24(H / 2.0f));
    put(l, 0x04d, f24(-1.0f)); /* depth = -z/w */
    put(l, 0x04e, 0);
    put(l, 0x1c6, 1); /* lighting off */
    for (unsigned s = 1; s < 6; ++s) put(l, s < 4 ? 0x0c0 + s * 8 : 0x0f0 + (s - 4) * 8, 0x000f000fu);
    put(l, 0x11d, COLOR / 8);
    put(l, 0x117, color_format << 16);
    put(l, 0x11e, W | (H - 1) << 12 | 1u << 24); /* rows from the top, as games have them and the reference does */
    put(l, 0x113, 15);
    put(l, 0x107, 0xf00);
    put(l, 0x102, 3);
    put(l, 0x200, VERTICES >> 3);
    put(l, 0x201, 0x7ff);
    put(l, 0x202, 2u << 28);
    put(l, 0x203, 0);
    put(l, 0x204, 0x210);
    put(l, 0x205, 3u << 28 | (uint32_t)sizeof(Vertex) << 16);
    put(l, SNX_PICA_TOPOLOGY, 0);
}
static void depth_target(List* l, unsigned format) {
    put(l, 0x11c, DEPTH / 8);
    put(l, 0x116, format);
    put(l, 0x115, 3);
}
static Vertex vertex(float x, float y, float z, uint32_t rgba, float s, float t) {
    Vertex v = {{x, y, z, 1.0f},
                {(rgba >> 24) / 255.0f, (rgba >> 16 & 0xff) / 255.0f, (rgba >> 8 & 0xff) / 255.0f,
                 (rgba & 0xff) / 255.0f},
                {s, t}};
    return v;
}

typedef struct {
    const char* name;
    unsigned color_format, depth_format; /* PICA's; depth 0xff none */
    List list;
    Vertex vertices[64];
    unsigned vertex_count;
    unsigned char texture[64 * 64 * 4];
    unsigned texture_bytes;
    uint8_t indices[256];
    unsigned index_bytes;
} Scene;

/* the scene's memory as it starts: its data, a random colour target, a depth target at 0.5 with random stencil */
static void setup(SnxCtrMemory* m, const Scene* s, const unsigned char* color, const unsigned char* depth) {
    snx_ctr_phys_write(m, VERTICES, s->vertices, sizeof s->vertices);
    snx_ctr_phys_write(m, TEXTURE, s->texture, sizeof s->texture);
    snx_ctr_phys_write(m, INDICES, s->indices, sizeof s->indices);
    snx_ctr_phys_write(m, COLOR, color, W * H * 4);
    snx_ctr_phys_write(m, DEPTH, depth, W * H * 4);
    snx_ctr_phys_write(m, LIST, s->list.w, s->list.n * 4);
}
static bool draw(SnxCtrMemory* m, const Scene* s, const SnxCtrPicaHost* host, unsigned char* color,
                 unsigned char* depth) {
    SnxCtrPica* p = calloc(1, sizeof *p);
    snx_ctr_pica_init(p);
    SnxCtrPicaResult r = snx_ctr_pica_run(p, m, LIST, s->list.n * 4, host);
    if (r != SNX_CTR_PICA_OK)
        fprintf(stderr, "%s: the list stopped at 0x%03x: %s\n", s->name, p->stop_register,
                p->stop_reason ? p->stop_reason : "?");
    snx_ctr_pica_free(p);
    free(p);
    bool ok = r == SNX_CTR_PICA_OK && snx_ctr_vk_finish();
    snx_ctr_phys_read(m, COLOR, color, W * H * 4);
    snx_ctr_phys_read(m, DEPTH, depth, W * H * 4);
    return ok;
}

static void compare(const Scene* s, const unsigned char* sw, const unsigned char* gpu, const unsigned char* sw_depth,
                    const unsigned char* gpu_depth) {
    static const unsigned formats[5] = {SNX_CTR_PIX_RGBA8, SNX_CTR_PIX_RGB8, SNX_CTR_PIX_RGB5A1, SNX_CTR_PIX_RGB565,
                                        SNX_CTR_PIX_RGBA4};
    unsigned format = formats[s->color_format], bpp = snx_ctr_pixel_bytes(format);
    /* a channel's step in the format: one 8-bit step, or one of 4, 5 or 6 bits */
    unsigned tolerance = s->color_format == 4 ? 17 : s->color_format >= 2 ? 9 : 3;
    unsigned off = 0, worst = 0, depth_off = 0;
    for (unsigned i = 0; i < W * H; ++i) {
        uint8_t a[4], b[4];
        snx_ctr_pixel_decode(format, sw + i * bpp, a);
        snx_ctr_pixel_decode(format, gpu + i * bpp, b);
        unsigned d = 0;
        for (unsigned c = 0; c < 4; ++c) {
            unsigned e = a[c] > b[c] ? a[c] - b[c] : b[c] - a[c];
            if (e > d) d = e;
        }
        if (d > worst) worst = d;
        off += d > tolerance;
    }
    if (s->depth_format != 0xff) {
        unsigned dbpp = s->depth_format == 0 ? 2 : s->depth_format == 2 ? 3 : 4;
        for (unsigned i = 0; i < W * H; ++i) {
            uint32_t a = 0, b = 0;
            for (unsigned k = 0; k < (dbpp == 4 ? 3u : dbpp); ++k)
                a |= (uint32_t)sw_depth[i * dbpp + k] << (8 * k), b |= (uint32_t)gpu_depth[i * dbpp + k] << (8 * k);
            uint32_t e = a > b ? a - b : b - a;
            bool stencil = dbpp == 4 && sw_depth[i * 4 + 3] != gpu_depth[i * 4 + 3];
            /* the reference snaps positions to 12.4 fixed point, the host to its own subpixel grid: depths differ by
               their slope times a fraction of a pixel */
            bool wrong = e > (dbpp == 2 ? 64u : 16384u) || stencil;
            if (wrong && depth_off < 4)
                printf("    depth at (%u, %u): the reference's 0x%06x stencil %u, the GPU's 0x%06x stencil %u\n", i % W,
                       i / W, a, dbpp == 4 ? sw_depth[i * 4 + 3] : 0, b, dbpp == 4 ? gpu_depth[i * 4 + 3] : 0);
            depth_off += wrong;
        }
    }
    /* edges: a pixel's worth of the target's outline and the triangles' */
    bool ok = off <= W * H / 25 && depth_off <= W * H / 25;
    printf("%-24s %4u pixels off by more than %u (largest %u), %4u depth or stencil values off: %s\n", s->name, off,
           tolerance, worst, depth_off, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

static void run(SnxCtrMemory* m, Scene* s) {
    static unsigned char color[W * H * 4], depth[W * H * 4];
    static unsigned char sw[W * H * 4], sw_depth[W * H * 4], gpu[W * H * 4], gpu_depth[W * H * 4];
    for (unsigned i = 0; i < W * H * 4; ++i) color[i] = (unsigned char)next();
    for (unsigned i = 0; i < W * H; ++i) { /* depth 0.5 (D16 and D24 alike), stencil random */
        if (s->depth_format == 0)
            depth[i * 2] = 0xff, depth[i * 2 + 1] = 0x7f;
        else if (s->depth_format == 2)
            depth[i * 3] = 0xff, depth[i * 3 + 1] = 0xff, depth[i * 3 + 2] = 0x7f;
        else
            depth[i * 4] = 0xff, depth[i * 4 + 1] = 0xff, depth[i * 4 + 2] = 0x7f,
            depth[i * 4 + 3] = (unsigned char)(next() & 3);
    }
    put(&s->list, SNX_PICA_IRQ_REQUEST, 0x12345678u); /* the end: autostop */
    SnxCtrPicaHost software = {NULL, NULL, snx_ctr_pica_draw, snx_ctr_pica_draw_immediate, NULL};
    SnxCtrPicaHost vulkan = {NULL, NULL, snx_ctr_vk_draw, snx_ctr_vk_vertex, snx_ctr_vk_vertices_end};
    setup(m, s, color, depth);
    bool a = draw(m, s, &software, sw, sw_depth);
    setup(m, s, color, depth);
    bool b = draw(m, s, &vulkan, gpu, gpu_depth);
    if (!a || !b) {
        printf("%-24s did not draw on the %s\n", s->name, a ? "GPU" : "CPU");
        ++failures;
        return;
    }
    compare(s, sw, gpu, sw_depth, gpu_depth);
}

static void triangles(Scene* s, unsigned count) {
    for (unsigned t = 0; t < count; ++t)
        for (unsigned k = 0; k < 3; ++k) {
            float x = (float)(next() % 2001) / 1000.0f - 1.0f, y = (float)(next() % 2001) / 1000.0f - 1.0f;
            float z = -(float)(next() % 900 + 50) / 1000.0f;
            s->vertices[s->vertex_count++] = vertex(x, y, z, next() << 8 | (next() & 0xff),
                                                    (float)(next() % 1000) / 1000.0f, (float)(next() % 1000) / 1000.0f);
        }
}
static Scene* scene(const char* name, unsigned color_format, unsigned depth_format) {
    Scene* s = calloc(1, sizeof *s);
    s->name = name;
    s->color_format = color_format;
    s->depth_format = depth_format;
    base(&s->list, color_format);
    if (depth_format != 0xff) depth_target(&s->list, depth_format);
    for (unsigned i = 0; i < sizeof s->texture; ++i) s->texture[i] = (unsigned char)next();
    return s;
}
static void array_draw(Scene* s) {
    put(&s->list, 0x228, s->vertex_count);
    put(&s->list, SNX_PICA_TRIGGER_DRAW, 1);
}

/* The presenter clears the window's image and blits the decoded screens over it, and the blits' writes must wait for
   the clear's: synchronization validation checks that when the run has a window. A bottom screen presented alone
   shows at its second present. Both presents must hash as the CPU decode of the framebuffer. */
#define PRESENT_FB         (VRAM + 0x200000u)
/* what present.c starts a frame's hash from: the decimal digits of SNX_FNV_BASIS without the last one */
#define PRESENT_HASH_BASIS 1469598103934665603ull
static void present_screens(SnxCtrMemory* m) {
    if (!snx_ctr_present_init(m, NULL)) {
        ++failures;
        return;
    }
    /* four uniform RGBA8 pixels in the guest's A,B,G,R byte order: any pixel order decodes to the same bytes */
    static const unsigned char abgr[16] = {0x40, 0x30, 0x20, 0x10, 0x40, 0x30, 0x20, 0x10,
                                           0x40, 0x30, 0x20, 0x10, 0x40, 0x30, 0x20, 0x10};
    static const unsigned char expect[16] = {0x10, 0x20, 0x30, 0x40, 0x10, 0x20, 0x30, 0x40,
                                             0x10, 0x20, 0x30, 0x40, 0x10, 0x20, 0x30, 0x40};
    uint64_t want = snx_fnv1a(PRESENT_HASH_BASIS, expect, sizeof expect);
    uint64_t first = 0, second = 0;
    bool ok = snx_ctr_phys_write(m, PRESENT_FB, abgr, sizeof abgr) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_present_frame(1, PRESENT_FB, 2, 2, 0, 2 * 4, &first);
    ok = ok && snx_ctr_present_frame(1, PRESENT_FB, 2, 2, 0, 2 * 4, &second);
    bool pass = ok && first == want && second == want;
    printf("%-24s hash %016llx and %016llx, CPU %016llx: %s\n", "screens presented", (unsigned long long)first,
           (unsigned long long)second, (unsigned long long)want, pass ? "ok" : "FAIL");
    if (!pass) ++failures;
}

static void all_scenes(SnxCtrMemory* m) {
    static const char* const color_names[5] = {"colour RGBA8", "colour RGB8", "colour RGB5A1", "colour RGB565",
                                               "colour RGBA4"};
    for (unsigned f = 0; f < 5; ++f) { /* vertex colours into each colour format */
        Scene* s = scene(color_names[f], f, 0xff);
        triangles(s, 6);
        array_draw(s);
        run(m, s);
        free(s);
    }
    static const char* const texture_names[14] = {"texture RGBA8", "texture RGB8", "texture RGB5A1", "texture RGB565",
                                                  "texture RGBA4", "texture IA8",  "texture RG8",    "texture I8",
                                                  "texture A8",    "texture IA4",  "texture I4",     "texture A4",
                                                  "texture ETC1",  "texture ETC1A4"};
    for (unsigned f = 0; f < 14; ++f) { /* each texture format, 32x32, nearest, repeated */
        Scene* s = scene(texture_names[f], 0, 0xff);
        put(&s->list, 0x080, 1);
        put(&s->list, 0x082, 32 | 32u << 16);
        put(&s->list, 0x083, 2u << 8 | 2u << 12); /* repeat in s and t, nearest */
        put(&s->list, 0x085, TEXTURE / 8);
        put(&s->list, 0x08e, f);
        put(&s->list, 0x0c0, 0x00030003u); /* colour and alpha from texture 0 */
        s->vertices[0] = vertex(-1, -1, -0.5f, 0xffffffffu, 0, 0);
        s->vertices[1] = vertex(1, -1, -0.5f, 0xffffffffu, 1, 0);
        s->vertices[2] = vertex(-1, 1, -0.5f, 0xffffffffu, 0, 1);
        s->vertices[3] = vertex(1, 1, -0.5f, 0xffffffffu, 1, 1);
        s->vertex_count = 4;
        put(&s->list, SNX_PICA_TOPOLOGY, 1u << 8); /* a strip */
        array_draw(s);
        run(m, s);
        free(s);
    }
    /* the reference samples the nearest texel and truncates coordinates towards zero (as Azahar's software renderer
       does), where the hardware filters and the GPU floors: only nearest sampling from coordinate 0 up compares */
    static const struct {
        const char* name;
        uint32_t params;
        float low, high;
    } samplings[3] = {{"texture mirrored", 3u << 8 | 3u << 12, 0.0f, 1.5f},
                      {"texture clamped", 0, 0.0f, 1.5f},
                      {"texture repeated", 2u << 8 | 2u << 12, 0.0f, 1.5f}};
    for (unsigned k = 0; k < 3; ++k) {
        Scene* s = scene(samplings[k].name, 0, 0xff);
        float lo = samplings[k].low, hi = samplings[k].high, far = lo + 2 * (hi - lo);
        put(&s->list, 0x080, 1);
        put(&s->list, 0x082, 16 | 16u << 16);
        put(&s->list, 0x083, samplings[k].params);
        put(&s->list, 0x085, TEXTURE / 8);
        put(&s->list, 0x08e, 0);
        put(&s->list, 0x0c0, 0x00030003u);
        s->vertices[0] = vertex(-1, -1, -0.5f, 0xffffffffu, lo, lo);
        s->vertices[1] = vertex(3, -1, -0.5f, 0xffffffffu, far, lo);
        s->vertices[2] = vertex(-1, 3, -0.5f, 0xffffffffu, lo, far);
        s->vertex_count = 3;
        array_draw(s);
        run(m, s);
        free(s);
    }
    {
        Scene* s = scene("blend source alpha", 0, 0xff);
        put(&s->list, 0x100, 1u << 8);
        put(&s->list, 0x101, 0u | 0u << 8 | 6u << 16 | 7u << 20 | 6u << 24 | 7u << 28);
        triangles(s, 6);
        array_draw(s);
        run(m, s);
        free(s);
    }
    {
        Scene* s = scene("logic op xor", 0, 0xff);
        put(&s->list, 0x102, 11);
        triangles(s, 4);
        array_draw(s);
        run(m, s);
        free(s);
    }
    static const char* const depth_names[3] = {"depth D16 less", "depth D24 less", "depth D24S8 less"};
    static const unsigned depth_formats[3] = {0, 2, 3};
    for (unsigned d = 0; d < 3; ++d) {
        Scene* s = scene(depth_names[d], 0, depth_formats[d]);
        put(&s->list, 0x107, 0xf00 | 1u | 4u << 4 | 1u << 12); /* test less, write */
        triangles(s, 8);
        array_draw(s);
        run(m, s);
        free(s);
    }
    {
        Scene* s = scene("stencil equal, replace", 0, 3);
        put(&s->list, 0x107, 0xf00 | 1u | 1u << 4 | 1u << 12);
        put(&s->list, 0x105, 1u | 2u << 4 | 0xffu << 8 | 2u << 16 | 0xffu << 24); /* equal to 2 */
        put(&s->list, 0x106, 1u | 0u << 4 | 3u << 8);                            /* fail zero, pass increment */
        triangles(s, 6);
        array_draw(s);
        run(m, s);
        free(s);
    }
    {
        Scene* s = scene("culling clockwise kept", 0, 0xff);
        put(&s->list, 0x040, 1);
        triangles(s, 10);
        array_draw(s);
        run(m, s);
        free(s);
        s = scene("culling counter-clockwise kept", 0, 0xff);
        put(&s->list, 0x040, 2);
        triangles(s, 10);
        array_draw(s);
        run(m, s);
        free(s);
    }
    {
        Scene* s = scene("alpha test, scissor", 0, 0xff);
        put(&s->list, 0x104, 1u | 6u << 4 | 0x80u << 8); /* greater than 0x80 */
        put(&s->list, 0x065, 3);                         /* keep the inside */
        put(&s->list, 0x066, 8 | 12u << 16);
        put(&s->list, 0x067, 50 | 40u << 16);
        triangles(s, 6);
        array_draw(s);
        run(m, s);
        free(s);
    }
    {
        Scene* s = scene("indexed 16-bit", 0, 0xff);
        triangles(s, 8);
        uint16_t* idx = (uint16_t*)s->indices;
        for (unsigned i = 0; i < 30; ++i) idx[i] = (uint16_t)(next() % s->vertex_count);
        put(&s->list, 0x227, (INDICES - VERTICES) | 1u << 31);
        put(&s->list, 0x228, 30);
        put(&s->list, SNX_PICA_TRIGGER_DRAW_INDEXED, 1);
        run(m, s);
        free(s);
        if (snx_ctr_vk()->index8) {
            s = scene("indexed 8-bit fan", 0, 0xff);
            triangles(s, 3);
            for (unsigned i = 0; i < 9; ++i) s->indices[i] = (uint8_t)i;
            put(&s->list, SNX_PICA_TOPOLOGY, 2u << 8);
            put(&s->list, 0x227, INDICES - VERTICES);
            put(&s->list, 0x228, 9);
            put(&s->list, SNX_PICA_TRIGGER_DRAW_INDEXED, 1);
            run(m, s);
            free(s);
        }
    }
    {
        Scene* s = scene("immediate mode", 0, 0xff);
        put(&s->list, SNX_PICA_DEFAULT_ATTR_INDEX, 15);
        for (unsigned t = 0; t < 9; ++t) {
            float v[3][4] = {{(float)(next() % 2001) / 1000.0f - 1.0f, (float)(next() % 2001) / 1000.0f - 1.0f,
                              -0.5f, 1.0f},
                             {(float)(next() % 256) / 255.0f, (float)(next() % 256) / 255.0f,
                              (float)(next() % 256) / 255.0f, 1.0f},
                             {0, 0, 0, 1}};
            for (unsigned a = 0; a < 3; ++a) { /* float24 w, z, y, x packed big-end first */
                uint32_t x = f24(v[a][0]), y = f24(v[a][1]), z = f24(v[a][2]), w = f24(v[a][3]);
                put(&s->list, SNX_PICA_DEFAULT_ATTR_VALUE, w << 8 | z >> 16);
                put(&s->list, SNX_PICA_DEFAULT_ATTR_VALUE, (z & 0xffff) << 16 | y >> 8);
                put(&s->list, SNX_PICA_DEFAULT_ATTR_VALUE, (y & 0xff) << 24 | x);
            }
        }
        run(m, s);
        free(s);
    }
    {
        Scene* s = scene("lighting, one light", 0, 0xff);
        put(&s->list, 0x140 + 2, 1023u << 20 | 512u << 10 | 1023u);
        put(&s->list, 0x140 + 3, 100u << 20 | 50u << 10 | 20u);
        put(&s->list, 0x140 + 4, 0x3c00);
        put(&s->list, 0x140 + 9, 1);
        put(&s->list, 0x1c4, 0xff7bff00u);
        put(&s->list, 0x1c6, 0);
        put(&s->list, 0x0c0, 1);
        triangles(s, 4);
        array_draw(s);
        run(m, s);
        free(s);
    }
}

typedef struct {
    const char* flag;
    const char* what;
    void (*run)(SnxCtrMemory* memory);
} Mode;
static const Mode scenes = {NULL, "the scenes", all_scenes};
/* the validation runs record only the commands under test */
static const Mode modes[] = {{"--present", "the presented screens", present_screens}};

int main(int argc, char** argv) {
    const Mode* mode = &scenes;
    if (argc > 1) {
        mode = NULL;
        if (argc == 2)
            for (size_t i = 0; i < sizeof modes / sizeof *modes; ++i)
                if (!strcmp(argv[1], modes[i].flag)) mode = &modes[i];
        if (!mode) {
            fprintf(stderr, "usage: %s [", argv[0]);
            for (size_t i = 0; i < sizeof modes / sizeof *modes; ++i)
                fprintf(stderr, i ? " | %s" : "%s", modes[i].flag);
            fputs("]\n", stderr);
            return 2;
        }
    }
    SnxCtrMemory* m = snx_ctr_memory_new(SNX_CTR_OLD);
    char why[256];
    if (!m) return 1;
    if (!snx_ctr_vk_init(m, why, sizeof why)) {
        printf("no Vulkan device (%s): GPU draw checks skipped\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    mode->run(m);
    snx_ctr_present_free();
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    printf("3ds vk draw: %s %s the CPU reference\n", mode->what, failures ? "differ from" : "agree with");
    return failures != 0;
}
