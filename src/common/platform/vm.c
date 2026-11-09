/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's address space (vm.h) */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE /* MAP_FIXED_NOREPLACE */
#endif
#include "common/platform/vm.h"
#include <errno.h>
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0 /* FreeBSD never reserves swap for a mapping */
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#if defined(_WIN32)
size_t plat_mem_page(void) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
}
/* the reason in errno, as mmap's */
static void* reserved(void* p) {
    if (!p) {
        DWORD why = GetLastError();
        errno = why == ERROR_NOT_ENOUGH_MEMORY || why == ERROR_COMMITMENT_LIMIT ? ENOMEM
                : why == ERROR_INVALID_ADDRESS                                  ? EEXIST
                                                                                : EINVAL;
    }
    return p;
}
void* plat_mem_reserve(uint64_t size, uint64_t align) {
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    if (align <= info.dwAllocationGranularity)
        return reserved(VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS));
    /* a larger range, given back for its aligned part: another thread may take that part in between */
    for (int tries = 0; tries < 16; tries++) {
        unsigned char* p = reserved(VirtualAlloc(NULL, (SIZE_T)(size + align), MEM_RESERVE, PAGE_NOACCESS));
        if (!p) return NULL;
        uintptr_t at = ((uintptr_t)p + (uintptr_t)(align - 1)) & ~(uintptr_t)(align - 1);
        VirtualFree(p, 0, MEM_RELEASE);
        void* q = VirtualAlloc((void*)at, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS);
        if (q) return q;
    }
    errno = ENOMEM;
    return NULL;
}
void* plat_mem_reserve_at(uint64_t address, uint64_t size) {
    return reserved(VirtualAlloc((void*)(uintptr_t)address, (SIZE_T)size, MEM_RESERVE, PAGE_NOACCESS));
}
int plat_mem_commit(void* p, uint64_t size) {
    VirtualFree(p, (SIZE_T)size, MEM_DECOMMIT); /* committed pages would keep their contents */
    return VirtualAlloc(p, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE) == p ? 0 : -1;
}
void* plat_mem_alloc(uint64_t size) {
    /* Windows charges the commit at once, and gives the pages at their first touch */
    return reserved(VirtualAlloc(NULL, (SIZE_T)size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
}
int plat_mem_decommit(void* p, uint64_t size) { return VirtualFree(p, (SIZE_T)size, MEM_DECOMMIT) ? 0 : -1; }
int plat_mem_protect(void* p, uint64_t size, unsigned access) {
    DWORD old, mode = !access                   ? PAGE_NOACCESS
                      : access & PLAT_MEM_EXEC  ? (access & PLAT_MEM_WRITE ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ)
                      : access & PLAT_MEM_WRITE ? PAGE_READWRITE
                                                : PAGE_READONLY;
    if (access && !VirtualAlloc(p, (SIZE_T)size, MEM_COMMIT, PAGE_READWRITE)) return -1;
    return VirtualProtect(p, (SIZE_T)size, mode, &old) ? 0 : -1;
}
void plat_mem_release(void* p, uint64_t size) {
    (void)size;
    VirtualFree(p, 0, MEM_RELEASE);
}
#else
size_t plat_mem_page(void) {
    long size = sysconf(_SC_PAGESIZE);
    return size > 0 ? (size_t)size : 4096;
}
void* plat_mem_reserve(uint64_t size, uint64_t align) {
    uint64_t host = plat_mem_page(), extra = align > host ? align - host : 0;
    unsigned char* p =
        mmap(NULL, (size_t)(size + extra), PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (!extra) return p;
    uint64_t head = (align - (uint64_t)(uintptr_t)p % align) % align;
    if (head) munmap(p, (size_t)head);
    if (extra > head) munmap(p + head + size, (size_t)(extra - head));
    return p + head;
}
void* plat_mem_reserve_at(uint64_t address, uint64_t size) {
    void* want = (void*)(uintptr_t)address;
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
#if defined(MAP_FIXED_NOREPLACE)
    flags |= MAP_FIXED_NOREPLACE;
#elif defined(MAP_EXCL)
    flags |= MAP_FIXED | MAP_EXCL; /* FreeBSD: fails where the range is mapped */
#endif
    void* p = mmap(want, (size_t)size, PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED) return NULL;
    if (p != want) { /* the address as a hint (macOS, and a Linux before 4.17) */
        munmap(p, (size_t)size);
        errno = EEXIST;
        return NULL;
    }
    return p;
}
int plat_mem_commit(void* p, uint64_t size) {
    int flags = MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    return mmap(p, (size_t)size, PROT_READ | PROT_WRITE, flags, -1, 0) == p ? 0 : -1;
}
void* plat_mem_alloc(uint64_t size) {
    void* p = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
/* fresh pages over the range: only Linux's MADV_DONTNEED drops the contents (Darwin's may keep them, FreeBSD's
   does) */
int plat_mem_decommit(void* p, uint64_t size) {
    int flags = MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE;
    return mmap(p, (size_t)size, PROT_NONE, flags, -1, 0) == p ? 0 : -1;
}
int plat_mem_protect(void* p, uint64_t size, unsigned access) {
    int prot = (access & PLAT_MEM_READ ? PROT_READ : 0) | (access & PLAT_MEM_WRITE ? PROT_WRITE : 0) |
               (access & PLAT_MEM_EXEC ? PROT_EXEC : 0);
    return mprotect(p, (size_t)size, prot) ? -1 : 0;
}
void plat_mem_release(void* p, uint64_t size) { munmap(p, (size_t)size); }
#endif
