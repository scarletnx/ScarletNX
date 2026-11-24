/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* x64_elf.h: the object built in memory, then written: the null section, the five of bytes, a .rela of each but
   .bss, .symtab (locals first, as ELF wants), .strtab, .shstrtab and .note.GNU-stack */
#include "common/cpu/recompiler/x64/x64_elf.h"
#include <stdlib.h>
#include <string.h>

/* module_x64.c makes one for any machine whose objects are ELF (elf_object.h has the definitions where the system's
   <elf.h> is missing) */
#if !defined(_WIN32)
#if defined(__APPLE__)
#include "common/cpu/recompiler/llvm/elf_defs.h"
#else
#include <elf.h>
#endif
/* elf_object.c's (its header clashes with this one's): r_info of a symbol and a type, MIPS64's n64 layout on EM_MIPS;
   the host's byte order; an object's tables turned to the other */
Elf64_Xword snx_elf_info(unsigned machine, size_t symbol, unsigned type);
unsigned char snx_elf_host_data(void);
int snx_elf_byteswap(unsigned char* object, size_t size, int to_foreign);

typedef struct {
    unsigned char* p;
    size_t n, cap;
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
    if (!n || room(b, n)) return; /* nothing to put: a buffer not made yet has no p to pass memcpy or memset */
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
    return o;
}
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
