/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* How far a compute program's shared memory accesses reach (shader.h's shader_shared_reach). A forward walk of the
   program's control flow keeps, at each block's start, the range of values each register can hold as the translation
   runs it: every register starts at 0, a write sets the range its operands give, a predicated write joins it with the
   old one, and paths that meet join theirs. A range a loop keeps growing becomes any value. The ranges are those of
   spirv_translate.c's arithmetic for the forms followed here (moves, thread and lane indices, adds, scaled adds,
   shifts, masks, selects, 16-bit products, unsigned bit field extracts); any other write leaves its registers holding
   any value, so the bound is never lower than what the program can touch. */
#include "nx/gpu/shader/translator.h"
#include <stdint.h>

typedef struct {
    u32 lo, hi;
} Span;

#define TOP          0xFFFFFFFFu /* the end of a range that may be any value */
#define WIDEN_AFTER  8u          /* merges into a block before a range that still grows is taken as any value */
#define REACH_BLOCKS 4096u       /* blocks the walk keeps ranges for: a program with more is not bounded */
#define REGS         255u

static Span span(u64 lo, u64 hi) {
    Span s = {0, TOP};
    if (lo <= hi && hi <= TOP) {
        s.lo = (u32)lo;
        s.hi = (u32)hi;
    }
    return s;
}
static Span any(void) { return span(0, TOP); }
static Span konst(u32 v) { return span(v, v); }
static Span join(Span a, Span b) { return span(a.lo < b.lo ? a.lo : b.lo, a.hi > b.hi ? a.hi : b.hi); }
/* a + b, any value when some pair wraps (the translation's adds are modulo 2^32) */
static Span add(Span a, Span b) {
    if ((u64)a.hi + b.hi > TOP) return any();
    return span((u64)a.lo + b.lo, (u64)a.hi + b.hi);
}
static Span add_signed(Span a, i64 k) {
    i64 lo = (i64)a.lo + k, hi = (i64)a.hi + k;
    if (lo < 0 || hi > (i64)TOP) return any();
    return span((u64)lo, (u64)hi);
}
static Span shl(Span a, u32 s) {
    if (s >= 32) return konst(0);
    if (((u64)a.hi << s) > TOP) return any();
    return span((u64)a.lo << s, (u64)a.hi << s);
}
static Span shr(Span a, u32 s) { return s >= 32 ? konst(0) : span(a.lo >> s, a.hi >> s); }
static Span low16(Span a) { return a.hi <= 0xFFFFu ? a : span(0, 0xFFFFu); }
static Span high16(Span a) { return shr(a, 16); }
/* a and the constant m: no more than either, and a itself when m keeps all of a's bits */
static Span and_const(Span a, u32 m) {
    if ((m & (m + 1)) == 0 && a.hi <= m) return a; /* m is 2^k - 1, at or above every value */
    return span(0, a.hi < m ? a.hi : m);
}
static u32 sext(u32 v, int bits) { return (u32)((i32)(v << (32 - bits)) >> (32 - bits)); }
/* an operand's 19-bit immediate, bit 56 its sign (integer_right's) */
static u32 imm19(u64 w) { return B(w, 20, 19) | (B(w, 56, 1) ? 0xFFF80000u : 0); }

/* each register's range at a point of the program */
typedef struct {
    Span r[REGS];
} Regs;
static Span reg(const Regs* s, u32 r) { return r >= REGS ? konst(0) : s->r[r]; }
static void put(Regs* s, u32 r, Span v, int guarded) {
    if (r >= REGS) return;
    s->r[r] = guarded ? join(s->r[r], v) : v;
}

/* an integer add as integer_add does it with avg (bits 48-49: negate b, negate a, both for plus one), no carry in.
   A negated side is followed only when it is one value */
static Span iadd(Span a, Span b, u32 avg) {
    if (avg == 0) return add(a, b);
    if (avg == 3) return add_signed(add(a, b), 1);
    if (avg == 1 && b.lo == b.hi) return add_signed(a, -(i64)b.lo);
    if (avg == 2 && a.lo == a.hi) return add_signed(b, -(i64)a.lo);
    return any();
}

/* the range op w (not under !PT) leaves in its destination: 1 when it is one of the forms followed, with the range in
   *out; 0 to take every register it writes as any value. MOV with a lane mask other than 1 or 15 writes nothing in
   the translation, so it is followed as no write: *out unchanged and 2 */
static int transfer(const Regs* s, u64 w, int id, const u32 local[3], Span* out) {
    u32 a = B(w, 8, 8), b = B(w, 20, 8);
    Span ra = reg(s, a);
    switch (id) {
        case O_MOV32I: *out = konst(B(w, 20, 32)); return 1;
        case O_MOV_R:
        case O_MOV_I:
        case O_MOV_C:
            if (B(w, 39, 4) != 1 && B(w, 39, 4) != 15) return 2;
            *out = id == O_MOV_R ? reg(s, b) : id == O_MOV_I ? konst(imm19(w)) : any();
            return 1;
        case O_S2R: {
            u32 sr = B(w, 20, 8);
            if (sr >= 0x21 && sr <= 0x23 && local[sr - 0x21])
                *out = span(0, local[sr - 0x21] - 1);
            else if (sr == 0) /* SR_LANEID, masked to 31 */
                *out = span(0, 31);
            else
                *out = any();
            return 1;
        }
        case O_IADD_R:
        case O_IADD_I: {
            Span rb = id == O_IADD_R ? reg(s, b) : konst(imm19(w));
            if (B(w, 43, 1)) return 0;              /* .X: the carry in */
            if (id == O_IADD_I && B(w, 48, 2) == 0) /* a negative immediate subtracts */
                *out = add_signed(ra, (i32)rb.lo);
            else
                *out = iadd(ra, rb, B(w, 48, 2));
            return 1;
        }
        case O_IADD32I:
            if (B(w, 53, 1)) return 0;
            *out = B(w, 55, 2) == 0 ? add_signed(ra, (i32)B(w, 20, 32)) : iadd(ra, konst(B(w, 20, 32)), B(w, 55, 2));
            return 1;
        case O_ISCADD_R:
        case O_ISCADD_I:
        case O_ISCADD32I: {
            int imm32 = id == O_ISCADD32I;
            Span left = shl(ra, B(w, imm32 ? 53 : 39, 5));
            Span rb = id == O_ISCADD_R ? reg(s, b) : konst(imm32 ? B(w, 20, 32) : imm19(w));
            u32 avg = imm32 ? 0 : B(w, 48, 2);
            *out = id != O_ISCADD_R && avg == 0 ? add_signed(left, (i32)rb.lo) : iadd(left, rb, avg);
            return 1;
        }
        case O_SHL_I: {
            u32 shift = imm19(w);
            *out = B(w, 39, 1) ? shl(ra, shift & 31) : shl(ra, shift); /* without .W a shift past 31 gives 0 */
            return 1;
        }
        case O_SHR_I: {
            u32 shift = imm19(w);
            if (B(w, 40, 1)) return 0; /* bit reversed */
            if (!B(w, 48, 1)) {
                *out = !B(w, 39, 1) && shift >= 32 ? konst(0) : shr(ra, shift & 31);
                return 1;
            }
            if (ra.hi >= 0x80000000u) return 0; /* an arithmetic shift of a negative value */
            *out = shr(ra, B(w, 39, 1) ? shift & 31 : shift < 31 ? shift : 31);
            return 1;
        }
        case O_LOP_R:
        case O_LOP_I:
        case O_LOP32I: {
            int imm32 = id == O_LOP32I;
            u32 op = B(w, imm32 ? 53 : 41, 2);
            Span rb = id == O_LOP_R ? reg(s, b) : konst(imm32 ? B(w, 20, 32) : imm19(w));
            if (B(w, imm32 ? 56 : 40, 1)) { /* the second operand inverted: followed when it is one value */
                if (rb.lo != rb.hi) return 0;
                rb = konst(~rb.lo);
            }
            if (op == 3) { /* PASS_B */
                *out = rb;
                return 1;
            }
            if (op != 0 || B(w, imm32 ? 55 : 39, 1)) return 0; /* OR, XOR, or the first operand inverted */
            *out = rb.lo == rb.hi ? and_const(ra, rb.lo) : span(0, ra.hi < rb.hi ? ra.hi : rb.hi);
            return 1;
        }
        case O_SEL_R:
        case O_SEL_I: *out = join(ra, id == O_SEL_R ? reg(s, b) : konst(imm19(w))); return 1;
        case O_XMAD_R:
        case O_XMAD_I: {
            /* spirv_translate.c's XMAD: halves of A and B (signed by bits 48 and 49), their product shifted by PSL
               (36), C picked by the select (50-52), MRG (37) putting B's low half on top; X (38) adds the carry */
            Span rb = id == O_XMAD_R ? reg(s, b) : konst(B(w, 20, 16));
            Span rc = reg(s, B(w, 39, 8)), third;
            int half_b = id == O_XMAD_R ? (int)B(w, 35, 1) : 0;
            if (B(w, 48, 1) || B(w, 49, 1) || B(w, 38, 1)) return 0;
            Span left = B(w, 53, 1) ? high16(ra) : low16(ra), right = half_b ? high16(rb) : low16(rb);
            Span product = span((u64)left.lo * right.lo, (u64)left.hi * right.hi);
            if (B(w, 36, 1)) product = shl(product, 16);
            switch (B(w, 50, 3)) {
                case 0: third = rc; break;
                case 1: third = low16(rc); break;
                case 2: third = high16(rc); break;
                case 4: third = add(shl(rb, 16), rc); break;
                default: return 0;
            }
            Span sum = add(product, third);
            if (B(w, 37, 1)) { /* MRG: the sum's low half under B's low half */
                Span top = low16(rb);
                int small = sum.hi <= 0xFFFFu;
                sum = span((u64)top.lo << 16 | (small ? sum.lo : 0), (u64)top.hi << 16 | (small ? sum.hi : 0xFFFFu));
            }
            *out = sum;
            return 1;
        }
        case O_BFE_I: {
            u32 packed = imm19(w);
            u32 raw_offset = packed & 255, offset = raw_offset > 31 ? 31 : raw_offset;
            u32 width = (packed >> 8) & 255;
            if (B(w, 48, 1) || B(w, 40, 1)) return 0; /* signed, or bit reversed */
            if (width > 32 - offset) width = 32 - offset;
            if (!width || raw_offset >= 32) {
                *out = konst(0);
                return 1;
            }
            u32 mask = width == 32 ? TOP : (1u << width) - 1;
            Span moved = shr(ra, offset);
            *out = moved.hi <= mask ? moved : span(0, mask);
            return 1;
        }
        default: return 0;
    }
}

/* one past the last byte a shared access w (LDS, STS, ATOMS, ATOMS.CAS) touches from a register in base: the word
   index is the address over 4 as the translation takes it, SH_SHARED_ANY when the address may wrap */
static u32 access_end(u64 w, int id, Span base) {
    i64 offset;
    u32 words;
    if (id == O_LDS || id == O_STS) {
        u32 size = B(w, 48, 3);
        offset = (i32)sext(B(w, 20, 24), 24);
        words = size == 5 ? 2 : size >= 6 ? 4 : 1;
    } else {
        offset = (i64)(i32)sext(B(w, 30, 22), 22) * 4;
        words = id == O_ATOMS ? (B(w, 28, 2) >= 2 ? 2 : 1) : (B(w, 52, 4) == 5 ? 2 : 1);
    }
    i64 lo = (i64)base.lo + offset, hi = (i64)base.hi + offset;
    if (lo < 0 || hi > (i64)TOP) return SH_SHARED_ANY;
    u64 end = ((u64)hi >> 2) * 4 + (u64)words * 4;
    return end >= SH_SHARED_ANY ? SH_SHARED_ANY : (u32)end;
}

/* runs instruction i on s: 0, or 1 for a word that is no instruction; with reach, a shared access's end is raised
   into it */
static int step(Regs* s, const u64* words, u32 i, const u32 local[3], u32* reach) {
    u64 w = words[i];
    const Op* op = decode(w);
    Span v;
    int how;
    if (!op) return 1;
    if (B(w, 16, 4) == 15) return 0; /* @!PT: never runs */
    int guarded = B(w, 16, 3) != 7;
    if (reach && (op->id == O_LDS || op->id == O_STS || op->id == O_ATOMS || op->id == O_ATOMS_CAS)) {
        u32 end = access_end(w, op->id, reg(s, B(w, 8, 8)));
        if (end > *reach) *reach = end;
    }
    how = transfer(s, w, op->id, local, &v);
    if (how == 1)
        put(s, B(w, 0, 8), v, guarded);
    else if (how == 0)
        for (u32 r = 0; r < REGS; r++)
            if (shader_writes_reg(w, op->id, r)) put(s, r, any(), guarded);
    return 0;
}

/* the word after i, past scheduling words */
static u32 next_word(u32 i) {
    i++;
    if (i >= 10 && (i - 10) % 4 == 0) i++;
    return i;
}

u32 shader_shared_reach(const u8* code, u32 max_bytes, const u32 local[3]) {
    u64* copy = NULL;
    const u64* words;
    u32 len = 0, n, nblocks = 0, reach = 0, result = SH_SHARED_ANY;
    if (!code || max_bytes < 96) return SH_SHARED_ANY;
    shader_hash(code, max_bytes, &len);
    n = len / 8;
    if (n < 12) return 0; /* no instruction */
    words = shader_words(code, max_bytes, &copy);
    if (!words) return SH_SHARED_ANY;
    int accesses = 0; /* a program without shared accesses needs none, whatever its control flow */
    for (u32 i = 11; i < n && !accesses; i = next_word(i)) {
        const Op* op = decode(words[i]);
        accesses = op && (op->id == O_LDS || op->id == O_STS || op->id == O_ATOMS || op->id == O_ATOMS_CAS);
    }
    if (!accesses) {
        free(copy);
        return 0;
    }
    /* one block: by word, the target of a SYNC, BRK or CONT (shader_resolve_crs), the block a leader starts (else ~0)
       and whether it is one; grown, once the blocks are counted, by what each block has: the ranges at its start, the
       merges into it, and 1 once walked or 2 while it has ranges to walk */
    size_t by_word = ((size_t)n * (2 * sizeof(u32) + 1) + 7) & ~(size_t)7, by_block;
    u8* mem = (u64)n * 9 + 8 + (u64)REACH_BLOCKS * (sizeof(Regs) + 5) <= SIZE_MAX ? calloc(1, by_word) : NULL;
    Regs* entry;
    u32 *pop, *block, *merges;
    u8 *lead, *reached;
    if (!mem) goto done;
    pop = (u32*)(void*)mem;
    block = pop + n;
    lead = (u8*)(block + n);
    if (!shader_resolve_crs(words, n, pop)) goto done;
    lead[11] = 1;
    for (u32 i = 11; i < n; i = next_word(i)) {
        const Op* op = decode(words[i]);
        if (!op) goto done;
        switch (op->id) {
            case O_BRA: {
                u32 t = bra_target(words[i], i);
                if (t >= n || !IDX_OK(t)) goto done;
                lead[t] = 1;
                break;
            }
            case O_SYNC:
            case O_BRK:
            case O_CONT:
                if (pop[i]) lead[pop[i]] = 1;
                break;
            case O_EXIT:
            case O_KIL:
            case O_SSY:
            case O_PBK:
            case O_PCNT: break;
            case O_BRX:
            case O_JMP:
            case O_CAL:
            case O_RET:
            case O_PRET:
            case O_PEXIT:
            case O_SETCRSPTR: goto done; /* control the walk doesn't follow */
            default: continue;
        }
        if (op->id == O_BRA || op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT || op->id == O_EXIT ||
            op->id == O_KIL) {
            u32 after = next_word(i);
            if (after < n) lead[after] = 1;
        }
    }
    for (u32 i = 0; i < n; i++) {
        block[i] = ~0u;
        if (lead[i] && IDX_OK(i)) block[i] = nblocks++;
    }
    if (nblocks > REACH_BLOCKS) goto done;
    by_block = (size_t)nblocks * (sizeof(Regs) + sizeof(u32) + 1);
    {
        u8* grown = realloc(mem, by_word + by_block);
        if (!grown) goto done;
        mem = grown;
    }
    memset(mem + by_word, 0, by_block);
    pop = (u32*)(void*)mem;
    block = pop + n;
    entry = (Regs*)(void*)(mem + by_word);
    merges = (u32*)(void*)(entry + nblocks);
    reached = (u8*)(merges + nblocks);
    reached[0] = 2; /* 2: to walk; registers start at 0, entry[0] as allocated */
    for (int again = 1; again;) {
        again = 0;
        for (u32 i = 11; i < n; i = next_word(i)) {
            if (block[i] == ~0u || reached[block[i]] != 2) continue;
            u32 from = block[i], last = i, succ[2], nsucc = 0;
            Regs s = entry[from];
            reached[from] = 1;
            for (u32 j = i;;) {
                if (step(&s, words, j, local, NULL)) goto done;
                last = j;
                j = next_word(j);
                if (j >= n || block[j] != ~0u) break;
            }
            u64 w = words[last];
            const Op* op = decode(w);
            u32 after = next_word(last);
            int falls = 1;
            if (op->id == O_BRA) {
                succ[nsucc++] = bra_target(w, last);
                falls = !flow_always(w);
            } else if (op->id == O_SYNC || op->id == O_BRK || op->id == O_CONT) {
                if (!pop[last]) goto done; /* a pop the stack walk never reached */
                succ[nsucc++] = pop[last];
                falls = !flow_always(w);
            } else if (op->id == O_EXIT || op->id == O_KIL)
                falls = !flow_always(w);
            if (falls && after < n) succ[nsucc++] = after;
            for (u32 k = 0; k < nsucc; k++) {
                u32 to = block[succ[k]];
                int grew = 0;
                if (to == ~0u) goto done;
                if (!reached[to]) {
                    entry[to] = s;
                    grew = 1;
                } else {
                    int widen = ++merges[to] > WIDEN_AFTER;
                    for (u32 r = 0; r < REGS; r++) {
                        Span old = entry[to].r[r], both = join(old, s.r[r]);
                        if (both.lo == old.lo && both.hi == old.hi) continue;
                        entry[to].r[r] = widen ? any() : both;
                        grew = 1;
                    }
                }
                if (grew) {
                    reached[to] = 2;
                    again = 1;
                }
            }
        }
    }
    /* the ranges at each block's start are final: the accesses, with the ranges where they run */
    for (u32 i = 11; i < n; i = next_word(i)) {
        if (block[i] == ~0u || !reached[block[i]]) continue;
        Regs s = entry[block[i]];
        for (u32 j = i;;) {
            step(&s, words, j, local, &reach);
            j = next_word(j);
            if (j >= n || block[j] != ~0u) break;
        }
    }
    result = reach;
done:
    free(mem);
    free(copy);
    return result;
}

int shader_shared_fit(const u8* code, u32 max_bytes, const u32 local[3], u32 shm, u32 device_max, u32* size,
                      u32* reach) {
    *size = shm;
    *reach = 0;
    if (shm <= device_max) return 1;
    *reach = shader_shared_reach(code, max_bytes, local);
    if (*reach > device_max) return 0;
    *size = device_max;
    return 1;
}
