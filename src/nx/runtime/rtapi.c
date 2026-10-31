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

/* ---- RtAPI ---- */

static void rt_note_hint(u64 pc) { hints_add(pc); }

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
        c->pc = pc;       /* where a thread a save state stopped here goes on */
        kthread_block(t); /* a game pause waits for no compile: the thread stops at the end of it */
        f = jit_wait(pc);
        kthread_unblock(t);
        if (t) core_enter(t);
        if (!f)
            fatal("The game reached code at %llx that the recompiler missed, and %s", (unsigned long long)pc,
                  jit_failure());
    }
    if (pc - g_code_lo < g_code_hi - g_code_lo) hints_add(pc);
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
