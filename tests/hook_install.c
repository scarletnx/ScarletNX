/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hook_install on a 64-bit game (src/nx/hle/hook/hook.c), over tests/hook/install.hook, with the runtime stubbed to a
   symbol list and a dispatch table: each hook takes its function's slot, a pure one of a verified family as verify
   mode's check. Then a save state loaded (state.c puts the state's g_hle in place of the running one): the functions
   hooked are still this process's, so a function's own code, the hook in its slot, its name and the slots put back
   after the JIT emptied them are what they were, whatever run saved the state. Verify mode's scratch memory is the
   state's: a state saved with none skips the check, the handler answering the call, and one saved with some checks
   on it */
#include "nx/hle/hook/hook.c"
#include <assert.h>
#include <stdio.h>

#define CODE   0x1000u
#define COPY   (CODE + 0x00)
#define ANSWER (CODE + 0x10)

/* ---- the handlers */
static unsigned copies, answers;
u64 test_copy(CPU* c, u64 dst, u64 src, u64 n) {
    (void)c, (void)src, (void)n;
    copies++;
    return dst;
}
u32 test_answer(CPU* c, u32 a) {
    (void)c;
    answers++;
    return a + 1;
}

/* ---- the game: two functions of one module, each slot holding its own code, which returns to x30 */
static unsigned own_copies, own_answers;
static void own_copy(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    own_copies++;
    c->pc = c->x[30];
}
static void own_answer(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    own_answers++;
    c->x[0] = 0x77;
    c->pc = c->x[30];
}
static GuestFn slots[0x400];
static const struct {
    const char* name;
    u64 addr;
} symbols[] = {{"copy", COPY}, {"answer", ANSWER}, {"unhooked", CODE + 0x20}};
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i >= sizeof symbols / sizeof symbols[0]) return 0;
    *addr = symbols[i].addr, *name = symbols[i].name;
    return 1;
}
static const SnxModInfo modules[1] = {{.name = "main", .base = CODE, .size = 0x1000}};
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

/* ---- the runtime, as far as hook.c reaches it */
#define SCRATCH 0x80000000ull
static unsigned allocations;
u64 kmem_alloc(u64 size, u32 state, u32 perm) {
    (void)size, (void)state, (void)perm;
    allocations++;
    return SCRATCH;
}
void mem_query(u64 addr, MemBlock* b) {
    memset(b, 0, sizeof *b);
    b->addr = addr, b->state = MS_CODE, b->perm = PERM_R | PERM_X;
}
GuestFn nce_slot(GuestFn f) { return f; }
void nce_slot_taken(u64 addr, const char* what) { (void)addr, (void)what; }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn; }
GuestFn jit_code_now(u64 pc) { return (void)pc, NULL; }
int linker_links(void) { return 0; }
/* verify.c's: counted, with the scratch slot it was given; the handler answers, as an agreeing check leaves it */
static unsigned checks;
static u64 check_scratch;
int hook_verify_call(CPU* c, u64 entry, GuestFn original, GuestFn handler, const HookVerify* v, u64 scratch,
                     u64 scratch_size, HookVerifyResult* r) {
    (void)v, (void)scratch_size, (void)r;
    assert(original == own_copy);
    checks++;
    check_scratch = scratch;
    handler(c, entry);
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
    fprintf(stderr, "fatal: %s\n", fmt);
    exit(1);
}

static int entry_of(const char* symbol) {
    int i;
    for (i = 0; i < HOOK_COUNT; i++)
        if (!strcmp(hook_symbol[i], symbol)) return i;
    assert(!"no such hook");
    return -1;
}

int main(void) {
    static CPU c;
    const char* handler = NULL;
    int answer = entry_of("answer");
    unsigned i;
    for (i = 0; i < sizeof slots / sizeof slots[0]; i++) slots[i] = own_answer;
    slots[(COPY - CODE) >> 2] = own_copy;
    c.mem = guest;
    c.disp = slots; /* the dispatch loop's table, for a function's own code that leaves for other code */
    g_settings.native_hooks = 1;
    snprintf(g_settings.hook_families_verified, sizeof g_settings.hook_families_verified, "pure");
    hook_install();
    assert(slots[(COPY - CODE) >> 2] == verify_fn && slots[(ANSWER - CODE) >> 2] == hook_fn[answer]);
    assert(slots[(CODE + 0x20 - CODE) >> 2] == own_answer); /* no hook names it */
    assert(allocations == 1 && g_hle.hook.verify_base == SCRATCH);
    assert(hook_original(COPY) == own_copy && hook_original(ANSWER) == own_answer);
    /* a call checked on the scratch memory this run made */
    c.x[30] = 0x9000;
    slots[(COPY - CODE) >> 2](&c, COPY);
    assert(checks == 1 && check_scratch == SCRATCH && copies == 1);
    /* the plain hook's slot runs its handler alone */
    c.x[0] = 7, c.x[30] = 0x9000;
    slots[(ANSWER - CODE) >> 2](&c, ANSWER);
    assert(answers == 1 && own_answers == 0 && c.x[0] == 8 && c.pc == 0x9000 && checks == 1);

    /* a save state loaded: g_hle is the state's whole, here one of a run that hooked and verified nothing */
    memset(&g_hle, 0, sizeof g_hle);
    assert(hook_original(COPY) == own_copy && hook_original(ANSWER) == own_answer);
    assert(hook_function(COPY) == verify_fn && hook_function(ANSWER) == hook_fn[answer]);
    assert(!strcmp(hook_at(ANSWER, &handler), "answer") && !strcmp(handler, "test_answer"));
    assert(hook_site("copy") == COPY && hook_function(CODE + 0x20) == NULL);
    /* the JIT emptied the range's slots: the hooks go back in theirs */
    slots[(COPY - CODE) >> 2] = slots[(ANSWER - CODE) >> 2] = NULL;
    hook_restore(CODE, 0x1000);
    assert(slots[(COPY - CODE) >> 2] == verify_fn && slots[(ANSWER - CODE) >> 2] == hook_fn[answer]);
    /* a handler that hands its call back runs the function's own code to the call's return */
    c.x[30] = 0x9000, c.pc = 0;
    hook_run_original(&c, ANSWER);
    assert(own_answers == 1 && c.x[0] == 0x77 && c.pc == 0x9000);
    /* the state has no scratch memory: the check is skipped and the handler answers */
    c.x[0] = 0x40, c.x[30] = 0x9100;
    slots[(COPY - CODE) >> 2](&c, COPY);
    assert(checks == 1 && copies == 2 && own_copies == 0 && c.x[0] == 0x40 && c.pc == 0x9100);
    assert(g_host.hook.skipped[0] + g_host.hook.skipped[1] == 1);
    /* a state saved by a run that verified holds its scratch memory, at its base */
    g_hle.hook.verify_base = 0x90000000ull;
    slots[(COPY - CODE) >> 2](&c, COPY);
    assert(checks == 2 && check_scratch == 0x90000000ull && copies == 3);
    assert(allocations == 1);
    puts("hook_install: passed");
    return 0;
}
