/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLSL ES tokens into typed syntax nodes. GLSL declares every name before its use, so the parser checks types and
   resolves names in the same pass: each expression node is made with its type, and an operation on scalar constants
   is folded into a GLSLC_N_CONST as it is made, which array sizes, case labels and layout values need. The descent
   counts how deep it is (SNX_GLSLC_MAX_DEPTH), so no source can take the C stack further than that. */
#include "glslc/internal.h"
#include <stdio.h>
#include <string.h>

#define TOKV(g, i)    (int)tk((g), (i))->text.len, glslc_token_text((g), tk((g), (i)))
#define VIEWV(g, v)   (int)(v).len, glslc_view_text((g), (v))
#define TX_EXTRA_BIAS 1u /* a texture call's op: it was given a bias */
#define TX_EXTRA_COMP 2u /* textureGather's component was given */

typedef struct {
    uint32_t tok; /* the first qualifier */
    uint8_t storage;
    uint8_t prec;
    uint8_t legacy; /* 1: attribute, 2: varying */
    uint8_t any;
    uint8_t const_in; /* const in: a parameter's */
    uint16_t qual;
    int16_t location, binding, set;
    uint8_t packing, matrix, format, early, has_layout, local_size_mask;
    uint16_t local_size[3];
} Qual;

static uint32_t parse_assignment(Glslc* g);
static uint32_t parse_expression(Glslc* g);
static uint32_t parse_conditional(Glslc* g);
static uint32_t parse_statement(Glslc* g, uint32_t* last);
static uint32_t parse_type_specifier(Glslc* g, int nested);

static const GlslcToken* tk(const Glslc* g, uint32_t i) { return &g->toks[i < g->ntoks ? i : g->ntoks - 1]; }
static uint8_t kind_at(const Glslc* g, uint32_t ahead) { return tk(g, g->pos + ahead)->kind; }
static uint32_t site(const Glslc* g, uint32_t tok) { return tk(g, tok)->site; }

static uint32_t next_tok(Glslc* g) {
    uint32_t t = g->pos;
    if (tk(g, t)->kind != TK_EOF) g->pos++;
    return t;
}

static int accept(Glslc* g, uint8_t kind) {
    if (kind_at(g, 0) != kind) return 0;
    g->pos++;
    return 1;
}

static const char* version_name(const Glslc* g) {
    static const char* const names[] = {"1.00", "3.00", "3.10", "3.20"};
    return names[g->vi];
}

static void describe(const Glslc* g, uint32_t tok, char* out, size_t size) {
    const GlslcToken* t = tk(g, tok);
    if (t->kind == TK_EOF) {
        snprintf(out, size, "the end of the source");
    } else {
        snprintf(out, size, "'%.*s'", (int)t->text.len, glslc_token_text(g, t));
    }
}

static uint32_t expect(Glslc* g, uint8_t kind, const char* what) {
    if (kind_at(g, 0) != kind) {
        char found[96];
        describe(g, g->pos, found, sizeof found);
        if (kind_at(g, 0) == TK_RESERVED)
            glslc_fail(g, site(g, g->pos), "expected %s, found %s, a word GLSL ES %s reserves", what, found,
                       version_name(g));
        glslc_fail(g, site(g, g->pos), "expected %s, found %s", what, found);
    }
    return g->pos++;
}

static void enter(Glslc* g, uint32_t tok) {
    if (++g->depth > SNX_GLSLC_MAX_DEPTH)
        glslc_fail(g, site(g, tok), "expressions and statements nest more than the %u deep glslc takes",
                   SNX_GLSLC_MAX_DEPTH);
}

static void leave(Glslc* g) { g->depth--; }

/* types */

static const GlslcType* ty(const Glslc* g, uint32_t t) { return &g->types[t]; }
static int is_scalar(const Glslc* g, uint32_t t) { return ty(g, t)->kind == GLSLC_T_SCALAR; }
static int is_vector(const Glslc* g, uint32_t t) { return ty(g, t)->kind == GLSLC_T_VECTOR; }
static int is_matrix(const Glslc* g, uint32_t t) { return ty(g, t)->kind == GLSLC_T_MATRIX; }
static int is_sv(const Glslc* g, uint32_t t) { return is_scalar(g, t) || is_vector(g, t); }
static int is_svm(const Glslc* g, uint32_t t) { return is_sv(g, t) || is_matrix(g, t); }
static int is_numeric(const Glslc* g, uint32_t t) { return is_svm(g, t) && ty(g, t)->base != SNX_GLSLC_BASE_BOOL; }
static int is_integer(const Glslc* g, uint32_t t) {
    return is_sv(g, t) && (ty(g, t)->base == SNX_GLSLC_BASE_INT || ty(g, t)->base == SNX_GLSLC_BASE_UINT);
}
static int is_opaque_kind(uint8_t kind) {
    return kind == GLSLC_T_SAMPLER || kind == GLSLC_T_IMAGE || kind == GLSLC_T_ATOMIC;
}
static int has_opaque(const Glslc* g, uint32_t t) {
    return is_opaque_kind(ty(g, t)->kind) || ((ty(g, t)->tflags & GLSLC_TF_OPAQUE) && ty(g, t)->kind == GLSLC_T_ARRAY);
}
static uint32_t element_of(const Glslc* g, uint32_t t) {
    while (ty(g, t)->kind == GLSLC_T_ARRAY) t = ty(g, t)->elem;
    return t;
}

uint32_t glslc_vector_type(uint32_t base, uint32_t rows) {
    return rows == 1 ? GT_BOOL + base : GT_BVEC2 + base * 3u + rows - 2u;
}

uint32_t glslc_matrix_type(uint32_t cols, uint32_t rows) { return GT_MAT2 + (cols - 2u) * 3u + rows - 2u; }

uint32_t glslc_type_components(const Glslc* g, uint32_t t) {
    const GlslcType* y = ty(g, t);
    if (y->kind == GLSLC_T_SCALAR) return 1;
    if (y->kind == GLSLC_T_VECTOR) return y->rows;
    if (y->kind == GLSLC_T_MATRIX) return (uint32_t)y->rows * y->cols;
    return 0;
}

void glslc_type_name(const Glslc* g, uint32_t t, char* out, size_t size) {
    uint32_t dims[8], n = 0;
    while (ty(g, t)->kind == GLSLC_T_ARRAY && n < 8) {
        dims[n++] = ty(g, t)->len;
        t = ty(g, t)->elem;
    }
    const GlslcType* y = ty(g, t);
    int w;
    if (t >= 1 && t <= glslc_builtin_type_count) {
        w = snprintf(out, size, "%s", glslc_builtin_types[t - 1].name);
    } else if (y->name.len) {
        w = snprintf(out, size, "%.*s", VIEWV(g, y->name));
    } else {
        w = snprintf(out, size, "an unnamed struct");
    }
    for (uint32_t i = 0; i < n && w >= 0 && (size_t)w < size; i++) {
        int k =
            dims[i] ? snprintf(out + w, size - (size_t)w, "[%u]", dims[i]) : snprintf(out + w, size - (size_t)w, "[]");
        if (k < 0) break;
        w += k;
    }
}

static uint32_t new_type(Glslc* g, uint32_t tok) {
    if (g->ntypes >= SNX_GLSLC_MAX_TYPES)
        glslc_fail(g, site(g, tok),
                   "the shader needs more than the %u types glslc takes (built-in ones, arrays, structs "
                   "and blocks)",
                   SNX_GLSLC_MAX_TYPES);
    uint32_t t = g->ntypes++;
    memset(&g->types[t], 0, sizeof g->types[t]);
    return t;
}

static uint32_t array_of(Glslc* g, uint32_t elem, uint32_t len, uint32_t tok) {
    for (uint32_t t = glslc_builtin_type_count + 1; t < g->ntypes; t++) {
        if (g->types[t].kind == GLSLC_T_ARRAY && g->types[t].elem == elem && g->types[t].len == len) return t;
    }
    uint32_t t = new_type(g, tok);
    g->types[t].kind = GLSLC_T_ARRAY;
    g->types[t].elem = (uint16_t)elem;
    g->types[t].len = (uint16_t)len;
    g->types[t].base = g->types[elem].base;
    g->types[t].tflags = has_opaque(g, elem) ? GLSLC_TF_OPAQUE : 0;
    return t;
}

/* symbols */

static uint32_t bucket(const Glslc* g, GlslcView name) {
    return glslc_hash(glslc_view_text(g, name), name.len) & (GLSLC_SYM_BUCKETS - 1u);
}

static uint32_t lookup(const Glslc* g, GlslcView name) {
    for (uint32_t s = g->sym_hash[bucket(g, name)]; s; s = g->syms[s].next) {
        if (glslc_views_equal(g, g->syms[s].name, name)) return s;
    }
    return 0;
}

static uint32_t add_symbol(Glslc* g, GlslcView name, uint8_t kind, uint32_t at, int link) {
    if (g->nsyms >= SNX_GLSLC_MAX_SYMBOLS)
        glslc_fail(g, at, "the shader declares more than the %u names glslc takes", SNX_GLSLC_MAX_SYMBOLS - 1);
    uint32_t s = g->nsyms++;
    GlslcSymbol* y = &g->syms[s];
    memset(y, 0, sizeof *y);
    y->name = name;
    y->kind = kind;
    y->location = y->binding = y->set = -1;
    if (link) {
        uint32_t b = bucket(g, name);
        y->next = g->sym_hash[b];
        g->sym_hash[b] = (uint16_t)s;
    }
    return s;
}

static void check_name(Glslc* g, GlslcView name, uint32_t at) {
    const char* text = glslc_view_text(g, name);
    if (name.len >= 3 && !memcmp(text, "gl_", 3))
        glslc_fail(g, at, "'%.*s': names starting with gl_ are GLSL's own", VIEWV(g, name));
    for (uint32_t i = 0; i + 1 < name.len; i++) {
        if (text[i] == '_' && text[i + 1] == '_') { /* reserved: an error in 1.00, a warning from 3.00 */
            if (g->vi == GLSLC_V100)
                glslc_fail(g, at, "'%.*s': GLSL ES 1.00 reserves names with two underscores in a row", VIEWV(g, name));
            glslc_warn(g, at, "'%.*s': names with two underscores in a row are reserved", VIEWV(g, name));
            break;
        }
    }
}

/* name declared in the innermost scope, which must not have it yet (a function may have overloads) */
static uint32_t declare(Glslc* g, GlslcView name, uint32_t at, uint8_t kind) {
    check_name(g, name, at);
    uint32_t start = g->scopes[g->nscopes - 1];
    for (uint32_t s = g->sym_hash[bucket(g, name)]; s; s = g->syms[s].next) {
        if (s < start || !glslc_views_equal(g, g->syms[s].name, name)) continue;
        if (kind == GLSLC_SK_FUNC && g->syms[s].kind == GLSLC_SK_FUNC) continue;
        glslc_fail(g, at, "'%.*s' is already declared in this scope", VIEWV(g, name));
    }
    return add_symbol(g, name, kind, at, 1);
}

static void push_scope(Glslc* g, uint32_t tok) {
    if (g->nscopes >= SNX_GLSLC_MAX_SCOPES)
        glslc_fail(g, site(g, tok), "scopes nest more than the %u deep glslc takes", SNX_GLSLC_MAX_SCOPES);
    g->scopes[g->nscopes++] = (uint16_t)g->nsyms;
}

static void pop_scope(Glslc* g) {
    uint32_t start = g->scopes[--g->nscopes];
    for (uint32_t s = g->nsyms; s-- > start;) {
        uint32_t b = bucket(g, g->syms[s].name);
        if (g->sym_hash[b] == s) g->sym_hash[b] = g->syms[s].next; /* only a linked symbol is ever a head */
    }
    while (g->nprecs && g->precs[g->nprecs - 1].scope > g->nscopes) g->nprecs--;
}

/* precision */

static uint32_t precision_key(const Glslc* g, uint32_t t) {
    t = element_of(g, t);
    const GlslcType* y = ty(g, t);
    if (is_svm(g, t)) {
        if (y->base == SNX_GLSLC_BASE_FLOAT) return GT_FLOAT;
        if (y->base == SNX_GLSLC_BASE_INT || y->base == SNX_GLSLC_BASE_UINT) return GT_INT;
        return 0;
    }
    return is_opaque_kind(y->kind) ? t : 0;
}

static uint8_t default_precision(const Glslc* g, uint32_t key) {
    for (uint32_t i = g->nprecs; i-- > 0;) {
        if (g->precs[i].type == key) return g->precs[i].prec;
    }
    int frag = g->stage == SNX_GLSLC_FRAGMENT;
    if (key == GT_FLOAT) return frag ? GLSLC_PREC_NONE : GLSLC_PREC_HIGH;
    if (key == GT_INT) return frag ? GLSLC_PREC_MEDIUM : GLSLC_PREC_HIGH;
    if (key == GT_SAMPLER_FIRST || key == GT_SAMPLER_FIRST + 1) return GLSLC_PREC_LOW; /* sampler2D, samplerCube */
    if (ty(g, key)->kind == GLSLC_T_ATOMIC) return GLSLC_PREC_HIGH;
    return GLSLC_PREC_NONE;
}

/* the precision a declaration of type t has: the one given, else the default; a type that needs one and has
   neither is an error */
static uint8_t resolve_precision(Glslc* g, uint32_t t, uint8_t given, uint32_t tok) {
    uint32_t key = precision_key(g, t);
    if (!key) return given;
    if (given) return given;
    uint8_t p = default_precision(g, key);
    if (!p) {
        char name[64];
        glslc_type_name(g, key, name, sizeof name);
        glslc_fail(g, site(g, tok),
                   "%s has no precision here: give it lowp, mediump or highp, or set a default with 'precision "
                   "mediump %s;'",
                   name, name);
    }
    return p;
}

/* nodes and constants */

static uint32_t node(Glslc* g, uint8_t kind, uint32_t tok, uint32_t type) {
    if (g->nnodes >= SNX_GLSLC_MAX_NODES)
        glslc_fail(g, site(g, tok), "the shader needs more than the %u syntax nodes glslc takes", SNX_GLSLC_MAX_NODES);
    uint32_t n = g->nnodes++;
    GlslcNode* x = &g->nodes[n];
    memset(x, 0, sizeof *x);
    x->kind = kind;
    x->tok = (uint16_t)tok;
    x->type = (uint16_t)type;
    return n;
}

static GlslcNode* nd(Glslc* g, uint32_t n) { return &g->nodes[n]; }
static int is_const(const Glslc* g, uint32_t n) { return (g->nodes[n].flags & GLSLC_NF_CONST) != 0; }
static uint32_t bits_of(const Glslc* g, uint32_t n) { return g->nodes[n].a | ((uint32_t)g->nodes[n].b << 16); }

static float f32(uint32_t bits) {
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static uint32_t u32(float f) {
    uint32_t bits;
    memcpy(&bits, &f, sizeof bits);
    return bits;
}

static uint32_t make_const(Glslc* g, uint32_t tok, uint32_t type, uint32_t bits) {
    uint32_t n = node(g, GLSLC_N_CONST, tok, type);
    nd(g, n)->a = (uint16_t)(bits & 0xffffu);
    nd(g, n)->b = (uint16_t)(bits >> 16);
    nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

/* a decimal literal as the nearest float, the same in every locale (strtod reads the C locale's decimal point) */
static uint32_t parse_float(const char* s, uint32_t len) {
    static const double tens[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                  1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
    uint64_t mantissa = 0;
    int digits = 0, exp10 = 0;
    uint32_t i = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') {
        if (digits < 19) {
            mantissa = mantissa * 10u + (uint64_t)(s[i] - '0');
            if (mantissa) digits++;
        } else {
            exp10++;
        }
        i++;
    }
    if (i < len && s[i] == '.') {
        i++;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (digits < 19) {
                mantissa = mantissa * 10u + (uint64_t)(s[i] - '0');
                if (mantissa) digits++;
                exp10--;
            }
            i++;
        }
    }
    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        int sign = 1, e = 0;
        i++;
        if (i < len && (s[i] == '+' || s[i] == '-')) sign = s[i++] == '-' ? -1 : 1;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (e < 100000) e = e * 10 + (s[i] - '0');
            i++;
        }
        exp10 += sign * e;
    }
    double v = (double)mantissa;
    while (exp10 > 22 && v != 0.0 && v < 1e300) {
        v *= 1e22;
        exp10 -= 22;
    }
    while (exp10 < -22 && v != 0.0 && v > 1e-300) {
        v /= 1e22;
        exp10 += 22;
    }
    if (exp10 > 22) {
        v = v == 0.0 ? 0.0 : 1e300 * 1e300; /* past every float: infinity */
    } else if (exp10 < -22) {
        v = 0.0;
    } else if (exp10 >= 0) {
        v *= tens[exp10];
    } else {
        v /= tens[-exp10];
    }
    return u32((float)v);
}

uint32_t glslc_fold_convert(const Glslc* g, uint32_t bits, uint32_t from, uint32_t to) {
    uint32_t fb = ty(g, from)->base, tb = ty(g, to)->base;
    if (fb == tb) return bits;
    if (tb == SNX_GLSLC_BASE_BOOL) return fb == SNX_GLSLC_BASE_FLOAT ? f32(bits) != 0.0f : bits != 0;
    if (fb == SNX_GLSLC_BASE_BOOL) return tb == SNX_GLSLC_BASE_FLOAT ? u32(bits ? 1.0f : 0.0f) : (bits ? 1u : 0u);
    if (tb == SNX_GLSLC_BASE_FLOAT) return u32(fb == SNX_GLSLC_BASE_INT ? (float)(int32_t)bits : (float)bits);
    if (fb == SNX_GLSLC_BASE_FLOAT) {
        float f = f32(bits);
        if (f != f) return 0;
        if (tb == SNX_GLSLC_BASE_INT) {
            if (f >= 2147483647.0f) return 0x7fffffffu;
            if (f <= -2147483648.0f) return 0x80000000u;
            return (uint32_t)(int32_t)f;
        }
        if (f <= 0.0f) return 0;
        if (f >= 4294967295.0f) return 0xffffffffu;
        return (uint32_t)f;
    }
    return bits; /* int and uint: the same bits */
}

/* a constant scalar for op on l and r (both GLSLC_N_CONST of type lt), or 0 to leave it to run time (a division by
   zero, a shift past the width) */
static int fold_binary(const Glslc* g, uint8_t op, uint32_t lt, uint32_t a, uint32_t b, uint32_t* out) {
    uint32_t base = ty(g, lt)->base;
    int32_t ia = (int32_t)a, ib = (int32_t)b;
    float fa = f32(a), fb = f32(b);
    int fl = base == SNX_GLSLC_BASE_FLOAT, si = base == SNX_GLSLC_BASE_INT;
    switch (op) {
        case GLSLC_OP_ADD: *out = fl ? u32(fa + fb) : a + b; return 1;
        case GLSLC_OP_SUB: *out = fl ? u32(fa - fb) : a - b; return 1;
        case GLSLC_OP_MUL: *out = fl ? u32(fa * fb) : a * b; return 1;
        case GLSLC_OP_DIV:
            if (fl) {
                *out = u32(fa / fb);
                return 1;
            }
            if (b == 0) return 0;
            if (si) {
                *out = (ia == INT32_MIN && ib == -1) ? a : (uint32_t)(ia / ib);
            } else {
                *out = a / b;
            }
            return 1;
        case GLSLC_OP_MOD:
            if (b == 0) return 0;
            if (si) {
                *out = (ib == -1) ? 0u : (uint32_t)(ia % ib);
            } else {
                *out = a % b;
            }
            return 1;
        case GLSLC_OP_SHL:
            if (b > 31) return 0;
            *out = a << b;
            return 1;
        case GLSLC_OP_SHR:
            if (b > 31) return 0;
            *out = si && (a & 0x80000000u) ? ~(~a >> b) : a >> b;
            return 1;
        case GLSLC_OP_AND: *out = a & b; return 1;
        case GLSLC_OP_OR: *out = a | b; return 1;
        case GLSLC_OP_XOR: *out = a ^ b; return 1;
        case GLSLC_OP_LT: *out = fl ? fa < fb : si ? ia < ib : a < b; return 1;
        case GLSLC_OP_GT: *out = fl ? fa > fb : si ? ia > ib : a > b; return 1;
        case GLSLC_OP_LE: *out = fl ? fa <= fb : si ? ia <= ib : a <= b; return 1;
        case GLSLC_OP_GE: *out = fl ? fa >= fb : si ? ia >= ib : a >= b; return 1;
        case GLSLC_OP_EQ: *out = fl ? fa == fb : a == b; return 1;
        case GLSLC_OP_NE: *out = fl ? fa != fb : a != b; return 1;
        case GLSLC_OP_LOGIC_AND: *out = a && b; return 1;
        case GLSLC_OP_LOGIC_OR: *out = a || b; return 1;
        case GLSLC_OP_LOGIC_XOR: *out = (a != 0) != (b != 0); return 1;
        default: return 0;
    }
}

/* component i of the constant vector or scalar expression n, when glslc can tell it */
static int const_component(const Glslc* g, uint32_t n, uint32_t i, uint32_t* out) {
    for (uint32_t hops = 0; hops < 64; hops++) {
        const GlslcNode* x = &g->nodes[n];
        if (x->kind == GLSLC_N_CONST) {
            if (i != 0) return 0;
            *out = bits_of(g, n);
            return 1;
        }
        if (x->kind == GLSLC_N_VAR) {
            const GlslcSymbol* s = &g->syms[x->a];
            if (s->kind == GLSLC_SK_BUILTIN && glslc_builtin_vars[s->aux].value == GLSLC_BUILTIN_VALUE_LOCAL_SIZE) {
                if (i > 2) return 0;
                *out = g->local_size[i];
                return 1;
            }
            if (s->storage != GLSLC_ST_CONST || !s->node || !g->nodes[s->node].b) return 0;
            n = g->nodes[s->node].b;
            continue;
        }
        if (x->kind != GLSLC_N_CONSTRUCT || !(x->flags & GLSLC_NF_CONST) || !is_vector(g, x->type)) return 0;
        uint32_t a = x->a;
        if (!g->nodes[a].next && is_scalar(g, g->nodes[a].type)) { /* a vector of one scalar */
            if (!const_component(g, a, 0, out)) return 0;
            *out = glslc_fold_convert(g, *out, g->nodes[a].type, x->type);
            return 1;
        }
        for (; a; a = g->nodes[a].next) {
            uint32_t c = glslc_type_components(g, g->nodes[a].type);
            if (i < c) {
                if (!is_sv(g, g->nodes[a].type) || !const_component(g, a, i, out)) return 0;
                *out = glslc_fold_convert(g, *out, glslc_vector_type(ty(g, g->nodes[a].type)->base, 1),
                                          glslc_vector_type(ty(g, x->type)->base, 1));
                return 1;
            }
            i -= c;
        }
        return 0;
    }
    return 0;
}

/* lvalues */

/* the symbol written through n, or 0 with *why saying what makes n read-only */
static uint32_t lvalue_root(Glslc* g, uint32_t n, const char** why) {
    for (;;) {
        const GlslcNode* x = &g->nodes[n];
        switch (x->kind) {
            case GLSLC_N_INDEX:
            case GLSLC_N_FIELD:
                if (x->kind == GLSLC_N_FIELD) {
                    const GlslcType* bt = ty(g, g->nodes[x->a].type);
                    if (bt->kind == GLSLC_T_BLOCK && (g->members[bt->first + x->b].mflags & GLSLC_MEM_READONLY)) {
                        *why = "a readonly block member";
                        return 0;
                    }
                }
                n = x->a;
                continue;
            case GLSLC_N_SWIZZLE:
                for (uint32_t i = 0; i < x->op; i++) {
                    for (uint32_t j = i + 1; j < x->op; j++) {
                        if (((x->b >> (2 * i)) & 3u) == ((x->b >> (2 * j)) & 3u)) {
                            *why = "a swizzle that names a component twice";
                            return 0;
                        }
                    }
                }
                n = x->a;
                continue;
            case GLSLC_N_VAR: {
                uint32_t s = x->a;
                const GlslcSymbol* y = &g->syms[s];
                if (y->kind == GLSLC_SK_BLOCK_MEMBER) {
                    const GlslcType* bt = ty(g, element_of(g, g->syms[y->aux].type));
                    if (g->members[bt->first + y->aux2].mflags & GLSLC_MEM_READONLY) {
                        *why = "a readonly block member";
                        return 0;
                    }
                }
                switch (y->storage) {
                    case GLSLC_ST_CONST: *why = "a constant"; return 0;
                    case GLSLC_ST_UNIFORM: *why = "a uniform"; return 0;
                    case GLSLC_ST_IN: *why = "an input"; return 0;
                    default: break;
                }
                if (y->qual & GLSLC_Q_CONST) {
                    *why = "a const parameter";
                    return 0;
                }
                if (y->qual & GLSLC_Q_READONLY) {
                    *why = "readonly";
                    return 0;
                }
                return s;
            }
            default: *why = "a value that is not a variable"; return 0;
        }
    }
}

static uint32_t need_lvalue(Glslc* g, uint32_t n, uint32_t tok, const char* what) {
    const char* why = "";
    uint32_t s = lvalue_root(g, n, &why);
    if (!s) glslc_fail(g, site(g, tok), "%s cannot write to %s", what, why);
    return s;
}

/* qualifiers */

static int same_word(const char* text, uint32_t len, const char* word) {
    size_t n = strlen(word);
    if (len != n) return 0;
    for (uint32_t i = 0; i < len; i++) {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c != word[i]) return 0;
    }
    return 1;
}

static void need_version(Glslc* g, uint32_t tok, uint8_t vi, const char* what) {
    static const char* const names[] = {"1.00", "3.00", "3.10", "3.20"};
    if (g->vi < vi)
        glslc_fail(g, site(g, tok), "%s needs GLSL ES %s, and this shader is GLSL ES %s", what, names[vi],
                   version_name(g));
}

static void parse_layout(Glslc* g, Qual* q) {
    uint32_t lt = next_tok(g);
    need_version(g, lt, GLSLC_V300, "layout(...)");
    q->any = 1;
    q->has_layout = 1;
    expect(g, TK_LPAREN, "'(' after layout");
    for (;;) {
        uint32_t id = g->pos;
        if (kind_at(g, 0) != TK_IDENT && kind_at(g, 0) != TK_SHARED) {
            char found[96];
            describe(g, id, found, sizeof found);
            glslc_fail(g, site(g, id), "expected a layout qualifier, found %s", found);
        }
        g->pos++;
        const char* text = glslc_token_text(g, tk(g, id));
        uint32_t len = tk(g, id)->text.len;
        int has_value = 0;
        uint32_t value = 0;
        if (accept(g, TK_ASSIGN)) {
            uint32_t e = parse_conditional(g);
            if (nd(g, e)->kind != GLSLC_N_CONST || !(nd(g, e)->type == GT_INT || nd(g, e)->type == GT_UINT))
                glslc_fail(g, site(g, nd(g, e)->tok), "layout %.*s takes a constant integer", TOKV(g, id));
            value = bits_of(g, e);
            if ((int32_t)value < 0 || value > 32767)
                glslc_fail(g, site(g, nd(g, e)->tok), "layout %.*s = %d: it takes 0 to 32767", TOKV(g, id),
                           (int)(int32_t)value);
            has_value = 1;
        }
        int wants_value = same_word(text, len, "location") || same_word(text, len, "binding") ||
                          same_word(text, len, "set") || same_word(text, len, "local_size_x") ||
                          same_word(text, len, "local_size_y") || same_word(text, len, "local_size_z");
        if (wants_value && !has_value) glslc_fail(g, site(g, id), "layout %.*s needs '= value'", TOKV(g, id));
        if (!wants_value && has_value) glslc_fail(g, site(g, id), "layout %.*s takes no value", TOKV(g, id));
        if (same_word(text, len, "location")) {
            q->location = (int16_t)value;
        } else if (same_word(text, len, "binding")) {
            need_version(g, id, GLSLC_V310, "layout(binding)");
            q->binding = (int16_t)value;
        } else if (same_word(text, len, "set")) {
            need_version(g, id, GLSLC_V310, "layout(set)");
            q->set = (int16_t)value;
        } else if (same_word(text, len, "std140")) {
            q->packing = GLSLC_PACK_STD140;
        } else if (same_word(text, len, "std430")) {
            need_version(g, id, GLSLC_V310, "layout(std430)");
            q->packing = GLSLC_PACK_STD430;
        } else if (same_word(text, len, "shared")) {
            q->packing = GLSLC_PACK_SHARED;
        } else if (same_word(text, len, "packed")) {
            q->packing = GLSLC_PACK_PACKED;
        } else if (same_word(text, len, "row_major")) {
            q->matrix = 1;
        } else if (same_word(text, len, "column_major")) {
            q->matrix = 2;
        } else if (same_word(text, len, "local_size_x") || same_word(text, len, "local_size_y") ||
                   same_word(text, len, "local_size_z")) {
            uint32_t axis = (uint32_t)(text[len - 1] | 0x20) - 'x';
            if (g->stage != SNX_GLSLC_COMPUTE)
                glslc_fail(g, site(g, id), "layout %.*s is only for compute shaders", TOKV(g, id));
            if (value == 0) glslc_fail(g, site(g, id), "layout %.*s = 0: a workgroup is 1 or more wide", TOKV(g, id));
            q->local_size[axis] = (uint16_t)value;
            q->local_size_mask |= (uint8_t)(1u << axis);
        } else if (same_word(text, len, "early_fragment_tests")) {
            need_version(g, id, GLSLC_V310, "layout(early_fragment_tests)");
            if (g->stage != SNX_GLSLC_FRAGMENT)
                glslc_fail(g, site(g, id), "layout(early_fragment_tests) is only for fragment shaders");
            q->early = 1;
        } else {
            uint32_t f;
            for (f = 0; f < glslc_format_count; f++) {
                if (same_word(text, len, glslc_format_names[f])) break;
            }
            if (f == glslc_format_count)
                glslc_fail(g, site(g, id), "'%.*s' is not a layout qualifier of GLSL ES", TOKV(g, id));
            need_version(g, id, GLSLC_V310, "an image format");
            q->format = (uint8_t)(f + 1);
        }
        if (!accept(g, TK_COMMA)) break;
    }
    expect(g, TK_RPAREN, "')' at the end of layout(...)");
}

static void set_storage(Glslc* g, Qual* q, uint8_t storage, uint32_t tok) {
    if (q->storage == GLSLC_ST_CONST && storage == GLSLC_ST_IN && !q->const_in) {
        q->const_in = 1;
        return;
    }
    if (q->storage)
        glslc_fail(g, site(g, tok), "'%.*s' is a second storage qualifier: a declaration takes one", TOKV(g, tok));
    q->storage = storage;
}

static void set_flag(Glslc* g, Qual* q, uint16_t flag, uint32_t tok) {
    if (q->qual & flag) glslc_fail(g, site(g, tok), "'%.*s' is given twice", TOKV(g, tok));
    q->qual |= flag;
}

static void parse_qualifiers(Glslc* g, Qual* q) {
    memset(q, 0, sizeof *q);
    q->location = q->binding = q->set = -1;
    q->tok = g->pos;
    for (;;) {
        uint32_t t = g->pos;
        switch (kind_at(g, 0)) {
            case TK_CONST: set_storage(g, q, GLSLC_ST_CONST, t); break;
            case TK_IN: set_storage(g, q, GLSLC_ST_IN, t); break;
            case TK_OUT: set_storage(g, q, GLSLC_ST_OUT, t); break;
            case TK_INOUT: set_storage(g, q, GLSLC_ST_PARAM_INOUT, t); break;
            case TK_UNIFORM: set_storage(g, q, GLSLC_ST_UNIFORM, t); break;
            case TK_BUFFER: set_storage(g, q, GLSLC_ST_BUFFER, t); break;
            case TK_SHARED: set_storage(g, q, GLSLC_ST_SHARED, t); break;
            case TK_ATTRIBUTE:
                set_storage(g, q, GLSLC_ST_IN, t);
                q->legacy = 1;
                break;
            case TK_VARYING:
                set_storage(g, q, g->stage == SNX_GLSLC_VERTEX ? GLSLC_ST_OUT : GLSLC_ST_IN, t);
                q->legacy = 2;
                break;
            case TK_CENTROID: set_flag(g, q, GLSLC_Q_CENTROID, t); break;
            case TK_FLAT: set_flag(g, q, GLSLC_Q_FLAT, t); break;
            case TK_SMOOTH: set_flag(g, q, GLSLC_Q_SMOOTH, t); break;
            case TK_INVARIANT: set_flag(g, q, GLSLC_Q_INVARIANT, t); break;
            case TK_PRECISE: set_flag(g, q, GLSLC_Q_PRECISE, t); break;
            case TK_SAMPLE: set_flag(g, q, GLSLC_Q_SAMPLE, t); break;
            case TK_COHERENT: set_flag(g, q, GLSLC_Q_COHERENT, t); break;
            case TK_VOLATILE: set_flag(g, q, GLSLC_Q_VOLATILE, t); break;
            case TK_RESTRICT: set_flag(g, q, GLSLC_Q_RESTRICT, t); break;
            case TK_READONLY: set_flag(g, q, GLSLC_Q_READONLY, t); break;
            case TK_WRITEONLY: set_flag(g, q, GLSLC_Q_WRITEONLY, t); break;
            case TK_LOWP:
            case TK_MEDIUMP:
            case TK_HIGHP:
                if (q->prec) glslc_fail(g, site(g, t), "'%.*s' is a second precision qualifier", TOKV(g, t));
                q->prec = kind_at(g, 0) == TK_LOWP      ? GLSLC_PREC_LOW
                          : kind_at(g, 0) == TK_MEDIUMP ? GLSLC_PREC_MEDIUM
                                                        : GLSLC_PREC_HIGH;
                break;
            case TK_LAYOUT: parse_layout(g, q); continue;
            default: return;
        }
        q->any = 1;
        g->pos++;
    }
}

/* types in declarations */

/* [n] or [] after a type or a name, any number of them, the first the outermost; only the outermost may be
   unsized, and the declaration says whether that is allowed */
static uint32_t parse_array_suffix(Glslc* g, uint32_t base) {
    uint32_t dims[8], toks[8], n = 0;
    while (kind_at(g, 0) == TK_LBRACKET) {
        uint32_t b = next_tok(g);
        if (n == 8) glslc_fail(g, site(g, b), "an array has more than the 8 dimensions glslc takes");
        if (n >= 1 || ty(g, base)->kind == GLSLC_T_ARRAY) need_version(g, b, GLSLC_V310, "an array of arrays");
        dims[n] = 0;
        if (kind_at(g, 0) != TK_RBRACKET) {
            uint32_t e = parse_conditional(g);
            uint32_t et = nd(g, e)->type;
            if (nd(g, e)->kind != GLSLC_N_CONST || !(et == GT_INT || et == GT_UINT))
                glslc_fail(g, site(g, nd(g, e)->tok),
                           "an array size is a constant integer expression glslc can fold (literals, constants and "
                           "operators on them)");
            int32_t v = (int32_t)bits_of(g, e);
            if (v <= 0 || v > 65535)
                glslc_fail(g, site(g, nd(g, e)->tok), "an array of %d elements: an array has 1 to 65535", (int)v);
            dims[n] = (uint32_t)v;
        }
        expect(g, TK_RBRACKET, "']'");
        toks[n] = b;
        n++;
    }
    if (n && ty(g, base)->kind == GLSLC_T_ARRAY && ty(g, base)->len == 0)
        glslc_fail(g, site(g, toks[0]), "an unsized array cannot be an array's element");
    uint32_t t = base;
    for (uint32_t i = n; i-- > 0;) {
        if (!dims[i] && i != 0) glslc_fail(g, site(g, toks[i]), "only the first dimension of an array may be unsized");
        t = array_of(g, t, dims[i], toks[i]);
    }
    return t;
}

static uint32_t parse_struct(Glslc* g, int nested) {
    uint32_t st = next_tok(g);
    if (nested) glslc_fail(g, site(g, st), "a struct cannot be defined inside a struct or block");
    uint32_t name_tok = 0;
    if (kind_at(g, 0) == TK_IDENT) name_tok = next_tok(g);
    expect(g, TK_LBRACE, "'{' after struct");
    uint32_t t = new_type(g, st);
    g->types[t].kind = GLSLC_T_STRUCT;
    g->types[t].base = SNX_GLSLC_BASE_STRUCT;
    if (name_tok) g->types[t].name = tk(g, name_tok)->text;
    g->types[t].first = (uint16_t)g->nmembers;
    uint32_t count = 0;
    while (kind_at(g, 0) != TK_RBRACE) {
        Qual q;
        parse_qualifiers(g, &q);
        if (q.storage || q.qual || q.has_layout)
            glslc_fail(g, site(g, q.tok), "a struct member takes no qualifier but a precision");
        uint32_t mt = parse_type_specifier(g, 1);
        do {
            uint32_t mn = expect(g, TK_IDENT, "a member name");
            uint32_t full = parse_array_suffix(g, mt);
            char name[64];
            glslc_type_name(g, full, name, sizeof name);
            if (element_of(g, full) == GT_VOID) glslc_fail(g, site(g, mn), "member %.*s cannot be void", TOKV(g, mn));
            if (ty(g, full)->kind == GLSLC_T_ARRAY && ty(g, full)->len == 0)
                glslc_fail(g, site(g, mn), "member %.*s is an array with no size", TOKV(g, mn));
            if (has_opaque(g, full))
                glslc_fail(g, site(g, mn),
                           "member %.*s is %s: glslc compiles no sampler or image inside a struct (Vulkan has no "
                           "such member)",
                           TOKV(g, mn), name);
            for (uint32_t m = g->types[t].first; m < g->nmembers; m++) {
                if (glslc_views_equal(g, g->members[m].name, tk(g, mn)->text))
                    glslc_fail(g, site(g, mn), "the struct has two members named %.*s", TOKV(g, mn));
            }
            if (g->nmembers >= SNX_GLSLC_MAX_MEMBERS)
                glslc_fail(g, site(g, mn), "the structs and blocks have more than the %u members glslc takes",
                           SNX_GLSLC_MAX_MEMBERS - 1);
            GlslcMember* m = &g->members[g->nmembers++];
            memset(m, 0, sizeof *m);
            m->name = tk(g, mn)->text;
            m->type = (uint16_t)full;
            m->prec = resolve_precision(g, full, q.prec, mn);
            m->location = -1;
            count++;
        } while (accept(g, TK_COMMA));
        expect(g, TK_SEMICOLON, "';' after a struct member");
        if (kind_at(g, 0) == TK_EOF) expect(g, TK_RBRACE, "'}' at the end of the struct");
    }
    g->pos++;
    if (!count) glslc_fail(g, site(g, st), "a struct needs at least one member");
    g->types[t].len = (uint16_t)count;
    if (name_tok) {
        uint32_t s = declare(g, tk(g, name_tok)->text, site(g, name_tok), GLSLC_SK_STRUCT);
        g->syms[s].type = (uint16_t)t;
    }
    return t;
}

static int is_struct_name(const Glslc* g, uint32_t tok) {
    if (tk(g, tok)->kind != TK_IDENT) return 0;
    uint32_t s = lookup(g, tk(g, tok)->text);
    return s && g->syms[s].kind == GLSLC_SK_STRUCT;
}

static uint32_t parse_type_specifier(Glslc* g, int nested) {
    uint32_t t0 = g->pos;
    uint8_t k = kind_at(g, 0);
    uint32_t t;
    if (k >= TK_TYPE) {
        t = (uint32_t)(k - TK_TYPE) + 1u;
        g->pos++;
        if (ty(g, t)->kind == GLSLC_T_ATOMIC)
            glslc_fail(g, site(g, t0),
                       "atomic_uint has no Vulkan form: glslc compiles no atomic counter (a buffer "
                       "block's uint and atomicAdd do the same)");
    } else if (k == TK_STRUCT) {
        t = parse_struct(g, nested);
    } else if (is_struct_name(g, t0)) {
        t = g->syms[lookup(g, tk(g, t0)->text)].type;
        g->pos++;
    } else {
        char found[96];
        describe(g, t0, found, sizeof found);
        if (k == TK_RESERVED) glslc_fail(g, site(g, t0), "%s is a word GLSL ES %s reserves", found, version_name(g));
        if (k == TK_IDENT) glslc_fail(g, site(g, t0), "%s is not a type", found);
        glslc_fail(g, site(g, t0), "expected a type, found %s", found);
        return 0;
    }
    if (kind_at(g, 0) == TK_LBRACKET) {
        need_version(g, g->pos, GLSLC_V300, "an array type ('float[2] a')");
        t = parse_array_suffix(g, t);
    }
    return t;
}

/* expressions */

static uint32_t binary_type(const Glslc* g, uint8_t op, uint32_t l, uint32_t r) {
    const GlslcType *a = ty(g, l), *b = ty(g, r);
    switch (op) {
        case GLSLC_OP_ADD:
        case GLSLC_OP_SUB:
        case GLSLC_OP_MUL:
        case GLSLC_OP_DIV:
            if (!is_numeric(g, l) || !is_numeric(g, r) || a->base != b->base) return 0;
            if (op == GLSLC_OP_MUL && !is_scalar(g, l) && !is_scalar(g, r) && (is_matrix(g, l) || is_matrix(g, r))) {
                if (is_matrix(g, l) && is_matrix(g, r))
                    return a->cols == b->rows ? glslc_matrix_type(b->cols, a->rows) : 0;
                if (is_vector(g, l)) return a->rows == b->rows ? glslc_vector_type(SNX_GLSLC_BASE_FLOAT, b->cols) : 0;
                return a->cols == b->rows ? glslc_vector_type(SNX_GLSLC_BASE_FLOAT, a->rows) : 0;
            }
            if (l == r) return l;
            if (is_scalar(g, l)) return r;
            if (is_scalar(g, r)) return l;
            return 0;
        case GLSLC_OP_MOD:
        case GLSLC_OP_AND:
        case GLSLC_OP_OR:
        case GLSLC_OP_XOR:
            if (!is_integer(g, l) || !is_integer(g, r) || a->base != b->base) return 0;
            if (l == r || is_scalar(g, r)) return l;
            if (is_scalar(g, l)) return r;
            return 0;
        case GLSLC_OP_SHL:
        case GLSLC_OP_SHR:
            if (!is_integer(g, l) || !is_integer(g, r)) return 0;
            if (is_scalar(g, r) || (is_vector(g, l) && a->rows == b->rows)) return l;
            return 0;
        case GLSLC_OP_LT:
        case GLSLC_OP_GT:
        case GLSLC_OP_LE:
        case GLSLC_OP_GE: return l == r && is_scalar(g, l) && is_numeric(g, l) ? GT_BOOL : 0;
        case GLSLC_OP_EQ:
        case GLSLC_OP_NE:
            if (l != r || has_opaque(g, l) || a->kind == GLSLC_T_VOID) return 0;
            if (a->kind == GLSLC_T_ARRAY && g->vi == GLSLC_V100) return 0;
            return GT_BOOL;
        case GLSLC_OP_LOGIC_AND:
        case GLSLC_OP_LOGIC_OR:
        case GLSLC_OP_LOGIC_XOR: return l == GT_BOOL && r == GT_BOOL ? GT_BOOL : 0;
        default: return 0;
    }
}

static const char* op_text(uint8_t op) {
    static const char* const text[] = {"",  "+", "-",  "*",  "/",  "%",  "<<", ">>", "&",  "|", "^",
                                       "<", ">", "<=", ">=", "==", "!=", "&&", "||", "^^", "="};
    return op < sizeof text / sizeof text[0] ? text[op] : "?";
}

static uint32_t make_binary(Glslc* g, uint8_t op, uint32_t tok, uint32_t l, uint32_t r) {
    uint32_t lt = nd(g, l)->type, rt = nd(g, r)->type;
    if (g->vi == GLSLC_V100 && (op == GLSLC_OP_MOD || op == GLSLC_OP_SHL || op == GLSLC_OP_SHR || op == GLSLC_OP_AND ||
                                op == GLSLC_OP_OR || op == GLSLC_OP_XOR))
        glslc_fail(g, site(g, tok), "operator %s needs GLSL ES 3.00 (1.00 reserves it)", op_text(op));
    uint32_t t = binary_type(g, op, lt, rt);
    if (!t) {
        char a[64], b[64];
        glslc_type_name(g, lt, a, sizeof a);
        glslc_type_name(g, rt, b, sizeof b);
        glslc_fail(g, site(g, tok), "no operator %s takes %s and %s (GLSL ES converts neither)", op_text(op), a, b);
    }
    uint32_t bits;
    if (nd(g, l)->kind == GLSLC_N_CONST && nd(g, r)->kind == GLSLC_N_CONST && is_scalar(g, t) &&
        fold_binary(g, op, lt, bits_of(g, l), bits_of(g, r), &bits))
        return make_const(g, tok, t, bits);
    uint32_t n = node(g, GLSLC_N_BINARY, tok, t);
    nd(g, n)->op = op;
    nd(g, n)->a = (uint16_t)l;
    nd(g, n)->b = (uint16_t)r;
    if (is_const(g, l) && is_const(g, r)) nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

static uint32_t make_unary(Glslc* g, uint8_t op, uint32_t tok, uint32_t a) {
    uint32_t t = nd(g, a)->type;
    char name[64];
    glslc_type_name(g, t, name, sizeof name);
    switch (op) {
        case GLSLC_OP_NEG:
        case GLSLC_OP_PLUS:
            if (!is_numeric(g, t))
                glslc_fail(g, site(g, tok), "unary %s takes a number, not %s", op == GLSLC_OP_NEG ? "-" : "+", name);
            break;
        case GLSLC_OP_NOT:
            if (t != GT_BOOL) glslc_fail(g, site(g, tok), "! takes a bool, not %s", name);
            break;
        case GLSLC_OP_BITNOT:
            if (g->vi == GLSLC_V100) glslc_fail(g, site(g, tok), "operator ~ needs GLSL ES 3.00 (1.00 reserves it)");
            if (!is_integer(g, t)) glslc_fail(g, site(g, tok), "~ takes an int or uint, not %s", name);
            break;
        default:
            if (!is_numeric(g, t))
                glslc_fail(g, site(g, tok), "%s takes a number, not %s",
                           op == GLSLC_OP_PRE_INC || op == GLSLC_OP_POST_INC ? "++" : "--", name);
            need_lvalue(g, a, tok, op == GLSLC_OP_PRE_INC || op == GLSLC_OP_POST_INC ? "++" : "--");
    }
    if (nd(g, a)->kind == GLSLC_N_CONST) {
        uint32_t v = bits_of(g, a);
        int fl = ty(g, t)->base == SNX_GLSLC_BASE_FLOAT;
        if (op == GLSLC_OP_NEG) return make_const(g, tok, t, fl ? u32(-f32(v)) : 0u - v);
        if (op == GLSLC_OP_PLUS) return make_const(g, tok, t, v);
        if (op == GLSLC_OP_NOT) return make_const(g, tok, t, !v);
        if (op == GLSLC_OP_BITNOT) return make_const(g, tok, t, ~v);
    }
    uint32_t n = node(g, GLSLC_N_UNARY, tok, t);
    nd(g, n)->op = op;
    nd(g, n)->a = (uint16_t)a;
    if (is_const(g, a) && op != GLSLC_OP_PRE_INC && op != GLSLC_OP_PRE_DEC && op != GLSLC_OP_POST_INC &&
        op != GLSLC_OP_POST_DEC)
        nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

static uint32_t make_index(Glslc* g, uint32_t tok, uint32_t base, uint32_t index) {
    uint32_t bt = nd(g, base)->type, it = nd(g, index)->type;
    const GlslcType* b = ty(g, bt);
    char name[64];
    glslc_type_name(g, bt, name, sizeof name);
    if (!(it == GT_INT || (it == GT_UINT && g->vi != GLSLC_V100))) {
        char iname[64];
        glslc_type_name(g, it, iname, sizeof iname);
        glslc_fail(g, site(g, nd(g, index)->tok), "an index is an int%s, not %s",
                   g->vi == GLSLC_V100 ? "" : " or a uint", iname);
    }
    uint32_t t, len;
    if (b->kind == GLSLC_T_ARRAY) {
        t = b->elem;
        len = b->len;
    } else if (b->kind == GLSLC_T_VECTOR) {
        t = glslc_vector_type(b->base, 1);
        len = b->rows;
    } else if (b->kind == GLSLC_T_MATRIX) {
        t = glslc_vector_type(SNX_GLSLC_BASE_FLOAT, b->rows);
        len = b->cols;
    } else {
        glslc_fail(g, site(g, tok), "%s cannot be indexed", name);
        return 0;
    }
    if (nd(g, index)->kind == GLSLC_N_CONST) {
        int32_t i = (int32_t)bits_of(g, index);
        if (i < 0) glslc_fail(g, site(g, nd(g, index)->tok), "index %d of %s is negative", (int)i, name);
        if (len && (uint32_t)i >= len)
            glslc_fail(g, site(g, nd(g, index)->tok), "index %d is past the end of %s", (int)i, name);
        uint32_t bits;
        if (b->kind == GLSLC_T_VECTOR && const_component(g, base, (uint32_t)i, &bits))
            return make_const(g, tok, t, bits);
    } else if (g->vi < GLSLC_V320 && b->kind == GLSLC_T_ARRAY &&
               (has_opaque(g, b->elem) || ty(g, element_of(g, bt))->kind == GLSLC_T_BLOCK)) {
        glslc_fail(g, site(g, tok), "%s is indexed by a constant expression only in GLSL ES %s", name, version_name(g));
    }
    uint32_t n = node(g, GLSLC_N_INDEX, tok, t);
    nd(g, n)->a = (uint16_t)base;
    nd(g, n)->b = (uint16_t)index;
    if (is_const(g, base) && is_const(g, index)) nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

static uint32_t make_swizzle(Glslc* g, uint32_t tok, uint32_t base) {
    uint32_t bt = nd(g, base)->type;
    const GlslcType* b = ty(g, bt);
    const char* text = glslc_token_text(g, tk(g, tok));
    uint32_t len = tk(g, tok)->text.len;
    char name[64];
    glslc_type_name(g, bt, name, sizeof name);
    if (b->kind != GLSLC_T_VECTOR) glslc_fail(g, site(g, tok), "%s has no field %.*s", name, TOKV(g, tok));
    if (len > 4)
        glslc_fail(g, site(g, tok), "the swizzle %.*s has %u components: a swizzle takes 1 to 4", TOKV(g, tok), len);
    static const char* const sets[] = {"xyzw", "rgba", "stpq"};
    uint32_t set = 3, packed = 0;
    for (uint32_t i = 0; i < len; i++) {
        uint32_t s, c = 4;
        for (s = 0; s < 3; s++) {
            const char* p = strchr(sets[s], text[i]);
            if (p && text[i]) {
                c = (uint32_t)(p - sets[s]);
                break;
            }
        }
        if (c == 4 || (set != 3 && s != set))
            glslc_fail(g, site(g, tok), "'%.*s' is not a swizzle: its letters come from one of xyzw, rgba and stpq",
                       TOKV(g, tok));
        if (c >= b->rows)
            glslc_fail(g, site(g, tok), "'%c' in .%.*s is past the %u components of %s", text[i], TOKV(g, tok),
                       (unsigned)b->rows, name);
        set = s;
        packed |= c << (2 * i);
    }
    uint32_t t = glslc_vector_type(b->base, len);
    uint32_t bits;
    if (len == 1 && const_component(g, base, packed, &bits)) return make_const(g, tok, t, bits);
    uint32_t n = node(g, GLSLC_N_SWIZZLE, tok, t);
    nd(g, n)->a = (uint16_t)base;
    nd(g, n)->b = (uint16_t)packed;
    nd(g, n)->op = (uint8_t)len;
    if (is_const(g, base)) nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

static uint32_t make_field(Glslc* g, uint32_t tok, uint32_t base) {
    uint32_t bt = nd(g, base)->type;
    const GlslcType* b = ty(g, bt);
    if (b->kind == GLSLC_T_STRUCT || b->kind == GLSLC_T_BLOCK) {
        for (uint32_t m = 0; m < b->len; m++) {
            if (glslc_views_equal(g, g->members[b->first + m].name, tk(g, tok)->text)) {
                uint32_t n = node(g, GLSLC_N_FIELD, tok, g->members[b->first + m].type);
                nd(g, n)->a = (uint16_t)base;
                nd(g, n)->b = (uint16_t)m;
                if (is_const(g, base)) nd(g, n)->flags = GLSLC_NF_CONST;
                return n;
            }
        }
        char name[64];
        glslc_type_name(g, bt, name, sizeof name);
        glslc_fail(g, site(g, tok), "%s has no member %.*s", name, TOKV(g, tok));
    }
    return make_swizzle(g, tok, base);
}

static uint32_t count_args(const Glslc* g, uint32_t first) {
    uint32_t n = 0;
    for (uint32_t a = first; a; a = g->nodes[a].next) n++;
    return n;
}

static void arg_list_text(const Glslc* g, uint32_t first, char* out, size_t size) {
    size_t w = 0;
    out[0] = 0;
    for (uint32_t a = first; a && w + 1 < size; a = g->nodes[a].next) {
        char name[64];
        glslc_type_name(g, g->nodes[a].type, name, sizeof name);
        int k = snprintf(out + w, size - w, "%s%s", w ? ", " : "", name);
        if (k < 0) break;
        w += (size_t)k;
    }
}

static uint32_t make_constructor(Glslc* g, uint32_t type, uint32_t tok, uint32_t first) {
    char name[64];
    glslc_type_name(g, type, name, sizeof name);
    const GlslcType* t = ty(g, type);
    uint32_t count = count_args(g, first), all_const = 1;
    for (uint32_t a = first; a; a = g->nodes[a].next) all_const &= is_const(g, a);
    if (!count) glslc_fail(g, site(g, tok), "%s() needs arguments", name);
    if (t->kind == GLSLC_T_ARRAY) {
        if (t->len == 0) {
            type = array_of(g, t->elem, count, tok);
            t = ty(g, type);
        } else if (count != t->len) {
            glslc_fail(g, site(g, tok), "%s takes %u elements, and %u are given", name, (unsigned)t->len, count);
        }
        uint32_t i = 0;
        for (uint32_t a = first; a; a = g->nodes[a].next, i++) {
            if (g->nodes[a].type != t->elem) {
                char en[64], an[64];
                glslc_type_name(g, t->elem, en, sizeof en);
                glslc_type_name(g, g->nodes[a].type, an, sizeof an);
                glslc_fail(g, site(g, g->nodes[a].tok), "element %u of %s is %s, not %s", i, name, an, en);
            }
        }
    } else if (t->kind == GLSLC_T_STRUCT) {
        if (count != t->len)
            glslc_fail(g, site(g, tok), "%s has %u members, and %u values are given", name, (unsigned)t->len, count);
        uint32_t i = 0;
        for (uint32_t a = first; a; a = g->nodes[a].next, i++) {
            uint32_t mt = g->members[t->first + i].type;
            if (g->nodes[a].type != mt) {
                char en[64], an[64];
                glslc_type_name(g, mt, en, sizeof en);
                glslc_type_name(g, g->nodes[a].type, an, sizeof an);
                glslc_fail(g, site(g, g->nodes[a].tok), "member %.*s of %s is %s, not %s",
                           VIEWV(g, g->members[t->first + i].name), name, en, an);
            }
        }
    } else if (is_svm(g, type)) {
        uint32_t need = glslc_type_components(g, type), sum = 0, last = 0;
        for (uint32_t a = first; a; a = g->nodes[a].next) {
            uint32_t at = g->nodes[a].type;
            if (!is_svm(g, at)) {
                char an[64];
                glslc_type_name(g, at, an, sizeof an);
                glslc_fail(g, site(g, g->nodes[a].tok), "%s cannot be made from %s", name, an);
            }
            if (t->kind == GLSLC_T_MATRIX && is_matrix(g, at) && count > 1)
                glslc_fail(g, site(g, g->nodes[a].tok), "a matrix made from a matrix takes no other argument");
            last = glslc_type_components(g, at);
            sum += last;
        }
        int single = count == 1;
        if (t->kind == GLSLC_T_SCALAR) {
            if (count != 1) glslc_fail(g, site(g, tok), "%s takes one argument, and %u are given", name, count);
        } else if (!(single && (is_scalar(g, g->nodes[first].type) ||
                                (is_matrix(g, type) && is_matrix(g, g->nodes[first].type))))) {
            if (sum < need)
                glslc_fail(g, site(g, tok), "%s needs %u components, and its arguments have %u", name, need, sum);
            if (sum - last >= need)
                glslc_fail(g, site(g, tok), "%s has %u components, and its arguments give %u: the last is left over",
                           name, need, sum);
        }
        uint32_t bits;
        if (t->kind == GLSLC_T_SCALAR && const_component(g, first, 0, &bits))
            return make_const(
                g, tok, type,
                glslc_fold_convert(g, bits, glslc_vector_type(ty(g, g->nodes[first].type)->base, 1), type));
    } else {
        glslc_fail(g, site(g, tok), "%s has no constructor", name);
    }
    uint32_t n = node(g, GLSLC_N_CONSTRUCT, tok, type);
    nd(g, n)->a = (uint16_t)first;
    if (all_const) nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

/* the type a built-in signature gives for these arguments, or 0 */
static uint32_t match_signature(const Glslc* g, const char* sig, const uint16_t* at, uint32_t n) {
    uint32_t size = 0, mat = 0, p = 0, r = 0;
    const char* s = sig + 2;
    for (uint32_t i = 0; i < n; i++) {
        char c = *s;
        if (!c) return 0;
        const GlslcType* t = ty(g, at[i]);
        uint32_t base = 0;
        int vec_only = 0, gen = 1;
        switch (c) {
            case 'F': base = SNX_GLSLC_BASE_FLOAT; break;
            case 'I': base = SNX_GLSLC_BASE_INT; break;
            case 'U': base = SNX_GLSLC_BASE_UINT; break;
            case 'B': base = SNX_GLSLC_BASE_BOOL; break;
            case 'V':
                base = SNX_GLSLC_BASE_FLOAT;
                vec_only = 1;
                break;
            case 'J':
                base = SNX_GLSLC_BASE_INT;
                vec_only = 1;
                break;
            case 'K':
                base = SNX_GLSLC_BASE_UINT;
                vec_only = 1;
                break;
            case 'L':
                base = SNX_GLSLC_BASE_BOOL;
                vec_only = 1;
                break;
            default: gen = 0;
        }
        if (gen) {
            if (!(t->kind == GLSLC_T_SCALAR || t->kind == GLSLC_T_VECTOR) || t->base != base) return 0;
            uint32_t rows = t->kind == GLSLC_T_SCALAR ? 1u : t->rows;
            if ((vec_only && rows == 1) || (size && size != rows)) return 0;
            size = rows;
        } else {
            switch (c) {
                case 'f':
                    if (at[i] != GT_FLOAT) return 0;
                    break;
                case 'i':
                    if (at[i] != GT_INT) return 0;
                    break;
                case 'u':
                    if (at[i] != GT_UINT) return 0;
                    break;
                case 'b':
                    if (at[i] != GT_BOOL) return 0;
                    break;
                case '2':
                    if (at[i] != GT_VEC2) return 0;
                    break;
                case '3':
                    if (at[i] != GT_VEC3) return 0;
                    break;
                case '4':
                    if (at[i] != GT_VEC4) return 0;
                    break;
                case 'M':
                case 'S':
                    if (t->kind != GLSLC_T_MATRIX || (c == 'S' && t->rows != t->cols) || (mat && mat != at[i]))
                        return 0;
                    mat = at[i];
                    break;
                case 'P':
                case 'R':
                    if (t->kind != GLSLC_T_VECTOR || t->base != SNX_GLSLC_BASE_FLOAT) return 0;
                    if (c == 'P') {
                        p = t->rows;
                    } else {
                        r = t->rows;
                    }
                    break;
                default: return 0;
            }
        }
        s++;
        if (*s == '&') s++;
    }
    if (*s) return 0;
    switch (sig[0]) {
        case 'F': return glslc_vector_type(SNX_GLSLC_BASE_FLOAT, size);
        case 'I': return glslc_vector_type(SNX_GLSLC_BASE_INT, size);
        case 'U': return glslc_vector_type(SNX_GLSLC_BASE_UINT, size);
        case 'B':
        case 'L': return glslc_vector_type(SNX_GLSLC_BASE_BOOL, size);
        case 'f': return GT_FLOAT;
        case 'i': return GT_INT;
        case 'u': return GT_UINT;
        case 'b': return GT_BOOL;
        case '2': return GT_VEC2;
        case '3': return GT_VEC3;
        case '4': return GT_VEC4;
        case 'M':
        case 'S': return mat;
        case 'T': return glslc_matrix_type(ty(g, mat)->rows, ty(g, mat)->cols);
        case 'O': return glslc_matrix_type(r, p);
        case 'v': return GT_VOID;
        default: return 0;
    }
}

static uint32_t ivec(uint32_t n) { return glslc_vector_type(SNX_GLSLC_BASE_INT, n); }
static uint32_t fvec(uint32_t n) { return glslc_vector_type(SNX_GLSLC_BASE_FLOAT, n); }

/* the type a texture or image function gives for these arguments, or 0 */
static uint32_t match_texture(const Glslc* g, const GlslcBuiltinFn* f, const uint16_t* at, uint32_t n, uint8_t* extra) {
    uint32_t fl = f->code;
    if (n < 1) return 0;
    const GlslcType* s = ty(g, at[0]);
    uint32_t dims = s->dim == GLSLC_DIM_2D ? 2u : s->dim == GLSLC_DIM_BUFFER ? 1u : 3u;
    int arrayed = (s->tflags & GLSLC_TF_ARRAYED) != 0, cube = s->dim == GLSLC_DIM_CUBE;
    int buffer = s->dim == GLSLC_DIM_BUFFER;
    if (fl & (GLSLC_TX_IMAGE_LOAD | GLSLC_TX_IMAGE_STORE | GLSLC_TX_IMAGE_SIZE | GLSLC_TX_IMAGE_ATOMIC)) {
        if (s->kind != GLSLC_T_IMAGE) return 0;
        uint32_t coord = ivec(dims + (uint32_t)(arrayed && !cube));
        uint32_t texel = glslc_vector_type(s->base, 4);
        if (fl & GLSLC_TX_IMAGE_SIZE)
            return n == 1 ? ivec(cube ? 2u + (uint32_t)arrayed : dims + (uint32_t)arrayed) : 0;
        if (n < 2 || at[1] != coord) return 0;
        if (fl & GLSLC_TX_IMAGE_LOAD) return n == 2 ? texel : 0;
        if (fl & GLSLC_TX_IMAGE_STORE) return n == 3 && at[2] == texel ? GT_VOID : 0;
        uint32_t data = glslc_vector_type(s->base, 1);
        if (s->base == SNX_GLSLC_BASE_FLOAT && f->code2 != 229) return 0; /* a float image takes only exchange */
        if (f->code2 == 230) return n == 4 && at[2] == data && at[3] == data ? data : 0;
        return n == 3 && at[2] == data ? data : 0;
    }
    if (s->kind != GLSLC_T_SAMPLER) return 0;
    if ((fl & GLSLC_TX_2D_ONLY) && at[0] != GT_SAMPLER_FIRST) return 0;
    if ((fl & GLSLC_TX_CUBE_ONLY) && at[0] != GT_SAMPLER_FIRST + 1) return 0;
    if ((fl & GLSLC_TX_3D_ONLY) && at[0] != GT_SAMPLER_FIRST + 2) return 0;
    if ((fl & GLSLC_TX_SHADOW_ONLY) && at[0] != GT_SAMPLER_FIRST + 3) return 0;
    int shadow = (s->tflags & GLSLC_TF_SHADOW) != 0, ms = (s->tflags & GLSLC_TF_MS) != 0;
    uint32_t coords = dims + (uint32_t)arrayed;
    uint32_t texel = shadow ? GT_FLOAT : glslc_vector_type(s->base, 4);
    if (fl & GLSLC_TX_SIZE) {
        uint32_t size = ivec(cube ? 2u + (uint32_t)arrayed : coords);
        if (ms || buffer) return n == 1 ? size : 0;
        return n == 2 && at[1] == GT_INT ? size : 0;
    }
    if (fl & GLSLC_TX_FETCH) {
        if (cube || shadow) return 0;
        if (buffer) return n == 2 && at[1] == GT_INT ? texel : 0;
        if (ms) return (fl & GLSLC_TX_OFFSET) || n != 3 || at[1] != ivec(coords) || at[2] != GT_INT ? 0 : texel;
        uint32_t want = 3u + ((fl & GLSLC_TX_OFFSET) ? 1u : 0u);
        if (n != want || at[1] != ivec(coords) || at[2] != GT_INT) return 0;
        if ((fl & GLSLC_TX_OFFSET) && at[3] != ivec(dims)) return 0;
        return texel;
    }
    if (ms || buffer) return 0;
    if (fl & GLSLC_TX_GATHER) {
        if (s->dim == GLSLC_DIM_3D || (cube && (fl & GLSLC_TX_OFFSET))) return 0;
        uint32_t i = 2;
        if (n < 2 || at[1] != fvec(coords)) return 0;
        if (shadow && (i >= n || at[i++] != GT_FLOAT)) return 0;
        if ((fl & GLSLC_TX_OFFSET) && (i >= n || at[i++] != GT_IVEC2)) return 0;
        if (!shadow && i < n) {
            if (at[i++] != GT_INT) return 0;
            *extra |= TX_EXTRA_COMP;
        }
        return i == n ? (shadow ? GT_VEC4 : glslc_vector_type(s->base, 4)) : 0;
    }
    int cube_array_shadow = cube && arrayed && shadow;
    uint32_t i = 2;
    if (n < 2) return 0;
    if (fl & GLSLC_TX_PROJ) {
        if (cube || arrayed) return 0;
        if (s->dim == GLSLC_DIM_2D && !shadow) {
            if (at[1] != GT_VEC3 && at[1] != GT_VEC4) return 0;
        } else if (at[1] != GT_VEC4) {
            return 0;
        }
    } else {
        uint32_t p = coords + (uint32_t)(shadow && !cube_array_shadow);
        if (at[1] != fvec(p)) return 0;
        if (cube_array_shadow) {
            if ((fl & (GLSLC_TX_LOD | GLSLC_TX_GRAD | GLSLC_TX_OFFSET)) || n < 3 || at[2] != GT_FLOAT) return 0;
            i = 3;
        }
    }
    if (fl & GLSLC_TX_LOD) {
        if (shadow && (cube || arrayed)) return 0;
        if (i >= n || at[i++] != GT_FLOAT) return 0;
    }
    if (fl & GLSLC_TX_GRAD) {
        uint32_t d = fvec(cube ? 3u : dims);
        if (i + 1 >= n || at[i] != d || at[i + 1] != d) return 0;
        i += 2;
    }
    if (fl & GLSLC_TX_OFFSET) {
        if (cube || i >= n || at[i++] != ivec(dims)) return 0;
    }
    if ((fl & GLSLC_TX_BIAS) && i < n && !cube_array_shadow) {
        if (at[i++] != GT_FLOAT) return 0;
        *extra |= TX_EXTRA_BIAS;
    }
    return i == n ? texel : 0;
}

static int builtin_available(const Glslc* g, const GlslcBuiltinFn* f) {
    return (f->vmask & (1u << g->vi)) || (f->ext && g->ext[f->ext] != GLSLC_BEHAVIOR_DISABLE);
}

static uint32_t builtin_call(Glslc* g, uint32_t name_tok, uint32_t first) {
    GlslcView name = tk(g, name_tok)->text;
    uint16_t at[16];
    uint32_t n = 0, any = 0, in_version = 0, in_stage = 0, row = 0, type = 0;
    uint8_t extra = 0;
    for (uint32_t a = first; a; a = g->nodes[a].next) {
        if (n == 16) break;
        at[n++] = g->nodes[a].type;
    }
    int too_many = count_args(g, first) > 16;
    for (uint32_t r = 0; r < glslc_builtin_fn_count; r++) {
        const GlslcBuiltinFn* f = &glslc_builtin_fns[r];
        if (!glslc_view_is(g, name, f->name)) continue;
        any = 1;
        if (!builtin_available(g, f)) continue;
        in_version = 1;
        if (!(f->stages & g->smask)) continue;
        in_stage = 1;
        if (too_many) continue;
        extra = 0;
        type = f->bk == GLSLC_BK_TEXTURE ? match_texture(g, f, at, n, &extra) : match_signature(g, f->sig, at, n);
        if (type) {
            row = r;
            break;
        }
    }
    if (!any) glslc_fail(g, site(g, name_tok), "no function '%.*s' is declared", VIEWV(g, name));
    if (!in_version)
        glslc_fail(g, site(g, name_tok), "the built-in function %.*s is not in GLSL ES %s%s", VIEWV(g, name),
                   version_name(g), g->vi == GLSLC_V100 ? " without its extension" : "");
    if (!in_stage) {
        const char* where = "this stage";
        for (uint32_t r = 0; r < glslc_builtin_fn_count; r++) {
            if (glslc_view_is(g, name, glslc_builtin_fns[r].name)) {
                uint8_t st = glslc_builtin_fns[r].stages;
                where = st == GLSLC_SF   ? "fragment shaders"
                        : st == GLSLC_SC ? "compute shaders"
                        : st == GLSLC_SV ? "vertex shaders"
                                         : "other stages";
                break;
            }
        }
        glslc_fail(g, site(g, name_tok), "the built-in function %.*s is only in %s", VIEWV(g, name), where);
    }
    if (!type) {
        char args[160];
        arg_list_text(g, first, args, sizeof args);
        glslc_fail(g, site(g, name_tok), "no overload of %.*s takes (%s)", VIEWV(g, name), args);
    }
    const GlslcBuiltinFn* f = &glslc_builtin_fns[row];
    if ((extra & TX_EXTRA_BIAS) && g->stage != SNX_GLSLC_FRAGMENT)
        glslc_fail(g, site(g, name_tok), "%.*s takes a bias only in a fragment shader", VIEWV(g, name));
    /* the out arguments, and what must be constant */
    uint32_t i = 0;
    const char* s = f->sig[0] ? f->sig + 2 : "";
    for (uint32_t a = first; a; a = g->nodes[a].next, i++) {
        if (*s) {
            if (s[1] == '&') {
                need_lvalue(g, a, g->nodes[a].tok, "an out argument");
                s++;
            }
            s++;
        }
    }
    if (f->bk == GLSLC_BK_SPECIAL && (f->code == GLSLC_BS_ATOMIC || f->code == GLSLC_BS_ATOMIC_COMP_SWAP)) {
        const char* why = "";
        uint32_t root = lvalue_root(g, first, &why);
        uint8_t st = root ? g->syms[root].storage : 0;
        if (root && g->syms[root].kind == GLSLC_SK_BLOCK_MEMBER) st = g->syms[g->syms[root].aux].storage;
        if (st != GLSLC_ST_BUFFER && st != GLSLC_ST_SHARED)
            glslc_fail(g, site(g, g->nodes[first].tok), "%.*s works on a buffer or shared variable", VIEWV(g, name));
    }
    if (f->bk == GLSLC_BK_TEXTURE) {
        uint32_t k = 0;
        for (uint32_t a = first; a; a = g->nodes[a].next, k++) {
            int offset_arg = (f->code & GLSLC_TX_OFFSET) &&
                             g->nodes[a].type == ivec(2 + (ty(g, at[0])->dim == GLSLC_DIM_3D)) && k >= 2;
            int comp_arg = (extra & TX_EXTRA_COMP) && !g->nodes[a].next;
            if ((offset_arg || comp_arg) && !is_const(g, a))
                glslc_fail(g, site(g, g->nodes[a].tok), "this argument of %.*s must be a constant expression",
                           VIEWV(g, name));
            for (uint32_t c = 0; offset_arg && c < glslc_type_components(g, g->nodes[a].type); c++) {
                uint32_t v;
                if (const_component(g, a, c, &v) && ((int32_t)v < -8 || (int32_t)v > 7))
                    glslc_fail(g, site(g, g->nodes[a].tok),
                               "the offset %d is outside gl_MinProgramTexelOffset (-8) to gl_MaxProgramTexelOffset (7)",
                               (int)(int32_t)v);
            }
            if (comp_arg) {
                uint32_t c;
                if (!const_component(g, a, 0, &c) || c > 3)
                    glslc_fail(g, site(g, g->nodes[a].tok), "textureGather's component is 0, 1, 2 or 3");
            }
        }
        if ((f->code & (GLSLC_TX_IMAGE_LOAD | GLSLC_TX_IMAGE_STORE | GLSLC_TX_IMAGE_ATOMIC))) {
            const char* why = "";
            uint32_t root = lvalue_root(g, first, &why);
            uint16_t q = root ? g->syms[root].qual : 0;
            if ((f->code & GLSLC_TX_IMAGE_LOAD) && (q & GLSLC_Q_WRITEONLY))
                glslc_fail(g, site(g, name_tok), "imageLoad reads a writeonly image");
            if ((f->code & (GLSLC_TX_IMAGE_STORE | GLSLC_TX_IMAGE_ATOMIC)) && (q & GLSLC_Q_READONLY))
                glslc_fail(g, site(g, name_tok), "%.*s writes a readonly image", VIEWV(g, name));
        }
    }
    uint32_t node_ = node(g, GLSLC_N_BUILTIN, name_tok, type);
    nd(g, node_)->a = (uint16_t)first;
    nd(g, node_)->b = (uint16_t)row;
    nd(g, node_)->op = extra;
    int pure = f->bk == GLSLC_BK_EXT || f->bk == GLSLC_BK_EXT_SPLAT ||
               ((f->bk == GLSLC_BK_OP || f->bk == GLSLC_BK_OP_SPLAT) && !(f->stages == GLSLC_SF)) ||
               (f->bk == GLSLC_BK_SPECIAL &&
                (f->code == GLSLC_BS_MIX_BOOL || f->code == GLSLC_BS_MATRIX_COMP_MULT ||
                 f->code == GLSLC_BS_OUTER_PRODUCT || f->code == GLSLC_BS_TRANSPOSE || f->code == GLSLC_BS_BITCAST));
    int all_const = 1;
    for (uint32_t a = first; a; a = g->nodes[a].next) all_const &= is_const(g, a);
    if (pure && all_const && first) nd(g, node_)->flags = GLSLC_NF_CONST;
    return node_;
}

static int same_params(const Glslc* g, uint32_t first_param, uint32_t count, uint32_t args) {
    for (uint32_t i = 0; i < count; i++, args = g->nodes[args].next) {
        if (!args || g->nodes[args].type != g->params[first_param + i].type) return 0;
    }
    return args == 0;
}

static uint32_t function_call(Glslc* g, uint32_t name_tok, uint32_t first) {
    GlslcView name = tk(g, name_tok)->text;
    uint32_t s = lookup(g, name);
    if (s && g->syms[s].kind != GLSLC_SK_FUNC)
        glslc_fail(g, site(g, name_tok), "'%.*s' is a variable, not a function", VIEWV(g, name));
    if (!s) return builtin_call(g, name_tok, first);
    uint32_t found = 0;
    for (uint32_t u = s; u; u = g->syms[u].next) {
        if (g->syms[u].kind == GLSLC_SK_FUNC && glslc_views_equal(g, g->syms[u].name, name) &&
            same_params(g, g->syms[u].aux, g->syms[u].aux2, first)) {
            found = u;
            break;
        }
    }
    if (!found) {
        char args[160];
        arg_list_text(g, first, args, sizeof args);
        glslc_fail(g, site(g, name_tok), "no %.*s(%s) is declared (GLSL ES converts no argument)", VIEWV(g, name),
                   args);
    }
    uint32_t i = 0;
    for (uint32_t a = first; a; a = g->nodes[a].next, i++) {
        uint8_t st = g->params[g->syms[found].aux + i].storage;
        if (st == GLSLC_ST_PARAM_OUT || st == GLSLC_ST_PARAM_INOUT)
            need_lvalue(g, a, g->nodes[a].tok, st == GLSLC_ST_PARAM_OUT ? "an out argument" : "an inout argument");
    }
    if (g->func) {
        if (g->ncalls >= SNX_GLSLC_MAX_CALLS)
            glslc_fail(g, site(g, name_tok), "the shader has more than the %u calls between its functions glslc takes",
                       SNX_GLSLC_MAX_CALLS);
        GlslcCall* c = &g->calls[g->ncalls++];
        c->caller = g->func;
        c->callee = (uint16_t)found;
        c->tok = (uint16_t)name_tok;
    }
    g->syms[found].qual |= GLSLC_Q_USED;
    uint32_t n = node(g, GLSLC_N_CALL, name_tok, g->syms[found].type);
    nd(g, n)->a = (uint16_t)first;
    nd(g, n)->b = (uint16_t)found;
    return n;
}

/* arguments after '(' up to ')' */
static uint32_t parse_arguments(Glslc* g) {
    uint32_t first = 0, tail = 0;
    if (kind_at(g, 0) == TK_TYPE + GT_VOID - 1 && kind_at(g, 1) == TK_RPAREN) {
        g->pos += 2;
        return 0;
    }
    if (!accept(g, TK_RPAREN)) {
        do {
            uint32_t a = parse_assignment(g);
            if (tail) {
                nd(g, tail)->next = (uint16_t)a;
            } else {
                first = a;
            }
            tail = a;
        } while (accept(g, TK_COMMA));
        expect(g, TK_RPAREN, "')' after the arguments");
    }
    return first;
}

static uint32_t builtin_variable(Glslc* g, uint32_t tok) {
    GlslcView name = tk(g, tok)->text;
    uint32_t row = glslc_builtin_var_count, other_stage = 0, other_version = 0;
    for (uint32_t r = 0; r < glslc_builtin_var_count; r++) {
        const GlslcBuiltinVar* v = &glslc_builtin_vars[r];
        if (!glslc_view_is(g, name, v->name)) continue;
        if (!((v->vmask & (1u << g->vi)) || (v->ext && g->ext[v->ext] != GLSLC_BEHAVIOR_DISABLE))) {
            other_version = 1;
            continue;
        }
        if (!(v->stages & g->smask)) {
            other_stage = v->stages;
            continue;
        }
        row = r;
        break;
    }
    if (row == glslc_builtin_var_count) {
        if (other_stage)
            glslc_fail(g, site(g, tok), "%.*s is only in %s shaders", VIEWV(g, name),
                       other_stage == GLSLC_SV   ? "vertex"
                       : other_stage == GLSLC_SF ? "fragment"
                                                 : "compute");
        if (other_version)
            glslc_fail(g, site(g, tok), "%.*s is not in GLSL ES %s%s", VIEWV(g, name), version_name(g),
                       g->vi == GLSLC_V100 ? " without its extension" : "");
        glslc_fail(g, site(g, tok), "'%.*s' is not declared (names starting with gl_ are GLSL's own)", VIEWV(g, name));
    }
    const GlslcBuiltinVar* v = &glslc_builtin_vars[row];
    if (v->storage == GLSLC_ST_CONST && v->value != GLSLC_BUILTIN_VALUE_LOCAL_SIZE)
        return make_const(g, tok, v->type, (uint32_t)v->value);
    if (v->value == GLSLC_BUILTIN_VALUE_LOCAL_SIZE && !g->local_size_set)
        glslc_fail(g, site(g, tok), "gl_WorkGroupSize is used before layout(local_size_x = ...) in; sets it");
    if (!strcmp(v->name, "gl_FragColor")) g->frag_outputs |= 1;
    if (!strcmp(v->name, "gl_FragData")) g->frag_outputs |= 2;
    if (g->frag_outputs == 3)
        glslc_fail(g, site(g, tok), "a fragment shader writes gl_FragColor or gl_FragData, not both");
    uint32_t s = g->builtin_syms[row];
    if (!s) {
        s = add_symbol(g, name, GLSLC_SK_BUILTIN, site(g, tok), 0);
        g->builtin_syms[row] = (uint16_t)s;
        GlslcSymbol* y = &g->syms[s];
        y->storage = v->storage;
        y->aux = (uint16_t)row;
        y->prec = GLSLC_PREC_HIGH;
        uint32_t len = v->array;
        if (len && !strcmp(v->name, "gl_FragData"))
            len = g->ext[GLSLC_EXT_EXT_DRAW_BUFFERS] != GLSLC_BEHAVIOR_DISABLE ? 4u : 1u;
        y->type = (uint16_t)(len ? array_of(g, v->type, len, tok) : v->type);
    }
    g->syms[s].qual |= GLSLC_Q_USED;
    uint32_t n = node(g, GLSLC_N_VAR, tok, g->syms[s].type);
    nd(g, n)->a = (uint16_t)s;
    if (v->storage == GLSLC_ST_CONST) nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

static uint32_t variable(Glslc* g, uint32_t tok) {
    GlslcView name = tk(g, tok)->text;
    if (name.len >= 3 && !memcmp(glslc_view_text(g, name), "gl_", 3)) return builtin_variable(g, tok);
    uint32_t s = lookup(g, name);
    if (!s) glslc_fail(g, site(g, tok), "'%.*s' is not declared", VIEWV(g, name));
    const GlslcSymbol* y = &g->syms[s];
    if (y->kind == GLSLC_SK_FUNC) glslc_fail(g, site(g, tok), "'%.*s' is a function: call it with ()", VIEWV(g, name));
    if (y->kind == GLSLC_SK_STRUCT)
        glslc_fail(g, site(g, tok), "'%.*s' is a type: make one with %.*s(...)", VIEWV(g, name), VIEWV(g, name));
    g->syms[s].qual |= GLSLC_Q_USED;
    if (y->kind == GLSLC_SK_BLOCK_MEMBER) g->syms[y->aux].qual |= GLSLC_Q_USED;
    if (y->storage == GLSLC_ST_CONST && y->node && g->nodes[y->node].b &&
        g->nodes[g->nodes[y->node].b].kind == GLSLC_N_CONST)
        return make_const(g, tok, y->type, bits_of(g, g->nodes[y->node].b));
    uint32_t n = node(g, GLSLC_N_VAR, tok, y->type);
    nd(g, n)->a = (uint16_t)s;
    if (y->storage == GLSLC_ST_CONST) nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

static uint32_t parse_primary(Glslc* g) {
    uint32_t t = g->pos;
    uint8_t k = kind_at(g, 0);
    switch (k) {
        case TK_INTCONST:
        case TK_UINTCONST: {
            g->pos++;
            uint32_t v;
            if (!glslc_parse_int(glslc_token_text(g, tk(g, t)), tk(g, t)->text.len, &v))
                glslc_fail(g, site(g, t), "%.*s does not fit in 32 bits", TOKV(g, t));
            return make_const(g, t, k == TK_INTCONST ? GT_INT : GT_UINT, v);
        }
        case TK_FLOATCONST:
            g->pos++;
            return make_const(g, t, GT_FLOAT, parse_float(glslc_token_text(g, tk(g, t)), tk(g, t)->text.len));
        case TK_TRUE:
        case TK_FALSE: g->pos++; return make_const(g, t, GT_BOOL, k == TK_TRUE);
        case TK_LPAREN: { /* parse_expression counts the depth */
            g->pos++;
            uint32_t e = parse_expression(g);
            expect(g, TK_RPAREN, "')'");
            return e;
        }
        case TK_IDENT:
            if (kind_at(g, 1) == TK_LPAREN || (kind_at(g, 1) == TK_LBRACKET && is_struct_name(g, t))) {
                if (is_struct_name(g, t)) break; /* a constructor, below */
                g->pos += 2;
                uint32_t args = parse_arguments(g);
                return function_call(g, t, args);
            }
            g->pos++;
            return variable(g, t);
        default:
            if (k < TK_TYPE) {
                char found[96];
                describe(g, t, found, sizeof found);
                if (k == TK_RESERVED)
                    glslc_fail(g, site(g, t), "%s is a word GLSL ES %s reserves", found, version_name(g));
                glslc_fail(g, site(g, t), "expected an expression, found %s", found);
            }
    }
    /* a constructor: a type, maybe an array of it, then the arguments */
    uint32_t type = parse_type_specifier(g, 0);
    expect(g, TK_LPAREN, "'(' after a type, to make one");
    uint32_t args = parse_arguments(g);
    return make_constructor(g, type, t, args);
}

static uint32_t parse_postfix(Glslc* g) {
    uint32_t e = parse_primary(g);
    for (;;) {
        uint32_t t = g->pos;
        switch (kind_at(g, 0)) {
            case TK_LBRACKET: {
                g->pos++;
                uint32_t i = parse_expression(g);
                expect(g, TK_RBRACKET, "']' after an index");
                e = make_index(g, t, e, i);
                break;
            }
            case TK_DOT: {
                g->pos++;
                uint32_t f = expect(g, TK_IDENT, "a field or swizzle after '.'");
                if (glslc_view_is(g, tk(g, f)->text, "length") && kind_at(g, 0) == TK_LPAREN) {
                    g->pos++;
                    expect(g, TK_RPAREN, "')' after length(");
                    uint32_t bt = nd(g, e)->type;
                    const GlslcType* b = ty(g, bt);
                    need_version(g, f, GLSLC_V300, ".length()");
                    if (b->kind == GLSLC_T_ARRAY && b->len) {
                        e = make_const(g, f, GT_INT, b->len);
                    } else if (b->kind == GLSLC_T_VECTOR || b->kind == GLSLC_T_MATRIX) {
                        e = make_const(g, f, GT_INT, b->kind == GLSLC_T_VECTOR ? b->rows : b->cols);
                    } else if (b->kind == GLSLC_T_ARRAY) {
                        uint32_t n = node(g, GLSLC_N_LENGTH, f, GT_INT);
                        nd(g, n)->a = (uint16_t)e;
                        e = n;
                    } else {
                        char name[64];
                        glslc_type_name(g, bt, name, sizeof name);
                        glslc_fail(g, site(g, f), "%s has no length()", name);
                    }
                    break;
                }
                e = make_field(g, f, e);
                break;
            }
            case TK_INC:
            case TK_DEC:
                g->pos++;
                e = make_unary(g, tk(g, t)->kind == TK_INC ? GLSLC_OP_POST_INC : GLSLC_OP_POST_DEC, t, e);
                break;
            default: return e;
        }
    }
}

static uint32_t parse_unary(Glslc* g) {
    uint32_t t = g->pos;
    uint8_t op = 0;
    switch (kind_at(g, 0)) {
        case TK_PLUS: op = GLSLC_OP_PLUS; break;
        case TK_MINUS: op = GLSLC_OP_NEG; break;
        case TK_BANG: op = GLSLC_OP_NOT; break;
        case TK_TILDE: op = GLSLC_OP_BITNOT; break;
        case TK_INC: op = GLSLC_OP_PRE_INC; break;
        case TK_DEC: op = GLSLC_OP_PRE_DEC; break;
        default: return parse_postfix(g);
    }
    g->pos++;
    enter(g, t);
    uint32_t a = parse_unary(g);
    leave(g);
    return make_unary(g, op, t, a);
}

static int binary_op(uint8_t kind, uint8_t* op) {
    static const struct {
        uint8_t kind, op, prec;
    } ops[] = {{TK_OROR, GLSLC_OP_LOGIC_OR, 1},
               {TK_XORXOR, GLSLC_OP_LOGIC_XOR, 2},
               {TK_ANDAND, GLSLC_OP_LOGIC_AND, 3},
               {TK_BAR, GLSLC_OP_OR, 4},
               {TK_CARET, GLSLC_OP_XOR, 5},
               {TK_AMP, GLSLC_OP_AND, 6},
               {TK_EQ, GLSLC_OP_EQ, 7},
               {TK_NE, GLSLC_OP_NE, 7},
               {TK_LT, GLSLC_OP_LT, 8},
               {TK_GT, GLSLC_OP_GT, 8},
               {TK_LE, GLSLC_OP_LE, 8},
               {TK_GE, GLSLC_OP_GE, 8},
               {TK_SHL, GLSLC_OP_SHL, 9},
               {TK_SHR, GLSLC_OP_SHR, 9},
               {TK_PLUS, GLSLC_OP_ADD, 10},
               {TK_MINUS, GLSLC_OP_SUB, 10},
               {TK_STAR, GLSLC_OP_MUL, 11},
               {TK_SLASH, GLSLC_OP_DIV, 11},
               {TK_PERCENT, GLSLC_OP_MOD, 11}};
    for (uint32_t i = 0; i < sizeof ops / sizeof ops[0]; i++) {
        if (ops[i].kind == kind) {
            *op = ops[i].op;
            return ops[i].prec;
        }
    }
    return 0;
}

static uint32_t parse_binary(Glslc* g, int min_prec) {
    uint32_t l = parse_unary(g);
    for (;;) {
        uint8_t op = 0;
        int prec = binary_op(kind_at(g, 0), &op);
        if (!prec || prec < min_prec) return l;
        uint32_t t = next_tok(g);
        enter(g, t);
        uint32_t r = parse_binary(g, prec + 1);
        leave(g);
        l = make_binary(g, op, t, l, r);
    }
}

static uint32_t parse_conditional(Glslc* g) {
    uint32_t c = parse_binary(g, 1);
    if (kind_at(g, 0) != TK_QUESTION) return c;
    uint32_t t = next_tok(g);
    enter(g, t);
    uint32_t a = parse_expression(g);
    expect(g, TK_COLON, "':' in a ?: expression");
    uint32_t b = parse_assignment(g);
    leave(g);
    if (nd(g, c)->type != GT_BOOL) {
        char name[64];
        glslc_type_name(g, nd(g, c)->type, name, sizeof name);
        glslc_fail(g, site(g, t), "the condition of ?: is a bool, not %s", name);
    }
    uint32_t at = nd(g, a)->type, bt = nd(g, b)->type;
    if (at != bt) {
        char an[64], bn[64];
        glslc_type_name(g, at, an, sizeof an);
        glslc_type_name(g, bt, bn, sizeof bn);
        glslc_fail(g, site(g, t), "the two sides of ?: are %s and %s, not one type", an, bn);
    }
    if (has_opaque(g, at) || (ty(g, at)->kind == GLSLC_T_ARRAY && g->vi == GLSLC_V100) || at == GT_VOID) {
        char an[64];
        glslc_type_name(g, at, an, sizeof an);
        glslc_fail(g, site(g, t), "?: cannot choose between two %s", an);
    }
    if (nd(g, c)->kind == GLSLC_N_CONST && nd(g, a)->kind == GLSLC_N_CONST && nd(g, b)->kind == GLSLC_N_CONST)
        return make_const(g, t, at, bits_of(g, c) ? bits_of(g, a) : bits_of(g, b));
    uint32_t n = node(g, GLSLC_N_TERNARY, t, at);
    nd(g, n)->a = (uint16_t)c;
    nd(g, n)->b = (uint16_t)a;
    nd(g, n)->c = (uint16_t)b;
    if (is_const(g, c) && is_const(g, a) && is_const(g, b)) nd(g, n)->flags = GLSLC_NF_CONST;
    return n;
}

static uint8_t assign_op(uint8_t kind) {
    switch (kind) {
        case TK_ASSIGN: return GLSLC_OP_ASSIGN;
        case TK_ADD_ASSIGN: return GLSLC_OP_ADD;
        case TK_SUB_ASSIGN: return GLSLC_OP_SUB;
        case TK_MUL_ASSIGN: return GLSLC_OP_MUL;
        case TK_DIV_ASSIGN: return GLSLC_OP_DIV;
        case TK_MOD_ASSIGN: return GLSLC_OP_MOD;
        case TK_SHL_ASSIGN: return GLSLC_OP_SHL;
        case TK_SHR_ASSIGN: return GLSLC_OP_SHR;
        case TK_AND_ASSIGN: return GLSLC_OP_AND;
        case TK_XOR_ASSIGN: return GLSLC_OP_XOR;
        case TK_OR_ASSIGN: return GLSLC_OP_OR;
        default: return 0;
    }
}

static uint32_t parse_assignment(Glslc* g) {
    uint32_t t0 = g->pos;
    enter(g, t0);
    uint32_t l = parse_conditional(g);
    uint8_t op = assign_op(kind_at(g, 0));
    if (!op) {
        leave(g);
        return l;
    }
    uint32_t t = next_tok(g);
    uint32_t r = parse_assignment(g);
    leave(g);
    uint32_t lt = nd(g, l)->type, rt = nd(g, r)->type;
    char ln[64], rn[64];
    glslc_type_name(g, lt, ln, sizeof ln);
    glslc_type_name(g, rt, rn, sizeof rn);
    need_lvalue(g, l, t, "this assignment");
    if (has_opaque(g, lt)) glslc_fail(g, site(g, t), "%s cannot be assigned: it is or holds a sampler or image", ln);
    if (op == GLSLC_OP_ASSIGN) {
        if (lt != rt) glslc_fail(g, site(g, t), "cannot assign %s to %s (GLSL ES converts no value)", rn, ln);
        if (ty(g, lt)->kind == GLSLC_T_ARRAY && g->vi == GLSLC_V100)
            glslc_fail(g, site(g, t), "an array cannot be assigned in GLSL ES 1.00");
    } else {
        if (g->vi == GLSLC_V100 && (op == GLSLC_OP_MOD || op == GLSLC_OP_SHL || op == GLSLC_OP_SHR ||
                                    op == GLSLC_OP_AND || op == GLSLC_OP_OR || op == GLSLC_OP_XOR))
            glslc_fail(g, site(g, t), "operator %s= needs GLSL ES 3.00 (1.00 reserves it)", op_text(op));
        if (binary_type(g, op, lt, rt) != lt)
            glslc_fail(g, site(g, t), "no operator %s= takes %s and %s", op_text(op), ln, rn);
    }
    uint32_t n = node(g, GLSLC_N_ASSIGN, t, lt);
    nd(g, n)->op = op;
    nd(g, n)->a = (uint16_t)l;
    nd(g, n)->b = (uint16_t)r;
    return n;
}

static uint32_t parse_expression(Glslc* g) {
    uint32_t e = parse_assignment(g);
    while (kind_at(g, 0) == TK_COMMA) {
        uint32_t t = next_tok(g);
        uint32_t r = parse_assignment(g);
        uint32_t n = node(g, GLSLC_N_COMMA, t, nd(g, r)->type);
        nd(g, n)->a = (uint16_t)e;
        nd(g, n)->b = (uint16_t)r;
        if (is_const(g, e) && is_const(g, r)) nd(g, n)->flags = GLSLC_NF_CONST;
        e = n;
    }
    return e;
}

/* declarations */

static void check_interface(Glslc* g, const Qual* q, uint32_t type, uint32_t name_tok) {
    uint32_t e = element_of(g, type);
    const GlslcType* et = ty(g, e);
    char name[64];
    glslc_type_name(g, type, name, sizeof name);
    int in = q->storage == GLSLC_ST_IN, vertex = g->stage == SNX_GLSLC_VERTEX,
        fragment = g->stage == SNX_GLSLC_FRAGMENT;
    if (g->stage == SNX_GLSLC_COMPUTE)
        glslc_fail(g, site(g, q->tok), "a compute shader has no %s variables", in ? "in" : "out");
    if (q->legacy == 1 && !vertex) glslc_fail(g, site(g, q->tok), "attribute is only for vertex shaders");
    if (!q->legacy && g->vi == GLSLC_V100)
        glslc_fail(g, site(g, q->tok), "%s at global scope needs GLSL ES 3.00 (1.00 has attribute and varying)",
                   in ? "in" : "out");
    if (q->legacy && et->base != SNX_GLSLC_BASE_FLOAT)
        glslc_fail(g, site(g, name_tok), "%s %.*s is %s: it takes float types only",
                   q->legacy == 1 ? "attribute" : "varying", TOKV(g, name_tok), name);
    if (!is_svm(g, e) || et->base == SNX_GLSLC_BASE_BOOL)
        glslc_fail(g, site(g, name_tok), "%s %.*s is %s: %s variables are numbers, vectors or matrices",
                   in ? "in" : "out", TOKV(g, name_tok), name, in ? "in" : "out");
    if (ty(g, type)->kind == GLSLC_T_ARRAY && in && vertex)
        glslc_fail(g, site(g, name_tok), "a vertex input cannot be an array");
    if (q->location >= 0 && g->vi == GLSLC_V300 && ((vertex && !in) || (fragment && in)))
        glslc_fail(g, site(g, q->tok), "layout(location) on a %s needs GLSL ES 3.10",
                   vertex ? "vertex output" : "fragment input");
    if (fragment && !in && is_matrix(g, e)) glslc_fail(g, site(g, name_tok), "a fragment output cannot be a matrix");
    if (et->base != SNX_GLSLC_BASE_FLOAT && ((fragment && in) || (vertex && !in)) && !(q->qual & GLSLC_Q_FLAT))
        glslc_fail(g, site(g, name_tok), "%.*s is %s: an integer %s must be flat", TOKV(g, name_tok), name,
                   vertex ? "vertex output" : "fragment input");
    if ((q->qual & (GLSLC_Q_FLAT | GLSLC_Q_SMOOTH | GLSLC_Q_CENTROID | GLSLC_Q_SAMPLE)) &&
        ((vertex && in) || (fragment && !in)))
        glslc_fail(g, site(g, q->tok), "interpolation qualifiers are for vertex outputs and fragment inputs");
}

/* declares the variable name of type with q; init is its initializer node or 0 */
static uint32_t declare_variable(Glslc* g, const Qual* q, uint32_t type, uint32_t name_tok, uint32_t init, int global) {
    char name[64];
    glslc_type_name(g, type, name, sizeof name);
    if (element_of(g, type) == GT_VOID) glslc_fail(g, site(g, name_tok), "%.*s cannot be void", TOKV(g, name_tok));
    const GlslcType* t = ty(g, type);
    if (t->kind == GLSLC_T_ARRAY && t->len == 0) {
        if (!init || ty(g, nd(g, init)->type)->kind != GLSLC_T_ARRAY || ty(g, nd(g, init)->type)->elem != t->elem)
            glslc_fail(g, site(g, name_tok), "the array %.*s needs a size, or an initializer that gives one",
                       TOKV(g, name_tok));
        type = nd(g, init)->type;
        glslc_type_name(g, type, name, sizeof name);
    }
    uint8_t storage = q->storage ? q->storage : (global ? GLSLC_ST_GLOBAL : GLSLC_ST_LOCAL);
    if (init) {
        uint32_t it = nd(g, init)->type;
        if (storage != GLSLC_ST_GLOBAL && storage != GLSLC_ST_LOCAL && storage != GLSLC_ST_CONST)
            glslc_fail(g, site(g, name_tok), "%.*s cannot be initialized: it is %s", TOKV(g, name_tok),
                       storage == GLSLC_ST_UNIFORM  ? "a uniform"
                       : storage == GLSLC_ST_SHARED ? "shared"
                                                    : "an interface variable");
        if (has_opaque(g, type)) glslc_fail(g, site(g, name_tok), "%s cannot be initialized", name);
        if (it != type) {
            char iname[64];
            glslc_type_name(g, it, iname, sizeof iname);
            glslc_fail(g, site(g, name_tok), "cannot initialize %s %.*s with %s (GLSL ES converts no value)", name,
                       TOKV(g, name_tok), iname);
        }
        if ((global || storage == GLSLC_ST_CONST) && !is_const(g, init))
            glslc_fail(g, site(g, nd(g, init)->tok), "the initializer of %.*s must be a constant expression",
                       TOKV(g, name_tok));
    } else if (storage == GLSLC_ST_CONST) {
        glslc_fail(g, site(g, name_tok), "the constant %.*s needs an initializer", TOKV(g, name_tok));
    }
    if (global) {
        switch (storage) {
            case GLSLC_ST_IN:
            case GLSLC_ST_OUT: check_interface(g, q, type, name_tok); break;
            case GLSLC_ST_BUFFER:
                glslc_fail(g, site(g, q->tok), "a buffer variable is a block's member: buffer Name { ... };");
                break;
            case GLSLC_ST_SHARED:
                if (g->stage != SNX_GLSLC_COMPUTE)
                    glslc_fail(g, site(g, q->tok), "shared variables are only in compute shaders");
                if (has_opaque(g, type)) glslc_fail(g, site(g, name_tok), "a shared variable cannot be %s", name);
                break;
            case GLSLC_ST_PARAM_INOUT: glslc_fail(g, site(g, q->tok), "inout is only for parameters"); break;
            case GLSLC_ST_GLOBAL:
            case GLSLC_ST_CONST:
                if (has_opaque(g, type))
                    glslc_fail(g, site(g, name_tok), "%s %.*s must be a uniform", name, TOKV(g, name_tok));
                break;
            default: break;
        }
        if (ty(g, element_of(g, type))->kind == GLSLC_T_IMAGE) {
            if (!q->format)
                glslc_fail(g, site(g, name_tok), "the image %.*s needs a format: layout(rgba8) and the like",
                           TOKV(g, name_tok));
            uint8_t fmt_base = q->format <= GLSLC_FMT_RGBA8_SNORM ? SNX_GLSLC_BASE_FLOAT
                               : q->format <= GLSLC_FMT_R32I      ? SNX_GLSLC_BASE_INT
                                                                  : SNX_GLSLC_BASE_UINT;
            if (fmt_base != ty(g, element_of(g, type))->base)
                glslc_fail(g, site(g, name_tok), "%s %.*s cannot have the format %s", name, TOKV(g, name_tok),
                           glslc_format_names[q->format - 1]);
            int one_channel =
                q->format == GLSLC_FMT_R32F || q->format == GLSLC_FMT_R32I || q->format == GLSLC_FMT_R32UI;
            if (!one_channel && !(q->qual & (GLSLC_Q_READONLY | GLSLC_Q_WRITEONLY)))
                glslc_fail(g, site(g, name_tok),
                           "the image %.*s (format %s) must be readonly or writeonly: only r32f, r32i and r32ui are "
                           "both",
                           TOKV(g, name_tok), glslc_format_names[q->format - 1]);
        } else if (q->format) {
            glslc_fail(g, site(g, name_tok), "an image format is only for images, and %.*s is %s", TOKV(g, name_tok),
                       name);
        }
        if (q->location >= 0 && storage != GLSLC_ST_IN && storage != GLSLC_ST_OUT &&
            !(storage == GLSLC_ST_UNIFORM && g->vi >= GLSLC_V310))
            glslc_fail(g, site(g, q->tok), "layout(location) is for in and out variables");
        if ((q->binding >= 0 || q->set >= 0) && !(storage == GLSLC_ST_UNIFORM && has_opaque(g, type)))
            glslc_fail(g, site(g, q->tok), "layout(binding) and layout(set) are for samplers, images and blocks");
    } else {
        if (q->storage && q->storage != GLSLC_ST_CONST)
            glslc_fail(g, site(g, q->tok), "a variable inside a function takes no storage qualifier but const");
        if (has_opaque(g, type))
            glslc_fail(g, site(g, name_tok), "%s %.*s must be a uniform at global scope", name, TOKV(g, name_tok));
    }
    if (q->has_layout && !global) glslc_fail(g, site(g, q->tok), "layout(...) is only for global declarations");
    if (q->const_in) glslc_fail(g, site(g, q->tok), "const in is for parameters");
    if ((q->qual & GLSLC_Q_INVARIANT) && storage != GLSLC_ST_OUT && !(storage == GLSLC_ST_IN && g->vi == GLSLC_V100))
        glslc_fail(g, site(g, q->tok), "invariant is only for outputs");
    if ((q->qual & (GLSLC_Q_FLAT | GLSLC_Q_SMOOTH | GLSLC_Q_CENTROID | GLSLC_Q_SAMPLE)) && storage != GLSLC_ST_IN &&
        storage != GLSLC_ST_OUT)
        glslc_fail(g, site(g, q->tok), "interpolation qualifiers are for in and out variables");
    if ((q->qual & (GLSLC_Q_READONLY | GLSLC_Q_WRITEONLY | GLSLC_Q_COHERENT | GLSLC_Q_VOLATILE | GLSLC_Q_RESTRICT)) &&
        ty(g, element_of(g, type))->kind != GLSLC_T_IMAGE)
        glslc_fail(g, site(g, q->tok), "memory qualifiers are for images and buffer blocks");
    uint8_t prec = resolve_precision(g, type, q->prec, name_tok);
    uint32_t s = declare(g, tk(g, name_tok)->text, site(g, name_tok), GLSLC_SK_VAR);
    GlslcSymbol* y = &g->syms[s];
    y->type = (uint16_t)type;
    y->storage = storage;
    y->prec = prec;
    y->qual = q->qual;
    if (g->invariant_all && storage == GLSLC_ST_OUT) y->qual |= GLSLC_Q_INVARIANT;
    y->location = q->location;
    y->binding = q->binding;
    y->set = q->set;
    y->packing = q->format;
    return s;
}

static uint32_t parse_declarators(Glslc* g, const Qual* q, uint32_t type, int global, uint32_t* last) {
    uint32_t first = 0, tail = 0;
    do {
        uint32_t name = expect(g, TK_IDENT, "a variable name");
        uint32_t t = parse_array_suffix(g, type);
        uint32_t init = 0;
        if (accept(g, TK_ASSIGN)) init = parse_assignment(g);
        uint32_t s = declare_variable(g, q, t, name, init, global);
        uint32_t n = node(g, global ? GLSLC_N_GLOBAL : GLSLC_N_DECL, name, 0);
        nd(g, n)->a = (uint16_t)s;
        nd(g, n)->b = (uint16_t)init;
        g->syms[s].node = (uint16_t)n;
        if (tail) {
            nd(g, tail)->next = (uint16_t)n;
        } else {
            first = n;
        }
        tail = n;
    } while (accept(g, TK_COMMA));
    expect(g, TK_SEMICOLON, "';' after a declaration");
    *last = tail;
    return first;
}

static int starts_declaration(Glslc* g) {
    uint8_t k = kind_at(g, 0);
    switch (k) {
        case TK_CONST:
        case TK_IN:
        case TK_OUT:
        case TK_INOUT:
        case TK_UNIFORM:
        case TK_BUFFER:
        case TK_SHARED:
        case TK_ATTRIBUTE:
        case TK_VARYING:
        case TK_CENTROID:
        case TK_FLAT:
        case TK_SMOOTH:
        case TK_INVARIANT:
        case TK_PRECISE:
        case TK_SAMPLE:
        case TK_LAYOUT:
        case TK_COHERENT:
        case TK_VOLATILE:
        case TK_RESTRICT:
        case TK_READONLY:
        case TK_WRITEONLY:
        case TK_LOWP:
        case TK_MEDIUMP:
        case TK_HIGHP:
        case TK_STRUCT: return 1;
        default: break;
    }
    if (!(k >= TK_TYPE || is_struct_name(g, g->pos))) return 0;
    uint8_t k1 = kind_at(g, 1);
    if (k1 == TK_IDENT || k1 == TK_RESERVED) return 1; /* the declarator says what is wrong with a reserved word */
    if (k1 != TK_LBRACKET) return 0;
    /* float[2] a; or float[2](...): past the brackets */
    uint32_t i = 1, depth = 0;
    for (;; i++) {
        uint8_t kk = kind_at(g, i);
        if (kk == TK_EOF) return 0;
        if (kk == TK_LBRACKET) depth++;
        if (kk == TK_RBRACKET && --depth == 0 && kind_at(g, i + 1) != TK_LBRACKET) break;
    }
    return kind_at(g, i + 1) == TK_IDENT;
}

static void parse_precision(Glslc* g) {
    uint32_t t = next_tok(g);
    uint8_t p = kind_at(g, 0) == TK_LOWP      ? GLSLC_PREC_LOW
                : kind_at(g, 0) == TK_MEDIUMP ? GLSLC_PREC_MEDIUM
                : kind_at(g, 0) == TK_HIGHP   ? GLSLC_PREC_HIGH
                                              : 0;
    if (!p) glslc_fail(g, site(g, g->pos), "precision takes lowp, mediump or highp, then a type");
    g->pos++;
    uint8_t k = kind_at(g, 0);
    uint32_t type = k >= TK_TYPE ? (uint32_t)(k - TK_TYPE) + 1u : 0;
    if (!type || !(type == GT_FLOAT || type == GT_INT || is_opaque_kind(ty(g, type)->kind))) {
        char found[96];
        describe(g, g->pos, found, sizeof found);
        glslc_fail(g, site(g, g->pos), "a precision statement sets float, int, a sampler or an image, not %s", found);
    }
    g->pos++;
    expect(g, TK_SEMICOLON, "';' after a precision statement");
    if (g->nprecs >= sizeof g->precs / sizeof g->precs[0])
        glslc_fail(g, site(g, t), "the shader has more than the %u precision statements glslc takes",
                   (unsigned)(sizeof g->precs / sizeof g->precs[0]));
    g->precs[g->nprecs].type = (uint16_t)type;
    g->precs[g->nprecs].prec = p;
    g->precs[g->nprecs].scope = (uint8_t)g->nscopes;
    g->nprecs++;
}

/* statements */

/* a { } block; the statement it is, or the function it is the body of, counts the depth */
static uint32_t parse_compound(Glslc* g, int new_scope) {
    uint32_t open = expect(g, TK_LBRACE, "'{'");
    if (new_scope) push_scope(g, open);
    uint32_t first = 0, tail = 0;
    while (kind_at(g, 0) != TK_RBRACE) {
        if (kind_at(g, 0) == TK_EOF) glslc_fail(g, site(g, open), "this { has no } before the end of the source");
        uint32_t last;
        uint32_t s = parse_statement(g, &last);
        if (!s) continue;
        if (tail) {
            nd(g, tail)->next = (uint16_t)s;
        } else {
            first = s;
        }
        tail = last;
    }
    g->pos++;
    if (new_scope) pop_scope(g);
    uint32_t n = node(g, GLSLC_N_BLOCK, open, 0);
    nd(g, n)->a = (uint16_t)first;
    return n;
}

/* the body of an if or a loop: a scope of its own, one node */
static uint32_t parse_substatement(Glslc* g) {
    uint32_t t = g->pos, last;
    push_scope(g, t);
    uint32_t s = parse_statement(g, &last);
    pop_scope(g);
    if (s && s != last) {
        uint32_t b = node(g, GLSLC_N_BLOCK, t, 0);
        nd(g, b)->a = (uint16_t)s;
        return b;
    }
    return s;
}

static uint32_t parse_condition(Glslc* g) {
    /* while (bool b = ...) declares b for the loop */
    if (starts_declaration(g)) {
        Qual q;
        parse_qualifiers(g, &q);
        uint32_t type = parse_type_specifier(g, 0);
        uint32_t name = expect(g, TK_IDENT, "a variable name");
        expect(g, TK_ASSIGN, "'=' and a value: a condition's variable is initialized");
        uint32_t init = parse_assignment(g);
        uint32_t s = declare_variable(g, &q, type, name, init, 0);
        uint32_t n = node(g, GLSLC_N_DECL, name, type);
        nd(g, n)->a = (uint16_t)s;
        nd(g, n)->b = (uint16_t)init;
        g->syms[s].node = (uint16_t)n;
        if (type != GT_BOOL) glslc_fail(g, site(g, name), "a condition is a bool");
        return n;
    }
    uint32_t e = parse_expression(g);
    if (nd(g, e)->type != GT_BOOL) {
        char name[64];
        glslc_type_name(g, nd(g, e)->type, name, sizeof name);
        glslc_fail(g, site(g, nd(g, e)->tok), "a condition is a bool, not %s", name);
    }
    return e;
}

static uint32_t parse_switch(Glslc* g) {
    uint32_t t = next_tok(g);
    expect(g, TK_LPAREN, "'(' after switch");
    uint32_t sel = parse_expression(g);
    expect(g, TK_RPAREN, "')' after the switch's value");
    uint32_t st = nd(g, sel)->type;
    if (st != GT_INT && st != GT_UINT) {
        char name[64];
        glslc_type_name(g, st, name, sizeof name);
        glslc_fail(g, site(g, t), "a switch takes an int or uint, not %s", name);
    }
    uint32_t open = expect(g, TK_LBRACE, "'{' after switch (...)");
    push_scope(g, open);
    g->switches++;
    uint32_t first = 0, tail = 0, last_label = 0, after_label = 0;
    int has_default = 0;
    while (kind_at(g, 0) != TK_RBRACE) {
        if (kind_at(g, 0) == TK_EOF) glslc_fail(g, site(g, open), "this { has no } before the end of the source");
        uint32_t lt = g->pos;
        uint32_t s, last;
        if (kind_at(g, 0) == TK_CASE || kind_at(g, 0) == TK_DEFAULT) {
            int is_default = kind_at(g, 0) == TK_DEFAULT;
            g->pos++;
            uint32_t v = 0;
            if (!is_default) {
                v = parse_conditional(g);
                if (nd(g, v)->kind != GLSLC_N_CONST || nd(g, v)->type != st)
                    glslc_fail(g, site(g, lt), "a case label is a constant of the switch's type");
                for (uint32_t x = first; x; x = g->nodes[x].next) {
                    if (g->nodes[x].kind == GLSLC_N_CASE && g->nodes[x].a && bits_of(g, g->nodes[x].a) == bits_of(g, v))
                        glslc_fail(g, site(g, lt), "case %d is given twice", (int)(int32_t)bits_of(g, v));
                }
            } else {
                if (has_default) glslc_fail(g, site(g, lt), "a switch takes one default");
                has_default = 1;
            }
            expect(g, TK_COLON, "':' after a case label");
            s = node(g, GLSLC_N_CASE, lt, 0);
            nd(g, s)->a = (uint16_t)v;
            last = s;
            last_label = lt;
            after_label = 0;
        } else {
            if (!first) glslc_fail(g, site(g, lt), "a switch's body starts with a case or default label");
            s = parse_statement(g, &last);
            after_label = 1;
            if (!s) continue;
        }
        if (tail) {
            nd(g, tail)->next = (uint16_t)s;
        } else {
            first = s;
        }
        tail = last;
    }
    g->pos++;
    if (last_label && !after_label)
        glslc_fail(g, site(g, last_label), "the last case or default label of a switch has no statement after it");
    g->switches--;
    pop_scope(g);
    uint32_t n = node(g, GLSLC_N_SWITCH, t, 0);
    nd(g, n)->a = (uint16_t)sel;
    nd(g, n)->b = (uint16_t)first;
    return n;
}

static uint32_t parse_local_declaration(Glslc* g, uint32_t* last) {
    Qual q;
    parse_qualifiers(g, &q);
    if (kind_at(g, 0) == TK_SEMICOLON || (q.storage && q.storage != GLSLC_ST_CONST))
        glslc_fail(g, site(g, q.tok), "a variable inside a function takes no storage qualifier but const");
    uint32_t type = parse_type_specifier(g, 0);
    if (accept(g, TK_SEMICOLON)) {
        *last = 0;
        return 0; /* a struct, declared */
    }
    return parse_declarators(g, &q, type, 0, last);
}

static uint32_t parse_statement(Glslc* g, uint32_t* last) {
    uint32_t t = g->pos, n = 0;
    *last = 0;
    enter(g, t);
    switch (kind_at(g, 0)) {
        case TK_LBRACE: n = parse_compound(g, 1); break;
        case TK_SEMICOLON: g->pos++; break;
        case TK_IF: {
            g->pos++;
            expect(g, TK_LPAREN, "'(' after if");
            uint32_t c = parse_expression(g);
            if (nd(g, c)->type != GT_BOOL) {
                char name[64];
                glslc_type_name(g, nd(g, c)->type, name, sizeof name);
                glslc_fail(g, site(g, nd(g, c)->tok), "an if's condition is a bool, not %s", name);
            }
            expect(g, TK_RPAREN, "')' after the if's condition");
            uint32_t a = parse_substatement(g), b = 0;
            if (accept(g, TK_ELSE)) b = parse_substatement(g);
            n = node(g, GLSLC_N_IF, t, 0);
            nd(g, n)->a = (uint16_t)c;
            nd(g, n)->b = (uint16_t)a;
            nd(g, n)->c = (uint16_t)b;
            break;
        }
        case TK_SWITCH: n = parse_switch(g); break;
        case TK_CASE:
        case TK_DEFAULT:
            glslc_fail(g, site(g, t), "a case or default label belongs directly in a switch's body");
            break;
        case TK_WHILE: {
            g->pos++;
            expect(g, TK_LPAREN, "'(' after while");
            push_scope(g, t);
            uint32_t c = parse_condition(g);
            expect(g, TK_RPAREN, "')' after the while's condition");
            g->loops++;
            uint32_t body = kind_at(g, 0) == TK_LBRACE ? parse_compound(g, 0) : parse_substatement(g);
            g->loops--;
            pop_scope(g);
            n = node(g, GLSLC_N_WHILE, t, 0);
            nd(g, n)->a = (uint16_t)c;
            nd(g, n)->b = (uint16_t)body;
            break;
        }
        case TK_DO: {
            g->pos++;
            g->loops++;
            uint32_t body = parse_substatement(g);
            g->loops--;
            expect(g, TK_WHILE, "while after do's body");
            expect(g, TK_LPAREN, "'(' after while");
            uint32_t c = parse_expression(g);
            if (nd(g, c)->type != GT_BOOL) glslc_fail(g, site(g, nd(g, c)->tok), "a do-while's condition is a bool");
            expect(g, TK_RPAREN, "')' after the condition");
            expect(g, TK_SEMICOLON, "';' after do-while");
            n = node(g, GLSLC_N_DO, t, 0);
            nd(g, n)->a = (uint16_t)body;
            nd(g, n)->b = (uint16_t)c;
            break;
        }
        case TK_FOR: {
            g->pos++;
            expect(g, TK_LPAREN, "'(' after for");
            push_scope(g, t);
            uint32_t init = 0, cond = 0, step = 0, il;
            if (!accept(g, TK_SEMICOLON)) {
                if (starts_declaration(g)) {
                    init = parse_local_declaration(g, &il);
                    if (init && init != il) {
                        uint32_t b = node(g, GLSLC_N_BLOCK, t, 0);
                        nd(g, b)->a = (uint16_t)init;
                        init = b;
                    }
                } else {
                    uint32_t e = parse_expression(g);
                    expect(g, TK_SEMICOLON, "';' after the for's first part");
                    init = node(g, GLSLC_N_EXPR, nd(g, e)->tok, 0);
                    nd(g, init)->a = (uint16_t)e;
                }
            }
            if (kind_at(g, 0) != TK_SEMICOLON) cond = parse_condition(g);
            expect(g, TK_SEMICOLON, "';' after the for's condition");
            if (kind_at(g, 0) != TK_RPAREN) step = parse_expression(g);
            expect(g, TK_RPAREN, "')' after for (...)");
            g->loops++;
            uint32_t body = kind_at(g, 0) == TK_LBRACE ? parse_compound(g, 0) : parse_substatement(g);
            g->loops--;
            pop_scope(g);
            uint32_t rest = node(g, GLSLC_N_FOR_REST, t, 0);
            nd(g, rest)->a = (uint16_t)step;
            nd(g, rest)->b = (uint16_t)body;
            n = node(g, GLSLC_N_FOR, t, 0);
            nd(g, n)->a = (uint16_t)init;
            nd(g, n)->b = (uint16_t)cond;
            nd(g, n)->c = (uint16_t)rest;
            break;
        }
        case TK_BREAK:
            g->pos++;
            if (!g->loops && !g->switches) glslc_fail(g, site(g, t), "break is only inside a loop or a switch");
            expect(g, TK_SEMICOLON, "';' after break");
            n = node(g, GLSLC_N_BREAK, t, 0);
            break;
        case TK_CONTINUE:
            g->pos++;
            if (!g->loops) glslc_fail(g, site(g, t), "continue is only inside a loop");
            expect(g, TK_SEMICOLON, "';' after continue");
            n = node(g, GLSLC_N_CONTINUE, t, 0);
            break;
        case TK_DISCARD:
            g->pos++;
            if (g->stage != SNX_GLSLC_FRAGMENT) glslc_fail(g, site(g, t), "discard is only in fragment shaders");
            expect(g, TK_SEMICOLON, "';' after discard");
            n = node(g, GLSLC_N_DISCARD, t, 0);
            break;
        case TK_RETURN: {
            g->pos++;
            uint32_t ret = g->syms[g->func].type, v = 0;
            char rn[64];
            glslc_type_name(g, ret, rn, sizeof rn);
            if (kind_at(g, 0) != TK_SEMICOLON) {
                v = parse_expression(g);
                if (ret == GT_VOID) glslc_fail(g, site(g, t), "this function returns void, and return gives a value");
                if (nd(g, v)->type != ret) {
                    char vn[64];
                    glslc_type_name(g, nd(g, v)->type, vn, sizeof vn);
                    glslc_fail(g, site(g, t), "return gives %s, and the function returns %s", vn, rn);
                }
            } else if (ret != GT_VOID) {
                glslc_fail(g, site(g, t), "return needs a value: the function returns %s", rn);
            }
            expect(g, TK_SEMICOLON, "';' after return");
            g->func_returns = 1;
            n = node(g, GLSLC_N_RETURN, t, 0);
            nd(g, n)->a = (uint16_t)v;
            break;
        }
        case TK_PRECISION: parse_precision(g); break;
        default:
            if (starts_declaration(g)) {
                n = parse_local_declaration(g, last);
                leave(g);
                return n;
            } else {
                uint32_t e = parse_expression(g);
                expect(g, TK_SEMICOLON, "';' after an expression");
                n = node(g, GLSLC_N_EXPR, t, 0);
                nd(g, n)->a = (uint16_t)e;
            }
    }
    leave(g);
    *last = n;
    return n;
}

/* global declarations */

static void add_global(Glslc* g, uint32_t first, uint32_t last) {
    if (!first) return;
    if (g->last_decl) {
        nd(g, g->last_decl)->next = (uint16_t)first;
    } else {
        g->first_decl = (uint16_t)first;
    }
    g->last_decl = (uint16_t)last;
}

static void parse_block(Glslc* g, const Qual* q) {
    uint32_t name_tok = next_tok(g);
    uint8_t st = q->storage;
    int io = st == GLSLC_ST_IN || st == GLSLC_ST_OUT, vertex = g->stage == SNX_GLSLC_VERTEX;
    int fragment = g->stage == SNX_GLSLC_FRAGMENT;
    if (io) {
        if (g->vi < GLSLC_V320 && !(g->vi == GLSLC_V310 && g->ext[GLSLC_EXT_IO_BLOCKS] != GLSLC_BEHAVIOR_DISABLE))
            glslc_fail(g, site(g, name_tok),
                       "an in or out block needs GLSL ES 3.20, or 3.10 with GL_EXT_shader_io_blocks, and this shader "
                       "is GLSL ES %s",
                       version_name(g));
        if (g->stage == SNX_GLSLC_COMPUTE || (vertex && st == GLSLC_ST_IN) || (fragment && st == GLSLC_ST_OUT))
            glslc_fail(g, site(g, name_tok),
                       "a %s shader has no %s blocks: in and out blocks are what one stage gives the next",
                       vertex     ? "vertex"
                       : fragment ? "fragment"
                                  : "compute",
                       st == GLSLC_ST_IN ? "in" : "out");
        if ((q->qual & ~(GLSLC_Q_FLAT | GLSLC_Q_SMOOTH | GLSLC_Q_CENTROID | GLSLC_Q_SAMPLE)) || q->binding >= 0 ||
            q->set >= 0 || q->format || q->packing || q->matrix)
            glslc_fail(g, site(g, q->tok), "an in or out block takes interpolation qualifiers and a location only");
    } else {
        need_version(g, name_tok, st == GLSLC_ST_BUFFER ? GLSLC_V310 : GLSLC_V300,
                     st == GLSLC_ST_BUFFER ? "a buffer block" : "a uniform block");
        if (q->qual & ~(GLSLC_Q_READONLY | GLSLC_Q_WRITEONLY | GLSLC_Q_COHERENT | GLSLC_Q_VOLATILE | GLSLC_Q_RESTRICT))
            glslc_fail(g, site(g, q->tok), "a block takes layout, storage and memory qualifiers only");
        if (st == GLSLC_ST_UNIFORM &&
            (q->qual & (GLSLC_Q_READONLY | GLSLC_Q_WRITEONLY | GLSLC_Q_COHERENT | GLSLC_Q_VOLATILE | GLSLC_Q_RESTRICT)))
            glslc_fail(g, site(g, q->tok), "memory qualifiers are for buffer blocks");
        if (q->location >= 0 || q->format) glslc_fail(g, site(g, q->tok), "a block takes no location or format");
    }
    g->pos++; /* { */
    uint32_t t = new_type(g, name_tok);
    g->types[t].kind = GLSLC_T_BLOCK;
    g->types[t].base = SNX_GLSLC_BASE_STRUCT;
    g->types[t].name = tk(g, name_tok)->text;
    g->types[t].first = (uint16_t)g->nmembers;
    uint8_t packing = q->packing ? q->packing : st == GLSLC_ST_BUFFER ? g->buffer_packing : g->uniform_packing;
    uint8_t row_major = q->matrix ? q->matrix == 1 : st == GLSLC_ST_BUFFER ? g->buffer_row_major : g->uniform_row_major;
    if (!packing) packing = st == GLSLC_ST_BUFFER ? GLSLC_PACK_STD430 : GLSLC_PACK_STD140;
    if (io) packing = GLSLC_PACK_NONE; /* an interface, laid out by location, not by offset */
    uint32_t count = 0, unsized_at = 0, located = 0;
    while (kind_at(g, 0) != TK_RBRACE) {
        if (kind_at(g, 0) == TK_EOF) expect(g, TK_RBRACE, "'}' at the end of the block");
        Qual mq;
        parse_qualifiers(g, &mq);
        if (mq.storage && mq.storage != st)
            glslc_fail(g, site(g, mq.tok), "a block member takes its block's storage, if any");
        if (io) {
            if ((mq.qual & ~(GLSLC_Q_FLAT | GLSLC_Q_SMOOTH | GLSLC_Q_CENTROID | GLSLC_Q_SAMPLE)) || mq.binding >= 0 ||
                mq.set >= 0 || mq.format || mq.packing || mq.matrix)
                glslc_fail(g, site(g, mq.tok),
                           "a member of an in or out block takes interpolation qualifiers, a location and a precision "
                           "only");
        } else {
            if (mq.qual &
                ~(GLSLC_Q_READONLY | GLSLC_Q_WRITEONLY | GLSLC_Q_COHERENT | GLSLC_Q_VOLATILE | GLSLC_Q_RESTRICT))
                glslc_fail(g, site(g, mq.tok), "a block member takes no interpolation or invariant qualifier");
            if ((mq.qual & (GLSLC_Q_READONLY | GLSLC_Q_WRITEONLY)) && st != GLSLC_ST_BUFFER)
                glslc_fail(g, site(g, mq.tok), "memory qualifiers are for buffer blocks");
            if (mq.location >= 0 || mq.binding >= 0 || mq.set >= 0 || mq.format || mq.packing)
                glslc_fail(g, site(g, mq.tok), "a block member takes row_major or column_major only");
        }
        uint32_t mt = parse_type_specifier(g, 1);
        int first_declarator = 1;
        do {
            uint32_t mn = expect(g, TK_IDENT, "a member name");
            uint32_t full = parse_array_suffix(g, mt);
            char tname[64];
            glslc_type_name(g, full, tname, sizeof tname);
            if (unsized_at)
                glslc_fail(g, site(g, unsized_at), "only a buffer block's last member may be an array with no size");
            if (ty(g, full)->kind == GLSLC_T_ARRAY && ty(g, full)->len == 0) {
                if (st != GLSLC_ST_BUFFER)
                    glslc_fail(g, site(g, mn),
                               "member %.*s is an array with no size: only a buffer block's last "
                               "member may be one",
                               TOKV(g, mn));
                unsized_at = mn;
            }
            if (element_of(g, full) == GT_VOID) glslc_fail(g, site(g, mn), "member %.*s cannot be void", TOKV(g, mn));
            if (has_opaque(g, full))
                glslc_fail(g, site(g, mn), "member %.*s is %s: a block holds no sampler or image", TOKV(g, mn), tname);
            uint16_t interp =
                (uint16_t)((mq.qual | q->qual) & (GLSLC_Q_FLAT | GLSLC_Q_SMOOTH | GLSLC_Q_CENTROID | GLSLC_Q_SAMPLE));
            if (io) {
                const GlslcType* et = ty(g, element_of(g, full));
                if (!is_svm(g, element_of(g, full)) || et->base == SNX_GLSLC_BASE_BOOL)
                    glslc_fail(g, site(g, mn),
                               "member %.*s is %s: an in or out block holds numbers, vectors and matrices", TOKV(g, mn),
                               tname);
                /* the fragment input is what interpolates: a vertex output member needs no flat (glslang's rule) */
                if (et->base != SNX_GLSLC_BASE_FLOAT && fragment && !(interp & GLSLC_Q_FLAT))
                    glslc_fail(g, site(g, mn), "member %.*s is %s: an integer fragment input must be flat", TOKV(g, mn),
                               tname);
            }
            for (uint32_t m = g->types[t].first; m < g->nmembers; m++) {
                if (glslc_views_equal(g, g->members[m].name, tk(g, mn)->text))
                    glslc_fail(g, site(g, mn), "the block has two members named %.*s", TOKV(g, mn));
            }
            if (g->nmembers >= SNX_GLSLC_MAX_MEMBERS)
                glslc_fail(g, site(g, mn), "the structs and blocks have more than the %u members glslc takes",
                           SNX_GLSLC_MAX_MEMBERS - 1);
            GlslcMember* m = &g->members[g->nmembers++];
            memset(m, 0, sizeof *m);
            m->name = tk(g, mn)->text;
            m->type = (uint16_t)full;
            m->prec = resolve_precision(g, full, mq.prec, mn);
            m->location = -1;
            int rm = mq.matrix ? mq.matrix == 1 : row_major;
            m->mflags = (uint8_t)(rm ? GLSLC_MEM_ROW_MAJOR : GLSLC_MEM_COL_MAJOR);
            if ((mq.qual | q->qual) & GLSLC_Q_READONLY) m->mflags |= GLSLC_MEM_READONLY;
            if ((mq.qual | q->qual) & GLSLC_Q_WRITEONLY) m->mflags |= GLSLC_MEM_WRITEONLY;
            if (interp & GLSLC_Q_FLAT) m->mflags |= GLSLC_MEM_FLAT;
            if (interp & GLSLC_Q_CENTROID) m->mflags |= GLSLC_MEM_CENTROID;
            if (interp & GLSLC_Q_SAMPLE) m->mflags |= GLSLC_MEM_SAMPLE;
            if (io && first_declarator && mq.location >= 0) { /* the declarators after it follow it */
                m->location = mq.location;
                located++;
            }
            first_declarator = 0;
            count++;
        } while (accept(g, TK_COMMA));
        expect(g, TK_SEMICOLON, "';' after a block member");
    }
    g->pos++;
    if (!count) glslc_fail(g, site(g, name_tok), "a block needs at least one member");
    if (io && q->location < 0 && located && located != count)
        glslc_fail(g, site(g, name_tok),
                   "block %.*s has no location, and %u of its %u members have one: every member has one, or none",
                   TOKV(g, name_tok), located, count);
    g->types[t].len = (uint16_t)count;
    g->types[t].dim = packing;
    uint32_t s;
    if (kind_at(g, 0) == TK_IDENT) {
        uint32_t inst = next_tok(g);
        uint32_t full = parse_array_suffix(g, t);
        if (io && full != t && (ty(g, ty(g, full)->elem)->kind != GLSLC_T_BLOCK || located))
            glslc_fail(g, site(g, inst),
                       "an array of in or out blocks has one dimension, and its members take no location of their own");
        if (ty(g, full)->kind == GLSLC_T_ARRAY && ty(g, full)->len == 0)
            glslc_fail(g, site(g, inst), "an array of blocks needs a size");
        s = declare(g, tk(g, inst)->text, site(g, inst), GLSLC_SK_VAR);
        g->syms[s].type = (uint16_t)full;
    } else {
        s = add_symbol(g, tk(g, name_tok)->text, GLSLC_SK_VAR, site(g, name_tok), 0);
        g->syms[s].type = (uint16_t)t;
        for (uint32_t m = 0; m < count; m++) {
            const GlslcMember* mb = &g->members[g->types[t].first + m];
            uint32_t ms = declare(g, mb->name, mb->name.pos, GLSLC_SK_BLOCK_MEMBER);
            g->syms[ms].type = mb->type;
            g->syms[ms].storage = st;
            g->syms[ms].prec = mb->prec;
            g->syms[ms].aux = (uint16_t)s;
            g->syms[ms].aux2 = (uint16_t)m;
        }
    }
    expect(g, TK_SEMICOLON, "';' after a block");
    GlslcSymbol* y = &g->syms[s];
    y->storage = st;
    y->binding = q->binding;
    y->set = q->set;
    y->location = q->location;
    y->packing = packing;
    y->qual = q->qual;
    uint32_t n = node(g, GLSLC_N_GLOBAL, name_tok, 0);
    nd(g, n)->a = (uint16_t)s;
    y->node = (uint16_t)n;
    add_global(g, n, n);
}

static void qualifier_only(Glslc* g, const Qual* q) {
    if (q->local_size_mask) {
        if (q->storage != GLSLC_ST_IN) glslc_fail(g, site(g, q->tok), "a local size is declared with 'in;'");
        need_version(g, q->tok, GLSLC_V310, "a compute shader's local size");
        for (uint32_t i = 0; i < 3; i++) {
            uint16_t v = (q->local_size_mask & (1u << i)) ? q->local_size[i] : 1;
            if (g->local_size_set && g->local_size[i] != v)
                glslc_fail(g, site(g, q->tok), "local_size_%c is %u here and %u before", (int)('x' + i), v,
                           g->local_size[i]);
            g->local_size[i] = v;
        }
        g->local_size_set = 1;
        return;
    }
    if (q->early) {
        if (q->storage != GLSLC_ST_IN) glslc_fail(g, site(g, q->tok), "early_fragment_tests is declared with 'in;'");
        g->early_fragment_tests = 1;
        return;
    }
    if ((q->storage == GLSLC_ST_UNIFORM || q->storage == GLSLC_ST_BUFFER) && q->has_layout && q->location < 0 &&
        q->binding < 0 && q->set < 0 && !q->format) {
        if (q->storage == GLSLC_ST_UNIFORM) {
            if (q->packing) g->uniform_packing = q->packing;
            if (q->matrix) g->uniform_row_major = q->matrix == 1;
        } else {
            if (q->packing) g->buffer_packing = q->packing;
            if (q->matrix) g->buffer_row_major = q->matrix == 1;
        }
        return;
    }
    glslc_fail(g, site(g, q->tok), "this declaration declares nothing");
}

static void invariant_names(Glslc* g, const Qual* q) {
    if (q->qual != GLSLC_Q_INVARIANT || q->storage || q->has_layout || q->prec)
        glslc_fail(g, site(g, q->tok), "a declaration of names alone takes invariant only");
    do {
        uint32_t t = expect(g, TK_IDENT, "a variable name");
        uint32_t n = variable(g, t);
        uint32_t s = nd(g, n)->kind == GLSLC_N_VAR ? nd(g, n)->a : 0;
        if (!s || g->syms[s].storage != GLSLC_ST_OUT)
            glslc_fail(g, site(g, t), "invariant applies to outputs, and %.*s is not one", TOKV(g, t));
        g->syms[s].qual |= GLSLC_Q_INVARIANT;
    } while (accept(g, TK_COMMA));
    expect(g, TK_SEMICOLON, "';' after the invariant names");
}

static int is_builtin_function(const Glslc* g, GlslcView name) {
    for (uint32_t r = 0; r < glslc_builtin_fn_count; r++) {
        if (glslc_view_is(g, name, glslc_builtin_fns[r].name) && builtin_available(g, &glslc_builtin_fns[r])) return 1;
    }
    return 0;
}

static void parse_function(Glslc* g, const Qual* q, uint32_t ret, uint32_t name_tok) {
    GlslcView name = tk(g, name_tok)->text;
    char rn[64];
    glslc_type_name(g, ret, rn, sizeof rn);
    if (q->storage || q->qual || q->has_layout)
        glslc_fail(g, site(g, q->tok), "a function's return type takes a precision qualifier only");
    if (has_opaque(g, ret)) glslc_fail(g, site(g, name_tok), "a function cannot return %s", rn);
    if (ty(g, ret)->kind == GLSLC_T_ARRAY) need_version(g, name_tok, GLSLC_V300, "a function returning an array");
    uint8_t ret_prec = ret == GT_VOID ? 0 : resolve_precision(g, ret, q->prec, name_tok);
    check_name(g, name, site(g, name_tok));
    if (is_builtin_function(g, name))
        glslc_fail(g, site(g, name_tok),
                   "%.*s is a built-in function, which a GLSL ES shader cannot redefine or "
                   "overload",
                   VIEWV(g, name));
    g->pos++; /* ( */
    uint32_t first = g->nparams, count = 0;
    if (kind_at(g, 0) == TK_TYPE + GT_VOID - 1 && kind_at(g, 1) == TK_RPAREN) {
        g->pos++;
    } else if (kind_at(g, 0) != TK_RPAREN) {
        do {
            Qual pq;
            parse_qualifiers(g, &pq);
            uint8_t st = GLSLC_ST_PARAM_IN;
            uint16_t qual = 0;
            if (pq.storage == GLSLC_ST_CONST) {
                qual = GLSLC_Q_CONST;
            } else if (pq.storage == GLSLC_ST_OUT) {
                st = GLSLC_ST_PARAM_OUT;
            } else if (pq.storage == GLSLC_ST_PARAM_INOUT) {
                st = GLSLC_ST_PARAM_INOUT;
            } else if (pq.storage && pq.storage != GLSLC_ST_IN) {
                glslc_fail(g, site(g, pq.tok), "a parameter takes const, in, out or inout");
            }
            if (pq.qual || pq.has_layout || pq.legacy)
                glslc_fail(g, site(g, pq.tok), "a parameter takes const, in, out, inout and a precision only");
            uint32_t ptok = g->pos;
            uint32_t pt = parse_type_specifier(g, 0);
            uint32_t pname = 0;
            if (kind_at(g, 0) == TK_IDENT) {
                pname = next_tok(g);
                pt = parse_array_suffix(g, pt);
            }
            char pn[64];
            glslc_type_name(g, pt, pn, sizeof pn);
            if (pt == GT_VOID) glslc_fail(g, site(g, ptok), "void is a parameter list alone: f(void)");
            if (ty(g, pt)->kind == GLSLC_T_ARRAY && ty(g, pt)->len == 0)
                glslc_fail(g, site(g, ptok), "a parameter cannot be an array with no size");
            if (has_opaque(g, pt) && st != GLSLC_ST_PARAM_IN)
                glslc_fail(g, site(g, ptok), "%s is an in parameter only", pn);
            if (ty(g, element_of(g, pt))->kind == GLSLC_T_IMAGE)
                glslc_fail(g, site(g, ptok), "glslc passes no image to a function: use the image's uniform in it");
            if (g->nparams >= SNX_GLSLC_MAX_PARAMS)
                glslc_fail(g, site(g, ptok), "the functions have more than the %u parameters glslc takes",
                           SNX_GLSLC_MAX_PARAMS);
            GlslcParam* p = &g->params[g->nparams++];
            memset(p, 0, sizeof *p);
            if (pname) p->name = tk(g, pname)->text;
            p->type = (uint16_t)pt;
            p->storage = st;
            p->qual = qual;
            p->prec = resolve_precision(g, pt, pq.prec, ptok);
            count++;
        } while (accept(g, TK_COMMA));
    }
    expect(g, TK_RPAREN, "')' after the parameters");
    /* the function: one declared before with these parameters, or a new overload */
    uint32_t s = 0;
    for (uint32_t u = g->sym_hash[bucket(g, name)]; u; u = g->syms[u].next) {
        if (!glslc_views_equal(g, g->syms[u].name, name)) continue;
        if (g->syms[u].kind != GLSLC_SK_FUNC)
            glslc_fail(g, site(g, name_tok), "'%.*s' is already declared, and not as a function", VIEWV(g, name));
        if (g->syms[u].aux2 != count) continue;
        uint32_t i;
        for (i = 0; i < count; i++) {
            if (g->params[g->syms[u].aux + i].type != g->params[first + i].type) break;
        }
        if (i == count) {
            s = u;
            break;
        }
    }
    if (s) {
        if (g->syms[s].type != ret)
            glslc_fail(g, site(g, name_tok), "%.*s is declared again with another return type", VIEWV(g, name));
        for (uint32_t i = 0; i < count; i++) {
            if (g->params[g->syms[s].aux + i].storage != g->params[first + i].storage ||
                (g->params[g->syms[s].aux + i].qual & GLSLC_Q_CONST) != (g->params[first + i].qual & GLSLC_Q_CONST))
                glslc_fail(g, site(g, name_tok),
                           "parameter %u of %.*s differs from its earlier declaration in const, "
                           "in, out or inout",
                           i + 1, VIEWV(g, name));
        }
    } else {
        s = declare(g, name, site(g, name_tok), GLSLC_SK_FUNC);
        g->syms[s].type = (uint16_t)ret;
        g->syms[s].prec = ret_prec;
        g->syms[s].aux = (uint16_t)first;
        g->syms[s].aux2 = (uint16_t)count;
    }
    if (glslc_view_is(g, name, "main") && (ret != GT_VOID || count))
        glslc_fail(g, site(g, name_tok), "main is 'void main()'");
    if (accept(g, TK_SEMICOLON)) {
        if (g->syms[s].aux != first) g->nparams = first; /* declared before: its own list stays */
        return;
    }
    if (kind_at(g, 0) != TK_LBRACE) expect(g, TK_LBRACE, "';' or a body after a function's parameters");
    if (g->syms[s].node) glslc_fail(g, site(g, name_tok), "%.*s is defined twice", VIEWV(g, name));
    g->syms[s].aux = (uint16_t)first; /* the definition's list, with its names */
    push_scope(g, name_tok);
    for (uint32_t i = 0; i < count; i++) {
        GlslcParam* p = &g->params[first + i];
        if (!p->name.len) continue;
        uint32_t ps = declare(g, p->name, p->name.pos, GLSLC_SK_PARAM);
        g->syms[ps].type = p->type;
        g->syms[ps].storage = p->storage;
        g->syms[ps].prec = p->prec;
        g->syms[ps].qual = p->qual;
        p->sym = (uint16_t)ps;
    }
    g->func = (uint16_t)s;
    g->func_returns = 0;
    uint32_t n = node(g, GLSLC_N_FUNCTION, name_tok, ret);
    g->syms[s].node = (uint16_t)n; /* defined from its body on: a call inside it is recursion, not a missing body */
    uint32_t body = parse_compound(g, 0);
    pop_scope(g);
    g->func = 0;
    if (ret != GT_VOID && !g->func_returns)
        glslc_fail(g, site(g, name_tok), "%.*s returns %s and has no return statement", VIEWV(g, name), rn);
    nd(g, n)->a = (uint16_t)s;
    nd(g, n)->b = (uint16_t)body;
    add_global(g, n, n);
}

static void parse_global(Glslc* g) {
    if (kind_at(g, 0) == TK_PRECISION) {
        parse_precision(g);
        return;
    }
    Qual q;
    parse_qualifiers(g, &q);
    uint8_t k = kind_at(g, 0);
    if (q.any && k == TK_SEMICOLON) {
        qualifier_only(g, &q);
        g->pos++;
        return;
    }
    if (q.qual == GLSLC_Q_INVARIANT && !q.storage && !q.has_layout && k == TK_IDENT && !is_struct_name(g, g->pos) &&
        kind_at(g, 1) != TK_LBRACE) {
        invariant_names(g, &q);
        return;
    }
    if (k == TK_IDENT && kind_at(g, 1) == TK_LBRACE &&
        (q.storage == GLSLC_ST_UNIFORM || q.storage == GLSLC_ST_BUFFER || q.storage == GLSLC_ST_IN ||
         q.storage == GLSLC_ST_OUT)) {
        parse_block(g, &q);
        return;
    }
    uint32_t t0 = g->pos;
    uint32_t type = parse_type_specifier(g, 0);
    if (accept(g, TK_SEMICOLON)) {
        if (tk(g, t0)->kind != TK_STRUCT || q.any) glslc_fail(g, site(g, t0), "this declaration declares nothing");
        return;
    }
    uint32_t name = expect(g, TK_IDENT, "a name");
    if (kind_at(g, 0) == TK_LPAREN) {
        parse_function(g, &q, type, name);
        return;
    }
    g->pos = name;
    uint32_t last;
    uint32_t first = parse_declarators(g, &q, type, 1, &last);
    add_global(g, first, last);
}

/* after the last declaration: main, the bodies the calls need, and no recursion */
static void check_program(Glslc* g) {
    uint32_t end = site(g, g->ntoks - 1);
    uint32_t main_sym = 0;
    for (uint32_t s = 1; s < g->nsyms; s++) {
        if (g->syms[s].kind == GLSLC_SK_FUNC && glslc_view_is(g, g->syms[s].name, "main") && g->syms[s].node)
            main_sym = s;
    }
    if (!main_sym) glslc_fail(g, end, "the shader has no main function");
    g->syms[main_sym].qual |= GLSLC_Q_USED;
    for (uint32_t i = 0; i < g->ncalls; i++) {
        const GlslcCall* c = &g->calls[i];
        if (!g->syms[c->callee].node)
            glslc_fail(g, site(g, c->tok), "%.*s is called, and never defined", VIEWV(g, g->syms[c->callee].name));
    }
    /* depth-first from each function: reaching one still on the stack is recursion */
    uint16_t* colour = g->walk;
    uint16_t* stack = g->walk + SNX_GLSLC_MAX_SYMBOLS;
    memset(colour, 0, g->nsyms * sizeof *colour);
    for (uint32_t f = 1; f < g->nsyms; f++) {
        if (g->syms[f].kind != GLSLC_SK_FUNC || colour[f]) continue;
        uint32_t depth = 0;
        stack[0] = (uint16_t)f;
        stack[1] = 0;
        colour[f] = 1;
        depth = 1;
        while (depth) {
            uint32_t u = stack[2 * (depth - 1)], e = stack[2 * (depth - 1) + 1];
            for (; e < g->ncalls; e++) {
                if (g->calls[e].caller == u) break;
            }
            if (e == g->ncalls) {
                colour[u] = 2;
                depth--;
                continue;
            }
            stack[2 * (depth - 1) + 1] = (uint16_t)(e + 1);
            uint32_t v = g->calls[e].callee;
            if (colour[v] == 1)
                glslc_fail(g, site(g, g->calls[e].tok), "%.*s is called from inside itself: GLSL ES has no recursion",
                           VIEWV(g, g->syms[v].name));
            if (colour[v] == 0) {
                colour[v] = 1;
                stack[2 * depth] = (uint16_t)v;
                stack[2 * depth + 1] = 0;
                depth++;
            }
        }
    }
    if (g->stage == SNX_GLSLC_COMPUTE && !g->local_size_set)
        glslc_fail(g, end, "a compute shader declares its local size: layout(local_size_x = 64) in;");
}

void glslc_parse(Glslc* g) {
    g->ntypes = glslc_builtin_type_count + 1;
    memset(&g->types[0], 0, sizeof g->types[0]);
    for (uint32_t i = 0; i < glslc_builtin_type_count; i++) {
        const GlslcBuiltinType* b = &glslc_builtin_types[i];
        GlslcType* t = &g->types[i + 1];
        memset(t, 0, sizeof *t);
        t->kind = b->kind;
        t->base = b->base;
        t->rows = b->rows;
        t->cols = b->cols;
        t->dim = b->dim;
        t->tflags = b->tflags;
    }
    g->nscopes = 0;
    push_scope(g, 0);
    g->pos = 0;
    while (kind_at(g, 0) != TK_EOF) {
        if (kind_at(g, 0) == TK_SEMICOLON)
            glslc_fail(g, site(g, g->pos), "a ';' alone is not a declaration in GLSL ES");
        parse_global(g);
    }
    check_program(g);
}
