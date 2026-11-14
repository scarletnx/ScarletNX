/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* alloc.c's arena in a program that links jemalloc as scarletnx does (whole, unexported): the family is ready, says
   it found jemalloc's functions as the program's own, and malloc, calloc, realloc, operator new aligned and free run
   in the window above the layout, in guest memory the test reserves; a pointer the arena didn't make goes to the
   function's own code */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0 /* FreeBSD's mmap reserves nothing anyway */
#endif
/* where alloc.c finds jemalloc: an ELF program's own functions, the je_ ones a Mach-O program exports by dlsym */
#if defined(__ELF__)
#define FOUND "(the program's own)"
#else
#define FOUND "(found by dlsym)"
#endif
#define GIB      (1ull << 30)
#define WINDOW   (1 * GIB) /* the layout's end, above which the arena's window starts */
#define AS_TOTAL (8 * GIB)

u8* g_mem;
MemLayout g_layout;
const SnxModule* g_mod;
static u64 g_next = WINDOW; /* mem_find_free's next free address */
static int g_original;
static char g_log[512];

u64 kernel_find_symbol_exact(const char* name) { return (void)name, 0; }
void hook_run_original(CPU* c, u64 addr) {
    (void)c, (void)addr;
    g_original++;
}
void logf_(int level, const char* format, ...) {
    va_list args;
    (void)level;
    va_start(args, format);
    vsnprintf(g_log, sizeof g_log, format, args);
    va_end(args);
}
/* the guest's pages: a reservation the test made, writable once mapped */
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) {
    u64 a = (g_next < lo ? lo : g_next + align - 1) & ~(align - 1);
    if (a + size > hi) return 0;
    g_next = a + size;
    return a;
}
int mem_map(u64 addr, u64 size, u32 state, u32 perm) {
    (void)state, (void)perm;
    return mprotect(g_mem + addr, (size_t)size, PROT_READ | PROT_WRITE) ? MEM_NOMEM : 0;
}
int mem_unmap(u64 addr, u64 size) { return mprotect(g_mem + addr, (size_t)size, PROT_NONE) ? -1 : 0; }

int main(void) {
    CPU c;
    void* r = mmap(NULL, AS_TOTAL, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(r != MAP_FAILED);
    g_mem = r;
    g_layout.kern_end = WINDOW;
    g_layout.end = AS_TOTAL;
    memset(&c, 0, sizeof c);
    c.mem = g_mem;

    assert(alloc_ready() && strstr(g_log, FOUND) && strstr(g_log, "from 40000000 to 200000000"));
    u64 p = alloc_malloc(&c, 100);
    assert(p >= WINDOW && p < AS_TOTAL && !(p & 15));
    memset(g_mem + p, 0xAB, 100);
    u64 z = alloc_calloc(&c, 50, 4);
    for (int i = 0; i < 200; i++) assert(g_mem[z + i] == 0);
    u64 q = alloc_realloc(&c, p, 100000);
    assert(q >= WINDOW && g_mem[q] == 0xAB && g_mem[q + 99] == 0xAB);
    u64 a = alloc_new_aligned(&c, 64, 4096);
    assert(a >= WINDOW && !(a & 4095));
    alloc_free(&c, q), alloc_free(&c, z), alloc_delete(&c, a);
    assert(!g_original);
    alloc_free(&c, 0x1000); /* the game's own allocator's: its own code */
    assert(g_original == 1);
    alloc_free(&c, 0);
    assert(g_original == 1);
    puts("PASS the alloc family's arena: jemalloc found in the program, allocations in the window, a foreign pointer's "
         "own code");
    return 0;
}
