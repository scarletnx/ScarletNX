/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT (jit_service.h). C90 in style; the target's C11 is for the LLVM-C headers and cpu.h's inline
   functions */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1 /* sched_getaffinity: the CPUs this process may run on */
#endif
#include "common/cpu/recompiler/llvm/jit_service.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/rtlib_object.h"
#include <llvm-c/TargetMachine.h>
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
#include <stdlib.h>
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
   batch compiles from it. FAILED: it can't be compiled (snx_jit_wait answers NULL), and isn't tried again. GONE: its image
   was removed (snx_jit_remove_image): the pc is new to the service if code turns up there again */
enum { QUEUED_WARM = 1, QUEUED, IN_FLIGHT, DONE, FAILED, GONE };
enum { LIVE, WARM, QUEUES }; /* the queues, by priority */

typedef struct {
    SnxU64 key; /* pc | 1 (a pc is 4-aligned), 0 for a free slot */
    GuestFn code;
    int state;
} Entry;

/* pcs to compile, first in first out: a ring that grows on its own */
typedef struct {
    SnxU64* pcs;
    size_t head, count, capacity; /* capacity: 0 or a power of two */
} Queue;

/* A dispatch table the service fills: the main one (config.dispatch over [code_lo, code_hi), regions[0]), and one per
   image added while the game runs (snx_jit_add_image: an NRO's text), whose slots snx_jit_resolve reads, as nothing
   else knows them. A free region has hi 0. lo and hi change under lock and are read without it (hi last, with acquire
   and release ordering), and an added region's arrays live until its removal, which waits for the batches reading its
   image (users). pending: one bit per instruction, set (under lock, never cleared) once the pc is in the misses'
   queue, compiling or failed. A guest thread that misses a pc with its bit set and its slot empty gets NULL without
   the lock: every miss after the first, until the code is published, costs two loads. It also tells discovery,
   without the lock, which pcs other batches have or will have, so a batch doesn't compile them too */
typedef struct {
    SnxU64 lo, hi;
    GuestFn* slots;
    unsigned char* pending;
    SnxCodeImage image; /* an added region's */
    unsigned users;     /* under lock: batches compiling with its image */
} Region;

/* The cache file (SnxJitConfig.cache_path) as this run read it: its records, and the leaders of their functions by pc
   for the batches to find. A record is one unit: its table's name, its functions (each its leaders, its instructions
   as runs of addresses and a hash of their words) and its object. The file: CACHE_MAGIC, the key's length and the key
   (what the objects depend on), then records, each RECORD_MAGIC, its body's length, the body and the body's hash, so
   a run that ended in the middle of a record leaves the ones before it readable */
typedef struct {
    const unsigned char* functions; /* the body's function list, in the buffer */
    const unsigned char* object;
    size_t object_size, function_count;
    char name[64];
    int state; /* 0 not loaded yet, 1 loaded, -1 refused (its code changed since, or the loader refused it) */
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
} Cache;

struct SnxJitService {
    SnxJitConfig config;
    SnxCodeLoader* loader;
    Region regions[1 + SNX_JIT_IMAGES];
    /* the pcs: the misses and warm pcs queued, compiling or failed, and every leader of every compiled function, for
       a pc outside the dispatch table or one whose slot holds something else (a wrapper). An open-addressing table,
       at most half full, under lock */
    Entry* entries;
    size_t entry_count, entry_capacity; /* capacity: a power of two */
    Queue queues[QUEUES];               /* under lock */
    unsigned busy;                      /* batches compiling, under lock */
    int stop;                           /* under lock: the compile threads end */
    SnxJitStats stats;                  /* under lock */
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
    size_t i;
    if (!s->entry_capacity) return NULL;
    for (i = entry_hash(key, s->entry_capacity); s->entries[i].key; i = (i + 1) & (s->entry_capacity - 1))
        if (s->entries[i].key == key) return &s->entries[i];
    return NULL;
}

/* pc's entry, added with state when it has none. NULL when memory runs out */
static Entry* add(SnxJitService* s, SnxU64 pc, int state) {
    SnxU64 key = pc | 1;
    Entry* e = find(s, pc);
    size_t i;
    if (e) return e;
    if ((s->entry_count + 1) * 2 > s->entry_capacity) {
        size_t capacity = s->entry_capacity ? s->entry_capacity * 2 : 1024, j;
        Entry* grown;
        if (capacity > ((size_t)-1) / sizeof(*grown)) return NULL;
        grown = (Entry*)calloc(capacity, sizeof(*grown));
        if (!grown) return NULL;
        for (j = 0; j < s->entry_capacity; ++j)
            if (s->entries[j].key) {
                i = entry_hash(s->entries[j].key, capacity);
                while (grown[i].key) i = (i + 1) & (capacity - 1);
                grown[i] = s->entries[j];
            }
        free(s->entries);
        s->entries = grown;
        s->entry_capacity = capacity;
    }
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
    SnxU64* grown;
    size_t capacity, i;
    if (q->count == q->capacity) {
        capacity = q->capacity ? q->capacity * 2 : 256;
        if (capacity > ((size_t)-1) / sizeof(*grown)) return -1;
        grown = (SnxU64*)malloc(capacity * sizeof(*grown));
        if (!grown) return -1;
        for (i = 0; i < q->count; ++i) grown[i] = q->pcs[(q->head + i) & (q->capacity - 1)];
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
    const SnxCodeImage* m;
    size_t i;
    for (i = 0; !(pc & 3) && i < s->config.image_count; ++i) {
        m = &s->config.images[i];
        if (pc >= m->base + m->text_off && pc - (m->base + m->text_off) < m->text_size) return 1;
    }
    return 0;
}

/* the region whose table has a slot for pc, or NULL */
static Region* region_of(const SnxJitService* s, SnxU64 pc) {
    Region* r = (Region*)&s->regions[0];
    SnxU64 hi;
    size_t i;
    if (pc & 3) return NULL;
    if (pc - r->lo < r->hi - r->lo) return r;
    for (i = 1; i <= SNX_JIT_IMAGES; ++i) {
        r = (Region*)&s->regions[i];
        hi = __atomic_load_n(&r->hi, __ATOMIC_ACQUIRE);
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
    size_t i;
    if (!r) return;
    i = (size_t)((pc - r->lo) >> 2);
    __atomic_fetch_or(&r->pending[i >> 3], (unsigned char)(1u << (i & 7)), __ATOMIC_RELAXED);
}

/* ---- the compile ---- */

/* a batch's pcs by address, for discovery's question */
typedef struct {
    SnxJitService* service;
    const SnxU64* pcs;
    size_t count;
} Batch;

static int in_batch(const Batch* b, SnxU64 pc) {
    size_t lo = 0, hi = b->count, middle;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
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
    Entry* e;
    int result;
    if (r) {
        if (__atomic_load_n(slot(r, address), __ATOMIC_RELAXED)) return 1;
        return is_pending(r, address) && !in_batch(b, address);
    }
    lock(&s->lock);
    e = find(s, address);
    result = e && e->state != QUEUED_WARM && e->state != GONE && !(e->state == IN_FLIGHT && in_batch(b, address));
    unlock(&s->lock);
    return result;
}

/* the AOT function to call for target, as a host address (SnxIr.direct), 0 when the AOT module has no entry there */
static SnxU64 direct(void* user, SnxU64 target) {
    const SnxJitService* s = (const SnxJitService*)user;
    size_t lo = 0, hi = s->config.aot_count, middle;
    while (lo < hi) {
        middle = lo + (hi - lo) / 2;
        if (s->config.aot_addresses[middle] < target)
            lo = middle + 1;
        else
            hi = middle;
    }
    if (lo == s->config.aot_count || s->config.aot_addresses[lo] != target || !s->config.aot_functions[lo]) return 0;
    return (SnxU64)(size_t)s->config.aot_functions[lo];
}

/* the loader's resolver: snx_aot_<target>, the AOT module's function for target (ir_codegen.c's call) */
static SnxU64 resolve_aot(void* user, const char* name) {
    SnxU64 target = 0;
    const char* p;
    if (strncmp(name, "snx_aot_", 8) || !name[8]) return 0;
    for (p = name + 8; *p; ++p) {
        int digit = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
        if (digit < 0) return 0;
        target = target << 4 | (SnxU64)digit;
    }
    return direct(user, target);
}

/* The unit's object: every function of codegen, internal to it, and table_name, an array of their addresses in
   codegen's order, its only global. Internal functions give two units the same F_<address> without a clash in the
   loader's table, where every global stays */
static int emit_unit(SnxJitService* s, SnxCodegen* codegen, const char* table_name, unsigned char** object,
                     size_t* size, char* error, size_t error_size) {
    SnxIr* ir = snx_ir_new("jit", s->config.opt, error, error_size);
    LLVMValueRef *functions, table;
    LLVMTypeRef type;
    size_t i;
    int rc;
    if (!ir) return -1;
    functions = (LLVMValueRef*)malloc(codegen->function_count * sizeof(*functions));
    if (!functions) {
        snx_ir_free(ir);
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    ir->direct = direct;
    ir->direct_user = s;
    codegen->hook_direct_calls = s->config.hook_direct_calls;
    for (i = 0; i < codegen->function_count; ++i) snx_ir_emit(ir, codegen, &codegen->functions[i]);
    for (i = 0; i < codegen->function_count; ++i) {
        functions[i] = snx_ir_guest_function(ir, codegen->functions[i].start);
        LLVMSetLinkage(functions[i], LLVMInternalLinkage);
        LLVMSetVisibility(functions[i], LLVMDefaultVisibility); /* a local symbol has no visibility */
    }
    type = LLVMArrayType(ir->t_ptr, (unsigned)codegen->function_count);
    table = LLVMAddGlobal(ir->mod, type, table_name);
    LLVMSetInitializer(table, LLVMConstArray(ir->t_ptr, functions, (unsigned)codegen->function_count));
    LLVMSetGlobalConstant(table, 1);
    LLVMSetVisibility(table, LLVMHiddenVisibility);
    free(functions);
    rc = snx_ir_object(ir, s->config.opt, object, size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

/* function i's code in a loaded unit's table */
static GuestFn unit_function(SnxU64 table, size_t i) {
    GuestFn code;
    memcpy(&code, (const unsigned char*)(size_t)table + i * sizeof(code), sizeof(code));
    return code;
}

/* ---- the cache (Cache) ---- */

#define CACHE_MAGIC  "SNXJIT01"
#define RECORD_MAGIC 0x524A4E53u         /* "SNJR" */
#define CACHE_MAX    ((size_t)256 << 20) /* a larger file starts over */

static SnxU32 get32(const unsigned char* p) {
    return (SnxU32)p[0] | (SnxU32)p[1] << 8 | (SnxU32)p[2] << 16 | (SnxU32)p[3] << 24;
}
static SnxU64 get64(const unsigned char* p) { return get32(p) | (SnxU64)get32(p + 4) << 32; }

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
    b[0] = (unsigned char)v;
    b[1] = (unsigned char)(v >> 8);
    b[2] = (unsigned char)(v >> 16);
    b[3] = (unsigned char)(v >> 24);
    out_bytes(o, b, 4);
}
static void out64(Out* o, SnxU64 v) {
    out32(o, (SnxU32)v);
    out32(o, (SnxU32)(v >> 32));
}

/* FNV-1a over bytes, from basis: the body's check, the runtime library's identity */
static SnxU64 fnv(const unsigned char* p, size_t n, SnxU64 basis) {
    size_t i;
    for (i = 0; i < n; ++i) basis = (basis ^ p[i]) * (SnxU64)0x100000001B3ULL;
    return basis;
}

/* the instruction word at address in the images: 0, or -1 outside their text */
static int image_word(const SnxCodeImage* images, size_t count, SnxU64 address, SnxU32* word) {
    size_t i;
    for (i = 0; i < count; ++i) {
        const SnxCodeImage* m = &images[i];
        SnxU64 start = m->base + m->text_off, off = address - m->base;
        if (address < start || address - start + 4 > m->text_size || off + 4 > m->image_size) continue;
        *word = get32(m->image + (size_t)off);
        return 0;
    }
    return -1;
}

/* Two 64-bit hashes of a function's instructions, each address with its word: 0, or -1 when one is outside the
   images. runs: count pairs of (start, words) */
static int hash_runs(const SnxCodeImage* images, size_t image_count, const unsigned char* runs, size_t count,
                     SnxU64* h1, SnxU64* h2) {
    SnxU64 a = 0xCBF29CE484222325ULL, b = 0x84222325CBF29CE4ULL, address;
    SnxU32 word, n, k;
    size_t i;
    for (i = 0; i < count; ++i) {
        address = get64(runs + 12 * i);
        n = get32(runs + 12 * i + 8);
        for (k = 0; k < n; ++k, address += 4) {
            SnxU64 v;
            if (image_word(images, image_count, address, &word)) return -1;
            v = address << 32 ^ word;
            a = (a ^ v) * (SnxU64)0x100000001B3ULL;
            b = (b ^ (v * (SnxU64)0x9E3779B97F4A7C15ULL)) * (SnxU64)0xC2B2AE3D27D4EB4FULL;
        }
    }
    *h1 = a;
    *h2 = b;
    return 0;
}

/* What the objects depend on beyond the code: the ABI, LLVM, the runtime library, the host CPU and its features (the
   code is compiled for them), the -O level and hooked direct calls */
static char* cache_key(const SnxJitService* s) {
    char *cpu = LLVMGetHostCPUName(), *features = LLVMGetHostCPUFeatures(), *key;
    size_t n = strlen(cpu) + strlen(features) + 160;
    key = (char*)malloc(n);
    if (key)
        sprintf(key, "abi=%d llvm=%s rtlib=%08lx%08lx cpu=%s opt=%c hook=%d features=%s", SNX_ABI, LLVM_VERSION_STRING,
                (unsigned long)(fnv(snx_rtlib_object, snx_rtlib_object_size, 0xCBF29CE484222325ULL) >> 32),
                (unsigned long)(fnv(snx_rtlib_object, snx_rtlib_object_size, 0xCBF29CE484222325ULL) & 0xFFFFFFFFu), cpu,
                s->config.opt, s->config.hook_direct_calls, features);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    return key;
}

/* the records of buffer (size bytes after the header), parsed until the first that isn't whole: their count, and the
   bytes they take */
static size_t cache_parse(Cache* c, size_t start, size_t size, size_t* used) {
    size_t at = start, count = 0, f;
    while (at + 12 <= size) {
        SnxU64 length = get64(c->buffer + at + 4);
        const unsigned char *body = c->buffer + at + 12, *p;
        CacheRecord* r;
        size_t name_length;
        if (get32(c->buffer + at) != RECORD_MAGIC || length > size - at - 12 || size - at - 12 - length < 8 ||
            fnv(body, (size_t)length, 0xCBF29CE484222325ULL) != get64(body + length))
            break;
        r = &c->records[count];
        p = body;
        name_length = p[0] | (size_t)p[1] << 8;
        if (name_length >= sizeof(r->name) || 2 + name_length + 4 > length) break;
        memcpy(r->name, p + 2, name_length);
        r->name[name_length] = 0;
        p += 2 + name_length;
        r->function_count = get32(p);
        p += 4;
        r->functions = p;
        /* walk the functions to the object: each is start, leaders, runs, two hashes */
        for (f = 0; f < r->function_count; ++f) {
            size_t leaders, runs;
            if ((size_t)(p - body) + 12 > length) break;
            leaders = get32(p + 8);
            if (leaders > (length - (size_t)(p - body) - 12) / 8) break;
            p += 12 + 8 * leaders;
            if ((size_t)(p - body) + 4 > length) break;
            runs = get32(p);
            if (runs > (length - (size_t)(p - body) - 4) / 12) break;
            p += 4 + 12 * runs;
            if ((size_t)(p - body) + 16 > length) break;
            p += 16;
        }
        if (f < r->function_count || (size_t)(p - body) + 8 > length) break;
        r->object_size = (size_t)get64(p);
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
    size_t i, f, k, total = 0, slot;
    for (i = 0; i < c->count; ++i) {
        const unsigned char* p = c->records[i].functions;
        for (f = 0; f < c->records[i].function_count; ++f) {
            size_t leaders = get32(p + 8);
            total += leaders;
            p += 12 + 8 * leaders;
            p += 4 + 12 * (size_t)get32(p) + 16;
        }
    }
    for (c->capacity = 1024; c->capacity < 2 * total + 2; c->capacity *= 2) continue;
    c->index = (CacheSlot*)calloc(c->capacity, sizeof(*c->index));
    if (!c->index) return -1;
    for (i = 0; i < c->count; ++i) {
        const unsigned char* p = c->records[i].functions;
        for (f = 0; f < c->records[i].function_count; ++f) {
            size_t leaders = get32(p + 8);
            for (k = 0; k < leaders; ++k) {
                SnxU64 key = get64(p + 12 + 8 * k) | 1;
                for (slot = entry_hash(key, c->capacity); c->index[slot].key && c->index[slot].key != key;
                     slot = (slot + 1) & (c->capacity - 1))
                    continue;
                c->index[slot].key = key;
                c->index[slot].record = i;
            }
            p += 12 + 8 * leaders;
            p += 4 + 12 * (size_t)get32(p) + 16;
        }
    }
    return 0;
}

/* Opens the cache: reads what an earlier run with the same key wrote, and starts the file over when there is none, its
   key differs, it is too large or a run left a record unfinished (rewritten with the whole ones) */
static void cache_open(SnxJitService* s) {
    Cache* c = &s->cache;
    char *key = cache_key(s), *temporary;
    FILE* f = fopen(s->config.cache_path, "rb");
    size_t key_length, size = 0, header, used = 0, slots;
    long length = -1;
    if (!key) return;
    key_length = strlen(key);
    header = 8 + 4 + key_length;
    if (f && !fseek(f, 0, SEEK_END)) length = ftell(f);
    if (f && length >= (long)header && (size_t)length <= CACHE_MAX && !fseek(f, 0, SEEK_SET)) {
        size = (size_t)length;
        c->buffer = (unsigned char*)malloc(size);
        if (c->buffer && fread(c->buffer, 1, size, f) != size) {
            free(c->buffer);
            c->buffer = NULL;
        }
    }
    if (f) fclose(f);
    if (c->buffer && (memcmp(c->buffer, CACHE_MAGIC, 8) || get32(c->buffer + 8) != key_length ||
                      memcmp(c->buffer + 12, key, key_length))) {
        free(c->buffer); /* another build or host's: of no use here */
        c->buffer = NULL;
    }
    if (c->buffer) {
        slots = (size - header) / 20 + 1; /* a record takes more than 20 bytes */
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
    if (c->buffer && used == size) {
        c->file = fopen(s->config.cache_path, "ab");
    } else {
        /* a new file: the header, and the whole records of the old one */
        temporary = (char*)malloc(strlen(s->config.cache_path) + 8);
        if (temporary) {
            sprintf(temporary, "%s.new", s->config.cache_path);
            c->file = fopen(temporary, "wb");
            if (c->file) {
                Out o;
                memset(&o, 0, sizeof(o));
                out_bytes(&o, CACHE_MAGIC, 8);
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
    memset(c, 0, sizeof(*c));
}

/* the record whose functions have pc as a leader, or NULL */
static CacheRecord* cache_find(Cache* c, SnxU64 pc) {
    SnxU64 key = pc | 1;
    size_t slot;
    if (!c->capacity) return NULL;
    for (slot = entry_hash(key, c->capacity); c->index[slot].key; slot = (slot + 1) & (c->capacity - 1))
        if (c->index[slot].key == key) return &c->records[c->index[slot].record];
    return NULL;
}

/* Appends the unit just loaded: its functions as codegen found them, hashed from the images, and its object. Under
   load_lock */
static void cache_append(SnxJitService* s, const SnxCodegen* codegen, const char* name, const unsigned char* object,
                         size_t size, const SnxCodeImage* images, size_t image_count) {
    Out body, runs;
    size_t i, j, leaders, run_count, name_length = strlen(name);
    SnxU64 h1 = 0, h2 = 0, start = 0, previous = 0;
    unsigned char head[12];
    if (!s->cache.file) return;
    memset(&body, 0, sizeof(body));
    memset(&runs, 0, sizeof(runs));
    head[0] = (unsigned char)name_length;
    head[1] = (unsigned char)(name_length >> 8);
    out_bytes(&body, head, 2);
    out_bytes(&body, name, name_length);
    out32(&body, (SnxU32)codegen->function_count);
    for (i = 0; i < codegen->function_count && !body.failed; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        out64(&body, f->start);
        for (j = 0, leaders = 0; j < f->count; ++j) leaders += f->leaders[j] != 0;
        out32(&body, (SnxU32)leaders);
        for (j = 0; j < f->count; ++j)
            if (f->leaders[j]) out64(&body, snx_function_address(f, j));
        /* the instructions as runs of consecutive addresses */
        runs.size = 0;
        run_count = 0;
        for (j = 0; j < f->count; ++j) {
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
        h1 = fnv(body.bytes, body.size, 0xCBF29CE484222325ULL);
        head[0] = (unsigned char)RECORD_MAGIC;
        head[1] = (unsigned char)(RECORD_MAGIC >> 8);
        head[2] = (unsigned char)(RECORD_MAGIC >> 16);
        head[3] = (unsigned char)(RECORD_MAGIC >> 24);
        for (i = 0; i < 8; ++i) head[4 + i] = (unsigned char)((SnxU64)body.size >> (8 * i));
        out64(&body, h1);
        if (!body.failed && (fwrite(head, 1, 12, s->cache.file) != 12 ||
                             fwrite(body.bytes, 1, body.size, s->cache.file) != body.size || fflush(s->cache.file))) {
            fclose(s->cache.file); /* a disk that failed once: no more records this run */
            s->cache.file = NULL;
        }
    }
    free(body.bytes);
    free(runs.bytes);
}

/* Loads the record r and publishes its functions as a batch publishes its own, when every function's instructions
   are still those it was compiled from: 1, or 0 when it is refused (and it stays so). Under load_lock */
static int cache_load(SnxJitService* s, CacheRecord* r, const SnxCodeImage* images, size_t image_count) {
    const unsigned char* p = r->functions;
    SnxLinkInput input;
    const char* names[1];
    SnxU64 table = 0, h1, h2;
    size_t f, k, leaders, runs;
    char error[256];
    for (f = 0; f < r->function_count; ++f) { /* the code it was compiled from, unchanged */
        leaders = get32(p + 8);
        p += 12 + 8 * leaders;
        runs = get32(p);
        if (hash_runs(images, image_count, p + 4, runs, &h1, &h2) || h1 != get64(p + 4 + 12 * runs) ||
            h2 != get64(p + 12 + 12 * runs)) {
            r->state = -1;
            return 0;
        }
        p += 4 + 12 * runs + 16;
    }
    input.data = r->object;
    input.size = r->object_size;
    input.name = r->name;
    names[0] = r->name;
    if (snx_code_load(s->loader, &input, 1, names, 1, &table, error, sizeof(error))) {
        r->state = -1;
        return 0;
    }
    r->state = 1;
    /* publish: each function's leaders, into their empty slots and the service's table (compile's) */
    p = r->functions;
    for (f = 0; f < r->function_count; ++f) {
        GuestFn code = unit_function(table, f);
        leaders = get32(p + 8);
        for (k = 0; k < leaders; ++k) {
            SnxU64 address = get64(p + 12 + 8 * k);
            Region* region = region_of(s, address);
            GuestFn expected = NULL;
            if (region)
                __atomic_compare_exchange_n(slot(region, address), &expected, code, 0, __ATOMIC_RELEASE,
                                            __ATOMIC_RELAXED);
        }
        lock(&s->lock);
        for (k = 0; k < leaders; ++k) {
            Entry* e = add(s, get64(p + 12 + 8 * k), DONE);
            if (!e || e->state == FAILED || e->state == GONE || (e->state == DONE && e->code)) continue;
            e->state = DONE; /* a pc queued or compiling elsewhere is done: its batch's end leaves it so */
            e->code = code;
        }
        unlock(&s->lock);
        p += 12 + 8 * leaders;
        p += 4 + 12 * (size_t)get32(p) + 16;
    }
    lock(&s->lock);
    ++s->stats.cached;
    cond_broadcast(&s->done);
    unlock(&s->lock);
    return 1;
}

/* Compiles the batch of pcs (count of them by address, each IN_FLIGHT), publishes the code and ends their entries:
   DONE with the code that enters at each, or FAILED. A batch of several that fails to emit or load is split, so one pc
   LLVM or the loader refuses fails alone */
/* Under lock: the images a batch reads, the config's and every added one, which stay readable until images_release
   (snx_jit_remove_image waits for it). NULL when memory runs out */
static SnxCodeImage* images_take(SnxJitService* s, size_t* count, unsigned char* taken) {
    SnxCodeImage* images =
        (SnxCodeImage*)malloc((s->config.image_count + SNX_JIT_IMAGES) * sizeof(*images) + sizeof(*images));
    size_t i, n = s->config.image_count;
    if (!images) return NULL;
    memcpy(images, s->config.images, s->config.image_count * sizeof(*images));
    for (i = 1; i <= SNX_JIT_IMAGES; ++i) {
        taken[i] = s->regions[i].hi != 0;
        if (!taken[i]) continue;
        images[n++] = s->regions[i].image;
        ++s->regions[i].users;
    }
    *count = n;
    return images;
}
static void images_release(SnxJitService* s, SnxCodeImage* images, const unsigned char* taken) {
    size_t i;
    lock(&s->lock);
    for (i = 1; i <= SNX_JIT_IMAGES; ++i)
        if (taken[i]) --s->regions[i].users;
    cond_broadcast(&s->released);
    unlock(&s->lock);
    free(images);
}

static void compile(SnxJitService* s, const SnxU64* pcs, size_t count) {
    SnxCodegen codegen;
    SnxJitReport report;
    SnxLinkInput input;
    SnxCodeImage* images;
    unsigned char taken[1 + SNX_JIT_IMAGES];
    unsigned char* object = NULL;
    size_t size = 0, i, j, used, failed = 0, image_count = 0;
    char error[512], name[48];
    const char* names[1];
    SnxU64 table = 0, address, *left = NULL;
    GuestFn code, expected;
    const SnxFunction* f;
    Region* r;
    Entry* e;
    Batch batch;
    int rc, discovered;
    memset(&report, 0, sizeof(report));
    report.pc = pcs[0];
    report.pcs = count;
    report.seconds = snx_seconds();
    error[0] = 0;
    batch.service = s;
    batch.pcs = pcs;
    batch.count = count;
    lock(&s->lock);
    images = images_take(s, &image_count, taken);
    unlock(&s->lock);
    if (images && s->cache.capacity && (left = (SnxU64*)malloc(count * sizeof(*left))) != NULL) {
        /* what the cache has: its units loaded and published, and the pcs they covered done */
        lock(&s->load_lock);
        for (i = 0; i < count; ++i) {
            CacheRecord* record = cache_find(&s->cache, pcs[i]);
            if (record && !record->state) cache_load(s, record, images, image_count);
        }
        unlock(&s->load_lock);
        lock(&s->lock);
        for (i = 0, j = 0; i < count; ++i) {
            e = find(s, pcs[i]);
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
    rc = images ? snx_codegen_discover(&codegen, images, image_count, pcs, count, known, &batch, s->config.budget) : -1;
    if (!images) memset(&codegen, 0, sizeof(codegen));
    discovered = !rc;
    if (rc) snprintf(error, sizeof(error), "no guest code to compile at %llx", (unsigned long long)pcs[0]);
    for (i = 0; !rc && i < codegen.function_count; ++i) report.instructions += codegen.functions[i].count;
    report.functions = codegen.function_count;
    if (!rc) {
        lock(&s->load_lock);
        /* unique in the loader's table, the cache's units from earlier runs included */
        do sprintf(name, "snx_jit_unit_%s_%lu", s->run, ++s->units);
        while (snx_code_lookup(s->loader, name, &address));
        unlock(&s->load_lock);
        rc = emit_unit(s, &codegen, name, &object, &size, error, sizeof(error));
    }
    if (!rc) {
        input.data = object;
        input.size = size;
        input.name = name;
        names[0] = name;
        /* the code is complete and visible to instruction fetch when this returns: its addresses may go to any
           thread from here (code_loader.h) */
        lock(&s->load_lock);
        used = snx_code_used(s->loader);
        rc = snx_code_load(s->loader, &input, 1, names, 1, &table, error, sizeof(error));
        report.bytes = snx_code_used(s->loader) - used;
        if (!rc) cache_append(s, &codegen, name, object, size, images, image_count);
        unlock(&s->load_lock);
    }
    free(object);
    if (rc && discovered && count > 1) {
        snx_codegen_free(&codegen);
        if (images) images_release(s, images, taken);
        for (i = 0; i < count; ++i) compile(s, &pcs[i], 1);
        free(left);
        return;
    }
    /* publish: every leader's slot that is still empty, with release ordering, so a thread that loads the address
       (relaxed, cpu.h: x86-64 keeps loads in order) reaches the code and data written before it. A slot something
       else filled meanwhile (another batch, a wrapper) keeps what it holds */
    for (i = 0; !rc && i < codegen.function_count; ++i) {
        f = &codegen.functions[i];
        code = unit_function(table, i);
        for (j = 0; j < f->count; ++j) {
            if (!f->leaders[j]) continue;
            address = snx_function_address(f, j);
            r = region_of(s, address); /* an added region being removed has none: its arrays stay until users is 0 */
            if (!r) continue;
            expected = NULL;
            __atomic_compare_exchange_n(slot(r, address), &expected, code, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        }
    }
    lock(&s->lock);
    for (i = 0; !rc && i < codegen.function_count; ++i) {
        f = &codegen.functions[i];
        code = unit_function(table, i);
        for (j = 0; j < f->count; ++j) {
            if (!f->leaders[j]) continue;
            /* a queued pc this batch compiled is done (its queue skips it); one another batch compiles is that
               batch's to end, as this batch's own pcs are below, and an earlier batch's keeps its code */
            e = add(s, snx_function_address(f, j), DONE);
            if (!e || e->state == IN_FLIGHT || e->state == FAILED || e->state == GONE || (e->state == DONE && e->code))
                continue;
            e->state = DONE;
            e->code = code;
        }
    }
    for (i = 0; i < count; ++i) {
        e = find(s, pcs[i]);
        f = rc ? NULL : snx_codegen_entry(&codegen, pcs[i]);
        code = f ? unit_function(table, (size_t)(f - codegen.functions)) : NULL;
        /* its image went while the batch compiled (snx_jit_remove_image clears hi under this lock): gone, not failed */
        if (!code && !in_text(s, pcs[i]) && !region_of(s, pcs[i])) {
            if (e) {
                e->state = GONE;
                e->code = NULL;
            }
            continue;
        }
        if (e) {
            e->state = code ? DONE : FAILED;
            e->code = code;
        }
        if (code)
            ++s->stats.compiled;
        else
            ++failed;
    }
    report.seconds = snx_seconds() - report.seconds;
    ++s->stats.batches;
    s->stats.failures += (unsigned long)failed;
    if (!rc) {
        s->stats.functions += (unsigned long)report.functions;
        s->stats.instructions += (unsigned long)report.instructions;
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

/* Under lock: up to max pcs of the first queue that has any into pcs, made IN_FLIGHT; 0 when both are empty. Skips
   the pcs a batch compiled while they waited */
static size_t take(SnxJitService* s, SnxU64* pcs) {
    size_t n = 0, max;
    int q, state;
    SnxU64 pc;
    Entry* e;
    for (q = LIVE; q < QUEUES && !n; ++q) {
        max = q == LIVE ? s->config.batch : (s->config.batch + 7) / 8;
        while (n < max && s->queues[q].count) {
            pc = pop(&s->queues[q]);
            e = find(s, pc);
            state = q == LIVE ? QUEUED : QUEUED_WARM;
            /* compiled since as another batch's leader (whatever its slot holds: a wrapper isn't code for it), or moved
               to the misses' queue */
            if (!e || e->state != state) continue;
            e->state = IN_FLIGHT;
            set_pending(s, pc);
            pcs[n++] = pc;
        }
    }
    s->stats.queued = (unsigned long)(s->queues[LIVE].count + s->queues[WARM].count);
    return n;
}

static int compare_pc(const void* a, const void* b) {
    SnxU64 x = *(const SnxU64*)a, y = *(const SnxU64*)b;
    return x < y ? -1 : x > y;
}

/* A compile thread: batch after batch until the service stops. A batch that leaves pcs queued wakes another thread
   for them */
static void worker(SnxJitService* s) {
    SnxU64* pcs = (SnxU64*)malloc(s->config.batch * sizeof(*pcs));
    size_t n;
    /* below the guest threads: when the host has no idle CPU, a compile waits, not the game */
#if defined(__linux__)
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#elif defined(_WIN32)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
    lock(&s->lock);
    while (pcs && !s->stop) {
        n = take(s, pcs);
        if (!n) {
            if (!s->busy) cond_broadcast(&s->idle);
            cond_wait(&s->work, &s->lock);
            continue;
        }
        ++s->busy;
        if (s->queues[LIVE].count || s->queues[WARM].count) cond_signal(&s->work);
        unlock(&s->lock);
        qsort(pcs, n, sizeof(*pcs), compare_pc);
        compile(s, pcs, n);
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
static int thread_start(JitThread* t, SnxJitService* s) { return pthread_create(t, NULL, worker_main, s) ? -1 : 0; }
static void thread_join(JitThread t) { pthread_join(t, NULL); }
#endif

/* ---- the interface ---- */

GuestFn snx_jit_resolve(SnxJitService* s, SnxU64 pc) {
    GuestFn code = NULL;
    Region* r = region_of(s, pc);
    Entry* e;
    /* an added image's table is the service's alone: its code is found here, without the lock (the main table's slot
       is empty, or the caller's lookup would not have missed) */
    if (r && r != &s->regions[0] && (code = __atomic_load_n(slot(r, pc), __ATOMIC_ACQUIRE)) != NULL) return code;
    if (!in_text(s, pc) && !(r && r != &s->regions[0])) return NULL;
    if (r && is_pending(r, pc) && !__atomic_load_n(slot(r, pc), __ATOMIC_ACQUIRE)) return NULL;
    lock(&s->lock);
    e = find(s, pc);
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
    size_t i, queued = 0;
    Region* r;
    Entry* e;
    lock(&s->lock);
    for (i = 0; i < count; ++i) {
        if (!in_text(s, pcs[i]) || find(s, pcs[i])) continue;
        r = region_of(s, pcs[i]);
        if (r && __atomic_load_n(slot(r, pcs[i]), __ATOMIC_RELAXED)) continue;
        e = add(s, pcs[i], QUEUED_WARM);
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
    GuestFn code = snx_jit_resolve(s, pc); /* queued at the front of the misses, if it wasn't */
    Entry* e;
    if (code) return code;
    lock(&s->lock);
    for (;;) {
        /* found again each time: the table moves when it grows */
        e = find(s, pc);
        if (!e || e->state == FAILED || e->state == GONE) break; /* outside the images, or uncompilable */
        if (e->state == DONE) {
            code = e->code;
            break;
        }
        if (e->state == QUEUED_WARM) { /* in the warm queue only: moved to the misses' */
            unlock(&s->lock);
            if ((code = snx_jit_resolve(s, pc)) != NULL) return code;
            lock(&s->lock);
            continue;
        }
        cond_wait(&s->done, &s->lock);
    }
    unlock(&s->lock);
    return code;
}

int snx_jit_add_image(SnxJitService* s, const SnxCodeImage* image) {
    SnxU64 lo = image->base + image->text_off, hi = lo + image->text_size;
    size_t i, n = (size_t)(image->text_size / 4);
    GuestFn* slots;
    unsigned char* pending;
    Region* r = NULL;
    if (!image->text_size || (lo & 3) || hi < lo || n > ((size_t)-1) / sizeof(*slots)) return -1;
    slots = (GuestFn*)calloc(n, sizeof(*slots));
    pending = (unsigned char*)calloc(n / 8 + 1, 1);
    if (!slots || !pending) {
        free(slots);
        free(pending);
        return -1;
    }
    lock(&s->lock);
    if (lo < s->regions[0].hi && s->regions[0].lo < hi) r = &s->regions[0]; /* over the main table: refused */
    for (i = 1; r != &s->regions[0] && i <= SNX_JIT_IMAGES; ++i)
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
    unlock(&s->lock);
    return 0;
}

void snx_jit_remove_image(SnxJitService* s, SnxU64 base) {
    Region* r = NULL;
    GuestFn* slots;
    unsigned char* pending;
    SnxU64 lo, hi, pc;
    size_t i;
    lock(&s->lock);
    for (i = 1; i <= SNX_JIT_IMAGES && !r; ++i)
        if (s->regions[i].hi && s->regions[i].image.base == base) r = &s->regions[i];
    if (!r) {
        unlock(&s->lock);
        return;
    }
    lo = r->lo;
    hi = r->hi;
    __atomic_store_n(&r->hi, 0, __ATOMIC_RELEASE); /* no batch takes the image, no publish finds the table */
    while (r->users) cond_wait(&s->released, &s->lock);
    /* its pcs are new to the service again: queued ones are skipped when taken, compiled ones forgotten */
    for (i = 0; i < s->entry_capacity; ++i) {
        pc = s->entries[i].key & ~(SnxU64)1;
        if (!s->entries[i].key || pc < lo || pc >= hi) continue;
        s->entries[i].state = GONE;
        s->entries[i].code = NULL;
    }
    cond_broadcast(&s->done); /* a thread waiting for one of them gets NULL */
    slots = r->slots;
    pending = r->pending;
    r->slots = NULL;
    r->pending = NULL;
    memset(&r->image, 0, sizeof(r->image));
    unlock(&s->lock);
    free(slots);
    free(pending);
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
    SnxJitService* s;
    SnxLinkInput input;
    size_t i, bits;
    for (i = 1; i < config->aot_count; ++i)
        if (config->aot_addresses[i - 1] >= config->aot_addresses[i]) {
            snprintf(error, error_size, "the AOT entries are not in ascending order");
            return NULL;
        }
    if (snx_ir_init(error, error_size)) return NULL;
    s = (SnxJitService*)calloc(1, sizeof(*s));
    if (!s) {
        snprintf(error, error_size, "out of memory");
        return NULL;
    }
    s->config = *config;
    if (!s->config.budget) s->config.budget = SNX_JIT_BUDGET;
    if (!s->config.batch) s->config.batch = SNX_JIT_BATCH;
    if (!s->config.opt) s->config.opt = '2';
    if (!s->config.threads) s->config.threads = default_threads();
    if (s->config.threads > SNX_JIT_THREADS) s->config.threads = SNX_JIT_THREADS;
    bits = s->config.code_hi > s->config.code_lo ? (size_t)((s->config.code_hi - s->config.code_lo) >> 2) : 0;
    s->regions[0].lo = s->config.code_lo;
    s->regions[0].hi = s->config.code_hi > s->config.code_lo ? s->config.code_hi : s->config.code_lo;
    s->regions[0].slots = s->config.dispatch;
    s->regions[0].pending = (unsigned char*)calloc(bits / 8 + 1, 1);
    s->loader = s->regions[0].pending ? snx_code_loader_new(0, error, error_size) : NULL;
    if (!s->loader) {
        if (!s->regions[0].pending) snprintf(error, error_size, "out of memory");
        free(s->regions[0].pending);
        free(s);
        return NULL;
    }
    snx_code_set_resolver(s->loader, resolve_aot, s);
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
        seed = fnv((const unsigned char*)&seed, sizeof(seed), 0xCBF29CE484222325ULL);
        sprintf(s->run, "%08lx%08lx", (unsigned long)(seed >> 32), (unsigned long)(seed & 0xFFFFFFFFu));
    }
    if (s->config.cache_path) cache_open(s);
    lock_init(&s->lock);
    lock_init(&s->load_lock);
    cond_init(&s->work);
    cond_init(&s->idle);
    cond_init(&s->released);
    cond_init(&s->done);
    for (i = 0; i < s->config.threads; ++i)
        if (thread_start(&s->threads[i], s)) break;
    s->stats.threads = (unsigned)i;
    if (!i) {
        snprintf(error, error_size, "cannot start a compile thread");
        snx_jit_service_free(s);
        return NULL;
    }
    return s;
}

void snx_jit_service_free(SnxJitService* s) {
    unsigned i;
    int q;
    if (!s) return;
    lock(&s->lock);
    s->stop = 1;
    cond_broadcast(&s->work);
    unlock(&s->lock);
    for (i = 0; i < s->stats.threads; ++i) thread_join(s->threads[i]);
    cache_close(&s->cache);
    snx_code_loader_free(s->loader);
    free(s->entries);
    for (q = 0; q < QUEUES; ++q) free(s->queues[q].pcs);
    for (i = 0; i <= SNX_JIT_IMAGES; ++i) {
        free(s->regions[i].pending);
        if (i) free(s->regions[i].slots);
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
