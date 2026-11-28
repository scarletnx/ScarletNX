/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook_install on a 32-bit game (src/nx/nn/hook.c), over tests/hook/a32_marked.hook, with the runtime stubbed to
   a symbol list and a code table of slots: a hook of the 32-bit program's table (one declared a32, or for a 32-bit
   program alone) takes its function's slot, an A32 function's and a Thumb one's alike, and its adapter runs the
   handler with the arguments AAPCS32 gives it; one not declared a32, a symbol no hook names and a family that
   hooks_off names keep their slots. A versioned hook takes the slot of a function whose module's SDK label holds
   its version, and none in a module with no label; one declared a32 for some versions is in the table for any and
   in a 32-bit program's for those, and in a game of another SDK its function keeps its own code, which
   hook_missed_text says with the SDK, and hook_unread_text with the SDK it was read in, the functions of each family
   and what a player can do. The two tables: a symbol declared again for a 32-bit program
   alone has each program's handler in its table, and one only a 32-bit program has is in no other. And a handler
   that hands its call back: the function's own block, the one its slot held, runs until the call's return address,
   and the adapter still returns there; and one that calls another function of the game, with its four words in r0
   to r3, and is back at its own function after it */
#include "nx/nn/hook.c"
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
u32 test_ranged(CPU* c, u32 a) { return (void)c, a + 1; }
u32 test_own(CPU* c, u32 a) { return a == 5 ? (u32)hook_run_original_a32(c) + 1 : 0; }
u32 test_caller(CPU* c, u32 fn) {
    hook_guest_call_a32(c, fn, 11, 12, 13, 14);
    return (u32)hook_run_original_a32(c); /* the thread is at the hooked function again: its own code is found */
}
i64 test_now(CPU* c) { return (void)c, 0x1122334455667788ll; }
void test_now_out(CPU* c, u64 out) { memcpy(c->mem + out, "\x88\x77\x66\x55\x44\x33\x22\x11", 8); }
u32 test_save(CPU* c, const HookLayout_Report32* l, u64 report, u64 size) {
    seen[0] = l->size, seen[1] = size;
    return hook_get_u32(c, l->used, report);
}

/* ---- the runtime, as far as hook_install reaches it */
static const struct {
    const char* name;
    u64 addr; /* bit 0 set for a Thumb function */
} symbols[] = {{"caller", 0x1070},
               {"own", 0x1060},
               {"own", 0x2021},
               {"answer", 0x1000},
               {"wide", 0x1010},
               {"unhooked", 0x1020},
               {"thumb_answer", 0x2001},
               {"off_answer", 0x1030},
               {"now", 0x1040},
               {"save_j", 0x1050},
               {"ranged", 0x1080},
               {"ranged_too", 0x1090},
               {"grouped", 0x10a0},
               {"save_j", 0x2011 /* in the module with no SDK label */},
               {"answer", 0x3000 /* in no module */},
               {"answer", 0x100001000ull /* outside a 32-bit game's addresses */}};
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= sizeof symbols / sizeof symbols[0]) return 0;
    *addr = symbols[i].addr, *name = symbols[i].name;
    return 1;
}
static void own_block(SnxArmCPU* a) { (void)a; }
/* the own code of the two functions named own: each returns a value of its own, and leaves lr changed */
static void own_a32(SnxArmCPU* a) { a->r[0] = 0x40 + a->r[0], a->r[14] = 0xdead; }
static void own_thumb(SnxArmCPU* a) { a->r[0] = 0x80 + a->r[0], a->r[14] = 0xdead; }
static void own_caller(SnxArmCPU* a) { a->r[0] = 0xc0, a->r[14] = 0xdead; }
static _Atomic SnxArmBlock a32_slots[0x40], thumb_slots[0x40];
_Atomic SnxArmBlock* arm_slot(u32 addr, int thumb) {
    assert(!(addr & 1));
    if (thumb) return addr >= 0x2000 && addr < 0x2080 ? &thumb_slots[(addr - 0x2000) / 2] : NULL;
    return addr >= 0x1000 && addr < 0x1100 ? &a32_slots[(addr - 0x1000) / 4] : NULL;
}
/* arm.c's: what a slot holds, and a run of one block that returns to stop_at as the function's bx lr would */
SnxArmBlock arm_block(u32 addr, int thumb) {
    const _Atomic SnxArmBlock* slot = arm_slot(addr, thumb);
    return slot ? *slot : NULL;
}
/* the game's thread as the runtime holds it (KThread): the CPU a handler is given and its 32-bit registers in one
   object, each found from the other by its place, as arm.c finds them. An adapter takes the registers through a
   restrict pointer, so a handler's way to them has to start at that pointer: registers of their own, apart from the
   CPU, are another object to the compiler, whose writes it may move past the adapter's reads */
typedef struct {
    CPU cpu;
    SnxArmCPU arm;
} Thread32;
static Thread32 thread;
static unsigned runs;
static u32 called[6]; /* a guest call's r0 to r3, its function and its state */
SnxArmCPU* arm_cpu(CPU* c) { return &((Thread32*)(void*)((char*)c - offsetof(Thread32, cpu)))->arm; }
void arm_run(SnxArmCPU* c, SnxArmBlock first, u32 stop_at) {
    runs++;
    if (first) {
        first(c);
    } else { /* from r15: the function a guest call names, which changes r0 to r3 and lr as a function may */
        memcpy(called, c->r, 4 * sizeof(u32));
        called[4] = c->r[15], called[5] = c->cpsr & SNX_ARM_T;
        c->r[0] = c->r[1] = c->r[2] = c->r[3] = 0x99, c->r[14] = 0xdead;
    }
    c->r[15] = stop_at & ~1u;
    c->cpsr = (c->cpsr & ~SNX_ARM_T) | (stop_at & 1 ? SNX_ARM_T : 0);
}
/* the game's modules: the first, with the A32 functions, has the label of SDK 1.2.3 in its read-only data; the
   second, with the Thumb ones, has none */
static u8 guest[0x4000];
static const SnxModInfo modules[2] = {
    {.name = "sdk", .base = 0x1000, .size = 0x1000, .ro_off = 0x800, .ro_size = 0x100},
    {.name = "main", .base = 0x2000, .size = 0x1000}};
static SnxArmModule module = {.nmods = 2, .mods = modules};
const SnxArmModule* g_arm_mod = &module;
const SnxModule* g_mod;
_Atomic GuestFn* g_disp;
u64 g_code_lo, g_code_hi;
u8* g_mem;
Hle g_hle;
HleHost g_host;
Settings g_settings;
const SnxRoutes* g_routes;
static u8 mem32[0x100];
CPU* hook_a32_cpu(SnxArmCPU* arm) { return &((Thread32*)(void*)((char*)arm - offsetof(Thread32, arm)))->cpu; }
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
/* hook.c takes its invalidation lock only when a hooked function's code changed */
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
/* the entry of table t for symbol at a version: hook_select's */
static int entry(const HookTable* t, const char* symbol, u32 sdk) {
    u32 versions[HOOK_KEYS] = {0};
    int first;
    versions[HOOK_SDK] = sdk;
    return hook_select(t, symbol, versions, &first);
}

/* the 64-bit side, which a 32-bit game never reaches */
static void not_reached(void) { assert(!"the 64-bit install ran for a 32-bit game"); }
u64 kmem_alloc(u64 size, u32 state, u32 perm) { return (void)size, (void)state, (void)perm, not_reached(), 0; }
void mem_query(u64 addr, MemBlock* b) { (void)addr, (void)b, not_reached(); }
GuestFn nce_slot(GuestFn f) { return not_reached(), f; }
int nce_slot_taken(u64 addr, const char* what) {
    (void)addr, (void)what, not_reached();
    return 0;
}
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
    /* hookgen --a32-marked: the hooks declared a32 are in both tables, one that isn't in the table alone, and one for
       a 32-bit program alone in that program's */
    int answer = entry(&hook32_table, "answer", 0), thumb = entry(&hook32_table, "thumb_answer", 0);
    int now = entry(&hook32_table, "now", 0), off = entry(&hook32_table, "off_answer", 0);
    char text[640];
    assert(HOOK_COUNT == 9 && HOOK32_COUNT == 13);
    assert(answer >= 0 && thumb >= 0 && now >= 0 && off >= 0);
    assert(entry(&hook_table, "answer", 0) >= 0 && entry(&hook_table, "wide", 0) >= 0);
    assert(entry(&hook32_table, "wide", 0) == -1 && entry(&hook_table, "save_j", 0) == -1);
    /* one symbol, each program's own declaration */
    assert(!strcmp(hook_handler[entry(&hook_table, "now", 0)], "test_now"));
    assert(!strcmp(hook32_handler[now], "test_now_out"));
    /* a 32-bit program's versioned hook: none for an unknown version, and the layout of each version */
    assert(entry(&hook32_table, "save_j", 0) == -2);
    assert(!strncmp(hook32_layouts[entry(&hook32_table, "save_j", 0x10000)], "Report32 of line ", 17));
    assert(!strncmp(hook32_layouts[entry(&hook32_table, "save_j", 0x20000)], "Report32 of line ", 17));
    assert(entry(&hook32_table, "save_j", 0x10000) != entry(&hook32_table, "save_j", 0x20000));
    /* a32 with versions: the table's entry is for any version, a 32-bit program's for the ones after its a32 */
    assert(hook_key[entry(&hook_table, "ranged", 0)] == HOOK_ANY);
    assert(entry(&hook32_table, "ranged", 0) == -2 && entry(&hook32_table, "ranged", 0x20000) == -2);
    assert(hook32_key[entry(&hook32_table, "ranged", 0x10203)] == HOOK_SDK);
    assert(hook32_lo[entry(&hook32_table, "ranged", 0x10203)] == 0);
    assert(hook32_hi[entry(&hook32_table, "ranged", 0x10203)] == 0x20000);
    /* its second group, one version, an entry of its own; and those of a declaration for a 32-bit program alone */
    assert(hook32_lo[entry(&hook32_table, "ranged", 0x40400)] == 0x40400);
    assert(hook32_hi[entry(&hook32_table, "ranged", 0x40400)] == 0x40401);
    assert(entry(&hook32_table, "ranged", 0x40401) == -2 && entry(&hook32_table, "ranged", 0x403ff) == -2);
    assert(entry(&hook32_table, "grouped", 0x10203) >= 0 && entry(&hook32_table, "grouped", 0x40400) >= 0);
    assert(entry(&hook32_table, "grouped", 0x10203) != entry(&hook32_table, "grouped", 0x40400));
    assert(entry(&hook32_table, "grouped", 0x30001) == -2 && entry(&hook_table, "grouped", 0) == -1);

    for (unsigned i = 0; i < 0x40; i++) a32_slots[i] = thumb_slots[i] = own_block;
    a32_slots[24] = own_a32, thumb_slots[16] = own_thumb, a32_slots[28] = own_caller;
    g_mem = guest;
    strcpy((char*)guest + 0x1810, "SDK MW+Vendor+VendorSdk_nnSdk-1_2_3-Release");
    g_settings.native_hooks = 1;
    snprintf(g_settings.hook_families_off, sizeof g_settings.hook_families_off, "switched_off");
    hook_install();
    assert(a32_slots[0] == hook32_fn[answer]);  /* an A32 function */
    assert(thumb_slots[0] == hook32_fn[thumb]); /* a Thumb one, by its address without bit 0 */
    assert(a32_slots[4] == own_block);          /* not declared a32: its own code */
    assert(a32_slots[8] == own_block);          /* no hook names it */
    assert(a32_slots[12] == own_block);         /* its family is off */
    assert(a32_slots[16] == hook32_fn[now]);    /* the 32-bit program's declaration of a symbol with two */
    /* versioned: the hook for the SDK its module's label names, and none where the module has no label */
    assert(a32_slots[20] == hook32_fn[entry(&hook32_table, "save_j", 0x10203)]);
    assert(a32_slots[20] != hook32_fn[entry(&hook32_table, "save_j", 0x20000)]);
    assert(a32_slots[32] == hook32_fn[entry(&hook32_table, "ranged", 0x10203)]);
    assert(a32_slots[36] == hook32_fn[entry(&hook32_table, "ranged_too", 0x10203)]);
    assert(a32_slots[40] == hook32_fn[entry(&hook32_table, "grouped", 0x10203)]);
    for (unsigned i = 1; i < 0x40; i++) assert(i == 16 || thumb_slots[i] == own_block);
    /* what was left for a version: the one symbol in the module with no label, by its name, module and versions */
    hook_missed_text(text, sizeof text);
    assert(!strncmp(text, "1 of the SDK functions", 22) && strstr(text, "in this 32-bit game"));
    assert(strstr(text, "the first is save_j in main, of version unknown, declared for sdk < 2.0.0; sdk >= 2.0.0"));
    /* every declaration holds for the module with the label, and the other module's SDK is not known to be another */
    hook_unread_text(text, sizeof text);
    assert(!*text);

    /* the adapter: r0 and r1 to the handler, its answer in r0, back to lr */
    thread.cpu.mem = mem32;
    thread.arm.mem = mem32;
    thread.arm.mem_size = sizeof mem32;
    thread.arm.r[0] = 7, thread.arm.r[1] = 1, thread.arm.r[14] = 0x3000;
    a32_slots[0](&thread.arm);
    assert(seen[0] == 7 && seen[1] == 1 && thread.arm.r[0] == 0x1234 && thread.arm.r[15] == 0x3000);
    thread.arm.r[0] = 0x40, thread.arm.r[14] = 0x3001; /* back into Thumb code */
    thumb_slots[0](&thread.arm);
    assert(mem32[0x40] == 0x5a && thread.arm.r[15] == 0x3000 && (thread.arm.cpsr & SNX_ARM_T));
    /* the 32-bit program's own declaration: the result through the pointer in r0, r1 untouched */
    thread.arm.cpsr = 0;
    thread.arm.r[0] = 0x80, thread.arm.r[1] = 0xdead, thread.arm.r[14] = 0x3000;
    a32_slots[16](&thread.arm);
    assert(!memcmp(mem32 + 0x80, "\x88\x77\x66\x55\x44\x33\x22\x11", 8) && thread.arm.r[1] == 0xdead &&
           thread.arm.r[15] == 0x3000);
    /* its versioned hook's adapters: each passes its version's layout, the size_t from r1 */
    mem32[0x28] = 0x11, mem32[0x30] = 0x22;
    thread.arm.r[0] = 0x20, thread.arm.r[1] = 0xfffffff0u, thread.arm.r[14] = 0x3000;
    hook32_fn[entry(&hook32_table, "save_j", 0x10000)](&thread.arm);
    assert(thread.arm.r[0] == 0x11 && seen[0] == 0xc && seen[1] == 0xfffffff0u);
    thread.arm.r[0] = 0x20, thread.arm.r[1] = 4;
    hook32_fn[entry(&hook32_table, "save_j", 0x20000)](&thread.arm);
    assert(thread.arm.r[0] == 0x22 && seen[0] == 0x14 && seen[1] == 4);

    /* a handler that hands the call back: the block the function's slot held runs with the call's registers until
       the call's lr, an A32 function's and a Thumb one's each its own, and the adapter returns to that lr though the
       function's code changed r14 */
    assert(a32_slots[24] == hook32_fn[entry(&hook32_table, "own", 0)] && thumb_slots[16] == a32_slots[24]);
    thread.arm.cpsr = 0;
    thread.arm.r[0] = 5, thread.arm.r[14] = 0x3000, thread.arm.r[15] = 0x1060;
    a32_slots[24](&thread.arm);
    assert(runs == 1 && thread.arm.r[0] == 0x46 && thread.arm.r[15] == 0x3000 && !(thread.arm.cpsr & SNX_ARM_T));
    thread.arm.cpsr = SNX_ARM_T;
    thread.arm.r[0] = 5, thread.arm.r[14] = 0x3001, thread.arm.r[15] = 0x2020;
    thumb_slots[16](&thread.arm);
    assert(runs == 2 && thread.arm.r[0] == 0x86 && thread.arm.r[15] == 0x3000 && (thread.arm.cpsr & SNX_ARM_T));
    thread.arm.r[0] = 4,
    thread.arm.r[14] = 0x3001; /* a call the handler answers itself: the function's code doesn't run */
    thumb_slots[16](&thread.arm);
    assert(runs == 2 && thread.arm.r[0] == 0);

    /* a handler that calls a Thumb function of the game from an A32 one's hook: the words in r0 to r3, the function
       in r15 in its state, run until the handler's lr; then the thread is at the hooked function again, in its
       state, with that lr */
    thread.arm.cpsr = 0;
    thread.arm.r[0] = 0x2041, thread.arm.r[14] = 0x3000, thread.arm.r[15] = 0x1070;
    a32_slots[28](&thread.arm);
    assert(runs == 4 && called[0] == 11 && called[1] == 12 && called[2] == 13 && called[3] == 14);
    assert(called[4] == 0x2040 && called[5] == SNX_ARM_T);
    assert(thread.arm.r[0] == 0xc0 && thread.arm.r[15] == 0x3000 && !(thread.arm.cpsr & SNX_ARM_T));

    /* a game of another SDK, 3.0.1: the function declared a32 for versions under 2.0.0 keeps its own code there,
       the versioned one takes its later layout and the unversioned ones their hooks, and the text names the
       function, its module, that SDK and the versions it is declared for, with the one of the module with no label */
    for (unsigned i = 0; i < 0x40; i++) a32_slots[i] = thumb_slots[i] = own_block;
    memset(&g_host.hook, 0, sizeof g_host.hook);
    strcpy((char*)guest + 0x1810, "SDK MW+Vendor+VendorSdk_nnSdk-3_0_1-Release");
    hook_install();
    assert(a32_slots[32] == own_block && a32_slots[36] == own_block && a32_slots[40] == own_block);
    assert(a32_slots[0] == hook32_fn[answer] && a32_slots[20] == hook32_fn[entry(&hook32_table, "save_j", 0x20000)]);
    hook_missed_text(text, sizeof text);
    assert(!strncmp(text, "4 of the SDK functions", 22));
    assert(strstr(text, "the first is ranged in sdk, of version 3.0.1, declared for sdk < 2.0.0; sdk == 4.4.0"));
    hook_missed_text(text, 30); /* cut to the buffer */
    assert(strlen(text) == 29 && !strncmp(text, "4 of the SDK functions", 22));
    /* why, in the words the stop says it in: the SDK, where it is, the SDK read, the functions left out by family and
       what to do; the 32-bit declarations hold for the SDK read, so a game of that SDK or an older one says nothing */
    hook_unread_text(text, sizeof text);
    assert(!strcmp(
        text, "This 32-bit game's SDK is 3.0.1 (module sdk) and the runtime's declarations for a 32-bit game were "
              "read in SDKs 0.12.16 and 4.4.0 alone, so 3 of its SDK functions run their own code (marked 2, second "
              "1), which "
              "needs services the runtime answers only natively. A 64-bit update of the game, if it has one, "
              "plays; for this version, an SDK of 3.0.1 has to be read (issue #2242)"));
    hook_unread_text(text, 30); /* cut to the buffer */
    assert(strlen(text) == 29 && !strncmp(text, "This 32-bit game's SDK is 3.0", 29));
    /* a game of the second 32-bit SDK read, 4.4.0: each declaration's second group, and nothing left to say for the
       module with the label */
    for (unsigned i = 0; i < 0x40; i++) a32_slots[i] = thumb_slots[i] = own_block;
    memset(&g_host.hook, 0, sizeof g_host.hook);
    strcpy((char*)guest + 0x1810, "SDK MW+Vendor+VendorSdk_nnSdk-4_4_0-Release");
    hook_install();
    assert(a32_slots[32] == hook32_fn[entry(&hook32_table, "ranged", 0x40400)]);
    assert(a32_slots[40] == hook32_fn[entry(&hook32_table, "grouped", 0x40400)]);
    assert(a32_slots[36] == own_block); /* ranged_too, one group: not that SDK's */
    hook_unread_text(text, sizeof text);
    assert(strstr(text, "This 32-bit game's SDK is 4.4.0") && strstr(text, "so 1 of its SDK functions"));

    /* hooks=0 takes nothing, and leaves nothing to say */
    for (unsigned i = 0; i < 0x40; i++) a32_slots[i] = thumb_slots[i] = own_block;
    g_settings.native_hooks = 0;
    hook_install();
    assert(a32_slots[0] == own_block && thumb_slots[0] == own_block);
    hook_missed_text(text, sizeof text);
    assert(!*text);
    hook_unread_text(text, sizeof text);
    assert(!*text);
    puts("hook_install_a32: passed");
    return 0;
}
/* hook.c asks which families can run (family.c, not linked here): every one of this test's */
const char* hook_family_unready(const char* family) {
    (void)family;
    return NULL;
}
