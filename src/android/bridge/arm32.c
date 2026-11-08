/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 32-bit native bridge's core (arm32.h): the guest's window, its heap, each thread's CPU state, the dispatch loop
   that runs the block compiler's blocks and the host handlers of the imports, calls from the host into the guest, and
   the loader of ELF32 ARM libraries */
#define _GNU_SOURCE
#include "android/bridge/arm32.h"
#include "android/bridge/hle_impl.h"
#include "common/cpu/arm/exec.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

uint8_t* arm32_mem;

/* the window's layout. The imports' addresses are above the heap, each 8 bytes apart; the page of Linux's kernel
   helpers (__kuser_cmpxchg and the others, which ARMv5 code calls for its atomics) is at 0xffff0000 */
#define WINDOW_SIZE  (UINT64_C(1) << 32)
#define NULL_SIZE    0x10000u
#define HEAP_LO      0x10000u
#define HEAP_HI      0xfe000000u
#define EXPORT_BASE  0xfff00000u
#define EXPORT_LIMIT 0xffff0000u
#define KUSER_PAGE   0xffff0000u
#define STOP_ADDRESS 0xfffff000u
#define STACK_SIZE   (8u << 20)
#define PAGE         4096u

/* ---- the state ---- */

typedef struct {
    char* name;
    Arm32Handler handler;
    void* data;
} Export;

/* a range of code, with the blocks compiled for each address of it */
typedef struct {
    uint32_t lo, hi;
    SnxArmBlock* arm;   /* one for each word */
    SnxArmBlock* thumb; /* one for each halfword */
} CodeRange;
enum { MAX_RANGES = 64 };

static struct {
    bool active;
    pthread_mutex_t lock; /* the exports, the compiler, the ranges, the libraries */
    Export* exports;
    uint32_t export_count, export_capacity;
    SnxArmExec* exec;
    CodeRange ranges[MAX_RANGES];
    SnxArmExecRange exec_ranges[MAX_RANGES];
    unsigned range_count;
} s = {.lock = PTHREAD_MUTEX_INITIALIZER};

typedef struct {
    SnxArmCPU cpu;
    uint32_t stack, stack_size;
    uint32_t tls; /* 64 slots: 0 itself, 1 the thread's id, 2 errno */
} Thread;
static _Thread_local Thread* t_thread;
/* the signals sent to this thread and not run yet (arm32_thread.c): the dispatch loop runs them between blocks */
_Thread_local uint32_t arm32_pending_signals;

bool arm32_active(void) { return s.active; }

/* ---- the heap ---- */

/* The page allocator: the free ranges of the window, by address. The small allocator takes slabs from it */
typedef struct {
    uint32_t start, size;
} Range;
static struct {
    pthread_mutex_t lock;
    Range* free;
    size_t count, capacity;
    uint32_t lists[256]; /* each small class's free blocks, linked through their first word */
} h = {.lock = PTHREAD_MUTEX_INITIALIZER};

static uint32_t rd32(uint32_t a) { return snx_ld_le32(arm32_mem + a); }
static void wr32(uint32_t a, uint32_t v) { snx_st_le32(arm32_mem + a, v); }

static uint32_t pages_locked(uint32_t size) {
    size = (uint32_t)snx_align_up(size, PAGE);
    for (size_t i = 0; i < h.count; ++i)
        if (h.free[i].size >= size) {
            uint32_t a = h.free[i].start;
            h.free[i].start += size;
            h.free[i].size -= size;
            if (!h.free[i].size) {
                memmove(&h.free[i], &h.free[i + 1], (h.count - i - 1) * sizeof *h.free);
                --h.count;
            }
            return a;
        }
    return 0;
}
static void unpages_locked(uint32_t start, uint32_t size) {
    size = (uint32_t)snx_align_up(size, PAGE);
    size_t i = 0;
    while (i < h.count && h.free[i].start < start) ++i;
    /* merged with the range before it, the one after it, or both */
    if (i > 0 && h.free[i - 1].start + h.free[i - 1].size == start) {
        h.free[i - 1].size += size;
        if (i < h.count && start + size == h.free[i].start) {
            h.free[i - 1].size += h.free[i].size;
            memmove(&h.free[i], &h.free[i + 1], (h.count - i - 1) * sizeof *h.free);
            --h.count;
        }
        return;
    }
    if (i < h.count && start + size == h.free[i].start) {
        h.free[i].start = start;
        h.free[i].size += size;
        return;
    }
    if (h.count == h.capacity) {
        size_t n = h.capacity ? h.capacity * 2 : 256;
        Range* r = realloc(h.free, n * sizeof *r);
        if (!r) return; /* the pages are lost, not reused */
        h.free = r;
        h.capacity = n;
    }
    memmove(&h.free[i + 1], &h.free[i], (h.count - i) * sizeof *h.free);
    h.free[i].start = start;
    h.free[i].size = size;
    ++h.count;
}

uint32_t arm32_map(uint32_t size) {
    if (!size || size > HEAP_HI) return 0;
    pthread_mutex_lock(&h.lock);
    uint32_t a = pages_locked(size);
    pthread_mutex_unlock(&h.lock);
    if (a) memset(arm32_mem + a, 0, snx_align_up(size, PAGE));
    return a;
}
bool arm32_map_at(uint32_t address, uint32_t size) {
    if (address < HEAP_LO || address & (PAGE - 1) || !size || (uint64_t)address + size > HEAP_HI) return false;
    uint32_t end = (uint32_t)snx_align_up((uint64_t)address + size, PAGE);
    pthread_mutex_lock(&h.lock);
    /* the free ranges that the pages cut, kept on each side */
    for (size_t i = 0; i < h.count; ++i) {
        uint32_t lo = h.free[i].start, hi = h.free[i].start + h.free[i].size;
        if (hi <= address || lo >= end) continue;
        if (lo < address && hi > end) {
            h.free[i].size = address - lo;
            pthread_mutex_unlock(&h.lock);
            arm32_unmap(end, hi - end);
            pthread_mutex_lock(&h.lock);
            break;
        }
        if (lo < address) {
            h.free[i].size = address - lo;
        } else if (hi > end) {
            h.free[i].start = end;
            h.free[i].size = hi - end;
        } else {
            memmove(&h.free[i], &h.free[i + 1], (h.count - i - 1) * sizeof *h.free);
            --h.count;
            --i;
        }
    }
    pthread_mutex_unlock(&h.lock);
    memset(arm32_mem + address, 0, end - address);
    return true;
}

void arm32_unmap(uint32_t address, uint32_t size) {
    if (!address || !size) return;
    pthread_mutex_lock(&h.lock);
    unpages_locked(address & ~(PAGE - 1), (uint32_t)snx_align_up(size + (address & (PAGE - 1)), PAGE));
    pthread_mutex_unlock(&h.lock);
}

/* A block has an 8-byte header before what malloc gives: its usable size, then its kind. A small block is of one of
   256 classes of 16 bytes to 4 KiB (with its header), carved from slabs; a large one is whole pages; an aligned one
   (memalign's) is inside another block, whose address is the word before its header */
#define KIND_SMALL   0x534d0000u
#define KIND_LARGE   0x4c415247u
#define KIND_ALIGNED 0x414c474eu
#define SLAB         0x10000u

uint32_t arm32_malloc(uint32_t size) {
    if (size > HEAP_HI) return 0;
    uint32_t total = (uint32_t)snx_align_up((uint64_t)size + 8, 16);
    if (total <= 4096) {
        unsigned cls = total / 16 - 1;
        pthread_mutex_lock(&h.lock);
        if (!h.lists[cls]) {
            uint32_t slab = pages_locked(SLAB);
            if (!slab) {
                pthread_mutex_unlock(&h.lock);
                return 0;
            }
            /* the slab's blocks linked in address order */
            uint32_t n = SLAB / total;
            for (uint32_t i = 0; i < n; ++i) wr32(slab + i * total + 8, i + 1 < n ? slab + (i + 1) * total + 8 : 0);
            h.lists[cls] = slab + 8;
        }
        uint32_t p = h.lists[cls];
        h.lists[cls] = rd32(p);
        pthread_mutex_unlock(&h.lock);
        wr32(p - 8, total - 8);
        wr32(p - 4, KIND_SMALL | cls);
        return p;
    }
    pthread_mutex_lock(&h.lock);
    uint32_t a = pages_locked(total);
    pthread_mutex_unlock(&h.lock);
    if (!a) return 0;
    wr32(a, (uint32_t)snx_align_up(total, PAGE) - 8);
    wr32(a + 4, KIND_LARGE);
    return a + 8;
}

/* the usable size of a block, 0 for an address that is no block */
static uint32_t usable(uint32_t p) {
    if (p < HEAP_LO + 8 || p >= HEAP_HI) return 0;
    uint32_t kind = rd32(p - 4);
    if ((kind & 0xffff0000u) == KIND_SMALL || kind == KIND_LARGE || kind == KIND_ALIGNED) return rd32(p - 8);
    return 0;
}

void arm32_free(uint32_t p) {
    if (!p) return;
    uint32_t kind = rd32(p - 4);
    if (kind == KIND_ALIGNED) {
        arm32_free(rd32(p - 12));
        return;
    }
    if ((kind & 0xffff0000u) == KIND_SMALL && (kind & 0xffff) < 256) {
        unsigned cls = kind & 0xff;
        pthread_mutex_lock(&h.lock);
        wr32(p, h.lists[cls]);
        h.lists[cls] = p;
        pthread_mutex_unlock(&h.lock);
        return;
    }
    if (kind == KIND_LARGE) {
        uint32_t size = rd32(p - 8) + 8;
        wr32(p - 4, 0);
        pthread_mutex_lock(&h.lock);
        unpages_locked(p - 8, size);
        pthread_mutex_unlock(&h.lock);
        return;
    }
    fprintf(stderr, "scarletnx-android: arm32: free(0x%08x) of no block, ignored\n", p);
}

uint32_t arm32_memalign(uint32_t align, uint32_t size) {
    if (align <= 8) return arm32_malloc(size);
    if (align & (align - 1)) return 0;
    uint32_t raw = arm32_malloc(size + align + 16);
    if (!raw) return 0;
    uint32_t p = (uint32_t)snx_align_up((uint64_t)raw + 16, align);
    wr32(p - 12, raw);
    wr32(p - 8, size);
    wr32(p - 4, KIND_ALIGNED);
    return p;
}

uint32_t arm32_realloc(uint32_t p, uint32_t size) {
    if (!p) return arm32_malloc(size);
    if (!size) {
        arm32_free(p);
        return 0;
    }
    uint32_t old = usable(p);
    if (old >= size && rd32(p - 4) != KIND_ALIGNED) return p;
    uint32_t n = arm32_malloc(size);
    if (!n) return 0;
    memcpy(arm32_mem + n, arm32_mem + p, old < size ? old : size);
    arm32_free(p);
    return n;
}

uint32_t arm32_strdup(const char* text) {
    size_t n = strlen(text) + 1;
    uint32_t p = arm32_malloc((uint32_t)n);
    if (p) memcpy(arm32_mem + p, text, n);
    return p;
}

/* ---- exports ---- */

uint32_t arm32_export(const char* name, Arm32Handler handler, void* data) {
    pthread_mutex_lock(&s.lock);
    for (uint32_t i = 0; i < s.export_count; ++i)
        if (!strcmp(s.exports[i].name, name)) {
            pthread_mutex_unlock(&s.lock);
            return EXPORT_BASE + 8 * i;
        }
    uint32_t a = 0;
    if (EXPORT_BASE + 8 * (s.export_count + 1) < EXPORT_LIMIT) {
        if (s.export_count == s.export_capacity) {
            uint32_t n = s.export_capacity ? s.export_capacity * 2 : 1024;
            Export* e = realloc(s.exports, n * sizeof *e);
            if (e) {
                s.exports = e;
                s.export_capacity = n;
            }
        }
        char* copy = strdup(name);
        if (s.export_count < s.export_capacity && copy) {
            s.exports[s.export_count] = (Export){copy, handler, data};
            a = EXPORT_BASE + 8 * s.export_count++;
        } else {
            free(copy);
        }
    }
    pthread_mutex_unlock(&s.lock);
    return a;
}

/* ---- threads ---- */

SnxArmCPU* arm32_cpu(void) { return arm32_cpu_with_stack(STACK_SIZE); }

SnxArmCPU* arm32_cpu_with_stack(uint32_t stack_size) {
    if (t_thread) return &t_thread->cpu;
    Thread* t = calloc(1, sizeof *t);
    stack_size = (uint32_t)snx_align_up(stack_size < 0x10000 ? 0x10000 : stack_size, PAGE);
    uint32_t stack = arm32_map(stack_size);
    uint32_t tls = arm32_map(PAGE);
    if (!t || !stack || !tls) {
        fprintf(stderr, "scarletnx-android: arm32: no memory for a thread's guest stack\n");
        abort();
    }
    t->stack = stack;
    t->stack_size = stack_size;
    t->tls = tls;
    static uint32_t next_id = 1000;
    wr32(tls, tls);
    wr32(tls + 4, __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED));
    t->cpu.cpsr = 0x10;
    t->cpu.mem = arm32_mem;
    t->cpu.mem_size = WINDOW_SIZE;
    t->cpu.r[13] = stack + stack_size - 16;
    t->cpu.tpidruro = tls;
    t_thread = t;
    return &t->cpu;
}

uint32_t arm32_errno_address(void) {
    arm32_cpu();
    return t_thread->tls + 8;
}
uint32_t arm32_thread_self(void) {
    arm32_cpu();
    return t_thread->tls;
}
uint32_t arm32_thread_id(void) {
    arm32_cpu();
    return rd32(t_thread->tls + 4);
}
void arm32_thread_stack(uint32_t* base, uint32_t* size) {
    arm32_cpu();
    *base = t_thread->stack;
    *size = t_thread->stack_size;
}
void arm32_thread_end(void) {
    Thread* t = t_thread;
    if (!t) return;
    t_thread = NULL;
    arm32_unmap(t->stack, t->stack_size);
    arm32_unmap(t->tls, PAGE);
    free(t);
}

/* ---- the dispatch loop ---- */

static _Noreturn void guest_fault(const SnxArmCPU* c, const char* what) {
    fprintf(stderr, "scarletnx-android: arm32: %s at pc 0x%08x (%s, exit %u, syndrome 0x%08x)\n", what, c->r[15],
            c->cpsr & SNX_ARM_T ? "Thumb" : "A32", c->exit, c->syndrome);
    const Arm32Library* l = arm32_owner(c->r[15]);
    if (l) fprintf(stderr, "  in %s\n", arm32_library_name(l));
    for (unsigned i = 0; i < 16; i += 4)
        fprintf(stderr, "  r%-2u %08x  r%-2u %08x  r%-2u %08x  r%-2u %08x\n", i, c->r[i], i + 1, c->r[i + 1], i + 2,
                c->r[i + 2], i + 3, c->r[i + 3]);
    abort();
}

/* Code outside the libraries (a JIT's, as Mono's): blocks compiled from where they are, kept by the 4 KiB page they
   start in, and dropped when the guest says the code changed (the cacheflush system call). A thread's cache of
   them is good while the generation is the one it was filled at */
typedef struct {
    uint32_t key, end; /* key: pc | thumb */
    SnxArmBlock fn;
} DynBlock;
typedef struct DynPage {
    uint32_t page;
    DynBlock* blocks;
    uint32_t count, capacity;
    struct DynPage* next;
} DynPage;
enum { DYN_BUCKETS = 4096, DYN_MAX_BYTES = 1024, THREAD_CACHE = 1024 };
static struct {
    DynPage* buckets[DYN_BUCKETS];
    uint32_t generation;
} dyn;
static _Thread_local struct {
    uint32_t key, generation;
    SnxArmBlock fn;
} t_dyn[THREAD_CACHE];

static DynPage* dyn_page(uint32_t page, bool make) {
    DynPage** at = &dyn.buckets[(page * 2654435761u >> 20) % DYN_BUCKETS];
    for (DynPage* p = *at; p; p = p->next)
        if (p->page == page) return p;
    if (!make) return NULL;
    DynPage* p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->page = page;
    p->next = *at;
    *at = p;
    return p;
}

static SnxArmBlock dyn_block(uint32_t pc, int thumb) {
    uint32_t key = pc | (uint32_t)thumb, slot = (key >> 1) % THREAD_CACHE;
    uint32_t generation = __atomic_load_n(&dyn.generation, __ATOMIC_ACQUIRE);
    if (t_dyn[slot].key == key && t_dyn[slot].generation == generation && t_dyn[slot].fn) return t_dyn[slot].fn;
    pthread_mutex_lock(&s.lock);
    SnxArmBlock fn = NULL;
    DynPage* p = dyn_page(pc >> 12, true);
    for (uint32_t i = 0; p && i < p->count && !fn; ++i)
        if (p->blocks[i].key == key) fn = p->blocks[i].fn;
    if (!fn && p && pc >= HEAP_LO && pc < HEAP_HI) {
        size_t consumed = 0, unit = thumb ? 2 : 4, count = DYN_MAX_BYTES / unit;
        if (pc + DYN_MAX_BYTES > HEAP_HI) count = (HEAP_HI - pc) / unit;
        fn = snx_arm_exec_compile(s.exec, arm32_mem + pc, count, pc, thumb, SNX_ARM_PROFILE_A57, 0, &consumed);
        if (fn && p->count == p->capacity) {
            uint32_t n = p->capacity ? p->capacity * 2 : 8;
            DynBlock* b = realloc(p->blocks, n * sizeof *b);
            if (b) {
                p->blocks = b;
                p->capacity = n;
            }
        }
        if (fn && p->count < p->capacity) p->blocks[p->count++] = (DynBlock){key, pc + (uint32_t)(consumed * unit), fn};
    }
    generation = dyn.generation;
    pthread_mutex_unlock(&s.lock);
    if (fn) {
        t_dyn[slot].key = key;
        t_dyn[slot].generation = generation;
        t_dyn[slot].fn = fn;
    }
    return fn;
}

void arm32_code_changed(uint32_t lo, uint32_t hi) {
    if (hi <= lo) return;
    pthread_mutex_lock(&s.lock);
    uint32_t first = lo > DYN_MAX_BYTES ? (lo - DYN_MAX_BYTES) >> 12 : 0, last = (hi - 1) >> 12;
    for (uint32_t page = first; page <= last; ++page) {
        DynPage* p = dyn_page(page, false);
        for (uint32_t i = 0; p && i < p->count;)
            if (p->blocks[i].end > lo && (p->blocks[i].key & ~1u) < hi) p->blocks[i] = p->blocks[--p->count];
            else ++i;
    }
    __atomic_add_fetch(&dyn.generation, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s.lock);
}

static SnxArmBlock block_at(uint32_t pc, int thumb) {
    unsigned n = __atomic_load_n(&s.range_count, __ATOMIC_ACQUIRE);
    SnxArmBlock* slot = NULL;
    for (unsigned i = 0; i < n; ++i) {
        CodeRange* r = &s.ranges[i];
        if (pc >= r->lo && pc < r->hi) {
            slot = thumb ? &r->thumb[(pc - r->lo) >> 1] : &r->arm[(pc - r->lo) >> 2];
            SnxArmBlock b = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
            if (b) return b;
            break;
        }
    }
    if (!slot) return dyn_block(pc, thumb);
    pthread_mutex_lock(&s.lock);
    SnxArmBlock b = snx_arm_exec_block(s.exec, s.exec_ranges, s.range_count, pc, thumb, arm32_mem);
    pthread_mutex_unlock(&s.lock);
    if (b && slot) __atomic_store_n(slot, b, __ATOMIC_RELEASE);
    return b;
}

/* the return of a host handler, or of a kernel helper, to lr */
static void return_to_lr(SnxArmCPU* c) {
    uint32_t lr = c->r[14];
    c->r[15] = lr & ~1u;
    if (lr & 1) c->cpsr |= SNX_ARM_T;
    else c->cpsr &= ~SNX_ARM_T;
}

/* Linux's kernel helpers at the top of the window (Documentation/arch/arm/kernel_user_helpers.rst) */
static void kuser(SnxArmCPU* c) {
    switch (c->r[15]) {
    case 0xffff0fa0: /* __kuser_memory_barrier */ __atomic_thread_fence(__ATOMIC_SEQ_CST); break;
    case 0xffff0fc0: { /* __kuser_cmpxchg(oldval, newval, ptr): 0 and C set when *ptr was oldval and is newval now */
        uint32_t expected = c->r[0];
        bool ok = __atomic_compare_exchange_n((uint32_t*)(arm32_mem + c->r[2]), &expected, c->r[1], false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        c->r[0] = ok ? 0 : 1;
        if (ok) c->cpsr |= 1u << 29;
        else c->cpsr &= ~(1u << 29);
        break;
    }
    case 0xffff0fe0: /* __kuser_get_tls */ c->r[0] = c->tpidruro; break;
    case 0xffff0f60: { /* __kuser_cmpxchg64(oldval*, newval*, ptr) */
        uint64_t expected, desired;
        memcpy(&expected, arm32_mem + c->r[0], 8);
        memcpy(&desired, arm32_mem + c->r[1], 8);
        bool ok = __atomic_compare_exchange_n((uint64_t*)(arm32_mem + c->r[2]), &expected, desired, false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        c->r[0] = ok ? 0 : 1;
        if (ok) c->cpsr |= 1u << 29;
        else c->cpsr &= ~(1u << 29);
        break;
    }
    default: guest_fault(c, "a call into the kernel helpers' page");
    }
    return_to_lr(c);
}

/* runs c until it returns to STOP_ADDRESS */
static void run(SnxArmCPU* c) {
    for (;;) {
        uint32_t pc = c->r[15];
        if (pc >= EXPORT_BASE) {
            if (pc == STOP_ADDRESS) return;
            if (pc >= KUSER_PAGE) {
                kuser(c);
                continue;
            }
            uint32_t i = (pc - EXPORT_BASE) / 8;
            pthread_mutex_lock(&s.lock);
            Export e = i < s.export_count ? s.exports[i] : (Export){0};
            pthread_mutex_unlock(&s.lock);
            if (!e.handler) guest_fault(c, "a call to no import");
            e.handler(c, e.data);
            /* a handler that set the pc (sigsetjmp's, which goes on in guest code) does not return */
            if (c->r[15] == pc) return_to_lr(c);
            continue;
        }
        if (__atomic_load_n(&arm32_pending_signals, __ATOMIC_RELAXED)) {
            arm32_signals_run(c);
            continue;
        }
        int thumb = (c->cpsr & SNX_ARM_T) != 0;
        SnxArmBlock b = block_at(pc, thumb);
        if (!b) guest_fault(c, "code that does not compile");
        c->exit = SNX_ARM_OK;
        b(c);
        if (c->exit == SNX_ARM_OK) continue;
        if (c->exit == SNX_ARM_SVC) {
            /* Linux's EABI: the number in r7, the arguments in r0 to r5, the result or -errno in r0 */
            uint32_t args[6] = {c->r[0], c->r[1], c->r[2], c->r[3], c->r[4], c->r[5]};
            c->r[0] = arm32_syscall(c->r[7], args);
            continue;
        }
        guest_fault(c, c->exit == SNX_ARM_UNDEFINED     ? "an undefined instruction"
                       : c->exit == SNX_ARM_DATA_ABORT  ? "a data abort"
                       : c->exit == SNX_ARM_BREAKPOINT  ? "a breakpoint"
                       : c->exit == SNX_ARM_ALIGNMENT   ? "an unaligned access"
                                                        : "an instruction the block compiler stops at");
    }
}

/* The guest runs in the C locale (hle_locale_enter, as the 64-bit bridge's calls into guest code do): the host's C
   library that its imports call would take the user's, which the JVM set for the process, with a decimal comma and
   other names of days and months */
uint32_t arm32_call(uint32_t fn, const uint32_t* words, unsigned count, uint32_t* high) {
    hle_locale_enter();
    SnxArmCPU* c = arm32_cpu();
    uint32_t saved[16], saved_cpsr = c->cpsr;
    memcpy(saved, c->r, sizeof saved);
    for (unsigned i = 0; i < 4; ++i) c->r[i] = i < count ? words[i] : 0;
    uint32_t sp = c->r[13];
    if (count > 4) {
        sp = (sp - 4 * (count - 4)) & ~7u;
        for (unsigned i = 4; i < count; ++i) wr32(sp + 4 * (i - 4), words[i]);
    } else {
        sp &= ~7u;
    }
    c->r[13] = sp;
    c->r[14] = STOP_ADDRESS;
    c->r[15] = fn & ~1u;
    if (fn & 1) c->cpsr |= SNX_ARM_T;
    else c->cpsr &= ~SNX_ARM_T;
    run(c);
    uint32_t r0 = c->r[0];
    if (high) *high = c->r[1];
    memcpy(c->r, saved, sizeof saved);
    c->cpsr = saved_cpsr;
    hle_locale_leave();
    return r0;
}

/* ---- the window ---- */

bool arm32_init(char* error, size_t error_size) {
    if (s.active) return true;
    int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_NORESERVE
    flags |= MAP_NORESERVE;
#endif
    void* m = mmap(NULL, WINDOW_SIZE, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (m == MAP_FAILED) {
        snprintf(error, error_size, "no 4 GiB window for the 32-bit guest");
        return false;
    }
    arm32_mem = m;
    mprotect(m, NULL_SIZE, PROT_NONE);
    /* __kuser_helper_version: 3 helpers' sets (cmpxchg64 is version 5's) */
    wr32(KUSER_PAGE + 0xffc, 5);
    h.free = malloc(256 * sizeof *h.free);
    if (!h.free) {
        snprintf(error, error_size, "out of memory");
        return false;
    }
    h.capacity = 256;
    h.free[0] = (Range){HEAP_LO, HEAP_HI - HEAP_LO};
    h.count = 1;
    s.exec = snx_arm_exec_new(error, error_size);
    if (!s.exec) return false;
    s.active = true;
    arm32_hle_init();
    return true;
}

/* code at [lo, hi) of a library, compiled when first reached */
static void add_code(uint32_t lo, uint32_t hi) {
    pthread_mutex_lock(&s.lock);
    if (s.range_count < MAX_RANGES) {
        CodeRange* r = &s.ranges[s.range_count];
        r->lo = lo & ~3u;
        r->hi = (uint32_t)snx_align_up(hi, 4);
        r->arm = calloc((r->hi - r->lo) / 4, sizeof *r->arm);
        r->thumb = calloc((r->hi - r->lo) / 2, sizeof *r->thumb);
        s.exec_ranges[s.range_count] = (SnxArmExecRange){r->lo, arm32_mem + r->lo, r->hi - r->lo};
        if (r->arm && r->thumb) __atomic_store_n(&s.range_count, s.range_count + 1, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&s.lock);
}

uint32_t arm32_guest_code(const uint32_t* words, unsigned count) {
    uint32_t a = arm32_map(count * 4);
    if (!a) return 0;
    for (unsigned i = 0; i < count; ++i) wr32(a + 4 * i, words[i]);
    add_code(a, a + count * 4);
    return a;
}

/* ---- the loader ---- */

#define PT_LOAD_      1
#define PT_DYNAMIC_   2
#define PT_ARM_EXIDX_ 0x70000001u
#define PF_X_         1
enum {
    DT_NULL_ = 0,
    DT_NEEDED_ = 1,
    DT_PLTRELSZ_ = 2,
    DT_HASH_ = 4,
    DT_STRTAB_ = 5,
    DT_SYMTAB_ = 6,
    DT_STRSZ_ = 10,
    DT_INIT_ = 12,
    DT_SONAME_ = 14,
    DT_REL_ = 17,
    DT_RELSZ_ = 18,
    DT_JMPREL_ = 23,
    DT_INIT_ARRAY_ = 25,
    DT_INIT_ARRAYSZ_ = 27,
    DT_GNU_HASH_ = 0x6ffffef5,
};
enum { R_ARM_NONE_ = 0, R_ARM_ABS32_ = 2, R_ARM_GLOB_DAT_ = 21, R_ARM_JUMP_SLOT_ = 22, R_ARM_RELATIVE_ = 23 };

struct Arm32Library {
    char* name;
    char* path;
    uint32_t base, size; /* the bias, and the span its segments take */
    uint32_t symtab, strtab, hash, nbucket, nchain;
    uint32_t gnu_hash; /* DT_GNU_HASH's table, the only one of an NDK r28 library for API level 26 */
    uint32_t exidx, exidx_count;
    Arm32Library* next;
};
static Arm32Library* libraries; /* in load order */
static Arm32Library** libraries_end = &libraries;
static char* library_dir;

void arm32_set_library_dir(const char* dir) {
    free(library_dir);
    library_dir = dir ? strdup(dir) : NULL;
}
const char* arm32_library_name(const Arm32Library* l) { return l->name; }

const Arm32Library* arm32_owner(uint32_t address) {
    for (const Arm32Library* l = libraries; l; l = l->next)
        if (address >= l->base && address - l->base < l->size) return l;
    return NULL;
}

uint32_t arm32_exidx(uint32_t pc, int* count) {
    const Arm32Library* l = arm32_owner(pc);
    *count = l ? (int)l->exidx_count : 0;
    return l ? l->exidx : 0;
}

static uint32_t elf_hash(const char* name) {
    uint32_t hv = 0;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) {
        hv = (hv << 4) + *p;
        uint32_t g = hv & 0xf0000000u;
        hv ^= g >> 24;
        hv &= ~g;
    }
    return hv;
}

/* symbol i of a library: its address when the library defines it under name, else 0 */
static uint32_t named(const Arm32Library* l, uint32_t i, const char* name) {
    uint32_t sym = l->symtab + 16 * i;
    uint16_t shndx = snx_ld_le16(arm32_mem + sym + 14);
    uint8_t info = arm32_mem[sym + 12];
    if (!shndx || (info >> 4) == 0 /* local */) return 0;
    return strcmp((const char*)arm32_mem + l->strtab + rd32(sym), name) ? 0 : l->base + rd32(sym + 4);
}

/* GNU's hash of a name (DT_GNU_HASH): h * 33 + c from 5381 */
static uint32_t gnu_hash_of(const char* name) {
    uint32_t h = 5381;
    for (const unsigned char* p = (const unsigned char*)name; *p; ++p) h = h * 33 + *p;
    return h;
}

/* a symbol a library defines, by its SysV hash or its GNU one: its address, 0 when it has none. The GNU table is
   nbuckets, symoffset, bloom_size, bloom_shift, the bloom filter's 32-bit words, the buckets, then a hash for each
   symbol from symoffset on, bit 0 set on the last of a bucket's chain */
static uint32_t defined(const Arm32Library* l, const char* name) {
    if (l->hash && l->nbucket) {
        uint32_t hv = elf_hash(name);
        for (uint32_t i = rd32(l->hash + 8 + 4 * (hv % l->nbucket)); i && i < l->nchain;
             i = rd32(l->hash + 8 + 4 * l->nbucket + 4 * i)) {
            uint32_t a = named(l, i, name);
            if (a) return a;
        }
        return 0;
    }
    if (!l->gnu_hash) return 0;
    uint32_t nbuckets = rd32(l->gnu_hash), symoffset = rd32(l->gnu_hash + 4);
    uint32_t bloom_size = rd32(l->gnu_hash + 8), bloom_shift = rd32(l->gnu_hash + 12);
    if (!nbuckets || !bloom_size) return 0;
    uint32_t h = gnu_hash_of(name);
    uint32_t bloom = rd32(l->gnu_hash + 16 + 4 * ((h / 32) % bloom_size));
    if (!((bloom >> (h % 32)) & (bloom >> ((h >> bloom_shift) % 32)) & 1)) return 0;
    uint32_t buckets = l->gnu_hash + 16 + 4 * bloom_size, chains = buckets + 4 * nbuckets;
    uint32_t i = rd32(buckets + 4 * (h % nbuckets));
    if (i < symoffset) return 0;
    for (;; ++i) {
        uint32_t chain = rd32(chains + 4 * (i - symoffset));
        if ((chain | 1) == (h | 1)) {
            uint32_t a = named(l, i, name);
            if (a) return a;
        }
        if (chain & 1) return 0;
    }
}

uint32_t arm32_symbol(const Arm32Library* library, const char* name) {
    if (library) return defined(library, name);
    for (const Arm32Library* l = libraries; l; l = l->next) {
        uint32_t a = defined(l, name);
        if (a) return a;
    }
    return 0;
}

static Arm32Library* find_loaded(const char* name) {
    for (Arm32Library* l = libraries; l; l = l->next)
        if (!strcmp(l->name, name)) return l;
    return NULL;
}

static bool relocate(Arm32Library* l, uint32_t rel, uint32_t size, char* error, size_t error_size) {
    for (uint32_t at = rel; at + 8 <= rel + size; at += 8) {
        uint32_t where = l->base + rd32(at), info = rd32(at + 4);
        uint32_t type = info & 0xff, symbol = info >> 8;
        uint32_t value = 0;
        if (symbol) {
            uint32_t sym = l->symtab + 16 * symbol;
            const char* name = (const char*)arm32_mem + l->strtab + rd32(sym);
            uint16_t shndx = snx_ld_le16(arm32_mem + sym + 14);
            uint8_t bind = arm32_mem[sym + 12] >> 4;
            value = arm32_symbol(NULL, name);
            if (!value && shndx) value = l->base + rd32(sym + 4);
            if (!value) value = arm32_hle_symbol(name);
            if (!value && bind != 2 /* weak */) value = arm32_hle_missing(name);
        }
        switch (type) {
        case R_ARM_NONE_: break;
        case R_ARM_ABS32_: wr32(where, rd32(where) + value); break;
        case R_ARM_GLOB_DAT_:
        case R_ARM_JUMP_SLOT_: wr32(where, value); break;
        case R_ARM_RELATIVE_: wr32(where, rd32(where) + l->base); break;
        default:
            snprintf(error, error_size, "%s: relocation type %u is not done", l->name, type);
            return false;
        }
    }
    return true;
}

static Arm32Library* load(const char* path, char* error, size_t error_size);

static Arm32Library* load_needed(const char* name, char* error, size_t error_size) {
    Arm32Library* l = find_loaded(name);
    if (l) return l;
    char* path = library_dir ? snx_path_join(library_dir, name) : NULL;
    if (!path || !snx_is_file(path)) {
        free(path);
        snprintf(error, error_size, "library \"%s\" not found", name);
        return NULL;
    }
    l = load(path, error, error_size);
    free(path);
    return l;
}

static Arm32Library* load(const char* path, char* error, size_t error_size) {
    const char* slash = strrchr(path, '/');
    const char* base_name = slash ? slash + 1 : path;
    Arm32Library* l = find_loaded(base_name);
    if (l) return l;
    size_t size = 0;
    unsigned char* file = snx_read_file(path, &size);
    if (!file) {
        snprintf(error, error_size, "library \"%s\" cannot be read", path);
        return NULL;
    }
    bool ok = size >= 52 && !memcmp(file, "\177ELF", 4) && file[4] == 1 && file[5] == 1 &&
              snx_ld_le16(file + 18) == 40 /* EM_ARM */;
    if (!ok) {
        free(file);
        snprintf(error, error_size, "\"%s\" is not a 32-bit ARM library", path);
        return NULL;
    }
    uint32_t phoff = snx_ld_le32(file + 28);
    uint16_t phnum = snx_ld_le16(file + 44), phentsize = snx_ld_le16(file + 42);
    uint32_t lo = UINT32_MAX, hi = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const unsigned char* ph = file + phoff + (size_t)i * phentsize;
        if (phoff + (size_t)(i + 1) * phentsize > size) break;
        if (snx_ld_le32(ph) != PT_LOAD_) continue;
        uint32_t vaddr = snx_ld_le32(ph + 8), memsz = snx_ld_le32(ph + 20);
        if (vaddr < lo) lo = vaddr;
        if (vaddr + memsz > hi) hi = vaddr + memsz;
    }
    lo &= ~(PAGE - 1);
    uint32_t span = (uint32_t)snx_align_up(hi - lo, PAGE);
    uint32_t at = lo <= hi ? arm32_map(span + SLAB) : 0;
    if (!at) {
        free(file);
        snprintf(error, error_size, "no room in the 32-bit window for \"%s\"", path);
        return NULL;
    }
    l = calloc(1, sizeof *l);
    l->path = strdup(path);
    l->name = strdup(base_name);
    /* the segments at 64 KiB alignment, as bionic's linker gives them */
    l->base = (uint32_t)snx_align_up(at, SLAB) - lo;
    l->size = hi;
    uint32_t dynamic = 0;
    for (unsigned i = 0; i < phnum; ++i) {
        const unsigned char* ph = file + phoff + (size_t)i * phentsize;
        uint32_t type = snx_ld_le32(ph), offset = snx_ld_le32(ph + 4), vaddr = snx_ld_le32(ph + 8);
        uint32_t filesz = snx_ld_le32(ph + 16), memsz = snx_ld_le32(ph + 20), pflags = snx_ld_le32(ph + 24);
        if (type == PT_LOAD_) {
            if ((uint64_t)offset + filesz <= size) memcpy(arm32_mem + l->base + vaddr, file + offset, filesz);
            if (pflags & PF_X_) add_code(l->base + vaddr, l->base + vaddr + memsz);
        } else if (type == PT_DYNAMIC_) {
            dynamic = l->base + vaddr;
        } else if (type == PT_ARM_EXIDX_) {
            l->exidx = l->base + vaddr;
            l->exidx_count = memsz / 8;
        }
    }
    free(file);
    *libraries_end = l;
    libraries_end = &l->next;
    uint32_t rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, init = 0, init_array = 0, init_arraysz = 0;
    for (uint32_t d = dynamic; d && rd32(d) != DT_NULL_; d += 8) {
        uint32_t tag = rd32(d), value = rd32(d + 4);
        switch (tag) {
        case DT_HASH_: l->hash = l->base + value; break;
        case DT_GNU_HASH_: l->gnu_hash = l->base + value; break;
        case DT_STRTAB_: l->strtab = l->base + value; break;
        case DT_SYMTAB_: l->symtab = l->base + value; break;
        case DT_REL_: rel = l->base + value; break;
        case DT_RELSZ_: relsz = value; break;
        case DT_JMPREL_: jmprel = l->base + value; break;
        case DT_PLTRELSZ_: pltrelsz = value; break;
        case DT_INIT_: init = l->base + value; break;
        case DT_INIT_ARRAY_: init_array = l->base + value; break;
        case DT_INIT_ARRAYSZ_: init_arraysz = value; break;
        default: break;
        }
    }
    if (l->hash) {
        l->nbucket = rd32(l->hash);
        l->nchain = rd32(l->hash + 4);
    }
    /* the libraries it needs first: the app's own, loaded; the system's, the HLE's */
    for (uint32_t d = dynamic; d && rd32(d) != DT_NULL_; d += 8)
        if (rd32(d) == DT_NEEDED_) {
            const char* needed = (const char*)arm32_mem + l->strtab + rd32(d + 4);
            if (arm32_hle_is_system_library(needed)) continue;
            if (!load_needed(needed, error, error_size)) return NULL;
        }
    if (!relocate(l, rel, relsz, error, error_size) || !relocate(l, jmprel, pltrelsz, error, error_size))
        return NULL;
    if (init) arm32_call(init, NULL, 0, NULL);
    for (uint32_t i = 0; i < init_arraysz / 4; ++i) {
        uint32_t fn = rd32(init_array + 4 * i);
        if (fn && fn != UINT32_MAX) arm32_call(fn, NULL, 0, NULL);
    }
    return l;
}

Arm32Library* arm32_load(const char* path, char* error, size_t error_size) {
    static pthread_mutex_t load_lock = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&load_lock);
    /* a name alone is a library of the app's folder, as bionic's linker finds it */
    Arm32Library* l = strchr(path, '/') ? load(path, error, error_size) : load_needed(path, error, error_size);
    pthread_mutex_unlock(&load_lock);
    return l;
}

/* ---- arguments ---- */

void arm32_args(Arm32Args* a, const SnxArmCPU* c, unsigned first_word) {
    *a = (Arm32Args){c, first_word, false, 0};
}
void arm32_args_va(Arm32Args* a, uint32_t va_list) { *a = (Arm32Args){NULL, 0, true, va_list}; }
uint32_t arm32_arg(Arm32Args* a) {
    if (a->va) {
        uint32_t v = rd32(a->pointer);
        a->pointer += 4;
        return v;
    }
    uint32_t w = a->word++;
    return w < 4 ? a->c->r[w] : rd32(a->c->r[13] + 4 * (w - 4));
}
uint64_t arm32_arg64(Arm32Args* a) {
    if (a->va) a->pointer = (uint32_t)snx_align_up(a->pointer, 8);
    else a->word = (uint32_t)snx_align_up(a->word, 2);
    uint64_t lo = arm32_arg(a);
    return lo | (uint64_t)arm32_arg(a) << 32;
}
