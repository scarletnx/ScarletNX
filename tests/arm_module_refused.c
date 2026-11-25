/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a 32-bit game's recompile says of the blocks the native back end leaves to LLVM (nx/loader/module_arm.c,
   itself): how many of how many it made, and for each reason the count of its blocks and the state and the address of
   the first, whatever thread made them. The test has the back end refuse every third A32 block for one reason and
   every Thumb one for another */
#include "common/cpu/recompiler/mach/native.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/text_lines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char refused_arm[] = "an ARM block the test refuses", refused_thumb[] = "a Thumb block the test refuses";
static _Thread_local struct {
    const char* why;
} t_arm_module_refused;
/* whether the test refuses the A32 block at pc: the blocks are of 8 bytes, the first at the module's start */
static int test_refuses(uint32_t pc) { return !((pc >> 3) % 3); }
static int test_arm_block(SnxNativeArm* c, const char* name, const unsigned char* bytes, size_t count, uint32_t pc,
                          size_t* consumed, SnxNativeArmNext next, void* user) {
    t_arm_module_refused.why = refused_arm;
    return !test_refuses(pc) && snx_native_arm_block(c, name, bytes, count, pc, consumed, next, user);
}
static int test_thumb_block(SnxNativeArm* c, const char* name, const unsigned char* bytes, size_t count, uint32_t pc,
                            size_t* consumed, SnxNativeArmNext next, void* user) {
    (void)c, (void)name, (void)bytes, (void)count, (void)pc, (void)consumed, (void)next, (void)user;
    t_arm_module_refused.why = refused_thumb;
    return 0;
}
static const char* test_why(const SnxNativeArm* c) { return (void)c, t_arm_module_refused.why; }
#define snx_native_arm_block   test_arm_block
#define snx_native_thumb_block test_thumb_block
#define snx_native_arm_why     test_why
#include "nx/loader/module_arm.c"
#undef snx_native_arm_block
#undef snx_native_thumb_block
#undef snx_native_arm_why

#define BASE         0x200000u
#define TEXT_OFF     0x1000u
#define START        (BASE + TEXT_OFF)
/* more blocks than one thread makes alone in a round (discover) */
#define ARM_BLOCKS   600u
#define THUMB_BLOCKS 7u
#define THUMB_START  (START + 8 * ARM_BLOCKS)

static struct {
    unsigned char memory[0x4000];
    SnxU64 hints[ARM_BLOCKS + THUMB_BLOCKS];
    int failures;
} g_arm_module_refused;

static void fail(const char* what, const char* text) {
    fprintf(stderr, "FAIL: %s:\n%s\n", what, text);
    ++g_arm_module_refused.failures;
}

/* the program: a block every 8 bytes of the text, A32's of an ADD and a BX LR, then Thumb's of an ADDS and a BX LR */
static void program(SnxModuleImage* image) {
    unsigned char* text = g_arm_module_refused.memory + TEXT_OFF;
    for (unsigned i = 0; i < ARM_BLOCKS; ++i) {
        snx_st_le32(text + 8 * i, 0xE2800001u);
        snx_st_le32(text + 8 * i + 4, 0xE12FFF1Eu);
        g_arm_module_refused.hints[i] = START + 8 * i;
    }
    for (unsigned i = 0; i < THUMB_BLOCKS; ++i) {
        unsigned char* at = text + 8 * ARM_BLOCKS + 8 * i;
        snx_st_le16(at, 0x3001u);
        snx_st_le16(at + 2, 0x4770u);
        g_arm_module_refused.hints[ARM_BLOCKS + i] = (THUMB_START + 8 * i) | 1;
    }
    image->name = "test";
    image->base = BASE;
    image->size = sizeof g_arm_module_refused.memory;
    image->image = g_arm_module_refused.memory;
    image->image_size = sizeof g_arm_module_refused.memory;
    image->text_off = TEXT_OFF;
    image->text_size = 0x2000;
}

int main(void) {
    if (!snx_native_arm_available()) {
        printf("SKIP: no native back end on this host: nothing to refuse\n");
        return 77;
    }
    SnxModuleImage image = {0};
    SnxProgram loaded = {0};
    SnxCodegenOptions options = {0};
    SnxLlvmBuild build = {0};
    program(&image);
    loaded.modules = &image;
    loaded.module_count = 1;
    loaded.npdm.title_id = 0x0100000000000000ull;
    loaded.npdm.stack = 0x100000;
    strcpy(loaded.npdm.name, "test");
    options.title = "test";
    options.abi = SNX_ABI;
    options.hints = g_arm_module_refused.hints;
    options.hint_count = ARM_BLOCKS + THUMB_BLOCKS;
    char *folder = snx_temp_dir(NULL), *lines = folder ? snx_path_join(folder, "lines") : NULL;
    char* module = folder ? snx_path_join(folder, "module") : NULL;
    if (!module || !lines || !freopen(lines, "w", stdout)) return 1;
    build.output = module;
    build.opt = '2';
    build.jobs = 4;
    int rc = snx_build_module_arm(&loaded, &options, &build);
    fflush(stdout);
    size_t size = 0;
    char* text = (char*)snx_read_file(lines, &size);
    char* said = (char*)snx_alloc(size + 1);
    if (text) memcpy(said, text, size);
    said[text ? size : 0] = 0;
    free(text);
    snx_test_text_lines(said);
    if (rc) fail("the module does not build", said);
    unsigned refused = 0, first = 0;
    for (unsigned i = ARM_BLOCKS; i--;)
        if (test_refuses(START + 8 * i)) ++refused, first = START + 8 * i;
    char wanted[512];
    snprintf(wanted, sizeof wanted,
             "%s back end: %u of %u AArch32 blocks, LLVM the rest\n"
             "  %u blocks, the first the ARM one at 0x%08x: an ARM block the test refuses\n"
             "  %u blocks, the first the Thumb one at 0x%08x: a Thumb block the test refuses\n",
             snx_native_name(), ARM_BLOCKS - refused, ARM_BLOCKS + THUMB_BLOCKS, refused, first, THUMB_BLOCKS,
             THUMB_START);
    if (!strstr(said, wanted)) {
        fprintf(stderr, "wanted:\n%s", wanted);
        fail("the recompile does not say who made its blocks", said);
    }
#ifdef _WIN32
    if (!freopen("NUL", "w", stdout)) return 1; /* the folder's file closed */
#else
    if (!freopen("/dev/null", "w", stdout)) return 1;
#endif
    if (snx_remove_tree(folder)) fail("the folder is not removed", folder);
    free(said), free(module), free(lines), free(folder);
    return g_arm_module_refused.failures != 0;
}
