/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen --extract: the definitions of sail-arm's files that a list of roots reaches, copied as they are, in the
   order of the files, so the tree holds only what its generated code reads and an update is the same command on a
   newer sail-arm. It splits the files at their top-level definitions (column 0) without parsing the bodies, which
   parse.c only has to read once they are extracted, and follows every name a definition's text mentions.

   The roots file, one directive per line (# starts a comment):
     root NAME     a function to start from: its val, its definition, and every decode clause that calls it
     stop NAME     a name not followed and not copied: the state the generator maps (_R, SP_EL0, PSTATE...)
                   and what it lowers itself (gen.c's builtins) */
#include "sailgen.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned file;     /* index in the inputs */
    size_t start, end; /* its text */
    unsigned name;     /* string offset */
    int clause;        /* a function clause: name is the scattered function's */
    int enum_def;      /* an enum: its members are names too */
    int keep;
} Piece;

static struct {
    Piece* pieces;
    unsigned count, cap;
    char** texts;
    size_t* sizes;
    unsigned* roots;
    unsigned root_count;
    unsigned* stops;
    unsigned stop_count;
    unsigned* queue;
    unsigned queue_count;
} g_extract;

static const char* const top_words[] = {
    "function", "val",           "enum",    "overload", "register",  "let",
    "type",     "struct",        "union",   "bitfield", "mapping",   "infix",
    "infixl",   "infixr",        "default", "end",      "scattered", "termination_measure",
    "$include", "$ifdef",        "$ifndef", "$endif",   "$define",   "$else",
    "$option",  "$sail_internal"};

static size_t word_at(const char* s, size_t i, size_t n) {
    size_t e = i;
    while (e < n && (isalnum((unsigned char)s[e]) || s[e] == '_' || s[e] == '$' || s[e] == '#' || s[e] == '\'')) ++e;
    return e - i;
}

static int is_top_word(const char* s, size_t length) {
    size_t i;
    for (i = 0; i < sizeof top_words / sizeof *top_words; ++i)
        if (strlen(top_words[i]) == length && !memcmp(top_words[i], s, length)) return 1;
    return 0;
}

static void piece_add(unsigned file, size_t start, const char* s, size_t n) {
    Piece* p;
    size_t i = start, w;
    if (g_extract.count == g_extract.cap) {
        g_extract.cap = g_extract.cap ? g_extract.cap * 2 : 4096;
        g_extract.pieces = (Piece*)realloc(g_extract.pieces, sizeof(Piece) * g_extract.cap);
        if (!g_extract.pieces) {
            fprintf(stderr, "sailgen: out of memory for the pieces\n");
            exit(1);
        }
    }
    p = &g_extract.pieces[g_extract.count++];
    memset(p, 0, sizeof *p);
    p->file = file;
    p->start = start;
    w = word_at(s, i, n);
    p->enum_def = w == 4 && !memcmp(s + i, "enum", 4);
    i += w;
    while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
    w = word_at(s, i, n);
    if ((w == 6 && !memcmp(s + i, "clause", 6)) || (w == 8 && !memcmp(s + i, "operator", 8))) {
        p->clause = s[i] == 'c';
        i += w;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) ++i;
        w = word_at(s, i, n);
        if (!w) { /* overload operator ==: the operator's characters */
            while (i + w < n && !isspace((unsigned char)s[i + w])) ++w;
        }
    }
    p->name = sg_intern(s + i, w);
}

/* the input split at its top-level definitions: each runs to the next one */
static void split(unsigned file) {
    const char* s = g_extract.texts[file];
    size_t n = g_extract.sizes[file], line = 0;
    unsigned first = g_extract.count;
    while (line < n) {
        size_t w = word_at(s, line, n), next;
        if (w && is_top_word(s + line, w)) {
            if (g_extract.count > first) g_extract.pieces[g_extract.count - 1].end = line;
            piece_add(file, line, s, n);
            g_extract.pieces[g_extract.count - 1].end = n;
        }
        next = line;
        while (next < n && s[next] != '\n') ++next;
        line = next + 1;
    }
}

static int listed(const unsigned* names, unsigned count, const char* s, size_t length) {
    unsigned i;
    for (i = 0; i < count; ++i)
        if (strlen(sg_str(names[i])) == length && !memcmp(sg_str(names[i]), s, length)) return 1;
    return 0;
}

static void keep(unsigned index) {
    if (g_extract.pieces[index].keep) return;
    g_extract.pieces[index].keep = 1;
    g_extract.queue[g_extract.queue_count++] = index;
}

/* every piece a name refers to: those it names, and the enum it is a member of */
static void follow_name(const char* s, size_t length) {
    unsigned i;
    if (listed(g_extract.stops, g_extract.stop_count, s, length)) return;
    for (i = 0; i < g_extract.count; ++i) {
        const Piece* p = &g_extract.pieces[i];
        const char* name = sg_str(p->name);
        if (p->clause) continue;
        if (strlen(name) == length && !memcmp(name, s, length))
            keep(i);
        else if (p->enum_def) {
            /* a member: the enum's text after its "=" holds the name as a word */
            const char* t = g_extract.texts[p->file] + p->start;
            size_t k, n = p->end - p->start;
            const char* eq = (const char*)memchr(t, '=', n);
            if (!eq) continue;
            n -= (size_t)(eq + 1 - t);
            t = eq + 1;
            for (k = 0; k + length <= n; ++k)
                if (!memcmp(t + k, s, length) && (k == 0 || !(isalnum((unsigned char)t[k - 1]) || t[k - 1] == '_')) &&
                    (k + length == n || !(isalnum((unsigned char)t[k + length]) || t[k + length] == '_'))) {
                    keep(i);
                    break;
                }
        }
    }
}

static void follow(unsigned index) {
    const Piece* p = &g_extract.pieces[index];
    const char* s = g_extract.texts[p->file];
    size_t i = p->start, w;
    /* the definition's own name is not followed: its other definitions (a val, overloads) are */
    while (i < p->end) {
        if (s[i] == '/' && i + 1 < p->end && s[i + 1] == '/') {
            while (i < p->end && s[i] != '\n') ++i;
            continue;
        }
        if (s[i] == '"') {
            ++i;
            while (i < p->end && s[i] != '"') ++i;
            ++i;
            continue;
        }
        if (isalpha((unsigned char)s[i]) || s[i] == '_') {
            w = word_at(s, i, p->end);
            if (!(i > 0 && (isalnum((unsigned char)s[i - 1]) || s[i - 1] == '\''))) follow_name(s + i, w);
            i += w;
            continue;
        }
        ++i;
    }
}

int sg_extract(const char* roots_path, const char* out_path, const char* const* inputs, int input_count) {
    size_t size, i;
    char* roots = sg_read(roots_path, &size);
    unsigned k, f, line_no = 0;
    g_extract.texts = (char**)sg_alloc(sizeof(char*) * (size_t)input_count);
    g_extract.sizes = (size_t*)sg_alloc(sizeof(size_t) * (size_t)input_count);
    g_extract.roots = (unsigned*)sg_alloc(sizeof(unsigned) * (size + 1));
    g_extract.stops = (unsigned*)sg_alloc(sizeof(unsigned) * (size + 1));
    for (f = 0; f < (unsigned)input_count; ++f) {
        g_extract.texts[f] = sg_read(inputs[f], &g_extract.sizes[f]);
        split(f);
    }
    g_extract.queue = (unsigned*)sg_alloc(sizeof(unsigned) * (g_extract.count + 1));
    for (i = 0; i < size;) {
        size_t e = i, w;
        char word[16];
        ++line_no;
        while (e < size && roots[e] != '\n') ++e;
        while (i < e && isspace((unsigned char)roots[i])) ++i;
        if (i < e && roots[i] != '#') {
            w = word_at(roots, i, e);
            snprintf(word, sizeof word, "%.*s", (int)(w < 15 ? w : 15), roots + i);
            i += w;
            while (i < e && isspace((unsigned char)roots[i])) ++i;
            w = word_at(roots, i, e);
            if (!w || (strcmp(word, "root") && strcmp(word, "stop"))) {
                fprintf(stderr, "%s:%u: error: expected \"root NAME\" or \"stop NAME\", found \"%.*s\"\n", roots_path,
                        line_no, (int)(e - i + w), roots + i - w);
                free(roots);
                return 1;
            }
            if (word[0] == 'r')
                g_extract.roots[g_extract.root_count++] = sg_intern(roots + i, w);
            else
                g_extract.stops[g_extract.stop_count++] = sg_intern(roots + i, w);
        }
        i = e + 1;
    }
    free(roots); /* its names are interned */
    for (k = 0; k < g_extract.root_count; ++k) {
        const char* root = sg_str(g_extract.roots[k]);
        size_t length = strlen(root);
        unsigned found = 0;
        for (i = 0; i < g_extract.count; ++i) {
            const Piece* p = &g_extract.pieces[i];
            if (!p->clause && !strcmp(sg_str(p->name), root)) {
                keep((unsigned)i);
                found = 1;
            }
            if (p->clause) { /* the clause that calls it */
                const char* t = g_extract.texts[p->file] + p->start;
                size_t j, n = p->end - p->start;
                for (j = 0; j + length < n; ++j)
                    if (!memcmp(t + j, root, length) && t[j + length] == '(' &&
                        (j == 0 || !(isalnum((unsigned char)t[j - 1]) || t[j - 1] == '_'))) {
                        keep((unsigned)i);
                        break;
                    }
            }
        }
        if (!found) {
            fprintf(stderr, "%s: error: root %s is defined in none of the inputs\n", roots_path, root);
            return 1;
        }
    }
    for (k = 0; k < g_extract.queue_count; ++k) follow(g_extract.queue[k]);
    {
        const char* base = strrchr(roots_path, '/');
        sg_out("/* sail-arm's definitions that %s reaches (sailgen --extract), as they are in:\n",
               base ? base + 1 : roots_path);
    }
    for (f = 0; f < (unsigned)input_count; ++f) {
        const char* base = strrchr(inputs[f], '/');
        sg_out("     %s\n", base ? base + 1 : inputs[f]);
    }
    sg_out("   BSD 3-Clause Clear License: LICENSE beside this file. Do not edit: an NX fixup goes in nx.sail */\n\n");
    for (i = 0; i < g_extract.count; ++i) {
        const Piece* p = &g_extract.pieces[i];
        size_t end = p->end;
        if (!p->keep) continue;
        while (end > p->start && isspace((unsigned char)g_extract.texts[p->file][end - 1])) --end;
        sg_out("%.*s\n\n", (int)(end - p->start), g_extract.texts[p->file] + p->start);
    }
    return sg_flush(out_path);
}
