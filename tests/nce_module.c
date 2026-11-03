/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nce_module (src/nx/runtime/nce/nce.c), the game's modules as the runtime runs them. Under NCE on Android the guest's
   layout moves up (mem_moved), and the modules' addresses with it: the copy it returns has them moved, and the main
   thread's entry it gives is the first module's text there, never its address before the move, where nothing is
   mapped. Without a move the module and its entry stay as they are. Any host: nce.c's part that moves the module is
   host-independent */
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/nce/nce_host.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#if SNX_NCE_HOST
/* nce.c compiles the host's NCE code on this host too (AArch64 Linux), and the rest of the runtime is not linked here:
   what it reads of it, as nothing on these paths runs in this test */
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/guest_stats.h"
#include "nx/runtime/main.h"
char g_datadir[1024], g_exename[256], g_exedir[1024];
MemLayout g_layout;
Hle g_hle;
const SnxArmModule* g_arm_mod;
int mem_identity(void) { return 0; }
void guest_stats_add(u32 ix, unsigned what) {
    (void)ix;
    (void)what;
}
KThread* kthread_cur(void) { return NULL; }
int nce_host_check(char* why, size_t size) {
    (void)why;
    (void)size;
    return -1;
}
int nce_host_attach(CPU* c, char* why, size_t size) {
    (void)c;
    (void)why;
    (void)size;
    return -1;
}
int nce_host_tid(void) { return -1; }
void nce_host_kick(int tid) { (void)tid; }
void nce_host_run(CPU* c) { (void)c; }
int nce_host_context(const void* context, CPU* c) {
    (void)context;
    (void)c;
    return 0;
}
#endif

static int failures;
static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/* mem.c's: how far the layout moved */
static u64 g_moved;
u64 mem_moved(void) { return g_moved; }
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    exit(2);
}

int main(void) {
    static SnxModInfo mods[2];
    static u64 entry_addr[2] = {0x8004000ull, 0x8100000ull}, fn_lo[2] = {0x8004000ull, 0x8100000ull};
    SnxModule m = {0};
    const SnxModule* run;
    u64 entry = 0;
    const u64 by = 0x1000000000ull; /* 64 GiB: where NCE on Android puts the guest's layout */
    mods[0].base = 0x8000000ull;    /* rtld */
    mods[0].text_off = 0x4000u;
    mods[1].base = 0x8046000ull; /* main */
    mods[1].text_off = 0x4000u;
    m.mods = mods;
    m.nmods = 2;
    m.entry_addr = entry_addr;
    m.nentries = 2;
    m.fn_lo = fn_lo;
    m.nfns = 2;
    m.code_lo = 0x8004000ull;
    m.code_hi = 0x8200000ull;

    run = nce_module(&m, &entry);
    check(run == &m && entry == 0x8004000ull, "no move: the module as it is, its entry at its first text");

    g_moved = by;
    run = nce_module(&m, &entry);
    check(run != &m && run->nmods == 2 && run->mods[0].base == by + 0x8000000ull &&
              run->mods[1].base == by + 0x8046000ull,
          "a move: a copy with each module's base moved");
    check(run->entry_addr[0] == by + 0x8004000ull && run->fn_lo[1] == by + 0x8100000ull &&
              run->code_lo == by + 0x8004000ull && run->code_hi == by + 0x8200000ull,
          "its entries, functions and code range moved with it");
    check(entry == by + 0x8004000ull && entry == run->mods[0].base + run->mods[0].text_off,
          "the main thread starts at the first module's text where it moved, not where it was");
    check(m.mods[0].base == 0x8000000ull && m.entry_addr[0] == 0x8004000ull, "the module it was given unchanged");
    free((SnxModule*)run); /* the moved copy is one block, the runtime's for the whole run */
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("nce_module: ok");
    return 0;
}
