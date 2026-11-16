/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* alloc.c's operator new check: alloc_new_shape leaves a module's operator new to its own code when the module copies
   its operator delete's code where no hook takes it, so that no block of the arena reaches the game's own allocator
   there. Each case is a module of its own, whose delete looks the block's allocator up in a table by its address and
   calls it, as one game's does: copied inline, or in a function no symbol names that _ZdlPvm branches to, it is found;
   its own body, the same code in another delete or in the family's other functions, loads spread wider than a copy's,
   a delete that forwards to another function or reads one global, and code outside every module are not. The verdict
   is read once a module and said once, with the delete's address, its first load, the count and the first copy */
#include "nx/hle/hook/hook.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int alloc_new_shape(u64 addr);

#define MOD    0x8000u /* each case's module: its code, then its globals at GLOBAL */
#define GLOBAL 0x7000u /* the delete's table (base, size, table) and the default allocator at +0x20 */
#define CASES  9

static u8 g_guest[CASES * MOD];
u8* g_mem = g_guest;
MemLayout g_layout;
static SnxModInfo g_info[CASES];
static const SnxModule g_module = {.nmods = CASES, .mods = g_info};
const SnxModule* g_mod = &g_module;
static char g_log[4096];
static int g_said;

/* the modules' exported functions, in address order as the runtime keeps them */
static struct {
    u64 addr, size;
    const char* name;
} g_syms[64];
static u32 g_nsyms;
static void sym(u64 addr, u64 size, const char* name) {
    assert(g_nsyms < sizeof g_syms / sizeof *g_syms && (!g_nsyms || g_syms[g_nsyms - 1].addr < addr));
    g_syms[g_nsyms].addr = addr, g_syms[g_nsyms].size = size, g_syms[g_nsyms].name = name, g_nsyms++;
}
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= g_nsyms) return 0;
    *addr = g_syms[i].addr, *name = g_syms[i].name;
    return 1;
}
u64 kernel_symbol_size(u32 i) { return i < g_nsyms ? g_syms[i].size : 0; }
u64 kernel_find_symbol_exact(const char* name) { return (void)name, 0; }
void hook_run_original(CPU* c, u64 addr) { (void)c, (void)addr; }
void logf_(int level, const char* format, ...) {
    va_list args;
    (void)level;
    va_start(args, format);
    vsnprintf(g_log, sizeof g_log, format, args);
    va_end(args);
    if (strstr(g_log, "copies the code")) g_said++;
}
void fatal(const char* format, ...) { /* alloc.c's table of blocks full, which no case here reaches */
    (void)format;
    abort();
}
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) { return (void)lo, (void)hi, (void)size, (void)align, 0; }
int mem_map(u64 addr, u64 size, u32 state, u32 perm) { return (void)addr, (void)size, (void)state, (void)perm, -1; }
int mem_unmap(u64 addr, u64 size) { return (void)addr, (void)size, 0; }

/* ---- the instructions */
static u64 at;
static void put(u32 w) {
    snx_st_le32(g_guest + at, w);
    at += 4;
}
#define NOP 0xD503201Fu
#define RET 0xD65F03C0u
static void adrp(u32 d, u64 target) {
    i64 pages = (i64)(target >> 12) - (i64)(at >> 12);
    put(0x90000000u | (u32)(pages & 3) << 29 | (u32)(pages >> 2 & 0x7FFFF) << 5 | d);
}
static void add(u32 d, u32 n, u32 imm) { put(0x91000000u | imm << 10 | n << 5 | d); }
static void ldr(u32 t, u32 n, u32 off) { put(0xF9400000u | off / 8 << 10 | n << 5 | t); }
static void ldr_reg(u32 t, u32 n, u32 m) { put(0xF8606800u | m << 16 | n << 5 | t); } /* ldr xt, [xn, xm] */
static void sub(u32 d, u32 n, u32 m) { put(0xCB000000u | m << 16 | n << 5 | d); }
static void mov(u32 d, u32 m) { put(0xAA0003E0u | m << 16 | d); }
static void b(u64 to) { put(0x14000000u | (u32)(((i64)to - (i64)at) >> 2 & 0x3FFFFFF)); }
static void cbz(u32 t, u64 to) { put(0xB4000000u | (u32)(((i64)to - (i64)at) >> 2 & 0x7FFFF) << 5 | t); }
static void cbnz(u32 t, u64 to) { put(0xB5000000u | (u32)(((i64)to - (i64)at) >> 2 & 0x7FFFF) << 5 | t); }

/* the lookup of the block in p's allocator, into x0, with t and u for scratch: the table's address, base and size
   at g, the default allocator at g + 0x20; then the allocator's Free through its table's slot 6 */
static void lookup(u64 g, u32 p, u32 t, u32 u, int spread) {
    adrp(t, g);
    add(t, t, (u32)(g & 0xFFF));
    ldr(u, t, 0x10);
    ldr(t, t, 0);
    for (int i = 0; spread && i < 30; i++) put(NOP);
    sub(t, p, t);
    put(0xD351FC00u | t << 5 | t); /* lsr t, t, #17 */
    put(0x927DAC00u | t << 5 | t); /* and t, t, #0x7ffffffffff8 */
    ldr_reg(t, u, t);
    adrp(u, g + 0x20);
    ldr(u, u, (u32)((g + 0x20) & 0xFFF));
    put(0xF100001Fu | t << 5);           /* cmp t, #0 */
    put(0x9A800000u | t << 16 | u << 5); /* csel x0, u, t, eq */
}
/* operator delete(void*): an early return for a null pointer, the lookup, the tail call. Its size */
static u64 delete_at(u64 entry, u64 g) {
    at = entry;
    cbnz(0, entry + 8);
    put(RET);
    mov(1, 0);
    lookup(g, 0, 8, 9, 0);
    cbz(0, entry + 4);
    ldr(8, 0, 0);
    ldr(2, 8, 0x30);
    put(0xD61F0040u); /* br x2 */
    return at - entry;
}
/* a destructor that ends in the delete inline, on this in x19 */
static void inlined(u64 entry, u64 g, int spread) {
    at = entry;
    put(NOP), put(NOP);
    lookup(g, 19, 10, 11, spread);
    cbz(0, at + 16);
    mov(1, 19);
    ldr(8, 0, 0);
    ldr(2, 8, 0x30);
    put(0xD61F0040u);
    put(RET);
}
/* operator new: the default allocator's Alloc, through one load of the delete's addresses */
static u64 new_at(u64 entry, u64 g) {
    at = entry;
    adrp(8, g + 0x20);
    ldr(0, 8, (u32)((g + 0x20) & 0xFFF));
    ldr(8, 0, 0);
    ldr(8, 8, 0x28);
    put(0xD61F0100u); /* br x8 */
    return at - entry;
}

/* the line alloc_new_shape said last: module m's delete at del, which loads first, copied once, the copy's at copy */
static void said(const char* m, u64 del, u64 first, u64 copy) {
    char want[512];
    snprintf(want, sizeof want,
             "hooks: %s copies the code of its _ZdlPv at %llx, which loads from %llx and 2 more guest addresses, to 1 "
             "place no hook takes, the first at %llx: its operator new keeps its own code",
             m, (unsigned long long)del, (unsigned long long)first, (unsigned long long)copy);
    if (strncmp(g_log, want, strlen(want))) {
        fprintf(stderr, "said:  %swanted: %s\n", g_log, want);
        abort();
    }
}

int main(void) {
    u64 base[CASES], g[CASES], n[CASES], sz;
    static const char* const names[CASES] = {"plain",    "inlined", "clone", "family", "spread",
                                             "forwards", "oneload", "twice", "outside"};
    for (int k = 0; k < CASES; k++) {
        base[k] = (u64)k * MOD, g[k] = base[k] + GLOBAL;
        g_info[k] = (SnxModInfo){.name = names[k], .base = base[k], .size = MOD, .text_off = 0, .text_size = GLOBAL};
    }

    /* 0: the delete and operator new alone */
    sz = delete_at(base[0] + 0x100, g[0]);
    sym(base[0] + 0x100, sz, "_ZdlPv");
    sz = new_at(n[0] = base[0] + 0x200, g[0]);
    sym(n[0], sz, "_Znwm");
    /* 1: and a destructor that inlines it, which no symbol names */
    sz = delete_at(base[1] + 0x100, g[1]);
    sym(base[1] + 0x100, sz, "_ZdlPv");
    sz = new_at(n[1] = base[1] + 0x200, g[1]);
    sym(n[1], sz, "_Znwm");
    inlined(base[1] + 0x1000, g[1], 0);
    /* 2: _ZdlPvm branches to a copy of the delete that no symbol names */
    sz = delete_at(base[2] + 0x100, g[2]);
    sym(base[2] + 0x100, sz, "_ZdlPv");
    sz = new_at(n[2] = base[2] + 0x200, g[2]);
    sym(n[2], sz, "_Znwm");
    at = base[2] + 0x300;
    b(base[2] + 0x2000);
    sym(base[2] + 0x300, 4, "_ZdlPvm");
    delete_at(base[2] + 0x2000, g[2]);
    /* 3: the same code in delete[] and inside realloc, functions the family hooks */
    sz = delete_at(base[3] + 0x100, g[3]);
    sym(base[3] + 0x100, sz, "_ZdlPv");
    sz = new_at(n[3] = base[3] + 0x200, g[3]);
    sym(n[3], sz, "_Znwm");
    sz = delete_at(base[3] + 0x400, g[3]);
    sym(base[3] + 0x400, sz, "_ZdaPv");
    inlined(base[3] + 0x1000, g[3], 0);
    sym(base[3] + 0x1000, 0x200, "realloc");
    /* 4: a function that loads the same addresses, too far apart for a copy */
    sz = delete_at(base[4] + 0x100, g[4]);
    sym(base[4] + 0x100, sz, "_ZdlPv");
    sz = new_at(n[4] = base[4] + 0x200, g[4]);
    sym(n[4], sz, "_Znwm");
    inlined(base[4] + 0x1000, g[4], 1);
    /* 5: a delete that forwards to free, and a destructor that inlines that */
    at = base[5] + 0x100;
    cbz(0, at + 8), b(base[5] + 0x600), put(RET);
    sym(base[5] + 0x100, 12, "_ZdlPv");
    sz = new_at(n[5] = base[5] + 0x200, g[5]);
    sym(n[5], sz, "_Znwm");
    at = base[5] + 0x600;
    put(RET);
    sym(base[5] + 0x600, 4, "free");
    at = base[5] + 0x1000;
    put(NOP), put(0x94000000u | (u32)(((i64)(base[5] + 0x600) - (i64)at) >> 2 & 0x3FFFFFF)), put(RET);
    /* 6: a delete that reads one global, the allocator, and a destructor that inlines it: no verdict */
    at = base[6] + 0x100;
    adrp(8, g[6] + 0x20), ldr(0, 8, (u32)((g[6] + 0x20) & 0xFFF)), ldr(8, 0, 0), ldr(2, 8, 0x30), put(0xD61F0040u);
    sym(base[6] + 0x100, 20, "_ZdlPv");
    sz = new_at(n[6] = base[6] + 0x200, g[6]);
    sym(n[6], sz, "_Znwm");
    at = base[6] + 0x1000;
    adrp(10, g[6] + 0x20), ldr(0, 10, (u32)((g[6] + 0x20) & 0xFFF)), ldr(8, 0, 0), ldr(2, 8, 0x30), put(RET);
    /* 7: as 1, with every operator new asked */
    sz = delete_at(base[7] + 0x100, g[7]);
    sym(base[7] + 0x100, sz, "_ZdlPv");
    sz = new_at(n[7] = base[7] + 0x200, g[7]);
    sym(n[7], sz, "_Znwm");
    sz = new_at(base[7] + 0x280, g[7]);
    sym(base[7] + 0x280, sz, "_Znam");
    inlined(base[7] + 0x1000, g[7], 0);

    assert(alloc_new_shape(n[0]) && !g_said);
    assert(!alloc_new_shape(n[1]) && g_said == 1);
    said(names[1], base[1] + 0x100, g[1] + 0x10, base[1] + 0x1000 + 0x2C); /* the third load of the inlined copy */
    assert(!alloc_new_shape(n[2]) && g_said == 2);
    said(names[2], base[2] + 0x100, g[2] + 0x10, base[2] + 0x2000 + 0x30); /* the clone's, once for both deletes */
    assert(alloc_new_shape(n[3]) && g_said == 2);
    assert(alloc_new_shape(n[4]) && g_said == 2);
    assert(alloc_new_shape(n[5]) && g_said == 2);
    assert(alloc_new_shape(n[6]) && g_said == 2);
    assert(!alloc_new_shape(n[7]) && !alloc_new_shape(base[7] + 0x280) && !alloc_new_shape(n[7]) && g_said == 3);
    assert(alloc_new_shape(sizeof g_guest + 0x100) && g_said == 3); /* in no module */
    puts("PASS operator new's check: a delete copied inline or to a function no symbol names keeps the module's new "
         "its own; its own body, the family's functions, wide loads, a forwarder and one global do not; said once");
    return 0;
}
