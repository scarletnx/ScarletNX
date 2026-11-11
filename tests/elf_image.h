/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A small ELF64 shared object made in memory, for the tests of what reads one's dynamic section (elf_dynamic.c,
   gpu_driver_stage.c): its header, one segment that loads the whole file, a dynamic section with a soname, needed
   libraries, version needs, and a symbol table with its GNU hash table. In this machine's byte order. The file's
   first byte loads at ELF_IMAGE_VADDR, so no address in it is its offset */
#ifndef SNX_TESTS_ELF_IMAGE_H
#define SNX_TESTS_ELF_IMAGE_H
#include "common/cpu/recompiler/llvm/elf_defs.h"
#include "common/cpu/recompiler/llvm/elf_hash.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define ELF_IMAGE_VADDR   0x40000u
#define ELF_IMAGE_SYMBOLS 12 /* more than a test names: its list ends with a NULL */
#define ELF_IMAGE_BUCKETS 3u
#define ELF_IMAGE_FILTER  2u /* the hash table's filter, in 64-bit words */
#define ELF_IMAGE_SHIFT   6u

typedef struct {
    const char* soname;                     /* NULL for none */
    const char* needed[6];                  /* until NULL */
    const char* versions[4];                /* the libraries of its version needs, until NULL */
    const char* symbols[ELF_IMAGE_SYMBOLS]; /* the functions it defines, until NULL */
    const char* undefined;                  /* a function it only asks for, NULL for none */
    int own_version_names; /* a version need's library has a string of its own; else the needed library's, as linkers
                              write them */
    int full_filter;       /* every bit of the hash table's filter set: a name it has not still reaches a chain */
    uint64_t address_bias; /* added to the dynamic section's addresses, as glibc leaves them in a loaded image */
} ElfImage;

typedef struct {
    size_t size;                      /* the file's */
    size_t symbol[ELF_IMAGE_SYMBOLS]; /* where each function is in it, in e->symbols' order */
    size_t dynamic, strings, strsz;   /* its dynamic section and string table */
    size_t verneed;                   /* its first version need */
} ElfImageMade;

static inline size_t elf_image_string_(unsigned char* strings, size_t* used, const char* text, int share) {
    size_t n = strlen(text) + 1, at;
    for (at = 0; share && at < *used; at += strlen((const char*)strings + at) + 1)
        if (!strcmp((const char*)strings + at, text)) return at;
    at = *used;
    memcpy(strings + at, text, n);
    *used += n;
    return at;
}

/* e as a file's bytes in out, room of them at least 4096 */
static inline ElfImageMade elf_image(const ElfImage* e, unsigned char* out, size_t room) {
    ElfImageMade made = {0};
    unsigned char strings[1024] = {0};
    size_t used = 1, needed[6], versions[4], names[ELF_IMAGE_SYMBOLS], order[ELF_IMAGE_SYMBOLS], soname = 0,
           undefined = 0;
    size_t n_needed = 0, n_versions = 0, n_symbols = 0, first, at, code, symtab, hash, verneed, dynamic, k = 0;
    uint32_t hashes[ELF_IMAGE_SYMBOLS], head[4], buckets[ELF_IMAGE_BUCKETS] = {0}, chain[ELF_IMAGE_SYMBOLS];
    uint64_t filter[ELF_IMAGE_FILTER] = {0};
    Elf64_Ehdr eh = {0};
    Elf64_Phdr ph[2] = {{0}, {0}};
    Elf64_Sym sym = {0};
    Elf64_Dyn dyn[20];
    const uint16_t one = 1;
    assert(room >= 4096);
    memset(out, 0, room);
    if (e->soname) soname = elf_image_string_(strings, &used, e->soname, 1);
    for (; n_needed < 6 && e->needed[n_needed]; ++n_needed)
        needed[n_needed] = elf_image_string_(strings, &used, e->needed[n_needed], 1);
    for (; n_versions < 4 && e->versions[n_versions]; ++n_versions)
        versions[n_versions] = elf_image_string_(strings, &used, e->versions[n_versions], !e->own_version_names);
    if (e->undefined) undefined = elf_image_string_(strings, &used, e->undefined, 1);
    for (; n_symbols < ELF_IMAGE_SYMBOLS && e->symbols[n_symbols]; ++n_symbols) {
        names[n_symbols] = elf_image_string_(strings, &used, e->symbols[n_symbols], 1);
        hashes[n_symbols] = snx_elf_gnu_hash(e->symbols[n_symbols]);
    }
    /* the table holds its symbols a bucket after another */
    for (uint32_t b = 0; b < ELF_IMAGE_BUCKETS; ++b)
        for (size_t i = 0; i < n_symbols; ++i)
            if (hashes[i] % ELF_IMAGE_BUCKETS == b) order[k++] = i;
    first = e->undefined ? 2 : 1; /* after the null symbol and the one it asks for */
    code = sizeof eh + sizeof ph;
    at = code + 16 * ELF_IMAGE_SYMBOLS;
    made.strings = at;
    made.strsz = used;
    at = (at + used + 7) & ~(size_t)7;
    symtab = at;
    at += (first + n_symbols) * sizeof sym;
    hash = at;
    at += sizeof head + sizeof filter + sizeof buckets + n_symbols * 4;
    at = (at + 7) & ~(size_t)7;
    verneed = at;
    at += n_versions * sizeof(Elf64_Verneed);
    dynamic = at;
    memcpy(out + made.strings, strings, used);
    /* the symbols */
    if (e->undefined) {
        sym.st_name = (Elf64_Word)undefined;
        sym.st_info = (unsigned char)(STB_GLOBAL << 4 | STT_FUNC);
        memcpy(out + symtab + sizeof sym, &sym, sizeof sym);
    }
    for (size_t i = 0; i < n_symbols; ++i) {
        size_t s = order[i];
        uint32_t b = hashes[s] % ELF_IMAGE_BUCKETS;
        made.symbol[s] = code + 16 * s;
        sym.st_name = (Elf64_Word)names[s];
        sym.st_info = (unsigned char)(STB_GLOBAL << 4 | STT_FUNC);
        sym.st_shndx = 1;
        sym.st_value = ELF_IMAGE_VADDR + made.symbol[s];
        sym.st_size = 16;
        memcpy(out + symtab + (first + i) * sizeof sym, &sym, sizeof sym);
        if (!buckets[b]) buckets[b] = (uint32_t)(first + i);
        chain[i] = hashes[s] & ~1u;
        if (i + 1 == n_symbols || hashes[order[i + 1]] % ELF_IMAGE_BUCKETS != b) chain[i] |= 1u;
        filter[(hashes[s] / 64) % ELF_IMAGE_FILTER] |= 1ull << (hashes[s] % 64);
        filter[(hashes[s] / 64) % ELF_IMAGE_FILTER] |= 1ull << ((hashes[s] >> ELF_IMAGE_SHIFT) % 64);
    }
    if (e->full_filter) memset(filter, 0xff, sizeof filter);
    head[0] = ELF_IMAGE_BUCKETS;
    head[1] = (uint32_t)first;
    head[2] = ELF_IMAGE_FILTER;
    head[3] = ELF_IMAGE_SHIFT;
    memcpy(out + hash, head, sizeof head);
    memcpy(out + hash + sizeof head, filter, sizeof filter);
    memcpy(out + hash + sizeof head + sizeof filter, buckets, sizeof buckets);
    memcpy(out + hash + sizeof head + sizeof filter + sizeof buckets, chain, n_symbols * 4);
    for (size_t i = 0; i < n_versions; ++i) {
        Elf64_Verneed need = {1, 0, (Elf64_Word)versions[i], 0, i + 1 < n_versions ? (Elf64_Word)sizeof need : 0};
        memcpy(out + verneed + i * sizeof need, &need, sizeof need);
    }
    made.verneed = verneed;
    /* the dynamic section */
    k = 0;
    for (size_t i = 0; i < n_needed; ++i) {
        dyn[k].d_tag = DT_NEEDED;
        dyn[k++].d_un.d_val = needed[i];
    }
    if (e->soname) {
        dyn[k].d_tag = DT_SONAME;
        dyn[k++].d_un.d_val = soname;
    }
    dyn[k].d_tag = DT_STRTAB;
    dyn[k++].d_un.d_ptr = ELF_IMAGE_VADDR + made.strings + e->address_bias;
    dyn[k].d_tag = DT_STRSZ;
    dyn[k++].d_un.d_val = used;
    dyn[k].d_tag = DT_SYMTAB;
    dyn[k++].d_un.d_ptr = ELF_IMAGE_VADDR + symtab + e->address_bias;
    dyn[k].d_tag = DT_GNU_HASH;
    dyn[k++].d_un.d_ptr = ELF_IMAGE_VADDR + hash + e->address_bias;
    if (n_versions) {
        dyn[k].d_tag = DT_VERNEED;
        dyn[k++].d_un.d_ptr = ELF_IMAGE_VADDR + verneed + e->address_bias;
        dyn[k].d_tag = DT_VERNEEDNUM;
        dyn[k++].d_un.d_val = n_versions;
    }
    dyn[k].d_tag = DT_NULL;
    dyn[k++].d_un.d_val = 0;
    memcpy(out + dynamic, dyn, k * sizeof dyn[0]);
    made.dynamic = dynamic;
    made.size = dynamic + k * sizeof dyn[0];
    assert(made.size <= room);
    /* the headers */
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS64;
    eh.e_ident[EI_DATA] = *(const unsigned char*)&one ? ELFDATA2LSB : ELFDATA2MSB;
    eh.e_type = ET_DYN;
    eh.e_phoff = sizeof eh;
    eh.e_ehsize = sizeof eh;
    eh.e_phentsize = sizeof ph[0];
    eh.e_phnum = 2;
    ph[0].p_type = PT_LOAD;
    ph[0].p_vaddr = ELF_IMAGE_VADDR;
    ph[0].p_filesz = ph[0].p_memsz = made.size;
    ph[1].p_type = PT_DYNAMIC;
    ph[1].p_offset = dynamic;
    ph[1].p_vaddr = ELF_IMAGE_VADDR + dynamic;
    ph[1].p_filesz = ph[1].p_memsz = k * sizeof dyn[0];
    memcpy(out, &eh, sizeof eh);
    memcpy(out + sizeof eh, ph, sizeof ph);
    return made;
}

/* how many times text, its 0 too, is in the size bytes at image */
static inline int elf_image_count(const unsigned char* image, size_t size, const char* text) {
    size_t n = strlen(text) + 1;
    int count = 0;
    for (size_t i = 0; i + n <= size; ++i)
        if (!memcmp(image + i, text, n) && (!i || !image[i - 1])) ++count;
    return count;
}
#endif
