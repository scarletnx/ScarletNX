/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pages a test writes code into and then runs: zeroed and read-write, then read-execute, and back */
#ifndef SNX_TEST_EXEC_PAGES_H
#define SNX_TEST_EXEC_PAGES_H
#include <stddef.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

/* size bytes of them, NULL where there is no memory */
static inline unsigned char* snx_test_pages(size_t size) {
#if defined(_WIN32)
    return (unsigned char*)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? NULL : (unsigned char*)p;
#endif
}
/* the pages to run (exec) or to write: 0 done */
static inline int snx_test_pages_exec(unsigned char* p, size_t size, int exec) {
#if defined(_WIN32)
    DWORD old;
    if (!VirtualProtect(p, size, exec ? PAGE_EXECUTE_READ : PAGE_READWRITE, &old)) return -1;
    return exec && !FlushInstructionCache(GetCurrentProcess(), p, size) ? -1 : 0;
#else
    return mprotect(p, size, PROT_READ | (exec ? PROT_EXEC : PROT_WRITE));
#endif
}
static inline void snx_test_pages_free(unsigned char* p, size_t size) {
#if defined(_WIN32)
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, size);
#endif
}

#endif
