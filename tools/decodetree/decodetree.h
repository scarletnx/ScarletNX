/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from QEMU's scripts/decodetree.py, LGPL-2.1-or-later:
   Copyright (c) 2018 Linaro Limited */
/* decodetree's parse of the .decode files, which its two outputs read: decodetree.c's QEMU-style decoder and
   a64.c's A64 tables */
#ifndef SNX_TOOLS_DECODETREE_H
#define SNX_TOOLS_DECODETREE_H

#define NAME_LEN 64
#define MAX_TOK  128
#define MAX_SUBS 16
#define MAX_FLDS 64

typedef unsigned long long u64;

/* a field: bits of the word (F_FIELD), parts joined (F_MULTI), a constant, a function of a field (F_FUNC) or of
   nothing (F_PARAM), or another argument's value (F_NAMED) */
enum { F_FIELD, F_MULTI, F_CONST, F_FUNC, F_PARAM, F_NAMED };

typedef struct {
    int kind, sign, pos, len;
    long long value;
    u64 mask;
    char func[NAME_LEN], name[NAME_LEN];
    int base;
    int subs[MAX_SUBS], nsubs;
} FNode;

/* an ordered map of names to fields */
typedef struct {
    char name[MAX_FLDS][NAME_LEN];
    int node[MAX_FLDS];
    int n;
} Flds;

/* an argument set */
typedef struct {
    char name[NAME_LEN];
    char fields[MAX_FLDS][NAME_LEN];
    char types[MAX_FLDS][NAME_LEN];
    int nfields, extern_;
} Args;

/* a format, or a pattern */
typedef struct {
    char name[NAME_LEN];
    const char* file;
    int lineno;
    int base; /* a format: its argument set; a pattern: its format */
    u64 fixedbits, fixedmask, undefmask, fieldmask;
    Flds fields;
    Flds all; /* a pattern: every field, its format's too, in the order the line gives them */
    u64 ignmask;
    int width;
    int dangling_done;
    char dangling[MAX_FLDS][NAME_LEN];
    int ndangling;
} General;

/* the pattern tree: a pattern, a { } group, a [ ] group (the top level is one), or a node of a [ ] group's tree */
enum { N_PAT, N_INC, N_EXC, N_TREE };

typedef struct {
    int kind;
    const char* file;
    int lineno;
    int pat;
    int* kids;
    u64* bits;
    int nkids, ckids;
    u64 fixedbits, fixedmask, undefmask;
    u64 thismask;
    int tree;
    int base;
    int width;
    int parent;
} Node;

extern FNode* fnodes;
extern int nfnodes;
extern Flds* gfields;
extern Args* args;
extern int nargs;
extern General* formats;
extern int nformats;
extern General* pats;
extern int npats;
extern Node* nodes;
extern int nnodes;
extern const char* input_file;

void error_with_file(const char* file, int lineno, const char* fmt, ...);
#define error(lineno, ...) error_with_file(input_file, lineno, __VA_ARGS__)
int flds_find(const Flds* f, const char* name);
int args_has(const Args* a, const char* name);

/* a64.c: the A64 tables from the parse (the patterns in order, their groups) and the profile */
int a64_generate(const char* profile, const char* header, const char* decoder, int stats, const char* const* files,
                 int nfiles);

#endif
