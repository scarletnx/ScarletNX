/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/board/board.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include <stdio.h>
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
    board->memcnt[0] = 0x0020, board->memcnt[1] = 0x0D00; /* GBATEK's 0x0D000020: 256 KiB of EWRAM, 2 wait states */
    /* the BIOS's boot ends on line 126, 919 cycles into it, where mGBA's machine starts the cartridge when it skips the
       BIOS (line 0x7E, its first horizontal blank at cycle 1008 of the line 120 cycles away, then 31 cycles of its reset
       code): a game that turns its interrupts on as it boots meets the first vertical blank 34 lines later, not 160 */
    board->ppu.vcount = AGB_BOOT_LINE;
    board->ppu.dot = AGB_BOOT_DOT;
    agb_gpio_reset(board, 0);
}

int agb_board_insert(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size) {
    AgbCartridge cartridge;
    if (agb_cartridge_load(&cartridge, image, size, error, error_size)) return -1;
    agb_cartridge_unload(&board->cartridge);
    board->cartridge = cartridge;
    board->has_cartridge = 1;
    agb_save_reset(board, agb_save_detect((const uint8_t*)image, size));
    agb_gpio_reset(board, agb_gpio_detect((const uint8_t*)image, size));
    return 0;
}

int agb_board_multiboot(AgbBoard* board, const void* image, size_t size, char* error, size_t error_size) {
    if (!size || size > AGB_EWRAM_SIZE) {
        snprintf(error, error_size,
                 "the multiboot program is %zu bytes: EWRAM, where the BIOS receives one, holds 1 to %u", size,
                 (unsigned)AGB_EWRAM_SIZE);
        return -1;
    }
    memcpy(board->memory.ewram, image, size);
    board->multiboot_size = (uint32_t)size;
    board->multiboot_hash = snx_fnv1a(SNX_FNV_BASIS, image, size);
    return 0;
}

uint32_t agb_board_entry(const AgbBoard* board) {
    /* with no cartridge, mGBA's machine starts a program at 0x020000C0 when that word is not zero, else at EWRAM's
       start, where a program whose header has no entry begins */
    if (board->has_cartridge || !board->multiboot_size) return 0x08000000;
    return snx_ld_le32(board->memory.ewram + 0xC0) ? 0x020000C0 : 0x02000000;
}

void agb_board_destroy(AgbBoard* board) {
    agb_cartridge_unload(&board->cartridge);
    agb_memory_free(&board->memory);
    memset(board, 0, sizeof(*board));
}

/* ---- the I/O window ---- */

/* What a read of a register gives besides its value: the registers the console reads back, the unused halfwords it
   reads as 0 (the sound's gaps, DMAxCNT_L, a few past the serial port and the interrupts), and the open bus for every
   other address: the write-only ones (scrolls, the affine parameters, the windows' edges, MOSAIC, BLDY, the FIFOs, the
   DMA addresses) and the unused ones. mGBA's GBAIORead has these, and its test suite's io-read.c the console's */
enum { IO_OPEN, IO_ZERO, IO_READ };
static int io_kind(uint32_t byte) {
    static const uint16_t zero[] = {0x066, 0x06A, 0x06E, 0x076, 0x07A, 0x07E, 0x086, 0x08A, 0x0B8,
                                    0x0C4, 0x0D0, 0x0DC, 0x136, 0x142, 0x15A, 0x206, 0x20A, 0x302};
    static const uint16_t read[][2] = {{0x000, 0x00E}, {0x048, 0x04A}, {0x050, 0x052}, {0x060, 0x064}, {0x068, 0x068},
                                       {0x06C, 0x06C}, {0x070, 0x074}, {0x078, 0x078}, {0x07C, 0x07C}, {0x080, 0x084},
                                       {0x088, 0x088}, {0x090, 0x09E}, {0x0BA, 0x0BA}, {0x0C6, 0x0C6}, {0x0D2, 0x0D2},
                                       {0x0DE, 0x0DE}, {0x100, 0x10E}, {0x120, 0x12A}, {0x130, 0x134}, {0x140, 0x140},
                                       {0x150, 0x158}, {0x200, 0x204}, {0x208, 0x208}, {0x300, 0x300}};
    for (size_t i = 0; i < sizeof read / sizeof read[0]; ++i)
        if (byte >= read[i][0] && byte <= read[i][1]) return IO_READ;
    for (size_t i = 0; i < sizeof zero / sizeof zero[0]; ++i)
        if (byte == zero[i]) return IO_ZERO;
    return IO_OPEN;
}

/* the bits of a register a read gives back, the rest reading 0: BG0CNT's and BG1CNT's wrap bit, the windows' and
   blending's unused bits, the sound's write-only lengths and FIFO resets, the DMA controls' unused bits and DMA0-2's
   cartridge request (the suite's io-read.c, from the console) */
static uint16_t io_readable(uint32_t byte) {
    static const uint16_t masks[][2] = {{0x008, 0xDFFF}, {0x00A, 0xDFFF}, {0x048, 0x3F3F}, {0x04A, 0x3F3F},
                                        {0x050, 0x3FFF}, {0x052, 0x1F1F}, {0x062, 0xFFC0}, {0x068, 0xFFC0},
                                        {0x072, 0xE000}, {0x078, 0xFF00}, {0x082, 0x770F}, {0x0BA, 0xF7E0},
                                        {0x0C6, 0xF7E0}, {0x0D2, 0xF7E0}, {0x0DE, 0xFFE0}};
    for (size_t i = 0; i < sizeof masks / sizeof masks[0]; ++i)
        if (byte == masks[i][0]) return masks[i][1];
    return 0xFFFF;
}

/* The serial port's mode: RCNT's bits 15-14 pick general purpose (10) or JOY Bus (11), else SIOCNT's bits 13-12 the
   normal 8 bit, normal 32 bit, multi-player or UART one */
enum { SIO_NORMAL8, SIO_NORMAL32, SIO_MULTI, SIO_UART, SIO_GPIO, SIO_JOY };
static int sio_mode(const AgbBoard* board) {
    uint16_t rcnt = board->io[0x134 >> 1];
    if (rcnt & 0x8000) return rcnt & 0x4000 ? SIO_JOY : SIO_GPIO;
    return board->io[0x128 >> 1] >> 12 & 3;
}

/* The serial registers as the console reads them back with no cable (the suite's sio-read.c): the data halfwords only
   in the modes that have them, SIOCNT without its unused bits, RCNT's lines at the levels the idle port gives them
   (in general purpose mode the ones written), JOYCNT's interrupt enable and flags, JOY_TRANS reading 0 */
static uint16_t sio_read(AgbBoard* board, uint32_t byte, uint32_t offset) {
    int mode = sio_mode(board);
    uint16_t value = board->io[offset];
    switch (byte) {
        case 0x120:
        case 0x122: return mode == SIO_NORMAL32 || mode == SIO_MULTI ? value : 0;
        case 0x124:
        case 0x126: return mode == SIO_MULTI ? value : 0;
        case 0x128: return value & (mode == SIO_UART ? 0x7FAF : 0x7F8F);
        case 0x12A: return mode == SIO_UART ? 0 : value;
        case 0x134: {
            static const uint16_t lines[] = {0x5, 0x5, 0xF, 0xF, 0x0, 0xC};
            return (uint16_t)((value & 0xC1F0) | (mode == SIO_GPIO ? value & 0xF : lines[mode]));
        }
        case 0x140: return value & 0x0047;
        case 0x154:
        case 0x156: return 0;
        default: return value;
    }
}

/* the register's value as the devices hold it */
static uint16_t io_value(AgbBoard* board, uint32_t byte, uint32_t offset) {
    if (byte <= 0x5F) return agb_ppu_read(board, byte);
    if (byte >= 0x60 && byte <= 0xA7) return agb_apu_read(board, byte);
    if (byte >= 0xB0 && byte <= 0xDF) return agb_dma_read(board, byte);
    if (byte >= 0x100 && byte <= 0x10F) return agb_timer_read(board, byte);
    if (byte == 0x130) return (uint16_t)(~board->keys & 0x3FF); /* KEYINPUT: a key held reads 0 */
    if (byte == 0x200) return board->irq_ie;
    if (byte == 0x202) return board->irq_if;
    if (byte == 0x208) return board->irq_ime;
    if ((byte >= 0x120 && byte <= 0x12A) || byte == 0x134 || byte == 0x140 || (byte >= 0x150 && byte <= 0x158))
        return sio_read(board, byte, offset);
    return board->io[offset];
}

/* The window decodes its registers at 0x04000000-0x040003FE and the internal memory control at 0x04000800, repeated
   every 64 KiB (GBATEK, "Internal Memory Control"). Nothing else answers there: a read gives the open bus and a write
   goes nowhere, as mGBA's GBAIORead and GBAIOWrite have it past GBA_REG_MAX. A register's address with the window's
   other bits set is not that register */
static int io_unused(uint32_t address) { return (address & 0xFFFFFF) >= 0x400 && (address & 0xFFFC) != 0x800; }

uint16_t agb_io_read16(AgbBoard* board, uint32_t address) {
    uint32_t offset = (address >> 1) & (AGB_IO_COUNT - 1), byte = offset * 2;
    if (io_unused(address)) return (uint16_t)board->open_bus;
    if ((address & 0xFFFFFF) >= 0x400) return board->memcnt[address >> 1 & 1];
    switch (io_kind(byte)) {
        case IO_OPEN: return (uint16_t)board->open_bus; /* its low half, as GBAIORead gives it */
        case IO_ZERO: return 0;
        default: return io_value(board, byte, offset) & io_readable(byte);
    }
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
   a child with no parent and SD that the line is ready, the normal modes' SI is pulled up. A transfer started keeps the
   start bit set for the transfer's time (agb_sio_step), then ends with the data no partner sent, 0, and the serial
   interrupt when SIOCNT asks for it. A game that looks for a partner finds none and plays alone */
static void sio_write(AgbBoard* board, uint16_t value) {
    uint16_t old = board->io[0x128 >> 1], rcnt = board->io[0x134 >> 1];
    unsigned mode = ((rcnt & 0xC000) | (value & 0x3000)) >> 12;
    int multi = mode < 8 && (mode & 3) == 2, normal = mode < 8 && (mode & 3) < 2;
    if (multi) {
        value = (uint16_t)((value & 0xFF83) | (old & 0x00FC) | 0x0004); /* SI: a child, ID 0 */
        value &= (uint16_t)~0x0030;
        value |= 0x0008; /* SD: ready */
    } else if (normal)
        value |= 0x0004; /* SI pulled up */
    if ((multi || normal) && (value & 0x80) && !(old & 0x80)) {
        /* mGBA's GBASIOTransferCycles with no partner: a multi-player word at its baud rate (bits 0-1), else 8 or 32
           bits at 256 KiHz or, with bit 1, 2 MiHz */
        static const uint32_t multi_cycles[4] = {31976, 8378, 5750, 3140};
        board->sio_cycles = multi ? multi_cycles[value & 3] : (mode & 1 ? 32u : 8u) * (value & 2 ? 8u : 64u);
    } else if (!(value & 0x80))
        board->sio_cycles = 0; /* the start bit cleared: the transfer stops */
    board->io[0x128 >> 1] = value;
}

void agb_sio_step(AgbBoard* board, uint32_t cycles) {
    if (!board->sio_cycles) return;
    if (cycles < board->sio_cycles) {
        board->sio_cycles -= cycles;
        return;
    }
    board->sio_cycles = 0;
    uint16_t siocnt = board->io[0x128 >> 1];
    if (sio_mode(board) == SIO_MULTI)
        for (uint32_t r = 0x120; r <= 0x126; r += 2) board->io[r >> 1] = 0; /* no partner's word */
    else {
        board->io[0x12A >> 1] = 0; /* SIODATA8, and SIODATA32's halves */
        board->io[0x120 >> 1] = board->io[0x122 >> 1] = 0;
    }
    board->io[0x128 >> 1] = (uint16_t)(siocnt & ~0x0080);
    if (siocnt & 0x4000) agb_irq_request(board, AGB_IRQ_SERIAL);
}

/* the interrupt line: IME on and an interrupt both IE and IF hold */
static int irq_line(const AgbBoard* board) { return (board->irq_ime & 1) && (board->irq_ie & board->irq_if & 0x3FFF); }

void agb_io_write16(AgbBoard* board, uint32_t address, uint16_t value, uint16_t mask) {
    uint32_t offset = (address >> 1) & (AGB_IO_COUNT - 1), byte = offset * 2;
    if (io_unused(address)) return;
    if ((address & 0xFFFFFF) >= 0x400) {
        /* kept as written: the wait states and the RAM it switches are not the board's */
        uint16_t* half = &board->memcnt[address >> 1 & 1];
        *half = (uint16_t)((value & mask) | (*half & ~mask));
        return;
    }
    if (byte == 0x202) {
        board->irq_if &= (uint16_t)~(value & mask); /* one bits acknowledge the interrupt flags of the lanes written */
        return;
    }
    if (byte == 0x200 || byte == 0x208) {
        /* the CPU's line rises 3 cycles after a write that lets a held interrupt through */
        int was = irq_line(board);
        if (mask != 0xFFFF) value = (uint16_t)((value & mask) | (io_written(board, byte, offset) & ~mask));
        if (byte == 0x200)
            board->irq_ie = value;
        else
            board->irq_ime = value & 1;
        if (!was && irq_line(board)) board->irq_at = board->now + 3;
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
    if (byte == 0x128) {
        sio_write(board, value);
        return;
    }
    /* what the port received is not the CPU's to write: the multi-player words and JOY_RECV; JOYCNT's flags clear
       where a one is written */
    if ((byte >= 0x120 && byte <= 0x126 && sio_mode(board) == SIO_MULTI) || byte == 0x150 || byte == 0x152) return;
    if (byte == 0x140) value = (uint16_t)((board->io[offset] & 0x0007 & ~value) | (value & 0x0040));
    board->io[offset] = value;
}

void agb_board_keys(AgbBoard* board, uint16_t held) {
    /* KEYCNT (0x132): the keys of bits 0 to 9, any of them held (bit 15 clear) or all of them (set), with bit 14 */
    uint16_t control = board->io[0x132 >> 1], keys = control & 0x3FF;
    board->keys = held & 0x3FF;
    if ((control & 0x4000) && keys && ((control & 0x8000) ? (board->keys & keys) == keys : (board->keys & keys)))
        agb_irq_request(board, AGB_IRQ_KEYPAD);
}

void agb_irq_raise(AgbBoard* board, uint32_t mask, uint64_t at) {
    int was = irq_line(board);
    board->irq_if |= (uint16_t)mask;
    board->cpu_halted = 0; /* any raised flag wakes a halted CPU */
    if (!was && irq_line(board)) board->irq_at = at + 3;
}

void agb_irq_request(AgbBoard* board, uint32_t mask) { agb_irq_raise(board, mask, board->now); }

uint64_t agb_board_next_irq(const AgbBoard* board) {
    uint64_t at = agb_timer_next_irq(board), display = agb_ppu_next_irq(board);
    if (display < at) at = display;
    /* the serial transfer ends with its interrupt when SIOCNT asks for one */
    if (board->sio_cycles && (board->io[0x128 >> 1] & 0x4000) && (board->irq_ie & AGB_IRQ_SERIAL) &&
        board->now + board->sio_cycles < at)
        at = board->now + board->sio_cycles;
    return at == UINT64_MAX ? at : at + 3;
}
