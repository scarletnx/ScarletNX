/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's side of RtAPI (rtapi.c), and the hints file (hints.c): code addresses a run met that the recompiler
   had not found, so the next recompile compiles them too */
#ifndef SNX_RTAPI_H
#define SNX_RTAPI_H
#include "common/cpu/aarch64/cpu.h"

extern const RtAPI g_rtapi; /* every game thread's CPU.rt */
void hints_init(void);      /* <exedir><name>.hints, once the game is named */
void load_hints(void);      /* the file's addresses, so a save keeps them */
void save_hints(void);      /* the file again, when this run added any. Any thread */
int hints_add(u64 pc);      /* records one nonzero address, returns 1 only when new */
void snx_fpcr_apply(CPU* c); /* the thread's FPCR as it starts and on each write */
/* the hints so far, loaded and found, in the order they were found: malloc'd (the caller frees), NULL when none */
u64* hints_copy(size_t* count);

#endif
