/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* MiiModel, system data 0100000000000802, generated here (mii_model.c): the Mii parts' shapes and textures a game's
   SDK (nn::mii::detail, as the game links it) builds a Mii's head from, drawn in code so that a game draws a Mii
   with a face, hair and a nose when the player's firmware is not in the game's data folder. Nothing of the console's
   own parts is in it: only their format and their list (which part kinds there are, how many of each, which are
   absent, each texture's size, format and mip levels, each shape kind's largest part).

   The archive is six files, each read whole by the SDK and indexed from its start without a check of its header:
   four of textures (NFTR: Low and Mid quality, each in a Linear and an SRGB copy, the same here) and two of shapes
   (NFSR: Mid and High, each part with as many quads as fit the console's largest of its kind there). A file is 0xc
   bytes of header (its magic, version 1, its size), then per part kind its largest decompressed part and that part's
   alignment (which size the SDK's pool), then a table of one entry per part, then (shapes only) the hair and faceline
   transforms, then each part's data as a zlib stream. A part whose decompressed size is 0 is absent. A game sizes its
   Mii heap for the console's files, which the SDK reads whole into it, so no file here is bigger than the console's
   of its name. */
#ifndef SNX_MII_MODEL_H
#define SNX_MII_MODEL_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/mii_db.h"

#define SNX_MII_MODEL_DATA_ID 0x0100000000000802ull
#define SNX_MII_MODEL_FILES   6

/* file i's name, as the SDK opens it */
void snx_mii_model_name(u32 i, char name[32]);
/* the file whose bytes file i has: an earlier one (a Linear and an SRGB copy are the same), or i */
u32 snx_mii_model_same(u32 i);
/* file i into out (zeroed, at least the bytes out NULL gives): its size, 0 when there was no memory to make it. With
   out NULL, the most bytes it takes: its parts are deflated as they are made, so only writing it tells its size */
u32 snx_mii_model_write(u32 i, u8* out);

/* A Mii's picture (a nn::mii::CharInfo) as the launcher and acc show a user's: a bust from the front, side x side
   pixels of RGBA8 (row after row, alpha 255) in look's colors, drawn on the CPU from this model's own parts: the head,
   hair (and its tails and ties) and nose rasterized, the hair with a band of shine across its strands, each part
   outlined where it is in front of another or of the background, the eyes, brows and mouth drawn on the face in 2D
   where the mask's texture puts them, over the favorite color's light background and a shirt of it. side 16 to 1024:
   0, else -1 with nothing drawn */
int snx_mii_picture(const u8 charinfo[SNX_MII_CHARINFO], const SnxMiiLook* look, int side, u8* rgba);

/* The 3DS's Mii parts, CFL_Res.dat, the one file of its Mii data archive (0004009B00010202) that a 3DS game's face
   library builds a Mii's head from: 20 sections, 9 of meshes (in the order of SNX_MII_CFL_*) and 11 of textures, each
   a table of its items (an item may be empty, or the same as an earlier one of its section) and the largest item's
   size, the layout citra_system_archives' mii.py writes. Positions and normals are 1/256ths and texture coordinates
   1/8192ths in signed 16 bits, and a mesh indexes its vertices with a byte, so none has more than 256
   (SNX_MII_CFL_VERTICES) */
enum {
    SNX_MII_CFL_BEARD,    /* 4 */
    SNX_MII_CFL_HAT,      /* 264: a hair style's hat part, 0-131 over the hair and 132-263 the cap form */
    SNX_MII_CFL_FACELINE, /* 12, each with the hair's, the nose's and the beard's place on it */
    SNX_MII_CFL_FOREHEAD, /* 264, as the hat */
    SNX_MII_CFL_GLASS,    /* 1: the quad the glasses' texture is drawn on */
    SNX_MII_CFL_HAIR,     /* 264, as the hat, each with the three places a game's hat goes on it */
    SNX_MII_CFL_MASK,     /* 12: the sheet over a faceline the eyes, brows and mouth are drawn on */
    SNX_MII_CFL_NOSELINE, /* 18: the quad the noseline's texture is drawn on */
    SNX_MII_CFL_NOSE,     /* 18 */
    SNX_MII_CFL_MESHES
};
#define SNX_MII_CFL_SECTIONS  20
#define SNX_MII_CFL_VERTICES  256
#define SNX_MII_MESH_VERTICES 4096 /* a mesh's room: the Switch's largest part, a High hair, has about 3000 */
#define SNX_MII_MESH_INDICES  24576
/* one part's mesh, as a parts file takes it: triangles counterclockwise from the side their normals face. same: 0,
   or 1 + an earlier item of its section whose mesh this one is (written once, the item pointing at it). anchor: a
   faceline's hair, nose and beard places (9 floats); a hair style's hat front, side and top, each a rotation (radians
   about x, y and z) then a place (18) */
typedef struct {
    u32 vertices, indices, uv, same;
    float anchor[18];
    float pos[SNX_MII_MESH_VERTICES][3], nrm[SNX_MII_MESH_VERTICES][3], tex[SNX_MII_MESH_VERTICES][2];
    u16 idx[SNX_MII_MESH_INDICES];
} SnxMiiMesh;
/* where a parts file's meshes come from: item of mesh section into mesh (zeroed by the caller), 1, or 0 when the part
   is absent; -1 when it cannot be given, which stops the file (the source says why) */
typedef int (*SnxMiiMeshSource)(void* ctx, u32 section, u32 item, SnxMiiMesh* mesh);
/* this model's own parts as 3DS meshes: its shapes with as many quads as SNX_MII_CFL_VERTICES hold */
int snx_mii_ctr_mesh(void* ctx, u32 section, u32 item, SnxMiiMesh* mesh);
/* CFL_Res.dat from source's meshes and this model's textures into out (room bytes): its size, 0 when it does not fit
   or the source stopped it */
u32 snx_mii_cfl_write(SnxMiiMeshSource source, void* ctx, u8* out, u32 room);
/* the archive's RomFS (level 3, the one file CFL_Res.dat in its root) of our own parts: its size, 0 when it does not
   fit in room */
u32 snx_mii_cfl_romfs(u8* out, u32 room);

#endif
