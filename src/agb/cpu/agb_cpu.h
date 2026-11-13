/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The machine loop: recompiled blocks run on the flat map, software interrupts go to the HLE BIOS, and the
   privileged instructions the ARM recompiler refuses are interpreted here. */
#ifndef SNX_AGB_CPU_H
#define SNX_AGB_CPU_H
#include "common/cpu/arm/arm.h"
#include "agb/cpu/recomp.h"
#include "agb/cpu/timing.h"
#include <stddef.h>
#include <stdint.h>

typedef struct AgbBoard AgbBoard;
typedef struct {
    SnxArmCPU arm;
    AgbBoard* board; /* the machine the bus hook serves */
    /* the running block's trace (llvm.h's SnxArmCodegenOptions): per instruction unit its access's address or its
       multiplier, and a bit for each conditional one that did not run, which the machine loop clears before it */
    uint32_t trace[AGB_BLOCK_MAX];
    uint32_t skipped[AGB_BLOCK_MAX / 32];
    /* the time the CPU takes on the bus (timing.h): the wait states for WAITCNT, and the bus with the running block's
       replay, which the bus hook moves on to an I/O register's access and the machine loop to the block's end */
    AgbWaits waits;
    AgbReplay replay;
    uint32_t block_pc, block_thumb, in_block; /* the running block */
    uint32_t advanced;  /* the cycles of the running block the board has run already, up to an I/O access */
    uint32_t yield_end; /* the unit after the instruction whose write ended the running block early, else 0 */
    uint32_t note;      /* the trace slot of the instruction the machine interprets, which its transfer fills */
    /* the cycles of the BIOS's code for the call it is in (agb/bios/bios_time.h), which pass before the CPU's next
       instruction, over runs when they are long */
    uint32_t bios_held;
    /* the banked registers of the ARM7TDMI's modes: usr and sys share one bank */
    uint32_t banked_r13[6], banked_r14[6], spsr[6];
    uint32_t fiq_r8[5], usr_r8[5]; /* the ARM7TDMI banks r8 to r12 in FIQ mode */
    /* the cycles the last run went past its budget, which the next run's takes back: a run of a frame's cycles then
       ends where the machine's frame does, not later at each frame */
    uint32_t over;
    /* the last software interrupt whose number the BIOS does not have, the address of that instruction, and how many
       such calls ran: each returns at once, as mGBA's HLE BIOS answers them, for the runner to say */
    uint32_t unknown_swi, unknown_swi_at, unknown_swi_count;
} AgbCpu;

void agb_cpu_init(AgbCpu* cpu, AgbBoard* board, uint8_t* map);
/* Runs the board for the given number of CPU cycles, less those the last run went past its own. audio collects interleaved stereo samples at
   AGB_APU_RATE, up to capacity. Returns 0, or -1 with a message when the machine stops. */
int agb_cpu_run(AgbBoard* board, AgbCpu* cpu, AgbRecomp* recomp, uint32_t cycles, int16_t* audio,
                uint32_t audio_capacity, uint32_t* audio_written, char* error, size_t error_size);
#endif
