/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2016-2026 melonDS team
   Copyright 2026 the ScarletNX contributors */
/* KEY1 after melonDS's (NDSCart.cpp at 497aa6c, GPL-3.0-or-later: Key1_Encrypt, Key1_Decrypt, Key1_ApplyKeycode,
   Key1_InitKeycode and DecryptSecureArea), which GBATEK's "DS Encryption by Gamecode/Idcode (KEY1)" gives as
   encrypt_64bit, apply_keycode, init_keycode and gamecart_decryption. */
#include "nds/cartridge/key1.h"
#include "common/utilities/endian.h"
#include <string.h>

#define KEY1_MODULO_WORDS 2u /* the card's modulo, 8 bytes */

static uint32_t round_f(const NdsKey1* key, uint32_t z) {
    uint32_t x = key->word[0x012 + (z >> 24)];
    x += key->word[0x112 + (z >> 16 & 0xFF)];
    x ^= key->word[0x212 + (z >> 8 & 0xFF)];
    return x + key->word[0x312 + (z & 0xFF)];
}

void nds_key1_encrypt(const NdsKey1* key, uint32_t block[2]) {
    uint32_t y = block[0], x = block[1];
    for (unsigned i = 0; i <= 0x0F; ++i) {
        uint32_t z = key->word[i] ^ x;
        x = round_f(key, z) ^ y;
        y = z;
    }
    block[0] = x ^ key->word[0x10];
    block[1] = y ^ key->word[0x11];
}

void nds_key1_decrypt(const NdsKey1* key, uint32_t block[2]) {
    uint32_t y = block[0], x = block[1];
    for (unsigned i = 0x11; i >= 0x02; --i) {
        uint32_t z = key->word[i] ^ x;
        x = round_f(key, z) ^ y;
        y = z;
    }
    block[0] = x ^ key->word[0x01];
    block[1] = y ^ key->word[0x00];
}

static uint32_t bswap32(uint32_t v) { return v >> 24 | (v >> 8 & 0xFF00u) | (v << 8 & 0xFF0000u) | v << 24; }

static void apply_keycode(NdsKey1* key, uint32_t code[3]) {
    nds_key1_encrypt(key, &code[1]);
    nds_key1_encrypt(key, &code[0]);
    for (unsigned i = 0; i <= 0x11; ++i) key->word[i] ^= bswap32(code[i % KEY1_MODULO_WORDS]);
    uint32_t scratch[2] = {0, 0};
    for (unsigned i = 0; i < NDS_KEY1_WORDS; i += 2) {
        nds_key1_encrypt(key, scratch);
        key->word[i] = scratch[1];
        key->word[i + 1] = scratch[0];
    }
}

void nds_key1_init(NdsKey1* key, const unsigned char* table, uint32_t idcode, unsigned level) {
    for (unsigned i = 0; i < NDS_KEY1_WORDS; ++i) key->word[i] = snx_ld_le32(table + i * 4);
    uint32_t code[3] = {idcode, idcode >> 1, idcode << 1};
    if (level >= 1) apply_keycode(key, code);
    if (level >= 2) apply_keycode(key, code);
    code[1] <<= 1;
    code[2] >>= 1;
    if (level >= 3) apply_keycode(key, code);
}

static void decrypt_at(const NdsKey1* key, unsigned char* at) {
    uint32_t block[2] = {snx_ld_le32(at), snx_ld_le32(at + 4)};
    nds_key1_decrypt(key, block);
    snx_st_le32(at, block[0]);
    snx_st_le32(at + 4, block[1]);
}

bool nds_key1_secure_decrypt(unsigned char* area, const unsigned char* table, uint32_t gamecode) {
    NdsKey1 key;
    nds_key1_init(&key, table, gamecode, 2);
    decrypt_at(&key, area);
    nds_key1_init(&key, table, gamecode, 3);
    for (unsigned at = 0; at < NDS_KEY1_SECURE; at += 8) decrypt_at(&key, area + at);
    return !memcmp(area, "encryObj", 8);
}
