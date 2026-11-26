/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AArch32 executor where the loader does not take a native block's object: LLVM makes the block, its reason counted
   and said once however many blocks have it, and a block LLVM's object cannot load either is NULL, said too. exec.c is
   here, its loads counted and refused as the test asks. A build that took LLVM 22 (SNX_ACCEPT_LLVM22_MISCOMPILE, #2914)
   says so once as LLVM starts, and only that build */
#include "common/cpu/recompiler/llvm/code_loader.h"
#include <llvm/Config/llvm-config.h>
#include <stdio.h>
static struct {
    int refuse; /* the loads to refuse from here on */
    int failures;
} g_fallback;
static int fallback_load(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                         size_t name_count, SnxU64* addresses, char* error, size_t error_size) {
    if (g_fallback.refuse > 0) {
        --g_fallback.refuse;
        snprintf(error, error_size, "refused by the test");
        return -1;
    }
    return snx_code_load(loader, inputs, count, names, name_count, addresses, error, error_size);
}
#define snx_code_load fallback_load
#include "common/cpu/arm/exec.c"
#undef snx_code_load

#define CHECK(x, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stdout, "FAIL: " __VA_ARGS__);                                                                     \
            fprintf(stdout, "\n");                                                                                     \
            ++g_fallback.failures;                                                                                     \
        }                                                                                                              \
    } while (0)

int main(void) {
    static const uint32_t words[2] = {0xE3A00001u, 0xEF000000u}; /* mov r0, #1; svc #0 */
    const char* path = "arm-exec-fallback.txt";
    char said[2048] = "";
    if (!snx_native_arm_available()) {
        printf("no native back end: there is no object of its to refuse\n");
        return 0;
    }
    if (!freopen(path, "w", stderr)) return 1;
    SnxArmExec* exec = snx_arm_exec_new(NULL, 0);
    if (!exec) return 1;
    size_t took = 0;
    const char* why = "";
    CHECK(snx_arm_exec_compile(exec, words, 2, 0x1000, 0, SNX_ARM_PROFILE_A57, 0, &took) && took == 2,
          "a native block is not made, or takes %zu of 2 words", took);
    CHECK(!snx_arm_exec_refused(exec, &why) && !snx_ir_started(), "a native block is asked of LLVM: %s", why);
    for (int i = 0; i < 3; ++i) {
        took = 0;
        g_fallback.refuse = 1; /* the native object's load */
        CHECK(snx_arm_exec_compile(exec, words, 2, 0x1000, 0, SNX_ARM_PROFILE_A57, 0, &took) && took == 2,
              "a block whose native object does not load is not LLVM's (%zu of 2 words)", took);
        CHECK(snx_arm_exec_refused(exec, &why) == (size_t)i + 1 && strstr(why, "refused by the test"),
              "%zu blocks asked of LLVM (%s), %d wanted", snx_arm_exec_refused(exec, NULL), why, i + 1);
        CHECK(snx_ir_started(), "LLVM did not start for it");
    }
    g_fallback.refuse = 2; /* LLVM's too */
    CHECK(!snx_arm_exec_compile(exec, words, 2, 0x1000, 0, SNX_ARM_PROFILE_A57, 0, &took),
          "a block neither object of which loads is made");
    snx_arm_exec_free(exec);
    fflush(stderr);
    FILE* f = fopen(path, "r");
    size_t n = f ? fread(said, 1, sizeof said - 1, f) : 0;
    if (f) fclose(f);
    said[n] = 0;
    remove(path);
    size_t lines = 0, took22 = LLVM_VERSION_MAJOR == 22; /* the configure refuses 22 without the option */
    for (const char* p = said; *p; ++p) lines += *p == '\n';
    CHECK(lines == 2 + took22 && strstr(said, "LLVM makes a block it leaves: ") &&
              strstr(said, "nothing makes a block: "),
          "%zu lines said, %zu wanted, LLVM's and nobody's:\n%s", lines, 2 + took22, said);
    CHECK(!strstr(said, "#2914") == !took22, "LLVM %s's start %s of #2914:\n%s", LLVM_VERSION_STRING,
          took22 ? "says nothing" : "speaks", said);
    /* the line itself, said or not */
    for (int accepted = 0; accepted < 2; ++accepted) {
        char line[512] = "";
        FILE* out = tmpfile();
        if (!out) return 1;
        snx_ir_say_miscompile(out, accepted, "22.1.8");
        rewind(out);
        n = fread(line, 1, sizeof line - 1, out);
        fclose(out);
        line[n] = 0;
        CHECK(accepted ? strstr(line, "LLVM 22.1.8: ") == line && strstr(line, "SNX_ACCEPT_LLVM22_MISCOMPILE") &&
                             strstr(line, "VRSHL and VQSHL") && strstr(line, "(#2914)\n")
                       : !n,
              "accepted %d: \"%s\"", accepted, line);
    }
    if (!g_fallback.failures) puts("a native block's object that does not load is LLVM's, said once");
    return g_fallback.failures != 0;
}
