/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's shaders, programs and program pipelines (gl_state.h's layer), shared by the Android bridge and the
   console: glslc compiles each stage into SPIR-V, the link places the stages' resources at one binding each and
   lists the uniforms, inputs and outputs, and the loose uniforms' values live in each stage's default block.

   The objects are flat records in one block (gl_program.c's), named by their index: shaders and programs share one
   space of names, as GL numbers them. What has a variable size (a shader's source and log, a program's log, its
   glBindAttribLocation pairs, and what its link made: bindings, uniforms, locations, units, inputs, outputs, names,
   the default blocks and the stages' SPIR-V) lives in the layer's byte arena, a range of it each. A save state holds
   the block and the arena as they are. A platform's Vulkan objects of a program (its modules, layouts and pipelines)
   are its own, made from the record at their first use and again when the program's generation moves.

   A pointer into the arena (snx_gl_bytes and the program's arrays below) holds until the next call that compiles,
   links, sets a source or a name, or loads a save state; a platform reads them within one call. The caller holds
   its platform's GL lock around every call, as for gl_state.h's */
#ifndef SNX_GL_PROGRAM_H
#define SNX_GL_PROGRAM_H
#include "glslc/glslc.h" /* the records' kinds and bases are glslc's (GlslcReflectKind, GlslcBase) */
#include <stddef.h>
#include <stdint.h>

#define SNX_GL_PROGRAM_NAMES 1024 /* names of shaders and programs, one space: 54 programs of two stages in a game */
#define SNX_GL_PIPELINES     64   /* program pipelines */
#define SNX_GL_UBO_BINDINGS  36   /* uniform buffer binding points, GL_MAX_UNIFORM_BUFFER_BINDINGS */
#define SNX_GL_ATTRIBS       16   /* GL_MAX_VERTEX_ATTRIBS */

/* a range of the layer's byte arena: size bytes at offset at; {0, 0} is none */
typedef struct {
    uint32_t at, size;
} SnxGlBytes;

/* the texture targets a sampler reads, as indices */
enum { SNX_GL_T2D = 0, SNX_GL_TCUBE = 1, SNX_GL_T3D = 2, SNX_GL_TARRAY = 3, SNX_GL_TMS = 4 };

/* a uniform of a program, as GL lists them: a default block's member (of a stage or several), a block's member, or a
   sampler */
typedef struct {
    uint32_t name;     /* the offset of its name in the program's names */
    uint32_t type;     /* GL_FLOAT_VEC4, GL_SAMPLER_2D and the others */
    uint16_t count;    /* elements of an array, 1 for others */
    int16_t block;     /* the uniform block's index, -1 for the default block */
    int32_t offset[3]; /* in each stage's default block (or the block), -1 where the stage has none */
    uint16_t array_stride, matrix_stride;
    int32_t location; /* the first element's, -1 in a block */
    int32_t sampler;  /* the index of its first unit in the program's units, -1 when not a sampler */
} SnxGlUniform;

/* a descriptor binding of a program's set */
typedef struct {
    uint32_t name; /* its name in names: a sampler's or a block's */
    uint8_t kind;  /* SNX_GLSLC_REFLECT_SAMPLER, IMAGE, UNIFORM_BLOCK or BUFFER_BLOCK; UNIFORM_BLOCK with an empty name
                       is a stage's default block */
    uint8_t stage; /* a default block's stage */
    uint8_t dim;   /* a sampler's SNX_GL_T*, from its SPIR-V type */
    uint8_t texel_buffer;
    uint16_t count; /* descriptors */
    uint16_t binding;
    uint16_t first; /* a sampler's first unit in the program's units; a block's index */
    uint8_t stages; /* the stages that read it, a bit each (0 vertex, 1 fragment, 2 compute) */
    uint8_t pad;
    uint32_t size; /* a block's bytes */
} SnxGlBinding;

/* a vertex input or a fragment output */
typedef struct {
    uint32_t name;
    uint32_t type;
    uint16_t count;   /* elements of an array */
    uint8_t base;     /* GlslcBase */
    uint8_t columns;  /* locations each element takes */
    int32_t location; /* GL's */
    int32_t spirv;    /* the module's */
} SnxGlInput;

typedef struct {
    uint8_t used, deleted, compiled, pad;
    uint32_t type;     /* GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_COMPUTE_SHADER */
    uint32_t attached; /* how many programs have it */
    SnxGlBytes source; /* its text and a NUL */
    SnxGlBytes log;    /* glGetShaderInfoLog's, with its NUL */
} SnxGlShader;

typedef struct {
    uint8_t used, deleted, linked;
    uint8_t separable; /* GL_PROGRAM_SEPARABLE: it links without the other stage */
    uint8_t hidden;    /* a program pipeline's stages linked together, which GL does not name */
    uint8_t pad[3];
    uint32_t shaders[3]; /* the vertex, fragment and compute shaders attached */
    uint32_t generation; /* counts the links: a platform's objects of an older one are stale */
    SnxGlBytes log;      /* glGetProgramInfoLog's, with its NUL */
    SnxGlBytes bound;    /* glBindAttribLocation's: name\0 and a 32-bit location, each, for the next link */
    SnxGlBytes link;     /* the link's arrays below, at their offsets in it */
    uint32_t bindings, uniforms, locations, units, inputs, outputs, names;
    uint32_t defaults[3], spirv[3]; /* each stage's default block and SPIR-V module, 0 size where it has none */
    uint32_t binding_count, uniform_count, location_count, unit_count, input_count, output_count, block_count;
    uint32_t names_size, default_size[3], spirv_words[3];
    uint32_t default_serial[3];                  /* counts the writes to each stage's default block */
    uint32_t block_binding[SNX_GL_UBO_BINDINGS]; /* each uniform block's binding point */
    uint32_t local_size[3];                      /* a compute shader's */
} SnxGlProgram;

/* GLES 3.1's program pipeline: a separable program for each stage. A draw uses a hidden program linked from the
   stages' shaders, its uniforms copied from theirs */
typedef struct {
    uint32_t used;
    uint32_t stages[3];      /* the vertex, fragment and compute stages' programs */
    uint32_t active;         /* glActiveShaderProgram's, for glUniform* */
    uint32_t linked;         /* the hidden program */
    uint32_t generations[2]; /* of the vertex and fragment programs it was linked from */
    uint32_t names[2];
} SnxGlPipeline;

/* Where a platform whose draws bind each stage's resources in a set of the stage's own (the console's renderer) wants
   a program's resources, and how its programs read them, as the platform's translations of the console's programs do:
   each stage's set of blocks and set of samplers; from block_base, the stage's default block and then the program's
   uniform blocks in the order the stages declare them, blocks bindings in all; from sampler_base, the program's
   samplers in that order, samplers bindings in all. A sampler array, an image or a storage block has no place in it:
   the link refuses the program, naming it. Block i (from block_base) is read from a uniform buffer below
   uniform_blocks, else as a read-only storage buffer; a stage with window_push reads its uniform
   ones through windows (glslc's SNX_GLSLC_BLOCK_WINDOW), block i's offset the 16 bits at bit 16 (i & 1) of the push
   constant word at byte window_push + 4 (i / 2) */
typedef struct {
    uint32_t set[3]; /* the vertex, fragment and compute stages' */
    uint32_t block_base, blocks;
    uint32_t sampler_base, samplers;
    uint32_t sampler_set[3];
    uint32_t uniform_blocks;
    uint32_t window_push[3]; /* 0: the stage reads its blocks whole */
} SnxGlLayout;
/* how stage reads the block at binding (glslc's SNX_GLSLC_BLOCK_*) in layout, with a window's push constant bit */
int snx_gl_layout_block(const SnxGlLayout* layout, int stage, uint32_t binding, uint32_t* offset_bit);

/* What a platform hears from the programs: a program's link is gone (linked again, deleted), so its objects made from
   the old one go too; and a shader that does not compile or a program that does not link, for its log. Either may be
   NULL. And where its draws want the resources: stage_sets 0, the program's one set (binding 0 to 2 the stages'
   default blocks, then the resources), with gl_FragCoord through a flip in a push constant; 1, layout's sets, with
   no push constant. Not part of a save state: a platform gives it again after a load */
typedef struct {
    void (*program_changed)(uint32_t program);
    void (*say)(const char* line);
    int stage_sets;
    SnxGlLayout layout;
} SnxGlProgramHost;
void snx_gl_program_host(const SnxGlProgramHost* host);

/* the bytes of a range of the arena (NULL for none) */
uint8_t* snx_gl_bytes(SnxGlBytes b);
/* a shader or program of a name, NULL for one not made */
SnxGlShader* snx_gl_shader(uint32_t name);
SnxGlProgram* snx_gl_program(uint32_t name);
SnxGlPipeline* snx_gl_pipeline(uint32_t name);
/* a linked program's arrays */
static inline const SnxGlBinding* snx_gl_program_bindings(const SnxGlProgram* p) {
    return (const SnxGlBinding*)(snx_gl_bytes(p->link) + p->bindings);
}
static inline const SnxGlUniform* snx_gl_program_uniforms(const SnxGlProgram* p) {
    return (const SnxGlUniform*)(snx_gl_bytes(p->link) + p->uniforms);
}
static inline int32_t* snx_gl_program_units(const SnxGlProgram* p) {
    return (int32_t*)(snx_gl_bytes(p->link) + p->units);
}
static inline const SnxGlInput* snx_gl_program_inputs(const SnxGlProgram* p) {
    return (const SnxGlInput*)(snx_gl_bytes(p->link) + p->inputs);
}
static inline const SnxGlInput* snx_gl_program_outputs(const SnxGlProgram* p) {
    return (const SnxGlInput*)(snx_gl_bytes(p->link) + p->outputs);
}
static inline const char* snx_gl_program_names(const SnxGlProgram* p) {
    return (const char*)(snx_gl_bytes(p->link) + p->names);
}
static inline uint8_t* snx_gl_program_default(const SnxGlProgram* p, int stage) {
    return snx_gl_bytes(p->link) + p->defaults[stage];
}
static inline const uint32_t* snx_gl_program_spirv(const SnxGlProgram* p, int stage) {
    return (const uint32_t*)(snx_gl_bytes(p->link) + p->spirv[stage]);
}

/* the context's current program and program pipeline (glUseProgram, glBindProgramPipeline) */
uint32_t snx_gl_current_program(uint32_t ctx);
uint32_t snx_gl_current_pipeline(uint32_t ctx);
/* the program a draw (or a dispatch, compute) uses: the current one, else the bound program pipeline's, linked from
   its stages now when they changed; 0 for none */
uint32_t snx_gl_draw_program(uint32_t ctx, int compute);

/* GL's calls. Each takes the context it runs in, records GL's error for what GL refuses, and reads and writes the
   client's memory through host pointers, which each platform makes of the guest's */
uint32_t snx_gl_create_shader(uint32_t ctx, uint32_t type);
void snx_gl_delete_shader(uint32_t ctx, uint32_t shader);
int snx_gl_is_shader(uint32_t shader);
/* glShaderSource's text, size bytes joined from its strings: the room to write it into (and a NUL after it), NULL
   with the error recorded for a shader not made, or with no room */
char* snx_gl_shader_source(uint32_t ctx, uint32_t shader, size_t size);
void snx_gl_compile_shader(uint32_t ctx, uint32_t shader);
/* glGetShaderiv's value of pname into *out; 0 with the error recorded */
int snx_gl_get_shaderiv(uint32_t ctx, uint32_t shader, uint32_t pname, int32_t* out);
/* a shader's info log or source, a program's info log: "" for one that has none, NULL (the error recorded) for a
   name not made */
const char* snx_gl_shader_info_log(uint32_t ctx, uint32_t shader);
const char* snx_gl_shader_text(uint32_t ctx, uint32_t shader);
const char* snx_gl_program_info_log(uint32_t ctx, uint32_t program);
void snx_gl_shader_precision_format(uint32_t precision, int32_t range[2], int32_t* bits);

uint32_t snx_gl_create_program(uint32_t ctx);
void snx_gl_delete_program(uint32_t ctx, uint32_t program);
int snx_gl_is_program(uint32_t program);
void snx_gl_attach_shader(uint32_t ctx, uint32_t program, uint32_t shader);
void snx_gl_detach_shader(uint32_t ctx, uint32_t program, uint32_t shader);
/* the shaders attached, up to max of them into out: how many it wrote, -1 with the error recorded */
int32_t snx_gl_attached_shaders(uint32_t ctx, uint32_t program, int32_t max, uint32_t* out);
void snx_gl_bind_attrib_location(uint32_t ctx, uint32_t program, uint32_t index, const char* name);
void snx_gl_link_program(uint32_t ctx, uint32_t program);
void snx_gl_use_program(uint32_t ctx, uint32_t program);
void snx_gl_validate_program(uint32_t ctx, uint32_t program);
void snx_gl_program_parameter(uint32_t ctx, uint32_t program, uint32_t pname, int32_t value);
/* glGetProgramiv's values of pname into out: how many (1, or 3 for GL_COMPUTE_WORK_GROUP_SIZE), 0 with the error
   recorded */
int snx_gl_get_programiv(uint32_t ctx, uint32_t program, uint32_t pname, int32_t out[3]);
/* glGetActiveUniform and glGetActiveAttrib: the name of the index'th, its count and type; NULL with the error
   recorded */
const char* snx_gl_active(uint32_t ctx, uint32_t program, uint32_t index, int uniforms, int32_t* count, uint32_t* type);
int32_t snx_gl_attrib_location(uint32_t ctx, uint32_t program, const char* name);
int32_t snx_gl_frag_data_location(uint32_t ctx, uint32_t program, const char* name);
int32_t snx_gl_uniform_location(uint32_t ctx, uint32_t program, const char* name);
/* glUniform*: count values of comps 32-bit components each (a matrix's columns after each other, or its rows with
   transpose) at data, kind 'f', 'i' or 'u', into the uniform at location of the current program, else of the bound
   pipeline's active one. glProgramUniform*: of program */
void snx_gl_uniform(uint32_t ctx, int32_t location, int32_t count, const void* data, int comps, char kind,
                    int transpose);
void snx_gl_program_uniform(uint32_t ctx, uint32_t program, int32_t location, int32_t count, const void* data,
                            int comps, char kind, int transpose);
/* glGetUniform*v: the value at location, kind 'f' or 'i' (16 values at most), into out */
void snx_gl_get_uniform(uint32_t ctx, uint32_t program, int32_t location, char kind, void* out);
uint32_t snx_gl_uniform_block_index(uint32_t ctx, uint32_t program, const char* name);
void snx_gl_uniform_block_binding(uint32_t ctx, uint32_t program, uint32_t index, uint32_t binding);
/* glGetActiveUniformBlockiv's values of pname into out (GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES: as many as the block
   has uniforms): how many, 0 with the error recorded */
int snx_gl_active_uniform_blockiv(uint32_t ctx, uint32_t program, uint32_t index, uint32_t pname, int32_t* out,
                                  int32_t max);
const char* snx_gl_active_uniform_block_name(uint32_t ctx, uint32_t program, uint32_t index);
uint32_t snx_gl_uniform_index(uint32_t ctx, uint32_t program, const char* name);
/* glGetActiveUniformsiv's pname of uniform index into *out: 0 with the error recorded */
int snx_gl_active_uniformiv(uint32_t ctx, uint32_t program, uint32_t index, uint32_t pname, int32_t* out);
/* GLES 3.1's program interface queries */
int snx_gl_program_interfaceiv(uint32_t ctx, uint32_t program, uint32_t interface, uint32_t pname, int32_t* out);
uint32_t snx_gl_program_resource_index(uint32_t ctx, uint32_t program, uint32_t interface, const char* name);
const char* snx_gl_program_resource_name(uint32_t ctx, uint32_t program, uint32_t interface, uint32_t index);
/* glGetProgramResourceiv: count properties at props into params, size of them at most: how many it wrote */
int32_t snx_gl_program_resourceiv(uint32_t ctx, uint32_t program, uint32_t interface, uint32_t index, int32_t count,
                                  const uint32_t* props, int32_t size, int32_t* params);
int32_t snx_gl_program_resource_location(uint32_t ctx, uint32_t program, uint32_t interface, const char* name);
/* glProgramBinary: no format is taken, so the program is left unlinked with that in its log */
void snx_gl_program_binary(uint32_t ctx, uint32_t program);
/* glCreateShaderProgramv's program of one stage: shader (snx_gl_create_shader's, its text set) compiled and attached
   to a separable program, which is linked and returned; the shader goes with the program. 0 for no program */
uint32_t snx_gl_create_shader_program(uint32_t ctx, uint32_t shader);

void snx_gl_gen_pipelines(uint32_t ctx, int32_t n, uint32_t* out);
void snx_gl_delete_pipelines(uint32_t ctx, int32_t n, const uint32_t* names);
int snx_gl_is_pipeline(uint32_t name);
void snx_gl_bind_pipeline(uint32_t ctx, uint32_t pipeline);
void snx_gl_use_program_stages(uint32_t ctx, uint32_t pipeline, uint32_t stages, uint32_t program);
void snx_gl_active_shader_program(uint32_t ctx, uint32_t pipeline, uint32_t program);
void snx_gl_validate_pipeline(uint32_t ctx, uint32_t pipeline);
int snx_gl_get_pipelineiv(uint32_t ctx, uint32_t pipeline, uint32_t pname, int32_t* out);

/* a string out to a client's buffer of size bytes, NUL included, and its length without the NUL to *length when
   length is not NULL (both host pointers, of any alignment) */
void snx_gl_string_out(const char* text, int32_t size, void* length, char* out);

/* the save state's part of the programs (gl_state.c's copy holds it after the contexts): its bytes, a copy, the check
   that refuses one this build cannot load (why names the field and its value), and its load */
size_t snx_gl_program_state_bytes(void);
void snx_gl_program_state_save(void* out);
int snx_gl_program_state_check(const void* in, size_t size, char* why, size_t whysize);
/* 0, or -1 when there is no memory for the arena (the running state left as it was). Every program's objects a
   platform made are stale after it: program_changed hears each name */
int snx_gl_program_state_load(const void* in, size_t size);
/* a checked copy's: whether program (0, or a program made) and pipeline (0, or a pipeline made) may be bound */
int snx_gl_program_state_bound(const void* in, uint32_t program, uint32_t pipeline);

#endif
