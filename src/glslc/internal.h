/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* glslc's passes and the arrays they share (glslc.h says why the arrays are bounded and indexed). lexer.c turns the
   source into tokens through the preprocessor; parser.c makes typed syntax nodes from them, checking types and
   names as it goes, since GLSL declares everything before its use; emit.c writes SPIR-V from the nodes. Every array
   is carved from one block in glslc.c. Index 0 of the nodes, types, members, symbols and macros is no object, so 0
   is "none" wherever one refers to another. */
#ifndef SNX_GLSLC_INTERNAL_H
#define SNX_GLSLC_INTERNAL_H
#include "glslc/glslc.h"
#include <setjmp.h>
#include <string.h>

/* versions, as an index (vi) and as a mask of indices */
enum { GLSLC_V100 = 0, GLSLC_V300 = 1, GLSLC_V310 = 2, GLSLC_V320 = 3 };
#define GLSLC_M100 1u
#define GLSLC_M300 2u
#define GLSLC_M310 4u
#define GLSLC_M320 8u
#define GLSLC_M3XX (GLSLC_M300 | GLSLC_M310 | GLSLC_M320)
#define GLSLC_M31X (GLSLC_M310 | GLSLC_M320)
#define GLSLC_MALL (GLSLC_M100 | GLSLC_M3XX)
/* stages, as a mask */
#define GLSLC_SV   1u
#define GLSLC_SF   2u
#define GLSLC_SC   4u
#define GLSLC_SALL 7u

/* Token kinds, 32 bits. A punctuator is its own characters: one is its ASCII code ('('), two or three are packed from
   the first in the low byte (GLSLC_TK2('+', '='), GLSLC_TK3('<', '<', '=')), made from the bytes one at a time, so a
   kind is the same number on a big-endian host. The kinds no character spells are below ' '; keywords from 0x80, the
   built-in types' from TK_TYPE (0x100), which no two characters reach (their second byte is past ' '). A name the
   preprocessor let through is GLSLC_TK_NAME | its index in g->names: two tokens name the same thing when their kinds
   are equal. Inside the lexer an identifier is TK_IDENT until its name is needed */
#define GLSLC_TK2(a, b)    ((uint32_t)(unsigned char)(a) | (uint32_t)(unsigned char)(b) << 8)
#define GLSLC_TK3(a, b, c) (GLSLC_TK2(a, b) | (uint32_t)(unsigned char)(c) << 16)
#define GLSLC_TK_NAME      0x80000000u
#define GLSLC_IS_NAME(k)   (((k)&GLSLC_TK_NAME) != 0)
#define GLSLC_NAME_OF(k)   ((k) & ~GLSLC_TK_NAME)
#define GLSLC_IS_TYPE(k)   ((k) >= TK_TYPE && (k) < TK_TYPE + 0x100u)
enum {
    TK_EOF = 0,
    TK_IDENT,      /* an identifier the lexer has not named yet */
    TK_INTCONST,
    TK_UINTCONST,
    TK_FLOATCONST,
    TK_JUNK,     /* a character no token starts with, only seen in a section #if leaves out */
    TK_RESERVED, /* a word the version reserves: an error wherever it is used */
    TK_LPAREN = '(',
    TK_RPAREN = ')',
    TK_LBRACKET = '[',
    TK_RBRACKET = ']',
    TK_LBRACE = '{',
    TK_RBRACE = '}',
    TK_DOT = '.',
    TK_COMMA = ',',
    TK_COLON = ':',
    TK_SEMICOLON = ';',
    TK_QUESTION = '?',
    TK_PLUS = '+',
    TK_MINUS = '-',
    TK_STAR = '*',
    TK_SLASH = '/',
    TK_PERCENT = '%',
    TK_LT = '<',
    TK_GT = '>',
    TK_BANG = '!',
    TK_TILDE = '~',
    TK_AMP = '&',
    TK_BAR = '|',
    TK_CARET = '^',
    TK_ASSIGN = '=',
    TK_HASH = '#',
    TK_LE = GLSLC_TK2('<', '='),
    TK_GE = GLSLC_TK2('>', '='),
    TK_EQ = GLSLC_TK2('=', '='),
    TK_NE = GLSLC_TK2('!', '='),
    TK_ANDAND = GLSLC_TK2('&', '&'),
    TK_OROR = GLSLC_TK2('|', '|'),
    TK_XORXOR = GLSLC_TK2('^', '^'),
    TK_SHL = GLSLC_TK2('<', '<'),
    TK_SHR = GLSLC_TK2('>', '>'),
    TK_INC = GLSLC_TK2('+', '+'),
    TK_DEC = GLSLC_TK2('-', '-'),
    TK_ADD_ASSIGN = GLSLC_TK2('+', '='),
    TK_SUB_ASSIGN = GLSLC_TK2('-', '='),
    TK_MUL_ASSIGN = GLSLC_TK2('*', '='),
    TK_DIV_ASSIGN = GLSLC_TK2('/', '='),
    TK_MOD_ASSIGN = GLSLC_TK2('%', '='),
    TK_AND_ASSIGN = GLSLC_TK2('&', '='),
    TK_XOR_ASSIGN = GLSLC_TK2('^', '='),
    TK_OR_ASSIGN = GLSLC_TK2('|', '='),
    TK_HASHHASH = GLSLC_TK2('#', '#'),
    TK_SHL_ASSIGN = GLSLC_TK3('<', '<', '='),
    TK_SHR_ASSIGN = GLSLC_TK3('>', '>', '='),
    /* keywords, classified as the tokens leave the preprocessor: which words are keywords depends on the version */
    TK_ATTRIBUTE = 0x80,
    TK_VARYING,
    TK_CONST,
    TK_UNIFORM,
    TK_BUFFER,
    TK_SHARED,
    TK_IN,
    TK_OUT,
    TK_INOUT,
    TK_CENTROID,
    TK_FLAT,
    TK_SMOOTH,
    TK_PATCH,
    TK_SAMPLE,
    TK_INVARIANT,
    TK_PRECISE,
    TK_LAYOUT,
    TK_COHERENT,
    TK_VOLATILE,
    TK_RESTRICT,
    TK_READONLY,
    TK_WRITEONLY,
    TK_LOWP,
    TK_MEDIUMP,
    TK_HIGHP,
    TK_PRECISION,
    TK_STRUCT,
    TK_BREAK,
    TK_CONTINUE,
    TK_DO,
    TK_FOR,
    TK_WHILE,
    TK_SWITCH,
    TK_CASE,
    TK_DEFAULT,
    TK_IF,
    TK_ELSE,
    TK_DISCARD,
    TK_RETURN,
    TK_TRUE,
    TK_FALSE,
    TK_TYPE = 0x100 /* TK_TYPE + i is the keyword of built-in type i + 1 (GT_VOID is TK_TYPE) */
};

/* token flags */
#define GLSLC_TF_SYNTH 1u /* text is in the synthesized text (a predefined macro's value), not in the source */
#define GLSLC_TF_BOL   2u /* first token of its line, for the preprocessor */

typedef struct {
    GlslcView text; /* the token's text in the source, or in the synthesized text with GLSLC_TF_SYNTH */
    uint32_t kind;  /* TK_*, or GLSLC_TK_NAME | the name */
    uint16_t site;  /* where an error about it points: its own place, or the macro use it came out of */
    uint8_t flags;
    uint8_t pad;
} GlslcToken;

/* A name the preprocessor read: every identifier with the same text has the same one, so what the lexer learns of a
   name (the macro it names, whether it is a keyword) it learns once */
#define GLSLC_NAME_SYNTH 1u     /* text is in the synthesized text: a predefined macro's name */
#define GLSLC_NAME_PLAIN 0xffffu /* keyword: the name is no keyword in any version */
typedef struct {
    GlslcView text;   /* its first place */
    uint16_t next;    /* the next name in its hash bucket */
    uint16_t macro;   /* the macro it names now, 0 for none */
    uint16_t keyword; /* glslc_keywords' row plus one, GLSLC_NAME_PLAIN, or 0 until looked up */
    /* the parser's: the innermost symbol of the name in scope (0 for none), and the first rows of
       glslc_builtin_fns and glslc_builtin_vars with its text, plus one (the table's count plus one for none, 0
       until looked up) */
    uint16_t sym;
    uint16_t builtin_fn;
    uint8_t builtin_var;
    uint8_t flags; /* GLSLC_NAME_* */
} GlslcName;
#define GLSLC_NAME_BUCKETS 2048u

#define GLSLC_MF_FUNCTION 1u  /* takes arguments */
#define GLSLC_MF_SYNTH    2u  /* name is in the synthesized text: a predefined macro */
#define GLSLC_MF_LINE     4u  /* __LINE__: the line it is used on */
#define GLSLC_MF_DEAD     8u  /* #undef'd */
#define GLSLC_MF_VERSION  16u /* __VERSION__: the version #version set */

typedef struct {
    GlslcView name;
    uint16_t first; /* its parameters, then its body, in mtoks */
    uint16_t count; /* parameters and body tokens */
    uint16_t pad;
    uint8_t params; /* how many of the tokens at first are the parameters */
    uint8_t flags;  /* GLSLC_MF_* */
} GlslcMacro;

typedef struct {
    uint16_t pos; /* the first source byte the directive numbers */
    uint16_t pad;
    uint32_t line; /* that byte's line */
} GlslcLineMark;

/* types: the built-in ones at fixed indices (GT_*), then the arrays and structs a shader declares */
enum {
    GLSLC_T_VOID,
    GLSLC_T_SCALAR,
    GLSLC_T_VECTOR,
    GLSLC_T_MATRIX,
    GLSLC_T_SAMPLER,
    GLSLC_T_IMAGE,
    GLSLC_T_ATOMIC,
    GLSLC_T_ARRAY,
    GLSLC_T_STRUCT,
    GLSLC_T_BLOCK
};
enum { GLSLC_DIM_2D, GLSLC_DIM_3D, GLSLC_DIM_CUBE, GLSLC_DIM_BUFFER };
#define GLSLC_TF_ARRAYED 1u
#define GLSLC_TF_SHADOW  2u
#define GLSLC_TF_MS      4u
#define GLSLC_TF_OPAQUE  16u /* arrays and structs: holds a sampler, image or atomic_uint */

typedef struct {
    uint8_t kind;   /* GLSLC_T_* */
    uint8_t base;   /* GlslcBase: of the components, or the sampled or texel type of a sampler or image */
    uint8_t rows;   /* components of a vector, rows of a matrix (1 for a scalar) */
    uint8_t cols;   /* columns of a matrix (1 otherwise) */
    uint8_t dim;    /* samplers and images: GLSLC_DIM_*; blocks: their packing, GLSLC_PACK_* */
    uint8_t tflags; /* samplers and images: GLSLC_TF_ARRAYED, SHADOW, MS */
    uint16_t elem;  /* arrays: the element type */
    uint16_t len;   /* arrays: elements, 0 when sized at run time; structs and blocks: members */
    uint16_t first; /* structs and blocks: the first member */
    GlslcView name; /* structs and blocks: the name */
} GlslcType;

enum {
    GT_NONE = 0,
    GT_VOID,
    GT_BOOL,
    GT_INT,
    GT_UINT,
    GT_FLOAT,
    GT_BVEC2,
    GT_BVEC3,
    GT_BVEC4,
    GT_IVEC2,
    GT_IVEC3,
    GT_IVEC4,
    GT_UVEC2,
    GT_UVEC3,
    GT_UVEC4,
    GT_VEC2,
    GT_VEC3,
    GT_VEC4,
    GT_MAT2,
    GT_MAT2X3,
    GT_MAT2X4,
    GT_MAT3X2,
    GT_MAT3,
    GT_MAT3X4,
    GT_MAT4X2,
    GT_MAT4X3,
    GT_MAT4,
    GT_SAMPLER_FIRST /* then the samplers, the images and atomic_uint, in builtins.c's table order */
};

/* member flags */
#define GLSLC_MEM_ROW_MAJOR 1u
#define GLSLC_MEM_COL_MAJOR 2u
#define GLSLC_MEM_FLAT      4u
#define GLSLC_MEM_CENTROID  8u
#define GLSLC_MEM_READONLY  16u
#define GLSLC_MEM_WRITEONLY 32u
#define GLSLC_MEM_SAMPLE    64u

typedef struct {
    GlslcView name;
    uint16_t type;
    uint8_t prec;
    uint8_t mflags;   /* GLSLC_MEM_* */
    int16_t location; /* in and out blocks' members: the one given, -1 without one */
    uint16_t key;     /* the name's index in g->names */
} GlslcMember;

/* symbols */
enum { GLSLC_SK_VAR = 1, GLSLC_SK_PARAM, GLSLC_SK_FUNC, GLSLC_SK_STRUCT, GLSLC_SK_BLOCK_MEMBER, GLSLC_SK_BUILTIN };
enum {
    GLSLC_ST_LOCAL = 0,
    GLSLC_ST_GLOBAL, /* a global with no storage qualifier: Private */
    GLSLC_ST_CONST,
    GLSLC_ST_IN,
    GLSLC_ST_OUT,
    GLSLC_ST_UNIFORM,
    GLSLC_ST_BUFFER,
    GLSLC_ST_SHARED,
    GLSLC_ST_PARAM_IN,
    GLSLC_ST_PARAM_OUT,
    GLSLC_ST_PARAM_INOUT
};
/* qualifier flags */
#define GLSLC_Q_FLAT      0x0001u
#define GLSLC_Q_SMOOTH    0x0002u
#define GLSLC_Q_CENTROID  0x0004u
#define GLSLC_Q_INVARIANT 0x0008u
#define GLSLC_Q_PRECISE   0x0010u
#define GLSLC_Q_READONLY  0x0020u
#define GLSLC_Q_WRITEONLY 0x0040u
#define GLSLC_Q_COHERENT  0x0080u
#define GLSLC_Q_VOLATILE  0x0100u
#define GLSLC_Q_RESTRICT  0x0200u
#define GLSLC_Q_SAMPLE    0x0400u
#define GLSLC_Q_CONST     0x0800u /* a const parameter */
#define GLSLC_Q_USED      0x1000u /* read or written somewhere */
#define GLSLC_Q_INIT      0x2000u /* a global main initializes, its initializer being no SPIR-V constant */

enum { GLSLC_PREC_NONE = 0, GLSLC_PREC_LOW, GLSLC_PREC_MEDIUM, GLSLC_PREC_HIGH };
enum { GLSLC_PACK_NONE = 0, GLSLC_PACK_STD140, GLSLC_PACK_STD430, GLSLC_PACK_SHARED, GLSLC_PACK_PACKED };

typedef struct {
    GlslcView name;
    uint8_t kind;    /* GLSLC_SK_* */
    uint8_t storage; /* GLSLC_ST_* */
    uint8_t prec;    /* GLSLC_PREC_* */
    uint8_t packing; /* blocks: GLSLC_PACK_*; images: the format (GLSLC_FMT_*) */
    uint16_t qual;   /* GLSLC_Q_* */
    uint16_t type;
    uint16_t node; /* variables: their declaration; functions: their definition (0 while only declared) */
    uint16_t aux;  /* functions: first parameter; block members: the block's symbol; built-ins: the table row */
    uint16_t aux2; /* functions: parameters; block members: the member */
    uint16_t next; /* the symbol of its name it hides, in scope again after it */
    int16_t location, binding, set; /* -1 without one */
    uint16_t key;                   /* the name's index in g->names, 0 for a symbol no name finds */
} GlslcSymbol;

typedef struct {
    GlslcView name;
    uint16_t type;
    uint8_t storage; /* GLSLC_ST_PARAM_* */
    uint8_t prec;
    uint16_t qual;
    uint16_t sym; /* the parameter's symbol in the function's definition */
    uint16_t key; /* the name's index in g->names, 0 for no name */
    uint16_t pad;
} GlslcParam;

typedef struct {
    uint16_t caller, callee; /* function symbols */
    uint16_t tok;            /* the call */
    uint16_t pad;
} GlslcCall;

typedef struct {
    uint16_t type; /* the built-in type a precision statement set */
    uint8_t prec;
    uint8_t scope;
} GlslcDefaultPrec;

/* nodes */
enum {
    GLSLC_N_NONE = 0,
    /* expressions: type is theirs */
    GLSLC_N_CONST,     /* a 32-bit scalar: a, b are its low and high halves */
    GLSLC_N_VAR,       /* a: the symbol */
    GLSLC_N_UNARY,     /* op: GLSLC_OP_*; a: operand */
    GLSLC_N_BINARY,    /* op: GLSLC_OP_*; a, b: operands */
    GLSLC_N_ASSIGN,    /* op: GLSLC_OP_ASSIGN or the operator of a compound assignment; a: target; b: value */
    GLSLC_N_TERNARY,   /* a ? b : c */
    GLSLC_N_INDEX,     /* a[b] */
    GLSLC_N_SWIZZLE,   /* a: vector; b: components, 2 bits each from bit 0; op: how many */
    GLSLC_N_FIELD,     /* a: struct or block; b: the member's index */
    GLSLC_N_CALL,      /* a: first argument; b: the function's symbol */
    GLSLC_N_BUILTIN,   /* a: first argument; b: the built-in function's row; c: its overload's mask */
    GLSLC_N_CONSTRUCT, /* a: first argument; type: what it makes */
    GLSLC_N_LENGTH,    /* a.length(): a: the array */
    GLSLC_N_COMMA,     /* a, b */
    /* statements */
    GLSLC_N_BLOCK,    /* a: first statement */
    GLSLC_N_EXPR,     /* a: the expression */
    GLSLC_N_DECL,     /* a: the variable's symbol; b: its initializer */
    GLSLC_N_IF,       /* a: condition; b: then; c: else */
    GLSLC_N_FOR,      /* a: initialization; b: condition; c: a GLSLC_N_FOR_REST */
    GLSLC_N_FOR_REST, /* a: the expression after each pass; b: the body */
    GLSLC_N_WHILE,    /* a: condition; b: body */
    GLSLC_N_DO,       /* a: body; b: condition */
    GLSLC_N_SWITCH,   /* a: selector; b: first statement of the body */
    GLSLC_N_CASE,     /* a: value (0 for default) */
    GLSLC_N_BREAK,
    GLSLC_N_CONTINUE,
    GLSLC_N_DISCARD,
    GLSLC_N_RETURN,   /* a: value */
    GLSLC_N_FUNCTION, /* a: the function's symbol; b: the body */
    GLSLC_N_GLOBAL    /* a declaration at global scope: a: the variable's symbol; b: its initializer */
};

enum {
    GLSLC_OP_NONE = 0,
    GLSLC_OP_ADD,
    GLSLC_OP_SUB,
    GLSLC_OP_MUL,
    GLSLC_OP_DIV,
    GLSLC_OP_MOD,
    GLSLC_OP_SHL,
    GLSLC_OP_SHR,
    GLSLC_OP_AND,
    GLSLC_OP_OR,
    GLSLC_OP_XOR,
    GLSLC_OP_LT,
    GLSLC_OP_GT,
    GLSLC_OP_LE,
    GLSLC_OP_GE,
    GLSLC_OP_EQ,
    GLSLC_OP_NE,
    GLSLC_OP_LOGIC_AND,
    GLSLC_OP_LOGIC_OR,
    GLSLC_OP_LOGIC_XOR,
    GLSLC_OP_ASSIGN,
    GLSLC_OP_NEG,
    GLSLC_OP_PLUS,
    GLSLC_OP_NOT,
    GLSLC_OP_BITNOT,
    GLSLC_OP_PRE_INC,
    GLSLC_OP_PRE_DEC,
    GLSLC_OP_POST_INC,
    GLSLC_OP_POST_DEC
};

/* node flags */
#define GLSLC_NF_CONST 1u /* a constant expression */

typedef struct {
    uint8_t kind; /* GLSLC_N_* */
    uint8_t op;
    uint8_t flags; /* GLSLC_NF_* */
    uint8_t prec;
    uint16_t tok; /* the token it starts at, for errors */
    uint16_t type;
    uint16_t a, b, c;
    uint16_t next; /* the next of a list: arguments, statements, declarations */
} GlslcNode;

/* image formats, by the layout qualifier's name (builtins.c's table) */
enum {
    GLSLC_FMT_NONE = 0,
    GLSLC_FMT_RGBA32F,
    GLSLC_FMT_RGBA16F,
    GLSLC_FMT_R32F,
    GLSLC_FMT_RGBA8,
    GLSLC_FMT_RGBA8_SNORM,
    GLSLC_FMT_RGBA32I,
    GLSLC_FMT_RGBA16I,
    GLSLC_FMT_RGBA8I,
    GLSLC_FMT_R32I,
    GLSLC_FMT_RGBA32UI,
    GLSLC_FMT_RGBA16UI,
    GLSLC_FMT_RGBA8UI,
    GLSLC_FMT_R32UI
};

/* what the extensions glslc knows add (builtins.c's table names them: one feature may have two names) */
enum {
    GLSLC_EXT_NONE = 0,
    GLSLC_EXT_OES_STANDARD_DERIVATIVES,
    GLSLC_EXT_EXT_SHADER_TEXTURE_LOD,
    GLSLC_EXT_EXT_FRAG_DEPTH,
    GLSLC_EXT_EXT_DRAW_BUFFERS,
    GLSLC_EXT_OES_TEXTURE_3D,
    GLSLC_EXT_EXT_SHADOW_SAMPLERS,
    GLSLC_EXT_IMAGE_ATOMIC,      /* GL_OES_shader_image_atomic */
    GLSLC_EXT_TEXTURE_BUFFER,    /* GL_EXT_texture_buffer, GL_OES_texture_buffer */
    GLSLC_EXT_CUBE_MAP_ARRAY,    /* GL_EXT_texture_cube_map_array, GL_OES_texture_cube_map_array */
    GLSLC_EXT_SAMPLE_VARIABLES,  /* GL_OES_sample_variables */
    GLSLC_EXT_MULTISAMPLE_ARRAY, /* GL_OES_texture_storage_multisample_2d_array */
    GLSLC_EXT_IO_BLOCKS,         /* GL_EXT_shader_io_blocks, GL_OES_shader_io_blocks */
    GLSLC_EXT_COUNT
};
typedef struct {
    const char* name;
    uint8_t len; /* the name's, builtins.c's ROW */
    uint8_t feature; /* GLSLC_EXT_* */
    uint8_t vmask;   /* the versions it is for */
} GlslcExtension;
enum { GLSLC_BEHAVIOR_DISABLE = 0, GLSLC_BEHAVIOR_ENABLE, GLSLC_BEHAVIOR_REQUIRE, GLSLC_BEHAVIOR_WARN };

/* built-in tables (builtins.c) */
typedef struct {
    const char* name;
    uint8_t len; /* the name's, builtins.c's ROW */
    uint8_t kind, base, rows, cols, dim, tflags;
    uint8_t vmask; /* versions it is a type in */
    uint8_t ext;   /* the extension that adds it to 1.00 */
} GlslcBuiltinType;

typedef struct {
    const char* name;
    uint8_t len;   /* the name's, builtins.c's ROW */
    uint16_t kind; /* the token kind */
    uint8_t kw;    /* versions it is a keyword in */
    uint8_t res;   /* versions it is reserved in */
    uint8_t ext;   /* the extension that makes it a keyword where it is not one */
} GlslcKeyword;

/* how a built-in function is written in SPIR-V */
enum {
    GLSLC_BK_EXT = 1,   /* a GLSL.std.450 instruction on the arguments */
    GLSLC_BK_EXT_SPLAT, /* the same, with each scalar argument made a vector of the result's size */
    GLSLC_BK_OP,        /* a core instruction on the arguments */
    GLSLC_BK_OP_SPLAT,  /* the same, with each scalar argument made a vector of the result's size */
    GLSLC_BK_SPECIAL,   /* code of its own in emit.c, by its code (GLSLC_BS_*) */
    GLSLC_BK_TEXTURE,   /* a texture or image function: code is its GLSLC_TX_* */
};

enum {
    GLSLC_BS_MIX_BOOL = 1,
    GLSLC_BS_MATRIX_COMP_MULT,
    GLSLC_BS_OUTER_PRODUCT,
    GLSLC_BS_TRANSPOSE,
    GLSLC_BS_MODF,
    GLSLC_BS_FREXP,
    GLSLC_BS_BITCAST,
    GLSLC_BS_BARRIER,
    GLSLC_BS_MEMORY_BARRIER,
    GLSLC_BS_MEMORY_BARRIER_SHARED,
    GLSLC_BS_MEMORY_BARRIER_BUFFER,
    GLSLC_BS_MEMORY_BARRIER_IMAGE,
    GLSLC_BS_GROUP_MEMORY_BARRIER,
    GLSLC_BS_ATOMIC,
    GLSLC_BS_ATOMIC_COMP_SWAP,
    GLSLC_BS_CARRY,    /* uaddCarry, usubBorrow */
    GLSLC_BS_EXTENDED, /* umulExtended, imulExtended */
    GLSLC_BS_BIT_COUNT,
    GLSLC_BS_FIND_MSB,
    GLSLC_BS_ANY_ALL,
    GLSLC_BS_EQUAL /* equal, notEqual on any vector */
};

/* texture and image functions' shapes */
#define GLSLC_TX_PROJ         0x0001u
#define GLSLC_TX_LOD          0x0002u
#define GLSLC_TX_BIAS         0x0004u /* an optional bias after the other arguments, fragment shaders only */
#define GLSLC_TX_OFFSET       0x0008u
#define GLSLC_TX_GRAD         0x0010u
#define GLSLC_TX_FETCH        0x0020u
#define GLSLC_TX_SIZE         0x0040u
#define GLSLC_TX_GATHER       0x0080u
#define GLSLC_TX_2D_ONLY      0x0100u /* the 1.00 names: sampler2D */
#define GLSLC_TX_CUBE_ONLY    0x0200u /* the 1.00 names: samplerCube */
#define GLSLC_TX_3D_ONLY      0x0400u /* texture3D: sampler3D */
#define GLSLC_TX_SHADOW_ONLY  0x0800u /* shadow2DEXT */
#define GLSLC_TX_IMAGE_LOAD   0x1000u
#define GLSLC_TX_IMAGE_STORE  0x2000u
#define GLSLC_TX_IMAGE_SIZE   0x4000u
#define GLSLC_TX_IMAGE_ATOMIC 0x8000u

typedef struct {
    const char* name;
    uint8_t len; /* the name's, builtins.c's ROW */
    const char* sig; /* return, '=', arguments: one letter each (builtins.c says what each means) */
    uint8_t bk;      /* GLSLC_BK_* */
    uint8_t vmask;   /* versions it is in */
    uint8_t stages;  /* stages it is in */
    uint8_t ext;     /* the extension that adds it to versions outside vmask */
    uint16_t code;   /* GLSL.std.450 or core opcode, GLSLC_BS_*, GLSLC_TX_* */
    uint16_t code2;  /* a second opcode: the signed form, a texture function's atomic op */
} GlslcBuiltinFn;

typedef struct {
    const char* name;
    uint8_t len; /* the name's, builtins.c's ROW */
    uint16_t type;   /* GT_* */
    uint8_t storage; /* GLSLC_ST_IN, OUT, CONST */
    uint8_t stages;
    uint8_t vmask;
    uint8_t ext;      /* the extension that adds it to versions outside vmask */
    uint16_t builtin; /* SPIR-V BuiltIn, or GLSLC_NOT_BUILTIN */
    int32_t value;    /* constants */
    uint16_t array;   /* elements of an array (gl_FragData), 0 for one */
    uint16_t pad;
} GlslcBuiltinVar;
#define GLSLC_NOT_BUILTIN              0xffffu
#define GLSLC_BUILTIN_VALUE_LOCAL_SIZE 0x7fffffff /* gl_WorkGroupSize: the shader's local size */

extern const GlslcBuiltinType glslc_builtin_types[];
extern const uint32_t glslc_builtin_type_count;
extern const GlslcKeyword glslc_keywords[];
extern const uint32_t glslc_keyword_count;
extern const GlslcBuiltinFn glslc_builtin_fns[];
extern const uint32_t glslc_builtin_fn_count;
extern const GlslcBuiltinVar glslc_builtin_vars[];
extern const uint32_t glslc_builtin_var_count;
extern const GlslcExtension glslc_extensions[];
extern const uint32_t glslc_extension_count;
extern const char* const glslc_format_names[];
extern const uint32_t glslc_format_count;
#define GLSLC_MAX_BUILTIN_VARS 64

typedef struct {
    uint32_t* w;
    uint32_t n, cap;
    const char* what; /* for the error a full section gives */
} GlslcWords;

/* a type, constant or other declaration made once: its key, and its id (0 for a free slot) */
typedef struct {
    uint32_t key[4];
    uint32_t id;
} GlslcHashEntry;

/* the emitter's arrays and sections */
#define GLSLC_MAX_RESOURCES 256u  /* samplers, images and blocks: their (set, binding) pairs */
#define GLSLC_MAX_LOCATION  1024u /* an in or out variable's last location, plus one */
typedef struct {
    uint32_t* type_ids;    /* each type's SPIR-V type, by layout: [type * 3 + GLSLC_LAYOUT_*] */
    uint32_t* sym_ids;     /* each symbol's variable (a pointer) or function; a loose uniform's member */
    uint32_t* sym_types;   /* variables: their SPIR-V storage class plus one; functions: their OpTypeFunction */
    uint32_t* sampler_ids; /* a uniform or parameter of a struct that holds samplers: its first sampler's variable or
                              parameter, the next sampler's at the next id */
    /* pointer, image and sampler array types, constants: open addressing on GLSLC_HASH_SIZE. A sparse set says which
       slots are taken (Briggs and Torczon's): slot i is when hash_back[i] < nhash and hash_used[hash_back[i]] is i,
       which nothing left in the workspace can make true, so the hash is empty without being cleared */
    GlslcHashEntry* hash;
    uint16_t *hash_used, *hash_back;
    uint32_t nhash;
    GlslcWords caps, imports, entry, modes, names, annotations, globals, functions, locals, body;
    uint32_t next_id;
    uint32_t glsl_ext;  /* the GLSL.std.450 import */
    uint32_t cur_block; /* the block being written, 0 after its terminator */
    uint32_t site;      /* the source place of what is being written, for the errors of a full section */
    uint32_t main_id;
    uint64_t cap_mask; /* the capabilities written, by number */
    uint32_t nest;     /* loops and switches around the statement being written */
    uint32_t break_ids[SNX_GLSLC_MAX_DEPTH];
    uint32_t continue_ids[SNX_GLSLC_MAX_DEPTH]; /* 0 for a switch */
    uint32_t default_block;                     /* the default uniform block's variable, 0 without loose uniforms */
    uint32_t default_type;                      /* its struct */
    uint32_t default_size;
    uint16_t default_members;
    uint16_t default_binding;
    uint32_t interface[1024]; /* the Input and Output variables, for OpEntryPoint */
    /* flip_y: gl_FragCoord's [0] and gl_PointCoord's [1] Input variables, and the Private copies the shader reads */
    uint32_t flip_in[2], flip_copy[2];
    uint32_t interface_count;
    uint32_t resources[GLSLC_MAX_RESOURCES]; /* (set << 16) | binding taken */
    uint32_t nresources;
    uint8_t in_locations[GLSLC_MAX_LOCATION];
    uint8_t out_locations[GLSLC_MAX_LOCATION];
    uint8_t depth_replacing;
    uint32_t* stack; /* ids being gathered for one instruction, nested: GLSLC_STACK_WORDS */
    uint32_t nstack;
} GlslcEmitter;
#define GLSLC_STACK_WORDS 65536u
#define GLSLC_HASH_SIZE   8192u
enum { GLSLC_LAYOUT_NONE = 0, GLSLC_LAYOUT_STD140 = 1, GLSLC_LAYOUT_STD430 = 2 };

#define GLSLC_SOURCE_PAD 8u
#define GLSLC_TYPE_NAMES 4u /* the most types one message names, and one more */
typedef struct {
    /* the input: a copy of the source in the workspace, GLSLC_SOURCE_PAD zero bytes after its end, so the lexer reads
       a byte or three ahead without asking whether the source has them (a view's place is the same in both) */
    const char* src;
    uint32_t src_len;
    GlslcStage stage;
    uint32_t smask; /* the stage, as GLSLC_S* */
    uint8_t vi;     /* the version, GLSLC_V* */
    uint16_t version;
    GlslcResult* result;
    GlslcBindFn bind; /* the backend's binding map, or NULL */
    void* bind_user;
    int map_default; /* the map places the default uniform block too */
    int flip_y;      /* gl_FragCoord and gl_PointCoord read through the push constant's flip */
    jmp_buf fail;
    /* the raw scanner */
    uint32_t cur;
    uint8_t bol;
    /* the preprocessor */
    char* synth;
    uint32_t nsynth;
    GlslcToken* toks;
    uint32_t ntoks;
    uint32_t overflow_site; /* the first token past SNX_GLSLC_MAX_TOKENS */
    GlslcMacro* macros;
    uint32_t nmacros;
    GlslcName* names;
    uint32_t nnames;
    uint16_t name_hash[GLSLC_NAME_BUCKETS];
    GlslcToken* mtoks;
    uint32_t nmtoks;
    GlslcToken* exp;
    uint32_t nexp;
    uint32_t exp_site; /* the macro use (or #if) being expanded, where an error about it points */
    uint16_t active[SNX_GLSLC_MAX_MACRO_DEPTH];
    uint32_t nactive;
    uint32_t ifs[SNX_GLSLC_MAX_IF_DEPTH]; /* GLSLC_IF_* flags, and the directive's place from bit 8 */
    uint32_t nifs;
    uint8_t invariant_all; /* #pragma STDGL invariant(all) */
    GlslcLineMark* marks;
    uint32_t nmarks;
    uint8_t ext[GLSLC_EXT_COUNT]; /* GLSLC_BEHAVIOR_* */
    uint8_t seen_code;            /* a token that is not a directive went out */
    uint8_t seen_any;             /* anything but comments and spaces, for #version */
    uint8_t explicit_version;
    /* the front end */
    uint32_t pos; /* the token being parsed */
    GlslcNode* nodes;
    uint32_t nnodes;
    GlslcType* types;
    uint32_t ntypes;
    GlslcMember* members;
    uint32_t nmembers;
    GlslcSymbol* syms; /* each name's innermost one is its g->names entry's sym */
    uint32_t nsyms;
    GlslcParam* params;
    uint32_t nparams;
    GlslcCall* calls;
    uint32_t ncalls;
    uint16_t scopes[SNX_GLSLC_MAX_SCOPES]; /* the symbol count at each scope's start */
    uint32_t nscopes;
    GlslcDefaultPrec precs[128];
    uint32_t nprecs;
    uint16_t builtin_syms[GLSLC_MAX_BUILTIN_VARS]; /* each built-in variable's symbol, once used */
    uint32_t depth;
    uint16_t func;       /* the function being parsed */
    uint16_t first_decl; /* the first global declaration or function */
    uint16_t last_decl;
    uint8_t func_returns; /* the function being parsed has a return statement */
    uint16_t* walk;       /* the recursion check: each symbol's colour, then its stack of (function, call) */
    uint16_t loops;       /* loops around the statement being parsed */
    uint16_t switches;    /* switches around it */
    uint16_t local_size[3];
    uint8_t local_size_set;
    uint8_t early_fragment_tests;
    uint8_t frag_outputs;    /* gl_FragColor (1), gl_FragData (2) used */
    uint8_t uniform_packing; /* layout(std140) uniform; and its kind: the blocks' defaults */
    uint8_t buffer_packing;
    uint8_t uniform_row_major;
    uint8_t buffer_row_major;
    /* the types an error's message names, made only when it is (parser.c's type_text) */
    char type_names[GLSLC_TYPE_NAMES][64];
    uint32_t type_name_next;
    /* the back end */
    GlslcEmitter e;
} Glslc;

/* glslc.c: a compilation in a workspace's memory, and its passes up to last (the tests stop after a pass to read its
   arrays). glslc_open refuses a workspace, stage, size or source glslc cannot take (NULL, and result->error) */
enum { GLSLC_PASS_LEX = 0, GLSLC_PASS_PARSE, GLSLC_PASS_EMIT };
Glslc* glslc_open(void* memory, size_t memory_size, GlslcStage stage, const char* source, size_t size,
                  GlslcResult* result);
int glslc_run(Glslc* g, int last, uint32_t* words, size_t capacity);
void glslc_fail(Glslc* g, uint32_t site, const char* fmt, ...);
void glslc_warn(Glslc* g, uint32_t site, const char* fmt, ...);
void glslc_locate(const Glslc* g, uint32_t site, uint32_t* line, uint32_t* column);
/* text's hash, 8 bytes a step, each made from its bytes (the same on any host), then mixed (MurmurHash3's finalizer).
   It reads up to 7 bytes past text's end, which the source's copy (GLSLC_SOURCE_PAD) and the synthesized text (the
   workspace's next array) have */
uint32_t glslc_hash(const char* text, uint32_t len);
const char* glslc_view_text(const Glslc* g, GlslcView v);
const char* glslc_token_text(const Glslc* g, const GlslcToken* t);
/* whether v is the n bytes at word: the lengths first, so most names differ without a byte read */
static inline int glslc_view_is_n(const Glslc* g, GlslcView v, const char* word, uint32_t n) {
    return v.len == n && !memcmp(g->src + v.pos, word, n);
}
/* whether v is word: inline, so a literal's strlen is the compiler's */
static inline int glslc_view_is(const Glslc* g, GlslcView v, const char* word) {
    return glslc_view_is_n(g, v, word, (uint32_t)strlen(word));
}
/* whether v is a table row's name (builtins.c's ROW gives its length) */
#define GLSLC_VIEW_IS_ROW(g, v, row) glslc_view_is_n((g), (v), (row)->name, (row)->len)
/* lexer.c: fills toks, ending with TK_EOF */
void glslc_lex(Glslc* g);
int glslc_parse_int(const char* text, uint32_t len, uint32_t* value); /* 0 when it does not fit in 32 bits */
/* builtins.c */
const GlslcKeyword* glslc_find_keyword(const char* text, uint32_t len);
/* parser.c: fills the nodes, types and symbols */
void glslc_parse(Glslc* g);
uint32_t glslc_type_components(const Glslc* g, uint32_t type);
uint32_t glslc_fold_convert(const Glslc* g, uint32_t bits, uint32_t from, uint32_t to); /* a scalar's bits as another */
uint32_t glslc_vector_type(uint32_t base, uint32_t rows);
uint32_t glslc_matrix_type(uint32_t cols, uint32_t rows);
void glslc_type_name(const Glslc* g, uint32_t type, char* out, size_t size);
/* emit.c: writes the module into words, or says how many words it needs */
void glslc_emit(Glslc* g, uint32_t* words, size_t capacity);

#define GLSLC_TEXT(g, v) (int)(v).len, glslc_view_text((g), (v))
#endif
