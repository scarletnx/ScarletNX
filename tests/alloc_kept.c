/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* alloc.c's check of a game's own allocator: alloc_object_call finds a function that takes its block from an allocator
   object through the object's virtual table, as one game's malloc (a static object's Allocate), operator new (the
   thread's current allocator's) and operator delete (the owner's Free, the object moved to x0) do. Not one: a PLT
   stub's load from its GOT, a function pointer in a global, a table of functions a global points to with no object
   passed, a call through a table whose object isn't in x0 or was overwritten before the call, a malloc that calls the
   SDK's allocator directly, and a virtual call past the end of the function before it.
   alloc_kept reads its verdict once a run, so each program is a run of its own: "off", whose malloc calls an object
   (the family is off, naming it), and "on", whose malloc calls the SDK's allocator while its operator new and free
   call objects, as another game's do (the family stays on) */
#include "nx/hle/hook/hook.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOD 0x10000u /* the module: its functions, 0x100 bytes apart, then its globals at GLOBAL */
#define GLOBAL 0x8000u

static struct {
    u8 guest[2 * MOD];
    SnxModInfo info[2];
    char log[4096];
    struct {
        u64 addr, size;
        const char* name;
    } syms[16];
    u32 nsyms;
    u64 at;
} g_test;
u8* g_mem = g_test.guest;
MemLayout g_layout;
static SnxModule g_module = {.nmods = 2, .mods = g_test.info};
const SnxModule* g_mod = &g_module;
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
}
void fatal(const char* format, ...) {
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
#define NOP      0xD503201Fu
#define RET      0xD65F03C0u
#define PACIBSP  0xD503237Fu
#define AUTIBSP  0xD50323FFu
#define STP_FP   0xA9BF7BFDu /* stp x29, x30, [sp, #-0x10]! */
#define LDP_FP   0xA8C17BFDu /* ldp x29, x30, [sp], #0x10 */
static void adrp(u32 d, u64 target) {
    i64 pages = (i64)(target >> 12) - (i64)(g_test.at >> 12);
    put(0x90000000u | (u32)(pages & 3) << 29 | (u32)(pages >> 2 & 0x7FFFF) << 5 | d);
}
static void add(u32 d, u32 n, u32 imm) { put(0x91000000u | imm << 10 | n << 5 | d); }
static void ldr(u32 t, u32 n, u32 off) { put(0xF9400000u | off / 8 << 10 | n << 5 | t); }
static void mov(u32 d, u32 m) { put(0xAA0003E0u | m << 16 | d); }
static void movz(u32 d, u32 imm) { put(0x52800000u | imm << 5 | d); } /* mov wd, #imm */
static void br(u32 n) { put(0xD61F0000u | n << 5); }
static void blr(u32 n) { put(0xD63F0000u | n << 5); }
static void bl(u64 to) { put(0x94000000u | (u32)(((i64)to - (i64)g_test.at) >> 2 & 0x3FFFFFF)); }
static void cbz(u32 t, u64 to) { put(0xB4000000u | (u32)(((i64)to - (i64)g_test.at) >> 2 & 0x7FFFF) << 5 | t); }

/* each case's function at its own place, its size */
static u64 start(u32 k) { return g_test.at = (u64)k * 0x100; }
static u64 size_of(u64 entry) { return g_test.at - entry; }

int main(int argc, char** argv) {
    int off = argc > 1 && !strcmp(argv[1], "off");
    g_test.info[0] = (SnxModInfo){.name = "main", .base = 0, .size = MOD, .text_size = GLOBAL};
    g_test.info[1] = (SnxModInfo){.name = "sdk", .base = MOD, .size = MOD, .text_size = GLOBAL};
    u64 obj = GLOBAL + 0x40, at[16], size[16];

    /* 0: malloc as one game's: a static object's Allocate (its table's slot 3), the object in x0 */
    at[0] = start(0);
    put(PACIBSP), put(STP_FP), adrp(0, obj), add(0, 0, (u32)(obj & 0xFFF)), ldr(8, 0, 0), movz(2, 16), ldr(3, 8, 0x18);
    put(LDP_FP), put(AUTIBSP), br(3);
    size[0] = size_of(at[0]);
    /* 1: operator new: the thread's allocator (a call gives its slot), else a default object; a guard skips ahead */
    at[1] = start(1);
    put(PACIBSP), put(STP_FP), mov(19, 0), bl(0x900), ldr(0, 0, 0), cbz(0, g_test.at + 12), ldr(8, 0, 0);
    mov(1, 19), ldr(3, 8, 0x18), put(LDP_FP), put(AUTIBSP), br(3);
    size[1] = size_of(at[1]);
    /* 2: operator delete: the owner's Free (slot 5) with the object moved into x0 */
    at[2] = start(2);
    ldr(8, 20, 0), mov(0, 20), mov(1, 19), ldr(8, 8, 0x28), blr(8), put(RET);
    size[2] = size_of(at[2]);
    /* 3: a PLT stub: one load from its GOT entry (here at a page's start), and the branch */
    at[3] = start(3);
    adrp(16, GLOBAL), ldr(17, 16, 0), add(16, 16, 0), br(17);
    size[3] = size_of(at[3]);
    /* 4: a function pointer in a global */
    at[4] = start(4);
    adrp(8, GLOBAL), ldr(8, 8, 0x10), br(8);
    size[4] = size_of(at[4]);
    /* 5: a table of functions a global points to, the global's address no object the call is passed */
    at[5] = start(5);
    adrp(9, GLOBAL), ldr(8, 9, 0), ldr(10, 8, 8), br(10);
    size[5] = size_of(at[5]);
    /* 6: an object's table, but the object in x1 at the call */
    at[6] = start(6);
    ldr(8, 1, 0), ldr(2, 8, 0x18), br(2);
    size[6] = size_of(at[6]);
    /* 7: the object in x0 at the load, x0 overwritten before the call */
    at[7] = start(7);
    ldr(8, 0, 0), mov(0, 1), ldr(3, 8, 0x18), br(3);
    size[7] = size_of(at[7]);
    /* 8: malloc as the SDK's: a direct call of its allocator */
    at[8] = start(8);
    put(STP_FP), mov(1, 0), bl(0x900), put(LDP_FP), put(RET);
    size[8] = size_of(at[8]);
    /* 9: a return, then another function's virtual call, the size unknown: the walk stops at the return */
    at[9] = start(9);
    put(NOP), put(RET), ldr(8, 0, 0), ldr(3, 8, 0x18), br(3);
    size[9] = 0;
    /* 10: the same with the size known */
    at[10] = start(10);
    put(NOP), put(RET), ldr(8, 0, 0), ldr(3, 8, 0x18), br(3);
    size[10] = 8;

    static const int want[11] = {1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
    int failed = 0;
    for (u32 k = 0; k < 11; k++) {
        int got = alloc_object_call(at[k], size[k]);
        if (got != want[k]) fprintf(stderr, "case %u: %d, wanted %d\n", k, got, want[k]), failed = 1;
    }

    /* the program's functions: the verdict's run */
    typedef typeof(g_test.syms[0]) Sym;
    if (off) {
        g_test.syms[g_test.nsyms++] = (Sym){at[0], size[0], "malloc"};
        g_test.syms[g_test.nsyms++] = (Sym){at[2], size[2], "free"};
        g_test.syms[g_test.nsyms++] = (Sym){MOD + 0x100, 0, "_Znam"};
    } else {
        g_test.syms[g_test.nsyms++] = (Sym){at[8], size[8], "malloc"};
        g_test.syms[g_test.nsyms++] = (Sym){at[2], size[2], "free"};
        g_test.syms[g_test.nsyms++] = (Sym){at[1], size[1], "_Znwm"};
        g_test.syms[g_test.nsyms++] = (Sym){at[3], size[3], "calloc"};
    }
    const char* why = alloc_kept();
    if (off ? !why || !strstr(why, "the game's malloc (main+0)") || alloc_kept() != why : why != NULL) {
        fprintf(stderr, "alloc_kept (%s): %s\n", off ? "off" : "on", why ? why : "(null)");
        failed = 1;
    }
    if (!failed) printf("alloc_kept (%s): all checks pass\n", off ? "off" : "on");
    return failed;
}
