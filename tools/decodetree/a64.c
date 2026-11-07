/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* decodetree's A64 output (--a64): the A64 instruction decoder from the .decode files (QEMU's a64.decode, and later
   sve.decode and sme.decode) and ScarletNX's profile of them (src/common/cpu/aarch64/decode/a64.profile). It was
   tools/a64dec; its .decode parser is now decodetree.c's, whose patterns load_rows takes in order.

   The profile gives each name a line: its flow kind, `nop` when it has no architectural effect at EL0, predicates on
   its arguments (a row whose predicate fails does not match), and the FEAT_* features it needs, some only when a
   condition on its arguments holds, or EL1 for an encoding that is UNDEF at EL0. A row needing a feature the target
   lacks does not match either, so the word is UNDEF unless another row (in a group, a later one) matches it. A
   profile name the .decode files lack, and a name they have that the profile lacks, are errors. The profile also
   declares the features (their bit numbers, in that order) and the target sets. Two rows that can match the same
   word must be in a { } group, and not both in one [ ] group in it. A field's !function= is one of a fixed list.

   It writes two files, both derived from the .decode files (LGPL-2.1-or-later):
   - the header: the features and targets, SNX_A64_OPS(X) (the names), SnxA64Op, SnxA64Flow, SnxA64Args (every
     argument of every row, as int) and SnxA64Insn
   - the decoder: the rows (mask, value, features, op, body), each row's body (its fields, predicates and conditional
     features) in one switch, a static index of 2048 buckets on bits [31:21] with a second level on a run of lower
     bits for a bucket of more than 16 rows, and snx_a64_decode(word, features, insn). It ends with
     SNX_A64_DECODE_HASH, the FNV-1a hash of everything before it. */
#include "decodetree.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILES    8
#define MAX_FIELDS   1024
#define MAX_SUB      4
#define MAX_ARGSETS  128
#define MAX_MEMBERS  16
#define MAX_FORMATS  512
#define MAX_ASSIGN   16
#define MAX_ROWS     4096
#define MAX_NAMES    2048
#define MAX_ARGS     160
#define MAX_PRED     8
#define MAX_OR       4
#define MAX_FEATS    64
#define MAX_NFEAT    6
#define MAX_TARGETS  8
#define MAX_TOKENS   64
#define LINE_LEN     2048
#define A64_NAME     40
#define BUCKET_BITS  11
#define BUCKETS      (1 << BUCKET_BITS)
#define CROWDED      16
#define MAX_SPLIT    6
#define MAX_LIST     65535

enum { FLOW_UNDEF, FLOW_PLAIN, FLOW_B, FLOW_BL, FLOW_BC, FLOW_BR, FLOW_BLR, FLOW_RET, FLOW_ERET, FLOW_SVC, FLOW_BRK,
       FLOW_EXC, FLOW_COUNT };
static const char* const flow_names[FLOW_COUNT] = {"UNDEF", "PLAIN", "B",   "BL",  "BC",  "BR",
                                                   "BLR",   "RET",   "ERET", "SVC", "BRK", "EXC"};

enum { OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE };
static const char* const op_text[] = {"==", "!=", "<", "<=", ">", ">="};

/* the !function= decodetree may name; @ stands for the field's value */
static const struct {
    const char* name;
    const char* expr;
} funcs[] = {
    {"", "@"},
    {"plus_1", "(@ + 1)"},
    {"plus_2", "(@ + 2)"},
    {"xor_2", "(@ ^ 2)"},
    {"rsub_8", "(8 - @)"},
    {"rsub_16", "(16 - @)"},
    {"rsub_32", "(32 - @)"},
    {"rsub_64", "(64 - @)"},
    {"shl_12", "(@ * 4096)"},
    {"times_4", "(@ * 4)"},
    {"times_8", "(@ * 8)"},
    /* the size of an MTE tag granule is 16 bytes */
    {"scale_by_log2_tag_granule", "(@ * 16)"},
    /* sz:3 below imm12, the immediate scaled by the access size */
    {"uimm_scaled", "(int)(((unsigned)@ >> 3) << ((unsigned)@ & 7))"},
};
#define NFUNCS ((int)(sizeof funcs / sizeof funcs[0]))

typedef struct {
    int pos, len, sign;
    int ref; /* another argument's index + 1, in place of bits */
} Sub;

typedef struct {
    char name[A64_NAME];
    Sub sub[MAX_SUB];
    int nsub, func, file;
} Field;

typedef struct {
    char name[A64_NAME];
    int member[MAX_MEMBERS];
    int nmember, file;
} Argset;

typedef struct {
    int arg;
    int field; /* index + 1, or 0 for the constant */
    int value;
} Assign;

/* a format, or a pattern merged with its format */
typedef struct {
    char name[A64_NAME];
    unsigned fixmask, fixval, fieldmask, ignmask;
    Assign as[MAX_ASSIGN];
    int nas;
    int argset; /* index + 1 */
    int file, line;
} Pat;

typedef struct {
    Pat p;
    int name;
    int node; /* its node in decodetree.c's tree, for check_overlaps */
    unsigned long long feat;
    int body;
} Row;

typedef struct {
    int arg, op, value;
    int rhs; /* another argument's index + 1, in place of value */
    int mask; /* arg & mask is compared, 0 for the whole value */
} Cmp;

/* comparisons joined by |, or a named check of one argument */
typedef struct {
    Cmp c[MAX_OR];
    int n;
    int bitmask; /* valid=bitmask(arg): c[0].arg is a logical immediate's N:immr:imms */
} Pred;

typedef struct {
    int feat;
    int has_cond;
    Pred cond;
} Need;

typedef struct {
    char name[A64_NAME];
    int flow, nop, profiled, line;
    Pred pred[MAX_PRED];
    int npred;
    Need need[MAX_NFEAT];
    int nneed;
} Name;

typedef struct {
    char name[A64_NAME];
    unsigned long long feats;
} Target;

typedef struct {
    int start, count, lo, bits;
} Bucket;

static struct {
    const char* files[MAX_FILES];
    int nfiles, file, line, stats;
    int cand[MAX_ROWS]; /* build_index's rows of one bucket */
    const char* where; /* the file being read, for messages */
    Field fields[MAX_FIELDS];
    int nfields;
    Argset argsets[MAX_ARGSETS];
    int nargsets;
    Pat formats[MAX_FORMATS];
    int nformats;
    Row rows[MAX_ROWS];
    int nrows;
    Name names[MAX_NAMES];
    int nnames;
    char args[MAX_ARGS][A64_NAME];
    int nargs;
    char feats[MAX_FEATS][A64_NAME];
    int nfeats;
    Target targets[MAX_TARGETS];
    int ntargets;
    int depth_group, depth_sub, ngroups, nsubs, in_group, in_sub;
    /* the bodies, as text, one per distinct row body */
    char** bodies;
    int nbodies;
    int uses_bitmask;
    Bucket buckets[BUCKETS];
    Bucket subs[BUCKETS * 8];
    int nsubs_out;
    unsigned short list[MAX_LIST];
    int nlist;
    char* obuf;
    size_t olen, ocap;
} g;

static void fail(const char* fmt, ...) {
    va_list ap;
    if (g.where && g.line)
        fprintf(stderr, "%s:%d: ", g.where, g.line);
    else
        fprintf(stderr, "decodetree: ");
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void out(const char* fmt, ...) {
    va_list ap;
    int n;
    for (;;) {
        size_t room = g.ocap - g.olen;
        va_start(ap, fmt);
        n = vsnprintf(g.obuf ? g.obuf + g.olen : NULL, room, fmt, ap);
        va_end(ap);
        if (n < 0) fail("cannot format the output");
        if ((size_t)n < room) break;
        g.ocap = (g.olen + (size_t)n + 1) * 2;
        g.obuf = (char*)realloc(g.obuf, g.ocap);
        if (!g.obuf) fail("out of memory");
    }
    g.olen += (size_t)n;
}

/* written only when it changes, so what includes it recompiles only then (Ninja's restat) */
static void flush(const char* path) {
    FILE* f = fopen(path, "rb");
    if (f) {
        char* old = (char*)malloc(g.olen + 1);
        size_t got = old ? fread(old, 1, g.olen + 1, f) : 0;
        int same = old && got == g.olen && !memcmp(old, g.obuf, g.olen);
        free(old);
        fclose(f);
        if (same) {
            g.olen = 0;
            return;
        }
    }
    f = fopen(path, "wb");
    if (!f || fwrite(g.obuf, 1, g.olen, f) != g.olen || fclose(f)) fail("cannot write %s", path);
    g.olen = 0;
}

static void copy_name(char* dst, const char* src) {
    if (strlen(src) >= A64_NAME) fail("the name %s is too long", src);
    strcpy(dst, src);
}

static int is_ident(const char* s) {
    if (!isalpha((unsigned char)*s) && *s != '_') return 0;
    for (s++; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_') return 0;
    return 1;
}

static int parse_int(const char* s, int* value) {
    char* end;
    long v = strtol(s, &end, 0);
    if (!*s || *end || v < -0x7FFFFFFFL || v > 0x7FFFFFFFL) return 0;
    *value = (int)v;
    return 1;
}

static int arg_index(const char* name, int add) {
    int i;
    for (i = 0; i < g.nargs; i++)
        if (!strcmp(g.args[i], name)) return i;
    if (!add) return -1;
    if (!is_ident(name)) fail("%s is not an argument name", name);
    if (g.nargs == MAX_ARGS) fail("more than %d argument names", MAX_ARGS);
    copy_name(g.args[g.nargs], name);
    return g.nargs++;
}

static int name_index(const char* name, int add) {
    int i;
    for (i = 0; i < g.nnames; i++)
        if (!strcmp(g.names[i].name, name)) return i;
    if (!add) return -1;
    if (g.nnames == MAX_NAMES) fail("more than %d names", MAX_NAMES);
    copy_name(g.names[g.nnames].name, name);
    return g.nnames++;
}

static int split(char* line, char** tok) {
    int n = 0;
    char* p = line;
    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) return n;
        if (n == MAX_TOKENS) fail("more than %d tokens on a line", MAX_TOKENS);
        tok[n++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
}

/* ---- the rows, from decodetree.c's parse ---- */

static void add_assign(Pat* p, int arg, int field, int value) {
    int i;
    for (i = 0; i < p->nas; i++)
        if (p->as[i].arg == arg) fail("%s is given twice", g.args[arg]);
    if (p->nas == MAX_ASSIGN) fail("more than %d arguments", MAX_ASSIGN);
    p->as[p->nas].arg = arg;
    p->as[p->nas].field = field;
    p->as[p->nas].value = value;
    p->nas++;
}

/* the bits covered by a field's subfields */
static unsigned field_bits(const Field* f) {
    unsigned m = 0;
    int i;
    for (i = 0; i < f->nsub; i++)
        if (!f->sub[i].ref) m |= (f->sub[i].len == 32 ? 0xFFFFFFFFu : ((1u << f->sub[i].len) - 1)) << f->sub[i].pos;
    return m;
}

static void finish_row(Row* r) {
    unsigned covered = r->p.fixmask | r->p.fieldmask | r->p.ignmask;
    int i;
    if (r->p.fixmask & (r->p.fieldmask | r->p.ignmask)) fail("a fixed bit is also a field or ignored");
    for (i = 0; i < r->p.nas; i++)
        if (r->p.as[i].field) covered |= field_bits(&g.fields[r->p.as[i].field - 1]);
    if (covered != 0xFFFFFFFFu) fail("bits 0x%08x are undefined", ~covered);
    /* a field naming another argument needs that argument */
    for (i = 0; i < r->p.nas; i++) {
        const Field* f;
        int k, j;
        if (!r->p.as[i].field) continue;
        f = &g.fields[r->p.as[i].field - 1];
        for (k = 0; k < f->nsub; k++) {
            if (!f->sub[k].ref) continue;
            for (j = 0; j < r->p.nas && r->p.as[j].arg != f->sub[k].ref - 1; j++) {}
            if (j == r->p.nas) fail("%%%s needs %s", f->name, g.args[f->sub[k].ref - 1]);
            if (r->p.as[j].field) {
                const Field* o = &g.fields[r->p.as[j].field - 1];
                int m;
                for (m = 0; m < o->nsub; m++)
                    if (o->sub[m].ref) fail("%%%s needs %s, which needs another argument", f->name, o->name);
            }
        }
    }
    /* an argset's members are exactly the arguments given */
    if (r->p.argset) {
        const Argset* a = &g.argsets[r->p.argset - 1];
        if (a->nmember != r->p.nas) fail("&%s has %d members but %d arguments are given", a->name, a->nmember, r->p.nas);
        for (i = 0; i < a->nmember; i++) {
            int j;
            for (j = 0; j < r->p.nas && r->p.as[j].arg != a->member[i]; j++) {}
            if (j == r->p.nas) fail("&%s's %s is not given", a->name, g.args[a->member[i]]);
        }
    }
}

static int func_index(const char* name) {
    int k;
    for (k = 1; k < NFUNCS; k++)
        if (!strcmp(funcs[k].name, name)) return k;
    fail("unknown function %s", name);
    return 0;
}

static void add_sub(Field* f, int node) {
    const FNode* n = &fnodes[node];
    Sub* s;
    if (f->nsub == MAX_SUB) fail("more than %d subfields", MAX_SUB);
    s = &f->sub[f->nsub++];
    if (n->len < 1 || n->len > 32) fail("bad length %d", n->len);
    s->len = n->len;
    s->sign = n->sign;
    if (n->kind == F_FIELD)
        s->pos = n->pos;
    else if (n->kind == F_NAMED)
        s->ref = arg_index(n->name, 1) + 1;
    else
        fail("%s: a part of a field is bits or an argument", f->name);
}

/* a field of decodetree.c's as a64's: its parts, and its !function= from the list */
static int make_field(const char* name, int node) {
    Field* f;
    if (g.nfields == MAX_FIELDS) fail("more than %d fields", MAX_FIELDS);
    f = &g.fields[g.nfields];
    memset(f, 0, sizeof *f);
    copy_name(f->name, name);
    f->file = -1;
    if (fnodes[node].kind == F_FUNC) {
        f->func = func_index(fnodes[node].func);
        node = fnodes[node].base;
    }
    if (fnodes[node].kind == F_MULTI) {
        int k;
        for (k = 0; k < fnodes[node].nsubs; k++) add_sub(f, fnodes[node].subs[k]);
    } else
        add_sub(f, node);
    { /* rows share a field of the same name and parts */
        int k;
        for (k = 0; k < g.nfields; k++)
            if (!strcmp(g.fields[k].name, f->name) && g.fields[k].func == f->func && g.fields[k].nsub == f->nsub &&
                !memcmp(g.fields[k].sub, f->sub, sizeof(Sub) * (size_t)f->nsub))
                return k;
    }
    return g.nfields++;
}

/* the patterns, in the files' order, as rows */
static void load_rows(void) {
    int i, k;
    for (i = 0; i < nargs; i++)
        for (k = 0; k < args[i].nfields; k++) arg_index(args[i].fields[k], 1);
    for (i = 0; i < nnodes; i++) {
        const General* p;
        Row* r;
        if (nodes[i].kind != N_PAT) continue;
        p = &pats[nodes[i].pat];
        g.where = p->file;
        g.line = p->lineno;
        if (p->width != 32) fail("%s: an A64 pattern has 32 bits", p->name);
        if (g.nrows == MAX_ROWS) fail("more than %d rows", MAX_ROWS);
        r = &g.rows[g.nrows++];
        memset(r, 0, sizeof *r);
        copy_name(r->p.name, p->name);
        for (k = 0; k < g.nfiles && g.files[k] != p->file; k++) continue;
        r->p.file = k;
        r->p.line = p->lineno;
        r->node = i;
        r->name = name_index(p->name, 1);
        r->p.fixmask = (unsigned)p->fixedmask;
        r->p.fixval = (unsigned)p->fixedbits;
        r->p.fieldmask = (unsigned)p->fieldmask;
        r->p.ignmask = (unsigned)p->ignmask;
        for (k = 0; k < p->all.n; k++) {
            const FNode* n = &fnodes[p->all.node[k]];
            int arg = arg_index(p->all.name[k], 1);
            if (n->kind == F_CONST) {
                if (n->value < -0x7FFFFFFFLL || n->value > 0x7FFFFFFFLL) fail("bad value %lld", n->value);
                add_assign(&r->p, arg, 0, (int)n->value);
            } else if (n->kind == F_PARAM)
                fail("%s: a field of no bits", p->all.name[k]);
            else
                add_assign(&r->p, arg, make_field(p->all.name[k], p->all.node[k]) + 1, 0);
        }
        finish_row(r);
    }
    g.where = NULL;
    g.line = 0;
}

/* ---- the profile ---- */

static int feat_index(const char* name) {
    int i;
    for (i = 0; i < g.nfeats; i++)
        if (!strcmp(g.feats[i], name)) return i;
    fail("undeclared feature %s", name);
    return -1;
}

static void parse_cmp(Cmp* c, const char* text) {
    char lhs[A64_NAME], rhs[A64_NAME];
    const char* p = text;
    const char* amp;
    size_t n;
    int k;
    memset(c, 0, sizeof *c);
    for (n = 0; *p && (isalnum((unsigned char)*p) || *p == '_' || *p == '&'); p++) {
        if (n + 1 >= sizeof lhs) fail("bad comparison %s", text);
        lhs[n++] = *p;
    }
    lhs[n] = 0;
    for (k = 5; k >= 0; k--)
        if (!strncmp(p, op_text[k], strlen(op_text[k])) && (strlen(op_text[k]) == 2 || (p[1] != '='))) break;
    if (k < 0) fail("bad comparison %s", text);
    c->op = k;
    p += strlen(op_text[k]);
    if (strlen(p) >= sizeof rhs) fail("bad comparison %s", text);
    strcpy(rhs, p);
    amp = strchr(lhs, '&');
    if (amp) {
        if (!parse_int(amp + 1, &c->mask) || c->mask <= 0) fail("bad mask in %s", text);
        lhs[amp - lhs] = 0;
    }
    c->arg = arg_index(lhs, 0);
    if (c->arg < 0) fail("no argument %s", lhs);
    if (!parse_int(rhs, &c->value)) {
        int a = arg_index(rhs, 0);
        if (a < 0) fail("no argument or number %s", rhs);
        c->rhs = a + 1;
    }
}

static void parse_pred(Pred* pr, const char* text) {
    char buf[LINE_LEN];
    char* p;
    memset(pr, 0, sizeof *pr);
    if (!strncmp(text, "valid=bitmask(", 14)) {
        char* close;
        strcpy(buf, text + 14);
        close = strchr(buf, ')');
        if (!close || close[1]) fail("bad %s", text);
        *close = 0;
        pr->bitmask = 1;
        pr->n = 1;
        pr->c[0].arg = arg_index(buf, 0);
        if (pr->c[0].arg < 0) fail("no argument %s", buf);
        g.uses_bitmask = 1;
        return;
    }
    if (strlen(text) >= sizeof buf) fail("too long: %s", text);
    strcpy(buf, text);
    for (p = buf;;) {
        char* bar = strchr(p, '|');
        if (bar) *bar = 0;
        if (pr->n == MAX_OR) fail("more than %d alternatives", MAX_OR);
        parse_cmp(&pr->c[pr->n++], p);
        if (!bar) break;
        p = bar + 1;
    }
}

/* EL1 is not an architecture feature but the exception level an instruction needs: the encodings that are UNDEF at
   EL0 need it, and no target of user-space code has it (the profile's `*` leaves it out) */
static int is_feature_token(const char* t) {
    return !strncmp(t, "FEAT_", 5) || !strncmp(t, "v8.0", 4) || !strncmp(t, "EL1", 3);
}

static void parse_profile(const char* path) {
    FILE* f = fopen(path, "rb");
    char line[LINE_LEN];
    if (!f) fail("cannot read %s", path);
    g.where = path;
    g.line = 0;
    while (fgets(line, sizeof line, f)) {
        char* tok[MAX_TOKENS];
        char* hash = strchr(line, '#');
        int ntok, i;
        Name* nm;
        g.line++;
        if (hash) *hash = 0;
        line[strcspn(line, "\r\n")] = 0;
        ntok = split(line, tok);
        if (!ntok) continue;
        if (!strcmp(tok[0], "feature")) {
            for (i = 1; i < ntok; i++) {
                int k;
                if (!is_feature_token(tok[i])) fail("a feature is v8.0, FEAT_* or EL1: %s", tok[i]);
                for (k = 0; k < g.nfeats; k++)
                    if (!strcmp(g.feats[k], tok[i])) fail("%s declared twice", tok[i]);
                if (g.nfeats == MAX_FEATS) fail("more than %d features", MAX_FEATS);
                copy_name(g.feats[g.nfeats++], tok[i]);
            }
            continue;
        }
        if (!strcmp(tok[0], "target")) {
            Target* t;
            if (ntok < 3) fail("target NAME FEATURE...");
            if (g.ntargets == MAX_TARGETS) fail("more than %d targets", MAX_TARGETS);
            t = &g.targets[g.ntargets++];
            memset(t, 0, sizeof *t);
            if (!is_ident(tok[1])) fail("bad target name %s", tok[1]);
            copy_name(t->name, tok[1]);
            for (i = 2; i < ntok; i++) {
                if (!strcmp(tok[i], "*")) {
                    int k;
                    for (k = 0; k < g.nfeats; k++)
                        if (strcmp(g.feats[k], "EL1")) t->feats |= 1ull << k;
                } else
                    t->feats |= 1ull << feat_index(tok[i]);
            }
            continue;
        }
        i = name_index(tok[0], 0);
        if (i < 0) fail("%s is not a name of the .decode files", tok[0]);
        nm = &g.names[i];
        if (nm->profiled) fail("%s has two lines (the other at %d)", tok[0], nm->line);
        nm->profiled = 1;
        nm->line = g.line;
        if (ntok < 3) fail("NAME FLOW ... FEATURE");
        for (nm->flow = 1; nm->flow < FLOW_COUNT && strcmp(flow_names[nm->flow], tok[1]); nm->flow++) {}
        if (nm->flow == FLOW_COUNT) fail("unknown flow %s", tok[1]);
        for (i = 2; i < ntok; i++) {
            char* t = tok[i];
            if (!strcmp(t, "nop")) {
                nm->nop = 1;
            } else if (is_feature_token(t)) {
                Need* need;
                char* open = strchr(t, '(');
                if (nm->nneed == MAX_NFEAT) fail("more than %d features", MAX_NFEAT);
                need = &nm->need[nm->nneed++];
                memset(need, 0, sizeof *need);
                if (open) {
                    size_t n = strlen(open);
                    if (open[n - 1] != ')') fail("bad condition %s", t);
                    open[n - 1] = 0;
                    *open++ = 0;
                    need->has_cond = 1;
                    parse_pred(&need->cond, open);
                }
                need->feat = feat_index(t);
            } else {
                if (nm->npred == MAX_PRED) fail("more than %d predicates", MAX_PRED);
                parse_pred(&nm->pred[nm->npred++], t);
            }
        }
        for (i = 0; i < nm->nneed && nm->need[i].has_cond; i++) {}
        if (i == nm->nneed) fail("%s has no feature it always needs", nm->name);
    }
    fclose(f);
    g.line = 0;
    for (int i = 0; i < g.nnames; i++)
        if (!g.names[i].profiled) fail("%s has no line in the profile %s", g.names[i].name, path);
}

/* ---- rows: constants, expressions, bodies ---- */

static const Assign* row_assign(const Row* r, int arg) {
    int i;
    for (i = 0; i < r->p.nas; i++)
        if (r->p.as[i].arg == arg) return &r->p.as[i];
    return NULL;
}

static int apply_func(int func, int x) {
    switch (func) {
    case 1: return x + 1;
    case 2: return x + 2;
    case 3: return x ^ 2;
    case 4: return 8 - x;
    case 5: return 16 - x;
    case 6: return 32 - x;
    case 7: return 64 - x;
    case 8: return x * 4096;
    case 9: return x * 4;
    case 10: return x * 8;
    case 11: return x * 16;
    case 12: return (int)(((unsigned)x >> 3) << ((unsigned)x & 7));
    default: return x;
    }
}

static int row_arg_const(const Row* r, int arg, int* value);

/* a field's value when every bit it reads is fixed in the row */
static int field_const(const Row* r, const Field* f, int* value) {
    int i, v = 0;
    for (i = 0; i < f->nsub; i++) {
        const Sub* s = &f->sub[i];
        int part;
        unsigned m = s->len == 32 ? 0xFFFFFFFFu : (1u << s->len) - 1;
        if (s->ref) {
            if (!row_arg_const(r, s->ref - 1, &part)) return 0;
            part &= (int)m;
        } else {
            unsigned bits = m << s->pos;
            if ((r->p.fixmask & bits) != bits) return 0;
            part = (int)((r->p.fixval >> s->pos) & m);
            if (s->sign && (part >> (s->len - 1)) & 1) part -= (int)(1u << (s->len - 1)) * 2;
        }
        v = v * (1 << s->len) + part;
    }
    *value = apply_func(f->func, v);
    return 1;
}

static int row_arg_const(const Row* r, int arg, int* value) {
    const Assign* a = row_assign(r, arg);
    if (!a) fail("row %s (line %d) has no argument %s", r->p.name, r->p.line, g.args[arg]);
    if (!a->field) {
        *value = a->value;
        return 1;
    }
    return field_const(r, &g.fields[a->field - 1], value);
}

static void field_expr(const Field* f, char* buf, size_t size) {
    char inner[512], part[160];
    const char* t;
    size_t n = 0;
    int i, rest = 0;
    inner[0] = 0;
    for (i = 0; i < f->nsub; i++) rest += f->sub[i].len;
    for (i = 0; i < f->nsub; i++) {
        const Sub* s = &f->sub[i];
        unsigned m = s->len == 32 ? 0xFFFFFFFFu : (1u << s->len) - 1;
        rest -= s->len;
        if (s->ref)
            sprintf(part, "(a->%s & 0x%x)", g.args[s->ref - 1], m);
        else if (s->sign)
            sprintf(part, "(((int)((w >> %d) & 0x%xu) ^ 0x%x) - 0x%x)", s->pos, m, 1u << (s->len - 1),
                    1u << (s->len - 1));
        else
            sprintf(part, "(int)((w >> %d) & 0x%xu)", s->pos, m);
        if (strlen(inner) + strlen(part) + 32 >= sizeof inner) fail("the field %s is too long", f->name);
        if (inner[0]) strcat(inner, " + ");
        strcat(inner, part);
        if (rest) sprintf(inner + strlen(inner), " * %d", 1 << rest);
    }
    if (f->nsub > 1) {
        memmove(inner + 1, inner, strlen(inner) + 1);
        inner[0] = '(';
        strcat(inner, ")");
    }
    for (t = funcs[f->func].expr; *t; t++) {
        const char* add = *t == '@' ? inner : NULL;
        size_t k = add ? strlen(add) : 1;
        if (n + k + 1 >= size) fail("the field %s is too long", f->name);
        if (add)
            memcpy(buf + n, add, k);
        else
            buf[n] = *t;
        n += k;
    }
    buf[n] = 0;
}

static int cmp_eval(int lhs, int op, int rhs) {
    switch (op) {
    case OP_EQ: return lhs == rhs;
    case OP_NE: return lhs != rhs;
    case OP_LT: return lhs < rhs;
    case OP_LE: return lhs <= rhs;
    case OP_GT: return lhs > rhs;
    default: return lhs >= rhs;
    }
}

/* 1 when the predicate always holds in the row, 0 when it never does, -1 when it depends on the word; the
   alternatives that can't hold are left out of text */
static int pred_fold(const Row* r, const Pred* pr, char* text, size_t size) {
    int i, any = 0;
    text[0] = 0;
    if (pr->bitmask) {
        if (!row_assign(r, pr->c[0].arg))
            fail("row %s (line %d) has no argument %s", r->p.name, r->p.line, g.args[pr->c[0].arg]);
        snprintf(text, size, "snx_a64_bitmask_ok(a->%s)", g.args[pr->c[0].arg]);
        return -1;
    }
    for (i = 0; i < pr->n; i++) {
        const Cmp* c = &pr->c[i];
        int lhs, rhs = c->value, lc, rc = 1;
        char one[160], lt[64], rt[64];
        lc = row_arg_const(r, c->arg, &lhs);
        if (c->rhs) rc = row_arg_const(r, c->rhs - 1, &rhs);
        if (lc && c->mask) lhs &= c->mask;
        if (lc && rc) {
            if (cmp_eval(lhs, c->op, rhs)) return 1;
            continue;
        }
        if (c->mask)
            sprintf(lt, "(a->%s & %d)", g.args[c->arg], c->mask);
        else
            sprintf(lt, "a->%s", g.args[c->arg]);
        if (c->rhs)
            sprintf(rt, "a->%s", g.args[c->rhs - 1]);
        else
            sprintf(rt, "%d", c->value);
        sprintf(one, "%s %s %s", lt, op_text[c->op], rt);
        if (strlen(text) + strlen(one) + 8 >= size) fail("a predicate is too long");
        if (any++) strcat(text, " || ");
        strcat(text, one);
    }
    return any ? -1 : 0;
}

static void feat_macro(int feat, char* buf) {
    const char* s = g.feats[feat];
    char* p;
    if (!strncmp(s, "FEAT_", 5)) s += 5;
    sprintf(buf, "SNX_A64_FEAT_");
    p = buf + strlen(buf);
    for (; *s; s++) *p++ = *s == '.' ? '_' : (char)toupper((unsigned char)*s);
    *p = 0;
}

static void append(char** text, size_t* len, size_t* cap, const char* s) {
    size_t n = strlen(s);
    if (*len + n + 1 > *cap) {
        *cap = (*len + n + 1) * 2;
        *text = (char*)realloc(*text, *cap);
        if (!*text) fail("out of memory");
    }
    memcpy(*text + *len, s, n + 1);
    *len += n;
}

/* the row's body: its arguments, predicates and conditional features; a row that can never match is an error */
static void build_body(Row* r) {
    const Name* nm = &g.names[r->name];
    char* text = NULL;
    size_t len = 0, cap = 0;
    char line[1024], expr[768], cond[512];
    int i, pass, conds = 0;
    append(&text, &len, &cap, "");
    for (pass = 0; pass < 2; pass++) {
        for (i = 0; i < r->p.nas; i++) {
            const Assign* a = &r->p.as[i];
            const Field* f;
            int k, has_ref = 0;
            if (!a->field) {
                if (pass == 0) {
                    sprintf(line, "        a->%s = %d;\n", g.args[a->arg], a->value);
                    append(&text, &len, &cap, line);
                }
                continue;
            }
            f = &g.fields[a->field - 1];
            for (k = 0; k < f->nsub; k++) has_ref |= f->sub[k].ref != 0;
            if (has_ref != pass) continue;
            field_expr(f, expr, sizeof expr);
            sprintf(line, "        a->%s = %s;\n", g.args[a->arg], expr);
            append(&text, &len, &cap, line);
        }
    }
    for (i = 0; i < nm->npred; i++) {
        int v = pred_fold(r, &nm->pred[i], cond, sizeof cond);
        if (v == 0) fail("%s (line %d) can never match: its predicate %d never holds", nm->name, r->p.line, i + 1);
        if (v < 0) {
            sprintf(line, "        if (!(%s)) return 0;\n", cond);
            append(&text, &len, &cap, line);
        }
    }
    r->feat = 0;
    for (i = 0; i < nm->nneed; i++) {
        const Need* need = &nm->need[i];
        char macro[64];
        int v = need->has_cond ? pred_fold(r, &need->cond, cond, sizeof cond) : 1;
        if (v == 1) r->feat |= 1ull << need->feat;
        if (v >= 0) continue;
        feat_macro(need->feat, macro);
        sprintf(line, "        if (%s) insn->feature |= %s;\n", cond, macro);
        append(&text, &len, &cap, line);
        conds++;
    }
    append(&text, &len, &cap, conds ? "        return (insn->feature & ~features) == 0;\n" : "        return 1;\n");
    for (i = 0; i < g.nbodies; i++)
        if (!strcmp(g.bodies[i], text)) break;
    if (i == g.nbodies) {
        g.bodies = (char**)realloc(g.bodies, (size_t)(g.nbodies + 1) * sizeof *g.bodies);
        if (!g.bodies) fail("out of memory");
        g.bodies[g.nbodies++] = text;
    } else {
        free(text);
    }
    r->body = i;
}

/* two rows can match the same word unless a bit both fix differs */
static int overlap(const Row* a, const Row* b) {
    return ((a->p.fixval ^ b->p.fixval) & a->p.fixmask & b->p.fixmask) == 0;
}

/* the innermost group two nodes are both in */
static int common(int a, int b) {
    int x, y;
    for (x = a; x >= 0; x = nodes[x].parent)
        for (y = b; y >= 0; y = nodes[y].parent)
            if (x == y) return x;
    return 0;
}

static void check_overlaps(void) {
    int i, j;
    for (i = 0; i < g.nrows; i++)
        for (j = i + 1; j < g.nrows; j++) {
            const Row* a = &g.rows[i];
            const Row* b = &g.rows[j];
            if (!overlap(a, b)) continue;
            if (nodes[common(a->node, b->node)].kind == N_INC) continue;
            g.where = g.files[a->p.file];
            g.line = a->p.line;
            fail("%s overlaps %s (%s:%d), outside an overlapping group", a->p.name, b->p.name, g.files[b->p.file],
                 b->p.line);
        }
}

/* ---- the index ---- */

static int row_in(const Row* r, unsigned word, unsigned mask) {
    return ((r->p.fixval ^ word) & r->p.fixmask & mask) == 0;
}

static void push_list(int row) {
    if (g.nlist == MAX_LIST) fail("the bucket lists hold more than %d rows", MAX_LIST);
    g.list[g.nlist++] = (unsigned short)row;
}

static void build_index(void) {
    int* cand = g.cand;
    int b, i;
    for (b = 0; b < BUCKETS; b++) {
        unsigned top = (unsigned)b << 21;
        Bucket* bk = &g.buckets[b];
        int n = 0, best_lo = 0, best_bits = 0, best_max = 0;
        for (i = 0; i < g.nrows; i++)
            if (row_in(&g.rows[i], top, 0xFFE00000u)) cand[n++] = i;
        bk->count = n;
        if (n <= CROWDED) {
            bk->start = g.nlist;
            for (i = 0; i < n; i++) push_list(cand[i]);
            continue;
        }
        /* the run of bits below 21 that leaves the fewest rows in the fullest part */
        best_max = n + 1;
        {
            int lo, bits;
            for (bits = 1; bits <= MAX_SPLIT; bits++)
                for (lo = 0; lo + bits <= 21; lo++) {
                    int v, worst = 0;
                    for (v = 0; v < (1 << bits); v++) {
                        int c = 0;
                        for (i = 0; i < n; i++)
                            c += row_in(&g.rows[cand[i]], (unsigned)v << lo, ((1u << bits) - 1) << lo);
                        if (c > worst) worst = c;
                    }
                    if (worst < best_max) {
                        best_max = worst;
                        best_lo = lo;
                        best_bits = bits;
                    }
                }
        }
        bk->lo = best_lo;
        bk->bits = best_bits;
        bk->start = g.nsubs_out;
        if (g.nsubs_out + (1 << best_bits) > (int)(sizeof g.subs / sizeof g.subs[0])) fail("too many split parts");
        for (i = 0; i < (1 << best_bits); i++) {
            Bucket* s = &g.subs[g.nsubs_out++];
            int k;
            s->start = g.nlist;
            s->count = 0;
            for (k = 0; k < n; k++)
                if (row_in(&g.rows[cand[k]], (unsigned)i << best_lo, ((1u << best_bits) - 1) << best_lo)) {
                    push_list(cand[k]);
                    s->count++;
                }
        }
    }
}

/* ---- output ---- */

static unsigned long long fnv1a(const char* s, size_t n) {
    unsigned long long h = 0xcbf29ce484222325ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= (unsigned char)s[i];
        h *= 0x100000001b3ull;
    }
    return h;
}

static int cmp_args(const void* a, const void* b) {
    return strcmp(g.args[*(const int*)a], g.args[*(const int*)b]);
}

/* a path's file name, for the outputs, which don't depend on where the build is */
static const char* base_name(const char* path) {
    const char* p;
    const char* b = path;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

static void notice(const char* what) {
    int i;
    out("/* Generated by tools/decodetree (--a64): %s. Do not edit.\n   Derived from", what);
    for (i = 0; i < g.nfiles; i++) out(" %s", base_name(g.files[i]));
    out(" (QEMU's decodetree files, Copyright (c) 2023 Linaro, Ltd,\n   LGPL-2.1-or-later) and ScarletNX's"
        " profile of them (GPL-3.0-or-later). */\n");
}

static void write_header(const char* path) {
    int order[MAX_ARGS];
    int i;
    char macro[64];
    notice("the A64 decoder's types");
    out("#ifndef SNX_A64_DECODE_H\n#define SNX_A64_DECODE_H\n\n#include <stdint.h>\n\n");
    out("/* the features a row may need, FEAT_* as the Arm ARM names them; v8.0 is the base architecture. EL1 is the\n"
        "   exception level the encodings UNDEF at EL0 need, which no target of user-space code has */\n");
    for (i = 0; i < g.nfeats; i++) {
        feat_macro(i, macro);
        out("#define %s (1ull << %d)\n", macro, i);
    }
    out("\n/* the targets: a decode takes the features of one */\n");
    for (i = 0; i < g.ntargets; i++) {
        const Target* t = &g.targets[i];
        char up[A64_NAME];
        int k;
        for (k = 0; t->name[k]; k++) up[k] = (char)toupper((unsigned char)t->name[k]);
        up[k] = 0;
        out("#define SNX_A64_TARGET_%s 0x%llxull\n", up, t->feats);
    }
    out("\n/* the names of the .decode files, in order: X(name) */\n#define SNX_A64_OPS(X)");
    for (i = 0; i < g.nnames; i++) out(" \\\n    X(%s)", g.names[i].name);
    out("\n\ntypedef enum {\n    SNX_A64_OP_UNDEF,\n#define SNX_A64_OP_ENUM(name) SNX_A64_OP_##name,\n"
        "    SNX_A64_OPS(SNX_A64_OP_ENUM)\n#undef SNX_A64_OP_ENUM\n    SNX_A64_OP_COUNT\n} SnxA64Op;\n\n");
    out("/* what an instruction does to the flow: UNDEF when no row matches; BC is a conditional branch; EXC an\n"
        "   exception other than SVC and BRK (HVC, SMC, HLT) */\ntypedef enum {\n");
    for (i = 0; i < FLOW_COUNT; i++) out("    SNX_A64_FLOW_%s,\n", flow_names[i]);
    out("} SnxA64Flow;\n\n/* every argument of every row; those a row doesn't give are 0 */\ntypedef struct {\n");
    for (i = 0; i < g.nargs; i++) order[i] = i;
    qsort(order, (size_t)g.nargs, sizeof order[0], cmp_args);
    for (i = 0; i < g.nargs; i++) out("    int %s;\n", g.args[order[i]]);
    out("} SnxA64Args;\n\n/* a decoded word: feature is what this instance needs */\ntypedef struct {\n"
        "    SnxA64Op op;\n    SnxA64Flow flow;\n    int nop;\n    uint64_t feature;\n    SnxA64Args args;\n"
        "} SnxA64Insn;\n\n#endif\n");
    flush(path);
}

/* each row's argument fields as masks of the word's bits (a field of several parts is one mask), for tests that
   enumerate a field's values; compiled only where SNX_A64_DECODE_FIELDS is defined */
static void write_fields(void) {
    int first[MAX_ROWS], count[MAX_ROWS];
    int i, total = 0;
    out("#ifdef SNX_A64_DECODE_FIELDS\n/* each row's argument fields: the masks of snx_a64_field_masks[first...] */\n"
        "typedef struct {\n    uint16_t first, count;\n} SnxA64RowFields;\n\n"
        "static const uint32_t snx_a64_field_masks[] = {");
    for (i = 0; i < g.nrows; i++) {
        const Row* r = &g.rows[i];
        unsigned masks[MAX_ASSIGN];
        int n = 0, k, j;
        for (k = 0; k < r->p.nas; k++) {
            unsigned m;
            if (!r->p.as[k].field) continue;
            m = field_bits(&g.fields[r->p.as[k].field - 1]) & ~r->p.fixmask;
            for (j = 0; j < n && masks[j] != m; j++) {}
            if (m && j == n) masks[n++] = m;
        }
        first[i] = total;
        count[i] = n;
        for (j = 0; j < n; j++, total++) out("%s0x%08xu,", total % 8 == 0 ? "\n    " : " ", masks[j]);
    }
    if (!total) out("0");
    out("\n};\n\nstatic const SnxA64RowFields snx_a64_row_fields[%d] = {", g.nrows);
    for (i = 0; i < g.nrows; i++)
        out("%s{%d, %d},", i % 8 == 0 ? "\n    " : " ", first[i], count[i]);
    out("\n};\n#endif\n\n");
}

static void write_decoder(const char* path) {
    unsigned long long hash;
    int i;
    notice("the A64 decoder, included by one C file after a64_decode.h");
    out("#include <stddef.h>\n\n");
    out("typedef struct {\n    uint32_t mask, value;\n    uint64_t feature;\n    uint16_t op, body;\n"
        "} SnxA64Row;\n\n");
    out("typedef struct {\n    uint16_t start;\n    uint8_t count, lo, bits;\n} SnxA64Bucket;\n\n");
    out("static const char* const snx_a64_op_names[SNX_A64_OP_COUNT] = {\n    \"UNDEF\",\n"
        "#define SNX_A64_OP_NAME(name) #name,\n    SNX_A64_OPS(SNX_A64_OP_NAME)\n#undef SNX_A64_OP_NAME\n};\n\n");
    out("static const uint8_t snx_a64_op_flow[SNX_A64_OP_COUNT] = {\n    SNX_A64_FLOW_UNDEF,\n");
    for (i = 0; i < g.nnames; i++)
        out("    SNX_A64_FLOW_%s, /* %s */\n", flow_names[g.names[i].flow], g.names[i].name);
    out("};\n\nstatic const uint8_t snx_a64_op_nop[SNX_A64_OP_COUNT] = {\n    0,");
    for (i = 0; i < g.nnames; i++) out("%s%d,", i % 32 == 31 ? "\n    " : " ", g.names[i].nop);
    out("\n};\n\n/* the rows, in the .decode files' order: mask, value, the features always needed, op, body */\n"
        "static const SnxA64Row snx_a64_rows[%d] = {\n", g.nrows);
    for (i = 0; i < g.nrows; i++) {
        const Row* r = &g.rows[i];
        out("    {0x%08xu, 0x%08xu, 0x%llxull, SNX_A64_OP_%s, %d}, /* %s:%d */\n", r->p.fixmask, r->p.fixval, r->feat,
            g.names[r->name].name, r->body, base_name(g.files[r->p.file]), r->p.line);
    }
    out("};\n\n");
    write_fields();
    out("/* the rows each bucket can match, in order */\nstatic const uint16_t snx_a64_list[%d] = {", g.nlist);
    for (i = 0; i < g.nlist; i++) out("%s%d,", i % 16 == 0 ? "\n    " : " ", g.list[i]);
    out("\n};\n\n/* by bits [31:21]: a run of the list, or (bits != 0) the parts of snx_a64_split picked by the word's\n"
        "   bits [lo + bits - 1:lo] */\nstatic const SnxA64Bucket snx_a64_buckets[%d] = {", BUCKETS);
    for (i = 0; i < BUCKETS; i++) {
        const Bucket* b = &g.buckets[i];
        out("%s{%d, %d, %d, %d},", i % 6 == 0 ? "\n    " : " ", b->start, b->bits ? 0 : b->count, b->lo, b->bits);
    }
    out("\n};\n\nstatic const SnxA64Bucket snx_a64_split[%d] = {", g.nsubs_out ? g.nsubs_out : 1);
    for (i = 0; i < g.nsubs_out; i++)
        out("%s{%d, %d, 0, 0},", i % 6 == 0 ? "\n    " : " ", g.subs[i].start, g.subs[i].count);
    if (!g.nsubs_out) out("{0, 0, 0, 0}");
    out("\n};\n\n");
    if (g.uses_bitmask)
        out("/* a logical immediate's N:immr:imms names a mask: the element size is the highest set bit of N:NOT(imms),\n"
            "   at least 2 bits, and imms must not be all ones within it */\n"
            "static int snx_a64_bitmask_ok(int dbm) {\n"
            "    unsigned imms = (unsigned)dbm & 63, x = (((unsigned)dbm >> 6) & 64) | (~imms & 63), levels;\n"
            "    int len = 6;\n    while (len >= 0 && !((x >> len) & 1)) len--;\n    if (len < 1) return 0;\n"
            "    levels = (1u << len) - 1;\n    return (imms & levels) != levels;\n}\n\n");
    out("/* a row's arguments, predicates and conditional features; 0 when the row doesn't match after all */\n"
        "static int snx_a64_body(unsigned body, uint32_t w, uint64_t features, SnxA64Insn* insn) {\n"
        "    SnxA64Args* a = &insn->args;\n    (void)w;\n    (void)features;\n    switch (body) {\n");
    for (i = 0; i < g.nbodies; i++) out("    case %d:\n%s", i, g.bodies[i]);
    out("    default:\n        return 0;\n    }\n}\n\n");
    out("/* decodes word for a target with these features (SNX_A64_FEAT_*, or a SNX_A64_TARGET_*): the op, or\n"
        "   SNX_A64_OP_UNDEF when no row matches, with insn filled in either way */\n"
        "static SnxA64Op snx_a64_decode(uint32_t word, uint64_t features, SnxA64Insn* insn) {\n"
        "    static const SnxA64Insn undef = {SNX_A64_OP_UNDEF, SNX_A64_FLOW_UNDEF, 0, 0, {0}};\n"
        "    const SnxA64Bucket* b = &snx_a64_buckets[word >> 21];\n    unsigned start = b->start, count = b->count, i;\n"
        "    if (b->bits) {\n        b = &snx_a64_split[start + ((word >> b->lo) & ((1u << b->bits) - 1))];\n"
        "        start = b->start;\n        count = b->count;\n    }\n"
        "    for (i = 0; i < count; i++) {\n        const SnxA64Row* r = &snx_a64_rows[snx_a64_list[start + i]];\n"
        "        if ((word & r->mask) != r->value || (r->feature & ~features)) continue;\n"
        "        *insn = undef;\n        insn->feature = r->feature;\n"
        "        if (!snx_a64_body(r->body, word, features, insn)) continue;\n"
        "        insn->op = (SnxA64Op)r->op;\n        insn->flow = (SnxA64Flow)snx_a64_op_flow[r->op];\n"
        "        insn->nop = snx_a64_op_nop[r->op];\n        return insn->op;\n    }\n"
        "    *insn = undef;\n    return SNX_A64_OP_UNDEF;\n}\n\n");
    out("static inline const char* snx_a64_op_name(SnxA64Op op) {\n"
        "    return (unsigned)op < SNX_A64_OP_COUNT ? snx_a64_op_names[op] : \"?\";\n}\n\n");
    hash = fnv1a(g.obuf, g.olen);
    out("/* FNV-1a of everything above: names this decoder in caches of code it decoded */\n"
        "#define SNX_A64_DECODE_HASH 0x%016llxull\n",
        hash);
    flush(path);
}

static void stats(void) {
    int i, t, largest = 0, largest_at = 0, split = 0;
    fprintf(stderr, "decodetree: %d names, %d rows, %d bodies, %d list entries\n", g.nnames, g.nrows, g.nbodies, g.nlist);
    for (t = 0; t < g.ntargets; t++) {
        int n = 0;
        for (i = 0; i < g.nrows; i++) n += (g.rows[i].feat & ~g.targets[t].feats) == 0;
        fprintf(stderr, "decodetree: target %s: %d rows can match\n", g.targets[t].name, n);
    }
    for (i = 0; i < BUCKETS; i++) {
        if (g.buckets[i].count > largest) {
            largest = g.buckets[i].count;
            largest_at = i;
        }
        split += g.buckets[i].bits != 0;
    }
    fprintf(stderr, "decodetree: largest bucket 0x%03x: %d rows; %d buckets split", largest_at, largest, split);
    largest = 0;
    for (i = 0; i < g.nsubs_out; i++)
        if (g.subs[i].count > largest) largest = g.subs[i].count;
    fprintf(stderr, ", largest part %d rows\n", largest);
}

int a64_generate(const char* profile, const char* header, const char* decoder, int stats_, const char* const* files,
                 int nfiles) {
    int i;
    if (nfiles > MAX_FILES) fail("more than %d .decode files", MAX_FILES);
    for (i = 0; i < nfiles; i++) g.files[i] = files[i];
    g.nfiles = nfiles;
    g.stats = stats_;
    load_rows();
    parse_profile(profile);
    for (i = 0; i < g.nrows; i++) {
        g.where = g.files[g.rows[i].p.file];
        g.line = g.rows[i].p.line;
        build_body(&g.rows[i]);
    }
    g.where = NULL;
    g.line = 0;
    check_overlaps();
    build_index();
    write_header(header);
    write_decoder(decoder);
    if (g.stats) stats();
    return 0;
}
