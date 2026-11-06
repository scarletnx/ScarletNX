/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_heap_used: jemalloc's stats.allocated, read with its mallctl. A file of its own, so a program that links this
   library without jemalloc (the unit tests) takes no reference to its symbol. Android's jemalloc goes whole into the
   executable that loads the driver's library and is not exported from it, so the library never reads it there: its
   plots show the process's footprint instead. */
#include "common/memory/jemalloc.h"
#include <stddef.h>

#if defined(SNX_JEMALLOC) && !defined(__ANDROID__)
/* jemalloc's own prototype, with the prefix its build takes: je_ for the macho and pecoff ABIs alone
   (configure.ac:1250), so not on Linux with any libc, nor on FreeBSD or Android. cmake/jemalloc.cmake passes
   no --with-jemalloc-prefix, so this mirrors the rule rather than the host's. Declared by hand: its header is
   built into the build folder, and this file's one symbol needs no more */
#if defined(__APPLE__)
extern int je_mallctl(const char* name, void* oldp, size_t* oldlenp, void* newp, size_t newlen);
#define snx_mallctl je_mallctl
#else
extern int mallctl(const char* name, void* oldp, size_t* oldlenp, void* newp, size_t newlen);
#define snx_mallctl mallctl
#endif

uint64_t snx_heap_used(void) {
    size_t used = 0, len = sizeof used;
    return snx_mallctl("stats.allocated", &used, &len, NULL, 0) == 0 ? (uint64_t)used : 0;
}
#else
uint64_t snx_heap_used(void) { return 0; }
#endif
