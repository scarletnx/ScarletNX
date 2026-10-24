/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/board/board.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(expression)                                                                                              \
    do {                                                                                                               \
        if (!(expression)) {                                                                                           \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expression);                                           \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define ROM_SIZE 0x200

/* A ROM image with a valid header: an ARM branch over the header, then the title block. */
static void fix_checksum(uint8_t* rom) {
    unsigned sum = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i) sum += rom[i];
    rom[0xBD] = (uint8_t)(0u - sum - 0x19u);
}
/* an EEPROM request as a game's DMA sends it, a bit a halfword: 1 0 to write (data's 64 bits, then a 0) or 1 1 to read
   (then a 0), with block's number in width bits between, most significant bit first. Its halfwords */
static unsigned eeprom_request(uint16_t* bits, int read, unsigned block, unsigned width, const uint8_t* data) {
    unsigned n = 0;
    bits[n++] = 1;
    bits[n++] = (uint16_t)read;
    for (unsigned i = width; i-- > 0;) bits[n++] = (uint16_t)(block >> i & 1);
    for (unsigned i = 0; !read && i < 64; ++i) bits[n++] = (uint16_t)(data[i / 8] >> (7 - i % 8) & 1);
    bits[n++] = 0;
    return n;
}
/* DMA3 from source to destination, count halfwords, at once */
static void dma3(AgbBoard* board, uint32_t source, uint32_t destination, unsigned count) {
    agb_write32(board, 0x040000D4, source);
    agb_write32(board, 0x040000D8, destination);
    agb_write16(board, 0x040000DC, (uint16_t)count);
    agb_write16(board, 0x040000DE, 0x8000);
}
static void make_rom(uint8_t* rom) {
    memset(rom, 0, ROM_SIZE);
    rom[0] = 0x2E;
    rom[3] = 0xEA; /* B 0xC0: the entry branch every GBA ROM starts with */
    memcpy(rom + 0xA0, "TESTGAME", 8);
    memcpy(rom + 0xAC, "BZVE", 4);
    memcpy(rom + 0xB0, "01", 2);
    rom[0xB2] = 0x96;
    rom[0xB3] = 0x00;
    rom[0xB4] = 0x00;
    rom[0xBC] = 3;
    fix_checksum(rom);
}

int main(void) {
    uint8_t* rom = (uint8_t*)malloc(ROM_SIZE);
    char error[256];
    CHECK(rom);
    make_rom(rom);

    AgbBoard board;
    agb_board_init(&board);
    /* the registers as the BIOS leaves them: forced blank, the serial port in its general purpose mode (a game reads it
       to tell a link boot from its own), the sound's bias at its middle, POSTFLG */
    CHECK(agb_read16(&board, 0x04000000) == 0x0080 && agb_read16(&board, 0x04000134) == 0x8000);
    CHECK(agb_read16(&board, 0x04000088) == 0x0200 && agb_read8(&board, 0x04000300) == 1);
    CHECK(!agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(board.has_cartridge);
    const AgbCartridge* cartridge = &board.cartridge;
    CHECK(cartridge->size == ROM_SIZE);
    CHECK(!strcmp(cartridge->title, "TESTGAME"));
    CHECK(!strcmp(cartridge->game_code, "BZVE"));
    CHECK(!strcmp(cartridge->maker_code, "01"));
    CHECK(cartridge->version == 3 && cartridge->unit == 0 && cartridge->device == 0);
    CHECK(cartridge->entry == 0xEA00002E && cartridge->entry_to == 0xC0);
    CHECK(!memcmp(cartridge->data, rom, ROM_SIZE));
    CHECK(cartridge->data != rom); /* the cartridge keeps its own copy */

    /* titles run to the first NUL */
    make_rom(rom);
    rom[0xA4] = 0;
    fix_checksum(rom);
    CHECK(!agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(!strcmp(board.cartridge.title, "TEST"));

    /* each header violation names the byte it failed on */
    make_rom(rom);
    CHECK(agb_board_insert(&board, rom, AGB_ROM_MIN - 1, error, sizeof error));
    CHECK(strstr(error, "too small"));
    CHECK(agb_board_insert(&board, rom, (size_t)AGB_ROM_MAX + 4, error, sizeof error));
    CHECK(strstr(error, "32 MiB"));
    make_rom(rom);
    rom[3] = 0x00;
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "branch"));
    make_rom(rom);
    rom[0xB2] = 0x00;
    fix_checksum(rom);
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "0x96"));
    make_rom(rom);
    rom[0xBD] ^= 0x40;
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "checksum"));

    /* a rejected insert leaves the accepted cartridge in place */
    CHECK(board.has_cartridge && !strcmp(board.cartridge.title, "TEST"));

    /* the BIOS reads as the opcode it fetched last, at the read's lane, and ignores writes */
    CHECK(agb_read32(&board, 0x00000000) == AGB_BIOS_PREFETCH_BOOT);
    CHECK(agb_read16(&board, 0x00000102) == (AGB_BIOS_PREFETCH_BOOT >> 16));
    CHECK(agb_read8(&board, 0x00003FFD) == (uint8_t)(AGB_BIOS_PREFETCH_BOOT >> 8));
    agb_write32(&board, 0x00000000, 0);
    board.bios_prefetch = AGB_BIOS_PREFETCH_SWI;
    CHECK(agb_read32(&board, 0x00000000) == AGB_BIOS_PREFETCH_SWI);
    /* the cartridge windows hold the image once, and past it the bus still holds address / 2 */
    CHECK(agb_read32(&board, 0x08000000) == board.cartridge.entry);
    CHECK(agb_read16(&board, 0x08000000 + ROM_SIZE) == ROM_SIZE / 2);
    CHECK(agb_read8(&board, 0x08000001 + ROM_SIZE) == (ROM_SIZE / 2) >> 8);
    CHECK(agb_read32(&board, 0x0C000000 + 0x123454) == (0x123454u / 2 | (0x123456u / 2) << 16));
    CHECK(agb_read16(&board, 0x09FFFFFE) == 0xFFFF);

    /* a 32 KiB SRAM repeats twice in the 64 KiB window, which repeats over 0x0E000000 to 0x0FFFFFFF */
    agb_save_reset(&board, AGB_SAVE_SRAM);
    agb_write8(&board, 0x0E000010, 0x5A);
    CHECK(agb_read8(&board, 0x0E008010) == 0x5A && agb_read8(&board, 0x0F018010) == 0x5A);
    agb_write8(&board, 0x0E008020, 0xA5);
    CHECK(agb_read8(&board, 0x0E000020) == 0xA5);
    /* the runner keeps a save its kind's size once the game wrote it */
    CHECK(agb_save_size(&board) == 0x8000 && board.save.dirty);
    board.save.dirty = 0;
    agb_write8(&board, 0x0E000020, 0xA5); /* the same byte again: nothing new to keep */
    CHECK(!board.save.dirty);
    agb_save_reset(&board, AGB_SAVE_FLASH64);
    CHECK(agb_save_size(&board) == 0x10000 && !board.save.dirty);
    /* a 128 KiB flash's chip erase clears both of its banks, whichever is selected */
    agb_save_reset(&board, AGB_SAVE_FLASH128);
    CHECK(agb_save_size(&board) == 0x20000);
    board.memory.sram[0x00005] = 0x12;
    board.memory.sram[0x10005] = 0x34;
    {
        static const uint32_t addresses[] = {0x5555, 0x2AAA, 0x5555, 0x5555, 0x2AAA, 0x5555};
        static const uint8_t values[] = {0xAA, 0x55, 0x80, 0xAA, 0x55, 0x10};
        for (int i = 0; i < 6; ++i) agb_write8(&board, 0x0E000000 + addresses[i], values[i]);
    }
    CHECK(board.memory.sram[0x00005] == 0xFF && board.memory.sram[0x10005] == 0xFF && board.save.dirty);

    /* EEPROM: a write of block 5 (6 bits of number on the 512 byte chip) lands at byte 40, most significant bit first,
       and keeps it busy, reading 0, for 115000 cycles. A read request is answered by 68 bits: 4, then the block's. A
       block past 63, with 14 bits of number, makes it the 8 KiB chip */
    agb_save_reset(&board, AGB_SAVE_EEPROM);
    CHECK(agb_save_size(&board) == 0x200);
    {
        static const uint8_t data[8] = {0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0xF0};
        uint16_t bits[81];
        unsigned n = eeprom_request(bits, 0, 5, 6, data);
        CHECK(n == 73);
        for (unsigned i = 0; i < n; ++i) agb_write16(&board, 0x02000000 + i * 2, bits[i]);
        dma3(&board, 0x02000000, 0x0D000000, n);
        CHECK(!memcmp(board.memory.sram + 40, data, 8) && board.save.dirty);
        CHECK(agb_read16(&board, 0x0D000000) == 0);
        agb_save_step(&board, 114999);
        CHECK(agb_read16(&board, 0x0D000000) == 0);
        agb_save_step(&board, 1);
        CHECK(agb_read16(&board, 0x0D000000) == 1);
        n = eeprom_request(bits, 1, 5, 6, NULL);
        CHECK(n == 9);
        for (unsigned i = 0; i < n; ++i) agb_write16(&board, 0x02000000 + i * 2, bits[i]);
        dma3(&board, 0x02000000, 0x0D000000, n);
        dma3(&board, 0x0D000000, 0x02000100, 68);
        for (unsigned i = 0; i < 68; ++i)
            CHECK(agb_read16(&board, 0x02000100 + i * 2) == (i < 4 ? 0 : data[(i - 4) / 8] >> (7 - (i - 4) % 8) & 1));
        CHECK(agb_read16(&board, 0x0D000000) == 1); /* the answer given, the chip is ready again */
        n = eeprom_request(bits, 0, 0x123, 14, data);
        CHECK(n == 81);
        for (unsigned i = 0; i < n; ++i) agb_write16(&board, 0x02000000 + i * 2, bits[i]);
        dma3(&board, 0x02000000, 0x0D000000, n);
        CHECK(board.save.type == AGB_SAVE_EEPROM8K && agb_save_size(&board) == 0x2000);
        CHECK(!memcmp(board.memory.sram + 0x123 * 8, data, 8));
        n = eeprom_request(bits, 1, 0x123, 14, NULL);
        for (unsigned i = 0; i < n; ++i) agb_write16(&board, 0x02000000 + i * 2, bits[i]);
        dma3(&board, 0x02000000, 0x0D000000, n);
        dma3(&board, 0x0D000000, 0x02000100, 68);
        CHECK(agb_read16(&board, 0x02000100 + 4 * 2) == 0 && agb_read16(&board, 0x02000100 + 7 * 2) == 1); /* 0x12 */
        CHECK(agb_read16(&board, 0x02000100 + 67 * 2) == 0); /* 0xF0's last bit */
    }
    agb_save_reset(&board, AGB_SAVE_NONE);

    /* the keypad: KEYINPUT reads a key held as 0, and KEYCNT's interrupt comes once its keys meet its condition */
    agb_board_keys(&board, 0x009); /* A and Start */
    CHECK(agb_read16(&board, 0x04000130) == (0x3FF & ~0x009));
    board.irq_if = 0;
    agb_write16(&board, 0x04000132, 0x4000 | 0x003); /* A or B */
    agb_board_keys(&board, 0x008);
    CHECK(!(board.irq_if & AGB_IRQ_KEYPAD));
    agb_board_keys(&board, 0x00A);
    CHECK(board.irq_if & AGB_IRQ_KEYPAD);
    board.irq_if = 0;
    agb_write16(&board, 0x04000132, 0xC000 | 0x003); /* A and B */
    agb_board_keys(&board, 0x001);
    CHECK(!(board.irq_if & AGB_IRQ_KEYPAD));
    agb_board_keys(&board, 0x003);
    CHECK(board.irq_if & AGB_IRQ_KEYPAD);
    board.irq_if = 0;
    agb_write16(&board, 0x04000132, 0x0003); /* the same keys, the interrupt off */
    agb_board_keys(&board, 0x003);
    CHECK(!(board.irq_if & AGB_IRQ_KEYPAD));
    agb_board_keys(&board, 0);

    /* a byte store to the I/O window writes its lane alone, the other as the register holds it written */
    board.irq_if = 0x0101;
    agb_write8(&board, 0x04000203, 0x01); /* IF+1 acknowledges bit 8 alone */
    CHECK(board.irq_if == 0x0001);
    agb_write16(&board, 0x04000100, 0x1234); /* TM0CNT_L sets the reload, and reads as the counter */
    board.timer[0].counter = 0xABCD;
    agb_write8(&board, 0x04000100, 0x56);
    CHECK(board.timer[0].reload == 0x1256);
    agb_write16(&board, 0x040000B0, 0x1234); /* DMA0SAD, write-only */
    agb_write8(&board, 0x040000B1, 0x56);
    CHECK((board.dma[0].source & 0xFFFF) == 0x5634);
    agb_write16(&board, 0x0400006C, 0x0123); /* SOUND2CNT_H: its frequency reads as 0 */
    agb_write8(&board, 0x0400006D, 0x04);
    CHECK((board.apu.cnt_x[1] & 0x7FF) == 0x423);
    agb_write16(&board, 0x0400007C, 0x8001); /* SOUND4CNT_H: a trigger acts once, the next byte store's lane has none */
    board.apu.enabled[3] = 0;
    agb_write8(&board, 0x0400007C, 0x02);
    CHECK(!board.apu.enabled[3] && (board.apu.cnt_x[3] & 0xFF) == 0x02);
    agb_write16(&board, 0x04000000, 0x0403); /* DISPCNT */
    agb_write8(&board, 0x04000001, 0x01);
    CHECK(board.ppu.dispcnt == 0x0103);

    /* SIOCNT with no cable plugged: the multi-player mode says a child (SI) and a ready line (SD), and a transfer ends at
       once with no partner's words and the serial interrupt; the normal mode's SI is pulled up */
    agb_write16(&board, 0x04000134, 0x0000); /* RCNT: the serial modes */
    board.irq_if = 0;
    agb_write16(&board, 0x04000128, 0x2003); /* multi-player, 115200 bauds */
    CHECK((agb_read16(&board, 0x04000128) & 0x000C) == 0x000C);
    agb_write16(&board, 0x04000120, 0x1234); /* SIOMULTI0, before the transfer */
    agb_write16(&board, 0x04000128, 0x6083); /* start, the interrupt asked for */
    CHECK(!(agb_read16(&board, 0x04000128) & 0x0080) && agb_read16(&board, 0x04000120) == 0);
    CHECK(board.irq_if & AGB_IRQ_SERIAL);
    board.irq_if = 0;
    agb_write16(&board, 0x04000128, 0x0001); /* normal 8 bit mode, internal clock */
    CHECK(agb_read16(&board, 0x04000128) & 0x0004);
    agb_write16(&board, 0x04000128, 0x0081); /* start, no interrupt */
    CHECK(!(agb_read16(&board, 0x04000128) & 0x0080) && !(board.irq_if & AGB_IRQ_SERIAL));

    /* the DMA channels' registers, 12 bytes apart from 0xB0: DMA3 copies at a 32-bit store of its count and control,
       as a game copies its interrupt handler to IWRAM, and DMA1's control is DMA1's alone */
    agb_write32(&board, 0x040000D4, 0x08000000); /* DMA3SAD */
    agb_write32(&board, 0x040000D8, 0x03000100); /* DMA3DAD */
    agb_write32(&board, 0x040000DC, 0x84000002); /* DMA3CNT: two words of 32 bits, now */
    CHECK(agb_read32(&board, 0x03000100) == agb_read32(&board, 0x08000000) &&
          agb_read32(&board, 0x03000104) == agb_read32(&board, 0x08000004));
    CHECK(board.dma[3].count == 2 && !(board.dma[3].control & 0x8000)); /* done: the enable bit clears */
    agb_write16(&board, 0x040000C6, 0xB640); /* DMA1CNT_H: the sound FIFO's timing, repeat, 32 bits */
    CHECK(board.dma[1].control == 0xB640 && !board.dma[0].control && !board.dma[2].control);
    CHECK(agb_read16(&board, 0x040000C6) == 0xB640);
    agb_board_destroy(&board);
    CHECK(!board.has_cartridge && !board.cartridge.data);

    free(rom);
    puts("agb_board: ok");
    return 0;
}
