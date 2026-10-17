/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_JIT_H
#define SNX_GNU_JIT_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/execution/execution.h"
#include <stddef.h>
/* How guest A64 code becomes host code. Both modes share one dispatch table over the whole guest address space
   (cpu->disp with code_lo 0): an empty slot sends the generated code to gnu_jit_miss (gnu_rtapi's RtAPI.miss), which
   compiles the code there in this process with the LLVM backend and runs it.
     JIT: everything is compiled that way, as execution reaches it.
     AOT: the main executable is compiled before it starts, with the host compiler (common/cpu/execution); the code
          the program maps later (the dynamic linker, shared libraries) still comes through gnu_jit_miss. */
enum { GNU_JIT, GNU_AOT };
struct snx_recompiler;
struct snx_recompiled;
typedef struct GnuJit {
    int mode;
    const char* cc; /* AOT: the host compiler, NULL for the build's */
    struct snx_recompiler* recompiler;
    GuestFn* table; /* guest address >> 2 -> function, reserved for the whole guest address space */
    struct snx_recompiled** modules;
    size_t module_count, module_capacity;
    SnxExecution aot;
    int verbose;
    size_t step;                     /* instructions a just-in-time step compiles */
    uint64_t changed_lo, changed_hi; /* what gnu_jit_invalidate took since the last gnu_jit_changed */
} GnuJit;
/* cache: the folder of compiled modules (snx_recompiler_set_cache), NULL for none */
int gnu_jit_init(GnuJit* jit, int mode, const char* cc, const char* cache, char* error, size_t error_size);
/* the host code of a previous program goes (execve), the table stays */
void gnu_jit_reset(GnuJit* jit);
void gnu_jit_destroy(GnuJit* jit);
/* AOT: compile the main executable's code ranges; nothing in JIT mode */
int gnu_jit_prepare(GnuJit* jit, const SnxExecRange* ranges, size_t count, uint64_t entry, char* error,
                    size_t error_size);
void gnu_jit_bind(GnuJit* jit, CPU* cpu);
/* runs the guest function at pc: its host function returns when the guest function returns */
void gnu_jit_call(GnuJit* jit, CPU* cpu, uint64_t pc);
/* a host function for guest address pc (tests) */
void gnu_jit_install(GnuJit* jit, uint64_t pc, GuestFn function);
/* RtAPI.miss (gnu_rtapi): a pc the table has no function for, compiled from the executable mapping around it and run,
   or a fault when it isn't executable */
void gnu_jit_miss(CPU* c, u64 pc);
/* the guest code in [address, address + length) changed or went: its host code is not entered again */
void gnu_jit_invalidate(GnuJit* jit, uint64_t address, uint64_t length);
/* whether code at pc went since the last call (a syscall's mprotect, munmap or mmap over it) */
int gnu_jit_changed(GnuJit* jit, uint64_t pc);
/* Windows reserves the table and commits it as it is touched: nonzero when address was in it and is usable now */
int gnu_jit_commit(GnuJit* jit, uintptr_t address);
#endif
