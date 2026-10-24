/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The cartridge's save memory: plain SRAM or flash with its command set, on an 8 bit bus, so the wider accesses the
   CPU makes are one byte each on the way in and replicated on the way out (board/memory.c applies those rules); or an
   EEPROM, which DMA talks to a bit a halfword at 0x0D000000. */
#ifndef SNX_AGB_SAVE_H
#define SNX_AGB_SAVE_H
#include <stdint.h>

typedef struct AgbBoard AgbBoard;
/* EEPROM is the 512 byte chip until the game names a block past it, then the 8 KiB one, as mGBA sizes it */
enum { AGB_SAVE_NONE, AGB_SAVE_SRAM, AGB_SAVE_FLASH64, AGB_SAVE_FLASH128, AGB_SAVE_EEPROM, AGB_SAVE_EEPROM8K };
#define AGB_SAVE_SRAM_SIZE 0x8000u /* the SRAM chips are 32 KiB */

typedef struct {
    uint8_t type;
    uint8_t phase;        /* the unlock sequence: 0xAA, then 0x55 */
    uint8_t erase_armed, program_armed, bank_armed, id_mode, bank;
    uint8_t dirty; /* written since the runner last kept it on disk */
    /* EEPROM: the request it is reading (save.c's EEPROM_ states), the 64 bit block it names, the bits a read has left
       to give, and the CPU cycles a write still takes */
    uint8_t eeprom_state, eeprom_left;
    uint16_t eeprom_block;
    uint32_t eeprom_busy;
} AgbSave;
/* the bytes of board->memory.sram the save kind holds, a .sav's size: 0 for none */
uint32_t agb_save_size(const AgbBoard* board);

/* The save kind the ROM's tag names, or AGB_SAVE_NONE. */
uint8_t agb_save_detect(const uint8_t* image, uint64_t size);
void agb_save_reset(AgbBoard* board, uint8_t type);
uint8_t agb_save_read8(AgbBoard* board, uint32_t address);
void agb_save_write8(AgbBoard* board, uint32_t address, uint8_t value);
/* 1 when the save memory is an EEPROM, which the halfwords at 0x0D000000 reach */
int agb_save_eeprom(const AgbBoard* board);
/* EEPROM: a bit of a request, the halfwords of its transfer left counting this one (1 for a store of the CPU's), and a
   bit read: 0 while a write is busy and 1 after, or a read request's answer */
void agb_eeprom_write(AgbBoard* board, uint16_t value, uint32_t left);
uint16_t agb_eeprom_read(AgbBoard* board);
/* cycles of the CPU passing: an EEPROM write's time runs out */
void agb_save_step(AgbBoard* board, uint32_t cycles);
#endif
