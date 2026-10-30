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
#include "3ds/gpu/vk/vk_transfer.h"
#include "3ds/gpu/vk/vk_y2r.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
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
#define AFTER_IN     (FCRAM + 0x200000u)
#define AFTER_OUT    (VRAM + 0x400000u)
#define AFTER_SPAN   0x20000u
#define AFTER_FILL   0x1000u
#define SECOND_COLOR (VRAM + 0x300000u)
#define STORE_AT     (COLOR + 0x1000)
#define RELOAD_AT    (COLOR + (H - 2) * W * 4) /* the tiled guest buffer's last two 8x8 tiles, a corner of the target */
#define RELOAD_SPAN  (2 * W * 4)
#define POISON       0xa5
/* PICA registers of texture unit 0 and the first TEV stage, the colour buffer, and the vertex count and offset */
#define TEX_MAIN     0x080
#define TEX0_SIZE    0x082
#define TEX0_PARAMS  0x083
#define TEX0_ADDRESS 0x085
#define TEX0_FORMAT  0x08e
#define TEV_STAGE0   0x0c0
#define FB_COLOR_ADDR 0x11d
#define NUM_VERTICES  0x228
#define VERTEX_OFFSET 0x22a

static int failures;
static uint32_t seed = 0x3d5u;
static uint32_t next(void) {
    seed = seed * 1664525u + 1013904223u;
    return seed >> 8;
}
static void random_fill(unsigned char* bytes, size_t size) {
    for (size_t i = 0; i < size; ++i) bytes[i] = (unsigned char)next();
}

typedef struct {
    uint32_t w[4096];
    unsigned n;
} List;
static void put(List* l, uint32_t id, uint32_t value) {
    l->w[l->n++] = value;
    l->w[l->n++] = id | 15u << 16;
}
/* the end of a list: the interrupt request stops it */
static void list_end(List* l) { put(l, SNX_PICA_IRQ_REQUEST, 0x12345678u); }
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
static const SnxCtrPicaHost software = {NULL, NULL, snx_ctr_pica_draw, snx_ctr_pica_draw_immediate, NULL};
static const SnxCtrPicaHost vulkan = {NULL, NULL, snx_ctr_vk_draw, snx_ctr_vk_vertex, snx_ctr_vk_vertices_end};
/* setup with a zeroed colour and depth target */
static void setup_clear(SnxCtrMemory* m, const Scene* s) {
    static const unsigned char clear[W * H * 4];
    setup(m, s, clear, clear);
}
/* a scene that did not draw on one of the renderers has nothing to compare */
static bool drew(const Scene* s, bool cpu, bool gpu) {
    if (!cpu || !gpu) {
        printf("%-24s did not draw on the %s\n", s->name, cpu ? "GPU" : "CPU");
        ++failures;
    }
    return cpu && gpu;
}
/* the scene's list through host, its commands recorded and not submitted */
static bool record(SnxCtrMemory* m, const Scene* s, const SnxCtrPicaHost* host) {
    SnxCtrPica* p = calloc(1, sizeof *p);
    snx_ctr_pica_init(p);
    SnxCtrPicaResult r = snx_ctr_pica_run(p, m, LIST, s->list.n * 4, host);
    if (r != SNX_CTR_PICA_OK)
        fprintf(stderr, "%s: the list stopped at 0x%03x: %s\n", s->name, p->stop_register,
                p->stop_reason ? p->stop_reason : "?");
    snx_ctr_pica_free(p);
    free(p);
    return r == SNX_CTR_PICA_OK;
}
static bool draw(SnxCtrMemory* m, const Scene* s, const SnxCtrPicaHost* host, unsigned char* color,
                 unsigned char* depth) {
    bool ok = record(m, s, host) && snx_ctr_vk_finish();
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
    list_end(&s->list);
    setup(m, s, color, depth);
    bool a = draw(m, s, &software, sw, sw_depth);
    setup(m, s, color, depth);
    bool b = draw(m, s, &vulkan, gpu, gpu_depth);
    if (drew(s, a, b)) compare(s, sw, gpu, sw_depth, gpu_depth);
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

/* one check: the bytes that the GPU left against the CPU reference's, after every step before them worked */
static void check_bytes(const char* name, bool ok, const unsigned char* actual, const unsigned char* expected,
                        size_t size) {
    unsigned off = 0;
    for (size_t i = 0; i < size; ++i) off += actual[i] != expected[i];
    printf("%-24s %4u bytes differ from the CPU reference: %s\n", name, off, ok && !off ? "ok" : "FAIL");
    if (!ok || off) ++failures;
}
/* random triangles whose draw is the whole list */
static Scene* triangle_scene(const char* name, unsigned count) {
    Scene* s = scene(name, 0, 0xff);
    triangles(s, count);
    array_draw(s);
    list_end(&s->list);
    return s;
}

/* a target's store under another target's open rendering must follow that rendering's end */
static void store_drawn_target(SnxCtrMemory* m) {
    static unsigned char expected[AFTER_FILL], actual[AFTER_FILL];
    Scene* first = triangle_scene("a target stored under another's", 4);
    Scene* second = scene(first->name, 0, 0xff);
    memcpy(second->vertices, first->vertices, sizeof first->vertices);
    second->vertex_count = first->vertex_count;
    put(&second->list, FB_COLOR_ADDR, SECOND_COLOR / 8);
    array_draw(second);
    list_end(&second->list);
    setup_clear(m, first);
    bool ok = record(m, first, &vulkan); /* the first target newer than memory, its rendering open */
    snx_ctr_phys_write(m, LIST, second->list.w, second->list.n * 4);
    ok = ok && record(m, second, &vulkan); /* the second's rendering open, the first still to store */
    uint32_t value = 0x9abcdef0u, control = SNX_CTR_FILL_32BIT | 1;
    snx_ctr_fill(expected, AFTER_FILL, value, control);
    ok = ok && snx_ctr_vk_fill(STORE_AT, AFTER_FILL, value, control) == SNX_CTR_XFER_OK;
    ok = ok && record(m, second, &vulkan); /* its rendering ended by the foreign store: the draw opens it again */
    ok = ok && snx_ctr_vk_finish() && snx_ctr_phys_read(m, STORE_AT, actual, AFTER_FILL) == SNX_CTR_MEM_OK;
    check_bytes(first->name, ok, actual, expected, AFTER_FILL);
    free(first);
    free(second);
}
/* a fill over a drawn target stores the target first, so the next draw's reload keeps the filled bytes */
static void fill_over_draw_reload(SnxCtrMemory* m) {
    static unsigned char expected[RELOAD_SPAN], actual[RELOAD_SPAN];
    Scene* first = triangle_scene("a fill over a draw, then a draw", 6);
    Scene* centre = scene(first->name, 0, 0xff);
    centre->vertex_count = 3;
    centre->vertices[0] = vertex(-0.1f, -0.1f, -0.5f, 0xff0000ffu, 0.0f, 0.0f);
    centre->vertices[1] = vertex(0.1f, -0.1f, -0.5f, 0xff00ff00u, 0.0f, 0.0f);
    centre->vertices[2] = vertex(0.0f, 0.1f, -0.5f, 0xffff0000u, 0.0f, 0.0f);
    array_draw(centre);
    list_end(&centre->list);
    setup_clear(m, first);
    bool ok = record(m, first, &vulkan); /* its target newer than memory, its rendering open */
    uint32_t value = 0x2468ace0u, control = SNX_CTR_FILL_32BIT | 1;
    snx_ctr_fill(expected, RELOAD_SPAN, value, control);
    ok = ok && snx_ctr_vk_fill(RELOAD_AT, RELOAD_SPAN, value, control) == SNX_CTR_XFER_OK; /* stores it, then writes */
    snx_ctr_phys_write(m, VERTICES, centre->vertices, sizeof centre->vertices);
    snx_ctr_phys_write(m, LIST, centre->list.w, centre->list.n * 4);
    ok = ok && record(m, centre, &vulkan); /* memory loaded again, away from the filled tiles */
    ok = ok && snx_ctr_vk_finish() && snx_ctr_phys_read(m, RELOAD_AT, actual, RELOAD_SPAN) == SNX_CTR_MEM_OK;
    check_bytes(first->name, ok, actual, expected, RELOAD_SPAN);
    free(first);
    free(centre);
}
/* a transfer recorded after a draw must follow the end of that draw's rendering, and match the CPU reference */
static void transfer_after_draw(SnxCtrMemory* m) {
    static unsigned char in[AFTER_SPAN], before[AFTER_SPAN], expected[AFTER_SPAN], actual[AFTER_SPAN];
    Scene* s = triangle_scene("transfers after draw", 6);
    random_fill(in, sizeof in);
    random_fill(before, sizeof before);
    memcpy(expected, before, sizeof expected);
    snx_ctr_phys_write(m, AFTER_IN, in, AFTER_SPAN);
    snx_ctr_phys_write(m, AFTER_OUT, before, AFTER_SPAN);
    setup_clear(m, s);
    bool ok = record(m, s, &vulkan);
    uint32_t value = 0x12345678u, control = SNX_CTR_FILL_32BIT | 1;
    snx_ctr_fill(expected, AFTER_FILL, value, control);
    ok = ok && snx_ctr_vk_fill(AFTER_OUT, AFTER_FILL, value, control) == SNX_CTR_XFER_OK;
    ok = ok && record(m, s, &vulkan); /* a draw of its own before the display transfer, the rendering open again */
    SnxCtrDisplayTransfer dt = {64 | 64u << 16, 64 | 64u << 16, 0};
    ok = ok &&
         snx_ctr_display_transfer(&dt, in, AFTER_SPAN, expected + 0x10000, AFTER_SPAN - 0x10000) == SNX_CTR_XFER_OK;
    ok = ok && snx_ctr_vk_display_transfer(&dt, AFTER_IN, AFTER_OUT + 0x10000) == SNX_CTR_XFER_OK;
    ok = ok && record(m, s, &vulkan); /* and one before the texture copy */
    SnxCtrTextureCopy tc = {0x400, 0, 0};
    ok = ok && snx_ctr_texture_copy(&tc, in + 0x10000, AFTER_SPAN - 0x10000, expected + 0x18000,
                                    AFTER_SPAN - 0x18000) == SNX_CTR_XFER_OK;
    ok = ok && snx_ctr_vk_texture_copy(&tc, AFTER_IN + 0x10000, AFTER_OUT + 0x18000) == SNX_CTR_XFER_OK;
    ok = ok && snx_ctr_vk_finish() && snx_ctr_phys_read(m, AFTER_OUT, actual, AFTER_SPAN) == SNX_CTR_MEM_OK;
    check_bytes(s->name, ok, actual, expected, AFTER_SPAN);
    free(s);
}

/* four equal RGBA8 pixels, as the guest stores them (A, B, G, R) and as the presenter decodes them */
static void pixels(unsigned char guest[16], unsigned char decoded[16], const unsigned char rgba[4]) {
    for (unsigned i = 0; i < 16; ++i) {
        decoded[i] = rgba[i % 4];
        guest[i] = rgba[3 - i % 4];
    }
}
/* the presents hash as the CPU decode: the blits wait for the clear, and a decode waits for the draw's rendering */
#define PRESENT_FB         (VRAM + 0x200000u)
#define PRESENT_WARM       (VRAM + 0x220000u)
/* what present.c starts a frame's hash from: the decimal digits of SNX_FNV_BASIS without the last one */
#define PRESENT_HASH_BASIS 1469598103934665603ull
static void present_screens(SnxCtrMemory* m, bool after_draw) {
    if (!snx_ctr_present_init(m, NULL)) {
        ++failures;
        return;
    }
    static const unsigned char warm_rgba[4] = {0x50, 0x60, 0x70, 0x80}, rgba[4] = {0x10, 0x20, 0x30, 0x40};
    unsigned char warm_guest[16], warm_decoded[16], guest[16], decoded[16];
    pixels(warm_guest, warm_decoded, warm_rgba);
    pixels(guest, decoded, rgba);
    uint64_t warm_want = snx_fnv1a(PRESENT_HASH_BASIS, warm_decoded, sizeof warm_decoded);
    uint64_t want = snx_fnv1a(PRESENT_HASH_BASIS, decoded, sizeof decoded);
    uint64_t before = 0, after = 0;
    bool ok = snx_ctr_phys_write(m, PRESENT_WARM, warm_guest, sizeof warm_guest) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, PRESENT_FB, guest, sizeof guest) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_present_frame(1, PRESENT_WARM, 2, 2, 0, 2 * 4, &before);
    if (after_draw) {
        Scene* s = triangle_scene("a frame after a draw", 4);
        setup_clear(m, s);
        ok = ok && record(m, s, &vulkan); /* the rendering open, the framebuffer untouched */
        free(s);
    }
    ok = ok && snx_ctr_present_frame(1, PRESENT_FB, 2, 2, 0, 2 * 4, &after);
    bool pass = ok && before == warm_want && after == want;
    printf("%-24s hash %016llx, first %016llx, CPU %016llx, first CPU %016llx: %s\n",
           after_draw ? "a frame after a draw" : "screens presented", (unsigned long long)after,
           (unsigned long long)before, (unsigned long long)want, (unsigned long long)warm_want, pass ? "ok" : "FAIL");
    if (!pass) ++failures;
}
/* A Y2R conversion recorded after a draw, from pages that the draw does not touch, matches the CPU reference byte
   for byte, and the destination page keeps its poison past the converted span. */
static void y2r_after_draw(SnxCtrMemory* m) {
    static unsigned char y[512], u[256], v[256], before[2048], expected[4096], actual[4096], page[4096];
    SnxCtrY2rConfig c = {SNX_CTR_Y2R_422_8, SNX_CTR_Y2R_RGBA8, 0, 0, 64, 8, {0}, 0};
    memcpy(c.coefficients, snx_ctr_y2r_standard[0], sizeof c.coefficients);
    SnxCtrY2rBuffer streams[SNX_CTR_Y2R_STREAMS] = {{0, 0, 64, 0}, {0, 0, 32, 0}, {0, 0, 32, 0}, {0, 0, 256, 0}};
    uint64_t spans[SNX_CTR_Y2R_STREAMS];
    bool ok = snx_ctr_y2r_check(&c, streams, spans);
    random_fill(y, sizeof y);
    random_fill(u, sizeof u);
    random_fill(v, sizeof v);
    random_fill(before, sizeof before);
    memset(expected, POISON, sizeof expected);
    memcpy(expected, before, sizeof before);
    const unsigned char* const inputs[3] = {y, u, v};
    if (ok) snx_ctr_y2r_convert(&c, streams, inputs, expected);
    /* a conversion that the destination already holds proves nothing */
    ok = ok && memcmp(before, expected, sizeof before) != 0;
    /* one page per stream, in FCRAM away from the draw target: 512, 256, 256 and 2048 bytes */
    static const unsigned char* const src[4] = {y, u, v, before};
    static const uint32_t pages_at[4] = {FCRAM + 0x400000u, FCRAM + 0x401000u, FCRAM + 0x402000u, FCRAM + 0x403000u};
    for (unsigned i = 0; ok && i < 4; ++i) {
        memset(page, POISON, sizeof page);
        memcpy(page, src[i], (size_t)spans[i]);
        ok = snx_ctr_phys_write(m, pages_at[i], page, sizeof page) == SNX_CTR_MEM_OK;
    }
    const uint32_t* const pages[4] = {&pages_at[0], &pages_at[1], &pages_at[2], &pages_at[3]};
    const uint32_t counts[4] = {1, 1, 1, 1};
    Scene* s = triangle_scene("a conversion after a draw", 4);
    setup_clear(m, s);
    ok = ok && record(m, s, &vulkan); /* the rendering open, the conversion pages untouched */
    ok = ok && snx_ctr_vk_y2r(&c, streams, pages, counts) == SNX_CTR_XFER_OK;
    ok = ok && snx_ctr_vk_finish() &&
         snx_ctr_phys_read(m, pages_at[SNX_CTR_Y2R_DST], actual, sizeof actual) == SNX_CTR_MEM_OK;
    check_bytes(s->name, ok, actual, expected, sizeof expected);
    free(s);
}

/* an image the cache does not hold, sampled under an open rendering, needs its transition after that rendering */
#define TEX_AFTER_A    16
#define TEX_AFTER_B    32
#define TEX_AFTER_B_AT (TEXTURE + 0x2000)
static void strip_vertices(Scene* s) {
    static const uint32_t colors[4] = {0xff0000ffu, 0xff00ff00u, 0xffff0000u, 0xffffffffu};
    s->vertex_count = 8;
    for (unsigned k = 0; k < 4; ++k) {
        float x = k & 1 ? 1.0f : -1.0f, y = k & 2 ? 1.0f : -1.0f;
        s->vertices[k] = vertex(x, y, -0.5f, colors[k], 0, 0);
        s->vertices[4 + k] = vertex(x, y, -0.5f, 0xffffffffu, k & 1 ? 1.0f : 0.0f, k & 2 ? 1.0f : 0.0f);
    }
}
/* texture unit 0's state for a nearest, repeated texture of size at address */
static void texture_state(List* l, uint32_t address, unsigned size) {
    put(l, TEX_MAIN, 1);
    put(l, TEX0_SIZE, size | size << 16);
    put(l, TEX0_PARAMS, 2u << 8 | 2u << 12);
    put(l, TEX0_ADDRESS, address / 8);
    put(l, TEX0_FORMAT, 0);
    put(l, TEV_STAGE0, 0x00030003u); /* colour and alpha from texture 0 */
}
static void strip_draw(List* l, uint32_t first) {
    put(l, VERTEX_OFFSET, first);
    put(l, NUM_VERTICES, 4);
    put(l, SNX_PICA_TRIGGER_DRAW, 1);
}
/* a strip drawn without a texture, then to the same target with a 16x16 one */
static Scene* texture_scene(const char* name) {
    Scene* s = scene(name, 0, 0xff);
    strip_vertices(s);
    put(&s->list, SNX_PICA_TOPOLOGY, 1u << 8);
    strip_draw(&s->list, 0);
    texture_state(&s->list, TEXTURE, TEX_AFTER_A);
    strip_draw(&s->list, 4);
    return s;
}
static void texture_after_draw(SnxCtrMemory* m) {
    Scene* a = texture_scene("a texture after a draw");
    run(m, a);
    free(a);
    Scene* b = texture_scene("a texture shape after a draw");
    strip_draw(&b->list, 4); /* the same texture, kept from the cache */
    texture_state(&b->list, TEX_AFTER_B_AT, TEX_AFTER_B);
    strip_draw(&b->list, 4); /* a new 32x32 image */
    run(m, b);
    free(b);
}

/* A draw whose indices a texture copy writes, recorded before it in the same command buffer, must be drawn: the
   indices' page is the GPU's until the copy's submission is done, and the draw's scan of them waits for that. The copy
   is the indices' only source. */
#define INDEX_SOURCE     (FCRAM + 0x180000u)
#define INDEX_COPY_SMALL 24
#define INDEX_COPY_LARGE 2088 /* more bytes than one 4096-byte chunk of the scan */
static void index_copy_case(SnxCtrMemory* m, const char* name, unsigned count) {
    static unsigned char color[W * H * 4], sw[W * H * 4], gpu[W * H * 4];
    static unsigned char indices[INDEX_COPY_LARGE * 2], copy[INDEX_COPY_LARGE * 2];
    static const unsigned char zeros[INDEX_COPY_LARGE * 2];
    unsigned bytes = count * 2;
    Scene* s = scene(name, 0, 0xff);
    for (unsigned t = 0; t < 8; ++t) { /* eight nondegenerate triangles, a grid across the target */
        float x = (float)(t & 3) * 0.4f - 0.8f, y = (float)(t >> 2) * 0.6f - 0.8f;
        s->vertices[s->vertex_count++] = vertex(x, y, -0.5f, 0xff0000ffu, 0, 0);
        s->vertices[s->vertex_count++] = vertex(x + 0.4f, y, -0.5f, 0xff00ff00u, 0, 0);
        s->vertices[s->vertex_count++] = vertex(x, y + 0.6f, -0.5f, 0xffff0000u, 0, 0);
    }
    for (unsigned i = 0; i < count; ++i) snx_st_le16(indices + i * 2, (uint16_t)(i % s->vertex_count));
    put(&s->list, 0x227, (INDICES - VERTICES) | 1u << 31);
    put(&s->list, 0x228, count);
    put(&s->list, SNX_PICA_TRIGGER_DRAW_INDEXED, 1);
    list_end(&s->list);
    random_fill(color, sizeof color);
    SnxCtrTextureCopy tc = {bytes, 0, 0}; /* contiguous: no gap, so no line widths */
    setup(m, s, color, color);            /* no depth target */
    bool a = snx_ctr_texture_copy(&tc, indices, bytes, copy, bytes) == SNX_CTR_XFER_OK;
    a = a && snx_ctr_phys_write(m, INDICES, copy, bytes) == SNX_CTR_MEM_OK && record(m, s, &software);
    snx_ctr_phys_read(m, COLOR, sw, sizeof sw);
    setup(m, s, color, color);
    bool b = snx_ctr_phys_write(m, INDEX_SOURCE, indices, bytes) == SNX_CTR_MEM_OK;
    b = b && snx_ctr_phys_write(m, INDICES, zeros, bytes) == SNX_CTR_MEM_OK; /* only the copy may fill them */
    b = b && snx_ctr_vk_texture_copy(&tc, INDEX_SOURCE, INDICES) == SNX_CTR_XFER_OK;
    b = b && record(m, s, &vulkan) && snx_ctr_vk_finish();
    snx_ctr_phys_read(m, COLOR, gpu, sizeof gpu);
    if (drew(s, a, b)) compare(s, sw, gpu, NULL, NULL);
    free(s);
}
static void draw_after_index_copy(SnxCtrMemory* m) {
    index_copy_case(m, "draw after index copy", INDEX_COPY_SMALL);
    index_copy_case(m, "indices over a scan chunk", INDEX_COPY_LARGE);
}
/* A count of 2^31 16-bit indices spans 2^32 bytes, which wraps to none in 32-bit arithmetic: the draw reaches outside
   guest RAM and must draw nothing. */
static void index_count_past_ram(SnxCtrMemory* m) {
    static unsigned char color[W * H * 4], after[W * H * 4];
    Scene* s = scene("indices past the RAM", 0, 0xff);
    triangles(s, 2);
    put(&s->list, 0x227, (INDICES - VERTICES) | 1u << 31);
    put(&s->list, 0x228, 1u << 31);
    put(&s->list, SNX_PICA_TRIGGER_DRAW_INDEXED, 1);
    list_end(&s->list);
    random_fill(color, sizeof color);
    setup(m, s, color, color); /* no depth target */
    bool ok = record(m, s, &vulkan) && snx_ctr_vk_finish() &&
              snx_ctr_phys_read(m, COLOR, after, sizeof after) == SNX_CTR_MEM_OK;
    bool same = ok && !memcmp(color, after, sizeof color);
    printf("%-24s the target %s: %s\n", s->name, same ? "is unchanged" : "changed", same ? "ok" : "FAIL");
    if (!same) ++failures;
    free(s);
}

static void commands_after_draw(SnxCtrMemory* m) {
    y2r_after_draw(m);
    transfer_after_draw(m);
    store_drawn_target(m);
    fill_over_draw_reload(m);
}
static void screens(SnxCtrMemory* m) { present_screens(m, false); }
static void presented_after_draw(SnxCtrMemory* m) {
    present_screens(m, true);
    commands_after_draw(m);
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
    commands_after_draw(m);
    texture_after_draw(m);
    draw_after_index_copy(m);
    index_count_past_ram(m);
}

typedef struct {
    const char* flag;
    const char* what;
    void (*run)(SnxCtrMemory* memory);
} Mode;
static const Mode scenes = {NULL, "the scenes", all_scenes};
/* the validation runs record only the commands under test */
static const Mode modes[] = {{"--present", "the presented screens", screens},
                             {"--after-draw", "the commands after a draw", presented_after_draw},
                             {"--texture-after-draw", "the textures after a draw", texture_after_draw},
                             {"--draw-after-index-copy", "the indices copied before a draw", draw_after_index_copy}};

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
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_y2r_free();
    snx_ctr_present_free();
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    printf("3ds vk draw: %s %s the CPU reference\n", mode->what, failures ? "differ from" : "agree with");
    return failures != 0;
}
