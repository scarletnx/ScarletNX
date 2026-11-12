/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A 32-bit game's blocks compiled while it runs from code outside its modules (ldr:ro's NROs, runtime/arm.c) go when
   that memory is unmapped. ldr:ro maps one NRO where it unmapped another, and the first one's blocks ran on the second
   one's data, which held no code where they branched. Real arm.c and block compiler; memory is a buffer that mem_query
   calls code, and the unmap is mem.c's call to the hook arm.c gives it */
#include "nx/runtime/arm.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/rtapi.h"
#include "nx/hle/kernel/kernel_int.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "arm_code_invalidate:%d: %s\n", __LINE__, #x);                                             \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define NRO   0x10000u /* where ldr:ro maps both NROs, one after the other */
#define OTHER 0x20000u /* another NRO's code, mapped all along */
#define LR    0x8000u

static u8 g_memory[0x30000];
u8* g_mem = g_memory;
static void (*g_invalidate)(u64, u64);
void mem_set_code_invalidate(void (*fn)(u64 addr, u64 size)) { g_invalidate = fn; }
/* each page an NRO's text, as ldr:ro maps it */
void mem_query(u64 addr, MemBlock* out) {
    memset(out, 0, sizeof *out);
    out->addr = addr & ~0xFFFull;
    out->size = 0x1000;
    out->state = MS_ALIASCODE;
    out->perm = PERM_RX;
}
static void note_hint(uint64_t pc) { (void)pc; }
const RtAPI g_rtapi = {.note_hint = note_hint};
KThread* kthread_cur(void) { return NULL; }
void core_poll(KThread* t) { (void)t; }
void svc_exit_thread(KThread* t) {
    (void)t;
    CHECK(!"a thread exit");
}
void kernel_dump_regs(CPU* c) { (void)c; }
void snx_svc32(SnxArmCPU* a, u32 imm) { (void)a, (void)imm; }
void save_hints(void) {}
u64 guest_ticks(void) { return 0; }
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("fatal: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

/* mov r0, #value; bx lr at addr, the guest's little-endian words */
static void code(u32 addr, u32 value) {
    const u32 words[2] = {0xE3A00000u | value, 0xE12FFF1Eu};
    for (unsigned k = 0; k < 8; k++) g_memory[addr + k] = (u8)(words[k / 4] >> (k % 4 * 8));
}
/* r0 after the block at addr returns, entered there as arm.c's loop enters it */
static u32 run(u32 addr, SnxArmBlock b) {
    SnxArmCPU c;
    memset(&c, 0, sizeof c);
    c.mem = g_memory;
    c.mem_size = sizeof g_memory;
    c.cpsr = 0x10; /* user mode, A32 */
    c.r[14] = LR;
    c.r[15] = addr;
    b(&c);
    if (c.r[15] != LR || c.exit)
        fprintf(stderr, "the block ended at %x with exit %u (syndrome %x), r0 %u\n", (unsigned)c.r[15],
                (unsigned)c.exit, (unsigned)c.syndrome, (unsigned)c.r[0]);
    CHECK(c.r[15] == LR && !c.exit);
    return c.r[0];
}

int main(void) {
    static const SnxArmModule no_modules; /* a game whose modules hold none of this code */
    SnxArmBlock first, other, second;
    g_arm_mod = &no_modules;
    arm_dispatch_init();
    CHECK(g_invalidate); /* arm.c hears of mapping changes */

    code(NRO, 1);
    code(OTHER, 3);
    first = arm_block(NRO, 0);
    other = arm_block(OTHER, 0);
    CHECK(first && other && run(NRO, first) == 1 && run(OTHER, other) == 3);

    /* ldr:ro unmaps the first NRO (mem_unmap_code) and maps the next one at the same address */
    g_invalidate(NRO, 0x1000);
    code(NRO, 2);
    second = arm_block(NRO, 0);
    CHECK(second && second != first);
    CHECK(run(NRO, second) == 2);
    CHECK(arm_block(OTHER, 0) == other); /* the other NRO's block stays */
    puts("A block compiled from an unmapped NRO goes, and the NRO mapped in its place runs its own code");
    return 0;
}
