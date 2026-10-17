/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#ifndef SNX_RUNTIME_INTERFACE_H
#define SNX_RUNTIME_INTERFACE_H
#include <stdint.h>
struct CPU;
/* runtime callbacks used by generated code */
typedef struct RtAPI {
    void (*svc)(struct CPU* c, uint32_t imm);
    void (*undef)(struct CPU* c, uint64_t pc, uint32_t w);
    void (*note_hint)(uint64_t pc);
    uint64_t (*ticks)(void); /* runtime-supplied counter */
    void (*brk)(struct CPU* c, uint64_t pc, uint32_t imm);
    void (*fpcr_changed)(struct CPU* c);
    void (*sev)(void);
    void (*wfe)(struct CPU* c);
    /* CPU.poll was set: a loop that makes no SVC lets the runtime take the thread off its core here */
    void (*poll)(struct CPU* c);
    /* snx_lookup's answer for a pc without code in CPU.disp, called as the guest function at pc (a GuestFn): the
       runtime runs it in the interpreter or compiles it first. The module no longer carries its own fallback, so the
       runtime decides what a miss does */
    void (*miss)(struct CPU* c, uint64_t pc);
} RtAPI;

#endif
