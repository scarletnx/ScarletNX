/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An NDS card image (.nds): its header, the ARM9 program as the console's memory holds it once it runs (the SDK's
   start code decompresses it backwards in place: here it is decompressed when the image is read), and every piece of
   code the image holds at a known address, which the offline compiler takes (aot/compiler.c): the program, the blocks
   its start code copies elsewhere (the ITCM's) and the overlays the game loads over one another while it plays. */
#ifndef SNX_NDS_ROM_H
#define SNX_NDS_ROM_H
#include "common/platform/file.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NDS_HEADER_SIZE 0x170u /* what the firmware copies to 0x027FFE00 */
#define NDS_NITROCODE   0xDEC00621u

typedef enum { NDS_REGION_PROGRAM, NDS_REGION_AUTOLOAD, NDS_REGION_OVERLAY } NdsRegionKind;
/* Code at a known address: size bytes at address, as memory holds them when they run. Overlays share addresses, so
   an address alone names no code: a block is a region's while memory holds the region's bytes there (cpu/recomp.c) */
typedef struct {
    NdsRegionKind kind;
    uint32_t id; /* an overlay's number, an autoload block's index */
    uint32_t address, size;
    unsigned char* data; /* owned */
    uint64_t hash;       /* FNV-1a of its bytes */
    uint32_t *entries, entry_count; /* the addresses the image names as code: pc | 1 for Thumb */
} NdsRegion;

typedef struct {
    PFileMap map; /* the image, read where it lies */
    const unsigned char* data;
    size_t size;
    char title[13], code[5];
    uint32_t arm9_offset, arm9_entry, arm9_address, arm9_size;
    uint32_t arm7_offset, arm7_entry, arm7_address, arm7_size;
    uint32_t fat_offset, fat_size, overlay_offset, overlay_size;
    uint32_t chip_id;
    uint32_t sdk_version; /* the SDK the program was built with, from its module parameters: 0 when it has none */
    uint16_t header_crc, secure_crc;
    /* the ARM9 program as memory holds it: decompressed, the field its start code decompresses by cleared */
    unsigned char* program;
    uint32_t program_size;
    NdsRegion* regions;
    unsigned region_count;
    uint64_t identity; /* FNV-1a over every region's hash, address and size: what a compiled module is of */
} NdsRom;

/* The image at path. False with the reason in error. An image whose secure area is still encrypted, as the card
   holds it, is refused: the key is the console's */
bool nds_rom_open(NdsRom* rom, const char* path, char* error, size_t error_size);
void nds_rom_close(NdsRom* rom);
/* The backwards LZ the SDK compresses programs and overlays with (MIi_UncompressBackward): compressed bytes at data,
   decompressed in place into capacity bytes. The decompressed size, 0 when the stream is not one */
uint32_t nds_blz(unsigned char* data, uint32_t compressed, uint32_t capacity);
/* the size a stream decompresses to, 0 when it has no footer */
uint32_t nds_blz_size(const unsigned char* data, uint32_t compressed);
#endif
