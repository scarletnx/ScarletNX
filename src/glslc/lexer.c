/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLSL ES source into tokens. A raw scanner finds each token's place in the source; the preprocessor runs the
   directives and expands macros, and what it lets through is classified (which words are keywords depends on the
   version) into g->toks. Macro expansion works on a stack of tokens, g->exp: a use's arguments are expanded, put into
   its body, and the result scanned again with the macro turned off, each step on top of the last; the result is then
   moved down over the steps. A token never copies text: its view is a place in the source, or in g->synth for the
   few values the preprocessor makes (__LINE__, __VERSION__, defined's 0 and 1). */
#include "glslc/internal.h"
#include <stdio.h>
#include <string.h>

#define IF_LIVE    1u /* the group being read is compiled */
#define IF_TAKEN   2u /* a group of this #if was compiled */
#define IF_ELSE    4u /* its #else was seen */
#define IF_DEAD    8u /* the #if is inside a group left out */
#define MAX_PARAMS 64u

static int ident_start(unsigned c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int ident_char(unsigned c) { return ident_start(c) || (c >= '0' && c <= '9'); }
static int digit(unsigned c) { return c >= '0' && c <= '9'; }
static int hex_digit(unsigned c) { return digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static unsigned at(const Glslc* g, uint32_t i) { return i < g->src_len ? (unsigned char)g->src[i] : 0u; }
static int skipping(const Glslc* g) { return g->nifs && !(g->ifs[g->nifs - 1] & IF_LIVE); }

int glslc_parse_int(const char* text, uint32_t len, uint32_t* value) {
    uint64_t v = 0;
    uint32_t i = 0, base = 10;
    if (len >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (len >= 2 && text[0] == '0' && digit((unsigned char)text[1])) {
        base = 8;
        i = 1;
    }
    for (; i < len; i++) {
        unsigned c = (unsigned char)text[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            d = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            d = c - 'A' + 10;
        } else {
            break; /* the u suffix */
        }
        v = v * base + d;
        if (v > 0xffffffffull) return 0;
    }
    *value = (uint32_t)v;
    return 1;
}

/* the length of a line continuation at i (a backslash ending its line), 0 when there is none */
static uint32_t continuation(const Glslc* g, uint32_t i) {
    if (at(g, i) != '\\') return 0;
    if (at(g, i + 1) == '\n') return 2;
    if (at(g, i + 1) == '\r' && at(g, i + 2) == '\n') return 3;
    return 0;
}

static uint8_t number(Glslc* g, uint32_t* pi) {
    uint32_t i = *pi, start = i;
    uint8_t kind = TK_INTCONST;
    if (at(g, i) == '0' && (at(g, i + 1) == 'x' || at(g, i + 1) == 'X')) {
        i += 2;
        if (!hex_digit(at(g, i)) && !skipping(g)) glslc_fail(g, start, "'0x' has no hexadecimal digits after it");
        while (hex_digit(at(g, i))) i++;
    } else {
        while (digit(at(g, i))) i++;
        if (at(g, i) == '.') {
            kind = TK_FLOATCONST;
            i++;
            while (digit(at(g, i))) i++;
        }
        if (at(g, i) == 'e' || at(g, i) == 'E') {
            uint32_t e = i + 1;
            if (at(g, e) == '+' || at(g, e) == '-') e++;
            if (!digit(at(g, e))) {
                if (!skipping(g))
                    glslc_fail(g, start, "the exponent of '%.*s' has no digits", (int)(e - start), g->src + start);
                *pi = e;
                return TK_JUNK;
            }
            kind = TK_FLOATCONST;
            i = e;
            while (digit(at(g, i))) i++;
        }
        if (kind == TK_INTCONST && at(g, start) == '0' && !skipping(g)) {
            for (uint32_t k = start; k < i; k++) {
                if (at(g, k) > '7')
                    glslc_fail(g, start, "'%.*s' starts with 0, so it is octal, and has the digit %c", (int)(i - start),
                               g->src + start, (int)at(g, k));
            }
        }
    }
    if (kind == TK_FLOATCONST && (at(g, i) == 'f' || at(g, i) == 'F')) {
        if (g->vi == GLSLC_V100 && !skipping(g))
            glslc_fail(g, start, "'%.*s': the suffix f needs GLSL ES 3.00 or later", (int)(i + 1 - start),
                       g->src + start);
        i++;
    } else if (kind == TK_INTCONST && (at(g, i) == 'u' || at(g, i) == 'U')) {
        if (g->vi == GLSLC_V100 && !skipping(g))
            glslc_fail(g, start, "'%.*s': the suffix u needs GLSL ES 3.00 or later", (int)(i + 1 - start),
                       g->src + start);
        kind = TK_UINTCONST;
        i++;
    }
    if (ident_char(at(g, i))) {
        while (ident_char(at(g, i))) i++;
        if (!skipping(g)) glslc_fail(g, start, "'%.*s' is not a number", (int)(i - start), g->src + start);
        kind = TK_JUNK;
    }
    *pi = i;
    return kind;
}

static uint8_t punct(Glslc* g, uint32_t* pi) {
    uint32_t i = *pi;
    unsigned c = at(g, i), d = at(g, i + 1), e = at(g, i + 2);
    uint8_t k = TK_JUNK;
    uint32_t n = 1;
    switch (c) {
        case '(': k = TK_LPAREN; break;
        case ')': k = TK_RPAREN; break;
        case '[': k = TK_LBRACKET; break;
        case ']': k = TK_RBRACKET; break;
        case '{': k = TK_LBRACE; break;
        case '}': k = TK_RBRACE; break;
        case '.': k = TK_DOT; break;
        case ',': k = TK_COMMA; break;
        case ':': k = TK_COLON; break;
        case ';': k = TK_SEMICOLON; break;
        case '?': k = TK_QUESTION; break;
        case '~': k = TK_TILDE; break;
        case '+':
            k = d == '+' ? TK_INC : d == '=' ? TK_ADD_ASSIGN : TK_PLUS;
            n = k == TK_PLUS ? 1 : 2;
            break;
        case '-':
            k = d == '-' ? TK_DEC : d == '=' ? TK_SUB_ASSIGN : TK_MINUS;
            n = k == TK_MINUS ? 1 : 2;
            break;
        case '*':
            k = d == '=' ? TK_MUL_ASSIGN : TK_STAR;
            n = k == TK_STAR ? 1 : 2;
            break;
        case '/':
            k = d == '=' ? TK_DIV_ASSIGN : TK_SLASH;
            n = k == TK_SLASH ? 1 : 2;
            break;
        case '%':
            k = d == '=' ? TK_MOD_ASSIGN : TK_PERCENT;
            n = k == TK_PERCENT ? 1 : 2;
            break;
        case '<':
            if (d == '<') {
                k = e == '=' ? TK_SHL_ASSIGN : TK_SHL;
                n = e == '=' ? 3 : 2;
            } else {
                k = d == '=' ? TK_LE : TK_LT;
                n = d == '=' ? 2 : 1;
            }
            break;
        case '>':
            if (d == '>') {
                k = e == '=' ? TK_SHR_ASSIGN : TK_SHR;
                n = e == '=' ? 3 : 2;
            } else {
                k = d == '=' ? TK_GE : TK_GT;
                n = d == '=' ? 2 : 1;
            }
            break;
        case '=':
            k = d == '=' ? TK_EQ : TK_ASSIGN;
            n = d == '=' ? 2 : 1;
            break;
        case '!':
            k = d == '=' ? TK_NE : TK_BANG;
            n = d == '=' ? 2 : 1;
            break;
        case '&':
            k = d == '&' ? TK_ANDAND : d == '=' ? TK_AND_ASSIGN : TK_AMP;
            n = k == TK_AMP ? 1 : 2;
            break;
        case '|':
            k = d == '|' ? TK_OROR : d == '=' ? TK_OR_ASSIGN : TK_BAR;
            n = k == TK_BAR ? 1 : 2;
            break;
        case '^':
            k = d == '^' ? TK_XORXOR : d == '=' ? TK_XOR_ASSIGN : TK_CARET;
            n = k == TK_CARET ? 1 : 2;
            break;
        case '#':
            k = d == '#' ? TK_HASHHASH : TK_HASH;
            n = d == '#' ? 2 : 1;
            break;
        default:
            if (skipping(g)) break;
            if (c == '\\')
                glslc_fail(g, i, "a backslash is only a line continuation at the end of a line, outside a token");
            if (c >= 0x21 && c < 0x7f) glslc_fail(g, i, "the character '%c' is not in GLSL ES's character set", (int)c);
            glslc_fail(g, i, "the byte 0x%02x is not in GLSL ES's character set", c);
    }
    *pi = i + n;
    return k;
}

/* the next raw token at g->cur, with GLSLC_TF_BOL when it starts its line. With line_only, a token that starts a
   line is not read (TK_EOF, with GLSLC_TF_BOL): the directive that looks for its own line's end may be an #if that
   leaves out the next line, whose characters must not be looked at */
static void scan(Glslc* g, GlslcToken* t, int line_only) {
    uint32_t i = g->cur;
    uint8_t bol = g->bol;
    while (i < g->src_len) {
        unsigned c = at(g, i);
        if (c == '\n' || (c == '\r' && at(g, i + 1) != '\n')) { /* a carriage return alone ends a line too */
            i++;
            bol = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\v' || c == '\f') {
            i++;
            continue;
        }
        uint32_t cont = continuation(g, i);
        if (cont) {
            if (g->vi == GLSLC_V100 && !skipping(g))
                glslc_fail(g, i, "a line continuation (a backslash ending a line) needs GLSL ES 3.00 or later");
            i += cont;
            continue;
        }
        if (c == '/' && at(g, i + 1) == '/') {
            i += 2;
            while (i < g->src_len && at(g, i) != '\n' && !(at(g, i) == '\r' && at(g, i + 1) != '\n')) {
                uint32_t k = g->vi != GLSLC_V100 ? continuation(g, i) : 0;
                i += k ? k : 1;
            }
            continue;
        }
        if (c == '/' && at(g, i + 1) == '*') {
            uint32_t start = i;
            i += 2;
            while (i < g->src_len && !(at(g, i) == '*' && at(g, i + 1) == '/')) i++;
            if (i >= g->src_len) glslc_fail(g, start, "this comment has no */ before the end of the source");
            i += 2;
            continue;
        }
        break;
    }
    t->text.pos = (uint16_t)i;
    t->site = (uint16_t)i;
    t->flags = bol ? GLSLC_TF_BOL : 0;
    if (i >= g->src_len || (line_only && bol)) {
        t->kind = TK_EOF;
        t->text.len = 0;
        g->cur = i;
        g->bol = bol;
        return;
    }
    uint32_t start = i;
    unsigned c = at(g, i);
    if (ident_start(c)) {
        while (ident_char(at(g, i))) i++;
        t->kind = TK_IDENT;
    } else if (digit(c) || (c == '.' && digit(at(g, i + 1)))) {
        t->kind = number(g, &i);
    } else {
        t->kind = punct(g, &i);
    }
    t->text.len = (uint16_t)(i - start);
    /* a continuation right after a name or number, before more of one, splices them in C: glslc does not */
    uint32_t cont = continuation(g, i);
    if (cont && (t->kind == TK_IDENT || t->kind == TK_INTCONST || t->kind == TK_FLOATCONST) &&
        ident_char(at(g, i + cont)) && !skipping(g))
        glslc_fail(g, i, "a line continuation inside a name or number is not supported by glslc");
    g->cur = i;
    g->bol = 0;
}

/* the next token of the directive's line; 0 at its end, where the token stays for the next scan */
static int line_next(Glslc* g, GlslcToken* t) {
    uint32_t cur = g->cur;
    uint8_t bol = g->bol;
    scan(g, t, 1);
    if (t->kind == TK_EOF || (t->flags & GLSLC_TF_BOL)) {
        g->cur = cur;
        g->bol = bol;
        return 0;
    }
    return 1;
}

static void skip_line(Glslc* g) {
    GlslcToken t;
    while (line_next(g, &t)) {}
}

static const char* text_of(const Glslc* g, const GlslcToken* t) { return glslc_token_text(g, t); }

static int token_is(const Glslc* g, const GlslcToken* t, const char* word) {
    size_t n = strlen(word);
    return t->text.len == n && !memcmp(text_of(g, t), word, n);
}

static uint32_t synth_add(Glslc* g, const char* text, uint32_t len, uint32_t site) {
    if (g->nsynth + len > SNX_GLSLC_MAX_SYNTH)
        glslc_fail(g, site, "the preprocessor's text (predefined macros, __LINE__'s numbers) needs more than %u bytes",
                   SNX_GLSLC_MAX_SYNTH);
    uint32_t pos = g->nsynth;
    memcpy(g->synth + pos, text, len);
    g->nsynth += len;
    return pos;
}

static GlslcToken synth_number(Glslc* g, uint32_t value, uint32_t site) {
    char text[16];
    int n = snprintf(text, sizeof text, "%u", value);
    GlslcToken t = {{0, 0}, 0, TK_INTCONST, GLSLC_TF_SYNTH};
    t.text.pos = (uint16_t)synth_add(g, text, (uint32_t)n, site);
    t.text.len = (uint16_t)n;
    t.site = (uint16_t)site;
    return t;
}

static const char* macro_name(const Glslc* g, const GlslcMacro* m) {
    return (m->flags & GLSLC_MF_SYNTH) ? g->synth + m->name.pos : g->src + m->name.pos;
}

static uint32_t find_macro(const Glslc* g, const char* text, uint32_t len) {
    for (uint32_t m = g->macro_hash[glslc_hash(text, len) & 255u]; m; m = g->macros[m].next) {
        const GlslcMacro* mc = &g->macros[m];
        if (!(mc->flags & GLSLC_MF_DEAD) && mc->name.len == len && !memcmp(macro_name(g, mc), text, len)) return m;
    }
    return 0;
}

static uint32_t new_macro(Glslc* g, GlslcView name, uint8_t flags, uint32_t site) {
    if (g->nmacros >= SNX_GLSLC_MAX_MACROS)
        glslc_fail(g, site, "the shader defines more than the %u macros glslc takes (the predefined ones count)",
                   SNX_GLSLC_MAX_MACROS - 1);
    uint32_t m = g->nmacros++;
    GlslcMacro* mc = &g->macros[m];
    mc->name = name;
    mc->flags = flags;
    mc->first = (uint16_t)g->nmtoks;
    mc->count = 0;
    mc->params = 0;
    uint32_t bucket = glslc_hash(macro_name(g, mc), name.len) & 255u;
    mc->next = g->macro_hash[bucket];
    g->macro_hash[bucket] = (uint16_t)m;
    return m;
}

static void mtok_push(Glslc* g, const GlslcToken* t) {
    if (g->nmtoks >= SNX_GLSLC_MAX_MACRO_TOKENS)
        glslc_fail(g, t->site, "the macros' parameters and bodies have more than the %u tokens glslc takes",
                   SNX_GLSLC_MAX_MACRO_TOKENS);
    g->mtoks[g->nmtoks++] = *t;
}

static void define_synth(Glslc* g, const char* name, const char* value, uint8_t flags) {
    GlslcView v;
    v.len = (uint16_t)strlen(name);
    v.pos = (uint16_t)synth_add(g, name, v.len, 0);
    uint32_t m = new_macro(g, v, (uint8_t)(flags | GLSLC_MF_SYNTH), 0);
    if (value) {
        GlslcToken t = {{0, 0}, 0, TK_INTCONST, GLSLC_TF_SYNTH};
        t.text.len = (uint16_t)strlen(value);
        t.text.pos = (uint16_t)synth_add(g, value, t.text.len, 0);
        mtok_push(g, &t);
        g->macros[m].count = 1;
    }
}

static void exp_push(Glslc* g, const GlslcToken* t) {
    if (g->nexp >= SNX_GLSLC_MAX_EXPANSION)
        glslc_fail(g, g->exp_site, "a macro use expands to more than the %u tokens glslc takes, with its arguments",
                   SNX_GLSLC_MAX_EXPANSION);
    g->exp[g->nexp++] = *t;
}

static int is_active(const Glslc* g, uint32_t m) {
    for (uint32_t i = 0; i < g->nactive; i++) {
        if (g->active[i] == m) return 1;
    }
    return 0;
}

static uint32_t rescan(Glslc* g, uint32_t lo, uint32_t hi, uint32_t site);

/* Expands macro m, used at exp[i] with its arguments (if it takes some) up to exp[*end); the result is left at the
   top of exp, from the index returned */
static uint32_t expand_one(Glslc* g, uint32_t m, uint32_t i, uint32_t args_end, uint32_t site) {
    const GlslcMacro* mc = &g->macros[m];
    if (mc->flags & GLSLC_MF_LINE) {
        uint32_t line, column, at_ = g->nexp;
        glslc_locate(g, site, &line, &column);
        GlslcToken t = synth_number(g, line, site);
        exp_push(g, &t);
        return at_;
    }
    if (mc->flags & GLSLC_MF_VERSION) {
        uint32_t at_ = g->nexp;
        GlslcToken t = synth_number(g, g->version, site);
        exp_push(g, &t);
        return at_;
    }
    uint32_t arg_lo[MAX_PARAMS], arg_hi[MAX_PARAMS], nargs = 0;
    if (mc->flags & GLSLC_MF_FUNCTION) {
        /* exp[i + 1] is '(' and exp[args_end - 1] its ')' */
        uint32_t depth = 0, start = i + 2;
        for (uint32_t k = i + 2; k < args_end; k++) {
            uint8_t kind = g->exp[k].kind;
            int last = k == args_end - 1;
            if (kind == TK_LPAREN) depth++;
            if (kind == TK_RPAREN && !last) depth--;
            if ((kind == TK_COMMA && depth == 0) || last) {
                if (nargs == MAX_PARAMS)
                    glslc_fail(g, g->exp[k].site, "a macro use has more than %u arguments", MAX_PARAMS);
                arg_lo[nargs] = start;
                arg_hi[nargs] = k;
                nargs++;
                start = k + 1;
            }
        }
        if (mc->params == 0 && nargs == 1 && arg_lo[0] == arg_hi[0]) nargs = 0;
        if (nargs != mc->params) {
            glslc_fail(g, g->exp[i].site, "the macro %.*s takes %u argument%s, and is given %u", (int)mc->name.len,
                       macro_name(g, mc), mc->params, mc->params == 1 ? "" : "s", nargs);
        }
    }
    /* each argument expanded, as C does before putting it in the body */
    uint32_t exp_lo[MAX_PARAMS], exp_hi[MAX_PARAMS];
    for (uint32_t a = 0; a < nargs; a++) {
        exp_lo[a] = rescan(g, arg_lo[a], arg_hi[a], site);
        exp_hi[a] = g->nexp;
    }
    uint32_t sub = g->nexp;
    for (uint32_t b = mc->params; b < mc->count; b++) {
        const GlslcToken* t = &g->mtoks[mc->first + b];
        uint32_t p = mc->params;
        if (t->kind == TK_IDENT) {
            for (p = 0; p < mc->params; p++) {
                const GlslcToken* name = &g->mtoks[mc->first + p];
                if (name->text.len == t->text.len && !memcmp(text_of(g, name), text_of(g, t), t->text.len)) break;
            }
        }
        if (p < mc->params) {
            for (uint32_t k = exp_lo[p]; k < exp_hi[p]; k++) {
                GlslcToken copy = g->exp[k];
                exp_push(g, &copy);
            }
        } else {
            exp_push(g, t);
        }
    }
    if (g->nactive >= SNX_GLSLC_MAX_MACRO_DEPTH)
        glslc_fail(g, site, "macros expand inside each other more than %u deep", SNX_GLSLC_MAX_MACRO_DEPTH);
    g->active[g->nactive++] = (uint16_t)m;
    uint32_t result = rescan(g, sub, g->nexp, site);
    g->nactive--;
    return result;
}

static uint32_t rescan(Glslc* g, uint32_t lo, uint32_t hi, uint32_t site) {
    uint32_t out = g->nexp, w = out;
    for (uint32_t i = lo; i < hi;) {
        const GlslcToken* t = &g->exp[i];
        uint32_t m = t->kind == TK_IDENT ? find_macro(g, text_of(g, t), t->text.len) : 0;
        if (!m || is_active(g, m)) {
            GlslcToken copy = *t;
            g->nexp = w;
            exp_push(g, &copy);
            w++;
            i++;
            continue;
        }
        uint32_t end = i + 1;
        if (g->macros[m].flags & GLSLC_MF_FUNCTION) {
            if (i + 1 >= hi || g->exp[i + 1].kind != TK_LPAREN) {
                GlslcToken copy = *t;
                g->nexp = w;
                exp_push(g, &copy);
                w++;
                i++;
                continue;
            }
            uint32_t depth = 0;
            for (end = i + 1; end < hi; end++) {
                if (g->exp[end].kind == TK_LPAREN) depth++;
                if (g->exp[end].kind == TK_RPAREN && --depth == 0) break;
            }
            if (end >= hi)
                glslc_fail(g, t->site, "the macro use %.*s( has no ) in what it can take arguments from",
                           (int)t->text.len, text_of(g, t));
            end++;
        }
        g->nexp = w;
        uint32_t result = expand_one(g, m, i, end, site);
        uint32_t n = g->nexp - result;
        memmove(&g->exp[w], &g->exp[result], n * sizeof(GlslcToken));
        w += n;
        g->nexp = w;
        i = end;
    }
    g->nexp = w;
    return out;
}

static void put(Glslc* g, GlslcToken t) {
    if (t.kind == TK_IDENT) {
        const GlslcKeyword* k = glslc_find_keyword(text_of(g, &t), t.text.len);
        if (k) {
            uint32_t vm = 1u << g->vi;
            if ((k->kw & vm) || (k->ext && g->ext[k->ext] != GLSLC_BEHAVIOR_DISABLE))
                t.kind = k->kind;
            else if (k->res & vm)
                t.kind = TK_RESERVED;
        }
    } else if (t.kind == TK_HASH || t.kind == TK_HASHHASH) {
        glslc_fail(g, t.site, "'%.*s' is only a directive at the start of a line", (int)t.text.len, text_of(g, &t));
    }
    t.flags &= (uint8_t)~GLSLC_TF_BOL;
    if (g->ntoks < SNX_GLSLC_MAX_TOKENS) {
        g->toks[g->ntoks] = t;
    } else if (g->ntoks == SNX_GLSLC_MAX_TOKENS) {
        g->overflow_site = t.site;
    }
    g->ntoks++;
}

static void settle_version(Glslc* g, uint32_t site) {
    if (g->seen_any) return;
    g->seen_any = 1;
    if (g->stage == SNX_GLSLC_COMPUTE && g->vi < GLSLC_V310) {
        glslc_fail(g, site, "a compute shader needs #version 310 es or later, and this one is GLSL ES %u%s", g->version,
                   g->vi == GLSLC_V100 ? "" : " es");
    }
}

/* the source's macro use at use, with its arguments read from the source, expanded into the tokens */
static void expand_source(Glslc* g, const GlslcToken* use, uint32_t m) {
    g->nexp = 0;
    g->exp_site = use->site;
    exp_push(g, use);
    if (g->macros[m].flags & GLSLC_MF_FUNCTION) {
        uint32_t cur = g->cur;
        uint8_t bol = g->bol;
        GlslcToken t;
        scan(g, &t, 0);
        if (t.kind != TK_LPAREN) {
            g->cur = cur;
            g->bol = bol;
            put(g, *use);
            return;
        }
        exp_push(g, &t);
        for (uint32_t depth = 1; depth;) {
            scan(g, &t, 0);
            if (t.kind == TK_EOF)
                glslc_fail(g, use->site, "the macro use %.*s( has no ) before the end of the source",
                           (int)use->text.len, text_of(g, use));
            if (t.kind == TK_HASH && (t.flags & GLSLC_TF_BOL))
                glslc_fail(g, t.site, "a directive inside the arguments of macro %.*s", (int)use->text.len,
                           text_of(g, use));
            if (t.kind == TK_LPAREN) depth++;
            if (t.kind == TK_RPAREN) depth--;
            exp_push(g, &t);
        }
    }
    uint32_t result = rescan(g, 0, g->nexp, use->site);
    for (uint32_t i = result; i < g->nexp; i++) {
        GlslcToken t = g->exp[i];
        t.site = use->site;
        put(g, t);
    }
    g->nexp = 0;
}

/* #if's expression */
typedef struct {
    uint32_t i, end;
} PpCursor;

static int64_t pp_binary(Glslc* g, PpCursor* c, int level, int live);

static const GlslcToken* pp_peek(const Glslc* g, const PpCursor* c) { return c->i < c->end ? &g->exp[c->i] : NULL; }

static int64_t pp_unary(Glslc* g, PpCursor* c, int live, uint32_t site) {
    const GlslcToken* t = pp_peek(g, c);
    if (!t) glslc_fail(g, site, "this #if expression ends where it needs a value");
    if (++g->depth > SNX_GLSLC_MAX_DEPTH)
        glslc_fail(g, t->site, "this #if expression nests more than %u deep", SNX_GLSLC_MAX_DEPTH);
    c->i++;
    int64_t v = 0;
    switch (t->kind) {
        case TK_PLUS: v = pp_unary(g, c, live, site); break;
        case TK_MINUS: v = -pp_unary(g, c, live, site); break;
        case TK_TILDE: v = ~pp_unary(g, c, live, site); break;
        case TK_BANG: v = !pp_unary(g, c, live, site); break;
        case TK_LPAREN: {
            v = pp_binary(g, c, 0, live);
            const GlslcToken* r = pp_peek(g, c);
            if (!r || r->kind != TK_RPAREN) glslc_fail(g, t->site, "this ( of the #if expression has no )");
            c->i++;
            break;
        }
        case TK_INTCONST:
        case TK_UINTCONST: {
            uint32_t u;
            if (!glslc_parse_int(text_of(g, t), t->text.len, &u))
                glslc_fail(g, t->site, "'%.*s' does not fit in 32 bits", (int)t->text.len, text_of(g, t));
            v = u;
            break;
        }
        case TK_IDENT:
            if (live) {
                glslc_fail(g, t->site,
                           "'%.*s' is not a defined macro: a #if in GLSL ES takes defined macros and numbers only",
                           (int)t->text.len, text_of(g, t));
            }
            break;
        default: glslc_fail(g, t->site, "'%.*s' cannot be in a #if expression", (int)t->text.len, text_of(g, t));
    }
    g->depth--;
    return v;
}

static int pp_level(uint8_t kind) {
    switch (kind) {
        case TK_OROR: return 0;
        case TK_ANDAND: return 1;
        case TK_BAR: return 2;
        case TK_CARET: return 3;
        case TK_AMP: return 4;
        case TK_EQ:
        case TK_NE: return 5;
        case TK_LT:
        case TK_GT:
        case TK_LE:
        case TK_GE: return 6;
        case TK_SHL:
        case TK_SHR: return 7;
        case TK_PLUS:
        case TK_MINUS: return 8;
        case TK_STAR:
        case TK_SLASH:
        case TK_PERCENT: return 9;
        default: return -1;
    }
}

static int64_t pp_binary(Glslc* g, PpCursor* c, int level, int live) {
    uint32_t site = c->i < c->end ? g->exp[c->i].site : (c->end ? g->exp[c->end - 1].site : 0);
    if (level > 9) return pp_unary(g, c, live, site);
    int64_t l = pp_binary(g, c, level + 1, live);
    for (;;) {
        const GlslcToken* t = pp_peek(g, c);
        if (!t || pp_level(t->kind) != level) return l;
        c->i++;
        int right_live = live;
        if (t->kind == TK_OROR) right_live = live && !l;
        if (t->kind == TK_ANDAND) right_live = live && l;
        int64_t r = pp_binary(g, c, level + 1, right_live);
        switch (t->kind) {
            case TK_OROR: l = l || r; break;
            case TK_ANDAND: l = l && r; break;
            case TK_BAR: l |= r; break;
            case TK_CARET: l ^= r; break;
            case TK_AMP: l &= r; break;
            case TK_EQ: l = l == r; break;
            case TK_NE: l = l != r; break;
            case TK_LT: l = l < r; break;
            case TK_GT: l = l > r; break;
            case TK_LE: l = l <= r; break;
            case TK_GE: l = l >= r; break;
            case TK_SHL:
            case TK_SHR:
                if (r < 0 || r > 31) {
                    if (live) glslc_fail(g, t->site, "a #if shift by %lld: it takes 0 to 31", (long long)r);
                    l = 0;
                } else {
                    l = t->kind == TK_SHL ? (int64_t)((uint64_t)l << r) : l >> r;
                }
                break;
            case TK_PLUS: l += r; break;
            case TK_MINUS: l -= r; break;
            case TK_STAR: l *= r; break;
            default:
                if (r == 0) {
                    if (live) glslc_fail(g, t->site, "a #if expression divides by zero");
                    l = 0;
                } else {
                    l = t->kind == TK_SLASH ? l / r : l % r;
                }
        }
        /* the arithmetic of a 32-bit int, as the compiler's own constants */
        l = (int64_t)(int32_t)(uint32_t)(uint64_t)l;
    }
}

static int if_expression(Glslc* g, const GlslcToken* directive) {
    g->nexp = 0;
    g->exp_site = directive->site;
    GlslcToken t;
    while (line_next(g, &t)) {
        if (t.kind == TK_IDENT && token_is(g, &t, "defined")) {
            GlslcToken n, r;
            int paren = 0;
            if (!line_next(g, &n)) glslc_fail(g, t.site, "'defined' needs a macro name after it");
            if (n.kind == TK_LPAREN) {
                paren = 1;
                if (!line_next(g, &n)) glslc_fail(g, t.site, "'defined(' needs a macro name after it");
            }
            if (n.kind != TK_IDENT)
                glslc_fail(g, n.site, "'defined' takes a macro name, not '%.*s'", (int)n.text.len, text_of(g, &n));
            if (paren && (!line_next(g, &r) || r.kind != TK_RPAREN))
                glslc_fail(g, t.site, "'defined(%.*s' has no )", (int)n.text.len, text_of(g, &n));
            GlslcToken v = {{0, 1}, 0, TK_INTCONST, GLSLC_TF_SYNTH};
            v.text.pos = find_macro(g, text_of(g, &n), n.text.len) ? 1 : 0; /* g->synth starts with "01" */
            v.site = t.site;
            exp_push(g, &v);
            continue;
        }
        exp_push(g, &t);
    }
    if (g->nexp == 0)
        glslc_fail(g, directive->site, "#%.*s needs an expression", (int)directive->text.len, text_of(g, directive));
    PpCursor c;
    c.i = rescan(g, 0, g->nexp, directive->site);
    c.end = g->nexp;
    if (c.i == c.end) glslc_fail(g, directive->site, "this #if expression is empty once its macros are expanded");
    int64_t v = pp_binary(g, &c, 0, 1);
    if (c.i != c.end)
        glslc_fail(g, g->exp[c.i].site, "'%.*s' is left over at the end of the #if expression",
                   (int)g->exp[c.i].text.len, text_of(g, &g->exp[c.i]));
    g->nexp = 0;
    return v != 0;
}

static void push_if(Glslc* g, const GlslcToken* hash, uint32_t flags) {
    if (g->nifs >= SNX_GLSLC_MAX_IF_DEPTH)
        glslc_fail(g, hash->site, "#if, #ifdef and #ifndef nest more than %u deep", SNX_GLSLC_MAX_IF_DEPTH);
    g->ifs[g->nifs++] = flags | ((uint32_t)hash->site << 8);
}

static void no_more(Glslc* g, const GlslcToken* name) {
    GlslcToken t;
    if (line_next(g, &t))
        glslc_fail(g, t.site, "'%.*s' is left over at the end of the #%.*s line", (int)t.text.len, text_of(g, &t),
                   (int)name->text.len, text_of(g, name));
}

static int has_double_underscore(const char* text, uint32_t len) {
    for (uint32_t i = 0; i + 1 < len; i++) {
        if (text[i] == '_' && text[i + 1] == '_') return 1;
    }
    return 0;
}

static void directive_define(Glslc* g, const GlslcToken* hash) {
    GlslcToken name, t;
    if (!line_next(g, &name) || name.kind != TK_IDENT) glslc_fail(g, hash->site, "#define needs a macro name");
    const char* text = text_of(g, &name);
    if (name.text.len >= 3 && !memcmp(text, "GL_", 3))
        glslc_fail(g, name.site, "'%.*s': macro names starting with GL_ are reserved", (int)name.text.len, text);
    if (token_is(g, &name, "defined")) glslc_fail(g, name.site, "'defined' cannot be a macro's name");
    uint32_t old = find_macro(g, text, name.text.len);
    if (old && (g->macros[old].flags & GLSLC_MF_SYNTH))
        glslc_fail(g, name.site, "'%.*s' is a predefined macro, which cannot be defined again", (int)name.text.len,
                   text);
    if (has_double_underscore(text, name.text.len)) { /* reserved: an error in 1.00, a warning from 3.00 */
        if (g->vi == GLSLC_V100)
            glslc_fail(g, name.site, "'%.*s': GLSL ES 1.00 reserves names with two underscores in a row",
                       (int)name.text.len, text);
        glslc_warn(g, name.site, "'%.*s': names with two underscores in a row are reserved", (int)name.text.len, text);
    }
    uint32_t first = g->nmtoks, params = 0;
    uint8_t flags = 0;
    int have = line_next(g, &t);
    if (have && t.kind == TK_LPAREN && t.text.pos == name.text.pos + name.text.len) {
        flags |= GLSLC_MF_FUNCTION;
        if (!line_next(g, &t))
            glslc_fail(g, name.site, "the parameters of macro %.*s have no )", (int)name.text.len, text);
        while (t.kind != TK_RPAREN) {
            if (t.kind != TK_IDENT)
                glslc_fail(g, t.site, "'%.*s' is not a macro parameter's name", (int)t.text.len, text_of(g, &t));
            for (uint32_t p = 0; p < params; p++) {
                const GlslcToken* q = &g->mtoks[first + p];
                if (q->text.len == t.text.len && !memcmp(text_of(g, q), text_of(g, &t), t.text.len))
                    glslc_fail(g, t.site, "macro %.*s has two parameters named %.*s", (int)name.text.len, text,
                               (int)t.text.len, text_of(g, &t));
            }
            if (params == MAX_PARAMS)
                glslc_fail(g, t.site, "macro %.*s has more than the %u parameters glslc takes", (int)name.text.len,
                           text, MAX_PARAMS);
            mtok_push(g, &t);
            params++;
            if (!line_next(g, &t))
                glslc_fail(g, name.site, "the parameters of macro %.*s have no )", (int)name.text.len, text);
            if (t.kind == TK_RPAREN) break;
            if (t.kind != TK_COMMA || !line_next(g, &t))
                glslc_fail(g, t.site, "the parameters of macro %.*s take ',' or ')' here, not '%.*s'",
                           (int)name.text.len, text, (int)t.text.len, text_of(g, &t));
        }
        have = line_next(g, &t);
    }
    while (have) {
        if (t.kind == TK_HASHHASH) glslc_fail(g, t.site, "token pasting (##) is not in GLSL ES");
        if (t.kind == TK_HASH) glslc_fail(g, t.site, "the # operator (stringizing) is not in GLSL ES");
        mtok_push(g, &t);
        have = line_next(g, &t);
    }
    uint32_t count = g->nmtoks - first;
    if (old) {
        const GlslcMacro* m = &g->macros[old];
        int same = m->count == count && m->params == params && (m->flags & GLSLC_MF_FUNCTION) == flags;
        for (uint32_t k = 0; same && k < count; k++) {
            const GlslcToken* a = &g->mtoks[m->first + k];
            const GlslcToken* b = &g->mtoks[first + k];
            same =
                a->kind == b->kind && a->text.len == b->text.len && !memcmp(text_of(g, a), text_of(g, b), a->text.len);
        }
        if (!same)
            glslc_fail(g, name.site, "macro %.*s is defined again with another body: #undef it first",
                       (int)name.text.len, text);
        g->nmtoks = first; /* the same definition again changes nothing */
        return;
    }
    uint32_t m = new_macro(g, name.text, flags, name.site);
    g->macros[m].first = (uint16_t)first;
    g->macros[m].count = (uint16_t)count;
    g->macros[m].params = (uint8_t)params;
}

static void directive_undef(Glslc* g, const GlslcToken* hash, const GlslcToken* directive) {
    GlslcToken name;
    if (!line_next(g, &name) || name.kind != TK_IDENT) glslc_fail(g, hash->site, "#undef needs a macro name");
    no_more(g, directive);
    const char* text = text_of(g, &name);
    if (name.text.len >= 3 && !memcmp(text, "GL_", 3))
        glslc_fail(g, name.site, "'%.*s': macro names starting with GL_ are reserved", (int)name.text.len, text);
    uint32_t m = find_macro(g, text, name.text.len);
    if (m && (g->macros[m].flags & GLSLC_MF_SYNTH))
        glslc_fail(g, name.site, "'%.*s' is a predefined macro, which cannot be undefined", (int)name.text.len, text);
    if (m) g->macros[m].flags |= GLSLC_MF_DEAD;
}

static void directive_version(Glslc* g, const GlslcToken* hash) {
    GlslcToken n, p;
    if (g->seen_any) glslc_fail(g, hash->site, "#version must come before anything but comments and white space");
    if (!line_next(g, &n) || n.kind != TK_INTCONST)
        glslc_fail(g, hash->site, "#version needs a number: 100, 300 es, 310 es or 320 es");
    uint32_t v = 0;
    if (!glslc_parse_int(text_of(g, &n), n.text.len, &v)) v = 0xffffffffu;
    int es = line_next(g, &p);
    if (es && !(p.kind == TK_IDENT && token_is(g, &p, "es")))
        glslc_fail(g, p.site, "'#version %u %.*s': GLSL ES's profile is es", v, (int)p.text.len, text_of(g, &p));
    if (es) {
        GlslcToken x;
        if (line_next(g, &x))
            glslc_fail(g, x.site, "'%.*s' is left over at the end of the #version line", (int)x.text.len,
                       text_of(g, &x));
    }
    switch (v) {
        case 100:
            if (es) glslc_fail(g, p.site, "#version 100 takes no profile: es is for 300 and later");
            g->vi = GLSLC_V100;
            break;
        case 300:
        case 310:
        case 320:
            if (!es)
                glslc_fail(g, n.site,
                           "#version %u without es is desktop GLSL, which glslc does not compile: GLSL ES is "
                           "'#version %u es'",
                           v, v);
            g->vi = v == 300 ? GLSLC_V300 : v == 310 ? GLSLC_V310 : GLSLC_V320;
            break;
        default:
            glslc_fail(g, n.site,
                       "#version %.*s is not a GLSL ES version: glslc compiles 100, 300 es, 310 es and 320 es",
                       (int)n.text.len, text_of(g, &n));
    }
    g->version = (uint16_t)v;
    g->explicit_version = 1;
    settle_version(g, hash->site);
}

static void directive_extension(Glslc* g, const GlslcToken* hash, const GlslcToken* directive) {
    GlslcToken name, colon, behavior;
    if (!line_next(g, &name) || name.kind != TK_IDENT)
        glslc_fail(g, hash->site, "#extension needs a name: #extension name : behavior");
    if (!line_next(g, &colon) || colon.kind != TK_COLON || !line_next(g, &behavior) || behavior.kind != TK_IDENT)
        glslc_fail(g, name.site, "#extension %.*s needs ': behavior' after it (require, enable, warn or disable)",
                   (int)name.text.len, text_of(g, &name));
    no_more(g, directive);
    uint8_t b;
    if (token_is(g, &behavior, "require")) {
        b = GLSLC_BEHAVIOR_REQUIRE;
    } else if (token_is(g, &behavior, "enable")) {
        b = GLSLC_BEHAVIOR_ENABLE;
    } else if (token_is(g, &behavior, "warn")) {
        b = GLSLC_BEHAVIOR_WARN;
    } else if (token_is(g, &behavior, "disable")) {
        b = GLSLC_BEHAVIOR_DISABLE;
    } else {
        glslc_fail(g, behavior.site, "'%.*s' is not an #extension behavior: require, enable, warn or disable",
                   (int)behavior.text.len, text_of(g, &behavior));
        return;
    }
    if (token_is(g, &name, "all")) {
        if (b == GLSLC_BEHAVIOR_REQUIRE || b == GLSLC_BEHAVIOR_ENABLE)
            glslc_fail(g, behavior.site, "#extension all takes warn or disable, not %.*s", (int)behavior.text.len,
                       text_of(g, &behavior));
        for (uint32_t e = 1; e < GLSLC_EXT_COUNT; e++) g->ext[e] = b;
        return;
    }
    for (uint32_t e = 0; e < glslc_extension_count; e++) {
        if (token_is(g, &name, glslc_extensions[e].name) && (glslc_extensions[e].vmask & (1u << g->vi))) {
            g->ext[glslc_extensions[e].feature] = b;
            return;
        }
    }
    if (b == GLSLC_BEHAVIOR_REQUIRE)
        glslc_fail(g, name.site, "the extension %.*s is not one glslc supports", (int)name.text.len, text_of(g, &name));
    if (b != GLSLC_BEHAVIOR_DISABLE)
        glslc_warn(g, name.site, "the extension %.*s is not one glslc supports: nothing it adds is compiled",
                   (int)name.text.len, text_of(g, &name));
}

static void directive_line(Glslc* g, const GlslcToken* hash, const GlslcToken* directive) {
    GlslcToken n, s;
    uint32_t line = 0, source = 0;
    if (!line_next(g, &n) || n.kind != TK_INTCONST || !glslc_parse_int(text_of(g, &n), n.text.len, &line))
        glslc_fail(g, hash->site, "#line needs a line number");
    if (line_next(g, &s) && (s.kind != TK_INTCONST || !glslc_parse_int(text_of(g, &s), s.text.len, &source)))
        glslc_fail(g, s.site, "#line takes a source string number after the line, not '%.*s'", (int)s.text.len,
                   text_of(g, &s));
    no_more(g, directive);
    if (g->nmarks >= SNX_GLSLC_MAX_LINE_MARKS)
        glslc_fail(g, hash->site, "the shader has more than the %u #line directives glslc takes",
                   SNX_GLSLC_MAX_LINE_MARKS);
    uint32_t pos = g->cur;
    while (pos < g->src_len && g->src[pos] != '\n' && g->src[pos] != '\r') pos++;
    if (pos < g->src_len && g->src[pos] == '\r') pos++;
    if (pos < g->src_len && g->src[pos] == '\n') pos++;
    g->marks[g->nmarks].pos = (uint16_t)pos;
    g->marks[g->nmarks].line = line;
    g->nmarks++;
}

static void directive_pragma(Glslc* g) {
    GlslcToken a, b, c, d, e;
    if (line_next(g, &a) && token_is(g, &a, "STDGL") && line_next(g, &b) && token_is(g, &b, "invariant") &&
        line_next(g, &c) && c.kind == TK_LPAREN && line_next(g, &d) && token_is(g, &d, "all") && line_next(g, &e) &&
        e.kind == TK_RPAREN) {
        g->invariant_all = 1;
    }
    skip_line(g); /* the other pragmas change nothing glslc writes, as GLSL ES asks of unknown ones */
}

static void directive(Glslc* g, const GlslcToken* hash) {
    GlslcToken name;
    if (!line_next(g, &name)) {
        if (!skipping(g)) settle_version(g, hash->site);
        return; /* # alone on its line */
    }
    int dead = skipping(g);
    if (name.kind == TK_IDENT &&
        (token_is(g, &name, "if") || token_is(g, &name, "ifdef") || token_is(g, &name, "ifndef"))) {
        if (dead) {
            skip_line(g);
            push_if(g, hash, IF_DEAD);
            return;
        }
        settle_version(g, hash->site);
        int v;
        if (token_is(g, &name, "if")) {
            v = if_expression(g, &name);
        } else {
            GlslcToken m;
            if (!line_next(g, &m) || m.kind != TK_IDENT)
                glslc_fail(g, hash->site, "#%.*s needs a macro name", (int)name.text.len, text_of(g, &name));
            no_more(g, &name);
            v = find_macro(g, text_of(g, &m), m.text.len) != 0;
            if (token_is(g, &name, "ifndef")) v = !v;
        }
        push_if(g, hash, v ? IF_LIVE | IF_TAKEN : 0);
        return;
    }
    if (name.kind == TK_IDENT && (token_is(g, &name, "elif") || token_is(g, &name, "else"))) {
        int is_else = token_is(g, &name, "else");
        if (!g->nifs) glslc_fail(g, hash->site, "#%.*s without #if", (int)name.text.len, text_of(g, &name));
        uint32_t* top = &g->ifs[g->nifs - 1];
        if (*top & IF_ELSE) glslc_fail(g, hash->site, "#%.*s after #else", (int)name.text.len, text_of(g, &name));
        if ((*top & IF_DEAD) || (*top & IF_TAKEN)) {
            skip_line(g);
            *top &= ~IF_LIVE;
        } else if (is_else) {
            no_more(g, &name);
            *top |= IF_LIVE | IF_TAKEN;
        } else if (if_expression(g, &name)) {
            *top |= IF_LIVE | IF_TAKEN;
        }
        if (is_else) *top |= IF_ELSE;
        return;
    }
    if (name.kind == TK_IDENT && token_is(g, &name, "endif")) {
        if (!g->nifs) glslc_fail(g, hash->site, "#endif without #if");
        if (!dead) no_more(g, &name);
        skip_line(g);
        g->nifs--;
        return;
    }
    if (dead) {
        skip_line(g);
        return;
    }
    if (name.kind == TK_IDENT && token_is(g, &name, "version")) {
        directive_version(g, hash);
        return;
    }
    settle_version(g, hash->site);
    if (name.kind != TK_IDENT) {
        glslc_fail(g, name.site, "'#%.*s' is not a directive", (int)name.text.len, text_of(g, &name));
    } else if (token_is(g, &name, "define")) {
        directive_define(g, hash);
    } else if (token_is(g, &name, "undef")) {
        directive_undef(g, hash, &name);
    } else if (token_is(g, &name, "extension")) {
        directive_extension(g, hash, &name);
    } else if (token_is(g, &name, "line")) {
        directive_line(g, hash, &name);
    } else if (token_is(g, &name, "pragma")) {
        directive_pragma(g);
    } else if (token_is(g, &name, "error")) {
        uint32_t start = name.text.pos + name.text.len, end = start;
        while (end < g->src_len && g->src[end] != '\n' && g->src[end] != '\r') end++;
        while (start < end && (g->src[start] == ' ' || g->src[start] == '\t')) start++;
        while (end > start && (g->src[end - 1] == ' ' || g->src[end - 1] == '\t' || g->src[end - 1] == '\r')) end--;
        glslc_fail(g, hash->site, "#error %.*s", (int)(end - start), g->src + start);
    } else {
        glslc_fail(g, name.site, "'#%.*s' is not a directive", (int)name.text.len, text_of(g, &name));
    }
}

void glslc_lex(Glslc* g) {
    synth_add(g, "01", 2, 0); /* defined's values */
    define_synth(g, "GL_ES", "1", 0);
    define_synth(g, "__VERSION__", NULL, GLSLC_MF_VERSION);
    define_synth(g, "__LINE__", NULL, GLSLC_MF_LINE);
    define_synth(g, "__FILE__", "0", 0);
    define_synth(g, "GL_FRAGMENT_PRECISION_HIGH", "1", 0);
    for (uint32_t e = 0; e < glslc_extension_count; e++) define_synth(g, glslc_extensions[e].name, "1", 0);
    for (;;) {
        GlslcToken t;
        scan(g, &t, 0);
        if (t.kind == TK_EOF) break;
        if (t.kind == TK_HASH && (t.flags & GLSLC_TF_BOL)) {
            directive(g, &t);
            continue;
        }
        if (skipping(g)) continue;
        settle_version(g, t.site);
        g->seen_code = 1;
        if (t.kind == TK_IDENT) {
            uint32_t m = find_macro(g, text_of(g, &t), t.text.len);
            if (m) {
                expand_source(g, &t, m);
                continue;
            }
        }
        put(g, t);
    }
    if (g->nifs) {
        uint32_t site = g->ifs[g->nifs - 1] >> 8;
        glslc_fail(g, site, "this #if has no #endif before the end of the source");
    }
    settle_version(g, g->src_len);
    GlslcToken end = {{0, 0}, 0, TK_EOF, 0};
    end.text.pos = (uint16_t)g->src_len;
    end.site = (uint16_t)g->src_len;
    put(g, end);
    if (g->ntoks > SNX_GLSLC_MAX_TOKENS)
        glslc_fail(g, g->overflow_site, "the shader has %u tokens after preprocessing, more than the %u glslc takes",
                   g->ntoks, SNX_GLSLC_MAX_TOKENS);
}
