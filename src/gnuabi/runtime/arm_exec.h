/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* AArch32 (A32/Thumb) execution for the GNU ABI runtime. Blocks are compiled
   on demand by the shared executor (common/cpu/arm/exec.h: the ARM LLVM
   backend, loaded by the runtime JIT's loader) and cached by address and state.
   The CPU state is SnxArmCPU, separate from the A64 CPU*. */
#ifndef SNX_GNU_ARM_EXEC_H
#define SNX_GNU_ARM_EXEC_H
#include "gnuabi/process/process.h"
#include "common/cpu/arm/arm.h"
/* Compiles blocks on demand and runs the thread's guest code until it exits, faults or execs: why it stopped, as
   execution.c's run_a64 says it (1 the process ends, 2 an execve, 4 the thread ends). The AArch32 guest gets no
   signal handlers: a system call that a signal stopped fails with EINTR */
int gnu_arm_run(GnuThread* thread);
#endif
