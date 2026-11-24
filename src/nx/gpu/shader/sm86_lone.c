/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An SM86 compute program's use of its warp's other lanes, on a device whose compute subgroups are narrower than a warp
   of 32 (sm86_translate.c's lane_gate, which runs each invocation as a warp of its own), handed to the proof that the
   Maxwell programs take (warp_idioms.c): the program's lanes, masks, ballots, counts, shuffles, comparisons and atomics
   as Maxwell words, its registers and predicates renamed into that instruction set's, its own ones as they are, and the
   notes of what each instruction reads and writes (the translator's, warp_notes.h). What the idioms are and why they
   hold is there.

   A branch over the lanes of one leader (BRA past a straight run of instructions, which the compiler writes for a block
   that a leader runs) is turned into a guard of each of those instructions, which is the same program. */
#include "nx/gpu/shader/sm86.h"
#include "nx/gpu/shader/translate.h"
#include <stdio.h>
#include <string.h>

/* the notes' variables of this translator (warp_notes.h), then of the pseudo program's */
#define UREGS_AT 256u
#define NOVAR    0xffffffffu
/* the longest run a branch is turned into guards for */
#define RUN_MAX  96u

typedef struct {
    const Sm86Insn* in;
    u32 n;
    u32 reg[UREGS_AT +
            64]; /* a register's number, uniform ones from UREGS_AT, as the pseudo program's: NOVAR for none */
    u32 regs;
    u8 pred[14]; /* the predicates, then the uniform ones: 0 to 6, 0xff for none */
    u32 preds;
    u32* at;      /* each instruction's index in the pseudo program */
    u32 total;    /* its instructions */
    u8* run_pred; /* the predicate whose branch is turned into this instruction's guard, 0xff for none */
    u8* run_not;  /* it is the predicate's false */
    u8* idle;     /* the branch turned into guards */
    u32* note_at; /* the notes of each instruction: by first, into note */
    u32* note;    /* the variable << 1 | 1 for a write */
} Lone;

static int grow_reg(Lone* l, u32 var) {
    if (l->reg[var] != NOVAR) return 1;
    if (l->regs == 254) return 0;
    l->reg[var] = l->regs++;
    return 1;
}
static int grow_pred(Lone* l, u32 p) {
    if (l->pred[p] != 0xff) return 1;
    if (l->preds == 7) return 0;
    l->pred[p] = (u8)l->preds++;
    return 1;
}
/* the pseudo program's number of a register operand as it is, 255 for a zero register, NOVAR for another. Of an integer
   instruction: the bit of an absolute value is another field there, which each looks at */
static u32 operand(const Lone* l, const Sm86Src* s) {
    if (s->neg) return NOVAR;
    if (s->kind == SM86_SRC_REG) return s->reg >= SM86_RZ ? 255 : l->reg[s->reg];
    if (s->kind == SM86_SRC_UREG) return s->reg >= SM86_URZ ? 255 : l->reg[UREGS_AT + s->reg];
    return NOVAR;
}
/* a register that an instruction's field names: 255 the zero register, NOVAR one that nothing read or wrote */
static u32 reg_of(const Lone* l, u32 r) { return r >= SM86_RZ ? 255 : l->reg[r]; }

/* Rd, Ra and Rb in an ALU word */
static u64 word_dab(u64 form, u32 a, u32 b) { return form | (u64)a << 8 | (u64)b << 20; }
/* the forms that warp_idioms.c takes */
#define VOTE_ALL   0x50d8e38000070000ull
#define FLO_U32    0x5c30000000070000ull
#define POPC_R     0x5c08000000070000ull
#define LOP_AND    0x5c47000000070000ull
#define IADD_R     0x5c10000000070000ull
#define ISETP_EQ   0x5b64038000070007ull
#define ISETP_FULL 0x376403fffff70007ull
#define IMNMX_R    0x5c20038000070000ull
#define SHFL_IDX   0xef17007c20070000ull
#define SHFL_BFLY  0xef17007cf0070000ull
#define PLAIN_MOV  0x5c98000000070000ull

/* what an instruction is, as warp_idioms.c reads it: its opcode, and its word with no guard */
static int pseudo(const Lone* l, u32 i, u64* w) {
    const Sm86Insn* in = &l->in[i];
    const u32 a = operand(l, &in->a), b = operand(l, &in->b), c = operand(l, &in->c);
    *w = PLAIN_MOV;
    switch (in->op) {
        case SM86_VOTE:
        case SM86_UVOTE:
            /* the ballot of the running lanes, with no predicate written */
            *w = sm86_field(in, 87, 3) == SM86_PT && !sm86_field(in, 90, 1) && sm86_field(in, 81, 3) == SM86_PT
                     ? VOTE_ALL
                     : 0;
            return O_VOTE;
        case SM86_S2R:
        case SM86_S2UR: *w = (u64)sm86_field(in, 72, 8) << 20 | 0x70000ull; return O_S2R;
        case SM86_SHFL_II:
        case SM86_SHFL_RI: {
            const u32 value = reg_of(l, sm86_field(in, 24, 8)), mode = sm86_field(in, 58, 2),
                      seg = sm86_field(in, 40, 13);
            *w = 0;
            if (sm86_field(in, 81, 3) != SM86_PT || seg != 0x1f || value == 255 || value == NOVAR) return O_SHFL;
            if (mode == 0 && in->op == SM86_SHFL_RI && reg_of(l, sm86_field(in, 32, 8)) != NOVAR)
                *w = word_dab(SHFL_IDX, value, reg_of(l, sm86_field(in, 32, 8)));
            else if (mode == 3 && in->op == SM86_SHFL_II)
                *w = SHFL_BFLY | (u64)value << 8 | (u64)sm86_field(in, 53, 5) << 20;
            return O_SHFL;
        }
        case SM86_MATCH:
            /* MATCH.ALL of a pair, unguarded, its predicate written: the warp's addresses' */
            *w = 0x70000ull;
            return sm86_field(in, 79, 1) || !sm86_field(in, 73, 1) || sm86_field(in, 81, 3) == SM86_PT ? O_SHFL
                                                                                                       : LO_MATCH;
        case SM86_REDUX:
        case SM86_SHFL_IR:
        case SM86_SHFL_RR: *w = 0; return O_SHFL;
        case SM86_FLO:
        case SM86_UFLO:
            /* the highest set bit, or with SH (74) the 31 - it of the same of a mask reversed, the lowest running lane */
            if (b == NOVAR || sm86_field(in, 63, 1) || sm86_field(in, 73, 1)) return O_MOV_R;
            if (!sm86_field(in, 74, 1)) {
                *w = FLO_U32 | (u64)b << 20;
                return O_MOV_R;
            }
            *w = (u64)b << 20 | 0x70000ull;
            return LO_FLO_SH;
        case SM86_BREV:
        case SM86_UBREV:
            if (b == NOVAR) return O_MOV_R;
            *w = (u64)b << 20 | 0x70000ull;
            return LO_BREV;
        case SM86_POPC:
        case SM86_UPOPC:
            if (b == NOVAR || sm86_field(in, 63, 1)) return O_MOV_R;
            *w = POPC_R | (u64)b << 20;
            return O_MOV_R;
        case SM86_LOP3:
        case SM86_ULOP3:
            /* a & b, whatever c is, which the table has no bit of */
            if (a == NOVAR || b == NOVAR || sm86_field(in, 72, 8) != 0xc0 || sm86_field(in, 81, 3) != SM86_PT)
                return O_MOV_R;
            *w = word_dab(LOP_AND, a, b);
            return O_MOV_R;
        case SM86_ISETP:
        case SM86_UISETP: {
            const u32 compare = sm86_field(in, 76, 3);
            /* compared, AND of a true predicate, into one predicate, with no carry in */
            if (sm86_field(in, 72, 1) || sm86_field(in, 74, 2) || sm86_field(in, 87, 3) != SM86_PT ||
                sm86_field(in, 90, 1) || sm86_field(in, 84, 3) != SM86_PT || sm86_field(in, 81, 3) == SM86_PT ||
                a == NOVAR)
                return O_MOV_R;
            /* equal, or not, to a register's or a zero's; or to all ones */
            if (compare != 2 && compare != 5) return O_MOV_R;
            const int imm = in->b.kind == SM86_SRC_IMM;
            if (imm && in->b.value == 0xffffffffu && compare == 2) {
                *w = ISETP_FULL | (u64)a << 8;
                return O_MOV_R;
            }
            if (b == NOVAR && !(imm && in->b.value == 0)) return O_MOV_R;
            const u64 regs = word_dab(0, a, b == NOVAR ? 255 : b);
            *w = compare == 2 ? ISETP_EQ | regs : regs | 0x70000ull;
            return compare == 2 ? O_MOV_R : LO_ISETP_NE;
        }
        case SM86_IMNMX:
        case SM86_UIMNMX:
            /* the minimum where the predicate, PT or !PT, holds, else the maximum */
            if (a == NOVAR || b == NOVAR || sm86_field(in, 87, 3) != SM86_PT || sm86_field(in, 81, 3) != SM86_PT)
                return O_MOV_R;
            *w = word_dab(IMNMX_R, a, b) | (u64)sm86_field(in, 90, 1) << 42 | (u64)sm86_field(in, 73, 1) << 48;
            return O_MOV_R;
        case SM86_IADD3:
        case SM86_UIADD3:
            if (sm86_field(in, 74, 1) || sm86_field(in, 81, 3) != SM86_PT || sm86_field(in, 84, 3) != SM86_PT)
                return O_MOV_R;
            if (c == 255 && a != NOVAR && b != NOVAR) *w = word_dab(IADD_R, a, b);
            return O_MOV_R;
        case SM86_IMAD: /* a * 1 + c */
            if (!sm86_field(in, 74, 1) && in->b.kind == SM86_SRC_IMM && in->b.value == 1 && a != NOVAR && c != NOVAR)
                *w = word_dab(IADD_R, a, c);
            return O_MOV_R;
        case SM86_LDC:
        case SM86_ULDC:
        case SM86_ULDC_IDX: return O_LDC;
        case SM86_LDS: return O_LDS;
        case SM86_ATOMG:
        case SM86_ATOMG_NOU:
        case SM86_RED:
        case SM86_ATOMS:
        case SM86_ATOMS_NOU: {
            const int red = in->op == SM86_RED, shared = in->op == SM86_ATOMS || in->op == SM86_ATOMS_NOU;
            const u32 type = sm86_field(in, 73, 3), how = sm86_field(in, 87, red ? 3 : 4);
            const u32 data = reg_of(l, sm86_field(in, 32, 8)), address = reg_of(l, sm86_field(in, 24, 8));
            /* an add of 32 bits or a minimum or maximum, else an and, which none of the proofs takes */
            u32 op = type <= 1 && how <= 2 ? how : 5, size = type <= 1 && how ? type : 0;
            const u32 result = red ? 255 : reg_of(l, sm86_field(in, 16, 8));
            if (data == NOVAR || address == NOVAR || result == NOVAR) return O_STG;
            if (red)
                *w = (u64)data | (u64)address << 8 | (u64)size << 20 | (u64)op << 23 | 0x70000ull;
            else
                *w = (u64)result | (u64)address << 8 | (u64)data << 20 | (u64)op << 52 | 0x70000ull |
                     (shared ? (u64)size << 28 : (u64)size << 49);
            return red ? O_RED : shared ? O_ATOMS : O_ATOM;
        }
        case SM86_EXIT: *w = 15ull | 0x70000ull; return O_EXIT;
        case SM86_BRA: *w = 15ull | 0x70000ull; return O_BRA;
        case SM86_BRX:
        case SM86_BRXU:
        case SM86_CALL:
        case SM86_RET:
        case SM86_UBRA:
        case SM86_KILL: return O_BRX;
        case SM86_NOP:
        case SM86_BSSY:
        case SM86_BSYNC:
        case SM86_BREAK:
        case SM86_MEMBAR:
        case SM86_BAR: return O_NOP;
        case SM86_STG:
        case SM86_STG_NOU:
        case SM86_STL:
        case SM86_STL_NOU:
        case SM86_STS:
        case SM86_STS_NOU:
        case SM86_ATOMG_CAS:
        case SM86_ATOMS_CAS:
        case SM86_ATOMS_POPC_INC:
        case SM86_SUST_P:
        case SM86_SUST_D:
        case SM86_SUST_P_B:
        case SM86_SUST_D_B:
        case SM86_SUATOM:
        case SM86_SUATOM_B:
        case SM86_SUATOM_CAS_B:
        case SM86_SURED:
        case SM86_SURED_B: return O_STG;
        case SM86_LDG:
        case SM86_LDG_NOU:
        case SM86_LDL:
        case SM86_TEX:
        case SM86_TLD:
        case SM86_TLD4:
        case SM86_TXD:
        case SM86_SULD_P:
        case SM86_SULD_D: return O_STG;
        default: return O_MOV_R;
    }
}
/* the atomics' opcodes that warp_idioms.c's rules are written on */
static int is_flow_86(const Sm86Insn* in) {
    return in->op == SM86_BRA || in->op == SM86_BRX || in->op == SM86_BRXU || in->op == SM86_UBRA ||
           in->op == SM86_EXIT || in->op == SM86_CALL || in->op == SM86_RET || in->op == SM86_KILL;
}

/* the notes of each instruction, in order: note from note_at[i] to note_at[i + 1], what of it is in the note's bits 0-9 */
static int index_notes(Lone* l, const ShNotes* notes) {
    u32* next = calloc(l->n + 1, sizeof *next);
    l->note_at = calloc(l->n + 2, sizeof *l->note_at);
    l->note = malloc((notes->n + 1) * sizeof *l->note);
    if (next && l->note_at && l->note) {
        for (u32 q = 0; q < notes->n; q++)
            if (notes->word[q] >> 10 && notes->word[q] >> 10 <= l->n) l->note_at[notes->word[q] >> 10]++;
        for (u32 i = 0; i < l->n; i++) l->note_at[i + 1] += l->note_at[i], next[i] = l->note_at[i];
        for (u32 q = 0; q < notes->n; q++) {
            const u32 i = (notes->word[q] >> 10) - 1;
            if (notes->word[q] >> 10 && i < l->n) l->note[next[i]++] = notes->word[q] & 0x3ffu;
        }
    }
    const int good = next && l->note_at && l->note;
    free(next);
    return good;
}
static int remap(Lone* l, const ShNotes* notes) {
    for (u32 v = 0; v < UREGS_AT + 64; v++) l->reg[v] = NOVAR;
    memset(l->pred, 0xff, sizeof l->pred);
    for (u32 q = 0; q < notes->n; q++) {
        const u32 v = notes->word[q] >> 1 & 0x1ffu;
        if (v < SM86_NOTE_PRED) {
            if (!grow_reg(l, v)) return 0;
        } else if (v < SM86_NOTE_UPRED) {
            if (!grow_pred(l, v - SM86_NOTE_PRED)) return 0;
        } else if (v < SM86_NOTE_UREG) {
            if (!grow_pred(l, 7 + v - SM86_NOTE_UPRED)) return 0;
        } else if (!grow_reg(l, UREGS_AT + v - SM86_NOTE_UREG))
            return 0;
    }
    return 1;
}
static u32 variable(const Lone* l, u32 v) {
    if (v < SM86_NOTE_PRED) return l->reg[v];
    if (v < SM86_NOTE_UPRED) return VAR_PRED + l->pred[v - SM86_NOTE_PRED];
    if (v < SM86_NOTE_UREG) return VAR_PRED + l->pred[7 + v - SM86_NOTE_UPRED];
    return l->reg[UREGS_AT + v - SM86_NOTE_UREG];
}
/* the guard of instruction i, as a Maxwell word has it: the predicate's bits (16-18) and the negation (19) */
static int guard_bits(const Lone* l, u32 i, u64* bits) {
    const Sm86Insn* in = &l->in[i];
    u32 p = in->pred, not= in->pred_not;
    if (l->run_pred[i] != 0xff) {
        if (p != SM86_PT) return 0;
        p = l->run_pred[i], not= l->run_not[i];
    }
    if (p != SM86_PT) {
        if (l->pred[p] == 0xff) return 0;
        *bits = (u64)l->pred[p] << 16 | (u64) not<< 19;
    } else
        *bits = (not? 15ull : 7ull) << 16;
    return 1;
}

/* whether instruction t is a branch's target, and the run of instructions from i + 1 up to it is no one else's */
static int no_target_between(const Lone* l, const u8* targeted, u32 from, u32 to) {
    for (u32 j = from; j < to; j++)
        if (targeted[j]) return 0;
    return 1;
}
/* a forward branch over a straight run of instructions as a guard of each. The branch of i, taken where its predicate
   holds, skips them, so they run where it does not; only a run of instructions that run whatever their own guards are
   (none) and no flow, whose first is entered by falling into it */
static void runs(Lone* l) {
    u8* targeted = calloc(l->n + 1, 1);
    if (!targeted) return;
    for (u32 i = 0; i < l->n; i++)
        if (l->in[i].op == SM86_BRA || l->in[i].op == SM86_UBRA) {
            const u32 t = sm86_branch_target(&l->in[i], i);
            if (t <= l->n) targeted[t] = 1;
        }
    for (u32 i = l->n; i-- > 0;) {
        const Sm86Insn* in = &l->in[i];
        if (in->op != SM86_BRA || in->pred == SM86_PT || sm86_field(in, 87, 3) != SM86_PT || sm86_field(in, 90, 1))
            continue;
        const u32 t = sm86_branch_target(in, i);
        if (t <= i + 1 || t > l->n || t - i > RUN_MAX || !no_target_between(l, targeted, i + 1, t)) continue;
        int straight = 1;
        for (u32 j = i + 1; j < t && straight; j++)
            straight = !is_flow_86(&l->in[j]) && l->in[j].pred == SM86_PT && l->run_pred[j] == 0xff;
        if (!straight) continue;
        for (u32 j = i + 1; j < t; j++) l->run_pred[j] = in->pred, l->run_not[j] = !in->pred_not;
        l->idle[i] = 1;
    }
    free(targeted);
}

/* whether instruction i, in this program, writes variable v (the notes' number) */
static int writes_var(const Lone* l, u32 i, u32 v) {
    for (u32 q = l->note_at[i]; q < l->note_at[i + 1]; q++)
        if (l->note[q] == (v << 1 | 1)) return 1;
    return 0;
}
/* whether the program counter goes from instruction i to i + 1 only: no branch, no target in between */
static int straight(const Lone* l, const u8* targeted, u32 from, u32 to) {
    for (u32 j = from; j < to; j++)
        if (is_flow_86(&l->in[j]) || (j > from && targeted[j])) return 0;
    return 1;
}
/* MOV Rd, 1 or IMAD Rd, RZ, RZ, 1, whatever its guard: the value of a constant 1 */
static int makes_one(const Sm86Insn* in) {
    if (in->op == SM86_MOV) return in->b.kind == SM86_SRC_IMM && in->b.value == 1 && !in->b.neg;
    return in->op == SM86_IMAD && in->a.kind == SM86_SRC_REG && in->a.reg >= SM86_RZ && in->b.kind == SM86_SRC_REG &&
           in->b.reg >= SM86_RZ && in->c.kind == SM86_SRC_IMM && in->c.value == 1 && !in->c.neg;
}
/* an atomic add of 32 bits to global memory: its data register and its result register, SM86_RZ for none */
static int add_32(const Sm86Insn* in, u32* data, u32* result) {
    const int red = in->op == SM86_RED;
    *data = sm86_field(in, 32, 8);
    *result = red ? SM86_RZ : sm86_field(in, 16, 8);
    return (red || in->op == SM86_ATOMG || in->op == SM86_ATOMG_NOU) && sm86_field(in, 73, 3) <= 1 &&
           sm86_field(in, 87, red ? 3 : 4) == 0;
}
/* what an atomic adds its address up from, beside the pair of registers a MATCH compared: offset, uniform register and
   how it is made */
static int same_place(const Sm86Insn* x, const Sm86Insn* y) {
    return x->op == y->op
               ? sm86_field(x, 24, 8) == sm86_field(y, 24, 8) && sm86_field(x, 40, 24) == sm86_field(y, 40, 24) &&
                     sm86_field(x, 64, 6) == sm86_field(y, 64, 6) && sm86_field(x, 72, 1) == sm86_field(y, 72, 1) &&
                     sm86_field(x, 90, 2) == sm86_field(y, 90, 2)
               : 0;
}

/* MATCH.ALL of an address pair, the atomics of each lane's own where the addresses differ, and the run that the lanes
   take where they do not (warp_idioms.c's matches): the plan's tables, in this pseudo program's indexes */
static void match_runs(Lone* l, u8* same, u8* own, u8* leave, ShMatchRun* run, u32* runs) {
    u8* targeted = calloc(l->n + 1, 1);
    *runs = 0;
    if (!targeted) return;
    for (u32 i = 0; i < l->n; i++)
        if (l->in[i].op == SM86_BRA || l->in[i].op == SM86_UBRA) {
            const u32 t = sm86_branch_target(&l->in[i], i);
            if (t <= l->n) targeted[t] = 1;
        }
    for (u32 i = 0; i < l->n && *runs < 16; i++) {
        const Sm86Insn* m = &l->in[i];
        const u32 address = sm86_field(m, 24, 8), p = sm86_field(m, 81, 3);
        if (m->op != SM86_MATCH || m->pred != SM86_PT || p == SM86_PT || address + 1 >= SM86_RZ) continue;
        /* the lane's own atomic, under the predicate's false, of an address no one wrote since */
        u32 j = i + 1, data, result;
        while (j < l->n && j - i < 16 && l->in[j].pred == SM86_PT && !is_flow_86(&l->in[j]) &&
               !writes_var(l, j, address) && !writes_var(l, j, address + 1))
            j++;
        if (j >= l->n || !add_32(&l->in[j], &data, &result) || l->in[j].pred != p || !l->in[j].pred_not ||
            sm86_field(&l->in[j], 24, 8) != address || data == SM86_RZ || !straight(l, targeted, i, j + 1))
            continue;
        /* it adds 1 */
        u32 one = j;
        while (one > 0 && !is_flow_86(&l->in[one - 1]) && !targeted[one - 1] && j - one < 64 &&
               !writes_var(l, one - 1, data))
            one--;
        if (one == 0 || is_flow_86(&l->in[one - 1]) || j - one >= 64 || !writes_var(l, one - 1, data) ||
            !makes_one(&l->in[one - 1]) || l->in[one - 1].pred != SM86_PT)
            continue;
        u32 at = j + 1;
        while (at < l->n && l->in[at].op == SM86_NOP) at++;
        const Sm86Insn* go = &l->in[at];
        if (at >= l->n || go->pred != p || !go->pred_not || (go->op != SM86_EXIT && go->op != SM86_BRA) ||
            sm86_field(go, 87, 3) != SM86_PT || sm86_field(go, 90, 1))
            continue;
        /* the end of the run: where the branch goes to, or the EXIT at its end, if it falls there with no flow between */
        u32 end;
        if (go->op == SM86_BRA) {
            end = sm86_branch_target(go, at);
            if (end <= at + 1 || end > l->n || !straight(l, targeted, at + 1, end)) continue;
        } else {
            end = at + 1;
            while (end < l->n && !is_flow_86(&l->in[end]) && (end == at + 1 || !targeted[end])) end++;
            if (end >= l->n || l->in[end].op != SM86_EXIT || !sm86_always(&l->in[end]) || l->idle[end]) continue;
        }
        ShMatchRun* r = &run[(*runs)++];
        r->from = l->at[at + 1], r->to = l->at[end];
        r->own_dst = result >= SM86_RZ ? NOVAR : variable(l, result);
        own[l->at[j]] = 1, leave[l->at[at]] = 1;
        for (u32 x = at + 1; x < end; x++) {
            u32 d, rr;
            if (writes_var(l, x, address) || writes_var(l, x, address + 1)) break;
            if (add_32(&l->in[x], &d, &rr) && same_place(&l->in[x], &l->in[j])) same[l->at[x]] = 1;
        }
    }
    free(targeted);
}

/* the pseudo program's notes, their instruction the pseudo program's, its run's predicate read too */
static int pseudo_notes(Lone* l, const ShNotes* notes, ShNotes* out) {
    for (u32 q = 0; q < notes->n; q++) {
        const u32 i = (notes->word[q] >> 10) - 1, v = notes->word[q] >> 1 & 0x1ffu, write = notes->word[q] & 1;
        if (notes->word[q] >> 10 == 0 || i >= l->n) continue;
        if (l->idle[i]) continue; /* the branch its run's predicate is: nothing else */
        out->at = l->at[i];
        shader_note(out, variable(l, v), write);
        /* a run's instructions read its predicate where they read nothing of it: once, with the first note */
    }
    for (u32 i = 0; i < l->n; i++)
        if (l->run_pred[i] != 0xff) {
            out->at = l->at[i];
            shader_note(out, VAR_PRED + l->pred[l->run_pred[i]], 0);
        }
    return !out->full;
}

int sm86_lone_gives(const Sm86Insn* in, u32 n, const ShNotes* notes, const ShaderInfo* si, char* why, size_t size) {
    Lone l = {.in = in, .n = n};
    ShNotes out = {0};
    int good = 0;
    const Op** op = NULL;
    Op* ops = NULL;
    u64* insn = NULL;
    u32* where = NULL;
    u8 *same = NULL, *own = NULL, *leave = NULL;
    ShMatchRun run[16];
    u32 nruns = 0;
    snprintf(why, size, "subgroup ballot/shuffle unavailable: out of memory");
    l.run_pred = calloc(n + 1, 1);
    l.run_not = calloc(n + 1, 1);
    l.idle = calloc(n + 1, 1);
    l.at = calloc(n + 1, sizeof *l.at);
    if (l.run_pred) memset(l.run_pred, 0xff, n);
    u32 k = 11;
    for (u32 i = 0; l.at && i < n; i++) {
        if ((k - 10) % 4 == 0) k++;
        l.at[i] = k++;
    }
    l.total = k;
    if (!l.run_pred || !l.run_not || !l.idle || !l.at || notes->full || l.total > DIRECT_WORDS_MAX) goto done;
    if (!index_notes(&l, notes)) goto done;
    if (!remap(&l, notes)) {
        snprintf(why, size, "subgroup ballot/shuffle unavailable: too many registers");
        goto done;
    }
    runs(&l);
    insn = calloc(l.total + 1, sizeof *insn);
    ops = calloc(l.total + 1, sizeof *ops);
    op = calloc(l.total + 1, sizeof *op);
    where = calloc(l.total + 1, sizeof *where);
    same = calloc(l.total + 1, 1);
    own = calloc(l.total + 1, 1);
    leave = calloc(l.total + 1, 1);
    if (!insn || !ops || !op || !where || !same || !own || !leave) goto done;
    match_runs(&l, same, own, leave, run, &nruns);
    for (u32 i = 0; i < n; i++) {
        const u32 j = l.at[i];
        u64 w = 0, bits;
        int id = pseudo(&l, i, &w);
        if (l.idle[i]) id = O_NOP, w = 0x70000ull;
        if (!guard_bits(&l, i, &bits)) {
            snprintf(why, size, "subgroup ballot/shuffle unavailable: %s at %x: an unknown predicate",
                     sm86_ops[in[i].op].name, i * 16);
            goto done;
        }
        if (!l.idle[i]) w = (w & ~(0xfull << 16)) | bits;
        if (id == O_BRA) {
            const u32 t = sm86_branch_target(&in[i], i);
            if (t > n) {
                snprintf(why, size, "subgroup ballot/shuffle unavailable: %s at %x: a branch out of the program",
                         sm86_ops[in[i].op].name, i * 16);
                goto done;
            }
            const u32 to = t == n ? l.total : l.at[t];
            if (sm86_field(&in[i], 87, 3) != SM86_PT || sm86_field(&in[i], 90, 1)) id = O_BRX; /* two predicates */
            w |= (u64)((u32)((int)(to - j - 1) * 8) & 0xffffffu) << 20;
            if (t == n) id = O_BRX;
        }
        if (id == O_EXIT && (sm86_field(&in[i], 87, 3) != SM86_PT || sm86_field(&in[i], 90, 1))) id = O_BRX;
        ops[j] = (Op){id, sm86_ops[in[i].op].name};
        op[j] = &ops[j];
        insn[j] = w;
        where[j] = i * 16;
    }
    if (!pseudo_notes(&l, notes, &out)) goto done;
    const ShFlow flow = {.insn = insn, .n = l.total, .op = op};
    const ShLanePlan plan = {.where = where, .same = same, .own = own, .leave = leave, .run = run, .runs = nruns};
    good = shader_lone_lane_gives_pseudo(&flow, &out, si, &plan, why, size);
done:
    free(out.word);
    free(l.run_pred);
    free(l.run_not);
    free(l.idle);
    free(l.at);
    free(insn);
    free(ops);
    free(op);
    free(where);
    free(same);
    free(own);
    free(leave);
    free(l.note_at);
    free(l.note);
    return good;
}
