/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Android bridge's record of the guest's private anonymous mappings and its MADV_DONTNEED
   (src/android/bridge/host.c): zero pages again, with each part's access, on hosts whose own advice keeps the
   contents (FreeBSD, macOS) or has none (Windows). It runs the same code on every host */
#include "android/bridge/host.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static unsigned char* map(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static bool all(const unsigned char* p, size_t size, unsigned char value) {
    for (size_t i = 0; i < size; ++i)
        if (p[i] != value) return false;
    return true;
}

int main(void) {
    const size_t page = 65536; /* a multiple of every host's page and of Windows' allocation granularity */
    unsigned char* p = map(8 * page);
    assert(p);
    uint64_t a = (uint64_t)(uintptr_t)p;
    host_anon_map(a, 8 * page, HOST_READ | HOST_WRITE);
    memset(p, 0x5a, 8 * page);

    /* the whole range back to zero, still writable */
    assert(host_dontneed(a + page, 2 * page) == 0);
    assert(all(p, page, 0x5a) && all(p + page, 2 * page, 0) && all(p + 3 * page, 5 * page, 0x5a));
    p[page] = 1;

    /* a part read-only in the middle: it stays read-only, its neighbours writable, all zero */
    plat_mem_protect(p + 4 * page, page, HOST_READ);
    host_anon_protect(a + 4 * page, page, HOST_READ);
    assert(host_dontneed(a + 3 * page, 3 * page) == 0);
    assert(all(p + 3 * page, 3 * page, 0));
    p[3 * page] = 2;
    p[5 * page] = 3;

    /* a range forgotten (munmap, a file mapping over it) keeps the host's advice: nothing changes here */
    memset(p + 6 * page, 0x77, page);
    host_anon_unmap(a + 6 * page, page);
    assert(host_dontneed(a + 6 * page, page) == 0);
    assert(all(p + 6 * page, page, 0x77));

    /* a cut inside one part, then the parts on either side still recorded */
    memset(p + 7 * page, 0x33, page);
    host_anon_unmap(a + 7 * page + page / 2, 4096);
    assert(host_dontneed(a + 7 * page, page / 2) == 0);
    assert(all(p + 7 * page, page / 2, 0));
    puts("android_host_anon: ok");
    return 0;
}
