/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --encoders: an encoder for each decode clause of the Sail, the instruction table a back end that writes
   that architecture's own code takes its encodings from (README.md, "Encoders"). A clause's pattern fixes some of
   the word's bits; its lets slice the others into the decode function's fields (`let Rd = Slice(__opcode, 0, 5)`):
   the encoder is the fixed bits ORed with each field in its place, the fields the pattern fixes whole left out of
   its parameters. Nothing about any instruction is written here: the clauses are the Sail's.

     sailgen --encoders PREFIX -o OUT INPUT... [--overlay FILE]... [--profile PROFILE] [--roots ROOTS]

   PREFIX names the clauses' scattered function (__DecodeA64: every __DecodeA64_* clause). With --roots, only the
   clauses that call a root of that file (a roots file's root lines); else every one. Each encoder is

     static inline SnxU32 sge_TAG_NAME(fields...)

   TAG the architecture's in PREFIX, lower case (a64), NAME the decode function's, less decode_ and its
   _aarch64_instrs_... (add_addsub_shift: ADD, shifted register); decode functions named alike get the shortest tail
   of their names that tells them apart (ldr_imm_gen_post_idx, ldr_imm_gen_pre_idx). With a table of every encoder's
   family (its scattered function's name past PREFIX: DataProcFPSIMD), fixed bits, mask and fields
   (sge_TAG_table), for the tests to walk and a back end to find a word's clause in; and each clause's fp, what its code
   reaches of the floating-point control and status registers (enc_fp: --overlay files over the inputs, --profile the
   decoder's features, as --generate takes them), which a back end that runs the word as the host's own instruction
   switches to the guest's FPCR for */
#include "sailgen.h"
#include <limits.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N(i) (g_parse.nodes[i])
static const char* name_of(unsigned node) { return sg_str(N(node).name); }

typedef struct {
    unsigned name; /* string offset */
    unsigned lo, width;
} Field;

typedef struct {
    unsigned def, decode;  /* the clause, the decode function's name (string offset) */
    int fp;                /* what its code reaches of FPCR and FPSR (enc_fp) */
    unsigned width;        /* 32, or 16 for a T16 halfword */
    SgU64 mask, value;     /* the fixed bits */
    Field fields[16];
    unsigned field_count;
    char short_name[160];
} Enc;

static void enc_bits(unsigned def, Enc* e) {
    unsigned p = g_parse.defs[def].params, x, i = 0, count = 0;
    if (N(p).kind != N_TUPLE || !N(N(p).a).next) sg_fail(p, "a decode clause's pattern is not (pc, (... as __opcode))");
    p = N(N(p).a).next;
    if (N(p).kind == N_BINOP && !strcmp(name_of(p), "as")) p = N(p).a;
    if (N(p).kind == N_BITS) {
        e->width = N(p).width;
        e->mask = e->width == 32 ? 0xffffffffull : 0xffffull;
        e->value = N(p).value;
        return;
    }
    if (N(p).kind != N_VEC) sg_fail(p, "a decode clause's pattern is not a bit vector");
    for (x = N(p).a; x; x = N(x).next) ++count;
    if (count != 32 && count != 16) sg_fail(p, "a decode pattern of %u bits", count);
    e->width = count;
    e->mask = e->value = 0;
    for (x = N(p).a; x; x = N(x).next, ++i) {
        unsigned bit = count - 1 - i;
        if (N(x).kind == N_WILD) continue;
        if (N(x).kind != N_ID || (strcmp(name_of(x), "bitone") && strcmp(name_of(x), "bitzero")))
            sg_fail(x, "a decode pattern bit that is not bitzero, bitone or _");
        e->mask |= 1ull << bit;
        if (!strcmp(name_of(x), "bitone")) e->value |= 1ull << bit;
    }
}

/* the decode function a clause calls, anywhere in its body (CFINV's under an if of its unpredictable bits) */
static unsigned enc_call(unsigned n) {
    unsigned r;
    if (!n) return 0;
    if (N(n).kind == N_CALL && !strncmp(name_of(n), "decode_", 7)) return N(n).name;
    if ((r = enc_call(N(n).a)) || (r = enc_call(N(n).b)) || (r = enc_call(N(n).c)) || (r = enc_call(N(n).d))) return r;
    return enc_call(N(n).next);
}

/* the clause's lets of slices of the word, and the decode function it calls */
static void enc_fields(unsigned def, Enc* e) {
    unsigned body = g_parse.defs[def].body, item;
    if (N(body).kind != N_BLOCK) sg_fail(body, "a decode clause's body is not a block");
    for (item = N(body).a; item; item = N(item).next) {
        unsigned v;
        if (N(item).kind != N_LET) continue;
        v = N(item).b;
        if (N(v).kind == N_CALL && !strcmp(name_of(v), "Slice")) {
            unsigned word = N(v).a, lo = N(word).next, n = lo ? N(lo).next : 0;
            Field* f;
            if (N(N(item).a).kind != N_ID || N(word).kind != N_ID || strcmp(name_of(word), "__opcode") || !n ||
                N(lo).kind != N_NUM || N(n).kind != N_NUM)
                sg_fail(item, "a decode clause's let that is not a field of the word");
            if (e->field_count == sizeof e->fields / sizeof e->fields[0]) sg_fail(item, "a clause of too many fields");
            f = &e->fields[e->field_count++];
            f->name = N(N(item).a).name;
            f->lo = (unsigned)N(lo).value;
            f->width = (unsigned)N(n).value;
        }
    }
    e->decode = enc_call(N(body).a);
    if (!e->decode) sg_fail(body, "a decode clause that calls no decode function");
}

/* ---- what a clause's code reaches of the floating-point environment (enc_fp) ----
   The clause's code walked as the generator folds it: the fields its pattern fixes whole are known, and so are the
   lets and parameters made of them; a feature the profile names in no feature line is false (IsFeatureImplemented, as
   the generator's), one it names unknown; an if, a match, an & or an | down its taken side alone where its condition
   is known, every side where it isn't. FPCR_read's value is followed as a value: reached where something reads it (a
   field, an operator, a condition), not where it is bound, returned or passed on to a function the input defines.
   FPCR or FPSR named is reached (read, or a field written: FPSR[QC]). A function the input declares (a val) but
   doesn't define is one the back end lowers (FPAdd; a64.roots' lower lines): a call of one is ENC_FP_LOWERED, what its
   lowering reads the back end's (FPRecipStepFused reads FPCR though none is passed to it), but for those of
   enc_fp_integer given no FPCR. Every guess is the one that reaches more: what isn't known is both sides, a loop's body
   twice */
enum { ENC_FP_NONE, ENC_FP_DIRECT, ENC_FP_LOWERED };
enum { EV_NONE, EV_BOOL, EV_INT, EV_BITS, EV_ENUM };
typedef struct {
    unsigned char known; /* v is the value */
    unsigned char kind;
    unsigned char fpcr; /* FPCR_read's value, or made of it */
    unsigned width;     /* EV_BITS */
    SgU64 v;            /* a bool's 0 or 1, the bits, an enum member's (ev_member's offset) */
    long long i;        /* an integer's */
} Ev;
typedef struct {
    unsigned def, count, next; /* the function, how many arguments, the next entry of its */
    unsigned char done, reach;
    Ev ret, args[24];
} EncCall;
static struct {
    unsigned *names, bound, cap, base; /* the names bound, from base the current function's */
    Ev* values;
    unsigned* members; /* every enum member's name, sorted (names are compared as text: no two share an offset) */
    unsigned member_count;
    unsigned char reach; /* ENC_FP_DIRECT and ENC_FP_LOWERED, as bits */
    unsigned uncertain;  /* under a condition not known: an assignment leaves its variable unknown */
    unsigned depth;      /* calls under way */
    Ev* ret;             /* the current function's result */
    int has_ret;
    SgU64 mask, value; /* the clause's fixed bits */
    char* features;    /* the profile's, " FEAT_A FEAT_B "; NULL without one: every feature unknown */
    EncCall* calls;    /* each function walked with its arguments */
    unsigned call_count, call_cap, *heads;
} g_enc;

static Ev ev_unknown(int fpcr) {
    Ev v = {0};
    v.fpcr = (unsigned char)fpcr;
    return v;
}
static Ev ev_known(int kind, SgU64 value, unsigned width) {
    Ev v = {0};
    v.known = 1, v.kind = (unsigned char)kind, v.v = value, v.width = width;
    return v;
}
static Ev ev_integer(long long value) {
    Ev v = ev_known(EV_INT, 0, 0);
    v.i = value;
    return v;
}
/* a and b known, and the same value */
static int ev_equal(Ev a, Ev b) {
    return a.known && b.known && a.kind == b.kind && a.v == b.v && a.i == b.i && a.width == b.width;
}
static int ev_same(Ev a, Ev b) { return a.known == b.known && a.fpcr == b.fpcr && (!a.known || ev_equal(a, b)); }
static Ev ev_merge(Ev a, Ev b) {
    Ev r = ev_equal(a, b) ? a : ev_unknown(0);
    r.fpcr = a.fpcr | b.fpcr;
    return r;
}
/* a value read: FPCR's is reached */
static void ev_use(Ev v) {
    if (v.fpcr) g_enc.reach |= ENC_FP_DIRECT;
}
/* the enum member named name: the one string offset that stands for it, 0 where none is named so */
static unsigned ev_member(unsigned name) {
    unsigned lo = 0, hi = g_enc.member_count;
    const char* text = sg_str(name);
    while (lo < hi) {
        unsigned mid = (lo + hi) / 2;
        int c = strcmp(sg_str(g_enc.members[mid]), text);
        if (!c) return g_enc.members[mid];
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}
static Ev* ev_lookup(unsigned name) {
    unsigned i;
    const char* text = sg_str(name);
    for (i = g_enc.bound; i > g_enc.base; --i)
        if (!strcmp(sg_str(g_enc.names[i - 1]), text)) return &g_enc.values[i - 1];
    return NULL;
}
static void ev_push(unsigned name, Ev v) {
    if (g_enc.bound == g_enc.cap) {
        g_enc.cap = g_enc.cap ? 2 * g_enc.cap : 1024;
        g_enc.names = (unsigned*)realloc(g_enc.names, g_enc.cap * sizeof *g_enc.names);
        g_enc.values = (Ev*)realloc(g_enc.values, g_enc.cap * sizeof *g_enc.values);
        if (!g_enc.names || !g_enc.values) sg_fail(0, "out of memory");
    }
    g_enc.names[g_enc.bound] = name;
    g_enc.values[g_enc.bound++] = v;
}
/* the names a pattern binds (a let's, a parameter's), v the whole's value */
static void ev_bind(unsigned p, Ev v) {
    unsigned q;
    if (!p) return;
    if (N(p).kind == N_TYPED)
        ev_bind(N(p).a, v);
    else if (N(p).kind == N_ID && !ev_member(N(p).name))
        ev_push(N(p).name, v);
    else if (N(p).kind == N_TUPLE)
        for (q = N(p).a; q; q = N(q).next) ev_bind(q, ev_unknown(v.fpcr));
    else if (N(p).kind == N_BINOP && !strcmp(name_of(p), "as")) {
        ev_bind(N(p).a, ev_unknown(v.fpcr));
        ev_bind(N(p).b, v);
    }
}
/* every name assigned under n, made unknown where it is bound (before a loop's body: what one turn leaves the next
   reads) */
static void ev_forget(unsigned n) {
    for (; n; n = N(n).next) {
        if (N(n).kind == N_ASSIGN) {
            unsigned t = N(n).a;
            Ev* b;
            while (t && (N(t).kind == N_INDEX || N(t).kind == N_SLICE || N(t).kind == N_FIELD || N(t).kind == N_TYPED))
                t = N(t).a;
            if (t && N(t).kind == N_ID && (b = ev_lookup(N(t).name))) *b = ev_unknown(b->fpcr);
        }
        ev_forget(N(n).a), ev_forget(N(n).b), ev_forget(N(n).c), ev_forget(N(n).d);
    }
}
static int ev_bool(Ev v, int* out) {
    if (!v.known || v.kind != EV_BOOL) return 0;
    *out = (int)v.v;
    return 1;
}
static int ev_int(Ev v, long long* out) {
    if (!v.known || v.kind != EV_INT) return 0;
    *out = v.i;
    return 1;
}
static SgU64 ev_ones(unsigned width) { return width >= 64 ? ~0ull : (1ull << width) - 1; }

static Ev ev(unsigned n);
static Ev ev_call_def(unsigned f, const Ev* args, unsigned count);

/* the lowered functions that compute on integers and bytes alone, no FPCR read and no FPSR written: the saturations
   (whose callers set QC, which the walk sees), SM4's table, AES's, the unsigned estimates of URECPE and URSQRTE, and
   nx.sail's guest memory, exclusive monitor and atomics (Guest...) */
static int enc_fp_integer(const char* name) {
    if (!strncmp(name, "Guest", 5)) return 1;
    static const char* const names[] = {"SatQ",
                                        "SignedSatQ",
                                        "UnsignedSatQ",
                                        "Sbox",
                                        "AESSubBytes",
                                        "AESInvSubBytes",
                                        "AESMixColumns",
                                        "AESInvMixColumns",
                                        "UnsignedRecipEstimate",
                                        "UnsignedRSqrtEstimate"};
    for (size_t i = 0; i < sizeof names / sizeof names[0]; ++i)
        if (!strcmp(names[i], name)) return 1;
    return 0;
}

/* a case's pattern against the scrutinee: 1 it matches, 0 it doesn't, -1 not known */
static int ev_pattern(unsigned p, Ev s) {
    if (N(p).kind == N_WILD) return 1;
    if (N(p).kind == N_TYPED) return ev_pattern(N(p).a, s);
    if (N(p).kind == N_NUM)
        return s.known && s.kind == EV_INT && N(p).value <= LLONG_MAX ? s.i == (long long)N(p).value : -1;
    if (N(p).kind == N_BITS || N(p).kind == N_BOOL)
        return s.known && (s.kind == EV_BITS || s.kind == EV_BOOL) ? s.v == N(p).value : -1;
    if (N(p).kind == N_VEC) {
        unsigned e, count = 0, i = 0;
        SgU64 mask = 0, value = 0;
        for (e = N(p).a; e; e = N(e).next) ++count;
        for (e = N(p).a; e; e = N(e).next, ++i) {
            SgU64 bit = count - 1 - i < 64 ? 1ull << (count - 1 - i) : 0;
            if (N(e).kind == N_WILD) continue;
            if (N(e).kind != N_ID || (strcmp(name_of(e), "bitone") && strcmp(name_of(e), "bitzero"))) return -1;
            mask |= bit;
            if (!strcmp(name_of(e), "bitone")) value |= bit;
        }
        if (!mask) return 1;
        return s.known && s.kind == EV_BITS ? (s.v & mask) == value : -1;
    }
    if (N(p).kind == N_ID && ev_member(N(p).name))
        return s.known && s.kind == EV_ENUM ? s.v == ev_member(N(p).name) : -1;
    return -1;
}

static Ev ev_match(unsigned n) {
    Ev s = ev(N(n).a), acc = ev_unknown(0);
    unsigned c;
    int any = 0, maybe = 0;
    ev_use(s);
    for (c = N(n).b; c; c = N(c).next) {
        int m = ev_pattern(N(c).a, s), g = 1;
        unsigned mark = g_enc.bound;
        Ev v;
        if (!m) continue;
        if (m < 0) ev_bind(N(c).a, ev_unknown(s.fpcr));
        if (N(c).c) {
            Ev gv = ev(N(c).c);
            ev_use(gv);
            if (!ev_bool(gv, &g))
                m = -1;
            else if (!g) {
                g_enc.bound = mark;
                continue;
            }
        }
        if (m > 0 && !maybe) {
            v = ev(N(c).b);
            g_enc.bound = mark;
            return any ? ev_merge(acc, v) : v;
        }
        ++g_enc.uncertain;
        v = ev(N(c).b);
        --g_enc.uncertain;
        g_enc.bound = mark;
        acc = any ? ev_merge(acc, v) : v;
        any = 1;
        if (m < 0)
            maybe = 1;
        else
            break; /* matched for certain, after cases that may have */
    }
    return any ? ev_merge(acc, ev_unknown(0)) : acc;
}

static Ev ev_binop(unsigned n) {
    const char* op = name_of(n);
    Ev a = ev(N(n).a), b;
    int x, y;
    long long i, j;
    ev_use(a);
    if ((!strcmp(op, "&") || !strcmp(op, "|")) && ev_bool(a, &x) && x == (op[0] == '|'))
        return a; /* false & ..., true | ...: the rest is never made */
    b = ev(N(n).b);
    ev_use(b);
    if (!strcmp(op, "&") || !strcmp(op, "|")) {
        if (ev_bool(b, &y) && y == (op[0] == '|')) return b;
        if (ev_bool(a, &x) && ev_bool(b, &y)) return ev_known(EV_BOOL, (SgU64)(op[0] == '|' ? x | y : x & y), 0);
        if (a.known && b.known && a.kind == EV_BITS && b.kind == EV_BITS && a.width == b.width)
            return ev_known(EV_BITS, op[0] == '|' ? a.v | b.v : a.v & b.v, a.width);
        return ev_unknown(0);
    }
    if (!strcmp(op, "==") || !strcmp(op, "!=")) {
        if (!a.known || !b.known || (a.kind == EV_ENUM) != (b.kind == EV_ENUM)) return ev_unknown(0);
        return ev_known(EV_BOOL, ev_equal(a, b) == (op[0] == '='), 0);
    }
    if (ev_int(a, &i) && ev_int(b, &j)) {
        long long r;
        if (!strcmp(op, "<")) return ev_known(EV_BOOL, i < j, 0);
        if (!strcmp(op, ">")) return ev_known(EV_BOOL, i > j, 0);
        if (!strcmp(op, "<=")) return ev_known(EV_BOOL, i <= j, 0);
        if (!strcmp(op, ">=")) return ev_known(EV_BOOL, i >= j, 0);
        /* Sail's integers have no bound: a result past long long's is not known */
        if (!strcmp(op, "+")) return ckd_add(&r, i, j) ? ev_unknown(0) : ev_integer(r);
        if (!strcmp(op, "-")) return ckd_sub(&r, i, j) ? ev_unknown(0) : ev_integer(r);
        if (!strcmp(op, "*")) return ckd_mul(&r, i, j) ? ev_unknown(0) : ev_integer(r);
        if (!strcmp(op, "<<") && j >= 0 && j < 63 && i >= 0 && i <= LLONG_MAX >> j) return ev_integer(i << j);
    }
    if (!strcmp(op, "@") && a.known && b.known && a.kind == EV_BITS && b.kind == EV_BITS && a.width + b.width <= 64)
        return ev_known(EV_BITS, a.v << b.width | b.v, a.width + b.width);
    return ev_unknown(0);
}

/* a call of a function the input doesn't define, given FPCR's value where fpcr: one with a val the back end lowers,
   else the generator's own (ZeroExtend, set_R_bits), which computes on what it is given */
static void ev_bodiless(const char* name, int fpcr) {
    if (sg_find(name, D_VAL)) {
        if (fpcr || !enc_fp_integer(name)) g_enc.reach |= ENC_FP_LOWERED;
    } else if (fpcr)
        g_enc.reach |= ENC_FP_DIRECT;
}

static Ev ev_call(unsigned n) {
    const char* name = name_of(n);
    Ev args[24], r = ev_unknown(0);
    unsigned count = 0, a, f, o, m;
    int fpcr = 0;
    if (!strcmp(name, "FPCR_read")) return ev_unknown(1);
    if (!strcmp(name, "IsFeatureImplemented")) {
        char word[72];
        size_t k;
        a = N(n).a;
        if (!a || N(a).kind != N_ID || strncmp(name_of(a), "FEAT_", 5) || strlen(name_of(a)) > 64 || !g_enc.features)
            return ev_unknown(0);
        snprintf(word, sizeof word, " %s ", name_of(a));
        for (k = 0; word[k]; ++k) word[k] = (char)(word[k] >= 'a' && word[k] <= 'z' ? word[k] - 32 : word[k]);
        return strstr(g_enc.features, word) ? ev_unknown(0) : ev_known(EV_BOOL, 0, 0);
    }
    if (!strcmp(name, "Slice") && (a = N(n).a) && N(a).kind == N_ID && !strcmp(name_of(a), "__opcode")) {
        unsigned lo = N(a).next, width = lo ? N(lo).next : 0;
        if (width && N(lo).kind == N_NUM && N(width).kind == N_NUM && N(width).value < 64) {
            SgU64 field = ev_ones((unsigned)N(width).value) << N(lo).value;
            if ((g_enc.mask & field) == field)
                return ev_known(EV_BITS, (g_enc.value & field) >> N(lo).value, (unsigned)N(width).value);
        }
        return ev_unknown(0);
    }
    for (a = N(n).a; a; a = N(a).next) {
        if (count == sizeof args / sizeof args[0]) sg_fail(n, "a call of over 24 arguments");
        args[count] = ev(a);
        fpcr |= args[count++].fpcr;
    }
    if (!strcmp(name, "not_bool") && count == 1) {
        int x;
        ev_use(args[0]);
        return ev_bool(args[0], &x) ? ev_known(EV_BOOL, !x, 0) : ev_unknown(0);
    }
    if ((!strcmp(name, "UInt") || !strcmp(name, "SInt")) && count == 1) {
        ev_use(args[0]);
        if (!args[0].known || args[0].kind != EV_BITS || !args[0].width || args[0].width > 64) return ev_unknown(0);
        {
            unsigned w = args[0].width;
            int top = args[0].v >> (w - 1) & 1;
            long long low = (long long)(args[0].v & ev_ones(w - 1)); /* below 2^63 */
            if (!top) return ev_integer(low);
            if (name[0] == 'S') return ev_integer(low + (w == 64 ? LLONG_MIN : -(1ll << (w - 1))));
            return w == 64 ? ev_unknown(0) : ev_integer(low + (1ll << (w - 1)));
        }
    }
    if ((f = sg_find(name, D_FUNCTION))) return ev_call_def(f, args, count);
    if ((o = sg_find_next(name, D_OVERLOAD, 0))) {
        int any = 0;
        for (; o; o = sg_find_next(name, D_OVERLOAD, o))
            for (m = g_parse.defs[o].params; m; m = N(m).next)
                if ((f = sg_find(name_of(m), D_FUNCTION))) {
                    Ev v = ev_call_def(f, args, count);
                    r = any ? ev_merge(r, v) : v;
                    any = 1;
                } else
                    ev_bodiless(name_of(m), fpcr);
        return any ? ev_merge(r, ev_unknown(0)) : ev_unknown(0);
    }
    ev_bodiless(name, fpcr);
    return ev_unknown(0);
}

/* f walked with args: what it gives, its reaches added to the caller's. Each function once for the same arguments; a
   call back into one under way gives nothing new (that walk goes on), one past 64 deep is taken as reaching */
static Ev ev_call_def(unsigned f, const Ev* args, unsigned count) {
    unsigned k, p, i, saved_base = g_enc.base, saved_bound = g_enc.bound, saved_uncertain = g_enc.uncertain;
    unsigned char saved_reach = g_enc.reach;
    Ev ret, *saved_ret = g_enc.ret;
    int saved_has = g_enc.has_ret, fpcr = 0;
    for (i = 0; i < count; ++i) fpcr |= args[i].fpcr;
    for (k = g_enc.heads[f]; k; k = g_enc.calls[k - 1].next) {
        const EncCall* c = &g_enc.calls[k - 1];
        if (c->count != count) continue;
        for (i = 0; i < count && ev_same(c->args[i], args[i]); ++i) {}
        if (i < count) continue;
        if (!c->done) return ev_unknown(fpcr);
        g_enc.reach |= c->reach;
        return c->ret;
    }
    if (g_enc.depth >= 64) {
        g_enc.reach |= ENC_FP_DIRECT;
        return ev_unknown(fpcr);
    }
    if (g_enc.call_count == g_enc.call_cap) {
        g_enc.call_cap = g_enc.call_cap ? 2 * g_enc.call_cap : 1024;
        g_enc.calls = (EncCall*)realloc(g_enc.calls, g_enc.call_cap * sizeof *g_enc.calls);
        if (!g_enc.calls) sg_fail(0, "out of memory");
    }
    k = g_enc.call_count++;
    g_enc.calls[k] = (EncCall){0};
    g_enc.calls[k].def = f, g_enc.calls[k].count = count, g_enc.calls[k].next = g_enc.heads[f];
    memcpy(g_enc.calls[k].args, args, count * sizeof *args);
    g_enc.heads[f] = k + 1;
    g_enc.base = g_enc.bound;
    for (p = g_parse.defs[f].params, i = 0; p; p = N(p).next, ++i) ev_bind(p, i < count ? args[i] : ev_unknown(0));
    ret = ev_unknown(0);
    g_enc.ret = &ret, g_enc.has_ret = 0, g_enc.reach = 0, g_enc.uncertain = 0, ++g_enc.depth;
    {
        Ev v = ev(g_parse.defs[f].body);
        ret = g_enc.has_ret ? ev_merge(ret, v) : v;
    }
    --g_enc.depth;
    g_enc.calls[k].ret = ret, g_enc.calls[k].reach = g_enc.reach, g_enc.calls[k].done = 1;
    g_enc.reach |= saved_reach;
    g_enc.base = saved_base, g_enc.bound = saved_bound, g_enc.uncertain = saved_uncertain;
    g_enc.ret = saved_ret, g_enc.has_ret = saved_has;
    return ret;
}

static void ev_assign(unsigned t, Ev v) {
    unsigned base = t;
    Ev* b;
    if (!t) return;
    if (N(t).kind == N_TYPED) {
        ev_assign(N(t).a, v);
        return;
    }
    if (N(t).kind == N_TUPLE) {
        for (base = N(t).a; base; base = N(base).next) ev_assign(base, ev_unknown(v.fpcr));
        return;
    }
    while (N(base).kind == N_INDEX || N(base).kind == N_SLICE || N(base).kind == N_FIELD) {
        if (N(base).kind != N_FIELD) ev_use(ev(N(base).b)), ev_use(ev(N(base).c));
        base = N(base).a;
    }
    if (N(base).kind != N_ID) {
        (void)ev(base);
        return;
    }
    if (!strcmp(name_of(base), "FPSR") || !strcmp(name_of(base), "FPCR")) g_enc.reach |= ENC_FP_DIRECT;
    if (!(b = ev_lookup(N(base).name))) return; /* the state's: the walk keeps none */
    if (base == t && !g_enc.uncertain)
        *b = v;
    else
        *b = ev_unknown(b->fpcr | v.fpcr);
}

static Ev ev(unsigned n) {
    Ev a, b, c;
    unsigned item, mark;
    int x;
    long long i, j;
    if (!n) return ev_unknown(0);
    switch (N(n).kind) {
        case N_ID: {
            const char* s = name_of(n);
            Ev* v;
            if (!strcmp(s, "FPCR") || !strcmp(s, "FPSR")) {
                g_enc.reach |= ENC_FP_DIRECT;
                return ev_unknown(0);
            }
            if ((v = ev_lookup(N(n).name))) return *v;
            if ((item = ev_member(N(n).name))) return ev_known(EV_ENUM, item, 0);
            if (!strcmp(s, "bitone") || !strcmp(s, "bitzero")) return ev_known(EV_BITS, s[3] == 'o', 1);
            return ev_unknown(0);
        }
        case N_NUM: return N(n).value <= LLONG_MAX ? ev_integer((long long)N(n).value) : ev_unknown(0);
        case N_BITS: return ev_known(EV_BITS, N(n).value, N(n).width);
        case N_BOOL: return ev_known(EV_BOOL, N(n).value != 0, 0);
        case N_CALL: return ev_call(n);
        case N_BINOP: return ev_binop(n);
        case N_NOT:
            a = ev(N(n).a);
            ev_use(a);
            return ev_bool(a, &x) ? ev_known(EV_BOOL, !x, 0) : ev_unknown(0);
        case N_NEG:
            a = ev(N(n).a);
            ev_use(a);
            return ev_int(a, &i) && i != LLONG_MIN ? ev_integer(-i) : ev_unknown(0);
        case N_SLICE:
        case N_INDEX:
            a = ev(N(n).a), b = ev(N(n).b), c = N(n).kind == N_SLICE ? ev(N(n).c) : b;
            ev_use(a), ev_use(b), ev_use(c);
            if (a.known && a.kind == EV_BITS && ev_int(b, &i) && ev_int(c, &j) && j >= 0 && i >= j && i < 64)
                return ev_known(EV_BITS, a.v >> j & ev_ones((unsigned)(i - j + 1)), (unsigned)(i - j + 1));
            return ev_unknown(0);
        case N_VEC: {
            Ev r = ev_known(EV_BITS, 0, 0);
            for (item = N(n).a; item; item = N(item).next) {
                Ev e = ev(item);
                r.fpcr |= e.fpcr;
                if (r.known && e.known && e.kind == EV_BITS && r.width + e.width <= 64)
                    r.v = r.v << e.width | e.v, r.width += e.width;
                else
                    r.known = 0;
            }
            return r;
        }
        case N_IF:
            a = ev(N(n).a);
            ev_use(a);
            if (ev_bool(a, &x)) return x ? ev(N(n).b) : ev(N(n).c);
            ++g_enc.uncertain;
            b = ev(N(n).b), c = ev(N(n).c);
            --g_enc.uncertain;
            return ev_merge(b, c);
        case N_FIELD:
            a = ev(N(n).a);
            ev_use(a);
            return ev_unknown(0);
        case N_TUPLE: {
            int fpcr = 0;
            for (item = N(n).a; item; item = N(item).next) fpcr |= ev(item).fpcr;
            return ev_unknown(fpcr);
        }
        case N_TYPED: return ev(N(n).a);
        case N_BLOCK:
            mark = g_enc.bound, a = ev_unknown(0);
            for (item = N(n).a; item; item = N(item).next) a = ev(item);
            g_enc.bound = mark;
            return a;
        case N_LET:
            a = ev(N(n).b);
            if (!N(n).c) { /* a block's: its names stay for the items after it */
                ev_bind(N(n).a, a);
                return ev_unknown(0);
            }
            mark = g_enc.bound;
            ev_bind(N(n).a, a);
            b = ev(N(n).c);
            g_enc.bound = mark;
            return b;
        case N_VAR: ev_push(N(n).name, ev(N(n).b)); return ev_unknown(0);
        case N_ASSIGN: ev_assign(N(n).a, ev(N(n).b)); return ev_unknown(0);
        case N_MATCH: return ev_match(n);
        case N_FOREACH:
        case N_WHILE: {
            unsigned body = N(n).kind == N_FOREACH ? N(n).d : N(n).b;
            mark = g_enc.bound;
            if (N(n).kind == N_FOREACH) {
                ev_use(ev(N(n).a)), ev_use(ev(N(n).b)), ev_use(ev(N(n).c));
                ev_push(N(n).name, ev_unknown(0));
            }
            ++g_enc.uncertain;
            for (x = 0; x < 2; ++x) {
                ev_forget(body);
                if (N(n).kind == N_WHILE) ev_use(ev(N(n).a));
                (void)ev(body);
            }
            --g_enc.uncertain;
            g_enc.bound = mark;
            return ev_unknown(0);
        }
        case N_RETURN:
            a = ev(N(n).a);
            *g_enc.ret = g_enc.has_ret ? ev_merge(*g_enc.ret, a) : a;
            g_enc.has_ret = 1;
            return a;
        case N_THROW: (void)ev(N(n).a); return ev_unknown(0);
        case N_UPDATE:
            a = ev(N(n).a);
            ev_use(ev(N(n).b)), ev_use(ev(N(n).c));
            b = ev(N(n).d);
            return ev_unknown(a.fpcr | b.fpcr);
        default: return ev_unknown(0);
    }
}

/* the clause's fp: ENC_FP_DIRECT where its code reaches FPCR or FPSR, else ENC_FP_LOWERED where a lowered function
   is given FPCR's value, else ENC_FP_NONE. ENC_FP_DIRECT where the input doesn't define its decode function (a
   branch's or a load's the generator makes itself): its code unknown */
static int enc_fp(const Enc* e) {
    if (!sg_find(sg_str(e->decode), D_FUNCTION)) return ENC_FP_DIRECT;
    g_enc.mask = e->mask, g_enc.value = e->value, g_enc.reach = 0, g_enc.uncertain = 0;
    g_enc.bound = g_enc.base = 0, g_enc.depth = 0;
    {
        Ev ret = ev_unknown(0);
        g_enc.ret = &ret, g_enc.has_ret = 0;
        (void)ev(g_parse.defs[e->def].body);
    }
    /* what a function gave depends on the clause's fields: nothing of one clause's walk is another's */
    while (g_enc.call_count) g_enc.heads[g_enc.calls[--g_enc.call_count].def] = 0;
    return g_enc.reach & ENC_FP_DIRECT ? ENC_FP_DIRECT : g_enc.reach & ENC_FP_LOWERED ? ENC_FP_LOWERED : ENC_FP_NONE;
}

static int enc_member_order(const void* a, const void* b) {
    return strcmp(sg_str(*(const unsigned*)a), sg_str(*(const unsigned*)b));
}
/* the walk's tables: every enum member's name, and room for the functions walked */
static void enc_fp_init(const char* profile_path) {
    unsigned d, m, count = 0;
    for (d = 1; d < g_parse.def_count; ++d)
        if (g_parse.defs[d].kind == D_ENUM)
            for (m = g_parse.defs[d].params; m; m = N(m).next) ++count;
    g_enc.members = (unsigned*)malloc((count + 1) * sizeof *g_enc.members);
    g_enc.heads = (unsigned*)calloc(g_parse.def_count + 1, sizeof *g_enc.heads);
    if (!g_enc.members || !g_enc.heads) sg_fail(0, "out of memory");
    for (d = 1; d < g_parse.def_count; ++d)
        if (g_parse.defs[d].kind == D_ENUM)
            for (m = g_parse.defs[d].params; m; m = N(m).next) g_enc.members[g_enc.member_count++] = N(m).name;
    qsort(g_enc.members, g_enc.member_count, sizeof *g_enc.members, enc_member_order);
    g_enc.features = profile_path ? sg_profile_features(profile_path) : NULL;
}
static void enc_fp_free(void) {
    free(g_enc.names), free(g_enc.values), free(g_enc.members), free(g_enc.calls), free(g_enc.heads);
    free(g_enc.features);
    g_enc = (typeof(g_enc)){0};
}

static int enc_in_roots(unsigned name, const unsigned* roots, unsigned root_count) {
    unsigned i;
    if (!roots) return 1;
    for (i = 0; i < root_count; ++i)
        if (!strcmp(sg_str(roots[i]), sg_str(name))) return 1;
    return 0;
}

/* a field the pattern fixes whole is no parameter */
static int enc_field_free(const Enc* e, const Field* f) {
    SgU64 m = (f->width >= 64 ? ~0ull : (1ull << f->width) - 1) << f->lo;
    return (e->mask & m) != m;
}

int sg_encoders(const char* prefix, const char* roots_path, const char* const* inputs, const int* overlays,
                int input_count, const char* profile_path, const char* out_path) {
    unsigned d, root_count = 0, root_cap = 0, *roots = NULL, count = 0, cap = 0, k, j;
    size_t plen = strlen(prefix);
    Enc* encs = NULL;
    char tag[32];
    int i;
    /* the encoders' prefix: sge_ and the architecture's tag, __DecodeA64's a64 */
    if (strncmp(prefix, "__Decode", 8) || !prefix[8] || strlen(prefix + 8) >= sizeof tag)
        sg_fail(0, "--encoders takes a decode function's name, __Decode and the architecture (__DecodeA64)");
    for (k = 0; prefix[8 + k]; ++k) tag[k] = (char)(prefix[8 + k] >= 'A' && prefix[8 + k] <= 'Z' ? prefix[8 + k] + 32 : prefix[8 + k]);
    tag[k] = 0;
    for (i = 0; i < input_count; ++i) sg_parse_file(inputs[i], overlays[i]);
    enc_fp_init(profile_path);
    if (roots_path) {
        size_t size;
        char *text = sg_read(roots_path, &size), *line;
        for (line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
            char word[32], name[512];
            if (sscanf(line, "%31s %511s", word, name) != 2 || strcmp(word, "root")) continue;
            if (root_count == root_cap) {
                root_cap = root_cap ? 2 * root_cap : 256;
                roots = realloc(roots, root_cap * sizeof *roots);
                if (!roots) sg_fail(0, "out of memory");
            }
            roots[root_count++] = sg_intern(name, strlen(name));
        }
        free(text);
    }
    for (d = 1; d < g_parse.def_count; ++d) {
        const SgDef* def = &g_parse.defs[d];
        Enc e;
        const char *dn, *cut;
        if (def->kind != D_CLAUSE || strncmp(sg_str(def->name), prefix, plen)) continue;
        memset(&e, 0, sizeof e);
        e.def = d;
        enc_bits(d, &e);
        enc_fields(d, &e);
        if (!enc_in_roots(e.decode, roots, root_count)) continue;
        e.fp = enc_fp(&e);
        dn = sg_str(e.decode) + 7;
        cut = strstr(dn, "_aarch64_instrs");
        if (!cut) cut = strstr(dn, "_aarch32_instrs");
        snprintf(e.short_name, sizeof e.short_name, "%.*s", cut ? (int)(cut - dn) : (int)strlen(dn), dn);
        if (count == cap) {
            cap = cap ? 2 * cap : 1024;
            encs = realloc(encs, cap * sizeof *encs);
            if (!encs) sg_fail(0, "out of memory");
        }
        encs[count++] = e;
    }
    /* names two clauses share: each with the shortest tail of its decode function's name (its words split at _)
       that is its own in the group (LDR immediate's post_idx, pre_idx and unsigned_offset) */
    for (k = 0; k < count; ++k) {
        char base[160];
        unsigned group[64], g = 0, m, words;
        if (encs[k].short_name[0] == '!') continue;
        for (j = k; j < count && g < 64; ++j)
            if (!strcmp(encs[j].short_name, encs[k].short_name)) group[g++] = j;
        if (g < 2) continue;
        snprintf(base, sizeof base, "%s", encs[k].short_name);
        for (words = 1; words < 8; ++words) {
            const char* tails[64];
            unsigned a, b, same = 0;
            for (m = 0; m < g; ++m) {
                const char* t = sg_str(encs[group[m]].decode) + strlen(sg_str(encs[group[m]].decode));
                unsigned w = 0;
                while (t > sg_str(encs[group[m]].decode) && w < words)
                    if (*--t == '_') ++w;
                tails[m] = *t == '_' ? t + 1 : t;
            }
            for (a = 0; a < g; ++a)
                for (b = a + 1; b < g; ++b) same |= !strcmp(tails[a], tails[b]);
            if (same) continue;
            for (m = 0; m < g; ++m)
                snprintf(encs[group[m]].short_name, sizeof encs[group[m]].short_name, "%s_%s", base, tails[m]);
            break;
        }
        if (words == 8) sg_fail(0, "the decode functions of %s are named alike", base);
    }
    if (roots)
        for (k = 0; k < root_count; ++k) {
            for (j = 0; j < count && strcmp(sg_str(encs[j].decode), sg_str(roots[k])); ++j) {}
            if (j == count) sg_fail(0, "%s: no %s clause calls the root %s", roots_path, prefix, sg_str(roots[k]));
        }
    sg_out("/* Generated by tools/sailgen (--encoders %s) from", prefix);
    for (k = 0; k < g_parse.file_count; ++k) sg_out(" %s", g_parse.files[k].path);
    sg_out(": do not edit */\n\n");
    for (k = 0; k < count; ++k) {
        const Enc* e = &encs[k];
        int first = 1;
        sg_out("/* %s */\nstatic inline SnxU32 sge_%s_%s(", sg_str(e->decode), tag, e->short_name);
        for (j = 0; j < e->field_count; ++j) {
            if (!enc_field_free(e, &e->fields[j])) continue;
            sg_out("%sSnxU32 %s", first ? "" : ", ", sg_str(e->fields[j].name));
            first = 0;
        }
        if (first) sg_out("void");
        sg_out(") {\n    return 0x%08llXu", e->value);
        for (j = 0; j < e->field_count; ++j) {
            const Field* f = &e->fields[j];
            SgU64 m = ((1ull << f->width) - 1) << f->lo;
            if (!enc_field_free(e, f)) continue;
            sg_out(" | ((%s << %u) & 0x%08llXu)", sg_str(f->name), f->lo, m & ~e->mask);
        }
        sg_out(";\n}\n");
    }
    sg_out("\n/* every encoder's fixed bits and fields, in the clauses' order */\n");
    sg_out("static const struct {\n    const char* name;\n    const char* family;\n    SnxU32 mask, value;\n    unsigned char nfields;\n"
           "    struct {\n        const char* name;\n        unsigned char lo, width;\n    } fields[16];\n"
           "    unsigned char fp; /* what its code reaches of FPCR and FPSR: 0 nothing, 1 FPCR read or FPSR\n"
           "                         written, 2 FPCR given to a function the back end lowers */\n} sge_%s_table[] = {\n",
           tag);
    for (k = 0; k < count; ++k) {
        const Enc* e = &encs[k];
        sg_out("    {\"%s\", \"%s\", 0x%08llXu, 0x%08llXu, %u, {", e->short_name,
               sg_str(g_parse.defs[e->def].name) + plen + (sg_str(g_parse.defs[e->def].name)[plen] == '_'), e->mask,
               e->value, e->field_count);
        for (j = 0; j < e->field_count; ++j)
            sg_out("%s{\"%s\", %u, %u}", j ? ", " : "", sg_str(e->fields[j].name), e->fields[j].lo, e->fields[j].width);
        sg_out("}, %d},\n", e->fp);
    }
    sg_out("};\n#define sge_%s_count %uu\n", tag, count);
    free(encs);
    free(roots);
    enc_fp_free();
    return sg_flush(out_path);
}
