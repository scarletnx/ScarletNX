/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The recompiler as a library (issue #171): ARM64 (A64) and AArch32 (A32/Thumb) machine code in memory to host code in
   the calling process, with none of the Switch runtime (HLE, GPU) behind it. The caller provides guest memory and the
   SVC handler, and runs the result on CPU states it owns.

   A64, the recompiled path:
     struct snx_recompiler* r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
     struct snx_recompiled* m = snx_recompile(r, 0x1000, code, size);  // code at guest address 0x1000
     RtAPI rt; snx_runtime_defaults(&rt);
     rt.svc = my_svc; rt.undef = my_undef; rt.brk = my_brk;              // my_svc is an snx_svc_call64
     CPU cpu = {0}; cpu.mem = guest_memory; cpu.rt = &rt; cpu.x[30] = 0;  // returning to 0 ends the run
     snx_recompiled_bind(m, &cpu);
     snx_recompiled_run(m, &cpu, 0x1000, 0);
   The guest memory is flat: guest address a is host byte cpu.mem[a] (cpu.h), and the code must be present in it at
   its address. Every recompiled function is a
   host function F(CPU*, entry) that returns when the guest function returns (its RET sets cpu->pc to x30), or after
   an undefined instruction word or BRK (cpu->pc at the instruction, after the rt->undef or rt->brk call). The
   generated code's contract with the caller is cpu.h (the CPU state, SNX_ABI) and runtime/interface.h (RtAPI).

   A32: snx_recompile32 wraps the AArch32 block compiler (cpu/arm/exec.h): blocks are compiled on first use, the state
   is SnxArmCPU (cpu/arm/arm.h), and the SVC handler is an snx_svc_call32. LLVM only.

   This header needs the GNU C dialect cpu.h is written in (Clang or GCC). Not thread-safe per object: one thread
   uses a given snx_recompiler or snx_recompiled at a time; a recompiled module may be bound to many CPUs run by
   many threads. */
#ifndef SNX_RECOMPILER_LIBRARY_H
#define SNX_RECOMPILER_LIBRARY_H

#include <stddef.h>
#include <stdint.h>
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"

/* the shared library's exported interface (SNX_RECOMPILER_EXPORTS while it is built); nothing for the static one */
#if defined(SNX_RECOMPILER_EXPORTS) && defined(_WIN32)
#define SNX_RECOMPILER_API __declspec(dllexport)
#elif defined(SNX_RECOMPILER_EXPORTS)
#define SNX_RECOMPILER_API __attribute__((visibility("default")))
#else
#define SNX_RECOMPILER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the recompiler ---- */

/* flags of snx_recompiler_new: the backend. Exactly one; LLVM is built into every recompiler, libgccjit only where
   the build found it (snx_recompiler_new fails otherwise, and snx_recompiler_has tells beforehand) */
#define SNX_RECOMPILER_FLAG_LLVM   1 /* LLVM: the default and the recommended backend */
#define SNX_RECOMPILER_FLAG_GCCJIT 2 /* libgccjit (for hosts LLVM has no code generator for); A64 only */

struct snx_recompiler;

/* NULL when the flags name no usable backend or the backend cannot target this host (snx_recompiler_has) */
SNX_RECOMPILER_API struct snx_recompiler* snx_recompiler_new(int flags);
SNX_RECOMPILER_API void snx_recompiler_free(struct snx_recompiler* r);
/* 1 when a recompiler with these flags can be made in this build, on this host */
SNX_RECOMPILER_API int snx_recompiler_has(int flags);
/* The contract version of the code this library generates (SNX_ABI of its cpu.h): a caller built against another
   cpu.h must not run it */
SNX_RECOMPILER_API unsigned snx_recompiler_abi(void);
/* The last failure of a call on r, as text owned by r (valid until the next call on r); "" when none */
SNX_RECOMPILER_API const char* snx_recompiler_error(const struct snx_recompiler* r);
/* The -O letter the generated code is optimized at: '0', '1', '2' (the default), '3', 's' or 'z' */
SNX_RECOMPILER_API int snx_recompiler_set_optimization(struct snx_recompiler* r, char opt);
/* Code units compiled at once: host threads with LLVM, child processes with libgccjit (one compile per process); 0,
   the default, uses one per processor */
SNX_RECOMPILER_API void snx_recompiler_set_jobs(struct snx_recompiler* r, unsigned jobs);
/* Progress lines on stdout, as the scarletnx command prints them; off by default */
SNX_RECOMPILER_API void snx_recompiler_set_verbose(struct snx_recompiler* r, int verbose);
/* A folder the generated code's text (LLVM IR as .ll, libgccjit's C dump) is saved in, for inspection; NULL (the
   default) saves nothing. The string is copied */
SNX_RECOMPILER_API int snx_recompiler_set_keep_ir(struct snx_recompiler* r, const char* folder);
/* A folder of linked modules (LLVM), made if missing, by a hash of everything a module is made from: the code and its
   addresses, the entries, the settings, SNX_ABI and the host CPU. A module found there is loaded instead of compiled,
   and one compiled is saved there; NULL (the default) keeps none. The string is copied. 0, or -1 when the folder
   can't be made */
SNX_RECOMPILER_API int snx_recompiler_set_cache(struct snx_recompiler* r, const char* folder);

/* ---- A64 ---- */

/* A64 SVC #imm: generated code calls it with cpu->pc already past the SVC. The handler reads its arguments from and
   writes its results to the registers in cpu; cpu->thread is the caller's own pointer (never read by the generated
   code). This is RtAPI.svc (runtime/interface.h) */
typedef void (*snx_svc_call64)(CPU* cpu, uint32_t imm);

/* Fills every hook of an RtAPI with a default the generated code can call: ticks counts a monotonic clock at the
   console's 19.2 MHz, fpcr_changed, sev, wfe, poll and note_hint do nothing, miss runs the code the module the CPU is
   bound to (snx_recompiled_bind) compiles for a pc it has no function for, waiting for the compile (its JIT, started
   by the first miss), and calls undef for a pc it can't compile (outside the code ranges, a host without the JIT). svc, undef and brk are set to NULL: the
   caller supplies all three (snx_recompiled_bind refuses a CPU whose table lacks one). brk(cpu, pc, imm) is called
   with cpu->pc at the instruction and the host function returns right after, so a handler that wants the run to end
   sets cpu->pc to the stop address. undef(cpu, pc, word) is called the same way for an instruction word that decodes
   to nothing, but also from inside an instruction whose encoding the helpers reject (a reserved SIMD or system
   register form), where cpu->pc is not set and execution goes on with the next instruction: a handler that must stop
   there records the fault for the caller, or longjmps out */
SNX_RECOMPILER_API void snx_runtime_defaults(RtAPI* rt);

/* A range of guest code: size bytes at guest address address, read from code (which the call copies). A64:
   address and size are multiples of 4, and code is required. A32: multiples of 2 (Thumb), the range ends at or below
   4 GiB, and code is unused (the blocks decode from cpu->mem) */
struct snx_code_range {
    uint64_t address;
    const void* code;
    size_t size;
};

struct snx_recompiled;

/* Recompiles the A64 code in buffer code (size bytes), as it will be found at guest address address, and loads the
   result into this process. Functions are found by exploring calls and branches from the start of the buffer and
   from the hints; what the exploration does not reach is compiled when first reached. NULL with the reason in
   snx_recompiler_error (except out of memory, which ends the process, as everywhere in this recompiler) */
SNX_RECOMPILER_API struct snx_recompiled* snx_recompile(struct snx_recompiler* r, uint64_t address, const void* code,
                                                        size_t size);
/* The same for several ranges (the .text of several modules), with hint_count known function addresses (an entry
   point, a symbol table) as further starting points; a hint is also where a function may be entered */
SNX_RECOMPILER_API struct snx_recompiled* snx_recompile_ranges(struct snx_recompiler* r,
                                                               const struct snx_code_range* ranges, size_t range_count,
                                                               const uint64_t* hints, size_t hint_count);
/* A just-in-time compiler's step: the functions at the hints, then the functions they call, breadth first, until about
   limit instructions are found; a call target skip(context, address) says is compiled already is left to the dispatch
   table. Only those functions are compiled, and the module's table covers just them. The ranges are where code may be
   read (a program's executable segments) */
SNX_RECOMPILER_API struct snx_recompiled* snx_recompile_from(struct snx_recompiler* r,
                                                             const struct snx_code_range* ranges, size_t range_count,
                                                             const uint64_t* hints, size_t hint_count, size_t limit,
                                                             int (*skip)(void* context, uint64_t address),
                                                             void* context);
/* Unloads the host code. Every CPU bound to m must have stopped */
SNX_RECOMPILER_API void snx_recompiled_free(struct snx_recompiled* m);
/* The guest address range the module's dispatch table covers (code_lo inclusive, code_hi exclusive) */
SNX_RECOMPILER_API void snx_recompiled_range(const struct snx_recompiled* m, uint64_t* code_lo, uint64_t* code_hi);
/* Functions recompiled, and instructions without an emitter: always 0, as a module with one fails to build (kept so
   callers of earlier versions still link) */
SNX_RECOMPILER_API size_t snx_recompiled_functions(const struct snx_recompiled* m);
SNX_RECOMPILER_API size_t snx_recompiled_fallbacks(const struct snx_recompiled* m);
/* Points cpu at the module: its dispatch table and code range (cpu->disp, code_lo, code_hi). cpu->mem and cpu->rt
   are the caller's and must be set before, cpu->rt with svc, undef, brk and miss (snx_runtime_defaults' miss compiles
   what the bound module lacks). 0, or -1 when cpu->rt lacks a hook */
SNX_RECOMPILER_API int snx_recompiled_bind(struct snx_recompiled* m, CPU* cpu);
/* The host function for guest address pc: the recompiled function (also for a pc inside one, at a leader), or the
   one the module's JIT compiles for any other pc, waiting for it. NULL when pc can't be compiled */
SNX_RECOMPILER_API GuestFn snx_recompiled_lookup(struct snx_recompiled* m, uint64_t pc);
/* The recompiled function entered at pc (a leader), NULL for any other pc */
SNX_RECOMPILER_API GuestFn snx_recompiled_function(const struct snx_recompiled* m, uint64_t pc);
/* What a miss in the module runs (snx_runtime_defaults' RtAPI.miss, snx_recompiled_lookup and _call) instead of the
   module's JIT: a caller's own just-in-time compiler, called as the guest function at the pc, which compiles the code
   there and runs it. NULL restores the module's JIT. A caller with its own RtAPI.miss and dispatch table (gnuabi)
   doesn't need it */
SNX_RECOMPILER_API void snx_recompiled_set_fallback(struct snx_recompiled* m, GuestFn fallback);
/* Calls the guest function at pc (snx_recompiled_lookup) and returns when that host function returns: at the guest
   function's RET, with cpu->pc its return address, or after rt->undef or rt->brk */
SNX_RECOMPILER_API void snx_recompiled_call(struct snx_recompiled* m, CPU* cpu, uint64_t pc);
/* Runs from pc until cpu->pc is stop: snx_recompiled_call(pc), then of cpu->pc, again and again. The caller makes
   the entry function return to stop (cpu->x[30] = stop) and has its undef and brk hooks set cpu->pc = stop when
   they end the run, or the loop re-enters the faulting instruction */
SNX_RECOMPILER_API void snx_recompiled_run(struct snx_recompiled* m, CPU* cpu, uint64_t pc, uint64_t stop);

/* ---- A32 (and Thumb) ---- */

/* A32 SVC #imm, with the registers presented as the console's 64-bit SVC surface: number is r7, arguments r0 to r5
   zero-extended. Returns 0 to continue, or any other value to end the run (cpu/arm/exec.h's SnxArmSyscallFn) */
typedef int (*snx_svc_call32)(void* context, uint32_t imm, uint64_t number, const uint64_t arguments[6]);

struct snx_recompiled32;

/* Prepares the A32/Thumb code at guest address address (size bytes; the bytes are read from cpu->mem at run time,
   so they must be there too) for execution by blocks compiled on first use. LLVM only: NULL with the reason in
   snx_recompiler_error for a libgccjit recompiler */
SNX_RECOMPILER_API struct snx_recompiled32* snx_recompile32(struct snx_recompiler* r, uint64_t address, size_t size);
SNX_RECOMPILER_API struct snx_recompiled32*
snx_recompile32_ranges(struct snx_recompiler* r, const struct snx_code_range* ranges, size_t range_count);
SNX_RECOMPILER_API void snx_recompiled32_free(struct snx_recompiled32* m);
/* Runs from cpu->r[15] (CPSR.T selects Thumb) until an exit: an SVC the handler ends the run on, an undefined
   instruction, BKPT, a data abort, or a branch out of the ranges (SNX_ARM_BAD_ENTRY). Returns cpu->exit
   (SnxArmExit), 0 when the handler ended the run. cpu->exit is cleared on entry, so a run after an exit starts over
   from r[15] */
SNX_RECOMPILER_API int snx_recompiled32_run(struct snx_recompiled32* m, SnxArmCPU* cpu, snx_svc_call32 svc,
                                            void* context);

#ifdef __cplusplus
}
#endif
#endif
