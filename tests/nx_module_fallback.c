/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game module made through the fallback (nx/loader/module_fallback.h, recompiler/fallback.h), as a program's link
   gives it: LLVM's, or none (a program that links no LLVM). The builders are this program's own (module_llvm.c here,
   module_arm.c in nx_module_fallback_arm.c), so the test chooses what the native back end leaves:
   - every function or block the native back end's: the module, which nx_module_fallback_same finds the same file
     whichever fallback the program links;
   - a function with an instruction the native back end has no code for: LLVM makes it, and with none the native back
     end makes it, the instruction undefined where it runs;
   - one it leaves otherwise, or a block it leaves: LLVM makes it, none refuses the module, naming the code, its
     address, its first instruction and why;
   - what LLVM alone has (the IR kept, a --layout, a profile cache's key, a host with no native back end): LLVM's, and
     none refuses it naming the option and its value
     nx_module_fallback FOLDER   (where its modules go, a folder of each fallback's) */
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/x64/x64_cache.h"
#include "common/utilities/common.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/module_llvm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SUFFIX ".dll"
#else
#define SUFFIX ".so"
#endif
#define TEXT_OFF 0x1000u
#define F1       (SNX_CODE_BASE + TEXT_OFF + 0x10u) /* the second function: ADD X0, X0, #1; B f2 */
#define F2       (SNX_CODE_BASE + TEXT_OFF + 0x18u) /* the third, which only f1's B reaches: a hint makes it one */
#define ADD_ONE  0x91000400u

static struct {
    int failures;
    SnxU32 without;  /* a word the native back end is to have no code for, 0 for none */
    int leave;       /* its function left to the fallback whatever the builder asks, as one past its branches' reach */
    int no_native;   /* the host is to have no native back end */
    char said[8192]; /* the errors the builder said since the last build, a line each */
    unsigned char memory[TEXT_OFF + 0x1000];
} g_nx_module_fallback;

static int test_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                              unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                              size_t error_size) {
    SnxNativeOptions asked = *options;
    asked.without = &g_nx_module_fallback.without;
    asked.without_count = g_nx_module_fallback.without != 0;
    if (g_nx_module_fallback.leave) asked.alone = 0;
    return snx_native_cached_object(codegen, first, last, &asked, made, object, size, reused, error, error_size);
}
static int test_native_available(void) { return !g_nx_module_fallback.no_native && snx_native_available(); }
#define snx_native_cached_object test_cached_object
#define snx_native_available     test_native_available
#include "nx/loader/module_llvm.c"
#undef snx_native_cached_object
#undef snx_native_available

/* nx_module_fallback_arm.c's: the same of a 32-bit program, its failures counted */
int snx_test_arm_fallback(const char* folder);

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            ++g_nx_module_fallback.failures;                                                                           \
        }                                                                                                              \
    } while (0)

static void sink(const char* line) {
    size_t n = strlen(g_nx_module_fallback.said);
    snprintf(g_nx_module_fallback.said + n, sizeof g_nx_module_fallback.said - n, "%s\n", line);
}

/* f0: x9 = x30; bl f1; x30 = x9; ret. f1: x0 += 1; b f2. f2: x0 += 1; ret. Each function a unit */
static void program(void) {
    static const SnxU32 code[] = {0xAA1E03E9u, 0x94000003u, 0xAA0903FEu, 0xD65F03C0u,
                                  ADD_ONE,     0x14000001u, ADD_ONE,     0xD65F03C0u};
    for (size_t i = 0; i < sizeof code / sizeof *code; ++i)
        for (size_t j = 0; j < 4; ++j)
            g_nx_module_fallback.memory[TEXT_OFF + 4 * i + j] = (unsigned char)(code[i] >> (8 * j));
}

/* the program's module as output, with what llvm and options add (their output, title and the like set here): 0, or
   the builder's failure, which it said (g_nx_module_fallback.said) */
static int build(const char* output, SnxLlvmBuild llvm, SnxCodegenOptions options) {
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    image.name = "test";
    image.base = SNX_CODE_BASE;
    image.size = sizeof g_nx_module_fallback.memory;
    image.image = g_nx_module_fallback.memory;
    image.image_size = sizeof g_nx_module_fallback.memory;
    image.text_off = TEXT_OFF;
    image.text_size = 0x1000;
    image.is_nro = 1;
    loaded.modules = &image;
    loaded.module_count = 1;
    loaded.npdm.title_id = 0x0100000000000000ull;
    loaded.npdm.stack = 0x100000;
    loaded.npdm.is64 = 1;
    strcpy(loaded.npdm.name, "test");
    options.title = "test";
    options.abi = SNX_ABI;
    options.quiet = 1;
    options.chunk_instrs = 1;
    static const SnxU64 hints[1] = {F2};
    options.hints = hints;
    options.hint_count = 1;
    llvm.output = output;
    llvm.opt = '2';
    llvm.jobs = 1;
    llvm.quiet = 1;
    g_nx_module_fallback.said[0] = 0;
    return snx_build_module_llvm(&loaded, &options, &llvm);
}

/* where a file of folder's is: malloc'd */
static char* at(const char* folder, const char* name) { return snx_path_join(folder, name); }

static void a64(const char* folder) {
    const int llvm = snx_fallback_available();
    SnxLlvmBuild asked = {0};
    SnxCodegenOptions options = {0};
    char *module = at(folder, "a64" SUFFIX), *left = at(folder, "left" SUFFIX), wanted[512];
    /* every function the native back end's: the module, nothing said */
    CHECK(!build(module, asked, options) && snx_is_file(module) && !g_nx_module_fallback.said[0],
          "the module of the native back end's functions: %s", g_nx_module_fallback.said);
    /* an instruction it has no code for: f1's and f2's ADD */
    g_nx_module_fallback.without = ADD_ONE;
    int rc = build(left, asked, options);
    if (llvm)
        CHECK(!rc && snx_is_file(left) &&
                  strstr(g_nx_module_fallback.said,
                         "native back end: 2 of 3 functions have an instruction it has no code for: LLVM made them"),
              "LLVM makes the functions left: %s", g_nx_module_fallback.said);
    else
        CHECK(!rc && snx_is_file(left) &&
                  strstr(g_nx_module_fallback.said, "it raises an undefined instruction where it runs") &&
                  strstr(g_nx_module_fallback.said, "native back end: 2 of 3 functions have an instruction it has no "
                                                    "code for: each made, the instruction undefined where it runs") &&
                  !strstr(g_nx_module_fallback.said, "LLVM"),
              "with none the native back end makes them, the ADD undefined: %s", g_nx_module_fallback.said);
    /* a function it leaves otherwise */
    g_nx_module_fallback.leave = 1;
    free(left);
    left = at(folder, "refused" SUFFIX);
    rc = build(left, asked, options);
    g_nx_module_fallback.leave = 0;
    g_nx_module_fallback.without = 0;
    if (llvm)
        CHECK(!rc && snx_is_file(left), "LLVM makes the functions left: %s", g_nx_module_fallback.said);
    else {
        snprintf(wanted, sizeof wanted, "code_001: the function at 0x%llx, which starts with ADD",
                 (unsigned long long)F1);
        CHECK(rc && !snx_is_file(left) && strstr(g_nx_module_fallback.said, wanted) &&
                  strstr(g_nx_module_fallback.said, "(91000400): the native back end") &&
                  strstr(g_nx_module_fallback.said, snx_native_name()) &&
                  strstr(g_nx_module_fallback.said,
                         "left it and this build has no LLVM to make it (SNX_WITH_LLVM=OFF)") &&
                  !strstr(g_nx_module_fallback.said, "LLVM backend"),
              "none refuses a function the native back end left, naming it (%s): %s", wanted,
              g_nx_module_fallback.said);
    }
    /* each unit's IR kept */
    char *kept = at(folder, "kept"), *ir = at(kept, "code_000.ll"), *descriptor_ir = at(kept, "module.ll");
    char* output = at(folder, "kept" SUFFIX);
    CHECK(!snx_mkdirs(kept), "cannot make %s", kept);
    asked.keep_ir = kept;
    rc = build(output, asked, options);
    asked.keep_ir = NULL;
    if (llvm)
        CHECK(!rc && snx_is_file(ir) && snx_is_file(descriptor_ir), "LLVM keeps its IR in %s: %s", kept,
              g_nx_module_fallback.said);
    else {
        snprintf(wanted, sizeof wanted,
                 "--keep-ir: the IR it keeps in %s is LLVM's, and this build has no LLVM (SNX_WITH_LLVM=OFF)\n", kept);
        CHECK(rc && !strcmp(g_nx_module_fallback.said, wanted) && !snx_is_file(output), "none and --keep-ir: %s",
              g_nx_module_fallback.said);
    }
    /* a --layout */
    const SnxU64 order[1] = {F1};
    asked.layout = order;
    asked.layout_count = 1;
    free(output);
    output = at(folder, "layout" SUFFIX);
    rc = build(output, asked, options);
    asked.layout = NULL;
    asked.layout_count = 0;
    if (llvm)
        CHECK(!rc && snx_is_file(output), "LLVM orders the module: %s", g_nx_module_fallback.said);
    else
        CHECK(rc &&
                  !strcmp(g_nx_module_fallback.said, "--layout: the order of its 1 addresses puts LLVM's functions in "
                                                     "sections of their own, and this build has no LLVM "
                                                     "(SNX_WITH_LLVM=OFF)\n"),
              "none and --layout: %s", g_nx_module_fallback.said);
    /* a profile cache, which there is none of: its key is made, and the file not opened */
    char* cache = at(folder, "no.jitcache");
    options.jit_cache = cache;
    free(output);
    output = at(folder, "cache" SUFFIX);
    rc = build(output, asked, options);
    options.jit_cache = NULL;
    if (llvm)
        CHECK(rc && strstr(g_nx_module_fallback.said, "JIT cache: ") && !strstr(g_nx_module_fallback.said, "key"),
              "LLVM makes the cache's key: %s", g_nx_module_fallback.said);
    else {
        snprintf(wanted, sizeof wanted,
                 "--jit-cache %s: the key of a profile cache made at -O2 names the LLVM that made its units, and this "
                 "build has no LLVM to make it (SNX_WITH_LLVM=OFF)\n",
                 cache);
        CHECK(rc && !strcmp(g_nx_module_fallback.said, wanted), "none and --jit-cache: %s", g_nx_module_fallback.said);
    }
    /* a host with no native back end */
    g_nx_module_fallback.no_native = 1;
    free(output);
    output = at(folder, "alone" SUFFIX);
    rc = build(output, asked, options);
    g_nx_module_fallback.no_native = 0;
    if (llvm)
        CHECK(!rc && snx_is_file(output), "LLVM makes the whole module: %s", g_nx_module_fallback.said);
    else
        CHECK(rc &&
                  !strcmp(g_nx_module_fallback.said, "this host has no native back end, and this build has no LLVM to "
                                                     "make the module (SNX_WITH_LLVM=OFF)\n") &&
                  !snx_is_file(output),
              "none on a host with no native back end: %s", g_nx_module_fallback.said);
    free(cache);
    free(output);
    free(descriptor_ir);
    free(ir);
    free(kept);
    free(left);
    free(module);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        printf("usage: %s FOLDER\n", argv[0]);
        return 2;
    }
    if (!snx_native_available()) {
        printf("SKIP: no native back end on this host: nothing it makes to compare\n");
        return 77;
    }
    if (snx_mkdirs(argv[1])) {
        printf("FAIL: cannot make %s\n", argv[1]);
        return 1;
    }
    snx_x64_cache_ceiling(0); /* each unit made, none taken from an earlier build's */
    snx_error_sink(sink);
    program();
    a64(argv[1]);
    g_nx_module_fallback.failures += snx_test_arm_fallback(argv[1]);
    if (!g_nx_module_fallback.failures)
        printf("a game module through the fallback (%s): as it should be\n", snx_fallback_name());
    return g_nx_module_fallback.failures != 0;
}
