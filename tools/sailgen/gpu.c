/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --gpu: the shader translators' instruction emitters from Sail written for a GPU's ISA (src/nx/gpu/shader/
   sail/), apart from the CPUs' generator (gen.c), with which it shares only the parse. tools/sailgen/README.md's "GPU
   mode" describes the Sail it reads, the roots file and the code it writes.

   Each root is a Sail function of no parameters that one or more of the decoder's opcodes run. It is walked with
   every function it calls inlined, and each value it makes is known when the translator runs (STATIC: the
   instruction's fields and what only they make, C here) or when the program runs (DYN: a value of the back end, SgV,
   that the back end's functions make). The output is C: one function per root, one per fusion and its guard, and the
   dispatchers. */
#include "sailgen.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NODE(i) (g_parse.nodes[i])

enum { TS, TD }; /* STATIC, DYN */
/* VK_OPAQUE: a run-time value of a type the Sail does not compute with (a vector, a pair of halves), which the back
   end makes and takes: it is bound, passed and chosen, nothing else */
enum { VK_UNIT, VK_BITS, VK_BOOL, VK_INT, VK_ENUM, VK_STR, VK_OPAQUE };

/* a value while the walk makes it: STATIC values are C expressions of uint64_t (bits), int64_t (int), int (bool or
   an enum's member); DYN ones the name of the C variable that holds their SgV. A DYN int is UInt or SInt of bits:
   their name and width, and sign 1 for SInt */
typedef struct {
    int time, kind, width, sign;
    const char* c;
} Val;

/* ---- strings and the output buffers ---- */

/* the strings fmt made: the walk keeps them in its values to the end, which frees them together (fmt_free) */
static struct {
    char** p;
    size_t n, cap;
} g_fmt;

static char* fmt(const char* f, ...) {
    va_list ap;
    int n;
    char* s;
    va_start(ap, f);
    n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    s = (char*)sg_alloc((size_t)n + 1);
    va_start(ap, f);
    vsnprintf(s, (size_t)n + 1, f, ap);
    va_end(ap);
    if (g_fmt.n == g_fmt.cap) {
        g_fmt.cap = g_fmt.cap ? 2 * g_fmt.cap : 1024;
        g_fmt.p = (char**)realloc(g_fmt.p, g_fmt.cap * sizeof *g_fmt.p);
        if (!g_fmt.p) {
            fprintf(stderr, "sailgen: out of memory for %zu strings\n", g_fmt.cap);
            exit(1);
        }
    }
    g_fmt.p[g_fmt.n++] = s;
    return s;
}
static void fmt_free(void) {
    for (size_t i = 0; i < g_fmt.n; ++i) free(g_fmt.p[i]);
    free(g_fmt.p);
    g_fmt.p = NULL;
    g_fmt.n = g_fmt.cap = 0;
}

typedef struct {
    char* text;
    size_t length, cap;
} Buf;

static Buf* g_buf; /* where statements go */
static int g_indent = 1;

static void buf_add(Buf* b, const char* s) {
    size_t n = strlen(s);
    if (b->length + n + 1 > b->cap) {
        b->cap = (b->length + n + 1) * 2;
        b->text = (char*)realloc(b->text, b->cap);
        if (!b->text) {
            fprintf(stderr, "sailgen: out of memory for the output\n");
            exit(1);
        }
    }
    memcpy(b->text + b->length, s, n + 1);
    b->length += n;
}

/* a statement at the walk's indent */
static void emit(const char* f, ...) {
    va_list ap;
    int n, i;
    char* s;
    va_start(ap, f);
    n = vsnprintf(NULL, 0, f, ap);
    va_end(ap);
    s = (char*)sg_alloc((size_t)n + 1);
    va_start(ap, f);
    vsnprintf(s, (size_t)n + 1, f, ap);
    va_end(ap);
    for (i = 0; i < g_indent; ++i) buf_add(g_buf, "    ");
    buf_add(g_buf, s);
    buf_add(g_buf, "\n");
    free(s);
}

static unsigned g_temp;
static const char* temp(const char* prefix) { return fmt("%s%u", prefix, ++g_temp); }

/* ---- the roots file ---- */

#define MAX_ROOTS 512
#define MAX_OPS   32

typedef struct {
    char name[96];
    char ops[MAX_OPS][64];
    int nops;
    int fuse; /* a fusion: ops is its sequence of opcodes */
} Root;

static struct {
    char prefix[32];
    Root roots[MAX_ROOTS];
    int nroots;
    char statics[128][64]; /* the back end's functions whose values are STATIC */
    int nstatics;
} g_roots;

static int is_static_fn(const char* name) {
    int i;
    for (i = 0; i < g_roots.nstatics; ++i)
        if (!strcmp(g_roots.statics[i], name)) return 1;
    return 0;
}

static void read_roots(const char* path) {
    size_t size;
    char* text = sg_read(path, &size);
    static int depth;
    char* line = text;
    unsigned number = 0;
    if (!depth && !g_roots.prefix[0]) strcpy(g_roots.prefix, "sg");
    while (line && *line) {
        char* next = strchr(line, '\n');
        char* words[MAX_OPS + 2];
        int count = 0;
        char* p;
        if (next) *next++ = 0;
        ++number;
        p = strchr(line, '#');
        if (p) *p = 0;
        for (p = strtok(line, " \t\r"); p && count < MAX_OPS + 2; p = strtok(NULL, " \t\r")) words[count++] = p;
        line = next;
        if (!count) continue;
        if (!strcmp(words[0], "include") && count == 2) { /* another roots file, beside this one */
            const char* slash = strrchr(path, '/');
            char* other = slash ? fmt("%.*s/%s", (int)(slash - path), path, words[1]) : fmt("%s", words[1]);
            if (++depth > 8) sg_fail(0, "%s:%u: roots included 8 deep", path, number);
            read_roots(other);
            --depth;
        } else if (!strcmp(words[0], "prefix") && count == 2) {
            snprintf(g_roots.prefix, sizeof g_roots.prefix, "%s", words[1]);
        } else if (!strcmp(words[0], "static")) {
            int i;
            for (i = 1; i < count; ++i) {
                if (g_roots.nstatics == 128) sg_fail(0, "%s:%u: more than 128 static functions", path, number);
                snprintf(g_roots.statics[g_roots.nstatics++], 64, "%s", words[i]);
            }
        } else if ((!strcmp(words[0], "op") || !strcmp(words[0], "fuse")) && count >= 3) {
            Root* r;
            int i;
            if (g_roots.nroots == MAX_ROOTS) sg_fail(0, "%s:%u: more than %d roots", path, number, MAX_ROOTS);
            r = &g_roots.roots[g_roots.nroots++];
            snprintf(r->name, sizeof r->name, "%s", words[1]);
            r->fuse = words[0][0] == 'f';
            for (i = 2; i < count; ++i) snprintf(r->ops[r->nops++], 64, "%s", words[i]);
            if (r->fuse && (r->nops < 2 || r->nops > 4))
                sg_fail(0, "%s:%u: a fusion takes 2 to 4 instructions", path, number);
        } else
            sg_fail(0, "%s:%u: expected \"prefix NAME\", \"static FN...\", \"op FN OPCODE...\" or \"fuse FN OPCODE...\"",
                    path, number);
    }
    free(text);
}

/* ---- the environment: let, var and parameters, innermost last ---- */

typedef struct {
    unsigned name;
    Val v;
    int mutable;
} Bind;

static Bind g_env[1024];
static int g_nenv;

static void bind(unsigned name, Val v, int mutable) {
    if (g_nenv == 1024) sg_fail(0, "more than 1024 names bound");
    g_env[g_nenv].name = name;
    g_env[g_nenv].v = v;
    g_env[g_nenv].mutable = mutable;
    ++g_nenv;
}
static int lookup(unsigned name) {
    int i;
    for (i = g_nenv - 1; i >= 0; --i)
        if (!strcmp(sg_str(g_env[i].name), sg_str(name))) return i;
    return -1;
}

/* ---- values ---- */

static Val v_static(int kind, int width, const char* c) {
    Val v = {TS, kind, width, 0, c};
    return v;
}
static Val v_unit(void) { return v_static(VK_UNIT, 0, "0"); }

static const char* kind_name(int k) {
    static const char* const names[] = {"unit", "bits", "bool", "int", "enum", "string", "opaque value"};
    return names[k];
}

/* a STATIC bits or bool value as DYN: the back end's constant */
static Val dyn(unsigned at, Val v) {
    Val r;
    if (v.time == TD) return v;
    r = v;
    r.time = TD;
    r.c = temp("v");
    if (v.kind == VK_BITS) {
        if (!v.width) sg_fail(at, "bits of a width sailgen doesn't know made a run-time value");
        emit("SgV %s = sg_const(g, %d, %s);", r.c, v.width, v.c);
    } else if (v.kind == VK_BOOL)
        emit("SgV %s = sg_bool(g, %s);", r.c, v.c);
    else
        sg_fail(at, "a %s made a run-time value", kind_name(v.kind));
    return r;
}

/* a DYN int (UInt or SInt) as its bits */
static Val int_bits(Val v) {
    Val r = v;
    r.kind = VK_BITS;
    r.sign = 0;
    return r;
}

static Val dyn_op(const char* fn, int kind, int width, const char* args) {
    Val r = {TD, kind, width, 0, temp("v")};
    emit("SgV %s = %s(g, %s);", r.c, fn, args);
    return r;
}

/* ---- the walk ---- */

static Val ev(unsigned n);
static unsigned g_depth;      /* inlining depth */
static int g_dyn_ctl;         /* under an if whose condition is DYN: no state may change */
static unsigned g_root_node;  /* the root's body, for errors */
static int g_returned;        /* a return was walked in the block being walked */

/* the val of a back end function: its parameters' kinds and its result's type text */
typedef struct {
    int found;
    char ret[64];
    char params[8][64];
    int nparams;
} Sig;

/* a carriage return is a blank too: a checkout on Windows ends the Sail's lines with CR LF */
static void trim(char* s) {
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\t' || s[n - 1] == '\r')) s[--n] = 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') memmove(s, s + 1, strlen(s));
}

/* a val's text read again: "val NAME : [forall ... .] (T, ...) -> R" or "T -> R" */
static Sig signature(const char* name) {
    Sig s;
    unsigned d = sg_find(name, D_VAL);
    const SgFile* f;
    char text[1024], *p, *arrow = NULL, *q;
    int depth = 0, i;
    size_t n;
    memset(&s, 0, sizeof s);
    if (!d) return s;
    s.found = 1;
    f = &g_parse.files[g_parse.defs[d].source];
    n = g_parse.defs[d].text_end - g_parse.defs[d].text_start;
    if (n >= sizeof text) n = sizeof text - 1;
    memcpy(text, f->text + g_parse.defs[d].text_start, n);
    text[n] = 0;
    /* comments before it are its text too: from "val" */
    p = strstr(text, "val ");
    if (!p) return s;
    p = strchr(p, ':');
    if (!p) return s;
    ++p;
    q = strstr(p, "forall");
    if (q) {
        q = strchr(q, '.');
        if (q) p = q + 1;
    }
    for (q = p; *q; ++q) {
        if (*q == '(') ++depth;
        if (*q == ')') --depth;
        if (!depth && q[0] == '-' && q[1] == '>') arrow = q;
    }
    if (!arrow) return s;
    *arrow = 0;
    snprintf(s.ret, sizeof s.ret, "%s", arrow + 2);
    /* the result's text ends at a comment or the end */
    q = strstr(s.ret, "/*");
    if (q) *q = 0;
    q = strstr(s.ret, "//");
    if (q) *q = 0;
    trim(s.ret);
    trim(p);
    if (*p == '(') { /* a parenthesized list: split at the top level's commas */
        char* start;
        size_t len = strlen(p);
        if (len && p[len - 1] == ')') p[len - 1] = 0;
        ++p;
        start = p;
        depth = 0;
        for (q = p;; ++q) {
            if (*q == '(') ++depth;
            if (*q == ')') --depth;
            if ((!depth && *q == ',') || !*q) {
                int end = !*q;
                *q = 0;
                if (s.nparams < 8) {
                    snprintf(s.params[s.nparams], 64, "%s", start);
                    trim(s.params[s.nparams]);
                    if (s.params[s.nparams][0]) ++s.nparams;
                }
                if (end) break;
                start = q + 1;
            }
        }
    } else if (strcmp(p, "unit")) {
        snprintf(s.params[0], 64, "%s", p);
        s.nparams = 1;
    }
    for (i = 0; i < s.nparams; ++i) { /* "name : type" in a val names a parameter: the type */
        q = strchr(s.params[i], ':');
        if (q) {
            memmove(s.params[i], q + 1, strlen(q));
            trim(s.params[i]);
        }
    }
    return s;
}

/* the kind of a type's text, and for bits(N) its width (0 when N isn't a number: wid_param names it) */
static int type_kind(const char* t, int* width, char* wid_param) {
    *width = 0;
    if (wid_param) wid_param[0] = 0;
    if (!strncmp(t, "bits", 4)) {
        const char* p = strchr(t, '(');
        if (p) {
            if (p[1] == '\'') {
                if (wid_param) sscanf(p + 2, "%31[A-Za-z0-9_]", wid_param);
            } else
                *width = atoi(p + 1);
        }
        return VK_BITS;
    }
    if (!strcmp(t, "bool")) return VK_BOOL;
    if (!strcmp(t, "unit")) return VK_UNIT;
    if (!strcmp(t, "string")) return VK_STR;
    if (!strncmp(t, "int", 3) || !strncmp(t, "nat", 3) || !strncmp(t, "range", 5) || !strncmp(t, "atom", 4))
        return VK_INT;
    /* a Sail enum's name, else a type of the back end's own */
    {
        char name[64];
        sscanf(t, "%63[A-Za-z0-9_]", name);
        return sg_find(name, D_ENUM) ? VK_ENUM : VK_OPAQUE;
    }
}

/* the width a type's text gives, for a let's or var's ascription */
static int ascribed_width(unsigned type) {
    int w;
    type_kind(sg_str(NODE(type).name), &w, NULL);
    return w;
}

static int is_enum_member(unsigned name, int* index) {
    unsigned d;
    for (d = 1; d < g_parse.def_count; ++d) {
        unsigned m;
        int i = 0;
        if (g_parse.defs[d].kind != D_ENUM) continue;
        for (m = g_parse.defs[d].params; m; m = NODE(m).next, ++i)
            if (!strcmp(sg_str(NODE(m).name), sg_str(name))) {
                if (index) *index = i;
                return 1;
            }
    }
    return 0;
}

static Val ev_args_call(unsigned n);

static int const_int(Val v, long long* out) {
    char* end;
    if (v.time != TS) return 0;
    *out = strtoll(v.c, &end, 0);
    return *end == 0 || !strcmp(end, "ll") || !strcmp(end, "ull");
}

static Val v_int_static(const char* c) { return v_static(VK_INT, 0, c); }

/* the bits a slice or a ZeroExtend makes are at most 64 wide */
static void check_width(unsigned at, int w) {
    if (w < 1 || w > 64) sg_fail(at, "bits of width %d: sailgen's run-time values are 1 to 64 bits", w);
}

static const char* mask_of(int w) { return w >= 64 ? "0xffffffffffffffffull" : fmt("0x%llxull", (1ull << w) - 1); }

/* STATIC bits of width w: masked to w */
static const char* masked(const char* c, int w) { return w && w < 64 ? fmt("((%s) & %s)", c, mask_of(w)) : c; }

/* b, the right operand of a binary operator whose left one is a: where a is known when the translator runs and b is a
   run-time value, a is made the back end's constant before b's code, as C's left to right arguments of the
   hand-written emitters made it (the constant's id then comes first) */
static Val ev_right(unsigned n, Val* a) {
    Buf* outer = g_buf;
    Buf code = {0};
    Val b;
    g_buf = &code;
    b = ev(NODE(n).b);
    g_buf = outer;
    if (a->time == TS && b.time == TD && strcmp(sg_str(NODE(n).name), "@")) {
        if (a->kind == VK_BITS && (b.kind == VK_BITS || b.kind == VK_INT)) {
            Val x = *a;
            if (!x.width) x.width = b.width;
            if (x.width) *a = dyn(n, x);
        } else if (a->kind == VK_BOOL && b.kind == VK_BOOL)
            *a = dyn(n, *a);
        else if (a->kind == VK_INT && b.kind == VK_INT && b.width) {
            Val x = dyn(n, v_static(VK_BITS, b.width, a->c));
            x.kind = VK_INT;
            x.sign = b.sign;
            *a = x;
        }
    }
    buf_add(g_buf, code.text ? code.text : "");
    free(code.text);
    return b;
}

static Val ev_binop(unsigned n) {
    const char* op = sg_str(NODE(n).name);
    Val a = ev(NODE(n).a), b;
    int w;
    if (!strcmp(op, "&") || !strcmp(op, "|")) {
        /* bool's and and or are lazy in Sail: a STATIC false (true) left side decides it in C */
        b = ev_right(n, &a);
        if (a.kind != b.kind) sg_fail(n, "%s of %s and %s", op, kind_name(a.kind), kind_name(b.kind));
        if (a.kind == VK_BOOL) {
            if (a.time == TS && b.time == TS) return v_static(VK_BOOL, 0, fmt("(%s %s%s %s)", a.c, op, op, b.c));
            a = dyn(n, a);
            b = dyn(n, b);
            return dyn_op(op[0] == '&' ? "sg_and_bool" : "sg_or_bool", VK_BOOL, 0, fmt("%s, %s", a.c, b.c));
        }
        if (a.kind != VK_BITS) sg_fail(n, "%s of %s", op, kind_name(a.kind));
        w = a.width ? a.width : b.width;
        if (a.time == TS && b.time == TS) return v_static(VK_BITS, w, fmt("(%s %s %s)", a.c, op, b.c));
        if (a.width && b.width && a.width != b.width) sg_fail(n, "%s of bits(%d) and bits(%d)", op, a.width, b.width);
        a = dyn(n, a.width ? a : (a.width = w, a));
        b = dyn(n, b.width ? b : (b.width = w, b));
        return dyn_op(op[0] == '&' ? "sg_and" : "sg_or", VK_BITS, w, fmt("%d, %s, %s", w, a.c, b.c));
    }
    b = ev_right(n, &a);
    if (!strcmp(op, "@")) {
        if (a.kind != VK_BITS || b.kind != VK_BITS || !a.width || !b.width) sg_fail(n, "@ of bits of unknown widths");
        check_width(n, a.width + b.width);
        if (a.time == TS && b.time == TS)
            return v_static(VK_BITS, a.width + b.width, fmt("((%s) << %d | (%s))", a.c, b.width, b.c));
        a = dyn(n, a);
        b = dyn(n, b);
        return dyn_op("sg_concat", VK_BITS, a.width + b.width, fmt("%d, %s, %d, %s", a.width, a.c, b.width, b.c));
    }
    if (!strcmp(op, "==") || !strcmp(op, "!=")) {
        int ne = op[0] == '!';
        if (a.kind == VK_INT || b.kind == VK_INT) {
            if (a.time == TD || b.time == TD) {
                Val x = a.kind == VK_INT ? int_bits(a) : a, y = b.kind == VK_INT ? int_bits(b) : b;
                if (x.kind != VK_BITS || y.kind != VK_BITS) sg_fail(n, "%s of a run-time int and a %s", op, kind_name(b.kind));
                if (!x.width) x.width = y.width;
                if (!y.width) y.width = x.width;
                x = dyn(n, x);
                y = dyn(n, y);
                return dyn_op(ne ? "sg_ne" : "sg_eq", VK_BOOL, 0, fmt("%d, %s, %s", x.width, x.c, y.c));
            }
            return v_static(VK_BOOL, 0, fmt("((int64_t)(%s) %s (int64_t)(%s))", a.c, op, b.c));
        }
        if (a.kind != b.kind) sg_fail(n, "%s of %s and %s", op, kind_name(a.kind), kind_name(b.kind));
        if (a.time == TS && b.time == TS) return v_static(VK_BOOL, 0, fmt("(%s %s %s)", a.c, op, b.c));
        if (a.kind == VK_BOOL) {
            a = dyn(n, a);
            b = dyn(n, b);
            return dyn_op(ne ? "sg_ne_bool" : "sg_eq_bool", VK_BOOL, 0, fmt("%s, %s", a.c, b.c));
        }
        if (a.kind != VK_BITS) sg_fail(n, "%s of run-time %s", op, kind_name(a.kind));
        w = a.width ? a.width : b.width;
        a.width = b.width = w;
        a = dyn(n, a);
        b = dyn(n, b);
        return dyn_op(ne ? "sg_ne" : "sg_eq", VK_BOOL, 0, fmt("%d, %s, %s", w, a.c, b.c));
    }
    if (!strcmp(op, "<") || !strcmp(op, "<=") || !strcmp(op, ">") || !strcmp(op, ">=")) {
        static const char* const names[2][4] = {{"sg_ult", "sg_ule", "sg_ugt", "sg_uge"},
                                                {"sg_slt", "sg_sle", "sg_sgt", "sg_sge"}};
        int k = op[0] == '<' ? (op[1] ? 1 : 0) : (op[1] ? 3 : 2), sign;
        if (a.kind != VK_INT || b.kind != VK_INT) sg_fail(n, "%s of %s and %s: compare ints", op, kind_name(a.kind), kind_name(b.kind));
        if (a.time == TS && b.time == TS) return v_static(VK_BOOL, 0, fmt("((int64_t)(%s) %s (int64_t)(%s))", a.c, op, b.c));
        /* a run-time comparison: UInt against UInt (or a number), SInt against SInt */
        sign = a.time == TD ? a.sign : b.sign;
        if (a.time == TD && b.time == TD && a.sign != b.sign) sg_fail(n, "%s of UInt and SInt", op);
        if (a.time == TD && b.time == TD && a.width != b.width) sg_fail(n, "%s of ints of %d and %d bits", op, a.width, b.width);
        w = a.time == TD ? a.width : b.width;
        a = a.time == TD ? int_bits(a) : dyn(n, v_static(VK_BITS, w, a.c));
        b = b.time == TD ? int_bits(b) : dyn(n, v_static(VK_BITS, w, b.c));
        return dyn_op(names[sign][k], VK_BOOL, 0, fmt("%d, %s, %s", w, a.c, b.c));
    }
    if (!strcmp(op, "+") || !strcmp(op, "-") || !strcmp(op, "*")) {
        const char* fn = op[0] == '+' ? "sg_add" : op[0] == '-' ? "sg_sub" : "sg_mul";
        if (a.kind == VK_INT && b.kind == VK_INT) {
            if (a.time == TD || b.time == TD) sg_fail(n, "%s of run-time ints: slice the bits instead", op);
            return v_int_static(fmt("((int64_t)(%s) %s (int64_t)(%s))", a.c, op, b.c));
        }
        if (a.kind == VK_BITS && b.kind == VK_INT) b = v_static(VK_BITS, a.width, b.c);
        if (a.kind != VK_BITS || b.kind != VK_BITS) sg_fail(n, "%s of %s and %s", op, kind_name(a.kind), kind_name(b.kind));
        w = a.width ? a.width : b.width;
        if (a.width && b.width && a.width != b.width) sg_fail(n, "%s of bits(%d) and bits(%d)", op, a.width, b.width);
        if (a.time == TS && b.time == TS) return v_static(VK_BITS, w, masked(fmt("(%s %s %s)", a.c, op, b.c), w));
        a.width = b.width = w;
        a = dyn(n, a);
        b = dyn(n, b);
        return dyn_op(fn, VK_BITS, w, fmt("%d, %s, %s", w, a.c, b.c));
    }
    if (!strcmp(op, "^")) { /* an int's power, known when the translator runs */
        if (a.kind != VK_INT || b.kind != VK_INT || a.time == TD || b.time == TD) sg_fail(n, "^ of other than ints known when the translator runs");
        return v_int_static(fmt("sg_pow_int(%s, %s)", a.c, b.c));
    }
    if (!strcmp(op, "<<") || !strcmp(op, ">>")) {
        if (a.kind == VK_INT && b.kind == VK_INT && a.time == TS && b.time == TS)
            return v_int_static(fmt("((int64_t)(%s) %s (%s))", a.c, op, b.c));
        if (a.kind != VK_BITS) sg_fail(n, "%s of a %s", op, kind_name(a.kind));
        if (b.kind == VK_BITS) b = b.time == TS ? v_int_static(b.c) : b;
        if (a.time == TS && b.time == TS) return v_static(VK_BITS, a.width, masked(fmt("((uint64_t)(%s) %s (%s))", a.c, op, b.c), a.width));
        a = dyn(n, a);
        b = b.time == TS ? dyn(n, v_static(VK_BITS, a.width, b.c)) : b;
        return dyn_op(op[0] == '<' ? "sg_shl" : "sg_lshr", VK_BITS, a.width, fmt("%d, %s, %s", a.width, a.c, b.c));
    }
    sg_fail(n, "the operator %s", op);
    return v_unit();
}

/* the declared type of a parameter of a Sail function, for binding its argument */
static Val ev_slice(unsigned n) {
    Val x = ev(NODE(n).a), hi = ev(NODE(n).b), lo = ev(NODE(n).c);
    long long h, l;
    if (x.kind != VK_BITS) sg_fail(n, "a slice of a %s", kind_name(x.kind));
    if (!const_int(hi, &h) || !const_int(lo, &l)) {
        if (x.time == TD) sg_fail(n, "a run-time value's slice at bounds not known when sailgen runs");
        return v_static(VK_BITS, 0, fmt("(((uint64_t)(%s) >> (%s)) & ((2ull << ((%s) - (%s))) - 1))", x.c, lo.c, hi.c, lo.c));
    }
    if (h < l || h > 63 || (x.width && h >= x.width)) sg_fail(n, "the slice [%lld .. %lld] of bits(%d)", h, l, x.width);
    if (x.time == TS) return v_static(VK_BITS, (int)(h - l + 1), masked(fmt("((uint64_t)(%s) >> %lld)", x.c, l), (int)(h - l + 1)));
    return dyn_op("sg_slice", VK_BITS, (int)(h - l + 1), fmt("%d, %s, %lld, %lld", x.width, x.c, h, l));
}

/* a call: a builtin of sailgen's, a Sail function (inlined) or the back end's */
static Val ev_call(unsigned n) {
    const char* name = sg_str(NODE(n).name);
    unsigned arg = NODE(n).a, d;
    Val args[8];
    int nargs = 0, i;
    unsigned an;
    /* the builtins of every machine */
    if (!strcmp(name, "not_bool")) {
        Val a = ev(arg);
        if (a.kind != VK_BOOL) sg_fail(n, "not_bool of a %s", kind_name(a.kind));
        if (a.time == TS) return v_static(VK_BOOL, 0, fmt("(!%s)", a.c));
        return dyn_op("sg_not_bool", VK_BOOL, 0, a.c);
    }
    if (!strcmp(name, "not_vec")) {
        Val a = ev(arg);
        if (a.kind != VK_BITS) sg_fail(n, "not_vec of a %s", kind_name(a.kind));
        if (a.time == TS) return v_static(VK_BITS, a.width, masked(fmt("(~(uint64_t)(%s))", a.c), a.width));
        return dyn_op("sg_not", VK_BITS, a.width, fmt("%d, %s", a.width, a.c));
    }
    if (!strcmp(name, "EOR") || !strcmp(name, "xor_vec")) {
        Val a = ev(arg), b = ev(NODE(arg).next);
        int w = a.width ? a.width : b.width;
        if (a.time == TS && b.time == TS) return v_static(VK_BITS, w, fmt("((%s) ^ (%s))", a.c, b.c));
        a.width = b.width = w;
        a = dyn(n, a);
        b = dyn(n, b);
        return dyn_op("sg_xor", VK_BITS, w, fmt("%d, %s, %s", w, a.c, b.c));
    }
    if (!strcmp(name, "UInt") || !strcmp(name, "SInt")) {
        Val a = ev(arg);
        int s = name[0] == 'S';
        if (a.kind != VK_BITS) sg_fail(n, "%s of a %s", name, kind_name(a.kind));
        if (a.time == TS) {
            if (!s) return v_int_static(fmt("((int64_t)(%s))", a.c));
            if (!a.width) sg_fail(n, "SInt of bits of a width sailgen doesn't know");
            return v_int_static(fmt("((int64_t)((uint64_t)(%s) << %d) >> %d)", a.c, 64 - a.width, 64 - a.width));
        }
        a.kind = VK_INT;
        a.sign = s;
        return a;
    }
    if (!strcmp(name, "ZeroExtend") || !strcmp(name, "SignExtend") || !strcmp(name, "Zeros") || !strcmp(name, "Ones")) {
        long long w;
        int z = !strcmp(name, "Zeros"), o = !strcmp(name, "Ones");
        Val a = z || o ? v_unit() : ev(arg), wv = ev(z || o ? arg : NODE(arg).next);
        if (!const_int(wv, &w)) sg_fail(n, "%s to a width not known when sailgen runs", name);
        check_width(n, (int)w);
        if (z) return v_static(VK_BITS, (int)w, "0ull");
        if (o) return v_static(VK_BITS, (int)w, mask_of((int)w));
        if (a.kind != VK_BITS || !a.width) sg_fail(n, "%s of bits of a width sailgen doesn't know", name);
        if (a.width > w) sg_fail(n, "%s of bits(%d) to %lld bits", name, a.width, w);
        if (a.time == TS) {
            if (name[0] == 'Z') return v_static(VK_BITS, (int)w, a.c);
            return v_static(VK_BITS, (int)w,
                            masked(fmt("((uint64_t)((int64_t)((uint64_t)(%s) << %d) >> %d))", a.c, 64 - a.width, 64 - a.width), (int)w));
        }
        if (a.width == w) return a;
        return dyn_op(name[0] == 'Z' ? "sg_zext" : "sg_sext", VK_BITS, (int)w, fmt("%d, %lld, %s", a.width, w, a.c));
    }
    if (!strcmp(name, "DIV") || !strcmp(name, "MOD")) { /* of ints known when the translator runs */
        Val a = ev(arg), b = ev(NODE(arg).next);
        if (a.kind != VK_INT || b.kind != VK_INT || a.time == TD || b.time == TD) sg_fail(n, "%s of other than ints known when the translator runs", name);
        return v_int_static(name[0] == 'D' ? fmt("sg_div_int(%s, %s)", a.c, b.c) : fmt("sg_mod_int(%s, %s)", a.c, b.c));
    }
    if (!strcmp(name, "to_bits")) { /* to_bits(N, i): an int's N bits */
        long long w;
        Val wv = ev(arg), a = ev(NODE(arg).next);
        if (!const_int(wv, &w)) sg_fail(n, "to_bits of a width not known when sailgen runs");
        check_width(n, (int)w);
        if (a.kind != VK_INT) sg_fail(n, "to_bits of a %s", kind_name(a.kind));
        if (a.time == TS) return v_static(VK_BITS, (int)w, masked(fmt("(uint64_t)(%s)", a.c), (int)w));
        if (a.width != w) sg_fail(n, "to_bits(%lld) of an int of %d bits", w, a.width);
        return int_bits(a);
    }
    if (!strcmp(name, "Abs")) {
        Val a = ev(arg);
        if (a.kind != VK_INT) sg_fail(n, "Abs of a %s", kind_name(a.kind));
        if (a.time == TS) return v_int_static(fmt("((int64_t)(%s) < 0 ? -(int64_t)(%s) : (int64_t)(%s))", a.c, a.c, a.c));
        if (!a.sign) return a;
        {
            Val r = dyn_op("sg_sabs", VK_BITS, a.width, fmt("%d, %s", a.width, a.c));
            r.kind = VK_INT;
            r.sign = 1;
            return r;
        }
    }
    if (!strcmp(name, "LSL") || !strcmp(name, "LSR") || !strcmp(name, "ASR")) { /* bits by an int or bits of theirs */
        Val a = ev(arg), b = ev(NODE(arg).next);
        if (a.kind != VK_BITS) sg_fail(n, "%s of a %s", name, kind_name(a.kind));
        if (a.time == TS && b.time == TS) {
            if (name[1] == 'S' && name[2] == 'L') return v_static(VK_BITS, a.width, masked(fmt("((uint64_t)(%s) << (%s))", a.c, b.c), a.width));
            if (name[0] == 'L') return v_static(VK_BITS, a.width, fmt("((uint64_t)(%s) >> (%s))", a.c, b.c));
            if (!a.width) sg_fail(n, "ASR of bits of a width sailgen doesn't know");
            return v_static(VK_BITS, a.width, masked(fmt("((uint64_t)(((int64_t)((uint64_t)(%s) << %d) >> %d) >> (%s)))", a.c, 64 - a.width, 64 - a.width, b.c), a.width));
        }
        a = dyn(n, a);
        if (b.time == TS) b = dyn(n, v_static(VK_BITS, a.width, b.c));
        else if (b.kind == VK_INT) b = int_bits(b);
        if (b.width != a.width) sg_fail(n, "%s of bits(%d) by bits(%d)", name, a.width, b.width);
        return dyn_op(name[1] == 'S' && name[2] == 'L' ? "sg_shl" : name[0] == 'L' ? "sg_lshr" : "sg_ashr", VK_BITS, a.width,
                      fmt("%d, %s, %s", a.width, a.c, b.c));
    }
    if (!strcmp(name, "Min") || !strcmp(name, "Max")) {
        Val a = ev(arg), b = ev(NODE(arg).next);
        int w;
        if (a.kind != VK_INT || b.kind != VK_INT) sg_fail(n, "%s of a %s and a %s", name, kind_name(a.kind), kind_name(b.kind));
        if (a.time == TS && b.time == TS)
            return v_int_static(fmt("((int64_t)(%s) %s (int64_t)(%s) ? (int64_t)(%s) : (int64_t)(%s))", a.c,
                                    name[1] == 'i' ? "<" : ">", b.c, a.c, b.c));
        if (a.time == TD && b.time == TD && (a.sign != b.sign || a.width != b.width))
            sg_fail(n, "%s of ints of other signs or widths", name);
        w = a.time == TD ? a.width : b.width;
        {
            int s = a.time == TD ? a.sign : b.sign;
            Val x = a.time == TD ? int_bits(a) : dyn(n, v_static(VK_BITS, w, a.c));
            Val y = b.time == TD ? int_bits(b) : dyn(n, v_static(VK_BITS, w, b.c));
            Val r = dyn_op(fmt("sg_%s%s", s ? "s" : "u", name[1] == 'i' ? "min" : "max"), VK_BITS, w,
                           fmt("%d, %s, %s", w, x.c, y.c));
            r.kind = VK_INT;
            r.sign = s;
            return r;
        }
    }
    /* a Sail function: inlined, its parameters bound to the arguments */
    d = sg_find(name, D_FUNCTION);
    if (d) {
        unsigned p = g_parse.defs[d].params;
        int mark = g_nenv, saved_returned = g_returned;
        Val r;
        if (++g_depth > 64) sg_fail(n, "%s: inlined 64 deep, a recursion sailgen doesn't read", name);
        for (an = arg; an; an = NODE(an).next) {
            if (nargs == 8) sg_fail(n, "a call of more than 8 arguments");
            args[nargs++] = ev(an);
        }
        /* function f(x) or (x : T, ...): a list of patterns; function f() = ... has none */
        for (i = 0; p; p = NODE(p).next, ++i) {
            unsigned pat = p;
            if (NODE(pat).kind == N_UNIT) break;
            if (NODE(pat).kind == N_TYPED) pat = NODE(pat).a;
            if (NODE(pat).kind == N_TUPLE) sg_fail(n, "%s's parameters as a tuple pattern", name);
            if (i >= nargs) sg_fail(n, "%s takes more arguments than the call's %d", name, nargs);
            if (NODE(pat).kind != N_ID) sg_fail(n, "%s's parameter %d is no name", name, i);
            if (NODE(p).kind == N_TYPED && args[i].kind == VK_BITS && !args[i].width)
                args[i].width = ascribed_width(NODE(p).b);
            bind(NODE(pat).name, args[i], 0);
        }
        if (i != nargs) sg_fail(n, "%s takes %d arguments, not %d", name, i, nargs);
        g_returned = 0;
        r = ev(g_parse.defs[d].body);
        g_returned = saved_returned;
        g_nenv = mark;
        --g_depth;
        return r;
    }
    return ev_args_call(n);
}

/* a function with no Sail body: the back end's sg_NAME, its arguments as its val types them (bits and bool DYN, int,
   an enum, a string STATIC), its result DYN unless the roots name it static */
static Val ev_args_call(unsigned n) {
    const char* name = sg_str(NODE(n).name);
    Sig s = signature(name);
    unsigned an;
    const char* list = "";
    int i = 0, st = is_static_fn(name), kind, width;
    char wid_param[32];
    long long wid_value = 0;
    if (!s.found) sg_fail(n, "%s has no Sail definition and no val for the back end", name);
    kind = type_kind(s.ret, &width, wid_param);
    for (an = NODE(n).a; an; an = NODE(an).next, ++i) {
        Val a;
        int pk, pw;
        char pparam[32];
        if (i >= s.nparams) sg_fail(n, "%s's val takes %d arguments", name, s.nparams);
        pk = type_kind(s.params[i], &pw, pparam);
        if (NODE(an).kind == N_ID && sg_str(NODE(an).name)[0] == '"') {
            list = fmt("%s, %s", list, sg_str(NODE(an).name));
            continue;
        }
        a = ev(an);
        if (a.kind == VK_INT && pk == VK_INT && wid_param[0] && strstr(s.params[i], fmt("'%s", wid_param))) {
            if (!const_int(a, &wid_value)) sg_fail(n, "%s's width argument is not known when sailgen runs", name);
            width = (int)wid_value;
        }
        if (pk == VK_BITS && a.kind == VK_INT && a.time == TS) a = v_static(VK_BITS, pw, a.c);
        /* an int parameter takes any value known when the translator runs, as C */
        if (pk == VK_INT && a.time == TS && (a.kind == VK_BITS || a.kind == VK_BOOL || a.kind == VK_ENUM)) a.kind = VK_INT;
        if (pk != a.kind && !(pk == VK_ENUM && a.kind == VK_INT))
            sg_fail(n, "%s's argument %d is a %s, its val's a %s (%s)", name, i + 1, kind_name(a.kind),
                    kind_name(pk), s.params[i]);
        if (st) {
            if (a.time == TD) sg_fail(n, "%s is static, its argument %d a run-time value", name, i + 1);
            list = fmt("%s, %s", list, a.c);
        } else if (pk == VK_OPAQUE) {
            if (a.kind != VK_OPAQUE) sg_fail(n, "%s's argument %d is a %s, not an opaque value", name, i + 1, kind_name(a.kind));
            list = fmt("%s, %s", list, a.c);
        } else if (pk == VK_BITS || pk == VK_BOOL) {
            if (pk == VK_BITS && !a.width) a.width = pw;
            list = fmt("%s, %s", list, dyn(an, a).c);
        } else {
            if (a.time == TD) sg_fail(n, "%s's argument %d (%s) is a run-time value", name, i + 1, s.params[i]);
            list = fmt("%s, %s", list, a.c);
        }
    }
    if (i != s.nparams) sg_fail(n, "%s takes %d arguments, the call gives %d", name, s.nparams, i);
    if (st) {
        /* made once, where the Sail asks for it: a back end's function the translator runs may have effects of its
           own (a resource it declares, what it notes in the program's info), in their order */
        const char* t;
        if (kind == VK_UNIT) sg_fail(n, "%s is static and gives nothing", name);
        t = temp("s");
        emit("const %s %s = sg_%s(g%s);",
             kind == VK_BITS ? "uint64_t" : kind == VK_INT ? "int64_t" : kind == VK_STR ? "char*" : "int", t, name, list);
        return v_static(kind, width, t);
    }
    if (kind == VK_UNIT) {
        if (g_dyn_ctl) sg_fail(n, "%s changes state under a run-time condition", name);
        emit("sg_%s(g%s);", name, list);
        return v_unit();
    }
    if (kind != VK_BITS && kind != VK_BOOL && kind != VK_OPAQUE) sg_fail(n, "%s gives a %s at run time", name, kind_name(kind));
    if (kind == VK_BITS) check_width(n, width);
    {
        Val r = {TD, kind, width, 0, temp("v")};
        emit("SgV %s = sg_%s(g%s);", r.c, name, list);
        return r;
    }
}

/* ---- control: branches whose locals merge ---- */

typedef struct {
    Buf text;
    Bind env[1024];
    int nenv;
    Val value;
    int aborted;  /* ends in a throw: its locals and value don't merge */
    int returned; /* ends in a return: the choice must be the function's last item */
} Arm;

/* what branches keeps while it walks its arms and merges them, some 80 KiB: in the heap, as it calls itself at each
   nested choice and Windows gives a program's main thread 1 MiB of stack */
typedef struct {
    Bind base[1024];         /* the environment the arms start from */
    const char* names[1025]; /* the merged locals and value: their C names, types and places in the environment */
    Val types[1025];
    int which[1025];
} Walk;

/* the value of a merged local: the time and kind both arms agree on */
static Val merge_type(unsigned at, Val a, Val b) {
    Val r = a;
    if (a.kind != b.kind) sg_fail(at, "a local is a %s in one branch and a %s in the other", kind_name(a.kind), kind_name(b.kind));
    r.time = a.time == TD || b.time == TD ? TD : TS;
    if (!r.width) r.width = b.width;
    if (a.kind == VK_INT && r.time == TD) sg_fail(at, "a run-time int merged from branches");
    return r;
}

static int same_val(Val a, Val b) { return a.time == b.time && a.kind == b.kind && !strcmp(a.c, b.c); }

/* the C declaration of a merged local */
static const char* c_type(Val v) {
    if (v.time == TD) return "SgV";
    return v.kind == VK_BITS ? "uint64_t" : v.kind == VK_INT ? "int64_t" : "int";
}

/* walks the arms (n of them, conds[i] the C condition of arm i, NULL for the last's else) of a STATIC choice, each
   from the same environment, and merges what they leave: the locals they set differently and their values. A
   DYN condition (dyn_cond, two arms) merges them as selects instead, and no arm may change state */
static Val branches(unsigned at, const char** conds, const unsigned* bodies, int n, const char* dyn_cond) {
    Arm* arms = (Arm*)sg_alloc(sizeof(Arm) * (size_t)n);
    Walk* walk = (Walk*)sg_alloc(sizeof *walk);
    Bind* base = walk->base;
    Buf* outer = g_buf;
    int i, k, base_nenv = g_nenv, has_value = 0, live = 0;
    Val result = v_unit();
    memcpy(base, g_env, sizeof(Bind) * (size_t)g_nenv);
    if (dyn_cond) ++g_dyn_ctl;
    for (i = 0; i < n; ++i) {
        memcpy(g_env, base, sizeof(Bind) * (size_t)base_nenv);
        g_nenv = base_nenv;
        g_buf = &arms[i].text;
        g_indent += dyn_cond ? 0 : 1;
        g_returned = 0;
        arms[i].value = bodies[i] ? ev(bodies[i]) : v_unit();
        arms[i].aborted = g_returned == 2;
        arms[i].returned = g_returned == 1;
        g_indent -= dyn_cond ? 0 : 1;
        memcpy(arms[i].env, g_env, sizeof(Bind) * (size_t)base_nenv);
        arms[i].nenv = base_nenv;
        if (!arms[i].text.text) buf_add(&arms[i].text, "");
        if (!arms[i].aborted) {
            if (arms[i].value.kind != VK_UNIT) has_value = 1;
            ++live;
        }
    }
    g_returned = 0;
    {
        int returned = 0, flowing = 0;
        for (i = 0; i < n; ++i) {
            if (arms[i].aborted) continue;
            if (arms[i].returned)
                ++returned;
            else
                ++flowing;
        }
        if (n == 1 && returned) flowing = 1; /* an if with no else: the missing arm flows on */
        if (returned && flowing) sg_fail(at, "an early return: write the rest as the else");
        if (returned) g_returned = 1;
    }
    if (dyn_cond) --g_dyn_ctl;
    g_buf = outer;
    g_nenv = base_nenv;
    memcpy(g_env, base, sizeof(Bind) * (size_t)base_nenv);
    if (dyn_cond) {
        /* both arms run: their code, then a select of each local they set differently and of their values */
        if (arms[0].aborted || arms[1].aborted) sg_fail(at, "a throw under a run-time condition");
        buf_add(g_buf, arms[0].text.text);
        buf_add(g_buf, arms[1].text.text);
        for (k = 0; k < base_nenv; ++k) {
            Val a = arms[0].env[k].v, b = arms[1].env[k].v, t;
            if (same_val(a, b)) continue;
            t = merge_type(at, a, b);
            if (a.kind == VK_INT || a.kind == VK_ENUM) sg_fail(at, "a %s chosen at run time", kind_name(a.kind));
            a = dyn(at, a.width ? a : (a.width = t.width, a));
            b = dyn(at, b.width ? b : (b.width = t.width, b));
            g_env[k].v = dyn_op("sg_select", t.kind, t.width,
                                fmt("%d, %s, %s, %s", t.kind == VK_BOOL ? 0 : t.kind == VK_OPAQUE ? -1 : t.width, dyn_cond, a.c, b.c));
        }
        if (has_value) {
            Val a = arms[0].value, b = arms[1].value, t;
            int sign = -1;
            /* a run-time int of each arm: its bits chosen, ints of one sign and width */
            if (a.kind == VK_INT && b.kind == VK_INT && a.time == TD && b.time == TD) {
                if (a.sign != b.sign || a.width != b.width) sg_fail(at, "ints of other signs or widths chosen at run time");
                sign = a.sign;
                a = int_bits(a);
                b = int_bits(b);
            }
            t = merge_type(at, a, b);
            if (t.kind != VK_BITS && t.kind != VK_BOOL && t.kind != VK_OPAQUE) sg_fail(at, "a %s chosen at run time", kind_name(t.kind));
            if (t.kind != VK_OPAQUE) {
                a = dyn(at, a.width ? a : (a.width = t.width, a));
                b = dyn(at, b.width ? b : (b.width = t.width, b));
            }
            result = dyn_op("sg_select", t.kind, t.width, fmt("%d, %s, %s, %s", t.kind == VK_BOOL ? 0 : t.kind == VK_OPAQUE ? -1 : t.width, dyn_cond, a.c, b.c));
            if (sign >= 0) {
                result.kind = VK_INT;
                result.sign = sign;
            }
        }
    } else {
        /* the merged locals and value are declared before the choice; each arm ends by setting them */
        const char** names = walk->names;
        Val* types = walk->types;
        int* which = walk->which;
        int nm = 0, first_live = -1, implicit_else = n == 1 || conds[n - 1] != NULL;
        for (i = 0; i < n; ++i)
            if (!arms[i].aborted) {
                first_live = i;
                break;
            }
        for (k = 0; k < base_nenv && first_live >= 0; ++k) {
            Val t = arms[first_live].env[k].v;
            int differ = 0;
            for (i = 0; i < n; ++i) {
                if (arms[i].aborted) continue;
                if (!same_val(arms[i].env[k].v, arms[first_live].env[k].v)) differ = 1;
                t = merge_type(at, t, arms[i].env[k].v);
            }
            if (implicit_else) { /* the choice that takes no arm leaves the local as it was */
                if (!same_val(base[k].v, arms[first_live].env[k].v)) differ = 1;
                t = merge_type(at, t, base[k].v);
            }
            if (!differ) {
                g_env[k].v = arms[first_live].env[k].v;
                continue;
            }
            names[nm] = temp(t.time == TD ? "m" : "n");
            types[nm] = t;
            which[nm++] = k;
        }
        if (has_value) {
            Val t = arms[first_live].value;
            for (i = 0; i < n; ++i)
                if (!arms[i].aborted) t = merge_type(at, t, arms[i].value);
            names[nm] = temp(t.time == TD ? "m" : "n");
            types[nm] = t;
            which[nm++] = -1;
        }
        for (k = 0; k < nm; ++k) emit("%s %s;", c_type(types[k]), names[k]);
        for (i = 0; i < n; ++i) {
            Buf* save = g_buf;
            if (i == 0)
                emit("if (%s) {", conds[0]);
            else if (conds[i])
                emit("} else if (%s) {", conds[i]);
            else
                emit("} else {");
            buf_add(g_buf, arms[i].text.text);
            if (!arms[i].aborted) {
                ++g_indent;
                for (k = 0; k < nm; ++k) {
                    Val v = which[k] < 0 ? arms[i].value : arms[i].env[which[k]].v;
                    if (types[k].time == TD && v.time == TS) {
                        if (!v.width) v.width = types[k].width;
                        v = dyn(at, v);
                    }
                    emit("%s = %s;", names[k], v.c);
                }
                --g_indent;
            }
            g_buf = save;
        }
        if (n == 1 || conds[n - 1]) { /* no else: the locals keep what they were */
            emit("} else {");
            ++g_indent;
            for (k = 0; k < nm; ++k) {
                Val v = which[k] < 0 ? v_unit() : base[which[k]].v;
                if (which[k] < 0) sg_fail(at, "an if with a value and no else");
                if (types[k].time == TD && v.time == TS) {
                    if (!v.width) v.width = types[k].width;
                    v = dyn(at, v);
                }
                emit("%s = %s;", names[k], v.c);
            }
            --g_indent;
        }
        emit("}");
        for (k = 0; k < nm; ++k) {
            Val v = types[k];
            v.c = names[k];
            if (which[k] < 0)
                result = v;
            else
                g_env[which[k]].v = v;
        }
        if (!live && !(n == 1 || conds[n - 1])) g_returned = 2; /* every arm threw */
    }
    for (i = 0; i < n; ++i) free(arms[i].text.text);
    free(arms);
    free(walk);
    return result;
}

static Val ev_if(unsigned n) {
    Val c = ev(NODE(n).a);
    unsigned bodies[2] = {NODE(n).b, NODE(n).c};
    const char* conds[2];
    if (c.kind != VK_BOOL) sg_fail(n, "an if on a %s", kind_name(c.kind));
    if (c.time == TD) {
        Val r = branches(n, NULL, bodies, 2, c.c);
        return r;
    }
    conds[0] = c.c;
    conds[1] = NULL;
    return branches(n, conds, bodies, NODE(n).c ? 2 : 1, NULL);
}

/* the C test of a case's pattern against a STATIC value */
static const char* case_test(unsigned pat, Val x) {
    if (NODE(pat).kind == N_WILD) return NULL;
    if (NODE(pat).kind == N_BITS) return fmt("(%s) == 0x%llxull", x.c, (unsigned long long)NODE(pat).value);
    if (NODE(pat).kind == N_NUM) return fmt("(int64_t)(%s) == %lld", x.c, (long long)NODE(pat).value);
    if (NODE(pat).kind == N_ID) {
        int index;
        if (is_enum_member(NODE(pat).name, &index)) {
            /* a member matched against a member: known when sailgen runs */
            if (!strncmp(x.c, "SGE_", 4)) return strcmp(x.c + 4, sg_str(NODE(pat).name)) ? "0" : "1";
            return fmt("(%s) == SGE_%s", x.c, sg_str(NODE(pat).name));
        }
    }
    if (NODE(pat).kind == N_BINOP && !strcmp(sg_str(NODE(pat).name), "|"))
        return fmt("(%s) || (%s)", case_test(NODE(pat).a, x), case_test(NODE(pat).b, x));
    sg_fail(pat, "a case pattern sailgen doesn't read");
    return NULL;
}

static Val ev_match(unsigned n) {
    Val x = ev(NODE(n).a);
    unsigned c;
    const char* conds[256];
    unsigned bodies[256];
    int count = 0;
    if (x.time == TD) sg_fail(n, "a match on a run-time value: write it as ifs");
    for (c = NODE(n).b; c; c = NODE(c).next) {
        if (count == 256) sg_fail(n, "a match of more than 256 cases"); /* lut3.sail's tables are 256 */
        if (NODE(c).c) sg_fail(c, "a case's guard");
        conds[count] = case_test(NODE(c).a, x);
        bodies[count++] = NODE(c).b;
        if (!conds[count - 1]) break; /* _ takes the rest */
    }
    if (count && conds[count - 1]) { /* no wildcard: what no case takes is the root's failure */
        sg_fail(n, "a match with no _ case");
    }
    if (count == 1) return ev(bodies[0]);
    return branches(n, conds, bodies, count, NULL);
}

/* a foreach whose bounds are STATIC: a C loop. The locals it sets are carried through it, each one's time the latest
   any iteration gives it: the body is walked again until none changes */
static Val ev_foreach(unsigned n) {
    Val from = ev(NODE(n).a), to = ev(NODE(n).b), by = NODE(n).c ? ev(NODE(n).c) : v_int_static("1");
    int base_nenv = g_nenv, k, round;
    Walk* walk = (Walk*)sg_alloc(sizeof *walk); /* the environment and the carried locals' names, as branches' */
    Bind* base = walk->base;
    const char** carried = walk->names;
    const char* var;
    if (from.time == TD || to.time == TD || by.time == TD) sg_fail(n, "a foreach of run-time bounds");
    if (g_dyn_ctl) sg_fail(n, "a foreach under a run-time condition");
    memcpy(base, g_env, sizeof(Bind) * (size_t)g_nenv);
    var = temp("i");
    for (round = 0; round < 8; ++round) {
        Buf body = {0};
        Buf* outer = g_buf;
        int changed = 0;
        memcpy(g_env, base, sizeof(Bind) * (size_t)base_nenv);
        g_nenv = base_nenv;
        /* each local the body may set is a C variable the loop carries */
        for (k = 0; k < base_nenv; ++k) {
            carried[k] = NULL;
            if (!g_env[k].mutable) continue;
            carried[k] = temp(g_env[k].v.time == TD ? "m" : "n");
            g_env[k].v.c = carried[k];
        }
        bind(NODE(n).name, v_int_static(var), 0);
        g_buf = &body;
        g_indent += 1;
        ev(NODE(n).d);
        if (g_returned) sg_fail(n, "a return or a throw in a foreach");
        for (k = 0; k < base_nenv; ++k) {
            if (!carried[k]) continue;
            if (g_env[k].v.time != base[k].v.time) {
                if (g_env[k].v.time == TD && base[k].v.time == TS) {
                    /* again, with the local DYN from the start: made so before the loop */
                    Buf* inner = g_buf;
                    int indent = g_indent;
                    Val v = base[k].v;
                    g_buf = outer;
                    g_indent -= 1;
                    if (!v.width) v.width = g_env[k].v.width;
                    base[k].v = dyn(n, v);
                    g_buf = inner;
                    g_indent = indent;
                    changed = 1;
                } else if (g_env[k].v.time == TS) {
                    Val v = g_env[k].v;
                    if (!v.width) v.width = base[k].v.width;
                    g_env[k].v = dyn(n, v);
                }
            }
            if (strcmp(g_env[k].v.c, carried[k])) emit("%s = %s;", carried[k], g_env[k].v.c);
        }
        g_indent -= 1;
        g_buf = outer;
        if (changed) {
            free(body.text);
            continue;
        }
        g_nenv = base_nenv;
        for (k = 0; k < base_nenv; ++k) {
            Val v = base[k].v;
            g_env[k] = base[k];
            if (!carried[k]) continue;
            emit("%s %s = %s;", c_type(v), carried[k], v.c);
            g_env[k].v.c = carried[k];
        }
        if (NODE(n).dec)
            emit("for (int64_t %s = %s; %s >= %s; %s -= %s) {", var, from.c, var, to.c, var, by.c);
        else
            emit("for (int64_t %s = %s; %s <= %s; %s += %s) {", var, from.c, var, to.c, var, by.c);
        buf_add(g_buf, body.text ? body.text : "");
        emit("}");
        free(body.text);
        free(walk);
        return v_unit();
    }
    sg_fail(n, "a foreach whose locals' times don't settle");
    return v_unit();
}

static void ev_assign(unsigned n) {
    unsigned lhs = NODE(n).a;
    Val v = ev(NODE(n).b);
    int i;
    if (NODE(lhs).kind != N_ID) sg_fail(n, "an assignment to other than a local");
    i = lookup(NODE(lhs).name);
    if (i < 0 || !g_env[i].mutable) sg_fail(n, "an assignment to %s, which is no var", sg_str(NODE(lhs).name));
    if (v.kind != g_env[i].v.kind) sg_fail(n, "%s is a %s, assigned a %s", sg_str(NODE(lhs).name), kind_name(g_env[i].v.kind), kind_name(v.kind));
    if (v.kind == VK_BITS && !v.width) v.width = g_env[i].v.width;
    if (v.kind == VK_BITS && g_env[i].v.width && v.width != g_env[i].v.width)
        sg_fail(n, "%s is bits(%d), assigned bits(%d)", sg_str(NODE(lhs).name), g_env[i].v.width, v.width);
    /* a STATIC value into a C variable a loop carries keeps it STATIC; a merge promotes it */
    if (v.time == TS && v.kind != VK_ENUM) v.c = fmt("(%s)", v.c);
    g_env[i].v = v;
}

static void ev_let(unsigned n) {
    unsigned pat = NODE(n).a;
    Val v = ev(NODE(n).b);
    int width = 0;
    if (NODE(pat).kind == N_TYPED) {
        width = ascribed_width(NODE(pat).b);
        pat = NODE(pat).a;
    }
    if (NODE(pat).kind == N_WILD) return;
    if (NODE(pat).kind != N_ID) sg_fail(n, "a let of a pattern other than a name");
    if (v.kind == VK_BITS && !v.width) v.width = width;
    if (v.kind == VK_BITS && width && v.width != width) sg_fail(n, "%s is bits(%d), its value bits(%d)", sg_str(NODE(pat).name), width, v.width);
    /* a STATIC value of some length is a C constant of its own */
    if (v.time == TS && strlen(v.c) > 24 && v.kind != VK_STR) {
        const char* t = temp("s");
        emit("const %s %s = %s;", c_type(v), t, v.c);
        v.c = t;
    }
    bind(NODE(pat).name, v, 0);
}

static Val ev_block(unsigned n) {
    unsigned item;
    int mark = g_nenv;
    Val last = v_unit();
    for (item = NODE(n).a; item; item = NODE(item).next) {
        if (g_returned) sg_fail(item, "an item after a return or a throw");
        switch (NODE(item).kind) {
            case N_LET:
                if (NODE(item).c) { /* let ... in e */
                    last = ev(item);
                    break;
                }
                ev_let(item);
                last = v_unit();
                break;
            case N_VAR: {
                Val v = ev(NODE(item).b);
                int width = ascribed_width(NODE(item).c);
                if (v.kind == VK_BITS && !v.width) v.width = width;
                if (v.time == TS) v.c = fmt("(%s)", v.c);
                bind(NODE(item).name, v, 1);
                last = v_unit();
                break;
            }
            case N_ASSIGN:
                ev_assign(item);
                last = v_unit();
                break;
            default: last = ev(item);
        }
    }
    /* the locals of the block end with it; the vars of outer blocks it set stay set */
    g_nenv = mark;
    return last;
}

static Val ev(unsigned n) {
    switch (NODE(n).kind) {
        case N_NUM: return v_int_static(fmt("%llull", (unsigned long long)NODE(n).value));
        case N_BITS: return v_static(VK_BITS, NODE(n).width, fmt("0x%llxull", (unsigned long long)NODE(n).value));
        case N_BOOL: return v_static(VK_BOOL, 0, NODE(n).value ? "1" : "0");
        case N_UNIT: return v_unit();
        case N_ID: {
            int i = lookup(NODE(n).name), index;
            const char* name = sg_str(NODE(n).name);
            unsigned d;
            if (i >= 0) return g_env[i].v;
            if (name[0] == '"') return v_static(VK_STR, 0, name);
            if (is_enum_member(NODE(n).name, &index)) return v_static(VK_ENUM, 0, fmt("SGE_%s", name));
            d = sg_find(name, D_LET);
            if (d) return ev(g_parse.defs[d].body);
            sg_fail(n, "%s is no local, enum member or let", name);
            return v_unit();
        }
        case N_TYPED: {
            Val v = ev(NODE(n).a);
            if (v.kind == VK_BITS && !v.width) v.width = ascribed_width(NODE(n).b);
            return v;
        }
        case N_BINOP: return ev_binop(n);
        case N_NEG: {
            Val a = ev(NODE(n).a);
            if (a.kind != VK_INT || a.time == TD) sg_fail(n, "- of other than an int known when sailgen runs");
            return v_int_static(fmt("(-(int64_t)(%s))", a.c));
        }
        case N_SLICE: return ev_slice(n);
        case N_INDEX: {
            Val x = ev(NODE(n).a), i = ev(NODE(n).b);
            long long k;
            if (x.kind != VK_BITS) sg_fail(n, "an index of a %s", kind_name(x.kind));
            if (x.time == TS) return v_static(VK_BITS, 1, fmt("(((uint64_t)(%s) >> (%s)) & 1)", x.c, i.c));
            if (!const_int(i, &k)) sg_fail(n, "a run-time value's bit at an index not known when sailgen runs");
            return dyn_op("sg_slice", VK_BITS, 1, fmt("%d, %s, %lld, %lld", x.width, x.c, k, k));
        }
        case N_CALL: return ev_call(n);
        case N_IF: return ev_if(n);
        case N_MATCH: return ev_match(n);
        case N_FOREACH: return ev_foreach(n);
        case N_BLOCK: return ev_block(n);
        case N_LET: {
            int mark = g_nenv;
            Val r;
            ev_let(n);
            r = NODE(n).c ? ev(NODE(n).c) : v_unit();
            g_nenv = mark;
            return r;
        }
        case N_RETURN: {
            Val r = ev(NODE(n).a);
            g_returned = 1;
            return r;
        }
        case N_THROW: {
            /* throw(Error(...)): the back end's sg_throw_NAME with the call's arguments; the root ends */
            unsigned e = NODE(n).a, an;
            const char* list = "";
            if (g_dyn_ctl) sg_fail(n, "a throw under a run-time condition");
            if (NODE(e).kind != N_CALL) sg_fail(n, "a throw of other than a call");
            for (an = NODE(e).a; an; an = NODE(an).next) {
                Val a = ev(an);
                if (a.time == TD) sg_fail(n, "a throw of a run-time value");
                /* a message's arguments, as the translators' fail() takes them: unsigned */
                list = fmt("%s, %s%s", list, a.kind == VK_STR ? "" : "(unsigned)", a.c);
            }
            emit("sg_throw_%s(g%s);", sg_str(NODE(e).name), list);
            emit("return;");
            g_returned = 2;
            return v_unit();
        }
        case N_ASSERT: return v_unit();
        case N_TUPLE: sg_fail(n, "a tuple: sailgen --gpu has none"); break;
        default: sg_fail(n, "this expression (kind %d) in GPU mode", NODE(n).kind);
    }
    return v_unit();
}

/* ---- the output ---- */

static void gen_enums(void) {
    unsigned d;
    for (d = 1; d < g_parse.def_count; ++d) {
        unsigned m;
        int i = 0;
        if (g_parse.defs[d].kind != D_ENUM) continue;
        sg_out("/* %s */\nenum {", sg_str(g_parse.defs[d].name));
        for (m = g_parse.defs[d].params; m; m = NODE(m).next, ++i) sg_out("%s SGE_%s = %d", i ? "," : "", sg_str(NODE(m).name), i);
        sg_out(" };\n");
    }
}

static void gen_function(const char* c_name, const char* sail_name, int guard) {
    unsigned d = sg_find(sail_name, D_FUNCTION);
    Buf body = {0};
    Val r;
    if (!d) sg_fail(0, "the root %s has no Sail function", sail_name);
    g_buf = &body;
    g_indent = 1;
    g_nenv = 0;
    g_depth = 0;
    g_dyn_ctl = 0;
    g_returned = 0;
    g_root_node = g_parse.defs[d].body;
    r = ev(g_parse.defs[d].body);
    if (guard) {
        if (r.kind != VK_BOOL || r.time != TS) sg_fail(g_root_node, "%s, a fusion's guard, gives other than a bool known when the translator runs", sail_name);
        emit("return %s;", r.c);
        sg_out("static int %s(SgG* g) {\n%s}\n", c_name, body.text ? body.text : "");
    } else {
        if (r.kind != VK_UNIT) sg_fail(g_root_node, "the root %s gives a value", sail_name);
        sg_out("static void %s(SgG* g) {\n%s}\n", c_name, body.text ? body.text : "");
    }
    free(body.text);
}

int sg_gpu_generate(const char* roots_path, const char* const* backend_paths, int backend_count, const char* out_path) {
    int i, k, nfuse = 0;
    read_roots(roots_path);
    sg_out("/* Generated by sailgen --gpu from %s: do not edit. */\n", roots_path);
    /* the Sail's enums first: the back end's files name their members */
    gen_enums();
    sg_out("static inline int64_t sg_div_int(int64_t a, int64_t b) { return a / b; }\n"
           "static inline int64_t sg_mod_int(int64_t a, int64_t b) { return a %% b; }\n"
           "static inline int64_t sg_pow_int(int64_t a, int64_t b) {\n"
           "    int64_t r = 1;\n"
           "    while (b-- > 0) r *= a;\n"
           "    return r;\n"
           "}\n");
    for (i = 0; i < backend_count; ++i) {
        size_t size;
        char* text = sg_read(backend_paths[i], &size);
        sg_out("%s\n", text);
        free(text);
    }
    for (i = 0; i < g_roots.nroots; ++i) {
        Root* r = &g_roots.roots[i];
        if (r->fuse) {
            gen_function(fmt("%s_fuse_%s_ok", g_roots.prefix, r->name), fmt("%s_ok", r->name), 1);
            gen_function(fmt("%s_fuse_%s", g_roots.prefix, r->name), r->name, 0);
            ++nfuse;
        } else
            gen_function(fmt("%s_%s", g_roots.prefix, r->name), fmt("execute_%s", r->name), 0);
    }
    /* the dispatcher: the root of op, 1 when it ran, 0 when no root takes op */
    sg_out("[[maybe_unused]] static int %s_emit(SgG* g, int op) {\n    switch (op) {\n", g_roots.prefix);
    for (i = 0; i < g_roots.nroots; ++i) {
        Root* r = &g_roots.roots[i];
        if (r->fuse) continue;
        /* each opcode of the root, its place in the line given to the back end (sg_set_form): an ISA whose
           opcodes are one instruction's forms (SM53's _R, _C, _I) reads it as Form() */
        for (k = 0; k < r->nops; ++k)
            sg_out("        case %s:\n            sg_set_form(g, %d);\n            %s_%s(g);\n            return 1;\n", r->ops[k], k,
                   g_roots.prefix, r->name);
    }
    sg_out("        default: return 0;\n    }\n}\n");
    /* whether a root takes op: a translator refuses an opcode none takes before it writes anything */
    sg_out("[[maybe_unused]] static int %s_takes(int op) {\n    switch (op) {\n", g_roots.prefix);
    for (i = 0; i < g_roots.nroots; ++i) {
        Root* r = &g_roots.roots[i];
        if (r->fuse) continue;
        for (k = 0; k < r->nops; ++k) sg_out("        case %s:\n", r->ops[k]);
    }
    sg_out("            return 1;\n        default: return 0;\n    }\n}\n");
    /* the fusions: at the instructions whose opcodes are ops (avail of them, the first the one being translated, none
       after it a branch lands on), the first fusion whose opcodes and guard they meet, run: how many it took, 0 for
       none. The back end's g holds the instructions (sg_fuse_at) */
    sg_out("[[maybe_unused]] static unsigned %s_fuse(SgG* g, const int* ops, unsigned avail) {\n", g_roots.prefix);
    if (!nfuse) sg_out("    (void)g, (void)ops, (void)avail;\n");
    for (i = 0; i < g_roots.nroots; ++i) {
        Root* r = &g_roots.roots[i];
        if (!r->fuse) continue;
        sg_out("    if (avail >= %d", r->nops);
        for (k = 0; k < r->nops; ++k) sg_out(" && ops[%d] == %s", k, r->ops[k]);
        sg_out(" && %s_fuse_%s_ok(g)) {\n        %s_fuse_%s(g);\n        return %d;\n    }\n", g_roots.prefix, r->name,
               g_roots.prefix, r->name, r->nops);
    }
    sg_out("    return 0;\n}\n");
    /* whether a fusion starts with op: a translator asks before it gathers the instructions after it */
    sg_out("[[maybe_unused]] static int %s_fuse_starts(int op) {\n    switch (op) {\n", g_roots.prefix);
    for (i = 0; i < g_roots.nroots; ++i) {
        Root* r = &g_roots.roots[i];
        int j, seen = 0;
        if (!r->fuse) continue;
        for (j = 0; j < i; ++j)
            if (g_roots.roots[j].fuse && !strcmp(g_roots.roots[j].ops[0], r->ops[0])) seen = 1;
        if (!seen) sg_out("        case %s:\n", r->ops[0]);
    }
    sg_out("%s        default: return 0;\n    }\n}\n", nfuse ? "            return 1;\n" : "");
    const int rc = sg_flush(out_path);
    fmt_free();
    return rc;
}
