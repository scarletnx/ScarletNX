/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3DS's Mii parts file our model writes (src/common/utilities/mii_model.c's CFL_Res.dat) read back as the face
   library's layout says, with the checks citra_system_archives' extractor makes and more: the RomFS around it, the
   20 sections in order with no gap, each section's item count, item table and largest item, an empty item for an
   absent part and only for it, an item the same as an earlier one pointing at a part with data of its own; every mesh's
   counts, its 1/256ths and 1/8192ths, its byte indices inside its vertices, its triangles facing their normals, a
   faceline's and a hair's places; every texture's size and format. Then the file is the same bytes twice, and a mesh
   source the file cannot take (too many vertices, an index past them, a place past 128, a part pointing ahead) stops
   it, while a cube as the faceline and a head of a mask alone (no faceline, hair or nose) are written. A glTF model
   (src/common/utilities/mii_gltf.c) of our own parts, built here as the reader's conventions name them, gives back
   every mesh, place and shared part ours does, and a file from it reads whole; a cube model is a head; a model part
   with no normals, no texture coordinates where its kind needs them, points instead of triangles, an index past its
   vertices or a faceline without its nose's place stops the file. The Switch's shape files take the same sources: all
   ours is the file as it always is, a cube for faceline 0 and a mask alone are written, a hair over the console's
   largest is refused, and so is the glTF model of our 3DS parts (its beards are over the Switch's largest), whose
   facelines alone fit ShapeHigh.dat (not ShapeMid.dat) and are written as it gives them. */
#include "common/utilities/mii_gltf.h"
#include "common/utilities/mii_model.h"
#include "common/utilities/endian.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ROOM 0x200000u
/* a glTF model built here: its text and its one buffer */
typedef struct {
    char* json;
    u32 json_at, json_room;
    u8* bin;
    u32 bin_at, bin_room;
} Gltf;
/* the model's arrays as they are built, each a JSON list's text */
typedef struct {
    u32 views, accessors, meshes, nodes, roots;
    char views_js[1 << 16], accessors_js[1 << 18], meshes_js[1 << 16], nodes_js[1 << 20], roots_js[1 << 14];
    u32 at[5];
} Parts;
/* the test's state: its failures, two files' room, the model it builds, the meshes it compares */
static struct {
    int fails;
    u8 a[ROOM], b[ROOM];
    u32 bad;                /* how bad() breaks faceline 0 */
    const char* node_extra; /* more members for the next part's node: its own transform */
    Parts parts;
    Gltf gltf;
    u8 gltf_bin[0x800000];
    char gltf_json[0xc00000];
    SnxMiiMesh mesh, other;
    u32 mesh_of[9][264];
    float anchor_of[9][264][18];
    u8 file[2][0x300000];
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

/* ---- glTF models built here, as a .gltf whose one buffer is a base64 data URI */

static void js(Gltf* g, const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(g->json + g->json_at, g->json_room - g->json_at, fmt, ap);
    va_end(ap);
    CHECK(n >= 0 && (u32)n < g->json_room - g->json_at);
    if (n > 0 && (u32)n < g->json_room - g->json_at) g->json_at += (u32)n;
}
/* bytes into the buffer, 4-aligned: their offset */
static u32 bin(Gltf* g, const void* p, u32 n) {
    u32 at = g->bin_at;
    CHECK(at + n + 4 <= g->bin_room);
    memcpy(g->bin + at, p, n);
    g->bin_at = (at + n + 3) & ~3u;
    return at;
}
/* the quaternion (x, y, z, w) of rotations about x, then y, then z */
static void quat(const float e[3], float q[4]) {
    float cx = cosf(e[0] / 2), sx = sinf(e[0] / 2), cy = cosf(e[1] / 2), sy = sinf(e[1] / 2), cz = cosf(e[2] / 2),
          sz = sinf(e[2] / 2);
    q[0] = sx * cy * cz - cx * sy * sz, q[1] = cx * sy * cz + sx * cy * sz, q[2] = cx * cy * sz - sx * sy * cz;
    q[3] = cx * cy * cz + sx * sy * sz;
}
/* the model's text: accessors over views of the buffer, a mesh each, a node per part with its places as children */
static void appendf(char* b, u32* at, u32 room, const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(b + *at, room - *at, fmt, ap);
    va_end(ap);
    CHECK(n >= 0 && (u32)n < room - *at);
    if (n > 0 && (u32)n < room - *at) *at += (u32)n;
}
static u32 accessor(Gltf* g, const void* p, u32 count, u32 comps, int index) {
    u32 off = bin(g, p, count * (index ? 2 : 4 * comps));
    Parts* t = &g_mii_cfl.parts;
    appendf(t->views_js, &t->at[0], sizeof t->views_js, "%s{\"buffer\":0,\"byteOffset\":%u,\"byteLength\":%u}",
            t->views ? "," : "", off, count * (index ? 2 : 4 * comps));
    appendf(t->accessors_js, &t->at[1], sizeof t->accessors_js,
            "%s{\"bufferView\":%u,\"componentType\":%u,\"count\":%u,\"type\":\"%s\"}", t->accessors ? "," : "",
            t->views++, index ? 5123u : 5126u, count,
            index        ? "SCALAR"
            : comps == 3 ? "VEC3"
                         : "VEC2");
    return t->accessors++;
}
/* mesh m as a glTF mesh: its index. bad: 1 points instead of triangles, 2 no normals, 4 no texture coordinates */
static u32 gltf_mesh(Gltf* g, const SnxMiiMesh* m, u32 bad) {
    Parts* t = &g_mii_cfl.parts;
    u32 pos = accessor(g, m->pos, m->vertices, 3, 0), nrm = bad & 2 ? 0 : accessor(g, m->nrm, m->vertices, 3, 0);
    u32 tex = m->uv && !(bad & 4) ? accessor(g, m->tex, m->vertices, 2, 0) : 0;
    u32 idx = accessor(g, m->idx, m->indices, 1, 1);
    appendf(t->meshes_js, &t->at[2], sizeof t->meshes_js, "%s{\"primitives\":[{\"attributes\":{\"POSITION\":%u",
            t->meshes ? "," : "", pos);
    if (!(bad & 2)) appendf(t->meshes_js, &t->at[2], sizeof t->meshes_js, ",\"NORMAL\":%u", nrm);
    if (m->uv && !(bad & 4)) appendf(t->meshes_js, &t->at[2], sizeof t->meshes_js, ",\"TEXCOORD_0\":%u", tex);
    appendf(t->meshes_js, &t->at[2], sizeof t->meshes_js, "},\"indices\":%u,\"mode\":%u}]}", idx, bad & 1 ? 0u : 4u);
    return t->meshes++;
}
/* a node: its index; children its places (count 0, 3 of a faceline's, 3 of a hair's) */
static u32 gltf_node(const char* name, int mesh, const float* anchor, u32 section) {
    Parts* t = &g_mii_cfl.parts;
    static const char *const face[3] = {"hair", "nose", "beard"}, *const hat[3] = {"hat_front", "hat_side", "hat_top"};
    u32 self = t->nodes, k, kids = section == SNX_MII_CFL_FACELINE || section == SNX_MII_CFL_HAIR ? 3 : 0;
    t->nodes += 1 + kids;
    appendf(t->roots_js, &t->at[4], sizeof t->roots_js, "%s%u", t->roots++ ? "," : "", self);
    appendf(t->nodes_js, &t->at[3], sizeof t->nodes_js, "%s{\"name\":\"%s\"", self ? "," : "", name);
    if (mesh >= 0) appendf(t->nodes_js, &t->at[3], sizeof t->nodes_js, ",\"mesh\":%d", mesh);
    if (g_mii_cfl.node_extra) appendf(t->nodes_js, &t->at[3], sizeof t->nodes_js, "%s", g_mii_cfl.node_extra);
    if (kids)
        appendf(t->nodes_js, &t->at[3], sizeof t->nodes_js, ",\"children\":[%u,%u,%u]", self + 1, self + 2, self + 3);
    appendf(t->nodes_js, &t->at[3], sizeof t->nodes_js, "}");
    for (k = 0; k < kids; k++) {
        const float* place = section == SNX_MII_CFL_FACELINE ? anchor + 3 * k : anchor + 6 * k + 3;
        appendf(t->nodes_js, &t->at[3], sizeof t->nodes_js, ",{\"name\":\"%s.%s\",\"translation\":[%.9g,%.9g,%.9g]",
                name, section == SNX_MII_CFL_FACELINE ? face[k] : hat[k], (double)place[0], (double)place[1],
                (double)place[2]);
        if (section == SNX_MII_CFL_HAIR) {
            float q[4];
            quat(anchor + 6 * k, q);
            appendf(t->nodes_js, &t->at[3], sizeof t->nodes_js, ",\"rotation\":[%.9g,%.9g,%.9g,%.9g]", (double)q[0],
                    (double)q[1], (double)q[2], (double)q[3]);
        }
        appendf(t->nodes_js, &t->at[3], sizeof t->nodes_js, "}");
    }
    return self;
}
/* the whole text into g->json, the buffer as base64 */
static void gltf_finish(Gltf* g) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    Parts* t = &g_mii_cfl.parts;
    u32 i;
    js(g, "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[%s]}],\"nodes\":[%s],", t->roots_js,
       t->nodes_js);
    js(g, "\"meshes\":[%s],\"accessors\":[%s],\"bufferViews\":[%s],", t->meshes_js, t->accessors_js, t->views_js);
    js(g, "\"buffers\":[{\"byteLength\":%u,\"uri\":\"data:application/octet-stream;base64,", g->bin_at);
    for (i = 0; i < g->bin_at; i += 3) {
        u32 v = (u32)g->bin[i] << 16 | (i + 1 < g->bin_at ? (u32)g->bin[i + 1] << 8 : 0) |
                (i + 2 < g->bin_at ? g->bin[i + 2] : 0);
        js(g, "%c%c%c%c", b64[v >> 18], b64[(v >> 12) & 63], i + 1 < g->bin_at ? b64[(v >> 6) & 63] : '=',
           i + 2 < g->bin_at ? b64[v & 63] : '=');
    }
    js(g, "\"}]}");
}

static void gltf_reset(void) {
    memset(&g_mii_cfl.parts, 0, sizeof g_mii_cfl.parts);
    g_mii_cfl.gltf.json = g_mii_cfl.gltf_json, g_mii_cfl.gltf.json_room = sizeof g_mii_cfl.gltf_json,
    g_mii_cfl.gltf.json_at = 0;
    g_mii_cfl.gltf.bin = g_mii_cfl.gltf_bin, g_mii_cfl.gltf.bin_room = sizeof g_mii_cfl.gltf_bin,
    g_mii_cfl.gltf.bin_at = 0;
}
static void part_name(u32 s, u32 i, char name[32]) {
    static const char* const kinds[9] = {"beard", "hat",  "faceline", "forehead", "glass",
                                         "hair",  "mask", "noseline", "nose"};
    u32 cap = want_items[s] == 264 && i >= 132;
    snprintf(name, 32, "%s%s.%u", kinds[s], cap ? "_cap" : "", cap ? i - 132 : i);
}
/* our parts as a model: a node each, the ones the same as an earlier part on that part's mesh */
static void gltf_ours(void) {
    u32 s, i;
    gltf_reset();
    for (s = 0; s < 9; s++)
        for (i = 0; i < want_items[s]; i++) {
            char name[32];
            memset(&g_mii_cfl.mesh, 0, sizeof g_mii_cfl.mesh);
            if (snx_mii_ctr_mesh(NULL, s, i, &g_mii_cfl.mesh) <= 0) continue;
            part_name(s, i, name);
            if (g_mii_cfl.mesh.same) {
                g_mii_cfl.mesh_of[s][i] = g_mii_cfl.mesh_of[s][g_mii_cfl.mesh.same - 1];
                memcpy(g_mii_cfl.anchor_of[s][i], g_mii_cfl.anchor_of[s][g_mii_cfl.mesh.same - 1],
                       sizeof g_mii_cfl.anchor_of[s][i]);
            } else {
                g_mii_cfl.mesh_of[s][i] = gltf_mesh(&g_mii_cfl.gltf, &g_mii_cfl.mesh, 0);
                memcpy(g_mii_cfl.anchor_of[s][i], g_mii_cfl.mesh.anchor, sizeof g_mii_cfl.anchor_of[s][i]);
            }
            gltf_node(name, (int)g_mii_cfl.mesh_of[s][i], g_mii_cfl.anchor_of[s][i], s);
        }
    gltf_finish(&g_mii_cfl.gltf);
}
static int near(const float* a, const float* b, u32 n, float tol) {
    u32 i;
    for (i = 0; i < n; i++)
        if (fabsf(a[i] - b[i]) > tol) return 0;
    return 1;
}
/* a model of the cube as faceline 0, broken as bad says (gltf_mesh's), its places left out with no_places */
static SnxMiiGltf* gltf_cube(u32 bad, int no_places) {
    memset(&g_mii_cfl.mesh, 0, sizeof g_mii_cfl.mesh);
    cube(&g_mii_cfl.mesh);
    gltf_reset();
    gltf_node("faceline.0", (int)gltf_mesh(&g_mii_cfl.gltf, &g_mii_cfl.mesh, bad), g_mii_cfl.mesh.anchor,
              no_places ? SNX_MII_CFL_MASK : SNX_MII_CFL_FACELINE);
    gltf_finish(&g_mii_cfl.gltf);
    return snx_mii_gltf_open_memory(g_mii_cfl.gltf.json, g_mii_cfl.gltf.json_at);
}
/* the cube as faceline 0 in a node turned a quarter about y and stretched 2 along x (mirrored too with mirror): its
   positions and normals in the world, its triangles still counterclockwise */
static void check_transform(int mirror) {
    SnxMiiGltf* g;
    u32 k;
    g_mii_cfl.node_extra = mirror ? ",\"rotation\":[0,0.70710678,0,0.70710678],\"scale\":[-2,1,1]"
                                  : ",\"rotation\":[0,0.70710678,0,0.70710678],\"scale\":[2,1,1]";
    g = gltf_cube(0, 0);
    g_mii_cfl.node_extra = NULL;
    CHECK(g != NULL);
    if (!g) return;
    memset(&g_mii_cfl.other, 0, sizeof g_mii_cfl.other);
    CHECK(snx_mii_gltf_mesh(g, SNX_MII_CFL_FACELINE, 0, &g_mii_cfl.other) == 1);
    for (k = 0; k < 24; k++) { /* (x, y, z) -> (s z, y, -2 s x) with s -1 mirrored: the normal (s z, y, -x / 2 s) */
        const float *p = g_mii_cfl.mesh.pos[k], *n = g_mii_cfl.mesh.nrm[k], sx = mirror ? -1.0f : 1.0f;
        float want[3] = {p[2], p[1], -2.0f * sx * p[0]}, nw[3] = {n[2], n[1], -n[0] / (2.0f * sx)}, len;
        len = sqrtf(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
        nw[0] /= len, nw[1] /= len, nw[2] /= len;
        CHECK(near(g_mii_cfl.other.pos[k], want, 3, 1e-3f) && near(g_mii_cfl.other.nrm[k], nw, 3, 1e-4f));
    }
    for (k = 0; k + 2 < 36; k += 3) { /* each triangle faces its normal */
        const float *a = g_mii_cfl.other.pos[g_mii_cfl.other.idx[k]],
                    *b = g_mii_cfl.other.pos[g_mii_cfl.other.idx[k + 1]],
                    *c = g_mii_cfl.other.pos[g_mii_cfl.other.idx[k + 2]],
                    *n = g_mii_cfl.other.nrm[g_mii_cfl.other.idx[k]];
        float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        CHECK((e1[1] * e2[2] - e1[2] * e2[1]) * n[0] + (e1[2] * e2[0] - e1[0] * e2[2]) * n[1] +
                  (e1[0] * e2[1] - e1[1] * e2[0]) * n[2] >
              0);
    }
    snx_mii_gltf_close(g);
}
static void check_gltf(u32 ours_size) {
    SnxMiiGltf* g;
    u32 s, i, n, k;
    gltf_ours();
    g = snx_mii_gltf_open_memory(g_mii_cfl.gltf.json, g_mii_cfl.gltf.json_at);
    CHECK(g != NULL);
    if (!g) return;
    for (s = 0; s < 9; s++)
        for (i = 0; i < want_items[s]; i++) { /* every part as ours */
            int a, b;
            memset(&g_mii_cfl.mesh, 0, sizeof g_mii_cfl.mesh), memset(&g_mii_cfl.other, 0, sizeof g_mii_cfl.other);
            a = snx_mii_ctr_mesh(NULL, s, i, &g_mii_cfl.mesh), b = snx_mii_gltf_mesh(g, s, i, &g_mii_cfl.other);
            CHECK(a == b && g_mii_cfl.mesh.same == g_mii_cfl.other.same);
            if (a != 1 || g_mii_cfl.mesh.same) continue;
            CHECK(g_mii_cfl.mesh.vertices == g_mii_cfl.other.vertices &&
                  g_mii_cfl.mesh.indices == g_mii_cfl.other.indices && g_mii_cfl.mesh.uv == g_mii_cfl.other.uv);
            CHECK(!memcmp(g_mii_cfl.mesh.idx, g_mii_cfl.other.idx, 2 * g_mii_cfl.mesh.indices));
            for (k = 0; k < g_mii_cfl.mesh.vertices; k++) {
                CHECK(near(g_mii_cfl.mesh.pos[k], g_mii_cfl.other.pos[k], 3, 1e-5f) &&
                      near(g_mii_cfl.mesh.nrm[k], g_mii_cfl.other.nrm[k], 3, 1e-5f));
                if (g_mii_cfl.mesh.uv) CHECK(near(g_mii_cfl.mesh.tex[k], g_mii_cfl.other.tex[k], 2, 1e-6f));
            }
            CHECK(near(g_mii_cfl.mesh.anchor, g_mii_cfl.other.anchor, 18, 1e-4f));
        }
    n = snx_mii_cfl_write(snx_mii_gltf_mesh, g, g_mii_cfl.b, ROOM);
    CHECK(n == ours_size); /* the same items, the same sizes */
    if (n) check_file(g_mii_cfl.b, n, 1);
    snx_mii_gltf_close(g);
    printf("our parts as a glTF model: %u bytes of text, %u meshes, %u nodes\n", g_mii_cfl.gltf.json_at,
           g_mii_cfl.parts.meshes, g_mii_cfl.parts.nodes);

    g = gltf_cube(0, 0); /* the cube as a model, every other part absent: a head of it */
    CHECK(g != NULL);
    n = g ? snx_mii_cfl_write(snx_mii_gltf_mesh, g, g_mii_cfl.b, ROOM) : 0;
    CHECK(n > 0);
    if (n) check_file(g_mii_cfl.b, n, 0);
    snx_mii_gltf_close(g);
    for (k = 0; k < 4; k++) { /* points, no normals, no texture coordinates, no places: refused */
        g = gltf_cube(k < 3 ? 1u << k : 0, k == 3);
        CHECK(!g || !snx_mii_cfl_write(snx_mii_gltf_mesh, g, g_mii_cfl.b, ROOM));
        snx_mii_gltf_close(g);
    }
    CHECK(!snx_mii_gltf_open("/nonexistent/model.glb")); /* no file: said */
    check_transform(0);
    check_transform(1);
}

/* ---- the Switch's shape files from a source */

static int ours(void* ctx, u32 s, u32 i, SnxMiiMesh* m) {
    (void)ctx, (void)s, (void)i, (void)m;
    return SNX_MII_MESH_OURS;
}
static int tofu_switch(void* ctx, u32 s, u32 i, SnxMiiMesh* m) { /* a cube as faceline 0, ours the rest */
    (void)ctx;
    return s == SNX_MII_CFL_FACELINE && !i ? cube(m) : SNX_MII_MESH_OURS;
}
static int mask_only(void* ctx, u32 s, u32 i, SnxMiiMesh* m) {
    (void)ctx, (void)i, (void)m;
    return s == SNX_MII_CFL_MASK ? SNX_MII_MESH_OURS : 0;
}
static int big_hair(void* ctx, u32 s, u32 i, SnxMiiMesh* m) { /* hair 0 of 4000 vertices: over the Mid file's room */
    u32 k;
    (void)ctx;
    if (s != SNX_MII_CFL_HAIR || i) return SNX_MII_MESH_OURS;
    for (k = 0; k < 4000; k++) m->pos[k][0] = (float)(k % 64), m->pos[k][1] = (float)(k / 64), m->nrm[k][2] = 1;
    for (k = 0; k < 3 * 3900; k++) m->idx[k] = (u16)(k / 3 + k % 3);
    m->vertices = 4000, m->indices = 3 * 3900;
    return 1;
}
static int faceline_from_model(void* ctx, u32 s, u32 i, SnxMiiMesh* m) { /* the model's facelines, ours the rest */
    return s == SNX_MII_CFL_FACELINE ? snx_mii_gltf_mesh(ctx, s, i, m) : SNX_MII_MESH_OURS;
}
/* shape file f's entry for Switch kind kind's part i: its decompressed size, position bytes and index bytes */
static void entry(const u8* f, u32 kind, u32 i, u32* raw, u32* pos, u32* idx) {
    static const u32 counts[12] = {4, 12, 12, 132, 132, 132, 132, 132, 132, 1, 18, 18};
    u32 e = i, k;
    for (k = 0; k < kind; k++) e += counts[k];
    *raw = snx_ld_le32(f + 0x6c + 0x58 * e + 4), *pos = snx_ld_le32(f + 0x6c + 0x58 * e + 0x24);
    *idx = snx_ld_le32(f + 0x6c + 0x58 * e + 0x3c);
}
static void check_switch(void) {
    u32 i, n, raw, pos, idx;
    SnxMiiGltf* g;
    for (i = 4; i < 6; i++) { /* every part ours: the file as it always is */
        u32 a = snx_mii_model_write(i, g_mii_cfl.file[0]),
            b = snx_mii_model_write_from(i, ours, NULL, g_mii_cfl.file[1]);
        CHECK(a && a == b && !memcmp(g_mii_cfl.file[0], g_mii_cfl.file[1], a));
    }
    n = snx_mii_model_write_from(4, tofu_switch, NULL, g_mii_cfl.file[1]); /* the cube as the head, in ShapeMid.dat */
    CHECK(n > 0 && n <= 0xe517c);
    entry(g_mii_cfl.file[1], 1, 0, &raw, &pos, &idx);
    CHECK(pos == 24 * 8 && idx == 36 * 2 && raw >= pos + idx);
    entry(g_mii_cfl.file[1], 1, 1, &raw, &pos, &idx); /* the other facelines ours */
    CHECK(raw > 0 && pos > 24 * 8);
    memset(g_mii_cfl.file[1], 0, sizeof g_mii_cfl.file[1]);              /* the writer takes its out zeroed */
    n = snx_mii_model_write_from(4, mask_only, NULL, g_mii_cfl.file[1]); /* the eyes' sheet and nothing else */
    CHECK(n > 0);
    entry(g_mii_cfl.file[1], 1, 0, &raw, &pos, &idx);
    CHECK(!raw); /* the faceline absent */
    entry(g_mii_cfl.file[1], 2, 0, &raw, &pos, &idx);
    CHECK(raw > 0);
    CHECK(
        !snx_mii_model_write_from(4, big_hair, NULL, g_mii_cfl.file[1])); /* over the console's largest hair: refused */
    gltf_ours(); /* our parts through a glTF model, at the 3DS's detail: its beards are over the Switch's largest */
    g = snx_mii_gltf_open_memory(g_mii_cfl.gltf.json, g_mii_cfl.gltf.json_at);
    CHECK(g != NULL);
    if (!g) return;
    for (i = 4; i < 6; i++) CHECK(!snx_mii_model_write_from(i, snx_mii_gltf_mesh, g, g_mii_cfl.file[1]));
    /* its 3DS facelines are over the Switch's largest in ShapeMid.dat, within it in ShapeHigh.dat */
    CHECK(!snx_mii_model_write_from(4, faceline_from_model, g, g_mii_cfl.file[1]));
    memset(g_mii_cfl.file[1], 0, sizeof g_mii_cfl.file[1]);
    CHECK(snx_mii_model_write_from(5, faceline_from_model, g, g_mii_cfl.file[1]) > 0);
    memset(&g_mii_cfl.mesh, 0, sizeof g_mii_cfl.mesh);
    snx_mii_ctr_mesh(NULL, SNX_MII_CFL_FACELINE, 0, &g_mii_cfl.mesh);
    entry(g_mii_cfl.file[1], 1, 0, &raw, &pos, &idx); /* the model's faceline, as it gives it */
    CHECK(pos == 8 * g_mii_cfl.mesh.vertices && idx == 2 * g_mii_cfl.mesh.indices);
    snx_mii_gltf_close(g);
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
    check_gltf(size);
    check_switch();
    if (g_mii_cfl.fails) {
        fprintf(stderr, "%d checks failed\n", g_mii_cfl.fails);
        return 1;
    }
    printf("mii_cfl: ok\n");
    return 0;
}
