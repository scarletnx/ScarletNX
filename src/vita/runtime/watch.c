/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pages' states and generations, one each a host page of the guest's space. The fault handler runs on any thread
   (the game's, the Vulkan driver's when it is the CPU), so a page's state changes and its protection under one spin
   lock, which the handler can take: the main thread holds it only around page-protection calls */
#include "vita/runtime/watch.h"
#include "vita/runtime/process.h"
#include <stdlib.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

enum { UNWATCHED, PROTECTED, WRITTEN };

typedef struct {
    unsigned char* base;
    uint64_t page, pages;
    unsigned char* state;
    uint64_t* generation;
    uint64_t now;
    char now_pad[56];
    int lock;
    char lock_pad[60];
#ifdef _WIN32
    PVOID handler;
#endif
} Watch;
static Watch g_watch;

static void lock(void) {
    while (__atomic_exchange_n(&g_watch.lock, 1, __ATOMIC_ACQUIRE)) continue;
}
static void unlock(void) { __atomic_store_n(&g_watch.lock, 0, __ATOMIC_RELEASE); }
static bool page_protect(uint64_t first, uint64_t count, bool writable) {
#ifdef _WIN32
    DWORD old;
    DWORD protection = writable ? PAGE_READWRITE : PAGE_READONLY;
    return VirtualProtect(g_watch.base + first * g_watch.page, (SIZE_T)(count * g_watch.page), protection, &old) != 0;
#else
    int protection = writable ? PROT_READ | PROT_WRITE : PROT_READ;
    return !mprotect(g_watch.base + first * g_watch.page, (size_t)(count * g_watch.page), protection);
#endif
}
#ifdef _WIN32
/* The reserved guest space contains uncommitted pages, which VirtualProtect cannot change. */
static uint64_t committed_region_end(uint64_t first, uint64_t limit, bool* committed) {
    MEMORY_BASIC_INFORMATION info;
    uintptr_t address = (uintptr_t)g_watch.base + first * g_watch.page;
    if (!VirtualQuery((const void*)address, &info, sizeof info)) {
        *committed = false;
        return first + 1;
    }
    uintptr_t end = (uintptr_t)info.BaseAddress + info.RegionSize;
    uint64_t next = end > (uintptr_t)g_watch.base ? (end - (uintptr_t)g_watch.base) / g_watch.page : first + 1;
    if (next <= first) next = first + 1;
    if (next > limit) next = limit;
    *committed = info.State == MEM_COMMIT;
    return next;
}
static bool page_writable(uint64_t page) {
    MEMORY_BASIC_INFORMATION info;
    uintptr_t address = (uintptr_t)g_watch.base + page * g_watch.page;
    if (!VirtualQuery((const void*)address, &info, sizeof info) || info.State != MEM_COMMIT ||
        (info.Protect & PAGE_GUARD))
        return false;
    DWORD protection = info.Protect & 0xffu;
    return protection == PAGE_READWRITE || protection == PAGE_EXECUTE_READWRITE;
}
static LONG CALLBACK watch_exception(EXCEPTION_POINTERS* e) {
    const EXCEPTION_RECORD* r = e->ExceptionRecord;
    if (r->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || r->NumberParameters < 2 ||
        r->ExceptionInformation[0] != 1)
        return EXCEPTION_CONTINUE_SEARCH;
    return vita_watch_fault((uintptr_t)r->ExceptionInformation[1]) ? EXCEPTION_CONTINUE_EXECUTION
                                                                   : EXCEPTION_CONTINUE_SEARCH;
}
#endif
/* the pages [first, last] of a guest range */
static bool span(uint32_t address, uint32_t size, uint64_t* first, uint64_t* last) {
    if (!g_watch.state || !size) return false;
    *first = address / g_watch.page, *last = ((uint64_t)address + size - 1) / g_watch.page;
    return *last < g_watch.pages;
}
static uint64_t next_generation(void) { return __atomic_add_fetch(&g_watch.now, 1, __ATOMIC_ACQ_REL); }

bool vita_watch_init(VitaProcess* p) {
#ifdef _WIN32
    SYSTEM_INFO info;
    vita_watch_free();
    if (!p || !p->mem) return false;
    GetSystemInfo(&info);
    g_watch.page = info.dwPageSize;
#else
    vita_watch_free();
    if (!p || !p->mem) return false;
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) return false;
    g_watch.page = (uint64_t)page;
#endif
    if (!g_watch.page || VITA_SPACE % g_watch.page) {
        vita_watch_free();
        return false;
    }
    g_watch.pages = VITA_SPACE / g_watch.page;
    size_t page_count = (size_t)g_watch.pages;
    if ((uint64_t)page_count != g_watch.pages || page_count > SIZE_MAX - (sizeof(uint64_t) - 1)) {
        vita_watch_free();
        return false;
    }
    size_t generation_offset = (page_count + sizeof(uint64_t) - 1) & ~(sizeof(uint64_t) - 1);
    if (page_count > (SIZE_MAX - generation_offset) / sizeof *g_watch.generation) {
        vita_watch_free();
        return false;
    }
    size_t total = generation_offset + page_count * sizeof *g_watch.generation;
    g_watch.state = calloc(1, total);
    if (!g_watch.state) {
        vita_watch_free();
        return false;
    }
    g_watch.generation = (uint64_t*)(g_watch.state + generation_offset);
    g_watch.base = p->mem;
#ifdef _WIN32
    /* run.c installs the crash handler first; this handler is inserted ahead of it. */
    g_watch.handler = AddVectoredExceptionHandler(1, watch_exception);
    if (!g_watch.handler) {
        vita_watch_free();
        return false;
    }
#endif
    return true;
}
void vita_watch_free(void) {
#ifdef _WIN32
    if (g_watch.handler) RemoveVectoredExceptionHandler(g_watch.handler);
#endif
    free(g_watch.state);
    g_watch = (Watch){0};
}
uint64_t vita_watch_protect(uint32_t address, uint32_t size) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return 0;
    lock();
    uint64_t limit = last + 1;
    for (uint64_t i = first; i < limit;) {
#ifdef _WIN32
        bool committed;
        uint64_t region_end = committed_region_end(i, limit, &committed);
        if (!committed) {
            for (uint64_t k = i; k < region_end; ++k)
                __atomic_store_n(&g_watch.state[k], UNWATCHED, __ATOMIC_RELEASE);
            i = region_end;
            continue;
        }
#else
        uint64_t region_end = limit;
#endif
        if (__atomic_load_n(&g_watch.state[i], __ATOMIC_ACQUIRE) == PROTECTED) {
            do {
                ++i;
            } while (i < region_end && __atomic_load_n(&g_watch.state[i], __ATOMIC_ACQUIRE) == PROTECTED);
            continue;
        }
        uint64_t end = i + 1;
        while (end < region_end && __atomic_load_n(&g_watch.state[end], __ATOMIC_ACQUIRE) != PROTECTED) ++end;
        unsigned char state = page_protect(i, end - i, false) ? PROTECTED : UNWATCHED;
        for (uint64_t k = i; k < end; ++k) __atomic_store_n(&g_watch.state[k], state, __ATOMIC_RELEASE);
        i = end;
    }
    unlock();
    return __atomic_load_n(&g_watch.now, __ATOMIC_ACQUIRE);
}
bool vita_watch_changed(uint32_t address, uint32_t size, uint64_t since) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return true;
    for (uint64_t i = first; i <= last; ++i)
        if (__atomic_load_n(&g_watch.state[i], __ATOMIC_ACQUIRE) != PROTECTED ||
            __atomic_load_n(&g_watch.generation[i], __ATOMIC_ACQUIRE) > since)
            return true;
    return false;
}
uint64_t vita_watch_epoch(void) { return __atomic_load_n(&g_watch.now, __ATOMIC_ACQUIRE); }
void vita_watch_written(uint32_t address, uint32_t size) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return;
    uint64_t now = next_generation();
    for (uint64_t i = first; i <= last; ++i) __atomic_store_n(&g_watch.generation[i], now, __ATOMIC_RELEASE);
}
void vita_watch_release(uint32_t address, uint32_t size) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return;
    lock();
    uint64_t now = next_generation();
    for (uint64_t i = first; i <= last;) {
        if (__atomic_load_n(&g_watch.state[i], __ATOMIC_ACQUIRE) != PROTECTED) {
            __atomic_store_n(&g_watch.generation[i], now, __ATOMIC_RELEASE);
            __atomic_store_n(&g_watch.state[i++], UNWATCHED, __ATOMIC_RELEASE);
            continue;
        }
        uint64_t end = i;
        while (end <= last && __atomic_load_n(&g_watch.state[end], __ATOMIC_ACQUIRE) == PROTECTED) ++end;
#ifdef _WIN32
        for (uint64_t k = i; k < end;) {
            bool committed;
            uint64_t region_end = committed_region_end(k, end, &committed);
            bool writable = !committed || page_protect(k, region_end - k, true);
            for (uint64_t page = k; page < region_end; ++page) {
                __atomic_store_n(&g_watch.generation[page], now, __ATOMIC_RELEASE);
                if (writable) __atomic_store_n(&g_watch.state[page], UNWATCHED, __ATOMIC_RELEASE);
            }
            k = region_end;
        }
#else
        bool writable = page_protect(i, end - i, true);
        for (uint64_t k = i; k < end; ++k) {
            __atomic_store_n(&g_watch.generation[k], now, __ATOMIC_RELEASE);
            if (writable) __atomic_store_n(&g_watch.state[k], UNWATCHED, __ATOMIC_RELEASE);
        }
#endif
        i = end;
    }
    unlock();
}
bool vita_watch_fault(uintptr_t host) {
    if (!g_watch.state || host < (uintptr_t)g_watch.base || host >= (uintptr_t)g_watch.base + VITA_SPACE) return false;
    uint64_t i = (host - (uintptr_t)g_watch.base) / g_watch.page;
    lock();
    unsigned char state = __atomic_load_n(&g_watch.state[i], __ATOMIC_ACQUIRE);
    bool watched = state == PROTECTED || state == WRITTEN;
#ifdef _WIN32
    if (state == UNWATCHED) watched = page_writable(i);
#endif
    /* A second fault may be waiting here after another thread made the page writable. */
    if (state == PROTECTED) {
        __atomic_store_n(&g_watch.generation[i], next_generation(), __ATOMIC_RELEASE);
        if (page_protect(i, 1, true))
            __atomic_store_n(&g_watch.state[i], WRITTEN, __ATOMIC_RELEASE);
        else
            watched = false;
    }
    unlock();
    return watched;
}
