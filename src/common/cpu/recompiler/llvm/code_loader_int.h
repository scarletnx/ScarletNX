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
    unsigned machine; /* the objects' machine: an ELF e_machine, or SNX_CODE_COFF_X64 */
    SnxCodeRegion region[SNX_CODE_REGIONS];
    size_t committed;     /* bytes from the data region's start that are read-write */
    SnxLinkNames symbols; /* name -> address: the globals of earlier loads and the defined names */
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
/* the code in [start, end) visible to instruction fetch on every core, before any address of it goes out */
void snx_code_publish(const SnxCodeLoader* loader, uint64_t start, uint64_t end);
/* code_loader_coff.c's snx_code_load, for a loader of SNX_CODE_COFF_X64 */
int snx_code_load_coff(SnxCodeLoader* loader, const SnxLinkInput* inputs, size_t count, const char* const* names,
                       size_t name_count, SnxU64* addresses, char* error, size_t error_size);

#endif
