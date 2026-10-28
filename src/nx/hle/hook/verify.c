/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Verify mode's check of one call (verify.h). It depends on the CPU and guest memory alone, so tests run it on
   functions of their own */
#include "nx/hle/hook/verify.h"
#include <stddef.h>
#include <string.h>

/* A copy keeps its buffer's address modulo this, so code that takes another path for another alignment takes the
   same one, and has this much room after it, for code that reads whole aligned words past a buffer's end (without
   crossing a page, which the copy's room can't promise: such a read past the end of a copy at a page's end reads
   whatever scratch memory follows) */
#define KEEP_ALIGN 64u
#define SLACK      128u

typedef struct {
    u64 lo, hi;  /* live [lo, hi) */
    u64 copy;    /* the copy's address */
    int group;   /* the merged range it is in */
} Extent;

const char* hook_verify_text(int what) {
    switch (what) {
        case HOOK_AGREE: return "agrees";
        case HOOK_DIFFER_RESULT: return "returns another result";
        case HOOK_DIFFER_REG: return "leaves a callee-saved register otherwise";
        case HOOK_DIFFER_BYTES: return "writes other bytes";
        case HOOK_SKIP_SIZE: return "skipped (its buffers don't fit in the scratch memory)";
        default: return "skipped (a buffer's length is out of range)";
    }
}

/* the registers a call may change, as one block: the CPU's fields before mem (x, pc, flags, v, fpcr/fpsr, tls,
   the exclusive monitor), never poll, which another thread writes */
#define REGS_SIZE offsetof(CPU, mem)

/* the original's result and callee-saved registers, which the handler has to leave the same */
static int compare_regs(const CPU* h, const CPU* o, int ret, const Extent* e, int n, HookVerifyResult* r) {
    u64 a = h->x[0], b = o->x[0];
    int i, same = 1;
    switch (ret) {
        case HOOK_RET_VOID: break;
        case HOOK_RET_BOOL: same = ((a & 0xFF) != 0) == ((b & 0xFF) != 0); break;
        case HOOK_RET_X8: same = (u8)a == (u8)b; break;
        case HOOK_RET_X16: same = (u16)a == (u16)b; break;
        case HOOK_RET_X32: same = (u32)a == (u32)b; break;
        case HOOK_RET_ADDR:
            for (i = 0; i < n; i++) /* a pointer into a copy is the same pointer into the live buffer */
                if (b >= e[i].copy && b - e[i].copy <= e[i].hi - e[i].lo) {
                    b = e[i].lo + (b - e[i].copy);
                    break;
                }
            same = a == b;
            break;
        case HOOK_RET_F32:
            a = SNX_VS(h->v[0], 0), b = SNX_VS(o->v[0], 0);
            same = a == b;
            break;
        case HOOK_RET_F64:
            a = SNX_VD(h->v[0], 0), b = SNX_VD(o->v[0], 0);
            same = a == b;
            break;
        default: same = a == b; break;
    }
    if (!same) {
        r->what = HOOK_DIFFER_RESULT, r->a = a, r->b = b;
        return 0;
    }
    for (i = 19; i <= 31; i++) /* x19-x29 and sp (x30, the link register, isn't kept) */
        if (i != 30 && h->x[i] != o->x[i]) {
            r->what = HOOK_DIFFER_REG, r->reg = i, r->a = h->x[i], r->b = o->x[i];
            return 0;
        }
    for (i = 8; i <= 15; i++) /* d8-d15: only their low halves are callee-saved */
        if (SNX_VD(h->v[i], 0) != SNX_VD(o->v[i], 0)) {
            r->what = HOOK_DIFFER_REG, r->reg = 32 + i, r->a = SNX_VD(h->v[i], 0), r->b = SNX_VD(o->v[i], 0);
            return 0;
        }
    return 1;
}

int hook_verify_call(CPU* c, u64 entry, GuestFn original, GuestFn handler, const HookVerify* v, u64 scratch,
                     u64 scratch_size, HookVerifyResult* r) {
    Extent e[HOOK_BUFS];
    u64 group_lo[HOOK_BUFS], group_hi[HOOK_BUFS], group_copy[HOOK_BUFS], at = scratch;
    u64 ret = c->x[30], sp = c->x[31];
    int i, j, n = 0, groups = 0, order[HOOK_BUFS];
    CPU saved, orig;
    memset(r, 0, sizeof *r);
    for (i = 0; i < v->nbufs; i++) { /* the buffers' live ranges */
        const HookBuf* b = &v->buf[i];
        u64 len = b->len_add, base = c->x[b->reg];
        if (b->len_reg != HOOK_NO_REG) {
            u64 count = c->x[b->len_reg];
            if (b->len_mul && count > (~0ull - len) / b->len_mul) return r->what = HOOK_SKIP_RANGE;
            len += count * b->len_mul;
        }
        if (len > (1ull << 40) || base > (1ull << 48) - len) return r->what = HOOK_SKIP_RANGE;
        e[i].lo = base, e[i].hi = base + len;
        order[n] = n;
        n++;
    }
    for (i = 0; i < n; i++) { /* by start, so that one sweep merges overlapping (or touching) buffers into one copy */
        int k = i;
        for (j = i + 1; j < n; j++)
            if (e[order[j]].lo < e[order[k]].lo) k = j;
        j = order[i], order[i] = order[k], order[k] = j;
    }
    for (i = 0; i < n; i++) {
        Extent* x = &e[order[i]];
        if (!groups || x->lo > group_hi[groups - 1]) {
            group_lo[groups] = x->lo, group_hi[groups] = x->hi;
            groups++;
        } else if (x->hi > group_hi[groups - 1]) {
            group_hi[groups - 1] = x->hi;
        }
        x->group = groups - 1;
    }
    for (i = 0; i < groups; i++) { /* the copies, each at its buffer's alignment */
        u64 base = (at + KEEP_ALIGN - 1) & ~(u64)(KEEP_ALIGN - 1);
        base += group_lo[i] & (KEEP_ALIGN - 1);
        if (base < at || base - scratch > scratch_size ||
            group_hi[i] - group_lo[i] + SLACK > scratch + scratch_size - base)
            return r->what = HOOK_SKIP_SIZE;
        group_copy[i] = base;
        at = base + (group_hi[i] - group_lo[i]) + SLACK;
    }
    for (i = 0; i < groups; i++)
        if (group_hi[i] > group_lo[i])
            memcpy(c->mem + group_copy[i], c->mem + group_lo[i], (size_t)(group_hi[i] - group_lo[i]));
    for (i = 0; i < n; i++) e[i].copy = group_copy[e[i].group] + (e[i].lo - group_lo[e[i].group]);

    /* the function's own code on the copies: its entry, then whatever it left for the dispatch loop until it
       returns to the call's return address with the call's stack */
    memcpy(&saved, c, REGS_SIZE);
    for (i = 0; i < n; i++) c->x[v->buf[i].reg] = e[i].copy;
    original(c, entry);
    while (c->pc != ret || c->x[31] != sp) snx_lookup(c, c->pc)(c, c->pc);
    memcpy(&orig, c, REGS_SIZE);
    memcpy(c, &saved, REGS_SIZE);

    /* the handler on the live memory, as the call goes on from it */
    handler(c, entry);

    if (!compare_regs(c, &orig, v->ret, e, n, r)) return r->what;
    for (i = 0; i < n; i++) {
        const u8* live = c->mem + e[i].lo;
        const u8* copy = c->mem + e[i].copy;
        u64 len = e[i].hi - e[i].lo, k;
        if (!memcmp(live, copy, (size_t)len)) continue;
        for (k = 0; live[k] == copy[k]; k++) {
        }
        r->what = HOOK_DIFFER_BYTES, r->buf = i, r->offset = k, r->a = live[k], r->b = copy[k];
        return r->what;
    }
    return r->what = HOOK_AGREE;
}
