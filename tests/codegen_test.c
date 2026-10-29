/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic decoder, CFG and code tests: the generated decoder's UNDEF set, flow and branch targets; function
   discovery; architectural results of the LLVM backend's steps (llvm_steps.c) against expected values; and a
   compiled loop's poll at its backward branch. */
#include "llvm_steps.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/codegen.h"
#include "nx/loader/codegen_view.h"
#include <fenv.h>
#include <stdlib.h>
#include <string.h>

static int failures;

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

static void put32(unsigned char* image, size_t offset, SnxU32 word) {
    unsigned i;
    for (i = 0; i < 4; ++i) image[offset + i] = (unsigned char)(word >> (8 * i));
}

static void decoder_test(void) {
    static const SnxU32 invalid[] = {0,
                                     0xFFFFFFFFUL,
                                     0x1200FC00UL,
                                     0x52C00000UL,
                                     0x53400000UL,
                                     0x13808000UL,
                                     0xF8600800UL,
                                     0x0A008000UL,
                                     0x0BC00000UL,
                                     0x0B201400UL,
                                     0x1E180000UL,
                                     0x1EE60020UL, /* General-register half FMOV requires later FP16. */
                                     0x1EE70020UL,
                                     0x9EE60020UL,
                                     0x9EE70020UL};
    /* ERET, HVC #0, SMC #0, HLT #0, MSR SPSel, DAIFSet, DAIFClr, PAN, UAO (immediate) */
    static const SnxU32 el1[] = {0xD69F03E0UL, 0xD4000002UL, 0xD4000003UL, 0xD4400000UL, 0xD50040BFUL,
                                 0xD50340DFUL, 0xD50340FFUL, 0xD500409FUL, 0xD500407FUL};
    static const SnxU32 reserved[] = {0x5E218820UL, 0x0E618820UL, 0x5E217820UL,
                                      0x5EA1C820UL, 0x7EA1C820UL, 0x7EA1F820UL};
    static const SnxU32 backwards[] = {0x17FFFFFFUL, 0x97FFFFFFUL, 0x54FFFFE0UL, 0xB4FFFFE0UL, 0x3607FFE0UL};
    SnxA64Insn insn;
    size_t i;
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i)
        check(snx_a64_flow(invalid[i], 0) == SNX_A64_FLOW_UNDEF, "reject reserved/invalid encoding");
    /* the FP witnesses' reserved encodings: scalar FRINTN, FRINTN .1D, scalar FCVTL, scalar URECPE and URSQRTE,
       FSQRT's AdvSIMD scalar form */
    for (i = 0; i < sizeof(reserved) / sizeof(reserved[0]); ++i)
        check(snx_a64_flow(reserved[i], 0) == SNX_A64_FLOW_UNDEF &&
                  snx_a64_flow(reserved[i], SNX_A64_TARGET_MAX) == SNX_A64_FLOW_UNDEF,
              "reserved AdvSIMD FP encoding");
    check(snx_a64_flow(0x1EE24020UL, 0) == SNX_A64_FLOW_PLAIN, "A57 FCVT S,H remains allocated");
    check(snx_a64_flow(0x1EE2C020UL, 0) == SNX_A64_FLOW_PLAIN, "A57 FCVT D,H remains allocated");
    check(snx_a64_flow(0x1E23C020UL, 0) == SNX_A64_FLOW_PLAIN, "A57 FCVT H,S remains allocated");
    check(snx_a64_flow(0x1E63C020UL, 0) == SNX_A64_FLOW_PLAIN, "A57 FCVT H,D remains allocated");
    check(snx_a64_flow(0xD503201FUL, 0) == SNX_A64_FLOW_PLAIN, "NOP decode");
    check(snx_a64_flow(0xD65F03C0UL, 0) == SNX_A64_FLOW_RET, "RET decode");
    check(snx_a64_flow(0x94000002UL, 0) == SNX_A64_FLOW_BL, "BL decode");
    /* EL0: ERET, HVC, SMC, HLT and the EL1 MSR immediates are UNDEF whatever features are asked for; CFINV and the
       unallocated MSR immediates are for the Switch */
    for (i = 0; i < sizeof(el1) / sizeof(el1[0]); ++i)
        check(snx_a64_flow(el1[i], 0) == SNX_A64_FLOW_UNDEF && snx_a64_flow(el1[i], ~(SnxU64)0) == SNX_A64_FLOW_UNDEF,
              "EL1 encoding is UNDEF at EL0");
    check(snx_a64_flow(0xD500401FUL, 0) == SNX_A64_FLOW_UNDEF, "CFINV (FEAT_FlagM) is UNDEF on the Switch");
    check(snx_a64_flow(0xD500401FUL, SNX_A64_TARGET_MAX) == SNX_A64_FLOW_PLAIN, "CFINV decodes with every feature");
    check(snx_a64_flow(0xD502401FUL, SNX_A64_TARGET_MAX) == SNX_A64_FLOW_UNDEF, "unallocated MSR immediate is UNDEF");
    for (i = 0; i < sizeof(backwards) / sizeof(backwards[0]); ++i) {
        snx_a64_decode_word(backwards[i], 0, &insn);
        check(snx_a64_branch_target(&insn, 0x1000) == 0xFFC, "sign-extend negative branch offset");
        check(snx_a64_branch_target(&insn, 0) == ~(SnxU64)3, "wrap negative branch target to 64 bits");
    }
}

static int has_leader(const SnxFunction* function, SnxU64 address) {
    size_t i;
    for (i = 0; i < function->count; ++i)
        if (snx_function_address(function, i) == address) return function->leaders[i] != 0;
    return 0;
}

static void setup_program(SnxProgram* program, SnxModuleImage* module, unsigned char* image, size_t size) {
    memset(program, 0, sizeof(*program));
    memset(module, 0, sizeof(*module));
    module->name = "synthetic";
    module->image = image;
    module->image_size = size;
    module->base = SNX_CODE_BASE;
    module->size = 0x1000;
    module->text_size = (SnxU32)size;
    program->modules = module;
    program->module_count = 1;
    program->npdm.title_id = 0x123456789ABCDEF0ull;
    program->npdm.stack = 0x100000;
    program->npdm.prio = 44;
    program->npdm.core = 2;
}

static void analysis_test(void) {
    static const SnxU32 words[] = {0xAA1E03F3UL, 0x91000400UL, 0x9400000AUL, 0xF1002C1FUL, 0x540000A1UL,
                                   0x91000800UL, 0xAA1303FEUL, 0xD65F03C0UL, 0xD503201FUL, 0x91019000UL,
                                   0x17FFFFFCUL, 0xD4200000UL, 0x91002800UL, 0xD65F03C0UL};
    unsigned char image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde ranges[2];
    SnxU64 interior, symbol;
    const SnxFunction* function;
    size_t i;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    ranges[0].start = 0;
    ranges[0].size = 44;
    ranges[1].start = 20;
    ranges[1].size = 8;
    module.fde = ranges;
    module.fde_count = 2;
    symbol = 20;
    module.functions = &symbol;
    module.function_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze FDE and BL target");
    check(codegen.function_count == 2, "overlapping FDE discarded, BL function discovered");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && function->addresses == NULL && function->count == 11, "FDE uses a compact address range");
    if (function) {
        check(!has_leader(function, SNX_CODE_BASE + 20), "discarded FDE start is not an extra symbol seed");
        check(has_leader(function, SNX_CODE_BASE + 12), "BL continuation leader");
        check(has_leader(function, SNX_CODE_BASE + 24), "backwards B target leader");
        check(has_leader(function, SNX_CODE_BASE + 36), "conditional target leader");
    }
    check(snx_codegen_function(&codegen, SNX_CODE_BASE + 48) != NULL, "recursive BL target exists");
    snx_codegen_free(&codegen);
    interior = SNX_CODE_BASE + 20;
    check(!snx_nx_codegen_analyze(&codegen, &program, &interior, 1, 1), "analyze explicit interior hint");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && has_leader(function, interior), "execution hint adds interior dispatch entry");
    snx_codegen_free(&codegen);
}

/* Function pointers built in code must be compiled even without symbols or unwind information. */
static void address_taken_test(void) {
    unsigned char image[80];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde range = {0};
    const SnxFunction* function;
    size_t i;
    int with_fde;
    for (i = 0; i < sizeof(image); i += 4) put32(image, i, 0xD4200000UL);
    put32(image, 0, 0x10000100UL);  /* ADR x0, +32: callback */
    put32(image, 4, 0x10000041UL);  /* ADR x1, +8: interior entry at 12 */
    put32(image, 8, 0xD503201FUL);  /* NOP */
    put32(image, 12, 0xD65F03C0UL); /* RET */
    put32(image, 32, 0x90000002UL); /* ADRP x2, current page */
    put32(image, 36, 0x91010042UL); /* ADD x2, x2, #64: another callback */
    put32(image, 40, 0xD65F03C0UL);
    put32(image, 64, 0xD65F03C0UL);
    for (with_fde = 0; with_fde < 2; ++with_fde) {
        setup_program(&program, &module, image, sizeof(image));
        range.start = 0;
        range.size = 16;
        module.fde = with_fde ? &range : NULL;
        module.fde_count = with_fde;
        check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze address-taken callbacks");
        check(snx_codegen_function(&codegen, SNX_CODE_BASE + 32) != NULL, "ADR callback discovered");
        check(snx_codegen_function(&codegen, SNX_CODE_BASE + 64) != NULL, "ADRP/ADD callback discovered");
        function = snx_codegen_function(&codegen, SNX_CODE_BASE);
        check(function && has_leader(function, SNX_CODE_BASE + 12), "interior address becomes a leader");
        check(codegen.function_count == 3, "interior address does not duplicate a function");
        snx_codegen_free(&codegen);
    }
}

/* ADRP and an ADD of its page offset shifted by 12 (ADD Xd, Xn, #imm, LSL #12): the callback a page further on */
static void address_taken_shifted_test(void) {
    static unsigned char image[0x1010];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    size_t i;
    for (i = 0; i < sizeof(image); i += 4) put32(image, i, 0xD4200000UL);
    put32(image, 0, 0x90000003UL);      /* ADRP x3, current page */
    put32(image, 4, 0x91400463UL);      /* ADD x3, x3, #1, LSL #12: the next page */
    put32(image, 8, 0xD65F03C0UL);      /* RET */
    put32(image, 0x1000, 0xD65F03C0UL); /* the callback */
    setup_program(&program, &module, image, sizeof(image));
    module.size = 0x2000;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze a shifted ADD");
    check(snx_codegen_function(&codegen, SNX_CODE_BASE + 0x1000) != NULL, "ADRP/ADD LSL #12 callback discovered");
    snx_codegen_free(&codegen);
}

static void large_range_test(void) {
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde range;
    unsigned char* image;
    size_t i, size;
    size = 1024 * 1024;
    image = (unsigned char*)snx_alloc(size);
    for (i = 0; i < size; i += 4) put32(image, i, 0xD503201FUL);
    put32(image, size - 4, 0xD65F03C0UL);
    setup_program(&program, &module, image, size);
    range.start = 0;
    range.size = size;
    module.fde = &range;
    module.fde_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze a large FDE");
    check(codegen.function_count == 1 && codegen.functions[0].addresses == NULL &&
              codegen.functions[0].count == size / 4 && codegen.functions[0].leader_count == 1,
          "large FDE stays a range, without one address allocation per instruction");
    snx_codegen_free(&codegen);
    free(image);
}

/* the semantic cases: words with their starting state and expected end, as LLVM steps at pc 0x1000 */
static const SnxU32 g_words[] = {
    0x31000420UL, /* ADDS W0,W1,#1: zero-extension and NZCV. */
    0xBA1F03E0UL, /* ADCS X0,XZR,XZR: integer carry input. */
    0xDA1F03E0UL, /* SBC X0,XZR,XZR: 64-bit complement of zero. */
    0xD3485C20UL, /* UBFX X0,X1,#8,#16. */
    0x93401C20UL, /* SXTB X0,W1. */
    0x93C12020UL, /* ROR X0,X1,#8. */
    0x9E220020UL, /* SCVTF S0,X1: direct int64 -> float32 rounding. */
    0x9E660020UL, /* FMOV X0,D1: preserve NaN payload bits. */
    0x1E620C20UL, /* FCSEL D0,D1,D2,EQ: preserve selected bits. */
    0x4EA28420UL, /* ADD V0.4S,V1.4S,V2.4S. */
    0x4E22CC20UL, /* FMLA V0.4S,V1.4S,V2.4S. */
    0x4E040C20UL, /* DUP V0.4S,W1. */
    0x4E183C20UL, /* UMOV X0,V1.D[1]. */
    0x5FA21820UL, /* FMLA S0,S1,V2.S[3]: scalar upper-lane clearing. */
    0xF94007E0UL, /* LDR X0,[SP,#8]. */
    0xF900043FUL, /* STR XZR,[X1,#8]. */
    0x17FFFFFFUL, /* B pc-4. */
    0x1200FC00UL, /* Invalid logical immediate: UNDEF. */
    0x1E221820UL, /* FDIV S0,S1,S2: architectural invalid NaN and IOC. */
    0x1F020C20UL, /* FMADD S0,S1,S2,S3: addend-first NaN priority. */
    0x1E22C020UL, /* FCVT D0,S1: inspect signalling before widening. */
    0x1E200020UL, /* FCVTNS W0,S1: QNaN is invalid for integer conversion. */
    0x4E22D420UL, /* FADD V0.4S,V1.4S,V2.4S: guest rounding. */
    0x5FA21820UL, /* FMLA S0,S1,V2.S[3]: by-element SNaN priority. */
    0x1E63C020UL, /* FCVT H0,D1: alternative half infinity conversion. */
    0x9E580020UL, /* FCVTZS X0,D1,#64: scaling before integer saturation. */
    0x1E61C020UL, /* FSQRT D0,D1: directed rounding and IXC. */
    0x1E228820UL, /* FNMUL S0,S1,S2: negate the architectural NaN result. */
    0x1E23C020UL, /* FCVT H0,S1: rounds in the FPCR mode, here toward plus infinity. */
    0x1E63C020UL, /* FCVT H0,D1: a negative overflow toward plus infinity is the largest finite. */
    0x0E216820UL, /* FCVTN V0.4H,V1.4S toward zero: truncates, overflows finite, keeps -0. */
    0x1E23C020UL, /* FCVT H0,S1: FZ flushes a denormal operand before rounding, and sets IDC. */
    0x1F420C20UL, /* FMADD D0,D1,D2,D3: inf*0 gives the default NaN. */
    0x1F620C20UL, /* FNMADD D0,D1,D2,D3: of two quiet NaNs, the first (negated). */
    0x4E62CC20UL, /* FMLA V0.2D,V1.2D,V2.2D: the same, per lane. */
    0x7EE1D820UL, /* FRSQRTE D0,D1: FZ flushes a denormal operand, so +inf and IDC. */
    0x4EA1D820UL, /* FRECPE V0.4S,V1.4S: the same per lane, with the sign. */
    0x4EA1D820UL, /* FRECPE V0.4S,V1.4S without FZ: denormals get an estimate. */
    0x4EA1D820UL, /* FRECPE V0.4S,V1.4S under FZ: from 2^126 up the estimate is a zero, UFC. */
    0x4EA1D820UL, /* FRECPE V0.4S,V1.4S under RP: a tiny operand overflows to +inf or -max, OFC and IXC. */
    0x4EE1D820UL, /* FRECPE V0.2D,V1.2D under RZ: to the largest finite. */
    0x5EE1D820UL, /* FRECPE D0,D1 under FZ: from 2^1022 up, a zero. */
    0x4EA1D820UL, /* FRECPE V0.4S,V1.4S under RM: -inf or +max. */
    0x9E630020UL, /* UCVTF D0,X1: 0 is +0 under round toward minus infinity. */
    0x6E61D820UL, /* UCVTF V0.2D,V1.2D: the same per lane; 2^63+1 rounds down. */
    0x9E630020UL, /* UCVTF D0,X1: 2^63+1 rounds up, once, toward plus infinity. */
    0x4E218800UL, /* FRINTN V0.4S,V0.4S: SNaN quieted, signed zero, tie to even, QNaN kept, cumulative IOC. */
    0x4E22CC20UL, /* FMLA V0.4S,V1.4S,V2.4S: QNaN addend with inf*0, fused overflow cancelled; FPSR's NZCV, QC kept. */
    0x6E20D400UL, /* FADDP V0.4S,V0.4S,V0.4S: tie to even, SNaN priority, the destination its own source. */
    0x6E61D820UL, /* UCVTF V0.2D,V1.2D: lanes that fit, 0 is +0 under round toward minus infinity. */
    0x6E21D820UL, /* UCVTF V0.4S,V1.4S: the same for singles. */
    0x7EA1D820UL, /* FRSQRTE S0,S1: an even exponent's estimate, the upper lanes cleared. */
    0x4EA1D820UL  /* FRECPE V0.4S,V1.4S under FZ: the estimates of normals, the smallest normal's of 2^126. */
};
#define WORDS (sizeof(g_words) / sizeof(g_words[0]))

static void setf(V128* v, unsigned i, float x) {
    SnxU32 b;
    memcpy(&b, &x, 4);
    SNX_VSET_S(*v, i, b);
}
static float getf(const V128* v, unsigned i) {
    SnxU32 b = SNX_VS(*v, i);
    float x;
    memcpy(&x, &b, 4);
    return x;
}

static void setup(unsigned i, CPU* c, unsigned char* memory) {
    unsigned j;
    u64 value;
    switch (i) {
        case 0: c->x[1] = 0xFFFFFFFFULL; break;
        case 1: c->cf = 1; break;
        case 2: c->cf = 0; break;
        case 3: c->x[1] = 0x12345678; break;
        case 4: c->x[1] = 0x80; break;
        case 5: c->x[1] = 0x123456789ABCDEF0ULL; break;
        case 6: c->x[1] = 16777217; break;
        case 7: SNX_VSET_D(c->v[1], 0, 0x7FF8000000001234ULL); break;
        case 8:
            c->z = 1;
            SNX_VSET_D(c->v[1], 0, 0x7FF8000000001234ULL);
            SNX_VSET_D(c->v[2], 0, 0x3FF0000000000000ULL);
            break;
        case 9:
            for (j = 0; j < 4; j++) {
                SNX_VSET_S(c->v[1], j, 0xFFFFFFFFu);
                SNX_VSET_S(c->v[2], j, j + 1);
            }
            break;
        case 10:
            for (j = 0; j < 4; j++) {
                setf(&c->v[0], j, 3);
                setf(&c->v[1], j, 4);
                setf(&c->v[2], j, 5);
            }
            break;
        case 11: c->x[1] = 0x123456789ABCDEF0ULL; break;
        case 12: SNX_VSET_D(c->v[1], 1, 0xFEDCBA9876543210ULL); break;
        case 13:
            SNX_VSET_D(c->v[0], 1, ~0ULL);
            setf(&c->v[0], 0, 3);
            setf(&c->v[1], 0, 4);
            setf(&c->v[2], 3, 5);
            break;
        case 14:
            c->x[31] = 16;
            value = SNX_GUEST64(0x123456789ABCDEF0ULL);
            memcpy(memory + 24, &value, 8);
            break;
        case 15:
            c->x[1] = 16;
            c->x[31] = 0xFFFFFFFF;
            break;
        case 18: SNX_VSET_S(c->v[2], 0, 0x80000000); break;
        case 19:
            SNX_VSET_S(c->v[1], 0, 0xFFC00456);
            SNX_VSET_S(c->v[2], 0, 0x7FC00789);
            SNX_VSET_S(c->v[3], 0, 0x7FC00123);
            break;
        case 20: SNX_VSET_S(c->v[1], 0, 0x7F800456); break;
        case 21:
            c->x[0] = ~0ULL;
            SNX_VSET_S(c->v[1], 0, 0xFFC00123);
            break;
        case 22:
            c->fpcr = 1u << 22;
            for (j = 0; j < 4; j++) {
                SNX_VSET_S(c->v[1], j, 0x3F800000);
                SNX_VSET_S(c->v[2], j, 0x33800000);
            }
            break;
        case 23:
            SNX_VSET_S(c->v[0], 0, 0x7FC00123);
            SNX_VSET_D(c->v[0], 1, ~0ULL);
            SNX_VSET_S(c->v[1], 0, 0xFF800456);
            SNX_VSET_S(c->v[2], 3, 0x7FC00ABC);
            c->fpsr = 0x08000010;
            break;
        case 24:
            c->fpcr = 1u << 26;
            SNX_VSET_D(c->v[1], 0, 0x7FF0000000000000ULL);
            break;
        case 25: SNX_VSET_D(c->v[1], 0, 0x3FE0000000000000ULL); break;
        case 26:
            c->fpcr = 3u << 22;
            SNX_VSET_D(c->v[1], 0, 0x4000000000000000ULL);
            break;
        case 27: SNX_VSET_S(c->v[2], 0, 0x7F800000); break;
        /* FCVT and FCVTN to half: the FPCR mode, overflow and FZ */
        case 28:
            c->fpcr = 0x400000;
            SNX_VSET_S(c->v[1], 0, 0x3F800001u);
            break;
        case 29:
            c->fpcr = 0x400000;
            SNX_VSET_D(c->v[1], 0, 0xC202A05F20000000ULL);
            break;
        case 30:
            c->fpcr = 0xC00000;
            SNX_VSET_D(c->v[0], 0, ~0ULL);
            SNX_VSET_D(c->v[0], 1, ~0ULL);
            SNX_VSET_S(c->v[1], 0, 0x3F801FFFu);
            SNX_VSET_S(c->v[1], 1, 0x477FF000u);
            SNX_VSET_S(c->v[1], 2, 0x33000001u);
            SNX_VSET_S(c->v[1], 3, 0x80000000u);
            break;
        case 31:
            c->fpcr = 0x1400000;
            SNX_VSET_S(c->v[1], 0, 1);
            break;
        /* fused multiply-add NaNs in f64 */
        case 32:
            SNX_VSET_D(c->v[1], 0, 0x7FF0000000000000ULL);
            SNX_VSET_D(c->v[3], 0, 0x3FF0000000000000ULL);
            break;
        case 33:
            SNX_VSET_D(c->v[1], 0, 0x7FF8000000000000ULL);
            SNX_VSET_D(c->v[2], 0, ~0ULL);
            SNX_VSET_D(c->v[3], 0, 0x3FF0000000000000ULL);
            break;
        case 34:
            SNX_VSET_D(c->v[0], 0, 0x3FF0000000000000ULL);
            SNX_VSET_D(c->v[0], 1, 0x3FF0000000000000ULL);
            SNX_VSET_D(c->v[1], 0, 0x7FF0000000000000ULL);
            SNX_VSET_D(c->v[1], 1, 0x7FF8000000000001ULL);
            SNX_VSET_D(c->v[2], 1, 0xFFF8000000000002ULL);
            break;
        /* FRECPE and FRSQRTE: FZ and the FPCR mode */
        case 35:
            c->fpcr = 0x1000000;
            SNX_VSET_D(c->v[1], 0, 1);
            break;
        case 36:
        case 37:
            c->fpcr = i == 36 ? 0x1000000 : 0;
            SNX_VSET_S(c->v[1], 0, 0x00400000u);
            SNX_VSET_S(c->v[1], 1, 0x80400000u);
            SNX_VSET_S(c->v[1], 2, 0x007FFFFFu);
            SNX_VSET_S(c->v[1], 3, 0x3F800000u);
            break;
        case 38:
            c->fpcr = 0x1000000;
            SNX_VSET_S(c->v[1], 0, 0x7E800000u);
            SNX_VSET_S(c->v[1], 1, 0xFF000000u);
            SNX_VSET_S(c->v[1], 2, 0x7E7FFFFFu);
            SNX_VSET_S(c->v[1], 3, 0x3F800000u);
            break;
        case 39:
            c->fpcr = 0x400000;
            SNX_VSET_S(c->v[1], 0, 1);
            SNX_VSET_S(c->v[1], 1, 0x80000001u);
            SNX_VSET_S(c->v[1], 2, 0x001FFFFFu);
            SNX_VSET_S(c->v[1], 3, 0x00200000u);
            break;
        case 40:
            c->fpcr = 0xC00000;
            SNX_VSET_D(c->v[1], 0, 1);
            SNX_VSET_D(c->v[1], 1, 0x7FD0000000000000ULL);
            break;
        case 41:
            c->fpcr = 0x1000000;
            SNX_VSET_D(c->v[1], 0, 0xFFD0000000000000ULL);
            break;
        case 42:
            c->fpcr = 0x800000;
            SNX_VSET_S(c->v[1], 0, 1);
            SNX_VSET_S(c->v[1], 1, 0x80000001u);
            SNX_VSET_S(c->v[1], 2, 0x7E800000u);
            SNX_VSET_S(c->v[1], 3, 0x3F800000u);
            break;
        /* UCVTF of 0 and of 2^63+1 in the directed modes */
        case 43: c->fpcr = 0x800000; break;
        case 44:
            c->fpcr = 0x800000;
            SNX_VSET_D(c->v[1], 1, 0x8000000000000001ULL);
            break;
        case 45:
            c->fpcr = 0x400000;
            c->x[1] = 0x8000000000000001ULL;
            break;
        /* the FP witnesses' vector cases (a64_fp_test.c, a64_fp_arithmetic_test.c) */
        case 46:
            SNX_VSET_S(c->v[0], 0, 0x7F800123);
            SNX_VSET_S(c->v[0], 1, 0xBF000000);
            SNX_VSET_S(c->v[0], 2, 0x3FC00000);
            SNX_VSET_S(c->v[0], 3, 0xFFC00456);
            break;
        case 47:
            c->fpsr = 0xA8000000u;
            c->n = c->cf = 1;
            SNX_VSET_S(c->v[0], 0, 0x7FC00123);
            SNX_VSET_S(c->v[1], 0, 0x7F800000);
            SNX_VSET_S(c->v[2], 0, 0);
            SNX_VSET_S(c->v[0], 1, 0xFF7FFFFF);
            SNX_VSET_S(c->v[1], 1, 0x7F7FFFFF);
            SNX_VSET_S(c->v[2], 1, 0x40000000);
            break;
        case 48:
            c->fpsr = 0xA8000000u;
            c->n = c->cf = 1;
            SNX_VSET_S(c->v[0], 0, 0x3F800000);
            SNX_VSET_S(c->v[0], 1, 0x33800000);
            SNX_VSET_S(c->v[0], 2, 0x7FC00123);
            SNX_VSET_S(c->v[0], 3, 0x7F800456);
            break;
        /* UCVTF of lanes that fit, 0 among them, in round toward minus infinity */
        case 49:
            c->fpcr = 0x800000;
            SNX_VSET_D(c->v[1], 1, 0x1FFFFFFFFFFFFFULL);
            break;
        case 50:
            c->fpcr = 0x800000;
            SNX_VSET_S(c->v[1], 1, 1);
            SNX_VSET_S(c->v[1], 2, 0xFFFFFF);
            SNX_VSET_S(c->v[1], 3, 0x800000);
            break;
        /* FRSQRTE and FRECPE's fast paths, which must not depend on the host's rounding mode */
        case 51:
            SNX_VSET_S(c->v[0], 2, 0x12345678);
            SNX_VSET_S(c->v[1], 0, 0x40800001);
            break;
        case 52:
            c->fpcr = 0x1000000;
            SNX_VSET_S(c->v[1], 0, 0x3FC00000);
            SNX_VSET_S(c->v[1], 1, 0xC2F6E979);
            SNX_VSET_S(c->v[1], 2, 0x00800000);
            SNX_VSET_S(c->v[1], 3, 0x7E7FFFFF);
            break;
        default: break;
    }
}

static int verify(unsigned i, const CPU* c, const unsigned char* memory) {
    unsigned j;
    u64 value;
    int ok = 1;
    switch (i) {
        case 0: ok = c->x[0] == 0 && c->z == 1 && c->cf == 1 && !c->n && !c->vf; break;
        case 1: ok = c->x[0] == 1; break;
        case 2: ok = c->x[0] == ~0ULL; break;
        case 3: ok = c->x[0] == 0x3456; break;
        case 4: ok = c->x[0] == ~127ULL; break;
        case 5: ok = c->x[0] == 0xF0123456789ABCDEULL; break;
        case 6: ok = SNX_VS(c->v[0], 0) == 0x4B800000u && !SNX_VD(c->v[0], 1) && c->fpsr == 0x10; break;
        case 7: ok = c->x[0] == 0x7FF8000000001234ULL; break;
        case 8: ok = SNX_VD(c->v[0], 0) == 0x7FF8000000001234ULL && !SNX_VD(c->v[0], 1); break;
        case 9:
            for (j = 0; j < 4; j++) ok &= SNX_VS(c->v[0], j) == j;
            break;
        case 10:
            for (j = 0; j < 4; j++) ok &= getf(&c->v[0], j) == 23;
            break;
        case 11:
            for (j = 0; j < 4; j++) ok &= SNX_VS(c->v[0], j) == 0x9ABCDEF0u;
            break;
        case 12: ok = c->x[0] == 0xFEDCBA9876543210ULL; break;
        case 13: ok = getf(&c->v[0], 0) == 23 && !SNX_VS(c->v[0], 1) && !SNX_VD(c->v[0], 1); break;
        case 14: ok = c->x[0] == 0x123456789ABCDEF0ULL; break;
        case 15:
            memcpy(&value, memory + 24, 8);
            ok = value == 0 && c->x[31] == 0xFFFFFFFF;
            break;
        case 18: ok = SNX_VS(c->v[0], 0) == 0x7FC00000 && c->fpsr == 1; break;
        case 19: ok = SNX_VS(c->v[0], 0) == 0x7FC00123 && c->fpsr == 0; break;
        case 20: ok = SNX_VD(c->v[0], 0) == 0x7FF8008AC0000000ULL && c->fpsr == 1; break;
        case 21: ok = c->x[0] == 0 && c->fpsr == 1; break;
        case 22:
            for (j = 0; j < 4; j++) ok &= SNX_VS(c->v[0], j) == 0x3F800001;
            ok &= c->fpsr == 0x10;
            break;
        case 23:
            ok = SNX_VS(c->v[0], 0) == 0xFFC00456 && !SNX_VS(c->v[0], 1) && !SNX_VD(c->v[0], 1) &&
                 c->fpsr == 0x08000011;
            break;
        case 24: ok = SNX_VD(c->v[0], 0) == 0x7FFF && !SNX_VD(c->v[0], 1) && c->fpsr == 1; break;
        case 25: ok = c->x[0] == 0x7FFFFFFFFFFFFFFFULL && c->fpsr == 1; break;
        case 26: ok = SNX_VD(c->v[0], 0) == 0x3FF6A09E667F3BCCULL && c->fpsr == 0x10; break;
        case 27: ok = SNX_VS(c->v[0], 0) == 0xFFC00000 && c->fpsr == 1; break;
        case 28: ok = SNX_VD(c->v[0], 0) == 0x3C01 && !SNX_VD(c->v[0], 1); break;
        case 29: ok = SNX_VD(c->v[0], 0) == 0xFBFF && !SNX_VD(c->v[0], 1); break;
        case 30: ok = SNX_VD(c->v[0], 0) == 0x800000007BFF3C00ULL && !SNX_VD(c->v[0], 1); break;
        case 31: ok = !SNX_VD(c->v[0], 0) && !SNX_VD(c->v[0], 1) && (c->fpsr & 0x80); break;
        case 32: ok = SNX_VD(c->v[0], 0) == 0x7FF8000000000000ULL && !SNX_VD(c->v[0], 1); break;
        case 33: ok = SNX_VD(c->v[0], 0) == 0xFFF8000000000000ULL && !SNX_VD(c->v[0], 1); break;
        case 34: ok = SNX_VD(c->v[0], 0) == 0x7FF8000000000000ULL && SNX_VD(c->v[0], 1) == 0x7FF8000000000001ULL; break;
        case 35: ok = SNX_VD(c->v[0], 0) == 0x7FF0000000000000ULL && !SNX_VD(c->v[0], 1) && (c->fpsr & 0x80); break;
        case 36:
            ok = SNX_VD(c->v[0], 0) == 0xFF8000007F800000ULL && SNX_VD(c->v[0], 1) == 0x3F7F80007F800000ULL &&
                 (c->fpsr & 0x80);
            break;
        case 37:
            ok = SNX_VD(c->v[0], 0) == 0xFEFF80007EFF8000ULL && SNX_VD(c->v[0], 1) == 0x3F7F80007E800000ULL &&
                 !c->fpsr;
            break;
        case 38:
            ok = SNX_VD(c->v[0], 0) == 0x8000000000000000ULL && SNX_VD(c->v[0], 1) == 0x3F7F800000800000ULL &&
                 c->fpsr == 8;
            break;
        case 39:
            ok = SNX_VD(c->v[0], 0) == 0xFF7FFFFF7F800000ULL && SNX_VD(c->v[0], 1) == 0x7F7F80007F800000ULL &&
                 c->fpsr == 0x14;
            break;
        case 40:
            ok = SNX_VD(c->v[0], 0) == 0x7FEFFFFFFFFFFFFFULL && SNX_VD(c->v[0], 1) == 0x000FF80000000000ULL &&
                 c->fpsr == 0x14;
            break;
        case 41: ok = SNX_VD(c->v[0], 0) == 0x8000000000000000ULL && !SNX_VD(c->v[0], 1) && c->fpsr == 8; break;
        case 42:
            ok = SNX_VD(c->v[0], 0) == 0xFF8000007F7FFFFFULL && SNX_VD(c->v[0], 1) == 0x3F7F8000007FC000ULL &&
                 c->fpsr == 0x14;
            break;
        case 43: ok = !SNX_VD(c->v[0], 0) && !SNX_VD(c->v[0], 1); break;
        case 44: ok = !SNX_VD(c->v[0], 0) && SNX_VD(c->v[0], 1) == 0x43E0000000000000ULL; break;
        case 45: ok = SNX_VD(c->v[0], 0) == 0x43E0000000000001ULL && !SNX_VD(c->v[0], 1); break;
        case 46:
            ok = SNX_VS(c->v[0], 0) == 0x7FC00123 && SNX_VS(c->v[0], 1) == 0x80000000 &&
                 SNX_VS(c->v[0], 2) == 0x40000000 && SNX_VS(c->v[0], 3) == 0xFFC00456 && c->fpsr == 1;
            break;
        case 47:
            ok = SNX_VS(c->v[0], 0) == 0x7FC00000 && SNX_VS(c->v[0], 1) == 0x7F7FFFFF && c->fpsr == 0xA8000001u &&
                 get_nzcv(c) == 0xA0000000u;
            break;
        case 48:
            ok = SNX_VS(c->v[0], 0) == 0x3F800000 && SNX_VS(c->v[0], 1) == 0x7FC00456 &&
                 SNX_VS(c->v[0], 2) == SNX_VS(c->v[0], 0) && SNX_VS(c->v[0], 3) == SNX_VS(c->v[0], 1) &&
                 c->fpsr == 0xA8000011u;
            break;
        case 49: ok = !SNX_VD(c->v[0], 0) && SNX_VD(c->v[0], 1) == 0x433FFFFFFFFFFFFFULL && !c->fpsr; break;
        case 50:
            ok = SNX_VD(c->v[0], 0) == 0x3F80000000000000ULL && SNX_VD(c->v[0], 1) == 0x4B0000004B7FFFFFULL && !c->fpsr;
            break;
        case 51: ok = SNX_VD(c->v[0], 0) == 0x3EFF8000ULL && !SNX_VD(c->v[0], 1) && !c->fpsr; break;
        case 52:
            ok = SNX_VD(c->v[0], 0) == 0xBC0500003F2A8000ULL && SNX_VD(c->v[0], 1) == 0x008000007E7F8000ULL && !c->fpsr;
            break;
        default: break;
    }
    return ok;
}

static int g_traps, g_polls;
static void undefined(CPU* c, u64 pc, u32 w) {
    (void)c;
    (void)pc;
    (void)w;
    ++g_traps;
}

/* each word's LLVM step from its starting state under each host rounding mode (the generated code must not depend
   on it), against the expected end: registers, flags, FPSR, memory, where it went and the UNDEF of case 17 */
static void semantics_test(void) {
    static const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    const int original = fegetround();
    LlvmSteps steps;
    char error[1024];
    unsigned i, m;
    if (llvm_steps_begin(&steps, 1, 0, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        check(0, "start the LLVM steps");
        return;
    }
    for (i = 0; i < WORDS; ++i) llvm_steps_add(&steps, g_words[i], 0x1000, 0);
    if (llvm_steps_load(&steps, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        check(0, "compile and load the LLVM steps");
        llvm_steps_end(&steps);
        return;
    }
    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
        check(!fesetround(modes[m]), "set the host rounding mode");
        for (i = 0; i < WORDS; ++i) {
            CPU c;
            RtAPI rt;
            unsigned char memory[64];
            u64 pc;
            int ok;
            memset(&rt, 0, sizeof(rt));
            rt.undef = undefined;
            memset(&c, 0, sizeof(c));
            memset(memory, 0xAA, sizeof(memory));
            c.mem = memory;
            c.rt = &rt;
            g_traps = 0;
            setup(i, &c, memory);
            pc = steps.steps[i](&c);
            ok = verify(i, &c, memory) && pc == (i == 16 ? 0xFFC : i == 17 ? 0x1000 : 0x1004) &&
                 g_traps == (i == 17);
            if (!ok) fprintf(stderr, "semantic case %u (%08lx) under host rounding mode %u failed\n", i,
                             (unsigned long)g_words[i], m);
            check(ok, "an LLVM step's architectural result");
        }
    }
    fesetround(original);
    llvm_steps_end(&steps);
}

/* A loop of generated code runs with CPU.poll set: its backward branch calls rt->poll, the runtime's chance to take
   the core from a thread that makes no SVC, and the loop still runs to its end; without the flag it never polls */
static void on_poll(CPU* c) {
    ++g_polls;
    __atomic_store_n(&c->poll, 0, __ATOMIC_RELAXED);
}
static int run_loop(GuestFn f, u64 count, u32 flag) {
    CPU c;
    RtAPI rt;
    memset(&c, 0, sizeof(c));
    memset(&rt, 0, sizeof(rt));
    rt.poll = on_poll;
    rt.undef = undefined;
    c.rt = &rt;
    c.x[0] = count;
    c.x[30] = 0x2000;
    c.poll = flag;
    g_polls = 0;
    f(&c, SNX_CODE_BASE);
    return c.x[0] == 0 && c.pc == 0x2000 ? g_polls : -1;
}

static void poll_test(void) {
    static const SnxU32 words[] = {0xF1000400UL,  /* SUBS X0,X0,#1 */
                                   0x54FFFFE1UL,  /* B.NE back to it */
                                   0xD65F03C0UL}; /* RET */
    unsigned char image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde range;
    const SnxFunction* function;
    LlvmSteps steps;
    char error[1024];
    size_t i, index;
    int a, b;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    range.start = 0;
    range.size = sizeof(image);
    module.fde = &range;
    module.fde_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1), "analyze the poll loop");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function != NULL, "the poll loop is one function");
    if (function && !llvm_steps_begin(&steps, 1, 0, error, sizeof(error))) {
        index = llvm_steps_add_function(&steps, &codegen, function);
        if (!llvm_steps_load(&steps, error, sizeof(error))) {
            GuestFn f = (GuestFn)(void (*)(void))steps.steps[index];
            a = run_loop(f, 3, 1);
            b = run_loop(f, 5, 0);
            if (a != 1 || b != 0) fprintf(stderr, "poll loop: %d polls with the flag, %d without\n", a, b);
            check(a == 1 && b == 0, "a loop with CPU.poll set calls rt->poll once and runs to its end");
        } else {
            fprintf(stderr, "%s\n", error);
            check(0, "compile and load the poll loop");
        }
        llvm_steps_end(&steps);
    } else if (function) {
        fprintf(stderr, "%s\n", error);
        check(0, "start the poll loop's module");
    }
    snx_codegen_free(&codegen);
}

int main(void) {
    decoder_test();
    analysis_test();
    address_taken_test();
    address_taken_shifted_test();
    large_range_test();
    semantics_test();
    poll_test();
    printf("Native codegen: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
