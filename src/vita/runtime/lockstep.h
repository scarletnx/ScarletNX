/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_VITA_LOCKSTEP_H
#define SNX_VITA_LOCKSTEP_H
#include "vita/runtime/process.h"

/* The AArch32 backend checked against Unicorn while the game runs (lockstep.c, with SNX_VITA_LOCKSTEP): from the
   import call the setting vita_lockstep names (0: from the start; -1, the default: off), each instruction (a Thumb
   IT block with the instructions it covers) runs in Unicorn and then as a block of its own, from the same state, and
   the first whose registers or written memory differ is reported, which ends the run */
typedef enum { VITA_LOCKSTEP_OFF, VITA_LOCKSTEP_RAN, VITA_LOCKSTEP_NATIVE, VITA_LOCKSTEP_MISMATCH } VitaLockstep;
/* the thread's next instruction run so: NATIVE when the dispatcher runs it as usual (an SVC, or lockstep off) */
VitaLockstep vita_lockstep_step(VitaProcess* p, VitaThread* t);
void vita_lockstep_free(void);
#endif
