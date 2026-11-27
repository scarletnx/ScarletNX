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
    X(native_estimates)                                                                                                \
    X(native_fma_residual)                                                                                             \
    X(native_fp_env)                                                                                                   \
    X(native_fp_store)                                                                                                 \
    X(native_half)                                                                                                     \
    X(native_module)                                                                                                   \
    X(native_recpe)                                                                                                    \
    X(native_round)                                                                                                    \
    X(native_simd_shift)                                                                                               \
    X(native_vfp_compare)                                                                                              \
    X(native_vfp_minmax)                                                                                               \
    X(native_vfp_residual)
#define WITH_NONE(X)                                                                                                   \
    X(crc32_helper)                                                                                                    \
    X(native_recpx)                                                                                                    \
    X(native_rsqrte)                                                                                                   \
    X(native_unsigned_estimates)                                                                                       \
    X(native_vfp_batch)                                                                                                \
    X(native_vfp_lanes)

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
int native_main_native_block_fuzz(int argc, char** argv);
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
        {"native_block_fuzz", native_main_native_block_fuzz},
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
