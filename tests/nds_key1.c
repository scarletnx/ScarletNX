/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The card's secure area: KEY1 over a Blowfish table made here, not the console's, against what melonDS's own KEY1 code
   gives for the same table; then the loader, which decrypts an image's secure area with the DS's ARM7 BIOS and refuses
   it without one, or with another file, saying where the BIOS goes and what it read. No game's data, no console key. */
#include "nds/cartridge/key1.h"
#include "nds/cartridge/rom.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/zip.h"
#include "support/crc_forge.h"
#include "support/nds_image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s failed\n", __FILE__, __LINE__, #x);                                             \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define CODE      0x54534554u /* "TEST", the game code the area is encrypted for */
#define CODE_TEXT "TEST"
/* melonDS's Key1_InitKeycode and Key1_Encrypt (NDSCart.cpp) over the same table, area and game code, as its SetCart
   encrypts a decrypted image's secure area: the CRC-32 of the 2 KiB and their first 8 bytes */
#define KNOWN_CRC 0xA62B7908u
static const unsigned char known_head[8] = {0x5E, 0xAD, 0x9B, 0x6A, 0xD6, 0x89, 0xBD, 0x7A};
#define PROGRAM 0x1000u /* the test program: the secure area's 2 KiB, then 2 KiB more */

/* the test's table: xorshift32 from "KEY1", a word at a time, little-endian as the BIOS holds its own */
static void make_table(unsigned char* table) {
    uint32_t x = 0x4B455931u;
    for (unsigned i = 0; i < NDS_KEY1_WORDS; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        snx_st_le32(table + i * 4, x);
    }
}

/* a decrypted program: the area's ID, its bytes, then the rest of the program */
static void make_program(unsigned char* program) {
    memcpy(program, "encryObj", 8);
    for (unsigned i = 8; i < PROGRAM; ++i) program[i] = (unsigned char)(i * 37 + 11);
}

static void encrypt_at(const NdsKey1* key, unsigned char* at) {
    uint32_t block[2] = {snx_ld_le32(at), snx_ld_le32(at + 4)};
    nds_key1_encrypt(key, block);
    snx_st_le32(at, block[0]);
    snx_st_le32(at + 4, block[1]);
}

/* the area as the card holds it: the firmware's decryption backwards, every block at level 3, then the first at 2 */
static void encrypt_area(unsigned char* area, const unsigned char* table, uint32_t code) {
    NdsKey1 key;
    nds_key1_init(&key, table, code, 3);
    for (unsigned at = 0; at < NDS_KEY1_SECURE; at += 8) encrypt_at(&key, area + at);
    nds_key1_init(&key, table, code, 2);
    encrypt_at(&key, area);
}

static bool put(const char* path, const void* data, size_t size) { return !snx_write_file(path, data, size); }

/* KEY1 against melonDS's answer, both ways, and the game code it is keyed by */
static void test_known_answer(const unsigned char* table) {
    unsigned char plain[PROGRAM], area[NDS_KEY1_SECURE];
    make_program(plain);
    memcpy(area, plain, sizeof area);
    encrypt_area(area, table, CODE);
    CHECK(snx_crc32(area, sizeof area) == KNOWN_CRC);
    CHECK(!memcmp(area, known_head, sizeof known_head));
    unsigned char copy[NDS_KEY1_SECURE];
    memcpy(copy, area, sizeof copy);
    CHECK(nds_key1_secure_decrypt(copy, table, CODE));
    CHECK(!memcmp(copy, plain, sizeof copy));
    /* another game's code keys another table: the ID does not come out */
    memcpy(copy, area, sizeof copy);
    CHECK(!nds_key1_secure_decrypt(copy, table, CODE + 1));
    CHECK(memcmp(copy, "encryObj", 8));
}

/* the image at path, opened with the BIOS at bios7: its error when it is refused, NULL when it opens */
static const char* refused(const char* path, const char* bios7, char* error, size_t size) {
    NdsRom rom;
    error[0] = 0;
    if (!nds_rom_open(&rom, path, bios7, error, size)) return error;
    nds_rom_close(&rom);
    return NULL;
}

/* opened with the BIOS at bios7, its program starts as the firmware leaves it: the ID destroyed, the rest as plain */
static void check_opens(const char* path, const char* bios7, const unsigned char* plain) {
    char error[1024] = "";
    NdsRom rom;
    if (!nds_rom_open(&rom, path, bios7, error, sizeof error)) {
        fprintf(stderr, "%s\n", error);
        exit(1);
    }
    CHECK(!strcmp(rom.code, CODE_TEXT));
    CHECK(rom.program_size == PROGRAM && rom.region_count && rom.regions[0].size == PROGRAM);
    CHECK(snx_ld_le32(rom.program) == NDS_SECURE_DESTROYED && snx_ld_le32(rom.program + 4) == NDS_SECURE_DESTROYED);
    CHECK(!memcmp(rom.program + 8, plain + 8, PROGRAM - 8));
    CHECK(!memcmp(rom.regions[0].data, rom.program, PROGRAM)); /* what the compiler takes */
    nds_rom_close(&rom);
}

static void test_images(const char* dir, const unsigned char* table) {
    char error[1024], expected[512];
    unsigned char plain[PROGRAM], card[PROGRAM];
    make_program(plain);
    memcpy(card, plain, sizeof card);
    encrypt_area(card, table, CODE);
    char *image = snx_path_join(dir, "card.nds"), *other = snx_path_join(dir, "other.nds");
    char *bios7 = snx_path_join(dir, NDS_BIOS7_FILE), *clear = snx_path_join(dir, "clear.nds");
    CHECK(image && other && bios7 && clear);
    CHECK(nds_test_card(image, 0x4000, CODE_TEXT, card, PROGRAM));
    /* the same bytes on a card whose game code is another: the key is the code's */
    CHECK(nds_test_card(other, 0x4000, "TESU", card, PROGRAM));

    /* no BIOS: where it goes, its size and its CRC-32 */
    CHECK(refused(image, bios7, error, sizeof error));
    snprintf(expected, sizeof expected, "install your DS's ARM7 BIOS at %s (16384 bytes, CRC-32 0x1280F0D5;", bios7);
    CHECK(strstr(error, image) && strstr(error, "secure area encrypted") && strstr(error, expected));
    CHECK(refused(image, NULL, error, sizeof error) && strstr(error, "no data folder could be named"));

    /* another file there: what it is, and what the BIOS is */
    unsigned char* bios = calloc(1, NDS_BIOS7_SIZE);
    CHECK(bios);
    memcpy(bios + NDS_KEY1_TABLE_AT, table, NDS_KEY1_WORDS * 4);
    CHECK(put(bios7, bios, 100));
    snprintf(expected, sizeof expected,
             "%s is not the DS's ARM7 BIOS that decrypts it: it has 100 bytes, the BIOS 16384", bios7);
    CHECK(refused(image, bios7, error, sizeof error) && strstr(error, expected));
    CHECK(put(bios7, bios, NDS_BIOS7_SIZE));
    uint32_t crc = snx_crc32(bios, NDS_BIOS7_SIZE);
    CHECK(crc != NDS_BIOS7_CRC32);
    snprintf(expected, sizeof expected, "its CRC-32 is 0x%08X, the BIOS's 0x1280F0D5", crc);
    CHECK(refused(image, bios7, error, sizeof error) && strstr(error, expected));

    /* the test's table in a file with the console's BIOS's size and CRC-32, which the loader takes as the console's */
    crc_forge(bios, NDS_BIOS7_SIZE, NDS_BIOS7_CRC32);
    CHECK(snx_crc32(bios, NDS_BIOS7_SIZE) == NDS_BIOS7_CRC32);
    CHECK(put(bios7, bios, NDS_BIOS7_SIZE));
    check_opens(image, bios7, plain);
    CHECK(refused(other, bios7, error, sizeof error));
    snprintf(expected, sizeof expected, "the DS's ARM7 BIOS at %s does not decrypt it with the game code TESU", bios7);
    CHECK(strstr(error, expected) && strstr(error, "not encryObj"));

    /* a dump decrypted with its ID kept needs no BIOS: the firmware's check passes and destroys it */
    CHECK(nds_test_card(clear, 0x4000, CODE_TEXT, plain, PROGRAM));
    check_opens(clear, NULL, plain);
    /* and one whose ID the firmware destroyed already reads as it is */
    unsigned char destroyed[PROGRAM];
    memcpy(destroyed, plain, sizeof destroyed);
    snx_st_le32(destroyed, NDS_SECURE_DESTROYED);
    snx_st_le32(destroyed + 4, NDS_SECURE_DESTROYED);
    CHECK(nds_test_card(clear, 0x4000, CODE_TEXT, destroyed, PROGRAM));
    check_opens(clear, NULL, plain);
    /* a homebrew program's code at 0x4000 is no secure area, and needs no BIOS */
    unsigned char homebrew[PROGRAM];
    memcpy(homebrew, plain, sizeof homebrew);
    snx_st_le32(homebrew, 0xEA0001FEu); /* b 0x02000800 */
    CHECK(nds_test_card(clear, 0x4000, CODE_TEXT, homebrew, PROGRAM));
    NdsRom rom;
    CHECK(nds_rom_open(&rom, clear, NULL, error, sizeof error));
    CHECK(!memcmp(rom.program, homebrew, PROGRAM));
    nds_rom_close(&rom);
    free(bios);
    free(clear);
    free(bios7);
    free(other);
    free(image);
}

int main(void) {
    unsigned char* table = malloc(NDS_KEY1_WORDS * 4);
    char* dir = snx_temp_dir(NULL);
    CHECK(table && dir);
    make_table(table);
    test_known_answer(table);
    test_images(dir, table);
    (void)snx_remove_tree(dir);
    free(dir);
    free(table);
    puts("nds_key1: ok");
    return 0;
}
