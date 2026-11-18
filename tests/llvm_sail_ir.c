/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The integer, hint and barrier emitters and the branch operands tools/sailgen generates from the Sail (ir_sail.c)
   against the hand-written ones they replaced (support/ir_int_reference.c), word by word: the same IR, and the same
   results.
     snx-test-llvm-sail-ir [--report FILE] [--samples N] [--states N]
   The words: samples of every decoder row the generated subset covers (llvm_qemu_words.c), decoded for the ounce target
   (TARGET: the Switch's features with PAuth and FlagM), each again with its register fields 31 (the zero register or
   SP). On three paths (x86-64 with SSE4.2's CRC32C, AArch64 with CRC32, and the generic one other hosts take; a path
   whose target this LLVM lacks is said and skipped) each word becomes a step (snx_ir_static_step) in two modules, one
   emitted by each, optimized as a game's module is (snx_ir_module_optimize, O2, the SLP vectorizer off as the setting's
   default), and the two functions are compared. Then both sets of steps run on this host from random states, and every
   register, the flags, the exclusive monitor, the next pc, UNDEF and the runtime's calls for SEV and WFE must match.
   Sequences that hand flags or a register from one instruction to the next (SUBS then CSEL, CMP then CCMP then CSEL,
   ADDS then ADC, SUBS then B.HI, a load from a register an ADD just wrote, FCMP then CSEL) are emitted as functions
   (snx_ir_emit) and must give the hand-written emitters' IR, so a compare's flags still reach the condition as the
   compare, and a value one emitter writes is the one the other reads back, whichever of the two (generated or written
   by hand) wrote it. Both keep their state in one per-block cache (the back end's), so in a sequence the hand-written
   integer emitters run with no value carried through it from one instruction to the next (reference_uncached): a cache
   that kept a stale one shows in the generated side only. Two IRs that differ only in the order of their instructions
   and their names count as the same; a step's that differs otherwise may not have more instructions than the
   hand-written one, nor be another IR for an op whose effect running can't see (strict_ops), and is listed. --report
   writes each difference's two IR texts; --time ROUNDS only times the two emitters (time_emitters) */
#include "llvm_steps.h"
#include "llvm_qemu_words.h"
#include "support/ir_int_reference.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the ops ir_int.c sends to the Sail: the data-processing ones (generated()), the branches' operands
   (generated_branch()) */
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
    "PACIBZ",   "PACIBSP",  "AUTIAZ",  "AUTIASP",   "AUTIBZ",    "AUTIBSP",   "CLREX",     "DSB_DMB", "ISB"};
#define GENERATED_OPS (sizeof generated_ops / sizeof *generated_ops)

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
};
#define SEQUENCES (sizeof sequences / sizeof *sequences)

static struct {
    u64 seed;
    FILE* report;
    unsigned failures;
    unsigned undef, sev, wfe; /* the UNDEFs a step raised, its runtime calls for SEV and WFE */
    size_t outcomes[4];
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
static const char* op_name(u32 word) {
    SnxA64Insn insn;
    return snx_a64_decode_word(word, TARGET, &insn) != SNX_A64_OP_UNDEF ? snx_a64_op_text(insn.op) : NULL;
}

static int generated(u32 word) {
    const char* name = op_name(word);
    if (!name) return 0;
    for (size_t i = 0; i < GENERATED_OPS; ++i)
        if (!strcmp(name, generated_ops[i])) return 1;
    return 0;
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

/* one comparison: counted; a longer generated IR fails, and another IR of a strict op; the report gets every one that
   is not the same text */
static void judge(int r, const char* what, u32 word, const char* path, const char* a, const char* b,
                  const size_t* sizes) {
    ++g_test.outcomes[r];
    if (r == LONGER) {
        ++g_test.failures;
        fprintf(stderr, "FAIL: %s %08x (%s): the generated IR has %zu instructions, the hand-written %zu\n", what,
                (unsigned)word, path, sizes[1], sizes[0]);
    } else if (r == OTHER_NOT_LONGER && strict(what)) {
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
            fn[k][i] = snx_ir_static_step(ir[k], words[i], 0x1000, TARGET, name);
        }
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
        }
        snx_ir_emit(ir, &codegen, &function);
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

static int run_compare(const u32* words, size_t count, unsigned states) {
    char error[1024];
    LlvmSteps set[2];
    RtAPI rt = {.undef = on_undef, .sev = on_sev, .wfe = on_wfe, .cas128 = snx_runtime_cas128};
    for (int k = 0; k < 2; ++k) {
        if (llvm_steps_begin(&set[k], 0, LLVM_STEPS_HOST, error, sizeof error)) return snx_error("%s", error);
        if (k == 0) {
            set[k].ir->int_reference = snx_ir_int_reference;
            set[k].ir->operand_reference = snx_ir_int_operand_reference;
        }
        for (size_t i = 0; i < count; ++i) llvm_steps_add(&set[k], words[i], 0x1000, TARGET);
        if (llvm_steps_load(&set[k], error, sizeof error)) return snx_error("%s", error);
    }
    for (size_t i = 0; i < count; ++i)
        for (unsigned s = 0; s < states; ++s) {
            CPU start = {0}, end[2];
            u64 next[2];
            unsigned undef[2], sev[2], wfe[2];
            for (unsigned r = 0; r < 32; ++r) start.x[r] = rnd();
            if (s & 1) start.x[(rnd() & 31)] = 0; /* zeros and edges reach the divisions and the shifts */
            if (s & 2) start.x[(rnd() & 31)] = 0x8000000000000000ull;
            if (s & 4) start.x[(rnd() & 31)] = ~0ull;
            start.n = rnd() & 1;
            start.z = rnd() & 1;
            start.cf = rnd() & 1;
            start.vf = rnd() & 1;
            start.excl_on = (u32)(rnd() & 1); /* CLREX clears it */
            start.rt = &rt;
            start.pc = 0x1000;
            for (int k = 0; k < 2; ++k) {
                end[k] = start;
                g_test.undef = g_test.sev = g_test.wfe = 0;
                next[k] = set[k].steps[i](&end[k]);
                undef[k] = g_test.undef;
                sev[k] = g_test.sev;
                wfe[k] = g_test.wfe;
            }
            if (next[0] != next[1] || undef[0] != undef[1] || sev[0] != sev[1] || wfe[0] != wfe[1] ||
                memcmp(end[0].x, end[1].x, sizeof end[0].x) || end[0].n != end[1].n || end[0].z != end[1].z ||
                end[0].cf != end[1].cf || end[0].vf != end[1].vf || end[0].pc != end[1].pc ||
                end[0].excl_on != end[1].excl_on) {
                ++g_test.failures;
                fprintf(stderr, "FAIL: %s %08x state %u: the generated emitter's result differs\n", op_name(words[i]),
                        (unsigned)words[i], s);
                break;
            }
        }
    for (int k = 0; k < 2; ++k) llvm_steps_end(&set[k]);
    return 0;
}

/* --time ROUNDS: what each emitter costs the recompile, the corpus's steps emitted and optimized (O2) ROUNDS times
   with each, alternating, on this host, in processor time; the best round of each, not a gate */
/* the processor time this process has used, in s (the emitting and optimizing run on this one thread) */
static double now_seconds(void) { return (double)snx_process_cpu_ns() / 1e9; }
static int time_emitters(const u32* words, size_t count, unsigned rounds) {
    double best[2] = {1e30, 1e30}, emit_best[2] = {1e30, 1e30};
    char error[1024];
    if (!snx_process_cpu_ns()) return snx_error("--time: this system gives no processor time (snx_process_cpu_ns)");
    for (unsigned r = 0; r < rounds; ++r)
        for (int k = 0; k < 2; ++k) {
            double start = now_seconds(), emitted;
            SnxIr* ir = snx_ir_new("timing", '2', error, sizeof error);
            if (!ir) return snx_error("%s", error);
            if (k == 0) {
                ir->int_reference = snx_ir_int_reference;
                ir->operand_reference = snx_ir_int_operand_reference;
            }
            for (size_t i = 0; i < count; ++i) {
                char name[32];
                snprintf(name, sizeof name, "S_%zu", i);
                snx_ir_static_step(ir, words[i], 0x1000, TARGET, name);
            }
            emitted = now_seconds();
            if (snx_ir_module_optimize(ir, ir->mod, '2', error, sizeof error)) return snx_error("%s", error);
            if (now_seconds() - start < best[k]) best[k] = now_seconds() - start;
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
    unsigned samples = 4, states = 8, rounds = 0;
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
        else
            return snx_error("unknown argument %s (--report FILE, --samples N, --states N, --time ROUNDS)", argv[i]);
    }
    if (snx_ir_init(error, sizeof error)) return snx_error("%s", error);
    /* the other paths' targets, and the SLP vectorizer off, as a game's module is made by default */
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmPrinters();
    snx_ir_slp_vectorizer(0);
    count = corpus(samples, &words);
    printf("%zu words of the %zu generated ops\n", count, GENERATED_OPS);
    for (size_t i = 0; i < GENERATED_OPS; ++i) { /* each op has words, or it is compared nowhere */
        size_t k = 0;
        while (k < count && strcmp(op_name(words[k]), generated_ops[i])) ++k;
        if (k == count) {
            fprintf(stderr, "FAIL: %s has no word in the corpus\n", generated_ops[i]);
            ++g_test.failures;
        }
    }
    if (rounds) {
        int r = time_emitters(words, count, rounds);
        free(words);
        return r;
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
    for (unsigned s = 0; s < SEQUENCES; ++s) compare_sequence(s);
    printf("%zu sequences compared\n", SEQUENCES);
    if (run_compare(words, count, states)) return 1;
    printf("%zu words run from %u states each\n", count, states);
    free(words);
    if (g_test.report) fclose(g_test.report);
    printf("sail IR: %s (%u failures; of %zu step IRs, %zu the same or reordered)\n",
           g_test.failures ? "FAILED" : "passed", g_test.failures, compared, equal);
    return g_test.failures != 0;
}
