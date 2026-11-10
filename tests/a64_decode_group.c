/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What tools/decodetree --a64 makes of a row whose body refuses a word: the word goes to the rows after it. The
   decoder is a64_decode_group/'s, four rows: FIRST, SECOND and THIRD take the same words in a { } group, FIRST
   refuses rd 31 by a predicate, SECOND needs FEAT_AES from rd 30 on, THIRD takes every one; ALONE has FIRST's
   predicate and no row after it. a64.decode has no word that one row refuses and a later one takes, so only this
   decoder walks that path. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "a64_decode.h"
#include "a64_decode.inc"

static int g_failures;

/* decodes word for the target and expects the op, rd, the flow, nop and the features needed */
static void dec(uint32_t word, uint64_t target, SnxA64Op op, int rd, SnxA64Flow flow, int nop, uint64_t feature,
                int line) {
    SnxA64Insn d;
    SnxA64Op got;
    memset(&d, 0xa5, sizeof d);
    got = snx_a64_decode(word, target, &d);
    if (got == op && d.op == op && d.args.rd == rd && d.flow == flow && d.nop == nop && d.feature == feature) return;
    fprintf(
        stderr,
        "a64_decode_group.c:%d: 0x%08x decodes as %s (insn: %s) rd %d flow %d nop %d feature 0x%llx, expected %s rd "
        "%d flow %d nop %d feature 0x%llx\n",
        line, (unsigned)word, snx_a64_op_name(got), snx_a64_op_name(d.op), (int)d.args.rd, (int)d.flow, d.nop,
        (unsigned long long)d.feature, snx_a64_op_name(op), rd, (int)flow, nop, (unsigned long long)feature);
    g_failures++;
}

#define DEC(word, target, op, rd, flow, nop, feature)                                                                  \
    dec(word, SNX_A64_TARGET_##target, SNX_A64_OP_##op, rd, SNX_A64_FLOW_##flow, nop, feature, __LINE__)
#define V8  SNX_A64_FEAT_V8_0
#define AES SNX_A64_FEAT_AES

int main(void) {
    /* the first row of the group takes what its predicate allows */
    DEC(0x00000000u, BASE, FIRST, 0, PLAIN, 0, V8);
    DEC(0x0000001eu, BASE, FIRST, 30, PLAIN, 0, V8);
    /* rd 31: FIRST's predicate refuses it and SECOND takes it where the target has the feature it then needs */
    DEC(0x0000001fu, MAX, SECOND, 31, PLAIN, 0, V8 | AES);
    /* without the feature SECOND refuses it too, and THIRD's answer carries nothing of SECOND's try */
    DEC(0x0000001fu, BASE, THIRD, 31, B, 1, V8);
    /* a refusal with no row after it is UNDEF, the instruction all zero */
    DEC(0x0100001fu, MAX, UNDEF, 0, UNDEF, 0, 0);
    DEC(0x0100001eu, MAX, ALONE, 30, PLAIN, 0, V8);
    /* a word of no row */
    DEC(0x02000000u, MAX, UNDEF, 0, UNDEF, 0, 0);
    if (g_failures) {
        fprintf(stderr, "a64_decode_group: %d failures\n", g_failures);
        return 1;
    }
    printf("a64_decode_group: ok\n");
    return 0;
}
