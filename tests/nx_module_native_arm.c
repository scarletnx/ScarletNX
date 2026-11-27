/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nx_module_native.c's 32-bit program: module_arm.c, here, its native back end refusing the block the test asks or
   none at all. Every block the native back end's: the module; a block it leaves, a host with no native back end for
   AArch32: refused, naming the block */
#include "common/cpu/recompiler/mach/native.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
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
#define BASE     0x200000u
#define TEXT_OFF 0x1000u
#define START    (BASE + TEXT_OFF)
#define BLOCKS   3u /* A32's of an ADD and a BX LR, each 8 bytes from START, then a Thumb one */

static struct {
    int failures;
    SnxU32 refuse;   /* the block the native back end leaves, 0 for none */
    int no_native;   /* the host is to have no native back end for AArch32 */
    char said[4096]; /* the errors said since the last build, a line each */
    unsigned char memory[TEXT_OFF + 0x1000];
    SnxU64 hints[BLOCKS + 1];
} g_nx_module_native_arm;

static const char refused[] = "a block the test refuses";
static int test_arm_block(SnxNativeArm* c, const char* name, const unsigned char* bytes, size_t count, uint32_t pc,
                          size_t* consumed, SnxNativeArmNext next, void* user) {
    return pc != g_nx_module_native_arm.refuse &&
           snx_native_arm_block(c, name, bytes, count, pc, consumed, next, user);
}
static const char* test_why(const SnxNativeArm* c) {
    return g_nx_module_native_arm.refuse ? refused : snx_native_arm_why(c);
}
static int test_arm_available(void) { return !g_nx_module_native_arm.no_native && snx_native_arm_available(); }
#define snx_native_arm_block     test_arm_block
#define snx_native_arm_why       test_why
#define snx_native_arm_available test_arm_available
#include "nx/loader/module_arm.c"
#undef snx_native_arm_block
#undef snx_native_arm_why
#undef snx_native_arm_available

int snx_test_arm_native(const char* folder);

#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            ++g_nx_module_native_arm.failures;                                                                       \
        }                                                                                                              \
    } while (0)

static void sink(const char* line) {
    size_t n = strlen(g_nx_module_native_arm.said);
    snprintf(g_nx_module_native_arm.said + n, sizeof g_nx_module_native_arm.said - n, "%s\n", line);
}

/* the program's module as output: 0, or the builder's failure, said */
static int build(const char* output) {
    unsigned char* text = g_nx_module_native_arm.memory + TEXT_OFF;
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    SnxCodegenOptions options = {0};
    SnxModuleBuild job = {0};
    for (unsigned i = 0; i < BLOCKS; ++i) {
        snx_st_le32(text + 8 * i, 0xE2800001u);     /* add r0, r0, #1 */
        snx_st_le32(text + 8 * i + 4, 0xE12FFF1Eu); /* bx lr */
        g_nx_module_native_arm.hints[i] = START + 8 * i;
    }
    snx_st_le16(text + 8 * BLOCKS, 0x3001u);     /* adds r0, #1 */
    snx_st_le16(text + 8 * BLOCKS + 2, 0x4770u); /* bx lr */
    g_nx_module_native_arm.hints[BLOCKS] = (START + 8 * BLOCKS) | 1;
    image.name = "test";
    image.base = BASE;
    image.size = sizeof g_nx_module_native_arm.memory;
    image.image = g_nx_module_native_arm.memory;
    image.image_size = sizeof g_nx_module_native_arm.memory;
    image.text_off = TEXT_OFF;
    image.text_size = 0x1000;
    loaded.modules = &image;
    loaded.module_count = 1;
    loaded.npdm.title_id = 0x0100000000000000ull;
    loaded.npdm.stack = 0x100000;
    strcpy(loaded.npdm.name, "test");
    options.title = "test";
    options.abi = SNX_ABI;
    options.quiet = 1;
    options.chunk_instrs = 1;
    options.hints = g_nx_module_native_arm.hints;
    options.hint_count = BLOCKS + 1;
    job.output = output;
    job.opt = '2';
    job.jobs = 1;
    job.quiet = 1;
    g_nx_module_native_arm.said[0] = 0;
    snx_error_sink(sink);
    return snx_build_module_arm(&loaded, &options, &job);
}

int snx_test_arm_native(const char* folder) {
    char *module = snx_path_join(folder, "a32" SUFFIX), *output = snx_path_join(folder, "a32-left" SUFFIX);
    char wanted[512];
    if (!snx_native_arm_available()) {
        printf("no native back end for AArch32 blocks on this host: no module of them\n");
        return 0;
    }
    /* every block the native back end's */
    CHECK(!build(module) && snx_is_file(module) && !g_nx_module_native_arm.said[0],
          "the module of the native back end's blocks: %s", g_nx_module_native_arm.said);
    /* one it leaves: the second A32 block */
    g_nx_module_native_arm.refuse = START + 8;
    int rc = build(output);
    g_nx_module_native_arm.refuse = 0;
    snprintf(wanted, sizeof wanted, "AArch32: the %s back end made no ARM block at 0x%08x: %s\n", snx_native_name(),
             START + 8, refused);
    CHECK(rc && !strcmp(g_nx_module_native_arm.said, wanted) && !snx_is_file(output),
          "a block the native back end leaves refuses the module, naming it: %s", g_nx_module_native_arm.said);
    /* a host with no native back end for AArch32 */
    g_nx_module_native_arm.no_native = 1;
    free(output);
    output = snx_path_join(folder, "a32-alone" SUFFIX);
    rc = build(output);
    g_nx_module_native_arm.no_native = 0;
    snprintf(wanted, sizeof wanted, "this host's native back end (%s) makes no AArch32 blocks\n", snx_native_name());
    CHECK(rc && !strcmp(g_nx_module_native_arm.said, wanted) && !snx_is_file(output),
          "a host with no native back end for AArch32: %s", g_nx_module_native_arm.said);
    free(output);
    free(module);
    return g_nx_module_native_arm.failures;
}
