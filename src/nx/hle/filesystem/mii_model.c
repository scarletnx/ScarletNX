/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* MiiModel generated in code (mii_model.h): every shape a parametric surface, every texture drawn from distance
   functions, each in the console's layout so that the SDK's resource loader and shaders take it. */
#include "nx/hle/filesystem/mii_model.h"
#include "nx/gpu/texture/blocklinear.h"
#include "common/utilities/endian.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PI_F 3.14159265f

/* ============================================================ the part list */

/* texture kinds, in the order a texture file's tables hold them, and their counts */
enum {
    T_HAT,
    T_EYE,
    T_EYEBROW,
    T_BEARD,
    T_WRINKLE,
    T_MAKEUP,
    T_GLASS,
    T_MOLE,
    T_MOUTH,
    T_MUSTACHE,
    T_NOSELINE,
    T_KINDS
};
static const u16 tex_counts[T_KINDS] = {132, 62, 24, 2, 12, 12, 20, 2, 37, 6, 18};
/* shape kinds, in a shape file's order, and their counts */
enum {
    S_BEARD,
    S_FACELINE,
    S_MASK,
    S_HAT_NORMAL,
    S_HAT_CAP,
    S_FOREHEAD_NORMAL,
    S_FOREHEAD_CAP,
    S_HAIR_NORMAL,
    S_HAIR_CAP,
    S_GLASS,
    S_NOSE,
    S_NOSELINE,
    S_KINDS
};
static const u16 shape_counts[S_KINDS] = {4, 12, 12, 132, 132, 132, 132, 132, 132, 1, 18, 18};

#define TEX_ENTRIES   327u
#define SHAPE_ENTRIES 857u
#define HAIR_STYLES   132u
#define FACELINES     12u
/* a texture file's tables: 0xc bytes of header, 11 largest sizes and 11 alignments, then a 0x1c-byte entry a part */
#define TEX_TABLE     0x64u
#define TEX_ENTRY     0x1cu
#define TEX_DATA      (TEX_TABLE + TEX_ENTRY * TEX_ENTRIES)
/* a shape file's: 12 largest sizes and alignments, 0x58-byte entries, then 72 bytes of hair transforms a hair style
   and 36 of faceline transforms a faceline */
#define SHAPE_TABLE   0x6cu
#define SHAPE_ENTRY   0x58u
#define HAIR_TF       (SHAPE_TABLE + SHAPE_ENTRY * SHAPE_ENTRIES)
#define FACE_TF       (HAIR_TF + 0x48u * HAIR_STYLES)
#define SHAPE_DATA    (FACE_TF + 0x24u * FACELINES)
/* a part's zlib parameters as the console's parts carry them (deflate, its default window), and the alignment the
   SDK gives a texture's decompressed image */
#define PART_ZLIB     0x806u
#define TEX_ALIGN     0x200u
#define SHAPE_ALIGN   4u

/* the hair styles that are a hat (with a hat shape and texture), and those of them that have a cap form */
static const u8 hat_styles[14] = {9, 26, 34, 46, 57, 61, 77, 78, 84, 97, 109, 114, 126, 130};
static const u8 hat_cap_styles[4] = {9, 84, 97, 130};
/* the hair styles with no forehead, in normal and in cap form */
static const u8 no_forehead[10] = {13, 33, 34, 37, 100, 107, 112, 115, 119, 120};
static const u8 no_forehead_cap[12] = {13, 33, 37, 79, 91, 100, 107, 112, 115, 119, 120, 123};
#define BALD_STYLE 30u /* the one hair style with no hair */

static int listed(const u8* list, u32 n, u32 i) {
    u32 k;
    for (k = 0; k < n; k++)
        if (list[k] == i) return 1;
    return 0;
}
static int tex_present(u32 kind, u32 i) {
    switch (kind) {
        case T_HAT: return listed(hat_styles, 14, i);
        case T_EYEBROW: return i != 23;
        case T_WRINKLE:
        case T_MAKEUP:
        case T_GLASS:
        case T_MUSTACHE: return i != 0; /* part 0 is none */
        case T_MOLE: return i == 1;
        default: return 1;
    }
}
static int shape_present(u32 kind, u32 i) {
    switch (kind) {
        case S_BEARD: return i != 0;
        case S_HAT_NORMAL: return listed(hat_styles, 14, i);
        case S_HAT_CAP: return listed(hat_cap_styles, 4, i);
        case S_FOREHEAD_NORMAL: return !listed(no_forehead, 10, i);
        case S_FOREHEAD_CAP: return !listed(no_forehead_cap, 12, i);
        case S_HAIR_NORMAL:
        case S_HAIR_CAP: return i != BALD_STYLE;
        case S_NOSE: return i != 0 && i != 2; /* the flat noses: their noseline draws them */
        default: return 1;
    }
}

/* texture formats as the SDK's resource enum names them: one-channel BC4, two-channel BC5, ASTC 4x4 */
enum { FMT_BC4 = 3, FMT_BC5 = 4, FMT_ASTC = 6 };
/* a texture part's geometry in a file of quality mid (0 Low, 1 Mid): every part a full mip chain, but glasses and
   noselines, which have one level */
typedef struct {
    u32 w, h, fmt, mips;
} TexGeom;
static TexGeom tex_geom(u32 kind, u32 i, u32 mid) {
    static const struct {
        u8 fmt;
        u16 lw, lh, mw, mh;
    } t[T_KINDS] = {
        {FMT_BC4, 8, 8, 8, 8},         {FMT_ASTC, 76, 64, 152, 128}, {FMT_BC4, 144, 128, 144, 128},
        {FMT_BC4, 256, 512, 256, 512}, {FMT_BC4, 96, 192, 256, 512}, {FMT_ASTC, 96, 192, 256, 512},
        {FMT_BC5, 128, 128, 256, 256}, {FMT_BC4, 32, 32, 128, 128},  {FMT_ASTC, 88, 64, 176, 128},
        {FMT_BC4, 64, 128, 128, 256},  {FMT_BC4, 96, 96, 128, 128},
    };
    TexGeom g;
    u32 big;
    g.fmt = t[kind].fmt;
    g.w = mid ? t[kind].mw : t[kind].lw;
    g.h = mid ? t[kind].mh : t[kind].lh;
    if (kind == T_HAT && i == 34) { /* the patterned hats */
        g.w = mid ? 128 : 64;
        g.h = mid ? 256 : 128;
    } else if (kind == T_HAT && i == 57) {
        g.w = 64;
        g.h = 16;
    }
    for (g.mips = 1, big = g.w > g.h ? g.w : g.h; big > 1; big >>= 1) g.mips++;
    if (kind == T_GLASS || kind == T_NOSELINE) g.mips = 1;
    return g;
}

/* ============================================================ variants: which drawing a part is */

/* the drawings of each texture kind: a part takes one of them (parts that share a drawing share its data) */
static const u8 tex_variants[T_KINDS] = {3, 12, 8, 2, 3, 3, 6, 1, 10, 5, 6};
static u32 tex_variant(u32 kind, u32 i) {
    switch (kind) {
        case T_HAT: return i == 34 ? 1 : i == 57 ? 2 : 0;
        case T_BEARD: return i;
        case T_MOLE: return 0;
        case T_WRINKLE:
        case T_MAKEUP:
        case T_GLASS:
        case T_MUSTACHE: return (i - 1) % tex_variants[kind];
        default: return i % tex_variants[kind];
    }
}
#define HAIR_VARIANTS 24u
#define NOSE_VARIANTS 6u
/* the hair styles with tails, the console's part list's: twin tails high on the sides, low behind the ears, curled,
   one ponytail on the side or at the back; each its own hair shape (variants past HAIR_VARIANTS), over the shell
   variant shell, with the ties its hat part is (for those of them the part list gives a hat) */
enum { TAIL_TWIN_HIGH = 1, TAIL_TWIN_LOW, TAIL_TWIN_CURL, TAIL_SIDE, TAIL_BACK };
#define TAIL_STYLES 10u
static const struct {
    u8 style, tail, shell;
} tail_styles[TAIL_STYLES] = {{9, TAIL_BACK, 3},       {19, TAIL_TWIN_LOW, 6},   {29, TAIL_TWIN_CURL, 6},
                              {78, TAIL_TWIN_HIGH, 9}, {84, TAIL_SIDE, 9},       {97, TAIL_BACK, 4},
                              {104, TAIL_TWIN_LOW, 7}, {112, TAIL_TWIN_CURL, 6}, {126, TAIL_TWIN_HIGH, 7},
                              {130, TAIL_BACK, 5}};
static int tail_of(u32 style) {
    u32 k;
    for (k = 0; k < TAIL_STYLES; k++)
        if (tail_styles[k].style == style) return (int)k;
    return -1;
}
static u32 hair_variant(u32 style) {
    int t = tail_of(style);
    return t >= 0 ? HAIR_VARIANTS + (u32)t : (style * 11u) % HAIR_VARIANTS;
}
/* the hat part of a hat style, by what it is: a cap (0, the two styles with a patterned texture), the ties of a tail
   style (1 + its tail_styles index), a headband (HAT_BAND) or a bow at the back of the head or a clip on its side */
#define HAT_BAND (1u + TAIL_STYLES)
#define HAT_BOW  (2u + TAIL_STYLES)
#define HAT_CLIP (3u + TAIL_STYLES)
static u32 hat_variant(u32 style) {
    int t = tail_of(style);
    if (t >= 0) return 1u + (u32)t;
    if (style == 46 || style == 114) return HAT_BAND;
    if (style == 26 || style == 109) return HAT_BOW;
    if (style == 61) return HAT_CLIP;
    return 0;
}
static u32 shape_variant(u32 kind, u32 i) {
    switch (kind) {
        case S_HAIR_NORMAL:
        case S_HAIR_CAP: return hair_variant(i);
        case S_HAT_NORMAL:
        case S_HAT_CAP: return hat_variant(i);
        case S_NOSE: return i % NOSE_VARIANTS;
        case S_FACELINE:
        case S_MASK:
        case S_BEARD: return i;
        default: return 0; /* one shape for every part of the kind */
    }
}

/* ============================================================ small math */

typedef struct {
    float x, y, z;
} Vec;
static Vec vec(float x, float y, float z) {
    Vec v;
    v.x = x;
    v.y = y;
    v.z = z;
    return v;
}
static Vec vsub(Vec a, Vec b) { return vec(a.x - b.x, a.y - b.y, a.z - b.z); }
static Vec vadd(Vec a, Vec b) { return vec(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vec vmul(Vec a, float s) { return vec(a.x * s, a.y * s, a.z * s); }
static float vdot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec vcross(Vec a, Vec b) { return vec(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float vlen(Vec a) { return sqrtf(vdot(a, a)); }
static Vec vnorm(Vec a) {
    float l = vlen(a);
    return l > 1e-12f ? vmul(a, 1.0f / l) : vec(0, 0, 1);
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float mixf(float a, float b, float t) { return a + (b - a) * t; }
static float smooth(float e0, float e1, float x) {
    float t = clampf((x - e0) / (e1 - e0), 0, 1);
    return t * t * (3 - 2 * t);
}

/* a float as a half, rounded to nearest (the parts' values are well inside the half's normal range) */
static u16 half_of(float f) {
    u32 b, sign, mant;
    i32 exp;
    memcpy(&b, &f, 4);
    sign = (b >> 16) & 0x8000u;
    exp = (i32)((b >> 23) & 0xff) - 127 + 15;
    mant = b & 0x7fffffu;
    if (exp <= 0) return (u16)sign; /* below the half's normal range: zero */
    if (exp >= 31) return (u16)(sign | 0x7c00u);
    mant += 0x1000u; /* round the 13 bits that go */
    if (mant & 0x800000u) {
        mant = 0;
        exp++;
        if (exp >= 31) return (u16)(sign | 0x7c00u);
    }
    return (u16)(sign | ((u32)exp << 10) | (mant >> 13));
}

/* ============================================================ the output and zlib's stored blocks */

/* where a file's bytes go (NULL: only measured) and the next free byte of its data */
typedef struct {
    u8* b;
    u32 at;
} Out;

static u32 adler32(const u8* p, u32 n) {
    u32 a = 1, s = 0, i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        s = (s + a) % 65521u;
    }
    return (s << 16) | a;
}
/* the bytes n raw bytes take as a zlib stream of stored blocks: its header, a 5-byte header a block of 65535 bytes
   at most, and the Adler-32 */
static u32 zstored_size(u32 n) { return 2u + 5u * ((n + 65534u) / 65535u) + n + 4u; }
/* where a part's raw bytes are made, n of them, so that zstored_wrap can move them into their stream in place */
static u8* zstored_raw(const Out* o, u32 n) { return o->b + o->at + 2u + 5u * ((n + 65534u) / 65535u); }
/* the n raw bytes at zstored_raw into a zlib stream at o->at: each block moved down behind its header, the first
   first, so none is overwritten before it moves */
static void zstored_wrap(Out* o, u32 n) {
    u32 blocks = (n + 65534u) / 65535u, k, adler;
    u8* raw = zstored_raw(o, n);
    u8* z = o->b + o->at;
    adler = adler32(raw, n);
    z[0] = 0x78; /* deflate, a 32 KiB window, no dictionary, and the check bits that make it a multiple of 31 */
    z[1] = 0x01;
    for (k = 0; k < blocks; k++) {
        u32 len = n - 65535u * k < 65535u ? n - 65535u * k : 65535u;
        u8* hdr = z + 2 + (5u + 65535u) * k;
        memmove(hdr + 5, raw + 65535u * k, len);
        hdr[0] = k + 1 == blocks; /* BFINAL on the last, BTYPE 0: stored */
        snx_st_le16(hdr + 1, (u16)len);
        snx_st_le16(hdr + 3, (u16)~len);
    }
    snx_st_be32(z + 2 + 5u * blocks + n, adler);
}

/* ============================================================ the head */

/* one faceline's head: its radii (the upper half's raised by ty, as the hair and the face's features are), its center
   and how much its jaw narrows toward the chin */
typedef struct {
    float rx, ry_up, ry_down, rz_front, rz_back, cy, cz, jaw, ty;
} Head;
static Head head_of(u32 f) {
    /* per faceline: the jaw's narrowing, the width, the chin's length and the upper half's rise */
    static const float t[FACELINES][4] = {
        {0.18f, 1.00f, 30.0f, 0.0f},  {0.12f, 1.00f, 30.0f, 0.0f}, {0.26f, 0.98f, 31.5f, 2.5f},
        {0.05f, 1.03f, 30.0f, 0.0f},  {0.22f, 1.00f, 30.5f, 0.0f}, {0.10f, 1.02f, 29.5f, 0.0f},
        {0.30f, 0.97f, 31.0f, 2.5f},  {0.16f, 0.99f, 31.0f, 2.5f}, {0.14f, 1.01f, 30.5f, 1.5f},
        {0.20f, 1.00f, 28.5f, -2.0f}, {0.08f, 1.04f, 30.5f, 1.5f}, {0.24f, 0.98f, 31.5f, 2.5f},
    };
    Head h;
    h.jaw = t[f][0];
    h.rx = 25.5f * t[f][1];
    h.ry_down = t[f][2];
    h.ty = t[f][3];
    h.ry_up = 34.0f + h.ty;
    h.rz_front = 23.0f;
    h.rz_back = 21.0f;
    h.cy = 30.0f;
    h.cz = 2.0f;
    return h;
}
/* the head's surface at azimuth th (0 the front, +z; positive toward +x) and elevation ph, its ears bumps at the
   sides; its horizontal sections fuller than an ellipsoid's, as a Mii's cheeks are */
static Vec head_at(const Head* h, float th, float ph) {
    float sp = sinf(ph), cp = powf(fabsf(cosf(ph)), 0.75f), st = sinf(th), ct = cosf(th);
    float down = sp < 0 ? -sp : 0;
    float ry = mixf(h->ry_down, h->ry_up, smooth(-0.25f, 0.25f, sp));
    float rz = mixf(h->rz_back, h->rz_front * (1.0f - 0.18f * down * down), smooth(-0.2f, 0.2f, ct));
    float side = fabsf(th) - PI_F * 0.5f, up = ph - 0.12f;
    float ear = 2.4f * expf(-(side * side) / (0.16f * 0.16f) - (up * up) / (0.15f * 0.15f));
    float x = h->rx * cp * st * (1.0f - h->jaw * down * down);
    return vec(x + (st < 0 ? -ear : ear), h->cy + ry * sp, h->cz + rz * cp * ct);
}
static Vec head_center(const Head* h) { return vec(0, h->cy, h->cz); }

/* ============================================================ shapes */

/* a shape: up to three grids (g) of ns by nt quads, each over a parametric surface (shape_at), one after the other
   in its vertices and triangles; its vertices' attributes */
enum { A_POS, A_NRM, A_UV, A_TAN, A_COL };
#define GRIDS_MAX 3u
typedef struct {
    u32 kind, variant, grids, g; /* g: the grid shape_at draws */
    u32 ns[GRIDS_MAX], nt[GRIDS_MAX];
    u32 attrs; /* the attributes present, a bit per A_ */
} Shape;

/* hair styles' looks, from their variant: length 0 to 2, the fringe 0 to 3, a large volume */
typedef struct {
    float thick, front, side, back, part, spikes, hang;
} Hair;
static Hair hair_of(u32 v) {
    if (v >= HAIR_VARIANTS) v = tail_styles[v - HAIR_VARIANTS].shell;
    {
        static const float fronts[4] = {0.66f, 0.54f, 0.38f, 0.58f};
        static const float sides[3] = {0.12f, -0.18f, -0.60f};
        static const float backs[3] = {-0.25f, -0.55f, -1.05f};
        Hair r;
        u32 len = v % 3, fringe = (v / 3) % 4, big = v / 12;
        r.thick = big ? 6.0f : 3.2f + 0.6f * (float)(v % 2);
        r.front = fronts[fringe];
        r.side = sides[len];
        r.back = backs[len];
        r.part = fringe == 3 ? 0.35f : 0.0f;
        r.spikes = (v % 7 == 5) ? 1.6f : 0.0f;
        r.hang = len == 2 ? 6.0f : 0.0f;
        return r;
    }
}
/* a tail of a tail style (k 0 or 1 of a pair): where it leaves the hair, the way out, how long it falls, its
   thickness at the root and the tip, and whether it curls */
typedef struct {
    Vec root, out;
    float fall, r0, r1, curl;
} Tail;
static Tail tail_of_style(u32 tail, u32 k) {
    float side = k ? -1.0f : 1.0f;
    Tail t;
    t.curl = 0;
    switch (tail) {
        case TAIL_TWIN_HIGH: /* long, from high on the sides */
            t.root = vec(side * 28.0f, 52.0f, -8.0f), t.out = vec(side * 9.0f, 2.0f, -4.0f);
            t.fall = 64.0f, t.r0 = 4.4f, t.r1 = 1.8f;
            break;
        case TAIL_TWIN_LOW:
        case TAIL_TWIN_CURL: /* from behind the ears, the curled ones in rings down their length */
            t.root = vec(side * 21.0f, 34.0f, -16.0f), t.out = vec(side * 5.0f, 0, -3.0f);
            t.fall = tail == TAIL_TWIN_CURL ? 40.0f : 34.0f, t.r0 = 3.8f, t.r1 = tail == TAIL_TWIN_CURL ? 2.4f : 1.4f;
            t.curl = tail == TAIL_TWIN_CURL ? 0.35f : 0;
            break;
        case TAIL_SIDE: /* one, high on one side */
            t.root = vec(25.0f, 54.0f, -12.0f), t.out = vec(8.0f, 3.0f, -4.0f);
            t.fall = 52.0f, t.r0 = 4.8f, t.r1 = 1.8f;
            break;
        default: /* TAIL_BACK: one, at the back of the head */
            t.root = vec(0, 46.0f, -28.0f), t.out = vec(0, 1.0f, -6.0f);
            t.fall = 34.0f, t.r0 = 4.2f, t.r1 = 1.5f;
            break;
    }
    return t;
}
static u32 tail_count(u32 tail) { return tail == TAIL_SIDE || tail == TAIL_BACK ? 1 : 2; }
/* the tail's middle at t (0 its root, 1 its tip): out, then falling, a little back */
static Vec tail_axis(const Tail* tl, float t) {
    float o = sinf(clampf(t * 2.5f, 0, 1) * PI_F * 0.5f);
    return vec(tl->root.x + tl->out.x * o, tl->root.y + tl->out.y * o - tl->fall * t * t * 0.4f - tl->fall * t * 0.6f,
               tl->root.z + tl->out.z * o - 3.0f * t);
}
/* a ring of radius r around the tail's axis at t, at angle a: s around it counterclockwise as the axis runs, so the
   grid faces out */
static Vec tail_ring(const Tail* tl, float t, float a, float r) {
    Vec c = tail_axis(tl, t), d = vsub(tail_axis(tl, t + 0.01f), tail_axis(tl, t - 0.01f)), n1, n2;
    d = vnorm(d);
    n1 = vcross(d, vec(0, 0, 1));
    if (vlen(n1) < 0.2f) n1 = vcross(d, vec(1, 0, 0));
    n1 = vnorm(n1);
    n2 = vcross(d, n1);
    return vadd(c, vadd(vmul(n1, r * cosf(a)), vmul(n2, r * sinf(a))));
}
/* the hair's lower edge's elevation at azimuth th: the fringe at the front, the sides at the ears, the back */
static float hair_edge(const Hair* r, float th) {
    float a = fabsf(th) / PI_F; /* 0 front, 0.5 side, 1 back */
    float e = a < 0.5f ? mixf(r->front, r->side, smooth(0.0f, 0.5f, a)) : mixf(r->side, r->back, smooth(0.5f, 1.0f, a));
    if (a < 0.35f) e -= r->part * (th > 0 ? 1.0f : -0.3f) * (1.0f - a / 0.35f); /* a side part: lower on one side */
    return e;
}
static const Head* base_head(Head* h) {
    *h = head_of(0);
    return h;
}

/* the cap line a cap form keeps its hair under */
#define CAP_EDGE 0.30f

/* a hat part that is no cap: the ties of a tail style (one ring a tail around its root), a headband over the crown
   from ear to ear, a bow at the back of the head, a clip on its side */
static Vec hat_part_at(const Shape* sh, float s, float t) {
    Head h;
    if (sh->variant <= TAIL_STYLES) { /* a tie: grid g around tail g's root */
        Tail tl = tail_of_style(tail_styles[sh->variant - 1].tail, sh->g);
        float a = s * 2 * PI_F, b = t * 2 * PI_F, big = tl.r0 + 0.5f;
        Vec c = tail_axis(&tl, 0.06f), ring = vsub(tail_ring(&tl, 0.06f, a, 1.0f), c);
        Vec d = vnorm(vsub(tail_axis(&tl, 0.07f), tail_axis(&tl, 0.05f)));
        return vadd(c, vadd(vmul(ring, big + 1.1f * cosf(b)), vmul(d, 1.1f * sinf(b))));
    }
    base_head(&h);
    if (sh->variant == HAT_BAND) {
        float u = (s - 0.5f) * 2, ph = (1.0f - fabsf(u)) * (PI_F * 0.5f - 0.1f) + 0.1f;
        Vec p = head_at(&h, u >= 0 ? PI_F * 0.5f : -PI_F * 0.5f, ph);
        p = vadd(head_center(&h), vmul(vsub(p, head_center(&h)), 1.0f + 6.0f / vlen(vsub(p, head_center(&h)))));
        return vadd(p, vec(0, 0, (0.5f - t) * 4.0f));
    }
    {
        Vec c = sh->variant == HAT_BOW ? vec(0, 63.0f, -17.0f) : vec(26.0f, 50.0f, 4.0f);
        float r = sh->variant == HAT_BOW ? 4.0f : 3.0f, th = s * 2 * PI_F, ph = (t - 0.5f) * PI_F;
        return vadd(c, vec(r * cosf(ph) * sinf(th), r * sinf(ph), r * cosf(ph) * cosf(th)));
    }
}
/* the shape's point at grid (s, t), both in [0, 1], of its grid g */
static Vec shape_at(const Shape* sh, float s, float t) {
    Head h;
    if ((sh->kind == S_HAIR_NORMAL || sh->kind == S_HAIR_CAP) && sh->g) { /* a tail */
        Tail tl = tail_of_style(tail_styles[sh->variant - HAIR_VARIANTS].tail, sh->g - 1);
        float r = mixf(tl.r0, tl.r1, t) * (1.0f + tl.curl * sinf(t * 10.0f * PI_F));
        return tail_ring(&tl, t, s * 2 * PI_F, r);
    }
    if ((sh->kind == S_HAT_NORMAL || sh->kind == S_HAT_CAP) && sh->variant) return hat_part_at(sh, s, t);
    switch (sh->kind) {
        case S_FACELINE: h = head_of(sh->variant); return head_at(&h, (s - 0.5f) * 2 * PI_F, (t - 0.5f) * PI_F);
        case S_MASK: { /* the face's front, a hair above it */
            float th = (s - 0.5f) * 3.5f, ph = mixf(-1.35f, 1.2f, t), e = 0.004f;
            Vec p, n;
            h = head_of(sh->variant);
            p = head_at(&h, th, ph);
            n = vnorm(vcross(vsub(head_at(&h, th + e, ph), head_at(&h, th - e, ph)),
                             vsub(head_at(&h, th, ph + e), head_at(&h, th, ph - e))));
            return vadd(p, vmul(n, 0.3f));
        }
        case S_FOREHEAD_NORMAL:
        case S_FOREHEAD_CAP: { /* the crown, inside the head's own: present for the SDK, hidden by the faceline */
            Vec p;
            base_head(&h);
            p = head_at(&h, (s - 0.5f) * 2 * PI_F, mixf(1.25f, PI_F * 0.5f, t));
            return vadd(head_center(&h), vmul(vsub(p, head_center(&h)), 0.97f));
        }
        case S_HAIR_NORMAL:
        case S_HAIR_CAP: {
            Hair r = hair_of(sh->variant);
            float th = (s - 0.5f) * 2 * PI_F, lo = hair_edge(&r, th), hi = PI_F * 0.5f, ph, thick;
            Vec p, c, d;
            if (sh->kind == S_HAIR_CAP) hi = lo < CAP_EDGE ? CAP_EDGE : lo; /* under a cap: only below its line */
            ph = mixf(lo, hi, t);
            base_head(&h);
            p = head_at(&h, th, ph);
            c = head_center(&h);
            thick = r.thick * (sh->kind == S_HAIR_CAP ? 0.6f : 1.0f);
            /* spikes around the top, gone toward the crown, where their ring is too small to fold */
            thick +=
                r.spikes * (0.5f + 0.5f * sinf(th * 7.0f)) * smooth(0.2f, 0.7f, ph) * (1.0f - smooth(1.05f, 1.45f, ph));
            d = vnorm(vsub(p, c));
            p = vadd(p, vmul(d, thick));
            if (ph < 0 && r.hang > 0) { /* long hair hangs straight below the head's widest line */
                float drop = -sinf(ph);
                p.y -= r.hang * drop * smooth(0.25f, 0.6f, fabsf(th) / PI_F);
                p = vadd(p, vmul(vnorm(vec(d.x, 0, d.z)), 2.0f * drop));
            }
            return p;
        }
        case S_HAT_NORMAL:
        case S_HAT_CAP: { /* a knit cap over the head, its rim at the brow */
            float th = (s - 0.5f) * 2 * PI_F, ph = mixf(0.42f, PI_F * 0.5f, t);
            Vec p, c;
            base_head(&h);
            p = head_at(&h, th, ph);
            c = head_center(&h);
            return vadd(c, vmul(vsub(p, c), 1.16f));
        }
        case S_BEARD: { /* a full beard over the jaw, under the mouth, up to the ears */
            float th = (s - 0.5f) * 4.0f, a = fabsf(th) / 2.0f, top = mixf(-0.62f, 0.05f, a * a);
            float ph = mixf(-PI_F * 0.5f, top, t);
            Vec p, c;
            base_head(&h);
            p = head_at(&h, th, ph);
            c = head_center(&h);
            return vadd(p, vmul(vnorm(vsub(p, c)), 1.0f + 0.9f * (float)sh->variant));
        }
        case S_NOSE: { /* half an ellipsoid, its tip forward, its back inside the face */
            static const float w[NOSE_VARIANTS] = {3.0f, 3.6f, 2.6f, 4.2f, 3.2f, 2.8f};
            static const float len[NOSE_VARIANTS] = {5.5f, 6.0f, 6.5f, 5.0f, 7.0f, 6.0f};
            static const float out[NOSE_VARIANTS] = {6.0f, 6.5f, 7.0f, 5.5f, 7.5f, 6.0f};
            u32 v = sh->variant;
            float th = (s - 0.5f) * PI_F, ph = (t - 0.5f) * PI_F, cp = cosf(ph), sp = sinf(ph);
            float tip = 1.0f + 0.25f * (1.0f - sp) * 0.5f; /* fuller below */
            return vec(w[v] * cp * sinf(th) * tip, 1.5f + len[v] * sp, -3.5f + out[v] * cp * cosf(th) * tip);
        }
        case S_NOSELINE: /* a quad under the nose */ return vec(mixf(-3.2f, 3.2f, s), mixf(-2.6f, 1.4f, t), 0);
        default: /* S_GLASS: a quad across both eyes, each half one lens of the texture */
            return vec(mixf(-30.0f, 30.0f, s), mixf(-15.0f, 15.0f, t), 0);
    }
}
/* the point the shape's normals point away from at (s, t): a tail's or a tie's middle there, a ball's center, else
   the head's or the part's own */
static Vec shape_center(const Shape* sh, float s, float t) {
    Head h;
    if ((sh->kind == S_HAIR_NORMAL || sh->kind == S_HAIR_CAP) && sh->g) {
        Tail tl = tail_of_style(tail_styles[sh->variant - HAIR_VARIANTS].tail, sh->g - 1);
        return tail_axis(&tl, t);
    }
    if ((sh->kind == S_HAT_NORMAL || sh->kind == S_HAT_CAP) && sh->variant && sh->variant != HAT_BAND) {
        if (sh->variant <= TAIL_STYLES) {
            Tail tl = tail_of_style(tail_styles[sh->variant - 1].tail, sh->g);
            Vec c = tail_axis(&tl, 0.06f);
            return vadd(c, vmul(vsub(tail_ring(&tl, 0.06f, s * 2 * PI_F, 1.0f), c), tl.r0 + 0.5f));
        }
        return sh->variant == HAT_BOW ? vec(0, 63.0f, -17.0f) : vec(26.0f, 50.0f, 4.0f);
    }
    switch (sh->kind) {
        case S_NOSE: return vec(0, 1.5f, -12.0f);
        case S_NOSELINE:
        case S_GLASS: return vec(0, 0, -100.0f);
        case S_FACELINE:
        case S_MASK: h = head_of(sh->variant); return head_center(&h);
        default: base_head(&h); return head_center(&h);
    }
}
static Vec shape_normal(const Shape* sh, float s, float t, Vec p) {
    const float e = 1.0f / 256;
    Vec ds = vsub(shape_at(sh, s + e, t), shape_at(sh, s - e, t));
    Vec dt = vsub(shape_at(sh, s, t + e), shape_at(sh, s, t - e));
    Vec n = vcross(ds, dt), out = vsub(p, shape_center(sh, s, t));
    if (vlen(n) < 1e-6f) return vnorm(out); /* a pole */
    n = vnorm(n);
    return vdot(n, out) < 0 ? vmul(n, -1) : n;
}
static void shape_uv(const Shape* sh, Vec p, float s, float t, float uv[2]) {
    Head h;
    switch (sh->kind) {
        case S_FACELINE: /* the faceline texture across the face, mirrored at its middle (u 1), its back at a corner */
            h = head_of(sh->variant);
            if (fabsf(s - 0.5f) > 0.30f) {
                uv[0] = uv[1] = 0;
                break;
            }
            uv[0] = clampf(1.0f + p.x / 35.6f, 0, 2);
            uv[1] = clampf((49.6f + h.ty - p.y) / 48.0f, 0, 1);
            break;
        case S_MASK: /* the mask texture the SDK composes, 100 units across, its top 80.35 above the chin's line */
            h = head_of(sh->variant);
            uv[0] = 0.5f + p.x / 100.0f;
            uv[1] = 0.8035f + h.ty / 100.0f - p.y / 100.0f;
            break;
        case S_HAT_NORMAL:
        case S_HAT_CAP:
            uv[0] = 2.0f * s;
            uv[1] = 1.0f - t;
            break;
        case S_NOSELINE: /* our noselines draw inside the middle of their texture */
            uv[0] = mixf(0.2f, 0.8f, s);
            uv[1] = mixf(0.75f, 0.25f, t);
            break;
        case S_GLASS: /* one lens a half, mirrored: u 0 to 2 */
            uv[0] = 2.0f * s;
            uv[1] = 1.0f - t;
            break;
        default: /* untextured parts take a corner */
            uv[0] = 0;
            uv[1] = 1;
            break;
    }
}
static Shape shape_of(u32 kind, u32 variant) {
    Shape sh = {0};
    u32 g, ns = 1, nt = 1, grids = 1;
    sh.kind = kind;
    sh.variant = variant;
    sh.attrs = 1u << A_POS | 1u << A_NRM | 1u << A_UV;
    switch (kind) {
        case S_FACELINE: ns = 32, nt = 18; break;
        case S_MASK: ns = 22, nt = 16; break;
        case S_HAIR_NORMAL:
        case S_HAIR_CAP:
            ns = 32, nt = 10;
            sh.attrs = 1u << A_POS | 1u << A_NRM | 1u << A_TAN | 1u << A_COL;
            if (variant >= HAIR_VARIANTS) grids = 1 + tail_count(tail_styles[variant - HAIR_VARIANTS].tail);
            break;
        case S_HAT_NORMAL:
        case S_HAT_CAP:
            if (!variant)
                ns = 24, nt = 6;
            else if (variant <= TAIL_STYLES)
                ns = 10, nt = 6, grids = tail_count(tail_styles[variant - 1].tail);
            else
                ns = variant == HAT_BAND ? 16 : 8, nt = variant == HAT_BAND ? 1 : 6;
            break;
        case S_BEARD: ns = 16, nt = 6; break;
        case S_NOSE: ns = 8, nt = 8; break;
        case S_FOREHEAD_NORMAL:
        case S_FOREHEAD_CAP: ns = 8, nt = 1; break;
        default: break; /* the quads */
    }
    sh.grids = grids;
    for (g = 0; g < grids; g++) {
        sh.ns[g] = g && (kind == S_HAIR_NORMAL || kind == S_HAIR_CAP) ? 10 : ns;
        sh.nt[g] = g && (kind == S_HAIR_NORMAL || kind == S_HAIR_CAP) ? 12 : nt;
    }
    return sh;
}

/* a shape's decompressed bytes: its attributes, a vertex each, then its 16-bit triangle list on 4 bytes */
static u32 shape_raw_size(const Shape* sh, u32 off[5], u32 size[5], u32* idx_off, u32* idx_size) {
    static const u8 bytes[5] = {8, 4, 4, 4, 4}; /* four halves, 10:10:10:2, two halves, four snorm8, four unorm8 */
    u32 nv = 0, quads = 0, at = 0, a, g;
    for (g = 0; g < sh->grids; g++) {
        nv += (sh->ns[g] + 1) * (sh->nt[g] + 1);
        quads += sh->ns[g] * sh->nt[g];
    }
    for (a = 0; a < 5; a++) {
        off[a] = size[a] = 0;
        if (!(sh->attrs & (1u << a))) continue;
        off[a] = at;
        size[a] = nv * bytes[a];
        at += size[a];
    }
    *idx_off = (at + 3) & ~3u;
    *idx_size = quads * 6 * 2;
    return *idx_off + *idx_size;
}
static u32 snorm10(float v) { return (u32)(i32)lroundf(clampf(v, -1, 1) * 511.0f) & 0x3ffu; }
static u8 snorm8(float v) { return (u8)(i8)lroundf(clampf(v, -1, 1) * 127.0f); }
/* the shape's bytes into raw, and its bounding box */
static void shape_write(const Shape* shape, u8* raw, float box[6]) {
    u32 off[5], size[5], idx_off, idx_size, i, j, n = 0, g, base, w;
    Shape grid = *shape;
    const Shape* sh = &grid;
    shape_raw_size(sh, off, size, &idx_off, &idx_size);
    box[0] = box[1] = box[2] = 1e30f;
    box[3] = box[4] = box[5] = -1e30f;
    for (grid.g = 0; grid.g < grid.grids; grid.g++)
        for (j = 0; j <= sh->nt[sh->g]; j++)
            for (i = 0; i <= sh->ns[sh->g]; i++, n++) {
                float s = (float)i / (float)sh->ns[sh->g], t = (float)j / (float)sh->nt[sh->g], uv[2];
                Vec p = shape_at(sh, s, t), nr = shape_normal(sh, s, t, p);
                u8* q = raw + off[A_POS] + 8 * n;
                snx_st_le16(q, half_of(p.x));
                snx_st_le16(q + 2, half_of(p.y));
                snx_st_le16(q + 4, half_of(p.z));
                snx_st_le16(q + 6, half_of(1.0f));
                snx_st_le32(raw + off[A_NRM] + 4 * n,
                            snorm10(nr.x) | snorm10(nr.y) << 10 | snorm10(nr.z) << 20 | 1u << 30);
                if (size[A_UV]) {
                    shape_uv(sh, p, s, t, uv);
                    snx_st_le16(raw + off[A_UV] + 4 * n, half_of(uv[0]));
                    snx_st_le16(raw + off[A_UV] + 4 * n + 2, half_of(uv[1]));
                }
                if (size[A_TAN]) { /* the hair's strands run down its surface, the direction its shading streaks */
                    Vec down = vsub(vec(0, -1, 0), vmul(nr, -nr.y)), tg;
                    if (vlen(down) < 0.05f) down = vsub(vec(0, 0, -1), vmul(nr, -nr.z)); /* at the crown */
                    tg = vnorm(down);
                    raw[off[A_TAN] + 4 * n] = snorm8(tg.x);
                    raw[off[A_TAN] + 4 * n + 1] = snorm8(tg.y);
                    raw[off[A_TAN] + 4 * n + 2] = snorm8(tg.z);
                    raw[off[A_TAN] + 4 * n + 3] = 127;
                }
                if (size[A_COL]) { /* the hair's shading parameters: none of its strands set apart */
                    raw[off[A_COL] + 4 * n + 1] = 255;
                    raw[off[A_COL] + 4 * n + 3] = 255;
                }
                if (p.x < box[0]) box[0] = p.x;
                if (p.y < box[1]) box[1] = p.y;
                if (p.z < box[2]) box[2] = p.z;
                if (p.x > box[3]) box[3] = p.x;
                if (p.y > box[4]) box[4] = p.y;
                if (p.z > box[5]) box[5] = p.z;
            }
    /* two triangles a quad: s runs to the right and t up as the surface faces out, so (a, b, c) and (a, c, d) are
       counterclockwise from the side its normals face, as the console's parts are; each grid's after the last's */
    for (g = 0, n = 0, base = 0; g < sh->grids; base += (sh->ns[g] + 1) * (sh->nt[g] + 1), g++)
        for (j = 0, w = sh->ns[g] + 1; j < sh->nt[g]; j++)
            for (i = 0; i < sh->ns[g]; i++) {
                u16 a = (u16)(base + j * w + i), b = (u16)(a + 1), c = (u16)(a + w + 1), d = (u16)(a + w);
                const u16 tri[6] = {a, b, c, a, c, d};
                u32 k;
                for (k = 0; k < 6; k++) snx_st_le16(raw + idx_off + 2 * n++, tri[k]);
            }
}

/* the hair transforms a style: where a game puts a hat's front, side and top on the hair, each a place and a
   rotation, from the hair's own surface and its normal there */
static void hair_transform(u32 style, float out[18]) {
    static const float at[3][2] = {{0.0f, 1.15f}, {-PI_F * 0.5f, 0.75f}, {PI_F, 1.0f}};
    Shape sh = shape_of(S_HAIR_NORMAL, hair_variant(style));
    Hair r = hair_of(sh.variant);
    u32 k;
    for (k = 0; k < 3; k++) {
        float th = at[k][0], ph = at[k][1], lo = hair_edge(&r, th);
        float t = (ph - lo) / (PI_F * 0.5f - lo), s = th / (2 * PI_F) + 0.5f;
        Vec p, n;
        if (s > 1) s -= 1;
        t = clampf(t, 0, 1);
        p = shape_at(&sh, s, t);
        n = shape_normal(&sh, s, t, p);
        out[6 * k] = k == 1 ? p.x : 0;
        out[6 * k + 1] = p.y;
        out[6 * k + 2] = p.z;
        out[6 * k + 3] = k == 1 ? 0 : atan2f(n.z, n.y);
        out[6 * k + 4] = 0;
        out[6 * k + 5] = k == 1 ? atan2f(-n.x, n.y) : 0;
    }
}
/* a faceline's transforms: the hair's place (raised as the head is), the nose's on the face's front, the beard's */
static void faceline_transform(u32 f, float out[9]) {
    Head h = head_of(f);
    float ny = 25.0f + 0.5f * h.ty, ph = asinf((ny - h.cy) / h.ry_down);
    Vec front = head_at(&h, 0, ph);
    memset(out, 0, 9 * sizeof *out);
    out[1] = h.ty;
    out[4] = ny;
    out[5] = front.z - 0.6f;
}

/* ============================================================ textures */

/* a distance's coverage of a texel aa wide: 1 inside, 0 outside */
static float cover(float d, float aa) { return clampf(0.5f - d / aa, 0, 1); }
static float sd_circle(float x, float y, float cx, float cy, float r) { return hypotf(x - cx, y - cy) - r; }
/* an ellipse, rotated by rot, as a distance near enough its edge */
static float sd_ellipse(float x, float y, float cx, float cy, float rx, float ry, float rot) {
    float c = cosf(rot), s = sinf(rot), dx = x - cx, dy = y - cy;
    float lx = dx * c + dy * s, ly = -dx * s + dy * c, k = hypotf(lx / rx, ly / ry);
    return (k - 1.0f) * (rx < ry ? rx : ry);
}
static float sd_capsule(float x, float y, float ax, float ay, float bx, float by, float r) {
    float px = x - ax, py = y - ay, dx = bx - ax, dy = by - ay;
    float h = clampf((px * dx + py * dy) / (dx * dx + dy * dy), 0, 1);
    return hypotf(px - dx * h, py - dy * h) - r;
}
/* a stroke r thick along a circle's arc from angle a0 to a1 (y down: pi/2 is the circle's bottom) */
static float sd_arc(float x, float y, float cx, float cy, float rad, float a0, float a1, float r) {
    float a = atan2f(y - cy, x - cx), mid = 0.5f * (a0 + a1), half = 0.5f * (a1 - a0), da = a - mid;
    while (da > PI_F) da -= 2 * PI_F;
    while (da < -PI_F) da += 2 * PI_F;
    if (fabsf(da) <= half) return fabsf(hypotf(x - cx, y - cy) - rad) - r;
    {
        float e = mid + (da > 0 ? half : -half);
        return hypotf(x - (cx + rad * cosf(e)), y - (cy + rad * sinf(e))) - r;
    }
}
static float sd_box(float x, float y, float cx, float cy, float hx, float hy, float round) {
    float dx = fabsf(x - cx) - hx + round, dy = fabsf(y - cy) - hy + round;
    float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0;
    return hypotf(ox, oy) + (dx > dy ? (dx < 0 ? dx : 0) : (dy < 0 ? dy : 0)) - round;
}
/* a texel's value from a hash of where it is: stubble */
static float speckle(float x, float y) {
    u32 h = (u32)(i32)(x * 977.0f) * 73856093u ^ (u32)(i32)(y * 977.0f) * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    return (float)((h >> 8) & 0xff) / 255.0f;
}

/* the eye: G its white, B its iris (the pupil in it left black), A all of it with its outline and lashes */
static void tex_eye(u32 v, float x, float y, float aa, float c[4]) {
    float cx = 0.73f, cy = 0.52f; /* the eye sits right of the middle, as the SDK places it */
    float rx = 0.31f + 0.04f * (float)(v % 3), ry = 0.18f + 0.03f * (float)((v / 3) % 3);
    float tilt = 0.08f * ((float)(v % 4) - 1.5f), ir = ry * (0.95f + 0.08f * (float)(v % 2));
    float d = sd_ellipse(x, y, cx, cy, rx, ry, tilt), lid = cover(d, aa);
    float iris = cover(sd_circle(x, y, cx + 0.02f, cy + 0.01f, ir), aa) * lid;
    float pupil = cover(sd_circle(x, y, cx + 0.02f, cy + 0.01f, ir * 0.42f), aa) * lid;
    /* the outline, its upper lid twice as thick as its lower */
    float all = cover(d - mixf(0.035f, 0.075f, smooth(cy, cy - 0.6f * ry, y)), aa);
    if (v % 2) { /* a lash at the outer corner */
        float ex = cx - rx * cosf(tilt), ey = cy - rx * sinf(tilt);
        all = fmaxf(all, cover(sd_capsule(x, y, ex + 0.03f, ey, ex - 0.09f, ey - 0.10f, 0.022f), aa));
    }
    c[0] = 0;
    c[1] = fmaxf(0, lid - iris);
    c[2] = fmaxf(0, iris - pupil);
    c[3] = all;
}
/* the mouth: R its lips and tongue, G its teeth, A all of it; what A has without R or G is its dark inside and
   outline. Ten mouths, most of them closed: smiles, lines, a frown, an open smile, a grin, an O */
static void tex_mouth(u32 v, float x, float y, float aa, float c[4]) {
    static const u8 kind[10] = {0, 3, 1, 0, 3, 4, 5, 2, 0, 1}; /* 5: the smile turned down */
    float cx = 0.6875f, cy = 0.5f, w = (v < 5 ? 0.30f : 0.22f) + 0.02f * (float)(v % 2);
    float all = 0, lips = 0, teeth = 0, d;
    switch (kind[v] % 5) {
        case 0: /* a closed smile, or a frown */
            if (kind[v] != 5)
                d = sd_arc(x, y, cx, cy - 0.30f, 0.36f, PI_F * 0.5f - w / 0.36f, PI_F * 0.5f + w / 0.36f, 0.028f);
            else
                d = sd_arc(x, y, cx, cy + 0.36f, 0.36f, -PI_F * 0.5f - w / 0.36f, -PI_F * 0.5f + w / 0.36f, 0.028f);
            all = cover(d, aa);
            break;
        case 1: /* an open smile: teeth above, the tongue below */
            d = fmaxf(sd_circle(x, y, cx, cy - 0.30f, w + 0.10f), cy - 0.03f - y);
            d = fmaxf(d, fabsf(x - cx) - w);
            all = cover(d - 0.03f, aa);
            teeth = cover(d, aa) * cover(y - (cy + 0.03f), aa);
            lips = cover(d, aa) * cover((cy + 0.09f) - y, aa);
            break;
        case 2: /* an O */
            d = sd_ellipse(x, y, cx, cy, w * 0.45f, 0.11f, 0);
            all = cover(d - 0.03f, aa);
            lips = cover(d, aa) * cover((cy + 0.03f) - y, aa);
            break;
        case 3: /* a flat line */ all = cover(sd_capsule(x, y, cx - w, cy, cx + w, cy, 0.025f), aa); break;
        default: /* a wide grin, all teeth */
            d = fmaxf(sd_ellipse(x, y, cx, cy + 0.02f, w * 1.1f, 0.13f, 0), cy - 0.06f - y);
            all = cover(d - 0.03f, aa);
            teeth = cover(d, aa) * cover(y - (cy + 0.09f), aa);
            lips = cover(d, aa) * cover((cy + 0.10f) - y, aa);
            break;
    }
    c[0] = lips;
    c[1] = teeth;
    c[2] = 0;
    c[3] = all;
}
/* one channel's value of a part drawn in white on black */
static float tex_value(u32 kind, u32 v, float x, float y, float aa, float ar) {
    switch (kind) {
        case T_HAT:
            if (v == 1) return 0.65f + 0.35f * (sinf(x * 60.0f) > 0); /* stripes */
            if (v == 2) return y > 0.25f && y < 0.75f ? 1.0f : 0.7f;  /* a band */
            return 1;
        case T_EYEBROW: { /* an arch, thicker at its inner end (the right) */
            float arch = 0.08f + 0.03f * (float)(v % 3), th = 0.09f + 0.02f * (float)((v / 3) % 3), d = 1e9f;
            float tilt = 0.04f * ((float)(v % 4) - 1.5f);
            u32 k;
            for (k = 0; k < 8; k++) {
                float t0 = (float)k / 8, t1 = (float)(k + 1) / 8;
                float x0 = mixf(0.20f, 1.02f, t0) * ar / 1.125f, x1 = mixf(0.20f, 1.02f, t1) * ar / 1.125f;
                float y0 = 0.42f - arch * sinf(PI_F * t0) + tilt * (t0 - 0.5f);
                float y1 = 0.42f - arch * sinf(PI_F * t1) + tilt * (t1 - 0.5f);
                float r = th * mixf(0.55f, 1.0f, t0);
                float dk = sd_capsule(x, y, x0, y0, x1, y1, r);
                if (dk < d) d = dk;
            }
            return cover(d, aa);
        }
        case T_BEARD: { /* the jaw of a half face, up the side to the ear, the mouth left open; the second a stubble.
                           u (x / ar) 0 is the face's side and 1 its middle; v (y) 0 the hairline and 1 the chin */
            float u = x / ar, d = fmaxf((0.46f + 0.20f * u) - y, 0.22f - u);
            float mouth = sd_ellipse(x, y, ar, 0.70f, 0.14f, 0.075f, 0);
            float full = cover(d, aa) * cover(-mouth, aa);
            return v ? full * (speckle(x, y) > 0.45f ? 0.55f : 0.25f) : full;
        }
        case T_WRINKLE: { /* lines at the eye's corner, beside the nose to the mouth, or across the forehead, in the
                         half face's u (0 its side, 1 its middle) and v (0 the hairline, 1 the chin) */
            float u = x / ar, d;
            if (v == 0)
                d = fminf(sd_capsule(u, y, 0.42f, 0.31f, 0.33f, 0.28f, 0.006f),
                          sd_capsule(u, y, 0.42f, 0.34f, 0.33f, 0.37f, 0.006f));
            else if (v == 1)
                d = sd_capsule(u, y, 0.80f, 0.54f, 0.72f, 0.72f, 0.006f);
            else
                d = fminf(sd_capsule(u, y, 0.55f, 0.10f, 1.02f, 0.09f, 0.005f),
                          sd_capsule(u, y, 0.60f, 0.15f, 1.02f, 0.14f, 0.005f));
            return 0.8f * cover(d, aa);
        }
        case T_MOLE: return cover(sd_circle(x, y, 0.5f, 0.5f, 0.36f), aa);
        case T_MUSTACHE: { /* half of it, the face's middle at the right edge */
            float d;
            if (v == 0)
                d = sd_ellipse(x, y, 0.36f, 0.52f, 0.14f, 0.045f, 0.15f);
            else if (v == 1)
                d = sd_arc(x, y, 0.5f, 0.40f, 0.16f, 0.6f, 2.2f, 0.025f);
            else if (v == 2)
                d = fminf(sd_ellipse(x, y, 0.38f, 0.52f, 0.12f, 0.04f, 0),
                          sd_arc(x, y, 0.22f, 0.47f, 0.05f, 0.0f, 3.4f, 0.016f));
            else if (v == 3)
                d = sd_capsule(x, y, 0.5f, 0.52f, 0.24f, 0.60f, 0.03f);
            else
                d = sd_ellipse(x, y, 0.37f, 0.53f, 0.16f, 0.07f, 0.1f);
            return cover(d, aa);
        }
        case T_NOSELINE: { /* drawn inside the middle of the image, where the noseline's quad maps */
            float d;
            if (v == 0)
                d = sd_arc(x, y, 0.5f, 0.45f, 0.10f, 0.4f, 2.7f, 0.022f);
            else if (v == 1)
                d = sd_arc(x, y, 0.5f, 0.40f, 0.17f, 0.6f, 2.5f, 0.022f);
            else if (v == 2)
                d = fminf(sd_arc(x, y, 0.40f, 0.52f, 0.06f, 1.2f, 3.6f, 0.02f),
                          sd_arc(x, y, 0.60f, 0.52f, 0.06f, -0.5f, 1.9f, 0.02f));
            else if (v == 3)
                d = fminf(sd_capsule(x, y, 0.56f, 0.30f, 0.60f, 0.58f, 0.02f),
                          sd_capsule(x, y, 0.60f, 0.58f, 0.44f, 0.60f, 0.02f));
            else if (v == 4)
                d = fminf(sd_circle(x, y, 0.42f, 0.56f, 0.03f), sd_circle(x, y, 0.58f, 0.56f, 0.03f));
            else
                d = fminf(sd_capsule(x, y, 0.38f, 0.48f, 0.5f, 0.6f, 0.02f),
                          sd_capsule(x, y, 0.5f, 0.6f, 0.62f, 0.48f, 0.02f));
            return cover(d, aa);
        }
        default: return 0;
    }
}
/* a texel of a part, its four channels in [0, 1]; x and y in the image's height, x up to its width over height */
static void tex_texel(u32 kind, u32 v, float x, float y, float aa, float ar, float c[4]) {
    c[0] = c[1] = c[2] = c[3] = 0;
    switch (kind) {
        case T_EYE: tex_eye(v, x, y, aa, c); break;
        case T_MOUTH: tex_mouth(v, x, y, aa, c); break;
        case T_MAKEUP: { /* blush on the cheek, shadow over the eye, or both, in their own colors */
            float blush = expf(-powf((x - 0.62f * ar) / 0.12f, 2) - powf((y - 0.58f) / 0.06f, 2));
            float shadow = expf(-powf((x - 0.55f * ar) / 0.10f, 2) - powf((y - 0.30f) / 0.035f, 2));
            if (v == 0) shadow = 0;
            if (v == 1) blush = 0;
            c[0] = blush > shadow ? 0.95f : 0.55f;
            c[1] = blush > shadow ? 0.45f : 0.35f;
            c[2] = blush > shadow ? 0.50f : 0.75f;
            c[3] = 0.6f * fmaxf(blush, shadow);
            break;
        }
        case T_GLASS: { /* R the frame's shade (1, or a dark lens), G its coverage: one lens and half the bridge */
            static const float hx[6] = {0.25f, 0.30f, 0.30f, 0.30f, 0.25f, 0.31f},
                               hy[6] = {0.25f, 0.20f, 0.20f, 0.18f, 0.25f, 0.21f};
            float cx = 0.59f, cy = 0.49f, th = v == 4 ? 0.05f : 0.028f, d;
            float bridge = sd_capsule(x, y, cx + hx[v] - 0.02f, cy - 0.06f, 1.02f, cy - 0.06f, 0.022f);
            float hinge = sd_capsule(x, y, cx - hx[v] + 0.02f, cy - 0.08f, cx - hx[v] - 0.06f, cy - 0.10f, 0.02f);
            if (v == 0 || v == 4)
                d = sd_circle(x, y, cx, cy, hx[v]);
            else if (v == 1)
                d = sd_ellipse(x, y, cx, cy, hx[v], hy[v], 0);
            else if (v == 3)
                d = sd_ellipse(x, y, cx, cy, hx[v], hy[v], -0.25f);
            else
                d = sd_box(x, y, cx, cy, hx[v], hy[v], 0.06f);
            c[1] = fmaxf(cover(fabsf(d) - th, aa), fmaxf(cover(bridge, aa), cover(hinge, aa)));
            c[0] = 1;
            if (v == 5 && d < 0) { /* sunglasses: a dark lens */
                c[0] = c[1] > 0.5f ? 1.0f : 0.15f;
                c[1] = fmaxf(c[1], 0.85f);
            }
            break;
        }
        default: c[0] = tex_value(kind, v, x, y, aa, ar); break;
    }
}

/* ---- block encoders: BC4 (one channel), BC5 (two), ASTC 4x4 (four) */

static u8 unorm8(float v) { return (u8)lroundf(clampf(v, 0, 1) * 255.0f); }
/* sixteen values as a BC4 block: its largest and smallest as the endpoints (8 levels between), each the nearest */
static void bc4_block(const u8 v[16], u8 out[8]) {
    u8 lo = 255, hi = 0, pal[8];
    u64 bits = 0;
    u32 i, k;
    for (i = 0; i < 16; i++) {
        if (v[i] < lo) lo = v[i];
        if (v[i] > hi) hi = v[i];
    }
    out[0] = hi;
    out[1] = lo;
    if (hi == lo) {
        memset(out + 2, 0, 6);
        return;
    }
    pal[0] = hi;
    pal[1] = lo;
    for (k = 2; k < 8; k++) pal[k] = (u8)(((8 - k) * hi + (k - 1) * lo + 3) / 7);
    for (i = 0; i < 16; i++) {
        u32 best = 0, bd = 999;
        for (k = 0; k < 8; k++) {
            u32 d = (u32)abs((int)v[i] - (int)pal[k]);
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        bits |= (u64)best << (3 * i);
    }
    for (i = 0; i < 6; i++) out[2 + i] = (u8)(bits >> (8 * i));
}
static void put_bits(u8* b, u32 at, u32 n, u32 v) {
    u32 i;
    for (i = 0; i < n; i++)
        if (v >> i & 1) b[(at + i) >> 3] |= (u8)(1u << ((at + i) & 7));
}
/* sixteen RGBA texels as an ASTC 4x4 block: one partition, LDR RGBA direct endpoints (8 bits each), a 4x4 grid of
   2-bit weights. The endpoints are the two texels farthest apart, each texel the nearest of the four colors between */
static void astc_block(const u8 t[16][4], u8 out[16]) {
    u32 i, j, a = 0, b = 0, best = 0, k;
    u8 w[16];
    int e0[4], e1[4];
    float dir[4], len2 = 0;
    memset(out, 0, 16);
    for (i = 0; i < 16; i++)
        for (j = i + 1; j < 16; j++) {
            u32 d = 0;
            for (k = 0; k < 4; k++) d += (u32)((t[i][k] - t[j][k]) * (t[i][k] - t[j][k]));
            if (d > best) {
                best = d;
                a = i;
                b = j;
            }
        }
    for (k = 0; k < 4; k++) {
        e0[k] = t[a][k];
        e1[k] = t[b][k];
        dir[k] = (float)(e1[k] - e0[k]);
        len2 += dir[k] * dir[k];
    }
    for (i = 0; i < 16; i++) {
        float p = 0;
        for (k = 0; k < 4; k++) p += (float)(t[i][k] - e0[k]) * dir[k];
        w[i] = len2 > 0 ? (u8)lroundf(clampf(p / len2, 0, 1) * 3.0f) : 0;
    }
    /* the decoder swaps endpoints whose second is darker (blue contraction): keep the second the brighter */
    if (e1[0] + e1[1] + e1[2] < e0[0] + e0[1] + e0[2]) {
        for (k = 0; k < 4; k++) {
            int s = e0[k];
            e0[k] = e1[k];
            e1[k] = s;
        }
        for (i = 0; i < 16; i++) w[i] = (u8)(3 - w[i]);
    }
    put_bits(out, 0, 11, 0x042); /* block mode: a 4x4 grid, weights 0 to 3, one plane */
    put_bits(out, 11, 2, 0);     /* one partition */
    put_bits(out, 13, 4, 12);    /* color endpoint mode 12: LDR RGBA direct */
    for (k = 0; k < 4; k++) {
        put_bits(out, 17 + 16 * k, 8, (u32)e0[k]);
        put_bits(out, 25 + 16 * k, 8, (u32)e1[k]);
    }
    for (i = 0; i < 16; i++) /* the weights from the block's top bit down */
        for (k = 0; k < 2; k++)
            if (w[i] >> k & 1) out[(127 - (2 * i + k)) >> 3] |= (u8)(1u << ((127 - (2 * i + k)) & 7));
}

/* ---- the block linear layout of a texture's levels */

/* a level's block height (log2 GOBs): level 0's the least that holds its rows, at most 16 GOBs; a smaller level's
   halves while half of it still holds the level's rows */
static u32 level_bh(u32 rows0, u32 rows) {
    u32 bh = 0;
    while (bh < 4 && (GOB_SIZE_Y << bh) < rows0) bh++;
    while (bh > 0 && rows <= (GOB_SIZE_Y << bh) / 2) bh--;
    return bh;
}
static u32 tex_bpb(u32 fmt) { return fmt == FMT_BC4 ? 8 : 16; }
static u32 level_size(const TexGeom* g, u32 l) {
    u32 w = g->w >> l ? g->w >> l : 1, h = g->h >> l ? g->h >> l : 1;
    u32 bx = (w + 3) / 4, by = (h + 3) / 4, rows0 = (g->h + 3) / 4, bh = level_bh(rows0, by);
    return (u32)(bl_gobs_x(bx * tex_bpb(g->fmt)) * GOB_SIZE_X * bl_block_rows(by, bh) * (GOB_SIZE_Y << bh));
}
static u32 tex_raw_size(const TexGeom* g) {
    u32 l, n = 0;
    for (l = 0; l < g->mips; l++) n += level_size(g, l);
    return n;
}
/* a texture's levels into raw, each drawn at its own size and laid out block linear */
static void tex_write(u32 kind, u32 v, const TexGeom* g, u8* raw) {
    float ar = (float)g->w / (float)g->h;
    u32 l, bpb = tex_bpb(g->fmt);
    for (l = 0; l < g->mips; l++) {
        u32 w = g->w >> l ? g->w >> l : 1, h = g->h >> l ? g->h >> l : 1;
        u32 bx = (w + 3) / 4, by = (h + 3) / 4, bh = level_bh((g->h + 3) / 4, by);
        u64 gobs_x = bl_gobs_x(bx * bpb);
        float aa = 1.4f / (float)h;
        u32 i, j;
        for (j = 0; j < by; j++)
            for (i = 0; i < bx; i++) {
                u8 t[16][4], block[16], ch[2][16];
                u32 p, xb = i * bpb, k;
                u64 at;
                for (p = 0; p < 16; p++) {
                    float c[4], x = ((float)(i * 4 + p % 4) + 0.5f) / (float)w * ar,
                                y = ((float)(j * 4 + p / 4) + 0.5f) / (float)h;
                    tex_texel(kind, v, x, y, aa, ar, c);
                    for (k = 0; k < 4; k++) t[p][k] = unorm8(c[k]);
                    ch[0][p] = t[p][0];
                    ch[1][p] = t[p][1];
                }
                if (g->fmt == FMT_ASTC)
                    astc_block((const u8(*)[4])t, block);
                else {
                    bc4_block(ch[0], block);
                    if (g->fmt == FMT_BC5) bc4_block(ch[1], block + 8);
                }
                /* the block's bytes: whole in one 16-byte run of a GOB row */
                at = (((u64)(j >> (3 + bh)) * gobs_x + (xb >> 6)) << (GOB_SIZE_SHIFT + bh)) +
                     ((u64)((j >> 3) & ((1u << bh) - 1)) << GOB_SIZE_SHIFT) + gob_x_offset(xb & 63) +
                     gob_y_offset(j & 7);
                memcpy(raw + at, block, bpb);
            }
        raw += level_size(g, l);
    }
}

/* ============================================================ the files */

/* the data a drawing shared by several parts already has in the file */
typedef struct {
    u32 off, raw, stored;
    float box[6]; /* a shape's bounding box */
} Placed;
#define VARIANTS_MAX (HAIR_VARIANTS + TAIL_STYLES)

static u32 texture_file(u32 mid, u8* b) {
    Out o;
    Placed placed[T_KINDS][VARIANTS_MAX];
    u32 largest[T_KINDS] = {0}, kind, i, e = 0;
    memset(placed, 0, sizeof placed);
    o.b = b;
    o.at = TEX_DATA;
    for (kind = 0; kind < T_KINDS; kind++)
        for (i = 0; i < tex_counts[kind]; i++, e++) {
            TexGeom g = tex_geom(kind, i, mid);
            u32 v = tex_variant(kind, i), raw = tex_raw_size(&g);
            Placed* p = &placed[kind][v];
            if (!tex_present(kind, i)) continue;
            if (!p->raw) {
                o.at = (o.at + 15) & ~15u;
                p->off = o.at;
                p->raw = raw;
                p->stored = zstored_size(raw);
                if (b) {
                    tex_write(kind, v, &g, zstored_raw(&o, raw));
                    zstored_wrap(&o, raw);
                }
                o.at += p->stored;
            }
            if (raw > largest[kind]) largest[kind] = raw;
            if (b) {
                u8* t = b + TEX_TABLE + TEX_ENTRY * e;
                snx_st_le32(t, p->off);
                snx_st_le32(t + 4, raw);
                snx_st_le32(t + 8, p->stored);
                snx_st_le32(t + 0xc, PART_ZLIB);
                snx_st_le32(t + 0x10, TEX_ALIGN);
                snx_st_le16(t + 0x14, (u16)g.w);
                snx_st_le16(t + 0x16, (u16)g.h);
                t[0x18] = (u8)g.fmt;
                t[0x19] = (u8)g.mips;
            }
        }
    if (b) {
        memcpy(b, "NFTR", 4);
        snx_st_le32(b + 4, 1);
        snx_st_le32(b + 8, o.at);
        for (kind = 0; kind < T_KINDS; kind++) {
            snx_st_le32(b + 0xc + 4 * kind, largest[kind]);
            snx_st_le32(b + 0xc + 4 * T_KINDS + 4 * kind, TEX_ALIGN);
        }
    }
    return o.at;
}

static u32 shape_file(u8* b) {
    Out o;
    Placed placed[S_KINDS][VARIANTS_MAX];
    u32 largest[S_KINDS] = {0}, kind, i, e = 0;
    memset(placed, 0, sizeof placed);
    o.b = b;
    o.at = SHAPE_DATA;
    for (kind = 0; kind < S_KINDS; kind++)
        for (i = 0; i < shape_counts[kind]; i++, e++) {
            Shape sh = shape_of(kind, shape_variant(kind, i));
            u32 off[5], size[5], idx_off, idx_size, raw = shape_raw_size(&sh, off, size, &idx_off, &idx_size), k;
            Placed* p = &placed[kind][sh.variant];
            if (!shape_present(kind, i)) continue;
            if (!p->raw) {
                o.at = (o.at + 15) & ~15u;
                p->off = o.at;
                p->raw = raw;
                p->stored = zstored_size(raw);
                if (b) {
                    shape_write(&sh, zstored_raw(&o, raw), p->box);
                    zstored_wrap(&o, raw);
                }
                o.at += p->stored;
            }
            if (raw > largest[kind]) largest[kind] = raw;
            if (b) {
                u8* t = b + SHAPE_TABLE + SHAPE_ENTRY * e;
                snx_st_le32(t, p->off);
                snx_st_le32(t + 4, raw);
                snx_st_le32(t + 8, p->stored);
                snx_st_le32(t + 0xc, PART_ZLIB);
                for (k = 0; k < 5; k++) {
                    snx_st_le32(t + 0x10 + 4 * k, off[k]);
                    snx_st_le32(t + 0x24 + 4 * k, size[k]);
                }
                snx_st_le32(t + 0x38, idx_off);
                snx_st_le32(t + 0x3c, idx_size);
                for (k = 0; k < 6; k++) {
                    u32 bits;
                    memcpy(&bits, &p->box[k], 4);
                    snx_st_le32(t + 0x40 + 4 * k, bits);
                }
            }
        }
    if (b) {
        float tf[18];
        memcpy(b, "NFSR", 4);
        snx_st_le32(b + 4, 1);
        snx_st_le32(b + 8, o.at);
        for (kind = 0; kind < S_KINDS; kind++) {
            snx_st_le32(b + 0xc + 4 * kind, largest[kind]);
            snx_st_le32(b + 0xc + 4 * S_KINDS + 4 * kind, SHAPE_ALIGN);
        }
        for (i = 0; i < HAIR_STYLES; i++) {
            u32 k;
            hair_transform(i, tf);
            for (k = 0; k < 18; k++) {
                u32 bits;
                memcpy(&bits, &tf[k], 4);
                snx_st_le32(b + HAIR_TF + 0x48 * i + 4 * k, bits);
            }
        }
        for (i = 0; i < FACELINES; i++) {
            u32 k;
            faceline_transform(i, tf);
            for (k = 0; k < 9; k++) {
                u32 bits;
                memcpy(&bits, &tf[k], 4);
                snx_st_le32(b + FACE_TF + 0x24 * i + 4 * k, bits);
            }
        }
    }
    return o.at;
}

void snx_mii_model_name(u32 i, char name[32]) {
    static const char* const names[SNX_MII_MODEL_FILES] = {"NXTextureLowLinear.dat", "NXTextureLowSRGB.dat",
                                                           "NXTextureMidLinear.dat", "NXTextureMidSRGB.dat",
                                                           "ShapeMid.dat",           "ShapeHigh.dat"};
    snprintf(name, 32, "%s", names[i]);
}
u32 snx_mii_model_same(u32 i) { return i == 1 || i == 3 || i == 5 ? i - 1 : i; }
u32 snx_mii_model_write(u32 i, u8* out) { return i < 4 ? texture_file(i >= 2, out) : shape_file(out); }
