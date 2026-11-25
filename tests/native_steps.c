/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's native back end's step functions for the instruction tests (native_steps.h) */
#include "native_steps.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/recompiler/llvm/rtlib_object.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include <fenv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/* the units of one module file or of one load of the JIT's loader */
#define CHUNK 1024

/* x86-64's features past the baseline (x64.h's SGX_F_*: BMI1, BMI2, LZCNT, F16C, AVX, FMA, AES-NI and SHA-NI), as
   tests/CMakeLists.txt's snx_plain_ops_features says them */
#define X64_OPTIONAL 0x3d07u

struct NativeUnit {
    u32 word; /* a word's step: its word at pc for a guest with these features, where codegen is NULL */
    u64 pc, features;
    SnxCodegen* codegen; /* the functions [first, last) of a codegen of the test's own */
    size_t first, last;
    size_t step; /* the index of its first step */
    unsigned char* object;
    size_t size;
    int made;
    char table[24]; /* the unit's one global: its functions' addresses */
};

static int fail(char* error, size_t size, const char* what, const char* why) {
    snprintf(error, size, "%s%s%s", what, why ? ": " : "", why ? why : "");
    return -1;
}

int native_steps_available(void) { return snx_native_available(); }
int native_steps_host_mode(int mode) { return snx_x64_available() ? FE_TONEAREST : mode; }
int native_steps_forms(void) { return snx_x64_available() || snx_arm64_available() ? 2 : 1; }

int native_steps_begin(NativeSteps* s, int jit, int form, char* error, size_t error_size) {
    *s = (NativeSteps){0};
    if (form != NATIVE_STEPS_HOST && form != NATIVE_STEPS_PORTABLE) {
        snprintf(error, error_size, "form is %d, not a NATIVE_STEPS_ value (%d or %d)", form, NATIVE_STEPS_HOST,
                 NATIVE_STEPS_PORTABLE);
        return -1;
    }
    if (!native_steps_available()) return fail(error, error_size, "this host has no native back end", NULL);
    s->jit = jit;
    s->form = form;
    return 0;
}

int native_steps_begin_opt(NativeSteps* s, int jit, int form, char opt, char* error, size_t error_size) {
    (void)opt; /* a native back end has one level */
    return native_steps_begin(s, jit, form, error, error_size);
}

static NativeUnit* unit_new(NativeSteps* s) {
    if (s->nunits == s->cap_units) {
        s->cap_units = s->cap_units ? s->cap_units * 2 : 256;
        s->units = (NativeUnit*)snx_realloc(s->units, s->cap_units * sizeof(*s->units));
    }
    s->units[s->nunits] = (NativeUnit){0};
    return &s->units[s->nunits++];
}

/* the step's slot in the arrays the load fills */
static size_t step_new(NativeSteps* s, u64 pc) {
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 256;
        s->fns = (GuestFn*)snx_realloc(s->fns, s->cap * sizeof(*s->fns));
        s->pcs = (u64*)snx_realloc(s->pcs, s->cap * sizeof(*s->pcs));
        s->stale = (u8**)snx_realloc(s->stale, s->cap * sizeof(*s->stale));
    }
    s->fns[s->count] = NULL;
    s->pcs[s->count] = pc;
    s->stale[s->count] = NULL;
    return s->count++;
}

size_t native_steps_add(NativeSteps* s, u32 word, u64 pc, u64 features) {
    NativeUnit* u = unit_new(s);
    u->word = word;
    u->pc = pc;
    u->features = features;
    u->step = step_new(s, pc);
    return u->step;
}

size_t native_steps_add_function(NativeSteps* s, SnxCodegen* codegen, const SnxFunction* function) {
    const size_t index = (size_t)(function - codegen->functions);
    NativeUnit* u = s->nunits ? &s->units[s->nunits - 1] : NULL;
    const size_t step = step_new(s, snx_function_address(function, 0));
    if (!u || u->codegen != codegen || u->last != index) {
        u = unit_new(s);
        u->codegen = codegen;
        u->first = u->last = index;
        u->step = step;
    }
    u->last = index + 1;
    return step;
}

/* A word whose branch goes to itself: a function that contains it would loop in place. Its poll, taken at every pass
   (CPU.poll is set while a step runs), ends it: the poll marks the unit's code stale and goes back to the function's
   entry, which then leaves */
static int branches_to_itself(u32 word, u64 pc, u64 features) {
    SnxA64Insn insn;
    snx_a64_decode_word(word, features, &insn);
    return (insn.flow == SNX_A64_FLOW_B || insn.flow == SNX_A64_FLOW_BC) && snx_a64_branch_target(&insn, pc) == pc;
}

/* the functions of one codegen as the host's back end makes them, for the form's processor */
static int make(const NativeSteps* s, SnxCodegen* codegen, size_t first, size_t last, SnxNativeOptions* options,
                unsigned char* made, NativeUnit* u, char* error, size_t error_size) {
    if (s->form == NATIVE_STEPS_PORTABLE && snx_arm64_available())
        return snx_arm64_object(codegen, first, last, options, snx_arm64_windows() ? SNX_ARM64_F_WIN : 0, made,
                                &u->object, &u->size, error, error_size);
    return snx_native_object(codegen, first, last, options, made, &u->object, &u->size, error, error_size);
}

static int unit_make(const NativeSteps* s, NativeUnit* u, char* error, size_t error_size) {
    size_t refused = 0, i;
    SnxNativeOptions options = {.jit = 1, .table_name = u->table, .refused = &refused};
    unsigned char made[1];
    int rc;
    if (u->codegen) {
        const size_t count = u->last - u->first;
        unsigned char* all = (unsigned char*)snx_alloc(count);
        rc = make(s, u->codegen, u->first, u->last, &options, all, u, error, error_size);
        u->made = 1;
        for (i = 0; i < count; ++i) u->made &= all[i];
        free(all);
        return rc;
    }
    unsigned char image[4], leader = 1;
    snx_st_le32(image, u->word);
    SnxCodeImage module = {.image = image, .image_size = sizeof image, .base = u->pc, .text_size = sizeof image};
    SnxFunction function = {
        .start = u->pc, .end = u->pc + sizeof image, .count = 1, .leaders = &leader, .leader_count = 1};
    /* a call of its own address goes through CPU.disp, as any other's does, and not into the function */
    SnxCodegen codegen = {.features = u->features,
                          .modules = &module,
                          .module_count = 1,
                          .functions = &function,
                          .function_count = 1,
                          .hook_direct_calls = 1};
    options.stale = branches_to_itself(u->word, u->pc, u->features);
    rc = make(s, &codegen, 0, 1, &options, made, u, error, error_size);
    u->made = made[0];
    return rc;
}

/* The data of a chunk: an array of pointers, &snx_stale and then each unit's table, its one global (snx_test_table_N)
   and, where the chunk defines it, snx_stale */
static int chunk_data(const NativeSteps* s, size_t first, size_t last, const char* name, int stale, unsigned char** out,
                      size_t* size, char* error, size_t error_size) {
    unsigned pointer = 0;
    SnxXElf* o = snx_native_data(&pointer);
    size_t at, n = 0, i, bytes = 8;
    int rc;
    for (i = first; i < last; ++i) bytes += s->units[i].made ? 8 : 0;
    if (!o) return fail(error, error_size, "out of memory", NULL);
    if (stale) {
        at = snx_xelf_put(o, SNX_ELF_DATA, NULL, 8, 8);
        snx_xelf_symbol(o, "snx_stale", SNX_ELF_DATA, at, 8, SNX_ELF_GLOBAL);
    }
    at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, bytes, 8);
    snx_xelf_symbol(o, name, SNX_ELF_RELRO, at, bytes, SNX_ELF_GLOBAL);
    snx_xelf_reloc(o, SNX_ELF_RELRO, at, "snx_stale", pointer, 0);
    for (i = first; i < last; ++i)
        if (s->units[i].made) snx_xelf_reloc(o, SNX_ELF_RELRO, at + 8 * ++n, s->units[i].table, pointer, 0);
    rc = snx_xelf_write(o, out, size);
    snx_xelf_free(o);
    return rc ? fail(error, error_size, "cannot write the steps' table", NULL) : 0;
}

/* the module of a chunk's objects, from a file of its own: its table's address, or NULL with the reason in error */
static void* module_table(NativeSteps* s, const SnxLinkInput* inputs, size_t count, const char* table, char* error,
                          size_t error_size) {
    SnxLinkOutput linked;
    char *path, *file;
    void *library, *address;
    if (!s->dir) s->dir = snx_temp_dir(NULL);
    if (!s->dir) {
        fail(error, error_size, "cannot make a temporary directory", NULL);
        return NULL;
    }
    if (snx_link(inputs, count, &table, 1, "steps", &linked, error, error_size)) return NULL;
#ifdef _WIN32
    file = snx_format("steps%zu.dll", s->nlibraries);
#else
    file = snx_format("steps%zu.so", s->nlibraries);
#endif
    path = snx_path_join(s->dir, file);
    free(file);
    if (snx_write_file(path, linked.data, linked.size)) {
        free(linked.data);
        free(path);
        fail(error, error_size, "cannot write the module", NULL);
        return NULL;
    }
    free(linked.data);
    s->libraries = (void**)snx_realloc(s->libraries, (s->nlibraries + 1) * sizeof(*s->libraries));
#ifdef _WIN32
    library = LoadLibraryA(path);
    address = library ? (void*)GetProcAddress((HMODULE)library, table) : NULL;
    if (!address) fail(error, error_size, "cannot load the module", NULL);
#else
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    address = library ? dlsym(library, table) : NULL;
    if (!address) fail(error, error_size, "cannot load the module", dlerror());
#endif
    free(path);
    if (library) s->libraries[s->nlibraries++] = library;
    return address;
}

/* loads the units [first, last): the address of their chunk's table, or NULL with the reason in error */
static void* chunk_load(NativeSteps* s, size_t first, size_t last, char* error, size_t error_size) {
    /* a load of the JIT's loader finds an earlier one's globals: snx_stale is the first chunk's. A module has its own */
    const int stale = !s->jit || !first;
    char name[32];
    const char* names[1] = {name};
    SnxLinkInput* inputs = (SnxLinkInput*)snx_alloc((last - first + 2) * sizeof(*inputs));
    unsigned char* data = NULL;
    size_t data_size = 0, n = 0, i;
    void* address = NULL;
    SnxU64 loaded = 0;
    snprintf(name, sizeof name, "snx_test_table_%zu", first / CHUNK);
    if (chunk_data(s, first, last, name, stale, &data, &data_size, error, error_size)) {
        free(inputs);
        return NULL;
    }
    if (!s->jit) inputs[n++] = (SnxLinkInput){snx_rtlib_object, snx_rtlib_object_size, "rtlib"};
    inputs[n++] = (SnxLinkInput){data, data_size, "table"};
    for (i = first; i < last; ++i)
        if (s->units[i].made) inputs[n++] = (SnxLinkInput){s->units[i].object, s->units[i].size, "step"};
    if (!s->jit)
        address = module_table(s, inputs, n, name, error, error_size);
    else if (!snx_code_load(s->loader, inputs, n, names, 1, &loaded, error, error_size))
        address = (void*)(size_t)loaded;
    free(inputs);
    free(data);
    return address;
}

int native_steps_dump(NativeSteps* s, const char* prefix, char* error, size_t error_size) {
    NativeUnit* u = s->nunits ? s->units : NULL;
    char* path = snx_format("%s.o", prefix);
    int failed = 0;
    if (!u) failed = 1, fail(error, error_size, "no step to dump", NULL);
    if (!failed) snprintf(u->table, sizeof u->table, "T_0");
    if (!failed) failed = unit_make(s, u, error, error_size);
    if (!failed && !u->object) failed = 1, fail(error, error_size, "the back end has no code for the first step", NULL);
    if (!failed && snx_write_file(path, u->object, u->size)) {
        snprintf(error, error_size, "cannot write %s", path);
        failed = 1;
    }
    free(path);
    return failed ? -1 : 0;
}

/* each unit's object, named its table's; the words without code are counted and, with no NativeSteps.partial, refused */
static int units_make(NativeSteps* s, char* error, size_t error_size) {
    size_t i, failed = s->nunits;
    if (s->form == NATIVE_STEPS_PORTABLE) snx_x64_without(X64_OPTIONAL);
    for (i = 0; i < s->nunits && failed == s->nunits; ++i) {
        NativeUnit* u = &s->units[i];
        snprintf(u->table, sizeof u->table, "T_%zu", i);
        if (unit_make(s, u, error, error_size))
            failed = i;
        else if (!u->made && !s->partial) {
            snprintf(error, error_size, "the back end has no code for %08x", u->word);
            failed = i;
        }
    }
    snx_x64_without(0);
    return failed == s->nunits ? 0 : -1;
}

int native_steps_load(NativeSteps* s, char* error, size_t error_size) {
    size_t first, last, i, k, n;
    if (!s->nunits) native_steps_add(s, 0xD503201Fu, 0, 0); /* an empty table is no array */
    if (units_make(s, error, error_size)) return -1;
    if (s->jit) {
        SnxLinkInput rtlib = {snx_rtlib_object, snx_rtlib_object_size, "rtlib"};
        s->loader = snx_code_loader_new(0, error, error_size);
        if (!s->loader || snx_code_load(s->loader, &rtlib, 1, NULL, 0, NULL, error, error_size)) return -1;
    }
    for (first = 0; first < s->nunits; first = last) {
        void** table;
        last = first + CHUNK < s->nunits ? first + CHUNK : s->nunits;
        for (n = 0, i = first; i < last; ++i) n += (size_t)s->units[i].made;
        if (!n) continue;
        table = (void**)chunk_load(s, first, last, error, error_size);
        if (!table) return -1;
        for (n = 0, i = first; i < last; ++i) {
            const NativeUnit* u = &s->units[i];
            const size_t count = u->codegen ? u->last - u->first : 1;
            GuestFn* functions;
            if (!u->made) continue;
            functions = (GuestFn*)table[1 + n++];
            for (k = 0; k < count; ++k) {
                s->fns[u->step + k] = functions[k];
                s->stale[u->step + k] = (u8*)table[0];
            }
        }
    }
    for (i = 0; i < s->nunits; ++i) {
        free(s->units[i].object);
        s->units[i].object = NULL;
    }
    return 0;
}

void native_steps_end(NativeSteps* s) {
    size_t i;
    for (i = 0; i < s->nunits; ++i) free(s->units[i].object);
    if (s->loader) snx_code_loader_free(s->loader);
    for (i = 0; i < s->nlibraries; ++i)
#ifdef _WIN32
        FreeLibrary((HMODULE)s->libraries[i]);
#else
        dlclose(s->libraries[i]);
#endif
    if (s->dir) {
        snx_remove_tree(s->dir);
        free(s->dir);
    }
    free(s->libraries);
    free(s->units);
    free(s->fns);
    free(s->pcs);
    free(s->stale);
    *s = (NativeSteps){0};
}

/* ---- a step's run ---- */

/* where the step that runs goes on: the first pc the runtime is asked for (RtAPI.miss) */
static struct {
    int missed;
    u64 next;
    u8* stale;
} g_run;

/* a call or a jump of the word's: its target. CPU.pc, the return of a call that came back, is no pc of the guest's, so
   the function leaves at the check of it and goes no further */
static void on_miss(CPU* c, u64 pc) {
    if (!g_run.missed) g_run.next = pc;
    g_run.missed = 1;
    c->pc = ~(u64)0;
}

/* the branch back of a word that goes to itself: the function's entry is stale, and leaves with its pc in CPU.pc */
static void on_poll(CPU* c) {
    (void)c;
    *g_run.stale = 1;
}

u64 native_step_run(NativeStep step, CPU* c) {
    const NativeSteps* s = step.steps;
    const CPU before = *c;
    RtAPI rt = {0};
    u64 next;
    if (c->rt) rt = *c->rt;
    rt.miss = on_miss;
    rt.poll = on_poll;
    g_run.missed = 0;
    g_run.stale = s->stale[step.index];
    *g_run.stale = 0;
    c->rt = &rt;
    c->disp = NULL; /* nothing is compiled elsewhere: every call and jump misses */
    c->code_lo = c->code_hi = 0;
    atomic_store_explicit(&c->poll, 1, memory_order_relaxed);
    s->fns[step.index](c, s->pcs[step.index]);
    next = g_run.missed ? g_run.next : c->pc;
    c->pc = before.pc;
    c->rt = before.rt;
    c->disp = before.disp;
    c->code_lo = before.code_lo;
    c->code_hi = before.code_hi;
    atomic_store_explicit(&c->poll, atomic_load_explicit(&before.poll, memory_order_relaxed), memory_order_relaxed);
    *g_run.stale = 0;
    return next;
}
