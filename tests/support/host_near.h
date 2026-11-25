/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pages the code in an arena reaches, for the tests' hosts (host_win64.c, host_unicorn.c) */
#ifndef SNX_TEST_HOST_NEAR_H
#define SNX_TEST_HOST_NEAR_H
#include <stddef.h>
#include <stdint.h>
#include <sys/mman.h>
/* a system without the flag takes the address as a hint */
#ifdef MAP_FIXED_NOREPLACE
#define SNX_TEST_NOREPLACE MAP_FIXED_NOREPLACE
#else
#define SNX_TEST_NOREPLACE 0
#endif

/* size bytes of pages within 1 GiB of the arena's (arena of them at base), where the code's 32-bit distances reach them:
   the first free ones going down from it, else up from its end. NULL where there are none */
static inline void* hostuc_near(const void* base, size_t arena, size_t size) {
    const uintptr_t lowest = (uintptr_t)base, end = lowest + arena, step = 1u << 20;
    const uintptr_t reach = (uintptr_t)1 << 30;
    const size_t page = 4096;
    size = (size + page - 1) & ~(page - 1);
    for (uintptr_t down = 16 * page + size; down <= reach && down < lowest; down += step) {
        void* p = mmap((void*)(lowest - down), size, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | SNX_TEST_NOREPLACE, -1, 0);
        if (p == (void*)(lowest - down)) return p;
        if (p != MAP_FAILED) munmap(p, size); /* a kernel without the flag takes it as a hint */
    }
    for (uintptr_t up = 16 * page; up + size <= reach; up += step) {
        void* p =
            mmap((void*)(end + up), size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | SNX_TEST_NOREPLACE, -1, 0);
        if (p == (void*)(end + up)) return p;
        if (p != MAP_FAILED) munmap(p, size);
    }
    return NULL;
}

#endif
