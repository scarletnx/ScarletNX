/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the AGB DMA controller: four channels moving memory on demand, for the video modes, the sound FIFOs and
   everything else the CPU sets them up for. */
#include "agb/board/board.h"

/* DMAxCNT_H: the control halfword the CPU writes. */
#define AGB_DMA_ENABLE 0x8000
#define AGB_DMA_IRQ    0x4000
#define AGB_DMA_REPEAT 0x0200
#define AGB_DMA_WORD   0x0400
#define AGB_DMA_TIMING 0x3000

/* the channel a register at offset belongs to, 12 bytes each from DMA0SAD (0xB0), and its place in that channel */
static int channel_of(uint32_t offset, int* part) {
    uint32_t at = offset - 0xB0;
    *part = (int)(at % 12);
    return at < 4 * 12 ? (int)(at / 12) : -1;
}

static void dma_transfer(AgbBoard* board, int index, int fifo) {
    AgbDmaChannel* channel = &board->dma[index];
    uint32_t count = channel->count ? channel->count : (index == 3 ? 0x10000 : 0x4000);
    int word = channel->control & AGB_DMA_WORD, step = word ? 4 : 2;
    int source_mode = channel->control >> 7 & 3, destination_mode = channel->control >> 5 & 3;
    uint32_t source = channel->source_now, destination = channel->destination_now;
    if (fifo) {
        word = 1;
        step = 4;
        count = 4;
        destination_mode = 2;
    }
    /* the bus is the DMA's until it ends: 2N + 2(n-1)S + 2I (4I when both ends are on the cartridge), which the CPU
       waits out (agb_cpu_run) */
    board->held_cycles += (source >> 27 & 1) && (destination >> 27 & 1) ? 4 : 2;
    for (uint32_t i = 0; i < count; ++i) {
        board->held_cycles += agb_access_cycles(board, source, word ? 32 : 16, i > 0) +
                              agb_access_cycles(board, destination, word ? 32 : 16, i > 0);
        /* the channel reads nothing below EWRAM (the BIOS, the unused space): it moves the unit it read last again,
           a halfword in both halves of its latch (mGBA's GBADMAService, the console's in its suite's dma.c) */
        if (word) {
            if (source >= 0x02000000) channel->latch = agb_read32(board, source);
            agb_write32(board, destination, channel->latch);
        } else {
            if (source >= 0x02000000) channel->latch = agb_read16(board, source) * 0x00010001u;
            /* a request to the EEPROM: the halfwords left say where its block's number ends */
            if ((destination >> 24 & 0x0F) == 0x0D && agb_save_eeprom(board))
                agb_eeprom_write(board, (uint16_t)channel->latch, count - i);
            else
                agb_write16(board, destination, (uint16_t)(channel->latch >> (destination & 2) * 8));
        }
        /* from the cartridge the source counts up whatever the control says (the suite's dma.c on the console: a fixed
           or decrementing source there moves the next units, as mGBA's GBADMAService has it) */
        if (source - 0x08000000u < 0x06000000u)
            source += (uint32_t)step;
        else
            switch (source_mode) {
                case 0: source += (uint32_t)step; break;
                case 1: source -= (uint32_t)step; break;
                case 3: source += (uint32_t)step; break;
                default: break;
            }
        switch (destination_mode) {
            case 0:
            case 3: destination += (uint32_t)step; break;
            case 1: destination -= (uint32_t)step; break;
            default: break;
        }
    }
    channel->source_now = source;
    /* repeat only takes a channel that waits for its moment (a blank, a FIFO): one that started at once ends, its
       enable bit clear for the next start, as mGBA's GBADMAService has it */
    if ((channel->control & AGB_DMA_REPEAT) && (channel->control & AGB_DMA_TIMING)) {
        /* the destination reloads in increment-reload mode, and never moves in FIFO mode */
        channel->destination_now = fifo                    ? channel->destination
                                   : destination_mode == 3 ? channel->destination
                                                           : destination;
    } else {
        channel->control &= (uint16_t)~AGB_DMA_ENABLE;
        channel->destination_now = destination;
    }
    if (channel->control & AGB_DMA_IRQ) agb_irq_request(board, AGB_IRQ_DMA(index));
}

void agb_dma_write(AgbBoard* board, uint32_t offset, uint16_t value) {
    int part, index = channel_of(offset, &part);
    if (index < 0 || index > 3) return;
    AgbDmaChannel* channel = &board->dma[index];
    /* the addresses' widths (GBATEK, DMA Transfers): DMA0 reads only the console's own memory, 27 bits, the others
       28; DMA3 writes 28 bits, the others 27. DMA0 pointed at the cartridge reads the BIOS's addresses so */
    uint32_t source_mask = index ? 0x0FFFFFFF : 0x07FFFFFF, destination_mask = index == 3 ? 0x0FFFFFFF : 0x07FFFFFF;
    switch (part) {
        case 0x0: channel->source = ((channel->source & 0xFFFF0000) | value) & source_mask; return;
        case 0x2: channel->source = ((channel->source & 0x0000FFFF) | (uint32_t)value << 16) & source_mask; return;
        case 0x4: channel->destination = ((channel->destination & 0xFFFF0000) | value) & destination_mask; return;
        case 0x6:
            channel->destination = ((channel->destination & 0x0000FFFF) | (uint32_t)value << 16) & destination_mask;
            return;
        case 0x8: channel->count = value; return;
        case 0xA: {
            int was = channel->control & AGB_DMA_ENABLE;
            channel->control = value;
            channel->source_now = channel->source;
            channel->destination_now = channel->destination;
            if (!was && (value & AGB_DMA_ENABLE)) {
                int timing = value >> 12 & 3;
                if (!timing) dma_transfer(board, index, 0);
            }
            return;
        }
        default: return;
    }
}

uint16_t agb_dma_written(AgbBoard* board, uint32_t offset) {
    int part, index = channel_of(offset, &part);
    if (index < 0 || index > 3) return 0;
    const AgbDmaChannel* channel = &board->dma[index];
    switch (part) {
        case 0x0: return (uint16_t)channel->source;
        case 0x2: return (uint16_t)(channel->source >> 16);
        case 0x4: return (uint16_t)channel->destination;
        case 0x6: return (uint16_t)(channel->destination >> 16);
        default: return agb_dma_read(board, offset);
    }
}

uint16_t agb_dma_read(AgbBoard* board, uint32_t offset) {
    int part, index = channel_of(offset, &part);
    if (index < 0 || index > 3) return 0;
    AgbDmaChannel* channel = &board->dma[index];
    if (part == 0xA) return channel->control;
    if (part == 0x8) return channel->count;
    return 0;
}

void agb_dma_event(AgbBoard* board, int event) {
    for (int index = 0; index < 4; ++index) {
        AgbDmaChannel* channel = &board->dma[index];
        int timing = channel->control >> 12 & 3;
        if (!(channel->control & AGB_DMA_ENABLE)) continue;
        if (event == AGB_DMA_VBLANK && timing == 1) dma_transfer(board, index, 0);
        if (event == AGB_DMA_HBLANK && timing == 2) dma_transfer(board, index, 0);
        if (timing == 3 && (index == 1 || index == 2)) {
            uint32_t target = channel->destination_now & 0x0FFFFFFC;
            if (event == AGB_DMA_FIFO_A && target == 0x040000A0) dma_transfer(board, index, 1);
            if (event == AGB_DMA_FIFO_B && target == 0x040000A4) dma_transfer(board, index, 1);
        }
    }
}
