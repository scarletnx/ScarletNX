/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_GNU_MEMORY_H
#define SNX_GNU_MEMORY_H
#include <stdint.h>
#include <stddef.h>
#define GNU_PAGE_SIZE     4096u
#define GNU_PROT_READ     1u
#define GNU_PROT_WRITE    2u
#define GNU_PROT_EXEC     4u
#define GNU_PAGE_MAPPED   8u
#define GNU_PAGE_CODE     16u /* writable, and compiled from: the host's copy is read-only until a write (watch_code) */
#define GNU_ADDRESS_LIMIT 0x100000000ull
typedef struct GnuMemory {
    unsigned char* base;
    unsigned char* pages;
    uint64_t size, mmap_next, brk_base, brk_current, brk_limit;
    /* the host's page, which base is aligned to and protected in: GNU_PAGE_SIZE, or larger (16 KiB on Apple silicon
       and some Android kernels, 64 KiB on PowerPC64), when each host page holds several guest pages (host_protect) */
    uint64_t host_page;
    /* called before executable pages are remapped, unmapped or have their permissions changed: the JIT's host code
       for them must not run again (gnu_jit_invalidate). Set after gnu_memory_init */
    void (*code_changed)(void* context, uint64_t address, uint64_t length);
    void* code_context;
} GnuMemory;
/* --host-page=N: the page gnu_memory_init's memories work in, a power of two from the host's page to 1 MiB that main
   checked (0: the host's), as a host with those pages does (tests). Before any memory is made; POSIX only */
void gnu_memory_host_page(uint64_t page);
int gnu_memory_init(GnuMemory* memory);
void gnu_memory_destroy(GnuMemory* memory);
void* gnu_memory_access(GnuMemory* memory, uint64_t address, size_t length, unsigned permissions);
int gnu_memory_read(GnuMemory* memory, uint64_t address, void* destination, size_t length);
int gnu_memory_write(GnuMemory* memory, uint64_t address, const void* source, size_t length);
int gnu_memory_read_string(GnuMemory* memory, uint64_t address, char* destination, size_t capacity);
int gnu_memory_map(GnuMemory* memory, uint64_t address, uint64_t length, unsigned permissions, int replace);
int gnu_memory_unmap(GnuMemory* memory, uint64_t address, uint64_t length);
int gnu_memory_protect(GnuMemory* memory, uint64_t address, uint64_t length, unsigned permissions);
int64_t gnu_memory_mmap(GnuMemory* memory, uint64_t address, uint64_t length, unsigned permissions, uint64_t flags);
uint64_t gnu_memory_brk(GnuMemory* memory, uint64_t requested);
size_t gnu_memory_maps(GnuMemory* memory, char* destination, size_t capacity);
/* The JIT compiled code from these writable pages: a write to one of them is seen (gnu_memory_code_written) */
void gnu_memory_watch_code(GnuMemory* memory, uint64_t address, uint64_t length);
/* a host write fault at guest address: 1 when it was a watched page, which is writable again and whose code went
   (code_changed), so the write can be retried */
int gnu_memory_code_written(GnuMemory* memory, uint64_t address);
#endif
