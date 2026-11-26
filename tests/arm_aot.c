/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AOT compilers' blocks (cpu/arm/aot.h): the native back end's, linked into a module and run, and one it refuses (a
   name longer than it keeps) an error naming the block and the reason; the counts and the line that says them; a data
   object of the native back end's (native.h's snx_native_data) whose pointers are the blocks', as an AOT module's index
   has them */
#include "common/cpu/arm/aot.h"
#include "common/cpu/recompiler/rtlib/rtlib_object.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/platform/dl.h"
#include "common/platform/file.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "support/text_lines.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            printf("%s:%d: %s\n", __FILE__, __LINE__, #x);                                                             \
            ++g_test.failures;                                                                                         \
        }                                                                                                              \
    } while (0)
static struct {
    int failures;
    char* folder;
} g_test;

#define PC 0x00100000u
/* add r0, r0, #1; add r0, r0, r1, lsl #3; svc #7 */
static const uint32_t arm_code[3] = {0xE2800001u, 0xE0800181u, 0xEF000007u};
/* adds r0, #2; lsls r1, r0, #1; svc #9 */
static const uint16_t thumb_code[3] = {0x3002u, 0x0041u, 0xDF09u};
/* a name the native back end refuses, of its 24 characters */
#define LONG_NAME "a_block_of_a_name_too_long"

static const SnxArmCodegenOptions options = {SNX_ARM_CODEGEN_OPTIONS_ABI, SNX_ARM_PROFILE_ARM11, SNX_ARM_MEMORY_HOSTED,
                                             0, 0};
/* the code as guest memory holds it */
static size_t code_bytes(unsigned char out[12], int thumb) {
    for (size_t i = 0; i < 3; ++i)
        if (thumb)
            snx_st_le16(out + i * 2, thumb_code[i]);
        else
            snx_st_le32(out + i * 4, arm_code[i]);
    return 3;
}
static int make(SnxArmAot* a, const char* name, int thumb, char* error, size_t size) {
    unsigned char code[12];
    size_t count = code_bytes(code, thumb), consumed = 0;
    if (snx_arm_aot_block(a, name, code, count, PC, thumb, &options, &consumed, error, size)) return -1;
    CHECK(consumed == 3); /* to the SVC, which ends it */
    return 0;
}
[[clang::no_sanitize("function")]] static void run_block(SnxArmBlock block, SnxArmCPU* cpu) { block(cpu); }
static void check_block(void* symbol, int thumb) {
    SnxArmCPU cpu = {0};
    CHECK(symbol != NULL);
    if (!symbol) return;
    cpu.r[0] = 5, cpu.r[1] = 3, cpu.r[15] = PC;
    cpu.cpsr = 0x10u | (thumb ? SNX_ARM_T : 0);
    run_block((SnxArmBlock)symbol, &cpu);
    CHECK(cpu.exit == SNX_ARM_SVC && cpu.syndrome == (thumb ? 9u : 7u) && cpu.r[15] == PC + (thumb ? 6u : 12u));
    CHECK(cpu.r[0] == (thumb ? 7u : 30u) && cpu.r[1] == (thumb ? 14u : 3u));
}
/* the objects linked with the runtime library into folder/name, exports its symbols, and loaded: NULL with the reason
   printed where it could not be */
static void* load(const char* name, SnxLinkInput* objects, size_t count, const char* const* exports, size_t nexports) {
    char error[512] = "";
    SnxLinkOutput linked = {NULL, 0};
    char* path = snx_path_join(g_test.folder, name);
    void* library = NULL;
    if (snx_link(objects, count, exports, nexports, name, &linked, error, sizeof error))
        printf("%s: %s\n", name, error);
    else if (!path || snx_write_file(path, linked.data, linked.size))
        printf("%s: not written\n", name);
    else if (!(library = plat_dl_open(path)))
        printf("%s: %s\n", name, plat_dl_error());
    free(linked.data);
    free(path);
    return library;
}
static SnxLinkInput armlib(void) {
    return (SnxLinkInput){(const unsigned char*)snx_armlib_object, (size_t)snx_armlib_object_size, "armlib"};
}

/* the native back end's blocks in one module, with an index of them in native data, and the one it refuses an error */
static void test_blocks(void) {
    static const char* const names[] = {"plain_arm", "plain_thumb", "index_of_blocks"};
    char error[512] = "";
    SnxArmAot* a = snx_arm_aot_new(error, sizeof error);
    SnxLinkInput in[3] = {armlib()};
    if (!a) printf("%s\n", error);
    CHECK(a != NULL);
    if (!a) return;
    if (make(a, names[0], 0, error, sizeof error) || make(a, names[1], 1, error, sizeof error)) {
        printf("%s\n", error);
        ++g_test.failures;
        snx_arm_aot_free(a);
        return;
    }
    error[0] = 0;
    CHECK(make(a, LONG_NAME, 0, error, sizeof error) < 0);
    CHECK(strstr(error, "ARM block at 0x00100000") && strstr(error, "a block's name too long"));
    if (g_test.failures) printf("the refusal: %s\n", error);
    CHECK(snx_arm_aot_count(a)->native == 2);
    int n = snx_arm_aot_objects(a, in + 1, error, sizeof error);
    if (n < 0) printf("%s\n", error);
    CHECK(n == 1);
    /* none made since: no object */
    SnxLinkInput none;
    CHECK(snx_arm_aot_objects(a, &none, error, sizeof error) == 0 && !none.data);
    unsigned pointer = 0;
    SnxXElf* o = snx_native_data(&pointer);
    CHECK(o != NULL);
    size_t count = 1 + (size_t)(n > 0);
    if (o) {
        unsigned char* object = NULL;
        size_t size = 0, at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, 2 * sizeof(SnxArmBlock), sizeof(SnxArmBlock));
        snx_xelf_symbol(o, names[2], SNX_ELF_RELRO, at, 2 * sizeof(SnxArmBlock), SNX_ELF_GLOBAL);
        for (size_t k = 0; k < 2; ++k)
            snx_xelf_reloc(o, SNX_ELF_RELRO, at + k * sizeof(SnxArmBlock), names[k], pointer, 0);
        CHECK(!snx_xelf_write(o, &object, &size));
        snx_xelf_free(o);
        in[count++] = (SnxLinkInput){object, size, "index"};
    }
    void* library = load("arm_aot_blocks.so", in, count, names, 3);
    CHECK(library != NULL);
    if (library) {
        for (int k = 0; k < 2; ++k) check_block(plat_dl_sym(library, names[k]), k == 1);
        SnxArmBlock* index = (SnxArmBlock*)plat_dl_sym(library, names[2]);
        CHECK(index != NULL);
        for (int k = 0; index && k < 2; ++k) CHECK((void*)index[k] == plat_dl_sym(library, names[k]));
        plat_dl_close(library);
    }
    for (size_t k = 1; k < count; ++k) free((void*)in[k].data);
    snx_arm_aot_free(a);
}
/* a block no compiler takes is an error that names it */
static void test_refused(void) {
    char error[512] = "";
    unsigned char code[12];
    size_t consumed = 5;
    SnxArmAot* a = snx_arm_aot_new(error, sizeof error);
    CHECK(a != NULL);
    if (!a) return;
    code_bytes(code, 0);
    CHECK(snx_arm_aot_block(a, "odd", code, 3, PC + 2, 0, &options, &consumed, error, sizeof error) < 0);
    CHECK(strstr(error, "0x00100002") && strstr(error, "ARM") && !consumed);
    SnxArmCodegenOptions bad = options;
    bad.abi = 99;
    error[0] = 0;
    CHECK(snx_arm_aot_block(a, "bad", code, 3, PC, 0, &bad, &consumed, error, sizeof error) < 0);
    CHECK(strstr(error, "options no block compiler takes"));
    CHECK(!snx_arm_aot_count(a)->native);
    snx_arm_aot_free(a);
}
/* the counts of the threads together, and their line: none when quiet or of no blocks */
static void test_report(void) {
    SnxArmAotCount one = {.native = 10}, two = {.native = 5}, all = {0}, empty = {0};
    snx_arm_aot_add(&all, &one);
    snx_arm_aot_add(&all, &two);
    CHECK(all.native == 15);
    char text[256] = "", want[96];
    FILE* out = tmpfile();
    CHECK(out != NULL);
    if (!out) return;
    snx_arm_aot_report(&all, out);
    snx_arm_aot_report(&all, NULL);
    snx_arm_aot_report(&empty, out);
    rewind(out);
    size_t size = fread(text, 1, sizeof text - 1, out);
    text[size] = 0;
    fclose(out);
    snx_test_text_lines(text);
    snprintf(want, sizeof want, "%s back end: 15 of 15 blocks\n", snx_native_name());
    CHECK(!strcmp(text, want));
    if (strcmp(text, want)) printf("the lines:\n%s", text);
}

int main(void) {
    if (!(g_test.folder = snx_temp_dir(NULL))) {
        puts("arm_aot: no temporary folder");
        return 1;
    }
    test_blocks();
    test_refused();
    test_report();
    (void)snx_remove_tree(g_test.folder);
    free(g_test.folder);
    if (g_test.failures) {
        printf("arm_aot: %d failures\n", g_test.failures);
        return 1;
    }
    printf("arm_aot: ok (%s)\n", snx_native_name());
    return 0;
}
