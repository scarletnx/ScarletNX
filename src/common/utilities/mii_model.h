/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* MiiModel, system data 0100000000000802, generated here (mii_model.c): the Mii parts' shapes and textures a game's
   SDK (nn::mii::detail, as the game links it) builds a Mii's head from, drawn in code so that a game draws a Mii
   with a face, hair and a nose when the player's firmware is not in the game's data folder. Nothing of the console's
   own parts is in it: only their format and their list (which part kinds there are, how many of each, which are
   absent, each texture's size, format and mip levels).

   The archive is six files, each read whole by the SDK and indexed from its start without a check of its header:
   four of textures (NFTR: Low and Mid quality, each in a Linear and an SRGB copy, the same here) and two of shapes
   (NFSR: Mid and High, the same here). A file is 0xc bytes of header (its magic, version 1, its size), then per
   part kind its largest decompressed part and that part's alignment (which size the SDK's pool), then a table of
   one entry per part, then (shapes only) the hair and faceline transforms, then each part's data as a zlib stream.
   A part whose decompressed size is 0 is absent. */
#ifndef SNX_MII_MODEL_H
#define SNX_MII_MODEL_H
#include "common/cpu/aarch64/cpu.h"
#include "common/utilities/mii_db.h"

#define SNX_MII_MODEL_DATA_ID 0x0100000000000802ull
#define SNX_MII_MODEL_FILES   6

/* file i's name, as the SDK opens it */
void snx_mii_model_name(u32 i, char name[32]);
/* the file whose bytes file i has: an earlier one (a Linear and an SRGB copy are the same, as are the two shape
   qualities), or i */
u32 snx_mii_model_same(u32 i);
/* file i, into out (zeroed, at least its size), or only measured with out NULL: its size in bytes */
u32 snx_mii_model_write(u32 i, u8* out);

/* A Mii's picture (a nn::mii::CharInfo) as the launcher and acc show a user's: a bust from the front, side x side
   pixels of RGBA8 (row after row, alpha 255) in look's colors, drawn on the CPU from this model's own parts: the head,
   hair (and its tails and ties) and nose rasterized, the eyes, brows and mouth drawn on the face in 2D where the
   mask's texture puts them, over the favorite color's light background and a shirt of it. side 16 to 1024: 0, else
   -1 with nothing drawn */
int snx_mii_picture(const u8 charinfo[SNX_MII_CHARINFO], const SnxMiiLook* look, int side, u8* rgba);

#endif
