/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module of a 64-bit program: its functions found (module.c's analysis), made in units on threads by the
   host's native back end (recompiler/mach/native.h), what it leaves by the fallback (recompiler/fallback.h), and the
   descriptor of module_abi.h, linked with the runtime library by recompiler/object/link.c. Where the native back end
   made every function the descriptor is its own (module_x64.c), else the fallback's (module_fallback.h) */
#include "nx/loader/module_fallback.h"
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/auto_workers.h"
#include "common/cpu/recompiler/compile_thread.h"
#include "nx/loader/codegen_view.h"
#include "nx/loader/ounce.h"
#include "common/cpu/recompiler/object/link.h"
#include "common/cpu/recompiler/object/objects_pack.h"
#include "nx/loader/module_abi.h"
#include "common/utilities/bits.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "nx/loader/module_x64.h"
#include "snx_settings.h"
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

typedef struct {
    size_t first, last; /* functions */
    unsigned char* object;
    size_t size;
    char* stats; /* the fallback's IR counts of the unit (ir_stats), NULL for none */
    /* the x86-64 back end's object of the functions it made (x64_codegen.h); the fallback's (object) has the rest, NULL
       when there is none */
    unsigned char* x64;
    size_t x64_size, x64_made;
    size_t x64_refused; /* of the others, those with an instruction it has no code for */
    int reused; /* the x86-64 back end's object came from memory (x64_cache.h) */
} Unit;

#define NO_RANK ((size_t)-1)

typedef struct {
    SnxCodegen* codegen;
    const SnxLlvmBuild* build;
    /* the module's bound branches (SnxCodegenOptions.bound) by address, and the functions of other modules among
       their targets, each once, ascending: its imports (snx_imports) */
    const SnxBoundBranch* bound;
    size_t bound_count;
    SnxU64* imports;
    size_t import_count;
    Unit* units;
    size_t* rank; /* per function: its place in the --layout order, or NO_RANK. NULL without one */
    size_t count, next, done;
    /* for the control of threads left open (auto_workers.h): the units under way, and the bytes of code the finished
       ones keep in memory */
    unsigned active;
    SnxU64 held;
    int failed;
    int x64; /* the x86-64 back end makes what it can */
    char error[1024];
#ifdef _WIN32
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
} Queue;

static void lock(Queue* q) {
#ifdef _WIN32
    EnterCriticalSection(&q->lock);
#else
    pthread_mutex_lock(&q->lock);
#endif
}
static void unlock(Queue* q) {
#ifdef _WIN32
    LeaveCriticalSection(&q->lock);
#else
    pthread_mutex_unlock(&q->lock);
#endif
}

/* The pack's write (snx_objects_pack_write) of inputs, some of them the units' objects, LLVM's or a native back end's:
   each freed once written */
typedef struct {
    Unit* units;
    const SnxLinkInput* inputs;
    size_t count;
} PackUnits;
static void written_unit(void* arg, size_t index) {
    PackUnits* p = (PackUnits*)arg;
    const unsigned char* data = p->inputs[index].data;
    for (size_t i = 0; data && i < p->count; ++i) {
        Unit* u = &p->units[i];
        if (u->object == data) {
            free(u->object);
            u->object = NULL;
            return;
        }
        if (u->x64 == data) {
            free(u->x64);
            u->x64 = NULL;
            return;
        }
    }
}

/* SnxNativeOptions.bound: the function the BR at address reaches once it is bound, 0 for none, and whether its slot
   may hold another */
static SnxU64 bound_at(void* user, SnxU64 address, int* guard) {
    const Queue* q = (const Queue*)user;
    size_t lo = 0, hi = q->bound_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (q->bound[middle].at < address)
            lo = middle + 1;
        else
            hi = middle;
    }
    if (lo == q->bound_count || q->bound[lo].at != address) return 0;
    *guard = q->bound[lo].guard;
    return q->bound[lo].target;
}
/* SnxNativeOptions.direct: nonzero for a target the module imports, which its snx_aot_<target> calls */
static SnxU64 imported(void* user, SnxU64 target) {
    const Queue* q = (const Queue*)user;
    size_t lo = 0, hi = q->import_count;
    while (lo < hi) {
        size_t middle = lo + (hi - lo) / 2;
        if (q->imports[middle] < target)
            lo = middle + 1;
        else
            hi = middle;
    }
    return lo < q->import_count && q->imports[lo] == target;
}

/* one chunk of functions, as module.c's code_NNN.c: the native back end's where it makes them, the fallback's the rest.
   A function of the --layout order gets its own code section, which the linker places by its rank: the function's
   code is the same, only where it goes changes */
static int compile_unit(Queue* q, size_t index, char* error, size_t error_size) {
    Unit* u = &q->units[index];
    char name[32];
    size_t i;
    int rc;
    unsigned char* made = NULL;
    SnxNativeOptions options = {0};
    sprintf(name, "code_%03lu", (unsigned long)index);
    options.stale = 1;
    options.llvm_at = snx_fallback_dump_pc();
    /* with no fallback, an instruction it has no code for is undefined where it runs, and its function is made */
    options.alone = !snx_fallback_available();
    options.refused = &u->x64_refused;
    if (q->bound_count) {
        options.bound = bound_at;
        options.bound_user = q;
    }
    if (q->import_count) {
        options.direct = imported;
        options.direct_user = q;
    }
    if (q->x64) {
        made = (unsigned char*)snx_alloc(u->last - u->first);
        rc = q->build->triple ? snx_native_target_object(q->build->triple, q->codegen, u->first, u->last, &options,
                                                         made, &u->x64, &u->x64_size, error, error_size)
                              : snx_native_cached_object(q->codegen, u->first, u->last, &options, made, &u->x64,
                                                         &u->x64_size, &u->reused, error, error_size);
        if (rc) {
            free(made);
            return -1;
        }
        for (i = 0; i < u->last - u->first; ++i) u->x64_made += made[i];
        if (u->x64_made == u->last - u->first) {
            free(made);
            return 0;
        }
    }
    u->reused = 0; /* the fallback makes the rest of the unit */
    SnxFallbackUnit unit = {.triple = q->build->triple,
                            .tuning = q->build->tuning,
                            .keep_ir = q->build->keep_ir,
                            .rank = q->rank,
                            .callees_first = 1};
    rc = snx_fallback_object(q->codegen, u->first, u->last, &options, made, &unit, name, q->build->opt, &u->object,
                             &u->size, error, error_size);
    u->stats = unit.stats;
    free(made);
    return rc;
}

static void worker(Queue* q) {
    char error[1024], name[32], *label;
    double start, seconds;
    for (;;) {
        size_t i;
        int rc;
        lock(q);
        if (snx_abort_requested() && !q->failed) {
            q->failed = 1;
            snprintf(q->error, sizeof(q->error), "aborted by the player, %zu of %zu code units compiled", q->done,
                     q->count);
        }
        if (q->failed || q->next == q->count) {
            unlock(q);
            return;
        }
        i = q->next++;
        ++q->active;
        unlock(q);
        error[0] = 0;
        start = snx_seconds();
        rc = compile_unit(q, i, error, sizeof(error));
        seconds = snx_seconds() - start;
        label = rc || q->build->quiet ? NULL : snx_chunk_label(q->codegen, q->units[i].first, q->units[i].last);
        lock(q);
        ++q->done;
        --q->active;
        if (q->units[i].object) q->held += q->units[i].size;
        if (rc && !q->failed) {
            q->failed = 1;
            snprintf(q->error, sizeof(q->error), "%s", error);
        }
        if (label) {
            /* every unit: a line every few seconds or minutes, whichever the machine manages */
            sprintf(name, "code_%03lu", (unsigned long)i);
            snx_report_unit(q->done, q->count + 1, name, label, q->units[i].reused, seconds); /* + the descriptor */
            if (q->units[i].stats) printf("  IR stats %s: %s\n", name, q->units[i].stats);
        }
        unlock(q);
        free(label);
    }
}
#ifdef _WIN32
static DWORD WINAPI thread_main(LPVOID p) {
    worker((Queue*)p);
    return 0;
}
#else
static void* thread_main(void* p) {
    worker((Queue*)p);
    return NULL;
}
#endif

/* the queue as the control of threads left open reads it */
static void queue_state(void* arg, SnxAotReading* r) {
    Queue* q = (Queue*)arg;
    lock(q);
    r->active = q->active;
    r->done = q->done;
    r->left = q->failed ? 0 : q->count - q->next;
    r->held = q->held;
    unlock(q);
}
static void queue_worker(void* arg) { worker((Queue*)arg); }

/* jobs threads, or with open set as many of them as the memory holds, opened as the units are measured */
static void run(Queue* q, unsigned jobs, int open) {
    unsigned i, created = 0;
#ifdef _WIN32
    HANDLE* threads;
    InitializeCriticalSection(&q->lock);
#else
    pthread_t* threads;
    pthread_mutex_init(&q->lock, NULL);
#endif
    if (jobs > q->count) jobs = (unsigned)q->count;
    if (jobs < 1) jobs = 1;
    if (open) {
        SnxAutoWorkers w = {0};
        w.worker = queue_worker;
        w.state = queue_state;
        w.arg = q;
        w.most = jobs;
        w.most_key = open == 2;
        w.quiet = q->build->quiet;
        snx_run_workers_auto(&w);
#ifdef _WIN32
        DeleteCriticalSection(&q->lock);
#else
        pthread_mutex_destroy(&q->lock);
#endif
        return;
    }
    threads = snx_alloc(jobs * sizeof(*threads));
    for (i = 1; i < jobs; ++i) {
#ifdef _WIN32
        HANDLE h = snx_compile_thread(thread_main, q);
        if (!h) break;
        threads[created++] = h;
#else
        if (snx_compile_thread(&threads[created], thread_main, q)) break;
        ++created;
#endif
    }
    worker(q);
    for (i = 0; i < created; ++i) {
#ifdef _WIN32
        WaitForSingleObject(threads[i], INFINITE);
        CloseHandle(threads[i]);
#else
        pthread_join(threads[i], NULL);
#endif
    }
#ifdef _WIN32
    DeleteCriticalSection(&q->lock);
#else
    pthread_mutex_destroy(&q->lock);
#endif
    free(threads);
}

unsigned snx_module_abi(void) { return SNX_ABI; }
/* what a failure the fallback reports starts with: its own name, where it is LLVM */
static const char* fallback_said(void) { return snx_fallback_available() ? "LLVM backend: " : ""; }
static const char* base_name(const char* path) {
    const char *name = path, *p;
    for (p = path; *p; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

int snx_build_module_llvm(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxLlvmBuild* build) {
    SnxCodegen codegen;
    Queue q;
    SnxJitCache* cache = NULL;
    SnxLinkInput* inputs = NULL;
    SnxLinkOutput linked;
    unsigned char *module_object = NULL, *rtlib_object = NULL;
    size_t module_size = 0, rtlib_size = 0, first, last, count, limit, i, entry_count = 0, wrapped = 0, n = 0;
    size_t cache_count = 0, variant_count = 0, export_count = 1;
    const char* exports[5];
    char error[1024], *tmp = NULL;
    double start;
    int rc = -1, x64_only = 0, rtlib_static = 0;
    memset(&codegen, 0, sizeof(codegen));
    memset(&q, 0, sizeof(q));
    memset(&linked, 0, sizeof(linked));
    if (!loaded->module_count) return snx_error("no modules to generate");
    /* what LLVM's code alone has: its IR, and its functions' own sections */
    if (!snx_fallback_available() && build->keep_ir)
        return snx_error("--keep-ir: the IR it keeps in %s is LLVM's, and this build has no LLVM (SNX_WITH_LLVM=OFF)",
                         build->keep_ir);
    if (!snx_fallback_available() && build->layout_count)
        return snx_error("--layout: the order of its %zu addresses puts LLVM's functions in sections of their own, and "
                         "this build has no LLVM (SNX_WITH_LLVM=OFF)",
                         build->layout_count);
    if (options->jit_cache && build->triple) {
        snx_error("--jit-cache cannot go with --target %s: a JIT cache is this host's", build->triple);
        goto done;
    }
    /* the program's CPU (loader/ounce.h): the runtime JIT takes the same features from the same NPDM */
    SnxU64 features = snx_guest_features(loaded->npdm.ounce, g_settings.cpu_extensions);
    if (options->jit_cache) {
        char* key = snx_module_fallback_cache_key(options->jit_cache, build->opt, options->hook_direct_calls, features,
                                                  error, sizeof(error));
        if (!key) {
            snx_error("%s%s", fallback_said(), error);
            goto done;
        }
        cache = snx_jit_cache_open(options->jit_cache, key, error, sizeof(error));
        free(key);
        if (!cache) {
            snx_error("JIT cache: %s", error);
            goto done;
        }
        cache_count = snx_jit_cache_count(cache);
    }
    if (!options->interp_only &&
        snx_nx_codegen_analyze(&codegen, loaded, options->hints, options->hint_count, options->quiet, features))
        goto done;
    if (build->found) *build->found = codegen.instruction_count;
    codegen.features = features;
    codegen.direct_svc = 1; /* the runtime's RtAPI.svcs (svc.c's g_svc64) */
    codegen.hook_direct_calls = options->hook_direct_calls;
    /* chunks of whole functions, as module.c splits the C */
    limit = options->chunk_instrs ? options->chunk_instrs : (size_t)g_settings.unit_size;
    q.units = (Unit*)snx_alloc((codegen.function_count + 1) * sizeof(*q.units));
    memset(q.units, 0, (codegen.function_count + 1) * sizeof(*q.units));
    for (first = 0; first < codegen.function_count; first = last) {
        last = first;
        count = 0;
        do { count += codegen.functions[last++].count; } while (last < codegen.function_count && count < limit);
        q.units[q.count].first = first;
        q.units[q.count++].last = last;
    }
    q.codegen = &codegen;
    q.build = build;
    /* the host's native back end for this host's module (mach/native.h), or the one that makes another machine's
       objects (snx_native_target: iOS's), the fallback making what it can't: not where the IR is kept or counted
       (ir_stats), nor with a --layout (each function's own hot section is LLVM's). With every call through the table
       (hook_direct_calls) its calls go through CPU.disp too */
    const char* native = build->triple ? "arm64" : snx_native_name();
    q.x64 = (build->triple ? snx_native_target(build->triple) : snx_native_available()) && !build->keep_ir &&
            !build->layout_count && !snx_fallback_stats();
    if (!q.x64 && !snx_fallback_available()) {
        if (build->triple)
            snx_error("the arm64 back end makes no module for %s on this host, and this build has no LLVM to make it "
                      "(SNX_WITH_LLVM=OFF)",
                      build->triple);
        else
            snx_error("this host has no native back end, and this build has no LLVM to make the module "
                      "(SNX_WITH_LLVM=OFF)");
        goto done;
    }
    /* the bound branches, and what they import: a target that is no function of this module's own. With every call
       through CPU.disp (--hook-direct-calls) none is bound */
    if (options->bound_count && !options->hook_direct_calls) {
        q.bound = options->bound;
        q.bound_count = options->bound_count;
        q.imports = (SnxU64*)snx_alloc(q.bound_count * sizeof(*q.imports));
        for (i = 0; i < q.bound_count; ++i)
            if (q.bound[i].game && !snx_codegen_function(&codegen, q.bound[i].target))
                q.imports[q.import_count++] = q.bound[i].target;
        qsort(q.imports, q.import_count, sizeof(*q.imports), snx_compare_u64);
        for (i = 0, n = 0; i < q.import_count; ++i)
            if (!n || q.imports[n - 1] != q.imports[i]) q.imports[n++] = q.imports[i];
        q.import_count = n;
        n = 0;
    }
    if (build->layout_count) {
#if defined(_WIN32) || defined(__APPLE__)
        int elf = 0;
#else
        int elf = !build->triple;
#endif
        size_t placed = 0;
        if (!elf)
            fprintf(stderr, "--layout: only an ELF module for this host takes an order: the code keeps its own\n");
        else if (codegen.function_count) {
            q.rank = (size_t*)snx_alloc(codegen.function_count * sizeof(*q.rank));
            for (i = 0; i < codegen.function_count; ++i) q.rank[i] = NO_RANK;
            for (i = 0; i < build->layout_count; ++i) {
                const SnxFunction* f = snx_codegen_function(&codegen, build->layout[i]);
                if (f && q.rank[f - codegen.functions] == NO_RANK) q.rank[f - codegen.functions] = placed++;
            }
        }
        if (elf && !options->quiet)
            printf("layout: %lu functions first, in the order of %lu addresses\n", (unsigned long)placed,
                   (unsigned long)build->layout_count);
    }
    if (!options->quiet)
        printf("%s%s: %lu code unit%s and the module descriptor\n", q.x64 ? native : "LLVM",
               !q.x64                     ? ""
               : snx_fallback_available() ? " back end (LLVM where it can't)"
                                          : " back end",
               (unsigned long)q.count, q.count == 1 ? "" : "s");
    /* the x86-64 back end's units are quick and small: their threads all at once, not opened one a second as for
       LLVM's (the memory a unit of LLVM's takes is what the opening waits on) */
    if (q.count && q.x64 && build->jobs_open && !options->quiet) {
        unsigned n = build->jobs < 1 ? 1 : build->jobs;
        if (n > q.count) n = (unsigned)q.count;
        printf("aot_threads=0 (Auto): %u thread%s at once, the x86-64 back end's units\n", n, n == 1 ? "" : "s");
    }
    if (q.count) run(&q, build->jobs, q.x64 ? 0 : build->jobs_open);
    if (q.failed) {
        snx_error("%s%s", fallback_said(), q.error);
        goto done;
    }
    {
        size_t refused = 0;
        for (i = 0; i < q.count; ++i) refused += q.units[i].x64_refused;
        if (refused)
            snx_error("native back end: %lu of %lu functions have an instruction it has no code for: %s",
                      (unsigned long)refused, (unsigned long)codegen.function_count,
                      snx_fallback_available() ? "LLVM made them"
                                               : "each made, the instruction undefined where it runs");
    }
    if (q.x64 && !options->quiet) {
        size_t made = 0;
        for (i = 0; i < q.count; ++i) made += q.units[i].x64_made;
        printf("%s back end: %lu of %lu functions%s\n", native, (unsigned long)made,
               (unsigned long)codegen.function_count, made == codegen.function_count ? "" : ", LLVM the rest");
    }
    /* the module descriptor: the native back end's where it made every function (with the profile cache's units'
       variants and wrappers), the fallback's otherwise */
    start = snx_seconds();
    x64_only = q.x64;
    for (i = 0; x64_only && i < q.count; ++i)
        if (q.units[i].object || q.units[i].x64_made != q.units[i].last - q.units[i].first) x64_only = 0;
    if (x64_only) {
        SnxJitCacheRecord* records = cache_count ? (SnxJitCacheRecord*)calloc(cache_count, sizeof *records) : NULL;
        size_t nrecords = 0;
        if (cache_count && !records) {
            snx_error("%s back end: out of memory", native);
            goto done;
        }
        for (i = 0; i < cache_count; ++i)
            if (snx_jit_cache_record(cache, i)) records[nrecords++] = *snx_jit_cache_record(cache, i);
        int failed = snx_x64_descriptor_for(build->triple ? &snx_a64_desc : snx_native_desc(), &codegen, options,
                                            loaded, q.imports, q.import_count, records, nrecords, &module_object,
                                            &module_size, &entry_count, &variant_count, error, sizeof(error));
        free(records);
        if (failed) {
            snx_error("%s back end: %s", native, error);
            goto done;
        }
        if (build->wrappers) *build->wrappers = nrecords ? entry_count : 0;
        if (options->jit_cache && !options->quiet)
            printf("AOT pack: %lu cached function%s%s\n", (unsigned long)variant_count, variant_count == 1 ? "" : "s",
                   variant_count ? "" : " (the cache did not match the game)");
        goto descriptor_done;
    }
    if (snx_module_fallback_descriptor(&codegen, options, loaded, build, q.imports, q.import_count,
                                       cache_count ? cache : NULL, &module_object, &module_size, &entry_count, &wrapped,
                                       &variant_count, error, sizeof(error))) {
        snx_error("%s%s", fallback_said(), error);
        goto done;
    }
    if (build->wrappers) *build->wrappers = wrapped;
    if (options->jit_cache && !options->quiet)
        printf("AOT pack: %lu cached function%s%s\n", (unsigned long)variant_count, variant_count == 1 ? "" : "s",
               variant_count ? "" : " (the cache did not match the game)");
descriptor_done:
    if (!options->quiet) {
        char* label = snx_format("SnxModule descriptor, %lu entries", (unsigned long)entry_count);
        snx_report_unit(q.count + 1, q.count + 1, "module", label, 0, snx_seconds() - start);
        free(label);
    }
    /* link: runtime library, descriptor, chunks. A module for another machine has no runtime library of its own
       (this host has none for that machine): the app that loads it brings its own */
    if (x64_only && !build->triple) {
        /* the one the recompiler's build compiled (rtlib_object.h), as the JIT links: no LLVM */
        rtlib_object = (unsigned char*)snx_rtlib_object;
        rtlib_size = (size_t)snx_rtlib_object_size;
        rtlib_static = 1;
    } else if (!build->triple && snx_fallback_rtlib(build->opt, &rtlib_object, &rtlib_size, error, sizeof(error))) {
        snx_error("%s%s", fallback_said(), error);
        goto done;
    }
    /* link: runtime library, descriptor, chunks, the profile cache's units. A module for another machine has no
       runtime library of its own (this host has none for that machine): the app that loads it brings its own */
    inputs = (SnxLinkInput*)snx_alloc((2 * q.count + 2 + cache_count) * sizeof(*inputs));
    if (rtlib_object) {
        inputs[n].data = rtlib_object;
        inputs[n].size = rtlib_size;
        inputs[n++].name = "rtlib";
    }
    inputs[n].data = module_object;
    inputs[n].size = module_size;
    inputs[n++].name = "module";
    for (i = 0; i < q.count; ++i) {
        if (q.units[i].object || !q.units[i].x64) {
            inputs[n].data = q.units[i].object;
            inputs[n].size = q.units[i].size;
            inputs[n++].name = "code chunk";
        }
    }
    for (i = 0; i < q.count; ++i)
        if (q.units[i].x64) {
            inputs[n].data = q.units[i].x64;
            inputs[n].size = q.units[i].x64_size;
            inputs[n++].name = "x86-64 code chunk";
        }
    for (i = 0; i < cache_count; ++i) {
        const SnxJitCacheRecord* record = snx_jit_cache_record(cache, i);
        inputs[n].data = record->object;
        inputs[n].size = record->object_size;
        inputs[n++].name = record->name;
    }
    exports[0] = "snx_module";
    exports[export_count++] = "snx_routes";
    if (variant_count) exports[export_count++] = "snx_variants";
    if (options->nro_count) exports[export_count++] = "snx_nros";
    if (q.import_count) exports[export_count++] = "snx_imports";
    tmp = snx_format("%s.tmp", build->output);
    if (build->triple) {
        /* the pack to its file a record at a time, the code generator's tables gone first: a game of hundreds of
           thousands of functions has them, its objects and a copy of them as one pack in memory otherwise */
        PackUnits pack;
        char reason[256];
        pack.units = q.units;
        pack.inputs = inputs;
        pack.count = q.count;
        snx_codegen_free(&codegen);
        if (snx_objects_pack_write(inputs, n, tmp, &linked.size, NULL, written_unit, &pack, error, sizeof(error))) {
            snx_error("packing failed: %s", error);
            if (snx_is_file(tmp)) snx_remove_tree(tmp);
            goto done;
        }
        if (plat_replace_file(tmp, build->output)) {
            snx_error("packing failed: cannot rename %s to %s: %s", tmp, build->output,
                      snx_os_error(reason, sizeof reason));
            snx_remove_tree(tmp);
            goto done;
        }
    } else {
        if (snx_link(inputs, n, exports, export_count, base_name(build->output), &linked, error, sizeof(error))) {
            snx_error("linking failed: %s", error);
            goto done;
        }
        /* a module Android can map: its text is loaded executable, which its file must allow there */
        if (snx_write_file(tmp, linked.data, linked.size) || snx_make_executable(tmp) ||
            plat_replace_file(tmp, build->output)) {
            snx_error("linking failed: cannot write %s or make it executable", build->output);
            if (snx_is_file(tmp)) snx_remove_tree(tmp);
            goto done;
        }
    }
    if (!options->quiet) {
        printf("%s module: %.1f MB, %lu entries\n", x64_only ? native : "LLVM", (double)linked.size / 1000000.0,
               (unsigned long)entry_count);
    }
    rc = 0;
done:
    for (i = 0; i < q.count; ++i) {
        free(q.units[i].object);
        free(q.units[i].x64);
        free(q.units[i].stats);
    }
    free(q.units);
    free(q.rank);
    free(q.imports);
    free(inputs);
    free(module_object);
    if (!rtlib_static) free(rtlib_object);
    free(linked.data);
    free(tmp);
    snx_jit_cache_close(cache);
    snx_codegen_free(&codegen);
    return rc;
}
