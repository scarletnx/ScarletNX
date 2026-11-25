/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An AOT module's descriptor without LLVM, for a module whose functions a native back end made (module_llvm.c):
   the same symbols and data module_llvm.c's emit_module, emit_routes, emit_nros and emit_imports give, written by
   x64_elf.h's builder, their few functions in the host's own code: x86-64's as bytes (of Windows' calling convention in
   a COFF object where the back end's code is Windows': none keeps a frame, so the unwinder needs no table of them),
   arm64's by the A64 Sail's encoders (a64_enc.inc; the same in a COFF object on Windows), a RISC machine's by its back end (mach/native.h's SnxNativeDesc,
   mach/risc_desc.inc). The data's layout is module_abi.h's and nro_slots.h's C types, the host's own (this is the host's
   module) */
#include "nx/loader/module_x64.h"
#include "nx/loader/module.h"
#include "nx/loader/module_abi.h"
#include "nx/loader/hook_routes.h"
#include "nx/loader/loader.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/recompiler/arm64/arm64_codegen.h"
#include "common/cpu/recompiler/x64/x64_codegen.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/variants.h"
#include "common/runtime/interface.h"
#include "common/utilities/bits.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__) || defined(_WIN32)
#include "common/cpu/recompiler/object/elf_defs.h"
#else
#include <elf.h>
#endif
/* AArch64's relocations (ELF for the Arm 64-bit Architecture, 5.7), which not every <elf.h> names, and x86-64's
   PLT32 */
enum {
    A64_ABS64 = 257,
    A64_ADR_PREL_PG_HI21 = 275,
    A64_ADD_ABS_LO12_NC = 277,
    A64_JUMP26 = 282,
    A64_LDST64_ABS_LO12_NC = 286,
    X64_PLT32 = 4
};
#include "a64_enc.inc"

/* the descriptor's machine: a RISC machine's (g_desc), else arm64's where the arm64 back end made the module (this
   host's, or a Windows one's a test asked for), else x86-64's */
static _Thread_local int g_arm64;
static _Thread_local const SnxNativeDesc* g_desc;
static _Thread_local int g_desc_failed; /* a function of g_desc's out of room */
/* the object for the machine: d's (NULL: this host's x86-64 or arm64) */
/* a function of g_desc's (f, made: 0) as name (flags), its literals' relocations */
static void desc_put(SnxXElf* o, const char* name, int flags, const SnxNativeFn* f, int made) {
    size_t at;
    if (made) {
        g_desc_failed = 1;
        return;
    }
    at = snx_xelf_put(o, SNX_ELF_TEXT, f->code, f->size, 16);
    snx_xelf_symbol(o, name, SNX_ELF_TEXT, at, f->size, flags | SNX_ELF_FUNC);
    for (int k = 0; k < f->nrel; ++k)
        snx_xelf_reloc(o, SNX_ELF_TEXT, at + f->rel[k].at, f->rel[k].sym,
                       f->rel[k].type ? f->rel[k].type : g_desc->literal, f->rel[k].addend);
}
/* it is x86-64's, of Windows' calling convention in a COFF object */
static int win64(void) { return !g_desc && !g_arm64 && snx_x64_win64(); }
const SnxNativeDesc snx_a64_desc = {.name = "arm64", .em = EM_AARCH64};
static SnxXElf* desc_new(const SnxNativeDesc* d) {
    SnxXElf* o = snx_xelf_new();
    const int packed = d == &snx_a64_desc; /* an ELF object whatever this host's own are: a module packed for iOS */
    g_desc_failed = 0;
    g_arm64 = packed || (!d && (snx_arm64_windows() || (!snx_x64_available() && snx_arm64_available())));
    if (g_arm64) d = NULL;
    g_desc = d;
    if (o && (win64() || (g_arm64 && !packed && snx_arm64_windows()))) snx_xelf_set_coff(o, 1);
    if (o && d) {
        snx_xelf_set_machine(o, d->em);
        snx_xelf_set_flags(o, d->elf_flags);
        snx_xelf_set_big(o, d->big);
    } else if (o && g_arm64)
        snx_xelf_set_machine(o, EM_AARCH64);
    return o;
}
/* the object written and freed: 0, or -1 with the reason in error */
static int desc_write(SnxXElf* o, unsigned char** object, size_t* size, char* error, size_t error_size) {
    int rc = g_desc_failed ? -1 : snx_xelf_write(o, object, size);
    snx_xelf_free(o);
    if (rc)
        snprintf(error, error_size, "%s",
                 g_desc_failed ? "a descriptor function too large"
                 : rc == -2    ? "a descriptor's relocation COFF has no form of"
                               : "out of memory");
    return rc ? -1 : 0;
}
static void a64(SnxXElf* o, SnxU32 w) { snx_xelf_put(o, SNX_ELF_TEXT, &w, 4, 1); }
/* ADRP Xd, sym; ADD Xd, Xd, :lo12:sym (+ addend) */
static void a64_adr(SnxXElf* o, unsigned d, const char* sym, long long addend) {
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
    snx_xelf_reloc(o, SNX_ELF_TEXT, at, sym, A64_ADR_PREL_PG_HI21, addend);
    a64(o, sge_a64_adrp(d, 0, 0));
    snx_xelf_reloc(o, SNX_ELF_TEXT, at + 4, sym, A64_ADD_ABS_LO12_NC, addend);
    a64(o, sge_a64_add_addsub_imm(d, d, 0, 0, 1));
}
/* MOVZ and MOVKs of a doubleword */
static void a64_movi(SnxXElf* o, unsigned d, SnxU64 v) {
    unsigned k;
    a64(o, sge_a64_movz(d, (SnxU32)(v & 0xFFFF), 0, 1));
    for (k = 1; k < 4; ++k)
        if ((v >> (16 * k)) & 0xFFFF) a64(o, sge_a64_movk(d, (SnxU32)((v >> (16 * k)) & 0xFFFF), k, 1));
}
/* a word's offset field at at (imm19 at bit 5) to the code here */
static void a64_patch19(SnxXElf* o, size_t at) {
    size_t here = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
    unsigned char* t = snx_xelf_bytes(o, SNX_ELF_TEXT);
    SnxU32 w;
    memcpy(&w, t + at, 4);
    w = (w & ~(0x7FFFFu << 5)) | ((SnxU32)((here - at) / 4) & 0x7FFFFu) << 5;
    memcpy(t + at, &w, 4);
}

/* ---- code bytes into .text ---- */
static void b8(SnxXElf* o, unsigned v) {
    unsigned char c = (unsigned char)v;
    snx_xelf_put(o, SNX_ELF_TEXT, &c, 1, 1);
}
static void b32(SnxXElf* o, SnxU32 v) { snx_xelf_put(o, SNX_ELF_TEXT, &v, 4, 1); }
/* LEA reg, [RIP + sym] (reg 0-15) */
static void lea(SnxXElf* o, int reg, const char* sym) {
    size_t at;
    b8(o, 0x48 | (reg >> 3) << 2);
    b8(o, 0x8D);
    b8(o, 0x05 | (reg & 7) << 3);
    at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    snx_xelf_reloc(o, SNX_ELF_TEXT, at, sym, R_X86_64_PC32, -4);
}
/* a function of flags at the code to come */
static size_t fn_begin(SnxXElf* o, const char* name, int flags) {
    size_t at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 16);
    snx_xelf_symbol(o, name, SNX_ELF_TEXT, at, 0, flags | SNX_ELF_FUNC);
    return at;
}
/* const T* f(void) { return &sym; } */
static void returns(SnxXElf* o, const char* name, const char* sym) {
    if (g_desc) {
        SnxNativeFn f;
        desc_put(o, name, SNX_ELF_GLOBAL, &f, g_desc->returns(&f, sym));
        return;
    }
    fn_begin(o, name, SNX_ELF_GLOBAL);
    if (g_arm64) {
        a64_adr(o, 0, sym, 0);
        a64(o, sge_a64_ret(30));
        return;
    }
    lea(o, 0, sym);
    b8(o, 0xC3);
}
/* a 64-bit pointer at offset of section, to sym */
static void ptr(SnxXElf* o, int section, size_t at, const char* sym) {
    snx_xelf_reloc(o, section, at, sym, g_desc ? g_desc->abs64 : g_arm64 ? A64_ABS64 : R_X86_64_64, 0);
}
/* a string in .rodata, its local symbol name */
static void string(SnxXElf* o, const char* text, const char* name) {
    size_t at = snx_xelf_put(o, SNX_ELF_RODATA, text, strlen(text) + 1, 1);
    snx_xelf_symbol(o, name, SNX_ELF_RODATA, at, strlen(text) + 1, SNX_ELF_LOCAL);
}
static void fn_name(char* out, size_t size, SnxU64 address) {
    snprintf(out, size, "F_%llX", (unsigned long long)address);
}

/* snx_module(): ENT_ADDR[i] = BASE + ENT_OFF[i], ENT_PTR[i] = FNS[ENT_FN[i]] for each entry (as emit_module's loop:
   no relocation for each), then &MOD. ENT_FN is fn_at bytes past ENT_OFF and ENT_PTR ptr_at past ENT_ADDR, which a
   RISC machine's loop reads, and Windows' the first */
static void module_fn(SnxXElf* o, size_t count, size_t fn_at, size_t ptr_at) {
    size_t loop, jae, jmp, done;
    if (g_desc) {
        SnxNativeFn f;
        desc_put(o, "snx_module", SNX_ELF_GLOBAL, &f, g_desc->module_fn(&f, count, SNX_CODE_BASE, fn_at, ptr_at));
        return;
    }
    fn_begin(o, "snx_module", SNX_ELF_GLOBAL);
    if (g_arm64) {
        /* X2-X6 the tables, X7 BASE, W8 the index, W9 count */
        a64_adr(o, 2, "ENT_OFF", 0), a64_adr(o, 3, "ENT_FN", 0), a64_adr(o, 4, "ENT_ADDR", 0);
        a64_adr(o, 5, "ENT_PTR", 0), a64_adr(o, 6, "FNS", 0);
        a64_movi(o, 7, SNX_CODE_BASE);
        a64(o, sge_a64_movz(8, 0, 0, 0));
        a64_movi(o, 9, count);
        loop = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        a64(o, sge_a64_subs_addsub_shift(31, 8, 0, 9, 0, 0)); /* CMP W8, W9 */
        jae = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        a64(o, sge_a64_b_cond(2, 0));                              /* B.HS done */
        a64(o, sge_a64_ldr_reg_gen(10, 2, 1, 2, 8, 2));            /* LDR W10, [X2, W8, UXTW #2] */
        a64(o, sge_a64_add_addsub_shift(10, 10, 0, 7, 0, 1));      /* ADD X10, X10, X7 */
        a64(o, sge_a64_str_reg_gen(10, 4, 1, 2, 8, 3));            /* STR X10, [X4, W8, UXTW #3] */
        a64(o, sge_a64_ldr_reg_gen(10, 3, 1, 2, 8, 2));            /* LDR W10, [X3, W8, UXTW #2] */
        a64(o, sge_a64_ldr_reg_gen(10, 6, 1, 3, 10, 3));           /* LDR X10, [X6, X10, LSL #3] */
        a64(o, sge_a64_str_reg_gen(10, 5, 1, 2, 8, 3));            /* STR X10, [X5, W8, UXTW #3] */
        a64(o, sge_a64_add_addsub_imm(8, 8, 1, 0, 0));             /* ADD W8, W8, #1 */
        jmp = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        a64(o, sge_a64_b_uncond((SnxU32)((long long)(loop - jmp) / 4) & 0x3FFFFFF));
        a64_patch19(o, jae);
        a64_adr(o, 0, "MOD", 0);
        a64(o, sge_a64_ret(30));
        return;
    }
    const int win = win64();
    /* Windows' RSI and RDI are its caller's: RAX, and ENT_FN fn_at past it */
    lea(o, win ? 0 : 6, "ENT_OFF"); /* rsi */
    if (!win) lea(o, 7, "ENT_FN");  /* rdi */
    lea(o, 8, "ENT_ADDR"); /* r8 */
    lea(o, 9, "ENT_PTR");  /* r9 */
    lea(o, 10, "FNS");     /* r10 */
    b8(o, 0x49), b8(o, 0xBB);
    {
        SnxU64 base = SNX_CODE_BASE;
        snx_xelf_put(o, SNX_ELF_TEXT, &base, 8, 1); /* mov r11, BASE */
    }
    b8(o, 0x31), b8(o, 0xC9); /* xor ecx, ecx */
    loop = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
    b8(o, 0x81), b8(o, 0xF9), b32(o, (SnxU32)count); /* cmp ecx, count */
    b8(o, 0x0F), b8(o, 0x83);                        /* jae done */
    jae = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    b8(o, 0x8B), b8(o, 0x14), b8(o, win ? 0x88 : 0x8E); /* mov edx, [rsi + rcx*4] */
    b8(o, 0x4C), b8(o, 0x01), b8(o, 0xDA);            /* add rdx, r11 */
    b8(o, 0x49), b8(o, 0x89), b8(o, 0x14), b8(o, 0xC8); /* mov [r8 + rcx*8], rdx */
    if (win)
        b8(o, 0x8B), b8(o, 0x94), b8(o, 0x88), b32(o, (SnxU32)fn_at); /* mov edx, [rax + rcx*4 + fn_at] */
    else
        b8(o, 0x8B), b8(o, 0x14), b8(o, 0x8F);          /* mov edx, [rdi + rcx*4] */
    b8(o, 0x49), b8(o, 0x8B), b8(o, 0x14), b8(o, 0xD2); /* mov rdx, [r10 + rdx*8] */
    b8(o, 0x49), b8(o, 0x89), b8(o, 0x14), b8(o, 0xC9); /* mov [r9 + rcx*8], rdx */
    b8(o, 0xFF), b8(o, 0xC1);                          /* inc ecx */
    b8(o, 0xE9);                                       /* jmp loop */
    jmp = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    done = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
    lea(o, 0, "MOD");
    b8(o, 0xC3);
    {
        unsigned char* t = snx_xelf_bytes(o, SNX_ELF_TEXT);
        SnxU32 d1 = (SnxU32)(done - (jae + 4)), d2 = (SnxU32)(loop - (jmp + 4));
        memcpy(t + jae, &d1, 4);
        memcpy(t + jmp, &d2, 4);
    }
}

/* name (flags): a tail call through the pointer at sym + addend, read when it runs (an import's IMPORT_FN slot, a
   cached unit's table's) */
static void tail_slot(SnxXElf* o, const char* name, int flags, const char* sym, long long addend) {
    size_t r;
    if (g_desc) {
        SnxNativeFn f;
        desc_put(o, name, flags, &f, g_desc->tail_slot(&f, sym, addend));
        return;
    }
    fn_begin(o, name, flags);
    if (g_arm64) {
        /* ADRP X16, sym + addend; LDR X16, [X16, :lo12:]; BR X16 */
        r = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        snx_xelf_reloc(o, SNX_ELF_TEXT, r, sym, A64_ADR_PREL_PG_HI21, addend);
        a64(o, sge_a64_adrp(16, 0, 0));
        snx_xelf_reloc(o, SNX_ELF_TEXT, r + 4, sym, A64_LDST64_ABS_LO12_NC, addend);
        a64(o, sge_a64_ldr_imm_gen_immediate_unsigned(16, 16, 0, 3));
        a64(o, sge_a64_br(16));
        return;
    }
    b8(o, 0xFF), b8(o, 0x25); /* jmp [rip + sym + addend] */
    r = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    snx_xelf_reloc(o, SNX_ELF_TEXT, r, sym, R_X86_64_PC32, addend - 4);
}
/* name (flags): a tail call of sym (B, jmp) */
static void tail_to(SnxXElf* o, const char* name, int flags, const char* sym) {
    size_t r;
    if (g_desc) {
        SnxNativeFn f;
        desc_put(o, name, flags, &f, g_desc->tail_to(&f, sym));
        return;
    }
    fn_begin(o, name, flags);
    r = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
    if (g_arm64) {
        snx_xelf_reloc(o, SNX_ELF_TEXT, r, sym, A64_JUMP26, 0);
        a64(o, sge_a64_b_uncond(0));
        return;
    }
    b8(o, 0xE9);
    snx_xelf_reloc(o, SNX_ELF_TEXT, r + 1, sym, X64_PLT32, -4);
    snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
}

/* snx_variants(): the profile run's units the module links (the JIT cache's records), each function's leaders and
   runs, its hashes and a tail call through its unit's table (snx_variant_<record>_<index>), as
   module_variants.c's table gives them; the count of functions */
static int variants(SnxXElf* o, const SnxJitCacheRecord* records, size_t record_count, size_t* count) {
    size_t total = 0, i, j, at, k = 0;
    SnxVariantFunction* fns;
    SnxVariants table;
    char name[64];
    *count = 0;
    for (i = 0; i < record_count; ++i) total += records[i].function_count;
    if (!total) return 0;
    if (total > 0xFFFFFFFFu) return -1;
    if (!(fns = (SnxVariantFunction*)calloc(total, sizeof *fns))) return -1;
    for (i = 0; i < record_count; ++i)
        for (j = 0; j < records[i].function_count; ++j, ++k) {
            const SnxJitCacheFunction* f = &records[i].functions[j];
            fns[k].start = f->start;
            fns[k].leader_count = (SnxU32)f->leader_count;
            fns[k].run_count = (SnxU32)f->run_count;
            fns[k].h1 = f->h1;
            fns[k].h2 = f->h2;
        }
    at = snx_xelf_put(o, SNX_ELF_RELRO, fns, total * sizeof *fns, 8);
    snx_xelf_symbol(o, "VARIANT_FNS", SNX_ELF_RELRO, at, total * sizeof *fns, SNX_ELF_LOCAL);
    free(fns);
    for (i = 0, k = 0; i < record_count; ++i)
        for (j = 0; j < records[i].function_count; ++j, ++k) {
            const SnxJitCacheFunction* f = &records[i].functions[j];
            size_t place = at + k * sizeof(SnxVariantFunction), data;
            if (f->leader_count) {
                data = snx_xelf_put(o, SNX_ELF_RODATA, f->leaders, f->leader_count * 8, 8);
                snprintf(name, sizeof name, "VARIANT_L_%zu_%zu", i, j);
                snx_xelf_symbol(o, name, SNX_ELF_RODATA, data, f->leader_count * 8, SNX_ELF_LOCAL);
                ptr(o, SNX_ELF_RELRO, place + offsetof(SnxVariantFunction, leaders), name);
            }
            if (f->run_count) {
                data = snx_xelf_put(o, SNX_ELF_RODATA, f->runs, f->run_count * sizeof *f->runs, 8);
                snprintf(name, sizeof name, "VARIANT_R_%zu_%zu", i, j);
                snx_xelf_symbol(o, name, SNX_ELF_RODATA, data, f->run_count * sizeof *f->runs, SNX_ELF_LOCAL);
                ptr(o, SNX_ELF_RELRO, place + offsetof(SnxVariantFunction, runs), name);
            }
            snprintf(name, sizeof name, "snx_variant_%zu_%zu", i, j);
            tail_slot(o, name, SNX_ELF_LOCAL, records[i].name, (long long)(8 * j));
            ptr(o, SNX_ELF_RELRO, place + offsetof(SnxVariantFunction, fn), name);
        }
    memset(&table, 0, sizeof table);
    table.version = SNX_VARIANTS_VERSION;
    table.count = (SnxU32)total;
    at = snx_xelf_put(o, SNX_ELF_RELRO, &table, sizeof table, 8);
    snx_xelf_symbol(o, "VARIANTS", SNX_ELF_RELRO, at, sizeof table, SNX_ELF_LOCAL);
    ptr(o, SNX_ELF_RELRO, at + offsetof(SnxVariants, functions), "VARIANT_FNS");
    returns(o, "snx_variants", "VARIANTS");
    *count = total;
    return 0;
}

/* snx_import_dyn(c, e): the function dispatch has for e (CPU.disp, inside [code_lo, code_hi) and aligned), else the
   runtime's miss, tail called with c and e: snx_lookup's, as emit_imports' CALLDYN */
static void import_dyn(SnxXElf* o) {
    size_t miss, found, at;
    if (g_desc) {
        SnxNativeFn f;
        desc_put(o, "snx_import_dyn", SNX_ELF_LOCAL, &f, g_desc->import_dyn(&f));
        return;
    }
    fn_begin(o, "snx_import_dyn", SNX_ELF_LOCAL);
    if (g_arm64) {
        /* X0 c, X1 e; X16 and X17 scratch */
        a64(o, sge_a64_ldr_imm_gen_immediate_unsigned(16, 0, (SnxU32)(offsetof(CPU, code_lo) / 8), 3));
        a64(o, sge_a64_ldr_imm_gen_immediate_unsigned(17, 0, (SnxU32)(offsetof(CPU, code_hi) / 8), 3));
        a64(o, sge_a64_sub_addsub_shift(17, 17, 0, 16, 0, 1));
        a64(o, sge_a64_sub_addsub_shift(16, 1, 0, 16, 0, 1));
        a64(o, sge_a64_subs_addsub_shift(31, 16, 0, 17, 0, 1));
        miss = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        a64(o, sge_a64_b_cond(2, 0));                      /* B.HS miss */
        a64(o, sge_a64_ands_log_imm(31, 1, 1, 0, 1, 1));   /* TST X1, #3 */
        at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        a64(o, sge_a64_b_cond(1, 0));                      /* B.NE miss */
        a64(o, sge_a64_ubfm(16, 16, 63, 2, 1, 1));         /* LSR X16, X16, #2 */
        a64(o, sge_a64_ldr_imm_gen_immediate_unsigned(17, 0, (SnxU32)(offsetof(CPU, disp) / 8), 3));
        a64(o, sge_a64_ldr_reg_gen(16, 17, 1, 3, 16, 3));  /* LDR X16, [X17, X16, LSL #3] */
        found = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        a64(o, sge_a64_cbnz(16, 0, 1));
        a64_patch19(o, miss);
        a64_patch19(o, at);
        a64(o, sge_a64_ldr_imm_gen_immediate_unsigned(16, 0, (SnxU32)(offsetof(CPU, rt) / 8), 3));
        a64(o, sge_a64_ldr_imm_gen_immediate_unsigned(16, 16, (SnxU32)(offsetof(RtAPI, miss) / 8), 3));
        a64_patch19(o, found);
        a64(o, sge_a64_br(16));
        return;
    }
    /* c in RDI, e in RSI, RDX scratch; Windows' RCX, RDX and R8 */
    const int win = win64();
    const unsigned c = win ? 0x81 : 0x87, t = win ? 0x81 : 0x97, rt = win ? 0x4C : 0x48;
    b8(o, 0x48), b8(o, 0x89), b8(o, win ? 0xD0 : 0xF0);                         /* mov rax, rsi */
    b8(o, 0x48), b8(o, 0x2B), b8(o, c), b32(o, (SnxU32)offsetof(CPU, code_lo)); /* sub rax, [rdi + lo] */
    b8(o, rt), b8(o, 0x8B), b8(o, t), b32(o, (SnxU32)offsetof(CPU, code_hi));   /* mov rdx, [rdi + hi] */
    b8(o, rt), b8(o, 0x2B), b8(o, t), b32(o, (SnxU32)offsetof(CPU, code_lo));   /* sub rdx, [rdi + lo] */
    b8(o, rt), b8(o, 0x39), b8(o, win ? 0xC0 : 0xD0);                           /* cmp rax, rdx */
    b8(o, 0x0F), b8(o, 0x83);                                                   /* jae miss */
    miss = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    if (win)
        b8(o, 0xF6), b8(o, 0xC2), b8(o, 0x03); /* test dl, 3 */
    else
        b8(o, 0x40), b8(o, 0xF6), b8(o, 0xC6), b8(o, 0x03); /* test sil, 3 */
    b8(o, 0x0F), b8(o, 0x85);                               /* jnz miss */
    at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    b8(o, 0x48), b8(o, 0xC1), b8(o, 0xE8), b8(o, 0x02);                                   /* shr rax, 2 */
    b8(o, rt), b8(o, 0x8B), b8(o, t), b32(o, (SnxU32)offsetof(CPU, disp));                /* mov rdx, [rdi + disp] */
    b8(o, win ? 0x49 : 0x48), b8(o, 0x8B), b8(o, 0x04), b8(o, win ? 0xC0 : 0xC2);         /* mov rax, [rdx + rax*8] */
    b8(o, 0x48), b8(o, 0x85), b8(o, 0xC0);                                                /* test rax, rax */
    b8(o, 0x0F), b8(o, 0x85);                                                             /* jnz found */
    found = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 4, 1);
    {
        size_t here = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        unsigned char* t = snx_xelf_bytes(o, SNX_ELF_TEXT);
        SnxU32 d = (SnxU32)(here - (miss + 4));
        memcpy(t + miss, &d, 4);
        d = (SnxU32)(here - (at + 4));
        memcpy(t + at, &d, 4);
    }
    b8(o, 0x48), b8(o, 0x8B), b8(o, c), b32(o, (SnxU32)offsetof(CPU, rt));        /* mov rax, [rdi + rt] */
    b8(o, 0x48), b8(o, 0x8B), b8(o, 0x80), b32(o, (SnxU32)offsetof(RtAPI, miss)); /* mov rax, [rax + miss] */
    {
        size_t here = snx_xelf_put(o, SNX_ELF_TEXT, NULL, 0, 1);
        unsigned char* t = snx_xelf_bytes(o, SNX_ELF_TEXT);
        SnxU32 d = (SnxU32)(here - (found + 4));
        memcpy(t + found, &d, 4);
    }
    b8(o, 0xFF), b8(o, 0xE0); /* jmp rax */
}

int snx_x64_descriptor(const SnxCodegen* codegen, const SnxCodegenOptions* options, const SnxProgram* loaded,
                       const SnxU64* imports, size_t import_count, const SnxJitCacheRecord* records,
                       size_t record_count, unsigned char** object, size_t* size, size_t* entry_count,
                       size_t* variant_count, char* error, size_t error_size) {
    return snx_x64_descriptor_for(snx_native_desc(), codegen, options, loaded, imports, import_count, records,
                                  record_count, object, size, entry_count, variant_count, error, error_size);
}
int snx_x64_descriptor_for(const SnxNativeDesc* desc, const SnxCodegen* codegen, const SnxCodegenOptions* options,
                           const SnxProgram* loaded, const SnxU64* imports, size_t import_count,
                           const SnxJitCacheRecord* records, size_t record_count, unsigned char** object,
                           size_t* size, size_t* entry_count, size_t* variant_count, char* error, size_t error_size) {
#ifdef _WIN32
    if (desc == &snx_a64_desc) {
        snprintf(error, error_size, "arm64's descriptor is an ELF object, which this host does not write");
        return -1;
    }
#endif
    SnxXElf* o = desc_new(desc);
    SnxModuleEntry* entries;
    size_t count, i, nfns = codegen->function_count ? codegen->function_count : 1, at, fn_at = 0, ptr_at = 0;
    SnxU64 lo = ~(SnxU64)0, hi = 0;
    char name[48];
    if (!o) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    entries = snx_module_entries(codegen, &count);
    for (i = 0; i < count; ++i)
        if (entries[i].address < SNX_CODE_BASE || entries[i].address - SNX_CODE_BASE > 0xFFFFFFFFUL) {
            free(entries);
            snx_xelf_free(o);
            snprintf(error, error_size, "AOT entry exceeds the module ABI's 32-bit offset range");
            return -1;
        }
    /* FNS[]: every function; FN_LO[], FN_LEN[]: the guest code each was made from */
    at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, 8 * nfns, 8);
    snx_xelf_symbol(o, "FNS", SNX_ELF_RELRO, at, 8 * nfns, SNX_ELF_LOCAL);
    for (i = 0; i < codegen->function_count; ++i) {
        fn_name(name, sizeof name, codegen->functions[i].start);
        ptr(o, SNX_ELF_RELRO, at + 8 * i, name);
    }
    {
        SnxU64* fn_lo = (SnxU64*)calloc(nfns, sizeof *fn_lo);
        SnxU32* fn_len = (SnxU32*)calloc(nfns, sizeof *fn_len);
        for (i = 0; i < codegen->function_count; ++i) {
            const SnxFunction* f = &codegen->functions[i];
            SnxU64 first = f->count ? snx_function_address(f, 0) : 0;
            SnxU64 end = f->count ? snx_function_address(f, f->count - 1) + 4 : 0;
            if (end - first > 0xFFFFFFFFu) {
                free(fn_lo), free(fn_len), free(entries);
                snx_xelf_free(o);
                snprintf(error, error_size, "a function's code spans more than the module ABI's 32-bit length");
                return -1;
            }
            fn_lo[i] = first;
            fn_len[i] = (SnxU32)(end - first);
        }
        at = snx_xelf_put(o, SNX_ELF_RODATA, fn_lo, 8 * nfns, 8);
        snx_xelf_symbol(o, "FN_LO", SNX_ELF_RODATA, at, 8 * nfns, SNX_ELF_LOCAL);
        at = snx_xelf_put(o, SNX_ELF_RODATA, fn_len, 4 * nfns, 4);
        snx_xelf_symbol(o, "FN_LEN", SNX_ELF_RODATA, at, 4 * nfns, SNX_ELF_LOCAL);
        free(fn_lo), free(fn_len);
    }
    /* snx_stale[]: each function's byte, the runtime's to set */
    at = snx_xelf_put(o, SNX_ELF_BSS, NULL, nfns, 8);
    snx_xelf_symbol(o, "snx_stale", SNX_ELF_BSS, at, nfns, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN);
    /* ENT_OFF[], ENT_FN[], and ENT_ADDR[], ENT_PTR[] snx_module() fills */
    {
        size_t n = count ? count : 1;
        SnxU32* off = (SnxU32*)calloc(n, 4);
        SnxU32* fn = (SnxU32*)calloc(n, 4);
        for (i = 0; i < count; ++i) {
            off[i] = (SnxU32)(entries[i].address - SNX_CODE_BASE);
            fn[i] = (SnxU32)entries[i].function;
        }
        at = snx_xelf_put(o, SNX_ELF_RODATA, off, 4 * n, 4);
        snx_xelf_symbol(o, "ENT_OFF", SNX_ELF_RODATA, at, 4 * n, SNX_ELF_LOCAL);
        fn_at = snx_xelf_put(o, SNX_ELF_RODATA, fn, 4 * n, 4);
        snx_xelf_symbol(o, "ENT_FN", SNX_ELF_RODATA, fn_at, 4 * n, SNX_ELF_LOCAL);
        fn_at -= at;
        free(off), free(fn);
        at = snx_xelf_put(o, SNX_ELF_BSS, NULL, 8 * n, 8);
        snx_xelf_symbol(o, "ENT_ADDR", SNX_ELF_BSS, at, 8 * n, SNX_ELF_LOCAL);
        ptr_at = snx_xelf_put(o, SNX_ELF_BSS, NULL, 8 * n, 8);
        snx_xelf_symbol(o, "ENT_PTR", SNX_ELF_BSS, ptr_at, 8 * n, SNX_ELF_LOCAL);
        ptr_at -= at;
    }
    /* MODS[]: the modules */
    {
        SnxModInfo* mods = (SnxModInfo*)calloc(loaded->module_count ? loaded->module_count : 1, sizeof *mods);
        for (i = 0; i < loaded->module_count; ++i) {
            const SnxModuleImage* m = &loaded->modules[i];
            mods[i].base = m->base;
            mods[i].size = m->size;
            memcpy(mods[i].build_id, m->build_id, 32);
            mods[i].text_off = m->text_off;
            mods[i].text_size = m->text_size;
            mods[i].ro_off = m->ro_off;
            mods[i].ro_size = m->ro_size;
            mods[i].data_off = m->data_off;
            mods[i].data_size = m->data_size;
            mods[i].bss_size = m->bss;
            mods[i].is_nro = !!m->is_nro;
            if (m->base + m->text_off < lo) lo = m->base + m->text_off;
            if (m->base + m->text_off + m->text_size > hi) hi = m->base + m->text_off + m->text_size;
        }
        at = snx_xelf_put(o, SNX_ELF_RELRO, mods, loaded->module_count * sizeof *mods, 8);
        snx_xelf_symbol(o, "MODS", SNX_ELF_RELRO, at, loaded->module_count * sizeof *mods, SNX_ELF_LOCAL);
        for (i = 0; i < loaded->module_count; ++i) {
            snprintf(name, sizeof name, "MOD_NAME_%lu", (unsigned long)i);
            string(o, loaded->modules[i].name, name);
            ptr(o, SNX_ELF_RELRO, at + i * sizeof *mods + offsetof(SnxModInfo, name), name);
        }
        free(mods);
    }
    /* MOD: the descriptor */
    {
        SnxModule mod;
        const SnxNpdm* npdm = &loaded->npdm;
        memset(&mod, 0, sizeof mod);
        mod.abi = options->abi;
        mod.nmods = (u32)loaded->module_count;
        mod.nentries = (u32)count;
        mod.code_lo = lo;
        mod.code_hi = hi;
        mod.title_id = npdm->title_id;
        mod.main_stack = (u32)npdm->stack;
        mod.main_prio = (u32)npdm->prio;
        mod.main_core = (u32)npdm->core;
        mod.nfns = (u32)codegen->function_count;
        string(o, options->title ? options->title : "Game", "TITLE");
        at = snx_xelf_put(o, SNX_ELF_DATA, &mod, sizeof mod, 8);
        snx_xelf_symbol(o, "MOD", SNX_ELF_DATA, at, sizeof mod, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, mods), "MODS");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, entry_addr), "ENT_ADDR");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, entry_fn), "ENT_PTR");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, title), "TITLE");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, fn), "FNS");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, fn_lo), "FN_LO");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, fn_len), "FN_LEN");
        ptr(o, SNX_ELF_DATA, at + offsetof(SnxModule, stale), "snx_stale");
    }
    module_fn(o, count, fn_at, ptr_at);
    /* snx_aot_<entry>: what the profile run's units' direct calls name (ir_codegen.c), a tail call of the entry's
       function, where there are any */
    for (i = 0; record_count && i < count; ++i) {
        char target[48];
        snprintf(name, sizeof name, "snx_aot_%llx", (unsigned long long)entries[i].address);
        fn_name(target, sizeof target, codegen->functions[entries[i].function].start);
        tail_to(o, name, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN, target);
    }
    if (variants(o, records, record_count, variant_count)) {
        free(entries);
        snx_xelf_free(o);
        snprintf(error, error_size, "the variant table is too large or out of memory");
        return -1;
    }
    /* snx_routes(): the route list's hashes, sorted */
    {
        size_t n = snx_hook_route_count();
        SnxU64* hashes = (SnxU64*)calloc(n ? n : 1, sizeof *hashes);
        SnxRoutes routes;
        for (i = 0; i < n; ++i) hashes[i] = snx_hook_route_hash(snx_hook_route_name(i));
        qsort(hashes, n, sizeof *hashes, snx_compare_u64);
        at = snx_xelf_put(o, SNX_ELF_RODATA, hashes, 8 * n, 8);
        snx_xelf_symbol(o, "ROUTE_NAMES", SNX_ELF_RODATA, at, 8 * n, SNX_ELF_LOCAL);
        free(hashes);
        memset(&routes, 0, sizeof routes);
        routes.version = SNX_ROUTES_VERSION;
        routes.count = (u32)n;
        routes.list = snx_hook_route_list_hash();
        routes.all = options->hook_direct_calls != 0;
        at = snx_xelf_put(o, SNX_ELF_RELRO, &routes, sizeof routes, 8);
        snx_xelf_symbol(o, "ROUTES", SNX_ELF_RELRO, at, sizeof routes, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxRoutes, names), "ROUTE_NAMES");
        returns(o, "snx_routes", "ROUTES");
    }
    /* snx_nros(): where the NROs the program maps go */
    if (options->nro_count) {
        SnxNros nros;
        SnxU64 end = 0;
        for (i = 0; i < loaded->module_count; ++i)
            if (loaded->modules[i].base + loaded->modules[i].size > end)
                end = loaded->modules[i].base + loaded->modules[i].size;
        at = snx_xelf_put(o, SNX_ELF_RODATA, options->nros, options->nro_count * sizeof(SnxNroSlot), 8);
        snx_xelf_symbol(o, "NRO_SLOTS", SNX_ELF_RODATA, at, options->nro_count * sizeof(SnxNroSlot), SNX_ELF_LOCAL);
        memset(&nros, 0, sizeof nros);
        nros.version = SNX_NROS_VERSION;
        nros.count = (u32)options->nro_count;
        nros.lo = end;
        nros.hi = options->nro_end;
        at = snx_xelf_put(o, SNX_ELF_RELRO, &nros, sizeof nros, 8);
        snx_xelf_symbol(o, "NROS", SNX_ELF_RELRO, at, sizeof nros, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxNros, slots), "NRO_SLOTS");
        returns(o, "snx_nros", "NROS");
    }
    /* snx_imports(): the other modules' functions the code calls, each through its pointer */
    if (import_count) {
        SnxNroImports table;
        import_dyn(o);
        at = snx_xelf_put(o, SNX_ELF_DATA, NULL, 8 * import_count, 8);
        snx_xelf_symbol(o, "IMPORT_FN", SNX_ELF_DATA, at, 8 * import_count, SNX_ELF_LOCAL);
        for (i = 0; i < import_count; ++i) ptr(o, SNX_ELF_DATA, at + 8 * i, "snx_import_dyn");
        at = snx_xelf_put(o, SNX_ELF_RODATA, imports, 8 * import_count, 8);
        snx_xelf_symbol(o, "IMPORT_ADDR", SNX_ELF_RODATA, at, 8 * import_count, SNX_ELF_LOCAL);
        for (i = 0; i < import_count; ++i) {
            /* snx_aot_<address>: jmp [IMPORT_FN + 8i] (the pointer the runtime may change, read once) */
            snprintf(name, sizeof name, "snx_aot_%llx", (unsigned long long)imports[i]);
            tail_slot(o, name, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN, "IMPORT_FN", (long long)(8 * i));
        }
        memset(&table, 0, sizeof table);
        table.version = SNX_NRO_IMPORTS_VERSION;
        table.count = (u32)import_count;
        at = snx_xelf_put(o, SNX_ELF_RELRO, &table, sizeof table, 8);
        snx_xelf_symbol(o, "IMPORTS", SNX_ELF_RELRO, at, sizeof table, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxNroImports, address), "IMPORT_ADDR");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxNroImports, function), "IMPORT_FN");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxNroImports, dynamic), "snx_import_dyn");
        returns(o, "snx_imports", "IMPORTS");
    }
    free(entries);
    *entry_count = count;
    return desc_write(o, object, size, error, error_size);
}

/* a 32-bit program's SnxArmModule, as module_arm.c's emit_descriptor gives it: ENT_ADDR the blocks' keys, ENT_FN
   their functions (A_ or T_ and the address), MODS, the title, the thread's settings and snx_arm_poll_offset, the
   module's i64 the runtime writes (hidden: the blocks read it PC-relative), and snx_module_arm() returning &MOD */
int snx_x64_arm_descriptor(const SnxProgram* loaded, const SnxCodegenOptions* options, const SnxU32* keys, size_t count,
                           unsigned char** object, size_t* size, char* error, size_t error_size) {
    return snx_x64_arm_descriptor_for(snx_native_desc(), loaded, options, keys, count, object, size, error, error_size);
}
int snx_x64_arm_descriptor_for(const SnxNativeDesc* desc, const SnxProgram* loaded, const SnxCodegenOptions* options,
                               const SnxU32* keys, size_t count, unsigned char** object, size_t* size, char* error,
                               size_t error_size) {
    SnxXElf* o = desc_new(desc);
    size_t at, n = count ? count : 1;
    char name[48];
    if (!o) {
        snprintf(error, error_size, "out of memory");
        return -1;
    }
    {
        u32* addr = (u32*)calloc(n, sizeof *addr);
        if (!addr) {
            snx_xelf_free(o);
            snprintf(error, error_size, "out of memory");
            return -1;
        }
        for (size_t i = 0; i < count; ++i) addr[i] = keys[i];
        at = snx_xelf_put(o, SNX_ELF_RODATA, addr, n * sizeof *addr, 4);
        snx_xelf_symbol(o, "ENT_ADDR", SNX_ELF_RODATA, at, n * sizeof *addr, SNX_ELF_LOCAL);
        free(addr);
    }
    at = snx_xelf_put(o, SNX_ELF_RELRO, NULL, 8 * n, 8);
    snx_xelf_symbol(o, "ENT_FN", SNX_ELF_RELRO, at, 8 * n, SNX_ELF_LOCAL);
    for (size_t i = 0; i < count; ++i) {
        snprintf(name, sizeof name, "%c_%08x", keys[i] & 1 ? 'T' : 'A', (unsigned)(keys[i] & ~1u));
        ptr(o, SNX_ELF_RELRO, at + 8 * i, name);
    }
    {
        SnxModInfo* mods = (SnxModInfo*)calloc(loaded->module_count ? loaded->module_count : 1, sizeof *mods);
        if (!mods) {
            snx_xelf_free(o);
            snprintf(error, error_size, "out of memory");
            return -1;
        }
        for (size_t i = 0; i < loaded->module_count; ++i) {
            const SnxModuleImage* m = &loaded->modules[i];
            mods[i].base = m->base;
            mods[i].size = m->size;
            memcpy(mods[i].build_id, m->build_id, 32);
            mods[i].text_off = m->text_off;
            mods[i].text_size = m->text_size;
            mods[i].ro_off = m->ro_off;
            mods[i].ro_size = m->ro_size;
            mods[i].data_off = m->data_off;
            mods[i].data_size = m->data_size;
            mods[i].bss_size = m->bss;
            mods[i].is_nro = !!m->is_nro;
        }
        at = snx_xelf_put(o, SNX_ELF_RELRO, mods, loaded->module_count * sizeof *mods, 8);
        snx_xelf_symbol(o, "MODS", SNX_ELF_RELRO, at, loaded->module_count * sizeof *mods, SNX_ELF_LOCAL);
        for (size_t i = 0; i < loaded->module_count; ++i) {
            snprintf(name, sizeof name, "MOD_NAME_%zu", i);
            string(o, loaded->modules[i].name, name);
            ptr(o, SNX_ELF_RELRO, at + i * sizeof *mods + offsetof(SnxModInfo, name), name);
        }
        free(mods);
    }
    {
        i64 zero = 0;
        at = snx_xelf_put(o, SNX_ELF_DATA, &zero, sizeof zero, 8);
        snx_xelf_symbol(o, "snx_arm_poll_offset", SNX_ELF_DATA, at, sizeof zero, SNX_ELF_GLOBAL | SNX_ELF_HIDDEN);
    }
    string(o, options->title ? options->title : "Game", "TITLE");
    {
        SnxArmModule mod = {0};
        const SnxNpdm* npdm = &loaded->npdm;
        mod.abi = SNX_ARM_MODULE_ABI;
        mod.nmods = (u32)loaded->module_count;
        mod.nentries = (u32)count;
        mod.title_id = npdm->title_id;
        mod.main_stack = (u32)npdm->stack;
        mod.main_prio = npdm->prio;
        mod.main_core = npdm->core;
        mod.aspace = npdm->aspace;
        at = snx_xelf_put(o, SNX_ELF_RELRO, &mod, sizeof mod, 8);
        snx_xelf_symbol(o, "MOD", SNX_ELF_RELRO, at, sizeof mod, SNX_ELF_LOCAL);
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, mods), "MODS");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, entry_addr), "ENT_ADDR");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, entry_fn), "ENT_FN");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, title), "TITLE");
        ptr(o, SNX_ELF_RELRO, at + offsetof(SnxArmModule, poll_offset), "snx_arm_poll_offset");
    }
    returns(o, "snx_module_arm", "MOD");
    return desc_write(o, object, size, error, error_size);
}
