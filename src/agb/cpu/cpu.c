/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "agb/cpu/agb_cpu.h"
#include "agb/bios/bios.h"
#include "agb/bios/bios_time.h"
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

/* the board's devices run for the given cycles of the CPU's */
static void devices(AgbBoard* board, uint32_t cycles) {
    agb_ppu_step(board, cycles);
    agb_timer_step(board, cycles);
    agb_save_step(board, cycles);
    agb_sio_step(board, cycles);
}

/* the wait states for the WAITCNT the board has: turning the prefetch unit off makes its next fetch a first one */
static void waits_now(AgbCpu* cpu, const AgbBoard* board) {
    uint32_t waitcnt = board->io[0x204 >> 1];
    if (cpu->waits.waitcnt == waitcnt) return;
    if ((cpu->waits.waitcnt & 0x4000) && !(waitcnt & 0x4000)) cpu->replay.bus.was_disabled = 1;
    agb_waits(&cpu->waits, waitcnt);
}

/* the running block's replay moved on to the instruction at unit, up to its data access `access` (timing.h), and the
   cycles from the block's start to there */
static uint32_t replay(AgbCpu* cpu, uint32_t unit, int32_t access) {
    const AgbBoard* board = cpu->board;
    waits_now(cpu, board);
    AgbBlockRun run = {.code = cpu->arm.mem + cpu->block_pc,
                       .pc = cpu->block_pc,
                       .thumb = cpu->block_thumb,
                       .trace = cpu->trace,
                       .skipped = cpu->skipped,
                       .next = cpu->arm.r[15],
                       .next_thumb = (cpu->arm.cpsr & SNX_ARM_T) != 0};
    return agb_replay_to(&cpu->replay, &cpu->waits, board->io[0x204 >> 1] >> 14 & 1, &run, unit, access);
}

/* The board catches up with the running block at an access to an I/O register: its devices run for the time the
   block's instructions took up to that access, so a timer or the display read there, or a timer started there, is as
   the console has it then */
static void catch_up(AgbCpu* cpu, uint32_t address) {
    uint32_t unit = (cpu->arm.r[15] - cpu->block_pc) >> (cpu->block_thumb ? 1 : 2);
    if (unit >= AGB_BLOCK_MAX) return;
    /* a multiple transfer's word: its place from the lowest, which the trace holds */
    uint32_t word = ((address & ~3u) - (cpu->trace[unit] & ~3u)) >> 2;
    uint32_t cycles = replay(cpu, unit, word < 16 ? (int32_t)word : 0);
    if (cycles <= cpu->advanced) return;
    devices(cpu->board, cycles - cpu->advanced);
    cpu->advanced = cycles;
}

/* a write that may raise an interrupt soon or let one through: to a timer's control, DISPSTAT, IE, IME or SIOCNT */
static int interrupt_write(uint32_t address, unsigned bytes) {
    static const uint16_t registers[] = {0x004, 0x102, 0x106, 0x10A, 0x10E, 0x128, 0x200, 0x208};
    uint32_t low = address & 0xFFFF; /* the registers repeat every 64 KiB of the window */
    if ((address >> 24) != 0x04) return 0;
    for (unsigned i = 0; i < sizeof registers / sizeof registers[0]; ++i)
        if (low <= registers[i] + 1u && low + bytes > registers[i]) return 1;
    return 0;
}

/* The bus hook the recompiled blocks call for the regions with bus rules a plain access cannot follow:
   the I/O window, the palette, OAM and the save memory, and the stores to the cartridge's ROM, which the board drops.
   This keeps the 1 KiB mirrors, the byte write widths and the register side effects correct while generated code
   runs at memory speed elsewhere. The machine's CPU state is its first member. */
static uint32_t bus(SnxArmCPU* arm, uint32_t address, uint32_t size_write, uint32_t value) {
    AgbCpu* cpu = (AgbCpu*)arm;
    AgbBoard* board = cpu->board;
    unsigned bytes = size_write & 0xFF;
    if (cpu->in_block && (address >> 24) == 0x04) catch_up(cpu, address);
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
        /* the block ends after this instruction where the write started a DMA, which takes the bus 2 cycles after it
           at the CPU's first access from then on (timing.h), or may bring an interrupt before the block would end (a
           timer's control, DISPSTAT, IE, IME, SIOCNT): the machine looks at it before the next instruction */
        if (board->held_cycles != held) {
            agb_bus_dma(&cpu->replay.bus, board->held_cycles - held,
                        board->held_cartridge > held ? board->held_cartridge - held : 0);
            board->held_cycles = held;
            board->held_cartridge = 0;
        } else if (!interrupt_write(address, bytes))
            return 0;
        cpu->yield_end = ((arm->r[15] - cpu->block_pc) >> (cpu->block_thumb ? 1 : 2)) + 1;
        return 1;
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
    agb_waits(&cpu->waits, board->io[0x204 >> 1]);
    cpu->replay.done = AGB_REPLAY_FETCH;
}

/* the sound for cycles of the run, which the run counts */
static void sound(AgbSink* sink, uint32_t cycles) {
    AgbBoard* board = sink->board;
    if (sink->audio && sink->written < sink->capacity)
        sink->written += agb_apu_run(board, cycles, sink->audio + sink->written * 2, sink->capacity - sink->written);
    sink->elapsed += cycles;
}

/* the machine's time passes outside the CPU's instructions: a DMA's, a halt's */
static void advance(AgbCpu* cpu, AgbSink* sink, uint32_t cycles) {
    (void)cpu;
    devices(sink->board, cycles);
    sound(sink, cycles);
}

/* the running block's time up to its replay's end: what the board has not run of it yet, and the sound of it all */
static void block_time(AgbCpu* cpu, AgbSink* sink, uint32_t cycles, uint32_t* charged) {
    if (cycles > cpu->advanced) {
        devices(sink->board, cycles - cpu->advanced);
        cpu->advanced = cycles;
    }
    sound(sink, cycles - *charged);
    *charged = cycles;
}

/* ---- interrupts ---- */

/* the 14 interrupt lines, IE's and IF's bits 0 to 13: the DMAs' (8 to 11), the keypad's and the cartridge's too */
static int irq_pending(const AgbBoard* board) {
    return (board->irq_ime & 1) && (board->irq_ie & board->irq_if & 0x3FFF);
}

/* The BIOS's interrupt code as the console runs it (GBATEK, BIOS Interrupt handling), whose time an interrupt takes: at
   0x18 the vector's branch to 0x128; there the registers stacked, the I/O base, the return address and the jump through
   0x03FFFFFC to the game's handler; at 0x138, once the handler returns, the registers back and the return to the code
   it interrupted. Its words, little endian */
static const uint8_t bios_vector[] = {0x42, 0x00, 0x00, 0xEA}; /* b 0x128 */
static const uint8_t bios_entry[] = {
    0x0F, 0x50, 0x2D, 0xE9, 0x01, 0x03, 0xA0, 0xE3,  /* stmfd sp!, {r0-r3, r12, lr} */
    0x00, 0xE0, 0x8F, 0xE2, 0x04, 0xF0, 0x10, 0xE5}; /* add lr, pc, #0; ldr pc, [r0, #-4] */
static const uint8_t bios_exit[] = {0x0F, 0x50, 0xBD, 0xE8,
                                    0x04, 0xF0, 0x5E, 0xE2}; /* ldmfd sp!, {...}; subs pc, lr, #4 */

static int prefetch_on(const AgbBoard* board) { return board->io[0x204 >> 1] >> 14 & 1; }

/* the time BIOS code at pc takes, units of it with the accesses trace gives, up to its jump to next */
static uint32_t bios_time(AgbCpu* cpu, const uint8_t* code, uint32_t pc, uint32_t units, const uint32_t* trace,
                          uint32_t next, int next_thumb) {
    const uint32_t skipped[1] = {0};
    AgbBlockRun run = {.code = code,
                       .pc = pc,
                       .thumb = 0,
                       .trace = trace,
                       .skipped = skipped,
                       .next = next,
                       .next_thumb = (uint32_t)next_thumb};
    AgbReplay bios = {.bus = cpu->replay.bus, .done = AGB_REPLAY_FETCH};
    waits_now(cpu, cpu->board);
    uint32_t cycles = agb_replay_to(&bios, &cpu->waits, prefetch_on(cpu->board), &run, units, AGB_REPLAY_WHOLE);
    cpu->replay.bus = bios.bus;
    return cycles;
}

/* The interrupt entry the BIOS stub performs: the live registers land on the IRQ stack, the handler at
   0x03007FFC runs, and its return reaches the epilogue the machine services. Its cycles, the CPU's fetch it abandons
   and the BIOS's code; 0 while no handler is installed yet, so the caller keeps the machine moving. */
static uint32_t irq_enter(AgbBoard* board, AgbCpu* cpu) {
    uint32_t handler = agb_read32(board, 0x03007FFC);
    uint32_t saved_cpsr = cpu->arm.cpsr, next = cpu->arm.r[15], sp, cycles;
    unsigned width = saved_cpsr & SNX_ARM_T ? 2 : 4;
    if (!handler) return 0;
    {
        uint32_t irq_sp = bank_index(saved_cpsr) == 2 ? cpu->arm.r[13] : cpu->banked_r13[2];
        uint32_t entry[4] = {irq_sp - 24, 0, 0, 0x03FFFFFC}, vector[1] = {0};
        AgbBus* bus = &cpu->replay.bus;
        waits_now(cpu, board);
        cycles = agb_bus_fetch(bus, &cpu->waits, prefetch_on(board), next + 2 * width, width, bus->sequential);
        cycles += agb_bus_fetch(bus, &cpu->waits, prefetch_on(board), 0x18, 4, 0);
        cycles += agb_bus_fetch(bus, &cpu->waits, prefetch_on(board), 0x1C, 4, 1);
        bus->sequential = 1;
        cycles += bios_time(cpu, bios_vector, 0x18, 1, vector, 0x128, 0);
        cycles += bios_time(cpu, bios_entry, 0x128, 4, entry, handler & ~(uint32_t)1, handler & 1);
    }
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
    return cycles;
}

/* The other half of the BIOS stub: the saved registers come back and the interrupted code resumes. Its cycles */
static uint32_t irq_return(AgbBoard* board, AgbCpu* cpu) {
    uint32_t sp = cpu->arm.r[13], saved_cpsr = cpu->spsr[2], back, cycles;
    {
        uint32_t trace[2] = {sp, 0};
        cycles =
            bios_time(cpu, bios_exit, 0x138, 2, trace, agb_read32(board, sp + 20) - 4, (saved_cpsr & SNX_ARM_T) != 0);
    }
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
    return cycles;
}

/* ---- software interrupts and the privileged residue ---- */

/* the bus a BIOS call runs on, its stacks where the CPU's mode in cpsr has them now (bios_time.h) */
static AgbBiosClock bios_clock(AgbCpu* cpu, uint32_t cpsr) {
    int bank = bank_index(cpsr);
    waits_now(cpu, cpu->board);
    AgbBiosClock clock = {.bus = &cpu->replay.bus,
                          .waits = &cpu->waits,
                          .prefetch = prefetch_on(cpu->board),
                          .svc_sp = bank == 3 ? cpu->arm.r[13] : cpu->banked_r13[3],
                          .sys_sp = bank == 0 ? cpu->arm.r[13] : cpu->banked_r13[0]};
    return clock;
}

/* the caller a call that waits returns to, from the frame the BIOS's entry keeps on the supervisor stack (swi_enter):
   its address and whether it runs Thumb */
static uint32_t waiting_caller(AgbCpu* cpu, int* thumb) {
    uint32_t frame = bank_index(cpu->arm.cpsr) == 3 ? cpu->arm.r[13] : cpu->banked_r13[3];
    *thumb = (agb_read32(cpu->board, frame) & SNX_ARM_T) != 0;
    return agb_read32(cpu->board, frame + 12);
}

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
static void swi_return(AgbBoard* board, AgbCpu* cpu, int halted) {
    uint32_t sp = cpu->arm.r[13], caller, back;
    if (halted) { /* Halt's return once woken, then the BIOS's way out (bios_time.h) */
        int thumb;
        uint32_t to = waiting_caller(cpu, &thumb);
        AgbBiosClock clock = bios_clock(cpu, cpu->arm.cpsr);
        cpu->bios_held += agb_bios_halt_leave(&clock, to, thumb);
    }
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
    int thumb, done;
    uint32_t to = waiting_caller(cpu, &thumb);
    AgbBiosClock clock = bios_clock(cpu, cpu->arm.cpsr);
    cpu->arm.r[14] = AGB_BIOS_INTR_WAIT + 4;
    done = intr_check(board, cpu) != 0;
    cpu->bios_held += agb_bios_intr_check(&clock, to, thumb, done); /* its look at the flags, and halt or return */
    if (!done) {
        board->cpu_halted = 1;
        return;
    }
    cpu->arm.r[4] = agb_read32(board, cpu->arm.r[13]);
    cpu->arm.r[14] = agb_read32(board, cpu->arm.r[13] + 4);
    cpu->arm.r[13] += 8;
    cpu->arm.r[15] = cpu->arm.r[14] & ~(uint32_t)3;
}

/* The opcode the CPU fetched last while the instruction at pc runs, which a read with nothing to answer gets (llvm.c's
   open_bus, mGBA's GBALoadBad): ARM [$+8]; Thumb [$+4] in both halves, but [$+4] above [$+2] from the BIOS and OAM, and
   from IWRAM [$+4] above [$+2] or below it by $+4's place in its word */
static uint32_t open_bus_at(AgbBoard* board, uint32_t pc, int thumb) {
    uint32_t region = (pc + 4) >> 24, h2, h4;
    if (!thumb) return agb_read32(board, pc + 8);
    h2 = agb_read16(board, pc + 2), h4 = agb_read16(board, pc + 4);
    if (region == 0x00 || region == 0x07 || (region == 0x03 && ((pc + 4) & 2))) return h4 << 16 | h2;
    return h4 | (region == 0x03 ? h2 : h4) << 16;
}

static void handle_swi(AgbCpu* cpu, AgbSink* sink, uint32_t syndrome, int thumb) {
    AgbBoard* board = sink->board;
    uint32_t number = thumb ? syndrome & 0xFF : (syndrome >> 16) & 0xFF;
    /* the BIOS's own time (bios_time.h): its code for the call from the vector on, with the caller's stacks as the
       call finds them; back is the address after the SWI, where the call returns */
    uint32_t back = cpu->arm.r[15], source = cpu->arm.r[0], destination = cpu->arm.r[1], control = cpu->arm.r[2];
    AgbBiosClock clock = bios_clock(cpu, cpu->arm.cpsr);
    /* what the BIOS's own reads of nothing give: the code after the call, which mGBA's HLE BIOS runs inside */
    board->open_bus = open_bus_at(board, cpu->arm.r[15] - (thumb ? 2u : 4u), thumb);
    board->bios_prefetch = AGB_BIOS_PREFETCH_SWI;
    if (number == AGB_SWI_SOFT_RESET) {
        soft_reset(board, cpu);
        return;
    }
    if (number == AGB_SWI_HALT || number == AGB_SWI_STOP) {
        /* halted inside the BIOS, r2 zero and the I/O base in r12 as its routine has them: the interrupt that wakes
           the machine is taken there, then the call returns */
        cpu->bios_held += agb_bios_halt(&clock, back, number);
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
        cpu->bios_held += agb_bios_intr_wait(&clock, back, number, number == AGB_SWI_VBLANK_INTR_WAIT, discard);
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
    /* the routine runs at once; its time is its code's (CpuSet, CpuFastSet), a stall in mGBA's loop for the math and
       LZ77, or the BIOS's entry and return with what the routine counted (the memory it streamed through) */
    board->counting = 1;
    board->counted_cycles = 0;
    int ran = agb_bios_call(board, number, cpu->arm.r);
    board->counting = 0;
    if ((number == AGB_SWI_CPU_SET || number == AGB_SWI_CPU_FAST_SET) && source >= 0x02000000u)
        cpu->bios_held += agb_bios_cpu_set(&clock, back, thumb, number, source, destination, control);
    else if ((number >= AGB_SWI_DIV && number <= AGB_SWI_ARCTAN2) || number == AGB_SWI_LZ77_UNCOMP_WRAM ||
             number == AGB_SWI_LZ77_UNCOMP_VRAM)
        cpu->bios_held += agb_bios_stalled(&clock, back, thumb, number, board->counted_cycles);
    else
        cpu->bios_held += agb_bios_direct(&clock, back, thumb, board->counted_cycles);
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
    cpu->trace[cpu->note] = address;
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
            cpu->trace[cpu->note] = base;
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
        cpu->trace[cpu->note] = address;
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
            cpu->trace[cpu->note] = address;
            if (load)
                cpu->arm.r[15] = agb_read32(board, address) & ~(uint32_t)1;
            else {
                agb_write32(board, address, cpu->arm.r[15] + 12);
                cpu->arm.r[15] += 4;
            }
            cpu->arm.r[n] = up ? base + 0x40 : base - 0x40;
            return 0;
        }
        /* with the PC in a load's list the S bit is an exception return, and the registers are the mode's own */
        int user = sbit && !(load && (list >> 15 & 1));
        uint32_t pc = 0;
        for (unsigned i = 0; i < 16; ++i) count += list >> i & 1;
        uint32_t adjusted = up ? base + count * 4 : base - count * 4;
        uint32_t start = up ? base + (pre ? 4 : 0) : adjusted + (pre ? 0 : 4);
        cpu->trace[cpu->note] = start;
        for (unsigned i = 0; i < 16; ++i) {
            if (!(list >> i & 1)) continue;
            uint32_t address = start + index++ * 4;
            if (load) {
                uint32_t value = agb_read32(board, address);
                if (user) {
                    /* the user bank, whichever mode the machine is in */
                    if (i == 13 || i == 14) {
                        if (bank_index(cpu->arm.cpsr)) (i == 13 ? cpu->banked_r13 : cpu->banked_r14)[0] = value;
                        else
                            cpu->arm.r[i] = value;
                    } else if (i >= 8 && i < 13 && bank_index(cpu->arm.cpsr) == 1)
                        cpu->usr_r8[i - 8] = value;
                    else
                        cpu->arm.r[i] = value;
                } else if (i == 15)
                    pc = value;
                else
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
        /* the writeback is lost when a load brings the base in the list: the loaded value stands. It goes to the
           base of the mode the instruction ran in: an exception return changes the mode after it, as mGBA's LDM does */
        if (wb && !(load && (list >> n & 1))) cpu->arm.r[n] = adjusted;
        if (load && (list >> 15 & 1)) {
            if (sbit) exception_return(cpu);
            cpu->arm.r[15] = pc & ~(uint32_t)1;
        } else
            cpu->arm.r[15] += 4;
        return 0;
    }
    if ((word & 0x0C000000) == 0 && (word >> 28) != 15) return data_quirk(cpu, word);
    return -1;
}

/* whether the CPU may take an interrupt before the block would end: IME on, IRQs on in CPSR, and the line up or rising
   within a bound on the block's time, twice its instructions' sequential fetches and 16 cycles more */
static int interrupt_soon(AgbCpu* cpu, const AgbBoard* board, const AgbBlock* block) {
    if (block->instructions < 2 || !(board->irq_ime & 1) || (cpu->arm.cpsr & 0x80)) return 0;
    uint64_t line = irq_pending(board) ? board->irq_at : agb_board_next_irq(board);
    if (line == UINT64_MAX) return 0;
    waits_now(cpu, board);
    uint32_t page = block->pc >> 24 & 0xF;
    return line < board->now + 16u + 2u * block->instructions * cpu->waits.wait32[1][page];
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
        /* the BIOS's code for its call takes its time first, as the CPU runs it */
        if (cpu->bios_held) {
            uint32_t room = cycles - sink.elapsed, spent = cpu->bios_held < room ? cpu->bios_held : room;
            cpu->bios_held -= spent;
            advance(cpu, &sink, spent);
            continue;
        }
        if (board->held_cycles && board->cpu_halted) board->held_cycles = 0;
        if (board->held_cycles) {
            uint32_t room = cycles - sink.elapsed, held = board->held_cycles < room ? board->held_cycles : room;
            board->held_cycles -= held;
            agb_bus_waited(&cpu->replay.bus, board->io[0x204 >> 1] >> 14 & 1, held, board->held_cartridge);
            board->held_cartridge = 0;
            advance(cpu, &sink, held);
            continue;
        }
        /* IntrWait looks at its flags once awake, after the interrupt that woke it, which the CPU takes first */
        if (cpu->arm.r[15] == AGB_BIOS_INTR_WAIT && !board->cpu_halted &&
            !(irq_pending(board) && !(cpu->arm.cpsr & 0x80)))
            intr_wait(board, cpu);
        if (board->cpu_halted && (board->irq_if & board->irq_ie)) board->cpu_halted = 0;
        if (board->cpu_halted) {
            /* up to the cycle an interrupt IE lets through wakes it, 2 after its flag (a cycle before the line) */
            uint64_t wake = agb_board_next_irq(board);
            uint32_t room = cycles - sink.elapsed;
            advance(cpu, &sink,
                    wake == UINT64_MAX || wake - 1 <= board->now ? (room < 64 ? room : 64)
                    : wake - 1 - board->now < (uint64_t)room     ? (uint32_t)(wake - 1 - board->now)
                                                                 : room);
            continue;
        }
        if (irq_pending(board) && !(cpu->arm.cpsr & 0x80)) {
            if (board->now >= board->irq_at) {
                uint32_t entry = irq_enter(board, cpu);
                advance(cpu, &sink, entry ? entry : 64);
                continue;
            }
            /* a CPU waiting in the BIOS takes it there once the line is up */
            if (cpu->arm.r[15] == AGB_BIOS_HALT || cpu->arm.r[15] == AGB_BIOS_INTR_WAIT) {
                advance(cpu, &sink, (uint32_t)(board->irq_at - board->now));
                continue;
            }
        }
        if (cpu->arm.r[15] == AGB_IRQ_RETURN) {
            advance(cpu, &sink, irq_return(board, cpu));
            continue;
        }
        if (cpu->arm.r[15] == AGB_BIOS_HALT || cpu->arm.r[15] == AGB_BIOS_SWI_RETURN) { /* Halt awake: bx lr */
            swi_return(board, cpu, cpu->arm.r[15] == AGB_BIOS_HALT);
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
        /* where the interrupt line may rise before a block would end, the CPU runs its instructions one at a time, to
           take the interrupt after the one it rises in, as the console does */
        int thumb = (cpu->arm.cpsr & SNX_ARM_T) != 0;
        const AgbBlock* block = agb_recomp_lookup(recomp, board, cpu->arm.r[15], thumb, error, error_size);
        if (block && interrupt_soon(cpu, board, block))
            block = agb_recomp_step(recomp, board, cpu->arm.r[15], thumb, error, error_size);
        if (!block) return -1;
        /* the block runs, then its time is replayed from what it left (timing.h): up to the instruction a DMA's start
           ended it after, or the one it stopped at for the machine to interpret, else all of it */
        unsigned shift = block->thumb ? 1 : 2;
        uint32_t charged = 0, end;
        cpu->block_pc = block->pc;
        cpu->block_thumb = block->thumb;
        cpu->advanced = cpu->yield_end = 0;
        memset(cpu->skipped, 0, sizeof cpu->skipped);
        cpu->replay.cycles = cpu->replay.unit = 0;
        cpu->replay.done = AGB_REPLAY_FETCH;
        cpu->arm.exit = SNX_ARM_OK;
        cpu->in_block = 1;
        block->fn(&cpu->arm);
        cpu->in_block = 0;
        end = cpu->arm.exit == SNX_ARM_UNDEFINED ? (cpu->arm.r[15] - block->pc) >> shift
              : cpu->yield_end                   ? cpu->yield_end
                                                 : block->instructions;
        block_time(cpu, &sink, replay(cpu, end, AGB_REPLAY_WHOLE), &charged);
        if (cpu->arm.exit == SNX_ARM_SVC)
            handle_swi(cpu, &sink, cpu->arm.syndrome, block->thumb);
        else if (cpu->arm.exit == SNX_ARM_UNDEFINED) {
            cpu->trace[end] = 0; /* what its transfer reaches, if it has one, interpret notes */
            cpu->note = end;
            if (interpret(board, cpu, cpu->arm.syndrome, block->thumb)) {
                snprintf(error, error_size, "unimplemented instruction 0x%08X at 0x%08X", cpu->arm.syndrome,
                         cpu->arm.r[15]);
                return -1;
            }
            block_time(cpu, &sink, replay(cpu, end + 1, AGB_REPLAY_WHOLE), &charged);
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
