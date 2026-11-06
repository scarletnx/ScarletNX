/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The six default Miis come from Eden (src/core/hle/service/mii/types/raw_data.cpp), as misc.c's notice gives */
/* The Mii database ScarletNX keeps (mii_db.h): its Miis' fields, their CharInfo, and how a picture of each looks */
#include "common/utilities/mii_db.h"
#include <string.h>

typedef uint8_t u8;
typedef uint32_t u32;
#define MII_NAME 0x10 /* CharInfo's nickname, UTF-16, after the create id */

/* nn::mii's DefaultMii, the fields in this order (colors in the 3DS's numbering): face type, color, wrinkle, makeup;
   hair type, color, flip; eye type, color, scale, aspect, rotate, x, y; eyebrow type, color, scale, aspect, rotate,
   x, y; nose type, scale, y; mouth type, color, scale, aspect, y; mustache type, beard type, beard color, mustache
   scale, mustache y; glasses type, color, scale, y; mole type, scale, x, y; height, weight, gender, favorite color,
   region move, font region, type. Each is called "no name" */
#define MII_FIELDS 49
static const u8 mii_defaults[6][MII_FIELDS] = {
    {0, 4, 0, 0,  68, 0, 0, 2, 0,  4, 3, 4, 2,  12, 6, 0, 4,  3,  6,  2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0,  0, 0, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 64, 64, 0, 4,  0, 0, 0},
    {0, 0, 0, 0,  55, 6, 0, 2, 4,  4, 3, 4, 2,  12, 6, 6, 4,  3,  6,  2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0,  0, 0, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 64, 64, 0, 5,  0, 0, 0},
    {0, 1, 0, 0,  33, 1, 0, 2, 0,  4, 3, 4, 2,  12, 6, 1, 4,  3,  6,  2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0,  0, 0, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 64, 64, 0, 0,  0, 0, 0},
    {0, 2, 0, 0,  24, 0, 0, 4, 0,  4, 3, 3, 2,  12, 0, 0, 4,  3,  6,  2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0,  0, 0, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 64, 64, 1, 2,  0, 0, 0},
    {0, 0, 0, 0,  14, 7, 0, 4, 5,  4, 3, 3, 2,  12, 0, 7, 4,  3,  6,  2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0,  0, 0, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 64, 64, 1, 6,  0, 0, 0},
    {0, 0, 0, 0,  12, 1, 0, 4, 0,  4, 3, 3, 2,  12, 0, 1, 4,  3,  6,  2, 10, 1, 4, 9, 23,
     0, 4, 3, 13, 0,  0, 0, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 64, 64, 1, 7,  0, 0, 0},
};
/* The database the console keeps in its system save, here pre-baked and never changed: what a game asks of source
   Database is these eleven, before the defaults. Their fields are each in the range CharInfo::Verify takes, and their
   create ids are fixed, so a Mii a game saved is found again on the next run. The first three are ScarletNX's own, as
   a player would make them: baguette girl (red-magenta curled twin tails, red eyes), leek girl (long teal twin tails,
   teal eyes) and phone girl (a yellow ponytail on one side, amber eyes). Their colors are the Switch's common colors
   (0 to 99, past the 3DS's eight), the eight after them the defaults' with other parts in the 3DS numbering.
   Baguette girl, the first, is the Mii a game takes when it takes the database's first */
#define MII_DATABASE_COUNT 11
#define MII_OWN            3 /* the first three: ScarletNX's own, in the Switch's colors */
static const struct {
    u8 create_id[16];
    char name[11];
    u8 f[MII_FIELDS];
} mii_database[MII_DATABASE_COUNT] = {
    /* curled twin tails (hair 112) in crimson (32), red eyes (15), a lashed round eye (28), an open grin (22) */
    {{0xa9, 0x36, 0x9a, 0x34, 0xd9, 0xc5, 0x4e, 0x24, 0xa0, 0xe6, 0xe7, 0x4a, 0x77, 0xf6, 0xcd, 0x59},
     "Baguette",
     {0,  0, 0, 0,  112, 32, 0,  28, 15, 4, 3, 4, 2,  12, 1, 32, 4,  3,  6,  2, 10, 1, 4, 9, 22,
      21, 4, 3, 13, 0,   0,  32, 4,  10, 0, 8, 4, 10, 0,  4, 2,  20, 50, 64, 1, 0,  0, 0, 0}},
    /* long twin tails (126) in teal (61), teal eyes (60), wide lashed eyes (35), an open smile (19) */
    {{0x2e, 0x1c, 0x52, 0xad, 0x0a, 0xd9, 0x4a, 0x32, 0x95, 0x2f, 0xc8, 0xf5, 0xb4, 0x62, 0xbf, 0x0a},
     "Leek Girl",
     {0,  0, 0, 0,  126, 61, 0,  35, 60, 4, 3, 4, 2,  12, 0, 61, 4,  3,  6,  2, 10, 1, 4, 9, 19,
      21, 4, 3, 13, 0,   0,  61, 4,  10, 0, 8, 4, 10, 0,  4, 2,  20, 70, 50, 1, 6,  0, 0, 0}},
    /* a side ponytail (84) in yellow (85), amber eyes (90), half-lidded lashed eyes (59), a flat mouth (20) */
    {{0x04, 0x43, 0xe1, 0x70, 0x77, 0x08, 0x45, 0x23, 0x92, 0xcb, 0x69, 0xe2, 0xf2, 0xdd, 0x9e, 0x58},
     "Phone Girl",
     {0,  0, 0, 0,  84, 85, 0,  59, 90, 4, 3, 4, 2,  12, 19, 85, 4,  3,  6,  2, 10, 1, 4, 9, 20,
      21, 4, 3, 13, 0,  0,  85, 4,  10, 0, 8, 4, 10, 0,  4,  2,  20, 62, 56, 1, 2,  0, 0, 0}},
    {{0x5d, 0xb8, 0xe6, 0x89, 0xf3, 0xad, 0x4a, 0xb8, 0xa6, 0x4b, 0xa4, 0x92, 0xce, 0xb8, 0xf7, 0xed},
     "Alex",
     {1, 1, 0, 0,  33, 1, 0, 2, 4,  4, 3, 4, 2,  12, 6, 1, 4,  3,  6,  2, 10, 1, 4, 9, 23,
      0, 4, 3, 13, 0,  0, 1, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 72, 60, 0, 0,  0, 0, 0}},
    {{0xd8, 0xb0, 0x80, 0x1e, 0xea, 0xc1, 0x45, 0xd0, 0xb3, 0xd9, 0x4f, 0xb2, 0x36, 0x22, 0xc8, 0xe5},
     "Sam",
     {0, 0, 0, 0,  12, 6, 1, 4, 5,  4, 3, 3, 2,  12, 0, 6, 4,  3,  6,  2, 10, 1, 4, 9, 23,
      1, 4, 3, 13, 0,  0, 6, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 58, 56, 1, 6,  0, 0, 0}},
    {{0xf6, 0x70, 0x42, 0x18, 0x0e, 0xef, 0x41, 0x2a, 0x9e, 0xc1, 0xad, 0xeb, 0xb4, 0x93, 0xa2, 0xc4},
     "Kai",
     {2, 2, 0, 0,  7, 0, 0, 0, 0,  4, 3, 4, 2,  12, 3, 0, 4,  3,  6,  2, 10, 5, 4, 9, 11,
      0, 4, 3, 13, 1, 2, 0, 4, 10, 0, 0, 4, 10, 0,  4, 2, 20, 80, 76, 0, 8,  0, 0, 0}},
    {{0x8e, 0x0b, 0x19, 0x1d, 0xb3, 0xcb, 0x4c, 0x75, 0xb8, 0xe1, 0x36, 0xee, 0xe5, 0x53, 0x6e, 0xad},
     "Robin",
     {3, 3, 0, 0,  90, 4, 0, 13, 1,  4, 3, 3, 2,  12, 10, 4, 4,  3,  6,  2, 10, 2, 4, 9, 1,
      2, 4, 3, 13, 0,  0, 4, 4,  10, 1, 1, 4, 10, 0,  4,  2, 20, 50, 64, 1, 10, 0, 0, 0}},
    {{0xfc, 0xb5, 0xd0, 0xc8, 0xe4, 0x2e, 0x42, 0x56, 0xa4, 0xb7, 0x7e, 0x52, 0x2c, 0x4f, 0x2a, 0x2f},
     "Jamie",
     {4, 5, 0, 0,  112, 3, 1, 19, 2,  4, 3, 4, 2,  12, 15, 3, 4,  3,  6,  2, 10, 8, 4, 9, 6,
      0, 4, 3, 13, 0,   0, 3, 4,  10, 0, 0, 4, 10, 0,  4,  2, 20, 96, 40, 0, 3,  0, 0, 0}},
    {{0x19, 0x23, 0xeb, 0xb3, 0xf1, 0xa5, 0x4a, 0x70, 0xac, 0x40, 0xdc, 0xc1, 0x55, 0x56, 0x30, 0xd2},
     "Taylor",
     {5, 4, 0, 0,  45, 2, 0, 27, 3,  4, 3, 3, 2,  12, 20, 2, 4,  3,  6,  2, 10, 3, 4, 9, 30,
      3, 4, 3, 13, 0,  0, 2, 4,  10, 0, 0, 4, 10, 0,  4,  2, 20, 64, 64, 1, 1,  0, 0, 0}},
    {{0xc0, 0x2c, 0x74, 0x19, 0xbe, 0xd2, 0x43, 0x9e, 0x9d, 0x51, 0xd4, 0x31, 0xe8, 0x29, 0xe0, 0x30},
     "Morgan",
     {6, 0, 0, 0,  128, 7, 0, 40, 5,  4, 3, 4, 2,  12, 22, 7, 4,  3,  6,  2, 10, 12, 4, 9, 17,
      0, 4, 3, 13, 2,   0, 7, 4,  10, 5, 3, 4, 10, 0,  4,  2, 20, 40, 88, 0, 5,  0,  0, 0}},
    {{0xaf, 0x62, 0x27, 0xb5, 0xcb, 0x66, 0x46, 0x1a, 0x92, 0x34, 0x58, 0x88, 0x3a, 0x0e, 0x16, 0xe0},
     "Riley",
     {8, 1, 0, 0,  77, 5, 1, 50, 2,  4, 3, 3, 2,  12, 12, 5, 4,  3,  6,  2, 10, 6, 4, 9, 34,
      4, 4, 3, 13, 0,  0, 5, 4,  10, 0, 0, 4, 10, 0,  4,  2, 20, 70, 52, 1, 9,  0, 0, 0}},
};
/* where each field goes in nn::mii::CharInfo, and the table that turns its 3DS color into the Switch's (0 none) */
static const u8 mii_char_offset[MII_FIELDS] = {
    0x2d, 0x2e, 0x2f, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b, 0x3c, 0x3d,
    0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4c, 0x4b, 0x4a, 0x4d, 0x4e,
    0x4f, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x29, 0x2a, 0x28, 0x27, 0x2c, 0x26, 0x2b};
enum { MII_FACE = 1, MII_HAIR, MII_EYE, MII_MOUTH, MII_GLASS };
static const u8 mii_color_kind[MII_FIELDS] = {
    0, MII_FACE,  0, 0, 0, MII_HAIR, 0, 0, MII_EYE,   0, 0, 0, 0, 0, 0,        MII_HAIR, 0,
    0, 0,         0, 0, 0, 0,        0, 0, MII_MOUTH, 0, 0, 0, 0, 0, MII_HAIR, 0,        0,
    0, MII_GLASS, 0, 0, 0, 0,        0, 0, 0,         0, 0, 0, 0, 0, 0};
static u8 mii_color(int kind, u8 v) {
    static const u8 face[8] = {0x0, 0x1, 0x2, 0x3, 0x4, 0x5};
    static const u8 hair[8] = {0x8, 0x1, 0x2, 0x3, 0x4, 0x5, 0x6, 0x7};
    static const u8 eye[6] = {0x8, 0x9, 0xa, 0xb, 0xc, 0xd};
    static const u8 mouth[5] = {0x13, 0x14, 0x15, 0x16, 0x17};
    static const u8 glass[7] = {0x8, 0xe, 0xf, 0x10, 0x11, 0x12, 0x0};
    switch (kind) {
        case MII_FACE: return v < 8 ? face[v] : 0;
        case MII_HAIR: return v < 8 ? hair[v] : 0;
        case MII_EYE: return v < 6 ? eye[v] : 0;
        case MII_MOUTH: return v < 5 ? mouth[v] : 0;
        case MII_GLASS: return v < 7 ? glass[v] : 0;
    }
    return v;
}
void snx_mii_charinfo(uint8_t out[SNX_MII_CHARINFO], const uint8_t create_id[16], const char* name,
                      const uint8_t f[SNX_MII_FIELDS], int common) {
    int i;
    memset(out, 0, SNX_MII_CHARINFO);
    memcpy(out, create_id, 16);
    for (i = 0; name[i]; i++) out[MII_NAME + 2 * i] = (u8)name[i];
    for (i = 0; i < MII_FIELDS; i++) out[mii_char_offset[i]] = common ? f[i] : mii_color(mii_color_kind[i], f[i]);
}
const char* snx_mii_db_name(uint32_t i) { return i < SNX_MII_DB_COUNT ? mii_database[i].name : NULL; }
const char* const* snx_mii_db_names(void) {
    static const char* const names[SNX_MII_DB_COUNT + 1] = {
        mii_database[0].name, mii_database[1].name, mii_database[2].name,  mii_database[3].name,
        mii_database[4].name, mii_database[5].name, mii_database[6].name,  mii_database[7].name,
        mii_database[8].name, mii_database[9].name, mii_database[10].name, NULL};
    return names;
}
int snx_mii_db_find(const char* name) {
    u32 i;
    for (i = 0; i < SNX_MII_DB_COUNT; i++)
        if (!strcmp(name, mii_database[i].name)) return (int)i;
    return -1;
}
int snx_mii_db_find_id(const uint8_t id[16]) {
    u32 i;
    for (i = 0; i < SNX_MII_DB_COUNT; i++)
        if (!memcmp(id, mii_database[i].create_id, 16)) return (int)i;
    return -1;
}
void snx_mii_db_charinfo(uint32_t i, uint8_t out[SNX_MII_CHARINFO]) {
    snx_mii_charinfo(out, mii_database[i].create_id, mii_database[i].name, mii_database[i].f, i < MII_OWN);
}
void snx_mii_db_charinfo_as(uint32_t i, const uint8_t id[16], uint8_t out[SNX_MII_CHARINFO]) {
    snx_mii_charinfo(out, id, mii_database[i].name, mii_database[i].f, i < MII_OWN);
}
void snx_mii_default_charinfo(uint32_t i, const uint8_t id[16], uint8_t out[SNX_MII_CHARINFO]) {
    snx_mii_charinfo(out, id, "no name", mii_defaults[i], 0);
}

/* ---- pictures: the colors ScarletNX draws each database Mii in, by what its fields say (a skin tone, a 3DS hair,
   eye or lip color, a favorite color), and our own three's hair and eyes as they were made */
static const u8 look_skin[6][3] = {{0xf8, 0xd8, 0xb8}, {0xf0, 0xc0, 0x90}, {0xd8, 0x98, 0x68},
                                   {0xf8, 0xc4, 0xa8}, {0xa8, 0x68, 0x48}, {0x68, 0x40, 0x30}};
static const u8 look_hair[8][3] = {{0x2a, 0x24, 0x20}, {0x5a, 0x38, 0x20}, {0x7a, 0x22, 0x12}, {0x8a, 0x4a, 0x20},
                                   {0x8a, 0x8a, 0x90}, {0x4a, 0x3a, 0x14}, {0xa0, 0x70, 0x28}, {0xd8, 0xa8, 0x50}};
static const u8 look_eye[6][3] = {{0x20, 0x20, 0x20}, {0x70, 0x70, 0x70}, {0x70, 0x40, 0x28},
                                  {0x70, 0x60, 0x20}, {0x30, 0x50, 0xc0}, {0x30, 0x80, 0x60}};
static const u8 look_mouth[5][3] = {
    {0xe8, 0x60, 0x30}, {0xe0, 0x20, 0x20}, {0xf0, 0x60, 0x70}, {0xf0, 0xa0, 0x80}, {0x30, 0x30, 0x30}};
static const u8 look_favorite[12][3] = {{0xd0, 0x20, 0x20}, {0xf0, 0x70, 0x20}, {0xf0, 0xd0, 0x20}, {0x90, 0xd0, 0x20},
                                        {0x20, 0xa0, 0x40}, {0x20, 0x50, 0xd0}, {0x40, 0xa8, 0xe8}, {0xf0, 0x70, 0xb0},
                                        {0x80, 0x40, 0xc0}, {0x70, 0x40, 0x20}, {0xf0, 0xf0, 0xf0}, {0x30, 0x30, 0x30}};
/* our own three's hair and eyes: crimson and red, teal and teal, yellow and amber */
static const u8 look_own[SNX_MII_DB_OWN][2][3] = {{{0xc8, 0x20, 0x5a}, {0xb0, 0x18, 0x20}},
                                                  {{0x40, 0xb4, 0xb6}, {0x2a, 0x8a, 0x90}},
                                                  {{0xf6, 0xdc, 0x4a}, {0xf0, 0x90, 0x20}}};
static void look_copy(uint8_t out[3], const u8 in[3]) { memcpy(out, in, 3); }
void snx_mii_db_look(uint32_t i, SnxMiiLook* look) {
    const u8* f = mii_database[i].f;
    look_copy(look->skin, look_skin[f[1] % 6]);
    look_copy(look->favorite, look_favorite[f[45] % 12]);
    if (i < SNX_MII_DB_OWN) {
        look_copy(look->hair, look_own[i][0]);
        look_copy(look->eye, look_own[i][1]);
        look_copy(look->mouth, look_mouth[2]);
        return;
    }
    look_copy(look->hair, look_hair[f[5] % 8]);
    look_copy(look->eye, look_eye[f[8] % 6]);
    look_copy(look->mouth, look_mouth[f[25] % 5]);
}
