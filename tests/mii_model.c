/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The generated MiiModel archive (src/common/utilities/mii_model.c, included for its drawing functions) against the
   layout the SDK reads: each file's header and part tables, every part of the console's part list present (and none
   of its absent ones), each part's zlib stream and the data in it. A texture's size is the block linear layout's for
   its format, size and levels (the largest of each kind sizes the SDK's pool), and every level, taken out of the
   layout and decoded by tests/support's ASTC and BCn decoders, is the drawing it was made from; a shape's attributes,
   triangles and box agree, its normals face out of the head, and the mask maps the SDK's composed texture where the
   face is. */
#include "common/utilities/mii_model.c"
#include "nx/gpu/texture/blocklinear.h"
#include "common/utilities/endian.h"
#include "support/astc.h"
#include "support/bcn.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the test's state: its failures, the buffers a part is inflated, laid out again and decoded in, and the six
   files */
static struct {
    int fails;
    u8 raw[0x40000];
    u8 lin[0x40000];
    u8 px[256 * 512 * 8];
    u8 files[6][0x300000];
} g_mii_model;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                      \
            if (++g_mii_model.fails > 20) exit(1);                                                                     \
        }                                                                                                              \
    } while (0)

/* ---- the console's part list, as the SDK indexes it */

static const char* const want_tex_names[11] = {"hat",   "eye",  "eyebrow", "beard",    "wrinkle", "makeup",
                                               "glass", "mole", "mouth",   "mustache", "noseline"};
static const u32 want_tex_counts[11] = {132, 62, 24, 2, 12, 12, 20, 2, 37, 6, 18};
static const u32 want_shape_counts[12] = {4, 12, 12, 132, 132, 132, 132, 132, 132, 1, 18, 18};
static const u8 want_hats[14] = {9, 26, 34, 46, 57, 61, 77, 78, 84, 97, 109, 114, 126, 130};

static int in(const u8* list, u32 n, u32 v) {
    u32 i;
    for (i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}
static int want_tex_present(u32 k, u32 i) {
    if (k == 0) return in(want_hats, 14, i);
    if (k == 2) return i != 23;
    if (k == 4 || k == 5 || k == 6 || k == 9) return i != 0;
    if (k == 7) return i == 1;
    return 1;
}
static int want_shape_present(u32 k, u32 i) {
    static const u8 caps[4] = {9, 84, 97, 130};
    static const u8 no_fh[10] = {13, 33, 34, 37, 100, 107, 112, 115, 119, 120};
    static const u8 no_fh_cap[12] = {13, 33, 37, 79, 91, 100, 107, 112, 115, 119, 120, 123};
    switch (k) {
        case 0: return i != 0;
        case 3: return in(want_hats, 14, i);
        case 4: return in(caps, 4, i);
        case 5: return !in(no_fh, 10, i);
        case 6: return !in(no_fh_cap, 12, i);
        case 7:
        case 8: return i != 30;
        case 10: return i != 0 && i != 2;
        default: return 1;
    }
}
/* a texture kind's format (3 BC4, 4 BC5, 6 ASTC 4x4) and size in a Low (mid 0) or Mid file */
static void want_tex(u32 k, u32 i, int mid, u32* fmt, u32* w, u32* h, u32* mips) {
    static const u16 t[11][5] = {{3, 8, 8, 8, 8},         {6, 76, 64, 152, 128},  {3, 144, 128, 144, 128},
                                 {3, 256, 512, 256, 512}, {3, 96, 192, 256, 512}, {6, 96, 192, 256, 512},
                                 {4, 128, 128, 256, 256}, {3, 32, 32, 128, 128},  {6, 88, 64, 176, 128},
                                 {3, 64, 128, 128, 256},  {3, 96, 96, 128, 128}};
    u32 big;
    *fmt = t[k][0];
    *w = mid ? t[k][3] : t[k][1];
    *h = mid ? t[k][4] : t[k][2];
    if (k == 0 && i == 34) {
        *w = mid ? 128 : 64;
        *h = mid ? 256 : 128;
    }
    if (k == 0 && i == 57) {
        *w = 64;
        *h = 16;
    }
    for (*mips = 1, big = *w > *h ? *w : *h; big > 1; big >>= 1) (*mips)++;
    if (k == 6 || k == 10) *mips = 1;
}

/* ---- zlib's stored blocks, the only kind the archive writes: the n bytes inflated into out, 0 when anything
   differs */
static u32 want_adler(const u8* p, u32 n) {
    u32 a = 1, s = 0, i;
    for (i = 0; i < n; i++) {
        a = (a + p[i]) % 65521u;
        s = (s + a) % 65521u;
    }
    return s << 16 | a;
}
static int want_inflate(const u8* z, u32 stored, u8* out, u32 n) {
    u32 at = 2, got = 0;
    if (stored < 6 || z[0] != 0x78 || (z[0] * 256u + z[1]) % 31) return 0;
    for (;;) {
        u32 len;
        int last;
        if (at + 5 > stored || (z[at] & 6)) return 0; /* BTYPE 0 */
        last = z[at] & 1;
        len = snx_ld_le16(z + at + 1);
        if ((u16)~len != snx_ld_le16(z + at + 3) || at + 5 + len > stored || got + len > n) return 0;
        memcpy(out + got, z + at + 5, len);
        got += len;
        at += 5 + len;
        if (last) break;
    }
    return got == n && at + 4 == stored && snx_ld_be32(z + at) == want_adler(out, n);
}

/* ---- the block linear layout */
static u32 want_bh(u32 rows0, u32 rows) {
    u32 bh = 0;
    while (bh < 4 && (8u << bh) < rows0) bh++;
    while (bh > 0 && rows <= (4u << bh)) bh--;
    return bh;
}
static u32 want_bpb(u32 fmt) { return fmt == 3 ? 8 : 16; }
static u32 want_level_bytes(u32 fmt, u32 w, u32 h, u32 l) {
    u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, bx = (lw + 3) / 4, by = (lh + 3) / 4;
    u32 bh = want_bh((h + 3) / 4, by), wb = bx * want_bpb(fmt);
    return ((wb + 63) / 64 * 64) * ((by + (8u << bh) - 1) / (8u << bh) * (8u << bh));
}
/* level l's blocks out of the layout (the level at at in src), row after row */
static void want_deswizzle(const u8* src, u32 fmt, u32 w, u32 h, u32 l, u8* dst) {
    u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, bpb = want_bpb(fmt), bx = (lw + 3) / 4, by = (lh + 3) / 4;
    u32 bh = want_bh((h + 3) / 4, by), wb = bx * bpb, gobs = (wb + 63) / 64, x, y;
    for (y = 0; y < by; y++)
        for (x = 0; x < wb; x++) {
            u32 off = (((y >> (3 + bh)) * gobs + (x >> 6)) << (9 + bh)) + (((y >> 3) & ((1u << bh) - 1)) << 9) +
                      gob_x_offset(x & 63) + gob_y_offset(y & 7);
            dst[y * wb + x] = src[off];
        }
}
/* every level of a texture part decoded and set against what tex_texel draws at its texels: the mean difference of
   its channels (ASTC's four, BC5's two, BC4's one) within the encoders' error. A level in the wrong layout, a block
   the decoder reads otherwise, or a channel left out, is far from it */
static void check_drawing(u32 k, u32 v, u32 fmt, u32 w, u32 h, u32 mips, int mid) {
    u32 l, at = 0, nch = fmt == 6 ? 4 : fmt == 4 ? 2 : 1;
    float ar = (float)w / (float)h;
    for (l = 0; l < mips; l++) {
        u32 lw = w >> l ? w >> l : 1, lh = h >> l ? h >> l : 1, bw = (lw + 3) / 4 * 4, x, y, c;
        float aa = 1.4f / (float)lh;
        double sum = 0, mean, limit;
        want_deswizzle(g_mii_model.raw + at, fmt, w, h, l, g_mii_model.lin);
        if (fmt == 6)
            astc_decode_image(g_mii_model.lin, bw, (lh + 3) / 4 * 4, 4, 4, g_mii_model.px, 0);
        else
            bcn_decode_image(g_mii_model.lin, bw, (lh + 3) / 4 * 4, fmt == 3 ? BCN_BC4 : BCN_BC5, g_mii_model.px);
        for (y = 0; y < lh; y++)
            for (x = 0; x < lw; x++) {
                float want[4];
                u32 r = y * bw + x;
                tex_texel(k, v, ((float)x + 0.5f) / (float)lw * ar, ((float)y + 0.5f) / (float)lh, aa, ar, want);
                for (c = 0; c < nch; c++) { /* RGBA8, or R16 and R16G16 (their high bytes) */
                    int got = fmt == 6 ? g_mii_model.px[4 * r + c] : g_mii_model.px[(fmt == 4 ? 4 : 2) * r + 2 * c + 1];
                    sum += fabs((double)got - (double)lroundf(want[c] * 255.0f));
                }
            }
        mean = sum / ((double)lw * lh * nch);
        /* two endpoints a block blur a small level's edges more: the worst seen is 2.2, 4.4, 9.2, 12.9 and 14.4 a
           level of 4096, 1024, 256, 64 texels and fewer */
        limit = lw * lh >= 4096 ? 4.0 : lw * lh >= 1024 ? 8.0 : lw * lh >= 256 ? 14.0 : 24.0;
        if (mean > limit)
            fprintf(stderr, "%s %s drawing %u level %u (%ux%u): decoded %.1f off the drawing, %.0f at most\n",
                    mid ? "Mid" : "Low", want_tex_names[k], v, l, lw, lh, mean, limit);
        CHECK(mean <= limit);
        at += want_level_bytes(fmt, w, h, l);
    }
}

static float want_half(u16 h) {
    u32 e = (h >> 10) & 31, m = h & 1023;
    float v = e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24);
    return h & 0x8000 ? -v : v;
}
static float want_snorm10(u32 v) {
    i32 s = (i32)(v & 0x3ff);
    if (s >= 512) s -= 1024;
    return (float)s / 511.0f;
}
static float f32at(const u8* p) {
    u32 b = snx_ld_le32(p);
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static void check_textures(const u8* f, u32 size, int mid) {
    u32 k, i, e = 0, largest[11] = {0}, drawn[11] = {0}, mouth_color = 0;
    CHECK(!memcmp(f, "NFTR", 4) && snx_ld_le32(f + 4) == 1 && snx_ld_le32(f + 8) == size);
    for (k = 0; k < 11; k++)
        for (i = 0; i < want_tex_counts[k]; i++, e++) {
            const u8* t = f + 0x64 + 0x1c * e;
            u32 off = snx_ld_le32(t), raw = snx_ld_le32(t + 4), stored = snx_ld_le32(t + 8), fmt, w, h, mips, l, n = 0;
            if (!want_tex_present(k, i)) {
                CHECK(raw == 0 && stored == 0);
                continue;
            }
            want_tex(k, i, mid, &fmt, &w, &h, &mips);
            CHECK(snx_ld_le16(t + 0x14) == w && snx_ld_le16(t + 0x16) == h && t[0x18] == fmt && t[0x19] == mips);
            CHECK(snx_ld_le32(t + 0xc) == 0x806 && snx_ld_le32(t + 0x10) == 0x200);
            for (l = 0; l < mips; l++) n += want_level_bytes(fmt, w, h, l);
            CHECK(raw == n && raw <= sizeof g_mii_model.raw);
            CHECK(off >= 0x64 + 0x1c * 327 && off + stored <= size);
            if (raw != n || raw > sizeof g_mii_model.raw || off + stored > size) continue;
            CHECK(want_inflate(f + off, stored, g_mii_model.raw, raw));
            if (raw > largest[k]) largest[k] = raw;
            /* each drawing once: its levels are what it draws, and its coverage (A, G or R) not blank */
            {
                u32 v = tex_variant(k, i), p, sum = 0;
                if (v >= 32 || (drawn[k] >> v & 1)) continue;
                drawn[k] |= 1u << v;
                check_drawing(k, v, fmt, w, h, mips, mid);
                want_deswizzle(g_mii_model.raw, fmt, w, h, 0, g_mii_model.lin);
                if (fmt == 6)
                    astc_decode_image(g_mii_model.lin, (w + 3) / 4 * 4, (h + 3) / 4 * 4, 4, 4, g_mii_model.px, 0);
                else
                    bcn_decode_image(g_mii_model.lin, (w + 3) / 4 * 4, (h + 3) / 4 * 4, fmt == 3 ? BCN_BC4 : BCN_BC5,
                                     g_mii_model.px);
                for (p = 0; p < ((w + 3) / 4 * 4) * h; p++)
                    sum += fmt == 6   ? g_mii_model.px[4 * p + 3]
                           : fmt == 4 ? g_mii_model.px[4 * p + 3]
                                      : g_mii_model.px[2 * p + 1];
                if (!sum) fprintf(stderr, "%s %s %u: level 0 blank\n", mid ? "Mid" : "Low", want_tex_names[k], i);
                CHECK(sum > 0);
                if (k == 1) { /* an eye: its white (G) and its iris (B), its coverage right of the middle */
                    u32 bw = (w + 3) / 4 * 4, g = 0, b = 0, cx = 0, cn = 0;
                    for (p = 0; p < bw * h; p++) {
                        g += g_mii_model.px[4 * p + 1] > 128;
                        b += g_mii_model.px[4 * p + 2] > 128;
                        if (g_mii_model.px[4 * p + 3] > 128) {
                            cx += p % bw;
                            cn++;
                        }
                    }
                    CHECK(g > 0 && b > 0 && cn > 0 && cx / cn > w / 2);
                }
                if (k == 8) /* a mouth: its lips (R) or teeth (G) in some, its outline (A) in all */
                    for (p = 0; p < ((w + 3) / 4 * 4) * h; p++)
                        mouth_color += g_mii_model.px[4 * p] + g_mii_model.px[4 * p + 1];
            }
        }
    CHECK(e == 327);
    CHECK(mouth_color > 0);
    for (k = 0; k < 11; k++) /* each kind's largest part and its alignment, which size the SDK's pool */
        CHECK(snx_ld_le32(f + 0xc + 4 * k) == largest[k] && largest[k] && snx_ld_le32(f + 0x38 + 4 * k) == 0x200);
    if (mid) { /* the console's largest Mid parts, which the same layout gives */
        static const u32 console[11] = {0x6000,  0x7600, 0x4200, 0x16000, 0x16000, 0x2b400,
                                        0x10000, 0x3400, 0x8200, 0x6000,  0x2000};
        for (k = 0; k < 11; k++) CHECK(largest[k] == console[k]);
    }
}

static void check_shapes(const u8* f, u32 size) {
    u32 k, i, e = 0, largest[12] = {0}, tris = 0;
    float ty[12];
    CHECK(!memcmp(f, "NFSR", 4) && snx_ld_le32(f + 4) == 1 && snx_ld_le32(f + 8) == size);
    /* the faceline transforms: the hair's rise, then the nose on the face's front */
    for (i = 0; i < 12; i++) {
        const u8* tf = f + 0x6c + 0x58 * 857 + 0x48 * 132 + 0x24 * i;
        ty[i] = f32at(tf + 4);
        CHECK(f32at(tf) == 0 && fabsf(ty[i]) < 5 && f32at(tf + 16) > 15 && f32at(tf + 16) < 35 && f32at(tf + 20) > 15 &&
              f32at(tf + 20) < 30);
    }
    for (i = 0; i < 132 * 18; i++) CHECK(isfinite(f32at(f + 0x6c + 0x58 * 857 + 4 * i)));
    for (k = 0; k < 12; k++)
        for (i = 0; i < want_shape_counts[k]; i++, e++) {
            const u8* t = f + 0x6c + 0x58 * e;
            u32 off = snx_ld_le32(t), raw = snx_ld_le32(t + 4), stored = snx_ld_le32(t + 8), a, nv, at = 0, n,
                inward = 0;
            u32 aoff[5], asz[5], ioff = snx_ld_le32(t + 0x38), isz = snx_ld_le32(t + 0x3c);
            int hair = k == 7 || k == 8;
            float box[6], lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
            if (!want_shape_present(k, i)) {
                CHECK(raw == 0 && stored == 0);
                continue;
            }
            for (a = 0; a < 5; a++) {
                aoff[a] = snx_ld_le32(t + 0x10 + 4 * a);
                asz[a] = snx_ld_le32(t + 0x24 + 4 * a);
            }
            for (a = 0; a < 6; a++) box[a] = f32at(t + 0x40 + 4 * a);
            CHECK(snx_ld_le32(t + 0xc) == 0x806 && off >= 0x14dd4 && off + stored <= size &&
                  raw <= sizeof g_mii_model.raw);
            if (off + stored > size || raw > sizeof g_mii_model.raw) continue;
            CHECK(want_inflate(f + off, stored, g_mii_model.raw, raw));
            if (raw > largest[k]) largest[k] = raw;
            /* position (four halves), normal (10:10:10:2), then a UV (two halves) or, for hair, a tangent and a color
               (four bytes each), one after the other; the triangle list on 4 bytes past them */
            nv = asz[0] / 8;
            CHECK(nv > 2 && aoff[0] == 0 && asz[1] == 4 * nv && asz[2] == (hair ? 0 : 4 * nv) &&
                  asz[3] == (hair ? 4 * nv : 0) && asz[4] == (hair ? 4 * nv : 0));
            for (a = 0; a < 5; a++)
                if (asz[a]) {
                    CHECK(aoff[a] == at);
                    at = aoff[a] + asz[a];
                }
            CHECK(ioff == ((at + 3) & ~3u) && isz && isz % 6 == 0 && raw == ioff + isz);
            if (raw != ioff + isz || isz % 6) continue;
            for (n = 0; n < nv; n++) {
                const u8* p = g_mii_model.raw + 8 * n;
                u32 nw = snx_ld_le32(g_mii_model.raw + aoff[1] + 4 * n), c;
                float pos[3], nx = want_snorm10(nw), ny = want_snorm10(nw >> 10), nz = want_snorm10(nw >> 20);
                for (c = 0; c < 3; c++) {
                    pos[c] = want_half(snx_ld_le16(p + 2 * c));
                    if (pos[c] < lo[c]) lo[c] = pos[c];
                    if (pos[c] > hi[c]) hi[c] = pos[c];
                }
                CHECK(want_half(snx_ld_le16(p + 6)) == 1.0f && nw >> 30 == 1);
                /* the parts around the head face out of it, from its middle (0, 30, 2) */
                if (k <= 1 || k == 3 || k == 4 || k == 7 || k == 8)
                    inward += nx * pos[0] + ny * (pos[1] - 30) + nz * (pos[2] - 2) <= 0;
                CHECK(fabsf(sqrtf(nx * nx + ny * ny + nz * nz) - 1) < 0.02f);
                if (k == 2) { /* the mask: the SDK's composed texture 100 units across, its top 80.35 + the rise */
                    float u = want_half(snx_ld_le16(g_mii_model.raw + aoff[2] + 4 * n)),
                          v = want_half(snx_ld_le16(g_mii_model.raw + aoff[2] + 4 * n + 2));
                    CHECK(fabsf(u - (0.5f + pos[0] / 100)) < 0.0012f);
                    CHECK(fabsf(v - (0.8035f + ty[i] / 100 - pos[1] / 100)) < 0.0012f);
                }
                if (hair) { /* a tangent across the normal, its sign 1 */
                    const i8* tg = (const i8*)(g_mii_model.raw + aoff[3] + 4 * n);
                    float d = ((float)tg[0] * nx + (float)tg[1] * ny + (float)tg[2] * nz) / 127.0f;
                    CHECK(fabsf(d) < 0.08f && tg[3] == 127);
                }
            }
            /* hair with tails (7, 8) is part tubes, whose sides toward the head face it; a hat part (3, 4) that is no
               cap (34 and 57 are) is a tie, band, bow or clip, its facing checked by its triangles alone */
            if (k == 7 || k == 8)
                CHECK(inward * 10 <= nv * 3);
            else if ((k != 3 && k != 4) || i == 34 || i == 57)
                CHECK(inward * 20 <= nv);
            for (a = 0; a < 3; a++) /* the box holds the vertices, to the halves' precision */
                CHECK(fabsf(box[a] - lo[a]) < 0.05f && fabsf(box[3 + a] - hi[a]) < 0.05f);
            if (k == 1) /* a faceline in the console's frame: chin at 0, its front near +25 */
                CHECK(lo[1] > -2 && lo[1] < 2 && hi[2] > 20 && hi[2] < 28 && hi[0] < 30 && lo[2] > -25);
            if (k ==
                7) { /* the tail styles' tails: long twin tails (126) below the chin on both sides, a side tail (84)
                             on one side only, the head's other hair styles no lower than the nape */
                u32 left = 0, right = 0;
                for (n = 0; n < nv; n++) {
                    float x = want_half(snx_ld_le16(g_mii_model.raw + 8 * n)),
                          y = want_half(snx_ld_le16(g_mii_model.raw + 8 * n + 2));
                    left += x < -26 && y < 30, right += x > 26 && y < 30;
                }
                if (i == 126) CHECK(lo[1] < 0 && left > 20 && right > 20);
                if (i == 84) CHECK((left > 20) != (right > 20));
                if (i == 0 || i == 33) CHECK(left == 0 && right == 0);
            }
            if (k == 3) { /* a tie style's hat part is two ties at the sides, no cap; a cap style's covers the crown */
                if (i == 126) CHECK(lo[0] < -24 && hi[0] > 24 && hi[1] < 62);
                if (i == 34 || i == 57) CHECK(hi[1] > 65);
            }
            /* every index a vertex; every triangle counterclockwise from where its normals face, or flat */
            for (n = 0; n < isz / 2; n += 3) {
                u16 ix[3];
                float q[3][3], nrm[3] = {0, 0, 0}, ab[3], ac[3], cr[3], area, dot;
                u32 c, v;
                for (v = 0; v < 3; v++) {
                    u32 nw;
                    ix[v] = snx_ld_le16(g_mii_model.raw + ioff + 2 * (n + v));
                    CHECK(ix[v] < nv);
                    if (ix[v] >= nv) return;
                    for (c = 0; c < 3; c++) q[v][c] = want_half(snx_ld_le16(g_mii_model.raw + 8 * ix[v] + 2 * c));
                    nw = snx_ld_le32(g_mii_model.raw + aoff[1] + 4 * ix[v]);
                    nrm[0] += want_snorm10(nw);
                    nrm[1] += want_snorm10(nw >> 10);
                    nrm[2] += want_snorm10(nw >> 20);
                }
                for (c = 0; c < 3; c++) {
                    ab[c] = q[1][c] - q[0][c];
                    ac[c] = q[2][c] - q[0][c];
                }
                cr[0] = ab[1] * ac[2] - ab[2] * ac[1];
                cr[1] = ab[2] * ac[0] - ab[0] * ac[2];
                cr[2] = ab[0] * ac[1] - ab[1] * ac[0];
                area = sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]);
                dot = cr[0] * nrm[0] + cr[1] * nrm[1] + cr[2] * nrm[2];
                if (area > 0.05f && !(dot > 0))
                    fprintf(stderr, "shape kind %u part %u: triangle %u faces in\n", k, i, n / 3);
                if (area > 0.05f) CHECK(dot > 0);
                tris++;
            }
        }
    CHECK(e == 857 && tris > 0);
    for (k = 0; k < 12; k++)
        CHECK(snx_ld_le32(f + 0xc + 4 * k) == largest[k] && largest[k] && snx_ld_le32(f + 0x3c + 4 * k) == 4);
}

/* a pixel of a picture side pixels wide at the model's (x, y): its RGB */
static const u8* pic_at(const u8* rgba, int side, float x, float y) {
    int px = (int)((x + 42.0f) / 84.0f * (float)side), py = (int)((76.0f - y) / 84.0f * (float)side);
    return rgba + 4 * (py * side + px);
}
/* count of the pixels in the box (x0, y0)-(x1, y1) of model units that pass is */
static int pic_count(const u8* rgba, int side, float x0, float y0, float x1, float y1, int (*is)(const u8*)) {
    int n = 0;
    float x, y;
    for (y = y0; y <= y1; y += 0.5f)
        for (x = x0; x <= x1; x += 0.5f) n += is(pic_at(rgba, side, x, y));
    return n;
}
static int is_red_eye(const u8* p) { return p[0] > 110 && p[1] < 60 && p[2] < 60; }
static int is_white(const u8* p) { return p[0] > 215 && p[1] > 215 && p[2] > 215; }
static int is_crimson(const u8* p) { return p[0] > 2 * p[1] && p[2] > p[1] && p[0] > 90; }
static int is_teal(const u8* p) { return p[1] > p[0] + 40 && p[2] > p[0] + 30; }
static const u8* g_pic_corner; /* is_corner's picture */
static int is_corner(const u8* p) {
    return abs(p[0] - g_pic_corner[0]) < 40 && abs(p[1] - g_pic_corner[1]) < 40 && abs(p[2] - g_pic_corner[2]) < 40;
}
/* the pictures (snx_mii_picture) of our own Miis: the background of the favorite color, the hair on top and in two
   tails at the sides, skin in the middle, the eyes in their color with their whites on the face */
static void check_picture(void) {
    static u8 rgba[128 * 128 * 4];
    u8 ci[SNX_MII_CHARINFO];
    SnxMiiLook look;
    const u8* p;
    snx_mii_db_charinfo(0, ci); /* Baguette: crimson twin tails, red eyes, red favorite */
    snx_mii_db_look(0, &look);
    CHECK(snx_mii_picture(ci, &look, 15, rgba) == -1 && snx_mii_picture(ci, &look, 1025, rgba) == -1);
    CHECK(snx_mii_picture(ci, &look, 128, rgba) == 0);
    p = rgba; /* a corner: the background, the favorite red lightened */
    CHECK(p[3] == 255 && p[0] > p[1] + 30 && p[0] > 200);
    CHECK(is_crimson(pic_at(rgba, 128, 0, 64)));                    /* the crown */
    p = pic_at(rgba, 128, 0, 28);                                   /* between the eyes and the mouth */
    CHECK(p[0] > 170 && p[1] > 130 && p[2] > 90 && !is_crimson(p)); /* skin */
    CHECK(pic_count(rgba, 128, -16, 30, -3, 38, is_red_eye) > 3);   /* an eye, red */
    CHECK(pic_count(rgba, 128, 3, 30, 16, 38, is_red_eye) > 3);     /* the other */
    CHECK(pic_count(rgba, 128, -16, 30, 16, 38, is_white) > 3);     /* their whites */
    CHECK(pic_count(rgba, 128, -40, 0, -27, 30, is_crimson) > 20);  /* a tail at each side */
    CHECK(pic_count(rgba, 128, 27, 0, 40, 30, is_crimson) > 20);
    CHECK(pic_count(rgba, 128, -12, 21, 12, 27, is_crimson) == 0); /* no hair over the cheeks, between eyes and mouth */
    snx_mii_db_charinfo(1, ci);                                    /* Leek Girl: teal */
    snx_mii_db_look(1, &look);
    CHECK(snx_mii_picture(ci, &look, 128, rgba) == 0);
    CHECK(is_teal(pic_at(rgba, 128, 0, 64)) && pic_count(rgba, 128, -40, 0, -27, 40, is_teal) > 20);
    snx_mii_db_charinfo(3, ci); /* Alex: short hair, no tails */
    snx_mii_db_look(3, &look);
    CHECK(snx_mii_picture(ci, &look, 128, rgba) == 0);
    g_pic_corner = rgba; /* nothing of it beside the cheeks: the background there, as in its corner */
    CHECK(pic_count(rgba, 128, -40, 10, -32, 24, is_corner) > 100 && !is_teal(pic_at(rgba, 128, 0, 64)));
}

int main(void) {
    u32 i, sizes[6];
    for (i = 0; i < SNX_MII_MODEL_FILES; i++) {
        char name[32];
        sizes[i] = snx_mii_model_write(i, NULL);
        CHECK(sizes[i] <= sizeof g_mii_model.files[i]);
        if (sizes[i] > sizeof g_mii_model.files[i]) return 1;
        CHECK(snx_mii_model_write(i, g_mii_model.files[i]) == sizes[i]);
        snx_mii_model_name(i, name);
        CHECK(!strcmp(name,
                      (const char* const[]){"NXTextureLowLinear.dat", "NXTextureLowSRGB.dat", "NXTextureMidLinear.dat",
                                            "NXTextureMidSRGB.dat", "ShapeMid.dat", "ShapeHigh.dat"}[i]));
        /* a copy names its source and has its bytes */
        if (snx_mii_model_same(i) != i) {
            u32 s = snx_mii_model_same(i);
            CHECK(s < i && sizes[s] == sizes[i] && !memcmp(g_mii_model.files[s], g_mii_model.files[i], sizes[i]));
        }
    }
    /* each SRGB copy is its Linear file, and the High shapes the Mid ones: the archive keeps them once */
    CHECK(snx_mii_model_same(0) == 0 && snx_mii_model_same(1) == 0 && snx_mii_model_same(2) == 2 &&
          snx_mii_model_same(3) == 2 && snx_mii_model_same(4) == 4 && snx_mii_model_same(5) == 4);
    check_textures(g_mii_model.files[0], sizes[0], 0);
    check_textures(g_mii_model.files[2], sizes[2], 1);
    check_shapes(g_mii_model.files[4], sizes[4]);
    check_picture();
    /* the six files with the RomFS's tables fit the room fs.h keeps for them */
    CHECK(sizes[0] + sizes[2] + sizes[4] + 0x1000 <= 0x500000);
    if (g_mii_model.fails) return 1;
    printf("PASS generated MiiModel: Low %u, Mid %u, shapes %u bytes\n", sizes[0], sizes[2], sizes[4]);
    return 0;
}
