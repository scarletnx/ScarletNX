/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb/cpu/agb_cpu.h"
#include "agb/bios/bios.h"
#include "agb/board/board.h"
#include <stdio.h>
#include <string.h>

/* The handler returns to this address: the machine takes the interrupt's epilogue from there. */
#define AGB_IRQ_RETURN             0x00000004u
#define AGB_BIOS_FLAGS             0x03007FF8u /* the interrupt flags a game's handler sets for IntrWait */

typedef struct {
    AgbBoard* board;
    int16_t* audio;
    uint32_t capacity, written, elapsed; /* every slice of machine time, waits included */
} AgbSink;

static int bank_index(uint32_t cpsr) {
    switch (cpsr & 0x1F) {
        case 0x11: return 1; /* fiq */
        case 0x12: return 2; /* irq */
        case 0x13: return 3; /* svc */
        case 0x17: return 4; /* abt */
        case 0x1B: return 5; /* und */
        default: return 0;   /* usr and sys */
    }
}

/* The rotated immediate of the A32 data processing forms. */
static uint32_t expanded(uint32_t word) {
    unsigned rotate = (word >> 8 & 15) * 2;
    uint32_t value = word & 0xFF;
    return rotate ? (value >> rotate) | (value << (32 - rotate)) : value;
}

static void switch_mode(AgbCpu* cpu, uint32_t mode) {
    int old_mode = bank_index(cpu->arm.cpsr), new_mode = bank_index(mode);
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)0x1F) | (mode & 0x1F);
    if (old_mode == new_mode) return;
    cpu->banked_r13[old_mode] = cpu->arm.r[13];
    cpu->banked_r14[old_mode] = cpu->arm.r[14];
    /* r8 to r12 change banks only in FIQ mode */
    for (int i = 0; i < 5; ++i) {
        uint32_t* saved = old_mode == 1 ? cpu->fiq_r8 : cpu->usr_r8;
        saved[i] = cpu->arm.r[8 + i];
    }
    cpu->arm.r[13] = cpu->banked_r13[new_mode];
    cpu->arm.r[14] = cpu->banked_r14[new_mode];
    for (int i = 0; i < 5; ++i) cpu->arm.r[8 + i] = new_mode == 1 ? cpu->fiq_r8[i] : cpu->usr_r8[i];
}

/* An exception return, an S-bit write to the PC, copies the SPSR into the CPSR. User and system mode have no SPSR:
   there the write sets the flags as any other and the mode stays, as mGBA's _ARMModeHasSPSR has it */
static void exception_return(AgbCpu* cpu) {
    int bank = bank_index(cpu->arm.cpsr);
    if (!bank) return;
    uint32_t saved = cpu->spsr[bank];
    switch_mode(cpu, saved & 0x1F);
    cpu->arm.cpsr = saved;
}

/* The bus hook the recompiled blocks call for the regions with bus rules a plain access cannot follow:
   the I/O window, the palette, OAM and the save memory. This keeps the 1 KiB mirrors, the byte write
   widths and the register side effects correct while generated code runs at memory speed elsewhere.
   The machine's CPU state is its first member. */
static uint32_t bus(SnxArmCPU* arm, uint32_t address, uint32_t size_write, uint32_t value) {
    AgbBoard* board = ((AgbCpu*)arm)->board;
    unsigned bytes = size_write & 0xFF;
    if (!(size_write & 0x100)) board->open_bus = value; /* a read's value: the open bus (llvm.c's open_bus) */
    if (size_write & 0x200) {
        /* LDRSH: the ARM7TDMI quirk reads a sign extended byte at an odd address */
        if (address & 1) return (uint32_t)(int32_t)(int8_t)agb_read8(board, address);
        return (uint32_t)(int32_t)(int16_t)agb_read16(board, address);
    }
    if (size_write & 0x100) {
        uint32_t held = board->held_cycles;
        if (bytes == 1)
            agb_write8(board, address, (uint8_t)value);
        else if (bytes == 2)
            agb_write16(board, address, (uint16_t)value);
        else
            agb_write32(board, address, value);
        /* a DMA the write started holds the bus: the block ends after this instruction, and the next one runs once
           the CPU waited it out (agb_cpu_run), as the console's does */
        return board->held_cycles != held;
    }
    return bytes == 1 ? agb_read8(board, address) : bytes == 2 ? agb_read16(board, address)
                                                             : agb_read32(board, address);
}

void agb_cpu_init(AgbCpu* cpu, AgbBoard* board, uint8_t* map) {
    *cpu = (AgbCpu){0};
    cpu->board = board;
    cpu->arm.mem = map; /* the flat window: one pointer add per guest access */
    cpu->arm.mem_size = AGB_MAP_SIZE + AGB_MAP_GUARD;
    cpu->arm.io = bus;
    /* the boot state the BIOS leaves behind: system mode with every stack set up */
    cpu->arm.cpsr = 0x1F;
    cpu->arm.r[13] = 0x03007F00;
    for (int i = 0; i < 6; ++i) cpu->banked_r13[i] = 0x03007F00;
    cpu->banked_r13[2] = 0x03007FA0;
    cpu->banked_r13[3] = 0x03007FE0;
    /* the BIOS jumps to the cartridge's first word, the branch its header starts with, or to a multiboot program */
    cpu->arm.r[15] = agb_board_entry(board);
    /* the first run takes the frame the boot ended in up to its vertical blank, line 160, where mGBA's frame ends: every
       run then ends there, its picture whole, and the next run's keys come as the blank starts */
    cpu->over =
        (uint32_t)((board->ppu.vcount + AGB_FRAME_LINES - AGB_SCREEN_HEIGHT) % AGB_FRAME_LINES) * AGB_SCANLINE_CYCLES +
        board->ppu.dot;
    cpu->costs_waitcnt = 0x10000;    /* none made yet: the first block's run makes them */
    cpu->bus_costs = &cpu->costs[0]; /* a block run on its own reads one, all zero */
}

static void advance(AgbCpu* cpu, AgbSink* sink, uint32_t cycles) {
    AgbBoard* board = sink->board;
    (void)cpu;
    agb_ppu_step(board, cycles);
    agb_timer_step(board, cycles);
    agb_save_step(board, cycles);
    if (sink->audio && sink->written < sink->capacity)
        sink->written += agb_apu_run(board, cycles, sink->audio + sink->written * 2, sink->capacity - sink->written);
    sink->elapsed += cycles;
}

/* ---- interrupts ---- */

/* the 14 interrupt lines, IE's and IF's bits 0 to 13: the DMAs' (8 to 11), the keypad's and the cartridge's too */
static int irq_pending(AgbBoard* board) { return (board->irq_ime & 1) && (board->irq_ie & board->irq_if & 0x3FFF); }

/* The interrupt entry the BIOS stub performs: the live registers land on the IRQ stack, the handler at
   0x03007FFC runs, and its return reaches the epilogue the machine services. Returns 0 while no handler
   is installed yet, so the caller keeps the machine moving. */
static int irq_enter(AgbBoard* board, AgbCpu* cpu) {
    uint32_t handler = agb_read32(board, 0x03007FFC);
    uint32_t saved_cpsr = cpu->arm.cpsr, next = cpu->arm.r[15], sp;
    if (!handler) return 0;
    switch_mode(cpu, 0x12);
    cpu->spsr[2] = saved_cpsr;
    cpu->arm.r[14] = next + 4; /* hardware sets lr to the interrupted pc plus four */
    sp = cpu->arm.r[13] - 24;
    agb_write32(board, sp, cpu->arm.r[0]);
    agb_write32(board, sp + 4, cpu->arm.r[1]);
    agb_write32(board, sp + 8, cpu->arm.r[2]);
    agb_write32(board, sp + 12, cpu->arm.r[3]);
    agb_write32(board, sp + 16, cpu->arm.r[12]);
    agb_write32(board, sp + 20, cpu->arm.r[14]);
    cpu->arm.r[13] = sp;
    cpu->arm.r[0] = 0x04000000;                                    /* the stub hands the handler the I/O base */
    cpu->arm.r[14] = AGB_IRQ_RETURN;                               /* the handler returns to the epilogue */
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T) | 0x80; /* ARM state, interrupts off */
    cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T) | (handler & 1 ? SNX_ARM_T : 0);
    cpu->arm.r[15] = handler & ~(uint32_t)1;
    board->bios_prefetch = AGB_BIOS_PREFETCH_IRQ;
    return 1;
}

/* The other half of the BIOS stub: the saved registers come back and the interrupted code resumes. */
static void irq_return(AgbBoard* board, AgbCpu* cpu) {
    uint32_t sp = cpu->arm.r[13], saved_cpsr = cpu->spsr[2], back;
    cpu->arm.r[0] = agb_read32(board, sp);
    cpu->arm.r[1] = agb_read32(board, sp + 4);
    cpu->arm.r[2] = agb_read32(board, sp + 8);
    cpu->arm.r[3] = agb_read32(board, sp + 12);
    cpu->arm.r[12] = agb_read32(board, sp + 16);
    back = agb_read32(board, sp + 20);
    cpu->arm.r[13] = sp + 24;
    switch_mode(cpu, saved_cpsr & 0x1F);
    cpu->arm.cpsr = saved_cpsr;
    cpu->arm.r[15] = back - 4;
    board->bios_prefetch = AGB_BIOS_PREFETCH_IRQ_RETURN;
}

/* ---- software interrupts and the privileged residue ---- */

/* SoftReset, as the BIOS runs it: the top 0x200 bytes of IWRAM (the stacks, the BIOS's flags and its IRQ vector)
   cleared, r0 to r12 zero, the irq and svc stacks set with their lr and spsr cleared, then system mode in ARM state
   from EWRAM's start when the byte at 0x03007FFA asks for it (a game that copied a program there, as multiboot does),
   else from the cartridge's */
static void soft_reset(AgbBoard* board, AgbCpu* cpu) {
    uint32_t entry = agb_read8(board, 0x03007FFA) ? 0x02000000u : 0x08000000u;
    switch_mode(cpu, 0x1F);
    memset(board->memory.iwram + AGB_IWRAM_SIZE - 0x200, 0, 0x200);
    for (int i = 0; i < 13; ++i) cpu->arm.r[i] = 0;
    cpu->arm.r[13] = 0x03007F00;
    cpu->banked_r13[2] = 0x03007FA0;
    cpu->banked_r13[3] = 0x03007FE0;
    cpu->banked_r14[2] = cpu->banked_r14[3] = cpu->spsr[2] = cpu->spsr[3] = 0;
    cpu->arm.cpsr = 0x1F;
    cpu->arm.r[15] = entry;
}

/* The BIOS's entry to a call that waits, as the console's has it: the caller's CPSR, r11, r12 and the return address
   go on the supervisor stack, the call runs in system mode in ARM state with interrupts as the caller had them, and r2
   and lr go on the caller's stack. An interrupt handler that runs while the call waits works below them, and what it
   leaves on the stack sits that much lower */
static void swi_enter(AgbBoard* board, AgbCpu* cpu) {
    uint32_t caller = cpu->arm.cpsr, back = cpu->arm.r[15], sp;
    switch_mode(cpu, 0x13);
    cpu->spsr[3] = caller;
    cpu->arm.r[14] = back;
    sp = cpu->arm.r[13] - 16;
    agb_write32(board, sp, caller);
    agb_write32(board, sp + 4, cpu->arm.r[11]);
    agb_write32(board, sp + 8, cpu->arm.r[12]);
    agb_write32(board, sp + 12, back);
    cpu->arm.r[13] = sp;
    switch_mode(cpu, 0x1F);
    cpu->arm.cpsr &= ~(uint32_t)SNX_ARM_T;
    sp = cpu->arm.r[13] - 8;
    agb_write32(board, sp, cpu->arm.r[2]);
    agb_write32(board, sp + 4, cpu->arm.r[14]);
    cpu->arm.r[13] = sp;
    cpu->arm.r[11] = (caller & 0x80) | 0x1F;
    cpu->arm.r[14] = AGB_BIOS_SWI_RETURN;
}

/* The other half, at AGB_BIOS_SWI_RETURN: r2 and lr come back, then what the supervisor stack holds, and the caller
   resumes */
static void swi_return(AgbBoard* board, AgbCpu* cpu) {
    uint32_t sp = cpu->arm.r[13], caller, back;
    cpu->arm.r[2] = agb_read32(board, sp);
    cpu->arm.r[14] = agb_read32(board, sp + 4);
    cpu->arm.r[13] = sp + 8;
    switch_mode(cpu, 0x13);
    sp = cpu->arm.r[13];
    caller = agb_read32(board, sp);
    cpu->arm.r[11] = agb_read32(board, sp + 4);
    cpu->arm.r[12] = agb_read32(board, sp + 8);
    back = agb_read32(board, sp + 12);
    cpu->arm.r[13] = sp + 16;
    cpu->spsr[3] = caller;
    cpu->arm.r[14] = back;
    switch_mode(cpu, caller & 0x1F);
    cpu->arm.cpsr = caller;
    cpu->arm.r[15] = back;
    board->bios_prefetch = AGB_BIOS_PREFETCH_SWI;
}

/* IntrWait's look at the BIOS's flags: the interrupts r1 names that the game's handler flagged come out of them and
   into r0, the rest stay in r2, and IME is on after it */
static uint32_t intr_check(AgbBoard* board, AgbCpu* cpu) {
    uint16_t flags = agb_read16(board, AGB_BIOS_FLAGS), done = flags & (uint16_t)cpu->arm.r[1];
    if (done) agb_write16(board, AGB_BIOS_FLAGS, flags ^ done);
    cpu->arm.r[0] = done;
    cpu->arm.r[2] = flags ^ done;
    board->irq_ime = 1;
    return done;
}

/* IntrWait's loop, at AGB_BIOS_INTR_WAIT: over once a handler flagged an interrupt it waits for, when r4 and its
   return come back from the caller's stack; else it halts for the next interrupt */
static void intr_wait(AgbBoard* board, AgbCpu* cpu) {
    cpu->arm.r[14] = AGB_BIOS_INTR_WAIT + 4;
    if (!intr_check(board, cpu)) {
        board->cpu_halted = 1;
        return;
    }
    cpu->arm.r[4] = agb_read32(board, cpu->arm.r[13]);
    cpu->arm.r[14] = agb_read32(board, cpu->arm.r[13] + 4);
    cpu->arm.r[13] += 8;
    cpu->arm.r[15] = cpu->arm.r[14] & ~(uint32_t)3;
}

static void handle_swi(AgbCpu* cpu, AgbSink* sink, uint32_t syndrome, int thumb) {
    AgbBoard* board = sink->board;
    uint32_t number = thumb ? syndrome & 0xFF : (syndrome >> 16) & 0xFF;
    board->bios_prefetch = AGB_BIOS_PREFETCH_SWI;
    if (number == AGB_SWI_SOFT_RESET) {
        soft_reset(board, cpu);
        return;
    }
    if (number == AGB_SWI_HALT || number == AGB_SWI_STOP) {
        /* halted inside the BIOS, r2 zero and the I/O base in r12 as its routine has them: the interrupt that wakes
           the machine is taken there, then the call returns */
        swi_enter(board, cpu);
        cpu->arm.r[2] = 0;
        cpu->arm.r[12] = 0x04000000;
        cpu->arm.r[15] = AGB_BIOS_HALT;
        board->cpu_halted = 1;
        return;
    }
    if (number == AGB_SWI_INTR_WAIT || number == AGB_SWI_VBLANK_INTR_WAIT) {
        /* the BIOS's way: r4 and its return on the caller's stack below the entry's two, the flags asked for (r1)
           dropped first when r0 says so (VBlankIntrWait always), then it halts with IME on until the game's interrupt
           handler sets one of them in its flags at 0x03007FF8 (intr_wait). The hardware's IF is the handler's to
           acknowledge. It waits with r3 = 0, r4 = 1 and the I/O base in r12, which an interrupt handler that runs
           meanwhile may use */
        if (number == AGB_SWI_VBLANK_INTR_WAIT) cpu->arm.r[0] = cpu->arm.r[1] = 1;
        int discard = cpu->arm.r[0] != 0;
        swi_enter(board, cpu);
        cpu->arm.r[13] -= 8;
        agb_write32(board, cpu->arm.r[13], cpu->arm.r[4]);
        agb_write32(board, cpu->arm.r[13] + 4, cpu->arm.r[14]);
        cpu->arm.r[3] = 0;
        cpu->arm.r[4] = 1;
        cpu->arm.r[12] = 0x04000000;
        cpu->arm.r[15] = AGB_BIOS_INTR_WAIT;
        if (discard) {
            intr_check(board, cpu);
            cpu->arm.r[14] = AGB_BIOS_INTR_WAIT - 4;
            board->cpu_halted = 1;
        }
        return;
    }
    /* the BIOS's routine takes its time: its entry and return (45 cycles, as mGBA's HLE BIOS counts them) and the
       memory it streams through, which the CPU spends before its next instruction */
    board->counting = 1;
    board->counted_cycles = 0;
    int ran = agb_bios_call(board, number, cpu->arm.r);
    board->counting = 0;
    board->held_cycles += 45 + board->counted_cycles;
    if (ran < 0) {
        cpu->unknown_swi = number;
        cpu->unknown_swi_at = cpu->arm.r[15] - (thumb ? 2u : 4u); /* the PC is past the instruction already */
        ++cpu->unknown_swi_count;
    }
}

/* The recompiler refuses the privileged instructions of the ARM7TDMI. The machine interprets them
   against the banked state and resumes at the next instruction. Operands reading the PC get the
   pipelined value the instruction sees. */
static uint32_t operand(AgbCpu* cpu, uint32_t index) {
    return index == 15 ? cpu->arm.r[15] + 8 : cpu->arm.r[index];
}

/* The recompiler refuses the instructions the ARM7TDMI defines differently from ARMv8-A, and the
   unpredictable forms the ARM7TDMI still runs. The machine computes them here against the banked
   state and resumes at the next instruction. Operands reading the PC take the pipelined value, and
   the register shifted form takes the ARM7TDMI's PC + 12. */

static uint32_t shift_register(uint32_t value, unsigned type, uint32_t amount, uint32_t* carry) {
    amount &= 0xFF;
    switch (type) {
        case 0: /* LSL */
            if (!amount) break;
            if (amount < 32) {
                *carry = value >> (32 - amount) & 1;
                value <<= amount;
            } else {
                *carry = amount == 32 ? value & 1 : 0;
                value = 0;
            }
            break;
        case 1: /* LSR */
            if (!amount) break;
            if (amount < 32) {
                *carry = value >> (amount - 1) & 1;
                value >>= amount;
            } else {
                *carry = amount == 32 ? value >> 31 & 1 : 0;
                value = 0;
            }
            break;
        case 2: /* ASR */
            if (!amount) break;
            if (amount < 32) {
                *carry = value >> (amount - 1) & 1;
                value = (uint32_t)((int32_t)value >> amount);
            } else {
                *carry = value >> 31 & 1;
                value = (int32_t)value < 0 ? 0xFFFFFFFFu : 0;
            }
            break;
        default: /* ROR */
            if (!amount) break;
            amount &= 31;
            if (amount) {
                *carry = value >> (amount - 1) & 1;
                value = value >> amount | value << (32 - amount);
            } else
                *carry = value >> 31 & 1;
            break;
    }
    return value;
}

static int data_quirk(AgbCpu* cpu, uint32_t word) {
    unsigned opcode = word >> 21 & 15, set = word >> 20 & 1, rn = word >> 16 & 15, rd = word >> 12 & 15;
    uint32_t carry = cpu->arm.cpsr >> 29 & 1, overflow = cpu->arm.cpsr >> 28 & 1, result = 0;
    uint32_t bias = word & 0x10 ? 12 : 8; /* the register shifted form reads the PC + 12 */
    uint32_t a = rn == 15 ? cpu->arm.r[15] + bias : cpu->arm.r[rn];
    uint32_t value;
    uint64_t wide;
    if (word & (1u << 25))
        value = expanded(word);
    else {
        value = (word & 15) == 15 ? cpu->arm.r[15] + bias : cpu->arm.r[word & 15];
        value = shift_register(value, word >> 5 & 3, word & 0x10 ? cpu->arm.r[word >> 8 & 15] : word >> 7 & 31,
                              &carry);
    }
    switch (opcode) {
        case 0: result = a & value; break;
        case 1: result = a ^ value; break;
        case 2: wide = (uint64_t)a - value; result = (uint32_t)wide; carry = a >= value;
            overflow = (a ^ value) & (a ^ result) & 0x80000000u ? 1 : 0; break;
        case 3: wide = (uint64_t)value - a; result = (uint32_t)wide; carry = value >= a;
            overflow = (a ^ value) & (value ^ result) & 0x80000000u ? 1 : 0; break;
        case 4: wide = (uint64_t)a + value; result = (uint32_t)wide; carry = wide >> 32 ? 1 : 0;
            overflow = ~(a ^ value) & (a ^ result) & 0x80000000u ? 1 : 0; break;
        case 5: wide = (uint64_t)a + value + (cpu->arm.cpsr >> 29 & 1); result = (uint32_t)wide;
            carry = wide >> 32 ? 1 : 0; overflow = ~(a ^ value) & (a ^ result) & 0x80000000u ? 1 : 0; break;
        case 6: wide = (uint64_t)a - value - 1 + (cpu->arm.cpsr >> 29 & 1); result = (uint32_t)wide;
            carry = (uint64_t)a >= (uint64_t)value + 1 - (cpu->arm.cpsr >> 29 & 1);
            overflow = (a ^ value) & (a ^ result) & 0x80000000u ? 1 : 0; break;
        case 7: wide = (uint64_t)value - a - 1 + (cpu->arm.cpsr >> 29 & 1); result = (uint32_t)wide;
            carry = (uint64_t)value >= (uint64_t)a + 1 - (cpu->arm.cpsr >> 29 & 1);
            overflow = (a ^ value) & (value ^ result) & 0x80000000u ? 1 : 0; break;
        case 8: result = a & value; break;
        case 9: result = a ^ value; break;
        case 10: wide = (uint64_t)a - value; result = (uint32_t)wide; carry = a >= value;
            overflow = (a ^ value) & (a ^ result) & 0x80000000u ? 1 : 0; break;
        case 11: wide = (uint64_t)a + value; result = (uint32_t)wide; carry = wide >> 32 ? 1 : 0;
            overflow = ~(a ^ value) & (a ^ result) & 0x80000000u ? 1 : 0; break;
        case 12: result = a | value; break;
        case 13: result = value; break;
        case 14: result = a & ~value; break;
        default: result = ~value; break;
    }
    if (set) {
        uint32_t flags = cpu->arm.cpsr;
        int arithmetic = (opcode >= 2 && opcode <= 7) || opcode == 10 || opcode == 11;
        flags &= 0x0FFFFFFFu;
        if (result & 0x80000000u) flags |= 0x80000000u;
        if (!result) flags |= 0x40000000u;
        if (carry) flags |= 0x20000000u;
        if (arithmetic) {
            if (overflow) flags |= 0x10000000u;
        } else
            flags |= cpu->arm.cpsr & 0x10000000u; /* the logical operations keep V */
        cpu->arm.cpsr = flags;
    }
    if (opcode >= 8 && opcode <= 11) {
        /* The test operations write no result and the ARM7TDMI ignores their Rd field, but with the S
           bit and Rd=15 they still restore the SPSR, changing the mode without touching the PC. */
        if (rd == 15 && set) exception_return(cpu);
        cpu->arm.r[15] += 4;
        return 0;
    }
    if (rd == 15) {
        if (set) exception_return(cpu);
        cpu->arm.r[15] = result & ~(uint32_t)1;
    } else {
        cpu->arm.r[rd] = result;
        cpu->arm.r[15] += 4;
    }
    return 0;
}

/* The transfers the recompiler rejects: the base in the destination, the base in the writeback. The
   ARM7TDMI runs them with its own ordering, and stores the PC + 12. */
static int transfer_quirk(AgbBoard* board, AgbCpu* cpu, uint32_t word, unsigned n, unsigned d, int load,
                          unsigned bytes, int sign, uint32_t offset) {
    int pre = !!(word & (1u << 24)), up = !!(word & (1u << 23)), wb = !pre || !!(word & (1u << 21));
    uint32_t base = n == 15 ? cpu->arm.r[15] + 8 : cpu->arm.r[n];
    uint32_t adjusted = up ? base + offset : base - offset;
    uint32_t address = pre ? adjusted : base, value = 0, mis;
    if (load) {
        address &= ~(uint32_t)(bytes - 1); /* the single transfer reads the aligned unit and rotates */
        value = bytes == 1   ? agb_read8(board, address)
                : bytes == 2 ? agb_read16(board, address)
                             : agb_read32(board, address);
        mis = ((address ^ (pre ? adjusted : base)) & (bytes - 1)) * 8;
        if (mis) value = value >> mis | value << (32 - mis);
        if (sign) value = bytes == 1 ? (uint32_t)(int32_t)(int8_t)value : (uint32_t)(int32_t)(int16_t)value;
    } else {
        address &= ~(uint32_t)(bytes - 1);
        value = d == 15 ? cpu->arm.r[15] + 12 : cpu->arm.r[d];
    }
    if (!load) {
        if (bytes == 1)
            agb_write8(board, address, (uint8_t)value);
        else if (bytes == 2)
            agb_write16(board, address, (uint16_t)value);
        else
            agb_write32(board, address, value);
    }
    if (wb) cpu->arm.r[n] = adjusted;
    if (load) cpu->arm.r[d] = value;
    cpu->arm.r[15] += 4;
    return 0;
}

static int interpret(AgbBoard* board, AgbCpu* cpu, uint32_t word, int thumb) {
    if (thumb) {
        /* the ARM7TDMI quirk: an empty register list transfers R15 and moves the base by 0x40 */
        if ((word & 0xF000) == 0xC000 && !(word & 0xFF)) {
            uint32_t base = cpu->arm.r[word >> 8 & 7];
            if (word & 0x0800) {
                cpu->arm.r[15] = agb_read32(board, base) & ~(uint32_t)1;
            } else {
                agb_write32(board, base, cpu->arm.r[15] + 6); /* the thumb quirk stores the PC + 6 */
                cpu->arm.r[15] += 2;
            }
            cpu->arm.r[word >> 8 & 7] = base + 0x40;
            return 0;
        }
        return -1;
    }
    if ((word & 0x0fbf0fff) == 0x010f0000) { /* MRS Rd, CPSR or SPSR */
        unsigned rd = word >> 12 & 15;
        cpu->arm.r[rd] = word & (1u << 22) ? cpu->spsr[bank_index(cpu->arm.cpsr)] : cpu->arm.cpsr;
        cpu->arm.r[15] += 4;
        return 0;
    }
    if ((word & 0x0fb0fff0) == 0x0120f000 || (word & 0x0fb0f000) == 0x0320f000) { /* MSR */
        unsigned fields = word >> 16 & 15;
        uint32_t value = word & (1u << 25) ? expanded(word) : operand(cpu, word & 15);
        uint32_t bits = 0;
        if (fields & 8) bits |= 0xFF000000;
        if (fields & 4) bits |= 0x00FF0000;
        if (fields & 2) bits |= 0x0000FF00;
        if (fields & 1) bits |= 0x000000FF;
        if (word & (1u << 22)) {
            uint32_t* spsr = &cpu->spsr[bank_index(cpu->arm.cpsr)];
            *spsr = (*spsr & ~bits) | (value & bits);
        } else {
            uint32_t updated = (cpu->arm.cpsr & ~bits) | (value & bits);
            if ((updated & 0x1F) != (cpu->arm.cpsr & 0x1F)) switch_mode(cpu, updated & 0x1F);
            cpu->arm.cpsr = updated;
            if (updated & SNX_ARM_T) {
                /* the T bit set this way switches to Thumb with the next word already fetched as ARM, which runs as a
                   no-op: Thumb starts two words on, as mGBA's MSR has it (a packer's check of the machine) */
                cpu->arm.r[15] += 8;
                return 0;
            }
        }
        cpu->arm.r[15] += 4;
        return 0;
    }
    if ((word & 0x0fb00ff0) == 0x01000090) { /* SWP and SWPB */
        uint32_t address = operand(cpu, word >> 16 & 15), held = cpu->arm.r[word & 15];
        unsigned rd = word >> 12 & 15, mis;
        if (word & (1u << 22)) {
            cpu->arm.r[rd] = agb_read8(board, address);
            agb_write8(board, address, (uint8_t)held);
        } else {
            uint32_t value = agb_read32(board, address & ~(uint32_t)3);
            mis = (address & 3) * 8; /* the misaligned swap rotates the aligned word */
            if (mis) value = value >> mis | value << (32 - mis);
            cpu->arm.r[rd] = value;
            agb_write32(board, address & ~(uint32_t)3, held);
        }
        cpu->arm.r[15] += 4;
        return 0;
    }
    if ((word & 0x0E000090) == 0x00000090 && (word & 0x60) && (word >> 28) != 15) {
        /* halfword and signed transfers */
        unsigned n = word >> 16 & 15, d = word >> 12 & 15, op = word >> 5 & 3;
        int load = (word >> 20) & 1;
        unsigned bytes = op == 2 ? 1 : 2, sign = op >= 2;
        return transfer_quirk(board, cpu, word, n, d, load, bytes, sign,
                              (uint32_t)(((word >> 4) & 0xF0) | (word & 15)));
    }
    if ((word & 0x0C000000) == 0x04000000 && (word >> 28) != 15) {
        unsigned n = word >> 16 & 15, d = word >> 12 & 15, load = (word >> 20) & 1;
        uint32_t offset = word & 0xFFF;
        if (word & (1u << 25)) {
            uint32_t carry = 0;
            offset = shift_register(cpu->arm.r[word & 15], word >> 5 & 3, word >> 7 & 31, &carry);
        }
        return transfer_quirk(board, cpu, word, n, d, load, word & (1u << 22) ? 1 : 4, 0, offset);
    }
    if ((word & 0x0E000000) == 0x08000000 && (word >> 28) != 15) {
        /* The list transfers the recompiler rejects: the S bit form moves the user bank registers, the
           base in the list stores the written back base unless it comes first, and the empty list
           transfers R15 and moves the base by 0x40. List transfers go to the exact addresses. */
        unsigned n = word >> 16 & 15, list = word & 0xFFFF, count = 0, index = 0;
        int load = (word >> 20) & 1, pre = !!(word & (1u << 24)), up = !!(word & (1u << 23));
        int wb = !!(word & (1u << 21)), sbit = !!(word & (1u << 22));
        uint32_t base = n == 15 ? cpu->arm.r[15] + 8 : cpu->arm.r[n];
        if (!list) {
            /* the empty list transfers R15 where the first of 16 registers would go, and moves the base by
               0x40 in its direction */
            uint32_t address = up ? base + (pre ? 4 : 0) : base - 0x40 + (pre ? 0 : 4);
            if (load)
                cpu->arm.r[15] = agb_read32(board, address) & ~(uint32_t)1;
            else {
                agb_write32(board, address, cpu->arm.r[15] + 12);
                cpu->arm.r[15] += 4;
            }
            cpu->arm.r[n] = up ? base + 0x40 : base - 0x40;
            return 0;
        }
        for (unsigned i = 0; i < 16; ++i) count += list >> i & 1;
        uint32_t adjusted = up ? base + count * 4 : base - count * 4;
        uint32_t start = up ? base + (pre ? 4 : 0) : adjusted + (pre ? 0 : 4);
        for (unsigned i = 0; i < 16; ++i) {
            if (!(list >> i & 1)) continue;
            uint32_t address = start + index++ * 4;
            if (load) {
                uint32_t value = agb_read32(board, address);
                if (sbit && i != 15) {
                    /* the user bank, whichever mode the machine is in */
                    if (i == 13 || i == 14) {
                        if (bank_index(cpu->arm.cpsr)) (i == 13 ? cpu->banked_r13 : cpu->banked_r14)[0] = value;
                        else
                            cpu->arm.r[i] = value;
                    } else if (i >= 8 && i < 13 && bank_index(cpu->arm.cpsr) == 1)
                        cpu->usr_r8[i - 8] = value;
                    else
                        cpu->arm.r[i] = value;
                } else if (i == 15) {
                    if (sbit) exception_return(cpu);
                    cpu->arm.r[15] = value & ~(uint32_t)1;
                } else
                    cpu->arm.r[i] = value;
            } else {
                uint32_t value;
                if (i == 15)
                    value = cpu->arm.r[15] + 12;
                else if (sbit) {
                    if (i == 13 || i == 14)
                        value = bank_index(cpu->arm.cpsr) ? (i == 13 ? cpu->banked_r13 : cpu->banked_r14)[0]
                                                          : cpu->arm.r[i];
                    else if (i >= 8 && i < 13 && bank_index(cpu->arm.cpsr) == 1)
                        value = cpu->usr_r8[i - 8];
                    else
                        value = cpu->arm.r[i];
                } else
                    value = cpu->arm.r[i];
                /* the base stores the written back value unless it comes first in the list */
                if (i == n && wb && (list & ((1u << i) - 1))) value = adjusted;
                agb_write32(board, address, value);
            }
        }
        /* the writeback is lost when a load brings the base in the list: the loaded value stands */
        if (wb && !(load && (list >> n & 1))) cpu->arm.r[n] = adjusted;
        if (!(load && (list >> 15 & 1))) cpu->arm.r[15] += 4;
        return 0;
    }
    if ((word & 0x0C000000) == 0 && (word >> 28) != 15) return data_quirk(cpu, word);
    return -1;
}

/* The bus costs of the loads and stores of code running from each region (arm.h's SnxArmBusCosts), for the WAITCNT
   the board has, as mGBA counts them (gba/memory.c: GBALoad*, GBAStore*, GBAMemoryStall): a first access 1 + its wait
   states, each next word of a multiple one 1 + a sequential word's, then the fetch after it a non-sequential one. With
   the prefetch buffer on, code on the cartridge has its next halfwords fetched while the CPU reaches other memory: the
   instruction's time there hides up to that many fetches, 8 at most (1 + 8 sequential wait states), and its fetch
   after it is one of them */
static void bus_costs(AgbCpu* cpu, const AgbBoard* board) {
    int prefetch = (board->io[0x204 >> 1] & 0x4000) != 0;
    for (unsigned code = 0; code < 16; ++code) {
        SnxArmBusCosts* c = &cpu->costs[code];
        int n = (int)agb_access_cycles(board, code << 24, 16, 0) - 1,
            s = (int)agb_access_cycles(board, code << 24, 16, 1) - 1;
        int cartridge = code >= 0x8 && code <= 0xD;
        for (unsigned region = 0; region < 16; ++region) {
            c->nonseq[region][0] = (uint8_t)agb_access_cycles(board, region << 24, 8, 0);
            c->nonseq[region][1] = (uint8_t)agb_access_cycles(board, region << 24, 16, 0);
            c->nonseq[region][2] = c->nonseq[region][3] = (uint8_t)agb_access_cycles(board, region << 24, 32, 0);
            c->seq[region] = (uint8_t)agb_access_cycles(board, region << 24, 32, 1);
            c->adjust[region] = (int8_t)(cartridge && prefetch && region < 0x8 ? -(1 + 8 * s) : n - s);
        }
    }
    cpu->costs_waitcnt = board->io[0x204 >> 1];
}

/* The cycles a block took, as mGBA counts an instruction's: each its fetch, 1 + a sequential access's wait states
   (GBATEK's memory timings by region and WAITCNT), whatever the prefetch buffer; a jump to next (a branch taken, not
   the instruction after the block) a refill of the pipeline there, a non-sequential and a sequential fetch and two
   cycles; and what its loads and stores counted (bus_costs) */
static uint32_t block_cycles(AgbCpu* cpu, const AgbBoard* board, const AgbBlock* block, uint32_t next) {
    unsigned width = block->thumb ? 16 : 32;
    uint32_t fallthrough = block->pc + (uint32_t)block->instructions * (width / 8);
    uint32_t cycles = (uint32_t)block->instructions * agb_access_cycles(board, block->pc, width, 1) + cpu->bus_cycles;
    unsigned next_width = cpu->arm.cpsr & SNX_ARM_T ? 16 : 32;
    if (next != fallthrough)
        cycles += agb_access_cycles(board, next, next_width, 0) + agb_access_cycles(board, next, next_width, 1);
    cpu->bus_cycles = 0;
    return cycles;
}

int agb_cpu_run(AgbBoard* board, AgbCpu* cpu, AgbRecomp* recomp, uint32_t cycles, int16_t* audio,
                uint32_t audio_capacity, uint32_t* audio_written, char* error, size_t error_size) {
    AgbSink sink = {board, audio, audio_capacity, 0, cpu->over};
    while (sink.elapsed < cycles) {
        /* the ARM7TDMI holds R15's bits 1 and 0 at zero in ARM state: a jump there with bit 1 set (a Thumb BX PC two
           bytes past a word) runs the word below */
        if (!(cpu->arm.cpsr & SNX_ARM_T)) cpu->arm.r[15] &= ~(uint32_t)3;
        /* the CPU waits while a DMA holds the bus: the time its transfers took passes before the next instruction, up
           to this run's end, the rest in the next run's, so a run ends where the machine's frame does and gives a
           frame's sound. A DMA that ran while the CPU was halted took none of the CPU's time */
        if (board->held_cycles && board->cpu_halted) board->held_cycles = 0;
        if (board->held_cycles) {
            uint32_t room = cycles - sink.elapsed, held = board->held_cycles < room ? board->held_cycles : room;
            board->held_cycles -= held;
            advance(cpu, &sink, held);
            continue;
        }
        if (cpu->arm.r[15] == AGB_BIOS_INTR_WAIT && !board->cpu_halted) intr_wait(board, cpu);
        if (board->cpu_halted && (board->irq_if & board->irq_ie)) board->cpu_halted = 0;
        if (board->cpu_halted) {
            advance(cpu, &sink, 64);
            continue;
        }
        if (irq_pending(board) && !(cpu->arm.cpsr & 0x80)) {
            if (!irq_enter(board, cpu)) advance(cpu, &sink, 64);
            continue;
        }
        if (cpu->arm.r[15] == AGB_IRQ_RETURN) {
            irq_return(board, cpu);
            continue;
        }
        if (cpu->arm.r[15] == AGB_BIOS_HALT) cpu->arm.r[15] = AGB_BIOS_SWI_RETURN; /* Halt awake: bx lr */
        if (cpu->arm.r[15] == AGB_BIOS_SWI_RETURN) {
            swi_return(board, cpu);
            continue;
        }
        /* IntrWait awake with no interrupt it may take: the BIOS's loop goes round */
        if (cpu->arm.r[15] == AGB_BIOS_INTR_WAIT) {
            advance(cpu, &sink, 64);
            continue;
        }
        if (cpu->arm.r[15] == AGB_BIOS_NOP_CALL) { /* a sound driver function SoundDriverGetJumpList named: bx lr */
            cpu->arm.cpsr = (cpu->arm.cpsr & ~(uint32_t)SNX_ARM_T) | (cpu->arm.r[14] & 1 ? SNX_ARM_T : 0);
            cpu->arm.r[15] = cpu->arm.r[14] & ~(uint32_t)1;
            continue;
        }
        const AgbBlock* block =
            agb_recomp_lookup(recomp, board, cpu->arm.r[15], (cpu->arm.cpsr & SNX_ARM_T) != 0, error, error_size);
        if (!block) return -1;
        if (cpu->costs_waitcnt != board->io[0x204 >> 1]) bus_costs(cpu, board);
        cpu->bus_costs = &cpu->costs[block->pc >> 24 & 0xF];
        cpu->bus_cycles = 0;
        cpu->arm.exit = SNX_ARM_OK;
        block->fn(&cpu->arm);
        advance(cpu, &sink, block_cycles(cpu, board, block, cpu->arm.r[15]));
        if (cpu->arm.exit == SNX_ARM_SVC)
            handle_swi(cpu, &sink, cpu->arm.syndrome, block->thumb);
        else if (cpu->arm.exit == SNX_ARM_UNDEFINED) {
            if (interpret(board, cpu, cpu->arm.syndrome, block->thumb)) {
                snprintf(error, error_size, "unimplemented instruction 0x%08X at 0x%08X", cpu->arm.syndrome,
                         cpu->arm.r[15]);
                return -1;
            }
        } else if (cpu->arm.exit != SNX_ARM_OK) {
            snprintf(error, error_size, "the recompiled ROM stopped (exit %u) at 0x%08X (syndrome 0x%08X)",
                     cpu->arm.exit, cpu->arm.r[15], cpu->arm.syndrome);
            return -1;
        }
    }
    cpu->over = sink.elapsed - cycles;
    if (audio_written) *audio_written = sink.written;
    return 0;
}
