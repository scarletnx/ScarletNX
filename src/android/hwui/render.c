/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The libhwui HLE's renderer: a frame's display list (docs/android/HWUI.md) drawn with Vulkan into the SDL window,
   or into an image of its own without one. The CPU reads the list, keeps the transform and clip stack, and makes
   each shape into triangles in device pixels (path.c). The GPU rasterizes them with 4x MSAA. A convex shape is a fan
   drawn directly. A path, a stroke and glyphs are drawn by stencil then cover: their triangles count the winding
   in the stencil, then a rectangle over their bounds draws where it is not zero and clears it again */
#include "android/hwui/render.h"
#include "android/hwui/draw_frag_spv.h"
#include "android/hwui/draw_vert_spv.h"
#include "android/hwui/path.h"
#include "android/hwui/text.h"
#include "android/runtime/jvm.h"
#include "common/gpu/vk/ring.h"
#include "common/gpu/vk/swapchain.h"
#include "common/gpu/vk/window.h"
#include "common/utilities/endian.h"
#include <SDL3/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SLOTS = 2 };

/* a vertex: its position in device pixels and its premultiplied color as bytes R, G, B, A */
typedef struct {
    float x, y;
    unsigned char rgba[4];
} Vertex;

enum {
    PIPE_COLOR,     /* source over, no stencil */
    PIPE_COPY,      /* the source replaces the target (Clear, Src) */
    PIPE_WINDING,   /* stencil only: +1 for each front face, -1 for each back face */
    PIPE_EVEN_ODD,  /* stencil only: its low bit flipped */
    PIPE_SET,       /* stencil only: 1 (a stroke's triangles, which overlap) */
    PIPE_COVER,     /* source over where the stencil is not zero, which it zeroes */
    PIPE_COVER_COPY, /* the same, the source replacing the target */
    PIPE_COUNT
};

typedef struct {
    uint8_t pipe;
    VkRect2D scissor;
    uint32_t first, count;
} Draw;

/* the state of a save, a layer or a node */
typedef struct {
    Affine m;
    float clip[4]; /* left, top, right, bottom in device pixels */
    float alpha;
} State;

/* a glyph's outline in font units, y up: verbs, and their points' coordinates */
typedef struct {
    jint font;
    uint32_t glyph;
    uint8_t* verbs;
    float* points;
    uint32_t verb_count, point_count; /* point_count floats */
    bool used;
} Glyph;

static struct {
    /* the device */
    SnxVkWindow host;
    VkPhysicalDevice phys;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory;
    SnxVkRing ring;
    SnxVkSwapchain swap;
    VkImageView swap_views[SNX_VK_SWAP_IMAGES];
    VkSemaphore acquired[SLOTS], rendered[SNX_VK_SWAP_IMAGES];
    VkFormat color_format, stencil_format;
    VkSampleCountFlagBits samples;
    bool up;

    /* the targets: the multisampled color and stencil, and the image a frame goes to without a window */
    VkExtent2D extent;
    VkImage msaa, stencil, offscreen;
    VkDeviceMemory msaa_memory, stencil_memory, offscreen_memory;
    VkImageView msaa_view, stencil_view, offscreen_view;

    VkPipelineLayout layout;
    VkPipeline pipes[PIPE_COUNT];
    VkBuffer vertices[SLOTS];
    VkDeviceMemory vertex_memory[SLOTS];
    void* vertex_map[SLOTS];
    size_t vertex_capacity[SLOTS]; /* bytes */

    /* the frame being made */
    Vertex* verts;
    size_t vert_count, vert_capacity;
    Draw* draws;
    size_t draw_count, draw_capacity;
    State* states;
    size_t state_count, state_capacity;
    Polyline poly;
    Triangles tris;
    bool failed;
    int width, height; /* the frame's */

    /* glyph outlines, open addressing */
    Glyph* glyphs;
    size_t glyph_capacity, glyph_count;

    /* a screenshot of frame shot_frame at shot (snx.android.shot), and the frames presented */
    char shot[1024];
    long shot_frame, frames;
    bool warned_shader, warned_bitmap;
} r;

/* ---- the frame's vertices and draws ---- */

static bool reserve(void** data, size_t* capacity, size_t need, size_t size) {
    if (need <= *capacity) return true;
    size_t n = *capacity ? *capacity : 256;
    while (n < need) n *= 2;
    void* d = realloc(*data, n * size);
    if (!d) return false;
    *data = d;
    *capacity = n;
    return true;
}

static VkRect2D scissor(void) {
    const float* c = r.states[r.state_count - 1].clip;
    float l = c[0] < 0 ? 0 : c[0], t = c[1] < 0 ? 0 : c[1];
    float rr = c[2] > (float)r.width ? (float)r.width : c[2], b = c[3] > (float)r.height ? (float)r.height : c[3];
    VkRect2D s = {{0, 0}, {0, 0}};
    if (rr <= l || b <= t) return s;
    s.offset.x = (int32_t)floorf(l);
    s.offset.y = (int32_t)floorf(t);
    s.extent.width = (uint32_t)ceilf(rr) - (uint32_t)s.offset.x;
    s.extent.height = (uint32_t)ceilf(b) - (uint32_t)s.offset.y;
    return s;
}

/* the vertices from first on, as a draw of pipe: joined to the last draw when it continues it */
static void add_draw(uint8_t pipe, size_t first) {
    if (r.failed || r.vert_count == first) return;
    VkRect2D s = scissor();
    if (!s.extent.width || !s.extent.height) {
        r.vert_count = first;
        return;
    }
    if (r.draw_count) {
        Draw* d = &r.draws[r.draw_count - 1];
        if (d->pipe == pipe && (pipe == PIPE_COLOR || pipe == PIPE_COPY) && d->first + d->count == first &&
            !memcmp(&d->scissor, &s, sizeof s)) {
            d->count = (uint32_t)(r.vert_count - d->first);
            return;
        }
    }
    if (!reserve((void**)&r.draws, &r.draw_capacity, r.draw_count + 1, sizeof *r.draws)) {
        r.failed = true;
        return;
    }
    r.draws[r.draw_count++] = (Draw){pipe, s, (uint32_t)first, (uint32_t)(r.vert_count - first)};
}

static void vertex(Point p, const unsigned char rgba[4]) {
    if (r.failed || !reserve((void**)&r.verts, &r.vert_capacity, r.vert_count + 1, sizeof *r.verts)) {
        r.failed = true;
        return;
    }
    Vertex* v = &r.verts[r.vert_count++];
    v->x = p.x;
    v->y = p.y;
    memcpy(v->rgba, rgba, 4);
}

/* ---- paints ---- */

/* a paint as the renderer uses it: its color, premultiplied with the state's alpha, and how it draws */
typedef struct {
    unsigned char rgba[4];
    uint8_t style, cap, join, blend;
    float width, miter;
} Paint;

typedef struct {
    const unsigned char *p, *end;
    bool bad;
} Reader;
static bool need(Reader* rd, size_t n) {
    if (rd->bad || (size_t)(rd->end - rd->p) < n) {
        rd->bad = true;
        return false;
    }
    return true;
}
static unsigned rd_u8(Reader* rd) {
    return need(rd, 1) ? *rd->p++ : 0;
}
static unsigned rd_u16(Reader* rd) {
    if (!need(rd, 2)) return 0;
    unsigned v = snx_ld_le16(rd->p);
    rd->p += 2;
    return v;
}
static int32_t rd_i32(Reader* rd) {
    if (!need(rd, 4)) return 0;
    uint32_t v = snx_ld_le32(rd->p);
    rd->p += 4;
    return (int32_t)v;
}
static float rd_f32(Reader* rd) {
    if (!need(rd, 4)) return 0;
    float v = snx_ld_lef32(rd->p);
    rd->p += 4;
    return v;
}
static void skip(Reader* rd, size_t n) {
    if (need(rd, n)) rd->p += n;
}

/* Skia's blend modes, as BlendMode.skia() gives them */
enum { BLEND_CLEAR = 0, BLEND_SRC = 1, BLEND_DST = 2, BLEND_SRC_OVER = 3, BLEND_SRC_IN = 5, BLEND_SRC_ATOP = 9,
       BLEND_MODULATE = 13 };

static float channel(uint32_t argb, unsigned shift) {
    return (float)((argb >> shift) & 0xff) / 255.0f;
}
static void premultiply(float a, float red, float green, float blue, unsigned char out[4]) {
    if (a < 0) a = 0;
    if (a > 1) a = 1;
    const float c[3] = {red, green, blue};
    for (int i = 0; i < 3; ++i) {
        float v = c[i] < 0 ? 0 : c[i] > 1 ? 1 : c[i];
        out[i] = (unsigned char)lroundf(v * a * 255.0f);
    }
    out[3] = (unsigned char)lroundf(a * 255.0f);
}

/* Paint.write's record. A shader's color is the average of its colors (gradients are not drawn yet); a color filter
   of a blend mode or a color matrix applies to the paint's color here, as Skia's does to a solid color */
static void read_paint(Reader* rd, Paint* out) {
    uint32_t color = (uint32_t)rd_i32(rd);
    out->style = (uint8_t)rd_u8(rd);
    out->cap = (uint8_t)rd_u8(rd);
    out->join = (uint8_t)rd_u8(rd);
    rd_u8(rd); /* flags */
    out->width = rd_f32(rd);
    out->miter = rd_f32(rd);
    out->blend = (uint8_t)rd_u8(rd);
    unsigned shader = rd_u8(rd), filter = rd_u8(rd), dashes = rd_u8(rd);
    float a = channel(color, 24), red = channel(color, 16), green = channel(color, 8), blue = channel(color, 0);
    if (shader) {
        rd_u8(rd);
        rd_u8(rd);
        unsigned n = rd_u16(rd);
        skip(rd, 4 * (6 + 9));
        float sum[4] = {0, 0, 0, 0};
        for (unsigned i = 0; i < n; ++i) {
            uint32_t c = (uint32_t)rd_i32(rd);
            sum[0] += channel(c, 24);
            sum[1] += channel(c, 16);
            sum[2] += channel(c, 8);
            sum[3] += channel(c, 0);
        }
        unsigned positions = rd_u8(rd);
        skip(rd, 3);
        if (positions) skip(rd, 4 * (size_t)n);
        int32_t bitmap = rd_i32(rd);
        if (n) { /* the shader's color, with the paint's alpha over it */
            a *= sum[0] / (float)n;
            red = sum[1] / (float)n;
            green = sum[2] / (float)n;
            blue = sum[3] / (float)n;
        }
        if (!r.warned_shader && (n > 1 || bitmap)) {
            fprintf(stderr, "scarletnx-android: gradients and bitmap shaders draw as one color for now\n");
            r.warned_shader = true;
        }
    }
    if (filter == 1) {
        uint32_t c = (uint32_t)rd_i32(rd);
        unsigned mode = rd_u8(rd);
        skip(rd, 3);
        float fa = channel(c, 24), fr = channel(c, 16), fg = channel(c, 8), fb = channel(c, 0);
        switch (mode) {
        case BLEND_SRC: a = fa, red = fr, green = fg, blue = fb; break;
        case BLEND_SRC_IN: a *= fa, red = fr, green = fg, blue = fb; break;
        case BLEND_SRC_ATOP: /* the filter's color over the paint's, where the paint is */
            red = fr * fa + red * (1 - fa), green = fg * fa + green * (1 - fa), blue = fb * fa + blue * (1 - fa);
            break;
        case BLEND_MODULATE: a *= fa, red *= fr, green *= fg, blue *= fb; break;
        default: break;
        }
    } else if (filter == 2) {
        float m[20];
        for (int i = 0; i < 20; ++i) m[i] = rd_f32(rd);
        /* Android's color matrix: rows for R, G, B and A over (R, G, B, A, 1), the offsets in 0..255 */
        float in[4] = {red, green, blue, a}, o[4];
        for (int row = 0; row < 4; ++row)
            o[row] = m[5 * row] * in[0] + m[5 * row + 1] * in[1] + m[5 * row + 2] * in[2] + m[5 * row + 3] * in[3] +
                     m[5 * row + 4] / 255.0f;
        red = o[0], green = o[1], blue = o[2], a = o[3];
    }
    if (dashes) skip(rd, 4 * (1 + (size_t)dashes)); /* dashed strokes draw solid for now */
    a *= r.states[r.state_count - 1].alpha;
    if (out->blend == BLEND_CLEAR) a = red = green = blue = 0;
    premultiply(a, red, green, blue, out->rgba);
}

/* whether the paint draws over the target (source over) or replaces it */
static bool copies(const Paint* p) {
    return p->blend == BLEND_CLEAR || p->blend == BLEND_SRC;
}
/* a paint that changes nothing */
static bool invisible(const Paint* p) {
    return p->blend == BLEND_DST || (!copies(p) && p->rgba[3] == 0);
}

/* ---- shapes ---- */

static float tolerance(void) {
    return 0.25f;
}
static Affine current(void) {
    return r.states[r.state_count - 1].m;
}
static Point at(Affine m, float x, float y) {
    Point p = {m.a * x + m.b * y + m.c, m.d * x + m.e * y + m.f};
    return p;
}

/* a convex polygon (device pixels) filled directly */
static void fill_convex(const Point* pts, size_t n, const Paint* p) {
    size_t first = r.vert_count;
    for (size_t i = 1; i + 1 < n; ++i) {
        vertex(pts[0], p->rgba);
        vertex(pts[i], p->rgba);
        vertex(pts[i + 1], p->rgba);
    }
    add_draw(copies(p) ? PIPE_COPY : PIPE_COLOR, first);
}

/* the bounds of points, a cover's rectangle */
static void cover(const Point* pts, size_t n, const Paint* p) {
    if (!n) return;
    float l = pts[0].x, t = pts[0].y, rr = l, b = t;
    for (size_t i = 1; i < n; ++i) {
        if (pts[i].x < l) l = pts[i].x;
        if (pts[i].x > rr) rr = pts[i].x;
        if (pts[i].y < t) t = pts[i].y;
        if (pts[i].y > b) b = pts[i].y;
    }
    l -= 1, t -= 1, rr += 1, b += 1;
    Point q[4] = {{l, t}, {rr, t}, {rr, b}, {l, b}};
    size_t first = r.vert_count;
    vertex(q[0], p->rgba);
    vertex(q[1], p->rgba);
    vertex(q[2], p->rgba);
    vertex(q[0], p->rgba);
    vertex(q[2], p->rgba);
    vertex(q[3], p->rgba);
    add_draw(copies(p) ? PIPE_COVER_COPY : PIPE_COVER, first);
}

/* triangles into the stencil by pipe, then their cover */
static void stencil_cover(const Triangles* t, uint8_t pipe, const Paint* p) {
    if (!t->count) return;
    static const unsigned char none[4] = {0, 0, 0, 0};
    size_t first = r.vert_count;
    for (size_t i = 0; i < t->count; ++i) vertex(t->points[i], none);
    add_draw(pipe, first);
    cover(t->points, t->count, p);
}

/* the polyline (device pixels) filled by its fill type (0 winding, 1 even-odd; inverse types fill as their normal
   ones for now), and stroked when the paint strokes */
static void draw_poly(const Paint* p, unsigned fill_type, bool fill, bool stroke, float stroke_width) {
    if (fill) {
        r.tris.count = 0;
        fan_poly(&r.tris, &r.poly);
        stencil_cover(&r.tris, (fill_type & 1) ? PIPE_EVEN_ODD : PIPE_WINDING, p);
    }
    if (stroke) {
        r.tris.count = 0;
        stroke_poly(&r.tris, &r.poly, stroke_width, p->cap, p->join, p->miter, tolerance());
        stencil_cover(&r.tris, PIPE_SET, p);
    }
    if (r.poly.failed || r.tris.failed) r.failed = true;
}

/* the stroke's width in device pixels: a hairline (0) is one pixel */
static float device_width(const Paint* p) {
    return p->width > 0 ? p->width * affine_scale(current()) : 1.0f;
}

/* a shape given as a closed local polygon, drawn with the paint's style */
static void draw_shape(const Point* local, size_t n, const Paint* p) {
    if (invisible(p) || n < 2) return;
    Affine m = current();
    poly_clear(&r.poly);
    for (size_t i = 0; i < n; ++i) {
        Point d = at(m, local[i].x, local[i].y);
        if (i) poly_line(&r.poly, d);
        else poly_move(&r.poly, d);
    }
    poly_close(&r.poly);
    if (r.poly.failed) {
        r.failed = true;
        return;
    }
    if (p->style != 1 && r.poly.contours) fill_convex(r.poly.points, r.poly.ends[0], p);
    if (p->style != 0) draw_poly(p, 0, false, true, device_width(p));
}

/* an ellipse's points from angle a0 through sweep (radians, y down), at least enough that each chord stays within
   the tolerance in device pixels */
static size_t ellipse(Point* out, size_t max, float cx, float cy, float rx, float ry, float a0, float sweep) {
    float radius = (rx > ry ? rx : ry) * affine_scale(current());
    float step = radius > tolerance() ? 2 * acosf(1 - tolerance() / radius) : 1.0f;
    if (!(step > 0.02f)) step = 0.02f;
    size_t n = (size_t)ceilf(fabsf(sweep) / step);
    if (n < 4) n = 4;
    if (n > max - 1) n = max - 1;
    for (size_t i = 0; i <= n; ++i) {
        float a = a0 + sweep * (float)i / (float)n;
        out[i] = (Point){cx + rx * cosf(a), cy + ry * sinf(a)};
    }
    return n + 1;
}

enum { SHAPE_MAX = 1024 };
static Point shape[SHAPE_MAX];

static void draw_rrect(float l, float t, float rr, float b, float rx, float ry, const Paint* p) {
    if (rr < l) { float s = l; l = rr; rr = s; }
    if (b < t) { float s = t; t = b; b = s; }
    rx = fminf(fmaxf(rx, 0), (rr - l) / 2);
    ry = fminf(fmaxf(ry, 0), (b - t) / 2);
    if (rx <= 0 || ry <= 0) {
        Point q[4] = {{l, t}, {rr, t}, {rr, b}, {l, b}};
        draw_shape(q, 4, p);
        return;
    }
    size_t n = 0;
    const float h = HWUI_PI / 2;
    n += ellipse(shape + n, SHAPE_MAX / 4, rr - rx, t + ry, rx, ry, -h, h);
    n += ellipse(shape + n, SHAPE_MAX / 4, rr - rx, b - ry, rx, ry, 0, h);
    n += ellipse(shape + n, SHAPE_MAX / 4, l + rx, b - ry, rx, ry, h, h);
    n += ellipse(shape + n, SHAPE_MAX / 4, l + rx, t + ry, rx, ry, 2 * h, h);
    draw_shape(shape, n, p);
}

static void draw_oval(float l, float t, float rr, float b, const Paint* p) {
    size_t n = ellipse(shape, SHAPE_MAX, (l + rr) / 2, (t + b) / 2, fabsf(rr - l) / 2, fabsf(b - t) / 2, 0, 2 * HWUI_PI);
    draw_shape(shape, n - 1, p);
}

static void draw_arc(float l, float t, float rr, float b, float start, float sweep, bool center, const Paint* p) {
    if (invisible(p)) return;
    float cx = (l + rr) / 2, cy = (t + b) / 2;
    if (sweep > 360) sweep = 360;
    if (sweep < -360) sweep = -360;
    size_t n = ellipse(shape, SHAPE_MAX - 1, cx, cy, fabsf(rr - l) / 2, fabsf(b - t) / 2, start * HWUI_PI / 180,
                       sweep * HWUI_PI / 180);
    if (center) shape[n++] = (Point){cx, cy};
    Affine m = current();
    poly_clear(&r.poly);
    for (size_t i = 0; i < n; ++i) {
        Point d = at(m, shape[i].x, shape[i].y);
        if (i) poly_line(&r.poly, d);
        else poly_move(&r.poly, d);
    }
    /* a stroke without the center is the open arc */
    if (center || p->style == 0) poly_close(&r.poly);
    else poly_end(&r.poly);
    draw_poly(p, 0, p->style != 1, p->style != 0, device_width(p));
}

/* a recorded path (Canvas.writePath): its fill type, then the path */
static bool read_path(Reader* rd, unsigned* fill_type) {
    *fill_type = rd_u8(rd);
    uint32_t verbs = (uint32_t)rd_i32(rd), points = (uint32_t)rd_i32(rd);
    if (!need(rd, (size_t)verbs + 4 * (size_t)points)) return false;
    const uint8_t* v = rd->p;
    const unsigned char* pts = rd->p + verbs;
    rd->p += verbs + 4 * (size_t)points;
    poly_clear(&r.poly);
    poly_path(&r.poly, v, verbs, pts, points, current(), tolerance());
    return !r.poly.failed;
}

/* ---- glyphs ---- */

typedef struct {
    uint8_t* verbs;
    float* points;
    uint32_t verb_count, point_count, verb_capacity, point_capacity;
    bool failed;
} Outline;
static void outline_verb(Outline* o, uint8_t verb, const float* pts, unsigned n) {
    if (o->failed) return;
    size_t vc = o->verb_capacity, pc = o->point_capacity;
    if (!reserve((void**)&o->verbs, &vc, (size_t)o->verb_count + 1, 1) ||
        !reserve((void**)&o->points, &pc, (size_t)o->point_count + n, sizeof(float))) {
        o->failed = true;
        return;
    }
    o->verb_capacity = (uint32_t)vc;
    o->point_capacity = (uint32_t)pc;
    o->verbs[o->verb_count++] = verb;
    memcpy(o->points + o->point_count, pts, n * sizeof(float));
    o->point_count += n;
}
static void move_to(hb_draw_funcs_t* f, void* data, hb_draw_state_t* s, float x, float y, void* u) {
    (void)f, (void)s, (void)u;
    float p[2] = {x, y};
    outline_verb(data, VERB_MOVE, p, 2);
}
static void line_to(hb_draw_funcs_t* f, void* data, hb_draw_state_t* s, float x, float y, void* u) {
    (void)f, (void)s, (void)u;
    float p[2] = {x, y};
    outline_verb(data, VERB_LINE, p, 2);
}
static void quad_to(hb_draw_funcs_t* f, void* data, hb_draw_state_t* s, float cx, float cy, float x, float y,
                    void* u) {
    (void)f, (void)s, (void)u;
    float p[4] = {cx, cy, x, y};
    outline_verb(data, VERB_QUAD, p, 4);
}
static void cubic_to(hb_draw_funcs_t* f, void* data, hb_draw_state_t* s, float c1x, float c1y, float c2x, float c2y,
                     float x, float y, void* u) {
    (void)f, (void)s, (void)u;
    float p[6] = {c1x, c1y, c2x, c2y, x, y};
    outline_verb(data, VERB_CUBIC, p, 6);
}
static void close_path(hb_draw_funcs_t* f, void* data, hb_draw_state_t* s, void* u) {
    (void)f, (void)s, (void)u;
    outline_verb(data, VERB_CLOSE, NULL, 0);
}

static size_t glyph_slot(jint font, uint32_t glyph) {
    uint32_t h = (uint32_t)font * 0x9e3779b1u ^ glyph * 0x85ebca77u;
    size_t mask = r.glyph_capacity - 1, i = h & mask;
    while (r.glyphs[i].used && (r.glyphs[i].font != font || r.glyphs[i].glyph != glyph)) i = (i + 1) & mask;
    return i;
}
/* a glyph's outline, read once from its font */
static const Glyph* glyph_outline(jint font, uint32_t glyph) {
    if (r.glyph_count * 2 >= r.glyph_capacity) {
        size_t old = r.glyph_capacity, n = old ? old * 2 : 1024;
        Glyph* list = r.glyphs;
        r.glyphs = calloc(n, sizeof *r.glyphs);
        if (!r.glyphs) {
            r.glyphs = list;
            return NULL;
        }
        r.glyph_capacity = n;
        for (size_t i = 0; i < old; ++i)
            if (list[i].used) r.glyphs[glyph_slot(list[i].font, list[i].glyph)] = list[i];
        free(list);
    }
    size_t i = glyph_slot(font, glyph);
    if (r.glyphs[i].used) return &r.glyphs[i];
    hb_font_t* f = android_text_font(font);
    if (!f) return NULL;
    static hb_draw_funcs_t* funcs;
    if (!funcs) {
        funcs = hb_draw_funcs_create();
        hb_draw_funcs_set_move_to_func(funcs, move_to, NULL, NULL);
        hb_draw_funcs_set_line_to_func(funcs, line_to, NULL, NULL);
        hb_draw_funcs_set_quadratic_to_func(funcs, quad_to, NULL, NULL);
        hb_draw_funcs_set_cubic_to_func(funcs, cubic_to, NULL, NULL);
        hb_draw_funcs_set_close_path_func(funcs, close_path, NULL, NULL);
        hb_draw_funcs_make_immutable(funcs);
    }
    Outline o = {0};
    hb_font_draw_glyph(f, glyph, funcs, &o);
    if (o.failed) {
        free(o.verbs);
        free(o.points);
        return NULL;
    }
    r.glyphs[i] = (Glyph){font, glyph, o.verbs, o.points, o.verb_count, o.point_count, true};
    ++r.glyph_count;
    return &r.glyphs[i];
}

static void draw_glyphs(Reader* rd) {
    jint font = rd_i32(rd);
    float size = rd_f32(rd), scale_x = rd_f32(rd), skew = rd_f32(rd);
    rd_u8(rd); /* fake bold */
    int32_t n = rd_i32(rd);
    if (n < 0 || !need(rd, 12 * (size_t)n)) return;
    const unsigned char* ids = rd->p;
    const unsigned char* pos = rd->p + 4 * (size_t)n;
    rd->p += 12 * (size_t)n;
    Paint p;
    read_paint(rd, &p);
    hb_font_t* f = android_text_font(font);
    if (!f || invisible(&p) || rd->bad) return;
    int upem_x, upem_y;
    hb_font_get_scale(f, &upem_x, &upem_y);
    float s = size / (float)(upem_y ? upem_y : 1000);
    Affine m = current();
    poly_clear(&r.poly);
    for (int32_t g = 0; g < n; ++g) {
        const Glyph* gl = glyph_outline(font, snx_ld_le32(ids + 4 * (size_t)g));
        if (!gl) continue;
        float x = snx_ld_lef32(pos + 8 * (size_t)g), y = snx_ld_lef32(pos + 8 * (size_t)g + 4);
        /* font units (y up) to the canvas: scaled, the y axis turned down, the x skewed by the text's skew */
        Affine glyph = {s * scale_x, -s * skew, x, 0, -s, y};
        Affine to = affine_mul(m, glyph);
        uint32_t k = 0;
        for (uint32_t v = 0; v < gl->verb_count; ++v) {
            const float* q = gl->points + k;
            switch (gl->verbs[v]) {
            case VERB_MOVE: poly_move(&r.poly, at(to, q[0], q[1])), k += 2; break;
            case VERB_LINE: poly_line(&r.poly, at(to, q[0], q[1])), k += 2; break;
            case VERB_QUAD: poly_quad(&r.poly, at(to, q[0], q[1]), at(to, q[2], q[3]), tolerance()), k += 4; break;
            case VERB_CUBIC:
                poly_cubic(&r.poly, at(to, q[0], q[1]), at(to, q[2], q[3]), at(to, q[4], q[5]), tolerance());
                k += 6;
                break;
            case VERB_CLOSE: poly_close(&r.poly); break;
            default: break;
            }
        }
        poly_end(&r.poly);
    }
    p.style = 0;
    draw_poly(&p, 0, true, false, 0);
}

/* ---- the display list ---- */

static void push(void) {
    if (!reserve((void**)&r.states, &r.state_capacity, r.state_count + 1, sizeof *r.states)) {
        r.failed = true;
        return;
    }
    r.states[r.state_count] = r.states[r.state_count - 1];
    ++r.state_count;
}
/* the clip intersected with a local rectangle's device bounds */
static void clip_rect(float l, float t, float rr, float b) {
    Affine m = current();
    Point q[4] = {at(m, l, t), at(m, rr, t), at(m, rr, b), at(m, l, b)};
    float x0 = q[0].x, y0 = q[0].y, x1 = x0, y1 = y0;
    for (int i = 1; i < 4; ++i) {
        x0 = fminf(x0, q[i].x), x1 = fmaxf(x1, q[i].x);
        y0 = fminf(y0, q[i].y), y1 = fmaxf(y1, q[i].y);
    }
    float* c = r.states[r.state_count - 1].clip;
    c[0] = fmaxf(c[0], x0), c[1] = fmaxf(c[1], y0), c[2] = fminf(c[2], x1), c[3] = fminf(c[3], y1);
}
static void clip_poly(void) {
    if (!r.poly.count) return;
    float x0 = r.poly.points[0].x, y0 = r.poly.points[0].y, x1 = x0, y1 = y0;
    for (size_t i = 1; i < r.poly.count; ++i) {
        x0 = fminf(x0, r.poly.points[i].x), x1 = fmaxf(x1, r.poly.points[i].x);
        y0 = fminf(y0, r.poly.points[i].y), y1 = fmaxf(y1, r.poly.points[i].y);
    }
    float* c = r.states[r.state_count - 1].clip;
    c[0] = fmaxf(c[0], x0), c[1] = fmaxf(c[1], y0), c[2] = fminf(c[2], x1), c[3] = fminf(c[3], y1);
}
static Affine read_matrix(Reader* rd) {
    float v[9];
    for (int i = 0; i < 9; ++i) v[i] = rd_f32(rd);
    Affine m = {v[0], v[1], v[2], v[3], v[4], v[5]};
    return m;
}
static void fill_clip(const Paint* p) {
    const float* c = r.states[r.state_count - 1].clip;
    Point q[4] = {{c[0], c[1]}, {c[2], c[1]}, {c[2], c[3]}, {c[0], c[3]}};
    fill_convex(q, 4, p);
}

/* the frame's operations. Restores never go below the save depth that the node began at */
static void interpret(Reader* rd) {
    size_t* bases = NULL; /* each open node's state depth */
    size_t base_count = 0, base_capacity = 0;
    size_t base = 1;
    while (rd->p < rd->end && !rd->bad && !r.failed) {
        unsigned op = rd_u8(rd);
        Paint p;
        switch (op) {
        case 1: push(); break; /* SAVE */
        case 2:                /* RESTORE */
            if (r.state_count > base) --r.state_count;
            break;
        case 4: { /* TRANSLATE */
            float dx = rd_f32(rd), dy = rd_f32(rd);
            Affine t = {1, 0, dx, 0, 1, dy};
            r.states[r.state_count - 1].m = affine_mul(current(), t);
            break;
        }
        case 5: { /* SCALE */
            float sx = rd_f32(rd), sy = rd_f32(rd);
            Affine t = {sx, 0, 0, 0, sy, 0};
            r.states[r.state_count - 1].m = affine_mul(current(), t);
            break;
        }
        case 6: { /* ROTATE */
            float a = rd_f32(rd) * HWUI_PI / 180, c = cosf(a), s = sinf(a);
            Affine t = {c, -s, 0, s, c, 0};
            r.states[r.state_count - 1].m = affine_mul(current(), t);
            break;
        }
        case 7: r.states[r.state_count - 1].m = affine_mul(current(), read_matrix(rd)); break; /* CONCAT */
        case 8: { /* SET_MATRIX: relative to the node's own transform */
            Affine m = read_matrix(rd);
            r.states[r.state_count - 1].m = affine_mul(r.states[base - 1].m, m);
            break;
        }
        case 10: { /* CLIP_RECT */
            float l = rd_f32(rd), t = rd_f32(rd), rr = rd_f32(rd), b = rd_f32(rd);
            if (rd_u8(rd) == 0) clip_rect(l, t, rr, b); /* a difference leaves the clip (its bounds) as it is */
            break;
        }
        case 11: { /* CLIP_PATH */
            unsigned fill;
            if (read_path(rd, &fill) && rd_u8(rd) == 0) clip_poly();
            break;
        }
        case 12: { /* DRAW_COLOR */
            uint32_t c = (uint32_t)rd_i32(rd);
            p.blend = (uint8_t)rd_u8(rd);
            premultiply(channel(c, 24) * r.states[r.state_count - 1].alpha, channel(c, 16), channel(c, 8),
                        channel(c, 0), p.rgba);
            if (p.blend == BLEND_CLEAR) memset(p.rgba, 0, 4);
            if (!invisible(&p)) fill_clip(&p);
            break;
        }
        case 13: /* DRAW_PAINT */
            read_paint(rd, &p);
            if (!invisible(&p)) fill_clip(&p);
            break;
        case 14: { /* DRAW_RECT */
            float l = rd_f32(rd), t = rd_f32(rd), rr = rd_f32(rd), b = rd_f32(rd);
            read_paint(rd, &p);
            Point q[4] = {{l, t}, {rr, t}, {rr, b}, {l, b}};
            draw_shape(q, 4, &p);
            break;
        }
        case 15: { /* DRAW_RRECT */
            float l = rd_f32(rd), t = rd_f32(rd), rr = rd_f32(rd), b = rd_f32(rd), rx = rd_f32(rd), ry = rd_f32(rd);
            read_paint(rd, &p);
            draw_rrect(l, t, rr, b, rx, ry, &p);
            break;
        }
        case 16: { /* DRAW_OVAL */
            float l = rd_f32(rd), t = rd_f32(rd), rr = rd_f32(rd), b = rd_f32(rd);
            read_paint(rd, &p);
            draw_oval(l, t, rr, b, &p);
            break;
        }
        case 17: { /* DRAW_CIRCLE */
            float cx = rd_f32(rd), cy = rd_f32(rd), radius = rd_f32(rd);
            read_paint(rd, &p);
            draw_oval(cx - radius, cy - radius, cx + radius, cy + radius, &p);
            break;
        }
        case 18: { /* DRAW_ARC */
            float l = rd_f32(rd), t = rd_f32(rd), rr = rd_f32(rd), b = rd_f32(rd), start = rd_f32(rd), sweep = rd_f32(rd);
            bool center = rd_u8(rd) != 0;
            read_paint(rd, &p);
            draw_arc(l, t, rr, b, start, sweep, center, &p);
            break;
        }
        case 19:   /* DRAW_LINES */
        case 20: { /* DRAW_POINTS */
            int32_t n = rd_i32(rd);
            if (n < 0 || !need(rd, 4 * (size_t)n)) break;
            const unsigned char* pts = rd->p;
            rd->p += 4 * (size_t)n;
            read_paint(rd, &p);
            if (invisible(&p)) break;
            Affine m = current();
            poly_clear(&r.poly);
            unsigned step = op == 19 ? 4 : 2;
            for (int32_t i = 0; i + (int32_t)step <= n; i += (int32_t)step) {
                const unsigned char* q = pts + 4 * (size_t)i;
                Point a = at(m, snx_ld_lef32(q), snx_ld_lef32(q + 4));
                poly_move(&r.poly, a);
                poly_line(&r.poly, op == 19 ? at(m, snx_ld_lef32(q + 8), snx_ld_lef32(q + 12)) : a);
                poly_end(&r.poly);
            }
            /* a point is a dot of the stroke's width, square unless the cap is round */
            if (op == 20 && p.cap == CAP_BUTT) p.cap = CAP_SQUARE;
            draw_poly(&p, 0, false, true, device_width(&p));
            break;
        }
        case 21: { /* DRAW_PATH */
            unsigned fill;
            bool ok = read_path(rd, &fill);
            read_paint(rd, &p);
            if (ok && !invisible(&p)) {
                /* stroking the path read again: draw_poly strokes what it fills */
                draw_poly(&p, fill, p.style != 1, p.style != 0, device_width(&p));
            }
            break;
        }
        case 22: draw_glyphs(rd); break; /* DRAW_GLYPHS */
        case 23:                         /* DRAW_BITMAP */
            rd_i32(rd);
            skip(rd, 4 * 8);
            read_paint(rd, &p);
            if (!r.warned_bitmap) {
                fprintf(stderr, "scarletnx-android: bitmaps do not draw yet\n");
                r.warned_bitmap = true;
            }
            break;
        case 25: { /* SAVE_LAYER: its alpha on what it holds (no offscreen layer yet) */
            skip(rd, 16);
            int32_t alpha = rd_i32(rd);
            rd_u8(rd);
            push();
            r.states[r.state_count - 1].alpha *= (float)alpha / 255.0f;
            break;
        }
        case 30: { /* NODE_BEGIN */
            Affine m = read_matrix(rd);
            float alpha = rd_f32(rd), w = rd_f32(rd), h = rd_f32(rd);
            bool clip_bounds = rd_u8(rd) != 0;
            push();
            if (!reserve((void**)&bases, &base_capacity, base_count + 1, sizeof *bases)) {
                r.failed = true;
                break;
            }
            bases[base_count++] = base;
            base = r.state_count;
            State* st = &r.states[r.state_count - 1];
            st->m = affine_mul(st->m, m);
            st->alpha *= alpha;
            if (clip_bounds) clip_rect(0, 0, w, h);
            unsigned outline = rd_u8(rd);
            if (outline == 1) {
                float l = rd_f32(rd), t = rd_f32(rd), rr = rd_f32(rd), b = rd_f32(rd);
                rd_f32(rd); /* the radius: the outline clips to its bounds for now */
                clip_rect(l, t, rr, b);
            } else if (outline == 2) {
                unsigned fill;
                if (read_path(rd, &fill)) clip_poly();
            }
            if (rd_u8(rd)) {
                float l = rd_f32(rd), t = rd_f32(rd), rr = rd_f32(rd), b = rd_f32(rd);
                clip_rect(l, t, rr, b);
            }
            break;
        }
        case 31: /* NODE_END */
            if (!base_count) {
                rd->bad = true;
                break;
            }
            r.state_count = base - 1;
            base = bases[--base_count];
            break;
        default: rd->bad = true; break;
        }
    }
    if (rd->bad) fprintf(stderr, "scarletnx-android: a display list that can't be read\n");
    free(bases);
}

/* ---- Vulkan ---- */

static bool make_device(VkPhysicalDevice phys, uint32_t family, void* user) {
    (void)user;
    VkPhysicalDeviceVulkan13Features v13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v13};
    vkGetPhysicalDeviceFeatures2(phys, &features);
    if (!v13.dynamicRendering || !v13.synchronization2) return false;
    const char* extensions[1];
    uint32_t n = 0;
    if (r.host.surface) {
        SnxVkExtensions list;
        if (!snx_vk_device_extensions(phys, &list)) return false;
        bool ok = snx_vk_extensions_has(&list, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        snx_vk_extensions_free(&list);
        if (!ok) return false;
        extensions[n++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    }
    VkPhysicalDeviceVulkan13Features want13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                               .dynamicRendering = VK_TRUE,
                                               .synchronization2 = VK_TRUE};
    VkPhysicalDeviceFeatures2 want = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &want13};
    if (snx_vk_device_create(phys, family, &want, extensions, n, &r.device) != VK_SUCCESS) return false;
    r.phys = phys;
    r.family = family;
    return true;
}

static bool make_image(VkFormat format, VkImageUsageFlags usage, VkSampleCountFlagBits samples,
                       VkImageAspectFlags aspect, VkImage* image, VkDeviceMemory* memory, VkImageView* view) {
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {r.extent.width, r.extent.height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = samples,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(r.device, &ii, NULL, image) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(r.device, *image, &req);
    uint32_t type = snx_vk_memory_type(&r.memory, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(r.device, &ai, NULL, memory) != VK_SUCCESS) return false;
    vkBindImageMemory(r.device, *image, *memory, 0);
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = *image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {aspect, 0, 1, 0, 1}};
    return vkCreateImageView(r.device, &vi, NULL, view) == VK_SUCCESS;
}
static void free_image(VkImage* image, VkDeviceMemory* memory, VkImageView* view) {
    if (*view) vkDestroyImageView(r.device, *view, NULL);
    if (*image) vkDestroyImage(r.device, *image, NULL);
    if (*memory) vkFreeMemory(r.device, *memory, NULL);
    *view = VK_NULL_HANDLE;
    *image = VK_NULL_HANDLE;
    *memory = VK_NULL_HANDLE;
}

static void free_targets(void) {
    free_image(&r.msaa, &r.msaa_memory, &r.msaa_view);
    free_image(&r.stencil, &r.stencil_memory, &r.stencil_view);
    free_image(&r.offscreen, &r.offscreen_memory, &r.offscreen_view);
    for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
        if (r.swap_views[i]) vkDestroyImageView(r.device, r.swap_views[i], NULL), r.swap_views[i] = VK_NULL_HANDLE;
}

/* the targets for an extent: the swapchain's images (a window's) or the offscreen image, and the multisampled ones */
static bool make_targets(VkExtent2D extent) {
    vkDeviceWaitIdle(r.device);
    free_targets();
    r.extent = extent;
    if (r.host.surface) {
        static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        SnxVkSwapchainInfo info = {.surface = r.host.surface,
                                   .formats = formats,
                                   .format_count = 2,
                                   .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                                   .window = extent};
        if (!snx_vk_swapchain_make(r.phys, r.device, &info, &r.swap)) return false;
        r.extent = r.swap.extent;
        if (r.swap.format.format != r.color_format) return false; /* the pipelines' format */
        for (uint32_t i = 0; i < r.swap.count; ++i) {
            VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                        .image = r.swap.images[i],
                                        .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                        .format = r.color_format,
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            if (vkCreateImageView(r.device, &vi, NULL, &r.swap_views[i]) != VK_SUCCESS) return false;
        }
    } else if (!make_image(r.color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                           VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &r.offscreen, &r.offscreen_memory,
                           &r.offscreen_view)) {
        return false;
    }
    if (r.samples != VK_SAMPLE_COUNT_1_BIT &&
        !make_image(r.color_format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                    r.samples, VK_IMAGE_ASPECT_COLOR_BIT, &r.msaa, &r.msaa_memory, &r.msaa_view))
        return false;
    VkImageAspectFlags stencil_aspect = r.stencil_format == VK_FORMAT_S8_UINT
                                            ? VK_IMAGE_ASPECT_STENCIL_BIT
                                            : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    return make_image(r.stencil_format,
                      VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, r.samples,
                      stencil_aspect, &r.stencil, &r.stencil_memory, &r.stencil_view);
}

static VkShaderModule shader(const u32* words, u64 size) {
    VkShaderModuleCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                   .codeSize = (size_t)size,
                                   .pCode = words};
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(r.device, &ci, NULL, &m);
    return m;
}

static bool make_pipelines(void) {
    VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .pushConstantRangeCount = 1,
                                     .pPushConstantRanges = &push};
    if (vkCreatePipelineLayout(r.device, &li, NULL, &r.layout) != VK_SUCCESS) return false;
    VkShaderModule vs = shader(android_draw_vert_spv, android_draw_vert_spv_size);
    VkShaderModule fs = shader(android_draw_frag_spv, android_draw_frag_spv_size);
    bool ok = vs && fs;
    VkPipelineShaderStageCreateInfo stages[2] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vs,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fs,
         .pName = "main"}};
    VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0},
                                                       {1, 0, VK_FORMAT_R8G8B8A8_UNORM, 8}};
    VkPipelineVertexInputStateCreateInfo input = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                  .vertexBindingDescriptionCount = 1,
                                                  .pVertexBindingDescriptions = &binding,
                                                  .vertexAttributeDescriptionCount = 2,
                                                  .pVertexAttributeDescriptions = attributes};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo multisample = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = r.samples};
    VkDynamicState dynamic[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamics = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                 .dynamicStateCount = 2,
                                                 .pDynamicStates = dynamic};
    VkFormat stencil_depth = r.stencil_format == VK_FORMAT_S8_UINT ? VK_FORMAT_UNDEFINED : r.stencil_format;
    VkPipelineRenderingCreateInfo rendering = {.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                               .colorAttachmentCount = 1,
                                               .pColorAttachmentFormats = &r.color_format,
                                               .depthAttachmentFormat = stencil_depth,
                                               .stencilAttachmentFormat = r.stencil_format};
    for (int i = 0; i < PIPE_COUNT && ok; ++i) {
        bool stencil_only = i == PIPE_WINDING || i == PIPE_EVEN_ODD || i == PIPE_SET;
        bool cover = i == PIPE_COVER || i == PIPE_COVER_COPY;
        bool copy = i == PIPE_COPY || i == PIPE_COVER_COPY;
        VkPipelineColorBlendAttachmentState blend = {
            .blendEnable = !copy && !stencil_only,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = stencil_only ? 0
                                           : VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT};
        VkPipelineColorBlendStateCreateInfo blending = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &blend};
        VkStencilOpState front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS,
                                  0xff, 0xff, 0};
        VkStencilOpState back = front;
        if (i == PIPE_WINDING) {
            front.passOp = VK_STENCIL_OP_INCREMENT_AND_WRAP;
            back.passOp = VK_STENCIL_OP_DECREMENT_AND_WRAP;
        } else if (i == PIPE_EVEN_ODD) {
            front.passOp = back.passOp = VK_STENCIL_OP_INVERT;
            front.writeMask = back.writeMask = 1;
        } else if (i == PIPE_SET) {
            front.passOp = back.passOp = VK_STENCIL_OP_REPLACE;
            front.reference = back.reference = 1;
        } else if (cover) {
            front.compareOp = back.compareOp = VK_COMPARE_OP_NOT_EQUAL;
            front.passOp = back.passOp = VK_STENCIL_OP_ZERO;
        }
        VkPipelineDepthStencilStateCreateInfo depth = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .stencilTestEnable = stencil_only || cover,
            .front = front,
            .back = back};
        VkGraphicsPipelineCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                           .pNext = &rendering,
                                           .stageCount = 2,
                                           .pStages = stages,
                                           .pVertexInputState = &input,
                                           .pInputAssemblyState = &assembly,
                                           .pViewportState = &viewport,
                                           .pRasterizationState = &raster,
                                           .pMultisampleState = &multisample,
                                           .pDepthStencilState = &depth,
                                           .pColorBlendState = &blending,
                                           .pDynamicState = &dynamics,
                                           .layout = r.layout};
        ok = vkCreateGraphicsPipelines(r.device, VK_NULL_HANDLE, 1, &pi, NULL, &r.pipes[i]) == VK_SUCCESS;
    }
    if (vs) vkDestroyShaderModule(r.device, vs, NULL);
    if (fs) vkDestroyShaderModule(r.device, fs, NULL);
    return ok;
}

/* the device, the window's or none, and what the frames need. False with the reason printed */
static bool start(int width, int height, const char* title) {
    char error[256];
    if (!snx_vk_window_open("scarletnx-android", title, width, height, &r.host, error, sizeof error)) {
        fprintf(stderr, "scarletnx-android: %s\n", error);
        return false;
    }
    if (!snx_vk_first_device(r.host.instance.instance, VK_API_VERSION_1_3, VK_QUEUE_GRAPHICS_BIT, r.host.surface,
                             make_device, NULL)) {
        fprintf(stderr, "scarletnx-android: no Vulkan 1.3 device with dynamic rendering\n");
        snx_vk_window_close(&r.host);
        return false;
    }
    volkLoadDevice(r.device);
    vkGetDeviceQueue(r.device, r.family, 0, &r.queue);
    vkGetPhysicalDeviceMemoryProperties(r.phys, &r.memory);
    VkPhysicalDeviceProperties properties;
    vkGetPhysicalDeviceProperties(r.phys, &properties);
    /* 4x MSAA where the color and stencil targets take it */
    VkSampleCountFlags counts = properties.limits.framebufferColorSampleCounts &
                                properties.limits.framebufferStencilSampleCounts;
    r.samples = counts & VK_SAMPLE_COUNT_4_BIT ? VK_SAMPLE_COUNT_4_BIT : VK_SAMPLE_COUNT_1_BIT;
    static const VkFormat stencils[3] = {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
    for (unsigned i = 0; i < 3 && !r.stencil_format; ++i) {
        VkFormatProperties f;
        vkGetPhysicalDeviceFormatProperties(r.phys, stencils[i], &f);
        if (f.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) r.stencil_format = stencils[i];
    }
    r.color_format = VK_FORMAT_B8G8R8A8_UNORM;
    if (r.host.surface) {
        static const VkFormat formats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM};
        VkSurfaceFormatKHR f;
        snx_vk_surface_format(r.phys, r.host.surface, formats, 2, &f);
        r.color_format = f.format;
    }
    VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
    VkDescriptorPoolCreateInfo dp = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 16,
                                     .poolSizeCount = 1,
                                     .pPoolSizes = &size};
    bool ok = r.stencil_format && snx_vk_ring_init(&r.ring, r.device, r.queue, r.family, SLOTS, &dp);
    VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (unsigned i = 0; i < SLOTS && ok; ++i) ok = vkCreateSemaphore(r.device, &si, NULL, &r.acquired[i]) == VK_SUCCESS;
    for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES && ok; ++i)
        ok = vkCreateSemaphore(r.device, &si, NULL, &r.rendered[i]) == VK_SUCCESS;
    ok = ok && make_pipelines() && make_targets((VkExtent2D){(uint32_t)width, (uint32_t)height});
    if (!ok) {
        fprintf(stderr, "scarletnx-android: the renderer's Vulkan objects can't be made\n");
        return false;
    }
    r.states = malloc(16 * sizeof *r.states);
    r.state_capacity = r.states ? 16 : 0;
    const char* shot = getenv("SNX_ANDROID_SHOT");
    if (shot && shot[0]) {
        snprintf(r.shot, sizeof r.shot, "%s", shot);
        const char* frame = getenv("SNX_ANDROID_SHOT_FRAME");
        r.shot_frame = frame ? atol(frame) : 60;
    }
    r.up = r.states != NULL;
    return r.up;
}

/* the slot's vertex buffer, with room for size bytes */
static bool vertex_room(unsigned slot, size_t size) {
    if (r.vertex_capacity[slot] >= size) return true;
    if (r.vertices[slot]) {
        vkUnmapMemory(r.device, r.vertex_memory[slot]);
        vkDestroyBuffer(r.device, r.vertices[slot], NULL);
        vkFreeMemory(r.device, r.vertex_memory[slot], NULL);
        r.vertices[slot] = VK_NULL_HANDLE;
        r.vertex_memory[slot] = VK_NULL_HANDLE;
        r.vertex_capacity[slot] = 0;
    }
    size_t capacity = 1 << 16;
    while (capacity < size) capacity *= 2;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = capacity,
                             .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    if (vkCreateBuffer(r.device, &bi, NULL, &r.vertices[slot]) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r.device, r.vertices[slot], &req);
    uint32_t type = snx_vk_memory_type(&r.memory, req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    if (type == UINT32_MAX || vkAllocateMemory(r.device, &ai, NULL, &r.vertex_memory[slot]) != VK_SUCCESS) return false;
    vkBindBufferMemory(r.device, r.vertices[slot], r.vertex_memory[slot], 0);
    if (vkMapMemory(r.device, r.vertex_memory[slot], 0, VK_WHOLE_SIZE, 0, &r.vertex_map[slot]) != VK_SUCCESS)
        return false;
    r.vertex_capacity[slot] = capacity;
    return true;
}

static void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 src_stage, VkAccessFlags2 src, VkPipelineStageFlags2 dst_stage,
                    VkAccessFlags2 dst) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                               .srcStageMask = src_stage,
                               .srcAccessMask = src,
                               .dstStageMask = dst_stage,
                               .dstAccessMask = dst,
                               .oldLayout = from,
                               .newLayout = to,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image,
                               .subresourceRange = {aspect, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1,
                          .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

/* the target image read back and written as a binary PPM (RGB), the debug screenshot */
static void screenshot(VkImage image) {
    size_t size = (size_t)r.extent.width * r.extent.height * 4;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                             .size = size,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer buffer;
    if (vkCreateBuffer(r.device, &bi, NULL, &buffer) != VK_SUCCESS) return;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(r.device, buffer, &req);
    uint32_t type = snx_vk_memory_type(&r.memory, req.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (type == UINT32_MAX)
        type = snx_vk_memory_type(&r.memory, req.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    VkDeviceMemory memory;
    if (type == UINT32_MAX || vkAllocateMemory(r.device, &ai, NULL, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(r.device, buffer, NULL);
        return;
    }
    vkBindBufferMemory(r.device, buffer, memory, 0);
    VkCommandBuffer cb = snx_vk_ring_commands(&r.ring);
    VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                              .imageExtent = {r.extent.width, r.extent.height, 1}};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    snx_vk_ring_wait(&r.ring, snx_vk_ring_submit(&r.ring, VK_NULL_HANDLE, VK_NULL_HANDLE), UINT64_MAX);
    void* data;
    FILE* f = fopen(r.shot, "wb");
    if (f && vkMapMemory(r.device, memory, 0, VK_WHOLE_SIZE, 0, &data) == VK_SUCCESS) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = memory,
                                     .size = VK_WHOLE_SIZE};
        vkInvalidateMappedMemoryRanges(r.device, 1, &range);
        fprintf(f, "P6\n%u %u\n255\n", r.extent.width, r.extent.height);
        const unsigned char* px = data;
        bool bgra = r.color_format == VK_FORMAT_B8G8R8A8_UNORM;
        for (size_t i = 0; i < size; i += 4) {
            unsigned char rgb[3] = {px[i + (bgra ? 2 : 0)], px[i + 1], px[i + (bgra ? 0 : 2)]};
            fwrite(rgb, 1, 3, f);
        }
        vkUnmapMemory(r.device, memory);
        fprintf(stderr, "scarletnx-android: frame %ld saved as %s\n", r.frames, r.shot);
    }
    if (f) fclose(f);
    vkDestroyBuffer(r.device, buffer, NULL);
    vkFreeMemory(r.device, memory, NULL);
}

/* the frame's draws recorded and submitted, and presented to the window */
static void render(void) {
    VkExtent2D want = snx_vk_window_extent(&r.host, (VkExtent2D){(uint32_t)r.width, (uint32_t)r.height});
    if (want.width != r.extent.width || want.height != r.extent.height || (r.host.surface && !r.swap.swapchain))
        if (!make_targets(want)) return;
    if (!r.extent.width || !r.extent.height) return;
    VkCommandBuffer cb = snx_vk_ring_commands(&r.ring);
    unsigned slot = r.ring.slot;
    size_t bytes = r.vert_count * sizeof *r.verts;
    if (bytes && !vertex_room(slot, bytes)) return;
    if (bytes) memcpy(r.vertex_map[slot], r.verts, bytes);

    uint32_t image_index = 0;
    VkImage target = r.offscreen;
    VkImageView target_view = r.offscreen_view;
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    if (r.host.surface) {
        acquired = r.acquired[slot];
        VkResult res = vkAcquireNextImageKHR(r.device, r.swap.swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE,
                                             &image_index);
        if (res == VK_ERROR_OUT_OF_DATE_KHR) {
            make_targets(want);
            return;
        }
        if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) return;
        target = r.swap.images[image_index];
        target_view = r.swap_views[image_index];
        rendered = r.rendered[image_index];
    }
    const VkPipelineStageFlags2 color_stage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    const VkPipelineStageFlags2 stencil_stage = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                                                VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            color_stage, 0, color_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (r.msaa)
        barrier(cb, r.msaa, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, color_stage, 0, color_stage,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    VkImageAspectFlags stencil_aspect = r.stencil_format == VK_FORMAT_S8_UINT
                                            ? VK_IMAGE_ASPECT_STENCIL_BIT
                                            : VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    barrier(cb, r.stencil, stencil_aspect, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
            stencil_stage, 0, stencil_stage,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo color = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                       .imageView = r.msaa ? r.msaa_view : target_view,
                                       .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       .resolveMode = r.msaa ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE,
                                       .resolveImageView = r.msaa ? target_view : VK_NULL_HANDLE,
                                       .resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                       .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                       .storeOp = r.msaa ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE,
                                       .clearValue = {.color = {{0, 0, 0, 1}}}};
    VkRenderingAttachmentInfo stencil = {.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                         .imageView = r.stencil_view,
                                         .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                         .clearValue = {.depthStencil = {1, 0}}};
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                          .renderArea = {{0, 0}, r.extent},
                          .layerCount = 1,
                          .colorAttachmentCount = 1,
                          .pColorAttachments = &color,
                          .pDepthAttachment = r.stencil_format == VK_FORMAT_S8_UINT ? NULL : &stencil,
                          .pStencilAttachment = &stencil};
    vkCmdBeginRendering(cb, &ri);
    /* the frame's pixels onto the target's: the window keeps the app's size until the next frame lays out anew */
    VkViewport viewport = {0, 0, (float)r.extent.width, (float)r.extent.height, 0, 1};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    float scale[2] = {2.0f / (float)r.extent.width, 2.0f / (float)r.extent.height};
    vkCmdPushConstants(cb, r.layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof scale, scale);
    if (bytes) {
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &r.vertices[slot], &offset);
    }
    int bound = -1;
    for (size_t i = 0; i < r.draw_count; ++i) {
        const Draw* d = &r.draws[i];
        if (bound != d->pipe) vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, r.pipes[d->pipe]);
        bound = d->pipe;
        VkRect2D s = d->scissor;
        if ((uint32_t)s.offset.x >= r.extent.width || (uint32_t)s.offset.y >= r.extent.height) continue;
        if (s.offset.x + s.extent.width > r.extent.width) s.extent.width = r.extent.width - (uint32_t)s.offset.x;
        if (s.offset.y + s.extent.height > r.extent.height) s.extent.height = r.extent.height - (uint32_t)s.offset.y;
        vkCmdSetScissor(cb, 0, 1, &s);
        vkCmdDraw(cb, d->count, 1, d->first, 0);
    }
    vkCmdEndRendering(cb);
    ++r.frames;
    bool shot = r.shot[0] && r.frames == r.shot_frame;
    if (shot || !r.host.surface) {
        barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_stage, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    if (shot) {
        snx_vk_ring_submit(&r.ring, acquired, VK_NULL_HANDLE);
        acquired = VK_NULL_HANDLE;
        screenshot(target);
        cb = snx_vk_ring_commands(&r.ring);
    }
    if (r.host.surface) {
        barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT,
                shot ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, color_stage | VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        snx_vk_ring_submit(&r.ring, acquired, rendered);
        VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                               .waitSemaphoreCount = 1,
                               .pWaitSemaphores = &rendered,
                               .swapchainCount = 1,
                               .pSwapchains = &r.swap.swapchain,
                               .pImageIndices = &image_index};
        VkResult res = vkQueuePresentKHR(r.queue, &pi);
        if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR) r.swap.extent = (VkExtent2D){0, 0};
    } else if (!shot) {
        snx_vk_ring_submit(&r.ring, VK_NULL_HANDLE, VK_NULL_HANDLE);
    }
}

/* ---- the natives of snx.android.ui.Window ---- */

static jboolean JNICALL native_open(JNIEnv* env, jclass unused, jint width, jint height, jstring title) {
    (void)unused;
    if (r.up) return JNI_TRUE;
    const char* t = title ? (*env)->GetStringUTFChars(env, title, NULL) : NULL;
    bool ok = start(width, height, t ? t : "ScarletNX Android");
    if (t) (*env)->ReleaseStringUTFChars(env, title, t);
    return ok ? JNI_TRUE : JNI_FALSE;
}

static void JNICALL native_present(JNIEnv* env, jclass unused, jint width, jint height, jbyteArray list, jint size,
                                   jobjectArray bitmaps) {
    (void)unused;
    (void)bitmaps;
    if (!r.up || size < 0) return;
    unsigned char* bytes = malloc((size_t)size + 1);
    if (!bytes) return;
    (*env)->GetByteArrayRegion(env, list, 0, size, (jbyte*)bytes);
    r.width = width;
    r.height = height;
    r.vert_count = r.draw_count = 0;
    r.failed = false;
    r.state_count = 1;
    r.states[0] = (State){{1, 0, 0, 0, 1, 0}, {0, 0, (float)width, (float)height}, 1};
    Reader rd = {bytes, bytes + size, false};
    interpret(&rd);
    free(bytes);
    if (r.failed) fprintf(stderr, "scarletnx-android: a frame ran out of memory\n");
    else render();
}

/* the window's events since the last poll, 8 floats each into out (kind, action, x, y, key, modifiers, unicode,
   unused), the count of them. Kinds: 1 a pointer, 2 a key, 3 a size, 4 the window closed */
static jint JNICALL native_poll(JNIEnv* env, jclass unused, jfloatArray out) {
    (void)unused;
    if (!r.host.window) return 0;
    jsize room = (*env)->GetArrayLength(env, out) / 8;
    jfloat events[8 * 64];
    if (room > 64) room = 64;
    jint n = 0;
    SDL_Event e;
    while (n < room && SDL_PollEvent(&e)) {
        jfloat* v = events + 8 * n;
        memset(v, 0, 8 * sizeof *v);
        switch (e.type) {
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            if (e.button.button != SDL_BUTTON_LEFT) continue;
            v[0] = 1, v[1] = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? 0 : 1;
            v[2] = e.button.x, v[3] = e.button.y;
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (!(e.motion.state & SDL_BUTTON_LMASK)) continue;
            v[0] = 1, v[1] = 2, v[2] = e.motion.x, v[3] = e.motion.y;
            break;
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
            v[0] = 2, v[1] = e.type == SDL_EVENT_KEY_DOWN ? 0 : 1;
            v[4] = (jfloat)e.key.key, v[5] = (jfloat)e.key.mod;
            break;
        case SDL_EVENT_TEXT_INPUT: {
            /* each character of the text, as a key without a key code */
            const unsigned char* s = (const unsigned char*)e.text.text;
            while (*s && n < room) {
                uint32_t cp = *s++;
                if (cp >= 0xc0) {
                    int extra = cp >= 0xf0 ? 3 : cp >= 0xe0 ? 2 : 1;
                    cp &= 0x3fu >> extra;
                    for (int k = 0; k < extra && (*s & 0xc0) == 0x80; ++k) cp = cp << 6 | (*s++ & 0x3fu);
                }
                v = events + 8 * n;
                memset(v, 0, 8 * sizeof *v);
                v[0] = 2, v[1] = 3, v[6] = (jfloat)cp;
                ++n;
            }
            continue;
        }
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
            v[0] = 3, v[2] = (jfloat)e.window.data1, v[3] = (jfloat)e.window.data2;
            break;
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED: v[0] = 4; break;
        default: continue;
        }
        ++n;
    }
    if (n) (*env)->SetFloatArrayRegion(env, out, 0, 8 * n, events);
    return n;
}

static jboolean JNICALL native_has_window(JNIEnv* env, jclass unused) {
    (void)env;
    (void)unused;
    return r.host.window ? JNI_TRUE : JNI_FALSE;
}

bool android_render_register(JNIEnv* env) {
    static const JNINativeMethod methods[] = {
        {(char*)"nativeOpen", (char*)"(IILjava/lang/String;)Z", (void*)native_open},
        {(char*)"nativePresent", (char*)"(II[BI[Landroid/graphics/Bitmap;)V", (void*)native_present},
        {(char*)"nativePoll", (char*)"([F)I", (void*)native_poll},
        {(char*)"nativeHasWindow", (char*)"()Z", (void*)native_has_window},
    };
    return android_jvm_register(env, "snx/android/ui/Window", methods, (int)(sizeof methods / sizeof *methods));
}

void android_render_close(void) {
    if (!r.device) return;
    vkDeviceWaitIdle(r.device);
    free_targets();
    snx_vk_swapchain_free(r.device, &r.swap);
    for (int i = 0; i < PIPE_COUNT; ++i)
        if (r.pipes[i]) vkDestroyPipeline(r.device, r.pipes[i], NULL);
    if (r.layout) vkDestroyPipelineLayout(r.device, r.layout, NULL);
    for (unsigned i = 0; i < SLOTS; ++i) {
        if (r.vertices[i]) vkDestroyBuffer(r.device, r.vertices[i], NULL);
        if (r.vertex_memory[i]) vkFreeMemory(r.device, r.vertex_memory[i], NULL);
        if (r.acquired[i]) vkDestroySemaphore(r.device, r.acquired[i], NULL);
    }
    for (unsigned i = 0; i < SNX_VK_SWAP_IMAGES; ++i)
        if (r.rendered[i]) vkDestroySemaphore(r.device, r.rendered[i], NULL);
    snx_vk_ring_free(&r.ring);
    vkDestroyDevice(r.device, NULL);
    snx_vk_window_close(&r.host);
    for (size_t i = 0; i < r.glyph_capacity; ++i) {
        free(r.glyphs[i].verbs);
        free(r.glyphs[i].points);
    }
    free(r.glyphs);
    free(r.verts);
    free(r.draws);
    free(r.states);
    poly_free(&r.poly);
    tri_free(&r.tris);
    memset(&r, 0, sizeof r);
}
