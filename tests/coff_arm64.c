/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ARM64 COFF, Windows on Arm's objects, as the AOT linker (link_coff.c) and the JIT loader (code_loader_coff.c) take
   them, checked on any host: every relocation type snx_coff_a64_relocate writes and both veneers, against encodings
   worked out by hand from the Arm ARM, each with the addend COFF keeps in the field; then objects of the LLVM backend
   for aarch64-w64-windows-gnu (when its LLVM has the AArch64 target) linked into a DLL whose header, function table,
   base relocations and calls are read back, again with more code between a call and its target than B and BL reach (the
   call then takes the veneer after its object's code, and a branch with an addend is refused), and loaded into an ARM64
   arena whose code is only read, never run: a call to a table symbol lands on it directly when in reach, else on a stub
   through its slot */
#include "common/utilities/common.h"
#include "common/cpu/recompiler/loader/code_loader.h"
#include "common/cpu/recompiler/object/coff_defs.h"
#include "common/cpu/recompiler/llvm/ir.h"
#include "common/cpu/recompiler/object/link.h"
#include <llvm-c/Target.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BASE ((uint64_t)0x180000000ULL) /* link_coff.c's */

/* the failed checks, and the data the JIT load reads through its slot */
static struct {
    int failures;
    SnxU64 probe_data;
} g_coff_arm64 = {0, 0x1234};

static void check(int ok, const char* what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++g_coff_arm64.failures;
    }
}

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t get64(const unsigned char* p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }
static void put32(unsigned char* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(v >> 8 * i);
}
static void put16(unsigned char* p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

/* insn relocated as type: whether snx_coff_a64_relocate gave rc and, when it wrote, want */
static void a64(const char* what, uint32_t insn, unsigned type, uint64_t S, uint64_t P, uint64_t base, uint64_t start,
                int rc, uint32_t want) {
    unsigned char p[4];
    char line[160];
    put32(p, insn);
    int got = snx_coff_a64_relocate(p, 4, type, S, P, base, start);
    snprintf(line, sizeof line, "%s: rc %d (want %d), %08x (want %08x)", what, got, rc, (unsigned)get32(p),
             (unsigned)(rc ? insn : want));
    check(got == rc && get32(p) == (rc ? insn : want), line);
}

static void relocations(void) {
    unsigned char p[12];
    a64("ADRP to a page ahead", 0x90000000u, REL_A64_PAGEBASE_REL21, 0x12345678, 0x1000, 0, 0, 0, 0x90091A20u);
    a64("ADRP with 0x2000 bytes of addend", 0x90010000u, REL_A64_PAGEBASE_REL21, 0x10000, 0, 0, 0, 0, 0xD0000080u);
    a64("ADRP four pages back", 0x90000000u, REL_A64_PAGEBASE_REL21, 0x1000, 0x5000, 0, 0, 0, 0x90FFFFE0u);
    a64("ADRP at 4 GiB", 0x90000000u, REL_A64_PAGEBASE_REL21, (uint64_t)1 << 32, 0, 0, 0, SNX_LINK_RANGE, 0);
    a64("ADD :lo12:", 0x91000000u, REL_A64_PAGEOFFSET_12A, 0x12345678, 0, 0, 0, 0, 0x9119E000u);
    a64("ADD :lo12: with 8 of addend, into the next page", 0x91002000u, REL_A64_PAGEOFFSET_12A, 0x1FFC, 0, 0, 0, 0,
        0x91001000u);
    a64("LDR x :lo12:, scaled by 8", 0xF9400001u, REL_A64_PAGEOFFSET_12L, 0x12345680, 0, 0, 0, 0, 0xF9434001u);
    a64("LDR x :lo12: with 16 of addend", 0xF9400801u, REL_A64_PAGEOFFSET_12L, 0x1FF8, 0, 0, 0, 0, 0xF9400401u);
    a64("LDR q :lo12:, scaled by 16", 0x3DC00000u, REL_A64_PAGEOFFSET_12L, 0x1230, 0, 0, 0, 0, 0x3DC08C00u);
    a64("LDRB :lo12:, unscaled", 0x39400000u, REL_A64_PAGEOFFSET_12L, 0x1003, 0, 0, 0, 0, 0x39400C00u);
    a64("LDR w :lo12: of an address not 4-aligned", 0xB9400000u, REL_A64_PAGEOFFSET_12L, 0x1002, 0, 0, 0,
        SNX_LINK_MISALIGNED, 0);
    a64("BL ahead", 0x94000000u, REL_A64_BRANCH26, 0x2000, 0x1000, 0, 0, 0, 0x94000400u);
    a64("BL back", 0x94000000u, REL_A64_BRANCH26, 0, 0x1000, 0, 0, 0, 0x97FFFC00u);
    a64("BL at its farthest", 0x94000000u, REL_A64_BRANCH26, 0x1000 + 0x7FFFFFC, 0x1000, 0, 0, 0, 0x95FFFFFFu);
    a64("BL at 128 MiB", 0x94000000u, REL_A64_BRANCH26, 0x1000 + 0x8000000, 0x1000, 0, 0, SNX_LINK_RANGE, 0);
    a64("BL to an address not 4-aligned", 0x94000000u, REL_A64_BRANCH26, 0x2002, 0x1000, 0, 0, SNX_LINK_MISALIGNED, 0);
    a64("B.EQ", 0x54000000u, REL_A64_BRANCH19, 0x1100, 0x1000, 0, 0, 0, 0x54000800u);
    a64("B.EQ at 1 MiB", 0x54000000u, REL_A64_BRANCH19, 0x1000 + 0x100000, 0x1000, 0, 0, SNX_LINK_RANGE, 0);
    a64("TBZ", 0x36000000u, REL_A64_BRANCH14, 0x1020, 0x1000, 0, 0, 0, 0x36000100u);
    a64("TBZ at 32 KiB", 0x36000000u, REL_A64_BRANCH14, 0x1000 + 0x8000, 0x1000, 0, 0, SNX_LINK_RANGE, 0);
    a64("ADR", 0x10000000u, REL_A64_REL21, 0x1005, 0x1000, 0, 0, 0, 0x30000020u);
    a64("ADR at 1 MiB", 0x10000000u, REL_A64_REL21, 0x1000 + 0x100000, 0x1000, 0, 0, SNX_LINK_RANGE, 0);
    a64("ADDR32NB, from the base, 0x10 of addend", 0x10, REL_A64_ADDR32NB, 0x180002000, 0, 0x180000000, 0, 0, 0x2010);
    a64("ADDR32NB past 4 GiB", 0, REL_A64_ADDR32NB, 0x280000000, 0, 0x180000000, 0, SNX_LINK_RANGE, 0);
    a64("SECREL, 4 of addend", 4, REL_A64_SECREL, 0x3010, 0, 0, 0x3000, 0, 0x14);
    a64("ADD :secrel_lo12:", 0x91000000u, REL_A64_SECREL_LOW12A, 0x12345, 0, 0, 0, 0, 0x910D1400u);
    a64("ADD :secrel_hi12:", 0x91400000u, REL_A64_SECREL_HIGH12A, 0x12345, 0, 0, 0, 0, 0x91404800u);
    a64("ADD :secrel_hi12: at 16 MiB", 0x91400000u, REL_A64_SECREL_HIGH12A, 0x1000000, 0, 0, 0, SNX_LINK_RANGE, 0);
    a64("LDR x :secrel_lo12:", 0xF9400000u, REL_A64_SECREL_LOW12L, 0x10348, 0, 0, 0x10000, 0, 0xF941A400u);
    a64("REL32, from the field's end", 0, REL_A64_REL32, 0x2000, 0x1000, 0, 0, 0, 0xFFCu);
    a64("REL32 back", 0, REL_A64_REL32, 0x1000, 0x5000, 0, 0, 0, 0xFFFFBFFCu);
    a64("TOKEN, a type the linkers do not know", 0, 0x0C, 0, 0, 0, 0, SNX_LINK_UNSUPPORTED, 0);
    a64("ABSOLUTE writes nothing", 0x12345678u, REL_A64_ABSOLUTE, 0x1000, 0, 0, 0, 0, 0x12345678u);
    check(snx_coff_a64_relocate(p, 3, REL_A64_BRANCH26, 0x2000, 0x1000, 0, 0) == SNX_LINK_OUTSIDE,
          "a field past its section's end");
    check(!snx_coff_a64_veneer(p, 0x1000, 0x12345678, 0) && get32(p) == 0x90091A30u && get32(p + 4) == 0x9119E210u &&
              get32(p + 8) == 0xD61F0200u,
          "direct veneer: ADRP x16, ADD x16, BR x16");
    check(!snx_coff_a64_veneer(p, 0x1000, 0x12345680, 1) && get32(p) == 0x90091A30u && get32(p + 4) == 0xF9434210u &&
              get32(p + 8) == 0xD61F0200u,
          "indirect veneer: ADRP x16, LDR x16, BR x16");
    check(snx_coff_a64_veneer(p, 0x1000, 0x12345684, 1) == SNX_LINK_MISALIGNED,
          "indirect veneer to a slot not 8-aligned");
    check(snx_coff_a64_veneer(p, 0, (uint64_t)1 << 32, 0) == SNX_LINK_RANGE, "veneer at 4 GiB");
}

/* ---- the LLVM backend's objects ---- */

static int64_t sign(uint64_t value, unsigned bits) {
    uint64_t m = (uint64_t)1 << (bits - 1);
    value &= (m << 1) - 1;
    return (int64_t)((value ^ m) - m);
}
static uint64_t adrp_page(uint32_t insn, uint64_t address) {
    uint64_t imm = (insn >> 29 & 3) | (uint64_t)(insn >> 5 & 0x7FFFF) << 2;
    return (address & ~(uint64_t)0xFFF) + (uint64_t)(sign(imm, 21) * 4096);
}
static int is_branch(uint32_t insn) { return (insn & 0x7C000000u) == 0x14000000u; } /* B, BL */

/* the first B or BL of the code at address, read from code, before its RET: its target (0 for none) */
static uint64_t first_branch(const unsigned char* code, uint64_t address) {
    for (size_t i = 0; i < 64 && get32(code + 4 * i) != 0xD65F03C0u; ++i)
        if (is_branch(get32(code + 4 * i)))
            return address + 4 * i + (uint64_t)(sign(get32(code + 4 * i) & 0x3FFFFFF, 26) * 4);
    return 0;
}
/* the 8-byte slot the code's first ADRP and LDR x pair reads (0 for none) */
static uint64_t first_slot(const unsigned char* code, uint64_t address) {
    for (size_t i = 0; i < 64; ++i) {
        uint32_t adrp = get32(code + 4 * i);
        if ((adrp & 0x9F000000u) != 0x90000000u) continue;
        for (size_t j = i + 1; j < i + 8 && j < 64; ++j) {
            uint32_t ldr = get32(code + 4 * j);
            if ((ldr & 0xFFC00000u) == 0xF9400000u && (ldr >> 5 & 31) == (adrp & 31))
                return adrp_page(adrp, address + 4 * i) + (ldr >> 10 & 0xFFF) * 8;
        }
    }
    return 0;
}
/* where the veneer in v (at address) goes: the slot it reads when *indirect, else its target; 0 for no veneer */
static uint64_t veneer_target(const unsigned char* v, uint64_t address, int* indirect) {
    uint32_t adrp = get32(v), second = get32(v + 4);
    uint64_t page = adrp_page(adrp, address);
    if ((adrp & 0x9F00001Fu) != 0x90000010u || get32(v + 8) != 0xD61F0200u) return 0;
    *indirect = (second & 0xFFC003FFu) == 0xF9400210u;
    if (*indirect) return page + (second >> 10 & 0xFFF) * 8;
    if ((second & 0xFFC003FFu) != 0x91000210u) return 0;
    return page + (second >> 10 & 0xFFF);
}

/* asynchronous unwind tables for function, as the generated code's (ir_codegen.c): its .pdata entry */
static void unwind_table(SnxIr* ir, LLVMValueRef function) {
    unsigned kind = LLVMGetEnumAttributeKindForName("uwtable", 7);
    LLVMAddAttributeAtIndex(function, LLVMAttributeFunctionIndex, LLVMCreateEnumAttribute(ir->ctx, kind, 2));
}

/* An object: caller() returns callee(*data + 1) + 1 */
static int caller_object(const char* triple, const char* callee_name, const char* caller_name, unsigned char** data,
                         size_t* size) {
    char error[1024];
    SnxIr* ir = snx_ir_new_target("caller", '2', triple, error, sizeof(error));
    LLVMTypeRef params[1];
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_i64;
    LLVMValueRef value = LLVMAddGlobal(ir->mod, ir->t_i64, "ext_data");
    LLVMValueRef callee = ir_extern(ir, callee_name, ir->t_i64, params, 1);
    LLVMValueRef caller = LLVMAddFunction(ir->mod, caller_name, LLVMFunctionType(ir->t_i64, NULL, 0, 0));
    unwind_table(ir, caller);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, caller, "entry"));
    LLVMValueRef args[1] = {
        LLVMBuildAdd(ir->b, LLVMBuildLoad2(ir->b, ir->t_i64, value, "v"), LLVMConstInt(ir->t_i64, 1, 0), "a")};
    LLVMValueRef result = LLVMBuildCall2(ir->b, LLVMGlobalGetValueType(callee), callee, args, 1, "c");
    LLVMBuildRet(ir->b, LLVMBuildAdd(ir->b, result, LLVMConstInt(ir->t_i64, 1, 0), "r"));
    if (snx_ir_object(ir, '2', data, size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    return 0;
}
/* what caller_object calls into: ext_data, 41, and ext_fn(x), x * 3 */
static int definitions_object(const char* triple, unsigned char** data, size_t* size) {
    char error[1024];
    SnxIr* ir = snx_ir_new_target("definitions", '2', triple, error, sizeof(error));
    LLVMTypeRef params[1];
    if (!ir) return snx_error("%s", error);
    params[0] = ir->t_i64;
    LLVMValueRef value = LLVMAddGlobal(ir->mod, ir->t_i64, "ext_data");
    LLVMSetInitializer(value, LLVMConstInt(ir->t_i64, 41, 0));
    LLVMValueRef callee = ir_extern(ir, "ext_fn", ir->t_i64, params, 1);
    unwind_table(ir, callee);
    LLVMPositionBuilderAtEnd(ir->b, LLVMAppendBasicBlockInContext(ir->ctx, callee, "entry"));
    LLVMBuildRet(ir->b, LLVMBuildMul(ir->b, LLVMGetParam(callee, 0), LLVMConstInt(ir->t_i64, 3, 0), "m"));
    if (snx_ir_object(ir, '2', data, size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    return 0;
}
/* an object of nothing but bytes of code in .text, module assembly */
static int filler_object(const char* triple, size_t bytes, unsigned char** data, size_t* size) {
    char error[1024], text[64];
    SnxIr* ir = snx_ir_new_target("filler", '2', triple, error, sizeof(error));
    if (!ir) return snx_error("%s", error);
    snprintf(text, sizeof text, ".text\n.space %zu\n", bytes);
    LLVMSetModuleInlineAsm2(ir->mod, text, strlen(text));
    if (snx_ir_object(ir, '2', data, size, error, sizeof(error))) return snx_error("%s", error);
    snx_ir_free(ir);
    return 0;
}

/* a PE image as link_coff.c writes it, read back */
typedef struct {
    const unsigned char* data;
    size_t size;
    const unsigned char *coff, *optional, *sections;
    unsigned nsections;
} Pe;

static int pe_open(Pe* pe, const unsigned char* data, size_t size) {
    pe->data = data;
    pe->size = size;
    if (size < 0x40 || data[0] != 'M' || data[1] != 'Z') return -1;
    uint32_t at = get32(data + 0x3C);
    if (at + 24 + 240 > size || memcmp(data + at, "PE\0\0", 4)) return -1;
    pe->coff = data + at + 4;
    pe->optional = pe->coff + 20;
    pe->nsections = pe->coff[2] | pe->coff[3] << 8;
    pe->sections = pe->optional + (pe->coff[16] | pe->coff[17] << 8);
    return (size_t)(pe->sections + 40 * pe->nsections - data) <= size ? 0 : -1;
}
/* the image's bytes at rva, need of them in one section's file data; NULL when there are not */
static const unsigned char* pe_at(const Pe* pe, uint32_t rva, uint32_t need) {
    for (unsigned i = 0; i < pe->nsections; ++i) {
        const unsigned char* s = pe->sections + 40 * i;
        uint32_t va = get32(s + 12), raw_size = get32(s + 16), raw = get32(s + 20);
        if (rva >= va && rva - va + (uint64_t)need <= raw_size && raw + (uint64_t)raw_size <= pe->size)
            return pe->data + raw + (rva - va);
    }
    return NULL;
}
/* data directory index's RVA and size */
static uint32_t pe_directory(const Pe* pe, unsigned index, uint32_t* size) {
    *size = get32(pe->optional + 112 + 8 * index + 4);
    return get32(pe->optional + 112 + 8 * index);
}
/* the RVA the export directory gives name, 0 for none */
static uint32_t pe_export(const Pe* pe, const char* name) {
    uint32_t size, dir = pe_directory(pe, 0, &size);
    const unsigned char* e = pe_at(pe, dir, 40);
    if (!e) return 0;
    uint32_t count = get32(e + 24);
    const unsigned char *functions = pe_at(pe, get32(e + 28), 4 * count), *names = pe_at(pe, get32(e + 32), 4 * count),
                        *ordinals = pe_at(pe, get32(e + 36), 2 * count);
    if (!functions || !names || !ordinals) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* n = pe_at(pe, get32(names + 4 * i), (uint32_t)strlen(name) + 1);
        if (n && !memcmp(n, name, strlen(name) + 1))
            return get32(functions + 4 * (ordinals[2 * i] | ordinals[2 * i + 1] << 8));
    }
    return 0;
}
/* whether .reloc has a DIR64 entry for the 8 bytes at rva */
static int pe_rebased(const Pe* pe, uint32_t rva) {
    uint32_t size, dir = pe_directory(pe, 5, &size);
    const unsigned char* r = pe_at(pe, dir, size);
    for (uint32_t at = 0; r && at + 8 <= size;) {
        uint32_t page = get32(r + at), block = get32(r + at + 4);
        if (block < 8) return 0;
        for (uint32_t k = 8; k + 2 <= block; k += 2) {
            unsigned entry = r[at + k] | r[at + k + 1] << 8;
            if (entry >> 12 == 10 && page + (entry & 0xFFF) == rva) return 1;
        }
        at += block;
    }
    return 0;
}

/* AOT: caller and definitions into one DLL, read back */
static void module(const unsigned char* caller, size_t caller_size, const unsigned char* defs, size_t defs_size) {
    static const char* const exports[] = {"caller", "ext_fn", "ext_data"};
    SnxLinkInput inputs[2] = {{caller, caller_size, "caller"}, {defs, defs_size, "definitions"}};
    SnxLinkOutput linked;
    char error[1024];
    Pe pe;
    if (snx_link(inputs, 2, exports, 3, "caller.dll", &linked, error, sizeof(error))) {
        check(0, error);
        return;
    }
    if (pe_open(&pe, linked.data, linked.size)) {
        check(0, "the DLL's headers");
        free(linked.data);
        return;
    }
    check((pe.coff[0] | pe.coff[1] << 8) == COFF_ARM64, "the DLL's machine is ARM64");
    uint32_t fn = pe_export(&pe, "ext_fn"), call = pe_export(&pe, "caller"), data = pe_export(&pe, "ext_data");
    const unsigned char* code = pe_at(&pe, call, 256);
    check(fn && call && data && code, "the DLL exports caller, ext_fn and ext_data");
    if (code) {
        check(first_branch(code, call) == fn, "caller's BL reaches ext_fn");
        uint64_t slot = first_slot(code, call);
        const unsigned char* held = slot ? pe_at(&pe, (uint32_t)slot, 8) : NULL;
        check(held && get64(held) == IMAGE_BASE + data, "caller's .refptr slot holds ext_data's address");
        check(slot && pe_rebased(&pe, (uint32_t)slot), "the slot is rebased (.reloc DIR64)");
    }
    uint32_t size, dir = pe_directory(&pe, 3, &size);
    const unsigned char* table = pe_at(&pe, dir, size);
    int sorted = table && size && size % 8 == 0, has_call = 0;
    for (uint32_t i = 0; table && i + 8 <= size; i += 8) {
        if (i && get32(table + i) <= get32(table + i - 8)) sorted = 0;
        has_call |= get32(table + i) == call;
    }
    /* ext_fn, a leaf that saves nothing, needs no entry: the unwinder takes a function without one for that */
    check(sorted && has_call, "the function table: 8-byte entries, sorted, caller's among them");
    free(linked.data);
}

/* An ARM64 object written by hand, as LLVM writes none: two_call() is BL ext_fn, then BL ext_fn+8, its addend in the
   field as COFF keeps it. The header, .text's section header, its 8 bytes and 2 relocations, 2 symbols and an empty
   string table */
#define TWO_CALLS_SIZE 128
static void two_calls_object(unsigned char* o) {
    memset(o, 0, TWO_CALLS_SIZE);
    put16(o, COFF_ARM64);
    put16(o + 2, 1);  /* sections */
    put32(o + 8, 88); /* the symbol table */
    put32(o + 12, 2); /* symbols */
    memcpy(o + 20, ".text", 5);
    put32(o + 36, 8);  /* its raw data's size */
    put32(o + 40, 60); /* where it is */
    put32(o + 44, 68); /* its relocations */
    put16(o + 52, 2);
    put32(o + 56, 0x60500020u); /* code, executable, readable, 16-byte aligned */
    put32(o + 60, 0x94000000u); /* BL ext_fn */
    put32(o + 64, 0x94000002u); /* BL ext_fn+8 */
    /* BRANCH26 to symbol 1, at 0 and 4 */
    for (unsigned i = 0; i < 2; ++i) {
        put32(o + 68 + 10 * i, 4 * i);
        put32(o + 72 + 10 * i, 1);
        put16(o + 76 + 10 * i, REL_A64_BRANCH26);
    }
    memcpy(o + 88, "two_call", 8); /* defined in .text, external, a function */
    put16(o + 100, 1);
    put16(o + 102, 0x20);
    o[104] = 2;
    memcpy(o + 106, "ext_fn", 6); /* undefined, external */
    put16(o + 120, 0x20);
    o[122] = 2;
    put32(o + 124, 4); /* the string table: its size alone */
}

/* AOT: more code between caller and ext_fn than BL reaches: the call takes the veneer after caller's code. A branch
   whose field holds an addend is refused: the one veneer of its symbol is the other branches' too */
static void large_module(const unsigned char* caller, size_t caller_size, const unsigned char* defs, size_t defs_size,
                         const char* triple) {
    static const char* const exports[] = {"caller", "ext_fn"};
    unsigned char* filler;
    size_t filler_size;
    SnxLinkOutput linked;
    char error[1024];
    Pe pe;
    int indirect = -1;
    if (filler_object(triple, (size_t)140 << 20, &filler, &filler_size)) {
        check(0, "the filler object");
        return;
    }
    SnxLinkInput inputs[3] = {
        {caller, caller_size, "caller"}, {filler, filler_size, "filler"}, {defs, defs_size, "definitions"}};
    if (snx_link(inputs, 3, exports, 2, "large.dll", &linked, error, sizeof(error))) {
        check(0, error);
        free(filler);
        return;
    }
    if (pe_open(&pe, linked.data, linked.size))
        check(0, "the large DLL's headers");
    else {
        uint32_t fn = pe_export(&pe, "ext_fn"), call = pe_export(&pe, "caller");
        const unsigned char* code = pe_at(&pe, call, 256);
        uint64_t to = code ? first_branch(code, call) : 0;
        const unsigned char* veneer = to ? pe_at(&pe, (uint32_t)to, SNX_COFF_A64_VENEER_SIZE) : NULL;
        check(fn > call + ((uint32_t)128 << 20), "ext_fn beyond BL's reach of caller");
        check(veneer && to < (uint64_t)call + ((uint32_t)1 << 20) && veneer_target(veneer, to, &indirect) == fn &&
                  !indirect,
              "caller's BL takes the veneer after its code, which goes to ext_fn");
    }
    free(linked.data);
    unsigned char two[TWO_CALLS_SIZE];
    static const char* const two_exports[] = {"two_call", "ext_fn"};
    two_calls_object(two);
    inputs[0] = (SnxLinkInput){two, sizeof two, "two_calls"};
    if (!snx_link(inputs, 3, two_exports, 2, "two.dll", &linked, error, sizeof(error))) {
        check(0, "BL ext_fn+8 beyond BL's reach linked, through the veneer BL ext_fn takes");
        free(linked.data);
    } else
        check(strstr(error, "two_calls: ARM64 relocation 3 at .text+0x4 to RVA") &&
                  strstr(error, "out of the instruction's reach, and a branch with an addend takes no veneer"),
              error);
    free(filler);
}

static SnxU64 probe_fn(SnxU64 a) { return a; }

/* JIT: the objects loaded into an ARM64 arena, read and never run */
static void jit(const char* triple, const unsigned char* caller_data, size_t caller_size) {
    char error[1024];
    unsigned char* data;
    size_t size;
    const char* names[1] = {"caller"};
    SnxU64 caller, other;
    int indirect = -1;
    SnxCodeLoader* loader = snx_code_loader_new_machine(0, SNX_CODE_COFF_ARM64, error, sizeof(error));
    if (!loader) {
        check(0, error);
        return;
    }
    if (snx_code_define(loader, "ext_fn", (SnxU64)(size_t)&probe_fn) ||
        snx_code_define(loader, "ext_data", (SnxU64)(size_t)&g_coff_arm64.probe_data)) {
        check(0, "ext_fn and ext_data defined in the loader");
        snx_code_loader_free(loader);
        return;
    }
    SnxLinkInput input = {caller_data, caller_size, "caller"};
    if (snx_code_load(loader, &input, 1, names, 1, &caller, error, sizeof(error))) {
        check(0, error);
        snx_code_loader_free(loader);
        return;
    }
    const unsigned char* code = (const unsigned char*)(size_t)caller;
    uint64_t target = first_branch(code, caller), slot;
    if (target == (SnxU64)(size_t)&probe_fn)
        printf("the arena is within 128 MiB of this executable: no stub to check\n");
    else {
        slot = target ? veneer_target((const unsigned char*)(size_t)target, target, &indirect) : 0;
        check(slot && indirect == 1 && get64((const unsigned char*)(size_t)slot) == (SnxU64)(size_t)&probe_fn,
              "the call to ext_fn, outside the arena, through a stub and its slot");
    }
    slot = first_slot(code, caller);
    check(slot && get64((const unsigned char*)(size_t)slot) == (SnxU64)(size_t)&g_coff_arm64.probe_data,
          "the .refptr slot of ext_data holds its address");
    /* a later load calling the first, in reach: directly */
    if (caller_object(triple, "caller", "second", &data, &size)) return;
    input = (SnxLinkInput){data, size, "second"};
    names[0] = "second";
    if (snx_code_load(loader, &input, 1, names, 1, &other, error, sizeof(error)))
        check(0, error);
    else
        check(first_branch((const unsigned char*)(size_t)other, other) == caller, "a call to an earlier load, direct");
    free(data);
    /* and one to a symbol in the arena beyond BL's reach: through its stub */
    if (snx_code_define(loader, "far_in_arena", caller + ((SnxU64)300 << 20))) check(0, "far_in_arena defined");
    if (caller_object(triple, "far_in_arena", "third", &data, &size)) return;
    input = (SnxLinkInput){data, size, "third"};
    names[0] = "third";
    if (snx_code_load(loader, &input, 1, names, 1, &other, error, sizeof(error)))
        check(0, error);
    else {
        target = first_branch((const unsigned char*)(size_t)other, other);
        slot = target ? veneer_target((const unsigned char*)(size_t)target, target, &indirect) : 0;
        check(slot && get64((const unsigned char*)(size_t)slot) == caller + ((SnxU64)300 << 20),
              "a call 300 MiB away in the arena, through a stub and its slot");
    }
    free(data);
    snx_code_loader_free(loader);
}

static int llvm_objects(void) {
    static const char* const triple = "aarch64-w64-windows-gnu";
    char error[1024];
    unsigned char *caller, *defs, *x64;
    size_t caller_size, defs_size, x64_size;
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    LLVMInitializeAllAsmParsers(); /* the filler is module assembly */
    if (snx_ir_init(error, sizeof(error))) return snx_error("%s", error);
    SnxIr* probe = snx_ir_new_target("probe", '2', triple, error, sizeof(error));
    if (!probe) {
        printf("skipped the LLVM objects: %s\n", error);
        return 0;
    }
    snx_ir_free(probe);
    if (caller_object(triple, "ext_fn", "caller", &caller, &caller_size) ||
        definitions_object(triple, &defs, &defs_size))
        return 1;
    check(caller_size > 2 && (caller[0] | caller[1] << 8) == COFF_ARM64,
          "LLVM writes ARM64 COFF for aarch64-w64-windows-gnu");
    module(caller, caller_size, defs, defs_size);
    large_module(caller, caller_size, defs, defs_size, triple);
    jit(triple, caller, caller_size);
    /* objects of two machines in one module: refused, saying the first's */
    if (!definitions_object("x86_64-w64-windows-gnu", &x64, &x64_size)) {
        SnxLinkInput inputs[2] = {{caller, caller_size, "caller"}, {x64, x64_size, "x86-64"}};
        static const char* const exports[] = {"caller"};
        SnxLinkOutput linked;
        check(snx_link(inputs, 2, exports, 1, "mixed.dll", &linked, error, sizeof(error)) &&
                  strstr(error, "x86-64: not of the machine of the first object (ARM64)"),
              "an x86-64 object after an ARM64 one refused, naming the first's machine");
        free(x64);
    }
    free(caller);
    free(defs);
    return 0;
}

int main(void) {
    relocations();
    if (llvm_objects()) return 1;
    printf("coff_arm64: %s\n", g_coff_arm64.failures ? "FAILED" : "passed");
    return g_coff_arm64.failures ? 1 : 0;
}
