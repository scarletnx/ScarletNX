/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The objects a native back end writes for an AOT module, on any host: the guest functions of a small program (a call,
   a tail call, a loop, a branch through a register, a stale check, a helper's call), its module descriptor, and two
   chained A32 blocks with theirs. None may have an absolute address in its code: the linker refuses one in a read-only
   section (link_elf.c), since a shared object would need a text relocation there. The little-endian machines' objects
   are then linked into a module (snx_link), which the recompiler would write */
#include "nx/loader/module_x64.h"
#include "nx/loader/loader.h"
#include "common/cpu/recompiler/arm64/arm64_arm.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/codegen.h"
#include "common/cpu/recompiler/la64/la64_arm.h"
#include "common/cpu/recompiler/la64/la64_codegen.h"
#include "common/cpu/recompiler/llvm/elf_object.h"
#include "common/cpu/recompiler/llvm/link.h"
#include "common/cpu/recompiler/mips64/mips64_arm.h"
#include "common/cpu/recompiler/mips64/mips64_codegen.h"
#include "common/cpu/recompiler/mips64/mips64be_arm.h"
#include "common/cpu/recompiler/mips64/mips64be_codegen.h"
#include "common/cpu/recompiler/ppc64/ppc64_arm.h"
#include "common/cpu/recompiler/ppc64/ppc64_codegen.h"
#include "common/cpu/recompiler/ppc64/ppc64be_arm.h"
#include "common/cpu/recompiler/ppc64/ppc64be_codegen.h"
#include "common/cpu/recompiler/rv64/rv64_arm.h"
#include "common/cpu/recompiler/rv64/rv64_codegen.h"
#include "common/cpu/recompiler/s390x/s390x_arm.h"
#include "common/cpu/recompiler/s390x/s390x_codegen.h"
#include "common/cpu/recompiler/x64/x64_arm.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE  0x8000000ull
#define ARM   0x200000u
#define WORDS 0x100

static struct {
    size_t failures;
    unsigned char code[2][WORDS * 4]; /* the program's, and the helper's caller's */
} g_test;

#define FAIL(...)                                                                                                      \
    do {                                                                                                               \
        fprintf(stderr, "FAIL " __VA_ARGS__);                                                                          \
        fprintf(stderr, "\n");                                                                                         \
        ++g_test.failures;                                                                                             \
    } while (0)

/* the relocations of the type in the object's executable sections (type 0: the absolute ones, a failure each); -1 where
   it isn't an object this host reads */
static int relocations(const char* name, const char* what, const unsigned char* object, size_t size, unsigned type) {
    Elf64_Ehdr eh;
    unsigned char* copy;
    int count = 0;
    if (size < sizeof eh || !(copy = (unsigned char*)malloc(size))) return -1;
    memcpy(copy, object, size);
    if (object[EI_DATA] != snx_elf_host_data() && snx_elf_byteswap(copy, size, 0)) {
        free(copy);
        return -1;
    }
    memcpy(&eh, copy, sizeof eh);
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(const void*)(copy + eh.e_shoff);
    for (size_t k = 0; k < eh.e_shnum; ++k) {
        if (sh[k].sh_type != SHT_RELA || !(sh[sh[k].sh_info].sh_flags & SHF_EXECINSTR)) continue;
        const Elf64_Rela* r = (const Elf64_Rela*)(const void*)(copy + sh[k].sh_offset);
        for (size_t n = sh[k].sh_size / sizeof *r, j = 0; j < n; ++j) {
            unsigned t = snx_elf_type(eh.e_machine, r[j].r_info);
            if (type ? t != type : !snx_elf_absolute_relocation(eh.e_machine, t)) continue;
            if (!type && !count)
                FAIL("%s: %s: an absolute address in the code at +%llx", name, what, (unsigned long long)r[j].r_offset);
            ++count;
        }
    }
    free(copy);
    return count;
}

typedef struct {
    const char* name; /* the descriptor's (snx_native_desc_named), NULL for a machine without one of its own */
    int little;
    int (*functions)(SnxCodegen* codegen, const SnxNativeOptions* options, unsigned char* made, unsigned char** object,
                     size_t* size, char* error, size_t error_size);
    int (*arm)(const uint32_t* code, size_t count, unsigned char** object, size_t* size, const char** why);
} Back;
static size_t arm_next(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]) {
    (void)user, (void)units, (void)count;
    if (key != ARM) return 0;
    keys[0] = ARM; /* BNE, back to the block's start: past the thread's poll */
    keys[1] = ARM + 8;
    return 2;
}
/* a machine's two functions, over its back end's */
#define MACHINE(m, T, ...)                                                                                             \
    static int functions_##m(SnxCodegen* codegen, const SnxNativeOptions* options, unsigned char* made,                \
                             unsigned char** object, size_t* size, char* error, size_t error_size) {                   \
        return snx_##m##_object(codegen, 0, codegen->function_count, options, __VA_ARGS__ made, object, size, error,   \
                                error_size);                                                                           \
    }                                                                                                                  \
    static int arm_##m(const uint32_t* code, size_t count, unsigned char** object, size_t* size, const char** why) {   \
        T* c = snx_##m##_arm_new();                                                                                    \
        size_t consumed = 0, at = 0, made = 0;                                                                         \
        *why = "no compiler";                                                                                          \
        if (!c) return -1;                                                                                             \
        *why = "refused";                                                                                              \
        while (at < count) {                                                                                           \
            if (!snx_##m##_arm_block(c, NULL, (const unsigned char*)(code + at), count - at, ARM + (uint32_t)at * 4,   \
                                     &consumed, arm_next, NULL)) {                                                     \
                *why = snx_##m##_arm_why(c);                                                                           \
                break;                                                                                                 \
            }                                                                                                          \
            at += consumed;                                                                                            \
        }                                                                                                              \
        int rc = at < count || snx_##m##_arm_object(c, object, size, &made);                                           \
        snx_##m##_arm_free(c);                                                                                         \
        return rc;                                                                                                     \
    }
MACHINE(rv64, SnxRv64Arm)
MACHINE(la64, SnxLa64Arm)
MACHINE(ppc64, SnxPpc64Arm)
MACHINE(ppc64be, SnxPpc64beArm)
MACHINE(mips64, SnxMips64Arm)
MACHINE(mips64be, SnxMips64beArm)
MACHINE(s390x, SnxS390xArm)
MACHINE(arm64, SnxArm64Arm, SNX_ARM64_F_ALL, )
#if defined(__x86_64__)
MACHINE(x64, SnxX64Arm)
#endif

/* the code's descriptor and functions, made and checked; linked (little-endian): 0, or a failure said */
static void guest(const Back* back, SnxCodegen* cg, int helper, unsigned char** kept, size_t* kept_size) {
    SnxNativeOptions options = {.stale = 1};
    unsigned char* made = (unsigned char*)calloc(cg->function_count + 1, 1);
    unsigned char* object = NULL;
    size_t size = 0, n = 0;
    char error[256] = "";
    if (!made || back->functions(cg, &options, made, &object, &size, error, sizeof error) < 0 || !object) {
        FAIL("%s: the functions' object: %s", back->name, error);
        free(made), free(object);
        return;
    }
    for (size_t k = 0; k < cg->function_count; ++k) n += made[k];
    if (n != cg->function_count) FAIL("%s: %zu of %zu functions made", back->name, n, cg->function_count);
    if (relocations(back->name, helper ? "a function with a helper's call" : "the functions", object, size, 0) < 0)
        FAIL("%s: the functions' object is not one", back->name);
    free(made);
    if (helper) {
        free(object);
        return;
    }
    *kept = object;
    *kept_size = size;
}
/* a JIT unit's literals on MIPS64 hold addresses: its code may be out of a PC32's reach of what it names */
static void jit_unit(const Back* back, SnxCodegen* cg) {
    SnxNativeOptions options = {.jit = 1, .table_name = "UNIT"};
    unsigned char made = 0, *object = NULL;
    size_t size = 0;
    char error[256] = "";
    if (back->functions(cg, &options, &made, &object, &size, error, sizeof error) < 0 || !made || !object)
        FAIL("%s: a JIT unit: %s", back->name, error);
    else if (relocations(back->name, "a JIT unit", object, size, SNX_MIPS_64) < 1 ||
             relocations(back->name, "a JIT unit", object, size, SNX_MIPS_PC32) != 0)
        FAIL("%s: a JIT unit's literals are not the addresses", back->name);
    free(object);
}
static void module(const Back* back, SnxCodegen* cg, const unsigned char* object, size_t size) {
    const SnxNativeDesc* desc = snx_native_desc_named(back->name);
    SnxProgram* loaded = (SnxProgram*)calloc(1, sizeof *loaded);
    SnxCodegenOptions options = {0};
    unsigned char* descriptor = NULL;
    size_t descriptor_size = 0, entries = 0, variants = 0;
    char error[256] = "";
    if (!desc || !loaded ||
        snx_x64_descriptor_for(desc, cg, &options, loaded, NULL, 0, NULL, 0, &descriptor, &descriptor_size, &entries,
                               &variants, error, sizeof error)) {
        FAIL("%s: the descriptor: %s", back->name, error);
        free(loaded);
        return;
    }
    free(loaded);
    if (relocations(back->name, "the descriptor", descriptor, descriptor_size, 0) < 0)
        FAIL("%s: the descriptor is not an object", back->name);
    if (back->little) {
        const SnxLinkInput inputs[2] = {{object, size, "code"}, {descriptor, descriptor_size, "module"}};
        const char* const exports[1] = {"snx_module"};
        SnxLinkOutput linked = {0};
        if (snx_link(inputs, 2, exports, 1, "module.so", &linked, error, sizeof error))
            FAIL("%s: linking the module: %s", back->name, error);
        free(linked.data);
    }
    free(descriptor);
}
static void arm(const Back* back) {
    /* SUBS R0, R0, #1; BNE the block; then ADD R1, R1, #1; BX LR */
    static const uint32_t code[4] = {0xE2500001u, 0x1AFFFFFDu, 0xE2811001u, 0xE12FFF1Eu};
    unsigned char *object = NULL, *descriptor = NULL;
    size_t size = 0, descriptor_size = 0;
    const char* why = NULL;
    char error[256] = "";
    if (back->arm(code, 4, &object, &size, &why)) {
        FAIL("%s: the A32 blocks: %s", back->name, why);
        free(object);
        return;
    }
    if (relocations(back->name, "the A32 blocks", object, size, 0) < 0)
        FAIL("%s: the A32 blocks' object is not one", back->name);
    const SnxNativeDesc* desc = snx_native_desc_named(back->name);
    SnxProgram* loaded = (SnxProgram*)calloc(1, sizeof *loaded);
    SnxCodegenOptions options = {0};
    const SnxU32 keys[2] = {ARM, ARM + 8};
    if (desc && loaded &&
        snx_x64_arm_descriptor_for(desc, loaded, &options, keys, 2, &descriptor, &descriptor_size, error, sizeof error))
        FAIL("%s: the A32 descriptor: %s", back->name, error);
    else if (desc && loaded) {
        if (relocations(back->name, "the A32 descriptor", descriptor, descriptor_size, 0) < 0)
            FAIL("%s: the A32 descriptor is not an object", back->name);
        if (back->little) {
            const SnxLinkInput inputs[2] = {{object, size, "blocks"}, {descriptor, descriptor_size, "module"}};
            const char* const exports[1] = {"snx_module_arm"};
            SnxLinkOutput linked = {0};
            if (snx_link(inputs, 2, exports, 1, "arm.so", &linked, error, sizeof error))
                FAIL("%s: linking the A32 module: %s", back->name, error);
            free(linked.data);
        }
    }
    free(loaded), free(object), free(descriptor);
}

int main(void) {
    static const Back backs[] = {
#define BACK(m, name, little) {name, little, functions_##m, arm_##m}
        BACK(rv64, "riscv64", 1),    BACK(la64, "loongarch64", 1), BACK(ppc64, "ppc64le", 1), BACK(ppc64be, "ppc64", 0),
        BACK(mips64, "mips64el", 1), BACK(mips64be, "mips64", 0),  BACK(s390x, "s390x", 0),   BACK(arm64, "arm64", 1),
#if defined(__x86_64__)
        BACK(x64, "x86-64", 1),
#endif
    };
    /* f0 at CODE, f1 at +0x100: X0 counted down to 0 in a loop, f1 called and then jumped to; f2 at +0x200: BR X3 */
    static const struct {
        size_t at;
        uint32_t words[6];
    } program[] = {{0, {0xD2800020u, 0x9400003Fu, 0xF1000400u, 0x54FFFFE1u, 0x1400003Cu}},
                   {0x40, {0x91000400u, 0xD65F03C0u}},
                   {0x80, {0xD61F0060u}}};
    /* f3: FADD D0, D1, D2 and RET, whose rounding the RISC machines call a helper for */
    static const uint32_t helper[2] = {0x1E622820u, 0xD65F03C0u};
    const uint64_t hints[3] = {CODE, CODE + 0x100, CODE + 0x200}, helper_hint[1] = {CODE};
    SnxCodeImage view = {
        .image = g_test.code[0], .image_size = sizeof g_test.code[0], .text_size = sizeof g_test.code[0], .base = CODE};
    SnxCodegen cg, cgh;
    for (size_t k = 0; k < sizeof program / sizeof *program; ++k)
        memcpy(g_test.code[0] + program[k].at * 4, program[k].words, sizeof program[k].words);
    if (snx_codegen_analyze(&cg, &view, 1, hints, 3, 1, SNX_A64_TARGET_SWITCH) || cg.function_count != 3) {
        FAIL("the program's functions: %zu", cg.function_count);
        return 1;
    }
    memcpy(g_test.code[1], helper, sizeof helper);
    view.image = g_test.code[1];
    if (snx_codegen_analyze(&cgh, &view, 1, helper_hint, 1, 1, SNX_A64_TARGET_SWITCH) || cgh.function_count != 1) {
        FAIL("the helper's function: %zu", cgh.function_count);
        return 1;
    }
    for (size_t k = 0; k < sizeof backs / sizeof *backs; ++k) {
        size_t before = g_test.failures, size = 0;
        unsigned char* object = NULL;
        guest(&backs[k], &cg, 0, &object, &size);
        guest(&backs[k], &cgh, 1, &object, &size);
        if (!strncmp(backs[k].name, "mips64", 6)) jit_unit(&backs[k], &cgh);
        /* the host's own descriptor is made by the host alone; every RISC machine's here */
        if (object && snx_native_desc_named(backs[k].name)) module(&backs[k], &cg, object, size);
        arm(&backs[k]);
        free(object);
        printf("native_link %s: %s\n", backs[k].name, g_test.failures == before ? "ok" : "FAILED");
    }
    snx_codegen_free(&cg), snx_codegen_free(&cgh);
    return g_test.failures != 0;
}
