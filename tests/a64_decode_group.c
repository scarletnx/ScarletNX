/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What tools/decodetree --a64 makes of two things a64.decode cannot show, on a64_decode_group/'s decoder.
   A row whose body refuses a word: the word goes to the rows after it. FIRST, SECOND and THIRD take the same words in
   a { } group, FIRST refuses rd 31 by a predicate, SECOND needs FEAT_AES from rd 30 on, THIRD takes every one; ALONE
   has FIRST's predicate and no row after it. a64.decode has no word that one row refuses and a later one takes.
   An argument's type: the narrowest that holds every value its rows give it, so the EDGE rows' arguments stop at 8,
   9, 16 and 17 bits, at a constant, and at the ends of a function's values. */
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

static void eq(long long got, long long want, const char* what, int line) {
    if (got == want) return;
    fprintf(stderr, "a64_decode_group.c:%d: %s is %lld, expected %lld\n", line, what, got, want);
    g_failures++;
}

/* the argument of word's row: its value, and the bytes its type takes */
#define ARG(word, op, field, want, bytes)                                                                              \
    do {                                                                                                               \
        SnxA64Insn d;                                                                                                  \
        eq(snx_a64_decode(word, SNX_A64_TARGET_BASE, &d), SNX_A64_OP_##op, #op, __LINE__);                             \
        eq(d.args.field, want, #field, __LINE__);                                                                      \
        eq((long long)sizeof d.args.field, bytes, "sizeof " #field, __LINE__);                                         \
    } while (0)

/* each argument at both ends of what its rows give it: a type one step narrower would cut the value, and the size
   says none is wider than it needs */
static void edges(void) {
    /* 8 bits fit a byte, unsigned (255) or signed (-128, 127); 9 bits need two */
    ARG(0x8100ff00u, EDGE_8, u8, 255, 1);
    ARG(0x81000080u, EDGE_8, s8, -128, 1);
    ARG(0x8100007fu, EDGE_8, s8, 127, 1);
    ARG(0x8203fe00u, EDGE_9, u9, 511, 2);
    ARG(0x82000100u, EDGE_9, s9, -256, 2);
    ARG(0x820000ffu, EDGE_9, s9, 255, 2);
    /* 16 bits fit two bytes, 17 need four */
    ARG(0x83ffff00u, EDGE_U16, u16, 65535, 2);
    ARG(0x84800000u, EDGE_S16, s16, -32768, 2);
    ARG(0x847fff00u, EDGE_S16, s16, 32767, 2);
    ARG(0x85ffff80u, EDGE_U17, u17, 131071, 4);
    ARG(0x86800000u, EDGE_S17, s17, -65536, 4);
    ARG(0x867fff80u, EDGE_S17, s17, 65535, 4);
    /* constants */
    ARG(0x87000000u, EDGE_K, k, -200, 2);
    ARG(0x87000000u, EDGE_K, m, 70000, 4);
    /* 8 - x of 5 bits runs from 8 down to -23; x ^ 2 of 2 bits stays in 0 to 3 */
    ARG(0x88000000u, EDGE_F, down, 8, 1);
    ARG(0x8800001fu, EDGE_F, down, -23, 1);
    ARG(0x88000000u, EDGE_F, swap, 2, 1);
    ARG(0x88000100u, EDGE_F, swap, 3, 1);
    /* an argument no EDGE row gives is 0 in its instruction */
    ARG(0x8100ff00u, EDGE_8, m, 0, 4);
    /* the widest come first, so nothing pads them: 3 of four bytes, 5 of two, 5 of one, to a multiple of four */
    eq((long long)sizeof(SnxA64Args), 28, "sizeof(SnxA64Args)", __LINE__);
}

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
    edges();
    if (g_failures) {
        fprintf(stderr, "a64_decode_group: %d failures\n", g_failures);
        return 1;
    }
    printf("a64_decode_group: ok\n");
    return 0;
}
