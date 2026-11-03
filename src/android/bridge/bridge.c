/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native bridge's core (bridge.h). The JIT follows the GNU ABI runtime's (src/gnuabi/runtime/jit.c), with a lock:
   guest threads are host threads here, and one compiles at a time while the others that miss wait for it */
#include "android/bridge/bridge.h"
#include "android/bridge/hle_impl.h"
#include "android/bridge/hle_signal.h"
#include "android/bridge/host.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/utilities/common.h"
#include "gnuabi/kernel/kernel.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The window: 2 GiB of address space for the guest's code and its exports, its last 16 MiB the exports' addresses.
   The dispatch table has a slot for each 4 bytes of it */
#define WINDOW_SIZE    ((uint64_t)2 << 30)
#define WINDOW_ADDRESS ((uint64_t)256 << 30)
#define EXPORTS_SIZE  ((uint64_t)16 << 20)
#define TABLE_BYTES   (WINDOW_SIZE / 4 * sizeof(GuestFn))
/* One JIT step: the function reached and the functions it calls, breadth first, inside the aligned block of the
   code window that holds it; a call out of the block goes through the dispatch table. A step depends on its entry
   alone, never on what the process compiled before (which depends on which thread reached what first), so the same
   entry makes the same unit in every run and the cache finds it. A later miss in the same block compiles the block's
   functions it reaches again, at most a block's worth */
#define JIT_BLOCK     ((uint64_t)64 << 10)
#define JIT_STEP      65536
/* a guest thread's stack */
#define STACK_SIZE    ((size_t)8 << 20)

typedef struct {
    char* name;
    BridgeHandler handler;
    void* data;
    bool blocks; /* a call that can wait: a signal runs its handler during the wait (hle_signal.h) */
} Export;
/* the most exports: one for each 4 bytes of their addresses */
#define EXPORTS_MAX (EXPORTS_SIZE / 4)

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
    /* by (address - exports' base) / 4: reserved whole, so that a call reads it while another thread adds one */
    Export* exports;
    size_t export_count;
    uint64_t exports_base;
    uint64_t stop;      /* the address that ends a host call into guest code */
    uint64_t sigreturn; /* the address a guest signal handler returns to */
    uint64_t guard;
    char optimization; /* bridge_set_optimization's, 0 for the default */
    bool verbose;      /* bridge_set_verbose's */
    RtAPI rt;
    pthread_mutex_t lock; /* the JIT, the exports and the ranges */
} b = {.lock = PTHREAD_MUTEX_INITIALIZER};

/* the state of a host thread that runs guest code */
typedef struct {
    CPU cpu;
    unsigned char* stack;
    GuestSignals signals;
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
typedef struct {
    uint64_t lo, hi;
} Block;
static int outside_block(void* context, uint64_t address) {
    const Block* block = context;
    return address < block->lo || address >= block->hi;
}

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
    Block block = {pc & ~(JIT_BLOCK - 1), (pc & ~(JIT_BLOCK - 1)) + JIT_BLOCK};
    struct snx_recompiled* m = snx_recompile_from(b.recompiler, &range, 1, &pc, 1, JIT_STEP, outside_block, &block);
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
    if (b.verbose)
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
    signals_in_export(entry);
    hle_errno_begin();
    if (e->blocks) {
        int parked = signals_park(c);
        e->handler(c, e->data);
        signals_unpark(parked);
    } else
        e->handler(c, e->data);
    hle_errno_end();
    signals_in_export(0);
    c->pc = c->x[30];
    /* a signal the call raised or unblocked, or one that came during it, before the caller goes on */
    signals_check(c);
}

/* the end of a run of guest code: <stop> for a host call, <sigreturn> for a signal handler */
static void stop(CPU* c, u64 entry) {
    c->pc = entry;
}

/* generated code reached a poll: a guest signal, the only reason the bridge sets CPU.poll */
static void poll(CPU* c) {
    __atomic_store_n(&c->poll, 0, __ATOMIC_RELAXED);
    signals_check(c);
}

uint64_t bridge_export(const char* name, BridgeHandler handler, void* data) {
    pthread_mutex_lock(&b.lock);
    for (size_t i = 0; i < b.export_count; ++i)
        if (!strcmp(b.exports[i].name, name)) {
            pthread_mutex_unlock(&b.lock);
            return b.exports_base + 4 * i;
        }
    size_t i = b.export_count;
    if (i + 1 > EXPORTS_MAX) {
        pthread_mutex_unlock(&b.lock);
        return 0;
    }
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

void bridge_export_blocks(uint64_t address) {
    if (address >= b.exports_base && address < b.exports_base + 4 * b.export_count && !(address & 3))
        b.exports[(address - b.exports_base) >> 2].blocks = true;
}

const char* bridge_export_name(uint64_t address) {
    if (address < b.exports_base || address >= b.exports_base + 4 * b.export_count || (address & 3)) return NULL;
    return b.exports[(address - b.exports_base) >> 2].name;
}

/* ---- the window ---- */

bool bridge_set_optimization(char level) {
    if (!strchr("0123sz", level) || !level) return false;
    b.optimization = level;
    return true;
}

void bridge_set_verbose(bool verbose) {
    b.verbose = verbose;
}

bool bridge_init(const char* cache, char* error, size_t error_size) {
    if (b.window) return true;
    /* the window at the same address on every run: the compiled code's cache is keyed by the code's addresses */
    void* w = host_reserve(WINDOW_ADDRESS, WINDOW_SIZE, true);
    if (!w) {
        fprintf(stderr, "scarletnx-android: the code window's address is taken: no compiled code is kept\n");
        w = host_reserve(0, WINDOW_SIZE, false);
    }
    void* t = host_lazy(TABLE_BYTES);
    void* e = host_lazy(EXPORTS_MAX * sizeof(Export));
    if (!w || !t || !e) {
        snprintf(error, error_size, "cannot reserve the native bridge's code window");
        return false;
    }
    b.window = w;
    b.table = t;
    b.exports = e;
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
    if (b.optimization) snx_recompiler_set_optimization(b.recompiler, b.optimization);
    if (cache && snx_recompiler_set_cache(b.recompiler, cache))
        fprintf(stderr, "scarletnx-android: %s; compiling without a cache\n", snx_recompiler_error(b.recompiler));
    snx_runtime_defaults(&b.rt);
    b.rt.svc = svc;
    b.rt.undef = undef;
    b.rt.brk = guest_brk;
    b.rt.miss = miss;
    b.rt.poll = poll;
    /* the guest's stack guard: random, as the kernel's AT_RANDOM gives bionic, with a zero low byte as glibc's */
    if (gnu_random_bytes(&b.guard, sizeof b.guard)) b.guard = 0x5ca7105ca7100ull;
    b.guard &= ~(uint64_t)0xff;
    b.stop = bridge_export("<stop>", NULL, NULL);
    b.table[(b.stop - b.lo) >> 2] = stop;
    b.sigreturn = bridge_export("<sigreturn>", NULL, NULL);
    b.table[(b.sigreturn - b.lo) >> 2] = stop;
    signals_init();
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
    if (!host_commit((void*)(uintptr_t)address, span)) return;
    memcpy((void*)(uintptr_t)address, code, size);
    host_protect((void*)(uintptr_t)address, span, HOST_READ);
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
    t->stack = host_lazy(STACK_SIZE);
    if (!t->stack) {
        fprintf(stderr, "scarletnx-android: no memory for a guest stack\n");
        abort();
    }
    /* the lowest page a guard, as a thread's stack has */
    host_protect(t->stack, 4096, HOST_NONE);
    t->tls[1 + 5] = b.guard;          /* TLS_SLOT_STACK_GUARD */
    t->tls[1 + 1] = host_tid(); /* TLS_SLOT_THREAD_ID: bionic keeps its pthread there, read through its API */
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
    signals_thread_start(&t->signals, c);
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
    signals_thread_end(&t->signals);
    t_thread = NULL;
    host_unmap(t->stack, STACK_SIZE);
    free(t);
}

uint64_t bridge_call(uint64_t fn, const uint64_t* args, unsigned count) {
    CPU* c = bridge_cpu();
    /* the caller's guest state that the callee may change: a host function called from guest code calls back into
       it, and returns to its own caller after */
    uint64_t pc = c->pc, lr = c->x[30];
    for (unsigned i = 0; i < count && i < 8; ++i) c->x[i] = args[i];
    c->x[30] = b.stop;
    signals_enter();
    for (uint64_t at = fn; at != b.stop; at = c->pc) snx_lookup(c, at)(c, at);
    signals_leave();
    uint64_t result = c->x[0];
    c->pc = pc;
    c->x[30] = lr;
    return result;
}

uint64_t bridge_sigreturn(void) {
    return b.sigreturn;
}

bool bridge_run_handler(CPU* c) {
    for (uint64_t at = c->pc;; at = c->pc) {
        if (at == b.sigreturn) return true;
        if (at == b.stop) return false;
        snx_lookup(c, at)(c, at);
    }
}

void bridge_prepare(uint64_t address) {
    if (address < b.lo || address >= b.exports_base) return;
    pthread_mutex_lock(&b.lock);
    if (!compiled(NULL, address)) compile(address);
    pthread_mutex_unlock(&b.lock);
}
