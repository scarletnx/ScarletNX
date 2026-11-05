/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Ounce control section (version 0xc9 at 0x04, formats 7 and 11): the fields of its program's stage and of a
   mesh or task program, from the bytes of the section (0xd8 of them). One game's sections show them:
   - 0x56 (halfword): the program's NVN stage. 6 is a task program and 7 a mesh program, in the order of the
     mesh programs' code areas, their sizes and the header after a mesh program's code (docs/OUNCE.md).
   - 0x58 (halfword): the invocations of the workgroup. 0x5a (halfword): the task payload's bytes, in whole 128-byte
     lines. A mesh program gives the payload of its task program, or 0x80 when it has none.
   - 0x5e (byte): 1 when a task program launches the mesh program. 0x5f (byte): the vertices of a primitive (3).
   - 0x60 and 0x62 (halfwords): the mesh program's most vertices and primitives.
   - 0x64: the offset in the code area of the header of the per-primitive outputs (a geometry SPH with passthrough
     on), 0 for none. 0x68: that header's bytes (0x80).
   The SPH cannot tell these stages: a task program, and a mesh program without one, have the header of a vertex
   program, and a mesh program after a task program has the header of a tessellation evaluation program, as Mesa's
   NAK writes them (nak/sph.rs). */
#ifndef SNX_NVN_SECTION_H
#define SNX_NVN_SECTION_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/endian.h"

#define NVN_SECTION_BYTES 0xd8u

/* the program's NVN stage (NVN_SHADER_STAGE_*) */
static inline u32 nvn_section_stage(const u8* s) { return snx_ld_le16(s + 0x56); }

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
#endif
