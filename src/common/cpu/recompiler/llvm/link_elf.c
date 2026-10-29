/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF: x86-64, AArch64, PowerPC64, RISC-V or LoongArch ET_REL objects into an ET_DYN shared object for Linux, FreeBSD
   and Android.
   Layout: [R: headers, .hash .dynsym .dynstr .rela.dyn, read-only data] [RX: .text] [RW: .data .got .dynamic, .bss].
   Every symbol must be defined by one of the objects; the only dynamic relocations are R_*_RELATIVE, for absolute
   addresses in writable data and for GOT slots. The objects are read and relocated by elf_object.c, as the JIT loader
   (code_loader.c) reads and relocates them; the layout, the dynamic sections, the veneer islands and PowerPC64's TOC
   are the module's own. The objects are in the host's byte order, as the module is linked where it runs: a big-endian
   PowerPC64 module is linked on a big-endian host. PowerPC64 comes in both of its ABIs, as the objects' e_flags say:
   ELFv2 and ELFv1, whose function symbols name .opd descriptors (snx_elf_ppc_entry). The module has one TOC, so a call
   never changes r2 and the nop after it stays one. */
#include "common/cpu/recompiler/llvm/link_internal.h"
#include "common/utilities/bits.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(__APPLE__)
/* Windows loads PE and Darwin Mach-O: neither has <elf.h>, and an ELF module would not load there */
int snx_link_elf(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                 const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    (void)inputs;
    (void)count;
    (void)exports;
    (void)export_count;
    (void)soname;
    (void)output;
    snprintf(error, error_size, "ELF output is not supported on this host");
    return -1;
}
#else
#include "common/cpu/recompiler/llvm/elf_object.h"
#ifndef DT_RELACOUNT
#define DT_RELACOUNT 0x6ffffff9
#endif
#ifndef PT_GNU_STACK
#define PT_GNU_STACK 0x6474e551
#endif
/* Below this much code (snx_elf_text_size) every PowerPC64 bl reaches its target (32 MiB), and no object gets veneers;
   AArch64's is SNX_ELF_VENEER_TEXT, which the JIT loader shares */
#define PPC_VENEER_TEXT ((uint64_t)30 << 20)

typedef struct {
    SnxElfObject* objs;
    size_t nobjs;
    SnxLinkNames globals;       /* name -> (object << 32 | symbol index) */
    SnxLinkNames got;           /* "object:symbol" of a symbol's definition -> GOT slot */
    SnxLinkNames veneer;        /* "caller:object:symbol" -> the caller's veneer to that definition */
    unsigned machine, relative; /* EM_X86_64, EM_AARCH64, EM_PPC64, EM_RISCV or EM_LOONGARCH, and its R_*_RELATIVE */
    unsigned abi;               /* PowerPC64's (snx_elf_ppc_abi), RISC-V's and LoongArch's e_flags, 0 elsewhere */
    uint64_t page;              /* segment alignment: AArch64 kernels may use 16 or 64 KiB pages, PowerPC64's 64 */
    uint64_t toc;               /* PowerPC64: .TOC., the base its TOC16 relocations count from */
    uint64_t kind_size[SNX_ELF_KINDS], kind_align[SNX_ELF_KINDS], kind_address[SNX_ELF_KINDS];
    uint64_t got_address;
    size_t got_count, relative_count;
    char* error;
    size_t error_size;
} Link;

static int fail(Link* l, const char* format, const char* a, const char* b) {
    return snx_link_fail(l->error, l->error_size, format, a, b);
}

/* The defining object and symbol of object o's symbol index; *undefined_weak for an unresolved weak reference */
static int definition(Link* l, size_t object, size_t index, size_t* def_object, size_t* def_index,
                      int* undefined_weak) {
    const SnxElfObject* o = &l->objs[object];
    *undefined_weak = 0;
    int found = snx_elf_definition(l->objs, &l->globals, object, index, def_object, def_index, l->error, l->error_size);
    if (found != SNX_ELF_ELSEWHERE) return found;
    const Elf64_Sym* s = &o->syms[index];
    if (ELF64_ST_BIND(s->st_info) == STB_WEAK) {
        *undefined_weak = 1;
        return 0;
    }
    if (!strcmp(snx_elf_symbol_name(o, s), "_GLOBAL_OFFSET_TABLE_") || !strcmp(snx_elf_symbol_name(o, s), ".TOC.")) {
        *def_object = (size_t)-1;
        return 0;
    }
    return fail(l, "undefined symbol %s (referenced by %s)", snx_elf_symbol_name(o, s), o->input->name);
}

/* final address of a definition; 0 for an absolute symbol's value. Object -1 is the linker's own symbol: the GOT, or
   PowerPC64's .TOC. */
static uint64_t symbol_address(Link* l, size_t object, size_t index, int* absolute) {
    *absolute = 0;
    if (object == (size_t)-1) return l->machine == EM_PPC64 ? l->toc : l->got_address;
    return snx_elf_symbol_address(l->objs, object, index, absolute);
}

/* A RISC-V low half's value, the displacement of the HI20 relocation it completes: in the same relocations r (n of
   them, of section s), at the place of the low half's label, the AUIPC. 0, or -1 with the reason */
static int riscv_hi20_value(Link* l, size_t object, const Elf64_Shdr* s, const Elf64_Rela* r, size_t n,
                            const Elf64_Rela* lo, uint64_t* value) {
    const SnxElfObject* o = &l->objs[object];
    size_t index = ELF64_R_SYM(lo->r_info), def_object = 0, def_index = 0, found = 0;
    int weak = 0, absolute;
    if (index >= o->nsyms || o->syms[index].st_shndx != s->sh_info)
        return fail(l, "%s: a RISC-V low half whose AUIPC is not in its section%s", o->input->name, "");
    long hi = snx_elf_riscv_hi20(r, n, o->syms[index].st_value);
    if (hi < 0) return fail(l, "%s: a RISC-V low half without its HI20 relocation%s", o->input->name, "");
    uint64_t P = o->address[s->sh_info] + r[hi].r_offset;
    if (definition(l, object, ELF64_R_SYM(r[hi].r_info), &def_object, &def_index, &weak)) return -1;
    if (ELF64_R_TYPE(r[hi].r_info) == SNX_RV_GOT_HI20) {
        char key[72];
        snx_link_got_key(key, def_object, def_index);
        snx_link_find(&l->got, key, &found);
        *value = l->got_address + 8 * (uint64_t)found + (uint64_t)r[hi].r_addend - P;
    } else
        *value = (weak ? 0 : symbol_address(l, def_object, def_index, &absolute)) + (uint64_t)r[hi].r_addend - P;
    return 0;
}

/* PowerPC64's R_PPC64_TOC, which writes the TOC's address whatever its symbol */
static int toc_relocation(const Link* l, unsigned type) { return l->machine == EM_PPC64 && type == SNX_PPC_TOC; }
/* A relocation that takes no symbol: index 0 (S is 0, and the value absolute), a NONE, and R_PPC64_TOC */
static int symbolless(const Link* l, const Elf64_Rela* r) {
    return !ELF64_R_SYM(r->r_info) || !ELF64_R_TYPE(r->r_info) || toc_relocation(l, ELF64_R_TYPE(r->r_info));
}

/* first pass over relocations: GOT slots, the R_*_RELATIVE count and the veneers each object may need: one per
   AArch64 branch to another object's definition (PowerPC64: any call), once the code is large enough that it may be
   out of reach. x86-64 modules need none, their rel32 calls reaching across the 2 GiB a module may span */
static int scan_relocations(Link* l) {
    size_t def_object, def_index, slot;
    uint64_t text = snx_elf_text_size(l->objs, l->nobjs);
    int weak, far = (l->machine == EM_AARCH64 && text >= SNX_ELF_VENEER_TEXT) ||
                    (l->machine == EM_PPC64 && text >= PPC_VENEER_TEXT);
    char key[72];
    for (size_t i = 0; i < l->nobjs; ++i) {
        SnxElfObject* o = &l->objs[i];
        for (size_t j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = &o->sh[j];
            if (s->sh_type == SHT_REL) return fail(l, "%s: REL relocations are not supported%s", o->input->name, "");
            if (s->sh_type != SHT_RELA || s->sh_info >= o->eh->e_shnum || o->kind[s->sh_info] < 0) continue;
            const Elf64_Rela* r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            size_t n = s->sh_size / sizeof(Elf64_Rela);
            for (size_t k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                if (symbolless(l, &r[k])) {
                    def_object = (size_t)-1;
                    weak = !toc_relocation(l, type); /* nothing to rebase */
                } else if (definition(l, i, ELF64_R_SYM(r[k].r_info), &def_object, &def_index, &weak))
                    return -1;
                if (snx_elf_got_relocation(l->machine, type)) {
                    if (weak) return fail(l, "%s: GOT reference to an undefined weak symbol%s", o->input->name, "");
                    snx_link_got_key(key, def_object, def_index);
                    if (!snx_link_find(&l->got, key, &slot)) {
                        snx_link_insert(&l->got, key, l->got_count++);
                        ++l->relative_count;
                    }
                } else if (far && snx_elf_call_relocation(l->machine, type) && !weak &&
                           (def_object != i || l->machine == EM_PPC64) && def_object != (size_t)-1) {
                    snx_link_veneer_key(key, i, def_object, def_index);
                    if (!snx_link_find(&l->veneer, key, &slot)) snx_link_insert(&l->veneer, key, o->veneers++);
                } else if (snx_elf_absolute_relocation(l->machine, type) && !weak) {
                    if (o->kind[s->sh_info] != SNX_ELF_DATA)
                        return fail(l, "%s: absolute address in read-only section %s", o->input->name,
                                    snx_elf_section_name(o, s->sh_info));
                    ++l->relative_count;
                }
            }
        }
    }
    return 0;
}

static unsigned long elf_hash(const char* name) {
    unsigned long h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000UL;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

/* a hot section (SNX_LINK_HOT_TEXT) and its rank */
typedef struct {
    unsigned long long rank;
    SnxElfPlace place;
} Ranked;
static int by_rank(const void* a, const void* b) {
    const Ranked* x = (const Ranked*)a;
    const Ranked* y = (const Ranked*)b;
    if (x->rank != y->rank) return x->rank < y->rank ? -1 : 1;
    if (x->place.object != y->place.object) return x->place.object < y->place.object ? -1 : 1;
    return (x->place.section > y->place.section) - (x->place.section < y->place.section);
}

/* the code's order for snx_elf_layout: the hot sections by their ranks, then the others in object and section order.
   *order stays NULL (object order) without hot sections, or when an object has veneers. 0, or -1 */
static int text_order(Link* l, SnxElfPlace** order) {
    size_t hot = 0, total = 0, n = 0, prefix = strlen(SNX_LINK_HOT_TEXT);
    Ranked* ranked;
    *order = NULL;
    for (size_t i = 0; i < l->nobjs; ++i) {
        const SnxElfObject* o = &l->objs[i];
        if (o->veneers) return 0;
        for (size_t j = 0; j < o->eh->e_shnum; ++j)
            if (o->kind[j] == SNX_ELF_TEXT) {
                ++total;
                hot += !strncmp(snx_elf_section_name(o, j), SNX_LINK_HOT_TEXT, prefix);
            }
    }
    if (!hot) return 0;
    ranked = (Ranked*)malloc(hot * sizeof(*ranked));
    *order = (SnxElfPlace*)malloc(total * sizeof(**order));
    if (!ranked || !*order) {
        free(ranked);
        return fail(l, "out of memory%s%s", "", "");
    }
    for (size_t i = 0; i < l->nobjs; ++i)
        for (size_t j = 0; j < l->objs[i].eh->e_shnum; ++j) {
            const char* name = snx_elf_section_name(&l->objs[i], j);
            if (l->objs[i].kind[j] != SNX_ELF_TEXT || strncmp(name, SNX_LINK_HOT_TEXT, prefix)) continue;
            ranked[n].rank = strtoull(name + prefix, NULL, 10);
            ranked[n].place.object = i;
            ranked[n++].place.section = j;
        }
    qsort(ranked, hot, sizeof(*ranked), by_rank);
    for (n = 0; n < hot; ++n) (*order)[n] = ranked[n].place;
    free(ranked);
    for (size_t i = 0; i < l->nobjs; ++i)
        for (size_t j = 0; j < l->objs[i].eh->e_shnum; ++j)
            if (l->objs[i].kind[j] == SNX_ELF_TEXT &&
                strncmp(snx_elf_section_name(&l->objs[i], j), SNX_LINK_HOT_TEXT, prefix)) {
                (*order)[n].object = i;
                (*order)[n++].section = j;
            }
    return 0;
}

/* the output's section headers, for tools: debuggers and objdump find the code and the symbols */
enum {
    S_NULL,
    S_HASH,
    S_DYNSYM,
    S_DYNSTR,
    S_RELA,
    S_RODATA,
    S_TEXT,
    S_DATA,
    S_GOT,
    S_DYNAMIC,
    S_BSS,
    S_SYMTAB,
    S_STRTAB,
    S_SHSTRTAB,
    S_COUNT
};
static const char* const section_names[S_COUNT] = {"",        ".hash",   ".dynsym", ".dynstr",  ".rela.dyn",
                                                   ".rodata", ".text",   ".data",   ".got",     ".dynamic",
                                                   ".bss",    ".symtab", ".strtab", ".shstrtab"};
static const int kind_section[SNX_ELF_KINDS] = {S_RODATA, S_TEXT, S_DATA, S_BSS};

int snx_link_elf(const SnxLinkInput* inputs, size_t count, const char* const* exports, size_t export_count,
                 const char* soname, SnxLinkOutput* output, char* error, size_t error_size) {
    Link l = {0};
    SnxLinkBuffer out = {0}, dynstr = {0}, strtab = {0}, symtab = {0}, rela = {0};
    Elf64_Shdr shdr[S_COUNT];
    Elf64_Phdr ph[5];
    Elf64_Ehdr eh;
    size_t nsyms = export_count + 1, ndyn = 0;
    int rc = -1;
    Elf64_Dyn dyn[16];
    SnxElfPlace* text = NULL;
    (void)soname;
    *output = (SnxLinkOutput){0};
    l.error = error;
    l.error_size = error_size;
    l.nobjs = count;
    l.objs = (SnxElfObject*)calloc(count, sizeof(SnxElfObject));
    uint64_t* export_address = (uint64_t*)calloc(export_count + 1, sizeof(uint64_t));
    if (!l.objs || !export_address) goto done;
    for (size_t i = 0; i < count; ++i)
        if (snx_elf_parse(&l.objs[i], &inputs[i], &l.machine, &l.abi, error, error_size)) goto done;
    l.relative = snx_elf_relative_type(l.machine);
    l.page = l.machine == EM_X86_64 ? 0x1000 : 0x10000;
    if (snx_elf_collect_globals(l.objs, count, NULL, &l.globals, error, error_size) || scan_relocations(&l) ||
        text_order(&l, &text))
        goto done;
    snx_elf_layout(l.objs, count, text, l.kind_size, l.kind_align);
    /* dynamic symbol names */
    snx_link_append(&dynstr, "", 1);
    for (size_t i = 0; i < export_count; ++i) snx_link_append(&dynstr, exports[i], strlen(exports[i]) + 1);
    uint64_t nbucket = export_count ? export_count : 1;
    /* addresses */
    uint64_t hash_off = snx_align_up(sizeof(Elf64_Ehdr) + sizeof(ph), 8);
    uint64_t dynsym_off = snx_align_up(hash_off + 4 * (2 + nbucket + nsyms), 8);
    uint64_t dynstr_off = dynsym_off + nsyms * sizeof(Elf64_Sym);
    uint64_t rela_off = snx_align_up(dynstr_off + dynstr.size, 8);
    uint64_t rodata_off = snx_align_up(rela_off + l.relative_count * sizeof(Elf64_Rela), l.kind_align[SNX_ELF_RODATA]);
    uint64_t end_r = rodata_off + l.kind_size[SNX_ELF_RODATA];
    uint64_t text_off = snx_align_up(end_r, l.page);
    uint64_t data_off = snx_align_up(snx_align_up(text_off + l.kind_size[SNX_ELF_TEXT], l.page),
                                     l.kind_align[SNX_ELF_DATA]);
    uint64_t got_off = snx_align_up(data_off + l.kind_size[SNX_ELF_DATA], 8);
    uint64_t dynamic_off = got_off + 8 * l.got_count;
    uint64_t end_data = dynamic_off + sizeof(dyn);
    uint64_t bss_off = snx_align_up(end_data, l.kind_align[SNX_ELF_BSS]);
    l.kind_address[SNX_ELF_RODATA] = rodata_off;
    l.kind_address[SNX_ELF_TEXT] = text_off;
    l.kind_address[SNX_ELF_DATA] = data_off;
    l.kind_address[SNX_ELF_BSS] = bss_off;
    l.got_address = got_off;
    /* PowerPC64's TOC16 relocations reach 2 GiB either side of it in pairs (@ha, @l): the whole module */
    l.toc = data_off + 0x8000;
    for (size_t i = 0; i < count; ++i)
        for (size_t j = 0; j < l.objs[i].eh->e_shnum; ++j)
            if (l.objs[i].kind[j] >= 0)
                l.objs[i].address[j] = l.kind_address[l.objs[i].kind[j]] + l.objs[i].position[j];
    snx_link_reserve(&out, end_data);
    /* section contents */
    for (size_t i = 0; i < count; ++i) {
        SnxElfObject* o = &l.objs[i];
        for (size_t j = 0; j < o->eh->e_shnum; ++j)
            if (o->kind[j] >= 0 && o->kind[j] != SNX_ELF_BSS && o->sh[j].sh_size)
                memcpy(out.data + o->address[j], o->input->data + o->sh[j].sh_offset, o->sh[j].sh_size);
    }
    /* GOT slots: the address of their symbol, relocated by the loader */
    for (size_t i = 0; i < l.got.capacity; ++i)
        if (l.got.keys[i]) {
            unsigned long object, index;
            int absolute;
            uint64_t slot = got_off + 8 * l.got.values[i];
            Elf64_Rela r;
            sscanf(l.got.keys[i], "%lu:%lu", &object, &index);
            uint64_t value = symbol_address(&l, object, index, &absolute);
            snx_st_le64(out.data + slot, value);
            r.r_offset = slot;
            r.r_info = ELF64_R_INFO((Elf64_Xword)0, l.relative);
            r.r_addend = (int64_t)value;
            snx_link_append(&rela, &r, sizeof(r));
        }
    /* relocations, through elf_object.c as the JIT loader writes them; the module's own parts are the RELATIVE
       entries for absolute addresses, the veneers of branches out of reach and PowerPC64's TOC */
    for (size_t i = 0; i < count; ++i) {
        SnxElfObject* o = &l.objs[i];
        for (size_t j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = &o->sh[j];
            size_t def_object = 0, def_index = 0;
            if (s->sh_type != SHT_RELA || s->sh_info >= o->eh->e_shnum || o->kind[s->sh_info] < 0) continue;
            const Elf64_Rela* r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            size_t n = s->sh_size / sizeof(Elf64_Rela);
            for (size_t k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                uint64_t P = o->address[s->sh_info] + r[k].r_offset, S = 0, slot = 0, entry = 0;
                uint64_t got = l.machine == EM_PPC64 ? l.toc : got_off; /* PowerPC64's TOC (elf_object.h) */
                int64_t A = r[k].r_addend;
                int weak = 0, absolute = 1, written = SNX_LINK_OUTSIDE;
                char key[72];
                size_t found = 0;
                if (symbolless(&l, &r[k])) {
                    def_object = (size_t)-1; /* no veneer or GOT slot of the last relocation's symbol */
                    absolute = !toc_relocation(&l, type);
                } else if (definition(&l, i, ELF64_R_SYM(r[k].r_info), &def_object, &def_index, &weak))
                    goto done;
                else if (!weak)
                    S = symbol_address(&l, def_object, def_index, &absolute);
                if (l.machine == EM_PPC64 && snx_elf_call_relocation(l.machine, type) && !symbolless(&l, &r[k])) {
                    /* to the entry point: the code an ELFv1 descriptor names, ELFv2's local entry. A weak call
                       has nowhere to go */
                    if (weak) {
                        snprintf(error, error_size, "%s: a call to an undefined weak symbol", o->input->name);
                        goto done;
                    }
                    entry = S + (uint64_t)A;
                    if (def_object != (size_t)-1 &&
                        snx_elf_ppc_entry(l.objs, &l.globals, l.abi, def_object, def_index, S, A, &entry, &S, error,
                                          error_size))
                        goto done;
                    if (def_object == (size_t)-1) S = entry;
                    A = 0;
                }
                if (snx_elf_got_relocation(l.machine, type)) {
                    snx_link_got_key(key, def_object, def_index);
                    snx_link_find(&l.got, key, &found);
                    slot = got_off + 8 * (uint64_t)found;
                }
                if (snx_elf_riscv_lo12(l.machine, type) && riscv_hi20_value(&l, i, s, r, n, &r[k], &S)) goto done;
                if (r[k].r_offset <= o->sh[s->sh_info].sh_size) {
                    unsigned char* p = out.data + P;
                    written = snx_elf_relocate(l.machine, p, o->sh[s->sh_info].sh_size - r[k].r_offset, type, S, A, P,
                                               got, slot);
                    if (written == SNX_LINK_RANGE && snx_elf_call_relocation(l.machine, type) && !weak) {
                        /* the caller's veneer, which it reaches, jumps there: PowerPC64's to the global entry, as
                           through a pointer */
                        snx_link_veneer_key(key, i, def_object, def_index);
                        if (snx_link_find(&l.veneer, key, &found)) {
                            if (l.machine == EM_PPC64) {
                                uint64_t at = l.kind_address[SNX_ELF_TEXT] + o->island +
                                     SNX_ELF_PPC_VENEER_SIZE * (uint64_t)found;
                                written = snx_elf_ppc_branch_veneer(p, P, out.data + at, at, entry, l.toc);
                            } else {
                                uint64_t at = l.kind_address[SNX_ELF_TEXT] + o->island
                                    + SNX_ELF_VENEER_SIZE * (uint64_t)found;
                                written = snx_elf_branch_veneer(p, P, out.data + at, at, S + (uint64_t)A, 0);
                            }
                        }
                    }
                }
                if (!written && snx_elf_absolute_relocation(l.machine, type) && !weak && !absolute) {
                    Elf64_Rela d;
                    d.r_offset = P;
                    d.r_info = ELF64_R_INFO((Elf64_Xword)0, l.relative);
                    d.r_addend = (int64_t)(toc_relocation(&l, type) ? l.toc : S + (uint64_t)A);
                    snx_link_append(&rela, &d, sizeof(d));
                }
                if (written == SNX_LINK_RANGE)
                    snprintf(error, error_size, "%s: relocation %u out of range (the module is too large)",
                             o->input->name, type);
                else if (written == SNX_LINK_MISALIGNED)
                    snprintf(error, error_size, "%s: relocation %u to a misaligned address", o->input->name, type);
                else if (written == SNX_LINK_OUTSIDE)
                    snprintf(error, error_size, "%s: relocation outside its section", o->input->name);
                else if (written)
                    snprintf(error, error_size, "%s: unsupported relocation type %u", o->input->name, type);
                if (written) goto done;
            }
        }
    }
    /* exports */
    for (size_t i = 0; i < export_count; ++i) {
        size_t found;
        int absolute;
        if (!snx_link_find(&l.globals, exports[i], &found)) {
            snprintf(error, error_size, "exported symbol %s is not defined", exports[i]);
            goto done;
        }
        export_address[i] = symbol_address(&l, found >> 32, found & 0xFFFFFFFFu, &absolute);
    }
    /* .hash: nbucket, nchain, buckets, chains, words in the host's order as the dynamic loader reads them (a
       big-endian PowerPC64 host's too) */
    unsigned char* p = out.data + hash_off;
    uint32_t word = (uint32_t)nbucket;
    memcpy(p, &word, 4);
    word = (uint32_t)nsyms;
    memcpy(p + 4, &word, 4);
    for (size_t i = 0; i < export_count; ++i) {
        unsigned char* bucket = p + 8 + 4 * (elf_hash(exports[i]) % nbucket);
        memcpy(p + 8 + 4 * nbucket + 4 * (i + 1), bucket, 4); /* the chain goes on to the bucket's previous head */
        word = (uint32_t)(i + 1);
        memcpy(bucket, &word, 4);
    }
    /* .dynsym */
    for (size_t i = 0, k = 1; i < export_count; ++i) {
        Elf64_Sym s;
        memset(&s, 0, sizeof(s));
        s.st_name = (uint32_t)k;
        k += strlen(exports[i]) + 1;
        s.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
        s.st_shndx = S_TEXT;
        s.st_value = export_address[i];
        memcpy(out.data + dynsym_off + (i + 1) * sizeof(Elf64_Sym), &s, sizeof(s));
    }
    memcpy(out.data + dynstr_off, dynstr.data, dynstr.size);
    if (rela.size) /* none made: no buffer, which memcpy can't take even for 0 bytes */
        memcpy(out.data + rela_off, rela.data, rela.size);
    /* .dynamic */
#define DYN(tag, value)                                                                                                \
    do {                                                                                                               \
        dyn[ndyn].d_tag = (tag);                                                                                       \
        dyn[ndyn++].d_un.d_val = (value);                                                                              \
    } while (0)
    DYN(DT_HASH, hash_off);
    DYN(DT_STRTAB, dynstr_off);
    DYN(DT_SYMTAB, dynsym_off);
    DYN(DT_STRSZ, dynstr.size);
    DYN(DT_SYMENT, sizeof(Elf64_Sym));
    if (rela.size) {
        DYN(DT_RELA, rela_off);
        DYN(DT_RELASZ, rela.size);
        DYN(DT_RELAENT, sizeof(Elf64_Rela));
        DYN(DT_RELACOUNT, rela.size / sizeof(Elf64_Rela));
    }
    while (ndyn < sizeof(dyn) / sizeof(dyn[0])) DYN(DT_NULL, 0);
#undef DYN
    memcpy(out.data + dynamic_off, dyn, sizeof(dyn));
    /* program headers */
    memset(ph, 0, sizeof(ph));
    ph[0].p_type = PT_LOAD;
    ph[0].p_flags = PF_R;
    ph[0].p_filesz = ph[0].p_memsz = end_r;
    ph[0].p_align = l.page;
    ph[1].p_type = PT_LOAD;
    ph[1].p_flags = PF_R | PF_X;
    ph[1].p_offset = ph[1].p_vaddr = ph[1].p_paddr = text_off;
    ph[1].p_filesz = ph[1].p_memsz = l.kind_size[SNX_ELF_TEXT];
    ph[1].p_align = l.page;
    ph[2].p_type = PT_LOAD;
    ph[2].p_flags = PF_R | PF_W;
    ph[2].p_offset = ph[2].p_vaddr = ph[2].p_paddr = data_off;
    ph[2].p_filesz = end_data - data_off;
    ph[2].p_memsz = bss_off + l.kind_size[SNX_ELF_BSS] - data_off;
    ph[2].p_align = l.page;
    ph[3].p_type = PT_DYNAMIC;
    ph[3].p_flags = PF_R | PF_W;
    ph[3].p_offset = ph[3].p_vaddr = ph[3].p_paddr = dynamic_off;
    ph[3].p_filesz = ph[3].p_memsz = sizeof(dyn);
    ph[3].p_align = 8;
    ph[4].p_type = PT_GNU_STACK; /* without it, the loader makes the stack executable */
    ph[4].p_flags = PF_R | PF_W;
    ph[4].p_align = 16;
    memcpy(out.data + sizeof(Elf64_Ehdr), ph, sizeof(ph));
    /* .symtab: every named function and object, for debuggers and crash reports; the exports last, global */
    snx_link_append(&strtab, "", 1);
    {
        Elf64_Sym s;
        memset(&s, 0, sizeof(s));
        snx_link_append(&symtab, &s, sizeof(s));
    }
    for (size_t i = 0; i < count; ++i) {
        SnxElfObject* o = &l.objs[i];
        for (size_t j = 1; j < o->nsyms; ++j) {
            const Elf64_Sym* src = &o->syms[j];
            int type = ELF64_ST_TYPE(src->st_info);
            Elf64_Sym s;
            size_t found;
            if ((type != STT_FUNC && type != STT_OBJECT) || src->st_shndx == SHN_UNDEF ||
                src->st_shndx >= SHN_LORESERVE || o->kind[src->st_shndx] < 0 || !o->strtab[src->st_name])
                continue;
            if (ELF64_ST_BIND(src->st_info) != STB_LOCAL &&
                snx_link_find(&l.globals, snx_elf_symbol_name(o, src), &found) && found != ((i << 32) | j))
                continue; /* an overridden weak definition */
            memset(&s, 0, sizeof(s));
            s.st_name = (uint32_t)snx_link_append(&strtab, snx_elf_symbol_name(o, src),
                                                  strlen(snx_elf_symbol_name(o, src)) + 1);
            s.st_info = ELF64_ST_INFO(STB_LOCAL, type);
            s.st_other = STV_DEFAULT;
            s.st_shndx = (uint16_t)kind_section[o->kind[src->st_shndx]];
            s.st_value = o->address[src->st_shndx] + src->st_value;
            s.st_size = src->st_size;
            snx_link_append(&symtab, &s, sizeof(s));
        }
    }
    size_t symbol_count = symtab.size / sizeof(Elf64_Sym);
    for (size_t i = 0; i < export_count; ++i) {
        Elf64_Sym s;
        memset(&s, 0, sizeof(s));
        s.st_name = (uint32_t)snx_link_append(&strtab, exports[i], strlen(exports[i]) + 1);
        s.st_info = ELF64_ST_INFO(STB_GLOBAL, STT_FUNC);
        s.st_shndx = S_TEXT;
        s.st_value = export_address[i];
        snx_link_append(&symtab, &s, sizeof(s));
    }
    /* trailing, unloaded: symbol table, strings, section names, section headers */
    memset(shdr, 0, sizeof(shdr));
    snx_link_align(&out, 8);
    shdr[S_SYMTAB].sh_offset = snx_link_append(&out, symtab.data, symtab.size);
    shdr[S_STRTAB].sh_offset = snx_link_append(&out, strtab.data, strtab.size);
    {
        SnxLinkBuffer names = {0};
        for (size_t i = 0; i < S_COUNT; ++i)
            shdr[i].sh_name = (uint32_t)snx_link_append(&names, section_names[i], strlen(section_names[i]) + 1);
        shdr[S_SHSTRTAB].sh_offset = snx_link_append(&out, names.data, names.size);
        shdr[S_SHSTRTAB].sh_size = names.size;
        free(names.data);
    }
#define SECTION(i, type, flags, address, size, link, info, align, entsize)                                             \
    do {                                                                                                               \
        shdr[i].sh_type = (type);                                                                                      \
        shdr[i].sh_flags = (flags);                                                                                    \
        shdr[i].sh_addr = (address);                                                                                   \
        if ((type) != SHT_SYMTAB && (type) != SHT_STRTAB) shdr[i].sh_offset = (address);                               \
        if (i == S_STRTAB || i == S_SYMTAB || i == S_SHSTRTAB) shdr[i].sh_addr = 0;                                    \
        shdr[i].sh_size = (size);                                                                                      \
        shdr[i].sh_link = (link);                                                                                      \
        shdr[i].sh_info = (info);                                                                                      \
        shdr[i].sh_addralign = (align);                                                                                \
        shdr[i].sh_entsize = (entsize);                                                                                \
    } while (0)
    SECTION(S_HASH, SHT_HASH, SHF_ALLOC, hash_off, 4 * (2 + nbucket + nsyms), S_DYNSYM, 0, 8, 4);
    SECTION(S_DYNSYM, SHT_DYNSYM, SHF_ALLOC, dynsym_off, nsyms * sizeof(Elf64_Sym), S_DYNSTR, 1, 8, sizeof(Elf64_Sym));
    SECTION(S_DYNSTR, SHT_STRTAB, SHF_ALLOC, dynstr_off, dynstr.size, 0, 0, 1, 0);
    shdr[S_DYNSTR].sh_offset = dynstr_off;
    SECTION(S_RELA, SHT_RELA, SHF_ALLOC, rela_off, rela.size, S_DYNSYM, 0, 8, sizeof(Elf64_Rela));
    SECTION(S_RODATA, SHT_PROGBITS, SHF_ALLOC, rodata_off, l.kind_size[SNX_ELF_RODATA], 0, 0,
            l.kind_align[SNX_ELF_RODATA], 0);
    SECTION(S_TEXT, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR, text_off, l.kind_size[SNX_ELF_TEXT], 0, 0, 16, 0);
    SECTION(S_DATA, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, data_off, l.kind_size[SNX_ELF_DATA], 0, 0,
            l.kind_align[SNX_ELF_DATA], 0);
    SECTION(S_GOT, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE, got_off, 8 * l.got_count, 0, 0, 8, 8);
    SECTION(S_DYNAMIC, SHT_DYNAMIC, SHF_ALLOC | SHF_WRITE, dynamic_off, sizeof(dyn), S_DYNSTR, 0, 8, sizeof(Elf64_Dyn));
    SECTION(S_BSS, SHT_NOBITS, SHF_ALLOC | SHF_WRITE, bss_off, l.kind_size[SNX_ELF_BSS], 0, 0,
            l.kind_align[SNX_ELF_BSS], 0);
    shdr[S_BSS].sh_offset = end_data;
    SECTION(S_SYMTAB, SHT_SYMTAB, 0, 0, symtab.size, S_STRTAB, (uint32_t)symbol_count, 8, sizeof(Elf64_Sym));
    SECTION(S_STRTAB, SHT_STRTAB, 0, 0, strtab.size, 0, 0, 1, 0);
    shdr[S_SHSTRTAB].sh_type = SHT_STRTAB;
    shdr[S_SHSTRTAB].sh_addralign = 1;
#undef SECTION
    snx_link_align(&out, 8);
    /* ELF header */
    memset(&eh, 0, sizeof(eh));
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = snx_elf_host_data();
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_ident[EI_OSABI] = ELFOSABI_NONE;
    eh.e_type = ET_DYN;
    eh.e_machine = (Elf64_Half)l.machine;
    eh.e_version = EV_CURRENT;
    /* PowerPC64's ABI, and RISC-V's and LoongArch's float ABI, which the loader checks */
    eh.e_flags =
        l.machine == EM_PPC64 || l.machine == EM_RISCV || l.machine == EM_LOONGARCH ? l.objs[0].eh->e_flags : 0;
    eh.e_phoff = sizeof(Elf64_Ehdr);
    eh.e_ehsize = sizeof(Elf64_Ehdr);
    eh.e_phentsize = sizeof(Elf64_Phdr);
    eh.e_phnum = sizeof(ph) / sizeof(ph[0]);
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = S_COUNT;
    eh.e_shstrndx = S_SHSTRTAB;
    eh.e_shoff = snx_link_append(&out, shdr, sizeof(shdr));
    memcpy(out.data, &eh, sizeof(eh));
    output->data = out.data;
    output->size = out.size;
    out.data = NULL;
    rc = 0;
done:
    for (size_t i = 0; l.objs && i < count; ++i) snx_elf_free(&l.objs[i]);
    free(l.objs);
    free(text);
    free(export_address);
    snx_link_names_free(&l.globals);
    snx_link_names_free(&l.got);
    snx_link_names_free(&l.veneer);
    free(out.data);
    free(dynstr.data);
    free(strtab.data);
    free(symtab.data);
    free(rela.data);
    return rc;
}
#endif
