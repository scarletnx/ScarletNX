/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The objects a native back end writes for an AOT module, on any host: the guest functions of a small program (a call,
   a tail call, a loop, a branch through a register, a stale check, a helper's call), its module descriptor, and two
   chained blocks of each AArch32 state with theirs. None may have an absolute address in its code: the linker refuses one in a read-only
   section (link_elf.c), since a shared object would need a text relocation there. The little-endian machines' objects
   are then linked into a module (snx_link), which the recompiler would write; a host whose linker writes no ELF says
   so, and the objects are still checked. On an x86-64 host its objects for Windows too (COFF, linked into a DLL, whose
   exports, function table and base relocations are read back) */
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
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CODE  0x8000000ull
#define ARM   0x200000u
#define THUMB 0x200101u /* a Thumb block's key: its address with bit 0 set */
#define WORDS 0x100

static struct {
    size_t failures, linked, unlinked; /* the modules linked, and those the host's linker writes no ELF for */
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

/* a machine's module linked, or a failure said, or said to be no link of this host's with the linker's reason
   (link_elf.c's for a host whose modules are not ELF) */
static void link_module(const char* name, const char* what, const SnxLinkInput* inputs, const char* export,
                        const char* soname) {
    const char* const exports[1] = {export};
    SnxLinkOutput linked = {0};
    char error[256] = "";
    if (!snx_link(inputs, 2, exports, 1, soname, &linked, error, sizeof error))
        ++g_test.linked;
    else if (strstr(error, "is not supported on this host")) {
        printf("native_link %s: %s not linked on this host: %s\n", name, what, error);
        ++g_test.unlinked;
    } else
        FAIL("%s: linking %s: %s", name, what, error);
    free(linked.data);
}

typedef struct {
    const char* name; /* the descriptor's (snx_native_desc_named), NULL for a machine without one of its own */
    int little;
    int (*functions)(SnxCodegen* codegen, const SnxNativeOptions* options, unsigned char* made, unsigned char** object,
                     size_t* size, char* error, size_t error_size);
    int (*arm)(const uint32_t* code, size_t count, const uint16_t* half, size_t halves, unsigned char** object,
               size_t* size, const char** why);
} Back;
static size_t arm_next(void* user, uint32_t key, size_t units, size_t count, uint32_t keys[2]) {
    (void)user, (void)count;
    if (key != ARM && key != THUMB) return 0;
    keys[0] = key; /* BNE, back to the block's start: past the thread's poll */
    keys[1] = key + (uint32_t)units * (key & 1 ? 2 : 4);
    return 2;
}
/* a machine's two functions, over its back end's */
#define MACHINE(m, T, ...)                                                                                             \
    static int functions_##m(SnxCodegen* codegen, const SnxNativeOptions* options, unsigned char* made,                \
                             unsigned char** object, size_t* size, char* error, size_t error_size) {                   \
        return snx_##m##_object(codegen, 0, codegen->function_count, options, __VA_ARGS__ made, object, size, error,   \
                                error_size);                                                                           \
    }                                                                                                                  \
    static int arm_##m(const uint32_t* code, size_t count, const uint16_t* half, size_t halves,                        \
                       unsigned char** object, size_t* size, const char** why) {                                       \
        T* c = snx_##m##_arm_new();                                                                                    \
        size_t consumed = 0, at = 0, thumb = 0, made = 0;                                                              \
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
        while (at == count && thumb < halves) {                                                                        \
            if (!snx_##m##_arm_thumb_block(c, NULL, (const unsigned char*)(half + thumb), halves - thumb,              \
                                           (THUMB & ~1u) + (uint32_t)thumb * 2, &consumed, arm_next, NULL)) {          \
                *why = snx_##m##_arm_why(c);                                                                           \
                break;                                                                                                 \
            }                                                                                                          \
            thumb += consumed;                                                                                         \
        }                                                                                                              \
        int rc = at < count || thumb < halves || snx_##m##_arm_object(c, object, size, &made);                         \
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
        link_module(back->name, "the module", inputs, "snx_module", "module.so");
    }
    free(descriptor);
}
static void arm(const Back* back) {
    /* SUBS R0, R0, #1; BNE the block; then ADD R1, R1, #1; BX LR */
    static const uint32_t code[4] = {0xE2500001u, 0x1AFFFFFDu, 0xE2811001u, 0xE12FFF1Eu};
    static const uint16_t half[4] = {0x3801u, 0xD1FDu, 0x3101u, 0x4770u}; /* the same in T16 */
    unsigned char *object = NULL, *descriptor = NULL;
    size_t size = 0, descriptor_size = 0;
    const char* why = NULL;
    char error[256] = "";
    if (back->arm(code, 4, half, 4, &object, &size, &why)) {
        FAIL("%s: the AArch32 blocks: %s", back->name, why);
        free(object);
        return;
    }
    if (relocations(back->name, "the AArch32 blocks", object, size, 0) < 0)
        FAIL("%s: the AArch32 blocks' object is not one", back->name);
    const SnxNativeDesc* desc = snx_native_desc_named(back->name);
    SnxProgram* loaded = (SnxProgram*)calloc(1, sizeof *loaded);
    SnxCodegenOptions options = {0};
    const SnxU32 keys[4] = {ARM, ARM + 8, THUMB, THUMB + 4};
    if (desc && loaded &&
        snx_x64_arm_descriptor_for(desc, loaded, &options, keys, 4, &descriptor, &descriptor_size, error, sizeof error))
        FAIL("%s: the A32 descriptor: %s", back->name, error);
    else if (desc && loaded) {
        if (relocations(back->name, "the A32 descriptor", descriptor, descriptor_size, 0) < 0)
            FAIL("%s: the A32 descriptor is not an object", back->name);
        if (back->little) {
            const SnxLinkInput inputs[2] = {{object, size, "blocks"}, {descriptor, descriptor_size, "module"}};
            link_module(back->name, "the A32 module", inputs, "snx_module_arm", "arm.so");
        }
    }
    free(loaded), free(object), free(descriptor);
}

#if defined(__x86_64__)
/* a section of the DLL at rva: its bytes in the file, *size its own; NULL where no section holds it */
static const unsigned char* at_rva(const unsigned char* pe, uint32_t rva, uint32_t* size) {
    const unsigned char* sections = pe + 0x80 + 24 + 240;
    for (unsigned k = 0, n = snx_ld_le16(pe + 0x80 + 6); k < n; ++k) {
        const unsigned char* s = sections + 40 * (size_t)k;
        uint32_t lo = snx_ld_le32(s + 12), length = snx_ld_le32(s + 8);
        if (rva < lo || rva >= lo + length || !snx_ld_le32(s + 20)) continue;
        *size = lo + length - rva;
        return pe + snx_ld_le32(s + 20) + (rva - lo);
    }
    return NULL;
}
/* a COFF object's .bss: 1 where it takes no bytes of the file, 0 where it does, -1 where there is none */
static int bss_without_bytes(const unsigned char* object) {
    for (uint32_t i = 0; i < snx_ld_le16(object + 2); ++i) {
        const unsigned char* h = object + 20 + 40 * (size_t)i;
        if (!strncmp((const char*)h, ".bss", 8)) return !snx_ld_le32(h + 20);
    }
    return -1;
}
/* the functions, an object of A32 and Thumb blocks and their descriptors for Windows, each pair linked into a DLL */
static void windows(SnxCodegen* cg) {
    static const uint32_t code[4] = {0xE2500001u, 0x1AFFFFFDu, 0xE2811001u, 0xE12FFF1Eu};
    static const uint16_t half[4] = {0x3801u, 0xD1FDu, 0x3101u, 0x4770u};
    const SnxU32 keys[4] = {ARM, ARM + 8, THUMB, THUMB + 4};
    SnxNativeOptions options = {.stale = 1};
    SnxCodegenOptions module_options = {0};
    unsigned char* made = (unsigned char*)calloc(cg->function_count + 1, 1);
    SnxProgram* loaded = (SnxProgram*)calloc(1, sizeof *loaded);
    unsigned char *objects[2] = {NULL, NULL}, *descriptors[2] = {NULL, NULL};
    size_t sizes[2] = {0}, descriptor_sizes[2] = {0}, entries = 0, variants = 0;
    const char* why = "";
    char error[256] = "";
    snx_x64_set_win64(1);
    if (!made || !loaded ||
        snx_x64_object(cg, 0, cg->function_count, &options, made, &objects[0], &sizes[0], error, sizeof error) < 0 ||
        snx_x64_descriptor_for(NULL, cg, &module_options, loaded, NULL, 0, NULL, 0, &descriptors[0],
                               &descriptor_sizes[0], &entries, &variants, error, sizeof error) ||
        arm_x64(code, 4, half, 4, &objects[1], &sizes[1], &why) ||
        snx_x64_arm_descriptor_for(NULL, loaded, &module_options, keys, 4, &descriptors[1], &descriptor_sizes[1], error,
                                   sizeof error))
        FAIL("windows: an object: %s %s", error, why);
    else
        for (int k = 0; k < 2; ++k) {
            const SnxLinkInput inputs[2] = {{objects[k], sizes[k], "code"},
                                            {descriptors[k], descriptor_sizes[k], "module"}};
            const char* const exports[1] = {k ? "snx_module_arm" : "snx_module"};
            const int bss = bss_without_bytes(descriptors[k]);
            SnxLinkOutput dll = {0};
            uint32_t left = 0, count = 0, last = 0, size, pdata, relocs;
            const unsigned char *p, *name;
            if (!bss || (!k && bss < 0)) FAIL("windows: the %s object's .bss: %d", exports[0], bss);
            if (snx_link(inputs, 2, exports, 1, "module.dll", &dll, error, sizeof error)) {
                FAIL("windows: linking the %s: %s", exports[0], error);
                continue;
            }
            if (memcmp(dll.data, "MZ", 2) || memcmp(dll.data + 0x80, "PE\0", 4) ||
                snx_ld_le16(dll.data + 0x84) != 0x8664)
                FAIL("windows: %s: not an x86-64 DLL", exports[0]);
            /* the data directories: the exports, the function table (3), the base relocations (5) */
            p = at_rva(dll.data, snx_ld_le32(dll.data + 0x98 + 112), &left);
            name = p ? at_rva(dll.data, snx_ld_le32(p + 32), &left) : NULL;
            name = name ? at_rva(dll.data, snx_ld_le32(name), &left) : NULL;
            if (!name || strcmp((const char*)name, exports[0])) FAIL("windows: %s is not exported", exports[0]);
            pdata = snx_ld_le32(dll.data + 0x98 + 112 + 24), size = snx_ld_le32(dll.data + 0x98 + 112 + 28);
            p = at_rva(dll.data, pdata, &left);
            for (uint32_t at = 0; p && at + 12 <= size; at += 12, ++count) {
                uint32_t begin = snx_ld_le32(p + at), end = snx_ld_le32(p + at + 4);
                const unsigned char* text = at_rva(dll.data, begin, &left);
                const unsigned char* info = at_rva(dll.data, snx_ld_le32(p + at + 8), &left);
                if (begin < last || end <= begin || !text || !info || info[0] != 1) {
                    FAIL("windows: %s: entry %u of the function table", exports[0], count);
                    break;
                }
                /* PUSH RBX, every prologue's first */
                if (text[0] != 0x53) FAIL("windows: %s: entry %u is not where a function starts", exports[0], count);
                /* a block's goes to its failed check's exit: MOV EDX, the block's address */
                text = at_rva(dll.data, end, &left);
                if (k && (!text || text[0] != 0xBA))
                    FAIL("windows: %s: entry %u does not end where its frame does", exports[0], count);
                last = end;
            }
            if (count != (k ? 4 : cg->function_count))
                FAIL("windows: %s: %u entries in its function table", exports[0], count);
            relocs = snx_ld_le32(dll.data + 0x98 + 112 + 44);
            if (!relocs) FAIL("windows: %s: no base relocations", exports[0]);
            free(dll.data);
        }
    snx_x64_set_win64(0);
    for (int k = 0; k < 2; ++k) free(objects[k]), free(descriptors[k]);
    free(made), free(loaded);
}
#endif

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
#if defined(__x86_64__)
    if (snx_x64_available()) {
        size_t before = g_test.failures;
        windows(&cg);
        printf("native_link windows: %s\n", g_test.failures == before ? "ok" : "FAILED");
    }
#endif
    snx_codegen_free(&cg), snx_codegen_free(&cgh);
    /* the four little-endian machines with a descriptor here, their two modules each: all of them where the host's
       modules are ELF, none where its linker says they are not */
    printf("native_link: %zu modules linked, %zu not linked on this host\n", g_test.linked, g_test.unlinked);
#if defined(_WIN32) || defined(__APPLE__)
    if (g_test.linked || g_test.unlinked != 8)
#else
    if (g_test.linked != 8 || g_test.unlinked)
#endif
        FAIL("%zu modules linked and %zu not linked, wanted all 8 of them on the one side", g_test.linked,
             g_test.unlinked);
    return g_test.failures != 0;
}
