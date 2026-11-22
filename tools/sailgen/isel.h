/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The x86-64 back end's instruction selection (--isel): the SDM's pseudocode parsed (sdm_code.c), evaluated to
   bit-vector terms in a canonical form (term.c), and matched against the back end's primitives (isel.c). README.md,
   "The x86-64 back end", says how */
#ifndef SNX_TOOLS_SAILGEN_ISEL_H
#define SNX_TOOLS_SAILGEN_ISEL_H

#include <stddef.h>

typedef unsigned long long XU64;

/* ---- the pseudocode's syntax tree (sdm_code.c) ---- */

enum {
    X_NONE,
    /* expressions */
    X_NUM,   /* value; width: the digits' width for a hexadecimal or binary literal (0: an integer) */
    X_NAME,  /* text: a name, dotted as the manual dots them (CPUID.80000001H:ECX.LZCNT is a name) */
    X_BIN,   /* text: the operator (+ - * / MOD AND OR XOR = != < > <= >= << >> ...); a, b */
    X_UN,    /* text: - or NOT; a */
    X_CALL,  /* text: the function; a: the first argument (a list) */
    X_SLICE, /* a[b:c] */
    X_INDEX, /* a[b] */
    X_PROSE, /* text: what isn't an expression, as words */
    /* statements: a list each, by next */
    X_ASSIGN, /* a := b */
    X_IF,     /* IF a THEN b ELSE c (c 0: none) */
    X_WHILE,  /* WHILE a DO b OD */
    X_FOR,    /* FOR text := a TO/DOWNTO b DO c (down) */
    X_RAISE,  /* #text(a) */
    X_DO,     /* a call or prose as a statement: a */
    X_CASE,   /* CASE a OF arms (b: the first X_ARM) ESAC */
    X_ARM     /* a: the label (0: DEFAULT); b: the statements */
};

typedef struct {
    unsigned char kind;
    unsigned char down;  /* X_FOR: DOWNTO */
    unsigned char sign;  /* X_BIN '/': 1 a signed divide rounding toward negative infinity, 2 an unsigned one, as
                            the line's comment says (SAR's, SHR's); 0 unsaid */
    unsigned short width;
    const char* text;
    unsigned a, b, c;
    unsigned next;
    unsigned line;
    XU64 value;
} XNode;

typedef struct {
    XNode* nodes;
    unsigned count, cap;
    char** names; /* the copies of the source's names the nodes' text points at, freed with the code */
    unsigned nnames, names_cap;
    const char* error; /* the first place the text could not be read, or NULL */
    unsigned error_line;
} XCode;

/* parses an Operation section (or the back end's primitives, in the same notation) into code; the first statement's
   index, 0 for none. Never fails: what isn't an expression is prose, which the evaluator refuses */
unsigned x_parse(XCode* code, const char* text);
void x_code_free(XCode* code);
#include <stdio.h>
/* the tree, for a report */
void x_dump(const XCode* code, unsigned first, int depth, FILE* f);

#endif
