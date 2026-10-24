/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime JIT (recompiler/llvm/jit_service.c) driven as the runtime drives it: A64 functions in a guest memory
   buffer, a dispatch table with one "AOT" function (a host function standing in for the module's), and an RtAPI whose
   miss waits for the service's code, as rtapi.c's does. Checks that a miss gets the code of the pc, which a compile
   thread compiles with what it calls, and later calls run the published code without a miss. That the compiled code calls the AOT function directly
   and other JIT code through the table, that threads missing one pc together queue it once, that a pc whose slot holds
   a wrapper is served from the service's table, that pcs outside the code are refused. Then a bank of functions missed
   from several threads at once: they compile in batches, packed into the arena with no page writable and executable,
   measured against one pc per compile. Then the warm start: queued pcs compile before any miss, and a miss goes ahead
   of them. Then an image added while the service runs, as ldr:ro maps an NRO outside CPU.disp: its misses compile and
   its code is found by snx_jit_resolve alone, it calls the AOT module directly, and removing it, even with its
   compiles in flight, forgets its code, so the different code mapped at the same place later is what runs. Then the
   cache: a second service on the same file loads the first one's units instead of compiling them, refuses a unit whose
   code changed (and compiles the new code), and reads the records before one a run left unfinished. A change in the
   middle of a function drops its whole unit */
#include "common/cpu/recompiler/llvm/jit_service.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sched.h>
#endif

#define MEMORY     0x40000
#define CODE_LO    0x1000
#define CODE_HI    0x30000
#define THREADS    8
#define BANK       0x20000 /* BANK_COUNT functions of 8 instructions, every 0x40 bytes */
#define BANK_COUNT 512
#define RETURN     0xFFF0  /* where the outermost RET goes */
#define NRO        0x34000 /* the added image's text, NRO_COUNT functions every 0x40 bytes, outside CPU.disp */
#define NRO_COUNT  64

static int failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);                                                    \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

static unsigned char* memory;
static GuestFn dispatch[(CODE_HI - CODE_LO) / 4];
static SnxJitService* service;
static RtAPI rt;
static unsigned misses, wrapped;
/* the batches the report saw, and the first after which 0x3800 had code */
static unsigned batch_count, live_batch;

/* the guest's code is little-endian whatever the host's byte order */
static void put(u64 address, const u32* words, size_t count) {
    size_t i;
    for (i = 0; i < count * 4; ++i) memory[address + i] = (unsigned char)(words[i / 4] >> (i % 4 * 8));
}
static GuestFn* slot(u64 address) { return &dispatch[(address - CODE_LO) / 4]; }
static GuestFn get(u64 address) { return __atomic_load_n(slot(address), __ATOMIC_RELAXED); }

/* the module's function at 0x2000: x0 += 100, then return */
static void aot_add100(CPU* c, u64 e) {
    (void)e;
    c->x[0] += 100;
    c->pc = c->x[30];
}
/* put in 0x2000's slot once the JIT has compiled its callers: their direct calls never see it */
static void aot_poisoned(CPU* c, u64 e) {
    (void)e;
    c->x[0] += 1000;
    c->pc = c->x[30];
}

/* rtapi.c's rt_miss */
static void miss(CPU* c, u64 pc) {
    GuestFn f;
    __atomic_fetch_add(&misses, 1, __ATOMIC_RELAXED);
    f = snx_jit_wait(service, pc);
    if (!f) {
        fprintf(stderr, "no code for %llx\n", (unsigned long long)pc);
        __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
        c->pc = c->x[30];
        return;
    }
    f(c, pc);
}
static void poll(CPU* c) { __atomic_store_n(&c->poll, 0, __ATOMIC_RELAXED); }
static void undef(CPU* c, u64 pc, u32 w) {
    (void)c;
    fprintf(stderr, "undefined instruction %08x at %llx\n", w, (unsigned long long)pc);
    __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
}
/* on a compile thread */
static void report(const SnxJitReport* r) {
    unsigned i = __atomic_fetch_add(&batch_count, 1, __ATOMIC_RELAXED), unset = ~0u;
    if (get(0x3800)) __atomic_compare_exchange_n(&live_batch, &unset, i, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED);
    if (r->error) {
        fprintf(stderr, "jit: %llx: %s\n", (unsigned long long)r->pc, r->error);
        __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
    }
}

/* the function at 0x3400 behind a wrapper, as gpu/nvn_trace.c puts one into an empty slot: its original is the miss */
static void wrapper(CPU* c, u64 e) {
    __atomic_fetch_add(&wrapped, 1, __ATOMIC_RELAXED);
    rt.miss(c, e);
}

static void cpu_init(CPU* c, u64 stack) {
    memset(c, 0, sizeof(*c));
    c->mem = memory;
    c->disp = dispatch;
    c->code_lo = CODE_LO;
    c->code_hi = CODE_HI;
    c->rt = &rt;
    c->x[31] = stack;
    c->x[30] = RETURN;
}

/* CALLDYN(pc), from the host: what a thread's loop does (kernel/thread.c) */
static void run(CPU* c, u64 pc) { snx_lookup(c, pc)(c, pc); }

/* a fresh table, with only the module's function, and a service over it */
static const char* cache_path; /* start's config.cache_path */

static SnxJitService* start(size_t batch, unsigned threads) {
    SnxJitConfig config;
    static SnxCodeImage image;
    static const u64 aot_addresses[] = {0x2000};
    static const GuestFn aot_functions[] = {aot_add100};
    char error[512];
    SnxJitService* s;
    memset(dispatch, 0, sizeof(dispatch));
    *slot(0x2000) = aot_add100; /* as main.c's build_dispatch fills the module's entries */
    memset(&image, 0, sizeof(image));
    image.image = memory;
    image.image_size = MEMORY;
    image.base = 0;
    image.text_off = CODE_LO;
    image.text_size = CODE_HI - CODE_LO;
    memset(&config, 0, sizeof(config));
    config.images = &image;
    config.image_count = 1;
    config.dispatch = dispatch;
    config.code_lo = CODE_LO;
    config.code_hi = CODE_HI;
    config.aot_addresses = aot_addresses;
    config.aot_functions = aot_functions;
    config.aot_count = 1;
    config.batch = batch;
    config.threads = threads;
    config.report = report;
    config.cache_path = cache_path;
    __atomic_store_n(&batch_count, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&live_batch, ~0u, __ATOMIC_RELAXED);
    s = snx_jit_service_new(&config, error, sizeof(error));
    if (!s) fprintf(stderr, "jit: %s\n", error);
    return s;
}

/* THREADS threads waiting for each other, rounds after rounds (macOS has no pthread_barrier_t) */
typedef struct {
    pthread_mutex_t lock;
    pthread_cond_t all_in;
    unsigned waiting, round;
} Barrier;
static void barrier_init(Barrier* b) {
    pthread_mutex_init(&b->lock, NULL);
    pthread_cond_init(&b->all_in, NULL);
    b->waiting = b->round = 0;
}
static void barrier_wait(Barrier* b) {
    unsigned round;
    pthread_mutex_lock(&b->lock);
    round = b->round;
    if (++b->waiting == THREADS) {
        b->waiting = 0;
        ++b->round;
        pthread_cond_broadcast(&b->all_in);
    } else
        while (b->round == round) pthread_cond_wait(&b->all_in, &b->lock);
    pthread_mutex_unlock(&b->lock);
}
static void barrier_destroy(Barrier* b) {
    pthread_cond_destroy(&b->all_in);
    pthread_mutex_destroy(&b->lock);
}

typedef struct {
    Barrier* barrier;
    CPU cpu;
    u64 pc;
    int bank; /* run the bank's functions index, index + THREADS... instead of pc */
    int index;
} Racer;
static void* race(void* p) {
    Racer* r = (Racer*)p;
    int i;
    barrier_wait(r->barrier);
    if (!r->bank) {
        run(&r->cpu, r->pc);
        return NULL;
    }
    for (i = r->index; i < BANK_COUNT; i += THREADS) {
        r->cpu.x[0] = 0;
        r->cpu.x[1] = 0x30000;
        r->cpu.x[30] = RETURN;
        run(&r->cpu, BANK + 0x40 * (u64)i);
        if (r->cpu.x[0] != (u64)i || r->cpu.pc != RETURN) __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}
/* THREADS threads run racers[i] at once */
static void race_all(Racer* racers) {
    Barrier barrier;
    pthread_t threads[THREADS];
    int i;
    barrier_init(&barrier);
    for (i = 0; i < THREADS; ++i) {
        racers[i].barrier = &barrier;
        CHECK(!pthread_create(&threads[i], NULL, race, &racers[i]));
    }
    for (i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    barrier_destroy(&barrier);
}

/* every thread misses the bank's functions: the time until all are compiled, and what it took */
static void run_bank(const char* label, SnxJitStats* out) {
    Racer racers[THREADS];
    SnxJitStats before, after;
    double t;
    int i;
    snx_jit_stats(service, &before);
    t = snx_seconds();
    for (i = 0; i < THREADS; ++i) {
        cpu_init(&racers[i].cpu, 0x8800 + 0x800 * (u64)i);
        racers[i].bank = 1;
        racers[i].index = i;
    }
    race_all(racers);
    snx_jit_drain(service);
    t = snx_seconds() - t;
    snx_jit_stats(service, &after);
    for (i = 0; i < BANK_COUNT; ++i) CHECK(get(BANK + 0x40 * (u64)i) != NULL);
    out->batches = after.batches - before.batches;
    out->compiled = after.compiled - before.compiled;
    out->functions = after.functions - before.functions;
    out->bytes = after.bytes - before.bytes;
    out->seconds = after.seconds - before.seconds;
    printf("%s: %lu pcs in %lu batches (%.1f per batch) on %u threads, %.2f ms per batch, %.3f ms per function, "
           "%lu bytes per function, all compiled %.0f ms after the first miss\n",
           label, out->compiled, out->batches, (double)out->compiled / (double)(out->batches ? out->batches : 1),
           after.threads, out->seconds * 1000.0 / (double)(out->batches ? out->batches : 1),
           out->seconds * 1000.0 / (double)(out->functions ? out->functions : 1),
           (unsigned long)(out->bytes / (out->functions ? out->functions : 1)), t * 1000.0);
}

/* no mapping of the process both writable and executable */
static int wx_mappings(void) {
    FILE* f = fopen("/proc/self/maps", "r");
    char line[512], perms[8];
    int count = 0;
    if (!f) return 0;
    while (fgets(line, sizeof(line), f))
        if (sscanf(line, "%*s %7s", perms) == 1 && strchr(perms, 'w') && strchr(perms, 'x')) {
            fprintf(stderr, "writable and executable: %s", line);
            ++count;
        }
    fclose(f);
    return count;
}

/* threads run the added image's functions, each NRO + 0x40 * i returning x0 + i (not the first, put_nro's add) */
static void* run_nro(void* p) {
    Racer* r = (Racer*)p;
    int i;
    barrier_wait(r->barrier);
    for (i = r->index; i < NRO_COUNT; i += THREADS) {
        if (!i) continue;
        r->cpu.x[0] = 0;
        r->cpu.x[30] = RETURN;
        run(&r->cpu, NRO + 0x40 * (u64)i);
        if (r->cpu.x[0] != (u64)i) __atomic_fetch_add(&failures, 1, __ATOMIC_RELAXED);
    }
    return NULL;
}

static void put_nro(u32 add) {
    /* NRO + 0x40 * i: x0 += i (i > 0), the last function at 0x100 calls the first and the AOT function at 0x2000 */
    static const u32 caller[] = {0xA9BF7BFD, 0x97FFFFFF, 0x97FFFFFF, 0xA8C17BFD, 0xD65F03C0};
    u32 words[2], call[5];
    int i;
    for (i = 0; i < NRO_COUNT; ++i) {
        words[0] = 0x91000000 | (u32)(i ? i : add) << 10; /* add x0, x0, #i */
        words[1] = 0xD65F03C0;
        put(NRO + 0x40 * (u64)i, words, 2);
    }
    memcpy(call, caller, sizeof(call));
    call[1] = 0x94000000 | (u32)(((u64)NRO - (NRO + 0x1000 + 4)) >> 2 & 0x3FFFFFF);    /* bl NRO */
    call[2] = 0x94000000 | (u32)(((u64)0x2000 - (NRO + 0x1000 + 8)) >> 2 & 0x3FFFFFF); /* bl 0x2000 */
    put(NRO + 0x1000, call, 5);
}

static void test_added_image(void) {
    static SnxCodeImage nro, over_main;
    unsigned before_misses;
    Barrier barrier;
    pthread_t threads[THREADS];
    Racer racers[THREADS];
    int i;
    CPU c;
    service = start(0, 2);
    if (!service) {
        ++failures;
        return;
    }
    memset(&nro, 0, sizeof(nro));
    nro.image = memory;
    nro.image_size = MEMORY;
    nro.text_off = NRO;
    nro.text_size = 0x2000;
    over_main = nro;
    over_main.text_off = CODE_HI - 0x1000;
    put_nro(7);
    CHECK(!snx_jit_resolve(service, NRO + 0x1000)); /* not added yet: refused */
    CHECK(snx_jit_add_image(service, &nro) == 0);
    CHECK(snx_jit_add_image(service, &nro) == -1 && snx_jit_add_image(service, &over_main) == -1);
    /* NRO + 0x1000: x0 + 7 + 100, compiled at the first miss, then found in the service's table for the image, which
       reaches the AOT function directly */
    for (i = 0; i < 2; ++i) {
        cpu_init(&c, 0xF000);
        c.x[0] = 1;
        run(&c, NRO + 0x1000);
        CHECK(c.x[0] == 1 + 7 + 100 && c.pc == RETURN);
        snx_jit_drain(service);
    }
    CHECK(snx_jit_resolve(service, NRO + 0x1000) != NULL);
    /* removed: refused, and its code forgotten. The image mapped again with other code there compiles again */
    snx_jit_remove_image(service, 0);
    CHECK(!snx_jit_resolve(service, NRO + 0x1000));
    put_nro(9);
    CHECK(snx_jit_add_image(service, &nro) == 0);
    for (i = 0; i < 2; ++i) {
        cpu_init(&c, 0xF000);
        c.x[0] = 1;
        run(&c, NRO + 0x1000);
        CHECK(c.x[0] == 1 + 9 + 100);
        snx_jit_drain(service);
    }
    /* every thread misses the image's functions, and the image goes while their batches compile: nothing of it is
       published after, and mapped again, every function compiles and runs */
    barrier_init(&barrier);
    for (i = 0; i < THREADS; ++i) {
        cpu_init(&racers[i].cpu, 0x8800 + 0x800 * (u64)i);
        racers[i].barrier = &barrier;
        racers[i].index = i;
        CHECK(!pthread_create(&threads[i], NULL, run_nro, &racers[i]));
    }
    for (i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    barrier_destroy(&barrier);
    snx_jit_remove_image(service, 0);
    for (i = 1; i < NRO_COUNT; ++i) CHECK(!snx_jit_resolve(service, NRO + 0x40 * (u64)i));
    snx_jit_drain(service);
    CHECK(snx_jit_add_image(service, &nro) == 0);
    before_misses = misses;
    barrier_init(&barrier);
    for (i = 0; i < THREADS; ++i) {
        cpu_init(&racers[i].cpu, 0x8800 + 0x800 * (u64)i);
        racers[i].barrier = &barrier;
        CHECK(!pthread_create(&threads[i], NULL, run_nro, &racers[i]));
    }
    for (i = 0; i < THREADS; ++i) pthread_join(threads[i], NULL);
    barrier_destroy(&barrier);
    snx_jit_drain(service);
    for (i = 1; i < NRO_COUNT; ++i) CHECK(snx_jit_resolve(service, NRO + 0x40 * (u64)i) != NULL);
    printf("added image: %u misses, every function compiled after the image went and came back\n",
           misses - before_misses);
    snx_jit_service_free(service);
}

/* x0 from f1 at 0x1000 (calls the AOT function and 0x3000) and 0x2800 */
static u64 run_pair(void) {
    CPU c;
    u64 sum;
    cpu_init(&c, 0xF000);
    c.x[0] = 1;
    run(&c, 0x1000);
    sum = c.x[0];
    cpu_init(&c, 0xF000);
    c.x[0] = 1;
    run(&c, 0x2800);
    return sum * 1000 + c.x[0];
}

static void test_cache(void) {
    static const u32 f3_changed[] = {0x91002800, 0xD65F03C0}; /* 0x3000: x0 += 10 */
    static const u32 f3_again[] = {0x91001400, 0xD65F03C0};   /* main's f3: x0 += 5 */
    char path[512];
    SnxJitStats stats;
    FILE* f;
    long size;
    u64 expected = (1 * 3 + 1 + 100 + 5) * 1000 + 101;
    snprintf(path, sizeof(path), "snx-test-jit-cache.bin"); /* in the test's directory */
    remove(path);
    cache_path = path;
    /* the first service compiles and writes */
    service = start(0, 1);
    if (!service) {
        ++failures;
        return;
    }
    CHECK(run_pair() == expected);
    snx_jit_drain(service);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiled >= 2 && !stats.cached);
    snx_jit_service_free(service);
    /* the second loads them */
    service = start(0, 1);
    if (!service) {
        ++failures;
        return;
    }
    CHECK(run_pair() == expected);
    snx_jit_drain(service);
    snx_jit_stats(service, &stats);
    printf("cache: second run %lu units loaded, %lu pcs compiled\n", stats.cached, stats.compiled);
    CHECK(stats.cached >= 1 && !stats.compiled);
    snx_jit_service_free(service);
    /* 0x3000 changes: its unit is refused, the new code compiles */
    put(0x3000, f3_changed, 2);
    service = start(0, 1);
    if (service) {
        CHECK(run_pair() == (1 * 3 + 1 + 100 + 10) * 1000 + 101);
        snx_jit_drain(service);
        snx_jit_stats(service, &stats);
        CHECK(stats.compiled >= 1);
        snx_jit_service_free(service);
    }
    put(0x3000, f3_again, 2);
    /* a record cut off: the file still opens, and the run is right */
    f = fopen(path, "rb");
    size = -1;
    if (f && !fseek(f, 0, SEEK_END)) size = ftell(f);
    if (f) fclose(f);
    CHECK(size > 64);
    if (size > 64) {
        unsigned char* bytes = (unsigned char*)malloc((size_t)size);
        f = fopen(path, "rb");
        if (bytes && f && fread(bytes, 1, (size_t)size, f) == (size_t)size) {
            fclose(f);
            f = fopen(path, "wb");
            if (f) fwrite(bytes, 1, (size_t)size - 7, f);
        }
        if (f) fclose(f);
        free(bytes);
    }
    service = start(0, 1);
    if (service) {
        CHECK(run_pair() == expected);
        snx_jit_service_free(service);
    }
    remove(path);
    cache_path = NULL;
}

/* invalidated code: its slot and entry go, the next lookup queues it, and it compiles again into new code */
static void test_invalidate(void) {
    static const u32 words[] = {0x91000400, 0xD65F03C0}; /* add x0, x0, #1; ret */
    GuestFn before, after;
    CPU c;
    service = start(0, 2);
    if (!service) {
        ++failures;
        return;
    }
    put(0x3800, words, 2);
    cpu_init(&c, 0xF000);
    run(&c, 0x3800);
    CHECK(c.x[0] == 1 && c.pc == RETURN);
    snx_jit_drain(service);
    before = snx_jit_resolve(service, 0x3800);
    CHECK(before != NULL && get(0x3800) == before);
    /* the range's code is stale: the main table's slot and the service's entry for it go */
    snx_jit_invalidate(service, 0x3800, 4);
    CHECK(get(0x3800) == NULL);
    CHECK(snx_jit_resolve(service, 0x3800) == NULL); /* queued again */
    snx_jit_drain(service);
    after = snx_jit_resolve(service, 0x3800);
    CHECK(after != NULL && after != before && get(0x3800) == after);
    cpu_init(&c, 0xF000);
    run(&c, 0x3800);
    CHECK(c.x[0] == 1 && c.pc == RETURN);
    snx_jit_service_free(service);
}

/* a change in the middle of a compiled function, its entry outside the range, as a mod's hook patches an instruction
   and invalidates its line (IC IVAU): the whole unit goes, so the next call runs the new instruction */
static void test_invalidate_middle(void) {
    static const u32 words[] = {0x91000400, 0x91000400, 0xD65F03C0}; /* add x0,x0,#1; add x0,x0,#1; ret */
    static const u32 patch = 0x91002800;                               /* add x0,x0,#10 */
    GuestFn before, after;
    CPU c;
    service = start(0, 2);
    if (!service) {
        ++failures;
        return;
    }
    put(0x3A00, words, 3);
    cpu_init(&c, 0xF000);
    run(&c, 0x3A00);
    CHECK(c.x[0] == 2 && c.pc == RETURN);
    snx_jit_drain(service);
    before = snx_jit_resolve(service, 0x3A00);
    CHECK(before != NULL && get(0x3A00) == before);
    put(0x3A04, &patch, 1);
    snx_jit_invalidate(service, 0x3A04, 4);
    CHECK(get(0x3A00) == NULL); /* the entry, outside the range, went with its unit */
    CHECK(snx_jit_resolve(service, 0x3A00) == NULL);
    snx_jit_drain(service);
    after = snx_jit_resolve(service, 0x3A00);
    CHECK(after != NULL && after != before);
    cpu_init(&c, 0xF000);
    run(&c, 0x3A00);
    CHECK(c.x[0] == 11 && c.pc == RETURN);
    snx_jit_service_free(service);
}

/* an invalidation of another range while a pc compiles: the batch read code that may be stale, so it publishes nothing
   and its pcs go back to their queue. Before, they were left GONE with their pending bit set, which snx_jit_resolve's
   fast path reads as "queued, wait for it": nothing queued them again, and the pc was never compiled (rt_miss's fatal,
   DQ7 at 98f1fc4). Each round invalidates elsewhere until its batch ends, so the batch reads the epoch before one of
   them and is dropped, unless it was quicker than the first; a run where none was dropped proves nothing and fails.
   The function is long so its batch outlasts a scheduler slice: on one CPU (a loaded runner, taskset -c 0) the woken
   compile thread ran a short batch to its end before this thread ran again, and no round dropped one */
#define IN_FLIGHT_PC   0x10000
#define IN_FLIGHT_ADDS 4000
/* a spin of a round: an invalidation elsewhere and the stats, then the processor to the others. A lock taken again at
   once by the thread that let it go (Windows' SRW lock is not fair) leaves the compile thread waiting for it, its
   batch never begun */
static void in_flight_spin(SnxJitStats* stats) {
    snx_jit_invalidate(service, 0x8000, 0x10);
    snx_jit_stats(service, stats);
#ifdef _WIN32
    SwitchToThread();
#else
    sched_yield();
#endif
}
static void test_invalidate_in_flight(void) {
    static const u32 words[] = {0x91000400, 0xD65F03C0}; /* add x0, x0, #1; ret */
    static const SnxU64 warm_pc[] = {0x4800};
    static u32 long_words[IN_FLIGHT_ADDS + 1];
    SnxJitStats stats;
    unsigned long batches, dropped;
    time_t until;
    unsigned round;
    size_t i;
    service = start(1, 1);
    if (!service) {
        ++failures;
        return;
    }
    for (i = 0; i < IN_FLIGHT_ADDS; ++i) long_words[i] = words[0];
    long_words[IN_FLIGHT_ADDS] = words[1];
    put(IN_FLIGHT_PC, long_words, IN_FLIGHT_ADDS + 1);
    snx_jit_stats(service, &stats);
    dropped = stats.dropped;
    for (round = 0; round < 8; ++round) {
        snx_jit_invalidate(service, IN_FLIGHT_PC, 4); /* the pc is new to the service again */
        snx_jit_stats(service, &stats);
        batches = stats.batches;
        CHECK(snx_jit_resolve(service, IN_FLIGHT_PC) == NULL);
        for (until = time(NULL) + 60; stats.batches == batches && time(NULL) < until;) in_flight_spin(&stats);
        CHECK(stats.batches != batches);
        CHECK(snx_jit_wait(service, IN_FLIGHT_PC) != NULL); /* queued again, and compiled once nothing changes */
    }
    /* a warm pc goes back to the warm queue, and is compiled from there */
    put(0x4800, words, 2);
    snx_jit_stats(service, &stats);
    batches = stats.batches;
    CHECK(snx_jit_warm(service, warm_pc, 1) == 1);
    for (until = time(NULL) + 60; stats.batches == batches && time(NULL) < until;) in_flight_spin(&stats);
    snx_jit_drain(service);
    CHECK(get(0x4800) != NULL);
    snx_jit_stats(service, &stats);
    printf("invalidate in flight: %lu batches dropped in 9 rounds, each pc compiled again\n", stats.dropped - dropped);
    CHECK(stats.dropped > dropped);
    snx_jit_service_free(service);
}

/* added images in different 16 MiB chunks: the page map finds each, and removing one leaves the other's pages */
static void test_image_map(void) {
    static const u32 far_words[] = {0x91000400, 0xD65F03C0}; /* add x0, x0, #1; ret */
    SnxCodeImage image;
    CPU c;
    service = start(0, 1);
    if (!service) {
        ++failures;
        return;
    }
    put(0x38000, far_words, 2);
    memset(&image, 0, sizeof(image));
    image.image = memory + 0x38000;
    image.image_size = 8;
    image.base = 0x38000;
    image.text_size = 8;
    CHECK(snx_jit_add_image(service, &image) == 0);
    memset(&image, 0, sizeof(image));
    image.image = memory + 0x38000; /* the same code, in another chunk: the map has to cover both */
    image.image_size = 8;
    image.base = 0x2100000;
    image.text_size = 8;
    CHECK(snx_jit_add_image(service, &image) == 0);
    cpu_init(&c, 0xF000);
    run(&c, 0x38000);
    run(&c, 0x2100000);
    CHECK(c.x[0] == 2);
    snx_jit_drain(service);
    CHECK(snx_jit_resolve(service, 0x38000) != NULL);
    CHECK(snx_jit_resolve(service, 0x2100000) != NULL);
    snx_jit_remove_image(service, 0x38000);
    CHECK(snx_jit_resolve(service, 0x38000) == NULL);
    CHECK(snx_jit_resolve(service, 0x2100000) != NULL);
    snx_jit_service_free(service);
}

int main(void) {
    /* 0x1000: x0 = x0 * 3 + 1, then the AOT function at 0x2000, then 0x3000, which the same batch may find */
    static const u32 f1[] = {0xD2800061, 0x9B017C00, 0x91000400, 0xA9BF7BFD,
                             0x940003FC, 0x940007FB, 0xA8C17BFD, 0xD65F03C0};
    static const u32 f3[] = {0x91001400, 0xD65F03C0}; /* 0x3000: x0 += 5 */
    /* 0x3800: x0 = 2 * f1(x0), calling 0x1000 through a register: CALLDYN to code of another unit */
    static const u32 f4[] = {0xA9BF7BFD, 0xD2820002, 0xD63F0040, 0x8B000000, 0xA8C17BFD, 0xD65F03C0};
    /* 0x2800: x0 += 100 in a loop, with a backward branch */
    static const u32 f5[] = {0xD2800C83, 0x91000400, 0xD1000463, 0xB5FFFFC3, 0xD65F03C0};
    static const u32 f6[] = {0x91002400, 0xD65F03C0}; /* 0x3400: x0 += 9 */
    /* 0x3C00 calls 0x3E00 (x0 += 3): threads miss both at once, so two batches may each hold 0x3E00 */
    static const u32 f7[] = {0xA9BF7BFD, 0x9400007F, 0xA8C17BFD, 0xD65F03C0};
    static const u32 f8[] = {0x91000C00, 0xD65F03C0};
    /* the bank's function i: x2 = i plus eight words at x1, then x0 += x2 */
    u32 bank[8] = {0xD2800002, 0xD2800103, 0xF8408424, 0x8B040042, 0xD1000463, 0xB5FFFFA3, 0x8B020000, 0xD65F03C0};
    static const SnxU64 warm[] = {0x1000, 0x2800, 0x3C00, 0x2000, 0x1002, CODE_HI};
    SnxJitStats stats, batched, single;
    Racer racers[THREADS];
    unsigned long before;
    unsigned before_misses, i;
    static SnxU64 bank_pcs[BANK_COUNT];
    SnxU64 lowest = ~(SnxU64)0, highest = 0, address;
    CPU c;
    memory = (unsigned char*)calloc(1, MEMORY);
    if (!memory) return 1;
    put(0x1000, f1, 8);
    put(0x3000, f3, 2);
    put(0x3800, f4, 6);
    put(0x2800, f5, 5);
    put(0x3400, f6, 2);
    put(0x3C00, f7, 4);
    put(0x3E00, f8, 2);
    for (i = 0; i < BANK_COUNT; ++i) {
        bank[0] = 0xD2800002 | (u32)i << 5;
        put(BANK + 0x40 * (u64)i, bank, 8);
    }
    rt.miss = miss;
    rt.poll = poll;
    rt.undef = undef;
    service = start(0, 0);
    if (!service) return 1;

    /* a miss at 0x1000 waits for the batch that compiles it with 0x3000, which it calls: both published, with 0x1000's
       leaders, by the time the miss runs the code */
    cpu_init(&c, 0xF000);
    c.x[0] = 7;
    run(&c, 0x1000);
    CHECK(c.x[0] == 7 * 3 + 1 + 100 + 5 && c.pc == RETURN && c.x[31] == 0xF000);
    CHECK(misses >= 1 && misses <= 2);
    snx_jit_drain(service);
    CHECK(get(0x1000) && get(0x1014) && get(0x1018) && get(0x3000));
    snx_jit_stats(service, &stats);
    CHECK(stats.compiled >= 1 && stats.compiled <= 2 && stats.functions >= 2 && !stats.failures && !stats.queued);
    /* now without a miss, in the compiled code */
    before_misses = misses;
    cpu_init(&c, 0xF000);
    c.x[0] = 7;
    run(&c, 0x1000);
    CHECK(c.x[0] == 7 * 3 + 1 + 100 + 5 && c.pc == RETURN && misses == before_misses);
    /* the JIT's call to 0x2000 is direct: what its slot holds now doesn't matter */
    __atomic_store_n(slot(0x2000), aot_poisoned, __ATOMIC_RELAXED);
    cpu_init(&c, 0xF000);
    c.x[0] = 1;
    run(&c, 0x1000);
    CHECK(c.x[0] == 1 * 3 + 1 + 100 + 5 && misses == before_misses);

    /* 0x3800 calls 0x1000 through the table: a miss the first time, then compiled code, the same either way */
    for (i = 0; i < 2; ++i) {
        cpu_init(&c, 0xF000);
        c.x[0] = 1;
        run(&c, 0x3800);
        CHECK(c.x[0] == 2 * (1 * 3 + 1 + 100 + 5) && c.pc == RETURN);
        snx_jit_drain(service);
    }
    CHECK(misses == before_misses + 1 && get(0x3800));

    /* threads missing 0x2800 together: each waits for the one compile it is queued for */
    snx_jit_stats(service, &stats);
    before = stats.compiled;
    before_misses = misses;
    for (i = 0; i < THREADS; ++i) {
        cpu_init(&racers[i].cpu, 0x8800 + 0x800 * (u64)i);
        racers[i].cpu.x[0] = (u64)i;
        racers[i].pc = 0x2800;
        racers[i].bank = 0;
    }
    race_all(racers);
    for (i = 0; i < THREADS; ++i) CHECK(racers[i].cpu.x[0] == (u64)i + 100 && racers[i].cpu.pc == RETURN);
    snx_jit_drain(service);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiled == before + 1 && misses - before_misses >= 1 && misses - before_misses <= THREADS);
    CHECK(get(0x2800) != NULL);

    /* half the threads miss a caller, half its callee: whichever compiles 0x3E00, every thread gets x0 + 3 */
    before = stats.compiled;
    for (i = 0; i < THREADS; ++i) {
        cpu_init(&racers[i].cpu, 0x8800 + 0x800 * (u64)i);
        racers[i].cpu.x[0] = (u64)i;
        racers[i].pc = i & 1 ? 0x3E00 : 0x3C00;
    }
    race_all(racers);
    for (i = 0; i < THREADS; ++i) CHECK(racers[i].cpu.x[0] == (u64)i + 3 && racers[i].cpu.pc == RETURN);
    snx_jit_drain(service);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiled - before >= 1 && stats.compiled - before <= 2 && get(0x3C00) && get(0x3E00));
    before = stats.compiled;

    /* a wrapper in an empty slot: the first miss waits for 0x3400's compile, the second finds the code in the service's
       table */
    __atomic_store_n(slot(0x3400), wrapper, __ATOMIC_RELAXED);
    before_misses = misses;
    for (i = 0; i < 2; ++i) {
        cpu_init(&c, 0xF000);
        c.x[0] = 1;
        run(&c, 0x3400);
        CHECK(c.x[0] == 10 && c.pc == RETURN);
        snx_jit_drain(service);
    }
    snx_jit_stats(service, &stats);
    CHECK(wrapped == 2 && misses == before_misses + 2);
    CHECK(stats.compiled == before + 1 && get(0x3400) == wrapper);

    /* refused: unaligned, outside the text */
    CHECK(!snx_jit_resolve(service, 0x1002) && !snx_jit_resolve(service, CODE_HI));
    snx_jit_drain(service);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiled == before + 1 && !stats.failures && stats.bytes > 0);
    snx_jit_service_free(service);

    /* the bank, missed by THREADS threads at once: on every compile thread there can be, then one pc per compile on
       one thread. Each thread waits for its miss, so the queue holds at most THREADS pcs and the batches are small:
       the compile threads work in parallel instead (the warm start below takes large batches) */
    service = start(0, SNX_JIT_THREADS);
    if (!service) return 1;
    run_bank("batched", &batched);
    CHECK(batched.compiled == BANK_COUNT && batched.batches <= BANK_COUNT);
    CHECK(!wx_mappings());
    snx_jit_service_free(service);
    service = start(1, 1);
    if (!service) return 1;
    run_bank("one pc per compile", &single);
    CHECK(single.compiled == BANK_COUNT && single.batches == BANK_COUNT);
    /* packed: a load per function, and their code within a few hundred bytes of each other, not a page apart */
    for (i = 0; i < BANK_COUNT; ++i) {
        address = (SnxU64)(size_t)get(BANK + 0x40 * (u64)i);
        if (address < lowest) lowest = address;
        if (address > highest) highest = address;
    }
    printf("packing: %d loads of one function each span %lu KiB of code, %lu bytes per load\n", BANK_COUNT,
           (unsigned long)((highest - lowest) >> 10), (unsigned long)((highest - lowest) / (BANK_COUNT - 1)));
    CHECK(highest - lowest < (SnxU64)BANK_COUNT * 1024);
    CHECK(!wx_mappings());
    snx_jit_service_free(service);

    /* warm start: queued pcs compile with no miss (the AOT entry and the refused ones aren't queued) */
    service = start(0, 1);
    if (!service) return 1;
    CHECK(snx_jit_warm(service, warm, sizeof(warm) / sizeof(warm[0])) == 3);
    snx_jit_drain(service);
    CHECK(get(0x1000) && get(0x2800) && get(0x3C00) && get(0x3E00) && get(0x3000));
    before_misses = misses;
    cpu_init(&c, 0xF000);
    c.x[0] = 1;
    run(&c, 0x1000);
    CHECK(c.x[0] == 1 * 3 + 1 + 100 + 5 && misses == before_misses);
    snx_jit_service_free(service);
    /* a miss goes ahead of the warm queue: the bank queued warm, then a miss at 0x3800 (and 0x1000, which it calls
       through the table), compiled by one of the first batches on the one compile thread while the bank takes many
       more */
    service = start(0, 1);
    if (!service) return 1;
    for (i = 0; i < BANK_COUNT; ++i) bank_pcs[i] = BANK + 0x40 * (SnxU64)i;
    CHECK(snx_jit_warm(service, bank_pcs, BANK_COUNT) == BANK_COUNT);
    cpu_init(&c, 0xF000);
    c.x[0] = 1;
    run(&c, 0x3800);
    CHECK(c.x[0] == 2 * (1 * 3 + 1 + 100 + 5));
    snx_jit_drain(service);
    printf("warm start: %d pcs and 3 misses in %u batches, the miss at 0x3800 compiled by batch %u\n", BANK_COUNT,
           batch_count, live_batch);
    CHECK(live_batch <= 2 && batch_count > 8);
    snx_jit_stats(service, &stats);
    CHECK(stats.compiled + stats.failures >= BANK_COUNT + 1 && !stats.failures);
    snx_jit_service_free(service);

    test_added_image();
    test_cache();
    test_invalidate();
    test_invalidate_middle();
    test_invalidate_in_flight();
    test_image_map();
    free(memory);
    if (failures) return 1;
    printf("jit service: passed\n");
    return 0;
}
