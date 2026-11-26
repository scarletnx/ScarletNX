/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The recompiler's library (library.c) in a program that links no LLVM: beside it the fallback that refuses
   (fallback_none.c) and the JIT's sources with the native back end's cache key, as a build without LLVM has them
   (tests/cmake/recompiler_core.cmake). Its modules are the native back end's whole: a program recompiled and run, the
   same from the cache folder with its descriptor read as data, a pc the exploration missed made by the JIT, the stale
   checks' table of a module whose calls go through the dispatch table, an instruction the native back end has no code
   for undefined where it runs, and what only LLVM makes refused by name. A host without a native back end has no
   recompiler here, which is checked instead */
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(ok, ...)                                                                                                 \
    do {                                                                                                               \
        if (!(ok)) {                                                                                                   \
            printf("FAIL: ");                                                                                          \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

/* the A64 program of recompiler_library.c (llvm-mc -triple=aarch64), at CODE in guest memory:
   entry:  mov x0,#0; mov x1,#10; mov x3,x30; loop: add x0,x0,x1; sub x1,x1,#1; cbnz x1,loop   (x0 = 55)
           bl helper; svc #0x42; str x0,[x2]; mov x30,x3; ret                                   (helper: x0 = 1055)
   helper: add x0,x0,#1000; ret
   far:    mov x0,#99; brk #7; ret                                                              (reached by no call) */
#define CODE   0x1000u
#define HELPER (CODE + 11 * 4)
#define FAR    (CODE + 13 * 4)
#define ADD    0x910fa000u /* the helper's add x0,x0,#1000 */
#define STORE  0x8000u
#define STOP   0u
#define MEMORY 0x10000u
static const uint32_t k_words[] = {0xd2800000, 0xd2800141, 0xaa1e03e3, 0x8b010000, 0xd1000421, 0xb5ffffc1,
                                   0x94000005, 0xd4000841, 0xf9000040, 0xaa0303fe, 0xd65f03c0, ADD,
                                   0xd65f03c0, 0xd2800c60, 0xd42000e0, 0xd65f03c0};

typedef struct {
    unsigned svcs, brks, undefs;
    uint64_t svc_x0, undef_pc;
    uint32_t brk_imm, undef_word;
} Seen;
static void svc(CPU* c, uint32_t imm) {
    Seen* s = (Seen*)c->thread;
    ++s->svcs;
    s->svc_x0 = imm == 0x42 ? c->x[0] : ~0ull;
    c->x[0] = 7;
}
static void brk(CPU* c, uint64_t pc, uint32_t imm) {
    Seen* s = (Seen*)c->thread;
    (void)pc;
    ++s->brks;
    s->brk_imm = imm;
    c->pc = STOP;
}
static void undef(CPU* c, uint64_t pc, uint32_t word) {
    Seen* s = (Seen*)c->thread;
    ++s->undefs;
    s->undef_pc = pc;
    s->undef_word = word;
    c->pc = STOP;
}

/* the lines the library says (snx_error), the last kept */
static char g_line[512];
static void on_line(const char* line) { snprintf(g_line, sizeof g_line, "%s", line); }

/* m run from pc with the hooks above, until STOP */
static Seen run(struct snx_recompiled* m, uint8_t* mem, uint64_t pc, CPU* cpu, RtAPI* rt) {
    Seen seen = {0};
    snx_runtime_defaults(rt);
    rt->svc = svc;
    rt->brk = brk;
    rt->undef = undef;
    memset(cpu, 0, sizeof *cpu);
    cpu->mem = mem;
    cpu->rt = rt;
    cpu->thread = &seen;
    CHECK(snx_recompiled_bind(m, cpu) == 0, "bind");
    cpu->x[2] = STORE;
    cpu->x[30] = STOP;
    snx_recompiled_run(m, cpu, pc, STOP);
    cpu->thread = NULL;
    return seen;
}

/* the entry's run: the loop, the call, the SVC, the store */
static void program(struct snx_recompiled* m, uint8_t* mem, const char* what) {
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    CPU cpu;
    Seen seen = run(m, mem, CODE, &cpu, &rt);
    CHECK(seen.svcs == 1 && seen.svc_x0 == 1055 && cpu.x[0] == 7 && mem[STORE] == 7 && !seen.brks && !seen.undefs,
          "%s: one SVC #0x42 with x0 = 1055, then 7 stored (saw %u, x0 %llu, stored %u, %u BRK, %u undefined)", what,
          seen.svcs, (unsigned long long)seen.svc_x0, mem[STORE], seen.brks, seen.undefs);
    memset(mem + STORE, 0, 8);
}

/* how many files of folder have a name that starts with prefix ("": all of them) */
static size_t files(const char* folder, const char* prefix) {
    char** names = NULL;
    size_t count = 0, n = 0;
    if (snx_list_dir(folder, &names, &count)) return 0;
    for (size_t i = 0; i < count; ++i) n += !strncmp(names[i], prefix, strlen(prefix));
    snx_free_strings(names, count);
    return n;
}

static void modules(uint8_t* mem, const char* cache) {
    struct snx_recompiler* r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    CHECK(r != NULL, "snx_recompiler_new with the %s back end", snx_native_name());
    if (!r) return;
    snx_recompiler_set_jobs(r, 1);
    /* compiled, then the same from the cache folder: the descriptor is data there, and no runtime library is kept */
    CHECK(!snx_recompiler_set_cache(r, cache), "a cache folder: %s", snx_recompiler_error(r));
    for (int pass = 0; pass < 2; ++pass) {
        struct snx_recompiled* m = snx_recompile(r, CODE, mem + CODE, sizeof k_words);
        CHECK(m != NULL, "pass %d: %s", pass, snx_recompiler_error(r));
        if (!m) continue;
        CHECK(snx_recompiled_functions(m) == 2, "pass %d: 2 functions (%zu)", pass, snx_recompiled_functions(m));
        CHECK(snx_recompiled_x64_functions(m) == (pass ? 0u : 2u),
              "pass %d: %s (%zu of the functions made by the %s back end)", pass,
              pass ? "from the cache folder" : "the native back end's whole", snx_recompiled_x64_functions(m),
              snx_native_name());
        program(m, mem, pass ? "from the cache" : "compiled");
        /* far, which no call reaches: the JIT's, and the native back end's there too */
        RtAPI rt = {.cas128 = snx_runtime_cas128};
        CPU cpu;
        Seen seen = run(m, mem, FAR, &cpu, &rt);
        CHECK(seen.brks == 1 && seen.brk_imm == 7 && cpu.x[0] == 99, "pass %d: far by the JIT (%u BRK #%u, x0 %llu)",
              pass, seen.brks, seen.brk_imm, (unsigned long long)cpu.x[0]);
        snx_recompiled_free(m);
    }
    const size_t all = files(cache, ""), jit = files(cache, "library-jit");
    CHECK(!files(cache, "rtlib-") && all - jit == 1,
          "the cache folder: the module and no runtime library of LLVM's (%zu files, %zu of them the JIT's)", all, jit);
    CHECK(!snx_recompiler_set_cache(r, NULL), "the cache folder off");

    /* every call through the dispatch table: the descriptor's stale checks' table, which retiring the helper marks */
    snx_recompiler_set_hook_direct_calls(r, 1);
    struct snx_recompiled* m = snx_recompile(r, CODE, mem + CODE, sizeof k_words);
    CHECK(m != NULL, "calls through the table: %s", snx_recompiler_error(r));
    if (m) {
        program(m, mem, "calls through the table");
        CHECK(snx_recompiled_invalidate(m, HELPER, 4, NULL, NULL) == 1 && !snx_recompiled_function(m, HELPER) &&
                  snx_recompiled_function(m, CODE),
              "the helper retired, the entry kept");
        program(m, mem, "the helper made again by the JIT");
        snx_recompiled_free(m);
    }
    snx_recompiler_set_hook_direct_calls(r, 0);

    /* an instruction the native back end has no code for: its function is made, the instruction undefined there */
    static const SnxU32 k_without[] = {ADD};
    snx_native_without(r, k_without, 1);
    g_line[0] = 0;
    snx_error_sink(on_line);
    m = snx_recompile(r, CODE, mem + CODE, sizeof k_words);
    snx_error_sink(NULL);
    CHECK(m != NULL, "without ADD: %s", snx_recompiler_error(r));
    CHECK(strstr(g_line, "1 of 2 functions have an instruction it has no code for: each made, the instruction "
                         "undefined where it runs") != NULL,
          "the count of them, said: %s", g_line);
    if (m) {
        RtAPI rt = {.cas128 = snx_runtime_cas128};
        CPU cpu;
        Seen seen = run(m, mem, CODE, &cpu, &rt);
        CHECK(seen.undefs == 1 && seen.undef_pc == HELPER && seen.undef_word == ADD && !seen.svcs,
              "the helper's ADD undefined where it runs (%u at %#llx, word %08x)", seen.undefs,
              (unsigned long long)seen.undef_pc, seen.undef_word);
        CHECK(snx_recompiled_x64_functions(m) == 2, "both made by the native back end (%zu)",
              snx_recompiled_x64_functions(m));
        snx_recompiled_free(m);
    }
    snx_native_without(r, NULL, 0);

    /* what only LLVM makes: refused, saying so */
    CHECK(snx_recompiler_set_keep_ir(r, "ir-folder") == -1 && strstr(snx_recompiler_error(r), "ir-folder") &&
              strstr(snx_recompiler_error(r), "no IR is kept"),
          "keeping the IR: %s", snx_recompiler_error(r));
    snx_recompiler_set_backend(r, 0);
    m = snx_recompile(r, CODE, mem + CODE, sizeof k_words);
    CHECK(!m && strstr(snx_recompiler_error(r), "LLVM alone") && strstr(snx_recompiler_error(r), "SNX_WITH_LLVM=OFF") &&
              strstr(snx_recompiler_error(r), snx_native_name()),
          "LLVM alone: %s", snx_recompiler_error(r));
    if (m) snx_recompiled_free(m);
    snx_recompiler_free(r);
}

int main(void) {
    CHECK(!snx_fallback_available() && !strcmp(snx_fallback_name(), "none"), "the fallback is none's (%s)",
          snx_fallback_name());
    if (!snx_native_available()) {
        CHECK(!snx_recompiler_has(SNX_RECOMPILER_FLAG_LLVM) && !snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM),
              "a host without a native back end has no recompiler with no LLVM");
        if (!g_failures) printf("the library without LLVM: no native back end on this host, and so no recompiler\n");
        return g_failures != 0;
    }
    CHECK(snx_recompiler_has(SNX_RECOMPILER_FLAG_LLVM), "the library makes code with the %s back end alone",
          snx_native_name());
    uint8_t* mem = (uint8_t*)calloc(1, MEMORY);
    char* cache = snx_temp_dir(NULL);
    CHECK(mem && cache, "guest memory and a cache folder");
    if (mem && cache) {
        for (size_t i = 0; i < sizeof k_words / sizeof *k_words; ++i)
            for (unsigned b = 0; b < 4; ++b) mem[CODE + 4 * i + b] = (uint8_t)(k_words[i] >> (8 * b));
        modules(mem, cache);
    }
    if (cache) snx_remove_tree(cache);
    free(cache);
    free(mem);
    if (!g_failures) printf("the library without LLVM (%s back end): as it should be\n", snx_native_name());
    return g_failures != 0;
}
