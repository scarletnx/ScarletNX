/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS's Mii parts file our model writes (src/common/utilities/mii_model.c's CFL_Res.dat) read back as the face
   library's layout says, with the checks citra_system_archives' extractor makes and more: the RomFS around it, the
   20 sections in order with no gap, each section's item count, item table and largest item, an empty item for an
   absent part and only for it, an item the same as an earlier one pointing at a part with data of its own; every mesh's
   counts, its 1/256ths and 1/8192ths, its byte indices inside its vertices, its triangles facing their normals, a
   faceline's and a hair's places; every texture's size and format. Then the file is the same bytes twice, and a mesh
   source the file cannot take (too many vertices, an index past them, a place past 128, a part pointing ahead) stops
   it, while a cube as the faceline and a head of a mask alone (no faceline, hair or nose) are written. */
#include "common/utilities/mii_model.h"
#include "common/utilities/endian.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOM 0x200000u
/* the test's state: its failures, two files' room */
static struct {
    int fails;
    u8 a[ROOM], b[ROOM];
    u32 bad; /* how bad() breaks faceline 0 */
} g_mii_cfl;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c);                                      \
            if (++g_mii_cfl.fails > 20) exit(1);                                                                       \
        }                                                                                                              \
    } while (0)

/* the face library's sections: their items (citra_system_archives' mii.py, item_counts) */
static const u32 want_items[20] = {4, 264, 12, 264, 1, 264, 12, 18, 18, 132, 62, 24, 3, 12, 12, 9, 2, 37, 6, 18};
static const u8 want_hats[14] = {9, 26, 34, 46, 57, 61, 77, 78, 84, 97, 109, 114, 126, 130};
static const u8 want_caps[4] = {9, 84, 97, 130};
static int in(const u8* list, u32 n, u32 v) {
    u32 i;
    for (i = 0; i < n; i++)
        if (list[i] == v) return 1;
    return 0;
}
/* whether our model has section s's item i (the Switch's part list, which the 3DS's mirrors) */
static int want_present(u32 s, u32 i) {
    u32 style = i % 132, cap = i >= 132;
    switch (s) {
        case 0: return i != 0;
        case 1: return cap ? in(want_caps, 4, style) : in(want_hats, 14, style);
        case 3: {
            static const u8 none[10] = {13, 33, 34, 37, 100, 107, 112, 115, 119, 120};
            static const u8 none_cap[12] = {13, 33, 37, 79, 91, 100, 107, 112, 115, 119, 120, 123};
            return cap ? !in(none_cap, 12, style) : !in(none, 10, style);
        }
        case 5: return style != 30;
        case 8: return i != 0 && i != 2;
        case 9: return in(want_hats, 14, i);
        case 11: return i != 23;
        case 13:
        case 14:
        case 15:
        case 18: return i != 0;
        case 16: return i == 1;
        default: return 1;
    }
}
static float s16at(const u8* p) { return (float)(i16)snx_ld_le16(p); }
static float f32at(const u8* p) {
    union {
        u32 u;
        float f;
    } x;
    x.u = snx_ld_le32(p);
    return x.f;
}

/* one mesh item of size bytes at p: its counts, attributes and triangles; 0 when it does not read whole */
static int check_mesh(u32 s, u32 item, const u8* p, u32 size) {
    u32 anchors = s == 2 ? 9 : s == 5 ? 18 : 0, at = 4 * anchors, c, n, t, il, j, k, per, facing = 0, solid = 0;
    if (size < at + 8) return 0;
    for (k = 0; k < anchors; k++) CHECK(isfinite(f32at(p + 4 * k)));
    if (s == 2) CHECK(f32at(p + 16) > 0 && f32at(p + 20) > 0); /* the nose above the chin, in front of the face */
    c = snx_ld_le16(p + at), n = snx_ld_le16(p + at + 2), t = snx_ld_le16(p + at + 4), il = snx_ld_le16(p + at + 6);
    CHECK(c >= 3 && c <= 256 && n == c && (t == c || t == 0) && il == 1);
    CHECK((t != 0) == (s != 5)); /* everything has texture coordinates but the hair */
    per = 12 + (t ? 4 : 0);
    at += 8;
    if (size < at + per * c + 4) return 0;
    for (j = 0; j < c; j++) { /* normals of length 1 */
        const u8* v = p + at + per * j;
        float len = sqrtf(s16at(v + 6) * s16at(v + 6) + s16at(v + 8) * s16at(v + 8) + s16at(v + 10) * s16at(v + 10));
        CHECK(fabsf(len - 256.0f) < 4.0f);
    }
    {
        const u8* idx = p + at + per * c + 4;
        u32 count = snx_ld_le16(p + at + per * c + 2), tail;
        CHECK(snx_ld_le16(p + at + per * c) == 4 && count && count % 3 == 0);
        tail = at + per * c + 4 + count + (s == 6 ? 2 * (count / 3) : 0);
        if (tail > size || size - tail >= 4 || size % 4) return 0; /* only the padding after it */
        for (j = 0; j < count; j++) CHECK(idx[j] < c);
        for (j = 0; j + 2 < count; j += 3) { /* counterclockwise from the side the normals face */
            const u8 *a = p + at + per * idx[j], *b = p + at + per * idx[j + 1], *d = p + at + per * idx[j + 2];
            float e1[3], e2[3], cr[3], nsum = 0;
            for (k = 0; k < 3; k++)
                e1[k] = s16at(b + 2 * k) - s16at(a + 2 * k), e2[k] = s16at(d + 2 * k) - s16at(a + 2 * k);
            cr[0] = e1[1] * e2[2] - e1[2] * e2[1], cr[1] = e1[2] * e2[0] - e1[0] * e2[2],
            cr[2] = e1[0] * e2[1] - e1[1] * e2[0];
            if (sqrtf(cr[0] * cr[0] + cr[1] * cr[1] + cr[2] * cr[2]) < 256.0f * 256.0f * 0.01f) continue;
            for (k = 0; k < 3; k++)
                nsum += cr[k] * (s16at(a + 6 + 2 * k) + s16at(b + 6 + 2 * k) + s16at(d + 6 + 2 * k));
            solid++;
            facing += nsum > 0;
        }
        if (s == 6)
            for (j = 0; j < count / 3; j++) CHECK(idx[count + 2 * j] == 0x0f && idx[count + 2 * j + 1] == 0x0f);
        if (solid * 50 > facing * 51)
            fprintf(stderr, "section %u item %u: %u of %u triangles face their normals\n", s, item, facing, solid);
        CHECK(solid * 50 <= facing * 51); /* 98% or more: a few at a pole or a tail's tip lean */
    }
    return 1;
}
/* one texture item */
static int check_tex(const u8* p, u32 size) {
    u32 w = snx_ld_le16(p), h = snx_ld_le16(p + 2), x, y, alpha = 0;
    if (size < 8) return 0;
    CHECK(w >= 8 && w <= 64 && !(w & (w - 1)) && h >= 8 && h <= 64 && !(h & (h - 1)));
    CHECK(p[4] == 1 && p[5] == 10 && p[6] <= 2 && p[7] == 0);
    if (size != 8 + 2 * w * h) return 0;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) alpha += snx_ld_le16(p + 8 + 2 * (y * w + x)) & 15u;
    CHECK(alpha > 0); /* every drawing covers something */
    return 1;
}

/* the RomFS around it: one file, CFL_Res.dat, in the root; its bytes and size */
static const u8* romfs_file(const u8* r, u32 n, u32* size) {
    static const char name[] = "CFL_Res.dat";
    u32 meta = snx_ld_le32(r + 28), data = snx_ld_le32(r + 36), h = 0 ^ 123456789u, i;
    const char* q;
    CHECK(snx_ld_le32(r) == 0x28 && snx_ld_le32(r + 4) == 0x28 && snx_ld_le32(r + 8) == 12 && data % 16 == 0);
    for (q = name; *q; q++) h = ((h >> 5) | (h << 27)) ^ (u8)*q;
    CHECK(snx_ld_le32(r + snx_ld_le32(r + 20) + 4 * (h % 3)) == 0); /* the name's slot holds the file */
    CHECK(snx_ld_le32(r + meta + 28) == 22);
    for (i = 0; i < 11; i++) CHECK(snx_ld_le16(r + meta + 32 + 2 * i) == (u8)name[i]);
    *size = (u32)snx_ld_le64(r + meta + 16);
    CHECK(snx_ld_le64(r + meta + 8) == 0 && data + *size <= n && n == data + ((*size + 15) & ~15u));
    return r + data;
}
/* the file: every section and item; the items it counts present, and the mesh and texture items whole */
static void check_file(const u8* f, u32 size, int ours) {
    u32 s, at = 84;
    CHECK(snx_ld_le16(f) == 20 && snx_ld_le16(f + 2) == 0x0509);
    for (s = 0; s < 20; s++) {
        u32 n = snx_ld_le16(f + at), most = snx_ld_le16(f + at + 2), base = at + 4 + 4 * (n + 1), i, big = 0;
        CHECK(snx_ld_le32(f + 4 + 4 * s) == at && n == want_items[s]); /* no gap before a section */
        for (i = 0; i < n; i++) {
            u32 e = snx_ld_le32(f + at + 4 + 4 * i), from = e & 0x3fffffu,
                to = snx_ld_le32(f + at + 8 + 4 * i) & 0x3fffffu, same = e >> 22, item = to - from;
            CHECK(to >= from && base + to <= size);
            if (item > big) big = item;
            if (same) { /* the same as an earlier item with data of its own */
                u32 o = snx_ld_le32(f + at + 4 + 4 * (same - 1));
                CHECK(!item && same - 1 < i && !(o >> 22) &&
                      (snx_ld_le32(f + at + 8 + 4 * (same - 1)) & 0x3fffffu) > (o & 0x3fffffu));
            }
            if (ours) CHECK(want_present(s, i) == (item || same));
            if (item && s < 9 && !check_mesh(s, i, f + base + from, item)) {
                fprintf(stderr, "section %u item %u: a mesh of %u bytes does not read whole\n", s, i, item);
                CHECK(0);
            }
            if (item && s >= 9) CHECK(check_tex(f + base + from, item));
        }
        CHECK(big == most);
        at = base + (snx_ld_le32(f + at + 4 + 4 * n) & 0x3fffffu);
    }
    CHECK(at == size);
}

/* ---- sources the file is written from in place of ours */

static int cube(SnxMiiMesh* m) { /* a cube 40 across, the chin at 0: each face its own four vertices */
    static const float n[6][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}};
    static const u16 quad[6] = {0, 1, 2, 0, 2, 3};
    u32 f, k, i;
    for (f = 0; f < 6; f++) {
        /* b up the face (z on the top and bottom), a = b x n: a x b = n, so the corners below go round
           counterclockwise seen from outside */
        const float* z = n[f];
        float b[3] = {0, z[1] != 0 ? 0.0f : 1.0f, z[1] != 0 ? 1.0f : 0.0f}, a[3];
        a[0] = b[1] * z[2] - b[2] * z[1], a[1] = b[2] * z[0] - b[0] * z[2], a[2] = b[0] * z[1] - b[1] * z[0];
        for (k = 0; k < 4; k++) {
            float sa = k == 1 || k == 2 ? 1.0f : -1.0f, sb = k >= 2 ? 1.0f : -1.0f;
            u32 v = 4 * f + k;
            for (i = 0; i < 3; i++) {
                m->pos[v][i] = 20.0f * (z[i] + sa * a[i] + sb * b[i]);
                m->nrm[v][i] = z[i];
            }
            m->pos[v][1] += 20.0f;
            m->tex[v][0] = 0.5f + 0.5f * sa, m->tex[v][1] = 0.5f - 0.5f * sb;
        }
        for (k = 0; k < 6; k++) m->idx[6 * f + k] = (u16)(4 * f + quad[k]);
    }
    m->vertices = 24, m->indices = 36, m->uv = 1;
    m->anchor[1] = 20.0f, m->anchor[4] = 18.0f, m->anchor[5] = 20.5f; /* the hair over it, the nose on its front */
    return 1;
}
static int tofu(void* ctx, u32 s, u32 i, SnxMiiMesh* m) { /* our parts, a cube as every faceline */
    return s == SNX_MII_CFL_FACELINE ? cube(m) : snx_mii_ctr_mesh(ctx, s, i, m);
}
static int eyes_only(void* ctx, u32 s, u32 i, SnxMiiMesh* m) { /* the mask alone: no faceline, hair or nose */
    return s == SNX_MII_CFL_MASK ? snx_mii_ctr_mesh(ctx, s, i, m) : 0;
}
static int bad(void* ctx, u32 s, u32 i,
               SnxMiiMesh* m) { /* our parts but the faceline 0, broken as g_mii_cfl.bad says */
    int r = snx_mii_ctr_mesh(ctx, s, i, m);
    if (s != SNX_MII_CFL_FACELINE || i) return r;
    switch (g_mii_cfl.bad) {
        case 0: m->vertices = 257; break;
        case 1: m->idx[0] = (u16)m->vertices; break;
        case 2: m->pos[0][1] = 130.0f; break;
        case 3: m->same = 2; break;
        case 4: m->indices = 2; break;
        default: break;
    }
    return r;
}

int main(void) {
    u32 n, m, size, k;
    const u8* f;
    n = snx_mii_cfl_romfs(g_mii_cfl.a, ROOM);
    CHECK(n > 0);
    f = romfs_file(g_mii_cfl.a, n, &size);
    check_file(f, size, 1);
    m = snx_mii_cfl_romfs(g_mii_cfl.b, ROOM);
    CHECK(m == n && !memcmp(g_mii_cfl.a, g_mii_cfl.b, n)); /* the same bytes every time */
    CHECK(!snx_mii_cfl_romfs(g_mii_cfl.b, n - 16));        /* no room: nothing */
    printf("our CFL_Res.dat: %u bytes, the RomFS %u\n", size, n);

    m = snx_mii_cfl_write(tofu, NULL, g_mii_cfl.b, ROOM); /* a cube for a head */
    CHECK(m > 0);
    if (m) check_file(g_mii_cfl.b, m, 0);
    m = snx_mii_cfl_write(eyes_only, NULL, g_mii_cfl.b, ROOM); /* eyes and nothing else */
    CHECK(m > 0);
    if (m) check_file(g_mii_cfl.b, m, 0);
    for (k = 0; k < 5; k++) { /* what the file cannot hold stops it, saying why */
        g_mii_cfl.bad = k;
        CHECK(!snx_mii_cfl_write(bad, NULL, g_mii_cfl.b, ROOM));
    }
    if (g_mii_cfl.fails) {
        fprintf(stderr, "%d checks failed\n", g_mii_cfl.fails);
        return 1;
    }
    printf("mii_cfl: ok\n");
    return 0;
}
