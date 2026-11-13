/* SPDX-License-Identifier: GPL-3.0-or-later AND BSD-2-Clause
   Copyright 2026 the ScarletNX contributors
   Copyright 2026 Mireille Meyer, Gloria Goertz */
/* The bus accesses each ARM7TDMI instruction makes, their order and the cartridge's prefetch unit follow
   NanoBoyAdvance (its ARM7TDMI.hh handlers and Bus/Timing.cc, commit 2da64af), altered from their original form into
   C over a block's trace:

   Redistribution and use in source and binary forms, with or without modification, are permitted provided that the
   following conditions are met:

   1. Redistributions of source code must retain the above copyright notice, this list of conditions and the
      following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the
      following disclaimer in the documentation and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
   INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
   DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
   SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */
#include "agb/cpu/timing.h"
#include "common/utilities/endian.h"
#include <string.h>

void agb_waits(AgbWaits* waits, uint32_t waitcnt) {
    /* the cycles of a first access (WAITCNT's two bit fields) and of a sequential one per window, the base one included */
    static const uint8_t first[4] = {5, 4, 3, 9}, next0[2] = {3, 2}, next1[2] = {5, 2}, next2[2] = {9, 2};
    /* the BIOS, IWRAM, I/O and OAM: 32 bits wide, no wait; EWRAM 16 bits wide with 2 wait states; the palette and VRAM
       16 bits wide */
    static const uint8_t base16[16] = {1, 1, 3, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    static const uint8_t base32[16] = {1, 1, 6, 1, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    uint8_t (*w16)[16] = waits->wait16, (*w32)[16] = waits->wait32;
    for (int s = 0; s < 2; ++s) {
        memcpy(w16[s], base16, sizeof base16);
        memcpy(w32[s], base32, sizeof base32);
    }
    for (int i = 0; i < 2; ++i) {
        w16[0][0x8 + i] = first[waitcnt >> 2 & 3];
        w16[0][0xA + i] = first[waitcnt >> 5 & 3];
        w16[0][0xC + i] = first[waitcnt >> 8 & 3];
        w16[1][0x8 + i] = next0[waitcnt >> 4 & 1];
        w16[1][0xA + i] = next1[waitcnt >> 7 & 1];
        w16[1][0xC + i] = next2[waitcnt >> 10 & 1];
        /* the cartridge's bus is 16 bits wide: a word is a halfword and a sequential one */
        for (int window = 0x8; window <= 0xC; window += 2) {
            w32[0][window + i] = (uint8_t)(w16[0][window] + w16[1][window]);
            w32[1][window + i] = (uint8_t)(w16[1][window] * 2);
        }
        /* the save memory, 8 bits wide: every access a first one */
        for (int s = 0; s < 2; ++s) w16[s][0xE + i] = w32[s][0xE + i] = first[waitcnt & 3];
    }
    waits->waitcnt = waitcnt;
}

static int cartridge(uint32_t address) { return address >= 0x08000000u && address < 0x0E000000u; }

/* cycles pass on the bus: the prefetch unit, while it works, reads on until it holds its fill */
static uint32_t step(AgbBus* bus, int prefetch, int32_t cycles) {
    AgbPrefetch* p = &bus->prefetch;
    if (p->active) {
        p->countdown -= cycles;
        while (p->countdown <= 0) {
            ++p->count;
            if (!prefetch || p->count >= p->capacity) break;
            p->last += p->width;
            p->countdown += p->duty;
        }
    }
    return (uint32_t)cycles;
}

/* the unit stops for the CPU's access elsewhere on the cartridge: one that ends a halfword's read in that cycle costs
   one more, while the CPU runs from the cartridge */
static uint32_t stop(AgbBus* bus, int prefetch, uint32_t pc) {
    AgbPrefetch* p = &bus->prefetch;
    uint32_t cycles = 0;
    if (!p->active) return 0;
    if (cartridge(pc) && (p->countdown == 1 || (p->width == 4 && p->countdown == (p->duty >> 1) + 1)))
        cycles = step(bus, prefetch, 1);
    p->active = 0;
    return cycles;
}

/* an access to the cartridge: an opcode the unit holds or is reading, or else the bus's own access, after which the unit
   reads on from there */
static uint32_t cartridge_access(AgbBus* bus, const AgbWaits* waits, int prefetch, uint32_t address, unsigned width,
                                 int code, int32_t cycles, uint32_t pc) {
    AgbPrefetch* p = &bus->prefetch;
    uint32_t page = address >> 24 & 0xF, total = 0;
    if (!code) return stop(bus, prefetch, pc) + step(bus, prefetch, cycles);
    if (p->active) {
        if (p->count && address == p->head) {
            --p->count;
            p->head += p->width;
            return step(bus, prefetch, 1);
        }
        if (p->countdown > 0 && address == p->last) {
            total = step(bus, prefetch, p->countdown);
            p->head = p->last;
            p->count = 0;
            return total;
        }
    }
    total = stop(bus, prefetch, pc);
    if (bus->was_disabled) {
        /* the access after WAITCNT turned the unit off is a first one, whatever its order */
        if (cycles == waits->wait16[1][page])
            cycles = waits->wait16[0][0x8];
        else if (cycles == waits->wait32[1][page])
            cycles = waits->wait32[0][0x8];
        bus->was_disabled = 0;
    }
    total += step(bus, prefetch, cycles);
    if (prefetch) {
        p->active = 1;
        p->count = 0;
        p->width = (uint8_t)width;
        p->capacity = width == 2 ? 8 : 4;
        p->duty = width == 2 ? waits->wait16[1][page] : waits->wait32[1][page];
        p->countdown = p->duty;
        p->last = p->head = address + width;
    }
    return total;
}

/* the cycles of an access to a region outside the cartridge, by its width */
static int32_t plain_cycles(const AgbWaits* waits, uint32_t address, unsigned width) {
    uint32_t page = address >> 24;
    if (page > 0xF) return 1;
    return width == 4 ? waits->wait32[0][page] : waits->wait16[0][page];
}

/* a cartridge access is sequential when it follows the last in order, unless it starts a 128 KiB page */
static int in_sequence(uint32_t address, int sequential) { return sequential && (address & 0x1FFFF); }

uint32_t agb_bus_fetch(AgbBus* bus, const AgbWaits* waits, int prefetch, uint32_t address, unsigned width,
                       int sequential) {
    if (!cartridge(address)) return step(bus, prefetch, plain_cycles(waits, address, width));
    int s = in_sequence(address, sequential);
    uint32_t page = address >> 24 & 0xF;
    return cartridge_access(bus, waits, prefetch, address, width, 1,
                            width == 2 ? waits->wait16[s][page] : waits->wait32[s][page], address);
}

uint32_t agb_bus_data(AgbBus* bus, const AgbWaits* waits, int prefetch, uint32_t address, unsigned width, int write,
                      int sequential, uint32_t pc) {
    uint32_t page = address >> 24;
    if (page >= 0xE && page <= 0xF) return stop(bus, prefetch, pc) + step(bus, prefetch, waits->wait16[0][0xE]);
    if (!cartridge(address)) return step(bus, prefetch, plain_cycles(waits, address, width));
    int s = in_sequence(address, sequential);
    int32_t cycles = width == 4 ? waits->wait32[s][page] : waits->wait16[s][page];
    if (write) return stop(bus, prefetch, pc) + step(bus, prefetch, cycles);
    return cartridge_access(bus, waits, prefetch, address, width, 0, cycles, pc);
}

uint32_t agb_bus_idle(AgbBus* bus, int prefetch, uint32_t cycles) {
    uint32_t total = 0;
    for (uint32_t i = 0; i < cycles; ++i) total += step(bus, prefetch, 1);
    return total;
}

/* ---- the instructions ---- */

enum {
    OP_PLAIN,  /* its fetch alone, the next one sequential */
    OP_SHIFT,  /* a register's shift amount: an internal cycle, the next fetch nonsequential */
    OP_LOAD,   /* the data, an internal cycle */
    OP_STORE,  /* the data */
    OP_SWAP,   /* a load, a store and an internal cycle */
    OP_MUL,    /* the multiplier's cycles */
    OP_MULL,   /* the same and one more */
    OP_BRANCH, /* the pipeline refilled at the target */
    OP_SWI     /* the pipeline refilled at the BIOS's vector */
};

typedef struct {
    uint8_t kind;
    uint8_t width;      /* a single transfer's bytes, 4 for a multiple one */
    uint8_t words;      /* the transfer's accesses */
    uint8_t reload;     /* it writes the PC: the pipeline refills at the target */
    uint8_t shift;      /* a register shift's internal cycle before it all */
    uint8_t is_signed;  /* a multiply whose multiplier ends on its sign */
    uint8_t accumulate; /* a multiply that adds: one more internal cycle */
    uint8_t cond;       /* an ARM one that may not run */
    uint8_t thumb_cond; /* Thumb's conditional branch */
} AgbOp;

static unsigned popcount16(uint32_t list) {
    unsigned n = 0;
    for (; list; list &= list - 1) ++n;
    return n;
}

static AgbOp arm_op(uint32_t w) {
    AgbOp op = {0};
    unsigned rd = w >> 12 & 15;
    op.width = 4;
    if (w >> 28 == 0xF) return op; /* never runs on the ARM7TDMI */
    op.cond = w >> 28 != 0xE;
    if ((w & 0x0FFFFFF0) == 0x012FFF10) {
        op.kind = OP_BRANCH; /* BX */
        op.reload = 1;
    } else if ((w & 0x0FC000F0) == 0x00000090) {
        op.kind = OP_MUL;
        op.is_signed = 1;
        op.accumulate = w >> 21 & 1;
        op.reload = (w >> 16 & 15) == 15;
    } else if ((w & 0x0F8000F0) == 0x00800090) {
        op.kind = OP_MULL;
        op.is_signed = w >> 22 & 1;
        op.accumulate = w >> 21 & 1;
        op.reload = (w >> 16 & 15) == 15 || rd == 15;
    } else if ((w & 0x0FB00FF0) == 0x01000090) {
        op.kind = OP_SWAP;
        op.width = w & 1u << 22 ? 1 : 4;
        op.words = 2;
        op.reload = rd == 15;
    } else if ((w & 0x0E000090) == 0x00000090 && (w & 0x60)) {
        unsigned kind = w >> 5 & 3;
        op.kind = w & 1u << 20 ? OP_LOAD : OP_STORE;
        op.width = kind == 2 ? 1 : 2;
        op.words = 1;
        op.reload = op.kind == OP_LOAD && rd == 15;
    } else if ((w & 0x0FBF0FFF) == 0x010F0000 || (w & 0x0FB0FFF0) == 0x0120F000 || (w & 0x0FB0F000) == 0x0320F000) {
        op.kind = OP_PLAIN; /* MRS, MSR */
    } else if ((w & 0x0C000000) == 0) {
        unsigned code = w >> 21 & 15;
        op.shift = !(w & 1u << 25) && (w & 0x10);
        op.reload = rd == 15 && (code < 8 || code > 11);
        op.kind = op.reload ? OP_BRANCH : op.shift ? OP_SHIFT : OP_PLAIN;
    } else if ((w & 0x0E000010) == 0x06000010) {
        op.kind = OP_PLAIN; /* undefined */
    } else if ((w & 0x0C000000) == 0x04000000) {
        op.kind = w & 1u << 20 ? OP_LOAD : OP_STORE;
        op.width = w & 1u << 22 ? 1 : 4;
        op.words = 1;
        op.reload = op.kind == OP_LOAD && rd == 15;
    } else if ((w & 0x0E000000) == 0x08000000) {
        uint32_t list = w & 0xFFFF;
        op.kind = w & 1u << 20 ? OP_LOAD : OP_STORE;
        /* an empty list moves the PC alone */
        op.words = (uint8_t)(list ? popcount16(list) : 1);
        op.reload = op.kind == OP_LOAD && (!list || (list & 0x8000));
    } else if ((w & 0x0E000000) == 0x0A000000) {
        op.kind = OP_BRANCH;
        op.reload = 1;
    } else if ((w & 0x0F000000) == 0x0F000000) {
        op.kind = OP_SWI;
    }
    if (op.kind == OP_PLAIN) op.cond = 0;
    return op;
}

static AgbOp thumb_op(uint32_t hw) {
    AgbOp op = {0};
    op.width = 4;
    if ((hw >> 10) == 0x10) {
        unsigned code = hw >> 6 & 15;
        if (code == 2 || code == 3 || code == 4 || code == 7) op.kind = OP_SHIFT; /* LSL, LSR, ASR, ROR by Rs */
        if (code == 13) {
            op.kind = OP_MUL;
            op.is_signed = 1;
        }
    } else if ((hw >> 10) == 0x11) {
        unsigned code = hw >> 8 & 3, rd = (hw & 7) | (hw >> 4 & 8);
        if (code == 3 || ((code == 0 || code == 2) && rd == 15)) {
            op.kind = OP_BRANCH;
            op.reload = 1;
        }
    } else if ((hw >> 11) == 0x09) {
        op.kind = OP_LOAD; /* LDR Rd, [PC, #imm] */
        op.words = 1;
    } else if ((hw >> 12) == 0x5) {
        op.words = 1;
        if (!(hw & 0x200)) {
            op.kind = hw & 0x800 ? OP_LOAD : OP_STORE;
            op.width = hw & 0x400 ? 1 : 4;
        } else {
            unsigned code = hw >> 10 & 3; /* STRH, LDRSB, LDRH, LDRSH */
            op.kind = code ? OP_LOAD : OP_STORE;
            op.width = code == 1 ? 1 : 2;
        }
    } else if ((hw >> 13) == 0x3) {
        op.kind = hw & 0x800 ? OP_LOAD : OP_STORE;
        op.width = hw & 0x1000 ? 1 : 4;
        op.words = 1;
    } else if ((hw >> 12) == 0x8) {
        op.kind = hw & 0x800 ? OP_LOAD : OP_STORE;
        op.width = 2;
        op.words = 1;
    } else if ((hw >> 12) == 0x9) {
        op.kind = hw & 0x800 ? OP_LOAD : OP_STORE;
        op.words = 1;
    } else if ((hw & 0xF600) == 0xB400) {
        unsigned list = hw & 0xFF, pc = hw >> 8 & 1;
        op.kind = hw & 0x800 ? OP_LOAD : OP_STORE;
        op.words = (uint8_t)(list || pc ? popcount16(list) + pc : 1);
        op.reload = op.kind == OP_LOAD && (pc || !list);
    } else if ((hw >> 12) == 0xC) {
        unsigned list = hw & 0xFF;
        op.kind = hw & 0x800 ? OP_LOAD : OP_STORE;
        op.words = (uint8_t)(list ? popcount16(list) : 1);
        op.reload = op.kind == OP_LOAD && !list;
    } else if ((hw >> 12) == 0xD) {
        unsigned cond = hw >> 8 & 15;
        if (cond == 0xF)
            op.kind = OP_SWI;
        else if (cond != 0xE) {
            op.kind = OP_BRANCH;
            op.reload = 1;
            op.thumb_cond = 1;
        }
    } else if ((hw >> 11) == 0x1C || (hw >> 11) == 0x1F) {
        op.kind = OP_BRANCH; /* B, and BL's second half */
        op.reload = 1;
    }
    return op;
}

/* the multiplier's internal cycles: one per byte from the low one up, until the rest of it is all zero (or all one
   for a signed one) */
static unsigned multiply_cycles(uint32_t multiplier, int is_signed) {
    uint32_t mask = 0xFFFFFF00u;
    unsigned n = 1;
    for (;;) {
        multiplier &= mask;
        if (!multiplier || (is_signed && multiplier == mask)) break;
        mask <<= 8;
        ++n;
    }
    return n;
}

/* the internal cycles before an instruction's data, and the ones after it */
static unsigned before_data(const AgbOp* op, uint32_t trace) {
    if (op->kind == OP_MUL) return multiply_cycles(trace, op->is_signed) + op->accumulate;
    if (op->kind == OP_MULL) return multiply_cycles(trace, op->is_signed) + 1 + op->accumulate;
    return op->shift;
}
static unsigned after_data(const AgbOp* op) { return op->kind == OP_LOAD || op->kind == OP_SWAP; }

uint32_t agb_replay_to(AgbReplay* replay, const AgbWaits* waits, int prefetch, const AgbBlockRun* run, uint32_t unit,
                       int32_t access) {
    unsigned width = run->thumb ? 2 : 4;
    AgbBus* bus = &replay->bus;
    while (replay->unit < unit || (replay->unit == unit && access != AGB_REPLAY_WHOLE && replay->done <= access)) {
        uint32_t pc = run->pc + replay->unit * width;
        const uint8_t* code = run->code + replay->unit * width;
        AgbOp op = run->thumb ? thumb_op(snx_ld_le16(code)) : arm_op(snx_ld_le32(code));
        uint32_t trace = run->trace[replay->unit];
        int skipped = op.cond && (run->skipped[replay->unit >> 5] >> (replay->unit & 31) & 1);
        /* Thumb's conditional branch that falls through to the next instruction did not branch */
        if (op.thumb_cond && run->next == pc + 2 && run->next_thumb) skipped = 1;
        if (replay->done == AGB_REPLAY_FETCH) {
            replay->cycles += agb_bus_fetch(bus, waits, prefetch, pc + 2 * width, width, bus->sequential);
            if (skipped || op.kind == OP_PLAIN) {
                bus->sequential = 1;
                ++replay->unit;
                continue;
            }
            unsigned idle = before_data(&op, trace);
            if (idle) {
                replay->cycles += agb_bus_idle(bus, prefetch, idle);
                bus->sequential = 0;
            }
            replay->done = 0;
            continue;
        }
        if (replay->done < (int32_t)op.words) {
            int write = op.kind == OP_STORE || (op.kind == OP_SWAP && replay->done == 1);
            uint32_t address = op.kind == OP_SWAP ? trace : trace + (uint32_t)replay->done * 4;
            if (op.width < 4 && op.kind != OP_SWAP) address = trace;
            /* the data's accesses are a first one, then (a multiple transfer's) sequential */
            replay->cycles += agb_bus_data(bus, waits, prefetch, address & ~(uint32_t)(op.width - 1), op.width, write,
                                           op.kind != OP_SWAP && replay->done > 0, pc + 2 * width);
            bus->sequential = 0;
            ++replay->done;
            continue;
        }
        /* the rest of it: an internal cycle after a load's data, the pipeline refilled after a branch */
        if (after_data(&op)) replay->cycles += agb_bus_idle(bus, prefetch, 1);
        if (op.kind == OP_SWI || op.reload) {
            uint32_t target = op.kind == OP_SWI ? 0x08 : run->next;
            unsigned next_width = op.kind == OP_SWI || !run->next_thumb ? 4 : 2;
            replay->cycles += agb_bus_fetch(bus, waits, prefetch, target, next_width, 0);
            replay->cycles += agb_bus_fetch(bus, waits, prefetch, target + next_width, next_width, 1);
            bus->sequential = 1;
        } else
            bus->sequential = op.kind == OP_PLAIN;
        replay->done = AGB_REPLAY_FETCH;
        ++replay->unit;
    }
    return replay->cycles;
}
