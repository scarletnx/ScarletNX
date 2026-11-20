/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The integer, hint, barrier, general-purpose load and store and scalar floating-point emitters and the branch operands
   tools/sailgen generates from the Sail (ir_sail.c) against the hand-written ones they replaced
   (support/ir_int_reference.c, ir_ldst_reference.c, ir_fpu_reference.c), word by word: the same IR, and the same
   results.
     snx-test-llvm-sail-ir [--report FILE] [--samples N] [--states N] [--part K/N]
   The words: samples of every decoder row the generated subset covers (llvm_qemu_words.c), decoded for the ounce target
   (TARGET: the Switch's features with PAuth and FlagM), or for every feature where only that has the op (FJCVTZS,
   FRINT32Z and the others of FEAT_FRINTTS, BFCVT), each again with its register fields 31 (the zero register or
   SP). On three paths (x86-64 with SSE4.2's CRC32C, AArch64 with CRC32, and the generic one other hosts take; a path
   whose target this LLVM lacks is said and skipped) each word becomes a step (snx_ir_static_step) in two modules, one
   emitted by each, optimized as a game's module is (snx_ir_module_optimize, O2, the SLP vectorizer off as the setting's
   default), and the two functions are compared. Then both sets of steps run on this host from random states (the
   SIMD&FP registers' low lanes zeros, infinities, NaNs, denormals, rounding ties and conversion edges as often as
   random bits; FPCR's rounding mode, FZ, DN, AHP and FZ16, and FPSR's sticky flags, random), and every register, the
   flags, the exclusive monitor, FPCR, FPSR, the next pc, UNDEF and the runtime's calls for SEV and WFE must match; a
   load or store runs on guest memory of random bytes, its address registers made to point into it (ldst_start), and the
   bytes it may write must match too. Sequences that hand flags or a register from one instruction to the next (SUBS
   then CSEL, CMP then CCMP then CSEL, ADDS then ADC, SUBS then B.HI, a load from a register an ADD just wrote, FCMP
   then CSEL, a sum FMUL reads, FCMP then FCSEL or B.GT, CMP then FCCMP) are emitted as functions (snx_ir_emit) and
   must give the hand-written emitters' IR, so a compare's flags still reach the condition as the compare, and a value
   one emitter writes is the one the other reads back, whichever of the two (generated or written by hand) wrote it.
   Both keep their state in one per-block cache (the back end's), so in a sequence the hand-written emitters run with
   no value carried through it from one instruction to the next (reference_uncached, fpu_reference_uncached): a cache
   that kept a stale one shows in the generated side only. Two IRs that differ only in the order of their instructions
   and their names count as the same; a step's that differs otherwise may not have more instructions than the
   hand-written one, nor be another IR for an op whose effect running can't see (strict_ops) or a load or store
   (unless ldst_other names its form, with why), so a dropped fence, ordering or atomicity shows, and is listed.
   --report writes each difference's two IR texts; --time ROUNDS only times the two emitters (time_emitters); --part
   K/N compares and runs only every Nth word from the Kth, and the sequences in part 1, so N ctest runs share the
   words */
#include "llvm_steps.h"
#include "llvm_qemu_words.h"
#include "support/ir_int_reference.h"
#include "support/ir_ldst_reference.h"
#include "support/ir_fpu_reference.h"
#include "support/ir_vec_reference.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/utilities/endian.h"
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the ops ir_int.c sends to the Sail: the data-processing ones (generated()), the branches' operands
   (generated_branch()); then ir_ldst.c's (generated()), from "STR_i" on (FIRST_LDST) */
static const char* const generated_ops[] = {
    "ADR",      "ADRP",     "ADD_i",   "ADDS_i",    "SUB_i",     "SUBS_i",    "AND_i",     "ORR_i",   "EOR_i",
    "ANDS_i",   "MOVN",     "MOVZ",    "MOVK",      "SBFM",      "BFM",       "UBFM",      "EXTR",    "AND_r",
    "ORR_r",    "EOR_r",    "ANDS_r",  "ADD_r",     "SUB_r",     "ADDS_r",    "SUBS_r",    "ADD_ext", "SUB_ext",
    "ADDS_ext", "SUBS_ext", "ADC",     "ADCS",      "SBC",       "SBCS",      "CCMP",      "CSEL",    "RBIT",
    "REV16",    "REV32",    "REV64",   "CLZ",       "CLS",       "UDIV",      "SDIV",      "LSLV",    "LSRV",
    "ASRV",     "RORV",     "CRC32",   "CRC32C",    "MADD_w",    "MSUB_w",    "MADD_x",    "MSUB_x",  "SMADDL",
    "SMSUBL",   "UMADDL",   "UMSUBL",  "SMULH",     "UMULH",     "XPACI",     "XPACD",     "PACGA",   "PACIA",
    "PACIB",    "PACDA",    "PACDB",   "AUTIA",     "AUTIB",     "AUTDA",     "AUTDB",     "CFINV",   "RMIF",
    "SETF8",    "SETF16",   "B_cond",  "CBZ",       "TBZ",       "BR",        "BLR",       "RET",     "BRA",
    "BRAZ",     "BLRA",     "BLRAZ",   "RETA",      "NOP",       "YIELD",     "WFE",       "WFI",     "SEV",
    "SEVL",     "ESB",      "XPACLRI", "PACIA1716", "PACIB1716", "AUTIA1716", "AUTIB1716", "PACIAZ",  "PACIASP",
    "PACIBZ",   "PACIBSP",  "AUTIAZ",  "AUTIASP",   "AUTIBZ",    "AUTIBSP",   "CLREX",     "DSB_DMB", "ISB",
    "STR_i",    "LDR_i",    "STR",     "LDR",       "LD_lit",    "STP",       "LDP",       "STXR",    "LDXR",
    "STXP",     "LDXP",     "STLR",    "LDAR",      "LDAPR",     "LDAPR_i",   "STLR_i",    "CAS",     "CASP",
    "LDADD",    "LDCLR",    "LDEOR",   "LDSET",     "LDSMAX",    "LDSMIN",    "LDUMAX",    "LDUMIN",  "SWP"};
#define GENERATED_OPS (sizeof generated_ops / sizeof *generated_ops)
#define FIRST_LDST    (GENERATED_OPS - 27)
/* and the ones ir_fpu.c sends to it: every scalar floating-point instruction */
static const char* const fp_ops[] = {
    "FADD_s",    "FSUB_s",     "FDIV_s",     "FMUL_s",     "FNMUL_s",    "FMAX_s",    "FMIN_s",    "FMAXNM_s",
    "FMINNM_s",  "FMADD",      "FMSUB",      "FNMADD",     "FNMSUB",     "FMOV_s",    "FABS_s",    "FNEG_s",
    "FSQRT_s",   "FRINTN_s",   "FRINTP_s",   "FRINTM_s",   "FRINTZ_s",   "FRINTA_s",  "FRINTX_s",  "FRINTI_s",
    "BFCVT_s",   "FRINT32Z_s", "FRINT32X_s", "FRINT64Z_s", "FRINT64X_s", "FCVT_s_ds", "FCVT_s_hs", "FCVT_s_sd",
    "FCVT_s_hd", "FCVT_s_sh",  "FCVT_s_dh",  "FMOVI_s",    "FCMP",       "FCCMP",     "FCSEL",     "SCVTF_g",
    "UCVTF_g",   "FCVTNS_g",   "FCVTNU_g",   "FCVTPS_g",   "FCVTPU_g",   "FCVTMS_g",  "FCVTMU_g",  "FCVTZS_g",
    "FCVTZU_g",  "FCVTAS_g",   "FCVTAU_g",   "FJCVTZS",    "FMOV_ws",    "FMOV_sw",   "FMOV_xd",   "FMOV_dx",
    "FMOV_xu",   "FMOV_ux",    "FMOV_xh",    "FMOV_hx"};
#define FP_OPS (sizeof fp_ops / sizeof *fp_ops)
/* and the SIMD ones ir_vec.c sends to it (generated()), each element loop one vector operation, a shuffle (the
   pairwise ops) or a reduction (across lanes) */
static const char* const vec_ops[] = {"ADD_v",  "SUB_v",  "CMEQ_v",  "CMTST_v", "CMGE_v",  "CMGT_v",  "CMHI_v",
                                      "CMHS_v", "SMAX_v", "SMIN_v",  "UMAX_v",  "UMIN_v",  "NEG_v",   "ABS_v",
                                      "MUL_v",  "ADDP_v", "SMAXP_v", "SMINP_v", "UMAXP_v", "UMINP_v", "ADDV",
                                      "SADDLV", "UADDLV", "SMAXV",   "SMINV",   "UMAXV",   "UMINV"};
#define VEC_OPS (sizeof vec_ops / sizeof *vec_ops)

typedef struct {
    const char* name;
    const char* triple;
    SnxIrFeatures features;
} Path;

static const Path paths[] = {
    {"x86", "x86_64-unknown-linux-gnu", {.x86 = 1, .ssse3 = 1, .sse42 = 1, .crc32 = 1}},
    {"aarch64", "aarch64-unknown-linux-gnu", {.aarch64 = 1, .arm_crc = 1}},
    {"generic", "riscv64-unknown-linux-gnu", {0}},
};
#define PATHS (sizeof paths / sizeof *paths)

/* the sequences: each ends in RET (llvm-mc's encodings). Each hands a register or the flags from one emitter to the
   next, so its IR is the hand-written emitters' (in any order): another means one read what the other did not write,
   or a compare reached the condition as flags. other says why one may differ */
static const struct {
    const char* what;
    u32 words[5];
    const char* other;
} sequences[] = {
    {"SUBS then CSEL LT", {0xeb020020u, 0x9a85b083u, 0xd65f03c0u}, NULL},
    {"ADDS #5 then CSEL EQ (W)", {0x31001420u, 0x1a850083u, 0xd65f03c0u}, NULL},
    {"ANDS then CSINC GE", {0xea020020u, 0x9a85a483u, 0xd65f03c0u}, NULL},
    {"CMP #3, CCMP NE, CSEL GT",
     {0xf1000c3fu, 0xfa431044u, 0x9a85c080u, 0xd65f03c0u},
     "CCMP's flags are a select each, not one of a nibble"},
    {"ADDS then ADC", {0xab020020u, 0x9a050083u, 0xd65f03c0u}, NULL},
    {"CMN #7 then CSET HS (W)", {0x31001c3fu, 0x1a9f37e0u, 0xd65f03c0u}, NULL},
    {"TST #0xff then CSET EQ", {0xf2401c3fu, 0x9a9f17e0u, 0xd65f03c0u}, NULL},
    {"SUBS then CSNEG VS (W)", {0x6b020020u, 0x5a856483u, 0xd65f03c0u}, NULL},
    {"ADDS #0 then CSEL MI", {0xb1000020u, 0x9a844062u, 0xd65f03c0u}, NULL},
    {"SUBS then B.HI", {0xeb020020u, 0x54000048u, 0x91000400u, 0xd65f03c0u}, NULL},
    {"LDR from X1, ADD to X1, LDR from X1", {0xf9400022u, 0x91002021u, 0xf9400023u, 0xd65f03c0u}, NULL},
    {"ADD to X1, STR of X1, ADD of X1", {0x91000401u, 0xf9000041u, 0x8b010023u, 0xd65f03c0u}, NULL},
    {"FCMP then CSEL GT", {0x1e612000u, 0x9a82c020u, 0xd65f03c0u}, NULL},
    {"CFINV then B.HS",
     {0xd500401fu, 0x54000042u, 0x91000400u, 0xd65f03c0u},
     "CFINV's generated emitter flips C alone, the hand-written one packs NZCV and unpacks it"},
    {"ADCS then B.HS", {0xba020020u, 0x54000042u, 0x91000400u, 0xd65f03c0u}, NULL},
    {"SUBS, LDR, CSEL EQ", {0xeb020020u, 0xf9400003u, 0x9a8600a4u, 0xd65f03c0u}, NULL},
    {"FADD then FMUL of its sum", {0x1e222820u, 0x1e240803u, 0xd65f03c0u}, NULL},
    {"FMOV S0 from W1, FADD, FMOV W2 from S2", {0x1e270020u, 0x1e232802u, 0x1e260042u, 0xd65f03c0u}, NULL},
    {"FCMP then FCSEL GT", {0x1e612000u, 0x1e64cc62u, 0xd65f03c0u}, NULL},
    {"SUBS then FCSEL LT", {0xeb020020u, 0x1e24bc62u, 0xd65f03c0u}, NULL},
    {"FCMP then B.GT", {0x1e612000u, 0x5400004cu, 0x91000400u, 0xd65f03c0u}, NULL},
    {"CMP #3, FCCMP NE, CSEL GT", {0xf1000c1fu, 0x1e211404u, 0x9a86c0a0u, 0xd65f03c0u}, NULL},
    {"FCMP, FCCMPE EQ, FCSEL EQ", {0x1e612000u, 0x1e610410u, 0x1e230c41u, 0xd65f03c0u}, NULL},
    {"SCVTF then FCVTZS", {0x9e620020u, 0x1e780002u, 0xd65f03c0u}, NULL},
    {"FMADD then FNMUL of it", {0x1f420c20u, 0x1e658804u, 0xd65f03c0u}, NULL},
    {"FCMP #0 then CSET MI", {0x1e202008u, 0x9a9f57e0u, 0xd65f03c0u}, NULL},
};
#define SEQUENCES (sizeof sequences / sizeof *sequences)

/* guest memory for the loads and stores: each side's own, guest address 0 MEMORY_ZERO bytes in, the same random
   bytes in both before each run. The address registers of a load or store are made 0x1000 to 0x1ff0 (16-aligned, so
   the exclusives and the atomics are aligned host atomics), so every access lands below MEMORY_TOUCHED, which a run
   compares and puts back; a literal reaches pc 0x1000 plus or minus 1 MiB, which only loads read */
#define MEMORY_SIZE    (4u << 20)
#define MEMORY_ZERO    (2u << 20)
#define MEMORY_TOUCHED 0x14000u

static struct {
    u64 seed;
    FILE* report;
    unsigned failures;
    unsigned undef, sev, wfe; /* the UNDEFs a step raised, its runtime calls for SEV and WFE */
    /* each side's guest memory, and the bytes both start from (run_compare); 16-aligned, as the 128-bit exclusives and
       CASP are 16-byte host atomics (x86-64's cmpxchg16b, AArch64's exclusive pairs), which fault on another address */
    _Alignas(16) u8 memory[3][MEMORY_SIZE];
    size_t outcomes[4];
    u32 word;          /* the word being emitted where says (a path, a sequence, the runs), for on_abort */
    const char* where; /* NULL while nothing is emitted */
} g_test;

static u64 rnd(void) {
    g_test.seed ^= g_test.seed << 13;
    g_test.seed ^= g_test.seed >> 7;
    g_test.seed ^= g_test.seed << 17;
    return g_test.seed;
}

/* the guest the words are decoded, emitted and run for: the ounce target, the Switch's features with PAuth and FlagM,
   for which every op ir_int.c sends to the Sail exists (on the Switch's alone 15 of them are UNDEF on both sides) */
#define TARGET SNX_A64_TARGET_OUNCE
static int is_fp_op(const char* name) {
    for (size_t i = 0; i < FP_OPS; ++i)
        if (!strcmp(name, fp_ops[i])) return 1;
    return 0;
}
/* the target a word is decoded for: TARGET, or every feature for a floating-point op TARGET lacks (FEAT_JSCVT,
   FEAT_FRINTTS, FEAT_BF16), which no target of the console has and the generated emitters must still match */
static SnxU64 word_target(u32 word) {
    SnxA64Insn insn;
    if (snx_a64_decode_word(word, TARGET, &insn) != SNX_A64_OP_UNDEF) return TARGET;
    if (snx_a64_decode_word(word, SNX_A64_TARGET_MAX, &insn) != SNX_A64_OP_UNDEF && is_fp_op(snx_a64_op_text(insn.op)))
        return SNX_A64_TARGET_MAX;
    return TARGET;
}
static const char* op_name(u32 word) {
    SnxA64Insn insn;
    return snx_a64_decode_word(word, word_target(word), &insn) != SNX_A64_OP_UNDEF ? snx_a64_op_text(insn.op) : NULL;
}

static int generated(u32 word) {
    const char* name = op_name(word);
    if (!name) return 0;
    for (size_t i = 0; i < GENERATED_OPS; ++i)
        if (!strcmp(name, generated_ops[i])) return 1;
    for (size_t i = 0; i < VEC_OPS; ++i)
        if (!strcmp(name, vec_ops[i])) return 1;
    return is_fp_op(name);
}

/* a load or store: its IR is held to the hand-written one's instructions, and it runs with guest memory */
static int is_ldst(u32 word) {
    const char* name = op_name(word);
    for (size_t i = FIRST_LDST; name && i < GENERATED_OPS; ++i)
        if (!strcmp(name, generated_ops[i])) return 1;
    return 0;
}

/* the loads and stores whose generated IR may be another than the hand-written one (not longer), and why, for the words
   whose bits under mask are value (and with one_register, whose Rt and Rt2 are one): any other must give the same
   instructions, so an access that lost its ordering, its width or its atomicity shows */
static const struct {
    const char* op;
    u32 mask, value;
    int one_register;
    const char* why;
} ldst_other[] = {
    {"STP", 1u << 31, 0, 1,
     "a pair of words of one register (Rt == Rt2): the value stored is its low word times 0x100000001, one instruction "
     "fewer"},
    {"LDXP", 1u << 30, 0, 0, "a pair of words is one 8-byte load-exclusive (ldx), the hand-written one ldxp of words"},
    {"STXP", 1u << 30, 0, 0,
     "a pair of words is one 8-byte store-exclusive (stx), which an 8-byte ldx arms, the hand-written one stxp of words, "
     "which ldxp of words arms"},
    {NULL, 0, 0, 0, NULL},
};
static const char* ldst_other_why(u32 word) {
    const char* name = op_name(word);
    for (size_t i = 0; name && ldst_other[i].op; ++i)
        if (!strcmp(name, ldst_other[i].op) && (word & ldst_other[i].mask) == ldst_other[i].value &&
            (!ldst_other[i].one_register || (word & 0x1fu) == (word >> 10 & 0x1fu)))
            return ldst_other[i].why;
    return NULL;
}

/* a function's text without its name and its metadata attachments, which number differently from module to module */
static char* body_text(LLVMValueRef fn) {
    char* text = LLVMPrintValueToString(fn);
    char* out = (char*)snx_realloc(NULL, strlen(text) + 1);
    const char* s = strchr(text, '{');
    size_t n = 0;
    if (!s) s = text;
    while (*s) {
        if (s[0] == ',' && s[1] == ' ' && s[2] == '!' && s[3] != '\0') { /* , !name !N */
            const char* e = s + 3;
            while (*e && *e != ' ' && *e != '\n') ++e;
            if (*e == ' ' && e[1] == '!') {
                ++e;
                while (*e && *e != ',' && *e != '\n') ++e;
                s = e;
                continue;
            }
        }
        out[n++] = *s++;
    }
    out[n] = 0;
    LLVMDisposeMessage(text);
    return out;
}

/* ---- the same instructions in another order ----
   Each instruction's hash: its opcode, type, predicate, callee and its operands' hashes (sorted for the commutative
   ones), so two functions that compute the same values with the same instructions, in whatever order and under
   whatever names, have the same sorted list of hashes. Their count is the function's size */
typedef struct {
    LLVMValueRef v;
    u64 h;
} Memo;
typedef struct {
    Memo* memo;
    size_t count, cap;
    LLVMValueRef fn;
} Dag;

static u64 fnv(u64 h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}
static u64 fnv_text(u64 h, char* text) {
    h = fnv(h, text, strlen(text));
    LLVMDisposeMessage(text);
    return h;
}
static int cmp_u64(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return x < y ? -1 : x > y;
}

static u64 value_hash(Dag* d, LLVMValueRef v) {
    u64 h = 0xcbf29ce484222325ull;
    if (LLVMIsAInstruction(v)) {
        LLVMOpcode op = LLVMGetInstructionOpcode(v);
        unsigned n = (unsigned)LLVMGetNumOperands(v), i;
        u64 ops[16];
        for (i = 0; i < d->count; ++i)
            if (d->memo[i].v == v) return d->memo[i].h;
        h = fnv(h, &op, sizeof op);
        h = fnv_text(h, LLVMPrintTypeToString(LLVMTypeOf(v)));
        if (op == LLVMICmp) {
            LLVMIntPredicate p = LLVMGetICmpPredicate(v);
            h = fnv(h, &p, sizeof p);
        }
        /* an access's ordering, which no operand holds (a fence acquire is not a fence release) */
        if (op == LLVMFence || op == LLVMLoad || op == LLVMStore || op == LLVMAtomicRMW) {
            LLVMAtomicOrdering o = LLVMGetOrdering(v);
            h = fnv(h, &o, sizeof o);
        }
        if (n > 16) n = 16;
        for (i = 0; i < n; ++i) ops[i] = value_hash(d, LLVMGetOperand(v, i));
        if (op == LLVMAdd || op == LLVMMul || op == LLVMAnd || op == LLVMOr || op == LLVMXor ||
            (op == LLVMICmp && (LLVMGetICmpPredicate(v) == LLVMIntEQ || LLVMGetICmpPredicate(v) == LLVMIntNE)))
            qsort(ops, n, sizeof *ops, cmp_u64);
        h = fnv(h, ops, n * sizeof *ops);
        if (d->count == d->cap) {
            d->cap = d->cap ? d->cap * 2 : 64;
            d->memo = (Memo*)snx_realloc(d->memo, d->cap * sizeof *d->memo);
        }
        d->memo[d->count].v = v;
        d->memo[d->count].h = h;
        ++d->count;
        return h;
    }
    if (LLVMValueIsBasicBlock(v)) { /* a block by its place in the function */
        unsigned k = 0;
        for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(d->fn); b; b = LLVMGetNextBasicBlock(b), ++k)
            if (LLVMBasicBlockAsValue(b) == v) return fnv(h ^ 0xb10c, &k, sizeof k);
        return h;
    }
    if (LLVMIsAArgument(v)) {
        unsigned k = 0;
        for (LLVMValueRef a = LLVMGetFirstParam(d->fn); a; a = LLVMGetNextParam(a), ++k)
            if (a == v) return fnv(h ^ 0xa49, &k, sizeof k);
        return h;
    }
    if (LLVMIsAGlobalValue(v)) {
        size_t length;
        const char* name = LLVMGetValueName2(v, &length);
        return fnv(h ^ 0x610b, name, length);
    }
    return fnv_text(h, LLVMPrintValueToString(v)); /* a constant */
}

/* the sorted hashes of fn's instructions; their number in *count */
static u64* dag(LLVMValueRef fn, size_t* count) {
    Dag d = {NULL, 0, 0, fn};
    size_t n = 0, cap = 64;
    u64* list = (u64*)snx_realloc(NULL, cap * sizeof *list);
    for (LLVMBasicBlockRef b = LLVMGetFirstBasicBlock(fn); b; b = LLVMGetNextBasicBlock(b))
        for (LLVMValueRef i = LLVMGetFirstInstruction(b); i; i = LLVMGetNextInstruction(i)) {
            if (n == cap) list = (u64*)snx_realloc(list, (cap *= 2) * sizeof *list);
            list[n++] = value_hash(&d, i);
        }
    qsort(list, n, sizeof *list, cmp_u64);
    free(d.memo);
    *count = n;
    return list;
}

/* how fn[1] (generated) compares with fn[0] (hand-written): 0 the same text, 1 the same instructions in another
   order, 2 another IR of as many instructions or fewer, 3 more instructions */
enum { SAME, REORDERED, OTHER_NOT_LONGER, LONGER };
static int compare_fns(LLVMValueRef hand, LLVMValueRef gen, char** text_hand, char** text_gen, size_t* sizes) {
    size_t n0, n1;
    u64 *a = dag(hand, &n0), *b = dag(gen, &n1);
    int r;
    *text_hand = body_text(hand);
    *text_gen = body_text(gen);
    sizes[0] = n0;
    sizes[1] = n1;
    if (!strcmp(*text_hand, *text_gen))
        r = SAME;
    else if (n0 == n1 && !memcmp(a, b, n0 * sizeof *a))
        r = REORDERED;
    else
        r = n1 > n0 ? LONGER : OTHER_NOT_LONGER;
    free(a);
    free(b);
    return r;
}

/* the words: every generated row's samples, and each again with its register fields 31 */
static size_t corpus(unsigned samples, u32** out) {
    u32* rows = NULL;
    size_t count = lqd_row_words(samples, rnd, &rows), n = 0;
    u32* words = (u32*)snx_realloc(NULL, sizeof(u32) * (count * 8 + 1));
    for (size_t i = 0; i < count; ++i) {
        static const u32 fields[3] = {0x1fu, 0x1fu << 5, 0x1fu << 16};
        const char* name = op_name(rows[i]);
        if (!generated(rows[i])) continue;
        words[n++] = rows[i];
        for (unsigned f = 0; f < 7; ++f) { /* each nonempty set of Rd, Rn and Rm made 31 */
            u32 w = rows[i];
            for (unsigned k = 0; k < 3; ++k)
                if ((f + 1) & (1u << k)) w |= fields[k];
            if (w != rows[i] && op_name(w) && !strcmp(op_name(w), name)) words[n++] = w;
        }
    }
    free(rows);
    *out = words;
    return n;
}

static SnxIr* path_ir(const Path* p, int reference, char* error, size_t size) {
    SnxIr* ir = snx_ir_new_target(reference ? "reference" : "generated", '2', p->triple, error, size);
    if (!ir) return NULL;
    ir->features = &p->features;
    if (reference) {
        ir->int_reference = snx_ir_int_reference;
        ir->operand_reference = snx_ir_int_operand_reference;
        ir->ldst_reference = snx_ir_ldst_reference;
        ir->fpu_reference = snx_ir_fpu_reference;
        ir->vec_reference = snx_ir_vec_reference;
    }
    return ir;
}

static const char* const outcome[4] = {"the same", "the same instructions reordered", "another IR, not longer",
                                       "LONGER"};

/* the ops whose effect the runs below cannot see (a fence, the host's spin-wait hint) or that have none: their IR is
   the hand-written one, in any order, or they fail */
static const char* const strict_ops[] = {"NOP", "YIELD", "WFE", "WFI", "SEV", "SEVL", "ESB", "CLREX", "DSB_DMB", "ISB"};
static int strict(const char* op) {
    for (size_t i = 0; i < sizeof strict_ops / sizeof *strict_ops; ++i)
        if (!strcmp(op, strict_ops[i])) return 1;
    return 0;
}

/* one comparison: counted; a longer generated IR fails, and another IR of a strict op or of a load or store
   ldst_other does not name; the report gets every one that is not the same text */
static void judge(int r, const char* what, u32 word, const char* path, const char* a, const char* b,
                  const size_t* sizes) {
    ++g_test.outcomes[r];
    if (r == LONGER) {
        ++g_test.failures;
        fprintf(stderr, "FAIL: %s %08x (%s): the generated IR has %zu instructions, the hand-written %zu\n", what,
                (unsigned)word, path, sizes[1], sizes[0]);
    } else if (r == OTHER_NOT_LONGER &&
               (strict(what) || (is_ldst(word) && !strcmp(what, op_name(word)) && !ldst_other_why(word)))) {
        ++g_test.failures;
        fprintf(stderr,
                "FAIL: %s %08x (%s): the generated IR is another than the hand-written, and running can't tell\n", what,
                (unsigned)word, path);
    }
    if (r != SAME && g_test.report)
        fprintf(g_test.report, "== %s %08x on %s: %s (%zu, %zu)\n-- hand-written\n%s\n-- generated\n%s\n", what,
                (unsigned)word, path, outcome[r], sizes[0], sizes[1], a, b);
}

/* each word's step from both emitters on path p, optimized, compared */
static int compare_path(const Path* p, const u32* words, size_t count, size_t* equal) {
    char error[1024];
    SnxIr* ir[2];
    LLVMValueRef* fn[2];
    int r = 0;
    for (int k = 0; k < 2; ++k) {
        ir[k] = path_ir(p, k == 0, error, sizeof error);
        if (!ir[k]) {
            printf("path %s (%s) not compared: %s\n", p->name, p->triple, error);
            if (k) snx_ir_free(ir[0]);
            return 0;
        }
        fn[k] = (LLVMValueRef*)snx_realloc(NULL, sizeof(LLVMValueRef) * (count + 1));
        for (size_t i = 0; i < count; ++i) {
            char name[32];
            snprintf(name, sizeof name, "S_%zu", i);
            g_test.word = words[i];
            g_test.where = p->name;
            fn[k][i] = snx_ir_static_step(ir[k], words[i], 0x1000, word_target(words[i]), name);
        }
        g_test.where = NULL;
        if (ir[k]->fallbacks) {
            fprintf(stderr, "FAIL: %s: no emitter for %08x (%s)\n", k ? "generated" : "hand-written",
                    (unsigned)ir[k]->fallback_word, op_name(ir[k]->fallback_word));
            ++g_test.failures;
        }
        if (snx_ir_module_optimize(ir[k], ir[k]->mod, '2', error, sizeof error)) {
            fprintf(stderr, "FAIL: path %s: %s\n", p->name, error);
            ++g_test.failures;
            r = -1;
        }
    }
    for (size_t i = 0; i < count && !r; ++i) {
        char *a, *b;
        size_t sizes[2];
        int k = compare_fns(fn[0][i], fn[1][i], &a, &b, sizes);
        judge(k, op_name(words[i]), words[i], p->name, a, b, sizes);
        *equal += k <= REORDERED;
        free(a);
        free(b);
    }
    for (int k = 0; k < 2; ++k) {
        free(fn[k]);
        snx_ir_free(ir[k]);
    }
    return r;
}

/* the hand-written integer emitters with the registers' and the flags' values dropped from the cache before and
   after each: the reference of a sequence then reads every value an instruction left from the CPU structure, which
   O2 forwards from the store, and the generated side alone reads them from the cache. What made the flags stays
   (llvm_flags_cache checks it): a condition after a compare is the compare on both sides */
static void reference_uncached(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc) {
    ir_registers_invalidate(ir);
    ir->state.flags.valid = 0;
    snx_ir_int_reference(ir, decoded, word, pc);
    ir_registers_invalidate(ir);
    ir->state.flags.valid = 0;
}
/* and the hand-written floating-point ones, with the SIMD&FP registers' cache dropped too. The flags an FP compare
   made stay cached, as on the generated side: dropped, a B.cond in another block reads them from the CPU structure,
   which ir_promote_registers turns into phis the generated side never has (the flags' own cache is the integer
   sequences' to check) */
static void fpu_reference_uncached(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc) {
    ir_registers_invalidate(ir);
    ir_vectors_invalidate(ir);
    snx_ir_fpu_reference(ir, decoded, word, pc);
    ir_registers_invalidate(ir);
    ir_vectors_invalidate(ir);
}

/* the same for the hand-written loads and stores */
static void reference_ldst_uncached(SnxIr* ir, const void* decoded, SnxU32 word, SnxU64 pc) {
    ir_registers_invalidate(ir);
    ir->state.flags.valid = 0;
    snx_ir_ldst_reference(ir, decoded, word, pc);
    ir_registers_invalidate(ir);
    ir->state.flags.valid = 0;
}

/* a sequence as a function of its own (snx_ir_emit), from each emitter, on this host, compared */
static void compare_sequence(unsigned s) {
    char error[1024];
    unsigned char image[4 * 8];
    unsigned char leaders[8] = {1};
    size_t count = 0;
    char* text[2];
    LLVMValueRef fn[2];
    SnxIr* irs[2];
    size_t leader_count = 1;
    for (; count < 5 && sequences[s].words[count]; ++count) snx_st_le32(image + 4 * count, sequences[s].words[count]);
    for (size_t i = 0; i < count; ++i) /* a B.cond's target and the word after it start blocks */
        if ((sequences[s].words[i] & 0xff000010u) == 0x54000000u) {
            size_t target = i + ((sequences[s].words[i] >> 5) & 0x7ffffu);
            if (i + 1 < count && !leaders[i + 1]) leaders[i + 1] = 1, ++leader_count;
            if (target < count && !leaders[target]) leaders[target] = 1, ++leader_count;
        }
    for (int k = 0; k < 2; ++k) {
        SnxCodeImage module = {.image = image, .image_size = 4 * count, .base = 0x1000, .text_size = 4 * count};
        SnxFunction function = {.start = 0x1000,
                                .end = 0x1000 + 4 * count,
                                .count = count,
                                .leaders = leaders,
                                .leader_count = leader_count};
        SnxCodegen codegen = {
            .modules = &module, .module_count = 1, .functions = &function, .function_count = 1, .features = TARGET};
        SnxIr* ir = snx_ir_new("sequence", '2', error, sizeof error);
        if (!ir) {
            fprintf(stderr, "FAIL: %s\n", error);
            ++g_test.failures;
            if (k) snx_ir_free(irs[0]);
            return;
        }
        if (k == 0) {
            ir->int_reference = reference_uncached;
            ir->operand_reference = snx_ir_int_operand_reference;
            ir->ldst_reference = reference_ldst_uncached;
            ir->fpu_reference = fpu_reference_uncached;
            ir->vec_reference = snx_ir_vec_reference;
        }
        g_test.word = sequences[s].words[0];
        g_test.where = sequences[s].what;
        snx_ir_emit(ir, &codegen, &function);
        g_test.where = NULL;
        if (snx_ir_module_optimize(ir, ir->mod, '2', error, sizeof error)) {
            fprintf(stderr, "FAIL: %s: %s\n", sequences[s].what, error);
            ++g_test.failures;
        }
        fn[k] = snx_ir_guest_function(ir, 0x1000);
        irs[k] = ir;
    }
    {
        size_t sizes[2];
        int r = compare_fns(fn[0], fn[1], &text[0], &text[1], sizes);
        judge(r, sequences[s].what, sequences[s].words[0], "this host", text[0], text[1], sizes);
        if (r > REORDERED && !sequences[s].other) {
            ++g_test.failures;
            fprintf(stderr, "FAIL: %s: the generated IR is %s, not the hand-written emitters'\n", sequences[s].what,
                    outcome[r]);
        }
    }
    for (int k = 0; k < 2; ++k) snx_ir_free(irs[k]);
    free(text[0]);
    free(text[1]);
}

/* ---- running both ---- */

static void on_undef(CPU* c, u64 pc, u32 word) {
    (void)c;
    (void)pc;
    (void)word;
    ++g_test.undef;
}
static void on_sev(void) { ++g_test.sev; }
static void on_wfe(CPU* c) {
    (void)c;
    ++g_test.wfe;
}

/* a load or store's start: its address registers in the window; for a store-exclusive the monitor armed at its
   address in some states, with the value there (it stores) or another (it fails), for the size its own load-exclusive
   arms: STXR's, and STXP's of doublewords (ldxp's 0x13). A pair of words is left unarmed: its generated emitter is a
   64-bit stx, which a 64-bit ldx arms, where the hand-written one was stxp, which ldxp arms (cpu.h) */
static void ldst_start(CPU* c, u32 word, unsigned s) {
    SnxA64Insn insn;
    u64 address;
    snx_a64_decode_word(word, TARGET, &insn);
    c->x[31] = 0x1000 + (rnd() & 0xff0);
    c->x[insn.args.rn] = 0x1000 + (rnd() & 0xff0);
    c->x[insn.args.rm] = 0x1000 + (rnd() & 0xff0);
    if (insn.op == SNX_A64_OP_LDAPR_i || insn.op == SNX_A64_OP_STLR_i) /* the base plus the offset aligned (ordered) */
        c->x[insn.args.rn] -= (c->x[insn.args.rn] + (u64)(int64_t)insn.args.imm) & ((1u << insn.args.sz) - 1);
    address = c->x[insn.args.rn];
    c->excl_on = 0;
    if (!(s & 1)) return;
    if (insn.op == SNX_A64_OP_STXR || (insn.op == SNX_A64_OP_STXP && insn.args.sz == 3)) {
        c->excl_on = 1;
        c->excl_addr = address;
        memcpy(&c->excl_val, g_test.memory[2] + MEMORY_ZERO + address, 8);
        memcpy(&c->excl_val2, g_test.memory[2] + MEMORY_ZERO + address + 8, 8);
        if (s & 2) c->excl_val = ~c->excl_val;
        c->excl_size = insn.op == SNX_A64_OP_STXP ? 0x13 : (u32)insn.args.sz;
    }
}

/* an ordered access's address (an acquire or a release, an exclusive, an atomic) when it is off the alignment it needs,
   its size or a pair's two, which an AArch64 host's ldar, stlr, ldxp or cas fault on and an x86-64 host's do not: 0
   when it is on it, or the access is a plain one */
static u64 ordered_misaligned(const CPU* c, u32 word) {
    SnxA64Insn insn;
    u64 address, size;
    snx_a64_decode_word(word, TARGET, &insn);
    address = c->x[insn.args.rn];
    size = 1ull << insn.args.sz;
    switch (insn.op) {
        case SNX_A64_OP_LDAPR_i:
        case SNX_A64_OP_STLR_i: address += (u64)(int64_t)insn.args.imm; break;
        case SNX_A64_OP_STXP:
        case SNX_A64_OP_LDXP:
        case SNX_A64_OP_CASP: size *= 2; break;
        case SNX_A64_OP_STXR:
        case SNX_A64_OP_LDXR:
        case SNX_A64_OP_STLR:
        case SNX_A64_OP_LDAR:
        case SNX_A64_OP_LDAPR:
        case SNX_A64_OP_CAS:
        case SNX_A64_OP_LDADD:
        case SNX_A64_OP_LDCLR:
        case SNX_A64_OP_LDEOR:
        case SNX_A64_OP_LDSET:
        case SNX_A64_OP_LDSMAX:
        case SNX_A64_OP_LDSMIN:
        case SNX_A64_OP_LDUMAX:
        case SNX_A64_OP_LDUMIN:
        case SNX_A64_OP_SWP: break;
        default: return 0;
    }
    return address % size ? address : 0;
}

/* a SIMD&FP register's low lanes and FPCR and FPSR for state s: the values the floating-point paths branch on (signed
   zeros, infinities, quiet and signalling NaNs, denormals, the smallest normal, ties, the integers' edges) in half of
   the registers, random bits in the rest; state 0 in round to nearest without FZ, as most games run */
static void fp_state(CPU* c, unsigned s) {
    static const u64 doubles[] = {
        0x0000000000000000ull, 0x8000000000000000ull, 0x7FF0000000000000ull, 0xFFF0000000000000ull,
        0x7FF8000000000001ull, 0x7FF0000000000001ull, 0x000FFFFFFFFFFFFFull, 0x8000000000000001ull,
        0x0010000000000000ull, 0x3FF8000000000000ull, 0xC004000000000000ull, 0x3FE0000000000000ull,
        0x41E0000000000000ull, 0xC3E0000000000000ull, 0x43F0000000000000ull, 0x3FF0000000000000ull};
    static const u32 singles[] = {0x00000000u, 0x80000000u, 0x7F800000u, 0xFF800000u, 0x7FC00001u, 0x7F800001u,
                                  0x007FFFFFu, 0x80000001u, 0x00800000u, 0x3FC00000u, 0xC0200000u, 0x3F000000u,
                                  0x4F000000u, 0xDF000000u, 0x5F800000u, 0x3F800000u};
    static const u16 halves[] = {0x0000u, 0x8000u, 0x7C00u, 0xFC00u, 0x7E01u, 0x7C01u, 0x03FFu, 0x8001u,
                                 0x0400u, 0x3E00u, 0xC100u, 0x3800u, 0x7BFFu, 0xFBFFu, 0x3C01u, 0x3C00u};
    for (unsigned r = 0; r < 32; ++r) {
        c->v[r].d[0] = rnd();
        c->v[r].d[1] = rnd();
        switch (rnd() & 7) {
            case 0: c->v[r].d[0] = doubles[rnd() & 15]; break;
            case 1: c->v[r].d[0] = (c->v[r].d[0] & ~0xFFFFFFFFull) | singles[rnd() & 15]; break;
            case 2: c->v[r].d[0] = (c->v[r].d[0] & ~0xFFFFull) | halves[rnd() & 15]; break;
            case 3: c->v[r].d[0] = (doubles[rnd() & 15] & ~0xFFFull) | (rnd() & 0xFFFull); break;
            default: break;
        }
    }
    c->fpcr = s ? (u32)(rnd() & ((3u << 22) | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 19))) : 0;
    c->fpsr = (u32)(rnd() & 0x9Fu);
}

static int run_compare(const u32* words, size_t count, unsigned states) {
    char error[1024];
    LlvmSteps set[2];
    RtAPI rt = {.undef = on_undef, .sev = on_sev, .wfe = on_wfe, .cas128 = snx_runtime_cas128};
    if ((uintptr_t)g_test.memory % 16)
        return snx_error("the guest memory at %p is not 16-aligned: the 128-bit atomics would fault",
                         (void*)g_test.memory);
    for (size_t i = 0; i < MEMORY_SIZE; i += 8) {
        u64 v = rnd();
        memcpy(g_test.memory[2] + i, &v, 8);
    }
    memcpy(g_test.memory[0], g_test.memory[2], MEMORY_SIZE);
    memcpy(g_test.memory[1], g_test.memory[2], MEMORY_SIZE);
    for (int k = 0; k < 2; ++k) {
        if (llvm_steps_begin(&set[k], 0, LLVM_STEPS_HOST, error, sizeof error)) return snx_error("%s", error);
        if (k == 0) {
            set[k].ir->int_reference = snx_ir_int_reference;
            set[k].ir->operand_reference = snx_ir_int_operand_reference;
            set[k].ir->ldst_reference = snx_ir_ldst_reference;
            set[k].ir->fpu_reference = snx_ir_fpu_reference;
            set[k].ir->vec_reference = snx_ir_vec_reference;
        }
        for (size_t i = 0; i < count; ++i) {
            g_test.word = words[i];
            g_test.where = "the runs";
            llvm_steps_add(&set[k], words[i], 0x1000, word_target(words[i]));
        }
        g_test.where = NULL;
        if (llvm_steps_load(&set[k], error, sizeof error)) return snx_error("%s", error);
    }
    for (size_t i = 0; i < count; ++i)
        for (unsigned s = 0; s < states; ++s) {
            CPU start = {0}, end[2];
            u64 next[2];
            unsigned undef[2], sev[2], wfe[2];
            int ldst = is_ldst(words[i]), memory;
            for (unsigned r = 0; r < 32; ++r) start.x[r] = rnd();
            if (s & 1) start.x[(rnd() & 31)] = 0; /* zeros and edges reach the divisions and the shifts */
            if (s & 2) start.x[(rnd() & 31)] = 0x8000000000000000ull;
            if (s & 4) start.x[(rnd() & 31)] = ~0ull;
            start.n = rnd() & 1;
            start.z = rnd() & 1;
            start.cf = rnd() & 1;
            start.vf = rnd() & 1;
            start.excl_on = (u32)(rnd() & 1); /* CLREX clears it */
            fp_state(&start, s);
            if (ldst) ldst_start(&start, words[i], s);
            if (ldst && ordered_misaligned(&start, words[i])) {
                ++g_test.failures;
                fprintf(stderr, "FAIL: %s %08x state %u: its ordered access at %llx is off its alignment\n",
                        op_name(words[i]), (unsigned)words[i], s,
                        (unsigned long long)ordered_misaligned(&start, words[i]));
                break;
            }
            start.rt = &rt;
            start.pc = 0x1000;
            for (int k = 0; k < 2; ++k) {
                end[k] = start;
                end[k].mem = g_test.memory[k] + MEMORY_ZERO;
                g_test.undef = g_test.sev = g_test.wfe = 0;
                next[k] = set[k].steps[i](&end[k]);
                undef[k] = g_test.undef;
                sev[k] = g_test.sev;
                wfe[k] = g_test.wfe;
            }
            memory = ldst && memcmp(g_test.memory[0] + MEMORY_ZERO, g_test.memory[1] + MEMORY_ZERO, MEMORY_TOUCHED);
            if (ldst)
                for (int k = 0; k < 2; ++k)
                    memcpy(g_test.memory[k] + MEMORY_ZERO, g_test.memory[2] + MEMORY_ZERO, MEMORY_TOUCHED);
            if (next[0] != next[1] || undef[0] != undef[1] || sev[0] != sev[1] || wfe[0] != wfe[1] ||
                memcmp(end[0].x, end[1].x, sizeof end[0].x) || end[0].n != end[1].n || end[0].z != end[1].z ||
                end[0].cf != end[1].cf || end[0].vf != end[1].vf || end[0].pc != end[1].pc ||
                end[0].excl_on != end[1].excl_on || memory || memcmp(end[0].v, end[1].v, sizeof end[0].v) ||
                end[0].fpcr != end[1].fpcr || end[0].fpsr != end[1].fpsr) {
                ++g_test.failures;
                fprintf(stderr, "FAIL: %s %08x state %u: the generated emitter's result differs\n", op_name(words[i]),
                        (unsigned)words[i], s);
                break;
            }
        }
    for (int k = 0; k < 2; ++k) llvm_steps_end(&set[k]);
    return 0;
}

/* sailgen's back end ends the process on what it can't make (sg_fail, an abort): the word being emitted is named as a
   failure, before the abort goes on */
static void on_abort(int signal_number) {
    const char* name = g_test.where ? op_name(g_test.word) : NULL;
    (void)signal_number;
    if (g_test.where)
        fprintf(stderr, "FAIL: %s %08x (%s): the test stopped while it was emitted\n", name ? name : "UNDEF",
                (unsigned)g_test.word, g_test.where);
}

/* --time ROUNDS: what each emitter costs the recompile, the corpus's steps emitted and optimized (O2) ROUNDS times
   with each, alternating, on this host, in processor time; each round's line, so a chart shows the noise, then the
   best round of each, not a gate */
/* the processor time this process has used, in s (the emitting and optimizing run on this one thread) */
static double now_seconds(void) { return (double)snx_process_cpu_ns() / 1e9; }
static int time_emitters(const u32* words, size_t count, unsigned rounds) {
    double best[2] = {1e30, 1e30}, emit_best[2] = {1e30, 1e30};
    char error[1024];
    if (!snx_process_cpu_ns()) return snx_error("--time: this system gives no processor time (snx_process_cpu_ns)");
    for (unsigned r = 0; r < rounds; ++r)
        for (int k = 0; k < 2; ++k) {
            double start = now_seconds(), emitted, total;
            SnxIr* ir = snx_ir_new("timing", '2', error, sizeof error);
            if (!ir) return snx_error("%s", error);
            if (k == 0) {
                ir->int_reference = snx_ir_int_reference;
                ir->operand_reference = snx_ir_int_operand_reference;
                ir->ldst_reference = snx_ir_ldst_reference;
                ir->fpu_reference = snx_ir_fpu_reference;
                ir->vec_reference = snx_ir_vec_reference;
            }
            for (size_t i = 0; i < count; ++i) {
                char name[32];
                snprintf(name, sizeof name, "S_%zu", i);
                g_test.word = words[i];
                g_test.where = "--time";
                snx_ir_static_step(ir, words[i], 0x1000, word_target(words[i]), name);
                g_test.where = NULL;
            }
            emitted = now_seconds();
            if (snx_ir_module_optimize(ir, ir->mod, '2', error, sizeof error)) return snx_error("%s", error);
            total = now_seconds() - start;
            printf("round %u, %s: %.4f s (emit %.4f s)\n", r + 1, k ? "generated" : "hand-written", total,
                   emitted - start);
            if (total < best[k]) best[k] = total;
            if (emitted - start < emit_best[k]) emit_best[k] = emitted - start;
            snx_ir_free(ir);
        }
    printf("time, best of %u: hand-written %.3f s (emit %.3f s), generated %.3f s (emit %.3f s): %+.1f%% (emit "
           "%+.1f%%)\n",
           rounds, best[0], emit_best[0], best[1], emit_best[1], 100.0 * (best[1] - best[0]) / best[0],
           100.0 * (emit_best[1] - emit_best[0]) / emit_best[0]);
    return 0;
}

int main(int argc, char** argv) {
    unsigned samples = 4, states = 8, rounds = 0, part = 1, parts = 1;
    u32* words;
    size_t count, equal = 0, compared = 0;
    char error[1024];
    g_test.seed = 0x5a11c0de2026ull;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--report") && i + 1 < argc) {
            g_test.report = fopen(argv[++i], "w");
            if (!g_test.report) return snx_error("cannot write %s", argv[i]);
        } else if (!strcmp(argv[i], "--samples") && i + 1 < argc)
            samples = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--states") && i + 1 < argc)
            states = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--time") && i + 1 < argc)
            rounds = (unsigned)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--part") && i + 1 < argc) {
            char end;
            ++i;
            if (strspn(argv[i], "0123456789/") != strlen(argv[i]) ||
                sscanf(argv[i], "%u/%u%c", &part, &parts, &end) != 2 || !part || part > parts)
                return snx_error("--part %s is not K/N with 1 <= K <= N", argv[i]);
        } else
            return snx_error("unknown argument %s (--report FILE, --samples N, --states N, --time ROUNDS, --part K/N)",
                             argv[i]);
    }
    signal(SIGABRT, on_abort);
    if (snx_ir_init(error, sizeof error)) return snx_error("%s", error);
    /* the other paths' targets, and the SLP vectorizer off, as a game's module is made by default */
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    snx_ir_slp_vectorizer(0);
    count = corpus(samples, &words);
    printf("%zu words of the %zu generated ops\n", count, GENERATED_OPS + FP_OPS + VEC_OPS);
    for (size_t i = 0; i < GENERATED_OPS + FP_OPS + VEC_OPS; ++i) { /* each op has words, or it is compared nowhere */
        const char* name = i < GENERATED_OPS            ? generated_ops[i]
                           : i < GENERATED_OPS + FP_OPS ? fp_ops[i - GENERATED_OPS]
                                                        : vec_ops[i - GENERATED_OPS - FP_OPS];
        size_t k = 0;
        while (k < count && strcmp(op_name(words[k]), name)) ++k;
        if (k == count) {
            fprintf(stderr, "FAIL: %s has no word in the corpus\n", name);
            ++g_test.failures;
        }
    }
    if (rounds) {
        int r = time_emitters(words, count, rounds);
        free(words);
        return r;
    }
    if (parts > 1) {
        size_t n = 0;
        for (size_t i = part - 1; i < count; i += parts) words[n++] = words[i];
        count = n;
        printf("part %u of %u: %zu words\n", part, parts, count);
    }
    for (size_t p = 0; p < PATHS; ++p) {
        size_t before = equal;
        size_t outcomes[4];
        memcpy(outcomes, g_test.outcomes, sizeof outcomes);
        if (compare_path(&paths[p], words, count, &equal)) break;
        if (g_test.outcomes[SAME] == outcomes[SAME] && g_test.outcomes[REORDERED] == outcomes[REORDERED] &&
            g_test.outcomes[OTHER_NOT_LONGER] == outcomes[OTHER_NOT_LONGER] &&
            g_test.outcomes[LONGER] == outcomes[LONGER])
            continue; /* not compared, said */
        compared += count;
        printf(
            "path %s: of %zu words, %zu the same IR, %zu the same instructions reordered, %zu another IR not longer, "
            "%zu longer\n",
            paths[p].name, count, g_test.outcomes[SAME] - outcomes[SAME],
            g_test.outcomes[REORDERED] - outcomes[REORDERED],
            g_test.outcomes[OTHER_NOT_LONGER] - outcomes[OTHER_NOT_LONGER], g_test.outcomes[LONGER] - outcomes[LONGER]);
        (void)before;
    }
    if (part == 1) {
        for (unsigned s = 0; s < SEQUENCES; ++s) compare_sequence(s);
        printf("%zu sequences compared\n", SEQUENCES);
    }
    if (run_compare(words, count, states)) return 1;
    printf("%zu words run from %u states each\n", count, states);
    free(words);
    if (g_test.report) fclose(g_test.report);
    printf("sail IR: %s (%u failures; of %zu step IRs, %zu the same or reordered)\n",
           g_test.failures ? "FAILED" : "passed", g_test.failures, compared, equal);
    return g_test.failures != 0;
}
