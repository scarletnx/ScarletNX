/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* MiiModel generated in code (mii_model.h): every shape a parametric surface, every texture drawn from distance
   functions, each in the console's layout so that the SDK's resource loader and shaders take it. */
#include "common/utilities/mii_model.h"
#include "common/utilities/endian.h"
#include <libdeflate.h>
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
    u32 w, h, fmt, levels;
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
    for (g.levels = 1, big = g.w > g.h ? g.w : g.h; big > 1; big >>= 1) g.levels++;
    if (kind == T_GLASS || kind == T_NOSELINE) g.levels = 1;
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
/* the hair styles with tails, the console's part list's: twin tails high on the sides, low behind the ears, twin
   drills (the curled ones: short ringlets out from the sides), one ponytail on the side or at the back; each its own hair
   shape (variants past HAIR_VARIANTS), over the shell variant shell, with the ties its hat part is (for those of them
   the part list gives a hat), and one of the drill styles with an ahoge, a lock standing up from the crown
   (TAIL_AHOGE: drawn as a tail is, never a style's tail) */
enum { TAIL_TWIN_HIGH = 1, TAIL_TWIN_LOW, TAIL_TWIN_DRILL, TAIL_SIDE, TAIL_BACK, TAIL_AHOGE };
#define TAIL_STYLES 10u
static const struct {
    u8 style, tail, shell, ahoge;
} tail_styles[TAIL_STYLES] = {{9, TAIL_BACK, 3, 0},       {19, TAIL_TWIN_LOW, 6, 0},    {29, TAIL_TWIN_DRILL, 6, 0},
                              {78, TAIL_TWIN_HIGH, 9, 0}, {84, TAIL_SIDE, 9, 0},        {97, TAIL_BACK, 4, 0},
                              {104, TAIL_TWIN_LOW, 7, 0}, {112, TAIL_TWIN_DRILL, 6, 1}, {126, TAIL_TWIN_HIGH, 7, 0},
                              {130, TAIL_BACK, 5, 0}};
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

/* ============================================================ the output and its zlib streams */

/* where a file's bytes go (NULL: only measured) and the next free byte of its data; while it is written, the part
   being made (raw, cap bytes) and the compressor that deflates it */
typedef struct {
    u8* b;
    u32 at, cap;
    u8* raw;
    struct libdeflate_compressor* z;
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
/* the n bytes of o->raw into a zlib stream at o->at, deflated at zlib's default level as the console's parts are, or
   in stored blocks where that takes no more: its bytes, zstored_size(n) at most. The SDK reads a file whole into a
   heap the game sizes for the console's files */
static u32 zpart(Out* o, u32 n) {
    size_t z = libdeflate_zlib_compress(o->z, o->raw, n, o->b + o->at, zstored_size(n));
    if (z) return (u32)z;
    memcpy(zstored_raw(o, n), o->raw, n);
    zstored_wrap(o, n);
    return zstored_size(n);
}
/* a file's compressor, for o->b (its part buffer grows as parts need it): 0, or -1 when there is no memory for it */
static int out_open(Out* o) {
    o->raw = NULL;
    o->cap = 0;
    o->z = o->b ? libdeflate_alloc_compressor(6) : NULL;
    return o->b && !o->z ? -1 : 0;
}
static void out_close(Out* o) {
    free(o->raw);
    libdeflate_free_compressor(o->z);
}
/* o->raw zeroed for a part of n bytes, as a drawing writes only what it covers: NULL when there is no memory for it */
static u8* part_buffer(Out* o, u32 n) {
    if (n > o->cap) {
        u8* r = (u8*)realloc(o->raw, n);
        if (!r) return NULL;
        o->raw = r;
        o->cap = n;
    }
    memset(o->raw, 0, n);
    return o->raw;
}

/* ============================================================ the head */

/* one faceline's head: its radii (the upper half's raised by ty, as the hair and the face's features are), its center
   and how much its jaw narrows toward the chin. Every faceline is a plush's: wide and round, a short chin under a low
   face, the crown not much higher than the head is wide */
typedef struct {
    float rx, ry_up, ry_down, rz_front, rz_back, cy, cz, jaw, ty;
} Head;
#define HEAD_RX 32.0f
#define HEAD_UP 28.0f
#define HEAD_CY 18.0f
static Head head_of(u32 f) {
    /* per faceline: the jaw's narrowing, the width, the chin's length and the upper half's rise */
    static const float t[FACELINES][4] = {
        {0.135f, 1.00f, 18.0f, 0.0f},  {0.09f, 1.00f, 18.0f, 0.0f},  {0.195f, 0.98f, 18.9f, 2.5f},
        {0.0375f, 1.03f, 18.0f, 0.0f}, {0.165f, 1.00f, 18.3f, 0.0f}, {0.075f, 1.02f, 17.7f, 0.0f},
        {0.225f, 0.97f, 18.6f, 2.5f},  {0.12f, 0.99f, 18.6f, 2.5f},  {0.105f, 1.01f, 18.3f, 1.5f},
        {0.15f, 1.00f, 17.1f, -2.0f},  {0.06f, 1.04f, 18.3f, 1.5f},  {0.18f, 0.98f, 18.9f, 2.5f},
    };
    Head h;
    h.jaw = t[f][0];
    h.rx = HEAD_RX * t[f][1];
    h.ry_down = t[f][2];
    h.ty = t[f][3];
    h.ry_up = HEAD_UP + h.ty;
    h.rz_front = 21.0f;
    h.rz_back = 24.0f;
    h.cy = HEAD_CY;
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
/* The plush face: the features a Mii's mask and faceline textures carry (eyes, brows, mouth, wrinkles, makeup) where
   the SDK puts them, on a face of a plush's: the eyes big and low, the mouth small just under them. face_sdk takes a
   point (x, y) of the face, the faceline risen by ty, to where the SDK's frame has it (xs, ys): above FACE_BREAK under
   the eyes' line both ways FACE_KE times smaller (the eyes and brows that much bigger), the eyes' line (34 above the
   chin in the SDK's frame) at FACE_EYE_Y; under it ys gains FACE_KM times faster (the mouth that much shorter, close
   under the eyes) and xs FACE_KXM times */
#define FACE_EYE_Y 18.0f
#define FACE_KE    1.3f
#define FACE_KX    1.6f
#define FACE_KM    0.45f
#define FACE_KXM   1.0f
#define FACE_BREAK 5.0f
static void face_sdk(float x, float y, float ty, float* xs, float* ys) {
    float yb = FACE_EYE_Y + ty - FACE_BREAK, sb = 34.0f + ty - FACE_BREAK / FACE_KE;
    float k = mixf(FACE_KXM, FACE_KX, smooth(yb - 3.0f, yb + 3.0f, y));
    *xs = x / k;
    *ys = y >= yb ? sb + (y - yb) / FACE_KE : sb - (yb - y) / FACE_KM;
}
/* the face's height whose point the SDK's frame has at ys: face_sdk's inverse */
static float face_y(float ys, float ty) {
    float yb = FACE_EYE_Y + ty - FACE_BREAK, sb = 34.0f + ty - FACE_BREAK / FACE_KE;
    return ys >= sb ? yb + (ys - sb) * FACE_KE : yb - (sb - ys) * FACE_KM;
}

/* ============================================================ shapes */

/* a shape: up to four grids (g) of ns by nt quads, each over a parametric surface (shape_at), one after the other
   in its vertices and triangles; its vertices' attributes */
enum { A_POS, A_NRM, A_UV, A_TAN, A_COL };
#define GRIDS_MAX 4u
typedef struct {
    u32 kind, variant, grids, g; /* g: the grid shape_at draws */
    u32 ns[GRIDS_MAX], nt[GRIDS_MAX];
    u32 attrs; /* the attributes present, a bit per A_ */
} Shape;

/* hair styles' looks, from their variant: length 0 to 2, the fringe 0 to 3, a large volume. Every one a plush's: thick,
   the fringe down to the eyes' top, the sides and the back down to the jaw or lower */
typedef struct {
    float thick, front, side, back, part, spikes, hang;
} Hair;
static Hair hair_of(u32 v) {
    if (v >= HAIR_VARIANTS) v = tail_styles[v - HAIR_VARIANTS].shell;
    {
        static const float fronts[4] = {0.46f, 0.34f, 0.18f, 0.38f};
        static const float sides[3] = {-0.98f, -1.28f, -1.45f};
        static const float backs[3] = {-0.9f, -1.2f, -1.45f};
        Hair r;
        u32 len = v % 3, fringe = (v / 3) % 4, big = v / 12;
        r.thick = big ? 10.5f : 6.5f + 0.8f * (float)(v % 2);
        r.front = fronts[fringe];
        r.side = sides[len];
        r.back = backs[len];
        r.part = fringe == 3 ? 0.35f : 0.0f;
        r.spikes = (v % 7 == 5) ? 1.6f : 0.0f;
        r.hang = len == 2 ? 6.0f : 0.0f;
        return r;
    }
}
/* a tail of a tail style (k 0 or 1 of a pair): where it leaves the hair, the way out (taken over its first part, or
   along all of it as spread goes to 1), how long it falls, its thickness at the root and the tip, for a drill the
   turns its ringlet winds down its length (0 for a plain tail), how much of its width its thickness front to back is
   (flat, 1 round), and whether it is a lock (leaf): narrow at its tie, widest a third of the way, then to a point */
typedef struct {
    Vec root, out;
    float fall, r0, r1, turns, spread, flat;
    int leaf;
} Tail;
static Tail tail_of_style(u32 tail, u32 k) {
    float side = k ? -1.0f : 1.0f;
    Tail t;
    t.turns = 0;
    t.spread = 0;
    t.flat = 1.0f;
    t.leaf = 0;
    switch (tail) {
        case TAIL_TWIN_HIGH: /* wide flat locks from high on the sides, far out and down past the chin */
            t.root = vec(side * 0.86f * HEAD_RX, HEAD_CY + 0.70f * HEAD_UP, -8.0f),
            t.out = vec(side * 28.0f, -2.0f, -4.0f);
            t.fall = 48.0f, t.r0 = 7.5f, t.r1 = 0.6f, t.spread = 1.0f, t.flat = 0.45f, t.leaf = 1;
            break;
        case TAIL_TWIN_LOW: /* wide flat locks from behind the ears, out a little and down past the chin */
            t.root = vec(side * 0.82f * HEAD_RX, HEAD_CY + 0.12f * HEAD_UP, -16.0f),
            t.out = vec(side * 10.0f, -2.0f, -3.0f);
            t.fall = 32.0f, t.r0 = 8.0f, t.r1 = 0.6f, t.spread = 0.6f, t.flat = 0.5f, t.leaf = 1;
            break;
        case TAIL_TWIN_DRILL: /* out from the sides at the eyes' height: short and chunky, nearly as thick at the end
                                 as at the root, three rings */
            t.root = vec(side * 0.70f * HEAD_RX, HEAD_CY + 0.4f * HEAD_UP, -6.0f),
            t.out = vec(side * 25.0f, -3.0f, 1.0f);
            t.fall = 0.0f, t.r0 = 18.0f, t.r1 = 12.0f, t.turns = 3.0f, t.spread = 1.0f;
            break;
        case TAIL_SIDE: /* one wide flat lock, from high on one side, close to the hair, down past the jaw */
            t.root = vec(0.98f * HEAD_RX, HEAD_CY + 0.76f * HEAD_UP, -10.0f), t.out = vec(8.0f, 2.0f, -4.0f);
            t.fall = 42.0f, t.r0 = 10.0f, t.r1 = 0.6f, t.spread = 0.6f, t.flat = 0.5f, t.leaf = 1;
            break;
        case TAIL_AHOGE: /* thin, up from the crown, leaning forward and to one side */
            t.root = vec(0, HEAD_CY + HEAD_UP + 3.5f, 1.0f), t.out = vec(7.0f, 0, 5.0f); /* inside the crown's hair */
            t.fall = -12.0f, t.r0 = 2.8f, t.r1 = 0.3f, t.spread = 1.0f;
            break;
        default: /* TAIL_BACK: one wide flat lock down the back of the head */
            t.root = vec(0, HEAD_CY + 0.47f * HEAD_UP, -26.0f), t.out = vec(0, 1.0f, -5.0f); /* out of the hair */
            t.fall = 34.0f, t.r0 = 9.0f, t.r1 = 0.6f, t.spread = 0.6f, t.flat = 0.5f, t.leaf = 1;
            break;
    }
    return t;
}
static u32 tail_count(u32 tail) { return tail == TAIL_SIDE || tail == TAIL_BACK ? 1 : 2; }
/* the grids past the head's of a hair variant with tails: its tails, then its ahoge */
static u32 hair_grids(u32 variant) {
    return variant < HAIR_VARIANTS
               ? 1
               : 1 + tail_count(tail_styles[variant - HAIR_VARIANTS].tail) + tail_styles[variant - HAIR_VARIANTS].ahoge;
}
/* a tail's radius at t along it (0 its root, 1 its tip) and angle a around it. A drill swells from a narrow root
   inside the hair to its thickest a sixth of the way out, then narrows ring by ring to a point; each turn of its
   ringlet is a cone, narrow where it starts and wide where it ends, the next one starting inside it, and the turn a
   point is on moves on as a goes around, so the edges between them wind along the tail. A grid with fewer than eight
   rows a turn draws a drill without its rings (rings 0): sampled that sparsely they fold its surface */
static float tail_radius(const Tail* tl, float t, float a, float rings) {
    float env, f;
    if (tl->leaf) return mixf(tl->r0 * mixf(0.5f, 1.0f, smooth(0.0f, 0.3f, t)), tl->r1, smooth(0.4f, 1.0f, t));
    if (tl->turns == 0) return mixf(tl->r0, tl->r1, t);
    env = mixf(tl->r0, tl->r1, t) * mixf(0.3f, 1.0f, smooth(0.0f, 0.15f, t)) * (1.0f - 0.8f * smooth(0.85f, 1.0f, t));
    f = tl->turns * t + a / (2 * PI_F);
    f -= floorf(f);
    /* the rings start past the root, which only swells, or a ring's edge would fold the surface there; each ring
       narrows to six tenths, deeper folds a drill this thick */
    return env * mixf(1.0f, 0.6f + 0.4f * sqrtf(smooth(0.0f, 0.82f, f)) * (1.0f - smooth(0.82f, 1.0f, f)),
                      rings * smooth(0.1f, 0.2f, t));
}
/* the tail's middle at t (0 its root, 1 its tip): out, then falling, a little back */
static Vec tail_axis(const Tail* tl, float t) {
    float o = mixf(sinf(clampf(t * 2.5f, 0, 1) * PI_F * 0.5f), t, tl->spread);
    return vec(tl->root.x + tl->out.x * o, tl->root.y + tl->out.y * o - tl->fall * t * t * 0.4f - tl->fall * t * 0.6f,
               tl->root.z + tl->out.z * o - 3.0f * t);
}
/* a ring of radius r around the tail's axis at t, at angle a, flattened front to back by its flat: s around it
   counterclockwise as the axis runs, so the grid faces out */
static Vec tail_ring(const Tail* tl, float t, float a, float r) {
    Vec c = tail_axis(tl, t), d = vsub(tail_axis(tl, t + 0.01f), tail_axis(tl, t - 0.01f)), n1, n2;
    d = vnorm(d);
    n1 = vcross(d, vec(0, 0, 1));
    if (vlen(n1) < 0.2f) n1 = vcross(d, vec(1, 0, 0));
    n1 = vnorm(n1);
    n2 = vcross(d, n1);
    return vadd(c, vadd(vmul(n1, r * cosf(a)), vmul(n2, r * tl->flat * sinf(a))));
}
/* what grid g (past the head's) of a hair variant with tails draws: tail g - 1, or past its tails its ahoge */
static Tail hair_grid_tail(u32 variant, u32 g) {
    u32 tail = tail_styles[variant - HAIR_VARIANTS].tail;
    return g > tail_count(tail) ? tail_of_style(TAIL_AHOGE, 0) : tail_of_style(tail, g - 1);
}
/* the locks the hair's edge ends in, around the head, each a point */
#define HAIR_LOCKS 16
/* the hair's lower edge's elevation at azimuth th: the fringe at the front, the sides at the ears, the back. Across the
   fringe and the sides it ends in locks, pointed at their tips and rounded between them, one at the front's middle */
static float hair_edge(const Hair* r, float th) {
    float a = fabsf(th) / PI_F; /* 0 front, 0.5 side, 1 back */
    float e =
        a < 0.5f ? mixf(r->front, r->side, smooth(0.16f, 0.3f, a)) : mixf(r->side, r->back, smooth(0.5f, 1.0f, a));
    float u = th * (float)HAIR_LOCKS / (2 * PI_F), lock = 1.0f - 2.0f * fabsf(u - floorf(u + 0.5f));
    if (a < 0.35f) e -= r->part * (th > 0 ? 1.0f : -0.3f) * (1.0f - a / 0.35f); /* a side part: lower on one side */
    return e - 0.10f * lock * lock * (1.0f - smooth(0.4f, 0.7f, a));
}
static const Head* base_head(Head* h) {
    *h = head_of(0);
    return h;
}

/* the cap line a cap form keeps its hair under */
#define CAP_EDGE 0.30f

/* the middle of a bow (at the back of the head, high) or a clip (on its side), over the hair */
static Vec hat_ball(u32 variant) {
    return variant == HAT_BOW ? vec(0, HEAD_CY + 0.97f * HEAD_UP, -23.5f)
                              : vec(HEAD_RX + 6.5f, HEAD_CY + 0.59f * HEAD_UP, 4.0f);
}
/* a hat part that is no cap: the ties of a tail style (one ring a tail around its root), a headband over the crown
   from ear to ear, a bow at the back of the head, a clip on its side */
static Vec hat_part_at(const Shape* sh, float s, float t) {
    Head h;
    if (sh->variant <= TAIL_STYLES) { /* a tie: grid g around tail g's root */
        Tail tl = tail_of_style(tail_styles[sh->variant - 1].tail, sh->g);
        float a = s * 2 * PI_F, b = t * 2 * PI_F, big = tail_radius(&tl, 0.06f, a, 1.0f) + 0.5f;
        Vec c = tail_axis(&tl, 0.06f), ring = vsub(tail_ring(&tl, 0.06f, a, 1.0f), c);
        Vec d = vnorm(vsub(tail_axis(&tl, 0.07f), tail_axis(&tl, 0.05f)));
        return vadd(c, vadd(vmul(ring, big + 1.1f * cosf(b)), vmul(d, 1.1f * sinf(b))));
    }
    base_head(&h);
    if (sh->variant == HAT_BAND) {
        float u = (s - 0.5f) * 2, ph = (1.0f - fabsf(u)) * (PI_F * 0.5f - 0.1f) + 0.1f;
        Vec p = head_at(&h, u >= 0 ? PI_F * 0.5f : -PI_F * 0.5f, ph);
        p = vadd(head_center(&h), vmul(vsub(p, head_center(&h)), 1.0f + 8.0f / vlen(vsub(p, head_center(&h)))));
        return vadd(p, vec(0, 0, (0.5f - t) * 4.0f));
    }
    {
        Vec c = hat_ball(sh->variant);
        float r = sh->variant == HAT_BOW ? 6.0f : 4.5f, th = s * 2 * PI_F, ph = (t - 0.5f) * PI_F;
        return vadd(c, vec(r * cosf(ph) * sinf(th), r * sinf(ph), r * cosf(ph) * cosf(th)));
    }
}
/* the shape's point at grid (s, t), both in [0, 1], of its grid g */
static Vec shape_at(const Shape* sh, float s, float t) {
    Head h;
    if ((sh->kind == S_HAIR_NORMAL || sh->kind == S_HAIR_CAP) && sh->g) { /* a tail or the ahoge */
        Tail tl = hair_grid_tail(sh->variant, sh->g);
        float rings = tl.turns > 0 ? smooth(6.0f, 8.0f, (float)sh->nt[sh->g] / tl.turns) : 0;
        return tail_ring(&tl, t, s * 2 * PI_F, tail_radius(&tl, t, s * 2 * PI_F, rings));
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
            if (sh->kind ==
                S_HAIR_CAP) /* under a cap: only below its line, a strip at least where the edge reaches it */
                hi = fmaxf(lo + 0.05f, CAP_EDGE);
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
            if (ph < 0) { /* under the head's widest line the hair falls straight, a bell as wide as that line */
                Vec q = head_at(&h, th, 0), b;
                float k = 0.4f * smooth(0.0f, 0.5f, -ph);
                q = vadd(q, vmul(vnorm(vsub(q, c)), thick));
                b = vec(mixf(p.x, q.x, k), p.y, mixf(p.z, q.z, k));
                p = b;
            }
            if (ph < 0 && r.hang > 0) { /* long hair hangs straight below the head's widest line */
                float drop = -sinf(ph);
                p.y -= r.hang * drop * smooth(0.25f, 0.6f, fabsf(th) / PI_F);
                p = vadd(p, vmul(vnorm(vec(d.x, 0, d.z)), 2.0f * drop));
            }
            return p;
        }
        case S_HAT_NORMAL:
        case S_HAT_CAP: { /* a knit cap over the head and the thickest hair, its rim at the brow */
            float th = (s - 0.5f) * 2 * PI_F, ph = mixf(0.42f, PI_F * 0.5f, t);
            Vec p, c;
            base_head(&h);
            p = head_at(&h, th, ph);
            c = head_center(&h);
            return vadd(p, vmul(vnorm(vsub(p, c)), 12.0f));
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
        Tail tl = hair_grid_tail(sh->variant, sh->g);
        return tail_axis(&tl, t);
    }
    if ((sh->kind == S_HAT_NORMAL || sh->kind == S_HAT_CAP) && sh->variant && sh->variant != HAT_BAND) {
        if (sh->variant <= TAIL_STYLES) {
            Tail tl = tail_of_style(tail_styles[sh->variant - 1].tail, sh->g);
            Vec c = tail_axis(&tl, 0.06f);
            return vadd(c, vmul(vsub(tail_ring(&tl, 0.06f, s * 2 * PI_F, 1.0f), c),
                                tail_radius(&tl, 0.06f, s * 2 * PI_F, 1.0f) + 0.5f));
        }
        return hat_ball(sh->variant);
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
    float xs, ys;
    switch (sh->kind) {
        case S_FACELINE: /* the faceline texture across the face, mirrored at its middle (u 1), its back at a corner */
            h = head_of(sh->variant);
            if (fabsf(s - 0.5f) > 0.30f) {
                uv[0] = uv[1] = 0;
                break;
            }
            face_sdk(p.x, p.y, h.ty, &xs, &ys);
            uv[0] = clampf(1.0f + xs / 35.6f, 0, 2);
            uv[1] = clampf((49.6f + h.ty - ys) / 48.0f, 0, 1);
            break;
        case S_MASK: /* the mask texture the SDK composes, 100 units across, its top 80.35 above the chin's line */
            h = head_of(sh->variant);
            face_sdk(p.x, p.y, h.ty, &xs, &ys);
            uv[0] = 0.5f + xs / 100.0f;
            uv[1] = 0.8035f + h.ty / 100.0f - ys / 100.0f;
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
        case S_HAIR_CAP: /* four quads a lock around */
            ns = 4 * HAIR_LOCKS, nt = 10;
            sh.attrs = 1u << A_POS | 1u << A_NRM | 1u << A_TAN | 1u << A_COL;
            grids = hair_grids(variant);
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
        sh.ns[g] = ns;
        sh.nt[g] = nt;
        if (g && (kind == S_HAIR_NORMAL || kind == S_HAIR_CAP)) {
            /* a drill's turns take a dozen quads around it and a dozen along each; the thin ahoge few */
            int drill = hair_grid_tail(variant, g).turns > 0;
            int ahoge = g > tail_count(tail_styles[variant - HAIR_VARIANTS].tail);
            sh.ns[g] = drill ? 12 : ahoge ? 6 : 10;
            sh.nt[g] = drill ? 36 : ahoge ? 8 : 12;
        }
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
/* a shape file's quality, in the archive's order; Q_CTR a 3DS part, whose vertices a byte indexes; Q_FULL every
   quad shape_of draws (what a 3D program refines the parts from) */
enum { Q_MID, Q_HIGH, Q_SHAPES, Q_CTR = Q_SHAPES, Q_FULL };
/* the console's largest decompressed part of each shape kind in its Mid and High files. A shape file's header gives
   its own to the SDK, which sizes a Mii model's memory with them, and a game sizes its Mii heaps for the console's */
static const u32 shape_budget[Q_SHAPES][S_KINDS] = {
    {0x6fa, 0xbf0, 0x9f8, 0x10f0, 0x390, 0x6d0, 0x420, 0x23b8, 0x1f70, 0x4c, 0x33c, 0xe4},
    {0xa4a, 0x18c4, 0x12dc, 0x6088, 0x684, 0x1b78, 0xd0c, 0x10872, 0xb23c, 0x4c, 0xb82, 0xe4},
};
/* n less an eighth, not under least (n itself when it is not over least) */
static u32 fewer(u32 n, u32 least) {
    u32 m = n - (n + 7) / 8;
    return n <= least ? n : m < least ? least : m;
}
/* whether a shape is too big for quality q: its bytes over the console's largest of its kind there, or for Q_CTR,
   its vertices over the 256 a 3DS part's byte indexes reach */
static int shape_over(const Shape* sh, u32 q) {
    u32 off[5], size[5], idx_off, idx_size, nv = 0, g;
    if (q == Q_FULL) return 0;
    if (q != Q_CTR) return shape_raw_size(sh, off, size, &idx_off, &idx_size) > shape_budget[q][sh->kind];
    for (g = 0; g < sh->grids; g++) nv += (sh->ns[g] + 1) * (sh->nt[g] + 1);
    return nv > SNX_MII_CFL_VERTICES;
}
/* a part of a shape file of quality q: shape_of's, with an eighth fewer quads each way at a time (down to 3 by 1 a
   grid) until it is no longer over (shape_over). Hair that does not fit first goes to a column a lock's tip and one
   its valley around the head, which keeps its locks pointed, and gives up quads from its tails (down to five around)
   and its head's rows (down to four) before its columns */
static Shape shape_in(u32 kind, u32 variant, u32 q) {
    Shape sh = shape_of(kind, variant);
    u32 keep = 0;
    if ((kind == S_HAIR_NORMAL || kind == S_HAIR_CAP) && shape_over(&sh, q)) {
        sh.ns[0] = 2 * HAIR_LOCKS;
        keep = 1;
    }
    while (shape_over(&sh, q)) {
        u32 g, changed = 0;
        for (g = 0; g < sh.grids; g++) {
            u32 ns = keep && !g ? sh.ns[g] : fewer(sh.ns[g], keep ? 5 : 3), nt = fewer(sh.nt[g], keep && !g ? 4 : 1);
            changed |= ns != sh.ns[g] || nt != sh.nt[g];
            sh.ns[g] = ns;
            sh.nt[g] = nt;
        }
        if (!changed && !keep) break;
        if (!changed) keep = 0; /* still too big: the head's columns and rows too */
    }
    return sh;
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
    float ny = face_y(25.0f + 0.5f * h.ty, h.ty), ph = asinf((ny - h.cy) / h.ry_down);
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

/* the eye: G its white, B its iris (the pupil in it left black, a catchlight in it white toward the outer corner), A
   all of it with its outline and lashes */
static void tex_eye(u32 v, float x, float y, float aa, float c[4]) {
    /* the eye sits right of the middle, as the SDK places it, and low; a plush's, big: as wide and tall as its texture
       holds with its outline and lash */
    float cx = 0.69f, cy = 0.58f;
    float rx = 0.36f + 0.015f * (float)(v % 3), ry = 0.27f + 0.015f * (float)((v / 3) % 3);
    float tilt = 0.08f * ((float)(v % 4) - 1.5f), ir = ry * (0.95f + 0.08f * (float)(v % 2));
    float d = sd_ellipse(x, y, cx, cy, rx, ry, tilt), lid = cover(d, aa);
    float iris = cover(sd_circle(x, y, cx + 0.02f, cy + 0.01f, ir), aa) * lid;
    float pupil = cover(sd_circle(x, y, cx + 0.02f, cy + 0.01f, ir * 0.42f), aa) * lid;
    float glint = cover(sd_circle(x, y, cx + 0.02f - 0.4f * ir, cy + 0.01f - 0.4f * ir, ir * 0.26f), aa) * iris;
    /* the outline, its upper lid twice as thick as its lower */
    float all = cover(d - mixf(0.035f, 0.075f, smooth(cy, cy - 0.6f * ry, y)), aa);
    if (v % 2) { /* a lash at the outer corner */
        float ex = cx - rx * cosf(tilt), ey = cy - rx * sinf(tilt);
        all = fmaxf(all, cover(sd_capsule(x, y, ex + 0.03f, ey, ex - 0.09f, ey - 0.10f, 0.022f), aa));
    }
    c[0] = 0;
    c[1] = fmaxf(0, lid - iris) + glint;
    c[2] = fmaxf(0, iris - pupil - glint);
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
/* a GOB is 64 bytes by 8 rows (docs/layers/gpu/blocklinear.md); inside it byte x of row y is at mii_gob_x(x) + mii_gob_y(y) */
#define MII_GOB_X     64u
#define MII_GOB_Y     8u
#define MII_GOB_SHIFT 9
static u32 mii_gob_x(u32 x) { return ((x & 32) << 3) | ((x & 16) << 1) | (x & 15); }
static u32 mii_gob_y(u32 y) { return (((y & 7) >> 1) << 6) | ((y & 1) << 4); }
static u64 mii_gobs_x(u32 width_bytes) { return ((u64)width_bytes + MII_GOB_X - 1) / MII_GOB_X; }
static u64 mii_block_rows(u32 rows, u32 bh) { return (((u64)rows + MII_GOB_Y - 1) / MII_GOB_Y + (1u << bh) - 1) >> bh; }

/* a level's block height (log2 GOBs): level 0's the least that holds its rows, at most 16 GOBs; a smaller level's
   halves while half of it still holds the level's rows */
static u32 level_bh(u32 rows0, u32 rows) {
    u32 bh = 0;
    while (bh < 4 && (MII_GOB_Y << bh) < rows0) bh++;
    while (bh > 0 && rows <= (MII_GOB_Y << bh) / 2) bh--;
    return bh;
}
static u32 tex_bpb(u32 fmt) { return fmt == FMT_BC4 ? 8 : 16; }
static u32 level_size(const TexGeom* g, u32 l) {
    u32 w = g->w >> l ? g->w >> l : 1, h = g->h >> l ? g->h >> l : 1;
    u32 bx = (w + 3) / 4, by = (h + 3) / 4, rows0 = (g->h + 3) / 4, bh = level_bh(rows0, by);
    return (u32)(mii_gobs_x(bx * tex_bpb(g->fmt)) * MII_GOB_X * mii_block_rows(by, bh) * (MII_GOB_Y << bh));
}
static u32 tex_raw_size(const TexGeom* g) {
    u32 l, n = 0;
    for (l = 0; l < g->levels; l++) n += level_size(g, l);
    return n;
}
/* a texture's levels into raw, each drawn at its own size and laid out block linear */
static void tex_write(u32 kind, u32 v, const TexGeom* g, u8* raw) {
    float ar = (float)g->w / (float)g->h;
    u32 l, bpb = tex_bpb(g->fmt);
    for (l = 0; l < g->levels; l++) {
        u32 w = g->w >> l ? g->w >> l : 1, h = g->h >> l ? g->h >> l : 1;
        u32 bx = (w + 3) / 4, by = (h + 3) / 4, bh = level_bh((g->h + 3) / 4, by);
        u64 gobs_x = mii_gobs_x(bx * bpb);
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
                at = (((u64)(j >> (3 + bh)) * gobs_x + (xb >> 6)) << (MII_GOB_SHIFT + bh)) +
                     ((u64)((j >> 3) & ((1u << bh) - 1)) << MII_GOB_SHIFT) + mii_gob_x(xb & 63) + mii_gob_y(j & 7);
                memcpy(raw + at, block, bpb);
            }
        raw += level_size(g, l);
    }
}

/* ============================================================ the files */

/* the data a drawing shared by several parts already has in the file */
typedef struct {
    u32 off, raw, stored;
    u32 aoff[5], asize[5], idx_off, idx_size; /* a shape's attributes and triangles in its decompressed bytes */
    float box[6];                             /* a shape's bounding box */
} Placed;
#define VARIANTS_MAX (HAIR_VARIANTS + TAIL_STYLES)

static u32 texture_file(u32 mid, u8* b) {
    Out o;
    Placed placed[T_KINDS][VARIANTS_MAX];
    u32 largest[T_KINDS] = {0}, kind, i, e = 0;
    memset(placed, 0, sizeof placed);
    o.b = b;
    o.at = TEX_DATA;
    if (out_open(&o)) return 0;
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
                    u8* r = part_buffer(&o, raw);
                    if (!r) {
                        out_close(&o);
                        return 0;
                    }
                    tex_write(kind, v, &g, r);
                    p->stored = zpart(&o, raw);
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
                t[0x19] = (u8)g.levels;
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
    out_close(&o);
    return o.at;
}

/* the items of each section, in the order of SNX_MII_CFL_* and then the textures' */
static const u16 cfl_items[SNX_MII_CFL_SECTIONS] = {4,  264, 12, 264, 1,  264, 12, 18, 18, 132,
                                                    62, 24,  3,  12,  12, 9,   2,  37, 6,  18};
/* a mesh section's shape kind over the hair and (hat, forehead and hair, from item 132) under a cap */
static const u8 cfl_shape[SNX_MII_CFL_MESHES][2] = {
    {S_BEARD, S_BEARD}, {S_HAT_NORMAL, S_HAT_CAP},   {S_FACELINE, S_FACELINE}, {S_FOREHEAD_NORMAL, S_FOREHEAD_CAP},
    {S_GLASS, S_GLASS}, {S_HAIR_NORMAL, S_HAIR_CAP}, {S_MASK, S_MASK},         {S_NOSELINE, S_NOSELINE},
    {S_NOSE, S_NOSE}};
/* a texture section's kind */
static const u8 cfl_texture[SNX_MII_CFL_SECTIONS - SNX_MII_CFL_MESHES] = {
    T_HAT, T_EYE, T_EYEBROW, T_BEARD, T_WRINKLE, T_MAKEUP, T_GLASS, T_MOLE, T_MOUTH, T_MUSTACHE, T_NOSELINE};
static const char* const cfl_names[SNX_MII_CFL_MESHES] = {"beard", "hat",  "faceline", "forehead", "glass",
                                                          "hair",  "mask", "noseline", "nose"};
/* a Switch shape kind's part i as a mesh section's item */
static void kind_item(u32 kind, u32 i, u32* section, u32* item) {
    static const u8 sec[S_KINDS] = {SNX_MII_CFL_BEARD,    SNX_MII_CFL_FACELINE, SNX_MII_CFL_MASK,
                                    SNX_MII_CFL_HAT,      SNX_MII_CFL_HAT,      SNX_MII_CFL_FOREHEAD,
                                    SNX_MII_CFL_FOREHEAD, SNX_MII_CFL_HAIR,     SNX_MII_CFL_HAIR,
                                    SNX_MII_CFL_GLASS,    SNX_MII_CFL_NOSE,     SNX_MII_CFL_NOSELINE};
    *section = sec[kind];
    *item = kind == S_HAT_CAP || kind == S_FOREHEAD_CAP || kind == S_HAIR_CAP ? HAIR_STYLES + i : i;
}
/* the console's shape files' sizes: a game's heap holds them, and no more */
static const u32 shape_file_most[Q_SHAPES] = {0xe517c, 0x2bcf40};
/* a mesh's decompressed part as kind's: its attributes (a hair's tangents and colors in place of texture coordinates) */
static u32 mesh_raw_size(u32 kind, const SnxMiiMesh* m, u32 off[5], u32 size[5], u32* idx_off, u32* idx_size) {
    static const u8 bytes[5] = {8, 4, 4, 4, 4};
    u32 attrs = kind == S_HAIR_NORMAL || kind == S_HAIR_CAP ? 1u << A_POS | 1u << A_NRM | 1u << A_TAN | 1u << A_COL
                                                            : 1u << A_POS | 1u << A_NRM | 1u << A_UV,
        at = 0, a;
    for (a = 0; a < 5; a++) {
        off[a] = size[a] = 0;
        if (!(attrs & (1u << a))) continue;
        off[a] = at;
        size[a] = m->vertices * bytes[a];
        at += size[a];
    }
    *idx_off = (at + 3) & ~3u;
    *idx_size = m->indices * 2;
    return *idx_off + *idx_size;
}
/* a mesh's bytes into raw, as shape_write writes a shape's, and its box */
static void mesh_write(u32 kind, const SnxMiiMesh* m, u8* raw, float box[6]) {
    u32 off[5], size[5], idx_off, idx_size, n, k;
    mesh_raw_size(kind, m, off, size, &idx_off, &idx_size);
    box[0] = box[1] = box[2] = 1e30f;
    box[3] = box[4] = box[5] = -1e30f;
    for (n = 0; n < m->vertices; n++) {
        Vec p = vec(m->pos[n][0], m->pos[n][1], m->pos[n][2]), nr = vec(m->nrm[n][0], m->nrm[n][1], m->nrm[n][2]);
        u8* q = raw + off[A_POS] + 8 * n;
        snx_st_le16(q, half_of(p.x));
        snx_st_le16(q + 2, half_of(p.y));
        snx_st_le16(q + 4, half_of(p.z));
        snx_st_le16(q + 6, half_of(1.0f));
        snx_st_le32(raw + off[A_NRM] + 4 * n, snorm10(nr.x) | snorm10(nr.y) << 10 | snorm10(nr.z) << 20 | 1u << 30);
        if (size[A_UV]) {
            snx_st_le16(raw + off[A_UV] + 4 * n, half_of(m->uv ? m->tex[n][0] : 0));
            snx_st_le16(raw + off[A_UV] + 4 * n + 2, half_of(m->uv ? m->tex[n][1] : 1));
        }
        if (size[A_TAN]) { /* as shape_write: the strands run down the surface */
            Vec down = vsub(vec(0, -1, 0), vmul(nr, -nr.y)), tg;
            if (vlen(down) < 0.05f) down = vsub(vec(0, 0, -1), vmul(nr, -nr.z));
            tg = vnorm(down);
            raw[off[A_TAN] + 4 * n] = snorm8(tg.x);
            raw[off[A_TAN] + 4 * n + 1] = snorm8(tg.y);
            raw[off[A_TAN] + 4 * n + 2] = snorm8(tg.z);
            raw[off[A_TAN] + 4 * n + 3] = 127;
            raw[off[A_COL] + 4 * n + 1] = 255;
            raw[off[A_COL] + 4 * n + 3] = 255;
        }
        for (k = 0; k < 3; k++) {
            float v = k == 0 ? p.x : k == 1 ? p.y : p.z;
            if (v < box[k]) box[k] = v;
            if (v > box[3 + k]) box[3 + k] = v;
        }
    }
    for (n = 0; n < m->indices; n++) snx_st_le16(raw + idx_off + 2 * n, m->idx[n]);
}
static u32 shape_file(u32 q, u8* b, SnxMiiMeshSource source, void* ctx) {
    static const char* const qname[Q_SHAPES] = {"ShapeMid.dat", "ShapeHigh.dat"};
    Out o;
    Placed placed[S_KINDS][VARIANTS_MAX], given[S_KINDS][HAIR_STYLES];
    float hair_tf[HAIR_STYLES][18], face_tf[FACELINES][9];
    SnxMiiMesh* m = NULL;
    u32 largest[S_KINDS] = {0}, kind, i, k, e = 0;
    memset(placed, 0, sizeof placed);
    memset(given, 0, sizeof given);
    for (i = 0; i < HAIR_STYLES; i++) hair_transform(i, hair_tf[i]);
    for (i = 0; i < FACELINES; i++) faceline_transform(i, face_tf[i]);
    if (source && !(m = (SnxMiiMesh*)malloc(sizeof *m))) {
        fprintf(stderr, "mii parts: no memory for a mesh (%zu bytes)\n", sizeof *m);
        return 0;
    }
    o.b = b;
    o.at = SHAPE_DATA;
    if (out_open(&o)) {
        free(m);
        return 0;
    }
    for (kind = 0; kind < S_KINDS; kind++)
        for (i = 0; i < shape_counts[kind]; i++, e++) {
            Shape sh = shape_in(kind, shape_variant(kind, i), q);
            Placed* p = &placed[kind][sh.variant];
            int r = SNX_MII_MESH_OURS, fresh;
            u32 sec = 0, item = 0;
            if (source) {
                kind_item(kind, i, &sec, &item);
                memset(m, 0, sizeof *m);
                if ((r = source(ctx, sec, item, m)) < 0) goto stop;
            }
            if (!r || (r == SNX_MII_MESH_OURS && !shape_present(kind, i))) continue;
            if (r == 1 && m->same && m->same - 1 < item - i && m->vertices)
                m->same = 0;         /* the same as the other form's part: written again in this kind */
            if (r == 1 && m->same) { /* the earlier part the source says this one is */
                u32 j = m->same - 1 - (item - i);
                if (m->same - 1 >= item || j >= i) {
                    fprintf(stderr, "mii parts: %s %u is said to be the same as item %u, not an earlier one\n",
                            cfl_names[sec], item, m->same - 1);
                    goto stop;
                }
                p = given[kind][j].raw ? &given[kind][j] : &placed[kind][shape_variant(kind, j)];
                if (!p->raw) {
                    fprintf(stderr, "mii parts: %s %u is said to be the same as item %u, which is absent\n",
                            cfl_names[sec], item, m->same - 1);
                    goto stop;
                }
                if (kind == S_HAIR_NORMAL) memcpy(hair_tf[i], hair_tf[j], sizeof hair_tf[i]);
                if (kind == S_FACELINE) memcpy(face_tf[i], face_tf[j], sizeof face_tf[i]);
                given[kind][i] = *p;
            } else if (r == 1) { /* a mesh of the source's own */
                p = &given[kind][i];
                p->raw = mesh_raw_size(kind, m, p->aoff, p->asize, &p->idx_off, &p->idx_size);
                if (!m->vertices || m->vertices > 0xffff || !m->indices || m->indices % 3 ||
                    p->raw > shape_budget[q][kind]) {
                    fprintf(stderr,
                            "mii parts: %s %u has %u vertices and %u indices, %u bytes in %s; a part is whole "
                            "triangles, and the console's largest %s there, which the game's heap is sized for, has "
                            "%u\n",
                            cfl_names[sec], item, m->vertices, m->indices, p->raw, qname[q], cfl_names[sec],
                            shape_budget[q][kind]);
                    goto stop;
                }
                for (k = 0; k < m->indices; k++)
                    if (m->idx[k] >= m->vertices) {
                        fprintf(stderr, "mii parts: %s %u's index %u is %u, past its %u vertices\n", cfl_names[sec],
                                item, k, m->idx[k], m->vertices);
                        goto stop;
                    }
                p->raw = 0; /* placed below */
                if (kind == S_HAIR_NORMAL)
                    for (k = 0; k < 3; k++) { /* the Switch's order: each place, then its rotation */
                        memcpy(hair_tf[i] + 6 * k, m->anchor + 6 * k + 3, 3 * sizeof(float));
                        memcpy(hair_tf[i] + 6 * k + 3, m->anchor + 6 * k, 3 * sizeof(float));
                    }
                if (kind == S_FACELINE) memcpy(face_tf[i], m->anchor, sizeof face_tf[i]);
            }
            fresh = !p->raw;
            if (fresh) {
                u32 raw = r == 1 ? mesh_raw_size(kind, m, p->aoff, p->asize, &p->idx_off, &p->idx_size)
                                 : shape_raw_size(&sh, p->aoff, p->asize, &p->idx_off, &p->idx_size);
                o.at = (o.at + 15) & ~15u;
                p->off = o.at;
                p->raw = raw;
                p->stored = zstored_size(raw);
                if (b) {
                    u8* w = part_buffer(&o, raw);
                    if (!w) goto stop;
                    if (r == 1)
                        mesh_write(kind, m, w, p->box);
                    else
                        shape_write(&sh, w, p->box);
                    p->stored = zpart(&o, raw);
                }
                o.at += p->stored;
            }
            if (p->raw > largest[kind]) largest[kind] = p->raw;
            if (b) {
                u8* t = b + SHAPE_TABLE + SHAPE_ENTRY * e;
                snx_st_le32(t, p->off);
                snx_st_le32(t + 4, p->raw);
                snx_st_le32(t + 8, p->stored);
                snx_st_le32(t + 0xc, PART_ZLIB);
                for (k = 0; k < 5; k++) {
                    snx_st_le32(t + 0x10 + 4 * k, p->aoff[k]);
                    snx_st_le32(t + 0x24 + 4 * k, p->asize[k]);
                }
                snx_st_le32(t + 0x38, p->idx_off);
                snx_st_le32(t + 0x3c, p->idx_size);
                for (k = 0; k < 6; k++) {
                    u32 bits;
                    memcpy(&bits, &p->box[k], 4);
                    snx_st_le32(t + 0x40 + 4 * k, bits);
                }
            }
        }
    if (source && o.at > shape_file_most[q]) {
        fprintf(stderr, "mii parts: %s takes %u bytes; the console's, which the game's heap is sized for, %u\n",
                qname[q], o.at, shape_file_most[q]);
        goto stop;
    }
    if (b) {
        memcpy(b, "NFSR", 4);
        snx_st_le32(b + 4, 1);
        snx_st_le32(b + 8, o.at);
        for (kind = 0; kind < S_KINDS; kind++) {
            snx_st_le32(b + 0xc + 4 * kind, largest[kind]);
            snx_st_le32(b + 0xc + 4 * S_KINDS + 4 * kind, SHAPE_ALIGN);
        }
        for (i = 0; i < HAIR_STYLES; i++)
            for (k = 0; k < 18; k++) {
                u32 bits;
                memcpy(&bits, &hair_tf[i][k], 4);
                snx_st_le32(b + HAIR_TF + 0x48 * i + 4 * k, bits);
            }
        for (i = 0; i < FACELINES; i++)
            for (k = 0; k < 9; k++) {
                u32 bits;
                memcpy(&bits, &face_tf[i][k], 4);
                snx_st_le32(b + FACE_TF + 0x24 * i + 4 * k, bits);
            }
    }
    out_close(&o);
    free(m);
    return o.at;
stop:
    out_close(&o);
    free(m);
    return 0;
}

void snx_mii_model_name(u32 i, char name[32]) {
    static const char* const names[SNX_MII_MODEL_FILES] = {"NXTextureLowLinear.dat", "NXTextureLowSRGB.dat",
                                                           "NXTextureMidLinear.dat", "NXTextureMidSRGB.dat",
                                                           "ShapeMid.dat",           "ShapeHigh.dat"};
    snprintf(name, 32, "%s", names[i]);
}
u32 snx_mii_model_same(u32 i) { return i == 1 || i == 3 ? i - 1 : i; }
u32 snx_mii_model_write(u32 i, u8* out) {
    return i < 4 ? texture_file(i >= 2, out) : shape_file(i - 4, out, NULL, NULL);
}
u32 snx_mii_model_write_from(u32 i, SnxMiiMeshSource source, void* ctx, u8* out) {
    return i < 4 ? texture_file(i >= 2, out) : shape_file(i - 4, out, source, ctx);
}

/* ============================================================ a Mii's picture */

/* the picture's window: x from -55 to 55, y from -32 to 78 (the chin at 0, the plush's wide hair inside it, its
   shoulders under it), seen from +z */
#define PIC_X0   (-55.0f)
#define PIC_Y1   78.0f
#define PIC_SPAN 110.0f
enum { PIC_NONE, PIC_SKIN, PIC_FACE, PIC_HAIR, PIC_TIE }; /* what a pixel shows: the face takes the features */
#define PIC_EDGE 0x80u /* with what: the pixel is on an outline, the edge of a part in front of another or of nothing */
typedef struct {
    int side;
    float scale;
    float* z;     /* side * side depths, nearest kept */
    u8* what;     /* PIC_ each */
    float* lit;   /* its shading */
    float* shine; /* the light the hair reflects there, added as white */
} Pic;
/* the triangle a, b, c (normals na, nb, nc) into the depth buffer as what, its back left out */
static void pic_triangle(Pic* p, Vec a, Vec b, Vec c, Vec na, Vec nb, Vec nc, u8 what) {
    float ax = (a.x - PIC_X0) * p->scale, ay = (PIC_Y1 - a.y) * p->scale, bx = (b.x - PIC_X0) * p->scale,
          by = (PIC_Y1 - b.y) * p->scale, cx = (c.x - PIC_X0) * p->scale, cy = (PIC_Y1 - c.y) * p->scale;
    float area = (bx - ax) * (cy - ay) - (cx - ax) * (by - ay);
    int x0, x1, y0, y1, x, y;
    Vec light = vnorm(vec(0.3f, 0.5f, 0.8f)), half = vnorm(vadd(light, vec(0, 0, 1)));
    if (fabsf(area) < 1e-6f) return;
    x0 = (int)floorf(fminf(ax, fminf(bx, cx))), x1 = (int)ceilf(fmaxf(ax, fmaxf(bx, cx)));
    y0 = (int)floorf(fminf(ay, fminf(by, cy))), y1 = (int)ceilf(fmaxf(ay, fmaxf(by, cy)));
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > p->side - 1) x1 = p->side - 1;
    if (y1 > p->side - 1) y1 = p->side - 1;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = ((bx - px) * (cy - py) - (cx - px) * (by - py)) / area;
            float w1 = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / area;
            float w2 = 1.0f - w0 - w1, z;
            size_t i = (size_t)y * (size_t)p->side + (size_t)x;
            Vec n;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            z = w0 * a.z + w1 * b.z + w2 * c.z;
            if (z <= p->z[i]) continue;
            n = vnorm(vadd(vadd(vmul(na, w0), vmul(nb, w1)), vmul(nc, w2)));
            if (n.z < 0) continue; /* its back */
            p->z[i] = z;
            p->what[i] = what;
            p->lit[i] = 0.55f + 0.45f * fmaxf(0, vdot(n, light));
            if (what == PIC_HAIR) { /* a band across its strands, which run down it, where they reflect the light */
                Vec strand = vec(n.x * n.y, n.y * n.y - 1.0f, n.z * n.y);
                float along = vlen(strand) > 0.05f ? vdot(vnorm(strand), half) : 0;
                p->shine[i] = 0.18f * powf(fmaxf(0, 1.0f - along * along), 60.0f);
            }
        }
}
/* a shape's every grid into the picture, moved by at and mirrored in x with flip */
static void pic_shape(Pic* p, Shape sh, Vec at, int flip, u8 what) {
    for (sh.g = 0; sh.g < sh.grids; sh.g++) {
        u32 i, j, ns = sh.ns[sh.g], nt = sh.nt[sh.g];
        for (j = 0; j < nt; j++)
            for (i = 0; i < ns; i++) {
                float s0 = (float)i / (float)ns, s1 = (float)(i + 1) / (float)ns, t0 = (float)j / (float)nt,
                      t1 = (float)(j + 1) / (float)nt;
                Vec q[4], n[4];
                const float st[4][2] = {{s0, t0}, {s1, t0}, {s1, t1}, {s0, t1}};
                u32 k;
                for (k = 0; k < 4; k++) {
                    q[k] = shape_at(&sh, st[k][0], st[k][1]);
                    n[k] = shape_normal(&sh, st[k][0], st[k][1], q[k]);
                    q[k] = vadd(q[k], at);
                    if (flip) q[k].x = -q[k].x, n[k].x = -n[k].x;
                }
                pic_triangle(p, q[0], q[1], q[2], n[0], n[1], n[2], what);
                pic_triangle(p, q[0], q[2], q[3], n[0], n[2], n[3], what);
            }
    }
}
/* the outlines: a pixel of a part whose neighbor (w pixels away) is nothing, or the face's while it is hair's or the
   hair's while it is the face's and behind it, or hair 3 units or more behind it (a tail in front of the head's hair) */
static void pic_edges(Pic* p) {
    int w = 1 + p->side / 256, x, y, k;
    static const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
    for (y = 0; y < p->side; y++)
        for (x = 0; x < p->side; x++) {
            size_t i = (size_t)y * (size_t)p->side + (size_t)x;
            u8 a = p->what[i] & 0x7fu;
            if (a == PIC_NONE) continue;
            for (k = 0; k < 4; k++) {
                int nx = x + w * d[k][0], ny = y + w * d[k][1];
                size_t j;
                u8 b;
                int hair_a = a == PIC_HAIR || a == PIC_TIE, hair_b;
                if (nx < 0 || ny < 0 || nx >= p->side || ny >= p->side) continue;
                j = (size_t)ny * (size_t)p->side + (size_t)nx;
                b = p->what[j] & 0x7fu;
                hair_b = b == PIC_HAIR || b == PIC_TIE;
                if (b == PIC_NONE || (hair_a != hair_b && p->z[j] < p->z[i]) ||
                    (hair_a && hair_b && p->z[j] < p->z[i] - 3.0f)) {
                    p->what[i] |= PIC_EDGE;
                    break;
                }
            }
        }
}
/* what an outline keeps of the color it darkens in a picture side pixels wide: more in a small one, of which a line
   a pixel wide is more */
static float pic_line(int side) { return mixf(0.85f, 0.55f, smooth(32.0f, 128.0f, (float)side)); }
/* the eye texel the CharInfo c puts at (x, y) of the SDK's frame (face_sdk's), the faceline risen by ty: its texture's
   middle where the eye goes (our own reading of the fields), mirrored for the Mii's left; G its white, B its iris, A
   all of it. 0 where no eye's texture is */
static int face_eye(const u8* c, float x, float y, float ty, float aa, float col[4]) {
    float w = 12.0f * (1.0f + 0.1f * ((float)c[0x36] - 4.0f)), h = w * 128.0f / 152.0f;
    float ey = 34.0f + ty - ((float)c[0x3a] - 12.0f), v = (ey - y) / h + 0.5f;
    int side;
    for (side = -1; side <= 1; side += 2) {
        float ex = (float)side * (8.0f + 0.9f * (float)c[0x39]), u = (x - ex) / w * (float)side * -1.0f + 0.5f;
        if (u < 0 || u > 1 || v < 0 || v > 1) continue;
        tex_eye(tex_variant(T_EYE, c[0x34]), u * 152.0f / 128.0f, v, aa / h, col);
        return col[3] > 0;
    }
    return 0;
}
/* the color a face pixel at (x, y) of the SDK's frame (face_sdk's) takes from the features drawn on it: eyes, brows and
   mouth where the CharInfo puts them (our own reading of its fields), over skin; ty the faceline's rise */
static void pic_features(const u8* c, const SnxMiiLook* look, float x, float y, float ty, float aa, float rgb[3]) {
    float eye_y = 34.0f + ty - ((float)c[0x3a] - 12.0f), col[4];
    int side, k;
    if (face_eye(c, x, y, ty, aa, col)) /* the eye in its color, its white, its outline dark */
        for (k = 0; k < 3; k++) {
            float want = (look->eye[k] / 255.0f) * col[2] + col[1] + 0.08f * fmaxf(0, col[3] - col[1] - col[2]);
            rgb[k] = rgb[k] * (1 - col[3]) + fminf(1, want) * col[3];
        }
    for (side = -1; side <= 1; side += 2) { /* the brow above each eye, its thick end toward the nose */
        float w = 12.0f * (1.0f + 0.1f * ((float)c[0x3d] - 4.0f)), h = w * 128.0f / 144.0f;
        float ex = (float)side * (8.0f + 0.9f * (float)c[0x40]), u = (x - ex) / w * (float)side * -1.0f + 0.5f;
        float v = (eye_y + 8.0f - 0.8f * ((float)c[0x41] - 10.0f) - y) / h + 0.5f;
        if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
            float a =
                tex_value(T_EYEBROW, tex_variant(T_EYEBROW, c[0x3b]), u * 144.0f / 128.0f, v, aa / h, 144.0f / 128.0f);
            for (k = 0; k < 3; k++) rgb[k] = rgb[k] * (1 - a) + (look->hair[k] / 255.0f) * a;
        }
    }
    { /* the mouth: R its lips, G its teeth, the rest of it dark */
        float w = 16.0f * (1.0f + 0.1f * ((float)c[0x47] - 4.0f)), h = w * 128.0f / 176.0f, col[4];
        float u = x / w + 0.5f, v = (16.0f + ty - ((float)c[0x49] - 13.0f) - y) / h + 0.5f;
        if (u >= 0 && u <= 1 && v >= 0 && v <= 1) {
            tex_mouth(tex_variant(T_MOUTH, c[0x45]), u * 176.0f / 128.0f, v, aa / h, col);
            for (k = 0; k < 3; k++) {
                float want = (look->mouth[k] / 255.0f) * col[0] + col[1] + 0.1f * fmaxf(0, col[3] - col[0] - col[1]);
                rgb[k] = rgb[k] * (1 - col[3]) + fminf(1, want) * col[3];
            }
        }
    }
}
int snx_mii_picture(const u8 charinfo[SNX_MII_CHARINFO], const SnxMiiLook* look, int side, u8* rgba) {
    const u8* c = charinfo;
    u32 face = c[0x2d] % FACELINES, style = c[0x31] % HAIR_STYLES, nose = c[0x42] % 18u, n, i;
    float tf[9], ty, aa, line;
    Pic p;
    u8* block;
    if (side < 16 || side > 1024) return -1;
    n = (u32)side * (u32)side;
    /* the depths, the shading, the shine, then what each shows */
    block = (u8*)malloc((size_t)n * (sizeof(float) * 3 + 1));
    if (!block) return -1;
    p.side = side;
    p.scale = (float)side / PIC_SPAN;
    p.z = (float*)(void*)block;
    p.lit = p.z + n;
    p.shine = p.lit + n;
    p.what = block + (size_t)n * sizeof(float) * 3;
    for (i = 0; i < n; i++) p.z[i] = -1e30f, p.what[i] = PIC_NONE, p.lit[i] = 1, p.shine[i] = 0;
    faceline_transform(face, tf);
    ty = tf[1];
    aa = PIC_SPAN / (float)side;
    pic_shape(&p, shape_of(S_FACELINE, face), vec(0, 0, 0), 0, PIC_FACE);
    if (shape_present(S_NOSE, nose))
        pic_shape(&p, shape_of(S_NOSE, shape_variant(S_NOSE, nose)), vec(0, tf[4], tf[5]), 0, PIC_SKIN);
    if (shape_present(S_HAIR_NORMAL, style))
        pic_shape(&p, shape_of(S_HAIR_NORMAL, hair_variant(style)), vec(0, ty, 0), c[0x33] & 1, PIC_HAIR);
    if (shape_present(S_HAT_NORMAL, style))
        pic_shape(&p, shape_of(S_HAT_NORMAL, hat_variant(style)), vec(0, ty, 0), c[0x33] & 1, PIC_TIE);
    pic_edges(&p);
    line = pic_line(side);
    for (i = 0; i < n; i++) {
        float x = PIC_X0 + ((float)(i % (u32)side) + 0.5f) / p.scale,
              y = PIC_Y1 - ((float)(i / (u32)side) + 0.5f) / p.scale;
        float rgb[3], light = 1.0f - 0.25f * (float)(i / (u32)side) / (float)side;
        u32 k;
        u8 what = p.what[i] & 0x7fu;
        const uint8_t* col = what == PIC_HAIR ? look->hair : what == PIC_TIE ? look->favorite : look->skin;
        for (k = 0; k < 3; k++) /* the background: the favorite color lightened, darker toward the bottom */
            rgb[k] = (0.7f + 0.3f * (look->favorite[k] / 255.0f)) * light;
        if (what == PIC_NONE) {
            /* the shirt over the shoulders, the neck above it */
            float sx = x / 34.0f, sy = (y + 34.0f) / 30.0f;
            if (sx * sx + sy * sy < 1.0f && y < 4.0f)
                for (k = 0; k < 3; k++) rgb[k] = look->favorite[k] / 255.0f * (0.85f + 0.15f * (1 - sx * sx));
            else if (fabsf(x) < 7.0f && y < 6.0f && y > -6.0f)
                for (k = 0; k < 3; k++) rgb[k] = look->skin[k] / 255.0f * 0.8f;
        } else {
            for (k = 0; k < 3; k++) rgb[k] = col[k] / 255.0f;
            if (what == PIC_FACE) {
                float xs, ys;
                face_sdk(x, y, ty, &xs, &ys);
                pic_features(c, look, xs, ys, ty, aa / FACE_KX, rgb);
            }
            for (k = 0; k < 3; k++) rgb[k] = (rgb[k] * p.lit[i] + p.shine[i]) * (p.what[i] & PIC_EDGE ? line : 1.0f);
        }
        for (k = 0; k < 3; k++) rgba[4 * i + k] = (u8)lroundf(clampf(rgb[k], 0, 1) * 255.0f);
        rgba[4 * i + 3] = 255;
    }
    free(block);
    return 0;
}

/* ============================================================ the 3DS's parts file, CFL_Res.dat */

#define CFL_RGBA4 10u /* the face library's texture format: 16 bits, 4 a channel */
#define CFL_INDEX 4u  /* the word before a mesh's index list (a list of triangles) */
#define CFL_OFFSET                                                                                                     \
    22 /* an item's offset in its section's table takes the low 22 bits, the item it is the same as
                            (1 + its index) the high 10 */

static void cfl_kind(u32 section, u32 item, u32* kind, u32* i) {
    u32 cap = cfl_items[section] == 2 * HAIR_STYLES && item >= HAIR_STYLES;
    *kind = cfl_shape[section][cap];
    *i = cap ? item - HAIR_STYLES : item;
}

/* section's item as a mesh of quality q (Q_CTR or Q_FULL), as snx_mii_ctr_mesh gives it */
static int part_mesh(u32 section, u32 item, u32 q, SnxMiiMesh* m) {
    u32 kind, i, j, n = 0, g, base, w, k, most = q == Q_CTR ? SNX_MII_CFL_VERTICES : SNX_MII_MESH_VERTICES;
    Shape grid;
    if (section >= SNX_MII_CFL_MESHES || item >= cfl_items[section]) {
        fprintf(stderr, "mii parts: no mesh section %u item %u: there are %d sections of 4 to 264 items\n", section,
                item, SNX_MII_CFL_MESHES);
        return -1;
    }
    cfl_kind(section, item, &kind, &i);
    if (!shape_present(kind, i)) return 0;
    for (j = 0; j < item; j++) { /* an earlier item of the section drawn the same: this one is it */
        u32 kj, ij;
        cfl_kind(section, j, &kj, &ij);
        if (kj == kind && shape_present(kj, ij) && shape_variant(kj, ij) == shape_variant(kind, i)) {
            m->same = j + 1;
            return 1;
        }
    }
    grid = shape_in(kind, shape_variant(kind, i), q);
    m->uv = (grid.attrs >> A_UV) & 1u;
    for (grid.g = 0; grid.g < grid.grids; grid.g++)
        for (j = 0; j <= grid.nt[grid.g]; j++)
            for (k = 0; k <= grid.ns[grid.g]; k++, n++) {
                float s = (float)k / (float)grid.ns[grid.g], t = (float)j / (float)grid.nt[grid.g];
                Vec p = shape_at(&grid, s, t), nr = shape_normal(&grid, s, t, p);
                if (n >= most) {
                    fprintf(stderr, "mii parts: our %s %u has more than %u vertices, the most %s\n", cfl_names[section],
                            item, most, q == Q_CTR ? "a 3DS part has" : "a mesh holds");
                    return -1;
                }
                m->pos[n][0] = p.x, m->pos[n][1] = p.y, m->pos[n][2] = p.z;
                m->nrm[n][0] = nr.x, m->nrm[n][1] = nr.y, m->nrm[n][2] = nr.z;
                if (m->uv) shape_uv(&grid, p, s, t, m->tex[n]);
            }
    m->vertices = n;
    for (g = 0, n = 0, base = 0; g < grid.grids; base += (grid.ns[g] + 1) * (grid.nt[g] + 1), g++)
        for (j = 0, w = grid.ns[g] + 1; j < grid.nt[g]; j++)
            for (k = 0; k < grid.ns[g]; k++) { /* two triangles a quad, counterclockwise as shape_write's */
                u16 a = (u16)(base + j * w + k), b = (u16)(a + 1), c = (u16)(a + w + 1), d = (u16)(a + w);
                const u16 tri[6] = {a, b, c, a, c, d};
                u32 q;
                if (n + 6 > SNX_MII_MESH_INDICES) {
                    fprintf(stderr, "mii parts: our %s %u has more than %d triangles\n", cfl_names[section], item,
                            SNX_MII_MESH_INDICES / 3);
                    return -1;
                }
                for (q = 0; q < 6; q++) m->idx[n++] = tri[q];
            }
    m->indices = n;
    if (section == SNX_MII_CFL_FACELINE) faceline_transform(i, m->anchor);
    if (section == SNX_MII_CFL_HAIR) { /* each of the hat's places: its rotation, then the place */
        float t[18];
        hair_transform(i, t);
        for (k = 0; k < 3; k++) {
            memcpy(m->anchor + 6 * k, t + 6 * k + 3, 3 * sizeof *t);
            memcpy(m->anchor + 6 * k + 3, t + 6 * k, 3 * sizeof *t);
        }
    }
    return 1;
}

int snx_mii_ctr_mesh(void* ctx, u32 section, u32 item, SnxMiiMesh* m) {
    (void)ctx;
    return part_mesh(section, item, Q_CTR, m);
}

/* the value in 1/scale steps as the file's signed 16 bits, 0 with *bad set when it does not fit */
static u16 cfl_fixed(float v, float scale, int* bad) {
    float f = v * scale;
    if (!(f > -32768.5f && f < 32767.5f)) {
        *bad = 1;
        return 0;
    }
    return (u16)(i16)lroundf(f);
}
/* whether mesh m can be section's item: says why not */
static int cfl_mesh_fits(u32 section, u32 item, const SnxMiiMesh* m) {
    u32 i, a;
    int bad = 0;
    if (!m->vertices || m->vertices > SNX_MII_CFL_VERTICES || !m->indices || m->indices % 3 || m->indices > 0xffff) {
        fprintf(stderr,
                "mii parts: %s %u has %u vertices and %u indices; a 3DS part has 1 to %d vertices and a whole number "
                "of triangles, at most 65535 indices\n",
                cfl_names[section], item, m->vertices, m->indices, SNX_MII_CFL_VERTICES);
        return 0;
    }
    for (i = 0; i < m->indices; i++)
        if (m->idx[i] >= m->vertices) {
            fprintf(stderr, "mii parts: %s %u's index %u is %u, past its %u vertices\n", cfl_names[section], item, i,
                    m->idx[i], m->vertices);
            return 0;
        }
    for (i = 0; i < m->vertices; i++) {
        for (a = 0; a < 3; a++) cfl_fixed(m->pos[i][a], 256.0f, &bad), cfl_fixed(m->nrm[i][a], 256.0f, &bad);
        if (m->uv) cfl_fixed(m->tex[i][0], 8192.0f, &bad), cfl_fixed(m->tex[i][1], 8192.0f, &bad);
        if (bad) {
            fprintf(stderr,
                    "mii parts: %s %u's vertex %u is at (%g, %g, %g), texture (%g, %g): a 3DS part's places reach 128 "
                    "and its texture coordinates 4 either way\n",
                    cfl_names[section], item, i, (double)m->pos[i][0], (double)m->pos[i][1], (double)m->pos[i][2],
                    (double)(m->uv ? m->tex[i][0] : 0), (double)(m->uv ? m->tex[i][1] : 0));
            return 0;
        }
    }
    return 1;
}
/* mesh m as section's item at out + at: the end, 0 when it does not fit in room */
static u32 cfl_mesh(u32 section, const SnxMiiMesh* m, u8* out, u32 at, u32 room) {
    u32 anchors = section == SNX_MII_CFL_FACELINE ? 9 : section == SNX_MII_CFL_HAIR ? 18 : 0;
    u32 per = 12 + (m->uv ? 4 : 0), i, a, end;
    int bad = 0;
    end = at + 4 * anchors + 8 + per * m->vertices + 4 + m->indices +
          (section == SNX_MII_CFL_MASK ? 2 * m->indices / 3 : 0);
    end = (end + 3) & ~3u;
    if (end > room || end < at) return 0;
    for (a = 0; a < anchors; a++) {
        union {
            float f;
            u32 u;
        } x;
        x.f = m->anchor[a];
        snx_st_le32(out + at, x.u);
        at += 4;
    }
    snx_st_le16(out + at, (u16)m->vertices);
    snx_st_le16(out + at + 2, (u16)m->vertices); /* a normal each */
    snx_st_le16(out + at + 4, (u16)(m->uv ? m->vertices : 0));
    snx_st_le16(out + at + 6, 1); /* one index list */
    at += 8;
    for (i = 0; i < m->vertices; i++) {
        for (a = 0; a < 3; a++) snx_st_le16(out + at + 2 * a, cfl_fixed(m->pos[i][a], 256.0f, &bad));
        for (a = 0; a < 3; a++) snx_st_le16(out + at + 6 + 2 * a, cfl_fixed(m->nrm[i][a], 256.0f, &bad));
        at += 12;
        if (m->uv) {
            snx_st_le16(out + at, cfl_fixed(m->tex[i][0], 8192.0f, &bad));
            snx_st_le16(out + at + 2, cfl_fixed(m->tex[i][1], 8192.0f, &bad));
            at += 4;
        }
    }
    snx_st_le16(out + at, CFL_INDEX);
    snx_st_le16(out + at + 2, (u16)m->indices);
    at += 4;
    for (i = 0; i < m->indices; i++) out[at++] = (u8)m->idx[i];
    if (section == SNX_MII_CFL_MASK) /* each triangle's cover of the mask's 16 by 16 grid: all of it */
        for (i = 0; i < m->indices / 3; i++, at += 2) out[at] = out[at + 1] = 0x0f;
    while (at < end) out[at++] = 0;
    return end;
}

/* a texture side for the 3DS: the power of two at most half the Switch's low one, 8 to 64 */
static u32 cfl_side(u32 v) {
    u32 p = 8;
    while (p * 2 <= v / 2 && p < 64) p *= 2;
    return p;
}
/* texture section's item at out + at: the end (at when the part is absent), 0 when it does not fit in room. *same: 1 +
   the earlier item it is drawn as, or 0. Every texture is RGBA4, a channel of one-channel drawings in all four, the
   eyes' and mouths' as the Switch's (G white or teeth, B iris, R lips, A all of it), the glasses' cover in A too */
static u32 cfl_tex(u32 section, u32 item, u8* out, u32 at, u32 room, u32* same) {
    static const u8 lut[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15}; /* x's bits in an 8 by 8 tile */
    u32 kind = cfl_texture[section - SNX_MII_CFL_MESHES], v, j, x, y, end;
    TexGeom sw;
    float ar, aa;
    u32 w, h;
    *same = 0;
    if (!tex_present(kind, item)) return at;
    v = tex_variant(kind, item) % tex_variants[kind];
    for (j = 0; j < item; j++)
        if (tex_present(kind, j) && tex_variant(kind, j) % tex_variants[kind] == v) {
            *same = j + 1;
            return at;
        }
    sw = tex_geom(kind, item, 0);
    w = cfl_side(sw.w), h = cfl_side(sw.h);
    ar = (float)sw.w / (float)sw.h, aa = 1.4f / (float)h;
    end = at + 8 + 2 * w * h;
    if (end > room || end < at) return 0;
    snx_st_le16(out + at, (u16)w);
    snx_st_le16(out + at + 2, (u16)h);
    out[at + 4] = 1; /* one level */
    out[at + 5] = CFL_RGBA4;
    out[at + 6] = kind == T_NOSELINE ? 0
                  : kind == T_HAT    ? 1
                                     : 2; /* u: clamp, repeat, or mirrored at the face's middle */
    out[at + 7] = 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            float c[4];
            u32 px = (y / 8) * w * 8 + (x / 8) * 64 + lut[x % 8] + 2u * lut[y % 8], k;
            u16 rgba = 0;
            tex_texel(kind, v, ((float)x + 0.5f) / (float)w * ar, ((float)y + 0.5f) / (float)h, aa, ar, c);
            if (kind == T_GLASS)
                c[3] = c[1];
            else if (kind != T_EYE && kind != T_MOUTH && kind != T_MAKEUP)
                c[1] = c[2] = c[3] = c[0];
            for (k = 0; k < 4; k++) rgba |= (u16)(lroundf(clampf(c[k], 0, 1) * 15.0f) << (12 - 4 * k));
            snx_st_le16(out + at + 8 + 2 * px, rgba);
        }
    return end;
}

u32 snx_mii_cfl_write(SnxMiiMeshSource source, void* ctx, u8* out, u32 room) {
    SnxMiiMesh* m = (SnxMiiMesh*)malloc(sizeof *m);
    u32 at = 4 + 4 * SNX_MII_CFL_SECTIONS, s;
    if (!m) {
        fprintf(stderr, "mii parts: no memory for a mesh (%zu bytes)\n", sizeof *m);
        return 0;
    }
    if (room < at) goto full;
    snx_st_le16(out, SNX_MII_CFL_SECTIONS);
    snx_st_le16(out + 2, 0x0509);
    for (s = 0; s < SNX_MII_CFL_SECTIONS; s++) {
        u32 n = cfl_items[s], head = at, base = at + 4 + 4 * (n + 1), i, most = 0;
        if (base > room) goto full;
        snx_st_le32(out + 4 + 4 * s, at);
        snx_st_le16(out + head, (u16)n);
        at = base;
        for (i = 0; i < n; i++) {
            u32 start = at, same = 0;
            if (s < SNX_MII_CFL_MESHES) {
                int r;
                memset(m, 0, sizeof *m);
                r = source(ctx, s, i, m);
                if (r < 0) goto stop;
                if (r && m->same) {
                    u32 j = m->same - 1, from, to;
                    from = j < i ? snx_ld_le32(out + head + 4 + 4 * j) : 0;
                    to = j + 1 < i ? snx_ld_le32(out + head + 8 + 4 * j) : start - base;
                    if (j >= i || from >> CFL_OFFSET || (to & ((1u << CFL_OFFSET) - 1)) == from) {
                        fprintf(stderr,
                                "mii parts: %s %u is said to be the same as item %u, which is not an earlier "
                                "one with a mesh of its own\n",
                                cfl_names[s], i, j);
                        goto stop;
                    }
                    same = m->same;
                } else if (r) {
                    if (!cfl_mesh_fits(s, i, m)) goto stop;
                    if (!(at = cfl_mesh(s, m, out, at, room))) goto full;
                }
            } else if (!(at = cfl_tex(s, i, out, at, room, &same)))
                goto full;
            if (at - base >= 1u << CFL_OFFSET) {
                fprintf(stderr, "mii parts: section %u passes 4 MiB, the most its offsets reach\n", s);
                goto stop;
            }
            snx_st_le32(out + head + 4 + 4 * i, (start - base) | same << CFL_OFFSET);
            if (at - start > most) most = at - start;
        }
        if (most > 0xffff) {
            fprintf(stderr, "mii parts: section %u has an item of %u bytes; its header holds 65535\n", s, most);
            goto stop;
        }
        snx_st_le32(out + head + 4 + 4 * n, at - base);
        snx_st_le16(out + head + 2, (u16)most);
    }
    free(m);
    return at;
full:
    fprintf(stderr, "mii parts: CFL_Res.dat takes more than the %u bytes given\n", room);
stop:
    free(m);
    return 0;
}

/* a RomFS name's hash, as the 3DS's: its UTF-16 code units (ASCII here) over the parent's offset */
static u32 cfl_romfs_hash(const char* name, u32 parent) {
    u32 h = parent ^ 123456789u;
    for (; *name; name++) h = ((h >> 5) | (h << 27)) ^ (u8)*name;
    return h;
}
u32 snx_mii_cfl_romfs(u8* out, u32 room) {
    static const char name[] = "CFL_Res.dat";
    const u32 len = 2 * (sizeof name - 1), dirhash = 0x28, dirmeta = dirhash + 12, filehash = dirmeta + 0x18,
              filemeta = filehash + 12, meta = 0x20 + len + (len % 4 ? 2 : 0), data = (filemeta + meta + 15) & ~15u;
    u32 size, i;
    if (room < data) {
        fprintf(stderr, "mii parts: the RomFS takes more than the %u bytes given\n", room);
        return 0;
    }
    size = snx_mii_cfl_write(snx_mii_ctr_mesh, NULL, out + data, room - data);
    if (!size) return 0;
    if (((size + 15) & ~15u) > room - data) {
        fprintf(stderr, "mii parts: the RomFS takes more than the %u bytes given\n", room);
        return 0;
    }
    memset(out, 0, data);
    memset(out + data + size, 0, ((size + 15) & ~15u) - size);
    snx_st_le32(out, 0x28);
    snx_st_le32(out + 4, dirhash), snx_st_le32(out + 8, 12);
    snx_st_le32(out + 12, dirmeta), snx_st_le32(out + 16, 0x18);
    snx_st_le32(out + 20, filehash), snx_st_le32(out + 24, 12);
    snx_st_le32(out + 28, filemeta), snx_st_le32(out + 32, meta);
    snx_st_le32(out + 36, data);
    for (i = 0; i < 3; i++) { /* three slots each: the root's and its file's */
        snx_st_le32(out + dirhash + 4 * i, 0xffffffffu);
        snx_st_le32(out + filehash + 4 * i, 0xffffffffu);
    }
    snx_st_le32(out + dirhash + 4 * (cfl_romfs_hash("", 0) % 3), 0);
    snx_st_le32(out + filehash + 4 * (cfl_romfs_hash(name, 0) % 3), 0);
    /* the root: no parent, sibling or folder, its file the first, no other in its slot, no name */
    snx_st_le32(out + dirmeta + 4, 0xffffffffu);
    snx_st_le32(out + dirmeta + 8, 0xffffffffu);
    snx_st_le32(out + dirmeta + 16, 0xffffffffu);
    /* the file: in the root, no sibling, its data at the start, its size, no other in its slot, its name */
    snx_st_le32(out + filemeta + 4, 0xffffffffu);
    snx_st_le64(out + filemeta + 16, size);
    snx_st_le32(out + filemeta + 24, 0xffffffffu);
    snx_st_le32(out + filemeta + 28, len);
    for (i = 0; i < sizeof name - 1; i++) snx_st_le16(out + filemeta + 32 + 2 * i, (u8)name[i]);
    return data + ((size + 15) & ~15u);
}
