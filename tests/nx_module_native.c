/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A game module of the native back end's code (nx/loader/module_a64.c, module_arm.c). The builders are this program's
   own (module_a64.c here, module_arm.c in nx_module_native_arm.c), so the test chooses what the native back end
   leaves:
   - every function or block the native back end's: the module, nothing said;
   - a function with an instruction it has no code for: made, the instruction undefined where it runs, and said;
   - one it leaves (as one longer than its branches reach is), or a block it leaves: the module refused, naming the
     code, its address, its first instruction and why;
   - a --layout the native back end does not order its functions by yet, and a host with no native back end: refused,
     naming the option and its value; a profile cache's key made, and its missing
     file said
     nx_module_native FOLDER   (where its modules go) */
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/x64/x64_cache.h"
#include "common/utilities/common.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/module_a64.h"
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
    int leave;       /* F1 left by the native back end, as one it can't make whole is */
    int no_native;   /* the host is to have no native back end */
    char said[8192]; /* the errors the builder said since the last build, a line each */
    unsigned char memory[TEXT_OFF + 0x1000];
} g_nx_module_native;

static int test_cached_object(SnxCodegen* codegen, size_t first, size_t last, const SnxNativeOptions* options,
                              unsigned char* made, unsigned char** object, size_t* size, int* reused, char* error,
                              size_t error_size) {
    SnxNativeOptions asked = *options;
    asked.without = &g_nx_module_native.without;
    asked.without_count = g_nx_module_native.without != 0;
    int rc = snx_native_cached_object(codegen, first, last, &asked, made, object, size, reused, error, error_size);
    for (size_t k = first; !rc && g_nx_module_native.leave && k < last; ++k)
        if (codegen->functions[k].start == F1) made[k - first] = 0;
    return rc;
}
static int test_native_available(void) { return !g_nx_module_native.no_native && snx_native_available(); }
#define snx_native_cached_object test_cached_object
#define snx_native_available     test_native_available
#include "nx/loader/module_a64.c"
#undef snx_native_cached_object
#undef snx_native_available

/* nx_module_native_arm.c's: the same of a 32-bit program, its failures counted */
int snx_test_arm_native(const char* folder);

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            ++g_nx_module_native.failures;                                                                           \
        }                                                                                                              \
    } while (0)

static void sink(const char* line) {
    size_t n = strlen(g_nx_module_native.said);
    snprintf(g_nx_module_native.said + n, sizeof g_nx_module_native.said - n, "%s\n", line);
}

/* f0: x9 = x30; bl f1; x30 = x9; ret. f1: x0 += 1; b f2. f2: x0 += 1; ret. Each function a unit */
static void program(void) {
    static const SnxU32 code[] = {0xAA1E03E9u, 0x94000003u, 0xAA0903FEu, 0xD65F03C0u,
                                  ADD_ONE,     0x14000001u, ADD_ONE,     0xD65F03C0u};
    for (size_t i = 0; i < sizeof code / sizeof *code; ++i)
        for (size_t j = 0; j < 4; ++j)
            g_nx_module_native.memory[TEXT_OFF + 4 * i + j] = (unsigned char)(code[i] >> (8 * j));
}

/* the program's module as output, with what job and options add (their output, title and the like set here): 0, or
   the builder's failure, which it said (g_nx_module_native.said) */
static int build(const char* output, SnxModuleBuild job, SnxCodegenOptions options) {
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    image.name = "test";
    image.base = SNX_CODE_BASE;
    image.size = sizeof g_nx_module_native.memory;
    image.image = g_nx_module_native.memory;
    image.image_size = sizeof g_nx_module_native.memory;
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
    job.output = output;
    job.opt = '2';
    job.jobs = 1;
    job.quiet = 1;
    g_nx_module_native.said[0] = 0;
    return snx_build_module_a64(&loaded, &options, &job);
}

/* where a file of folder's is: malloc'd */
static char* at(const char* folder, const char* name) { return snx_path_join(folder, name); }

static void a64(const char* folder) {
    SnxModuleBuild asked = {0};
    SnxCodegenOptions options = {0};
    char *module = at(folder, "a64" SUFFIX), *left = at(folder, "left" SUFFIX), wanted[512];
    /* every function the native back end's: the module, nothing said */
    CHECK(!build(module, asked, options) && snx_is_file(module) && !g_nx_module_native.said[0],
          "the module of the native back end's functions: %s", g_nx_module_native.said);
    /* an instruction it has no code for: f1's and f2's ADD */
    g_nx_module_native.without = ADD_ONE;
    int rc = build(left, asked, options);
    CHECK(!rc && snx_is_file(left) && strstr(g_nx_module_native.said, "it raises an undefined instruction where it runs") &&
              strstr(g_nx_module_native.said, "native back end: 2 of 3 functions have an instruction it has no code "
                                              "for: each made, the instruction undefined where it runs") &&
              !strstr(g_nx_module_native.said, "LLVM"),
          "the native back end makes them, the ADD undefined: %s", g_nx_module_native.said);
    /* a function it leaves otherwise */
    g_nx_module_native.leave = 1;
    free(left);
    left = at(folder, "refused" SUFFIX);
    rc = build(left, asked, options);
    g_nx_module_native.leave = 0;
    g_nx_module_native.without = 0;
    snprintf(wanted, sizeof wanted, "code_001: the function at 0x%llx, which starts with ADD", (unsigned long long)F1);
    char made[96];
    snprintf(made, sizeof made, "(91000400): the native back end (%s) did not make it\n", snx_native_name());
    CHECK(rc && !snx_is_file(left) && strstr(g_nx_module_native.said, wanted) && strstr(g_nx_module_native.said, made),
          "a function the native back end left refuses the module, naming it (%s): %s", wanted,
          g_nx_module_native.said);
    /* a --layout */
    const SnxU64 order[1] = {F1};
    asked.layout = order;
    asked.layout_count = 1;
    char* output = at(folder, "layout" SUFFIX);
    rc = build(output, asked, options);
    asked.layout = NULL;
    asked.layout_count = 0;
    CHECK(rc && !strcmp(g_nx_module_native.said,
                        "--layout of 1 addresses: the native back end does not order its functions yet\n"),
          "--layout: %s", g_nx_module_native.said);
    /* a profile cache, which there is none of: its key is made, and the file not opened */
    char* cache = at(folder, "no.jitcache");
    options.jit_cache = cache;
    free(output);
    output = at(folder, "cache" SUFFIX);
    rc = build(output, asked, options);
    options.jit_cache = NULL;
    CHECK(rc && strstr(g_nx_module_native.said, "JIT cache: ") && !strstr(g_nx_module_native.said, "key"),
          "the cache's key is made, its file missing: %s", g_nx_module_native.said);
    /* a host with no native back end */
    g_nx_module_native.no_native = 1;
    free(output);
    output = at(folder, "alone" SUFFIX);
    rc = build(output, asked, options);
    g_nx_module_native.no_native = 0;
    CHECK(rc && !strcmp(g_nx_module_native.said, "this host has no native back end to make the module\n") &&
              !snx_is_file(output),
          "a host with no native back end: %s", g_nx_module_native.said);
    free(cache);
    free(output);
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
    g_nx_module_native.failures += snx_test_arm_native(argv[1]);
    if (!g_nx_module_native.failures) printf("a game module of the native back end's code: as it should be\n");
    return g_nx_module_native.failures != 0;
}
