/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Armature in the runtime: the game's A64 code runs on Armature, a JIT from A64 to x86-64 in Rust that the runtime
   loads from the library that the setting armature names, in place of the code that the recompiler made ahead of
   time. It is for A/B tests against that code.

   It takes the place of NCE's native code (nce.h), with the same hooks: an empty dispatch slot (g_disp) is the
   game's own code, which Armature runs (armature_slot). A slot that a handler holds (a hook, NVN, the linker) is a
   trap at the start of the block there: Armature calls the runtime, which runs the handler. The SVCs, BRK, UDF,
   the counter and CPU.poll come to the runtime through Armature's callbacks, which answer them through RtAPI */
#ifndef SNX_ARMATURE_H
#define SNX_ARMATURE_H
#include "common/cpu/aarch64/cpu.h"

struct KThread;

/* after the executables are loaded, before the hooks: loads the library and empties the dispatch slots of the AOT
   module. The game's code runs on Armature from here when armature_on() */
void armature_init(void);
int armature_on(void);
/* thread_run's loop under Armature: t runs from pc with the state of its CPU, until its entry function returns */
void armature_thread_run(struct KThread* t, u64 pc);
/* t's CPU.poll was set: the guest code that t runs calls RtAPI.poll at its next check. Any thread */
void armature_kick(struct KThread* t);
/* the code of an empty dispatch slot under Armature (the game's own, which Armature runs), else NULL */
GuestFn armature_slot(void);
/* a handler took the dispatch slot of the function at addr: code that Armature translated there before goes */
void armature_slot_taken(u64 addr);
#endif
