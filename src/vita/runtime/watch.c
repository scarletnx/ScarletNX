/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The pages' states and generations, one each a host page of the guest's space. The fault handler runs on any thread
   (the game's, the Vulkan driver's when it is the CPU), so a page's state changes and its protection under one spin
   lock, which the handler can take: the main thread holds it only around page-protection calls */
#include "vita/runtime/watch.h"
#include "vita/runtime/process.h"
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

enum { UNWATCHED, PROTECTED, WRITTEN };
#define EVENTS 1024u /* the last generations whose pages are known */

typedef struct {
    unsigned char* base;
    uint64_t page, pages;
    _Atomic unsigned char* state;
    _Atomic uint64_t* generation;
    /* the pages generation g wrote, at g % EVENTS: its low 24 bits, the first page and the last (20 bits each, as
       pages are 4 KiB or larger) */
    _Atomic uint64_t* events;
    _Atomic uint64_t now;
    char now_pad[56];
    _Atomic int lock;
    char lock_pad[60];
#ifdef _WIN32
    PVOID handler;
#endif
} Watch;
static Watch g_watch;

static void lock(void) {
    while (atomic_exchange_explicit(&g_watch.lock, 1, memory_order_acquire)) continue;
}
static void unlock(void) { atomic_store_explicit(&g_watch.lock, 0, memory_order_release); }
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
/* the generation that counts a write of the pages [first, last] */
static uint64_t next_generation(uint64_t first, uint64_t last) {
    uint64_t now = atomic_fetch_add_explicit(&g_watch.now, 1, memory_order_acq_rel) + 1;
    atomic_store_explicit(&g_watch.events[now % EVENTS], now << 40 | first << 20 | last, memory_order_release);
    return now;
}

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
    if (g_watch.page < 4096 || VITA_SPACE % g_watch.page) {
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
    if (page_count > (SIZE_MAX - generation_offset) / sizeof *g_watch.generation - EVENTS) {
        vita_watch_free();
        return false;
    }
    size_t total = generation_offset + (page_count + EVENTS) * sizeof *g_watch.generation;
    g_watch.state = calloc(1, total);
    if (!g_watch.state) {
        vita_watch_free();
        return false;
    }
    g_watch.generation = (_Atomic uint64_t*)(g_watch.state + generation_offset);
    g_watch.events = g_watch.generation + page_count;
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
                atomic_store_explicit(&g_watch.state[k], UNWATCHED, memory_order_release);
            i = region_end;
            continue;
        }
#else
        uint64_t region_end = limit;
#endif
        if (atomic_load_explicit(&g_watch.state[i], memory_order_acquire) == PROTECTED) {
            do {
                ++i;
            } while (i < region_end && atomic_load_explicit(&g_watch.state[i], memory_order_acquire) == PROTECTED);
            continue;
        }
        uint64_t end = i + 1;
        while (end < region_end && atomic_load_explicit(&g_watch.state[end], memory_order_acquire) != PROTECTED) ++end;
        unsigned char state = page_protect(i, end - i, false) ? PROTECTED : UNWATCHED;
        for (uint64_t k = i; k < end; ++k) atomic_store_explicit(&g_watch.state[k], state, memory_order_release);
        i = end;
    }
    unlock();
    return atomic_load_explicit(&g_watch.now, memory_order_acquire);
}
bool vita_watch_changed(uint32_t address, uint32_t size, uint64_t since) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return true;
    for (uint64_t i = first; i <= last; ++i)
        if (atomic_load_explicit(&g_watch.state[i], memory_order_acquire) != PROTECTED ||
            atomic_load_explicit(&g_watch.generation[i], memory_order_acquire) > since)
            return true;
    return false;
}
uint64_t vita_watch_page(void) { return g_watch.state ? g_watch.page : 4096; }
/* The protected pages of [first, last] writable, or protected again. Their states stay, but for a page that can't be
   protected again, which is written and watched no more */
static void store_protection(uint64_t first, uint64_t last, bool writable) {
    lock();
    for (uint64_t i = first; i <= last;) {
        uint64_t end = i;
        while (end <= last && atomic_load_explicit(&g_watch.state[end], memory_order_acquire) == PROTECTED) ++end;
        if (end > i && !page_protect(i, end - i, writable) && !writable) {
            uint64_t now = next_generation(i, end - 1);
            for (uint64_t k = i; k < end; ++k) {
                atomic_store_explicit(&g_watch.generation[k], now, memory_order_release);
                atomic_store_explicit(&g_watch.state[k], UNWATCHED, memory_order_release);
            }
        }
        i = end > i ? end : i + 1;
    }
    unlock();
}
bool vita_watch_store(uint32_t address, const void* data, uint32_t row, uint32_t stride, uint32_t rows) {
    const unsigned char* from = data;
    uint64_t first, last, size = rows ? (uint64_t)(rows - 1) * stride + row : 0;
    if (size > UINT32_MAX || !span(address, (uint32_t)size, &first, &last)) return false;
    store_protection(first, last, true);
    /* outside the lock, which the fault handler takes: a page the game gave back faults here, and the run says it */
    for (size_t y = 0; y < rows; ++y) memcpy(g_watch.base + address + y * stride, from + y * stride, row);
    store_protection(first, last, false);
    return true;
}
uint64_t vita_watch_epoch(void) { return atomic_load_explicit(&g_watch.now, memory_order_acquire); }
bool vita_watch_wrote(uint64_t generation, uint64_t* begin, uint64_t* end) {
    if (!g_watch.state) return false;
    uint64_t event = atomic_load_explicit(&g_watch.events[generation % EVENTS], memory_order_acquire);
    if (event >> 40 != (generation & 0xffffffu)) return false;
    *begin = (event >> 20 & 0xfffffu) * g_watch.page, *end = ((event & 0xfffffu) + 1) * g_watch.page;
    return true;
}
void vita_watch_written(uint32_t address, uint32_t size) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return;
    uint64_t now = next_generation(first, last);
    for (uint64_t i = first; i <= last; ++i) atomic_store_explicit(&g_watch.generation[i], now, memory_order_release);
}
void vita_watch_release(uint32_t address, uint32_t size) {
    uint64_t first, last;
    if (!span(address, size, &first, &last)) return;
    lock();
    uint64_t now = next_generation(first, last);
    for (uint64_t i = first; i <= last;) {
        if (atomic_load_explicit(&g_watch.state[i], memory_order_acquire) != PROTECTED) {
            atomic_store_explicit(&g_watch.generation[i], now, memory_order_release);
            atomic_store_explicit(&g_watch.state[i++], UNWATCHED, memory_order_release);
            continue;
        }
        uint64_t end = i;
        while (end <= last && atomic_load_explicit(&g_watch.state[end], memory_order_acquire) == PROTECTED) ++end;
#ifdef _WIN32
        for (uint64_t k = i; k < end;) {
            bool committed;
            uint64_t region_end = committed_region_end(k, end, &committed);
            bool writable = !committed || page_protect(k, region_end - k, true);
            for (uint64_t page = k; page < region_end; ++page) {
                atomic_store_explicit(&g_watch.generation[page], now, memory_order_release);
                if (writable) atomic_store_explicit(&g_watch.state[page], UNWATCHED, memory_order_release);
            }
            k = region_end;
        }
#else
        bool writable = page_protect(i, end - i, true);
        for (uint64_t k = i; k < end; ++k) {
            atomic_store_explicit(&g_watch.generation[k], now, memory_order_release);
            if (writable) atomic_store_explicit(&g_watch.state[k], UNWATCHED, memory_order_release);
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
    unsigned char state = atomic_load_explicit(&g_watch.state[i], memory_order_acquire);
    bool watched = state == PROTECTED || state == WRITTEN;
#ifdef _WIN32
    if (state == UNWATCHED) watched = page_writable(i);
#endif
    /* A second fault may be waiting here after another thread made the page writable. */
    if (state == PROTECTED) {
        atomic_store_explicit(&g_watch.generation[i], next_generation(i, i), memory_order_release);
        if (page_protect(i, 1, true))
            atomic_store_explicit(&g_watch.state[i], WRITTEN, memory_order_release);
        else
            watched = false;
    }
    unlock();
    return watched;
}
