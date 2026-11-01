/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The shared font block APT:GetSharedFont hands out: Azahar's service/apt/apt.cpp LoadSharedFont and
   bcfnt/bcfnt.cpp RelocateSharedFont (GPL-2.0-or-later, revision in docs/3DS.md). */
#include "3ds/services/font.h"
#include "3ds/services/romfs.h"
#include "common/utilities/endian.h"
#include <string.h>
#define START 0x80u /* the font follows a header of 0x20 words */
/* LZ11 (the 0x11 header, a 24-bit size or 0 and a 32-bit one): the length of the output, or 0 for a stream that ends
   short or does not fit */
static size_t lz11(const unsigned char* in, size_t in_size, unsigned char* out, size_t out_size) {
    if (in_size < 4 || in[0] != 0x11) return 0;
    uint64_t size = snx_ld_le32(in) >> 8;
    size_t at = 4;
    if (!size) {
        if (in_size < 8) return 0;
        size = snx_ld_le32(in + 4);
        at = 8;
    }
    if (size > out_size) return 0;
    size_t n = 0;
    for (unsigned flags = 0, left = 0; n < size;) {
        if (!left) {
            if (at >= in_size) return 0;
            flags = in[at++];
            left = 8;
        }
        bool copy = flags & 0x80;
        flags <<= 1;
        --left;
        if (!copy) {
            if (at >= in_size) return 0;
            out[n++] = in[at++];
            continue;
        }
        if (at >= in_size) return 0;
        unsigned kind = in[at] >> 4, length, extra = kind > 1 ? 1 : kind ? 3 : 2;
        if (in_size - at < extra + 1u) return 0;
        const unsigned char* b = in + at;
        if (kind > 1)
            length = kind + 1;
        else if (kind)
            length = (((unsigned)(b[0] & 0xf) << 12) | (unsigned)b[1] << 4 | b[2] >> 4) + 0x111;
        else
            length = (((unsigned)(b[0] & 0xf) << 4) | b[1] >> 4) + 0x11;
        uint32_t back = (((uint32_t)(b[extra - 1] & 0xf) << 8) | b[extra]) + 1;
        at += extra + 1;
        if (back > n || length > size - n) return 0;
        for (unsigned i = 0; i < length; ++i, ++n) out[n] = out[n - back];
    }
    return n;
}
static uint32_t word(const unsigned char* b, size_t at) { return snx_ld_le32(b + at); }
static void add(unsigned char* b, size_t at, uint32_t delta) { snx_st_le32(b + at, word(b, at) + delta); }
/* The font's section offsets are absolute addresses once the block lies at its address: the stored ones are those of
   the address it was built for, which the sections' own positions tell. The block is the CFNT at START: its header,
   then the FINF, TGLP, CWDH and CMAP sections, each an 8-byte header (magic, size) and a body */
static bool relocate(unsigned char* b, size_t size, uint32_t address) {
    if (size < START + 0x14 || memcmp(b + START, "CFNU", 4)) return false;
    uint32_t blocks = word(b, START + 0x10), end = (uint32_t)size;
    size_t first = START + (size_t)snx_ld_le16(b + START + 6);
    uint32_t cmap = 0, cwdh = 0, tglp = 0, assumed[3] = {0, 0, 0}; /* the stored offsets less their section header */
    size_t at = first;
    for (uint32_t i = 0; i < blocks; ++i) {
        if (at + 8 > end) return false;
        if (!memcmp(b + at, "FINF", 4)) {
            if (at + 28 > end) return false;
            assumed[0] = word(b, at + 24) - 8; /* CMAP */
            assumed[1] = word(b, at + 20) - 8; /* CWDH */
            assumed[2] = word(b, at + 16) - 8; /* TGLP */
        } else if (!cmap && !memcmp(b + at, "CMAP", 4))
            cmap = (uint32_t)at;
        else if (!cwdh && !memcmp(b + at, "CWDH", 4))
            cwdh = (uint32_t)at;
        else if (!tglp && !memcmp(b + at, "TGLP", 4))
            tglp = (uint32_t)at;
        uint32_t length = word(b, at + 4);
        if (!length || length > end - at) return false;
        at += length;
    }
    uint32_t base = assumed[0] - cmap;
    if (base != assumed[1] - cwdh || base != assumed[2] - tglp) return false;
    uint32_t delta = address - base;
    at = first;
    for (uint32_t i = 0; i < blocks; ++i) {
        if (!memcmp(b + at, "FINF", 4)) {
            add(b, at + 16, delta);
            add(b, at + 20, delta);
            add(b, at + 24, delta);
        } else if (!memcmp(b + at, "CMAP", 4)) {
            if (word(b, at + 16)) add(b, at + 16, delta);
        } else if (!memcmp(b + at, "CWDH", 4)) {
            if (word(b, at + 12)) add(b, at + 12, delta);
        } else if (!memcmp(b + at, "TGLP", 4))
            add(b, at + 28, delta);
        at += word(b, at + 4);
    }
    return true;
}
bool snx_ctr_shared_font_build(const unsigned char* archive, size_t archive_size, unsigned char* block, size_t size,
                               uint32_t address) {
    uint64_t offset, length;
    if (!snx_ctr_romfs_image_file(archive, archive_size, "cbf_std.bcfnt.lz", &offset, &length)) return false;
    size_t decompressed = size > START ? lz11(archive + offset, (size_t)length, block + START, size - START) : 0;
    if (!decompressed) return false;
    memset(block, 0, START);
    snx_st_le32(block, 2); /* successfully loaded */
    snx_st_le32(block + 4, 1); /* the region: Japan, Europe and America */
    snx_st_le32(block + 8, (uint32_t)decompressed);
    block[START + 3] = 'U'; /* the magic goes from CFNT to CFNU */
    return relocate(block, START + decompressed, address);
}
