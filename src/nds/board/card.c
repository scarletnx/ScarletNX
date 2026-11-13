/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The card's slot as the ARM9 drives it: the eight command bytes, ROMCTRL and the data port. The commands a started
   game sends are the data read (0xB7) and the chip ID (0xB8), which answer at once; the card's serial bus, which
   reaches the save memory, is the ARM7's in the SDK and is answered there (hle/pxi.c). GBATEK has the registers. */
#include "nds/board/nds.h"
#include <stdio.h>
#include "common/utilities/endian.h"

static void done(NdsBoard* board) {
    board->romctrl &= ~(1u << 31 | 1u << 23);
    board->card_left = 0;
    if (board->auxspicnt & 0x4000) nds_irq(board, NDS_IRQ_CARD_DONE);
}

void nds_card_write_romctrl(NdsBoard* board, uint32_t value) {
    board->romctrl = (value & 0x7F7FFFFFu) | (board->romctrl & (1u << 31 | 1u << 23)) | (value & 1u << 29);
    if (!(value >> 31)) return;
    unsigned code = value >> 24 & 7;
    uint32_t bytes = !code ? 0 : code == 7 ? 4 : 0x100u << code;
    const uint8_t* c = board->card_command;
    board->card_kind = 0;
    if (c[0] == 0xB7) {
        uint32_t address = snx_ld_be32(c + 1);
        /* the card gives no byte below 0x8000 to the data read: it reads the 512 bytes from 0x8000 again */
        if (address < 0x8000) address = 0x8000 + (address & 0x1FF);
        board->card_kind = 1;
        board->card_address = address;
    } else if (c[0] == 0xB8 || c[0] == 0x90)
        board->card_kind = 2;
    board->card_left = bytes;
    if (board->trace > 1)
        fprintf(stderr, "nds card: command %02X%02X%02X%02X%02X, %u bytes, ROMCTRL %08X AUXSPICNT %04X\n", c[0], c[1], c[2],
                c[3], c[4], (unsigned)bytes, value, board->auxspicnt);
    board->romctrl |= 1u << 31;
    if (!bytes) {
        done(board);
        return;
    }
    board->romctrl |= 1u << 23;
    nds_dma_trigger(board, NDS_DMA_CARD);
}

uint32_t nds_card_read_data(NdsBoard* board) {
    if (!board->card_left) return 0xFFFFFFFFu;
    uint32_t value = 0xFFFFFFFFu;
    if (board->card_kind == 2)
        value = board->rom->chip_id;
    else if (board->card_kind == 1) {
        const NdsRom* rom = board->rom;
        uint32_t at = board->card_address;
        if ((uint64_t)at + 4 <= rom->size)
            value = snx_ld_le32(rom->data + at);
        else {
            unsigned char bytes[4] = {0xFF, 0xFF, 0xFF, 0xFF};
            for (unsigned i = 0; i < 4; ++i)
                if ((uint64_t)at + i < rom->size) bytes[i] = rom->data[at + i];
            value = snx_ld_le32(bytes);
        }
        board->card_address = at + 4;
    }
    board->card_left = board->card_left > 4 ? board->card_left - 4 : 0;
    if (!board->card_left) done(board);
    return value;
}
