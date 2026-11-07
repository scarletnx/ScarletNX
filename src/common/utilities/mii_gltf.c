/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Mii parts from a glTF 2.0 model (mii_gltf.h), read with cgltf */
#include "common/utilities/mii_gltf.h"
#include <cgltf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SnxMiiGltf {
    cgltf_data* data;
    char name[96]; /* the model's file, as the log names it */
};

static const char* const gltf_kinds[SNX_MII_CFL_MESHES] = {"beard", "hat",  "faceline", "forehead", "glass",
                                                           "hair",  "mask", "noseline", "nose"};
static const u32 gltf_items[SNX_MII_CFL_MESHES] = {4, 264, 12, 264, 1, 264, 12, 18, 18};
static const char* const gltf_results[] = {
    "read",      "too short",  "of an unknown format", "not JSON", "not glTF", "given bad options",
    "not found", "unreadable", "out of memory",        "glTF 1.0"};

static void part_name(u32 section, u32 item, char out[32]) {
    u32 cap = gltf_items[section] == 264 && item >= 132;
    snprintf(out, 32, "%s%s.%u", gltf_kinds[section], cap ? "_cap" : "", cap ? item - 132 : item);
}
static cgltf_node* node_named(const cgltf_data* d, const char* name) {
    cgltf_size i;
    for (i = 0; i < d->nodes_count; i++)
        if (d->nodes[i].name && !strcmp(d->nodes[i].name, name)) return &d->nodes[i];
    return NULL;
}
static cgltf_node* child_named(const cgltf_node* n, const char* name) {
    cgltf_size i;
    for (i = 0; i < n->children_count; i++)
        if (n->children[i]->name && !strcmp(n->children[i]->name, name)) return n->children[i];
    return NULL;
}

static SnxMiiGltf* gltf_take(cgltf_options* o, cgltf_data* d, cgltf_result r, const char* path, const char* name) {
    SnxMiiGltf* g;
    if (r == cgltf_result_success) r = cgltf_load_buffers(o, d, path);
    if (r == cgltf_result_success) r = cgltf_validate(d);
    if (r != cgltf_result_success) {
        fprintf(stderr, "mii parts: %s is not a glTF 2.0 model it can read: %s (cgltf result %d)\n", name,
                (unsigned)r < sizeof gltf_results / sizeof *gltf_results ? gltf_results[r] : "unknown", (int)r);
        cgltf_free(d);
        return NULL;
    }
    g = (SnxMiiGltf*)calloc(1, sizeof *g);
    if (!g) {
        fprintf(stderr, "mii parts: no memory to hold %s\n", name);
        cgltf_free(d);
        return NULL;
    }
    g->data = d;
    snprintf(g->name, sizeof g->name, "%s", name);
    return g;
}
SnxMiiGltf* snx_mii_gltf_open(const char* path) {
    cgltf_options o = {0};
    cgltf_data* d = NULL;
    cgltf_result r = cgltf_parse_file(&o, path, &d);
    return gltf_take(&o, d, r, path, path);
}
SnxMiiGltf* snx_mii_gltf_open_memory(const void* bytes, size_t size) {
    cgltf_options o = {0};
    cgltf_data* d = NULL;
    cgltf_result r = cgltf_parse(&o, bytes, size, &d);
    return gltf_take(&o, d, r, NULL, "the model in memory");
}
void snx_mii_gltf_close(SnxMiiGltf* g) {
    if (!g) return;
    cgltf_free(g->data);
    free(g);
}

/* a node's world matrix (column major) and its 3x3's inverse transpose for normals; the 3x3's determinant */
static float node_world(const cgltf_node* n, float w[16], float nm[9]) {
    float a, b, c, d, e, f, gg, h, i, det;
    cgltf_node_transform_world(n, w);
    a = w[0], b = w[4], c = w[8], d = w[1], e = w[5], f = w[9], gg = w[2], h = w[6], i = w[10];
    det = a * (e * i - f * h) - b * (d * i - f * gg) + c * (d * h - e * gg);
    if (det != 0) { /* the inverse's transpose, rows as the matrix's */
        nm[0] = (e * i - f * h) / det, nm[1] = -(d * i - f * gg) / det, nm[2] = (d * h - e * gg) / det;
        nm[3] = -(b * i - c * h) / det, nm[4] = (a * i - c * gg) / det, nm[5] = -(a * h - b * gg) / det;
        nm[6] = (b * f - c * e) / det, nm[7] = -(a * f - c * d) / det, nm[8] = (a * e - b * d) / det;
    }
    return det;
}
/* a child's place (its world translation) and, with rot, its rotation as angles about x, y and z (applied x first) */
static void child_place(const cgltf_node* n, float place[3], float* rot) {
    float w[16], cx, cy, cz;
    cgltf_node_transform_world(n, w);
    place[0] = w[12], place[1] = w[13], place[2] = w[14];
    if (!rot) return;
    cx = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]), cy = sqrtf(w[4] * w[4] + w[5] * w[5] + w[6] * w[6]),
    cz = sqrtf(w[8] * w[8] + w[9] * w[9] + w[10] * w[10]);
    if (cx == 0 || cy == 0 || cz == 0) {
        rot[0] = rot[1] = rot[2] = 0;
        return;
    }
    /* R = Rz Ry Rx: R[2][1] = cy sx, R[2][2] = cy cx, R[2][0] = -sy, R[1][0] = cy sz, R[0][0] = cy cz */
    rot[0] = atan2f(w[6] / cy, w[10] / cz);
    rot[1] = asinf(fmaxf(-1.0f, fminf(1.0f, -w[2] / cx)));
    rot[2] = atan2f(w[1] / cx, w[0] / cx);
}
/* a part's places from its children: 0 with the reason said when one is missing */
static int part_anchors(const SnxMiiGltf* g, const char* name, u32 section, const cgltf_node* n, float anchor[18]) {
    static const char *const face[3] = {"hair", "nose", "beard"}, *const hat[3] = {"hat_front", "hat_side", "hat_top"};
    u32 k;
    if (section != SNX_MII_CFL_FACELINE && section != SNX_MII_CFL_HAIR) return 1;
    for (k = 0; k < 3; k++) {
        const char* place = section == SNX_MII_CFL_FACELINE ? face[k] : hat[k];
        char want[48];
        const cgltf_node* c;
        snprintf(want, sizeof want, "%s.%s", name, place);
        if (!(c = child_named(n, want))) {
            fprintf(stderr, "mii parts: %s's %s has no child node %s, the %s's place on it\n", g->name, name, want,
                    section == SNX_MII_CFL_FACELINE ? place : "hat");
            return 0;
        }
        if (section == SNX_MII_CFL_FACELINE)
            child_place(c, anchor + 3 * k, NULL);
        else
            child_place(c, anchor + 6 * k + 3, anchor + 6 * k);
    }
    return 1;
}

int snx_mii_gltf_mesh(void* ctx, u32 section, u32 item, SnxMiiMesh* m) {
    const SnxMiiGltf* g = (const SnxMiiGltf*)ctx;
    const cgltf_accessor *pos = NULL, *nrm = NULL, *uv = NULL;
    const cgltf_primitive* p;
    const cgltf_node* n;
    char name[32];
    float w[16], nm[9], det;
    cgltf_size i, count, indices;
    u32 j, k;
    if (section >= SNX_MII_CFL_MESHES || item >= gltf_items[section]) {
        fprintf(stderr, "mii parts: no mesh section %u item %u\n", section, item);
        return -1;
    }
    part_name(section, item, name);
    if (!(n = node_named(g->data, name))) return 0;
    if (!n->mesh || !n->mesh->primitives_count) {
        fprintf(stderr, "mii parts: %s's node %s has no mesh\n", g->name, name);
        return -1;
    }
    p = &n->mesh->primitives[0];
    for (i = 0; i < p->attributes_count; i++) {
        const cgltf_attribute* a = &p->attributes[i];
        if (a->type == cgltf_attribute_type_position) pos = a->data;
        if (a->type == cgltf_attribute_type_normal) nrm = a->data;
        if (a->type == cgltf_attribute_type_texcoord && a->index == 0) uv = a->data;
    }
    count = pos ? pos->count : 0;
    indices = p->indices ? p->indices->count : count;
    if (p->type != cgltf_primitive_type_triangles || !pos || !nrm || (section != SNX_MII_CFL_HAIR && !uv)) {
        fprintf(stderr,
                "mii parts: %s's %s draws %s with%s positions, with%s normals and with%s texture coordinates: a part "
                "is triangles with positions and normals%s\n",
                g->name, name, p->type == cgltf_primitive_type_triangles ? "triangles" : "no triangles",
                pos ? "" : "out", nrm ? "" : "out", uv ? "" : "out",
                section == SNX_MII_CFL_HAIR ? "" : ", and texture coordinates");
        return -1;
    }
    if (!count || count > SNX_MII_MESH_VERTICES || !indices || indices % 3 || indices > SNX_MII_MESH_INDICES) {
        fprintf(stderr,
                "mii parts: %s's %s has %zu vertices and %zu indices; a part takes 1 to %d vertices and a whole "
                "number of triangles, at most %d indices\n",
                g->name, name, (size_t)count, (size_t)indices, SNX_MII_MESH_VERTICES, SNX_MII_MESH_INDICES);
        return -1;
    }
    det = node_world(n, w, nm);
    if (det == 0) {
        fprintf(stderr, "mii parts: %s's %s is scaled flat (its node's scale has a 0)\n", g->name, name);
        return -1;
    }
    for (i = 0; i < count; i++) {
        float v[3], q[3], len;
        cgltf_accessor_read_float(pos, i, v, 3);
        for (k = 0; k < 3; k++) m->pos[i][k] = w[k] * v[0] + w[4 + k] * v[1] + w[8 + k] * v[2] + w[12 + k];
        cgltf_accessor_read_float(nrm, i, v, 3);
        for (k = 0; k < 3; k++) q[k] = nm[3 * k] * v[0] + nm[3 * k + 1] * v[1] + nm[3 * k + 2] * v[2];
        len = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
        if (!(len > 0)) {
            fprintf(stderr, "mii parts: %s's %s has a normal of length 0 at vertex %zu\n", g->name, name, (size_t)i);
            return -1;
        }
        for (k = 0; k < 3; k++) m->nrm[i][k] = q[k] / len;
        if (uv) cgltf_accessor_read_float(uv, i, m->tex[i], 2);
    }
    for (i = 0; i < indices; i++) {
        cgltf_size v = p->indices ? cgltf_accessor_read_index(p->indices, i) : i;
        if (v >= count) {
            fprintf(stderr, "mii parts: %s's %s's index %zu is %zu, past its %zu vertices\n", g->name, name, (size_t)i,
                    (size_t)v, (size_t)count);
            return -1;
        }
        m->idx[i] = (u16)v;
    }
    if (det < 0) /* a mirroring node turns its triangles: put them back counterclockwise */
        for (i = 0; i < indices; i += 3) {
            u16 t = m->idx[i + 1];
            m->idx[i + 1] = m->idx[i + 2];
            m->idx[i + 2] = t;
        }
    m->vertices = (u32)count, m->indices = (u32)indices, m->uv = uv != NULL;
    if (!part_anchors(g, name, section, n, m->anchor)) return -1;
    for (j = 0; j < item; j++) { /* an earlier part with the same mesh, place and children places: this one is it */
        char other[32];
        const cgltf_node* o;
        float ow[16], onm[9], oa[18] = {0};
        part_name(section, j, other);
        o = node_named(g->data, other);
        if (!o || o->mesh != n->mesh) continue;
        node_world(o, ow, onm);
        if (memcmp(ow, w, sizeof w) || !part_anchors(g, other, section, o, oa) || memcmp(oa, m->anchor, sizeof oa))
            continue;
        m->same = j + 1;
        break;
    }
    return 1;
}
