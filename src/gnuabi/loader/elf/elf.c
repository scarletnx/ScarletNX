/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Linux ELF64/AArch64 layout, independently decoded rather than host Elf headers. */
#include "gnuabi/loader/elf/elf.h"
#include "gnuabi/abi/errno/errno.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <inttypes.h>
static uint64_t down(uint64_t n) { return n & ~4095ull; }
static uint64_t up(uint64_t n) { return (n + 4095) & ~4095ull; }
/* One program header, normalized across ELF32 and ELF64 layouts. */
typedef struct {
    uint32_t pt, flags;
    uint64_t offset, va, filesz, memsz, align;
} Phdr;
static void rd_phdr(const unsigned char* p, unsigned bits, Phdr* h) {
    if (bits == 32) {
        h->pt = snx_ld_le32(p);
        h->offset = snx_ld_le32(p + 4);
        h->va = snx_ld_le32(p + 8);
        h->filesz = snx_ld_le32(p + 16);
        h->memsz = snx_ld_le32(p + 20);
        h->flags = snx_ld_le32(p + 24);
        h->align = snx_ld_le32(p + 28);
    } else {
        h->pt = snx_ld_le32(p);
        h->flags = snx_ld_le32(p + 4);
        h->offset = snx_ld_le64(p + 8);
        h->va = snx_ld_le64(p + 16);
        h->filesz = snx_ld_le64(p + 32);
        h->memsz = snx_ld_le64(p + 40);
        h->align = snx_ld_le64(p + 48);
    }
}
static int fail(char* error, size_t cap, const char* format, ...) {
    va_list args;
    va_start(args, format);
    if (cap) vsnprintf(error, cap, format, args);
    va_end(args);
    return -GNU_EINVAL;
}
void gnu_elf_destroy(GnuElf* elf) {
    free(elf->image);
    free(elf->code);
    memset(elf, 0, sizeof *elf);
}
int gnu_elf_load_image(GnuElf* elf, GnuMemory* m, const void* source, size_t size, char* error, size_t cap) {
    return gnu_elf_load_at(elf, m, source, size, GNU_ELF_PROGRAM_BIAS, 1, error, cap);
}
int gnu_elf_load_at(GnuElf* elf, GnuMemory* m, const void* source, size_t size, uint64_t dyn_bias, int program,
                    char* error, size_t cap) {
    const unsigned char* b = source;
    memset(elf, 0, sizeof *elf);
    if (size < 52 || memcmp(b, "\177ELF", 4) || b[5] != 1 || b[6] != 1)
        return fail(error, cap, "Expected a little-endian ELF executable");
    unsigned bits = b[4] == 1 ? 32 : b[4] == 2 ? 64 : 0;
    if (!bits) return fail(error, cap, "ELF class %u is neither 1 (32-bit) nor 2 (64-bit)", b[4]);
    elf->bits = bits;
    uint16_t type = snx_ld_le16(b + 16), machine = snx_ld_le16(b + 18);
    uint16_t phsize = snx_ld_le16(b + (bits == 32 ? 42 : 54)), phnum = snx_ld_le16(b + (bits == 32 ? 44 : 56));
    uint64_t phoff = bits == 32 ? snx_ld_le32(b + 28) : snx_ld_le64(b + 32);
    uint64_t entry = bits == 32 ? snx_ld_le32(b + 24) : snx_ld_le64(b + 24);
    unsigned want_machine = bits == 32 ? 40 : 183; /* EM_ARM : EM_AARCH64 */
    unsigned want_phsize = bits == 32 ? 32 : 56, want_ehsize = bits == 32 ? 52 : 64;
    uint16_t ehsize = snx_ld_le16(b + (bits == 32 ? 40 : 52));
    if (type != 2 && type != 3) return fail(error, cap, "ELF type %u is neither 2 (EXEC) nor 3 (DYN)", type);
    if (machine != want_machine) return fail(error, cap, "ELF machine %u, not %u", machine, want_machine);
    if (snx_ld_le32(b + 20) != 1) return fail(error, cap, "ELF version %u, not 1", snx_ld_le32(b + 20));
    if (ehsize != want_ehsize || phsize != want_phsize)
        return fail(error, cap, "ELF header and program header sizes %u and %u, not %u and %u", ehsize, phsize,
                    want_ehsize, want_phsize);
    if (!phnum || phnum > 1024) return fail(error, cap, "%u program headers, not 1 to 1024", phnum);
    if (phoff > size || (uint64_t)phnum * phsize > size - phoff)
        return fail(error, cap, "%u program headers at offset %" PRIu64 " run past the file's %zu bytes", phnum, phoff,
                    size);
    elf->thumb = bits == 32 && (entry & 1);
    elf->bias = type == 3 ? dyn_bias : 0;
    elf->stack_permissions = GNU_PROT_READ | GNU_PROT_WRITE;
    size_t executable_segments = 0, load_segments = 0;
    uint64_t phdr = 0, end = 0;
    Phdr h;
    for (unsigned i = 0; i < phnum; i++) {
        rd_phdr(b + phoff + i * phsize, bits, &h);
        if (h.pt == 3) {
            /* the dynamic linker, which the process loads beside the program (gnu_process_load) */
            if (!program) return fail(error, cap, "The dynamic linker itself names a dynamic linker");
            if (h.offset > size || !h.filesz || h.filesz > size - h.offset || h.filesz > sizeof elf->interp ||
                b[h.offset + h.filesz - 1] != 0)
                return fail(error, cap, "PT_INTERP %u is not a NUL-terminated path within the file", i);
            memcpy(elf->interp, b + h.offset, (size_t)h.filesz);
            continue;
        }
        if (h.pt == 0x6474e551)
            elf->stack_permissions = (h.flags & 4 ? 1u : 0u) | (h.flags & 2 ? 2u : 0u) | (h.flags & 1 ? 4u : 0u);
        if (h.pt != 1) continue;
        if (!h.memsz) {
            if (h.filesz) return fail(error, cap, "PT_LOAD %u has file bytes without a memory interval", i);
            continue;
        }
        if (h.filesz > h.memsz || h.offset > size || h.filesz > size - h.offset || h.va > m->size ||
            elf->bias > m->size - h.va || h.memsz > m->size - h.va - elf->bias ||
            (h.align > 1 && ((h.align & (h.align - 1)) || (h.va & (h.align - 1)) != (h.offset & (h.align - 1)))))
            return fail(error, cap, "PT_LOAD %u is invalid or outside the bounded guest address space", i);
        uint64_t va = h.va + elf->bias;
        if (va < GNU_PAGE_SIZE || up(va + h.memsz) > 0x40000000ull)
            return fail(error, cap, "PT_LOAD %u overlaps reserved guest regions", i);
        if ((va & 4095) != (h.offset & 4095)) return fail(error, cap, "PT_LOAD %u has inconsistent page offsets", i);
        if (va + h.memsz > end) end = va + h.memsz;
        if (phoff >= h.offset && phoff - h.offset <= h.filesz &&
            (uint64_t)phnum * phsize <= h.filesz - (phoff - h.offset))
            phdr = va + phoff - h.offset;
        if (h.flags & 1) executable_segments++;
        load_segments++;
    }
    if (!load_segments || !executable_segments) return fail(error, cap, "ELF has no executable PT_LOAD");
    if (entry > m->size - elf->bias)
        return fail(error, cap, "ELF entry 0x%" PRIx64 " is past the guest address space", entry);
    for (unsigned i = 0; i < phnum; i++) {
        rd_phdr(b + phoff + i * phsize, bits, &h);
        if (h.pt != 1 || !h.memsz) continue;
        uint64_t begin = h.va + elf->bias, finish = begin + h.memsz;
        for (unsigned j = 0; j < i; j++) {
            Phdr g;
            rd_phdr(b + phoff + j * phsize, bits, &g);
            if (g.pt == 1 && g.memsz) {
                uint64_t lo = g.va + elf->bias;
                if (begin < lo + g.memsz && lo < finish)
                    return fail(error, cap, "PT_LOAD %u overlaps PT_LOAD %u in memory", i, j);
            }
        }
    }
    elf->image = malloc(size);
    elf->code = calloc(executable_segments, sizeof *elf->code);
    if (!elf->image || !elf->code) {
        gnu_elf_destroy(elf);
        return -GNU_ENOMEM;
    }
    memcpy(elf->image, b, size);
    elf->size = size;
    for (unsigned i = 0; i < phnum; i++) {
        rd_phdr(b + phoff + i * phsize, bits, &h);
        if (h.pt != 1 || !h.memsz) continue;
        uint64_t va = h.va + elf->bias;
        for (uint64_t page = down(va); page < up(va + h.memsz); page += GNU_PAGE_SIZE) {
            if (!(m->pages[page / GNU_PAGE_SIZE] & GNU_PAGE_MAPPED) && gnu_memory_map(m, page, GNU_PAGE_SIZE, 3, 0)) {
                gnu_elf_destroy(elf);
                return fail(error, cap, "Cannot map the ELF's page at 0x%" PRIx64, page);
            }
        }
        memcpy(m->base + va, b + h.offset, (size_t)h.filesz);
        memset(m->base + va + h.filesz, 0, (size_t)(h.memsz - h.filesz));
        if (h.flags & 1) elf->code[elf->code_count++] = (GnuElfRange){va, m->base + va, (size_t)h.memsz};
    }
    unsigned char* permissions = calloc((size_t)(up(end) / GNU_PAGE_SIZE), 1);
    if (!permissions) {
        gnu_elf_destroy(elf);
        return -GNU_ENOMEM;
    }
    for (unsigned i = 0; i < phnum; i++) {
        rd_phdr(b + phoff + i * phsize, bits, &h);
        if (h.pt != 1 || !h.memsz) continue;
        unsigned prot = (h.flags & 4 ? 1u : 0u) | (h.flags & 2 ? 2u : 0u) | (h.flags & 1 ? 4u : 0u);
        uint64_t va = h.va + elf->bias;
        for (uint64_t page = down(va); page < up(va + h.memsz); page += GNU_PAGE_SIZE)
            permissions[page / GNU_PAGE_SIZE] |= (unsigned char)(GNU_PAGE_MAPPED | prot);
    }
    for (uint64_t i = 0; i < up(end) / GNU_PAGE_SIZE;) {
        if (!permissions[i]) {
            i++;
            continue;
        }
        uint64_t begin = i;
        unsigned p = permissions[i];
        do { i++; } while (i < up(end) / GNU_PAGE_SIZE && permissions[i] == p);
        if (gnu_memory_protect(m, begin * GNU_PAGE_SIZE, (i - begin) * GNU_PAGE_SIZE, p & 7u)) {
            free(permissions);
            gnu_elf_destroy(elf);
            return fail(error, cap, "Cannot protect the ELF's pages 0x%" PRIx64 " to 0x%" PRIx64, begin * GNU_PAGE_SIZE,
                        i * GNU_PAGE_SIZE);
        }
    }
    free(permissions);
    elf->entry = (entry & (bits == 32 ? ~1u : ~0ull)) + elf->bias;
    unsigned entry_mask = bits == 64 ? 3u : (elf->thumb ? 1u : 3u);
    if (!gnu_memory_access(m, elf->entry, 4, GNU_PROT_EXEC) || (elf->entry & entry_mask)) {
        gnu_elf_destroy(elf);
        return fail(error, cap, "ELF entry is not an aligned executable address");
    }
    if (!phdr && program) {
        phdr = GNU_PAGE_SIZE;
        uint64_t bytes = (uint64_t)phnum * phsize;
        if (gnu_memory_map(m, phdr, bytes, 3, 0) || gnu_memory_write(m, phdr, b + phoff, (size_t)bytes) ||
            gnu_memory_protect(m, phdr, up(bytes), 1)) {
            gnu_elf_destroy(elf);
            return fail(error, cap, "Cannot place the ELF's program headers at 0x%" PRIx64, phdr);
        }
    }
    elf->phdr = phdr;
    elf->phnum = phnum;
    elf->phentsize = phsize;
    elf->image_end = up(end);
    if (program) {
        m->brk_base = m->brk_current = up(end);
        m->brk_limit = up(end) + 0x04000000ull;
        if (m->brk_limit > 0x40000000ull) m->brk_limit = 0x40000000ull;
        /* below the dynamic linker */
        if (m->brk_base < GNU_ELF_INTERP_BIAS && m->brk_limit > GNU_ELF_INTERP_BIAS) m->brk_limit = GNU_ELF_INTERP_BIAS;
    }
    if (cap) error[0] = 0;
    return 0;
}
int gnu_elf_load(GnuElf* elf, GnuMemory* memory, const char* path, char* error, size_t cap) {
    FILE* file = fopen(path, "rb");
    if (!file) return fail(error, cap, "Cannot open ELF '%s': %s", path, strerror(errno));
    if (fseek(file, 0, SEEK_END) || ftell(file) < 0) {
        int why = errno;
        fclose(file);
        return fail(error, cap, "Cannot size ELF '%s': %s", path, strerror(why));
    }
    long size = ftell(file);
    if (size < 64 || (uint64_t)size > 0x10000000ull) {
        fclose(file);
        return fail(error, cap, "ELF '%s' is %ld bytes, not 64 to 256 MiB", path, size);
    }
    unsigned char* bytes = malloc((size_t)size);
    if (!bytes) {
        fclose(file);
        return -GNU_ENOMEM;
    }
    rewind(file);
    size_t n = fread(bytes, 1, (size_t)size, file);
    fclose(file);
    if (n != (size_t)size) {
        free(bytes);
        return fail(error, cap, "Cannot read ELF '%s': %zu of its %ld bytes read", path, n, size);
    }
    int result = gnu_elf_load_image(elf, memory, bytes, (size_t)size, error, cap);
    free(bytes);
    return result;
}
