/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The display's time and the 2D engines' registers. Nothing is drawn here: at each visible line's horizontal blank the
   registers the line was drawn with are kept (NdsLine), and the frame is composed from the 192 of them on the GPU when
   its last line is done (gpu/vk/compose.c). The registers' behavior follows melonDS's GPU.cpp and GPU2D.cpp (the
   reference points a line counts on, the windows' vertical flags, the flags of DISPSTAT). */
#include "nds/board/nds.h"
#include "nds/gpu/gpu3d.h"
#include "nds/gpu/present.h"
#include "common/utilities/bits.h"

/* a register's halfword in the engine's block */
#define REG(e, offset) board->lcd[e][(offset) >> 1]

void nds_lcd_reset(NdsBoard* board) {
    board->vcount = 0;
    board->dispstat = 0;
    board->line_start = board->cycles;
    for (unsigned e = 0; e < 2; ++e) {
        REG(e, 0x20) = REG(e, 0x26) = REG(e, 0x30) = REG(e, 0x36) = 0x100; /* the affine identities */
    }
    nds_schedule(board, NDS_EVENT_LCD, board->line_start + NDS_HBLANK_CYCLE);
}

static int32_t reference(const NdsBoard* board, unsigned e, uint32_t offset) {
    uint32_t raw = board->lcd[e][offset >> 1] | (uint32_t)board->lcd[e][(offset >> 1) + 1] << 16;
    return snx_sext32(raw, 28);
}

/* the line's registers, as the compositor reads them (gpu/vk/shaders/compose.glsl has the same places) */
static void keep_line(NdsBoard* board, unsigned e, unsigned line) {
    uint32_t* w = board->lines[e][line].words;
    w[0] = REG(e, 0x00) | (uint32_t)REG(e, 0x02) << 16;
    w[1] = REG(e, 0x08) | (uint32_t)REG(e, 0x0A) << 16;
    w[2] = REG(e, 0x0C) | (uint32_t)REG(e, 0x0E) << 16;
    for (unsigned bg = 0; bg < 4; ++bg) w[3 + bg] = REG(e, 0x10 + bg * 4) | (uint32_t)REG(e, 0x12 + bg * 4) << 16;
    for (unsigned bg = 0; bg < 2; ++bg) {
        w[7 + bg * 4] = REG(e, 0x20 + bg * 16) | (uint32_t)REG(e, 0x22 + bg * 16) << 16;
        w[8 + bg * 4] = REG(e, 0x24 + bg * 16) | (uint32_t)REG(e, 0x26 + bg * 16) << 16;
        w[9 + bg * 4] = (uint32_t)board->affine_x[e][bg];
        w[10 + bg * 4] = (uint32_t)board->affine_y[e][bg];
    }
    w[15] = REG(e, 0x40) | (uint32_t)REG(e, 0x42) << 16;
    w[16] = REG(e, 0x48) | (uint32_t)REG(e, 0x4A) << 16;
    w[17] = REG(e, 0x4C) | (uint32_t)REG(e, 0x50) << 16;
    w[18] = REG(e, 0x52) | (uint32_t)REG(e, 0x54) << 16;
    w[19] = REG(e, 0x6C) | (uint32_t)board->window_y[e][0] << 16 | (uint32_t)board->window_y[e][1] << 17 |
            (uint32_t)(board->powcnt >> 15 & 1) << 18 | (uint32_t)(board->powcnt & 1) << 19 |
            (uint32_t)(board->powcnt >> (e ? 9 : 1) & 1) << 20;
    w[20] = REG(e, 0x64) | (uint32_t)REG(e, 0x66) << 16;
    w[21] = REG(e, 0x44) | (uint32_t)REG(e, 0x46) << 16;
    w[22] = REG(0, 0x60);
}

static void line_begins(NdsBoard* board) {
    unsigned line = board->vcount;
    for (unsigned e = 0; e < 2; ++e) {
        if (line == 0) {
            for (unsigned bg = 0; bg < 2; ++bg) {
                board->affine_x[e][bg] = reference(board, e, 0x28 + bg * 16);
                board->affine_y[e][bg] = reference(board, e, 0x2C + bg * 16);
            }
        }
        /* a window's vertical flag goes on at its first line and off at its end, whatever else the line is */
        for (unsigned win = 0; win < 2; ++win) {
            uint16_t v = REG(e, 0x44 + win * 2);
            if (line == (v & 0xFFu))
                board->window_y[e][win] = 0;
            else if (line == (unsigned)(v >> 8))
                board->window_y[e][win] = 1;
        }
    }
    /* VCOUNT's match */
    unsigned match = board->dispstat >> 8 | (board->dispstat & 0x80) << 1;
    if (line == match) {
        board->dispstat |= 4;
        if (board->dispstat & 0x20) nds_irq(board, NDS_IRQ_VCOUNT);
    } else
        board->dispstat &= ~4u;
    if (line >= 2 && line < 194) nds_dma_trigger(board, NDS_DMA_LINE);
    nds_pxi_line(board, line);
    if (line == NDS_SCREEN_HEIGHT) {
        board->dispstat |= 1;
        /* the frame is whole: composed and shown from its lines, then the 3D engine's buffers swap */
        nds_present_frame(board);
        REG(0, 0x66) &= 0x7FFF; /* a display capture is done: its busy bit goes */
        nds_gpu3d_vblank(board);
        if (board->dispstat & 8) nds_irq(board, NDS_IRQ_VBLANK);
        nds_dma_trigger(board, NDS_DMA_VBLANK);
        ++board->frames;
        board->frame_done = 1;
    } else if (line == 215)
        nds_gpu3d_render(board); /* the 3D engine starts the next frame's picture here, as the console's does */
    else if (line == NDS_FRAME_LINES - 1)
        board->dispstat &= ~1u;
}

void nds_lcd_event(NdsBoard* board) {
    if (!board->in_hblank) {
        unsigned line = board->vcount;
        board->in_hblank = 1;
        board->dispstat |= 2;
        if (line < NDS_SCREEN_HEIGHT) {
            for (unsigned e = 0; e < 2; ++e) {
                keep_line(board, e, line);
                for (unsigned bg = 0; bg < 2; ++bg) {
                    board->affine_x[e][bg] += (int16_t)REG(e, 0x22 + bg * 16);
                    board->affine_y[e][bg] += (int16_t)REG(e, 0x26 + bg * 16);
                }
            }
            nds_dma_trigger(board, NDS_DMA_HBLANK);
        }
        if (board->dispstat & 0x10) nds_irq(board, NDS_IRQ_HBLANK);
        nds_schedule(board, NDS_EVENT_LCD, board->line_start + NDS_LINE_CYCLES);
        return;
    }
    board->in_hblank = 0;
    board->dispstat &= ~2u;
    board->line_start += NDS_LINE_CYCLES;
    board->vcount = (uint16_t)((board->vcount + 1) % NDS_FRAME_LINES);
    nds_schedule(board, NDS_EVENT_LCD, board->line_start + NDS_HBLANK_CYCLE);
    line_begins(board);
}

uint16_t nds_lcd_read(NdsBoard* board, unsigned e, uint32_t offset) {
    switch (offset) {
        case 0x00:
        case 0x02:
        case 0x08:
        case 0x0A:
        case 0x0C:
        case 0x0E:
        case 0x48:
        case 0x4A:
        case 0x50:
        case 0x52:
        case 0x64:
        case 0x66:
        case 0x6C: return REG(e, offset);
        default: return 0;
    }
}

void nds_lcd_write(NdsBoard* board, unsigned e, uint32_t offset, uint16_t value, uint16_t mask) {
    uint16_t merged = (uint16_t)((REG(e, offset) & ~mask) | (value & mask));
    if (e && (offset == 0x00 || offset == 0x02)) /* engine B has no 3D, no bitmap objects' banks and no VRAM display */
        merged &= offset ? 0xC0B1 : 0xFFF7;
    if (offset == 0x6C) merged &= 0xC01F;
    REG(e, offset) = merged;
    /* a reference point written while the frame is drawn restarts the count from it */
    if (offset >= 0x28 && offset < 0x40 && (offset & 0x0F) >= 8 && board->vcount < NDS_SCREEN_HEIGHT) {
        unsigned bg = offset >= 0x38;
        if ((offset & 0x0F) < 0x0C)
            board->affine_x[e][bg] = reference(board, e, 0x28 + bg * 16);
        else
            board->affine_y[e][bg] = reference(board, e, 0x2C + bg * 16);
    }
}
