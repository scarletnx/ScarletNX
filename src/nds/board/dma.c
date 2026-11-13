/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ARM9's four DMA channels. A transfer runs whole when it starts: the board keeps no bus time, so the CPU does not
   wait for it, and a transfer to the geometry FIFO, which the console paces by the FIFO's room, runs at once, since the
   3D engine takes its commands as they come (gpu/gpu3d.c). */
#include "nds/board/nds.h"
#include <stdio.h>
#include "common/utilities/endian.h"

static bool plain(uint32_t address, uint64_t bytes) {
    return address >> 24 != 0x04 && (address + bytes - 1) >> 24 != 0x04 && (uint64_t)address + bytes <= NDS_MAP_SIZE;
}

static void run(NdsBoard* board, unsigned channel) {
    NdsDma* d = &board->dma[channel];
    uint32_t control = d->control, count = d->left;
    unsigned width = control & (1u << 26) ? 4 : 2, to = control >> 21 & 3, from = control >> 23 & 3;
    int32_t from_step = from == 0 ? (int32_t)width : from == 1 ? -(int32_t)width : 0;
    int32_t to_step = to == 0 || to == 3 ? (int32_t)width : to == 1 ? -(int32_t)width : 0;
    uint32_t source = d->at_source & ~(width - 1), destination = d->at_destination & ~(width - 1);
    uint64_t span = (uint64_t)count * width;
    /* memory to memory, both rising or the source held: the window's bytes moved as they are */
    if (from_step >= 0 && to_step > 0 && plain(source, from_step ? span : width) && plain(destination, span)) {
        unsigned char* base = board->mem.base;
        if (from_step) {
            for (uint32_t i = 0; i < count; ++i) {
                if (width == 4)
                    snx_st_le32(base + destination, snx_ld_le32(base + source));
                else
                    snx_st_le16(base + destination, snx_ld_le16(base + source));
                source += width, destination += width;
            }
        } else {
            for (uint32_t i = 0; i < count; ++i, destination += width) {
                if (width == 4)
                    snx_st_le32(base + destination, snx_ld_le32(base + source));
                else
                    snx_st_le16(base + destination, snx_ld_le16(base + source));
            }
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) {
            if (width == 4)
                nds_write32(board, destination, nds_read32(board, source));
            else
                nds_write16(board, destination, nds_read16(board, source));
            source += (uint32_t)from_step, destination += (uint32_t)to_step;
        }
    }
    d->at_source = source, d->at_destination = destination;
    unsigned mode = control >> 27 & 7;
    if ((control & (1u << 25)) && mode != NDS_DMA_NOW && mode != NDS_DMA_GXFIFO) {
        d->left = control & 0x1FFFFF ? control & 0x1FFFFF : 0x200000;
        if (to == 3) d->at_destination = d->destination;
    } else {
        d->control &= 0x7FFFFFFFu;
        d->running = 0;
    }
    if (control & (1u << 30)) nds_irq(board, NDS_IRQ_DMA0 + channel);
}

/* The card's words go to a channel as long as the card has them. A channel that repeats takes its count again each
   time (the SDK reads a block of 512 bytes with a count of one word), and one whose count is more than the block
   stays on for the next */
static void card_feed(NdsBoard* board, unsigned channel) {
    NdsDma* d = &board->dma[channel];
    while (d->running && (d->control >> 31) && board->card_left >= 4) {
        uint32_t words = board->card_left / 4, left = d->left;
        if (words < left) {
            uint32_t control = d->control;
            d->left = words;
            d->control |= 1u << 25; /* more to come: the channel stays on */
            d->control &= ~(1u << 30);
            run(board, channel);
            d->control = control;
            d->left = left - words;
            d->running = 1;
            return;
        }
        run(board, channel);
    }
}

void nds_dma_write_control(NdsBoard* board, unsigned channel, uint32_t value) {
    NdsDma* d = &board->dma[channel];
    uint32_t old = d->control;
    d->control = value;
    if (board->trace > 1 && (value >> 27 & 7) == NDS_DMA_CARD)
        fprintf(stderr, "nds dma %u: control %08X (was %08X) to %08X, card has %u bytes\n", channel, value, old,
                d->destination, (unsigned)board->card_left);
    if (!(value >> 31)) {
        d->running = 0;
        return;
    }
    if (old >> 31) return;
    d->at_source = d->source & 0x0FFFFFFFu;
    d->at_destination = d->destination & 0x0FFFFFFFu;
    d->left = value & 0x1FFFFF ? value & 0x1FFFFF : 0x200000;
    d->running = 1;
    unsigned mode = value >> 27 & 7;
    if (mode == NDS_DMA_NOW || mode == NDS_DMA_GXFIFO) run(board, channel);
    if (mode == NDS_DMA_CARD) card_feed(board, channel);
}

void nds_dma_trigger(NdsBoard* board, unsigned when) {
    for (unsigned channel = 0; channel < 4; ++channel) {
        NdsDma* d = &board->dma[channel];
        if (!d->running || (d->control >> 27 & 7) != when) continue;
        if (when == NDS_DMA_CARD)
            card_feed(board, channel);
        else
            run(board, channel);
    }
}
