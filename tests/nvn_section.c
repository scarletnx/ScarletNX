/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The fields of an Ounce control section that nvn_section.h reads: the stage, and a mesh or task program's own */
#include "nx/gpu/nvn/nvn.h"
#include "nx/gpu/nvn/nvn_section.h"
#undef NDEBUG /* the checks are the test */
#include <assert.h>

/* a section with the magic, the version and the format, and the words from 0x54 on */
static void section(u8* s, const u32* words, u32 n) {
    for (u32 i = 0; i < NVN_SECTION_BYTES; i++) s[i] = 0;
    snx_st_le32(s, 0x98761234u);
    snx_st_le32(s + 4, 0xc9);
    snx_st_le32(s + 8, 11);
    for (u32 i = 0; i < n; i++) snx_st_le32(s + 0x54 + 4 * i, words[i]);
}

int main(void) {
    u8 s[NVN_SECTION_BYTES];
    /* a mesh program after a task program: 8 invocations, a payload of 0x580 bytes, triangles, 32 vertices and 16
       primitives, and the header of its per-primitive outputs at 0xd00 of its code area */
    const u32 mesh[] = {0x70000, 0x5800008, 0x3010000, 0x100020, 0xd00, 0x80};
    section(s, mesh, 6);
    assert(nvn_section_stage(s) == NVN_SHADER_STAGE_MESH);
    NvnSectionMesh m = nvn_section_mesh(s);
    assert(m.threads == 8 && m.payload == 0x580);
    assert(m.has_task == 1 && m.primitive_vertices == 3);
    assert(m.max_vertices == 32 && m.max_primitives == 16);
    assert(m.gs_header == 0xd00 && m.gs_bytes == 0x80);
    /* the mesh program's fields of a program's record, which keeps the task program's */
    DsMeshKey k = {.task_threads = 32, .task_payload = 0x580};
    nvn_section_mesh_key(s, NVN_SHADER_STAGE_MESH, &k);
    assert(k.task_threads == 32 && k.task_payload == 0x580 && k.mesh_threads == 8 && k.mesh_payload == 0x580);
    assert(k.has_task == 1 && k.primitive_vertices == 3 && k.max_vertices == 32 && k.max_primitives == 16);
    assert(k.prim_sph == 0xd00);
    /* a per-primitive SPH of fewer bytes than the translator reads of it is none */
    const u32 short_sph[] = {0x70000, 0x5800008, 0x3010000, 0x100020, 0xd00, 0x40};
    section(s, short_sph, 6);
    nvn_section_mesh_key(s, NVN_SHADER_STAGE_MESH, &k);
    assert(k.prim_sph == 0);
    /* a mesh program alone: no per-primitive outputs */
    const u32 alone[] = {0x70000, 0x800020, 0x3000000, 0xc00100};
    section(s, alone, 4);
    m = nvn_section_mesh(s);
    assert(m.threads == 32 && m.payload == 0x80 && !m.has_task);
    assert(m.max_vertices == 256 && m.max_primitives == 192 && !m.gs_header && !m.gs_bytes);
    /* a task program: 32 invocations and its payload, nothing of the vertices */
    const u32 task[] = {0x60000, 0x1000020};
    section(s, task, 2);
    assert(nvn_section_stage(s) == NVN_SHADER_STAGE_TASK);
    m = nvn_section_mesh(s);
    assert(m.threads == 32 && m.payload == 0x100 && !m.max_vertices && !m.max_primitives);
    /* the task program's fields of a record, which keeps the mesh program's */
    nvn_section_mesh_key(s, NVN_SHADER_STAGE_TASK, &k);
    assert(k.task_threads == 32 && k.task_payload == 0x100 && k.mesh_threads == 8 && k.max_vertices == 32);
    /* the stages of the other programs, as the sections number them */
    const u32 fragment[] = {0x10000};
    section(s, fragment, 1);
    assert(nvn_section_stage(s) == NVN_SHADER_STAGE_FRAGMENT);
    const u32 compute[] = {0x50000, 64, 1, 1};
    section(s, compute, 4);
    assert(nvn_section_stage(s) == NVN_SHADER_STAGE_COMPUTE);
    return 0;
}
