/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The card image's layout is GBATEK's (the header, the overlay table, the file allocation table), and the module
   parameters the start code reads are the SDK's, as the code of every game shows them: the autoload list, the
   compressed program's end and the two words that end them (0xDEC00621, 0x2106C0DE). */
#include "nds/cartridge/rom.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint32_t nds_blz_size(const unsigned char* data, uint32_t compressed) {
    if (compressed < 8) return 0;
    uint32_t bounds = snx_ld_le32(data + compressed - 8), extra = snx_ld_le32(data + compressed - 4);
    uint32_t top = bounds & 0xFFFFFF, bottom = bounds >> 24;
    if (bottom < 8 || bottom > top || top > compressed || extra > 0x01000000u) return 0;
    return compressed + extra;
}

uint32_t nds_blz(unsigned char* data, uint32_t compressed, uint32_t capacity) {
    uint32_t size = nds_blz_size(data, compressed);
    if (!size || size > capacity) return 0;
    uint32_t bounds = snx_ld_le32(data + compressed - 8);
    /* from the stream's bottom down to its top, the output from the buffer's end down: the bytes below the top stay
       as they are */
    uint32_t source = compressed - (bounds >> 24), stop = compressed - (bounds & 0xFFFFFF), out = size;
    while (source > stop) {
        unsigned flags = data[--source];
        for (int bit = 0; bit < 8 && source > stop; ++bit, flags <<= 1) {
            if (!(flags & 0x80)) {
                if (!out) return 0;
                data[--out] = data[--source];
                continue;
            }
            if (source < stop + 2) return 0;
            unsigned high = data[--source], low = data[--source];
            uint32_t distance = (((high & 15u) << 8) | low) + 3, length = (high >> 4) + 3;
            if (length > out) return 0;
            for (uint32_t i = 0; i < length; ++i) {
                --out;
                if ((uint64_t)out + distance >= size) return 0;
                data[out] = data[out + distance];
            }
        }
    }
    return size;
}

static uint32_t word(const NdsRom* rom, uint64_t offset) {
    return offset + 4 <= rom->size ? snx_ld_le32(rom->data + offset) : 0;
}

static NdsRegion* add_region(NdsRom* rom, NdsRegionKind kind, uint32_t id, uint32_t address, unsigned char* data,
                             uint32_t size) {
    NdsRegion* grown = realloc(rom->regions, (rom->region_count + 1) * sizeof *grown);
    if (!grown) {
        free(data);
        return NULL;
    }
    rom->regions = grown;
    NdsRegion* r = &grown[rom->region_count++];
    *r = (NdsRegion){.kind = kind, .id = id, .address = address, .size = size, .data = data};
    r->hash = snx_fnv1a(SNX_FNV_BASIS, data, size);
    return r;
}

static bool add_entry(NdsRegion* r, uint32_t pc) {
    uint32_t* grown = realloc(r->entries, (r->entry_count + 1) * sizeof *grown);
    if (!grown) return false;
    r->entries = grown;
    grown[r->entry_count++] = pc;
    return true;
}

/* the module parameters' offset in the program: the word after the nitrocode that follows the program in the image,
   else the place of the two words that end them, looked for in the start code */
static uint32_t module_params(const NdsRom* rom) {
    uint64_t after = (uint64_t)rom->arm9_offset + rom->arm9_size;
    if (word(rom, after) == NDS_NITROCODE) {
        uint32_t at = word(rom, after + 4);
        if (at && at + 0x24 <= rom->arm9_size) return at; /* the parameters' start */
    }
    for (uint32_t at = 0; at + 8 <= rom->arm9_size && at < 0x4000; at += 4)
        if (word(rom, rom->arm9_offset + at) == NDS_NITROCODE && word(rom, rom->arm9_offset + at + 4) == 0x2106C0DEu)
            return at >= 0x1C ? at - 0x1C : 0;
    return 0;
}

static bool load_program(NdsRom* rom, char* error, size_t error_size) {
    uint32_t params = module_params(rom), size = rom->arm9_size, ends = 0;
    if (params && params + 0x18 <= size) ends = word(rom, rom->arm9_offset + params + 0x14);
    if (params && params + 0x1C <= size) rom->sdk_version = word(rom, rom->arm9_offset + params + 0x18);
    uint32_t compressed = ends > rom->arm9_address && ends - rom->arm9_address <= size ? ends - rom->arm9_address : 0;
    uint32_t whole = compressed ? nds_blz_size(rom->data + rom->arm9_offset, compressed) : 0;
    if (compressed && !whole) {
        snprintf(error, error_size, "the ARM9 program says it is compressed up to 0x%08X, and has no footer there",
                 ends);
        return false;
    }
    if (whole < size) whole = size;
    rom->program = malloc(whole ? whole : 1);
    if (!rom->program) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    memset(rom->program, 0, whole);
    memcpy(rom->program, rom->data + rom->arm9_offset, size);
    if (compressed) {
        if (!nds_blz(rom->program, compressed, whole)) {
            snprintf(error, error_size, "the ARM9 program's compressed stream is damaged");
            return false;
        }
        /* its start code decompresses when this is not zero: it is done */
        snx_st_le32(rom->program + params + 0x14, 0);
    }
    rom->program_size = whole;
    unsigned char* copy = malloc(whole ? whole : 1);
    if (!copy) return false;
    memcpy(copy, rom->program, whole);
    NdsRegion* program = add_region(rom, NDS_REGION_PROGRAM, 0, rom->arm9_address, copy, whole);
    if (!program || !add_entry(program, rom->arm9_entry)) return false;
    /* the autoload list: the blocks the start code copies out of the program, the ITCM's code among them. Each entry
       is where it goes, its bytes and the zeros after them; the blocks' bytes lie one after another */
    if (params && params + 0x0C <= whole) {
        uint32_t list = snx_ld_le32(rom->program + params), list_end = snx_ld_le32(rom->program + params + 4);
        uint32_t source = snx_ld_le32(rom->program + params + 8), base = rom->arm9_address;
        if (list >= base && list_end >= list && list_end - base <= whole && source >= base && (list_end - list) <= 0x100)
            for (uint32_t at = list - base, index = 0; at + 12 <= list_end - base; at += 12, ++index) {
                uint32_t to = snx_ld_le32(rom->program + at), bytes = snx_ld_le32(rom->program + at + 4);
                if (!bytes || source - base > whole || bytes > whole - (source - base)) break;
                unsigned char* block = malloc(bytes);
                if (!block) return false;
                memcpy(block, rom->program + (source - base), bytes);
                if (!add_region(rom, NDS_REGION_AUTOLOAD, index, to, block, bytes)) return false;
                source += bytes;
            }
    }
    return true;
}

static bool load_overlays(NdsRom* rom, char* error, size_t error_size) {
    for (uint32_t at = 0; at + 32 <= rom->overlay_size; at += 32) {
        uint64_t entry = (uint64_t)rom->overlay_offset + at;
        uint32_t id = word(rom, entry), address = word(rom, entry + 4), size = word(rom, entry + 8);
        uint32_t init = word(rom, entry + 16), init_end = word(rom, entry + 20), file = word(rom, entry + 24);
        uint32_t flags = word(rom, entry + 28);
        uint64_t fat = (uint64_t)rom->fat_offset + (uint64_t)file * 8;
        uint32_t start = word(rom, fat), end = word(rom, fat + 4);
        if ((uint64_t)file * 8 + 8 > rom->fat_size || end < start || end > rom->size || !size || size > 0x400000u) {
            snprintf(error, error_size, "overlay %u names a file the image does not hold", id);
            return false;
        }
        uint32_t stored = end - start, compressed = flags >> 24 & 1 ? flags & 0xFFFFFF : 0;
        uint32_t room = size > stored ? size : stored;
        unsigned char* data = calloc(room, 1);
        if (!data) return false;
        memcpy(data, rom->data + start, stored);
        if (compressed && compressed <= stored && !nds_blz(data, compressed, room)) {
            snprintf(error, error_size, "overlay %u's compressed stream is damaged", id);
            free(data);
            return false;
        }
        NdsRegion* r = add_region(rom, NDS_REGION_OVERLAY, id, address, data, size);
        if (!r) return false;
        /* its static initializers, which the SDK calls when the overlay starts */
        if (init >= address && init_end >= init && init_end - address <= size)
            for (uint32_t p = init - address; p + 4 <= init_end - address; p += 4) {
                uint32_t fn = snx_ld_le32(r->data + p);
                if (fn && !add_entry(r, fn)) return false;
            }
    }
    return true;
}

bool nds_rom_open(NdsRom* rom, const char* path, char* error, size_t error_size) {
    *rom = (NdsRom){0};
    if (plat_file_map(&rom->map, path) || !rom->map.data) {
        char why[256];
        snprintf(error, error_size, "cannot read %s: %s", path, snx_os_error(why, sizeof why));
        return false;
    }
    rom->data = rom->map.data;
    rom->size = (size_t)rom->map.size;
    if (rom->size < 0x8000) {
        snprintf(error, error_size, "%s is %zu bytes, too short for an NDS card image", path, rom->size);
        goto fail;
    }
    memcpy(rom->title, rom->data, 12);
    memcpy(rom->code, rom->data + 12, 4);
    for (char* c = rom->title; *c; ++c)
        if (*c < 0x20 || *c > 0x7E) *c = ' ';
    for (char* c = rom->code; *c; ++c)
        if (*c < 0x20 || *c > 0x7E) *c = '_';
    rom->arm9_offset = word(rom, 0x20), rom->arm9_entry = word(rom, 0x24);
    rom->arm9_address = word(rom, 0x28), rom->arm9_size = word(rom, 0x2C);
    rom->arm7_offset = word(rom, 0x30), rom->arm7_entry = word(rom, 0x34);
    rom->arm7_address = word(rom, 0x38), rom->arm7_size = word(rom, 0x3C);
    rom->fat_offset = word(rom, 0x48), rom->fat_size = word(rom, 0x4C);
    rom->overlay_offset = word(rom, 0x50), rom->overlay_size = word(rom, 0x54);
    rom->secure_crc = snx_ld_le16(rom->data + 0x6C);
    rom->header_crc = snx_ld_le16(rom->data + 0x15E);
    if (rom->arm9_offset < 0x4000 || (uint64_t)rom->arm9_offset + rom->arm9_size > rom->size || rom->arm9_size < 0x800 ||
        rom->arm9_address < 0x02000000u || (uint64_t)rom->arm9_address + rom->arm9_size > 0x02400000u ||
        rom->arm9_entry < rom->arm9_address || rom->arm9_entry >= rom->arm9_address + rom->arm9_size ||
        (uint64_t)rom->fat_offset + rom->fat_size > rom->size ||
        (uint64_t)rom->overlay_offset + rom->overlay_size > rom->size) {
        snprintf(error, error_size, "%s is not an NDS card image: its header names a program outside it", path);
        goto fail;
    }
    /* the first 2 KiB of a program at 0x4000 are encrypted on the card (KEY1), with the console's key. A dump has
       them decrypted, starting with 0xE7FFDEFF twice, which the firmware puts there once it checked them */
    if (rom->arm9_offset < 0x8000 && word(rom, rom->arm9_offset) != 0xE7FFDEFFu && word(rom, rom->arm9_offset) != 0) {
        uint32_t first = word(rom, rom->arm9_offset);
        /* a homebrew program starts with code: an ARM branch or a load */
        if ((first >> 24) != 0xEA && (first >> 24) != 0xE5 && (first >> 24) != 0xE3 && (first >> 24) != 0xE1) {
            snprintf(error, error_size,
                     "%s has its secure area encrypted, as the card holds it: decrypt the dump first (the key is the "
                     "console's)",
                     path);
            goto fail;
        }
    }
    {
        /* the chip ID a card of this size answers with, as melonDS makes it: a Macronix one */
        uint32_t id = 0xC2;
        if (rom->size >= 1024 * 1024 && rom->size <= 128 * 1024 * 1024)
            id |= (uint32_t)((rom->size >> 20) - 1) << 8;
        else
            id |= (uint32_t)(0x100 - (rom->size >> 28)) << 8;
        rom->chip_id = id;
    }
    if (!load_program(rom, error, error_size) || !load_overlays(rom, error, error_size)) {
        if (error_size && !error[0]) snprintf(error, error_size, "out of memory");
        goto fail;
    }
    rom->identity = SNX_FNV_BASIS;
    for (unsigned i = 0; i < rom->region_count; ++i) {
        const NdsRegion* r = &rom->regions[i];
        uint64_t key[3] = {r->hash, r->address, r->size};
        rom->identity = snx_fnv1a(rom->identity, key, sizeof key);
    }
    return true;
fail:
    nds_rom_close(rom);
    return false;
}

void nds_rom_close(NdsRom* rom) {
    for (unsigned i = 0; i < rom->region_count; ++i) {
        free(rom->regions[i].data);
        free(rom->regions[i].entries);
    }
    free(rom->regions);
    free(rom->program);
    if (rom->map.data) plat_file_unmap(&rom->map);
    *rom = (NdsRom){0};
}
