/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook_install (src/nx/hle/hook/hook.c) over a full table, the spec tests/CMakeLists.txt writes with as many hooks as
   tools/hookgen takes, with the runtime stubbed: a game whose module exports every one of their functions gets each
   one hooked, the last as the first. With "twice", a second module exports them all again, and the install stops the
   game at the first function it has no site for, naming it and the sites there are */
#include "nx/hle/hook/hook.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>

#define CODE 0x1000u

/* ---- the handler of every hook */
static unsigned calls;
void test_full(CPU* c) {
    (void)c;
    calls++;
}

/* ---- the game: each module exports the table's functions in its order, 4 bytes apart; each slot holds the
   function's own code */
static unsigned own_calls;
static void own(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    own_calls++;
    c->pc = c->x[30];
}
static GuestFn slots[2 * HOOK_COUNT];
static u32 exporting = 1; /* the modules that export the table's functions */
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= exporting * HOOK_COUNT) return 0;
    *addr = CODE + 4ull * i, *name = hook_symbol[i % HOOK_COUNT];
    return 1;
}
static const SnxModInfo modules[2] = {{.name = "sdk", .base = CODE, .size = 4 * HOOK_COUNT},
                                      {.name = "subsdk0", .base = CODE + 4 * HOOK_COUNT, .size = 4 * HOOK_COUNT}};
static SnxModule module = {.nmods = 2, .mods = modules};
const SnxModule* g_mod = &module;
const SnxArmModule* g_arm_mod;
GuestFn* g_disp = slots;
u64 g_code_lo = CODE, g_code_hi = CODE + sizeof slots / sizeof slots[0] * 4;
static u8 guest[0x2000];
u8* g_mem = guest;
Hle g_hle;
HleHost g_host;
Settings g_settings;
const SnxRoutes* g_routes;

/* ---- the runtime, as far as hook.c reaches it */
u64 kmem_alloc(u64 size, u32 state, u32 perm) { return (void)size, (void)state, (void)perm, 0; }
void mem_query(u64 addr, MemBlock* b) {
    memset(b, 0, sizeof *b);
    b->addr = addr, b->state = MS_CODE, b->perm = PERM_R | PERM_X;
}
GuestFn nce_slot(GuestFn f) { return f; }
void nce_slot_taken(u64 addr, const char* what) { (void)addr, (void)what; }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn; }
GuestFn jit_code_now(u64 pc) { return (void)pc, NULL; }
SnxArmBlock* arm_slot(u32 addr, int thumb) { return (void)addr, (void)thumb, NULL; }
SnxArmBlock arm_block(u32 addr, int thumb) { return (void)addr, (void)thumb, NULL; }
SnxArmCPU* arm_cpu(CPU* c) { return (void)c, NULL; }
void arm_run(SnxArmCPU* c, SnxArmBlock first, u32 stop_at) { (void)c, (void)first, (void)stop_at; }
int linker_links(void) { return 0; }
int hook_verify_call(CPU* c, u64 entry, GuestFn original, GuestFn handler, const HookVerify* v, u64 scratch,
                     u64 scratch_size, HookVerifyResult* r) {
    (void)c, (void)entry, (void)original, (void)handler, (void)v, (void)scratch, (void)scratch_size, (void)r;
    assert(!"no family is verified");
    return HOOK_AGREE;
}
const char* hook_verify_text(int what) { return (void)what, ""; }
size_t snx_hook_route_count(void) { return 0; }
SnxU64 snx_hook_route_hash(const char* name) { return (void)name, 0; }
SnxU64 snx_hook_route_list_hash(void) { return 0; }
int snx_hook_key_at(const char* name, const unsigned char build_id[32], SnxU64* offset) {
    return (void)name, (void)build_id, (void)offset, 0;
}
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stdout);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    exit(1);
}

int main(int argc, char** argv) {
    static CPU c;
    u32 i;
    assert(HOOK_COUNT == 1024); /* the spec tests/CMakeLists.txt writes */
    for (i = 0; i < sizeof slots / sizeof slots[0]; i++) slots[i] = own;
    c.mem = guest;
    g_settings.native_hooks = 1;
    if (argc > 1 && !strcmp(argv[1], "twice")) {
        exporting = 2;
        hook_install(); /* stops in fatal at the second module's first function, as ctest's hook_sites_twice reads */
        printf("hook_sites: the install went on past %u sites\n", (unsigned)g_host.hook.count);
        return 1;
    }
    hook_install();
    assert(g_host.hook.count == HOOK_COUNT);
    for (i = 0; i < HOOK_COUNT; i++) assert(slots[i] == hook_fn[i] && g_host.hook.addr[i] == CODE + 4ull * i);
    /* the last function hooked runs its handler, not its own code */
    c.x[30] = 0x9000;
    slots[HOOK_COUNT - 1](&c, CODE + 4ull * (HOOK_COUNT - 1));
    assert(calls == 1 && own_calls == 0 && c.pc == 0x9000);
    puts("hook_sites: passed");
    return 0;
}
