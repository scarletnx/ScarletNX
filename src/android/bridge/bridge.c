/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native bridge's core (bridge.h). The JIT follows the GNU ABI runtime's (src/gnuabi/runtime/jit.c), with a lock:
   guest threads are host threads here, and one compiles at a time while the others that miss wait for it */
#define _GNU_SOURCE /* MAP_NORESERVE */
#include "android/bridge/bridge.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/utilities/common.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* The window: 2 GiB of address space for the guest's code and its exports, its last 16 MiB the exports' addresses.
   The dispatch table has a slot for each 4 bytes of it */
#define WINDOW_SIZE    ((uint64_t)2 << 30)
#define WINDOW_ADDRESS ((uint64_t)256 << 30)
#define EXPORTS_SIZE  ((uint64_t)16 << 20)
#define TABLE_BYTES   (WINDOW_SIZE / 4 * sizeof(GuestFn))
/* instructions one JIT step compiles: the function reached and the functions it calls, breadth first */
#define JIT_STEP      65536
/* a guest thread's stack */
#define STACK_SIZE    ((size_t)8 << 20)

typedef struct {
    char* name;
    BridgeHandler handler;
    void* data;
} Export;

typedef struct {
    uint64_t address;
    size_t size;
} CodeRange;

static struct {
    unsigned char* window; /* the reservation; a guest address is its host address */
    uint64_t lo, hi;       /* the window's guest addresses */
    uint64_t next;         /* the first free address for a library */
    GuestFn* table;
    struct snx_recompiler* recompiler;
    struct snx_recompiled** modules;
    size_t module_count, module_capacity;
    CodeRange* ranges;
    size_t range_count, range_capacity;
    Export* exports; /* by (address - exports' base) / 4 */
    size_t export_count;
    uint64_t exports_base;
    uint64_t stop; /* the address that ends a host call into guest code */
    uint64_t guard;
    RtAPI rt;
    pthread_mutex_t lock; /* the JIT, the exports and the ranges */
} b = {.lock = PTHREAD_MUTEX_INITIALIZER};

/* the state of a host thread that runs guest code */
typedef struct {
    CPU cpu;
    unsigned char* stack;
    /* bionic's TLS slots on arm64: TPIDR_EL0 points to slot 0, and slot -1 is bionic's own (tls_defines.h) */
    uint64_t tls[1 + 8];
} Thread;
static _Thread_local Thread* t_thread;

/* ---- the JIT ---- */

static int compiled(void* context, uint64_t address) {
    (void)context;
    return address >= b.lo && address < b.hi && __atomic_load_n(&b.table[(address - b.lo) >> 2], __ATOMIC_RELAXED);
}

static const CodeRange* range_of(uint64_t pc) {
    for (size_t i = 0; i < b.range_count; ++i)
        if (pc >= b.ranges[i].address && pc - b.ranges[i].address < b.ranges[i].size) return &b.ranges[i];
    return NULL;
}

/* the code at pc compiled, with the lock held: false when it can't be */
static bool compile(uint64_t pc) {
    const CodeRange* r = range_of(pc);
    if (!r) return false;
    /* the library's tables cover at most 1 GiB: the part of the range around pc */
    struct snx_code_range range = {r->address, (const void*)(uintptr_t)r->address, r->size};
    const size_t most = (size_t)1 << 30;
    if (range.size > most) {
        uint64_t start = pc - (pc - r->address) % (most / 2);
        range.address = start;
        range.code = (const void*)(uintptr_t)start;
        range.size = r->address + r->size - start < most ? (size_t)(r->address + r->size - start) : most;
    }
    double started = snx_seconds();
    struct snx_recompiled* m = snx_recompile_from(b.recompiler, &range, 1, &pc, 1, JIT_STEP, compiled, NULL);
    if (!m) {
        fprintf(stderr, "scarletnx-android: cannot compile the guest code at 0x%llx: %s\n", (unsigned long long)pc,
                snx_recompiler_error(b.recompiler));
        return false;
    }
    uint64_t lo, hi;
    snx_recompiled_range(m, &lo, &hi);
    for (uint64_t a = lo; a < hi; a += 4) {
        GuestFn f = snx_recompiled_function(m, a);
        if (f) __atomic_store_n(&b.table[(a - b.lo) >> 2], f, __ATOMIC_RELEASE);
    }
    if (b.module_count == b.module_capacity) {
        b.module_capacity = b.module_capacity ? b.module_capacity * 2 : 64;
        b.modules = snx_realloc(b.modules, b.module_capacity * sizeof *b.modules);
    }
    b.modules[b.module_count++] = m;
    if (getenv("SNX_ANDROID_JIT_VERBOSE"))
        fprintf(stderr, "scarletnx-android: JIT 0x%llx: 0x%llx-0x%llx, %.2f s\n", (unsigned long long)pc,
                (unsigned long long)lo, (unsigned long long)hi, snx_seconds() - started);
    return compiled(NULL, pc);
}

/* a guest address with no function in the table */
static void miss(CPU* c, u64 pc) {
    pthread_mutex_lock(&b.lock);
    bool ok = compiled(NULL, pc) || compile(pc);
    pthread_mutex_unlock(&b.lock);
    if (!ok) {
        fprintf(stderr, "scarletnx-android: the guest jumped to 0x%llx, which is no code (from 0x%llx)\n",
                (unsigned long long)pc, (unsigned long long)c->x[30]);
        abort();
    }
    b.table[(pc - b.lo) >> 2](c, pc);
}

static void undef(CPU* c, u64 pc, u32 word) {
    fprintf(stderr, "scarletnx-android: undefined instruction 0x%08x at 0x%llx\n", (unsigned)word,
            (unsigned long long)pc);
    (void)c;
    abort();
}
static void guest_brk(CPU* c, u64 pc, u32 imm) {
    fprintf(stderr, "scarletnx-android: BRK #%u at 0x%llx (an abort of the guest)\n", (unsigned)imm,
            (unsigned long long)pc);
    (void)c;
    abort();
}
/* a system call the guest makes itself, not through libc: none is answered yet */
static void svc(CPU* c, u32 imm) {
    fprintf(stderr, "scarletnx-android: system call %llu (svc #%u) at 0x%llx: not answered\n",
            (unsigned long long)c->x[8], (unsigned)imm, (unsigned long long)c->pc - 4);
    c->x[0] = (u64)-38; /* -ENOSYS */
}

/* ---- exports ---- */

static void call_export(CPU* c, u64 entry) {
    Export* e = &b.exports[(entry - b.exports_base) >> 2];
    e->handler(c, e->data);
    c->pc = c->x[30];
}

static void stop(CPU* c, u64 entry) {
    c->pc = entry;
}

uint64_t bridge_export(const char* name, BridgeHandler handler, void* data) {
    pthread_mutex_lock(&b.lock);
    for (size_t i = 0; i < b.export_count; ++i)
        if (!strcmp(b.exports[i].name, name)) {
            pthread_mutex_unlock(&b.lock);
            return b.exports_base + 4 * i;
        }
    size_t i = b.export_count;
    if ((i + 1) * 4 > EXPORTS_SIZE) {
        pthread_mutex_unlock(&b.lock);
        return 0;
    }
    b.exports = snx_realloc(b.exports, (i + 1) * sizeof *b.exports);
    size_t n = strlen(name) + 1;
    b.exports[i].name = snx_alloc(n);
    memcpy(b.exports[i].name, name, n);
    b.exports[i].handler = handler;
    b.exports[i].data = data;
    b.export_count = i + 1;
    uint64_t address = b.exports_base + 4 * i;
    __atomic_store_n(&b.table[(address - b.lo) >> 2], call_export, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&b.lock);
    return address;
}

const char* bridge_export_name(uint64_t address) {
    if (address < b.exports_base || address >= b.exports_base + 4 * b.export_count || (address & 3)) return NULL;
    return b.exports[(address - b.exports_base) >> 2].name;
}

/* ---- the window ---- */

bool bridge_init(const char* cache, char* error, size_t error_size) {
    if (b.window) return true;
    /* the window at the same address on every run: the compiled code's cache is keyed by the code's addresses */
    void* w = mmap((void*)(uintptr_t)WINDOW_ADDRESS, WINDOW_SIZE, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (w == MAP_FAILED) {
        fprintf(stderr, "scarletnx-android: the code window's address is taken: no compiled code is kept\n");
        w = mmap(NULL, WINDOW_SIZE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    }
    void* t = mmap(NULL, TABLE_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (w == MAP_FAILED || t == MAP_FAILED) {
        snprintf(error, error_size, "cannot reserve the native bridge's code window");
        return false;
    }
    b.window = w;
    b.table = t;
    b.lo = (uint64_t)(uintptr_t)w;
    b.hi = b.lo + WINDOW_SIZE;
    b.next = b.lo + 0x10000; /* address 0 of the window stays unmapped */
    b.exports_base = b.hi - EXPORTS_SIZE;
    b.recompiler = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!b.recompiler) {
        snprintf(error, error_size, "the LLVM backend cannot target this host");
        return false;
    }
    /* a phone's CPU: Armv8.2, without pointer authentication (getauxval's HWCAP says the same) */
    snx_recompiler_set_a64_target(b.recompiler, SNX_RECOMPILER_A64_ANDROID);
    if (cache && snx_recompiler_set_cache(b.recompiler, cache))
        fprintf(stderr, "scarletnx-android: %s; compiling without a cache\n", snx_recompiler_error(b.recompiler));
    snx_runtime_defaults(&b.rt);
    b.rt.svc = svc;
    b.rt.undef = undef;
    b.rt.brk = guest_brk;
    b.rt.miss = miss;
    /* the guest's stack guard: random, as the kernel's AT_RANDOM gives bionic, with a zero low byte as glibc's */
    FILE* f = fopen("/dev/urandom", "rb");
    if (!f || fread(&b.guard, sizeof b.guard, 1, f) != 1) b.guard = 0x5ca7105ca7100ull;
    if (f) fclose(f);
    b.guard &= ~(uint64_t)0xff;
    b.stop = bridge_export("<stop>", NULL, NULL);
    b.table[(b.stop - b.lo) >> 2] = stop;
    return true;
}

uint64_t bridge_window_reserve(size_t size, size_t align) {
    pthread_mutex_lock(&b.lock);
    uint64_t at = (b.next + align - 1) & ~(uint64_t)(align - 1);
    uint64_t address = at + size <= b.exports_base ? at : 0;
    if (address) b.next = at + size;
    pthread_mutex_unlock(&b.lock);
    return address;
}

void bridge_add_code(uint64_t address, size_t size) {
    pthread_mutex_lock(&b.lock);
    if (b.range_count == b.range_capacity) {
        b.range_capacity = b.range_capacity ? b.range_capacity * 2 : 32;
        b.ranges = snx_realloc(b.ranges, b.range_capacity * sizeof *b.ranges);
    }
    b.ranges[b.range_count++] = (CodeRange){address, size};
    pthread_mutex_unlock(&b.lock);
}

void bridge_map_code(uint64_t address, const void* code, size_t size) {
    size_t span = (size + 0xfff) & ~(size_t)0xfff;
    if (mmap((void*)(uintptr_t)address, span, PROT_READ | PROT_WRITE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) ==
        MAP_FAILED)
        return;
    memcpy((void*)(uintptr_t)address, code, size);
    mprotect((void*)(uintptr_t)address, span, PROT_READ);
    bridge_add_code(address, (size + 3) & ~(size_t)3);
}

uint64_t bridge_stack_guard(void) {
    return b.guard;
}

/* ---- threads and calls ---- */

CPU* bridge_cpu(void) {
    if (t_thread) return &t_thread->cpu;
    Thread* t = snx_alloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (t->stack == MAP_FAILED) {
        fprintf(stderr, "scarletnx-android: no memory for a guest stack\n");
        abort();
    }
    /* the lowest page a guard, as a thread's stack has */
    mprotect(t->stack, 4096, PROT_NONE);
    t->tls[1 + 5] = b.guard;          /* TLS_SLOT_STACK_GUARD */
    t->tls[1 + 1] = (uint64_t)gettid(); /* TLS_SLOT_THREAD_ID: bionic keeps its pthread there, read through its API */
    CPU* c = &t->cpu;
    c->mem = NULL;
    c->rt = &b.rt;
    c->disp = b.table;
    c->code_lo = b.lo;
    c->code_hi = b.hi;
    c->thread = t;
    c->tpidr = (uint64_t)(uintptr_t)&t->tls[1];
    c->x[31] = ((uint64_t)(uintptr_t)t->stack + STACK_SIZE) & ~(uint64_t)15;
    t_thread = t;
    return c;
}

void bridge_stack(uint64_t* base, size_t* size) {
    bridge_cpu();
    *base = (uint64_t)(uintptr_t)t_thread->stack;
    *size = STACK_SIZE;
}

void bridge_thread_end(void) {
    Thread* t = t_thread;
    if (!t) return;
    t_thread = NULL;
    munmap(t->stack, STACK_SIZE);
    free(t);
}

uint64_t bridge_call(uint64_t fn, const uint64_t* args, unsigned count) {
    CPU* c = bridge_cpu();
    /* the caller's guest state that the callee may change: a host function called from guest code calls back into
       it, and returns to its own caller after */
    uint64_t pc = c->pc, lr = c->x[30];
    for (unsigned i = 0; i < count && i < 8; ++i) c->x[i] = args[i];
    c->x[30] = b.stop;
    for (uint64_t at = fn; at != b.stop; at = c->pc) snx_lookup(c, at)(c, at);
    uint64_t result = c->x[0];
    c->pc = pc;
    c->x[30] = lr;
    return result;
}
