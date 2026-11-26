/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Switch's settings for the code a run compiles, as a console's run takes them (common/utilities/console_settings.c):
   perf_map and gdb_jit, given by value so the caller's settings stay its own */
#ifndef SNX_RECOMPILER_JIT_JIT_SETTINGS_H
#define SNX_RECOMPILER_JIT_JIT_SETTINGS_H

/* Before the run's first compile, its recompile's too: the code the JIT compiles named in perf's map with perf_map
   (Linux) and to a GDB attached to the process with gdb_jit, which who's line on stderr says, as GDB stops the game for
   each compile while attached. Once a process */
void snx_jit_settings(const char* who, int perf_map, int gdb_jit);
#endif
