/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The SDM's pseudocode, parsed: the notation of Vol. 2A, 3.1.1.9 ("Operation Section"): := assignment,
   IF/THEN/ELSE/FI, WHILE/DO/OD, FOR/TO/DO/ROF, CASE/OF/ESAC, (* comments *), #XX(code) for exceptions, register
   names, DEST/SRC operand names and bit fields [hi:lo]. The text is typeset prose rather than a formal language, so the
   parse is lenient, as vibe86's tools/ucc/sdm.py (which this ports) is:

   - a statement ends at ';' or at the end of its line, unless the line ends in an operator;
   - an IF without THEN takes the rest of its line as the condition;
   - 'ELSE IF' opens a nested IF, and an ELSE that finds its IF has one already extends the trailing chain (the manual
     closes some chains early);
   - a condition or a statement that isn't an expression is prose, which the evaluator refuses.

   One thing the parse reads that sdm.py's did not: a comment that says what a division is. SAR's and SARX's loops
   divide by 2 'signed, rounding toward negative infinity' and SHR's 'unsigned', in a comment on the line of the '/'
   (the semantics is in the comment, as vibe86's erratum E-9 found of IMUL, whose '*' is a 'Signed multiplication' in
   one): such a '/' or '*' is marked so (XNode.sign) */
#include "isel.h"
#include "sailgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_ID, T_NUM, T_OP, T_NL, T_KW, T_EXC, T_STR, T_EOF, T_WS };

typedef struct {
    int kind;
    char* text;
    unsigned line, col;
    XU64 value;
    unsigned width;
} Tok;

static const char* const k_keywords[] = {"IF",   "THEN", "ELSE",   "FI",   "WHILE",  "DO",    "OD",
                                         "FOR",  "TO",   "DOWNTO", "ROF",  "CASE",   "OF",    "ESAC",
                                         "REPEAT", "UNTIL", "BREAK", "RETURN", NULL};

static int is_al(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

static char* xdup(const char* s, size_t n) {
    char* d = (char*)sg_alloc(n + 1);
    memcpy(d, s, n);
    return d;
}

/* the typographic characters of the PDF's text, as the notation's */
static char* normalize(const char* text) {
    static const char* const from[] = {"\xE2\x80\x93", "\xE2\x88\x92", "\xE2\x80\x94", "\xE2\x88\x97", "\xC3\x97",
                                       "\xE2\x89\xA0", "\xE2\x89\xA4", "\xE2\x89\xA5", "\xE2\x86\x90", "\xE2\x80\x99",
                                       "\xE2\x80\x98", "\xE2\x80\x9C", "\xE2\x80\x9D", "\xC2\xA0",     "<>",
                                       "\xE2\x89\xAA", "\xE2\x89\xAB", NULL};
    static const char* const to[] = {"-", "-", "-", "*", "*", "!=", "<=", ">=", ":=", "'", "'", "\"", "\"", " ", "!=",
                                     "<<", ">>"};
    size_t n = strlen(text), i = 0, o = 0;
    char* out = (char*)sg_alloc(n + 1);
    while (i < n) {
        int k, hit = 0;
        for (k = 0; from[k]; ++k) {
            size_t l = strlen(from[k]);
            if (!strncmp(text + i, from[k], l)) {
                memcpy(out + o, to[k], strlen(to[k]));
                o += strlen(to[k]);
                i += l;
                hit = 1;
                break;
            }
        }
        if (!hit) out[o++] = text[i++];
    }
    out[o] = 0;
    return out;
}

/* lowercase copy, for reading a comment */
static int comment_says(const char* s, size_t n, const char* what) {
    size_t w = strlen(what), i, j;
    for (i = 0; i + w <= n; ++i) {
        for (j = 0; j < w; ++j) {
            char c = s[i + j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != what[j]) break;
        }
        if (j == w) return 1;
    }
    return 0;
}

/* strips (* nested *) and // comments, keeping the lines; each line's division comment in signs[line] */
static char* strip_comments(const char* text, unsigned char* signs, unsigned max_line) {
    size_t n = strlen(text), i = 0, o = 0, start = 0;
    char* out = (char*)sg_alloc(n + 1);
    int depth = 0;
    unsigned line = 1;
    while (i < n) {
        if (text[i] == '(' && text[i + 1] == '*') {
            if (!depth) start = i;
            ++depth;
            i += 2;
            continue;
        }
        if (depth && text[i] == '*' && text[i + 1] == ')') {
            i += 2;
            if (!--depth && line < max_line) {
                if (comment_says(text + start, i - start, "unsigned divide")) signs[line] = 2;
                else if (comment_says(text + start, i - start, "signed divide") ||
                         comment_says(text + start, i - start, "signed division") ||
                         comment_says(text + start, i - start, "signed multiplication"))
                    signs[line] = 1;
            }
            continue;
        }
        if (!depth && text[i] == '/' && text[i + 1] == '/') {
            start = i;
            while (i < n && text[i] != '\n') ++i;
            if (line < max_line) {
                if (comment_says(text + start, i - start, "unsigned divide")) signs[line] = 2;
                else if (comment_says(text + start, i - start, "signed divide")) signs[line] = 1;
            }
            continue;
        }
        if (text[i] == '\n') {
            out[o++] = '\n';
            ++line;
            ++i;
            continue;
        }
        if (depth) {
            ++i;
            continue;
        }
        out[o++] = text[i++];
    }
    out[o] = 0;
    return out;
}

/* a numeric literal: 1FH, 0000FFFFH, 10B, 64, 0x10; AH, BH, CH and DH are registers */
static int number(const char* w, size_t n, XU64* v, unsigned* width) {
    size_t i;
    *width = 0;
    if (n == 2 && w[1] == 'H' && (w[0] == 'A' || w[0] == 'B' || w[0] == 'C' || w[0] == 'D')) return 0;
    if (n >= 2 && w[n - 1] == 'H' && ((w[0] >= '0' && w[0] <= '9') || n >= 3)) {
        XU64 x = 0;
        for (i = 0; i + 1 < n; ++i) {
            int c = w[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else return 0;
            x = x * 16 + (XU64)d;
        }
        *v = x;
        *width = (unsigned)(n - 1) * 4;
        return 1;
    }
    if (n > 2 && w[n - 1] == 'B') {
        XU64 x = 0;
        for (i = 0; i + 1 < n; ++i) {
            if (w[i] != '0' && w[i] != '1') return 0;
            x = x * 2 + (XU64)(w[i] - '0');
        }
        *v = x;
        *width = (unsigned)(n - 1);
        return 1;
    }
    if (n > 2 && w[0] == '0' && w[1] == 'x') {
        XU64 x = 0;
        for (i = 2; i < n; ++i) {
            int c = w[i], d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else return 0;
            x = x * 16 + (XU64)d;
        }
        *v = x;
        *width = (unsigned)(n - 2) * 4;
        return 1;
    }
    for (i = 0; i < n; ++i)
        if (w[i] < '0' || w[i] > '9') return 0;
    {
        XU64 x = 0;
        for (i = 0; i < n; ++i) x = x * 10 + (XU64)(w[i] - '0');
        *v = x;
    }
    return 1;
}

typedef struct {
    Tok* t;
    size_t n, cap, i;
    XCode* code;
    unsigned char* signs;
    unsigned max_line;
    int failed;
} P;

static void tok_add(P* p, int kind, const char* s, size_t n, unsigned line, unsigned col) {
    Tok t = {kind, xdup(s, n), line, col, 0, 0};
    if (kind == T_ID) {
        size_t k;
        if (number(s, n, &t.value, &t.width)) t.kind = T_NUM;
        else
            for (k = 0; k_keywords[k]; ++k)
                if (strlen(k_keywords[k]) == n && !strncmp(k_keywords[k], s, n)) t.kind = T_KW;
    }
    if (p->n == p->cap) {
        p->cap = p->cap ? p->cap * 2 : 256;
        p->t = (Tok*)realloc(p->t, p->cap * sizeof *p->t);
        if (!p->t) exit(1);
    }
    p->t[p->n++] = t;
}

static void lex(P* p, const char* text) {
    size_t i = 0, n = strlen(text), linestart = 0;
    unsigned line = 1;
    while (i < n) {
        char c = text[i];
        size_t s = i;
        if (c == ' ' || c == '\t' || c == '\r') {
            ++i;
            continue;
        }
        if (c == '\n') {
            if (p->n && p->t[p->n - 1].kind != T_NL) tok_add(p, T_NL, "\n", 1, line, 0);
            ++line;
            linestart = ++i;
            continue;
        }
        if (c == '#' && text[i + 1] >= 'A' && text[i + 1] <= 'Z' && text[i + 2] >= 'A' && text[i + 2] <= 'Z' &&
            !is_al(text[i + 3])) {
            tok_add(p, T_EXC, text + i, 3, line, (unsigned)(i - linestart));
            i += 3;
            continue;
        }
        if (c == '"') {
            ++i;
            while (i < n && text[i] != '"' && text[i] != '\n') ++i;
            if (i < n && text[i] == '"') ++i;
            tok_add(p, T_STR, text + s, i - s, line, (unsigned)(s - linestart));
            continue;
        }
        {
            static const char* const two[] = {":=", "!=", "<=", ">=", "<<", ">>", "==", "&&", "||", NULL};
            int k, hit = 0;
            for (k = 0; two[k]; ++k)
                if (text[i] == two[k][0] && text[i + 1] == two[k][1]) {
                    tok_add(p, T_OP, text + i, 2, line, (unsigned)(i - linestart));
                    i += 2;
                    hit = 1;
                    break;
                }
            if (hit) continue;
        }
        if (strchr("-+*/=<>()[]:,;.{}&|^~!?'", c)) {
            tok_add(p, T_OP, text + i, 1, line, (unsigned)(i - linestart));
            ++i;
            continue;
        }
        if (is_al(c)) {
            while (i < n && is_al(text[i])) ++i;
            if (i - s == 6 && !strncmp(text + s, "ELSEIF", 6)) {
                /* RORX's 'ELSEIF': ELSE IF */
                tok_add(p, T_ID, "ELSE", 4, line, (unsigned)(s - linestart));
                tok_add(p, T_ID, "IF", 2, line, (unsigned)(s - linestart + 4));
                continue;
            }
            tok_add(p, T_ID, text + s, i - s, line, (unsigned)(s - linestart));
            continue;
        }
        /* anything else (a stray character of the PDF): an operator the parse won't take, so prose */
        {
            size_t l = 1;
            if ((unsigned char)c >= 0xC0) {
                while (i + l < n && ((unsigned char)text[i + l] & 0xC0) == 0x80) ++l;
            }
            tok_add(p, T_OP, text + i, l, line, (unsigned)(i - linestart));
            i += l;
        }
    }
    tok_add(p, T_NL, "\n", 1, line, 0);
    tok_add(p, T_EOF, "", 0, line, 0);
}

/* ---- nodes ---- */

static unsigned node(P* p, int kind, const char* text, unsigned a, unsigned b, unsigned c, unsigned line) {
    XCode* code = p->code;
    XNode* x;
    if (code->count + 1 >= code->cap) {
        code->cap = code->cap ? code->cap * 2 : 1024;
        code->nodes = (XNode*)realloc(code->nodes, code->cap * sizeof *code->nodes);
        if (!code->nodes) exit(1);
        if (code->count == 0) code->count = 1; /* index 0 is none */
    }
    if (code->count == 0) code->count = 1;
    x = &code->nodes[code->count];
    memset(x, 0, sizeof *x);
    x->kind = (unsigned char)kind;
    x->text = text ? text : "";
    x->a = a, x->b = b, x->c = c;
    x->line = line;
    return code->count++;
}

/* a name kept for the code's life (sailgen's string table moves as it grows, so not there) */
static const char* intern(P* p, const char* s) {
    XCode* code = p->code;
    if (code->nnames == code->names_cap) {
        code->names_cap = code->names_cap ? code->names_cap * 2 : 256;
        code->names = (char**)realloc(code->names, code->names_cap * sizeof *code->names);
        if (!code->names) {
            fprintf(stderr, "sailgen --isel: out of memory for %u names of the pseudocode\n", code->names_cap);
            exit(1);
        }
    }
    return code->names[code->nnames++] = xdup(s, strlen(s));
}

/* ---- expressions: precedence climbing over tokens [from, to) ---- */

typedef struct {
    P* p;
    size_t i, end;
    int bad;
} E;

static const Tok* epeek(E* e, size_t k) {
    static Tok eof = {T_EOF, (char*)"", 0, 0, 0, 0};
    size_t j = e->i + k;
    while (j < e->end && e->p->t[j].kind == T_NL) ++j; /* expressions span lines */
    if (k) {
        /* the k-th token, newlines skipped */
        size_t m = e->i, seen = 0;
        for (; m < e->end; ++m) {
            if (e->p->t[m].kind == T_NL) continue;
            if (seen++ == k) return &e->p->t[m];
        }
        return &eof;
    }
    return j < e->end ? &e->p->t[j] : &eof;
}
static const Tok* etake(E* e) {
    while (e->i < e->end && e->p->t[e->i].kind == T_NL) ++e->i;
    if (e->i >= e->end) {
        e->bad = 1;
        return epeek(e, 0);
    }
    return &e->p->t[e->i++];
}
static int eis(E* e, const char* s) { return !strcmp(epeek(e, 0)->text, s) && epeek(e, 0)->kind != T_STR; }
static void eexpect(E* e, const char* s) {
    if (!eis(e, s)) e->bad = 1;
    else etake(e);
}

static unsigned binary(E* e, int level);

/* the levels, lowest first; each a list of the operators' spellings */
static const char* const k_prec[][6] = {
    {"OR", "or", "||", NULL}, {"AND", "and", "&&", "bitwiseAND", NULL}, {"|", NULL}, {"XOR", "^", NULL},
    {"&", NULL}, {"=", "==", "!=", NULL}, {"<", ">", "<=", ">=", NULL}, {"<<", ">>", NULL}, {"+", "-", NULL},
    {"*", "/", "MOD", "SignedModulus", NULL}};
#define LEVELS 10

static const char* canon_op(P* p, const char* s) {
    if (!strcmp(s, "or") || !strcmp(s, "||") || !strcmp(s, "|")) return "OR";
    if (!strcmp(s, "and") || !strcmp(s, "&&") || !strcmp(s, "&") || !strcmp(s, "bitwiseAND")) return "AND";
    if (!strcmp(s, "^")) return "XOR";
    if (!strcmp(s, "==")) return "=";
    return intern(p, s);
}

static unsigned primary(E* e);

/* a register's or a segment's name: what may start a pair, DX:AX or SS:RSP (and not a slice's bound, A[i:W - 8]) */
static int is_register(const char* n) {
    static const char* const regs[] = {"AL", "AH", "AX", "EAX", "RAX", "DL", "DH", "DX", "EDX", "RDX", "CL", "CX",
                                       "ECX", "RCX", "BX", "EBX", "RBX", "CS", "DS", "ES", "FS", "GS", "SS", NULL};
    int k;
    for (k = 0; regs[k]; ++k)
        if (!strcmp(n, regs[k])) return 1;
    return 0;
}

static unsigned postfix(E* e, unsigned x) {
    for (;;) {
        if (e->bad) return x;
        if (eis(e, "[")) {
            unsigned a, b;
            etake(e);
            a = binary(e, 0);
            if (eis(e, ":")) {
                etake(e);
                b = binary(e, 0);
                eexpect(e, "]");
                x = node(e->p, X_SLICE, NULL, x, a, b, 0);
            } else {
                eexpect(e, "]");
                x = node(e->p, X_INDEX, NULL, x, a, 0, 0);
            }
        } else if (eis(e, ":") && e->p->code->nodes[x].kind == X_NAME && epeek(e, 1)->kind == T_ID &&
                   is_register(e->p->code->nodes[x].text)) {
            /* a register pair, DX:AX, EDX:EAX, RDX:RAX: their concatenation (and a segment's address, SS:RSP) */
            unsigned y;
            etake(e);
            y = postfix(e, primary(e));
            e->p->code->nodes[x].next = y;
            x = node(e->p, X_CALL, "Concat", x, 0, 0, 0);
        } else
            return x;
    }
}

static unsigned primary(E* e) {
    const Tok* t = etake(e);
    if (e->bad) return 0;
    if (t->kind == T_NUM) {
        unsigned x = node(e->p, X_NUM, NULL, 0, 0, 0, t->line);
        e->p->code->nodes[x].value = t->value;
        e->p->code->nodes[x].width = (unsigned short)t->width;
        return x;
    }
    if (!strcmp(t->text, "(") && t->kind == T_OP) {
        unsigned x = binary(e, 0);
        eexpect(e, ")");
        return x;
    }
    if (!strcmp(t->text, "[") && t->kind == T_OP) { /* NEG's 'DEST := [- (DEST)]' */
        unsigned x = binary(e, 0);
        eexpect(e, "]");
        return x;
    }
    if (t->kind == T_ID || (t->kind == T_KW && !strcmp(t->text, "OF"))) {
        /* OF is CASE's keyword and the overflow flag's name: in an expression, the flag */
        char name[256];
        size_t len = strlen(t->text);
        if (len >= sizeof name) len = sizeof name - 1;
        memcpy(name, t->text, len);
        name[len] = 0;
        /* dotted names: CPUID.80000001H:ECX, CR4.PCIDE, RFLAGS.IF */
        while (eis(e, ".") && (epeek(e, 1)->kind == T_ID || epeek(e, 1)->kind == T_NUM || epeek(e, 1)->kind == T_KW)) {
            const Tok* f;
            etake(e);
            f = etake(e);
            if (len + 1 + strlen(f->text) < sizeof name) {
                name[len++] = '.';
                memcpy(name + len, f->text, strlen(f->text));
                len += strlen(f->text);
                name[len] = 0;
            }
        }
        if (eis(e, "(")) {
            unsigned first = 0, last = 0;
            etake(e);
            if (!eis(e, ")")) {
                for (;;) {
                    unsigned a = binary(e, 0);
                    if (e->bad) return 0;
                    if (last) e->p->code->nodes[last].next = a;
                    else first = a;
                    last = a;
                    if (eis(e, ",")) {
                        etake(e);
                        continue;
                    }
                    break;
                }
            }
            eexpect(e, ")");
            return node(e->p, X_CALL, intern(e->p, name), first, 0, 0, t->line);
        }
        return node(e->p, X_NAME, intern(e->p, name), 0, 0, 0, t->line);
    }
    e->bad = 1;
    return 0;
}

static unsigned unary(E* e) {
    const Tok* t = epeek(e, 0);
    if (t->kind != T_STR && (!strcmp(t->text, "-") || !strcmp(t->text, "NOT") || !strcmp(t->text, "~") ||
                             !strcmp(t->text, "!"))) {
        int neg = !strcmp(t->text, "-");
        unsigned line = t->line;
        etake(e);
        return node(e->p, X_UN, neg ? "-" : "NOT", unary(e), 0, 0, line);
    }
    return postfix(e, primary(e));
}

static unsigned binary(E* e, int level) {
    unsigned x;
    if (level == LEVELS) return unary(e);
    x = binary(e, level + 1);
    for (;;) {
        const Tok* t = epeek(e, 0);
        int k, hit = 0;
        if (e->bad || (t->kind != T_OP && t->kind != T_ID)) return x;
        for (k = 0; k_prec[level][k]; ++k)
            if (!strcmp(t->text, k_prec[level][k])) hit = 1;
        if (!hit) return x;
        {
            unsigned line = t->line, y, z;
            const char* op = canon_op(e->p, t->text);
            size_t at = e->i;
            etake(e);
            y = binary(e, level + 1);
            z = node(e->p, X_BIN, op, x, y, 0, line);
            if (!strcmp(op, "/") || !strcmp(op, "*")) {
                /* the division's or the product's comment, on its line (IMUL's 'Signed multiplication') */
                unsigned l = e->p->t[at].line;
                if (l < e->p->max_line) e->p->code->nodes[z].sign = e->p->signs[l];
            }
            x = z;
        }
    }
}

/* tokens [from, to) as one expression, or prose */
static unsigned expr_or_prose(P* p, size_t from, size_t to) {
    E e = {p, from, to, 0};
    unsigned x;
    size_t k;
    int any = 0;
    for (k = from; k < to; ++k)
        if (p->t[k].kind != T_NL) any = 1;
    if (!any) return node(p, X_PROSE, "", 0, 0, 0, 0);
    x = binary(&e, 0);
    while (e.i < e.end && p->t[e.i].kind == T_NL) ++e.i;
    if (!e.bad && e.i == e.end) return x;
    {
        /* the words, as text */
        size_t len = 0;
        char* s;
        for (k = from; k < to; ++k) len += strlen(p->t[k].text) + 1;
        s = (char*)sg_alloc(len + 1);
        len = 0;
        for (k = from; k < to; ++k) {
            if (p->t[k].kind == T_NL) continue;
            if (len) s[len++] = ' ';
            memcpy(s + len, p->t[k].text, strlen(p->t[k].text));
            len += strlen(p->t[k].text);
        }
        x = node(p, X_PROSE, intern(p, s), 0, 0, 0, p->t[from].line);
        free(s);
        return x;
    }
}

/* ---- statements ---- */

static const Tok* peek(P* p, size_t k) { return &p->t[p->i + k < p->n ? p->i + k : p->n - 1]; }
static const Tok* take(P* p) { return &p->t[p->i < p->n - 1 ? p->i++ : p->i]; }
static int at_kw(P* p, const char* kw) { return peek(p, 0)->kind == T_KW && !strcmp(peek(p, 0)->text, kw); }
static void skip_nl(P* p) {
    while (peek(p, 0)->kind == T_NL || (peek(p, 0)->kind == T_OP && !strcmp(peek(p, 0)->text, ";"))) ++p->i;
}
static void end_stmt(P* p) {
    if (peek(p, 0)->kind == T_OP && !strcmp(peek(p, 0)->text, ";")) take(p);
}
static void fail(P* p, const char* why) {
    if (!p->code->error) {
        p->code->error = why;
        p->code->error_line = peek(p, 0)->line;
    }
    p->failed = 1;
}

static unsigned block(P* p, const char* const* stops);

static size_t line_end_from(P* p, size_t j) {
    while (j < p->n && p->t[j].kind != T_NL && p->t[j].kind != T_EOF && strcmp(p->t[j].text, ";")) ++j;
    return j;
}

/* the tokens of a condition: up to the keyword, or to the end of the line when the next line doesn't continue it */
static void condition(P* p, const char* kw, size_t* from, size_t* to) {
    *from = p->i;
    for (;;) {
        const Tok* t = peek(p, 0);
        if (t->kind == T_EOF) {
            fail(p, "a condition runs off the end");
            *to = p->i;
            return;
        }
        if (t->kind == T_KW && !strcmp(t->text, kw)) {
            *to = p->i;
            take(p);
            return;
        }
        if (t->kind == T_OP && !strcmp(t->text, ";")) {
            size_t j;
            *to = p->i;
            take(p);
            j = p->i;
            while (p->t[j].kind == T_NL) ++j;
            if (p->t[j].kind == T_KW && !strcmp(p->t[j].text, kw)) p->i = j + 1;
            return;
        }
        if (t->kind == T_NL) {
            const Tok* nx = peek(p, 1);
            const Tok* pv = p->i > *from ? &p->t[p->i - 1] : NULL;
            static const char* const cont[] = {"AND", "OR", "and", "or", "(", ")", NULL};
            static const char* const tail[] = {"AND", "OR", "and", "or", "(", ",", "=", "+", "-", NULL};
            int k, go = nx->kind == T_KW && !strcmp(nx->text, kw);
            for (k = 0; cont[k] && !go; ++k)
                if (!strcmp(nx->text, cont[k])) go = 1;
            for (k = 0; pv && tail[k] && !go; ++k)
                if (!strcmp(pv->text, tail[k])) go = 1;
            if (go) {
                take(p);
                continue;
            }
            *to = p->i;
            take(p);
            return;
        }
        take(p);
    }
}

static unsigned if_stmt(P* p, unsigned col);

/* the statements under an ELSE IF chain's head: what follows THEN, then ELSE ... FI */
static void else_part(P* p, unsigned x, unsigned col) {
    static const char* const stop_ef[] = {"FI", "ELSE", NULL};
    XNode* n;
    if (at_kw(p, "FI")) {
        take(p);
        end_stmt(p);
        return;
    }
    if (!at_kw(p, "ELSE")) return; /* no FI: the block ended at its enclosing construct's end */
    {
        unsigned else_col = take(p)->col;
        if (at_kw(p, "IF")) {
            /* an ELSE IF sits at its ELSE's column */
            unsigned inner = if_stmt(p, else_col);
        p->code->nodes[x].c = inner;
        /* a chain closed early: 'ELSE IF a THEN x FI; ELSE IF b ...' */
        for (;;) {
            skip_nl(p);
            if (at_kw(p, "ELSE") && peek(p, 0)->col >= col) {
                unsigned tail = x, target;
                while (p->code->nodes[tail].c && p->code->nodes[p->code->nodes[tail].c].kind == X_IF &&
                       !p->code->nodes[p->code->nodes[tail].c].next && p->code->nodes[p->code->nodes[tail].c].c)
                    tail = p->code->nodes[tail].c;
                n = &p->code->nodes[tail];
                target = n->c && p->code->nodes[n->c].kind == X_IF ? n->c : tail;
                if (p->code->nodes[target].c) return; /* complete: the ELSE is an enclosing IF's */
                {
                    const Tok* et = take(p);
                    if (at_kw(p, "IF")) p->code->nodes[target].c = if_stmt(p, et->col);
                    else
                        p->code->nodes[target].c = block(p, stop_ef);
                }
                continue;
            }
            if (at_kw(p, "FI") && peek(p, 0)->col >= col) {
                take(p);
                end_stmt(p);
            }
            return;
        }
        }
    }
    p->code->nodes[x].c = block(p, stop_ef);
    if (!p->code->nodes[x].c) p->code->nodes[x].c = node(p, X_DO, NULL, node(p, X_PROSE, "", 0, 0, 0, 0), 0, 0, 0);
    if (at_kw(p, "ELSE")) {
        fail(p, "a second ELSE");
        return;
    }
    if (at_kw(p, "FI")) {
        take(p);
        end_stmt(p);
    }
}

static unsigned if_stmt(P* p, unsigned col) {
    static const char* const stop_ef[] = {"ELSE", "FI", NULL};
    const Tok* t = take(p); /* IF */
    unsigned line = t->line, x, cond;
    size_t from, to;
    if (col == ~0u) col = t->col;
    condition(p, "THEN", &from, &to);
    cond = expr_or_prose(p, from, to);
    x = node(p, X_IF, NULL, cond, 0, 0, line);
    p->code->nodes[x].b = block(p, stop_ef);
    else_part(p, x, col);
    return x;
}

static unsigned while_stmt(P* p) {
    static const char* const stop_od[] = {"OD", NULL};
    unsigned line = take(p)->line, cond, body;
    size_t from, to;
    condition(p, "DO", &from, &to);
    cond = expr_or_prose(p, from, to);
    body = block(p, stop_od);
    if (at_kw(p, "OD")) {
        take(p);
        end_stmt(p);
    }
    return node(p, X_WHILE, NULL, cond, body, 0, line);
}

static unsigned for_stmt(P* p) {
    static const char* const stop_rof[] = {"ROF", "OD", NULL};
    unsigned line = take(p)->line, lo, hi, body, x;
    size_t from, to, k, kto = 0;
    condition(p, "DO", &from, &to);
    for (k = from; k < to; ++k)
        if (p->t[k].kind == T_KW && (!strcmp(p->t[k].text, "TO") || !strcmp(p->t[k].text, "DOWNTO"))) {
            kto = k;
            break;
        }
    if (!kto || to - from < 3 || strcmp(p->t[from + 1].text, ":=")) {
        fail(p, "a FOR it can't read");
        return node(p, X_DO, NULL, node(p, X_PROSE, "FOR", 0, 0, 0, line), 0, 0, line);
    }
    lo = expr_or_prose(p, from + 2, kto);
    hi = expr_or_prose(p, kto + 1, to);
    body = block(p, stop_rof);
    if (at_kw(p, "ROF") || at_kw(p, "OD")) {
        take(p);
        end_stmt(p);
    }
    x = node(p, X_FOR, intern(p, p->t[from].text), lo, hi, body, line);
    p->code->nodes[x].down = !strcmp(p->t[kto].text, "DOWNTO");
    return x;
}

static unsigned statement(P* p, const char* const* stops);

static unsigned case_stmt(P* p) {
    static const char* const stop_esac[] = {"ESAC", NULL};
    unsigned line = take(p)->line, subj, first = 0, last = 0;
    size_t from, to;
    condition(p, "OF", &from, &to);
    subj = expr_or_prose(p, from, to);
    for (;;) {
        size_t j;
        unsigned label, body_first = 0, body_last = 0, arm;
        skip_nl(p);
        if (at_kw(p, "ESAC")) {
            take(p);
            end_stmt(p);
            break;
        }
        if (peek(p, 0)->kind == T_EOF) {
            fail(p, "a CASE without ESAC");
            break;
        }
        j = p->i;
        while (strcmp(p->t[j].text, ":") && p->t[j].kind != T_NL && p->t[j].kind != T_EOF) ++j;
        if (strcmp(p->t[j].text, ":")) {
            fail(p, "a CASE arm it can't read");
            break;
        }
        label = (j == p->i + 1 && (!strcmp(p->t[p->i].text, "DEFAULT") || !strcmp(p->t[p->i].text, "default")))
                    ? 0
                    : expr_or_prose(p, p->i, j);
        p->i = j + 1;
        for (;;) {
            size_t k;
            unsigned s;
            skip_nl(p);
            if (at_kw(p, "ESAC") || peek(p, 0)->kind == T_EOF) break;
            k = p->i;
            while (p->t[k].kind != T_NL && p->t[k].kind != T_EOF && strcmp(p->t[k].text, ":") &&
                   strcmp(p->t[k].text, ":=") && strcmp(p->t[k].text, ";"))
                ++k;
            if (!strcmp(p->t[k].text, ":") && strcmp(p->t[k + 1].text, "=")) break;
            s = statement(p, stop_esac);
            if (!s) continue;
            if (body_last) p->code->nodes[body_last].next = s;
            else body_first = s;
            body_last = s;
            while (p->code->nodes[body_last].next) body_last = p->code->nodes[body_last].next;
        }
        arm = node(p, X_ARM, NULL, label, body_first, 0, line);
        if (last) p->code->nodes[last].next = arm;
        else first = arm;
        last = arm;
    }
    return node(p, X_CASE, NULL, subj, first, 0, line);
}

static unsigned statement(P* p, const char* const* stops) {
    const Tok* t = peek(p, 0);
    (void)stops;
    if (t->kind == T_KW && strcmp(t->text, "OF")) { /* 'OF := ...' is the flag's assignment */
        if (!strcmp(t->text, "IF")) return if_stmt(p, ~0u);
        if (!strcmp(t->text, "WHILE")) return while_stmt(p);
        if (!strcmp(t->text, "FOR")) return for_stmt(p);
        if (!strcmp(t->text, "CASE")) return case_stmt(p);
        if (!strcmp(t->text, "DO") && peek(p, 1)->kind == T_KW && !strcmp(peek(p, 1)->text, "WHILE")) {
            /* 'DO WHILE (c) ... OD' (SARX's) */
            take(p);
            return while_stmt(p);
        }
        if (!strcmp(t->text, "BREAK") || !strcmp(t->text, "RETURN")) {
            unsigned line = t->line;
            take(p);
            end_stmt(p);
            return node(p, X_DO, NULL, node(p, X_CALL, intern(p, t->text), 0, 0, 0, line), 0, 0, line);
        }
        fail(p, "a keyword where its construct doesn't want it");
        take(p);
        return 0;
    }
    if (t->kind == T_EXC) {
        unsigned line = t->line, code = 0;
        const char* exc = intern(p, t->text + 1);
        take(p);
        if (peek(p, 0)->kind == T_OP && !strcmp(peek(p, 0)->text, "(")) {
            size_t j = p->i + 1;
            int depth = 1;
            while (depth && p->t[j].kind != T_EOF) {
                if (!strcmp(p->t[j].text, "(")) ++depth;
                else if (!strcmp(p->t[j].text, ")"))
                    --depth;
                ++j;
            }
            code = expr_or_prose(p, p->i + 1, j - 1);
            p->i = j;
        }
        end_stmt(p);
        return node(p, X_RAISE, exc, code, 0, 0, line);
    }
    {
        /* an assignment or a call, up to ';' or the end of the line (a line ending in an operator goes on) */
        size_t start = p->i, j = line_end_from(p, p->i), k;
        int depth = 0;
        static const char* const cont[] = {"+", "-", "*", "AND", "OR", ":=", "(", ",", NULL};
        for (;;) {
            int go = 0, c;
            if (p->t[j].kind != T_NL || j == start) break;
            for (c = 0; cont[c]; ++c)
                if (!strcmp(p->t[j - 1].text, cont[c])) go = 1;
            if (!go) break;
            p->t[j].kind = T_WS;
            j = line_end_from(p, j + 1);
        }
        p->i = j;
        end_stmt(p);
        if (j == start) return 0;
        for (k = start; k < j; ++k) {
            const char* s = p->t[k].text;
            if (p->t[k].kind == T_WS) continue;
            if (!strcmp(s, "(") || !strcmp(s, "[")) ++depth;
            else if (!strcmp(s, ")") || !strcmp(s, "]"))
                --depth;
            else if (depth == 0 && (!strcmp(s, ":=") ||
                                    (!strcmp(s, "=") && k == start + 1 && j > start + 2 && p->t[start].kind == T_ID))) {
                unsigned d = expr_or_prose(p, start, k), v = expr_or_prose(p, k + 1, j);
                return node(p, X_ASSIGN, NULL, d, v, 0, p->t[start].line);
            }
        }
        return node(p, X_DO, NULL, expr_or_prose(p, start, j), 0, 0, p->t[start].line);
    }
}

static unsigned block(P* p, const char* const* stops) {
    unsigned first = 0, last = 0;
    for (;;) {
        const Tok* t;
        unsigned s;
        int k, stop = 0;
        skip_nl(p);
        t = peek(p, 0);
        if (t->kind == T_EOF) return first;
        if (t->kind == T_KW && stops)
            for (k = 0; stops[k]; ++k)
                if (!strcmp(t->text, stops[k])) stop = 1;
        if (stop) return first;
        s = statement(p, stops);
        if (!s) continue;
        if (last) p->code->nodes[last].next = s;
        else first = s;
        last = s;
        while (p->code->nodes[last].next) last = p->code->nodes[last].next;
    }
}

unsigned x_parse(XCode* code, const char* text) {
    P p = {0};
    char* norm = normalize(text);
    unsigned lines = 2, first;
    const char* c;
    char* stripped;
    size_t k;
    for (c = norm; *c; ++c)
        if (*c == '\n') ++lines;
    p.signs = (unsigned char*)sg_alloc(lines + 1);
    p.max_line = lines + 1;
    stripped = strip_comments(norm, p.signs, p.max_line);
    p.code = code;
    lex(&p, stripped);
    first = block(&p, NULL);
    for (k = 0; k < p.n; ++k) free(p.t[k].text);
    free(p.t);
    free(p.signs);
    free(stripped);
    free(norm);
    return first;
}

void x_code_free(XCode* code) {
    for (unsigned i = 0; i < code->nnames; ++i) free(code->names[i]);
    free(code->names);
    free(code->nodes);
    memset(code, 0, sizeof *code);
}

static void dump_expr(const XCode* c, unsigned x, FILE* f) {
    const XNode* n = &c->nodes[x];
    unsigned a;
    if (!x) {
        fputs("<none>", f);
        return;
    }
    switch (n->kind) {
    case X_NUM: fprintf(f, "%llu", n->value); break;
    case X_NAME: fputs(n->text, f); break;
    case X_BIN:
        fputc('(', f);
        dump_expr(c, n->a, f);
        fprintf(f, " %s%s ", n->text, n->sign == 1 ? "s" : n->sign == 2 ? "u" : "");
        dump_expr(c, n->b, f);
        fputc(')', f);
        break;
    case X_UN: fprintf(f, "%s ", n->text), dump_expr(c, n->a, f); break;
    case X_CALL:
        fprintf(f, "%s(", n->text);
        for (a = n->a; a; a = c->nodes[a].next) {
            dump_expr(c, a, f);
            if (c->nodes[a].next) fputs(", ", f);
        }
        fputc(')', f);
        break;
    case X_SLICE: dump_expr(c, n->a, f), fputc('[', f), dump_expr(c, n->b, f), fputc(':', f), dump_expr(c, n->c, f), fputc(']', f); break;
    case X_INDEX: dump_expr(c, n->a, f), fputc('[', f), dump_expr(c, n->b, f), fputc(']', f); break;
    case X_PROSE: fprintf(f, "<<%s>>", n->text); break;
    default: fprintf(f, "<%d>", n->kind); break;
    }
}

void x_dump(const XCode* c, unsigned s, int depth, FILE* f) {
    for (; s; s = c->nodes[s].next) {
        const XNode* n = &c->nodes[s];
        fprintf(f, "%*s%u: ", depth * 2, "", n->line);
        switch (n->kind) {
        case X_ASSIGN: dump_expr(c, n->a, f), fputs(" := ", f), dump_expr(c, n->b, f), fputc('\n', f); break;
        case X_IF:
            fputs("IF ", f), dump_expr(c, n->a, f), fputc('\n', f);
            x_dump(c, n->b, depth + 1, f);
            if (n->c) fprintf(f, "%*sELSE\n", depth * 2, ""), x_dump(c, n->c, depth + 1, f);
            break;
        case X_WHILE: fputs("WHILE ", f), dump_expr(c, n->a, f), fputc('\n', f), x_dump(c, n->b, depth + 1, f); break;
        case X_FOR: fprintf(f, "FOR %s\n", n->text), x_dump(c, n->c, depth + 1, f); break;
        case X_RAISE: fprintf(f, "#%s\n", n->text); break;
        case X_DO: fputs("DO ", f), dump_expr(c, n->a, f), fputc('\n', f); break;
        case X_CASE: fputs("CASE ", f), dump_expr(c, n->a, f), fputc('\n', f); break;
        default: fprintf(f, "<%d>\n", n->kind); break;
        }
    }
}
