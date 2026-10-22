/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What generated code calls in the runtime (RtAPI), and the hints those calls record */
#include "nx/runtime/rtapi.h"
#include "common/utilities/endian.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- hints: a set of addresses, and the order they were found in for the file ---- */

/* One block: cap slots of the open-addressing set (0 is an empty slot, and never a hint), then the found order, which
   holds at most cap / 2 since the set is kept at most half full. Under g_hints_lock */
typedef struct {
    u64* slot;
    u64 cap, n;
    int dirty; /* added to since the file was last written */
} Hints;
static Hints g_hints;
static PMutex g_hints_lock = PMUTEX_INIT;
static char g_hints_path[1400];

static u64* hints_order(void) { return g_hints.slot + g_hints.cap; }
static u64 hint_hash(u64 pc, u64 cap) {
    unsigned bits = 0;
    while (((u64)1 << bits) < cap) bits++;
    return ((pc >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - bits); /* Fibonacci hashing: cap is a power of two */
}
/* 1 when pc was put in, 0 when it was there already */
static int set_put_nl(u64* slot, u64 cap, u64 pc) {
    u64 i = hint_hash(pc, cap);
    while (slot[i]) {
        if (slot[i] == pc) return 0;
        i = (i + 1) & (cap - 1);
    }
    slot[i] = pc;
    return 1;
}
static int grow_nl(void) {
    u64 cap = g_hints.cap ? g_hints.cap * 2 : 1024, i;
    u64 *block, *order;
    if (cap > SIZE_MAX / sizeof(u64) / 2) return -1;
    block = (u64*)calloc((size_t)(cap + cap / 2), sizeof(u64));
    if (!block) return -1;
    order = block + cap;
    for (i = 0; i < g_hints.n; i++) {
        order[i] = hints_order()[i];
        set_put_nl(block, cap, order[i]);
    }
    free(g_hints.slot);
    g_hints.slot = block;
    g_hints.cap = cap;
    return 0;
}
/* 1 when pc is new */
static int hint_add_nl(u64 pc) {
    if (!pc) return 0;
    if ((g_hints.n + 1) * 2 > g_hints.cap && grow_nl()) {
        WARN_LIMIT(1, "hints: out of memory, the hints file misses some addresses\n");
        return 0;
    }
    if (!set_put_nl(g_hints.slot, g_hints.cap, pc)) return 0;
    hints_order()[g_hints.n++] = pc;
    return 1;
}

void hints_init(void) { snprintf(g_hints_path, sizeof g_hints_path, "%s%s.hints", g_exedir, g_exename); }

void load_hints(void) {
    FILE* f = fopen(g_hints_path, "r");
    char line[64];
    if (!f) return;
    pm_lock(&g_hints_lock);
    while (fgets(line, sizeof line, f)) {
        char* end;
        unsigned long long pc = strtoull(line, &end, 16);
        if (end != line) hint_add_nl((u64)pc);
    }
    pm_unlock(&g_hints_lock);
    fclose(f);
}

/* the JIT's warm start (guest_jit.c) */
u64* hints_copy(size_t* count) {
    u64* copy;
    pm_lock(&g_hints_lock);
    copy = g_hints.n ? malloc(8 * (size_t)g_hints.n) : NULL;
    *count = copy ? (size_t)g_hints.n : 0;
    if (copy) memcpy(copy, hints_order(), 8 * (size_t)g_hints.n);
    pm_unlock(&g_hints_lock);
    return copy;
}

/* Under the lock for the whole write: the main loop and a thread stopping the game may save at once, and neither may
   truncate the file under the other. Written beside it, then moved over it */
void save_hints(void) {
    char tmp[sizeof g_hints_path + 8];
    FILE* f;
    u64 i;
    int failed;
    if (!g_hints_path[0]) return;
    pm_lock(&g_hints_lock);
    if (!g_hints.dirty) {
        pm_unlock(&g_hints_lock);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", g_hints_path);
    f = fopen(tmp, "w");
    failed = !f;
    for (i = 0; f && i < g_hints.n; i++)
        if (fprintf(f, "%llx\n", (unsigned long long)hints_order()[i]) < 0) failed = 1;
    if (f && fclose(f)) failed = 1;
    if (!failed && plat_replace_file(tmp, g_hints_path)) failed = 1;
    if (failed) {
        remove(tmp);
        WARN_LIMIT(1, "hints: could not write %s\n", g_hints_path);
    } else
        g_hints.dirty = 0;
    pm_unlock(&g_hints_lock);
}

/* ---- RtAPI ---- */

/* 1 when pc is a new hint */
static int hint_add(u64 pc) {
    int added;
    pm_lock(&g_hints_lock);
    added = hint_add_nl(pc);
    if (added) g_hints.dirty = 1;
    pm_unlock(&g_hints_lock);
    return added;
}
static void rt_note_hint(u64 pc) { hint_add(pc); }

/* the game cannot go on past either: what is needed to look into it, then the end */
static void rt_undef(CPU* c, u64 pc, u32 w) {
    /* the code was compiled from a word memory no longer holds: written since, through a view of the code (a mod's
       hook trampoline, in a pool that was zeros when the game was recompiled). What was compiled from the page is
       dropped, and the thread's loop, which runs c->pc (pc) once the callers return, compiles what is there now */
    u32 now;
    memcpy(&now, GP(pc), 4);
    if (snx_le32(now) != w) {
        LOG_LIMIT(16, "jit: code at %llx changed since it was compiled (%08x, was %08x): compiled again\n",
                  (unsigned long long)pc, snx_le32(now), w);
        jit_code_invalidate(pc & ~0xFFFull, 0x1000);
        return;
    }
    WARN("\n*** The game ran instruction %08x at %llx, which the recompiler cannot run ***\n", w,
         (unsigned long long)pc);
    kernel_dump_regs(c);
    save_hints();
    fatal("The game ran an instruction the recompiler cannot run (%08x at %llx). See the .log for details.", w,
          (unsigned long long)pc);
}
static void rt_brk(CPU* c, u64 pc, u32 imm) {
    WARN("\n*** The game stopped at BRK #%x at %llx ***\n", imm, (unsigned long long)pc);
    kernel_dump_regs(c);
    save_hints();
    fatal("The game stopped at a breakpoint (BRK #%x at %llx). See the .log for details.", imm,
          (unsigned long long)pc);
}

/* Guest floating point runs in the helpers, which read FPCR themselves: the host's FP environment stays as it is */
void snx_fpcr_apply(CPU* c) { (void)c; }

/* No thread sleeps in WFE (rt_wfe comes back at once), so SEV has nobody to wake */
static void rt_sev(void) {}
/* A spin on WFE waits for another thread to write memory: one that may be waiting for this thread's core */
static void rt_wfe(CPU* c) {
    KThread* t = (KThread*)c->thread;
    core_leave(t);
    plat_yield();
    core_enter(t);
}
static void rt_poll(CPU* c) { core_poll((KThread*)c->thread); }

/* snx_lookup found no function for pc: first the AOT module's variant table, which the build linked in from a profile
   run's cache, then the JIT for the functions the pack does not cover. A JIT compile may still be running: wait for it
   off the guest core, so the core's other threads run meanwhile. It is noted as a hint, so the next AOT build can put
   the function in the pack: not code the game mapped, which the pack never holds and which lands elsewhere next time */
static void rt_miss(CPU* c, u64 pc) {
    GuestFn f = jit_resolve_or_add(pc);
    if (!f) {
        KThread* t = kthread_cur();
        if (t) core_leave(t);
        kthread_block(t); /* a game pause waits for no compile: the thread stops at the end of it */
        f = jit_wait(pc);
        kthread_unblock(t);
        if (t) core_enter(t);
        if (!f)
            fatal("The game reached code at %llx that the recompiler missed, and %s", (unsigned long long)pc,
                  jit_failure());
    }
    if (pc - g_code_lo < g_code_hi - g_code_lo) hint_add(pc);
    f(c, pc);
}
/* IC IVAU: what was compiled from the line goes, AOT functions and JIT code alike (guest_jit.c), when the line is
   one a view of code changed (named through the view or in the code), or one no view knows */
static void rt_code_changed(CPU* c, u64 line) {
    u64 code;
    (void)c;
    if (svc_code_line(line, &code)) jit_code_changed(code, 64);
}
const RtAPI g_rtapi = {snx_svc, rt_undef, rt_note_hint, guest_ticks, rt_brk,             snx_fpcr_apply, rt_sev,
                       rt_wfe,  rt_poll,  g_svc64,      rt_miss,     snx_runtime_cas128, rt_code_changed};
