/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Maxwell decoder tools/decodetree makes from sm53.decode: every opcode has an encoding, the opcode is the top 16
   bits' alone (but for EXIT.FCSM_TR, read as NOP), and the patterns that overlap pick the narrower. */
#include "nx/gpu/shader/translator.h"

static int failures;
static u64 state = 0x9E3779B97F4A7C15ull;

static u64 next_random(void) {
    state ^= state << 13;
    state ^= state >> 7;
    state ^= state << 17;
    return state;
}

static int id_of(u64 w) {
    const Op* op = decode(w);
    return op ? op->id : O_NONE;
}

/* the opcode of the top 16 bits, the rest of the word random */
static void expect(unsigned top, int id, const char* name) {
    u64 w = (u64)top << 48 | (next_random() & 0xFFFFFFFFFFE0ull);
    const Op* op = decode(w);
    if (!op || op->id != id || strcmp(op->name, name)) {
        fprintf(stderr, "FAIL: %04x decodes as %s, not %s\n", top, op ? op->name : "nothing", name);
        failures++;
    }
}

int main(void) {
    static unsigned char seen[O__COUNT];
    unsigned top;
    int i, k;
    for (top = 0; top < 0x10000; top++) {
        int id = id_of((u64)top << 48);
        seen[id] = 1;
        for (k = 0; k < 8; k++) {
            u64 w = (u64)top << 48 | (next_random() & 0xFFFFFFFFFFFFull);
            int want = id == O_EXIT && B(w, 0, 5) == 0x1C ? O_NOP : id;
            if (id_of(w) != want) {
                fprintf(stderr, "FAIL: %016llx decodes apart from its top 16 bits\n", (unsigned long long)w);
                failures++;
                break;
            }
        }
    }
    for (i = 1; i < O__COUNT; i++)
        if (!seen[i]) {
            fprintf(stderr, "FAIL: opcode %d has no encoding\n", i);
            failures++;
        }
    expect(0x5C58, O_FADD_R, "FADD (reg)");
    expect(0x3858, O_FADD_I, "FADD (imm)");
    expect(0x3958, O_FADD_I, "FADD (imm)");
    /* the { } groups: IMAD32I inside LD, LDC and MEMBAR inside CCTLL, LDG, STG, STP and ATOM.CAS inside ATOMS.CAS */
    expect(0x8000, O_IMAD32I, "IMAD32I");
    expect(0x8400, O_LD, "LD");
    expect(0xEF90, O_LDC, "LDC");
    expect(0xEF98, O_MEMBAR, "MEMBAR");
    expect(0xEF80, O_CCTLL, "CCTLL");
    expect(0xEED0, O_LDG, "LDG");
    expect(0xEEF0, O_ATOM_CAS, "ATOM.CAS");
    expect(0xEE00, O_ATOMS_CAS, "ATOMS.CAS");
    /* CCTL's further encoding */
    expect(0xEBF0, O_CCTL, "CCTLT");
    if (id_of(0xE30000000000001Full) != O_EXIT || id_of(0xE30000000000001Cull) != O_NOP) {
        fprintf(stderr, "FAIL: EXIT.FCSM_TR is not a NOP\n");
        failures++;
    }
    if (decode(0xFFFFFFFFFFFFFFFFull)) {
        fprintf(stderr, "FAIL: an all-ones word decodes\n");
        failures++;
    }
    return failures ? 1 : 0;
}
