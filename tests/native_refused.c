/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An instruction a native back end has no code for (SnxNativeOptions.without: here a word of SHA512 and one of SM4, each
   in a function that never runs it and in one that does), and what is said of it. Of every machine's back end, on any
   host:
   - beside another back end (alone 0) the function is left, and the line at the recompile names the function, the op,
     its word, its address, the feature and the target, once a function, and counts it;
   - with none (alone 1) the function is made, and the line says it raises an undefined instruction.
   On an x86-64 host the code made alone runs: the function that never reaches its word returns, the other calls the
   runtime's undef with the instruction's pc and word, whose text (snx_a64_word_text) names the op and the feature. And
   through the library, with words LLVM has no code for (FP8's): the lines and their count, and the undefined
   instruction where one runs */
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/la64/la64_codegen.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/mips64/mips64_codegen.h"
#include "common/cpu/recompiler/mips64/mips64be_codegen.h"
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#include "common/cpu/recompiler/rv64/rv64_codegen.h"
#include "common/cpu/recompiler/s390x/s390x_codegen.h"
#include "common/cpu/recompiler/snx_recompiler.h"
#include "common/cpu/recompiler/x64/x64_cache.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE     0x10000u
#define EACH     0x40u
#define STOP     0u
#define SHA512H  0xCE628020u /* SHA512H Q0, Q1, V2.2D */
#define SM4E     0xCEC08420u /* SM4E V0.4S, V1.4S */
#define CBZ_OVER 0xB400005Fu /* CBZ XZR, over the next word */
#define ADD_ONE  0x91000400u /* ADD X0, X0, #1 */
#define F1CVTL   0x2E217820u /* F1CVTL V0.8H, V1.8B */
#define FSCALE   0x2EA2FC20u /* FSCALE V0.2S, V1.2S, V2.2S */
#define RET      0xD65F03C0u

/* the functions: the word each has (0 none), whether it runs it, its op and feature */
static struct {
    uint32_t words[3];
    uint32_t word;
    int runs;
    const char *op, *feature;
} k_functions[] = {{{CBZ_OVER, SHA512H, RET}, SHA512H, 0, "SHA512H", "FEAT_SHA512"},
                   {{SHA512H, ADD_ONE, RET}, SHA512H, 1, "SHA512H", "FEAT_SHA512"},
                   {{CBZ_OVER, SM4E, RET}, SM4E, 0, "SM4E", "FEAT_SM4"},
                   {{SM4E, ADD_ONE, RET}, SM4E, 1, "SM4E", "FEAT_SM4"},
                   {{ADD_ONE, ADD_ONE, RET}, 0, 1, "", ""}};
static const SnxU32 k_without[] = {SHA512H, SM4E}, k_library_without[] = {F1CVTL, FSCALE};
enum { FUNCTIONS = sizeof k_functions / sizeof *k_functions, REFUSED = 4 };

static struct {
    int failures;
    char lines[32][512];
    unsigned count;
    unsigned undefs;
    uint64_t undef_pc;
    char undef_text[64];
} g_test;

static void check(int ok, const char* what, const char* of) {
    if (ok) return;
    ++g_test.failures;
    fprintf(stderr, "FAIL: %s: %s\n", of, what);
}
static void on_line(const char* line) {
    if (g_test.count < sizeof g_test.lines / sizeof *g_test.lines)
        snprintf(g_test.lines[g_test.count], sizeof g_test.lines[0], "%s", line);
    ++g_test.count;
}
/* the runtime's undefined instruction: its text as the runtime's says it, then the run ends */
static void on_undef(CPU* c, uint64_t pc, uint32_t word) {
    ++g_test.undefs;
    g_test.undef_pc = pc;
    snx_a64_word_text(word, g_test.undef_text, sizeof g_test.undef_text);
    c->pc = STOP;
}
static void on_svc(CPU* c, uint32_t imm) { (void)c, (void)imm; }
static void on_brk(CPU* c, uint64_t pc, uint32_t imm) { (void)c, (void)pc, (void)imm; }

static uint8_t g_code[FUNCTIONS * EACH];
static uint64_t g_hints[FUNCTIONS];

/* the lines said of the functions of k_functions: one each, with every part of it, and the way it ends */
static void check_lines(const char* of, const char* end) {
    unsigned f, l;
    check(g_test.count == REFUSED, "a line for each function with an instruction it has no code for", of);
    for (f = 0; f < REFUSED; ++f) {
        char want[4][80];
        unsigned found = 0, k;
        snprintf(want[0], sizeof want[0], "the function at 0x%llx: ", (unsigned long long)g_hints[f]);
        snprintf(want[1], sizeof want[1], "%s (%08x) at 0x%llx needs %s", k_functions[f].op,
                 (unsigned)k_functions[f].word, (unsigned long long)(g_hints[f] + (k_functions[f].runs ? 0 : 4)),
                 k_functions[f].feature);
        snprintf(want[2], sizeof want[2], "(the guest's target is max)");
        snprintf(want[3], sizeof want[3], "%s", end);
        for (l = 0; l < g_test.count && l < sizeof g_test.lines / sizeof *g_test.lines; ++l) {
            for (k = 0; k < 4 && strstr(g_test.lines[l], want[k]); ++k) {}
            found += k == 4;
        }
        if (found != 1) {
            ++g_test.failures;
            fprintf(stderr, "FAIL: %s: %u lines name \"%s\", \"%s\", \"%s\" and \"%s\"\n", of, found, want[0], want[1],
                    want[2], want[3]);
        }
    }
}

typedef int (*MakeObject)(SnxCodegen* cg, const SnxNativeOptions* options, unsigned char* made, unsigned char** object,
                          size_t* size, char* error, size_t error_size);
static int make_arm64(SnxCodegen* cg, const SnxNativeOptions* o, unsigned char* m, unsigned char** p, size_t* n,
                      char* e, size_t s) {
    return snx_arm64_object(cg, 0, cg->function_count, o, 0, m, p, n, e, s);
}
#define MAKE(name)                                                                                                     \
    static int make_##name(SnxCodegen* cg, const SnxNativeOptions* o, unsigned char* m, unsigned char** p, size_t* n,  \
                           char* e, size_t s) {                                                                        \
        return snx_##name##_object(cg, 0, cg->function_count, o, m, p, n, e, s);                                       \
    }
MAKE(x64)
MAKE(rv64)
MAKE(ppc64)
MAKE(ppc64be)
MAKE(mips64)
MAKE(mips64be)
MAKE(la64)
MAKE(s390x)

static const struct {
    const char* name;
    MakeObject make;
} k_machines[] = {{"x86-64", make_x64},      {"arm64", make_arm64},      {"riscv64", make_rv64},
                  {"ppc64le", make_ppc64},   {"ppc64", make_ppc64be},    {"mips64el", make_mips64},
                  {"mips64", make_mips64be}, {"loongarch64", make_la64}, {"s390x", make_s390x}};

/* a machine's back end beside another and alone; its object made alone in *alone (the caller's) */
static void machine(const char* name, MakeObject make, SnxCodegen* cg, unsigned char** alone, size_t* alone_size) {
    SnxNativeOptions options = {0};
    unsigned char made[FUNCTIONS], *object = NULL;
    size_t size = 0, refused = 0, f;
    char error[256] = "", of[64];
    options.refused = &refused;
    options.without = k_without;
    options.without_count = sizeof k_without / sizeof *k_without;
    snprintf(of, sizeof of, "%s beside another back end", name);
    g_test.count = 0;
    check(make(cg, &options, made, &object, &size, error, sizeof error) >= 0, error, of);
    for (f = 0; f < FUNCTIONS; ++f)
        check(made[f] == !k_functions[f].word, "leaves the functions with one, no other", of);
    check(refused == REFUSED, "counts them", of);
    check_lines(of, ": LLVM makes the function");
    free(object);
    object = NULL;
    snprintf(of, sizeof of, "%s alone", name);
    options.alone = 1;
    refused = 0;
    g_test.count = 0;
    check(make(cg, &options, made, &object, &size, error, sizeof error) >= 0, error, of);
    for (f = 0; f < FUNCTIONS; ++f) check(made[f], "makes every function", of);
    check(refused == REFUSED, "counts them", of);
    check_lines(of, ": it raises an undefined instruction where it runs");
    *alone = object;
    *alone_size = size;
}

static void run_begin(CPU* c, RtAPI* rt, uint8_t* mem) {
    memset(c, 0, sizeof *c);
    c->mem = mem;
    c->rt = rt;
    c->x[30] = STOP;
    c->v[0].d[0] = 1, c->v[0].d[1] = 2, c->v[1].d[0] = 3, c->v[1].d[1] = 4, c->v[2].d[0] = 5, c->v[2].d[1] = 6;
    g_test.undefs = 0;
    g_test.undef_pc = 0;
    g_test.undef_text[0] = 0;
}
/* what function f's run must have been: undefined where it runs a word there is no code for, else its return */
static void check_run(const CPU* c, unsigned f, int undefined, const char* of) {
    char text[64];
    if (undefined) {
        snprintf(text, sizeof text, "%s, which needs %s", k_functions[f].op, k_functions[f].feature);
        check(g_test.undefs == 1, "an undefined instruction where the word runs", of);
        check(g_test.undef_pc == g_hints[f], "at its pc", of);
        check(!strcmp(g_test.undef_text, text), g_test.undef_text[0] ? g_test.undef_text : "a text", of);
        check(c->x[0] == 0, "nothing past it ran", of);
    } else {
        check(!g_test.undefs, "no undefined instruction", of);
        check(c->pc == STOP, "the function returned", of);
    }
}

int main(void) {
    static uint8_t mem[0x20000];
    SnxCodegen cg;
    SnxCodeImage view = {0};
    unsigned char* objects[sizeof k_machines / sizeof *k_machines] = {0};
    size_t sizes[sizeof k_machines / sizeof *k_machines] = {0};
    RtAPI rt;
    unsigned f, k, m;
    char error[256] = "";
    for (f = 0; f < FUNCTIONS; ++f) {
        g_hints[f] = CODE + f * EACH;
        for (k = 0; k < EACH / 4; ++k) {
            uint32_t w = k < 3 ? k_functions[f].words[k] : RET;
            for (unsigned b = 0; b < 4; ++b) g_code[f * EACH + 4 * k + b] = (uint8_t)(w >> (8 * b));
        }
    }
    memcpy(mem + CODE, g_code, sizeof g_code);
    view.image = g_code;
    view.image_size = view.text_size = sizeof g_code;
    view.base = CODE;
    if (snx_codegen_analyze(&cg, &view, 1, g_hints, FUNCTIONS, 1, SNX_A64_TARGET_MAX) ||
        cg.function_count != FUNCTIONS) {
        fprintf(stderr, "FAIL: the program's functions\n");
        return 1;
    }
    snx_error_sink(on_line);
    for (m = 0; m < sizeof k_machines / sizeof *k_machines; ++m) {
        if (!m && !snx_x64_available()) continue;
        machine(k_machines[m].name, k_machines[m].make, &cg, &objects[m], &sizes[m]);
    }
    snx_runtime_defaults(&rt);
    rt.undef = on_undef;
    rt.svc = on_svc;
    rt.brk = on_brk;
    if (snx_x64_available()) {
        /* a unit with one is not kept in the memory of units: made, and said, each time */
        SnxNativeOptions options = {0};
        for (k = 0; k < 2; ++k) {
            unsigned char made[FUNCTIONS], *object = NULL;
            size_t size = 0, refused = 0;
            int reused = 1;
            options.refused = &refused;
            options.without = k_without;
            options.without_count = sizeof k_without / sizeof *k_without;
            g_test.count = 0;
            check(
                !snx_x64_cached_object(&cg, 0, FUNCTIONS, &options, made, &object, &size, &reused, error, sizeof error),
                error, "the unit cache");
            check(!reused && refused == REFUSED, "made each time, and counted", "the unit cache");
            check_lines("the unit cache", ": LLVM makes the function");
            free(object);
        }
        /* the code made alone, run */
        SnxCodeLoader* loader = snx_code_loader_new(0, error, sizeof error);
        SnxLinkInput input = {0};
        const char* names[FUNCTIONS];
        char name[FUNCTIONS][40];
        SnxU64 at[FUNCTIONS];
        for (f = 0; f < FUNCTIONS; ++f) {
            snprintf(name[f], sizeof name[f], "F_%llX", (unsigned long long)g_hints[f]);
            names[f] = name[f];
        }
        input.name = "alone";
        input.data = objects[0];
        input.size = sizes[0];
        if (!loader || snx_code_load(loader, &input, 1, names, FUNCTIONS, at, error, sizeof error)) {
            check(0, error, "loading the code made alone");
        } else
            for (f = 0; f < FUNCTIONS; ++f) {
                CPU c;
                char of[80];
                snprintf(of, sizeof of, "%s run in the code made alone%s",
                         k_functions[f].op[0] ? k_functions[f].op : "ADD",
                         k_functions[f].runs ? "" : " (never reached)");
                run_begin(&c, &rt, mem);
                c.pc = g_hints[f];
                ((GuestFn)(uintptr_t)at[f])(&c, g_hints[f]);
                check_run(&c, f, k_functions[f].word && k_functions[f].runs, of);
            }
        snx_code_loader_free(loader);
        /* and through the library: words LLVM has no code for either, undefined where they run */
        for (f = 0; f < REFUSED; ++f) {
            uint32_t w = f < 2 ? F1CVTL : FSCALE;
            size_t at = f * EACH + (k_functions[f].runs ? 0 : 4);
            k_functions[f].word = w;
            k_functions[f].op = f < 2 ? "F1CVTL" : "FSCALE";
            k_functions[f].feature = "FEAT_FP8";
            for (unsigned b = 0; b < 4; ++b) mem[CODE + at + b] = (uint8_t)(w >> (8 * b));
        }
        struct snx_recompiler* r = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
        struct snx_code_range range = {CODE, mem + CODE, sizeof g_code};
        struct snx_recompiled* made = NULL;
        g_test.count = 0;
        if (r) snx_native_without(r, k_library_without, sizeof k_library_without / sizeof *k_library_without);
        if (r && !snx_recompiler_set_a64_target(r, SNX_RECOMPILER_A64_MAX))
            made = snx_recompile_ranges(r, &range, 1, g_hints, FUNCTIONS);
        if (!made)
            check(0, r ? snx_recompiler_error(r) : "no recompiler", "the library");
        else {
            char last[128];
            check(g_test.count == REFUSED + 1, "a line each and their count", "the library");
            snprintf(last, sizeof last, "%u of %u functions have an instruction it has no code for: LLVM made them",
                     REFUSED, FUNCTIONS);
            check(g_test.count && strstr(g_test.lines[g_test.count - 1], last) != NULL, "the count, at the end",
                  "the library");
            if (g_test.count) --g_test.count;
            check_lines("the library", ": LLVM makes the function");
            check(snx_recompiled_x64_functions(made) == 1, "LLVM makes the ones with one", "the library");
            for (f = 0; f < FUNCTIONS; ++f) {
                CPU c;
                run_begin(&c, &rt, mem);
                check(!snx_recompiled_bind(made, &c), "bound", "the library");
                snx_recompiled_run(made, &c, g_hints[f], STOP);
                check_run(&c, f, k_functions[f].word && k_functions[f].runs, "the library's LLVM code");
            }
            snx_recompiled_free(made);
        }
        snx_recompiler_free(r);
    }
    snx_error_sink(NULL);
    {
        char text[64];
        snx_a64_word_text(0x4E228420u, text, sizeof text); /* a base instruction: no feature */
        check(!strcmp(text, "ADD_v"), text, "a base instruction's text");
        snx_a64_word_text(0xD53B4440u, text, sizeof text);
        check(!strcmp(text, "MRS of FPMR (S3_3_C4_C4_2)"), text, "a read of FPMR, its text");
        snx_a64_word_text(0xD51B4440u, text, sizeof text);
        check(!strcmp(text, "MSR of FPMR (S3_3_C4_C4_2)"), text, "a write of FPMR, its text");
        snx_a64_word_text(0xD53BF000u, text, sizeof text);
        check(!strcmp(text, "MRS of S3_3_C15_C0_0"), text, "a register with no name here, its text");
        snx_a64_word_text(0xD50B7E20u, text, sizeof text);
        check(!strcmp(text, "SYS of S1_3_C7_C14_1"), text, "DC CIVAC's text");
        snx_a64_word_text(0, text, sizeof text);
        check(!strcmp(text, "no instruction"), text, "an unallocated word's text");
    }
    for (m = 0; m < sizeof k_machines / sizeof *k_machines; ++m) free(objects[m]);
    snx_codegen_free(&cg);
    printf("native_refused: %d failures\n", g_test.failures);
    return g_test.failures != 0;
}
