/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* glslc: the OpenGL ES shading language compiled into SPIR-V, for the backends that run OpenGL ES programs on Vulkan.
   It takes GLSL ES 1.00, 3.00, 3.10 and 3.20 (what an OpenGL ES 3.2 GPU of the Mali-G52's class takes) for the
   vertex, fragment and compute stages, and writes a SPIR-V 1.3 module for Vulkan 1.1. docs/GLSLC.md lists what it
   compiles, what it does not yet, and how the OpenGL ES interface (locations, bindings, loose uniforms) maps onto
   Vulkan's. Nothing in the build compiles a shader with it: ScarletNX's own shaders are Vulkan GLSL, compiled by the
   host's glslangValidator (cmake/glsl.cmake).

   The source stays where the caller keeps it: every token and name is a view of it, a 16-bit position and length.
   Tokens, syntax nodes, types and symbols are arrays of fixed-size records that refer to each other by 16-bit index,
   carved from one allocation per compilation. Each array has the upper bound below; a shader that needs more is
   refused with an error that names the bound and how much it needed. */
#ifndef SNX_GLSLC_H
#define SNX_GLSLC_H
#include <stddef.h>
#include <stdint.h>

/* The bounds of one compilation. Positions and indices are 16 bits, so none is past 65535 */
#define SNX_GLSLC_MAX_SOURCE       65535u  /* bytes of source: a view's position is 16 bits */
#define SNX_GLSLC_MAX_TOKENS       32768u  /* tokens after preprocessing, the end included */
#define SNX_GLSLC_MAX_MACROS       512u    /* #define'd names, predefined ones included */
#define SNX_GLSLC_MAX_MACRO_TOKENS 8192u   /* the parameters and bodies of every macro */
#define SNX_GLSLC_MAX_EXPANSION    4096u   /* the tokens one macro use expands to, with its arguments */
#define SNX_GLSLC_MAX_MACRO_DEPTH  32u     /* macros expanding inside each other */
#define SNX_GLSLC_MAX_IF_DEPTH     64u     /* #if, #ifdef and #ifndef nested */
#define SNX_GLSLC_MAX_LINE_MARKS   256u    /* #line directives */
#define SNX_GLSLC_MAX_SYNTH        4096u   /* bytes of text glslc writes: predefined macros, __LINE__'s numbers */
#define SNX_GLSLC_MAX_NODES        32768u  /* syntax nodes: no construct makes more than it has tokens */
#define SNX_GLSLC_MAX_TYPES        2048u   /* types: the built-in ones, arrays, structs and blocks */
#define SNX_GLSLC_MAX_MEMBERS      2048u   /* members of every struct and block */
#define SNX_GLSLC_MAX_SYMBOLS      4096u   /* variables, parameters, functions and struct names declared */
#define SNX_GLSLC_MAX_PARAMS       2048u   /* parameters of every function prototype */
#define SNX_GLSLC_MAX_CALLS        4096u   /* calls between user functions, for the recursion check */
#define SNX_GLSLC_MAX_DEPTH        96u     /* expressions and statements nested in each other */
#define SNX_GLSLC_MAX_SCOPES       64u     /* scopes nested in each other */
#define SNX_GLSLC_MAX_WORDS        262144u /* words of SPIR-V the module may have */
#define SNX_GLSLC_MAX_REFLECT      256u    /* entries of GlslcResult.reflect */

typedef enum { SNX_GLSLC_VERTEX = 0, SNX_GLSLC_FRAGMENT = 1, SNX_GLSLC_COMPUTE = 2 } GlslcStage;

/* A piece of the source: source[pos, pos + len) */
typedef struct {
    uint16_t pos, len;
} GlslcView;

/* What the shader's interface became, for the caller to bind: GLSL ES names its inputs and resources, Vulkan numbers
   them */
typedef enum {
    SNX_GLSLC_REFLECT_INPUT = 0,         /* an in (or attribute, varying) variable: location */
    SNX_GLSLC_REFLECT_OUTPUT = 1,        /* an out variable, gl_FragColor and gl_FragData included: location */
    SNX_GLSLC_REFLECT_UNIFORM = 2,       /* a uniform outside a block, a member of the default uniform block: offset */
    SNX_GLSLC_REFLECT_SAMPLER = 3,       /* binding, set */
    SNX_GLSLC_REFLECT_IMAGE = 4,         /* binding, set */
    SNX_GLSLC_REFLECT_UNIFORM_BLOCK = 5, /* binding, set, size; the default uniform block has an empty name */
    SNX_GLSLC_REFLECT_BUFFER_BLOCK = 6   /* binding, set, size (without a runtime-sized last member) */
} GlslcReflectKind;

typedef enum {
    SNX_GLSLC_BASE_BOOL = 0,
    SNX_GLSLC_BASE_INT = 1,
    SNX_GLSLC_BASE_UINT = 2,
    SNX_GLSLC_BASE_FLOAT = 3,
    SNX_GLSLC_BASE_STRUCT = 4, /* a struct, or a block */
    SNX_GLSLC_BASE_OPAQUE = 5  /* a sampler or an image */
} GlslcBase;

typedef struct {
    GlslcView name;    /* the name the source declares it with; a gl_ name is a view of its first use */
    uint8_t kind;      /* GlslcReflectKind */
    uint8_t base;      /* GlslcBase of the type, or of an array's elements */
    uint8_t rows;      /* components of a vector, rows of a matrix, 1 for a scalar */
    uint8_t cols;      /* columns of a matrix, 1 otherwise */
    uint16_t array;    /* elements of an array, 0 when it is not one */
    uint16_t location; /* inputs and outputs: the location; samplers, images and blocks: the binding */
    uint16_t set;      /* samplers, images and blocks: the descriptor set */
    uint16_t pad;
    uint32_t offset; /* uniforms: the byte offset in the default uniform block; blocks: their size in bytes */
} GlslcReflect;

typedef struct {
    uint32_t words;        /* SPIR-V words of the module: written when they fit the buffer, else what it must hold */
    uint32_t line, column; /* the error's place, from 1 (the #line directives applied); 0 without an error */
    uint16_t version;      /* the GLSL ES version compiled: 100, 300, 310 or 320 */
    uint16_t warnings;     /* how many warnings there were; warning holds the first */
    uint16_t reflect_count;
    uint16_t pad;
    char error[256];   /* the error, "" when the compilation worked */
    char warning[256]; /* the first warning, "" without one */
    GlslcReflect reflect[SNX_GLSLC_MAX_REFLECT];
} GlslcResult;

/* Compiles size bytes of GLSL ES at source for stage into a SPIR-V module at words, which holds capacity words.
   Returns 0 when the module is written, -1 when the source has an error (result->error, line, column) or the module
   needs more than capacity words (result->words says how many). The source needs no terminating NUL, and must stay
   while result's views are read. One allocation, freed before returning; -1 with "out of memory" when it fails */
int snx_glslc_compile(GlslcStage stage, const char* source, size_t size, uint32_t* words, size_t capacity,
                      GlslcResult* result);
#endif
