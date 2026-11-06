/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SPIR-V evaluator of the tests (spv_eval.h). Every id keeps a value of up to four components, each 64 bits.
   Memory is words of 32 bits: a scalar of 64 bits takes two, a vector, array or struct its parts one after the
   other, without the padding that decorations ask for, as the translators' blocks have none. A pointer is a block
   and a word offset in it. */
#include "spv_eval.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { K_NONE, K_TYPE, K_CONST, K_VAR, K_VALUE, K_LABEL, K_EXT };
typedef struct {
    uint64_t c[4];
} Val;
typedef struct {
    uint8_t kind;
    uint16_t op;       /* a type's opcode */
    uint32_t type;     /* a value's, constant's or variable's type */
    uint32_t a, b;     /* a type: its element type and count (int and float: width, signedness) */
    uint32_t at;       /* a label: its word in the module */
    uint32_t block;    /* a variable: its memory block */
    uint32_t builtin;  /* a variable: its BuiltIn + 1 */
    uint32_t location; /* a variable: its Location + 1 */
    uint32_t binding;  /* a variable: its Binding + 1 */
    uint32_t sclass;   /* a pointer type or a variable: its storage class */
    Val v;
} Id;
typedef struct {
    uint32_t* words;
    uint32_t n;
    int owned;
} Block;
typedef struct {
    const uint32_t* w;
    size_t count;
    Id* ids;
    uint32_t bound;
    Block* blocks;
    uint32_t nblocks, room;
    SpvEvalEnv* env;
} Ev;

static uint32_t type_words(Ev* e, uint32_t t);
static uint32_t struct_member(Ev* e, uint32_t t, uint32_t m, uint32_t* off);

static int type_ok(Ev* e, uint32_t t) { return t < e->bound && e->ids[t].kind == K_TYPE; }
/* the struct's members are in the module after its result id */
static const uint32_t* struct_words(Ev* e, uint32_t t, uint32_t* n) {
    const uint32_t* p = e->w + e->ids[t].at;
    *n = (p[0] >> 16) - 2;
    return p + 2;
}
static uint32_t type_words(Ev* e, uint32_t t) {
    if (!type_ok(e, t)) return 0;
    const Id* y = &e->ids[t];
    switch (y->op) {
        case 20: return 1;
        case 21:
        case 22: return y->a == 64 ? 2 : 1;
        case 23: return y->b * type_words(e, y->a);
        case 28: return (uint32_t)e->ids[y->b].v.c[0] * type_words(e, y->a);
        case 29: return 0;
        case 30: {
            uint32_t n, s = 0;
            const uint32_t* m = struct_words(e, t, &n);
            for (uint32_t k = 0; k < n; k++) s += type_words(e, m[k]);
            return s;
        }
        default: return 0;
    }
}
static uint32_t struct_member(Ev* e, uint32_t t, uint32_t m, uint32_t* off) {
    uint32_t n, s = 0;
    const uint32_t* mem = struct_words(e, t, &n);
    for (uint32_t k = 0; k < m && k < n; k++) s += type_words(e, mem[k]);
    *off = s;
    return m < n ? mem[m] : 0;
}
static uint32_t components(Ev* e, uint32_t t) {
    const Id* y = &e->ids[t];
    return y->op == 23 ? y->b : 1;
}
static uint32_t scalar_of(Ev* e, uint32_t t) { return e->ids[t].op == 23 ? e->ids[t].a : t; }
static int is64(Ev* e, uint32_t t) {
    uint32_t s = scalar_of(e, t);
    return (e->ids[s].op == 21 || e->ids[s].op == 22) && e->ids[s].a == 64;
}
static int is_float(Ev* e, uint32_t t) { return e->ids[scalar_of(e, t)].op == 22; }

static float f32(uint64_t v) {
    uint32_t u = (uint32_t)v;
    float f;
    memcpy(&f, &u, 4);
    return f;
}
static uint64_t u_f32(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
static double f64(uint64_t v) {
    double d;
    memcpy(&d, &v, 8);
    return d;
}
static uint64_t u_f64(double d) {
    uint64_t u;
    memcpy(&u, &d, 8);
    return u;
}
/* a component kept to the width of type t */
static uint64_t trunc_to(Ev* e, uint32_t t, uint64_t v) {
    uint32_t s = scalar_of(e, t);
    if (e->ids[s].op == 20) return v != 0;
    return is64(e, s) ? v : (uint32_t)v;
}
static int64_t sext(Ev* e, uint32_t t, uint64_t v) { return is64(e, t) ? (int64_t)v : (int64_t)(int32_t)(uint32_t)v; }

static uint32_t* mem_at(Ev* e, uint64_t ptr, uint32_t words) {
    uint32_t b = (uint32_t)(ptr >> 32), off = (uint32_t)ptr;
    if (b >= e->nblocks || !e->blocks[b].words) return NULL;
    if (off > e->blocks[b].n || words > e->blocks[b].n - off) return NULL;
    return e->blocks[b].words + off;
}
static int load(Ev* e, uint32_t t, uint64_t ptr, Val* out) {
    uint32_t n = components(e, t), wide = is64(e, t) ? 2 : 1;
    if (e->ids[t].op != 23 && e->ids[t].op != 20 && e->ids[t].op != 21 && e->ids[t].op != 22) return 0;
    uint32_t* p = mem_at(e, ptr, n * wide);
    if (!p) return 0;
    *out = (Val){{0}};
    for (uint32_t k = 0; k < n; k++) out->c[k] = wide == 2 ? (uint64_t)p[2 * k] | (uint64_t)p[2 * k + 1] << 32 : p[k];
    return 1;
}
static int store(Ev* e, uint32_t t, uint64_t ptr, const Val* v) {
    uint32_t n = components(e, t), wide = is64(e, t) ? 2 : 1;
    if (e->ids[t].op != 23 && e->ids[t].op != 20 && e->ids[t].op != 21 && e->ids[t].op != 22) return 0;
    uint32_t* p = mem_at(e, ptr, n * wide);
    if (!p) return 0;
    for (uint32_t k = 0; k < n; k++)
        if (wide == 2) {
            p[2 * k] = (uint32_t)v->c[k];
            p[2 * k + 1] = (uint32_t)(v->c[k] >> 32);
        } else
            p[k] = (uint32_t)v->c[k];
    return 1;
}
static uint32_t new_block(Ev* e, uint32_t* words, uint32_t n, int owned) {
    if (e->nblocks == e->room) {
        uint32_t more = e->room ? 2 * e->room : 256;
        Block* grown = realloc(e->blocks, more * sizeof *grown);
        if (!grown) return 0xffffffffu;
        e->blocks = grown;
        e->room = more;
    }
    e->blocks[e->nblocks] = (Block){words, n, owned};
    return e->nblocks++;
}

/* GLSL.std.450 */
static const char* ext_inst(Ev* e, uint32_t t, uint32_t fn, const uint32_t* args, uint32_t nargs, Val* r) {
    uint32_t n = components(e, t);
    int fl = is_float(e, t);
    *r = (Val){{0}};
    if (fn == 58) { /* PackHalf2x16: x into the low half, y into the high one, each rounded to the nearest even */
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t f = (uint32_t)e->ids[args[0]].v.c[k], sign = (f >> 16) & 0x8000u, h;
            float a = fabsf(f32(f));
            if (a != a)
                h = 0x7e00u;
            else if (a >= 65520.0f)
                h = 0x7c00u;
            else if (a < 6.103515625e-05f) /* a subnormal half: in units of 2^-24 */
                h = (uint32_t)nearbyintf(ldexpf(a, 24));
            else {
                int ex;
                float m = frexpf(a, &ex); /* a = m 2^ex, m in [0.5, 1) */
                h = (uint32_t)(ex + 14) << 10;
                h += (uint32_t)nearbyintf(ldexpf(m, 11)) - 0x400u; /* a carry into the exponent is the next binade */
            }
            r->c[0] |= (uint64_t)(sign | h) << (16 * k);
        }
        return NULL;
    }
    if (fn == 62) { /* UnpackHalf2x16: the low half into x, the high one into y */
        for (uint32_t k = 0; k < 2; k++) {
            uint32_t h = (uint32_t)(e->ids[args[0]].v.c[0] >> (16 * k)) & 0xffffu;
            uint32_t sign = (h >> 15) << 31, exp = (h >> 10) & 31, man = h & 0x3ffu;
            if (exp == 31)
                r->c[k] = sign | 0x7f800000u | man << 13;
            else {
                float f = exp ? ldexpf((float)(man | 0x400u), (int)exp - 25) : ldexpf((float)man, -24);
                r->c[k] = u_f32(sign ? -f : f);
            }
        }
        return NULL;
    }
    for (uint32_t k = 0; k < n; k++) {
        uint64_t x = nargs > 0 ? e->ids[args[0]].v.c[k] : 0, y = nargs > 1 ? e->ids[args[1]].v.c[k] : 0,
                 z = nargs > 2 ? e->ids[args[2]].v.c[k] : 0;
        uint64_t v;
        if (fl && !is64(e, t)) {
            float a = f32(x), b = f32(y), c = f32(z);
            switch (fn) {
                case 1: v = u_f32(roundf(a)); break;
                case 2: v = u_f32(nearbyintf(a)); break;
                case 3: v = u_f32(truncf(a)); break;
                case 4: v = u_f32(fabsf(a)); break;
                case 8: v = u_f32(floorf(a)); break;
                case 9: v = u_f32(ceilf(a)); break;
                case 10: v = u_f32(a - floorf(a)); break;
                case 13: v = u_f32(sinf(a)); break;
                case 14: v = u_f32(cosf(a)); break;
                case 29: v = u_f32(exp2f(a)); break;
                case 30: v = u_f32(log2f(a)); break;
                case 31: v = u_f32(sqrtf(a)); break;
                case 32: v = u_f32(1.0f / sqrtf(a)); break;
                case 37:
                case 79: v = u_f32(fminf(a, b)); break;
                case 40:
                case 80: v = u_f32(fmaxf(a, b)); break;
                case 43:
                case 81: v = u_f32(fminf(fmaxf(a, b), c)); break;
                case 50: v = u_f32(fmaf(a, b, c)); break;
                default: return "GLSL function";
            }
        } else if (!fl) {
            int64_t sa = sext(e, t, x), sb = sext(e, t, y), sc = sext(e, t, z);
            uint64_t ua = trunc_to(e, t, x), ub = trunc_to(e, t, y), uc = trunc_to(e, t, z);
            switch (fn) {
                case 38: v = ua < ub ? ua : ub; break;
                case 39: v = (uint64_t)(sa < sb ? sa : sb); break;
                case 41: v = ua > ub ? ua : ub; break;
                case 42: v = (uint64_t)(sa > sb ? sa : sb); break;
                case 44: v = ua < ub ? ub : ua > uc ? uc : ua; break;
                case 45: v = (uint64_t)(sa < sb ? sb : sa > sc ? sc : sa); break;
                case 73: {
                    v = 0xffffffffu;
                    for (int bit = 0; bit < 32; bit++)
                        if (ua >> bit & 1) {
                            v = (uint64_t)bit;
                            break;
                        }
                    break;
                }
                case 74:
                case 75: {
                    uint32_t u = (uint32_t)ua;
                    if (fn == 74 && (u >> 31)) u = ~u;
                    v = 0xffffffffu;
                    for (int bit = 31; bit >= 0; bit--)
                        if (u >> bit & 1) {
                            v = (uint64_t)bit;
                            break;
                        }
                    break;
                }
                default: return "GLSL function";
            }
        } else
            return "GLSL function on doubles";
        r->c[k] = trunc_to(e, t, v);
    }
    return NULL;
}

static uint64_t binary(Ev* e, uint32_t op, uint32_t t, uint32_t ta, uint64_t x, uint64_t y, int* bad) {
    int wide = is64(e, ta);
    switch (op) {
        case 128: return x + y;
        case 130: return x - y;
        case 132: return x * y;
        case 134: return trunc_to(e, ta, y) ? trunc_to(e, ta, x) / trunc_to(e, ta, y) : 0;
        case 137: return trunc_to(e, ta, y) ? trunc_to(e, ta, x) % trunc_to(e, ta, y) : 0;
        case 129: return wide ? u_f64(f64(x) + f64(y)) : u_f32(f32(x) + f32(y));
        case 131: return wide ? u_f64(f64(x) - f64(y)) : u_f32(f32(x) - f32(y));
        case 133: return wide ? u_f64(f64(x) * f64(y)) : u_f32(f32(x) * f32(y));
        case 136: return wide ? u_f64(f64(x) / f64(y)) : u_f32(f32(x) / f32(y));
        case 194: return trunc_to(e, ta, x) >> (y & (wide ? 63 : 31));
        case 195: return (uint64_t)(sext(e, ta, x) >> (y & (wide ? 63 : 31)));
        case 196: return x << (y & (wide ? 63 : 31));
        case 197: return x | y;
        case 198: return x ^ y;
        case 199: return x & y;
        case 164: return (x != 0) == (y != 0);
        case 165: return (x != 0) != (y != 0);
        case 166: return x || y;
        case 167: return x && y;
        case 170: return trunc_to(e, ta, x) == trunc_to(e, ta, y);
        case 171: return trunc_to(e, ta, x) != trunc_to(e, ta, y);
        case 172: return trunc_to(e, ta, x) > trunc_to(e, ta, y);
        case 173: return sext(e, ta, x) > sext(e, ta, y);
        case 174: return trunc_to(e, ta, x) >= trunc_to(e, ta, y);
        case 175: return sext(e, ta, x) >= sext(e, ta, y);
        case 176: return trunc_to(e, ta, x) < trunc_to(e, ta, y);
        case 177: return sext(e, ta, x) < sext(e, ta, y);
        case 178: return trunc_to(e, ta, x) <= trunc_to(e, ta, y);
        case 179: return sext(e, ta, x) <= sext(e, ta, y);
        default: break;
    }
    if (op >= 180 && op <= 191) {
        double a = wide ? f64(x) : f32(x), b = wide ? f64(y) : f32(y);
        int unord = isnan(a) || isnan(b), r;
        switch ((op - 180) / 2) {
            case 0: r = a == b; break;
            case 1: r = a != b; break;
            case 2: r = a < b; break;
            case 3: r = a > b; break;
            case 4: r = a <= b; break;
            default: r = a >= b; break;
        }
        if (op & 1) return unord || r; /* the unordered forms are the odd opcodes */
        return !unord && r;
    }
    (void)t;
    *bad = 1;
    return 0;
}

static const char* run(Ev* e, uint32_t entry_at) {
    uint32_t pc = entry_at, prev_label = 0, cur_label = 0, steps = 0,
             max = e->env->steps_max ? e->env->steps_max : 1000000u;
    /* the calls that run: where each returns to, and its labels (for OpPhi) */
    uint32_t back[16][3], depth = 0;
    for (;;) {
        if (pc >= e->count || !(e->w[pc] >> 16)) return "ran past the module";
        if (++steps > max) return "too many steps";
        const uint32_t* in = e->w + pc;
        uint32_t op = in[0] & 0xffff, len = in[0] >> 16;
        uint32_t next = pc + len;
        Id* r = len > 2 && in[2] < e->bound ? &e->ids[in[2]] : NULL;
        uint32_t t = len > 1 ? in[1] : 0;
        switch (op) {
            case 248: /* OpLabel */
                prev_label = cur_label;
                cur_label = in[1];
                break;
            case 59: { /* OpVariable in the function: its memory each time it is reached (once here) */
                uint32_t pointee = e->ids[t].a, n = type_words(e, pointee);
                uint32_t* m = calloc(n ? n : 1, 4);
                if (!m) return "out of memory";
                uint32_t b = new_block(e, m, n, 1);
                if (b == 0xffffffffu) {
                    free(m);
                    return "out of memory";
                }
                r->kind = K_VAR;
                r->type = t;
                r->v.c[0] = (uint64_t)b << 32;
                if (len > 4) {
                    Val init = e->ids[in[4]].v;
                    if (!store(e, pointee, r->v.c[0], &init)) return "variable initializer";
                }
                break;
            }
            case 61: { /* OpLoad */
                Val v;
                const Id* pt = &e->ids[t];
                if (pt->kind == K_TYPE && (pt->op == 25 || pt->op == 27)) { /* an image: its binding */
                    r->kind = K_VALUE;
                    r->type = t;
                    r->v = (Val){{e->ids[in[3]].binding}};
                    break;
                }
                if (!load(e, t, e->ids[in[3]].v.c[0], &v)) return "load";
                r->kind = K_VALUE;
                r->type = t;
                r->v = v;
                break;
            }
            case 62: /* OpStore */
                if (!store(e, e->ids[e->ids[in[1]].type].a, e->ids[in[1]].v.c[0], &e->ids[in[2]].v)) return "store";
                break;
            case 65:
            case 66: { /* OpAccessChain */
                uint64_t ptr = e->ids[in[3]].v.c[0];
                uint32_t cur = e->ids[e->ids[in[3]].type].a;
                for (uint32_t k = 4; k < len; k++) {
                    uint64_t idx = e->ids[in[k]].v.c[0];
                    const Id* y = &e->ids[cur];
                    if (y->op == 30) {
                        uint32_t off;
                        cur = struct_member(e, cur, (uint32_t)idx, &off);
                        ptr += off;
                    } else if (y->op == 23 || y->op == 28 || y->op == 29) {
                        ptr += (uint32_t)idx * type_words(e, y->a);
                        cur = y->a;
                    } else
                        return "access chain into a scalar";
                }
                r->kind = K_VALUE;
                r->type = t;
                r->v.c[0] = ptr;
                break;
            }
            case 86:  /* OpSampledImage */
            case 100: /* OpImage */
                r->kind = K_VALUE;
                r->type = t;
                r->v = e->ids[in[3]].v;
                break;
            case 87:   /* OpImageSampleImplicitLod */
            case 88:   /* OpImageSampleExplicitLod */
            case 95: { /* OpImageFetch */
                uint64_t b = e->ids[in[3]].v.c[0];
                if (!b || b > 64) return "a sample of an image with no binding";
                r->kind = K_VALUE;
                r->type = t;
                r->v = (Val){{0}};
                for (uint32_t k = 0; k < 4; k++) r->v.c[k] = e->env->texel[b - 1][k];
                break;
            }
            case 12: { /* OpExtInst */
                const char* why = ext_inst(e, t, in[4], in + 5, len - 5, &r->v);
                if (why) return why;
                r->kind = K_VALUE;
                r->type = t;
                break;
            }
            case 77: /* OpVectorExtractDynamic */
                r->kind = K_VALUE;
                r->type = t;
                r->v = (Val){{e->ids[in[3]].v.c[e->ids[in[4]].v.c[0] & 3]}};
                break;
            case 79: { /* OpVectorShuffle */
                Val v = {{0}};
                uint32_t na = components(e, e->ids[in[3]].type);
                for (uint32_t k = 5; k < len && k < 9; k++)
                    v.c[k - 5] = in[k] < na ? e->ids[in[3]].v.c[in[k]] : e->ids[in[4]].v.c[(in[k] - na) & 3];
                r->kind = K_VALUE;
                r->type = t;
                r->v = v;
                break;
            }
            case 80: { /* OpCompositeConstruct of scalars */
                Val v = {{0}};
                for (uint32_t k = 3; k < len && k < 7; k++) v.c[k - 3] = e->ids[in[k]].v.c[0];
                r->kind = K_VALUE;
                r->type = t;
                r->v = v;
                break;
            }
            case 81: /* OpCompositeExtract of a vector */
                r->kind = K_VALUE;
                r->type = t;
                r->v = (Val){{e->ids[in[3]].v.c[in[4] & 3]}};
                break;
            case 82: { /* OpCompositeInsert */
                Val v = e->ids[in[4]].v;
                v.c[in[5] & 3] = e->ids[in[3]].v.c[0];
                r->kind = K_VALUE;
                r->type = t;
                r->v = v;
                break;
            }
            case 83: /* OpCopyObject */
                r->kind = K_VALUE;
                r->type = t;
                r->v = e->ids[in[3]].v;
                break;
            case 124: { /* OpBitcast, between widths of the same total */
                Val v = e->ids[in[3]].v;
                uint32_t ts = e->ids[in[3]].type;
                if (components(e, ts) == components(e, t))
                    r->v = v;
                else if (components(e, ts) == 2 && is64(e, t))
                    r->v = (Val){{(v.c[0] & 0xffffffffu) | v.c[1] << 32}};
                else if (is64(e, ts) && components(e, t) == 2)
                    r->v = (Val){{v.c[0] & 0xffffffffu, v.c[0] >> 32}};
                else
                    return "bitcast";
                r->kind = K_VALUE;
                r->type = t;
                break;
            }
            case 109:
            case 110:
            case 111:
            case 112:
            case 113:
            case 114:
            case 115:
            case 126:
            case 127:
            case 168:
            case 200:
            case 204:
            case 205:
            case 156:
            case 157: {
                uint32_t ta = e->ids[in[3]].type;
                Val v = {{0}};
                for (uint32_t k = 0; k < components(e, t); k++) {
                    uint64_t x = e->ids[in[3]].v.c[k], y;
                    int sw = is64(e, ta), dw = is64(e, t);
                    double d = sw ? f64(x) : f32(x);
                    switch (op) {
                        case 109:
                            y = isnan(d) || d <= -1.0                               ? 0
                                : d >= (dw ? 18446744073709551615.0 : 4294967295.0) ? (dw ? ~0ull : 0xffffffffu)
                                                                                    : (uint64_t)d;
                            break;
                        case 110: {
                            double lo = dw ? -9223372036854775808.0 : -2147483648.0,
                                   hi = dw ? 9223372036854775807.0 : 2147483647.0;
                            y = isnan(d)  ? 0
                                : d <= lo ? (uint64_t)(int64_t)lo
                                : d >= hi ? (dw ? 0x7fffffffffffffffull : 0x7fffffffu)
                                          : (uint64_t)(int64_t)d;
                            break;
                        }
                        case 111: y = dw ? u_f64((double)sext(e, ta, x)) : u_f32((float)sext(e, ta, x)); break;
                        case 112: y = dw ? u_f64((double)trunc_to(e, ta, x)) : u_f32((float)trunc_to(e, ta, x)); break;
                        case 113: y = trunc_to(e, ta, x); break;
                        case 114: y = (uint64_t)sext(e, ta, x); break;
                        case 115:
                            y = dw ? u_f64(sw ? f64(x) : (double)f32(x)) : u_f32(sw ? (float)f64(x) : f32(x));
                            break;
                        case 126: y = 0 - x; break;
                        case 127: y = sw ? x ^ 0x8000000000000000ull : x ^ 0x80000000u; break;
                        case 168: y = !x; break;
                        case 200: y = ~x; break;
                        case 204: {
                            uint32_t u = (uint32_t)x, b = 0;
                            for (int i = 0; i < 32; i++) b |= (u >> i & 1) << (31 - i);
                            y = b;
                            break;
                        }
                        case 205: {
                            uint32_t u = (uint32_t)x, b = 0;
                            while (u) {
                                b += u & 1;
                                u >>= 1;
                            }
                            y = b;
                            break;
                        }
                        case 156: y = isnan(d); break;
                        default: y = isinf(d); break;
                    }
                    v.c[k] = trunc_to(e, t, y);
                }
                r->kind = K_VALUE;
                r->type = t;
                r->v = v;
                break;
            }
            case 169: { /* OpSelect */
                Val v = {{0}};
                for (uint32_t k = 0; k < components(e, t); k++) {
                    uint64_t cond = e->ids[in[3]].v.c[components(e, e->ids[in[3]].type) > 1 ? k : 0];
                    v.c[k] = cond ? e->ids[in[4]].v.c[k] : e->ids[in[5]].v.c[k];
                }
                r->kind = K_VALUE;
                r->type = t;
                r->v = v;
                break;
            }
            case 201:
            case 202:
            case 203: { /* OpBitFieldInsert, SExtract, UExtract */
                Val v = {{0}};
                for (uint32_t k = 0; k < components(e, t); k++) {
                    uint64_t base = e->ids[in[3]].v.c[k];
                    uint32_t off = (uint32_t)e->ids[in[op == 201 ? 5 : 4]].v.c[0],
                             cnt = (uint32_t)e->ids[in[op == 201 ? 6 : 5]].v.c[0];
                    uint64_t mask = cnt >= 64 ? ~0ull : ((1ull << cnt) - 1);
                    if (op == 201)
                        v.c[k] = (base & ~(mask << off)) | ((e->ids[in[4]].v.c[k] & mask) << off);
                    else {
                        uint64_t f = (base >> off) & mask;
                        if (op == 202 && cnt && (f >> (cnt - 1) & 1)) f |= ~mask;
                        v.c[k] = f;
                    }
                    v.c[k] = trunc_to(e, t, v.c[k]);
                }
                r->kind = K_VALUE;
                r->type = t;
                r->v = v;
                break;
            }
            case 149:
            case 150:
            case 151:
            case 152: /* IAddCarry, ISubBorrow, UMulExtended, SMulExtended: results as a struct of two */
                return "extended arithmetic";
            case 245: { /* OpPhi */
                for (uint32_t k = 3; k + 1 < len; k += 2)
                    if (in[k + 1] == prev_label) {
                        r->kind = K_VALUE;
                        r->type = t;
                        r->v = e->ids[in[k]].v;
                    }
                break;
            }
            case 246:
            case 247: /* the merges */ break;
            case 249: next = e->ids[in[1]].at; break;
            case 250: next = e->ids[e->ids[in[1]].v.c[0] ? in[2] : in[3]].at; break;
            case 251: {
                uint64_t sel = trunc_to(e, e->ids[in[1]].type, e->ids[in[1]].v.c[0]);
                uint32_t target = in[2];
                for (uint32_t k = 3; k + 1 < len; k += 2)
                    if (in[k] == (uint32_t)sel) target = in[k + 1];
                next = e->ids[target].at;
                break;
            }
            case 252:
            case 4416: e->env->killed = 1; return NULL;
            case 253: /* OpReturn */
                if (!depth) return NULL;
                depth--;
                next = back[depth][0], cur_label = back[depth][1], prev_label = back[depth][2];
                break;
            case 57: /* OpFunctionCall of a function without parameters */
                if (len != 4) return "a call with arguments";
                if (depth == 16) return "calls too deep";
                if (!e->ids[in[3]].at) return "a call of no function";
                back[depth][0] = next, back[depth][1] = cur_label, back[depth][2] = prev_label;
                depth++;
                next = e->ids[in[3]].at;
                break;
            case 224: /* OpControlBarrier, OpMemoryBarrier: the invocation is alone */
            case 225: break;
            case 5295: /* OpSetMeshOutputsEXT */
                e->env->mesh_outputs[0] = (uint32_t)e->ids[in[1]].v.c[0];
                e->env->mesh_outputs[1] = (uint32_t)e->ids[in[2]].v.c[0];
                e->env->set_outputs++;
                break;
            case 5294: /* OpEmitMeshTasksEXT, which ends the invocation */
                for (uint32_t k = 0; k < 3; k++) e->env->mesh_tasks[k] = (uint32_t)e->ids[in[1 + k]].v.c[0];
                e->env->emitted_tasks++;
                return NULL;
            case 5380: e->env->killed = 1; break; /* demoted: it runs on, its outputs dropped */
            case 333:                             /* Elect */
            case 334:                             /* All */
            case 335:                             /* Any */
            case 336:                             /* AllEqual */
                r->kind = K_VALUE;
                r->type = t;
                r->v = (Val){{op == 333 || op == 336 ? 1 : e->ids[in[4]].v.c[0] != 0}};
                break;
            case 337: /* Broadcast */
            case 338: /* BroadcastFirst */
            case 345: /* Shuffle: the invocation is alone, so every lane's value is its own */
            case 346:
                r->kind = K_VALUE;
                r->type = t;
                r->v = e->ids[in[4]].v;
                break;
            case 339: { /* Ballot */
                uint32_t lane = e->env->builtin[41][0];
                r->kind = K_VALUE;
                r->type = t;
                r->v = (Val){{0}};
                if (e->ids[in[4]].v.c[0]) r->v.c[(lane >> 5) & 3] = 1ull << (lane & 31);
                break;
            }
            default:
                if (op >= 227 && op <= 242) { /* atomics on one invocation's memory */
                    uint64_t ptr = e->ids[in[3]].v.c[0];
                    Val old;
                    if (!load(e, t, ptr, &old)) return "atomic";
                    r->kind = K_VALUE;
                    r->type = t;
                    r->v = old;
                    if (op == 227) break;
                    if (op == 228) {
                        if (!store(e, e->ids[e->ids[in[1]].type].a, e->ids[in[1]].v.c[0], &e->ids[in[4]].v))
                            return "atomic store";
                        r->kind = K_NONE;
                        break;
                    }
                    uint64_t x = old.c[0],
                             y = op == 230                ? e->ids[in[7]].v.c[0]
                                 : op >= 234 || op == 229 ? e->ids[in[6]].v.c[0]
                                                          : 1,
                             nv;
                    switch (op) {
                        case 229: nv = y; break;
                        case 230: nv = trunc_to(e, t, x) == trunc_to(e, t, y) ? e->ids[in[6]].v.c[0] : x; break;
                        case 232: nv = x + 1; break;
                        case 233: nv = x - 1; break;
                        case 234: nv = x + y; break;
                        case 235: nv = x - y; break;
                        case 236: nv = sext(e, t, x) < sext(e, t, y) ? x : y; break;
                        case 237: nv = trunc_to(e, t, x) < trunc_to(e, t, y) ? x : y; break;
                        case 238: nv = sext(e, t, x) > sext(e, t, y) ? x : y; break;
                        case 239: nv = trunc_to(e, t, x) > trunc_to(e, t, y) ? x : y; break;
                        case 240: nv = x & y; break;
                        case 241: nv = x | y; break;
                        default: nv = x ^ y; break;
                    }
                    Val nvv = {{trunc_to(e, t, nv)}};
                    if (!store(e, t, ptr, &nvv)) return "atomic";
                    break;
                }
                if ((op >= 128 && op <= 141) || (op >= 164 && op <= 199)) {
                    uint32_t ta = e->ids[in[3]].type;
                    Val v = {{0}};
                    int bad = 0;
                    for (uint32_t k = 0; k < components(e, t); k++)
                        v.c[k] = trunc_to(e, t, binary(e, op, t, ta, e->ids[in[3]].v.c[k], e->ids[in[4]].v.c[k], &bad));
                    if (bad) return "an arithmetic opcode";
                    r->kind = K_VALUE;
                    r->type = t;
                    r->v = v;
                    break;
                }
                return "an opcode";
        }
        pc = next;
    }
}

const char* spv_eval(const uint32_t* w, size_t count, SpvEvalEnv* env) {
    if (count < 5 || w[0] != 0x07230203u) return "not SPIR-V";
    Ev* e = calloc(1, sizeof *e);
    if (!e) return "out of memory";
    e->w = w;
    e->count = count;
    e->bound = w[3];
    e->env = env;
    e->ids = calloc(e->bound + 1, sizeof *e->ids);
    if (!e->ids) {
        free(e);
        return "out of memory";
    }
    new_block(e, NULL, 0, 0); /* block 0: none */
    const char* why = NULL;
    uint32_t entry = 0, entry_at = 0;
    /* the declarations, then the variables' memory */
    for (size_t pc = 5; pc < count && (w[pc] >> 16); pc += w[pc] >> 16) {
        const uint32_t* in = w + pc;
        uint32_t op = in[0] & 0xffff, len = in[0] >> 16;
        if (op == 15 && !entry) entry = in[2];
        if (op >= 19 && op <= 39 && len > 1 && in[1] < e->bound) {
            Id* y = &e->ids[in[1]];
            y->kind = K_TYPE;
            y->op = (uint16_t)op;
            y->at = (uint32_t)pc;
            if (op == 21 || op == 22 || op == 23 || op == 28) {
                y->a = in[2];
                y->b = len > 3 ? in[3] : 0;
            } else if (op == 29)
                y->a = in[2];
            else if (op == 32) {
                y->sclass = in[2];
                y->a = in[3];
            }
        }
        if ((op == 41 || op == 42 || op == 43 || op == 44 || op == 46) && in[2] < e->bound) {
            Id* c = &e->ids[in[2]];
            c->kind = K_CONST;
            c->type = in[1];
            if (op == 41) c->v.c[0] = 1;
            if (op == 43) c->v.c[0] = len > 4 ? (uint64_t)in[3] | (uint64_t)in[4] << 32 : in[3];
            if (op == 44)
                for (uint32_t k = 3; k < len && k < 7; k++) c->v.c[k - 3] = e->ids[in[k]].v.c[0];
        }
        if (op == 11 && in[1] < e->bound) e->ids[in[1]].kind = K_EXT;
        if (op == 71 && in[1] < e->bound && len > 3) {
            Id* v = &e->ids[in[1]];
            if (in[2] == 11) v->builtin = in[3] + 1;
            if (in[2] == 30) v->location = in[3] + 1;
            if (in[2] == 33) v->binding = in[3] + 1;
        }
        if (op == 248 && in[1] < e->bound) {
            e->ids[in[1]].kind = K_LABEL;
            e->ids[in[1]].at = (uint32_t)pc;
        }
        if (op == 54 && in[2] == entry) entry_at = (uint32_t)(pc + len);
        if (op == 54 && in[2] < e->bound) e->ids[in[2]].at = (uint32_t)(pc + len); /* a function: its first word */
    }
    for (size_t pc = 5; pc < count && (w[pc] >> 16) && !why; pc += w[pc] >> 16) {
        const uint32_t* in = w + pc;
        uint32_t op = in[0] & 0xffff, len = in[0] >> 16;
        if (op == 54) break; /* the functions' own variables are made as they are reached */
        if (op != 59) continue;
        Id* v = &e->ids[in[2]];
        uint32_t sc = in[3], pointee = e->ids[in[1]].a, n = type_words(e, pointee);
        v->kind = K_VAR;
        v->type = in[1];
        v->sclass = sc;
        uint32_t b;
        if ((sc == 2 || sc == 12) && v->binding) { /* Uniform, StorageBuffer */
            uint32_t k = v->binding - 1;
            int bound = k < SPV_EVAL_BINDINGS;
            b = new_block(e, bound ? env->ubo[k] : NULL, bound ? env->ubo_words[k] : 0, 0);
        } else {
            uint32_t* m = calloc(n ? n : 1, 4);
            if (!m) {
                why = "out of memory";
                break;
            }
            if (sc == 1 && v->builtin && v->builtin - 1 < 64)
                memcpy(m, env->builtin[v->builtin - 1], (n < 4 ? n : 4) * 4);
            if (sc == 1 && !v->builtin && v->location && v->location - 1 < 32)
                memcpy(m, env->in[v->location - 1], (n < 4 ? n : 4) * 4);
            if (sc == 9) memcpy(m, env->push, (n < 20 ? n : 20) * 4);
            if (sc == 5402) memcpy(m, env->payload, (n < 1024 ? n : 1024) * 4); /* TaskPayloadWorkgroupEXT */
            b = new_block(e, m, n, 1);
            if (b != 0xffffffffu && len > 4) {
                Val init = e->ids[in[4]].v;
                store(e, pointee, (uint64_t)b << 32, &init);
            }
        }
        if (b == 0xffffffffu) why = "out of memory";
        v->block = b;
        v->v.c[0] = (uint64_t)b << 32;
    }
    if (!why && !entry_at) why = "no entry point";
    if (!why) why = run(e, entry_at);
    /* the outputs, by location, FragDepth (BuiltIn 22) and Position (BuiltIn 0) */
    for (uint32_t id = 0; id < e->bound; id++) {
        Id* v = &e->ids[id];
        if (v->kind != K_VAR || v->sclass != 3) continue;
        uint32_t* m = e->blocks[v->block].words;
        uint32_t n = e->blocks[v->block].n;
        if (!m) continue;
        if (v->location && v->location - 1 < 32) {
            for (uint32_t k = 0; k < 4 && k < n; k++) env->out[v->location - 1][k] = m[k];
            env->out_written[v->location - 1] = 1;
        }
        if (v->builtin == 22 + 1) env->frag_depth = m[0];
        if (v->builtin == 0 + 1)
            for (uint32_t k = 0; k < 4 && k < n; k++) env->position[k] = m[k];
        /* an array's elements */
        uint32_t slot = v->location ? v->location - 1 : v->builtin == 1 ? 32 : v->builtin == 5296 + 1 ? 33 : 34;
        if (e->ids[e->ids[v->type].a].op == 28 && slot < 34 && env->arrays[slot])
            memcpy(env->arrays[slot], m, (n < env->array_words[slot] ? n : env->array_words[slot]) * 4);
    }
    for (uint32_t id = 0; id < e->bound; id++) {
        Id* v = &e->ids[id];
        if (v->kind == K_VAR && v->sclass == 5402 && e->blocks[v->block].words)
            memcpy(env->payload, e->blocks[v->block].words,
                   (e->blocks[v->block].n < 1024 ? e->blocks[v->block].n : 1024) * 4);
    }
    for (uint32_t id = 0; id < e->bound; id++) {
        Id* v = &e->ids[id];
        if (v->kind != K_VAR || v->sclass != 4 || env->shared_words || !e->blocks[v->block].words) continue;
        env->shared_words = e->blocks[v->block].n < 4096 ? e->blocks[v->block].n : 4096;
        memcpy(env->shared, e->blocks[v->block].words, env->shared_words * 4);
    }
    for (uint32_t b = 0; b < e->nblocks; b++)
        if (e->blocks[b].owned) free(e->blocks[b].words);
    free(e->blocks);
    free(e->ids);
    free(e);
    return why;
}
