/* SPDX-License-Identifier: MPL-2.0
   Copyright 2026 the mGBA contributors
   Copyright 2026 the ScarletNX contributors */
/* The time the BIOS takes for a call, which the HLE BIOS's routines (bios.c) do not spend themselves: the code mGBA's
   HLE BIOS runs for it (its hle-bios.s), replayed on the CPU's bus (agb/cpu/timing.h). Each function returns its
   cycles and leaves the bus as that code leaves it. The stacks are where the caller's registers say (IWRAM, mostly) */
#ifndef SNX_AGB_BIOS_TIME_H
#define SNX_AGB_BIOS_TIME_H
#include "agb/cpu/timing.h"
#include <stdint.h>

/* the bus a BIOS call runs on: its state, the wait states, WAITCNT's prefetch bit, and the stack pointers the
   dispatch pushes on (the supervisor's, then the system mode's) */
typedef struct {
    AgbBus* bus;
    const AgbWaits* waits;
    int prefetch;
    uint32_t svc_sp, sys_sp;
} AgbBiosClock;

/* A call through the BIOS's vector: the branch at 0x08 and the dispatch up to the routine (0 for none: the dispatch
   goes on to its way out), the call's number read from the caller's code at back - 2 (back: the address after the
   SWI) */
uint32_t agb_bios_enter(const AgbBiosClock* clock, uint32_t back, uint32_t number, uint32_t routine);
/* the dispatch's way out from the routine's return: the registers back and the return to back, Thumb or ARM */
uint32_t agb_bios_leave(const AgbBiosClock* clock, uint32_t back, int thumb);
/* A call the BIOS answers without its code (mGBA's GBASwi16 when it needs no stall): its entry and return, 45 cycles
   less the vector's refill the SWI's own time holds, its number's read, spent cycles of its own, and the refill at
   back */
uint32_t agb_bios_direct(const AgbBiosClock* clock, uint32_t back, int thumb, uint32_t spent);
/* A call with a stall of spent cycles (Div, Sqrt, ArcTan, LZ77): spent less 18 in mGBA's stall loop between the
   dispatch's entry and its way out, as mGBA's HLE BIOS runs it from 18 cycles on; under 18, the direct way */
uint32_t agb_bios_stalled(const AgbBiosClock* clock, uint32_t back, int thumb, uint32_t number, uint32_t spent);
/* Halt: its entry up to the store to HALTCNT; then, woken, its return and the dispatch's way out */
uint32_t agb_bios_halt(const AgbBiosClock* clock, uint32_t back, uint32_t number);
uint32_t agb_bios_halt_leave(const AgbBiosClock* clock, uint32_t back, int thumb);
/* IntrWait (VBlankIntrWait: vblank set): its entry up to its first halt (discard) or its first look at the flags;
   then each look, after which it halts again or (done) returns through the dispatch */
uint32_t agb_bios_intr_wait(const AgbBiosClock* clock, uint32_t back, uint32_t number, int vblank, int discard);
uint32_t agb_bios_intr_check(const AgbBiosClock* clock, uint32_t back, int thumb, int done);
/* CpuSet and CpuFastSet of count units (words, or halfwords for CpuSet without bit 26) from source to destination,
   filled from the source's first one when fill: the routine's loop, and its return through the dispatch */
uint32_t agb_bios_cpu_set(const AgbBiosClock* clock, uint32_t back, int thumb, uint32_t number, uint32_t source,
                          uint32_t destination, uint32_t control);
#endif
