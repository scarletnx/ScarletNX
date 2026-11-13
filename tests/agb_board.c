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
/* the flash's unlock, 0xAA at 0x5555 and 0x55 at 0x2AAA, then a command's byte at address */
static void flash_command(AgbBoard* board, uint32_t address, uint8_t command) {
    agb_write8(board, 0x0E005555, 0xAA);
    agb_write8(board, 0x0E002AAA, 0x55);
    agb_write8(board, 0x0E000000 + address, command);
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
/* the cartridge clock's serial line as the clock library drives it through the GPIO port: a byte out, from bit 0, each
   bit set on SIO with SCK low and taken as SCK rises, chip select high */
static void rtc_send(AgbBoard* board, uint8_t byte) {
    for (unsigned i = 0; i < 8; ++i) {
        uint16_t sio = (uint16_t)((byte >> i & 1) << 1);
        agb_write16(board, 0x080000C4, 4 | sio);
        agb_write16(board, 0x080000C4, 5 | sio);
    }
}
/* a byte in: SIO an input, each bit the clock's after SCK falls, from bit 0 */
static uint8_t rtc_receive(AgbBoard* board) {
    uint8_t byte = 0;
    for (unsigned i = 0; i < 8; ++i) {
        agb_write16(board, 0x080000C4, 4);
        byte |= (uint8_t)((agb_read16(board, 0x080000C4) >> 1 & 1) << i);
        agb_write16(board, 0x080000C4, 5);
    }
    return byte;
}
/* a command to the clock: chip select low then high with SCK high, its byte, then for a read SIO turned into an input */
static void rtc_command(AgbBoard* board, uint8_t command) {
    agb_write16(board, 0x080000C6, 7);
    agb_write16(board, 0x080000C4, 1);
    agb_write16(board, 0x080000C4, 5);
    rtc_send(board, command);
    if (command & 0x80) agb_write16(board, 0x080000C6, 5);
}
/* The real-time clock a cartridge with the clock library's tag has on its GPIO port: the date and time it reads, in BCD,
   its status register's 24 hour mode, the port's registers as they read back or not, and none without the tag */
static int check_rtc(void) {
    static const uint8_t date[7] = {0x26, 0x11, 0x12, 0x04, 0x22, 0x30, 0x45}; /* 2026-11-12, a Thursday, 22:30:45 */
    static const uint8_t leap[7] = {0x00, 0x02, 0x29, 0x02, 0x13, 0x00, 0x07}; /* 2000-02-29, a Tuesday, 13:00:07 */
    char error[256];
    uint8_t* rom = (uint8_t*)malloc(ROM_SIZE);
    AgbBoard board;
    CHECK(rom);
    make_rom(rom);
    memcpy(rom + 0x180, "SIIRTC_V001", 11);
    rom[0xC4] = 0x11, rom[0xC6] = 0x22, rom[0xC8] = 0x33;
    agb_board_init(&board);
    CHECK(!agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(board.gpio.rtc);
    /* before a write the port's registers read as the ROM's bytes */
    CHECK(agb_read16(&board, 0x080000C4) == 0x11 && agb_read8(&board, 0x080000C8) == 0x33);
    agb_write16(&board, 0x080000C8, 1); /* the port reads back */
    board.gpio.clock = 847837845;       /* 2026-11-12 22:30:45 */
    rtc_command(&board, 0xA6);          /* read the date and time */
    for (unsigned i = 0; i < 7; ++i) {
        uint8_t got = rtc_receive(&board);
        if (got != date[i]) {
            fprintf(stderr, "agb_board: the clock's byte %u is 0x%02X, not 0x%02X\n", i, got, date[i]);
            return 1;
        }
    }
    agb_write16(&board, 0x080000C4, 1); /* chip select low: the transfer ends */
    /* the registers read back in every cartridge window: SIO an input still, chip select low, SCK high */
    CHECK(agb_read16(&board, 0x080000C6) == 5 && agb_read16(&board, 0x0A0000C8) == 1 &&
          agb_read16(&board, 0x0C0000C4) == 3);
    board.gpio.clock = 5144407; /* 2000-02-29 13:00:07, a leap day */
    rtc_command(&board, 0xA6);
    for (unsigned i = 0; i < 7; ++i) CHECK(rtc_receive(&board) == leap[i]);
    /* the status register: 0x40 (24 hours) as the clock starts; written 0, the time reads its hour in 12 hours */
    rtc_command(&board, 0xC6);
    CHECK(rtc_receive(&board) == 0x40);
    rtc_command(&board, 0x46);
    rtc_send(&board, 0x00);
    rtc_command(&board, 0xC6);
    CHECK(rtc_receive(&board) == 0x00);
    rtc_command(&board, 0xE6); /* the time alone: hour, minute, second */
    CHECK(rtc_receive(&board) == 0x01 && rtc_receive(&board) == 0x00 && rtc_receive(&board) == 0x07);
    /* write-only, the port's registers read zeros */
    agb_write16(&board, 0x080000C8, 0);
    CHECK(agb_read16(&board, 0x080000C4) == 0 && agb_read16(&board, 0x080000C6) == 0);
    agb_board_destroy(&board);

    /* no tag, no clock: the store goes nowhere and the ROM's bytes stay */
    make_rom(rom);
    rom[0xC6] = 0x5A;
    agb_board_init(&board);
    CHECK(!agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(!board.gpio.rtc);
    agb_write16(&board, 0x080000C8, 1);
    agb_write16(&board, 0x080000C6, 7);
    CHECK(agb_read8(&board, 0x080000C6) == 0x5A && agb_read16(&board, 0x080000C8) == 0);
    agb_board_destroy(&board);
    free(rom);
    return 0;
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
    /* the registers answer at their own addresses only: a store past them, as a debug port a test ROM writes to on an
       emulator, leaves IE and DISPCNT alone and a read there gives the open bus; the memory control at 0x04000800, at
       each 64 KiB, keeps what was written */
    agb_write16(&board, 0x04000200, 0x0001);
    agb_write16(&board, 0x04FFF600, 0x654D);
    agb_write8(&board, 0x04000600, 0x44);
    agb_write32(&board, 0x04000400, 0x12345678);
    board.open_bus = 0xE3A01234;
    if (agb_read16(&board, 0x04000200) != 0x0001 || agb_read16(&board, 0x04000000) != 0x0080 ||
        agb_read16(&board, 0x04FFF600) != 0x1234 || agb_read32(&board, 0x04000400) != 0x12341234) {
        fprintf(stderr,
                "agb_board: stores past the registers left IE 0x%04X (0x0001) and DISPCNT 0x%04X (0x0080); "
                "0x04FFF600 reads 0x%04X (0x1234), 0x04000400 0x%08X (0x12341234)\n",
                agb_read16(&board, 0x04000200), agb_read16(&board, 0x04000000), agb_read16(&board, 0x04FFF600),
                agb_read32(&board, 0x04000400));
        return 1;
    }
    CHECK(agb_read32(&board, 0x04000800) == 0x0D000020);
    agb_write32(&board, 0x04000800, 0x0E000020);
    agb_write8(&board, 0x04120801, 0x01);
    if (agb_read32(&board, 0x04FF0800) != 0x0E000120 || agb_read16(&board, 0x04000000) != 0x0080) {
        fprintf(stderr, "agb_board: the memory control reads 0x%08X, not 0x0E000120, and DISPCNT 0x%04X (0x0080)\n",
                agb_read32(&board, 0x04FF0800), agb_read16(&board, 0x04000000));
        return 1;
    }
    agb_write16(&board, 0x04000200, 0);
    {
        /* each register written all ones reads back its readable bits, an unused one 0 or the open bus, as the
           console reads them in mGBA's test suite (io-read.c) */
        static const struct {
            uint32_t address;
            int32_t expected; /* -1: the open bus's low half */
        } reads[] = {
            {0x04000008, 0xDFFF}, {0x0400000C, 0xFFFF}, {0x04000048, 0x3F3F}, {0x04000050, 0x3FFF},
            {0x04000062, 0xFFC0}, {0x04000072, 0xE000}, {0x04000078, 0xFF00}, {0x04000082, 0x770F},
            {0x040000BA, 0xF7E0}, {0x040000DE, 0xFFE0}, {0x04000066, 0},      {0x04000136, 0},
            {0x04000206, 0},      {0x04000302, 0},      {0x0400008C, -1},     {0x040000A8, -1},
            {0x040000E0, -1},     {0x04000110, -1},
        };
        agb_write16(&board, 0x04000084, 0x0080); /* the sound on: its registers take writes */
        board.open_bus = 0xDEADDEAD;
        for (size_t i = 0; i < sizeof reads / sizeof reads[0]; ++i) {
            uint16_t expected = (uint16_t)(reads[i].expected < 0 ? 0xDEAD : reads[i].expected), got;
            agb_write16(&board, reads[i].address, 0xFFFF);
            got = agb_read16(&board, reads[i].address);
            agb_write16(&board, reads[i].address, 0);
            if (got != expected) {
                fprintf(stderr, "agb_board: 0x%08X written 0xFFFF reads 0x%04X, not 0x%04X\n", reads[i].address, got,
                        expected);
                return 1;
            }
        }
        agb_write16(&board, 0x04000084, 0);
    }
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
    rom[0xBD] ^= 0x40;
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "ROM header starts with 0x0000002E, not an ARM branch"));
    /* a whole header over no program, the data cartridge another console's game reads, and a flash memory's image,
       erased where a header would be: each refused saying what it is */
    make_rom(rom);
    memset(rom, 0xFF, 4);
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "ROM header (title \"TESTGAME\", code BZVE) is whole, but its first word is 0xFFFFFFFF, not an "
                        "ARM branch: a data cartridge a game reads, not a GBA program"));
    {
        uint8_t* flash = (uint8_t*)malloc(0x20000);
        CHECK(flash);
        memset(flash, 0xFF, 0x20000);
        flash[0x10000] = 0x12;
        CHECK(agb_board_insert(&board, flash, 0x20000, error, sizeof error));
        free(flash);
        CHECK(strstr(error, "ROM image of 131072 bytes starts with 0xFFFFFFFF and its header is erased, all 0xFF: a "
                            "flash memory's image"));
    }
    make_rom(rom);
    rom[0xB2] = 0x00;
    fix_checksum(rom);
    CHECK(agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
    CHECK(strstr(error, "ROM header's byte 0xB2 is 0x00, not the fixed 0x96"));

    /* an immediate DMA with the repeat bit ends as any immediate one, its enable bit clear, so the next start runs: a
       game that cleared OAM so and loaded its palettes after got none */
    agb_write32(&board, 0x02000200, 0x11223344);
    agb_write32(&board, 0x040000D4, 0x02000200);
    agb_write32(&board, 0x040000D8, 0x02000300);
    agb_write16(&board, 0x040000DC, 4);
    agb_write16(&board, 0x040000DE, 0x8700); /* on, now, words, repeat, the source fixed */
    CHECK(agb_read32(&board, 0x0200030C) == 0x11223344);
    if (agb_read16(&board, 0x040000DE) != 0x0700) {
        fprintf(stderr, "agb_board: an immediate repeat DMA left DMA3CNT_H at 0x%04X, not 0x0700\n",
                agb_read16(&board, 0x040000DE));
        return 1;
    }
    agb_write32(&board, 0x040000D4, 0x02000200);
    agb_write32(&board, 0x040000D8, 0x05000000);
    agb_write16(&board, 0x040000DC, 1);
    agb_write16(&board, 0x040000DE, 0x8400);
    CHECK(agb_read32(&board, 0x05000000) == 0x11223344);
    /* a DMA reads nothing below EWRAM: it moves the unit its channel read last, a halfword in both halves of a word,
       and DMA0, which reaches only the console's own memory, reads the cartridge's addresses there too */
    agb_write32(&board, 0x02000400, 0xABCD1234);
    dma3(&board, 0x02000402, 0x02000410, 1);     /* halfwords: its latch is 0xABCDABCD */
    agb_write32(&board, 0x040000D4, 0x00000010); /* the BIOS */
    agb_write32(&board, 0x040000D8, 0x02000420);
    agb_write32(&board, 0x040000DC, 0x84000002); /* two words, now */
    agb_write32(&board, 0x040000B0, 0x02000400);
    agb_write32(&board, 0x040000B4, 0x02000430);
    agb_write32(&board, 0x040000B8, 0x84000001); /* DMA0: a word, its latch 0xABCD1234 */
    agb_write32(&board, 0x040000B0, 0x08000000); /* the cartridge, which DMA0 does not reach */
    agb_write32(&board, 0x040000B4, 0x02000434);
    agb_write32(&board, 0x040000B8, 0x84000001);
    if (agb_read32(&board, 0x02000420) != 0xABCDABCD || agb_read32(&board, 0x02000424) != 0xABCDABCD ||
        agb_read32(&board, 0x02000434) != 0xABCD1234) {
        fprintf(stderr,
                "agb_board: a DMA from the BIOS moved 0x%08X 0x%08X (0xABCDABCD), DMA0 from the cartridge 0x%08X "
                "(0xABCD1234)\n",
                agb_read32(&board, 0x02000420), agb_read32(&board, 0x02000424), agb_read32(&board, 0x02000434));
        return 1;
    }
    agb_write32(&board, 0x040000B8, 0);
    /* a DMA from the cartridge counts its source up even when the control says fixed: two halfwords, not one twice */
    agb_write32(&board, 0x040000D4, 0x08000000);
    agb_write32(&board, 0x040000D8, 0x02000500);
    agb_write32(&board, 0x040000DC, 0x81000002); /* now, halfwords, the source fixed */
    if (agb_read32(&board, 0x02000500) != 0xEA00002E) {
        fprintf(stderr,
                "agb_board: a fixed DMA from the cartridge moved 0x%08X, not its first two halfwords 0xEA00002E\n",
                agb_read32(&board, 0x02000500));
        return 1;
    }

    /* a rejected insert leaves the accepted cartridge in place */
    CHECK(board.has_cartridge && !strcmp(board.cartridge.title, "TEST"));

    /* a header whose checksum is not its title block's runs, as mGBA runs it, where the console's BIOS stops: the
       cartridge keeps both for the runner to say */
    make_rom(rom);
    {
        uint8_t title = rom[0xBD];
        rom[0xBD] ^= 0x40;
        CHECK(!agb_board_insert(&board, rom, ROM_SIZE, error, sizeof error));
        CHECK(board.cartridge.checksum == (uint8_t)(title ^ 0x40) && board.cartridge.checksum_title == title);
    }

    /* the BIOS reads as the opcode it fetched last, at the read's lane, and ignores writes */
    CHECK(agb_read32(&board, 0x00000000) == AGB_BIOS_PREFETCH_BOOT);
    CHECK(agb_read16(&board, 0x00000102) == (AGB_BIOS_PREFETCH_BOOT >> 16));
    CHECK(agb_read8(&board, 0x00003FFD) == (uint8_t)(AGB_BIOS_PREFETCH_BOOT >> 8));
    agb_write32(&board, 0x00000000, 0);
    board.bios_prefetch = AGB_BIOS_PREFETCH_SWI;
    CHECK(agb_read32(&board, 0x00000000) == AGB_BIOS_PREFETCH_SWI);
    /* where the console decodes nothing, past the BIOS up to EWRAM and past the 28 bits (0x8xxxxxxx too), a read gives
       the open bus at its lane and a write goes nowhere, as mGBA's GBALoadBad */
    board.open_bus = 0x0A000005;
    CHECK(agb_read32(&board, 0x2E00A610) == 0x0A000005 && agb_read16(&board, 0xFFFFFFFE) == 0x0A00 &&
          agb_read8(&board, 0x10000001) == 0x00 && agb_read8(&board, 0x7FFFFFF3) == 0x0A);
    CHECK(agb_read32(&board, 0x00004000) == 0x0A000005 && agb_read16(&board, 0x01FFFFFE) == 0x0A00 &&
          agb_read8(&board, 0x80000000) == 0x05);
    agb_write32(&board, 0x2E00A610, 0x12345678);
    agb_write8(&board, 0x9E000000, 0x77);
    CHECK(agb_read32(&board, 0x2E00A610) == 0x0A000005);
    /* bit 31 mirrors nothing: a word there reads the open bus, not EWRAM's, and a store of any width there reaches
       neither EWRAM nor the save memory */
    agb_save_reset(&board, AGB_SAVE_SRAM);
    agb_write32(&board, 0x02000040, 0x5A5A5A5A);
    {
        uint8_t saved[2] = {agb_read8(&board, 0x0E000040), agb_read8(&board, 0x0E000044)};
        agb_write32(&board, 0x82000040, 0x11111111);
        agb_write16(&board, 0x82000044, 0x2222);
        agb_write8(&board, 0x8E000040, 0x33);
        CHECK(agb_read32(&board, 0x82000040) == 0x0A000005 && agb_read32(&board, 0x02000040) == 0x5A5A5A5A);
        CHECK(agb_read8(&board, 0x0E000040) == saved[0] && agb_read8(&board, 0x0E000044) == saved[1] &&
              !board.save.dirty);
    }

    /* the cartridge windows hold the image once, and past it the bus still holds address / 2 */
    CHECK(agb_read32(&board, 0x08000000) == board.cartridge.entry);
    CHECK(agb_read16(&board, 0x08000000 + ROM_SIZE) == ROM_SIZE / 2);
    CHECK(agb_read8(&board, 0x08000001 + ROM_SIZE) == (ROM_SIZE / 2) >> 8);
    CHECK(agb_read32(&board, 0x0C000000 + 0x123454) == (0x123454u / 2 | (0x123456u / 2) << 16));
    CHECK(agb_read16(&board, 0x09FFFFFE) == 0xFFFF);

    /* a 32 KiB SRAM repeats twice in the 64 KiB window, which repeats over 0x0E000000 to 0x0FFFFFFF */
    agb_save_reset(&board, AGB_SAVE_SRAM);
    /* a DMA of a word to an address that is not one puts the word's low byte at the word's address, the bits below
       its width dropped */
    agb_write32(&board, 0x03000300, 0xA5B6C7D8);
    agb_write8(&board, 0x0E000100, 0x66);
    agb_write8(&board, 0x0E000101, 0x66);
    agb_write32(&board, 0x040000D4, 0x03000300);
    agb_write32(&board, 0x040000D8, 0x0E000101);
    agb_write32(&board, 0x040000DC, 0x84000001);
    if (agb_read8(&board, 0x0E000100) != 0xD8 || agb_read8(&board, 0x0E000101) != 0x66) {
        fprintf(stderr, "agb_board: a word DMA to 0x0E000101 left 0x%02X 0x%02X there, not 0xD8 0x66\n",
                agb_read8(&board, 0x0E000100), agb_read8(&board, 0x0E000101));
        return 1;
    }
    /* its 8 bit bus reads the byte the whole address names, however wide the read */
    agb_write8(&board, 0x0E000201, 0x61);
    if (agb_read16(&board, 0x0E000201) != 0x6161 || agb_read32(&board, 0x0E000201) != 0x61616161) {
        fprintf(stderr, "agb_board: SRAM at 0x0E000201 reads 0x%04X and 0x%08X, not 0x6161 and 0x61616161\n",
                agb_read16(&board, 0x0E000201), agb_read32(&board, 0x0E000201));
        return 1;
    }
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
    /* one flash command waits at a time: a reset (0xF0) or another command ends an erase's setup, and a sector's
       erase is its 0x30 after a new unlock, never a lone 0x30 */
    agb_save_reset(&board, AGB_SAVE_FLASH64);
    memset(board.memory.sram, 0x11, 0x10000);
    flash_command(&board, 0x5555, 0x80);
    flash_command(&board, 0x5555, 0xF0);
    agb_write8(&board, 0x0E003000, 0x30);
    CHECK(board.memory.sram[0x3000] == 0x11);
    flash_command(&board, 0x5555, 0x80);
    flash_command(&board, 0x5555, 0xA0);
    agb_write8(&board, 0x0E001000, 0x10);
    CHECK(board.memory.sram[0x1000] == 0x10);
    agb_write8(&board, 0x0E003000, 0x30);
    CHECK(board.memory.sram[0x3000] == 0x11);
    flash_command(&board, 0x5555, 0x80);
    agb_write8(&board, 0x0E003000, 0x30);
    CHECK(board.memory.sram[0x3000] == 0x11);
    flash_command(&board, 0x5555, 0x80);
    flash_command(&board, 0x3000, 0x30);
    CHECK(board.memory.sram[0x3000] == 0xFF && board.memory.sram[0x3FFF] == 0xFF);
    CHECK(board.memory.sram[0x2FFF] == 0x11 && board.memory.sram[0x4000] == 0x11);

    /* the save kind a ROM's tag names, wherever it is, the image's last bytes too */
    {
        uint8_t image[64] = {0};
        memcpy(image + 20, "SRAM_V113", 9);
        CHECK(agb_save_detect(image, sizeof image) == AGB_SAVE_SRAM);
        memset(image, 0, sizeof image);
        memcpy(image + sizeof image - 8, "EEPROM_V", 8);
        CHECK(agb_save_detect(image, sizeof image) == AGB_SAVE_EEPROM);
        CHECK(agb_save_detect(image, sizeof image - 1) == AGB_SAVE_NONE); /* cut short of the tag's end */
    }

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
    agb_timer_step(&board, 1); /* a timer takes a write a cycle after it */
    CHECK(board.timer[0].reload == 0x1256);
    /* timer 0 has no timer before it: its count-up bit does nothing and it counts the clock, as a game that sets that
       bit and plays its sound FIFOs at timer 0's rate needs; timer 1's counts timer 0's overflows */
    agb_write16(&board, 0x04000100, 0xFF00);
    agb_write16(&board, 0x04000104, 0xFFFE);
    agb_write16(&board, 0x04000106, 0x0084); /* timer 1 on, counting up */
    agb_write16(&board, 0x04000102, 0x0084); /* timer 0 on, its count-up bit set */
    /* the write lands a cycle later and the count starts a cycle after that */
    agb_timer_step(&board, 0x102);
    CHECK(agb_read16(&board, 0x04000100) == 0xFF00 && agb_read16(&board, 0x04000104) == 0xFFFF);
    agb_timer_step(&board, 0x80);
    CHECK(agb_read16(&board, 0x04000100) == 0xFF80 && agb_read16(&board, 0x04000104) == 0xFFFF);
    agb_write16(&board, 0x04000102, 0x0000);
    agb_write16(&board, 0x04000106, 0x0000);
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

    /* a halted CPU waits while IE and IF share no interrupt: a blank's flag with only a timer's enabled leaves it
       halted, the timer's wakes it */
    board.irq_if = 0;
    agb_write16(&board, 0x04000200, AGB_IRQ_TIMER(1)); /* IE */
    board.cpu_halted = 1;
    agb_irq_request(&board, AGB_IRQ_VBLANK);
    CHECK(board.cpu_halted);
    agb_irq_request(&board, AGB_IRQ_TIMER(1));
    CHECK(!board.cpu_halted);
    agb_write16(&board, 0x04000200, 0);
    board.irq_if = 0;

    /* SIOCNT with no cable plugged: the multi-player mode says a child (SI) and a ready line (SD), and a transfer stays
       busy for its time (mGBA's, which the console's start bit read right after shows in its suite's sio-read.c), then
       ends with no partner's words and the serial interrupt; the normal mode's SI is pulled up */
    agb_write16(&board, 0x04000134, 0x0000); /* RCNT: the serial modes */
    board.irq_if = 0;
    agb_write16(&board, 0x04000128, 0x2003); /* multi-player, 115200 bauds */
    CHECK((agb_read16(&board, 0x04000128) & 0x000C) == 0x000C);
    agb_write16(&board, 0x04000128, 0x6083); /* start, the interrupt asked for */
    CHECK((agb_read16(&board, 0x04000128) & 0x0080) && !(board.irq_if & AGB_IRQ_SERIAL));
    agb_sio_step(&board, 3139);
    CHECK(agb_read16(&board, 0x04000128) & 0x0080);
    agb_sio_step(&board, 1);
    CHECK(!(agb_read16(&board, 0x04000128) & 0x0080) && agb_read16(&board, 0x04000120) == 0);
    CHECK(board.irq_if & AGB_IRQ_SERIAL);
    board.irq_if = 0;
    agb_write16(&board, 0x04000128, 0x0001); /* normal 8 bit mode, internal clock at 256 KiHz */
    CHECK(agb_read16(&board, 0x04000128) & 0x0004);
    agb_write16(&board, 0x04000128, 0x0081); /* start, no interrupt: 8 bits of 64 cycles */
    if (!(agb_read16(&board, 0x04000128) & 0x0080) || board.sio_cycles != 512) {
        fprintf(stderr,
                "agb_board: a normal 8 bit transfer reads SIOCNT 0x%04X and takes %u cycles, not busy for 512\n",
                agb_read16(&board, 0x04000128), board.sio_cycles);
        return 1;
    }
    agb_sio_step(&board, 512);
    CHECK(!(agb_read16(&board, 0x04000128) & 0x0080) && !(board.irq_if & AGB_IRQ_SERIAL));
    {
        /* each serial register written all ones in each mode reads back what the console keeps (mGBA's suite's
           sio-read.c): the data halfwords only where the mode has them, SIOCNT and RCNT without their unused bits and
           RCNT's lines at the idle port's levels, JOYCNT's interrupt enable alone, JOY_RECV and JOY_TRANS 0 */
        static const struct {
            uint16_t rcnt, siocnt;
            uint32_t address;
            uint16_t expected;
        } reads[] = {
            {0x0000, 0x1000, 0x04000120, 0xFFFF}, {0x0000, 0x0000, 0x04000120, 0x0000},
            {0x0000, 0x2000, 0x04000120, 0x0000}, {0x0000, 0x2000, 0x04000124, 0x0000},
            {0x0000, 0x3000, 0x04000128, 0x7FAF}, {0x8000, 0x0000, 0x04000128, 0x4F8F},
            {0x0000, 0x3000, 0x0400012A, 0x0000}, {0x0000, 0x0000, 0x0400012A, 0xFFFF},
            {0x0000, 0x0000, 0x04000134, 0x01F5}, {0x0000, 0x2000, 0x04000134, 0x01FF},
            {0x8000, 0x0000, 0x04000134, 0x81FF}, {0xC000, 0x0000, 0x04000134, 0xC1FC},
            {0x0000, 0x0000, 0x04000140, 0x0040}, {0xC000, 0x0000, 0x04000150, 0x0000},
            {0xC000, 0x0000, 0x04000156, 0x0000},
        };
        for (size_t i = 0; i < sizeof reads / sizeof reads[0]; ++i) {
            uint16_t value = 0xFFFF, got;
            agb_write16(&board, 0x04000134, reads[i].rcnt);
            agb_write16(&board, 0x04000128, reads[i].siocnt);
            if (reads[i].address == 0x04000128) value = (uint16_t)(reads[i].siocnt | 0xCFFF);
            if (reads[i].address == 0x04000134) value = (uint16_t)(reads[i].rcnt | 0x3FFF);
            agb_write16(&board, reads[i].address, value);
            got = agb_read16(&board, reads[i].address);
            if (got != reads[i].expected) {
                fprintf(stderr,
                        "agb_board: RCNT 0x%04X SIOCNT 0x%04X: 0x%08X written 0x%04X reads 0x%04X, not 0x%04X\n",
                        reads[i].rcnt, reads[i].siocnt, reads[i].address, value, got, reads[i].expected);
                return 1;
            }
            agb_write16(&board, reads[i].address, 0);
        }
        agb_write16(&board, 0x04000134, 0x8000);
        agb_write16(&board, 0x04000128, 0);
    }

    /* the DMA channels' registers, 12 bytes apart from 0xB0: DMA3 copies at a 32-bit store of its count and control,
       as a game copies its interrupt handler to IWRAM, and DMA1's control is DMA1's alone */
    agb_write32(&board, 0x040000D4, 0x08000000); /* DMA3SAD */
    agb_write32(&board, 0x040000D8, 0x03000100); /* DMA3DAD */
    agb_write32(&board, 0x040000DC, 0x84000002); /* DMA3CNT: two words of 32 bits, now */
    CHECK(agb_read32(&board, 0x03000100) == agb_read32(&board, 0x08000000) &&
          agb_read32(&board, 0x03000104) == agb_read32(&board, 0x08000004));
    CHECK(board.dma[3].count == 2 && !(board.dma[3].control & 0x8000)); /* done: the enable bit clears */
    /* a DMA holds the bus for 2N + 2(n-1)S + 2I: four words from the cartridge (WAITCNT 0x4317: a halfword's first
       access 4 cycles, the next 2) to IWRAM 2 + (6 + 1) + 3 x (4 + 1) = 24, four halfwords EWRAM to EWRAM
       2 + 4 x (3 + 3) = 26 */
    agb_write16(&board, 0x04000204, 0x4317);
    board.held_cycles = 0;
    agb_write32(&board, 0x040000D4, 0x08000000);
    agb_write32(&board, 0x040000D8, 0x03000200);
    agb_write32(&board, 0x040000DC, 0x84000004);
    CHECK(board.held_cycles == 24);
    board.held_cycles = 0;
    agb_write32(&board, 0x040000D4, 0x02000000);
    agb_write32(&board, 0x040000D8, 0x02000100);
    agb_write32(&board, 0x040000DC, 0x80000004);
    CHECK(board.held_cycles == 26);
    /* only the first access on the cartridge is a first one: four words from it to it, 2 + (6 + 4) + 3 x (4 + 4) */
    board.held_cycles = 0;
    agb_write32(&board, 0x040000D4, 0x08000000);
    agb_write32(&board, 0x040000D8, 0x08000100);
    agb_write32(&board, 0x040000DC, 0x84000004);
    CHECK(board.held_cycles == 36);
    board.held_cycles = 0;
    agb_write16(&board, 0x04000204, 0);
    agb_write16(&board, 0x040000C6, 0xB640); /* DMA1CNT_H: the sound FIFO's timing, repeat, 32 bits */
    CHECK(board.dma[1].control == 0xB640 && !board.dma[0].control && !board.dma[2].control);
    CHECK(agb_read16(&board, 0x040000C6) == 0xB640);
    /* a channel takes its count as it starts: one written while it waits for its blank is the next repeat's, as the
       suite's misc-edge.c finds on the console (two halfwords, then four) */
    for (uint32_t i = 0; i < 8; ++i) agb_write16(&board, 0x02000300 + i * 2, (uint16_t)i);
    agb_write32(&board, 0x040000D4, 0x02000300);
    agb_write32(&board, 0x040000D8, 0x02000400);
    agb_write32(&board, 0x040000DC, 0x12400002); /* the vertical blank, repeat, halfwords, a fixed destination */
    agb_write16(&board, 0x040000DE, (uint16_t)(agb_read16(&board, 0x040000DE) | 0x8000));
    agb_write16(&board, 0x040000DC, 4);
    agb_dma_event(&board, AGB_DMA_VBLANK);
    if (agb_read16(&board, 0x02000400) != 1) {
        fprintf(stderr, "agb_board: the first blank's DMA ends at halfword %u, not 1\n",
                agb_read16(&board, 0x02000400));
        return 1;
    }
    agb_dma_event(&board, AGB_DMA_VBLANK);
    if (agb_read16(&board, 0x02000400) != 5) {
        fprintf(stderr, "agb_board: the repeat ends at halfword %u, not 5\n", agb_read16(&board, 0x02000400));
        return 1;
    }
    agb_write16(&board, 0x040000DE, 0);
    /* DMA0 to DMA2 count 14 bits: 0xC001 is one unit */
    agb_write16(&board, 0x02000502, 0xFFFF);
    agb_write32(&board, 0x040000B0, 0x02000302);
    agb_write32(&board, 0x040000B4, 0x02000500);
    agb_write32(&board, 0x040000B8, 0x8000C001);
    CHECK(agb_read16(&board, 0x02000500) == 1 && agb_read16(&board, 0x02000502) == 0xFFFF);
    agb_board_destroy(&board);
    CHECK(!board.has_cartridge && !board.cartridge.data);

    free(rom);
    if (check_rtc()) return 1;
    puts("agb_board: ok");
    return 0;
}
