/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook_install on a 32-bit game (src/nx/hle/hook/hook.c), over tests/hook/a32_marked.hook, with the runtime stubbed to
   a symbol list and a code table of slots: a hook declared a32 takes its function's slot, an A32 function's and a
   Thumb one's alike, and its adapter runs the handler with the arguments AAPCS32 gives it; one not declared a32, a
   symbol no hook names and a family that hooks_off names keep their slots */
#include "nx/hle/hook/hook.c"
#include <assert.h>
#include <stdio.h>

/* ---- the handlers */
static u64 seen[2];
u32 test_answer(CPU* c, u32 a, int b) {
    (void)c;
    seen[0] = a, seen[1] = (u64)b;
    return 0x1234;
}
void test_thumb_answer(CPU* c, u64 out) { c->mem[out] = 0x5a; }
u64 test_wide(CPU* c, u64 size) { return (void)c, size; }
u32 test_off(CPU* c) { return (void)c, 1; }

/* ---- the runtime, as far as hook_install reaches it */
static const struct {
    const char* name;
    u64 addr; /* bit 0 set for a Thumb function */
} symbols[] = {{"answer", 0x1000},     {"wide", 0x1010},
               {"unhooked", 0x1020},   {"thumb_answer", 0x2001},
               {"off_answer", 0x1030}, {"answer", 0x100001000ull /* outside a 32-bit game's addresses */}};
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= sizeof symbols / sizeof symbols[0]) return 0;
    *addr = symbols[i].addr, *name = symbols[i].name;
    return 1;
}
static void own_block(SnxArmCPU* a) { (void)a; }
static SnxArmBlock a32_slots[0x40], thumb_slots[0x40];
SnxArmBlock* arm_slot(u32 addr, int thumb) {
    assert(!(addr & 1));
    if (thumb) return addr >= 0x2000 && addr < 0x2080 ? &thumb_slots[(addr - 0x2000) / 2] : NULL;
    return addr >= 0x1000 && addr < 0x1100 ? &a32_slots[(addr - 0x1000) / 4] : NULL;
}
static SnxArmModule module;
const SnxArmModule* g_arm_mod = &module;
const SnxModule* g_mod;
GuestFn* g_disp;
u64 g_code_lo, g_code_hi;
u8* g_mem;
Hle g_hle;
Settings g_settings;
const SnxRoutes* g_routes;
static CPU cpu32;
static u8 mem32[0x100];
CPU* hook_a32_cpu(SnxArmCPU* arm) { return (void)arm, &cpu32; }
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void fatal(const char* fmt, ...) {
    fprintf(stderr, "fatal: %s\n", fmt);
    exit(1);
}
/* the 64-bit side, which a 32-bit game never reaches */
static void not_reached(void) { assert(!"the 64-bit install ran for a 32-bit game"); }
u64 kmem_alloc(u64 size, u32 state, u32 perm) { return (void)size, (void)state, (void)perm, not_reached(), 0; }
void mem_query(u64 addr, MemBlock* b) { (void)addr, (void)b, not_reached(); }
GuestFn nce_slot(GuestFn f) { return not_reached(), f; }
void nce_slot_taken(u64 addr, const char* what) { (void)addr, (void)what, not_reached(); }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn, not_reached(); }
GuestFn jit_code_now(u64 pc) { return (void)pc, not_reached(), NULL; }
int linker_links(void) { return 0; }
int hook_verify_call(CPU* c, u64 entry, GuestFn original, GuestFn handler, const HookVerify* v, u64 scratch,
                     u64 scratch_size, HookVerifyResult* r) {
    (void)c, (void)entry, (void)original, (void)handler, (void)v, (void)scratch, (void)scratch_size, (void)r;
    return not_reached(), 0;
}
const char* hook_verify_text(int what) { return (void)what, ""; }
size_t snx_hook_route_count(void) { return 0; }
SnxU64 snx_hook_route_hash(const char* name) { return (void)name, 0; }
SnxU64 snx_hook_route_list_hash(void) { return 0; }
int snx_hook_key_at(const char* name, const unsigned char build_id[32], SnxU64* offset) {
    return (void)name, (void)build_id, (void)offset, 0;
}

int main(void) {
    static SnxArmCPU a;
    int answer = -1, thumb = -1, wide = -1, off = -1;
    for (int i = 0; i < HOOK_COUNT; i++) {
        if (!strcmp(hook_symbol[i], "answer")) answer = i;
        if (!strcmp(hook_symbol[i], "thumb_answer")) thumb = i;
        if (!strcmp(hook_symbol[i], "wide")) wide = i;
        if (!strcmp(hook_symbol[i], "off_answer")) off = i;
    }
    /* hookgen --a32-marked: an adapter for each hook declared a32, none for the other */
    assert(answer >= 0 && thumb >= 0 && wide >= 0 && off >= 0);
    assert(hook_a32[answer] && hook_a32[thumb] && hook_a32[off] && !hook_a32[wide]);

    for (unsigned i = 0; i < 0x40; i++) a32_slots[i] = thumb_slots[i] = own_block;
    g_settings.native_hooks = 1;
    snprintf(g_settings.hook_families_off, sizeof g_settings.hook_families_off, "switched_off");
    hook_install();
    assert(a32_slots[0] == hook_a32[answer]);  /* an A32 function */
    assert(thumb_slots[0] == hook_a32[thumb]); /* a Thumb one, by its address without bit 0 */
    assert(a32_slots[4] == own_block);         /* not declared a32: its own code */
    assert(a32_slots[8] == own_block);         /* no hook names it */
    assert(a32_slots[12] == own_block);        /* its family is off */
    for (unsigned i = 1; i < 0x40; i++) assert(thumb_slots[i] == own_block);

    /* the adapter: r0 and r1 to the handler, its answer in r0, back to lr */
    cpu32.mem = mem32;
    a.mem = mem32;
    a.mem_size = sizeof mem32;
    a.r[0] = 7, a.r[1] = 1, a.r[14] = 0x3000;
    a32_slots[0](&a);
    assert(seen[0] == 7 && seen[1] == 1 && a.r[0] == 0x1234 && a.r[15] == 0x3000);
    a.r[0] = 0x40, a.r[14] = 0x3001; /* back into Thumb code */
    thumb_slots[0](&a);
    assert(mem32[0x40] == 0x5a && a.r[15] == 0x3000 && (a.cpsr & SNX_ARM_T));

    /* hooks=0 takes nothing */
    for (unsigned i = 0; i < 0x40; i++) a32_slots[i] = thumb_slots[i] = own_block;
    g_settings.native_hooks = 0;
    hook_install();
    assert(a32_slots[0] == own_block && thumb_slots[0] == own_block);
    puts("hook_install_a32: passed");
    return 0;
}
