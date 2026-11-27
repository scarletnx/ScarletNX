/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module of a 32-bit program, ahead of time: one host function per block, from its entry to the next control
   transfer, which the runtime runs one after another (nx/runtime/arm.c). The host's native back end makes the blocks
   (recompiler/mach/native.h: x64/x64_arm.h's, arm64/arm64_arm.h's) as it finds them; one it refuses fails the module,
   named with the reason. The blocks are found by following the code from what is known to be code: each module's entry,
   its function symbols, the code pointers its relocations make, and the addresses the runtime recorded in the hints
   file because it had to compile them while the game ran (nx/runtime/arm.c). From each block, the blocks its last
   instruction leads to: a direct branch's target, and the next instruction after a call or a conditional transfer.
   An indirect branch's targets are known only when the game runs, unless something above names them */
#include "nx/loader/module_a64.h"
#include "common/cpu/recompiler/compile_thread.h"
#include "nx/loader/module_abi.h"
#include "common/cpu/arm/arm.h"
#include "common/cpu/recompiler/object/link.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/cpu/recompiler/mach/native.h"
#include "nx/loader/module_x64.h"
#include "snx_settings.h"
#include "common/utilities/endian.h"
#include "common/platform/file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#endif

/* the most code one block may run through: the block compiler's limit, in instructions (A32) or halfwords (Thumb) */
#define BLOCK_LIMIT 65536
/* the most blocks one block goes on to: a32_next and thumb_next queue its branch's target and what follows it */
#define SUCCESSORS  2

typedef struct {
    SnxU32 key; /* the address, with bit 0 set for Thumb */
    SnxU32 units;
} Block;
typedef struct {
    const SnxProgram* program;
    Block* blocks;
    size_t count, capacity;
    SnxU32* set; /* every key ever queued: open addressing, 0 is empty (no code is at address 0) */
    size_t set_capacity, set_count;
    SnxU32* work;
    size_t work_count, work_capacity;
    int collect; /* queue only lists the keys in work, for one block's successors (block_next) */
    /* the native back end, which makes each block as it is found (the first thread's, whose object takes the others'
       blocks) */
    SnxNativeArm* x64;
} Discovery;

/* the code from address to the end of its module's text, and how many bytes of it */
static const unsigned char* code_at(const SnxProgram* p, SnxU32 address, size_t* bytes) {
    size_t i;
    for (i = 0; i < p->module_count; ++i) {
        const SnxModuleImage* m = &p->modules[i];
        SnxU64 lo = m->base + m->text_off, hi = lo + m->text_size;
        if (address >= lo && address < hi) {
            *bytes = (size_t)(hi - address);
            return m->image + m->text_off + (address - lo);
        }
    }
    *bytes = 0;
    return NULL;
}

static int set_insert(Discovery* d, SnxU32 key) {
    size_t i;
    if ((d->set_count + 1) * 2 > d->set_capacity) {
        size_t capacity = d->set_capacity ? d->set_capacity * 2 : 4096, j;
        SnxU32* set = (SnxU32*)snx_alloc(capacity * sizeof(*set));
        memset(set, 0, capacity * sizeof(*set));
        for (j = 0; j < d->set_capacity; ++j)
            if (d->set[j]) {
                i = (size_t)(d->set[j] * 0x9E3779B1u) & (capacity - 1);
                while (set[i]) i = (i + 1) & (capacity - 1);
                set[i] = d->set[j];
            }
        free(d->set);
        d->set = set;
        d->set_capacity = capacity;
    }
    i = (size_t)(key * 0x9E3779B1u) & (d->set_capacity - 1);
    while (d->set[i]) {
        if (d->set[i] == key) return 0;
        i = (i + 1) & (d->set_capacity - 1);
    }
    d->set[i] = key;
    ++d->set_count;
    return 1;
}

/* queues the block at address, in Thumb state or not, when it is code this program has and wasn't queued yet. A32
   code is word aligned, Thumb code halfword aligned */
static void queue(Discovery* d, SnxU32 address, int thumb) {
    size_t bytes;
    SnxU32 key = address | (thumb ? 1u : 0u);
    if (address & (thumb ? 1u : 3u)) return;
    if (!code_at(d->program, address, &bytes) || bytes < (thumb ? 2u : 4u)) return;
    if (d->collect) {
        if (d->work_count < SUCCESSORS) d->work[d->work_count++] = key;
        return;
    }
    if (!set_insert(d, key)) return;
    if (d->work_count == d->work_capacity) {
        d->work_capacity = d->work_capacity ? d->work_capacity * 2 : 1024;
        d->work = (SnxU32*)snx_realloc(d->work, d->work_capacity * sizeof(*d->work));
    }
    d->work[d->work_count++] = key;
}
/* a code pointer: interworking's, Thumb when bit 0 is set */
static void queue_pointer(Discovery* d, SnxU64 pointer) {
    if (pointer >> 32) return;
    queue(d, (SnxU32)pointer & ~1u, (int)(pointer & 1));
}

/* whether control never reaches the instruction after the A32 instruction w when it runs: a branch without link, a
   write to the PC, BKPT. A call returns there, as does an instruction the block compiler leaves to the backend
   (SnxArmRt.undefined), which may run it. Anything else ends a block only at the code's end */
static int a32_ends(SnxU32 w) {
    unsigned rd = (w >> 12) & 15;
    switch (snx_a32_decode(w)) {
        case SNX_A32_BRANCH: return !(w & (1u << 24)); /* B; BL returns */
        case SNX_A32_BX: return !(w & 0x20);           /* BX; BLX returns */
        case SNX_A32_DATA: return rd == 15;
        case SNX_A32_SINGLE:
        case SNX_A32_EXTRA: return rd == 15 && (w & (1u << 20));
        case SNX_A32_MULTIPLE: return (w & (1u << 15)) && (w & (1u << 20));
        case SNX_A32_BKPT: return 1;
        default: return 0;
    }
}

/* What follows an A32 block whose last instruction is w at pc: the target of a direct branch, and the next
   instruction unless control can't get there (a32_ends, when the instruction isn't conditional). Leaving a successor
   out only sends the game through the runtime's block compiler there; one too many costs a block of code that never
   runs */
static void a32_next(Discovery* d, SnxU32 pc, SnxU32 w) {
    SnxA32Kind kind = snx_a32_decode(w);
    unsigned cond = w >> 28;
    if (kind == SNX_A32_BRANCH) queue(d, pc + 8 + ((SnxU32)snx_sext32(w & 0xFFFFFF, 24) << 2), 0);
    if (kind == SNX_A32_BLX_IMM) queue(d, pc + 8 + ((SnxU32)snx_sext32(w & 0xFFFFFF, 24) << 2) + ((w >> 23) & 2), 1);
    if (!(a32_ends(w) && (cond == 14 || cond == 15))) queue(d, pc + 4, 0);
}

/* The same for a Thumb block of units halfwords at pc, compiled from count halfwords: its instructions are walked as
   the block compiler walks them, IT state included, to its last one */
static void thumb_next(Discovery* d, SnxU32 pc, const unsigned char* code, size_t units, size_t count) {
    size_t used = 0, len = 1;
    unsigned it_len = 0, it_idx = 0, it_conds[4], cond = 14, i;
    SnxThumb t;
    memset(&t, 0, sizeof(t));
    while (used < units) {
        unsigned hw1 = code[used * 2] | (unsigned)code[used * 2 + 1] << 8;
        unsigned hw2 = used + 1 < count ? code[used * 2 + 2] | (unsigned)code[used * 2 + 3] << 8 : 0;
        len = snx_thumb_decode((uint16_t)hw1, (uint16_t)hw2, &t);
        if (!len || (len == TH_F32 && used + 1 >= count)) { /* as the block compiler steps over it */
            t.shape = TH_UNDEF;
            len = (hw1 >> 11) >= 0x1D && used + 1 < count ? 2 : 1;
        }
        cond = it_idx < it_len ? it_conds[it_idx++] : 14;
        if (used + len >= units) break;
        used += len;
        if (t.shape == TH_IT) { /* an IT block: 4 - ctz(mask) instructions */
            unsigned mask = t.imm & 15, first = (t.imm >> 4) & 15;
            for (it_len = 4; !(mask & 1); mask >>= 1) --it_len;
            mask = t.imm & 15;
            it_idx = 0;
            for (i = 0; i < it_len; ++i) it_conds[i] = i == 0 ? first : ((first & 0xE) | ((mask >> (4 - i)) & 1));
        }
    }
    pc += (SnxU32)used * 2; /* the last instruction */
    {
        SnxU32 next = pc + (SnxU32)len * 2, base = pc + 4;
        int always = cond == 14, ends = 0;
        switch (t.shape) {
            case TH_BRANCH: {
                unsigned hw1 = t.rn, hw2 = t.rm, s = (hw1 >> 10) & 1;
                unsigned i1 = !(((hw2 >> 13) & 1) ^ s), i2 = !(((hw2 >> 11) & 1) ^ s);
                switch (t.op) {
                    case BR_B:
                        queue(d, base + ((SnxU32)snx_sext32(t.imm, 11) << 1), 1);
                        ends = 1;
                        break;
                    case BR_BCOND:
                        queue(d, base + ((SnxU32)snx_sext32(t.imm, 8) << 1), 1);
                        always = 0;
                        break;
                    case BR_CBZ:
                    case BR_CBNZ:
                        queue(d, base + t.imm, 1);
                        always = 0;
                        break;
                    case BR_BL:
                    case BR_BW:
                        queue(d,
                              base +
                                  ((SnxU32)snx_sext32(
                                       (s << 23) | (i1 << 22) | (i2 << 21) | ((hw1 & 0x3FF) << 11) | (hw2 & 0x7FF), 24)
                                   << 1),
                              1);
                        ends = t.op == BR_BW;
                        break;
                    case BR_BLX_IMM:
                        queue(d,
                              (base & ~3u) +
                                  ((SnxU32)snx_sext32((s << 23) | (i1 << 22) | (i2 << 21) | ((hw1 & 0x3FF) << 11) |
                                                          (((hw2 >> 1) & 0x3FF) << 1),
                                                      24)
                                   << 1),
                              0);
                        break;
                    case BR_BCOND_W:
                        queue(d,
                              base +
                                  ((SnxU32)snx_sext32((s << 19) | (((hw2 >> 11) & 1) << 18) |
                                                          (((hw2 >> 13) & 1) << 17) | ((hw1 & 0x3F) << 11) |
                                                          (hw2 & 0x7FF),
                                                      20)
                                   << 1),
                              1);
                        always = 0;
                        break;
                    case BR_BX: ends = 1; break;
                    case BR_BKPT: ends = 1; break;
                    default: break; /* BLX register and SVC return */
                }
                break;
            }
            case TH_TABLE: ends = 1; break;
            case TH_HI: ends = t.rd == 15 && t.op != 1; break; /* MOV or ADD to the PC; not CMP */
            case TH_LOAD: ends = t.rd == 15; break;
            case TH_PUSHPOP: ends = t.op == 1 && t.ra; break; /* POP with the PC */
            case TH_A32: ends = a32_ends(t.imm); break;
            default: break;
        }
        if (!(ends && always)) queue(d, next, 1);
    }
}


static size_t block_next(const SnxProgram* p, SnxU32 key, size_t units, size_t count, SnxU32* next);
/* the successors a native block jumps to directly: the keys discover queues after it, which are blocks the module has */
static size_t x64_next(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]) {
    return block_next((const SnxProgram*)user, key, units, count, keys);
}
/* one thread of discovery's: its native back end, whose blocks go into the module's object */
typedef struct {
    SnxNativeArm* x64;
    char error[256];
} Finder;
/* a key of a round: the words its block covers, and the words the block compiler was given (Thumb's successors read
   them as it walked them) */
typedef struct {
    SnxU32 key;
    size_t units, count;
} Found;
/* a round: the keys queued before it, made on the threads, each taking the next */
typedef struct {
    const SnxProgram* program;
    Found* found;
    size_t count, next;
    int failed;
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
    Finder* finder;
} Round;

/* one key: its block made by the native back end into f's blocks, k->units the words or halfwords it covers of the
   k->count it was given. It reads the guest's code in place, as far as the block goes. 0, or -1 with the reason in
   f->error */
static int find(Finder* f, const SnxProgram* p, Found* k) {
    size_t bytes = 0, consumed = 0;
    int thumb = (int)(k->key & 1);
    SnxU32 address = k->key & ~1u;
    const unsigned char* code = code_at(p, address, &bytes);
    k->count = bytes / (thumb ? 2 : 4);
    if (k->count > BLOCK_LIMIT) k->count = BLOCK_LIMIT;
    if (!(thumb ? snx_native_thumb_block(f->x64, NULL, code, k->count, address, &consumed, x64_next, (void*)p)
                : snx_native_arm_block(f->x64, NULL, code, k->count, address, &consumed, x64_next, (void*)p))) {
        snprintf(f->error, sizeof f->error, "the %s back end made no %s block at 0x%08x: %s", snx_native_name(),
                 thumb ? "Thumb" : "ARM", (unsigned)address, snx_native_arm_why(f->x64));
        return -1;
    }
    k->units = consumed;
    return 0;
}
static void round_lock(Round* r) {
#ifdef _WIN32
    EnterCriticalSection(&r->lock);
#else
    pthread_mutex_lock(&r->lock);
#endif
}
static void round_unlock(Round* r) {
#ifdef _WIN32
    LeaveCriticalSection(&r->lock);
#else
    pthread_mutex_unlock(&r->lock);
#endif
}
static void round_work(Round* r, Finder* f) {
    for (;;) {
        round_lock(r);
        size_t i = r->failed ? r->count : r->next++;
        round_unlock(r);
        if (i >= r->count) return;
        if (find(f, r->program, &r->found[i])) {
            round_lock(r);
            r->failed = 1;
            round_unlock(r);
            return;
        }
    }
}
typedef struct {
    Round* round;
    Finder* finder;
} RoundArg;
#ifdef _WIN32
static DWORD WINAPI round_main(LPVOID p) {
    round_work(((RoundArg*)p)->round, ((RoundArg*)p)->finder);
    return 0;
}
#else
static void* round_main(void* p) {
    round_work(((RoundArg*)p)->round, ((RoundArg*)p)->finder);
    return NULL;
}
#endif
/* the round's keys made on threads threads, the calling one among them: 0, or -1 with the first failure's finder */
static int run_round(Round* r, Finder* finders, unsigned threads) {
    RoundArg args[64];
    unsigned made = 0;
#ifdef _WIN32
    HANDLE ids[64];
    InitializeCriticalSection(&r->lock);
#else
    pthread_t ids[64];
    pthread_mutex_init(&r->lock, NULL);
#endif
    if (threads > 64) threads = 64;
    for (unsigned i = 1; i < threads; ++i) {
        args[i].round = r;
        args[i].finder = &finders[i];
#ifdef _WIN32
        if (!(ids[made + 1] = snx_compile_thread(round_main, &args[i]))) break;
#else
        if (snx_compile_thread(&ids[made + 1], round_main, &args[i])) break;
#endif
        ++made;
    }
    round_work(r, &finders[0]);
    for (unsigned i = 1; i <= made; ++i) {
#ifdef _WIN32
        WaitForSingleObject(ids[i], INFINITE);
        CloseHandle(ids[i]);
#else
        pthread_join(ids[i], NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&r->lock);
#else
    pthread_mutex_destroy(&r->lock);
#endif
    return r->failed ? -1 : 0;
}

/* Every block reachable from the known code, found in rounds: each round makes the keys queued so far on the threads
   (the native back end makes each block then, into its thread's blocks), then queues what follows each, in their order,
   for the next round. A small round runs on the calling thread alone */
static int discover(Discovery* d, const SnxCodegenOptions* options, unsigned threads, char* error, size_t error_size) {
    const SnxProgram* p = d->program;
    if (threads < 1) threads = 1;
    if (threads > 64) threads = 64;
    Finder* finders = (Finder*)snx_alloc(threads * sizeof(*finders));
    for (unsigned i = 0; i < threads; ++i) finders[i] = (Finder){0};
    Round r = {0};
    int rc = 0;
    for (size_t i = 0; i < threads; ++i) {
        finders[i].x64 = i ? snx_native_arm_new() : d->x64;
        if (!finders[i].x64) {
            snprintf(error, error_size, "out of memory");
            rc = -1;
            threads = (unsigned)i + 1;
            goto done;
        }
    }
    for (size_t i = 0; i < p->module_count; ++i) {
        const SnxModuleImage* m = &p->modules[i];
        queue(d, (SnxU32)(m->base + m->text_off), 0); /* the module's start: Loader enters rtld's in A32 state */
        for (size_t j = 0; j < m->function_count; ++j) queue_pointer(d, m->base + m->functions[j]);
        for (size_t j = 0; j < m->relptr_count; ++j) queue_pointer(d, m->base + m->relptrs[j]);
    }
    for (size_t i = 0; i < options->hint_count; ++i) queue_pointer(d, options->hints[i]);
    r.program = p;
    while (d->work_count) {
        unsigned n = threads;
        r.count = d->work_count;
        r.found = (Found*)snx_realloc(r.found, r.count * sizeof(*r.found));
        for (size_t i = 0; i < r.count; ++i) {
            r.found[i].key = d->work[i];
            r.found[i].units = r.found[i].count = 0;
        }
        d->work_count = 0;
        r.next = 0;
        r.failed = 0;
        if (r.count < 256) n = 1; /* a thread's start costs more than a few blocks */
        if (run_round(&r, finders, n)) {
            for (unsigned i = 0; i < n; ++i)
                if (finders[i].error[0]) snprintf(error, error_size, "%s", finders[i].error);
            rc = -1;
            goto done;
        }
        for (size_t i = 0; i < r.count; ++i) {
            const Found* k = &r.found[i];
            SnxU32 address = k->key & ~1u;
            size_t bytes = 0;
            const unsigned char* code = code_at(p, address, &bytes);
            if (d->count == d->capacity) {
                d->capacity = d->capacity ? d->capacity * 2 : 4096;
                d->blocks = (Block*)snx_realloc(d->blocks, d->capacity * sizeof(*d->blocks));
            }
            d->blocks[d->count].key = k->key;
            d->blocks[d->count++].units = (SnxU32)k->units;
            if (k->key & 1)
                thumb_next(d, address, code, k->units, k->count);
            else
                a32_next(d, address + (SnxU32)(k->units - 1) * 4, snx_ld_le32(code + (k->units - 1) * 4));
        }
    }
done:
    free(r.found);
    for (unsigned i = 0; i < threads; ++i) {
        /* each thread's blocks into the first's object */
        if (i && finders[i].x64) {
            if (!rc && snx_native_arm_absorb(d->x64, finders[i].x64)) {
                snprintf(error, error_size, "out of memory");
                rc = -1;
            }
            snx_native_arm_free(finders[i].x64);
        }
    }
    free(finders);
    return rc;
}

/* the keys of what follows the block at key, covering units of code, as discover queues them, into next */
static size_t block_next(const SnxProgram* p, SnxU32 key, size_t units, size_t count, SnxU32* next) {
    Discovery s = {0};
    size_t bytes;
    SnxU32 address = key & ~1u;
    const unsigned char* code = code_at(p, address, &bytes);
    s.program = p;
    s.collect = 1;
    s.work = next;
    if (key & 1)
        thumb_next(&s, address, code, units, count);
    else
        a32_next(&s, address + (SnxU32)(units - 1) * 4, snx_ld_le32(code + (units - 1) * 4));
    return s.work_count;
}

static int compare_block(const void* a, const void* b) {
    SnxU32 x = ((const Block*)a)->key, y = ((const Block*)b)->key;
    return x < y ? -1 : x > y;
}

static const char* base_name(const char* path) {
    const char *name = path, *s;
    for (s = path; *s; ++s)
        if (*s == '/' || *s == '\\') name = s + 1;
    return name;
}

int snx_build_module_arm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxModuleBuild* build) {
    Discovery d;
    SnxLinkInput inputs[3];
    SnxLinkOutput linked;
    unsigned char *module_object = NULL, *x64_object = NULL;
    size_t module_size = 0, x64_size = 0, i, n = 0;
    SnxU32* keys = NULL;
    const char* exports[1];
    char error[1024], *tmp = NULL;
    double start;
    int rc = -1;
    memset(&d, 0, sizeof(d));
    memset(&linked, 0, sizeof(linked));
    if (!loaded->module_count) return snx_error("no modules to generate");
    if (build->triple)
        return snx_error("--target %s: the native back end makes AArch32 blocks for this host alone", build->triple);
    if (!snx_native_arm_available()) {
        if (!snx_native_floor(error, sizeof error))
            snprintf(error, sizeof error, "this host's native back end (%s) makes no AArch32 blocks",
                     snx_native_name());
        return snx_error("%s", error);
    }
    d.program = loaded;
    d.x64 = snx_native_arm_new();
    if (!d.x64) {
        snx_error("%s back end: out of memory", snx_native_name());
        goto done;
    }
    start = snx_seconds();
    if (discover(&d, options, build->jobs ? build->jobs : 1, error, sizeof(error))) {
        snx_error("AArch32: %s", error);
        goto done;
    }
    qsort(d.blocks, d.count, sizeof(*d.blocks), compare_block);
    if (!options->quiet) printf("AArch32: %lu blocks found (%.1f s)\n", (unsigned long)d.count, snx_seconds() - start);
    if (snx_native_arm_object(d.x64, &x64_object, &x64_size, NULL)) {
        snx_error("%s back end: out of memory", snx_native_name());
        goto done;
    }
    if (!options->quiet) printf("%s back end: %zu of %zu AArch32 blocks\n", snx_native_name(), d.count, d.count);
    start = snx_seconds();
    keys = (SnxU32*)snx_alloc((d.count + 1) * sizeof(*keys));
    for (i = 0; i < d.count; ++i) keys[i] = d.blocks[i].key;
    /* the descriptor of the native back end, as its blocks are (module_x64.c) */
    if (snx_x64_arm_descriptor(loaded, options, keys, d.count, &module_object, &module_size, error, sizeof(error))) {
        snx_error("%s back end: %s", snx_native_name(), error);
        goto done;
    }
    if (!options->quiet) {
        char* label = snx_format("SnxArmModule descriptor, %lu blocks", (unsigned long)d.count);
        snx_report_unit(1, 1, "module", label, 0, snx_seconds() - start);
        free(label);
    }
    /* the one the recompiler's build compiled (rtlib_object.h) */
    inputs[n].data = (unsigned char*)snx_armlib_object;
    inputs[n].size = (size_t)snx_armlib_object_size;
    inputs[n++].name = "armlib";
    inputs[n].data = module_object;
    inputs[n].size = module_size;
    inputs[n++].name = "module";
    if (x64_object) {
        inputs[n].data = x64_object;
        inputs[n].size = x64_size;
        inputs[n++].name = "native blocks";
    }
    exports[0] = "snx_module_arm";
    if (snx_link(inputs, n, exports, 1, base_name(build->output), &linked, error, sizeof(error))) {
        snx_error("linking failed: %s", error);
        goto done;
    }
    tmp = snx_format("%s.tmp", build->output);
    if (snx_write_file(tmp, linked.data, linked.size) || plat_replace_file(tmp, build->output)) {
        snx_error("linking failed: cannot write %s", build->output);
        if (snx_is_file(tmp)) snx_remove_tree(tmp);
        goto done;
    }
    if (!options->quiet)
        printf("%s module: %.1f MB, %lu AArch32 blocks\n", snx_native_name(), (double)linked.size / 1000000.0,
               (unsigned long)d.count);
    rc = 0;
done:
    free(module_object);
    free(x64_object);
    free(keys);
    snx_native_arm_free(d.x64);
    free(linked.data);
    free(tmp);
    free(d.blocks);
    free(d.set);
    free(d.work);
    return rc;
}
