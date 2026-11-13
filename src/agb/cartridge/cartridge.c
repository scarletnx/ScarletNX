/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/cartridge/cartridge.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The complement check covers the title through the software version. */
static uint8_t header_checksum(const uint8_t* header) {
    unsigned sum = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i) sum += header[i];
    return (uint8_t)(0u - sum - 0x19u);
}

static int fail(char* error, size_t error_size, const char* what) {
    snprintf(error, error_size, "%s", what);
    return -1;
}

int agb_cartridge_load(AgbCartridge* cartridge, const void* image, size_t size, char* error, size_t error_size) {
    const uint8_t* bytes = (const uint8_t*)image;
    if (error_size) error[0] = 0;
    *cartridge = (AgbCartridge){0};
    if (size < AGB_ROM_MIN) {
        snprintf(error, error_size, "ROM image is %zu bytes: too small for a GBA header", size);
        return -1;
    }
    if (size > AGB_ROM_MAX) {
        snprintf(error, error_size, "ROM image is %zu bytes: larger than the 32 MiB a GBA cartridge holds", size);
        return -1;
    }
    cartridge->entry = snx_ld_le32(bytes);
    if ((cartridge->entry & 0xFF000000u) != 0xEA000000u) {
        /* what the file is, when its header says: a flash memory's image (an e-Reader card's dump) erased there, or a
           whole header over no program, the data cartridge another console's game reads */
        size_t erased = 0xA0;
        while (erased < 0xC0 && bytes[erased] == 0xFF) ++erased;
        if (erased == 0xC0)
            snprintf(error, error_size,
                     "ROM image of %zu bytes starts with 0x%08X and its header is erased, all 0xFF: a flash memory's "
                     "image, as an e-Reader card's dump is, not a GBA program",
                     size, cartridge->entry);
        else if (bytes[0xB2] == 0x96 && header_checksum(bytes) == bytes[0xBD])
            snprintf(error, error_size,
                     "ROM header (title \"%.12s\", code %.4s) is whole, but its first word is 0x%08X, not an ARM "
                     "branch: a data cartridge a game reads, not a GBA program",
                     (const char*)bytes + 0xA0, (const char*)bytes + 0xAC, cartridge->entry);
        else
            snprintf(error, error_size, "ROM header starts with 0x%08X, not an ARM branch", cartridge->entry);
        return -1;
    }
    /* the branch target as a ROM offset: 8 past the branch word, plus the sign extended imm24 shifted left 2 */
    cartridge->entry_to = 8u + (uint32_t)((int32_t)(cartridge->entry << 8) >> 6);
    if (bytes[0xB2] != 0x96) {
        snprintf(error, error_size, "ROM header's byte 0xB2 is 0x%02X, not the fixed 0x96", bytes[0xB2]);
        return -1;
    }
    cartridge->checksum = bytes[0xBD];
    cartridge->checksum_title = header_checksum(bytes);
    uint8_t* copy = (uint8_t*)malloc(size);
    if (!copy) return fail(error, error_size, "out of memory for the ROM image");
    memcpy(copy, image, size);
    cartridge->data = copy;
    cartridge->size = size;
    memcpy(cartridge->title, bytes + 0xA0, 12);
    cartridge->title[12] = 0;
    memcpy(cartridge->game_code, bytes + 0xAC, 4);
    cartridge->game_code[4] = 0;
    memcpy(cartridge->maker_code, bytes + 0xB0, 2);
    cartridge->maker_code[2] = 0;
    cartridge->unit = bytes[0xB3];
    cartridge->device = bytes[0xB4];
    cartridge->version = bytes[0xBC];
    return 0;
}

size_t agb_cartridge_extent(const AgbCartridge* cartridge) {
    return cartridge->size == 0x100000 ? 0x400000 : cartridge->size;
}

void agb_cartridge_unload(AgbCartridge* cartridge) {
    free(cartridge->data);
    *cartridge = (AgbCartridge){0};
}
