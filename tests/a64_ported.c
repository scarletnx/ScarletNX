/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* rEden's A64 cases (a64_ported_cases.h) through the LLVM backend's step functions (llvm_steps.h, loaded with the
   JIT's loader, or as a module file with --module): every case's expected values, on any host. Given qemu-aarch64
   and the runner (tests/qemu_ref), each run of a case also runs under QEMU (-cpu cortex-a57) from the state and
   memory the LLVM side started it from, and the two must end alike: every register, NZCV, FPCR, FPSR, where it went
   and the memory window. The code is at CODE on both sides, so they run the same pcs, and in guest memory on both
   (a literal load reads it).
     snx-test-a64-ported [--module] [QEMU RUNNER DIR] */
#include "llvm_steps.h"
#include "a64_ported_cases.h"
#include "qemu_ref/qemu_ref.h"
#include <stdlib.h>
#include <string.h>

#define CODE   0x10000ull /* after the window at 0 that most cases have */
#define WINDOW 0x4000u   /* a case's memory, at its base */
#define FZ     0x01000000u
#define DN     0x02000000u

/* a run compared with QEMU: where it started (the window as it was) and how the LLVM side ended it */
typedef struct {
    const char* name;
    const u32* words;
    u32 nwords;
    u64 pc, steps, base;
    int window; /* 0: no memory */
    SnxQrefState start, end;
    u64 end_pc;
    u8 *before, *after;
    int index; /* in the batch */
} Run;

static struct {
    int failures, checks, traps;
    const char* name;
    LlvmSteps steps;
    size_t* step_of; /* script op -> step */
    size_t minmax_first, fixed_first;
    u64 base;
    /* guest memory: the window at its base, and the case's code at CODE when the base is 0 */
    _Alignas(4096) u8 window[CODE + SNX_QREF_PAGE];
    u8 want_window[WINDOW];
    int compare; /* runs are recorded for QEMU */
    Run* runs;
    size_t nruns, cap;
} g;

static void expect(int ok, const char* what, u64 got, u64 want) {
    ++g.checks;
    if (ok) return;
    ++g.failures;
    fprintf(stderr, "FAIL %s: %s is %016llx, expected %016llx\n", g.name, what, (unsigned long long)got,
            (unsigned long long)want);
}

static void on_undef(CPU* c, u64 pc, u32 w) {
    (void)c, (void)pc, (void)w;
    ++g.traps;
}
static void on_svc(CPU* c, u32 imm) {
    (void)c, (void)imm;
    ++g.traps;
}
static void on_brk(CPU* c, u64 pc, u32 imm) {
    (void)c, (void)pc, (void)imm;
    ++g.traps;
}
static u64 on_ticks(void) { return 0; }

static const RtAPI runtime = {
    .undef = on_undef, .svc = on_svc, .brk = on_brk, .ticks = on_ticks, .cas128 = snx_runtime_cas128};

static void reset(CPU* c) {
    memset(c, 0, sizeof(*c));
    c->rt = &runtime;
    g.traps = 0;
}

/* the window holds guest addresses base..; seeded as qemu_ref seeds a case's (SNX_QREF_SEED_*) */
static void memory(CPU* c, u64 base, u32 kind) {
    u32 i;
    g.base = base;
    for (i = 0; i < WINDOW; ++i) g.window[i] = snx_qref_seed_byte(kind, 0, base, base + i);
    c->mem = (u8*)((uintptr_t)g.window - (uintptr_t)base); /* as integers: the pointer would leave the array */
}
static u8* at(u8* window, u64 address, u64 size) {
    if (address - g.base >= WINDOW || address - g.base + size > WINDOW) return NULL;
    return window + (address - g.base);
}

/* V128 holds the guest's bytes: its d[] views are byte-swapped numbers on a big-endian host */
static void vset(V128* v, u64 lo, u64 hi) {
    v->d[0] = SNX_GUEST64(lo);
    v->d[1] = SNX_GUEST64(hi);
}
static u64 vlo(const V128* v) { return SNX_GUEST64(v->d[0]); }
static u64 vhi(const V128* v) { return SNX_GUEST64(v->d[1]); }

static void state_of(const CPU* c, SnxQrefState* s) {
    unsigned i;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < 31; ++i) s->x[i] = c->x[i];
    s->sp = c->x[31];
    s->nzcv = get_nzcv(c);
    s->fpcr = c->fpcr;
    s->fpsr = c->fpsr;
    for (i = 0; i < 32; ++i) {
        s->v[i][0] = vlo(&c->v[i]);
        s->v[i][1] = vhi(&c->v[i]);
    }
}

static Run* record(const char* name, const u32* words, u32 nwords, u64 pc, u64 steps, const CPU* c, int window) {
    Run* r;
    if (!g.compare) return NULL;
    if (g.nruns == g.cap) {
        g.cap = g.cap ? g.cap * 2 : 256;
        g.runs = (Run*)snx_realloc(g.runs, g.cap * sizeof(*g.runs));
    }
    r = &g.runs[g.nruns++];
    memset(r, 0, sizeof(*r));
    r->name = name;
    r->words = words;
    r->nwords = nwords;
    r->pc = pc;
    r->steps = steps;
    r->base = g.base;
    r->window = window;
    state_of(c, &r->start);
    if (window) {
        r->before = (u8*)snx_alloc(WINDOW);
        memcpy(r->before, g.window, WINDOW);
    }
    return r;
}
static void recorded(Run* r, const CPU* c, u64 pc) {
    if (!r) return;
    state_of(c, &r->end);
    r->end_pc = pc;
    if (r->window) {
        r->after = (u8*)snx_alloc(WINDOW);
        memcpy(r->after, g.window, WINDOW);
    }
}

/* one pass of the script; the number of cases */
static int script(void) {
    static u32 words[SNX_QREF_MAX_CODE / 4];
    size_t i, first = 0, count = 0;
    CPU c, want;
    u64 pc = 0;
    int cases = 0;
    char what[64];
    reset(&c);
    memset(&want, 0, sizeof(want));
    for (i = 0; snx_a64p_script[i].op != SNX_A64P_OP_END; ++i) {
        const SnxA64pOp* op = &snx_a64p_script[i];
        u8* p;
        switch (op->op) {
            case SNX_A64P_OP_CASE:
                g.name = op->text;
                reset(&c);
                memory(&c, 0, SNX_QREF_SEED_ADDRESS);
                first = i + 1;
                count = 0;
                pc = 0;
                ++cases;
                break;
            case SNX_A64P_OP_CODE:
                /* a case's code comes first, so its offset / 4 indexes it */
                expect(i == first + count, "the script: code after the state", 0, 0);
                ++count;
                break;
            case SNX_A64P_OP_SET_X: c.x[op->reg] = op->a; break;
            case SNX_A64P_OP_SET_V: vset(&c.v[op->reg], op->a, op->b); break;
            /* as the Cortex-A57's MSR leaves them: FZ16 and the trap enables are RES0 there, as are FPSR's bits
               between QC and IDC */
            case SNX_A64P_OP_SET_FPCR: c.fpcr = (u32)op->a & 0x07C00000u; break;
            case SNX_A64P_OP_SET_FPSR: c.fpsr = (u32)op->a & 0xF800009Fu; break;
            case SNX_A64P_OP_SET_NZCV: set_nzcv(&c, (u32)(op->a >> 28)); break;
            case SNX_A64P_OP_CLEAR_V: memset(c.v, 0, sizeof(c.v)); break;
            case SNX_A64P_OP_MEMORY:
                memory(&c, op->a, op->reg ? SNX_QREF_SEED_ZERO : SNX_QREF_SEED_ADDRESS);
                break;
            case SNX_A64P_OP_SET_MEM:
                p = at(g.window, op->a, strlen(op->text) + 1);
                expect(p != NULL, "the script: text outside the memory window", 0, 0);
                if (p) memcpy(p, op->text, strlen(op->text) + 1);
                break;
            case SNX_A64P_OP_RUN: {
                u64 t;
                size_t k;
                /* QEMU can't host a window above its guest's 47 bits or over the code */
                const int hosted = g.base < (1ull << 44) && !(g.base < CODE + 0x10000 && g.base + WINDOW > CODE);
                Run* r;
                for (k = 0; k < count; ++k) {
                    unsigned b;
                    words[k] = (u32)snx_a64p_script[first + k].a;
                    for (b = 0; !g.base && b < 4 && k < SNX_QREF_PAGE / 4; ++b)
                        g.window[CODE + 4 * k + b] = (u8)(words[k] >> (8 * b));
                }
                r = hosted ? record(g.name, NULL, (u32)count, CODE + op->a, op->b, &c, 1) : NULL;
                if (r) { /* its own copy of the case's words (a run with a window owns them) */
                    u32* copy = (u32*)snx_alloc(count * sizeof(u32));
                    memcpy(copy, words, count * sizeof(u32));
                    r->words = copy;
                }
                pc = CODE + op->a;
                /* past the code, dynarmic's environment reads B . */
                for (t = 0; t < op->b; ++t) {
                    if (pc < CODE || ((pc - CODE) & 3) || (pc - CODE) / 4 >= count) break;
                    pc = g.steps.steps[g.step_of[first + (size_t)((pc - CODE) / 4)]](&c);
                }
                recorded(r, &c, pc);
                expect(!g.traps, "undefined-instruction, SVC or BRK traps", (u64)g.traps, 0);
                g.traps = 0;
                break;
            }
            case SNX_A64P_OP_X:
                snprintf(what, sizeof(what), "X%u & %016llx", op->reg, (unsigned long long)op->b);
                expect((c.x[op->reg] & op->b) == op->a, what, c.x[op->reg] & op->b, op->a);
                break;
            case SNX_A64P_OP_V:
                snprintf(what, sizeof(what), "V%u low", op->reg);
                expect(vlo(&c.v[op->reg]) == op->a, what, vlo(&c.v[op->reg]), op->a);
                snprintf(what, sizeof(what), "V%u high", op->reg);
                expect(vhi(&c.v[op->reg]) == op->b, what, vhi(&c.v[op->reg]), op->b);
                break;
            case SNX_A64P_OP_PC: {
                const u64 offset = pc - CODE < 0x10000 ? pc - CODE : pc;
                expect(offset == op->a, "PC", offset, op->a);
                break;
            }
            case SNX_A64P_OP_FPSR:
                expect((c.fpsr & op->b) == op->a, "FPSR", c.fpsr & op->b, op->a);
                break;
            case SNX_A64P_OP_NZCV: expect(get_nzcv(&c) == op->a, "NZCV", get_nzcv(&c), op->a); break;
            case SNX_A64P_OP_MEM64: {
                u64 v = 0;
                unsigned b;
                p = at(g.window, op->a, 8);
                for (b = 0; p && b < 8; ++b) v |= (u64)p[b] << (8 * b); /* guest memory is little-endian */
                snprintf(what, sizeof(what), "the 64 bits at %llx", (unsigned long long)op->a);
                expect(p && v == op->b, what, v, op->b);
                break;
            }
            case SNX_A64P_OP_MEM_SAME: {
                u8 *x = at(g.window, op->a, op->reg), *y = at(g.window, op->b, op->reg);
                expect(x && y && !memcmp(x, y, op->reg), "the copied bytes", 0, 1);
                break;
            }
            case SNX_A64P_OP_PRESET: {
                /* interpreter_visitor.cpp's Initial(), its region at address 0: no zero byte in memory, so a stored
                   zero shows; SP ends in 0xee0, so SP stored in place of zero shows at every size */
                unsigned n;
                expect(op->reg == 1, "the script: an unknown preset", op->reg, 1);
                memory(&c, 0, SNX_QREF_SEED_NCE);
                for (n = 0; n < 31; ++n) c.x[n] = 0xDEAD000000000000ull | n;
                c.x[0] = 0x1000;
                c.x[1] = 0x1800;
                c.x[2] = 0x30;
                c.x[31] = 0x2EE0;
                for (n = 0; n < 32; ++n) vset(&c.v[n], 0x5555000000000000ull | n, 0xAAAA000000000000ull | n);
                break;
            }
            case SNX_A64P_OP_MARK:
                want = c;
                memcpy(g.want_window, g.window, WINDOW);
                break;
            case SNX_A64P_OP_WANT_X: want.x[op->reg] = op->a; break;
            case SNX_A64P_OP_WANT_V: vset(&want.v[op->reg], op->a, op->b); break;
            case SNX_A64P_OP_WANT_MEM: {
                unsigned b;
                p = at(g.want_window, op->a, op->reg);
                expect(p != NULL && op->reg <= 8, "the script: an expectation outside the window", op->a, 0);
                for (b = 0; p && b < op->reg && b < 8; ++b) p[b] = (u8)(op->b >> (8 * b));
                break;
            }
            case SNX_A64P_OP_SAME: {
                unsigned n;
                for (n = 0; n < 32; ++n) {
                    snprintf(what, sizeof(what), n == 31 ? "SP" : "X%u", n);
                    expect(c.x[n] == want.x[n], what, c.x[n], want.x[n]);
                }
                for (n = 0; n < 32; ++n) {
                    snprintf(what, sizeof(what), "V%u", n);
                    expect(!memcmp(&c.v[n], &want.v[n], 16), what, vlo(&c.v[n]), vlo(&want.v[n]));
                }
                for (n = 0; n < WINDOW; ++n)
                    if (g.window[n] != g.want_window[n]) {
                        snprintf(what, sizeof(what), "the byte at %llx", (unsigned long long)(g.base + n));
                        expect(0, what, g.window[n], g.want_window[n]);
                        break;
                    }
                break;
            }
            default: expect(0, "the script: an unknown step", op->op, 0);
        }
    }
    return cases;
}

static int is_nan32(u32 v) { return (v & 0x7F800000u) == 0x7F800000u && (v & 0x007FFFFFu); }

/* fp_min_max.cpp's run_test for each instruction */
static int minmax(void) {
    static const u32 bases[] = {0, FZ};
    const size_t rows = sizeof(snx_a64p_minmax) / sizeof(snx_a64p_minmax[0]);
    const size_t words = sizeof(snx_a64p_minmax_words) / sizeof(snx_a64p_minmax_words[0]);
    size_t w, b, r;
    int dn, swap, runs = 0;
    char what[96];
    CPU c;
    for (w = 0; w < words; ++w) {
        const SnxA64pMinMaxWord* word = &snx_a64p_minmax_words[w];
        g.name = word->text;
        for (b = 0; b < 2; ++b)
            for (dn = 0; dn < 2; ++dn)
                for (r = 0; r < rows; ++r)
                    for (swap = 0; swap < (snx_a64p_minmax[r].one_way ? 1 : 2); ++swap) {
                        const SnxA64pMinMax* row = &snx_a64p_minmax[r];
                        u32 want = row->result[word->column];
                        Run* run;
                        u64 pc;
                        if (dn && is_nan32(want)) want = 0x7FC00000u;
                        reset(&c);
                        memory(&c, 0, SNX_QREF_SEED_ZERO);
                        c.fpcr = bases[b] | (dn ? DN : 0);
                        vset(&c.v[0], 42, 0);
                        vset(&c.v[1], swap ? row->b : row->a, 0);
                        vset(&c.v[2], swap ? row->a : row->b, 0);
                        run = record(word->text, &word->word, 1, CODE, 1, &c, 0);
                        pc = g.steps.steps[g.minmax_first + w](&c);
                        recorded(run, &c, pc);
                        snprintf(what, sizeof(what), "V0[63:0] for %08llx, %08llx under FPCR %08x",
                                 (unsigned long long)vlo(&c.v[1]), (unsigned long long)vlo(&c.v[2]), c.fpcr);
                        expect(vlo(&c.v[0]) == want && !g.traps, what, vlo(&c.v[0]), want);
                        ++runs;
                    }
    }
    return runs;
}

/* FPToFixed.cpp, as FCVT<rounding>S Rd0, Fn0: the words qemu_ref_selftest runs them with */
static const u32 g_rmode[5] = {0x00000u, 0x80000u, 0x100000u, 0x180000u, 0x40000u}; /* N P M Z A */
static u32 fixed_word(const SnxA64pFixed* f) {
    return 0x1E200000u | (f->int_width == 64 ? 0x80000000u : 0) | (f->fp_width == 64 ? 0x400000u : 0) |
           g_rmode[f->rm];
}
static u32 g_fixed_words[sizeof(snx_a64p_fixed) / sizeof(snx_a64p_fixed[0])];

static int fixed(void) {
    const size_t rows = sizeof(snx_a64p_fixed) / sizeof(snx_a64p_fixed[0]);
    size_t r;
    char what[96];
    CPU c;
    g.name = "FPToFixed";
    for (r = 0; r < rows; ++r) {
        const SnxA64pFixed* f = &snx_a64p_fixed[r];
        Run* run;
        u64 pc;
        reset(&c);
        memory(&c, 0, SNX_QREF_SEED_ZERO);
        vset(&c.v[0], f->input, 0);
        run = record("FPToFixed", &g_fixed_words[r], 1, CODE, 1, &c, 0);
        pc = g.steps.steps[g.fixed_first + r](&c);
        recorded(run, &c, pc);
        snprintf(what, sizeof(what), "%016llx to %d bits, rounding %d", (unsigned long long)f->input, f->int_width,
                 f->rm);
        expect(c.x[0] == f->output && !g.traps, what, c.x[0], f->output);
        if (f->fpsr != ~0u) {
            snprintf(what, sizeof(what), "FPSR after %016llx to %d bits", (unsigned long long)f->input, f->int_width);
            expect(c.fpsr == f->fpsr, what, c.fpsr, f->fpsr);
        }
    }
    return (int)rows;
}

/* every recorded run under QEMU, against how the LLVM side ended it */
static int against_qemu(const char* qemu, const char* runner, const char* dir) {
    SnxQrefBatch b = {0};
    char error[512], what[160];
    size_t i;
    unsigned k;
    for (i = 0; i < g.nruns; ++i) {
        Run* r = &g.runs[i];
        SnxQrefCase c;
        SnxQrefPatch patch;
        snx_qref_case_init(&c);
        c.window = r->window ? r->base : 0;
        c.window_size = r->window ? WINDOW : 0;
        c.seed_kind = SNX_QREF_SEED_ZERO;
        c.code = CODE;
        c.code_size = (r->nwords * 4 + 8 + SNX_QREF_PAGE - 1) & ~(SNX_QREF_PAGE - 1);
        c.pc = r->pc;
        c.steps = r->steps > 100000 ? 0 : r->steps; /* more runs free: the program ends by leaving its code */
        c.flags = SNX_QREF_FULL_DIFF;
        c.state = r->start;
        patch.address = r->base;
        patch.bytes = r->before;
        patch.size = WINDOW;
        r->index = snx_qref_add(&b, &c, r->words, r->nwords, r->window ? &patch : NULL, r->window ? 1 : 0);
        if (r->index < 0) {
            fprintf(stderr, "FAIL %s: the runner refuses the case\n", r->name);
            ++g.failures;
        }
    }
    if (snx_qref_run(&b, qemu, runner, "cortex-a57", dir, error, sizeof(error))) {
        fprintf(stderr, "FAIL: QEMU: %s\n", error);
        snx_qref_free(&b);
        return -1;
    }
    for (i = 0; i < g.nruns; ++i) {
        Run* r = &g.runs[i];
        const SnxQrefResult* q;
        unsigned n;
        if (r->index < 0) continue;
        q = &b.results[r->index];
        g.name = r->name;
        if (q->outcome != SNX_QREF_NEXT && q->outcome != SNX_QREF_BRANCH) {
            snprintf(what, sizeof(what), "QEMU's run, ended by %s (signal %u, fault %llx), at",
                     snx_qref_outcome_name(q->outcome), q->signo, (unsigned long long)q->fault);
            expect(0, what, q->pc, r->end_pc);
            continue;
        }
        expect(q->pc == r->end_pc, "where the run went, against QEMU", r->end_pc, q->pc);
        for (n = 0; n < 31; ++n) {
            snprintf(what, sizeof(what), "X%u against QEMU", n);
            expect(q->state.x[n] == r->end.x[n], what, r->end.x[n], q->state.x[n]);
        }
        expect(q->state.sp == r->end.sp, "SP against QEMU", r->end.sp, q->state.sp);
        expect(q->state.nzcv == r->end.nzcv, "NZCV against QEMU", r->end.nzcv, q->state.nzcv);
        expect(q->state.fpcr == r->end.fpcr, "FPCR against QEMU", r->end.fpcr, q->state.fpcr);
        expect(q->state.fpsr == r->end.fpsr, "FPSR against QEMU", r->end.fpsr, q->state.fpsr);
        for (n = 0; n < 32; ++n) {
            snprintf(what, sizeof(what), "V%u low against QEMU", n);
            expect(q->state.v[n][0] == r->end.v[n][0], what, r->end.v[n][0], q->state.v[n][0]);
            snprintf(what, sizeof(what), "V%u high against QEMU", n);
            expect(q->state.v[n][1] == r->end.v[n][1], what, r->end.v[n][1], q->state.v[n][1]);
        }
        if (!r->window) continue;
        /* QEMU's window: the one the run started from, with its changes */
        for (k = 0; k < q->changes; ++k) {
            const SnxQrefChange* ch = &b.changes[q->first_change + k];
            if (ch->address - r->base < WINDOW && ch->address - r->base + ch->size <= WINDOW)
                memcpy(r->before + (ch->address - r->base), b.bytes + ch->offset, ch->size);
            else
                expect(0, "QEMU's run changed memory outside its window at", ch->address, r->base);
        }
        for (n = 0; n < WINDOW; ++n)
            if (r->before[n] != r->after[n]) {
                snprintf(what, sizeof(what), "the byte at %llx against QEMU", (unsigned long long)(r->base + n));
                expect(0, what, r->after[n], r->before[n]);
                break;
            }
    }
    snx_qref_free(&b);
    return 0;
}

int main(int argc, char** argv) {
    const size_t ops = sizeof(snx_a64p_script) / sizeof(snx_a64p_script[0]);
    const size_t words = sizeof(snx_a64p_minmax_words) / sizeof(snx_a64p_minmax_words[0]);
    const size_t rows = sizeof(snx_a64p_fixed) / sizeof(snx_a64p_fixed[0]);
    const char *qemu = NULL, *runner = NULL, *dir = NULL;
    char error[1024];
    size_t i, address = 0;
    int a = 1, jit = 1, cases, minmaxes, conversions;
    if (a < argc && !strcmp(argv[a], "--module")) {
        jit = 0;
        ++a;
    }
    if (argc - a == 3) {
        qemu = argv[a];
        runner = argv[a + 1];
        dir = argv[a + 2];
        g.compare = 1;
    } else if (argc != a) {
        fputs("usage: snx-test-a64-ported [--module] [QEMU RUNNER DIR]\n", stderr);
        return 2;
    }
    /* each case's code from CODE: a branch's step resolves its target from its own pc */
    if (llvm_steps_begin(&g.steps, jit, 0, error, sizeof(error))) return snx_error("%s", error);
    g.step_of = (size_t*)snx_alloc(ops * sizeof(*g.step_of));
    for (i = 0; i < ops; ++i) {
        if (snx_a64p_script[i].op == SNX_A64P_OP_CASE) address = 0;
        if (snx_a64p_script[i].op != SNX_A64P_OP_CODE) continue;
        g.step_of[i] = llvm_steps_add(&g.steps, (u32)snx_a64p_script[i].a, CODE + address, 0);
        address += 4;
    }
    g.minmax_first = g.steps.count;
    for (i = 0; i < words; ++i) llvm_steps_add(&g.steps, snx_a64p_minmax_words[i].word, CODE, 0);
    g.fixed_first = g.steps.count;
    for (i = 0; i < rows; ++i) {
        g_fixed_words[i] = fixed_word(&snx_a64p_fixed[i]);
        llvm_steps_add(&g.steps, g_fixed_words[i], CODE, 0);
    }
    if (llvm_steps_load(&g.steps, error, sizeof(error))) return snx_error("%s", error);
    cases = script();
    minmaxes = minmax();
    conversions = fixed();
    if (g.compare && against_qemu(qemu, runner, dir)) ++g.failures;
    printf("A64 cases ported from rEden's tests through the LLVM steps (%s): %d cases, %d min/max runs, "
           "%d conversions, %s, %d checks, %d failed\n",
           jit ? "the JIT's loader" : "a module", cases, minmaxes, conversions,
           g.compare ? "each run against QEMU too" : "without QEMU", g.checks, g.failures);
    for (i = 0; i < g.nruns; ++i) {
        if (g.runs[i].window) free((u32*)g.runs[i].words);
        free(g.runs[i].before);
        free(g.runs[i].after);
    }
    free(g.runs);
    free(g.step_of);
    llvm_steps_end(&g.steps);
    return g.failures != 0;
}
