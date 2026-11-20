/* SPDX-License-Identifier: MIT
   Copyright (C) 2013 Jorge Jimenez, Jose I. Echevarria, Belen Masia, Fernando Navarro and Diego Gutierrez
   Copyright 2026 the ScarletNX contributors */
/* areatexgen: SMAA's area texture, AreaTex.h, a C form of the SMAA distribution's Scripts/AreaTex.py (iryoku/smaa
   71c806a838) that writes the header its Textures/AreaTex.h is, byte for byte: 160x560 R8G8, for each edge pattern
   and each distance to the pattern's two ends the area the line through it takes of the pixel, on each side. The left
   half holds the 16 orthogonal patterns at 7 subsample offsets, their distances 0 to 15 standing for their squares,
   the right half the 16 diagonal ones at 5 offsets. Each value is the script's double arithmetic in the script's
   order, so every rounding has to stay as it is: no fused multiply-add (cmake/areatex.cmake builds this with
   -ffp-contract=off) and no wider evaluation (FLT_EVAL_METHOD 0). A value the 0..255 bytes cannot hold stops it,
   named. It is strict ISO C23 and depends on nothing, so a cross build can build it for the machine it runs on.

   Usage: areatexgen OUT.h (rewritten only when it changes) */
#include <float.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if FLT_EVAL_METHOD != 0
#error "areatexgen needs each double operation rounded to double (FLT_EVAL_METHOD 0), as AreaTex.py's are"
#endif

#define WIDTH               160
#define HEIGHT              560
#define BYTES               (WIDTH * HEIGHT * 2)
#define SIZE_ORTHO          16 /* each orthogonal pattern's distances; the 5x5 slots of a subsample offset: 80 */
#define SIZE_DIAG           20 /* each diagonal pattern's; its 4x4 slots: 80 */
#define SAMPLES_DIAG        30 /* each way, in a pixel of a diagonal pattern */
#define SMOOTH_MAX_DISTANCE 32 /* the U patterns are smoothed up to it */
#define PER_LINE            12 /* the header's bytes on each line */

typedef struct {
    double x, y;
} Vec2;

static const double ortho_offsets[] = {0.0, -0.25, 0.25, -0.125, 0.125, -0.375, 0.375};
static const Vec2 diag_offsets[] = {{0.0, 0.0}, {0.25, -0.25}, {-0.25, 0.25}, {0.125, -0.125}, {-0.125, 0.125}};
static_assert(sizeof ortho_offsets / sizeof ortho_offsets[0] * 5 * SIZE_ORTHO == HEIGHT, "the orthogonal rows");
static_assert(sizeof diag_offsets / sizeof diag_offsets[0] * 4 * SIZE_DIAG <= HEIGHT, "the diagonal rows");

/* each pattern's slot, column and row, in its offset's square */
static const unsigned char edges_ortho[16][2] = {{0, 0}, {3, 0}, {0, 3}, {3, 3}, {1, 0}, {4, 0}, {1, 3}, {4, 3},
                                                 {0, 1}, {3, 1}, {0, 4}, {3, 4}, {1, 1}, {4, 1}, {1, 4}, {4, 4}};
static const unsigned char edges_diag[16][2] = {{0, 0}, {1, 0}, {0, 2}, {1, 2}, {2, 0}, {3, 0}, {2, 2}, {3, 2},
                                                {0, 1}, {1, 1}, {0, 3}, {1, 3}, {2, 1}, {3, 1}, {2, 3}, {3, 3}};

/* each diagonal pattern's line, from (x1, y1) to (x2 + d, y2 + d), d the pattern's length; two where the pattern's
   ends are not known (an L and a U end differently), whose areas are blended, as areadiag lists them */
static const struct {
    unsigned char n;
    unsigned char line[2][4]; /* x1, y1, x2, y2 */
} diag_lines[16] = {
    {2, {{1, 1, 1, 1}, {1, 0, 1, 0}}},
    {2, {{1, 0, 0, 0}, {1, 0, 1, 0}}},
    {2, {{0, 0, 1, 0}, {1, 0, 1, 0}}},
    {1, {{1, 0, 1, 0}}},
    {2, {{1, 1, 0, 0}, {1, 1, 1, 0}}},
    {2, {{1, 1, 0, 0}, {1, 0, 1, 0}}},
    {1, {{1, 1, 1, 0}}},
    {2, {{1, 1, 1, 0}, {1, 0, 1, 0}}},
    {2, {{0, 0, 1, 1}, {1, 0, 1, 1}}},
    {1, {{1, 0, 1, 1}}},
    {2, {{0, 0, 1, 1}, {1, 0, 1, 0}}},
    {2, {{1, 0, 1, 1}, {1, 0, 1, 0}}},
    {1, {{1, 1, 1, 1}}},
    {2, {{1, 1, 1, 1}, {1, 0, 1, 1}}},
    {2, {{1, 1, 1, 1}, {1, 1, 1, 0}}},
    {2, {{1, 1, 1, 1}, {1, 0, 1, 0}}},
};

/* what the header says around the bytes, as the distribution's file has it */
static const char header_start[] =
    "/**\n"
    " * Copyright (C) 2013 Jorge Jimenez (jorge@iryoku.com)\n"
    " * Copyright (C) 2013 Jose I. Echevarria (joseignacioechevarria@gmail.com)\n"
    " * Copyright (C) 2013 Belen Masia (bmasia@unizar.es)\n"
    " * Copyright (C) 2013 Fernando Navarro (fernandn@microsoft.com)\n"
    " * Copyright (C) 2013 Diego Gutierrez (diegog@unizar.es)\n"
    " *\n"
    " * Permission is hereby granted, free of charge, to any person obtaining a copy\n"
    " * this software and associated documentation files (the \"Software\"), to deal in\n"
    " * the Software without restriction, including without limitation the rights to\n"
    " * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies\n"
    " * of the Software, and to permit persons to whom the Software is furnished to\n"
    " * do so, subject to the following conditions:\n"
    " *\n"
    " * The above copyright notice and this permission notice shall be included in\n"
    " * all copies or substantial portions of the Software. As clarification, there\n"
    " * is no requirement that the copyright notice and permission be included in\n"
    " * binary distributions of the Software.\n"
    " *\n"
    " * THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
    " * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
    " * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
    " * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
    " * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
    " * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
    " * SOFTWARE.\n"
    " */\n"
    "\n"
    "\n"
    "#ifndef AREATEX_H\n"
    "#define AREATEX_H\n"
    "\n"
    "#define AREATEX_WIDTH 160\n"
    "#define AREATEX_HEIGHT 560\n"
    "#define AREATEX_PITCH (AREATEX_WIDTH * 2)\n"
    "#define AREATEX_SIZE (AREATEX_HEIGHT * AREATEX_PITCH)\n"
    "\n"
    "/**\n"
    " * Stored in R8G8 format. Load it in the following format:\n"
    " *  - DX9:  D3DFMT_A8L8 \n"
    " *  - DX10: DXGI_FORMAT_R8G8_UNORM\n"
    " */\n"
    "static const unsigned char areaTexBytes[] = {\n";
static const char header_end[] = "};\n\n#endif\n";

/* "    0x00, " for each byte, the last without ", ", and "\n" after each line */
#define TEXT_MAX (sizeof header_start - 1 + BYTES * 6 + (BYTES / PER_LINE + 1) * 5 + sizeof header_end - 1)

static struct {
    unsigned char tex[HEIGHT][WIDTH][2];
    char text[TEXT_MAX];
    size_t text_n;
} g_areatexgen;

[[noreturn]] static void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("areatexgen: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* The area under the line p1->p2 over the pixel x..x+1, the part below the edge and the part above it (areaortho's
   area) */
static Vec2 ortho_line(Vec2 p1, Vec2 p2, int x) {
    double dx = p2.x - p1.x, dy = p2.y - p1.y;
    double x1 = (double)x, x2 = x + 1.0;
    double y1 = p1.y + dy * (x1 - p1.x) / dx;
    double y2 = p1.y + dy * (x2 - p1.x) / dx;
    if (!((x1 >= p1.x && x1 < p2.x) || (x2 > p1.x && x2 <= p2.x))) return (Vec2){0.0, 0.0};
    if (copysign(1.0, y1) == copysign(1.0, y2) || fabs(y1) < 1e-4 || fabs(y2) < 1e-4) {
        double a = (y1 + y2) / 2.0; /* a trapezoid */
        return a < 0.0 ? (Vec2){fabs(a), 0.0} : (Vec2){0.0, fabs(a)};
    }
    /* two triangles, on either side of where the line crosses the edge */
    double cross = -p1.y * dx / dy + p1.x;
    double whole = 0.0;
    double frac = modf(cross, &whole);
    double a1 = cross > p1.x ? y1 * frac / 2.0 : 0.0;
    double a2 = cross < p2.x ? y2 * (1.0 - frac) / 2.0 : 0.0;
    double a = fabs(a1) > fabs(a2) ? a1 : -a2;
    return a < 0.0 ? (Vec2){fabs(a1), fabs(a2)} : (Vec2){fabs(a2), fabs(a1)};
}

static Vec2 add(Vec2 a, Vec2 b) { return (Vec2){a.x + b.x, a.y + b.y}; }

static Vec2 half(Vec2 a) { return (Vec2){a.x / 2.0, a.y / 2.0}; }

/* a U pattern's two halves, a1 and a2, smoothed toward their square roots the shorter it is (smootharea), and
   summed */
static Vec2 smooth(int d, Vec2 a1, Vec2 a2) {
    Vec2 b1 = {sqrt(2.0 * a1.x) * 0.5, sqrt(2.0 * a1.y) * 0.5};
    Vec2 b2 = {sqrt(2.0 * a2.x) * 0.5, sqrt(2.0 * a2.y) * 0.5};
    double p = d / (double)SMOOTH_MAX_DISTANCE;
    p = p < 0.0 ? 0.0 : p > 1.0 ? 1.0 : p;
    Vec2 s1 = {b1.x + (a1.x - b1.x) * p, b1.y + (a1.y - b1.y) * p};
    Vec2 s2 = {b2.x + (a2.x - b2.x) * p, b2.y + (a2.y - b2.y) * p};
    return add(s1, s2);
}

/* An orthogonal pattern's area at left and right, the distances to its ends, biased by a subsample offset
   (areaortho). An L pattern is offset only on its crossing edge's side, so that it meets the unfiltered pattern 0; a Z
   pattern blends the whole offset line with the two offset Ls, so that its middle, which sees the Z, and its sides,
   which may see an L, agree */
static Vec2 area_ortho(int pattern, int left, int right, double offset) {
    int d = left + right + 1;
    double o1 = 0.5 + offset;
    double o2 = 0.5 + offset - 1.0;
    Vec2 start1 = {0.0, o1}, start2 = {0.0, o2}, mid = {d / 2.0, 0.0}, end1 = {d, o1}, end2 = {d, o2};
    switch (pattern) {
        case 1: return left <= right ? ortho_line(start2, mid, left) : (Vec2){0.0, 0.0};
        case 2: return left >= right ? ortho_line(mid, end2, left) : (Vec2){0.0, 0.0};
        case 3: return smooth(d, ortho_line(start2, mid, left), ortho_line(mid, end2, left));
        case 4: return left <= right ? ortho_line(start1, mid, left) : (Vec2){0.0, 0.0};
        case 6:
            if (fabs(offset) > 0.0)
                return half(add(ortho_line(start1, end2, left),
                                add(ortho_line(start1, mid, left), ortho_line(mid, end2, left))));
            return ortho_line(start1, end2, left);
        case 7: return ortho_line(start1, end2, left);
        case 8: return left >= right ? ortho_line(mid, end1, left) : (Vec2){0.0, 0.0};
        case 9:
            if (fabs(offset) > 0.0)
                return half(add(ortho_line(start2, end1, left),
                                add(ortho_line(start2, mid, left), ortho_line(mid, end1, left))));
            return ortho_line(start2, end1, left);
        case 11: return ortho_line(start2, end1, left);
        case 12: return smooth(d, ortho_line(start1, mid, left), ortho_line(mid, end1, left));
        case 13: return ortho_line(start2, end1, left);
        case 14: return ortho_line(start1, end2, left);
        default: return (Vec2){0.0, 0.0}; /* 0, 5, 10 and 15: no line to follow */
    }
}

/* The share of the pixel at p that lies on the far side of the line p1->p2, by brute force: 30x30 samples from its
   corner to the opposite one (areadiag's area1). All of it when the line is a point */
static double diag_samples(Vec2 p1, Vec2 p2, Vec2 p) {
    if (p1.x == p2.x && p1.y == p2.y) return 1.0;
    double xm = (p1.x + p2.x) / 2.0, ym = (p1.y + p2.y) / 2.0;
    double a = p2.y - p1.y, b = p1.x - p2.x;
    int n = 0;
    for (int x = 0; x < SAMPLES_DIAG; x++)
        for (int y = 0; y < SAMPLES_DIAG; y++) {
            double sx = p.x + x / (double)(SAMPLES_DIAG - 1), sy = p.y + y / (double)(SAMPLES_DIAG - 1);
            if (a * (sx - xm) + b * (sy - ym) > 0) n++;
        }
    return n / (double)(SAMPLES_DIAG * SAMPLES_DIAG);
}

/* A diagonal pattern's area at left, the pixel and its opposite (areadiag's area): the offset moves each end that has
   a crossing edge */
static Vec2 diag_line(int pattern, Vec2 p1, Vec2 p2, int left, Vec2 offset) {
    if (edges_diag[pattern][0] > 0) p1 = add(p1, offset);
    if (edges_diag[pattern][1] > 0) p2 = add(p2, offset);
    double a1 = diag_samples(p1, p2, (Vec2){1.0 + left, 0.0 + left});
    double a2 = diag_samples(p1, p2, (Vec2){1.0 + left, 1.0 + left});
    return (Vec2){1.0 - a1, a2};
}

static Vec2 area_diag(int pattern, int left, int right, Vec2 offset) {
    int d = left + right + 1;
    Vec2 a[2] = {0};
    for (int i = 0; i < diag_lines[pattern].n; i++) {
        const unsigned char* l = diag_lines[pattern].line[i];
        a[i] = diag_line(pattern, (Vec2){l[0], l[1]}, (Vec2){l[2] + (double)d, l[3] + (double)d}, left, offset);
    }
    return diag_lines[pattern].n == 2 ? half(add(a[0], a[1])) : a[0];
}

/* one texel, each area as the script's int(255.0 * a) takes it */
static void put(int x, int y, Vec2 area, const char* kind, int pattern, int left, int right, int slot) {
    double v[2] = {255.0 * area.x, 255.0 * area.y};
    for (int c = 0; c < 2; c++) {
        if (!(v[c] > -1.0 && v[c] < 256.0))
            fail("the %s pattern %d at distances %d and %d, offset %d, gives the area %.17g, which a byte from 0 to "
                 "255 cannot hold",
                 kind, pattern, left, right, slot, c ? area.y : area.x);
        g_areatexgen.tex[y][x][c] = (unsigned char)(int)v[c];
    }
}

static void build(void) {
    for (int k = 0; k < (int)(sizeof ortho_offsets / sizeof ortho_offsets[0]); k++)
        for (int pattern = 0; pattern < 16; pattern++)
            for (int left = 0; left < SIZE_ORTHO; left++)
                for (int right = 0; right < SIZE_ORTHO; right++) {
                    /* the distances squared: a slot of 16 reaches 225 pixels */
                    Vec2 a = area_ortho(pattern, left * left, right * right, ortho_offsets[k]);
                    put(SIZE_ORTHO * edges_ortho[pattern][0] + left,
                        5 * SIZE_ORTHO * k + SIZE_ORTHO * edges_ortho[pattern][1] + right, a, "orthogonal", pattern,
                        left * left, right * right, k);
                }
    for (int k = 0; k < (int)(sizeof diag_offsets / sizeof diag_offsets[0]); k++)
        for (int pattern = 0; pattern < 16; pattern++)
            for (int left = 0; left < SIZE_DIAG; left++)
                for (int right = 0; right < SIZE_DIAG; right++)
                    put(5 * SIZE_ORTHO + SIZE_DIAG * edges_diag[pattern][0] + left,
                        4 * SIZE_DIAG * k + SIZE_DIAG * edges_diag[pattern][1] + right,
                        area_diag(pattern, left, right, diag_offsets[k]), "diagonal", pattern, left, right, k);
}

static void text(const char* s, size_t n) {
    if (n > TEXT_MAX - g_areatexgen.text_n) fail("the header outgrows its %zu bytes", (size_t)TEXT_MAX);
    memcpy(g_areatexgen.text + g_areatexgen.text_n, s, n);
    g_areatexgen.text_n += n;
}

static void write_header(const char* path) {
    static const char hex[] = "0123456789abcdef";
    const unsigned char* bytes = &g_areatexgen.tex[0][0][0];
    text(header_start, sizeof header_start - 1);
    for (size_t i = 0; i < BYTES; i++) {
        char entry[6] = {'0', 'x', hex[bytes[i] >> 4], hex[bytes[i] & 15], ',', ' '};
        if (i % PER_LINE == 0) text("    ", 4);
        text(entry, i + 1 < BYTES ? 6 : 4);
        if (i + 1 == BYTES || (i + 1) % PER_LINE == 0) text("\n", 1);
    }
    text(header_end, sizeof header_end - 1);

    FILE* f = fopen(path, "rb");
    if (f) {
        char* old = malloc(g_areatexgen.text_n + 1);
        size_t got = old ? fread(old, 1, g_areatexgen.text_n + 1, f) : 0;
        int same = old && got == g_areatexgen.text_n && !memcmp(old, g_areatexgen.text, got);
        free(old);
        fclose(f);
        if (same) return;
    }
    f = fopen(path, "wb");
    if (!f || fwrite(g_areatexgen.text, 1, g_areatexgen.text_n, f) != g_areatexgen.text_n || fclose(f))
        fail("cannot write %s", path);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: areatexgen OUT.h\n");
        return 2;
    }
    build();
    write_header(argv[1]);
    return 0;
}
