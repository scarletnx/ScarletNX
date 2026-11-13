/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A file of a test's own given the CRC-32 a console's file is known by (the DS's ARM7 BIOS), none of that file's bytes
   in it: what takes the file by its CRC-32 takes the test's. */
#ifndef SNX_TEST_CRC_FORGE_H
#define SNX_TEST_CRC_FORGE_H
#include <stddef.h>
#include <stdint.h>

/* The last 4 of the size bytes at data set so that the whole has zlib's CRC-32 crc: the table's indices that end at
   crc, found from its top bytes backwards (each table entry's top byte is another), then the bytes that reach each
   index from the register the bytes before them leave */
static inline void crc_forge(unsigned char* data, size_t size, uint32_t crc) {
    uint32_t table[256], reg = ~crc;
    unsigned char index[4];
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) c = c & 1 ? c >> 1 ^ 0xEDB88320u : c >> 1;
        table[i] = c;
    }
    for (int k = 3; k >= 0; --k) {
        unsigned j = 0;
        while (table[j] >> 24 != reg >> 24) ++j;
        index[k] = (unsigned char)j;
        reg = (reg ^ table[j]) << 8;
    }
    reg = 0xFFFFFFFFu;
    for (size_t i = 0; i + 4 < size; ++i) reg = reg >> 8 ^ table[(reg ^ data[i]) & 0xFF];
    for (int k = 0; k < 4; ++k) {
        data[size - 4 + k] = (unsigned char)((reg ^ index[k]) & 0xFF);
        reg = reg >> 8 ^ table[index[k]];
    }
}
#endif
