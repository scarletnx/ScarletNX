/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Preloaded into recompiler_library_arena_edge (tests/CMakeLists.txt): every reservation of address space (a mapping
   asked for with no address and no access, as plat_mem_reserve makes a code arena) gets an inaccessible page of its
   own just below it. What lies below an arena otherwise is whatever the system mapped after it, a readable view in
   most runs and nothing in some: a read of the bytes before an arena's first function (the function sanitizer's
   check of an indirect call reads the 8 before its callee) then faults in every run, not in those alone */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

typedef void* (*Map)(void*, size_t, int, int, int, off_t);
typedef void* (*Map64)(void*, size_t, int, int, int, off64_t);
static struct {
    Map mmap;
    Map64 mmap64;
} g_arena_guard_below;

static int reserves(const void* address, int protection, int flags) {
    return !address && protection == PROT_NONE && !(flags & MAP_FIXED);
}
/* the reservation in p, which was asked for a page longer: past its first page, which stays as the guard */
static void* past_guard(void* p, size_t page) { return p == MAP_FAILED ? p : (char*)p + page; }

void* mmap(void* address, size_t length, int protection, int flags, int fd, off_t offset) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (!g_arena_guard_below.mmap) {
        void* sym = dlsym(RTLD_NEXT, "mmap");
        memcpy(&g_arena_guard_below.mmap, &sym,
               sizeof sym); /* a function pointer isn't a void*'s object to store through */
    }
    if (!reserves(address, protection, flags))
        return g_arena_guard_below.mmap(address, length, protection, flags, fd, offset);
    return past_guard(g_arena_guard_below.mmap(NULL, length + page, protection, flags, fd, offset), page);
}
/* what a program built with _FILE_OFFSET_BITS=64 calls, as the tests are */
void* mmap64(void* address, size_t length, int protection, int flags, int fd, off64_t offset) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (!g_arena_guard_below.mmap64) {
        void* sym = dlsym(RTLD_NEXT, "mmap64");
        memcpy(&g_arena_guard_below.mmap64, &sym, sizeof sym);
    }
    if (!reserves(address, protection, flags))
        return g_arena_guard_below.mmap64(address, length, protection, flags, fd, offset);
    return past_guard(g_arena_guard_below.mmap64(NULL, length + page, protection, flags, fd, offset), page);
}
