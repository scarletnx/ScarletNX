/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The analyses over a program's words before it is translated: flow targets, reaching definitions,
   guards, and where a texture handle, a constant or a global memory base comes from */
#include "nx/gpu/shader/translate.h"

static int direct_reaching_def_same(const u64* insn, u32 n, u32 index, u32 reg,
                                    int (*same)(const u64*, u32, u32, u32, int), int depth);
static int direct_reaching_def(const u64* insn, u32 n, u32 index, u32 reg);

/* Where control can arrive from the flow instruction at k, besides the next instruction: a branch, SSY, PBK or PCNT
   target, a jump table's entries, a CAL's callee and the return point after it, a PRET's return point. -1 when that
   can't be known (a JMP, or a jump table that isn't found). elsewhere[t] is set for arrivals that come from other
   instructions than k: a PCNT's target takes CONTs from later in the loop, and a return point takes RETs. */
static int direct_flow_targets(const u64* insn, u32 n, u32 k, u32 targets[64], u8 elsewhere[64]) {
    const Op* flow = decode(insn[k]);
    if (!flow) return 0;
    switch (flow->id) {
        case O_JMP: return -1;
        case O_BRX: {
            int count = shader_brx_targets(insn, k, n, targets, 64);
            int t;
            for (t = 0; t < count; t++) elsewhere[t] = 0;
            return count;
        }
        case O_CAL:
            targets[0] = bra_target(insn[k], k);
            targets[1] = k + 1 + ((k + 1 - 10) % 4 == 0);
            elsewhere[0] = 0;
            elsewhere[1] = 1;
            return 2;
        case O_PRET:
            targets[0] = bra_target(insn[k], k);
            elsewhere[0] = 1;
            return 1;
        case O_BRA:
        case O_SSY:
        case O_PBK:
        case O_PCNT:
            targets[0] = bra_target(insn[k], k);
            elsewhere[0] = flow->id == O_PCNT;
            return 1;
        default: return 0;
    }
}

/* Whether a flow instruction whose targets aren't known at k can be left out of a search that only cares about targets
   at or below limit: every case of a jump table lies after its BRX (shader_brx_targets), so one at or after limit
   cannot reach the search's region. A JMP takes a register's target and can go anywhere, so it never can. */
static int direct_flow_below(const u64* insn, u32 k, u32 limit) {
    const Op* op = decode(insn[k]);
    return op && op->id == O_BRX && k >= limit;
}

/* Whether control goes from the instruction at from straight on to the one at to, with nothing between that leaves */
static int direct_straight(const u64* insn, u32 from, u32 to) {
    u32 k;
    for (k = from + 1; k < to; k++) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        switch (op->id) {
            case O_BRA:
            case O_BRX:
            case O_SSY:
            case O_SYNC:
            case O_PBK:
            case O_BRK:
            case O_PCNT:
            case O_CONT:
            case O_EXIT:
            case O_KIL:
            case O_JMP:
            case O_CAL:
            case O_RET:
            case O_PRET:
            case O_PEXIT: return 0;
            default: break;
        }
    }
    return 1;
}

static int direct_def_dominates(const u64* insn, u32 n, u32 def, u32 use, u32 reg,
                                int (*same)(const u64*, u32, u32, u32, int), int depth) {
    u32 end = use;
    for (;;) {
        u32 k, grown = end;
        for (k = 11; k < n; k++) {
            u32 targets[64];
            u8 elsewhere[64];
            int count;
            int t;
            if (!IDX_OK(k)) continue;
            const Op* op = decode(insn[k]);
            if (!op) return 0;
            count = direct_flow_targets(insn, n, k, targets, elsewhere);
            if (count < 0) {
                if (direct_flow_below(insn, k, end)) continue;
                return 0;
            }
            for (t = 0; t < count; t++) {
                if (targets[t] <= def || targets[t] > end) continue;
                if (elsewhere[t]) return 0;
                if (k < def) {
                    if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(insn, k, def)) continue;
                    /* a path that leaves before def carries def's value on if reg already has it there (a loop's
                       exit branch, taken before the loop's last write of reg) */
                    if (same) {
                        int jk = direct_reaching_def_same(insn, n, k, reg, same, depth + 1);
                        if (jk >= 0 && same(insn, n, def, (u32)jk, depth + 1)) continue;
                    }
                    return 0;
                }
                if (k > grown) grown = k;
            }
        }
        if (grown == end) return 1;
        for (k = end + 1; k <= grown; k++) {
            const Op* op;
            if (!IDX_OK(k)) continue;
            op = decode(insn[k]);
            if (!op) return 0;
            if (shader_writes_reg(insn[k], op->id, reg)) {
                /* a loop that rewrites reg with the definition def has doesn't hide def's value */
                if (same && B(insn[k], 0, 8) == reg && same(insn, n, def, k, depth + 1)) continue;
                return 0;
            }
        }
        end = grown;
    }
}

/* A predicate's local definition, following a saved PSET boolean restored by
   ISETP.EQ/NE against RZ. Output predicates of a plain ISETP are complements. */
static int direct_pred_origin(const u64* insn, u32 index, u32 floor, u32 guard, u32* invert, int depth) {
    u32 pred = guard & 7;
    if (pred == 7 || depth > 8) return -1;
    *invert ^= guard >> 3;
    for (u32 j = index; j-- > floor;) {
        const Op* op;
        u64 w;
        if (!IDX_OK(j)) continue;
        w = insn[j];
        op = decode(w);
        if (!op) return -1;
        if (!(shader_direct_pred_writes(w, op->id) >> pred & 1)) continue;
        if (B(w, 16, 4) != 7 || (op->id != O_ISETP_R && op->id != O_ISETP_C && op->id != O_ISETP_I) ||
            B(w, 39, 4) != 7 || B(w, 45, 2) || B(w, 43, 1))
            return -1;
        *invert ^= B(w, 0, 3) == pred;
        if (op->id == O_ISETP_R && B(w, 20, 8) == 255 && (B(w, 49, 3) == 2 || B(w, 49, 3) == 5)) {
            u32 reg = B(w, 8, 8);
            /* The saved boolean must have an unconditional local definition.
               A predicated overwrite would leave two possible sources. */
            for (u32 k = j; k-- > floor;) {
                const Op* saved;
                u64 v;
                if (!IDX_OK(k)) continue;
                v = insn[k];
                saved = decode(v);
                if (!saved) return -1;
                if (!shader_writes_reg(v, saved->id, reg)) continue;
                if (B(v, 0, 8) != reg || B(v, 16, 4) != 7) return -1;
                if (saved->id != O_PSET || B(v, 24, 2) || B(v, 45, 2) || B(v, 29, 4) != 7 || B(v, 39, 4) != 7)
                    return (int)j;
                *invert ^= B(w, 49, 3) == 2;
                return direct_pred_origin(insn, k, floor, B(v, 12, 4), invert, depth + 1);
            }
        }
        return (int)j;
    }
    /* No local write: the predicate entering this block is a symbolic value.
       Its saved/restored copies remain equal even when its producer is outside
       the block. -1 remains unknown, -2 through -8 identify P0 through P6. */
    return -(int)pred - 2;
}

/* Two locally computed guards can have different predicate registers or have
   been saved and restored. Equal compares are equivalent only while their
   register operands retain their values. Constant buffers are read-only. */
static int direct_same_guard(const u64* insn, u32 floor, u32 definition, u32 use) {
    u32 ai = 0, bi = 0;
    int a = direct_pred_origin(insn, definition, floor, B(insn[definition], 16, 4), &ai, 0);
    int b = direct_pred_origin(insn, use, floor, B(insn[use], 16, 4), &bi, 0);
    if (a == -1 || b == -1 || ai != bi) return 0;
    if (a == b) return 1;
    if (a < 0 || b < 0) return 0;
    if ((insn[a] & ~63ull) != (insn[b] & ~63ull)) return 0;
    const Op* op = decode(insn[a]);
    u32 lhs = B(insn[a], 8, 8), rhs = op->id == O_ISETP_R ? B(insn[a], 20, 8) : 255;
    u32 lo = (u32)(a < b ? a : b), hi = (u32)(a > b ? a : b);
    for (u32 k = lo + 1; k < hi; k++) {
        if (!IDX_OK(k)) continue;
        const Op* mid = decode(insn[k]);
        if (!mid || (lhs != 255 && shader_writes_reg(insn[k], mid->id, lhs)) ||
            (rhs != 255 && shader_writes_reg(insn[k], mid->id, rhs)))
            return 0;
    }
    return 1;
}

/* Whether pred changes between lo and hi: a requirement's value at one end describes the other only while it
   doesn't. */
static int direct_pred_written(const u64* insn, u32 lo, u32 hi, u32 pred) {
    u32 k;
    for (k = lo + 1; k < hi; k++) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 1;
        if (shader_direct_pred_writes(insn[k], op->id) >> pred & 1) return 1;
    }
    return 0;
}

static int direct_mark(u8* seen, u32 use, u32 at) {
    if (at > use || (seen[at >> 3] >> (at & 7) & 1)) return 0;
    seen[at >> 3] |= (u8)(1u << (at & 7));
    return 1;
}

/* Whether control from from can reach use, by the flow instructions as far as they are known: -1 when a jump's target
   isn't (an indirect jump, a token's BRK or CONT), so the caller keeps what it asked about. The bitset is walked to a
   fixed point, which reaches through loops as well as forward edges. */
static int direct_can_reach(const u64* insn, u32 n, u32 from, u32 use) {
    u8 seen[DIRECT_WORDS_MAX / 8];
    u32 k;
    int changed = 1;
    if (from > use) return 0;
    if (use >= DIRECT_WORDS_MAX) return -1;
    memset(seen, 0, sizeof seen);
    seen[from >> 3] |= (u8)(1u << (from & 7));
    while (changed) {
        changed = 0;
        for (k = from; k <= use; k++) {
            const Op* op;
            u32 targets[64];
            u8 elsewhere[64];
            int count;
            int t;
            int fall = 1;
            if (!(seen[k >> 3] >> (k & 7) & 1)) continue;
            if (!IDX_OK(k)) {
                changed |= direct_mark(seen, use, k + 1);
                continue;
            }
            op = decode(insn[k]);
            if (!op) return -1;
            switch (op->id) {
                /* Lanes that do not take a conditional exit keep their register definitions. */
                case O_EXIT: fall = !flow_always(insn[k]); break;
                case O_KIL:
                case O_RET:
                case O_PRET:
                case O_PEXIT: fall = 0; break;
                case O_BRK:
                case O_CONT:
                case O_JMP: return -1;
                case O_BRA: fall = B(insn[k], 16, 4) != 7; break;
                case O_BRX: fall = 0; break;
                default: break;
            }
            count = direct_flow_targets(insn, n, k, targets, elsewhere);
            if (count < 0) return -1;
            if (op->id == O_BRX && !count) return -1;
            for (t = 0; t < count; t++)
                if (!elsewhere[t]) changed |= direct_mark(seen, use, targets[t]);
            if (fall) changed |= direct_mark(seen, use, k + 1);
        }
    }
    return seen[use >> 3] >> (use & 7) & 1;
}

/* Where index's basic block starts: the last place control can arrive at before it, as direct_reaching_def finds it. */
static u32 direct_block_start(const u64* insn, u32 n, u32 index) {
    u32 start = 11;
    u32 k;
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        if (!IDX_OK(k)) continue;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) return start;
        for (t = 0; t < count; t++)
            if (targets[t] > start && targets[t] <= index) start = targets[t];
    }
    return start;
}

/* Whether control reaches use only through def and only forwards from it: no branch enters (def, use] from before def
   or from after it, and none in the region jumps back. A branch in between then tests a predicate def also saw, with
   no earlier pass through the region to change it. */
static int direct_forward_only(const u64* insn, u32 n, u32 def, u32 use) {
    u32 k;
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) {
            if (direct_flow_below(insn, k, use)) continue;
            return 0;
        }
        for (t = 0; t < count; t++) {
            if (targets[t] <= def || targets[t] > use) continue;
            if (elsewhere[t]) return 0;
            if (k < def) {
                if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(insn, k, def)) continue;
                return 0;
            }
            if (targets[t] <= k) return 0;
        }
    }
    return 1;
}

/* Whether every path to use passes the branch at from: a branch before from that lands past it is a way around. The
   edges in (def, use] all run forwards when direct_forward_only passed, so one scan is enough. */
static int direct_branch_dominates(const u64* insn, u32 n, u32 from, u32 use) {
    u32 k;
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) {
            if (direct_flow_below(insn, k, use)) continue;
            return 0;
        }
        for (t = 0; t < count; t++) {
            if (targets[t] <= from || targets[t] > use) continue;
            if (elsewhere[t]) return 0;
            if (k < from) {
                if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(insn, k, from)) continue;
                return 0;
            }
        }
    }
    return 1;
}

/* A three-valued boolean combine: 1 true, 0 false, -1 unknown. */
static int direct_pred_combine(u32 op, int a, int b) {
    if (op == 0) return a == 0 || b == 0 ? 0 : a == 1 && b == 1 ? 1 : -1;
    if (op == 1) return a == 1 || b == 1 ? 1 : a == 0 && b == 0 ? 0 : -1;
    if (op == 2) return a < 0 || b < 0 ? -1 : a != b;
    return a;
}

/* Whether a guard predicate must hold, given the values the branches to use force at def (known and value, bit p set
   when predicate p's value is known there). The guard resolves through the PSETP instructions that set it, which
   have to run on every path to def (floor starts def's straight run). A requirement is a value at def, so it describes
   a predicate read earlier only while nothing writes the predicate between. 1 when the guard holds, 0 when it doesn't,
   -1 when this can't tell. */
static int direct_guard_under(const u64* insn, u32 index, u32 floor, u32 guard, u32 known, u32 value, int depth) {
    u32 pred = guard & 7;
    int neg = (int)(guard >> 3);
    u32 j;
    if (pred == 7) return 1;
    if (depth > 8) return -1;
    if (known >> pred & 1) {
        int v = (int)(value >> pred & 1);
        return neg ? !v : v;
    }
    for (j = index; j-- > floor;) {
        const Op* op;
        u64 w;
        u32 pa, pb, pc, ka, kb, kc;
        int va, vb, vc, first;
        if (!IDX_OK(j)) continue;
        w = insn[j];
        op = decode(w);
        if (!op) return -1;
        if (!(shader_direct_pred_writes(w, op->id) >> pred & 1)) continue;
        if (B(w, 16, 4) != 7 || op->id != O_PSETP) return -1;
        pa = B(w, 12, 3);
        pb = B(w, 29, 3);
        pc = B(w, 39, 3);
        ka = known;
        kb = known;
        kc = known;
        if ((known >> pa & 1) && direct_pred_written(insn, j, index, pa)) ka &= ~(1u << pa);
        if ((known >> pb & 1) && direct_pred_written(insn, j, index, pb)) kb &= ~(1u << pb);
        if ((known >> pc & 1) && direct_pred_written(insn, j, index, pc)) kc &= ~(1u << pc);
        va = direct_guard_under(insn, j, floor, pa | B(w, 15, 1) << 3, ka, value, depth + 1);
        vb = direct_guard_under(insn, j, floor, pb | B(w, 32, 1) << 3, kb, value, depth + 1);
        vc = direct_guard_under(insn, j, floor, pc | B(w, 42, 1) << 3, kc, value, depth + 1);
        first = direct_pred_combine(B(w, 24, 2), va, vb);
        if (B(w, 0, 3) == pred) /* the second output is written last */
            first = direct_pred_combine(B(w, 24, 2), va < 0 ? -1 : !va, vb);
        else if (B(w, 3, 3) != pred)
            return -1;
        return direct_pred_combine(B(w, 45, 2), first, vc);
    }
    return -1;
}

/* Whether the branches on the way to use make def's predicated write run. A conditional branch that every path to
   use passes, and whose other edge doesn't reach use, forces its predicate's value at def; a guard those values make
   true under PSETP combinators then runs def on every path. The compiler guards a whole block's handles this way: one
   branch tests the predicate it later composes, so the def runs unguarded at the use. */
static int direct_guard_implied(const u64* insn, u32 n, u32 def, u32 use) {
    u32 known = 0, value = 0;
    u32 floor = direct_block_start(insn, n, def);
    u32 b;
    if (!direct_forward_only(insn, n, def, use)) return 0;
    /* the predicate setters resolved below have to run on every path to def */
    if (!direct_straight(insn, floor, def)) return 0;
    for (b = def + 1; b < use; b++) {
        const Op* op;
        u32 g, pred, target;
        int taken, fall, val;
        if (!IDX_OK(b)) continue;
        op = decode(insn[b]);
        if (!op) return 0;
        if (op->id != O_BRA) continue;
        g = B(insn[b], 16, 4);
        if ((g & 7) == 7) continue;
        if (!direct_branch_dominates(insn, n, b, use)) continue;
        target = bra_target(insn[b], b);
        taken = direct_can_reach(insn, n, target, use);
        fall = direct_can_reach(insn, n, b + 1, use);
        if (taken < 0 || fall < 0) return 0;
        if (taken == fall) continue; /* both ways reach use: the branch doesn't force its predicate */
        /* the branch is taken when the predicate is the negation bit's opposite */
        pred = g & 7;
        val = taken ? !(int)(g >> 3) : (int)(g >> 3);
        if (direct_pred_written(insn, def, b, pred)) continue;
        if (known >> pred & 1) {
            if ((int)(value >> pred & 1) != val) return 0;
            continue;
        }
        known |= 1u << pred;
        if (val) value |= 1u << pred;
    }
    if (!known) return 0;
    return direct_guard_under(insn, def, floor, B(insn[def], 16, 4), known, value, 0) == 1;
}

/* Whether a write under a predicate is followed, before the predicate changes or other flow, by a branch under
   want (that predicate or its complement) to where control doesn't reach use: under the same predicate the write's
   lanes all leave, so its value never arrives at use; under the complement the lanes that skipped it leave, so every
   lane that goes on to use made it */
static int direct_guard_branch(const u64* insn, u32 n, u32 def, u32 use, u32 want) {
    u32 guard = B(insn[def], 16, 4);
    if ((guard & 7) == 7) return 0;
    for (u32 m = def + 1; m < use; m++) {
        const Op* op;
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        if (!IDX_OK(m)) continue;
        op = decode(insn[m]);
        if (!op || shader_direct_pred_writes(insn[m], op->id) >> (guard & 7) & 1) return 0;
        if (op->id == O_BRA && B(insn[m], 16, 4) == want && B(insn[m], 0, 5) == 15) {
            count = direct_flow_targets(insn, n, m, targets, elsewhere);
            if (count <= 0) return 0;
            for (int t = 0; t < count; t++)
                if (elsewhere[t] || direct_can_reach(insn, n, targets[t], use) != 0) return 0;
            return 1;
        }
        if (op->id == O_BRA || op->id == O_BRX || op->id == O_JMP || op->id == O_CAL || op->id == O_RET ||
            op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT || op->id == O_EXIT || op->id == O_KIL)
            return 0;
    }
    return 0;
}

/* The writes of reg that reach use, as bits of defs (bit 63: the value entering the program), from the reaching
   definitions over the program's flow: an unpredicated write replaces what reached it, a predicated one adds to it.
   0 when the flow isn't known (JMP, BRK, CONT, a target elsewhere) or reg has more than 63 writes */
static u64 direct_reaching_writes(const u64* insn, u32 n, u32 use, u32 reg, u32* defs, u32* ndefs) {
    u64* in;
    u64 reach;
    int changed = 1;
    *ndefs = 0;
    if (n > DIRECT_WORDS_MAX || use >= n) return 0;
    for (u32 k = 11; k < n; k++) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        if (!shader_writes_reg(insn[k], op->id, reg)) continue;
        if (*ndefs == 63) return 0;
        defs[(*ndefs)++] = k;
    }
    in = calloc(n + 1, sizeof *in);
    if (!in) return 0;
    in[11] = 1ull << 63;
    while (changed) {
        changed = 0;
        for (u32 k = 11, d = 0; k < n; k++) {
            const Op* op;
            u32 targets[64];
            u8 elsewhere[64];
            int count, fall = 1;
            u64 out = in[k];
            if (!IDX_OK(k)) {
                changed |= (in[k + 1] | out) != in[k + 1];
                in[k + 1] |= out;
                continue;
            }
            op = decode(insn[k]);
            while (d < *ndefs && defs[d] < k) d++;
            if (d < *ndefs && defs[d] == k) out = (B(insn[k], 16, 4) == 7 ? 0 : out) | 1ull << d;
            switch (op->id) {
                case O_EXIT: fall = !flow_always(insn[k]); break;
                case O_KIL:
                case O_RET:
                case O_PRET:
                case O_PEXIT:
                case O_BRX: fall = 0; break;
                case O_BRK:
                case O_CONT:
                case O_JMP: free(in); return 0;
                case O_BRA: fall = !flow_always(insn[k]); break;
                default: break;
            }
            count = direct_flow_targets(insn, n, k, targets, elsewhere);
            /* an SSY carries no values to its target: each SYNC takes its own there, to the target of the innermost
               SSY still open (the nearest before it whose target is past it); a predicated one lets the others on */
            if (op->id == O_SSY) count = 0;
            if (op->id == O_SYNC) {
                count = -1;
                for (u32 m = k; m-- > 11 && count < 0;) {
                    if (!IDX_OK(m) || decode(insn[m])->id != O_SSY || bra_target(insn[m], m) <= k) continue;
                    targets[0] = bra_target(insn[m], m);
                    elsewhere[0] = 0;
                    count = 1;
                }
                fall = !flow_always(insn[k]);
            }
            if (count < 0 || (op->id == O_BRX && !count)) {
                free(in);
                return 0;
            }
            for (int t = 0; t < count; t++) {
                if (elsewhere[t] || targets[t] >= n) {
                    free(in);
                    return 0;
                }
                changed |= (in[targets[t]] | out) != in[targets[t]];
                in[targets[t]] |= out;
            }
            if (fall && k + 1 < n) {
                changed |= (in[k + 1] | out) != in[k + 1];
                in[k + 1] |= out;
            }
        }
    }
    reach = in[use];
    free(in);
    return reach;
}

/* Whether def's value reaches use however the lanes went: every write of reg that reaches use defines what def does
   (same), but a predicated one whose lanes all leave by a branch under the same predicate; and no lane arrives
   without one, or one of them is made by every lane that reaches use (unpredicated, or a branch under the complement
   takes away the lanes that skipped it) and dominates use. A handle written on each side of a branch, or under each
   side of a predicate rewritten before the use, is so one handle */
static int direct_all_defs_same(const u64* insn, u32 n, u32 def, u32 use, u32 reg,
                                int (*same)(const u64*, u32, u32, u32, int), int depth) {
    u32 defs[63], ndefs;
    u64 reach;
    int covered;
    if (depth > 6) return 0;
    reach = direct_reaching_writes(insn, n, use, reg, defs, &ndefs);
    if (!reach) return 0;
    covered = !(reach >> 63 & 1);
    for (u32 d = 0; d < ndefs; d++) {
        u32 k = defs[d], guard = B(insn[k], 16, 4);
        if (!(reach >> d & 1)) continue;
        if (guard != 7 && direct_guard_branch(insn, n, k, use, guard)) continue;
        if (B(insn[k], 0, 8) != reg || (k != def && !same(insn, n, def, k, depth + 1))) return 0;
        if (!covered && k < use && (guard == 7 || direct_guard_branch(insn, n, k, use, guard ^ 8)))
            covered = direct_def_dominates(insn, n, k, use, reg, same, depth);
    }
    return covered;
}

/* The instruction that last wrote reg before index, as its first destination, or -1. In index's basic block, a
   write under a predicate reaches only an instruction under the same one, with that predicate not written between
   them (others may be): the compiler loads a handle and uses it under the guard of one side of a branch it made
   predicated. Across flow, the write has to be unconditional and dominate index. */
static int direct_reaching_def_same(const u64* insn, u32 n, u32 index, u32 reg,
                                    int (*same)(const u64*, u32, u32, u32, int), int depth) {
    u32 guard, pred_written = 0;
    int crossed = 0;
    /* where index's basic block starts: the last place control can arrive at before it */
    u32 block_start = 11;
    u32 k;
    u32 j;
    if (reg == 255) return -1;
    guard = B(insn[index], 16, 4);
    for (k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        int count;
        int t;
        if (!IDX_OK(k)) continue;
        count = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (count < 0) {
            if (direct_flow_below(insn, k, index)) continue;
            return -1;
        }
        for (t = 0; t < count; t++)
            if (targets[t] > block_start && targets[t] <= index) block_start = targets[t];
    }
    for (j = index; j-- > 11;) {
        const Op* op;
        if (!IDX_OK(j)) continue;
        op = decode(insn[j]);
        if (!op) return -1;
        if (j < block_start || op->id == O_BRA || op->id == O_BRX || op->id == O_CAL || op->id == O_RET ||
            op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT || op->id == O_EXIT || op->id == O_KIL)
            crossed = 1;
        if (!shader_writes_reg(insn[j], op->id, reg)) {
            pred_written |= shader_direct_pred_writes(insn[j], op->id);
            continue;
        }
        /* A write in an earlier block that control never carries to index isn't the value there: the block's branch
           jumped past the use. */
        if (j < block_start && direct_can_reach(insn, n, j + 1, index) == 0) continue;
        /* A write under the complement of index's guard, the predicate kept since, runs in none of index's lanes */
        if ((guard & 7) != 7 && B(insn[j], 16, 4) == (guard ^ 8) && !crossed && !(pred_written >> (guard & 7) & 1))
            continue;
        /* A write that reaches reg from a lower destination leaves no single source. */
        if (B(insn[j], 0, 8) != reg) return -1;
        if ((B(insn[j], 16, 4) != 7 && (crossed || B(insn[j], 16, 4) != guard || (pred_written >> (guard & 7) & 1)) &&
             !direct_same_guard(insn, block_start, j, index) && !direct_guard_implied(insn, n, j, index)) ||
            (crossed && !direct_def_dominates(insn, n, j, index, reg, same, depth)))
            return same && direct_all_defs_same(insn, n, j, index, reg, same, depth) ? (int)j : -1;
        return (int)j;
    }
    return -1;
}

static int direct_reaching_def(const u64* insn, u32 n, u32 index, u32 reg) {
    return direct_reaching_def_same(insn, n, index, reg, NULL, 0);
}

/* reg at index holds the fragment's own column (addr 0x70) or row (0x74): the integer part of gl_FragCoord's x or y,
   F2I.FLOOR or .TRUNC of IPA's pass-through read of the position */
static int direct_frag_coord(const u64* insn, u32 n, u32 index, u32 reg, u32 addr, int depth) {
    int j = depth > 4 ? -1 : direct_reaching_def(insn, n, index, reg);
    if (j < 0) return 0;
    u64 w = insn[j];
    int op = decode(w)->id;
    if (op == O_MOV_R) return direct_frag_coord(insn, n, (u32)j, B(w, 20, 8), addr, depth + 1);
    /* F32 source, no negate or absolute value, rounding down or toward zero */
    if (op != O_F2I_R || B(w, 10, 2) != 2 || B(w, 45, 1) || B(w, 49, 1) || (B(w, 39, 2) != 1 && B(w, 39, 2) != 3))
        return 0;
    int k = direct_reaching_def(insn, n, (u32)j, B(w, 20, 8));
    if (k < 0) return 0;
    u64 v = insn[k];
    /* not indexed, not multiplied by 1/w, not saturated */
    return decode(v)->id == O_IPA && !B(v, 38, 1) && B(v, 30, 8) * 4 == addr && B(v, 54, 2) != 1 && !B(v, 51, 1);
}
/* the texel fetch at index reads the fragment's own pixel of a 2D texture: level 0, no offset, single sample */
int shader_direct_own_pixel_fetch(const u64* insn, u32 n, u32 index) {
    u64 w = insn[index];
    int op = decode(w)->id;
    u32 a = B(w, 8, 8), b = B(w, 20, 8);
    if (op == O_TLDS) /* encoding 2: 2D, level zero, x in Ra and y in Rb */
        return B(w, 53, 4) == 2 && direct_frag_coord(insn, n, index, a, 0x70, 0) &&
               direct_frag_coord(insn, n, index, b, 0x74, 0);
    if (op == O_TLD) /* 2D, not an array, no level (bit 55), offset (35) or sample (50): x in Ra, y in Ra + 1 */
        return B(w, 28, 3) == 2 && !B(w, 55, 1) && !B(w, 35, 1) && !B(w, 50, 1) && a < 254 &&
               direct_frag_coord(insn, n, index, a, 0x70, 0) && direct_frag_coord(insn, n, index, a + 1, 0x74, 0);
    return 0;
}

int shader_direct_track_const(const u64* insn, u32 n, u32 index, u32 reg, u32* value, int depth) {
    int j = depth > 6 ? -1 : direct_reaching_def(insn, n, index, reg);
    u64 w;
    int op;
    if (j < 0) return 0;
    w = insn[j];
    op = decode(w)->id;
    if (op == O_MOV32I) {
        *value = B(w, 20, 32);
        return 1;
    }
    if (op == O_MOV_R) return shader_direct_track_const(insn, n, (u32)j, B(w, 20, 8), value, depth + 1);
    return 0;
}

/* Resolve a register supplied image handle to the constant buffer word read in this basic block. */
int shader_direct_track_image_handle(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, int depth) {
    int j = depth > 6 ? -1 : direct_reaching_def(insn, n, index, reg);
    u64 w;
    int op;
    if (j < 0) return 0;
    w = insn[j];
    op = decode(w)->id;
    if (op == O_MOV_C) {
        *bank = B(w, 34, 5);
        *off = B(w, 20, 14);
        return *bank < 18;
    }
    if (op == O_LDC && B(w, 48, 3) >= 4 && B(w, 48, 3) <= 5) {
        i32 byte_off = (i16)B(w, 20, 16);
        *bank = B(w, 36, 5);
        *off = (u32)byte_off / 4;
        /* Keep the legacy first-descriptor behavior for indexed handles.
           Runtime descriptor selection for arbitrary indices needs a separate path. */
        return *bank < 18 && byte_off >= 0 && !(byte_off & 3);
    }
    if (op == O_MOV_R) return shader_direct_track_image_handle(insn, n, (u32)j, B(w, 20, 8), bank, off, depth + 1);
    return 0;
}

static int direct_handle_definition(const u64* insn, u32 n, u32 j, u32* bank, u32* off, u32* bank2, u32* off2,
                                    int depth) {
    u64 w = insn[j];
    int op = decode(w)->id;
    int plain_or;
    int plain_add;
    u32 lhs;
    u32 rhs;
    *bank2 = *off2 = 0;
    if (op == O_MOV_C) {
        *bank = B(w, 34, 5);
        *off = B(w, 20, 14);
        return *bank < 18;
    }
    if (op == O_LDC && B(w, 48, 3) >= 4 && B(w, 48, 3) <= 5) {
        i32 byte_off = (i16)B(w, 20, 16);
        *bank = B(w, 36, 5);
        *off = (u32)byte_off / 4;
        return *bank < 18 && byte_off >= 0 && !(byte_off & 3);
    }
    if (op == O_MOV_R)
        return shader_direct_track_image_handle_pair(insn, n, (u32)j, B(w, 20, 8), bank, off, bank2, off2, depth + 1);
    plain_or = (op == O_LOP_R || op == O_LOP_C) && B(w, 41, 2) == 1 && !B(w, 39, 2) && !B(w, 47, 1);
    plain_add = (op == O_IADD_R || op == O_IADD_C) && !B(w, 43, 1) && !B(w, 47, 1) && !B(w, 48, 2);
    if (!plain_or && !plain_add) return 0;
    lhs = B(w, 8, 8);
    if (op == O_LOP_C || op == O_IADD_C) {
        u32 cbank = B(w, 34, 5), coff = B(w, 20, 14);
        if (cbank >= 18) return 0;
        if (lhs == 255) {
            *bank = cbank;
            *off = coff;
            return 1;
        }
        *bank2 = cbank;
        *off2 = coff;
        return shader_direct_track_image_handle(insn, n, (u32)j, lhs, bank, off, depth + 1) ? 2 : 0;
    }
    rhs = B(w, 20, 8);
    if (lhs == 255)
        return shader_direct_track_image_handle_pair(insn, n, (u32)j, rhs, bank, off, bank2, off2, depth + 1);
    if (rhs == 255)
        return shader_direct_track_image_handle_pair(insn, n, (u32)j, lhs, bank, off, bank2, off2, depth + 1);
    /* both halves read, or one read and the other a constant (a sampler's from a MOV32I), kept as itself */
    for (int k = 0; k < 2; k++) {
        u32 half = k ? rhs : lhs, other = k ? lhs : rhs, value;
        if (!shader_direct_track_image_handle(insn, n, (u32)j, half, bank, off, depth + 1)) continue;
        if (shader_direct_track_image_handle(insn, n, (u32)j, other, bank2, off2, depth + 1)) return 2;
        if (!shader_direct_track_const(insn, n, (u32)j, other, &value, depth + 1)) return 0;
        *bank2 = SH_HANDLE_IMM;
        *off2 = value;
        return 2;
    }
    return 0;
}

/* Whether the write of reg at b defines the same handle as the one at a: a loop that rewrites reg with the same
   constant buffer reads leaves the handle at a use in the loop unchanged (the second tap of a blur, or its next
   iteration's first). */
static int direct_same_handle(const u64* insn, u32 n, u32 a, u32 b, int depth) {
    u32 ab, ao, ab2, ao2, bb, bo, bb2, bo2;
    int ca, cb;
    if (depth > 6) return 0;
    ca = direct_handle_definition(insn, n, a, &ab, &ao, &ab2, &ao2, depth);
    cb = ca ? direct_handle_definition(insn, n, b, &bb, &bo, &bb2, &bo2, depth) : 0;
    return ca && ca == cb && ab == bb && ao == bo && (ca == 1 || (ab2 == bb2 && ao2 == bo2));
}

/* At a forward branch merge, two different definitions can hold the same
   descriptor. Prove the value on every edge bypassing the nearest definition,
   rather than binding whichever side happens to appear last in the program.
   Indirect flow, reconvergence arrivals and back edges remain conservative. */
static int direct_joined_handle(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, u32* bank2, u32* off2,
                                int depth) {
    int j = -1, count, joined = 0;
    for (u32 k = index; k-- > 11;) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        if (!shader_writes_reg(insn[k], op->id, reg)) continue;
        if (B(insn[k], 0, 8) != reg || B(insn[k], 16, 4) != 7) return 0;
        j = (int)k;
        break;
    }
    if (j < 0 || !(count = direct_handle_definition(insn, n, (u32)j, bank, off, bank2, off2, depth + 1))) return 0;
    for (u32 k = 11; k < n; k++) {
        u32 targets[64];
        u8 elsewhere[64];
        const Op* op;
        int edges;
        if (!IDX_OK(k)) continue;
        op = decode(insn[k]);
        if (!op) return 0;
        if (k > (u32)j && k < index &&
            (op->id == O_CAL || op->id == O_RET || op->id == O_BRX || op->id == O_JMP || op->id == O_SYNC ||
             op->id == O_BRK || op->id == O_CONT))
            return 0;
        edges = direct_flow_targets(insn, n, k, targets, elsewhere);
        if (edges < 0) return 0;
        for (int t = 0; t < edges; t++) {
            u32 b, o, b2, o2;
            if (targets[t] <= (u32)j || targets[t] > index) continue;
            if (elsewhere[t] || k > index) return 0;
            if (k >= (u32)j) continue;
            if (op->id != O_BRA ||
                shader_direct_track_image_handle_pair(insn, n, k, reg, &b, &o, &b2, &o2, depth + 1) != count ||
                b != *bank || o != *off || (count == 2 && (b2 != *bank2 || o2 != *off2)))
                return 0;
            joined = 1;
        }
    }
    return joined ? count : 0;
}

int shader_direct_track_image_handle_pair(const u64* insn, u32 n, u32 index, u32 reg, u32* bank, u32* off, u32* bank2,
                                          u32* off2, int depth) {
    int j;
    if (depth > 6 || reg == 255) return 0;
    j = direct_reaching_def_same(insn, n, index, reg, direct_same_handle, depth);
    if (j >= 0) return direct_handle_definition(insn, n, (u32)j, bank, off, bank2, off2, depth);
    return direct_joined_handle(insn, n, index, reg, bank, off, bank2, off2, depth);
}

/* AL2P's predicate (bits 44-46) is a range check no source describes. The NVN compiler gives every AL2P one, P0 to P6
   in turn, and overwrites or leaves them, so a program is translated without it only when nothing can see it: no
   instruction reads the predicate, or every path from the AL2P meets a comparison or AL2P without a guard that writes
   it whole before anything reads it. Paths follow branches, jump tables and the pops' targets as shader_resolve_crs
   gives them. A pop it can't resolve, a call, a return, a JMP, or an EXIT a PEXIT may take counts as a read */
int shader_al2p_predicate_unseen(const u64* insn, u32 n, u32 at) {
    u32 p = B(insn[at], 44, 3), i, nwork = 0, *work = NULL, *pops = NULL, targets[64];
    int read = 0, pexit = 0, stack = 0, crs = 0, unseen = 0, count, t;
    u8* seen = NULL;
    if (p == 7) return 1;
    for (i = 11; i < n; i++) {
        const Op* op = IDX_OK(i) && i != at ? decode(insn[i]) : NULL;
        if (!op) continue;
        read |= shader_reads_predicate(insn[i], op->id, p);
        pexit |= op->id == O_PEXIT;
        stack |= op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT;
    }
    if (!read) return 1;
    seen = calloc(n, 1);
    work = malloc(n * sizeof *work);
    if (stack) pops = calloc(n, sizeof *pops);
    if (!seen || !work || (stack && !pops)) goto done;
    crs = stack && shader_resolve_crs(insn, n, pops);
    seen[at] = 1;
    i = at + 1 + ((at + 1 - 10) % 4 == 0);
    if (i < n) {
        seen[i] = 1;
        work[nwork++] = i;
    }
    while (nwork) {
        i = work[--nwork];
        for (;;) {
            u64 w = insn[i];
            const Op* op = decode(w);
            u32 next = i + 1 + ((i + 1 - 10) % 4 == 0);
            count = 0;
            if (!op || shader_reads_predicate(w, op->id, p)) goto done;
            if (B(w, 16, 4) == 7 && (op->id == O_AL2P || shader_predicate_comparison(op->id)) &&
                shader_writes_predicate(w, op->id, p))
                break;
            switch (op->id) {
                case O_BRA:
                    if (B(w, 5, 1)) goto done;
                    targets[count++] = bra_target(w, i);
                    break;
                case O_BRX:
                    count = shader_brx_targets(insn, i, n, targets, 64);
                    if (count < 0) goto done;
                    break;
                case O_SYNC:
                case O_BRK:
                case O_CONT:
                    if (!crs || !pops[i]) goto done;
                    targets[count++] = pops[i];
                    break;
                case O_EXIT:
                    if (pexit) goto done;
                    break;
                case O_JMP:
                case O_CAL:
                case O_RET:
                case O_PRET: goto done;
                default: break;
            }
            for (t = 0; t < count; t++) {
                if (targets[t] >= n || !IDX_OK(targets[t])) goto done;
                if (!seen[targets[t]]) {
                    seen[targets[t]] = 1;
                    work[nwork++] = targets[t];
                }
            }
            if ((count || op->id == O_EXIT) && flow_always(w)) break;
            if (next >= n || seen[next]) break;
            seen[next] = 1;
            i = next;
        }
    }
    unseen = 1;
done:
    free(seen);
    free(work);
    free(pops);
    return unseen;
}

/* An indexed handle: the LDC that loads it, through MOVs, takes its address from a register. Its flags
   (SNX_TEX_INDEXED and the stride's shift: the SHL that made the address, or 2 for words) and *ldc, that LDC's
   instruction plus one. 0 when the handle isn't indexed, or can't be: the device has no texture arrays, or the
   stage is compute, which binds none. */
u32 shader_direct_indexed_handle(const u64* insn, u32 n, u32 index, u32 reg, int stage, u32* ldc) {
    int j = -1, k, depth, op = 0;
    u64 w = 0;
    u32 shift = 2;
    if (!g_shader_texture_arrays || stage == ST_CS) return 0;
    for (depth = 0; depth < 6; depth++) {
        j = direct_reaching_def(insn, n, index, reg);
        if (j < 0) return 0;
        w = insn[j];
        op = decode(w)->id;
        if (op != O_MOV_R) break;
        index = (u32)j;
        reg = B(w, 20, 8);
    }
    if (depth == 6 || op != O_LDC || B(w, 8, 8) == 255 || B(w, 48, 3) != 4) return 0;
    k = direct_reaching_def(insn, n, (u32)j, B(w, 8, 8));
    if (k >= 0 && decode(insn[k])->id == O_SHL_I && B(insn[k], 20, 5) >= 2 && B(insn[k], 20, 5) <= 15)
        shift = B(insn[k], 20, 5);
    *ldc = (u32)j + 1;
    return SNX_TEX_INDEXED | shift << 4;
}

/* Whether a bindless slot reads the handle a tracker found: its bank and second read, or for an indexed handle the
   same LDC (bl2_off holds its instruction plus one, as no second read is kept). */
int shader_direct_bindless_matches(const ShTex* t, u32 bank, int handle_count, u32 bank2, u32 off2, u32 indexed,
                                   u32 ldc) {
    if ((u32)(t->flags & 0xF4) != indexed || t->bl_cbuf != bank) return 0;
    if (indexed) return t->bl2_off == ldc;
    return t->bl2 == (handle_count == 2) && (handle_count != 2 || (t->bl2_cbuf == bank2 && t->bl2_off == off2));
}

/* What a TXQ's texture is (Maxwell's type field halved, and array), from another instruction of the program reading
   the same texture: a bound one by its handle's offset, a bindless one by the first word of its tracked handle (the
   texture half: the sampler half may differ, as TXQ doesn't use it). TXQ itself doesn't say, and one game queries a 3D
   texture's depth with it, which a 2D slot can't bind. 0 when nothing else reads it: the query is then for a 2D one */
int shader_direct_query_type(const u64* insn, u32 n, int bindless, u32 bank, u32 off, u32* type, u32* array) {
    for (u32 k = 11; k < n; k++) {
        const Op* op;
        u32 ty, kb, ko, kb2, ko2;
        int b;
        if (!IDX_OK(k) || !(op = decode(insn[k]))) continue;
        switch (op->id) {
            case O_TEX:
            case O_TLD:
            case O_TLD4:
            case O_TMML: b = 0; break;
            case O_TEX_B:
            case O_TLD_B:
            case O_TLD4_B:
            case O_TMML_B: b = 1; break;
            default: continue;
        }
        if (b != bindless) continue;
        ty = B(insn[k], 28, 3);
        if (b) {
            if (!shader_direct_track_image_handle_pair(insn, n, k, B(insn[k], 20, 8), &kb, &ko, &kb2, &ko2, 0) ||
                kb != bank || ko != off)
                continue;
        } else if (B(insn[k], 36, 13) != off)
            continue;
        *type = ty >> 1;
        *array = ty & 1;
        return 1;
    }
    return 0;
}

/* the last instruction before i that writes reg, in program order (not its reaching definition: a refusal names it
   so a log shows where a value came from without the program), and its index in *at */
static const Op* last_writer(const u64* insn, u32 i, u32 reg, u32* at) {
    for (*at = i; *at > 11;) {
        const Op* o;
        --*at;
        o = IDX_OK(*at) ? decode(insn[*at]) : NULL;
        if (o && shader_writes_reg(insn[*at], o->id, reg)) return o;
    }
    return NULL;
}

/* a bindless instruction whose handle register no tracking reached: which register, of which instruction, where,
   and the last write of the register, with its value when that write is a constant (a MOV32I's, or a MOV of RZ) */
u32* shader_untracked_handle(ShaderInfo* si, const char* kind, const u64* insn, u32 i, u32 reg) {
    const Op *op = decode(insn[i]), *writer;
    char from[48] = ": no write before";
    u32 at;
    if ((writer = last_writer(insn, i, reg, &at))) {
        u64 w = insn[at];
        int n = snprintf(from, sizeof from, ": %s at %x", writer->name, at * 8);
        if (writer->id == O_MOV32I || (writer->id == O_MOV_R && B(w, 20, 8) == 255))
            snprintf(from + n, sizeof from - (size_t)n, " (the constant %#x)",
                     writer->id == O_MOV32I ? (u32)B(w, 20, 32) : 0u);
    }
    si->unsupported = 1;
    snprintf(si->first_unsupported, sizeof si->first_unsupported, "untracked %s handle R%u of %s at %x%s", kind, reg,
             op ? op->name : "?", i * 8, from);
    return NULL;
}

/* A global memory access at i whose base register (reg) the tracking lost, and that can't be found among the
   program's buffers when it runs (an atomic, a sub-word access, or no tracked buffer at all). The reason names the
   last instruction before it that wrote the base, so a log shows what to add without the program */
u32* shader_untracked_reject(ShaderInfo* si, const u64* insn, u32 i, u32 reg, u8* gb_at, u32* pop_targets) {
    u32 at;
    const Op* writer = last_writer(insn, i, reg, &at);
    free(gb_at);
    free(pop_targets);
    if (writer)
        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                 "global memory base R%u not tracked at %x: %s at %x", reg, i * 8, writer->name, at * 8);
    else
        snprintf(si->first_unsupported, sizeof si->first_unsupported,
                 "global memory base R%u not tracked at %x: no write before", reg, i * 8);
    si->unsupported++;
    return NULL;
}
/* The words of a program (n of them, sph its header, words in all) with every attribute load, store and
   interpolation of legacy varyings moved to their generics (shader_legacy_address): in *moved, a malloc'd copy of all the
   words, when there is one to move, and in
   *fs_groups the groups a fragment program reads (their interpolation is the header's color and texture coordinate
   maps', to_spirv). 0, or -1 with si's reason when a group and its generic are both read or both written, or memory
   ran out. An access some of whose words are legacy and others not is left, for shader_supported() to refuse */
int shader_legacy_varyings(const u64* insn, u32 n, u32 words, const u32* sph, int stage, ShaderInfo* si, u64** moved,
                           u32* fs_groups) {
    u32 legacy[2] = {0}, generic[2] = {0}, i, d; /* [0] read, [1] written */
    *moved = NULL;
    *fs_groups = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (i = 11; i < n; i++) {
            const Op* op = IDX_OK(i) ? decode(insn[i]) : NULL;
            u64 w;
            u32 addr, count, out, k;
            if (!op || (op->id != O_ALD && op->id != O_AST && op->id != O_IPA)) continue;
            w = insn[i];
            if (op->id == O_IPA) {
                if (stage != ST_FS) continue;
                if (B(w, 38, 1)) { /* indexed: any generic the header gives a mode */
                    for (u32 v = 0; v < 32; v++)
                        if ((sph[6 + v / 4] >> (v % 4 * 8)) & 0xff) generic[0] |= 1u << v;
                    continue;
                }
                addr = B(w, 30, 8) * 4;
                count = 1;
                out = 0;
            } else {
                if (B(w, 31, 1)) continue; /* a patch's attributes */
                out = op->id == O_AST || (stage == ST_TCS && B(w, 32, 1));
                if (B(w, 8, 8) != 255) { /* indexed: any generic input the header's map lists */
                    if (op->id == O_ALD)
                        for (u32 v = 0; v < 32; v++)
                            if ((sph[6 + v / 8] >> (v % 8 * 4)) & 15) generic[0] |= 1u << v;
                    continue;
                }
                if (stage == ST_VS && !out) continue; /* a vertex program's inputs are the vertex's attributes */
                addr = B(w, 20, 10);
                count = B(w, 47, 2) + 1;
            }
            if (!pass) {
                for (k = 0; k < count; k++) {
                    u32 a = addr + 4 * k;
                    int g = shader_legacy_group(a);
                    if (g >= 0)
                        legacy[out] |= 1u << g;
                    else if (a >= 0x80 && a < 0x280)
                        generic[out] |= 1u << ((a - 0x80) >> 4);
                }
                continue;
            }
            for (k = 0; k < count; k++)
                if (shader_legacy_group(addr + 4 * k) < 0 ||
                    shader_legacy_address(addr + 4 * k) != shader_legacy_address(addr) + 4 * k)
                    break;
            if (k < count) continue;
            (*moved)[i] = op->id == O_IPA ? (w & ~(0xffull << 30)) | (u64)(shader_legacy_address(addr) / 4) << 30
                                          : (w & ~(0x3ffull << 20)) | (u64)shader_legacy_address(addr) << 20;
        }
        if (pass) break;
        if (!legacy[0] && !legacy[1]) return 0;
        for (d = 0; d < 2; d++)
            for (u32 g = 0; g < 15; g++)
                if ((legacy[d] >> g & 1) && (generic[d] >> (LEGACY_GENERIC + g) & 1)) {
                    si->unsupported = 1;
                    snprintf(si->first_unsupported, sizeof si->first_unsupported,
                             "legacy varying 0x%x %s generic %u, which the program also %s", shader_legacy_base(g),
                             d ? "written as" : "read as", LEGACY_GENERIC + g, d ? "writes" : "reads");
                    return -1;
                }
        *moved = malloc(words * sizeof **moved);
        if (!*moved) return -1;
        memcpy(*moved, insn, words * sizeof **moved);
    }
    if (stage == ST_FS) *fs_groups = legacy[0];
    return 0;
}
