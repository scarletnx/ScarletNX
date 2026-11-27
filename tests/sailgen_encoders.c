/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --encoders of the in-tree a64.sail (a64_enc.inc): words the Arm ARM gives for instructions of every kind of
   field (registers, a shifted register's shift, an add's immediate and its shift, a scaled offset, a condition, a
   bitmask immediate's N:immr:imms, a branch's offset, a vector's size and Q), each made by its clause's encoder;
   every encoder's fields placed apart from each other and from the bits its pattern fixes; and what clauses of every
   kind reach of FPCR and FPSR (fp) */
#include <stdio.h>
#include <string.h>

typedef unsigned SnxU32;
#include "a64_enc.inc"

static int failures;

static void expect(const char* what, SnxU32 got, SnxU32 want) {
    if (got != want) {
        printf("%s: %08X, expected %08X\n", what, got, want);
        ++failures;
    }
}

static void fp_expect(const char* name, unsigned want) {
    unsigned k;
    for (k = 0; k < sge_a64_count && strcmp(sge_a64_table[k].name, name); ++k) {}
    if (k == sge_a64_count) {
        printf("%s: no such clause\n", name);
        ++failures;
    } else if (sge_a64_table[k].fp != want) {
        printf("%s: fp %u, expected %u\n", name, sge_a64_table[k].fp, want);
        ++failures;
    }
}

int main(void) {
    unsigned k, j;
    expect("ADD X1, X2, X3", sge_a64_add_addsub_shift(1, 2, 0, 3, 0, 1), 0x8B030041u);
    expect("ADD W1, W2, W3, LSL #4", sge_a64_add_addsub_shift(1, 2, 4, 3, 0, 0), 0x0B031041u);
    expect("CMP X4, #16", sge_a64_subs_addsub_imm(31, 4, 16, 0, 1), 0xF100409Fu);
    expect("ADD X0, SP, #1, LSL #12", sge_a64_add_addsub_imm(0, 31, 1, 1, 1), 0x914007E0u);
    expect("LDR X5, [X6, #64]", sge_a64_ldr_imm_gen_immediate_unsigned(5, 6, 8, 3), 0xF94020C5u);
    expect("LDRB W0, [X1], #1", sge_a64_ldrb_imm_post_idx(0, 1, 1), 0x38401420u);
    expect("CSEL X0, X1, X2, LT", sge_a64_csel(0, 1, 0xB, 2, 1), 0x9A82B020u);
    expect("AND X0, X1, #0xFF", sge_a64_and_log_imm(0, 1, 7, 0, 1, 1), 0x92401C20u);
    expect("B .+8", sge_a64_b_uncond(2), 0x14000002u);
    expect("BL .-8", sge_a64_bl(0x3FFFFFE), 0x97FFFFFEu);
    expect("MRS X0, TPIDR_EL0", sge_a64_mrs(0, 2, 0, 13, 3, 3), 0xD53BD040u);
    expect("CBNZ W3, .-4", sge_a64_cbnz(3, 0x7FFFF, 0), 0x35FFFFE3u);
    expect("UDIV X0, X1, X2", sge_a64_udiv(0, 1, 2, 1), 0x9AC20820u);
    expect("ADD V0.4S, V1.4S, V2.4S", sge_a64_add_advsimd_simd(0, 1, 2, 2, 1), 0x4EA28420u);
    /* what each clause's code reaches of FPCR and FPSR: nothing for an integer one; nothing where a field the pattern
       fixes folds a saturation away (SSHL's S), nor FEAT_AFP's FPCR.AH, which the profile names in no feature line
       (FABS); QC written (SQADD); FPCR given to a lowered function (FADD), or one that reads it with none given
       (FRECPS), 2; AES's lowered tables, nothing */
    fp_expect("add_advsimd_simd", 0);
    fp_expect("eor_advsimd", 0);
    fp_expect("sshl_advsimd_simd", 0);
    fp_expect("fabs_float", 0);
    fp_expect("fmov_float", 0);
    fp_expect("aese_advsimd", 0);
    fp_expect("sqadd_advsimd_simd", 1);
    fp_expect("sqshl_advsimd_reg_sisd", 1);
    fp_expect("fadd_advsimd_fp", 2);
    fp_expect("frecps_advsimd_recps_simd", 2);
    for (k = 0; k < sge_a64_count; ++k) {
        SnxU32 seen = sge_a64_table[k].mask;
        for (j = 0; j < sge_a64_table[k].nfields; ++j) {
            unsigned lo = sge_a64_table[k].fields[j].lo, w = sge_a64_table[k].fields[j].width;
            SnxU32 m = (w >= 32 ? ~0u : (1u << w) - 1) << lo;
            if (lo + w > 32) {
                printf("%s: field %s past bit 31\n", sge_a64_table[k].name, sge_a64_table[k].fields[j].name);
                ++failures;
            }
            if (m & seen & ~sge_a64_table[k].mask) {
                printf("%s: field %s overlaps another\n", sge_a64_table[k].name, sge_a64_table[k].fields[j].name);
                ++failures;
            }
            seen |= m;
        }
        if ((sge_a64_table[k].value & ~sge_a64_table[k].mask) != 0) {
            printf("%s: fixed bits outside the pattern's mask\n", sge_a64_table[k].name);
            ++failures;
        }
    }
    printf("sailgen_encoders: %u encoders, %d failures\n", sge_a64_count, failures);
    return failures != 0;
}
