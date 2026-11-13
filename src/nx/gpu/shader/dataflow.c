/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The analyses over a program's words before it is translated: flow targets, reaching definitions,
   guards, and where a texture handle, a constant or a global memory base comes from */
#include "nx/gpu/shader/translate.h"

static int direct_reaching_def_same(const ShFlow* f, u32 index, u32 reg, int (*same)(const ShFlow*, u32, u32, int),
                                    int depth);
static int direct_reaching_def(const ShFlow* f, u32 index, u32 reg);

/* Where control can arrive from the flow instruction at k, besides the next instruction: a branch, SSY, PBK or PCNT
   target, a jump table's entries, a CAL's callee and the return point after it, a PRET's return point. -1 when that
   can't be known (a JMP, or a jump table that isn't found). elsewhere[t] is set for arrivals that come from other
   instructions than k: a PCNT's target takes CONTs from later in the loop, and a return point takes RETs. */
static int direct_flow_decode(const u64* insn, u32 n, const Op* flow, u32 k, u32 targets[64], u8 elsewhere[64]) {
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

int shader_flow_init(ShFlow* f, const u64* insn, u32 n) {
    u32 k, edges = 0, cap = 64, targets[64];
    u8 elsewhere[64];
    *f = (ShFlow){.insn = insn, .n = n, .bad = n, .known_to = ~0u};
    f->op = malloc((n ? n : 1) * sizeof *f->op);
    f->edge_at = malloc((n + 1) * sizeof *f->edge_at);
    f->unknown = calloc(n ? n : 1, 1);
    f->block_start = malloc((n ? n : 1) * sizeof *f->block_start);
    f->flow = malloc((n ? n : 1) * sizeof *f->flow);
    f->target = malloc(cap * sizeof *f->target);
    f->elsewhere = malloc(cap);
    if (!f->op || !f->edge_at || !f->unknown || !f->block_start || !f->flow || !f->target || !f->elsewhere) goto fail;
    for (k = 0; k < n; k++) {
        f->op[k] = decode(insn[k]);
        f->edge_at[k] = edges;
        if (!IDX_OK(k)) continue;
        if (!f->op[k] && f->bad == n) f->bad = k;
        /* every instruction with targets has 0xE2 as its top byte (maxwell_opcodes.def) */
        int count = insn[k] >> 56 == 0xE2 ? direct_flow_decode(insn, n, f->op[k], k, targets, elsewhere) : 0;
        if (!count) continue;
        f->flow[f->nflow++] = k;
        if (count < 0) {
            f->unknown[k] = 1;
            /* direct_flow_below: a jump table's cases all lie after it, a JMP's target anywhere */
            if (f->op[k]->id != O_BRX)
                f->known_to = 0;
            else if (k < f->known_to)
                f->known_to = k;
            continue;
        }
        if (edges + (u32)count > cap) {
            cap = (edges + (u32)count) * 2;
            u32* t = realloc(f->target, cap * sizeof *t);
            if (t) f->target = t;
            u8* e = realloc(f->elsewhere, cap);
            if (e) f->elsewhere = e;
            if (!t || !e) goto fail;
        }
        memcpy(f->target + edges, targets, (size_t)count * sizeof *targets);
        memcpy(f->elsewhere + edges, elsewhere, (size_t)count);
        edges += (u32)count;
    }
    f->edge_at[n] = edges;
    /* the arrivals' places, then each instruction's last one at or before it */
    for (k = 0; k < n; k++) f->block_start[k] = 0;
    for (u32 e = 0; e < edges; e++)
        if (f->target[e] < n) f->block_start[f->target[e]] = 1;
    for (k = 0, edges = 11; k < n; k++) {
        if (k > 11 && f->block_start[k]) edges = k;
        f->block_start[k] = edges;
    }
    return 1;
fail:
    shader_flow_free(f);
    return 0;
}

void shader_flow_free(ShFlow* f) {
    free(f->op);
    free(f->edge_at);
    free(f->target);
    free(f->elsewhere);
    free(f->unknown);
    free(f->flow);
    free(f->block_start);
    *f = (ShFlow){0};
}

/* The flow targets of the word k (direct_flow_decode's, from f): their count, -1 when they can't be known */
static inline int direct_flow_targets(const ShFlow* f, u32 k, const u32** targets, const u8** elsewhere) {
    u32 at = f->edge_at[k];
    *targets = f->target + at;
    *elsewhere = f->elsewhere + at;
    return f->unknown[k] ? -1 : (int)(f->edge_at[k + 1] - at);
}

/* Whether a flow instruction whose targets aren't known at k can be left out of a search that only cares about targets
   at or below limit: every case of a jump table lies after its BRX (shader_brx_targets), so one at or after limit
   cannot reach the search's region. A JMP takes a register's target and can go anywhere, so it never can. */
static int direct_flow_below(const ShFlow* f, u32 k, u32 limit) {
    const Op* op = f->op[k];
    return op && op->id == O_BRX && k >= limit;
}

/* Whether control goes from the instruction at from straight on to the one at to, with nothing between that leaves */
static int direct_straight(const ShFlow* f, u32 from, u32 to) {
    u32 k;
    for (k = from + 1; k < to; k++) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = f->op[k];
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

static int direct_def_dominates(const ShFlow* f, u32 def, u32 use, u32 reg, int (*same)(const ShFlow*, u32, u32, int),
                                int depth) {
    const u64* insn = f->insn;
    u32 end = use;
    if (f->bad < f->n) return 0; /* an instruction decode doesn't know may go anywhere */
    for (;;) {
        u32 k, grown = end;
        for (u32 i = 0; i < f->nflow; i++) {
            const u32* targets;
            const u8* elsewhere;
            int count;
            int t;
            k = f->flow[i];
            const Op* op = f->op[k];
            count = direct_flow_targets(f, k, &targets, &elsewhere);
            if (count < 0) {
                if (direct_flow_below(f, k, end)) continue;
                return 0;
            }
            for (t = 0; t < count; t++) {
                if (targets[t] <= def || targets[t] > end) continue;
                if (elsewhere[t]) return 0;
                if (k < def) {
                    if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(f, k, def)) continue;
                    /* a path that leaves before def carries def's value on if reg already has it there (a loop's
                       exit branch, taken before the loop's last write of reg) */
                    if (same) {
                        int jk = direct_reaching_def_same(f, k, reg, same, depth + 1);
                        if (jk >= 0 && same(f, def, (u32)jk, depth + 1)) continue;
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
            op = f->op[k];
            if (!op) return 0;
            if (shader_writes_reg(insn[k], op->id, reg)) {
                /* a loop that rewrites reg with the definition def has doesn't hide def's value */
                if (same && B(insn[k], 0, 8) == reg && same(f, def, k, depth + 1)) continue;
                return 0;
            }
        }
        end = grown;
    }
}

/* A predicate's local definition, following a saved PSET boolean restored by
   ISETP.EQ/NE against RZ. Output predicates of a plain ISETP are complements. */
static int direct_pred_origin(const ShFlow* f, u32 index, u32 floor, u32 guard, u32* invert, int depth) {
    const u64* insn = f->insn;
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
                return direct_pred_origin(f, k, floor, B(v, 12, 4), invert, depth + 1);
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
static int direct_same_guard(const ShFlow* f, u32 floor, u32 definition, u32 use) {
    const u64* insn = f->insn;
    u32 ai = 0, bi = 0;
    int a = direct_pred_origin(f, definition, floor, B(insn[definition], 16, 4), &ai, 0);
    int b = direct_pred_origin(f, use, floor, B(insn[use], 16, 4), &bi, 0);
    if (a == -1 || b == -1 || ai != bi) return 0;
    if (a == b) return 1;
    if (a < 0 || b < 0) return 0;
    if ((insn[a] & ~63ull) != (insn[b] & ~63ull)) return 0;
    const Op* op = f->op[a];
    u32 lhs = B(insn[a], 8, 8), rhs = op->id == O_ISETP_R ? B(insn[a], 20, 8) : 255;
    u32 lo = (u32)(a < b ? a : b), hi = (u32)(a > b ? a : b);
    for (u32 k = lo + 1; k < hi; k++) {
        if (!IDX_OK(k)) continue;
        const Op* mid = f->op[k];
        if (!mid || (lhs != 255 && shader_writes_reg(insn[k], mid->id, lhs)) ||
            (rhs != 255 && shader_writes_reg(insn[k], mid->id, rhs)))
            return 0;
    }
    return 1;
}

/* Whether pred changes between lo and hi: a requirement's value at one end describes the other only while it
   doesn't. */
static int direct_pred_written(const ShFlow* f, u32 lo, u32 hi, u32 pred) {
    const u64* insn = f->insn;
    u32 k;
    for (k = lo + 1; k < hi; k++) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = f->op[k];
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
static int direct_can_reach(const ShFlow* f, u32 from, u32 use) {
    const u64* insn = f->insn;
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
            const u32* targets;
            const u8* elsewhere;
            int count;
            int t;
            int fall = 1;
            if (!(seen[k >> 3] >> (k & 7) & 1)) continue;
            if (!IDX_OK(k)) {
                changed |= direct_mark(seen, use, k + 1);
                continue;
            }
            op = f->op[k];
            if (!op) return -1;
            switch (op->id) {
                /* Lanes that do not take a conditional exit keep their register definitions. */
                case O_EXIT:
                case O_KIL: fall = !flow_always(insn[k]); break;
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
            count = direct_flow_targets(f, k, &targets, &elsewhere);
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
static u32 direct_block_start(const ShFlow* f, u32 index) {
    u32 start = 11;
    for (u32 i = 0; i < f->nflow; i++) {
        const u32* targets;
        const u8* elsewhere;
        int count;
        int t;
        count = direct_flow_targets(f, f->flow[i], &targets, &elsewhere);
        if (count < 0) return start;
        for (t = 0; t < count; t++)
            if (targets[t] > start && targets[t] <= index) start = targets[t];
    }
    return start;
}

/* Whether control reaches use only through def and only forwards from it: no branch enters (def, use] from before def
   or from after it, and none in the region jumps back. A branch in between then tests a predicate def also saw, with
   no earlier pass through the region to change it. */
static int direct_forward_only(const ShFlow* f, u32 def, u32 use) {
    if (f->bad < f->n) return 0; /* an instruction decode doesn't know may go anywhere */
    for (u32 i = 0; i < f->nflow; i++) {
        const u32* targets;
        const u8* elsewhere;
        int count;
        int t;
        u32 k = f->flow[i];
        const Op* op = f->op[k];
        count = direct_flow_targets(f, k, &targets, &elsewhere);
        if (count < 0) {
            if (direct_flow_below(f, k, use)) continue;
            return 0;
        }
        for (t = 0; t < count; t++) {
            if (targets[t] <= def || targets[t] > use) continue;
            if (elsewhere[t]) return 0;
            if (k < def) {
                if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(f, k, def)) continue;
                return 0;
            }
            if (targets[t] <= k) return 0;
        }
    }
    return 1;
}

/* Whether every path to use passes the branch at from: a branch before from that lands past it is a way around. The
   edges in (def, use] all run forwards when direct_forward_only passed, so one scan is enough. */
static int direct_branch_dominates(const ShFlow* f, u32 from, u32 use) {
    if (f->bad < f->n) return 0; /* an instruction decode doesn't know may go anywhere */
    for (u32 i = 0; i < f->nflow; i++) {
        const u32* targets;
        const u8* elsewhere;
        int count;
        int t;
        u32 k = f->flow[i];
        const Op* op = f->op[k];
        count = direct_flow_targets(f, k, &targets, &elsewhere);
        if (count < 0) {
            if (direct_flow_below(f, k, use)) continue;
            return 0;
        }
        for (t = 0; t < count; t++) {
            if (targets[t] <= from || targets[t] > use) continue;
            if (elsewhere[t]) return 0;
            if (k < from) {
                if ((op->id == O_SSY || op->id == O_PBK) && direct_straight(f, k, from)) continue;
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
static int direct_guard_under(const ShFlow* f, u32 index, u32 floor, u32 guard, u32 known, u32 value, int depth) {
    const u64* insn = f->insn;
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
        if ((known >> pa & 1) && direct_pred_written(f, j, index, pa)) ka &= ~(1u << pa);
        if ((known >> pb & 1) && direct_pred_written(f, j, index, pb)) kb &= ~(1u << pb);
        if ((known >> pc & 1) && direct_pred_written(f, j, index, pc)) kc &= ~(1u << pc);
        va = direct_guard_under(f, j, floor, pa | B(w, 15, 1) << 3, ka, value, depth + 1);
        vb = direct_guard_under(f, j, floor, pb | B(w, 32, 1) << 3, kb, value, depth + 1);
        vc = direct_guard_under(f, j, floor, pc | B(w, 42, 1) << 3, kc, value, depth + 1);
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
static int direct_guard_implied(const ShFlow* f, u32 def, u32 use) {
    const u64* insn = f->insn;
    u32 known = 0, value = 0;
    u32 floor = direct_block_start(f, def);
    u32 b;
    if (!direct_forward_only(f, def, use)) return 0;
    /* the predicate setters resolved below have to run on every path to def */
    if (!direct_straight(f, floor, def)) return 0;
    for (b = def + 1; b < use; b++) {
        const Op* op;
        u32 g, pred, target;
        int taken, fall, val;
        if (!IDX_OK(b)) continue;
        op = f->op[b];
        if (!op) return 0;
        if (op->id != O_BRA) continue;
        g = B(insn[b], 16, 4);
        if ((g & 7) == 7) continue;
        if (!direct_branch_dominates(f, b, use)) continue;
        target = bra_target(insn[b], b);
        taken = direct_can_reach(f, target, use);
        fall = direct_can_reach(f, b + 1, use);
        if (taken < 0 || fall < 0) return 0;
        if (taken == fall) continue; /* both ways reach use: the branch doesn't force its predicate */
        /* the branch is taken when the predicate is the negation bit's opposite */
        pred = g & 7;
        val = taken ? !(int)(g >> 3) : (int)(g >> 3);
        if (direct_pred_written(f, def, b, pred)) continue;
        if (known >> pred & 1) {
            if ((int)(value >> pred & 1) != val) return 0;
            continue;
        }
        known |= 1u << pred;
        if (val) value |= 1u << pred;
    }
    if (!known) return 0;
    return direct_guard_under(f, def, floor, B(insn[def], 16, 4), known, value, 0) == 1;
}

/* Whether a write under a predicate is followed, before the predicate changes or other flow, by a branch under
   want (that predicate or its complement) to where control doesn't reach use: under the same predicate the write's
   lanes all leave, so its value never arrives at use; under the complement the lanes that skipped it leave, so every
   lane that goes on to use made it */
static int direct_guard_branch(const ShFlow* f, u32 def, u32 use, u32 want) {
    const u64* insn = f->insn;
    u32 guard = B(insn[def], 16, 4);
    if ((guard & 7) == 7) return 0;
    for (u32 m = def + 1; m < use; m++) {
        const Op* op;
        const u32* targets;
        const u8* elsewhere;
        int count;
        if (!IDX_OK(m)) continue;
        op = f->op[m];
        if (!op || shader_direct_pred_writes(insn[m], op->id) >> (guard & 7) & 1) return 0;
        if (op->id == O_BRA && B(insn[m], 16, 4) == want && B(insn[m], 0, 5) == 15) {
            count = direct_flow_targets(f, m, &targets, &elsewhere);
            if (count <= 0) return 0;
            for (int t = 0; t < count; t++)
                if (elsewhere[t] || direct_can_reach(f, targets[t], use) != 0) return 0;
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
static u64 direct_reaching_writes(const ShFlow* f, u32 use, u32 reg, u32* defs, u32* ndefs) {
    const u64* insn = f->insn;
    u32 n = f->n;
    u64* in;
    u64 reach;
    int changed = 1;
    *ndefs = 0;
    if (n > DIRECT_WORDS_MAX || use >= n) return 0;
    for (u32 k = 11; k < n; k++) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = f->op[k];
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
            const u32* targets;
            const u8* elsewhere;
            int count, fall = 1;
            u64 out = in[k];
            if (!IDX_OK(k)) {
                changed |= (in[k + 1] | out) != in[k + 1];
                in[k + 1] |= out;
                continue;
            }
            op = f->op[k];
            while (d < *ndefs && defs[d] < k) d++;
            if (d < *ndefs && defs[d] == k) out = (B(insn[k], 16, 4) == 7 ? 0 : out) | 1ull << d;
            switch (op->id) {
                case O_EXIT:
                case O_KIL: fall = !flow_always(insn[k]); break;
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
            u32 sync_target;
            static const u8 sync_elsewhere = 0;
            count = direct_flow_targets(f, k, &targets, &elsewhere);
            /* an SSY carries no values to its target: each SYNC takes its own there, to the target of the innermost
               SSY still open (the nearest before it whose target is past it); a predicated one lets the others on */
            if (op->id == O_SSY) count = 0;
            if (op->id == O_SYNC) {
                count = -1;
                for (u32 m = k; m-- > 11 && count < 0;) {
                    if (!IDX_OK(m) || f->op[m]->id != O_SSY || bra_target(insn[m], m) <= k) continue;
                    sync_target = bra_target(insn[m], m);
                    targets = &sync_target;
                    elsewhere = &sync_elsewhere;
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
static int direct_all_defs_same(const ShFlow* f, u32 def, u32 use, u32 reg, int (*same)(const ShFlow*, u32, u32, int),
                                int depth) {
    const u64* insn = f->insn;
    u32 defs[63], ndefs;
    u64 reach;
    int covered;
    if (depth > 6) return 0;
    reach = direct_reaching_writes(f, use, reg, defs, &ndefs);
    if (!reach) return 0;
    covered = !(reach >> 63 & 1);
    for (u32 d = 0; d < ndefs; d++) {
        u32 k = defs[d], guard = B(insn[k], 16, 4);
        if (!(reach >> d & 1)) continue;
        if (guard != 7 && direct_guard_branch(f, k, use, guard)) continue;
        if (B(insn[k], 0, 8) != reg || (k != def && !same(f, def, k, depth + 1))) return 0;
        if (!covered && k < use && (guard == 7 || direct_guard_branch(f, k, use, guard ^ 8)))
            covered = direct_def_dominates(f, k, use, reg, same, depth);
    }
    return covered;
}

/* The instruction that last wrote reg before index, as its first destination, or -1. In index's basic block, a
   write under a predicate reaches only an instruction under the same one, with that predicate not written between
   them (others may be): the compiler loads a handle and uses it under the guard of one side of a branch it made
   predicated. Across flow, the write has to be unconditional and dominate index. */
static int direct_reaching_def_same(const ShFlow* f, u32 index, u32 reg, int (*same)(const ShFlow*, u32, u32, int),
                                    int depth) {
    const u64* insn = f->insn;
    u32 guard, pred_written = 0;
    int crossed = 0;
    u32 j;
    if (reg == 255 || index > f->known_to) return -1;
    assert(index < f->n);
    /* where index's basic block starts: the last place control can arrive at before it */
    u32 block_start = f->block_start[index];
    guard = B(insn[index], 16, 4);
    for (j = index; j-- > 11;) {
        const Op* op;
        if (!IDX_OK(j)) continue;
        op = f->op[j];
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
        if (j < block_start && direct_can_reach(f, j + 1, index) == 0) continue;
        /* A write under the complement of index's guard, the predicate kept since, runs in none of index's lanes */
        if ((guard & 7) != 7 && B(insn[j], 16, 4) == (guard ^ 8) && !crossed && !(pred_written >> (guard & 7) & 1))
            continue;
        /* A write that reaches reg from a lower destination leaves no single source. */
        if (B(insn[j], 0, 8) != reg) return -1;
        if ((B(insn[j], 16, 4) != 7 && (crossed || B(insn[j], 16, 4) != guard || (pred_written >> (guard & 7) & 1)) &&
             !direct_same_guard(f, block_start, j, index) && !direct_guard_implied(f, j, index)) ||
            (crossed && !direct_def_dominates(f, j, index, reg, same, depth)))
            return same && direct_all_defs_same(f, j, index, reg, same, depth) ? (int)j : -1;
        return (int)j;
    }
    return -1;
}

static int direct_reaching_def(const ShFlow* f, u32 index, u32 reg) {
    return direct_reaching_def_same(f, index, reg, NULL, 0);
}

/* reg at index holds the fragment's own column (addr 0x70) or row (0x74): the integer part of gl_FragCoord's x or y,
   F2I.FLOOR or .TRUNC of IPA's pass-through read of the position */
static int direct_frag_coord(const ShFlow* f, u32 index, u32 reg, u32 addr, int depth) {
    const u64* insn = f->insn;
    int j = depth > 4 ? -1 : direct_reaching_def(f, index, reg);
    if (j < 0) return 0;
    u64 w = insn[j];
    int op = decode(w)->id;
    if (op == O_MOV_R) return direct_frag_coord(f, (u32)j, B(w, 20, 8), addr, depth + 1);
    /* F32 source, no negate or absolute value, rounding down or toward zero */
    if (op != O_F2I_R || B(w, 10, 2) != 2 || B(w, 45, 1) || B(w, 49, 1) || (B(w, 39, 2) != 1 && B(w, 39, 2) != 3))
        return 0;
    int k = direct_reaching_def(f, (u32)j, B(w, 20, 8));
    if (k < 0) return 0;
    u64 v = insn[k];
    /* not indexed, not multiplied by 1/w, not saturated */
    return decode(v)->id == O_IPA && !B(v, 38, 1) && B(v, 30, 8) * 4 == addr && B(v, 54, 2) != 1 && !B(v, 51, 1);
}
/* the texel fetch at index reads the fragment's own pixel of a 2D texture: level 0, no offset, single sample */
int shader_direct_own_pixel_fetch(const ShFlow* f, u32 index) {
    const u64* insn = f->insn;
    u64 w = insn[index];
    int op = decode(w)->id;
    u32 a = B(w, 8, 8), b = B(w, 20, 8);
    if (op == O_TLDS) /* encoding 2: 2D, level zero, x in Ra and y in Rb */
        return B(w, 53, 4) == 2 && direct_frag_coord(f, index, a, 0x70, 0) && direct_frag_coord(f, index, b, 0x74, 0);
    if (op == O_TLD) /* 2D, not an array, no level (bit 55), offset (35) or sample (50): x in Ra, y in Ra + 1 */
        return B(w, 28, 3) == 2 && !B(w, 55, 1) && !B(w, 35, 1) && !B(w, 50, 1) && a < 254 &&
               direct_frag_coord(f, index, a, 0x70, 0) && direct_frag_coord(f, index, a + 1, 0x74, 0);
    return 0;
}

/* reg at index holds an IPA of the generic attribute word at addr, not indexed nor saturated, in pass mode or multiplied
   by a register: *mul gets the multiplier's value, its definition times 256 plus the register (one instruction may
   write several), -1 in pass mode, so that two reads can be told to interpolate alike. emit_io.c interpolates every
   IPA at the pixel's centre */
static int direct_ipa_at(const ShFlow* f, u32 index, u32 reg, u32 addr, int* mul, int depth) {
    const u64* insn = f->insn;
    int j = depth > 4 ? -1 : direct_reaching_def(f, index, reg);
    if (j < 0) return 0;
    u64 w = insn[j];
    int op = decode(w)->id;
    if (op == O_MOV_R) return direct_ipa_at(f, (u32)j, B(w, 20, 8), addr, mul, depth + 1);
    if (op != O_IPA || B(w, 38, 1) || B(w, 51, 1) || B(w, 30, 8) * 4 != addr || B(w, 54, 2) > 1) return 0;
    int def = B(w, 54, 2) == 1 && B(w, 20, 8) < 255 ? direct_reaching_def(f, (u32)j, B(w, 20, 8)) : -1;
    *mul = def >= 0 ? def * 256 + (int)B(w, 20, 8) : -1;
    return B(w, 54, 2) == 0 || *mul >= 0;
}
/* reg at index holds the reciprocal of a generic's w (MUFU.RCP of its IPA, no absolute value, negation or
   saturation): the generic's index, and in *mul its IPA's multiplier (direct_ipa_at); -1 when it isn't */
static int direct_rcp_w(const ShFlow* f, u32 index, u32 reg, int* mul, int depth) {
    const u64* insn = f->insn;
    int j = depth > 4 ? -1 : direct_reaching_def(f, index, reg);
    if (j < 0) return -1;
    u64 w = insn[j];
    int op = decode(w)->id;
    if (op == O_MOV_R) return direct_rcp_w(f, (u32)j, B(w, 20, 8), mul, depth + 1);
    if (op != O_MUFU || B(w, 20, 4) != 4 || B(w, 46, 1) || B(w, 48, 1) || B(w, 50, 1)) return -1;
    int k = direct_reaching_def(f, (u32)j, B(w, 8, 8));
    if (k < 0) return -1;
    u64 v = insn[k];
    if (decode(v)->id != O_IPA || B(v, 38, 1)) return -1;
    u32 addr = B(v, 30, 8) * 4;
    if (addr < 0x80 || addr >= 0x280 || (addr & 15) != 12) return -1;
    return direct_ipa_at(f, (u32)j, B(w, 8, 8), addr, mul, 0) ? (int)((addr - 0x80) >> 4) : -1;
}
/* reg at index holds a generic's component c (0 x, 1 y) divided by its w: FMUL, no negation, scale or saturation, of
   that component's IPA and the reciprocal of the w's IPA, both interpolated alike. The generic's index, or -1 */
static int direct_over_w(const ShFlow* f, u32 index, u32 reg, u32 c, int depth) {
    const u64* insn = f->insn;
    int j = depth > 4 ? -1 : direct_reaching_def(f, index, reg);
    if (j < 0) return -1;
    u64 w = insn[j];
    int op = decode(w)->id;
    if (op == O_MOV_R) return direct_over_w(f, (u32)j, B(w, 20, 8), c, depth + 1);
    if (op != O_FMUL_R || B(w, 48, 1) || B(w, 41, 3) || B(w, 50, 1)) return -1;
    u32 ops[2] = {B(w, 8, 8), B(w, 20, 8)};
    for (int side = 0; side < 2; side++) {
        int rmul, vmul;
        int g = direct_rcp_w(f, (u32)j, ops[side], &rmul, 0);
        if (g >= 0 && direct_ipa_at(f, (u32)j, ops[!side], 0x80 + (u32)g * 16 + c * 4, &vmul, 0) && vmul == rmul)
            return g;
    }
    return -1;
}
/* The texture read at index samples at (x, y) = a generic's (x / w, y / w), the same generic for both: the fragment's
   own pixel when the vertex stage wrote that generic as its position mapped onto the target, (pos.xy * (1, +-1) +
   pos.w) / 2 with pos.w as its w (ShaderInfo.screen_out_mask), and the draw's viewport covers the target the texture
   is (vk/render.c). The generic's index, or -1 */
int shader_direct_screen_uv(const ShFlow* f, u32 index, u32 x, u32 y) {
    int g = x < 255 ? direct_over_w(f, index, x, 0, 0) : -1;
    return g >= 0 && y < 255 && direct_over_w(f, index, y, 1, 0) == g ? g : -1;
}
/* reg at index holds a generic's component c divided by its w (direct_over_w) times +-1/2 plus 1/2: an FFMA with no
   saturation, its factor 1/2 or -1/2 (an immediate, or a register a MOV32I sets) and its addend 1/2 (a register a
   MOV32I sets), as a program maps the position itself from clip space onto [0, 1]. The generic's index, with *neg
   set for the factor -1/2, or -1 */
#define HALF_BITS 0x3f000000u
static int direct_over_w_half(const ShFlow* f, u32 index, u32 reg, u32 c, int* neg, int depth) {
    const u64* insn = f->insn;
    int j = depth > 4 ? -1 : direct_reaching_def(f, index, reg);
    if (j < 0) return -1;
    u64 w = insn[j];
    int op = decode(w)->id;
    if (op == O_MOV_R) return direct_over_w_half(f, (u32)j, B(w, 20, 8), c, neg, depth + 1);
    u32 factor, addend, d = B(w, 0, 8);
    if (op == O_FFMA_I) {
        factor = (u32)(B(w, 20, 19) << 12 | B(w, 56, 1) << 31) ^ (u32)B(w, 48, 1) << 31;
        if (B(w, 50, 1) || B(w, 49, 1) || !shader_direct_track_const(f, (u32)j, B(w, 39, 8), &addend, 0)) return -1;
    } else if (op == O_FFMA_R) {
        if (B(w, 50, 1) || B(w, 49, 1) || !shader_direct_track_const(f, (u32)j, B(w, 20, 8), &factor, 0) ||
            !shader_direct_track_const(f, (u32)j, B(w, 39, 8), &addend, 0))
            return -1;
        factor ^= (u32)B(w, 48, 1) << 31;
    } else if (op == O_FFMA32I) { /* the addend is the destination register's value before it */
        factor = (u32)B(w, 20, 32) ^ (u32)B(w, 56, 1) << 31;
        if (B(w, 55, 1) || B(w, 57, 1) || !shader_direct_track_const(f, (u32)j, d, &addend, 0)) return -1;
    } else
        return -1;
    if (addend != HALF_BITS || (factor & 0x7fffffffu) != HALF_BITS) return -1;
    *neg = (int)(factor >> 31);
    return direct_over_w(f, (u32)j, B(w, 8, 8), c, 0);
}
/* The texture read at index samples at (x, y) = a generic's (x / w / 2 + 1/2, +-y / w / 2 + 1/2), the same generic for
   both (direct_over_w_half): the fragment's own pixel's centre when the vertex stage wrote that generic as its
   position itself (ShaderInfo.pos_out_mask), the draw's viewport covers the target, and its height has the sign that
   maps y / w the way the program does (vk/render.c). The generic's index, with *yneg set for -y, or -1 */
int shader_direct_screen_ndc_uv(const ShFlow* f, u32 index, u32 x, u32 y, int* yneg) {
    int xneg = 1, g = x < 255 ? direct_over_w_half(f, index, x, 0, &xneg, 0) : -1;
    return g >= 0 && !xneg && y < 255 && direct_over_w_half(f, index, y, 1, yneg, 0) == g ? g : -1;
}

/* reg at index holds a generic's component c (0 x, 1 y) as the vertex stage wrote it, where the position's w is 1:
   IPA of that word, not indexed nor saturated, in pass mode, or multiplied by the reciprocal of the position's
   interpolated 1 / w (MUFU.RCP of IPA's pass-through read of a[0x7c], no absolute value, negation or saturation), the
   way the compiler reads a perspective input. The generic's index, or -1 */
static int direct_plain_generic(const ShFlow* f, u32 index, u32 reg, u32 c, int depth) {
    const u64* insn = f->insn;
    int j = depth > 4 ? -1 : direct_reaching_def(f, index, reg);
    if (j < 0) return -1;
    u64 w = insn[j];
    int op = decode(w)->id;
    if (op == O_MOV_R) return direct_plain_generic(f, (u32)j, B(w, 20, 8), c, depth + 1);
    u32 addr = B(w, 30, 8) * 4;
    if (op != O_IPA || B(w, 38, 1) || B(w, 51, 1) || B(w, 54, 2) > 1 || addr < 0x80 || addr >= 0x280 ||
        (addr & 15) != c * 4)
        return -1;
    if (B(w, 54, 2) == 1) {
        int k = direct_reaching_def(f, (u32)j, B(w, 20, 8));
        if (k < 0) return -1;
        u64 r = insn[k];
        if (decode(r)->id != O_MUFU || B(r, 20, 4) != 4 || B(r, 46, 1) || B(r, 48, 1) || B(r, 50, 1)) return -1;
        int m = direct_reaching_def(f, (u32)k, B(r, 8, 8));
        if (m < 0) return -1;
        u64 v = insn[m];
        if (decode(v)->id != O_IPA || B(v, 38, 1) || B(v, 51, 1) || B(v, 54, 2) || B(v, 30, 8) * 4 != 0x7c) return -1;
    }
    return (int)((addr - 0x80) >> 4);
}
/* The texture read at index samples at (x, y) = a generic's (x, y) as the vertex stage wrote them, the same generic
   for both: the fragment's own pixel's centre when the vertex stage wrote its position from that generic, mapping
   [0, 1] onto the viewport with a w of 1 (ShaderInfo.uv_out_mask), and the draw's viewport covers the target the
   texture is (vk/render.c). The generic's index, or -1 */
int shader_direct_plain_uv(const ShFlow* f, u32 index, u32 x, u32 y) {
    int g = x < 255 ? direct_plain_generic(f, index, x, 0, 0) : -1;
    return g >= 0 && y < 255 && direct_plain_generic(f, index, y, 1, 0) == g ? g : -1;
}

/* reg's value at index past full register moves: the instruction that makes it times 256 plus the register it
   writes, as one instruction may write several (ALD.128, a texture read). -1 when no single one reaches it */
static int direct_value_def(const ShFlow* f, u32 index, u32 reg) {
    const u64* insn = f->insn;
    for (int depth = 0; depth < 6; depth++) {
        int j = reg < 255 ? direct_reaching_def(f, index, reg) : -1;
        if (j < 0) return -1;
        if (f->op[j]->id != O_MOV_R || B(insn[j], 39, 4) != 0xf) return j * 256 + (int)reg;
        index = (u32)j;
        reg = B(insn[j], 20, 8);
    }
    return -1;
}
/* Whether every run of the program passes the instruction at index once: no EXIT, KIL or RET before it, no branch
   from before it that lands past it, none from after it back to it or before it. Its writes are the ones the program
   ends with, as long as nothing else writes there */
static int direct_once_to_end(const ShFlow* f, u32 index) {
    u32 n = f->n;
    for (u32 k = 11; k < n; k++) {
        const u32* targets;
        const u8* elsewhere;
        if (!IDX_OK(k)) continue;
        const Op* op = f->op[k];
        if (!op) return 0;
        if (k < index && (op->id == O_EXIT || op->id == O_KIL || op->id == O_RET)) return 0;
        int count = direct_flow_targets(f, k, &targets, &elsewhere);
        if (count < 0) {
            if (direct_flow_below(f, k, index + 1)) continue;
            return 0;
        }
        for (int t = 0; t < count; t++) {
            if (k > index && targets[t] <= index) return 0;
            /* an SSY or PBK only names where its SYNC or BRK goes, and none comes between it and index */
            if (k < index && targets[t] > index &&
                !((op->id == O_SSY || op->id == O_PBK) && direct_straight(f, k, index)))
                return 0;
        }
    }
    return 1;
}
/* the last store of each output word under 0x280 (by its address / 4), an AST not per patch, or -1 */
static void direct_output_stores(const ShFlow* f, int store[0x280 / 4]) {
    const u64* insn = f->insn;
    u32 n = f->n;
    for (u32 a = 0; a < 0x280 / 4; a++) store[a] = -1;
    for (u32 k = 11; k < n; k++) {
        u64 w = insn[k];
        /* AST's top 13 bits (maxwell_opcodes.def): another instruction isn't decoded */
        if (!IDX_OK(k) || w >> 51 != 0x1DFE) continue;
        const Op* op = decode(w);
        if (!op || op->id != O_AST || B(w, 31, 1)) continue;
        u32 base = B(w, 20, 10), end = base + (B(w, 47, 2) + 1) * 4;
        for (u32 addr = (base + 3) & ~3u; addr < end && addr < 0x280; addr += 4) store[addr / 4] = (int)k;
    }
}
/* The value (direct_value_def) the vertex program's output word at addr ends with: its last store's (store, from
   direct_output_stores), an AST with no guard, not per patch nor indexed, that every run passes once
   (direct_once_to_end), so no store before it can come after it. -1 when there isn't one */
static int direct_output_def(const ShFlow* f, const int store[0x280 / 4], u32 addr) {
    const u64* insn = f->insn;
    int k = store[addr / 4];
    if (k < 0) return -1;
    u64 w = insn[k];
    u32 reg = B(w, 0, 8) + (addr - B(w, 20, 10)) / 4;
    if (B(w, 16, 4) != 7 || B(w, 8, 8) != 255 || reg >= 255 || !direct_once_to_end(f, (u32)k)) return -1;
    return direct_value_def(f, (u32)k, reg);
}
/* The value (direct_value_def) is half of another: FMUL or FMUL32I by the immediate 0.5, with no negation, scale or
   saturation. That other value, or -1 */
static int direct_half_of(const ShFlow* f, int value) {
    const u64* insn = f->insn;
    if (value < 0) return -1;
    u32 def = (u32)value / 256;
    u64 w = insn[def];
    int op = decode(w)->id;
    if (op == O_FMUL_I) {
        if (((B(w, 20, 19) << 12) | (B(w, 56, 1) << 31)) != 0x3f000000u || B(w, 48, 1) || B(w, 41, 3) || B(w, 50, 1))
            return -1;
    } else if (op != O_FMUL32I || B(w, 20, 32) != 0x3f000000u || B(w, 55, 1))
        return -1;
    return direct_value_def(f, def, B(w, 8, 8));
}
/* The value (direct_value_def) is the sum of the values p and q, FADD with no absolute value or saturation and p not
   negated, in either order: 1 for p + q, 2 for p - q, 0 when it is neither */
static int direct_sum_of(const ShFlow* f, int value, int p, int q) {
    const u64* insn = f->insn;
    if (value < 0) return 0;
    u32 def = (u32)value / 256;
    u64 w = insn[def];
    if (decode(w)->id != O_FADD_R || B(w, 46, 1) || B(w, 49, 1) || B(w, 50, 1)) return 0;
    int left = direct_value_def(f, def, B(w, 8, 8)), right = direct_value_def(f, def, B(w, 20, 8));
    if (left == p && right == q && !B(w, 48, 1)) return B(w, 45, 1) ? 2 : 1;
    if (left == q && right == p && !B(w, 45, 1)) return B(w, 48, 1) ? 2 : 1;
    return 0;
}
/* The generic outputs the vertex program writes as its position mapped onto [0, 1]: x = (pos.w + pos.x) / 2,
   y = (pos.w + pos.y) / 2 or (pos.w - pos.y) / 2 (its bit in *yneg) and w = pos.w, each of those words and pos.x, y
   and w written once (direct_output_def). A fragment's (x / w, y / w) of one is the centre of its own pixel in [0, 1]
   of the viewport, which vk/render.c checks covers the target with the y this sign maps */
u32 shader_direct_screen_out(const ShFlow* f, u32* yneg) {
    int store[0x280 / 4];
    direct_output_stores(f, store);
    int px = direct_output_def(f, store, 0x70), py = direct_output_def(f, store, 0x74);
    int pw = direct_output_def(f, store, 0x7c);
    u32 mask = 0;
    *yneg = 0;
    if (px < 0 || py < 0 || pw < 0) return 0;
    for (u32 g = 0; g < 32; g++) {
        u32 at = 0x80 + g * 16;
        if (direct_output_def(f, store, at + 12) != pw ||
            direct_sum_of(f, direct_half_of(f, direct_output_def(f, store, at)), pw, px) != 1)
            continue;
        int sign = direct_sum_of(f, direct_half_of(f, direct_output_def(f, store, at + 4)), pw, py);
        if (!sign) continue;
        mask |= 1u << g;
        if (sign == 2) *yneg |= 1u << g;
    }
    return mask;
}

/* The generic outputs the vertex program writes as its position: x, y and w the very values the position's x, y and w
   end with, each of those words written once (direct_output_def). Interpolated with perspective and divided by its
   w, one is the fragment's own place in the viewport, which the fragment program can take from gl_FragCoord
   (ShaderInfo.pos_out_mask) */
u32 shader_direct_position_out(const ShFlow* f) {
    int store[0x280 / 4];
    direct_output_stores(f, store);
    int px = direct_output_def(f, store, 0x70), py = direct_output_def(f, store, 0x74);
    int pw = direct_output_def(f, store, 0x7c);
    u32 mask = 0;
    if (px < 0 || py < 0 || pw < 0) return 0;
    for (u32 g = 0; g < 32; g++) {
        u32 at = 0x80 + g * 16;
        if (direct_output_def(f, store, at) == px && direct_output_def(f, store, at + 4) == py &&
            direct_output_def(f, store, at + 12) == pw)
            mask |= 1u << g;
    }
    return mask;
}

/* The value (direct_value_def) is another's, src, mapped from [0, 1] onto clip space's [-1, 1]: FFMA by an immediate
   with a constant addend, unguarded and unsaturated. 1 for src * 2 - 1, 2 for src * -2 + 1 (the y of a viewport of
   negative height), 0 when it is neither: any other scale or offset puts the value elsewhere than its own pixel */
static int direct_clip_of(const ShFlow* f, int value, int src) {
    const u64* insn = f->insn;
    if (value < 0 || src < 0) return 0;
    u32 def = (u32)value / 256, addend;
    u64 w = insn[def];
    if (decode(w)->id != O_FFMA_I || B(w, 16, 4) != 7 || B(w, 50, 1) || direct_value_def(f, def, B(w, 8, 8)) != src ||
        !shader_direct_track_const(f, def, B(w, 39, 8), &addend, 0))
        return 0;
    u32 scale = ((B(w, 20, 19) << 12) | (B(w, 56, 1) << 31)) ^ (B(w, 48, 1) << 31);
    addend ^= B(w, 49, 1) << 31;
    return scale == 0x40000000u && addend == 0xbf800000u ? 1 : scale == 0xc0000000u && addend == 0x3f800000u ? 2 : 0;
}
/* The value the vertex program's output word at addr ends with, as direct_output_def gives it, or, where two writes
   of its register reach its store, the unguarded one's (the later of the two), with the guarded one in *early: the
   shape of a program that begins by writing its outputs for a vertex it drops, under a predicate, and leaves
   (direct_early_out says whether it is that). *early is -1 for the one value; the result -1 for anything else */
static int direct_output_or_early(const ShFlow* f, const int store[0x280 / 4], u32 addr, int* early) {
    const u64* insn = f->insn;
    int k = store[addr / 4], value = direct_output_def(f, store, addr);
    *early = -1;
    if (value >= 0 || k < 0) return value;
    u64 w = insn[k];
    u32 reg = B(w, 0, 8) + (addr - B(w, 20, 10)) / 4, defs[63], ndefs, last = 0, unguarded = 0, guarded = 0;
    if (B(w, 16, 4) != 7 || B(w, 8, 8) != 255 || reg >= 255 || !direct_once_to_end(f, (u32)k)) return -1;
    u64 reach = direct_reaching_writes(f, (u32)k, reg, defs, &ndefs);
    for (u32 d = 0; d < ndefs; d++) {
        if (!(reach >> d & 1)) continue;
        if (B(insn[defs[d]], 0, 8) != reg) return -1;
        if (B(insn[defs[d]], 16, 4) == 7) {
            last = defs[d];
            unguarded++;
        } else {
            *early = (int)defs[d];
            guarded++;
        }
    }
    if (unguarded != 1 || guarded != 1 || (u32)*early > last) return -1;
    return (int)(last * 256 + reg);
}
/* Whether the guarded writes in early (the position's x first) and the unguarded ones from first to last
   (direct_output_or_early) are the two ways of a program that drops some vertices before the stores at use: under one
   predicate it writes the outputs of a dropped vertex and leaves by a SYNC under that predicate, which lands past
   last (nothing may arrive before) and no later than use (direct_once_to_end, of the stores), and every other vertex
   runs first to last whole. A dropped vertex is outside the clip volume, its x a constant
   beyond 1 with a w of 1, so the choice has to be one for a primitive's vertices, or a triangle of both kinds would
   interpolate between them: nothing a vertex differs by is read before the branch (an attribute load other than the
   instance's number), so every vertex of an instance takes the same way */
static int direct_early_out(const ShFlow* f, const int early[4], u32 first, u32 last, u32 use) {
    const u64* insn = f->insn;
    u32 guard = B(insn[early[0]], 16, 4), lo = (u32)early[0], hi = lo, branch = 0, x;
    float fx;
    if (f->bad < f->n || use > f->known_to) return 0;
    for (int e = 0; e < 4; e++) {
        if (B(insn[early[e]], 16, 4) != guard) return 0;
        if ((u32)early[e] < lo) lo = (u32)early[e];
        if ((u32)early[e] > hi) hi = (u32)early[e];
    }
    x = B(insn[early[0]], 20, 32);
    memcpy(&fx, &x, sizeof fx);
    if (f->op[early[0]]->id != O_MOV32I || !(fx > 1.0f || fx < -1.0f)) return 0;
    /* the guarded writes, then the SYNC under their guard, in one run with the predicate kept */
    for (u32 k = lo + 1; k < first && !branch; k++) {
        if (!IDX_OK(k)) continue;
        int id = f->op[k]->id;
        if (shader_direct_pred_writes(insn[k], id) >> (guard & 7) & 1) return 0;
        if (k > hi && id == O_SYNC && B(insn[k], 16, 4) == guard && B(insn[k], 0, 5) == 15)
            branch = k;
        else if (id == O_BRA || id == O_BRX || id == O_JMP || id == O_CAL || id == O_JCAL || id == O_RET ||
                 id == O_PRET || id == O_PEXIT || id == O_SYNC || id == O_BRK || id == O_CONT || id == O_EXIT ||
                 id == O_KIL || id == O_SSY || id == O_PBK || id == O_PCNT)
            return 0;
    }
    if (!branch) return 0;
    /* everything from the SYNC to last runs whole for a vertex that goes on: nothing leaves on the way */
    for (u32 k = branch + 1; k < last; k++) {
        if (!IDX_OK(k)) continue;
        int id = f->op[k]->id;
        if (id == O_BRA || id == O_BRX || id == O_JMP || id == O_CAL || id == O_JCAL || id == O_RET || id == O_PRET ||
            id == O_PEXIT || id == O_SYNC || id == O_BRK || id == O_CONT || id == O_EXIT || id == O_KIL)
            return 0;
    }
    for (u32 k = 11; k < use; k++) {
        const u32* targets;
        const u8* elsewhere;
        if (!IDX_OK(k)) continue;
        int id = f->op[k]->id;
        /* before the SYNC: no input a vertex differs by, and no way around it */
        if (k < branch && (id == O_AL2P || (id == O_ALD && (B(insn[k], 8, 8) != 255 || B(insn[k], 47, 2) ||
                                                            B(insn[k], 31, 1) || B(insn[k], 20, 10) != 0x2f8))))
            return 0;
        if (k < lo && (id == O_BRX || id == O_JMP || id == O_CAL || id == O_JCAL || id == O_RET || id == O_PRET ||
                       id == O_PEXIT || id == O_SYNC || id == O_BRK || id == O_CONT))
            return 0;
        int count = direct_flow_targets(f, k, &targets, &elsewhere);
        if (count < 0) return 0;
        for (int t = 0; t < count; t++) {
            if (elsewhere[t]) return 0;
            /* nothing arrives between the guarded writes and last but by falling from one to the next: not the
               SYNC's own vertices either, which the SSY before the writes sends past last */
            if (targets[t] > lo && targets[t] <= last) return 0;
            /* a branch from before the guarded writes to past them misses both ways; an SSY only names a place */
            if (k < lo && targets[t] > last && id != O_SSY && id != O_PBK && id != O_PCNT) return 0;
            if (k > branch && targets[t] <= k) return 0; /* and nothing comes back */
        }
    }
    return 1;
}
/* The generic outputs whose (x, y) the vertex program maps onto its position: pos.x = x * 2 - 1, pos.y = y * 2 - 1 or
   y * -2 + 1 (its bit in *yneg), computed from the very values the generic's x and y end with (direct_clip_of), with
   the position's w 1, never stored or stored as that constant; each word written once, or once for a vertex the
   program drops and once for the others (direct_early_out). With a w of 1 the generic interpolates as the position
   does, so a fragment's (x, y) of one is the centre of its own pixel in [0, 1] of the viewport, which vk/render.c
   checks covers the target with the y this sign maps */
u32 shader_direct_uv_out(const ShFlow* f, u32* yneg) {
    const u64* insn = f->insn;
    int store[0x280 / 4], early[4];
    u32 mask = 0;
    *yneg = 0;
    direct_output_stores(f, store);
    if (store[0x7c / 4] >= 0) {
        int pw = direct_output_def(f, store, 0x7c);
        if (pw < 0 || f->op[pw / 256]->id != O_MOV32I || B(insn[pw / 256], 20, 32) != 0x3f800000u ||
            B(insn[pw / 256], 16, 4) != 7)
            return 0;
    }
    int px = direct_output_or_early(f, store, 0x70, &early[0]), py = direct_output_or_early(f, store, 0x74, &early[1]);
    if (px < 0 || py < 0) return 0;
    for (u32 g = 0; g < 32; g++) {
        u32 at = 0x80 + g * 16;
        int gx = direct_output_or_early(f, store, at, &early[2]),
            gy = direct_output_or_early(f, store, at + 4, &early[3]);
        int sign = direct_clip_of(f, px, gx) == 1 ? direct_clip_of(f, py, gy) : 0;
        int guarded = (early[0] >= 0) + (early[1] >= 0) + (early[2] >= 0) + (early[3] >= 0);
        if (!sign || (guarded && guarded != 4)) continue;
        if (guarded) {
            const int value[4] = {px, py, gx, gy};
            u32 first = (u32)px / 256, last = first, use = (u32)store[0x70 / 4];
            for (int v = 1; v < 4; v++) {
                if ((u32)value[v] / 256 < first) first = (u32)value[v] / 256;
                if ((u32)value[v] / 256 > last) last = (u32)value[v] / 256;
            }
            const u32 stores[3] = {(u32)store[0x74 / 4], (u32)store[at / 4], (u32)store[(at + 4) / 4]};
            for (int v = 0; v < 3; v++)
                if (stores[v] < use) use = stores[v];
            if (!direct_early_out(f, early, first, last, use)) continue;
        }
        mask |= 1u << g;
        if (sign == 2) *yneg |= 1u << g;
    }
    return mask;
}

int shader_direct_track_const(const ShFlow* f, u32 index, u32 reg, u32* value, int depth) {
    const u64* insn = f->insn;
    int j = depth > 6 ? -1 : direct_reaching_def(f, index, reg);
    u64 w;
    int op;
    if (j < 0) return 0;
    w = insn[j];
    op = decode(w)->id;
    if (op == O_MOV32I) {
        *value = B(w, 20, 32);
        return 1;
    }
    if (op == O_MOV_R) return shader_direct_track_const(f, (u32)j, B(w, 20, 8), value, depth + 1);
    return 0;
}

typedef struct {
    u32 known, value;
} KnownBits;
static KnownBits direct_known_bits(const ShFlow* f, u32 index, u32 reg, int depth);
/* The integer right operand's domain. Constant-buffer contents are deliberately not read: a proof about the
   instruction word must hold for every draw sharing this module. A caller can still specialize a whole fixed word. */
static KnownBits direct_known_right(const ShFlow* f, u32 index, u64 w, int kind, int depth) {
    if (!kind) return direct_known_bits(f, index, B(w, 20, 8), depth + 1);
    if (kind == 1) return (KnownBits){0};
    u32 value = B(w, 20, 19);
    return (KnownBits){~0u, B(w, 56, 1) ? value | 0xfff80000u : value};
}
static KnownBits direct_known_bits(const ShFlow* f, u32 index, u32 reg, int depth) {
    const u64* insn = f->insn;
    u32 n = f->n;
    if (reg == 255) return (KnownBits){~0u, 0};
    if (depth > 6 || index >= n) return (KnownBits){0};
    int definition = direct_reaching_def(f, index, reg);
    if (definition < 0) return (KnownBits){0};
    u32 at = (u32)definition;
    u64 w = insn[at];
    /* A skipped write preserves the old register. Even a matching use guard is left to the existing whole-value
       trackers; this small domain analysis only propagates unconditional definitions proved to reach the use. */
    if (B(w, 16, 4) != 7) return (KnownBits){0};
    int op = decode(w)->id;
    if (op == O_MOV32I) return (KnownBits){~0u, B(w, 20, 32)};
    if (op == O_MOV_R) return direct_known_bits(f, at, B(w, 20, 8), depth + 1);
    u32 source = B(w, 8, 8);
    if (op == O_LOP32I || op == O_LOP_R || op == O_LOP_C || op == O_LOP_I) {
        KnownBits left = direct_known_bits(f, at, source, depth + 1);
        KnownBits right =
            op == O_LOP32I ? (KnownBits){~0u, B(w, 20, 32)} : direct_known_right(f, at, w, op - O_LOP_R, depth);
        if (B(w, op == O_LOP32I ? 55 : 39, 1)) left.value ^= left.known;
        if (B(w, op == O_LOP32I ? 56 : 40, 1)) right.value ^= right.known;
        u32 mode = B(w, op == O_LOP32I ? 53 : 41, 2), ones, zeros;
        switch (mode) {
            case 0:
                ones = left.value & right.value;
                zeros = (left.known & ~left.value) | (right.known & ~right.value);
                return (KnownBits){ones | zeros, ones};
            case 1:
                ones = left.value | right.value;
                zeros = (left.known & ~left.value) & (right.known & ~right.value);
                return (KnownBits){ones | zeros, ones};
            case 2:
                return (KnownBits){left.known & right.known, (left.value ^ right.value) & left.known & right.known};
            default: return right;
        }
    }
    if (op == O_SHL_R || op == O_SHL_C || op == O_SHL_I || op == O_SHR_R || op == O_SHR_C || op == O_SHR_I) {
        int right_shift = op == O_SHR_R || op == O_SHR_C || op == O_SHR_I;
        if ((right_shift ? B(w, 43, 2) || B(w, 40, 1) : B(w, 43, 1)) || B(w, 47, 1))
            return (KnownBits){0};
        KnownBits control = direct_known_right(f, at, w, op - (right_shift ? O_SHR_R : O_SHL_R), depth);
        if (control.known != ~0u) return (KnownBits){0};
        u32 shift = control.value;
        if (B(w, 39, 1)) shift &= 31;
        else if (shift >= 32) {
            if (!right_shift || !B(w, 48, 1)) return (KnownBits){~0u, 0};
            shift = 31;
        }
        KnownBits input = direct_known_bits(f, at, source, depth + 1);
        if (!shift) return input;
        if (!right_shift)
            return (KnownBits){input.known << shift | ((1u << shift) - 1), input.value << shift};
        u32 high = ~0u << (32 - shift), known = input.known >> shift, value = input.value >> shift;
        if (!B(w, 48, 1)) known |= high;
        else if (input.known & 0x80000000u) {
            known |= high;
            if (input.value & 0x80000000u) value |= high;
        }
        return (KnownBits){known, value};
    }
    if (op == O_BFE_R || op == O_BFE_C || op == O_BFE_I) {
        if (B(w, 40, 1)) return (KnownBits){0}; /* reversed source */
        KnownBits control = direct_known_right(f, at, w, op - O_BFE_R, depth);
        if ((control.known & 0xffffu) != 0xffffu) return (KnownBits){0};
        u32 offset = control.value & 255, width = control.value >> 8 & 255;
        int sign = B(w, 48, 1);
        if (!sign && offset >= 32) return (KnownBits){~0u, 0};
        if (offset > 31) offset = 31;
        if (width > 32 - offset) width = 32 - offset;
        if (!width) return (KnownBits){~0u, 0};
        KnownBits input = direct_known_bits(f, at, source, depth + 1);
        u32 low = width == 32 ? ~0u : (1u << width) - 1;
        u32 known = (input.known >> offset) & low, value = (input.value >> offset) & low;
        if (!sign) known |= ~low;
        else if (known & (1u << (width - 1))) {
            known |= ~low;
            if (value & (1u << (width - 1))) value |= ~low;
        }
        return (KnownBits){known, value};
    }
    if (op == O_BFI_R || op == O_BFI_I) {
        KnownBits control = direct_known_right(f, at, w, op == O_BFI_R ? 0 : 2, depth);
        if ((control.known & 0xffffu) != 0xffffu) return (KnownBits){0};
        KnownBits base = direct_known_bits(f, at, B(w, 39, 8), depth + 1);
        u32 offset = control.value & 255, width = control.value >> 8 & 255;
        if (offset >= 32 || !width) return base;
        if (width > 32 - offset) width = 32 - offset;
        KnownBits input = direct_known_bits(f, at, source, depth + 1);
        u32 field = (width == 32 ? ~0u : (1u << width) - 1) << offset;
        return (KnownBits){(base.known & ~field) | (input.known << offset & field),
                            (base.value & ~field) | (input.value << offset & field)};
    }
    return (KnownBits){0};
}
int shader_direct_track_known_bits(const ShFlow* f, u32 index, u32 reg, u32* known, u32* value, int depth) {
    KnownBits bits = direct_known_bits(f, index, reg, depth);
    *known = bits.known;
    *value = bits.value & bits.known;
    return bits.known != 0;
}

/* A fixed constant-buffer word, for TXD's uniform offset specialization. Unlike the descriptor tracker below,
   LDC must have a proven address: a varying table index cannot select a translation-time texture offset. */
int shader_direct_track_cbuf_word(const ShFlow* f, u32 index, u32 reg, u32* bank, u32* off, int depth) {
    const u64* insn = f->insn;
    int j = depth > 6 ? -1 : direct_reaching_def(f, index, reg);
    if (j < 0 && depth <= 6 && reg && reg < 255) {
        /* The general tracker accepts only an instruction's first destination. For LDC.B64's high word, prove
           that its low-word definition dominates this use and that nothing has overwritten the high word. */
        int low = direct_reaching_def(f, index, reg - 1);
        if (low >= 0 && f->op[low]->id == O_LDC && B(insn[low], 48, 3) == 5) {
            int unchanged = 1;
            for (u32 at = (u32)low + 1; at < index; at++) {
                if (!IDX_OK(at)) continue;
                const Op* op = f->op[at];
                if (!op || shader_writes_reg(insn[at], op->id, reg)) unchanged = 0;
            }
            if (unchanged) j = low;
        }
    }
    if (j < 0) return 0;
    u64 w = insn[j];
    int op = decode(w)->id;
    if (op == O_MOV_C) {
        *bank = B(w, 34, 5);
        *off = B(w, 20, 14);
        return *bank < 18;
    }
    if (op == O_MOV_R) return shader_direct_track_cbuf_word(f, (u32)j, B(w, 20, 8), bank, off, depth + 1);
    if (op != O_LDC || B(w, 48, 3) < 4 || B(w, 48, 3) > 5) return 0;
    u32 source = 0, address_reg = B(w, 8, 8);
    if (address_reg != 255 && !shader_direct_track_const(f, (u32)j, address_reg, &source, depth + 1)) return 0;
    *bank = B(w, 36, 5);
    if (B(w, 44, 1)) {
        *bank += source >> 16;
        source &= 0xffff;
    }
    u32 address = source + (u32)(i32)(i16)B(w, 20, 16);
    if (B(w, 48, 3) == 5 && reg != B(w, 0, 8)) address += 4;
    if (*bank >= 18 || address > 65532 || (address & 3)) return 0;
    *off = address / 4;
    return 1;
}

/* Resolve a register supplied image handle to the constant buffer word read in this basic block. */
int shader_direct_track_image_handle(const ShFlow* f, u32 index, u32 reg, u32* bank, u32* off, int depth) {
    const u64* insn = f->insn;
    int j = depth > 6 ? -1 : direct_reaching_def(f, index, reg);
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
        if (B(w, 44, 1)) {
            u32 source;
            if (!shader_direct_track_const(f, (u32)j, B(w, 8, 8), &source, depth + 1)) return 0;
            *bank += source >> 16;
            byte_off += (i32)(source & 0xffff);
        }
        *off = (u32)byte_off / 4;
        /* Keep the legacy first-descriptor behavior for indexed handles.
           Runtime descriptor selection for arbitrary indices needs a separate path. */
        return *bank < 18 && byte_off >= 0 && !(byte_off & 3);
    }
    if (op == O_MOV_R) return shader_direct_track_image_handle(f, (u32)j, B(w, 20, 8), bank, off, depth + 1);
    return 0;
}

static int direct_handle_definition(const ShFlow* f, u32 j, u32* bank, u32* off, u32* bank2, u32* off2, int depth) {
    const u64* insn = f->insn;
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
        if (B(w, 44, 1)) {
            u32 source;
            if (!shader_direct_track_const(f, j, B(w, 8, 8), &source, depth + 1)) return 0;
            *bank += source >> 16;
            byte_off += (i32)(source & 0xffff);
        }
        *off = (u32)byte_off / 4;
        return *bank < 18 && byte_off >= 0 && !(byte_off & 3);
    }
    if (op == O_MOV_R)
        return shader_direct_track_image_handle_pair(f, (u32)j, B(w, 20, 8), bank, off, bank2, off2, depth + 1);
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
        return shader_direct_track_image_handle(f, (u32)j, lhs, bank, off, depth + 1) ? 2 : 0;
    }
    rhs = B(w, 20, 8);
    if (lhs == 255) return shader_direct_track_image_handle_pair(f, (u32)j, rhs, bank, off, bank2, off2, depth + 1);
    if (rhs == 255) return shader_direct_track_image_handle_pair(f, (u32)j, lhs, bank, off, bank2, off2, depth + 1);
    /* both halves read, or one read and the other a constant (a sampler's from a MOV32I), kept as itself */
    for (int k = 0; k < 2; k++) {
        u32 half = k ? rhs : lhs, other = k ? lhs : rhs, value;
        if (!shader_direct_track_image_handle(f, (u32)j, half, bank, off, depth + 1)) continue;
        if (shader_direct_track_image_handle(f, (u32)j, other, bank2, off2, depth + 1)) return 2;
        if (!shader_direct_track_const(f, (u32)j, other, &value, depth + 1)) return 0;
        *bank2 = SH_HANDLE_IMM;
        *off2 = value;
        return 2;
    }
    return 0;
}

/* Whether the write of reg at b defines the same handle as the one at a: a loop that rewrites reg with the same
   constant buffer reads leaves the handle at a use in the loop unchanged (the second tap of a blur, or its next
   iteration's first). */
static int direct_same_handle(const ShFlow* f, u32 a, u32 b, int depth) {
    u32 ab, ao, ab2, ao2, bb, bo, bb2, bo2;
    int ca, cb;
    if (depth > 6) return 0;
    ca = direct_handle_definition(f, a, &ab, &ao, &ab2, &ao2, depth);
    cb = ca ? direct_handle_definition(f, b, &bb, &bo, &bb2, &bo2, depth) : 0;
    return ca && ca == cb && ab == bb && ao == bo && (ca == 1 || (ab2 == bb2 && ao2 == bo2));
}

/* At a forward branch merge, two different definitions can hold the same
   descriptor. Prove the value on every edge bypassing the nearest definition,
   rather than binding whichever side happens to appear last in the program.
   Indirect flow, reconvergence arrivals and back edges remain conservative. */
static int direct_joined_handle(const ShFlow* f, u32 index, u32 reg, u32* bank, u32* off, u32* bank2, u32* off2,
                                int depth) {
    const u64* insn = f->insn;
    u32 n = f->n;
    int j = -1, count, joined = 0;
    for (u32 k = index; k-- > 11;) {
        const Op* op;
        if (!IDX_OK(k)) continue;
        op = f->op[k];
        if (!op) return 0;
        if (!shader_writes_reg(insn[k], op->id, reg)) continue;
        if (B(insn[k], 0, 8) != reg || B(insn[k], 16, 4) != 7) return 0;
        j = (int)k;
        break;
    }
    if (j < 0 || !(count = direct_handle_definition(f, (u32)j, bank, off, bank2, off2, depth + 1))) return 0;
    for (u32 k = 11; k < n; k++) {
        const u32* targets;
        const u8* elsewhere;
        const Op* op;
        int edges;
        if (!IDX_OK(k)) continue;
        op = f->op[k];
        if (!op) return 0;
        if (k > (u32)j && k < index &&
            (op->id == O_CAL || op->id == O_RET || op->id == O_BRX || op->id == O_JMP || op->id == O_SYNC ||
             op->id == O_BRK || op->id == O_CONT))
            return 0;
        edges = direct_flow_targets(f, k, &targets, &elsewhere);
        if (edges < 0) return 0;
        for (int t = 0; t < edges; t++) {
            u32 b, o, b2, o2;
            if (targets[t] <= (u32)j || targets[t] > index) continue;
            if (elsewhere[t] || k > index) return 0;
            if (k >= (u32)j) continue;
            if (op->id != O_BRA ||
                shader_direct_track_image_handle_pair(f, k, reg, &b, &o, &b2, &o2, depth + 1) != count || b != *bank ||
                o != *off || (count == 2 && (b2 != *bank2 || o2 != *off2)))
                return 0;
            joined = 1;
        }
    }
    return joined ? count : 0;
}

int shader_direct_track_image_handle_pair(const ShFlow* f, u32 index, u32 reg, u32* bank, u32* off, u32* bank2,
                                          u32* off2, int depth) {
    int j;
    if (depth > 6 || reg == 255) return 0;
    j = direct_reaching_def_same(f, index, reg, direct_same_handle, depth);
    if (j >= 0) return direct_handle_definition(f, (u32)j, bank, off, bank2, off2, depth);
    return direct_joined_handle(f, index, reg, bank, off, bank2, off2, depth);
}

/* The handles reg can hold at index when its writes there disagree on the texture: each one-word handle that a MOV or
   LDC reads from a constant buffer into reg, by a write that reaches index (the reaching definitions). One program
   picks its texture with a predicate (@P0 MOV R30, c[2][0x88]; @!P0 MOV R30, c[2][0x100]), and a path around both
   keeps a handle that it loaded from memory. The translation binds each candidate and takes the sample of the one
   whose word equals the register when the instruction runs, so a handle from elsewhere that equals a candidate's is
   that texture too, and one that equals none samples zeros. A MOV from another register adds that register's
   candidates where the MOV reads it. A reaching write that joins two words into a handle (a texture and a sampler)
   holds no candidate's word, so it refuses them all. Where the reaching definitions aren't known (a BRK anywhere in
   the program), each write that control can carry to index counts, a superset of them. The count, -1 for a refusal
   or more than max. */
static int handle_candidates(const ShFlow* f, u32 index, u32 reg, u32* banks, u32* offs, int count, int max,
                             int depth) {
    u32 defs[63], ndefs = 0;
    u64 reach;
    /* RZ or a copy too deep: a value that isn't a candidate */
    if (reg == 255 || depth > 4) return count;
    reach = direct_reaching_writes(f, index, reg, defs, &ndefs);
    if (!reach) {
        ndefs = 0;
        for (u32 k = 11; k < index && ndefs < 63; k++)
            if (IDX_OK(k) && f->op[k] && shader_writes_reg(f->insn[k], f->op[k]->id, reg) &&
                direct_can_reach(f, k + 1, index) != 0)
                defs[ndefs++] = k;
        if (ndefs == 63) return -1;
        reach = ndefs ? (1ull << ndefs) - 1 : 0;
    }
    for (u32 d = 0; d < ndefs && count >= 0; d++) {
        u32 k = defs[d], bank, off, bank2, off2;
        const Op* op = f->op[k];
        int seen = 0, words;
        if (!(reach >> d & 1) || !op || B(f->insn[k], 0, 8) != reg) continue;
        if (op->id == O_MOV_R) {
            count = handle_candidates(f, k, B(f->insn[k], 20, 8), banks, offs, count, max, depth + 1);
            continue;
        }
        words = direct_handle_definition(f, k, &bank, &off, &bank2, &off2, depth);
        if (words == 2) return -1;
        if (words != 1) continue;
        for (int c = 0; c < count; c++) seen |= banks[c] == bank && offs[c] == off;
        if (seen) continue;
        if (count == max) return -1;
        banks[count] = bank;
        offs[count++] = off;
    }
    return count;
}
/* Only when the handles disagree: with one candidate, the other writes are values the tracker couldn't follow, and a
   program refused for them stays refused */
int shader_direct_handle_candidates(const ShFlow* f, u32 index, u32 reg, u32* banks, u32* offs, int max) {
    int count = handle_candidates(f, index, reg, banks, offs, 0, max, 0);
    return count > 1 ? count : 0;
}

/* AL2P's predicate (bits 44-46) is a range check no source describes. The NVN compiler gives every AL2P one, P0 to P6
   in turn, and overwrites or leaves them, so a program is translated without it only when nothing can see it: no
   instruction reads the predicate, or every path from the AL2P meets a comparison or AL2P without a guard that writes
   it whole before anything reads it. Paths follow branches, jump tables and the pops' targets as shader_resolve_crs
   gives them. A pop it can't resolve, a call, a return, a JMP, or an EXIT a PEXIT may take counts as a read */
int shader_al2p_predicate_unseen(const ShFlow* f, u32 at) {
    const u64* insn = f->insn;
    u32 n = f->n;
    u32 p = B(insn[at], 44, 3), i, nwork = 0, *work = NULL, *pops = NULL, targets[64];
    int read = 0, pexit = 0, stack = 0, crs = 0, unseen = 0, count, t;
    u8* seen = NULL;
    if (p == 7) return 1;
    for (i = 11; i < n; i++) {
        const Op* op = IDX_OK(i) && i != at ? f->op[i] : NULL;
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
u32 shader_direct_indexed_handle(const ShFlow* f, u32 index, u32 reg, int stage, u32* ldc) {
    const u64* insn = f->insn;
    int j = -1, k, depth, op = 0;
    u64 w = 0;
    u32 shift = 2;
    if (!g_shader_texture_arrays || stage == ST_CS) return 0;
    for (depth = 0; depth < 6; depth++) {
        j = direct_reaching_def(f, index, reg);
        if (j < 0) return 0;
        w = insn[j];
        op = decode(w)->id;
        if (op != O_MOV_R) break;
        index = (u32)j;
        reg = B(w, 20, 8);
    }
    if (depth == 6 || op != O_LDC || B(w, 8, 8) == 255 || B(w, 48, 3) != 4 || B(w, 44, 1)) return 0;
    k = direct_reaching_def(f, (u32)j, B(w, 8, 8));
    if (k >= 0 && f->op[k]->id == O_SHL_I && B(insn[k], 20, 5) >= 2 && B(insn[k], 20, 5) <= 15)
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
int shader_direct_query_type(const ShFlow* f, int bindless, u32 bank, u32 off, u32* type, u32* array) {
    const u64* insn = f->insn;
    u32 n = f->n;
    for (u32 k = 11; k < n; k++) {
        const Op* op;
        u32 ty, kb, ko, kb2, ko2;
        int b;
        if (!IDX_OK(k) || !(op = f->op[k])) continue;
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
            if (!shader_direct_track_image_handle_pair(f, k, B(insn[k], 20, 8), &kb, &ko, &kb2, &ko2, 0) ||
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
