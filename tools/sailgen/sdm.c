/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --sdm: the instruction pages of Intel's SDM, Volume 2 (2A-2D, "Instruction Set Reference, A-Z", order
   number 325383), out of the text `pdftotext -layout` makes of its PDF, for the x86-64 back end's instruction
   selection (isel.c), which reads each page's pseudocode. The manual is not kept in the tree: the build hands its
   PDF to pdftotext and this, and the pages are the manual's own words, apart from page furniture
   (running footers, form feeds) and pdftotext's column artefacts. Where the text is wrong, or can't be read as
   written, an override (--overlay) replaces the sections it has, so every fix is a file of its own that says why.

   The pages file, one page after another:

     # <title> -- Intel SDM 325383-<rev>, <page>
     @opcodes
     <opcode> | <instruction> | <op/en> | <64-bit mode> | <compat/leg mode> [| <CPUID flag> [| <description>]]
     @encoding
     <op/en> | <operand 1> | <operand 2> | <operand 3> | <operand 4>
     @operation
     <pseudocode>
     @flags
     <prose>

   which is the format of vibe86's tools/sdm/extract.py, this one's origin (an override is a page of its own with
   only the sections it replaces, and comments in # lines), with two columns added where a row has them: the CPUID
   feature flag (the stacked tables'; empty for the general-purpose instructions') and the first line of the row's
   description, where Jcc, SETcc and CMOVcc say what their condition is ('Set byte if above (CF=0 and ZF=0).'). Reading a vendor's pseudocode as the executable specification
   follows Reid, "Trustworthy Specifications of ARM v8-A and v8-M System Level Architecture", FMCAD 2016 */
#include "sailgen.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* text and page point into the manual's text, which read_lines cuts into lines in place */
typedef struct {
    char* text; /* the line, right-trimmed, without form feeds; NULL for a page marker */
    const char* page; /* a page marker's page ("Vol. 2A 3-14") */
} Line;

typedef struct {
    char* s;
    size_t n, cap;
} Buf;

static void buf_add(Buf* b, const char* s, size_t n) {
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = (char*)realloc(b->s, b->cap);
        if (!b->s) {
            fprintf(stderr, "sailgen --sdm: out of memory\n");
            exit(1);
        }
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void buf_str(Buf* b, const char* s) { buf_add(b, s, strlen(s)); }

static char* dup_n(const char* s, size_t n) {
    char* d = (char*)sg_alloc(n + 1);
    memcpy(d, s, n);
    return d;
}
static char* dup_s(const char* s) { return dup_n(s, strlen(s)); }

static int is_upper(int c) { return c >= 'A' && c <= 'Z'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_upper(c) || (c >= 'a' && c <= 'z') || is_digit(c); }
static int is_hex(int c) { return is_digit(c) || (c >= 'A' && c <= 'F'); }
static int is_space(int c) { return c == ' ' || c == '\t'; }

static const char* skip_ws(const char* s) {
    while (is_space(*s)) ++s;
    return s;
}
static int starts(const char* s, const char* p) { return !strncmp(s, p, strlen(p)); }

/* the em dash between an instruction's name and its title */
#define DASH "\xE2\x80\x94"

/* ---- the text's lines ---- */

/* "...  Vol. 2A 3-14" at the end of a footer: the page, or NULL */
static const char* footer_page(const char* line) {
    size_t n = strlen(line);
    const char* v;
    const char* p;
    if (!n || is_space(line[0])) return NULL;
    for (v = line + n; v > line;) {
        --v;
        if (starts(v, "Vol.")) break;
    }
    if (v == line || !starts(v, "Vol.") || v - line < 2 || !is_space(v[-1]) || !is_space(v[-2])) return NULL;
    p = skip_ws(v + 4);
    if (p[0] != '2' || p[1] < 'A' || p[1] > 'D' || !is_space(p[2])) return NULL;
    p = skip_ws(p + 2);
    if (!is_alnum(*p)) return NULL;
    while (is_alnum(*p)) ++p;
    if (*p++ != '-' || !is_digit(*p)) return NULL;
    while (is_digit(*p)) ++p;
    p = skip_ws(p);
    if (*p) return NULL;
    return v;
}

static Line* read_lines(char* text, size_t size, size_t* count, char* revision) {
    Line* lines = NULL;
    size_t n = 0, cap = 0, i = 0;
    const char* order = strstr(text, "Order Number:");
    revision[0] = '?', revision[1] = 0;
    if (order) {
        const char* r = strstr(order, "325383-");
        if (r && r - order < 64) {
            size_t k = 0;
            r += 7;
            while (is_alnum(r[k]) && k < 15) revision[k] = r[k], ++k;
            revision[k] = 0;
        }
    }
    while (i < size) {
        size_t start = i, end;
        char* line = text + start;
        size_t len = 0, j;
        while (i < size && text[i] != '\n') ++i;
        end = i++;
        for (j = start; j < end; ++j)
            if (text[j] != '\f' && text[j] != '\r') line[len++] = text[j];
        while (len && is_space(line[len - 1])) --len;
        line[len] = 0;
        if (n + 2 > cap) {
            cap = cap ? cap * 2 : 1u << 17;
            lines = (Line*)realloc(lines, cap * sizeof *lines);
            if (!lines) {
                fprintf(stderr, "sailgen --sdm: out of memory\n");
                exit(1);
            }
        }
        {
            const char* vol = footer_page(line);
            if (vol) {
                lines[n].text = NULL;
                lines[n++].page = vol;
                continue;
            }
        }
        lines[n].text = line;
        lines[n++].page = NULL;
    }
    *count = n;
    return lines;
}

/* ---- the instruction pages ---- */

/* "NAME—Title": the name is capitals, digits, '/' and spaces */
static int header_line(const char* line) {
    const char* d;
    const char* p;
    if (!is_upper(line[0])) return 0;
    if (strstr(line, "Vol.") || strstr(line, ". . .") || strstr(line, "....")) return 0;
    /* the notation chapter's example ('CMC—Complement Carry Flag [this is an example]'), before CMC's own page */
    if (strstr(line, "[this is an example]")) return 0;
    d = strstr(line, DASH);
    if (!d) return 0;
    for (p = line; p < d; ++p)
        if (!is_alnum(*p) && *p != '/' && *p != ' ') return 0;
    while (d > line && d[-1] == ' ') --d;
    return d > line && is_alnum(d[-1]) && *skip_ws(strstr(line, DASH) + 3) != 0;
}

static int is_page(const Line* l) { return l->text == NULL; }

/* the headings that end a section of an instruction page */
static const char* const k_headings[] = {
    "Opcode", "Instruction Operand Encoding", "Description", "Operation", "Flags Affected",
    "Intel C/C++ Compiler Intrinsic Equivalent", "Intel C/C++ Compiler Intrinsic Equivalents",
    "Protected Mode Exceptions", "Real-Address Mode Exceptions", "Virtual-8086 Mode Exceptions",
    "Compatibility Mode Exceptions", "64-Bit Mode Exceptions", "Exceptions (All Operating Modes)",
    "Exceptions (All Modes of Operation)", "SIMD Floating-Point Exceptions", "Numeric Exceptions",
    "Other Exceptions", "FPU Flags Affected", "Floating-Point Exceptions", "IA-32 Architecture Compatibility",
    "Instruction Operand Encoding1", "NOTES:", "NOTE:", NULL};
static const char* const k_end_of_operation[] = {
    "Flags Affected", "Intel C/C++ Compiler Intrinsic Equivalent", "Intel C/C++ Compiler Intrinsic Equivalents",
    "Protected Mode Exceptions", "Exceptions (All Operating Modes)", "Exceptions (All Modes of Operation)",
    "SIMD Floating-Point Exceptions", "Numeric Exceptions", "Other Exceptions", "FPU Flags Affected",
    "Real-Address Mode Exceptions", "64-Bit Mode Exceptions", "Floating-Point Exceptions",
    "IA-32 Architecture Compatibility", "Virtual-8086 Mode Exceptions", "Compatibility Mode Exceptions", NULL};

static int is_heading(const char* line, const char* const* names) {
    const char* s = skip_ws(line);
    size_t n = strlen(s), i;
    for (i = 0; names[i]; ++i) {
        size_t h = strlen(names[i]);
        if (n == h && !memcmp(s, names[i], h)) return 1;
        if (n >= h && !memcmp(s, names[i], h) && names[i][h - 1] == ':') return 1;
    }
    return 0;
}

static int trimmed_eq(const char* line, const char* what) {
    const char* s = skip_ws(line);
    return !strcmp(s, what);
}

/* the lines of section name, up to the next heading of stops: [*first, *last), or 0 when the page has none. The
   headings start in column 0, but the centred table titles (indented) */
static int section(const Line* body, size_t n, const char* name, const char* const* stops, int indented,
                   size_t* first, size_t* last) {
    size_t i, start = 0, end;
    int found = 0;
    for (i = 0; i < n; ++i) {
        if (is_page(&body[i])) continue;
        if (!found) {
            if (trimmed_eq(body[i].text, name) && (indented || !is_space(body[i].text[0]))) {
                found = 1;
                start = i + 1;
            }
            continue;
        }
        if (body[i].text[0] && !is_space(body[i].text[0]) && is_heading(body[i].text, stops)) break;
    }
    if (!found) return 0;
    end = i;
    while (end > start && (is_page(&body[end - 1]) || !*skip_ws(body[end - 1].text))) --end;
    while (start < end && (is_page(&body[start]) || !*skip_ws(body[start].text))) ++start;
    *first = start;
    *last = end;
    return 1;
}

/* the columns of a table row: runs of text apart by two spaces or more */
static int columns(const char* line, char** cols, int max) {
    int n = 0;
    const char* p = skip_ws(line);
    while (*p && n < max) {
        const char* s = p;
        while (*p && !(is_space(p[0]) && is_space(p[1]))) ++p;
        {
            size_t len = (size_t)(p - s);
            while (len && is_space(s[len - 1])) --len;
            cols[n++] = dup_n(s, len);
        }
        p = skip_ws(p);
    }
    return n;
}
static void free_cols(char** cols, int n) {
    while (n--) free(cols[n]);
}

/* a legacy opcode at the start of a row: optional NP/NFx, optional REX(.W) +, then two hex digits, VEX. or EVEX. */
static int opcode_start(const char* line) {
    const char* p = line;
    int lead = 0;
    while (*p == ' ' && lead < 7) ++p, ++lead;
    if (lead > 6) return 0;
    if (starts(p, "NP ")) p = skip_ws(p + 3);
    else if (starts(p, "NFx ")) p = skip_ws(p + 4);
    if (starts(p, "REX")) {
        p += 3;
        if (*p == '.' && (p[1] == 'W' || p[1] == 'R' || p[1] == 'w' || p[1] == 'r')) p += 2;
        p = skip_ws(p);
        if (*p == '+') p = skip_ws(p + 1);
    }
    if (starts(p, "VEX.") || starts(p, "EVEX.")) return 1;
    return is_hex(p[0]) && is_hex(p[1]) && (!is_alnum(p[2]) || p[2] == '+' || (p[2] == '+'));
}

/* "r/m8*1", "r81" or "imm81" (a footnote's mark after an 8-bit operand): the mark dropped */
static void drop_footnotes(char* s) {
    static const char* const ops[] = {"r/m8", "r8", "m8", NULL};
    size_t i;
    for (i = 0; s[i]; ++i) {
        int k;
        for (k = 0; ops[k]; ++k) {
            size_t n = strlen(ops[k]);
            if (strncmp(s + i, ops[k], n)) continue;
            {
                size_t j = i + n;
                if (s[j] == '*') ++j;
                if (is_digit(s[j]) && !is_alnum(s[j + 1])) {
                    memmove(s + i + n, s + j + 1, strlen(s + j + 1) + 1);
                    break;
                }
            }
        }
    }
}

/* ",2 1" (an operand's footnote mark inline): ", 1" */
static void drop_inline_marks(char* s) {
    size_t i;
    for (i = 0; s[i]; ++i)
        if (s[i] == ',' && is_digit(s[i + 1]) && s[i + 2] == ' ' && s[i + 3] && s[i + 3] != ' ')
            memmove(s + i + 1, s + i + 2, strlen(s + i + 2) + 1);
}

typedef struct {
    char* c[7]; /* opcode, instruction, op/en, 64-bit mode, compat/leg mode, CPUID flag, description */
} Row;

typedef struct {
    Row* rows;
    size_t n, cap;
} Rows;

static void free_row(Row* row) {
    for (int j = 0; j < 7; ++j) free(row->c[j]);
}

static void rows_add(Rows* r, Row row) {
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->rows = (Row*)realloc(r->rows, r->cap * sizeof *r->rows);
        if (!r->rows) exit(1);
    }
    r->rows[r->n++] = row;
}

static char* concat3(const char* a, const char* sep, const char* b) {
    size_t na = strlen(a), ns = strlen(sep), nb = strlen(b);
    char* s = (char*)sg_alloc(na + ns + nb + 1);
    memcpy(s, a, na);
    memcpy(s + na, sep, ns);
    memcpy(s + na + ns, b, nb);
    return s;
}

/* columns joined by a space (a description pdftotext cut in two) */
static char* join_cols(char** cols, int n) {
    Buf b = {0};
    int j;
    for (j = 0; j < n; ++j) {
        if (j) buf_str(&b, " ");
        buf_str(&b, cols[j]);
    }
    return b.s ? b.s : dup_s("");
}

/* the rows of the 'Opcode | Instruction | Op/En | 64-Bit Mode | Compat/Leg Mode | Description' tables of the
   general-purpose instructions. pdftotext puts the operands a footnote's superscript sat after on the line above
   ('            1, r81' over '00 /r    ADD r/m8    MR ...'), which is mended here */
static void opcode_rows(const Line* body, size_t n, Rows* out) {
    int in_table = 0;
    char* pending = NULL;
    size_t i;
    for (i = 0; i < n; ++i) {
        const char* line = body[i].text;
        const char* s;
        if (!line) continue;
        s = skip_ws(line);
        if (starts(s, "Opcode")) {
            in_table = 1;
            continue;
        }
        if (!in_table) continue;
        if (starts(s, "NOTES") || starts(s, "NOTE:") || strstr(s, "Instruction Operand Encoding") ||
            !strcmp(s, "Description")) {
            in_table = 0;
            continue;
        }
        if (!*s || starts(s, "En ") || !strcmp(s, "Mode") || !strcmp(s, "Leg Mode") || !strcmp(s, "En")) continue;
        if (opcode_start(line)) {
            char* cols[8];
            int k = columns(line, cols, 8);
            Row row = {{0}};
            int j;
            for (j = 0; j < 5; ++j) row.c[j] = dup_s(j < k ? cols[j] : "");
            row.c[5] = dup_s("");
            row.c[6] = join_cols(cols + (k > 5 ? 5 : k), k > 5 ? k - 5 : 0);
            drop_footnotes(row.c[1]);
            drop_inline_marks(row.c[1]);
            if (pending) {
                size_t l = strlen(row.c[1]);
                if (l && row.c[1][l - 1] == ',') {
                    const char* p = pending;
                    char* joined;
                    while (*p == ',') ++p;
                    joined = concat3(row.c[1], "", p);
                    free(row.c[1]);
                    row.c[1] = joined;
                } else if (!strchr(row.c[1], ',') && strchr(row.c[1], ' ')) {
                    char* joined = concat3(row.c[1], "", pending);
                    free(row.c[1]);
                    row.c[1] = joined;
                }
                free(pending);
                pending = NULL;
            }
            free_cols(cols, k);
            if (row.c[0][0] && row.c[1][0]) rows_add(out, row);
            else
                free_row(&row);
            continue;
        }
        free(pending);
        pending = NULL;
        {
            /* '      1, r81': six spaces or more, a digit, maybe a comma, then text */
            size_t lead = (size_t)(s - line);
            const char* p = s;
            if (lead >= 6 && is_digit(*p)) {
                while (is_digit(*p)) ++p;
                if (*p == ',') ++p;
                p = skip_ws(p);
                if (*p) {
                    char* cols[2];
                    int k = columns(line, cols, 2);
                    const char* f;
                    char* frag;
                    drop_footnotes(cols[0]);
                    f = cols[0];
                    while (is_digit(*f)) ++f;
                    if (*f == ',') ++f;
                    f = skip_ws(f);
                    frag = concat3(", ", "", f);
                    pending = frag;
                    free_cols(cols, k);
                }
            }
        }
    }
    free(pending);
}

/* 'V/N.E.' (the support in 64-bit mode / in the others): each in the legacy tables' words. 0 when it is not one */
static int mode_pair(const char* m, char** a, char** b) {
    const char* slash = strchr(m, '/');
    static const char* const ok[] = {"V", "N.E.", "N.S.", "I", "Inv.", "Inv", NULL};
    char* parts[2];
    int k, side;
    if (!slash || strchr(slash + 1, '/')) return 0;
    parts[0] = dup_n(m, (size_t)(slash - m));
    parts[1] = dup_s(slash + 1);
    for (side = 0; side < 2; ++side) {
        int good = 0;
        for (k = 0; ok[k]; ++k)
            if (!strcmp(parts[side], ok[k])) good = 1;
        if (!good) {
            free(parts[0]);
            free(parts[1]);
            return 0;
        }
        if (!strcmp(parts[side], "V")) {
            free(parts[side]);
            parts[side] = dup_s("Valid");
        } else if (parts[side][0] == 'I') {
            free(parts[side]);
            parts[side] = dup_s("Invalid");
        }
    }
    *a = parts[0];
    *b = parts[1];
    return 1;
}

/* the CPUID column of a stacked row: its flags, up to a word with a lowercase letter, where pdftotext left one space
   before the description ('(AVX512VL AND Return the bitwise...'); the flags are capitals, digits, '.', '_' */
static char* cpuid_flag(const char* col, const char** rest) {
    const char* p = col;
    const char* end = col;
    *rest = col + strlen(col);
    while (*p) {
        const char* w = p;
        int lower = 0;
        while (*p && *p != ' ') lower |= (*p >= 'a' && *p <= 'z'), ++p;
        if (lower) {
            *rest = w;
            break;
        }
        end = p;
        while (*p == ' ') ++p;
    }
    return dup_n(col, (size_t)(end - col));
}

/* the 'Opcode/Instruction | Op/En | 64/32 bit Mode Support | CPUID Feature Flag | Description' tables of the
   SSE, AVX and BMI pages: the instruction is on the line(s) under its opcode */
static void opcode_rows_stacked(const Line* body, size_t n, Rows* out) {
    int in_table = 0;
    Row* cur = NULL;
    size_t i, first = out->n;
    for (i = 0; i < n; ++i) {
        const char* line = body[i].text;
        const char* s;
        if (!line) continue;
        s = skip_ws(line);
        if (starts(s, "Opcode") && (s[6] == '/' || (s[6] == ' ' && s[7] == '/') || (s[6] == '*' && s[7] == '/') ||
                                    (s[6] == ' ' && s[7] == '*'))) {
            in_table = 1;
            cur = NULL;
            continue;
        }
        if (!in_table) continue;
        if (starts(s, "NOTES") || starts(s, "NOTE:") || strstr(s, "Instruction Operand Encoding") ||
            !strcmp(s, "Description")) {
            in_table = 0;
            continue;
        }
        if (!*s || starts(s, "Instruction") || !strcmp(s, "Support") || !strcmp(s, "Flag") || !strcmp(s, "Mode"))
            continue;
        {
            /* opcode, two spaces, Op/En (a capital and up to five more), spaces, the mode pair, then the rest */
            char* cols[8];
            int k = columns(line, cols, 8), matched = 0;
            size_t lead = (size_t)(s - line);
            if (lead <= 6 && k >= 2 && (is_hex(s[0]) || starts(s, "REX") || starts(s, "VEX.") ||
                                        starts(s, "EVEX.") || starts(s, "MAP") || starts(s, "NP") ||
                                        starts(s, "NFx"))) {
                /* the Op/En and mode may share a column ('A       V/V' splits; 'RVM    V/N.E.' too) */
                int c = 1;
                char *open = NULL, *mode = NULL;
                char* one = cols[1];
                const char* sp = strchr(one, ' ');
                if (sp) {
                    open = dup_n(one, (size_t)(sp - one));
                    mode = dup_s(skip_ws(sp));
                    c = 2;
                } else if (k >= 3) {
                    open = dup_s(cols[1]);
                    mode = dup_s(cols[2]);
                    c = 3;
                }
                if (open && mode && is_upper(open[0]) && strlen(open) <= 6) {
                    char *a, *b;
                    if (mode_pair(mode, &a, &b)) {
                        Row row;
                        row.c[0] = dup_s(cols[0]);
                        row.c[1] = dup_s("");
                        row.c[2] = dup_s(open);
                        row.c[3] = a;
                        row.c[4] = b;
                        {
                            const char* rest;
                            row.c[5] = cpuid_flag(c < k ? cols[c] : "", &rest);
                            if (*rest) {
                                char* more = join_cols(cols + c + 1, c + 1 < k ? k - c - 1 : 0);
                                row.c[6] = *more ? concat3(rest, " ", more) : dup_s(rest);
                                free(more);
                            } else
                                row.c[6] = join_cols(cols + c + 1, c + 1 < k ? k - c - 1 : 0);
                        }
                        rows_add(out, row);
                        cur = &out->rows[out->n - 1];
                        matched = 1;
                    }
                }
                free(open);
                free(mode);
            }
            if (!matched && cur && lead <= 3) {
                char* frag = cols[0];
                char* joined = cur->c[1][0] ? concat3(cur->c[1], " ", frag) : dup_s(frag);
                free(cur->c[1]);
                cur->c[1] = joined;
            }
            free_cols(cols, k);
        }
    }
    /* "xmm1,xmm2" -> "xmm1, xmm2"; " ," -> "," */
    for (i = first; i < out->n; ++i) {
        Buf b = {0};
        const char* p = out->rows[i].c[1];
        for (; *p; ++p) {
            if (*p == ' ' && p[1] == ',') continue;
            buf_add(&b, p, 1);
            if (*p == ',' && p[1] && p[1] != ' ') buf_add(&b, " ", 1);
        }
        free(out->rows[i].c[1]);
        out->rows[i].c[1] = b.s ? b.s : dup_s("");
    }
}

static int has_stacked_header(const Line* body, size_t n) {
    size_t i;
    for (i = 0; i < n; ++i) {
        const char* s;
        if (!body[i].text) continue;
        s = skip_ws(body[i].text);
        if (starts(s, "Opcode") && (s[6] == '/' || (s[6] == ' ' && s[7] == '/') || (s[6] == '*' && s[7] == '/') ||
                                    (s[6] == ' ' && s[7] == '*')))
            return 1;
    }
    return 0;
}

/* a row's columns, up to its last that isn't empty (the CPUID flag, then the description's first line) */
static void write_row(Buf* o, const Row* r, int cols) {
    int j;
    while (cols > 5 && !r->c[cols - 1][0]) --cols;
    for (j = 0; j < cols; ++j) {
        if (j) buf_str(o, " | ");
        buf_str(o, r->c[j]);
    }
    buf_str(o, "\n");
}

/* ---- overrides ---- */

typedef struct {
    char* name;
    char* section[4]; /* opcodes, encoding, operation, flags: the text that replaces the page's, or NULL */
    const char* path;
    int used;
} Override;

static const char* const k_sections[4] = {"opcodes", "encoding", "operation", "flags"};

/* an instruction's name: its title before the dash */
static char* name_of(const char* title) {
    const char* d = strstr(title, DASH);
    size_t n = d ? (size_t)(d - title) : strlen(title);
    while (n && title[n - 1] == ' ') --n;
    return dup_n(title, n);
}

static Override read_override(const char* path) {
    size_t size, i = 0;
    char* text = sg_read(path, &size);
    Override ov = {0};
    Buf cur = {0};
    int sec = -1;
    ov.path = path;
    while (i < size) {
        size_t s = i;
        char* line;
        while (i < size && text[i] != '\n') ++i;
        line = dup_n(text + s, i - s);
        ++i;
        if (line[0] == '#' && sec < 0) {
            if (!ov.name && line[1] == ' ') ov.name = name_of(line + 2);
        } else if (line[0] == '@') {
            int k, found = -1;
            for (k = 0; k < 4; ++k)
                if (!strcmp(skip_ws(line + 1), k_sections[k])) found = k;
            if (found < 0) {
                fprintf(stderr, "%s: unknown section %s\n", path, line);
                exit(1);
            }
            if (sec >= 0) ov.section[sec] = cur.s ? cur.s : dup_s("");
            cur = (Buf){0};
            sec = found;
        } else if (sec >= 0 && !(line[0] == '#' && sec <= 1)) {
            buf_str(&cur, line);
            buf_str(&cur, "\n");
        }
        free(line);
    }
    if (sec >= 0) ov.section[sec] = cur.s ? cur.s : dup_s("");
    if (!ov.name) {
        fprintf(stderr, "%s: an override starts with '# NAME—Title' naming its page\n", path);
        exit(1);
    }
    free(text);
    return ov;
}

static void free_override(Override* ov) {
    free(ov->name);
    for (int k = 0; k < 4; ++k) free(ov->section[k]);
}

/* ---- the whole ---- */

int sg_sdm_extract(const char* text_path, const char* out_path, const char* const* override_paths,
                   int override_count) {
    size_t size, count, i, k, pages = 0;
    char revision[16];
    char* text = sg_read(text_path, &size);
    Line* lines = read_lines(text, size, &count, revision);
    size_t* starts_at = (size_t*)sg_alloc((count + 1) * sizeof *starts_at);
    size_t nstarts = 0;
    char** seen = (char**)sg_alloc((count + 1) * sizeof *seen);
    size_t nseen = 0;
    Override* ovs = (Override*)sg_alloc((size_t)(override_count + 1) * sizeof *ovs);
    Buf o = {0};
    int status = 0;
    for (k = 0; k < (size_t)override_count; ++k) ovs[k] = read_override(override_paths[k]);
    if (revision[0] == '?') fprintf(stderr, "sailgen --sdm: %s has no 'Order Number: 325383-'\n", text_path);
    /* a page starts at a 'NAME—Title' line with an opcode table's header a few lines below */
    for (i = 0; i < count; ++i) {
        size_t j;
        if (is_page(&lines[i]) || !lines[i].text[0] || is_space(lines[i].text[0]) || !header_line(lines[i].text))
            continue;
        for (j = i + 1; j < count && j < i + 9; ++j)
            if (lines[j].text && starts(skip_ws(lines[j].text), "Opcode")) {
                starts_at[nstarts++] = i;
                break;
            }
    }
    for (k = 0; k < nstarts; ++k) {
        size_t s = starts_at[k], e = k + 1 < nstarts ? starts_at[k + 1] : count, body_n = e - s - 1, first, last;
        const Line* body = lines + s + 1;
        const char* title = skip_ws(lines[s].text);
        const char* page = NULL;
        char* name = name_of(title);
        char* key;
        Override* ov = NULL;
        size_t j;
        Rows rows = {0};
        int dup = 0;
        for (j = 0; j < body_n; ++j)
            if (is_page(&body[j])) {
                page = body[j].page;
                break;
            }
        /* a second page of the same name (MOV to or from the control registers, the string MOVSD beside SSE2's):
           named by its title */
        for (j = 0; j < nseen; ++j)
            if (!strcmp(seen[j], name)) dup = 1;
        if (dup) {
            Buf t = {0};
            const char* p;
            for (p = title; *p; ++p) {
                if (starts(p, DASH)) {
                    buf_str(&t, " ");
                    p += 2;
                } else
                    buf_add(&t, p, 1);
            }
            key = t.s;
            for (j = 0; j < nseen; ++j)
                if (!strcmp(seen[j], key)) dup = 2;
            if (dup == 2) {
                free(key);
                free(name);
                continue;
            }
        } else
            key = dup_s(name);
        seen[nseen++] = key;
        for (j = 0; j < (size_t)override_count; ++j)
            if (!strcmp(ovs[j].name, key)) ov = &ovs[j];
        if (ov) ov->used = 1;
        buf_str(&o, "# ");
        buf_str(&o, title);
        buf_str(&o, " -- Intel SDM 325383-");
        buf_str(&o, revision);
        buf_str(&o, ", ");
        buf_str(&o, page ? page : "?");
        buf_str(&o, "\n@opcodes\n");
        if (ov && ov->section[0]) {
            buf_str(&o, ov->section[0]);
        } else {
            opcode_rows(body, body_n, &rows);
            if (has_stacked_header(body, body_n)) {
                /* a stacked table's opcode lines read as legacy rows only where their mode column is one */
                Rows keep = {0};
                for (j = 0; j < rows.n; ++j) {
                    const char* m = rows.rows[j].c[3];
                    if (rows.rows[j].c[1][0] && (!strcmp(m, "Valid") || !strcmp(m, "Invalid") || !strcmp(m, "N.E.") ||
                                                 !strcmp(m, "N.S.") || !strcmp(m, "Inv.")))
                        rows_add(&keep, rows.rows[j]);
                    else
                        free_row(&rows.rows[j]);
                }
                free(rows.rows);
                rows = keep;
                opcode_rows_stacked(body, body_n, &rows);
            }
            for (j = 0; j < rows.n; ++j)
                if (rows.rows[j].c[1][0]) write_row(&o, &rows.rows[j], 7);
        }
        buf_str(&o, "@encoding\n");
        if (ov && ov->section[1]) {
            buf_str(&o, ov->section[1]);
        } else if (section(body, body_n, "Instruction Operand Encoding", k_headings, 1, &first, &last) ||
                   section(body, body_n, "Instruction Operand Encoding1", k_headings, 1, &first, &last)) {
            for (j = first; j < last; ++j) {
                const char* s;
                char* cols[16];
                int c, q;
                if (!body[j].text) continue;
                s = skip_ws(body[j].text);
                if (!*s || starts(s, "Op/En") || starts(s, "Op / En")) continue;
                c = columns(body[j].text, cols, 16);
                if (c >= 2) {
                    for (q = 0; q < c; ++q) {
                        if (q) buf_str(&o, " | ");
                        buf_str(&o, cols[q]);
                    }
                    buf_str(&o, "\n");
                }
                free_cols(cols, c);
            }
        }
        buf_str(&o, "@operation\n");
        if (ov && ov->section[2]) {
            buf_str(&o, ov->section[2]);
        } else if (section(body, body_n, "Operation", k_end_of_operation, 0, &first, &last)) {
            for (j = first; j < last; ++j)
                if (body[j].text) {
                    buf_str(&o, body[j].text);
                    buf_str(&o, "\n");
                }
        }
        buf_str(&o, "@flags\n");
        if (ov && ov->section[3]) {
            buf_str(&o, ov->section[3]);
        } else if (section(body, body_n, "Flags Affected", k_headings, 0, &first, &last)) {
            for (j = first; j < last; ++j)
                if (body[j].text) {
                    buf_str(&o, skip_ws(body[j].text));
                    buf_str(&o, "\n");
                }
        }
        for (j = 0; j < rows.n; ++j) free_row(&rows.rows[j]);
        free(rows.rows);
        free(name);
        ++pages;
    }
    for (k = 0; k < (size_t)override_count; ++k)
        if (!ovs[k].used) {
            fprintf(stderr, "%s: the manual has no page %s\n", ovs[k].path, ovs[k].name);
            status = 1;
        }
    if (!pages) {
        fprintf(stderr, "sailgen --sdm: %s has no instruction pages (is it `pdftotext -layout` of SDM Vol. 2?)\n",
                text_path);
        status = 1;
    }
    if (!status) {
        sg_out("%s", o.s ? o.s : "");
        status = sg_flush(out_path);
        if (!status) fprintf(stderr, "sailgen --sdm: %zu instruction pages from 325383-%s\n", pages, revision);
    }
    free(o.s);
    for (k = 0; k < (size_t)override_count; ++k) free_override(&ovs[k]);
    free(ovs);
    for (k = 0; k < nseen; ++k) free(seen[k]);
    free(seen);
    free(starts_at);
    free(lines);
    free(text);
    return status;
}
