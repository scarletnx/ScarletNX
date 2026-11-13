/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/board/memory.h"
#include "agb/board/board.h"
#include "agb/board/save.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

void agb_memory_init(AgbMemory* memory) {
    *memory = (AgbMemory){0};
    memory->ewram = (uint8_t*)calloc(1, AGB_EWRAM_SIZE);
    memory->iwram = (uint8_t*)calloc(1, AGB_IWRAM_SIZE);
    memory->palette = (uint8_t*)calloc(1, AGB_PALETTE_SIZE);
    memory->vram = (uint8_t*)calloc(1, AGB_VRAM_SIZE);
    memory->oam = (uint8_t*)calloc(1, AGB_OAM_SIZE);
    memory->sram = (uint8_t*)calloc(1, AGB_SRAM_SIZE);
    /* save memory reads 0xFF until the game writes it */
    if (memory->sram) memset(memory->sram, 0xFF, AGB_SRAM_SIZE);
    memory->owned = 1;
}

void agb_memory_free(AgbMemory* memory) {
    if (!memory->owned) return;
    free(memory->ewram);
    free(memory->iwram);
    free(memory->palette);
    free(memory->vram);
    free(memory->oam);
    free(memory->sram);
    *memory = (AgbMemory){0};
}

static uint32_t vram_offset(uint32_t address) {
    uint32_t offset = address & 0x1FFFF; /* the console wraps VRAM in 128 KiB windows */
    /* the window's last 32 KiB mirror the 32 KiB above the bitmap region */
    if (offset >= AGB_VRAM_SIZE) offset -= 0x8000;
    return offset;
}

static uint8_t* region8(AgbBoard* board, uint32_t address) {
    switch (address >> 24) {
        case 0x02: return board->memory.ewram + (address & (AGB_EWRAM_SIZE - 1));
        case 0x03: return board->memory.iwram + (address & (AGB_IWRAM_SIZE - 1));
        case 0x05: return board->memory.palette + (address & (AGB_PALETTE_SIZE - 1));
        case 0x06: return board->memory.vram + vram_offset(address);
        case 0x07: return board->memory.oam + (address & (AGB_OAM_SIZE - 1));
        case 0x0E:
        case 0x0F: return board->memory.sram + (address & (AGB_SRAM_SIZE - 1));
        default: return NULL;
    }
}

uint8_t agb_rom_open_bus8(uint32_t address) { return (uint8_t)((address >> 1 & 0xFFFF) >> (address & 1) * 8); }

/* A byte of the cartridge windows at 0x08000000, 0x0A000000 and 0x0C000000, each holding the image over its extent. */
static uint8_t rom8(const AgbBoard* board, uint32_t address) {
    uint32_t offset = address & (AGB_ROM_MAX - 1);
    /* the GPIO port's registers, once a write reached it, over the header's bytes in every window */
    if (board->gpio.touched && offset - AGB_GPIO_DATA < sizeof board->gpio.view)
        return board->gpio.view[offset - AGB_GPIO_DATA];
    if (board->has_cartridge && offset < agb_cartridge_extent(&board->cartridge))
        return board->cartridge.data[offset % board->cartridge.size];
    return agb_rom_open_bus8(offset);
}

static int is_rom(uint32_t address) { return (address >> 24 & 0x0F) >= 0x08 && (address >> 24 & 0x0F) < 0x0E; }
static int is_bios(uint32_t address) { return (address & 0x0FFFFFFF) < AGB_BIOS_SIZE; }

uint32_t agb_access_cycles(const AgbBoard* board, uint32_t address, unsigned width, int sequential) {
    static const uint8_t first[4] = {4, 3, 2, 8}; /* WAITCNT's two bit field of the first access's wait states */
    uint16_t waitcnt = board->io[0x204 >> 1];
    uint32_t n, s;
    switch (address >> 24 & 0xF) {
        case 0x2: return width == 32 ? 6 : 3; /* EWRAM: 16 bits wide, 2 wait states */
        case 0x5:
        case 0x6: return width == 32 ? 2 : 1; /* palette RAM and VRAM: 16 bits wide */
        case 0x8:
        case 0x9: n = first[waitcnt >> 2 & 3], s = waitcnt >> 4 & 1 ? 1 : 2; break;
        case 0xA:
        case 0xB: n = first[waitcnt >> 5 & 3], s = waitcnt >> 7 & 1 ? 1 : 4; break;
        case 0xC:
        case 0xD: n = first[waitcnt >> 8 & 3], s = waitcnt >> 10 & 1 ? 1 : 8; break;
        case 0xE:
        case 0xF: return 1 + first[waitcnt & 3]; /* SRAM: 8 bits wide */
        default: return 1;                       /* BIOS, IWRAM, I/O and OAM: 32 bits wide, no wait */
    }
    /* the cartridge bus is 16 bits wide: a word is two halfwords, the second in sequence */
    return 1 + (sequential ? s : n) + (width == 32 ? 1 + s : 0);
}

/* Where the console decodes nothing a read gives the open bus, the opcode the CPU fetched last at the read's lane, and a
   write goes nowhere, as mGBA's GBALoadBad and its stores have it: past the BIOS up to EWRAM, and past the 28 bits it
   decodes (GBATEK's 10000000-FFFFFFFF). A game's table lookup that goes past its data lands there */
static int unused(uint32_t address) { return address >> 28 || (address >= AGB_BIOS_SIZE && address < 0x02000000); }

uint8_t agb_read8(AgbBoard* board, uint32_t address) {
    if (board->counting) board->counted_cycles += agb_access_cycles(board, address, 8, 1);
    if (unused(address)) return (uint8_t)(board->open_bus >> (address & 3) * 8);
    if ((address >> 24) == 0x04) return (uint8_t)(agb_io_read16(board, address & 0x0FFFFFFE) >> ((address & 1) * 8));
    if ((address >> 24) >= 0x0E) return agb_save_read8(board, address);
    if (is_bios(address)) return (uint8_t)(board->bios_prefetch >> (address & 3) * 8);
    uint8_t* p = region8(board, address);
    if (p) return *p;
    return is_rom(address) ? rom8(board, address) : 0;
}

uint16_t agb_read16(AgbBoard* board, uint32_t address) {
    if (board->counting) board->counted_cycles += agb_access_cycles(board, address, 16, 1);
    if (unused(address)) return (uint16_t)(board->open_bus >> (address & 2) * 8);
    address &= 0x0FFFFFFE;
    if ((address >> 24) == 0x04) return agb_io_read16(board, address);
    if ((address >> 24) >= 0x0E) {
        /* the save memory sits on an 8 bit bus: a wider read returns the byte twice */
        uint8_t byte = agb_save_read8(board, address);
        return (uint16_t)(byte * 0x0101u);
    }
    if ((address >> 24) == 0x0D && agb_save_eeprom(board)) return agb_eeprom_read(board);
    if (is_bios(address)) return (uint16_t)(board->bios_prefetch >> (address & 2) * 8);
    uint8_t* p = region8(board, address);
    if (p) return snx_ld_le16(p);
    return is_rom(address) ? (uint16_t)(rom8(board, address) | rom8(board, address + 1) << 8) : 0;
}

uint32_t agb_read32(AgbBoard* board, uint32_t address) {
    if (board->counting) board->counted_cycles += agb_access_cycles(board, address, 32, 1);
    if (unused(address)) return board->open_bus;
    address &= 0x0FFFFFFC;
    if ((address >> 24) == 0x04)
        return agb_io_read16(board, address) | (uint32_t)agb_io_read16(board, address + 2) << 16;
    if ((address >> 24) >= 0x0E) {
        uint8_t byte = agb_save_read8(board, address);
        return byte * 0x01010101u;
    }
    if (is_bios(address)) return board->bios_prefetch;
    uint8_t* p = region8(board, address);
    if (p) return snx_ld_le32(p);
    if (!is_rom(address)) return 0;
    return (uint32_t)rom8(board, address) | (uint32_t)rom8(board, address + 1) << 8 |
           (uint32_t)rom8(board, address + 2) << 16 | (uint32_t)rom8(board, address + 3) << 24;
}

void agb_write8(AgbBoard* board, uint32_t address, uint8_t value) {
    if (board->counting) board->counted_cycles += agb_access_cycles(board, address, 8, 1);
    if (unused(address)) return;
    if ((address >> 24) == 0x04) {
        agb_io_write16(board, address & 0x0FFFFFFE, (uint16_t)(value * 0x0101u), address & 1 ? 0xFF00 : 0x00FF);
        return;
    }
    if ((address >> 24) >= 0x0E) {
        agb_save_write8(board, address, value);
        return;
    }
    uint8_t* p = region8(board, address);
    if (!p) return;
    switch (address >> 24) {
        /* byte writes reach these halfword wide buses as the byte duplicated in both halves */
        case 0x05:
        case 0x06: {
            /* the OBJ half of VRAM has no byte writes at all, and OAM is halfword wide only */
            uint32_t object_base = (board->ppu.dispcnt & 7) >= 3 ? 0x14000 : 0x10000;
            if (address >> 24 == 0x06 && vram_offset(address) >= object_base) return;
            snx_st_le16(p - (address & 1), (uint16_t)(value * 0x0101u));
            break;
        }
        case 0x07: return;
        default: *p = value; break;
    }
}

void agb_write16(AgbBoard* board, uint32_t address, uint16_t value) {
    if (board->counting) board->counted_cycles += agb_access_cycles(board, address, 16, 1);
    if (unused(address)) return;
    if ((address >> 24) >= 0x0E) {
        /* the save memory is 8 bits wide: a wider store puts one lane's byte down */
        agb_save_write8(board, address, (uint8_t)(value >> ((address & 1) * 8)));
        return;
    }
    address &= 0x0FFFFFFE;
    if ((address >> 24) == 0x04) {
        agb_io_write16(board, address, value, 0xFFFF);
        return;
    }
    if ((address >> 24) == 0x0D && agb_save_eeprom(board)) {
        agb_eeprom_write(board, value, 1); /* a store of the CPU's is a transfer of one */
        return;
    }
    if (agb_gpio_write(board, address, value)) return;
    uint8_t* p = region8(board, address);
    if (p) snx_st_le16(p, value);
}

void agb_write32(AgbBoard* board, uint32_t address, uint32_t value) {
    if (board->counting) board->counted_cycles += agb_access_cycles(board, address, 32, 1);
    if (unused(address)) return;
    if ((address >> 24) >= 0x0E) {
        agb_save_write8(board, address, (uint8_t)(value >> ((address & 3) * 8)));
        return;
    }
    address &= 0x0FFFFFFC;
    if ((address >> 24) == 0x04) {
        agb_io_write16(board, address, (uint16_t)value, 0xFFFF);
        agb_io_write16(board, address + 2, (uint16_t)(value >> 16), 0xFFFF);
        return;
    }
    uint8_t* p = region8(board, address);
    if (p) snx_st_le32(p, value);
}
