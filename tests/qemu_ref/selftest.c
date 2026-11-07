/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The QEMU reference's self-test, under -cpu max and -cpu cortex-a57: hand-checked cases, the ported cases of
   a64_ported_cases.h with their expected values, and the decoder's UNDEF sets (decodetree's Switch and max targets)
   against where each CPU raises SIGILL, a few words of every row of the decoder's table and random words. The
   mismatches of that last part are listed (in qemu_ref_undef.txt too). Under max they don't fail the test: they are
   what the profile is fixed from. The Switch target must match the Cortex-A57 but for SYS, whose system registers
   the emitter decides (a64.profile).
   Usage: snx-test-qemu-ref QEMU RUNNER DIR, or snx-test-qemu-ref --skip REASON (exits 77: ctest's skip) */
#include "qemu_ref.h"

#include "../a64_ported_cases.h"
#include "common/utilities/common.h"
#include "a64_decode.h"
#include "a64_decode.inc"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORTED_CODE   0x20000000ull
#define PORTED_WINDOW 0x4000u
#define FREE_RUN_FROM 100000u /* a RUN of more steps runs free: the program ends by leaving its code */
#define ROW_SAMPLES   6
#define RANDOM_WORDS  20000

static const char* const g_cpus[2] = {"max", "cortex-a57"};

static struct {
    const char *qemu, *runner, *dir;
    int failures, checks, runs;
    double seconds;
    uint64_t rng;
    FILE* report;
} g_test;

static void check(int ok, const char* cpu, const char* what) {
    ++g_test.checks;
    if (ok) return;
    ++g_test.failures;
    fprintf(stderr, "FAIL [%s] %s\n", cpu, what);
}

static uint64_t next_random(void) { return snx_qref_splitmix(g_test.rng++); }

static int run(SnxQrefBatch* b, const char* cpu) {
    char error[512];
    double t0 = snx_seconds();
    if (snx_qref_run(b, g_test.qemu, g_test.runner, cpu, g_test.dir, error, sizeof(error))) {
        fprintf(stderr, "FAIL [%s] the runner: %s\n", cpu, error);
        ++g_test.failures;
        return -1;
    }
    g_test.seconds += snx_seconds() - t0;
    g_test.runs += (int)b->cases;
    return 0;
}

/* the 8 bytes at address after a case: the seed, then the case's changes */
static uint64_t memory64(const SnxQrefBatch* b, const SnxQrefCase* c, const SnxQrefResult* r, uint64_t address) {
    uint64_t v = 0;
    unsigned i;
    for (i = 0; i < 8; ++i) {
        uint64_t a = address + i;
        uint8_t byte = snx_qref_seed_byte(c->seed_kind, c->seed, c->window, a);
        uint32_t k;
        for (k = 0; k < r->changes; ++k) {
            const SnxQrefChange* ch = &b->changes[r->first_change + k];
            if (a - ch->address < ch->size) byte = b->bytes[ch->offset + (a - ch->address)];
        }
        v |= (uint64_t)byte << (8 * i);
    }
    return v;
}

/* the hand-checked cases */
enum {
    H_ADDS,
    H_BEQ_TAKEN,
    H_BEQ_NOT,
    H_BNE_BACK,
    H_LDR,
    H_STR,
    H_SVC,
    H_BRK,
    H_FADD,
    H_FDIV,
    H_CASAL,
    H_SIMD,
    H_SMC_ADD,
    H_SMC_SUB,
    H_FAULT,
    H_LOOP,
    H_COUNT
};

static void hand(const char* cpu) {
    static const uint32_t words[H_COUNT] = {
        0xAB020020u, /* adds x0, x1, x2 */
        0x54000200u, /* b.eq .+0x40 */
        0x54000200u, /* b.eq .+0x40 */
        0x54FFFE01u, /* b.ne .-0x40 */
        0xF9400420u, /* ldr x0, [x1, #8] */
        0xF9000822u, /* str x2, [x1, #16] */
        0xD40008A1u, /* svc #0x45 */
        0xD4200240u, /* brk #0x12 */
        0x1E222820u, /* fadd s0, s1, s2 */
        0x1E231820u, /* fdiv s0, s1, s3 */
        0xC8E0FC41u, /* casal x0, x1, [x2] */
        0x4EA28420u, /* add v0.4s, v1.4s, v2.4s */
        0x91000420u, /* add x0, x1, #1 */
        0xD1000420u, /* sub x0, x1, #1: the same pc as the add, so QEMU must translate it anew */
        0xF9400020u, /* ldr x0, [x1] */
        0xD61F0020u, /* br x1: to itself */
    };
    const int max = !strcmp(cpu, "max");
    const uint64_t data = SNX_QREF_WINDOW + 0x2000, pc = SNX_QREF_WORD_PC;
    SnxQrefBatch b = {0};
    SnxQrefCase cases[H_COUNT];
    char what[160];
    int i;
    for (i = 0; i < H_COUNT; ++i) {
        SnxQrefCase* c = &cases[i];
        snx_qref_case_init(c);
        c->seed = 7;
        switch (i) {
        case H_ADDS:
            c->state.x[1] = 0x7FFFFFFFFFFFFFFFull;
            c->state.x[2] = 1;
            break;
        case H_BEQ_TAKEN: c->state.nzcv = 0x40000000u; break;
        case H_LDR:
        case H_STR:
            c->state.x[1] = data;
            c->state.x[2] = 0x0123456789ABCDEFull;
            break;
        case H_FADD:
            c->state.v[1][0] = 0x3F800000u; /* 1 */
            c->state.v[2][0] = 0x33000001u; /* just over 2^-25: inexact */
            break;
        case H_FDIV: c->state.v[1][0] = 0x3F800000u; break;
        case H_CASAL:
            c->state.x[2] = data;
            c->state.x[0] = snx_qref_seed64(c->seed_kind, c->seed, c->window, data - c->window);
            c->state.x[1] = 0xFEEDFACECAFEBEEFull;
            break;
        case H_SIMD:
            c->state.v[1][0] = 0x0000000200000001ull;
            c->state.v[1][1] = 0xFFFFFFFF00000003ull;
            c->state.v[2][0] = 0x0000001000000010ull;
            c->state.v[2][1] = 0x0000000100000010ull;
            break;
        case H_SMC_ADD:
        case H_SMC_SUB: c->state.x[1] = 100; break;
        case H_FAULT: c->state.x[1] = 8; break; /* nothing is mapped at 8 */
        case H_LOOP: c->state.x[1] = pc; break;
        }
        if (snx_qref_add(&b, c, &words[i], 1, NULL, 0) != i) check(0, cpu, "add a hand-checked case");
    }
    if (run(&b, cpu)) {
        snx_qref_free(&b);
        return;
    }
#define R(n) (&b.results[n])
    check(R(H_ADDS)->outcome == SNX_QREF_NEXT && R(H_ADDS)->pc == pc + 4, cpu, "ADDS falls through");
    check(R(H_ADDS)->state.x[0] == 0x8000000000000000ull && R(H_ADDS)->state.nzcv == 0x90000000u, cpu,
          "ADDS: the sum and N, V set");
    check(R(H_BEQ_TAKEN)->outcome == SNX_QREF_BRANCH && R(H_BEQ_TAKEN)->pc == pc + 0x40, cpu, "B.EQ taken");
    check(R(H_BEQ_NOT)->outcome == SNX_QREF_NEXT && R(H_BEQ_NOT)->pc == pc + 4, cpu, "B.EQ not taken");
    check(R(H_BNE_BACK)->outcome == SNX_QREF_BRANCH && R(H_BNE_BACK)->pc == pc - 0x40, cpu,
          "B.NE back into the window's data, which isn't executable");
    check(R(H_LDR)->state.x[0] == memory64(&b, &cases[H_LDR], R(H_LDR), data + 8), cpu, "LDR reads the seed");
    check(R(H_LDR)->changes == 0, cpu, "LDR changes no memory");
    check(R(H_STR)->changes == 1 && b.changes[R(H_STR)->first_change].address == data + 16 &&
              b.changes[R(H_STR)->first_change].size == 8 &&
              memory64(&b, &cases[H_STR], R(H_STR), data + 16) == 0x0123456789ABCDEFull,
          cpu, "STR: the 8 bytes it wrote, and nothing else");
    check(R(H_SVC)->outcome == SNX_QREF_SVC && R(H_SVC)->imm == 0x45 && R(H_SVC)->pc == pc + 4 && R(H_SVC)->svcs == 1,
          cpu, "SVC #0x45 recorded and resumed");
    check(R(H_BRK)->outcome == SNX_QREF_BRK && R(H_BRK)->imm == 0x12 && R(H_BRK)->pc == pc, cpu, "BRK #0x12");
    check(R(H_FADD)->state.fpsr == 0x10 && R(H_FADD)->state.v[0][0] == 0x3F800000u, cpu, "FADD inexact sets IXC");
    check(R(H_FDIV)->state.fpsr == 0x2 && R(H_FDIV)->state.v[0][0] == 0x7F800000u, cpu, "FDIV by zero sets DZC");
    if (max) {
        check(R(H_CASAL)->outcome == SNX_QREF_NEXT && R(H_CASAL)->changes == 1 &&
                  memory64(&b, &cases[H_CASAL], R(H_CASAL), data) == 0xFEEDFACECAFEBEEFull,
              cpu, "CASAL swaps (LSE)");
    } else {
        check(R(H_CASAL)->outcome == SNX_QREF_UNDEF && R(H_CASAL)->pc == pc && R(H_CASAL)->changes == 0, cpu,
              "CASAL is UNDEF without LSE");
    }
    check(R(H_SIMD)->state.v[0][0] == 0x0000001200000011ull && R(H_SIMD)->state.v[0][1] == 0x0000000000000013ull, cpu,
          "ADD .4S");
    check(R(H_SMC_ADD)->state.x[0] == 101 && R(H_SMC_SUB)->state.x[0] == 99, cpu,
          "a new word at the same pc runs as itself");
    check(R(H_FAULT)->outcome == SNX_QREF_FAULT && R(H_FAULT)->fault == 8 && R(H_FAULT)->pc == pc, cpu,
          "LDR from an unmapped address faults");
    check(R(H_LOOP)->outcome == SNX_QREF_TIMEOUT, cpu, "a loop times out");
#undef R
    for (i = 0; i < H_COUNT; ++i) {
        if (b.results[i].outcome != SNX_QREF_ERROR) continue;
        snprintf(what, sizeof(what), "hand-checked case %d could not run (error %llu)", i,
                 (unsigned long long)b.results[i].imm);
        check(0, cpu, what);
    }
    snx_qref_free(&b);
}

/* the ported cases: each RUN of a case is a case of a batch, a round of batches for the cases' first RUNs, then
   their second, ...; the state and window go from one RUN's results to the next */
typedef struct {
    size_t op;             /* the next op */
    size_t first, count;   /* the code words: ops first.. */
    int done, skipped;
    uint64_t base, pc;     /* the window's address; the pc the last RUN went to (in the code: its offset) */
    uint32_t kind;
    int batch_index;
    SnxQrefState state, want;
    uint8_t window[PORTED_WINDOW], want_window[PORTED_WINDOW];
} Ported;

static uint32_t ported_kind(unsigned reg) { return reg ? SNX_QREF_SEED_ZERO : SNX_QREF_SEED_ADDRESS; }

static void ported_memory(Ported* p, uint64_t base, uint32_t kind) {
    uint32_t i;
    p->base = base;
    p->kind = kind;
    for (i = 0; i < PORTED_WINDOW; ++i) p->window[i] = snx_qref_seed_byte(kind, 0, base, base + i);
    /* the guest has 47 bits under qemu's -B, and the code its own region */
    p->skipped = base >= (1ull << 44) || (base < PORTED_CODE + 0x10000 && base + PORTED_WINDOW > PORTED_CODE);
}

static uint8_t* ported_at(Ported* p, uint8_t* window, uint64_t address, uint64_t size) {
    if (address - p->base >= PORTED_WINDOW || address - p->base + size > PORTED_WINDOW) return NULL;
    return window + (address - p->base);
}

static void ported_fail(const char* cpu, const char* name, const char* what, uint64_t got, uint64_t want) {
    char text[256];
    snprintf(text, sizeof(text), "%s: %s is %016llx, expected %016llx", name, what, (unsigned long long)got,
             (unsigned long long)want);
    check(0, cpu, text);
}

/* the script's ops of case p until its next RUN (added to the batch) or its end */
static void ported_advance(Ported* p, SnxQrefBatch* b, const char* cpu) {
    const char* name = snx_a64p_script[p->first - 1].text;
    char what[64];
    for (; !p->done; ++p->op) {
        const SnxA64pOp* op = &snx_a64p_script[p->op];
        uint8_t *x, *y;
        unsigned i;
        if (op->op == SNX_A64P_OP_END || op->op == SNX_A64P_OP_CASE) {
            p->done = 1;
            break;
        }
        switch (op->op) {
        case SNX_A64P_OP_CODE: break;
        case SNX_A64P_OP_SET_X:
            if (op->reg == 31)
                p->state.sp = op->a;
            else
                p->state.x[op->reg] = op->a;
            break;
        case SNX_A64P_OP_SET_V:
            p->state.v[op->reg][0] = op->a;
            p->state.v[op->reg][1] = op->b;
            break;
        case SNX_A64P_OP_SET_FPCR: p->state.fpcr = (uint32_t)op->a; break;
        case SNX_A64P_OP_SET_FPSR: p->state.fpsr = (uint32_t)op->a; break;
        case SNX_A64P_OP_SET_NZCV: p->state.nzcv = (uint32_t)op->a & 0xF0000000u; break;
        case SNX_A64P_OP_CLEAR_V: memset(p->state.v, 0, sizeof(p->state.v)); break;
        case SNX_A64P_OP_MEMORY: ported_memory(p, op->a, ported_kind(op->reg)); break;
        case SNX_A64P_OP_SET_MEM:
            x = ported_at(p, p->window, op->a, strlen(op->text) + 1);
            if (x) memcpy(x, op->text, strlen(op->text) + 1);
            break;
        case SNX_A64P_OP_PRESET:
            ported_memory(p, 0, SNX_QREF_SEED_NCE);
            for (i = 0; i < 31; ++i) p->state.x[i] = 0xDEAD000000000000ull | i;
            p->state.x[0] = 0x1000;
            p->state.x[1] = 0x1800;
            p->state.x[2] = 0x30;
            p->state.sp = 0x2EE0;
            for (i = 0; i < 32; ++i) {
                p->state.v[i][0] = 0x5555000000000000ull | i;
                p->state.v[i][1] = 0xAAAA000000000000ull | i;
            }
            break;
        case SNX_A64P_OP_RUN: {
            SnxQrefCase c;
            SnxQrefPatch patch;
            uint32_t words[64];
            size_t k;
            if (p->skipped) {
                p->done = 1;
                break;
            }
            for (k = 0; k < p->count && k < 64; ++k) words[k] = (uint32_t)snx_a64p_script[p->first + k].a;
            snx_qref_case_init(&c);
            c.window = p->base;
            c.window_size = PORTED_WINDOW;
            c.seed_kind = p->kind;
            c.seed = 0;
            c.code = PORTED_CODE;
            c.code_size = (p->count * 4 + 8 + SNX_QREF_PAGE - 1) & ~(SNX_QREF_PAGE - 1);
            c.pc = PORTED_CODE + op->a;
            c.steps = op->b > FREE_RUN_FROM ? 0 : op->b;
            c.flags = SNX_QREF_FULL_DIFF;
            c.state = p->state;
            patch.address = p->base;
            patch.bytes = p->window;
            patch.size = PORTED_WINDOW;
            p->batch_index = snx_qref_add(b, &c, words, (uint32_t)p->count, &patch, 1);
            check(p->batch_index >= 0 && p->count <= 64, cpu, "add a ported case");
            ++p->op;
            return;
        }
        case SNX_A64P_OP_X:
            if ((p->state.x[op->reg] & op->b) != op->a) {
                snprintf(what, sizeof(what), "X%u", op->reg);
                ported_fail(cpu, name, what, p->state.x[op->reg] & op->b, op->a);
            } else {
                check(1, cpu, name);
            }
            break;
        case SNX_A64P_OP_V:
            snprintf(what, sizeof(what), "V%u low", op->reg);
            if (p->state.v[op->reg][0] != op->a) ported_fail(cpu, name, what, p->state.v[op->reg][0], op->a);
            snprintf(what, sizeof(what), "V%u high", op->reg);
            if (p->state.v[op->reg][1] != op->b) ported_fail(cpu, name, what, p->state.v[op->reg][1], op->b);
            check(1, cpu, name);
            break;
        case SNX_A64P_OP_PC:
            if (p->pc != op->a) ported_fail(cpu, name, "PC", p->pc, op->a);
            break;
        case SNX_A64P_OP_FPSR:
            if ((p->state.fpsr & op->b) != op->a) ported_fail(cpu, name, "FPSR", p->state.fpsr & op->b, op->a);
            break;
        case SNX_A64P_OP_NZCV:
            if (p->state.nzcv != op->a) ported_fail(cpu, name, "NZCV", p->state.nzcv, op->a);
            break;
        case SNX_A64P_OP_MEM64: {
            uint64_t v = 0;
            x = ported_at(p, p->window, op->a, 8);
            for (i = 0; x && i < 8; ++i) v |= (uint64_t)x[i] << (8 * i);
            if (!x || v != op->b) ported_fail(cpu, name, "memory", v, op->b);
            break;
        }
        case SNX_A64P_OP_MEM_SAME:
            x = ported_at(p, p->window, op->a, op->reg);
            y = ported_at(p, p->window, op->b, op->reg);
            if (!x || !y || memcmp(x, y, op->reg)) ported_fail(cpu, name, "the copied bytes", 0, 1);
            break;
        case SNX_A64P_OP_MARK:
            p->want = p->state;
            memcpy(p->want_window, p->window, PORTED_WINDOW);
            break;
        case SNX_A64P_OP_WANT_X:
            if (op->reg == 31)
                p->want.sp = op->a;
            else
                p->want.x[op->reg] = op->a;
            break;
        case SNX_A64P_OP_WANT_V:
            p->want.v[op->reg][0] = op->a;
            p->want.v[op->reg][1] = op->b;
            break;
        case SNX_A64P_OP_WANT_MEM:
            x = ported_at(p, p->want_window, op->a, op->reg);
            for (i = 0; x && i < op->reg && i < 8; ++i) x[i] = (uint8_t)(op->b >> (8 * i));
            break;
        case SNX_A64P_OP_SAME:
            for (i = 0; i < 31; ++i)
                if (p->state.x[i] != p->want.x[i]) {
                    snprintf(what, sizeof(what), "X%u", i);
                    ported_fail(cpu, name, what, p->state.x[i], p->want.x[i]);
                }
            if (p->state.sp != p->want.sp) ported_fail(cpu, name, "SP", p->state.sp, p->want.sp);
            if (memcmp(p->state.v, p->want.v, sizeof(p->state.v))) ported_fail(cpu, name, "the V registers", 0, 0);
            for (i = 0; i < PORTED_WINDOW; ++i)
                if (p->window[i] != p->want_window[i]) {
                    snprintf(what, sizeof(what), "the byte at %llx", (unsigned long long)(p->base + i));
                    ported_fail(cpu, name, what, p->window[i], p->want_window[i]);
                    break;
                }
            check(1, cpu, name);
            break;
        default: check(0, cpu, "an unknown op in the ported script");
        }
    }
}

/* takes a RUN's result into the case */
static void ported_result(Ported* p, const SnxQrefBatch* b, const char* cpu) {
    const SnxQrefResult* r = &b->results[p->batch_index];
    const char* name = snx_a64p_script[p->first - 1].text;
    char what[160];
    uint32_t k;
    if (r->outcome != SNX_QREF_NEXT && r->outcome != SNX_QREF_BRANCH) {
        snprintf(what, sizeof(what), "%s: ended by %s at %llx (signal %u, fault %llx)", name,
                 snx_qref_outcome_name(r->outcome), (unsigned long long)r->pc, r->signo,
                 (unsigned long long)r->fault);
        check(0, cpu, what);
    }
    p->state = r->state;
    p->pc = r->pc - PORTED_CODE < 0x10000 ? r->pc - PORTED_CODE : r->pc;
    for (k = 0; k < r->changes; ++k) {
        const SnxQrefChange* c = &b->changes[r->first_change + k];
        uint8_t* at = ported_at(p, p->window, c->address, c->size);
        if (at) memcpy(at, b->bytes + c->offset, c->size);
        else
            check(0, cpu, "a ported case changed memory outside its window");
    }
}

static void ported(const char* cpu) {
    size_t i, n = 0, ops = sizeof(snx_a64p_script) / sizeof(snx_a64p_script[0]);
    Ported* cases;
    SnxQrefBatch b = {0};
    int skipped = 0, rounds = 0, pending = 1, before = g_test.failures;
    for (i = 0; i < ops; ++i) n += snx_a64p_script[i].op == SNX_A64P_OP_CASE;
    cases = calloc(n, sizeof(*cases));
    if (!cases) {
        check(0, cpu, "out of memory");
        return;
    }
    for (i = 0, n = 0; i < ops; ++i) {
        size_t k;
        if (snx_a64p_script[i].op != SNX_A64P_OP_CASE) continue;
        cases[n].first = i + 1;
        for (k = i + 1; k < ops && snx_a64p_script[k].op == SNX_A64P_OP_CODE; ++k) ++cases[n].count;
        cases[n].op = i + 1;
        ported_memory(&cases[n], 0, SNX_QREF_SEED_ADDRESS);
        ++n;
    }
    while (pending) {
        pending = 0;
        snx_qref_clear(&b);
        for (i = 0; i < n; ++i) {
            cases[i].batch_index = -1;
            ported_advance(&cases[i], &b, cpu);
        }
        if (!b.cases) break;
        if (run(&b, cpu)) break;
        ++rounds;
        for (i = 0; i < n; ++i)
            if (cases[i].batch_index >= 0) {
                ported_result(&cases[i], &b, cpu);
                pending = 1;
            }
    }
    for (i = 0; i < n; ++i) skipped += cases[i].skipped;
    printf("[%s] ported cases: %zu, %d of them skipped (a window above the guest's address space), %d rounds, %s\n",
           cpu, n, skipped, rounds, g_test.failures == before ? "all as expected" : "FAILED");
    snx_qref_free(&b);
    free(cases);
}

/* fp_min_max.cpp's table and FPToFixed.cpp's conversions, one word each */
static void ported_fp(const char* cpu) {
    static const uint32_t bases[2] = {0, 0x01000000u}; /* FZ */
    const size_t rows = sizeof(snx_a64p_minmax) / sizeof(snx_a64p_minmax[0]);
    const size_t words = sizeof(snx_a64p_minmax_words) / sizeof(snx_a64p_minmax_words[0]);
    const size_t fixed = sizeof(snx_a64p_fixed) / sizeof(snx_a64p_fixed[0]);
    SnxQrefBatch b = {0};
    size_t w, r, k = 0;
    int base, dn, swap, before = g_test.failures;
    char what[160];
    SnxQrefCase c;
    snx_qref_case_init(&c);
    c.window_size = 0;
    c.code = PORTED_CODE;
    c.pc = PORTED_CODE;
    for (w = 0; w < words; ++w)
        for (base = 0; base < 2; ++base)
            for (dn = 0; dn < 2; ++dn)
                for (r = 0; r < rows; ++r)
                    for (swap = 0; swap < (snx_a64p_minmax[r].one_way ? 1 : 2); ++swap) {
                        memset(&c.state, 0, sizeof(c.state));
                        c.state.fpcr = bases[base] | (dn ? 0x02000000u : 0);
                        c.state.v[0][0] = 42;
                        c.state.v[1][0] = swap ? snx_a64p_minmax[r].b : snx_a64p_minmax[r].a;
                        c.state.v[2][0] = swap ? snx_a64p_minmax[r].a : snx_a64p_minmax[r].b;
                        snx_qref_add(&b, &c, &snx_a64p_minmax_words[w].word, 1, NULL, 0);
                    }
    for (r = 0; r < fixed; ++r) {
        static const uint32_t rmode[5] = {0x00000u, 0x80000u, 0x100000u, 0x180000u, 0x40000u}; /* N P M Z A */
        const SnxA64pFixed* f = &snx_a64p_fixed[r];
        uint32_t word = 0x1E200000u | (f->int_width == 64 ? 0x80000000u : 0) | (f->fp_width == 64 ? 0x400000u : 0) |
                        rmode[f->rm]; /* FCVT<r>S Rd0, Fn0 */
        memset(&c.state, 0, sizeof(c.state));
        c.state.v[0][0] = f->input;
        snx_qref_add(&b, &c, &word, 1, NULL, 0);
    }
    if (run(&b, cpu)) {
        snx_qref_free(&b);
        return;
    }
    for (w = 0; w < words; ++w)
        for (base = 0; base < 2; ++base)
            for (dn = 0; dn < 2; ++dn)
                for (r = 0; r < rows; ++r)
                    for (swap = 0; swap < (snx_a64p_minmax[r].one_way ? 1 : 2); ++swap, ++k) {
                        const SnxQrefResult* res = &b.results[k];
                        uint32_t want = snx_a64p_minmax[r].result[snx_a64p_minmax_words[w].column];
                        if (dn && (want & 0x7F800000u) == 0x7F800000u && (want & 0x007FFFFFu)) want = 0x7FC00000u;
                        if (res->outcome != SNX_QREF_NEXT || res->state.v[0][0] != want) {
                            snprintf(what, sizeof(what), "%s of %08llx, %08llx under FPCR %08x: %llx, expected %08x",
                                     snx_a64p_minmax_words[w].text, (unsigned long long)res->state.v[1][0],
                                     (unsigned long long)res->state.v[2][0], res->state.fpcr,
                                     (unsigned long long)res->state.v[0][0], want);
                            check(0, cpu, what);
                        } else {
                            check(1, cpu, "min/max");
                        }
                    }
    for (r = 0; r < fixed; ++r, ++k) {
        const SnxA64pFixed* f = &snx_a64p_fixed[r];
        const SnxQrefResult* res = &b.results[k];
        if (res->state.x[0] != f->output || (f->fpsr != ~0u && res->state.fpsr != f->fpsr)) {
            snprintf(what, sizeof(what), "FPToFixed %016llx to %d bits rounding %d: %llx, FPSR %x",
                     (unsigned long long)f->input, f->int_width, f->rm, (unsigned long long)res->state.x[0],
                     res->state.fpsr);
            check(0, cpu, what);
        } else {
            check(1, cpu, "FPToFixed");
        }
    }
    printf("[%s] min/max and FPToFixed: %u words run, %s\n", cpu, b.cases,
           g_test.failures == before ? "all as expected" : "FAILED");
    snx_qref_free(&b);
}

/* the UNDEF cross-check */
#define ROWS (sizeof(snx_a64_rows) / sizeof(snx_a64_rows[0]))
#define CROSS_WORDS (ROWS * ROW_SAMPLES + RANDOM_WORDS)

/* a mismatch group: the decoder said one thing, QEMU the other, for words of this op; ops past the decoder's are
   the spaces of words no row decodes (SPACES) */
typedef struct {
    uint32_t op, decoded; /* decoded: the decoder decoded it and QEMU raised SIGILL; else the reverse */
    uint32_t count, example;
    uint64_t feature;
} Mismatch;

static const char* const g_spaces[3] = {"(SVE space)", "(SME space)", "(elsewhere)"};

/* a word no row decodes: SVE's encodings have op0 (bits 28-25) 0010, SME's bit 31 set and op0 0000 */
static uint32_t space(uint32_t w) {
    const uint32_t op0 = (w >> 25) & 0xF;
    return SNX_A64_OP_COUNT + (op0 == 2 ? 0 : (w >> 31) && !op0 ? 1 : 2);
}

static const char* group_name(uint32_t op) {
    return op >= SNX_A64_OP_COUNT ? g_spaces[op - SNX_A64_OP_COUNT] : snx_a64_op_name((SnxA64Op)op);
}

/* the FEAT_* names of a feature set, into text */
static void feature_names(uint64_t feature, char* text, size_t size) {
#define SNX_FEAT(name) {SNX_A64_FEAT_##name, #name}
    static const struct {
        uint64_t bit;
        const char* name;
    } names[] = {
        SNX_FEAT(V8_0),     SNX_FEAT(CRC32),   SNX_FEAT(AES),     SNX_FEAT(PMULL),    SNX_FEAT(SHA1),
        SNX_FEAT(SHA256),   SNX_FEAT(LSE),     SNX_FEAT(RDM),     SNX_FEAT(LOR),      SNX_FEAT(PAN),
        SNX_FEAT(FP16),     SNX_FEAT(DOTPROD), SNX_FEAT(FHM),     SNX_FEAT(SHA512),   SNX_FEAT(SHA3),
        SNX_FEAT(SM3),      SNX_FEAT(SM4),     SNX_FEAT(UAO),     SNX_FEAT(LRCPC),    SNX_FEAT(JSCVT),
        SNX_FEAT(FCMA),     SNX_FEAT(PAUTH),   SNX_FEAT(LRCPC2),  SNX_FEAT(DIT),      SNX_FEAT(FLAGM),
        SNX_FEAT(FLAGM2),   SNX_FEAT(FRINTTS), SNX_FEAT(SB),      SNX_FEAT(SSBS),     SNX_FEAT(MTE),
        SNX_FEAT(MTE2),     SNX_FEAT(BF16),    SNX_FEAT(I8MM),    SNX_FEAT(RAS),      SNX_FEAT(XS),
        SNX_FEAT(WFXT),     SNX_FEAT(HBC),     SNX_FEAT(MOPS),    SNX_FEAT(NMI),      SNX_FEAT(CSSC),
        SNX_FEAT(SME),      SNX_FEAT(GCS),     SNX_FEAT(CHK),     SNX_FEAT(LSE128),   SNX_FEAT(FAMINMAX),
        SNX_FEAT(LUT),      SNX_FEAT(FP8),     SNX_FEAT(FP8FMA),  SNX_FEAT(FP8DOT4),  SNX_FEAT(FP8DOT2),
        SNX_FEAT(F8F32MM),  SNX_FEAT(F8F16MM), SNX_FEAT(CMPBR),   SNX_FEAT(FPRCVT),   SNX_FEAT(EL1),
    };
#undef SNX_FEAT
    size_t i, used = 0;
    text[0] = 0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]) && used < size; ++i)
        if (feature & names[i].bit)
            used += (size_t)snprintf(text + used, size - used, "%s%s", used ? "+" : "", names[i].name);
}

static int mismatch_order(const void* a, const void* b) {
    const Mismatch *x = a, *y = b;
    if (x->decoded != y->decoded) return (int)x->decoded - (int)y->decoded;
    return strcmp(group_name(x->op), group_name(y->op));
}

static void cross(const char* cpu, const uint32_t* words) {
    const int max = !strcmp(cpu, "max");
    const uint64_t target = max ? SNX_A64_TARGET_MAX : SNX_A64_TARGET_SWITCH;
    SnxQrefBatch b = {0};
    SnxQrefCase c;
    Mismatch* groups = calloc(2 * (SNX_A64_OP_COUNT + 3), sizeof(Mismatch));
    int* index = malloc(CROSS_WORDS * sizeof(int));
    size_t i, total = 0, ngroups = 0, notrun = 0;
    unsigned k;
    if (!groups || !index) {
        check(0, cpu, "out of memory");
        free(groups);
        free(index);
        return;
    }
    snx_qref_case_init(&c);
    c.seed = 11;
    for (k = 0; k < 31; ++k) c.state.x[k] = SNX_QREF_WINDOW + 0x40000 + 0x1000ull * k + 0x40;
    c.state.sp = SNX_QREF_WINDOW + 0x180000;
    for (k = 0; k < 32; ++k) {
        c.state.v[k][0] = snx_qref_splitmix(k);
        c.state.v[k][1] = snx_qref_splitmix(k + 100);
    }
    for (i = 0; i < CROSS_WORDS; ++i) {
        SnxA64Insn insn;
        index[i] = -1;
        /* SME's SMSTART would leave the runner in streaming mode */
        if (max && snx_a64_decode(words[i], SNX_A64_TARGET_MAX, &insn) != SNX_A64_OP_UNDEF &&
            (insn.feature & SNX_A64_FEAT_SME)) {
            ++notrun;
            continue;
        }
        index[i] = snx_qref_add(&b, &c, &words[i], 1, NULL, 0);
    }
    if (run(&b, cpu)) goto done;
    for (i = 0; i < CROSS_WORDS; ++i) {
        SnxA64Insn insn;
        const SnxQrefResult* r;
        SnxA64Op op;
        int sigill, decoded;
        uint32_t g;
        if (index[i] < 0) continue;
        r = &b.results[index[i]];
        if (r->outcome == SNX_QREF_ERROR) {
            check(0, cpu, "a cross-check word could not run");
            continue;
        }
        op = snx_a64_decode(words[i], target, &insn);
        decoded = op != SNX_A64_OP_UNDEF;
        sigill = r->outcome == SNX_QREF_UNDEF;
        if (decoded != sigill) continue;
        ++total;
        /* the group: the op it decodes to here, or under max when not here, or its space when no row has it */
        if (!decoded) op = snx_a64_decode(words[i], SNX_A64_TARGET_MAX, &insn);
        g = op == SNX_A64_OP_UNDEF ? space(words[i]) : (uint32_t)op;
        for (k = 0; k < ngroups && !(groups[k].op == g && groups[k].decoded == (uint32_t)decoded); ++k) {
        }
        if (k == ngroups) {
            groups[k].op = g;
            groups[k].decoded = (uint32_t)decoded;
            groups[k].example = words[i];
            groups[k].feature = op == SNX_A64_OP_UNDEF ? 0 : insn.feature;
            ++ngroups;
        }
        g = k;
        ++groups[g].count;
    }
    qsort(groups, ngroups, sizeof(*groups), mismatch_order);
    printf("[%s] UNDEF cross-check against the %s target: %u words (%zu rows x %d, %d random), %zu not run, "
           "%zu mismatches in %zu groups\n",
           cpu, max ? "max" : "Switch", b.cases, ROWS, ROW_SAMPLES, RANDOM_WORDS, notrun, total, ngroups);
    for (i = 0; i < ngroups; ++i) {
        const char* line = groups[i].decoded ? "decodes, QEMU SIGILL" : "UNDEF here, QEMU runs";
        const char* name = group_name(groups[i].op);
        char features[160];
        feature_names(groups[i].feature, features, sizeof(features));
        printf("[%s]   %-22s %-16s %-18s %5u (e.g. %08x)\n", cpu, line, name, features, groups[i].count,
               groups[i].example);
        if (g_test.report)
            fprintf(g_test.report, "%s\t%s\t%s\t%s\t%u\t%08x\n", cpu,
                    groups[i].decoded ? "decodes-sigill" : "undef-runs", name, features, groups[i].count,
                    groups[i].example);
        if (!max)
            check(groups[i].decoded && groups[i].op == SNX_A64_OP_SYS, cpu,
                  "the Switch target's UNDEF set matches the Cortex-A57's outside SYS");
    }
done:
    snx_qref_free(&b);
    free(groups);
    free(index);
}

int main(int argc, char** argv) {
    uint32_t* words;
    size_t i;
    int cpu;
    char* report;
    if (argc == 3 && !strcmp(argv[1], "--skip")) {
        printf("qemu_ref_selftest skipped: %s\n", argv[2]);
        return 77;
    }
    if (argc != 4) {
        fputs("usage: snx-test-qemu-ref QEMU RUNNER DIR | --skip REASON\n", stderr);
        return 2;
    }
    g_test.qemu = argv[1];
    g_test.runner = argv[2];
    g_test.dir = argv[3];
    g_test.rng = 0x5EED;
    /* the cross-check's words: samples of every row (the row's bits with the rest random, all clear, all set), then
       random words */
    words = malloc(CROSS_WORDS * sizeof(*words));
    if (!words) return 1;
    for (i = 0; i < ROWS; ++i) {
        const SnxA64Row* row = &snx_a64_rows[i];
        unsigned s;
        for (s = 0; s < ROW_SAMPLES; ++s) {
            uint32_t free_bits = s == 0 ? 0 : s == 1 ? ~0u : (uint32_t)next_random();
            words[i * ROW_SAMPLES + s] = row->value | (free_bits & ~row->mask);
        }
    }
    for (i = 0; i < RANDOM_WORDS; ++i) words[ROWS * ROW_SAMPLES + i] = (uint32_t)next_random();
    report = snx_path_join(g_test.dir, "qemu_ref_undef.txt");
    if (report && !snx_mkdirs(g_test.dir)) g_test.report = fopen(report, "w");
    for (cpu = 0; cpu < 2; ++cpu) {
        hand(g_cpus[cpu]);
        ported(g_cpus[cpu]);
        ported_fp(g_cpus[cpu]);
        cross(g_cpus[cpu], words);
    }
    if (g_test.report) fclose(g_test.report);
    printf("QEMU reference: %d cases run in %.2f s of qemu (%.0f a second), %d checks, %d failed; mismatches in %s\n",
           g_test.runs, g_test.seconds, g_test.seconds > 0 ? g_test.runs / g_test.seconds : 0.0, g_test.checks,
           g_test.failures, report ? report : "-");
    free(report);
    free(words);
    return g_test.failures != 0;
}
