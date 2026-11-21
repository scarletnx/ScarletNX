/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* armature (github.com/eXxe-elcior/armature, an AArch64 to x86-64 JIT in Rust) in place of the AOT code and the
   runtime JIT, behind the setting armature (x86-64 Linux builds with SNX_ARMATURE): each guest thread runs in it from
   its entry until the entry function returns, as under NCE. The runtime answers it through RtAPI, as it answers the
   recompiled code: an SVC, BRK, an instruction it has no code for, the counter, CPU.poll.

   The hooks and NVN take a function as they do in the recompiled code, by its dispatch slot (g_disp): armature reads
   the function's first word as a BRK (the guest's memory stays as it is), whose handler runs what the slot holds and
   sends the guest to the return address it leaves. The function's own code, which an empty slot means, runs from a
   stub of that first word, placed past the guest's address space where only armature's code reads see it */
#ifndef SNX_ARMATURE_H
#define SNX_ARMATURE_H
#include "common/cpu/aarch64/cpu.h"

struct KThread;

/* after the executables are loaded and nce_init, before the hooks: the setting armature. The game's code runs in
   armature from here when arma_on() */
void arma_init(void);
int arma_on(void);
/* thread_run's loop under armature: t runs from pc with the state of its CPU, until its entry function returns */
void arma_thread_run(struct KThread* t, u64 pc);
/* t's CPU.poll was set: the guest code t runs in armature calls RtAPI.poll at its next synchronization point. Any
   thread */
void arma_kick(struct KThread* t);
/* the code of a dispatch slot: f, or under armature the function's own code for an empty slot */
GuestFn arma_slot(GuestFn f);
/* a handler took the dispatch slot of the function at addr: under armature its first word reads as a BRK from here */
void arma_slot_taken(u64 addr);
/* the crashing thread's registers into its CPU, when it ran guest code in armature, for the crash report */
void arma_crash_context(void);
/* the totals, into the log, at exit */
void arma_report(void);

#endif
