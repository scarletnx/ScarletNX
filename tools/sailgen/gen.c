/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's generator: one C function per decode function the roots name, which emits that instruction's IR through
   the LLVM back end (llvm.h, copied at the top of the output), and the dispatcher that picks one by the decode
   clauses' fixed bits.

   Each decode function is walked with everything it calls inlined, and each value is given the time it is known at:
   CONST (here, when sailgen runs: literals, an overlay's top-level let, what only they make), STATIC (when the
   recompiler runs: the word's fields and all that only they make) or DYN (when the guest runs: what reads a register, a
   flag among them). CONST folds here, so an if on one keeps one branch; STATIC is C in the output; DYN is the back
   end's calls, a register's read and write among them (below, "the state"). A mutable local takes the latest time any
   of its assignments has, found by walking each decode function until no local's time changes. A function whose
   arguments are all STATIC and that reaches no state becomes a C function of its own (DecodeBitMasks, DecodeShift); the
   rest are inlined, their return in tail position. An if on a DYN condition becomes a select of what each branch
   assigns, and its branches may not change state. Every C name is unique, so inlined bodies and Sail's blocks need no
   braces of their own. Anything outside the part of Sail this knows stops it with the file, the line and why */
#include "sailgen.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* LANE: when the guest runs, one value per lane of an element loop that is one vector operation (foreach_loop) */
enum { BT_CONST, BT_STATIC, BT_DYN, BT_LANE };

/* a value of a Sail struct (an access descriptor): known when the recompiler runs, a C struct of its fields (struct_ctype) */
#define K_STRUCT 32

typedef struct {
    unsigned char kind, bt, ntuple;
    long long value;    /* CONST bool, int or enum member */
    unsigned char huge; /* a CONST integer past 62 bits (WFE's timeout, 1 << 64): bound, never computed with */
    char c[512];        /* the C expression of the value */
    unsigned tuple[4]; /* K_TUPLE: the components, in the value pool */
    unsigned node;
    unsigned sdef; /* K_STRUCT: the struct's definition */
} Val;

typedef struct {
    unsigned name;
    Val val;
    int var;      /* a mutable local: its index in vars, else -1 */
    unsigned def; /* an immutable let: its value's node, which the CRC rule reads through */
} Binding;

typedef struct {
    unsigned char kind, bt;
    unsigned sdef;      /* K_STRUCT: the struct's definition */
    unsigned lane_loop; /* BT_LANE: the element loop that made it so */
    /* BT_LANE: set in the loop to an element's value rather than to the vector Elem_set builds, so after the loop it
       is no value of the Sail's (the last element's) */
    unsigned char lane_elem;
} Var;

typedef struct {
    char* text;
    size_t length, cap;
} Buffer;

/* a static function made a C function: its definition, the kinds it was made for, its name and results */
typedef struct {
    unsigned def;
    char key[80];
    char name[96];
    unsigned char ret_count;
    int tuple;
    unsigned char ret_kinds[4];
    unsigned ret_sdef[4]; /* a K_STRUCT result's struct */
} Helper;

typedef struct {
    unsigned clause; /* its definition */
    unsigned decode; /* the decode function it calls (a root) */
    SgU64 mask, value;
    unsigned shadowed; /* the earlier clause that covers it and hands no word on, plus 1 */
    int sees;          /* it hands some words on to the next clause (a throw of Error_See) */
} Clause;

static struct {
    Binding* stack;
    unsigned depth, cap, frame;
    Var* vars;
    unsigned var_count, var_cap, var_next;
    int changed;
    Val* pool;
    unsigned pool_count, pool_cap;
    unsigned temp;
    Buffer types, helpers, body, *out;
    unsigned structs[64];      /* the structs whose C types the types hold (struct_ctype) */
    char struct_names[64][96]; /* and those types' names */
    unsigned struct_count;
    Helper helper[64];
    unsigned helper_count;
    int indent;
    int in_helper;      /* walking a static helper: a throw returns 1, a return sets its outputs */
    int dynamic_branch; /* inside a branch of a DYN if: no state may change */
    char path[400];     /* the run-time condition (an i1's C expression) the code walked is under, "" for none */
    unsigned call_depth;
    int sees; /* the decode function walked has thrown Error_See */
    /* the element loop walked as one vector operation (foreach_loop): its node, its lanes' count (C, known when it
       recompiles), whether something in it can't be (lane_refuse), the loops of this decode function that can't */
    unsigned lane_loop;
    int lane, lane_fail, lane_restart;
    char lane_count[160];
    unsigned refused[64], refused_count;
    /* a builtin's arguments, evaluated once to see whether one is per lane (lane_builtin), for builtin() to reuse */
    unsigned memo_node, memo_count;
    Val memo[8];
    unsigned roots[1024];
    Clause clauses[1024];
    char* features;           /* --profile's features, " FEAT_A FEAT_B ", in capitals; NULL without one */
    char* backend_code;       /* the back end's text (--backend) with its comments and strings blanked */
    const char* backend_name; /* its file's name, for the errors */
} g_gen;

/* ---- output ---- */

static void buf_add(Buffer* b, const char* text, size_t n) {
    if (b->length + n + 1 > b->cap) {
        b->cap = (b->length + n + 1) * 2;
        b->text = (char*)realloc(b->text, b->cap);
        if (!b->text) sg_fail(0, "out of memory for the output");
    }
    memcpy(b->text + b->length, text, n);
    b->length += n;
    b->text[b->length] = 0;
}
static void lane_refuse(const char* why);
static void emit(const char* fmt, ...) {
    char line[4096];
    va_list ap;
    int i, n;
    for (i = 0; i < g_gen.indent; ++i) buf_add(g_gen.out, "    ", 4);
    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof line) sg_fail(0, "a generated line over %zu characters", sizeof line);
    if (g_gen.lane && strstr(line, "sg_lane")) lane_refuse("the lane's number used other than as an element's");
    buf_add(g_gen.out, line, (size_t)n);
}

#define N(i) (g_parse.nodes[i])
static const char* name_of(unsigned node) { return sg_str(N(node).name); }

static void temp(char* out, size_t size) { snprintf(out, size, "t%u", ++g_gen.temp); }

static const char* ctype(int kind, int bt) {
    if (bt == BT_LANE && (kind == K_BOOL || kind == K_INT || kind == K_BITS || kind == K_BIT)) return "SgL";
    if (bt == BT_DYN) {
        switch (kind) {
            case K_BOOL: return "LLVMValueRef";
            case K_INT: return "SgI";
            case K_BITS:
            case K_BIT: return "SgD";
            default: break;
        }
    } else {
        switch (kind) {
            case K_BOOL:
            case K_ENUM: return "int";
            case K_INT: return "long long";
            case K_BITS:
            case K_BIT: return "SgS";
            default: break;
        }
    }
    if (kind == K_STRUCT)
        sg_fail(0, "a struct's value known only at run time: sailgen keeps one known when it recompiles");
    sg_fail(0, "no C type for a value of kind %d at time %d", kind, bt);
    return "";
}

/* the struct a type names: its definition, else 0 (sailgen.h: a type name sailgen doesn't know is K_ENUM) */
static unsigned type_struct(unsigned type) {
    const char* text;
    char name[96];
    size_t k = 0;
    if (!type || N(type).value != K_ENUM) return 0;
    text = sg_str(N(type).name);
    while (text[k] && !isspace((unsigned char)text[k]) && text[k] != '(' && k + 1 < sizeof name) {
        name[k] = text[k];
        ++k;
    }
    name[k] = 0;
    return sg_find(name, D_STRUCT);
}

/* a field's C type: its kind's, a struct's own, or int for a type sailgen does not keep (a read or a write of such a
   field is refused) */
static const char* struct_ctype(unsigned sdef);
static const char* field_ctype(unsigned type) {
    unsigned st = type_struct(type);
    if (st) return struct_ctype(st);
    switch (type ? (int)N(type).value : K_NONE) {
        case K_INT: return "long long";
        case K_BITS:
        case K_BIT: return "SgS";
        default: return "int";
    }
}

/* the C type of a Sail struct's values: sgt_NAME, its fields in order, written to the types once */
static const char* struct_ctype(unsigned sdef) {
    unsigned i, f;
    Buffer* saved = g_gen.out;
    int saved_indent = g_gen.indent;
    for (i = 0; i < g_gen.struct_count; ++i)
        if (g_gen.structs[i] == sdef) return g_gen.struct_names[i];
    if (g_gen.struct_count == sizeof g_gen.structs / sizeof *g_gen.structs) sg_fail(0, "more than 64 structs");
    for (f = g_parse.defs[sdef].params; f; f = N(f).next) (void)field_ctype(N(f).b); /* the structs inside first */
    i = g_gen.struct_count++;
    g_gen.structs[i] = sdef;
    snprintf(g_gen.struct_names[i], sizeof g_gen.struct_names[i], "sgt_%s", sg_str(g_parse.defs[sdef].name));
    g_gen.out = &g_gen.types;
    g_gen.indent = 0;
    emit("typedef struct {\n");
    for (f = g_parse.defs[sdef].params; f; f = N(f).next) emit("    %s %s;\n", field_ctype(N(f).b), name_of(N(f).a));
    emit("} %s;\n\n", g_gen.struct_names[i]);
    g_gen.out = saved;
    g_gen.indent = saved_indent;
    return g_gen.struct_names[i];
}

/* the C type of a value of this kind, time and struct */
static const char* ctype_of(int kind, int bt, unsigned sdef) {
    if (kind == K_STRUCT && bt != BT_DYN) return struct_ctype(sdef);
    return ctype(kind, bt);
}

static Val val_c(int kind, int bt, const char* fmt, ...) {
    Val v = {0};
    va_list ap;
    int n;
    v.kind = (unsigned char)(kind == K_BIT ? K_BITS : kind);
    v.bt = (unsigned char)bt;
    va_start(ap, fmt);
    n = vsnprintf(v.c, sizeof v.c, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof v.c) sg_fail(0, "a C expression over %zu characters", sizeof v.c);
    return v;
}
static Val val_const(int kind, long long value) {
    Val v = val_c(kind, BT_CONST, kind == K_INT ? "%lldll" : "%lld", value);
    v.value = value;
    return v;
}
static Val unit(void) { return val_c(K_UNIT, BT_CONST, "0"); }

/* the value in a new temporary of its own C type, so a later assignment to what it came from leaves it */
static Val keep(Val v) {
    char t[16];
    if (v.kind == K_UNIT || v.kind == K_TUPLE || v.bt == BT_CONST) return v;
    temp(t, sizeof t);
    emit("%s %s = %s;\n", ctype_of(v.kind, v.bt, v.sdef), t, v.c);
    snprintf(v.c, sizeof v.c, "%s", t);
    return v;
}

/* the value at time bt (BT_DYN from STATIC or CONST) */
static Val raise(Val v, int bt) {
    if (v.bt == BT_LANE && bt != BT_LANE) { /* a lane's value where one value is wanted */
        if (g_gen.lane) {
            lane_refuse("a lane's value used where one value is wanted");
            return v;
        }
        if (v.kind != K_BITS) sg_fail(v.node, "a lane's value of kind %d used after its loop", v.kind);
        return val_c(K_BITS, BT_DYN, "sg_d_from_l(g, %s)", v.c);
    }
    if (bt == BT_LANE && v.bt != BT_LANE) { /* the same value in every lane */
        if (!g_gen.lane) sg_fail(v.node, "a value spread over lanes outside an element loop");
        if (v.kind != K_BITS && v.kind != K_INT && v.kind != K_BOOL) {
            lane_refuse("a value of a kind lanes don't hold");
            return v;
        }
        v = raise(v, BT_DYN);
        return val_c(v.kind, BT_LANE, "sg_l_splat_%s(g, %s, %s)",
                     v.kind == K_BITS  ? "d"
                     : v.kind == K_INT ? "i"
                                       : "b",
                     v.c, g_gen.lane_count);
    }
    if (bt <= BT_STATIC || v.bt == BT_DYN || v.bt == BT_LANE) {
        if (v.bt == BT_CONST && bt == BT_STATIC && v.kind != K_TUPLE) v.bt = BT_STATIC;
        return v;
    }
    switch (v.kind) {
        case K_BITS: return val_c(K_BITS, BT_DYN, "sg_d_from_s(%s)", v.c);
        case K_INT: return val_c(K_INT, BT_DYN, "sg_i_const(g, %s)", v.c);
        case K_BOOL: return val_c(K_BOOL, BT_DYN, "LLVMConstInt(g->ir->t_i1, (unsigned long long)(%s), 0)", v.c);
        default: sg_fail(v.node, "a value of kind %d can't be known only at run time", v.kind);
    }
    return v;
}

static unsigned pool_add(Val v) {
    if (g_gen.pool_count >= g_gen.pool_cap) {
        g_gen.pool_cap = g_gen.pool_cap ? g_gen.pool_cap * 2 : 256;
        g_gen.pool = (Val*)realloc(g_gen.pool, sizeof(Val) * g_gen.pool_cap);
        if (!g_gen.pool) sg_fail(0, "out of memory for the values");
    }
    g_gen.pool[g_gen.pool_count] = v;
    return g_gen.pool_count++;
}
static Val tuple_of(const Val* parts, unsigned count, unsigned node) {
    Val t = {0};
    unsigned i;
    t.kind = K_TUPLE;
    t.bt = BT_STATIC;
    t.ntuple = (unsigned char)count;
    t.node = node;
    for (i = 0; i < count; ++i) t.tuple[i] = pool_add(parts[i]);
    return t;
}

/* ---- bindings ---- */

static void bind(unsigned name, Val v, int var, unsigned def) {
    if (g_gen.depth >= g_gen.cap) {
        g_gen.cap = g_gen.cap ? g_gen.cap * 2 : 256;
        g_gen.stack = (Binding*)realloc(g_gen.stack, sizeof(Binding) * g_gen.cap);
        if (!g_gen.stack) sg_fail(0, "out of memory for the bindings");
    }
    g_gen.stack[g_gen.depth].name = name;
    g_gen.stack[g_gen.depth].val = v;
    g_gen.stack[g_gen.depth].var = var;
    g_gen.stack[g_gen.depth].def = def;
    ++g_gen.depth;
}
static Binding* lookup(const char* name) {
    unsigned i;
    for (i = g_gen.depth; i > g_gen.frame; --i)
        if (!strcmp(sg_str(g_gen.stack[i - 1].name), name)) return &g_gen.stack[i - 1];
    return NULL;
}
static Binding* var_binding(int var) {
    unsigned i;
    for (i = g_gen.depth; i > 0; --i)
        if (g_gen.stack[i - 1].var == var) return &g_gen.stack[i - 1];
    return NULL;
}

/* a mutable local: its index, the same from one walk to the next (the walk's order doesn't depend on the times) */
static int new_var(int kind, int bt) {
    unsigned i = g_gen.var_next++;
    if (i >= g_gen.var_cap) {
        g_gen.var_cap = g_gen.var_cap ? g_gen.var_cap * 2 : 256;
        g_gen.vars = (Var*)realloc(g_gen.vars, sizeof(Var) * g_gen.var_cap);
        if (!g_gen.vars) sg_fail(0, "out of memory for the locals");
    }
    if (i >= g_gen.var_count) {
        g_gen.vars[i].kind = (unsigned char)(kind == K_BIT ? K_BITS : kind);
        g_gen.vars[i].bt = (unsigned char)(bt < BT_STATIC ? BT_STATIC : bt);
        g_gen.vars[i].lane_loop = 0;
        g_gen.vars[i].lane_elem = 0;
        g_gen.var_count = i + 1;
        g_gen.changed = 1;
    } else if (bt > g_gen.vars[i].bt) {
        g_gen.vars[i].bt = (unsigned char)bt;
        g_gen.changed = 1;
    }
    return (int)i;
}
static void var_raise(int var, int bt) {
    if (bt > g_gen.vars[var].bt) {
        g_gen.vars[var].bt = (unsigned char)bt;
        g_gen.changed = 1;
    }
    if (bt == BT_LANE && g_gen.lane) g_gen.vars[var].lane_loop = g_gen.lane_loop;
}

/* ---- lanes: an element loop as one vector operation ----
   foreach (e from 0 to elements - 1) whose body reads and sets element e of bits (Elem_read, Elem_set) is walked once,
   e standing for every lane at once: each value it makes per element is a vector of them (BT_LANE, the back end's
   SgL), and the result is that vector. Whatever the walk meets that lanes can't hold refuses the loop, which is then
   walked again as the loop of elements it is (the walk of the decode function starts over) */
static void lane_refuse(const char* why) {
    (void)why;
    if (g_gen.lane) g_gen.lane_fail = 1;
}
static int lane_refused(unsigned loop) {
    unsigned i;
    for (i = 0; i < g_gen.refused_count; ++i)
        if (g_gen.refused[i] == loop) return 1;
    return 0;
}
/* the loop is walked as a loop from now on: the decode function's walk starts over without its lanes */
static void refuse_loop(unsigned loop) {
    if (lane_refused(loop)) return;
    if (g_gen.refused_count == sizeof g_gen.refused / sizeof *g_gen.refused) sg_fail(loop, "over 64 element loops");
    g_gen.refused[g_gen.refused_count++] = loop;
    g_gen.lane_restart = 1;
    g_gen.changed = 1;
}

/* the index of the enum member name, -1 when it is none */
static long long enum_member(const char* name) {
    unsigned i;
    for (i = 1; i < g_parse.def_count; ++i) {
        const SgDef* d = &g_parse.defs[i];
        unsigned m;
        long long k = 0;
        if (d->kind != D_ENUM) continue;
        for (m = d->params; m; m = N(m).next, ++k)
            if (!strcmp(name_of(m), name)) return k;
    }
    return -1;
}

static Val eval(unsigned n, int tail);
static void assign(unsigned target, Val v);
static Val undefined_of(unsigned type, int bt);
static void stmt(unsigned n) { (void)eval(n, 0); }

static void throw_undefined(unsigned n) {
    if (g_gen.dynamic_branch) sg_fail(n, "a throw under a condition known only at run time");
    if (g_gen.in_helper)
        emit("return 1;\n");
    else {
        emit("sg_undefined(g);\n");
        emit("return 1;\n");
    }
}
/* the decode function ends here, its word decoded: UNDEF (above), done (EndOfInstruction: a hint the guest's features
   make a NOP), or handed on to the next clause that matches it (Error_See: sg_see checks that nothing was emitted yet).
   Each is a return from the decode function, so none may be in a helper or under a run-time condition */
static void end_decode(unsigned n, const char* what, int see) {
    if (g_gen.dynamic_branch) sg_fail(n, "%s under a condition known only at run time", what);
    if (g_gen.in_helper) sg_fail(n, "%s in a function of what the recompiler knows", what);
    if (see) {
        emit("return sg_see(g);\n");
        g_gen.sees = 1;
    } else
        emit("return 1;\n");
}

static int top_bt(int a, int b) { return a > b ? a : b; }

/* c without the parentheses around all of it, for an if's condition (-Wparentheses-equality) */
static const char* cond_text(const char* c, char* out, size_t size) {
    size_t n = strlen(c), i;
    int depth = 0;
    if (n < 2 || c[0] != '(' || c[n - 1] != ')') return c;
    for (i = 0; i < n; ++i) {
        if (c[i] == '(') ++depth;
        if (c[i] == ')' && --depth == 0 && i != n - 1) return c; /* (a) == (b): the first ) closes before the end */
    }
    snprintf(out, size, "%.*s", (int)(n - 2), c + 1);
    return out;
}

static Val need_static_int(unsigned n, Val v, const char* what) {
    if (v.kind != K_INT && v.kind != K_ENUM && v.kind != K_BOOL)
        sg_fail(n, "%s must be an integer, found kind %d", what, v.kind);
    if (v.bt == BT_LANE && g_gen.lane) {
        lane_refuse("a lane's value where one known when it recompiles is wanted");
        return val_const(K_INT, 0);
    }
    if (v.huge) sg_fail(n, "%s is an integer over 62 bits", what);
    if (v.bt >= BT_DYN) sg_fail(n, "%s must be known when the recompiler runs", what);
    return v;
}
static Val need_bits(unsigned n, Val v, const char* what) {
    if (v.kind != K_BITS) sg_fail(n, "%s must be bits, found kind %d", what, v.kind);
    return v;
}

/* ---- calls ---- */

static unsigned arg_at(unsigned n, unsigned index) {
    unsigned a = N(n).a;
    while (a && index--) a = N(a).next;
    return a;
}
static unsigned arg_count(unsigned n) {
    unsigned a = N(n).a, k = 0;
    for (; a; a = N(a).next) ++k;
    if (k == 1 && N(N(n).a).kind == N_UNIT) return 0;
    return k;
}
static int is_call(unsigned n, const char* name) { return n && N(n).kind == N_CALL && !strcmp(name_of(n), name); }

/* the expression an immutable let bound n's name to, or n itself */
static unsigned through_let(unsigned n) {
    while (n && N(n).kind == N_ID) {
        Binding* b = lookup(name_of(n));
        if (!b || b->var >= 0 || !b->def) break;
        n = b->def;
    }
    return n;
}

/* does a function reach state (a register, a feature, the pc), or call one that does? A call of an overload may run
   any of its functions */
static int reaches_state(unsigned def, unsigned depth);
/* what the back end's sg_NAME of a function with no Sail definition takes after the context (backend_action) */
enum { BACKEND_ARGS = 1, BACKEND_PATH = 2 };
static int backend_action(const char* name);
static int node_reaches_state(unsigned n, unsigned depth) {
    if (!n) return 0;
    if (N(n).kind == N_ID) {
        const char* s = name_of(n);
        if (!strcmp(s, "_PC")) return 1;
        if (sg_find(s, D_REGISTER) && !sg_find(s, D_LET)) return 1;
    }
    if (N(n).kind == N_CALL) {
        const char* s = name_of(n);
        unsigned d = sg_find(s, D_FUNCTION), o = d ? 0 : sg_find(s, D_OVERLOAD), m;
        if (!strcmp(s, "IsFeatureImplemented") || !strcmp(s, "AddWithCarry") || !strcmp(s, "ConditionHolds") ||
            !strcmp(s, "BranchTo") || !strcmp(s, "BranchNotTaken") || !strcmp(s, "EndOfInstruction"))
            return 1;
        if (d && reaches_state(d, depth + 1)) return 1;
        if (!d && !o && backend_action(s) >= 0) return 1;
        for (m = o ? g_parse.defs[o].params : 0; m; m = N(m).next)
            if ((d = sg_find(name_of(m), D_FUNCTION)) != 0 && reaches_state(d, depth + 1)) return 1;
    }
    return node_reaches_state(N(n).a, depth) || node_reaches_state(N(n).b, depth) ||
           node_reaches_state(N(n).c, depth) || node_reaches_state(N(n).d, depth) ||
           node_reaches_state(N(n).next, depth);
}
static int reaches_state(unsigned def, unsigned depth) {
    if (depth > 32) return 1;
    return node_reaches_state(g_parse.defs[def].body, depth);
}

static void param_names(unsigned def, unsigned* names, unsigned* count) {
    unsigned p = g_parse.defs[def].params, k = 0;
    if (p && N(p).kind == N_TUPLE) p = N(p).a;
    for (; p; p = N(p).next) {
        unsigned q = N(p).kind == N_TYPED ? N(p).a : p;
        if (N(q).kind == N_UNIT) continue;
        if (N(q).kind != N_ID) sg_fail(q, "a parameter that is not a name");
        if (k == 16) sg_fail(q, "more than 16 parameters");
        names[k++] = N(q).name;
    }
    *count = k;
}

/* the function a call of name with count arguments runs: its definition, or the one of that many parameters among
   the overload name's (sail-arm's _R = {get_R, set_R, set_R_bits}: _R(n) is get_R, _R(n) = v set_R); 0 for none */
static unsigned function_def(const char* name, unsigned count) {
    unsigned d = sg_find(name, D_FUNCTION), o, m;
    if (d || !(o = sg_find(name, D_OVERLOAD))) return d;
    for (m = g_parse.defs[o].params; m; m = N(m).next) {
        unsigned names[16], k;
        if (!(d = sg_find(name_of(m), D_FUNCTION))) continue;
        param_names(d, names, &k);
        if (k == count) return d;
    }
    return 0;
}

static void bind_widths(unsigned def, const Val* args, unsigned count);

/* a function whose arguments are all known when the recompiler runs, and that reaches no state, as a C function
   (int: 1 when it throws; its results through pointers) */
static Val helper_call(unsigned n, unsigned def, Val* args, unsigned count) {
    char key[80], text[768];
    unsigned i, names[16], name_count;
    Helper* h = NULL;
    size_t at = 0;
    Val outs[4];
    param_names(def, names, &name_count);
    if (name_count != count)
        sg_fail(n, "%s takes %u arguments, given %u", sg_str(g_parse.defs[def].name), name_count, count);
    snprintf(key, sizeof key, "%s/", sg_str(g_parse.defs[def].name));
    for (i = 0; i < count && strlen(key) + 2 < sizeof key; ++i) {
        size_t l = strlen(key);
        key[l] = (char)('0' + args[i].kind);
        key[l + 1] = 0;
    }
    for (i = 0; i < g_gen.helper_count; ++i)
        if (!strcmp(g_gen.helper[i].key, key)) h = &g_gen.helper[i];
    if (!h) {
        Buffer local = {0, 0, 0}, *saved_out = g_gen.out;
        unsigned saved_frame = g_gen.frame, saved_depth = g_gen.depth, k;
        int saved_indent = g_gen.indent, saved_in = g_gen.in_helper, saved_dyn = g_gen.dynamic_branch;
        Val ret;
        char sig[768];
        size_t s = 0;
        if (g_gen.helper_count == sizeof g_gen.helper / sizeof *g_gen.helper) sg_fail(n, "more than 64 helpers");
        h = &g_gen.helper[g_gen.helper_count++];
        memset(h, 0, sizeof *h);
        h->def = def;
        snprintf(h->key, sizeof h->key, "%s", key);
        snprintf(h->name, sizeof h->name, "sgh_%u_%s", g_gen.helper_count, sg_str(g_parse.defs[def].name));
        g_gen.out = &local;
        g_gen.indent = 1;
        g_gen.in_helper = 1;
        g_gen.dynamic_branch = 0;
        for (k = 0; k < count; ++k) bind(names[k], val_c(args[k].kind, BT_STATIC, "a_%s", sg_str(names[k])), -1, 0);
        g_gen.frame = saved_depth;
        {
            Val bound[16];
            for (k = 0; k < count; ++k) bound[k] = val_c(args[k].kind, BT_STATIC, "a_%s", sg_str(names[k]));
            bind_widths(def, bound, count);
        }
        ret = eval(g_parse.defs[def].body, 1);
        if (ret.kind == K_TUPLE) {
            h->tuple = 1;
            h->ret_count = ret.ntuple;
            for (k = 0; k < ret.ntuple; ++k) {
                Val part = raise(g_gen.pool[ret.tuple[k]], BT_STATIC);
                if (part.bt == BT_DYN)
                    sg_fail(n, "%s's result is known only at run time", sg_str(g_parse.defs[def].name));
                h->ret_kinds[k] = part.kind;
                h->ret_sdef[k] = part.sdef;
                emit("*out%u = %s;\n", k, part.c);
            }
        } else if (ret.kind != K_UNIT) {
            h->ret_count = 1;
            h->ret_kinds[0] = ret.kind;
            h->ret_sdef[0] = ret.sdef;
            emit("*out0 = %s;\n", ret.c);
        }
        emit("return 0;\n");
        g_gen.out = &g_gen.helpers;
        g_gen.indent = 0;
        s += (size_t)snprintf(sig + s, sizeof sig - s, "static int %s(SgCtx* g", h->name);
        for (k = 0; k < count; ++k)
            s += (size_t)snprintf(sig + s, sizeof sig - s, ", %s a_%s", ctype(args[k].kind, BT_STATIC),
                                  sg_str(names[k]));
        for (k = 0; k < h->ret_count; ++k)
            s += (size_t)snprintf(sig + s, sizeof sig - s, ", %s* out%u",
                                  ctype_of(h->ret_kinds[k], BT_STATIC, h->ret_sdef[k]), k);
        emit("%s) {\n    (void)g;\n", sig);
        for (k = 0; k < count; ++k) emit("    (void)a_%s;\n", sg_str(names[k]));
        buf_add(&g_gen.helpers, local.text ? local.text : "", local.length);
        emit("}\n\n");
        free(local.text);
        g_gen.out = saved_out;
        g_gen.frame = saved_frame;
        g_gen.depth = saved_depth;
        g_gen.indent = saved_indent;
        g_gen.in_helper = saved_in;
        g_gen.dynamic_branch = saved_dyn;
    }
    at += (size_t)snprintf(text + at, sizeof text - at, "%s(g", h->name);
    for (i = 0; i < count; ++i)
        at += (size_t)snprintf(text + at, sizeof text - at, ", %s", raise(args[i], BT_STATIC).c);
    for (i = 0; i < h->ret_count; ++i) {
        char t[16];
        temp(t, sizeof t);
        emit("%s %s;\n", ctype_of(h->ret_kinds[i], BT_STATIC, h->ret_sdef[i]), t);
        outs[i] = val_c(h->ret_kinds[i], BT_STATIC, "%s", t);
        outs[i].sdef = h->ret_sdef[i];
        at += (size_t)snprintf(text + at, sizeof text - at, ", &%s", t);
    }
    if (at >= sizeof text) sg_fail(n, "a helper call over %zu characters", sizeof text);
    emit("if (%s)) {\n", text);
    ++g_gen.indent;
    throw_undefined(n);
    --g_gen.indent;
    emit("}\n");
    if (!h->ret_count) return unit();
    if (h->tuple) return tuple_of(outs, h->ret_count, n);
    return outs[0];
}

/* the type variables a function's val gives its bits parameters' widths by and no parameter names (FPAbs : forall
   'N. bits('N) -> bits('N) reads 'N), bound in the frame to those arguments' widths, known when it recompiles */
static void bind_widths(unsigned def, const Val* args, unsigned count) {
    unsigned val = sg_find(sg_str(g_parse.defs[def].name), D_VAL), k = 0;
    const char *text, *s, *e, *arrow = NULL, *open;
    int depth = 0;
    if (!val) return;
    text = g_parse.files[g_parse.defs[val].source].text;
    e = text + g_parse.defs[val].text_end;
    for (s = text + g_parse.defs[val].text_start; s < e; ++s) {
        if (s[0] == '/' && s + 1 < e && s[1] == '/') { /* a comment before the val */
            while (s < e && *s != '\n') ++s;
            continue;
        }
        if (*s == '(' || *s == '{') ++depth;
        if (*s == ')' || *s == '}') --depth;
        if (!depth && *s == '-' && s + 1 < e && s[1] == '>') arrow = s;
    }
    if (!arrow) return;
    for (s = arrow; s > text && isspace((unsigned char)s[-1]); --s) {}
    if (s == text || s[-1] != ')') return; /* one parameter of a type with no width (unit -> ...) */
    for (open = s - 1, depth = 0; open > text; --open) {
        if (*open == ')') ++depth;
        if (*open == '(' && --depth == 0) break;
    }
    if (open > text && (isalnum((unsigned char)open[-1]) || open[-1] == '_')) /* one parameter: bits('N) -> ... */
        while (open > text && (isalnum((unsigned char)open[-1]) || open[-1] == '_')) --open;
    else
        ++open; /* the list's parameters */
    for (s = open, depth = 0; s < arrow && k < count; ++k) {
        const char* t = s;
        while (t < arrow && isspace((unsigned char)*t)) ++t;
        if (!strncmp(t, "bits('", 6) && args[k].kind == K_BITS) {
            const char* name = t + 6;
            size_t length = 0;
            while (isalnum((unsigned char)name[length]) || name[length] == '_') ++length;
            if (name[length] == ')') {
                unsigned id = sg_intern(name, length);
                if (!lookup(sg_str(id))) bind(id, val_c(K_INT, BT_STATIC, "(long long)(%s).w", args[k].c), -1, 0);
            }
        }
        for (; s < arrow; ++s) { /* to the next parameter */
            if (*s == '(' || *s == '{') ++depth;
            if ((*s == ')' || *s == '}') && --depth < 0) break;
            if (*s == ',' && !depth) {
                ++s;
                break;
            }
        }
    }
}

/* an inlined function: its parameters bound to the arguments in a frame of its own, the value of its body */
static Val inline_call(unsigned n, unsigned def, Val* args, unsigned count) {
    unsigned names[16], name_count, k, saved_frame = g_gen.frame, base = g_gen.depth;
    Val r;
    param_names(def, names, &name_count);
    if (name_count != count)
        sg_fail(n, "%s takes %u arguments, given %u", sg_str(g_parse.defs[def].name), name_count, count);
    if (++g_gen.call_depth > 48) sg_fail(n, "calls nested over 48 deep inlining %s", sg_str(g_parse.defs[def].name));
    for (k = 0; k < count; ++k) bind(names[k], args[k], -1, args[k].node);
    g_gen.frame = base;
    bind_widths(def, args, count);
    emit("/* %s */\n", sg_str(g_parse.defs[def].name));
    r = eval(g_parse.defs[def].body, 1);
    if (r.kind != K_UNIT && r.kind != K_TUPLE) r = keep(r);
    g_gen.frame = saved_frame;
    g_gen.depth = base;
    --g_gen.call_depth;
    return r;
}

/* ---- builtins: Sail's library and the shared functions sailgen lowers itself (README.md, "Builtins") ---- */

/* a width known here over the 64 bits a value known when it recompiles keeps (SgS): such bits (a SIMD&FP register's
   Zeros(128)) are a map of constant bits instead, which costs no IR */
static int wide(Val width) { return width.bt == BT_CONST && width.value > 64; }

static Val arg(unsigned n, unsigned i) {
    if (g_gen.memo_node == n && i < g_gen.memo_count) return g_gen.memo[i]; /* lane_builtin evaluated it */
    return eval(arg_at(n, i), 0);
}
static void need_args(unsigned n, unsigned count) {
    if (arg_count(n) != count) sg_fail(n, "%s takes %u arguments, given %u", name_of(n), count, arg_count(n));
}

/* BitReverse(Poly32Mod2(EOR(BitReverse(acc) @ Zeros(size), BitReverse(val) @ Zeros(32)), poly)): the reflected CRC
   update, lowered whole (llvm.h's sg_crc32); 0 when x is not that */
static int crc_rule(unsigned x, Val* r) {
    unsigned mod = through_let(x), eor, left, right, acc, val;
    Val a, v, size, poly;
    if (!is_call(mod, "Poly32Mod2") || arg_count(mod) != 2) return 0;
    eor = through_let(arg_at(mod, 0));
    if (!is_call(eor, "EOR") || arg_count(eor) != 2) return 0;
    left = through_let(arg_at(eor, 0));
    right = through_let(arg_at(eor, 1));
    if (N(left).kind != N_BINOP || strcmp(name_of(left), "@") || N(right).kind != N_BINOP ||
        strcmp(name_of(right), "@"))
        return 0;
    acc = through_let(N(left).a);
    val = through_let(N(right).a);
    if (!is_call(acc, "BitReverse") || !is_call(val, "BitReverse") || !is_call(N(left).b, "Zeros") ||
        !is_call(N(right).b, "Zeros"))
        return 0;
    a = raise(need_bits(acc, eval(arg_at(acc, 0), 0), "a CRC's accumulator"), BT_DYN);
    v = raise(need_bits(val, eval(arg_at(val, 0), 0), "a CRC's value"), BT_DYN);
    size = need_static_int(left, eval(arg_at(N(left).b, 0), 0), "a CRC's size");
    poly = eval(arg_at(mod, 1), 0);
    if (poly.kind != K_BITS || poly.bt == BT_DYN) sg_fail(mod, "a CRC's polynomial must be known when it recompiles");
    *r = val_c(K_BITS, BT_DYN, "sg_crc32(g, %s, %s, %s, %s)", a.c, v.c, size.c, poly.c);
    return 1;
}

static Val shift(unsigned n, int kind) {
    static const char* const s_fn[4] = {"sg_s_lsl", "sg_s_lsr", "", "sg_s_ror"};
    static const char* const d_fn[4] = {"sg_d_lsl", "sg_d_lsr", "sg_d_asr", "sg_d_ror"};
    Val x = need_bits(n, arg(n, 0), "a shift's value"), amount = arg(n, 1);
    need_args(n, 2);
    if (amount.kind != K_INT) sg_fail(n, "a shift amount that is not an integer");
    if (amount.bt == BT_DYN) {
        x = raise(x, BT_DYN);
        return val_c(K_BITS, BT_DYN, "sg_d_shift_dyn(g, %s, (%s).v, %d)", x.c, amount.c, kind);
    }
    if (x.bt != BT_DYN && kind != 2) return val_c(K_BITS, BT_STATIC, "%s(%s, %s)", s_fn[kind], x.c, amount.c);
    x = raise(x, BT_DYN);
    return val_c(K_BITS, BT_DYN, "%s(g, %s, %s)", d_fn[kind], x.c, amount.c);
}

/* the builtins an element loop's lanes can go through: their arguments are evaluated here, and one per lane makes the
   result one per lane; else they stay in g_gen.memo for builtin_body. 1 when *r is the result */
static int lane_builtin(unsigned n, const char* name, Val* r) {
    static const char* const names[] = {"UInt", "SInt",       "asl_Int",    "Min",       "Max",
                                        "Abs",  "negate",     "IsZero",     "not_vec",   "not_bool",
                                        "EOR",  "ZeroExtend", "SignExtend", "Elem_read", "Elem_set"};
    unsigned count = arg_count(n), i, lanes = 0;
    Val* a = g_gen.memo;
    for (i = 0; i < sizeof names / sizeof *names && strcmp(name, names[i]); ++i) {}
    if (i == sizeof names / sizeof *names || count > 8) return 0;
    for (i = 0; i < count; ++i) {
        a[i] = eval(arg_at(n, i), 0);
        lanes += a[i].bt == BT_LANE;
    }
    g_gen.memo_node = n;
    g_gen.memo_count = count;
    if (!strcmp(name, "Elem_read") && count == 3 && strstr(a[1].c, "sg_lane")) { /* element e: the lanes */
        Val size = need_static_int(n, a[2], "an element's size");
        if (strcmp(a[1].c, "sg_lane") || a[0].bt == BT_LANE || a[0].kind != K_BITS) {
            lane_refuse("an element other than the lane's own");
            *r = a[0];
            return 1;
        }
        *r = val_c(K_BITS, BT_LANE, "sg_l_elem(g, %s, %s, %s)", raise(a[0], BT_DYN).c, size.c, g_gen.lane_count);
        return 1;
    }
    if (!strcmp(name, "Elem_set") && count == 4 && strstr(a[1].c, "sg_lane")) { /* element e set: the lanes */
        Val size = need_static_int(n, a[2], "an element's size");
        if (strcmp(a[1].c, "sg_lane") || a[3].kind != K_BITS) {
            lane_refuse("an element set other than the lane's own");
            *r = a[0];
            return 1;
        }
        *r = val_c(K_BITS, BT_LANE, "sg_l_bits(g, %s, %s)", raise(a[3], BT_LANE).c, size.c);
        return 1;
    }
    if (!lanes) return 0;
    if ((!strcmp(name, "UInt") || !strcmp(name, "SInt")) && count == 1 && a[0].kind == K_BITS)
        *r = val_c(K_INT, BT_LANE, "sg_l_int(g, %s, %d)", a[0].c, name[0] == 'S');
    else if (!strcmp(name, "asl_Int") && count == 2 && a[0].kind == K_BITS && a[1].bt <= BT_STATIC)
        *r = val_c(K_INT, BT_LANE, "sg_l_int(g, %s, !(%s))", a[0].c, a[1].c);
    else if ((!strcmp(name, "Min") || !strcmp(name, "Max")) && count == 2 && a[0].kind == K_INT && a[1].kind == K_INT)
        *r = val_c(K_INT, BT_LANE, "sg_l_minmax(g, %s, %s, %d)", raise(a[0], BT_LANE).c, raise(a[1], BT_LANE).c,
                   name[1] == 'i');
    else if ((!strcmp(name, "Abs") || !strcmp(name, "negate")) && count == 1 && a[0].kind == K_INT)
        *r = val_c(K_INT, BT_LANE, "sg_l_%s(g, %s)", name[0] == 'A' ? "abs" : "neg", a[0].c);
    else if (!strcmp(name, "IsZero") && count == 1 && a[0].kind == K_BITS)
        *r = val_c(K_BOOL, BT_LANE, "sg_l_is_zero(g, %s)", a[0].c);
    else if ((!strcmp(name, "not_vec") && count == 1 && a[0].kind == K_BITS) ||
             (!strcmp(name, "not_bool") && count == 1 && a[0].kind == K_BOOL))
        *r = val_c(a[0].kind, BT_LANE, "sg_l_not(g, %s)", a[0].c);
    else if (!strcmp(name, "EOR") && count == 2 && a[0].kind == K_BITS && a[1].kind == K_BITS)
        *r = val_c(K_BITS, BT_LANE, "sg_l_xor(g, %s, %s)", raise(a[0], BT_LANE).c, raise(a[1], BT_LANE).c);
    else if ((!strcmp(name, "ZeroExtend") || !strcmp(name, "SignExtend")) && count == 2 && a[0].bt == BT_LANE &&
             a[0].kind == K_BITS && a[1].bt <= BT_STATIC)
        *r = val_c(K_BITS, BT_LANE, "sg_l_ext(g, %s, %s, %d)", a[0].c, a[1].c, name[0] == 'S');
    else {
        lane_refuse("a builtin lanes don't have");
        *r = a[0];
    }
    return 1;
}

static int builtin_body(unsigned n, const char* name, Val* r);
static int builtin(unsigned n, const char* name, Val* r) {
    unsigned saved_node = g_gen.memo_node, saved_count = g_gen.memo_count;
    Val saved[8];
    int done;
    memcpy(saved, g_gen.memo, sizeof saved);
    g_gen.memo_node = 0;
    done = g_gen.lane && lane_builtin(n, name, r);
    if (!done) done = builtin_body(n, name, r);
    g_gen.memo_node = saved_node;
    g_gen.memo_count = saved_count;
    memcpy(g_gen.memo, saved, sizeof saved);
    return done;
}

static int builtin_body(unsigned n, const char* name, Val* r) {
    Val a, b, c;
    if (!strcmp(name, "Elem_read")) { /* Elem_read(x, e, size): element e of size bits of x */
        Val e, size;
        need_args(n, 3);
        a = need_bits(n, arg(n, 0), name);
        e = need_static_int(n, arg(n, 1), "an element's number");
        size = need_static_int(n, arg(n, 2), "an element's size");
        if (a.bt == BT_DYN)
            *r = val_c(K_BITS, BT_DYN, "sg_d_slice(g, %s, ((%s) + 1) * (%s) - 1, (%s) * (%s))", a.c, e.c, size.c, e.c,
                       size.c);
        else
            *r = val_c(K_BITS, BT_STATIC, "sg_s_slice(%s, ((%s) + 1) * (%s) - 1, (%s) * (%s))", a.c, e.c, size.c, e.c,
                       size.c);
        return 1;
    }
    if (!strcmp(name, "UInt") || !strcmp(name, "SInt")) {
        int s = name[0] == 'S';
        need_args(n, 1);
        a = need_bits(n, arg(n, 0), name);
        *r = a.bt == BT_DYN ? val_c(K_INT, BT_DYN, "sg_i_%s(g, %s)", s ? "sint" : "uint", a.c)
                            : val_c(K_INT, BT_STATIC, "sg_s_%s(%s)", s ? "sint" : "uint", a.c);
        return 1;
    }
    if (!strcmp(name, "asl_Int")) { /* asl_Int(x, unsigned) */
        need_args(n, 2);
        a = need_bits(n, arg(n, 0), name);
        b = arg(n, 1);
        if (b.kind != K_BOOL || b.bt == BT_DYN) sg_fail(n, "asl_Int's signedness must be known when it recompiles");
        if (a.bt == BT_DYN)
            *r = val_c(K_INT, BT_DYN, "((%s) ? sg_i_uint(g, %s) : sg_i_sint(g, %s))", b.c, a.c, a.c);
        else
            *r = val_c(K_INT, BT_STATIC, "((%s) ? sg_s_uint(%s) : sg_s_sint(%s))", b.c, a.c, a.c);
        return 1;
    }
    if (!strcmp(name, "ZeroExtend") || !strcmp(name, "SignExtend")) {
        int s = name[0] == 'S';
        need_args(n, 2);
        a = need_bits(n, arg(n, 0), name);
        b = need_static_int(n, arg(n, 1), "an extension's width");
        if (a.bt != BT_DYN && wide(b)) a = raise(a, BT_DYN);
        *r = a.bt == BT_DYN ? val_c(K_BITS, BT_DYN, "sg_d_%s(g, %s, %s)", s ? "sext" : "zext", a.c, b.c)
                            : val_c(K_BITS, BT_STATIC, "sg_s_%s(%s, %s)", s ? "sext" : "zext", a.c, b.c);
        return 1;
    }
    if (!strcmp(name, "Extend")) { /* Extend(x, n, unsigned) */
        need_args(n, 3);
        a = need_bits(n, arg(n, 0), name);
        b = need_static_int(n, arg(n, 1), "an extension's width");
        c = arg(n, 2);
        if (c.kind != K_BOOL || c.bt == BT_DYN) sg_fail(n, "Extend's signedness must be known when it recompiles");
        if (a.bt == BT_DYN)
            *r = val_c(K_BITS, BT_DYN, "((%s) ? sg_d_zext(g, %s, %s) : sg_d_sext(g, %s, %s))", c.c, a.c, b.c, a.c, b.c);
        else
            *r = val_c(K_BITS, BT_STATIC, "((%s) ? sg_s_zext(%s, %s) : sg_s_sext(%s, %s))", c.c, a.c, b.c, a.c, b.c);
        return 1;
    }
    if (!strcmp(name, "Zeros") || !strcmp(name, "Ones")) {
        need_args(n, 1);
        a = need_static_int(n, arg(n, 0), "a width");
        if (wide(a)) /* a SIMD&FP register's 128 bits: a map of constant bits */
            *r = val_c(K_BITS, BT_DYN, "sg_d_fill(%s, %d)", a.c, name[0] == 'O');
        else
            *r = val_c(K_BITS, BT_STATIC, "sg_s_%s(%s)", name[0] == 'Z' ? "zeros" : "ones", a.c);
        return 1;
    }
    if (!strcmp(name, "Replicate")) {
        need_args(n, 2);
        a = need_bits(n, arg(n, 0), name);
        b = need_static_int(n, arg(n, 1), "Replicate's count");
        *r = a.bt == BT_DYN ? val_c(K_BITS, BT_DYN, "sg_d_replicate(g, %s, %s)", a.c, b.c)
                            : val_c(K_BITS, BT_STATIC, "sg_s_replicate(%s, %s)", a.c, b.c);
        return 1;
    }
    if (!strcmp(name, "IsZero") || !strcmp(name, "IsZeroBit")) {
        int bit = name[6] == 'B';
        need_args(n, 1);
        a = need_bits(n, arg(n, 0), name);
        if (a.bt == BT_DYN)
            *r = bit ? val_c(K_BITS, BT_DYN, "sg_d_is_zero_bit(g, %s)", a.c)
                     : val_c(K_BOOL, BT_DYN, "sg_d_is_zero(g, %s)", a.c);
        else
            *r = bit ? val_c(K_BITS, BT_STATIC, "sg_s((%s).v == 0, 1)", a.c)
                     : val_c(K_BOOL, BT_STATIC, "((%s).v == 0)", a.c);
        return 1;
    }
    if (!strcmp(name, "not_vec")) {
        need_args(n, 1);
        a = need_bits(n, arg(n, 0), name);
        *r = a.bt == BT_DYN ? val_c(K_BITS, BT_DYN, "sg_d_not(g, %s)", a.c)
                            : val_c(K_BITS, BT_STATIC, "sg_s_not(%s)", a.c);
        return 1;
    }
    if (!strcmp(name, "not_bool")) {
        need_args(n, 1);
        a = arg(n, 0);
        if (a.kind != K_BOOL) sg_fail(n, "not_bool of other than a bool");
        if (a.bt == BT_CONST)
            *r = val_const(K_BOOL, !a.value);
        else if (a.bt == BT_STATIC)
            *r = val_c(K_BOOL, BT_STATIC, "(!(%s))", a.c);
        else
            *r = val_c(K_BOOL, BT_DYN, "LLVMBuildNot(SG_B, %s, \"\")", a.c);
        return 1;
    }
    if (!strcmp(name, "EOR")) {
        need_args(n, 2);
        a = need_bits(n, arg(n, 0), name);
        b = need_bits(n, arg(n, 1), name);
        if (a.bt != BT_DYN && b.bt != BT_DYN)
            *r = val_c(K_BITS, BT_STATIC, "sg_s_xor(%s, %s)", a.c, b.c);
        else
            *r = val_c(K_BITS, BT_DYN, "sg_d_xor(g, %s, %s)", raise(a, BT_DYN).c, raise(b, BT_DYN).c);
        return 1;
    }
    if (!strcmp(name, "Bit")) {
        need_args(n, 1);
        *r = need_bits(n, arg(n, 0), name);
        return 1;
    }
    if (!strcmp(name, "LSL") || !strcmp(name, "LSR") || !strcmp(name, "ASR") || !strcmp(name, "ROR")) {
        *r = shift(n, name[0] == 'L' ? (name[2] == 'L' ? 0 : 1) : name[0] == 'A' ? 2 : 3);
        return 1;
    }
    if (!strcmp(name, "MOD") || !strcmp(name, "DIV")) {
        need_args(n, 2);
        a = arg(n, 0);
        b = need_static_int(n, arg(n, 1), "a divisor");
        if (a.kind != K_INT) sg_fail(n, "%s of other than an integer", name);
        if (a.bt == BT_DYN) {
            if (name[0] != 'M') sg_fail(n, "DIV of an integer known only at run time");
            *r = val_c(K_INT, BT_DYN, "sg_i_mod(g, %s, %s)", a.c, b.c);
        } else if (a.bt == BT_CONST && b.bt == BT_CONST && b.value) {
            long long m = a.value % b.value;
            if (m < 0) m += b.value < 0 ? -b.value : b.value;
            *r = val_const(K_INT, name[0] == 'M' ? m : (a.value - m) / b.value);
        } else
            *r = val_c(K_INT, BT_STATIC, "sg_%s(%s, %s)", name[0] == 'M' ? "mod" : "div", a.c, b.c);
        return 1;
    }
    if (!strcmp(name, "RoundTowardsZero")) { /* RoundTowardsZero(Real(a) / Real(b)): the integer quotient */
        unsigned q = arg_at(n, 0);
        need_args(n, 1);
        if (N(q).kind != N_BINOP || strcmp(name_of(q), "/") || !is_call(N(q).a, "Real") || !is_call(N(q).b, "Real"))
            sg_fail(n, "RoundTowardsZero of other than Real(a) / Real(b)");
        a = eval(arg_at(N(q).a, 0), 0);
        b = eval(arg_at(N(q).b, 0), 0);
        if (a.kind != K_INT || b.kind != K_INT) sg_fail(n, "a quotient of other than integers");
        *r = val_c(K_INT, BT_DYN, "sg_i_div(g, %s, %s)", raise(a, BT_DYN).c, raise(b, BT_DYN).c);
        return 1;
    }
    if (!strcmp(name, "CountLeadingZeroBits") || !strcmp(name, "CountLeadingSignBits")) {
        need_args(n, 1);
        a = need_bits(n, arg(n, 0), name);
        if (a.bt != BT_DYN) sg_fail(n, "%s of a value the recompiler knows: not needed yet", name);
        *r = val_c(K_INT, BT_DYN, "sg_i_%s(g, %s)", !strcmp(name, "CountLeadingZeroBits") ? "clz" : "cls", a.c);
        return 1;
    }
    if (!strcmp(name, "HighestSetBit")) {
        need_args(n, 1);
        a = need_bits(n, arg(n, 0), name);
        if (a.bt == BT_DYN) sg_fail(n, "HighestSetBit of a value known only at run time");
        *r = val_c(K_INT, BT_STATIC, "sg_highest_set_bit(%s)", a.c);
        return 1;
    }
    if (!strcmp(name, "BitReverse")) {
        need_args(n, 1);
        if (crc_rule(arg_at(n, 0), r)) return 1;
        a = need_bits(n, arg(n, 0), name);
        *r = val_c(K_BITS, BT_DYN, "sg_d_bitreverse(g, %s)", raise(a, BT_DYN).c);
        return 1;
    }
    if (!strcmp(name, "AddWithCarry")) {
        char t1[16], t2[16];
        Val parts[2];
        need_args(n, 3);
        a = raise(need_bits(n, arg(n, 0), name), BT_DYN);
        b = raise(need_bits(n, arg(n, 1), name), BT_DYN);
        c = raise(need_bits(n, arg(n, 2), name), BT_DYN);
        temp(t1, sizeof t1);
        temp(t2, sizeof t2);
        emit("SgD %s, %s;\n", t1, t2);
        emit("sg_add_with_carry(g, %s, %s, %s, &%s, &%s);\n", a.c, b.c, c.c, t1, t2);
        parts[0] = val_c(K_BITS, BT_DYN, "%s", t1);
        parts[1] = val_c(K_BITS, BT_DYN, "%s", t2);
        *r = tuple_of(parts, 2, n);
        return 1;
    }
    if (!strcmp(name, "ConditionHolds")) {
        /* the compare that set the flags, when one did in the block; else ConditionHolds's own Sail */
        unsigned def = sg_find(name, D_FUNCTION);
        char t[16];
        Val v;
        need_args(n, 1);
        if (!def) sg_fail(n, "ConditionHolds has no definition");
        a = keep(need_bits(n, arg(n, 0), name));
        if (a.bt == BT_DYN) sg_fail(n, "ConditionHolds of a condition known only at run time");
        temp(t, sizeof t);
        emit("LLVMValueRef %s = sg_condition_origin(g, %s);\n", t, a.c);
        emit("if (!%s) {\n", t);
        ++g_gen.indent;
        v = inline_call(n, def, &a, 1);
        if (v.kind != K_BOOL) sg_fail(n, "ConditionHolds's Sail gave other than a bool");
        emit("%s = %s;\n", t, raise(v, BT_DYN).c);
        --g_gen.indent;
        emit("}\n");
        *r = val_c(K_BOOL, BT_DYN, "%s", t);
        return 1;
    }
    if (!strcmp(name, "IsFeatureImplemented")) {
        unsigned f = arg_at(n, 0);
        need_args(n, 1);
        if (N(f).kind != N_ID || strncmp(name_of(f), "FEAT_", 5))
            sg_fail(n, "IsFeatureImplemented of other than a FEAT_");
        /* the feature's bit from a64.profile (a64_decode.h, its names in capitals); one the profile names in no
           feature line no guest has, so its ifs fold (TME, BRBE, BTI) */
        {
            char upper[64], word[68];
            size_t k;
            snprintf(upper, sizeof upper, "%s", name_of(f));
            for (k = 0; upper[k]; ++k) upper[k] = (char)toupper((unsigned char)upper[k]);
            snprintf(word, sizeof word, " %s ", upper);
            if (g_gen.features && !strstr(g_gen.features, word))
                *r = val_const(K_BOOL, 0);
            else
                *r = val_c(K_BOOL, BT_STATIC, "sg_feature(g, SNX_A64_%s)", upper);
        }
        return 1;
    }
    if (!strcmp(name, "Min") || !strcmp(name, "Max")) {
        int min = name[1] == 'i';
        need_args(n, 2);
        a = need_static_int(n, arg(n, 0), name);
        b = need_static_int(n, arg(n, 1), name);
        if (a.bt == BT_CONST && b.bt == BT_CONST)
            *r = val_const(K_INT, (a.value < b.value) == min ? a.value : b.value);
        else
            *r = val_c(K_INT, BT_STATIC, "((%s) %s (%s) ? (%s) : (%s))", a.c, min ? "<" : ">", b.c, a.c, b.c);
        return 1;
    }
    if (!strcmp(name, "negate")) {
        need_args(n, 1);
        a = need_static_int(n, arg(n, 0), name);
        *r = a.bt == BT_CONST ? val_const(K_INT, -a.value) : val_c(K_INT, BT_STATIC, "(-(%s))", a.c);
        return 1;
    }
    if (!strcmp(name, "Slice")) { /* Slice(x, lo, n): x[lo + n - 1 .. lo] */
        need_args(n, 3);
        a = need_bits(n, arg(n, 0), name);
        b = need_static_int(n, arg(n, 1), "a slice's start");
        c = need_static_int(n, arg(n, 2), "a slice's width");
        *r = a.bt == BT_DYN ? val_c(K_BITS, BT_DYN, "sg_d_slice(g, %s, (%s) + (%s) - 1, %s)", a.c, b.c, c.c, b.c)
                            : val_c(K_BITS, BT_STATIC, "sg_s_slice(%s, (%s) + (%s) - 1, %s)", a.c, b.c, c.c, b.c);
        return 1;
    }
    if (!strcmp(name, "BranchTo")) { /* BranchTo(target, type, conditional): where the instruction goes, and when */
        need_args(n, 3);
        a = raise(need_bits(n, arg(n, 0), "a branch's target"), BT_DYN);
        emit("sg_branch_to(g, %s, %s);\n", a.c, g_gen.path[0] ? g_gen.path : "NULL");
        *r = unit();
        return 1;
    }
    if (!strcmp(name, "BranchNotTaken")) { /* the way on: nothing to record */
        need_args(n, 2);
        *r = unit();
        return 1;
    }
    if (!strcmp(name, "EndOfInstruction")) { /* the instruction ends here: a hint without its feature is a NOP */
        need_args(n, 0);
        end_decode(n, "EndOfInstruction()", 0);
        *r = unit();
        return 1;
    }
    if (!strcmp(name, "Elem_set")) { /* Elem_set(x, e, size, v): x with its element e of size bits made v */
        Val e, size, v;
        char t[16];
        need_args(n, 4);
        a = need_bits(n, arg(n, 0), name);
        e = need_static_int(n, arg(n, 1), "an element's number");
        size = need_static_int(n, arg(n, 2), "an element's size");
        v = need_bits(n, arg(n, 3), name);
        if (a.bt != BT_DYN && v.bt != BT_DYN) {
            *r = val_c(K_BITS, BT_STATIC, "sg_s_set_slice(%s, ((%s) + 1) * (%s) - 1, (%s) * (%s), %s)", a.c, e.c,
                       size.c, e.c, size.c, v.c);
            return 1;
        }
        temp(t, sizeof t);
        emit("SgD %s = %s;\n", t, raise(a, BT_DYN).c);
        emit("sg_d_set_slice(g, &%s, ((%s) + 1) * (%s) - 1, (%s) * (%s), %s);\n", t, e.c, size.c, e.c, size.c,
             raise(v, BT_DYN).c);
        *r = val_c(K_BITS, BT_DYN, "%s", t);
        return 1;
    }
    if (!strcmp(name, "Unreachable")) {
        need_args(n, 0);
        emit("sg_fail(\"Unreachable() reached at %s:%u\", 0, 0);\n", sg_str(N(n).file), N(n).line);
        *r = unit();
        return 1;
    }
    return 0;
}

/* the parts of a tuple a lowered function gives, as its val spells it ((bits('N), bits(1))): how many, each bits */
static unsigned tuple_parts(unsigned n, unsigned val) {
    const SgDef* d = &g_parse.defs[val];
    const char *text = g_parse.files[d->source].text, *s = text + d->text_start, *e = text + d->text_end, *arrow = NULL;
    unsigned parts = 1;
    int depth = 0;
    for (; s < e; ++s) {
        if (s[0] == '/' && s + 1 < e && s[1] == '/') { /* a comment before the val */
            while (s < e && *s != '\n') ++s;
            continue;
        }
        if (*s == '(' || *s == '{') ++depth;
        if (*s == ')' || *s == '}') --depth;
        if (!depth && *s == '-' && s + 1 < e && s[1] == '>') arrow = s;
    }
    for (s = arrow ? arrow + 2 : e; s < e && isspace((unsigned char)*s); ++s) {}
    if (s == e || *s != '(') sg_fail(n, "%s's result is a tuple its val does not spell as (T, ...)", sg_str(d->name));
    for (depth = 0; s < e; ++s) {
        if (*s == '(' || *s == ',') {
            const char* t = s + 1;
            if (*s == '(') ++depth;
            if (depth != 1) continue;
            while (t < e && isspace((unsigned char)*t)) ++t;
            if (strncmp(t, "bit", 3)) sg_fail(n, "%s's result has a part other than bits", sg_str(d->name));
            parts += *s == ',';
        } else if (*s == ')' && --depth == 0)
            break;
    }
    if (parts > 4) sg_fail(n, "%s's result has over 4 parts", sg_str(d->name));
    return parts;
}

/* NAME(args), a function with no Sail definition that the back end lowers as sg_NAME: the machine's action (a barrier,
   an event, a guest memory access), or a function whose Sail a64.roots' lower leaves out, its val kept (the
   floating-point arithmetic, on real numbers). sg_NAME takes the context, then (BACKEND_PATH) the run-time condition the
   call is under, NULL for none, so what it changes it changes only there (FCCMP's compare), then (BACKEND_ARGS) the
   arguments: bits as the back end's values whatever their time, the rest as C values or IR as they come. A call under
   a run-time condition needs the path. Its result is its val's: none, bits or a bool (IR: nx.sail's GuestMemRead), an
   enum (known when it recompiles: a member, or a value of the back end's own, FPRoundingMode's "the mode FPCR holds
   when the guest runs") or a tuple of bits, whose parts come out through pointers after the arguments */
static Val action_call(unsigned n, const char* name, int takes) {
    unsigned count = arg_count(n), i, parts = 0, val = sg_find(name, D_VAL);
    int kind = val ? (int)g_parse.defs[val].ret_kind : K_UNIT, at;
    char text[1024], t[16];
    Val outs[4];
    if (g_gen.dynamic_branch && !(takes & BACKEND_PATH))
        sg_fail(n, "%s, the back end's action, under a condition known only at run time", name);
    if (g_gen.in_helper) sg_fail(n, "%s, the back end's action, in a function of what the recompiler knows", name);
    if (g_gen.lane) lane_refuse("the back end's action in an element loop, which lanes run once");
    if (kind == K_BIT) kind = K_BITS;
    if (kind == K_TUPLE) parts = tuple_parts(n, val);
    if (kind != K_UNIT && kind != K_NONE && kind != K_BITS && kind != K_BOOL && kind != K_ENUM && kind != K_TUPLE)
        sg_fail(n, "%s gives %s: the back end's functions give nothing, bits, a bool, an enum or a tuple of bits", name,
                kind == K_INT    ? "an integer"
                : kind == K_REAL ? "a real"
                                 : "a value of a kind sailgen does not keep");
    at = snprintf(text, sizeof text, "sg_%s(g", name);
    if (takes & BACKEND_PATH)
        at += snprintf(text + at, sizeof text - (size_t)at, ", %s", g_gen.path[0] ? g_gen.path : "NULL");
    for (i = 0; (takes & BACKEND_ARGS) && i < count; ++i) {
        Val a = eval(arg_at(n, i), 0);
        if (a.kind == K_UNIT || a.kind == K_TUPLE) sg_fail(n, "%s's argument %u is no value", name, i + 1);
        if (a.kind == K_BITS) a = raise(a, BT_DYN);
        a = keep(a);
        at += snprintf(text + at, sizeof text - (size_t)at, ", %s", a.c);
        if (at >= (int)sizeof text) sg_fail(n, "%s's arguments over %zu characters", name, sizeof text);
    }
    for (i = 0; i < parts; ++i) {
        temp(t, sizeof t);
        emit("SgD %s;\n", t);
        outs[i] = val_c(K_BITS, BT_DYN, "%s", t);
        at += snprintf(text + at, sizeof text - (size_t)at, ", &%s", t);
        if (at >= (int)sizeof text) sg_fail(n, "%s's arguments over %zu characters", name, sizeof text);
    }
    if (kind == K_UNIT || kind == K_NONE || kind == K_TUPLE) {
        emit("%s);\n", text);
        return kind == K_TUPLE ? tuple_of(outs, parts, n) : unit();
    }
    temp(t, sizeof t);
    emit("%s %s = %s);\n", kind == K_BITS ? "SgD" : kind == K_BOOL ? "LLVMValueRef" : "int", t, text);
    return val_c(kind, kind == K_ENUM ? BT_STATIC : BT_DYN, "%s", t);
}

static Val call(unsigned n) {
    const char* name = name_of(n);
    unsigned count = arg_count(n), def, i;
    Val args[16], r;
    int all_static = 1, all_const = 1;
    if (builtin(n, name, &r)) return r;
    def = function_def(name, count);
    if (!def && !sg_find(name, D_FUNCTION) && !sg_find(name, D_OVERLOAD)) {
        int takes = backend_action(name);
        if (takes >= 0) return action_call(n, name, takes);
    }
    if (!def) sg_fail(n, "%s has no definition of %u arguments and is not one of sailgen's builtins", name, count);
    if (count > 16) sg_fail(n, "%s called with over 16 arguments", name);
    for (i = 0; i < count; ++i) {
        args[i] = eval(arg_at(n, i), 0);
        args[i].node = arg_at(n, i);
        all_static &=
            args[i].bt != BT_DYN && args[i].kind != K_TUPLE && args[i].kind != K_UNIT && args[i].kind != K_STRUCT;
        all_const &= args[i].bt == BT_CONST;
    }
    /* arguments all known here (none, or constants) inline, so a fixup's constant answer (nx.sail's HaveGCS) folds */
    if (all_static && !all_const && !reaches_state(def, 0)) return helper_call(n, def, args, count);
    for (i = 0; i < count; ++i) args[i] = keep(args[i]);
    return inline_call(n, def, args, count);
}

/* ---- the state: the Sail's registers ----
   A register a decode function reaches is read and written through the back end's functions named after it,
   sg_NAME_read and sg_NAME_write: the whole value of a register of bits, an element of a vector of them (nx.sail's
   GPR[n]), a field of a struct (PSTATE.N, which the back end numbers as SG_PSTATE_N). Consecutive fields assigned
   together (PSTATE.N @ PSTATE.Z @ PSTATE.C @ PSTATE.V = nzcv) are one write, of the first and how many, so the back end
   gets the four flags AddWithCarry makes as one value. Where the runtime keeps a register, and the cache of its
   values, are the back end's; one it does not define stops sailgen at the line that reaches it. The pc is the one
   register the recompiler knows: the instruction's own */

/* the register an unbound name n is: its definition, else 0 (a top-level let of that name, an overlay's, is a
   constant) */
static unsigned register_def(unsigned n) {
    const char* s;
    if (!n || N(n).kind != N_ID) return 0;
    s = name_of(n);
    if (lookup(s) || sg_find(s, D_LET)) return 0;
    return sg_find(s, D_REGISTER);
}
static const char* reg_name(unsigned reg) { return sg_str(g_parse.defs[reg].name); }
static int reg_kind(unsigned reg) { return g_parse.defs[reg].params ? (int)N(g_parse.defs[reg].params).value : K_NONE; }

/* the back end's text with its comments and string and character literals blanked, lines kept: a name in them
   defines nothing */
static char* backend_code(const char* text, size_t size) {
    char* c = (char*)malloc(size + 1);
    size_t i = 0;
    if (!c) sg_fail(0, "out of memory for the back end");
    memcpy(c, text, size);
    c[size] = 0;
    while (i < size) {
        if (c[i] == '/' && c[i + 1] == '*') {
            for (; i < size && !(c[i] == '*' && c[i + 1] == '/'); ++i)
                if (c[i] != '\n') c[i] = ' ';
            if (i < size) {
                c[i] = c[i + 1] = ' ';
                i += 2;
            }
        } else if (c[i] == '/' && c[i + 1] == '/') {
            for (; i < size && c[i] != '\n'; ++i) c[i] = ' ';
        } else if (c[i] == '"' || c[i] == '\'') {
            char quote = c[i++];
            for (; i < size && c[i] != quote && c[i] != '\n'; ++i) {
                if (c[i] == '\\' && i + 1 < size) c[i++] = ' ';
                c[i] = ' ';
            }
            ++i;
        } else
            ++i;
    }
    return c;
}
static int ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }
static const char* skip_blank(const char* s) {
    while (*s && isspace((unsigned char)*s)) ++s;
    return s;
}
/* where the back end's code holds name as a whole word from, after from (NULL to start); NULL when nowhere else */
static const char* backend_word(const char* name, const char* from) {
    const char *code = g_gen.backend_code, *s = from ? from + strlen(name) : code;
    size_t n = strlen(name);
    while ((s = strstr(s, name)) != NULL) {
        if ((s == code || !ident_char(s[-1])) && !ident_char(s[n])) return s;
        s += n;
    }
    return NULL;
}
/* the start of the line s is on, its indentation skipped */
static const char* line_start(const char* s) {
    while (s > g_gen.backend_code && s[-1] != '\n') --s;
    return skip_blank(s);
}
/* the back end defines name as a function: a line that starts with static and names it, its parameters, then its
   body's brace */
static int backend_defines_function(const char* name) {
    const char* s = NULL;
    while ((s = backend_word(name, s)) != NULL) {
        const char* p = skip_blank(s + strlen(name));
        int depth = 0;
        if (strncmp(line_start(s), "static", 6) || *p != '(') continue;
        for (; *p; ++p)
            if (*p == '(')
                ++depth;
            else if (*p == ')' && --depth == 0)
                break;
        if (*p && *skip_blank(p + 1) == '{') return 1;
    }
    return 0;
}
/* the back end lowers name, a function with no Sail definition, as sg_NAME (action_call): -1 when it does not, else
   what sg_NAME takes after the context: BACKEND_PATH when its next parameter is named path, BACKEND_ARGS when the call's
   arguments follow (none: they mean nothing to it) */
static const char* word_before(const char* s, size_t* n);
static int backend_action(const char* name) {
    char fn[160];
    const char* s = NULL;
    if (!g_gen.backend_code) return -1;
    snprintf(fn, sizeof fn, "sg_%s", name);
    if (!backend_defines_function(fn)) return -1;
    while ((s = backend_word(fn, s)) != NULL) {
        const char *p = skip_blank(s + strlen(fn)), *e, *w;
        int takes = 0;
        size_t length;
        if (strncmp(line_start(s), "static", 6) || *p != '(') continue;
        for (; *p && *p != ')' && *p != ','; ++p) {}
        if (*p != ',') return 0;
        for (e = p + 1; *e && *e != ')' && *e != ','; ++e) {} /* the parameter after the context, its name last */
        w = word_before(e, &length);
        if (length == 4 && !strncmp(w, "path", 4)) {
            takes |= BACKEND_PATH;
            p = e;
        }
        return takes | (*p == ',' ? BACKEND_ARGS : 0);
    }
    return -1;
}
/* the word that ends right before s, blanks skipped: where it starts, and its length in *n (0: none, at the start
   or after other than a name) */
static const char* word_before(const char* s, size_t* n) {
    const char* e = s;
    while (e > g_gen.backend_code && isspace((unsigned char)e[-1])) --e;
    s = e;
    while (s > g_gen.backend_code && ident_char(s[-1])) --s;
    *n = (size_t)(e - s);
    return s;
}
/* the back end defines name as a constant: a #define of it, or a member of an enum's list ("enum {" or
   "enum TAG {", then names, each first or after a comma, and before a comma, a "}" or an "=") */
static int backend_defines_constant(const char* name) {
    const char* s = NULL;
    while ((s = backend_word(name, s)) != NULL) {
        const char *line = line_start(s), *open = s, *after = skip_blank(s + strlen(name)), *w;
        size_t n;
        if (!strncmp(line, "#define", 7) && skip_blank(line + 7) == s) return 1;
        if (*after != ',' && *after != '}' && *after != '=') continue;
        while (open > g_gen.backend_code && !strchr("{};", open[-1])) --open;
        if (open == g_gen.backend_code || open[-1] != '{') continue;
        w = s;
        while (w > open && isspace((unsigned char)w[-1])) --w;
        if (w != open && w[-1] != ',') continue;
        w = word_before(open - 1, &n);
        if (n == 4 && !strncmp(w, "enum", 4)) return 1;
        if (n && (w = word_before(w, &n)) != NULL && n == 4 && !strncmp(w, "enum", 4)) return 1;
    }
    return 0;
}
/* sg_REG_read or sg_REG_write (what), which the back end must define */
static const char* reg_access(unsigned n, unsigned reg, const char* what) {
    static char name[160];
    snprintf(name, sizeof name, "sg_%s_%s", reg_name(reg), what);
    if (!backend_defines_function(name))
        sg_fail(n, "the register %s: the back end %s defines no %s", reg_name(reg), g_gen.backend_name, name);
    return name;
}
/* SG_REG_FIELD, the back end's number for the field */
static const char* reg_field(unsigned n, unsigned reg, const char* field) {
    static char name[160];
    snprintf(name, sizeof name, "SG_%s_%s", reg_name(reg), field);
    if (!backend_defines_constant(name))
        sg_fail(n, "%s.%s: the back end %s defines no %s", reg_name(reg), field, g_gen.backend_name, name);
    return name;
}

/* the struct a register is of: its definition, else 0 */
static unsigned reg_struct(unsigned reg) { return type_struct(g_parse.defs[reg].params); }
/* n is REG.FIELD, REG a struct register: REG's definition, and the field's place among the struct's in *index;
   0 when n is not a register's field */
static unsigned field_of(unsigned n, unsigned* index) {
    unsigned reg, st, f, k = 0;
    if (N(n).kind != N_FIELD || !(reg = register_def(N(n).a))) return 0;
    if (!(st = reg_struct(reg)))
        sg_fail(n, "%s.%s: the register %s is of no struct", reg_name(reg), name_of(n), reg_name(reg));
    for (f = g_parse.defs[st].params; f; f = N(f).next, ++k)
        if (!strcmp(name_of(N(f).a), name_of(n))) {
            *index = k;
            return reg;
        }
    sg_fail(n, "%s.%s: the struct %s has no field %s", reg_name(reg), name_of(n), sg_str(g_parse.defs[st].name),
            name_of(n));
    return 0;
}

/* REG, a register of bits, read whole */
static Val register_read(unsigned n, unsigned reg) {
    if (reg_kind(reg) != K_BITS)
        sg_fail(n,
                "the register %s read whole: sailgen reads a register of bits whole, a vector one by element and a "
                "struct one by field",
                reg_name(reg));
    return val_c(K_BITS, BT_DYN, "%s(g)", reg_access(n, reg, "read"));
}
/* REG[i], an element of a vector register: i known when it recompiles */
static Val element_read(unsigned n, unsigned reg) {
    Val i;
    if (reg_kind(reg) != K_VECTOR) sg_fail(n, "%s[...]: the register %s is not a vector", reg_name(reg), reg_name(reg));
    i = need_static_int(n, eval(N(n).b, 0), "a register's element number");
    return val_c(K_BITS, BT_DYN, "%s(g, %s)", reg_access(n, reg, "read"), i.c);
}
/* the top-level let REG_FIELD (an overlay's) that pins the field FIELD of the struct register REG to a constant, else 0 */
static unsigned field_pin(unsigned reg, const char* field) {
    char name[160];
    snprintf(name, sizeof name, "%s_%s", reg_name(reg), field);
    return sg_find(name, D_LET);
}
/* the field n names of a value of the struct sdef, as a value of that field's kind known when it recompiles: its C
   member of the struct's C type (struct_ctype) */
static Val struct_field(unsigned n, unsigned sdef, const char* of) {
    unsigned f, st;
    Val v;
    for (f = g_parse.defs[sdef].params; f && strcmp(name_of(N(f).a), name_of(n)); f = N(f).next) {}
    if (!f) sg_fail(n, "%s: the struct %s has no field %s", name_of(n), sg_str(g_parse.defs[sdef].name), name_of(n));
    st = type_struct(N(f).b);
    switch (st ? K_STRUCT : (int)N(N(f).b).value) {
        case K_BOOL:
        case K_ENUM:
        case K_INT:
        case K_BITS:
        case K_BIT:
        case K_STRUCT: break;
        default:
            sg_fail(n, "the field %s of %s: a type sailgen does not keep", name_of(n), sg_str(g_parse.defs[sdef].name));
    }
    v = val_c(st ? K_STRUCT : (int)N(N(f).b).value, BT_STATIC, "(%s).%s", of, name_of(n));
    v.sdef = st;
    return v;
}

/* REG.FIELD, a field of a struct register, or a field of a struct value */
static Val field_read(unsigned n) {
    unsigned index, reg = field_of(n, &index), pin;
    const char* field;
    char fn[160];
    if (!reg) {
        Val x = eval(N(n).a, 0);
        if (x.kind != K_STRUCT) sg_fail(n, "the field %s of other than a register or a struct", name_of(n));
        return struct_field(n, x.sdef, x.c);
    }
    if ((pin = field_pin(reg, name_of(n))) != 0) { /* the machine holds it fixed: nx.sail's PSTATE_EL */
        Val v = eval(g_parse.defs[pin].body, 0);
        if (v.bt == BT_DYN) sg_fail(n, "the top-level let %s_%s is not a constant", reg_name(reg), name_of(n));
        return v;
    }
    snprintf(fn, sizeof fn, "%s", reg_access(n, reg, "read"));
    field = reg_field(n, reg, name_of(n));
    return val_c(K_BITS, BT_DYN, "%s(g, %s)", fn, field);
}

/* the leaves of an @ chain, left to right: how many (at most cap) */
static unsigned concat_leaves(unsigned n, unsigned* out, unsigned cap) {
    if (N(n).kind == N_BINOP && !strcmp(name_of(n), "@")) {
        unsigned k = concat_leaves(N(n).a, out, cap);
        return k + concat_leaves(N(n).b, out + k, cap > k ? cap - k : 0);
    }
    if (!cap) return 1;
    out[0] = n;
    return 1;
}

/* the register written is not under a run-time condition (its branches are selects, which a store is not) */
static void need_static_write(unsigned n, unsigned reg) {
    if (g_gen.dynamic_branch)
        sg_fail(n, "the register %s written under a condition known only at run time", reg_name(reg));
}
/* REG.F = v, or REG.F1 @ REG.F2 @ ... = v of a struct register's consecutive fields: one write, of the first and how
   many */
static void fields_write(unsigned t, Val v) {
    unsigned leaves[8], count = concat_leaves(t, leaves, 8), reg = 0, first = 0, k;
    char fn[160], field[160];
    if (count > 8) sg_fail(t, "over 8 fields assigned at once");
    for (k = 0; k < count; ++k) {
        unsigned index = 0, r = field_of(leaves[k], &index);
        if (!r) sg_fail(leaves[k], "assigned other than a register's field");
        if (field_pin(r, name_of(leaves[k])))
            sg_fail(leaves[k], "%s.%s written, which the let %s_%s holds fixed", reg_name(r), name_of(leaves[k]),
                    reg_name(r), name_of(leaves[k]));
        if (!k) {
            reg = r;
            first = index;
        } else if (r != reg)
            sg_fail(leaves[k], "fields of two registers assigned at once");
        else if (index != first + k)
            sg_fail(leaves[k], "fields assigned other than consecutive, in their struct's order");
    }
    need_static_write(t, reg);
    snprintf(fn, sizeof fn, "%s", reg_access(t, reg, "write"));
    snprintf(field, sizeof field, "%s", reg_field(t, reg, name_of(leaves[0])));
    v = raise(need_bits(t, v, "a register's value"), BT_DYN);
    emit("%s(g, %s, %u, %s);\n", fn, field, count, v.c);
}

/* [r[i]] @ IsZeroBit(r) @ 0b00, i r's top bit: a logical result's NZCV, lowered whole (llvm.h's sg_d_logic_flags),
   so a write of the four flags keeps r as what a condition on them compares; 0 when n is not that */
static int logic_flags_rule(unsigned n, Val* out) {
    unsigned rhs[8], bit, index, zero;
    Val r, i;
    if (concat_leaves(n, rhs, 8) != 3) return 0;
    if (N(rhs[0]).kind != N_VEC || !N(rhs[0]).a || N(N(rhs[0]).a).next) return 0;
    bit = N(rhs[0]).a;
    zero = rhs[1];
    if (N(bit).kind != N_INDEX || N(N(bit).a).kind != N_ID || !is_call(zero, "IsZeroBit") || arg_count(zero) != 1 ||
        N(arg_at(zero, 0)).kind != N_ID || strcmp(name_of(N(bit).a), name_of(arg_at(zero, 0))))
        return 0;
    if (N(rhs[2]).kind != N_BITS || N(rhs[2]).width != 2 || N(rhs[2]).value) return 0;
    if (!lookup(name_of(N(bit).a))) return 0;
    index = N(bit).b;
    r = raise(eval(N(bit).a, 0), BT_DYN);
    i = need_static_int(index, eval(index, 0), "a bit index");
    emit("if ((%s) != (long long)(%s).w - 1) sg_fail(\"N from other than the result's top bit\", %s, (%s).w);\n", i.c,
         r.c, i.c, r.c);
    *out = keep(val_c(K_BITS, BT_DYN, "sg_d_logic_flags(g, %s)", r.c));
    return 1;
}

/* ---- names, operators, slices ---- */

static Val ident(unsigned n) {
    const char* s = name_of(n);
    Binding* b = lookup(s);
    unsigned d;
    long long m;
    if (b) {
        Val v = b->val;
        if (b->var >= 0) {
            v.bt = g_gen.vars[b->var].bt;
            v.kind = g_gen.vars[b->var].kind;
            if (v.bt == BT_LANE && !g_gen.lane && (v.kind != K_BITS || g_gen.vars[b->var].lane_elem)) {
                /* only the vector Elem_set built leaves the loop */
                refuse_loop(g_gen.vars[b->var].lane_loop); /* the walk starts over: any value of its kind */
                return v.kind == K_INT    ? val_c(K_INT, BT_DYN, "sg_i_const(g, 0)")
                       : v.kind == K_BITS ? val_c(K_BITS, BT_DYN, "sg_d_empty(1u)")
                                          : val_c(K_BOOL, BT_DYN, "LLVMConstInt(g->ir->t_i1, 0, 0)");
            }
        }
        v.node = n;
        return v;
    }
    if (!strcmp(s, "_PC")) return val_c(K_BITS, BT_STATIC, "sg_s(g->pc, 64)");
    if ((d = register_def(n)) != 0) return register_read(n, d);
    if ((d = sg_find(s, D_LET)) != 0) { /* a top-level constant (EL0, a bits literal, is STATIC): an overlay's fixup */
        Val v = eval(g_parse.defs[d].body, 0);
        if (v.bt == BT_DYN) sg_fail(n, "the top-level let %s is not a constant", s);
        return v;
    }
    if ((m = enum_member(s)) >= 0) {
        Val v = val_const(K_ENUM, m);
        snprintf(v.c, sizeof v.c, "%lld", m);
        return v;
    }
    sg_fail(n, "%s is not bound", s);
    return unit();
}

/* a and b in temporaries when their C would make the operation's too long for a value (a decode's long chain of
   checks of the word's bits) */
static void shorten(Val* a, Val* b) {
    if (strlen(a->c) + strlen(b->c) < sizeof a->c / 2) return;
    *a = keep(*a);
    *b = keep(*b);
}

/* a op b, one of them a lane's: the operation on every lane (llvm.h's sg_l_*), else the loop is refused */
enum { L_EQ, L_NE, L_LT, L_LE, L_GT, L_GE };
static Val lane_binop(unsigned n, const char* op, Val a, Val b) {
    static const char* const cmps[] = {"==", "!=", "<", "<=", ">", ">="};
    unsigned i;
    (void)n;
    if ((a.kind == K_BITS) != (b.kind == K_BITS) || (a.kind == K_BOOL) != (b.kind == K_BOOL)) {
        lane_refuse("an operation on lanes of two kinds");
        return a;
    }
    a = raise(a, BT_LANE);
    b = raise(b, BT_LANE);
    for (i = 0; i < 6; ++i)
        if (!strcmp(op, cmps[i])) {
            if (a.kind == K_BITS && i > 1) break;
            return val_c(K_BOOL, BT_LANE, "sg_l_cmp(g, %u, %s, %s)", i, a.c, b.c);
        }
    if (!strcmp(op, "&") || !strcmp(op, "|"))
        return val_c(a.kind, BT_LANE, "sg_l_%s(g, %s, %s)", op[0] == '&' ? "and" : "or", a.c, b.c);
    if (a.kind == K_BITS && (!strcmp(op, "+") || !strcmp(op, "-")))
        return val_c(K_BITS, BT_LANE, "sg_l_%s(g, %s, %s)", op[0] == '+' ? "add" : "sub", a.c, b.c);
    if (a.kind == K_INT && (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*")))
        return val_c(K_INT, BT_LANE, "sg_l_i%s(g, %s, %s)",
                     op[0] == '+'   ? "add"
                     : op[0] == '-' ? "sub"
                                    : "mul",
                     a.c, b.c);
    lane_refuse("an operation lanes don't have");
    return a;
}

static Val binop(unsigned n) {
    const char* op = name_of(n);
    Val a, b;
    int bt;
    if (!strcmp(op, "@") && logic_flags_rule(n, &a)) return a;
    a = eval(N(n).a, 0);
    /* & and | of bools short-circuit: a constant left side decides without the right */
    if ((!strcmp(op, "&") || !strcmp(op, "|")) && a.kind == K_BOOL) {
        int is_and = op[0] == '&';
        if (a.bt == BT_CONST && a.value == !is_and) return val_const(K_BOOL, !is_and);
        b = eval(N(n).b, 0);
        if (b.kind != K_BOOL) sg_fail(n, "%s of a bool and other than a bool", op);
        if (a.bt == BT_LANE || b.bt == BT_LANE) {
            if (b.bt == BT_CONST) return b.value == is_and ? a : val_const(K_BOOL, !is_and);
            return lane_binop(n, op, a, b);
        }
        shorten(&a, &b);
        if (a.bt == BT_CONST) return b;
        if (b.bt == BT_CONST) return b.value == is_and ? a : val_const(K_BOOL, !is_and);
        if (a.bt == BT_STATIC && b.bt == BT_STATIC)
            return val_c(K_BOOL, BT_STATIC, "((%s) %s (%s))", a.c, is_and ? "&&" : "||", b.c);
        return val_c(K_BOOL, BT_DYN, "LLVMBuild%s(SG_B, %s, %s, \"\")", is_and ? "And" : "Or", raise(a, BT_DYN).c,
                     raise(b, BT_DYN).c);
    }
    b = eval(N(n).b, 0);
    if (a.bt == BT_LANE || b.bt == BT_LANE) return lane_binop(n, op, a, b);
    shorten(&a, &b);
    bt = top_bt(a.bt, b.bt);
    if (a.huge || b.huge) sg_fail(n, "%s of an integer over 62 bits", op);
    if (!strcmp(op, "==") || !strcmp(op, "!=")) {
        int ne = op[0] == '!';
        if (a.kind == K_BITS && b.kind == K_BITS) {
            if (bt != BT_DYN) return val_c(K_BOOL, BT_STATIC, "(%ssg_s_eq(%s, %s))", ne ? "!" : "", a.c, b.c);
            {
                Val e = val_c(K_BOOL, BT_DYN, "sg_d_eq(g, %s, %s)", raise(a, BT_DYN).c, raise(b, BT_DYN).c);
                return ne ? val_c(K_BOOL, BT_DYN, "LLVMBuildNot(SG_B, %s, \"\")", e.c) : e;
            }
        }
        if (a.kind == K_BITS || b.kind == K_BITS) sg_fail(n, "%s of bits and other than bits", op);
        if (bt == BT_CONST) return val_const(K_BOOL, (a.value == b.value) != ne);
        if (bt == BT_STATIC) return val_c(K_BOOL, BT_STATIC, "((%s) %s (%s))", a.c, op, b.c);
        if (a.kind == K_BOOL)
            return val_c(K_BOOL, BT_DYN, "LLVMBuildICmp(SG_B, %s, %s, %s, \"\")", ne ? "LLVMIntNE" : "LLVMIntEQ",
                         raise(a, BT_DYN).c, raise(b, BT_DYN).c);
        sg_fail(n, "%s of integers known only at run time", op);
    }
    if (!strcmp(op, "<") || !strcmp(op, "<=") || !strcmp(op, ">") || !strcmp(op, ">=")) {
        need_static_int(n, a, "a comparison's side");
        need_static_int(n, b, "a comparison's side");
        if (bt == BT_CONST) {
            int v = op[0] == '<' ? (op[1] ? a.value <= b.value : a.value < b.value)
                                 : (op[1] ? a.value >= b.value : a.value > b.value);
            return val_const(K_BOOL, v);
        }
        return val_c(K_BOOL, BT_STATIC, "((%s) %s (%s))", a.c, op, b.c);
    }
    if (!strcmp(op, "@")) {
        need_bits(n, a, "a concatenation's side");
        need_bits(n, b, "a concatenation's side");
        if (bt != BT_DYN) return val_c(K_BITS, BT_STATIC, "sg_s_concat(%s, %s)", a.c, b.c);
        return val_c(K_BITS, BT_DYN, "sg_d_concat(g, %s, %s)", raise(a, BT_DYN).c, raise(b, BT_DYN).c);
    }
    if (!strcmp(op, "&") || !strcmp(op, "|")) {
        const char* f = op[0] == '&' ? "and" : "or";
        need_bits(n, a, op);
        need_bits(n, b, op);
        if (bt != BT_DYN) return val_c(K_BITS, BT_STATIC, "sg_s_%s(%s, %s)", f, a.c, b.c);
        return val_c(K_BITS, BT_DYN, "sg_d_%s(g, %s, %s)", f, raise(a, BT_DYN).c, raise(b, BT_DYN).c);
    }
    if (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*")) {
        int add = op[0] == '+', mul = op[0] == '*';
        if (a.kind == K_BITS && b.kind == K_BITS && !mul) {
            if (bt != BT_DYN) return val_c(K_BITS, BT_STATIC, "sg_s_%s(%s, %s)", add ? "add" : "sub", a.c, b.c);
            return val_c(K_BITS, BT_DYN, "sg_d_%s(g, %s, %s)", add ? "add" : "sub", raise(a, BT_DYN).c,
                         raise(b, BT_DYN).c);
        }
        if (a.kind == K_BITS && b.kind == K_INT && !mul) { /* bits + integer: the integer's bits of that width */
            Val k = need_static_int(n, b, "the integer added to bits");
            if (a.bt != BT_DYN)
                return val_c(K_BITS, BT_STATIC, "sg_s_%s(%s, sg_s((SnxU64)(%s), (%s).w))", add ? "add" : "sub", a.c,
                             k.c, a.c);
            return val_c(K_BITS, BT_DYN, "sg_d_add_int(g, %s, %s(%s))", a.c, add ? "" : "-", k.c);
        }
        if (a.kind != K_INT || b.kind != K_INT) sg_fail(n, "%s of kinds %d and %d", op, a.kind, b.kind);
        if (bt == BT_CONST)
            return val_const(K_INT, add ? a.value + b.value : mul ? a.value * b.value : a.value - b.value);
        if (bt == BT_STATIC) return val_c(K_INT, BT_STATIC, "((%s) %s (%s))", a.c, op, b.c);
        return val_c(K_INT, BT_DYN, "sg_i_%s(g, %s, %s)",
                     add   ? "add"
                     : mul ? "mul"
                           : "sub",
                     raise(a, BT_DYN).c, raise(b, BT_DYN).c);
    }
    if (!strcmp(op, "<<") || !strcmp(op, "^")) { /* integers: a * 2^b, 2^b */
        need_static_int(n, a, op);
        need_static_int(n, b, op);
        if (op[0] == '^' && !(a.bt == BT_CONST && a.value == 2)) sg_fail(n, "a power of other than 2");
        if (bt == BT_CONST && b.value >= 0 && b.value < 62)
            return val_const(K_INT, op[0] == '^' ? 1ll << b.value : a.value * (1ll << b.value));
        if (bt == BT_CONST && b.value >= 62) { /* past a long long: C that names it does not compile */
            Val v = val_c(K_INT, BT_CONST, "sg_integer_over_62_bits");
            v.huge = 1;
            return v;
        }
        if (op[0] == '^') return val_c(K_INT, BT_STATIC, "sg_pow2(%s)", b.c);
        return val_c(K_INT, BT_STATIC, "((%s) * sg_pow2(%s))", a.c, b.c);
    }
    sg_fail(n, "the operator %s", op);
    return unit();
}

static Val slice(unsigned n) {
    unsigned reg = N(n).kind == N_INDEX ? register_def(N(n).a) : 0;
    Val x, hi, lo;
    if (reg) return element_read(n, reg);
    if (N(n).kind == N_SLICE && N(N(n).a).kind == N_INDEX && (reg = register_def(N(N(n).a).a)) != 0 &&
        reg_kind(reg) == K_VECTOR) { /* REG[i][hi .. lo]: only those bits, where the back end reads them alone */
        char fn[160];
        snprintf(fn, sizeof fn, "sg_%s_read_bits", reg_name(reg));
        if (backend_defines_function(fn)) {
            Val i = need_static_int(n, eval(N(N(n).a).b, 0), "a register's element number");
            hi = need_static_int(n, eval(N(n).b, 0), "a slice's bound");
            lo = need_static_int(n, eval(N(n).c, 0), "a slice's bound");
            return val_c(K_BITS, BT_DYN, "%s(g, %s, %s, %s)", fn, i.c, hi.c, lo.c);
        }
    }
    x = eval(N(n).a, 0);
    hi = need_static_int(n, eval(N(n).b, 0), "a slice's bound");
    lo = N(n).kind == N_SLICE ? need_static_int(n, eval(N(n).c, 0), "a slice's bound") : hi;
    if (x.bt == BT_LANE) { /* every lane's bits hi..lo */
        if (x.kind != K_INT && x.kind != K_BITS) {
            lane_refuse("a slice of lanes of other than bits or integers");
            return x;
        }
        return val_c(K_BITS, BT_LANE, "sg_l_slice(g, %s, %s, %s)", x.c, hi.c, lo.c);
    }
    if (x.kind == K_INT && x.bt != BT_DYN && hi.bt == BT_CONST && lo.bt == BT_CONST && hi.value - lo.value >= 64)
        return val_c(K_BITS, BT_DYN, "sg_d_of_int(%s, %s, %s)", x.c, hi.c, lo.c); /* 0[127 .. 0]: constant bits */
    if (x.kind == K_INT) {
        if (x.bt == BT_DYN) return val_c(K_BITS, BT_DYN, "sg_i_slice(g, %s, %s, %s)", x.c, hi.c, lo.c);
        return val_c(K_BITS, BT_STATIC, "sg_s_of_int(%s, %s, %s)", x.c, hi.c, lo.c);
    }
    need_bits(n, x, "what is sliced");
    if (x.bt == BT_DYN) return val_c(K_BITS, BT_DYN, "sg_d_slice(g, %s, %s, %s)", x.c, hi.c, lo.c);
    return val_c(K_BITS, BT_STATIC, "sg_s_slice(%s, %s, %s)", x.c, hi.c, lo.c);
}

/* [a, b, ...]: the elements' bits, the first the most significant */
static Val vector(unsigned n) {
    unsigned e = N(n).a;
    Val r;
    if (!e) sg_fail(n, "an empty vector");
    r = need_bits(e, eval(e, 0), "a vector's element");
    for (e = N(e).next; e; e = N(e).next) {
        Val x = need_bits(e, eval(e, 0), "a vector's element");
        if (r.bt != BT_DYN && x.bt != BT_DYN)
            r = val_c(K_BITS, BT_STATIC, "sg_s_concat(%s, %s)", r.c, x.c);
        else
            r = val_c(K_BITS, BT_DYN, "sg_d_concat(g, %s, %s)", raise(r, BT_DYN).c, raise(x, BT_DYN).c);
    }
    return r;
}

/* ---- statements ---- */

static Val type_width(unsigned type) {
    if (!type || N(type).value != K_BITS || !N(type).a) sg_fail(type, "a bits type with no width");
    return need_static_int(type, eval(N(type).a, 0), "a width");
}
static int type_kind(unsigned type) { return type ? (int)N(type).value : K_NONE; }

/* the value undefined takes in a local of this type and time: zero */
static Val undefined_of(unsigned type, int bt) {
    int kind = type_kind(type);
    unsigned st = type_struct(type);
    Val w;
    if (bt == BT_LANE && (kind == K_BITS || kind == K_BIT || kind == K_INT || kind == K_BOOL))
        return val_c(kind == K_BIT ? K_BITS : kind, BT_LANE, "((SgL){0})"); /* every lane is set before it is read */
    if (st) {
        w = val_c(K_STRUCT, BT_STATIC, "((%s){0})", struct_ctype(st));
        w.sdef = st;
        return w;
    }
    switch (kind) {
        case K_BITS:
            w = type_width(type);
            return bt == BT_DYN ? val_c(K_BITS, BT_DYN, "sg_d_empty((unsigned)(%s))", w.c)
                                : val_c(K_BITS, BT_STATIC, "sg_s_zeros(%s)", w.c);
        case K_BIT:
            return bt == BT_DYN ? val_c(K_BITS, BT_DYN, "sg_d_from_s(sg_s(0, 1))")
                                : val_c(K_BITS, BT_STATIC, "sg_s(0, 1)");
        case K_INT: return bt == BT_DYN ? val_c(K_INT, BT_DYN, "sg_i_const(g, 0)") : val_c(K_INT, BT_STATIC, "0ll");
        case K_BOOL:
            return bt == BT_DYN ? val_c(K_BOOL, BT_DYN, "LLVMConstInt(g->ir->t_i1, 0, 0)")
                                : val_c(K_BOOL, BT_STATIC, "0");
        case K_ENUM: return val_c(K_ENUM, BT_STATIC, "0");
        default: sg_fail(type, "undefined of a type sailgen does not keep (%s)", sg_str(N(type).name));
    }
    return unit();
}

/* name : type = init, a mutable local, declared at the time its assignments reach */
static void declare_var(unsigned name, unsigned type, unsigned init_node) {
    Val init = N(init_node).kind == N_UNDEF ? unit() : eval(init_node, 0), local;
    unsigned sdef = init.kind != K_UNIT ? init.sdef : type_struct(type);
    int kind = init.kind != K_UNIT ? init.kind : sdef ? K_STRUCT : type_kind(type), var, bt;
    char cname[96];
    if (kind == K_BIT) kind = K_BITS;
    if (kind == K_NONE || kind == K_OTHER || kind == K_TUPLE)
        sg_fail(init_node, "a local of a type sailgen does not keep");
    var = new_var(kind, init.kind != K_UNIT ? init.bt : BT_STATIC);
    g_gen.vars[var].sdef = sdef;
    if (g_gen.dynamic_branch) var_raise(var, BT_DYN);
    bt = g_gen.vars[var].bt;
    snprintf(cname, sizeof cname, "v%d_%s", var, sg_str(name));
    if (bt == BT_LANE && !g_gen.lane && init.kind != K_UNIT) { /* a value set before the loop: not every lane's */
        refuse_loop(g_gen.vars[var].lane_loop);
        init = unit();
    }
    init = init.kind == K_UNIT ? undefined_of(type, bt) : raise(init, bt);
    emit("%s %s = %s;\n", ctype_of(kind, bt, sdef), cname, init.c);
    local = val_c(kind, bt, "%s", cname);
    local.sdef = sdef;
    bind(name, local, var, 0);
}

/* let pattern = value: the names bound to copies (the value may come from a local assigned later) */
static void let_bind(unsigned pattern, Val v, unsigned value_node) {
    unsigned p = pattern;
    if (N(p).kind == N_TYPED) p = N(p).a;
    if (N(p).kind == N_WILD) return;
    if (N(p).kind == N_ID) {
        v = keep(v);
        bind(N(p).name, v, -1, value_node);
        return;
    }
    if (N(p).kind == N_TUPLE) {
        unsigned q, k = 0;
        if (v.kind != K_TUPLE) sg_fail(pattern, "a tuple pattern for a value that is not one");
        for (q = N(p).a; q; q = N(q).next, ++k) {
            if (k >= v.ntuple) sg_fail(q, "more names than the tuple has values");
            let_bind(q, g_gen.pool[v.tuple[k]], 0);
        }
        return;
    }
    sg_fail(pattern, "a let pattern sailgen does not read");
}

static void assign(unsigned target, Val v) {
    unsigned t = target, reg;
    Binding* b;
    if (N(t).kind == N_TYPED) t = N(t).a;
    if (N(t).kind == N_WILD) return;
    if (g_gen.lane && (N(t).kind != N_ID || (b = lookup(name_of(t))) == NULL || b->var < 0)) {
        lane_refuse("state set in an element loop, which lanes set once");
        return;
    }
    if (N(t).kind == N_FIELD && N(N(t).a).kind == N_ID && (b = lookup(name_of(N(t).a))) != NULL) { /* x.f = v */
        Val f;
        if (b->var < 0 || b->val.kind != K_STRUCT)
            sg_fail(t, "the field %s assigned of other than a struct local", name_of(t));
        if (g_gen.dynamic_branch) sg_fail(t, "a struct's field assigned under a condition known only at run time");
        f = struct_field(t, b->val.sdef, b->val.c);
        if (v.bt == BT_DYN) sg_fail(t, "the field %s assigned a value known only at run time", name_of(t));
        if ((f.kind == K_BOOL) != (v.kind == K_BOOL) || (f.kind == K_BITS) != (v.kind == K_BITS) ||
            (f.kind == K_STRUCT && v.sdef != f.sdef))
            sg_fail(t, "the field %s (kind %d) assigned a value of kind %d", name_of(t), f.kind, v.kind);
        emit("%s = %s;\n", f.c, raise(v, BT_STATIC).c);
        return;
    }
    if (N(t).kind == N_FIELD) { /* PSTATE.N = v */
        fields_write(t, v);
        return;
    }
    if (N(t).kind == N_BINOP && !strcmp(name_of(t), "@")) { /* PSTATE.N @ PSTATE.Z @ ... = v */
        fields_write(t, v);
        return;
    }
    if (N(t).kind == N_TUPLE) {
        unsigned q, k = 0;
        if (v.kind != K_TUPLE) sg_fail(t, "a tuple assigned a value that is not one");
        for (q = N(t).a; q; q = N(q).next, ++k) assign(q, g_gen.pool[v.tuple[k]]);
        return;
    }
    if (N(t).kind == N_CALL) { /* a setter: f(args) = v is f(args, v) */
        const char* s = name_of(t);
        Val args[17];
        unsigned count = arg_count(t), i, def;
        if (g_gen.dynamic_branch) sg_fail(t, "%s() = under a condition known only at run time", s);
        def = function_def(s, count + 1);
        if (!def) sg_fail(t, "the setter %s has no definition of %u arguments", s, count + 1);
        for (i = 0; i < count; ++i) args[i] = keep(eval(arg_at(t, i), 0));
        args[count++] = keep(v);
        (void)inline_call(t, def, args, count);
        return;
    }
    if (N(t).kind == N_INDEX && (reg = register_def(N(t).a)) != 0) { /* GPR[n] = v */
        Val i;
        char fn[160];
        if (reg_kind(reg) != K_VECTOR)
            sg_fail(t, "%s[...] assigned: the register %s is not a vector", reg_name(reg), reg_name(reg));
        need_static_write(t, reg);
        i = need_static_int(t, eval(N(t).b, 0), "a register's element number");
        snprintf(fn, sizeof fn, "%s", reg_access(t, reg, "write"));
        v = raise(need_bits(t, v, "a register's value"), BT_DYN);
        emit("%s(g, %s, %s);\n", fn, i.c, v.c);
        return;
    }
    if (N(t).kind == N_SLICE || N(t).kind == N_INDEX) { /* x[hi .. lo] = v */
        unsigned x = N(t).a;
        Val hi, lo;
        b = NULL;
        if (N(x).kind != N_ID || !(b = lookup(name_of(x))) || b->var < 0)
            sg_fail(t, "a slice assigned of other than a local");
        hi = need_static_int(t, eval(N(t).b, 0), "a slice's bound");
        lo = N(t).kind == N_SLICE ? need_static_int(t, eval(N(t).c, 0), "a slice's bound") : hi;
        need_bits(t, v, "a slice's value");
        var_raise(b->var, v.bt);
        if (g_gen.dynamic_branch) var_raise(b->var, BT_DYN);
        if (g_gen.vars[b->var].bt == BT_DYN)
            emit("sg_d_set_slice(g, &%s, %s, %s, %s);\n", b->val.c, hi.c, lo.c, raise(v, BT_DYN).c);
        else
            emit("%s = sg_s_set_slice(%s, %s, %s, %s);\n", b->val.c, b->val.c, hi.c, lo.c, raise(v, BT_STATIC).c);
        return;
    }
    if (N(t).kind == N_ID) {
        const char* s = name_of(t);
        b = lookup(s);
        if (!b && (reg = register_def(t)) != 0) { /* a register of bits or a bool, whole */
            char fn[160];
            if (reg_kind(reg) != K_BITS && reg_kind(reg) != K_BOOL)
                sg_fail(t,
                        "the register %s assigned whole: sailgen writes a vector one by element and a struct one by "
                        "field",
                        reg_name(reg));
            need_static_write(t, reg);
            snprintf(fn, sizeof fn, "%s", reg_access(t, reg, "write"));
            if (reg_kind(reg) == K_BOOL) {
                if (v.kind != K_BOOL) sg_fail(t, "the bool register %s assigned other than a bool", reg_name(reg));
                v = raise(v, BT_DYN);
            } else
                v = raise(need_bits(t, v, "a register's value"), BT_DYN);
            emit("%s(g, %s);\n", fn, v.c);
            return;
        }
        if (!strcmp(s, "SEE")) return; /* the decoder's own bookkeeping */
        if (!b) sg_fail(t, "%s assigned but not declared", s);
        if (b->var < 0) sg_fail(t, "%s assigned but bound by let", s);
        var_raise(b->var, v.bt);
        if (g_gen.lane) { /* a local set in the loop holds a value per lane, or the loop is no vector operation */
            if (g_gen.vars[b->var].bt != BT_LANE) {
                lane_refuse("a local carried from one element to the next");
                return;
            }
            if (strncmp(v.c, "sg_l_bits(", 10)) g_gen.vars[b->var].lane_elem = 1;
        }
        if (g_gen.vars[b->var].bt == BT_LANE && !g_gen.lane) { /* the loop's lanes set again after it */
            refuse_loop(g_gen.vars[b->var].lane_loop);
            return;
        }
        if (g_gen.dynamic_branch) var_raise(b->var, BT_DYN);
        if (v.kind != K_UNIT && (v.kind != g_gen.vars[b->var].kind || v.sdef != g_gen.vars[b->var].sdef))
            sg_fail(t, "%s (kind %d) assigned a value of kind %d", s, g_gen.vars[b->var].kind, v.kind);
        emit("%s = %s;\n", b->val.c, raise(v, g_gen.vars[b->var].bt).c);
        return;
    }
    sg_fail(t, "an assignment to an expression of kind %d", N(t).kind);
}

/* the mutable locals a statement assigns (a DYN if's branches give them back as selects) */
static void assigned_vars(unsigned n, int* vars, unsigned* count) {
    unsigned q;
    if (!n) return;
    if (N(n).kind == N_ASSIGN) {
        unsigned t = N(n).a, list[8], k = 0, i;
        while (N(t).kind == N_SLICE || N(t).kind == N_INDEX || N(t).kind == N_TYPED) t = N(t).a;
        if (N(t).kind == N_TUPLE)
            for (q = N(t).a; q && k < 8; q = N(q).next) list[k++] = N(q).kind == N_TYPED ? N(q).a : q;
        else
            list[k++] = t;
        for (i = 0; i < k; ++i) {
            Binding* b = N(list[i]).kind == N_ID ? lookup(name_of(list[i])) : NULL;
            unsigned j;
            if (!b || b->var < 0) continue;
            for (j = 0; j < *count && vars[j] != b->var; ++j) {}
            if (j == *count) {
                if (*count == 32) sg_fail(n, "over 32 locals assigned under one run-time condition");
                vars[(*count)++] = b->var;
            }
        }
    }
    if (N(n).kind == N_BLOCK) {
        for (q = N(n).a; q; q = N(q).next) assigned_vars(q, vars, count);
        return;
    }
    if (N(n).kind == N_LET) { /* let ... in body */
        assigned_vars(N(n).b, vars, count);
        assigned_vars(N(n).c, vars, count);
        return;
    }
    assigned_vars(N(n).a, vars, count);
    assigned_vars(N(n).b, vars, count);
    assigned_vars(N(n).c, vars, count);
    assigned_vars(N(n).d, vars, count);
}

static const char* select_fn(int kind) {
    if (kind == K_BITS) return "sg_d_select";
    if (kind == K_INT) return "sg_i_select";
    return NULL;
}

/* if c then a else b, c known only at run time: each branch on copies of what they assign, then selects */
static Val dynamic_if(unsigned n, Val c, int tail) {
    int vars[32];
    unsigned count = 0, i, saved_depth;
    char names_t[32][16], names_e[32][16], cond[16];
    Val then_v, else_v = unit();
    temp(cond, sizeof cond);
    emit("LLVMValueRef %s = %s;\n", cond, c.c);
    assigned_vars(N(n).b, vars, &count);
    assigned_vars(N(n).c, vars, &count);
    for (i = 0; i < count; ++i) var_raise(vars[i], BT_DYN);
    for (i = 0; i < count; ++i) {
        Binding* b = var_binding(vars[i]);
        temp(names_t[i], sizeof names_t[i]);
        temp(names_e[i], sizeof names_e[i]);
        emit("%s %s = %s, %s = %s;\n", ctype(g_gen.vars[vars[i]].kind, BT_DYN), names_t[i], raise(b->val, BT_DYN).c,
             names_e[i], raise(b->val, BT_DYN).c);
    }
    ++g_gen.dynamic_branch;
    saved_depth = g_gen.depth;
    for (i = 0; i < count; ++i) {
        Binding copy = *var_binding(vars[i]);
        snprintf(copy.val.c, sizeof copy.val.c, "%s", names_t[i]);
        bind(copy.name, copy.val, copy.var, 0);
    }
    {
        char saved[sizeof g_gen.path];
        int n_path;
        snprintf(saved, sizeof saved, "%s", g_gen.path);
        n_path = saved[0] ? snprintf(g_gen.path, sizeof g_gen.path, "LLVMBuildAnd(SG_B, %s, %s, \"\")", saved, cond)
                          : snprintf(g_gen.path, sizeof g_gen.path, "%s", cond);
        if (n_path < 0 || (size_t)n_path >= sizeof g_gen.path) sg_fail(n, "run-time conditions nested too deep");
        then_v = eval(N(n).b, tail);
        if (then_v.kind != K_UNIT) then_v = keep(raise(then_v, BT_DYN));
        n_path = saved[0] ? snprintf(g_gen.path, sizeof g_gen.path,
                                     "LLVMBuildAnd(SG_B, %s, LLVMBuildNot(SG_B, %s, \"\"), \"\")", saved, cond)
                          : snprintf(g_gen.path, sizeof g_gen.path, "LLVMBuildNot(SG_B, %s, \"\")", cond);
        if (n_path < 0 || (size_t)n_path >= sizeof g_gen.path) sg_fail(n, "run-time conditions nested too deep");
        g_gen.depth = saved_depth;
        for (i = 0; i < count; ++i) {
            Binding copy = *var_binding(vars[i]);
            snprintf(copy.val.c, sizeof copy.val.c, "%s", names_e[i]);
            bind(copy.name, copy.val, copy.var, 0);
        }
        if (N(n).c) {
            else_v = eval(N(n).c, tail);
            if (else_v.kind != K_UNIT) else_v = keep(raise(else_v, BT_DYN));
        }
        snprintf(g_gen.path, sizeof g_gen.path, "%s", saved);
    }
    g_gen.depth = saved_depth;
    --g_gen.dynamic_branch;
    for (i = 0; i < count; ++i) {
        int kind = g_gen.vars[vars[i]].kind;
        Binding* b = var_binding(vars[i]);
        if (kind == K_BOOL)
            emit("%s = LLVMBuildSelect(SG_B, %s, %s, %s, \"\");\n", b->val.c, cond, names_t[i], names_e[i]);
        else if (select_fn(kind))
            emit("%s = %s(g, %s, %s, %s);\n", b->val.c, select_fn(kind), cond, names_t[i], names_e[i]);
        else
            sg_fail(n, "a local of kind %d assigned under a condition known only at run time", kind);
    }
    if (then_v.kind == K_UNIT) return unit();
    if (then_v.kind == K_BOOL)
        return keep(val_c(K_BOOL, BT_DYN, "LLVMBuildSelect(SG_B, %s, %s, %s, \"\")", cond, then_v.c, else_v.c));
    if (!select_fn(then_v.kind)) sg_fail(n, "an if's value of kind %d under a run-time condition", then_v.kind);
    return keep(val_c(then_v.kind, BT_DYN, "%s(g, %s, %s, %s)", select_fn(then_v.kind), cond, then_v.c, else_v.c));
}

/* walks n into a buffer that is dropped: what it gives (an if's or a match's branch, to declare the result first) */
static Val probe(unsigned n, int tail) {
    Buffer scratch = {0, 0, 0}, *saved = g_gen.out;
    unsigned saved_depth = g_gen.depth, saved_var = g_gen.var_next, saved_temp = g_gen.temp;
    int saved_indent = g_gen.indent;
    Val v;
    g_gen.out = &scratch;
    v = eval(n, tail);
    g_gen.out = saved;
    g_gen.depth = saved_depth;
    g_gen.var_next = saved_var;
    g_gen.temp = saved_temp;
    g_gen.indent = saved_indent;
    free(scratch.text);
    return v;
}

static Val if_expr(unsigned n, int tail) {
    Val c = eval(N(n).a, 0), a, b, r;
    char t[16], buffer[sizeof c.c];
    if (c.kind != K_BOOL) sg_fail(N(n).a, "an if's condition is not a bool");
    if (c.bt == BT_CONST) {
        if (c.value) return eval(N(n).b, tail);
        return N(n).c ? eval(N(n).c, tail) : unit();
    }
    if (c.bt == BT_DYN) return dynamic_if(n, c, tail);
    if (c.bt == BT_LANE) { /* a value per lane: each lane's of one branch or the other */
        a = N(n).c ? eval(N(n).b, tail) : unit();
        b = N(n).c ? eval(N(n).c, tail) : unit();
        if (a.kind != b.kind || (a.kind != K_BITS && a.kind != K_INT && a.kind != K_BOOL)) {
            lane_refuse("an if per lane that is not a value of both branches");
            return a.kind == K_UNIT ? unit() : a;
        }
        return val_c(a.kind, BT_LANE, "sg_l_select(g, %s, %s, %s)", c.c, raise(a, BT_LANE).c, raise(b, BT_LANE).c);
    }
    a = probe(N(n).b, tail);
    b = N(n).c ? probe(N(n).c, tail) : unit();
    if (a.kind == K_UNIT || b.kind == K_UNIT || a.kind == K_TUPLE) {
        emit("if (%s) {\n", cond_text(c.c, buffer, sizeof buffer));
        ++g_gen.indent;
        stmt(N(n).b);
        --g_gen.indent;
        if (N(n).c) {
            emit("} else {\n");
            ++g_gen.indent;
            stmt(N(n).c);
            --g_gen.indent;
        }
        emit("}\n");
        return unit();
    }
    r = val_c(a.kind, top_bt(top_bt(a.bt, b.bt), BT_STATIC), "");
    r.sdef = a.sdef;
    temp(t, sizeof t);
    snprintf(r.c, sizeof r.c, "%s", t);
    emit("%s %s;\n", ctype_of(r.kind, r.bt, r.sdef), t);
    emit("if (%s) {\n", cond_text(c.c, buffer, sizeof buffer));
    ++g_gen.indent;
    a = eval(N(n).b, tail);
    emit("%s = %s;\n", t, raise(a, r.bt).c);
    --g_gen.indent;
    emit("} else {\n");
    ++g_gen.indent;
    b = eval(N(n).c, tail);
    emit("%s = %s;\n", t, raise(b, r.bt).c);
    --g_gen.indent;
    emit("}\n");
    return r;
}

/* a case's pattern against the scrutinee, as a C condition: 0 for one that always matches */
static int pattern_test(unsigned p, Val s, char* out, size_t size) {
    if (N(p).kind == N_WILD) return 0;
    if (N(p).kind == N_BITS) {
        snprintf(out, size, "(%s).v == 0x%llxull", s.c, N(p).value);
        return 1;
    }
    if (N(p).kind == N_NUM) {
        snprintf(out, size, "(%s) == %lldll", s.c, (long long)N(p).value);
        return 1;
    }
    if (N(p).kind == N_VEC) { /* [bitzero, bitone, _, ...]: the bits it fixes, the first the top */
        unsigned e, count = 0, i = 0;
        SgU64 mask = 0, value = 0;
        for (e = N(p).a; e; e = N(e).next) ++count;
        if (count > 64) sg_fail(p, "a bit pattern of over 64 bits");
        for (e = N(p).a; e; e = N(e).next, ++i) {
            SgU64 bit = 1ull << (count - 1 - i);
            if (N(e).kind == N_WILD) continue;
            if (N(e).kind != N_ID || (strcmp(name_of(e), "bitone") && strcmp(name_of(e), "bitzero")))
                sg_fail(e, "a bit pattern's bit that is not bitzero, bitone or _");
            mask |= bit;
            if (!strcmp(name_of(e), "bitone")) value |= bit;
        }
        if (!mask) return 0;
        snprintf(out, size, "((%s).v & 0x%llxull) == 0x%llxull", s.c, (unsigned long long)mask,
                 (unsigned long long)value);
        return 1;
    }
    if (N(p).kind == N_ID) {
        long long m = enum_member(name_of(p));
        if (m < 0) sg_fail(p, "a pattern binding a name (%s): sailgen reads literals, enum members and _", name_of(p));
        snprintf(out, size, "(%s) == %lld", s.c, m);
        return 1;
    }
    sg_fail(p, "a pattern sailgen does not read");
    return 0;
}

static Val match_expr(unsigned n, int tail) {
    Val s = eval(N(n).a, 0), r = unit(), first;
    unsigned c;
    int valued, opened = 0, total = 0;
    char t[16];
    if (s.bt == BT_LANE) {
        lane_refuse("a match per lane");
        return unit();
    }
    if (s.bt == BT_DYN) sg_fail(n, "a match on a value known only at run time");
    s = keep(raise(s, BT_STATIC));
    for (c = N(n).b; c; c = N(c).next)
        if (N(c).c) sg_fail(c, "a guarded case");
    first = probe(N(N(n).b).b, tail);
    valued = first.kind != K_UNIT && first.kind != K_TUPLE;
    if (valued) {
        int bt = BT_STATIC;
        for (c = N(n).b; c; c = N(c).next) bt = top_bt(bt, probe(N(c).b, tail).bt);
        r = val_c(first.kind, bt, "");
        r.sdef = first.sdef;
        temp(t, sizeof t);
        snprintf(r.c, sizeof r.c, "%s", t);
        emit("%s %s;\n", ctype_of(r.kind, r.bt, r.sdef), t);
    }
    for (c = N(n).b; c; c = N(c).next) {
        char test[192];
        int conditional = pattern_test(N(c).a, s, test, sizeof test);
        if (!opened)
            emit(conditional ? "if (%s) {\n" : "{\n", test);
        else
            emit(conditional ? "} else if (%s) {\n" : "} else {\n", test);
        opened = 1;
        ++g_gen.indent;
        if (valued) {
            Val v = eval(N(c).b, tail);
            emit("%s = %s;\n", t, raise(v, r.bt).c);
        } else
            (void)eval(N(c).b, tail); /* a case at the function's end may return() */
        --g_gen.indent;
        if (!conditional) {
            total = 1;
            break;
        }
    }
    if (opened) {
        if (!total) { /* every value listed, or the decode makes the rest impossible: reached, it is a bug */
            emit("} else {\n");
            emit("    sg_fail(\"a match with no case for its value at %s:%u\", 0, 0);\n", sg_str(N(n).file), N(n).line);
        }
        emit("}\n");
    }
    return r;
}

static Val block(unsigned n, int tail) {
    unsigned item, saved = g_gen.depth;
    Val v = unit();
    for (item = N(n).a; item; item = N(item).next) {
        int last = !N(item).next;
        if (N(item).kind == N_LET && !N(item).c) {
            Val value = eval(N(item).b, 0);
            let_bind(N(item).a, value, N(item).b);
            v = unit();
            continue;
        }
        v = eval(item, last && tail);
    }
    /* the block's names go; its value is a temporary, which stays in scope (the block has no braces) */
    g_gen.depth = saved;
    return v;
}

static Val foreach_loop(unsigned n) {
    Val from = need_static_int(n, eval(N(n).a, 0), "a foreach's start");
    Val to = need_static_int(n, eval(N(n).b, 0), "a foreach's end");
    Val by = N(n).c ? need_static_int(n, eval(N(n).c, 0), "a foreach's step") : val_const(K_INT, 1);
    char v[96];
    unsigned saved = g_gen.depth;
    if (g_gen.dynamic_branch) sg_fail(n, "a foreach under a run-time condition");
    if (g_gen.lane) {
        lane_refuse("a loop inside an element loop");
        return unit();
    }
    /* from 0 by 1 up: tried as one vector operation, the lanes from 0 to `to` (lanes, above) */
    if (!g_gen.lane && !g_gen.in_helper && from.bt == BT_CONST && !from.value && by.bt == BT_CONST && by.value == 1 &&
        !N(n).dec && !lane_refused(n)) {
        Buffer scratch = {0, 0, 0}, *out = g_gen.out;
        int indent = g_gen.indent;
        g_gen.lane = 1;
        g_gen.lane_fail = 0;
        g_gen.lane_loop = n;
        snprintf(g_gen.lane_count, sizeof g_gen.lane_count, "(unsigned)((%s) + 1)", to.c);
        g_gen.out = &scratch;
        g_gen.indent = indent + 1;
        bind(N(n).name, val_c(K_INT, BT_STATIC, "sg_lane"), -1, 0);
        stmt(N(n).d);
        g_gen.depth = saved;
        g_gen.lane = 0;
        g_gen.out = out;
        g_gen.indent = indent;
        if (!g_gen.lane_fail) {
            emit("{ /* the lanes of %s */\n", name_of(n));
            buf_add(g_gen.out, scratch.text ? scratch.text : "", scratch.length);
            emit("}\n");
            free(scratch.text);
            return unit();
        }
        free(scratch.text);
        refuse_loop(n);
        return unit();
    }
    snprintf(v, sizeof v, "l%u_%s", ++g_gen.temp, name_of(n));
    emit("for (long long %s = %s; %s %s %s; %s %s= %s) {\n", v, from.c, v, N(n).dec ? ">=" : "<=", to.c, v,
         N(n).dec ? "-" : "+", by.c);
    ++g_gen.indent;
    bind(N(n).name, val_c(K_INT, BT_STATIC, "%s", v), -1, 0);
    stmt(N(n).d);
    g_gen.depth = saved;
    --g_gen.indent;
    emit("}\n");
    return unit();
}

static Val eval(unsigned n, int tail) {
    Val v;
    if (!n) return unit();
    switch (N(n).kind) {
        case N_NUM: return val_const(K_INT, (long long)N(n).value);
        case N_BITS:
            v = val_c(K_BITS, BT_STATIC, "sg_s(0x%llxull, %u)", N(n).value, N(n).width);
            v.node = n;
            return v;
        case N_BOOL: return val_const(K_BOOL, (long long)N(n).value);
        case N_UNIT: return unit();
        case N_ID: return ident(n);
        case N_CALL: v = call(n); break;
        case N_BINOP: v = binop(n); break;
        case N_NEG: {
            Val a = need_static_int(n, eval(N(n).a, 0), "a negation");
            if (a.bt == BT_CONST) return val_const(K_INT, -a.value);
            return val_c(K_INT, a.bt, "(-(%s))", a.c);
        }
        case N_SLICE:
        case N_INDEX: v = slice(n); break;
        case N_VEC: v = vector(n); break;
        case N_FIELD: return field_read(n);
        case N_IF: return if_expr(n, tail);
        case N_MATCH: return match_expr(n, tail);
        case N_BLOCK: return block(n, tail);
        case N_TYPED: /* undefined : T is T's zero, known when it recompiles */
            if (N(N(n).a).kind == N_UNDEF) return undefined_of(N(n).b, BT_STATIC);
            return eval(N(n).a, tail);
        case N_TUPLE: {
            Val parts[4];
            unsigned q, k = 0;
            for (q = N(n).a; q; q = N(q).next) {
                if (k == 4) sg_fail(n, "a tuple of more than 4");
                parts[k++] = keep(eval(q, 0));
            }
            return tuple_of(parts, k, n);
        }
        case N_LET: { /* let p = e in body */
            unsigned saved = g_gen.depth;
            Val value = eval(N(n).b, 0);
            let_bind(N(n).a, value, N(n).b);
            v = eval(N(n).c, tail);
            g_gen.depth = saved;
            return v;
        }
        case N_VAR: declare_var(N(n).name, N(n).c, N(n).b); return unit();
        case N_ASSIGN: assign(N(n).a, eval(N(n).b, 0)); return unit();
        case N_FOREACH: return foreach_loop(n);
        case N_RETURN:
            if (g_gen.in_helper && !tail) {
                Val r = eval(N(n).a, 0);
                unsigned k;
                if (r.kind == K_TUPLE)
                    for (k = 0; k < r.ntuple; ++k)
                        emit("*out%u = %s;\n", k, raise(g_gen.pool[r.tuple[k]], BT_STATIC).c);
                else if (r.kind != K_UNIT)
                    emit("*out0 = %s;\n", raise(r, BT_STATIC).c);
                emit("return 0;\n");
                return unit();
            }
            if (!tail) sg_fail(n, "a return before the end of an inlined function");
            return eval(N(n).a, 1);
        case N_THROW:
            /* Error_See: the word is another clause's, the next of those it matches (the hint space's PAuth hints).
               CONSTRAINED UNPREDICTABLE: the decoder routes no such word here; were one, UNDEF is one of its choices */
            if (is_call(N(n).a, "Error_See")) {
                end_decode(n, "a throw of Error_See", 1);
                return unit();
            }
            if (!is_call(N(n).a, "Error_Undefined") && !is_call(N(n).a, "Error_ConstrainedUnpredictable"))
                sg_fail(n, "a throw of other than Error_Undefined(), Error_See() or Error_ConstrainedUnpredictable()");
            throw_undefined(n);
            return unit();
        case N_ASSERT: return unit(); /* what the decode guarantees: nothing to generate */
        case N_UNDEF: sg_fail(n, "undefined other than as a local's start"); return unit();
        default: sg_fail(n, "an expression of kind %d sailgen does not read", N(n).kind); return unit();
    }
    if (!v.node) v.node = n;
    return v;
}

/* ---- the decode clauses and the output ---- */

/* the fixed bits of a clause's vector pattern [b31, ..., b0] of bitzero, bitone and _ */
static void clause_bits(unsigned def, SgU64* mask, SgU64* value) {
    unsigned p = g_parse.defs[def].params, e, i = 0;
    if (N(p).kind != N_TUPLE || !N(N(p).a).next) sg_fail(p, "a decode clause's pattern is not (pc, (... as __opcode))");
    p = N(N(p).a).next;
    if (N(p).kind == N_BINOP && !strcmp(name_of(p), "as")) p = N(p).a;
    if (N(p).kind == N_BITS) { /* every bit fixed */
        if (N(p).width != 32) sg_fail(p, "a decode pattern of %u bits", N(p).width);
        *mask = 0xffffffffull;
        *value = N(p).value;
        return;
    }
    if (N(p).kind != N_VEC) sg_fail(p, "a decode clause's pattern is not a bit vector");
    *mask = 0;
    *value = 0;
    for (e = N(p).a; e; e = N(e).next, ++i) {
        unsigned bit = 31 - i;
        if (i > 31) sg_fail(e, "a decode pattern over 32 bits");
        if (N(e).kind == N_WILD) continue;
        if (N(e).kind != N_ID || (strcmp(name_of(e), "bitone") && strcmp(name_of(e), "bitzero")))
            sg_fail(e, "a decode pattern bit that is not bitzero, bitone or _");
        *mask |= 1ull << bit;
        if (!strcmp(name_of(e), "bitone")) *value |= 1ull << bit;
    }
    if (i != 32) sg_fail(p, "a decode pattern of %u bits", i);
}

static void generate_decode(Clause* c) {
    unsigned body = g_parse.defs[c->clause].body;
    const char* decode_name = sg_str(g_parse.defs[c->decode].name);
    int pass;
    /* walked until no local's time changes; only the last walk's text is kept. The clause's body runs as it is:
       its lets slice __opcode (the word), its checks throw, its call is the decode function's */
    g_gen.var_count = 0;
    g_gen.refused_count = 0;
    g_gen.lane_restart = 0;
    for (pass = 0; pass < 8; ++pass) {
        Buffer scratch = {0, 0, 0};
        if (g_gen.lane_restart) { /* an element loop refused its lanes: its locals' times start over */
            g_gen.var_count = 0;
            g_gen.lane_restart = 0;
        }
        g_gen.changed = 0;
        g_gen.var_next = 0;
        g_gen.depth = 0;
        g_gen.frame = 0;
        g_gen.temp = 0;
        g_gen.call_depth = 0;
        g_gen.sees = 0;
        g_gen.dynamic_branch = 0;
        g_gen.path[0] = 0;
        g_gen.in_helper = 0;
        g_gen.out = &scratch;
        g_gen.indent = 1;
        bind(sg_intern("__opcode", 8), val_c(K_BITS, BT_STATIC, "sg_s(g->word, 32)"), -1, 0);
        bind(sg_intern("pc", 2), val_c(K_INT, BT_STATIC, "(long long)g->pc"), -1, 0);
        stmt(body);
        if (!g_gen.changed) {
            Buffer* out = &g_gen.body;
            g_gen.out = out;
            g_gen.indent = 0;
            emit("/* %s */\nstatic int sg_%s(SgCtx* g) {\n", decode_name, decode_name);
            buf_add(out, scratch.text ? scratch.text : "", scratch.length);
            emit("    return 1;\n}\n\n");
            c->sees = g_gen.sees;
            free(scratch.text);
            return;
        }
        free(scratch.text);
    }
    sg_fail(body, "the times of %s's locals did not settle in 8 walks", decode_name);
}

/* the root a clause's body calls, anywhere in it: its index in roots, or -1 */
static int clause_root(unsigned n, const unsigned* roots, unsigned count) {
    unsigned i;
    int r;
    if (!n) return -1;
    if (N(n).kind == N_CALL)
        for (i = 0; i < count; ++i)
            if (!strcmp(name_of(n), sg_str(roots[i]))) return (int)i;
    if ((r = clause_root(N(n).a, roots, count)) >= 0) return r;
    if ((r = clause_root(N(n).b, roots, count)) >= 0) return r;
    if ((r = clause_root(N(n).c, roots, count)) >= 0) return r;
    if ((r = clause_root(N(n).d, roots, count)) >= 0) return r;
    return clause_root(N(n).next, roots, count);
}

/* the roots file: root NAME (a decode function); stop lines are --extract's */
static unsigned read_roots(const char* path, unsigned* roots, unsigned cap) {
    size_t size, i;
    char* text = sg_read(path, &size);
    unsigned count = 0;
    for (i = 0; i < size;) {
        size_t e = i, w;
        while (e < size && text[e] != '\n') ++e;
        while (i < e && isspace((unsigned char)text[i])) ++i;
        if (i + 5 < e && !strncmp(text + i, "root", 4) && isspace((unsigned char)text[i + 4])) {
            i += 4;
            while (i < e && isspace((unsigned char)text[i])) ++i;
            w = 0;
            while (i + w < e && !isspace((unsigned char)text[i + w])) ++w;
            if (count == cap) sg_fail(0, "%s: more than %u roots", path, cap);
            roots[count++] = sg_intern(text + i, w);
        }
        i = e + 1;
    }
    free(text);
    return count;
}

/* the FEAT_ words of the profile's feature lines (a64.profile: "feature v8.0 FEAT_CRC32 ..."), " A B ", in capitals */
static char* profile_features(const char* path) {
    size_t size, i, at = 0;
    char* text = sg_read(path, &size);
    char* out = (char*)malloc(size + 2);
    if (!out) sg_fail(0, "out of memory for %s", path);
    out[at++] = ' ';
    for (i = 0; i < size;) {
        size_t e = i;
        while (e < size && text[e] != '\n') ++e;
        if (e - i > 8 && !strncmp(text + i, "feature", 7) && isspace((unsigned char)text[i + 7]))
            for (i += 7; i < e;) {
                size_t w;
                while (i < e && isspace((unsigned char)text[i])) ++i;
                for (w = 0; i + w < e && !isspace((unsigned char)text[i + w]) && text[i + w] != '#'; ++w) {}
                if (w > 5 && !strncmp(text + i, "FEAT_", 5)) {
                    for (size_t k = 0; k < w; ++k) out[at++] = (char)toupper((unsigned char)text[i + k]);
                    out[at++] = ' ';
                }
                if (i + w < e && text[i + w] == '#') break;
                i += w;
            }
        i = e + 1;
    }
    out[at] = 0;
    free(text);
    return out;
}

int sg_generate(const char* roots_path, const char* backend_path, const char* profile_path, const char* out_path) {
    unsigned* roots = g_gen.roots;
    Clause* clauses = g_gen.clauses;
    unsigned root_count = read_roots(roots_path, roots, 1024), clause_count = 0, i, k, d;
    size_t backend_size;
    char* backend = sg_read(backend_path, &backend_size);
    const char* base = strrchr(backend_path, '/');
    g_gen.backend_code = backend_code(backend, backend_size);
    g_gen.features = profile_path ? profile_features(profile_path) : NULL;
    g_gen.backend_name = base ? base + 1 : backend_path;
    for (d = 1; d < g_parse.def_count; ++d) { /* each root's clause: the one that calls it */
        const SgDef* def = &g_parse.defs[d];
        int root;
        if (def->kind != D_CLAUSE || (root = clause_root(def->body, roots, root_count)) < 0) continue;
        {
            Clause* c = &clauses[clause_count++];
            c->clause = d;
            c->decode = sg_find(sg_str(roots[root]), D_FUNCTION);
            if (!c->decode) sg_fail(def->body, "the root %s has no definition", sg_str(roots[root]));
            clause_bits(d, &c->mask, &c->value);
        }
    }
    for (i = 0; i < root_count; ++i) {
        for (k = 0; k < clause_count && strcmp(sg_str(g_parse.defs[clauses[k].decode].name), sg_str(roots[i])); ++k) {}
        if (k == clause_count) sg_fail(0, "%s: no decode clause calls the root %s", roots_path, sg_str(roots[i]));
    }
    /* the clauses are tried in their order, as Sail's scattered decode tries them, each after the one that handed the
       word on (Error_See): one an earlier clause covers whole and hands nothing on is never reached (REV16 and REV32
       go to REV's clause, which runs the same execute) */
    for (k = 0; k < clause_count; ++k) {
        for (i = 0; i < k && !clauses[k].shadowed; ++i)
            if (!clauses[i].shadowed && !clauses[i].sees && (clauses[i].mask & clauses[k].mask) == clauses[i].mask &&
                (clauses[k].value & clauses[i].mask) == clauses[i].value)
                clauses[k].shadowed = i + 1;
        if (!clauses[k].shadowed) generate_decode(&clauses[k]);
    }
    sg_out("/* Generated by tools/sailgen (--generate %s) from", roots_path);
    for (i = 0; i < g_parse.file_count; ++i) sg_out(" %s", g_parse.files[i].path);
    sg_out(": do not edit */\n\n");
    sg_out("%.*s\n", (int)backend_size, backend);
    free(backend);
    free(g_gen.backend_code);
    g_gen.backend_code = NULL;
    free(g_gen.features);
    g_gen.features = NULL;
    if (g_gen.types.length)
        sg_out("/* ---- the Sail's structs, as values the recompiler knows ---- */\n\n%s", g_gen.types.text);
    sg_out("/* ---- helpers: functions of what the recompiler knows ---- */\n\n");
    if (g_gen.helpers.length) sg_out("%s", g_gen.helpers.text);
    sg_out("/* ---- the decode functions ---- */\n\n");
    if (g_gen.body.length) sg_out("%s", g_gen.body.text);
    sg_out(
        "/* the word's instruction emitted at pc, when it is one of these: 1, else 0. Each decode function answers 1 when\n"
        "   the word is its (emitted, or UNDEF), 0 when it hands it on to the next clause that matches it. A branch's\n"
        "   condition (an i1) and target (an i64) in *taken and *target when they are not NULL (sg_branch_to) */\n");
    sg_out("static int sg_emit(SnxIr* ir, SnxU32 word, SnxU64 pc, LLVMValueRef* taken, LLVMValueRef* target) {\n");
    sg_out("    SgCtx context, *g = &context;\n");
    sg_out("    memset(g, 0, sizeof *g);\n    g->ir = ir;\n    g->word = word;\n    g->pc = pc;\n");
    sg_out("    g->start = LLVMGetLastInstruction(LLVMGetInsertBlock(ir->b));\n   ");
    for (i = 0; i < clause_count; ++i) {
        if (clauses[i].shadowed) {
            sg_out(" /* %s: its words are %s's */\n   ", sg_str(g_parse.defs[clauses[i].decode].name),
                   sg_str(g_parse.defs[clauses[clauses[i].shadowed - 1].decode].name));
            continue;
        }
        sg_out(" if ((word & 0x%08llxu) == 0x%08llxu && sg_%s(g)) {\n    } else", clauses[i].mask, clauses[i].value,
               sg_str(g_parse.defs[clauses[i].decode].name));
    }
    sg_out(
        "\n        return 0;\n    sg_accesses_combine(g);\n    sg_flags_flush(g);\n    sg_branch_result(g, taken, target);\n"
        "    return 1;\n}\n");
    return sg_flush(out_path);
}
