/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --isel: the x86-64 back end's instruction selection, derived from the SDM. Each general-purpose
   instruction form of the pages sailgen --sdm cut out of the manual (an opcode table's row valid in 64-bit mode, of
   register, register-or-memory used as a register, and immediate operands) is evaluated from its page's Operation and
   Flags Affected sections to terms (term.c): what it writes to each register and flag, as a function of what it reads.
   The back end's primitives (x64.ops) are evaluated the same way, and a form is selected for a primitive where some
   binding of the primitive's operands to the form's makes the two the same terms: its result in the register the
   form writes, and each of Arm's flags the primitive gives one of Intel's (or its negation). Intel's condition codes
   are the predicates the Jcc, SETcc and CMOVcc tables give on the flags, matched the same way with Arm's conditions on
   each selected flag-setting form. What is written (x64_isel.inc) is C the back end compiles with: an encoder per
   selected form, from its opcode column and operand encoding, and the tables that say how the primitive's operands
   go into it. README.md, "Instruction selection", says what is derived and what is assumed */
#include "isel.h"
#include "sailgen.h"
#include "term.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char* dupn(const char* s, size_t n) {
    char* d = (char*)sg_alloc(n + 1);
    memcpy(d, s, n);
    return d;
}
static char* dups(const char* s) { return dupn(s, strlen(s)); }
static const char* ws(const char* s) {
    while (*s == ' ' || *s == '\t') ++s;
    return s;
}
static int starts(const char* s, const char* p) { return !strncmp(s, p, strlen(p)); }

/* ---- the pages ---- */

typedef struct {
    char* c[7];
} PRow;

typedef struct {
    char* open;
    char* op[5];
    int nop; /* the entries after the name: a tuple type's column and the operands' */
} PEnc;

typedef struct {
    char* title;
    char* name; /* before the dash */
    PRow* rows;
    unsigned nrows;
    PEnc* enc;
    unsigned nenc;
    char* operation;
    char* flags;
} Page;

static struct {
    Page* pages;
    unsigned npages;
} g_sdm;

/* "a | b | c": the columns, trimmed ("" past the last) */
static void split_cols(const char* line, char** out, int max) {
    int n;
    const char* p = line;
    for (n = 0; n < max; ++n) out[n] = (char*)"";
    n = 0;
    while (p && n < max) {
        const char* bar = strstr(p, " | ");
        size_t len = bar ? (size_t)(bar - p) : strlen(p);
        const char* s = ws(p);
        len -= (size_t)(s - p) < len ? (size_t)(s - p) : len;
        while (len && s[len - 1] == ' ') --len;
        out[n++] = dupn(s, len);
        p = bar ? bar + 3 : NULL;
    }
}

static void read_pages(const char* path) {
    size_t size, i = 0;
    char* text = sg_read(path, &size);
    Page* cur = NULL;
    int sec = -1;
    size_t op_len = 0, fl_len = 0;
    while (i < size) {
        size_t s = i;
        char* line;
        while (i < size && text[i] != '\n') ++i;
        line = dupn(text + s, i - s);
        ++i;
        if (line[0] == '#' && line[1] == ' ' && sec != 2) {
            char* t;
            char* dash;
            g_sdm.pages = (Page*)realloc(g_sdm.pages, (g_sdm.npages + 1) * sizeof *g_sdm.pages);
            if (!g_sdm.pages) exit(1);
            cur = &g_sdm.pages[g_sdm.npages++];
            memset(cur, 0, sizeof *cur);
            t = strstr(line, " -- ");
            cur->title = dupn(line + 2, t ? (size_t)(t - line - 2) : strlen(line + 2));
            dash = strstr(cur->title, "\xE2\x80\x94");
            cur->name = dupn(cur->title, dash ? (size_t)(dash - cur->title) : strlen(cur->title));
            cur->operation = dups("");
            cur->flags = dups("");
            op_len = fl_len = 0;
            sec = -1;
            free(line);
            continue;
        }
        if (!cur) {
            free(line);
            continue;
        }
        if (line[0] == '@') {
            sec = !strcmp(line, "@opcodes") ? 0 : !strcmp(line, "@encoding") ? 1 : !strcmp(line, "@operation") ? 2
                  : !strcmp(line, "@flags")                                     ? 3
                                                                                : -1;
            free(line);
            continue;
        }
        if (sec == 0 && line[0]) {
            PRow r;
            split_cols(line, r.c, 7);
            cur->rows = (PRow*)realloc(cur->rows, (cur->nrows + 1) * sizeof *cur->rows);
            if (!cur->rows) exit(1);
            cur->rows[cur->nrows++] = r;
        } else if (sec == 1 && line[0]) {
            char* c[6];
            PEnc e;
            int k;
            const char* q;
            split_cols(line, c, 6);
            e.open = c[0];
            for (k = 0; k < 5; ++k) e.op[k] = c[k + 1];
            e.nop = 0;
            for (q = line; (q = strstr(q, " | ")) != NULL; q += 3) ++e.nop;
            cur->enc = (PEnc*)realloc(cur->enc, (cur->nenc + 1) * sizeof *cur->enc);
            if (!cur->enc) exit(1);
            cur->enc[cur->nenc++] = e;
        } else if (sec == 2 || sec == 3) {
            char** dst = sec == 2 ? &cur->operation : &cur->flags;
            size_t* len = sec == 2 ? &op_len : &fl_len;
            size_t l = strlen(line);
            *dst = (char*)realloc(*dst, *len + l + 2);
            if (!*dst) exit(1);
            memcpy(*dst + *len, line, l);
            (*dst)[*len + l] = sec == 2 ? '\n' : ' ';
            (*dst)[*len + l + 1] = 0;
            *len += l + 1;
        }
        free(line);
    }
    free(text);
}

/* ---- forms ---- */

enum { OK_NONE, OK_REG, OK_IMM, OK_FIXED, OK_ONE };
enum { ROLE_NONE, ROLE_MODRM_REG, ROLE_MODRM_RM, ROLE_VVVV, ROLE_IMM, ROLE_OPCODE_REG, ROLE_IMPLICIT };

typedef struct {
    int kind;
    int xmm;     /* an XMM register (OK_REG: xmm1, xmm2/m128...) */
    int size;    /* bits */
    int fixed;   /* OK_FIXED: the register's number (0 RAX, 1 RCX, 2 RDX...) */
    int role;    /* how it is encoded */
    int read, write;
} FOp;

typedef struct {
    const Page* page;
    const PRow* row;
    char mnem[24];
    char text[64]; /* the instruction column */
    int nops;
    FOp op[4];
    int opsize;
    char feature[24]; /* the CPUID flag, or "" */
    int xmm;          /* an operand is an XMM register: an SSE form, which no primitive is matched with; its encoder
                         is emitted where an 'enc' line of the primitives' file asks for it */
    /* the encoding */
    int vex;          /* 0: legacy */
    int vex_l, vex_pp, vex_map, vex_w;
    unsigned char prefix[4];
    int nprefix;
    int rex_w;
    unsigned char opc[4];
    int nopc;
    int modrm;       /* -1: none; 8: /r; 0-7: /digit */
    int plus_r;      /* the opcode's low three bits are a register (B8+rd) */
    int imm_bytes;
    int cc;          /* -1, or the condition code in the opcode's low nibble (Jcc, SETcc, CMOVcc) */
    unsigned cond_pred; /* that condition's predicate on the flags (cc forms) */
    /* the evaluation */
    int evaluated, unreadable;
    const char* why;
} Form;

static Form* g_forms;
static unsigned g_nforms;

static int parse_hex2(const char* s, unsigned char* out) {
    int v = 0, k;
    for (k = 0; k < 2; ++k) {
        int c = s[k], d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        else if (c >= 'a' && c <= 'f' && k == 1 && s[0] == '0') d = c - 'a' + 10; /* PMOVSX's '66 0f 38' (S-10) */
        else return 0;
        v = v * 16 + d;
    }
    *out = (unsigned char)v;
    return 1;
}

/* "REX.W + 81 /0 id", "F3 REX.W 0F BD /r", "B8+ rd io", "VEX.LZ.0F38.W1 F2 /r": 0 when it is not one read here */
static int parse_opcode(Form* f, const char* s0) {
    char buf[96];
    char* tok[24];
    int n = 0, i;
    size_t len = strlen(s0);
    if (len >= sizeof buf) return 0;
    memcpy(buf, s0, len + 1);
    /* '66 0F E0, /r' (PAVGB's row, S-9): a comma among the bytes */
    for (i = 0; buf[i]; ++i)
        if (buf[i] == ',') buf[i] = ' ';
    /* 'VEX.LZ. 0F38.W1': pdftotext's space inside the prefix */
    for (i = 0; buf[i]; ++i)
        if (buf[i] == '.' && buf[i + 1] == ' ') memmove(buf + i + 1, buf + i + 2, strlen(buf + i + 2) + 1);
    {
        char* p = buf;
        while (*p && n < 24) {
            while (*p == ' ') ++p;
            if (!*p) break;
            tok[n++] = p;
            while (*p && *p != ' ') ++p;
            if (*p) *p++ = 0;
        }
    }
    f->modrm = -1;
    f->plus_r = 0;
    f->imm_bytes = 0;
    f->nopc = f->nprefix = 0;
    f->rex_w = 0;
    f->vex = 0;
    for (i = 0; i < n; ++i) {
        char* t = tok[i];
        unsigned char b;
        if (!strcmp(t, "+") || !strcmp(t, "NP")) continue;
        if (!strcmp(t, "NFx")) return 0;
        if (!strcmp(t, "REX.W")) {
            f->rex_w = 1;
            continue;
        }
        if (!strcmp(t, "REX")) continue;
        if (starts(t, "VEX.")) {
            char* q = t + 4;
            f->vex = 1;
            f->vex_l = 0;
            f->vex_pp = 0;
            f->vex_map = 0;
            f->vex_w = 0;
            while (*q) {
                char* dot = strchr(q, '.');
                size_t l = dot ? (size_t)(dot - q) : strlen(q);
                if ((l == 2 && !strncmp(q, "LZ", 2)) || (l == 2 && !strncmp(q, "L0", 2)) || (l == 3 && !strncmp(q, "128", 3)) ||
                    (l == 3 && !strncmp(q, "LIG", 3)))
                    f->vex_l = 0;
                else if ((l == 2 && !strncmp(q, "L1", 2)) || (l == 3 && !strncmp(q, "256", 3))) f->vex_l = 1;
                else if (l == 2 && !strncmp(q, "66", 2)) f->vex_pp = 1;
                else if (l == 2 && !strncmp(q, "F3", 2)) f->vex_pp = 2;
                else if (l == 2 && !strncmp(q, "F2", 2)) f->vex_pp = 3;
                else if (l == 2 && !strncmp(q, "0F", 2)) f->vex_map = 1;
                else if (l == 4 && !strncmp(q, "0F38", 4)) f->vex_map = 2;
                else if (l == 4 && !strncmp(q, "0F3A", 4)) f->vex_map = 3;
                else if (l == 2 && !strncmp(q, "W0", 2)) f->vex_w = 0;
                else if (l == 2 && !strncmp(q, "W1", 2)) f->vex_w = 1;
                else if (l == 3 && !strncmp(q, "WIG", 3)) f->vex_w = 0;
                else return 0;
                q += l;
                if (*q == '.') ++q;
            }
            if (!f->vex_map) return 0;
            continue;
        }
        if (t[0] == '/' && t[1] == 'r' && !t[2]) {
            f->modrm = 8;
            continue;
        }
        if (t[0] == '/' && t[1] >= '0' && t[1] <= '7' && !t[2]) {
            f->modrm = t[1] - '0';
            continue;
        }
        if (!strcmp(t, "ib") || !strcmp(t, "iw") || !strcmp(t, "id") || !strcmp(t, "io")) {
            f->imm_bytes = t[1] == 'b' ? 1 : t[1] == 'w' ? 2 : t[1] == 'd' ? 4 : 8;
            continue;
        }
        if (!strcmp(t, "rb") || !strcmp(t, "rw") || !strcmp(t, "rd") || !strcmp(t, "ro")) {
            f->plus_r = 1;
            continue;
        }
        if (strlen(t) >= 2 && parse_hex2(t, &b)) {
            const char* rest = t + 2;
            if (*rest == '+') {
                f->plus_r = 1;
                ++rest;
                if (*rest && strcmp(rest, "rb") && strcmp(rest, "rw") && strcmp(rest, "rd") && strcmp(rest, "ro"))
                    return 0;
            } else if (*rest) {
                /* '55/r' (pdftotext's lost space) */
                if (!strcmp(rest, "/r")) f->modrm = 8;
                else
                    return 0;
            }
            /* legacy prefixes before the opcode: 66, F2, F3 (mandatory ones) */
            if (!f->nopc && !f->vex && (b == 0x66 || b == 0xF2 || b == 0xF3) && i + 1 < n) {
                if (f->nprefix == 4) return 0;
                f->prefix[f->nprefix++] = b;
                continue;
            }
            if (f->nopc == 4) return 0;
            f->opc[f->nopc++] = b;
            continue;
        }
        return 0; /* cb, cd (branches), m64... */
    }
    return f->nopc > 0;
}

static int operand_kind(const char* s, FOp* o) {
    static const char* const fixed64[] = {"RAX", "RCX", "RDX", "RBX", NULL};
    static const char* const fixed32[] = {"EAX", "ECX", "EDX", "EBX", NULL};
    static const char* const fixed16[] = {"AX", "CX", "DX", "BX", NULL};
    static const char* const fixed8[] = {"AL", "CL", "DL", "BL", NULL};
    int k, n;
    memset(o, 0, sizeof *o);
    for (k = 0; fixed64[k]; ++k) {
        if (!strcmp(s, fixed64[k])) return o->kind = OK_FIXED, o->size = 64, o->fixed = k, 1;
        if (!strcmp(s, fixed32[k])) return o->kind = OK_FIXED, o->size = 32, o->fixed = k, 1;
        if (!strcmp(s, fixed16[k])) return o->kind = OK_FIXED, o->size = 16, o->fixed = k, 1;
        if (!strcmp(s, fixed8[k])) return o->kind = OK_FIXED, o->size = 8, o->fixed = k, 1;
    }
    if (!strcmp(s, "1")) return o->kind = OK_ONE, o->size = 8, 1;
    /* xmm, xmm1-xmm3 and their memory forms (xmm2/m128, xmm1/m64...), and m128, m64, m32 alone */
    if (starts(s, "xmm")) {
        const char* q = s + 3;
        if (*q >= '0' && *q <= '9') ++q;
        if (!*q) return o->kind = OK_REG, o->xmm = 1, o->size = 128, 1;
        if (*q == '/' && q[1] == 'm' && (n = atoi(q + 2)) >= 8 && n <= 128)
            return o->kind = OK_REG, o->xmm = 1, o->size = 128, 1;
        return 0;
    }
    /* the general-purpose register of an SSE form (MOVMSKPS reg, PEXTRB reg/m8, PINSRB xmm1, r32/m8): 32 bits */
    if (!strcmp(s, "reg") || starts(s, "reg/m") || starts(s, "r32/m")) return o->kind = OK_REG, o->size = 32, 1;
    /* PBLENDVB's and BLENDVPS's implicit mask */
    if (!strcmp(s, "<XMM0>")) return o->kind = OK_FIXED, o->xmm = 1, o->size = 128, o->fixed = 0, 1;
    if (s[0] == 'm' && (!strcmp(s + 1, "128") || !strcmp(s + 1, "64") || !strcmp(s + 1, "32")))
        return o->kind = OK_REG, o->xmm = 1, o->size = atoi(s + 1), 1;
    if (starts(s, "r/m")) {
        n = atoi(s + 3);
        if (n == 8 || n == 16 || n == 32 || n == 64) return o->kind = OK_REG, o->size = n, 1;
        return 0;
    }
    if (s[0] == 'r' && s[1] >= '0' && s[1] <= '9') {
        char* end;
        n = (int)strtol(s + 1, &end, 10);
        if ((n == 8 || n == 16 || n == 32 || n == 64) && (!*end || ((*end == 'a' || *end == 'b') && !end[1])))
            return o->kind = OK_REG, o->size = n, 1;
        return 0;
    }
    if (starts(s, "imm")) {
        n = atoi(s + 3);
        if (n == 8 || n == 16 || n == 32 || n == 64) return o->kind = OK_IMM, o->size = n, 1;
    }
    return 0;
}

static int role_of(const char* s, int* read, int* write) {
    const char* paren = strchr(s, '(');
    *read = *write = 0;
    if (paren) {
        *read = strchr(paren, 'r') != NULL;
        *write = strchr(paren, 'w') != NULL;
    }
    if (starts(s, "ModRM:reg")) return ROLE_MODRM_REG;
    if (starts(s, "ModRM:r/m")) return ROLE_MODRM_RM;
    if (starts(s, "VEX.vvvv")) return ROLE_VVVV;
    if (starts(s, "imm8") || starts(s, "imm16") || starts(s, "iw") || starts(s, "ib") || starts(s, "id") ||
        starts(s, "imm32") || starts(s, "imm64") || starts(s, "imm")) {
        *read = 1;
        return ROLE_IMM;
    }
    if (starts(s, "opcode") || starts(s, "Opcode")) return ROLE_OPCODE_REG;
    return ROLE_IMPLICIT;
}

/* the forms of a page's rows that are read here */
static void forms_of(const Page* p) {
    unsigned r;
    for (r = 0; r < p->nrows; ++r) {
        const PRow* row = &p->rows[r];
        Form f;
        char ins[96];
        char* ops[5];
        int n = 0, k;
        const PEnc* enc = NULL;
        memset(&f, 0, sizeof f);
        f.page = p;
        f.row = row;
        f.cc = -1;
        /* BSWAP r32's 'Valid*' (a footnote's mark), CMOVG r64's 'V/N.E.' (erratum E-7) */
        if (strcmp(row->c[3], "Valid") && strcmp(row->c[3], "Valid*") && strcmp(row->c[3], "V/N.E.")) continue;
        if (strlen(row->c[1]) >= sizeof ins) continue;
        strcpy(ins, row->c[1]);
        /* the mnemonic, then the operands, by commas */
        {
            char* sp = strchr(ins, ' ');
            char* q;
            size_t ml = sp ? (size_t)(sp - ins) : strlen(ins);
            if (ml >= sizeof f.mnem) continue;
            memcpy(f.mnem, ins, ml);
            f.mnem[ml] = 0;
            q = sp ? sp + 1 : NULL;
            while (q && n < 5) {
                char* comma = strchr(q, ',');
                if (comma) *comma = 0;
                ops[n++] = (char*)ws(q);
                {
                    size_t l = strlen(ops[n - 1]);
                    while (l && ops[n - 1][l - 1] == ' ') ops[n - 1][--l] = 0;
                }
                q = comma ? comma + 1 : NULL;
            }
        }
        if (n > 4) continue;
        snprintf(f.text, sizeof f.text, "%s", row->c[1]);
        f.nops = n;
        for (k = 0; k < n; ++k)
            if (!operand_kind(ops[k], &f.op[k])) break;
        if (k < n) continue;
        snprintf(f.feature, sizeof f.feature, "%s", !strcmp(p->name, "POPCNT") ? "POPCNT" : row->c[5]);
        for (k = 0; k < n; ++k) f.xmm |= f.op[k].xmm;
        if (!parse_opcode(&f, row->c[0])) continue;
        for (k = 0; k < (int)p->nenc; ++k)
            if (!strcmp(p->enc[k].open, row->c[2])) enc = &p->enc[k];
        if (!enc && n && !f.xmm) continue;
        for (k = 0; k < n && !enc; ++k) {
            /* an SSE form whose page's encoding table the extraction lost (MOVDQU's): the operand that may be
               memory is ModRM.rm, an immediate the immediate, <XMM0> implicit; of the registers, with /digit the
               one is r/m, else the first is ModRM.reg and the other r/m. Only an 'enc' line takes such a form */
            int memory = strstr(ops[k], "/m") || (ops[k][0] == 'm' && ops[k][1] >= '0' && ops[k][1] <= '9');
            int regs_before = 0, j;
            for (j = 0; j < k; ++j) regs_before += f.op[j].kind == OK_REG;
            f.op[k].read = 1;
            if (f.op[k].kind == OK_IMM) f.op[k].role = ROLE_IMM;
            else if (f.op[k].kind != OK_REG) f.op[k].role = ROLE_IMPLICIT;
            else if (memory || (f.modrm >= 0 && f.modrm < 8)) f.op[k].role = ROLE_MODRM_RM;
            else {
                int other_memory = 0;
                for (j = 0; j < n; ++j)
                    if (j != k && (strstr(ops[j], "/m") || (ops[j][0] == 'm' && ops[j][1] >= '0' && ops[j][1] <= '9')))
                        other_memory = 1;
                f.op[k].role = other_memory || !regs_before ? ROLE_MODRM_REG : ROLE_MODRM_RM;
            }
        }
        /* the encoding tables of the pages with EVEX forms have a tuple type before the operands ('A | N/A |
           ModRM:reg (w) | ...', 'B | Full Mem | ...'): operand 0 always has a role, so a first entry that is none is
           that column */
        int skip = 0;
        if (enc && n && enc->nop > n &&
            (!strcmp(enc->op[0], "N/A") || !strcmp(enc->op[0], "NA") || strstr(enc->op[0], "Tuple") ||
             strstr(enc->op[0], "Full") || strstr(enc->op[0], "Half") || strstr(enc->op[0], "Quarter") ||
             strstr(enc->op[0], "Eighth") || strstr(enc->op[0], "Mem") || strstr(enc->op[0], "Scalar")))
            skip = 1;
        for (k = 0; k < n && enc; ++k) {
            f.op[k].role = role_of(enc->op[k + skip], &f.op[k].read, &f.op[k].write);
            if (f.op[k].kind == OK_FIXED || f.op[k].kind == OK_ONE) f.op[k].role = ROLE_IMPLICIT;
            if (f.op[k].kind == OK_IMM) f.op[k].role = ROLE_IMM;
        }
        /* an r/m operand is a ModRM byte's, though SETcc's rows say neither /r nor /0 (erratum E-16) */
        if (f.modrm < 0 && !f.plus_r)
            for (k = 0; k < n; ++k)
                if (f.op[k].role == ROLE_MODRM_RM) f.modrm = 0;
        /* the operand size: the first register operand's (MOVZX's destination; SHL r/m64, CL's r/m64) */
        f.opsize = 0;
        for (k = 0; k < n && !f.opsize; ++k)
            if (f.op[k].kind == OK_REG || f.op[k].kind == OK_FIXED) f.opsize = f.op[k].size;
        if (!f.opsize) f.opsize = f.rex_w || f.vex_w ? 64 : 32;
        /* a 16-bit operand size is the operand-size prefix's, which the opcode column leaves out (Vol. 2A, 2.1.1) */
        if (f.opsize == 16 && !f.vex) {
            int has = 0;
            for (k = 0; k < f.nprefix; ++k) has |= f.prefix[k] == 0x66;
            if (!has) {
                if (f.nprefix == 4) continue;
                memmove(f.prefix + 1, f.prefix, (size_t)f.nprefix);
                f.prefix[0] = 0x66;
                ++f.nprefix;
            }
        }
        /* a condition in the low nibble of Jcc's, SETcc's and CMOVcc's opcode, which their page's name says */
        if (strstr(p->name, "cc") && f.nopc >= 1) f.cc = f.opc[f.nopc - 1] & 15;
        g_forms = (Form*)realloc(g_forms, (g_nforms + 1) * sizeof *g_forms);
        if (!g_forms) exit(1);
        g_forms[g_nforms++] = f;
    }
}

/* ---- evaluation ---- */

/* the cells an evaluation reads and writes: a form's operands, the general-purpose registers, the six status flags;
   a primitive's result, Arm's four flags and its inputs */
enum {
    C_OP0, C_OP1, C_OP2, C_OP3,
    C_GPR0, /* RAX RCX RDX RBX RSP RBP RSI RDI R8-R15 */
    C_CF = C_GPR0 + 16, C_PF, C_AF, C_ZF, C_SF, C_OF,
    C_RES, C_N, C_Z, C_C, C_V,
    C_IN0, /* a primitive's inputs */
    C_COND = C_IN0 + 8, /* the condition a cc form tests (its page's 'condition') */
    NCELL
};
#define NFLAG 6
static const char* const k_flag_names[NFLAG] = {"CF", "PF", "AF", "ZF", "SF", "OF"};
static const char* const k_gpr64[16] = {"RAX", "RCX", "RDX", "RBX", "RSP", "RBP", "RSI", "RDI",
                                        "R8",  "R9",  "R10", "R11", "R12", "R13", "R14", "R15"};
static const char* const k_gpr32[8] = {"EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"};
static const char* const k_gpr16[8] = {"AX", "CX", "DX", "BX", "SP", "BP", "SI", "DI"};
static const char* const k_gpr8[4] = {"AL", "CL", "DL", "BL"};
static const char* const k_gpr8h[4] = {"AH", "CH", "DH", "BH"};

enum { V_NONE, V_BITS, V_INT };
typedef struct {
    int kind;
    unsigned t;    /* V_BITS */
    unsigned u, s; /* V_INT: the value with bits read as unsigned and as signed integers (Vol. 1, 3.4.3.1: the
                      processor evaluates the result of an addition for both) */
} Val;

typedef struct {
    const char* name;
    Val v;
} Local;

typedef struct {
    unsigned cell[NCELL];
    unsigned char written[NCELL];
    Local* locals;
    unsigned nlocals;
    Val result; /* the last result an arithmetic assignment made: what 'set according to the result' reads */
    int result_bad;
    unsigned fault; /* one bit: the instruction raises an exception (#DE...) */
    int status_from_prose; /* ModifyStatusFlags: CMP's 'in the same manner as the SUB instruction' */
} Env;

typedef struct {
    /* a form, or a primitive */
    const Form* form;
    int opsize;
    int width; /* a primitive's W */
    const char* mnem;
    /* the names of a primitive's inputs, in C_IN0... */
    const char* in_names[8];
    unsigned in_widths[8];
    int nin;
    int unreadable;
    const char* why;
    char why_buf[160];
    int depth;
} Ctx;

static void unreadable(Ctx* c, const char* fmt, ...) {
    va_list ap;
    if (c->unreadable) return;
    c->unreadable = 1;
    va_start(ap, fmt);
    vsnprintf(c->why_buf, sizeof c->why_buf, fmt, ap);
    va_end(ap);
    c->why = c->why_buf;
}

static Val vbits(unsigned t) {
    Val v = {V_BITS, t, 0, 0};
    return v;
}
static Val vint(unsigned u, unsigned s) {
    Val v = {V_INT, 0, u, s};
    return v;
}
static Val vnum(long long k) { return vint(t_iconst(i128(k)), t_iconst(i128(k))); }
static Val vnone(void) {
    Val v = {V_NONE, 0, 0, 0};
    return v;
}

static unsigned U(Val v) { return v.kind == V_BITS ? t_uint(v.t) : v.u; }
/* a flag (one bit: ADC's CF) is a number 0 or 1 in arithmetic, signed or not */
static unsigned S(Val v) { return v.kind == V_BITS ? (t_width(v.t) == 1 ? t_uint(v.t) : t_sint(v.t)) : v.s; }

/* v as w bits: bits zero extended or truncated, an integer modulo 2^w */
static unsigned as_bits(Val v, unsigned w) {
    if (v.kind == V_BITS) return t_resize(v.t, w);
    if (v.kind == V_INT) return t_trunc(v.u, w);
    return t_unknown(w);
}

static Env env_new(void) {
    Env e;
    memset(&e, 0, sizeof e);
    e.fault = t_const(1, i128(0));
    return e;
}
static Env env_copy(const Env* e) {
    Env c = *e;
    if (e->nlocals) {
        c.locals = (Local*)sg_alloc(e->nlocals * sizeof *c.locals);
        memcpy(c.locals, e->locals, e->nlocals * sizeof *c.locals);
    } else
        c.locals = NULL;
    return c;
}
static void env_free(Env* e) {
    free(e->locals);
    e->locals = NULL;
    e->nlocals = 0;
}
static Val* local(Env* e, const char* name) {
    unsigned i;
    for (i = 0; i < e->nlocals; ++i)
        if (!strcmp(e->locals[i].name, name)) return &e->locals[i].v;
    return NULL;
}
static void set_local(Env* e, const char* name, Val v) {
    Val* p = local(e, name);
    if (p) {
        *p = v;
        return;
    }
    e->locals = (Local*)realloc(e->locals, (e->nlocals + 1) * sizeof *e->locals);
    if (!e->locals) exit(1);
    e->locals[e->nlocals].name = name;
    e->locals[e->nlocals++].v = v;
}

/* a name's cell and the bits of it the name is (a register's view, an operand at its size) */
typedef struct {
    int cell;
    unsigned hi, lo;
    int imm; /* an immediate operand: sign extended to the operand size where narrower (Vol. 2A 2.2.1.5) */
} Ref;

static int operand_of_name(const Ctx* c, const char* n) {
    int nops = c->form->nops;
    if (!strcmp(n, "DEST")) return 0;
    if (nops == 1) return !strcmp(n, "SRC") || !strcmp(n, "SRC1") ? 0 : -1;
    if (nops == 2) {
        if (!strcmp(n, "SRC1")) return 0;
        if (!strcmp(n, "SRC") || !strcmp(n, "SRC2") || !strcmp(n, "COUNT")) return 1;
        return -1;
    }
    if (nops >= 3) {
        if (!strcmp(n, "SRC1") || !strcmp(n, "SRC")) return 1;
        if (!strcmp(n, "SRC2")) return 2;
        /* SHLD's third operand is its count; SHLX's page names a temporary COUNT */
        if (!strcmp(n, "COUNT")) return c->form->op[2].kind == OK_IMM || c->form->op[2].kind == OK_FIXED ? 2 : -1;
        if (!strcmp(n, "SRC3")) return 3;
    }
    return -1;
}

static int resolve(const Ctx* c, const char* n, Ref* r) {
    int k;
    memset(r, 0, sizeof *r);
    if (c->form) {
        k = operand_of_name(c, n);
        if (k < 0 && (!strcmp(n, "imm8") || !strcmp(n, "imm") || !strcmp(n, "imm32") || !strcmp(n, "ib")))
            for (k = c->form->nops - 1; k >= 0 && c->form->op[k].kind != OK_IMM; --k) {
            }
        if (k >= 0 && k < c->form->nops) {
            const FOp* o = &c->form->op[k];
            if (o->kind == OK_FIXED) {
                r->cell = C_GPR0 + o->fixed;
                r->hi = (unsigned)o->size - 1;
                r->lo = 0;
                return 1;
            }
            r->cell = C_OP0 + k;
            r->hi = (unsigned)o->size - 1;
            r->lo = 0;
            r->imm = o->kind == OK_IMM || o->kind == OK_ONE;
            return 1;
        }
        for (k = 0; k < 16; ++k)
            if (!strcmp(n, k_gpr64[k])) return r->cell = C_GPR0 + k, r->hi = 63, r->lo = 0, 1;
        for (k = 0; k < 8; ++k) {
            if (!strcmp(n, k_gpr32[k])) return r->cell = C_GPR0 + k, r->hi = 31, r->lo = 0, 1;
            if (!strcmp(n, k_gpr16[k])) return r->cell = C_GPR0 + k, r->hi = 15, r->lo = 0, 1;
        }
        for (k = 0; k < 4; ++k) {
            if (!strcmp(n, k_gpr8[k])) return r->cell = C_GPR0 + k, r->hi = 7, r->lo = 0, 1;
            if (!strcmp(n, k_gpr8h[k])) return r->cell = C_GPR0 + k, r->hi = 15, r->lo = 8, 1;
        }
        for (k = 0; k < NFLAG; ++k)
            if (!strcmp(n, k_flag_names[k])) return r->cell = C_CF + k, r->hi = 0, r->lo = 0, 1;
        if (!strcmp(n, "RFLAGS.CF") || !strcmp(n, "EFLAGS.CF")) return r->cell = C_CF, 1;
        return 0;
    }
    if (!strcmp(n, "RESULT")) return r->cell = C_RES, r->hi = (unsigned)c->width - 1, r->lo = 0, 1;
    if (!strcmp(n, "N")) return r->cell = C_N, 1;
    if (!strcmp(n, "Z")) return r->cell = C_Z, 1;
    if (!strcmp(n, "C")) return r->cell = C_C, 1;
    if (!strcmp(n, "V")) return r->cell = C_V, 1;
    for (k = 0; k < c->nin; ++k)
        if (!strcmp(n, c->in_names[k])) return r->cell = C_IN0 + k, r->hi = c->in_widths[k] - 1, r->lo = 0, 1;
    return 0;
}

static unsigned read_ref(Ctx* c, Env* e, const Ref* r) {
    unsigned t = e->cell[r->cell];
    if (!t) {
        unreadable(c, "a cell read before it is set");
        return t_unknown(r->hi - r->lo + 1);
    }
    if (r->imm) {
        /* an immediate narrower than the operand size is sign extended to it (Vol. 2A, 2.2.1.5) */
        unsigned w = t_width(t);
        if ((int)w < c->opsize) return t_sext(t, (unsigned)c->opsize);
        return t;
    }
    if (r->hi >= t_width(t)) return t_zext(t, r->hi + 1);
    return t_extract(t, r->hi, r->lo);
}

/* writes v (bits of the view's width) into the view. In 64-bit mode a 32-bit register write zero extends to the
   whole register, an 8- or 16-bit one leaves the rest (Vol. 1, 3.4.1.1) */
static void write_ref(Env* e, const Ref* r, unsigned v, int whole_write) {
    unsigned old = e->cell[r->cell], w = t_width(old);
    unsigned hi = r->hi, lo = r->lo;
    e->written[r->cell] = 1;
    if (w == 1 || (lo == 0 && hi == w - 1)) {
        e->cell[r->cell] = v;
        return;
    }
    if (whole_write && lo == 0 && hi == 31 && w == 64) {
        e->cell[r->cell] = t_zext(v, 64);
        return;
    }
    {
        unsigned parts = v;
        if (lo > 0) parts = t_concat(parts, t_extract(old, lo - 1, 0));
        if (hi + 1 < w) parts = t_concat(t_extract(old, w - 1, hi + 1), parts);
        e->cell[r->cell] = parts;
    }
}

static Val eval(Ctx* c, Env* e, const XCode* code, unsigned x);
static void exec(Ctx* c, Env* e, const XCode* code, unsigned s);

static int const_int(Val v, long long* k) {
    I128 x;
    unsigned t = v.kind == V_BITS ? v.t : v.kind == V_INT ? v.u : 0;
    if (!t || !t_is_const(t, &x) || !i128_fits_ll(x)) return 0;
    if (v.kind == V_INT && v.u != v.s) return 0;
    *k = (long long)x.lo;
    return 1;
}

static unsigned cond_bit(Ctx* c, Val v) {
    if (v.kind == V_BITS) {
        if (t_width(v.t) == 1) return v.t;
        return t_not(t_eq(v.t, t_const(t_width(v.t), i128(0))));
    }
    if (v.kind == V_INT) return t_not(t_eq(v.u, t_iconst(i128(0))));
    unreadable(c, "a condition of no value");
    return t_unknown(1);
}

static int lower_eq(const char* a, const char* b) {
    for (; *a && *b; ++a, ++b) {
        char x = *a, y = *b;
        if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
        if (x != y) return 0;
    }
    return !*a && !*b;
}
static int lower_has(const char* s, const char* what) {
    size_t n = strlen(what), i;
    for (i = 0; s[i]; ++i) {
        size_t j;
        for (j = 0; j < n && s[i + j]; ++j) {
            char x = s[i + j], y = what[j];
            if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
            if (x != y) break;
        }
        if (j == n) return 1;
    }
    return 0;
}

static unsigned prose_cond(Ctx* c, Env* e, const char* text);

/* a condition the parse left as words: the manual's phrases this reads, else none */
static unsigned prose_atom(Ctx* c, Env* e, const char* t) {
    while (*t == ' ' || *t == '(') ++t;
    {
        char buf[200];
        size_t n = strlen(t);
        if (n >= sizeof buf) n = sizeof buf - 1;
        memcpy(buf, t, n);
        buf[n] = 0;
        while (n && (buf[n - 1] == ' ' || buf[n - 1] == ')')) buf[--n] = 0;
        if (lower_has(buf, "64 - bit mode") || lower_has(buf, "64-bit mode") || lower_has(buf, "ia - 32e mode") ||
            lower_has(buf, "ia-32e mode"))
            return t_const(1, i128(1));
        if (lower_eq(buf, "byte operation")) return t_const(1, i128(c->opsize == 8));
        if (lower_eq(buf, "condition") || lower_eq(buf, "condition true")) {
            if (!e->cell[C_COND]) unreadable(c, "a condition with no cc");
            return e->cell[C_COND] ? e->cell[C_COND] : t_unknown(1);
        }
        if (lower_has(buf, "instruction is ") && c->form) {
            /* 'instruction is SAL or SHL' */
            const char* p = strstr(buf, "is ");
            char word[32];
            int any = 0;
            p += 3;
            while (*p) {
                size_t l = 0;
                while (*p == ' ') ++p;
                while (p[l] && p[l] != ' ' && l + 1 < sizeof word) word[l] = p[l], ++l;
                word[l] = 0;
                p += l;
                if (!strcmp(word, "or") || !strcmp(word, "OR") || !l) continue;
                if (!strcmp(word, c->form->mnem)) any = 1;
            }
            return t_const(1, i128(any));
        }
        {
            /* an expression after all (a part of a conjunction the whole of which wasn't one) */
            XCode code = {0};
            char src[260];
            unsigned st;
            snprintf(src, sizeof src, "_c := (%s);", buf);
            st = x_parse(&code, src);
            if (st && code.nodes[st].kind == X_ASSIGN && code.nodes[code.nodes[st].b].kind != X_PROSE) {
                Val v = eval(c, e, &code, code.nodes[st].b);
                unsigned r = cond_bit(c, v);
                x_code_free(&code);
                return r;
            }
            x_code_free(&code);
        }
        unreadable(c, "a condition in words: %s", buf);
        return t_unknown(1);
    }
}

static unsigned prose_cond(Ctx* c, Env* e, const char* text) {
    /* the lowest operator first: ' or ', then ' and ' */
    static const char* const ors[] = {" or ", " OR ", NULL};
    static const char* const ands[] = {" and ", " AND ", NULL};
    int k;
    if (lower_has(text, "instruction is ")) return prose_atom(c, e, text);
    for (k = 0; ors[k]; ++k) {
        const char* p = strstr(text, ors[k]);
        if (p) {
            char* left = dupn(text, (size_t)(p - text));
            unsigned a = prose_cond(c, e, left), b = prose_cond(c, e, p + strlen(ors[k]));
            free(left);
            return t_or(a, b);
        }
    }
    for (k = 0; ands[k]; ++k) {
        const char* p = strstr(text, ands[k]);
        if (p) {
            char* left = dupn(text, (size_t)(p - text));
            unsigned a = prose_cond(c, e, left), b = prose_cond(c, e, p + strlen(ands[k]));
            free(left);
            return t_and(a, b);
        }
    }
    return prose_atom(c, e, text);
}

/* the special names: the operand size, the mode's fields */
static int special(Ctx* c, const char* n, Val* v) {
    const Form* f = c->form;
    if (!strcmp(n, "OperandSize") || !strcmp(n, "SIZE") || !strcmp(n, "OSIZE")) return *v = vnum(c->opsize), 1;
    if (!strcmp(n, "undefined") || !strcmp(n, "UNDEFINED")) return *v = vbits(t_unknown(64)), 1;
    if (!strcmp(n, "condition") && c->form && c->form->cc >= 0) return *v = vbits(t_var("COND", 1)), 1;
    if (!strcmp(n, "W") && !f) return *v = vnum(c->width), 1;
    if (!f) return 0;
    if (!strcmp(n, "NumberOfOperands")) return *v = vnum(f->nops), 1;
    if (!strcmp(n, "StackAddrSize") || !strcmp(n, "AddressSize")) return *v = vnum(64), 1;
    if (!strcmp(n, "CS.L") || !strcmp(n, "IA32_EFER.LMA")) return *v = vbits(t_const(1, i128(1))), 1;
    if (!strcmp(n, "REX.W")) return *v = vbits(t_const(1, i128(f->rex_w))), 1;
    if (!strcmp(n, "VEX.W1")) return *v = vbits(t_const(1, i128(f->vex && f->vex_w))), 1;
    if (!strcmp(n, "VEX.W")) return *v = vbits(t_const(1, i128(f->vex_w))), 1;
    if (!strcmp(n, "VEX.W0")) return *v = vbits(t_const(1, i128(f->vex && !f->vex_w))), 1;
    return 0;
}

static Val binop(Ctx* c, const char* op, int sign, Val a, Val b) {
    long long ka, kb;
    if (a.kind == V_NONE || b.kind == V_NONE) return vnone();
    if (!strcmp(op, "+") || !strcmp(op, "-")) {
        int add = op[0] == '+';
        return vint(add ? t_iadd(U(a), U(b)) : t_isub(U(a), U(b)), add ? t_iadd(S(a), S(b)) : t_isub(S(a), S(b)));
    }
    if (!strcmp(op, "*")) {
        unsigned u = t_imul(U(a), U(b)), s = t_imul(S(a), S(b));
        /* IMUL's 'signed multiplication' (in a comment): its value is the signed product */
        return sign == 1 ? vint(s, s) : vint(u, s);
    }
    if (!strcmp(op, "/") || !strcmp(op, "MOD")) {
        int k;
        if (!const_int(b, &kb) || kb <= 0 || (kb & (kb - 1))) {
            if (const_int(a, &ka) && const_int(b, &kb) && kb) return vnum(!strcmp(op, "/") ? ka / kb : ka % kb);
            /* by a value: the exact quotient (DIV's), or, where the line's comment says the division is signed
               (IDIV's), the signed one truncated toward zero; MOD the remainder of each */
            if (op[0] == '/') {
                unsigned q = sign == 1 ? t_isdiv(S(a), S(b)) : t_iudiv(U(a), U(b));
                return vint(q, q);
            } else {
                unsigned r = t_iurem(U(a), U(b));
                return vint(r, r);
            }
        }
        for (k = 0; (1ll << k) != kb; ++k) {
        }
        if (const_int(a, &ka)) return vnum(op[0] == '/' ? ka >> k : ka & (kb - 1));
        if (op[0] == 'M') {
            /* x MOD 2^k of bits, or of an integer of bits: the low k bits */
            unsigned bits;
            if (a.kind == V_BITS) bits = a.t;
            else if (g_terms[a.u].kind == TK_UINT)
                bits = g_terms[a.u].arg[0];
            else {
                unreadable(c, "a MOD of this integer");
                return vnone();
            }
            if ((unsigned)k >= t_width(bits)) return a.kind == V_BITS ? a : vint(a.u, a.u);
            /* of bits, bits of their width (ROR's count, a loop's counter); of an integer, an integer */
            if (a.kind == V_BITS) return vbits(t_zext(t_extract(bits, (unsigned)k - 1, 0), t_width(bits)));
            return vint(t_uint(t_extract(bits, (unsigned)k - 1, 0)), t_uint(t_extract(bits, (unsigned)k - 1, 0)));
        }
        if (a.kind != V_BITS) {
            unreadable(c, "a division of an integer");
            return vnone();
        }
        /* by 2^k: unsigned, a logical shift; signed (SAR's comment), an arithmetic one, rounding toward negative
           infinity */
        return vbits(sign == 1 ? t_ashr(a.t, t_iconst(i128(k))) : t_lshr(a.t, t_iconst(i128(k))));
    }
    if (!strcmp(op, "SignedModulus")) {
        unsigned r = t_isrem(S(a), S(b));
        return vint(r, r);
    }
    if (!strcmp(op, "AND") || !strcmp(op, "OR") || !strcmp(op, "XOR")) {
        unsigned w, x, y;
        if (a.kind == V_INT && b.kind == V_INT) {
            if (const_int(a, &ka) && const_int(b, &kb))
                return vnum(op[0] == 'A' ? (ka & kb) : op[0] == 'O' ? (ka | kb) : (ka ^ kb));
            unreadable(c, "a bitwise operation of integers");
            return vnone();
        }
        w = a.kind == V_BITS ? t_width(a.t) : t_width(b.t);
        if (a.kind == V_BITS && b.kind == V_BITS && t_width(b.t) > w) w = t_width(b.t);
        x = as_bits(a, w);
        y = as_bits(b, w);
        return vbits(op[0] == 'A' ? t_and(x, y) : op[0] == 'O' ? t_or(x, y) : t_xor(x, y));
    }
    if (!strcmp(op, "=") || !strcmp(op, "!=")) {
        unsigned r;
        I128 k;
        /* bits against a number that fits them: bits' equality (FFFFFFFFFFFFFFFFH included) */
        if (a.kind == V_BITS && b.kind == V_INT && b.u == b.s && t_is_const(b.u, &k) && !i128_neg_p(k) &&
            i128_cmp(k, i128_pow2(t_width(a.t))) < 0)
            b = vbits(t_const(t_width(a.t), k));
        if (b.kind == V_BITS && a.kind == V_INT && a.u == a.s && t_is_const(a.u, &k) && !i128_neg_p(k) &&
            i128_cmp(k, i128_pow2(t_width(b.t))) < 0)
            a = vbits(t_const(t_width(b.t), k));
        if (a.kind == V_BITS && b.kind == V_BITS && t_width(a.t) == t_width(b.t)) r = t_eq(a.t, b.t);
        else
            r = t_eq(U(a), U(b));
        return vbits(op[0] == '!' ? t_not(r) : r);
    }
    if (!strcmp(op, "<") || !strcmp(op, ">") || !strcmp(op, "<=") || !strcmp(op, ">=")) {
        unsigned x = U(a), y = U(b), r;
        if (op[0] == '<') r = op[1] ? t_not(t_ult(y, x)) : t_ult(x, y);
        else
            r = op[1] ? t_not(t_ult(x, y)) : t_ult(y, x);
        return vbits(r);
    }
    if (!strcmp(op, "<<") || !strcmp(op, ">>")) {
        if (a.kind != V_BITS) {
            unreadable(c, "a shift of an integer");
            return vnone();
        }
        return vbits(op[0] == '<' ? t_shl(a.t, U(b)) : t_lshr(a.t, U(b)));
    }
    unreadable(c, "the operator %s", op);
    return vnone();
}

static Val call(Ctx* c, Env* e, const XCode* code, const char* f, unsigned args) {
    Val a[4] = {vnone(), vnone(), vnone(), vnone()};
    int n = 0;
    unsigned x;
    long long k;
    for (x = args; x && n < 4; x = code->nodes[x].next) a[n++] = eval(c, e, code, x);
    if ((!strcmp(f, "ZeroExtend") || !strcmp(f, "SignExtend")) && n >= 1) {
        int sx = f[0] == 'S';
        unsigned w;
        if (n == 2 && const_int(a[1], &k)) w = (unsigned)k;
        else
            w = (unsigned)(c->opsize ? c->opsize : c->width);
        if (a[0].kind == V_INT) return sx ? vint(a[0].s, a[0].s) : vint(a[0].u, a[0].u);
        if (a[0].kind != V_BITS) return vnone();
        /* the manual's ZeroExtend(x) and SignExtend(x) extend to the operand size */
        if (t_width(a[0].t) >= w) return a[0];
        return vbits(sx ? t_sext(a[0].t, w) : t_zext(a[0].t, w));
    }
    if (!strcmp(f, "UInt") && n == 1) return vint(U(a[0]), U(a[0]));
    if (!strcmp(f, "SInt") && n == 1) return vint(S(a[0]), S(a[0]));
    if (!strcmp(f, "IsZero") && n == 1 && a[0].kind == V_BITS)
        return vbits(t_eq(a[0].t, t_const(t_width(a[0].t), i128(0))));
    if (!strcmp(f, "BitCount") && n == 1 && a[0].kind == V_BITS)
        return vbits(t_popcnt(a[0].t));
    if (!strcmp(f, "MSB") && n == 1 && a[0].kind == V_BITS)
        return vbits(t_extract(a[0].t, t_width(a[0].t) - 1, t_width(a[0].t) - 1));
    if (!strcmp(f, "LSB") && n == 1 && a[0].kind == V_BITS) return vbits(t_extract(a[0].t, 0, 0));
    if (!strcmp(f, "Bit") && n == 2 && a[0].kind == V_BITS && const_int(a[1], &k)) {
        if (k < 0 || k >= (long long)t_width(a[0].t)) return vbits(t_const(1, i128(0)));
        return vbits(t_extract(a[0].t, (unsigned)k, (unsigned)k));
    }
    if (!strcmp(f, "TruncateToOperandSize") && n == 1) return vbits(as_bits(a[0], (unsigned)c->opsize));
    if ((!strcmp(f, "LSL") || !strcmp(f, "LSR") || !strcmp(f, "ASR") || !strcmp(f, "ROR")) && n == 2 &&
        a[0].kind == V_BITS) {
        unsigned amt = U(a[1]);
        return vbits(f[1] == 'S' && f[2] == 'L' ? t_shl(a[0].t, amt) : f[0] == 'L' ? t_lshr(a[0].t, amt)
                     : f[0] == 'A'               ? t_ashr(a[0].t, amt)
                                                 : t_ror(a[0].t, amt));
    }
    if (!strcmp(f, "BitwiseXNOR") && n == 1 && a[0].kind == V_BITS) {
        /* PF: the XNOR of the low byte's bits */
        unsigned i, r = t_const(1, i128(1)), w = t_width(a[0].t);
        for (i = 0; i < w && i < 8; ++i) r = t_xor(r, t_extract(a[0].t, i, i));
        return vbits(r);
    }
    if (!strcmp(f, "Concat") && n == 2 && a[0].kind == V_BITS && a[1].kind == V_BITS)
        return vbits(t_concat(a[0].t, a[1].t));
    unreadable(c, "the function %s", f);
    return vnone();
}

static Val eval(Ctx* c, Env* e, const XCode* code, unsigned x) {
    const XNode* n = &code->nodes[x];
    switch (n->kind) {
    case X_NUM: {
        /* a literal is a nonnegative number, 64 bits of it (FFFFFFFFFFFFFFFFH is 2^64 - 1, not -1) */
        unsigned k = t_iconst(i128_u(n->value));
        return vint(k, k);
    }
    case X_NAME: {
        Ref r;
        Val v, *l = local(e, n->text);
        if (l) return *l;
        if (resolve(c, n->text, &r)) return vbits(read_ref(c, e, &r));
        if (special(c, n->text, &v)) return v;
        unreadable(c, "the name %s", n->text);
        return vnone();
    }
    case X_BIN: {
        Val a = eval(c, e, code, n->a), b = eval(c, e, code, code->nodes[x].b);
        return binop(c, code->nodes[x].text, code->nodes[x].sign, a, b);
    }
    case X_UN: {
        Val a = eval(c, e, code, n->a);
        if (code->nodes[x].text[0] == '-') return a.kind == V_NONE ? a : vint(t_ineg(U(a)), t_ineg(S(a)));
        if (a.kind == V_BITS) return vbits(t_not(a.t));
        if (a.kind == V_INT) {
            long long k;
            if (const_int(a, &k)) return vbits(t_const(1, i128(!k)));
        }
        unreadable(c, "a NOT of an integer");
        return vnone();
    }
    case X_CALL: return call(c, e, code, n->text, n->a);
    case X_SLICE:
    case X_INDEX: {
        Val v = eval(c, e, code, n->a), h = eval(c, e, code, code->nodes[x].b), l;
        long long hi, lo;
        unsigned w;
        l = code->nodes[x].kind == X_SLICE ? eval(c, e, code, code->nodes[x].c) : h;
        if (!const_int(h, &hi) || !const_int(l, &lo) || hi < 0 || lo < 0) {
            unreadable(c, "a slice at a run-time place");
            return vnone();
        }
        if (hi < lo) {
            long long s = hi; /* TEST's TEMP[0:7] */
            hi = lo;
            lo = s;
        }
        if (v.kind == V_INT) return vbits(t_extract(t_trunc(v.u, (unsigned)hi + 1), (unsigned)hi, (unsigned)lo));
        if (v.kind != V_BITS) return vnone();
        w = t_width(v.t);
        if ((unsigned)hi >= w) v.t = t_zext(v.t, (unsigned)hi + 1);
        return vbits(t_extract(v.t, (unsigned)hi, (unsigned)lo));
    }
    case X_PROSE: return vbits(prose_cond(c, e, n->text));
    default: unreadable(c, "an expression of kind %d", n->kind); return vnone();
    }
}

/* ---- statements ---- */

/* the result of an arithmetic assignment, for the flags 'set according to the result' */
static void note_result(Env* e, Val v) {
    if (v.kind != V_NONE) {
        e->result = v;
        e->result_bad = 0;
    }
}

static void assign(Ctx* c, Env* e, const XCode* code, unsigned target, Val v) {
    const XNode* t = &code->nodes[target];
    Ref r;
    if (v.kind == V_NONE) return;
    if (t->kind == X_NAME) {
        if (!local(e, t->text) && resolve(c, t->text, &r)) {
            unsigned w = r.hi - r.lo + 1;
            if (r.imm) {
                unreadable(c, "a write to an immediate");
                return;
            }
            write_ref(e, &r, as_bits(v, w), 1);
            if (r.cell < C_CF || r.cell == C_RES) note_result(e, v);
            return;
        }
        if (c->form && !local(e, t->text) &&
            (starts(t->text, "DEST") || (starts(t->text, "SRC") && t->text[3] >= '0' && t->text[3] <= '9'))) {
            /* an operand the names of DEST, SRC, SRC1 and SRC2 don't reach (MULX's DEST1 and DEST2) */
            unreadable(c, "the operand %s", t->text);
            return;
        }
        {
            /* a temporary first given bits keeps their width (SHL's tempDEST := tempDEST * 2) */
            Val* lv = local(e, t->text);
            if (v.kind == V_INT) note_result(e, v);
            if (lv && lv->kind == V_BITS && v.kind == V_INT) v = vbits(as_bits(v, t_width(lv->t)));
        }
        set_local(e, t->text, v);
        return;
    }
    if (t->kind == X_SLICE || t->kind == X_INDEX) {
        const XNode* base = &code->nodes[t->a];
        Val h = eval(c, e, code, t->b), l = t->kind == X_SLICE ? eval(c, e, code, t->c) : h;
        long long hi, lo;
        if (!const_int(h, &hi) || !const_int(l, &lo) || base->kind != X_NAME) {
            unreadable(c, "a slice assignment it can't place");
            return;
        }
        if (hi < lo) {
            long long s = hi;
            hi = lo;
            lo = s;
        }
        if (!local(e, base->text) && resolve(c, base->text, &r)) {
            Ref s = r;
            s.hi = r.lo + (unsigned)hi;
            s.lo = r.lo + (unsigned)lo;
            if (s.hi >= t_width(e->cell[r.cell])) {
                unreadable(c, "a slice past its register");
                return;
            }
            write_ref(e, &s, as_bits(v, (unsigned)(hi - lo + 1)), 0);
            return;
        }
        {
            Val* lv = local(e, base->text);
            if (lv && lv->kind == V_BITS && (unsigned)hi < t_width(lv->t)) {
                unsigned w = t_width(lv->t), parts = as_bits(v, (unsigned)(hi - lo + 1));
                if (lo > 0) parts = t_concat(parts, t_extract(lv->t, (unsigned)lo - 1, 0));
                if ((unsigned)hi + 1 < w) parts = t_concat(t_extract(lv->t, w - 1, (unsigned)hi + 1), parts);
                lv->t = parts;
                return;
            }
        }
        unreadable(c, "a slice of %s", base->text);
        return;
    }
    if (t->kind == X_CALL && !strcmp(t->text, "Concat")) {
        /* RDX:RAX := v */
        unsigned a = t->a, b = code->nodes[a].next;
        Ref ra, rb;
        if (code->nodes[a].kind == X_NAME && code->nodes[b].kind == X_NAME && resolve(c, code->nodes[a].text, &ra) &&
            resolve(c, code->nodes[b].text, &rb)) {
            unsigned wa = ra.hi - ra.lo + 1, wb = rb.hi - rb.lo + 1, all = as_bits(v, wa + wb);
            write_ref(e, &rb, t_extract(all, wb - 1, 0), 1);
            write_ref(e, &ra, t_extract(all, wa + wb - 1, wb), 1);
            note_result(e, v);
            return;
        }
    }
    unreadable(c, "an assignment to %s (line %u)", t->kind == X_PROSE || t->kind == X_NAME ? t->text : "this",
               code->nodes[target].line);
}

static Env merge(unsigned cnd, Env* a, Env* b) {
    Env m = env_copy(b);
    unsigned i;
    for (i = 0; i < NCELL; ++i) {
        if (a->cell[i] == b->cell[i]) continue;
        if (!a->cell[i] || !b->cell[i] || t_width(a->cell[i]) != t_width(b->cell[i]))
            m.cell[i] = a->cell[i] ? t_unknown(t_width(a->cell[i])) : t_unknown(t_width(b->cell[i]));
        else
            m.cell[i] = t_ite(cnd, a->cell[i], b->cell[i]);
        m.written[i] = a->written[i] | b->written[i];
    }
    for (i = 0; i < NCELL; ++i) m.written[i] = a->written[i] | b->written[i];
    for (i = 0; i < a->nlocals; ++i) {
        Val va = a->locals[i].v;
        Val* vb = local(&m, a->locals[i].name);
        Val r;
        if (!vb) {
            /* set on one way only: what the other way reads of it, nothing does (the manual's temporaries) */
            set_local(&m, a->locals[i].name, va);
            continue;
        }
        if (va.kind == V_BITS && vb->kind == V_BITS && t_width(va.t) == t_width(vb->t)) r = vbits(t_ite(cnd, va.t, vb->t));
        else if (va.kind == V_INT && vb->kind == V_INT && va.u == vb->u && va.s == vb->s)
            r = va;
        else
            r = vbits(t_unknown(64));
        *vb = r;
    }
    m.fault = t_ite(cnd, a->fault, b->fault);
    if (a->result.kind != V_NONE || b->result.kind != V_NONE) {
        if (a->result.kind == V_BITS && b->result.kind == V_BITS && t_width(a->result.t) == t_width(b->result.t))
            m.result = vbits(t_ite(cnd, a->result.t, b->result.t));
        else if (a->result.kind == b->result.kind && a->result.t == b->result.t && a->result.u == b->result.u &&
                 a->result.s == b->result.s)
            m.result = a->result;
        else
            m.result_bad = 1;
    }
    m.result_bad |= a->result_bad;
    m.status_from_prose = a->status_from_prose | b->status_from_prose;
    return m;
}

static void exec_list(Ctx* c, Env* e, const XCode* code, unsigned s) {
    for (; s && !c->unreadable; s = code->nodes[s].next) exec(c, e, code, s);
}

/* the names a loop's body assigns */
static void assigned_names(const XCode* code, unsigned s, const char** names, unsigned* n, unsigned max) {
    for (; s; s = code->nodes[s].next) {
        const XNode* x = &code->nodes[s];
        if (x->kind == X_ASSIGN) {
            const XNode* t = &code->nodes[x->a];
            const char* name = t->kind == X_NAME ? t->text : (t->kind == X_SLICE || t->kind == X_INDEX) &&
                                                                     code->nodes[t->a].kind == X_NAME
                                                                 ? code->nodes[t->a].text
                                                                 : NULL;
            unsigned i;
            if (name) {
                for (i = 0; i < *n; ++i)
                    if (!strcmp(names[i], name)) break;
                if (i == *n && *n < max) names[(*n)++] = name;
            }
        } else if (x->kind == X_IF) {
            assigned_names(code, x->b, names, n, max);
            assigned_names(code, x->c, names, n, max);
        } else if (x->kind == X_WHILE || x->kind == X_FOR)
            assigned_names(code, x->kind == X_WHILE ? x->b : x->c, names, n, max);
    }
}

/* a name's current value, as bits (a cell's whole, a local's) */
static int name_value(Ctx* c, Env* e, const char* name, unsigned* t, int* cell) {
    Val* l = local(e, name);
    Ref r;
    *cell = -1;
    if (l) {
        if (l->kind != V_BITS) return 0;
        *t = l->t;
        return 1;
    }
    if (resolve(c, name, &r) && !r.imm && e->cell[r.cell]) {
        *cell = r.cell;
        *t = e->cell[r.cell];
        return 1;
    }
    return 0;
}

/* a WHILE (n != 0) whose body counts n down by one and steps each other name it sets by one shift or rotation of
   itself (SHL's, SAR's, ROR's): n steps of it, which is the shift by n. 0 when the loop is not one */
static int summarize(Ctx* c, Env* e, const XCode* code, unsigned cond, unsigned body) {
    const XNode* cn = &code->nodes[cond];
    const char* names[16];
    unsigned n = 0, i, fresh[16], before[16];
    int cells[16];
    const char* counter;
    Env t;
    int ok = 1;
    if (cn->kind != X_BIN || strcmp(cn->text, "!=") || code->nodes[cn->a].kind != X_NAME ||
        code->nodes[cn->b].kind != X_NUM || code->nodes[cn->b].value != 0)
        return 0;
    counter = code->nodes[cn->a].text;
    assigned_names(code, body, names, &n, 16);
    {
        /* a name the body sets that has no value before the loop is the body's own temporary (ROR's tempCF): it
           carries nothing from one step to the next, and is left undefined after */
        unsigned o = 0;
        for (i = 0; i < n; ++i) {
            unsigned t;
            int cell;
            if (!local(e, names[i]) && !name_value(c, e, names[i], &t, &cell)) {
                Ref r;
                if (resolve(c, names[i], &r)) return 0;
                continue;
            }
            names[o++] = names[i];
        }
        n = o;
    }
    for (i = 0; i < n; ++i) {
        char vn[64];
        if (!name_value(c, e, names[i], &before[i], &cells[i])) return 0;
        snprintf(vn, sizeof vn, "loop.%s", names[i]);
        fresh[i] = t_var(vn, t_width(before[i]));
    }
    t = env_copy(e);
    for (i = 0; i < n; ++i) {
        if (cells[i] >= 0) t.cell[cells[i]] = fresh[i];
        else
            local(&t, names[i])->t = fresh[i];
    }
    exec_list(c, &t, code, body);
    if (c->unreadable || !t_is_const(t.fault, NULL)) {
        env_free(&t);
        return 0;
    }
    {
        unsigned after[16], k, count = 0;
        int counter_ok = 0;
        for (i = 0; i < n; ++i) {
            int cell;
            if (!name_value(c, &t, names[i], &after[i], &cell)) ok = 0;
        }
        for (i = 0; i < n && ok; ++i)
            if (!strcmp(names[i], counter)) {
                counter_ok = after[i] == t_sub(fresh[i], t_const(t_width(fresh[i]), i128(1)));
                count = t_uint(before[i]);
            }
        if (!ok || !counter_ok) {
            env_free(&t);
            return 0;
        }
        for (i = 0; i < n; ++i) {
            unsigned r, w = t_width(fresh[i]), one = t_iconst(i128(1)), nz = t_not(t_eq(count, t_iconst(i128(0))));
            int others = 0;
            if (!strcmp(names[i], counter)) {
                r = t_const(w, i128(0));
            } else {
                for (k = 0; k < n; ++k)
                    if (k != i && t_mentions(after[i], fresh[k])) others = 1;
                if (!others && !t_mentions(after[i], fresh[i])) r = t_ite(nz, after[i], before[i]);
                else if (!others && after[i] == t_shl(fresh[i], one))
                    r = t_shl(before[i], count);
                else if (!others && after[i] == t_lshr(fresh[i], one))
                    r = t_lshr(before[i], count);
                else if (!others && after[i] == t_ashr(fresh[i], one))
                    r = t_ashr(before[i], count);
                else if (!others && after[i] == t_rol(fresh[i], one))
                    r = t_rol(before[i], count);
                else if (!others && after[i] == t_ror(fresh[i], one))
                    r = t_ror(before[i], count);
                else
                    r = t_ite(nz, t_unknown(w), before[i]); /* CF's last bit out: not a shift of itself */
            }
            if (cells[i] >= 0) {
                e->cell[cells[i]] = r;
                e->written[cells[i]] = 1;
            } else
                local(e, names[i])->t = r;
        }
    }
    env_free(&t);
    return 1;
}

/* a WHILE Bit(SRC, temp) = 0 DO temp := temp + 1 (or - 1) OD: a scan of SRC from one end for its first set bit,
   whose index temp holds on exit. The loop runs past the operand when SRC is all zeroes; the page's SRC <> 0 guard
   keeps the old DEST then, so the value past the end is only a sentinel. 1 when recognized, temp becoming the
   index's term */
static int scan_bits(Ctx* c, Env* e, const XCode* code, unsigned cond, unsigned body) {
    const XNode* cn = &code->nodes[cond];
    const XNode *bit, *arg, *v;
    const char *src_name, *counter;
    unsigned a = cn->a, b = cn->b, second, src, tree, k, w = (unsigned)c->opsize, steps = 0;
    const XNode* st;
    Val* cv;
    Ref r;
    long long start;
    int up, stop;
    if (cn->kind != X_BIN || (strcmp(cn->text, "=") && strcmp(cn->text, "!="))) return 0;
    if (code->nodes[a].kind == X_CALL && !strcmp(code->nodes[a].text, "Bit") && code->nodes[b].kind == X_NUM &&
        code->nodes[b].value == 0)
        bit = &code->nodes[a];
    else if (code->nodes[b].kind == X_CALL && !strcmp(code->nodes[b].text, "Bit") &&
             code->nodes[a].kind == X_NUM && code->nodes[a].value == 0)
        bit = &code->nodes[b];
    else return 0;
    stop = !strcmp(cn->text, "="); /* the bit that ends the scan: 1 for '= 0', 0 for '!= 0' */
    arg = &code->nodes[bit->a];
    second = arg->next;
    if (arg->kind != X_NAME || !second || code->nodes[second].kind != X_NAME || code->nodes[second].next) return 0;
    src_name = arg->text;
    counter = code->nodes[second].text;
    cv = local(e, counter);
    if (!cv || !const_int(*cv, &start) || w < 1 || w > 128) return 0;
    up = start == 0;
    if (!up && start != (long long)w - 1) return 0;
    for (second = body; second; second = code->nodes[second].next) {
        st = &code->nodes[second];
        if (st->kind != X_ASSIGN || code->nodes[st->a].kind != X_NAME || strcmp(code->nodes[st->a].text, counter))
            return 0;
        v = &code->nodes[st->b];
        if (v->kind != X_BIN || (strcmp(v->text, "+") && strcmp(v->text, "-")) ||
            code->nodes[v->a].kind != X_NAME || strcmp(code->nodes[v->a].text, counter) ||
            code->nodes[v->b].kind != X_NUM || code->nodes[v->b].value != 1 || (v->text[0] == '+') != up)
            return 0;
        ++steps;
    }
    if (steps != 1 || !resolve(c, src_name, &r) || r.imm || !e->cell[r.cell] || r.hi < r.lo ||
        r.hi - r.lo + 1 != w)
        return 0;
    src = read_ref(c, e, &r);
    if (!src || c->unreadable || t_width(src) != w) return 0;
    /* fold from the far end: the first step tested is the outermost choice, as in the primitives' FOR loops */
    tree = t_const(w, i128((long long)w)); /* past the end, unreachable while SRC <> 0 */
    if (up)
        for (k = w; k-- > 0;) {
            unsigned one = t_extract(src, k, k);
            tree = t_ite(stop ? one : t_not(one), t_const(w, i128((long long)k)), tree);
        }
    else
        for (k = 0; k < w; ++k) {
            unsigned one = t_extract(src, k, k);
            tree = t_ite(stop ? one : t_not(one), t_const(w, i128((long long)k)), tree);
        }
    set_local(e, counter, vbits(tree));
    return 1;
}

/* a loop unrolled while its condition is decided, and as nested choices where it is not (LZCNT's, TZCNT's) */
static void unroll(Ctx* c, Env* e, const XCode* code, unsigned cond, unsigned body, int depth) {
    unsigned cb;
    if (c->unreadable) return;
    if (depth > 140) {
        unreadable(c, "a loop of more than 140 steps");
        return;
    }
    cb = cond_bit(c, eval(c, e, code, cond));
    {
        I128 k;
        if (t_is_const(cb, &k)) {
            if (!(k.lo & 1)) return;
            exec_list(c, e, code, body);
            unroll(c, e, code, cond, body, depth + 1);
            return;
        }
    }
    {
        Env taken = env_copy(e);
        Env m;
        exec_list(c, &taken, code, body);
        unroll(c, &taken, code, cond, body, depth + 1);
        m = merge(cb, &taken, e);
        env_free(&taken);
        env_free(e);
        *e = m;
    }
}

static void exec(Ctx* c, Env* e, const XCode* code, unsigned s) {
    const XNode* x = &code->nodes[s];
    switch (x->kind) {
    case X_ASSIGN: {
        Val v = eval(c, e, code, x->b);
        assign(c, e, code, code->nodes[s].a, v);
        return;
    }
    case X_IF: {
        unsigned cb = cond_bit(c, eval(c, e, code, x->a));
        I128 k;
        unsigned then_s = code->nodes[s].b, else_s = code->nodes[s].c;
        if (getenv("SG_ISEL_TRACE") && c->form && !strcmp(getenv("SG_ISEL_TRACE"), c->form->text)) {
            char t[600];
            t_print(cb, t, sizeof t);
            fprintf(stderr, "  IF line %u: %s\n", code->nodes[s].line, t);
        }
        if (c->unreadable) return;
        if (t_is_const(cb, &k)) {
            exec_list(c, e, code, (k.lo & 1) ? then_s : else_s);
            return;
        }
        {
            Env a = env_copy(e), b = env_copy(e), m;
            exec_list(c, &a, code, then_s);
            exec_list(c, &b, code, else_s);
            m = merge(cb, &a, &b);
            env_free(&a);
            env_free(&b);
            env_free(e);
            *e = m;
        }
        return;
    }
    case X_WHILE:
        if (!summarize(c, e, code, x->a, x->b)) {
            c->unreadable = 0;
            if (!scan_bits(c, e, code, x->a, x->b)) {
                c->unreadable = 0;
                unroll(c, e, code, code->nodes[s].a, code->nodes[s].b, 0);
            }
        }
        return;
    case X_FOR: {
        Val lo = eval(c, e, code, x->a), hi = eval(c, e, code, code->nodes[s].b);
        long long a, b, i;
        const char* var = code->nodes[s].text;
        int down = code->nodes[s].down;
        unsigned body = code->nodes[s].c;
        if (!const_int(lo, &a) || !const_int(hi, &b)) {
            unreadable(c, "a FOR of run-time bounds");
            return;
        }
        if (!down)
            for (i = a; i <= b && !c->unreadable; ++i) {
                set_local(e, var, vnum(i));
                exec_list(c, e, code, body);
            }
        else
            for (i = a; i >= b && !c->unreadable; --i) {
                set_local(e, var, vnum(i));
                exec_list(c, e, code, body);
            }
        return;
    }
    case X_RAISE: e->fault = t_const(1, i128(1)); return;
    case X_DO: {
        const XNode* d = &code->nodes[x->a];
        if (d->kind == X_NAME && !strcmp(d->text, "ModifyStatusFlags")) {
            e->status_from_prose = 1;
            return;
        }
        if (d->kind == X_PROSE && (!d->text[0] || !strcmp(d->text, ":") || !strcmp(d->text, ";"))) return;
        if (d->kind == X_PROSE && lower_has(d->text, "flags unchanged")) return; /* SAL's count of 0: as they were */
        if (d->kind == X_PROSE && lower_has(d->text, " is undefined")) {
            /* 'OF is undefined' */
            int j;
            for (j = 0; j < NFLAG; ++j)
                if (starts(d->text, k_flag_names[j]) && d->text[2] == ' ') {
                    e->cell[C_CF + j] = t_unknown(1);
                    e->written[C_CF + j] = 1;
                    return;
                }
        }
        if (d->kind == X_NAME && (!strcmp(d->text, "FI") || !strcmp(d->text, "OD"))) return;
        unreadable(c, "a statement: %s", d->kind == X_PROSE || d->kind == X_NAME || d->kind == X_CALL ? d->text : "?");
        return;
    }
    case X_CASE: {
        /* each arm's label a condition: the subject equal to it, or, where the subject is words ('determine
           count'), the label itself (RCL's 'SIZE := 8:', read as SIZE = 8); the first arm that holds runs */
        unsigned arm, subj = x->a;
        int words = code->nodes[subj].kind == X_PROSE;
        Val sv = words ? vnone() : eval(c, e, code, subj);
        for (arm = code->nodes[s].b; arm && !c->unreadable; arm = code->nodes[arm].next) {
            unsigned label = code->nodes[arm].a, cb;
            I128 k;
            if (!label) {
                exec_list(c, e, code, code->nodes[arm].b);
                return;
            }
            if (code->nodes[label].kind == X_PROSE && strstr(code->nodes[label].text, ":=")) {
                char buf[128];
                const char* t = code->nodes[label].text;
                const char* q = strstr(t, ":=");
                snprintf(buf, sizeof buf, "%.*s = %s", (int)(q - t), t, q + 2);
                cb = prose_cond(c, e, buf);
            } else if (words) {
                cb = cond_bit(c, eval(c, e, code, label));
            } else
                cb = cond_bit(c, binop(c, "=", 0, sv, eval(c, e, code, label)));
            if (!t_is_const(cb, &k)) {
                unreadable(c, "a CASE on a run-time value");
                return;
            }
            if (k.lo & 1) {
                exec_list(c, e, code, code->nodes[arm].b);
                return;
            }
        }
        return;
    }
    default: unreadable(c, "a statement of kind %d", x->kind); return;
    }
}

/* ---- a form's evaluation ---- */

typedef struct {
    int ok;
    const char* why;
    unsigned in[NCELL];  /* the variable each cell starts as */
    unsigned out[NCELL]; /* what each holds at the end */
    unsigned char written[NCELL];
    unsigned fault;
} FEval;

static FEval* g_evals;

/* (L in [lo, hi]) */
static unsigned in_range(unsigned l, I128 lo, I128 hi) {
    return t_and(t_ge0(t_isub(l, t_iconst(lo))), t_ge0(t_isub(t_iconst(hi), l)));
}

/* the Flags Affected section: each sentence's flags set according to the result (Vol. 1, 3.4.3.1: CF where the
   unsigned result doesn't fit the destination, OF where the signed one doesn't, SF its top bit, ZF whether it is
   zero, PF the parity of its low byte), cleared, set, undefined or unaffected. A flag the Operation sets keeps what it
   sets; a sentence that isn't one of these leaves every flag it names undefined, and makes the flags it doesn't name
   undefined too unless the section is only such sentences (nothing is taken as unaffected that the text doesn't say
   is) */
static void apply_flags(Ctx* c, Env* e, const char* prose) {
    char text[2048];
    size_t n = strlen(prose), i = 0;
    int mentioned[NFLAG] = {0}, unsure = 0, j;
    unsigned w = (unsigned)c->opsize;
    unsigned res = 0, sf = 0, zf = 0, pf = 0, cf = 0, of = 0;
    int have = !e->result_bad && e->result.kind != V_NONE;
    if (n >= sizeof text) n = sizeof text - 1;
    memcpy(text, prose, n);
    text[n] = 0;
    if (have) {
        unsigned u = U(e->result), s = S(e->result), k;
        res = as_bits(e->result, w);
        sf = t_extract(res, w - 1, w - 1);
        zf = t_eq(res, t_const(w, i128(0)));
        pf = t_const(1, i128(1));
        for (k = 0; k < 8 && k < w; ++k) pf = t_xor(pf, t_extract(res, k, k));
        cf = t_not(in_range(u, i128(0), i128_sub(i128_pow2(w), i128(1))));
        of = t_not(in_range(s, i128_neg(i128_pow2(w - 1)), i128_sub(i128_pow2(w - 1), i128(1))));
    }
    {
        const char* p = ws(text);
        if (!strcmp(p, "None.") || !strcmp(p, "None. ") || !*p) return;
    }
    while (text[i]) {
        size_t s = i;
        char sent[512];
        int flags[NFLAG] = {0}, any = 0, cls;
        while (text[i] && !(text[i] == '.' && (text[i + 1] == ' ' || !text[i + 1]))) ++i;
        if (i - s >= sizeof sent) {
            unsure = 1;
            if (text[i]) ++i;
            continue;
        }
        memcpy(sent, text + s, i - s);
        sent[i - s] = 0;
        if (text[i]) ++i;
        for (j = 0; j < NFLAG; ++j) {
            const char* q = sent;
            size_t l = strlen(k_flag_names[j]);
            while ((q = strstr(q, k_flag_names[j])) != NULL) {
                int before = q == sent || !((q[-1] >= 'A' && q[-1] <= 'Z') || (q[-1] >= 'a' && q[-1] <= 'z'));
                int after = !((q[l] >= 'A' && q[l] <= 'Z') || (q[l] >= 'a' && q[l] <= 'z'));
                if (before && after) {
                    flags[j] = 1;
                    any = 1;
                }
                q += l;
            }
        }
        if (!any) continue;
        if (lower_has(sent, "according to the result") || lower_has(sent, "based on result") ||
            lower_has(sent, "based on the result"))
            cls = 'R';
        else if ((lower_has(sent, "cleared") || lower_has(sent, "set to 0")) && !lower_has(sent, " if ") &&
                 !lower_has(sent, "otherwise"))
            cls = '0';
        else if (lower_has(sent, "set to 1") && !lower_has(sent, " if ") && !lower_has(sent, "otherwise"))
            cls = '1';
        else if (lower_has(sent, "undefined"))
            cls = 'U';
        else if (lower_has(sent, "unaffected") || lower_has(sent, "not affected"))
            cls = 'K';
        else {
            cls = 'U';
            unsure = 1;
        }
        for (j = 0; j < NFLAG; ++j) {
            unsigned v = 0;
            if (!flags[j]) continue;
            mentioned[j] = 1;
            if (e->written[C_CF + j]) continue; /* the Operation's own */
            switch (cls) {
            case 'R':
                if (!have) v = t_unknown(1);
                else
                    v = j == 0 ? cf : j == 1 ? pf : j == 2 ? t_unknown(1) : j == 3 ? zf : j == 4 ? sf : of;
                break;
            case '0': v = t_const(1, i128(0)); break;
            case '1': v = t_const(1, i128(1)); break;
            case 'K': continue;
            default: v = t_unknown(1); break;
            }
            e->cell[C_CF + j] = v;
            e->written[C_CF + j] = 1;
        }
    }
    if (unsure)
        for (j = 0; j < NFLAG; ++j)
            if (!mentioned[j] && !e->written[C_CF + j]) {
                e->cell[C_CF + j] = t_unknown(1);
                e->written[C_CF + j] = 1;
            }
}

static XCode* g_page_code; /* each page's Operation, parsed once */
static unsigned* g_page_first;
static char* g_page_parsed;

static unsigned page_index(const Page* p) { return (unsigned)(p - g_sdm.pages); }

/* POPCNT's Operation is the one C loop ('For (i=0; i < OperandSize; i++) { ... Count++; }'), which sdm_code.c
   reads as a statement it can't place. The Description and the loop say the primitive's BitCount of the source's
   low OperandSize bits. The page's exception for CPUID.01H:ECX.POPCNT gives the feature set in forms_of */
static const char* operation_of(const Page* p) {
    if (!strcmp(p->name, "POPCNT")) return "DEST := BitCount(TruncateToOperandSize(SRC));\n";
    return p->operation;
}

static void evaluate_form(unsigned fi) {
    Form* f = &g_forms[fi];
    FEval* r = &g_evals[fi];
    Ctx c;
    Env e;
    if (f->xmm) { /* an SSE form: an 'enc' line's encoder at most */
        r->ok = 0;
        r->why = NULL;
        return;
    }
    e = env_new();
    unsigned pi = page_index(f->page), k;
    char name[32];
    memset(&c, 0, sizeof c);
    memset(r, 0, sizeof *r);
    c.form = f;
    c.opsize = f->opsize;
    c.mnem = f->mnem;
    if (!g_page_parsed[pi]) {
        g_page_first[pi] = x_parse(&g_page_code[pi], operation_of(f->page));
        g_page_parsed[pi] = 1;
    }
    for (k = 0; k < (unsigned)f->nops; ++k) {
        const FOp* o = &f->op[k];
        snprintf(name, sizeof name, "op%u", k);
        if (o->kind == OK_REG)
            e.cell[C_OP0 + k] = t_var(name, 64);
        else if (o->kind == OK_IMM)
            e.cell[C_OP0 + k] = t_var(name, (unsigned)o->size);
        else if (o->kind == OK_ONE)
            e.cell[C_OP0 + k] = t_const(8, i128(1));
    }
    for (k = 0; k < 16; ++k) e.cell[C_GPR0 + k] = t_var(k_gpr64[k], 64);
    for (k = 0; k < NFLAG; ++k) {
        snprintf(name, sizeof name, "%s.in", k_flag_names[k]);
        e.cell[C_CF + k] = t_var(name, 1);
    }
    if (f->cc >= 0) e.cell[C_COND] = t_var("COND", 1);
    memcpy(r->in, e.cell, sizeof r->in);
    if (!g_page_first[pi]) {
        r->why = "no Operation section";
        env_free(&e);
        return;
    }
    if (getenv("SG_ISEL_DUMP") && !strcmp(getenv("SG_ISEL_DUMP"), f->text))
        x_dump(&g_page_code[pi], g_page_first[pi], 0, stderr); /* SG_ISEL_DUMP="SHL r/m64, CL": its page's tree */
    exec_list(&c, &e, &g_page_code[pi], g_page_first[pi]);
    if (!c.unreadable) {
        /* a 32-bit destination's write zero extends the register (Vol. 1, 3.4.1.1), slices of it too (BSWAP r32) */
        if (f->nops && f->op[0].kind == OK_REG && f->op[0].size == 32 && e.written[C_OP0])
            e.cell[C_OP0] = t_zext(t_extract(e.cell[C_OP0], 31, 0), 64);
        apply_flags(&c, &e, f->page->flags);
    }
    if (c.unreadable) {
        r->why = c.why ? dups(c.why) : "unreadable";
        env_free(&e);
        return;
    }
    r->ok = 1;
    memcpy(r->out, e.cell, sizeof r->out);
    memcpy(r->written, e.written, sizeof r->written);
    r->fault = e.fault;
    env_free(&e);
}

/* ---- the primitives (x64.ops) ---- */

enum { IN_REG, IN_IMM, IN_COND, IN_CARRY, IN_HI };

typedef struct {
    const char* name;
    int kind;
    char width[8]; /* "W", "L" (log2 W), or a number */
} OpIn;

typedef struct {
    const char* name;
    int widths[4];
    int nwidths;
    OpIn in[4];
    int nin;
    char* code;
    char* assume; /* ASSUME's condition: what the recompiler makes hold, so a form may fault where it fails */
} Prim;

typedef struct {
    const char* name;
    char* expr;
} ArmCond;

static Prim* g_prims;
static unsigned g_nprims;
/* the primitives' file's 'enc NAME FORM' lines: an SSE form's encoder by its instruction column, under NAME */
typedef struct {
    char* name;
    char* text;
    unsigned line;
} EncReq;
static EncReq* g_encs;
static unsigned g_nencs;
static ArmCond g_conds[16];
static unsigned g_nconds;

static void read_ops(const char* path) {
    size_t size, i = 0;
    char* text = sg_read(path, &size);
    Prim* cur = NULL;
    unsigned line_no = 0;
    while (i < size) {
        size_t s = i;
        char* line;
        while (i < size && text[i] != '\n') ++i;
        line = dupn(text + s, i - s);
        ++i;
        ++line_no;
        if (cur) {
            if (!strcmp(line, "end")) {
                cur = NULL;
            } else if (starts(line, "ASSUME ")) {
                cur->assume = dups(line + 7);
            } else {
                size_t a = strlen(cur->code), b = strlen(line);
                cur->code = (char*)realloc(cur->code, a + b + 2);
                if (!cur->code) exit(1);
                memcpy(cur->code + a, line, b);
                cur->code[a + b] = '\n';
                cur->code[a + b + 1] = 0;
            }
            free(line);
            continue;
        }
        if (line[0] == '#' || !line[0]) {
            free(line);
            continue;
        }
        if (starts(line, "enc ")) {
            char* p = (char*)ws(line + 4);
            char* sp = strchr(p, ' ');
            if (!sp) {
                fprintf(stderr, "%s:%u: enc NAME FORM\n", path, line_no);
                exit(1);
            }
            g_encs = (EncReq*)realloc(g_encs, (g_nencs + 1) * sizeof *g_encs);
            if (!g_encs) exit(1);
            g_encs[g_nencs].name = dupn(p, (size_t)(sp - p));
            g_encs[g_nencs].text = dups(ws(sp));
            g_encs[g_nencs++].line = line_no;
            free(line);
            continue;
        }
        if (starts(line, "cond ")) {
            char* p = line + 5;
            char* sp = strchr(p, ' ');
            if (!sp || g_nconds == 16) {
                fprintf(stderr, "%s:%u: cond NAME EXPR\n", path, line_no);
                exit(1);
            }
            g_conds[g_nconds].name = dupn(p, (size_t)(sp - p));
            g_conds[g_nconds++].expr = dups(ws(sp));
            free(line);
            continue;
        }
        if (starts(line, "op ")) {
            char* p = line + 3;
            char* paren = strchr(p, '(');
            char* close = paren ? strchr(paren, ')') : NULL;
            char* q;
            g_prims = (Prim*)realloc(g_prims, (g_nprims + 1) * sizeof *g_prims);
            if (!g_prims || !paren || !close) {
                fprintf(stderr, "%s:%u: op NAME WIDTH... (OPERAND, ...)\n", path, line_no);
                exit(1);
            }
            cur = &g_prims[g_nprims++];
            memset(cur, 0, sizeof *cur);
            cur->code = dups("");
            q = p;
            while (*q && *q != ' ') ++q;
            cur->name = dupn(p, (size_t)(q - p));
            while (q < paren) {
                q = (char*)ws(q);
                if (q >= paren) break;
                if (cur->nwidths < 4) cur->widths[cur->nwidths++] = atoi(q);
                while (*q && *q != ' ' && q < paren) ++q;
            }
            *close = 0;
            q = paren + 1;
            while (*q) {
                char* comma = strchr(q, ',');
                char* item;
                OpIn* in = &cur->in[cur->nin];
                char* colon;
                if (comma) *comma = 0;
                item = (char*)ws(q);
                memset(in, 0, sizeof *in);
                in->kind = IN_REG;
                strcpy(in->width, "W");
                if (starts(item, "imm ")) in->kind = IN_IMM, item = (char*)ws(item + 4);
                else if (starts(item, "hi "))
                    in->kind = IN_HI, item = (char*)ws(item + 3);
                else if (starts(item, "cond "))
                    in->kind = IN_COND, item = (char*)ws(item + 5), strcpy(in->width, "1");
                else if (starts(item, "carry "))
                    in->kind = IN_CARRY, item = (char*)ws(item + 6), strcpy(in->width, "1");
                colon = strchr(item, ':');
                if (colon) {
                    snprintf(in->width, sizeof in->width, "%s", colon + 1);
                    *colon = 0;
                }
                {
                    size_t l = strlen(item);
                    while (l && item[l - 1] == ' ') item[--l] = 0;
                }
                in->name = dups(item);
                if (cur->nin < 4) ++cur->nin;
                if (!comma) break;
                q = comma + 1;
            }
        }
        free(line);
    }
    free(text);
}

static unsigned in_width(const OpIn* in, int w) {
    if (in->kind == IN_HI || !strcmp(in->width, "W")) return (unsigned)w;
    if (!strcmp(in->width, "L")) return w == 64 ? 6 : w == 32 ? 5 : w == 16 ? 4 : 3;
    return (unsigned)atoi(in->width);
}

typedef struct {
    const Prim* prim;
    int w;
    int ok;
    const char* why;
    unsigned in[4];   /* the inputs' variables */
    unsigned result;  /* 0: none */
    unsigned flag[4]; /* N Z C V, 0 where it gives none */
    unsigned assume;  /* ASSUME's condition, 0 for none */
} PEval;

static PEval evaluate_prim(const Prim* p, int w) {
    PEval r;
    Ctx c;
    Env e = env_new();
    XCode code = {0};
    unsigned first, k;
    memset(&r, 0, sizeof r);
    memset(&c, 0, sizeof c);
    r.prim = p;
    r.w = w;
    c.width = w;
    c.opsize = w;
    c.nin = p->nin;
    for (k = 0; k < (unsigned)p->nin; ++k) {
        c.in_names[k] = p->in[k].name;
        c.in_widths[k] = in_width(&p->in[k], w);
        r.in[k] = e.cell[C_IN0 + k] = t_var(p->in[k].name, c.in_widths[k]);
    }
    e.cell[C_RES] = t_const((unsigned)w, i128(0));
    for (k = 0; k < 4; ++k) e.cell[C_N + k] = t_const(1, i128(0));
    first = x_parse(&code, p->code);
    exec_list(&c, &e, &code, first);
    if (c.unreadable) {
        fprintf(stderr, "sailgen --isel: the primitive %s at %d: %s\n", p->name, w, c.why);
        exit(1);
    }
    r.ok = 1;
    if (p->assume) {
        XCode ac = {0};
        char src[512];
        unsigned st;
        snprintf(src, sizeof src, "_c := (%s);", p->assume);
        st = x_parse(&ac, src);
        r.assume = cond_bit(&c, eval(&c, &e, &ac, ac.nodes[st].b));
        if (c.unreadable) {
            fprintf(stderr, "sailgen --isel: the primitive %s's ASSUME: %s\n", p->name, c.why);
            exit(1);
        }
        x_code_free(&ac);
    }
    r.result = e.written[C_RES] ? e.cell[C_RES] : 0;
    for (k = 0; k < 4; ++k) r.flag[k] = e.written[C_N + k] ? e.cell[C_N + k] : 0;
    x_code_free(&code);
    env_free(&e);
    return r;
}

/* Arm's condition, on four flags' terms */
static unsigned arm_cond(const ArmCond* a, const unsigned* nzcv) {
    Ctx c;
    Env e = env_new();
    XCode code = {0};
    char src[256];
    unsigned st, r;
    Val v;
    memset(&c, 0, sizeof c);
    c.width = 1;
    c.opsize = 1;
    for (st = 0; st < 4; ++st) e.cell[C_N + st] = nzcv[st] ? nzcv[st] : t_unknown(1);
    snprintf(src, sizeof src, "_c := (%s);", a->expr);
    st = x_parse(&code, src);
    v = eval(&c, &e, &code, code.nodes[st].b);
    r = cond_bit(&c, v);
    if (c.unreadable) {
        fprintf(stderr, "sailgen --isel: the condition %s: %s\n", a->name, c.why);
        exit(1);
    }
    x_code_free(&code);
    env_free(&e);
    return r;
}

/* Intel's condition codes: the predicate of cc on the flags (CF.in...), from SETcc's rows ('Set byte if above (CF=0
   and ZF=0).'): one per condition code, the opcode's low nibble */
static unsigned g_cc_pred[16];
static const char* g_cc_name[16];

static void read_conditions(void) {
    unsigned fi;
    for (fi = 0; fi < g_nforms; ++fi) {
        Form* f = &g_forms[fi];
        const char* d = f->row->c[6];
        const char* open = strchr(d, '(');
        const char* close = open ? strchr(open, ')') : NULL;
        Ctx c;
        Env e;
        unsigned k, pred;
        char text[128];
        if (f->cc < 0 || !open || !close || !starts(f->page->name, "SETcc")) continue;
        if ((size_t)(close - open - 1) >= sizeof text) continue;
        memcpy(text, open + 1, (size_t)(close - open - 1));
        text[close - open - 1] = 0;
        memset(&c, 0, sizeof c);
        c.form = f;
        c.opsize = 8;
        e = env_new();
        for (k = 0; k < NFLAG; ++k) e.cell[C_CF + k] = g_evals[fi].in[C_CF + k];
        pred = prose_cond(&c, &e, text);
        env_free(&e);
        if (c.unreadable) continue;
        if (g_cc_pred[f->cc] && g_cc_pred[f->cc] != pred) {
            fprintf(stderr, "sailgen --isel: SETcc's rows give condition code %d two predicates\n", f->cc);
            exit(1);
        }
        g_cc_pred[f->cc] = pred;
        if (!g_cc_name[f->cc]) g_cc_name[f->cc] = dups(f->mnem + 3);
    }
}

/* ---- matching ---- */

enum { SLOT_REG, SLOT_IMM, SLOT_COND, SLOT_CARRY };

typedef struct {
    int kind;
    int cell;
    unsigned var;
    unsigned size;
} Slot;

typedef struct {
    unsigned prim; /* index in g_prims */
    int w;
    unsigned form;
    int cell_of[4];          /* each input's cell (C_OP0 + k, or a fixed register's), -1 for none */
    unsigned char clean[4];  /* the input's register must hold it zero extended */
    char fit[4];             /* an immediate narrower than the input: 'S' it must be one sign extended, 'U' zero */
    int fit_bits[4];
    int carry_neg;           /* CF is the carry in's negation (SBB for SBCS) */
    int out_cell;            /* -1: none */
    int clean_out;           /* the result is zero extended in its register */
    int zero_cells;          /* a mask of cells that must hold 0 before it (SETcc's destination, for CSET) */
    int flag_of[4];          /* N Z C V: Intel's flag (0 CF ... 5 OF), -1 none */
    int flag_neg[4];
    unsigned clobber;        /* a mask of the cells it writes other than the result */
    unsigned flags_written;  /* a mask of the six flags */
    signed char cc_of[16];   /* for each Arm condition, Intel's condition code on its flags, -1 none */
} Match;

static Match* g_matches;
static unsigned g_nmatches;

static int slots_of(unsigned fi, Slot* slots) {
    const Form* f = &g_forms[fi];
    const FEval* ev = &g_evals[fi];
    int n = 0, k, c;
    for (k = 0; k < f->nops; ++k)
        if (f->op[k].kind == OK_IMM) {
            slots[n].kind = SLOT_IMM;
            slots[n].cell = C_OP0 + k;
            slots[n].var = ev->in[C_OP0 + k];
            slots[n++].size = (unsigned)f->op[k].size;
        }
    /* a register is an input where something the form writes depends on it */
    for (c = C_OP0; c < C_CF; ++c) {
        int used = 0, o;
        if (!ev->in[c] || t_width(ev->in[c]) != 64) continue;
        for (o = 0; o < NCELL && !used; ++o)
            if (ev->written[o] && t_mentions(ev->out[o], ev->in[c])) used = 1;
        if (used && n < 12) {
            slots[n].kind = SLOT_REG;
            slots[n].cell = c;
            slots[n].var = ev->in[c];
            slots[n++].size = 64;
        }
    }
    if (f->cc >= 0) {
        slots[n].kind = SLOT_COND;
        slots[n].cell = C_COND;
        slots[n++].var = ev->in[C_COND];
    }
    {
        int used = 0, o;
        for (o = 0; o < NCELL && !used; ++o)
            if (ev->written[o] && t_mentions(ev->out[o], ev->in[C_CF])) used = 1;
        if (used) {
            slots[n].kind = SLOT_CARRY;
            slots[n].cell = C_CF;
            slots[n++].var = ev->in[C_CF];
        }
    }
    return n;
}

/* t where the condition p holds (a primitive's ASSUME): each choice whose condition p decides takes its way (DIV's
   result, which the fault's way leaves as it was) */
static void collect_ites(unsigned t, unsigned* found, unsigned* n, unsigned max, unsigned char* seen, unsigned limit) {
    unsigned i;
    if (t < limit) {
        if (seen[t]) return;
        seen[t] = 1;
    }
    if (g_terms[t].kind == TK_ITE && *n < max) found[(*n)++] = t;
    for (i = 0; i < g_terms[t].n; ++i) collect_ites(g_terms[t].arg[i], found, n, max, seen, limit);
}
static unsigned under(unsigned t, unsigned p) {
    unsigned round;
    if (!p || !t) return t;
    for (round = 0; round < 8; ++round) {
        unsigned found[64], from[64], to[64], n = 0, m = 0, k, limit = t + 1;
        unsigned char* seen = (unsigned char*)sg_alloc(limit);
        collect_ites(t, found, &n, 64, seen, limit);
        free(seen);
        for (k = 0; k < n; ++k) {
            unsigned c = g_terms[found[k]].arg[0];
            I128 z;
            if (t_is_const(t_and(t_not(c), p), &z) && !(z.lo & 1)) from[m] = found[k], to[m++] = g_terms[found[k]].arg[1];
            else if (t_is_const(t_and(c, p), &z) && !(z.lo & 1))
                from[m] = found[k], to[m++] = g_terms[found[k]].arg[2];
        }
        if (!m) return t;
        t = t_subst(t, from, to, m);
    }
    return t;
}

static const int k_slot_kind[5] = {SLOT_REG, SLOT_IMM, SLOT_COND, SLOT_CARRY, SLOT_REG};

/* tries one binding (each input to slots[bind[i]]) with its variants; records a match */
static void try_binding(const PEval* pe, unsigned pi, unsigned fi, const Slot* slots, int nslots, const int* bind) {
    const Prim* p = &g_prims[pi];
    const FEval* ev = &g_evals[fi];
    int clean, fitv, neg, zero;
    for (zero = 0; zero < 2; ++zero)
        for (clean = 0; clean < 2; ++clean)
            for (fitv = 0; fitv < 2; ++fitv)
                for (neg = 0; neg < 2; ++neg) {
                    unsigned from[16], to[16], wfrom[4], wto[4];
                    int n = 0, wn = 0, i, used_fit = 0, used_carry = 0, any_unbound = 0;
                    Match m;
                    unsigned result, flags[4];
                    memset(&m, 0, sizeof m);
                    m.prim = pi;
                    m.w = pe->w;
                    m.form = fi;
                    m.out_cell = -1;
                    for (i = 0; i < 4; ++i) m.cell_of[i] = -1, m.flag_of[i] = -1;
                    for (i = 0; i < p->nin; ++i) {
                        const Slot* s = &slots[bind[i]];
                        unsigned v = pe->in[i], wi = t_width(v);
                        m.cell_of[i] = s->cell;
                        switch (s->kind) {
                        case SLOT_REG:
                            if (p->in[i].kind == IN_HI) {
                                /* the high half of the sign extension of the input its width names (IDIV's RDX,
                                   which CQO makes): a function of that input, not one of its own */
                                int j;
                                for (j = 0; j < p->nin; ++j)
                                    if (!strcmp(p->in[j].name, p->in[i].width)) break;
                                if (j == p->nin) continue;
                                v = t_extract(t_sext(pe->in[j], 2 * (unsigned)pe->w), 2 * (unsigned)pe->w - 1,
                                              (unsigned)pe->w);
                                break;
                            }
                            if (wi < 64) {
                                char g[16];
                                snprintf(g, sizeof g, "G.%d", i);
                                v = clean ? t_zext(v, 64) : t_concat(t_var(g, 64 - wi), v);
                                m.clean[i] = (unsigned char)clean;
                            }
                            break;
                        case SLOT_IMM:
                            if (wi <= s->size) v = t_zext(v, s->size);
                            else {
                                /* a narrower immediate: the input must be one extended from it */
                                char kn[16];
                                unsigned kv;
                                snprintf(kn, sizeof kn, "%s.%u", p->in[i].name, s->size);
                                kv = t_var(kn, s->size);
                                wfrom[wn] = pe->in[i];
                                wto[wn++] = fitv ? t_zext(kv, wi) : t_sext(kv, wi);
                                m.fit[i] = fitv ? 'U' : 'S';
                                m.fit_bits[i] = (int)s->size;
                                v = kv;
                                used_fit = 1;
                            }
                            break;
                        case SLOT_CARRY:
                            v = neg ? t_not(v) : v;
                            m.carry_neg = neg;
                            used_carry = 1;
                            break;
                        default: break;
                        }
                        from[n] = s->var;
                        to[n++] = v;
                    }
                    if ((fitv && !used_fit) || (neg && !used_carry)) continue;
                    /* the slots no input takes: free, or 0 (the zeroed destination) */
                    for (i = 0; i < nslots; ++i) {
                        int j, bound = 0;
                        for (j = 0; j < p->nin; ++j)
                            if (bind[j] == i) bound = 1;
                        if (bound || slots[i].kind != SLOT_REG) continue;
                        any_unbound = 1;
                        if (zero) {
                            from[n] = slots[i].var;
                            to[n++] = t_const(64, i128(0));
                            m.zero_cells |= 1 << slots[i].cell;
                        }
                    }
                    if (zero && !any_unbound) continue;
                    {
                        /* a form that may fault: where it faults exactly where the primitive's ASSUME fails (DIV by
                           0), which the recompiler makes hold */
                        unsigned f = t_subst(ev->fault, from, to, (unsigned)n);
                        I128 fk;
                        if (getenv("SG_ISEL_BIND") && !strcmp(getenv("SG_ISEL_BIND"), p->name) && pe->assume) {
                            char t1[800], t2[800];
                            t_print(f, t1, sizeof t1);
                            t_print(t_not(pe->assume), t2, sizeof t2);
                            fprintf(stderr, "%s binding: fault %s\n  vs %s\n", g_forms[fi].text, t1, t2);
                        }
                        if (t_is_const(f, &fk) ? (fk.lo & 1) : !(pe->assume && f == t_not(pe->assume))) continue;
                    }
                    result = pe->result ? t_subst(pe->result, wfrom, wto, (unsigned)wn) : 0;
                    for (i = 0; i < 4; ++i) flags[i] = pe->flag[i] ? t_subst(pe->flag[i], wfrom, wto, (unsigned)wn) : 0;
                    if (result) {
                        int c;
                        for (c = C_OP0; c < C_CF && m.out_cell < 0; ++c) {
                            unsigned o;
                            if (!ev->written[c] || t_width(ev->out[c]) != 64) continue;
                            o = under(t_subst(ev->out[c], from, to, (unsigned)n), pe->assume);
                            if (getenv("SG_ISEL_BIND") && !strcmp(getenv("SG_ISEL_BIND"), p->name)) {
                                char t1[900], t2[300];
                                t_print(o, t1, sizeof t1);
                                t_print(result, t2, sizeof t2);
                                fprintf(stderr, "  %s out %s = %s\n  want %s\n", g_forms[fi].text, c >= C_GPR0 ? k_gpr64[c - C_GPR0] : "op", t1, t2);
                            }
                            if (t_extract(o, (unsigned)pe->w - 1, 0) == result) {
                                m.out_cell = c;
                                m.clean_out = pe->w == 64 || t_extract(o, 63, (unsigned)pe->w) == t_const(64 - (unsigned)pe->w, i128(0));
                            }
                        }
                        if (m.out_cell < 0) continue;
                    }
                    {
                        int ok = 1, k;
                        static const int order[4] = {4, 3, 0, 5}; /* SF, ZF, CF, OF */
                        for (k = 0; k < 4 && ok; ++k) {
                            int j;
                            if (!flags[k]) continue;
                            for (j = 0; j < 4 && m.flag_of[k] < 0; ++j) {
                                int fl = order[j];
                                unsigned x;
                                if (!ev->written[C_CF + fl]) continue;
                                x = t_subst(ev->out[C_CF + fl], from, to, (unsigned)n);
                                if (x == flags[k]) m.flag_of[k] = fl, m.flag_neg[k] = 0;
                                else if (x == t_not(flags[k]))
                                    m.flag_of[k] = fl, m.flag_neg[k] = 1;
                            }
                            if (m.flag_of[k] < 0) ok = 0;
                        }
                        if (!ok) continue;
                    }
                    {
                        int c;
                        for (c = C_OP0; c < C_CF; ++c)
                            if (ev->written[c] && c != m.out_cell) m.clobber |= 1u << c;
                        for (c = 0; c < NFLAG; ++c)
                            if (ev->written[C_CF + c]) m.flags_written |= 1u << c;
                    }
                    /* Arm's conditions on its flags, as Intel's condition codes */
                    memset(m.cc_of, -1, sizeof m.cc_of);
                    if (flags[0] || flags[1] || flags[2] || flags[3]) {
                        unsigned fl[NFLAG], ffrom[NFLAG], a;
                        int j, cc;
                        for (j = 0; j < NFLAG; ++j) {
                            ffrom[j] = ev->in[C_CF + j];
                            fl[j] = ev->written[C_CF + j] ? t_subst(ev->out[C_CF + j], from, to, (unsigned)n) : ffrom[j];
                        }
                        for (a = 0; a < g_nconds; ++a) {
                            unsigned want = arm_cond(&g_conds[a], flags);
                            for (cc = 0; cc < 16; ++cc)
                                if (g_cc_pred[cc] && t_subst(g_cc_pred[cc], ffrom, fl, NFLAG) == want) {
                                    m.cc_of[a] = (signed char)cc;
                                    break;
                                }
                        }
                    }
                    g_matches = (Match*)realloc(g_matches, (g_nmatches + 1) * sizeof *g_matches);
                    if (!g_matches) exit(1);
                    g_matches[g_nmatches++] = m;
                    return;
                }
}

static void bind_inputs(const PEval* pe, unsigned pi, unsigned fi, const Slot* slots, int nslots, int* bind, int i) {
    const Prim* p = &g_prims[pi];
    int s, j;
    if (i == p->nin) {
        try_binding(pe, pi, fi, slots, nslots, bind);
        return;
    }
    for (s = 0; s < nslots; ++s) {
        int taken = 0;
        if (slots[s].kind != k_slot_kind[p->in[i].kind]) continue;
        for (j = 0; j < i; ++j)
            if (bind[j] == s) taken = 1;
        if (taken) continue;
        bind[i] = s;
        bind_inputs(pe, pi, fi, slots, nslots, bind, i + 1);
    }
}

static const char* cell_name(const Form* f, int c) {
    static char buf[4][16];
    static int at;
    char* b = buf[at++ & 3];
    if (c >= C_OP0 && c < C_GPR0) snprintf(b, 16, "op%d", c - C_OP0 + 1);
    else if (c >= C_GPR0 && c < C_CF)
        snprintf(b, 16, "%s", k_gpr64[c - C_GPR0]);
    else if (c >= C_CF && c < C_CF + NFLAG)
        snprintf(b, 16, "%s", k_flag_names[c - C_CF]);
    else if (c == C_COND)
        snprintf(b, 16, "cc");
    else
        snprintf(b, 16, "?");
    (void)f;
    return b;
}

static void report(FILE* out, const PEval* pes, unsigned npes) {
    unsigned i, k, ok = 0;
    fprintf(out, "# sailgen --isel: the forms the SDM's pages give and what each primitive selects\n");
    for (i = 0; i < g_nforms; ++i) ok += g_evals[i].ok;
    fprintf(out, "forms: %u, evaluated: %u\n", g_nforms, ok);
    fprintf(out, "\n## condition codes (SETcc's rows)\n");
    for (i = 0; i < 16; ++i) {
        char t[256];
        t_print(g_cc_pred[i], t, sizeof t);
        fprintf(out, "cc %2u %-4s %s\n", i, g_cc_name[i] ? g_cc_name[i] : "-", t);
    }
    fprintf(out, "\n## primitives\n");
    for (k = 0; k < npes; ++k) {
        const PEval* pe = &pes[k];
        unsigned n = 0;
        fprintf(out, "%s %d:\n", pe->prim->name, pe->w);
        for (i = 0; i < g_nmatches; ++i) {
            const Match* m = &g_matches[i];
            const Form* f = &g_forms[m->form];
            int j;
            if (&g_prims[m->prim] != pe->prim || m->w != pe->w) continue;
            ++n;
            fprintf(out, "  %-28s %-6s", f->text, f->feature[0] ? f->feature : "-");
            for (j = 0; j < pe->prim->nin; ++j) {
                fprintf(out, " %s=%s", pe->prim->in[j].name, cell_name(f, m->cell_of[j]));
                if (m->clean[j]) fprintf(out, "(clean)");
                if (m->fit[j]) fprintf(out, "(fits %c%d)", m->fit[j], m->fit_bits[j]);
            }
            if (m->carry_neg) fprintf(out, " CF=NOT carry");
            if (m->out_cell >= 0) fprintf(out, " -> %s%s", cell_name(f, m->out_cell), m->clean_out ? "" : "(dirty)");
            if (m->zero_cells) fprintf(out, " zeroed:%x", (unsigned)m->zero_cells);
            for (j = 0; j < 4; ++j)
                if (m->flag_of[j] >= 0)
                    fprintf(out, " %c=%s%s", "NZCV"[j], m->flag_neg[j] ? "!" : "", k_flag_names[m->flag_of[j]]);
            if (m->cc_of[0] >= 0 || m->cc_of[10] >= 0) {
                fprintf(out, " cc:");
                for (j = 0; j < (int)g_nconds; ++j)
                    fprintf(out, " %s=%s", g_conds[j].name, m->cc_of[j] >= 0 ? g_cc_name[(int)m->cc_of[j]] : "-");
            }
            fprintf(out, "\n");
        }
        if (!n) fprintf(out, "  (none)\n");
    }
}

static void report_unreadable(FILE* out) {
    unsigned i;
    fprintf(out, "\n## forms not evaluated\n");
    for (i = 0; i < g_nforms; ++i)
        if (!g_evals[i].ok && !g_forms[i].xmm)
            fprintf(out, "%-32s %s\n", g_forms[i].text, g_evals[i].why ? g_evals[i].why : "?");
}

/* ---- the output ---- */

/* what a candidate costs the recompiler, for the order it tries them in: its constraints first (a register the form
   needs zeroed before it, a fixed register, a result tied to an input), then the flags it writes that the primitive
   doesn't give, then its bytes */
static int cost(const Match* m) {
    const Form* f = &g_forms[m->form];
    const Prim* p = &g_prims[m->prim];
    int c = 0, i, gives_flags = 0;
    for (i = 0; i < 4; ++i) gives_flags |= m->flag_of[i] >= 0;
    if (m->zero_cells) c += 1000;
    if (m->out_cell >= C_GPR0) c += 300;
    for (i = 0; i < p->nin; ++i) {
        if (m->cell_of[i] >= C_GPR0 && m->cell_of[i] < C_CF) c += 200;
        if (m->clean[i]) c += 50;
    }
    if (m->out_cell >= 0 && (m->clobber & ((1u << C_GPR0) - 1))) c += 2000; /* writes an input too (XADD) */
    if (m->clobber >> C_GPR0) c += 100;
    for (i = 0; i < p->nin; ++i)
        if (m->out_cell == m->cell_of[i]) c += 20; /* two operands: the input is overwritten */
    if (!gives_flags && m->flags_written) c += 5;
    if (m->out_cell >= 0 && !m->clean_out) c += 2;
    if (f->vex) c += 3;
    if (f->feature[0]) c -= 25; /* a feature the host may have is worth trying first */
    for (i = 0; i < p->nin; ++i)
        if (m->fit[i]) c -= 4 - (m->fit_bits[i] >> 3); /* a short immediate, where it fits */
    return c;
}

static int by_cost(const void* a, const void* b) {
    const Match* x = (const Match*)a;
    const Match* y = (const Match*)b;
    int cx = cost(x), cy = cost(y);
    if (x->prim != y->prim) return x->prim < y->prim ? -1 : 1;
    if (x->w != y->w) return x->w < y->w ? -1 : 1;
    if (cx != cy) return cx < cy ? -1 : 1;
    return x->form < y->form ? -1 : x->form > y->form;
}

/* a candidate worth the table: one that writes no input besides its result (XADD, XCHG), and needs nothing zeroed
   where another doesn't */
static int keep(const Match* m, int any_unzeroed) {
    if (m->out_cell >= 0 && (m->clobber & ((1u << C_GPR0) - 1))) return 0;
    if (m->out_cell < 0 && (m->clobber & ((1u << C_GPR0) - 1))) return 0; /* CMP's flags from SUB: CMP */
    if (m->zero_cells && any_unzeroed) return 0;
    return 1;
}

static const char* feature_c(const char* f) {
    if (!f[0]) return "0";
    if (!strcmp(f, "BMI1")) return "SGX_F_BMI1";
    if (!strcmp(f, "BMI2")) return "SGX_F_BMI2";
    if (!strcmp(f, "LZCNT")) return "SGX_F_LZCNT";
    if (!strcmp(f, "POPCNT")) return "SGX_F_POPCNT";
    return NULL;
}

static void c_name(const Form* f, char* buf, size_t size) {
    size_t i, o = 0;
    for (i = 0; f->text[i] && o + 2 < size; ++i) {
        char ch = f->text[i];
        if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) buf[o++] = ch;
        else if (o && buf[o - 1] != '_')
            buf[o++] = '_';
    }
    while (o && buf[o - 1] == '_') --o;
    buf[o] = 0;
}

/* a form's encoder, its constant bytes folded when sailgen runs: r[k] is the host register of the form's operand k,
   imm the immediate, cc the condition code of a form whose opcode's low nibble is one (SETcc, CMOVcc). The r/m operand
   is a register (0 to 15) or the memory at a register plus the buffer's displacement (16 + the register: the back
   end's loads and stores of the guest's state), which the back end's sgx_modrm encodes (Vol. 2A, 2.1.5) */
static void emit_encoder(unsigned fi) {
    const Form* f = &g_forms[fi];
    char name[64];
    int k, reg = -1, rm = -1, vvvv = -1, opreg = -1, i, last = f->nopc - 1;
    c_name(f, name, sizeof name);
    for (k = 0; k < f->nops; ++k) {
        if (f->op[k].kind != OK_REG) continue;
        switch (f->op[k].role) {
        case ROLE_MODRM_REG: reg = k; break;
        case ROLE_MODRM_RM: rm = k; break;
        case ROLE_VVVV: vvvv = k; break;
        case ROLE_OPCODE_REG: opreg = k; break;
        default: break;
        }
    }
    if (f->plus_r && opreg < 0)
        for (k = 0; k < f->nops; ++k)
            if (f->op[k].kind == OK_REG && f->op[k].role != ROLE_MODRM_REG && f->op[k].role != ROLE_MODRM_RM) opreg = k;
    sg_out("/* %s: %s (the SDM's %s) */\n", f->text, f->row->c[0], f->page->name);
    sg_out("static inline void sgx_e_%s(SgXBuf* b, const unsigned char* r, long long imm, int cc) {\n", name);
    sg_out("    (void)r, (void)imm, (void)cc;\n");
    for (i = 0; i < f->nprefix; ++i) sg_out("    sgx_byte(b, 0x%02X);\n", f->prefix[i]);
    if (f->vex) {
        /* C4, then R X B (inverted) and the map, then W, vvvv (inverted), L and pp (Vol. 2A, 2.3.5) */
        sg_out("    sgx_byte(b, 0xC4);\n");
        sg_out("    sgx_byte(b, (unsigned char)(0x%02X", f->vex_map | (reg < 0 ? 0x80 : 0) | (rm < 0 ? 0x60 : 0));
        if (reg >= 0) sg_out(" | (r[%d] < 8 ? 0x80 : 0)", reg);
        if (rm >= 0) sg_out(" | ((r[%d] & 15) < 8 ? 0x20 : 0) | (sgx_rex_x(b, r[%d]) ? 0 : 0x40)", rm, rm);
        sg_out("));\n");
        sg_out("    sgx_byte(b, (unsigned char)(0x%02X", (f->vex_w << 7) | (f->vex_l << 2) | f->vex_pp | (vvvv < 0 ? 0x78 : 0));
        if (vvvv >= 0) sg_out(" | ((~r[%d] & 15) << 3)", vvvv);
        sg_out("));\n");
    } else {
        /* REX: W, R (ModRM.reg's fourth bit), B (ModRM.rm's or the opcode register's); and for a byte register 4 to
           7, SPL to DIL rather than AH to BH (Vol. 2A, 2.2.1.2) */
        int bsrc = rm >= 0 ? rm : opreg;
        int need8 = 0;
        for (k = 0; k < f->nops; ++k)
            if (f->op[k].kind == OK_REG && f->op[k].size == 8) need8 = 1;
        if (f->rex_w && reg < 0 && bsrc < 0) sg_out("    sgx_byte(b, 0x48);\n");
        else if (reg >= 0 || bsrc >= 0 || f->rex_w) {
            sg_out("    {\n        unsigned rex = 0x%02X", f->rex_w ? 0x48 : 0x40);
            if (reg >= 0) sg_out(" | (r[%d] >> 3) << 2", reg);
            if (bsrc >= 0) sg_out(" | (r[%d] & 15) >> 3", bsrc);
            if (rm >= 0) sg_out(" | sgx_rex_x(b, r[%d])", rm);
            sg_out(";\n");
            if (f->rex_w) sg_out("        sgx_byte(b, (unsigned char)rex);\n");
            else {
                sg_out("        if (rex != 0x40");
                if (need8)
                    for (k = 0; k < f->nops; ++k)
                        if (f->op[k].kind == OK_REG && f->op[k].size == 8) sg_out(" || (r[%d] >= 4 && r[%d] < 8)", k, k);
                sg_out(") sgx_byte(b, (unsigned char)rex);\n");
            }
            sg_out("    }\n");
        }
    }
    for (i = 0; i < f->nopc; ++i) {
        if (i == last && f->plus_r && opreg >= 0) sg_out("    sgx_byte(b, (unsigned char)(0x%02X | (r[%d] & 7)));\n", f->opc[i], opreg);
        else if (i == last && f->cc >= 0)
            sg_out("    sgx_byte(b, (unsigned char)(0x%02X | (cc & 15)));\n", f->opc[i] & 0xF0);
        else
            sg_out("    sgx_byte(b, 0x%02X);\n", f->opc[i]);
    }
    if (f->modrm >= 0) {
        /* ModRM (and a memory operand's SIB and displacement: sgx_modrm) */
        sg_out("    sgx_modrm(b, ");
        if (f->modrm == 8 && reg >= 0) sg_out("r[%d] & 7", reg);
        else
            sg_out("%d", f->modrm & 7);
        sg_out(", %s);\n", rm >= 0 ? (rm == 0 ? "r[0]" : rm == 1 ? "r[1]" : rm == 2 ? "r[2]" : "r[3]") : "0");
    }
    if (f->imm_bytes) sg_out("    sgx_imm(b, imm, %d);\n", f->imm_bytes);
    sg_out("}\n\n");
}

/* the form's operand ModRM.rm encodes, which may be memory: -1 for none */
static int rm_operand(const Form* f) {
    int k;
    for (k = 0; k < f->nops; ++k)
        if (f->op[k].kind == OK_REG && f->op[k].role == ROLE_MODRM_RM) return k;
    return -1;
}

/* the place a selected form takes an input or gives its result: its operand k (0 to 3), or 16 + a fixed register */
static int place(int cell) {
    if (cell >= C_OP0 && cell < C_GPR0) return cell - C_OP0;
    if (cell >= C_GPR0 && cell < C_CF) return 16 + cell - C_GPR0;
    return -1;
}

static void emit_output(const PEval* pes, unsigned npes, const char* source) {
    unsigned i, k;
    char* used = (char*)sg_alloc(g_nforms + 1);
    unsigned char* kept = (unsigned char*)sg_alloc(g_nmatches + 1);
    qsort(g_matches, g_nmatches, sizeof *g_matches, by_cost);
    for (k = 0; k < npes; ++k) {
        int unzeroed = 0, n = 0;
        unsigned pi = (unsigned)(pes[k].prim - g_prims);
        for (i = 0; i < g_nmatches; ++i)
            if (g_matches[i].prim == pi && g_matches[i].w == pes[k].w && !g_matches[i].zero_cells &&
                keep(&g_matches[i], 0))
                unzeroed = 1;
        for (i = 0; i < g_nmatches; ++i) {
            const Match* m = &g_matches[i];
            if (m->prim != pi || m->w != pes[k].w || !keep(m, unzeroed) || !feature_c(g_forms[m->form].feature)) continue;
            if (n == 8) break;
            kept[i] = 1;
            used[m->form] = 1;
            ++n;
        }
    }
    sg_out("/* SPDX-License-Identifier: GPL-3.0-or-later\n   Copyright 2026 the ScarletNX contributors */\n");
    sg_out("/* Generated by sailgen --isel from the Intel SDM's pages and tools/sailgen/x64.ops: the x86-64 back end's "
           "instruction\n   selection (tools/sailgen/README.md, \"The x86-64 back end\"). Do not edit\n");
    if (source) sg_out("   %s\n", source);
    sg_out("*/\n\n");
    sg_out("/* Intel's condition codes, the low nibble of Jcc's, SETcc's and CMOVcc's opcodes (SETcc's rows) */\n");
    sg_out("static const char* const sgx_cc_name[16] = {");
    for (i = 0; i < 16; ++i) sg_out("%s\"%s\"", i ? ", " : "", g_cc_name[i] ? g_cc_name[i] : "");
    sg_out("};\n");
    {
        /* the condition code that is a flag itself, and its negation: SETcc's predicates of one flag */
        int j;
        sg_out("/* each flag's condition code (CF PF AF ZF SF OF), and its negation's, -1 where none is */\n");
        sg_out("static const signed char sgx_flag_cc[6] = {");
        for (j = 0; j < NFLAG; ++j) {
            int cc, found = -1;
            for (cc = 0; cc < 16 && found < 0; ++cc)
                if (g_cc_pred[cc] && g_cc_pred[cc] == g_evals[0].in[C_CF + j]) found = cc;
            sg_out("%s%d", j ? ", " : "", found);
        }
        sg_out("};\nstatic const signed char sgx_flag_ncc[6] = {");
        for (j = 0; j < NFLAG; ++j) {
            int cc, found = -1;
            for (cc = 0; cc < 16 && found < 0; ++cc)
                if (g_cc_pred[cc] && g_cc_pred[cc] == t_not(g_evals[0].in[C_CF + j])) found = cc;
            sg_out("%s%d", j ? ", " : "", found);
        }
        sg_out("};\n");
    }
    sg_out("/* Arm's conditions, in x64.ops' order */\nenum {");
    for (i = 0; i < g_nconds; ++i) sg_out("%sSGX_ARM_%s", i ? ", " : " ", g_conds[i].name);
    sg_out(", SGX_ARM_COUNT };\n\n");
    for (i = 0; i < g_nforms; ++i)
        if (used[i]) emit_encoder(i);
    sg_out("/* each primitive's candidates, cheapest first: the first whose feature the host has and whose immediate fits "
           "is taken */\n");
    for (k = 0; k < npes; ++k) {
        unsigned pi = (unsigned)(pes[k].prim - g_prims), n = 0;
        const Prim* p = &g_prims[pi];
        sg_out("static const SgXSel sgx_sel_%s_%d[] = {\n", p->name, pes[k].w);
        for (i = 0; i < g_nmatches; ++i) {
            const Match* m = &g_matches[i];
            const Form* f = &g_forms[m->form];
            char name[64];
            int j, clean = 0, imm = 0, fit_bits = 0, fit_kind = 0, flag_neg = 0, zero = 0, tied = 0;
            unsigned clob = 0;
            if (!kept[i] || m->prim != pi || m->w != pes[k].w) continue;
            ++n;
            c_name(f, name, sizeof name);
            for (j = 0; j < p->nin; ++j) {
                if (m->clean[j]) clean |= 1 << j;
                if (p->in[j].kind == IN_IMM) imm |= 1 << j;
                if (m->fit[j]) fit_bits = m->fit_bits[j], fit_kind = m->fit[j];
                if (m->out_cell >= 0 && m->cell_of[j] == m->out_cell) tied = j + 1;
            }
            for (j = 0; j < 4; ++j)
                if (m->flag_of[j] >= 0 && m->flag_neg[j]) flag_neg |= 1 << j;
            for (j = C_GPR0; j < C_CF; ++j)
                if (m->clobber & (1u << j)) clob |= 1u << (j - C_GPR0);
            for (j = C_OP0; j < C_CF; ++j)
                if (m->zero_cells & (1 << j)) zero |= 1 << (place(j) & 31);
            sg_out("    /* %s */\n    {sgx_e_%s, %s, %d, {", f->text, name, feature_c(f->feature), rm_operand(f));
            for (j = 0; j < 4; ++j) sg_out("%s%d", j ? ", " : "", j < p->nin ? place(m->cell_of[j]) : -1);
            sg_out("}, 0x%x, 0x%x, %d, '%c', %d, %d, %d, 0x%x, 0x%x, 0x%x, {", clean, imm, fit_bits,
                   fit_kind ? fit_kind : '-', m->out_cell >= 0 ? place(m->out_cell) : -1, m->clean_out, tied, clob,
                   (unsigned)zero, m->flags_written);
            for (j = 0; j < 4; ++j) sg_out("%s%d", j ? ", " : "", m->flag_of[j]);
            sg_out("}, 0x%x, %d, {", flag_neg, m->carry_neg);
            for (j = 0; j < (int)g_nconds; ++j) sg_out("%s%d", j ? ", " : "", m->cc_of[j]);
            sg_out("}},\n");
        }
        sg_out("};\n");
        sg_out("#define sgx_sel_%s_%d_n %u\n", p->name, pes[k].w, n);
    }
    /* a primitive's widths it isn't selected at: no candidates (the back end names a table by its width) */
    for (k = 0; k < g_nprims; ++k) {
        static const int ws[2] = {32, 64};
        int j, q;
        for (j = 0; j < 2; ++j) {
            int has = 0;
            for (q = 0; q < g_prims[k].nwidths; ++q)
                if (g_prims[k].widths[q] == ws[j]) has = 1;
            if (!has)
                sg_out("#define sgx_sel_%s_%d ((const SgXSel*)0)\n#define sgx_sel_%s_%d_n 0u\n", g_prims[k].name, ws[j],
                       g_prims[k].name, ws[j]);
        }
    }
    free(used);
    free(kept);
}

/* an SSE form's CPUID flag as x64.h's SGX_F_*: SSE and SSE2 are every x86-64's */
static const char* xmm_feature_c(const char* f) {
    static const char* const names[][2] = {{"SSE", "0"},           {"SSE2", "0"},          {"SSE3", "SGX_F_SSE3"},
                                           {"SSSE3", "SGX_F_SSSE3"}, {"SSE4_1", "SGX_F_SSE41"}, {"SSE4_2", "SGX_F_SSE42"},
                                           {"PCLMULQDQ", "SGX_F_PCLMUL"}, {"F16C", "SGX_F_F16C"}, {"POPCNT", "SGX_F_POPCNT"},
                                           {"AVX", "SGX_F_AVX"},   {"FMA", "SGX_F_FMA"},   {"AES", "SGX_F_AES"},
                                           {"SHA", "SGX_F_SHA"},   {"AVX2", "SGX_F_AVX2"}, {"GFNI", "SGX_F_GFNI"},
                                           {"AVX GFNI", "SGX_F_AVX | SGX_F_GFNI"}, {"AVX SM4", "SGX_F_AVX | SGX_F_SM4"},
                                           {"", "0"}};
    unsigned k;
    for (k = 0; k < sizeof names / sizeof *names; ++k)
        if (!strcmp(f, names[k][0])) return names[k][1];
    return NULL;
}
/* text with its runs of spaces made one, and none after a comma ('MULSS xmm1,xmm2/m32' as some rows print it) */
static void squeeze(const char* in, char* out, size_t size) {
    size_t o = 0;
    for (; *in && o + 1 < size; ++in)
        if (*in != ' ' || (o && out[o - 1] != ' ' && out[o - 1] != ','))
            out[o++] = *in;
    while (o && out[o - 1] == ' ') --o;
    out[o] = 0;
}
/* the 'enc' lines' forms: each one's encoder and its operands (SgXVForm): where ModRM.reg, ModRM.rm and VEX.vvvv
   are, the XMM ones, the immediate's bytes and the CPUID flag it needs. A form the manual lacks stops the build */
/* SgXVForm sgx_v_<name> of the manual's form f (its encoder emitted once, done[]) */
static void emit_vform(const char* vname, unsigned f, char* done, const char* vex) {
    char name[64];
    int k, reg = -1, rm = -1, vvvv = -1;
    unsigned xmask = 0;
    const char* feat = xmm_feature_c(g_forms[f].feature);
    if (!feat) {
        fprintf(stderr, "sailgen --isel: enc %s: the CPUID flag %s isn't one x64.h knows\n", vname, g_forms[f].feature);
        exit(1);
    }
    if (!done[f]) {
        emit_encoder(f);
        done[f] = 1;
    }
    for (k = 0; k < g_forms[f].nops; ++k) {
        if (g_forms[f].op[k].xmm) xmask |= 1u << k;
        if (g_forms[f].op[k].kind != OK_REG) continue;
        if (g_forms[f].op[k].role == ROLE_MODRM_REG) reg = k;
        if (g_forms[f].op[k].role == ROLE_MODRM_RM) rm = k;
        if (g_forms[f].op[k].role == ROLE_VVVV) vvvv = k;
    }
    c_name(&g_forms[f], name, sizeof name);
    sg_out("static const SgXVForm sgx_v_%s = {sgx_e_%s, %s, %d, %d, %d, %d, %u, %d, %s%s}; /* %s */\n", vname, name,
           feat, g_forms[f].nops, reg, rm, vvvv, xmask, g_forms[f].imm_bytes, vex ? "&sgx_v_" : "NULL", vex ? vex : "",
           g_forms[f].row->c[0]);
}
static int find_form(const char* text) {
    char want[96], have[96];
    unsigned f;
    squeeze(text, want, sizeof want);
    for (f = 0; f < g_nforms; ++f) {
        squeeze(g_forms[f].text, have, sizeof have);
        if (!strcmp(have, want) && g_forms[f].modrm >= 0) return (int)f;
    }
    return -1;
}
/* the 'enc' lines' forms: each one's encoder and its operands (SgXVForm): where ModRM.reg, ModRM.rm and VEX.vvvv
   are, the XMM ones, the immediate's bytes and the CPUID flag it needs. A form the manual lacks stops the build. And
   each two-operand SSE form's AVX twin where the manual's page has it ('PADDD xmm1, xmm2/m128' and 'VPADDD xmm1, xmm2,
   xmm3/m128': the same operation, not tied, the first source in VEX.vvvv), for sgx_vex_of */
static void emit_encs(void) {
    unsigned i;
    char* done = (char*)sg_alloc(g_nforms + 1);
    char* twin = (char*)sg_alloc(g_nencs + 1);
    for (i = 0; i < g_nencs; ++i) {
        char want[96], vtext[128], vname[80];
        int f = find_form(g_encs[i].text), t;
        const char *sp, *comma;
        if (f < 0) {
            squeeze(g_encs[i].text, want, sizeof want);
            fprintf(stderr, "sailgen --isel: enc %s (line %u): no form '%s' in the manual's pages\n", g_encs[i].name,
                    g_encs[i].line, want);
            exit(1);
        }
        /* MNEM xmm1, xmm2/mN (no immediate) -> VMNEM xmm1, xmm2, xmm3/mN, emitted first */
        squeeze(g_encs[i].text, want, sizeof want);
        sp = strchr(want, ' ');
        comma = sp ? strchr(sp, ',') : NULL;
        t = -1;
        if (sp && comma && !strchr(comma + 1, ',') && !strncmp(sp + 1, "xmm1,", 5) && !strncmp(comma + 1, "xmm2/m", 6) &&
            want[0] != 'V' && !g_forms[f].imm_bytes) {
            snprintf(vtext, sizeof vtext, "V%.*s xmm1, xmm2, xmm3/%s", (int)(sp - want), want, comma + 1 + 5);
            t = find_form(vtext);
            if (t >= 0 && strcmp(g_forms[t].feature, "AVX")) t = -1;
        }
        snprintf(vname, sizeof vname, "%s_vex", g_encs[i].name);
        if (t >= 0) emit_vform(vname, (unsigned)t, done, NULL);
        emit_vform(g_encs[i].name, (unsigned)f, done, t >= 0 ? vname : NULL);
        twin[i] = t >= 0;
    }
    free(done);
    free(twin);
}

int sg_isel(const char* pages_path, const char* ops_path, const char* source, const char* out_path,
            const char* report_path) {
    unsigned i, k, npes = 0;
    PEval* pes;
    FILE* rep = NULL;
    read_pages(pages_path);
    read_ops(ops_path);
    for (i = 0; i < g_sdm.npages; ++i) forms_of(&g_sdm.pages[i]);
    g_evals = (FEval*)sg_alloc((g_nforms + 1) * sizeof *g_evals);
    g_page_code = (XCode*)sg_alloc((g_sdm.npages + 1) * sizeof *g_page_code);
    g_page_first = (unsigned*)sg_alloc((g_sdm.npages + 1) * sizeof *g_page_first);
    g_page_parsed = (char*)sg_alloc(g_sdm.npages + 1);
    for (i = 0; i < g_nforms; ++i) evaluate_form(i);
    read_conditions();
    unsigned widths = 0;
    for (i = 0; i < g_nprims; ++i) widths += (unsigned)g_prims[i].nwidths;
    pes = (PEval*)sg_alloc((widths + 1) * sizeof *pes);
    for (i = 0; i < g_nprims; ++i)
        for (k = 0; k < (unsigned)g_prims[i].nwidths; ++k) pes[npes++] = evaluate_prim(&g_prims[i], g_prims[i].widths[k]);
    if (getenv("SG_ISEL_DEBUG")) {
        /* SG_ISEL_DEBUG="PRIM:WIDTH:FORM-TEXT": the terms of both, to see why they differ */
        char want[128], *a, *b;
        snprintf(want, sizeof want, "%s", getenv("SG_ISEL_DEBUG"));
        a = strchr(want, ':');
        b = a ? strchr(a + 1, ':') : NULL;
        if (a && b) {
            char t[4096];
            *a = 0;
            *b = 0;
            for (k = 0; k < npes; ++k)
                if (!strcmp(pes[k].prim->name, want) && pes[k].w == atoi(a + 1)) {
                    int j;
                    t_print(pes[k].result, t, sizeof t);
                    fprintf(stderr, "%s %d RESULT %s\n", want, pes[k].w, t);
                    for (j = 0; j < 4; ++j) {
                        t_print(pes[k].flag[j], t, sizeof t);
                        fprintf(stderr, "  %c %s\n", "NZCV"[j], t);
                    }
                }
            for (i = 0; i < g_nforms; ++i)
                if (!strcmp(g_forms[i].text, b + 1)) {
                    int c;
                    fprintf(stderr, "%s: %s\n", g_forms[i].text, g_evals[i].ok ? "ok" : g_evals[i].why);
                    if (g_evals[i].ok) {
                        t_print(g_evals[i].fault, t, sizeof t);
                        fprintf(stderr, "  fault = %s\n", t);
                    }
                    for (c = 0; c < NCELL && g_evals[i].ok; ++c)
                        if (g_evals[i].written[c]) {
                            t_print(g_evals[i].out[c], t, sizeof t);
                            fprintf(stderr, "  %s = %s\n", cell_name(&g_forms[i], c), t);
                        }
                }
        }
    }
    for (k = 0; k < npes; ++k) {
        unsigned pi = (unsigned)(pes[k].prim - g_prims);
        for (i = 0; i < g_nforms; ++i) {
            Slot slots[16];
            int bind[4], n;
            if (!g_evals[i].ok) continue;
            n = slots_of(i, slots);
            bind_inputs(&pes[k], pi, i, slots, n, bind, 0);
        }
    }
    if (report_path) {
        rep = fopen(report_path, "w");
        if (!rep) {
            fprintf(stderr, "sailgen --isel: cannot write %s\n", report_path);
            free(pes);
            return 1;
        }
        report(rep, pes, npes);
        report_unreadable(rep);
        fclose(rep);
    }
    emit_output(pes, npes, source);
    emit_encs();
    free(pes);
    return sg_flush(out_path);
}
