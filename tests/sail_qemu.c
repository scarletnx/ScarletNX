/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sail_qemu.h's: the states, the window, the cases, the comparison with QEMU's end and the reference of it */
#include "sail_qemu.h"
#include "llvm_qemu_words.h"
#include "support/frozen.h"
#include "common/cpu/aarch64/a64_decoder.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* what the hashes of a reference depend on besides QEMU: a change to how the states are drawn, the window, the pcs or
   the hash takes the next number, and the reference is recorded again (the qemu-reference job records it each run) */
#define SAIL_GENERATOR 1u
#define REF_MAGIC      0x314C494153584E53ull /* "SNXSAIL1" */
#define TBI_MASK       0x00FFFFFFFFFFFFFFull

SailCalls g_sail_calls;
static struct {
    u64 rng;
    u64 target;
    int dirty; /* the touched part of the last work a native run wrote */
} g_sail = {1, 0, 1};

static u64 rnd(void) {
    g_sail.rng ^= g_sail.rng << 13;
    g_sail.rng ^= g_sail.rng >> 7;
    g_sail.rng ^= g_sail.rng << 17;
    return g_sail.rng;
}

/* ---- the runtime ---- */

static void on_undef(CPU* c, u64 pc, u32 word) {
    (void)c;
    ++g_sail_calls.undef;
    g_sail_calls.undef_args = 3 * g_sail_calls.undef_args + (pc ^ (u64)word << 32);
}
static void on_svc(CPU* c, u32 imm) { (void)c, ++g_sail_calls.svc, g_sail_calls.imm = imm; }
static void on_brk(CPU* c, u64 pc, u32 imm) { (void)c, (void)pc, ++g_sail_calls.brk, g_sail_calls.imm = imm; }
static void on_note_hint(u64 pc) { (void)pc; }
static u64 on_ticks(void) { return 0x0123456789ABCDEFull; }
static void on_fpcr_changed(CPU* c) { (void)c; }
static void on_sev(void) { ++g_sail_calls.sev; }
static void on_wfe(CPU* c) { (void)c, ++g_sail_calls.wfe; }
static void on_poll(CPU* c) { atomic_store_explicit(&c->poll, 0, memory_order_relaxed); }
static void on_miss(CPU* c, u64 pc) { c->pc = pc; }
static void on_code_changed(CPU* c, u64 addr) { (void)c, (void)addr; }
void sail_rt(RtAPI* rt) {
    *rt = (RtAPI){.svc = on_svc,
                  .undef = on_undef,
                  .note_hint = on_note_hint,
                  .ticks = on_ticks,
                  .brk = on_brk,
                  .fpcr_changed = on_fpcr_changed,
                  .sev = on_sev,
                  .wfe = on_wfe,
                  .poll = on_poll,
                  .miss = on_miss,
                  .code_changed = on_code_changed,
                  .cas128 = snx_runtime_cas128};
}

/* ---- the words and their states ---- */

void sail_set_target(u64 target) { g_sail.target = target; }

static const char* op_name(u32 word, u64 target, SnxA64Op* op) {
    SnxA64Insn insn;
    *op = snx_a64_decode_word(word, target, &insn);
    return *op != SNX_A64_OP_UNDEF ? snx_a64_op_text(*op) : NULL;
}

int sail_is_ldst(u32 word) {
    SnxA64Op op;
    const char* n = op_name(word, g_sail.target, &op);
    return n && (!strncmp(n, "LD", 2) || !strncmp(n, "ST", 2) || !strncmp(n, "CAS", 3) || !strncmp(n, "SWP", 3) ||
                 !strncmp(n, "PRF", 3));
}

/* a store-exclusive (STXR, STLXR, STXP, STLXP): size 001000 o2=0 L=0 o1 Rs o0 Rt2 Rn Rt */
static int store_exclusive(u32 w) { return (w & 0x3FC00000u) == 0x08000000u; }
/* the load-exclusive of the same size and address into registers the store doesn't name */
static u32 load_exclusive(u32 st) {
    const u32 rt = st & 31, rn = (st >> 5) & 31, rt2 = (st >> 10) & 31, rs = (st >> 16) & 31, pair = (st >> 21) & 1;
    u32 regs[2], k = 0, r;
    for (r = 0; r < 31 && k < 2; ++r)
        if (r != rt && r != rn && r != rt2 && r != rs) regs[k++] = r;
    return (st & 0xC0200000u) | 0x08400000u | 31u << 16 | (pair ? regs[1] : 31u) << 10 | rn << 5 | regs[0];
}
u32 sail_pair(u32 word, unsigned s) { return store_exclusive(word) && (s & 1) ? load_exclusive(word) : 0; }

const char* sail_unheld(u32 word) {
    SnxA64Insn max;
    SnxA64Op mine, theirs;
    int unrunnable = 0;
    (void)lqd_op_name(word, &unrunnable);
    if (unrunnable) return "QEMU's runner can't host it";
    (void)op_name(word, g_sail.target, &mine);
    theirs = snx_a64_decode_word(word, SNX_A64_TARGET_MAX, &max);
    if (theirs != mine) return "max decodes it as another op than the guest's";
    if (max.feature & SNX_A64_FEAT_PAUTH) return "QEMU signs with keys where the code runs with authentication off";
    if (theirs == SNX_A64_OP_FCVTN_bs && (word >> 30 & 1) && (word & 31) == (word >> 5 & 31))
        return "QEMU 11.1.2 reads FCVTN2's Vn after it has written Vd";
    return NULL;
}

/* the SIMD&FP state a start has: each doubleword random, or two singles or a double of the values floating point
   treats apart (zeros, infinities, quiet and signalling NaNs, denormals, the largest, ordinary numbers); FPCR's
   rounding mode, FZ, DN, AHP and FZ16 random in odd states (0 in the rest), FPSR's flags random */
static u64 fp_value(int dbl) {
    static const u64 d[] = {0,
                            0x8000000000000000ull,
                            0x3ff0000000000000ull,
                            0xc008000000000000ull,
                            0x7ff0000000000000ull,
                            0xfff0000000000000ull,
                            0x7ff8000000000000ull,
                            0x7ff4000000000000ull,
                            0x000fffffffffffffull,
                            0x8000000000000001ull,
                            0x7fefffffffffffffull,
                            0x0010000000000000ull,
                            0x41dfffffffc00000ull,
                            0xc3e0000000000000ull,
                            0x3fe0000000000000ull,
                            0x4059000000000000ull};
    static const u32 f[] = {0,           0x80000000u, 0x3f800000u, 0xc0400000u, 0x7f800000u, 0xff800000u,
                            0x7fc00000u, 0x7fa00000u, 0x007fffffu, 0x80000001u, 0x7f7fffffu, 0x00800000u,
                            0x4effffffu, 0xdf000000u, 0x3f000000u, 0x42c80000u};
    u64 r = rnd();
    if (r & 0x100) return r >> 9;
    if (dbl) return d[r & 15];
    return (u64)f[r & 15] | (u64)f[(r >> 4) & 15] << 32;
}

void sail_start(CPU* c, u32 word, unsigned s, const RtAPI* rt) {
    unsigned r;
    memset(c, 0, sizeof *c);
    g_sail.rng = snx_qref_splitmix(snx_qref_splitmix(0x9E3779B97F4A7C15ull ^ word) ^ s) | 1;
    for (r = 0; r < 32; ++r) c->x[r] = rnd();
    for (r = 0; r < 32; ++r) {
        snx_st_le64(c->v[r].b, fp_value(rnd() & 1));
        snx_st_le64(c->v[r].b + 8, fp_value(rnd() & 1));
    }
    c->fpcr = s & 1 ? (u32)rnd() & (3u << 22 | 1u << 24 | 1u << 25 | 1u << 26 | 1u << 19) : 0;
    c->fpsr = (u32)rnd() & 0x9f;
    if (s & 1) c->x[rnd() & 31] = 0;
    if (s & 2) c->x[rnd() & 31] = 0x8000000000000000ull;
    if (s & 4) c->x[rnd() & 31] = ~0ull;
    if (s & 8) c->x[rnd() & 31] = 0xffffffffull;
    c->n = rnd() & 1;
    c->z = rnd() & 1;
    c->cf = rnd() & 1;
    c->vf = rnd() & 1;
    c->rt = rt;
    c->pc = sail_pair(word, s) ? SAIL_PAIR_PC : SAIL_PC;
    if (sail_is_ldst(word)) {
        SnxA64Insn insn;
        snx_a64_decode_word(word, g_sail.target, &insn);
        c->x[31] = SAIL_WINDOW + 0x1000 + (rnd() & 0xff0);
        c->x[insn.args.rn] = SAIL_WINDOW + 0x1000 + (rnd() & 0xff0);
        c->x[insn.args.rm] = SAIL_WINDOW + 0x1000 + (rnd() & 0xff0);
        if (insn.op == SNX_A64_OP_LDAPR_i || insn.op == SNX_A64_OP_STLR_i)
            c->x[insn.args.rn] -= (c->x[insn.args.rn] + (u64)(int64_t)insn.args.imm) & ((1u << insn.args.sz) - 1);
    }
}

/* ---- the window ---- */

void sail_window(u8* initial) {
    u64 i;
    for (i = 0; i < SAIL_WINDOW_SIZE; i += 8)
        snx_st_le64(initial + i, snx_qref_seed64(SNX_QREF_SEED_RANDOM, SAIL_SEED, SAIL_WINDOW, i));
    /* guest memory is little-endian on every host, as QEMU's window is: a literal load at the pc reads the word */
    for (i = SAIL_PC - SAIL_WINDOW; i < SAIL_PC - SAIL_WINDOW + SAIL_CODE_SIZE; i += 4)
        snx_st_le32(initial + i, SNX_QREF_PAD);
}
void sail_window_start(u8* work, const u8* initial, u32 word, int pair) {
    if (g_sail.dirty) memcpy(work, initial, SAIL_TOUCHED);
    g_sail.dirty = 0;
    /* a pair's words are at SAIL_PAIR_PC, past the window: the code region in it holds pads, as QEMU's holds the seed
       there and neither side reads it */
    snx_st_le32(work + (SAIL_PC - SAIL_WINDOW), pair ? SNX_QREF_PAD : word);
}

void sail_state_of(const CPU* c, SnxQrefState* s) {
    unsigned i;
    memset(s, 0, sizeof *s);
    for (i = 0; i < 31; ++i) s->x[i] = c->x[i];
    s->sp = c->x[31];
    s->nzcv = c->n << 31 | c->z << 30 | c->cf << 29 | c->vf << 28;
    s->fpcr = c->fpcr;
    s->fpsr = c->fpsr;
    for (i = 0; i < 32; ++i) {
        s->v[i][0] = snx_ld_le64(c->v[i].b);
        s->v[i][1] = snx_ld_le64(c->v[i].b + 8);
    }
}

int sail_add_case(SnxQrefBatch* b, u32 word, unsigned s, const CPU* start) {
    SnxQrefCase qc;
    u32 words[2];
    const u32 first = sail_pair(word, s);
    snx_qref_case_init(&qc);
    qc.window = SAIL_WINDOW;
    qc.window_size = SAIL_WINDOW_SIZE;
    qc.seed_kind = SNX_QREF_SEED_RANDOM;
    qc.seed = SAIL_SEED;
    qc.code = qc.pc = first ? SAIL_PAIR_PC : SAIL_PC;
    qc.code_size = SAIL_CODE_SIZE;
    /* a pair runs free (each step enters through a signal, whose return clears the monitor) with the window writable,
       as a fault between the two would clear it too */
    qc.steps = first ? 0 : 1;
    qc.flags = first ? SNX_QREF_FULL_DIFF : 0;
    sail_state_of(start, &qc.state);
    words[0] = first ? first : word;
    words[1] = word;
    return snx_qref_add(b, &qc, words, first ? 2 : 1, NULL, 0);
}

/* ---- the ends ---- */

void sail_native_end(SailEnd* e, const CPU* c, u64 next, u64 pc, const u8* work, const u8* initial) {
    memset(e, 0, sizeof *e);
    sail_state_of(c, &e->state);
    g_sail.dirty = e->wrote = memcmp(work, initial, SAIL_TOUCHED) != 0;
    if (g_sail_calls.undef)
        e->how.outcome = SNX_QREF_UNDEF, e->how.pc = pc;
    else if (g_sail_calls.brk)
        e->how.outcome = SNX_QREF_BRK, e->how.pc = pc, e->how.imm = g_sail_calls.imm;
    else if (g_sail_calls.svc)
        e->how.outcome = SNX_QREF_SVC, e->how.pc = next, e->how.imm = g_sail_calls.imm;
    else
        e->how.outcome = next == pc + 4 ? SNX_QREF_NEXT : SNX_QREF_BRANCH, e->how.pc = next;
}
void sail_qemu_end(SailEnd* e, const SnxQrefResult* r) {
    memset(e, 0, sizeof *e);
    e->how.outcome = r->outcome;
    e->how.pc = r->pc;
    e->how.imm = r->imm;
    e->state = r->state;
}

/* how a case ended, both sides alike: NEXT and BRANCH are one, by the pc */
static u32 kind(u32 outcome) { return outcome == SNX_QREF_BRANCH ? SNX_QREF_NEXT : outcome; }
static const char* kind_name(u32 outcome) {
    return kind(outcome) == SNX_QREF_NEXT ? "runs" : snx_qref_outcome_name(outcome);
}

/* the register an MRS of a system register QEMU gives otherwise reads into, which the comparison leaves out (-1 for
   none), and the value the Switch gives for it when constant */
static int environment_register(u32 word, u64* value, int* fixed) {
    const char* name;
    int k = snx_qref_switch_sysreg(word, value, &name);
    *fixed = k == SNX_QREF_SYSREG_FIXED;
    if (k != SNX_QREF_SYSREG_NONE) return (int)(word & 31);
    if ((word & ~0x1Fu) == 0xD53BD040u) return (int)(word & 31); /* MRS TPIDR_EL0: QEMU's thread pointer */
    return -1;
}

static u64 mix(u64 h, u64 v) { return snx_qref_splitmix(h ^ v); }

typedef struct {
    u64 address;
    u8 value;
} Byte;
static int by_address(const void* a, const void* b) {
    const Byte *x = (const Byte*)a, *y = (const Byte*)b;
    return x->address < y->address ? -1 : x->address > y->address;
}

/* the bytes QEMU's run changed, those of the window that differ from initial and every one past it, by address;
   *count of them (NULL with none) */
static Byte* qemu_bytes(const u8* initial, const SnxQrefBatch* b, const SnxQrefResult* r, size_t* count) {
    size_t n = 0, total = 0;
    unsigned c;
    Byte* out;
    for (c = 0; c < r->changes; ++c) total += b->changes[r->first_change + c].size;
    *count = 0;
    if (!total || !(out = (Byte*)malloc(total * sizeof *out))) return NULL;
    for (c = 0; c < r->changes; ++c) {
        const SnxQrefChange* ch = &b->changes[r->first_change + c];
        u32 k;
        for (k = 0; k < ch->size; ++k) {
            const u64 a = ch->address + k;
            const u8 v = b->bytes[ch->offset + k];
            if (a >= SAIL_WINDOW && a < SAIL_WINDOW + SAIL_WINDOW_SIZE && initial[a - SAIL_WINDOW] == v) continue;
            out[n].address = a, out[n++].value = v;
        }
    }
    qsort(out, n, sizeof *out, by_address);
    *count = n;
    return out;
}

u64 sail_end_hash(u32 word, const SailEnd* e, const u8* initial, const u8* work, const SnxQrefBatch* b,
                  const SnxQrefResult* r) {
    u64 h = mix(SAIL_GENERATOR, kind(e->how.outcome)), value = 0;
    int fixed, env = environment_register(word, &value, &fixed);
    unsigned i;
    if (e->how.outcome != SNX_QREF_UNDEF) h = mix(h, e->how.pc & TBI_MASK);
    if (e->how.outcome == SNX_QREF_BRK || e->how.outcome == SNX_QREF_SVC) h = mix(h, e->how.imm);
    for (i = 0; i < 31; ++i) {
        u64 x = e->state.x[i];
        /* QEMU's value of a system register is not the Switch's: the Switch's where it is a constant, else none */
        if ((int)i == env) x = !fixed ? 0 : r ? value : x;
        h = mix(h, x);
    }
    h = mix(h, e->state.sp);
    h = mix(h, e->state.nzcv & 0xF0000000u);
    h = mix(h, e->state.fpcr);
    h = mix(h, e->state.fpsr);
    for (i = 0; i < 32; ++i) h = mix(mix(h, e->state.v[i][0]), e->state.v[i][1]);
    if (r) {
        size_t n, k;
        Byte* bytes = qemu_bytes(initial, b, r, &n);
        if (r->truncated) h = mix(h, ~0ull);
        for (k = 0; k < n; ++k) h = mix(h, bytes[k].address << 8 | bytes[k].value);
        free(bytes);
    } else if (e->wrote) {
        u64 a;
        for (a = 0; a < SAIL_TOUCHED; a += 8) {
            if (snx_ld_le64(work + a) == snx_ld_le64(initial + a)) continue;
            for (i = 0; i < 8; ++i)
                if (work[a + i] != initial[a + i]) h = mix(h, (SAIL_WINDOW + a + i) << 8 | work[a + i]);
        }
    }
    return h;
}

void sail_compare(u32 word, const SnxQrefState* start, const SailEnd* l, const SailEnd* q, const u8* initial,
                  const u8* work, const SnxQrefBatch* b, const SnxQrefResult* r, char* cause, size_t cause_size,
                  char* text, size_t text_size) {
    size_t used = 0, cu = 0, n, k;
    unsigned i;
    int x = 0, v = 0;
    u64 switch_value = 0;
    int fixed, env = environment_register(word, &switch_value, &fixed);
    Byte* bytes;
#define PART(name)                                                                                                     \
    cu += (size_t)snprintf(cause + cu, cu < cause_size ? cause_size - cu : 0, "%s%s", cu ? "+" : "", name)
#define TEXT(...)                                                                                                      \
    do {                                                                                                               \
        if (used < text_size) used += (size_t)snprintf(text + used, text_size - used, __VA_ARGS__);                    \
    } while (0)
    cause[0] = 0;
    text[0] = 0;
    if (kind(l->how.outcome) != kind(q->how.outcome)) {
        snprintf(cause, cause_size, "native-%s/qemu-%s", kind_name(l->how.outcome), kind_name(q->how.outcome));
        TEXT("  native %s pc %llx, qemu %s pc %llx (signal %u, fault %llx)\n", snx_qref_outcome_name(l->how.outcome),
             (unsigned long long)l->how.pc, snx_qref_outcome_name(q->how.outcome), (unsigned long long)q->how.pc,
             r->signo, (unsigned long long)r->fault);
        return;
    }
    /* the pc after an UNDEF is the runtime's to choose. Linux sets TCR_EL1.TBI0, so QEMU's user mode drops the top
       byte of a branch's target: the kernel's setting, not the instruction's */
    if (l->how.outcome != SNX_QREF_UNDEF && (l->how.pc & TBI_MASK) != (q->how.pc & TBI_MASK)) {
        PART("pc");
        TEXT("  pc native %llx qemu %llx\n", (unsigned long long)l->how.pc, (unsigned long long)q->how.pc);
    }
    if ((l->how.outcome == SNX_QREF_BRK || l->how.outcome == SNX_QREF_SVC) && l->how.imm != q->how.imm) {
        PART("imm");
        TEXT("  imm native %llx qemu %llx\n", (unsigned long long)l->how.imm, (unsigned long long)q->how.imm);
    }
    for (i = 0; i < 31; ++i) {
        u64 want = q->state.x[i];
        if ((int)i == env) {
            if (!fixed) continue;
            want = switch_value;
        }
        if (l->state.x[i] != want) {
            if (!x++) PART("x");
            TEXT("  x%u: start %016llx native %016llx qemu %016llx\n", i, (unsigned long long)start->x[i],
                 (unsigned long long)l->state.x[i], (unsigned long long)want);
        }
    }
    if (l->state.sp != q->state.sp) {
        PART("sp");
        TEXT("  sp: start %016llx native %016llx qemu %016llx\n", (unsigned long long)start->sp,
             (unsigned long long)l->state.sp, (unsigned long long)q->state.sp);
    }
    if ((l->state.nzcv ^ q->state.nzcv) & 0xF0000000u) {
        PART("nzcv");
        TEXT("  nzcv: start %x native %x qemu %x\n", start->nzcv >> 28, l->state.nzcv >> 28, q->state.nzcv >> 28);
    }
    for (i = 0; i < 32; ++i)
        if (l->state.v[i][0] != q->state.v[i][0] || l->state.v[i][1] != q->state.v[i][1]) {
            if (!v++) PART("v");
            TEXT("  v%u: start %016llx:%016llx native %016llx:%016llx qemu %016llx:%016llx\n", i,
                 (unsigned long long)start->v[i][1], (unsigned long long)start->v[i][0],
                 (unsigned long long)l->state.v[i][1], (unsigned long long)l->state.v[i][0],
                 (unsigned long long)q->state.v[i][1], (unsigned long long)q->state.v[i][0]);
        }
    if (l->state.fpsr != q->state.fpsr) {
        PART("fpsr");
        TEXT("  fpsr: start %08x native %08x qemu %08x\n", start->fpsr, l->state.fpsr, q->state.fpsr);
    }
    if (l->state.fpcr != q->state.fpcr) {
        PART("fpcr");
        TEXT("  fpcr: start %08x native %08x qemu %08x\n", start->fpcr, l->state.fpcr, q->state.fpcr);
    }
    /* memory: the bytes that differ from the start, QEMU's against this side's in the part a word reaches */
    bytes = qemu_bytes(initial, b, r, &n);
    {
        u64 a, first = ~0ull;
        int differs = r->truncated;
        for (k = 0; k < n && !differs; ++k)
            if (bytes[k].address >= SAIL_WINDOW + SAIL_TOUCHED ||
                work[bytes[k].address - SAIL_WINDOW] != bytes[k].value)
                differs = 1, first = bytes[k].address;
        for (a = 0, k = 0; l->wrote && a < SAIL_TOUCHED && !differs; ++a) {
            if (work[a] == initial[a]) continue;
            while (k < n && bytes[k].address < SAIL_WINDOW + a) ++k;
            if (k == n || bytes[k].address != SAIL_WINDOW + a) differs = 1, first = SAIL_WINDOW + a;
        }
        if (differs) {
            PART("mem");
            if (first == ~0ull)
                TEXT("  memory: QEMU's changes truncated\n");
            else if (first < SAIL_WINDOW + SAIL_TOUCHED)
                TEXT("  memory: first difference at %llx, start %02x native %02x (%zu bytes QEMU changed)\n",
                     (unsigned long long)first, initial[first - SAIL_WINDOW], work[first - SAIL_WINDOW], n);
            else
                TEXT("  memory: QEMU changed %llx, past what a word of the test reaches\n", (unsigned long long)first);
        }
    }
    free(bytes);
#undef PART
#undef TEXT
}

/* ---- the reference ---- */

u64 sail_generator(void) {
    u64 h = mix(SAIL_GENERATOR, SAIL_WINDOW);
    h = mix(mix(mix(h, SAIL_WINDOW_SIZE), SAIL_PC), SAIL_PAIR_PC);
    return mix(mix(mix(h, SAIL_SEED), SAIL_TOUCHED), SAIL_STATES_MAX);
}

static int put64(FILE* f, u64 v) {
    u8 b[8];
    snx_st_le64(b, v);
    return fwrite(b, 1, 8, f) == 8 ? 0 : -1;
}
typedef struct {
    u32 word;
    size_t at;
} Order;
static int by_word(const void* a, const void* b) {
    const Order *x = (const Order*)a, *y = (const Order*)b;
    return x->word < y->word ? -1 : x->word > y->word;
}

int sail_ref_write(const char* path, const u32* words, const u64* hashes, size_t count, char* error, size_t size) {
    FILE* f = fopen(path, "wb");
    Order* order = (Order*)malloc((count ? count : 1) * sizeof *order);
    size_t i, s;
    int failed = !f || !order;
    for (i = 0; i < count && order; ++i) order[i].word = words[i], order[i].at = i;
    if (order) qsort(order, count, sizeof *order, by_word);
    if (!failed)
        failed = put64(f, REF_MAGIC) || put64(f, sail_generator()) || put64(f, count) || put64(f, SAIL_STATES_MAX);
    for (i = 0; i < count && !failed; ++i) failed = put64(f, order[i].word);
    for (i = 0; i < count && !failed; ++i)
        for (s = 0; s < SAIL_STATES_MAX && !failed; ++s) failed = put64(f, hashes[order[i].at * SAIL_STATES_MAX + s]);
    if (f && fclose(f)) failed = 1;
    free(order);
    if (failed) snprintf(error, size, "cannot write %s", path);
    return failed ? -1 : 0;
}

int sail_ref_read(const char* path, SailRef* ref, char* error, size_t size) {
    FILE* f = fopen(path, "rb");
    u8* data = NULL;
    long length = 0;
    u64 states;
    size_t i;
    memset(ref, 0, sizeof *ref);
    if (!f) {
        snprintf(error, size, "%s is not there", path);
        return -1;
    }
    if (!fseek(f, 0, SEEK_END) && (length = ftell(f)) >= 32 && !fseek(f, 0, SEEK_SET) &&
        (data = (u8*)malloc((size_t)length)) && fread(data, 1, (size_t)length, f) != (size_t)length) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (!data || snx_ld_le64(data) != REF_MAGIC) {
        free(data);
        snprintf(error, size, "%s is not a reference of the sail tests (no %s at its start)", path, "SNXSAIL1");
        return -1;
    }
    ref->generator = snx_ld_le64(data + 8);
    ref->count = (size_t)snx_ld_le64(data + 16);
    states = snx_ld_le64(data + 24);
    if (ref->generator != sail_generator() || states != SAIL_STATES_MAX) {
        snprintf(error, size,
                 "%s was recorded with generator %016llx and %llu states a word, this test draws with "
                 "%016llx and %u: record it again (the qemu-reference job does)",
                 path, (unsigned long long)ref->generator, (unsigned long long)states,
                 (unsigned long long)sail_generator(), SAIL_STATES_MAX);
        free(data);
        return -1;
    }
    if ((u64)length != 32 + ref->count * 8 * (1 + SAIL_STATES_MAX)) {
        snprintf(error, size, "%s holds %ld bytes, not the %llu its %zu words take", path, length,
                 (unsigned long long)(32 + ref->count * 8 * (1 + SAIL_STATES_MAX)), ref->count);
        free(data);
        return -1;
    }
    ref->words = (u32*)malloc((ref->count ? ref->count : 1) * sizeof *ref->words);
    ref->hashes = (u64*)malloc((ref->count ? ref->count : 1) * SAIL_STATES_MAX * sizeof *ref->hashes);
    if (!ref->words || !ref->hashes) {
        free(data);
        sail_ref_free(ref);
        snprintf(error, size, "no memory for %s", path);
        return -1;
    }
    for (i = 0; i < ref->count; ++i) ref->words[i] = (u32)snx_ld_le64(data + 32 + 8 * i);
    for (i = 0; i < ref->count * SAIL_STATES_MAX; ++i) ref->hashes[i] = snx_ld_le64(data + 32 + 8 * (ref->count + i));
    free(data);
    return 0;
}

const u64* sail_ref_find(const SailRef* ref, u32 word) {
    size_t lo = 0, hi = ref->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (ref->words[mid] < word)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < ref->count && ref->words[lo] == word ? ref->hashes + lo * SAIL_STATES_MAX : NULL;
}
void sail_ref_free(SailRef* ref) {
    free(ref->words);
    free(ref->hashes);
    memset(ref, 0, sizeof *ref);
}

/* ---- holding the words ---- */

#define BATCH 8192 /* QEMU cases a run of its runner */

static const char* name_of(u32 word) {
    SnxA64Op op;
    const char* n = op_name(word, g_sail.target, &op);
    return n ? n : "UNDEF";
}

size_t sail_hold(SailHold* h, const u32* words, size_t n, unsigned states, size_t* compared) {
    SnxQrefBatch batch = {0};
    RtAPI api = {.cas128 = snx_runtime_cas128};
    CPU* starts = (CPU*)malloc(sizeof *starts * BATCH);
    u8* initial = (u8*)malloc(SAIL_WINDOW_SIZE);
    u32* recorded = h->record ? (u32*)malloc(sizeof(u32) * (n ? n : 1)) : NULL;
    u64* hashes = h->record ? (u64*)malloc(sizeof(u64) * SAIL_STATES_MAX * (n ? n : 1)) : NULL;
    unsigned char* failed = (unsigned char*)calloc(n ? n : 1, 1);
    size_t failures = 0, i, missing = 0, nrecorded = 0, disagree = 0, nwhy = 0, k;
    struct {
        const char* why;
        size_t count;
        u32 example;
    } whys[8];
    u32 missing_word = 0;
    char error[1024], cause[256], text[4096];
    if (!starts || !initial || !failed || (h->record && (!recorded || !hashes))) {
        fprintf(stderr, "FAIL: no memory for the comparison with QEMU\n");
        free(starts), free(initial), free(recorded), free(hashes), free(failed);
        return 1;
    }
    sail_rt(&api);
    sail_window(initial);
    memcpy(h->work, initial, SAIL_WINDOW_SIZE);
    g_sail.dirty = 0;
    for (i = 0; i < n;) {
        size_t first = i, end = i, cases = 0;
        while (end < n && (end - first + 1) * states <= BATCH) ++end;
        if (end == first) end = first + 1;
        snx_qref_clear(&batch);
        for (k = first; k < end; ++k) {
            const char* why = sail_unheld(words[k]);
            unsigned s;
            if (why) {
                size_t j = 0;
                while (j < nwhy && whys[j].why != why) ++j;
                if (j == nwhy && nwhy < sizeof whys / sizeof *whys) whys[nwhy++] = (typeof(*whys)){why, 0, words[k]};
                if (j < nwhy) ++whys[j].count;
                continue;
            }
            for (s = 0; s < states; ++s, ++cases) {
                sail_start(&starts[cases], words[k], s, &api);
                if (h->qemu && sail_add_case(&batch, words[k], s, &starts[cases]) < 0) {
                    fprintf(stderr, "FAIL: %s %08x: a case QEMU's runner has no room for\n", name_of(words[k]),
                            (unsigned)words[k]);
                    failures += 1 + disagree;
                    goto out;
                }
            }
        }
        if (h->qemu && cases && snx_qref_run(&batch, h->qemu[0], h->qemu[1], "max", h->qemu[2], error, sizeof error)) {
            fprintf(stderr, "FAIL: QEMU: %s\n", error);
            failures += 1 + disagree;
            goto out;
        }
        cases = 0;
        for (k = first; k < end; ++k) {
            const u32 w = words[k];
            const u64* expected = NULL;
            unsigned s;
            if (sail_unheld(w)) continue;
            if (h->ref && !(expected = sail_ref_find(h->ref, w))) {
                if (!missing++) missing_word = w;
                cases += states;
                continue;
            }
            if (h->record) recorded[nrecorded] = w;
            for (s = 0; s < states; ++s, ++cases) {
                const SnxQrefResult* r = h->qemu ? &batch.results[cases] : NULL;
                SailEnd qe, ne;
                SnxQrefState qstart;
                u64 hq;
                unsigned form;
                if (r) {
                    sail_qemu_end(&qe, r);
                    hq = sail_end_hash(w, &qe, initial, NULL, &batch, r);
                } else
                    hq = expected[s];
                if (h->record) hashes[nrecorded * SAIL_STATES_MAX + s] = hq;
                sail_state_of(&starts[cases], &qstart);
                for (form = 0; form < h->nforms; ++form) {
                    u64 hn;
                    int differs;
                    if (failed[k] || !h->has(h->user, k, form)) continue;
                    if (h->verbose == 2)
                        fprintf(stderr, "run %s %08x (%s host) state %u\n", name_of(w), (unsigned)w, h->forms[form], s);
                    h->run(h->user, k, form, s, &starts[cases], h->work, initial, &ne);
                    ++*compared;
                    hn = sail_end_hash(w, &ne, initial, h->work, NULL, NULL);
                    cause[0] = text[0] = 0;
                    if (r) {
                        sail_compare(w, &qstart, &ne, &qe, initial, h->work, &batch, r, cause, sizeof cause, text,
                                     sizeof text);
                        /* the reference's path holds a word by the hash alone: it must say what the comparison says */
                        if ((cause[0] != 0) != (hn != hq)) {
                            ++disagree;
                            fprintf(stderr, "FAIL: %s %08x (%s host) state %u: the hashes %s where the comparison %s\n",
                                    name_of(w), (unsigned)w, h->forms[form], s, hn == hq ? "agree" : "differ",
                                    cause[0] ? cause : "finds no difference");
                        }
                    }
                    differs = r ? cause[0] != 0 : hn != hq;
                    if (!differs) continue;
                    ++failures;
                    failed[k] = 1;
                    fprintf(stderr, "FAIL: %s %08x (%s host) state %u: the %s code's end is not QEMU's (%s)\n",
                            name_of(w), (unsigned)w, h->forms[form], s, h->name, r ? cause : "the reference's hash");
                    if (h->verbose && r) fputs(text, stderr);
                }
            }
            if (h->record) ++nrecorded;
        }
        i = end;
    }
    failures += disagree;
    for (k = 0; k < nwhy; ++k)
        printf("%s: %zu words not held to QEMU, as %08x (%s): %s\n", h->name, whys[k].count, (unsigned)whys[k].example,
               name_of(whys[k].example), whys[k].why);
    if (missing)
        printf("%s: %zu words the reference holds none of, as %08x (%s): not compared\n", h->name, missing,
               (unsigned)missing_word, name_of(missing_word));
    if (h->record) {
        if (sail_ref_write(h->record, recorded, hashes, nrecorded, error, sizeof error)) {
            fprintf(stderr, "FAIL: %s\n", error);
            ++failures;
        } else
            printf("%s: QEMU's ends of %zu words, %u states each, recorded in %s\n", h->name, nrecorded,
                   SAIL_STATES_MAX, h->record);
    }
out:
    snx_qref_free(&batch);
    free(starts), free(initial), free(recorded), free(hashes), free(failed);
    return failures;
}

/* ---- the words, and what LLVM gave of those QEMU can't stand for ---- */

static u64 g_candidates_rng;
static u64 candidates_rnd(void) {
    g_candidates_rng ^= g_candidates_rng << 13;
    g_candidates_rng ^= g_candidates_rng >> 7;
    g_candidates_rng ^= g_candidates_rng << 17;
    return g_candidates_rng;
}
size_t sail_candidates(unsigned samples, u32** out) {
    static const u32 fields[3] = {0x1fu, 0x1fu << 5, 0x1fu << 16};
    u32* rows = NULL;
    size_t count, i, n = 0;
    g_candidates_rng = 0x9e3779b97f4a7c15ull;
    count = lqd_row_words(samples, candidates_rnd, &rows);
    *out = (u32*)malloc(sizeof **out * (count * 8 + 1));
    if (!*out) {
        free(rows);
        return 0;
    }
    for (i = 0; i < count; ++i) {
        SnxA64Op op, other;
        const char* name = op_name(rows[i], g_sail.target, &op);
        unsigned f, k;
        if (!name) continue;
        for (f = 0; f < 8; ++f) {
            u32 w = rows[i];
            for (k = 0; k < 3; ++k)
                if (f & (1u << k)) w |= fields[k];
            if (f && (w == rows[i] || !op_name(w, g_sail.target, &other) || other != op)) continue;
            (*out)[n++] = w;
        }
    }
    free(rows);
    return n;
}

int sail_frozen_part(u32 word) {
    SnxA64Insn insn;
    if (sail_unheld(word)) return SAIL_FROZEN_UNHELD;
    snx_a64_decode_word(word, g_sail.target, &insn);
    if (insn.op == SNX_A64_OP_STXR || (insn.op == SNX_A64_OP_STXP && insn.args.sz == 3)) return SAIL_FROZEN_EXCLUSIVE;
    return -1;
}

void sail_frozen_start(CPU* c, u32 word, unsigned s, const RtAPI* rt, const u8* initial) {
    sail_start(c, word, s, rt);
    c->pc = SAIL_PC;
    if (sail_frozen_part(word) == SAIL_FROZEN_EXCLUSIVE) {
        SnxA64Insn insn;
        u64 address;
        snx_a64_decode_word(word, g_sail.target, &insn);
        address = c->x[insn.args.rn];
        c->excl_on = 1;
        c->excl_addr = address;
        c->excl_val = ~snx_ld_le64(initial + (address - SAIL_WINDOW));
        c->excl_val2 = snx_ld_le64(initial + (address - SAIL_WINDOW) + 8);
        c->excl_size = insn.op == SNX_A64_OP_STXP ? 0x13 : (u32)insn.args.sz;
    }
}

u64 sail_frozen_inputs(const u32* candidates, size_t n) {
    u64 h = snx_frozen_mix(SAIL_FROZEN_GENERATOR, sail_generator());
    size_t i;
    for (i = 0; i < n; ++i) {
        const int part = sail_frozen_part(candidates[i]);
        if (part >= 0) h = snx_frozen_mix(h, (u64)(unsigned)part << 32 | candidates[i]);
    }
    return h;
}

u64 sail_frozen_word(SailHold* h, size_t i, unsigned form, u32 word, const u8* initial) {
    RtAPI api = {.cas128 = snx_runtime_cas128};
    u64 fold = SAIL_FROZEN_GENERATOR;
    unsigned s;
    sail_rt(&api);
    for (s = 0; s < SAIL_STATES_MAX; ++s) {
        CPU start;
        SailEnd e;
        sail_frozen_start(&start, word, s, &api, initial);
        h->run(h->user, i, form, s, &start, h->work, initial, &e);
        fold = snx_frozen_mix(fold, sail_end_hash(word, &e, initial, h->work, NULL, NULL));
    }
    return fold;
}

size_t sail_frozen(SailHold* h, const char* path, const u32* words, size_t n, size_t* compared) {
    SnxFrozen f;
    u32* candidates = NULL;
    u8* initial = (u8*)malloc(SAIL_WINDOW_SIZE);
    size_t ncand, i, w, failures = 0, held[SAIL_FROZEN_PARTS] = {0}, index[SAIL_FROZEN_PARTS] = {0}, none = 0;
    Order* order;
    char error[1024];
    u64 inputs;
    if (!initial) return 1;
    if (snx_frozen_read(&f, path, SAIL_FROZEN_GENERATOR, error, sizeof error)) {
        fprintf(stderr, "FAIL: %s\n", error);
        free(initial);
        return 1;
    }
    ncand = sail_candidates(3, &candidates);
    inputs = sail_frozen_inputs(candidates, ncand);
    {
        size_t counts[SAIL_FROZEN_PARTS] = {0};
        for (i = 0; i < ncand; ++i) {
            const int part = sail_frozen_part(candidates[i]);
            if (part >= 0) ++counts[part];
        }
        if (f.parts != SAIL_FROZEN_PARTS || f.inputs != inputs || f.items[0] != counts[0] || f.items[1] != counts[1]) {
            fprintf(stderr,
                    "FAIL: %s holds %zu parts of %zu and %zu words, its inputs %016llx; this test draws 2 of %zu and "
                    "%zu, inputs %016llx: record it again (snx-test-sail-frozen --record, where LLVM is built)\n",
                    path, f.parts, f.items[0], f.parts > 1 ? f.items[1] : 0, (unsigned long long)f.inputs, counts[0],
                    counts[1], (unsigned long long)inputs);
            snx_frozen_free(&f);
            free(candidates), free(initial);
            return 1;
        }
    }
    sail_window(initial);
    memcpy(h->work, initial, SAIL_WINDOW_SIZE);
    g_sail.dirty = 0;
    /* each candidate of a part the test made: its index in words, by a sorted copy */
    order = (Order*)malloc(sizeof *order * (n ? n : 1));
    if (!order) {
        snx_frozen_free(&f);
        free(candidates), free(initial);
        return 1;
    }
    for (i = 0; i < n; ++i) order[i].word = words[i], order[i].at = i;
    qsort(order, n, sizeof *order, by_word);
    for (i = 0; i < ncand; ++i) {
        const int part = sail_frozen_part(candidates[i]);
        size_t item, lo = 0, hi = n;
        unsigned form;
        if (part < 0) continue;
        item = index[part]++;
        while (lo < hi) {
            const size_t mid = lo + (hi - lo) / 2;
            if (order[mid].word < candidates[i])
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo == n || order[lo].word != candidates[i]) continue; /* not made */
        w = order[lo].at;
        if (!snx_frozen_hash(&f, (size_t)part, item)) {
            ++none;
            continue;
        }
        ++held[part];
        for (form = 0; form < h->nforms; ++form) {
            u64 got;
            if (!h->has(h->user, w, form)) continue;
            got = sail_frozen_word(h, w, form, candidates[i], initial);
            *compared += SAIL_STATES_MAX;
            if (got == snx_frozen_hash(&f, (size_t)part, item)) continue;
            ++failures;
            fprintf(stderr,
                    "FAIL: %s %08x (%s host): its %u frozen states' ends hash to %016llx, %s:%zu holds %016llx "
                    "(LLVM's)\n",
                    name_of(candidates[i]), (unsigned)candidates[i], h->forms[form], SAIL_STATES_MAX,
                    (unsigned long long)got, path, snx_frozen_line(&f, (size_t)part, item),
                    (unsigned long long)snx_frozen_hash(&f, (size_t)part, item));
        }
    }
    printf("%s: %zu words held to LLVM's ends frozen in %s: %zu not held to QEMU, %zu store-exclusives with the "
           "monitor armed and the memory changed\n",
           h->name, held[0] + held[1], path, held[0], held[1]);
    if (none) printf("%s: %zu words of the file had no LLVM step: not compared\n", h->name, none);
    snx_frozen_free(&f);
    free(candidates), free(initial), free(order);
    return failures;
}
