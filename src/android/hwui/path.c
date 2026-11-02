/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Paths flattened and stroked on the CPU (path.h) */
#include "android/hwui/path.h"
#include "common/utilities/endian.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

Affine affine_mul(Affine o, Affine i) {
    Affine r = {o.a * i.a + o.b * i.d, o.a * i.b + o.b * i.e, o.a * i.c + o.b * i.f + o.c,
                o.d * i.a + o.e * i.d, o.d * i.b + o.e * i.e, o.d * i.c + o.e * i.f + o.f};
    return r;
}
float affine_scale(Affine m) {
    float sx = sqrtf(m.a * m.a + m.d * m.d), sy = sqrtf(m.b * m.b + m.e * m.e);
    return sx > sy ? sx : sy;
}
static Point apply(Affine m, float x, float y) {
    Point p = {m.a * x + m.b * y + m.c, m.d * x + m.e * y + m.f};
    return p;
}

/* a growing array's room for one more: false when memory ran out */
static bool grow(void** data, size_t* capacity, size_t count, size_t size) {
    if (count < *capacity) return true;
    size_t n = *capacity ? *capacity * 2 : 64;
    void* d = realloc(*data, n * size);
    if (!d) return false;
    *data = d;
    *capacity = n;
    return true;
}

void poly_free(Polyline* p) {
    free(p->points);
    free(p->ends);
    free(p->closed);
    *p = (Polyline){0};
}
void poly_clear(Polyline* p) {
    p->count = p->contours = 0;
    p->failed = false;
}
static void point(Polyline* p, Point at) {
    if (p->failed || !grow((void**)&p->points, &p->capacity, p->count, sizeof *p->points)) {
        p->failed = true;
        return;
    }
    p->points[p->count++] = at;
}
/* the current contour's first point index */
static size_t contour_start(const Polyline* p) {
    return p->contours ? p->ends[p->contours - 1] : 0;
}
static bool open_contour(const Polyline* p) {
    return p->count > contour_start(p);
}
static void end_contour(Polyline* p, bool closed) {
    if (!open_contour(p)) return;
    if (p->count - contour_start(p) < 2) { /* a lone point draws nothing */
        p->count = contour_start(p);
        return;
    }
    size_t cap = p->ends_capacity;
    if (p->failed || !grow((void**)&p->ends, &cap, p->contours, sizeof *p->ends)) {
        p->failed = true;
        return;
    }
    size_t cap2 = p->ends_capacity;
    if (!grow((void**)&p->closed, &cap2, p->contours, sizeof *p->closed)) {
        p->failed = true;
        return;
    }
    p->ends_capacity = cap < cap2 ? cap : cap2;
    p->ends[p->contours] = p->count;
    p->closed[p->contours] = closed;
    ++p->contours;
}
void poly_end(Polyline* p) {
    end_contour(p, false);
}
void poly_move(Polyline* p, Point at) {
    end_contour(p, false);
    point(p, at);
}
static Point last(const Polyline* p) {
    return p->points[p->count - 1];
}
void poly_line(Polyline* p, Point to) {
    if (!open_contour(p)) point(p, (Point){0, 0});
    point(p, to);
}
/* Wang's formula: the segments that keep a curve of degree n within tolerance of its chords */
static unsigned segments(float dd, float factor, float tolerance) {
    float n = ceilf(sqrtf(factor * dd / tolerance));
    if (!(n >= 1)) return 1;
    return n > 128 ? 128 : (unsigned)n;
}
void poly_quad(Polyline* p, Point c, Point to, float tolerance) {
    if (!open_contour(p)) point(p, (Point){0, 0});
    Point s = last(p);
    float dx = s.x - 2 * c.x + to.x, dy = s.y - 2 * c.y + to.y;
    unsigned n = segments(sqrtf(dx * dx + dy * dy), 0.25f, tolerance);
    for (unsigned i = 1; i <= n; ++i) {
        float t = (float)i / (float)n, u = 1 - t;
        point(p, (Point){u * u * s.x + 2 * u * t * c.x + t * t * to.x, u * u * s.y + 2 * u * t * c.y + t * t * to.y});
    }
}
void poly_cubic(Polyline* p, Point c1, Point c2, Point to, float tolerance) {
    if (!open_contour(p)) point(p, (Point){0, 0});
    Point s = last(p);
    float ax = s.x - 2 * c1.x + c2.x, ay = s.y - 2 * c1.y + c2.y;
    float bx = c1.x - 2 * c2.x + to.x, by = c1.y - 2 * c2.y + to.y;
    float da = sqrtf(ax * ax + ay * ay), db = sqrtf(bx * bx + by * by);
    unsigned n = segments(da > db ? da : db, 0.75f, tolerance);
    for (unsigned i = 1; i <= n; ++i) {
        float t = (float)i / (float)n, u = 1 - t;
        float w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        point(p, (Point){w0 * s.x + w1 * c1.x + w2 * c2.x + w3 * to.x, w0 * s.y + w1 * c1.y + w2 * c2.y + w3 * to.y});
    }
}
void poly_close(Polyline* p) {
    end_contour(p, true);
}

void poly_path(Polyline* p, const uint8_t* verbs, uint32_t verb_count, const unsigned char* points,
               uint32_t point_count, Affine m, float tolerance) {
    uint32_t k = 0; /* the next float */
    Point start = {0, 0};
    for (uint32_t i = 0; i < verb_count; ++i) {
        unsigned need = verbs[i] == VERB_MOVE || verbs[i] == VERB_LINE ? 2 : verbs[i] == VERB_QUAD ? 4
                        : verbs[i] == VERB_CUBIC ? 6 : 0;
        if (k + need > point_count) break;
        Point q[3];
        for (unsigned j = 0; j < need / 2; ++j) {
            const unsigned char* at = points + 4 * (k + 2 * j);
            q[j] = apply(m, snx_ld_lef32(at), snx_ld_lef32(at + 4));
        }
        k += need;
        switch (verbs[i]) {
        case VERB_MOVE:
            poly_move(p, q[0]);
            start = q[0];
            break;
        case VERB_LINE: poly_line(p, q[0]); break;
        case VERB_QUAD: poly_quad(p, q[0], q[1], tolerance); break;
        case VERB_CUBIC: poly_cubic(p, q[0], q[1], q[2], tolerance); break;
        case VERB_CLOSE:
            poly_close(p);
            /* a contour after a close without a move starts where the closed one did */
            if (i + 1 < verb_count && verbs[i + 1] != VERB_MOVE) point(p, start);
            break;
        default: break;
        }
    }
    poly_end(p);
}

void tri_free(Triangles* t) {
    free(t->points);
    *t = (Triangles){0};
}
void tri_add(Triangles* t, Point a, Point b, Point c) {
    if (t->failed) return;
    if (t->capacity - t->count < 3) {
        size_t n = t->capacity ? t->capacity * 2 : 192;
        Point* d = realloc(t->points, n * sizeof *d);
        if (!d) {
            t->failed = true;
            return;
        }
        t->points = d;
        t->capacity = n;
    }
    t->points[t->count++] = a;
    t->points[t->count++] = b;
    t->points[t->count++] = c;
}

void fan_poly(Triangles* out, const Polyline* p) {
    size_t begin = 0;
    for (size_t c = 0; c < p->contours; begin = p->ends[c++]) {
        size_t end = p->ends[c];
        for (size_t i = begin + 1; i + 1 < end; ++i) tri_add(out, p->points[begin], p->points[i], p->points[i + 1]);
    }
}

/* ---- strokes ---- */
static Point add(Point a, Point b) {
    return (Point){a.x + b.x, a.y + b.y};
}
static Point sub(Point a, Point b) {
    return (Point){a.x - b.x, a.y - b.y};
}
static Point mul(Point a, float s) {
    return (Point){a.x * s, a.y * s};
}
/* the unit normal of a to b (to its left), zero for a point */
static Point normal(Point a, Point b) {
    float dx = b.x - a.x, dy = b.y - a.y, l = sqrtf(dx * dx + dy * dy);
    return l > 0 ? (Point){-dy / l, dx / l} : (Point){0, 0};
}
/* a fan of the circle's arc around c from angle a0 through sweep, radius r */
static void arc_fan(Triangles* out, Point c, float r, float a0, float sweep, float tolerance) {
    float step = r > tolerance ? 2 * acosf(1 - tolerance / r) : 1.0f;
    if (!(step > 0.05f)) step = 0.05f;
    unsigned n = (unsigned)ceilf(fabsf(sweep) / step);
    if (n < 1) n = 1;
    Point prev = {c.x + r * cosf(a0), c.y + r * sinf(a0)};
    for (unsigned i = 1; i <= n; ++i) {
        float a = a0 + sweep * (float)i / (float)n;
        Point next = {c.x + r * cosf(a), c.y + r * sinf(a)};
        tri_add(out, c, prev, next);
        prev = next;
    }
}
static void join(Triangles* out, Point at, Point n0, Point n1, float h, int kind, float miter, float tolerance) {
    float cross = n0.x * n1.y - n0.y * n1.x, dot = n0.x * n1.x + n0.y * n1.y;
    if (fabsf(cross) < 1e-6f && dot > 0) return; /* straight on */
    /* the outer side: the normals' side the turn opens */
    float side = cross > 0 ? -1.0f : 1.0f;
    Point a = add(at, mul(n0, h * side)), b = add(at, mul(n1, h * side));
    if (kind == JOIN_ROUND) {
        float a0 = atan2f(n0.y * side, n0.x * side), a1 = atan2f(n1.y * side, n1.x * side), sweep = a1 - a0;
        while (sweep > HWUI_PI) sweep -= 2 * HWUI_PI;
        while (sweep < -HWUI_PI) sweep += 2 * HWUI_PI;
        arc_fan(out, at, h, a0, sweep, tolerance);
        return;
    }
    tri_add(out, at, a, b); /* the bevel */
    if (kind != JOIN_MITER) return;
    /* the miter's tip, while its length over the half width stays within the limit */
    Point mid = {n0.x + n1.x, n0.y + n1.y};
    float ml = sqrtf(mid.x * mid.x + mid.y * mid.y);
    if (ml < 1e-6f) return;
    float cos_half = ml / 2, ratio = 1 / cos_half;
    if (ratio > miter) return;
    Point tip = add(at, mul(mid, h * side * ratio / ml));
    tri_add(out, a, tip, b);
}
static void cap(Triangles* out, Point at, Point dir, Point n, float h, int kind, float tolerance) {
    if (kind == CAP_ROUND) {
        float a0 = atan2f(n.y, n.x);
        arc_fan(out, at, h, a0, HWUI_PI, tolerance);
        return;
    }
    if (kind != CAP_SQUARE) return;
    Point e = mul(dir, h), l = add(at, mul(n, h)), r = sub(at, mul(n, h));
    tri_add(out, l, r, add(r, e));
    tri_add(out, l, add(r, e), add(l, e));
}
void stroke_poly(Triangles* out, const Polyline* p, float width, int cap_kind, int join_kind, float miter,
                 float tolerance) {
    float h = width / 2;
    size_t begin = 0;
    for (size_t c = 0; c < p->contours; begin = p->ends[c++]) {
        size_t end = p->ends[c], n = end - begin;
        bool closed = p->closed[c];
        /* the segments, without the ones of zero length */
        Point first_n = {0, 0}, prev_n = {0, 0};
        bool any = false;
        size_t segs = closed ? n : n - 1;
        for (size_t i = 0; i < segs; ++i) {
            Point a = p->points[begin + i], b = p->points[begin + (i + 1) % n];
            Point nn = normal(a, b);
            if (nn.x == 0 && nn.y == 0) continue;
            Point o = mul(nn, h);
            tri_add(out, add(a, o), sub(a, o), sub(b, o));
            tri_add(out, add(a, o), sub(b, o), add(b, o));
            if (any) join(out, a, prev_n, nn, h, join_kind, miter, tolerance);
            else first_n = nn;
            prev_n = nn;
            any = true;
        }
        if (!any) {
            /* a contour of one point: a round or square cap draws a dot */
            Point at = p->points[begin];
            if (cap_kind == CAP_ROUND) arc_fan(out, at, h, 0, 2 * HWUI_PI, tolerance);
            else if (cap_kind == CAP_SQUARE)
                cap(out, sub(at, (Point){h, 0}), (Point){1, 0}, (Point){0, 1}, h, CAP_SQUARE, tolerance);
            continue;
        }
        if (closed) {
            join(out, p->points[begin], prev_n, first_n, h, join_kind, miter, tolerance);
        } else {
            Point s = p->points[begin], e = p->points[end - 1];
            cap(out, s, (Point){first_n.y, -first_n.x}, mul(first_n, -1), h, cap_kind, tolerance);
            cap(out, e, (Point){-prev_n.y, prev_n.x}, prev_n, h, cap_kind, tolerance);
        }
    }
}
