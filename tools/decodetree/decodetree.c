/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU's scripts/decodetree.py, LGPL-2.1-or-later:
   Copyright (c) 2018 Linaro Limited */
/* decodetree: generates a decoder from QEMU's .decode files (docs/devel/decodetree.rst in QEMU), as QEMU's
   scripts/decodetree.py does, in C, so the build needs no Python.

   The language: %fields (pos:len, pos:slen, name:len for an argument already named, and !function=), &argument sets
   (name or name:type members, !extern for a set another file defines), @formats, patterns, { } groups whose patterns
   overlap and are tried in order, and [ ] groups whose patterns must not overlap. The output is the same as
   decodetree.py's:
   - a typedef arg_SET for each argument set that is not !extern
   - for each pattern name, `typedef arg_SET arg_NAME;` and `bool trans_NAME(DisasContext *ctx, arg_NAME *a);`,
     static unless --translate names another prefix
   - a static `decode_extract_FORMAT(DisasContext *ctx, arg_SET *a, insn)` for each format
   - `bool decode(DisasContext *ctx, insn)`, a switch tree on the fixed bits that calls the trans_ function of the
     pattern that matches. A trans_ function that answers false lets the decode try the next pattern of a { } group.
   The file that includes it defines DisasContext, the trans_ functions, the !function= functions
   (`int f(DisasContext *ctx, int x)`, or `int f(DisasContext *ctx)` for a field of no bits), and extract32,
   sextract32 and deposit32 (QEMU's bitops.h).

   Options, as decodetree.py's: --decode NAME (an extern decode function), --static-decode NAME, --translate PREFIX
   (extern trans functions with that prefix), -w/--insnwidth 16|32|64, -o/--output FILE. Not supported:
   --varinsnwidth. The output is only rewritten when it changes. decodetree.py emits a field's assignments in an
   order that changes from run to run where no field refers to another; this one keeps the order the fields are
   written in, which is also valid.

   It is strict C99 and depends on nothing, so a cross build can build it for the machine it runs on. */
#include "decodetree.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ globals */
static int insnwidth = 32, bitop_width = 32;
static u64 insnmask = 0xffffffffull;
static const char* insntype = "uint32_t";
static const char* translate_prefix = "trans";
static const char* translate_scope = "static ";
static const char* decode_function = "decode";
static const char* decode_scope = "static ";
const char* input_file = "";
static int anyextern;

static void* xalloc(size_t n) {
    void* p = calloc(1, n ? n : 1);
    if (!p) {
        fprintf(stderr, "decodetree: out of memory\n");
        exit(1);
    }
    return p;
}

#define GROW(arr, n, cap)                                                                                              \
    do {                                                                                                               \
        if ((n) == (cap)) {                                                                                            \
            (cap) = (cap) ? (cap) * 2 : 16;                                                                            \
            (arr) = realloc((arr), sizeof *(arr) * (size_t)(cap));                                                     \
            if (!(arr)) {                                                                                              \
                fprintf(stderr, "decodetree: out of memory\n");                                                       \
                exit(1);                                                                                               \
            }                                                                                                          \
        }                                                                                                              \
    } while (0)

void error_with_file(const char* file, int lineno, const char* fmt, ...) {
    va_list ap;
    if (file && *file) fprintf(stderr, "%s:", file);
    if (lineno) fprintf(stderr, "%d:", lineno);
    if ((file && *file) || lineno) fputc(' ', stderr);
    fputs("error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}


/* ------------------------------------------------------------------ output */
static char* obuf;
static size_t olen, ocap;

static void out(const char* fmt, ...) {
    va_list ap;
    int n;
    for (;;) {
        size_t room = ocap - olen;
        va_start(ap, fmt);
        n = vsnprintf(obuf ? obuf + olen : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) error_with_file(NULL, 0, "cannot format the output");
        if ((size_t)n < room) break;
        ocap = (olen + (size_t)n + 1) * 2;
        obuf = (char*)realloc(obuf, ocap);
        if (!obuf) error_with_file(NULL, 0, "out of memory");
    }
    olen += (size_t)n;
}

static void indent(int i) {
    while (i-- > 0) out(" ");
}

/* written only when it changes, so what includes it recompiles only then (Ninja's restat) */
static void flush(const char* path) {
    FILE* f;
    if (!path) {
        fwrite(obuf, 1, olen, stdout);
        return;
    }
    f = fopen(path, "rb");
    if (f) {
        char* old = (char*)xalloc(olen + 1);
        size_t got = fread(old, 1, olen + 1, f);
        int same = got == olen && !memcmp(old, obuf, olen);
        free(old);
        fclose(f);
        if (same) return;
    }
    f = fopen(path, "wb");
    if (!f || fwrite(obuf, 1, olen, f) != olen || fclose(f)) error_with_file(NULL, 0, "cannot write %s", path);
}

static const char* whex(u64 val) {
    static char buf[4][32];
    static int k;
    k = (k + 1) & 3;
    sprintf(buf[k], "0x%0*llx", insnwidth / 4, val);
    return buf[k];
}

static const char* whexC(u64 val) {
    static char buf[4][40];
    static int k;
    k = (k + 1) & 3;
    sprintf(buf[k], "%s%s", whex(val), val >= 0x100000000ull ? "ull" : val >= 0x80000000ull ? "u" : "");
    return buf[k];
}

static const char* str_match_bits(u64 bits, u64 mask) {
    static char buf[4][160];
    static int k;
    char* r;
    u64 i = 1ull << (insnwidth - 1), space = 0x01010100ull;
    k = (k + 1) & 3;
    r = buf[k];
    while (i) {
        *r++ = (i & mask) ? ((i & bits) ? '1' : '0') : '.';
        if (i & space) *r++ = ' ';
        i >>= 1;
    }
    *r = 0;
    return buf[k];
}

static int ctz(u64 x) {
    int r = 0;
    while (!((x >> r) & 1)) r++;
    return r;
}

static int is_contiguous(u64 bits) {
    int shift;
    u64 v;
    if (!bits) return -1;
    shift = ctz(bits);
    v = (bits >> shift) + 1;
    return (v & (v - 1)) == 0 ? shift : -1;
}

/* ------------------------------------------------------------------ fields */


FNode* fnodes;
int nfnodes;
static int cfnodes;

static int new_fnode(int kind) {
    GROW(fnodes, nfnodes, cfnodes);
    memset(&fnodes[nfnodes], 0, sizeof fnodes[0]);
    fnodes[nfnodes].kind = kind;
    fnodes[nfnodes].base = -1;
    return nfnodes++;
}

/* an ordered map of names to field nodes */

int flds_find(const Flds* f, const char* name) {
    int i;
    for (i = 0; i < f->n; i++)
        if (!strcmp(f->name[i], name)) return i;
    return -1;
}

static void add_field(int lineno, Flds* f, const char* name, int node) {
    if (flds_find(f, name) >= 0) error(lineno, "duplicate field %s", name);
    if (f->n == MAX_FLDS) error(lineno, "too many fields");
    strcpy(f->name[f->n], name);
    f->node[f->n++] = node;
}

/* the %fields */
Flds* gfields;

/* Python's == between two field objects */
static int field_eq(int a, int b) {
    const FNode *x = &fnodes[a], *y = &fnodes[b];
    int i;
    switch (x->kind) {
        case F_FIELD: return x->sign == y->sign && x->mask == y->mask;
        case F_MULTI:
            if (y->kind != F_MULTI || x->nsubs != y->nsubs) return 0;
            for (i = 0; i < x->nsubs; i++)
                if (fnodes[x->subs[i]].kind != fnodes[y->subs[i]].kind || !field_eq(x->subs[i], y->subs[i])) return 0;
            return 1;
        case F_CONST: return a == b; /* no __eq__: identity */
        case F_FUNC: return (y->kind == F_FUNC || y->kind == F_PARAM) && !strcmp(x->func, y->func) &&
                            y->kind == F_FUNC && field_eq(x->base, y->base);
        case F_PARAM: return (y->kind == F_FUNC || y->kind == F_PARAM) && !strcmp(x->func, y->func);
        default: return y->kind == F_NAMED && !strcmp(x->name, y->name);
    }
}

static void referenced_fields(int n, char refs[][NAME_LEN], int* nrefs) {
    const FNode* f = &fnodes[n];
    int i;
    switch (f->kind) {
        case F_NAMED: strcpy(refs[(*nrefs)++], f->name); break;
        case F_MULTI:
            for (i = 0; i < f->nsubs; i++) referenced_fields(f->subs[i], refs, nrefs);
            break;
        case F_FUNC: referenced_fields(f->base, refs, nrefs); break;
        default: break;
    }
}

/* lvalue: "a->" or "u.f_ARG." */
static void str_extract(int n, const char* lv) {
    const FNode* f = &fnodes[n];
    int i, pos;
    switch (f->kind) {
        case F_FIELD: out("%sextract%d(insn, %d, %d)", f->sign ? "s" : "", bitop_width, f->pos, f->len); break;
        case F_MULTI: {
            /* deposit(..., ext) nested, the last sub innermost at bit 0 */
            int depth = 0;
            pos = 0;
            for (i = f->nsubs - 1; i >= 0; i--) {
                if (i) depth++;
                pos += fnodes[f->subs[i]].len;
            }
            /* build the string from the inside out: decodetree.py nests deposits around the earlier result */
            {
                char* save;
                size_t start = olen, inner_len;
                pos = 0;
                for (i = f->nsubs - 1; i >= 0; i--) {
                    if (pos == 0) {
                        str_extract(f->subs[i], lv);
                    } else {
                        size_t mid = olen;
                        inner_len = mid - start;
                        save = (char*)xalloc(inner_len + 1);
                        memcpy(save, obuf + start, inner_len);
                        olen = start;
                        out("deposit%d(", bitop_width);
                        out("%.*s", (int)inner_len, save);
                        out(", %d, %d, ", pos, bitop_width - pos);
                        str_extract(f->subs[i], lv);
                        out(")");
                        free(save);
                    }
                    pos += fnodes[f->subs[i]].len;
                }
            }
            (void)depth;
            break;
        }
        case F_CONST: out("%lld", f->value); break;
        case F_FUNC:
            out("%s(ctx, ", f->func);
            str_extract(f->base, lv);
            out(")");
            break;
        case F_PARAM: out("%s(ctx)", f->func); break;
        default: out("%sextract%d(%s%s, 0, %d)", f->sign ? "s" : "", bitop_width, lv, f->name, f->len); break;
    }
}

/* ------------------------------------------------------------------ argument sets */

Args* args;
int nargs;
static int cargs;

static int args_find(const char* name) {
    int i;
    for (i = 0; i < nargs; i++)
        if (!strcmp(args[i].name, name)) return i;
    return -1;
}

int args_has(const Args* a, const char* name) {
    int i;
    for (i = 0; i < a->nfields; i++)
        if (!strcmp(a->fields[i], name)) return 1;
    return 0;
}

/* ------------------------------------------------------------------ formats and patterns */

General* formats;
int nformats;
static int cformats;
General* pats;
int npats;
static int cpats;

static int formats_find(const char* name) {
    int i;
    for (i = 0; i < nformats; i++)
        if (!strcmp(formats[i].name, name)) return i;
    return -1;
}

static void dangling_references(General* g) {
    int i, j;
    if (g->dangling_done) return;
    g->dangling_done = 1;
    for (i = 0; i < g->fields.n; i++) {
        char refs[MAX_FLDS][NAME_LEN];
        int nrefs = 0;
        referenced_fields(g->fields.node[i], refs, &nrefs);
        for (j = 0; j < nrefs; j++)
            if (flds_find(&g->fields, refs[j]) < 0) strcpy(g->dangling[g->ndangling++], refs[j]);
    }
}

/* each field's assignment, a field after the ones it refers to */
static void output_fields(General* g, int ind, const char* lv) {
    int done[MAX_FLDS] = {0}, left = g->fields.n, i, j;
    while (left) {
        int progress = 0;
        for (i = 0; i < g->fields.n; i++) {
            char refs[MAX_FLDS][NAME_LEN];
            int nrefs = 0, ready = 1;
            if (done[i]) continue;
            referenced_fields(g->fields.node[i], refs, &nrefs);
            for (j = 0; j < nrefs && ready; j++) {
                int k = flds_find(&g->fields, refs[j]);
                if (k >= 0 && !done[k]) ready = 0;
            }
            if (!ready) continue;
            indent(ind);
            out("%s%s = ", lv, g->fields.name[i]);
            str_extract(g->fields.node[i], lv);
            out(";\n");
            done[i] = 1;
            left--;
            progress = 1;
        }
        if (!progress) error(g->lineno, "field definitions form a cycle");
    }
}

static void extract_name(int fmt, char* buf) { sprintf(buf, "%s_extract_%s", decode_function, formats[fmt].name); }

/* ------------------------------------------------------------------ the pattern tree */


Node* nodes;
int nnodes;
static int cnodes;

static int new_node(int kind, int lineno) {
    GROW(nodes, nnodes, cnodes);
    memset(&nodes[nnodes], 0, sizeof nodes[0]);
    nodes[nnodes].kind = kind;
    nodes[nnodes].file = input_file;
    nodes[nnodes].lineno = lineno;
    nodes[nnodes].base = -1;
    nodes[nnodes].tree = -1;
    nodes[nnodes].parent = -1;
    return nnodes++;
}

static void add_kid(int parent, int kid, u64 bits) {
    Node* p = &nodes[parent];
    if (p->nkids == p->ckids) {
        p->ckids = p->ckids ? p->ckids * 2 : 8;
        p->kids = (int*)realloc(p->kids, sizeof(int) * (size_t)p->ckids);
        p->bits = (u64*)realloc(p->bits, sizeof(u64) * (size_t)p->ckids);
        if (!p->kids || !p->bits) error_with_file(NULL, 0, "out of memory");
    }
    if (nodes[kid].parent < 0) nodes[kid].parent = parent; /* the group it was parsed in */
    p->kids[p->nkids] = kid;
    p->bits[p->nkids] = bits;
    p->nkids++;
}

static u64 node_fixedmask(int n) {
    return nodes[n].kind == N_PAT ? pats[nodes[n].pat].fixedmask : nodes[n].fixedmask;
}
static u64 node_fixedbits(int n) {
    return nodes[n].kind == N_PAT ? pats[nodes[n].pat].fixedbits : nodes[n].fixedbits;
}
static u64 node_undefmask(int n) {
    return nodes[n].kind == N_PAT ? pats[nodes[n].pat].undefmask : nodes[n].undefmask;
}
/* Python's .base for prop_format: a pattern's format, a group's None, a tree's own */
static int node_base(int n) {
    if (nodes[n].kind == N_PAT) return pats[nodes[n].pat].base;
    if (nodes[n].kind == N_TREE) return nodes[n].base;
    return -1;
}

static void node_str(int n, char* buf) {
    if (nodes[n].kind == N_PAT)
        sprintf(buf, "%s %s", pats[nodes[n].pat].name, str_match_bits(pats[nodes[n].pat].fixedbits, pats[nodes[n].pat].fixedmask));
    else
        sprintf(buf, "group %s", str_match_bits(nodes[n].fixedbits, nodes[n].fixedmask));
}

static void prop_masks(int n) {
    Node* g = &nodes[n];
    u64 fixedmask = insnmask, undefmask = insnmask, fixedbits = 0;
    int i, repeat = 1;
    if (g->kind == N_PAT) return;
    for (i = 0; i < g->nkids; i++) {
        prop_masks(g->kids[i]);
        fixedmask &= node_fixedmask(g->kids[i]);
        undefmask &= node_undefmask(g->kids[i]);
    }
    while (repeat && fixedmask) {
        int have = 0;
        repeat = 0;
        for (i = 0; i < g->nkids; i++) {
            u64 thisbits = node_fixedbits(g->kids[i]) & fixedmask;
            if (!have) {
                fixedbits = thisbits;
                have = 1;
            } else if (fixedbits != thisbits) {
                fixedmask &= ~(fixedbits ^ thisbits);
                repeat = 1;
                break;
            }
        }
        if (!repeat) break;
    }
    if (!fixedmask) fixedbits = 0;
    g->fixedbits = fixedbits;
    g->fixedmask = fixedmask;
    g->undefmask = undefmask;
}

static int exc_build_tree(const int* p, int np, u64 outerbits, u64 outermask) {
    u64 innermask = ~outermask & insnmask, fullmask;
    int i, j, t;
    u64* binbits;
    int **bins, *binn, nbins = 0;
    for (i = 0; i < np; i++) innermask &= node_fixedmask(p[i]);
    if (!innermask) {
        if (np == 1) {
            t = new_node(N_TREE, 0);
            nodes[t].fixedmask = outermask;
            nodes[t].thismask = 0;
            add_kid(t, p[0], 0);
            return t;
        }
        {
            char buf[256];
            fprintf(stderr, "%s:%d: error: overlapping patterns:", nodes[p[0]].file, nodes[p[0]].kind == N_PAT ? pats[nodes[p[0]].pat].lineno : nodes[p[0]].lineno);
            for (i = 0; i < np; i++) {
                node_str(p[i], buf);
                fprintf(stderr, "\n%s:%d: %s", nodes[p[i]].file,
                        nodes[p[i]].kind == N_PAT ? pats[nodes[p[i]].pat].lineno : nodes[p[i]].lineno, buf);
            }
            fputc('\n', stderr);
            exit(1);
        }
    }
    fullmask = outermask | innermask;
    binbits = (u64*)xalloc(sizeof(u64) * (size_t)np);
    bins = (int**)xalloc(sizeof(int*) * (size_t)np);
    binn = (int*)xalloc(sizeof(int) * (size_t)np);
    for (i = 0; i < np; i++) {
        u64 fb = node_fixedbits(p[i]) & innermask;
        for (j = 0; j < nbins && binbits[j] != fb; j++) continue;
        if (j == nbins) {
            binbits[j] = fb;
            bins[j] = (int*)xalloc(sizeof(int) * (size_t)np);
            nbins++;
        }
        bins[j][binn[j]++] = p[i];
    }
    t = new_node(N_TREE, 0);
    nodes[t].fixedmask = fullmask;
    nodes[t].thismask = innermask;
    for (j = 0; j < nbins; j++) {
        int s = bins[j][0];
        if (binn[j] > 1 || (node_fixedmask(s) & ~fullmask)) s = exc_build_tree(bins[j], binn[j], binbits[j] | outerbits, fullmask);
        add_kid(t, s, binbits[j]);
        free(bins[j]);
    }
    free(binbits);
    free(bins);
    free(binn);
    return t;
}

static void build_tree(int n) {
    Node* g = &nodes[n];
    int i;
    if (g->kind == N_PAT) return;
    if (g->kind == N_INC && !g->nkids) error_with_file(g->file, g->lineno, "empty pattern group");
    for (i = 0; i < nodes[n].nkids; i++) build_tree(nodes[n].kids[i]);
    if (nodes[n].kind == N_EXC) {
        int* copy = (int*)xalloc(sizeof(int) * (size_t)nodes[n].nkids);
        int t;
        memcpy(copy, nodes[n].kids, sizeof(int) * (size_t)nodes[n].nkids);
        t = exc_build_tree(copy, nodes[n].nkids, nodes[n].fixedbits, nodes[n].fixedmask);
        nodes[n].tree = t;
        free(copy);
    }
}

static void tree_prop_format(int t) {
    Node* tr = &nodes[t];
    int i, f = -1;
    for (i = 0; i < tr->nkids; i++)
        if (nodes[tr->kids[i]].kind == N_TREE) tree_prop_format(tr->kids[i]);
    for (i = 0; i < nodes[t].nkids; i++) {
        int b = node_base(nodes[t].kids[i]);
        if (i == 0) {
            f = b;
            if (f < 0) return;
        }
        if (f != b) return;
    }
    nodes[t].base = f;
}

static void prop_format(int n) {
    int i;
    if (nodes[n].kind == N_PAT) return;
    for (i = 0; i < nodes[n].nkids; i++) prop_format(nodes[n].kids[i]);
    if (nodes[n].kind == N_EXC) tree_prop_format(nodes[n].tree);
}

/* ------------------------------------------------------------------ code */
static void output_code(int n, int i, int extracted, u64 outerbits, u64 outermask);

static void pattern_output_code(int pi, int i, int extracted) {
    General* p = &pats[pi];
    General* fmt = &formats[p->base];
    const char* arg = args[fmt->base].name;
    char lv[NAME_LEN + 8], ex[NAME_LEN * 2];
    int k;
    indent(i);
    out("/* %s:%d */\n", p->file, p->lineno);
    dangling_references(fmt);
    dangling_references(p);
    for (k = 0; k < fmt->ndangling; k++)
        if (flds_find(&p->fields, fmt->dangling[k]) < 0) error(p->lineno, "format refers to undefined field %s", fmt->dangling[k]);
    for (k = 0; k < p->ndangling; k++)
        if (flds_find(&fmt->fields, p->dangling[k]) < 0) error(p->lineno, "pattern refers to undefined field %s", p->dangling[k]);
    if (p->ndangling && fmt->ndangling)
        error(p->lineno, "pattern that uses fields defined in format cannot use format that uses fields defined in pattern");
    sprintf(lv, "u.f_%s.", arg);
    if (fmt->ndangling) output_fields(p, i, lv);
    if (!extracted) {
        extract_name(p->base, ex);
        indent(i);
        out("%s(ctx, &u.f_%s, insn);\n", ex, arg);
    }
    if (!fmt->ndangling) output_fields(p, i, lv);
    indent(i);
    out("if (%s_%s(ctx, &u.f_%s)) return true;\n", translate_prefix, p->name, arg);
}

static int cmp_u64(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return x < y ? -1 : x > y;
}

static void tree_output_code(int t, int i, int extracted, u64 outerbits, u64 outermask) {
    Node* tr = &nodes[t];
    int sh = is_contiguous(tr->thismask), k, j;
    u64* order;
    char ex[NAME_LEN * 2];
    if (!extracted && tr->base >= 0) {
        dangling_references(&formats[tr->base]);
        if (!formats[tr->base].ndangling) {
            extract_name(tr->base, ex);
            indent(i);
            out("%s(ctx, &u.f_%s, insn);\n", ex, args[formats[tr->base].base].name);
            extracted = 1;
        }
    }
    indent(i);
    if (sh > 0)
        out("switch ((insn >> %d) & 0x%llx) {\n", sh, tr->thismask >> sh);
    else
        out("switch (insn & %s) {\n", whexC(tr->thismask));
    order = (u64*)xalloc(sizeof(u64) * (size_t)tr->nkids);
    memcpy(order, tr->bits, sizeof(u64) * (size_t)tr->nkids);
    qsort(order, (size_t)tr->nkids, sizeof(u64), cmp_u64);
    for (k = 0; k < nodes[t].nkids; k++) {
        u64 b = order[k], innermask, innerbits;
        int s = -1;
        for (j = 0; j < nodes[t].nkids; j++)
            if (nodes[t].bits[j] == b) s = nodes[t].kids[j];
        innermask = outermask | nodes[t].thismask;
        innerbits = outerbits | b;
        indent(i);
        if (sh > 0)
            out("case 0x%llx:\n", b >> sh);
        else
            out("case %s:\n", whexC(b));
        indent(i);
        out("    /* %s */\n", str_match_bits(innerbits, innermask));
        output_code(s, i + 4, extracted, innerbits, innermask);
        indent(i);
        out("    break;\n");
    }
    indent(i);
    out("}\n");
    free(order);
}

static void output_code(int n, int i, int extracted, u64 outerbits, u64 outermask) {
    Node* g = &nodes[n];
    int k;
    switch (g->kind) {
        case N_PAT: pattern_output_code(g->pat, i, extracted); break;
        case N_EXC: tree_output_code(g->tree, i, extracted, outerbits, outermask); break;
        case N_TREE: tree_output_code(n, i, extracted, outerbits, outermask); break;
        default:
            for (k = 0; k < nodes[n].nkids; k++) {
                int p = nodes[n].kids[k];
                u64 fm = node_fixedmask(p), fb = node_fixedbits(p);
                if (outermask != fm) {
                    u64 innermask = fm & ~outermask, innerbits = fb & ~outermask;
                    indent(i);
                    out("if ((insn & %s) == %s) {\n", whexC(innermask), whexC(innerbits));
                    indent(i);
                    out("    /* %s */\n", str_match_bits(fb, fm));
                    output_code(p, i + 4, extracted, fb, fm);
                    indent(i);
                    out("}\n");
                } else
                    output_code(p, i, extracted, fb, fm);
            }
            break;
    }
}

/* ------------------------------------------------------------------ parsing */
static int is_c_ident(const char* s) {
    if (!isalpha((unsigned char)*s)) return 0;
    for (s++; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

static int is_ident_chars(const char* s) { /* [a-zA-Z0-9_]* */
    for (; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

static int is_digits(const char* s) {
    if (!*s) return 0;
    for (; *s; s++)
        if (!isdigit((unsigned char)*s)) return 0;
    return 1;
}

/* NAME:[s]DIGITS, with NAME a C identifier (want_ident) or digits */
static int split_colon(const char* t, int want_ident, char* name, int* sign, int* len) {
    const char* c = strchr(t, ':');
    char left[NAME_LEN];
    size_t n;
    if (!c) return 0;
    n = (size_t)(c - t);
    if (n == 0 || n >= NAME_LEN) return 0;
    memcpy(left, t, n);
    left[n] = 0;
    if (want_ident ? !is_c_ident(left) : !is_digits(left)) return 0;
    c++;
    *sign = *c == 's';
    if (*sign) c++;
    if (!is_digits(c)) return 0;
    strcpy(name, left);
    *len = atoi(c);
    return 1;
}

static void parse_field(int lineno, const char* name, char** toks, int ntoks) {
    int subs[MAX_SUBS], nsubs = 0, width = 0, k, f;
    char func[NAME_LEN] = "";
    for (k = 0; k < ntoks; k++) {
        const char* t = toks[k];
        char left[NAME_LEN];
        int sign, len;
        if (!strncmp(t, "!function=", 10)) {
            if (*func) error(lineno, "duplicate function");
            if (strlen(t + 10) >= NAME_LEN) error(lineno, "function name too long");
            strcpy(func, t + 10);
            continue;
        }
        if (nsubs == MAX_SUBS) error(lineno, "too many field parts");
        if (split_colon(t, 1, left, &sign, &len)) {
            int n = new_fnode(F_NAMED);
            strcpy(fnodes[n].name, left);
            fnodes[n].sign = sign;
            fnodes[n].len = len;
            subs[nsubs++] = n;
            width += len;
            continue;
        }
        if (split_colon(t, 0, left, &sign, &len)) {
            int po = atoi(left), n;
            if (po + len > insnwidth) error(lineno, "field %s too large", t);
            n = new_fnode(F_FIELD);
            fnodes[n].sign = sign;
            fnodes[n].pos = po;
            fnodes[n].len = len;
            fnodes[n].mask = (len >= 64 ? ~0ull : ((1ull << len) - 1)) << po;
            subs[nsubs++] = n;
            width += len;
            continue;
        }
        error(lineno, "invalid field token \"%s\"", t);
    }
    if (width > insnwidth) error(lineno, "field too large");
    if (!nsubs) {
        if (!*func) error(lineno, "field with no value");
        f = new_fnode(F_PARAM);
        strcpy(fnodes[f].func, func);
    } else {
        if (nsubs == 1)
            f = subs[0];
        else {
            u64 mask = 0;
            for (k = 0; k < nsubs; k++) {
                if (mask & fnodes[subs[k]].mask) error(lineno, "field components overlap");
                mask |= fnodes[subs[k]].mask;
            }
            f = new_fnode(F_MULTI);
            memcpy(fnodes[f].subs, subs, sizeof subs);
            fnodes[f].nsubs = nsubs;
            fnodes[f].mask = mask;
            fnodes[f].sign = fnodes[subs[0]].sign;
        }
        if (*func) {
            int b = f;
            f = new_fnode(F_FUNC);
            strcpy(fnodes[f].func, func);
            fnodes[f].base = b;
            fnodes[f].mask = fnodes[b].mask;
            fnodes[f].sign = fnodes[b].sign;
        }
    }
    if (flds_find(gfields, name) >= 0) error(lineno, "duplicate field %s", name);
    add_field(lineno, gfields, name, f);
}

static void parse_arguments(int lineno, const char* name, char** toks, int ntoks) {
    Args a;
    int k;
    memset(&a, 0, sizeof a);
    if (strlen(name) >= NAME_LEN) error(lineno, "name too long");
    strcpy(a.name, name);
    for (k = 0; k < ntoks; k++) {
        char n[NAME_LEN], t[NAME_LEN];
        const char* c = strchr(toks[k], ':');
        if (!strcmp(toks[k], "!extern")) {
            a.extern_ = anyextern = 1;
            continue;
        }
        if (strlen(toks[k]) >= NAME_LEN) error(lineno, "name too long");
        if (c) {
            memcpy(n, toks[k], (size_t)(c - toks[k]));
            n[c - toks[k]] = 0;
            strcpy(t, c + 1);
            if (!is_c_ident(n) || !is_c_ident(t)) error(lineno, "invalid argument set token \"%s\"", toks[k]);
        } else if (is_c_ident(toks[k])) {
            strcpy(n, toks[k]);
            strcpy(t, "int");
        } else
            error(lineno, "invalid argument set token \"%s\"", toks[k]);
        if (args_has(&a, n)) error(lineno, "duplicate argument \"%s\"", n);
        if (a.nfields == MAX_FLDS) error(lineno, "too many arguments");
        strcpy(a.fields[a.nfields], n);
        strcpy(a.types[a.nfields], t);
        a.nfields++;
    }
    if (args_find(name) >= 0) error(lineno, "duplicate argument set %s", name);
    GROW(args, nargs, cargs);
    args[nargs++] = a;
}

static int infer_argument_set(const Flds* flds) {
    int i, k;
    Args a;
    for (i = 0; i < nargs; i++) {
        int ok = args[i].nfields == flds->n;
        for (k = 0; k < args[i].nfields && ok; k++) ok = !strcmp(args[i].types[k], "int");
        for (k = 0; k < flds->n && ok; k++) ok = args_has(&args[i], flds->name[k]);
        if (ok) return i;
    }
    memset(&a, 0, sizeof a);
    sprintf(a.name, "%s%d", decode_function, nargs);
    for (k = 0; k < flds->n; k++) {
        strcpy(a.fields[k], flds->name[k]);
        strcpy(a.types[k], "int");
    }
    a.nfields = flds->n;
    GROW(args, nargs, cargs);
    args[nargs++] = a;
    return nargs - 1;
}

static int eq_fields_for_fmts(const Flds* a, const Flds* b) {
    int k;
    if (a->n != b->n) return 0;
    for (k = 0; k < a->n; k++) {
        int j = flds_find(b, a->name[k]);
        if (j < 0) return 0;
        if (fnodes[a->node[k]].kind != fnodes[b->node[j]].kind || !field_eq(a->node[k], b->node[j])) return 0;
    }
    return 1;
}

/* decodetree.py's infer_format puts every field in the format (its test for constants never holds) */
static int infer_format(int arg, u64 fieldmask, const Flds* flds, int width) {
    int i;
    General f;
    if (arg < 0) arg = infer_argument_set(flds);
    for (i = 0; i < nformats; i++) {
        if (formats[i].base != arg || formats[i].fieldmask != fieldmask || formats[i].width != width) continue;
        if (!eq_fields_for_fmts(flds, &formats[i].fields)) continue;
        return i;
    }
    memset(&f, 0, sizeof f);
    sprintf(f.name, "%s_Fmt_%d", decode_function, nformats);
    f.file = input_file;
    f.base = arg;
    f.fieldmask = fieldmask;
    f.fields = *flds;
    f.width = width;
    GROW(formats, nformats, cformats);
    formats[nformats++] = f;
    return nformats - 1;
}

static void parse_generic(int lineno, int parent, const char* name, char** toks, int ntoks) {
    int is_format = parent < 0, width = 0, arg = -1, fmt = -1, fmt_at = -1, k, i;
    u64 fixedmask = 0, fixedbits = 0, undefmask = 0, fieldmask = 0;
    Flds* flds = (Flds*)xalloc(sizeof(Flds));
    General g;
    for (k = 0; k < ntoks; k++) {
        const char* t = toks[k];
        const char* eq = strchr(t, '=');
        char fname[NAME_LEN];
        int sign, len, shift = 0;
        if (t[0] == '&' && is_ident_chars(t + 1)) {
            if (arg >= 0) error(lineno, "multiple argument sets");
            if ((arg = args_find(t + 1)) < 0) error(lineno, "undefined argument set %s", t);
            continue;
        }
        if (t[0] == '@' && is_ident_chars(t + 1)) {
            if (fmt >= 0) error(lineno, "multiple formats");
            if ((fmt = formats_find(t + 1)) < 0) error(lineno, "undefined format %s", t);
            fmt_at = flds->n;
            continue;
        }
        if (t[0] == '%' && is_ident_chars(t + 1)) {
            int f = flds_find(gfields, t + 1);
            if (f < 0) error(lineno, "undefined field %s", t + 1);
            add_field(lineno, flds, t + 1, gfields->node[f]);
            continue;
        }
        if (eq && (size_t)(eq - t) < NAME_LEN) {
            memcpy(fname, t, (size_t)(eq - t));
            fname[eq - t] = 0;
            if (is_c_ident(fname) && eq[1] == '%' && is_ident_chars(eq + 2)) {
                int f = flds_find(gfields, eq + 2);
                if (f < 0) error(lineno, "undefined field %s", eq + 2);
                add_field(lineno, flds, fname, gfields->node[f]);
                continue;
            }
            if (is_c_ident(fname) && is_digits(eq + 1 + (eq[1] == '+' || eq[1] == '-'))) {
                int c = new_fnode(F_CONST);
                fnodes[c].value = strtoll(eq + 1, NULL, 10);
                fnodes[c].sign = fnodes[c].value < 0;
                add_field(lineno, flds, fname, c);
                continue;
            }
        }
        if (strspn(t, "01.-") == strlen(t)) {
            const char* c;
            shift = (int)strlen(t);
            for (c = t; *c; c++) {
                fixedbits = (fixedbits << 1) | (*c == '1');
                fixedmask = (fixedmask << 1) | (*c == '0' || *c == '1');
                undefmask = (undefmask << 1) | (*c == '-');
            }
        } else if (split_colon(t, 1, fname, &sign, &len)) {
            int f;
            shift = len;
            if (shift + width > insnwidth) error(lineno, "field %s exceeds insnwidth", fname);
            f = new_fnode(F_FIELD);
            fnodes[f].sign = sign;
            fnodes[f].pos = insnwidth - width - shift;
            fnodes[f].len = shift;
            fnodes[f].mask = ((1ull << shift) - 1) << fnodes[f].pos;
            add_field(lineno, flds, fname, f);
            fixedbits <<= shift;
            fixedmask <<= shift;
            undefmask <<= shift;
        } else
            error(lineno, "invalid token \"%s\"", t);
        width += shift;
    }
    if (!(is_format && width == 0) && width != insnwidth) error(lineno, "definition has %d bits", width);
    for (k = 0; k < flds->n; k++) fieldmask |= fnodes[flds->node[k]].mask;

    memset(&g, 0, sizeof g);
    if (strlen(name) >= NAME_LEN) error(lineno, "name too long");
    strcpy(g.name, name);
    g.file = input_file;
    g.lineno = lineno;
    if (is_format) {
        if (fmt >= 0) error(lineno, "format referencing format");
        if (arg >= 0) {
            for (k = 0; k < flds->n; k++)
                if (!args_has(&args[arg], flds->name[k]))
                    error(lineno, "field %s not in argument set %s", flds->name[k], args[arg].name);
        } else
            arg = infer_argument_set(flds);
        if (formats_find(name) >= 0) error(lineno, "duplicate format name %s", name);
        g.base = arg;
        g.fixedbits = fixedbits;
        g.fixedmask = fixedmask;
        g.undefmask = undefmask;
        g.fieldmask = fieldmask;
        g.fields = *flds;
        g.width = width;
        GROW(formats, nformats, cformats);
        formats[nformats++] = g;
    } else {
        int n;
        if (fmt >= 0) {
            if (arg >= 0) error(lineno, "pattern specifies both format and argument set");
            if (fixedmask & formats[fmt].fixedmask) error(lineno, "pattern fixed bits overlap format fixed bits");
            if (width != formats[fmt].width) error(lineno, "pattern uses format of different width");
            fieldmask |= formats[fmt].fieldmask;
            fixedbits |= formats[fmt].fixedbits;
            fixedmask |= formats[fmt].fixedmask;
            undefmask |= formats[fmt].undefmask;
            /* the order a64's bodies take: the pattern's fields with the format's where @format is */
            for (k = 0; k < fmt_at; k++) add_field(lineno, &g.all, flds->name[k], flds->node[k]);
            for (k = 0; k < formats[fmt].fields.n; k++)
                add_field(lineno, &g.all, formats[fmt].fields.name[k], formats[fmt].fields.node[k]);
            for (k = fmt_at; k < flds->n; k++) add_field(lineno, &g.all, flds->name[k], flds->node[k]);
        } else {
            fmt = infer_format(arg, fieldmask, flds, width);
            g.all = *flds;
            flds->n = 0;
        }
        arg = formats[fmt].base;
        for (k = 0; k < flds->n; k++) {
            if (!args_has(&args[arg], flds->name[k]))
                error(lineno, "field %s not in argument set %s", flds->name[k], args[arg].name);
            if (flds_find(&formats[fmt].fields, flds->name[k]) >= 0)
                error(lineno, "field %s set by format and pattern", flds->name[k]);
        }
        for (i = 0; i < args[arg].nfields; i++)
            if (flds_find(flds, args[arg].fields[i]) < 0 && flds_find(&formats[fmt].fields, args[arg].fields[i]) < 0)
                error(lineno, "field %s not initialized", args[arg].fields[i]);
        g.base = fmt;
        g.fixedbits = fixedbits;
        g.fixedmask = fixedmask;
        g.undefmask = undefmask;
        g.fieldmask = fieldmask;
        g.fields = *flds;
        g.width = width;
        g.ignmask = undefmask;
        GROW(pats, npats, cpats);
        pats[npats++] = g;
        n = new_node(N_PAT, lineno);
        nodes[n].pat = npats - 1;
        add_kid(parent, n, 0);
    }
    if (fieldmask & fixedmask) error(lineno, "fieldmask overlaps fixedmask (%s & %s)", whex(fieldmask), whex(fixedmask));
    if (fieldmask & undefmask) error(lineno, "fieldmask overlaps undefmask (%s & %s)", whex(fieldmask), whex(undefmask));
    if (fixedmask & undefmask) error(lineno, "fixedmask overlaps undefmask (%s & %s)", whex(fixedmask), whex(undefmask));
    if (!is_format && (fieldmask | fixedmask | undefmask) != insnmask)
        error(lineno, "bits left unspecified (%s)", whex((fieldmask | fixedmask | undefmask) ^ insnmask));
    free(flds);
}

static void parse_file(FILE* f, int parent) {
    char line[4096], expanded[8192], *toks[MAX_TOK], store[8192];
    int ntoks = 0, lineno = 0, nesting = 0, start_lineno = 0, ind = 0, stack[64], depth = 0;
    size_t used = 0;
    while (fgets(line, sizeof line, f)) {
        char *p, *e, *name;
        size_t len1, len2, col = 0, k;
        int nt = 0;
        lineno++;
        /* rstrip, expandtabs, then the indent */
        for (k = 0, p = line; *p; p++) {
            if (*p == '\t') {
                do expanded[k++] = ' ', col++;
                while (col % 8);
            } else if (*p != '\n' && *p != '\r') {
                expanded[k++] = *p;
                col++;
            }
        }
        while (k && isspace((unsigned char)expanded[k - 1])) k--;
        expanded[k] = 0;
        len1 = k;
        for (p = expanded; *p == ' ' || isspace((unsigned char)*p); p++) continue;
        len2 = strlen(p);
        if ((e = strchr(p, '#'))) *e = 0;
        /* split into tokens, kept in store */
        if (!ntoks) used = 0;
        for (;;) {
            char* w;
            while (*p && isspace((unsigned char)*p)) p++;
            if (!*p) break;
            w = p;
            while (*p && !isspace((unsigned char)*p)) p++;
            if (ntoks + nt == MAX_TOK || used + (size_t)(p - w) + 1 > sizeof store) error(lineno, "line too long");
            memcpy(store + used, w, (size_t)(p - w));
            store[used + (size_t)(p - w)] = 0;
            toks[ntoks + nt++] = store + used;
            used += (size_t)(p - w) + 1;
        }
        if (ntoks) {
            ntoks += nt;
        } else {
            if (len1 == 0) continue;
            ind = (int)(len1 - len2);
            if (!nt) {
                if (ind != nesting) error(lineno, "indentation %d != %d", ind, nesting);
                continue;
            }
            start_lineno = lineno;
            ntoks = nt;
        }
        if (!strcmp(toks[ntoks - 1], "\\")) {
            ntoks--;
            if (!ntoks) ntoks = 0;
            continue;
        }
        name = toks[0];
        if (!strcmp(name, "}") || !strcmp(name, "]")) {
            if (ntoks != 1) error(start_lineno, "extra tokens after close brace");
            if ((name[0] == '}') != (nodes[parent].kind == N_INC)) error(lineno, "mismatched close brace");
            if (!depth) error(lineno, "extra close brace");
            parent = stack[--depth];
            nesting -= 2;
            if (ind != nesting) error(lineno, "indentation %d != %d", ind, nesting);
            ntoks = 0;
            continue;
        }
        if (ind != nesting) error(start_lineno, "indentation %d != %d", ind, nesting);
        if (!strcmp(name, "{") || !strcmp(name, "[")) {
            int g;
            if (ntoks != 1) error(start_lineno, "extra tokens after open brace");
            g = new_node(name[0] == '{' ? N_INC : N_EXC, start_lineno);
            add_kid(parent, g, 0);
            if (depth == 64) error(lineno, "nested too deep");
            stack[depth++] = parent;
            parent = g;
            nesting += 2;
            ntoks = 0;
            continue;
        }
        if (name[0] == '%' && is_ident_chars(name + 1))
            parse_field(start_lineno, name + 1, toks + 1, ntoks - 1);
        else if (name[0] == '&' && is_ident_chars(name + 1))
            parse_arguments(start_lineno, name + 1, toks + 1, ntoks - 1);
        else if (name[0] == '@' && is_ident_chars(name + 1))
            parse_generic(start_lineno, -1, name + 1, toks + 1, ntoks - 1);
        else if (is_ident_chars(name))
            parse_generic(start_lineno, parent, name, toks + 1, ntoks - 1);
        else
            error(lineno, "invalid token \"%s\"", name);
        ntoks = 0;
    }
    if (nesting) error(lineno, "missing close brace");
}

/* ------------------------------------------------------------------ main */
static int cmp_args(const void* a, const void* b) { return strcmp(args[*(const int*)a].name, args[*(const int*)b].name); }
static int cmp_fmts(const void* a, const void* b) {
    return strcmp(formats[*(const int*)a].name, formats[*(const int*)b].name);
}

static void usage(void) {
    error_with_file(NULL, 0,
                    "usage: decodetree [--decode NAME | --static-decode NAME] [--translate PREFIX] [-w 16|32|64] "
                    "[-o OUT] FILE...\n       decodetree --a64-profile PROFILE --a64-header OUT.h --a64-decoder OUT.inc "
                    "[--stats] FILE...");
}

int main(int argc, char** argv) {
    const char* output_file = NULL;
    const char *a64_profile = NULL, *a64_header = NULL, *a64_decoder = NULL;
    int a64_stats = 0;
    const char* inputs[64];
    int ninputs = 0, i, k, top, *order;
    for (i = 1; i < argc; i++) {
        const char* a = argv[i];
        const char* v = NULL;
        const char* eq = strchr(a, '=');
        char opt[64];
        if (a[0] != '-') {
            if (ninputs == 64) usage();
            inputs[ninputs++] = a;
            continue;
        }
        if (a[1] == '-' && eq) { /* --opt=value */
            if ((size_t)(eq - a) >= sizeof opt) usage();
            memcpy(opt, a, (size_t)(eq - a));
            opt[eq - a] = 0;
            v = eq + 1;
        } else {
            if (strlen(a) >= sizeof opt) usage();
            strcpy(opt, a);
        }
#define VALUE() (v ? v : (i + 1 < argc ? argv[++i] : (usage(), "")))
        if (!strcmp(opt, "-o") || !strcmp(opt, "--output"))
            output_file = VALUE();
        else if (!strcmp(opt, "--decode")) {
            decode_function = VALUE();
            decode_scope = "";
        } else if (!strcmp(opt, "--static-decode"))
            decode_function = VALUE();
        else if (!strcmp(opt, "--translate")) {
            translate_prefix = VALUE();
            translate_scope = "";
        } else if (!strcmp(opt, "-w") || !strcmp(opt, "--insnwidth")) {
            insnwidth = atoi(VALUE());
            if (insnwidth == 16) {
                insntype = "uint16_t";
                insnmask = 0xffff;
            } else if (insnwidth == 64) {
                insntype = "uint64_t";
                insnmask = ~0ull;
                bitop_width = 64;
            } else if (insnwidth != 32)
                error_with_file(NULL, 0, "cannot handle insns of width %d", insnwidth);
        } else if (!strcmp(opt, "--a64-profile"))
            a64_profile = VALUE();
        else if (!strcmp(opt, "--a64-header"))
            a64_header = VALUE();
        else if (!strcmp(opt, "--a64-decoder"))
            a64_decoder = VALUE();
        else if (!strcmp(opt, "--stats"))
            a64_stats = 1;
        else
            usage();
#undef VALUE
    }
    if (!ninputs) error_with_file(NULL, 0, "missing input file");

    gfields = (Flds*)xalloc(sizeof(Flds) * 16);
    top = new_node(N_EXC, 0);
    for (i = 0; i < ninputs; i++) {
        FILE* f = fopen(inputs[i], "r");
        input_file = inputs[i];
        if (!f) error_with_file(NULL, 0, "cannot read %s", inputs[i]);
        parse_file(f, top);
        fclose(f);
    }
    /* the top level's own masks stay 0: decoding starts from nothing */
    for (i = 0; i < nodes[top].nkids; i++) prop_masks(nodes[top].kids[i]);
    build_tree(top);
    prop_format(top);
    if (a64_profile || a64_header || a64_decoder) {
        if (!a64_profile || !a64_header || !a64_decoder || output_file)
            error_with_file(NULL, 0, "--a64-profile, --a64-header and --a64-decoder go together, without -o");
        return a64_generate(a64_profile, a64_header, a64_decoder, a64_stats, inputs, ninputs);
    }

    out("/* This file is autogenerated by tools/decodetree, from QEMU's scripts/decodetree.py.  */\n\n");
    order = (int*)xalloc(sizeof(int) * (size_t)(nargs + nformats + 1));
    for (i = 0; i < nargs; i++) order[i] = i;
    qsort(order, (size_t)nargs, sizeof(int), cmp_args);
    for (i = 0; i < nargs; i++) {
        const Args* a = &args[order[i]];
        if (a->extern_) continue;
        out("typedef struct {\n");
        for (k = 0; k < a->nfields; k++) out("    %s %s;\n", a->types[k], a->fields[k]);
        out("} arg_%s;\n\n", a->name);
    }
    if (anyextern)
        out("#pragma GCC diagnostic push\n#pragma GCC diagnostic ignored \"-Wredundant-decls\"\n#ifdef __clang__\n"
            "#  pragma GCC diagnostic ignored \"-Wtypedef-redefinition\"\n#endif\n\n");
    for (i = 0; i < npats; i++) {
        int first = -1;
        for (k = 0; k < i && first < 0; k++)
            if (!strcmp(pats[k].name, pats[i].name)) first = k;
        if (first >= 0) {
            if (formats[pats[first].base].base != formats[pats[i].base].base)
                error_with_file(NULL, 0, "%s has conflicting argument sets", pats[i].name);
            continue;
        }
        out("typedef arg_%s arg_%s;\n", args[formats[pats[i].base].base].name, pats[i].name);
        out("%sbool %s_%s(DisasContext *ctx, arg_%s *a);\n", translate_scope, translate_prefix, pats[i].name,
            pats[i].name);
    }
    out("\n");
    if (anyextern) out("#pragma GCC diagnostic pop\n\n");
    for (i = 0; i < nformats; i++) order[i] = i;
    qsort(order, (size_t)nformats, sizeof(int), cmp_fmts);
    for (i = 0; i < nformats; i++) {
        General* f = &formats[order[i]];
        char ex[NAME_LEN * 2];
        extract_name(order[i], ex);
        out("static void %s(DisasContext *ctx, arg_%s *a, %s insn)\n{\n", ex, args[f->base].name, insntype);
        output_fields(f, 4, "a->");
        out("}\n\n");
    }
    out("%sbool %s(DisasContext *ctx, %s insn)\n{\n", decode_scope, decode_function, insntype);
    if (npats) {
        for (i = 0; i < nargs; i++) order[i] = i;
        qsort(order, (size_t)nargs, sizeof(int), cmp_args);
        out("    union {\n");
        for (i = 0; i < nargs; i++) out("        arg_%s f_%s;\n", args[order[i]].name, args[order[i]].name);
        out("    } u;\n\n");
        output_code(top, 4, 0, 0, 0);
    }
    out("    return false;\n}\n");
    flush(output_file);
    return 0;
}
