/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* COFF: x86-64 or ARM64 objects into a PE32+ DLL without imports or an entry point.
   Sections: .text, .rdata (read-only data, unwind data, the export directory), .data, .bss, .pdata (function table,
   sorted), .reloc (DIR64 base relocations for absolute addresses). COMDAT sections keep their first copy. An ARM64
   module of more code than B and BL reach (VENEER_TEXT) gives each object an island after its last code section,
   with a veneer for each symbol it branches to that another object defines, which a branch out of reach goes through
   when its field holds no addend: one veneer serves every branch of the object to the symbol.
   The structures are read and written byte by byte, so this builds anywhere (no <windows.h>). */
#include "common/cpu/recompiler/object/coff_defs.h"
#include "common/cpu/recompiler/object/link_internal.h"
#include "common/utilities/bits.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_BASE        ((uint64_t)0x180000000ULL)
#define SECTION_ALIGNMENT 0x1000
#define FILE_ALIGNMENT    0x200
/* Below this much code every ARM64 B and BL of a module reaches its target (128 MiB, with room for the islands), and
   no object gets veneers */
#define VENEER_TEXT       ((uint64_t)120 << 20)

/* ---- ARM64's relocations and veneers (coff_defs.h), shared with the JIT loader (code_loader_coff.c). Everything in
   uint64_t: a negative displacement is its two's complement, which the masks cut to the field ---- */

/* whether value, read as signed, fits a signed field of bits */
static int fits(uint64_t value, unsigned bits) { return (value + ((uint64_t)1 << (bits - 1))) >> bits == 0; }
/* the low bits of value as the signed number they are */
static uint64_t sign_extend(uint64_t value, unsigned bits) {
    uint64_t m = (uint64_t)1 << (bits - 1);
    return ((value & ((m << 1) - 1)) ^ m) - m;
}
/* ADR's and ADRP's immediate: its low two bits at 29, the rest at 5 */
static uint64_t adr_immediate(uint32_t insn) {
    return sign_extend((insn >> 29 & 3) | (uint64_t)(insn >> 5 & 0x7FFFF) << 2, 21);
}
/* the 12-bit immediate of an ADD or of a load or store's offset */
static uint64_t imm12(uint32_t insn) { return insn >> 10 & 0xFFF; }
/* log2 of a load or store's access size: its size bits, 4 for a 128-bit SIMD access (V with opc's high bit) */
static unsigned access_scale(uint32_t insn) { return (insn & 0x04800000u) == 0x04800000u ? 4 : insn >> 30; }
/* a branch's displacement, in instructions, into its field */
static int a64_branch(unsigned char* p, uint64_t displacement, unsigned bits, unsigned shift) {
    if (!fits(displacement, bits + 2)) return SNX_LINK_RANGE;
    if (displacement & 3) return SNX_LINK_MISALIGNED;
    snx_link_field(p, displacement >> 2, shift, bits);
    return 0;
}

int snx_coff_a64_relocate(unsigned char* p, size_t room, unsigned type, uint64_t S, uint64_t P, uint64_t base,
                          uint64_t section_start) {
    uint64_t target, offset;
    unsigned scale;
    if (type == REL_A64_ABSOLUTE) return 0;
    if (room < 4) return SNX_LINK_OUTSIDE;
    uint32_t insn = snx_ld_le32(p);
    switch (type) {
        case REL_A64_ADDR32NB:
        case REL_A64_SECREL:
            target = S - (type == REL_A64_ADDR32NB ? base : section_start) + insn;
            if (target > 0xFFFFFFFFu) return SNX_LINK_RANGE;
            snx_st_le32(p, (uint32_t)target);
            return 0;
        case REL_A64_REL32: return snx_link_rel32(p, S + sign_extend(insn, 32), P + 4); /* from the field's end */
        case REL_A64_BRANCH26: return a64_branch(p, S + (sign_extend(insn, 26) << 2) - P, 26, 0);
        case REL_A64_BRANCH19: return a64_branch(p, S + (sign_extend(insn >> 5, 19) << 2) - P, 19, 5);
        case REL_A64_BRANCH14: return a64_branch(p, S + (sign_extend(insn >> 5, 14) << 2) - P, 14, 5);
        case REL_A64_PAGEBASE_REL21: /* pages from P's to the target's: 21 bits reach 4 GiB either way */
            target = snx_link_page_of(S + adr_immediate(insn)) - snx_link_page_of(P);
            if (!fits(target, 33)) return SNX_LINK_RANGE;
            snx_link_adr_field(p, target >> 12);
            return 0;
        case REL_A64_REL21:
            target = S + adr_immediate(insn) - P;
            if (!fits(target, 21)) return SNX_LINK_RANGE;
            snx_link_adr_field(p, target);
            return 0;
        case REL_A64_PAGEOFFSET_12A: snx_link_field(p, S + imm12(insn), 10, 12); return 0;
        case REL_A64_PAGEOFFSET_12L: /* scaled by the access's size, which the address must be a multiple of */
            scale = access_scale(insn);
            target = S + (imm12(insn) << scale);
            if (target & (((uint64_t)1 << scale) - 1)) return SNX_LINK_MISALIGNED;
            snx_link_field(p, (target & 0xFFF) >> scale, 10, 12);
            return 0;
        /* the offset in S's section, each field's part added to the addend it holds, as lld-link writes them */
        case REL_A64_SECREL_HIGH12A:
            offset = S - section_start;
            if (offset >> 24) return SNX_LINK_RANGE;
            snx_link_field(p, imm12(insn) + (offset >> 12), 10, 12);
            return 0;
        case REL_A64_SECREL_LOW12A:
        case REL_A64_SECREL_LOW12L:
            offset = (S - section_start) & 0xFFF;
            scale = type == REL_A64_SECREL_LOW12L ? access_scale(insn) : 0;
            if (offset & (((uint64_t)1 << scale) - 1)) return SNX_LINK_MISALIGNED;
            snx_link_field(p, imm12(insn) + (offset >> scale), 10, 12);
            return 0;
        default: return SNX_LINK_UNSUPPORTED;
    }
}

int snx_coff_a64_veneer(unsigned char* v, uint64_t at, uint64_t target, int indirect) {
    uint64_t pages = snx_link_page_of(target) - snx_link_page_of(at);
    if (!fits(pages, 33)) return SNX_LINK_RANGE;
    if (indirect && (target & 7)) return SNX_LINK_MISALIGNED;
    snx_st_le32(v, 0x90000010u); /* ADRP x16, target */
    snx_link_adr_field(v, pages >> 12);
    if (indirect) {
        snx_st_le32(v + 4, 0xF9400210u); /* LDR x16, [x16, :lo12:target], scaled by 8 */
        snx_link_field(v + 4, (target & 0xFFF) >> 3, 10, 12);
    } else {
        snx_st_le32(v + 4, 0x91000210u); /* ADD x16, x16, :lo12:target */
        snx_link_field(v + 4, target & 0xFFF, 10, 12);
    }
    snx_st_le32(v + 8, 0xD61F0200u); /* BR x16 */
    return 0;
}

/* an ARM64 relocation type that snx_coff_a64_relocate writes, in the relocation loop: above any type's 16 bits */
#define A64_CODE 0x10000u

/* why snx_coff_a64_relocate refused, for a message */
static const char* a64_refusal(int rc) {
    return rc == SNX_LINK_RANGE        ? "its target is out of the instruction's reach"
           : rc == SNX_LINK_MISALIGNED ? "its target is not aligned as the instruction needs"
           : rc == SNX_LINK_OUTSIDE    ? "its field is past the end of its section"
                                       : "the linker does not know its type";
}

enum { O_TEXT, O_RDATA, O_DATA, O_BSS, O_PDATA, O_RELOC, O_COUNT };
static const char* const output_names[O_COUNT] = {".text", ".rdata", ".data", ".bss", ".pdata", ".reloc"};
static const uint32_t output_flags[O_COUNT] = {
    SCN_CODE | SCN_EXECUTE | SCN_READ,        SCN_INITIALIZED | SCN_READ, SCN_INITIALIZED | SCN_READ | SCN_WRITE,
    SCN_UNINITIALIZED | SCN_READ | SCN_WRITE, SCN_INITIALIZED | SCN_READ, SCN_INITIALIZED | SCN_READ | SCN_DISCARDABLE};

typedef struct {
    char name[256];
    const unsigned char* header;
    uint32_t flags, size, raw, relocs, nrelocs;
    int output;    /* O_*, or -1 when not loaded */
    int discarded; /* a later copy of a COMDAT */
    int selection, associate;
    uint32_t rva; /* final */
} Sec;

typedef struct {
    const SnxLinkInput* input;
    const unsigned char* symbols;
    uint32_t nsymbols, nsections;
    const char* strings;
    uint32_t strings_size;
    Sec* sections;
    uint32_t island, veneers; /* ARM64: the island's RVA, and its veneers (count_veneers) */
} Obj;

typedef struct {
    Obj* objs;
    size_t nobjs;
    SnxLinkNames globals; /* name -> object << 32 | symbol index */
    SnxLinkNames comdats;
    SnxLinkNames imports; /* "__imp_X" of a defined X -> pointer slot */
    SnxLinkNames veneer;  /* ARM64: "object:name" -> that object's veneer to name */
    unsigned machine;     /* COFF_X64 or COFF_ARM64, the first object's, which every object has */
    uint32_t imports_rva;
    uint32_t out_rva[O_COUNT], out_size[O_COUNT], out_raw[O_COUNT], out_raw_size[O_COUNT];
    char* error;
    size_t error_size;
} Link;

static int fail(Link* l, const char* format, const char* a, const char* b) {
    return snx_link_fail(l->error, l->error_size, format, a, b);
}

static const unsigned char* symbol(const Obj* o, uint32_t index) { return o->symbols + 18 * (size_t)index; }

static int classify(Sec* s) {
    if (s->flags & (SCN_LNK_REMOVE | SCN_LNK_INFO)) return -1;
    if (!strncmp(s->name, ".debug", 6) || !strncmp(s->name, ".llvm", 5)) return -1;
    if (!strcmp(s->name, ".pdata") || !strncmp(s->name, ".pdata$", 7)) return O_PDATA;
    if (s->flags & SCN_CODE) return O_TEXT;
    if (s->flags & SCN_UNINITIALIZED) return O_BSS;
    if (s->flags & SCN_WRITE) return O_DATA;
    return O_RDATA;
}

static int parse(Link* l, Obj* o, const SnxLinkInput* input) {
    const unsigned char* d = input->data;
    unsigned machine = input->size >= 20 ? snx_ld_le16(d) : 0;
    o->input = input;
    if (input->size < 20 || (machine != COFF_X64 && machine != COFF_ARM64) || snx_ld_le16(d + 16))
        return fail(l, "%s: not an x86-64 or ARM64 COFF object%s", input->name, "");
    if (!l->machine) l->machine = machine;
    if (machine != l->machine)
        return fail(l, "%s: not of the machine of the first object (%s)", input->name,
                    l->machine == COFF_ARM64 ? "ARM64" : "x86-64");
    o->nsections = snx_ld_le16(d + 2);
    uint32_t table = snx_ld_le32(d + 8);
    o->nsymbols = snx_ld_le32(d + 12);
    if (20 + 40 * (size_t)o->nsections > input->size || table + 18 * (size_t)o->nsymbols + 4 > input->size)
        return fail(l, "%s: truncated COFF object%s", input->name, "");
    o->symbols = d + table;
    o->strings = (const char*)d + table + 18 * (size_t)o->nsymbols;
    o->strings_size = snx_ld_le32((const unsigned char*)o->strings);
    o->sections = (Sec*)calloc(o->nsections + 1, sizeof(Sec));
    if (!o->sections) return fail(l, "out of memory%s%s", "", "");
    for (uint32_t i = 1; i <= o->nsections; ++i) {
        Sec* s = &o->sections[i];
        const unsigned char* h = d + 20 + 40 * (size_t)(i - 1);
        s->header = h;
        if (h[0] == '/') {
            unsigned long offset = strtoul((const char*)h + 1, NULL, 10);
            snprintf(s->name, sizeof(s->name), "%s", offset < o->strings_size ? o->strings + offset : "");
        } else {
            memcpy(s->name, h, 8);
            s->name[8] = 0;
        }
        s->size = snx_ld_le32(h + 16);
        s->raw = snx_ld_le32(h + 20);
        s->relocs = snx_ld_le32(h + 24);
        s->nrelocs = snx_ld_le16(h + 32);
        s->flags = snx_ld_le32(h + 36);
        if (s->flags & SCN_NRELOC_OVFL) s->nrelocs = snx_ld_le32(d + s->relocs) - 1, s->relocs += 10;
        if (!(s->flags & SCN_UNINITIALIZED) && (size_t)s->raw + s->size > input->size)
            return fail(l, "%s: truncated section %s", input->name, s->name);
        if ((size_t)s->relocs + 10 * (size_t)s->nrelocs > input->size)
            return fail(l, "%s: truncated relocations in %s", input->name, s->name);
        s->output = classify(s);
    }
    return 0;
}

/* COMDAT: the first object to define a key keeps its section; associative sections follow their section */
static void comdats(Link* l) {
    char name[256];
    int changed;
    for (size_t i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            int16_t section = (int16_t)snx_ld_le16(s + 12);
            if (section <= 0 || (uint32_t)section > o->nsections) continue;
            Sec* sec = &o->sections[section];
            if (!(sec->flags & SCN_LNK_COMDAT)) continue;
            if (s[16] == SYM_STATIC && s[17] && !snx_ld_le32(s + 8) && !sec->selection) {
                /* the section symbol: its auxiliary record has the selection and the associated section */
                sec->selection = s[18 + 14] ? s[18 + 14] : 2;
                sec->associate = snx_ld_le16(s + 18 + 12);
                continue;
            }
            if (sec->selection && sec->selection != SEL_ASSOCIATIVE && s[16] == SYM_EXTERNAL && !sec->discarded &&
                !sec->rva) {
                size_t first;
                snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
                if (snx_link_find(&l->comdats, name, &first) && first != i)
                    sec->discarded = 1;
                else
                    snx_link_insert(&l->comdats, name, i);
                sec->rva = 1; /* key handled */
            }
        }
        for (uint32_t n = 1; n <= o->nsections; ++n) o->sections[n].rva = 0;
    }
    do {
        changed = 0;
        for (size_t i = 0; i < l->nobjs; ++i)
            for (uint32_t n = 1; n <= l->objs[i].nsections; ++n) {
                Sec* s = &l->objs[i].sections[n];
                if (s->selection == SEL_ASSOCIATIVE && !s->discarded && s->associate &&
                    s->associate <= (int)l->objs[i].nsections && l->objs[i].sections[s->associate].discarded) {
                    s->discarded = 1;
                    changed = 1;
                }
            }
    } while (changed);
    for (size_t i = 0; i < l->nobjs; ++i)
        for (uint32_t n = 1; n <= l->objs[i].nsections; ++n)
            if (l->objs[i].sections[n].discarded) l->objs[i].sections[n].output = -1;
}

static int collect_globals(Link* l) {
    size_t found;
    char name[256];
    for (size_t i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            int16_t section = (int16_t)snx_ld_le16(s + 12);
            if (s[16] != SYM_EXTERNAL || section == 0) continue;
            if (section > 0 && ((uint32_t)section > o->nsections || o->sections[section].output < 0)) continue;
            snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
            if (snx_link_find(&l->globals, name, &found)) {
                const Obj* other = &l->objs[found >> 32];
                int16_t other_section = (int16_t)snx_ld_le16(symbol(other, (uint32_t)found) + 12);
                if (section > 0 && (o->sections[section].flags & SCN_LNK_COMDAT) && other_section > 0 &&
                    (other->sections[other_section].flags & SCN_LNK_COMDAT))
                    continue;
                return fail(l, "duplicate symbol %s (%s)", name, o->input->name);
            }
            snx_link_insert(&l->globals, name, (i << 32) | j);
        }
    }
    return 0;
}

/* the RVA (or, absolute, the value) of object o's symbol index */
static int resolve(Link* l, size_t object, uint32_t index, uint64_t* value, int* absolute, int* out_section) {
    const Obj* o = &l->objs[object];
    size_t found;
    char name[256];
    for (int depth = 0; depth < 8; ++depth) {
        if (index >= o->nsymbols) return fail(l, "%s: bad symbol index%s", o->input->name, "");
        const unsigned char* s = symbol(o, index);
        int16_t section = (int16_t)snx_ld_le16(s + 12);
        *absolute = 0;
        *out_section = -1;
        if (section > 0) {
            if ((uint32_t)section > o->nsections) return fail(l, "%s: bad section number%s", o->input->name, "");
            const Sec* sec = &o->sections[section];
            if (sec->output < 0) {
                /* a discarded COMDAT copy: its external symbols resolve to the kept copy */
                snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
                if (s[16] == SYM_EXTERNAL && snx_link_find(&l->globals, name, &found)) {
                    object = found >> 32;
                    index = (uint32_t)found;
                    o = &l->objs[object];
                    continue;
                }
                return fail(l, "%s: reference to discarded section %s", o->input->name, sec->name);
            }
            *value = sec->rva + snx_ld_le32(s + 8);
            *out_section = sec->output;
            return 0;
        }
        if (section == -1) {
            *absolute = 1;
            *value = snx_ld_le32(s + 8);
            return 0;
        }
        snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
        if (snx_link_find(&l->globals, name, &found) && (found >> 32 != object || (uint32_t)found != index)) {
            object = found >> 32;
            index = (uint32_t)found;
            o = &l->objs[object];
            continue;
        }
        if (s[16] == SYM_WEAK_EXTERNAL && s[17]) {
            index = snx_ld_le32(s + 18); /* the default definition */
            continue;
        }
        if (snx_link_find(&l->imports, name, &found)) {
            *value = l->imports_rva + 8 * found;
            *out_section = O_DATA;
            return 0;
        }
        if (!strcmp(name, "__ImageBase")) {
            *value = 0;
            return 0;
        }
        if (!strcmp(name, "_fltused")) {
            *absolute = 1;
            *value = 0x9875;
            return 0;
        }
        if (snx_ld_le32(s + 8)) return fail(l, "%s: common symbol %s is not supported", o->input->name, name);
        return fail(l, "undefined symbol %s (referenced by %s)", name, o->input->name);
    }
    return fail(l, "%s: symbol alias loop%s", o->input->name, "");
}

/* a dllimport declaration of a function the module defines itself (a C library function rtlib.c provides) refers to
   __imp_<name>, a pointer to it: give each one a slot in .data, as lld-link does for locally defined imports */
static void find_imports(Link* l) {
    size_t found;
    char name[256];
    for (size_t i = 0; i < l->nobjs; ++i) {
        const Obj* o = &l->objs[i];
        for (uint32_t j = 0; j < o->nsymbols; j += 1 + o->symbols[18 * (size_t)j + 17]) {
            const unsigned char* s = symbol(o, j);
            if (s[16] != SYM_EXTERNAL || snx_ld_le16(s + 12)) continue;
            snx_coff_symbol_name(o->strings, o->strings_size, s, name, sizeof(name));
            if (strncmp(name, "__imp_", 6) || snx_link_find(&l->imports, name, &found)) continue;
            if (snx_link_find(&l->globals, name + 6, &found)) snx_link_insert(&l->imports, name, l->imports.count);
        }
    }
}
/* ARM64, in a module with islands: each object's veneers, one for each symbol its B and BL name that it does not define
   (another object's) */
static void count_veneers(Link* l) {
    char name[256], key[300];
    size_t found;
    for (size_t i = 0; i < l->nobjs; ++i) {
        Obj* o = &l->objs[i];
        for (uint32_t n = 1; n <= o->nsections; ++n) {
            const Sec* s = &o->sections[n];
            if (s->output != O_TEXT) continue;
            for (uint32_t j = 0; j < s->nrelocs; ++j) {
                const unsigned char* r = o->input->data + s->relocs + 10 * (size_t)j;
                uint32_t index = snx_ld_le32(r + 4);
                if (snx_ld_le16(r + 8) != REL_A64_BRANCH26 || index >= o->nsymbols ||
                    snx_ld_le16(symbol(o, index) + 12))
                    continue;
                snx_coff_symbol_name(o->strings, o->strings_size, symbol(o, index), name, sizeof(name));
                snprintf(key, sizeof(key), "%zu:%s", i, name);
                if (!snx_link_find(&l->veneer, key, &found)) snx_link_insert(&l->veneer, key, o->veneers++);
            }
        }
    }
}

/* An ARM64 B or BL at p (RVA P) in object caller, to S with no addend in its field, which it does not reach: through
   the caller's veneer for its symbol index, written into text (the .text image, at text_rva) to jump there. 0, or
   SNX_LINK_RANGE when the caller has no veneer for the symbol or does not reach its own */
static int veneer_branch(Link* l, size_t caller, uint32_t index, unsigned char* text, uint32_t text_rva,
                         unsigned char* p, uint64_t P, uint64_t S) {
    const Obj* o = &l->objs[caller];
    char name[256], key[300];
    size_t slot;
    if (index >= o->nsymbols || snx_ld_le16(symbol(o, index) + 12)) return SNX_LINK_RANGE; /* its own definition */
    snx_coff_symbol_name(o->strings, o->strings_size, symbol(o, index), name, sizeof(name));
    snprintf(key, sizeof(key), "%zu:%s", caller, name);
    if (!snx_link_find(&l->veneer, key, &slot)) return SNX_LINK_RANGE;
    uint64_t at = o->island + (uint64_t)SNX_COFF_A64_VENEER_SIZE * slot;
    int rc = snx_coff_a64_veneer(text + (at - text_rva), at, S, 0);
    if (rc) return rc;
    return snx_coff_a64_relocate(p, 4, REL_A64_BRANCH26, at, P, 0, 0);
}

/* .reloc's entry for the 8-byte address at rva: 0, or -1 out of memory */
static int add_base(uint32_t** bases, uint32_t* nbases, uint32_t* cap_bases, uint32_t rva) {
    if (*nbases == *cap_bases) {
        uint32_t cap = *cap_bases ? *cap_bases * 2 : 1024;
        uint32_t* grown = (uint32_t*)realloc(*bases, cap * sizeof(**bases));
        if (!grown) return -1;
        *bases = grown;
        *cap_bases = cap;
    }
    (*bases)[(*nbases)++] = rva;
    return 0;
}

static uint32_t section_alignment(const Sec* s) {
    uint32_t bits = (s->flags & SCN_ALIGN_MASK) >> 20;
    return bits ? 1u << (bits - 1) : 16;
}

typedef struct {
    size_t object;
    uint32_t section;
} Order;
static const Link* sort_link;
static int compare_order(const void* a, const void* b) {
    const Order *x = (const Order*)a, *y = (const Order*)b;
    const Sec *p = &sort_link->objs[x->object].sections[x->section],
              *q = &sort_link->objs[y->object].sections[y->section];
    const char *dp = strchr(p->name, '$'), *dq = strchr(q->name, '$');
    /* grouped section names sort by their $ suffix; equal names keep object order */
    int c = strcmp(dp ? dp : "", dq ? dq : "");
    if (c) return c;
    if (x->object != y->object) return x->object < y->object ? -1 : 1;
    return x->section < y->section ? -1 : x->section > y->section;
}
static int compare_u32(const void* a, const void* b) {
    uint32_t x = *(const uint32_t*)a, y = *(const uint32_t*)b;
    return x < y ? -1 : x > y;
}

int snx_link_coff(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                  const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    Link l = {0};
    Order* order = NULL;
    size_t norder = 0;
    uint32_t edata_rva = 0, edata_size = 0, *bases = NULL, nbases = 0, cap_bases = 0;
    SnxLinkBuffer image[O_COUNT] = {{0}}, out = {0};
    const char** sorted = NULL;
    size_t* last_text = NULL; /* ARM64 with islands: each object's last code section in the order */
    int rc = -1, nout = 0, used[O_COUNT];
    *output = (SnxLinkOutput){0};
    l.error = error;
    l.error_size = error_size;
    l.nobjs = count;
    l.objs = (Obj*)calloc(count, sizeof(Obj));
    if (!l.objs) goto done;
    for (size_t i = 0; i < count; ++i)
        if (parse(&l, &l.objs[i], &inputs[i])) goto done;
    comdats(&l);
    if (collect_globals(&l)) goto done;
    find_imports(&l);
    /* order and place the input sections inside each output section */
    for (size_t i = 0; i < count; ++i) norder += l.objs[i].nsections;
    order = (Order*)calloc(norder + 1, sizeof(Order));
    if (!order) goto done;
    norder = 0;
    for (size_t i = 0; i < count; ++i)
        for (uint32_t n = 1; n <= l.objs[i].nsections; ++n)
            if (l.objs[i].sections[n].output >= 0) {
                order[norder].object = i;
                order[norder++].section = n;
            }
    sort_link = &l;
    qsort(order, norder, sizeof(Order), compare_order);
    if (l.machine == COFF_ARM64) {
        uint64_t text = 0;
        for (size_t k = 0; k < norder; ++k) {
            const Sec* s = &l.objs[order[k].object].sections[order[k].section];
            if (s->output == O_TEXT) text = snx_align_up(text, section_alignment(s)) + s->size;
        }
        if (text >= VENEER_TEXT) {
            count_veneers(&l);
            /* each object's last code section in the order, which its island follows */
            last_text = (size_t*)malloc(count * sizeof(*last_text));
            if (!last_text) goto done;
            for (size_t i = 0; i < count; ++i) last_text[i] = norder;
            for (size_t k = 0; k < norder; ++k)
                if (l.objs[order[k].object].sections[order[k].section].output == O_TEXT) last_text[order[k].object] = k;
        }
    }
    for (int o_index = 0; o_index < O_COUNT; ++o_index)
        for (size_t k = 0; k < norder; ++k) {
            Obj* o = &l.objs[order[k].object];
            Sec* s = &o->sections[order[k].section];
            if (s->output != o_index) continue;
            l.out_size[o_index] = snx_align_up(l.out_size[o_index], section_alignment(s));
            s->rva = l.out_size[o_index]; /* offset for now */
            l.out_size[o_index] += s->size;
            if (last_text && last_text[order[k].object] == k && o->veneers) {
                o->island = snx_align_up(l.out_size[O_TEXT], 4); /* offset for now */
                l.out_size[O_TEXT] = o->island + SNX_COFF_A64_VENEER_SIZE * o->veneers;
            }
        }
    if (l.imports.count) {
        l.out_size[O_DATA] = snx_align_up(l.out_size[O_DATA], 8);
        l.imports_rva = l.out_size[O_DATA]; /* offset for now */
        l.out_size[O_DATA] += 8 * (uint32_t)l.imports.count;
    }
    /* the export directory at the end of .rdata */
    if (export_count) {
        sorted = (const char**)malloc(export_count * sizeof(*sorted));
        if (!sorted) goto done;
        memcpy(sorted, exports, export_count * sizeof(*sorted));
        qsort(sorted, export_count, sizeof(*sorted), snx_compare_strings);
        edata_size = 40 + 10 * (uint32_t)export_count + (uint32_t)strlen(soname) + 1;
        for (size_t i = 0; i < export_count; ++i) edata_size += (uint32_t)strlen(sorted[i]) + 1;
        l.out_size[O_RDATA] = snx_align_up(l.out_size[O_RDATA], 4);
        edata_rva = l.out_size[O_RDATA];
        l.out_size[O_RDATA] += edata_size;
    }
    /* RVAs: headers, then the sections that have contents, each on its own pages and without gaps between them,
       which the Windows loader requires */
    uint32_t headers = snx_align_up(0x80 + 4 + 20 + 240 + 40 * O_COUNT, FILE_ALIGNMENT);
    uint32_t rva = SECTION_ALIGNMENT;
    for (int o_index = 0; o_index < O_RELOC; ++o_index) {
        used[o_index] = o_index == O_TEXT || l.out_size[o_index] != 0;
        l.out_rva[o_index] = rva;
        if (used[o_index]) rva = snx_align_up(rva + (l.out_size[o_index] ? l.out_size[o_index] : 1), SECTION_ALIGNMENT);
    }
    for (size_t k = 0; k < norder; ++k) {
        Sec* s = &l.objs[order[k].object].sections[order[k].section];
        s->rva += l.out_rva[s->output];
    }
    for (size_t i = 0; i < count; ++i)
        if (l.objs[i].veneers) l.objs[i].island += l.out_rva[O_TEXT];
    edata_rva += l.out_rva[O_RDATA];
    l.imports_rva += l.out_rva[O_DATA];
    for (int o_index = 0; o_index < O_RELOC; ++o_index)
        if (o_index != O_BSS) snx_link_reserve(&image[o_index], l.out_size[o_index]);
    /* between functions: int3 on x86-64, UDF #0 (zeros) on ARM64 */
    if (l.out_size[O_TEXT]) memset(image[O_TEXT].data, l.machine == COFF_ARM64 ? 0 : 0xCC, l.out_size[O_TEXT]);
    for (size_t k = 0; k < norder; ++k) {
        const Obj* o = &l.objs[order[k].object];
        const Sec* s = &o->sections[order[k].section];
        if (s->output != O_BSS && !(s->flags & SCN_UNINITIALIZED) && s->size)
            memcpy(image[s->output].data + (s->rva - l.out_rva[s->output]), o->input->data + s->raw, s->size);
        else if (s->output != O_BSS && s->size)
            memset(image[s->output].data + (s->rva - l.out_rva[s->output]), 0, s->size);
    }
    /* relocations, addends implicit in the section contents */
    for (size_t k = 0; k < norder; ++k) {
        const Obj* o = &l.objs[order[k].object];
        const Sec* s = &o->sections[order[k].section];
        if (s->output == O_BSS) continue;
        for (uint32_t j = 0; j < s->nrelocs; ++j) {
            const unsigned char* r = o->input->data + s->relocs + 10 * (size_t)j;
            uint32_t at = snx_ld_le32(r), type = snx_ld_le16(r + 8), P = s->rva + at;
            uint64_t S;
            int absolute, target_section;
            if (l.machine == COFF_ARM64) /* its data relocations, written as x86-64's below */
                type = type == REL_A64_ABSOLUTE  ? REL_ABSOLUTE
                       : type == REL_A64_ADDR64  ? REL_ADDR64
                       : type == REL_A64_ADDR32  ? REL_ADDR32
                       : type == REL_A64_SECTION ? REL_SECTION
                                                 : type | A64_CODE;
            if (type == REL_ABSOLUTE) continue;
            if (at + 4 > s->size || (type == REL_ADDR64 && at + 8 > s->size)) {
                snprintf(error, error_size, "%s: relocation outside section %s", o->input->name, s->name);
                goto done;
            }
            if (resolve(&l, order[k].object, snx_ld_le32(r + 4), &S, &absolute, &target_section)) goto done;
            unsigned char* p = image[s->output].data + (P - l.out_rva[s->output]);
            if (type & A64_CODE) {
                uint64_t start = target_section >= 0 ? l.out_rva[target_section] : 0;
                type &= ~A64_CODE;
                /* the branch's addend, which its symbol's one veneer cannot hold for each branch */
                int addend = type == REL_A64_BRANCH26 && (snx_ld_le32(p) & 0x3FFFFFFu);
                int rc = snx_coff_a64_relocate(p, s->size - at, type, S, P, 0, start);
                if (rc == SNX_LINK_RANGE && type == REL_A64_BRANCH26 && last_text && !addend)
                    rc = veneer_branch(&l, order[k].object, snx_ld_le32(r + 4), image[O_TEXT].data, l.out_rva[O_TEXT],
                                       p, P, S);
                if (rc) {
                    snprintf(error, error_size, "%s: ARM64 relocation %u at %s+%#x to RVA %#llx: %s%s", o->input->name,
                             (unsigned)type, s->name, (unsigned)at, (unsigned long long)S, a64_refusal(rc),
                             rc == SNX_LINK_RANGE && addend ? ", and a branch with an addend takes no veneer" : "");
                    goto done;
                }
                continue;
            }
            switch (type) {
                case REL_ADDR64: {
                    uint64_t addend = (uint64_t)snx_ld_le32(p) | (uint64_t)snx_ld_le32(p + 4) << 32;
                    snx_st_le64(p, (absolute ? S : IMAGE_BASE + S) + addend);
                    if (!absolute && add_base(&bases, &nbases, &cap_bases, P)) {
                        snprintf(error, error_size, "out of memory");
                        goto done;
                    }
                    break;
                }
                case REL_ADDR32:
                    if (!absolute) {
                        snprintf(error, error_size, "%s: 32-bit absolute address in %s", o->input->name, s->name);
                        goto done;
                    }
                    snx_st_le32(p, (uint32_t)(S + snx_ld_le32(p)));
                    break;
                case REL_ADDR32NB: snx_st_le32(p, (uint32_t)(S + snx_ld_le32(p))); break;
                case REL_SECTION: snx_st_le16(p, target_section >= 0 ? (uint32_t)target_section + 1 : 0); break;
                case REL_SECREL:
                    snx_st_le32(p, (uint32_t)(S - (target_section >= 0 ? l.out_rva[target_section] : 0)
                                              + snx_ld_le32(p)));
                    break;
                default:
                    if (type >= REL_REL32 && type <= REL_REL32_5) {
                        /* the addend is the field's signed value, relative to the end of the instruction */
                        if (snx_link_rel32(p, S + (uint64_t)(int64_t)(int32_t)snx_ld_le32(p),
                                           P + 4 + (type - REL_REL32))) {
                            snprintf(error, error_size, "%s: relocation out of range", o->input->name);
                            goto done;
                        }
                        break;
                    }
                    snprintf(error, error_size, "%s: unsupported relocation type %u", o->input->name, type);
                    goto done;
            }
        }
    }
    for (size_t k = 0; k < l.imports.capacity; ++k)
        if (l.imports.keys[k]) {
            size_t found;
            uint64_t S;
            int absolute, section;
            uint32_t slot = l.imports_rva + 8 * (uint32_t)l.imports.values[k];
            snx_link_find(&l.globals, l.imports.keys[k] + 6, &found);
            if (resolve(&l, found >> 32, (uint32_t)found, &S, &absolute, &section)) goto done;
            snx_st_le64(image[O_DATA].data + (slot - l.out_rva[O_DATA]), IMAGE_BASE + S);
            if (add_base(&bases, &nbases, &cap_bases, slot)) {
                snprintf(error, error_size, "out of memory");
                goto done;
            }
        }
    /* RtlLookupFunctionEntry binary-searches .pdata: sort the RUNTIME_FUNCTIONs by start address */
    size_t entry = snx_coff_function_size(l.machine);
    if (l.out_size[O_PDATA] % entry == 0)
        qsort(image[O_PDATA].data, l.out_size[O_PDATA] / entry, entry, snx_coff_compare_function);
    /* export directory */
    if (export_count) {
        uint32_t functions = edata_rva + 40, names = functions + 4 * (uint32_t)export_count,
                 ordinals = names + 4 * (uint32_t)export_count, strings = ordinals + 2 * (uint32_t)export_count;
        unsigned char* e = image[O_RDATA].data + (edata_rva - l.out_rva[O_RDATA]);
        memset(e, 0, 40);
        snx_st_le32(e + 12, strings); /* Name */
        snx_st_le32(e + 16, 1);       /* Base */
        snx_st_le32(e + 20, (uint32_t)export_count);
        snx_st_le32(e + 24, (uint32_t)export_count);
        snx_st_le32(e + 28, functions);
        snx_st_le32(e + 32, names);
        snx_st_le32(e + 36, ordinals);
        memcpy(image[O_RDATA].data + (strings - l.out_rva[O_RDATA]), soname, strlen(soname) + 1);
        strings += (uint32_t)strlen(soname) + 1;
        for (size_t i = 0; i < export_count; ++i) {
            size_t found;
            uint64_t S;
            int absolute, section;
            if (!snx_link_find(&l.globals, sorted[i], &found)) {
                snprintf(error, error_size, "exported symbol %s is not defined", sorted[i]);
                goto done;
            }
            if (resolve(&l, found >> 32, (uint32_t)found, &S, &absolute, &section)) goto done;
            snx_st_le32(image[O_RDATA].data + (functions + 4 * i - l.out_rva[O_RDATA]), (uint32_t)S);
            snx_st_le32(image[O_RDATA].data + (names + 4 * i - l.out_rva[O_RDATA]), strings);
            snx_st_le16(image[O_RDATA].data + (ordinals + 2 * i - l.out_rva[O_RDATA]), (uint32_t)i);
            memcpy(image[O_RDATA].data + (strings - l.out_rva[O_RDATA]), sorted[i], strlen(sorted[i]) + 1);
            strings += (uint32_t)strlen(sorted[i]) + 1;
        }
    }
    /* base relocations: one block per 4 KiB page */
    if (nbases) qsort(bases, nbases, sizeof(*bases), compare_u32);
    for (uint32_t j = 0; j < nbases;) {
        uint32_t page = bases[j] & ~0xFFFu, start = (uint32_t)image[O_RELOC].size, entries = 0;
        unsigned char header[8], entry[2];
        snx_link_append(&image[O_RELOC], header, 8);
        for (; j < nbases && (bases[j] & ~0xFFFu) == page; ++j, ++entries) {
            snx_st_le16(entry, (10u << 12) | (bases[j] & 0xFFF)); /* IMAGE_REL_BASED_DIR64 */
            snx_link_append(&image[O_RELOC], entry, 2);
        }
        if (entries & 1) {
            snx_st_le16(entry, 0);
            snx_link_append(&image[O_RELOC], entry, 2);
        }
        snx_st_le32(image[O_RELOC].data + start, page);
        snx_st_le32(image[O_RELOC].data + start + 4, (uint32_t)image[O_RELOC].size - start);
    }
    l.out_size[O_RELOC] = (uint32_t)image[O_RELOC].size;
    l.out_rva[O_RELOC] = rva;
    /* file layout */
    used[O_RELOC] = l.out_size[O_RELOC] != 0;
    if (used[O_RELOC]) rva = snx_align_up(rva + l.out_size[O_RELOC], SECTION_ALIGNMENT);
    for (int o_index = 0; o_index < O_COUNT; ++o_index)
        if (used[o_index]) ++nout;
    snx_link_reserve(&out, headers);
    for (int o_index = 0; o_index < O_COUNT; ++o_index) {
        if (!used[o_index]) continue;
        if (o_index == O_BSS) {
            l.out_raw[o_index] = 0;
            l.out_raw_size[o_index] = 0;
            continue;
        }
        snx_link_align(&out, FILE_ALIGNMENT);
        l.out_raw[o_index] = (uint32_t)snx_link_append(&out, image[o_index].data, l.out_size[o_index]);
        snx_link_align(&out, FILE_ALIGNMENT);
        l.out_raw_size[o_index] = snx_align_up(l.out_size[o_index], FILE_ALIGNMENT);
    }
    unsigned char* p = out.data;
    /* DOS header with e_lfanew; the stub is never run */
    p[0] = 'M';
    p[1] = 'Z';
    snx_st_le32(p + 0x3C, 0x80);
    memcpy(p + 0x80, "PE\0\0", 4);
    p += 0x84;
    snx_st_le16(p, l.machine);
    snx_st_le16(p + 2, (uint32_t)nout);
    snx_st_le16(p + 16, 240);
    snx_st_le16(p + 18, 0x0002 | 0x0020 | 0x2000); /* executable image, large address aware, DLL */
    p += 20;
    snx_st_le16(p, 0x20B);
    p[2] = 14;
    snx_st_le32(p + 4, l.out_raw_size[O_TEXT]);
    snx_st_le32(p + 8, l.out_raw_size[O_RDATA] + l.out_raw_size[O_DATA] + l.out_raw_size[O_PDATA]
                + l.out_raw_size[O_RELOC]);
    snx_st_le32(p + 12, snx_align_up(l.out_size[O_BSS], FILE_ALIGNMENT));
    snx_st_le32(p + 20, l.out_rva[O_TEXT]);
    snx_st_le64(p + 24, IMAGE_BASE);
    snx_st_le32(p + 32, SECTION_ALIGNMENT);
    snx_st_le32(p + 36, FILE_ALIGNMENT);
    snx_st_le16(p + 40, 6);
    snx_st_le16(p + 48, 6);
    snx_st_le32(p + 56, rva); /* SizeOfImage: the end of the last section */
    snx_st_le32(p + 60, headers);
    snx_st_le16(p + 68, 3);                        /* console subsystem */
    snx_st_le16(p + 70, 0x0020 | 0x0040 | 0x0100); /* high entropy VA, dynamic base, NX compatible */
    snx_st_le64(p + 72, 0x100000);
    snx_st_le64(p + 80, 0x1000);
    snx_st_le64(p + 88, 0x100000);
    snx_st_le64(p + 96, 0x1000);
    snx_st_le32(p + 108, 16);
    if (export_count) {
        snx_st_le32(p + 112, edata_rva);
        snx_st_le32(p + 116, edata_size);
    }
    if (l.out_size[O_PDATA]) {
        snx_st_le32(p + 112 + 3 * 8, l.out_rva[O_PDATA]);
        snx_st_le32(p + 116 + 3 * 8, l.out_size[O_PDATA]);
    }
    if (l.out_size[O_RELOC]) {
        snx_st_le32(p + 112 + 5 * 8, l.out_rva[O_RELOC]);
        snx_st_le32(p + 116 + 5 * 8, l.out_size[O_RELOC]);
    }
    p += 240;
    for (int o_index = 0; o_index < O_COUNT; ++o_index) {
        if (!used[o_index]) continue;
        memcpy(p, output_names[o_index], strlen(output_names[o_index]));
        snx_st_le32(p + 8, l.out_size[o_index]);
        snx_st_le32(p + 12, l.out_rva[o_index]);
        snx_st_le32(p + 16, l.out_raw_size[o_index]);
        snx_st_le32(p + 20, l.out_raw[o_index]);
        snx_st_le32(p + 36, output_flags[o_index]);
        p += 40;
    }
    output->data = out.data;
    output->size = out.size;
    out.data = NULL;
    rc = 0;
done:
    for (size_t i = 0; l.objs && i < count; ++i) free(l.objs[i].sections);
    free(l.objs);
    free(order);
    free(bases);
    free(sorted);
    for (int o_index = 0; o_index < O_COUNT; ++o_index) free(image[o_index].data);
    free(out.data);
    snx_link_names_free(&l.globals);
    snx_link_names_free(&l.comdats);
    snx_link_names_free(&l.imports);
    snx_link_names_free(&l.veneer);
    free(last_text);
    return rc;
}
