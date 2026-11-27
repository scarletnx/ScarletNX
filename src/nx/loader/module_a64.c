/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game module of a 64-bit program: its functions found (module.c's analysis), made in units on threads by the
   host's native back end (recompiler/mach/native.h), and the descriptor of module_abi.h (module_x64.c), linked with the
   runtime library by recompiler/object/link.c. An instruction the back end has no code for is undefined where it runs,
   its function made */
#include "nx/loader/module_a64.h"
#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/cpu/aarch64/a64_decoder.h"
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

/* The object bytes a guest instruction comes to at most, averaged over a unit (the back ends make 36 to 49 of code):
   what bounds the instructions a unit takes before any is made. A unit whose object is still over the ceiling is made
   again as two (split_units) */
#define UNIT_BYTES_AN_INSTRUCTION 96u

typedef struct {
    size_t first, last; /* functions */
    int made;           /* compiled already: a unit split after the others were made is not */
    /* the native back end's object of the unit (x64_codegen.h) */
    unsigned char* x64;
    size_t x64_size, x64_made;
    size_t x64_refused; /* of the others, those with an instruction it has no code for */
    int reused; /* the x86-64 back end's object came from memory (x64_cache.h) */
    SnxSsaCount ssa; /* its SSA, as the back end that made the object counted it (none of a reused one) */
} Unit;

typedef struct {
    SnxCodegen* codegen;
    const SnxModuleBuild* build;
    /* the module's bound branches (SnxCodegenOptions.bound) by address, and the functions of other modules among
       their targets, each once, ascending: its imports (snx_imports) */
    const SnxBoundBranch* bound;
    size_t bound_count;
    SnxU64* imports;
    size_t import_count;
    Unit* units;
    size_t count, next, done;
    /* for the control of threads left open (auto_workers.h): the units under way, and the bytes of code the finished
       ones keep in memory */
    unsigned active;
    SnxU64 held;
    double unit_seconds; /* what the threads spent in the units, summed */
    int failed;
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

/* The pack's write (snx_objects_pack_write) of inputs, some of them the units' objects: each freed once written */
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

/* one chunk of functions, as module.c's code_NNN.c, the native back end's: with alone set, an instruction it has no
   code for is undefined where it runs, and its function is made */
static int compile_unit(Queue* q, size_t index, char* error, size_t error_size) {
    Unit* u = &q->units[index];
    size_t i;
    int rc;
    SnxNativeOptions options = {0};
    options.stale = 1;
    options.refused = &u->x64_refused;
    u->ssa = (SnxSsaCount){0};
    options.ssa = &u->ssa;
    if (q->bound_count) {
        options.bound = bound_at;
        options.bound_user = q;
    }
    if (q->import_count) {
        options.direct = imported;
        options.direct_user = q;
    }
    unsigned char* made = (unsigned char*)snx_alloc(u->last - u->first);
    rc = q->build->triple ? snx_native_target_object(q->build->triple, q->codegen, u->first, u->last, &options, made,
                                                     &u->x64, &u->x64_size, error, error_size)
                          : snx_native_cached_object(q->codegen, u->first, u->last, &options, made, &u->x64,
                                                     &u->x64_size, &u->reused, error, error_size);
    for (i = 0; !rc && i < u->last - u->first; ++i) u->x64_made += made[i];
    for (i = 0; !rc && i < u->last - u->first && made[i]; ++i) {}
    if (!rc && i < u->last - u->first) {
        const SnxFunction* f = &q->codegen->functions[u->first + i];
        char text[64] = "no instruction";
        SnxU32 word = 0;
        if (snx_codegen_word(q->codegen, f->start, &word)) snx_a64_word_text(word, text, sizeof text);
        char more[48] = "";
        const size_t left = u->last - u->first - u->x64_made;
        if (left > 1) snprintf(more, sizeof more, ", nor %zu more of the unit's functions", left - 1);
        snprintf(error, error_size,
                 "code_%03lu: the function at 0x%llx, which starts with %s (%08x): the native back end (%s) did not "
                 "make it%s",
                 (unsigned long)index, (unsigned long long)f->start, text, (unsigned)word, snx_native_name(), more);
        rc = -1;
    }
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
        while (!q->failed && q->next < q->count && q->units[q->next].made) ++q->next;
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
        q->units[i].made = 1;
        q->unit_seconds += seconds;
        if (q->units[i].x64) q->held += q->units[i].x64_size;
        if (rc && !q->failed) {
            q->failed = 1;
            snprintf(q->error, sizeof(q->error), "%s", error);
        }
        if (label) {
            /* every unit: a line every few seconds or minutes, whichever the machine manages */
            sprintf(name, "code_%03lu", (unsigned long)i);
            snx_report_unit(q->done, q->count + 1, name, label, q->units[i].reused, seconds); /* + the descriptor */
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

/* A unit's object is at most half of what the machine's calls reach: all of its code then reaches the island its
   veneers are in, and a call of an earlier function of the unit that function's entry (the back ends'
   MG_DIRECT_ENTRY) */
static SnxU64 unit_ceiling(const SnxModuleBuild* build) {
    return build->unit_bytes ? build->unit_bytes : snx_native_reach(build->triple) / 2;
}

/* The units whose object came out over ceiling, each put back as two of about half its instructions, to be made by
   the next run of the queue; the others keep their objects and their place. 1 when any was, 0 for none, or -1 with
   the reason in q->error: a function that alone makes more than a unit holds */
static int split_units(Queue* q, SnxU64 ceiling) {
    size_t over = 0, n = 0;
    for (size_t i = 0; i < q->count; ++i) over += q->units[i].x64_size > ceiling;
    if (!over) return 0;
    Unit* units = (Unit*)snx_alloc((q->count + over) * sizeof(*units));
    for (size_t i = 0; i < q->count; ++i) {
        Unit* u = &q->units[i];
        if (u->x64_size <= ceiling) {
            units[n++] = *u;
            continue;
        }
        if (u->last - u->first < 2) {
            snprintf(q->error, sizeof(q->error),
                     "the function at 0x%llx alone makes an object of %.1f MiB, more than the %.1f MiB a unit holds",
                     (unsigned long long)q->codegen->functions[u->first].start, (double)u->x64_size / 1048576.0,
                     (double)ceiling / 1048576.0);
            free(units);
            return -1;
        }
        size_t count = 0, half = 0, mid = u->first;
        for (size_t k = u->first; k < u->last; ++k) count += q->codegen->functions[k].count;
        for (; mid + 1 < u->last && 2 * half < count; ++mid) half += q->codegen->functions[mid].count;
        if (mid == u->first) mid = u->first + 1;
        snx_error("a code unit of %zu instructions came to %.1f MiB, more than the %.1f MiB a unit holds on this "
                  "machine: made again as two, of %zu and %zu functions",
                  count, (double)u->x64_size / 1048576.0, (double)ceiling / 1048576.0, mid - u->first, u->last - mid);
        q->held -= u->x64_size;
        --q->done;
        free(u->x64);
        units[n] = (Unit){0};
        units[n].first = u->first;
        units[n++].last = mid;
        units[n] = (Unit){0};
        units[n].first = mid;
        units[n++].last = u->last;
    }
    free(q->units);
    q->units = units;
    q->count = n;
    q->next = 0;
    return 1;
}

unsigned snx_module_abi(void) { return SNX_ABI; }
static const char* base_name(const char* path) {
    const char *name = path, *p;
    for (p = path; *p; ++p)
        if (*p == '/' || *p == '\\') name = p + 1;
    return name;
}

int snx_build_module_a64(SnxProgram* loaded, const SnxCodegenOptions* options, const SnxModuleBuild* build) {
    SnxCodegen codegen;
    Queue q;
    SnxJitCache* cache = NULL;
    SnxLinkInput* inputs = NULL;
    SnxLinkOutput linked;
    unsigned char* module_object = NULL;
    size_t module_size = 0, first, last, count, limit, i, entry_count = 0, n = 0;
    size_t cache_count = 0, variant_count = 0, export_count = 1, units_at = 0, units_end = 0;
    const char* exports[5];
    char error[1024], unit_name[48] = "code unit", *tmp = NULL; /* unit_name: a unit's object in the linker's errors */
    double start;
    SnxRecompilePhases phases = {0};
    int rc = -1;
    memset(&codegen, 0, sizeof(codegen));
    memset(&q, 0, sizeof(q));
    memset(&linked, 0, sizeof(linked));
    if (!loaded->module_count) return snx_error("no modules to generate");
    /* a function order (tools/layout.py's), which the native back end does not take yet */
    if (build->layout_count)
        return snx_error("--layout of %zu addresses: the native back end does not order its functions yet",
                         build->layout_count);
    if (options->jit_cache && build->triple) {
        snx_error("--jit-cache cannot go with --target %s: a JIT cache is this host's", build->triple);
        goto done;
    }
    /* the program's CPU (loader/ounce.h): the runtime JIT takes the same features from the same NPDM */
    SnxU64 features = snx_guest_features(loaded->npdm.ounce, g_settings.cpu_extensions);
    if (options->jit_cache) {
        char* key = snx_jit_cache_key(build->opt, options->hook_direct_calls, 1, features);
        if (!key) {
            snx_error("--jit-cache %s: out of memory for its key", options->jit_cache);
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
    start = snx_seconds();
    if (!options->interp_only &&
        snx_nx_codegen_analyze(&codegen, loaded, options->hints, options->hint_count, options->quiet, features))
        goto done;
    phases.discovery = snx_seconds() - start;
    const SnxU64 guest = codegen.instruction_count;
    if (build->found) *build->found = codegen.instruction_count;
    codegen.features = features;
    codegen.direct_svc = 1; /* the runtime's RtAPI.svcs (svc.c's g_svc64) */
    codegen.hook_direct_calls = options->hook_direct_calls;
    /* chunks of whole functions, as module.c splits the C */
    limit = options->chunk_instrs ? options->chunk_instrs : (size_t)g_settings.unit_size;
    {
        /* no more instructions to a unit than its ceiling takes */
        const SnxU64 ceiling = unit_ceiling(build);
        const SnxU64 most = ceiling / UNIT_BYTES_AN_INSTRUCTION ? ceiling / UNIT_BYTES_AN_INSTRUCTION : 1;
        if (limit > most) {
            snx_error("unit_instructions=%zu would make units of more than the %.1f MiB one holds on this machine: "
                      "%llu in effect",
                      limit, (double)ceiling / 1048576.0, (unsigned long long)most);
            limit = (size_t)most;
        }
    }
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
       objects (snx_native_target: iOS's). With every call through the table (hook_direct_calls) its calls go through
       CPU.disp too */
    const char* native = build->triple ? "arm64" : snx_native_name();
    snprintf(unit_name, sizeof unit_name, "%s code unit", native);
    if (build->triple ? !snx_native_target(build->triple) : !snx_native_available()) {
        char floor[256];
        if (build->triple)
            snx_error("the arm64 back end makes no module for %s on this host", build->triple);
        else if (snx_native_floor(floor, sizeof floor))
            snx_error("%s", floor);
        else
            snx_error("this host has no native back end to make the module");
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
    if (!options->quiet)
        printf("%s back end: %lu code unit%s and the module descriptor\n", native, (unsigned long)q.count,
               q.count == 1 ? "" : "s");
    /* the native back end's units are quick and small: their threads all at once, not opened one a second */
    if (q.count && build->jobs_open && !options->quiet) {
        unsigned n = build->jobs < 1 ? 1 : build->jobs;
        if (n > q.count) n = (unsigned)q.count;
        printf("aot_threads=0 (Auto): %u thread%s at once, the x86-64 back end's units\n", n, n == 1 ? "" : "s");
    }
    start = snx_seconds();
    for (;;) {
        if (q.count) run(&q, build->jobs, 0);
        if (q.failed) break;
        const int split = split_units(&q, unit_ceiling(build));
        if (split < 0) q.failed = 1;
        if (split <= 0) break;
    }
    phases.compile = snx_seconds() - start;
    phases.units = q.unit_seconds;
    phases.threads = build->jobs < 1 ? 1 : build->jobs;
    if (phases.threads > q.count) phases.threads = q.count ? (unsigned)q.count : 1;
    if (q.failed) {
        snx_error("%s back end: %s", native, q.error);
        goto done;
    }
    {
        size_t refused = 0;
        for (i = 0; i < q.count; ++i) refused += q.units[i].x64_refused;
        if (refused)
            snx_error("native back end: %lu of %lu functions have an instruction it has no code for: each made, the "
                      "instruction undefined where it runs",
                      (unsigned long)refused, (unsigned long)codegen.function_count);
    }
    if (!options->quiet)
        printf("%s back end: %lu of %lu functions\n", native, (unsigned long)codegen.function_count,
               (unsigned long)codegen.function_count);
    /* the module descriptor, with the profile cache's units' variants and wrappers */
    start = snx_seconds();
    {
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
    }
    if (!options->quiet) {
        char* label = snx_format("SnxModule descriptor, %lu entries", (unsigned long)entry_count);
        snx_report_unit(q.count + 1, q.count + 1, "module", label, 0, snx_seconds() - start);
        free(label);
    }
    phases.descriptor = snx_seconds() - start;
    /* link: runtime library (the one the recompiler's build compiled, rtlib_object.h, as the JIT links), descriptor,
       chunks, the profile cache's units. A module for another machine has no runtime library of its own (this host has
       none for that machine): the app that loads it brings its own */
    inputs = (SnxLinkInput*)snx_alloc((q.count + 2 + cache_count) * sizeof(*inputs));
    if (!build->triple) {
        inputs[n].data = (unsigned char*)snx_rtlib_object;
        inputs[n].size = (size_t)snx_rtlib_object_size;
        inputs[n++].name = "rtlib";
    }
    inputs[n].data = module_object;
    inputs[n].size = module_size;
    inputs[n++].name = "module";
    units_at = n;
    for (i = 0; i < q.count; ++i)
        if (q.units[i].x64) {
            inputs[n].data = q.units[i].x64;
            inputs[n].size = q.units[i].x64_size;
            inputs[n++].name = unit_name;
        }
    units_end = n;
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
        phases.pack = 1;
        start = snx_seconds();
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
        phases.link = snx_seconds() - start;
    } else {
        /* the module to its file as it is linked, each unit's object given back once its code is there: a game's
           code is in memory once, not as the units' objects and as the module too. The code generator's tables go
           first, as for a pack */
        PackUnits pack = {q.units, inputs, q.count};
        unsigned char* owned = (unsigned char*)snx_alloc(n ? n : 1);
        memset(owned, 0, n ? n : 1);
        for (i = units_at; i < units_end; ++i) owned[i] = 1;
        snx_codegen_free(&codegen);
        phases.streamed = 1;
        start = snx_seconds();
        const int failed = snx_link_to_file(inputs, n, exports, export_count, base_name(build->output), tmp, owned,
                                            written_unit, &pack, &linked.size, error, sizeof(error));
        free(owned);
        if (failed) {
            snx_error("linking failed: %s", error);
            if (snx_is_file(tmp)) snx_remove_tree(tmp);
            goto done;
        }
        phases.link = snx_seconds() - start;
        start = snx_seconds();
        /* a module Android can map: its text is loaded executable, which its file must allow there */
        if (snx_make_executable(tmp) || plat_replace_file(tmp, build->output)) {
            snx_error("linking failed: cannot make %s executable or put it at %s", tmp, build->output);
            if (snx_is_file(tmp)) snx_remove_tree(tmp);
            goto done;
        }
        phases.write = snx_seconds() - start;
    }
    SnxSsaCount ssa = {0};
    size_t uncounted = 0; /* the units whose object came from memory: no back end ran to count their SSA */
    for (i = 0; i < q.count; ++i) {
        uncounted += q.units[i].reused != 0;
        ssa.instructions += q.units[i].ssa.instructions;
        ssa.bytes += q.units[i].ssa.bytes;
        ssa.packed += q.units[i].ssa.packed;
    }
    if (build->ssa) *build->ssa = ssa;
    if (!options->quiet) {
        char line[512];
        printf("%s module: %.1f MB, %lu entries\n", native, (double)linked.size / 1000000.0, (unsigned long)entry_count);
        phases.peak = snx_footprint_peak();
        snx_phases_line(line, sizeof line, &phases);
        puts(line);
        if (ssa.instructions) {
            snx_ssa_line(line, sizeof line, &ssa, guest);
            puts(line);
            if (uncounted)
                printf("ssa: %zu of the %zu units came from memory, their SSA not in it\n", uncounted, q.count);
        }
    }
    rc = 0;
done:
    for (i = 0; i < q.count; ++i) free(q.units[i].x64);
    free(q.units);
    free(q.imports);
    free(inputs);
    free(module_object);
    free(linked.data);
    free(tmp);
    snx_jit_cache_close(cache);
    snx_codegen_free(&codegen);
    return rc;
}
