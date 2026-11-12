/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ARM_EXEC_H
#define SNX_ARM_EXEC_H
#include "common/cpu/arm/arm.h"
#include <stddef.h>

/* A runnable AArch32 (A32/Thumb) block image, shared by the GNU ABI and NX
   runtimes. Blocks compile on demand through the ARM LLVM backend. */
typedef struct SnxArmExecRange {
    uint64_t address;
    const unsigned char* data;
    size_t size;
} SnxArmExecRange;

/* The DRY SVC surface. The runtime supplies one handler and both A32 and A64
   feed it 64-bit values: on A32 the executor zero-extends r0-r5 into args and
   r7 into number, so the same HLE works for either instruction set. */
typedef int (*SnxArmSyscallFn)(void* context, uint32_t imm, uint64_t number, const uint64_t arguments[6]);

typedef struct SnxArmExec SnxArmExec;

/* Returns NULL with a bounded error on failure. */
SnxArmExec* snx_arm_exec_new(char* error, size_t error_size);
/* The profile the blocks snx_arm_exec_block compiles from now on are for (A57 until set): the Vita's A9 */
void snx_arm_exec_set_profile(SnxArmExec* exec, SnxArmProfile profile);
void snx_arm_exec_free(SnxArmExec* exec);
/* The block at pc (Thumb when thumb is set), compiled now unless it was before, from base + pc (the guest's
   memory; ranges bound how far it may read). NULL when pc is outside the ranges or the compiler fails. Not thread-safe:
   callers serialize */
SnxArmBlock snx_arm_exec_block(SnxArmExec* exec, const SnxArmExecRange* ranges, size_t range_count, uint32_t pc,
                               int thumb, const unsigned char* base);
/* Forgets the blocks snx_arm_exec_block compiled for a pc in [lo, hi), whose code there changed (an NRO unmapped and
   another mapped in its place): the next call for one compiles it again. Their code stays loaded, as a thread may still
   be in one. Not thread-safe: callers serialize */
void snx_arm_exec_forget(SnxArmExec* exec, uint64_t lo, uint64_t hi);
/* A block compiled now from the code given (count A32 words, or Thumb halfwords when thumb is set, as host integers)
   for the profile and memory mode (a SnxArmMemoryMode of llvm.h: 0 flat, 1 checked (checked.h), 2 hosted): the
   caller's to look up and keep, never cached here, each compile a new copy. A hosted block leaves the checks of its
   entry to the caller. *consumed is how many it compiled. NULL when the compiler fails. Not thread-safe: callers
   serialize */
SnxArmBlock snx_arm_exec_compile(SnxArmExec* exec, const void* code, size_t count, uint32_t pc, int thumb,
                                 SnxArmProfile profile, int memory, size_t* consumed);
/* Runs from cpu->r[15] until the process exits (cpu->exit set by a trap or the
   syscall handler). code_ranges bound block compilation. Returns cpu->exit. */
int snx_arm_exec_run(SnxArmExec* exec, SnxArmCPU* cpu, const SnxArmExecRange* ranges, size_t range_count,
                     SnxArmSyscallFn syscall, void* context);

#endif
