/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef _WIN32
#define _GNU_SOURCE /* MAP_NORESERVE, madvise */
#endif
#include "gnuabi/runtime/jit.h"
#include "gnuabi/process/process.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* instructions one just-in-time step compiles: the function execution reached and the functions it calls, breadth
   first. Each module carries its own copy of the runtime library, so steps are coarse */
#define JIT_STEP_INSTRUCTIONS 65536
#define TABLE_SLOTS           (GNU_ADDRESS_LIMIT / 4)
#define TABLE_BYTES           (TABLE_SLOTS * sizeof(GuestFn))

static void table_clear(GnuJit* jit, uint64_t first, uint64_t count) {
    unsigned char* begin = (unsigned char*)(jit->table + first);
    unsigned char* end = (unsigned char*)(jit->table + first + count);
#ifdef _WIN32
    /* only what was committed: the rest reads as zero once committed */
    while (begin < end) {
        MEMORY_BASIC_INFORMATION region;
        unsigned char* stop;
        if (!VirtualQuery(begin, &region, sizeof region)) return;
        stop = (unsigned char*)region.BaseAddress + region.RegionSize;
        if (stop > end) stop = end;
        if (region.State == MEM_COMMIT) memset(begin, 0, (size_t)(stop - begin));
        begin = stop;
    }
#else
    unsigned char* low = (unsigned char*)(((uintptr_t)begin + GNU_PAGE_SIZE - 1) & ~(uintptr_t)(GNU_PAGE_SIZE - 1));
    unsigned char* high = (unsigned char*)((uintptr_t)end & ~(uintptr_t)(GNU_PAGE_SIZE - 1));
    if (low >= high) {
        memset(begin, 0, (size_t)(end - begin));
        return;
    }
    memset(begin, 0, (size_t)(low - begin));
    memset(high, 0, (size_t)(end - high));
    /* whole pages go back to the zero page: a program's worth of slots costs nothing once it is gone */
    (void)madvise(low, (size_t)(high - low), MADV_DONTNEED);
#endif
}

static void table_set(GnuJit* jit, uint64_t pc, GuestFn function) {
#ifdef _WIN32
    if (!VirtualAlloc(jit->table + (pc >> 2), sizeof(GuestFn), MEM_COMMIT, PAGE_READWRITE)) return;
#endif
    jit->table[pc >> 2] = function;
}

int gnu_jit_commit(GnuJit* jit, uintptr_t address) {
#ifdef _WIN32
    uintptr_t base = (uintptr_t)jit->table;
    if (!jit->table || address < base || address - base >= TABLE_BYTES) return 0;
    return VirtualAlloc((void*)address, sizeof(GuestFn), MEM_COMMIT, PAGE_READWRITE) != NULL;
#else
    (void)jit;
    (void)address;
    return 0;
#endif
}

static void add_module(GnuJit* jit, struct snx_recompiled* m) {
    if (jit->module_count == jit->module_capacity) {
        jit->module_capacity = jit->module_capacity ? jit->module_capacity * 2 : 16;
        jit->modules = snx_realloc(jit->modules, jit->module_capacity * sizeof *jit->modules);
    }
    jit->modules[jit->module_count++] = m;
}

static int compiled(void* context, uint64_t address) {
    GnuJit* jit = context;
    return address < GNU_ADDRESS_LIMIT && jit->table[address >> 2] != NULL;
}

/* compiles the code at pc, from the executable mapping around it; 0 when pc has a function then */
static int compile(GnuProcess* p, uint64_t pc) {
    GnuJit* jit = &p->jit;
    GnuMemory* memory = &p->memory;
    const unsigned want = GNU_PAGE_MAPPED | GNU_PROT_EXEC;
    uint64_t first = pc / GNU_PAGE_SIZE, last = first + 1, lo, hi, a;
    struct snx_code_range range;
    struct snx_recompiled* m;
    double started = snx_seconds();
    int writable;
    /* the library's tables cover at most 1 GiB of guest addresses */
    while (first > 0 && (memory->pages[first - 1] & want) == want && last - first < 0x20000) first--;
    while (last < GNU_ADDRESS_LIMIT / GNU_PAGE_SIZE && (memory->pages[last] & want) == want && last - first < 0x40000)
        last++;
    range.address = first * GNU_PAGE_SIZE;
    range.size = (size_t)((last - first) * GNU_PAGE_SIZE);
    range.code = memory->base + range.address; /* executable guest pages are host-readable */
    /* Code the guest may write (a writable and executable page) is compiled a function at a time: its calls then go
       through the table, which a write empties (gnu_memory_watch_code), instead of straight to the old callee */
    writable = (memory->pages[pc / GNU_PAGE_SIZE] & GNU_PROT_WRITE) != 0;
    m = snx_recompile_from(jit->recompiler, &range, 1, &pc, 1, writable ? 1 : jit->step, compiled, jit);
    if (!m) {
        snprintf(p->diagnostic, sizeof p->diagnostic, "Cannot compile guest code at 0x%llx: %s", (unsigned long long)pc,
                 snx_recompiler_error(jit->recompiler));
        return -1;
    }
    snx_recompiled_range(m, &lo, &hi);
    for (a = lo; a < hi; a += 4) {
        GuestFn f = snx_recompiled_function(m, a);
        if (f) table_set(jit, a, f);
    }
    add_module(jit, m);
    if (writable) gnu_memory_watch_code(memory, lo, hi - lo);
    if (jit->verbose)
        fprintf(stderr, "scarletnx-gnuabi: JIT 0x%llx: %lu functions, 0x%llx-0x%llx, %.2f s\n", (unsigned long long)pc,
                (unsigned long)snx_recompiled_functions(m), (unsigned long long)lo, (unsigned long long)hi,
                snx_seconds() - started);
    return jit->table[pc >> 2] ? 0 : -1;
}

/* gnu_rtapi's RtAPI.miss: a pc the table has no function for */
void gnu_jit_miss(CPU* c, u64 pc) {
    GnuProcess* p = c->thread;
    GuestFn f;
    if ((pc & 3) || !gnu_memory_access(&p->memory, pc, 4, GNU_PROT_EXEC)) {
        c->pc = pc;
        gnu_process_fault(p, 11, "Guest instruction fetch violated mapped execution permissions");
        return;
    }
    f = p->jit.table[pc >> 2];
    if (!f) {
        if (compile(p, pc)) {
            char reason[sizeof p->diagnostic];
            memcpy(reason, p->diagnostic, sizeof reason);
            c->pc = pc;
            gnu_process_fault(p, 4, reason);
            return;
        }
        f = p->jit.table[pc >> 2];
    }
    f(c, pc);
}

int gnu_jit_init(GnuJit* jit, int mode, const char* cc, const char* cache, char* error, size_t error_size) {
    memset(jit, 0, sizeof *jit);
    jit->mode = mode;
    jit->cc = cc;
    jit->step = JIT_STEP_INSTRUCTIONS;
#ifdef _WIN32
    jit->table = VirtualAlloc(NULL, (SIZE_T)TABLE_BYTES, MEM_RESERVE, PAGE_NOACCESS);
#else
    jit->table =
        mmap(NULL, (size_t)TABLE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (jit->table == MAP_FAILED) jit->table = NULL;
#endif
    if (!jit->table) {
        snprintf(error, error_size, "cannot reserve the dispatch table");
        return -1;
    }
    jit->recompiler = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!jit->recompiler) {
        snprintf(error, error_size, "the LLVM backend cannot target this host");
        gnu_jit_destroy(jit);
        return -1;
    }
    if (cache && snx_recompiler_set_cache(jit->recompiler, cache))
        fprintf(stderr, "scarletnx-gnuabi: %s; compiling without it\n", snx_recompiler_error(jit->recompiler));
    return 0;
}

void gnu_jit_reset(GnuJit* jit) {
    size_t i;
    for (i = 0; i < jit->module_count; ++i) snx_recompiled_free(jit->modules[i]);
    jit->module_count = 0;
    if (jit->aot.module) snx_execution_free(&jit->aot);
    if (jit->table) table_clear(jit, 0, TABLE_SLOTS);
}

void gnu_jit_destroy(GnuJit* jit) {
    gnu_jit_reset(jit);
    free(jit->modules);
    if (jit->recompiler) snx_recompiler_free(jit->recompiler);
    if (jit->table) {
#ifdef _WIN32
        VirtualFree(jit->table, 0, MEM_RELEASE);
#else
        munmap(jit->table, (size_t)TABLE_BYTES);
#endif
    }
    memset(jit, 0, sizeof *jit);
}

static void aot_fetch(CPU* cpu, uint64_t address, uint32_t* word) {
    GnuProcess* p = cpu->thread;
    void* instruction;
    cpu->pc = address;
    instruction = gnu_memory_access(&p->memory, address, 4, GNU_PROT_EXEC);
    if ((address & 3) || !instruction) {
        *word = 0;
        gnu_process_fault(p, 11, "Guest instruction fetch violated mapped execution permissions");
        return;
    }
    /* Execute-only guest pages are host-readable instruction data. */
    memcpy(word, instruction, 4);
}

int gnu_jit_prepare(GnuJit* jit, const SnxExecRange* ranges, size_t count, uint64_t entry, char* error,
                    size_t error_size) {
    uint64_t a;
    if (jit->mode != GNU_AOT) return 0;
    if (snx_execution_compile(&jit->aot, ranges, count, entry, jit->cc, error, error_size)) return -1;
    jit->aot.fetch = aot_fetch;
    if (jit->aot.set_fetch) jit->aot.set_fetch(aot_fetch);
    for (a = jit->aot.code_lo; a < jit->aot.code_hi; a += 4) {
        GuestFn f = jit->aot.dispatch[(a - jit->aot.code_lo) / 4];
        if (f && a < GNU_ADDRESS_LIMIT) table_set(jit, a, f);
    }
    return 0;
}

void gnu_jit_bind(GnuJit* jit, CPU* cpu) {
    cpu->disp = jit->table;
    cpu->code_lo = 0;
    cpu->code_hi = GNU_ADDRESS_LIMIT;
}

void gnu_jit_call(GnuJit* jit, CPU* cpu, uint64_t pc) {
    GuestFn f = pc < GNU_ADDRESS_LIMIT && !(pc & 3) ? jit->table[pc >> 2] : NULL;
    (f ? f : gnu_jit_miss)(cpu, pc);
}

void gnu_jit_install(GnuJit* jit, uint64_t pc, GuestFn function) { table_set(jit, pc, function); }

int gnu_jit_changed(GnuJit* jit, uint64_t pc) {
    int changed = pc >= jit->changed_lo && pc < jit->changed_hi;
    jit->changed_lo = jit->changed_hi = 0;
    return changed;
}

void gnu_jit_invalidate(GnuJit* jit, uint64_t address, uint64_t length) {
    uint64_t first, last;
    if (!jit->table || !length || address >= GNU_ADDRESS_LIMIT) return;
    if (length > GNU_ADDRESS_LIMIT - address) length = GNU_ADDRESS_LIMIT - address;
    if (jit->changed_hi == jit->changed_lo || address < jit->changed_lo) jit->changed_lo = address;
    if (address + length > jit->changed_hi) jit->changed_hi = address + length;
    first = address >> 2;
    last = (address + length + 3) >> 2;
    table_clear(jit, first, last - first);
}
