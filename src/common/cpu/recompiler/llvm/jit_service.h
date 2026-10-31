/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT: guest code the AOT module lacks, compiled once a guest thread reaches it. snx_lookup (cpu.h) finds
   no function in CPU.disp and calls RtAPI.miss; the runtime's miss asks snx_jit_wait for the code at pc. When the
   service has compiled it, that code is the answer at once. Otherwise the pc goes to the front of the queue and the
   thread waits for the batch that compiles it (the runtime takes it off its guest core meanwhile). There is no
   interpreter behind it: what can't be compiled (a pc outside the images, a compile LLVM or the loader refuses) gets
   NULL, which the runtime treats as the end of the game.

   Compile threads of the service take the queued pcs in batches. A batch finds the functions at all of its pcs and
   those they call (snx_codegen_discover, one budget for the batch, each branch graph bounded separately), emits them
   into one LLVM module (snx_ir_emit, as
   the AOT build does), loads its object into the code arena (code_loader.c, packed after the code already there) and
   publishes every leader of every function into the dispatch table, where later lookups find it without a miss. The
   compile threads are host threads: none is a guest thread or holds a guest core, so a compile never delays another
   guest thread's turn on its core.

   Two queues: the pcs guest threads missed, and the warm start's (snx_jit_warm, the pcs earlier runs missed), taken
   only while no miss waits and in smaller batches, so a miss that arrives behind a warm batch waits less. A pc is in
   the service's table once (queued, compiling, compiled or failed), so it is compiled once by the queue, though two
   batches compiling at once may each hold a function both call (the first to publish keeps the slot).

   The arena's first load is a copy of the runtime library (rtlib.c): JIT code calls its helpers directly, in reach of
   a 32-bit displacement. The library has no writable globals, so the copy shares nothing with the AOT module's.
   JIT code calls the AOT module's functions directly (config.aot_*), the functions of its own unit directly, and
   anything else through CPU.disp (CALLDYN), the code of other units included. Code is never freed: the arena lives as
   long as the service, which outlives every thread that runs its code.

   Images can join while the service runs (snx_jit_add_image: an NRO the game maps). Their pcs are outside CPU.disp,
   so the service keeps a table for each, which snx_jit_resolve reads without a lock before anything else: a call
   into such code misses in snx_lookup and costs RtAPI.miss and that load once compiled. Removing one (the game
   unmaps the NRO) waits for the batches reading it and forgets its pcs.

   Not NX-specific: gnuabi's execution can create one too, with its code ranges and table */
#ifndef SNX_RECOMPILER_LLVM_JIT_SERVICE_H
#define SNX_RECOMPILER_LLVM_JIT_SERVICE_H
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/aarch64/cpu.h"

/* instructions a batch discovers beyond the regions at its pcs (snx_codegen_discover's budget) */
#define SNX_JIT_BUDGET  20000
/* A live miss must not pull a huge branch graph into one LLVM function before it can resume. */
#define SNX_JIT_REGION_INSTRUCTIONS 4096
#define SNX_JIT_OVERFLOW_INSTRUCTIONS 64
#define SNX_JIT_LIVE_BUDGET 64
/* pcs a batch takes from the misses' queue; a warm batch takes an eighth of it. LLVM's fixed cost per module
   (context, pass pipeline, code generator, load) is shared by the batch, and a start-up burst of misses goes through
   in a few batches instead of thousands of compiles */
#define SNX_JIT_BATCH   256
/* compile threads at most */
#define SNX_JIT_THREADS 4
/* images added while the service runs at most (snx_jit_add_image), as many as ldr:ro maps NROs */
#define SNX_JIT_IMAGES  64

typedef struct SnxJitService SnxJitService;

/* one batch, for the log (config.report) */
typedef struct {
    SnxU64 pc;                                  /* its first pc */
    size_t pcs, functions, instructions, bytes; /* bytes: of the arena the load took */
    double seconds;
    const char* error; /* NULL when it compiled, else why its pcs stay with the caller's interpreter */
    int dropped;       /* guest memory changed while it compiled: nothing published, its pcs queued again */
} SnxJitReport;

typedef struct {
    /* the guest code the JIT may compile: images[i].image is the guest memory at images[i].base, readable for as long
       as the service lives. A pc outside their text is refused */
    const SnxCodeImage* images;
    size_t image_count;
    /* CPU.disp: one slot per instruction of [code_lo, code_hi), which the service fills from NULL only */
    GuestFn* dispatch;
    SnxU64 code_lo, code_hi;
    /* the AOT module's entries by ascending address: JIT code calls these directly */
    const u64* aot_addresses;
    const GuestFn* aot_functions;
    size_t aot_count;
    /* every call through CPU.disp, as the AOT module's --hook-direct-calls (gpu/nvn_trace.c wraps slots) */
    int hook_direct_calls;
    /* SVC #imm through RtAPI.svcs, as the AOT module's (SnxCodegen.direct_svc): the runtime must give RtAPI.svcs */
    int direct_svc;
    /* the guest's A64 features, what decodes (SnxCodegen.features): 0 is the Switch's */
    SnxU64 features;
    size_t budget;    /* 0: SNX_JIT_BUDGET */
    size_t batch;     /* 0: SNX_JIT_BATCH */
    size_t region_instructions; /* 0: adaptive REGION/OVERFLOW limits above; SIZE_MAX: previous unbounded discovery */
    size_t live_budget; /* 0: SNX_JIT_LIVE_BUDGET, SIZE_MAX: use budget for live misses too */
    unsigned threads; /* compile threads, 0: by the host's CPUs (jit_service.c), at most SNX_JIT_THREADS */
    char opt;         /* the -O letter, 0: '0' */
    /* after each batch, on its compile thread, outside every lock of the service (NULL: none) */
    void (*report)(const SnxJitReport* report);
    /* the JIT's cache (NULL: none): every unit compiled is appended to this file, and a later run loads a unit again
       instead of compiling it when the code it was compiled from is the same, byte for byte, and the file was written
       by the same ABI, IR object version, target triple, LLVM, runtime library, host CPU and settings */
    const char* cache_path;
} SnxJitConfig;

/* NULL with the reason in error: no code arena, a host the loader doesn't support (Windows before 10 1803), entries out of
   order, no compile thread. The config's arrays are used where they are */
SnxJitService* snx_jit_service_new(const SnxJitConfig* config, char* error, size_t error_size);
/* Stops the compile threads, after the batches they are compiling. Nothing may run the service's code any more */
void snx_jit_service_free(SnxJitService* service);
/* The guest function to call for pc (as GuestFn(c, pc)), or NULL while it is queued or compiling, or when it can't be
   compiled (unaligned, outside the images, a compile that failed). Queues it at the front of the misses. Never waits */
GuestFn snx_jit_resolve(SnxJitService* service, SnxU64 pc);
/* whether the service's images hold pc: it can be compiled from them (or already is), so no image of a block the game
   mapped itself must be added for it */
int snx_jit_covers(const SnxJitService* service, SnxU64 pc);
/* The guest function for pc, waiting for its compile when it has none yet: NULL only when it can't be compiled */
GuestFn snx_jit_wait(SnxJitService* service, SnxU64 pc);
/* Queues pcs to compile while no miss waits, those the service has no entry for and the dispatch table no function:
   the number queued */
size_t snx_jit_warm(SnxJitService* service, const SnxU64* pcs, size_t count);
/* Returns once nothing is queued or compiling: for tests, the runtime never waits for the JIT */
void snx_jit_drain(SnxJitService* service);
/* Guest code that joins while the service runs: image's text, readable until snx_jit_remove_image, outside the
   config's table and every other added image. 0, or -1 (overlap, SNX_JIT_IMAGES already, out of memory) */
int snx_jit_add_image(SnxJitService* service, const SnxCodeImage* image);
/* The image added at base goes, before its memory does: waits for the batches reading it, and forgets its pcs and
   code (code is never freed, but nothing finds it any more). No thread may run its code any more */
void snx_jit_remove_image(SnxJitService* service, SnxU64 base);
/* Guest memory in [addr, addr+size) changed: what was compiled from it is stale. Its dispatch slots and entries go;
   the next lookup compiles a pc in the range from what is there now, and a compile already running publishes nothing
   for it. The code stays loaded, as a thread may still be running it */
typedef void (*SnxJitInvalidatedRange)(void* context, SnxU64 start, SnxU64 end);
void snx_jit_invalidate(SnxJitService* service, SnxU64 addr, SnxU64 size);
void snx_jit_invalidate_ranges(SnxJitService* service, SnxU64 addr, SnxU64 size, SnxJitInvalidatedRange callback,
                               void* context);

typedef struct {
    unsigned long batches, compiled, failures; /* compiled, failures: pcs the queues held */
    unsigned long dropped;                     /* batches whose code memory changes made stale (SnxJitReport) */
    unsigned long functions, instructions;
    unsigned long queued; /* pcs waiting */
    unsigned long cached; /* units loaded from the cache instead of compiled */
    size_t bytes;         /* of the arena, the runtime library's copy included */
    double seconds;       /* compiling, summed over the compile threads */
    unsigned threads;
} SnxJitStats;
void snx_jit_stats(SnxJitService* service, SnxJitStats* stats);

#endif
