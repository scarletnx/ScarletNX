/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The AOT compilers' blocks (cpu/arm/aot.h): the native back end's, LLVM's for one it refuses (a name longer than it
   keeps), and every one LLVM's where the IR is kept, linked into a module and run; the counts of who made them and why,
   and the lines that say them; a data object of the native back end's (native.h's snx_native_data) whose pointers are
   the blocks', as an AOT module's index has them */
#include "common/cpu/arm/aot.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/llvm/rtlib_object.h"
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
    char whys[SNX_ARM_AOT_WHYS + 2][16]; /* reasons that stay, as the maker's do */
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

/* the native back end's blocks and the one it refuses, in one module, with an index of them in native data */
static void test_blocks(void) {
    static const char* const names[] = {"plain_arm", "plain_thumb", LONG_NAME, "index_of_blocks"};
    const int native = snx_native_arm_available();
    char error[512] = "";
    SnxArmAot* a = snx_arm_aot_new('1', NULL);
    SnxLinkInput in[4] = {armlib()};
    CHECK(a != NULL);
    if (!a) return;
    int rc = make(a, names[0], 0, error, sizeof error) || make(a, names[1], 1, error, sizeof error);
    if (native) CHECK(!snx_ir_started()); /* its work starts no LLVM */
    if (rc || make(a, names[2], 0, error, sizeof error)) {
        printf("%s\n", error);
        ++g_test.failures;
        snx_arm_aot_free(a);
        return;
    }
    const SnxArmAotCount* c = snx_arm_aot_count(a);
    CHECK(c->native + c->llvm == 3);
    if (native) {
        CHECK(c->native == 2 && c->llvm == 1 && c->nwhys == 1 && !strcmp(c->whys[0].why, "a block's name too long"));
        CHECK(c->whys[0].count == 1 && c->whys[0].lowest == PC);
    } else
        CHECK(c->llvm == 3 && c->nwhys == 1 && c->whys[0].why == snx_arm_aot_none);
    int n = snx_arm_aot_objects(a, "blocks", in + 1, error, sizeof error);
    if (n < 0) printf("%s\n", error);
    CHECK(n == (native ? 2 : 1));
    /* none made since: no object */
    SnxLinkInput none[2];
    CHECK(snx_arm_aot_objects(a, "none", none, error, sizeof error) == 0 && !none[0].data && !none[1].data);
    size_t count = 1 + (size_t)(n > 0 ? n : 0);
    unsigned pointer = 0;
    SnxXElf* o = snx_native_data(&pointer);
    CHECK(!o == !native);
    if (o) {
        unsigned char* object = NULL;
        size_t size = 0, at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, 3 * sizeof(SnxArmBlock), sizeof(SnxArmBlock));
        snx_xelf_symbol(o, names[3], SNX_ELF_RELRO, at, 3 * sizeof(SnxArmBlock), SNX_ELF_GLOBAL);
        for (size_t k = 0; k < 3; ++k)
            snx_xelf_reloc(o, SNX_ELF_RELRO, at + k * sizeof(SnxArmBlock), names[k], pointer, 0);
        CHECK(!snx_xelf_write(o, &object, &size));
        snx_xelf_free(o);
        in[count++] = (SnxLinkInput){object, size, "index"};
    }
    void* library = load("arm_aot_blocks.so", in, count, names, native ? 4 : 3);
    CHECK(library != NULL);
    if (library) {
        for (int k = 0; k < 3; ++k) check_block(plat_dl_sym(library, names[k]), k == 1);
        SnxArmBlock* index = native ? (SnxArmBlock*)plat_dl_sym(library, names[3]) : NULL;
        CHECK(!native || index);
        for (int k = 0; index && k < 3; ++k) CHECK((void*)index[k] == plat_dl_sym(library, names[k]));
        plat_dl_close(library);
    }
    for (size_t k = 1; k < count; ++k) free((void*)in[k].data);
    snx_arm_aot_free(a);
}
/* --keep-ir: every block LLVM's, whatever the host's back end, and their IR in the folder */
static void test_kept(void) {
    static const char* const names[] = {"kept_arm", "kept_thumb"};
    char error[512] = "";
    SnxArmAot* a = snx_arm_aot_new('1', g_test.folder);
    SnxLinkInput in[3] = {armlib()};
    CHECK(a != NULL);
    if (!a) return;
    CHECK(!make(a, names[0], 0, error, sizeof error) && !make(a, names[1], 1, error, sizeof error));
    const SnxArmAotCount* c = snx_arm_aot_count(a);
    CHECK(!c->native && c->llvm == 2 && c->nwhys == 1 && c->whys[0].why == snx_arm_aot_kept);
    CHECK(c->whys[0].count == 2 && c->whys[0].lowest == PC);
    CHECK(snx_arm_aot_objects(a, "kept_blocks", in + 1, error, sizeof error) == 1 && in[1].data && !in[2].data);
    char* path = snx_path_join(g_test.folder, "kept_blocks.ll");
    size_t size = 0;
    unsigned char* text = path ? snx_read_file(path, &size) : NULL;
    CHECK(text && size > 0);
    char* ir = text ? malloc(size + 1) : NULL;
    if (ir) {
        memcpy(ir, text, size);
        ir[size] = 0;
        CHECK(strstr(ir, "@kept_arm(") && strstr(ir, "@kept_thumb("));
    }
    void* library = load("arm_aot_kept.so", in, 2, names, 2);
    CHECK(library != NULL);
    if (library) {
        for (int k = 0; k < 2; ++k) check_block(plat_dl_sym(library, names[k]), k);
        plat_dl_close(library);
    }
    free(ir);
    free(text);
    free(path);
    free((void*)in[1].data);
    snx_arm_aot_free(a);
}
/* a block no compiler takes is an error that names it */
static void test_refused_by_both(void) {
    char error[512] = "";
    unsigned char code[12];
    size_t consumed = 5;
    SnxArmAot* a = snx_arm_aot_new('1', NULL);
    CHECK(a != NULL);
    if (!a) return;
    code_bytes(code, 0);
    CHECK(snx_arm_aot_block(a, "odd", code, 3, PC + 2, 0, &options, &consumed, error, sizeof error) < 0);
    CHECK(strstr(error, "0x00100002") && strstr(error, "ARM"));
    SnxArmCodegenOptions bad = options;
    bad.abi = 99;
    error[0] = 0;
    CHECK(snx_arm_aot_block(a, "bad", code, 3, PC, 0, &bad, &consumed, error, sizeof error) < 0 && error[0]);
    const SnxArmAotCount* c = snx_arm_aot_count(a);
    CHECK(!c->native && !c->llvm);
    snx_arm_aot_free(a);
}
/* the counts of the threads together: a reason once, with its lowest block, and the reasons past the list's room
   counted apart. Their lines: the native back end's blocks on stdout, the others' on stderr with the reason, how many
   and the first one's address */
static void test_report(void) {
    SnxArmAotCount one = {.native = 10}, two = {.native = 5}, all = {0};
    snx_arm_aot_refused(&one, "reason a", 0x2001);
    snx_arm_aot_refused(&one, "reason b", 0x3000);
    snx_arm_aot_refused(&two, "reason a", 0x1000);
    snx_arm_aot_refused(&two, "reason a", 0x5001);
    snx_arm_aot_add(&all, &one);
    snx_arm_aot_add(&all, &two);
    CHECK(all.native == 15 && all.llvm == 4 && all.nwhys == 2 && !all.unlisted);
    CHECK(all.whys[0].count == 3 && all.whys[0].lowest == 0x1000 && all.whys[1].count == 1);
    char *path = snx_path_join(g_test.folder, "stderr.txt"), text[2048] = "";
    size_t size = 0;
    CHECK(path && freopen(path, "w", stderr));
    snx_arm_aot_report(&all, "scarletnx-test", 1);
    SnxArmAotCount kept = {0}, full = {0}, both = {0};
    snx_arm_aot_refused(&kept, snx_arm_aot_kept, 0x1000);
    snx_arm_aot_report(&kept, "scarletnx-test", 1); /* none refused: no line of it on stderr */
    fflush(stderr);
    unsigned char* read = path ? snx_read_file(path, &size) : NULL;
    if (read && size < sizeof text) memcpy(text, read, size), text[size] = 0;
    snx_test_text_lines(text);
    CHECK(strstr(text, "scarletnx-test: 3 blocks made by LLVM, the first the ARM one at 0x00001000: reason a\n"));
    CHECK(strstr(text, "scarletnx-test: 1 block made by LLVM, the first the ARM one at 0x00003000: reason b\n"));
    CHECK(!strstr(text, "keep-ir"));
    if (g_test.failures) printf("the lines on stderr:\n%s", text);
    for (int k = 0; k < SNX_ARM_AOT_WHYS + 2; ++k) {
        snprintf(g_test.whys[k], sizeof g_test.whys[k], "reason %d", k);
        snx_arm_aot_refused(&full, g_test.whys[k], 0x1000u + 4u * (unsigned)k);
    }
    CHECK(full.llvm == SNX_ARM_AOT_WHYS + 2 && full.nwhys == SNX_ARM_AOT_WHYS && full.unlisted == 2);
    snx_arm_aot_add(&both, &full);
    CHECK(both.llvm == SNX_ARM_AOT_WHYS + 2 && both.unlisted == 2);
    /* the folder's file closed, so that the folder goes where an open file keeps it */
#ifdef _WIN32
    CHECK(freopen("NUL", "w", stderr));
#else
    CHECK(freopen("/dev/null", "w", stderr));
#endif
    free(read);
    free(path);
}

int main(void) {
    if (!(g_test.folder = snx_temp_dir(NULL))) {
        puts("arm_aot: no temporary folder");
        return 1;
    }
    test_blocks();
    test_kept();
    test_refused_by_both();
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
