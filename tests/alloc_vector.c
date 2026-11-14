/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* alloc.c's std::vector storage hook: alloc_vector_shape takes the destructor of a vector of a T with nothing to
   destroy (no loop, one call or tail call of operator delete, directly or through a PLT stub) and leaves every other
   shape to its own code; alloc_vector_free does nothing for an empty vector and gives a pointer the arena didn't make
   to the function's own code */
#include "nx/hle/hook/hook.h"
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/loader.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int alloc_vector_shape(u64 addr);
void alloc_vector_free(CPU* c, u64 self);

#define DELETE 0x8000u
#define OTHER  0x9000u

static u8 g_guest[0x10000];
u8* g_mem = g_guest;
MemLayout g_layout;
static const SnxModInfo g_info = {.name = "main", .base = 0, .size = sizeof g_guest};
static const SnxModule g_module = {.nmods = 1, .mods = &g_info};
const SnxModule* g_mod = &g_module;
static int g_original;

u64 kernel_find_symbol_exact(const char* name) { return !strcmp(name, "_ZdlPv") ? DELETE : 0; }
void hook_run_original(CPU* c, u64 addr) {
    (void)c, (void)addr;
    g_original++;
}
void logf_(int level, const char* format, ...) { (void)level, (void)format; }
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align) { return (void)lo, (void)hi, (void)size, (void)align, 0; }
int mem_map(u64 addr, u64 size, u32 state, u32 perm) { return (void)addr, (void)size, (void)state, (void)perm, -1; }
int mem_unmap(u64 addr, u64 size) { return (void)addr, (void)size, 0; }

/* the instructions, each with its branch's offset from itself */
static u32 b(i64 off) { return 0x14000000u | (u32)(off >> 2 & 0x3FFFFFF); }
static u32 bl(i64 off) { return 0x94000000u | (u32)(off >> 2 & 0x3FFFFFF); }
static u32 cbz(i64 off) { return 0xB4000000u | (u32)(off >> 2 & 0x7FFFF) << 5; }
static u32 bcond(int cond, i64 off) { return 0x54000000u | (u32)(off >> 2 & 0x7FFFF) << 5 | (u32)cond; }
#define RET 0xD65F03C0u
#define NOP 0xD503201Fu /* the loads, stores and arithmetic, which the check doesn't read */
#define BLR 0xD63F0100u /* blr x8 */

static u64 at;
static void put(u32 w) {
    snx_st_le32(g_guest + at, w);
    at += 4;
}
/* a PLT stub at stub whose GOT entry, at got (in its page), holds to */
static void stub(u64 stub, u64 got, u64 to) {
    i64 pages = (i64)((got & ~0xFFFull) - (stub & ~0xFFFull)) >> 12;
    at = stub;
    put(0x90000010u | (u32)(pages & 3) << 29 | (u32)(pages >> 2 & 0x7FFFF) << 5);
    put(0xF9400211u | (u32)((got & 0xFFF) / 8) << 10);
    put(0x91000210u | (u32)(got & 0xFFF) << 10);
    put(0xD61F0220u);
    snx_st_le64(g_guest + got, to);
}
/* libc++'s trivial ~__vector_base: begin = [this]; if (begin) { [this + 8] = begin; delete begin } with the
   tail call at the end to target */
static u64 trivial(u64 entry, u64 target) {
    at = entry;
    put(NOP), put(NOP);          /* mov x8, x0; ldr x0, [x8] */
    put(cbz(9 * 4));             /* to the ret */
    put(NOP), put(NOP);          /* ldr x9, [x8, #8]; cmp x9, x0 */
    put(bcond(0, 5 * 4));        /* b.eq to the tail call */
    put(NOP), put(NOP), put(NOP), put(NOP); /* end rounded down to begin */
    put(b((i64)target - (i64)at)); /* the tail call */
    put(RET);
    return entry;
}

int main(void) {
    CPU c;
    /* the PLT stubs: one to operator delete, one to another function, their GOT a page on */
    stub(0x4000, 0x5008, DELETE);
    stub(0x4010, 0x5010, OTHER);
    assert(alloc_vector_shape(trivial(0x000, 0x4000)));
    assert(alloc_vector_shape(trivial(0x100, DELETE))); /* delete called directly */
    assert(!alloc_vector_shape(trivial(0x200, 0x4010))); /* a stub to another function */
    assert(!alloc_vector_shape(trivial(0x300, OTHER)));
    /* a T with a destructor: a loop over the elements, then the tail call */
    at = 0x400;
    put(NOP), put(NOP), put(cbz(5 * 4)), put(NOP), put(NOP), put(bcond(1, -8)),
        put(b(DELETE - (i64)at)), put(RET);
    assert(!alloc_vector_shape(0x400));
    /* a call of something else before delete */
    at = 0x500;
    put(NOP), put(bl(OTHER - (i64)at)), put(b(DELETE - (i64)at));
    assert(!alloc_vector_shape(0x500));
    /* with a frame: delete called, then a return */
    at = 0x600;
    put(NOP), put(cbz(3 * 4)), put(bl(DELETE - (i64)at)), put(NOP), put(RET);
    assert(alloc_vector_shape(0x600));
    /* no delete, delete twice, a call through a register */
    at = 0x700;
    put(NOP), put(RET);
    assert(!alloc_vector_shape(0x700));
    at = 0x800;
    put(cbz(2 * 4)), put(bl(DELETE - (i64)at)), put(b(DELETE - (i64)at)), put(RET);
    assert(!alloc_vector_shape(0x800));
    at = 0x900;
    put(BLR), put(b(DELETE - (i64)at));
    assert(!alloc_vector_shape(0x900));
    /* code that runs off the module */
    assert(!alloc_vector_shape(sizeof g_guest - 4) && !alloc_vector_shape(sizeof g_guest));

    /* the handler: an empty vector does nothing, a pointer the arena didn't make runs the function's own code with
       the vector as it was */
    memset(&c, 0, sizeof c);
    c.mem = g_guest;
    snx_st_le64(g_guest + 0x3000, 0);
    alloc_vector_free(&c, 0x3000);
    assert(!g_original);
    snx_st_le64(g_guest + 0x3000, 0x3100);
    snx_st_le64(g_guest + 0x3008, 0x3140);
    alloc_vector_free(&c, 0x3000);
    assert(g_original == 1 && snx_ld_le64(g_guest + 0x3008) == 0x3140);
    puts("PASS std::vector's storage hook: the shapes it takes and leaves, an empty vector, a foreign pointer");
    return 0;
}
