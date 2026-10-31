/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT (jit_service.h). C99 in style; the target's C11 is for the LLVM-C headers */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 /* sched_getaffinity: the CPUs this process may run on */
#endif
#include "common/cpu/recompiler/llvm/jit_service.h"
#include "common/utilities/common.h"
#include "common/utilities/ascii.h"
#include "common/utilities/bits.h"
#include "common/utilities/endian.h"
#include "common/cpu/recompiler/llvm/jit_cache.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/rtlib_object.h"
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
typedef SRWLOCK JitLock;
typedef CONDITION_VARIABLE JitCond;
typedef HANDLE JitThread;
#else
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
typedef pthread_mutex_t JitLock;
typedef pthread_cond_t JitCond;
typedef pthread_t JitThread;
#endif

/* What the service knows of a pc. QUEUED_WARM: in the warm queue only. QUEUED: in the misses' queue. IN_FLIGHT: a
   batch compiles from it. FAILED: it can't be compiled (snx_jit_wait answers NULL), and isn't tried again. GONE: the
   memory it was compiled from changed (snx_jit_invalidate), or its image was removed (snx_jit_remove_image): the pc is
   new to the service if code turns up there again */
enum { QUEUED_WARM = 1, QUEUED, IN_FLIGHT, DONE, FAILED, GONE };
enum { LIVE, WARM, QUEUES }; /* the queues, by priority */

typedef struct {
    SnxU64 key; /* pc | 1 (a pc is 4-aligned), 0 for a free slot */
    GuestFn code;
    int state;
    SnxU64 taken; /* IN_FLIGHT: the take that made it so (s->takes), so a batch ends its own pcs only */
} Entry;

/* A unit published, from a batch or the cache: the span of guest code its functions were compiled from and the leaders
   it published, each with its code. Its functions call each other directly, so a change anywhere in the span drops
   the whole unit (snx_jit_invalidate), not only the leaders in the range: a mod's hook in the middle of a function
   reaches every caller. One allocation per unit, under lock */
typedef struct {
    SnxU64 pc;
    GuestFn code;
} Published;
typedef struct {
    SnxU64 lo, hi;
    size_t count;
    Published* leaders;
} UnitSpan;

/* pcs to compile, first in first out: a ring that grows on its own */
typedef struct {
    SnxU64* pcs;
    size_t head, count, capacity; /* capacity: 0 or a power of two */
} Queue;

/* A dispatch table the service fills: the main one (config.dispatch over [code_lo, code_hi), regions[0]), and one per
   image added while the game runs (snx_jit_add_image: an NRO's text), whose slots snx_jit_resolve reads, as nothing
   else knows them. A free region has hi 0. lo and hi change under lock and are read without it (hi last, with acquire
   and release ordering), and an added region's arrays live until its removal, which waits for the batches reading its
   image (users). pending: one bit per instruction, set (under lock) once the pc is in the misses' queue, compiling or
   failed, and cleared (under lock) when it is none of those any more: invalidated, or a warm pc going back to the warm
   queue from a dropped batch (compile). A set bit with nothing queued would leave the pc uncompiled for good, as
   snx_jit_resolve's fast path never takes the lock to queue it. A guest thread that misses a pc with its bit set and
   its slot empty gets NULL without the lock: every miss after the first, until the code is published, costs two loads.
   It also tells discovery, without the lock, which pcs other batches have or will have, so a batch doesn't compile them
   too */
typedef struct {
    SnxU64 lo, hi;
    GuestFn* slots;
    unsigned char* pending;
    SnxCodeImage image; /* an added region's */
    unsigned users;     /* under lock: batches compiling with its image */
} Region;

/* The added regions by page, for region_of: region_top[pc >> 24] is a leaf of 4096 entries, one per 4 KiB page of that
   16 MiB chunk, each the region's index in regions (0: none). The top grows toward higher chunks under the lock with
   an atomic pointer swap; the tops it had live on in older, until the service is freed, as region_of may be reading
   one. A leaf is added when an image first reaches its chunk, and its pages are set after the region's own release
   store, so a thread that reads an entry sees the region complete */
#define REGION_TOP_SHIFT    24
#define REGION_PAGE_SHIFT   12
#define REGION_LEAF_ENTRIES (1u << (REGION_TOP_SHIFT - REGION_PAGE_SHIFT))
typedef struct RegionTop {
    size_t count; /* entries in leaves */
    struct RegionTop* older;
    unsigned short* leaves[];
} RegionTop;

/* The cache file (SnxJitConfig.cache_path) as this run read it: its records, and the leaders of their functions by pc
   for the batches to find. A record is one unit: its table's name, its functions (each its leaders, its instructions
   as runs of addresses and a hash of their words) and its object. The file: CACHE_MAGIC, the key's length and the key
   (what the objects depend on), then records, each RECORD_MAGIC, its body's length, the body and the body's hash, so
   a run that ended in the middle of a record leaves the ones before it readable */
typedef struct {
    const unsigned char* functions; /* the body's function list, in the buffer */
    const unsigned char* object;
    size_t object_size, function_count;
    SnxU64 table; /* the unit's table, once loaded: a load again publishes its code from it */
    char name[64];
    int state; /* 0 not loaded yet, 1 loaded (and published again for a pc of it that is not done: its code was
                  invalidated since and may still be what the record holds), -1 refused (its code changed since, or
                  the loader refused it) */
} CacheRecord;
typedef struct {
    SnxU64 key; /* pc | 1, 0 for a free slot */
    size_t record;
} CacheSlot;
typedef struct {
    unsigned char* buffer; /* the file as read */
    CacheRecord* records;
    size_t count;
    CacheSlot* index; /* the records' leaders, the newest record's for a pc in several, at most half full */
    size_t capacity;  /* a power of two */
    FILE* file;       /* appended to; NULL when there is no cache or it can't be written */
    size_t size;      /* the file's bytes: a record that would take it past SNX_JIT_CACHE_MAX is left out */
} Cache;

struct SnxJitService {
    SnxJitConfig config;
    SnxCodeLoader* loader;
    Region regions[1 + SNX_JIT_IMAGES];
    RegionTop* region_top; /* under lock for writes, read without it (acquire): the added regions' pages */
    /* the pcs: the misses and warm pcs queued, compiling or failed, and every leader of every compiled function, for
       a pc outside the dispatch table or one whose slot holds something else (a wrapper). An open-addressing table,
       at most half full, under lock */
    Entry* entries;
    size_t entry_count, entry_capacity; /* capacity: a power of two */
    Queue queues[QUEUES];               /* under lock */
    unsigned busy;                      /* batches compiling, under lock */
    int stop;                           /* under lock: the compile threads end */
    SnxU64 epoch;                       /* under lock: incremented by snx_jit_invalidate */
    SnxU64 takes;                       /* under lock: incremented by each take, which stamps its pcs with it */
    SnxJitStats stats;                  /* under lock */
    UnitSpan* spans;                    /* under lock: the units published, which snx_jit_invalidate drops */
    size_t span_count, span_capacity;
    JitLock lock;                       /* everything above marked so */
    JitCond work;                       /* signalled when a pc is queued, broadcast at stop */
    JitCond idle;                       /* broadcast when nothing is queued or compiling */
    JitCond released;                   /* broadcast when a batch stops reading added images */
    JitCond done;                       /* broadcast when a batch ends its pcs, or an image's pcs are gone */
    JitLock load_lock;                  /* the loader, one load at a time (code_loader.h), and units */
    unsigned long units;                /* loads: the number in each unit's table name */
    char run[20];                       /* this run's part of each unit's table name, unique across runs */
    Cache cache;                        /* under load_lock but its index, fixed once read */
    JitThread threads[SNX_JIT_THREADS];
};

static void lock_init(JitLock* l) {
#ifdef _WIN32
    InitializeSRWLock(l);
#else
    pthread_mutex_init(l, NULL);
#endif
}
static void lock_free(JitLock* l) {
#ifdef _WIN32
    (void)l;
#else
    pthread_mutex_destroy(l);
#endif
}
static void lock(JitLock* l) {
#ifdef _WIN32
    AcquireSRWLockExclusive(l);
#else
    pthread_mutex_lock(l);
#endif
}
static void unlock(JitLock* l) {
#ifdef _WIN32
    ReleaseSRWLockExclusive(l);
#else
    pthread_mutex_unlock(l);
#endif
}
static void cond_init(JitCond* c) {
#ifdef _WIN32
    InitializeConditionVariable(c);
#else
    pthread_cond_init(c, NULL);
#endif
}
static void cond_free(JitCond* c) {
#ifdef _WIN32
    (void)c;
#else
    pthread_cond_destroy(c);
#endif
}
static void cond_wait(JitCond* c, JitLock* l) {
#ifdef _WIN32
    SleepConditionVariableSRW(c, l, INFINITE, 0);
#else
    pthread_cond_wait(c, l);
#endif
}
static void cond_signal(JitCond* c) {
#ifdef _WIN32
    WakeConditionVariable(c);
#else
    pthread_cond_signal(c);
#endif
}
static void cond_broadcast(JitCond* c) {
#ifdef _WIN32
    WakeAllConditionVariable(c);
#else
    pthread_cond_broadcast(c);
#endif
}

/* ---- the pcs and the queues, under lock ---- */

static size_t entry_hash(SnxU64 key, size_t capacity) {
    return (size_t)(((key >> 2) * (SnxU64)0x9E3779B97F4A7C15ULL) >> 32) & (capacity - 1);
}

static Entry* find(const SnxJitService* s, SnxU64 pc) {
    SnxU64 key = pc | 1;
    if (!s->entry_capacity) return NULL;
    for (size_t i = entry_hash(key, s->entry_capacity); s->entries[i].key; i = (i + 1) & (s->entry_capacity - 1))
        if (s->entries[i].key == key) return &s->entries[i];
    return NULL;
}

/* pc's entry, added with state when it has none. NULL when memory runs out */
static Entry* add(SnxJitService* s, SnxU64 pc, int state) {
    SnxU64 key = pc | 1;
    Entry* e = find(s, pc);
    if (e) return e;
    if ((s->entry_count + 1) * 2 > s->entry_capacity) {
        size_t capacity = s->entry_capacity ? s->entry_capacity * 2 : 1024;
        if (capacity > SIZE_MAX / sizeof(Entry)) return NULL;
        Entry* grown = (Entry*)calloc(capacity, sizeof(*grown));
        if (!grown) return NULL;
        for (size_t j = 0; j < s->entry_capacity; ++j)
            if (s->entries[j].key) {
                size_t i = entry_hash(s->entries[j].key, capacity);
                while (grown[i].key) i = (i + 1) & (capacity - 1);
                grown[i] = s->entries[j];
            }
        free(s->entries);
        s->entries = grown;
        s->entry_capacity = capacity;
    }
    size_t i;
    for (i = entry_hash(key, s->entry_capacity); s->entries[i].key; i = (i + 1) & (s->entry_capacity - 1)) continue;
    e = &s->entries[i];
    e->key = key;
    e->code = NULL;
    e->state = state;
    ++s->entry_count;
    return e;
}

/* 0, or -1 when memory runs out */
static int push(Queue* q, SnxU64 pc) {
    if (q->count == q->capacity) {
        size_t capacity = q->capacity ? q->capacity * 2 : 256;
        if (capacity > SIZE_MAX / sizeof(SnxU64)) return -1;
        SnxU64* grown = (SnxU64*)malloc(capacity * sizeof(*grown));
        if (!grown) return -1;
        for (size_t i = 0; i < q->count; ++i) grown[i] = q->pcs[(q->head + i) & (q->capacity - 1)];
        free(q->pcs);
        q->pcs = grown;
        q->head = 0;
        q->capacity = capacity;
    }
    q->pcs[(q->head + q->count) & (q->capacity - 1)] = pc;
    ++q->count;
    return 0;
}

static SnxU64 pop(Queue* q) {
    SnxU64 pc = q->pcs[q->head];
    q->head = (q->head + 1) & (q->capacity - 1);
    --q->count;
    return pc;
}

/* ---- what the threads read without the lock ---- */

/* pc in the text of the config's images */
static int in_text(const SnxJitService* s, SnxU64 pc) {
    for (size_t i = 0; !(pc & 3) && i < s->config.image_count; ++i) {
        const SnxCodeImage* m = &s->config.images[i];
        if (pc >= m->base + m->text_off && pc - (m->base + m->text_off) < m->text_size) return 1;
    }
    return 0;
}

/* the region whose table has a slot for pc, or NULL */
static Region* region_of(const SnxJitService* s, SnxU64 pc) {
    Region* r = (Region*)&s->regions[0];
    if (pc & 3) return NULL;
    if (pc - r->lo < r->hi - r->lo) return r;
    RegionTop* top = __atomic_load_n(&s->region_top, __ATOMIC_ACQUIRE);
    if (top && (size_t)(pc >> REGION_TOP_SHIFT) < top->count) {
        unsigned short* leaf = __atomic_load_n(&top->leaves[pc >> REGION_TOP_SHIFT], __ATOMIC_ACQUIRE);
        if (leaf) {
            unsigned short index = __atomic_load_n(&leaf[(pc >> REGION_PAGE_SHIFT) & (REGION_LEAF_ENTRIES - 1)],
                                                   __ATOMIC_ACQUIRE);
            if (index) return (Region*)&s->regions[index]; /* 1-based; regions[0] is never an image */
        }
    }
    /* a pc outside the map's coverage, or under a leaf whose entry isn't set: the regions themselves */
    for (size_t i = 1; i <= SNX_JIT_IMAGES; ++i) {
        r = (Region*)&s->regions[i];
        SnxU64 hi = __atomic_load_n(&r->hi, __ATOMIC_ACQUIRE);
        if (hi && pc >= __atomic_load_n(&r->lo, __ATOMIC_RELAXED) && pc < hi) return r;
    }
    return NULL;
}

static GuestFn* slot(const Region* r, SnxU64 pc) { return &r->slots[(pc - r->lo) >> 2]; }

static int is_pending(const Region* r, SnxU64 pc) {
    size_t i = (size_t)((pc - r->lo) >> 2);
    return __atomic_load_n(&r->pending[i >> 3], __ATOMIC_RELAXED) >> (i & 7) & 1;
}

/* under lock, with the entry that says so in place: a thread that reads the bit without the lock gets NULL, which is
   right whatever the entry says by then (the code, once published, is in the slot) */
static void set_pending(SnxJitService* s, SnxU64 pc) {
    Region* r = region_of(s, pc);
    if (!r) return;
    size_t i = (size_t)((pc - r->lo) >> 2);
    __atomic_fetch_or(&r->pending[i >> 3], (unsigned char)(1u << (i & 7)), __ATOMIC_RELAXED);
}

/* under lock: pc is new to the service again, so discovery isn't told another batch has it */
static void clear_pending(Region* r, SnxU64 pc) {
    size_t i = (size_t)((pc - r->lo) >> 2);
    __atomic_fetch_and(&r->pending[i >> 3], (unsigned char)~(1u << (i & 7)), __ATOMIC_RELAXED);
}

/* ---- the compile ---- */

/* a batch's pcs by address, for discovery's question */
typedef struct {
    SnxJitService* service;
    const SnxU64* pcs;
    size_t count;
} Batch;

static int in_batch(const Batch* b, SnxU64 pc) {
    size_t lo = 0, hi = b->count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (b->pcs[middle] < pc)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < b->count && b->pcs[lo] == pc;
}

/* snx_codegen_discover's question: code exists at address already (an AOT entry, an earlier batch's leader) or will
   without this batch (a pc queued, compiling in another batch, or failed) */
static int known(void* user, SnxU64 address) {
    const Batch* b = (const Batch*)user;
    SnxJitService* s = b->service;
    Region* r = region_of(s, address);
    if (r) {
        if (__atomic_load_n(slot(r, address), __ATOMIC_RELAXED)) return 1;
        return is_pending(r, address) && !in_batch(b, address);
    }
    lock(&s->lock);
    Entry* e = find(s, address);
    int result = e && e->state != QUEUED_WARM && e->state != GONE && !(e->state == IN_FLIGHT && in_batch(b, address));
    unlock(&s->lock);
    return result;
}

/* the AOT function to call for target, as a host address (SnxIr.direct), 0 when the AOT module has no entry there */
static SnxU64 direct(void* user, SnxU64 target) {
    const SnxJitService* s = (const SnxJitService*)user;
    size_t lo = 0, hi = s->config.aot_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (s->config.aot_addresses[middle] < target)
            lo = middle + 1;
        else
            hi = middle;
    }
    if (lo == s->config.aot_count || s->config.aot_addresses[lo] != target) return 0;
    /* a function the runtime found stale (its code changed: SNX_ABI 14) leaves the table while the JIT runs */
    return (SnxU64)(size_t)__atomic_load_n(&s->config.aot_functions[lo], __ATOMIC_ACQUIRE);
}

/* the loader's resolver: snx_aot_<target>, the AOT module's function for target (ir_codegen.c's call) */
static SnxU64 resolve_aot(void* user, const char* name) {
    SnxU64 target = 0;
    if (strncmp(name, "snx_aot_", 8) || !name[8]) return 0;
    for (const char* p = name + 8; *p; ++p) {
        int digit = snx_hex_value((unsigned char)*p);
        if (digit < 0) return 0;
        target = target << 4 | (SnxU64)digit;
    }
    return direct(user, target);
}

#if defined(__powerpc64__)
/* The TOC the AOT module's code keeps in r2, its one (link_elf.c, or library.c's arena), which the JIT's arena shares
   (snx_code_loader_new_toc): units call and jump into the module's functions directly, and the module's jumps through
   the dispatch table reach the units, each side with r2 as it left it. An ELFv1 function's descriptor holds it, and an
   ELFv2 global entry computes it from r12 (addis r2, r12, hi; addi r2, r2, lo), which a function that uses no TOC
   leaves out: the first function that shows it. When none does, none uses r2, and any address near them will do
   for the arena's own (the first function's: the loader's veneers reach the others from it) */
static uint64_t aot_toc(const SnxJitConfig* config) {
    for (size_t i = 0; i < config->aot_count; ++i) {
        const unsigned char* f = (const unsigned char*)(size_t)config->aot_functions[i];
        uint64_t toc;
        if (!f) continue;
#if defined(_CALL_ELF) && _CALL_ELF == 2
        {
            uint32_t hi, lo;
            memcpy(&hi, f, 4);
            memcpy(&lo, f + 4, 4);
            if ((hi & 0xFFFF0000u) != 0x3C4C0000u || (lo & 0xFFFF0000u) != 0x38420000u) continue;
            toc = (uint64_t)(size_t)f + (uint64_t)((int64_t)(int16_t)(hi & 0xFFFFu) * 65536) +
                  (uint64_t)(int64_t)(int16_t)(lo & 0xFFFFu);
        }
#else
        memcpy(&toc, f + 8, 8);
#endif
        if (toc) return toc;
    }
    for (size_t i = 0; i < config->aot_count; ++i)
        if (config->aot_functions[i]) return (uint64_t)(size_t)config->aot_functions[i];
    return 0;
}
#endif

/* The unit's object: every function of codegen, internal to it, and table_name, an array of their addresses in
   codegen's order, its only global. Internal functions give two units the same F_<address> without a clash in the
   loader's table, where every global stays */
static int emit_unit(SnxJitService* s, SnxCodegen* codegen, const char* table_name, unsigned char** object,
                     size_t* size, char* error, size_t error_size) {
    SnxIr* ir = snx_ir_new_jit("jit", s->config.opt, error, error_size);
    if (!ir) return -1;
    LLVMValueRef* functions = (LLVMValueRef*)malloc(codegen->function_count * sizeof(*functions));
    if (!functions) {
        snx_ir_free(ir);
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    ir->direct = direct;
    ir->direct_user = s;
    codegen->hook_direct_calls = s->config.hook_direct_calls;
    codegen->direct_svc = s->config.direct_svc;
    for (size_t i = 0; i < codegen->function_count; ++i) snx_ir_emit(ir, codegen, &codegen->functions[i]);
    for (size_t i = 0; i < codegen->function_count; ++i) {
        functions[i] = snx_ir_guest_function(ir, codegen->functions[i].start);
        LLVMSetLinkage(functions[i], LLVMInternalLinkage);
        LLVMSetVisibility(functions[i], LLVMDefaultVisibility); /* a local symbol has no visibility */
    }
    LLVMTypeRef type = LLVMArrayType(ir->t_ptr, (unsigned)codegen->function_count);
    LLVMValueRef table = LLVMAddGlobal(ir->mod, type, table_name);
    LLVMSetInitializer(table, LLVMConstArray(ir->t_ptr, functions, (unsigned)codegen->function_count));
    LLVMSetGlobalConstant(table, 1);
    LLVMSetVisibility(table, LLVMHiddenVisibility);
    free(functions);
    int rc = snx_ir_object(ir, s->config.opt, object, size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

/* function i's code in a loaded unit's table */
static GuestFn unit_function(SnxU64 table, size_t i) {
    GuestFn code;
    memcpy(&code, (const unsigned char*)(size_t)table + i * sizeof(code), sizeof(code));
    return code;
}

/* Under lock: a unit's span and leaders, taken over (freed when it can't be kept: a change then drops only the
   leaders in its range, as before) */
static void span_keep(SnxJitService* s, SnxU64 lo, SnxU64 hi, Published* leaders, size_t count) {
    if (!count || lo >= hi) {
        free(leaders);
        return;
    }
    if (s->span_count == s->span_capacity) {
        size_t capacity = s->span_capacity ? 2 * s->span_capacity : 256;
        UnitSpan* grown = (UnitSpan*)realloc(s->spans, capacity * sizeof(*grown));
        if (!grown) {
            free(leaders);
            return;
        }
        s->spans = grown;
        s->span_capacity = capacity;
    }
    s->spans[s->span_count].lo = lo;
    s->spans[s->span_count].hi = hi;
    s->spans[s->span_count].count = count;
    s->spans[s->span_count++].leaders = leaders;
}
/* the leaders of a batch's functions, with their code in table, as a span (span_keep) */
static void span_of_batch(SnxJitService* s, const SnxCodegen* codegen, SnxU64 table) {
    size_t count = 0, n = 0;
    SnxU64 lo = ~(SnxU64)0, hi = 0, a;
    for (size_t i = 0; i < codegen->function_count; ++i) count += codegen->functions[i].leader_count;
    Published* leaders = count ? (Published*)malloc(count * sizeof(*leaders)) : NULL;
    if (!leaders) return;
    for (size_t i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        GuestFn code = unit_function(table, i);
        if (!f->count) continue;
        if ((a = snx_function_address(f, 0)) < lo) lo = a;
        if ((a = snx_function_address(f, f->count - 1) + 4) > hi) hi = a;
        for (size_t j = 0; j < f->count && n < count; ++j)
            if (f->leaders[j]) {
                leaders[n].pc = snx_function_address(f, j);
                leaders[n++].code = code;
            }
    }
    span_keep(s, lo, hi, leaders, n);
}

/* ---- the cache (Cache) ---- */

/* a growing byte buffer for a record */
typedef struct {
    unsigned char* bytes;
    size_t size, capacity;
    int failed;
} Out;
static void out_bytes(Out* o, const void* data, size_t n) {
    if (o->failed) return;
    if (o->size + n > o->capacity) {
        size_t capacity = o->capacity ? o->capacity : 4096;
        unsigned char* grown;
        while (capacity < o->size + n) capacity *= 2;
        grown = (unsigned char*)realloc(o->bytes, capacity);
        if (!grown) {
            o->failed = 1;
            return;
        }
        o->bytes = grown;
        o->capacity = capacity;
    }
    memcpy(o->bytes + o->size, data, n);
    o->size += n;
}
static void out32(Out* o, SnxU32 v) {
    unsigned char b[4];
    snx_st_le32(b, v);
    out_bytes(o, b, 4);
}
static void out64(Out* o, SnxU64 v) {
    out32(o, (SnxU32)v);
    out32(o, (SnxU32)(v >> 32));
}

/* the instruction word at address in the images: 0, or -1 outside their text */
static int image_word(const SnxCodeImage* images, size_t count, SnxU64 address, SnxU32* word) {
    for (size_t i = 0; i < count; ++i) {
        const SnxCodeImage* m = &images[i];
        SnxU64 start = m->base + m->text_off, off = address - m->base;
        if (address < start || address - start + 4 > m->text_size || off + 4 > m->image_size) continue;
        *word = snx_ld_le32(m->image + (size_t)off);
        return 0;
    }
    return -1;
}

/* Two 64-bit hashes of a function's instructions, each address with its word: 0, or -1 when one is outside the
   images. runs: count pairs of (start, words) */
static int hash_runs(const SnxCodeImage* images, size_t image_count, const unsigned char* runs, size_t count,
                     SnxU64* h1, SnxU64* h2) {
    SnxU64 a = SNX_VARIANT_H1_BASIS, b = SNX_VARIANT_H2_BASIS;
    for (size_t i = 0; i < count; ++i) {
        SnxU64 address = snx_ld_le64(runs + 12 * i);
        SnxU32 n = snx_ld_le32(runs + 12 * i + 8);
        for (SnxU32 k = 0; k < n; ++k, address += 4) {
            SnxU32 word;
            if (image_word(images, image_count, address, &word)) return -1;
            snx_variant_mix(&a, &b, address, word);
        }
    }
    *h1 = a;
    *h2 = b;
    return 0;
}

/* What the objects depend on beyond the code: the ABI, the IR object version, the host target triple, LLVM, the
   runtime library, the host CPU and its features (the code is compiled for them), the -O level, hooked direct calls,
   direct SVCs, SLP vectorization, FPSR flag exactness, volatile guest accesses, the guest's A64 features and the
   decoder */
static char* cache_key(const SnxJitService* s) {
    return snx_jit_cache_key(s->config.opt, s->config.hook_direct_calls, s->config.direct_svc, s->config.features);
}

/* the records of buffer (size bytes after the header), parsed until the first that isn't whole: their count, and the
   bytes they take */
static size_t cache_parse(Cache* c, size_t start, size_t size, size_t* used) {
    size_t at = start, count = 0, f;
    while (at + 12 <= size) {
        SnxU64 length = snx_ld_le64(c->buffer + at + 4);
        const unsigned char* body = c->buffer + at + 12;
        if (snx_ld_le32(c->buffer + at) != SNX_JIT_RECORD_MAGIC || length > size - at - 12 ||
            size - at - 12 - length < 8 ||
            snx_fnv1a(SNX_FNV_BASIS, body, (size_t)length) != snx_ld_le64(body + length))
            break;
        CacheRecord* r = &c->records[count];
        const unsigned char* p = body;
        size_t name_length = snx_ld_le16(p);
        if (name_length >= sizeof(r->name) || 2 + name_length + 4 > length) break;
        memcpy(r->name, p + 2, name_length);
        r->name[name_length] = 0;
        p += 2 + name_length;
        r->function_count = snx_ld_le32(p);
        p += 4;
        r->functions = p;
        /* walk the functions to the object: each is start, leaders, runs, two hashes */
        for (f = 0; f < r->function_count; ++f) {
            if ((size_t)(p - body) + 12 > length) break;
            size_t leaders = snx_ld_le32(p + 8);
            if (leaders > (length - (size_t)(p - body) - 12) / 8) break;
            p += 12 + 8 * leaders;
            if ((size_t)(p - body) + 4 > length) break;
            size_t runs = snx_ld_le32(p);
            if (runs > (length - (size_t)(p - body) - 4) / 12) break;
            p += 4 + 12 * runs;
            if ((size_t)(p - body) + 16 > length) break;
            p += 16;
        }
        if (f < r->function_count || (size_t)(p - body) + 8 > length) break;
        r->object_size = (size_t)snx_ld_le64(p);
        r->object = p + 8;
        if (r->object_size != length - (size_t)(r->object - body)) break;
        r->state = 0;
        ++count;
        at += 12 + (size_t)length + 8;
    }
    *used = at;
    return count;
}

/* every leader of every record into the index, the newest record last so its own win */
static int cache_index(Cache* c) {
    size_t total = 0;
    for (size_t i = 0; i < c->count; ++i) {
        const unsigned char* p = c->records[i].functions;
        for (size_t f = 0; f < c->records[i].function_count; ++f) {
            size_t leaders = snx_ld_le32(p + 8);
            total += leaders;
            p += 12 + 8 * leaders;
            p += 4 + 12 * (size_t)snx_ld_le32(p) + 16;
        }
    }
    for (c->capacity = 1024; c->capacity < 2 * total + 2; c->capacity *= 2) continue;
    c->index = (CacheSlot*)calloc(c->capacity, sizeof(*c->index));
    if (!c->index) return -1;
    for (size_t i = 0; i < c->count; ++i) {
        const unsigned char* p = c->records[i].functions;
        for (size_t f = 0; f < c->records[i].function_count; ++f) {
            size_t leaders = snx_ld_le32(p + 8);
            for (size_t k = 0; k < leaders; ++k) {
                SnxU64 key = snx_ld_le64(p + 12 + 8 * k) | 1;
                size_t slot;
                for (slot = entry_hash(key, c->capacity); c->index[slot].key && c->index[slot].key != key;
                     slot = (slot + 1) & (c->capacity - 1))
                    continue;
                c->index[slot].key = key;
                c->index[slot].record = i;
            }
            p += 12 + 8 * leaders;
            p += 4 + 12 * (size_t)snx_ld_le32(p) + 16;
        }
    }
    return 0;
}

/* Opens the cache: reads what an earlier run with the same key wrote, and starts the file over when there is none or
   its key differs. A run that left a record unfinished, or a file past SNX_JIT_CACHE_MAX (an earlier build's appends
   had no limit), is rewritten with the whole records the limit holds: refusing the file outright threw a full cache
   away the run after it filled, so every other run started cold */
static void cache_open(SnxJitService* s) {
    Cache* c = &s->cache;
    char* key = cache_key(s);
    FILE* f = fopen(s->config.cache_path, "rb");
    size_t size = 0, used = 0;
    long length = -1;
    if (!key) return;
    size_t key_length = strlen(key);
    size_t header = 8 + 4 + key_length;
    if (f && !fseek(f, 0, SEEK_END)) length = ftell(f);
    if (f && length >= (long)header && !fseek(f, 0, SEEK_SET)) {
        size = (size_t)length < SNX_JIT_CACHE_MAX ? (size_t)length : SNX_JIT_CACHE_MAX;
        c->buffer = (unsigned char*)malloc(size);
        if (c->buffer && fread(c->buffer, 1, size, f) != size) {
            free(c->buffer);
            c->buffer = NULL;
        }
    }
    if (f) fclose(f);
    if (c->buffer && (memcmp(c->buffer, SNX_JIT_CACHE_MAGIC, 8) || snx_ld_le32(c->buffer + 8) != key_length ||
                      memcmp(c->buffer + 12, key, key_length))) {
        free(c->buffer); /* another build or host's: of no use here */
        c->buffer = NULL;
    }
    if (c->buffer) {
        size_t slots = (size - header) / 20 + 1; /* a record takes more than 20 bytes */
        c->records = (CacheRecord*)calloc(slots, sizeof(*c->records));
        if (c->records) c->count = cache_parse(c, header, size, &used);
        if (!c->records || cache_index(c)) {
            free(c->records);
            free(c->buffer);
            c->records = NULL;
            c->buffer = NULL;
            c->count = 0;
        }
    }
    if (c->buffer && used == (size_t)length) {
        c->file = fopen(s->config.cache_path, "ab");
        c->size = used;
    } else {
        /* a new file: the header, and the whole records of the old one */
        char* temporary = (char*)malloc(strlen(s->config.cache_path) + 8);
        if (temporary) {
            sprintf(temporary, "%s.new", s->config.cache_path);
            c->file = fopen(temporary, "wb");
            if (c->file) {
                Out o = {0};
                out_bytes(&o, SNX_JIT_CACHE_MAGIC, 8);
                out32(&o, (SnxU32)key_length);
                out_bytes(&o, key, key_length);
                if (c->buffer) out_bytes(&o, c->buffer + header, used - header);
                if (o.failed || fwrite(o.bytes, 1, o.size, c->file) != o.size || fflush(c->file)) {
                    fclose(c->file);
                    c->file = NULL;
                    remove(temporary);
                }
                free(o.bytes);
            }
            if (c->file) {
                fclose(c->file);
                remove(s->config.cache_path); /* Windows' rename doesn't replace */
                c->file = rename(temporary, s->config.cache_path) ? NULL : fopen(s->config.cache_path, "ab");
                c->size = header + (c->buffer ? used - header : 0);
            }
            free(temporary);
        }
    }
    free(key);
}

static void cache_close(Cache* c) {
    if (c->file) fclose(c->file);
    free(c->index);
    free(c->records);
    free(c->buffer);
    *c = (Cache){0};
}

/* the record whose functions have pc as a leader, or NULL */
static CacheRecord* cache_find(Cache* c, SnxU64 pc) {
    SnxU64 key = pc | 1;
    if (!c->capacity) return NULL;
    for (size_t slot = entry_hash(key, c->capacity); c->index[slot].key; slot = (slot + 1) & (c->capacity - 1))
        if (c->index[slot].key == key) return &c->records[c->index[slot].record];
    return NULL;
}

/* Appends the unit just loaded: its functions as codegen found them, hashed from the images, and its object. Under
   load_lock */
static void cache_append(SnxJitService* s, const SnxCodegen* codegen, const char* name, const unsigned char* object,
                         size_t size, const SnxCodeImage* images, size_t image_count) {
    size_t name_length = strlen(name);
    SnxU64 h1 = 0, h2 = 0, start = 0, previous = 0;
    unsigned char head[12];
    if (!s->cache.file) return;
    Out body = {0}, runs = {0};
    snx_st_le16(head, (uint16_t)name_length);
    out_bytes(&body, head, 2);
    out_bytes(&body, name, name_length);
    out32(&body, (SnxU32)codegen->function_count);
    for (size_t i = 0; i < codegen->function_count && !body.failed; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        out64(&body, f->start);
        size_t leaders = 0;
        for (size_t j = 0; j < f->count; ++j) leaders += f->leaders[j] != 0;
        out32(&body, (SnxU32)leaders);
        for (size_t j = 0; j < f->count; ++j)
            if (f->leaders[j]) out64(&body, snx_function_address(f, j));
        /* the instructions as runs of consecutive addresses */
        runs.size = 0;
        size_t run_count = 0;
        for (size_t j = 0; j < f->count; ++j) {
            SnxU64 address = snx_function_address(f, j);
            if (!j || address != previous + 4) {
                if (j) out32(&runs, (SnxU32)((previous - start) / 4 + 1));
                out64(&runs, address);
                start = address;
                ++run_count;
            }
            previous = address;
        }
        if (f->count) out32(&runs, (SnxU32)((previous - start) / 4 + 1));
        out32(&body, (SnxU32)run_count);
        out_bytes(&body, runs.bytes, runs.size);
        if (runs.failed || hash_runs(images, image_count, runs.bytes, run_count, &h1, &h2)) body.failed = 1;
        out64(&body, h1);
        out64(&body, h2);
    }
    out64(&body, (SnxU64)size);
    out_bytes(&body, object, size);
    if (!body.failed) {
        h1 = snx_fnv1a(SNX_FNV_BASIS, body.bytes, body.size);
        snx_st_le32(head, SNX_JIT_RECORD_MAGIC);
        snx_st_le64(head + 4, (SnxU64)body.size);
        out64(&body, h1);
        /* a full file takes no more: past the limit the next run would read only the records before it */
        if (!body.failed &&
            (s->cache.size > SNX_JIT_CACHE_MAX - 12 || body.size > SNX_JIT_CACHE_MAX - 12 - s->cache.size))
            body.failed = 1;
        if (!body.failed && (fwrite(head, 1, 12, s->cache.file) != 12 ||
                             fwrite(body.bytes, 1, body.size, s->cache.file) != body.size || fflush(s->cache.file))) {
            fclose(s->cache.file); /* a disk that failed once: no more records this run */
            s->cache.file = NULL;
        } else if (!body.failed)
            s->cache.size += 12 + body.size;
    }
    free(body.bytes);
    free(runs.bytes);
}

/* Loads the record r and publishes its functions as a batch publishes its own, when every function's instructions
   are still those it was compiled from: 1, or 0 when it is refused (and it stays so). Under load_lock */
static int cache_load(SnxJitService* s, CacheRecord* r, const SnxCodeImage* images, size_t image_count) {
    const unsigned char* p = r->functions;
    const char* names[1];
    SnxU64 table = 0, span_lo = ~(SnxU64)0, span_hi = 0;
    size_t span_count = 0;
    Published* span = NULL;
    char error[256];
    lock(&s->lock);
    SnxU64 epoch = s->epoch; /* memory changing after here makes the record stale: refused below */
    unlock(&s->lock);
    for (size_t f = 0; f < r->function_count; ++f) { /* the code it was compiled from, unchanged */
        SnxU64 h1, h2;
        size_t leaders = snx_ld_le32(p + 8);
        p += 12 + 8 * leaders;
        size_t runs = snx_ld_le32(p);
        if (hash_runs(images, image_count, p + 4, runs, &h1, &h2) || h1 != snx_ld_le64(p + 4 + 12 * runs) ||
            h2 != snx_ld_le64(p + 12 + 12 * runs)) {
            r->state = -1;
            return 0;
        }
        p += 4 + 12 * runs + 16;
    }
    if (r->state == 1)
        table = r->table; /* its code is in the arena still (code is never freed), compiled from what the check found */
    else {
        /* the object at whatever offset the file put it, copied where malloc aligns it: the ELF reader reads its
           headers, sections and relocations in place. Without the memory, refused as the loader refuses */
        unsigned char* object = (unsigned char*)malloc(r->object_size ? r->object_size : 1);
        if (object) memcpy(object, r->object, r->object_size);
        SnxLinkInput input;
        input.data = object;
        input.size = r->object_size;
        input.name = r->name;
        names[0] = r->name;
        int refused = !object || snx_code_load(s->loader, &input, 1, names, 1, &table, error, sizeof(error));
        free(object);
        if (refused) {
            r->state = -1;
            return 0;
        }
        r->state = 1;
        r->table = table;
    }
    /* publish: each function's leaders, into their empty slots and the service's table (compile's), unless memory
       changed since the record's code was checked */
    p = r->functions;
    lock(&s->lock);
    if (epoch != s->epoch) {
        /* memory changed somewhere while it loaded, perhaps not under it: the next miss checks it again (its object
           loaded, once it is) */
        unlock(&s->lock);
        return 0;
    }
    /* the unit's span: its functions' runs, and its leaders (span_keep) */
    {
        const unsigned char* q = r->functions;
        size_t total = 0, n;
        for (size_t f = 0; f < r->function_count; ++f) {
            total += snx_ld_le32(q + 8);
            q += 12 + 8 * (size_t)snx_ld_le32(q + 8);
            q += 4 + 12 * (size_t)snx_ld_le32(q) + 16;
        }
        span = total ? (Published*)malloc(total * sizeof(*span)) : NULL;
        q = r->functions;
        n = 0;
        for (size_t f = 0; span && f < r->function_count; ++f) {
            for (size_t k = 0; k < snx_ld_le32(q + 8); ++k) {
                span[n].pc = snx_ld_le64(q + 12 + 8 * k);
                span[n++].code = unit_function(table, f);
            }
            q += 12 + 8 * (size_t)snx_ld_le32(q + 8);
            for (size_t k = 0; k < snx_ld_le32(q); ++k) {
                SnxU64 run = snx_ld_le64(q + 4 + 12 * k);
                if (run < span_lo) span_lo = run;
                if (run + 4 * (SnxU64)snx_ld_le32(q + 4 + 12 * k + 8) > span_hi)
                    span_hi = run + 4 * (SnxU64)snx_ld_le32(q + 4 + 12 * k + 8);
            }
            q += 4 + 12 * (size_t)snx_ld_le32(q) + 16;
        }
        span_count = n;
    }
    for (size_t f = 0; f < r->function_count; ++f) {
        GuestFn code = unit_function(table, f);
        size_t leaders = snx_ld_le32(p + 8);
        for (size_t k = 0; k < leaders; ++k) {
            SnxU64 address = snx_ld_le64(p + 12 + 8 * k);
            Entry* e = add(s, address, DONE);
            GuestFn expected = NULL;
            /* a GONE pc was invalidated before the record's code was checked (epoch is unchanged since), and the check
               found the record's code still there: published again, as a load after an invalidation is for */
            Region* region = region_of(s, address);
            if (region)
                __atomic_compare_exchange_n(slot(region, address), &expected, code, 0, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED);
            if (!e || e->state == FAILED || (e->state == DONE && e->code)) continue;
            e->state = DONE; /* a pc queued or compiling elsewhere is done: its batch's end leaves it so */
            e->code = code;
        }
        p += 12 + 8 * leaders;
        p += 4 + 12 * (size_t)snx_ld_le32(p) + 16;
    }
    span_keep(s, span_lo, span_hi, span, span_count);
    ++s->stats.cached;
    cond_broadcast(&s->done);
    unlock(&s->lock);
    return 1;
}

/* Under lock: a pc of a batch that was dropped back on the queue its take found it in (LIVE or WARM), its pending bit
   as that queue's pcs have it: set for a miss, clear for a warm pc (snx_jit_warm leaves it so, and a lookup of it then
   moves it to the misses). FAILED when memory runs out, as snx_jit_resolve does */
static void requeue(SnxJitService* s, Entry* e, SnxU64 pc, int queue) {
    Region* r;
    if (push(&s->queues[queue], pc)) {
        e->state = FAILED;
        return;
    }
    e->state = queue == LIVE ? QUEUED : QUEUED_WARM;
    if (queue == WARM && (r = region_of(s, pc)) != NULL) clear_pending(r, pc);
    cond_signal(&s->work);
}

/* Compiles the batch of pcs (count of them by address, each IN_FLIGHT, stamped by the take stamp from queue), publishes
   the code and ends their entries: DONE with the code that enters at each, or FAILED. A batch of several that fails to
   emit or load is split, so one pc LLVM or the loader refuses fails alone */
/* Under lock: the images a batch reads, the config's and every added one, which stay readable until images_release
   (snx_jit_remove_image waits for it). NULL when memory runs out */
static SnxCodeImage* images_take(SnxJitService* s, size_t* count, unsigned char* taken) {
    SnxCodeImage* images =
        (SnxCodeImage*)malloc((s->config.image_count + SNX_JIT_IMAGES) * sizeof(*images) + sizeof(*images));
    size_t n = s->config.image_count;
    if (!images) return NULL;
    memcpy(images, s->config.images, s->config.image_count * sizeof(*images));
    for (size_t i = 1; i <= SNX_JIT_IMAGES; ++i) {
        taken[i] = s->regions[i].hi != 0;
        if (!taken[i]) continue;
        images[n++] = s->regions[i].image;
        ++s->regions[i].users;
    }
    *count = n;
    return images;
}
static void images_release(SnxJitService* s, SnxCodeImage* images, const unsigned char* taken) {
    lock(&s->lock);
    for (size_t i = 1; i <= SNX_JIT_IMAGES; ++i)
        if (taken[i]) --s->regions[i].users;
    cond_broadcast(&s->released);
    unlock(&s->lock);
    free(images);
}

static void compile(SnxJitService* s, const SnxU64* pcs, size_t count, SnxU64 stamp, int queue) {
    SnxCodegen codegen;
    SnxJitReport report = {0};
    unsigned char taken[1 + SNX_JIT_IMAGES];
    unsigned char* object = NULL;
    size_t size = 0, failed = 0, image_count = 0;
    char error[512], name[48];
    SnxU64 table = 0, *left = NULL;
    report.pc = pcs[0];
    report.pcs = count;
    report.seconds = snx_seconds();
    error[0] = 0;
    Batch batch;
    batch.service = s;
    batch.pcs = pcs;
    batch.count = count;
    lock(&s->lock);
    SnxCodeImage* images = images_take(s, &image_count, taken);
    unlock(&s->lock);
    if (images && s->cache.capacity && (left = (SnxU64*)malloc(count * sizeof(*left))) != NULL) {
        /* what the cache has: its units loaded and published, and the pcs they covered done */
        lock(&s->load_lock);
        for (size_t i = 0; i < count; ++i) {
            CacheRecord* record = cache_find(&s->cache, pcs[i]);
            if (!record || record->state < 0) continue;
            /* a pc of a record loaded before is missed again when its code was invalidated since (a mapping or
               permission change over it, which a mod loader's patching makes): the record is checked and loaded
               again, not compiled. Compiling those took most of a cached run's waits, and appended their units again */
            lock(&s->lock);
            Entry* e = find(s, pcs[i]);
            int done = e && e->state == DONE && e->code;
            unlock(&s->lock);
            if (!done) cache_load(s, record, images, image_count);
        }
        unlock(&s->load_lock);
        lock(&s->lock);
        size_t j = 0;
        for (size_t i = 0; i < count; ++i) {
            Entry* e = find(s, pcs[i]);
            if (!e || e->state != DONE || !e->code) left[j++] = pcs[i];
        }
        unlock(&s->lock);
        if (!j) {
            free(left);
            images_release(s, images, taken);
            return;
        }
        pcs = left;
        count = j;
        batch.pcs = pcs;
        batch.count = count;
        report.pc = pcs[0];
        report.pcs = count;
    }
    lock(&s->lock);
    /* memory changing after the code is read makes this batch's code stale: the publish skips it */
    SnxU64 epoch = s->epoch;
    unlock(&s->lock);
    size_t region_limit = s->config.region_instructions, overflow_limit = 0;
    if (!region_limit) {
        region_limit = SNX_JIT_REGION_INSTRUCTIONS;
        overflow_limit = SNX_JIT_OVERFLOW_INSTRUCTIONS;
    }
    if (region_limit == SIZE_MAX) region_limit = 0;
    size_t budget = s->config.budget;
    size_t live = s->config.live_budget ? s->config.live_budget : SNX_JIT_LIVE_BUDGET;
    if (queue == LIVE && budget > live) budget = live;
    int rc = images ? snx_codegen_discover(&codegen, images, image_count, pcs, count, known, &batch, budget,
                                           s->config.features, region_limit, overflow_limit)
                    : -1;
    if (!images) codegen = (SnxCodegen){0};
    int discovered = !rc;
    if (rc) snprintf(error, sizeof(error), "no guest code to compile at %llx", (unsigned long long)pcs[0]);
    for (size_t i = 0; !rc && i < codegen.function_count; ++i) report.instructions += codegen.functions[i].count;
    report.functions = codegen.function_count;
    if (!rc) {
        SnxU64 address;
        lock(&s->load_lock);
        /* unique in the loader's table, the cache's units from earlier runs included */
        do sprintf(name, "snx_jit_unit_%s_%lu", s->run, ++s->units);
        while (snx_code_lookup(s->loader, name, &address));
        unlock(&s->load_lock);
        rc = emit_unit(s, &codegen, name, &object, &size, error, sizeof(error));
    }
    if (!rc) {
        SnxLinkInput input;
        const char* names[1];
        input.data = object;
        input.size = size;
        input.name = name;
        names[0] = name;
        /* the code is complete and visible to instruction fetch when this returns: its addresses may go to any
           thread from here (code_loader.h) */
        lock(&s->load_lock);
        size_t used = snx_code_used(s->loader);
        rc = snx_code_load(s->loader, &input, 1, names, 1, &table, error, sizeof(error));
        report.bytes = snx_code_used(s->loader) - used;
        if (!rc) cache_append(s, &codegen, name, object, size, images, image_count);
        unlock(&s->load_lock);
    }
    free(object);
    if (rc && discovered && count > 1) {
        snx_codegen_free(&codegen);
        if (images) images_release(s, images, taken);
        for (size_t i = 0; i < count; ++i) compile(s, &pcs[i], 1, stamp, queue);
        free(left);
        return;
    }
    /* publish: under the lock, so snx_jit_invalidate's clear cannot fall between a slot's CAS and it, and only when
       memory has not changed since the batch read it. Every leader's slot that is still empty, with release ordering,
       so a thread that loads the address (relaxed, cpu.h: x86-64 keeps loads in order) reaches the code and data
       written before it. A slot something else filled meanwhile (another batch, a wrapper) keeps what it holds */
    lock(&s->lock);
    if (epoch == s->epoch) {
        for (size_t i = 0; !rc && i < codegen.function_count; ++i) {
            const SnxFunction* f = &codegen.functions[i];
            GuestFn code = unit_function(table, i);
            for (size_t j = 0; j < f->count; ++j) {
                if (!f->leaders[j]) continue;
                SnxU64 address = snx_function_address(f, j);
                /* an added region being removed has none: its arrays stay until users is 0 */
                Region* r = region_of(s, address);
                if (!r) continue;
                Entry* e = add(s, address, DONE);
                if (e && e->state == GONE) continue; /* invalidated while this batch compiled */
                GuestFn expected = NULL;
                __atomic_compare_exchange_n(slot(r, address), &expected, code, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
                /* a queued pc this batch compiled is done (its queue skips it); one another batch compiles is that
                   batch's to end, as this batch's own pcs are below, and an earlier batch's keeps its code */
                if (!e || e->state == IN_FLIGHT || e->state == FAILED || (e->state == DONE && e->code)) continue;
                e->state = DONE;
                e->code = code;
            }
        }
        if (!rc) span_of_batch(s, &codegen, table);
    }
    int dropped = epoch != s->epoch;
    for (size_t i = 0; i < count; ++i) {
        Entry* e = find(s, pcs[i]);
        const SnxFunction* f = rc ? NULL : snx_codegen_entry(&codegen, pcs[i]);
        GuestFn code = f ? unit_function(table, (size_t)(f - codegen.functions)) : NULL;
        /* a pc another batch took since (invalidated and missed again) is that batch's to end */
        if (e && e->state == IN_FLIGHT && e->taken != stamp) continue;
        if (dropped) {
            /* memory changed since the code was read, so the batch published nothing. A pc of its own still
               IN_FLIGHT wasn't in the range that changed (snx_jit_invalidate makes those GONE), but the code read with
               it may have been: it goes back to its queue to be compiled from what is there now. Leaving it IN_FLIGHT,
               or GONE with its pending bit set, would leave it uncompiled for good */
            if (e && e->state == IN_FLIGHT) requeue(s, e, pcs[i], queue);
            continue;
        }
        /* its image went while the batch compiled (snx_jit_remove_image clears hi under this lock): gone, not failed */
        if (!code && !in_text(s, pcs[i]) && !region_of(s, pcs[i])) {
            if (e) {
                e->state = GONE;
                e->code = NULL;
            }
            continue;
        }
        if (e) {
            if (e->state == GONE || e->state == QUEUED) continue; /* invalidated: the next lookup compiles it */
            e->state = code ? DONE : FAILED;
            e->code = code;
        }
        if (code)
            ++s->stats.compiled;
        else
            ++failed;
    }
    report.seconds = snx_seconds() - report.seconds;
    report.dropped = dropped;
    ++s->stats.batches;
    s->stats.failures += (unsigned long)failed;
    if (dropped) {
        ++s->stats.dropped;
        s->stats.queued = (unsigned long)(s->queues[LIVE].count + s->queues[WARM].count);
    }
    if (!rc) {
        if (!dropped) { /* a dropped batch's functions are thrown away, but its code still takes the arena */
            s->stats.functions += (unsigned long)report.functions;
            s->stats.instructions += (unsigned long)report.instructions;
        }
        s->stats.bytes += report.bytes;
    }
    s->stats.seconds += report.seconds;
    cond_broadcast(&s->done); /* snx_jit_wait's threads */
    unlock(&s->lock);
    if (failed && !error[0])
        snprintf(error, sizeof(error), "%lu of the batch's pcs are not where their code was found",
                 (unsigned long)failed);
    report.error = failed ? error : NULL;
    if (s->config.report) s->config.report(&report);
    snx_codegen_free(&codegen);
    if (images) images_release(s, images, taken);
    free(left);
}

/* Under lock: up to max pcs of the first queue that has any into pcs, made IN_FLIGHT and stamped with this take, which
   goes in *taken, and the queue they came from in *queue; 0 when both are empty. Skips the pcs a batch compiled while
   they waited */
static size_t take(SnxJitService* s, SnxU64* pcs, SnxU64* taken, int* queue) {
    size_t n = 0;
    *taken = ++s->takes;
    for (int q = LIVE; q < QUEUES && !n; ++q) {
        *queue = q;
        size_t max = q == LIVE ? s->config.batch : (s->config.batch + 7) / 8;
        while (n < max && s->queues[q].count) {
            SnxU64 pc = pop(&s->queues[q]);
            Entry* e = find(s, pc);
            int state = q == LIVE ? QUEUED : QUEUED_WARM;
            /* compiled since as another batch's leader (whatever its slot holds: a wrapper isn't code for it), or moved
               to the misses' queue */
            if (!e || e->state != state) continue;
            e->state = IN_FLIGHT;
            e->taken = *taken;
            set_pending(s, pc);
            pcs[n++] = pc;
        }
    }
    s->stats.queued = (unsigned long)(s->queues[LIVE].count + s->queues[WARM].count);
    return n;
}

/* A compile thread: batch after batch until the service stops. A batch that leaves pcs queued wakes another thread
   for them */
static void worker(SnxJitService* s) {
    SnxU64* pcs = (SnxU64*)malloc(s->config.batch * sizeof(*pcs));
    /* below the guest threads: when the host has no idle CPU, a compile waits, not the game */
#if defined(__linux__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    lock(&s->lock);
    while (pcs && !s->stop) {
        SnxU64 taken;
        int queue;
        size_t n = take(s, pcs, &taken, &queue);
        if (!n) {
            if (!s->busy) cond_broadcast(&s->idle);
            cond_wait(&s->work, &s->lock);
            continue;
        }
        ++s->busy;
        if (s->queues[LIVE].count || s->queues[WARM].count) cond_signal(&s->work);
        unlock(&s->lock);
        qsort(pcs, n, sizeof(*pcs), snx_compare_u64);
        compile(s, pcs, n, taken, queue);
        lock(&s->lock);
        --s->busy;
    }
    if (!s->busy) cond_broadcast(&s->idle);
    unlock(&s->lock);
    free(pcs);
}

#ifdef _WIN32
static DWORD WINAPI worker_main(LPVOID s) {
    worker((SnxJitService*)s);
    return 0;
}
static int thread_start(JitThread* t, SnxJitService* s) {
    *t = CreateThread(NULL, 0, worker_main, s, 0, NULL);
    return *t ? 0 : -1;
}
static void thread_join(JitThread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
static void* worker_main(void* s) {
    worker((SnxJitService*)s);
    return NULL;
}
static int thread_start(JitThread* t, SnxJitService* s) {
    int failed = pthread_create(t, NULL, worker_main, s);
    if (failed) errno = failed; /* the reason, for what the caller says */
    return failed ? -1 : 0;
}
static void thread_join(JitThread t) { pthread_join(t, NULL); }
#endif

/* ---- the interface ---- */

int snx_jit_covers(const SnxJitService* s, SnxU64 pc) {
    Region* r = region_of(s, pc);
    return in_text(s, pc) || (r && r != &s->regions[0]);
}

GuestFn snx_jit_resolve(SnxJitService* s, SnxU64 pc) {
    GuestFn code = NULL;
    Region* r = region_of(s, pc);
    /* an added image's table is the service's alone: its code is found here, without the lock (the main table's slot
       is empty, or the caller's lookup would not have missed) */
    if (r && r != &s->regions[0] && (code = __atomic_load_n(slot(r, pc), __ATOMIC_ACQUIRE)) != NULL) return code;
    if (!in_text(s, pc) && !(r && r != &s->regions[0])) return NULL;
    if (r && is_pending(r, pc) && !__atomic_load_n(slot(r, pc), __ATOMIC_ACQUIRE)) return NULL;
    lock(&s->lock);
    Entry* e = find(s, pc);
    if (!e || e->state == QUEUED_WARM || e->state == GONE) {
        if (!e) e = add(s, pc, QUEUED);
        if (e && !push(&s->queues[LIVE], pc)) {
            e->state = QUEUED;
            set_pending(s, pc);
            s->stats.queued = (unsigned long)(s->queues[LIVE].count + s->queues[WARM].count);
            cond_signal(&s->work);
        } else if (e && e->state == QUEUED)
            e->state = FAILED; /* in no queue: memory ran out */
    } else if (e->state == DONE)
        code = e->code;
    unlock(&s->lock);
    return code;
}

size_t snx_jit_warm(SnxJitService* s, const SnxU64* pcs, size_t count) {
    size_t queued = 0;
    lock(&s->lock);
    for (size_t i = 0; i < count; ++i) {
        if (!in_text(s, pcs[i]) || find(s, pcs[i])) continue;
        Region* r = region_of(s, pcs[i]);
        if (r && __atomic_load_n(slot(r, pcs[i]), __ATOMIC_RELAXED)) continue;
        Entry* e = add(s, pcs[i], QUEUED_WARM);
        if (!e) break;
        if (push(&s->queues[WARM], pcs[i])) {
            e->state = FAILED;
            break;
        }
        ++queued;
    }
    s->stats.queued = (unsigned long)(s->queues[LIVE].count + s->queues[WARM].count);
    cond_signal(&s->work);
    unlock(&s->lock);
    return queued;
}

void snx_jit_drain(SnxJitService* s) {
    lock(&s->lock);
    while (s->queues[LIVE].count || s->queues[WARM].count || s->busy) cond_wait(&s->idle, &s->lock);
    unlock(&s->lock);
}

GuestFn snx_jit_wait(SnxJitService* s, SnxU64 pc) {
    GuestFn code = snx_jit_resolve(s, pc); /* queued in the live misses' queue, if it wasn't */
    if (code) return code;
    lock(&s->lock);
    for (;;) {
        /* found again each time: the table moves when it grows */
        Entry* e = find(s, pc);
        if (!e || e->state == FAILED) break; /* outside the images, or uncompilable */
        if (e->state == DONE) {
            code = e->code;
            break;
        }
        if (e->state == QUEUED_WARM || e->state == GONE) { /* in the warm queue only, or invalidated: queued to compile */
            unlock(&s->lock);
            if ((code = snx_jit_resolve(s, pc)) != NULL) return code;
            lock(&s->lock);
            /* resolve queues a gone pc the images still hold; one they don't any more stays gone: uncompilable */
            e = find(s, pc);
            if (!e || e->state == GONE) break;
            continue;
        }
        cond_wait(&s->done, &s->lock);
    }
    unlock(&s->lock);
    return code;
}

/* under lock: chunk i's part of [lo, hi), page-aligned */
static void region_chunk_range(SnxU64 lo, SnxU64 hi, size_t i, SnxU64* from, SnxU64* to) {
    SnxU64 base = (SnxU64)i << REGION_TOP_SHIFT, end = base + (1u << REGION_TOP_SHIFT);
    *from = (lo > base ? lo : base) & ~(SnxU64)0xFFF;
    *to = hi < end ? hi : end;
}

/* under lock: the top has an entry for chunk last */
static int region_top_cover(SnxJitService* s, size_t last) {
    RegionTop* top = s->region_top;
    size_t count = top ? top->count : 0;
    if (last < count) return 0;
    size_t n = count ? count : 16;
    while (n <= last) n *= 2;
    RegionTop* grown = (RegionTop*)calloc(1, sizeof(*grown) + n * sizeof(*grown->leaves));
    if (!grown) return -1;
    if (top) memcpy(grown->leaves, top->leaves, count * sizeof(*grown->leaves));
    grown->count = n;
    grown->older = top;
    __atomic_store_n(&s->region_top, grown, __ATOMIC_RELEASE); /* readers may hold the old top until free */
    return 0;
}

/* under lock: the pages of [lo, hi) read as the region at index (regions, 1-based). A leaf or top the allocation can't
   carve out leaves those pages to region_of's scan */
static void region_map_add(SnxJitService* s, SnxU64 lo, SnxU64 hi, unsigned index) {
    size_t first = (size_t)(lo >> REGION_TOP_SHIFT), last = (size_t)((hi - 1) >> REGION_TOP_SHIFT);
    if (region_top_cover(s, last)) return;
    for (size_t i = first; i <= last; ++i) {
        RegionTop* top = s->region_top;
        unsigned short* leaf = __atomic_load_n(&top->leaves[i], __ATOMIC_RELAXED);
        if (!leaf) {
            leaf = (unsigned short*)calloc(REGION_LEAF_ENTRIES, sizeof(*leaf));
            if (!leaf) continue; /* region_of scans for them */
            __atomic_store_n(&top->leaves[i], leaf, __ATOMIC_RELEASE);
        }
        SnxU64 from, to;
        region_chunk_range(lo, hi, i, &from, &to);
        for (SnxU64 pc = from; pc < to; pc += 0x1000)
            __atomic_store_n(&leaf[(pc >> REGION_PAGE_SHIFT) & (REGION_LEAF_ENTRIES - 1)], (unsigned short)index,
                             __ATOMIC_RELEASE);
    }
}

/* under lock: the pages of [lo, hi) read as no added region again */
static void region_map_remove(SnxJitService* s, SnxU64 lo, SnxU64 hi) {
    RegionTop* top = s->region_top;
    if (!top) return;
    size_t first = (size_t)(lo >> REGION_TOP_SHIFT);
    size_t last = (size_t)((hi - 1) >> REGION_TOP_SHIFT);
    if (first >= top->count) return;
    if (last >= top->count) last = top->count - 1;
    for (size_t i = first; i <= last; ++i) {
        unsigned short* leaf = __atomic_load_n(&top->leaves[i], __ATOMIC_RELAXED);
        if (!leaf) continue;
        SnxU64 from, to;
        region_chunk_range(lo, hi, i, &from, &to);
        for (SnxU64 pc = from; pc < to; pc += 0x1000)
            __atomic_store_n(&leaf[(pc >> REGION_PAGE_SHIFT) & (REGION_LEAF_ENTRIES - 1)], (unsigned short)0,
                             __ATOMIC_RELEASE);
    }
}

int snx_jit_add_image(SnxJitService* s, const SnxCodeImage* image) {
    SnxU64 lo = image->base + image->text_off, hi = lo + image->text_size;
    size_t n = (size_t)(image->text_size / 4);
    Region* r = NULL;
    if (!image->text_size || (lo & 3) || hi < lo || n > SIZE_MAX / sizeof(GuestFn)) return -1;
    GuestFn* slots = (GuestFn*)calloc(n, sizeof(*slots));
    unsigned char* pending = (unsigned char*)calloc(n / 8 + 1, 1);
    if (!slots || !pending) {
        free(slots);
        free(pending);
        return -1;
    }
    lock(&s->lock);
    if (lo < s->regions[0].hi && s->regions[0].lo < hi) r = &s->regions[0]; /* over the main table: refused */
    for (size_t i = 1; r != &s->regions[0] && i <= SNX_JIT_IMAGES; ++i)
        if (!s->regions[i].hi) {
            if (!r) r = &s->regions[i];
        } else if (lo < s->regions[i].hi && s->regions[i].lo < hi)
            r = &s->regions[0];
    if (!r || r == &s->regions[0]) {
        unlock(&s->lock);
        free(slots);
        free(pending);
        return -1;
    }
    r->slots = slots;
    r->pending = pending;
    r->image = *image;
    __atomic_store_n(&r->lo, lo, __ATOMIC_RELAXED);
    __atomic_store_n(&r->hi, hi, __ATOMIC_RELEASE); /* last: region_of finds the region complete */
    region_map_add(s, lo, hi, (unsigned)(r - s->regions));
    unlock(&s->lock);
    return 0;
}

void snx_jit_remove_image(SnxJitService* s, SnxU64 base) {
    Region* r = NULL;
    lock(&s->lock);
    for (size_t i = 1; i <= SNX_JIT_IMAGES && !r; ++i)
        if (s->regions[i].hi && s->regions[i].image.base == base) r = &s->regions[i];
    if (!r) {
        unlock(&s->lock);
        return;
    }
    SnxU64 lo = r->lo;
    SnxU64 hi = r->hi;
    region_map_remove(s, lo, hi); /* its pages read as no region again */
    __atomic_store_n(&r->hi, 0, __ATOMIC_RELEASE); /* no batch takes the image, no publish finds the table */
    while (r->users) cond_wait(&s->released, &s->lock);
    /* its pcs are new to the service again: queued ones are skipped when taken, compiled ones forgotten */
    for (size_t i = 0; i < s->entry_capacity; ++i) {
        SnxU64 pc = s->entries[i].key & ~(SnxU64)1;
        if (!s->entries[i].key || pc < lo || pc >= hi) continue;
        s->entries[i].state = GONE;
        s->entries[i].code = NULL;
    }
    cond_broadcast(&s->done); /* a thread waiting for one of them gets NULL */
    GuestFn* slots = r->slots;
    unsigned char* pending = r->pending;
    r->slots = NULL;
    r->pending = NULL;
    r->image = (SnxCodeImage){0};
    unlock(&s->lock);
    free(slots);
    free(pending);
}

void snx_jit_invalidate_ranges(SnxJitService* s, SnxU64 addr, SnxU64 size, SnxJitInvalidatedRange callback,
                               void* context) {
    typedef struct { SnxU64 start, end; } InvalidatedRange;
    SnxU64 end = addr + size;
    size_t range_count = 0;
    InvalidatedRange* ranges = NULL;
    int ranges_incomplete = 0;
    if (!size || end < addr) return;
    lock(&s->lock);
    size_t range_capacity = s->span_count;
    if (callback && range_capacity) ranges = (InvalidatedRange*)calloc(range_capacity, sizeof(*ranges));
    if (callback && range_capacity && !ranges) ranges_incomplete = 1;
    /* a batch compiling now reads s->epoch before its code, and skips the publish when it has changed */
    ++s->epoch;
    if ((end - addr) / 4 < s->entry_capacity / 8) { /* a few lines (IC IVAU's 64 bytes): each pc looked up */
        for (SnxU64 pc = addr & ~(SnxU64)3; pc < end; pc += 4) {
            Entry* e = find(s, pc);
            if (e != NULL) {
                e->state = GONE;
                e->code = NULL;
            }
        }
    } else
        for (size_t i = 0; i < s->entry_capacity; ++i) {
            Entry* e = &s->entries[i];
            SnxU64 pc = e->key & ~(SnxU64)1;
            if (e->key && pc >= addr && pc < end) {
                e->state = GONE;
                e->code = NULL;
            }
        }
    for (size_t j = 0; j <= SNX_JIT_IMAGES; ++j) {
        Region* r = &s->regions[j];
        if (!r->hi || r->hi <= addr || r->lo >= end) continue; /* free, or outside the range */
        SnxU64 from = ((addr > r->lo ? addr : r->lo) + 3) & ~(SnxU64)3;
        SnxU64 to = end < r->hi ? end : r->hi;
        for (SnxU64 pc = from; pc < to; pc += 4) {
            __atomic_store_n(slot(r, pc), NULL, __ATOMIC_RELEASE);
            clear_pending(r, pc); /* its pc is new to the service again: discovery isn't told a batch has it */
        }
    }
    /* a unit whose code the change touches goes whole: its leaders outside the range too, as its functions call the
       ones inside directly. Its code stays in the arena, where a thread may still be running it */
    for (size_t i = 0; i < s->span_count;) {
        UnitSpan* u = &s->spans[i];
        if (u->hi <= addr || u->lo >= end) {
            ++i;
            continue;
        }
        if (callback) {
            if (ranges && range_count < range_capacity) {
                ranges[range_count].start = u->lo;
                ranges[range_count++].end = u->hi;
            } else {
                ranges_incomplete = 1;
            }
        }
        for (size_t j = 0; j < u->count; ++j) {
            GuestFn expected = u->leaders[j].code;
            Entry* e = find(s, u->leaders[j].pc);
            if (e && e->code == u->leaders[j].code) {
                e->state = GONE;
                e->code = NULL;
            }
            Region* r = region_of(s, u->leaders[j].pc);
            if (r && __atomic_compare_exchange_n(slot(r, u->leaders[j].pc), &expected, NULL, 0, __ATOMIC_RELEASE,
                                                 __ATOMIC_RELAXED))
                clear_pending(r, u->leaders[j].pc);
        }
        free(u->leaders);
        s->spans[i] = s->spans[--s->span_count];
    }
    cond_broadcast(&s->done); /* a thread waiting for one of them resolves it again */
    unlock(&s->lock);
    if (callback) {
        if (ranges_incomplete) {
            /* A coarse range is safe and avoids leaving a caller unaware of retired JIT code on OOM. */
            callback(context, addr, end);
        } else {
            for (size_t i = 0; i < range_count; ++i) callback(context, ranges[i].start, ranges[i].end);
        }
    }
    free(ranges);
}

void snx_jit_invalidate(SnxJitService* s, SnxU64 addr, SnxU64 size) {
    snx_jit_invalidate_ranges(s, addr, size, NULL, NULL);
}

/* the CPUs the process may run on: its affinity mask where the host has one (a game started under taskset gets the
   CPUs taskset gave it), else every online CPU */
static unsigned usable_cpus(void) {
#ifdef __linux__
    cpu_set_t set;
    if (!sched_getaffinity(0, sizeof(set), &set) && CPU_COUNT(&set) > 0) return (unsigned)CPU_COUNT(&set);
#endif
    return snx_cpu_count();
}
/* the compile threads when the config leaves it open: every CPU the process may use but two, from one up to
   SNX_JIT_THREADS. They run below the game's threads (compile_thread lowers their priority), so a compile only gets a
   CPU the guest threads, the GPU thread and audio leave idle. Keeping six CPUs for the game left one compile thread on
   hosts with up to 7 CPUs and two on 8, which fell minutes behind Ender Magnolia's misses */
static unsigned default_threads(void) {
    unsigned cpus = usable_cpus();
    return cpus >= SNX_JIT_THREADS + 2 ? SNX_JIT_THREADS : cpus > 3 ? cpus - 2 : 1;
}

SnxJitService* snx_jit_service_new(const SnxJitConfig* config, char* error, size_t error_size) {
    for (size_t i = 1; i < config->aot_count; ++i)
        if (config->aot_addresses[i - 1] >= config->aot_addresses[i]) {
            snprintf(error, error_size, "the AOT entries are not in ascending order");
            return NULL;
        }
    if (snx_ir_init(error, error_size)) return NULL;
    SnxJitService* s = (SnxJitService*)calloc(1, sizeof(*s));
    if (!s) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    s->config = *config;
    if (!s->config.budget) s->config.budget = SNX_JIT_BUDGET;
    if (!s->config.batch) s->config.batch = SNX_JIT_BATCH;
    if (!s->config.opt) s->config.opt = '0';
    if (!s->config.threads) s->config.threads = default_threads();
    if (s->config.threads > SNX_JIT_THREADS) s->config.threads = SNX_JIT_THREADS;
    size_t bits = s->config.code_hi > s->config.code_lo ? (size_t)((s->config.code_hi - s->config.code_lo) >> 2) : 0;
    s->regions[0].lo = s->config.code_lo;
    s->regions[0].hi = s->config.code_hi > s->config.code_lo ? s->config.code_hi : s->config.code_lo;
    s->regions[0].slots = s->config.dispatch;
    s->regions[0].pending = (unsigned char*)calloc(bits / 8 + 1, 1);
#if defined(__powerpc64__)
    if (s->regions[0].pending && s->config.aot_count)
        s->loader = snx_code_loader_new_toc(0, aot_toc(&s->config), error, error_size);
    else
#endif
        s->loader = s->regions[0].pending ? snx_code_loader_new(0, error, error_size) : NULL;
    if (!s->loader) {
        if (!s->regions[0].pending) snprintf(error, error_size, "out of memory");
        free(s->regions[0].pending);
        free(s);
        return NULL;
    }
    snx_code_set_resolver(s->loader, resolve_aot, s);
    SnxLinkInput input;
    input.data = snx_rtlib_object;
    input.size = snx_rtlib_object_size;
    input.name = "rtlib";
    if (snx_code_load(s->loader, &input, 1, NULL, 0, NULL, error, error_size)) {
        snx_code_loader_free(s->loader);
        free(s->regions[0].pending);
        free(s);
        return NULL;
    }
    s->stats.bytes = snx_code_used(s->loader);
    /* this run's part of unit names: two runs that share a cache name their units apart (compile's loop makes sure) */
    {
        SnxU64 seed = (SnxU64)time(NULL) << 20 ^ (SnxU64)clock() ^ (SnxU64)(size_t)s ^ (SnxU64)(size_t)&seed;
        seed = snx_fnv1a(SNX_FNV_BASIS, (const unsigned char*)&seed, sizeof(seed));
        sprintf(s->run, "%016llx", (unsigned long long)seed);
    }
    if (s->config.cache_path) cache_open(s);
    lock_init(&s->lock);
    lock_init(&s->load_lock);
    cond_init(&s->work);
    cond_init(&s->idle);
    cond_init(&s->released);
    cond_init(&s->done);
    size_t i;
    for (i = 0; i < s->config.threads; ++i)
        if (thread_start(&s->threads[i], s)) break;
    s->stats.threads = (unsigned)i;
    if (!i) {
        char why[256];
        snprintf(error, error_size, "cannot start a compile thread: %s", snx_os_error(why, sizeof why));
        snx_jit_service_free(s);
        return NULL;
    }
    return s;
}

void snx_jit_service_free(SnxJitService* s) {
    if (!s) return;
    lock(&s->lock);
    s->stop = 1;
    cond_broadcast(&s->work);
    unlock(&s->lock);
    for (unsigned i = 0; i < s->stats.threads; ++i) thread_join(s->threads[i]);
    cache_close(&s->cache);
    snx_code_loader_free(s->loader);
    for (size_t i = 0; i < s->span_count; ++i) free(s->spans[i].leaders);
    free(s->spans);
    free(s->entries);
    for (int q = 0; q < QUEUES; ++q) free(s->queues[q].pcs);
    for (unsigned i = 0; i <= SNX_JIT_IMAGES; ++i) {
        free(s->regions[i].pending);
        if (i) free(s->regions[i].slots);
    }
    /* the map's leaves are shared by the tops it grew through: free them from the newest, then the tops themselves */
    {
        RegionTop* top = s->region_top;
        if (top)
            for (size_t j = 0; j < top->count; ++j) free(top->leaves[j]);
        while (top) {
            RegionTop* older = top->older;
            free(top);
            top = older;
        }
    }
    cond_free(&s->work);
    cond_free(&s->idle);
    cond_free(&s->released);
    cond_free(&s->done);
    lock_free(&s->lock);
    lock_free(&s->load_lock);
    free(s);
}

void snx_jit_stats(SnxJitService* s, SnxJitStats* stats) {
    lock(&s->lock);
    *stats = s->stats;
    unlock(&s->lock);
}
