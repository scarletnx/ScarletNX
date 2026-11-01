/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 32-bit game: its module (nx/loader/module_abi.h's SnxArmModule), the thread loop running its AArch32 blocks, and
   the backend they leave coprocessors and what they can't run to (cpu/arm/arm.h's SnxArmRt) */
#ifndef SNX_NX_ARM_H
#define SNX_NX_ARM_H
#include "nx/loader/module_abi.h"

struct KThread;

/* the 32-bit game's module, NULL for a 64-bit one */
extern const SnxArmModule* g_arm_mod;
/* the backend the game's threads run with (SnxArmCPU.rt) */
extern const SnxArmRt g_arm_rt;
/* The SnxModule the rest of the runtime reads (its modules, title and main thread) for a 32-bit module, after checking
   its version: NULL when it is from another ScarletNX version. It has no A64 entries */
const SnxModule* arm_module(const SnxArmModule* module);
/* the block table, from the module's entries; before any thread runs */
void arm_dispatch_init(void);
/* runs the thread's AArch32 code until it exits; never returns */
void arm_thread_run(struct KThread* t);
/* the recompiled block whose host code is closest below host_pc, for the crash report: 0 when none is near it, else its
   guest address (bit 0 set for Thumb) and *offset into it */
u32 arm_block_at(const void* host_pc, u64* offset);
/* The slot of the block at addr (A32, or Thumb when thumb), for a hook to put a block of its own in: NULL when addr is
   outside the modules' code. A recompiled block's slot holds it from the start, and one compiled while the game runs
   is stored there once. A store into it takes effect at the next call, as every call ends a block */
SnxArmBlock* arm_slot(u32 addr, int thumb);
/* Runs the guest's code on c, which is a game thread's and called this from inside a hook's handler: block first, or
   the block at r15 when it is NULL, and then each block r15 names, until r15 is stop_at in the state its bit 0 says
   (set for Thumb), as a call returns to its return address. SVCs and core polls run as in the thread's own loop */
void arm_run(SnxArmCPU* c, SnxArmBlock first, u32 stop_at);
/* the block at addr (A32, or Thumb when thumb), compiled now when the recompiler didn't make it: what its slot held
   before a hook took it. NULL when addr isn't code */
SnxArmBlock arm_block(u32 addr, int thumb);
/* the 32-bit registers of the game thread whose CPU c is (hle/hook/hook.h's hook_a32_cpu the other way) */
SnxArmCPU* arm_cpu(CPU* c);
/* the modules' code, [lo, hi): 0 when there is none (a 64-bit game) */
int arm_code_range(u64* lo, u64* hi);

#endif
