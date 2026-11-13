/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* rt_miss (src/nx/runtime/rtapi.c), RtAPI.miss: what a pc that snx_lookup finds no code for runs. Under NCE the game's
   code runs natively, no slot holds it and no JIT runs, so the runtime's own calls into it (an NVN command buffer's
   memory callback, run as nvn_cmd.c's guest_call runs it) take the native code that an empty slot means (nce_slot),
   and return to the caller's link register. Without NCE the JIT compiles the function and runs it. The rest of the
   runtime is faked here: the JIT, the kernel's core and block calls, and NCE's runs of native code */
#include "nx/runtime/rtapi.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/memory/mem.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/guest_jit.h"
#include "nx/runtime/guest_stats.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/nce/nce.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
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

#define CALLBACK_PC 0x8000100ull /* the game's function the runtime calls: no slot holds it */
#define RETURN_PC   0x8000200ull /* the NVN call's return address, where the callback returns */

/* what ran: the native code (its pc and the link register it returned to), the JIT's code, the JIT's lookups */
static struct {
    int nce, native, jitted, asked;
    u64 native_pc, native_lr;
} g_t;

/* nce.c's run of the native code at entry: back to the caller's link register, as nce_native returns */
static void native(CPU* c, u64 entry) {
    g_t.native++;
    g_t.native_pc = entry;
    g_t.native_lr = c->x[30];
    c->pc = c->x[30];
}
GuestFn nce_slot(GuestFn f) { return f || !g_t.nce ? f : native; }

/* the JIT: code for any pc while NCE is off, none under NCE (main.c starts no JIT then) */
static void jitted(CPU* c, u64 pc) {
    (void)pc;
    g_t.jitted++;
    c->pc = c->x[30];
}
GuestFn jit_resolve_or_add(u64 pc) {
    (void)pc;
    g_t.asked++;
    return g_t.nce ? NULL : jitted;
}
GuestFn jit_wait(u64 pc) {
    (void)pc;
    return NULL;
}
const char* jit_failure(void) { return "no AOT pack and no JIT"; }
void jit_code_invalidate(u64 addr, u64 size) {
    (void)addr;
    (void)size;
}
void jit_code_changed(u64 addr, u64 size) {
    (void)addr;
    (void)size;
}

/* the rest of the runtime that rtapi.c names */
Hle g_hle;
u8* g_mem;
u64 g_code_lo = 0x8000000ull, g_code_hi = 0x8001000ull;
const SnxNros* g_nros; /* main.c's: a game module without NROs of its own */
void (*const g_svc64[SNX_SVC_COUNT])(CPU* c, u32 imm);
void snx_svc(CPU* c, u32 imm) {
    (void)c;
    (void)imm;
}
u64 guest_ticks(void) { return 0; }
int svc_code_line(u64 addr, u64* code) {
    (void)addr;
    (void)code;
    return 0;
}
uint32_t snx_runtime_cas128(uint64_t* p, uint64_t o0, uint64_t o1, uint64_t n0, uint64_t n1) {
    (void)p;
    (void)o0;
    (void)o1;
    (void)n0;
    (void)n1;
    return 0;
}
KThread* kthread_cur(void) { return NULL; }
void core_enter(KThread* t) { (void)t; }
void core_leave(KThread* t) { (void)t; }
void core_poll(KThread* t) { (void)t; }
void kthread_block(KThread* t) { (void)t; }
void kthread_unblock(KThread* t) { (void)t; }
void kernel_dump_regs(CPU* c) { (void)c; }
void guest_stats_add(u32 ix, unsigned what) {
    (void)ix;
    (void)what;
}
int hints_add(u64 pc) {
    (void)pc;
    return 0;
}
void save_hints(void) {}
void fs_save_report(void) {}
void plat_yield(void) {}
int mem_user_mapped(u64 addr, u64 size) {
    (void)addr;
    (void)size;
    return 0;
}
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "FAIL: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

/* the runtime's call of the game's function at pc, as nvn_cmd.c's guest_call makes it: until it returns to lr */
static void call(CPU* c, u64 pc) {
    u64 lr = c->x[30];
    do snx_lookup(c, pc)(c, pc);
    while ((pc = c->pc) != lr);
}

int main(void) {
    static GuestFn disp[(0x8001000 - 0x8000000) / 4]; /* no slot holds code: an NCE recompile's module */
    CPU c;
    memset(&c, 0, sizeof c);
    c.rt = &g_rtapi;
    c.disp = disp;
    c.code_lo = g_code_lo;
    c.code_hi = g_code_hi;

    /* under NCE: the native code at the callback's pc, which returns to the NVN call's return address */
    g_t.nce = 1;
    c.x[30] = RETURN_PC;
    call(&c, CALLBACK_PC);
    check(g_t.native == 1, "under NCE the callback runs the game's native code once");
    check(g_t.native_pc == CALLBACK_PC, "under NCE the native code runs from the callback's pc");
    check(g_t.native_lr == RETURN_PC && c.pc == RETURN_PC, "under NCE the callback returns to the NVN call's return");
    check(g_t.asked == 0 && g_t.jitted == 0, "under NCE no JIT is asked for the callback");

    /* without NCE: the JIT compiles it and its code runs */
    memset(&g_t, 0, sizeof g_t);
    c.x[30] = RETURN_PC;
    call(&c, CALLBACK_PC);
    check(g_t.asked == 1 && g_t.jitted == 1, "without NCE the JIT compiles the callback and runs it");
    check(g_t.native == 0, "without NCE no native code runs");
    check(c.pc == RETURN_PC, "without NCE the callback returns to the NVN call's return");

    if (failures) return 1;
    printf("rtapi_miss: ok\n");
    return 0;
}
