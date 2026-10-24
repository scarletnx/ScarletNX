/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb/board/save.h"
#include "agb/board/board.h"
#include <string.h>

/* The tags the SDK leaves in a ROM so the emulator knows the save kind. */
uint8_t agb_save_detect(const uint8_t* image, uint64_t size) {
    static const struct {
        const char* tag;
        uint8_t type;
    } tags[] = {
        {"FLASH1M_V", AGB_SAVE_FLASH128},
        {"FLASH512_V", AGB_SAVE_FLASH64},
        {"FLASH_V", AGB_SAVE_FLASH64},
        {"SRAM_V", AGB_SAVE_SRAM},
        {"EEPROM_V", AGB_SAVE_EEPROM}, /* the 512 byte chip, the 8 KiB one once a block past it is named */
    };
    for (uint64_t i = 0; i < size; ++i)
        for (size_t t = 0; t < sizeof(tags) / sizeof(tags[0]); ++t)
            if (strlen(tags[t].tag) <= size - i && !memcmp(image + i, tags[t].tag, strlen(tags[t].tag)))
                return tags[t].type;
    return AGB_SAVE_NONE;
}

void agb_save_reset(AgbBoard* board, uint8_t type) {
    board->save.type = type;
    board->save.phase = 0;
    board->save.erase_armed = board->save.program_armed = board->save.bank_armed = 0;
    board->save.id_mode = 0;
    board->save.bank = 0;
    board->save.dirty = 0;
    board->save.eeprom_state = board->save.eeprom_left = 0;
    board->save.eeprom_block = 0;
    board->save.eeprom_busy = 0;
}

uint32_t agb_save_size(const AgbBoard* board) {
    switch (board->save.type) {
        case AGB_SAVE_SRAM: return AGB_SAVE_SRAM_SIZE;
        case AGB_SAVE_FLASH64: return 0x10000;
        case AGB_SAVE_FLASH128: return 0x20000;
        case AGB_SAVE_EEPROM: return 0x200;
        case AGB_SAVE_EEPROM8K: return 0x2000;
        default: return 0;
    }
}

static uint32_t save_offset(const AgbBoard* board, uint32_t address) {
    uint32_t offset = address & 0xFFFF; /* the window repeats every 64 KiB */
    /* a 32 KiB SRAM has no 16th address line: it repeats twice inside the window (GBATEK) */
    if (board->save.type == AGB_SAVE_SRAM) offset &= AGB_SAVE_SRAM_SIZE - 1;
    if (board->save.type == AGB_SAVE_FLASH128) offset |= (uint32_t)board->save.bank << 16;
    return offset;
}

uint8_t agb_save_read8(AgbBoard* board, uint32_t address) {
    AgbSave* save = &board->save;
    if (save->id_mode) {
        switch (address & 0xFF) {
            case 0: return save->type == AGB_SAVE_FLASH128 ? 0x62 : 0x32; /* Sanyo or Panasonic */
            case 1: return save->type == AGB_SAVE_FLASH128 ? 0x13 : 0x1B;
            default: return 0xFF;
        }
    }
    return board->memory.sram[save_offset(board, address)];
}

static void flash_erase(AgbBoard* board, uint32_t address, int chip) {
    AgbSave* save = &board->save;
    uint32_t base = save->type == AGB_SAVE_FLASH128 ? (uint32_t)save->bank << 16 : 0;
    if (chip) /* the whole chip, both banks of a 128 KiB one */
        memset(board->memory.sram, 0xFF, agb_save_size(board));
    else
        memset(board->memory.sram + (base | (address & 0xF000)), 0xFF, 0x1000); /* 4 KiB sectors */
    save->dirty = 1;
}

void agb_save_write8(AgbBoard* board, uint32_t address, uint8_t value) {
    AgbSave* save = &board->save;
    if (save->type == AGB_SAVE_NONE || save->type == AGB_SAVE_SRAM) {
        uint8_t* byte = &board->memory.sram[save_offset(board, address)];
        if (*byte != value) save->dirty = 1;
        *byte = value;
        return;
    }
    /* flash: the command set is addressed by offsets inside the window */
    if (save->program_armed) {
        board->memory.sram[save_offset(board, address)] &= value; /* programming can only clear bits */
        save->dirty = 1;
        save->program_armed = 0;
        save->phase = 0;
        return;
    }
    if (save->bank_armed) {
        if ((address & 0xFFFF) == 0) save->bank = value & 1;
        save->bank_armed = 0;
        save->phase = 0;
        return;
    }
    if ((address & 0xFFFF) == 0x5555 && value == 0xAA && save->phase == 0) {
        save->phase = 1;
        return;
    }
    if ((address & 0xFFFF) == 0x2AAA && value == 0x55 && save->phase == 1) {
        save->phase = 2;
        return;
    }
    if (save->erase_armed && value == 0x30) {
        flash_erase(board, address, 0);
        save->erase_armed = 0;
        save->phase = 0;
        return;
    }
    if (save->phase == 2 && (address & 0xFFFF) == 0x5555) {
        switch (value) {
            case 0x10: if (save->erase_armed) flash_erase(board, address, 1); save->erase_armed = 0; break;
            case 0x80: save->erase_armed = 1; break;
            case 0x90: save->id_mode = 1; break;
            case 0xF0: save->id_mode = 0; break;
            case 0xA0: save->program_armed = 1; break;
            case 0xB0: save->bank_armed = 1; break;
            default: break;
        }
        save->phase = 0;
        return;
    }
    save->phase = 0;
}

/* EEPROM (GBATEK's "GBA Cart EEPROM"): a request is a bit a halfword, 1 0 to write and 1 1 to read, then the number of a
   64 bit block, most significant bit first, 6 bits on the 512 byte chip and 14 on the 8 KiB one (whose 10 low bits
   count), then a write's 64 bits and a 0, or a read's 0. The transfer's length says where the number ends: the 64 data
   bits and the 0 of a write follow it, the 0 alone a read's. A read request is answered by the next 68 reads, 4 that
   mean nothing then the block's bits; a write keeps the chip busy, reading 0, for as long as mGBA has it take. The
   blocks are kept most significant bit first, as mGBA's .sav keeps them */
enum { EEPROM_IDLE, EEPROM_KIND, EEPROM_WRITE, EEPROM_READ_BLOCK, EEPROM_READ };
#define EEPROM_WRITE_CYCLES 115000
#define EEPROM_READ_BITS    68

int agb_save_eeprom(const AgbBoard* board) {
    return board->save.type == AGB_SAVE_EEPROM || board->save.type == AGB_SAVE_EEPROM8K;
}

/* a block past the 512 byte chip's 64 makes it the 8 KiB one */
static void eeprom_reach(AgbBoard* board, uint32_t block) {
    if (block >= 0x40 && board->save.type == AGB_SAVE_EEPROM) board->save.type = AGB_SAVE_EEPROM8K;
}

void agb_eeprom_write(AgbBoard* board, uint16_t value, uint32_t left) {
    AgbSave* save = &board->save;
    unsigned bit = value & 1;
    switch (save->eeprom_state) {
        case EEPROM_KIND:
            save->eeprom_state = bit ? EEPROM_READ_BLOCK : EEPROM_WRITE;
            save->eeprom_block = 0;
            break;
        case EEPROM_WRITE:
            if (left > 65) /* the block's number */
                save->eeprom_block = (uint16_t)((save->eeprom_block << 1 | bit) & 0x3FF);
            else if (left > 1) { /* its bits: the first of 64 with 65 halfwords left */
                uint32_t at = 65 - left, byte = (uint32_t)save->eeprom_block * 8 + at / 8;
                uint8_t mask = (uint8_t)(0x80 >> (at & 7)), old = board->memory.sram[byte];
                eeprom_reach(board, save->eeprom_block);
                board->memory.sram[byte] = (uint8_t)(bit ? old | mask : old & ~mask);
                if (board->memory.sram[byte] != old) save->dirty = 1;
                save->eeprom_busy = EEPROM_WRITE_CYCLES;
            } else
                save->eeprom_state = EEPROM_IDLE; /* the closing 0 */
            break;
        case EEPROM_READ_BLOCK:
            if (left > 1)
                save->eeprom_block = (uint16_t)((save->eeprom_block << 1 | bit) & 0x3FF);
            else { /* the closing 0: the answer starts */
                eeprom_reach(board, save->eeprom_block);
                save->eeprom_state = EEPROM_READ;
                save->eeprom_left = EEPROM_READ_BITS;
            }
            break;
        default: /* idle, or a request's answer left unread: a 1 starts a request */
            save->eeprom_state = bit ? EEPROM_KIND : EEPROM_IDLE;
            break;
    }
}

uint16_t agb_eeprom_read(AgbBoard* board) {
    AgbSave* save = &board->save;
    uint32_t at;
    if (save->eeprom_state != EEPROM_READ) return save->eeprom_busy ? 0 : 1;
    at = (uint32_t)(EEPROM_READ_BITS - save->eeprom_left--);
    if (!save->eeprom_left) save->eeprom_state = EEPROM_IDLE;
    if (at < 4) return 0;
    at -= 4;
    return (uint16_t)(board->memory.sram[(uint32_t)save->eeprom_block * 8 + at / 8] >> (7 - (at & 7)) & 1);
}

void agb_save_step(AgbBoard* board, uint32_t cycles) {
    AgbSave* save = &board->save;
    save->eeprom_busy = save->eeprom_busy > cycles ? save->eeprom_busy - cycles : 0;
}
