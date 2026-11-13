/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ARM9 as the machine runs it: compiled blocks on the flat window, software interrupts to the HLE BIOS, and the
   instructions the block compiler leaves to the system (mode switches, exception returns, CP15) run here against the
   banked registers. */
#ifndef SNX_NDS_CPU_H
#define SNX_NDS_CPU_H
#include "common/cpu/arm/arm.h"
#include "nds/board/nds.h"

/* an exit of the machine's own: the block ended for the loop to look at the machine (an interrupt that may now be
   taken, a halt, a jump the runtime made), with r15 where it goes on */
#define NDS_EXIT_YIELD 0x100u

struct NdsRecomp;
typedef struct {
    SnxArmCPU arm; /* first: the bus hook and the backend get the machine from it */
    NdsBoard* board;
    struct NdsRecomp* recomp;
    SnxArmRt rt;
    /* the banked registers of the modes: usr and sys share a bank, FIQ has its own r8 to r12 */
    uint32_t banked_r13[6], banked_r14[6], spsr[6];
    uint32_t fiq_r8[5], usr_r8[5];
    uint64_t blocks, instructions; /* run so far */
    uint32_t trail[32];            /* the last blocks' addresses (| 1 for Thumb), for the report of a run that stops */
    uint32_t unknown_swi, unknown_swi_at, unknown_swi_count;
} NdsCpu;

void nds_cpu_init(NdsCpu* cpu, NdsBoard* board, struct NdsRecomp* recomp);
/* Runs the machine until the frame it is in is whole (line 192 begins). 0, or -1 with the reason in error when the
   machine stops */
int nds_cpu_run_frame(NdsCpu* cpu, char* error, size_t error_size);
#endif
