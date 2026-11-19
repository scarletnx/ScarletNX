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
#include "3ds/input/input.h"
#include "common/gpu/vk/vk_common.h"
#include "common/platform/file.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/3ds_sdl_events.h"
#include "support/3ds_vk_path.h"
#include "support/frame_dump.h"
#include "support/thread_nice.h"
#include "snx_settings.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The board the scenes run on and where they put their buffers and targets: an Old 3DS's FCRAM and VRAM, or with
   --new3ds a New 3DS's, every buffer and target in the upper half of FCRAM that only it has, the targets where VRAM
   would be, and the vertices in its extra RAM (0x1f000000). The indices stay above the vertices: the PICA finds them
   at the attributes' base plus a 28-bit offset */
static SnxCtrModel g_model = SNX_CTR_OLD;
static uint32_t g_fcram = 0x20000000u, g_vram = 0x18000000u, g_vertices = 0x20110000u;
#define FCRAM         g_fcram
#define VRAM          g_vram
#define LIST (FCRAM + 0x100000u)
#define VERTICES      g_vertices
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
#define THIRD_COLOR (VRAM + 0x380000u)
#define STORE_AT     (COLOR + 0x1000)
#define RELOAD_AT    (COLOR + (H - 2) * W * 4) /* the tiled guest buffer's last two 8x8 tiles, a corner of the target */
#define RELOAD_SPAN  (2 * W * 4)
#define POISON       0xa5
#define SCALED_FB     (FCRAM + 0x400000u) /* a top screen framebuffer that display transfers fill */
#define ROTATE_FB     (FCRAM + 0x480000u) /* the other of the two a game alternates a screen's picture between */
#define SCALED_COPY   (FCRAM + 0x500000u) /* the same bytes, which nothing kept at a render scale holds */
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
static int skips; /* checks that found nothing to run, which ctest has to see as 77 and not as a pass */
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

/* a vertex: position, colour, texture coordinate 0 and its w (which only scenes that load three components read) */
typedef struct {
    float pos[4], color[4], tc[3];
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
/* geometry shader instructions: SETEMIT's vertex (bits 24..25), primitive (23) and winding (22) flags, and EMIT */
static uint32_t setemit(unsigned vertex, bool primitive, bool winding) {
    return 0x2bu << 26 | vertex << 24 | (primitive ? 1u << 23 : 0) | (winding ? 1u << 22 : 0);
}
#define EMIT (0x2au << 26)
/* a geometry shader on the base's three outputs a vertex: use (0x229), config (0x252) and the unit's input buffer
   configuration (attributes - 1, input_to_uniform), its inputs mapped in order, its three outputs enabled */
static void geometry_shader(List* l, uint32_t use, uint32_t config, uint32_t input, const uint32_t* program,
                            unsigned words) {
    put(l, SNX_PICA_USE_GS, use);
    put(l, 0x24a, 2); /* vs_outmap_total_minus_1_a and _b */
    put(l, 0x251, 2);
    put(l, 0x252, config);
    put(l, SNX_PICA_TOPOLOGY, 3u << 8);
    put(l, SNX_PICA_GS + SNX_PICA_SH_INPUT_CONFIG, input | 0x08u << 24);
    put(l, SNX_PICA_GS + SNX_PICA_SH_ATTR_MAP_LOW, 0x76543210u);
    put(l, SNX_PICA_GS + SNX_PICA_SH_ATTR_MAP_HIGH, 0xfedcba98u);
    put(l, SNX_PICA_GS + SNX_PICA_SH_OUTPUT_MASK, 7);
    put(l, SNX_PICA_GS + SNX_PICA_SH_MAIN_OFFSET, 0);
    put(l, SNX_PICA_GS + SNX_PICA_SH_PROGRAM_OFFSET, 0);
    for (unsigned i = 0; i < words; ++i) put(l, SNX_PICA_GS + SNX_PICA_SH_PROGRAM_WORD, program[i]);
    put(l, SNX_PICA_GS + SNX_PICA_SH_SWIZZLE_OFFSET, 0);
    put(l, SNX_PICA_GS + SNX_PICA_SH_SWIZZLE_WORD, XYZW);
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
/* the scene's buffers and the targets' first bytes in the board's memory: a write outside it ends the test, as the two
   renderers would otherwise agree on what neither was given */
static void put_in(SnxCtrMemory* m, uint32_t pa, const void* bytes, size_t size, const char* what) {
    if (snx_ctr_phys_write(m, pa, bytes, size) != SNX_CTR_MEM_OK) {
        fprintf(stderr, "the %s at 0x%08x are outside the board's memory\n", what, pa);
        exit(1);
    }
}
static void setup(SnxCtrMemory* m, const Scene* s, const unsigned char* color, const unsigned char* depth) {
    put_in(m, VERTICES, s->vertices, sizeof s->vertices, "vertices");
    put_in(m, TEXTURE, s->texture, sizeof s->texture, "texture's texels");
    put_in(m, INDICES, s->indices, sizeof s->indices, "indices");
    put_in(m, COLOR, color, W * H * 4, "colour target's bytes");
    put_in(m, DEPTH, depth, W * H * 4, "depth target's bytes");
    put_in(m, LIST, s->list.w, s->list.n * 4, "command list's words");
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
                       i / W, a, dbpp == 4 ? sw_depth[i * 4 + 3] : 0u, b, dbpp == 4 ? gpu_depth[i * 4 + 3] : 0u);
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
/* count immediate-mode vertices of a random position and colour, each attribute float24 w, z, y, x packed big-end
   first */
static void immediate_vertices(List* l, unsigned count) {
    put(l, SNX_PICA_DEFAULT_ATTR_INDEX, 15);
    for (unsigned t = 0; t < count; ++t) {
        float v[3][4] = {
            {(float)(next() % 2001) / 1000.0f - 1.0f, (float)(next() % 2001) / 1000.0f - 1.0f, -0.5f, 1.0f},
            {(float)(next() % 256) / 255.0f, (float)(next() % 256) / 255.0f, (float)(next() % 256) / 255.0f, 1.0f},
            {0, 0, 0, 1}};
        for (unsigned a = 0; a < 3; ++a) {
            uint32_t x = f24(v[a][0]), y = f24(v[a][1]), z = f24(v[a][2]), w = f24(v[a][3]);
            put(l, SNX_PICA_DEFAULT_ATTR_VALUE, w << 8 | z >> 16);
            put(l, SNX_PICA_DEFAULT_ATTR_VALUE, (z & 0xffff) << 16 | y >> 8);
            put(l, SNX_PICA_DEFAULT_ATTR_VALUE, (y & 0xff) << 24 | x);
        }
    }
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
    if (!snx_ctr_present_init(m, NULL, false)) {
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
    const uint32_t pages_at[4] = {FCRAM + 0x400000u, FCRAM + 0x401000u, FCRAM + 0x402000u, FCRAM + 0x403000u};
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

/* A draw recorded again into the target a submission left open, after a wait for that submission, with the same
   uniforms and LUTs: its uploads and LUT views must be the new recording's, not the retired slot's */
static void draw_after_submit(SnxCtrMemory* m) {
    static unsigned char color[W * H * 4], sw[W * H * 4], gpu[W * H * 4];
    Scene* s = triangle_scene("a draw after a submission", 4);
    random_fill(color, sizeof color);
    setup(m, s, color, color); /* no depth target */
    bool a = record(m, s, &software) && record(m, s, &software);
    snx_ctr_phys_read(m, COLOR, sw, sizeof sw);
    setup(m, s, color, color);
    /* no CPU access between the two: the target stays open, so only the second draw's own setup opens the next
       recording */
    bool b = record(m, s, &vulkan) && snx_ctr_vk_finish() && record(m, s, &vulkan) && snx_ctr_vk_finish();
    snx_ctr_phys_read(m, COLOR, gpu, sizeof gpu);
    if (drew(s, a, b)) compare(s, sw, gpu, NULL, NULL);
    free(s);
}

static void commands_after_draw(SnxCtrMemory* m) {
    y2r_after_draw(m);
    transfer_after_draw(m);
    store_drawn_target(m);
    fill_over_draw_reload(m);
}
static void screens(SnxCtrMemory* m) { present_screens(m, false); }
/* async_pipelines: a draw whose pipeline is new is skipped while a worker makes it, so its target keeps what it held;
   once the pipeline is made the same list draws as the CPU reference does, and is not skipped again. The workers are
   held until the first draw is recorded: with shader objects they get its shaders as they are made, before the draw,
   and one done by then would leave nothing to skip. Of the three workers the third runs at the system's low priority.
   The stand-in shaders, which would draw it at once, are off: 3ds_vk_draw_stand_in holds them to the reference */
static void async_pipelines(SnxCtrMemory* m) {
    static unsigned char start[W * H * 4], none[W * H * 4], sw[W * H * 4], sw_depth[W * H * 4], gpu[W * H * 4],
        gpu_depth[W * H * 4];
    g_settings.async_pipelines = 1;
    g_settings.ctr_stand_in = 0;
    g_settings.pipeline_threads = 3;
    Scene* s = triangle_scene("async, then made", 6);
    random_fill(start, sizeof start);
    setup(m, s, start, none);
    snx_ctr_vk_pipelines_hold();
    bool recorded = draw(m, s, &vulkan, gpu, gpu_depth);
    uint64_t skipped = snx_ctr_vk_pipelines_wait();
    bool kept = recorded && skipped == 1 && !memcmp(gpu, start, sizeof gpu);
    printf("%-24s %llu draws skipped while the pipeline was made, the target %s: %s\n", "async, first draw",
           (unsigned long long)skipped, memcmp(gpu, start, sizeof gpu) ? "drawn on" : "as it was",
           kept ? "ok" : "FAIL");
    failures += !kept;
#if defined(__linux__)
    {
        int normal, low, other;
        bool split = thread_nice_settled("3DS pipelines", 2, 1, &normal, &low, &other);
        printf("%-24s %d workers at the main thread's nice, %d at nice +%d, %d at another: %s\n", "async, workers",
               normal, low, SNX_THREAD_LOW_NICE, other, split ? "ok" : "FAIL");
        failures += !split;
    }
#endif
    setup(m, s, start, none);
    bool a = draw(m, s, &software, sw, sw_depth);
    setup(m, s, start, none);
    bool b = draw(m, s, &vulkan, gpu, gpu_depth);
    if (drew(s, a, b)) compare(s, sw, gpu, sw_depth, gpu_depth);
    skipped = snx_ctr_vk_pipelines_wait();
    if (skipped != 1) {
        printf("%-24s %llu draws skipped in all, where the pipeline made before skips none: FAIL\n", s->name,
               (unsigned long long)skipped);
        ++failures;
    }
    free(s);
}
/* the pipeline cache kept between runs: a draw's pipeline goes into the file named for the driver, in the folder's
   shader_cache, and a write that fails, onto a folder in the file's place, is made again by the next one */
static void pipeline_cache(SnxCtrMemory* m) {
    if (snx_ctr_vk()->shader_object) {
        printf("%-24s skipped: draws bind shader objects, so no pipeline cache is made\n", "pipeline cache");
        return;
    }
    static unsigned char start[W * H * 4], none[W * H * 4], gpu[W * H * 4], gpu_depth[W * H * 4];
    char* base = snx_temp_dir(NULL);
    char* folder = base ? snx_path_join(base, "pipeline-cache") : NULL;
    if (!folder) {
        printf("pipeline cache: no temporary folder: FAIL\n");
        ++failures;
        free(base);
        return;
    }
    (void)snx_remove_tree(folder);
    (void)plat_mkdir(folder); /* the game's folder, where the renderer makes shader_cache */
    g_settings.vulkan_pipeline_cache = 1;
    snx_ctr_vk_pipeline_cache_open(folder);
    VkPhysicalDeviceDriverProperties driver = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 props = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &driver};
    vkGetPhysicalDeviceProperties2(snx_ctr_vk()->phys, &props);
    char tag[SNX_VK_DRIVER_TAG], dir[256], path[512];
    snx_vk_driver_tag(tag, sizeof tag, (uint32_t)driver.driverID, props.properties.vendorID, props.properties.deviceID,
                      NULL);
    snprintf(dir, sizeof dir, "%s/shader_cache", folder);
    snx_vk_driver_path(path, sizeof path, dir, "vk_pipelines", tag, ".bin");
    Scene* s = triangle_scene("pipeline cache", 3);
    random_fill(start, sizeof start);
    setup(m, s, start, none);
    bool drawn = draw(m, s, &vulkan, gpu, gpu_depth);
    bool blocked = !plat_mkdir(path);
    snx_ctr_vk_pipeline_cache_save(true); /* fails, and says so */
    bool kept_out = snx_is_dir(path) && !snx_remove_empty_dir(path);
    snx_ctr_vk_pipeline_cache_save(true);
    size_t size = 0;
    unsigned char* data = snx_read_file(path, &size);
    char** names = NULL;
    size_t count = 0, temporary = 0;
    if (!snx_list_dir(dir, &names, &count))
        for (size_t i = 0; i < count; ++i) {
            size_t n = strlen(names[i]);
            temporary += n > 4 && !strcmp(names[i] + n - 4, ".tmp");
        }
    snx_free_strings(names, count);
    /* lavapipe keeps no pipeline: its file is the header alone, or less (Mesa 26.2 gives 20 bytes) */
    bool ok = drawn && blocked && kept_out && data && size && !temporary;
    printf("%-24s %s: %zu bytes once the folder in its place went, %zu temporary files: %s\n", s->name, path, size,
           temporary, ok ? "ok" : "FAIL");
    failures += !ok;
    free(data);
    free(s);
    (void)snx_remove_tree(folder);
    (void)snx_remove_tree(base);
    free(folder);
    free(base);
}
static void presented_after_draw(SnxCtrMemory* m) {
    present_screens(m, true);
    commands_after_draw(m);
}
/* A frame of GSP's colour formats (present.h, transfer.h's order) drawn at a render scale above 1 is shown from its
   target: the presenter takes the target of the frame's own format and stores nothing to the RAM. The PICA's
   colour buffer format register (0x117) has another order: RGB5A1 2, RGB565 3 */
static void scaled_present(SnxCtrMemory* m) {
    static const struct {
        const char* name;
        unsigned pica, gsp;
    } cases[] = {{"an RGB565 frame at 2x", 3, SNX_CTR_PIX_RGB565}, {"an RGB5A1 frame at 2x", 2, SNX_CTR_PIX_RGB5A1}};
    if (!snx_ctr_present_init(m, NULL, false)) {
        ++failures;
        return;
    }
    g_settings.ctr_top_resolution = 2;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; ++i) {
        uint32_t at = COLOR + (uint32_t)i * 0x40000u;
        Scene* s = scene(cases[i].name, cases[i].pica, 0xff);
        /* the top screen's framebuffer, 240 x 400: a target the top screen's render scale takes */
        put(&s->list, 0x041, f24(240 / 2.0f));
        put(&s->list, 0x043, f24(400 / 2.0f));
        put(&s->list, 0x11e, 240 | (400 - 1) << 12 | 1u << 24);
        put(&s->list, 0x11d, at / 8);
        triangles(s, 4);
        array_draw(s);
        list_end(&s->list);
        setup_clear(m, s);
        bool ok = record(m, s, &vulkan); /* the target drawn, newer than the RAM */
        SnxCtrVkTraffic before = snx_ctr_vk_traffic();
        ok = ok && snx_ctr_present_frame(0, at, 240, 400, cases[i].gsp, 240 * 2, NULL);
        uint64_t stored = snx_ctr_vk_traffic().written - before.written;
        ok = ok && snx_ctr_vk_finish();
        bool pass = ok && stored == 0;
        printf("%-24s %llu bytes stored to the RAM by the present: %s\n", s->name, (unsigned long long)stored,
               pass ? "ok" : "FAIL");
        if (!pass) ++failures;
        free(s);
    }
    g_settings.ctr_top_resolution = 1;
}
/* a scene of random triangles over a target of width x height, its rows from the top as games have them, or from the
   bottom */
static Scene* scaled_scene(const char* name, unsigned width, unsigned height, bool from_top) {
    Scene* s = scene(name, 0, 0xff);
    put(&s->list, 0x11e, width | (height - 1) << 12 | (from_top ? 1u << 24 : 0));
    put(&s->list, 0x041, f24((float)width / 2.0f));
    put(&s->list, 0x043, f24((float)height / 2.0f));
    triangles(s, 6);
    array_draw(s);
    list_end(&s->list);
    return s;
}
/* A target of width x height (rows from the top or not) drawn at twice the console's scale and copied from its row
   first to SCALED_FB by a display transfer of out_size and flags, which leaves a 240-pixel framebuffer of rows rows in
   the transfer's output format at row of it: the presenter's lookup gives a copy at the transfer's scale and origin,
   and the frame is the picture the same bytes make decoded from memory. Geometry only: the copy and the 1x decode of
   one target differ by its quantisation alone, so this does not tell the copy from memory's own picture */
static void scaled_case(SnxCtrMemory* m, const char* dir, const char* name, unsigned width, unsigned height,
                        bool from_top, unsigned first, uint32_t out_size, uint32_t flags, unsigned row, unsigned rows,
                        unsigned index) {
    static const unsigned char clear[480 * 400 * 4];
    static unsigned char frame[240 * 400 * 4];
    Scene* s = scaled_scene(name, width, height, from_top);
    setup_clear(m, s);
    snx_ctr_phys_write(m, COLOR, clear, (size_t)width * height * 4);
    SnxCtrDisplayTransfer dt = {width | height << 16, out_size, flags};
    unsigned format = flags >> SNX_CTR_XFER_OUT_FORMAT & 7, pitch = 240 * snx_ctr_pixel_bytes(format);
    uint32_t fb = SCALED_FB + row * pitch, bytes = pitch * rows;
    SnxCtrVkScaled scaled = {0};
    bool ok = record(m, s, &vulkan) &&
              snx_ctr_vk_display_transfer(&dt, COLOR + first * width * 4, SCALED_FB) == SNX_CTR_XFER_OK;
    bool kept = ok && snx_ctr_vk_target_image(fb, 240, rows, format, pitch, &scaled) && scaled.scale == 2 &&
                scaled.x == 0 && scaled.y == row;
    ok = ok && snx_ctr_present_frame(0, fb, 240, rows, format, pitch, NULL) && snx_ctr_vk_finish();
    ok = ok && snx_ctr_phys_read(m, fb, frame, bytes) == SNX_CTR_MEM_OK &&
         snx_ctr_phys_write(m, SCALED_COPY, frame, bytes) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_present_frame(0, SCALED_COPY, 240, rows, format, pitch, NULL);
    unsigned w[2] = {0}, h[2] = {0}, off = 0;
    unsigned char* shown = test_ctr_frame_dump(dir, 0, index, &w[0], &h[0]);
    unsigned char* memory = test_ctr_frame_dump(dir, 0, index + 1, &w[1], &h[1]);
    bool same = shown && memory && w[0] == rows && h[0] == 240 && w[1] == rows && h[1] == 240;
    for (size_t i = 0; same && i < (size_t)rows * 240; ++i) {
        unsigned d = 0;
        for (unsigned c = 0; c < 3; ++c) {
            unsigned a = shown[i * 3 + c], b = memory[i * 3 + c], e = a > b ? a - b : b - a;
            if (e > d) d = e;
        }
        off += d > 8;
    }
    bool pass = ok && kept && same && off <= rows * 240 / 25;
    printf("%-24s the copy's scale and origin: %s, %5u pixels off the memory's picture: %s\n", name, kept ? "ok" : "no",
           off, pass ? "ok" : "FAIL");
    if (!pass) ++failures;
    free(shown);
    free(memory);
    free(s);
}
/* A target drawn at a render scale and copied to a framebuffer by display transfers is shown at that scale, with the
   picture its memory has, until a draw or a fill changes the framebuffer. The presenter numbers its first eight dumps
   only (present.c), two a case, so four cases fit */
static void scaled_transfer(SnxCtrMemory* m) {
    char* dir = snx_temp_dir(NULL);
    if (!dir) {
        ++failures;
        return;
    }
    int top = g_settings.ctr_top_resolution;
    g_settings.ctr_top_resolution = 2;
    if (!snx_ctr_present_init(m, dir, false)) {
        ++failures;
        g_settings.ctr_top_resolution = top;
        free(dir);
        return;
    }
    uint32_t rgb565 = SNX_CTR_PIX_RGB565 << SNX_CTR_XFER_OUT_FORMAT, whole = 240 | 400u << 16;
    scaled_case(m, dir, "a scaled transfer", 240, 400, true, 0, whole, 0, 0, 400, 0);
    scaled_case(m, dir, "a scaled flipped transfer", 240, 400, true, 0, whole, SNX_CTR_XFER_FLIP, 0, 400, 2);
    scaled_case(m, dir, "a scaled 2:1 transfer", 480, 400, false, 0, 480 | 400u << 16, 1u << SNX_CTR_XFER_SCALE_SHIFT,
                0, 400, 4);
    scaled_case(m, dir, "a scaled RGB565 part", 240, 400, true, 80, 240 | 320u << 16, rgb565, 16, 304, 6);
    /* the last case's copy, then a draw over it; the same copy again, then a fill over it */
    uint32_t part = SCALED_FB + 16 * 240 * 2;
    SnxCtrVkScaled scaled = {0};
    Scene* over = scene("a draw over it", 0, 0xff);
    put(&over->list, FB_COLOR_ADDR, SCALED_FB / 8);
    triangles(over, 4);
    array_draw(over);
    list_end(&over->list);
    snx_ctr_phys_write(m, VERTICES, over->vertices, sizeof over->vertices);
    snx_ctr_phys_write(m, LIST, over->list.w, over->list.n * 4);
    bool ok = record(m, over, &vulkan);
    bool drawn = ok && !snx_ctr_vk_target_image(part, 240, 304, SNX_CTR_PIX_RGB565, 240 * 2, &scaled);
    SnxCtrDisplayTransfer dt = {whole, 240 | 320u << 16, rgb565};
    ok = ok && snx_ctr_vk_display_transfer(&dt, COLOR + 80 * 240 * 4, SCALED_FB) == SNX_CTR_XFER_OK;
    bool again = ok && snx_ctr_vk_target_image(part, 240, 304, SNX_CTR_PIX_RGB565, 240 * 2, &scaled);
    ok = ok && snx_ctr_vk_fill(SCALED_FB, 0x1000, 0, SNX_CTR_FILL_32BIT | 1) == SNX_CTR_XFER_OK;
    bool filled = !snx_ctr_vk_target_image(part, 240, 304, SNX_CTR_PIX_RGB565, 240 * 2, &scaled);
    ok = ok && snx_ctr_vk_finish();
    bool pass = ok && drawn && again && filled;
    printf("%-24s the scaled image dropped: %s, kept again: %s, dropped after a fill: %s: %s\n", over->name,
           drawn ? "yes" : "no", again ? "yes" : "no", filled ? "yes" : "no", pass ? "ok" : "FAIL");
    if (!pass) ++failures;
    free(over);
    g_settings.ctr_top_resolution = top;
    (void)snx_remove_tree(dir);
    free(dir);
}
/* A display transfer's destination whose bytes the guest changes after it: the presenter shows the picture at the
   render scale while the framebuffer's memory still holds the transfer's bytes, and the memory's own pixel where it
   does not. A byte before the framebuffer is in its first page; a byte of a pixel makes that pixel the memory's, and
   a byte of the next pixel, its first byte equal to the copy's, tells a compare of the first byte from a full one */
static void scaled_verify(SnxCtrMemory* m) {
    static const unsigned char clear[240 * 400 * 4];
    static const unsigned char red[2] = {0x00, 0xf8}; /* RGB565 red */
    static unsigned char frame[240 * 320 * 2];
    char* dir = snx_temp_dir(NULL);
    if (!dir) {
        ++failures;
        return;
    }
    int top = g_settings.ctr_top_resolution;
    g_settings.ctr_top_resolution = 2;
    if (!snx_ctr_present_init(m, dir, false)) {
        ++failures;
        g_settings.ctr_top_resolution = top;
        free(dir);
        return;
    }
    Scene* s = scaled_scene("a changed destination", 240, 400, true);
    setup_clear(m, s);
    snx_ctr_phys_write(m, COLOR, clear, sizeof clear);
    SnxCtrDisplayTransfer dt = {240 | 400u << 16, 240 | 320u << 16, SNX_CTR_PIX_RGB565 << SNX_CTR_XFER_OUT_FORMAT};
    unsigned pitch = 240 * 2, row = 16, rows = 304, bytes = pitch * rows;
    uint32_t fb = SCALED_FB + row * pitch;
    bool ok = record(m, s, &vulkan) &&
              snx_ctr_vk_display_transfer(&dt, COLOR + 80 * 240 * 4, SCALED_FB) == SNX_CTR_XFER_OK &&
              snx_ctr_present_frame(0, fb, 240, rows, SNX_CTR_PIX_RGB565, pitch, NULL);
    /* the same bytes from another address: the memory's own picture */
    ok = ok && snx_ctr_phys_read(m, fb, frame, bytes) == SNX_CTR_MEM_OK &&
         snx_ctr_phys_write(m, SCALED_COPY, frame, bytes) == SNX_CTR_MEM_OK &&
         snx_ctr_present_frame(0, SCALED_COPY, 240, rows, SNX_CTR_PIX_RGB565, pitch, NULL);
    SnxCtrVkScaled scaled = {0};
    bool kept = snx_ctr_vk_target_image(fb, 240, rows, SNX_CTR_PIX_RGB565, pitch, &scaled) && scaled.scale == 2 &&
                scaled.kept != VK_NULL_HANDLE;
    /* a byte of the destination before the framebuffer, in the framebuffer's first page: the copy stays */
    unsigned char white[2] = {0xff, 0xff};
    ok = ok && snx_ctr_phys_write(m, fb - 2, white, sizeof white) == SNX_CTR_MEM_OK &&
         snx_ctr_present_frame(0, fb, 240, rows, SNX_CTR_PIX_RGB565, pitch, NULL);
    bool kept_page =
        snx_ctr_vk_target_image(fb, 240, rows, SNX_CTR_PIX_RGB565, pitch, &scaled) && scaled.kept != VK_NULL_HANDLE;
    /* a pixel of the framebuffer: that pixel shows the memory, the rest the copy */
    ok = ok && snx_ctr_phys_write(m, fb, red, sizeof red) == SNX_CTR_MEM_OK &&
         snx_ctr_present_frame(0, fb, 240, rows, SNX_CTR_PIX_RGB565, pitch, NULL);
    bool kept_pixel =
        snx_ctr_vk_target_image(fb, 240, rows, SNX_CTR_PIX_RGB565, pitch, &scaled) && scaled.kept != VK_NULL_HANDLE;
    /* one byte of the next pixel, its first byte equal to the copy's: a compare of the first byte alone would keep the
       copy's colour, a full one shows the memory's */
    unsigned char one = (unsigned char)(frame[3] ^ 0xff);
    ok = ok && snx_ctr_phys_write(m, fb + 3, &one, sizeof one) == SNX_CTR_MEM_OK &&
         snx_ctr_present_frame(0, fb, 240, rows, SNX_CTR_PIX_RGB565, pitch, NULL);
    bool kept_one =
        snx_ctr_vk_target_image(fb, 240, rows, SNX_CTR_PIX_RGB565, pitch, &scaled) && scaled.kept != VK_NULL_HANDLE;
    ok = ok && snx_ctr_phys_read(m, fb, frame, bytes) == SNX_CTR_MEM_OK &&
         snx_ctr_phys_write(m, SCALED_COPY, frame, bytes) == SNX_CTR_MEM_OK &&
         snx_ctr_present_frame(0, SCALED_COPY, 240, rows, SNX_CTR_PIX_RGB565, pitch, NULL);
    ok = snx_ctr_vk_finish() && ok;
    unsigned w[6] = {0}, h[6] = {0};
    unsigned char* before = test_ctr_frame_dump(dir, 0, 0, &w[0], &h[0]);
    unsigned char* memory = test_ctr_frame_dump(dir, 0, 1, &w[1], &h[1]);
    unsigned char* page = test_ctr_frame_dump(dir, 0, 2, &w[2], &h[2]);
    unsigned char* pixel = test_ctr_frame_dump(dir, 0, 3, &w[3], &h[3]);
    unsigned char* one_copy = test_ctr_frame_dump(dir, 0, 4, &w[4], &h[4]);
    unsigned char* one_memory = test_ctr_frame_dump(dir, 0, 5, &w[5], &h[5]);
    bool same = before && memory && page && pixel && one_copy && one_memory;
    for (unsigned i = 0; same && i < 6; ++i) same = w[i] == rows && h[i] == 240;
    unsigned copy_off = 0, page_off = 0, pixel_off = 0;
    bool red_ok = true;
    for (size_t i = 0; same && i < (size_t)rows * 240; ++i) {
        const unsigned char *a = &before[i * 3], *b = &memory[i * 3], *p = &page[i * 3], *q = &pixel[i * 3];
        copy_off += a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
        page_off += a[0] != p[0] || a[1] != p[1] || a[2] != p[2];
        if (a[0] != q[0] || a[1] != q[1] || a[2] != q[2]) {
            ++pixel_off;
            red_ok = red_ok && q[0] == 255 && q[1] == 0 && q[2] == 0;
        }
    }
    unsigned one_off = 0;
    bool one_ok = true;
    for (size_t i = 0; same && i < (size_t)rows * 240; ++i) {
        const unsigned char *p = &pixel[i * 3], *c = &one_copy[i * 3], *q = &one_memory[i * 3];
        if (p[0] != c[0] || p[1] != c[1] || p[2] != c[2]) {
            ++one_off;
            one_ok = one_ok && c[0] == q[0] && c[1] == q[1] && c[2] == q[2];
        }
    }
    /* not the memory's picture, the page's write leaves it alone, and the framebuffer's own byte is the memory's
       colour */
    bool pass = ok && kept && kept_page && kept_pixel && kept_one && same && copy_off > 0 && page_off == 0 &&
                pixel_off == 1 && red_ok && one_off == 1 && one_ok;
    printf("%-24s kept after a page byte %s, after a framebuffer byte %s, after one of the next %s; pixels off the "
           "memory: %u; off the copy: page %u, pixel %u; the one-byte pixel's frame equal to the memory: %s: %s\n",
           s->name, kept_page ? "yes" : "no", kept_pixel ? "yes" : "no", kept_one ? "yes" : "no", copy_off, page_off,
           pixel_off, one_ok ? "yes" : "no", pass ? "ok" : "FAIL");
    if (!pass) ++failures;
    free(before);
    free(memory);
    free(page);
    free(pixel);
    free(one_copy);
    free(one_memory);
    free(s);
    g_settings.ctr_top_resolution = top;
    (void)snx_remove_tree(dir);
    free(dir);
}
/* The screens' layout takes each frame at the console's size (screen_rect): a top screen drawn at 2x above a bottom one
   presented at 1x is laid out as the console's 400x240 over 320x240, so at a 600x720 window the pair is 1.5x and fills
   it, the bottom screen 480x360 at (60, 360), and a press on its own pixel (160, 220) is window (300, 690). A layout at
   the frames' own sizes would take the top screen at its 800x480 frame, scale the 800x720 pair to 0.75 and centre its
   600x540, the bottom screen 240x180 at (180, 450): the press would be below its 630 edge and miss */
static void scaled_layout(SnxCtrMemory* m) {
    if (!snx_ctr_vk()->host.window) {
        printf("%-24s no window to lay the screens out in: skipped\n", "the screens' layout");
        ++skips;
        return;
    }
    if (!snx_ctr_present_init(m, NULL, false) || !snx_ctr_present_windowed()) {
        printf("%-24s the presenter has no window to show the screens in: skipped\n", "the screens' layout");
        ++skips;
        return;
    }
    SDL_Window* window = snx_ctr_vk()->host.window;
    SDL_WindowID id = SDL_GetWindowID(window);
    int w = 0, h = 0;
    if (!SDL_GetWindowSize(window, &w, &h) || w != 600 || h != 720) {
        printf("%-24s the fixture needs the presenter's 600x720 window, not %dx%d: skipped\n", "the screens' layout", w,
               h);
        ++skips;
        return;
    }
    int top = g_settings.ctr_top_resolution;
    g_settings.ctr_top_resolution = 2;
    static const unsigned char clear[240 * 400 * 4];
    Scene* s = scaled_scene("the screens' layout", 240, 400, true);
    setup_clear(m, s);
    snx_ctr_phys_write(m, COLOR, clear, sizeof clear);
    bool ok = record(m, s, &vulkan);
    /* the top screen from its target at the render scale, the bottom one from memory at the console's */
    ok = ok && snx_ctr_present_frame(1, SCALED_COPY, 240, 320, SNX_CTR_PIX_RGBA8, 240 * 4, NULL) &&
         snx_ctr_present_frame(0, COLOR, 240, 400, SNX_CTR_PIX_RGBA8, 240 * 4, NULL);
    SnxCtrVkScaled scaled = {0};
    bool at_scale =
        ok && snx_ctr_vk_target_image(COLOR, 240, 400, SNX_CTR_PIX_RGBA8, 240 * 4, &scaled) && scaled.scale == 2;
    /* at a 600x720 window the console's pair is 1.5x: the bottom screen 480x360 at (60, 360), its pixel (160, 220)
       across the window at (300, 690), below the 630 edge of a layout at the frames' own sizes */
    event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, 300, 690);
    SnxCtrInput in;
    snx_ctr_input_read(&in);
    event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, id, 300, 690);
    bool touched = in.touch && in.touch_x == 160 && in.touch_y == 220;
    bool pass = at_scale && touched;
    printf("%-24s the top at %ux; a press on the bottom screen's pixel (160, 220) %s, the touch (%u, %u): %s\n",
           s->name, scaled.scale, in.touch ? "touched it" : "missed it", in.touch_x, in.touch_y, pass ? "ok" : "FAIL");
    if (!pass) ++failures;
    /* a press on the bottom screen's pixel (160, 220) under each layout that shows the bottom screen, with and without
       the swap (the one-screen layout only with the swap). The press is the centre of that pixel in the rectangle
       snx_ctr_layout_rects gives this window, so the presenter's screen_rect and touch mapping must use the same
       layout and swap to land on it */
    static const struct {
        int layout;
        bool swap;
    } views[] = {{0, false}, {0, true}, {1, true}, {2, false}, {2, true}, {3, false}, {3, true}};
    const SnxCtrScreens sizes = {400, 240, 320, 240};
    const int layout_was = g_settings.ctr_screen_layout, swap_was = g_settings.ctr_screen_swap;
    for (size_t i = 0; i < sizeof views / sizeof views[0]; ++i) {
        g_settings.ctr_screen_layout = views[i].layout;
        g_settings.ctr_screen_swap = views[i].swap;
        SnxCtrRects rects = {0};
        bool laid =
            snx_ctr_layout_rects((SnxCtrLayout)views[i].layout, views[i].swap, (uint32_t)w, (uint32_t)h, sizes, &rects);
        float px = (float)rects.bottom.x + 160.5f * (float)rects.bottom.width / 320.0f;
        float py = (float)rects.bottom.y + 220.5f * (float)rects.bottom.height / 240.0f;
        event_mouse(SDL_EVENT_MOUSE_BUTTON_DOWN, id, px, py);
        snx_ctr_input_read(&in);
        event_mouse(SDL_EVENT_MOUSE_BUTTON_UP, id, px, py);
        bool hit = laid && in.touch && in.touch_x == 160 && in.touch_y == 220;
        printf("%-24s layout %d, swap %s: a press on the bottom screen's pixel (160, 220) %s, "
               "the touch (%u, %u): %s\n",
               s->name, views[i].layout, views[i].swap ? "on" : "off", hit ? "touched it" : "missed it", in.touch_x,
               in.touch_y, hit ? "ok" : "FAIL");
        if (!hit) ++failures;
    }
    g_settings.ctr_screen_layout = layout_was;
    g_settings.ctr_screen_swap = swap_was;
    free(s);
    g_settings.ctr_top_resolution = top;
}
/* The screens' layout again through FSR 1 and FXAA: each screen's passes but its last before the window's rendering,
   and its FXAA pass drawn into the window there (the presenter's snx_fx_prepare and snx_fx_draw) */
static void scaled_layout_effects(SnxCtrMemory* m) {
    snprintf(g_settings.upscaler, sizeof g_settings.upscaler, "%s", "fsr1");
    snprintf(g_settings.anti_aliasing, sizeof g_settings.anti_aliasing, "%s", "fxaa");
    scaled_layout(m);
}
/* A game alternates a screen's picture between two framebuffers of one size: a display transfer fills one while the
   other is shown, so the copy the presenter looks up must be the destination's and not the last transfer's. A GPU
   engine's write over one drops its copy alone, and a transfer into it again keeps its own slot */
static void scaled_rotating(SnxCtrMemory* m) {
    static const unsigned char clear[240 * 400 * 4];
    int top = g_settings.ctr_top_resolution;
    g_settings.ctr_top_resolution = 2;
    const uint32_t fb[2] = {SCALED_FB, ROTATE_FB};
    unsigned pitch = 240 * 2, rows = 400;
    SnxCtrDisplayTransfer dt = {240 | 400u << 16, 240 | rows << 16, SNX_CTR_PIX_RGB565 << SNX_CTR_XFER_OUT_FORMAT};
    Scene* s = scaled_scene("rotating destinations", 240, 400, true);
    snx_ctr_phys_write(m, COLOR, clear, sizeof clear);
    snx_ctr_phys_write(m, VERTICES, s->vertices, sizeof s->vertices);
    snx_ctr_phys_write(m, LIST, s->list.w, s->list.n * 4);
    bool ok = record(m, s, &vulkan);
    for (unsigned n = 0; n < 2; ++n) ok = ok && snx_ctr_vk_display_transfer(&dt, COLOR, fb[n]) == SNX_CTR_XFER_OK;
    SnxCtrVkScaled first[2] = {0}, after[2] = {0};
    bool kept = ok && snx_ctr_vk_target_image(fb[0], 240, rows, SNX_CTR_PIX_RGB565, pitch, &first[0]) &&
                snx_ctr_vk_target_image(fb[1], 240, rows, SNX_CTR_PIX_RGB565, pitch, &first[1]);
    /* a fill over the first destination drops its copy, and the second one's stays */
    ok = ok && snx_ctr_vk_fill(fb[0], 0x1000, 0, SNX_CTR_FILL_32BIT | 1) == SNX_CTR_XFER_OK;
    bool dropped = ok && !snx_ctr_vk_target_image(fb[0], 240, rows, SNX_CTR_PIX_RGB565, pitch, &after[0]);
    /* a transfer into it again, while another slot is free: its own copy comes back */
    ok = ok && snx_ctr_vk_display_transfer(&dt, COLOR, fb[0]) == SNX_CTR_XFER_OK;
    bool kept_again = ok && snx_ctr_vk_target_image(fb[0], 240, rows, SNX_CTR_PIX_RGB565, pitch, &after[0]) &&
                      snx_ctr_vk_target_image(fb[1], 240, rows, SNX_CTR_PIX_RGB565, pitch, &after[1]);
    ok = ok && snx_ctr_vk_finish();
    bool same_slot = kept_again && after[0].view == first[0].view;
    bool origins = kept && !first[0].x && !first[0].y && !first[1].x && !first[1].y && kept_again && !after[0].x &&
                   !after[0].y && !after[1].x && !after[1].y;
    bool pass = ok && kept && dropped && kept_again && first[0].scale == 2 && first[1].scale == 2 &&
                after[0].scale == 2 && after[1].scale == 2 && origins && same_slot;
    printf("%-24s both destinations kept at 2x, one dropped by a fill: %s, both kept after a transfer into it: %s, "
           "the same image: %s: %s\n",
           s->name, dropped ? "yes" : "no", kept_again ? "yes" : "no", same_slot ? "yes" : "no", pass ? "ok" : "FAIL");
    if (!pass) ++failures;
    free(s);
    g_settings.ctr_top_resolution = top;
}

/* the top screen's framebuffer, which the render scale takes at its height of 400 rows */
#define SCALED_TARGET_W     240u
#define SCALED_TARGET_H     400u
#define SCALED_TARGET_BYTES (SCALED_TARGET_W * SCALED_TARGET_H * 4)
/* a colour target of the top screen's size at at, its rows from the top, or from y = 0 up when flip is set */
static void screen_target(List* l, uint32_t at, bool flip) {
    put(l, FB_COLOR_ADDR, at / 8);
    put(l, 0x041, f24(SCALED_TARGET_W / 2.0f));
    put(l, 0x043, f24(SCALED_TARGET_H / 2.0f));
    put(l, 0x11e, SCALED_TARGET_W | (SCALED_TARGET_H - 1) << 12 | (flip ? 0 : 1u << 24));
}
/* texture unit 0 over a top screen sized image at address: nearest and repeated, in the PICA format */
static void screen_texture(List* l, uint32_t address, unsigned format) {
    put(l, TEX_MAIN, 1);
    put(l, TEX0_SIZE, SCALED_TARGET_W << 16 | SCALED_TARGET_H);
    put(l, TEX0_PARAMS, 2u << 8 | 2u << 12);
    put(l, TEX0_ADDRESS, address / 8);
    put(l, TEX0_FORMAT, format);
    put(l, TEV_STAGE0, 0x00030003u); /* colour and alpha from texture 0 */
}
/* a colour as a texel of a texture in the format: what the console stores of it, read back */
static void quantised(unsigned format, const uint8_t color[4], uint8_t texel[4]) {
    unsigned char bytes[4];
    snx_ctr_pixel_encode(format, color, bytes);
    snx_ctr_pixel_decode(format, bytes, texel);
}
/* a colour as the word that vertex() takes: red in the top byte */
static uint32_t color_word(const uint8_t color[4]) {
    return (uint32_t)color[0] << 24 | (uint32_t)color[1] << 16 | (uint32_t)color[2] << 8 | color[3];
}
/* Target A is drawn at scale and sampled by B at once, with no CPU step between: B's recording stores A to memory.
   Above scale 1, the texture is made from A's scaled image: B's pixel (i, j) reads texel (i, j) of that image,
   quantised in the format. Texture row j reads image row j, or the last one less j when A is flipped
   (scaled_texture.comp). */
static void scaled_texture_case(SnxCtrMemory* m, const char* name, unsigned pica, unsigned pixel, unsigned scale,
                                bool flip) {
    static const uint8_t inside[4] = {100, 50, 200, 255}, outside[4] = {20, 220, 90, 255};
    static unsigned char clear[SCALED_TARGET_BYTES], zeros[SCALED_TARGET_BYTES], after[SCALED_TARGET_BYTES];
    size_t bpp = snx_ctr_pixel_bytes(pixel);
    /* the rectangle's left and top edges at console coordinate 1/scale, which is the scaled coordinate 1.0 */
    float left = 2.0f / (float)(scale * SCALED_TARGET_W) - 1.0f;
    float top = 2.0f / (float)(scale * SCALED_TARGET_H) - 1.0f;
    uint32_t word = color_word(inside);
    Scene* a = scene(name, pica, 0xff);
    screen_target(&a->list, COLOR, flip);
    a->vertices[0] = vertex(left, top, -0.5f, word, 0, 0);
    a->vertices[1] = vertex(1.0f, top, -0.5f, word, 0, 0);
    a->vertices[2] = vertex(left, 1.0f, -0.5f, word, 0, 0);
    a->vertices[3] = vertex(1.0f, top, -0.5f, word, 0, 0);
    a->vertices[4] = vertex(1.0f, 1.0f, -0.5f, word, 0, 0);
    a->vertices[5] = vertex(left, 1.0f, -0.5f, word, 0, 0);
    a->vertex_count = 6;
    array_draw(a);
    list_end(&a->list);
    for (unsigned p = 0; p < SCALED_TARGET_W * SCALED_TARGET_H; ++p)
        snx_ctr_pixel_encode(pixel, outside, clear + p * bpp);
    setup_clear(m, a);
    bool ok = snx_ctr_phys_write(m, COLOR, clear, SCALED_TARGET_W * SCALED_TARGET_H * bpp) == SNX_CTR_MEM_OK;
    ok = ok && record(m, a, &vulkan); /* A's rectangle, its rendering open */
    Scene* b = scene(name, 0, 0xff);
    screen_target(&b->list, SECOND_COLOR, false);
    screen_texture(&b->list, COLOR, pica);
    float tc = 1.0f / (float)scale;
    b->vertices[0] = vertex(-1.0f, -1.0f, -0.5f, 0xffffffffu, 0, 0);
    b->vertices[1] = vertex(1.0f, -1.0f, -0.5f, 0xffffffffu, tc, 0);
    b->vertices[2] = vertex(-1.0f, 1.0f, -0.5f, 0xffffffffu, 0, tc);
    b->vertices[3] = vertex(1.0f, 1.0f, -0.5f, 0xffffffffu, tc, tc);
    b->vertex_count = 4;
    put(&b->list, SNX_PICA_TOPOLOGY, 1u << 8);
    strip_draw(&b->list, 0);
    list_end(&b->list);
    ok = ok && snx_ctr_phys_write(m, SECOND_COLOR, zeros, SCALED_TARGET_BYTES) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, VERTICES, b->vertices, sizeof b->vertices) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, LIST, b->list.w, b->list.n * 4) == SNX_CTR_MEM_OK;
    ok = ok && record(m, b, &vulkan); /* B's quad samples A */
    ok = snx_ctr_vk_finish() && ok;
    ok = ok && snx_ctr_phys_read(m, SECOND_COLOR, after, SCALED_TARGET_BYTES) == SNX_CTR_MEM_OK;
    unsigned wrong = 0;
    for (unsigned j = 0; j < SCALED_TARGET_H; ++j)
        for (unsigned i = 0; i < SCALED_TARGET_W; ++i) {
            unsigned row = flip ? scale * SCALED_TARGET_H - 1 - j : j;
            uint8_t texel[4];
            unsigned char want[4];
            quantised(pixel, (i >= 1 && row >= 1) ? inside : outside, texel);
            snx_ctr_pixel_encode(SNX_CTR_PIX_RGBA8, texel, want);
            /* B is not flipped, so its row j is memory row H - 1 - j */
            unsigned y = SCALED_TARGET_H - 1 - j;
            size_t at = snx_ctr_morton_offset(i, y, 4) + (size_t)(y & ~7u) * SCALED_TARGET_W * 4;
            wrong += memcmp(after + at, want, sizeof want) != 0;
        }
    bool pass = ok && wrong == 0;
    printf("%-24s %6u of %u pixels differ from the oracle: %s\n", name, wrong, SCALED_TARGET_W * SCALED_TARGET_H,
           pass ? "ok" : "FAIL");
    if (!pass) ++failures;
    free(a);
    free(b);
}
/* target A in each format at scale 1 (the control), 2 and 3, unflipped and flipped */
static void scaled_texture_from_target(SnxCtrMemory* m) {
    static const struct {
        const char* name;
        unsigned pica, pixel;
    } formats[] = {{"RGBA8", SNX_CTR_TEX_RGBA8, SNX_CTR_PIX_RGBA8},
                   {"RGB8", SNX_CTR_TEX_RGB8, SNX_CTR_PIX_RGB8},
                   {"RGB5A1", SNX_CTR_TEX_RGB5A1, SNX_CTR_PIX_RGB5A1},
                   {"RGB565", SNX_CTR_TEX_RGB565, SNX_CTR_PIX_RGB565},
                   {"RGBA4", SNX_CTR_TEX_RGBA4, SNX_CTR_PIX_RGBA4}};
    int top = g_settings.ctr_top_resolution;
    for (size_t f = 0; f < sizeof formats / sizeof *formats; ++f)
        for (unsigned scale = 1; scale <= 3; ++scale)
            for (unsigned flip = 0; flip < 2; ++flip) {
                char name[64];
                snprintf(name, sizeof name, "%s at %ux, %s", formats[f].name, scale, flip ? "flipped" : "unflipped");
                g_settings.ctr_top_resolution = (int)scale;
                scaled_texture_case(m, name, formats[f].pica, formats[f].pixel, scale, flip != 0);
            }
    g_settings.ctr_top_resolution = top;
    snx_ctr_vk_finish();
}

/* a depth format read as the texture format that depth_texel pairs with it: the words depth.comp stores for depth 1.0
   (far) and 0.25 (near), and the pixel format that decodes them (word_texel) */
typedef struct {
    const char* name;
    unsigned depth, texture, pixel; /* FB_DEPTH_FORMAT, the PICA's texture format, transfer.h's pixel format */
    uint32_t far_word, near_word;
} DepthTexel;
static const DepthTexel depth_texels[] = {
    {"D24S8 as RGBA8", 3, 0, SNX_CTR_PIX_RGBA8, 0x00ffffffu, 0x00400000u},
    {"D24 as RGB8", 2, 1, SNX_CTR_PIX_RGB8, 0x00ffffffu, 0x00400000u},
    {"D16 as RGB5A1", 0, 2, SNX_CTR_PIX_RGB5A1, 0xffffu, 0x4000u},
    {"D16 as RGB565", 0, 3, SNX_CTR_PIX_RGB565, 0xffffu, 0x4000u},
    {"D16 as RGBA4", 0, 4, SNX_CTR_PIX_RGBA4, 0xffffu, 0x4000u},
};
/* the texel of a depth word read in a texture of pixel format pixel: the word's low bytes, least significant first,
   decoded by the CPU reference (snx_ctr_pixel_decode), so the oracle does not share the GPU's decode */
static void word_texel(unsigned pixel, uint32_t word, uint8_t texel[4]) {
    unsigned char bytes[4];
    for (unsigned k = 0; k < snx_ctr_pixel_bytes(pixel); ++k) bytes[k] = (unsigned char)(word >> (8 * k));
    snx_ctr_pixel_decode(pixel, bytes, texel);
}
/* A texture made from a scaled target's depth buffer keeps the render scale. Target A is a top screen sized colour
   target with a D24S8 depth buffer, drawn at scale: the buffer is cleared to 1.0, and a rectangle at 0.25 covers the
   scaled columns and rows from 1 on, its left and top edges at scaled coordinate 1.0 (as scaled_texture_case does).
   B samples the buffer as RGBA8 with nearest filtering, so B's console pixel (i, j) reads the scaled texel (i, j) of
   A's depth image, or row 400 * scale - 1 - j when A is flipped. The texel is the word depth.comp stores for the
   depth, its stencil in the top byte, read as RGBA8: 0x00ffffff is (0, 255, 255, 255), and 0.25 stores 0x00400000
   (0.25 * 16777215 rounds to 4194304), which is (0, 64, 0, 0). At scale 1 the texture is the 1x words the store
   wrote, the control. The other rows of depth_texels read their words in their own texture formats the same way. */
static void scaled_depth_texture_case(SnxCtrMemory* m, const char* name, const DepthTexel* t, unsigned scale,
                                      bool flip) {
    static unsigned char clear[SCALED_TARGET_BYTES], zeros[SCALED_TARGET_BYTES], after[SCALED_TARGET_BYTES];
    uint8_t far_texel[4], near_texel[4];
    unsigned bpp = snx_ctr_pixel_bytes(t->pixel);
    word_texel(t->pixel, t->far_word, far_texel);
    word_texel(t->pixel, t->near_word, near_texel);
    for (unsigned p = 0; p < SCALED_TARGET_W * SCALED_TARGET_H; ++p)
        for (unsigned k = 0; k < bpp; ++k)
            clear[p * bpp + k] = (unsigned char)(t->far_word >> (8 * k)); /* depth 1.0 */
    /* the rectangle's left and top edges at console coordinate 1/scale, which is the scaled coordinate 1.0 */
    float left = 2.0f / (float)(scale * SCALED_TARGET_W) - 1.0f;
    float top = 2.0f / (float)(scale * SCALED_TARGET_H) - 1.0f;
    Scene* a = scene(name, 0, 0xff);
    screen_target(&a->list, COLOR, flip);
    depth_target(&a->list, t->depth); /* the depth buffer at DEPTH, written */
    put(&a->list, 0x107, 0x1041u); /* depth test LESS, and the write on */
    a->vertices[0] = vertex(left, top, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[1] = vertex(1.0f, top, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[2] = vertex(left, 1.0f, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[3] = vertex(1.0f, top, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[4] = vertex(1.0f, 1.0f, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[5] = vertex(left, 1.0f, -0.25f, 0xffffffffu, 0, 0);
    a->vertex_count = 6;
    array_draw(a);
    list_end(&a->list);
    setup_clear(m, a);
    bool ok = snx_ctr_phys_write(m, COLOR, zeros, SCALED_TARGET_BYTES) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, DEPTH, clear, SCALED_TARGET_W * SCALED_TARGET_H * bpp) == SNX_CTR_MEM_OK;
    ok = ok && record(m, a, &vulkan); /* A's rectangle over the cleared depth, its rendering open */
    Scene* b = scene(name, 0, 0xff);
    screen_target(&b->list, SECOND_COLOR, false);
    /* A's depth buffer, read as the texture */
    screen_texture(&b->list, DEPTH, t->texture);
    float tc = 1.0f / (float)scale;
    b->vertices[0] = vertex(-1.0f, -1.0f, -0.5f, 0xffffffffu, 0, 0);
    b->vertices[1] = vertex(1.0f, -1.0f, -0.5f, 0xffffffffu, tc, 0);
    b->vertices[2] = vertex(-1.0f, 1.0f, -0.5f, 0xffffffffu, 0, tc);
    b->vertices[3] = vertex(1.0f, 1.0f, -0.5f, 0xffffffffu, tc, tc);
    b->vertex_count = 4;
    put(&b->list, SNX_PICA_TOPOLOGY, 1u << 8);
    strip_draw(&b->list, 0);
    list_end(&b->list);
    ok = ok && snx_ctr_phys_write(m, SECOND_COLOR, zeros, SCALED_TARGET_BYTES) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, VERTICES, b->vertices, sizeof b->vertices) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, LIST, b->list.w, b->list.n * 4) == SNX_CTR_MEM_OK;
    ok = ok && record(m, b, &vulkan); /* B's quad samples A's depth buffer */
    ok = snx_ctr_vk_finish() && ok;
    ok = ok && snx_ctr_phys_read(m, SECOND_COLOR, after, SCALED_TARGET_BYTES) == SNX_CTR_MEM_OK;
    unsigned wrong = 0;
    for (unsigned j = 0; j < SCALED_TARGET_H; ++j)
        for (unsigned i = 0; i < SCALED_TARGET_W; ++i) {
            /* B's row j reads texel row j, which is image row j, or the last one less j when A is flipped */
            unsigned row = flip ? scale * SCALED_TARGET_H - 1 - j : j;
            unsigned char want[4];
            snx_ctr_pixel_encode(SNX_CTR_PIX_RGBA8, (i >= 1 && row >= 1) ? near_texel : far_texel, want);
            /* B is not flipped, so its row j is memory row H - 1 - j */
            unsigned y = SCALED_TARGET_H - 1 - j;
            size_t at = snx_ctr_morton_offset(i, y, 4) + (size_t)(y & ~7u) * SCALED_TARGET_W * 4;
            wrong += memcmp(after + at, want, sizeof want) != 0;
        }
    bool pass = ok && wrong == 0;
    printf("%-24s %6u of %u pixels differ from the oracle: %s\n", name, wrong, SCALED_TARGET_W * SCALED_TARGET_H,
           pass ? "ok" : "FAIL");
    if (!pass) ++failures;
    free(a);
    free(b);
}
/* target A at render scale 1 (the control), 2 and 3, unflipped and flipped, for each row of depth_texels */
static void scaled_texture_from_depth(SnxCtrMemory* m) {
    int top = g_settings.ctr_top_resolution;
    for (size_t t = 0; t < sizeof depth_texels / sizeof *depth_texels; ++t)
        for (unsigned scale = 1; scale <= 3; ++scale)
            for (unsigned flip = 0; flip < 2; ++flip) {
                char name[64];
                snprintf(name, sizeof name, "%s at %ux, %s", depth_texels[t].name, scale,
                         flip ? "flipped" : "unflipped");
                g_settings.ctr_top_resolution = (int)scale;
                scaled_depth_texture_case(m, name, &depth_texels[t], scale, flip != 0);
            }
    g_settings.ctr_top_resolution = top;
    snx_ctr_vk_finish();
}

/* B takes A's depth at the render scale when it opens, not the buffer's 1x reload. A is cleared to 1.0, with a
   rectangle at 0.25 whose left and top edges are at scaled coordinate 1.0. B is cleared to green by a full quad
   with the depth test ALWAYS and no write. A full quad of red at 0.5, with LESS and no write, then shows where
   the rectangle is not. C samples B over the top-left 1/scale, so its console pixel (i, j) is B's texel (i, j):
   green where i >= 1 and j >= 1, red elsewhere. Without the change the 1x reload gives the far depth to the texels
   with i < scale or j < scale, so those with i and j >= 1 show red: 637 pixels at scale 2, 1272 at scale 3. */
static void scaled_shared_depth_case(SnxCtrMemory* m, const char* name, unsigned scale) {
    static const uint8_t red[4] = {255, 0, 0, 255}, green[4] = {0, 255, 0, 255};
    static unsigned char clear[SCALED_TARGET_BYTES], zeros[SCALED_TARGET_BYTES], after[SCALED_TARGET_BYTES];
    for (unsigned p = 0; p < SCALED_TARGET_W * SCALED_TARGET_H; ++p)
        clear[p * 4] = clear[p * 4 + 1] = clear[p * 4 + 2] = 0xff; /* depth 1.0, stencil 0 */
    /* the rectangle's left and top edges at console coordinate 1/scale, which is the scaled coordinate 1.0 */
    float left = 2.0f / (float)(scale * SCALED_TARGET_W) - 1.0f;
    float top = 2.0f / (float)(scale * SCALED_TARGET_H) - 1.0f;
    Scene* a = scene(name, 0, 0xff);
    screen_target(&a->list, COLOR, false);
    depth_target(&a->list, 3); /* D24S8 at DEPTH, written */
    put(&a->list, 0x107, 0xf00 | 1u | 4u << 4 | 1u << 12); /* colour written, depth test LESS, and the write on */
    a->vertices[0] = vertex(left, top, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[1] = vertex(1.0f, top, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[2] = vertex(left, 1.0f, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[3] = vertex(1.0f, top, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[4] = vertex(1.0f, 1.0f, -0.25f, 0xffffffffu, 0, 0);
    a->vertices[5] = vertex(left, 1.0f, -0.25f, 0xffffffffu, 0, 0);
    a->vertex_count = 6;
    array_draw(a);
    list_end(&a->list);
    setup_clear(m, a);
    bool ok = snx_ctr_phys_write(m, COLOR, zeros, SCALED_TARGET_BYTES) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, DEPTH, clear, SCALED_TARGET_BYTES) == SNX_CTR_MEM_OK;
    ok = ok && record(m, a, &vulkan); /* A's rectangle over the cleared depth, its rendering open */
    Scene* b = scene(name, 0, 0xff);
    screen_target(&b->list, SECOND_COLOR, false);
    depth_target(&b->list, 3);
    uint32_t green_word = color_word(green), red_word = color_word(red);
    b->vertices[0] = vertex(-1.0f, -1.0f, -0.5f, green_word, 0, 0);
    b->vertices[1] = vertex(1.0f, -1.0f, -0.5f, green_word, 0, 0);
    b->vertices[2] = vertex(-1.0f, 1.0f, -0.5f, green_word, 0, 0);
    b->vertices[3] = vertex(1.0f, 1.0f, -0.5f, green_word, 0, 0);
    b->vertices[4] = vertex(-1.0f, -1.0f, -0.5f, red_word, 0, 0);
    b->vertices[5] = vertex(1.0f, -1.0f, -0.5f, red_word, 0, 0);
    b->vertices[6] = vertex(-1.0f, 1.0f, -0.5f, red_word, 0, 0);
    b->vertices[7] = vertex(1.0f, 1.0f, -0.5f, red_word, 0, 0);
    b->vertex_count = 8;
    put(&b->list, SNX_PICA_TOPOLOGY, 1u << 8);
    put(&b->list, 0x107, 0xf00 | 1u | 1u << 4); /* the clear: colour written, depth test ALWAYS, and no write */
    strip_draw(&b->list, 0);
    put(&b->list, 0x107, 0xf00 | 1u | 4u << 4); /* colour written, depth test LESS, and no write */
    strip_draw(&b->list, 4);
    list_end(&b->list);
    ok = ok && snx_ctr_phys_write(m, VERTICES, b->vertices, sizeof b->vertices) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, LIST, b->list.w, b->list.n * 4) == SNX_CTR_MEM_OK;
    ok = ok && record(m, b, &vulkan); /* B's quads: its depth is A's, copied when B opened */
    Scene* c = scene(name, 0, 0xff);
    screen_target(&c->list, THIRD_COLOR, false);
    screen_texture(&c->list, SECOND_COLOR, 0); /* RGBA8 over B's colour */
    float tc = 1.0f / (float)scale;
    c->vertices[0] = vertex(-1.0f, -1.0f, -0.5f, 0xffffffffu, 0, 0);
    c->vertices[1] = vertex(1.0f, -1.0f, -0.5f, 0xffffffffu, tc, 0);
    c->vertices[2] = vertex(-1.0f, 1.0f, -0.5f, 0xffffffffu, 0, tc);
    c->vertices[3] = vertex(1.0f, 1.0f, -0.5f, 0xffffffffu, tc, tc);
    c->vertex_count = 4;
    put(&c->list, SNX_PICA_TOPOLOGY, 1u << 8);
    strip_draw(&c->list, 0);
    list_end(&c->list);
    ok = ok && snx_ctr_phys_write(m, VERTICES, c->vertices, sizeof c->vertices) == SNX_CTR_MEM_OK;
    ok = ok && snx_ctr_phys_write(m, LIST, c->list.w, c->list.n * 4) == SNX_CTR_MEM_OK;
    ok = ok && record(m, c, &vulkan); /* C samples B's scaled colour */
    ok = snx_ctr_vk_finish() && ok;
    ok = ok && snx_ctr_phys_read(m, THIRD_COLOR, after, SCALED_TARGET_BYTES) == SNX_CTR_MEM_OK;
    unsigned wrong = 0;
    for (unsigned j = 0; j < SCALED_TARGET_H; ++j)
        for (unsigned i = 0; i < SCALED_TARGET_W; ++i) {
            unsigned char want[4];
            snx_ctr_pixel_encode(SNX_CTR_PIX_RGBA8, (i >= 1 && j >= 1) ? green : red, want);
            /* C is not flipped, so its row j is memory row H - 1 - j */
            unsigned y = SCALED_TARGET_H - 1 - j;
            size_t at = snx_ctr_morton_offset(i, y, 4) + (size_t)(y & ~7u) * SCALED_TARGET_W * 4;
            wrong += memcmp(after + at, want, sizeof want) != 0;
        }
    bool pass = ok && wrong == 0;
    printf("%-24s %6u of %u pixels differ from the oracle: %s\n", name, wrong, SCALED_TARGET_W * SCALED_TARGET_H,
           pass ? "ok" : "FAIL");
    if (!pass) ++failures;
    free(a);
    free(b);
    free(c);
}
/* the shared depth buffer at render scale 1 (the control), 2 and 3 */
static void scaled_shared_depth(SnxCtrMemory* m) {
    int top = g_settings.ctr_top_resolution;
    for (unsigned scale = 1; scale <= 3; ++scale) {
        char name[64];
        snprintf(name, sizeof name, "shared D24S8 at %ux", scale);
        g_settings.ctr_top_resolution = (int)scale;
        scaled_shared_depth_case(m, name, scale);
    }
    g_settings.ctr_top_resolution = top;
    snx_ctr_vk_finish();
}
/* Shadow maps (Azahar's sw_framebuffer.cpp DrawShadowMapPixel and sw_rasterizer.cpp TextureColor): the colour buffer
   of a shadow pass holds one word per pixel, its bytes the depth from the most significant byte, then the strength */
#define TEX_CUBE   0x086
#define TEX_SHADOW 0x08b
#define OM_MODE    0x100
#define FB_SHADOW  0x130
#define F16_ONE    0x3c00u
#define F16_TWO    0x4000u
/* a strip over the whole target at depth 0.5 in one colour, drawn. Texture coordinate 0 goes from 0.25 to 0.75 over
   it (spread), else stays at 0, and its w is w */
static void shadow_quad(Scene* s, uint32_t rgba, bool spread, float w) {
    s->vertex_count = 4;
    for (unsigned k = 0; k < 4; ++k) {
        float x = k & 1 ? 1.0f : -1.0f, y = k & 2 ? 1.0f : -1.0f;
        float u = spread ? ((k & 1) ? 0.75f : 0.25f) : 0.0f, v = spread ? ((k & 2) ? 0.75f : 0.25f) : 0.0f;
        s->vertices[k] = vertex(x, y, -0.5f, rgba, u, v);
        s->vertices[k].tc[2] = w;
    }
    put(&s->list, SNX_PICA_TOPOLOGY, 1u << 8);
    strip_draw(&s->list, 0);
}
/* a colour buffer that holds a shadow map with the same word in every pixel */
static void shadow_fill(unsigned char* map, uint32_t depth, uint8_t strength) {
    for (unsigned i = 0; i < W * H * 4; i += 4) {
        map[i] = (unsigned char)(depth >> 16);
        map[i + 1] = (unsigned char)(depth >> 8);
        map[i + 2] = (unsigned char)depth;
        map[i + 3] = strength;
    }
}
/* A shadow pass at depth 0.5 (0x7fffff) with strength green, over a map of map_depth and map_strength. Where the map
   is nearer, the word stays. Else a fragment with no strength writes its depth, and one with strength writes it over
   (constant + linear * its depth / the map's) if that is less than the map's. FB_SHADOW holds constant in its low
   half and linear in its high half, as float16 */
static void shadow_pass(SnxCtrMemory* m, const char* name, uint8_t green, uint32_t factors, uint32_t map_depth,
                        uint8_t map_strength, uint32_t depth, uint8_t strength) {
    static unsigned char start[W * H * 4], sw[W * H * 4], gpu[W * H * 4], unused[W * H * 4];
    Scene* s = scene(name, 0, 0xff);
    put(&s->list, OM_MODE, 3);
    put(&s->list, FB_SHADOW, factors);
    shadow_quad(s, (uint32_t)green << 16 | 0xffu, false, 0.0f);
    list_end(&s->list);
    shadow_fill(start, map_depth, map_strength);
    setup(m, s, start, start);
    bool a = draw(m, s, &software, sw, unused);
    setup(m, s, start, start);
    bool b = draw(m, s, &vulkan, gpu, unused);
    if (drew(s, a, b)) {
        unsigned off = 0;
        for (unsigned i = 0; i < W * H; ++i) {
            const unsigned char* words[2] = {sw + i * 4, gpu + i * 4};
            for (unsigned k = 0; k < 2; ++k) {
                uint32_t z = (uint32_t)words[k][0] << 16 | (uint32_t)words[k][1] << 8 | words[k][2];
                uint32_t e = z > depth ? z - depth : depth - z;
                off += e > 16384u || words[k][3] != strength;
            }
        }
        printf("%-24s %4u words off: %s\n", s->name, off, off ? "FAIL" : "ok");
        if (off) ++failures;
    }
    free(s);
}
/* A draw into a second buffer, a shadow pass into the first, a draw into the second again, then a draw into the first.
   The last draw loads, as an attachment, the image that the shadow pass wrote as storage, with only a pass into
   another target between them. Both renderers must leave the last draw's colour, RGBA8 bytes a b g r */
static void shadow_then_colour(SnxCtrMemory* m) {
    static unsigned char start[W * H * 4], sw[W * H * 4], gpu[W * H * 4], unused[W * H * 4];
    static const unsigned char colour[4] = {0x40, 0x30, 0x20, 0x10};
    Scene* s = scene("shadow pass, then colour", 0, 0xff);
    put(&s->list, FB_COLOR_ADDR, SECOND_COLOR / 8);
    shadow_quad(s, 0x10203040u, false, 0.0f);
    put(&s->list, FB_COLOR_ADDR, COLOR / 8);
    put(&s->list, OM_MODE, 3);
    put(&s->list, FB_SHADOW, F16_ONE);
    strip_draw(&s->list, 0);
    put(&s->list, OM_MODE, 0);
    put(&s->list, FB_COLOR_ADDR, SECOND_COLOR / 8);
    strip_draw(&s->list, 0);
    put(&s->list, FB_COLOR_ADDR, COLOR / 8);
    strip_draw(&s->list, 0);
    list_end(&s->list);
    shadow_fill(start, 0xffffffu, 255);
    setup(m, s, start, start);
    bool a = draw(m, s, &software, sw, unused);
    setup(m, s, start, start);
    bool b = draw(m, s, &vulkan, gpu, unused);
    if (drew(s, a, b)) {
        unsigned off = 0;
        for (unsigned i = 0; i < W * H * 4; ++i) off += (sw[i] != colour[i % 4]) + (gpu[i] != colour[i % 4]);
        printf("%-24s %4u bytes off: %s\n", s->name, off, off ? "FAIL" : "ok");
        if (off) ++failures;
    }
    free(s);
}
/* A shadow map read by texture unit 0, each texel at depth 0.5. The 2D map has strength 200. The cube's +Z face, the
   one its direction (0, 0, w) picks, has strength 200 and its other faces 50. A fragment in front of the map gets the
   strength in every channel, one behind it 0. tex_shadow is TEX_SHADOW: bit 0 orthographic, bits 1-23 the bias. The
   colour buffer starts at 0x5a, so a draw that writes nothing fails. The 2D map is read away from its edges, where
   the GPU's four texels and the reference's one differ */
static void shadow_sample(SnxCtrMemory* m, const char* name, bool cube, float w, uint32_t tex_shadow,
                          uint8_t expected) {
    static unsigned char start[W * H * 4], sw[W * H * 4], gpu[W * H * 4], unused[W * H * 4];
    Scene* s = scene(name, 0, 0xff);
    for (unsigned i = 0; i < sizeof s->texture; i += 4) {
        s->texture[i] = 0x7f;
        s->texture[i + 1] = 0xff;
        s->texture[i + 2] = 0xff;
        s->texture[i + 3] = i < 0x1000 ? 200 : 50;
    }
    texture_state(&s->list, TEXTURE, 16);
    put(&s->list, TEX0_PARAMS, 2u << 8 | 2u << 12 | (cube ? 4u : 2u) << 28);
    put(&s->list, TEX_SHADOW, tex_shadow);
    if (cube) { /* +X, then -X +Y -Y +Z -Z in TEX_CUBE's order */
        put(&s->list, TEX0_ADDRESS, (TEXTURE + 0x1000) / 8);
        for (unsigned f = 0; f < 5; ++f)
            put(&s->list, TEX_CUBE + f, (f == 3 ? TEXTURE : TEXTURE + 0x1000) / 8 & 0x3fffff);
    }
    put(&s->list, 0x201, 0xbff);       /* texture coordinate 0 with three components */
    put(&s->list, 0x052, 0x1f100d0cu); /* o2.z is its w */
    shadow_quad(s, 0xffffffffu, !cube, w);
    list_end(&s->list);
    memset(start, 0x5a, sizeof start);
    setup(m, s, start, start);
    bool a = draw(m, s, &software, sw, unused);
    setup(m, s, start, start);
    bool b = draw(m, s, &vulkan, gpu, unused);
    if (drew(s, a, b)) {
        unsigned off = 0;
        for (unsigned i = 0; i < W * H * 4; ++i) off += (sw[i] != expected) + (gpu[i] != expected);
        printf("%-24s %4u bytes off: %s\n", s->name, off, off ? "FAIL" : "ok");
        if (off) ++failures;
    }
    free(s);
}
static void shadows(SnxCtrMemory* m) {
    shadow_pass(m, "shadow pass, opaque", 0, F16_ONE, 0xffffffu, 255, 0x7fffffu, 255);
    shadow_pass(m, "shadow pass, soft", 128, F16_ONE, 0xffffffu, 255, 0xffffffu, 128);
    /* 128 / (2 + 1 * 0x7fffff / 0xffffff) = 51.2 */
    shadow_pass(m, "shadow pass, scaled", 128, F16_ONE << 16 | F16_TWO, 0xffffffu, 255, 0xffffffu, 51);
    shadow_pass(m, "shadow pass, map nearer", 0, F16_ONE, 0x400000u, 90, 0x400000u, 90);
    shadow_pass(m, "shadow pass, map weaker", 128, F16_ONE, 0xffffffu, 100, 0xffffffu, 100);
    shadow_then_colour(m);
    shadow_sample(m, "shadow 2D, lit", false, 0.25f, 1, 200);
    shadow_sample(m, "shadow 2D, shadowed", false, 0.75f, 1, 0);
    /* perspective, at w 1: 0xffffff less the bias 0xa00000 is nearer than the map */
    shadow_sample(m, "shadow 2D, biased", false, 1.0f, 0x500000u << 1, 200);
    shadow_sample(m, "shadow cube, lit", true, 0.25f, 1, 200);
    shadow_sample(m, "shadow cube, shadowed", true, 0.75f, 1, 0);
}

/* count writes of one register in a burst: the header's extra count, the values after it, the list kept 8-byte
   aligned */
static void put_run(List* l, uint32_t id, const uint32_t* values, unsigned count) {
    l->w[l->n++] = values[0];
    l->w[l->n++] = id | 15u << 16 | (count - 1) << 20;
    for (unsigned i = 1; i < count; ++i) l->w[l->n++] = values[i];
    if (l->n & 1) l->w[l->n++] = 0;
}
/* lighting LUT table's 256 entries, a ramp from start, its differences to the next entry (12-bit values, 11-bit
   differences of 1.0 / 2047 with a sign) */
static void lighting_lut(List* l, unsigned table, unsigned start, int step) {
    uint32_t values[256];
    for (unsigned i = 0; i < 256; ++i) {
        int v = (int)start + step * (int)i;
        v = v < 0 ? 0 : v > 4095 ? 4095 : v;
        uint32_t diff = (uint32_t)(step < 0 ? -step : step) * 2047u / 4095u;
        values[i] = (uint32_t)v | diff << 12 | (step < 0 ? 1u << 23 : 0);
    }
    put(l, 0x1c5, table << 8);
    put_run(l, 0x1c8, values, 256);
}
/* the outputs as lighting reads them: o1 the normal quaternion (the vertex colours, normalized by the shader), o2 the
   view (the texture coordinate, z 0), the primary colour none (1.0) */
static void lit_outputs(List* l) {
    put(l, 0x051, 0x07060504u);
    put(l, 0x052, 0x1f141312u);
}
/* lighting with more of its parts, which the generated fragment shader and the stand-in (ctr_stand_in=2) must both
   draw as the reference does */
static void lit_scene(SnxCtrMemory* m) {
    {
        Scene* s = scene("lighting, two lights and LUTs", 0, 0xff);
        lit_outputs(&s->list);
        /* light 0: positional at (0, 0, 1), its spot down -z, distance and spot attenuation; light 1: directional,
           two-sided, the geometric factor on specular 0. Each in the slot of its number: the generated shader reads
           a LUT input's two-sided switch by the light's number, as Azahar's does, where the reference reads the
           slot's light */
        put(&s->list, 0x140 + 0, 600u << 20 | 300u << 10 | 100u);
        put(&s->list, 0x140 + 1, 200u << 20 | 800u << 10 | 400u);
        put(&s->list, 0x140 + 2, 1023u << 20 | 700u << 10 | 300u);
        put(&s->list, 0x140 + 3, 60u << 20 | 40u << 10 | 30u);
        put(&s->list, 0x140 + 4, 0);
        put(&s->list, 0x140 + 5, 0x3c00); /* z 1.0 */
        put(&s->list, 0x140 + 6, 0);
        put(&s->list, 0x140 + 7, 0x1801); /* spot z -1.0 */
        put(&s->list, 0x140 + 9, 0);
        put(&s->list, 0x140 + 10, 0);       /* distance bias 0 */
        put(&s->list, 0x140 + 11, 0x3e000); /* distance scale 0.5 */
        put(&s->list, 0x150 + 0, 300u << 20 | 300u << 10 | 900u);
        put(&s->list, 0x150 + 1, 100u << 20 | 100u << 10 | 100u);
        put(&s->list, 0x150 + 2, 200u << 20 | 500u << 10 | 1000u);
        put(&s->list, 0x150 + 3, 0);
        put(&s->list, 0x150 + 4, 0xb800u << 16 | 0x3800u); /* direction (0.5, -0.5, 1.0) */
        put(&s->list, 0x150 + 5, 0x3c00);
        put(&s->list, 0x150 + 9, 1 | 2 | 4);
        put(&s->list, 0x1c0, 20u << 20 | 20u << 10 | 40u); /* global ambient */
        put(&s->list, 0x1c2, 1);                           /* two lights */
        put(&s->list, 0x1d9, 0u | 1u << 4);                /* slot 0 light 0, slot 1 light 1 */
        /* configuration 4 (d0, d1, sp, rr, rg, rb), Fresnel into the primary alpha, highlights clamped */
        put(&s->list, 0x1c3, 4u << 4 | 1u << 2 | 1u << 27);
        /* shadows off; spot and distance attenuation for light 0 only; Fresnel off */
        put(&s->list, 0x1c4, 0xfeu << 24 | 1u << 19 | 0xfeu << 8 | 0xffu);
        put(&s->list, 0x1d0, 1u << 5 | 1u << 25); /* d1 and rr signed, the rest absolute */
        put(&s->list, 0x1d1, 0u | 2u << 4 | 4u << 8 | 1u << 16 | 3u << 20 | 2u << 24); /* the LUTs' inputs */
        put(&s->list, 0x1d2, 1u | 6u << 24);                                           /* d0 x2, rr x0.25 */
        lighting_lut(&s->list, 0, 400, 14);
        lighting_lut(&s->list, 1, 4000, -12);
        lighting_lut(&s->list, 4, 1000, 9);
        lighting_lut(&s->list, 5, 2000, 5);
        lighting_lut(&s->list, 6, 3000, -7);
        lighting_lut(&s->list, 8, 0, 16);      /* light 0's spot */
        lighting_lut(&s->list, 16, 4095, -10); /* light 0's distance */
        put(&s->list, 0x1c6, 0);               /* lighting on */
        /* stage 0: primary fragment + secondary fragment, alpha the primary fragment's */
        put(&s->list, 0x0c0, 1u | 2u << 4 | 1u << 16);
        put(&s->list, 0x0c2, 2u | 0u << 16);
        triangles(s, 6);
        array_draw(s);
        run(m, s);
        free(s);
    }
}
/* that, and fog */
static void lit_scenes(SnxCtrMemory* m) {
    lit_scene(m);
    {
        Scene* s = scene("fog", 0, 0xff);
        uint32_t values[128];
        for (unsigned i = 0; i < 128; ++i) {
            /* the factor 1.0 at depth 0 down to near 0 at 1, its difference to the next entry */
            uint32_t value = 2047u - i * 16u, diff = (uint32_t)(-16) & 0x1fff;
            values[i] = value << 13 | diff;
        }
        put(&s->list, 0x0e0, 5);                                /* fog */
        put(&s->list, 0x0e1, 0x20u << 16 | 0x80u << 8 | 0xc0u); /* its colour */
        put(&s->list, 0x0e6, 0);
        put_run(&s->list, 0x0e8, values, 128);
        triangles(s, 6);
        array_draw(s);
        run(m, s);
        free(s);
    }
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
    if (!snx_ctr_vk()->logic_op)
        printf("logic op xor: the driver has no logic ops, not compared\n");
    else {
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
        for (unsigned i = 0; i < 30; ++i) snx_st_le16(s->indices + i * 2, (uint16_t)(next() % s->vertex_count));
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
        immediate_vertices(&s->list, 9);
        run(m, s);
        free(s);
    }
    {
        /* point mode, three vertices a run: their triangle, then the same one (vertex 1 kept) with the winding flag */
        const uint32_t point[26] = {setemit(0, false, false),
                                    op1(0x13, 0, 0, 0, 0),
                                    op1(0x13, 1, 1, 0, 0),
                                    op1(0x13, 2, 2, 0, 0),
                                    EMIT,
                                    setemit(1, false, false),
                                    op1(0x13, 0, 3, 0, 0),
                                    op1(0x13, 1, 4, 0, 0),
                                    op1(0x13, 2, 5, 0, 0),
                                    EMIT,
                                    setemit(2, true, false),
                                    op1(0x13, 0, 6, 0, 0),
                                    op1(0x13, 1, 7, 0, 0),
                                    op1(0x13, 2, 8, 0, 0),
                                    EMIT,
                                    setemit(0, false, false),
                                    op1(0x13, 0, 6, 0, 0),
                                    op1(0x13, 1, 7, 0, 0),
                                    op1(0x13, 2, 8, 0, 0),
                                    EMIT,
                                    setemit(2, true, true),
                                    op1(0x13, 0, 0, 0, 0),
                                    op1(0x13, 1, 1, 0, 0),
                                    op1(0x13, 2, 2, 0, 0),
                                    EMIT,
                                    END};
        /* fixed primitive mode: three vertices of three outputs into c16 to c24 */
        const uint32_t fixed[16] = {setemit(0, false, false),
                                    op1(0x13, 0, 0x30, 0, 0),
                                    op1(0x13, 1, 0x31, 0, 0),
                                    op1(0x13, 2, 0x32, 0, 0),
                                    EMIT,
                                    setemit(1, false, false),
                                    op1(0x13, 0, 0x33, 0, 0),
                                    op1(0x13, 1, 0x34, 0, 0),
                                    op1(0x13, 2, 0x35, 0, 0),
                                    EMIT,
                                    setemit(2, true, false),
                                    op1(0x13, 0, 0x36, 0, 0),
                                    op1(0x13, 1, 0x37, 0, 0),
                                    op1(0x13, 2, 0x38, 0, 0),
                                    EMIT,
                                    END};
        Scene* s = scene("geometry shader, points", 0, 0xff);
        geometry_shader(&s->list, 2, 0, 8, point, 26);
        triangles(s, 4);
        array_draw(s);
        run(m, s);
        free(s);
        s = scene("geometry shader, culled", 0, 0xff);
        put(&s->list, 0x040, 1);
        geometry_shader(&s->list, 2, 0, 8, point, 26);
        triangles(s, 10);
        array_draw(s);
        run(m, s);
        free(s);
        s = scene("geometry shader, fixed", 0, 0xff);
        geometry_shader(&s->list, 2, 2u | 2u << 8 | 2u << 12 | 16u << 16, 1u << 8, fixed, 16);
        triangles(s, 8);
        for (unsigned i = 0; i < 30; ++i) snx_st_le16(s->indices + i * 2, (uint16_t)(next() % s->vertex_count));
        put(&s->list, 0x227, (INDICES - VERTICES) | 1u << 31);
        put(&s->list, 0x228, 30);
        put(&s->list, SNX_PICA_TRIGGER_DRAW_INDEXED, 1);
        run(m, s);
        free(s);
        s = scene("geometry shader, immediate", 0, 0xff);
        geometry_shader(&s->list, 2, 0, 8, point, 26);
        immediate_vertices(&s->list, 9);
        run(m, s);
        free(s);
        /* one run that emits a small triangle 180 * 180 times, more than one recording's uploads hold */
        const uint32_t many[19] = {0x29u << 26 | 17u << 10,            /* loop i0, to 17 */
                                   0x29u << 26 | 1u << 22 | 16u << 10, /* loop i1, to 16 */
                                   setemit(0, false, false),
                                   op1(0x13, 0, 0, 0, 0),
                                   op1(0x13, 1, 1, 0, 0),
                                   op1(0x13, 2, 2, 0, 0),
                                   EMIT,
                                   setemit(1, false, false),
                                   op1(0x13, 0, 3, 0, 0),
                                   op1(0x13, 1, 4, 0, 0),
                                   op1(0x13, 2, 5, 0, 0),
                                   EMIT,
                                   setemit(2, true, false),
                                   op1(0x13, 0, 6, 0, 0),
                                   op1(0x13, 1, 7, 0, 0),
                                   op1(0x13, 2, 8, 0, 0),
                                   EMIT,
                                   0x21u << 26, /* nop: the two loops end apart */
                                   END};
        s = scene("geometry shader, many triangles", 0, 0xff);
        geometry_shader(&s->list, 2, 0, 8, many, 19);
        put(&s->list, SNX_PICA_GS + SNX_PICA_SH_INT, 179u | 1u << 16); /* 180 passes, step 1 */
        put(&s->list, SNX_PICA_GS + SNX_PICA_SH_INT + 1, 179u | 1u << 16);
        s->vertices[0] = vertex(-0.1f, -0.1f, -0.5f, 0xff8040ffu, 0, 0);
        s->vertices[1] = vertex(0.1f, -0.1f, -0.5f, 0x40ff80ffu, 1, 0);
        s->vertices[2] = vertex(0.0f, 0.1f, -0.5f, 0x8040ffffu, 0, 1);
        s->vertex_count = 3;
        array_draw(s);
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
    lit_scenes(m);
    commands_after_draw(m);
    texture_after_draw(m);
    draw_after_index_copy(m);
    index_count_past_ram(m);
    shadows(m);
    draw_after_submit(m);
}

typedef struct {
    const char* flag;
    const char* what;
    void (*run)(SnxCtrMemory* memory);
} Mode;
static const Mode scenes = {NULL, "the scenes", all_scenes};
/* the validation runs record only the commands under test */
static const Mode modes[] = {
    {"--present", "the presented screens", screens},
    {"--scaled-present", "the frames shown from scaled targets", scaled_present},
    {"--scaled-transfer", "the scaled targets' transfers", scaled_transfer},
    {"--scaled-rotating", "the rotating destinations", scaled_rotating},
    {"--scaled-verify", "a destination whose bytes the guest changed", scaled_verify},
    {"--scaled-layout", "the screens' layout at different render scales", scaled_layout},
    {"--scaled-layout-effects", "the screens' layout through FSR 1 and FXAA", scaled_layout_effects},
    {"--after-draw", "the commands after a draw", presented_after_draw},
    {"--texture-after-draw", "the textures after a draw", texture_after_draw},
    {"--draw-after-index-copy", "the indices copied before a draw", draw_after_index_copy},
    {"--draw-after-submit", "a draw after a submission", draw_after_submit},
    {"--async", "the draws with async_pipelines", async_pipelines},
    {"--pipeline-cache", "the pipeline cache kept between runs", pipeline_cache},
    {"--scaled-texture-from-target", "the textures made from scaled targets", scaled_texture_from_target},
    {"--scaled-texture-from-depth", "the textures made from scaled depth buffers", scaled_texture_from_depth},
    {"--scaled-shared-depth", "two targets over one depth buffer at render scale", scaled_shared_depth},
    {"--shadow", "the shadow maps", shadows}};

int main(int argc, char** argv) {
    /* No window on a host that has a display: these checks render offscreen, and the ones that present do it through
       SDL's dummy driver, which is the driver a CI host with no display gives them. A window here takes the desktop's
       focus from whatever the user runs at the time. A caller that names a driver keeps it: ctest sets one for the
       layout check, which needs a window */
    if (!getenv("SDL_VIDEO_DRIVER")) {
#ifdef _WIN32
        _putenv_s("SDL_VIDEO_DRIVER", "dummy");
#else
        setenv("SDL_VIDEO_DRIVER", "dummy", 1);
#endif
    }
    g_model = test_ctr_model(&argc, argv);
    if (g_model == SNX_CTR_NEW) { /* the scenes in the memory only a New 3DS has */
        g_fcram = 0x28000000u;
        g_vram = 0x2c000000u;
        g_vertices = 0x1f000000u;
    }
    int vk_memory = test_ctr_vk_memory(&argc, argv);
    test_ctr_vk_extensions(&argc, argv);
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
    SnxCtrMemory* m = snx_ctr_memory_new(g_model);
    char why[256];
    if (!m) return 1;
    if (!snx_ctr_vk_init(m, false, why, sizeof why)) {
        printf("no Vulkan device (%s): GPU draw checks skipped\n", why);
        snx_ctr_memory_free(m);
        return 77;
    }
    test_ctr_vk_memory_taken(vk_memory);
    printf("drawn through %s\n", snx_ctr_vk()->shader_object ? "shader objects" : "pipelines");
    mode->run(m);
    snx_ctr_vk_transfer_free();
    snx_ctr_vk_y2r_free();
    snx_ctr_present_free();
    snx_ctr_vk_free();
    snx_ctr_memory_free(m);
    printf("3ds vk draw: %s %s the CPU reference\n", mode->what, failures ? "differ from" : "agree with");
    return failures != 0 ? 1 : skips != 0 ? 77 : 0;
}
