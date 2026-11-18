/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* alloc.c's operator new check: alloc_new_shape leaves a module's operator new to its own code when the module's
   operator delete looks the block's allocator up in a table by the block's address, as one game's does, and the module
   copies that code where no hook takes it, so that no block of the arena reaches the game's own allocator there. Each
   case is a module of its own: such a delete copied inline, or in a function no symbol names that _ZdlPvm branches
   to (on a mov's copy of the block), is found; its own body, the same code in another delete or in the family's other
   functions, loads spread wider than a copy's, a delete that forwards to another function, and code outside every
   module are not, nor is a copied delete without the lookup: one that asks the thread's heap, one that indexes its
   table by the size, one on a table at a link-time address that loads one global. The verdict is read once a module
   and said once, naming the delete that does the lookup, its first load, the count and the first copy */
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
#define CASES  11

static struct {
    u8 guest[CASES * MOD];
    SnxModInfo info[CASES];
    char log[4096]; /* the line alloc.c said last */
    int said;       /* the lines that said a verdict */
    struct {
        u64 addr, size;
        const char* name;
    } syms[64]; /* the modules' exported functions, in address order as the runtime keeps them */
    u32 nsyms;
    u64 at; /* where put writes the next instruction */
} g_test;
u8* g_mem = g_test.guest;
MemLayout g_layout;
static const SnxModule g_module = {.nmods = CASES, .mods = g_test.info};
const SnxModule* g_mod = &g_module;
static void sym(u64 addr, u64 size, const char* name) {
    u32 i = g_test.nsyms++;
    assert(i < sizeof g_test.syms / sizeof *g_test.syms && (!i || g_test.syms[i - 1].addr < addr));
    g_test.syms[i].addr = addr, g_test.syms[i].size = size, g_test.syms[i].name = name;
}
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= g_test.nsyms) return 0;
    *addr = g_test.syms[i].addr, *name = g_test.syms[i].name;
    return 1;
}
u64 kernel_symbol_size(u32 i) { return i < g_test.nsyms ? g_test.syms[i].size : 0; }
u64 kernel_find_symbol_exact(const char* name) { return (void)name, 0; }
void hook_run_original(CPU* c, u64 addr) { (void)c, (void)addr; }
void logf_(int level, const char* format, ...) {
    va_list args;
    (void)level;
    va_start(args, format);
    vsnprintf(g_test.log, sizeof g_test.log, format, args);
    va_end(args);
    if (strstr(g_test.log, "copies the code")) g_test.said++;
}
void fatal(const char* format, ...) { /* alloc.c's table of blocks full, which no case here reaches */
    (void)format;
    abort();
}
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) { return (void)lo, (void)hi, (void)size, (void)align, 0; }
int mem_map(u64 addr, u64 size, u32 state, u32 perm) { return (void)addr, (void)size, (void)state, (void)perm, -1; }
int mem_unmap(u64 addr, u64 size) { return (void)addr, (void)size, 0; }

/* ---- the instructions */
static void put(u32 w) {
    snx_st_le32(g_test.guest + g_test.at, w);
    g_test.at += 4;
}
#define NOP 0xD503201Fu
#define RET 0xD65F03C0u
static void adrp(u32 d, u64 target) {
    i64 pages = (i64)(target >> 12) - (i64)(g_test.at >> 12);
    put(0x90000000u | (u32)(pages & 3) << 29 | (u32)(pages >> 2 & 0x7FFFF) << 5 | d);
}
static void add(u32 d, u32 n, u32 imm) { put(0x91000000u | imm << 10 | n << 5 | d); }
static void ldr(u32 t, u32 n, u32 off) { put(0xF9400000u | off / 8 << 10 | n << 5 | t); }
static void ldr_reg(u32 t, u32 n, u32 m) { put(0xF8606800u | m << 16 | n << 5 | t); } /* ldr xt, [xn, xm] */
static void sub(u32 d, u32 n, u32 m) { put(0xCB000000u | m << 16 | n << 5 | d); }
static void mov(u32 d, u32 m) { put(0xAA0003E0u | m << 16 | d); }
static void b(u64 to) { put(0x14000000u | (u32)(((i64)to - (i64)g_test.at) >> 2 & 0x3FFFFFF)); }
static void bl(u64 to) { put(0x94000000u | (u32)(((i64)to - (i64)g_test.at) >> 2 & 0x3FFFFFF)); }
static void cbz(u32 t, u64 to) { put(0xB4000000u | (u32)(((i64)to - (i64)g_test.at) >> 2 & 0x7FFFF) << 5 | t); }
static void cbnz(u32 t, u64 to) { put(0xB5000000u | (u32)(((i64)to - (i64)g_test.at) >> 2 & 0x7FFFF) << 5 | t); }

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
/* operator delete(void*): an early return for a null pointer, the block kept in x19, the lookup on x<p> (0 or 19 for
   the block, 1 for a sized delete's size), the tail call. Its size */
static u64 delete_at(u64 entry, u64 g, u32 p) {
    g_test.at = entry;
    cbnz(0, entry + 8);
    put(RET);
    mov(19, 0);
    lookup(g, p, 8, 9, 0);
    cbz(0, entry + 4);
    ldr(8, 0, 0);
    ldr(2, 8, 0x30);
    put(0xD61F0040u); /* br x2 */
    return g_test.at - entry;
}
/* a destructor that ends in the delete inline, on this in x19 */
static void inlined(u64 entry, u64 g, int spread) {
    g_test.at = entry;
    put(NOP), put(NOP);
    lookup(g, 19, 10, 11, spread);
    cbz(0, g_test.at + 16);
    mov(1, 19);
    ldr(8, 0, 0);
    ldr(2, 8, 0x30);
    put(0xD61F0040u);
    put(RET);
}
/* the thread's heap of a block in p, asked as other games' deletes do: the heaps are ready (g), the heap's thread
   slot (g + 8) read and the thread's value asked from get at, then the heap's lock; t for scratch */
static void heap(u64 g, u32 p, u32 t, u64 get) {
    adrp(t, g);
    ldr(t, t, (u32)(g & 0xFFF));
    cbz(t, g_test.at + 0x40);
    cbz(p, g_test.at + 0x3C);
    adrp(t, g + 8);
    ldr(t, t, (u32)((g + 8) & 0xFFF));
    put(0xB9404800u | t << 5); /* ldr w0, [t, #72] */
    bl(get);
    put(0x91024000u | 24); /* add x24, x0, #144: the heap's lock */
    put(RET);
}
/* operator new: the default allocator's Alloc, through one load of the delete's addresses */
static u64 new_at(u64 entry, u64 g) {
    g_test.at = entry;
    adrp(8, g + 0x20);
    ldr(0, 8, (u32)((g + 0x20) & 0xFFF));
    ldr(8, 0, 0);
    ldr(8, 8, 0x28);
    put(0xD61F0100u); /* br x8 */
    return g_test.at - entry;
}
/* the delete and operator new every case has, the new at base + 0x200: its address */
static u64 pair(u64 base, u64 g, u32 p) {
    u64 sz = delete_at(base + 0x100, g, p);
    sym(base + 0x100, sz, "_ZdlPv");
    sz = new_at(base + 0x200, g);
    sym(base + 0x200, sz, "_Znwm");
    return base + 0x200;
}

/* the line alloc_new_shape said last: module m's delete name at del, which loads first, copied once, the copy's at
   copy */
static void said(const char* m, const char* name, u64 del, u64 first, u64 copy) {
    char want[512];
    snprintf(
        want, sizeof want,
        "hooks: %s copies the code of its %s at %llx, which looks a block's allocator up by its address and loads "
        "from %llx and 2 more guest addresses, to 1 place no hook takes, the first at %llx: its operator new keeps "
        "its own code",
        m, name, (unsigned long long)del, (unsigned long long)first, (unsigned long long)copy);
    if (strncmp(g_test.log, want, strlen(want))) {
        fprintf(stderr, "said:  %swanted: %s\n", g_test.log, want);
        abort();
    }
}

int main(void) {
    u64 base[CASES], g[CASES], n[CASES], sz;
    static const char* const names[CASES] = {"plain", "inlined", "clone",  "family", "spread", "forwards",
                                             "heap",  "sized",   "static", "named",  "twice"};
    for (int k = 0; k < CASES; k++) {
        base[k] = (u64)k * MOD, g[k] = base[k] + GLOBAL;
        g_test.info[k] =
            (SnxModInfo){.name = names[k], .base = base[k], .size = MOD, .text_off = 0, .text_size = GLOBAL};
    }

    /* 0: the delete and operator new alone */
    n[0] = pair(base[0], g[0], 0);
    /* 1: and a destructor that inlines it, which no symbol names */
    n[1] = pair(base[1], g[1], 0);
    inlined(base[1] + 0x1000, g[1], 0);
    /* 2: _ZdlPvm branches to a copy of the delete that no symbol names; both look x19 up, the block moved there */
    n[2] = pair(base[2], g[2], 19);
    g_test.at = base[2] + 0x300;
    b(base[2] + 0x2000);
    sym(base[2] + 0x300, 4, "_ZdlPvm");
    delete_at(base[2] + 0x2000, g[2], 19);
    /* 3: the same code in delete[] and inside realloc, functions the family hooks */
    n[3] = pair(base[3], g[3], 0);
    sz = delete_at(base[3] + 0x400, g[3], 0);
    sym(base[3] + 0x400, sz, "_ZdaPv");
    inlined(base[3] + 0x1000, g[3], 0);
    sym(base[3] + 0x1000, 0x200, "realloc");
    /* 4: a function that loads the same addresses, too far apart for a copy */
    n[4] = pair(base[4], g[4], 0);
    inlined(base[4] + 0x1000, g[4], 1);
    /* 5: a delete that forwards to free, and a destructor that inlines that */
    g_test.at = base[5] + 0x100;
    cbz(0, g_test.at + 8), b(base[5] + 0x600), put(RET);
    sym(base[5] + 0x100, 12, "_ZdlPv");
    sz = new_at(n[5] = base[5] + 0x200, g[5]);
    sym(n[5], sz, "_Znwm");
    g_test.at = base[5] + 0x600;
    put(RET);
    sym(base[5] + 0x600, 4, "free");
    g_test.at = base[5] + 0x1000;
    put(NOP), bl(base[5] + 0x600), put(RET);
    /* 6: a delete that asks the thread's heap, loading two globals, and a destructor that inlines it */
    g_test.at = base[6] + 0x100;
    mov(19, 0);
    heap(g[6], 19, 8, base[6] + 0x600);
    sym(base[6] + 0x100, g_test.at - (base[6] + 0x100), "_ZdlPv");
    sz = new_at(n[6] = base[6] + 0x200, g[6]);
    sym(n[6], sz, "_Znwm");
    g_test.at = base[6] + 0x600;
    put(RET);
    g_test.at = base[6] + 0x1000;
    heap(g[6], 20, 10, base[6] + 0x600);
    /* 7: a sized delete that indexes the same table by its size, and a destructor that inlines the lookup's loads */
    sz = delete_at(base[7] + 0x100, g[7], 1);
    sym(base[7] + 0x100, sz, "_ZdlPvm");
    sz = new_at(n[7] = base[7] + 0x200, g[7]);
    sym(n[7], sz, "_Znwm");
    inlined(base[7] + 0x1000, g[7], 0);
    /* 8: the lookup on a table and a base at link-time addresses, which loads one global, the default allocator, and
       a destructor that inlines it: no verdict */
    for (u64 e = base[8] + 0x100; e <= base[8] + 0x1000; e += 0x1000 - 0x100) {
        g_test.at = e;
        adrp(9, g[8] + 0x100), add(9, 9, 0x100), adrp(8, g[8] + 0x200), add(8, 8, 0x200), sub(8, 0, 8);
        put(0xD354FD08u), ldr_reg(8, 9, 8);       /* lsr x8, x8, #20; ldr x8, [x9, x8] */
        adrp(10, g[8] + 0x20), ldr(10, 10, 0x20); /* the default allocator */
        put(0xF100011Fu), put(0x9A880140u);       /* cmp x8, #0; csel x0, x10, x8, eq */
        ldr(8, 0, 0), ldr(2, 8, 0x30), put(0xD61F0040u);
        if (e == base[8] + 0x100) sym(e, g_test.at - e, "_ZdlPv");
    }
    sz = new_at(n[8] = base[8] + 0x200, g[8]);
    sym(n[8], sz, "_Znwm");
    /* 9: a sized delete like 7's first, then a delete that does the lookup, copied inline: the line names the latter */
    sz = delete_at(base[9] + 0x100, g[9], 1);
    sym(base[9] + 0x100, sz, "_ZdaPvm");
    sz = new_at(n[9] = base[9] + 0x200, g[9]);
    sym(n[9], sz, "_Znwm");
    sz = delete_at(base[9] + 0x400, g[9], 0);
    sym(base[9] + 0x400, sz, "_ZdlPv");
    inlined(base[9] + 0x1000, g[9], 0);
    /* 10: as 1, with every operator new asked */
    n[10] = pair(base[10], g[10], 0);
    sz = new_at(base[10] + 0x280, g[10]);
    sym(base[10] + 0x280, sz, "_Znam");
    inlined(base[10] + 0x1000, g[10], 0);

    assert(alloc_new_shape(n[0]) && !g_test.said);
    assert(!alloc_new_shape(n[1]) && g_test.said == 1);
    said(names[1], "_ZdlPv", base[1] + 0x100, g[1] + 0x10, base[1] + 0x1000 + 0x2C); /* the inlined copy's 3rd load */
    assert(!alloc_new_shape(n[2]) && g_test.said == 2);
    said(names[2], "_ZdlPv", base[2] + 0x100, g[2] + 0x10, base[2] + 0x2000 + 0x30); /* the clone's, once for both */
    assert(alloc_new_shape(n[3]) && g_test.said == 2);
    assert(alloc_new_shape(n[4]) && g_test.said == 2);
    assert(alloc_new_shape(n[5]) && g_test.said == 2);
    assert(alloc_new_shape(n[6]) && g_test.said == 2);
    assert(alloc_new_shape(n[7]) && g_test.said == 2);
    assert(alloc_new_shape(n[8]) && g_test.said == 2);
    assert(!alloc_new_shape(n[9]) && g_test.said == 3);
    said(names[9], "_ZdlPv", base[9] + 0x400, g[9] + 0x10, base[9] + 0x1000 + 0x2C);
    assert(!alloc_new_shape(n[10]) && !alloc_new_shape(base[10] + 0x280) && !alloc_new_shape(n[10]) &&
           g_test.said == 4);
    assert(alloc_new_shape(sizeof g_test.guest + 0x100) && g_test.said == 4); /* in no module */
    puts("PASS operator new's check: a delete that looks the block's allocator up by its address, copied inline or to "
         "a function no symbol names, keeps the module's new its own; its own body, the family's functions, wide "
         "loads, a forwarder, a heap's delete, a lookup by the size and one on a link-time table do not; said once");
    return 0;
}
