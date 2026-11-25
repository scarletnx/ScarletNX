/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What makes the code the native back end leaves (recompiler/fallback.h), as a program's link gives it:
   - LLVM's: a JIT unit's object, whose table's functions run; the functions of a unit the native back end did not make,
     alone in an object; AArch32 and Thumb blocks in one object; and a block of the executor's (exec.c, here, its loads
     counted and refused as the test asks) whose native object does not load, run;
   - none (fallback_none.c, a program that links no LLVM): each refused with an error that names the code asked for, its
     address and first instruction, why the native back end left it and that the build has no LLVM, and nothing else
     said. An executor on a host with no native back end is refused with it too, and the functions all made are none's
     to refuse */
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/fallback.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/runtime/interface.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int refuse; /* the executor's loads to refuse from here on */
    int failures;
    uint64_t svc; /* the executor's SVC's number */
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

#define CHECK(x, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stdout, "FAIL: " __VA_ARGS__);                                                                     \
            fprintf(stdout, "\n");                                                                                     \
            ++g_seam.failures;                                                                                         \
        }                                                                                                              \
    } while (0)

#define CODE    0x10000u
#define EACH    0x40u
#define ADD_ONE 0x91000400u /* ADD X0, X0, #1 */
#define RET     0xD65F03C0u
#define STOP    0u
static const uint32_t k_arm[2] = {0xE3A00007u, 0xEF000000u}; /* mov r0, #7; svc #0 */
static const uint16_t k_thumb[2] = {0x2009u, 0xDF00u};       /* movs r0, #9; svc #0 */

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

/* the A64 functions by LLVM: a JIT unit run, an AOT one's half */
static void made_functions(SnxCodegen* cg, const uint64_t hints[2]) {
    SnxNativeOptions jit = {.jit = 1, .table_name = "unit_table"}, aot = {0};
    const unsigned char half[2] = {1, 0}, whole[2] = {1, 1};
    unsigned char* object = NULL;
    size_t size = 0;
    char error[512] = "";
    CHECK(!snx_fallback_object(cg, 0, 2, &jit, NULL, "unit", '0', &object, &size, error, sizeof error) && object,
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
    CHECK(!snx_fallback_object(cg, 0, 2, &aot, half, "aot", '0', &object, &size, error, sizeof error) && object,
          "the function the native back end left: %s", error);
    if (object)
        CHECK(holds(object, size, "F_10040") && !holds(object, size, "F_10000"), "the object is of the one left alone");
    free(object);
    object = NULL;
    size = 1;
    CHECK(!snx_fallback_object(cg, 0, 2, &aot, whole, "aot", '0', &object, &size, error, sizeof error) && !object &&
              !size,
          "a unit made whole leaves nothing to make: %s", error);
    CHECK(snx_fallback_object(cg, 0, 2, &jit, half, "unit", '0', &object, &size, error, sizeof error) < 0 && !object &&
              strstr(error, "a JIT unit's functions are one object's, and 1 of codegen's 2 are asked for"),
          "a JIT unit half made: %s", error);
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
        CHECK(snx_fallback_object(cg, 0, 2, &jit, k_asked[i].made, "unit", '0', &object, &size, error, sizeof error) <
                      0 &&
                  !object && !size,
              "functions left to none are made");
        for (size_t k = 0; k < 6 && k_asked[i].want[k]; ++k)
            CHECK(strstr(error, k_asked[i].want[k]), "the error lacks \"%s\": %s", k_asked[i].want[k], error);
        CHECK(strstr(error, snx_native_name()), "the error names the native back end (%s): %s", snx_native_name(),
              error);
    }
    object = &dummy;
    CHECK(!snx_fallback_object(cg, 0, 2, &jit, whole, "unit", '0', &object, &size, error, sizeof error) && !object,
          "a unit made whole is refused: %s", error);
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
    snx_fallback_arm_free(f);
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
    static unsigned char mem[0x3000];
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
    cpu.mem_size = sizeof mem;
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

int main(void) {
    static unsigned char image[2 * EACH];
    uint64_t hints[2];
    SnxCodegen cg;
    if (functions(&cg, image, sizeof image, hints)) {
        printf("FAIL: the program's functions\n");
        return 1;
    }
    CHECK(!strcmp(snx_fallback_name(), snx_fallback_available() ? "LLVM" : "none"), "the fallback's name is %s",
          snx_fallback_name());
    if (snx_fallback_available()) {
        made_functions(&cg, hints);
        made_blocks();
    } else
        refused_code(&cg);
    executor();
    snx_codegen_free(&cg);
    if (!g_seam.failures) printf("the fallback (%s): as it should be\n", snx_fallback_name());
    return g_seam.failures != 0;
}
