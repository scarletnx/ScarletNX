/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The recompiler as a library (snx_recompiler.h, issue #171). The pieces are the ones the scarletnx command uses
   for a game: function discovery (codegen.c), the LLVM backend (llvm/ir*.c) or libgccjit (gccjit/jit*.c), and the
   runtime library every module carries (llvm/rtlib.c, prebuilt). What differs from the game workflow
   (nx/loader/module_llvm.c, module_gccjit.c): the input is a buffer at a guest address instead of NSO files, the module
   descriptor is the small one below instead of the Switch loader's SnxModule, and the result is loaded into this
   process straight from memory (or a temporary file the OS requires) instead of being written beside the game. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* memfd_create */
#endif
/* Android's C library has memfd_create from Android 11 (API 30): before it, the temporary file */
#if defined(__linux__) && (!defined(__ANDROID__) || __ANDROID_API__ >= 30)
#define SNX_MEMFD 1
#endif
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/arm/exec.h"
#include "common/cpu/arm/simd.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/jit_service.h"
#include "common/cpu/recompiler/llvm/link.h"
#include <llvm-c/Support.h>
#include <llvm-c/TargetMachine.h>
#ifdef SNX_WITH_GCCJIT
#include "common/cpu/recompiler/gccjit/jit.h"
#endif
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/mman.h>
#endif
#endif

/* the runtime library as an object for the libgccjit backend, compiled when the recompiler is built (rtlib.c,
   snx-rtlib-gen embed); the LLVM backend compiles its bitcode instead (snx_ir_rtlib_object) */
extern const unsigned char snx_rtlib_object[];
extern const unsigned long snx_rtlib_object_size;
/* the runtime library's bitcode the LLVM backend compiles (llvm/ir.c), which names its cached object */
extern const unsigned char snx_rtlib_bitcode[];
extern const unsigned long snx_rtlib_bitcode_size;

/* instructions per code unit (an LLVM module, a libgccjit context): whole functions until a unit reaches it, as the
   game workflow splits them (SNX_CHUNK_INSTRUCTIONS) */
#ifdef __ANDROID__
#define CHUNK_INSTRUCTIONS 20000
#else
#define CHUNK_INSTRUCTIONS 90000
#endif
/* the dispatch table holds a pointer per instruction slot of the code range: a range this wide is not a program */
#define MAX_CODE_SPAN ((uint64_t)1 << 30)

/* ---- the descriptor the generated module exports, read back by load_descriptor ----
   Written by the backends field by field at these offsets, into a byte array of this size, so neither backend has to
   describe the struct's layout. abi comes first: a module of another contract is recognized before anything else */
typedef struct {
    uint32_t abi;
    uint32_t reserved;
    uint64_t count;            /* entries: every leader of every function, by address */
    const uint64_t* addresses; /* ascending */
    const GuestFn* functions;  /* the function each leader belongs to (its F_, which dispatches on the entry) */
    GuestFn interp;            /* NULL: there is no interpreter, the module's JIT compiles what it lacks */
} Descriptor;
#define DESCRIPTOR_SYMBOL "snx_lib_module"
typedef const Descriptor* (*DescriptorFn)(void);

struct snx_recompiler {
    int backend; /* SNX_RECOMPILER_FLAG_* */
    char opt;
    unsigned jobs;
    int verbose;
    char* keep_ir;
    char* cache; /* folder of linked modules by the hash of what they were made from (LLVM), NULL for none */
    char error[1024];
};

struct snx_recompiled {
    void* image;       /* dlopen/LoadLibrary handle */
    char* image_file;  /* Windows: the DLL's file, deleted after FreeLibrary */
    int image_fd;      /* Linux: the memfd the module was loaded from, kept open while it is (see load_image) */
    void* jit_result;  /* libgccjit: gcc_jit_result*, released with the code */
    GuestFn* dispatch; /* (pc - code_lo) >> 2 -> function, NULL for one the JIT compiles when reached */
    /* snx_recompiled_set_fallback's: what a miss runs instead of the module's JIT, NULL for the JIT */
    GuestFn fallback;
    uint64_t code_lo, code_hi;
    size_t functions;
    /* the code ranges, views and bytes in one block (keep_code), which the JIT reads for as long as the module lives:
       the caller's may go once snx_recompile returns */
    SnxCodeImage* views;
    size_t view_count;
    /* the descriptor's entries, which the JIT's code calls directly */
    const uint64_t* entry_addresses;
    const GuestFn* entry_functions;
    size_t entry_count;
    char opt;
    /* made by the first miss (module_jit), JIT_NONE when it can't start */
    SnxJitService* jit;
};
static char jit_none;
#define JIT_NONE ((SnxJitService*)(void*)&jit_none)

static void fail(struct snx_recompiler* r, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r->error, sizeof(r->error), fmt, ap);
    va_end(ap);
}

/* ---- the recompiler ---- */

/* snx_ir_init once per process, whichever thread's recompiler comes first: it writes LLVM's and ir.c's process-wide
   state, which the game driver only ever calls from one thread */
static int llvm_ready;
static char llvm_error[256];
static void llvm_init_once(void) { llvm_ready = snx_ir_init(llvm_error, sizeof(llvm_error)) ? -1 : 1; }
#ifdef _WIN32
static BOOL CALLBACK llvm_init_callback(PINIT_ONCE once, PVOID parameter, PVOID* context) {
    (void)once;
    (void)parameter;
    (void)context;
    llvm_init_once();
    return TRUE;
}
#endif
static int llvm_init(char* error, size_t error_size) {
#ifdef _WIN32
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, llvm_init_callback, NULL, NULL);
#else
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, llvm_init_once);
#endif
    if (llvm_ready < 0 && error_size) snprintf(error, error_size, "%s", llvm_error);
    return llvm_ready < 0 ? -1 : 0;
}

int snx_recompiler_has(int flags) {
    char error[256];
    if (flags == SNX_RECOMPILER_FLAG_LLVM) return !llvm_init(error, sizeof(error));
#ifdef SNX_WITH_GCCJIT
    if (flags == SNX_RECOMPILER_FLAG_GCCJIT) return 1;
#endif
    return 0;
}

unsigned snx_recompiler_abi(void) { return SNX_ABI; }

struct snx_recompiler* snx_recompiler_new(int flags) {
    struct snx_recompiler* r;
    if (!snx_recompiler_has(flags)) return NULL;
    r = (struct snx_recompiler*)calloc(1, sizeof(*r));
    if (!r) return NULL;
    r->backend = flags;
    r->opt = '2';
    return r;
}

void snx_recompiler_free(struct snx_recompiler* r) {
    if (r) free(r->cache);
    if (!r) return;
    free(r->keep_ir);
    free(r);
}

const char* snx_recompiler_error(const struct snx_recompiler* r) { return r->error; }

int snx_recompiler_set_optimization(struct snx_recompiler* r, char opt) {
    if (!strchr("0123sz", opt) || !opt) {
        fail(r, "optimization level must be 0, 1, 2, 3, s or z");
        return -1;
    }
    r->opt = opt;
    return 0;
}

void snx_recompiler_set_jobs(struct snx_recompiler* r, unsigned jobs) { r->jobs = jobs; }
void snx_recompiler_set_verbose(struct snx_recompiler* r, int verbose) { r->verbose = verbose; }

int snx_recompiler_set_keep_ir(struct snx_recompiler* r, const char* folder) {
    char* copy = NULL;
    if (folder) {
        copy = snx_strdup(folder);
        if (!copy) {
            fail(r, "out of memory");
            return -1;
        }
    }
    free(r->keep_ir);
    r->keep_ir = copy;
    return 0;
}

int snx_recompiler_set_cache(struct snx_recompiler* r, const char* folder) {
    free(r->cache);
    r->cache = NULL;
    if (!folder) return 0;
    if (snx_mkdirs(folder)) {
        fail(r, "cannot create the module cache %s", folder);
        return -1;
    }
    r->cache = snx_strdup(folder);
    return 0;
}

/* ---- RtAPI defaults ---- */

static uint64_t default_ticks(void) {
#ifdef _WIN32
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    return (uint64_t)((double)now.QuadPart * 19200000.0 / (double)frequency.QuadPart);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return 0;
    return (uint64_t)ts.tv_sec * 19200000u + (uint64_t)ts.tv_nsec * 192 / 10000;
#endif
}
static void default_note_hint(uint64_t pc) { (void)pc; }
static void default_fpcr_changed(CPU* c) { (void)c; }
static void default_sev(void) {}
static void default_wfe(CPU* c) { (void)c; }
static void default_poll(CPU* c) { (void)c; }

/* The module's JIT, made by the first miss of any thread (the first to publish it keeps its own): NULL when it can't
   start (a host without the JIT loader) */
static SnxJitService* module_jit(struct snx_recompiled* m) {
    SnxJitService *s = __atomic_load_n(&m->jit, __ATOMIC_ACQUIRE), *expected = NULL;
    SnxJitConfig config;
    char error[512];
    if (s) return s == JIT_NONE ? NULL : s;
    if (!m->view_count) return NULL; /* snx_recompile_from's: the caller compiles what the module lacks */
    memset(&config, 0, sizeof(config));
    config.images = m->views;
    config.image_count = m->view_count;
    config.dispatch = m->dispatch;
    config.code_lo = m->code_lo;
    config.code_hi = m->code_hi;
    config.aot_addresses = m->entry_addresses;
    config.aot_functions = m->entry_functions;
    config.aot_count = m->entry_count;
    config.opt = m->opt;
    s = snx_jit_service_new(&config, error, sizeof(error));
    if (!__atomic_compare_exchange_n(&m->jit, &expected, s ? s : JIT_NONE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        snx_jit_service_free(s);
        s = expected;
    }
    return s == JIT_NONE ? NULL : s;
}

/* the code for pc of the module: its table's, or the JIT's, waiting for the compile; NULL when it can't be compiled */
static GuestFn module_code(struct snx_recompiled* m, uint64_t pc) {
    uint64_t o = pc - m->code_lo;
    SnxJitService* s;
    GuestFn f = NULL;
    if (o < m->code_hi - m->code_lo && !(pc & 3)) f = __atomic_load_n(&m->dispatch[o >> 2], __ATOMIC_ACQUIRE);
    if (!f) f = __atomic_load_n(&m->fallback, __ATOMIC_ACQUIRE);
    if (!f && (s = module_jit(m)) != NULL) f = snx_jit_wait(s, pc);
    return f;
}

/* RtAPI.miss, generated code's fallback for a pc its table leaves empty (SNX_ABI 8): the JIT's code for it, in the
   module the CPU is bound to, which load_descriptor keeps in front of the dispatch table. A pc that can't be compiled
   (outside the code ranges, a host without the JIT) is an undefined instruction there */
static void default_miss(CPU* c, uint64_t pc) {
    struct snx_recompiled* m = (struct snx_recompiled*)((void* const*)c->disp)[-1];
    GuestFn f = module_code(m, pc);
    if (f) {
        f(c, pc);
        return;
    }
    c->pc = pc;
    c->rt->undef(c, pc, 0);
}

void snx_runtime_defaults(RtAPI* rt) {
    memset(rt, 0, sizeof(*rt));
    rt->miss = default_miss;
    rt->note_hint = default_note_hint;
    rt->ticks = default_ticks;
    rt->fpcr_changed = default_fpcr_changed;
    rt->sev = default_sev;
    rt->wfe = default_wfe;
    rt->poll = default_poll;
}

/* ---- the input ---- */

/* SnxCodeImages over the caller's ranges, the whole of each being code, and the code span they cover. A64 ranges
   (need_code) come with their bytes and are word aligned; A32 ones are decoded from the CPU's memory at run time and
   may end on a Thumb halfword */
static SnxCodeImage* images(struct snx_recompiler* r, const struct snx_code_range* ranges, size_t count, int need_code,
                            uint64_t* lo, uint64_t* hi) {
    SnxCodeImage* views;
    size_t i;
    uint64_t align = need_code ? 3 : 1;
    if (!count) {
        fail(r, "no code ranges");
        return NULL;
    }
    views = (SnxCodeImage*)calloc(count, sizeof(*views));
    if (!views) {
        fail(r, "out of memory");
        return NULL;
    }
    *lo = ~(uint64_t)0;
    *hi = 0;
    for (i = 0; i < count; ++i) {
        if (!ranges[i].size || (ranges[i].address & align) || (ranges[i].size & align) ||
            (need_code && !ranges[i].code) || ranges[i].address + ranges[i].size < ranges[i].address) {
            fail(r, "code range %lu: a non-empty, %s aligned range%s is required", (unsigned long)i,
                 need_code ? "4-byte" : "2-byte", need_code ? " with its bytes" : "");
            free(views);
            return NULL;
        }
        views[i].image = (const unsigned char*)ranges[i].code;
        views[i].image_size = ranges[i].size;
        views[i].base = ranges[i].address;
        views[i].text_size = ranges[i].size;
        if (ranges[i].address < *lo) *lo = ranges[i].address;
        if (ranges[i].address + ranges[i].size > *hi) *hi = ranges[i].address + ranges[i].size;
    }
    if (*hi - *lo > MAX_CODE_SPAN) {
        fail(r, "the code ranges span more than 1 GiB of guest addresses");
        free(views);
        return NULL;
    }
    return views;
}

/* chunks of whole functions: units[i] = first function of unit i, units[count] = function_count */
static size_t* chunks(const SnxCodegen* codegen, unsigned jobs, size_t* count) {
    size_t *units, first, last, n, instructions, total = 0, unit;
    units = (size_t*)calloc(codegen->function_count + 2, sizeof(*units));
    if (!units) return NULL;
    /* a small program (a just-in-time step) is still split between the jobs, down to units of a few thousand
       instructions, below which a unit's fixed cost is more than its share of the work */
    for (first = 0; first < codegen->function_count; ++first) total += codegen->functions[first].count;
    if (!jobs) jobs = snx_cpu_count();
    unit = jobs > 1 ? total / jobs + 1 : CHUNK_INSTRUCTIONS;
    if (unit < 4000) unit = 4000;
    if (unit > CHUNK_INSTRUCTIONS) unit = CHUNK_INSTRUCTIONS;
    n = 0;
    for (first = 0; first < codegen->function_count; first = last) {
        last = first;
        instructions = 0;
        do {
            instructions += codegen->functions[last++].count;
        } while (last < codegen->function_count && instructions < unit);
        units[n++] = first;
    }
    units[n] = codegen->function_count;
    *count = n;
    return units;
}

/* the entries of the descriptor: every leader of every function by address, the first function's when two share one
   (as the Switch loader builds its table, nx/loader/module.c) */
typedef struct {
    uint64_t address;
    size_t function;
} Entry;
static int compare_entry(const void* a, const void* b) {
    const Entry *x = (const Entry*)a, *y = (const Entry*)b;
    if (x->address != y->address) return x->address < y->address ? -1 : 1;
    return x->function < y->function ? -1 : x->function > y->function;
}
static Entry* entries(const SnxCodegen* codegen, size_t* count) {
    Entry* e;
    size_t n, i, j, unique;
    n = 0;
    for (i = 0; i < codegen->function_count; ++i) n += codegen->functions[i].leader_count;
    e = (Entry*)calloc(n ? n : 1, sizeof(*e));
    if (!e) return NULL;
    n = 0;
    for (i = 0; i < codegen->function_count; ++i)
        for (j = 0; j < codegen->functions[i].count; ++j)
            if (codegen->functions[i].leaders[j]) {
                e[n].address = snx_function_address(&codegen->functions[i], j);
                e[n++].function = i;
            }
    if (n) qsort(e, n, sizeof(*e), compare_entry);
    unique = 0;
    for (i = 0; i < n; ++i) {
        if (unique && e[i].address == e[unique - 1].address) continue;
        e[unique++] = e[i];
    }
    *count = unique;
    return e;
}

static void report(struct snx_recompiler* r, const char* fmt, ...) {
    va_list ap;
    if (!r->verbose) return;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

/* ---- loading the linked module into this process ---- */

#ifndef _WIN32
static void* open_image(const char* path) { return dlopen(path, RTLD_NOW | RTLD_LOCAL); }
#endif

/* The shared object, as link.c wrote it, loaded from memory where the OS can (Linux: an anonymous memfd), and through
   a temporary file elsewhere. On Unix the file is removed right after loading: the mapping outlives the name. On
   Windows the DLL's file stays in place until FreeLibrary, so the module remembers it.
   The memfd stays open as long as the module is loaded: dlopen looks a path up among the objects already loaded
   before opening it, and a closed descriptor's number is the next one memfd_create hands out, so two modules
   loaded through the same "/proc/self/fd/N" would be one */
static int load_image(struct snx_recompiler* r, struct snx_recompiled* m, const unsigned char* data, size_t size) {
    char *dir = NULL, *path = NULL;
#ifdef SNX_MEMFD
    {
        int fd = memfd_create("scarletnx-recompiled", MFD_CLOEXEC);
        if (fd >= 0) {
            size_t done = 0;
            char proc[64];
            while (done < size) {
                ssize_t n = write(fd, data + done, size - done);
                if (n <= 0) break;
                done += (size_t)n;
            }
            if (done == size) {
                snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
                m->image = open_image(proc);
            }
            if (m->image) {
                m->image_fd = fd;
                return 0;
            }
            close(fd);
        }
    }
#endif
    dir = snx_temp_dir(NULL);
    if (!dir) {
        fail(r, "cannot create a temporary folder for the recompiled module");
        return -1;
    }
#ifdef _WIN32
    path = snx_path_join(dir, "recompiled.dll");
#else
    path = snx_path_join(dir, "recompiled.so");
#endif
    if (snx_write_file(path, data, size)) {
        fail(r, "cannot write the recompiled module to %s", path);
        snx_remove_tree(dir);
        free(dir);
        free(path);
        return -1;
    }
#ifdef _WIN32
    {
        int wide = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
        wchar_t* wpath = wide > 0 ? (wchar_t*)malloc((size_t)wide * sizeof(*wpath)) : NULL;
        if (wpath && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wpath, wide))
            m->image = (void*)LoadLibraryW(wpath);
        free(wpath);
    }
    if (m->image) {
        m->image_file = dir; /* the folder, removed with the DLL when the module is freed */
        free(path);
        return 0;
    }
    fail(r, "cannot load the recompiled module (Windows error %lu)", (unsigned long)GetLastError());
#else
    m->image = open_image(path);
    if (!m->image) fail(r, "cannot load the recompiled module: %s", dlerror());
#endif
    snx_remove_tree(dir);
    free(dir);
    free(path);
    return m->image ? 0 : -1;
}

static void* image_symbol(struct snx_recompiled* m, const char* name) {
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)m->image, name);
#else
    return dlsym(m->image, name);
#endif
}

static void unload_image(struct snx_recompiled* m) {
    if (!m->image) return;
#ifdef _WIN32
    FreeLibrary((HMODULE)m->image);
    if (m->image_file) {
        snx_remove_tree(m->image_file);
        free(m->image_file);
    }
#else
    dlclose(m->image);
#if defined(__linux__)
    if (m->image_fd >= 0) close(m->image_fd);
#endif
#endif
    m->image = NULL;
}

/* the dispatch table from the descriptor; the interpreter entry comes from a lookup of a pc the table leaves empty */
static int load_descriptor(struct snx_recompiler* r, struct snx_recompiled* m, const Descriptor* d) {
    size_t i, slots;
    void** block;
    if (!d || d->abi != SNX_ABI) {
        fail(r, "the recompiled module's descriptor is missing or of another ABI");
        return -1;
    }
    slots = (size_t)((m->code_hi - m->code_lo + 3) / 4);
    /* one slot more, in front of the table: the module, for default_miss to find from cpu->disp. Generated code only
       indexes the table from its start */
    block = (void**)calloc(slots + 1, sizeof(void*));
    if (!block) {
        fail(r, "out of memory for the dispatch table (%lu entries)", (unsigned long)slots);
        return -1;
    }
    block[0] = m;
    m->dispatch = (GuestFn*)(block + 1);
    for (i = 0; i < d->count; ++i) {
        uint64_t a = d->addresses[i];
        if (a < m->code_lo || a >= m->code_hi || (a & 3)) continue;
        m->dispatch[(a - m->code_lo) >> 2] = d->functions[i];
    }
    m->entry_addresses = d->addresses;
    m->entry_functions = d->functions;
    m->entry_count = (size_t)d->count;
    return 0;
}

/* ---- the LLVM backend ---- */

typedef struct {
    size_t first, last; /* functions */
    unsigned char* object;
    size_t size;
} Unit;

typedef struct {
    struct snx_recompiler* r;
    SnxCodegen* codegen;
    Unit* units;
    size_t count, next, done;
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

/* the unit's IR as <dir>/<name>.ll */
static int save_ir(SnxIr* ir, const char* dir, const char* name) {
    char *text = snx_ir_text(ir), *file = snx_format("%s.ll", name), *path = snx_path_join(dir, file);
    int rc = snx_write_file(path, text, strlen(text));
    LLVMDisposeMessage(text);
    free(file);
    free(path);
    return rc;
}

static int llvm_unit(Queue* q, size_t index, char* error, size_t error_size) {
    Unit* u = &q->units[index];
    char name[32];
    size_t i;
    SnxIr* ir;
    int rc = 0;
    snprintf(name, sizeof(name), "code_%03lu", (unsigned long)index);
    ir = snx_ir_new(name, q->r->opt, error, error_size);
    if (!ir) return -1;
    for (i = u->first; i < u->last; ++i) snx_ir_emit(ir, q->codegen, &q->codegen->functions[i]);
    if (q->r->keep_ir && save_ir(ir, q->r->keep_ir, name)) {
        snprintf(error, error_size, "cannot save the IR of %s in %s", name, q->r->keep_ir);
        rc = -1;
    }
    if (!rc) rc = snx_ir_object(ir, q->r->opt, &u->object, &u->size, error, error_size);
    snx_ir_free(ir);
    return rc;
}

static void worker(Queue* q) {
    char error[1024];
    for (;;) {
        size_t i;
        int rc;
        double start;
        lock(q);
        if (q->failed || q->next == q->count) {
            unlock(q);
            return;
        }
        i = q->next++;
        unlock(q);
        error[0] = 0;
        start = snx_seconds();
        rc = llvm_unit(q, i, error, sizeof(error));
        lock(q);
        ++q->done;
        if (rc && !q->failed) {
            q->failed = 1;
            snprintf(q->error, sizeof(q->error), "%s", error);
        }
        if (!rc)
            report(q->r, "  compiled unit %lu/%lu: functions %lu-%lu (%.1f s)\n", (unsigned long)q->done,
                   (unsigned long)q->count, (unsigned long)q->units[i].first, (unsigned long)q->units[i].last,
                   snx_seconds() - start);
        unlock(q);
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

static void run_queue(Queue* q, unsigned jobs) {
    unsigned i, created = 0;
#ifdef _WIN32
    HANDLE* threads;
    InitializeCriticalSection(&q->lock);
#else
    pthread_t* threads;
    pthread_mutex_init(&q->lock, NULL);
#endif
    if (!jobs) jobs = snx_cpu_count();
    if (jobs > q->count) jobs = (unsigned)q->count;
    if (jobs < 1) jobs = 1;
    threads = (void*)calloc(jobs, sizeof(*threads));
    for (i = 1; threads && i < jobs; ++i) {
#ifdef _WIN32
        HANDLE h = CreateThread(NULL, 0, thread_main, q, 0, NULL);
        if (!h) break;
        threads[created++] = h;
#else
        if (pthread_create(&threads[created], NULL, thread_main, q)) break;
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

/* the descriptor as IR: constant tables, and DESCRIPTOR_SYMBOL filling the Descriptor (generated code's misses go to
   RtAPI.miss since SNX_ABI 8, and default_miss's to the module's JIT) */
static int llvm_descriptor(SnxIr* ir, const SnxCodegen* codegen, char* error, size_t error_size) {
    Entry* e;
    size_t count, n, i;
    LLVMValueRef *values, addresses, functions, desc, fn;
    LLVMTypeRef t_addresses, t_functions;
    LLVMBasicBlockRef block;
    e = entries(codegen, &count);
    if (!e) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    n = count ? count : 1;
    values = (LLVMValueRef*)calloc(n, sizeof(*values));
    if (!values) {
        free(e);
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (i = 0; i < n; ++i) values[i] = ir_i64(ir, i < count ? e[i].address : 0);
    t_addresses = LLVMArrayType(ir->t_i64, (unsigned)n);
    addresses = LLVMAddGlobal(ir->mod, t_addresses, "snx_lib_addresses");
    LLVMSetLinkage(addresses, LLVMInternalLinkage);
    LLVMSetInitializer(addresses, LLVMConstArray(ir->t_i64, values, (unsigned)n));
    LLVMSetGlobalConstant(addresses, 1);
    for (i = 0; i < n; ++i)
        values[i] =
            i < count ? snx_ir_guest_function(ir, codegen->functions[e[i].function].start) : LLVMConstNull(ir->t_ptr);
    t_functions = LLVMArrayType(ir->t_ptr, (unsigned)n);
    functions = LLVMAddGlobal(ir->mod, t_functions, "snx_lib_functions");
    LLVMSetLinkage(functions, LLVMInternalLinkage);
    LLVMSetInitializer(functions, LLVMConstArray(ir->t_ptr, values, (unsigned)n));
    LLVMSetGlobalConstant(functions, 1);
    free(values);
    free(e);
    /* static Descriptor DESC, written at run time at the C offsets */
    desc = LLVMAddGlobal(ir->mod, LLVMArrayType(ir->t_i8, sizeof(Descriptor)), "snx_lib_descriptor");
    LLVMSetLinkage(desc, LLVMInternalLinkage);
    LLVMSetInitializer(desc, LLVMConstNull(LLVMArrayType(ir->t_i8, sizeof(Descriptor))));
    LLVMSetAlignment(desc, 8);
    fn = LLVMAddFunction(ir->mod, DESCRIPTOR_SYMBOL, LLVMFunctionType(ir->t_ptr, NULL, 0, 0));
    if (ir->windows) LLVMSetDLLStorageClass(fn, LLVMDLLExportStorageClass);
    ir->fn = fn;
    block = LLVMAppendBasicBlockInContext(ir->ctx, fn, "entry");
    LLVMPositionBuilderAtEnd(ir->b, block);
#define FIELD(member, value)                                                                                           \
    do {                                                                                                               \
        LLVMValueRef index__ = ir_i64(ir, offsetof(Descriptor, member));                                               \
        LLVMBuildStore(ir->b, value, LLVMBuildInBoundsGEP2(ir->b, ir->t_i8, desc, &index__, 1, ""));                   \
    } while (0)
    FIELD(abi, ir_i32(ir, SNX_ABI));
    FIELD(count, ir_i64(ir, count));
    FIELD(addresses, addresses);
    FIELD(functions, functions);
    FIELD(interp, LLVMConstNull(ir->t_ptr));
#undef FIELD
    LLVMBuildRet(ir->b, desc);
    return 0;
}

/* ---- the module cache ----
   A linked module is a function of the code it was made from, the addresses it sits at, where it may be entered (the
   leaders), the backend's settings, the contract with the runtime (SNX_ABI) and the CPU it was made for (the backends
   and rtlib.c use every extension the host has). Two 64-bit FNV-1a hashes of all that, with different offsets, name
   it in the cache folder */
typedef struct {
    uint64_t a, b;
} CacheKey;
static void key_bytes(CacheKey* k, const void* data, size_t size) {
    const unsigned char* p = (const unsigned char*)data;
    size_t i;
    for (i = 0; i < size; ++i) {
        k->a = (k->a ^ p[i]) * UINT64_C(0x100000001b3);
        k->b = (k->b ^ p[i]) * UINT64_C(0x100000001b3);
    }
}
static void key_u64(CacheKey* k, uint64_t v) {
    unsigned char bytes[8];
    int i;
    for (i = 0; i < 8; ++i) bytes[i] = (unsigned char)(v >> (i * 8));
    key_bytes(k, bytes, sizeof(bytes));
}
static char* cache_path(struct snx_recompiler* r, SnxCodegen* codegen) {
    CacheKey k;
    char *cpu, *features, name[64];
    size_t i, j;
    SnxU32 word;
    k.a = UINT64_C(0xcbf29ce484222325);
    k.b = UINT64_C(0x84222325cbf29ce4);
    key_bytes(&k, "snx-module-cache-1", 18);
    key_u64(&k, SNX_ABI);
    key_u64(&k, (uint64_t)r->backend);
    key_u64(&k, (uint64_t)(unsigned char)r->opt);
    cpu = LLVMGetHostCPUName();
    features = LLVMGetHostCPUFeatures();
    key_bytes(&k, cpu, strlen(cpu) + 1);
    key_bytes(&k, features, strlen(features) + 1);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    key_u64(&k, codegen->function_count);
    for (i = 0; i < codegen->function_count; ++i) {
        const SnxFunction* f = &codegen->functions[i];
        key_u64(&k, f->start);
        key_u64(&k, f->count);
        for (j = 0; j < f->count; ++j) {
            uint64_t a = snx_function_address(f, j);
            if (!snx_codegen_word(codegen, a, &word)) word = 0;
            key_u64(&k, a);
            key_u64(&k, (uint64_t)word << 1 | f->leaders[j]);
        }
    }
    snprintf(name, sizeof(name), "%016llx%016llx.%s", (unsigned long long)k.a, (unsigned long long)k.b,
#ifdef _WIN32
             "dll"
#else
             "so"
#endif
    );
    return snx_path_join(r->cache, name);
}
/* The runtime library compiled for this CPU (snx_ir_rtlib_object), once per optimization level and process: it takes
   seconds, and nothing else a recompiler sets changes it. Kept until the process ends */
#ifdef _WIN32
static SRWLOCK rtlib_lock = SRWLOCK_INIT;
#else
static pthread_mutex_t rtlib_lock = PTHREAD_MUTEX_INITIALIZER;
#endif
static unsigned char* rtlib_objects[6];
static size_t rtlib_sizes[6];

static void save_cached(const char* path, const unsigned char* data, size_t size);

/* the cache's copy of the compiled runtime library: a short-lived process (a just-in-time compiler's, which a
   program's every execve starts again) would spend most of its first compile on it */
static char* rtlib_cache_path(const char* cache, char opt) {
    CacheKey k;
    char *cpu, *features, name[80];
    k.a = UINT64_C(0xcbf29ce484222325);
    k.b = UINT64_C(0x84222325cbf29ce4);
    key_bytes(&k, "snx-rtlib-cache-1", 17);
    key_u64(&k, SNX_ABI);
    key_u64(&k, (uint64_t)(unsigned char)opt);
    key_bytes(&k, snx_rtlib_bitcode, snx_rtlib_bitcode_size);
    cpu = LLVMGetHostCPUName();
    features = LLVMGetHostCPUFeatures();
    key_bytes(&k, cpu, strlen(cpu) + 1);
    key_bytes(&k, features, strlen(features) + 1);
    LLVMDisposeMessage(cpu);
    LLVMDisposeMessage(features);
    snprintf(name, sizeof(name), "rtlib-%016llx%016llx.o", (unsigned long long)k.a, (unsigned long long)k.b);
    return snx_path_join(cache, name);
}

static int rtlib_object(const char* cache, char opt, const unsigned char** data, size_t* size, char* error,
                        size_t error_size) {
    static const char levels[] = "0123sz";
    const char* level = opt ? strchr(levels, opt) : NULL;
    size_t i = level ? (size_t)(level - levels) : 2;
    int rc = 0;
#ifdef _WIN32
    AcquireSRWLockExclusive(&rtlib_lock);
#else
    pthread_mutex_lock(&rtlib_lock);
#endif
    if (!rtlib_objects[i] && cache) {
        char* path = rtlib_cache_path(cache, levels[i]);
        rtlib_objects[i] = snx_read_file(path, &rtlib_sizes[i]);
        if (!rtlib_objects[i] &&
            !(rc = snx_ir_rtlib_object(levels[i], &rtlib_objects[i], &rtlib_sizes[i], error, error_size)))
            save_cached(path, rtlib_objects[i], rtlib_sizes[i]);
        free(path);
    } else if (!rtlib_objects[i])
        rc = snx_ir_rtlib_object(levels[i], &rtlib_objects[i], &rtlib_sizes[i], error, error_size);
    *data = rtlib_objects[i];
    *size = rtlib_sizes[i];
#ifdef _WIN32
    ReleaseSRWLockExclusive(&rtlib_lock);
#else
    pthread_mutex_unlock(&rtlib_lock);
#endif
    return rc;
}

/* the cached module at path, loaded; nonzero when there is none or it can't be loaded (then it is made again) */
static int load_cached(struct snx_recompiler* r, struct snx_recompiled* m, const char* path) {
    size_t size = 0;
    unsigned char* data = snx_read_file(path, &size);
    DescriptorFn describe;
    int rc = -1;
    if (!data) return -1;
    if (!load_image(r, m, data, size)) {
        describe = (DescriptorFn)(uintptr_t)image_symbol(m, DESCRIPTOR_SYMBOL);
        rc = describe ? load_descriptor(r, m, describe()) : -1;
        if (rc) {
            free(m->dispatch);
            m->dispatch = NULL;
            unload_image(m);
        }
    }
    free(data);
    r->error[0] = 0;
    return rc;
}
/* saved under a temporary name first and then renamed: another process may be loading the same module */
static void save_cached(const char* path, const unsigned char* data, size_t size) {
    static unsigned long serial;
    char* temporary = snx_format("%s.%lu.%lu.tmp", path, (unsigned long)(snx_seconds() * 1000000.0), ++serial);
    if (!snx_write_file(temporary, data, size) && !snx_replace(temporary, path)) {
        free(temporary);
        return;
    }
    snx_remove_tree(temporary);
    free(temporary);
}

static int recompile_llvm(struct snx_recompiler* r, struct snx_recompiled* m, SnxCodegen* codegen) {
    Queue q;
    SnxIr* ir = NULL;
    SnxLinkInput* inputs = NULL;
    SnxLinkOutput linked;
    unsigned char* descriptor_object = NULL;
    const unsigned char* rtlib = NULL;
    size_t descriptor_size = 0, rtlib_size = 0, *units = NULL, count = 0, i, n = 0;
    const char* exports[1];
    char error[1024];
    int rc = -1;
    char* cached = NULL;
    memset(&q, 0, sizeof(q));
    memset(&linked, 0, sizeof(linked));
    error[0] = 0;
    if (llvm_init(error, sizeof(error))) {
        fail(r, "%s", error);
        return -1;
    }
    if (r->cache) {
        cached = cache_path(r, codegen);
        if (!load_cached(r, m, cached)) {
            free(cached);
            return 0;
        }
    }
    units = chunks(codegen, r->jobs, &count);
    if (!units) {
        fail(r, "out of memory");
        return -1;
    }
    q.units = (Unit*)calloc(count ? count : 1, sizeof(*q.units));
    if (!q.units) {
        fail(r, "out of memory");
        free(units);
        return -1;
    }
    for (i = 0; i < count; ++i) {
        q.units[i].first = units[i];
        q.units[i].last = units[i + 1];
    }
    q.count = count;
    q.r = r;
    q.codegen = codegen;
    report(r, "LLVM: %lu code unit%s and the descriptor\n", (unsigned long)count, count == 1 ? "" : "s");
    if (count) run_queue(&q, r->jobs);
    if (q.failed) {
        fail(r, "LLVM backend: %s", q.error);
        goto done;
    }
    ir = snx_ir_new("module", r->opt, error, sizeof(error));
    if (!ir || llvm_descriptor(ir, codegen, error, sizeof(error)) ||
        (r->keep_ir && save_ir(ir, r->keep_ir, "module")) ||
        snx_ir_object(ir, r->opt, &descriptor_object, &descriptor_size, error, sizeof(error))) {
        fail(r, "LLVM backend: %s", error[0] ? error : "cannot save the descriptor's IR");
        goto done;
    }
    if (rtlib_object(r->cache, r->opt, &rtlib, &rtlib_size, error, sizeof(error))) {
        fail(r, "LLVM backend: %s", error);
        goto done;
    }
    /* link: runtime library, descriptor, units */
    inputs = (SnxLinkInput*)calloc(count + 2, sizeof(*inputs));
    if (!inputs) {
        fail(r, "out of memory");
        goto done;
    }
    inputs[n].data = rtlib;
    inputs[n].size = rtlib_size;
    inputs[n++].name = "rtlib";
    inputs[n].data = descriptor_object;
    inputs[n].size = descriptor_size;
    inputs[n++].name = "descriptor";
    for (i = 0; i < count; ++i) {
        inputs[n].data = q.units[i].object;
        inputs[n].size = q.units[i].size;
        inputs[n++].name = "code unit";
    }
    exports[0] = DESCRIPTOR_SYMBOL;
#ifdef _WIN32
    if (snx_link(inputs, n, exports, 1, "recompiled.dll", &linked, error, sizeof(error))) {
#else
    if (snx_link(inputs, n, exports, 1, "recompiled.so", &linked, error, sizeof(error))) {
#endif
        fail(r, "linking the recompiled module failed: %s", error);
        goto done;
    }
    report(r, "LLVM module: %.1f MB\n", (double)linked.size / 1000000.0);
    if (load_image(r, m, linked.data, linked.size)) goto done;
    if (cached) save_cached(cached, linked.data, linked.size);
    {
        DescriptorFn describe = (DescriptorFn)(uintptr_t)image_symbol(m, DESCRIPTOR_SYMBOL);
        if (!describe) {
            fail(r, "the recompiled module exports no descriptor");
            goto done;
        }
        if (load_descriptor(r, m, describe())) goto done;
    }
    rc = 0;
done:
    for (i = 0; i < count; ++i) free(q.units[i].object);
    free(q.units);
    free(units);
    free(inputs);
    free(descriptor_object);
    free(linked.data);
    free(cached);
    snx_ir_free(ir);
    return rc;
}

/* ---- the libgccjit backend ---- */

#ifdef SNX_WITH_GCCJIT
/* module_gccjit.c's structure: one context per chunk, compiled to an object file (jobs at a time, jit_units.c), then
   the descriptor in a last context whose in-memory compilation links the chunks and the runtime library with GCC's
   driver and loads the result */
static int jit_descriptor(SnxJit* jit, const SnxCodegen* codegen, char* error, size_t error_size) {
    gcc_jit_context* x = jit->ctx;
    Entry* e;
    size_t count, n, i;
    gcc_jit_rvalue **values, *base;
    gcc_jit_type *t_addresses, *t_functions, *t_desc;
    gcc_jit_lvalue *addresses, *functions, *desc;
    gcc_jit_function* fn;
    gcc_jit_block* block;
    e = entries(codegen, &count);
    if (!e) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    n = count ? count : 1;
    values = (gcc_jit_rvalue**)calloc(n, sizeof(*values));
    if (!values) {
        free(e);
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    for (i = 0; i < n; ++i) values[i] = jit_u64(jit, i < count ? e[i].address : 0);
    t_addresses = gcc_jit_context_new_array_type(x, NULL, jit->t_u64, (int)n);
    addresses = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_addresses, "snx_lib_addresses");
    gcc_jit_global_set_initializer_rvalue(addresses,
                                          gcc_jit_context_new_array_constructor(x, NULL, t_addresses, n, values));
    for (i = 0; i < n; ++i)
        values[i] = i < count ? gcc_jit_context_new_cast(
                                    x, NULL,
                                    gcc_jit_function_get_address(
                                        snx_jit_guest_function(jit, codegen->functions[e[i].function].start, 0), NULL),
                                    jit->t_guest_ptr)
                              : gcc_jit_context_null(x, jit->t_guest_ptr);
    t_functions = gcc_jit_context_new_array_type(x, NULL, jit->t_guest_ptr, (int)n);
    functions = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_functions, "snx_lib_functions");
    gcc_jit_global_set_initializer_rvalue(functions,
                                          gcc_jit_context_new_array_constructor(x, NULL, t_functions, n, values));
    free(values);
    free(e);
    /* static Descriptor DESC, as 8-byte words written at the C offsets */
    t_desc = gcc_jit_context_new_array_type(x, NULL, jit->t_u64, (int)((sizeof(Descriptor) + 7) / 8));
    desc = gcc_jit_context_new_global(x, NULL, GCC_JIT_GLOBAL_INTERNAL, t_desc, "snx_lib_descriptor");
    fn = gcc_jit_context_new_function(x, NULL, GCC_JIT_FUNCTION_EXPORTED, jit->t_ptr, DESCRIPTOR_SYMBOL, 0, NULL, 0);
    jit->fn = fn;
    block = gcc_jit_function_new_block(fn, "entry");
    jit->block = block;
    base = gcc_jit_lvalue_get_address(desc, NULL);
#define FIELD(member, type, value)                                                                                     \
    gcc_jit_block_add_assignment(block, NULL, jit_at(jit, base, offsetof(Descriptor, member), type), value)
    FIELD(abi, jit->t_u32, jit_u32(jit, SNX_ABI));
    FIELD(count, jit->t_u64, jit_u64(jit, count));
    FIELD(addresses, jit->t_ptr,
          gcc_jit_context_new_cast(x, NULL, gcc_jit_lvalue_get_address(addresses, NULL), jit->t_ptr));
    FIELD(functions, jit->t_ptr,
          gcc_jit_context_new_cast(x, NULL, gcc_jit_lvalue_get_address(functions, NULL), jit->t_ptr));
    FIELD(interp, jit->t_guest_ptr, gcc_jit_context_null(x, jit->t_guest_ptr));
#undef FIELD
    gcc_jit_block_end_with_return(block, NULL, gcc_jit_context_new_cast(x, NULL, base, jit->t_ptr));
    return 0;
}

static void jit_unit_done(const SnxJitUnits* units, size_t unit, size_t finished, double seconds) {
    report((struct snx_recompiler*)units->user, "  compiled unit %lu/%lu: functions %lu-%lu (%.1f s)\n",
           (unsigned long)finished, (unsigned long)units->count, (unsigned long)units->bounds[unit],
           (unsigned long)units->bounds[unit + 1], seconds);
}

static int recompile_gccjit(struct snx_recompiler* r, struct snx_recompiled* m, SnxCodegen* codegen) {
    SnxJitUnits u;
    SnxJit* jit = NULL;
    gcc_jit_result* result = NULL;
    size_t *units = NULL, count = 0, i;
    char *dir = NULL, *rtlib = NULL, **objects = NULL, error[1024];
    const char* message;
    int rc = -1;
    error[0] = 0;
    units = chunks(codegen, r->jobs, &count);
    if (!units) {
        fail(r, "out of memory");
        return -1;
    }
    dir = snx_temp_dir(NULL);
    if (!dir) {
        fail(r, "cannot create a temporary folder for libgccjit");
        free(units);
        return -1;
    }
    rtlib = snx_path_join(dir, "rtlib.o");
    if (snx_write_file(rtlib, snx_rtlib_object, snx_rtlib_object_size)) {
        fail(r, "cannot write %s", rtlib);
        goto done;
    }
    objects = (char**)calloc(count ? count : 1, sizeof(*objects));
    if (!objects) {
        fail(r, "out of memory");
        goto done;
    }
    for (i = 0; i < count; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "code_%03lu.o", (unsigned long)i);
        objects[i] = snx_path_join(dir, name);
    }
    report(r, "libgccjit: %lu code unit%s and the descriptor\n", (unsigned long)count, count == 1 ? "" : "s");
    memset(&u, 0, sizeof(u));
    u.codegen = codegen;
    u.bounds = units;
    u.objects = objects;
    u.count = count;
    u.opt = r->opt;
    u.jobs = r->jobs;
    u.keep_ir = r->keep_ir;
    u.done = jit_unit_done;
    u.user = r;
    if (snx_jit_compile_units(&u)) {
        fail(r, "%s", u.error);
        goto done;
    }
    /* the descriptor, linked by GCC's driver with the chunks and the runtime library, and loaded by libgccjit */
    jit = snx_jit_new(r->opt);
    if (!jit) {
        fail(r, "out of memory");
        goto done;
    }
    if (jit_descriptor(jit, codegen, error, sizeof(error))) {
        fail(r, "libgccjit: %s", error);
        goto done;
    }
    if (r->keep_ir) {
        char* path = snx_path_join(r->keep_ir, "module.c");
        gcc_jit_context_dump_to_file(jit->ctx, path, 0);
        free(path);
    }
    gcc_jit_context_add_driver_option(jit->ctx, rtlib);
    for (i = 0; i < count; ++i) gcc_jit_context_add_driver_option(jit->ctx, objects[i]);
    result = snx_jit_compile(jit);
    message = snx_jit_error(jit);
    if (message || !result) {
        fail(r, "libgccjit (module): %s", message ? message : "no result");
        goto done;
    }
    {
        DescriptorFn describe = (DescriptorFn)(uintptr_t)gcc_jit_result_get_code(result, DESCRIPTOR_SYMBOL);
        if (!describe) {
            fail(r, "libgccjit: the recompiled module has no descriptor");
            goto done;
        }
        if (load_descriptor(r, m, describe())) goto done;
    }
    m->jit_result = result;
    result = NULL;
    rc = 0;
done:
    snx_jit_free(jit);
    if (result) gcc_jit_result_release(result);
    if (objects) {
        for (i = 0; i < count; ++i) free(objects[i]);
        free(objects);
    }
    if (dir) snx_remove_tree(dir);
    free(dir);
    free(rtlib);
    free(units);
    return rc;
}
#endif

/* ---- A64 ---- */

/* the module's own copy of the code ranges, for its JIT: the views and the bytes in one block. 0, or -1 */
static int keep_code(struct snx_recompiler* r, struct snx_recompiled* m, const struct snx_code_range* ranges,
                     size_t count) {
    size_t i, total = 0, at;
    unsigned char* bytes;
    for (i = 0; i < count; ++i) {
        if (ranges[i].size > ((size_t)-1) / 2 - total) {
            fail(r, "the code ranges are too large");
            return -1;
        }
        total += ranges[i].size;
    }
    if (count > (((size_t)-1) / 2 - total) / sizeof(SnxCodeImage)) {
        fail(r, "the code ranges are too large");
        return -1;
    }
    m->views = (SnxCodeImage*)calloc(1, count * sizeof(SnxCodeImage) + total);
    if (!m->views) {
        fail(r, "out of memory for a copy of the code");
        return -1;
    }
    bytes = (unsigned char*)(m->views + count);
    for (i = 0, at = 0; i < count; ++i) {
        memcpy(bytes + at, ranges[i].code, ranges[i].size);
        m->views[i].image = bytes + at;
        m->views[i].image_size = ranges[i].size;
        m->views[i].base = ranges[i].address;
        m->views[i].text_size = ranges[i].size;
        at += ranges[i].size;
    }
    m->view_count = count;
    return 0;
}

/* the backend's half of snx_recompile_ranges and snx_recompile_from, for the functions codegen found; frees codegen */
static struct snx_recompiled* recompile(struct snx_recompiler* r, struct snx_recompiled* m, SnxCodegen* codegen) {
    int rc;
    m->functions = codegen->function_count;
    if (r->backend == SNX_RECOMPILER_FLAG_LLVM) rc = recompile_llvm(r, m, codegen);
#ifdef SNX_WITH_GCCJIT
    else if (r->backend == SNX_RECOMPILER_FLAG_GCCJIT)
        rc = recompile_gccjit(r, m, codegen);
#endif
    else {
        fail(r, "no backend");
        rc = -1;
    }
    snx_codegen_free(codegen);
    if (rc) {
        snx_recompiled_free(m);
        return NULL;
    }
    return m;
}

struct snx_recompiled* snx_recompile(struct snx_recompiler* r, uint64_t address, const void* code, size_t size) {
    struct snx_code_range range;
    range.address = address;
    range.code = code;
    range.size = size;
    return snx_recompile_ranges(r, &range, 1, NULL, 0);
}

struct snx_recompiled* snx_recompile_ranges(struct snx_recompiler* r, const struct snx_code_range* ranges,
                                            size_t range_count, const uint64_t* hints, size_t hint_count) {
    struct snx_recompiled* m;
    SnxCodeImage* views;
    SnxCodegen codegen;
    uint64_t lo, hi;
    r->error[0] = 0;
    views = images(r, ranges, range_count, 1, &lo, &hi);
    if (!views) return NULL;
    m = (struct snx_recompiled*)calloc(1, sizeof(*m));
    if (!m) {
        fail(r, "out of memory");
        free(views);
        return NULL;
    }
    m->code_lo = lo;
    m->code_hi = hi;
    m->image_fd = -1;
    m->opt = r->opt;
    if (keep_code(r, m, ranges, range_count)) {
        free(views);
        free(m);
        return NULL;
    }
    memset(&codegen, 0, sizeof(codegen));
    if (snx_codegen_analyze(&codegen, views, range_count, hints, hint_count, !r->verbose)) {
        fail(r, "function discovery failed");
        free(views);
        free(m->views);
        free(m);
        return NULL;
    }
    free(views);
    return recompile(r, m, &codegen);
}

struct snx_recompiled* snx_recompile_from(struct snx_recompiler* r, const struct snx_code_range* ranges,
                                          size_t range_count, const uint64_t* hints, size_t hint_count, size_t limit,
                                          int (*skip)(void* context, uint64_t address), void* context) {
    struct snx_recompiled* m;
    SnxCodeImage* views;
    SnxCodegen codegen;
    uint64_t lo, hi;
    size_t i;
    r->error[0] = 0;
    views = images(r, ranges, range_count, 1, &lo, &hi);
    if (!views) return NULL;
    if (snx_codegen_analyze_from(&codegen, views, range_count, hints, hint_count, limit, skip, context)) {
        fail(r, "no code at the starting addresses");
        snx_codegen_free(&codegen);
        free(views);
        return NULL;
    }
    free(views);
    report(r, "from 0x%llx: %lu functions, %lu instructions\n", (unsigned long long)hints[0],
           (unsigned long)codegen.function_count, (unsigned long)codegen.instruction_count);
    m = (struct snx_recompiled*)calloc(1, sizeof(*m));
    if (!m) {
        fail(r, "out of memory");
        snx_codegen_free(&codegen);
        return NULL;
    }
    m->image_fd = -1;
    m->opt = r->opt;
    /* the table covers what was found, not the ranges: they may be a whole program's code */
    m->code_lo = ~(uint64_t)0;
    m->code_hi = 0;
    for (i = 0; i < codegen.function_count; ++i) {
        const SnxFunction* f = &codegen.functions[i];
        uint64_t first = snx_function_address(f, 0), last = snx_function_address(f, f->count - 1) + 4;
        if (first < m->code_lo) m->code_lo = first;
        if (last > m->code_hi) m->code_hi = last;
    }
    return recompile(r, m, &codegen);
}

void snx_recompiled_free(struct snx_recompiled* m) {
    if (!m) return;
    if (m->jit && m->jit != JIT_NONE)
        snx_jit_service_free(m->jit); /* its threads stop before the code they read goes */
    free(m->views);
    if (m->dispatch) free((void**)m->dispatch - 1); /* load_descriptor's block, with the module in front */
#ifdef SNX_WITH_GCCJIT
    if (m->jit_result) gcc_jit_result_release((gcc_jit_result*)m->jit_result);
#endif
    unload_image(m);
    free(m);
}

void snx_recompiled_range(const struct snx_recompiled* m, uint64_t* code_lo, uint64_t* code_hi) {
    if (code_lo) *code_lo = m->code_lo;
    if (code_hi) *code_hi = m->code_hi;
}

size_t snx_recompiled_functions(const struct snx_recompiled* m) { return m->functions; }
size_t snx_recompiled_fallbacks(const struct snx_recompiled* m) {
    (void)m;
    return 0;
}

int snx_recompiled_bind(struct snx_recompiled* m, CPU* cpu) {
    if (!cpu->rt || !cpu->rt->svc || !cpu->rt->undef || !cpu->rt->brk || !cpu->rt->miss) return -1;
    cpu->disp = m->dispatch;
    cpu->code_lo = m->code_lo;
    cpu->code_hi = m->code_hi;
    return 0;
}

/* cpu.h's snx_lookup over the module's table, and the JIT's code for a pc the table lacks */
GuestFn snx_recompiled_lookup(struct snx_recompiled* m, uint64_t pc) { return module_code(m, pc); }

GuestFn snx_recompiled_function(const struct snx_recompiled* m, uint64_t pc) {
    uint64_t o = pc - m->code_lo;
    return o < m->code_hi - m->code_lo && !(pc & 3) ? __atomic_load_n(&m->dispatch[o >> 2], __ATOMIC_ACQUIRE) : NULL;
}

void snx_recompiled_set_fallback(struct snx_recompiled* m, GuestFn fallback) {
    __atomic_store_n(&m->fallback, fallback, __ATOMIC_RELEASE);
}

void snx_recompiled_call(struct snx_recompiled* m, CPU* cpu, uint64_t pc) {
    GuestFn f = module_code(m, pc);
    if (f)
        f(cpu, pc);
    else { /* nothing can run there: an undefined instruction, as default_miss's */
        cpu->pc = pc;
        cpu->rt->undef(cpu, pc, 0);
    }
}

void snx_recompiled_run(struct snx_recompiled* m, CPU* cpu, uint64_t pc, uint64_t stop) {
    cpu->pc = pc;
    while (cpu->pc != stop) snx_recompiled_call(m, cpu, cpu->pc);
}

/* ---- A32 ---- */

struct snx_recompiled32 {
    SnxArmExec* exec;
    SnxArmExecRange* ranges;
    size_t count;
};

struct snx_recompiled32* snx_recompile32(struct snx_recompiler* r, uint64_t address, size_t size) {
    struct snx_code_range range;
    range.address = address;
    range.code = NULL;
    range.size = size;
    return snx_recompile32_ranges(r, &range, 1);
}

struct snx_recompiled32* snx_recompile32_ranges(struct snx_recompiler* r, const struct snx_code_range* ranges,
                                                size_t range_count) {
    struct snx_recompiled32* m;
    SnxCodeImage* views;
    uint64_t lo, hi;
    size_t i;
    char error[256];
    r->error[0] = 0;
    if (r->backend != SNX_RECOMPILER_FLAG_LLVM) {
        fail(r, "A32 code is compiled by the LLVM backend only");
        return NULL;
    }
    views = images(r, ranges, range_count, 0, &lo, &hi);
    if (!views) return NULL;
    free(views);
    if (hi > ((uint64_t)1 << 32)) {
        fail(r, "A32 code lives below 4 GiB");
        return NULL;
    }
    m = (struct snx_recompiled32*)calloc(1, sizeof(*m));
    if (!m) {
        fail(r, "out of memory");
        return NULL;
    }
    m->ranges = (SnxArmExecRange*)calloc(range_count, sizeof(*m->ranges));
    if (!m->ranges) {
        fail(r, "out of memory");
        free(m);
        return NULL;
    }
    for (i = 0; i < range_count; ++i) {
        m->ranges[i].address = ranges[i].address;
        m->ranges[i].data = NULL; /* blocks decode from the CPU's memory (exec.c) */
        m->ranges[i].size = ranges[i].size;
    }
    m->count = range_count;
    m->exec = snx_arm_exec_new(error, sizeof(error));
    if (!m->exec) {
        fail(r, "%s", error);
        free(m->ranges);
        free(m);
        return NULL;
    }
    return m;
}

void snx_recompiled32_free(struct snx_recompiled32* m) {
    if (!m) return;
    snx_arm_exec_free(m->exec);
    free(m->ranges);
    free(m);
}

int snx_recompiled32_run(struct snx_recompiled32* m, SnxArmCPU* cpu, snx_svc_call32 svc, void* context) {
    cpu->exit = SNX_ARM_OK; /* a run after an exit starts over from r[15] */
    return snx_arm_exec_run(m->exec, cpu, m->ranges, m->count, svc, context);
}
