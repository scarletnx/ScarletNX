/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The ELF relocations the AOT linker (link_elf.c) and the JIT loader (code_loader.c) share (elf_object.c), checked on
   any host: every AArch64 relocation type and veneer against encodings worked out by hand from the Arm ARM, then
   AArch64 objects from the LLVM backend (when its LLVM has the AArch64 target) linked into a module and loaded into a
   code arena whose code is only read, never run: calls out of reach must land on stubs that reach their targets, and
   GOT slots must hold their symbols' addresses. */
#include "common/utilities/common.h"
#include "common/cpu/recompiler/llvm/elf_object.h"
#include "common/cpu/recompiler/llvm/code_loader.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include <llvm-c/Target.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char* p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

static void check(int ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

/* the instruction insn relocated: whether snx_elf_relocate gave rc and left want */
static void a64(const char* what, uint32_t insn, unsigned type, uint64_t S, int64_t A, uint64_t P, uint64_t slot,
                int rc, uint32_t want) {
    unsigned char p[4];
    int got;
    snx_link_put32(p, insn);
    got = snx_elf_relocate(EM_AARCH64, p, sizeof(p), type, S, A, P, 0, slot);
    if (got != rc || get32(p) != want) {
        printf("FAIL: %s: rc %d (want %d), %08x (want %08x)\n", what, got, rc, (unsigned)get32(p), (unsigned)want);
        ++failures;
    }
}

static void data_relocations(void) {
    unsigned char p[8];
    memset(p, 0xAA, sizeof(p));
    check(!snx_elf_relocate(EM_AARCH64, p, 8, SNX_A64_ABS64, 0x1122334455667788ull, 8, 0, 0, 0) &&
              get64(p) == 0x1122334455667790ull,
          "ABS64");
    check(!snx_elf_relocate(EM_AARCH64, p, 8, SNX_A64_PREL64, 0x20000, 0, 0x10000, 0, 0) && get64(p) == 0x10000,
          "PREL64");
    check(!snx_elf_relocate(EM_AARCH64, p, 8, SNX_A64_PREL64, 0x10000, -8, 0x20000, 0, 0) &&
              get64(p) == (uint64_t)-0x10008,
          "PREL64 backwards");
    memset(p, 0xAA, sizeof(p));
    check(!snx_elf_relocate(EM_AARCH64, p, 4, SNX_A64_PREL32, 0x10000, 0, 0x20000, 0, 0) && get32(p) == 0xFFFF0000u &&
              p[4] == 0xAA,
          "PREL32 backwards, 4 bytes");
    check(snx_elf_relocate(EM_AARCH64, p, 4, SNX_A64_PREL32, 0x80000000ull + 0x1000, 0, 0x1000, 0, 0) == SNX_LINK_RANGE,
          "PREL32 beyond 2 GiB");
    check(snx_elf_relocate(EM_AARCH64, p, 7, SNX_A64_ABS64, 0, 0, 0, 0, 0) == SNX_LINK_OUTSIDE, "ABS64 in 7 bytes");
    check(snx_elf_relocate(EM_AARCH64, p, 3, SNX_A64_CALL26, 0, 0, 0, 0, 0) == SNX_LINK_OUTSIDE, "CALL26 in 3 bytes");
    check(!snx_elf_relocate(EM_AARCH64, p, 0, SNX_A64_NONE, 0, 0, 0, 0, 0), "NONE in no room");
    /* the machine decides what a number means: 1 is R_X86_64_64 but an ILP32 relocation of AArch64's */
    check(snx_elf_relocate(EM_AARCH64, p, 8, 1, 0, 0, 0, 0, 0) == SNX_LINK_UNSUPPORTED, "AArch64 type 1");
    check(snx_elf_relocate(EM_X86_64, p, 8, SNX_A64_ABS64, 0, 0, 0, 0, 0) == SNX_LINK_UNSUPPORTED, "x86-64 type 257");
    /* x86-64's, as before */
    check(!snx_elf_relocate(EM_X86_64, p, 4, R_X86_64_PC32, 0x2000, -4, 0x1000, 0, 0) && get32(p) == 0xFFC,
          "x86-64 PC32");
    check(!snx_elf_relocate(EM_X86_64, p, 4, R_X86_64_REX_GOTPCRELX, 0, -4, 0x1000, 0, 0x3000) && get32(p) == 0x1FFC,
          "x86-64 REX_GOTPCRELX");
    check(snx_elf_relocate(EM_X86_64, p, 4, R_X86_64_PLT32, 0x100001000ull, -4, 0x1000, 0, 0) == SNX_LINK_RANGE,
          "x86-64 PLT32 beyond 2 GiB");
    check(snx_elf_relocate(EM_X86_64, p, 4, R_X86_64_GOTPC64, 0, 0, 0, 0, 0) == SNX_LINK_OUTSIDE,
          "x86-64 GOTPC64 in 4 bytes");
}

static void instruction_relocations(void) {
    const uint64_t P = 0x400123 & ~(uint64_t)3;
    /* ADR x0: immlo at 29, immhi at 5; 0x12345 is immlo 1, immhi 0x48D1 */
    a64("ADR", 0x10000000u, SNX_A64_ADR_PREL_LO21, P + 0x12345, 0, P, 0, 0, 0x30091A20u);
    a64("ADR backwards", 0x10000000u, SNX_A64_ADR_PREL_LO21, P, -4, P, 0, 0, 0x10FFFFE0u);
    a64("ADR beyond 1 MiB", 0x10000000u, SNX_A64_ADR_PREL_LO21, P + 0x100000, 0, P, 0, SNX_LINK_RANGE, 0x10000000u);
    /* ADRP x1: 0x12345000 - 0x400000 is 0x11F45 pages, immlo 1, immhi 0x47D1 */
    a64("ADRP", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21, 0x12345678, 0, P, 0, 0, 0xB008FA21u);
    a64("ADRP addend", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21, 0x12345000, 0x678, P, 0, 0, 0xB008FA21u);
    a64("ADRP NC", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21_NC, 0x12345678, 0, P, 0, 0, 0xB008FA21u);
    /* one page back: -1 pages, all 21 bits set */
    a64("ADRP backwards", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21, P - 0x1000, 0, P, 0, 0, 0xF0FFFFE1u);
    a64("ADRP beyond 4 GiB", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21, P + ((uint64_t)4 << 30), 0, P, 0, SNX_LINK_RANGE,
        0x90000001u);
    /* NC keeps the low 21 bits of the page count: 8 GiB and a page is 0x200001 pages, written as 1 */
    a64("ADRP NC beyond 4 GiB wraps", 0x90000001u, SNX_A64_ADR_PREL_PG_HI21_NC, P + ((uint64_t)8 << 30) + 0x1000, 0,
        P & ~(uint64_t)0xFFF, 0, 0, 0xB0000001u);
    /* ADD x1, x1, #imm12 at 10 */
    a64("ADD lo12", 0x91000021u, SNX_A64_ADD_ABS_LO12_NC, 0x12345678, 0, P, 0, 0, 0x9119E021u);
    /* loads: the low 12 bits scaled by the access size */
    a64("LDRB lo12", 0x39400022u, SNX_A64_LDST8_ABS_LO12_NC, 0x12345679, 0, P, 0, 0, 0x3959E422u);
    a64("LDRH lo12", 0x79400022u, SNX_A64_LDST16_ABS_LO12_NC, 0x1234567A, 0, P, 0, 0, 0x794CF422u);
    a64("LDRH misaligned", 0x79400022u, SNX_A64_LDST16_ABS_LO12_NC, 0x12345679, 0, P, 0, SNX_LINK_MISALIGNED,
        0x79400022u);
    a64("LDR w lo12", 0xB9400022u, SNX_A64_LDST32_ABS_LO12_NC, 0x12345678, 0, P, 0, 0, 0xB9467822u);
    a64("LDR x lo12", 0xF9400022u, SNX_A64_LDST64_ABS_LO12_NC, 0x12345678, 0, P, 0, 0, 0xF9433C22u);
    a64("LDR q lo12", 0x3DC00022u, SNX_A64_LDST128_ABS_LO12_NC, 0x12345670, 0, P, 0, 0, 0x3DC19C22u);
    a64("LDR q misaligned", 0x3DC00022u, SNX_A64_LDST128_ABS_LO12_NC, 0x12345678, 0, P, 0, SNX_LINK_MISALIGNED,
        0x3DC00022u);
    /* GOT: the slot's page (0x7000 - 0x1000: 6 pages, immlo 2, immhi 1) and its offset in 8-byte units */
    a64("ADRP got", 0x90000003u, SNX_A64_ADR_GOT_PAGE, 0, 0, 0x1000, 0x7008, 0, 0xD0000023u);
    a64("LDR got", 0xF9400063u, SNX_A64_LD64_GOT_LO12_NC, 0, 0, 0x1000, 0x7008, 0, 0xF9400463u);
    a64("LDR got misaligned", 0xF9400063u, SNX_A64_LD64_GOT_LO12_NC, 0, 0, 0x1000, 0x7004, SNX_LINK_MISALIGNED,
        0xF9400063u);
    /* branches: imm26 at 0, imm19 and imm14 at 5, in instructions */
    a64("BL", 0x94000000u, SNX_A64_CALL26, P + 0x1000, 0, P, 0, 0, 0x94000400u);
    a64("BL backwards", 0x94000000u, SNX_A64_CALL26, P - 8, 0, P, 0, 0, 0x97FFFFFEu);
    a64("BL at -128 MiB", 0x94000000u, SNX_A64_CALL26, P - ((uint64_t)128 << 20), 0, P, 0, 0, 0x96000000u);
    a64("BL at +128 MiB", 0x94000000u, SNX_A64_CALL26, P + ((uint64_t)128 << 20), 0, P, 0, SNX_LINK_RANGE, 0x94000000u);
    a64("BL misaligned", 0x94000000u, SNX_A64_CALL26, P + 2, 0, P, 0, SNX_LINK_MISALIGNED, 0x94000000u);
    a64("B", 0x14000000u, SNX_A64_JUMP26, P + 4, 0, P, 0, 0, 0x14000001u);
    a64("B.EQ", 0x54000000u, SNX_A64_CONDBR19, P + 0x40, 0, P, 0, 0, 0x54000200u);
    a64("B.EQ backwards", 0x54000000u, SNX_A64_CONDBR19, P - 4, 0, P, 0, 0, 0x54FFFFE0u);
    a64("B.EQ beyond 1 MiB", 0x54000000u, SNX_A64_CONDBR19, P + 0x100000, 0, P, 0, SNX_LINK_RANGE, 0x54000000u);
    a64("CBZ", 0xB4000000u, SNX_A64_CONDBR19, P + 8, 0, P, 0, 0, 0xB4000040u);
    a64("TBZ", 0x36180000u, SNX_A64_TSTBR14, P + 0x20, 0, P, 0, 0, 0x36180100u);
    a64("TBZ at -32 KiB", 0x36180000u, SNX_A64_TSTBR14, P - 0x8000, 0, P, 0, 0, 0x361C0000u);
    a64("TBZ at +32 KiB", 0x36180000u, SNX_A64_TSTBR14, P + 0x8000, 0, P, 0, SNX_LINK_RANGE, 0x36180000u);
}

static void veneers(void) {
    unsigned char v[SNX_ELF_VENEER_SIZE], p[4];
    /* ADRP x16 (0x2345 pages: immlo 1, immhi 0x8D1), ADD x16, x16, #0x678, BR x16 */
    check(!snx_elf_veneer(v, 0x10000000, 0x12345678, 0) && get32(v) == 0xB0011A30u && get32(v + 4) == 0x9119E210u &&
              get32(v + 8) == 0xD61F0200u,
          "direct veneer");
    /* ADRP x16 (3 pages: immlo 3), LDR x16, [x16, #8], BR x16 */
    check(!snx_elf_veneer(v, 0x10000000, 0x10003008, 1) && get32(v) == 0xF0000010u && get32(v + 4) == 0xF9400610u &&
              get32(v + 8) == 0xD61F0200u,
          "indirect veneer");
    check(snx_elf_veneer(v, 0x10000000, 0x10003004, 1) == SNX_LINK_MISALIGNED, "indirect veneer, misaligned slot");
    check(snx_elf_veneer(v, 0x10000000, 0x10000000 + ((uint64_t)5 << 30), 0) == SNX_LINK_RANGE, "veneer beyond 4 GiB");
    snx_link_put32(p, 0x94000000u);
    check(!snx_elf_branch_veneer(p, 0x1000, v, 0x2000, 0x40001234, 0) && get32(p) == 0x94000400u &&
              get32(v + 4) == 0x9108D210u,
          "branch through a veneer");
    snx_link_put32(p, 0x94000000u);
    check(snx_elf_branch_veneer(p, 0x1000, v, 0x1000 + ((uint64_t)200 << 20), 0x40001234, 0) == SNX_LINK_RANGE &&
              get32(p) == 0x94000000u,
          "branch to a veneer out of reach");
}

/* ---- the LLVM backend's AArch64 objects ---- */

static int64_t sign(uint64_t value, unsigned bits) {
    uint64_t m = (uint64_t)1 << (bits - 1);
    return (int64_t)((value ^ m) - m);
}
/* where an ADRP at address points: the page of its target */
static uint64_t adrp_page(uint32_t insn, uint64_t address) {
    uint64_t imm = (insn >> 29 & 3) | (uint64_t)(insn >> 5 & 0x7FFFF) << 2;
    return (address & ~(uint64_t)0xFFF) + (uint64_t)(sign(imm, 21) * 4096);
}
/* where a B or BL at address goes */
static uint64_t branch_target(uint32_t insn, uint64_t address) {
    return address + (uint64_t)(sign(insn & 0x3FFFFFF, 26) * 4);
}
static int is_branch(uint32_t insn) { return (insn & 0x7C000000u) == 0x14000000u; } /* B, BL */

/* where the stub at address jumps: through its GOT slot when *indirect */
static uint64_t stub_target(uint64_t address, int* indirect) {
    const unsigned char* s = (const unsigned char*)(size_t)address;
    uint32_t adrp = get32(s), second = get32(s + 4);
    uint64_t page = adrp_page(adrp, address);
    if ((adrp & 0x9F00001Fu) != 0x90000010u || get32(s + 8) != 0xD61F0200u) return 0;
    *indirect = (second & 0xFFC003FFu) == 0xF9400210u;
    if (*indirect) return page + (second >> 10 & 0xFFF) * 8;
    if ((second & 0xFFC003FFu) != 0x91000210u) return 0;
    return page + (second >> 10 & 0xFFF);
}

/* An AArch64 object: caller() returns callee(*data + 1) + 1; callee is hidden (BL), data default visibility (the GOT) */
static int object(const char* callee_name, const char* data_name, const char* caller_name, unsigned char** data,
                  size_t* size) {
    char error[1024];
    SnxIr* ir = snx_ir_new_target("caller", '2', "aarch64-unknown-linux-gnu", error, sizeof(error));
    LLVMValueRef value, callee, caller, args[1], result;
    LLVMTypeRef params[1];
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_i64;
    value = LLVMAddGlobal(ir->mod, ir->t_i64, data_name);
    callee = ir_extern(ir, callee_name, ir->t_i64, params, 1);
    caller = LLVMAddFunction(ir->mod, caller_name, LLVMFunctionType(ir->t_i64, NULL, 0, 0));
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, caller, "entry"));
    args[0] = LLVMBuildAdd(ir->b, LLVMBuildLoad2(ir->b, ir->t_i64, value, "v"), LLVMConstInt(ir->t_i64, 1, 0), "a");
    result = LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(callee), callee, args, 1, "c");
    LLVMBuildRet(ir->b, LLVMBuildAdd(ir->b, result, LLVMConstInt(ir->t_i64, 1, 0), "r"));
    if (snx_ir_object(ir, '2', data, size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    return 0;
}

/* the definitions object() calls into */
static int definitions(unsigned char** data, size_t* size) {
    char error[1024];
    SnxIr* ir = snx_ir_new_target("definitions", '2', "aarch64-unknown-linux-gnu", error, sizeof(error));
    LLVMValueRef value, callee;
    LLVMTypeRef params[1];
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_i64;
    value = LLVMAddGlobal(ir->mod, ir->t_i64, "ext_data");
    LLVMSetInitializer(value, LLVMConstInt(ir->t_i64, 41, 0));
    callee = ir_extern(ir, "ext_fn", ir->t_i64, params, 1);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, callee, "entry"));
    LLVMBuildRet(ir->b, LLVMBuildMul(ir->b, LLVMGetParam(callee, 0), LLVMConstInt(ir->t_i64, 3, 0), "m"));
    if (snx_ir_object(ir, '2', data, size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    return 0;
}

/* the first B or BL of the function at address, before its RET: its target (0 for none) */
static uint64_t first_branch(uint64_t address) {
    const unsigned char* code = (const unsigned char*)(size_t)address;
    size_t i;
    for (i = 0; i < 64 && get32(code + 4 * i) != 0xD65F03C0u; ++i)
        if (is_branch(get32(code + 4 * i))) return branch_target(get32(code + 4 * i), address + 4 * i);
    return 0;
}
/* the slot the function's first ADRP and LDR pair reads */
static uint64_t first_got_load(uint64_t address) {
    const unsigned char* code = (const unsigned char*)(size_t)address;
    size_t i, j;
    for (i = 0; i < 64; ++i) {
        uint32_t adrp = get32(code + 4 * i);
        if ((adrp & 0x9F000000u) != 0x90000000u) continue;
        for (j = i + 1; j < i + 8 && j < 64; ++j) {
            uint32_t ldr = get32(code + 4 * j);
            if ((ldr & 0xFFC00000u) == 0xF9400000u && (ldr >> 5 & 31) == (adrp & 31))
                return adrp_page(adrp, address + 4 * i) + (ldr >> 10 & 0xFFF) * 8;
        }
    }
    return 0;
}

static SnxU64 probe_data = 0x1234;
static SnxU64 probe_fn(SnxU64 a) { return a; }

static int llvm_objects(void) {
    char error[1024];
    unsigned char *caller_data, *defs_data, *second_data;
    size_t caller_size, defs_size, second_size, i;
    SnxLinkInput inputs[2];
    SnxLinkOutput linked;
    const char* names[1];
    const char* exports[1] = {"caller"};
    SnxU64 caller, second, target, slot;
    SnxCodeLoader* loader;
    int indirect = -1;
    const Elf64_Ehdr* eh;
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    {
        SnxIr* probe = snx_ir_new_target("probe", '2', "aarch64-unknown-linux-gnu", error, sizeof(error));
        if (!probe) {
            printf("skipped the LLVM objects: %s\n", error);
            return 0;
        }
        snx_ir_free(probe);
    }
    if (object("ext_fn", "ext_data", "caller", &caller_data, &caller_size) || definitions(&defs_data, &defs_size) ||
        object("caller", "ext_data", "second", &second_data, &second_size))
        return 1;
    /* AOT: one AArch64 module, rebased with R_AARCH64_RELATIVE (the GOT slot of ext_data at least) */
    inputs[0].data = caller_data;
    inputs[0].size = caller_size;
    inputs[0].name = "caller";
    inputs[1].data = defs_data;
    inputs[1].size = defs_size;
    inputs[1].name = "definitions";
    if (snx_link(inputs, 2, exports, 1, "caller.so", &linked, error, sizeof(error)))
        return snx_error("link: %s", error);
    eh = (const Elf64_Ehdr*)(const void*)linked.data;
    check(eh->e_machine == EM_AARCH64 && eh->e_type == ET_DYN, "AArch64 module header");
    {
        const Elf64_Shdr* sh = (const Elf64_Shdr*)(const void*)(linked.data + eh->e_shoff);
        size_t relatives = 0, others = 0;
        for (i = 0; i < eh->e_shnum; ++i)
            if (sh[i].sh_type == SHT_RELA) {
                const Elf64_Rela* r = (const Elf64_Rela*)(const void*)(linked.data + sh[i].sh_offset);
                size_t k;
                for (k = 0; k < sh[i].sh_size / sizeof(Elf64_Rela); ++k)
                    ELF64_R_TYPE(r[k].r_info) == SNX_A64_RELATIVE ? ++relatives : ++others;
            }
        check(relatives >= 1 && !others, "AArch64 module's dynamic relocations are R_AARCH64_RELATIVE");
    }
    free(linked.data);
    /* JIT: ext_fn and ext_data in this executable, outside the arena: the call through an indirect stub, the data
       through a GOT slot */
    loader = snx_code_loader_new_machine(0, EM_AARCH64, error, sizeof(error));
    if (!loader) return snx_error("loader: %s", error);
    if (snx_code_define(loader, "ext_fn", (SnxU64)(size_t)&probe_fn) ||
        snx_code_define(loader, "ext_data", (SnxU64)(size_t)&probe_data))
        return snx_error("loader: defined twice");
    names[0] = "caller";
    if (snx_code_load(loader, &inputs[0], 1, names, 1, &caller, error, sizeof(error)))
        return snx_error("loader: %s", error);
    target = first_branch(caller);
    if (target == (SnxU64)(size_t)&probe_fn)
        printf("the arena is within 128 MiB of this executable: no far call to check\n");
    else {
        check(target && stub_target(target, &indirect) && indirect == 1, "far call through an indirect stub");
        slot = stub_target(target, &indirect);
        check(slot && get64((const unsigned char*)(size_t)slot) == (SnxU64)(size_t)&probe_fn, "stub's GOT slot");
    }
    slot = first_got_load(caller);
    check(slot && get64((const unsigned char*)(size_t)slot) == (SnxU64)(size_t)&probe_data, "data's GOT slot");
    /* a later unit calling the first, in the arena and in reach: directly */
    inputs[0].data = second_data;
    inputs[0].size = second_size;
    inputs[0].name = "second";
    names[0] = "second";
    if (snx_code_load(loader, &inputs[0], 1, names, 1, &second, error, sizeof(error)))
        return snx_error("loader: %s", error);
    check(first_branch(second) == caller, "near call to an earlier load, direct");
    /* and one whose callee is in the arena but beyond B's 128 MiB: through a direct stub (ADRP, ADD) */
    if (snx_code_define(loader, "far_in_arena", caller + ((SnxU64)300 << 20))) return snx_error("loader: defined");
    free(second_data);
    if (object("far_in_arena", "ext_data", "third", &second_data, &second_size)) return 1;
    inputs[0].data = second_data;
    inputs[0].size = second_size;
    inputs[0].name = "third";
    names[0] = "third";
    if (snx_code_load(loader, &inputs[0], 1, names, 1, &second, error, sizeof(error)))
        return snx_error("loader: %s", error);
    target = first_branch(second);
    check(target && stub_target(target, &indirect) == caller + ((SnxU64)300 << 20) && indirect == 0,
          "call to a far symbol in the arena through a direct stub");
    snx_code_loader_free(loader);
    free(caller_data);
    free(defs_data);
    free(second_data);
    return 0;
}

int main(void) {
    data_relocations();
    instruction_relocations();
    veneers();
    if (llvm_objects()) ++failures;
    if (failures) {
        printf("%d failures\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
