/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce control section (version 0xc9 at 0x04, formats 7 and 11): the fields of its program's stage and of a
   mesh or task program, from the bytes of the section (0xd8 of them). One game's sections show them:
   - 0x56 (halfword): the program's NVN stage. 6 is a task program and 7 a mesh program, in the order of the
     mesh programs' code areas, their sizes and the header after a mesh program's code (docs/platforms/nx/ounce.md).
   - 0x58 (halfword): the invocations of the workgroup. 0x5a (halfword): the task payload's bytes, in whole 128-byte
     lines. A mesh program gives the payload of its task program, or 0x80 when it has none.
   - 0x5e (byte): 1 when a task program launches the mesh program. 0x5f (byte): the vertices of a primitive (3).
   - 0x60 and 0x62 (halfwords): the mesh program's most vertices and primitives.
   - 0x64: the offset of the header of the per-primitive outputs (a geometry SPH with passthrough on) from the
     start of the program's code area, its own header included: 0xd00 in a code area (0x44) of 0xe00 bytes, whose
     last 128 bytes are zero. 0 for none. 0x68: that header's bytes (0x80).
   The SPH cannot tell these stages: a task program, and a mesh program without one, have the header of a vertex
   program, and a mesh program after a task program has the header of a tessellation evaluation program, as Mesa's
   NAK writes them (nak/sph.rs).
   A compute program (stage 5) lays the same words out as its launch: 0x58, 0x5c and 0x60 the workgroup size, 0x64
   its shared memory's bytes, 0x68 and 0x6c its local memory's and 0x74 its barriers. They are the record the
   console's driver fills the QMD from, 0x1c into the section: SHARED_MEMORY_SIZE takes 0x64 rounded up to 256, and
   the SM's shared memory configuration (its 8, 16, 32, 64 or 100 KiB carve-outs) is chosen from that size and the
   block's registers and warps. The size is exact: one game's video upscaler keeps a 108-byte slot a thread from
   0x1e60 and declares 0x5460, 0x1e60 + 128 * 108 for its 32x4 block, and its programs that declare 0 have no
   shared memory instruction. */
#ifndef SNX_NVN_SECTION_H
#define SNX_NVN_SECTION_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/endian.h"
#include "nx/gpu/draw_state.h"
#include "nx/gpu/nvn/nvn.h"

#define NVN_SECTION_BYTES 0xd8u

/* the program's NVN stage (NVN_SHADER_STAGE_*) */
static inline u32 nvn_section_stage(const u8* s) { return snx_ld_le16(s + 0x56); }
/* a compute program's shared memory, in bytes: the word the console's driver puts in the QMD. 0 for a program with
   none */
static inline u32 nvn_section_shared(const u8* s) { return snx_ld_le32(s + 0x64); }

/* A mesh or task program's fields. Those of the vertices and primitives are 0 for a task program */
typedef struct {
    u16 threads, payload;
    u8 has_task, primitive_vertices;
    u16 max_vertices, max_primitives;
    u32 gs_header, gs_bytes;
} NvnSectionMesh;
static inline NvnSectionMesh nvn_section_mesh(const u8* s) {
    NvnSectionMesh m = {0};
    m.threads = snx_ld_le16(s + 0x58);
    m.payload = snx_ld_le16(s + 0x5a);
    m.has_task = s[0x5e];
    m.primitive_vertices = s[0x5f];
    m.max_vertices = snx_ld_le16(s + 0x60);
    m.max_primitives = snx_ld_le16(s + 0x62);
    m.gs_header = snx_ld_le32(s + 0x64);
    m.gs_bytes = snx_ld_le32(s + 0x68);
    return m;
}
/* A task or mesh program's fields (stage NVN_SHADER_STAGE_TASK or _MESH), from its section s, into k: the task
   program's or the mesh program's, the others kept, as a program keeps both in one record (nvn_program_mesh). The
   per-primitive SPH's offset only when the section gives the bytes the translator reads of it (ShMesh.prim_sph) */
static inline void nvn_section_mesh_key(const u8* s, u32 stage, DsMeshKey* k) {
    NvnSectionMesh m = nvn_section_mesh(s);
    if (stage == NVN_SHADER_STAGE_TASK) {
        k->task_threads = m.threads;
        k->task_payload = m.payload;
        return;
    }
    k->mesh_threads = m.threads;
    k->mesh_payload = m.payload;
    k->max_vertices = m.max_vertices;
    k->max_primitives = m.max_primitives;
    k->has_task = m.has_task;
    k->primitive_vertices = m.primitive_vertices;
    k->prim_sph = m.gs_bytes >= sizeof((ShMesh*)0)->prim_sph ? m.gs_header : 0;
}
#endif
