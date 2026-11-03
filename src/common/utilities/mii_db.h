/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Miis ScarletNX keeps as the console's database (mii_db.c), as nn::mii::CharInfo: what the mii service lists,
   what users.ini's mii= names a user's Mii from, and what the launcher picks among and draws. Their names are their
   nicknames, which are ASCII */
#ifndef SNX_COMMON_MII_DB_H
#define SNX_COMMON_MII_DB_H
#include <stdint.h>

#define SNX_MII_CHARINFO 0x58 /* nn::mii::CharInfo's bytes */
#define SNX_MII_FIELDS   49   /* the fields mii_db.c lists a Mii by (its order in mii_defaults' comment) */
#define SNX_MII_DB_COUNT 11
#define SNX_MII_DB_OWN   3 /* the first three are ScarletNX's own, baguette girl first, the default Mii */
#define SNX_MII_DEFAULTS 6 /* the console's own default Miis, which BuildDefault gives by index */

const char* snx_mii_db_name(uint32_t i);      /* database Mii i's nickname, NULL past the database */
const char* const* snx_mii_db_names(void);    /* every one's, in order, NULL-ended */
int snx_mii_db_find(const char* name);        /* the database Mii of that nickname (case as written), or -1 */
int snx_mii_db_find_id(const uint8_t id[16]); /* of that create id, or -1 */
/* database Mii i as CharInfo, under its own create id or the one given */
void snx_mii_db_charinfo(uint32_t i, uint8_t out[SNX_MII_CHARINFO]);
void snx_mii_db_charinfo_as(uint32_t i, const uint8_t id[16], uint8_t out[SNX_MII_CHARINFO]);
/* the console's default Mii i (0 to 5) under that create id, "no name" */
void snx_mii_default_charinfo(uint32_t i, const uint8_t id[16], uint8_t out[SNX_MII_CHARINFO]);
/* a CharInfo of those fields (mii_db.c's order), that ASCII nickname and create id; their colors the Switch's common
   colors with common set, else the 3DS's (0 to 7), which become the Switch's */
void snx_mii_charinfo(uint8_t out[SNX_MII_CHARINFO], const uint8_t id[16], const char* name,
                      const uint8_t f[SNX_MII_FIELDS], int common);

/* how a picture of a Mii looks (sRGB): its skin, hair (and brows), eyes, lips and its favorite color's */
typedef struct {
    uint8_t skin[3], hair[3], eye[3], mouth[3], favorite[3];
} SnxMiiLook;
/* database Mii i's: the colors drawn for its own, ScarletNX's choice for each */
void snx_mii_db_look(uint32_t i, SnxMiiLook* look);
#endif
