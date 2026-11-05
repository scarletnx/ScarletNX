/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nce_module (src/nx/runtime/nce/nce.c), the game's modules as the runtime runs them. Under NCE on Android the guest's
   layout moves up (mem_moved), and the modules' addresses with it: the copy it returns has them moved, and the main
   thread's entry it gives is the first module's text there, never its address before the move, where nothing is
   mapped. Without a move the module and its entry stay as they are. And the kicks' look at the guest's addresses
   (nce_foreign_look, test_foreign_look). Any host: those parts of nce.c are host-independent */
#include "nx/loader/module_abi.h"
#include "nx/memory/mem.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/platform/log.h"
#include "snx_settings.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* mem.c's look at what is not the game's in its addresses, for nce_foreign_look: the answer the test sets (n things
   found, named found; -1 a failed read), and how many times it was asked */
static struct {
    const char* found;
    int n, calls;
} g_foreign;
int mem_foreign(char* out, size_t size) {
    g_foreign.calls++;
    snprintf(out, size, "%s", g_foreign.n > 0 ? g_foreign.found : "");
    if (g_foreign.n < 0) errno = EIO;
    return g_foreign.n;
}
/* the log's lines since the test last cleared it, as far as they fit, and printed */
static char g_said[4096];
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    size_t at = strlen(g_said);
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_said + at, sizeof g_said - at, fmt, ap);
    va_end(ap);
    fputs(g_said + at, stdout);
}
/* how many times the log said what since it was cleared */
static int said(const char* what) {
    int n = 0;
    for (const char* p = g_said; (p = strstr(p, what)); p += strlen(what)) n++;
    return n;
}
/* the rounds from to to of the kicks' thread: how many of them looked */
static int looks(u64 from, u64 to) {
    int n = 0;
    for (u64 r = from; r <= to; r++) n += nce_foreign_look(r);
    return n;
}

/* The kicks' thread's look at the guest's addresses while the game runs (nce_foreign_scan): with the setting 0 a round
   asks nothing; with 1, one round in NCE_FOREIGN_EVERY (10 s of them) asks mem_foreign, a finding is said once while it
   stays, a new one said, one gone and back said again, and a failed read said once */
static void test_foreign_look(void) {
    const u64 every = NCE_FOREIGN_EVERY;
    static const char jit[] = "mapped at 0x1000000000-0x1000002000 r-xs /memfd:jit-cache (deleted)";
    static const char more[] = "mapped at 0x1000000000-0x1000002000 r-xs /memfd:jit-cache (deleted); nothing mapped "
                               "at 0x1000010000-0x1000012000";
    check(every == 5000, "a look every 10 s of 2 ms rounds");
    g_settings.nce_foreign_scan = 0;
    g_foreign.found = jit, g_foreign.n = 1;
    check(looks(1, 3 * every) == 0 && g_foreign.calls == 0, "nce_foreign_scan=0: no round looks");
    g_settings.nce_foreign_scan = 1;
    g_said[0] = 0;
    check(looks(1, every - 1) == 0 && g_foreign.calls == 0, "nce_foreign_scan=1: no look before 10 s of rounds");
    check(looks(every, 3 * every) == 3 && g_foreign.calls == 3, "nce_foreign_scan=1: a look every 10 s of rounds");
    check(said("nce: while the game runs, 1 thing of the host's process lies in the guest's addresses") == 1 &&
              said(jit) == 1,
          "a finding said once while it stays");
    g_foreign.found = more, g_foreign.n = 2;
    check(looks(4 * every, 4 * every) == 1 && said("2 things of the host's process lie") == 1 && said(more) == 1,
          "a new finding said");
    g_foreign.n = 0;
    check(looks(5 * every, 5 * every) == 1 && said("while the game runs") == 2, "nothing found says nothing");
    g_foreign.found = jit, g_foreign.n = 1;
    check(looks(6 * every, 6 * every) == 1 && said("while the game runs, 1 thing") == 2,
          "a finding gone and back said again");
    g_foreign.n = -1;
    check(looks(7 * every, 8 * every) == 2 && said("cannot be read (") == 1, "a failed read said once");
    g_settings.nce_foreign_scan = 0;
    g_foreign.calls = 0;
    check(looks(9 * every, 10 * every) == 0 && g_foreign.calls == 0, "nce_foreign_scan=0 again: no round looks");
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
    test_foreign_look();
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    puts("nce_module: ok");
    return 0;
}
