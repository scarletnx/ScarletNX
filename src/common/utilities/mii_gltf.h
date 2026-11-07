/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Mii parts from a glTF 2.0 model (mii_gltf.c, through cgltf): a SnxMiiMeshSource that a parts file
   (snx_mii_cfl_write) takes its meshes from. A part is the node named for it: "<kind>.<number>", the kinds beard, hat,
   faceline, forehead, glass, hair, mask, noseline and nose, a hat, forehead or hair under a cap "<kind>_cap.<style>"
   (hair.112, hair_cap.9, faceline.0, glass.0). Its mesh's first primitive, triangles with positions and normals (and
   texture coordinates for every kind but hair), is taken in the node's world space, one model unit a Mii unit (the
   chin at 0, the face toward +z). A faceline's places are its child nodes named for it and the place, hair, nose and
   beard (faceline.0.nose); a hair style's hat_front, hat_side and hat_top (hair.112.hat_top). Nodes that share a mesh
   and places are one part in the file. A kind's part with no node is absent; anything else a part cannot be stops the
   file, the log saying what it has and what it needs */
#ifndef SNX_MII_GLTF_H
#define SNX_MII_GLTF_H
#include "common/utilities/mii_model.h"
#include <stddef.h>

typedef struct SnxMiiGltf SnxMiiGltf;
/* a .glb or .gltf (with its buffers beside it or in it), NULL with the reason in the log */
SnxMiiGltf* snx_mii_gltf_open(const char* path);
/* one held in memory, a .glb or a .gltf whose buffers are in it */
SnxMiiGltf* snx_mii_gltf_open_memory(const void* bytes, size_t size);
void snx_mii_gltf_close(SnxMiiGltf* gltf);
/* the SnxMiiMeshSource: ctx an SnxMiiGltf */
int snx_mii_gltf_mesh(void* ctx, u32 section, u32 item, SnxMiiMesh* mesh);

#endif
