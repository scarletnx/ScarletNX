/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* alloc.c's arena in a program that links jemalloc as scarletnx does (whole, unexported): the family is ready, says
   it found jemalloc's functions as the program's own, and malloc, calloc, realloc, operator new aligned and free run
   in the window above the layout, in guest memory the test reserves, a block of a page or more at a page. A pointer
   that is no block the arena handed out goes to the function's own code: one outside the window, a piece of a region
   the game carved from an arena block, a block given back already; and the table of blocks stays right as many come
   and go (built with LIVE_BITS 8). Then the SDK heap's functions, which a game whose malloc is its StandardAllocator's
   hands a block malloc gave: an arena block is the arena's, with the heap's results, any other pointer the heap's own
   code, and the route list names each of them as every SDK here spells it */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/loader/hook_routes.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include <assert.h>
#include <stdarg.h>
#include <stdlib.h>
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
    (void)addr;
    g_original++;
    c->x[0] = 777; /* what the function's own code returned */
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
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
    /* a page or more starts at a page, as the SDK's allocator gives it: the kernel's transfer memory takes no other
       (jemalloc alone offsets a large block within its first page) */
    u64 big[4];
    for (int i = 0; i < 4; i++) {
        big[i] = alloc_malloc(&c, 0x800000);
        assert(big[i] >= WINDOW && !(big[i] & 0xFFF));
    }
    q = alloc_realloc(&c, q, 300000);
    assert(!(q & 0xFFF) && g_mem[q] == 0xAB && !(alloc_new(&c, 4096) & 0xFFF));
    for (int i = 0; i < 4; i++) alloc_free(&c, big[i]);
    alloc_free(&c, q), alloc_free(&c, z), alloc_delete(&c, a);
    assert(!g_original);
    alloc_free(&c, 0x1000); /* the game's own allocator's: its own code */
    assert(g_original == 1);
    alloc_free(&c, 0);
    assert(g_original == 1);
    /* a piece of a region the game's own allocator carved out of an arena block: in the window, but no block the arena
       handed out, so the function's own code takes it, and the block stays whole */
    u64 region = alloc_malloc(&c, 0x10000);
    memset(g_mem + region, 0x5A, 0x10000);
    alloc_delete(&c, region + 0x100);
    assert(g_original == 2 && alloc_realloc(&c, region + 0x200, 64) == 777 && g_original == 3);
    assert(g_mem[region] == 0x5A && g_mem[region + 0xFFFF] == 0x5A);
    /* the block itself goes back once; then it is no block of the arena's any more */
    alloc_free(&c, region);
    assert(g_original == 3);
    alloc_free(&c, region);
    assert(g_original == 4);
    /* blocks coming and going many times over the table's size (LIVE_BITS is 8 here, 256 slots): its tombstones are
       cleared, so a pointer it doesn't hold is still found absent */
    for (int round = 0; round < 40; round++) {
        u64 held[150];
        for (int i = 0; i < 150; i++) held[i] = alloc_malloc(&c, 16 + (u64)((round * 7 + i) % 64) * 16);
        for (int i = 0; i < 150; i++) alloc_free(&c, held[i]);
    }
    assert(g_original == 4);
    alloc_free(&c, region + 0x300);
    assert(g_original == 5);
    /* the SDK allocator's size of a block: the arena's own from jemalloc, at least what was asked, and the block stays
       the arena's; any other pointer, a region's piece among them, the SDK's own answer */
    const int was = g_original;
    u64 sized = alloc_malloc(&c, 100);
    assert(alloc_std_size(&c, 0, sized) >= 100 && g_original == was);
    assert(alloc_std_size(&c, 0, 0x1000) == 777 && g_original == was + 1);
    assert(alloc_std_size(&c, 0, sized + 16) == 777 && g_original == was + 2);
    alloc_free(&c, sized);
    assert(g_original == was + 2);
    /* the SDK heap's functions, through which a game whose malloc is its StandardAllocator's frees, sizes, resizes
       and shrinks a block malloc gave (CentralHeap's, and on a thread with a cache the TlsHeapCache's): an arena block
       is the arena's, with the heap's result; any other pointer, a region's piece among them, the heap's own code */
    u64 h = alloc_malloc(&c, 64);
    memset(g_mem + h, 0x3C, 64);
    assert(alloc_heap_free(&c, 0, h) == 0 && g_original == was + 2);
    assert(alloc_heap_free(&c, 0, h) == 777 && g_original == was + 3); /* gone already: the heap's own code */
    h = alloc_malloc(&c, 64);
    assert(alloc_heap_free_size(&c, 0, h, 64) == 0 && g_original == was + 3);
    assert(alloc_heap_free_size(&c, 0, h, 64) == 777 && g_original == was + 4);
    /* realloc: the block at *out, its bytes kept, the old pointer no block any more; EINVAL for a size of 0 and ENOMEM
       for one with its top bit set, *out untouched; ENOMEM with *out 0 and the block kept when there is no room */
    h = alloc_malloc(&c, 64);
    memset(g_mem + h, 0x3C, 64);
    const u64 out = alloc_malloc(&c, 8);
    assert(alloc_heap_realloc(&c, 0, h, 4096, out) == 0 && g_original == was + 4);
    u64 h2 = snx_ld_le64(g_mem + out);
    assert(h2 >= WINDOW && !(h2 & 0xFFF) && g_mem[h2] == 0x3C && g_mem[h2 + 63] == 0x3C);
    snx_st_le64(g_mem + out, 0x5555);
    assert(alloc_heap_realloc(&c, 0, h2, 0, out) == 22 && alloc_heap_realloc(&c, 0, h2, 1ull << 63, out) == 12);
    assert(snx_ld_le64(g_mem + out) == 0x5555 && g_original == was + 4);
    assert(alloc_heap_realloc(&c, 0, h2, 1ull << 40, out) == 12 && !snx_ld_le64(g_mem + out));
    assert(g_mem[h2 + 63] == 0x3C && alloc_std_size(&c, 0, h2) >= 4096 && g_original == was + 4);
    assert(h2 != h && alloc_heap_realloc(&c, 0, h, 32, out) == 777 && g_original == was + 5);
    assert(alloc_heap_realloc(&c, 0, 0x2000, 32, out) == 777 && alloc_heap_realloc(&c, 0, h2 + 16, 32, out) == 777);
    const int then = g_original;
    /* shrink: the block kept whole, 0 for a size that fits and EINVAL for one that doesn't, ENOMEM for the top bit */
    assert(alloc_heap_shrink(&c, 0, h2, 100) == 0 && alloc_heap_shrink(&c, 0, h2, 1ull << 20) == 22);
    assert(alloc_heap_shrink(&c, 0, h2, 1ull << 63) == 12 && alloc_std_size(&c, 0, h2) >= 4096 && g_original == then);
    assert(alloc_heap_shrink(&c, 0, 0x2000, 16) == 777 && g_original == then + 1);
    assert(alloc_heap_free(&c, 0, h2) == 0 && alloc_heap_free(&c, 0, 0x2000) == 777 && g_original == then + 2);
    alloc_free(&c, out);
    /* each of them hooked, in every spelling of the SDKs here (0.12 to 20): its calls, a direct one or the cache's
       through its table, go to its slot. Not the heap's allocations, whose blocks are its own */
    static const char* const heap[] = {
        "_ZNK2nn3mem17StandardAllocator9GetSizeOfEPKv",
        "_ZN2nn7nlibsdk4heap11CentralHeap4FreeEPv",
        "_ZN2nn7nlibsdk4heap11CentralHeap12GetAllocSizeEPKv",
        "_ZN2nn7nlibsdk4heap11CentralHeap7ReallocEPvmPS3_",
        "_ZN2nn7nlibsdk4heap11CentralHeap12FreeWithSizeEPvm",
        "_ZN2nn7nlibsdk4heap11CentralHeap6ShrinkEPvm",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache13FreeFuncMode0INSt3__117integral_constantIbLb0EEEEEiPS2_Pv",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache13FreeFuncMode0INSt3__117integral_constantIbLb1EEEEEiPS2_Pv",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache17FreeFuncSizeMode0INSt3__117integral_constantIbLb0EEEEEiPS2_Pvm",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache17FreeFuncSizeMode0INSt3__117integral_constantIbLb1EEEEEiPS2_Pvm",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache21GetAllocSizeFuncMode0EPS2_PKv",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache16ReallocFuncMode0INSt3__117integral_constantIbLb0EEEEEiPS2_PvmPS8_",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache16ReallocFuncMode0INSt3__117integral_constantIbLb1EEEEEiPS2_PvmPS8_",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache15ShrinkFuncMode0INSt3__117integral_constantIbLb0EEEEEiPS2_Pvm",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache15ShrinkFuncMode0INSt3__117integral_constantIbLb1EEEEEiPS2_Pvm",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache15ShrinkFuncMode0EPS2_Pvm",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache14FreeFunc_Mode0INSt3__117integral_constantIbLb0EEEEEiPS2_Pv",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache14FreeFunc_Mode0INSt3__117integral_constantIbLb1EEEEEiPS2_Pv",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache18FreeFuncSize_Mode0INSt3__117integral_constantIbLb0EEEEEiPS2_Pvm",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache18FreeFuncSize_Mode0INSt3__117integral_constantIbLb1EEEEEiPS2_Pvm",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache22GetAllocSizeFunc_Mode0EPS2_PKv",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache17ReallocFunc_Mode0INSt3__117integral_constantIbLb0EEEEEiPS2_PvmPS8_",
        "_ZN2nn7nlibsdk4heap12TlsHeapCache17ReallocFunc_Mode0INSt3__117integral_constantIbLb1EEEEEiPS2_PvmPS8_",
    };
    for (size_t i = 0; i < sizeof heap / sizeof *heap; i++) assert(snx_hook_routed(heap[i]));
    assert(!snx_hook_routed("_ZN2nn7nlibsdk4heap11CentralHeap5AllocEmm"));
    assert(!snx_hook_routed(
        "_ZN2nn7nlibsdk4heap12TlsHeapCache14AllocFuncMode0INSt3__117integral_constantIbLb1EEEEEPvPS2_m"));
    assert(!snx_hook_routed(
        "_ZN2nn7nlibsdk4heap12TlsHeapCache13FreeFuncMode0INSt3__117integral_constantIbLb1EEEEEiPS2_Pvm"));
    puts("PASS the alloc family's arena: jemalloc found in the program, allocations in the window, a foreign pointer's "
         "own code");
    return 0;
}
