/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A compute program's use of its warp's other lanes (VOTE, SHFL, SR_LANEID and the lane masks) on a device whose compute
   subgroups are narrower than the console's warp of 32: 16 lanes on a Mali, 8 on lavapipe. Each invocation then runs as a
   warp of its own, lane 0 (translate.h's lane_model): its ballot is 1, SHFL keeps its value. That gives what the console
   gives only for some uses, so a program is translated so where every one of them is among these, and refused with the
   instruction that is not:

   - A combined add. m = VOTE.ALL(PT), L = FLO(m), n = POPC(m); where L is the lane, an atomic adds n, or n times a value
     the whole warp shares; its old value goes to every lane by SHFL.IDX from L, and the lane's own is that plus
     POPC(m & SR_LTMASK). For any set of active lanes the atomic's memory ends n higher and the lanes' values are old,
     old + 1, ... old + n - 1, which is what each lane's own add of 1 gives in lane order.
   - A combined minimum or maximum. v = IMNMX(v, SHFL.BFLY(v, s)) for s = 1, 2, 4, 8 and 16, in any order, and lane 0
     alone does the atomic of the same kind with it. The warp's minimum is one of its lanes' and an atomic minimum
     of all of them ends the same, however they group. The console reads all 32 lanes, so all must be running: the chain
     is in the program's first block, with blocks of whole warps, or behind the next one.
   - A test that the whole warp runs: m = VOTE.ALL(PT), P = m == 0xffffffff; where it does not, each lane's own atomic
     minimum or maximum, and a branch past the path of the whole warp, which is the reduction above of the same value into
     the same memory, or lane 0's atomic of a value the whole warp shares. A lane alone is never the whole warp.

   The address of such an atomic, and what an add multiplies its count by, must be the same in every lane: made of
   constants, constant buffers, the block's id (SR_CTAID) and shared memory read at such an address, with one definition.
   (Shared memory that another lane writes meanwhile, with no barrier between, is a race on the console too.)

   What each instruction reads and writes is what the translator read and wrote for it (ShNotes), so nothing a program
   does with a lane's value is missed by a table of its own: an unknown reader is a refusal. */
#include "nx/gpu/shader/translate.h"
#include <stdio.h>

/* what an instruction's result is. PLAIN: the same for a lane alone as in the warp */
enum {
    PLAIN,
    LANE,       /* SR_LANEID */
    BELOW,      /* SR_LTMASK */
    MASK,       /* the ballot of the running lanes */
    LEADER,     /* the highest of them */
    COUNT,      /* how many */
    MASK_REV,   /* the ballot's bits reversed */
    MASK_BELOW, /* those below the lane */
    RANK,       /* how many of those */
    PART_LOW,   /* the first and second of the three XMADs of a 32-bit product by COUNT */
    PART_MRG,
    SCALED,    /* COUNT times a value the warp shares */
    OLD,       /* a combined add's old value, where the lane is the leader */
    BASE,      /* that in every lane */
    SWAPPED,   /* SHFL.BFLY of a value */
    REDUCED,   /* the IMNMX of a value and its SWAPPED, some steps of it */
    IS_LEADER, /* predicates */
    IS_OTHER,  /* the lane is not the leader */
    IS_LANE0,
    IS_FULL,
    IS_SAME, /* the lanes' addresses are the same */
    KINDS
};
static const char* const kind_name[KINDS] = {"a plain value",
                                             "SR_LANEID",
                                             "SR_LTMASK",
                                             "the ballot",
                                             "the leader's lane",
                                             "the count of lanes",
                                             "the ballot reversed",
                                             "the ballot below the lane",
                                             "the lane's rank",
                                             "part of a product by the count",
                                             "part of a product by the count",
                                             "a product by the count",
                                             "the leader's old value",
                                             "the combined add's old value",
                                             "another lane's value",
                                             "a warp's minimum or maximum",
                                             "the leader's predicate",
                                             "the other lanes' predicate",
                                             "lane 0's predicate",
                                             "the whole warp's predicate",
                                             "the lanes' addresses' predicate"};

#define NONE        0xffffffffu
#define RZ          255u
#define MAX_DEFS    32u
#define GUARDS      15u /* what a way back may know of a predicate (holds) */
#define MAX_UNIFORM 48  /* the definitions a uniform value is followed through */

typedef struct {
    const ShFlow* f;
    const u64* insn;
    u32 n;
    const u32* pop;           /* each SYNC, BRK and CONT's target, NULL where the stack is kept as the program runs */
    u32 *note_at, *note;      /* the notes of each instruction */
    u32 *from_at, *from;      /* its predecessors */
    u32* block;               /* the first instruction of its basic block */
    u32 *seen, *queue, stamp; /* by instruction and what is known there (reaching) */
    u8* kind;
    u32* vote;    /* the VOTE that its result comes from */
    u8* steps;    /* SWAPPED and REDUCED: the lanes' bits it crossed */
    u8* minmax;   /* REDUCED: IMNMX's pick and sign (minmax_of) */
    u32* root;    /* SWAPPED: the value's register; REDUCED: the first value's; a product's part: its first factor's */
    u32* version; /* SWAPPED: the value's write in its block (written_before); a product's part: the other factor */
    u32* home;    /* SWAPPED and REDUCED: the block its steps are in, NONE for more than one */
    u8* claimed;  /* lane 0's atomics that a whole warp's path accounts for */
    u8 lanes[VAR_COUNT]; /* a lane's value is written to it somewhere before */
    const ShLanePlan* plan; /* of a pseudo program (warp_notes.h), NULL for Maxwell's */
    u8* lead;            /* 1 an instruction that only the leader runs, 2 one that only the others run */
    u8* sum;             /* an addition of the combined add's old value and the lane's rank */
    char* why;
    size_t size;
} Warp;

static int refuse(Warp* a, u32 at, const char* what) {
    snprintf(a->why, a->size, "subgroup ballot/shuffle unavailable: %s at %x: %s",
             a->f->op[at] ? a->f->op[at]->name : "?", a->plan ? a->plan->where[at] : (at - 10) * 8, what);
    return 0;
}

static int plain_guard(u64 w) { return B(w, 16, 4) == 7; }
static int is_flow(int op) {
    return op == O_BRA || op == O_BRX || op == O_SYNC || op == O_BRK || op == O_CONT || op == O_EXIT || op == O_KIL;
}

/* the instructions after k, at most 64: the count, or -1 for a flow instruction the check does not follow */
static int after(const Warp* a, u32 k, u32 to[65]) {
    u64 w = a->insn[k];
    u32 next = shader_next_instruction(k, a->n);
    int count = 0;
    switch (a->f->op[k]->id) {
        case O_EXIT:
        case O_KIL:
            if (!flow_always(w) && next < a->n) to[count++] = next;
            return count;
        case O_BRA: to[count++] = bra_target(w, k); break;
        case O_SYNC:
        case O_BRK:
        case O_CONT:
            if (!a->pop) return -1;
            to[count++] = a->pop[k];
            break;
        case O_BRX:
        case O_JMP:
        case O_JMX:
        case O_CAL:
        case O_JCAL:
        case O_PRET:
        case O_RET:
        case O_PEXIT: return -1;
        default:
            if (next < a->n) to[count++] = next;
            return count;
    }
    if (to[0] >= a->n || !IDX_OK(to[0])) return -1;
    if (!flow_always(w) && next < a->n) to[count++] = next;
    return count;
}

/* the predecessors and the basic blocks */
static int flow_make(Warp* a) {
    u32 to[65], edges = 0;
    for (u32 pass = 0; pass < 2; pass++) {
        for (u32 k = 11; k < a->n; k++) {
            if (!IDX_OK(k)) continue;
            int count = after(a, k, to);
            if (count < 0) return refuse(a, k, "a lane alone follows no such flow");
            for (int t = 0; t < count; t++) {
                if (!pass)
                    a->from_at[to[t] + 1]++;
                else
                    a->from[a->from_at[to[t]]++] = k;
                edges += !pass;
            }
        }
        if (pass) break;
        for (u32 k = 0; k < a->n; k++) a->from_at[k + 1] += a->from_at[k];
        a->from = malloc((edges ? edges : 1) * sizeof *a->from);
        if (!a->from) return refuse(a, 11, "out of memory");
    }
    for (u32 k = a->n; k > 0; k--) a->from_at[k] = a->from_at[k - 1]; /* each end is the next one's start again */
    a->from_at[0] = 0;
    for (u32 k = 11, last = NONE; k < a->n; k++) {
        if (!IDX_OK(k)) continue;
        u32 from = a->from_at[k + 1] - a->from_at[k];
        int straight = last != NONE && from == 1 && a->from[a->from_at[k]] == last && !is_flow(a->f->op[last]->id);
        a->block[k] = straight ? a->block[last] : k;
        last = k;
    }
    return 1;
}

/* instruction k's result is a lane's: 1 */
static int mark(Warp* a, u32 k, u8 kind, u32 vote) {
    a->kind[k] = kind, a->vote[k] = vote;
    for (u32 q = a->note_at[k]; q < a->note_at[k + 1]; q++)
        if (a->note[q] & 1) a->lanes[a->note[q] >> 1] = 1;
    return 1;
}
static int writes(const Warp* a, u32 k, u32 variable) {
    for (u32 q = a->note_at[k]; q < a->note_at[k + 1]; q++)
        if (a->note[q] == (variable << 1 | 1)) return 1;
    return 0;
}
static int r2p(int op) { return op == O_R2P_R || op == O_R2P_C || op == O_R2P_I; }
/* what a guard says of its predicate's value where it holds: 0 for PT, else 1 + twice the predicate, + 1 for true */
static u32 holds(u64 w) { return B(w, 16, 3) == 7 ? 0 : 1 + 2 * B(w, 16, 3) + !B(w, 19, 1); }
static void back(Warp* a, u32 at, u32 known, u32* tail) {
    for (u32 q = a->from_at[at]; q < a->from_at[at + 1]; q++) {
        u32 state = a->from[q] * GUARDS + known;
        if (a->seen[state] != a->stamp) a->seen[state] = a->stamp, a->queue[(*tail)++] = state;
    }
}
/* the instructions whose write of variable may be what instruction at reads, into defs: their count, MAX_DEFS + 1 for more
   than fit; *entry set where the program's start is among them. A lane's way is followed back with what one guard on it
   says of a predicate, until the predicate is written: so a write under P and one under !P leave nothing from before
   them, and one under !P is not a lane's that reads under P */
static u32 reaching(Warp* a, u32 at, u32 variable, u32 defs[MAX_DEFS], int* entry) {
    u32 count = 0, head = 0, tail = 0;
    *entry = at == 11;
    a->stamp++;
    back(a, at, holds(a->insn[at]), &tail);
    while (head < tail) {
        u32 k = a->queue[head] / GUARDS, known = a->queue[head] % GUARDS;
        head++;
        if (known && writes(a, k, VAR_PRED + (known - 1) / 2)) known = 0;
        if (writes(a, k, variable) && B(a->insn[k], 16, 4) != 15) { /* not under !PT */
            u32 under = holds(a->insn[k]), same = known && under && (known - 1) / 2 == (under - 1) / 2, seen = 0;
            if (!same || known == under) {
                for (u32 d = 0; d < count; d++) seen |= defs[d] == k;
                if (!seen && count == MAX_DEFS) return MAX_DEFS + 1;
                if (!seen) defs[count++] = k;
                if (!r2p(a->f->op[k]->id) && (!under || same)) continue;
                if (!known && !r2p(a->f->op[k]->id)) known = ((under - 1) ^ 1) + 1;
            }
        }
        if (k == 11) *entry = 1;
        back(a, k, known, &tail);
    }
    return count;
}
/* the last instruction before at in its basic block that writes variable, or the block's own start (| 1 << 31) */
static u32 written_before(const Warp* a, u32 at, u32 variable) {
    for (u32 k = at; k-- > a->block[at];)
        if (IDX_OK(k) && writes(a, k, variable)) return k;
    return a->block[at] | 1u << 31;
}

/* the one instruction a value comes from on every way, or NONE: one that keeps what reached it is never one */
static u32 only_def(Warp* a, u32 at, u32 variable) {
    u32 defs[MAX_DEFS];
    int entry;
    return reaching(a, at, variable, defs, &entry) == 1 && !entry ? defs[0] : NONE;
}

/* the kind of variable at instruction at, *def the instruction it comes from: PLAIN where no lane's value reaches it,
   -1 where one does among others */
static int kind_of(Warp* a, u32 at, u32 variable, u32* def) {
    u32 defs[MAX_DEFS];
    int entry, lanes = 0;
    *def = 0;
    if (!a->lanes[variable]) return PLAIN;
    u32 count = reaching(a, at, variable, defs, &entry);
    if (count > MAX_DEFS) return -1;
    for (u32 d = 0; d < count; d++) lanes += a->kind[defs[d]] != PLAIN;
    if (!lanes) return PLAIN;
    if (count != 1 || entry) return -1;
    *def = defs[0];
    return a->kind[defs[0]];
}
static int reg_kind(Warp* a, u32 at, u32 r, u32* def) {
    *def = 0;
    return r == RZ ? PLAIN : kind_of(a, at, r, def);
}
/* its guard's: PLAIN for PT */
static int guard_kind(Warp* a, u32 at, u32* def) {
    u32 p = B(a->insn[at], 16, 3);
    *def = 0;
    return p == 7 ? PLAIN : kind_of(a, at, VAR_PRED + p, def);
}

/* whether variable is the same in every lane at instruction at: one definition on every way, of such values alone, which
   no lane's are (VOTE, SHFL and the S2Rs of a lane are none of these instructions) */
static int uniform(Warp* a, u32 at, u32 variable, int depth) {
    u32 d = depth < MAX_UNIFORM ? only_def(a, at, variable) : NONE;
    if (d == NONE) return 0;
    u64 w = a->insn[d];
    int op = a->f->op[d]->id;
    if (op == O_S2R) return B(w, 20, 8) >= 0x25 && B(w, 20, 8) <= 0x27;
    if (op != O_LDC && op != O_LDS && (op == O_CS2R || !shader_pure_op(op))) return 0;
    for (u32 q = a->note_at[d]; q < a->note_at[d + 1]; q++) {
        /* what only the leader runs: its predicate is none of the value's */
        if (a->lead && a->lead[d] && a->note[q] >> 1 == VAR_PRED + B(w, 16, 3)) continue;
        if (!(a->note[q] & 1) && !uniform(a, d, a->note[q] >> 1, depth + 1)) return 0;
    }
    return 1;
}

static int reg_uniform(Warp* a, u32 at, u32 r) { return r == RZ || uniform(a, at, r, 0); }
/* whether the two ballots that instructions x and y come from count the same lanes: one ballot, or in a pseudo program
   (a plan), two of one basic block, whose lanes are the same from its start to its end */
static int same_vote(const Warp* a, u32 x, u32 y) {
    u32 p = a->vote[x], q = a->vote[y];
    return p == q || (a->plan && a->block[p] == a->block[q]);
}

/* IMNMX's pick (bit 0: the maximum) and sign (bit 1), and the atomic opcode of each: OpAtomicUMin, UMax, SMin, SMax */
static u8 minmax_of(u64 w) { return (u8)(B(w, 42, 1) | B(w, 48, 1) << 1); }
static u32 minmax_atomic(u8 minmax) { return (minmax & 1 ? 239u : 237u) - (minmax >> 1); }

/* a 32-bit atomic of the global or the shared memory: its SPIR-V opcode, 0 for another instruction; the registers of its
   data and result (255: none) */
static u32 atomic_of(u64 w, int op, u32* data, u32* result) {
    *data = op == O_RED ? B(w, 0, 8) : B(w, 20, 8);
    *result = op == O_RED ? RZ : B(w, 0, 8);
    if (op == O_ATOMS) return shader_shared_atomic_opcode(w);
    if (op == O_ATOM || op == O_RED) return shader_global_atomic_opcode(w, op == O_RED);
    return 0;
}
/* whether what a guarded atomic reads besides its guard and data, its address, is the same in every lane */
static int address_uniform(Warp* a, u32 k, u32 data) {
    u32 guard = VAR_PRED + B(a->insn[k], 16, 3), address = B(a->insn[k], 8, 8);
    for (u32 q = a->note_at[k]; q < a->note_at[k + 1]; q++) {
        u32 v = a->note[q] >> 1;
        if (!(a->note[q] & 1) && v != guard && v != data && !uniform(a, k, v, 0)) return 0;
    }
    return address != data || reg_uniform(a, k, address);
}

/* the fixed bits of the forms below: each register field out of the way */
#define REGS_DB         0x0ff000ffull
#define REGS_DAB        0x0ff0ffffull
#define REGS_DABC       (REGS_DAB | 0xffull << 39)
#define ISETP_OPERANDS  (0xff38ull | 0xffull << 20 | 1ull << 48)
#define ISETP_IMMEDIATE (0xff38ull | 1ull << 48)
#define VOTE_ALL        0x50d8e38000070000ull /* VOTE Rd, PT, PT; the predicate's mode (48-49) aside */
#define FLO_U32         0x5c30000000070000ull
#define POPC_R          0x5c08000000070000ull
#define LOP_AND         0x5c47000000070000ull
#define IADD_R          0x5c10000000070000ull
#define ISETP_EQ        0x5b64038000070007ull /* .U32 or, with bit 48, .S32; the predicate (3-5) aside */
#define ISETP_FULL      0x376403fffff70007ull /* == 0xffffffff */
#define ISETP_ZERO      0x3664038000070007ull /* == 0 */
#define IMNMX_R         0x5c20038000070000ull /* the pick (42) and the sign (48) aside */
#define SHFL_IDX        0xef17007c20070000ull /* Rd, PT, Ra, Rb, 0x1f */
#define SHFL_BFLY       0xef17007cf0070000ull /* the lane's bits (20-24) aside */
#define XMAD_LOW        0x5b007f8000070000ull /* XMAD Rd, Ra, Rb, RZ */
#define XMAD_MRG        0x5b007fa800070000ull /* XMAD.MRG Rd, Ra, Rb.H1, RZ */
#define XMAD_HIGH       0x5b30001800070000ull /* XMAD.PSL.CBCC Rd, Ra.H1, Rb.H1, Rc */

/* whether instruction k reads no lane's value, its leader's predicate aside: 1, or the refusal */
static int plain_use(Warp* a, u32 k) {
    for (u32 q = a->note_at[k]; q < a->note_at[k + 1]; q++) {
        u32 d;
        if (a->lead && a->lead[k] && a->note[q] >> 1 == VAR_PRED + B(a->insn[k], 16, 3)) continue;
        int kind = a->note[q] & 1 ? PLAIN : kind_of(a, k, a->note[q] >> 1, &d);
        if (kind != PLAIN) return refuse(a, k, kind < 0 ? "a lane's value among others" : kind_name[kind]);
    }
    return 1;
}
/* a guarded atomic: the leader's add, lane 0's minimum or maximum (whole_warp and settle look at the rest of those), a
   lane's own where the warp is not whole, or one no lane decides */
static int guarded_atomic(Warp* a, u32 k, u32 atomic, u32 data, u32 result) {
    u32 dg, dd;
    int kg = guard_kind(a, k, &dg), kd = reg_kind(a, k, data, &dd), negated = B(a->insn[k], 19, 1);
    if (kg == IS_SAME) /* the lane's own atomic where the warp's addresses differ: a lone lane's never do */
        return negated && a->plan->own[k] ? 1 : refuse(a, k, "not its own's");
    if (kg != IS_LEADER && kg != IS_OTHER && kg != IS_LANE0 && kg != IS_FULL) return plain_use(a, k);
    /* the leader's: its own predicate true, or the other lanes' false */
    const int leader = kg == IS_LEADER || kg == IS_OTHER;
    if (negated != (kg == IS_FULL || kg == IS_OTHER)) return refuse(a, k, "under a lane's predicate, negated");
    if (kd < 0) return refuse(a, k, "a lane's value among others");
    if (!(a->plan && a->plan->same[k]) && !address_uniform(a, k, data))
        return refuse(a, k, "its address is not the warp's");
    if (leader) {
        /* the warp's minimum or maximum of one value, whatever it is, is what every lane's own gives */
        if (atomic >= 236 && atomic <= 239 && kd == PLAIN && result == RZ && reg_uniform(a, k, data)) return 1;
        if (atomic != 234 || (kd != COUNT && kd != SCALED) || !same_vote(a, dd, dg))
            return refuse(a, k, "not the leader's add of the count");
        if (a->block[a->vote[dg]] != a->block[k]) return refuse(a, k, "not in its ballot's block");
        if (result == RZ) return 1;
        return kd == COUNT ? mark(a, k, OLD, a->vote[dg]) : refuse(a, k, "a scaled add's old value");
    }
    if (atomic < 236 || atomic > 239 || result != RZ) return refuse(a, k, "not a minimum or maximum");
    if (kg == IS_FULL) return kd == PLAIN ? 1 : refuse(a, k, kind_name[kd]);
    if (kd == REDUCED && (a->steps[dd] != 31 || minmax_atomic(a->minmax[dd]) != atomic))
        return refuse(a, k, "not the whole reduction's kind");
    return 1;
}
/* IMNMX k of a value and its other half: one more step of a reduction, 0 for neither it nor a refusal */
static int reduction(Warp* a, u32 k, u32 ra, u32 rb) {
    u32 da, db;
    int ka = reg_kind(a, k, ra, &da), kb = reg_kind(a, k, rb, &db);
    if (ka != SWAPPED && kb != SWAPPED) return 0;
    u32 s = ka == SWAPPED ? da : db, r = ka == SWAPPED ? db : da, v = ka == SWAPPED ? rb : ra;
    int kv = ka == SWAPPED ? kb : ka;
    u8 minmax = minmax_of(a->insn[k]), before = kv == REDUCED ? a->steps[r] : 0;
    /* the value is the one that was swapped: its register, not written since the block began or since the same write. A
       step taken again reduces the same lanes again, which changes nothing */
    if ((kv != PLAIN && kv != REDUCED) || a->root[s] != v || a->version[s] != written_before(a, k, v) ||
        (kv == REDUCED && a->minmax[r] != minmax))
        return 0;
    a->steps[k] = before | a->steps[s], a->minmax[k] = minmax;
    a->root[k] = kv == REDUCED ? a->root[r] : v;
    a->home[k] = kv == REDUCED && a->home[r] != a->block[k] ? NONE : a->block[k];
    return mark(a, k, REDUCED, 0);
}
/* XMAD k, the first or the second of a 32-bit product of the count and a value the warp shares */
static int product_part(Warp* a, u32 k, u8 part, u32 ra, u32 rb) {
    u32 da, db;
    int ka = reg_kind(a, k, ra, &da), kb = reg_kind(a, k, rb, &db);
    if (!(ka == COUNT && kb == PLAIN && reg_uniform(a, k, rb)) &&
        !(kb == COUNT && ka == PLAIN && reg_uniform(a, k, ra)))
        return 0;
    a->root[k] = ra, a->version[k] = rb;
    return mark(a, k, part, a->vote[ka == COUNT ? da : db]);
}
/* XMAD k, the last of it: its parts' two factors are the same and unwritten since, and Ra is the first one */
static int product(Warp* a, u32 k, u32 ra, u32 rb, u32 rc) {
    u32 low, mrg;
    if (reg_kind(a, k, rb, &mrg) != PART_MRG || reg_kind(a, k, rc, &low) != PART_LOW) return 0;
    u32 other = a->version[low];
    if (a->root[low] != ra || a->root[mrg] != ra || a->version[mrg] != other || a->block[low] != a->block[k] ||
        a->block[mrg] != a->block[k] || written_before(a, low, ra) != written_before(a, k, ra) ||
        written_before(a, mrg, ra) != written_before(a, k, ra) ||
        written_before(a, low, other) != written_before(a, mrg, other))
        return 0;
    return mark(a, k, SCALED, a->vote[low]);
}
/* any other instruction, which reads a lane's value through its operands alone: 1 for a use the proofs cover, or none */
static int use(Warp* a, u32 k) {
    u64 w = a->insn[k];
    int op = a->f->op[k]->id;
    u32 ra = B(w, 8, 8), rb = B(w, 20, 8), da, db, data, result, atomic = atomic_of(w, op, &data, &result);
    if (atomic && !plain_guard(w)) return guarded_atomic(a, k, atomic, data, result);
    if (is_flow(op) && guard_kind(a, k, &da) == IS_FULL && B(w, 19, 1)) return 1; /* whole_warp */
    if (a->plan && is_flow(op) && guard_kind(a, k, &da) == IS_SAME) /* out of the run that only they take (a match) */
        return a->plan->leave[k] && B(w, 19, 1) ? 1 : refuse(a, k, "the lanes' addresses");
    if (a->plan && !is_flow(op)) {
        int kg = guard_kind(a, k, &da), negated = B(w, 19, 1);
        if (kg == IS_LEADER || kg == IS_OTHER) {
            /* what only the leader runs, or only the others, in a warp: a lone lane is the leader, so the others' run
               in none, and neither may do what the other lanes would see */
            a->lead[k] = (kg == IS_LEADER) != negated ? 1 : 2;
            if (op != O_S2R && op != O_LDC && op != O_LDS && op != O_NOP && !shader_pure_op(op) && op < LO_BREV)
                return refuse(a, k, "only some lanes run it");
            w = (w & ~(0xfull << 16)) | 7ull << 16; /* the forms below are of an instruction that always runs */
        }
    }
    if ((w & ~REGS_DB) == FLO_U32 && reg_kind(a, k, rb, &db) == MASK) return mark(a, k, LEADER, a->vote[db]);
    if ((w & ~REGS_DB) == POPC_R) {
        int kb = reg_kind(a, k, rb, &db);
        if (kb == MASK || kb == MASK_BELOW) return mark(a, k, kb == MASK ? COUNT : RANK, a->vote[db]);
    }
    switch (op) {
        case LO_BREV:
            if (reg_kind(a, k, rb, &db) == MASK) return mark(a, k, MASK_REV, a->vote[db]);
            return plain_use(a, k);
        case LO_FLO_SH:
            if (reg_kind(a, k, rb, &db) == MASK_REV) return mark(a, k, LEADER, a->vote[db]);
            return plain_use(a, k);
        case LO_ISETP_NE: {
            int ka = reg_kind(a, k, ra, &da), kb = reg_kind(a, k, rb, &db);
            if ((ka == LEADER && kb == LANE) || (ka == LANE && kb == LEADER))
                return mark(a, k, IS_OTHER, a->vote[ka == LEADER ? da : db]);
            return plain_use(a, k);
        }
    }
    if ((w & ~REGS_DAB) == LOP_AND) {
        int ka = reg_kind(a, k, ra, &da), kb = reg_kind(a, k, rb, &db);
        if ((ka == MASK && kb == BELOW) || (ka == BELOW && kb == MASK))
            return mark(a, k, MASK_BELOW, a->vote[ka == MASK ? da : db]);
    }
    if ((w & ~ISETP_OPERANDS) == ISETP_EQ) {
        int ka = reg_kind(a, k, ra, &da), kb = reg_kind(a, k, rb, &db);
        if ((ka == LEADER && kb == LANE) || (ka == LANE && kb == LEADER))
            return mark(a, k, IS_LEADER, a->vote[ka == LEADER ? da : db]);
        if ((ka == LANE && rb == RZ) || (kb == LANE && ra == RZ)) return mark(a, k, IS_LANE0, 0);
    }
    if ((w & ~ISETP_IMMEDIATE) == ISETP_FULL && reg_kind(a, k, ra, &da) == MASK)
        return mark(a, k, IS_FULL, a->vote[da]);
    if ((w & ~ISETP_IMMEDIATE) == ISETP_ZERO && reg_kind(a, k, ra, &da) == LANE) return mark(a, k, IS_LANE0, 0);
    if ((w & ~(REGS_DAB | 1ull << 42 | 1ull << 48)) == IMNMX_R && reduction(a, k, ra, rb)) return 1;
    if ((w & ~REGS_DAB) == IADD_R) {
        int ka = reg_kind(a, k, ra, &da), kb = reg_kind(a, k, rb, &db);
        if (((ka == BASE && kb == RANK) || (ka == RANK && kb == BASE)) && same_vote(a, da, db)) {
            if (a->sum) a->sum[k] = 1;
            return 1;
        }
    }
    if ((w & ~REGS_DAB) == XMAD_LOW && product_part(a, k, PART_LOW, ra, rb)) return 1;
    if ((w & ~REGS_DAB) == XMAD_MRG && product_part(a, k, PART_MRG, ra, rb)) return 1;
    if ((w & ~REGS_DABC) == XMAD_HIGH && product(a, k, ra, rb, B(w, 39, 8))) return 1;
    return plain_use(a, k);
}

/* VOTE, SHFL and S2R of the lane, which read the other lanes themselves, and any other instruction */
static int instruction(Warp* a, u32 k) {
    u64 w = a->insn[k];
    int op = a->f->op[k]->id;
    u32 ra = B(w, 8, 8), rb = B(w, 20, 8), d, bit = B(w, 20, 5), sr = B(w, 20, 8);
    if (op == O_S2R && (sr == 0 || (sr >= 0x38 && sr <= 0x3c))) {
        if (sr != 0 && sr != 0x39) return refuse(a, k, "a lane mask");
        return mark(a, k, sr ? BELOW : LANE, 0);
    }
    if (op == O_VOTE) {
        if ((w & ~(0xffull | 3ull << 48)) != VOTE_ALL) return refuse(a, k, "not the running lanes' ballot");
        return mark(a, k, MASK, k);
    }
    if (op == LO_MATCH) return mark(a, k, IS_SAME, k);
    if (op != O_SHFL) return use(a, k);
    if ((w & ~(0xffffull | 0x1full << 20)) == SHFL_BFLY && bit && !(bit & (bit - 1)) && ra != RZ) {
        int kind = reg_kind(a, k, ra, &d);
        if (kind != PLAIN && kind != REDUCED) return plain_use(a, k);
        a->steps[k] = (u8)bit, a->root[k] = ra, a->version[k] = written_before(a, k, ra), a->home[k] = a->block[k];
        return mark(a, k, SWAPPED, 0);
    }
    if ((w & ~REGS_DAB) == SHFL_IDX && reg_kind(a, k, rb, &d) == LEADER && ra != RZ) {
        /* its value is the leader's: the combined add's result, the last write of it in the block, which is the ballot's
           too. What else may be in the register is the other lanes', which nothing reads */
        u32 last = written_before(a, k, ra);
        if (last < a->n && a->kind[last] == OLD && same_vote(a, last, d)) return mark(a, k, BASE, a->vote[d]);
    }
    return refuse(a, k, "no combined add's, no reduction's");
}

/* whether instruction k writes what the guarded atomic at own reads, its guard aside */
static int writes_operand(const Warp* a, u32 k, u32 own) {
    u32 guard = VAR_PRED + B(a->insn[own], 16, 3);
    for (u32 q = a->note_at[own]; q < a->note_at[own + 1]; q++)
        if (!(a->note[q] & 1) && a->note[q] >> 1 != guard && writes(a, k, a->note[q] >> 1)) return 1;
    return 0;
}
static u32 next_of(const Warp* a, u32 k) { return shader_next_instruction(k, a->n); }
static int idle(const Warp* a, u32 k) { return a->f->op[k]->id == O_NOP || a->f->op[k]->id == O_DEPBAR; }

/* what reads the predicate of the test at k: the lane's own atomic and the branch past the whole warp's path, which ends
   its block */
static int test_uses(Warp* a, u32 k, u32* own, u32* leave) {
    u32 p = VAR_PRED + B(a->insn[k], 3, 3), defs[MAX_DEFS], data, result;
    int entry;
    *own = *leave = NONE;
    for (u32 j = 11; j < a->n; j++) {
        u32 reads = 0, mine = 0;
        if (!IDX_OK(j)) continue;
        for (u32 q = a->note_at[j]; q < a->note_at[j + 1]; q++) reads |= a->note[q] == p << 1;
        u32 count = reads ? reaching(a, j, p, defs, &entry) : 0;
        if (count > MAX_DEFS) return refuse(a, j, "too many writes of its predicate");
        for (u32 d = 0; d < count; d++) mine |= defs[d] == k;
        if (!mine) continue;
        int op = a->f->op[j]->id;
        u32* slot = is_flow(op) ? leave : atomic_of(a->insn[j], op, &data, &result) ? own : NULL;
        if (!slot || *slot != NONE) return refuse(a, j, kind_name[IS_FULL]);
        *slot = j;
    }
    if (*own == NONE || *leave == NONE || a->block[*own] != a->block[k] || a->block[*leave] != a->block[k] ||
        a->block[a->vote[k]] != a->block[k])
        return refuse(a, k, "not before each lane's own atomic");
    return 1;
}
/* the whole warp's path behind the test at k is a reduction of the same value into the same memory, and nothing else */
static int whole_warp(Warp* a, u32 k) {
    u32 own, leave, data, result, fdata, fresult, x;
    if (!test_uses(a, k, &own, &leave)) return 0;
    u64 w = a->insn[own], lw = a->insn[leave];
    int out = a->f->op[leave]->id;
    atomic_of(w, a->f->op[own]->id, &data, &result);
    if (shader_flow_has_cc(out) && B(lw, 0, 5) != 15) return refuse(a, leave, "on the flags too");
    for (u32 j = next_of(a, own); j < leave; j = next_of(a, j))
        if (writes_operand(a, j, own)) return refuse(a, j, "it writes what the atomic reads");
    u32 first = next_of(a, leave), full = first;
    for (;; full = next_of(a, full)) { /* to lane 0's atomic. What it writes is a lane's, which no address is made of */
        if (full >= a->n || a->block[full] != a->block[first]) return refuse(a, leave, "no reduction behind the test");
        if (atomic_of(a->insn[full], a->f->op[full]->id, &fdata, &fresult)) break;
        u8 kind = a->kind[full];
        if (!idle(a, full) && kind != LANE && kind != IS_LANE0 && kind != SWAPPED && kind != REDUCED)
            return refuse(a, full, "more than a reduction");
    }
    u64 fw = a->insn[full], same = ~(0xfull << 16 | (a->f->op[own]->id == O_RED ? 0xffull : 0xffull << 20));
    if (guard_kind(a, full, &x) != IS_LANE0 || B(fw, 19, 1) || (fw & same) != (w & same))
        return refuse(a, full, "not lane 0's, of the same memory");
    int kind = reg_kind(a, full, fdata, &x);
    /* the lane's own value, which nothing wrote since: the first of a chain in this block, or the warp's one */
    if (kind == REDUCED ? a->root[x] != data || a->home[x] != first
                        : kind != PLAIN || fdata != data || !reg_uniform(a, own, data))
        return refuse(a, full, "not the same value's");
    a->claimed[full] = 1;
    u32 end = next_of(a, full);
    while (end < a->n && idle(a, end) && a->block[end] == first) end = next_of(a, end);
    if (out == O_BRA ? end == bra_target(lw, leave)
                     : end < a->n && a->f->op[end]->id == out && flow_always(a->insn[end]) &&
                           (out == O_EXIT || (out != O_KIL && out != O_BRX && a->pop[end] == a->pop[leave])))
        return 1;
    return refuse(a, full, "the whole warp's path goes on");
}

/* after the kinds: the tests of a whole warp, the reductions that no test is before, and a lane's value that comes round
   a loop to an instruction before the one that made it */
static int settle(Warp* a, const ShaderInfo* si) {
    u32 defs[MAX_DEFS], data, result, d;
    int entry;
    for (u32 k = 11; k < a->n; k++)
        if (IDX_OK(k) && a->kind[k] == IS_FULL && !whole_warp(a, k)) return 0;
    for (u32 k = 11; k < a->n; k++) {
        if (!IDX_OK(k)) continue;
        u64 w = a->insn[k];
        for (u32 q = a->note_at[k]; q < a->note_at[k + 1]; q++) {
            u32 v = a->note[q] >> 1;
            if ((a->note[q] & 1) || !a->lanes[v] || (a->kind[k] == BASE && v == B(w, 8, 8))) continue;
            u32 count = reaching(a, k, v, defs, &entry);
            if (count > MAX_DEFS) return refuse(a, k, "too many writes of what it reads");
            for (u32 j = 0; j < count; j++)
                if (defs[j] >= k && a->kind[defs[j]] != PLAIN) return refuse(a, k, "a lane's value round a loop");
        }
        if (plain_guard(w) || !atomic_of(w, a->f->op[k]->id, &data, &result) || a->claimed[k] ||
            guard_kind(a, k, &d) != IS_LANE0)
            continue;
        /* every lane of the block's whole warps runs the program's first block */
        int64_t lanes = (int64_t)si->cs_local[0] * si->cs_local[1] * si->cs_local[2];
        if (reg_kind(a, k, data, &d) != REDUCED || a->block[k] != 11 || lanes <= 0 || lanes % 32)
            return refuse(a, k, "maybe not a whole warp's");
    }
    return 1;
}

/* in a pseudo program: the runs of the lanes whose addresses are the same, which a lone lane always takes: they store
   nothing but the leader's atomics of the compared address, go on from their end, and nothing they write is read after
   it, but each lane's own value of own_dst, which the leader's old value and its rank come to in a lone lane */
static int matches(Warp* a) {
    u32 defs[MAX_DEFS];
    int entry;
    for (u32 m = 0; m < a->plan->runs; m++) {
        const ShMatchRun* r = &a->plan->run[m];
        for (u32 k = r->from; k < r->to && k < a->n; k++) {
            if (!IDX_OK(k)) continue;
            const int id = a->f->op[k]->id, flow = is_flow(id);
            if (flow) return refuse(a, k, "a branch in the lanes' own");
            u32 data, result;
            int atomic = atomic_of(a->insn[k], id, &data, &result) != 0;
            if (atomic ? !(a->plan->same[k] && guard_kind(a, k, &data) != PLAIN) : id == O_STG)
                return refuse(a, k, "more than a combined add");
        }
        for (u32 k = r->to; k < a->n; k++) {
            if (!IDX_OK(k)) continue;
            for (u32 q = a->note_at[k]; q < a->note_at[k + 1]; q++) {
                if (a->note[q] & 1) continue;
                u32 v = a->note[q] >> 1, count = reaching(a, k, v, defs, &entry);
                if (count > MAX_DEFS) return refuse(a, k, "too many writes of what it reads");
                for (u32 d = 0; d < count; d++) {
                    if (defs[d] < r->from || defs[d] >= r->to) continue;
                    if (v != r->own_dst || !a->sum[defs[d]]) return refuse(a, k, "the lanes' own value");
                }
            }
        }
    }
    return 1;
}
/* in a pseudo program: what only the leader or only the others write is read by what they run alone, or through a SHFL
   from the leader, which the kinds have looked at */
static int leads(Warp* a) {
    u32 defs[MAX_DEFS];
    int entry;
    for (u32 k = 11; k < a->n; k++) {
        if (!IDX_OK(k)) continue;
        for (u32 q = a->note_at[k]; q < a->note_at[k + 1]; q++) {
            u32 v = a->note[q] >> 1;
            if ((a->note[q] & 1) || (a->kind[k] == BASE && v == B(a->insn[k], 8, 8))) continue;
            u32 count = reaching(a, k, v, defs, &entry);
            if (count > MAX_DEFS) return refuse(a, k, "too many writes of what it reads");
            for (u32 d = 0; d < count; d++)
                if (a->lead[defs[d]] && holds(a->insn[defs[d]]) != holds(a->insn[k]))
                    return refuse(a, k, "the leader's own value");
        }
    }
    return 1;
}

static int by_instruction(Warp* a, const ShNotes* notes) {
    a->note = malloc((notes->n ? notes->n : 1) * sizeof *a->note);
    if (!a->note) return 0;
    for (u32 q = 0; q < notes->n; q++)
        if (notes->word[q] >> 10 < a->n) a->note_at[(notes->word[q] >> 10) + 1]++;
    for (u32 k = 0; k < a->n; k++) a->note_at[k + 1] += a->note_at[k];
    for (u32 q = 0; q < notes->n; q++)
        if (notes->word[q] >> 10 < a->n) a->note[a->note_at[notes->word[q] >> 10]++] = notes->word[q] & 0x3ffu;
    for (u32 k = a->n; k > 0; k--) a->note_at[k] = a->note_at[k - 1];
    a->note_at[0] = 0;
    return 1;
}

void shader_note(ShNotes* notes, u32 variable, u32 write) {
    if (!notes->at) return;
    if (notes->n == notes->cap) {
        u32 cap = notes->cap ? notes->cap * 2 : 4096;
        u32* word = realloc(notes->word, cap * sizeof *word);
        if (!word) {
            notes->full = 1;
            return;
        }
        notes->word = word, notes->cap = cap;
    }
    notes->word[notes->n++] = notes->at << 10 | variable << 1 | write;
}

static int lane_gives(const ShFlow* f, const u32* pop_targets, const ShNotes* notes, const ShaderInfo* si,
                      const ShLanePlan* plan, char* why, size_t size) {
    size_t n = f->n, each = n + 1;
    Warp a = {.f = f, .insn = f->insn, .n = f->n, .pop = pop_targets, .plan = plan, .why = why, .size = size};
    snprintf(why, size, "subgroup ballot/shuffle unavailable: out of memory");
    if (notes->full || n > DIRECT_WORDS_MAX) return 0;
    /* the arrays by instruction, the words first */
    u32* words = calloc(each, (7 + 2 * GUARDS) * sizeof *words + 6);
    if (!words) return 0;
    a.note_at = words, a.from_at = words + each, a.block = words + 2 * each, a.vote = words + 3 * each;
    a.root = words + 4 * each, a.version = words + 5 * each, a.home = words + 6 * each, a.seen = words + 7 * each;
    a.queue = a.seen + GUARDS * each;
    a.kind = (u8*)(a.queue + GUARDS * each), a.steps = a.kind + each, a.minmax = a.kind + 2 * each;
    a.claimed = a.kind + 3 * each;
    a.lead = a.kind + 4 * each, a.sum = a.kind + 5 * each;
    int good = by_instruction(&a, notes) && flow_make(&a);
    for (u32 k = 11; good && k < a.n; k++)
        if (IDX_OK(k)) good = instruction(&a, k);
    good = good && settle(&a, si) && (!plan || (leads(&a) && matches(&a)));
    free(a.note);
    free(a.from);
    free(words);
    return good;
}
int shader_lone_lane_gives(const ShFlow* f, const u32* pop_targets, const ShNotes* notes, const ShaderInfo* si,
                           char* why, size_t size) {
    return lane_gives(f, pop_targets, notes, si, NULL, why, size);
}
int shader_lone_lane_gives_pseudo(const ShFlow* f, const ShNotes* notes, const ShaderInfo* si, const ShLanePlan* plan,
                                  char* why, size_t size) {
    return lane_gives(f, NULL, notes, si, plan, why, size);
}
