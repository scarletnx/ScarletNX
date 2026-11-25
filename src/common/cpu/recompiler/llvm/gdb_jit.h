/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GDB's JIT interface for the JIT loader (code_loader.c): each object a load placed in the arena is handed to an
   attached GDB, which then names the functions in it (and has the object's .eh_frame to unwind with). Off unless
   asked (snx_gdb_jit_enable: the NX runtime's gdb_jit with the GDB stub): GDB stops the process at every
   registration to read the object, and each object's copy stays in the descriptor's list until its loader goes; GDB
   is told of each removal. On Linux the same loads can also name their code for perf (snx_gdb_jit_perf_map), without
   changing what GDB sees: perf gets one map line per load, and the map keeps the lines of loads that are gone because
   perf still has samples that their code took */
#ifndef SNX_RECOMPILER_LLVM_GDB_JIT_H
#define SNX_RECOMPILER_LLVM_GDB_JIT_H
#include <stddef.h>

/* Registration on, for the rest of the process: once, before the first load and the threads that load. Nothing on
   Windows, where the GDB stub is not (nx/runtime/gdbstub.c) and the loader takes COFF */
void snx_gdb_jit_enable(void);
/* A copy of the size bytes at object, a load's object file, for owner (a loader): the caller writes into it where the
   load put the object's parts (an ELF object's sh_addr), then hands it to snx_gdb_jit_add. NULL when both GDB
   registration and the perf map are off, or out of memory */
unsigned char* snx_gdb_jit_copy(const void* owner, const void* object, size_t size);
/* Registers copy (snx_gdb_jit_copy's) with GDB when that is on and writes its perf map line when the map is on, until
   its owner's snx_gdb_jit_remove */
void snx_gdb_jit_add(unsigned char* copy);
/* Removes every object owner added: before its code goes */
void snx_gdb_jit_remove(const void* owner);
/* Linux only: truncate /tmp/perf-<pid>.map, then write the loaded code's names to it, one line per load as it is
   emitted, for perf's CPU samples and flame graphs. The truncate removes what a crash or a reused pid left behind.
   No-op on other hosts. On before the first load (nx/runtime/guest_jit.c's jit_init) */
void snx_gdb_jit_perf_map(int on);
/* Append the lines a load recorded but did not emit itself, so no earlier sample loses its name. Call at exit;
   no-op off Linux or when the writer is off. */
void snx_gdb_jit_perf_map_snapshot(void);
#endif
