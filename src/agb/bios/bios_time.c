/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
#include "agb/bios/bios_time.h"

/* an ARM word as code bytes, little endian */
#define W(x) (uint8_t)(x), (uint8_t)((x) >> 8), (uint8_t)((x) >> 16), (uint8_t)((x) >> 24)

/* where the code below sits in the BIOS's region: every fetch there takes a cycle, so only the region counts */
enum {
    VECTOR = 0x08,
    DISPATCH = 0x40,
    LEAVE = DISPATCH + 25 * 4, /* the dispatch's way out, where the routines return */
    STALL = 0x300,
    HALT = 0x320,
    VBLANK_INTR_WAIT = 0x330,
    INTR_WAIT = VBLANK_INTR_WAIT + 8,
    CPU_SET = 0x400,
    CPU_FAST_SET = 0x500
};

/* the vector's branch to the dispatch */
static const uint8_t vector[] = {W(0xEA00000C)};
/* mGBA's dispatch (swiBase): the caller's registers stacked, its number read from its code and looked up, system mode,
   and the routine called; mGBA's internal stall call stands as an instruction of a cycle */
static const uint8_t dispatch[] = {
    W(0xE35D0000), W(0x03A0D301), W(0x024DD020), W(0xE92D5800), W(0xE55EC002), W(0xE3A0BF72), W(0xE79BC10C),
    W(0xE3A0BFBF), W(0xE15B000C), W(0xE14FB000), W(0xE92D0800), W(0xE20BB080), W(0xE38BB01F), W(0xE121F00B),
    W(0x01A0B00B), W(0xE92D4004), W(0xE35C0000), W(0x00000000), W(0x00000000), W(0x00000000), W(0x00000000),
    W(0x00000000), W(0x00000000), W(0x11A0E00F), W(0x112FFF1C),
};
/* its way out: three no-ops, the registers back, the caller's mode and its return */
static const uint8_t leave[] = {
    W(0x00000000), W(0x00000000), W(0x00000000), W(0xE8BD4004), W(0xE329F093),
    W(0xE8BD0800), W(0xE169F00B), W(0xE8BD5800), W(0xE1B0F00E),
};
/* Halt: r11 and r12, the store to HALTCNT, and the return once woken */
static const uint8_t halt[] = {W(0xE3A0B000), W(0xE3A0C301), W(0xE5CCB301), W(0xE12FFF1E)};
/* VBlankIntrWait's two moves, then IntrWait: its frame, the flags it drops first (discard), the halt, then each look
   at the flags with IME off, and its return */
static const uint8_t intr_wait[] = {
    W(0xE3A00001), W(0xE3A01001), W(0xE92D400C), W(0xE3A0C301), W(0xE3500000), W(0xE3A00000), W(0xE3A02001),
    W(0x0A000000), W(0xE15C30B8), W(0xE1C33001), W(0xE14C30B8), W(0xE5CC0301), W(0xE5CC0208), W(0xE15C30B8),
    W(0xE0133001), W(0x10233001), W(0x114C30B8), W(0xE5CC2208), W(0x0A000000), W(0xE8BD800C),
};
static const uint8_t cpu_set[] = {
    W(0xE92D4030), W(0xE1A04602), W(0xE1A0C000), W(0xE1A05001), W(0xE3120401), W(0x0A000000), W(0xE3120301),
    W(0x0A000000), W(0xE0854524), W(0xE8B00008), W(0xE1510004), W(0xB8A10008), W(0xBA000000), W(0xEA000000),
    W(0xE3CCC001), W(0xE3C55001), W(0xE08545A4), W(0xE1DC30B0), W(0xE1550004), W(0xB0C530B2), W(0xBA000000),
    W(0xEA000000), W(0xE3120301), W(0x0A000000), W(0xE0854524), W(0xE1510004), W(0xB8B00008), W(0xB8A10008),
    W(0xBA000000), W(0xEA000000), W(0xE08545A4), W(0xE1550004), W(0xB0DC30B2), W(0xB0C530B2), W(0xBA000000),
    W(0xE3A03E17), W(0xE8BD8030),
};
static const uint8_t cpu_fast_set[] = {
    W(0xE92D47F0), W(0xE3120401), W(0xE1A03602), W(0xE0812523), W(0x0A000000), W(0xE5903000),
    W(0xE1A04003), W(0xE1A05003), W(0xE1A06003), W(0xE1A07003), W(0xE1A08003), W(0xE1A09003),
    W(0xE1A0A003), W(0xE1510002), W(0xB8A107F8), W(0xBA000000), W(0xEA000000), W(0xE1510002),
    W(0xB8B007F8), W(0xB8A107F8), W(0xBA000000), W(0xE8BD87F0),
};

/* units ARM instructions of code from unit first on, at base + first * 4, their accesses' addresses in trace, the
   conditional ones that did not run in skipped (a bit a unit), the last one's jump (if it jumps) to next */
static uint32_t run(const AgbBiosClock* clock, const uint8_t* code, uint32_t base, uint32_t first, uint32_t units,
                    const uint32_t* trace, uint32_t skipped, uint32_t next, int next_thumb) {
    const uint32_t skip[1] = {skipped};
    AgbBlockRun block = {.code = code + first * 4,
                         .pc = base + first * 4,
                         .thumb = 0,
                         .trace = trace,
                         .skipped = skip,
                         .next = next,
                         .next_thumb = (uint32_t)next_thumb};
    return agb_code_time(clock->bus, clock->waits, clock->prefetch, &block, units);
}

uint32_t agb_bios_enter(const AgbBiosClock* clock, uint32_t back, uint32_t number, uint32_t routine) {
    uint32_t trace[25] = {0};
    trace[3] = clock->svc_sp - 12;          /* r11, r12, lr */
    trace[4] = back - 2;                    /* the call's number, in the caller's code */
    trace[6] = 0x1C8 + (number & 0xFF) * 4; /* the routine's address, from the BIOS's table */
    trace[10] = clock->svc_sp - 16;         /* the SPSR */
    trace[15] = clock->sys_sp - 8;          /* r2, lr */
    uint32_t cycles = run(clock, vector, VECTOR, 0, 1, trace, 0, DISPATCH, 0);
    return cycles + run(clock, dispatch, DISPATCH, 0, 25, trace, routine ? 0 : 1u << 24, routine, 0);
}

uint32_t agb_bios_leave(const AgbBiosClock* clock, uint32_t back, int thumb) {
    uint32_t trace[9] = {0};
    trace[3] = clock->sys_sp - 8;
    trace[5] = clock->svc_sp - 16;
    trace[7] = clock->svc_sp - 12;
    return run(clock, leave, LEAVE, 0, 9, trace, 0, back, thumb);
}

uint32_t agb_bios_direct(const AgbBiosClock* clock, uint32_t back, int thumb, uint32_t spent) {
    unsigned width = thumb ? 2 : 4;
    uint32_t cycles = agb_bus_idle(clock->bus, clock->prefetch, 43, VECTOR);
    cycles += agb_bus_data(clock->bus, clock->waits, clock->prefetch, back - 2, 1, 0, 0, VECTOR);
    cycles += agb_bus_idle(clock->bus, clock->prefetch, spent, VECTOR);
    cycles += agb_bus_fetch(clock->bus, clock->waits, clock->prefetch, back, width, 0);
    cycles += agb_bus_fetch(clock->bus, clock->waits, clock->prefetch, back + width, width, 1);
    clock->bus->sequential = 1;
    return cycles;
}

uint32_t agb_bios_stalled(const AgbBiosClock* clock, uint32_t back, int thumb, uint32_t number, uint32_t spent) {
    if (spent < 18) return agb_bios_direct(clock, back, thumb, spent);
    /* mGBA: the stall less 18, its odd cycles at once, the rest 4 a turn of the loop (subs, bhi back, a refill of
       2), the last turn's bhi not taken, then bx lr's refill */
    uint32_t loop = (spent - 18) & ~3u, cycles = agb_bus_idle(clock->bus, clock->prefetch, (spent - 18) & 3, VECTOR);
    cycles += agb_bios_enter(clock, back, number, STALL);
    cycles += agb_bus_idle(clock->bus, clock->prefetch, (loop > 4 ? 4 * (loop / 4 - 1) : 0) + 2 + 3, STALL);
    return cycles + agb_bios_leave(clock, back, thumb);
}

uint32_t agb_bios_halt(const AgbBiosClock* clock, uint32_t back, uint32_t number) {
    const uint32_t trace[3] = {0, 0, 0x04000301};
    return agb_bios_enter(clock, back, number, HALT) + run(clock, halt, HALT, 0, 3, trace, 0, 0, 0);
}

uint32_t agb_bios_halt_leave(const AgbBiosClock* clock, uint32_t back, int thumb) {
    const uint32_t trace[1] = {0};
    return run(clock, halt, HALT, 3, 1, trace, 0, LEAVE, 0) + agb_bios_leave(clock, back, thumb);
}

/* IntrWait's frame on the system stack, below the dispatch's two words */
static uint32_t frame(const AgbBiosClock* clock) { return clock->sys_sp - 8 - 12; }

uint32_t agb_bios_intr_wait(const AgbBiosClock* clock, uint32_t back, uint32_t number, int vblank, int discard) {
    uint32_t trace[12] = {0, 0, frame(clock), 0, 0, 0, 0, 0, 0x03FFFFF8, 0, 0x03FFFFF8, 0x04000301};
    uint32_t cycles = agb_bios_enter(clock, back, number, vblank ? VBLANK_INTR_WAIT : INTR_WAIT);
    uint32_t first = vblank ? 0 : 2;
    /* without discard its beq goes to the first look at the flags (agb_bios_intr_check); with it, the flags dropped,
       on to the halt */
    if (!discard)
        return cycles + run(clock, intr_wait, VBLANK_INTR_WAIT, first, 8 - first, trace + first, 0,
                            VBLANK_INTR_WAIT + 12 * 4, 0);
    return cycles + run(clock, intr_wait, VBLANK_INTR_WAIT, first, 12 - first, trace + first, 1u << (7 - first), 0, 0);
}

uint32_t agb_bios_intr_check(const AgbBiosClock* clock, uint32_t back, int thumb, int done) {
    /* IME off, the flags read, the ones asked for dropped (done), IME on, then the halt again or the return */
    const uint32_t trace[8] = {0x04000208, 0x03FFFFF8, 0, 0, 0x03FFFFF8, 0x04000208, 0, frame(clock)};
    const uint32_t halt_trace[1] = {0x04000301};
    if (!done)
        return run(clock, intr_wait, VBLANK_INTR_WAIT, 12, 7, trace, 1u << 4, VBLANK_INTR_WAIT + 11 * 4, 0) +
               run(clock, intr_wait, VBLANK_INTR_WAIT, 11, 1, halt_trace, 0, 0, 0);
    /* the console's IntrWait returns 5 cycles after this code would (its look at the flags a call of its own): mGBA's
       suite's timers.c, which with them finds 934 of its 936 counts the console's, 828 without */
    return agb_bus_idle(clock->bus, clock->prefetch, 5, VECTOR) +
           run(clock, intr_wait, VBLANK_INTR_WAIT, 12, 8, trace, 1u << 6, LEAVE, 0) +
           agb_bios_leave(clock, back, thumb);
}

uint32_t agb_bios_cpu_set(const AgbBiosClock* clock, uint32_t back, int thumb, uint32_t number, uint32_t source,
                          uint32_t destination, uint32_t control) {
    int fill = (control >> 24) & 1, fast = number == 0x0C, words = fast || ((control >> 26) & 1);
    uint32_t count = control & 0x1FFFFF, cycles, step;
    uint32_t pushed = clock->sys_sp - 8 - (fast ? 32 : 12); /* the routine's frame: r4-r10 and lr, or r4, r5, lr */
    if (fast) {
        /* r2 << 12 >> 10: the count's low 20 bits, in words; the loop moves 8 a turn while it is short of the end */
        uint32_t turns = ((count & 0xFFFFF) + 7) / 8, trace[22] = {0};
        trace[0] = pushed;
        trace[5] = source;
        cycles = agb_bios_enter(clock, back, number, CPU_FAST_SET);
        if (fill) {
            cycles += run(clock, cpu_fast_set, CPU_FAST_SET, 0, 13, trace, 1u << 4, 0, 0);
            for (uint32_t turn = 0; turn < turns; ++turn) {
                uint32_t loop[3] = {0, destination + turn * 32, 0};
                cycles += run(clock, cpu_fast_set, CPU_FAST_SET, 13, 3, loop, 0, CPU_FAST_SET + 13 * 4, 0);
            }
            const uint32_t end[4] = {0, 0, 0, 0};
            cycles += run(clock, cpu_fast_set, CPU_FAST_SET, 13, 4, end, 3u << 1, CPU_FAST_SET + 21 * 4, 0);
        } else {
            cycles += run(clock, cpu_fast_set, CPU_FAST_SET, 0, 5, trace, 0, CPU_FAST_SET + 17 * 4, 0);
            for (uint32_t turn = 0; turn < turns; ++turn) {
                uint32_t loop[4] = {0, source + turn * 32, destination + turn * 32, 0};
                cycles += run(clock, cpu_fast_set, CPU_FAST_SET, 17, 4, loop, 0, CPU_FAST_SET + 17 * 4, 0);
            }
            const uint32_t end[4] = {0, 0, 0, 0};
            cycles += run(clock, cpu_fast_set, CPU_FAST_SET, 17, 4, end, 7u << 1, 0, 0);
        }
        const uint32_t out[1] = {pushed};
        cycles += run(clock, cpu_fast_set, CPU_FAST_SET, 21, 1, out, 0, LEAVE, 0);
        return cycles + agb_bios_leave(clock, back, thumb);
    }
    /* CpuSet: a unit a turn, words (bit 26) or halfwords, copied or filled */
    step = words ? 4 : 2;
    {
        uint32_t trace[37] = {0};
        trace[0] = pushed;
        trace[9] = source;
        trace[17] = source & ~1u;
        cycles = agb_bios_enter(clock, back, number, CPU_SET);
        /* the frame and the tests, down to the loop's first compare: each path's units and the branches it takes */
        if (fill && words)
            cycles += run(clock, cpu_set, CPU_SET, 0, 10, trace, (1u << 5) | (1u << 7), 0, 0);
        else if (fill)
            cycles += run(clock, cpu_set, CPU_SET, 0, 8, trace, 1u << 5, CPU_SET + 14 * 4, 0) +
                      run(clock, cpu_set, CPU_SET, 14, 4, trace + 14, 0, 0, 0);
        else if (words)
            cycles += run(clock, cpu_set, CPU_SET, 0, 6, trace, 0, CPU_SET + 22 * 4, 0) +
                      run(clock, cpu_set, CPU_SET, 22, 3, trace + 22, 1u << 1, 0, 0);
        else
            cycles += run(clock, cpu_set, CPU_SET, 0, 6, trace, 0, CPU_SET + 22 * 4, 0) +
                      run(clock, cpu_set, CPU_SET, 22, 2, trace + 22, 0, CPU_SET + 30 * 4, 0) +
                      run(clock, cpu_set, CPU_SET, 30, 1, trace + 30, 0, 0, 0);
    }
    {
        /* each turn's compare, its transfers and the branch back; then the last compare, the transfers skipped, the
           branch not taken, and on to the return */
        uint32_t loop = fill ? (words ? 10 : 18) : (words ? 25 : 31), units = fill ? 3 : 4;
        for (uint32_t turn = 0; turn < (count & 0xFFFFF); ++turn) {
            uint32_t trace[4] = {0, fill ? destination + turn * step : source + turn * step,
                                 fill ? 0 : destination + turn * step, 0};
            cycles += run(clock, cpu_set, CPU_SET, loop, units, trace, 0, CPU_SET + loop * 4, 0);
        }
        const uint32_t end[5] = {0, 0, 0, 0, 0};
        uint32_t skipped = fill ? 3u << 1 : 7u << 1;
        /* the halfword copy's loop ends at the return; the others take their b to it */
        cycles +=
            run(clock, cpu_set, CPU_SET, loop, units + (fill || words ? 1 : 0), end, skipped, CPU_SET + 35 * 4, 0);
        const uint32_t out[2] = {0, pushed};
        cycles += run(clock, cpu_set, CPU_SET, 35, 2, out, 0, LEAVE, 0);
    }
    return cycles + agb_bios_leave(clock, back, thumb);
}
