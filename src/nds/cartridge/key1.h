/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The card's KEY1 encryption: Blowfish, keyed by the game's code over the table the DS's ARM7 BIOS holds. A card holds
   the first 2 KiB of its secure area encrypted with it, and the firmware decrypts them as it boots the card. */
#ifndef SNX_NDS_KEY1_H
#define SNX_NDS_KEY1_H
#include <stdbool.h>
#include <stdint.h>

#define NDS_KEY1_WORDS    0x412u /* the Blowfish P array (18 words) and its four S boxes (256 words each) */
#define NDS_KEY1_TABLE_AT 0x30u  /* where the DS's ARM7 BIOS holds them, as little-endian words */
#define NDS_KEY1_SECURE   0x800u /* the bytes of the secure area the card holds encrypted, from its start */
#define NDS_BIOS7_SIZE    0x4000u
#define NDS_BIOS7_CRC32   0x1280F0D5u /* the DS's ARM7 BIOS, the one melonDS knows as the console's */
#define NDS_BIOS7_FILE "bios7.bin" /* its name in the data folder, where Install Keys puts it and the driver reads it */
#define NDS_SECURE_DESTROYED 0xE7FFDEFFu /* what the firmware writes over the secure area's ID once it checked it */

typedef struct {
    uint32_t word[NDS_KEY1_WORDS];
} NdsKey1;

/* The key for idcode (a card's game code, its header's 0x0C) from table (NDS_KEY1_WORDS words from the ARM7 BIOS's
   NDS_KEY1_TABLE_AT), the code applied level times (1 to 3) with a modulo of 8 bytes, the card's */
void nds_key1_init(NdsKey1* key, const unsigned char* table, uint32_t idcode, unsigned level);
/* one 64-bit block, its low word first */
void nds_key1_encrypt(const NdsKey1* key, uint32_t block[2]);
void nds_key1_decrypt(const NdsKey1* key, uint32_t block[2]);
/* The NDS_KEY1_SECURE bytes at area, as the card holds them, decrypted in place as the firmware does: the first block
   at level 2, then every block at level 3. True when they start with the ID "encryObj", which the caller then
   replaces with NDS_SECURE_DESTROYED twice, as the firmware does */
bool nds_key1_secure_decrypt(unsigned char* area, const unsigned char* table, uint32_t gamecode);
#endif
