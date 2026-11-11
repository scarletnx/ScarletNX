/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* elf_dynamic.h, from the System V ABI's dynamic linking chapter (generic ABI 5) and the GNU hash table and symbol
   version sections as the Linux Standard Base describes them. Every structure is copied out of the bytes before it is
   read: a file's offsets need not be aligned */
#include "common/utilities/elf_dynamic.h"
#include "common/cpu/recompiler/llvm/elf_defs.h"
#include "common/cpu/recompiler/llvm/elf_hash.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* this machine's byte order, as e_ident[EI_DATA] names it */
static int host_data(void) {
    const uint16_t one = 1;
    unsigned char first;
    memcpy(&first, &one, 1);
    return first ? ELFDATA2LSB : ELFDATA2MSB;
}

/* a file's program header i, its header checked */
static Elf64_Phdr file_phdr(const unsigned char* image, const Elf64_Ehdr* eh, unsigned i) {
    Elf64_Phdr ph;
    memcpy(&ph, image + eh->e_phoff + (size_t)i * sizeof ph, sizeof ph);
    return ph;
}

/* where the file keeps the byte its segments load at vaddr: 1, else 0 for an address no segment has in the file */
static int file_offset(const unsigned char* image, size_t size, const Elf64_Ehdr* eh, uint64_t vaddr,
                       uint64_t* offset) {
    for (unsigned i = 0; i < eh->e_phnum; ++i) {
        Elf64_Phdr ph = file_phdr(image, eh, i);
        if (ph.p_type != PT_LOAD || vaddr < ph.p_vaddr || vaddr - ph.p_vaddr >= ph.p_filesz) continue;
        if (ph.p_offset >= size || vaddr - ph.p_vaddr >= size - ph.p_offset) return 0;
        *offset = ph.p_offset + (vaddr - ph.p_vaddr);
        return 1;
    }
    return 0;
}

/* the name at at of a string table of strsz bytes, written over when it is one of names' from: 1, 0 for another name,
   -1 when the table has no name there */
static int rename_at(unsigned char* strings, uint64_t strsz, uint64_t at, const SnxElfRename* names, size_t count) {
    char* name = (char*)strings + at;
    if (at >= strsz || !memchr(name, 0, (size_t)(strsz - at))) return -1;
    for (size_t i = 0; i < count; ++i)
        if (!strcmp(name, names[i].from)) {
            memcpy(name, names[i].to, strlen(names[i].to));
            return 1;
        }
    return 0;
}

int snx_elf_rename(unsigned char* image, size_t size, const SnxElfRename* names, size_t count, char* why,
                   size_t why_size) {
    Elf64_Ehdr eh;
    Elf64_Dyn d;
    uint64_t dynamic = 0, entries = 0, strtab_at = 0, strsz = 0, verneed_at = 0, verneeds = 0, strtab = 0, at = 0;
    int has_dynamic = 0, wrote = 0, r;
    for (size_t i = 0; i < count; ++i) {
        if (!names[i].from[0] || strlen(names[i].from) != strlen(names[i].to)) {
            snprintf(why, why_size, "cannot have \"%s\" renamed \"%s\": a name of %zu letters for one of %zu",
                     names[i].from, names[i].to, strlen(names[i].to), strlen(names[i].from));
            return -1;
        }
        /* a string two entries share is met twice: the second time it must be no name to rename */
        for (size_t k = 0; k < count; ++k)
            if (!strcmp(names[i].to, names[k].from)) {
                snprintf(why, why_size, "cannot have \"%s\" renamed \"%s\", a name that is renamed \"%s\" itself",
                         names[i].from, names[i].to, names[k].to);
                return -1;
            }
    }
    if (size < sizeof eh || memcmp(image, ELFMAG, SELFMAG)) {
        snprintf(why, why_size, "is not an ELF file: %zu bytes, the first four %02x %02x %02x %02x", size,
                 size > 0 ? image[0] : 0u, size > 1 ? image[1] : 0u, size > 2 ? image[2] : 0u,
                 size > 3 ? image[3] : 0u);
        return -1;
    }
    memcpy(&eh, image, sizeof eh);
    if (eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_ident[EI_DATA] != host_data()) {
        snprintf(why, why_size,
                 "is not a 64-bit ELF file in this machine's byte order: its class is %d and its data %d",
                 eh.e_ident[EI_CLASS], eh.e_ident[EI_DATA]);
        return -1;
    }
    if (eh.e_phentsize != sizeof(Elf64_Phdr) || eh.e_phoff > size ||
        (size - eh.e_phoff) / sizeof(Elf64_Phdr) < eh.e_phnum) {
        snprintf(why, why_size, "has %u program headers of %u bytes at %llu, outside its %zu bytes", eh.e_phnum,
                 eh.e_phentsize, (unsigned long long)eh.e_phoff, size);
        return -1;
    }
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        Elf64_Phdr ph = file_phdr(image, &eh, i);
        if (ph.p_type != PT_DYNAMIC || ph.p_offset > size || size - ph.p_offset < ph.p_filesz) continue;
        dynamic = ph.p_offset;
        entries = ph.p_filesz / sizeof d;
        has_dynamic = 1;
    }
    if (!has_dynamic) {
        snprintf(why, why_size, "has no dynamic section in its %zu bytes", size);
        return -1;
    }
    for (uint64_t i = 0; i < entries; ++i) {
        memcpy(&d, image + dynamic + i * sizeof d, sizeof d);
        if (d.d_tag == DT_NULL) break;
        if (d.d_tag == DT_STRTAB) strtab_at = d.d_un.d_ptr;
        if (d.d_tag == DT_STRSZ) strsz = d.d_un.d_val;
        if (d.d_tag == DT_VERNEED) verneed_at = d.d_un.d_ptr;
        if (d.d_tag == DT_VERNEEDNUM) verneeds = d.d_un.d_val;
    }
    if (!strsz || !file_offset(image, size, &eh, strtab_at, &strtab) || size - strtab < strsz) {
        snprintf(why, why_size, "has no dynamic string table in its %zu bytes: DT_STRTAB is 0x%llx and DT_STRSZ %llu",
                 size, (unsigned long long)strtab_at, (unsigned long long)strsz);
        return -1;
    }
    for (uint64_t i = 0; i < entries; ++i) {
        memcpy(&d, image + dynamic + i * sizeof d, sizeof d);
        if (d.d_tag == DT_NULL) break;
        if (d.d_tag != DT_NEEDED && d.d_tag != DT_SONAME) continue;
        r = rename_at(image + strtab, strsz, d.d_un.d_val, names, count);
        if (r < 0) {
            snprintf(why, why_size, "has a library name at %llu of its dynamic string table of %llu bytes",
                     (unsigned long long)d.d_un.d_val, (unsigned long long)strsz);
            return -1;
        }
        wrote += r;
    }
    /* the linker matches a version need's file against the name the needed library loaded by */
    if (verneeds && !file_offset(image, size, &eh, verneed_at, &at)) {
        snprintf(why, why_size, "has its %llu version needs at 0x%llx, outside its %zu bytes",
                 (unsigned long long)verneeds, (unsigned long long)verneed_at, size);
        return -1;
    }
    for (uint64_t i = 0; i < verneeds; ++i) {
        Elf64_Verneed need;
        if (at > size || size - at < sizeof need) {
            snprintf(why, why_size, "has version need %llu of %llu at %llu, outside its %zu bytes",
                     (unsigned long long)i, (unsigned long long)verneeds, (unsigned long long)at, size);
            return -1;
        }
        memcpy(&need, image + at, sizeof need);
        r = rename_at(image + strtab, strsz, need.vn_file, names, count);
        if (r < 0) {
            snprintf(why, why_size,
                     "has version need %llu name its library at %u of its dynamic string table of %llu bytes",
                     (unsigned long long)i, need.vn_file, (unsigned long long)strsz);
            return -1;
        }
        wrote += r;
        if (!need.vn_next) break;
        at += need.vn_next;
    }
    return wrote;
}

/* a loaded image: where its virtual address 0 is in this process, and the virtual addresses its segments take */
typedef struct {
    uint64_t bias, low, high;
} Loaded;

/* n bytes the image has at vaddr, into out: 1, else 0 when it has none there */
static int peek(const Loaded* l, uint64_t vaddr, void* out, size_t n) {
    if (vaddr < l->low || vaddr >= l->high || l->high - vaddr < n) return 0;
    memcpy(out, (const void*)(uintptr_t)(l->bias + vaddr), n);
    return 1;
}

/* a dynamic entry's address as the file had it: glibc writes the loaded address over its own, bionic and musl leave
   the file's */
static uint64_t file_vaddr(const Loaded* l, uint64_t v) {
    return v >= l->bias && v - l->bias >= l->low && v - l->bias < l->high ? v - l->bias : v;
}

void* snx_elf_loaded_symbol(const void* base, const char* name) {
    const unsigned char* image = (const unsigned char*)base;
    Loaded l = {0, UINT64_MAX, 0};
    Elf64_Ehdr eh;
    Elf64_Phdr ph;
    Elf64_Dyn d;
    Elf64_Sym sym;
    uint64_t header = 0, dynamic = 0, dynamic_size = 0, strings = 0, strsz = 0, symbols = 0, table = 0, word, chain;
    uint32_t head[4], hash = snx_elf_gnu_hash(name), index, other;
    char got[SNX_ELF_NAME_MAX];
    size_t length = strlen(name);
    int has_header = 0, has_dynamic = 0;
    if (!base || length >= sizeof got) return NULL;
    memcpy(&eh, image, sizeof eh);
    if (memcmp(eh.e_ident, ELFMAG, SELFMAG) || eh.e_ident[EI_CLASS] != ELFCLASS64 ||
        eh.e_ident[EI_DATA] != host_data() || eh.e_phentsize != sizeof ph || eh.e_phnum > 128)
        return NULL;
    for (unsigned i = 0; i < eh.e_phnum; ++i) {
        memcpy(&ph, image + eh.e_phoff + (size_t)i * sizeof ph, sizeof ph);
        if (ph.p_type == PT_DYNAMIC) {
            dynamic = ph.p_vaddr;
            dynamic_size = ph.p_memsz;
            has_dynamic = 1;
        }
        if (ph.p_type != PT_LOAD) continue;
        if (ph.p_vaddr < l.low) l.low = ph.p_vaddr;
        if (ph.p_vaddr + ph.p_memsz > l.high) l.high = ph.p_vaddr + ph.p_memsz;
        if (!ph.p_offset) { /* the segment of the file's first byte, the header base is */
            header = ph.p_vaddr;
            has_header = 1;
        }
    }
    if (!has_header || !has_dynamic || eh.e_phoff + (uint64_t)eh.e_phnum * sizeof ph > l.high - header) return NULL;
    l.bias = (uint64_t)(uintptr_t)base - header;
    for (uint64_t at = 0; at + sizeof d <= dynamic_size; at += sizeof d) {
        if (!peek(&l, dynamic + at, &d, sizeof d) || d.d_tag == DT_NULL) break;
        if (d.d_tag == DT_STRTAB) strings = file_vaddr(&l, d.d_un.d_ptr);
        if (d.d_tag == DT_STRSZ) strsz = d.d_un.d_val;
        if (d.d_tag == DT_SYMTAB) symbols = file_vaddr(&l, d.d_un.d_ptr);
        if (d.d_tag == DT_GNU_HASH) table = file_vaddr(&l, d.d_un.d_ptr);
    }
    /* the table: its buckets' count, the first symbol it holds, its filter's count of 64-bit words and shift; then the
       filter, the buckets, and a hash for each symbol from the first, its low bit set on the last of a bucket */
    if (!strsz || !strings || !symbols || !table) return NULL;
    if (!peek(&l, table, head, sizeof head) || !head[0] || !head[2]) return NULL;
    if (!peek(&l, table + sizeof head + (uint64_t)((hash / 64) % head[2]) * 8, &word, sizeof word)) return NULL;
    if (!(word >> (hash % 64) & 1) || !(word >> ((hash >> (head[3] % 32)) % 64) & 1)) return NULL;
    chain = table + sizeof head + (uint64_t)head[2] * 8 + (uint64_t)head[0] * 4;
    if (!peek(&l, chain - (uint64_t)head[0] * 4 + (uint64_t)(hash % head[0]) * 4, &index, sizeof index)) return NULL;
    if (index < head[1]) return NULL;
    for (;; ++index) {
        if (!peek(&l, chain + (uint64_t)(index - head[1]) * 4, &other, sizeof other)) return NULL;
        if ((other | 1) == (hash | 1) && peek(&l, symbols + (uint64_t)index * sizeof sym, &sym, sizeof sym) &&
            sym.st_shndx != SHN_UNDEF && sym.st_name < strsz && strsz - sym.st_name > length &&
            peek(&l, strings + sym.st_name, got, length + 1) && !memcmp(got, name, length + 1)) {
            unsigned type = ELF64_ST_TYPE(sym.st_info), bind = ELF64_ST_BIND(sym.st_info);
            if ((type == STT_FUNC || type == STT_OBJECT || type == STT_NOTYPE) &&
                (bind == STB_GLOBAL || bind == STB_WEAK) && sym.st_value >= l.low && sym.st_value < l.high)
                return (void*)(uintptr_t)(l.bias + sym.st_value);
            return NULL;
        }
        if (other & 1) return NULL;
    }
}
