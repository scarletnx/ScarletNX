/* SPDX-License-Identifier: GPL-3.0-or-later AND BSD-2-Clause
   Copyright 2026 the ScarletNX contributors
   Copyright 2026 Mireille Meyer, Gloria Goertz */
/* The ARM7TDMI's time on the AGB board's bus, instruction by instruction: each one's opcode fetch, data accesses and
   internal cycles in the order the CPU makes them, with the cartridge's prefetch unit working between them
   (timing.c). A recompiled block runs at host speed and leaves what its time depends on in a trace (its loads' and
   stores' addresses, a multiplier, the conditional instructions that did not run); this replays it. */
#ifndef SNX_AGB_TIMING_H
#define SNX_AGB_TIMING_H
#include <stdint.h>

typedef struct AgbBoard AgbBoard;

/* the cycles of one access by region (address bits 24 to 27), [0] nonsequential and [1] sequential, for the WAITCNT
   they were made for (0x10000 before any) */
typedef struct {
    uint8_t wait16[2][16], wait32[2][16];
    uint32_t waitcnt;
} AgbWaits;

/* The cartridge's prefetch unit (GBATEK "GamePak Prefetch"): while the CPU works elsewhere it reads on from where the
   CPU's last opcode fetch left the cartridge, up to 8 halfwords (4 words in ARM state). An opcode it holds costs the
   CPU one cycle; one it is reading costs what is left of that read */
typedef struct {
    uint32_t head, last;     /* the first opcode it holds, and the one it reads now */
    int32_t countdown, duty; /* the cycles left of that read, and each read's */
    uint8_t active, count, capacity, width;
} AgbPrefetch;

/* the bus between two accesses */
typedef struct {
    AgbPrefetch prefetch;
    uint8_t sequential;   /* the CPU's next opcode fetch follows its last one in sequence */
    uint8_t was_disabled; /* WAITCNT turned the prefetch unit off: the next cartridge fetch is nonsequential */
} AgbBus;

/* a block's replay: where it is, the bus as it leaves it there, and the cycles from the block's start to there */
typedef struct {
    AgbBus bus;
    uint32_t cycles;
    uint32_t unit; /* the instruction it is in: its offset from the block's start, halfwords in Thumb, words in ARM */
    int32_t done;  /* that instruction's data accesses done, AGB_REPLAY_FETCH before its fetch */
} AgbReplay;

enum { AGB_REPLAY_FETCH = -1, AGB_REPLAY_WHOLE = 0x7FFFFFFF };

/* What a block's run left for its replay: its code (the bytes at its first instruction, its address, Thumb or ARM),
   per instruction unit the trace value (a load's or store's address, the first of a multiple one, a multiplier) and a
   bit for each conditional one that did not run, and the PC and state it left at its end (a branch's target) */
typedef struct {
    const uint8_t* code;
    uint32_t pc, thumb;
    const uint32_t* trace;
    const uint32_t* skipped;
    uint32_t next, next_thumb;
} AgbBlockRun;

/* the wait states WAITCNT gives (GBATEK "GBA System Control"): the cartridge's three windows and the save memory */
void agb_waits(AgbWaits* waits, uint32_t waitcnt);
/* The replay moved on from where it was: past every instruction before the one at `unit`, then that one's fetch and
   its data accesses up to `access` (AGB_REPLAY_WHOLE: none of it), and the cycles from the block's start to there. A
   position it has passed already changes nothing */
uint32_t agb_replay_to(AgbReplay* replay, const AgbWaits* waits, int prefetch, const AgbBlockRun* run, uint32_t unit,
                       int32_t access);
/* The cycles of one access the CPU makes outside a recompiled block (the BIOS's, an interrupt's entry): a data access
   of width bytes at address, write or read, sequential to the last or not, with the prefetch unit moving meanwhile */
uint32_t agb_bus_data(AgbBus* bus, const AgbWaits* waits, int prefetch, uint32_t address, unsigned width, int write,
                      int sequential, uint32_t pc);
/* an opcode fetch at address, width bytes, sequential to the last or not; idle: internal cycles */
uint32_t agb_bus_fetch(AgbBus* bus, const AgbWaits* waits, int prefetch, uint32_t address, unsigned width,
                       int sequential);
uint32_t agb_bus_idle(AgbBus* bus, int prefetch, uint32_t cycles);
#endif
