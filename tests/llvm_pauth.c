/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Pointer authentication as the switch2 target runs it, with the address keys off (ir_int.c), through the LLVM
   steps (llvm_steps.c). QEMU's user mode turns the keys on, so llvm_qemu_differential cannot check these: an AUT* of
   a pointer that no PAC* signed faults there. Also the counter's frequency of each console, and the switch target,
   which has none of it */
#include "llvm_steps.h"
#include <stdio.h>
#include <string.h>

static int failures;
static unsigned undefs;
static void undef(CPU* c, u64 pc, u32 w) {
    (void)c;
    (void)pc;
    (void)w;
    ++undefs;
}
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

#define PC 0x10000u

enum {
    PACIBSP,
    AUTIBSP,
    RETAB,
    RETAA,
    BRAAZ_X3,
    BLRAA_X3_X4,
    PACIA_X1_X2,
    AUTDB_X1_X2,
    XPACI_X5,
    XPACLRI,
    PACGA_X0_X1_X2,
    PACGA_X0_X1_SP,
    LDRAA_X0_X1_16_PRE,
    LDRAB_X0_X1_NEG8,
    CNTFRQ_SWITCH2,
    CNTFRQ_SWITCH,
    RETAB_SWITCH,
    PACIBSP_SWITCH,
    COUNT
};

static const u32 words[COUNT] = {
    0xD503237Fu,                               /* PACIBSP */
    0xD50323FFu,                               /* AUTIBSP */
    0xD65F0FFFu,                               /* RETAB */
    0xD65F0BFFu,                               /* RETAA */
    0xD61F081Fu | 3u << 5,                     /* BRAAZ X3 */
    0xD73F0800u | 3u << 5 | 4u,                /* BLRAA X3, X4 */
    0xDAC10000u | 2u << 5 | 1u,                /* PACIA X1, X2 */
    0xDAC11C00u | 2u << 5 | 1u,                /* AUTDB X1, X2 */
    0xDAC143E0u | 5u,                          /* XPACI X5 */
    0xD50320FFu,                               /* XPACLRI */
    0x9AC03000u | 2u << 16 | 1u << 5,          /* PACGA X0, X1, X2 */
    0x9AC03000u | 31u << 16 | 1u << 5,         /* PACGA X0, X1, SP */
    0xF8200C00u | 2u << 12 | 1u << 5,          /* LDRAA X0, [X1, #16]! */
    0xF8A00400u | 1u << 22 | 0x1FFu << 12 | 1u << 5, /* LDRAB X0, [X1, #-8] */
    0xD53BE000u,                               /* MRS X0, CNTFRQ_EL0 */
    0xD53BE000u,
    0xD65F0FFFu, /* RETAB, for the Switch's A57 */
    0xD503237Fu, /* PACIBSP, a hint the A57 runs as a NOP */
};

static u8 g_mem[0x100];

static void reset(CPU* c, RtAPI* rt) {
    memset(c, 0, sizeof *c);
    memset(rt, 0, sizeof *rt);
    rt->undef = undef;
    c->rt = rt;
    c->mem = g_mem;
    for (unsigned i = 0; i < 31; i++) c->x[i] = 0x1000u + i;
    c->x[30] = 0x8000124ull;
    c->x[31] = 0xFFFFF000ull; /* SP */
    undefs = 0;
}

int main(void) {
    LlvmSteps steps;
    char error[1024];
    CPU c, before;
    RtAPI rt;
    u64 next, first;
    if (llvm_steps_begin(&steps, 1, 0, error, sizeof error)) return snx_error("%s", error);
    for (unsigned i = 0; i < COUNT; i++)
        llvm_steps_add(&steps, words[i], PC, i >= CNTFRQ_SWITCH ? 0 : SNX_A64_TARGET_SWITCH2);
    if (llvm_steps_load(&steps, error, sizeof error)) return snx_error("%s", error);

    /* the hint forms and the register forms leave every register as it is */
    static const int same[] = {PACIBSP, AUTIBSP, PACIA_X1_X2, AUTDB_X1_X2, PACIBSP_SWITCH};
    for (unsigned i = 0; i < sizeof same / sizeof *same; i++) {
        reset(&c, &rt);
        before = c;
        next = steps.steps[same[i]](&c);
        check(next == PC + 4 && !undefs && !memcmp(c.x, before.x, sizeof c.x), "PAC* and AUT* change nothing");
    }

    /* the combined returns and branches go to the register: x30, Xn, and BLRAA links */
    reset(&c, &rt);
    check(steps.steps[RETAB](&c) == 0x8000124ull && !undefs, "RETAB returns to x30");
    reset(&c, &rt);
    check(steps.steps[RETAA](&c) == 0x8000124ull && !undefs, "RETAA returns to x30");
    reset(&c, &rt);
    check(steps.steps[BRAAZ_X3](&c) == 0x1003u, "BRAAZ X3 branches to x3");
    reset(&c, &rt);
    check(steps.steps[BLRAA_X3_X4](&c) == 0x1003u && c.x[30] == PC + 4, "BLRAA X3, X4 calls x3 and links");

    /* Strip with TBI off: bit 55 over the bits above the address */
    reset(&c, &rt);
    c.x[5] = 0xABCD001234567890ull; /* bit 55 set */
    steps.steps[XPACI_X5](&c);
    check(c.x[5] == 0xFFFF001234567890ull, "XPACI extends bit 55 when set");
    reset(&c, &rt);
    c.x[5] = 0x7F4D001234567890ull; /* bit 55 clear */
    steps.steps[XPACI_X5](&c);
    check(c.x[5] == 0x0000001234567890ull, "XPACI clears the field when bit 55 is clear");
    reset(&c, &rt);
    c.x[30] = 0x0123000008001234ull;
    steps.steps[XPACLRI](&c);
    check(c.x[30] == 0x08001234ull, "XPACLRI strips x30");

    /* PACGA: zero in the low half, one value for one input, another for another modifier, SP as Xm 31 */
    reset(&c, &rt);
    c.x[1] = 0x8001000ull;
    c.x[2] = 0xFFFFF000ull;
    steps.steps[PACGA_X0_X1_X2](&c);
    first = c.x[0];
    check(first && !(first & 0xFFFFFFFFull), "PACGA leaves the low half zero");
    steps.steps[PACGA_X0_X1_X2](&c);
    check(c.x[0] == first, "PACGA gives the same PAC for the same input");
    steps.steps[PACGA_X0_X1_SP](&c);
    check(c.x[0] == first, "PACGA's Xm 31 is SP");
    c.x[2] ^= 0x10;
    steps.steps[PACGA_X0_X1_X2](&c);
    check(c.x[0] != first, "PACGA depends on the modifier");

    /* LDRAA pre-indexed and LDRAB with a negative offset: a plain doubleword load */
    for (unsigned i = 0; i < sizeof g_mem; i++) g_mem[i] = (u8)i;
    reset(&c, &rt);
    c.x[1] = 0x40;
    steps.steps[LDRAA_X0_X1_16_PRE](&c);
    check(c.x[0] == 0x5756555453525150ull && c.x[1] == 0x50, "LDRAA [X1, #16]! loads and writes back");
    reset(&c, &rt);
    c.x[1] = 0x40;
    steps.steps[LDRAB_X0_X1_NEG8](&c);
    check(c.x[0] == 0x3F3E3D3C3B3A3938ull && c.x[1] == 0x40, "LDRAB [X1, #-8] loads without writeback");

    /* each console's counter frequency */
    reset(&c, &rt);
    steps.steps[CNTFRQ_SWITCH2](&c);
    check(c.x[0] == 31250000u, "the Switch 2's CNTFRQ_EL0 is 31.25 MHz");
    reset(&c, &rt);
    steps.steps[CNTFRQ_SWITCH](&c);
    check(c.x[0] == 19200000u, "the Switch's CNTFRQ_EL0 is 19.2 MHz");

    /* the Switch's A57 has no pointer authentication: RETAB is UNDEF there */
    reset(&c, &rt);
    check(steps.steps[RETAB_SWITCH](&c) == PC && undefs == 1, "RETAB is UNDEF on the Switch");

    llvm_steps_end(&steps);
    if (failures) return 1;
    printf("pointer authentication with the keys off: passed\n");
    return 0;
}
