/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's parser: the part of Sail that sail-arm's instruction and shared pseudocode is written in. Top-level
   definitions start at column 0 (sail-arm's files and the overlays are written so); the ones sailgen never reads
   (type, union, bitfield, mapping, infix, default, $ directives, a struct with parameters) are kept as text for
   --extract and skipped.
   Anything else the parser does not know stops it with the file, the line and what it found */
#include "sailgen.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

SgParse g_parse;

enum { T_EOF, T_ID, T_NUM, T_BITS, T_STR, T_PUNCT };

static unsigned intern_c(const char* text) { return sg_intern(text, strlen(text)); }

void sg_fail(unsigned node, const char* fmt, ...) {
    va_list ap;
    if (node && node < g_parse.node_count)
        fprintf(stderr, "%s:%u: error: ", sg_str(g_parse.nodes[node].file), g_parse.nodes[node].line);
    else
        fputs("sailgen: error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void fail_at(const SgToken* t, const char* fmt, ...) {
    va_list ap;
    fprintf(stderr, "%s:%u: error: ", g_parse.lex.file->path, t->line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, " (at \"%.*s\")\n", (int)(t->length > 40 ? 40 : t->length), g_parse.lex.file->text + t->start);
    exit(1);
}

static unsigned node(int kind, unsigned line) {
    SgNode* n;
    if (!g_parse.node_count) g_parse.node_count = 1; /* 0 is none */
    if (g_parse.node_count >= g_parse.node_cap) {
        g_parse.node_cap = g_parse.node_cap ? g_parse.node_cap * 2 : 4096;
        g_parse.nodes = (SgNode*)realloc(g_parse.nodes, sizeof(SgNode) * g_parse.node_cap);
        if (!g_parse.nodes) {
            fprintf(stderr, "sailgen: out of memory for the nodes\n");
            exit(1);
        }
    }
    n = &g_parse.nodes[g_parse.node_count];
    memset(n, 0, sizeof *n);
    n->kind = (unsigned char)kind;
    n->line = line;
    n->file = g_parse.lex.file_name;
    return g_parse.node_count++;
}
#define NODE(i) (g_parse.nodes[i])

/* ---- the lexer ---- */

static void skip_space(void) {
    const char* s = g_parse.lex.file->text;
    size_t n = g_parse.lex.file->size;
    for (;;) {
        while (g_parse.lex.pos < n && isspace((unsigned char)s[g_parse.lex.pos])) {
            if (s[g_parse.lex.pos] == '\n') {
                ++g_parse.lex.line;
                g_parse.lex.column = 0;
            } else
                ++g_parse.lex.column;
            ++g_parse.lex.pos;
        }
        if (g_parse.lex.pos + 1 < n && s[g_parse.lex.pos] == '/' && s[g_parse.lex.pos + 1] == '/') {
            while (g_parse.lex.pos < n && s[g_parse.lex.pos] != '\n') ++g_parse.lex.pos;
            continue;
        }
        if (g_parse.lex.pos + 1 < n && s[g_parse.lex.pos] == '/' && s[g_parse.lex.pos + 1] == '*') {
            int depth = 0;
            do { /* Sail's block comments nest */
                if (g_parse.lex.pos + 1 < n && s[g_parse.lex.pos] == '/' && s[g_parse.lex.pos + 1] == '*') {
                    ++depth;
                    g_parse.lex.pos += 2;
                    g_parse.lex.column += 2;
                } else if (g_parse.lex.pos + 1 < n && s[g_parse.lex.pos] == '*' && s[g_parse.lex.pos + 1] == '/') {
                    --depth;
                    g_parse.lex.pos += 2;
                    g_parse.lex.column += 2;
                } else {
                    if (s[g_parse.lex.pos] == '\n') {
                        ++g_parse.lex.line;
                        g_parse.lex.column = 0;
                    } else
                        ++g_parse.lex.column;
                    ++g_parse.lex.pos;
                }
            } while (depth && g_parse.lex.pos < n);
            continue;
        }
        break;
    }
}

static const char* const punct3[] = {"<->", "-->", "..."};
static const char* const punct2[] = {"==", "!=", "<=", ">=", "->", "=>", "..", "<<", ">>", "::", "&&", "||"};

static SgToken lex(void) {
    SgToken t;
    const char* s = g_parse.lex.file->text;
    size_t n = g_parse.lex.file->size, i;
    skip_space();
    t.start = g_parse.lex.pos;
    t.line = g_parse.lex.line;
    t.column = g_parse.lex.column;
    t.length = 0;
    if (g_parse.lex.pos >= n) {
        t.kind = T_EOF;
        return t;
    }
    if (s[g_parse.lex.pos] == '0' && g_parse.lex.pos + 1 < n &&
        (s[g_parse.lex.pos + 1] == 'b' || s[g_parse.lex.pos + 1] == 'x')) {
        size_t e = g_parse.lex.pos + 2;
        while (e < n && (isxdigit((unsigned char)s[e]) || s[e] == '_')) ++e;
        t.kind = T_BITS;
    } else if (isdigit((unsigned char)s[g_parse.lex.pos])) {
        size_t e = g_parse.lex.pos;
        while (e < n && isdigit((unsigned char)s[e])) ++e;
        t.kind = T_NUM;
        t.length = e - g_parse.lex.pos;
    } else if (isalpha((unsigned char)s[g_parse.lex.pos]) || s[g_parse.lex.pos] == '_' || s[g_parse.lex.pos] == '$' ||
               (s[g_parse.lex.pos] == '\'' && g_parse.lex.pos + 1 < n &&
                (isalpha((unsigned char)s[g_parse.lex.pos + 1]) || s[g_parse.lex.pos + 1] == '_'))) {
        size_t e = g_parse.lex.pos + 1;
        while (e < n && (isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == '#')) ++e;
        t.kind = T_ID;
        t.length = e - g_parse.lex.pos;
    } else if (s[g_parse.lex.pos] == '"') {
        size_t e = g_parse.lex.pos + 1;
        while (e < n && s[e] != '"') e += s[e] == '\\' ? 2 : 1;
        t.kind = T_STR;
        t.length = e + 1 - g_parse.lex.pos;
    } else {
        t.kind = T_PUNCT;
        t.length = 1;
        for (i = 0; i < sizeof punct3 / sizeof *punct3; ++i)
            if (!strncmp(s + g_parse.lex.pos, punct3[i], 3)) t.length = 3;
        if (t.length == 1)
            for (i = 0; i < sizeof punct2 / sizeof *punct2; ++i)
                if (!strncmp(s + g_parse.lex.pos, punct2[i], 2)) t.length = 2;
    }
    if (t.kind == T_BITS) {
        size_t e = g_parse.lex.pos + 2;
        while (e < n && (isxdigit((unsigned char)s[e]) || s[e] == '_')) ++e;
        t.length = e - g_parse.lex.pos;
    }
    g_parse.lex.pos += t.length;
    g_parse.lex.column += (unsigned)t.length;
    return t;
}

static const SgToken* peek(void) { return &g_parse.lex.tok; }
static SgToken take(void) {
    SgToken t = g_parse.lex.tok;
    if (g_parse.lex.have_ahead) {
        g_parse.lex.tok = g_parse.lex.ahead;
        g_parse.lex.have_ahead = 0;
    } else
        g_parse.lex.tok = lex();
    return t;
}
static const SgToken* peek2(void) {
    if (!g_parse.lex.have_ahead) {
        g_parse.lex.ahead = lex();
        g_parse.lex.have_ahead = 1;
    }
    return &g_parse.lex.ahead;
}
static int is(const SgToken* t, const char* text) {
    size_t n = strlen(text);
    return (t->kind == T_PUNCT || t->kind == T_ID) && t->length == n &&
           !memcmp(g_parse.lex.file->text + t->start, text, n);
}
static int at(const char* text) { return is(peek(), text); }
static void expect(const char* text) {
    if (!at(text)) fail_at(peek(), "expected \"%s\"", text);
    take();
}
static unsigned tok_str(const SgToken* t) { return sg_intern(g_parse.lex.file->text + t->start, t->length); }
static int top_level(const SgToken* t) { return t->kind == T_EOF || (t->column == 0 && t->kind == T_ID); }

/* ---- types ---- */

static int kind_of_type_name(const char* name) {
    if (!strcmp(name, "bits") || !strcmp(name, "vector") || !strcmp(name, "bitvector")) return K_BITS;
    if (!strcmp(name, "int") || !strcmp(name, "atom") || !strcmp(name, "range") || !strcmp(name, "nat") ||
        !strcmp(name, "integer") || !strcmp(name, "implicit"))
        return K_INT;
    if (!strcmp(name, "bool") || !strcmp(name, "boolean")) return K_BOOL;
    if (!strcmp(name, "bit")) return K_BIT;
    if (!strcmp(name, "unit")) return K_UNIT;
    if (!strcmp(name, "real")) return K_REAL;
    return K_ENUM; /* an enum, or a type sailgen never computes with: the generator checks */
}

static unsigned parse_expr(void);

/* a type: its kind, and for bits(E) the width E as child a; for vector(N, T) the length N as child a and T as child b,
   of kind K_VECTOR when T is bits, K_BITS when it is bit, else K_OTHER. The text is kept as name */
static unsigned parse_type(void) {
    SgToken t = *peek();
    unsigned n = node(N_TYPE, t.line);
    size_t start = t.start;
    if (at("(")) { /* a tuple */
        int depth = 0;
        do {
            if (at("(")) ++depth;
            if (at(")")) --depth;
            take();
        } while (depth && peek()->kind != T_EOF);
        NODE(n).value = K_TUPLE;
    } else if (at("{")) { /* {|32, 64|} or {32, 64}: a set of integers */
        int depth = 0;
        do {
            if (at("{")) ++depth;
            if (at("}")) --depth;
            take();
        } while (depth && peek()->kind != T_EOF);
        NODE(n).value = K_INT;
    } else {
        SgToken name;
        char buffer[64];
        if (peek()->kind != T_ID) fail_at(peek(), "expected a type");
        name = take();
        snprintf(buffer, sizeof buffer, "%.*s", (int)name.length, g_parse.lex.file->text + name.start);
        NODE(n).value = (SgU64)kind_of_type_name(buffer);
        if (at("(")) {
            if (NODE(n).value == K_BITS && !strcmp(buffer, "bits")) {
                take();
                NODE(n).a = parse_expr();
                expect(")");
            } else if (!strcmp(buffer, "vector")) { /* vector(N, [inc or dec,] T) */
                unsigned element, kind;
                take();
                NODE(n).a = parse_expr();
                expect(",");
                if (at("inc") || at("dec")) {
                    take();
                    expect(",");
                }
                element = parse_type();
                expect(")");
                kind = (unsigned)NODE(element).value;
                NODE(n).b = element;
                NODE(n).value = kind == K_BITS ? K_VECTOR : kind == K_BIT ? K_BITS : K_OTHER;
            } else {
                int depth = 0;
                do {
                    if (at("(")) ++depth;
                    if (at(")")) --depth;
                    take();
                } while (depth && peek()->kind != T_EOF);
            }
        }
    }
    NODE(n).name = sg_intern(g_parse.lex.file->text + start, g_parse.lex.tok.start - start);
    return n;
}

/* ---- expressions ---- */

static unsigned list_append(unsigned* first, unsigned last, unsigned item) {
    if (!*first)
        *first = item;
    else
        NODE(last).next = item;
    return item;
}

static unsigned parse_block(void);
static unsigned parse_match(void);

static SgU64 bits_value(const SgToken* t, unsigned* width) {
    const char* s = g_parse.lex.file->text + t->start;
    SgU64 v = 0;
    unsigned w = 0;
    size_t i;
    int hex = s[1] == 'x';
    for (i = 2; i < t->length; ++i) {
        int c = s[i], d;
        if (c == '_') continue;
        d = isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10);
        if (w + (hex ? 4 : 1) > 64) fail_at(t, "a bit literal over 64 bits");
        v = hex ? (v << 4) | (SgU64)d : (v << 1) | (SgU64)d;
        w += hex ? 4 : 1;
    }
    *width = w;
    return v;
}

/* the arguments of a call, after its "(" */
static unsigned parse_args(void) {
    unsigned first = 0, last = 0;
    if (at(")")) {
        take();
        return 0;
    }
    for (;;) {
        unsigned e = parse_expr();
        if (at(":")) { /* an ascription: e : type */
            unsigned typed = node(N_TYPED, NODE(e).line);
            take();
            NODE(typed).a = e;
            NODE(typed).b = parse_type();
            e = typed;
        }
        last = list_append(&first, last, e);
        if (at(",")) {
            take();
            continue;
        }
        expect(")");
        return first;
    }
}

static unsigned parse_primary(void) {
    SgToken t = *peek();
    unsigned n;
    if (t.kind == T_NUM) {
        take();
        n = node(N_NUM, t.line);
        NODE(n).value = strtoull(g_parse.lex.file->text + t.start, NULL, 10);
        return n;
    }
    if (t.kind == T_BITS) {
        unsigned w;
        take();
        n = node(N_BITS, t.line);
        NODE(n).value = bits_value(&t, &w);
        NODE(n).width = (unsigned short)w;
        return n;
    }
    if (t.kind == T_STR) {
        take();
        n = node(N_ID, t.line);
        NODE(n).name = tok_str(&t);
        return n;
    }
    if (at("(")) {
        unsigned first = 0, last = 0, e;
        take();
        if (at(")")) {
            take();
            return node(N_UNIT, t.line);
        }
        e = parse_expr();
        if (at(":")) {
            unsigned typed = node(N_TYPED, t.line);
            take();
            NODE(typed).a = e;
            NODE(typed).b = parse_type();
            e = typed;
        }
        if (at(")")) {
            take();
            return e;
        }
        n = node(N_TUPLE, t.line);
        last = list_append(&first, last, e);
        while (at(",")) {
            take();
            e = parse_expr();
            if (at(":")) {
                unsigned typed = node(N_TYPED, NODE(e).line);
                take();
                NODE(typed).a = e;
                NODE(typed).b = parse_type();
                e = typed;
            }
            last = list_append(&first, last, e);
        }
        expect(")");
        NODE(n).a = first;
        return n;
    }
    if (at("[")) {
        unsigned first = 0, last = 0;
        take();
        n = node(N_VEC, t.line);
        if (!at("]"))
            for (;;) {
                last = list_append(&first, last, parse_expr());
                if (!at(",")) break;
                take();
            }
        expect("]");
        NODE(n).a = first;
        return n;
    }
    if (at("{")) return parse_block();
    if (at("-")) {
        take();
        n = node(N_NEG, t.line);
        NODE(n).a = parse_primary();
        return n;
    }
    if (at("?")) { /* a pattern's binder in sail-arm's "? if ? == EL0": read, never generated */
        take();
        n = node(N_ID, t.line);
        NODE(n).name = intern_c("?");
        return n;
    }
    if (t.kind != T_ID) fail_at(&t, "expected an expression");
    if (at("if")) {
        take();
        n = node(N_IF, t.line);
        NODE(n).a = parse_expr();
        expect("then");
        NODE(n).b = parse_expr();
        if (at("else")) {
            take();
            NODE(n).c = parse_expr();
        }
        return n;
    }
    if (at("match")) return parse_match();
    if (at("let")) { /* let pattern = e in body */
        take();
        n = node(N_LET, t.line);
        NODE(n).a = parse_expr();
        if (at(":")) {
            unsigned typed = node(N_TYPED, t.line);
            take();
            NODE(typed).a = NODE(n).a;
            NODE(typed).b = parse_type();
            NODE(n).a = typed;
        }
        expect("=");
        NODE(n).b = parse_expr();
        expect("in");
        NODE(n).c = parse_expr();
        return n;
    }
    if (at("foreach")) {
        SgToken v;
        take();
        n = node(N_FOREACH, t.line);
        expect("(");
        v = take();
        NODE(n).name = tok_str(&v);
        expect("from");
        NODE(n).a = parse_expr();
        expect("to");
        NODE(n).b = parse_expr();
        if (at("by")) {
            take();
            NODE(n).c = parse_expr();
        }
        if (at("in")) {
            take();
            NODE(n).dec = (unsigned char)at("dec");
            take();
        }
        expect(")");
        NODE(n).d = parse_expr();
        return n;
    }
    if (at("return")) {
        take();
        n = node(N_RETURN, t.line);
        NODE(n).a = at("(") ? parse_primary() : parse_expr();
        return n;
    }
    if (at("throw")) {
        take();
        n = node(N_THROW, t.line);
        NODE(n).a = parse_primary();
        return n;
    }
    if (at("assert")) { /* assert(...), assert(constraint(...)): its arguments are not read */
        int depth = 0;
        take();
        n = node(N_ASSERT, t.line);
        do {
            if (at("(")) ++depth;
            if (at(")")) --depth;
            take();
        } while (depth && peek()->kind != T_EOF);
        return n;
    }
    if (at("true") || at("false")) {
        take();
        n = node(N_BOOL, t.line);
        NODE(n).value = is(&t, "true");
        return n;
    }
    if (at("undefined")) {
        take();
        return node(N_UNDEF, t.line);
    }
    if (at("_")) {
        take();
        return node(N_WILD, t.line);
    }
    take();
    if (at("(")) {
        take();
        n = node(N_CALL, t.line);
        NODE(n).name = tok_str(&t);
        NODE(n).a = parse_args();
        return n;
    }
    n = node(N_ID, t.line);
    NODE(n).name = t.start < g_parse.lex.file->size && g_parse.lex.file->text[t.start] == '\''
                       ? sg_intern(g_parse.lex.file->text + t.start + 1, t.length - 1)
                       : tok_str(&t);
    return n;
}

static unsigned parse_postfix(void) {
    unsigned e = parse_primary();
    for (;;) {
        if (at("[")) {
            unsigned n = node(N_INDEX, NODE(e).line), i;
            take();
            i = parse_expr();
            NODE(n).a = e;
            NODE(n).b = i;
            if (at("..")) {
                take();
                NODE(n).kind = N_SLICE;
                NODE(n).c = parse_expr();
            }
            expect("]");
            e = n;
        } else if (at(".") && peek2()->kind == T_ID) {
            unsigned n = node(N_FIELD, NODE(e).line);
            SgToken f;
            take();
            f = take();
            NODE(n).a = e;
            NODE(n).name = tok_str(&f);
            e = n;
        } else
            return e;
    }
}

/* Sail's fixities: ^ 8 (right), * / % << >> 7, + - 6, @ 5 (right), comparisons 4, & 3 (right), | 2 (right), and the
   pattern's "as" lowest */
static int precedence(const SgToken* t, int* right) {
    static const struct {
        const char* op;
        int level, right;
    } table[] = {{"^", 8, 1},  {"*", 7, 0}, {"/", 7, 0},  {"%", 7, 0},  {"<<", 7, 0}, {">>", 7, 0},
                 {"+", 6, 0},  {"-", 6, 0}, {"@", 5, 1},  {"==", 4, 0}, {"!=", 4, 0}, {"<", 4, 0},
                 {"<=", 4, 0}, {">", 4, 0}, {">=", 4, 0}, {"&", 3, 1},  {"|", 2, 1},  {"as", 1, 0}};
    size_t i;
    for (i = 0; i < sizeof table / sizeof *table; ++i)
        if (is(t, table[i].op)) {
            *right = table[i].right;
            return table[i].level;
        }
    return 0;
}

static unsigned parse_binary(int min_level) {
    unsigned left = parse_postfix();
    for (;;) {
        int right, level = precedence(peek(), &right);
        SgToken op;
        unsigned n;
        if (!level || level < min_level) return left;
        op = take();
        n = node(N_BINOP, op.line);
        NODE(n).name = tok_str(&op);
        NODE(n).a = left;
        NODE(n).b = parse_binary(right ? level : level + 1);
        left = n;
    }
}

static unsigned parse_expr(void) { return parse_binary(1); }

static unsigned parse_match(void) {
    SgToken t = take();
    unsigned n = node(N_MATCH, t.line), first = 0, last = 0;
    NODE(n).a = parse_expr();
    expect("{");
    while (!at("}")) {
        unsigned c = node(N_CASE, peek()->line);
        NODE(c).a = parse_expr();
        if (at("if")) { /* a guard */
            take();
            NODE(c).c = parse_expr();
        }
        expect("=>");
        NODE(c).b = parse_expr();
        last = list_append(&first, last, c);
        if (at(",")) take();
    }
    expect("}");
    NODE(n).b = first;
    return n;
}

/* one item of a block: let, a local's declaration, an assignment or an expression */
static unsigned parse_item(void) {
    SgToken t = *peek();
    unsigned n, e;
    if (at("let")) {
        take();
        n = node(N_LET, t.line);
        NODE(n).a = parse_expr();
        if (at(":")) {
            unsigned typed = node(N_TYPED, t.line);
            take();
            NODE(typed).a = NODE(n).a;
            NODE(typed).b = parse_type();
            NODE(n).a = typed;
        }
        expect("=");
        NODE(n).b = parse_expr();
        if (at("in")) { /* let ... in e, as an item */
            take();
            NODE(n).c = parse_expr();
        }
        return n;
    }
    if (at("var")) take();
    if (t.kind == T_ID && is(peek2(), ":") && !at("let")) {
        SgToken name = take();
        take();
        n = node(N_VAR, t.line);
        NODE(n).name = tok_str(&name);
        NODE(n).c = parse_type();
        expect("=");
        NODE(n).b = parse_expr();
        return n;
    }
    e = parse_expr();
    if (at("=")) {
        take();
        n = node(N_ASSIGN, t.line);
        NODE(n).a = e;
        NODE(n).b = parse_expr();
        return n;
    }
    return e;
}

static unsigned parse_block(void) {
    SgToken t = take(); /* { */
    unsigned n = node(N_BLOCK, t.line), first = 0, last = 0;
    while (!at("}")) {
        last = list_append(&first, last, parse_item());
        if (at(";"))
            take();
        else if (!at("}"))
            fail_at(peek(), "expected \";\" or \"}\" after an item of the block that starts at line %u", t.line);
    }
    take();
    NODE(n).a = first;
    return n;
}

/* ---- top level ---- */

static unsigned def_add(int kind, unsigned name, unsigned line, size_t start) {
    SgDef* d;
    if (!g_parse.def_count) g_parse.def_count = 1;
    if (g_parse.def_count >= g_parse.def_cap) {
        g_parse.def_cap = g_parse.def_cap ? g_parse.def_cap * 2 : 1024;
        g_parse.defs = (SgDef*)realloc(g_parse.defs, sizeof(SgDef) * g_parse.def_cap);
        if (!g_parse.defs) {
            fprintf(stderr, "sailgen: out of memory for the definitions\n");
            exit(1);
        }
    }
    d = &g_parse.defs[g_parse.def_count];
    memset(d, 0, sizeof *d);
    d->kind = (unsigned char)kind;
    d->name = name;
    d->line = line;
    d->file = g_parse.lex.file_name;
    d->text_start = start;
    d->source = (unsigned)(g_parse.lex.file - g_parse.files);
    return g_parse.def_count++;
}

static void skip_to_top_level(void) {
    while (!top_level(peek())) take();
}

/* a val's signature: the kinds of its parameters and of its result. The tokens up to the next definition are kept;
   the result is the type after the last "->" outside parentheses, the parameters the parenthesized list (or the one
   type) right before it. A type's kind is its first word's (parse_type) */
static int kind_at(const SgToken* t) {
    char buffer[64];
    if (is(t, "(")) return K_TUPLE;
    if (is(t, "{")) return K_INT;
    snprintf(buffer, sizeof buffer, "%.*s", (int)(t->length < 63 ? t->length : 63), g_parse.lex.file->text + t->start);
    return kind_of_type_name(buffer);
}

static void parse_val(unsigned d) {
    SgToken tokens[512];
    unsigned count = 0, arrow = 0, i, kinds = 0, params = 0;
    int depth = 0;
    while (!top_level(peek())) {
        if (count == sizeof tokens / sizeof *tokens) fail_at(peek(), "a val over %u tokens", count);
        tokens[count++] = take();
    }
    for (i = 0; i < count; ++i) {
        if (is(&tokens[i], "(") || is(&tokens[i], "{")) ++depth;
        if (is(&tokens[i], ")") || is(&tokens[i], "}")) --depth;
        if (!depth && is(&tokens[i], "->")) arrow = i;
    }
    if (!arrow || arrow + 1 >= count) { /* a value, not a function: val x : T */
        g_parse.defs[d].ret_kind = count ? (unsigned)kind_at(&tokens[count - 1]) : K_NONE;
        return;
    }
    g_parse.defs[d].ret_kind = (unsigned)kind_at(&tokens[arrow + 1]);
    if (is(&tokens[arrow - 1], ")")) {
        unsigned open = arrow - 1, part;
        depth = 0;
        for (;;) {
            if (is(&tokens[open], ")")) ++depth;
            if (is(&tokens[open], "(")) --depth;
            if (!depth || !open) break;
            --open;
        }
        if (open + 1 < arrow - 1) {
            part = open + 1;
            depth = 0;
            for (i = open + 1; i < arrow; ++i) {
                if (is(&tokens[i], "(") || is(&tokens[i], "{")) ++depth;
                if ((is(&tokens[i], ")") || is(&tokens[i], "}")) && depth) --depth;
                if ((!depth && is(&tokens[i], ",")) || i == arrow - 1) {
                    if (params < 4) kinds |= (unsigned)kind_at(&tokens[part]) << (8 * params);
                    ++params;
                    part = i + 1;
                }
            }
        }
    } else {
        /* one parameter: the type's first word is the token after the forall's ".", or the first */
        unsigned first = 0;
        for (i = 0; i < arrow; ++i)
            if (is(&tokens[i], ".")) first = i + 1;
        kinds = (unsigned)kind_at(&tokens[first]);
        params = 1;
    }
    g_parse.defs[d].param_kinds = kinds;
    g_parse.defs[d].param_count = params;
}

static unsigned parse_param_list(void) {
    unsigned first = 0, last = 0;
    if (at("(")) {
        take();
        if (at(")")) {
            take();
            return 0;
        }
        for (;;) {
            unsigned p = parse_expr();
            if (at(":")) {
                unsigned typed = node(N_TYPED, NODE(p).line);
                take();
                NODE(typed).a = p;
                NODE(typed).b = parse_type();
                p = typed;
            }
            last = list_append(&first, last, p);
            if (at(",")) {
                take();
                continue;
            }
            expect(")");
            return first;
        }
    }
    return parse_postfix(); /* function f x = ... */
}

static void parse_top(void) {
    SgToken t = *peek();
    char word[32];
    unsigned d;
    snprintf(word, sizeof word, "%.*s", (int)(t.length < 31 ? t.length : 31), g_parse.lex.file->text + t.start);
    if (!strcmp(word, "function")) {
        SgToken name;
        take();
        if (at("clause")) {
            take();
            name = take();
            d = def_add(D_CLAUSE, tok_str(&name), t.line, t.start);
            expect("(");
            g_parse.defs[d].params = parse_expr();
            if (at("if")) {
                take();
                g_parse.defs[d].guard = parse_expr();
            }
            expect(")");
        } else {
            name = take();
            d = def_add(D_FUNCTION, tok_str(&name), t.line, t.start);
            g_parse.defs[d].params = parse_param_list();
        }
        expect("=");
        g_parse.defs[d].body = parse_expr();
        g_parse.defs[d].text_end = g_parse.lex.tok.start;
        if (!top_level(peek())) fail_at(peek(), "expected the next definition at column 0");
        return;
    }
    if (!strcmp(word, "val")) {
        SgToken name;
        take();
        name = take();
        d = def_add(D_VAL, tok_str(&name), t.line, t.start);
        parse_val(d);
        g_parse.defs[d].text_end = g_parse.lex.tok.start;
        return;
    }
    if (!strcmp(word, "enum")) {
        SgToken name;
        unsigned first = 0, last = 0;
        take();
        name = take();
        d = def_add(D_ENUM, tok_str(&name), t.line, t.start);
        expect("=");
        if (at("{")) take();
        while (peek()->kind == T_ID) {
            SgToken m = take();
            unsigned id = node(N_ID, m.line);
            NODE(id).name = tok_str(&m);
            last = list_append(&first, last, id);
            if (at(",") || at("|"))
                take();
            else
                break;
        }
        if (at("}")) take();
        g_parse.defs[d].params = first;
        g_parse.defs[d].text_end = g_parse.lex.tok.start;
        return;
    }
    if (!strcmp(word, "overload")) {
        SgToken name;
        unsigned first = 0, last = 0;
        take();
        if (at("operator")) take();
        name = take();
        d = def_add(D_OVERLOAD, tok_str(&name), t.line, t.start);
        expect("=");
        expect("{");
        while (peek()->kind == T_ID) {
            SgToken m = take();
            unsigned id = node(N_ID, m.line);
            NODE(id).name = tok_str(&m);
            last = list_append(&first, last, id);
            if (at(",")) take();
        }
        expect("}");
        g_parse.defs[d].params = first;
        g_parse.defs[d].text_end = g_parse.lex.tok.start;
        return;
    }
    if (!strcmp(word, "let") || !strcmp(word, "register")) {
        SgToken name;
        int let = !strcmp(word, "let");
        take();
        name = take();
        d = def_add(let ? D_LET : D_REGISTER, tok_str(&name), t.line, t.start);
        if (at(":")) {
            unsigned type;
            take();
            type = parse_type();
            g_parse.defs[d].ret_kind = (unsigned)NODE(type).value;
            if (!let) g_parse.defs[d].params = type;
        }
        if (at("=")) {
            take();
            g_parse.defs[d].body = parse_expr();
        }
        g_parse.defs[d].text_end = g_parse.lex.tok.start;
        if (!top_level(peek())) fail_at(peek(), "expected the next definition at column 0");
        return;
    }
    if (!strcmp(word, "struct")) { /* struct NAME = { field : type, ... }: its fields in order */
        SgToken name;
        unsigned first = 0, last = 0;
        take();
        name = take();
        if (!at("=")) { /* a struct with parameters: text only */
            d = def_add(D_OTHER, intern_c(word), t.line, t.start);
            skip_to_top_level();
            g_parse.defs[d].text_end = g_parse.lex.tok.start;
            return;
        }
        d = def_add(D_STRUCT, tok_str(&name), t.line, t.start);
        expect("=");
        expect("{");
        while (peek()->kind == T_ID) {
            SgToken f = take();
            unsigned typed = node(N_TYPED, f.line), id = node(N_ID, f.line);
            NODE(id).name = tok_str(&f);
            NODE(typed).a = id;
            expect(":");
            NODE(typed).b = parse_type();
            last = list_append(&first, last, typed);
            if (!at(",")) break;
            take();
        }
        expect("}");
        g_parse.defs[d].params = first;
        g_parse.defs[d].text_end = g_parse.lex.tok.start;
        if (!top_level(peek())) fail_at(peek(), "expected the next definition at column 0");
        return;
    }
    /* type, union, bitfield, mapping, infix, default, end, $ directives: text only */
    take();
    d = def_add(D_OTHER, intern_c(word), t.line, t.start);
    skip_to_top_level();
    g_parse.defs[d].text_end = g_parse.lex.tok.start;
}

void sg_parse_file(const char* path, int overlay) {
    SgFile* f;
    if (g_parse.file_count == sizeof g_parse.files / sizeof *g_parse.files) {
        fprintf(stderr, "sailgen: more than %zu input files\n", sizeof g_parse.files / sizeof *g_parse.files);
        exit(1);
    }
    f = &g_parse.files[g_parse.file_count++];
    f->path = path;
    f->overlay = overlay;
    f->text = sg_read(path, &f->size);
    memset(&g_parse.lex, 0, sizeof g_parse.lex);
    g_parse.lex.file = f;
    g_parse.lex.file_name = intern_c(path);
    g_parse.lex.line = 1;
    g_parse.lex.tok = lex();
    while (peek()->kind != T_EOF) {
        if (peek()->column != 0) fail_at(peek(), "expected a definition at column 0");
        parse_top();
    }
}

unsigned sg_find_next(const char* name, int kind, unsigned after) {
    unsigned i;
    for (i = after + 1; i < g_parse.def_count; ++i)
        if (g_parse.defs[i].kind == kind && !strcmp(sg_str(g_parse.defs[i].name), name)) return i;
    return 0;
}

unsigned sg_find(const char* name, int kind) {
    unsigned i, found = 0;
    for (i = sg_find_next(name, kind, 0); i; i = sg_find_next(name, kind, i))
        if (!found || g_parse.files[g_parse.defs[i].source].overlay) found = i;
    return found;
}
