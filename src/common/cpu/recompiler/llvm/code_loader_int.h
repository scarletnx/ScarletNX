/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The JIT loader's insides (code_loader.h), shared by code_loader.c (the arena, ELF objects) and code_loader_coff.c
   (COFF objects) */
#ifndef SNX_RECOMPILER_LLVM_CODE_LOADER_INT_H
#define SNX_RECOMPILER_LLVM_CODE_LOADER_INT_H
#include "common/cpu/recompiler/llvm/code_loader.h"
#include <stdint.h>

/* the arena's regions, in address order (code_loader.h) */
enum { SNX_CODE_TEXT, SNX_CODE_RODATA, SNX_CODE_DATA, SNX_CODE_REGIONS };

typedef struct {
    size_t start, size; /* in the arena */
    size_t used;        /* bytes from start that loads hold */
    /* where the loader writes the region's bytes: its read-write view (code, read-only data) or the region itself
       (data) */
    unsigned char* write;
} SnxCodeRegion;

struct SnxCodeLoader {
    unsigned char* base; /* the arena */
    size_t reserved, page;
    unsigned machine; /* the objects' machine: an ELF e_machine, SNX_CODE_COFF_X64 or SNX_CODE_COFF_ARM64 */
    SnxCodeRegion region[SNX_CODE_REGIONS];
    size_t committed;     /* bytes from the data region's start that are read-write */
    SnxLinkNames symbols; /* name -> address: the globals of earlier loads and the defined names */
    /* PowerPC64: the arena's one TOC (its middle, so TOC-relative code reaches all of it), which every load's code
       keeps in r2; the ABI the first load had (an ELFv1 or ELFv2 object); and where a branch enters each function of
       the table, past ELFv2's global entry (which computes r2 from r12, unset by a branch), or at the code an ELFv1
       descriptor names */
    uint64_t toc;
    /* PowerPC64: the TOC is another's, an AOT module's (snx_code_loader_new_toc), whose functions loads call and jump
       to directly */
    int toc_shared;
    unsigned abi;
    SnxLinkNames entries;
    SnxU64 (*resolver)(void* user, const char* name); /* snx_code_set_resolver's */
    void* resolver_user;
};
/* name's address from the table, else from the resolver: 1 and *address, or 0 */
int snx_code_find(const SnxCodeLoader* loader, const char* name, SnxU64* address);

/* where the loader writes the byte the arena has at address: through its region's read-write view */
unsigned char* snx_code_at(const SnxCodeLoader* loader, uint64_t address);
/* the arena address of the next free byte of a region */
uint64_t snx_code_next_free(const SnxCodeLoader* loader, int region);
/* The data region read-write up to end (an arena address), a page at a time. 0, or -1 with the reason in error */
int snx_code_commit(SnxCodeLoader* loader, uint64_t end, char* error, size_t error_size);
/* the code in [start, end) visible to instruction fetch on every core, before any address of it goes out. 0, or -1
   with the reason in error, the code then never to run */
int snx_code_publish(const SnxCodeLoader* loader, uint64_t start, uint64_t end, char* error, size_t error_size);
/* 1 before a load writes the arena, 0 after: on macOS, where the code and read-only data are MAP_JIT memory written
   in place (code_loader.c's apple_arena), whether this thread sees them writable instead of executable, which no code
   loaded may run meanwhile on this thread. Nothing elsewhere, where a load writes through the read-write views */
void snx_code_writable(int writable);
/* code_loader_coff.c's snx_code_load, for a loader of SNX_CODE_COFF_X64 or SNX_CODE_COFF_ARM64 */
int snx_code_load_coff(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                       size_t name_count, SnxU64* addresses, char* error, size_t error_size);

#endif
