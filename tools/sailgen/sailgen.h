/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sailgen's parse of the Sail it reads (parse.c), which its two jobs share: --extract (extract.c) copies definitions
   out of sail-arm's files, and the generator (gen.c, with the LLVM back end in llvm.c) writes C that emits each
   instruction's IR. Nodes, definitions and names each live in one array, linked by index; index 0 is none */
#ifndef SNX_TOOLS_SAILGEN_H
#define SNX_TOOLS_SAILGEN_H

#include <stddef.h>

typedef unsigned long long SgU64;

/* expression and statement nodes. A block's items are a list (first child, then each node's next) */
enum {
    N_NONE,
    N_ID,      /* name: a variable, an enum member, a constant ('x is the variable x) */
    N_NUM,     /* an integer literal: value */
    N_BITS,    /* 0b... or 0x...: value, width */
    N_BOOL,    /* true or false: value */
    N_UNDEF,   /* undefined */
    N_UNIT,    /* () */
    N_CALL,    /* name(args): a = first argument */
    N_BINOP,   /* a op b: op is the operator's text */
    N_NOT,     /* not, of a bool: a (Sail's not_bool is a call) */
    N_NEG,     /* -a */
    N_SLICE,   /* a[b .. c] */
    N_INDEX,   /* a[b]: one bit */
    N_VEC,     /* [a, ...]: a bit vector of its elements, the first the most significant */
    N_IF,      /* if a then b else c (c 0: unit) */
    N_FIELD,   /* a.name */
    N_TUPLE,   /* (a, ...) */
    N_TYPED,   /* a : type (type is the type's node, kept as text) */
    N_BLOCK,   /* { a; ... } */
    N_LET,     /* let pattern a = b: in a block, scoping over the items after it; with c, let ... in c */
    N_VAR,     /* name : type = b, a mutable local; type kept in c */
    N_ASSIGN,  /* a = b */
    N_MATCH,   /* match a { cases }: b = first case */
    N_CASE,    /* pattern a => b; c = guard */
    N_FOREACH, /* foreach (name from a to b by c in dec?) d */
    N_RETURN,  /* return(a) */
    N_THROW,   /* throw(a) */
    N_ASSERT,  /* assert(...): the arguments are skipped */
    N_WILD,    /* _ in a pattern */
    N_TYPE,    /* a type, as its source text (name) */
    N_UPDATE,  /* [a with b = d], or [a with b .. c = d]: bits a with bit b (bits b to c) made d */
    N_COUNT
};

typedef struct {
    unsigned char kind;
    unsigned char dec;    /* N_FOREACH: counts down */
    unsigned short width; /* N_BITS */
    unsigned name;        /* string offset: N_ID, N_CALL, N_FIELD, N_BINOP (the operator), N_VAR, N_FOREACH */
    unsigned a, b, c, d;  /* children */
    unsigned next;        /* the next item of a list */
    unsigned line;        /* where it starts, in its file */
    unsigned file;        /* the file's name (string offset) */
    SgU64 value;          /* N_NUM, N_BITS, N_BOOL */
} SgNode;

/* top-level definitions */
enum { D_FUNCTION, D_CLAUSE, D_VAL, D_ENUM, D_OVERLOAD, D_LET, D_REGISTER, D_STRUCT, D_OTHER };

/* the kind of a type, enough to pick the C type of a value of it. K_VECTOR is a vector of bits values (a register
   file): the type's node has its length as child a and its element's type as child b. A struct's name is K_ENUM, as
   every type name sailgen doesn't know, and the generator looks it up */
enum { K_NONE, K_BOOL, K_INT, K_BITS, K_BIT, K_UNIT, K_ENUM, K_TUPLE, K_REAL, K_VECTOR, K_OTHER };

typedef struct {
    unsigned char kind;
    unsigned name;        /* string offset */
    unsigned params;      /* D_FUNCTION, D_CLAUSE: the first parameter pattern (a list); D_ENUM: the first member
                               (an N_ID list); D_OVERLOAD: the first name (an N_ID list); D_REGISTER: its type's
                               node; D_STRUCT: the first field (an N_TYPED list: a its name, b its type) */
    unsigned body;        /* D_FUNCTION, D_CLAUSE, D_LET: the body or value */
    unsigned guard;       /* D_CLAUSE: the if guard */
    unsigned ret_kind;    /* D_VAL: the kind its return type has; D_LET, D_REGISTER: the kind of the value */
    unsigned param_kinds; /* D_VAL: the kinds of its parameters, one byte each, from the low byte (4 at most) */
    unsigned param_count; /* D_VAL */
    unsigned file;        /* string offset */
    unsigned line;
    size_t text_start, text_end; /* its text in its file, comments before it included (--extract copies it) */
    unsigned source;             /* the file it came from (index in the parse's files) */
} SgDef;

typedef struct {
    const char* path;
    char* text;
    size_t size;
    int overlay; /* a later file's definitions take the place of an earlier one's of the same name */
} SgFile;

typedef struct {
    int kind;
    size_t start, length;
    unsigned line, column;
} SgToken;

/* the parse: everything read, in arrays indexed from 1, and the lexer's place in the file being read */
typedef struct {
    SgNode* nodes;
    unsigned node_count, node_cap;
    SgDef* defs;
    unsigned def_count, def_cap;
    SgFile files[16];
    unsigned file_count;
    struct {
        const SgFile* file;
        unsigned file_name;
        size_t pos;
        unsigned line, column;
        SgToken tok, ahead;
        int have_ahead;
    } lex;
} SgParse;

extern SgParse g_parse;

/* the text of a string offset */
const char* sg_str(unsigned offset);
unsigned sg_intern(const char* text, size_t length);
/* reads and parses one file into g_parse; exits with file:line and the reason on an error */
void sg_parse_file(const char* path, int overlay);
/* the definition of name of this kind that counts (an overlay's, else the first), 0 for none */
unsigned sg_find(const char* name, int kind);
/* every definition of name and kind, in order: the first after index after (0 to start), 0 when none is left */
unsigned sg_find_next(const char* name, int kind, unsigned after);
void sg_fail(unsigned node, const char* fmt, ...);
void* sg_alloc(size_t size);
char* sg_read(const char* path, size_t* size);

/* --extract: the definitions roots reach, copied into out (extract.c) */
int sg_extract(const char* roots_path, const char* out_path, const char* const* inputs, int input_count);
/* the generator: C for the decode functions roots names (gen.c), with the back end's files copied at its top */
int sg_generate(const char* roots_path, const char* const* backend_paths, int backend_count, const char* profile_path,
                const char* out_path);
/* GPU mode (gpu.c): C for the roots' instructions and fusions of a GPU's ISA, apart from the CPUs' generator */
int sg_gpu_generate(const char* roots_path, const char* const* backend_paths, int backend_count, const char* out_path);
/* --sdm: the instruction pages of Intel's SDM Vol. 2 out of pdftotext's text of it, with the overrides (sdm.c) */
int sg_sdm_extract(const char* text_path, const char* out_path, const char* const* override_paths,
                   int override_count);
/* --isel: the x86-64 back end's instruction selection, from the SDM's pages and the back end's primitives (isel.c) */
int sg_isel(const char* pages_path, const char* ops_path, const char* out_path, const char* report_path);
/* the output buffer, written only when it changes */
void sg_out(const char* fmt, ...);
int sg_flush(const char* path);

#endif
