/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The Mii selector (appletEd), the library applet a game starts to have the player pick a Mii, played in the applet's
   place. With no box over the game to pick in, it answers at once with ScarletNX's own Mii as a Mii of the console's
   database, as Azahar's DefaultMiiSelector answers with its standard Mii. Ported from Azahar
   core/hle/applets/mii_selector.{cpp,h}, core/hle/mii.{cpp,h} and core/frontend/applets/mii_selector.cpp
   (GPL-2.0-or-later, Copyright 2016-2023 Citra Emulator Project / Azahar Emulator Project) at ac4a97e: MiiConfig's
   and MiiResult's layouts, the Start and Finalize steps and the Mii's checksum. The Mii's fields are laid out as
   3dbrew's Mii page gives them; the Mii is ScarletNX's own, made of its fields, no console's */
#include "3ds/services/mii_selector.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/apt.h"
#include "3ds/services/cfg.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <string.h>

/* MiiConfig, 0x104 bytes (Azahar applets/mii_selector.h; 3dbrew's Mii Selector page names the flags) */
enum {
    CONFIG_CANCEL = 0x00,     /* a Cancel button */
    CONFIG_GUESTS = 0x01,     /* the Guest Miis may be picked */
    CONFIG_TITLE = 0x08,      /* 64 UTF-16 units over the list, the selector's own title with none */
    CONFIG_GUEST_PAGE = 0x8c, /* it opens on the Guest Miis' page */
    CONFIG_USER_LIST = 0x9a,  /* which of the database's 100 Miis may be picked: nonzero for each that may */
    CONFIG_MAGIC = 0x100,
    CONFIG_SIZE = 0x104
};
/* MiiResult, 0x84 bytes, its words big-endian: cancelled (0 for a Mii picked), a Guest Mii picked, which one (-1 for
   a Mii of the database), the Mii with its checksum, a Guest Mii's name */
enum { RESULT_GUEST_INDEX = 0x08, RESULT_MII = 0x0c, RESULT_CRC = 0x6a, RESULT_SIZE = 0x84 };
#define CONFIG_MAGIC_VALUE 0x13de28cfu /* without it the console's selector lists no Mii */
#define TITLE_UNITS        64u
/* the Mii's bytes (3dbrew's Mii format), its nickname's and its author's places, ten UTF-16 units each */
enum { MII_SIZE = 0x5c, MII_NAME = 0x1a, MII_AUTHOR = 0x48, MII_NAME_UNITS = 10 };

/* ScarletNX's own Mii, Baguette: curled twin tails, a lashed round eye, an open grin, in the 3DS's palette (red hair,
   brown eyes: it has no crimson and no red eyes). Her fields in this order: face shape, skin, wrinkles, makeup; hair
   style, color, flip; eye style, color, scale, y scale, rotation, x spacing, y; eyebrow style, color, scale, y scale,
   rotation, x spacing, y; nose style, scale, y; mouth style, color, scale, y scale, y; mustache style, beard style,
   beard color, mustache scale, mustache y; glasses style, color, scale, y; mole, its scale, x, y; height, width; sex
   (1 female), favorite color, favorite (the console's personal Mii always is) */
static const uint8_t baguette[] = {0,  0, 0, 0, 112, 2, 0, 28, 2, 4,  3, 4, 2, 12, 1, 2, 4, 3,  6,  2,  10, 1, 4, 9,
                                   22, 2, 4, 3, 13,  0, 0, 2,  4, 10, 0, 0, 4, 10, 0, 4, 2, 20, 50, 64, 1,  0, 1};
/* where each field goes: its first bit, counted from the Mii's first byte, little-endian within each of its words
   (the bytes at 0x30 to 0x33, the 32-bit eye and eyebrow words at 0x34 and 0x38, the 16-bit ones from 0x3c to 0x46,
   height and width at 0x2e, the 16-bit word at 0x18), and its bits */
static const struct {
    uint16_t bit;
    uint8_t bits;
} place[] = {{0x181, 4}, {0x185, 3}, {0x188, 4}, {0x18c, 4}, {0x190, 8}, {0x198, 3}, {0x19b, 1}, {0x1a0, 6},
             {0x1a6, 3}, {0x1a9, 4}, {0x1ad, 3}, {0x1b0, 5}, {0x1b5, 4}, {0x1b9, 5}, {0x1c0, 5}, {0x1c5, 3},
             {0x1c8, 4}, {0x1cc, 3}, {0x1d0, 5}, {0x1d5, 4}, {0x1d9, 5}, {0x1e0, 5}, {0x1e5, 4}, {0x1e9, 5},
             {0x1f0, 6}, {0x1f6, 3}, {0x1f9, 4}, {0x1fd, 3}, {0x200, 5}, {0x205, 3}, {0x210, 3}, {0x213, 3},
             {0x216, 4}, {0x21a, 5}, {0x220, 4}, {0x224, 3}, {0x227, 4}, {0x22b, 5}, {0x230, 1}, {0x231, 4},
             {0x235, 5}, {0x23a, 5}, {0x170, 8}, {0x178, 8}, {0x0c0, 1}, {0x0ca, 4}, {0x0ce, 1}};
#define MII_FIELDS (sizeof baguette)
_Static_assert(sizeof place / sizeof *place == MII_FIELDS, "a place for each of the Mii's fields");
/* her identity, ScarletNX's: the system she was made on, her Mii ID (big-endian: a Mii that is not special, made
   2026-11-03, its seconds since 2010 halved) and her maker's address */
static const uint8_t system_id[8] = {0x81, 0xb3, 0xab, 0x41, 0xf9, 0xfe, 0x0c, 0xd3};
static const uint8_t maker_address[6] = {0x26, 0x0d, 0xe8, 0xf1, 0x9d, 0xdf};
#define MII_ID 0x9fd5f480u
static const char nickname[] = "Baguette";

bool snx_ctr_mii_selector_is(uint32_t applet_id) { return applet_id == 0x202 || applet_id == 0x402; }

/* CRC-16/XMODEM (polynomial 0x1021, MSB first, from 0), the Mii's checksum (3dbrew's Mii, Checksum) */
static uint16_t crc16(const uint8_t* p, uint32_t n) {
    uint32_t crc = 0;
    for (uint32_t i = 0; i < n; ++i) {
        crc ^= (uint32_t)p[i] << 8;
        for (unsigned j = 0; j < 8; ++j) crc = ((crc << 1) ^ (crc & 0x8000 ? 0x1021u : 0)) & 0xffff;
    }
    return (uint16_t)crc;
}
/* Baguette in the 3DS's Mii format: version 3, made on a 3DS, the first of the database's first page; her author the
   console's user name, as a Mii made on the console has it */
static void mii(uint8_t out[MII_SIZE]) {
    uint16_t author[SNX_CTR_CFG_USERNAME];
    uint32_t n = snx_ctr_cfg_username(author);
    memset(out, 0, MII_SIZE);
    out[0] = 3;
    out[3] = 3 << 4;
    memcpy(out + 4, system_id, sizeof system_id);
    snx_st_be32(out + 0xc, MII_ID);
    memcpy(out + 0x10, maker_address, sizeof maker_address);
    for (unsigned i = 0; nickname[i]; ++i) snx_st_le16(out + MII_NAME + i * 2, (uint8_t)nickname[i]);
    for (uint32_t i = 0; i < n && i < MII_NAME_UNITS; ++i) snx_st_le16(out + MII_AUTHOR + i * 2, author[i]);
    for (unsigned f = 0; f < MII_FIELDS; ++f)
        for (unsigned b = 0; b < place[f].bits; ++b)
            if (baguette[f] >> b & 1) out[(place[f].bit + b) / 8] |= (uint8_t)(1u << (place[f].bit + b) % 8);
}
/* the configuration's title for the log: its ASCII, a ? for any other unit, cut at its terminator */
static void title(const unsigned char* config, char out[TITLE_UNITS + 1]) {
    unsigned n = 0;
    for (; n < TITLE_UNITS; ++n) {
        uint16_t u = snx_ld_le16(config + CONFIG_TITLE + n * 2);
        if (!u) break;
        out[n] = u >= 0x20 && u < 0x7f ? (char)u : '?';
    }
    out[n] = 0;
}
void snx_ctr_mii_selector_start(SnxCtrIpc* c, const unsigned char* config, uint32_t size) {
    SnxCtrProcess* p = c->process;
    if (size != CONFIG_SIZE) {
        fprintf(stderr,
                "3ds mii selector: the game starts the Mii selector with a %u-byte configuration: it takes "
                "0x104 bytes\n",
                (unsigned)size);
        c->state = SNX_CTR_IPC_UNSUPPORTED;
        return;
    }
    char shown[TITLE_UNITS + 1];
    uint32_t magic = snx_ld_le32(config + CONFIG_MAGIC), applet = p->apt_library;
    title(config, shown);
    fprintf(stderr,
            "3ds mii selector: the game opens the Mii selector: title \"%s\", Cancel %u, Guest Miis %u, the Guest "
            "page first %u, the database's first Mii pickable %u\n",
            shown, (unsigned)config[CONFIG_CANCEL], (unsigned)config[CONFIG_GUESTS],
            (unsigned)config[CONFIG_GUEST_PAGE], (unsigned)config[CONFIG_USER_LIST]);
    if (magic != CONFIG_MAGIC_VALUE)
        fprintf(stderr,
                "3ds mii selector: the configuration's magic is 0x%08x, not 0x%08x: the console's selector would list "
                "no Mii, this one answers with its own\n",
                (unsigned)magic, CONFIG_MAGIC_VALUE);
    unsigned char result[RESULT_SIZE] = {0}; /* picked, not cancelled; not a Guest Mii, no Guest Mii's name */
    mii(result + RESULT_MII);
    snx_st_be32(result + RESULT_GUEST_INDEX, 0xffffffffu);
    snx_st_be16(result + RESULT_CRC, crc16(result + RESULT_MII, MII_SIZE + 2));
    fprintf(stderr,
            "3ds mii selector: the Mii selector answers with ScarletNX's own Mii, %s, as no box asks the "
            "player\n",
            nickname);
    snx_ctr_apt_library_closed(p);
    snx_ctr_apt_send(p, applet, SNX_CTR_APT_WAKEUP_BY_EXIT, 0, result, RESULT_SIZE);
}
