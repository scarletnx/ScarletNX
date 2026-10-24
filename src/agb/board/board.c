/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/board/board.h"
#include <string.h>

void agb_board_init(AgbBoard* board) {
    memset(board, 0, sizeof(*board));
    agb_memory_init(&board->memory);
    board->bios_prefetch = AGB_BIOS_PREFETCH_BOOT;
    /* the registers as the BIOS leaves them for the cartridge, which the machine starts at: the screen in forced blank,
       the serial port in its general purpose mode, the sound's bias at its middle, the affine backgrounds unscaled and
       POSTFLG saying the boot is done. A game that only ORs bits into SOUNDBIAS keeps the bias */
    board->ppu.dispcnt = 0x0080;
    board->io[0x134 >> 1] = 0x8000;
    board->apu.soundbias = 0x200;
    board->ppu.bgpa[2] = board->ppu.bgpd[2] = board->ppu.bgpa[3] = board->ppu.bgpd[3] = 0x100;
    board->io[0x300 >> 1] = 1;
}

int agb_board_insert(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size) {
    AgbCartridge cartridge;
    if (agb_cartridge_load(&cartridge, image, size, error, error_size)) return -1;
    agb_cartridge_unload(&board->cartridge);
    board->cartridge = cartridge;
    board->has_cartridge = 1;
    agb_save_reset(board, agb_save_detect((const uint8_t*)image, size));
    return 0;
}

void agb_board_destroy(AgbBoard* board) {
    agb_cartridge_unload(&board->cartridge);
    agb_memory_free(&board->memory);
    memset(board, 0, sizeof(*board));
}

/* ---- the I/O window ---- */

/* the registers whose reads give the open bus, as mGBA's GBAIORead has them: the display's write-only ones (scrolls,
   the affine parameters, the windows' edges, MOSAIC, BLDY) and its unused ones, the sound FIFOs and the DMA addresses */
static int io_open(uint32_t byte) {
    return (byte >= 0x10 && byte <= 0x46) || byte == 0x4C || byte == 0x4E || (byte >= 0x54 && byte <= 0x5E) ||
           (byte >= 0xA0 && byte <= 0xA6) || (byte >= 0xB0 && byte <= 0xDE && (byte - 0xB0) % 12 < 8);
}

uint16_t agb_io_read16(AgbBoard* board, uint32_t address) {
    uint32_t offset = (address >> 1) & (AGB_IO_COUNT - 1), byte = offset * 2;
    if (io_open(byte)) return (uint16_t)board->open_bus;                   /* its low half, as GBAIORead gives it */
    if (byte >= 0xB8 && byte <= 0xDC && (byte - 0xB0) % 12 == 8) return 0; /* DMAxCNT_L */
    if (byte <= 0x5F) return agb_ppu_read(board, byte);
    if (byte >= 0x60 && byte <= 0xA7) return agb_apu_read(board, byte);
    if (byte >= 0xB0 && byte <= 0xDF) return agb_dma_read(board, byte);
    if (byte >= 0x100 && byte <= 0x10F) return agb_timer_read(board, byte);
    if (byte == 0x130) return (uint16_t)(~board->keys & 0x3FF); /* KEYINPUT: a key held reads 0 */
    if (byte == 0x200) return board->irq_ie;
    if (byte == 0x202) return board->irq_if;
    if (byte == 0x208) return board->irq_ime;
    return board->io[offset];
}

/* the register as written, for the lane a byte store leaves: a device's copy where a read gives another value */
static uint16_t io_written(AgbBoard* board, uint32_t byte, uint32_t offset) {
    if (byte <= 0x5F) return agb_ppu_read(board, byte);
    if (byte >= 0x60 && byte <= 0xA7) return agb_apu_written(board, byte);
    if (byte >= 0xB0 && byte <= 0xDF) return agb_dma_written(board, byte);
    if (byte >= 0x100 && byte <= 0x10F) return agb_timer_written(board, byte);
    if (byte == 0x200) return board->irq_ie;
    if (byte == 0x208) return board->irq_ime;
    return board->io[offset];
}

/* SIOCNT with no link cable plugged, as mGBA's machine without a link driver answers it: the multi-player mode's SI says
   a child with no parent and SD that the line is ready, the normal modes' SI is pulled up, and a transfer started ends
   at once (mGBA after the transfer's time) with the data no partner sent, 0, the start bit cleared and the serial
   interrupt raised when SIOCNT asks for it. A game that looks for a partner finds none and plays alone */
static void sio_write(AgbBoard* board, uint16_t value) {
    uint16_t old = board->io[0x128 >> 1], rcnt = board->io[0x134 >> 1];
    unsigned mode = ((rcnt & 0xC000) | (value & 0x3000)) >> 12;
    int multi = mode < 8 && (mode & 3) == 2, normal = mode < 8 && (mode & 3) < 2;
    if (multi) {
        value = (uint16_t)((value & 0xFF83) | (old & 0x00FC) | 0x0004); /* SI: a child, ID 0 */
        value &= (uint16_t)~0x0030;
        if ((value & 0x80) && !(old & 0x80)) {
            for (uint32_t r = 0x120; r <= 0x126; r += 2) board->io[r >> 1] = 0; /* no partner's word */
            value &= (uint16_t)~0x0080;
            if (value & 0x4000) agb_irq_request(board, AGB_IRQ_SERIAL);
        }
        value |= 0x0008; /* SD: ready */
    } else if (normal) {
        if ((value & 0x80) && !(old & 0x80)) {
            board->io[0x12A >> 1] = 0; /* SIODATA8, and SIODATA32's halves */
            board->io[0x120 >> 1] = board->io[0x122 >> 1] = 0;
            value &= (uint16_t)~0x0080;
            if (value & 0x4000) agb_irq_request(board, AGB_IRQ_SERIAL);
        }
        value |= 0x0004; /* SI pulled up */
    }
    board->io[0x128 >> 1] = value;
}

void agb_io_write16(AgbBoard* board, uint32_t address, uint16_t value, uint16_t mask) {
    uint32_t offset = (address >> 1) & (AGB_IO_COUNT - 1), byte = offset * 2;
    if (byte == 0x202) {
        board->irq_if &= (uint16_t)~(value & mask); /* one bits acknowledge the interrupt flags of the lanes written */
        return;
    }
    if (mask != 0xFFFF) value = (uint16_t)((value & mask) | (io_written(board, byte, offset) & ~mask));
    if (byte <= 0x5F) {
        agb_ppu_write(board, byte, value);
        return;
    }
    if (byte >= 0x60 && byte <= 0xA7) {
        agb_apu_write(board, byte, value);
        return;
    }
    if (byte >= 0xB0 && byte <= 0xDF) {
        agb_dma_write(board, byte, value);
        return;
    }
    if (byte >= 0x100 && byte <= 0x10F) {
        agb_timer_write(board, byte, value);
        return;
    }
    if (byte == 0x200) {
        board->irq_ie = value;
        return;
    }
    if (byte == 0x208) {
        board->irq_ime = value & 1;
        return;
    }
    if (byte == 0x128) {
        sio_write(board, value);
        return;
    }
    board->io[offset] = value;
}

void agb_board_keys(AgbBoard* board, uint16_t held) {
    /* KEYCNT (0x132): the keys of bits 0 to 9, any of them held (bit 15 clear) or all of them (set), with bit 14 */
    uint16_t control = board->io[0x132 >> 1], keys = control & 0x3FF;
    board->keys = held & 0x3FF;
    if ((control & 0x4000) && keys && ((control & 0x8000) ? (board->keys & keys) == keys : (board->keys & keys)))
        agb_irq_request(board, AGB_IRQ_KEYPAD);
}

void agb_irq_request(AgbBoard* board, uint32_t mask) {
    board->irq_if |= (uint16_t)mask;
    board->cpu_halted = 0; /* any raised flag wakes a halted CPU */
}
