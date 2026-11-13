/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ARM9's registers at 0x04000000 (GBATEK's "DS I/O Map"; the divider's and the FIFO's corner cases as melonDS's
   NDS.cpp has them). A register is a halfword here: a byte's write merges into it and a word's is two of them, but for
   the ports that take a whole word (the FIFOs, the card's data, the 3D engine's commands). */
#include "nds/board/nds.h"
#include "nds/gpu/gpu3d.h"
#include "common/utilities/bits.h"
#include <stdio.h>

static void divide(NdsBoard* board) {
    uint64_t n = board->div_numerator, d = board->div_denominator;
    board->divcnt &= ~0x4000u;
    if (!d) board->divcnt |= 0x4000;
    switch (board->divcnt & 3) {
        case 0: {
            int32_t num = (int32_t)(uint32_t)n, den = (int32_t)(uint32_t)d;
            if (!den) {
                /* +1 or -1 in the low word, and the high word the other way round */
                board->div_quotient = num < 0 ? 0xFFFFFFFF00000001ull : 0x00000000FFFFFFFFull;
                board->div_remainder = (uint64_t)(int64_t)num;
            } else if (num == INT32_MIN && den == -1) {
                board->div_quotient = 0x80000000ull;
                board->div_remainder = 0;
            } else {
                board->div_quotient = (uint64_t)(int64_t)(num / den);
                board->div_remainder = (uint64_t)(int64_t)(num % den);
            }
            break;
        }
        case 1:
        case 3: {
            int64_t num = (int64_t)n;
            int32_t den = (int32_t)(uint32_t)d;
            if (!den) {
                board->div_quotient = num < 0 ? 1 : UINT64_MAX;
                board->div_remainder = (uint64_t)num;
            } else if (num == INT64_MIN && den == -1) {
                board->div_quotient = 0x8000000000000000ull;
                board->div_remainder = 0;
            } else {
                board->div_quotient = (uint64_t)(num / den);
                board->div_remainder = (uint64_t)(num % den);
            }
            break;
        }
        default: {
            int64_t num = (int64_t)n, den = (int64_t)d;
            if (!den) {
                board->div_quotient = num < 0 ? 1 : UINT64_MAX;
                board->div_remainder = (uint64_t)num;
            } else if (num == INT64_MIN && den == -1) {
                board->div_quotient = 0x8000000000000000ull;
                board->div_remainder = 0;
            } else {
                board->div_quotient = (uint64_t)(num / den);
                board->div_remainder = (uint64_t)(num % den);
            }
            break;
        }
    }
}

static void square_root(NdsBoard* board) {
    uint64_t value = board->sqrtcnt & 1 ? board->sqrt_value : (uint32_t)board->sqrt_value;
    uint64_t root = 0, bit = 1ull << 62;
    while (bit > value) bit >>= 2;
    for (; bit; bit >>= 2) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else
            root >>= 1;
    }
    board->sqrt_result = (uint32_t)root;
}

/* a halfword of a 64-bit register */
static uint16_t part(uint64_t value, uint32_t offset) { return (uint16_t)(value >> ((offset & 6) * 8)); }
static void set_part(uint64_t* value, uint32_t offset, uint16_t half, uint16_t mask) {
    unsigned shift = (offset & 6) * 8;
    uint16_t merged = (uint16_t)((part(*value, offset) & ~mask) | (half & mask));
    *value = (*value & ~(0xFFFFull << shift)) | (uint64_t)merged << shift;
}
static void set_half(uint32_t* value, int high, uint16_t half, uint16_t mask) {
    unsigned shift = high ? 16 : 0;
    uint32_t merged = ((*value >> shift) & ~(uint32_t)mask & 0xFFFF) | (half & mask);
    *value = (*value & ~(0xFFFFu << shift)) | merged << shift;
}

static uint16_t ipc_status(const NdsBoard* board) {
    /* what the game sends is taken at once: its FIFO is always empty */
    return (uint16_t)(board->ipcfifocnt | 1 | (!board->ipc_recv_count ? 0x100 : board->ipc_recv_count >= 16 ? 0x200 : 0));
}

static uint16_t read16(NdsBoard* board, uint32_t offset) {
    if (offset < 0x70) {
        if (offset == 0x04) return board->dispstat;
        if (offset == 0x06) return board->vcount;
        if (offset == 0x60) return (uint16_t)nds_gpu3d_read(board, offset, 2);
        return nds_lcd_read(board, 0, offset);
    }
    if (offset >= 0x1000 && offset < 0x1070) return nds_lcd_read(board, 1, offset - 0x1000);
    if (offset >= 0xB0 && offset < 0xE0) {
        const NdsDma* d = &board->dma[(offset - 0xB0) / 12];
        unsigned reg = (offset - 0xB0) % 12;
        uint32_t value = reg < 4 ? d->source : reg < 8 ? d->destination : d->control;
        return (uint16_t)(value >> (reg & 2 ? 16 : 0));
    }
    if (offset >= 0xE0 && offset < 0xF0) return (uint16_t)(board->dma_fill[(offset - 0xE0) / 4] >> (offset & 2 ? 16 : 0));
    if (offset >= 0x100 && offset < 0x110) {
        unsigned index = (offset - 0x100) / 4;
        return offset & 2 ? board->timer[index].control : nds_timer_read(board, index);
    }
    switch (offset) {
        case 0x130: return board->keyinput;
        case 0x132: return board->keycnt;
        case 0x180: return board->ipcsync;
        case 0x184: return ipc_status(board);
        case 0x1A0: return board->auxspicnt;
        case 0x1A2: return 0xFF; /* nothing answers on the card's serial bus */
        case 0x1A4: return (uint16_t)board->romctrl;
        case 0x1A6: return (uint16_t)(board->romctrl >> 16);
        case 0x204: return board->exmemcnt;
        case 0x208: return (uint16_t)board->ime;
        case 0x20A: return 0;
        case 0x210: return (uint16_t)board->ie;
        case 0x212: return (uint16_t)(board->ie >> 16);
        case 0x214: return (uint16_t)board->irf;
        case 0x216: return (uint16_t)(board->irf >> 16);
        case 0x240: return (uint16_t)(board->vramcnt[0] | board->vramcnt[1] << 8);
        case 0x242: return (uint16_t)(board->vramcnt[2] | board->vramcnt[3] << 8);
        case 0x244: return (uint16_t)(board->vramcnt[4] | board->vramcnt[5] << 8);
        case 0x246: return (uint16_t)(board->vramcnt[6] | board->wramcnt << 8);
        case 0x248: return (uint16_t)(board->vramcnt[7] | board->vramcnt[8] << 8);
        case 0x280: return board->divcnt;
        case 0x2B0: return board->sqrtcnt;
        case 0x2B4: return (uint16_t)board->sqrt_result;
        case 0x2B6: return (uint16_t)(board->sqrt_result >> 16);
        case 0x300: return board->postflg;
        case 0x304: return board->powcnt;
        default: break;
    }
    if (offset >= 0x1A8 && offset < 0x1B0)
        return (uint16_t)(board->card_command[offset - 0x1A8] | board->card_command[offset - 0x1A7] << 8);
    if (offset >= 0x290 && offset < 0x298) return part(board->div_numerator, offset);
    if (offset >= 0x298 && offset < 0x2A0) return part(board->div_denominator, offset);
    if (offset >= 0x2A0 && offset < 0x2A8) return part(board->div_quotient, offset);
    if (offset >= 0x2A8 && offset < 0x2B0) return part(board->div_remainder, offset);
    if (offset >= 0x2B8 && offset < 0x2C0) return part(board->sqrt_value, offset);
    if (board->trace) fprintf(stderr, "nds io: read of 0x%08X\n", 0x04000000u + offset);
    return 0;
}

static void write16(NdsBoard* board, uint32_t offset, uint16_t value, uint16_t mask) {
    if (offset < 0x70) {
        if (offset == 0x04) {
            board->dispstat = (uint16_t)((board->dispstat & ~(mask & 0xFFB8)) | (value & mask & 0xFFB8));
            return;
        }
        if (offset == 0x06) return;
        if (offset == 0x60 || offset == 0x62) {
            nds_gpu3d_write(board, offset, 2, value & mask);
            if (offset == 0x60) board->lcd[0][0x30] = (uint16_t)nds_gpu3d_read(board, 0x60, 2);
            return;
        }
        nds_lcd_write(board, 0, offset, value, mask);
        return;
    }
    if (offset >= 0x1000 && offset < 0x1070) {
        nds_lcd_write(board, 1, offset - 0x1000, value, mask);
        return;
    }
    if (offset >= 0xB0 && offset < 0xE0) {
        unsigned channel = (offset - 0xB0) / 12, reg = (offset - 0xB0) % 12;
        NdsDma* d = &board->dma[channel];
        if (reg < 4)
            set_half(&d->source, reg & 2, value, mask);
        else if (reg < 8)
            set_half(&d->destination, reg & 2, value, mask);
        else {
            uint32_t control = d->control;
            set_half(&control, reg & 2, value, mask);
            if (reg & 2)
                nds_dma_write_control(board, channel, control);
            else
                d->control = (d->control & 0x80000000u) | (control & 0x7FFFFFFFu);
        }
        return;
    }
    if (offset >= 0xE0 && offset < 0xF0) {
        set_half(&board->dma_fill[(offset - 0xE0) / 4], offset & 2, value, mask);
        return;
    }
    if (offset >= 0x100 && offset < 0x110) {
        unsigned index = (offset - 0x100) / 4;
        NdsTimer* t = &board->timer[index];
        if (offset & 2)
            nds_timer_write(board, index, 0, (uint16_t)((t->control & ~mask) | (value & mask)), 2);
        else
            nds_timer_write(board, index, (uint16_t)((t->reload & ~mask) | (value & mask)), 0, 1);
        return;
    }
    if (offset >= 0x1A8 && offset < 0x1B0) {
        if (mask & 0x00FF) board->card_command[offset - 0x1A8] = (uint8_t)value;
        if (mask & 0xFF00) board->card_command[offset - 0x1A7] = (uint8_t)(value >> 8);
        return;
    }
    if (offset >= 0x240 && offset < 0x24A) {
        static const signed char bank[10] = {0, 1, 2, 3, 4, 5, 6, -1, 7, 8};
        bool changed = false;
        for (unsigned i = 0; i < 2; ++i) {
            if (!(mask >> (i * 8) & 0xFF)) continue;
            uint8_t byte = (uint8_t)(value >> (i * 8));
            int which = bank[offset - 0x240 + i];
            if (which < 0)
                board->wramcnt = byte & 3;
            else if (board->vramcnt[which] != (byte & 0x9F)) {
                board->vramcnt[which] = byte & 0x9F;
                changed = true;
            }
        }
        if (changed) nds_vram_remap(board);
        return;
    }
    if (offset >= 0x290 && offset < 0x2A0) {
        set_part(offset < 0x298 ? &board->div_numerator : &board->div_denominator, offset, value, mask);
        divide(board);
        return;
    }
    if (offset >= 0x2B8 && offset < 0x2C0) {
        set_part(&board->sqrt_value, offset, value, mask);
        square_root(board);
        return;
    }
    switch (offset) {
        case 0x130: return;
        case 0x132: board->keycnt = (uint16_t)((board->keycnt & ~mask) | (value & mask)); return;
        case 0x180:
            if (!(mask & 0xFF00)) return;
            board->ipcsync = (uint16_t)((board->ipcsync & 0x000F) | (value & 0x4F00));
            nds_pxi_sync(board);
            return;
        case 0x184: {
            uint16_t old = board->ipcfifocnt;
            value = (uint16_t)((old & ~mask) | (value & mask));
            if ((value & 0x0004) && !(old & 0x0004)) nds_irq(board, NDS_IRQ_IPC_SEND_DONE);
            if ((value & 0x0400) && !(old & 0x0400) && board->ipc_recv_count) nds_irq(board, NDS_IRQ_IPC_RECV);
            if (value & 0x4000) old &= ~0x4000u;
            board->ipcfifocnt = (uint16_t)((value & 0x8404) | (old & 0x4000));
            return;
        }
        case 0x1A0: board->auxspicnt = (uint16_t)((board->auxspicnt & ~mask) | (value & mask & 0xE043)); return;
        case 0x1A2: return;
        case 0x1A4: {
            uint32_t control = board->romctrl;
            set_half(&control, 0, value, mask);
            board->romctrl = (board->romctrl & 0xFFFF0000u) | (control & 0xFFFF);
            return;
        }
        case 0x1A6: {
            uint32_t control = board->romctrl & ~(1u << 31 | 1u << 23);
            set_half(&control, 1, value, mask);
            nds_card_write_romctrl(board, control);
            return;
        }
        case 0x204: board->exmemcnt = (uint16_t)((board->exmemcnt & ~mask) | (value & mask)); return;
        case 0x208:
            if (mask & 1) board->ime = value & 1;
            return;
        case 0x210: set_half(&board->ie, 0, value, mask); return;
        case 0x212: set_half(&board->ie, 1, value, mask); return;
        case 0x214: board->irf &= ~(uint32_t)(value & mask); return;
        case 0x216:
            board->irf &= ~((uint32_t)(value & mask) << 16);
            (void)nds_gpu3d_irq(board); /* a level: acknowledged while it holds, it is there again */
            return;
        case 0x280:
            board->divcnt = (uint16_t)((board->divcnt & 0xC000) | (value & mask & 3) | (board->divcnt & ~mask & 3));
            divide(board);
            return;
        case 0x2B0:
            board->sqrtcnt = (uint16_t)((value & mask & 1) | (board->sqrtcnt & ~mask & 1));
            square_root(board);
            return;
        case 0x300:
            if (mask & 0xFF) board->postflg = (uint8_t)((board->postflg & 1) | (value & 3));
            return;
        case 0x304: board->powcnt = (uint16_t)((board->powcnt & ~mask) | (value & mask & 0x820F)); return;
        default: break;
    }
    if (board->trace) fprintf(stderr, "nds io: write of 0x%04X to 0x%08X\n", value, 0x04000000u + offset);
}

uint32_t nds_io_read(NdsBoard* board, uint32_t address, unsigned bytes) {
    uint32_t offset = address & 0x00FFFFFFu;
    if (offset >= 0x320 && offset < 0x6A4) return nds_gpu3d_read(board, offset, bytes);
    if (offset == 0x100000) { /* the FIFO the ARM7 fills */
        if (!(board->ipcfifocnt & 0x8000)) return board->ipc_last;
        if (!board->ipc_recv_count) {
            board->ipcfifocnt |= 0x4000;
            return board->ipc_last;
        }
        board->ipc_last = board->ipc_recv[board->ipc_recv_at];
        board->ipc_recv_at = (board->ipc_recv_at + 1) % 64;
        --board->ipc_recv_count;
        return board->ipc_last;
    }
    if (offset == 0x100010) return nds_card_read_data(board);
    if (offset >= 0x2000) return 0;
    if (bytes == 4) return read16(board, offset & ~3u) | (uint32_t)read16(board, (offset & ~3u) + 2) << 16;
    if (bytes == 2) return read16(board, offset & ~1u);
    return (read16(board, offset & ~1u) >> ((offset & 1) * 8)) & 0xFF;
}

void nds_io_write(NdsBoard* board, uint32_t address, unsigned bytes, uint32_t value) {
    uint32_t offset = address & 0x00FFFFFFu;
    if (offset >= 0x320 && offset < 0x6A4) {
        nds_gpu3d_write(board, offset, bytes, value);
        return;
    }
    if (offset >= 0x2000) return;
    if (bytes == 4) {
        offset &= ~3u;
        if (offset == 0x188) {
            if (!(board->ipcfifocnt & 0x8000)) return;
            nds_pxi_request(board, value);
            if (board->ipcfifocnt & 0x0004) nds_irq(board, NDS_IRQ_IPC_SEND_DONE);
            return;
        }
        if (offset == 0x208) {
            board->ime = value & 1;
            return;
        }
        if (offset == 0x068) { /* DISP_MMEM_FIFO: two pixels of the main memory display */
            if (board->main_display_at + 2 <= NDS_SCREEN_HEIGHT * NDS_SCREEN_WIDTH) {
                board->main_display[board->main_display_at++] = (uint16_t)value;
                board->main_display[board->main_display_at++] = (uint16_t)(value >> 16);
            }
            return;
        }
        if (offset == 0x1A4) { /* one write, not the low half and then the start */
            nds_card_write_romctrl(board, value);
            return;
        }
        write16(board, offset, (uint16_t)value, 0xFFFF);
        write16(board, offset + 2, (uint16_t)(value >> 16), 0xFFFF);
        return;
    }
    if (bytes == 2) {
        write16(board, offset & ~1u, (uint16_t)value, 0xFFFF);
        return;
    }
    write16(board, offset & ~1u, (uint16_t)(value << ((offset & 1) * 8)), (uint16_t)(0xFF << ((offset & 1) * 8)));
}

void nds_ipc_reply(NdsBoard* board, uint32_t word) {
    if (board->ipc_recv_count == 64) {
        if (nds_once(board, 2)) fprintf(stderr, "scarletnx-nds: the game reads no answers of the ARM7: one is lost\n");
        return;
    }
    board->ipc_recv[(board->ipc_recv_at + board->ipc_recv_count) % 64] = word;
    if (!board->ipc_recv_count++ && (board->ipcfifocnt & 0x0400)) nds_irq(board, NDS_IRQ_IPC_RECV);
}
