/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* rt_undef (src/nx/runtime/rtapi.c), RtAPI.undef: the end of a game that ran a word the recompiler made no code for.
   A word no Arm CPU runs (the trap a game's own abort places after a call, A64's UDF) is said to be the game's, which
   the console stops on too; a word an Arm CPU runs is said to be the recompiler's. When lr is the pc and a call is
   just before it, the call came back there, so the registers are what that callee left, not the call's operands (x8
   = 0 after a BLR x8 is no call to address 0). The rest of the runtime is faked here */
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
#include <setjmp.h>
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

#define GUEST_SIZE 0x3000u /* the guest's memory here: [0, GUEST_SIZE), all of it mapped but the first page */

/* what the runtime said: the log's lines and the fatal error's text, and where fatal returns to */
static struct {
    char log[4096], fatal[512];
    size_t used;
    jmp_buf out;
} g_t;

void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    int n;
    (void)lvl;
    va_start(ap, fmt);
    n = vsnprintf(g_t.log + g_t.used, sizeof g_t.log - g_t.used, fmt, ap);
    va_end(ap);
    if (n > 0) g_t.used = g_t.used + (size_t)n < sizeof g_t.log ? g_t.used + (size_t)n : sizeof g_t.log - 1;
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_t.fatal, sizeof g_t.fatal, fmt, ap);
    va_end(ap);
    longjmp(g_t.out, 1);
}

/* the first page is free, as the guest's address 0 is: no code is read there */
int mem_user_mapped(u64 addr, u64 size) { return addr >= 0x1000 && addr + size <= GUEST_SIZE; }

/* the rest of the runtime that rtapi.c names */
GuestFn nce_slot(GuestFn f) { return f; }
GuestFn jit_resolve_or_add(u64 pc) {
    (void)pc;
    return NULL;
}
GuestFn jit_wait(u64 pc) {
    (void)pc;
    return NULL;
}
const char* jit_failure(void) { return "no AOT pack and no JIT"; }
static int g_invalidated;
void jit_code_invalidate(u64 addr, u64 size) {
    (void)addr;
    (void)size;
    g_invalidated++;
}
void jit_code_changed(u64 addr, u64 size) {
    (void)addr;
    (void)size;
}
Hle g_hle;
u8* g_mem;
u64 g_code_lo = 0x1000ull, g_code_hi = GUEST_SIZE;
const SnxNros* g_nros;
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

static void put_word(u64 at, u32 w) {
    g_mem[at] = (u8)w;
    g_mem[at + 1] = (u8)(w >> 8);
    g_mem[at + 2] = (u8)(w >> 16);
    g_mem[at + 3] = (u8)(w >> 24);
}

/* RtAPI.undef at pc with lr: 1 when it ended the game (fatal), 0 when it returned */
static int undef(CPU* c, u64 pc, u64 lr, u32 w) {
    memset(g_t.log, 0, sizeof g_t.log);
    memset(g_t.fatal, 0, sizeof g_t.fatal);
    g_t.used = 0;
    c->x[30] = lr;
    if (setjmp(g_t.out)) return 1;
    g_rtapi.undef(c, pc, w);
    return 0;
}

int main(void) {
    static u8 memory[GUEST_SIZE];
    CPU c;
    char what[160];
    memset(&c, 0, sizeof c);
    c.rt = &g_rtapi;
    g_mem = memory;

    /* a game's own stop after a call that came back, as one's allocator does after it printed its free list: LDR x8,
       [x8, #0x60]; BLR x8; then the A32 UDF word its compiler places there, which A64 leaves unallocated */
    put_word(0x1100, 0xf9403108u);
    put_word(0x1104, 0xd63f0100u);
    put_word(0x1108, 0xe7ffdefeu);
    check(undef(&c, 0x1108, 0x1108, 0xe7ffdefeu), "the game's trap word ends the game");
    check(strstr(g_t.fatal, "an undefined instruction (e7ffdefe at 1108), which the console's CPU cannot") != NULL,
          "the trap word is an undefined instruction, the game's");
    check(strstr(g_t.fatal, "recompiler") == NULL, "the trap word is not blamed on the recompiler");
    check(strstr(g_t.log, "*** The game ran undefined instruction e7ffdefe at 1108, which the console's CPU cannot run "
                          "either ***") != NULL,
          "the log's marker names the undefined instruction");
    check(strstr(g_t.log, "the call at 1104 (d63f0100) returned to it: x0 to x18 hold what that call left") != NULL,
          "the BLR before the trap is said to have come back, its registers the callee's");

    /* A64's UDF #0 where lr is elsewhere: undefined, with no call before it to name */
    put_word(0x1200, 0x00000000u);
    check(undef(&c, 0x1200, 0x1180, 0) && strstr(g_t.fatal, "undefined instruction (00000000 at 1200)") != NULL,
          "UDF #0 is an undefined instruction");
    check(strstr(g_t.log, "returned to it") == NULL, "no call came back to a UDF lr is not at");

    /* a word an Arm CPU runs (SHA512H, Armv8.2) that the recompiler made no code for: the recompiler's */
    put_word(0x1300, 0xce62c820u);
    check(undef(&c, 0x1300, 0x1300, 0xce62c820u), "an instruction the recompiler lacks ends the game");
    snprintf(what, sizeof what, "fatal: %s", g_t.fatal);
    check(strstr(g_t.fatal, "an instruction the recompiler cannot run (ce62c820 at 1300)") != NULL, what);
    check(strstr(g_t.log, "*** The game ran instruction ce62c820 at 1300, which the recompiler cannot run ***") != NULL,
          "the log's marker for an instruction the recompiler lacks is the one it always was");
    check(strstr(g_t.log, "returned to it") == NULL, "lr at pc after no call names no call");

    /* lr at a trap: a call is named only when the word before it is one */
    put_word(0x1400, 0xd503201fu); /* NOP */
    put_word(0x1404, 0xe7ffdefeu);
    check(undef(&c, 0x1404, 0x1404, 0xe7ffdefeu) && strstr(g_t.log, "returned to it") == NULL,
          "a NOP before the trap is no call that came back");
    put_word(0x1500, 0x94000010u); /* BL +0x40 */
    put_word(0x1504, 0xe7ffdefeu);
    check(undef(&c, 0x1504, 0x1504, 0xe7ffdefeu) &&
              strstr(g_t.log, "the call at 1500 (94000010) returned to it") != NULL,
          "a BL before the trap is a call that came back");

    /* the first word of the guest's first mapped page, the page before it free: nothing is read there */
    put_word(0x1000, 0xe7ffdefeu);
    check(undef(&c, 0x1000, 0x1000, 0xe7ffdefeu) && strstr(g_t.log, "returned to it") == NULL,
          "no word is read before the first mapped page");

    /* the word memory holds now is not the one compiled: compiled again, the game goes on */
    g_invalidated = 0;
    check(!undef(&c, 0x1504, 0x1504, 0x00000000u) && g_invalidated == 1 && !g_t.fatal[0],
          "code changed since its compile is compiled again");

    if (failures) return 1;
    printf("rtapi_undef: ok\n");
    return 0;
}
