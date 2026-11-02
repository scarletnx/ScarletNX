/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_PATH_H
#define SNX_ANDROID_PATH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The CPU half of the libhwui HLE: paths flattened to polygons and strokes made into triangles, in device pixels,
   for the renderer's stencil-then-cover fill (render.c). Curves are flattened with Wang's formula to a tolerance in
   device pixels */

/* an affine transform, Android's Matrix order: x' = a*x + b*y + c, y' = d*x + e*y + f */
typedef struct {
    float a, b, c, d, e, f;
} Affine;
Affine affine_mul(Affine outer, Affine inner);
/* the largest scale of the transform, for tolerances in local units */
float affine_scale(Affine m);

typedef struct {
    float x, y;
} Point;

/* pi as a float: strict C99 has no M_PI */
#define HWUI_PI 3.14159265358979323846f

/* flattened contours: points, and where each contour ends in them */
typedef struct {
    Point* points;
    size_t count, capacity;
    size_t* ends; /* the index past each contour's last point */
    size_t contours, ends_capacity;
    bool* closed; /* each contour's: a CLOSE verb ended it */
    bool failed;  /* out of memory */
} Polyline;
void poly_free(Polyline* p);
void poly_clear(Polyline* p);
void poly_move(Polyline* p, Point at);
void poly_line(Polyline* p, Point to);
void poly_quad(Polyline* p, Point c, Point to, float tolerance);
void poly_cubic(Polyline* p, Point c1, Point c2, Point to, float tolerance);
void poly_close(Polyline* p);
/* the current contour ended without a close */
void poly_end(Polyline* p);

/* Android's path verbs, as Path records them */
enum { VERB_MOVE = 0, VERB_LINE = 1, VERB_QUAD = 2, VERB_CUBIC = 3, VERB_CLOSE = 4 };
/* a recorded path (verbs, then x and y of each point, little-endian floats at points) flattened through m */
void poly_path(Polyline* p, const uint8_t* verbs, uint32_t verb_count, const unsigned char* points,
               uint32_t point_count, Affine m, float tolerance);

/* triangles as a list of points, three for each */
typedef struct {
    Point* points;
    size_t count, capacity;
    bool failed;
} Triangles;
void tri_free(Triangles* t);
void tri_add(Triangles* t, Point a, Point b, Point c);

enum { CAP_BUTT, CAP_ROUND, CAP_SQUARE };
enum { JOIN_MITER, JOIN_ROUND, JOIN_BEVEL };
/* The stroke of the contours, already in device pixels, as triangles that may overlap (the renderer fills them through
   the stencil, so that each pixel is drawn once). width is the stroke's in device pixels */
void stroke_poly(Triangles* out, const Polyline* p, float width, int cap, int join, float miter, float tolerance);
/* the triangles of a fan over each contour, for the stencil's winding count */
void fan_poly(Triangles* out, const Polyline* p);
#endif
