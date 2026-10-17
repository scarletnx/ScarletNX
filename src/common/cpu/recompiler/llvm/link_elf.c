/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ELF: x86-64 or AArch64 ET_REL objects into an ET_DYN shared object for Linux, FreeBSD and Android.
   Layout: [R: headers, .hash .dynsym .dynstr .rela.dyn, read-only data] [RX: .text] [RW: .data .got .dynamic, .bss].
   Every symbol must be defined by one of the objects; the only dynamic relocations are R_*_RELATIVE, for absolute
   addresses in writable data and for GOT slots. The objects are read and relocated by elf_object.c, as the JIT loader
   (code_loader.c) reads and relocates them; the layout, the dynamic sections and the AArch64 veneer islands are the
   module's own. */
#include "common/cpu/recompiler/llvm/link.h"
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
/* Below this much code every B and BL reaches its target (128 MiB), and no object gets veneers (elf_object.h) */
#define VENEER_TEXT ((uint64_t)120 << 20)

typedef struct {
    SnxElfObject* objs;
    size_t nobjs;
    SnxLinkNames globals;       /* name -> (object << 32 | symbol index) */
    SnxLinkNames got;           /* "object:symbol" of a symbol's definition -> GOT slot */
    SnxLinkNames veneer;        /* "caller:object:symbol" -> the caller's veneer to that definition */
    unsigned machine, relative; /* EM_X86_64 or EM_AARCH64, and its R_*_RELATIVE */
    uint64_t page;              /* segment alignment: AArch64 kernels may use 16 or 64 KiB pages */
    uint64_t kind_size[SNX_ELF_KINDS], kind_align[SNX_ELF_KINDS], kind_address[SNX_ELF_KINDS];
    uint64_t got_address;
    size_t got_count, relative_count;
    char* error;
    size_t error_size;
} Link;

static int fail(Link* l, const char* format, const char* a, const char* b) {
    snprintf(l->error, l->error_size, format, a, b);
    return -1;
}

/* The defining object and symbol of object o's symbol index; *undefined_weak for an unresolved weak reference */
static int definition(Link* l, size_t object, size_t index, size_t* def_object, size_t* def_index,
                      int* undefined_weak) {
    const SnxElfObject* o = &l->objs[object];
    const Elf64_Sym* s;
    int found;
    *undefined_weak = 0;
    found = snx_elf_definition(l->objs, &l->globals, object, index, def_object, def_index, l->error, l->error_size);
    if (found != SNX_ELF_ELSEWHERE) return found;
    s = &o->syms[index];
    if (ELF64_ST_BIND(s->st_info) == STB_WEAK) {
        *undefined_weak = 1;
        return 0;
    }
    if (!strcmp(snx_elf_symbol_name(o, s), "_GLOBAL_OFFSET_TABLE_")) {
        *def_object = (size_t)-1;
        return 0;
    }
    return fail(l, "undefined symbol %s (referenced by %s)", snx_elf_symbol_name(o, s), o->input->name);
}

/* final address of a definition; 0 for an absolute symbol's value */
static uint64_t symbol_address(Link* l, size_t object, size_t index, int* absolute) {
    *absolute = 0;
    if (object == (size_t)-1) return l->got_address;
    return snx_elf_symbol_address(l->objs, object, index, absolute);
}

static void got_key(char* key, size_t object, size_t index) {
    sprintf(key, "%lu:%lu", (unsigned long)object, (unsigned long)index);
}
static void veneer_key(char* key, size_t caller, size_t object, size_t index) {
    sprintf(key, "%lu:%lu:%lu", (unsigned long)caller, (unsigned long)object, (unsigned long)index);
}

/* text before veneers, which only a module this large needs */
static uint64_t text_size(const Link* l) {
    uint64_t size = 0;
    size_t i, j;
    for (i = 0; i < l->nobjs; ++i)
        for (j = 0; j < l->objs[i].eh->e_shnum; ++j)
            if (l->objs[i].kind[j] == SNX_ELF_TEXT) size += l->objs[i].sh[j].sh_size + l->objs[i].sh[j].sh_addralign;
    return size;
}

/* first pass over relocations: GOT slots, the R_*_RELATIVE count and the veneers each object may need: one per
   AArch64 branch to another object's definition, once the code is large enough that it may be out of reach. x86-64
   modules need none, their rel32 calls reaching across the 2 GiB a module may span */
static int scan_relocations(Link* l) {
    size_t i, j, k, def_object, def_index, slot;
    int weak, far = l->machine == EM_AARCH64 && text_size(l) >= VENEER_TEXT;
    char key[72];
    for (i = 0; i < l->nobjs; ++i) {
        SnxElfObject* o = &l->objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = &o->sh[j];
            const Elf64_Rela* r;
            size_t n;
            if (s->sh_type == SHT_REL) return fail(l, "%s: REL relocations are not supported%s", o->input->name, "");
            if (s->sh_type != SHT_RELA || s->sh_info >= o->eh->e_shnum || o->kind[s->sh_info] < 0) continue;
            r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            n = s->sh_size / sizeof(Elf64_Rela);
            for (k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                if (definition(l, i, ELF64_R_SYM(r[k].r_info), &def_object, &def_index, &weak)) return -1;
                if (snx_elf_got_relocation(l->machine, type)) {
                    if (weak) return fail(l, "%s: GOT reference to an undefined weak symbol%s", o->input->name, "");
                    got_key(key, def_object, def_index);
                    if (!snx_link_find(&l->got, key, &slot)) {
                        snx_link_insert(&l->got, key, l->got_count++);
                        ++l->relative_count;
                    }
                } else if (far && snx_elf_call_relocation(l->machine, type) && !weak && def_object != i &&
                           def_object != (size_t)-1) {
                    veneer_key(key, i, def_object, def_index);
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

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

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
    Link l;
    SnxLinkBuffer out, dynstr, strtab, symtab, rela;
    Elf64_Shdr shdr[S_COUNT];
    Elf64_Phdr ph[5];
    Elf64_Ehdr eh;
    uint64_t hash_off, dynsym_off, dynstr_off, rela_off, rodata_off, text_off, data_off, got_off, dynamic_off, bss_off,
        end_r, end_data, nbucket, *export_address;
    size_t i, j, k, nsyms = export_count + 1, ndyn = 0, symbol_count = 0;
    int rc = -1;
    unsigned char* p;
    Elf64_Dyn dyn[16];
    (void)soname;
    memset(&l, 0, sizeof(l));
    memset(&out, 0, sizeof(out));
    memset(&dynstr, 0, sizeof(dynstr));
    memset(&strtab, 0, sizeof(strtab));
    memset(&symtab, 0, sizeof(symtab));
    memset(&rela, 0, sizeof(rela));
    memset(output, 0, sizeof(*output));
    l.error = error;
    l.error_size = error_size;
    l.nobjs = count;
    l.objs = (SnxElfObject*)calloc(count, sizeof(SnxElfObject));
    export_address = (uint64_t*)calloc(export_count + 1, sizeof(uint64_t));
    if (!l.objs || !export_address) goto done;
    for (i = 0; i < count; ++i)
        if (snx_elf_parse(&l.objs[i], &inputs[i], &l.machine, error, error_size)) goto done;
    l.relative = snx_elf_relative_type(l.machine);
    l.page = l.machine == EM_AARCH64 ? 0x10000 : 0x1000;
    if (snx_elf_collect_globals(l.objs, count, NULL, &l.globals, error, error_size) || scan_relocations(&l)) goto done;
    snx_elf_layout(l.objs, count, l.kind_size, l.kind_align);
    /* dynamic symbol names */
    snx_link_append(&dynstr, "", 1);
    for (i = 0; i < export_count; ++i) snx_link_append(&dynstr, exports[i], strlen(exports[i]) + 1);
    nbucket = export_count ? export_count : 1;
    /* addresses */
    hash_off = align_up(sizeof(Elf64_Ehdr) + sizeof(ph), 8);
    dynsym_off = align_up(hash_off + 4 * (2 + nbucket + nsyms), 8);
    dynstr_off = dynsym_off + nsyms * sizeof(Elf64_Sym);
    rela_off = align_up(dynstr_off + dynstr.size, 8);
    rodata_off = align_up(rela_off + l.relative_count * sizeof(Elf64_Rela), l.kind_align[SNX_ELF_RODATA]);
    end_r = rodata_off + l.kind_size[SNX_ELF_RODATA];
    text_off = align_up(end_r, l.page);
    data_off = align_up(align_up(text_off + l.kind_size[SNX_ELF_TEXT], l.page), l.kind_align[SNX_ELF_DATA]);
    got_off = align_up(data_off + l.kind_size[SNX_ELF_DATA], 8);
    dynamic_off = got_off + 8 * l.got_count;
    end_data = dynamic_off + sizeof(dyn);
    bss_off = align_up(end_data, l.kind_align[SNX_ELF_BSS]);
    l.kind_address[SNX_ELF_RODATA] = rodata_off;
    l.kind_address[SNX_ELF_TEXT] = text_off;
    l.kind_address[SNX_ELF_DATA] = data_off;
    l.kind_address[SNX_ELF_BSS] = bss_off;
    l.got_address = got_off;
    for (i = 0; i < count; ++i)
        for (j = 0; j < l.objs[i].eh->e_shnum; ++j)
            if (l.objs[i].kind[j] >= 0)
                l.objs[i].address[j] = l.kind_address[l.objs[i].kind[j]] + l.objs[i].position[j];
    snx_link_reserve(&out, end_data);
    /* section contents */
    for (i = 0; i < count; ++i) {
        SnxElfObject* o = &l.objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j)
            if (o->kind[j] >= 0 && o->kind[j] != SNX_ELF_BSS && o->sh[j].sh_size)
                memcpy(out.data + o->address[j], o->input->data + o->sh[j].sh_offset, o->sh[j].sh_size);
    }
    /* GOT slots: the address of their symbol, relocated by the loader */
    for (i = 0; i < l.got.capacity; ++i)
        if (l.got.keys[i]) {
            unsigned long object, index;
            int absolute;
            uint64_t value, slot = got_off + 8 * l.got.values[i];
            Elf64_Rela r;
            sscanf(l.got.keys[i], "%lu:%lu", &object, &index);
            value = symbol_address(&l, object, index, &absolute);
            snx_link_put64(out.data + slot, value);
            r.r_offset = slot;
            r.r_info = ELF64_R_INFO((Elf64_Xword)0, l.relative);
            r.r_addend = (int64_t)value;
            snx_link_append(&rela, &r, sizeof(r));
        }
    /* relocations, through elf_object.c as the JIT loader writes them; the module's own parts are the RELATIVE
       entries for absolute addresses and the veneers of AArch64 branches out of reach */
    for (i = 0; i < count; ++i) {
        SnxElfObject* o = &l.objs[i];
        for (j = 0; j < o->eh->e_shnum; ++j) {
            const Elf64_Shdr* s = &o->sh[j];
            const Elf64_Rela* r;
            size_t n, def_object = 0, def_index = 0;
            if (s->sh_type != SHT_RELA || s->sh_info >= o->eh->e_shnum || o->kind[s->sh_info] < 0) continue;
            r = (const Elf64_Rela*)(o->input->data + s->sh_offset);
            n = s->sh_size / sizeof(Elf64_Rela);
            for (k = 0; k < n; ++k) {
                unsigned type = ELF64_R_TYPE(r[k].r_info);
                uint64_t P = o->address[s->sh_info] + r[k].r_offset, S = 0, slot = 0, at;
                int64_t A = r[k].r_addend;
                int weak, absolute = 1, written = SNX_LINK_OUTSIDE;
                char key[72];
                size_t found = 0;
                if (definition(&l, i, ELF64_R_SYM(r[k].r_info), &def_object, &def_index, &weak)) goto done;
                if (!weak) S = symbol_address(&l, def_object, def_index, &absolute);
                if (snx_elf_got_relocation(l.machine, type)) {
                    got_key(key, def_object, def_index);
                    snx_link_find(&l.got, key, &found);
                    slot = got_off + 8 * (uint64_t)found;
                }
                if (r[k].r_offset <= o->sh[s->sh_info].sh_size) {
                    p = out.data + P;
                    written = snx_elf_relocate(l.machine, p, o->sh[s->sh_info].sh_size - r[k].r_offset, type, S, A, P,
                                               got_off, slot);
                    if (written == SNX_LINK_RANGE && snx_elf_call_relocation(l.machine, type) && !weak) {
                        /* the caller's veneer, which it reaches, jumps there */
                        veneer_key(key, i, def_object, def_index);
                        if (snx_link_find(&l.veneer, key, &found)) {
                            at = l.kind_address[SNX_ELF_TEXT] + o->island + SNX_ELF_VENEER_SIZE * (uint64_t)found;
                            written = snx_elf_branch_veneer(p, P, out.data + at, at, S + (uint64_t)A, 0);
                        }
                    }
                }
                if (!written && snx_elf_absolute_relocation(l.machine, type) && !weak && !absolute) {
                    Elf64_Rela d;
                    d.r_offset = P;
                    d.r_info = ELF64_R_INFO((Elf64_Xword)0, l.relative);
                    d.r_addend = (int64_t)(S + (uint64_t)A);
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
    for (i = 0; i < export_count; ++i) {
        size_t found;
        int absolute;
        if (!snx_link_find(&l.globals, exports[i], &found)) {
            snprintf(error, error_size, "exported symbol %s is not defined", exports[i]);
            goto done;
        }
        export_address[i] = symbol_address(&l, found >> 32, found & 0xFFFFFFFFu, &absolute);
    }
    /* .hash: nbucket, nchain, buckets, chains */
    p = out.data + hash_off;
    snx_link_put32(p, (uint32_t)nbucket);
    snx_link_put32(p + 4, (uint32_t)nsyms);
    for (i = 0; i < export_count; ++i) {
        uint32_t* bucket = (uint32_t*)(void*)(p + 8 + 4 * (elf_hash(exports[i]) % nbucket));
        uint32_t* chain = (uint32_t*)(void*)(p + 8 + 4 * nbucket + 4 * (i + 1));
        uint32_t previous;
        memcpy(&previous, bucket, 4);
        memcpy(chain, &previous, 4);
        snx_link_put32((unsigned char*)bucket, (uint32_t)(i + 1));
    }
    /* .dynsym */
    for (i = 0, k = 1; i < export_count; ++i) {
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
    for (i = 0; i < count; ++i) {
        SnxElfObject* o = &l.objs[i];
        for (j = 1; j < o->nsyms; ++j) {
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
    symbol_count = symtab.size / sizeof(Elf64_Sym);
    for (i = 0; i < export_count; ++i) {
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
        SnxLinkBuffer names;
        memset(&names, 0, sizeof(names));
        for (i = 0; i < S_COUNT; ++i)
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
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_ident[EI_OSABI] = ELFOSABI_NONE;
    eh.e_type = ET_DYN;
    eh.e_machine = (Elf64_Half)l.machine;
    eh.e_version = EV_CURRENT;
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
    for (i = 0; l.objs && i < count; ++i) snx_elf_free(&l.objs[i]);
    free(l.objs);
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
