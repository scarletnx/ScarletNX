/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* x64_elf.h: the object built in memory, then written. ELF: the null section, the five of bytes, a .rela of each but
   .bss, .symtab (locals first, as ELF wants), .strtab, .shstrtab and .note.GNU-stack. COFF: the sections that hold
   anything, .xdata and .pdata with them, each with its relocations, then the symbols and their strings */
#include "common/cpu/recompiler/x64/x64_elf.h"
#include "common/cpu/recompiler/object/coff_defs.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* module_x64.c makes one for any machine whose objects are ELF (elf_object.h has the definitions where the system's
   <elf.h> is missing) */
#if defined(__APPLE__) || defined(_WIN32)
#include "common/cpu/recompiler/object/elf_defs.h"
#else
#include <elf.h>
#endif
#ifndef R_X86_64_PLT32
#define R_X86_64_PLT32 4
#endif
/* AArch64's machine and relocations, by number: not every <elf.h> has them (ELF for the Arm 64-bit Architecture, 5.7) */
#ifndef EM_AARCH64
#define EM_AARCH64 183
#endif
enum {
    A64_ABS64 = 257,
    A64_ADR_PREL_PG_HI21 = 275,
    A64_ADD_ABS_LO12_NC = 277,
    A64_LDST8_ABS_LO12_NC = 278,
    A64_JUMP26 = 282,
    A64_CALL26 = 283,
    A64_LDST64_ABS_LO12_NC = 286
};
#if !defined(_WIN32)
/* elf_object.c's (its header clashes with this one's): r_info of a symbol and a type, MIPS64's n64 layout on EM_MIPS;
   the host's byte order; an object's tables turned to the other */
Elf64_Xword snx_elf_info(unsigned machine, size_t symbol, unsigned type);
unsigned char snx_elf_host_data(void);
int snx_elf_byteswap(unsigned char* object, size_t size, int to_foreign);
#endif

typedef struct {
    unsigned char* p;
    size_t n, cap;
    int oom; /* a put's bytes are not in it */
} Buf;
typedef struct {
    char* name;
    int section, flags;
    size_t offset, size;
} Sym;
typedef struct {
    int section;
    size_t offset;
    char* name;
    unsigned type;
    long long addend;
} Rel;
/* the function table's entry: [begin, end) of .text, its UNWIND_INFO (arm64's record) at info of .xdata */
typedef struct {
    uint32_t begin, end, info;
} Unwind;
struct SnxXElf {
    Buf sec[SNX_ELF_SECTIONS];
    size_t align[SNX_ELF_SECTIONS];
    Sym* syms;
    size_t nsyms, cap_syms;
    Rel* rels;
    size_t nrels, cap_rels;
    int oom;
    unsigned machine; /* EM_X86_64 unless snx_xelf_set_machine said */
    unsigned flags;   /* e_flags (snx_xelf_set_flags) */
    int big;          /* a big-endian machine's (snx_xelf_set_big) */
    int coff;         /* Windows' (snx_xelf_set_coff) */
    Buf xdata;
    Unwind* unwinds;
    size_t nunwinds, cap_unwinds;
    size_t* infos; /* a hash table of .xdata's infos, their offsets + 1: each kind of prologue has one */
    size_t ninfos, cap_infos;
    int formless; /* a prologue, or a relocation, COFF has no form of */
};

static int room(Buf* b, size_t n) {
    if (b->n + n <= b->cap) return 0;
    {
        size_t cap = b->cap ? b->cap * 2 : 256;
        unsigned char* p;
        while (cap < b->n + n) cap *= 2;
        p = (unsigned char*)realloc(b->p, cap);
        if (!p) return -1;
        b->p = p;
        b->cap = cap;
    }
    return 0;
}
static void put(Buf* b, const void* data, size_t n) {
    if (!n) return; /* nothing to put: a buffer not made yet has no p to pass memcpy or memset */
    if (room(b, n)) {
        b->oom = 1;
        return;
    }
    if (data) memcpy(b->p + b->n, data, n);
    else
        memset(b->p + b->n, 0, n);
    b->n += n;
}
static char* dup(const char* s) {
    size_t n = strlen(s) + 1;
    char* d = (char*)malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

SnxXElf* snx_xelf_new(void) {
    SnxXElf* o = (SnxXElf*)calloc(1, sizeof *o);
    int k;
    if (o)
        for (k = 0; k < SNX_ELF_SECTIONS; ++k) o->align[k] = k == SNX_ELF_TEXT ? 16 : 8;
    if (o) o->machine = EM_X86_64;
#if defined(_WIN32)
    if (o) o->coff = 1;
#endif
    return o;
}
void snx_xelf_set_coff(SnxXElf* o, int coff) { o->coff = coff; }
void snx_xelf_set_machine(SnxXElf* o, unsigned machine) { o->machine = machine; }
void snx_xelf_set_flags(SnxXElf* o, unsigned flags) { o->flags = flags; }
void snx_xelf_set_big(SnxXElf* o, int big) { o->big = big; }
void snx_xelf_free(SnxXElf* o) {
    size_t i;
    int k;
    if (!o) return;
    for (k = 0; k < SNX_ELF_SECTIONS; ++k) free(o->sec[k].p);
    for (i = 0; i < o->nsyms; ++i) free(o->syms[i].name);
    for (i = 0; i < o->nrels; ++i) free(o->rels[i].name);
    free(o->syms);
    free(o->rels);
    free(o->xdata.p);
    free(o->unwinds);
    free(o->infos);
    free(o);
}
size_t snx_xelf_put(SnxXElf* o, int section, const void* bytes, size_t n, size_t align) {
    Buf* b = &o->sec[section];
    size_t at;
    if (align < 1) align = 1;
    if (align > o->align[section]) o->align[section] = align;
    if (section == SNX_ELF_BSS) {
        b->n = (b->n + align - 1) / align * align;
        at = b->n;
        b->n += n;
        return at;
    }
    while (b->n % align) put(b, NULL, 1);
    at = b->n;
    put(b, bytes, n);
    if (b->n != at + n) o->oom = 1;
    return at;
}
unsigned char* snx_xelf_bytes(SnxXElf* o, int section) { return o->sec[section].p; }
void snx_xelf_symbol(SnxXElf* o, const char* name, int section, size_t offset, size_t size, int flags) {
    if (o->nsyms == o->cap_syms) {
        size_t cap = o->cap_syms ? o->cap_syms * 2 : 64;
        Sym* p = (Sym*)realloc(o->syms, cap * sizeof *p);
        if (!p) {
            o->oom = 1;
            return;
        }
        o->syms = p;
        o->cap_syms = cap;
    }
    o->syms[o->nsyms].name = dup(name);
    o->syms[o->nsyms].section = section;
    o->syms[o->nsyms].flags = flags;
    o->syms[o->nsyms].offset = offset;
    o->syms[o->nsyms++].size = size;
}
void snx_xelf_reloc(SnxXElf* o, int section, size_t offset, const char* name, unsigned type, long long addend) {
    if (o->nrels == o->cap_rels) {
        size_t cap = o->cap_rels ? o->cap_rels * 2 : 256;
        Rel* p = (Rel*)realloc(o->rels, cap * sizeof *p);
        if (!p) {
            o->oom = 1;
            return;
        }
        o->rels = p;
        o->cap_rels = cap;
    }
    o->rels[o->nrels].section = section;
    o->rels[o->nrels].offset = offset;
    o->rels[o->nrels].name = dup(name);
    o->rels[o->nrels].type = type;
    o->rels[o->nrels++].addend = addend;
}

/* FNV-1a */
static size_t hash(const char* n) {
    size_t h = (size_t)1469598103934665603ull;
    for (; *n; ++n) h = (h ^ (unsigned char)*n) * (size_t)1099511628211ull;
    return h;
}

/* ---- COFF ---- */

/* an UNWIND_INFO's bytes at most: its header, and the codes of eight pushes, a frame and ten XMM registers (Microsoft's
   x64 exception handling: each code a doublet, with one or two more for a frame's size, one for a save's offset) */
#define INFO_MAX (4 + 2 * (8 + 3 + 2 * 10 + 1))
enum { UWOP_PUSH_NONVOL = 0, UWOP_ALLOC_LARGE = 1, UWOP_ALLOC_SMALL = 2, UWOP_SAVE_XMM128 = 8 };

/* the UNWIND_INFO of the prologue at code (size bytes of it at most), as x64_codegen.c's prologue and x64_arm.c's
   mg_prologue write it: pushes and MOV RBX, RCX, a frame of a page's probe (x64_asm.h's sgx_stack_probe), SUB RSP (or
   the four-byte NOP of no frame), then MOVAPS of the XMM registers saved, up to the first other instruction. Its size */
static size_t unwind_info(const unsigned char* code, size_t size, unsigned char* out) {
    struct {
        unsigned char at, op, info;
        uint32_t value;
    } codes[32];
    unsigned n = 0, count = 0, k;
    size_t at = 0, o;
    int frame = 0; /* past the frame's SUB */
    while (at < size && n < sizeof codes / sizeof *codes) {
        const unsigned char* c = code + at;
        size_t left = size - at;
        if (!frame && (c[0] & 0xF8) == 0x50) { /* push rax to rdi */
            codes[n].op = UWOP_PUSH_NONVOL, codes[n].info = c[0] & 7;
            at += 1;
        } else if (!frame && left >= 2 && c[0] == 0x41 && (c[1] & 0xF8) == 0x50) { /* push r8 to r15 */
            codes[n].op = UWOP_PUSH_NONVOL, codes[n].info = (unsigned char)(8 | (c[1] & 7));
            at += 2;
        } else if (!frame && left >= 3 && !memcmp(c, "\x48\x89\xCB", 3)) { /* mov rbx, rcx */
            at += 3;
            continue;
        } else if (!frame && left >= 4 && !memcmp(c, "\x0F\x1F\x40\x00", 4)) { /* nop: no frame */
            at += 4, frame = 1;
            continue;
        } else if (!frame && left >= 4 && !memcmp(c, "\x48\x83\xEC", 3)) { /* sub rsp, imm8 */
            codes[n].op = UWOP_ALLOC_LARGE, codes[n].value = c[3];
            at += 4, frame = 1;
        } else if (!frame && left >= 7 && !memcmp(c, "\x48\x81\xEC", 3)) { /* sub rsp, imm32 */
            codes[n].op = UWOP_ALLOC_LARGE, codes[n].value = snx_ld_le32(c + 3);
            at += 7, frame = 1;
        } else if (!frame && left >= 26 && c[0] == 0xB8 &&
                   !memcmp(c + 5,
                           "\x49\x89\xE1\x49\x81\xE9\x00\x10\x00\x00\x45\x84\x09\x48\x2D\x00\x10\x00\x00\x77\xEE",
                           21)) { /* the stack probe */
            at += 26;
            continue;
        } else if (frame && left >= 9 && !memcmp(c, "\x44\x0F\x29", 3) && (c[3] & 0xC7) == 0x84 && c[4] == 0x24) {
            codes[n].op = UWOP_SAVE_XMM128,
            codes[n].info = (unsigned char)(8 | (c[3] >> 3 & 7)); /* movaps [rsp + disp] */
            codes[n].value = snx_ld_le32(c + 5);
            at += 9;
        } else if (frame && left >= 8 && !memcmp(c, "\x0F\x29", 2) && (c[2] & 0xC7) == 0x84 && c[3] == 0x24) {
            codes[n].op = UWOP_SAVE_XMM128, codes[n].info = c[2] >> 3 & 7;
            codes[n].value = snx_ld_le32(c + 4);
            at += 8;
        } else
            break;
        codes[n].at = (unsigned char)at;
        if (codes[n].op != UWOP_ALLOC_LARGE || codes[n].value) ++n; /* a frame of nothing has no code */
    }
    out[0] = 1; /* version 1, no handler */
    out[1] = (unsigned char)at;
    out[3] = 0; /* no frame register */
    o = 4;
    for (k = n; k-- > 0;) { /* the last first */
        unsigned char* p = out + o;
        uint32_t v = codes[k].value;
        p[0] = codes[k].at;
        if (codes[k].op == UWOP_PUSH_NONVOL) {
            p[1] = (unsigned char)(codes[k].info << 4 | UWOP_PUSH_NONVOL);
            count += 1;
        } else if (codes[k].op == UWOP_SAVE_XMM128) {
            p[1] = (unsigned char)(codes[k].info << 4 | UWOP_SAVE_XMM128);
            snx_st_le16(p + 2, v / 16);
            count += 2;
        } else if (v <= 128) {
            p[1] = (unsigned char)((v - 8) / 8 << 4 | UWOP_ALLOC_SMALL);
            count += 1;
        } else if (v / 8 <= 0xFFFF) {
            p[1] = UWOP_ALLOC_LARGE;
            snx_st_le16(p + 2, v / 8);
            count += 2;
        } else {
            p[1] = 1 << 4 | UWOP_ALLOC_LARGE;
            snx_st_le32(p + 2, v);
            count += 3;
        }
        o = 4 + 2 * (size_t)count;
    }
    out[2] = (unsigned char)count;
    if (count % 2) snx_st_le16(out + o, 0), o += 2;
    return o;
}
/* .xdata's offset of info (size bytes, a multiple of 4), put there where it is not yet */
static size_t info_at(SnxXElf* o, const unsigned char* info, size_t size) {
    size_t h = (size_t)1469598103934665603ull, i;
    if (2 * (o->ninfos + 1) > o->cap_infos) {
        size_t cap = o->cap_infos ? 2 * o->cap_infos : 64;
        size_t* table = (size_t*)calloc(cap, sizeof *table);
        if (!table) {
            o->oom = 1;
            return 0;
        }
        for (i = 0; i < o->cap_infos; ++i)
            if (o->infos[i]) {
                const unsigned char* p = o->xdata.p + o->infos[i] - 1;
                size_t n = 4 + 2 * (size_t)((p[2] + 1) & ~1), k, g = (size_t)1469598103934665603ull;
                for (k = 0; k < n; ++k) g = (g ^ p[k]) * (size_t)1099511628211ull;
                for (g &= cap - 1; table[g]; g = (g + 1) & (cap - 1)) {}
                table[g] = o->infos[i];
            }
        free(o->infos);
        o->infos = table;
        o->cap_infos = cap;
    }
    for (i = 0; i < size; ++i) h = (h ^ info[i]) * (size_t)1099511628211ull;
    for (h &= o->cap_infos - 1; o->infos[h]; h = (h + 1) & (o->cap_infos - 1)) {
        const unsigned char* p = o->xdata.p + o->infos[h] - 1;
        if (4 + 2 * (size_t)((p[2] + 1) & ~1) == size && !memcmp(p, info, size)) return o->infos[h] - 1;
    }
    i = o->xdata.n;
    put(&o->xdata, info, size);
    if (o->xdata.n != i + size) {
        o->oom = 1;
        return 0;
    }
    o->infos[h] = i + 1;
    ++o->ninfos;
    return i;
}
static void unwind_entry(SnxXElf* o, size_t begin, size_t end, size_t info) {
    if (o->nunwinds == o->cap_unwinds) {
        size_t cap = o->cap_unwinds ? o->cap_unwinds * 2 : 256;
        Unwind* p = (Unwind*)realloc(o->unwinds, cap * sizeof *p);
        if (!p) {
            o->oom = 1;
            return;
        }
        o->unwinds = p;
        o->cap_unwinds = cap;
    }
    o->unwinds[o->nunwinds].begin = (uint32_t)begin;
    o->unwinds[o->nunwinds].end = (uint32_t)end;
    o->unwinds[o->nunwinds++].info = (uint32_t)info;
}

/* ---- arm64's unwind data (Microsoft's ARM64 exception handling): a record of each piece of the code, its words, its
   epilogues, and the unwind codes of the prologue (its last instruction's first, one of each instruction) and of an
   epilogue (in its order), which the unwinder skips as many of as the instructions not run yet, or run already ---- */

#define A64_PIECE 0x3FFFFu /* the words a record holds */
typedef struct {
    unsigned char b[96];
    size_t n;
} A64Codes;
enum { A64_END = 0xE4, A64_END_C = 0xE5, A64_SET_FP = 0xE1, A64_NOP = 0xE3 };

static void a64_code(A64Codes* c, unsigned v) {
    if (c->n < sizeof c->b) c->b[c->n] = (unsigned char)v;
    ++c->n;
}
/* alloc_s, alloc_m or alloc_l of bytes (a multiple of 16, under 256 MiB) */
static void a64_alloc(A64Codes* c, uint32_t bytes) {
    uint32_t n = bytes / 16;
    if (n < 32)
        a64_code(c, n);
    else if (n < 0x800) {
        a64_code(c, 0xC0 | n >> 8);
        a64_code(c, n & 0xFF);
    } else {
        a64_code(c, 0xE0);
        a64_code(c, n >> 16 & 0xFF);
        a64_code(c, n >> 8 & 0xFF);
        a64_code(c, n & 0xFF);
    }
}
static uint32_t a64_word(const unsigned char* code, size_t k) { return snx_ld_le32(code + 4 * k); }
/* an LDP's or STP's offset of doublewords, signed, in bytes */
static int32_t a64_pair_offset(uint32_t w) { return (int32_t)((w >> 15 & 0x7F) ^ 0x40) * 8 - 0x40 * 8; }
/* STP (LDP) Xn, Xn+1, [SP, #offset] of two of X19 to X28 (save_regp): 1 and its code, 0 another word */
static int a64_pair(A64Codes* c, uint32_t w) {
    uint32_t rt = w & 31;
    int32_t offset = a64_pair_offset(w);
    if ((w & 0xFFC003E0u) != 0xA90003E0u || rt < 19 || rt > 27 || (w >> 10 & 31) != rt + 1 || offset < 0) return 0;
    a64_code(c, 0xC8 | (rt - 19) >> 2);
    a64_code(c, ((rt - 19) & 3) << 6 | (uint32_t)offset / 8);
    return 1;
}
/* a function's frame as its prologue says it: the codes of the prologue and of an epilogue, and the epilogue's words */
typedef struct {
    A64Codes prologue, epilogue;
    uint32_t word[12];
    size_t words;
} A64Frame;
/* the prologue of words words at most at code, as arm64_codegen.c's prologue and arm64_arm.c's mg_prologue write it:
   STP X29, X30 with the frame's base in X29, the pairs saved, ADD X19, X0, #8, a frame of a page's probe (arm64_asm.h's
   sga_stack_probe), then SUB SP; and the epilogue they have: ADD SP, the pairs back (the first, X19's, last), LDP X29,
   X30. The prologue's words, 0 where it is none */
static size_t a64_prologue(const unsigned char* code, size_t words, A64Frame* f) {
    A64Codes step[24];
    uint32_t pair[8], base = 0;
    size_t k, n, pairs = 0;
    for (k = 0; k < words && k < sizeof step / sizeof *step; ++k) {
        uint32_t w = a64_word(code, k);
        step[k].n = 0;
        if (!k) { /* STP X29, X30, [SP, #-base]!: save_fplr_x */
            if ((w & 0xFFC07FFFu) != 0xA9807BFDu || a64_pair_offset(w) >= 0) return 0;
            base = (uint32_t)-a64_pair_offset(w);
            a64_code(&step[k], 0x80 | (base / 8 - 1));
        } else if (w == 0x910003FDu) /* MOV X29, SP */
            a64_code(&step[k], A64_SET_FP);
        else if (pairs < sizeof pair / sizeof *pair && a64_pair(&step[k], w))
            pair[pairs++] = w;
        else if (w == 0x91002013u ||                                                         /* ADD X19, X0, #8 */
                 w == 0x910003F1u || (w & 0xFF8003FFu) == 0xD10003F0u || w == 0xD1400631u || /* the probe's */
                 w == 0x3940023Fu || w == 0xEB10023Fu || w == 0x54FFFFA8u)
            a64_code(&step[k], A64_NOP);
        else if ((w & 0xFF8003FFu) == 0xD10003FFu && pairs) { /* SUB SP, SP, #frame */
            uint32_t frame = (w >> 10 & 0xFFF) << (w >> 22 & 1 ? 12 : 0);
            a64_alloc(&step[k], frame);
            for (n = k + 1; n-- > 0;)
                for (size_t b = 0; b < step[n].n; ++b) a64_code(&f->prologue, step[n].b[b]);
            a64_code(&f->prologue, A64_END);
            f->word[f->words++] = w ^ 0x40000000u; /* ADD SP */
            a64_alloc(&f->epilogue, frame);
            for (n = 1; n <= pairs; ++n) {
                uint32_t p = pair[n % pairs];
                (void)a64_pair(&f->epilogue, p);
                f->word[f->words++] = p | 0x00400000u; /* LDP */
            }
            a64_code(&f->epilogue, 0x80 | (base / 8 - 1));
            a64_code(&f->epilogue, A64_END);
            f->word[f->words++] = 0xA8C07BFDu | base / 8 << 15; /* LDP X29, X30, [SP], #base */
            return k + 1;
        } else
            return 0;
    }
    return 0;
}
/* the record of words words from at on, epilogues the words of the n that start in them, in order. A piece past the
   code's start has no prologue, its body's codes the code's own after end_c */
static void a64_record(SnxXElf* o, size_t begin, size_t at, size_t words, const A64Frame* f, const uint32_t* epilogues,
                       size_t n) {
    A64Codes codes = {.n = 0};
    size_t k, start, info = o->xdata.n, code_words;
    unsigned char w[4];
    if (at) a64_code(&codes, A64_END_C);
    for (k = 0; k < f->prologue.n; ++k) a64_code(&codes, f->prologue.b[k]);
    start = codes.n;
    for (k = 0; n && k < f->epilogue.n; ++k) a64_code(&codes, f->epilogue.b[k]);
    while (codes.n % 4) a64_code(&codes, A64_END);
    code_words = codes.n / 4; /* under 32: the frame's pairs are X19 to X28's */
    snx_st_le32(w, (uint32_t)(n < 32 ? words | n << 22 | code_words << 27 : words));
    put(&o->xdata, w, 4);
    if (n >= 32) {
        snx_st_le32(w, (uint32_t)(n | code_words << 16));
        put(&o->xdata, w, 4);
    }
    for (k = 0; k < n; ++k) {
        snx_st_le32(w, (uint32_t)(epilogues[k] - at) | (uint32_t)start << 22);
        put(&o->xdata, w, 4);
    }
    put(&o->xdata, codes.b, codes.n);
    if (o->xdata.oom) o->oom = 1;
    unwind_entry(o, begin + 4 * at, begin + 4 * (at + words), info);
}
static void a64_unwind(SnxXElf* o, size_t begin, size_t end, const unsigned char* code, size_t size) {
    A64Frame f = {0};
    size_t words = (end - begin) / 4, at, k, j, first = 0, last, n;
    Buf found = {0}; /* the epilogues' words, as uint32_t */
    k = size < end - begin ? 0 : a64_prologue(code, words, &f);
    if (!k) {
        o->formless = 1;
        return;
    }
    while (k + f.words < words) { /* a return or a jump after it */
        uint32_t here = (uint32_t)k;
        for (j = 0; j < f.words && a64_word(code, k + j) == f.word[j]; ++j) {}
        if (j == f.words) put(&found, &here, sizeof here);
        k += j == f.words ? j + 1 : 1;
    }
    n = found.n / sizeof(uint32_t);
    for (at = 0; at < words && !found.oom; at += last) {
        uint32_t none = 0;
        const uint32_t* epilogues = n ? (const uint32_t*)(const void*)found.p : &none;
        size_t count = 0;
        last = words - at > A64_PIECE ? A64_PIECE : words - at;
        while (first + count < n && epilogues[first + count] < at + last) ++count;
        /* an epilogue and what follows it in one piece */
        if (count && epilogues[first + count - 1] + f.words + 1 > at + last) last = epilogues[first + --count] - at;
        a64_record(o, begin, at, last, &f, epilogues + first, count);
        first += count;
    }
    if (found.oom) o->oom = 1;
    free(found.p);
}

void snx_xelf_unwind(SnxXElf* o, size_t begin, size_t end, const unsigned char* code, size_t size) {
    unsigned char info[INFO_MAX];
    if (!o->coff || begin >= end) return;
    if (o->machine == EM_AARCH64)
        a64_unwind(o, begin, end, code, size);
    else
        unwind_entry(o, begin, end, info_at(o, info, unwind_info(code, size, info)));
}

/* a symbol's record: name, value, section (1-based, 0 undefined), type 0x20 a function's, class */
static void coff_symbol(Buf* symtab, Buf* strings, const char* name, uint32_t value, int section, int function,
                        unsigned char class) {
    unsigned char s[18] = {0};
    size_t n = strlen(name);
    if (n <= 8)
        memcpy(s, name, n);
    else {
        snx_st_le32(s + 4, (uint32_t)strings->n);
        put(strings, name, n + 1);
    }
    snx_st_le32(s + 8, value);
    snx_st_le16(s + 12, (uint32_t)section);
    snx_st_le16(s + 14, function ? 0x20 : 0);
    s[16] = class;
    put(symtab, s, sizeof s);
}
static void coff_reloc(Buf* relocs, uint32_t at, size_t symbol, unsigned type) {
    unsigned char r[10];
    snx_st_le32(r, at);
    snx_st_le32(r + 4, (uint32_t)symbol);
    snx_st_le16(r + 8, type);
    put(relocs, r, sizeof r);
}
/* arm64's relocation r, its ELF type's, at p: COFF's type, its addend in the field; -1 for a type or an addend it has no
   form of */
static int a64_reloc(unsigned char* p, const Rel* r) {
    uint32_t w;
    unsigned long long a = (unsigned long long)r->addend;
    if (r->type == A64_ABS64) {
        snx_st_le64(p, a);
        return REL_A64_ADDR64;
    }
    w = snx_ld_le32(p);
    switch (r->type) {
        case A64_ADR_PREL_PG_HI21: /* in bytes */
            if (r->addend < -(1 << 20) || r->addend >= 1 << 20) return -1;
            snx_st_le32(p, (w & 0x9F00001Fu) | (uint32_t)(a & 3) << 29 | (uint32_t)(a >> 2 & 0x7FFFF) << 5);
            return REL_A64_PAGEBASE_REL21;
        case A64_ADD_ABS_LO12_NC:
            snx_st_le32(p, (w & ~0x3FFC00u) | (uint32_t)(a & 0xFFF) << 10);
            return REL_A64_PAGEOFFSET_12A;
        case A64_LDST8_ABS_LO12_NC:
        case A64_LDST64_ABS_LO12_NC: { /* in the access's size */
            unsigned scale = r->type == A64_LDST64_ABS_LO12_NC ? 3 : 0;
            if (a & ((1u << scale) - 1)) return -1;
            snx_st_le32(p, (w & ~0x3FFC00u) | (uint32_t)((a & 0xFFF) >> scale) << 10);
            return REL_A64_PAGEOFFSET_12L;
        }
        case A64_JUMP26:
        case A64_CALL26: return a ? -1 : REL_A64_BRANCH26;
        default: return -1;
    }
}
/* the sections the builder's five are, and the function table's two */
enum { C_XDATA = SNX_ELF_SECTIONS, C_PDATA, C_SECTIONS };

static int coff_write(SnxXElf* o, unsigned char** out, size_t* size) {
    static const char* const names[C_SECTIONS] = {".text", ".rdata", ".rdata", ".data", ".bss", ".xdata", ".pdata"};
    static const uint32_t flags[C_SECTIONS] = {SCN_CODE | SCN_EXECUTE | SCN_READ,
                                               SCN_INITIALIZED | SCN_READ,
                                               SCN_INITIALIZED | SCN_READ,
                                               SCN_INITIALIZED | SCN_READ | SCN_WRITE,
                                               SCN_UNINITIALIZED | SCN_READ | SCN_WRITE,
                                               SCN_INITIALIZED | SCN_READ,
                                               SCN_INITIALIZED | SCN_READ};
    Buf file = {0}, symtab = {0}, strings = {0}, pdata = {0}, relocs[C_SECTIONS] = {{0}};
    size_t hcap = 64, *table, *utable, nsyms = 0, *sym_index, i, at, text_symbol = 0, xdata_symbol = 0, nundef = 0;
    size_t* undef_index;
    const char** undef;
    int number[C_SECTIONS] = {0}, nsec = 0, k, rc = -1;
    unsigned char zero4[4] = {0};
    const int a64 = o->machine == EM_AARCH64;
    if (o->oom) return -1;
    if (o->formless) return -2;
    for (i = 0; i < o->nsyms; ++i) number[o->syms[i].section] = 1; /* one with a symbol is written though it is empty */
    for (k = 0; k < C_SECTIONS; ++k) {
        size_t n = k == C_XDATA ? o->xdata.n : k == C_PDATA ? o->nunwinds : o->sec[k].n;
        number[k] = n || number[k] ? ++nsec : 0;
        if (n > 0x7FFFFFFFu) return -1;
    }
    while (hcap < 2 * (o->nsyms + o->nrels) + 2) hcap *= 2;
    table = (size_t*)calloc(hcap, sizeof *table);
    utable = (size_t*)calloc(hcap, sizeof *utable);
    sym_index = (size_t*)calloc(o->nsyms + 1, sizeof *sym_index);
    undef_index = (size_t*)calloc(o->nrels + 1, sizeof *undef_index);
    undef = (const char**)calloc(o->nrels + 1, sizeof *undef);
    if (!table || !utable || !sym_index || !undef_index || !undef) goto done;
    put(&strings, zero4, 4); /* the table's own size, first */
    if (strings.oom) goto done;
    /* the code's and the unwind data's sections' symbols, which the function table names; the symbols; the undefined */
    if (number[SNX_ELF_TEXT]) {
        text_symbol = nsyms++;
        coff_symbol(&symtab, &strings, names[SNX_ELF_TEXT], 0, number[SNX_ELF_TEXT], 0, SYM_STATIC);
    }
    if (number[C_XDATA]) {
        xdata_symbol = nsyms++;
        coff_symbol(&symtab, &strings, names[C_XDATA], 0, number[C_XDATA], 0, SYM_STATIC);
    }
    for (i = 0; i < o->nsyms; ++i) {
        const Sym* s = &o->syms[i];
        size_t h = hash(s->name) & (hcap - 1);
        sym_index[i] = nsyms++;
        coff_symbol(&symtab, &strings, s->name, (uint32_t)s->offset, number[s->section], s->flags & SNX_ELF_FUNC,
                    s->flags & SNX_ELF_GLOBAL ? SYM_EXTERNAL : SYM_STATIC);
        while (table[h]) h = (h + 1) & (hcap - 1);
        table[h] = i + 1;
    }
    for (i = 0; i < o->nrels; ++i) {
        const Rel* r = &o->rels[i];
        size_t h0 = hash(r->name) & (hcap - 1), h = h0, symbol;
        unsigned char* p = o->sec[r->section].p + r->offset;
        const Sym* to = NULL;
        for (; table[h]; h = (h + 1) & (hcap - 1))
            if (!strcmp(o->syms[table[h] - 1].name, r->name)) break;
        if (table[h]) {
            to = &o->syms[table[h] - 1];
            symbol = sym_index[table[h] - 1];
        } else {
            for (h = h0; utable[h]; h = (h + 1) & (hcap - 1))
                if (!strcmp(undef[utable[h] - 1], r->name)) break;
            if (!utable[h]) {
                undef[nundef] = r->name;
                undef_index[nundef] = nsyms++;
                utable[h] = ++nundef;
                coff_symbol(&symtab, &strings, r->name, 0, 0, 0, SYM_EXTERNAL);
            }
            symbol = undef_index[utable[h] - 1];
        }
        if (a64) {
            Rel c = *r;
            int type;
            if (to && to->section == r->section && (r->type == A64_JUMP26 || r->type == A64_CALL26) && !r->addend) {
                /* both here: written, where B reaches */
                long long d = ((long long)to->offset - (long long)r->offset) / 4;
                if (d >= -(1 << 25) && d < 1 << 25) {
                    snx_st_le32(p, (snx_ld_le32(p) & 0xFC000000u) | ((uint32_t)d & 0x3FFFFFFu));
                    continue;
                }
            }
            if (to && r->type == A64_ADR_PREL_PG_HI21 && (r->addend < -(1 << 20) || r->addend >= 1 << 20)) {
                /* past an ADRP's field: a symbol of the place itself */
                symbol = nsyms++;
                coff_symbol(&symtab, &strings, "snx_at", (uint32_t)((long long)to->offset + r->addend),
                            number[to->section], 0, SYM_STATIC);
                c.addend = 0;
            }
            if ((type = a64_reloc(p, &c)) < 0) {
                rc = -2;
                goto done;
            }
            coff_reloc(&relocs[r->section], (uint32_t)r->offset, symbol, (unsigned)type);
        } else if (r->type == R_X86_64_64) {
            snx_st_le64(p, (uint64_t)r->addend);
            coff_reloc(&relocs[r->section], (uint32_t)r->offset, symbol, REL_ADDR64);
        } else if (r->type == R_X86_64_PC32 || r->type == R_X86_64_PLT32) {
            if (to && to->section == r->section) { /* both here: written */
                snx_st_le32(p, (uint32_t)(to->offset + (uint64_t)r->addend - r->offset));
                continue;
            }
            /* ELF's addend counts from the field, COFF's from its end */
            snx_st_le32(p, (uint32_t)((uint64_t)r->addend + 4));
            coff_reloc(&relocs[r->section], (uint32_t)r->offset, symbol, REL_REL32);
        } else {
            rc = -2; /* no other type of x86-64 ELF's is used */
            goto done;
        }
    }
    for (i = 0; i < o->nunwinds; ++i) {
        /* RUNTIME_FUNCTION: where it begins, x86-64's where it ends, its unwind data */
        unsigned char e[12];
        uint32_t here = (uint32_t)pdata.n;
        const uint32_t size_of = (uint32_t)snx_coff_function_size(a64 ? COFF_ARM64 : COFF_X64);
        snx_st_le32(e, o->unwinds[i].begin);
        if (!a64) snx_st_le32(e + 4, o->unwinds[i].end);
        snx_st_le32(e + size_of - 4, o->unwinds[i].info);
        put(&pdata, e, size_of);
        coff_reloc(&relocs[C_PDATA], here, text_symbol, a64 ? REL_A64_ADDR32NB : REL_ADDR32NB);
        if (!a64) coff_reloc(&relocs[C_PDATA], here + 4, text_symbol, REL_ADDR32NB);
        coff_reloc(&relocs[C_PDATA], here + size_of - 4, xdata_symbol, a64 ? REL_A64_ADDR32NB : REL_ADDR32NB);
    }
    put(&file, NULL, 20 + 40 * (size_t)nsec); /* the headers, below */
    if (!file.p) goto done;
    snx_st_le16(file.p, a64 ? COFF_ARM64 : COFF_X64);
    snx_st_le16(file.p + 2, (uint32_t)nsec);
    for (k = 0; k < C_SECTIONS; ++k) {
        const Buf* b = k == C_XDATA ? &o->xdata : k == C_PDATA ? &pdata : &o->sec[k];
        size_t align = k < SNX_ELF_SECTIONS ? o->align[k] : 4, raw = 0, reloc_at = 0, count = relocs[k].n / 10,
               n = b->n;
        unsigned char* h;
        uint32_t bits = 0, f = flags[k];
        if (!number[k]) continue;
        if (k != SNX_ELF_BSS) {
            while (file.n % 4 && !file.oom) put(&file, NULL, 1);
            raw = file.n;
            put(&file, b->p, n);
        }
        if (count) {
            reloc_at = file.n;
            if (count >= 0xFFFF) {
                /* too many for the header's 16 bits: the first relocation holds the count, its own too */
                unsigned char first[10] = {0};
                snx_st_le32(first, (uint32_t)count + 1);
                put(&file, first, sizeof first);
                f |= SCN_NRELOC_OVFL;
            }
            put(&file, relocs[k].p, relocs[k].n);
        }
        while (((size_t)1 << bits) < align) ++bits;
        h = file.p + 20 + 40 * (size_t)(number[k] - 1);
        memcpy(h, names[k], strlen(names[k]));
        snx_st_le32(h + 16, (uint32_t)n);
        snx_st_le32(h + 20, (uint32_t)raw);
        snx_st_le32(h + 24, (uint32_t)reloc_at);
        snx_st_le16(h + 32, count >= 0xFFFF ? 0xFFFF : (uint32_t)count);
        snx_st_le32(h + 36, f | (bits + 1) << 20);
    }
    at = file.n;
    put(&file, symtab.p, symtab.n);
    snx_st_le32(strings.p, (uint32_t)strings.n);
    put(&file, strings.p, strings.n);
    if (file.oom || symtab.oom || strings.oom || pdata.oom || at > 0x7FFFFFFFu) goto done;
    for (k = 0; k < C_SECTIONS; ++k)
        if (relocs[k].oom) goto done;
    snx_st_le32(file.p + 8, (uint32_t)at);
    snx_st_le32(file.p + 12, (uint32_t)nsyms);
    *out = file.p;
    *size = file.n;
    file.p = NULL;
    rc = 0;
done:
    free(file.p), free(symtab.p), free(strings.p), free(pdata.p);
    for (k = 0; k < C_SECTIONS; ++k) free(relocs[k].p);
    free(table), free(utable), free(sym_index), free(undef_index), free((void*)undef);
    return rc;
}

#if defined(_WIN32)
int snx_xelf_write(SnxXElf* o, unsigned char** out, size_t* size) { return coff_write(o, out, size); }
#else
int snx_xelf_write(SnxXElf* o, unsigned char** out, size_t* size) {
    static const char* const names[SNX_ELF_SECTIONS] = {".text", ".rodata", ".data.rel.ro", ".data", ".bss"};
    Buf file = {0}, strtab = {0}, symtab = {0}, shstr = {0}, rela[SNX_ELF_SECTIONS];
    Elf64_Ehdr eh;
    Elf64_Shdr sh[2 * SNX_ELF_SECTIONS + 5];
    Elf64_Sym sym;
    size_t i, nlocal, hcap = 64, *table, *utable, nundef = 0, *sym_index, *undef_index;
    size_t sec_off[SNX_ELF_SECTIONS], rela_off[SNX_ELF_SECTIONS], off_sym, off_str, off_shstr;
    unsigned sh_name[2 * SNX_ELF_SECTIONS + 5];
    int k, nsh;
    char** undef;
    if (o->coff) return coff_write(o, out, size);
    memset(rela, 0, sizeof rela);
    memset(sh, 0, sizeof sh);
    if (o->oom) return -1;
    while (hcap < 2 * (o->nsyms + o->nrels) + 2) hcap *= 2;
    table = (size_t*)calloc(hcap, sizeof *table);
    utable = (size_t*)calloc(hcap, sizeof *utable);
    sym_index = (size_t*)calloc(o->nsyms + 1, sizeof *sym_index);
    undef_index = (size_t*)calloc(o->nrels + 1, sizeof *undef_index);
    undef = (char**)calloc(o->nrels + 1, sizeof *undef);
    if (!table || !utable || !sym_index || !undef_index || !undef) {
        free(table), free(utable), free(sym_index), free(undef_index), free(undef);
        return -1;
    }
    put(&strtab, "", 1);
    put(&shstr, "", 1);
    /* the symbol table: null, the sections' symbols, the local symbols, then the global ones and the undefined */
    memset(&sym, 0, sizeof sym);
    put(&symtab, &sym, sizeof sym);
    for (k = 0; k < SNX_ELF_SECTIONS; ++k) {
        sym.st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
        sym.st_shndx = (Elf64_Half)(1 + k);
        put(&symtab, &sym, sizeof sym);
    }
    for (int pass = 0; pass < 2; ++pass)
        for (i = 0; i < o->nsyms; ++i) {
            const Sym* s = &o->syms[i];
            if (!!(s->flags & SNX_ELF_GLOBAL) != pass) continue;
            memset(&sym, 0, sizeof sym);
            sym.st_name = (Elf64_Word)strtab.n;
            put(&strtab, s->name, strlen(s->name) + 1);
            sym.st_info = ELF64_ST_INFO(pass ? STB_GLOBAL : STB_LOCAL, s->flags & SNX_ELF_FUNC ? STT_FUNC : STT_OBJECT);
            sym.st_other = s->flags & SNX_ELF_HIDDEN ? STV_HIDDEN : STV_DEFAULT;
            sym.st_shndx = (Elf64_Half)(1 + s->section);
            sym.st_value = s->offset;
            sym.st_size = s->size;
            sym_index[i] = symtab.n / sizeof sym;
            put(&symtab, &sym, sizeof sym);
            if (!pass) nlocal = symtab.n / sizeof sym;
        }
    nlocal = 1 + SNX_ELF_SECTIONS;
    for (i = 0; i < o->nsyms; ++i)
        if (!(o->syms[i].flags & SNX_ELF_GLOBAL)) ++nlocal;
    for (i = 0; i < o->nsyms; ++i) {
        size_t h = hash(o->syms[i].name) & (hcap - 1);
        while (table[h]) h = (h + 1) & (hcap - 1);
        table[h] = i + 1;
    }
    for (i = 0; i < o->nrels; ++i) {
        size_t h = hash(o->rels[i].name) & (hcap - 1), j;
        for (; table[h]; h = (h + 1) & (hcap - 1))
            if (!strcmp(o->syms[table[h] - 1].name, o->rels[i].name)) break;
        if (table[h]) {
            undef_index[i] = sym_index[table[h] - 1];
            continue;
        }
        /* an undefined name: by the second hash table, added the first time */
        {
            size_t u = hash(o->rels[i].name) & (hcap - 1);
            for (; utable[u]; u = (u + 1) & (hcap - 1))
                if (!strcmp(undef[utable[u] - 1], o->rels[i].name)) break;
            if (!utable[u]) {
                undef[nundef++] = o->rels[i].name;
                utable[u] = nundef;
                memset(&sym, 0, sizeof sym);
                sym.st_name = (Elf64_Word)strtab.n;
                put(&strtab, o->rels[i].name, strlen(o->rels[i].name) + 1);
                sym.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_NOTYPE);
                sym.st_shndx = SHN_UNDEF;
                put(&symtab, &sym, sizeof sym);
            }
            undef_index[i] = 1 + SNX_ELF_SECTIONS + o->nsyms + (utable[u] - 1);
        }
        (void)j;
    }
    for (i = 0; i < o->nrels; ++i) {
        Elf64_Rela r;
        r.r_offset = o->rels[i].offset;
        r.r_info = snx_elf_info(o->machine, undef_index[i], o->rels[i].type);
        r.r_addend = o->rels[i].addend;
        put(&rela[o->rels[i].section], &r, sizeof r);
    }
    /* the file */
    memset(&eh, 0, sizeof eh);
    put(&file, &eh, sizeof eh);
    for (k = 0; k < SNX_ELF_SECTIONS; ++k) {
        while (file.n % o->align[k]) put(&file, NULL, 1);
        sec_off[k] = file.n;
        if (k != SNX_ELF_BSS) put(&file, o->sec[k].p, o->sec[k].n);
    }
    for (k = 0; k < SNX_ELF_SECTIONS; ++k) {
        while (file.n % 8) put(&file, NULL, 1);
        rela_off[k] = file.n;
        put(&file, rela[k].p, rela[k].n);
    }
    while (file.n % 8) put(&file, NULL, 1);
    off_sym = file.n;
    put(&file, symtab.p, symtab.n);
    off_str = file.n;
    put(&file, strtab.p, strtab.n);
    nsh = 1;
    for (k = 0; k < SNX_ELF_SECTIONS; ++k) {
        sh_name[nsh] = (unsigned)shstr.n;
        put(&shstr, names[k], strlen(names[k]) + 1);
        sh[nsh].sh_name = sh_name[nsh];
        sh[nsh].sh_type = k == SNX_ELF_BSS ? SHT_NOBITS : SHT_PROGBITS;
        sh[nsh].sh_flags = SHF_ALLOC | (k == SNX_ELF_TEXT ? SHF_EXECINSTR : 0) |
                           (k == SNX_ELF_DATA || k == SNX_ELF_BSS || k == SNX_ELF_RELRO ? SHF_WRITE : 0);
        sh[nsh].sh_offset = sec_off[k];
        sh[nsh].sh_size = o->sec[k].n;
        sh[nsh].sh_addralign = o->align[k];
        ++nsh;
    }
    for (k = 0; k < SNX_ELF_SECTIONS; ++k) {
        char rn[32] = ".rela";
        strcat(rn, names[k]);
        sh[nsh].sh_name = (unsigned)shstr.n;
        put(&shstr, rn, strlen(rn) + 1);
        sh[nsh].sh_type = SHT_RELA;
        sh[nsh].sh_flags = SHF_INFO_LINK;
        sh[nsh].sh_offset = rela_off[k];
        sh[nsh].sh_size = rela[k].n;
        sh[nsh].sh_link = 2 * SNX_ELF_SECTIONS + 1;
        sh[nsh].sh_info = (Elf64_Word)(1 + k);
        sh[nsh].sh_addralign = 8;
        sh[nsh].sh_entsize = sizeof(Elf64_Rela);
        ++nsh;
    }
    /* .symtab, .strtab, .shstrtab, .note.GNU-stack */
    sh[nsh].sh_name = (unsigned)shstr.n;
    put(&shstr, ".symtab", 8);
    sh[nsh].sh_type = SHT_SYMTAB;
    sh[nsh].sh_offset = off_sym;
    sh[nsh].sh_size = symtab.n;
    sh[nsh].sh_link = (Elf64_Word)(nsh + 1);
    sh[nsh].sh_info = (Elf64_Word)nlocal;
    sh[nsh].sh_addralign = 8;
    sh[nsh].sh_entsize = sizeof(Elf64_Sym);
    ++nsh;
    sh[nsh].sh_name = (unsigned)shstr.n;
    put(&shstr, ".strtab", 8);
    sh[nsh].sh_type = SHT_STRTAB;
    sh[nsh].sh_offset = off_str;
    sh[nsh].sh_size = strtab.n;
    sh[nsh].sh_addralign = 1;
    ++nsh;
    sh[nsh].sh_name = (unsigned)shstr.n;
    put(&shstr, ".shstrtab", 10);
    sh[nsh].sh_type = SHT_STRTAB;
    sh[nsh].sh_addralign = 1;
    {
        int shstr_index = nsh++;
        sh[nsh].sh_name = (unsigned)shstr.n;
        put(&shstr, ".note.GNU-stack", 16);
        sh[nsh].sh_type = SHT_PROGBITS;
        sh[nsh].sh_addralign = 1;
        ++nsh;
        off_shstr = file.n;
        put(&file, shstr.p, shstr.n);
        sh[shstr_index].sh_offset = off_shstr;
        sh[shstr_index].sh_size = shstr.n;
        while (file.n % 8) put(&file, NULL, 1);
        memcpy(eh.e_ident, ELFMAG, SELFMAG);
        eh.e_ident[EI_CLASS] = ELFCLASS64;
        eh.e_ident[EI_DATA] = o->big ? ELFDATA2MSB : ELFDATA2LSB;
        eh.e_ident[EI_VERSION] = EV_CURRENT;
        eh.e_type = ET_REL;
        eh.e_machine = (Elf64_Half)o->machine;
        eh.e_flags = o->flags;
        eh.e_version = EV_CURRENT;
        eh.e_shoff = file.n;
        eh.e_ehsize = sizeof eh;
        eh.e_shentsize = sizeof(Elf64_Shdr);
        eh.e_shnum = (Elf64_Half)nsh;
        eh.e_shstrndx = (Elf64_Half)shstr_index;
    }
    put(&file, sh, (size_t)nsh * sizeof *sh);
    if (file.p) memcpy(file.p, &eh, sizeof eh);
    if (file.p && eh.e_ident[EI_DATA] != snx_elf_host_data()) snx_elf_byteswap(file.p, file.n, 1);
    for (k = 0; k < SNX_ELF_SECTIONS; ++k) free(rela[k].p);
    free(strtab.p), free(symtab.p), free(shstr.p), free(table), free(utable), free(sym_index), free(undef_index), free(undef);
    if (!file.p) return -1;
    *out = file.p;
    *size = file.n;
    return 0;
}

#endif
