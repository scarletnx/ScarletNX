/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The instruction semantics follow the constant folding of Mesa's NAK (src/nouveau/compiler/nak/ir.rs) and its
   encoder's fields (sm70_encode.rs): Copyright © 2022 Collabora, Ltd., SPDX-License-Identifier: MIT */
/* The CPU reference for SM86 compute programs (sm86_ref.h). It is the hardware's arithmetic, written apart from
   sm86_translate.c: the rounding modes come from fesetround (this file builds with -frounding-math), FTZ flushes the
   operands and the result, and the global memory is a 64-bit address space. */
#include "support/sm86_ref.h"
#include <fenv.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ---- the encoder ---- */

void sm86_set(Sm86Word* x, unsigned lo, unsigned n, u64 v) {
    for (unsigned i = 0; i < n; i++) {
        unsigned bit = lo + i;
        u64 m = 1ull << (bit & 63);
        if ((v >> i) & 1)
            x->w[bit >> 6] |= m;
        else
            x->w[bit >> 6] &= ~m;
    }
}
Sm86Word sm86_fix(u32 opcode) {
    Sm86Word x = {{0, 0}};
    sm86_set(&x, 0, 12, opcode);
    sm86_set(&x, 12, 3, 7);
    sm86_set(&x, 110, 3, 7);
    sm86_set(&x, 113, 3, 7);
    return x;
}
Sm86Word sm86_alu(u32 opcode9, u32 form, u32 dst, u32 a) {
    Sm86Word x = sm86_fix(0);
    sm86_set(&x, 0, 9, opcode9);
    sm86_set(&x, 9, 3, form);
    sm86_set(&x, 16, 8, dst);
    sm86_set(&x, 24, 8, a);
    return x;
}
void sm86_cbuf(Sm86Word* x, u32 bank, u32 off) {
    sm86_set(x, 38, 16, off);
    sm86_set(x, 54, 5, bank);
}
Sm86Word sm86_bra(u32 ip, u32 target, u32 pred, int negate) {
    Sm86Word x = sm86_fix(0x947);
    sm86_set(&x, 12, 3, pred);
    sm86_set(&x, 15, 1, (u64)negate);
    sm86_set(&x, 87, 3, 7);
    sm86_set(&x, 34, 48, (u64)target * 4 - (u64)ip * 4 - 4); /* set_rel_offset: in 32-bit words, from the next */
    return x;
}
Sm86Word sm86_exit(void) {
    Sm86Word x = sm86_fix(0x94d);
    sm86_set(&x, 87, 3, 7);
    return x;
}
void sm86_put(u8* code, u32 index, Sm86Word x) {
    for (int k = 0; k < 8; k++) {
        code[16 * index + k] = (u8)(x.w[0] >> (8 * k));
        code[16 * index + 8 + k] = (u8)(x.w[1] >> (8 * k));
    }
}

/* ---- the machine ---- */

typedef struct {
    u32 r[256], ur[64];
    u8 p[8], up[8];
    u32 tid[3], ctaid[3];
} Thread;

static float f_of(u32 v) {
    float f;
    memcpy(&f, &v, 4);
    return f;
}
static u32 u_of(float f) {
    u32 v;
    memcpy(&v, &f, 4);
    return v;
}
static u32 rget(const Thread* t, u32 r) { return r >= 255 ? 0 : t->r[r]; }
static void rset(Thread* t, u32 r, u32 v) {
    if (r < 255) t->r[r] = v;
}
static u32 urget(const Thread* t, u32 r) { return r >= 63 ? 0 : t->ur[r]; }
static void urset(Thread* t, u32 r, u32 v) {
    if (r < 63) t->ur[r] = v;
}
static int pget(const Thread* t, u32 p, u32 negate, int uniform) {
    int v = p >= 7 ? 1 : uniform ? t->up[p] : t->p[p];
    return negate ? !v : v;
}
static void pset(Thread* t, u32 p, int v, int uniform) {
    if (p >= 7) return;
    if (uniform)
        t->up[p] = (u8)v;
    else
        t->p[p] = (u8)v;
}
static u32 cbuf_word(const Sm86RefEnv* env, u32 bank, u32 off) {
    if (bank >= 18 || !env->cbuf[bank] || off / 4 >= env->cbuf_words[bank]) return 0;
    return env->cbuf[bank][off / 4];
}
static u32 src(const Thread* t, const Sm86RefEnv* env, const Sm86Src* s) {
    switch (s->kind) {
        case SM86_SRC_REG: return rget(t, s->reg);
        case SM86_SRC_UREG: return urget(t, s->reg);
        case SM86_SRC_IMM: return s->value;
        case SM86_SRC_CBUF: return cbuf_word(env, s->bank, s->value);
        default: return 0;
    }
}
static u32 src_neg(const Thread* t, const Sm86RefEnv* env, const Sm86Src* s) {
    u32 v = src(t, env, s);
    return s->neg && s->kind != SM86_SRC_IMM ? 0u - v : v;
}
static float flush(float f, int on) {
    if (on && fpclassify(f) == FP_SUBNORMAL) return signbit(f) ? -0.0f : 0.0f;
    return f;
}
static float srcf(const Thread* t, const Sm86RefEnv* env, const Sm86Src* s, int ftz) {
    float f = f_of(src(t, env, s));
    if (s->kind != SM86_SRC_IMM) {
        if (s->abs) f = fabsf(f);
        if (s->neg) f = -f;
    }
    return flush(f, ftz);
}
static const int rounding[4] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
/* the NaN that the hardware's float arithmetic gives, whatever NaN went in */
static u32 nan_canon(u32 v) { return isnan(f_of(v)) ? 0x7fffffffu : v; }
static float sat(float f, int on) {
    if (!on) return f;
    if (!(f > 0.0f)) return 0.0f; /* NaN too */
    return f > 1.0f ? 1.0f : f;
}
static const Sm86Src* second(const Sm86Insn* in) {
    return in->form == 2 || in->form == 3 || in->form == 7 ? &in->c : &in->b;
}
/* a global byte address: its buffer and offset, or NULL */
static u8* global_at(const Sm86RefEnv* env, u64 va, u32 bytes) {
    for (int k = 0; k < 4; k++)
        if (env->global[k] && va >= env->global_va[k] && va - env->global_va[k] + bytes <= env->global_bytes[k])
            return env->global[k] + (va - env->global_va[k]);
    return NULL;
}
static int fcmp(u32 op, float x, float y) {
    int un = isnan(x) || isnan(y);
    switch (op) {
        case 0: return 0;
        case 1: return x < y;
        case 2: return x == y;
        case 3: return x <= y;
        case 4: return x > y;
        case 5: return !un && x != y;
        case 6: return x >= y;
        case 7: return !un;
        case 8: return un;
        case 9: return un || x < y;
        case 10: return un || x == y;
        case 11: return un || x <= y;
        case 12: return un || x > y;
        case 13: return un || x != y;
        case 14: return un || x >= y;
        default: return 1;
    }
}
static int icmp(u32 op, int is_signed, u32 x, u32 y) {
    /* signed: with the sign bits flipped, the values compare as unsigned ones do */
    int eq = x == y, lt = is_signed ? (x ^ 0x80000000u) < (y ^ 0x80000000u) : x < y;
    switch (op) {
        case 1: return lt;
        case 2: return eq;
        case 3: return lt || eq;
        case 4: return !lt && !eq;
        case 5: return !eq;
        case 6: return !lt;
        case 7: return 1;
        default: return 0;
    }
}
static int combine(u32 set_op, int x, int acc) { return set_op == 0 ? x && acc : set_op == 1 ? x || acc : x != acc; }
static u32 lut3(u32 lut, u32 x, u32 y, u32 z) {
    u32 r = 0;
    for (u32 i = 0; i < 8; i++)
        if (lut & (1u << i)) r |= (i & 4 ? x : ~x) & (i & 2 ? y : ~y) & (i & 1 ? z : ~z);
    return r;
}

static const char* step(Thread* t, Sm86RefEnv* env, const Sm86Insn* in, u8* shared, int* done) {
    int op = in->op, uniform = sm86_ops[op].kind == SM86_K_UALU;
    volatile float vx, vy, vz; /* the operands of an operation in a rounding mode, so that it runs then */
    volatile double vd;
    switch (op) {
        case SM86_NOP:
        case SM86_DEPBAR:
        case SM86_BSSY:
        case SM86_BSYNC: return NULL;
        case SM86_EXIT: *done = 1; return NULL;
        case SM86_MOV: rset(t, in->dst, src(t, env, &in->b)); return NULL;
        case SM86_S2R: {
            u32 sr = sm86_field(in, 72, 8);
            if (sr >= 0x21 && sr <= 0x23)
                rset(t, in->dst, t->tid[sr - 0x21]);
            else if (sr >= 0x25 && sr <= 0x27)
                rset(t, in->dst, t->ctaid[sr - 0x25]);
            else
                return "S2R register";
            return NULL;
        }
        case SM86_CS2R:
            rset(t, in->dst, 0);
            if (sm86_field(in, 80, 1) && in->dst < 255) rset(t, in->dst + 1, 0);
            return NULL;
        case SM86_ULDC: {
            u32 bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16), d = sm86_field(in, 16, 6);
            urset(t, d, cbuf_word(env, bank, off));
            if (sm86_field(in, 73, 3) == 5 && d < 63) urset(t, d + 1, cbuf_word(env, bank, off + 4));
            return NULL;
        }
        case SM86_LDC: {
            u32 bank = sm86_field(in, 54, 5), off = sm86_field(in, 38, 16) + rget(t, sm86_field(in, 24, 8));
            rset(t, in->dst, cbuf_word(env, bank, off));
            if (sm86_field(in, 73, 3) == 5 && in->dst < 255) rset(t, in->dst + 1, cbuf_word(env, bank, off + 4));
            return NULL;
        }
        case SM86_IADD3:
        case SM86_UIADD3: {
            /* NAK's fold: the sum in 64 bits, a negated source as ~x + 1, the carries out at 2^32 and 2^33 */
            const Sm86Src* s3[3] = {&in->a, &in->b, &in->c};
            u64 sum = 0;
            for (int k = 0; k < 3; k++) {
                u32 v = src(t, env, s3[k]);
                sum += s3[k]->neg && s3[k]->kind != SM86_SRC_IMM ? (u64)(u32)~v + 1 : v;
            }
            if (sm86_field(in, 74, 1))
                sum += (u64)pget(t, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform) +
                       (u64)pget(t, sm86_field(in, 77, 3), sm86_field(in, 80, 1), uniform);
            if (uniform)
                urset(t, in->dst, (u32)sum);
            else
                rset(t, in->dst, (u32)sum);
            pset(t, sm86_field(in, 81, 3), sum >= 1ull << 32, uniform);
            pset(t, sm86_field(in, 84, 3), sum >= 2ull << 32, uniform);
            return NULL;
        }
        case SM86_IMAD:
            rset(t, in->dst, src_neg(t, env, &in->a) * src_neg(t, env, &in->b) + src_neg(t, env, &in->c));
            return NULL;
        case SM86_IMAD_WIDE:
        case SM86_IMAD_HI: {
            /* a * b in 64 bits (bit 73 signed): WIDE adds the pair at c, HI takes the high word and adds c */
            u32 x = src_neg(t, env, &in->a), y = src_neg(t, env, &in->b);
            u64 px = x, py = y;
            if (sm86_field(in, 73, 1)) {
                px = x >> 31 ? (u64)x | 0xffffffff00000000ull : x;
                py = y >> 31 ? (u64)y | 0xffffffff00000000ull : y;
            }
            if (op == SM86_IMAD_HI) {
                rset(t, in->dst, (u32)((px * py) >> 32) + src_neg(t, env, &in->c));
                return NULL;
            }
            u32 c = in->c.reg;
            u64 r = px * py + (((u64)rget(t, c >= 255 ? c : c + 1) << 32) | rget(t, c));
            rset(t, in->dst, (u32)r);
            if (in->dst < 255) rset(t, in->dst + 1, (u32)(r >> 32));
            return NULL;
        }
        case SM86_SGXT: {
            u32 x = src(t, env, &in->a), n = src(t, env, &in->b), r = 0;
            n = n < 32 ? n : 32;
            if (n == 32)
                r = x;
            else if (n) {
                r = x & ((1u << n) - 1);
                if (sm86_field(in, 73, 1) && (r >> (n - 1)) & 1) r |= ~((1u << n) - 1);
            }
            rset(t, in->dst, r);
            return NULL;
        }
        case SM86_PRMT: {
            u32 a = src(t, env, &in->a), sel = src(t, env, &in->b), c = src(t, env, &in->c), r = 0;
            u64 bytes = ((u64)c << 32) | a;
            for (u32 k = 0; k < 4; k++) {
                u32 nib = (sel >> (4 * k)) & 15, byte = (u32)(bytes >> (8 * (nib & 7))) & 0xff;
                if (nib & 8) byte = byte & 0x80 ? 0xff : 0;
                r |= byte << (8 * k);
            }
            rset(t, in->dst, r);
            return NULL;
        }
        case SM86_LEA: {
            u32 shift = sm86_field(in, 75, 5), a = src(t, env, &in->a), r;
            if (sm86_field(in, 80, 1))
                r = (u32)((((u64)src(t, env, &in->c) << 32) | a) >> (32 - shift));
            else
                r = a << shift;
            if (sm86_field(in, 72, 1)) r = 0u - r;
            u64 sum = (u64)r + src_neg(t, env, &in->b);
            if (sm86_field(in, 74, 1)) sum += (u64)pget(t, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            rset(t, in->dst, (u32)sum);
            pset(t, sm86_field(in, 81, 3), sum >> 32 != 0, 0);
            return NULL;
        }
        case SM86_LOP3: {
            u32 v = lut3(sm86_field(in, 72, 8), src(t, env, &in->a), src(t, env, &in->b), src(t, env, &in->c));
            rset(t, in->dst, v);
            pset(t, sm86_field(in, 81, 3), v != 0, 0);
            return NULL;
        }
        case SM86_SHF: {
            /* NAK's fold for SM70+: high:low shifted in 64 bits, the shift wrapped or clamped to the type's bits */
            u32 dtype = sm86_field(in, 73, 2), bits = dtype < 2 ? 64 : 32, s = src(t, env, &in->b);
            s = sm86_field(in, 75, 1) ? s & (bits - 1) : s < bits ? s : bits;
            u64 x = ((u64)src(t, env, &in->c) << 32) | src(t, env, &in->a), r;
            int right = (int)sm86_field(in, 76, 1), is_signed = dtype == 0 || dtype == 2;
            if (!right)
                r = s >= 64 ? 0 : x << s;
            else if (is_signed) {
                u64 sign = x >> 63 ? ~0ull : 0;
                r = s >= 64 ? sign : s == 0 ? x : (x >> s) | (sign << (64 - s));
            } else
                r = s >= 64 ? 0 : x >> s;
            rset(t, in->dst, sm86_field(in, 80, 1) ? (u32)(r >> 32) : (u32)r);
            return NULL;
        }
        case SM86_IMNMX: {
            u32 x = src(t, env, &in->a), y = src(t, env, &in->b);
            int lt = icmp(1, (int)sm86_field(in, 73, 1), x, y);
            int min = pget(t, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            rset(t, in->dst, min ? (lt ? x : y) : (lt ? y : x));
            return NULL;
        }
        case SM86_IABS: {
            u32 x = src(t, env, &in->b);
            rset(t, in->dst, x >> 31 ? 0u - x : x);
            return NULL;
        }
        case SM86_ISETP:
        case SM86_FSETP: {
            int cmp;
            if (op == SM86_FSETP) {
                int on = (int)sm86_field(in, 80, 1);
                cmp = fcmp(sm86_field(in, 76, 4), srcf(t, env, &in->a, on), srcf(t, env, &in->b, on));
            } else {
                u32 x = src_neg(t, env, &in->a), y = src_neg(t, env, &in->b), c = sm86_field(in, 76, 3);
                cmp = icmp(c, (int)sm86_field(in, 73, 1), x, y);
                if (sm86_field(in, 72, 1) && x == y && c != 0 && c != 7)
                    cmp = pget(t, sm86_field(in, 68, 3), sm86_field(in, 71, 1), uniform);
            }
            int acc = pget(t, sm86_field(in, 87, 3), sm86_field(in, 90, 1), uniform);
            u32 set_op = sm86_field(in, 74, 2);
            pset(t, sm86_field(in, 81, 3), combine(set_op, cmp, acc), uniform);
            pset(t, sm86_field(in, 84, 3), combine(set_op, !cmp, acc), uniform);
            return NULL;
        }
        case SM86_SEL:
        case SM86_FSEL: {
            int p = pget(t, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            if (op == SM86_SEL)
                rset(t, in->dst, p ? src(t, env, &in->a) : src(t, env, &in->b));
            else
                rset(t, in->dst, u_of(p ? srcf(t, env, &in->a, 0) : srcf(t, env, &in->b, 0)));
            return NULL;
        }
        case SM86_R2P: {
            u32 x = src(t, env, &in->a) >> (8 * sm86_field(in, 76, 2)), mask = in->b.value;
            for (u32 i = 0; i < 7; i++)
                if (mask & (1u << i)) pset(t, i, (x >> i) & 1, 0);
            return NULL;
        }
        case SM86_PLOP3: {
            int x = pget(t, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            int y = pget(t, sm86_field(in, 77, 3), sm86_field(in, 80, 1), 0);
            int z = pget(t, sm86_field(in, 68, 3), sm86_field(in, 71, 1), 0);
            u32 lut0 = sm86_field(in, 64, 3) | sm86_field(in, 72, 5) << 3, lut1 = sm86_field(in, 16, 8);
            u32 index = (u32)(x << 2 | y << 1 | z);
            pset(t, sm86_field(in, 81, 3), (lut0 >> index) & 1, 0);
            pset(t, sm86_field(in, 84, 3), (lut1 >> index) & 1, 0);
            return NULL;
        }
        case SM86_FADD:
        case SM86_FMUL:
        case SM86_FFMA: {
            int on = (int)sm86_field(in, 80, 1);
            u32 scale = sm86_field(in, 84, 3);
            vx = srcf(t, env, &in->a, on);
            vy = srcf(t, env, op == SM86_FADD ? second(in) : &in->b, on);
            vz = op == SM86_FFMA ? srcf(t, env, &in->c, on) : 0.0f;
            float r;
            fesetround(rounding[sm86_field(in, 78, 2)]);
            if (op == SM86_FADD)
                r = vx + vy;
            else if (op == SM86_FFMA)
                r = fmaf(vx, vy, vz);
            else if (scale == 4)
                r = vx * vy;
            else {
                /* the product, exact in a double, times 2^(scale - 4), then rounded once */
                vd = (double)vx * (double)vy * ldexp(1.0, (int)scale - 4);
                r = (float)vd;
            }
            fesetround(FE_TONEAREST);
            rset(t, in->dst, nan_canon(u_of(sat(flush(r, on), (int)sm86_field(in, 77, 1)))));
            return NULL;
        }
        case SM86_FMNMX: {
            int on = (int)sm86_field(in, 80, 1);
            float x = srcf(t, env, &in->a, on), y = srcf(t, env, &in->b, on);
            int min = pget(t, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0);
            rset(t, in->dst, nan_canon(u_of(min ? fminf(x, y) : fmaxf(x, y)))); /* the operand that is not NaN */
            return NULL;
        }
        case SM86_FSET: {
            int on = (int)sm86_field(in, 80, 1);
            int cmp = fcmp(sm86_field(in, 76, 4), srcf(t, env, &in->a, on), srcf(t, env, &in->b, on));
            cmp = combine(sm86_field(in, 74, 2), cmp, pget(t, sm86_field(in, 87, 3), sm86_field(in, 90, 1), 0));
            rset(t, in->dst, u_of(cmp ? 1.0f : 0.0f));
            return NULL;
        }
        case SM86_MUFU: {
            float x = srcf(t, env, &in->b, 0), r;
            switch (sm86_field(in, 74, 6)) {
                case 0: r = (float)cos(6.283185307179586 * x); break;
                case 1: r = (float)sin(6.283185307179586 * x); break;
                case 2: r = exp2f(x); break;
                case 3: r = log2f(x); break;
                case 4: r = 1.0f / x; break;
                case 5: r = 1.0f / sqrtf(x); break;
                case 8: r = sqrtf(x); break;
                default: return "MUFU function";
            }
            rset(t, in->dst, nan_canon(u_of(r)));
            return NULL;
        }
        case SM86_F2I: {
            /* the hardware saturates, and a NaN gives 0 */
            float x = srcf(t, env, &in->b, (int)sm86_field(in, 80, 1));
            u32 mode = sm86_field(in, 78, 2);
            double v = mode == 0 ? nearbyint(x) : mode == 1 ? floor(x) : mode == 2 ? ceil(x) : trunc(x);
            u32 r;
            if (isnan(x))
                r = 0;
            else if (sm86_field(in, 72, 1))
                r = v >= 2147483648.0 ? 0x7fffffffu : v <= -2147483648.0 ? 0x80000000u : (u32)(int32_t)v;
            else
                r = v >= 4294967296.0 ? 0xffffffffu : v <= 0.0 ? 0 : (u32)v;
            rset(t, in->dst, r);
            return NULL;
        }
        case SM86_I2F:
        case SM86_I2FP: {
            u32 x = src(t, env, &in->b);
            float r;
            fesetround(rounding[sm86_field(in, 78, 2)]);
            if (sm86_field(in, 74, 1)) {
                volatile int32_t s = x < 0x80000000u ? (int32_t)x : -(int32_t)(0xffffffffu - x) - 1; /* signed */
                r = (float)s;
            } else {
                volatile u32 u = x;
                r = (float)u;
            }
            fesetround(FE_TONEAREST);
            rset(t, in->dst, u_of(r));
            return NULL;
        }
        case SM86_FRND: {
            float x = srcf(t, env, &in->b, 0);
            u32 mode = sm86_field(in, 78, 2);
            rset(t, in->dst,
                 u_of(mode == 0   ? nearbyintf(x)
                      : mode == 1 ? floorf(x)
                      : mode == 2 ? ceilf(x)
                                  : truncf(x)));
            return NULL;
        }
        case SM86_LDG:
        case SM86_STG:
        case SM86_LDG_NOU:
        case SM86_STG_NOU: {
            /* the uniform form: R (a pair with bit 90) + UR (a pair with bit 72); the other: R, a pair with bit 72 */
            int load = op == SM86_LDG || op == SM86_LDG_NOU, uniform_form = op == SM86_LDG || op == SM86_STG;
            u32 ur = uniform_form ? sm86_field(in, load ? 32 : 64, 6) : 63, reg = sm86_field(in, 24, 8);
            u32 imm = sm86_field(in, 40, 24);
            u64 base = ur >= 63                ? 0
                       : sm86_field(in, 72, 1) ? ((u64)urget(t, ur + 1) << 32) | urget(t, ur)
                                               : urget(t, ur);
            int pair = uniform_form ? (int)sm86_field(in, 90, 1) : (int)sm86_field(in, 72, 1);
            u64 at = pair ? ((u64)rget(t, reg + 1) << 32) | rget(t, reg) : rget(t, reg);
            u64 va = base + at + (imm & 0x800000u ? (u64)imm - 0x1000000u : imm);
            u32 type = sm86_field(in, 73, 3), words = type == 5 ? 2 : type == 6 ? 4 : 1;
            u8* p = global_at(env, va, 4 * words);
            if (!p) return "global address outside the buffers";
            for (u32 k = 0; k < words; k++) {
                if (load)
                    rset(t, in->dst + k,
                         (u32)p[4 * k] | (u32)p[4 * k + 1] << 8 | (u32)p[4 * k + 2] << 16 | (u32)p[4 * k + 3] << 24);
                else {
                    u32 v = rget(t, sm86_field(in, 32, 8) + k);
                    for (int b = 0; b < 4; b++) p[4 * k + b] = (u8)(v >> (8 * b));
                }
            }
            return NULL;
        }
        case SM86_LDS:
        case SM86_STS:
        case SM86_STS_NOU: {
            static const u8 shifts[4] = {0, 2, 3, 4};
            u32 imm = sm86_field(in, 40, 24);
            u32 at = (rget(t, sm86_field(in, 24, 8)) << shifts[sm86_field(in, 78, 2)]) +
                     (imm & 0x800000u ? imm | 0xff000000u : imm);
            if (op != SM86_STS_NOU) at += urget(t, sm86_field(in, op == SM86_LDS ? 32 : 64, 6));
            u32 type = sm86_field(in, 73, 3), words = type == 5 ? 2 : type == 6 ? 4 : 1;
            if (at + 4 * words > env->shared_bytes || (at & 3)) return "shared address outside the memory";
            for (u32 k = 0; k < words; k++) {
                if (op == SM86_LDS)
                    rset(t, in->dst + k, ((u32*)(void*)shared)[at / 4 + k]);
                else
                    ((u32*)(void*)shared)[at / 4 + k] = rget(t, sm86_field(in, 32, 8) + k);
            }
            return NULL;
        }
        default: return sm86_ops[op].name;
    }
}

const char* sm86_ref_run(const u8* code, u32 bytes, Sm86RefEnv* env) {
    static char message[96];
    u32 n = bytes / 16;
    static Sm86Insn insn[4096];
    if (n > 4096) return "program too long";
    for (u32 i = 0; i < n; i++) {
        u64 lo = 0, hi = 0;
        for (int k = 0; k < 8; k++) {
            lo |= (u64)code[16 * i + k] << (8 * k);
            hi |= (u64)code[16 * i + 8 + k] << (8 * k);
        }
        if (!sm86_decode(lo, hi, &insn[i])) return "unknown opcode";
    }
    static u32 shared_words[16384];
    if (env->shared_bytes > sizeof shared_words) return "shared memory too large";
    for (u32 gz = 0; gz < env->groups[2]; gz++)
        for (u32 gy = 0; gy < env->groups[1]; gy++)
            for (u32 gx = 0; gx < env->groups[0]; gx++) {
                memset(shared_words, 0, sizeof shared_words);
                for (u32 lz = 0; lz < env->local[2]; lz++)
                    for (u32 ly = 0; ly < env->local[1]; ly++)
                        for (u32 lx = 0; lx < env->local[0]; lx++) {
                            static Thread t;
                            memset(&t, 0, sizeof t);
                            t.tid[0] = lx, t.tid[1] = ly, t.tid[2] = lz;
                            t.ctaid[0] = gx, t.ctaid[1] = gy, t.ctaid[2] = gz;
                            u32 pc = 0, steps = 0;
                            int done = 0;
                            while (!done) {
                                if (pc >= n || ++steps > 1000000) return "ran off the program";
                                const Sm86Insn* in = &insn[pc];
                                int run = pget(&t, in->pred, in->pred_not, 0);
                                if (in->op == SM86_BRA) {
                                    pc = run ? sm86_branch_target(in, pc) : pc + 1;
                                    continue;
                                }
                                if (run) {
                                    const char* why = step(&t, env, in, (u8*)(void*)shared_words, &done);
                                    if (why) {
                                        snprintf(message, sizeof message, "instruction %u: %s", pc, why);
                                        return message;
                                    }
                                }
                                pc++;
                            }
                        }
            }
    return NULL;
}
