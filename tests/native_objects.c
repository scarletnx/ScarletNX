/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* native_objects.h's objects, made with the native back ends' object builder (x64_elf.h) */
#include "native_objects.h"
#include "common/cpu/recompiler/mach/native.h"
#include "common/cpu/recompiler/x64/x64_elf.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* <elf.h>'s, which this file can't include beside x64_elf.h */
enum { EM_X64 = NOBJ_X86_64, EM_A64 = NOBJ_AARCH64 };
enum { X64_64 = 1, X64_PC32 = 2, X64_PLT32 = 4 };
enum { A64_ABS64 = 257, A64_ADR_PREL_PG_HI21 = 275, A64_CALL26 = 283, A64_LDST64_ABS_LO12_NC = 286 };

static SnxXElf* begin(unsigned machine, int coff, char* error, size_t error_size) {
    SnxXElf* o = snx_xelf_new();
    if (!o) {
        snprintf(error, error_size, "no object: out of memory");
        return NULL;
    }
    snx_xelf_set_coff(o, coff);
    if (machine != EM_X64) snx_xelf_set_machine(o, machine);
    return o;
}
static int end(SnxXElf* o, const char* what, unsigned char** out, size_t* size, char* error, size_t error_size) {
    const int rc = snx_xelf_write(o, out, size);
    snx_xelf_free(o);
    if (rc)
        snprintf(error, error_size, "%s: %s", what, rc == -1 ? "out of memory" : "a relocation COFF has no form of");
    return rc ? -1 : 0;
}
/* words of code, little-endian (AArch64's) or in the machine's order */
static size_t put_words(SnxXElf* o, const uint32_t* words, size_t n, int big) {
    unsigned char bytes[64];
    for (size_t i = 0; i < n; ++i)
        for (unsigned b = 0; b < 4; ++b) bytes[4 * i + b] = (unsigned char)(words[i] >> (big ? 24 - 8 * b : 8 * b));
    return snx_xelf_put(o, SNX_ELF_TEXT, bytes, 4 * n, 16);
}
/* a slot of the object's holding sym's address, read-only once relocated, named name */
static void slot(SnxXElf* o, const char* name, const char* sym, unsigned abs64) {
    const uint64_t zero = 0;
    const size_t at = snx_xelf_put(o, SNX_ELF_RELRO, &zero, sizeof zero, 8);
    snx_xelf_symbol(o, name, SNX_ELF_RELRO, at, sizeof zero, SNX_ELF_LOCAL);
    snx_xelf_reloc(o, SNX_ELF_RELRO, at, sym, abs64, 0);
}
static void slot_name(char* out, size_t size, const char* sym) { snprintf(out, size, "%s.slot", sym); }

/* stp x29, x30, [sp, #-32]!; mov x29, sp; stp x19, x20, [sp, #16]; sub sp, sp, #16: a frame as arm64_codegen.c's
   prologue makes one, which x64_elf.c's unwind data reads. Its epilogue, add sp, sp, #16; ldp x19, x20, [sp, #16];
   ldp x29, x30, [sp], #32, comes at once before its ret */
#define A64_PROLOGUE 0xa9be7bfdu, 0x910003fdu, 0xa90153f3u, 0xd10043ffu
#define A64_EPILOGUE 0x910043ffu, 0xa94153f3u, 0xa8c27bfdu

int nobj_a64_caller(int coff, const char* callee, const char* data, const char* caller, unsigned char** out,
                    size_t* size, char* error, size_t error_size) {
    /* the frame; adrp x8, slot; ldr x8, [x8, :lo12:slot]; ldr x0, [x8]; add x0, x0, #1; bl callee; add x0, x0, #1;
       the epilogue; ret */
    static const uint32_t code[] = {A64_PROLOGUE, 0x90000008u, 0xf9400108u,  0xf9400100u, 0x91000400u,
                                    0x94000000u,  0x91000400u, A64_EPILOGUE, 0xd65f03c0u};
    enum { ADRP = 4, LDR = 5, BL = 8 };
    char name[64];
    SnxXElf* o = begin(EM_A64, coff, error, error_size);
    if (!o) return -1;
    slot_name(name, sizeof name, data);
    slot(o, name, data, A64_ABS64);
    const size_t at = put_words(o, code, sizeof code / sizeof *code, 0);
    snx_xelf_symbol(o, caller, SNX_ELF_TEXT, at, sizeof code, SNX_ELF_GLOBAL | SNX_ELF_FUNC);
    snx_xelf_reloc(o, SNX_ELF_TEXT, at + 4 * ADRP, name, A64_ADR_PREL_PG_HI21, 0);
    snx_xelf_reloc(o, SNX_ELF_TEXT, at + 4 * LDR, name, A64_LDST64_ABS_LO12_NC, 0);
    snx_xelf_reloc(o, SNX_ELF_TEXT, at + 4 * BL, callee, A64_CALL26, 0);
    snx_xelf_unwind(o, at, at + sizeof code, snx_xelf_bytes(o, SNX_ELF_TEXT) + at, sizeof code);
    return end(o, caller, out, size, error, error_size);
}

int nobj_definitions(unsigned machine, int coff, unsigned char** out, size_t* size, char* error, size_t error_size) {
    static const uint32_t a64[] = {0x8b000400u, 0xd65f03c0u};              /* add x0, x0, x0, lsl #1; ret */
    static const unsigned char x64[] = {0x48, 0x8d, 0x04, 0x7f, 0xc3};     /* lea rax, [rdi + rdi * 2]; ret */
    static const unsigned char x64_win[] = {0x48, 0x8d, 0x04, 0x49, 0xc3}; /* lea rax, [rcx + rcx * 2]; ret */
    const uint64_t value = 41;
    SnxXElf* o = begin(machine, coff, error, error_size);
    if (!o) return -1;
    const size_t fn = machine == EM_A64 ? put_words(o, a64, 2, 0)
                                        : snx_xelf_put(o, SNX_ELF_TEXT, coff ? x64_win : x64, sizeof x64, 16);
    snx_xelf_symbol(o, "ext_fn", SNX_ELF_TEXT, fn, machine == EM_A64 ? sizeof a64 : sizeof x64,
                    SNX_ELF_GLOBAL | SNX_ELF_FUNC);
    const size_t at = snx_xelf_put(o, SNX_ELF_DATA, &value, sizeof value, 8);
    snx_xelf_symbol(o, "ext_data", SNX_ELF_DATA, at, sizeof value, SNX_ELF_GLOBAL);
    return end(o, "definitions", out, size, error, error_size);
}

int nobj_a64_reach(const char* callee, size_t bytes, unsigned char** out, size_t* size, char* error,
                   size_t error_size) {
    static const uint32_t code[] = {0x94000000u, 0xd65f03c0u}; /* bl callee; ret */
    SnxXElf* o = begin(EM_A64, 0, error, error_size);
    if (!o) return -1;
    const size_t at = put_words(o, code, sizeof code / sizeof *code, 0);
    snx_xelf_symbol(o, "reach", SNX_ELF_TEXT, at, sizeof code, SNX_ELF_GLOBAL | SNX_ELF_FUNC);
    snx_xelf_reloc(o, SNX_ELF_TEXT, at, callee, A64_CALL26, 0);
    snx_xelf_put(o, SNX_ELF_TEXT, NULL, bytes, 16);
    return end(o, "reach", out, size, error, error_size);
}

int nobj_filler(unsigned machine, int coff, size_t bytes, unsigned char** out, size_t* size, char* error,
                size_t error_size) {
    SnxXElf* o = begin(machine, coff, error, error_size);
    if (!o) return -1;
    const size_t at = snx_xelf_put(o, SNX_ELF_TEXT, NULL, bytes, 16);
    snx_xelf_symbol(o, "filler", SNX_ELF_TEXT, at, bytes, SNX_ELF_LOCAL | SNX_ELF_FUNC);
    return end(o, "filler", out, size, error, error_size);
}

int nobj_need(unsigned machine, const char* call1, const char* call2, const char* data1, const char* data2,
              unsigned char** out, size_t* size, char* error, size_t error_size) {
    /* adrp x8, slot; ldr x8, [x8, :lo12:slot]; ldr x8, [x8]; add x0, x0, x8; stp x29, x30, [sp, #-16]!;
       mov x29, sp; bl call; ldp x29, x30, [sp], #16; add x0, x0, #1; ret */
    static const uint32_t a64[] = {0x90000008u, 0xf9400108u, 0xf9400108u, 0x8b080000u, 0xa9bf7bfdu,
                                   0x910003fdu, 0x94000000u, 0xa8c17bfdu, 0x91000400u, 0xd65f03c0u};
    /* mov rax, [rip + slot]; mov rax, [rax]; add rdi, rax; call call; add rax, 1; ret */
    static const unsigned char x64[] = {0x48, 0x8b, 0x05, 0, 0, 0, 0,    0x48, 0x8b, 0x00, 0x48, 0x01,
                                        0xc7, 0xe8, 0,    0, 0, 0, 0x48, 0x83, 0xc0, 0x01, 0xc3};
    const char* calls[2] = {call1, call2};
    const char* reads[2] = {data1, data2};
    char name[2][64], fn[8];
    SnxXElf* o = begin(machine, 0, error, error_size);
    if (!o) return -1;
    for (int i = 0; i < 2; ++i) {
        slot_name(name[i], sizeof name[i], reads[i]);
        if (!i || strcmp(reads[0], reads[1])) slot(o, name[i], reads[i], machine == EM_A64 ? A64_ABS64 : X64_64);
    }
    for (int i = 0; i < 2; ++i) {
        snprintf(fn, sizeof fn, "need%d", i);
        if (machine == EM_A64) {
            const size_t at = put_words(o, a64, sizeof a64 / sizeof *a64, 0);
            snx_xelf_symbol(o, fn, SNX_ELF_TEXT, at, sizeof a64, SNX_ELF_GLOBAL | SNX_ELF_FUNC);
            snx_xelf_reloc(o, SNX_ELF_TEXT, at, name[i], A64_ADR_PREL_PG_HI21, 0);
            snx_xelf_reloc(o, SNX_ELF_TEXT, at + 4, name[i], A64_LDST64_ABS_LO12_NC, 0);
            snx_xelf_reloc(o, SNX_ELF_TEXT, at + 24, calls[i], A64_CALL26, 0);
        } else {
            const size_t at = snx_xelf_put(o, SNX_ELF_TEXT, x64, sizeof x64, 16);
            snx_xelf_symbol(o, fn, SNX_ELF_TEXT, at, sizeof x64, SNX_ELF_GLOBAL | SNX_ELF_FUNC);
            snx_xelf_reloc(o, SNX_ELF_TEXT, at + 3, name[i], X64_PC32, -4);
            snx_xelf_reloc(o, SNX_ELF_TEXT, at + 14, calls[i], X64_PLT32, -4);
        }
    }
    return end(o, "need", out, size, error, error_size);
}

int nobj_held(unsigned machine, unsigned char** out, size_t* size, char* error, size_t error_size) {
    const uint64_t other = 7;
    uint64_t table[64];
    SnxXElf* o = begin(machine, 0, error, error_size);
    if (!o) return -1;
    for (unsigned i = 0; i < 64; ++i) table[i] = (uint64_t)i * 3 + 1;
    size_t at = snx_xelf_put(o, SNX_ELF_DATA, &other, sizeof other, 8);
    snx_xelf_symbol(o, "other_data", SNX_ELF_DATA, at, sizeof other, SNX_ELF_GLOBAL);
    at = snx_xelf_put(o, SNX_ELF_RODATA, table, sizeof table, 8);
    snx_xelf_symbol(o, "held_table", SNX_ELF_RODATA, at, sizeof table, SNX_ELF_GLOBAL);
    at = snx_xelf_put(o, SNX_ELF_BSS, NULL, 4096, 16);
    snx_xelf_symbol(o, "held_zeros", SNX_ELF_BSS, at, 4096, SNX_ELF_GLOBAL);
    return end(o, "held", out, size, error, error_size);
}

/* ---- a RISC machine's, by its back end's descriptor functions ---- */

/* ext_fn(x) = 3x in the machine's words: MIPS64's daddu v0, a0, a0; daddu v0, v0, a0; jr ra; nop and s390x's
   lgr %r1, %r2; agr %r2, %r2; agr %r2, %r1; br %r14 (as halfwords) run on their hosts; the others are never run,
   RISC-V's slli t0, a0, 1; add a0, a0, t0; ret, LoongArch's alsl.d a0, a0, a0, 1; jirl zero, ra, 0 and PowerPC64's
   mulli r3, r3, 3; blr */
static size_t put_ext_fn(SnxXElf* o, const SnxNativeDesc* d, size_t* size) {
    static const uint32_t mips[] = {0x0084102du, 0x0044102du, 0x03e00008u, 0};
    static const uint32_t s390x[] = {0xb9040012u, 0xb9080022u, 0xb9080021u, 0x07fe0000u};
    static const uint32_t rv[] = {0x00151293u, 0x00550533u, 0x00008067u};
    static const uint32_t la[] = {0x002c1084u, 0x4c000020u};
    static const uint32_t ppc[] = {0x1c630003u, 0x4e800020u};
    const uint32_t* words = d->em == 8 ? mips : d->em == 22 ? s390x : d->em == 243 ? rv : d->em == 258 ? la : ppc;
    const size_t n = d->em == 8 || d->em == 22 ? 4 : d->em == 243 ? 3 : 2;
    *size = 4 * n;
    return put_words(o, words, n, d->big);
}
/* f as function name, its relocations into sites */
static int put_fn(SnxXElf* o, const SnxNativeDesc* d, const char* name, const SnxNativeFn* f, NobjSite* sites,
                  size_t max, size_t* n) {
    const size_t at = snx_xelf_put(o, SNX_ELF_TEXT, f->code, f->size, 16);
    snx_xelf_symbol(o, name, SNX_ELF_TEXT, at, f->size, SNX_ELF_GLOBAL | SNX_ELF_FUNC);
    for (int k = 0; k < f->nrel; ++k) {
        const unsigned type = f->rel[k].type ? f->rel[k].type : d->literal;
        snx_xelf_reloc(o, SNX_ELF_TEXT, at + f->rel[k].at, f->rel[k].sym, type, f->rel[k].addend);
        if (*n == max) return -1;
        NobjSite* s = &sites[(*n)++];
        snprintf(s->fn, sizeof s->fn, "%s", name);
        snprintf(s->sym, sizeof s->sym, "%s", f->rel[k].sym);
        s->at = f->rel[k].at;
        s->addend = f->rel[k].addend;
        s->type = type;
    }
    return 0;
}

int nobj_desc(const char* desc, int kind, NobjSite* sites, size_t max, size_t* nsites, unsigned* em, unsigned* flags,
              int* big, unsigned char** out, size_t* size, char* error, size_t error_size) {
    const SnxNativeDesc* d = snx_native_desc_named(desc);
    *nsites = 0;
    if (!d) {
        snprintf(error, error_size, "%s: this build has no such native back end", desc);
        return -1;
    }
    SnxXElf* o = snx_xelf_new();
    if (!o) {
        snprintf(error, error_size, "no object: out of memory");
        return -1;
    }
    snx_xelf_set_machine(o, d->em);
    snx_xelf_set_flags(o, d->elf_flags);
    snx_xelf_set_big(o, d->big);
    *em = d->em;
    *flags = d->elf_flags;
    *big = d->big;
    const uint64_t zero = 0;
    if (kind == 0) {
        SnxNativeFn f[4] = {0};
        if (d->returns(&f[0], "ext_data") || d->tail_to(&f[1], "ext_fn") || d->tail_slot(&f[2], "ext_pointer", 0) ||
            d->returns(&f[3], "ext_table")) {
            snx_xelf_free(o);
            snprintf(error, error_size, "%s: a descriptor function out of room", desc);
            return -1;
        }
        static const char* const names[4] = {"caller", "tail_direct", "tail_pointer", "third"};
        for (int i = 0; i < 4; ++i)
            if (put_fn(o, d, names[i], &f[i], sites, max, nsites)) {
                snx_xelf_free(o);
                snprintf(error, error_size, "%s: more than %zu relocations", desc, max);
                return -1;
            }
        const size_t at = snx_xelf_put(o, SNX_ELF_DATA, &zero, sizeof zero, 8);
        snx_xelf_symbol(o, "caller_slot", SNX_ELF_DATA, at, sizeof zero, SNX_ELF_GLOBAL);
        snx_xelf_reloc(o, SNX_ELF_DATA, at, "ext_data", d->abs64, 0);
    } else if (kind == 1) {
        size_t fn_size = 0;
        const size_t fn = put_ext_fn(o, d, &fn_size);
        snx_xelf_symbol(o, "ext_fn", SNX_ELF_TEXT, fn, fn_size, SNX_ELF_GLOBAL | SNX_ELF_FUNC);
        unsigned char value[8];
        for (unsigned b = 0; b < 8; ++b) value[b] = (unsigned char)(41ull >> (d->big ? 56 - 8 * b : 8 * b));
        size_t at = snx_xelf_put(o, SNX_ELF_DATA, value, sizeof value, 8);
        snx_xelf_symbol(o, "ext_data", SNX_ELF_DATA, at, sizeof value, SNX_ELF_GLOBAL);
        at = snx_xelf_put(o, SNX_ELF_DATA, &zero, sizeof zero, 8);
        snx_xelf_symbol(o, "ext_pointer", SNX_ELF_DATA, at, sizeof zero, SNX_ELF_GLOBAL);
        snx_xelf_reloc(o, SNX_ELF_DATA, at, "ext_fn", d->abs64, 0);
    } else {
        unsigned char table[24];
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned b = 0; b < 8; ++b)
                table[8 * i + b] = (unsigned char)((5ull + i) >> (d->big ? 56 - 8 * b : 8 * b));
        const size_t at = snx_xelf_put(o, SNX_ELF_DATA, table, sizeof table, 8);
        snx_xelf_symbol(o, "ext_table", SNX_ELF_DATA, at, sizeof table, SNX_ELF_GLOBAL);
    }
    return end(o, desc, out, size, error, error_size);
}
