/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Synthetic decoder, CFG and code tests: the generated decoder's UNDEF set, flow and branch targets; function
   discovery; architectural results of the native back end's steps (native_steps.c) against expected values; and a
   compiled loop's poll at its backward branch. */
#include "steps.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/cpu/recompiler/codegen.h"
#include "nx/loader/codegen_view.h"
#include <fenv.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

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
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze FDE and BL target");
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
    check(!snx_nx_codegen_analyze(&codegen, &program, &interior, 1, 1, 0), "analyze explicit interior hint");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && has_leader(function, interior), "execution hint adds interior dispatch entry");
    snx_codegen_free(&codegen);
}

/* A hooked function reached by a tail call: with no unwind information the function that ends in a B to it is explored
   first and would take its instructions as its own, so the B became a jump inside one function and never read the
   dispatch slot the hook is in. A routed function is a function of its own, whatever reaches it */
static void routed_tail_call_test(void) {
    /* 0: mov x0, #1; b 12 | 8: ret | 12: mov x0, #2; ret */
    static const SnxU32 words[] = {0xD2800020UL, 0x14000002UL, 0xD65F03C0UL, 0xD2800040UL, 0xD65F03C0UL};
    unsigned char image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 exported = 12;
    const SnxFunction* caller;
    size_t i;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    module.functions = &exported;
    module.function_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a tail call to an exported function");
    caller = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(caller && snx_function_contains(caller, SNX_CODE_BASE + 12), "a function no hook names joins its caller");
    snx_codegen_free(&codegen);
    module.routed = &exported;
    module.routed_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a tail call to a routed function");
    caller = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(caller && caller->count == 2 && !snx_function_contains(caller, SNX_CODE_BASE + 12),
          "the caller ends at its B to the routed function");
    check(snx_codegen_function(&codegen, SNX_CODE_BASE + 12) != NULL, "the routed function is a function of its own");
    check(snx_codegen_routed(&codegen, SNX_CODE_BASE + 12), "its calls go through its dispatch slot");
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
        check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze address-taken callbacks");
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
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a shifted ADD");
    check(snx_codegen_function(&codegen, SNX_CODE_BASE + 0x1000) != NULL, "ADRP/ADD LSL #12 callback discovered");
    snx_codegen_free(&codegen);
}

/* A switch into a table outside the text: compare, bound, the table's address, the entry load, the entry's ADD and
   the BR. Without an FDE the function's discovery stops at the BR, so unless it follows the table the cases are
   neither compiled into the function nor leaders, and the dispatch's default would miss at run time */
static void jump_table_test(void) {
    static const SnxU32 words[] = {
        0x71000C1FUL, /* cmp w0, #3 */
        0x540001E8UL, /* b.hi 0x40: the default */
        0x90000001UL, /* adrp x1, base's page */
        0x91020021UL, /* add x1, x1, #0x80: the table */
        0xB8A05822UL, /* ldrsw x2, [x1, w0, uxtw #2] */
        0x8B020022UL, /* add x2, x1, x2: an entry is a byte offset from the table */
        0xD61F0040UL, /* br x2 */
        0x52800020UL, /* case 0: mov w0, #1 */
        0x1400000AUL, /* b 0x48 */
        0x52800040UL, /* case 1 */
        0x14000008UL, /* b 0x48 */
        0x52800060UL, /* case 2 */
        0x14000006UL, /* b 0x48 */
        0x52800080UL, /* case 3 */
        0x14000004UL, /* b 0x48 */
        0xD503201FUL, /* nop */
        0x52800000UL, /* default: mov w0, #0 */
        0x14000001UL, /* b 0x48 */
        0xD65F03C0UL, /* 0x48: ret */
    };
    static const SnxU64 cases[] = {0x1C, 0x24, 0x2C, 0x34}; /* case 0..3 in the function */
    unsigned char image[0x100];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 targets[8];
    const SnxFunction* function;
    size_t i, n;
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    for (i = 0; i < 4; ++i) put32(image, 0x80 + i * 4, (SnxU32)(cases[i] - 0x80)); /* table: case - table */
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = sizeof(words) * 4; /* the table is .rodata, outside the text */
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a jump table");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    n = snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x18, targets, 8);
    check(n == 4, "the table's four cases");
    for (i = 0; i < n; ++i) check(targets[i] == SNX_CODE_BASE + cases[i], "the case the table's entry names");
    check(function && function->count == 18, "the cases are compiled into the function"); /* the dead NOP stays out */
    for (i = 0; i < 4; ++i) check(function && has_leader(function, SNX_CODE_BASE + cases[i]), "a case is a leader");
    check(!snx_codegen_function(&codegen, SNX_CODE_BASE + 0x80), "the table is not a function");
    /* an entry outside the text is not followed: the bound still holds the four entries, but one is not code */
    snx_codegen_free(&codegen);
    put32(image, 0x88, 0x1000);
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a table with a bad entry");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x18, targets, 8) == 3, "an entry outside the text is skipped");
    check(function && !has_leader(function, SNX_CODE_BASE + 0x2C), "a case outside the text is not a leader");
    snx_codegen_free(&codegen);
}

/* The sign-extended byte table GCC emits when the cases are close: ADR, LDRSB, ADD (SXTW, shift) and BR, the table
   inside the text; the ADD's shift and the load's sign both apply */
static void jump_table_sign_test(void) {
    static const SnxU32 words[] = {
        0x7100041FUL, /* cmp w0, #1 */
        0x54000128UL, /* b.hi 0x28 */
        0x10000142UL, /* adr x2, 0x30: the table */
        0x38E04841UL, /* ldrsb w1, [x2, w0, uxtw] */
        0x8B21C842UL, /* add x2, x2, w1, sxtw #2 */
        0xD61F0040UL, /* br x2 */
        0x52800020UL, /* case 0: mov w0, #1 */
        0xD65F03C0UL, /* ret */
        0x52800040UL, /* case 1: mov w0, #2 */
        0xD65F03C0UL, /* ret */
        0x52800000UL, /* default: mov w0, #0 */
        0xD65F03C0UL, /* ret */
    };
    unsigned char image[0x40];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 targets[8];
    const SnxFunction* function;
    size_t i;
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    image[0x30] = 0xFA; /* (0x18 - 0x30) / 4, as the ADD's #2 scales it */
    image[0x31] = 0xFC; /* (0x20 - 0x30) / 4 */
    setup_program(&program, &module, image, sizeof(image));
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a sign-extended table");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x14, targets, 8) == 2, "the sign-extended table's two cases");
    check(targets[0] == SNX_CODE_BASE + 0x18 && targets[1] == SNX_CODE_BASE + 0x20, "a scaled signed entry's case");
    check(function && has_leader(function, SNX_CODE_BASE + 0x18) && has_leader(function, SNX_CODE_BASE + 0x20),
          "the signed table's cases are leaders");
    check(function && function->count == 12, "the signed table's cases are in the function");
    check(!snx_codegen_function(&codegen, SNX_CODE_BASE + 0x30), "the sign table inside the text is not a function");
    snx_codegen_free(&codegen);
}

/* A thread stopped in an SVC (a save state's) starts again at the instruction after it, through dispatch: the
   function has an entry there */
static void svc_resume_test(void) {
    static const SnxU32 words[] = {
        0xD4000161UL, /* svc #0xb */
        0xD65F03C0UL, /* ret */
    };
    unsigned char image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    const SnxFunction* function;
    size_t i;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze an SVC's function");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && has_leader(function, SNX_CODE_BASE + 4), "the instruction after an SVC is a leader");
    check(snx_codegen_entry(&codegen, SNX_CODE_BASE + 4) == function, "a thread goes on after the SVC in its function");
    snx_codegen_free(&codegen);
}

/* A switch whose cases lie in the function after its own (a compiler's cold part, with its own unwind range): they
   are leaders of that function */
static void jump_table_other_function_test(void) {
    static const SnxU32 words[] = {
        0x7100041FUL, /* cmp w0, #1 */
        0x540000C8UL, /* b.hi 0x1c: the default */
        0x90000001UL, /* adrp x1, base's page */
        0x91020021UL, /* add x1, x1, #0x80: the table */
        0xB8A05822UL, /* ldrsw x2, [x1, w0, uxtw #2] */
        0x8B020022UL, /* add x2, x1, x2 */
        0xD61F0040UL, /* br x2 */
        0xD65F03C0UL, /* 0x1c, default: ret */
        0x52800020UL, /* 0x20, the next function, case 0: mov w0, #1 */
        0x52800040UL, /* 0x24, case 1: mov w0, #2 */
        0xD65F03C0UL, /* ret */
    };
    unsigned char image[0x100];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde ranges[2];
    const SnxFunction* function;
    size_t i;
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    put32(image, 0x80, (SnxU32)(0x20 - 0x80));
    put32(image, 0x84, (SnxU32)(0x24 - 0x80));
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = sizeof(words);
    ranges[0].start = 0;
    ranges[0].size = 0x20;
    ranges[1].start = 0x20;
    ranges[1].size = 0xC;
    module.fde = ranges;
    module.fde_count = 2;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0),
          "analyze a table with cases in the next function");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE + 0x20);
    check(function && has_leader(function, SNX_CODE_BASE + 0x24), "a case in the next function is its leader");
    snx_codegen_free(&codegen);
}

/* LLVM's compressed table: unsigned bytes outside the text, each the number of instructions from an address in the
   code that the function takes with an ADR, not from the table; the first case's move sits between the ADD and the
   BR. The cases are reached by nothing else, so unless the table is followed they have no code at all */
static void jump_table_anchor_test(void) {
    static const SnxU32 words[] = {
        0x7100081FUL, /* cmp w0, #2 */
        0x540001C8UL, /* b.hi 0x3c: the default */
        0x90000009UL, /* adrp x9, base's page */
        0x91020129UL, /* add x9, x9, #0x80: the table */
        0x100000AAUL, /* adr x10, 0x24: what an entry counts from */
        0x3860692BUL, /* ldrb w11, [x9, x0] */
        0x8B0B094AUL, /* add x10, x10, x11, lsl #2 */
        0x2A1F03E8UL, /* mov w8, wzr */
        0xD61F0140UL, /* br x10 */
        0x52800020UL, /* 0x24, case 0: mov w0, #1 */
        0x14000006UL, /* b 0x40 */
        0x52800040UL, /* 0x2c, case 1 */
        0x14000004UL, /* b 0x40 */
        0x52800060UL, /* 0x34, case 2 */
        0x14000002UL, /* b 0x40 */
        0x52800000UL, /* 0x3c, default: mov w0, #0 */
        0xD65F03C0UL, /* 0x40: ret */
    };
    static const SnxU64 cases[] = {0x24, 0x2C, 0x34};
    unsigned char image[0x100];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 targets[8];
    const SnxFunction* function;
    size_t i, n;
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    for (i = 0; i < 3; ++i) image[0x80 + i] = (unsigned char)((cases[i] - 0x24) / 4);
    image[0x83] = 6; /* the next table's byte, past the bound: it would name the default */
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = sizeof(words); /* the table is .rodata, outside the text */
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a table of offsets from the code");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    n = snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x20, targets, 8);
    check(n == 3, "the byte table's three cases, to its bound");
    for (i = 0; i < n && i < 3; ++i)
        check(targets[i] == SNX_CODE_BASE + cases[i], "a case counted in instructions from the ADR's address");
    check(function && function->count == 17, "the byte table's cases are in the function");
    for (i = 0; i < 3; ++i)
        check(function && has_leader(function, SNX_CODE_BASE + cases[i]), "a byte table's case is a leader");
    check(!snx_codegen_function(&codegen, SNX_CODE_BASE + 0x24), "the address entries count from is not a function");
    snx_codegen_free(&codegen);
    /* without the compare every byte would name an instruction of the text: such a table is not followed */
    put32(image, 0, 0xD503201FUL);
    put32(image, 4, 0xD503201FUL);
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a byte table without a bound");
    check(snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x20, targets, 8) == 0, "a byte table without a bound");
    snx_codegen_free(&codegen);
}

/* A byte table no compare guards, its index masked: and x0, x0, #3 bounds it to four entries, as a compare would */
static void jump_table_mask_test(void) {
    static const SnxU32 words[] = {
        0x92400400UL, /* and x0, x0, #3 */
        0xD503201FUL, /* nop */
        0x90000009UL, /* adrp x9, base's page */
        0x91020129UL, /* add x9, x9, #0x80: the table */
        0x100000AAUL, /* adr x10, 0x24: what an entry counts from */
        0x3860692BUL, /* ldrb w11, [x9, x0] */
        0x8B0B094AUL, /* add x10, x10, x11, lsl #2 */
        0x2A1F03E8UL, /* mov w8, wzr */
        0xD61F0140UL, /* br x10 */
        0x52800020UL, /* 0x24, case 0: mov w0, #1 */
        0x14000006UL, /* b 0x40 */
        0x52800040UL, /* 0x2c, case 1 */
        0x14000004UL, /* b 0x40 */
        0x52800060UL, /* 0x34, case 2 */
        0x14000002UL, /* b 0x40 */
        0x52800080UL, /* 0x3c, case 3 */
        0xD65F03C0UL, /* 0x40: ret */
    };
    static const SnxU64 cases[] = {0x24, 0x2C, 0x34, 0x3C};
    unsigned char image[0x100];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 targets[8];
    const SnxFunction* function;
    size_t i, n;
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    for (i = 0; i < 4; ++i) image[0x80 + i] = (unsigned char)((cases[i] - 0x24) / 4);
    image[0x84] = 7; /* past the mask's bound: it would name the RET */
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = sizeof(words);
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a table of a masked index");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    n = snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x20, targets, 8);
    check(n == 4, "the masked byte table's four cases");
    for (i = 0; i < n && i < 4; ++i) check(targets[i] == SNX_CODE_BASE + cases[i], "a masked table's case");
    for (i = 0; i < 4; ++i)
        check(function && has_leader(function, SNX_CODE_BASE + cases[i]), "a masked table's case is a leader");
    snx_codegen_free(&codegen);
    /* a mask of nine bits is no bound a byte table is followed to */
    put32(image, 0, 0x92402000UL); /* and x0, x0, #0x1ff */
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a table of a wide mask");
    check(snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x20, targets, 8) == 0, "a mask too wide for a bound");
    snx_codegen_free(&codegen);
}

/* Tables found from the function's flow, not the code just before the load (llvm-mc, at SNX_CODE_BASE):
   A, the base set before a RET in the layout, the switch reached by a branch around it:
     adrp x14, tbl; add x14, x14, #0x80; cmp w0, #3; b.ls sw; mov w0, #99; ret
     sw: adr x10, c0; ldrb w11, [x14, x0]; add x10, x10, x11, lsl #2; br x10; c0..c3: mov w0, #k; ret
   B, the compare on another block, its guard shared: adrp x9, tbl; add x9, x9, #0x80; cmp w0, #3; b join;
     mov w0, #7; ret; join: b.hi dflt; adr x10, c0; ldrb w11, [x9, x0]; add x10, x10, x11, lsl #2; br x10;
     c0..c3: mov w0, #k; ret; dflt: mov w0, #0; ret
   C, no bound at all (the index the caller's), in an unwind range: adr x9, tbl; adr x10, c0; ldrb w11, [x9, x0];
     add x10, x10, x11, lsl #2; br x10; c0..c2: mov w0, #k; ret; tbl: 0, 2, 4, then 0xff, past the function */
static void jump_table_flow_test(void) {
    static const SnxU32 a[] = {0x9000000EUL, 0x910201CEUL, 0x71000C1FUL, 0x54000069UL, 0x52800C60UL, 0xD65F03C0UL,
                               0x1000008AUL, 0x386069CBUL, 0x8B0B094AUL, 0xD61F0140UL, 0x52800020UL, 0xD65F03C0UL,
                               0x52800040UL, 0xD65F03C0UL, 0x52800060UL, 0xD65F03C0UL, 0x52800080UL, 0xD65F03C0UL};
    static const SnxU32 b[] = {0x90000009UL, 0x91020129UL, 0x71000C1FUL, 0x14000003UL, 0x528000E0UL, 0xD65F03C0UL,
                               0x540001A8UL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x52800020UL,
                               0xD65F03C0UL, 0x52800040UL, 0xD65F03C0UL, 0x52800060UL, 0xD65F03C0UL, 0x52800080UL,
                               0xD65F03C0UL, 0x52800000UL, 0xD65F03C0UL};
    static const SnxU32 c[] = {0x10000169UL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x52800020UL,
                               0xD65F03C0UL, 0x52800040UL, 0xD65F03C0UL, 0x52800060UL, 0xD65F03C0UL};
    static const unsigned char entries[] = {0, 2, 4, 6};
    unsigned char image[0x100];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 targets[8];
    SnxFde range;
    const SnxFunction* function;
    size_t i, n;
    /* A */
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(a) / sizeof(a[0]); ++i) put32(image, i * 4, a[i]);
    memcpy(image + 0x80, entries, 4);
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = sizeof(a); /* the table is .rodata */
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a table set before a RET");
    n = snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x24, targets, 8);
    check(n == 4, "the table whose base the flow gives: its four cases");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    for (i = 0; i < 4; ++i)
        check(n == 4 && targets[i] == SNX_CODE_BASE + 0x28 + 8 * i && function &&
                  has_leader(function, SNX_CODE_BASE + 0x28 + 8 * i),
              "a case of the table the flow gives, a leader of its function");
    snx_codegen_free(&codegen);
    /* B */
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(b) / sizeof(b[0]); ++i) put32(image, i * 4, b[i]);
    memcpy(image + 0x80, entries, 4);
    image[0x84] = 0x0a; /* past the bound: it would name dflt */
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = sizeof(b);
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a table guarded from another block");
    n = snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x28, targets, 8);
    check(n == 4, "the table whose bound the flow gives: its four cases, not the next byte's");
    for (i = 0; i < n && i < 4; ++i) check(targets[i] == SNX_CODE_BASE + 0x2C + 8 * i, "a case of the guarded table");
    snx_codegen_free(&codegen);
    /* C: in its unwind range, to the first entry past the function; explored, not followed */
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(c) / sizeof(c[0]); ++i) put32(image, i * 4, c[i]);
    image[0x2C] = 0, image[0x2D] = 2, image[0x2E] = 4, image[0x2F] = 0xff;
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = 0x30;
    range.start = 0;
    range.size = 0x2C;
    module.fde = &range;
    module.fde_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze an unbounded table in an unwind range");
    n = snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x10, targets, 8);
    check(n == 3, "the unbounded table's cases, to the entry that leaves its function");
    for (i = 0; i < n && i < 3; ++i) check(targets[i] == SNX_CODE_BASE + 0x14 + 8 * i, "a case inside the function");
    snx_codegen_free(&codegen);
    module.fde = NULL;
    module.fde_count = 0;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze it explored");
    check(snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x10, targets, 8) == 0,
          "an unbounded table of an explored function is not followed");
    snx_codegen_free(&codegen);
}

/* The same with halves, the table's address taken before the compare's branch: the code that takes it is not next
   to the load (in a loop it is outside the loop) */
static void jump_table_anchor_far_test(void) {
    static const SnxU32 words[] = {
        0x90000009UL, /* adrp x9, base's page */
        0x91020129UL, /* add x9, x9, #0x80: the table */
        0x7100041FUL, /* cmp w0, #1 */
        0x54000128UL, /* b.hi 0x30: the default */
        0x1000008AUL, /* adr x10, 0x20 */
        0x7860792BUL, /* ldrh w11, [x9, x0, lsl #1] */
        0x8B0B094AUL, /* add x10, x10, x11, lsl #2 */
        0xD61F0140UL, /* br x10 */
        0x52800020UL, /* 0x20, case 0: mov w0, #1 */
        0xD65F03C0UL, /* ret */
        0x52800040UL, /* 0x28, case 1: mov w0, #2 */
        0xD65F03C0UL, /* ret */
        0x52800000UL, /* 0x30, default: mov w0, #0 */
        0xD65F03C0UL, /* ret */
    };
    unsigned char image[0x100];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 targets[8];
    const SnxFunction* function;
    size_t i;
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    image[0x80] = 0; /* case 0 at the ADR's address */
    image[0x82] = 2; /* case 1 two instructions on */
    image[0x84] = 4; /* past the bound */
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = sizeof(words);
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a table whose address is taken early");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x1C, targets, 8) == 2, "the half table's two cases");
    check(targets[0] == SNX_CODE_BASE + 0x20 && targets[1] == SNX_CODE_BASE + 0x28, "a half table's case");
    check(function && function->count == 14 && has_leader(function, SNX_CODE_BASE + 0x28),
          "the half table's cases are leaders of the function");
    snx_codegen_free(&codegen);
}

/* The table's code in a block of its own, which the compare's branch jumps to: the bound is at that branch */
static void jump_table_anchor_branch_test(void) {
    static const SnxU32 words[] = {
        0x7100041FUL, /* cmp w0, #1 */
        0x54000069UL, /* b.ls 0x10: the switch */
        0x52800000UL, /* default: mov w0, #0 */
        0xD65F03C0UL, /* ret */
        0x90000009UL, /* 0x10: adrp x9, base's page */
        0x91020129UL, /* add x9, x9, #0x80: the table */
        0x1000008AUL, /* adr x10, 0x28 */
        0x3860692BUL, /* ldrb w11, [x9, x0] */
        0x8B0B094AUL, /* add x10, x10, x11, lsl #2 */
        0xD61F0140UL, /* br x10 */
        0x52800020UL, /* 0x28, case 0: mov w0, #1 */
        0xD65F03C0UL, /* ret */
        0x52800040UL, /* 0x30, case 1: mov w0, #2 */
        0xD65F03C0UL, /* ret */
    };
    unsigned char image[0x100];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 targets[8];
    const SnxFunction* function;
    size_t i;
    memset(image, 0, sizeof(image));
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    image[0x80] = 0;
    image[0x81] = 2;
    image[0x82] = 1; /* past the bound: it would name case 0's return */
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = sizeof(words);
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a table a compare's branch jumps to");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x24, targets, 8) == 2,
          "the bound at the branch to the table");
    check(targets[0] == SNX_CODE_BASE + 0x28 && targets[1] == SNX_CODE_BASE + 0x30, "the branched-to table's cases");
    check(function && function->count == 14 && has_leader(function, SNX_CODE_BASE + 0x30) &&
              !has_leader(function, SNX_CODE_BASE + 0x2C),
          "the branched-to table's cases are leaders, and nothing past its bound");
    snx_codegen_free(&codegen);
}

/* the host value whose bytes in memory are word's little-endian bytes, as a module's copy of the code holds it */
static SnxU32 copy_word(SnxU32 word) {
    unsigned char bytes[4];
    SnxU32 value;
    put32(bytes, 0, word);
    memcpy(&value, bytes, sizeof(value));
    return value;
}

/* A copy of the code that an update can change while a batch reads it (SnxCodeImage.generation, the recompiler
   library's): mov x0,#0; add x0,x0,#1; nop; ret, then a nop and a ret no function reaches */
enum { COPY_BASE = 0x10000, COPY_WORDS = 8, COPY_FLIP = 2, COPY_FAR = 6 };
static void copy_setup(_Atomic SnxU32* copy, _Atomic SnxU64* generation, SnxCodeImage* module) {
    static const SnxU32 words[COPY_WORDS] = {0xD2800000UL, 0x91000400UL, 0xD503201FUL, 0xD65F03C0UL, 0, 0,
                                             0xD503201FUL, 0xD65F03C0UL};
    for (size_t i = 0; i < COPY_WORDS; ++i) atomic_init(&copy[i], copy_word(words[i]));
    atomic_init(generation, 0);
    *module = (SnxCodeImage){.image = (const unsigned char*)(const void*)copy,
                             .image_size = COPY_WORDS * 4,
                             .base = COPY_BASE,
                             .text_size = COPY_WORDS * 4,
                             .atomic_words = 1,
                             .generation = generation};
}

/* The codegen answers each address of such a copy with the word it read there first: the emitter reads the words
   again after discovery, and a cbnz it read where discovery had read the nop branched to a block discovery never
   made */
static void copy_word_test(void) {
    _Atomic SnxU32 copy[COPY_WORDS];
    _Atomic SnxU64 generation;
    SnxCodeImage module;
    SnxCodegen codegen;
    SnxU64 hint = COPY_BASE;
    SnxU32 word = 0;
    copy_setup(copy, &generation, &module);
    check(!snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, 0), "analyze a copy an update can change");
    atomic_store_explicit(&copy[COPY_FLIP], copy_word(0xB5FFFFFFUL /* cbnz xzr,.-4 */), memory_order_relaxed);
    check(snx_codegen_word(&codegen, COPY_BASE + COPY_FLIP * 4, &word) && word == 0xD503201FUL,
          "a word of the copy updated after the codegen read it reads as it was then");
    snx_codegen_free(&codegen);
}

/* snx_codegen_words_current: an update of a word the codegen read leaves it stale; one that puts the word back,
   writes the same bytes again or changes a word no function reads does not */
static void copy_current_test(void) {
    _Atomic SnxU32 copy[COPY_WORDS];
    _Atomic SnxU64 generation;
    SnxCodeImage module;
    SnxCodegen codegen;
    SnxU64 hint = COPY_BASE;
    copy_setup(copy, &generation, &module);
    check(!snx_codegen_analyze(&codegen, &module, 1, &hint, 1, 1, 0), "analyze a copy an update can change");
    check(snx_codegen_words_current(&codegen), "a copy nothing updated is current");
    atomic_store_explicit(&copy[COPY_FAR], copy_word(0xD2800021UL /* mov x1,#1 */), memory_order_relaxed);
    check(snx_codegen_words_current(&codegen), "a word of the copy no function reads changed: still current");
    atomic_store_explicit(&copy[COPY_FLIP], copy_word(0xD503201FUL), memory_order_relaxed);
    check(snx_codegen_words_current(&codegen), "a word the codegen read written again with its bytes: still current");
    atomic_store_explicit(&copy[COPY_FLIP], copy_word(0xB5FFFFFFUL /* cbnz xzr,.-4 */), memory_order_relaxed);
    check(!snx_codegen_words_current(&codegen), "a word the codegen read changed: stale");
    atomic_store_explicit(&copy[COPY_FLIP], copy_word(0xD503201FUL), memory_order_relaxed);
    check(snx_codegen_words_current(&codegen), "that word put back as the codegen read it: current again");
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
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a large FDE");
    check(codegen.function_count == 1 && codegen.functions[0].addresses == NULL &&
              codegen.functions[0].count == size / 4 && codegen.functions[0].leader_count == 1,
          "large FDE stays a range, without one address allocation per instruction");
    snx_codegen_free(&codegen);
    free(image);
}

/* the semantic cases: words with their starting state and expected end, as native steps at pc 0x1000 */
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
    0x4EA1D820UL, /* FRECPE V0.4S,V1.4S under FZ: the estimates of normals, the smallest normal's of 2^126. */
    0x6E218820UL, /* FRINTA V0.4S,V1.4S: ties away from zero, a tie's neighbours, -0.5 to -1. */
    0x1E664020UL  /* FRINTA D0,D1: the half's predecessor rounds to +0. */
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
        /* FRINTA's, the same */
        case 53:
            SNX_VSET_S(c->v[1], 0, 0xC0200000);
            SNX_VSET_S(c->v[1], 1, 0x3EFFFFFF);
            SNX_VSET_S(c->v[1], 2, 0x4AFFFFFF);
            SNX_VSET_S(c->v[1], 3, 0xBF000000);
            break;
        case 54: SNX_VSET_D(c->v[1], 0, 0x3FDFFFFFFFFFFFFFULL); break;
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
        case 53:
            ok = SNX_VD(c->v[0], 0) == 0x00000000C0400000ULL && SNX_VD(c->v[0], 1) == 0xBF8000004B000000ULL && !c->fpsr;
            break;
        case 54: ok = !SNX_VD(c->v[0], 0) && !SNX_VD(c->v[0], 1) && !c->fpsr; break;
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

/* each word's native step from its starting state under each host rounding mode (the generated code must not depend
   on it), against the expected end: registers, flags, FPSR, memory, where it went and the UNDEF of case 17 */
static void semantics_test(void) {
    static const int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    const int original = fegetround();
    Steps steps;
    char error[1024];
    unsigned i, m;
    if (steps_begin(&steps, 1, 0, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        check(0, "start the native steps");
        return;
    }
    for (i = 0; i < WORDS; ++i) steps_add(&steps, g_words[i], 0x1000, 0);
    if (steps_load(&steps, error, sizeof(error))) {
        fprintf(stderr, "%s\n", error);
        check(0, "compile and load the native steps");
        steps_end(&steps);
        return;
    }
    for (m = 0; m < sizeof(modes) / sizeof(modes[0]); ++m) {
        check(!fesetround(modes[m]), "set the host rounding mode");
        for (i = 0; i < WORDS; ++i) {
            CPU c;
            RtAPI rt = {.cas128 = snx_runtime_cas128};
            unsigned char memory[64];
            u64 pc;
            int ok;
            rt.undef = undefined;
            memset(&c, 0, sizeof(c));
            memset(memory, 0xAA, sizeof(memory));
            c.mem = memory;
            c.rt = &rt;
            g_traps = 0;
            setup(i, &c, memory);
            pc = step_run(steps_at(&steps, i), &c);
            ok = verify(i, &c, memory) && pc == (i == 16 ? 0xFFC : i == 17 ? 0x1000 : 0x1004) &&
                 g_traps == (i == 17);
            if (!ok) fprintf(stderr, "semantic case %u (%08lx) under host rounding mode %u failed\n", i,
                             (unsigned long)g_words[i], m);
            check(ok, "a native step's architectural result");
        }
    }
    fesetround(original);
    steps_end(&steps);
}

/* A loop of generated code runs with CPU.poll set: its backward branch calls rt->poll, the runtime's chance to take
   the core from a thread that makes no SVC, then calls the function again at the loop's head (its entry mismatch and
   dispatch block), and the loop still runs to its end with the registers it had; without the flag it never polls */
static void on_poll(CPU* c) {
    ++g_polls;
    atomic_store_explicit(&c->poll, 0, memory_order_relaxed);
}
static int run_loop(GuestFn f, u64 count, u32 flag) {
    CPU c;
    RtAPI rt = {.cas128 = snx_runtime_cas128};
    memset(&c, 0, sizeof(c));
    rt.poll = on_poll;
    rt.undef = undefined;
    c.rt = &rt;
    c.x[0] = count;
    c.x[30] = 0x2000;
    c.poll = flag;
    g_polls = 0;
    f(&c, SNX_CODE_BASE);
    return c.x[0] == 0 && c.x[1] == count && c.pc == 0x2000 ? g_polls : -1;
}

static void poll_test(void) {
    static const SnxU32 words[] = {0xD2800001UL,  /* MOV X1,#0 */
                                   0x91000421UL,  /* ADD X1,X1,#1: the loop's head, not the function's start */
                                   0xF1000400UL,  /* SUBS X0,X0,#1 */
                                   0x54FFFFC1UL,  /* B.NE back to the ADD */
                                   0xD65F03C0UL}; /* RET */
    unsigned char image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde range;
    const SnxFunction* function;
    Steps steps;
    char error[1024];
    size_t i, index;
    int a, b;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    range.start = 0;
    range.size = sizeof(image);
    module.fde = &range;
    module.fde_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze the poll loop");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function != NULL, "the poll loop is one function");
    if (function && !steps_begin(&steps, 1, 0, error, sizeof(error))) {
        index = steps_add_function(&steps, &codegen, function);
        if (!steps_load(&steps, error, sizeof(error))) {
            GuestFn f = steps_function(&steps, index);
            a = run_loop(f, 3, 1);
            b = run_loop(f, 5, 0);
            if (a != 1 || b != 0) fprintf(stderr, "poll loop: %d polls with the flag, %d without\n", a, b);
            check(a == 1 && b == 0, "a loop with CPU.poll set calls rt->poll once and runs to its end");
        } else {
            fprintf(stderr, "%s\n", error);
            check(0, "compile and load the poll loop");
        }
        steps_end(&steps);
    } else if (function) {
        fprintf(stderr, "%s\n", error);
        check(0, "start the poll loop's module");
    }
    snx_codegen_free(&codegen);
}

/* Synthetic PAuth instructions with SCTLR authentication controls clear. No game code is used. */
static void cpu_extensions_test(void) {
    static const u32 words[] = {
        0xd65f0bffu, 0xd65f0fffu,                           /* RETAA, RETAB */
        0xd61f087fu, 0xd61f0c7fu, 0xd71f0864u, 0xd71f0c64u, /* BR[A/B] with zero/register modifier */
        0xd63f0bdfu, 0xd63f0fdfu, 0xd73f0bc4u, 0xd73f0fc4u, /* BLR[A/B] x30: read before LR write */
        0xd503233fu, 0xd503237fu, 0xd50323bfu, 0xd50323ffu, /* PAC/AUT[A/B]SP */
        0xdac10083u, 0xdac11083u                            /* PACIA/AUTIA x3, x4 */
    };
    for (int jit = 0; jit <= 1; ++jit) {
        Steps steps;
        char error[1024];
        if (steps_begin(&steps, jit, 0, error, sizeof error)) {
            fprintf(stderr, "%s\n", error);
            check(0, "start PAuth steps");
            return;
        }
        for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i)
            steps_add(&steps, words[i], 0x1000, SNX_A64_TARGET_SWITCH_EXTENDED);
        size_t xpaci = steps_add(&steps, 0xdac143e3u, 0x1000, SNX_A64_TARGET_SWITCH_EXTENDED);
        size_t xpacd = steps_add(&steps, 0xdac147e3u, 0x1000, SNX_A64_TARGET_SWITCH_EXTENDED);
        size_t xpaclri = steps_add(&steps, 0xd50320ffu, 0x1000, SNX_A64_TARGET_SWITCH_EXTENDED);
        size_t casal = steps_add(&steps, 0x88e1fc62u, 0x1000, SNX_A64_TARGET_SWITCH_EXTENDED);
        size_t ldaprb = steps_add(&steps, 0x38bfc061u, 0x1000, SNX_A64_TARGET_SWITCH_EXTENDED);
        if (steps_load(&steps, error, sizeof error)) {
            fprintf(stderr, "%s\n", error);
            check(0, "load PAuth steps");
            steps_end(&steps);
            return;
        }
        for (size_t i = 0; i < sizeof words / sizeof words[0]; ++i) {
            RtAPI rt = {.cas128 = snx_runtime_cas128};
            CPU c = {0};
            rt.undef = undefined;
            c.rt = &rt;
            c.x[3] = 0x4560;
            c.x[4] = 0x9870;
            c.x[30] = 0x1230;
            c.x[31] = 0x8000;
            g_traps = 0;
            u64 pc = step_run(steps_at(&steps, i), &c);
            check(pc == (i < 2 || (i >= 6 && i < 10) ? 0x1230
                         : i < 6                     ? 0x4560
                                                     : 0x1004),
                  "PAuth branch target with authentication disabled");
            check(c.x[30] == (i >= 6 && i < 10 ? 0x1004 : 0x1230), "PAuth link register");
            check(c.x[3] == 0x4560 && c.x[4] == 0x9870 && c.x[31] == 0x8000 && !g_traps,
                  "disabled PAuth preserves pointers and modifiers");
        }
        for (int upper = 0; upper <= 1; ++upper) {
            const u64 low = 0x1300001234ull;
            u64 pointer = low | 0x2505430000000000ull | (upper ? 1ull << 55 : 0);
            u64 expected = low | (upper ? 0xffffff8000000000ull : 0);
            RtAPI rt = {.cas128 = snx_runtime_cas128};
            CPU c = {0};
            rt.undef = undefined;
            c.rt = &rt;
            g_traps = 0;
            c.x[3] = pointer;
            check(step_run(steps_at(&steps, xpaci), &c) == 0x1004 && c.x[3] == expected,
                  "XPACI strips PAC for 39-bit VA");
            c.x[3] = pointer;
            check(step_run(steps_at(&steps, xpacd), &c) == 0x1004 && c.x[3] == expected,
                  "XPACD strips PAC for 39-bit VA");
            c.x[30] = pointer;
            check(step_run(steps_at(&steps, xpaclri), &c) == 0x1004 && c.x[30] == expected && !g_traps,
                  "XPACLRI strips PAC from LR without a branch");
        }
        for (int match = 0; match <= 1; ++match) {
            u32 memory = 0; /* aligned backing, stored and checked as guest bytes */
            put32((unsigned char*)&memory, 0, match ? 41 : 43);
            RtAPI rt = {.cas128 = snx_runtime_cas128};
            CPU c = {0};
            rt.undef = undefined;
            c.rt = &rt;
            c.mem = (u8*)&memory;
            c.x[1] = 41;
            c.x[2] = 42;
            g_traps = 0;
            check(step_run(steps_at(&steps, casal), &c) == 0x1004 && !g_traps, "LSE CASAL runs with extended profile");
            unsigned char expected[4];
            put32(expected, 0, match ? 42 : 43);
            check(!memcmp(&memory, expected, 4) && c.x[1] == (match ? 41u : 43u),
                  "LSE CASAL updates memory only when comparison succeeds");
            put32((unsigned char*)&memory, 0, 0x12ab);
            c.x[1] = ~0ull;
            check(step_run(steps_at(&steps, ldaprb), &c) == 0x1004 && c.x[1] == 0xab && !g_traps,
                  "RCpc LDAPRB zero-extends the acquired byte");
        }
        steps_end(&steps);
    }
    unsigned char image[12] = {0};
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    put32(image, 0, 0xd503237fu); /* PACIBSP */
    put32(image, 4, 0xd65f0fffu); /* RETAB */
    setup_program(&program, &module, image, sizeof image);
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, SNX_A64_TARGET_SWITCH_EXTENDED),
          "discover PAuth function");
    const SnxFunction* function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function && function->count == 2, "PAuth return terminates discovery");
    Steps steps;
    char error[1024];
    if (function && !steps_begin(&steps, 1, 0, error, sizeof error)) {
        size_t index = steps_add_function(&steps, &codegen, function);
        if (!steps_load(&steps, error, sizeof error)) {
            GuestFn f = steps_function(&steps, index);
            check(run_loop(f, 0, 0) == 0, "compiled PAuth function returns through x30");
        } else {
            fprintf(stderr, "%s\n", error);
            check(0, "load PAuth function");
        }
        steps_end(&steps);
    } else {
        check(0, "compile PAuth function");
    }
    snx_codegen_free(&codegen);
    check(snx_a64_flow(0x38bfc061u, 0) == SNX_A64_FLOW_UNDEF, "A57 still rejects RCpc LDAPRB");
    check(snx_a64_flow(0x88e1fc62u, 0) == SNX_A64_FLOW_UNDEF, "A57 still rejects LSE CASAL");
    check(snx_a64_flow(0xd65f0fffu, 0) == SNX_A64_FLOW_UNDEF, "A57 still rejects RETAB");
    check(snx_a64_flow(0xdac10083u, 0) == SNX_A64_FLOW_UNDEF, "A57 still rejects PACIA register");
}

/* Synthetic words of the ounce target (the Cortex-A78C) beyond switch_extended: the dot product, FlagM and ESB, in the
   step JIT and the AOT path, and the words the A78C does not decode. No game code is used. */
static void ounce_cpu_test(void) {
    static const u32 words[] = {
        0x6e849427u, /* UDOT v7.4s, v1.16b, v4.16b */
        0xd500401fu, /* CFINV */
        0xba02046au, /* RMIF x3, #4, #0b1010 (N and C) */
        0x3a00086du, /* SETF8 w3 */
        0x3a00486du, /* SETF16 w3 */
        0xd503221fu  /* ESB */
    };
    enum { O_UDOT, O_CFINV, O_RMIF, O_SETF8, O_SETF16, O_ESB, O_WORDS };
    for (int jit = 0; jit <= 1; ++jit) {
        Steps steps;
        char error[1024];
        if (steps_begin(&steps, jit, 0, error, sizeof error)) {
            fprintf(stderr, "%s\n", error);
            check(0, "start Ounce steps");
            return;
        }
        for (size_t i = 0; i < O_WORDS; ++i) steps_add(&steps, words[i], 0x1000, SNX_A64_TARGET_OUNCE);
        if (steps_load(&steps, error, sizeof error)) {
            fprintf(stderr, "%s\n", error);
            check(0, "load Ounce steps");
            steps_end(&steps);
            return;
        }
        RtAPI rt = {.cas128 = snx_runtime_cas128};
        rt.undef = undefined;
        CPU c = {0};
        c.rt = &rt;
        g_traps = 0;
        for (unsigned i = 0; i < 16; ++i) {
            c.v[1].b[i] = (u8)(i + 1);
            c.v[4].b[i] = 0xff;
        }
        for (unsigned i = 0; i < 4; ++i) c.v[7].s[i] = 10 * (i + 1);
        check(step_run(steps_at(&steps, O_UDOT), &c) == 0x1004 && !g_traps, "UDOT runs on the ounce target");
        check(c.v[7].s[0] == 2560 && c.v[7].s[1] == 6650 && c.v[7].s[2] == 10740 && c.v[7].s[3] == 14830,
              "UDOT adds each group of four unsigned byte products to its lane");

        c.n = 1, c.z = 0, c.cf = 0, c.vf = 1;
        check(step_run(steps_at(&steps, O_CFINV), &c) == 0x1004 && c.n == 1 && c.z == 0 && c.cf == 1 && c.vf == 1,
              "CFINV inverts C alone");

        c.n = 0, c.z = 1, c.cf = 0, c.vf = 0;
        c.x[3] = 0xa5; /* rotated right by 4: 1010 in bits 3:0, where the unrotated value has 0101 */
        check(step_run(steps_at(&steps, O_RMIF), &c) == 0x1004 && c.n == 1 && c.z == 1 && c.cf == 1 && c.vf == 0,
              "RMIF sets the masked flags from the rotated bits and keeps the others");

        c.n = 0, c.z = 1, c.cf = 1, c.vf = 1;
        c.x[3] = 0x180; /* bit 7 set, bit 8 set */
        check(step_run(steps_at(&steps, O_SETF8), &c) == 0x1004 && c.n == 1 && c.z == 0 && c.cf == 1 && c.vf == 0,
              "SETF8 sets N, Z and V from bits 8:0 and keeps C");
        c.x[3] = 0x80;
        check(step_run(steps_at(&steps, O_SETF8), &c) == 0x1004 && c.n == 1 && c.vf == 1,
              "SETF8 overflow is bit 8 XOR bit 7");

        c.n = 1, c.z = 0, c.cf = 0, c.vf = 0;
        c.x[3] = 0x10000;
        check(step_run(steps_at(&steps, O_SETF16), &c) == 0x1004 && c.n == 0 && c.z == 1 && c.cf == 0 && c.vf == 1,
              "SETF16 sets N, Z and V from bits 16:0 and keeps C");

        check(step_run(steps_at(&steps, O_ESB), &c) == 0x1004 && !g_traps, "ESB runs as a no-op at EL0");
        steps_end(&steps);
    }
    for (size_t i = 0; i < O_WORDS; ++i)
        check(snx_a64_flow(words[i], SNX_A64_TARGET_SWITCH_EXTENDED) == SNX_A64_FLOW_UNDEF || i == O_ESB,
              "switch_extended rejects the ounce target's extensions");
    /* ISAR1's JSCVT and FCMA are 0, PFR0's DIT is 0, PFR1's SSBS is 1 (no MSR SSBS), and SB and FlagM2 are later
       than the A78C */
    static const u32 refused[] = {
        0x1e7e0000u, /* FJCVTZS w0, d0 */
        0x6e80e400u, /* FCADD v0.4s, v0.4s, v0.4s, #90 */
        0xd503415fu, /* MSR DIT, #1 */
        0xd503413fu, /* MSR SSBS, #1 */
        0xd50330ffu, /* SB */
        0xd500403fu, /* XAFLAG */
        0xd500405fu  /* AXFLAG */
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; ++i)
        check(snx_a64_flow(refused[i], SNX_A64_TARGET_OUNCE) == SNX_A64_FLOW_UNDEF,
              "the ounce target rejects what the Cortex-A78C lacks");
}

/* DC ZVA's helper touches no register (it takes M and the address), so a memset loop of DC ZVA keeps its registers
   in SSA values: no copy to or from the CPU structure around the helper's call. The loop then zeroes the blocks and
   leaves the registers and flags as the instructions say */
static void zva_test(void) {
    static const SnxU32 words[] = {0xD2820001UL,  /* MOV X1,#0x1000 */
                                   0xD2800065UL,  /* MOV X5,#3 */
                                   0xD50B7421UL,  /* DC ZVA,X1: the loop's head */
                                   0x91010021UL,  /* ADD X1,X1,#64 */
                                   0x92403821UL,  /* AND X1,X1,#0x7FFF */
                                   0xCA0100A5UL,  /* EOR X5,X5,X1 */
                                   0x8B0500C6UL,  /* ADD X6,X6,X5 */
                                   0xF1000400UL,  /* SUBS X0,X0,#1 */
                                   0x54FFFF41UL,  /* B.NE back to the DC ZVA */
                                   0xD65F03C0UL}; /* RET */
    unsigned char memory[0x8000], expected[0x8000], image[sizeof(words)];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxFde range;
    const SnxFunction* function;
    Steps steps;
    char error[1024];
    size_t i, index;
    for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
    setup_program(&program, &module, image, sizeof(image));
    range.start = 0;
    range.size = sizeof(image);
    module.fde = &range;
    module.fde_count = 1;
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze the DC ZVA loop");
    function = snx_codegen_function(&codegen, SNX_CODE_BASE);
    check(function != NULL, "the DC ZVA loop is one function");
    if (function && !steps_begin(&steps, 1, 0, error, sizeof(error))) {
        index = steps_add_function(&steps, &codegen, function);
        if (!steps_load(&steps, error, sizeof(error))) {
            GuestFn f = steps_function(&steps, index);
            CPU c = {0};
            RtAPI rt = {.cas128 = snx_runtime_cas128};
            u64 x1 = 0x1000, x5 = 3, x6 = 7, n;
            rt.poll = on_poll;
            rt.undef = undefined;
            memset(memory, 0xA5, sizeof(memory));
            memset(expected, 0xA5, sizeof(expected));
            c.rt = &rt;
            c.mem = memory;
            c.x[0] = 1000; /* more blocks than the 0x8000 bytes hold: the address wraps */
            c.x[6] = x6;
            c.x[30] = 0x2000;
            for (n = 0; n < 1000; ++n) {
                memset(expected + (x1 & ~(u64)63), 0, 64);
                x1 = (x1 + 64) & 0x7FFF;
                x5 ^= x1;
                x6 += x5;
            }
            f(&c, SNX_CODE_BASE);
            if (c.x[1] != x1 || c.x[5] != x5 || c.x[6] != x6 || c.x[0] != 0)
                fprintf(stderr, "dc zva: x0 %llx x1 %llx x5 %llx x6 %llx, expected 0 %llx %llx %llx\n",
                        (unsigned long long)c.x[0], (unsigned long long)c.x[1], (unsigned long long)c.x[5],
                        (unsigned long long)c.x[6], (unsigned long long)x1, (unsigned long long)x5,
                        (unsigned long long)x6);
            check(c.x[0] == 0 && c.x[1] == x1 && c.x[5] == x5 && c.x[6] == x6 && c.pc == 0x2000,
                  "the DC ZVA loop ends with the registers its instructions give");
            check(c.n == 0 && c.z == 1 && c.cf == 1 && c.vf == 0, "the DC ZVA loop ends with SUBS 1-1's flags");
            check(!memcmp(memory, expected, sizeof(memory)), "the DC ZVA loop zeroes its blocks and nothing else");
        } else {
            fprintf(stderr, "%s\n", error);
            check(0, "compile and load the DC ZVA loop");
        }
        steps_end(&steps);
    } else if (function) {
        fprintf(stderr, "%s\n", error);
        check(0, "start the DC ZVA loop's module");
    }
    snx_codegen_free(&codegen);
}

/* What is between the compare and the guard, and what writes the index after them, of a table the flow bounds (llvm-mc):
   adrp x9, tbl; add x9, x9, #0x80; cmp w0, #3; [between]; b join; mov w0, #7; ret; join: b.hi dflt; [after]; adr x10, c0;
   ldrb w11, [x9, x0]; add x10, x10, x11, lsl #2; br x10; c0..c3: mov w0, #k; ret; dflt: mov w0, #0; ret */
static void jump_table_flags_test(void) {
    static const struct {
        SnxU32 between, after;
        int found;
        const char* what;
    } rows[] = {
        {0xD503201FUL, 0xD503201FUL, 1, "NOP sets no flags"},
        {0x1A8700C5UL, 0xD503201FUL, 1, "CSEL w5, w6, w7, eq sets no flags"},
        {0x1B077CC5UL, 0xD503201FUL, 1, "MUL w5, w6, w7 sets no flags"},
        {0x4EA71CE5UL, 0xD503201FUL, 1, "a vector MOV sets no flags"},
        {0x2B0700C5UL, 0xD503201FUL, 0, "ADDS w5, w6, w7 sets them"},
        {0x3A0700C5UL, 0xD503201FUL, 0, "ADCS w5, w6, w7 sets them"},
        {0x3A4008C0UL, 0xD503201FUL, 0, "CCMN w6, #0, #0, eq sets them"},
        {0x1E602000UL, 0xD503201FUL, 0, "FCMP d0, d0 sets them"},
        {0xD503201FUL, 0x1E380000UL, 0, "FCVTZS w0, s0 writes the index"},
        {0xD503201FUL, 0x1E18F000UL, 0, "FCVTZS w0, s0, #4 writes the index"},
        {0xD503201FUL, 0x1E18F005UL, 1, "FCVTZS w5, s0, #4 doesn't"},
    };
    static const unsigned char entries[] = {0, 2, 4, 6, 0x3F};
    size_t k, i;
    for (k = 0; k < sizeof(rows) / sizeof(rows[0]); ++k) {
        const SnxU32 words[] = {0x90000009UL, 0x91020129UL, 0x71000C1FUL,  rows[k].between, 0x14000003UL, 0x528000E0UL,
                                0xD65F03C0UL, 0x540001C8UL, rows[k].after, 0x1000008AUL,    0x3860692BUL, 0x8B0B094AUL,
                                0xD61F0140UL, 0x52800020UL, 0xD65F03C0UL,  0x52800040UL,    0xD65F03C0UL, 0x52800060UL,
                                0xD65F03C0UL, 0x52800080UL, 0xD65F03C0UL,  0x52800000UL,    0xD65F03C0UL};
        unsigned char image[0x100];
        SnxProgram program;
        SnxModuleImage module;
        SnxCodegen codegen;
        SnxU64 targets[8];
        memset(image, 0, sizeof(image));
        for (i = 0; i < sizeof(words) / sizeof(words[0]); ++i) put32(image, i * 4, words[i]);
        memcpy(image + 0x80, entries, sizeof(entries));
        setup_program(&program, &module, image, sizeof(image));
        module.text_size = sizeof(words);
        check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), "analyze a guard and what is before it");
        check(snx_codegen_jump_table(&codegen, SNX_CODE_BASE + 0x30, targets, 8) == (rows[k].found ? 4 : 0),
              rows[k].what);
        snx_codegen_free(&codegen);
    }
}

/* a program of words at SNX_CODE_BASE, its tables' bytes at 0x80 and 0xff past them, in nranges unwind ranges (none:
   explored), analyzed: the BR at each of want's br has these cases, in order (a 0 ends them), each a leader */
typedef struct {
    unsigned br;
    unsigned cases[8];
} TableCases;
static void table_cases(const char* what, const SnxU32* words, size_t n, const unsigned char* table, size_t bytes,
                        SnxFde* ranges, unsigned nranges, const TableCases* want, unsigned nwant) {
    unsigned char image[0x300];
    SnxProgram program;
    SnxModuleImage module;
    SnxCodegen codegen;
    SnxU64 targets[SNX_JUMP_TABLE_MAX];
    char text[160];
    size_t i, k, found;
    memset(image, 0, 0x80);
    memset(image + 0x80, 0xff, sizeof(image) - 0x80);
    for (i = 0; i < n; ++i) put32(image, i * 4, words[i]);
    memcpy(image + 0x80, table, bytes);
    setup_program(&program, &module, image, sizeof(image));
    module.text_size = n * 4;
    module.fde = ranges;
    module.fde_count = nranges;
    snprintf(text, sizeof(text), "%s: analyze", what);
    check(!snx_nx_codegen_analyze(&codegen, &program, NULL, 0, 1, 0), text);
    for (i = 0; i < nwant; ++i) {
        size_t cases = 0;
        found = snx_codegen_jump_table(&codegen, SNX_CODE_BASE + want[i].br, targets, SNX_JUMP_TABLE_MAX);
        while (cases < 8 && want[i].cases[cases]) ++cases;
        snprintf(text, sizeof(text), "%s: the BR at 0x%x has %zu cases, not %zu", what, want[i].br, found, cases);
        check(found == cases, text);
        for (k = 0; k < found && k < cases; ++k) {
            const SnxFunction* f = codegen.functions;
            while (f < codegen.functions + codegen.function_count && !snx_function_contains(f, targets[k])) ++f;
            snprintf(text, sizeof(text), "%s: case %zu of the BR at 0x%x is 0x%llx, not 0x%x", what, k, want[i].br,
                     (unsigned long long)(targets[k] - SNX_CODE_BASE), want[i].cases[k]);
            check(targets[k] == SNX_CODE_BASE + want[i].cases[k], text);
            check(f < codegen.functions + codegen.function_count && has_leader(f, targets[k]), "a case is a leader");
        }
    }
    snx_codegen_free(&codegen);
}

/* A table found by every way to its BR (llvm-mc).
   paths: cmp w1, #1; b.hi d; cbz w3, L2; mov w0, w1 (or w2); b join; L2: mov w0, w2 (or w1); b join; d: mov w0, #0; ret;
     join: adr x10, c0; ldrb w11, [x9, x0]; ...: the index of one way is bounded, the other's isn't, in either order
   nested: cmp w0, #3; b.hi d; adr x10, c0; ldrb w11, [x9, x0]; add x10, x10, x11, lsl #2; br x10; c0: ldrb w1, [x2];
     cmp w1, #3; b.hi d; adr x10, c0; ldrb w11, [x9, x1]; add x10, x10, x11, lsl #2; br x10: a switch in another's case,
     x9 set before both
   other: the same, but case 1 sets x9 to another address and branches to the second switch's case */
static void jump_table_ways_test(void) {
    static const SnxU32 paths_a[] = {0x90000009UL, 0x91020129UL, 0x7100043FUL, 0x540000C8UL, 0x34000063UL, 0x2A0103E0UL,
                                     0x14000005UL, 0x2A0203E0UL, 0x14000003UL, 0x52800000UL, 0xD65F03C0UL, 0x1000008AUL,
                                     0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x52800020UL, 0xD65F03C0UL, 0x52800040UL,
                                     0xD65F03C0UL, 0x52800060UL, 0xD65F03C0UL, 0x52800080UL, 0xD65F03C0UL};
    static const SnxU32 paths_b[] = {0x90000009UL, 0x91020129UL, 0x7100043FUL, 0x540000C8UL, 0x34000063UL, 0x2A0203E0UL,
                                     0x14000005UL, 0x2A0103E0UL, 0x14000003UL, 0x52800000UL, 0xD65F03C0UL, 0x1000008AUL,
                                     0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x52800020UL, 0xD65F03C0UL, 0x52800040UL,
                                     0xD65F03C0UL, 0x52800060UL, 0xD65F03C0UL, 0x52800080UL, 0xD65F03C0UL};
    static const SnxU32 nested[] = {0x90000009UL, 0x91020129UL, 0x71000C1FUL, 0x54000248UL, 0x1000008AUL, 0x3860692BUL,
                                    0x8B0B094AUL, 0xD61F0140UL, 0x39400041UL, 0x71000C3FUL, 0x54000168UL, 0x10FFFFAAUL,
                                    0x3861692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x52800040UL, 0xD65F03C0UL, 0x52800060UL,
                                    0xD65F03C0UL, 0x52800080UL, 0xD65F03C0UL, 0x52800000UL, 0xD65F03C0UL};
    static const SnxU32 other[] = {0x90000009UL, 0x91020129UL, 0x71000C1FUL, 0x54000248UL, 0x1000008AUL, 0x3860692BUL,
                                   0x8B0B094AUL, 0xD61F0140UL, 0x39400041UL, 0x71000C3FUL, 0x54000168UL, 0x10FFFFAAUL,
                                   0x3861692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x10FFFE29UL, 0x17FFFFF8UL, 0x52800060UL,
                                   0xD65F03C0UL, 0x52800080UL, 0xD65F03C0UL, 0x52800000UL, 0xD65F03C0UL};
    static const unsigned char four[] = {0, 2, 4, 6}, in_case[] = {0, 7, 9, 11};
    static SnxFde whole[] = {{0, sizeof(nested)}};
    static const TableCases none[] = {{0x38, {0}}};
    static const TableCases both[] = {{0x1C, {0x20, 0x3C, 0x44, 0x4C}}, {0x38, {0x20, 0x3C, 0x44, 0x4C}}};
    static const TableCases first[] = {{0x1C, {0x20, 0x3C, 0x44, 0x4C}}, {0x38, {0}}};
    table_cases("a way of two with a bound", paths_a, sizeof(paths_a) / 4, four, 4, NULL, 0, none, 1);
    table_cases("the ways swapped", paths_b, sizeof(paths_b) / 4, four, 4, NULL, 0, none, 1);
    table_cases("a switch in a case, explored", nested, sizeof(nested) / 4, in_case, 4, NULL, 0, both, 2);
    table_cases("a switch in a case, in an unwind range", nested, sizeof(nested) / 4, in_case, 4, whole, 1, both, 2);
    table_cases("a base of two addresses", other, sizeof(other) / 4, in_case, 4, whole, 1, first, 2);
}

/* Where a table without a compare ends, in an unwind range (llvm-mc; its cases mov w0, #k; ret, the byte table at 0x80).
   masked: and x0, x0, #7; ...: four entries, and the bytes past them name the function after it
   two: a switch of three cases, its table at 0x80, and one of three, its table at 0x83
   shifted: and w0, w0, #0x7fff; nop; ubfx x1, x0, #13, #19; ...: four values, and a fifth byte names a case's RET */
static void jump_table_end_test(void) {
    static const SnxU32 masked[] = {0x92400800UL, 0x90000009UL, 0x91020129UL, 0x1000008AUL, 0x3860692BUL,
                                    0x8B0B094AUL, 0xD61F0140UL, 0x52800020UL, 0xD65F03C0UL, 0x52800040UL,
                                    0xD65F03C0UL, 0x52800060UL, 0xD65F03C0UL, 0x52800080UL, 0xD65F03C0UL,
                                    0x528000A0UL, 0x11000400UL, 0x11000800UL, 0xD65F03C0UL};
    static const SnxU32 two[] = {0x35000201UL, 0x90000009UL, 0x91020129UL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL,
                                 0xD61F0140UL, 0x52800020UL, 0x11000400UL, 0xD65F03C0UL, 0x52800040UL, 0x11000400UL,
                                 0xD65F03C0UL, 0x52800060UL, 0x11000400UL, 0xD65F03C0UL, 0x90000009UL, 0x91020D29UL,
                                 0x1000008AUL, 0x3862692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x52800080UL, 0xD65F03C0UL,
                                 0x528000A0UL, 0xD65F03C0UL, 0x528000C0UL, 0xD65F03C0UL};
    static const SnxU32 shifted[] = {0x12003800UL, 0xD503201FUL, 0xD34D7C01UL, 0x90000009UL, 0x91020129UL, 0x1000008AUL,
                                     0x3861692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x52800020UL, 0xD65F03C0UL, 0x52800040UL,
                                     0xD65F03C0UL, 0x52800060UL, 0xD65F03C0UL, 0x52800080UL, 0xD65F03C0UL};
    static const unsigned char past[] = {0, 2, 4, 6, 9, 10}, next[] = {0, 3, 6, 0, 2, 4}, five[] = {0, 2, 4, 6, 1};
    static SnxFde ranges[] = {{0, 0x3C}, {0x3C, 0x10}}, all[] = {{0, sizeof(two)}}, shift[] = {{0, sizeof(shifted)}};
    static const TableCases four_cases[] = {{0x18, {0x1C, 0x24, 0x2C, 0x34}}};
    static const TableCases threes[] = {{0x18, {0x1C, 0x28, 0x34}}, {0x54, {0x58, 0x60, 0x68}}};
    static const TableCases fours[] = {{0x20, {0x24, 0x2C, 0x34, 0x3C}}};
    table_cases("a mask wider than the table", masked, sizeof(masked) / 4, past, 6, ranges, 2, four_cases, 1);
    table_cases("a table another follows", two, sizeof(two) / 4, next, 6, all, 1, threes, 2);
    table_cases("a mask and a shift", shifted, sizeof(shifted) / 4, five, 5, shift, 1, fours, 1);
}

/* What the way back from a table's load meets (llvm-mc), each a byte table at 0x80 of cases that are RETs, its guard
   cmp #3; b.hi d but where said.
   others: a switch in another's case; between them ccmp w5, w6, #9, eq; fmov s9, w5; scvtf d9, x5, #4; msr tpidr_el0, x9:
     none writes x9, the base's register
   copied: adrp x9; add x9; mov x12, x9; a switch of x9's, and in its case one of x12's
   apart: adrp x9; nine MOVs; add x9, x9, #0x80
   padded: cbz w5, j; ret; nop; nop; j: ...: nothing comes to j from the NOPs
   landed: cbz w5, j; ret; mov w6, #1; j: ...: something may, where the MOV is a landing pad
   aborted: cbz w5, j; bl f; j: ...: a call that changes x9, not a way the code takes to the load
   argued: cbz w5, j; adrp x1; add x1; b j; ret; j: ... the table in x1, which the caller may have set
   answered: adrp x1; add x1; svc #0; ...: the SVC's x1
   late: a switch of x12's, in its case one of x9's, whose case 2 sets x9 to another address and branches to it
   deep: a switch of x12's, in its case one of x9's, in that one's case another of x9's
   remade: cmp w1, #0; cneg w1, w1, mi; and x1, x1, #3; ...: the compare is of w1 before
   unreachable: five entries, the third the end of the unwind range, another function's start
   longer: cmp w0, #299; b.hi d: 300 entries of four cases, the last alone of the fifth */
static void jump_table_met_test(void) {
    static const SnxU32 others[] = {0x90000009UL, 0x91020129UL, 0x71000C1FUL, 0x54000248UL, 0x1000008AUL, 0x3860692BUL,
                                    0x8B0B094AUL, 0xD61F0140UL, 0x7A4600A9UL, 0x1E2700A9UL, 0x9E42F0A9UL, 0xD51BD049UL,
                                    0x71000C3FUL, 0x54000108UL, 0x10FFFF4AUL, 0x3861692BUL, 0x8B0B094AUL, 0xD61F0140UL,
                                    0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 copied[] = {0x90000009UL, 0x91020129UL, 0xAA0903ECUL, 0x71000C1FUL, 0x540001C8UL,
                                    0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x71000C3FUL,
                                    0x54000108UL, 0x10FFFFCAUL, 0x3861698BUL, 0x8B0B094AUL, 0xD61F0140UL,
                                    0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 apart[] = {0x90000009UL, 0x52800025UL, 0x52800046UL, 0x52800067UL, 0x52800085UL, 0x528000A6UL,
                                   0x528000C7UL, 0x528000E5UL, 0x52800106UL, 0x52800127UL, 0x91020129UL, 0x71000C1FUL,
                                   0x54000128UL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL,
                                   0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 padded[] = {0x90000009UL, 0x91020129UL, 0x34000085UL, 0xD65F03C0UL, 0xD503201FUL, 0xD503201FUL,
                                    0x71000C1FUL, 0x54000128UL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL,
                                    0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 landed[] = {0x90000009UL, 0x91020129UL, 0x34000065UL, 0xD65F03C0UL, 0x52800026UL, 0x71000C1FUL,
                                    0x54000128UL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL,
                                    0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 aborted[] = {0x90000009UL, 0x91020129UL, 0x34000045UL, 0x97FFFFFDUL, 0x71000C1FUL,
                                     0x54000128UL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL,
                                     0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 argued[] = {0x340000A5UL, 0x90000001UL, 0x91020021UL, 0x14000002UL, 0xD65F03C0UL, 0x71000C1FUL,
                                    0x54000128UL, 0x1000008AUL, 0x3860682BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL,
                                    0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 answered[] = {0x90000001UL, 0x91020021UL, 0xD4000001UL, 0x71000C1FUL, 0x54000128UL,
                                      0x1000008AUL, 0x3860682BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL,
                                      0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 late[] = {0x90000009UL, 0x91021129UL, 0x9000000CUL, 0x9102018CUL, 0x71000C1FUL, 0x540001E8UL,
                                  0x1000008AUL, 0x3860698BUL, 0x8B0B094AUL, 0xD61F0140UL, 0x71000C3FUL, 0x54000128UL,
                                  0x10FFFFCAUL, 0x3861692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL, 0x10FFFDE9UL,
                                  0x17FFFFF8UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 deep[] = {0x90000009UL, 0x91021129UL, 0x9000000CUL, 0x9102018CUL, 0x71000C1FUL,
                                  0x54000268UL, 0x1000008AUL, 0x3860698BUL, 0x8B0B094AUL, 0xD61F0140UL,
                                  0x71000C3FUL, 0x540001A8UL, 0x10FFFFCAUL, 0x3861692BUL, 0x8B0B094AUL,
                                  0xD61F0140UL, 0x71000C5FUL, 0x540000E8UL, 0x10FFFF0AUL, 0x3862692BUL,
                                  0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 remade[] = {0x90000009UL, 0x91020129UL, 0x7100003FUL, 0x5A815421UL, 0x92400421UL,
                                    0x1000008AUL, 0x3861692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL,
                                    0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 unreachable[] = {0x90000009UL, 0x91020129UL, 0x1000008AUL, 0x3860692BUL,
                                         0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL, 0xD65F03C0UL,
                                         0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 longer[] = {0x90000009UL, 0x91020129UL, 0x7104AC1FUL, 0x54000148UL, 0x1000008AUL,
                                    0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL, 0xD65F03C0UL,
                                    0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const unsigned char in_case[] = {0, 10, 11, 12}, copies[] = {0, 6, 7, 8}, four[] = {0, 1, 2, 3};
    static const unsigned char holed[] = {0, 1, 4, 2, 3}, in_late[] = {0, 0, 0, 0, 6, 7, 9, 6};
    static const unsigned char in_deep[] = {0, 0, 0, 0, 6, 12, 13, 6};
    static SnxFde of_padded[] = {{0, sizeof(padded)}}, of_landed[] = {{0, sizeof(landed)}};
    static SnxFde of_late[] = {{0, sizeof(late)}}, of_deep[] = {{0, sizeof(deep)}}, two[] = {{0, 0x28}, {0x28, 4}};
    static const TableCases others_want[] = {{0x1C, {0x20, 0x48, 0x4C, 0x50}}, {0x44, {0x20, 0x48, 0x4C, 0x50}}};
    static const TableCases copied_want[] = {{0x20, {0x24, 0x3C, 0x40, 0x44}}, {0x38, {0x24, 0x3C, 0x40, 0x44}}};
    static const TableCases apart_want[] = {{0x40, {0x44, 0x48, 0x4C, 0x50}}};
    static const TableCases padded_want[] = {{0x2C, {0x30, 0x34, 0x38, 0x3C}}};
    static const TableCases landed_want[] = {{0x28, {0}}};
    static const TableCases aborted_want[] = {{0x24, {0x28, 0x2C, 0x30, 0x34}}};
    static const TableCases answered_want[] = {{0x20, {0}}};
    static const TableCases late_want[] = {{0x24, {0x28}}, {0x3C, {0}}};
    static const TableCases deep_want[] = {{0x24, {0x28}}, {0x3C, {0x40, 0x58, 0x5C}}, {0x54, {0x40, 0x58, 0x5C}}};
    static const TableCases remade_want[] = {{0x20, {0x24, 0x28, 0x2C, 0x30}}};
    static const TableCases unreachable_want[] = {{0x14, {0x18, 0x1C, 0x20, 0x24}}};
    static const TableCases longer_want[] = {{0x1C, {0x20, 0x24, 0x28, 0x2C, 0x30}}};
    unsigned char entries[300];
    unsigned i;
    for (i = 0; i < sizeof(entries); ++i) entries[i] = (unsigned char)(i % 4);
    entries[sizeof(entries) - 1] = 4;
    table_cases("no write of the base", others, sizeof(others) / 4, in_case, 4, NULL, 0, others_want, 2);
    table_cases("a copy of the base", copied, sizeof(copied) / 4, copies, 4, NULL, 0, copied_want, 2);
    table_cases("an ADRP far from its ADD", apart, sizeof(apart) / 4, four, 4, NULL, 0, apart_want, 1);
    table_cases("a padded join", padded, sizeof(padded) / 4, four, 4, of_padded, 1, padded_want, 1);
    table_cases("a join nothing known leads to", landed, sizeof(landed) / 4, four, 4, of_landed, 1, landed_want, 1);
    table_cases("a call that doesn't return", aborted, sizeof(aborted) / 4, four, 4, NULL, 0, aborted_want, 1);
    table_cases("the caller's table", argued, sizeof(argued) / 4, four, 4, NULL, 0, landed_want, 1);
    table_cases("an SVC's register", answered, sizeof(answered) / 4, four, 4, NULL, 0, answered_want, 1);
    table_cases("a base refused late", late, sizeof(late) / 4, in_late, 8, of_late, 1, late_want, 2);
    table_cases("a switch two switches in", deep, sizeof(deep) / 4, in_deep, 8, of_deep, 1, deep_want, 3);
    table_cases("an index made after a compare", remade, sizeof(remade) / 4, four, 4, NULL, 0, remade_want, 1);
    table_cases("an unreachable case", unreachable, sizeof(unreachable) / 4, holed, 5, two, 2, unreachable_want, 1);
    table_cases("a table of 300 entries", longer, sizeof(longer) / 4, entries, sizeof(entries), NULL, 0, longer_want,
                1);
}

/* The guards of a table's compare in another block (llvm-mc), an entry past it in each: adrp x9; add x9; cmp w0, #imm;
   b join; ret; join: the guard; adr x10, c0; ldrb w11, [x9, x0]; ...: b.hs d (hs), b.lo s; b d; s: (lo), b.ls s; b d; s:
   (ls), two compares of two ways and b.hi d (wider), a call between the compare and b.hi d (called, its base x19) */
static void jump_table_guards_test(void) {
    static const SnxU32 hs[] = {0x90000009UL, 0x91020129UL, 0x7100101FUL, 0x14000002UL, 0xD65F03C0UL, 0x54000142UL,
                                0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL, 0xD65F03C0UL,
                                0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 lo[] = {0x90000009UL, 0x91020129UL, 0x7100101FUL, 0x14000002UL, 0xD65F03C0UL, 0x54000043UL,
                                0x1400000AUL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL,
                                0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 ls[] = {0x90000009UL, 0x91020129UL, 0x71000C1FUL, 0x14000002UL, 0xD65F03C0UL, 0x54000049UL,
                                0x1400000AUL, 0x1000008AUL, 0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL,
                                0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 wider[] = {0x90000009UL, 0x91020129UL, 0x34000065UL, 0x7100041FUL, 0x14000004UL,
                                   0x71000C1FUL, 0x14000002UL, 0xD65F03C0UL, 0x54000148UL, 0x1000008AUL,
                                   0x3860692BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL, 0xD65F03C0UL,
                                   0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const SnxU32 called[] = {0x90000013UL, 0x91020273UL, 0x71000C1FUL, 0x97FFFFFDUL, 0x14000002UL, 0xD65F03C0UL,
                                    0x54000148UL, 0x1000008AUL, 0x38606A6BUL, 0x8B0B094AUL, 0xD61F0140UL, 0xD65F03C0UL,
                                    0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL, 0xD65F03C0UL};
    static const unsigned char five[] = {0, 1, 2, 3, 4};
    static const TableCases hs_want[] = {{0x24, {0x28, 0x2C, 0x30, 0x34}}};
    static const TableCases lo_want[] = {{0x28, {0x2C, 0x30, 0x34, 0x38}}};
    static const TableCases wider_want[] = {{0x30, {0x34, 0x38, 0x3C, 0x40}}};
    static const TableCases called_want[] = {{0x28, {0}}};
    table_cases("a B.HS", hs, sizeof(hs) / 4, five, 5, NULL, 0, hs_want, 1);
    table_cases("a B.LO taken", lo, sizeof(lo) / 4, five, 5, NULL, 0, lo_want, 1);
    table_cases("a B.LS taken", ls, sizeof(ls) / 4, five, 5, NULL, 0, lo_want, 1);
    table_cases("two ways' compares", wider, sizeof(wider) / 4, five, 5, NULL, 0, wider_want, 1);
    table_cases("a call after the compare", called, sizeof(called) / 4, five, 5, NULL, 0, called_want, 1);
}

int main(void) {
    decoder_test();
    analysis_test();
    routed_tail_call_test();
    address_taken_test();
    address_taken_shifted_test();
    jump_table_test();
    jump_table_sign_test();
    svc_resume_test();
    jump_table_other_function_test();
    jump_table_anchor_test();
    jump_table_mask_test();
    jump_table_flow_test();
    jump_table_flags_test();
    jump_table_ways_test();
    jump_table_end_test();
    jump_table_met_test();
    jump_table_guards_test();
    jump_table_anchor_far_test();
    jump_table_anchor_branch_test();
    large_range_test();
    copy_word_test();
    copy_current_test();
    if (native_steps_available()) {
        semantics_test();
        poll_test();
        cpu_extensions_test();
        ounce_cpu_test();
        zva_test();
    } else
        puts("SKIP: the steps' tests: this host has no native back end");
    printf("Native codegen: %s\n", failures ? "FAILED" : "passed");
    return failures != 0;
}
