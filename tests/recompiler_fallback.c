/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What makes the code the native back end leaves (recompiler/fallback.h), as a program's link gives it:
   - LLVM's: a JIT unit's object, whose table's functions run; the functions of a unit the native back end did not make,
     alone in an object; AArch32 and Thumb blocks in one object, hidden in an AOT module's; a block's length alone,
     blocks chained and an import's stub, and their IR; an AOT module's index of tables, integers and structs, and its
     descriptor, with what lays them out otherwise than C; and a block of the executor's (exec.c, here, its loads
     counted and refused as the test asks) whose native object does not load, run;
   - either: the JIT (jit_service.c, linked beside it), which the native back end alone makes the code of, compiling a
     function and loading it again from its cache, whose key is the native back end's; refused on a host with none;
   - none (fallback_none.c, a program that links no LLVM): each refused with an error that names the code or the global
     asked for, its address and first instruction, why the native back end left it and that the build has no LLVM, and
     nothing else said. An executor on a host with no native back end is refused with it too, the functions all made are
     none's to refuse, and the AOT compilers' maker of blocks (cpu/arm/aot.c, here) has the native back end's alone */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/recompiler/jit/jit_cache.h"
#include "common/cpu/recompiler/jit/jit_service.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/runtime/interface.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE    0x10000u
#define EACH    0x40u
#define ADD_ONE 0x91000400u /* ADD X0, X0, #1 */
#define RET     0xD65F03C0u
#define STOP    0u

static struct {
    int refuse; /* the executor's loads to refuse from here on */
    int failures;
    uint64_t svc;                               /* the executor's SVC's number */
    unsigned char image[2 * EACH], mem[0x3000]; /* the A64 functions' code; the executor's guest memory */
    _Atomic GuestFn dispatch[2 * EACH / 4];     /* the JIT's table of the functions' code */
} g_seam;
static int seam_load(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                     size_t name_count, SnxU64* addresses, char* error, size_t error_size) {
    if (g_seam.refuse > 0) {
        --g_seam.refuse;
        snprintf(error, error_size, "refused by the test");
        return -1;
    }
    return snx_code_load(loader, inputs, count, names, name_count, addresses, error, error_size);
}
#define snx_code_load seam_load
#include "common/cpu/arm/exec.c"
#undef snx_code_load
#include "common/cpu/arm/aot.c"

#define CHECK(x, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stdout, "FAIL: " __VA_ARGS__);                                                                     \
            fprintf(stdout, "\n");                                                                                     \
            ++g_seam.failures;                                                                                         \
        }                                                                                                              \
    } while (0)

static const uint32_t k_arm[2] = {0xE3A00007u, 0xEF000000u}; /* mov r0, #7; svc #0 */
static const uint16_t k_thumb[2] = {0x2009u, 0xDF00u};       /* movs r0, #9; svc #0 */
static const uint32_t k_svc = 0xEF000003u;                   /* svc #3: an import's stub's */
/* hosted blocks, as the Vita's, and the chaining they take: the poll and the lookup table past the cpu, and an import's
   function after them */
static const SnxArmCodegenOptions k_hosted = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0,
                                              0};
static const SnxArmChainedOptions k_chained = {
    .options = {SNX_ARM_CHAINED_OPTIONS_ABI, SNX_ARM_PROFILE_A9, SNX_ARM_MEMORY_HOSTED, 0, 0},
    .poll = sizeof(SnxArmCPU),
    .lookup = sizeof(SnxArmCPU) + 8};

/* the bytes hold the name, a symbol's of one of the three object formats */
static int holds(const unsigned char* bytes, size_t size, const char* name) {
    const size_t n = strlen(name);
    for (size_t i = 0; i + n <= size; ++i)
        if (!memcmp(bytes + i, name, n)) return 1;
    return 0;
}

/* generated code has no type signature before its entry, which the function sanitizer reads (exec.c's run_block) */
[[clang::no_sanitize("function")]] static void call(GuestFn function, CPU* c, uint64_t pc) { function(c, pc); }

static RtAPI g_rt = {.cas128 = snx_runtime_cas128};

/* two functions, each adding one to X0 */
static int functions(SnxCodegen* cg, unsigned char* image, size_t size, uint64_t hints[2]) {
    SnxCodeImage view = {0};
    for (unsigned f = 0; f < 2; ++f) {
        const uint32_t words[2] = {ADD_ONE, RET};
        hints[f] = CODE + f * EACH;
        for (unsigned k = 0; k < EACH; ++k) image[f * EACH + k] = (unsigned char)(words[k < 4 ? 0 : 1] >> (k % 4 * 8));
    }
    view.image = image;
    view.image_size = view.text_size = size;
    view.base = CODE;
    return snx_codegen_analyze(cg, &view, 1, hints, 2, 1, 0) || cg->function_count != 2;
}

/* an AOT module's unit as LLVM makes it: the functions after those they call, one in the --layout order's section, its IR
   kept, another machine's triple asked for, and the runtime library */
static void made_unit(SnxCodegen* cg) {
    SnxNativeOptions aot = {.stale = 1};
    const size_t rank[2] = {SIZE_MAX, 0};
#if defined(_WIN32) || defined(__APPLE__)
    const int elf = 0; /* --layout's sections are an ELF module's */
#else
    const int elf = 1;
#endif
    SnxFallbackUnit unit = {.keep_ir = ".", .rank = elf ? rank : NULL, .callees_first = 1, .stats = (char*)rank};
    unsigned char* object = NULL;
    size_t size = 0;
    char error[512] = "", text[64] = "";
    CHECK(!snx_fallback_object(cg, 0, 2, &aot, NULL, &unit, "aot_unit", '0', &object, &size, error, sizeof error) &&
              object && holds(object, size, "F_10000") && holds(object, size, "F_10040"),
          "a unit of both functions: %s", error);
    CHECK(!elf || (object && holds(object, size, ".text.snx_hot.0")),
          "the second function is not in the order's section");
    CHECK(!unit.stats, "IR counts nobody asked for (ir_stats is off)");
    free(object);
    FILE* f = fopen("aot_unit.ll", "r");
    size_t n = f ? fread(text, 1, sizeof text - 1, f) : 0;
    if (f) fclose(f);
    CHECK(n && strstr(text, "aot_unit"), "the unit's IR is not kept in aot_unit.ll: %s", text);
    remove("aot_unit.ll");
    /* another machine's module: its triple is the target asked of LLVM, which has none of this name */
    SnxFallbackUnit other = {.triple = "nosuchmachine-unknown-none"};
    object = NULL;
    CHECK(snx_fallback_object(cg, 0, 2, &aot, NULL, &other, "other", '0', &object, &size, error, sizeof error) < 0 &&
              !object && strstr(error, "LLVM target nosuchmachine-unknown-none"),
          "a module for another machine's triple: %s", error);
    object = NULL;
    CHECK(!snx_fallback_rtlib('2', &object, &size, error, sizeof error) && object && holds(object, size, "roundf"),
          "the runtime library at -O2: %s", error);
    free(object);
}

/* the A64 functions by LLVM: a JIT unit run, an AOT one's half */
static void made_functions(SnxCodegen* cg, const uint64_t hints[2]) {
    SnxNativeOptions jit = {.jit = 1, .table_name = "unit_table"}, aot = {0};
    const unsigned char half[2] = {1, 0}, whole[2] = {1, 1};
    unsigned char* object = NULL;
    size_t size = 0;
    char error[512] = "";
    CHECK(!snx_fallback_object(cg, 0, 2, &jit, NULL, NULL, "unit", '0', &object, &size, error, sizeof error) && object,
          "a JIT unit's object: %s", error);
    SnxCodeLoader* loader = snx_code_loader_new(0, error, sizeof error);
    SnxLinkInput rtlib = {
        .data = (unsigned char*)snx_rtlib_object, .size = (size_t)snx_rtlib_object_size, .name = "rtlib"};
    SnxLinkInput unit = {.data = object, .size = size, .name = "unit"};
    const char* names[1] = {"unit_table"};
    SnxU64 table = 0;
    if (!loader || snx_code_load(loader, &rtlib, 1, NULL, 0, NULL, error, sizeof error) ||
        snx_code_load(loader, &unit, 1, names, 1, &table, error, sizeof error))
        CHECK(0, "the unit's object loads: %s", error);
    else
        for (unsigned f = 0; f < 2; ++f) {
            GuestFn function;
            CPU c = {0};
            memcpy(&function, (const unsigned char*)(uintptr_t)table + f * sizeof function, sizeof function);
            c.rt = &g_rt;
            c.x[0] = 41;
            c.x[30] = STOP;
            c.pc = hints[f];
            call(function, &c, hints[f]);
            CHECK(c.x[0] == 42 && c.pc == STOP, "function %u of the unit gives %llu at 0x%llx", f,
                  (unsigned long long)c.x[0], (unsigned long long)c.pc);
        }
    snx_code_loader_free(loader);
    free(object);
    object = NULL;
    CHECK(!snx_fallback_object(cg, 0, 2, &aot, half, NULL, "aot", '0', &object, &size, error, sizeof error) && object,
          "the function the native back end left: %s", error);
    if (object)
        CHECK(holds(object, size, "F_10040") && !holds(object, size, "F_10000"), "the object is of the one left alone");
    free(object);
    object = NULL;
    size = 1;
    CHECK(!snx_fallback_object(cg, 0, 2, &aot, whole, NULL, "aot", '0', &object, &size, error, sizeof error) &&
              !object && !size,
          "a unit made whole leaves nothing to make: %s", error);
    CHECK(snx_fallback_object(cg, 0, 2, &jit, half, NULL, "unit", '0', &object, &size, error, sizeof error) < 0 &&
              !object && strstr(error, "a JIT unit's functions are one object's, and 1 of codegen's 2 are asked for"),
          "a JIT unit half made: %s", error);
    made_unit(cg);
}
/* AArch32 and Thumb blocks by LLVM, one object */
static void made_blocks(void) {
    unsigned char* object = NULL;
    size_t size = 0, took = 0;
    char error[512] = "";
    SnxFallbackArm* f = snx_fallback_arm_new("blocks", '0', 1);
    if (!f) {
        CHECK(0, "out of memory");
        return;
    }
    CHECK(snx_fallback_arm_object(f, &object, &size, error, sizeof error) < 0 && strstr(error, "no block was made"),
          "an object of no block: %s", error);
    CHECK(!snx_fallback_arm_block(f, "arm_block", k_arm, 2, 0x1000, 0, NULL, "", &took, error, sizeof error) &&
              took == 2,
          "an ARM block of %zu words: %s", took, error);
    CHECK(snx_fallback_arm_block(f, "none", k_arm, 0, 0x1000, 0, NULL, "", &took, error, sizeof error) < 0 && !took &&
              strstr(error, "LLVM takes no such block"),
          "a block of no words: %s", error);
    CHECK(!snx_fallback_arm_block(f, "thumb_block", k_thumb, 2, 0x2000, 1, NULL, "", &took, error, sizeof error) &&
              took == 2,
          "a Thumb block of %zu halfwords: %s", took, error);
    CHECK(!snx_fallback_arm_object(f, &object, &size, error, sizeof error) && object &&
              holds(object, size, "arm_block") && holds(object, size, "thumb_block"),
          "the object of both: %s", error);
    free(object);
    object = NULL;
    CHECK(snx_fallback_arm_object(f, &object, &size, error, sizeof error) < 0 && !object, "its blocks are taken");
    snx_fallback_arm_free(f);
}

/* the IR's text, or NULL with the failure counted */
static char* arm_text(SnxFallbackArm* f) {
    char error[512] = "", *text = NULL;
    CHECK(!snx_fallback_arm_ir(f, &text, error, sizeof error) && text, "the blocks' IR: %s", error);
    return text;
}
/* AArch32 blocks by LLVM as an AOT module has them, hidden there, and as a JIT's object has them, not; their IR */
static void made_hidden(void) {
    size_t took = 0;
    char error[512] = "";
    SnxFallbackArm *aot = snx_fallback_arm_new("aot", '1', 0), *jit = snx_fallback_arm_new("jit", '0', 1);
    if (!aot || !jit) {
        CHECK(0, "out of memory");
        snx_fallback_arm_free(aot);
        snx_fallback_arm_free(jit);
        return;
    }
    CHECK(!snx_fallback_arm_block(aot, "aot_block", k_arm, 2, 0x1000, 0, NULL, "", &took, error, sizeof error) &&
              !snx_fallback_arm_block(jit, "jit_block", k_arm, 2, 0x1000, 0, NULL, "", &took, error, sizeof error),
          "the blocks: %s", error);
    char *aot_text = arm_text(aot), *jit_text = arm_text(jit);
    CHECK(aot_text && strstr(aot_text, "; ModuleID = 'aot'") && strstr(aot_text, "define hidden void @aot_block("),
          "an AOT module's block is hidden: %s", aot_text ? aot_text : "");
    CHECK(jit_text && strstr(jit_text, "define void @jit_block(") && !strstr(jit_text, "hidden"),
          "a JIT's block is not hidden: %s", jit_text ? jit_text : "");
    free(aot_text);
    free(jit_text);
    snx_fallback_arm_free(aot);
    snx_fallback_arm_free(jit);
}
/* a block's length by LLVM, nothing made of it; blocks chained to their successors, one through the lookup, and an
   import's stub; what chaining refuses */
static void made_chained(void) {
    unsigned char* object = NULL;
    size_t size = 0, took = 0;
    char error[512] = "";
    SnxFallbackArm* f = snx_fallback_arm_new("chained", '1', 0);
    if (!f) {
        CHECK(0, "out of memory");
        return;
    }
    CHECK(!snx_fallback_arm_length(f, k_arm, 2, 0x1000, 0, &k_hosted, "", &took, error, sizeof error) && took == 2,
          "an ARM block's length, %zu words: %s", took, error);
    CHECK(!snx_fallback_arm_length(f, k_thumb, 2, 0x2000, 1, &k_hosted, "", &took, error, sizeof error) && took == 2,
          "a Thumb block's length, %zu halfwords: %s", took, error);
    CHECK(snx_fallback_arm_length(f, k_arm, 0, 0x1000, 0, &k_hosted, "", &took, error, sizeof error) < 0 && !took &&
              strstr(error, "LLVM takes no such block"),
          "the length of a block of no words: %s", error);
    CHECK(snx_fallback_arm_object(f, &object, &size, error, sizeof error) < 0 && !object &&
              strstr(error, "no block was made"),
          "lengths make no object: %s", error);
    const uint32_t next[2] = {0x2001, 0x1000}, after = 0x3004;
    const char *const names[2] = {"thumb_next", "a_back"}, *const stub_next[1] = {"after_stub"};
    CHECK(!snx_fallback_arm_block(f, "a_back", k_arm, 2, 0x1000, 0, &k_hosted, "", &took, error, sizeof error) &&
              !snx_fallback_arm_block(f, "a_stub", &k_svc, 1, 0x3000, 0, &k_hosted, "", &took, error, sizeof error),
          "the blocks to chain: %s", error);
    CHECK(!snx_fallback_arm_chain(f, "a_back", 0x1000, &k_chained, next, names, 2, error, sizeof error),
          "a block chained: %s", error);
    SnxArmChainedOptions stub = k_chained;
    stub.direct = sizeof(SnxArmCPU) + 16, stub.import = 3;
    CHECK(!snx_fallback_arm_chain(f, "a_stub", 0x3000, &stub, &after, stub_next, 1, error, sizeof error),
          "a stub chained: %s", error);
    CHECK(snx_fallback_arm_chain(f, "a_stub", 0x3000, &stub, next, names, 2, error, sizeof error) < 0 &&
              strstr(error, "chained: the stub block a_stub goes on to 2 blocks, of 1 at most"),
          "a stub that goes on to two: %s", error);
    SnxArmChainedOptions bad = k_chained;
    bad.poll = 0; /* a lookup with no poll */
    CHECK(snx_fallback_arm_chain(f, "a_back", 0x1000, &bad, next, names, 2, error, sizeof error) < 0 &&
              strstr(error, "chained: the block a_back: chaining options no block compiler takes"),
          "chaining options of a lookup with no poll: %s", error);
    CHECK(snx_fallback_arm_chain(f, "thumb_next", 0x2001, &k_chained, next, names, 0, error, sizeof error) < 0 &&
              strstr(error, "chained: the block thumb_next to chain is none of the object's"),
          "a block declared, not made: %s", error);
    char* text = arm_text(f);
    static const char* const k_said[] = {"define hidden void @a_back(",
                                         "define hidden void @a_stub(",
                                         "declare hidden void @thumb_next(ptr noalias)",
                                         "musttail call void @a_back(",
                                         "musttail call void @thumb_next(",
                                         "musttail call void @after_stub(",
                                         "declare hidden void @after_stub(ptr noalias)",
                                         "probe:"};
    for (size_t k = 0; text && k < sizeof k_said / sizeof *k_said; ++k)
        CHECK(strstr(text, k_said[k]), "the chained blocks' IR lacks \"%s\"", k_said[k]);
    free(text);
    CHECK(!snx_fallback_arm_object(f, &object, &size, error, sizeof error) && object && holds(object, size, "a_back") &&
              holds(object, size, "a_stub"),
          "the chained blocks' object: %s", error);
    free(object);
    snx_fallback_arm_free(f);
}

/* ---- an AOT module's index ---- */

typedef struct {
    uint32_t a;
    uint64_t b;
    const void* p;
} DataPair;
typedef struct {
    uint64_t magic;
    unsigned char id[4];
    const DataPair* pairs;
} DataDescriptor;
static const unsigned char k_id[4] = {1, 2, 3, 0xF0};
static const SnxFallbackField k_pairs[6] = {{.bits = 32, .value = 1, .offset = offsetof(DataPair, a)},
                                            {.bits = 64, .value = 2, .offset = offsetof(DataPair, b)},
                                            {.global = "keys", .offset = offsetof(DataPair, p)},
                                            {.bits = 32, .value = 3, .offset = offsetof(DataPair, a)},
                                            {.bits = 64, .value = 4, .offset = offsetof(DataPair, b)},
                                            {.offset = offsetof(DataPair, p)}};
static const SnxFallbackField k_descriptor[3] = {
    {.bits = 64, .value = 0x1122, .offset = offsetof(DataDescriptor, magic)},
    {.bytes = k_id, .count = sizeof k_id, .offset = offsetof(DataDescriptor, id)},
    {.global = "pairs", .offset = offsetof(DataDescriptor, pairs)}};
static const uint32_t k_keys[2] = {7, 9};
/* every slot a block but the second */
static int data_name(void* user, size_t i, char* out, size_t size) {
    (void)user;
    if (i == 1) return 0;
    snprintf(out, size, "block_%zu", i);
    return 1;
}
/* the index by LLVM: its globals in the IR as asked, its object exporting the descriptor, and what it refuses */
static void made_data(void) {
    char error[512] = "", *text = NULL;
    unsigned char* object = NULL;
    size_t size = 0;
    SnxFallbackData* d = snx_fallback_data_new("unit", "unit-index", '1', "no native back end on this host");
    if (!d) {
        CHECK(0, "out of memory");
        return;
    }
    CHECK(!snx_fallback_data_blocks(d, "table", 3, data_name, NULL, error, sizeof error), "the table: %s", error);
    CHECK(!snx_fallback_data_blocks(d, "again", 1, data_name, NULL, error, sizeof error), "a second: %s", error);
    CHECK(!snx_fallback_data_integers(d, "keys", 32, 2, k_keys, error, sizeof error), "the keys: %s", error);
    CHECK(!snx_fallback_data_structs(d, "pairs", 2, k_pairs, 3, sizeof(DataPair), error, sizeof error),
          "the structs: %s", error);
    CHECK(!snx_fallback_data_structs(d, "empty", 0, k_pairs, 3, sizeof(DataPair), error, sizeof error),
          "no structs: %s", error);
    CHECK(!snx_fallback_data_descriptor(d, "desc", k_descriptor, 3, sizeof(DataDescriptor), error, sizeof error),
          "the descriptor: %s", error);
    CHECK(snx_fallback_data_integers(d, "odd", 16, 2, k_keys, error, sizeof error) < 0 &&
              strstr(error, "unit: odd of integers of 16 bits, which are 8, 32 or 64"),
          "integers of 16 bits: %s", error);
    SnxFallbackField lost[3] = {k_descriptor[0], k_descriptor[1], k_descriptor[2]};
    lost[2].global = "missing";
    CHECK(snx_fallback_data_descriptor(d, "lost", lost, 3, sizeof(DataDescriptor), error, sizeof error) < 0 &&
              strstr(error, "unit: lost points to missing, which no global before it is"),
          "a pointer to no global: %s", error);
    lost[2].global = "pairs", lost[2].offset += 8;
    CHECK(snx_fallback_data_descriptor(d, "moved", lost, 3, sizeof(DataDescriptor), error, sizeof error) < 0 &&
              strstr(error, "unit: moved is laid out otherwise for the target LLVM makes it for") &&
              strstr(error, "field 2 at 16) than in C (24 bytes, field 2 at 24)"),
          "a field C has elsewhere: %s", error);
    CHECK(!snx_fallback_data_ir(d, &text, error, sizeof error) && text, "the index's IR: %s", error);
    static const char* const k_said[] = {
        "; ModuleID = 'unit-index'",
        "@table = internal constant [3 x ptr] [ptr @block_0, ptr null, ptr @block_2]",
        "@again = internal constant [1 x ptr] [ptr @block_0]",
        "@keys = internal constant [2 x i32] [i32 7, i32 9]",
        ("@pairs = internal constant [2 x { i32, i64, ptr }] [{ i32, i64, ptr } { i32 1, i64 2, ptr @keys }, { i32, i64, "
         "ptr } { i32 3, i64 4, ptr null }]"),
        "@empty = internal constant [0 x { i32, i64, ptr }] zeroinitializer",
        "@desc = constant { i64, [4 x i8], ptr } { i64 4386, [4 x i8] c\"\\01\\02\\03\\F0\", ptr @pairs }",
        "declare hidden void @block_0(ptr)",
        "declare hidden void @block_2(ptr)"};
    for (size_t k = 0; text && k < sizeof k_said / sizeof *k_said; ++k)
        CHECK(strstr(text, k_said[k]), "the index's IR lacks \"%s\":\n%s", k_said[k], text);
    CHECK(text && !strstr(text, "@lost") && !strstr(text, "@moved") && !strstr(text, "@odd"),
          "what is refused is not made: %s", text ? text : "");
    free(text);
    CHECK(!snx_fallback_data_object(d, &object, &size, error, sizeof error) && object && holds(object, size, "desc"),
          "the index's object: %s", error);
    free(object);
    snx_fallback_data_free(d);
}

/* the same asked of none: what is refused, and how it is said */
static void refused_code(SnxCodegen* cg) {
    static const struct {
        unsigned char made[2];
        const char* want[6];
    } k_asked[] = {
        {{0, 0},
         {"unit: the function at 0x10000, which starts with ADD", "(91000400), and 1 more of the unit's functions",
          "the native back end", "left it", "SNX_WITH_LLVM=OFF"}},
        {{1, 0},
         {"unit: the function at 0x10040, which starts with ADD", "(91000400): the native back end",
          "SNX_WITH_LLVM=OFF"}},
    };
    SnxNativeOptions jit = {.jit = 1, .table_name = "unit_table"};
    const unsigned char whole[2] = {1, 1};
    unsigned char dummy = 0, *object = &dummy;
    size_t size = 1, took = 1;
    char error[512] = "";
    const char* why = "its name is too long";
    for (size_t i = 0; i < sizeof k_asked / sizeof *k_asked; ++i) {
        object = &dummy;
        size = 1;
        CHECK(snx_fallback_object(cg, 0, 2, &jit, k_asked[i].made, NULL, "unit", '0', &object, &size, error,
                                  sizeof error) < 0 &&
                  !object && !size,
              "functions left to none are made");
        for (size_t k = 0; k < 6 && k_asked[i].want[k]; ++k)
            CHECK(strstr(error, k_asked[i].want[k]), "the error lacks \"%s\": %s", k_asked[i].want[k], error);
        CHECK(strstr(error, snx_native_name()), "the error names the native back end (%s): %s", snx_native_name(),
              error);
    }
    object = &dummy;
    CHECK(!snx_fallback_object(cg, 0, 2, &jit, whole, NULL, "unit", '0', &object, &size, error, sizeof error) &&
              !object,
          "a unit made whole is refused: %s", error);
    SnxFallbackUnit unit = {.keep_ir = ".", .stats = (char*)&dummy};
    object = &dummy;
    CHECK(snx_fallback_object(cg, 0, 2, &jit, NULL, &unit, "kept", '0', &object, &size, error, sizeof error) < 0 &&
              !object && !unit.stats && strstr(error, "kept: the function at 0x10000"),
          "a unit with its IR kept: %s", error);
    object = &dummy;
    CHECK(snx_fallback_rtlib('2', &object, &size, error, sizeof error) < 0 && !object && !size &&
              strstr(error, "the runtime library at -O2") && strstr(error, "SNX_WITH_LLVM=OFF"),
          "the runtime library: %s", error);
    CHECK(!snx_fallback_dump_pc() && !snx_fallback_stats(), "none asks for a function's IR or a unit's counts");
    SnxFallbackArm* f = snx_fallback_arm_new("a32exec", '0', 1);
    if (!f) {
        CHECK(0, "out of memory");
        return;
    }
    CHECK(snx_fallback_arm_start(f, error, sizeof error) < 0 && strstr(error, "SNX_WITH_LLVM=OFF") &&
              strstr(error, "a32exec") && strstr(error, snx_native_name()),
          "it starts: %s", error);
    CHECK(snx_fallback_arm_block(f, "b", k_arm, 2, 0x1000, 0, NULL, why, &took, error, sizeof error) < 0 && !took &&
              strstr(error,
                     "a32exec: the ARM block at 0x00001000, which starts with e3a00007: its name is too long, and") &&
              strstr(error, "SNX_WITH_LLVM=OFF"),
          "an ARM block: %s", error);
    CHECK(
        snx_fallback_arm_block(f, "b", k_thumb, 2, 0x2000, 1, NULL, why, &took, error, sizeof error) < 0 && !took &&
            strstr(error, "a32exec: the Thumb block at 0x00002000, which starts with 2009: its name is too long, and"),
        "a Thumb block: %s", error);
    CHECK(snx_fallback_arm_block(f, "b", k_arm, 0, 0x1000, 0, NULL, why, &took, error, sizeof error) < 0 &&
              strstr(error, "the ARM block at 0x00001000, of no instructions: its name is too long"),
          "a block of no words: %s", error);
    CHECK(snx_fallback_arm_object(f, &object, &size, error, sizeof error) < 0 && !object && !size &&
              strstr(error, "no block was made"),
          "an object: %s", error);
    CHECK(snx_fallback_arm_length(f, k_arm, 2, 0x1000, 0, &k_hosted, why, &took, error, sizeof error) < 0 && !took &&
              strstr(error, "a32exec: the ARM block at 0x00001000, which starts with e3a00007: its name is too long"),
          "an ARM block's length: %s", error);
    CHECK(snx_fallback_arm_chain(f, "b", 0x1000, &k_chained, NULL, NULL, 0, error, sizeof error) < 0 &&
              strstr(error, "a32exec: the block b to chain was not made, and this build has no LLVM"),
          "a block chained: %s", error);
    char* text = (char*)&dummy;
    CHECK(snx_fallback_arm_ir(f, &text, error, sizeof error) < 0 && !text && strstr(error, "no block was made"),
          "the blocks' IR: %s", error);
    snx_fallback_arm_free(f);
}
/* the index asked of none: each global refused, with the module's name and why it was asked */
static void refused_data(void) {
    unsigned char dummy = 0, *object = &dummy;
    char error[512] = "", *text = (char*)&dummy;
    size_t size = 1;
    SnxFallbackData* d = snx_fallback_data_new("unit", "unit-index", '1', "no native back end on this host");
    if (!d) {
        CHECK(0, "out of memory");
        return;
    }
    static const char k_index[] = " of the index unit-index: no native back end on this host, and this build has no "
                                  "LLVM to make it (SNX_WITH_LLVM=OFF)";
    CHECK(snx_fallback_data_blocks(d, "table", 3, data_name, NULL, error, sizeof error) < 0 &&
              strstr(error, "unit: table") && strstr(error, k_index),
          "a table: %s", error);
    CHECK(snx_fallback_data_integers(d, "keys", 32, 2, k_keys, error, sizeof error) < 0 &&
              strstr(error, "unit: keys") && strstr(error, k_index),
          "integers: %s", error);
    CHECK(snx_fallback_data_structs(d, "pairs", 2, k_pairs, 3, sizeof(DataPair), error, sizeof error) < 0 &&
              strstr(error, "unit: pairs") && strstr(error, k_index),
          "structs: %s", error);
    CHECK(snx_fallback_data_descriptor(d, "desc", k_descriptor, 3, sizeof(DataDescriptor), error, sizeof error) < 0 &&
              strstr(error, "unit: desc") && strstr(error, k_index),
          "a descriptor: %s", error);
    CHECK(snx_fallback_data_ir(d, &text, error, sizeof error) < 0 && !text && strstr(error, "unit: the IR") &&
              strstr(error, k_index),
          "the index's IR: %s", error);
    CHECK(snx_fallback_data_object(d, &object, &size, error, sizeof error) < 0 && !object &&
              strstr(error, "unit: the object") && strstr(error, k_index),
          "the index's object: %s", error);
    snx_fallback_data_free(d);
}

/* the AOT compilers' blocks (aot.c, above) with none: the native back end's made, the others refused by name, with
   why the native back end left them, and the IR asked for of none */
static void refused_aot(void) {
    static const uint32_t words[3] = {0xE2800001u, 0xE0800181u, 0xEF000007u}; /* add, add, svc #7 */
    unsigned char code[12];
    char error[512] = "";
    size_t consumed = 0;
    for (size_t i = 0; i < 12; ++i) code[i] = (unsigned char)(words[i / 4] >> (i % 4 * 8));
    SnxArmAot *a = snx_arm_aot_new('1', NULL), *kept = snx_arm_aot_new('1', ".");
    if (!a || !kept) {
        CHECK(0, "out of memory");
        snx_arm_aot_free(a);
        snx_arm_aot_free(kept);
        return;
    }
    const SnxArmCodegenOptions* plain = &k_hosted;
    if (snx_native_arm_available()) {
        CHECK(!snx_arm_aot_block(a, "plain", code, 3, 0x1000, 0, plain, &consumed, error, sizeof error) &&
                  consumed == 3,
              "the native back end's block: %s", error);
        CHECK(snx_arm_aot_block(a, "a_block_of_a_name_too_long", code, 3, 0x2000, 0, plain, &consumed, error,
                                sizeof error) < 0 &&
                  strstr(error, "arm-aot: the ARM block at 0x00002000, which starts with e2800001: a block's name too "
                                "long, and this build has no LLVM to make it (SNX_WITH_LLVM=OFF)"),
              "a block the native back end refuses: %s", error);
        SnxLinkInput out[2];
        CHECK(snx_arm_aot_objects(a, "blocks", out, error, sizeof error) == 1 && out[0].data && !out[1].data,
              "the native back end's object alone: %s", error);
        free((void*)out[0].data);
        const SnxArmAotCount* c = snx_arm_aot_count(a);
        CHECK(c->native == 1 && !c->llvm && !c->nwhys, "who made them: %zu native, %zu more", c->native, c->llvm);
    } else
        CHECK(snx_arm_aot_block(a, "plain", code, 3, 0x1000, 0, plain, &consumed, error, sizeof error) < 0 &&
                  strstr(error, "0x00001000, which starts with e2800001: no native back end on this host, and this "
                                "build has no LLVM"),
              "a block on a host with no native back end: %s", error);
    CHECK(snx_arm_aot_block(kept, "kept", code, 3, 0x1000, 0, plain, &consumed, error, sizeof error) < 0 &&
              strstr(error, "arm-aot: the ARM block at 0x00001000, which starts with e2800001: --keep-ir keeps "
                            "LLVM's IR of every block, and this build has no LLVM to make it (SNX_WITH_LLVM=OFF)"),
          "a block of a kept IR: %s", error);
    snx_arm_aot_free(a);
    snx_arm_aot_free(kept);
}

/* ---- the executor (exec.c, above) ---- */

static int on_syscall(void* context, uint32_t imm, uint64_t number, const uint64_t arguments[6]) {
    (void)context;
    (void)number;
    g_seam.svc = 1000 + imm;
    (void)arguments;
    return 1; /* the run ends */
}
/* a block whose native object does not load: the fallback's, run, or refused and said */
static void executor(void) {
    unsigned char* mem = g_seam.mem;
    const char* path = "recompiler-fallback.txt";
    char said[2048] = "", error[512] = "";
    size_t n = 0;
    SnxArmCPU cpu = {0};
    const SnxArmExecRange range = {0x1000, mem + 0x1000, 0x1000};
    for (unsigned k = 0; k < 8; ++k) mem[0x1000 + k] = (unsigned char)(k_arm[k / 4] >> (k % 4 * 8));
    if (!freopen(path, "w", stderr)) {
        CHECK(0, "cannot write %s", path);
        return;
    }
    SnxArmExec* exec = snx_arm_exec_new(error, sizeof error);
    if (!snx_native_arm_available()) {
        /* the fallback makes every block, or there is no executor */
        CHECK(!exec == !snx_fallback_available(), "an executor with no native back end: %s", error);
        CHECK(exec || strstr(error, "SNX_WITH_LLVM=OFF"), "it is refused without saying why: %s", error);
        snx_arm_exec_free(exec);
        return;
    }
    CHECK(exec, "an executor: %s", error);
    if (!exec) return;
    cpu.mem = mem;
    cpu.mem_size = sizeof g_seam.mem;
    cpu.r[15] = 0x1000;
    g_seam.refuse = 1; /* the native object's load */
    const int exit = snx_arm_exec_run(exec, &cpu, &range, 1, on_syscall, NULL);
    const char* why = "";
    CHECK(snx_arm_exec_refused(exec, &why) == 1 && strstr(why, "refused by the test"), "the block's reason: %s", why);
    if (snx_fallback_available())
        CHECK(exit == SNX_ARM_OK && g_seam.svc == 1000 && cpu.r[0] == 7, "LLVM's block gives r0 %u and exit %d",
              (unsigned)cpu.r[0], exit);
    else
        CHECK(exit == SNX_ARM_BAD_ENTRY && !g_seam.svc, "a block nothing makes ends the run: exit %d", exit);
    snx_arm_exec_free(exec);
    fflush(stderr);
    FILE* f = fopen(path, "r");
    n = f ? fread(said, 1, sizeof said - 1, f) : 0;
    if (f) fclose(f);
    said[n] = 0;
    remove(path);
    if (snx_fallback_available()) {
        CHECK(strstr(said, "LLVM makes a block it leaves: its object does not load: refused by the test") &&
                  !strstr(said, "nothing makes a block"),
              "what is said: %s", said);
    } else {
        CHECK(strstr(said,
                     "nothing makes a block: a32exec: the ARM block at 0x00001000, which starts with e3a00007: its "
                     "object does not load: refused by the test, and this build has no LLVM to make it "
                     "(SNX_WITH_LLVM=OFF)") &&
                  !strstr(said, "LLVM makes a block"),
              "what is said: %s", said);
    }
}

/* the fallback's start: LLVM's, or none's where the host has a native back end, which then makes all of the code */
static void started(void) {
    char error[512] = "";
    CHECK(!snx_fallback_start(error, sizeof error) == (snx_fallback_available() || snx_native_available()),
          "the start: %s", error);
}

/* ---- the JIT (jit_service.c, beside) ---- */

/* the functions' JIT: the first compiled, run, and loaded again by a second service from the cache it wrote, under the
   key the build has */
static void jit(void) {
    static const char* const cache = "recompiler-fallback.jitcache";
    char error[512] = "", *key = snx_jit_cache_key('0', 0, 0, 0), want[64], code[64];
    snprintf(want, sizeof want, " native=%s ", snx_native_name());
    snprintf(code, sizeof code, " native_code=%s ", snx_native_code());
    CHECK(key && strstr(key, code), "the cache key names the native code's version (%s): %s", snx_native_code(),
          key ? key : "(none)");
    CHECK(key && strstr(key, want) && strstr(key, " opt=0 hook=0 svc=0 ") && strstr(key, " features=") &&
              !strstr(key, "llvm="),
          "the native back end's cache key: %s", key ? key : "(none)");
    free(key);
    SnxCodeImage image = {
        .image = g_seam.image, .image_size = sizeof g_seam.image, .base = CODE, .text_size = sizeof g_seam.image};
    SnxJitConfig config = {.images = &image,
                           .image_count = 1,
                           .dispatch = g_seam.dispatch,
                           .code_lo = CODE,
                           .code_hi = CODE + sizeof g_seam.image,
                           .threads = 1,
                           .cache_path = cache};
    remove(cache);
    for (int round = 0; round < 2; ++round) {
        for (size_t i = 0; i < sizeof g_seam.dispatch / sizeof *g_seam.dispatch; ++i)
            atomic_store_explicit(&g_seam.dispatch[i], NULL, memory_order_relaxed);
        SnxJitService* s = snx_jit_service_new(&config, error, sizeof error);
        if (!snx_native_available()) {
            CHECK(!s && strstr(error, "this host has no native back end"), "a JIT with no code generator: %s", error);
            snx_jit_service_free(s);
            break;
        }
        CHECK(s, "a JIT: %s", error);
        if (!s) break;
        GuestFn function = snx_jit_wait(s, CODE);
        CHECK(function, "the JIT's code of 0x%x", CODE);
        if (function) {
            CPU c = {0};
            c.rt = &g_rt;
            c.x[0] = 41;
            c.x[30] = STOP;
            c.pc = CODE;
            call(function, &c, CODE);
            CHECK(c.x[0] == 42 && c.pc == STOP, "the JIT's function gives %llu at 0x%llx", (unsigned long long)c.x[0],
                  (unsigned long long)c.pc);
        }
        snx_jit_drain(s);
        SnxJitStats stats;
        snx_jit_stats(s, &stats);
        CHECK(round ? stats.cached >= 1 && !stats.compiled : stats.compiled >= 1 && !stats.cached,
              "round %d: %lu units loaded from the cache, %lu pcs compiled", round, stats.cached, stats.compiled);
        snx_jit_service_free(s);
    }
    remove(cache);
}

int main(void) {
    uint64_t hints[2];
    SnxCodegen cg;
    if (functions(&cg, g_seam.image, sizeof g_seam.image, hints)) {
        printf("FAIL: the program's functions\n");
        return 1;
    }
    CHECK(!strcmp(snx_fallback_name(), snx_fallback_available() ? "LLVM" : "none"), "the fallback's name is %s",
          snx_fallback_name());
    if (snx_fallback_available()) {
        made_functions(&cg, hints);
        made_blocks();
        made_hidden();
        made_chained();
        made_data();
    } else {
        refused_code(&cg);
        refused_data();
        refused_aot();
    }
    started();
    executor();
    jit();
    snx_codegen_free(&cg);
    if (!g_seam.failures) printf("the fallback (%s): as it should be\n", snx_fallback_name());
    return g_seam.failures != 0;
}
