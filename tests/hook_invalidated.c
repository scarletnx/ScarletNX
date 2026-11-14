/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A native hook the game invalidates (src/nx/hle/hook/hook.c, jump.c), on this test's own x86_64 code, over
   tests/hook/install.hook with the runtime stubbed as tests/hook_install.c stubs it. A mod's hook writes a branch over
   the start of a hooked function: the native hook's adapter is rewritten into a jump to hook_invalidated, so a copy
   of its address taken before (as an NRO's import or the JIT's direct call holds one) runs the game's new code with
   the arguments of the call, its slot is left for the game's code, and a warning names it with both words. A change
   of a neighbour's code in the same cache line, which the JIT drops the same way, invalidates nothing, nor does a
   change past the function's first 4 bytes, which are all that is compared. Then the mod's trampoline, the
   function's first instructions and a jump back past them, takes answer's ext_skyline hook (tests/hook/
   ext_skyline.hooks) when it first runs, in each form a loader writes: the mod's call of the original runs the
   handler, and a handler's own code is the trampoline's, entered there. The system's refusal to make code writable is
   reported with its reason. Only x86_64 runs the jump: on another host the test is skipped (77) */
#include "nx/hle/hook/hook.c"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

#define CODE     0x1000u
#define COPY     (CODE + 0x00)
#define ANSWER   (CODE + 0x10)
#define UNHOOKED (CODE + 0x20)
#define TRAMP    (CODE + 0x200) /* the mod's trampolines, in a pool of its code */

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

/* ---- the game: two hooked functions of 16 bytes and an unhooked one, in one cache line. Each slot holds the
   function's own code at first; game_answer is the JIT's code of what the game put over answer's start */
static unsigned own_runs, game_runs;
static void own_code(CPU* RESTRICT c, u64 entry) {
    (void)entry;
    own_runs++;
    c->pc = c->x[30];
}
static u64 game_entry;
static void game_answer(CPU* RESTRICT c, u64 entry) {
    game_runs++;
    game_entry = entry;
    c->x[0] = 0x55;
    c->pc = c->x[30];
}
/* the JIT's code of the trampoline: the SDK's own answer, entered at the trampoline */
static unsigned tramp_runs;
static u64 tramp_entry;
static void tramp_code(CPU* RESTRICT c, u64 entry) {
    tramp_runs++;
    tramp_entry = entry;
    c->x[0] = 0x66;
    c->pc = c->x[30];
}
static GuestFn slots[0x400];
static const struct {
    const char* name;
    u64 addr;
} symbols[] = {{"copy", COPY}, {"answer", ANSWER}, {"unhooked", UNHOOKED}};
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
u64 kmem_alloc(u64 size, u32 state, u32 perm) { return (void)size, (void)state, (void)perm, 0; }
void mem_query(u64 addr, MemBlock* b) {
    memset(b, 0, sizeof *b);
    b->addr = CODE, b->size = 0x1000, b->state = MS_CODE, b->perm = PERM_R | PERM_X;
    if (addr < CODE || addr >= CODE + 0x1000) b->addr = addr, b->size = 0, b->state = MS_FREE;
}
GuestFn nce_slot(GuestFn f) { return f; }
void nce_slot_taken(u64 addr, const char* what) { (void)addr, (void)what; }
void jit_slot_taken(u64 addr, GuestFn fn) { (void)addr, (void)fn; }
GuestFn jit_code_now(u64 pc) { return pc == ANSWER ? game_answer : pc == TRAMP ? tramp_code : NULL; }
SnxArmBlock* arm_slot(u32 addr, int thumb) { return (void)addr, (void)thumb, NULL; }
SnxArmBlock arm_block(u32 addr, int thumb) { return (void)addr, (void)thumb, NULL; }
SnxArmCPU* arm_cpu(CPU* c) { return (void)c, NULL; }
void arm_run(SnxArmCPU* c, SnxArmBlock first, u32 stop_at) { (void)c, (void)first, (void)stop_at; }
int linker_links(void) { return 0; }
int hook_verify_call(CPU* c, u64 entry, GuestFn original, GuestFn handler, const HookVerify* v, u64 scratch,
                     u64 scratch_size, HookVerifyResult* r) {
    (void)c, (void)entry, (void)original, (void)handler, (void)v, (void)scratch, (void)scratch_size, (void)r;
    return HOOK_SKIP_SIZE;
}
const char* hook_verify_text(int what) { return (void)what, ""; }
size_t snx_hook_route_count(void) { return 0; }
SnxU64 snx_hook_route_hash(const char* name) { return (void)name, 0; }
SnxU64 snx_hook_route_list_hash(void) { return 0; }
int snx_hook_key_at(const char* name, const unsigned char build_id[32], SnxU64* offset) {
    return (void)name, (void)build_id, (void)offset, 0;
}
/* the warnings, one after the other */
static char warned[4096];
static unsigned warnings;
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    size_t used = strlen(warned);
    if (lvl) return;
    warnings++;
    va_start(ap, fmt);
    vsnprintf(warned + used, sizeof warned - used, fmt, ap);
    va_end(ap);
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static int entry_of(const char* symbol) {
    int i;
    for (i = 0; i < HOOK_COUNT; i++)
        if (!strcmp(hook_symbol[i], symbol)) return i;
    assert(!"no such hook");
    return -1;
}
/* the JIT dropped the line at addr: its slots are empty, and the hooks are put back */
static void line_dropped(u64 addr) {
    u64 a;
    for (a = addr & ~(u64)63; a < (addr & ~(u64)63) + 64; a += 4) slots[(a - CODE) >> 2] = NULL;
    hook_restore(addr & ~(u64)63, 64);
}

/* the host whose jump the test runs; every host compiles the whole test, so that what only it reads is read there */
#if defined(__x86_64__) || defined(_M_X64)
#define JUMP_RUNS 1
#else
#define JUMP_RUNS 0
#endif

int main(int argc, char** argv) {
    static CPU c;
    int answer, copy;
    /* read at each call as an import table is: an optimizer that knew the address would inline the adapter's code as
       it was before the rewrite, which no call of the game's can */
    GuestFn volatile held;
    const u8* adapter;
    unsigned i;
    (void)argc;
    if (!JUMP_RUNS) {
        printf("hook_invalidated: skipped: the jump is proved on x86_64 alone, and this host is %s\n",
               snx_hook_jump_host() ? snx_hook_jump_host() : "of no architecture with a jump");
        return 77;
    }
    answer = entry_of("answer"), copy = entry_of("copy");
    for (i = 0; i < sizeof slots / sizeof slots[0]; i++) slots[i] = own_code;
    for (i = 0; i < 0x30; i++) guest[CODE + i] = (u8)(0xA0 + i); /* the three functions' code */
    c.mem = guest;
    c.disp = slots;
    g_settings.native_hooks = 1;
    hook_install();
    assert(slots[(ANSWER - CODE) >> 2] == hook_fn[answer] && slots[(COPY - CODE) >> 2] == hook_fn[copy]);
    assert(snx_le32(g_host.hook.head[1][0]) == 0xB3B2B1B0u && snx_le32(g_host.hook.head[1][4]) == 0xC3C2C1C0u);
    /* the address of answer's hook, held as an NRO's import is: its handler answers */
    held = slots[(ANSWER - CODE) >> 2];
    c.x[0] = 7, c.x[30] = 0x9000;
    held(&c, ANSWER);
    assert(answers == 1 && c.x[0] == 8 && c.pc == 0x9000);

    /* the game changes the unhooked neighbour's code in the line: both hooks go back, nothing invalidated */
    guest[UNHOOKED] ^= 0xFF;
    line_dropped(UNHOOKED);
    assert(slots[(ANSWER - CODE) >> 2] == hook_fn[answer] && slots[(COPY - CODE) >> 2] == hook_fn[copy]);
    assert(warnings == 0 && !g_host.hook.invalid[0] && !g_host.hook.invalid[1]);

    /* a change past answer's first 4 bytes is not looked at, by design: a mod's hook writes over them */
    guest[ANSWER + 4] ^= 0xFF;
    line_dropped(ANSWER);
    assert(slots[(ANSWER - CODE) >> 2] == hook_fn[answer] && warnings == 0 && !g_host.hook.invalid[1]);

    /* a mod's hook: a branch over answer's first word. Its hook is off: its slot is left for the game's code, and the
       warning names the function, its address and the handler that answered it */
    snx_st_le32(guest + ANSWER, 0x14001234u);
    line_dropped(ANSWER);
    assert(slots[(ANSWER - CODE) >> 2] == NULL && hook_function(ANSWER) == NULL);
    assert(slots[(COPY - CODE) >> 2] == hook_fn[copy] && hook_function(COPY) == hook_fn[copy]);
    assert(warnings == 1 &&
           strstr(warned, "changed the first instruction of answer at 1010 from b3b2b1b0 to 14001234") &&
           strstr(warned, "which test_answer answered natively") &&
           strstr(warned, "but for the mod's call of the original, which runs test_answer once its trampoline runs"));
    /* the adapter's own code now jumps to the sad path, after its landing pad when it has one */
    adapter = (const u8*)(uintptr_t)hook_fn[answer];
    if (!memcmp(adapter, "\xF3\x0F\x1E\xFA", 4)) adapter += 4;
    assert(adapter[0] == 0xE9 || (adapter[0] == 0x49 && adapter[1] == 0xBB));
    /* the held address runs the game's code, with the call's arguments, and the handler no more */
    c.x[0] = 7, c.x[30] = 0x9100, c.pc = 0;
    held(&c, ANSWER);
    assert(game_runs == 1 && game_entry == ANSWER && answers == 1 && c.x[0] == 0x55 && c.pc == 0x9100);
    /* the other hook still answers, and dropping the line again says nothing more */
    c.x[30] = 0x9200;
    slots[(COPY - CODE) >> 2](&c, COPY);
    assert(copies == 1 && own_runs == 0 && c.pc == 0x9200);
    line_dropped(ANSWER);
    assert(slots[(ANSWER - CODE) >> 2] == NULL && warnings == 1);

    /* the forms of a trampoline, answer's first 1, 4 or 5 words and a jump back past them: a b, or ldr x17, #8 and
       br x17 of the address, after a nop that aligns it. Its words are the ones hook_install saw */
    {
        static const struct {
            unsigned k, nop, literal; /* "far" is a macro of windows.h */
        } forms[] = {{1, 0, 0}, {4, 0, 0}, {5, 0, 0}, {1, 0, 1}, {1, 1, 1}, {4, 1, 1}};
        unsigned f, w;
        for (f = 0; f < sizeof forms / sizeof forms[0]; f++) {
            u64 at = TRAMP + 4 * forms[f].k, back = ANSWER + 4 * forms[f].k;
            memset(guest + TRAMP, 0, 0x40);
            for (w = 0; w < forms[f].k; w++) memcpy(guest + TRAMP + 4 * w, &g_host.hook.head[1][w], 4);
            if (forms[f].nop) snx_st_le32(guest + at, 0xD503201Fu), at += 4;
            if (!forms[f].literal)
                snx_st_le32(guest + at, 0x14000000u | ((u32)((i64)(back - at) >> 2) & 0x3FFFFFFu));
            else
                snx_st_le32(guest + at, 0x58000051u), snx_st_le32(guest + at + 4, 0xD61F0220u),
                    snx_st_le64(guest + at + 8, back);
            assert(trampoline_of(1, TRAMP, CODE + 0x1000));
            assert(!trampoline_of(0, TRAMP, CODE + 0x1000)); /* not copy's */
            /* the jump back right but a moved instruction another: a first instruction that read the pc */
            guest[TRAMP + 4 * (forms[f].k - 1)] ^= 0xFF;
            assert(!trampoline_of(1, TRAMP, CODE + 0x1000));
            guest[TRAMP + 4 * (forms[f].k - 1)] ^= 0xFF;
            snx_st_le32(guest + at, 0xD503201Fu); /* no jump back where it should be */
            assert(!trampoline_of(1, TRAMP, CODE + 0x1000));
        }
    }
    /* the near form, as exlaunch writes it for a function within a b's reach: a pc that is no trampoline is no hook's */
    memset(guest + TRAMP, 0, 0x40);
    memcpy(guest + TRAMP, &g_host.hook.head[1][0], 4);
    snx_st_le32(guest + TRAMP + 4, 0x14000000u | ((u32)((i64)((ANSWER + 4) - (TRAMP + 4)) >> 2) & 0x3FFFFFFu));
    assert(hook_trampoline(TRAMP + 0x100) == NULL && hook_trampoline(TRAMP + 2) == NULL);
    /* its first run takes answer's ext_skyline hook, which takes its slot: the mod's call of the original runs the
       handler with its arguments */
    {
        const char* handler = NULL;
        GuestFn ext = hook_ext_skyline("answer", 0, &handler), got = hook_trampoline(TRAMP);
        assert(ext && got == ext && !strcmp(handler, "test_answer") && slots[(TRAMP - CODE) >> 2] == ext);
        assert(g_host.hook.tramp[1] == TRAMP && warnings == 1);
        c.x[0] = 41, c.x[30] = 0x9300, c.pc = 0;
        slots[(TRAMP - CODE) >> 2](&c, TRAMP);
        assert(answers == 2 && c.x[0] == 42 && c.pc == 0x9300);
        /* the handler's own code is the trampoline's, entered there: the SDK's, past the instruction the mod moved */
        c.x[30] = 0x9400, c.pc = 0;
        hook_run_original(&c, ANSWER);
        assert(tramp_runs == 1 && tramp_entry == TRAMP && c.x[0] == 0x66 && c.pc == 0x9400);
        /* the held adapter still runs the game's code at answer: the mod's detour */
        c.x[30] = 0x9500, c.pc = 0;
        held(&c, ANSWER);
        assert(game_runs == 2 && game_entry == ANSWER && answers == 2 && c.pc == 0x9500);
        /* the JIT dropped the trampoline's line: its next run takes the hook again */
        slots[(TRAMP - CODE) >> 2] = NULL;
        assert(hook_trampoline(TRAMP) == ext && slots[(TRAMP - CODE) >> 2] == ext);
        /* the mod rewrote it: no longer a trampoline, so no longer the hook's */
        snx_st_le32(guest + TRAMP + 4, 0xD503201Fu);
        assert(hook_trampoline(TRAMP) == NULL && g_host.hook.tramp[1] == 0);
    }

#if !defined(_WIN32)
    /* code the system refuses to make writable (a shared mapping of a file opened to read): nothing is written, and
       the reason comes back with its values */
    {
        char why[320];
        int fd = open(argv[0], O_RDONLY); /* this test's own file */
        void* p = fd < 0 ? MAP_FAILED : mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
        u8 before[16];
        assert(p != MAP_FAILED);
        memcpy(before, p, sizeof before);
        assert(snx_hook_jump_patch((GuestFn)(uintptr_t)p, hook_invalidated, why, sizeof why) == -1);
        assert(strstr(why, "mprotect of 4096 bytes at ") && strstr(why, "to read, write and execute was refused: "));
        assert(!memcmp(before, p, sizeof before));
        printf("hook_invalidated: refused as it should be: %s\n", why);
        munmap(p, 4096);
        close(fd);
    }
#else
    (void)argv;
#endif
    printf("hook_invalidated: passed, the warning: %s", warned);
    return 0;
}
