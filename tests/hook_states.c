/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* native_cond_var (src/nx/hle/hook/family.c, hook.c) over tests/hook/states.hook, with the runtime stubbed: at 1 the
   cond_var family's functions take its handlers and save states are refused with a message that names the setting
   and its value; at 0 they keep their own code (the recursive mutex's with the condition variable's) and states may
   be taken. hooks_off naming the family, or the game changing every hooked function's code, leaves the game's too.
   native_os_sync the same for the os_sync family, which also stays the game's where its SDK lacks a function that
   writes the words the family owns (os_sync_ready) */
#include "nx/hle/hook/hook.c"
#include "nx/hle/hook/family.c"
#include <assert.h>
#include <stdio.h>

#define CODE 0x1000u
#define LOCK (CODE + 0x00)
#define WAIT (CODE + 0x10)
#define NOW  (CODE + 0x20)
#define SLEEP (CODE + 0x30)

i64 test_now(CPU* c) { return (void)c, 0; }
void test_lock(CPU* c, u64 m) { (void)c, (void)m; }
void test_wait(CPU* c, u64 cv) { (void)c, (void)cv; }
void test_sleep(CPU* c, i64 ns) { (void)c, (void)ns; }

/* ---- the game: two functions of one module, each slot holding its own code */
static void own_code(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    c->pc = c->x[30];
}
static GuestFn slots[0x400];
static const struct {
    const char* name;
    u64 addr;
} symbols[] = {{"lock", LOCK}, {"wait", WAIT}, {"now", NOW}, {"sleep", SLEEP}};
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= sizeof symbols / sizeof symbols[0]) return 0;
    *addr = symbols[i].addr, *name = symbols[i].name;
    return 1;
}
static const SnxModInfo modules[1] = {{.name = "sdk", .base = CODE, .size = 0x1000}};
static SnxModule module = {.nmods = 1, .mods = modules};
const SnxModule* g_mod = &module;
const SnxArmModule* g_arm_mod;
GuestFn* g_disp = slots;
u64 g_code_lo = CODE, g_code_hi = CODE + 0x1000;
static u8 guest[0x2000];
u8* g_mem = guest;
Hle g_hle;
HleHost g_host;
Settings g_settings;
const SnxRoutes* g_routes;

/* ---- the runtime, as far as hook.c and family.c reach it */
u64 kmem_alloc(u64 size, u32 state, u32 perm) { return (void)size, (void)state, (void)perm, 0; }
void mem_query(u64 addr, MemBlock* b) {
    memset(b, 0, sizeof *b);
    b->addr = addr, b->state = MS_CODE, b->perm = PERM_R | PERM_X;
}
GuestFn nce_slot(GuestFn f) { return f; }
void nce_slot_taken(u64 addr, const char* what) { (void)addr, (void)what; }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn; }
GuestFn jit_code_now(u64 pc) { return (void)pc, (GuestFn)0; }
SnxArmBlock* arm_slot(u32 addr, int thumb) { return (void)addr, (void)thumb, NULL; }
SnxArmBlock arm_block(u32 addr, int thumb) { return (void)addr, (void)thumb, (SnxArmBlock)0; }
SnxArmCPU* arm_cpu(CPU* c) { return (void)c, NULL; }
void arm_run(SnxArmCPU* c, SnxArmBlock first, u32 stop_at) { (void)c, (void)first, (void)stop_at; }
int linker_links(void) { return 0; }
int hook_verify_call(CPU* c, u64 entry, GuestFn original, GuestFn handler, const HookVerify* v, u64 scratch,
                     u64 scratch_size, HookVerifyResult* r) {
    (void)c, (void)entry, (void)original, (void)handler, (void)v, (void)scratch, (void)scratch_size, (void)r;
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
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
/* ext_skyline.c's: no hook for a mod's trampoline */
GuestFn hook_ext_skyline(const char* symbol, u32 sdk, const char** handler) {
    return (void)symbol, (void)sdk, (void)handler, NULL;
}
void fatal(const char* fmt, ...) {
    fprintf(stderr, "fatal: %s\n", fmt);
    exit(1);
}
int alloc_ready(void) { return 0; }
int libc_wide_ready(void) { return 0; }
static int g_os_ready = 1;
int os_sync_ready(void) { return g_os_ready; }

/* hook_install over fresh slots, with native_cond_var and hooks_off as given; 1 when the condition variable's
   function took its hook, which the recursive mutex's follows: a mutex the host holds is out of a state as a wait on
   the host is */
static int install(int native, const char* off) {
    unsigned i;
    memset(&g_host, 0, sizeof g_host);
    for (i = 0; i < sizeof slots / sizeof slots[0]; i++) slots[i] = own_code;
    g_settings.native_hooks = 1;
    g_settings.native_cond_var = native;
    g_settings.native_os_sync = 0;
    snprintf(g_settings.hook_families_off, sizeof g_settings.hook_families_off, "%s", off);
    hook_install();
    assert(slots[(NOW - CODE) >> 2] != own_code); /* the sync family stays native either way */
    assert((slots[(LOCK - CODE) >> 2] != own_code) == (slots[(WAIT - CODE) >> 2] != own_code));
    return slots[(WAIT - CODE) >> 2] != own_code;
}

int main(void) {
    char why[128];
    /* native: the wait is the host's, and a state is refused naming the setting and its value */
    assert(install(1, "") == 1);
    assert(hook_family_unready("cond_var") == NULL);
    memset(why, 0, sizeof why);
    assert(hook_states_refused(why, sizeof why) == -1);
    assert(!strcmp(why, "save states need native_cond_var=0 (it is 1)"));
    /* the game changed the hooked functions' code (a mod's hook): the wait's own code alone leaves the host's mutex,
       which still holds states back; with both its own, states may be taken */
    for (u32 i = 0; i < g_host.hook.count; i++)
        if (!strcmp(hook_symbol[g_host.hook.hook[i]], "wait")) g_host.hook.invalid[i] = 1;
    assert(hook_states_refused(why, sizeof why) == -1);
    for (u32 i = 0; i < g_host.hook.count; i++)
        if (!strcmp(hook_symbol[g_host.hook.hook[i]], "lock")) g_host.hook.invalid[i] = 1;
    assert(hook_states_refused(why, sizeof why) == 0);
    /* emulated: the game's own wait, said with the value, and states may be taken */
    assert(install(0, "") == 0);
    assert(strstr(hook_family_unready("cond_var"), "native_cond_var is 0"));
    why[0] = 0;
    assert(hook_states_refused(why, sizeof why) == 0 && !why[0]);
    /* hooks_off names the family: the same */
    assert(install(1, "cond_var") == 0);
    assert(hook_states_refused(why, sizeof why) == 0);
    /* a 32-bit game runs none of the family */
    g_arm_mod = (const SnxArmModule*)&module;
    assert(!strcmp(hook_family_unready("cond_var"), "its handlers are for 64-bit games"));
    g_arm_mod = NULL;
    /* os_sync: native, the sleep is the host's and a state is refused naming its setting */
    memset(&g_host, 0, sizeof g_host);
    for (u32 i = 0; i < sizeof slots / sizeof slots[0]; i++) slots[i] = own_code;
    g_settings.native_cond_var = 0;
    g_settings.native_os_sync = 1;
    g_settings.hook_families_off[0] = 0;
    hook_install();
    assert(slots[(SLEEP - CODE) >> 2] != own_code && slots[(WAIT - CODE) >> 2] == own_code);
    assert(hook_family_unready("os_sync") == NULL);
    assert(hook_states_refused(why, sizeof why) == -1);
    assert(!strcmp(why, "save states need native_os_sync=0 (it is 1)"));
    /* an SDK without every word writer: the game's own code, and states may be taken */
    g_os_ready = 0;
    memset(&g_host, 0, sizeof g_host);
    for (u32 i = 0; i < sizeof slots / sizeof slots[0]; i++) slots[i] = own_code;
    hook_install();
    assert(slots[(SLEEP - CODE) >> 2] == own_code);
    assert(strstr(hook_family_unready("os_sync"), "doesn't export every function"));
    assert(hook_states_refused(why, sizeof why) == 0);
    g_os_ready = 1;
    /* emulated */
    g_settings.native_os_sync = 0;
    assert(strstr(hook_family_unready("os_sync"), "native_os_sync is 0"));
    g_arm_mod = (const SnxArmModule*)&module;
    assert(!strcmp(hook_family_unready("os_sync"), "its handlers are for 64-bit games"));
    g_arm_mod = NULL;
    puts("hook_states: passed");
    return 0;
}
