/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The instruction tests' programs over the host's native back end's steps (steps.h), in one executable: what the back
   ends of the nine machines are made of is in it once. Each is compiled with its main named after its file
   (tests/CMakeLists.txt), and takes its own arguments after the name
     snx-test-native-steps PROGRAM [ARGUMENT...] */
#include <stdio.h>
#include <string.h>

/* the programs whose main takes arguments, and those whose main takes none */
#define WITH_ARGUMENTS(X)                                                                                              \
    X(a64_ported)                                                                                                      \
    X(llvm_estimates)                                                                                                  \
    X(llvm_fma_residual)                                                                                               \
    X(llvm_fp_env)                                                                                                     \
    X(llvm_fp_store)                                                                                                   \
    X(llvm_half)                                                                                                       \
    X(llvm_module)                                                                                                     \
    X(llvm_recpe)                                                                                                      \
    X(llvm_round)                                                                                                      \
    X(llvm_simd_shift)                                                                                                 \
    X(llvm_vfp_compare)                                                                                                \
    X(llvm_vfp_minmax)                                                                                                 \
    X(llvm_vfp_residual)
#define WITH_NONE(X)                                                                                                   \
    X(crc32_helper)                                                                                                    \
    X(llvm_recpx)                                                                                                      \
    X(llvm_rsqrte)                                                                                                     \
    X(llvm_unsigned_estimates)                                                                                         \
    X(llvm_vfp_batch)                                                                                                  \
    X(llvm_vfp_lanes)

#define DECLARE(name) int native_main_##name(int argc, char** argv);
WITH_ARGUMENTS(DECLARE)
#undef DECLARE
#define DECLARE(name)                                                                                                  \
    int native_main_##name(void);                                                                                      \
    static int run_##name(int argc, char** argv) {                                                                     \
        (void)argc, (void)argv;                                                                                        \
        return native_main_##name();                                                                                   \
    }
WITH_NONE(DECLARE)
#undef DECLARE
#ifndef _WIN32
int native_main_llvm_block_fuzz(int argc, char** argv);
#endif

typedef struct {
    const char* name;
    int (*run)(int argc, char** argv);
} Program;

#define ENTRY(name) {#name, native_main_##name},
#define PLAIN(name) {#name, run_##name},
static const Program programs[] = {
    WITH_ARGUMENTS(ENTRY) WITH_NONE(PLAIN)
#ifndef _WIN32
        {"llvm_block_fuzz", native_main_llvm_block_fuzz},
#endif
};

int main(int argc, char** argv) {
    for (size_t i = 0; argc > 1 && i < sizeof(programs) / sizeof(programs[0]); ++i)
        if (!strcmp(argv[1], programs[i].name)) return programs[i].run(argc - 1, argv + 1);
    fprintf(stderr,
            "usage: snx-test-native-steps PROGRAM [ARGUMENT...]; %s is none of:", argc > 1 ? argv[1] : "(none)");
    for (size_t i = 0; i < sizeof(programs) / sizeof(programs[0]); ++i) fprintf(stderr, " %s", programs[i].name);
    fputs("\n", stderr);
    return 2;
}
