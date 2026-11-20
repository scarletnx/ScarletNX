/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's shaders and programs (gl_program.h). glslc compiles each stage into SPIR-V.

   glCompileShader compiles the source once for its errors and info log. glLinkProgram compiles each stage two more
   times. The first time, a binding map collects its samplers, images and blocks. The second time, the map puts each
   resource of one name at one binding of the program's set, the same in every stage: binding 0 is the vertex
   shader's default uniform block, 1 the fragment shader's, 2 the compute shader's, then the resources in the order
   the stages declare them. The fragment shader's inputs then take the locations of the vertex shader's outputs of
   the same names, which the link writes into its module's Location decorations: GL links the stages by name, Vulkan
   by location.

   The loose uniforms are the members of each stage's default block. The link reads their offsets, strides and types
   from the module's block type, flattens structs and arrays of structs into GL's names ("s.a", "arr[2].b"), and
   gives each element a location. glUniform* writes the value into each stage's copy of its block and counts the
   write, and a platform's draw that finds the count moved uploads the copy */
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_state.h"
#include "glslc/glslc.h"
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    GL_OUT_OF_MEMORY_ = 0x0505,
    GL_FRAGMENT_SHADER_ = 0x8b30,
    GL_VERTEX_SHADER_ = 0x8b31,
    GL_COMPUTE_SHADER_ = 0x91b9,
    GL_FLOAT_ = 0x1406,
    GL_INT_ = 0x1404,
    GL_UNSIGNED_INT_ = 0x1405,
    GL_BOOL_ = 0x8b56,
    ARENA_RESERVED = 16, /* offset 0 is no range */
    ARENA_MAX = 1u << 30,
};

/* what a save state holds of the programs, before the arena's bytes */
typedef struct {
    SnxGlShader shaders[SNX_GL_PROGRAM_NAMES]; /* by name - 1, as programs: one space of names */
    SnxGlProgram programs[SNX_GL_PROGRAM_NAMES];
    SnxGlPipeline pipelines[SNX_GL_PIPELINES];
    uint32_t arena_used; /* the arena's bytes in use, from its start; the ranges dropped among them */
    uint32_t arena_dead;
} SnxGlPrograms;

static struct {
    SnxGlPrograms s;
    uint8_t* arena; /* arena_capacity bytes */
    uint32_t arena_capacity;
    SnxGlProgramHost host;
    /* glslc's workspace and module buffer, one block, made at the first compilation */
    uint8_t* compiler;
    GlslcWorkspace workspace;
    uint32_t* words;
    GlslcResult result;
    SnxGlPrograms checked; /* a save state's copy being checked: a check never touches the running state */
} g_gl_program;

#define S g_gl_program.s

[[gnu::format(printf, 1, 2)]] static void say(const char* fmt, ...) {
    char line[700];
    va_list ap;
    if (!g_gl_program.host.say) return;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (line[0] && line[strlen(line) - 1] == '\n') line[strlen(line) - 1] = 0; /* a log's line, said as a line */
    g_gl_program.host.say(line);
}
static void changed(uint32_t program) {
    if (g_gl_program.host.program_changed) g_gl_program.host.program_changed(program);
}
void snx_gl_program_host(const SnxGlProgramHost* host) { g_gl_program.host = host ? *host : (SnxGlProgramHost){0}; }

/* ---- the arena ---- */

static uint32_t round16(uint32_t n) { return (n + 15) & ~15u; }

uint8_t* snx_gl_bytes(SnxGlBytes b) { return b.size ? g_gl_program.arena + b.at : NULL; }

static void arena_drop(SnxGlBytes* b) {
    if (b->size) S.arena_dead += round16(b->size);
    *b = (SnxGlBytes){0};
}
/* a live range copied to fresh at *used, and moved there */
static void arena_move(uint8_t* fresh, uint32_t* used, SnxGlBytes* b) {
    if (!b->size) return;
    memcpy(fresh + *used, g_gl_program.arena + b->at, b->size);
    b->at = *used;
    *used += round16(b->size);
}
/* the live ranges moved to the start of a new arena of capacity bytes: 0, or -1 without memory (nothing moved) */
static int arena_compact(uint32_t capacity) {
    uint8_t* fresh = malloc(capacity);
    uint32_t used = ARENA_RESERVED;
    if (!fresh) return -1;
    memset(fresh, 0, ARENA_RESERVED);
    for (uint32_t i = 0; i < SNX_GL_PROGRAM_NAMES; i++) {
        arena_move(fresh, &used, &S.shaders[i].source);
        arena_move(fresh, &used, &S.shaders[i].log);
        arena_move(fresh, &used, &S.programs[i].log);
        arena_move(fresh, &used, &S.programs[i].bound);
        arena_move(fresh, &used, &S.programs[i].link);
    }
    free(g_gl_program.arena);
    g_gl_program.arena = fresh;
    g_gl_program.arena_capacity = capacity;
    S.arena_used = used;
    S.arena_dead = 0;
    return 0;
}
/* a range of size bytes, zeroed: {0, 0} without room, which the caller turns into GL_OUT_OF_MEMORY */
static SnxGlBytes arena_take(uint32_t size) {
    uint32_t need = round16(size ? size : 1);
    if (!S.arena_used) S.arena_used = ARENA_RESERVED;
    if (need > ARENA_MAX - S.arena_used + S.arena_dead) return (SnxGlBytes){0};
    if (S.arena_used + need > g_gl_program.arena_capacity) {
        uint32_t live = S.arena_used - S.arena_dead,
                 cap = g_gl_program.arena_capacity ? g_gl_program.arena_capacity : 64u << 10;
        while (cap < live + need + (live + need) / 2 && cap < ARENA_MAX) cap *= 2;
        if (cap > ARENA_MAX) cap = ARENA_MAX;
        if (arena_compact(cap) || S.arena_used + need > g_gl_program.arena_capacity) return (SnxGlBytes){0};
    }
    SnxGlBytes b = {S.arena_used, size ? size : 1};
    memset(g_gl_program.arena + b.at, 0, need);
    S.arena_used += need;
    return b;
}
/* a copy of text (and its NUL) in a range of the arena, into *b after dropping what *b held: 0, or -1 without room */
static int arena_text(SnxGlBytes* b, const char* text) {
    size_t n = strlen(text) + 1;
    SnxGlBytes made;
    arena_drop(b);
    if (n > ARENA_MAX) return -1;
    made = arena_take((uint32_t)n);
    if (!made.size) return -1;
    memcpy(g_gl_program.arena + made.at, text, n);
    *b = made;
    return 0;
}
static const char* text_of(SnxGlBytes b) { return b.size ? (const char*)g_gl_program.arena + b.at : ""; }

/* ---- the objects ---- */

SnxGlShader* snx_gl_shader(uint32_t name) {
    return name && name <= SNX_GL_PROGRAM_NAMES && S.shaders[name - 1].used ? &S.shaders[name - 1] : NULL;
}
SnxGlProgram* snx_gl_program(uint32_t name) {
    return name && name <= SNX_GL_PROGRAM_NAMES && S.programs[name - 1].used ? &S.programs[name - 1] : NULL;
}
SnxGlPipeline* snx_gl_pipeline(uint32_t name) {
    return name && name <= SNX_GL_PIPELINES && S.pipelines[name - 1].used ? &S.pipelines[name - 1] : NULL;
}
/* a free name of shaders and programs, 0 (GL_OUT_OF_MEMORY recorded, and said) when every one is used */
static uint32_t new_name(uint32_t ctx) {
    for (uint32_t i = 0; i < SNX_GL_PROGRAM_NAMES; i++)
        if (!S.shaders[i].used && !S.programs[i].used) return i + 1;
    if (ctx) snx_gl_error(ctx, GL_OUT_OF_MEMORY_);
    say("every one of the %d names of shaders and programs is used: GL_OUT_OF_MEMORY", SNX_GL_PROGRAM_NAMES);
    return 0;
}
static void no_room(uint32_t ctx, const char* what) {
    if (ctx) snx_gl_error(ctx, GL_OUT_OF_MEMORY_);
    say("no memory for %s: GL_OUT_OF_MEMORY", what);
}
static void error(uint32_t ctx, uint32_t e) {
    if (ctx) snx_gl_error(ctx, e);
}

uint32_t snx_gl_current_program(uint32_t ctx) { return snx_gl_context(ctx)->program; }
uint32_t snx_gl_current_pipeline(uint32_t ctx) { return snx_gl_context(ctx)->pipeline; }

/* ---- SPIR-V ---- */

enum {
    OP_NAME = 5,
    OP_MEMBER_NAME = 6,
    OP_TYPE_BOOL = 20,
    OP_TYPE_INT = 21,
    OP_TYPE_FLOAT = 22,
    OP_TYPE_VECTOR = 23,
    OP_TYPE_MATRIX = 24,
    OP_TYPE_IMAGE = 25,
    OP_TYPE_SAMPLED_IMAGE = 27,
    OP_TYPE_ARRAY = 28,
    OP_TYPE_RUNTIME_ARRAY = 29,
    OP_TYPE_STRUCT = 30,
    OP_TYPE_POINTER = 32,
    OP_CONSTANT = 43,
    OP_VARIABLE = 59,
    OP_DECORATE = 71,
    OP_MEMBER_DECORATE = 72,
    DEC_ARRAY_STRIDE = 6,
    DEC_MATRIX_STRIDE = 7,
    DEC_LOCATION = 30,
    DEC_BINDING = 33,
    DEC_OFFSET = 35,
    SC_INPUT = 1,
    SC_UNIFORM = 2,
    SC_OUTPUT = 3,
    SC_UNIFORM_CONSTANT = 0,
    SC_STORAGE_BUFFER = 12,
};

/* a module: the word of each id's definition and of its OpName, in one block */
typedef struct {
    uint32_t* w;
    size_t n;
    uint32_t bound;
    uint32_t* def; /* bound entries, then name's */
    uint32_t* name;
} Spv;

static int spv_open(Spv* s, uint32_t* words, size_t n) {
    *s = (Spv){words, n, n > 5 ? words[3] : 0, NULL, NULL};
    if (n <= 5 || !s->bound || s->bound > 4u << 20) return 0;
    s->def = calloc((size_t)s->bound * 2, sizeof *s->def);
    if (!s->def) return 0;
    s->name = s->def + s->bound;
    for (size_t i = 5; i < n;) {
        uint32_t count = s->w[i] >> 16, op = s->w[i] & 0xffff;
        if (!count || i + count > n) break;
        if (op == OP_NAME && s->w[i + 1] < s->bound)
            s->name[s->w[i + 1]] = (uint32_t)i;
        else if ((op >= OP_TYPE_BOOL && op <= OP_TYPE_POINTER) && s->w[i + 1] < s->bound)
            s->def[s->w[i + 1]] = (uint32_t)i;
        else if ((op == OP_CONSTANT || op == OP_VARIABLE) && count > 2 && s->w[i + 2] < s->bound)
            s->def[s->w[i + 2]] = (uint32_t)i;
        i += count;
    }
    return 1;
}
static void spv_close(Spv* s) { free(s->def); }
static uint32_t op(const Spv* s, uint32_t id) { return id < s->bound && s->def[id] ? s->w[s->def[id]] & 0xffff : 0; }
static const uint32_t* at(const Spv* s, uint32_t id) { return &s->w[s->def[id]]; }
static const char* spv_name(const Spv* s, uint32_t id) {
    return id < s->bound && s->name[id] ? (const char*)&s->w[s->name[id] + 2] : "";
}
/* the literal of id's decoration, or of member's when member is not UINT32_MAX */
static int decoration(const Spv* s, uint32_t id, uint32_t member, uint32_t dec, uint32_t* value, uint32_t* word) {
    for (size_t i = 5; i < s->n;) {
        uint32_t count = s->w[i] >> 16, o = s->w[i] & 0xffff;
        if (!count) break;
        if (member == UINT32_MAX && o == OP_DECORATE && count >= 3 && s->w[i + 1] == id && s->w[i + 2] == dec) {
            if (value) *value = count > 3 ? s->w[i + 3] : 0;
            if (word) *word = (uint32_t)i + 3;
            return 1;
        }
        if (member != UINT32_MAX && o == OP_MEMBER_DECORATE && count >= 4 && s->w[i + 1] == id &&
            s->w[i + 2] == member && s->w[i + 3] == dec) {
            if (value) *value = count > 4 ? s->w[i + 4] : 0;
            return 1;
        }
        i += count;
    }
    return 0;
}
static const char* member_name(const Spv* s, uint32_t type, uint32_t member) {
    for (size_t i = 5; i < s->n;) {
        uint32_t count = s->w[i] >> 16, o = s->w[i] & 0xffff;
        if (!count) break;
        if (o == OP_MEMBER_NAME && count >= 3 && s->w[i + 1] == type && s->w[i + 2] == member)
            return (const char*)&s->w[i + 3];
        i += count;
    }
    return "";
}
static uint32_t constant(const Spv* s, uint32_t id) { return op(s, id) == OP_CONSTANT ? at(s, id)[3] : 0; }

/* the GL type of a SPIR-V scalar, vector or matrix type; 0 for others */
static uint32_t gl_type(const Spv* s, uint32_t t) {
    static const uint32_t matrices[3][3] = {
        {0x8b5a, 0x8b65, 0x8b66}, {0x8b67, 0x8b5b, 0x8b68}, {0x8b69, 0x8b6a, 0x8b5c}};
    uint32_t o = op(s, t);
    if (o == OP_TYPE_FLOAT) return GL_FLOAT_;
    if (o == OP_TYPE_INT) return at(s, t)[3] ? GL_INT_ : GL_UNSIGNED_INT_;
    if (o == OP_TYPE_BOOL) return GL_BOOL_;
    if (o == OP_TYPE_VECTOR) {
        uint32_t base = gl_type(s, at(s, t)[2]), n = at(s, t)[3];
        if (n < 2 || n > 4) return 0;
        switch (base) {
            case GL_FLOAT_: return 0x8b50 + n - 2;
            case GL_INT_: return 0x8b53 + n - 2;
            case GL_UNSIGNED_INT_: return 0x8dc6 + n - 2;
            case GL_BOOL_: return 0x8b57 + n - 2;
            default: return 0;
        }
    }
    if (o == OP_TYPE_MATRIX) {
        uint32_t col = at(s, t)[2], cols = at(s, t)[3];
        uint32_t rows = op(s, col) == OP_TYPE_VECTOR ? at(s, col)[3] : 0;
        if (cols < 2 || cols > 4 || rows < 2 || rows > 4) return 0;
        return matrices[cols - 2][rows - 2];
    }
    return 0;
}

/* the sampler's SNX_GL_T* dimension and GL type, from the type a variable points to */
static void sampler_kind(const Spv* s, uint32_t pointee, uint8_t* dim, uint32_t* type) {
    const uint32_t* i;
    uint32_t sdim, depth, arrayed, ms, base;
    int k;
    if (op(s, pointee) == OP_TYPE_ARRAY || op(s, pointee) == OP_TYPE_RUNTIME_ARRAY) pointee = at(s, pointee)[2];
    if (op(s, pointee) == OP_TYPE_SAMPLED_IMAGE) pointee = at(s, pointee)[2];
    *dim = SNX_GL_T2D;
    *type = 0x8b5e; /* GL_SAMPLER_2D */
    if (op(s, pointee) != OP_TYPE_IMAGE) return;
    i = at(s, pointee);
    sdim = i[3], depth = i[4], arrayed = i[5], ms = i[6];
    base = gl_type(s, i[2]);
    k = base == GL_INT_ ? 1 : base == GL_UNSIGNED_INT_ ? 2 : 0;
    if (ms) {
        *dim = SNX_GL_TMS;
        *type = k == 1 ? 0x9109 : k == 2 ? 0x910a : 0x9108;
    } else if (sdim == 2) {
        *dim = SNX_GL_T3D;
        *type = k == 1 ? 0x8dcb : k == 2 ? 0x8dd3 : 0x8b5f;
    } else if (sdim == 3) {
        *dim = SNX_GL_TCUBE;
        *type = depth ? 0x8dc5 : k == 1 ? 0x8dcc : k == 2 ? 0x8dd4 : 0x8b60;
    } else if (sdim == 5) {
        *dim = SNX_GL_T2D;
        *type = k == 1 ? 0x8dd0 : k == 2 ? 0x8dd8 : 0x8dc2; /* buffers */
    } else if (arrayed) {
        *dim = SNX_GL_TARRAY;
        *type = depth ? 0x8dc4 : k == 1 ? 0x8dcf : k == 2 ? 0x8dd7 : 0x8dc1;
    } else {
        *type = depth ? 0x8b62 : k == 1 ? 0x8dca : k == 2 ? 0x8dd2 : 0x8b5e;
    }
}

/* ---- the link's growing arrays, its own while it runs ---- */

typedef struct {
    char kind, member_dot;
    uint8_t texel_buffer;
    uint16_t array;
    int16_t source_binding;
    uint32_t size;
    uint32_t name; /* in names */
    uint32_t stages;
} Resource;

typedef struct {
    Resource* resources;
    uint32_t resource_count;
    SnxGlUniform* uniforms;
    uint32_t uniform_count;
    SnxGlInput* inputs;
    uint32_t input_count;
    SnxGlInput* outputs;
    uint32_t output_count;
    char* names;
    size_t names_size, names_capacity;
    /* the binding map's state */
    const char* source;
    uint32_t stage;
    int final;
    /* the vertex shader's outputs: name offset and location */
    struct {
        uint32_t name, location;
    } varyings[64];
    uint32_t varying_count;
    uint32_t default_size[3];
    uint32_t* spirv[3]; /* each stage's module */
    uint32_t spirv_words[3];
    uint32_t local_size[3];
    int failed;    /* no memory */
    char why[200]; /* what the platform's layout has no place for, "" for nothing */
} Link;

static void* grow(void* p, uint32_t count, size_t size) {
    if (count & (count - 1)) return p; /* room till the next power of 2 */
    return realloc(p, (count ? count * 2 : 8) * size);
}
static uint32_t name_add(Link* l, const char* text, size_t len) {
    uint32_t where;
    if (l->names_size + len + 1 > l->names_capacity) {
        size_t cap = (l->names_capacity ? l->names_capacity * 2 : 1024) + len;
        char* grown = realloc(l->names, cap);
        if (!grown) return l->failed = 1, 0;
        l->names = grown;
        l->names_capacity = cap;
    }
    where = (uint32_t)l->names_size;
    memcpy(l->names + where, text, len);
    l->names[where + len] = 0;
    l->names_size += len + 1;
    return where;
}

/* the binding map: collecting (the first compilation of a stage), then placing (the second) */
/* resource i's binding in the platform's layout (SnxGlLayout): 0, or nonzero with why it has none in l->why */
static int layout_bind(Link* l, uint32_t i, uint32_t* binding) {
    const SnxGlLayout* y = &g_gl_program.host.layout;
    const Resource* x = &l->resources[i];
    uint32_t before = 0, base = x->kind == SNX_GLSLC_REFLECT_SAMPLER ? y->sampler_base : y->block_base + 1;
    uint32_t room = x->kind == SNX_GLSLC_REFLECT_SAMPLER ? y->samplers : y->blocks - 1;
    if (x->kind != SNX_GLSLC_REFLECT_SAMPLER && x->kind != SNX_GLSLC_REFLECT_UNIFORM_BLOCK) {
        snprintf(l->why, sizeof l->why, "%s %s: this platform's GL layer binds no %s yet", l->names + x->name,
                 x->kind == SNX_GLSLC_REFLECT_IMAGE ? "is an image" : "is a storage block",
                 x->kind == SNX_GLSLC_REFLECT_IMAGE ? "images" : "storage blocks");
        return 1;
    }
    if (x->kind == SNX_GLSLC_REFLECT_SAMPLER && x->array) {
        snprintf(l->why, sizeof l->why, "%s is an array of %u samplers: this platform's GL layer binds one a slot",
                 l->names + x->name, (unsigned)x->array);
        return 2;
    }
    for (uint32_t k = 0; k < i; ++k) before += l->resources[k].kind == x->kind;
    if (before >= room) {
        snprintf(l->why, sizeof l->why, "%s is the program's %s number %u, past the %u this platform binds",
                 l->names + x->name, x->kind == SNX_GLSLC_REFLECT_SAMPLER ? "sampler" : "uniform block",
                 (unsigned)before + 1, (unsigned)room);
        return 3;
    }
    *binding = base + before;
    return 0;
}
int snx_gl_layout_block(const SnxGlLayout* y, int stage, uint32_t binding, uint32_t* offset_bit) {
    uint32_t i = binding - y->block_base;
    if (binding < y->block_base || i >= y->blocks || stage < 0 || stage > 2) return SNX_GLSLC_BLOCK_UNIFORM;
    if (i >= y->uniform_blocks) return SNX_GLSLC_BLOCK_STORAGE;
    if (!y->window_push[stage]) return SNX_GLSLC_BLOCK_UNIFORM;
    *offset_bit = y->window_push[stage] * 8 + i / 2 * 32 + (i & 1) * 16;
    return SNX_GLSLC_BLOCK_WINDOW;
}
static int link_block(void* user, uint32_t set, uint32_t binding, uint32_t* offset_bit) {
    const Link* l = user;
    (void)set;
    return snx_gl_layout_block(&g_gl_program.host.layout, l->stage, binding, offset_bit);
}
static int link_bind(void* user, const GlslcResource* r, uint32_t* set, uint32_t* binding) {
    Link* l = user;
    char text[256];
    int n;
    Resource* grown;
    int stage_sets = g_gl_program.host.stage_sets;
    *set = stage_sets ? g_gl_program.host.layout.set[l->stage] : 0;
    if (r->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && !r->name.len) { /* the stage's default block */
        *binding = stage_sets ? g_gl_program.host.layout.block_base : l->stage;
        return 0;
    }
    n = snprintf(text, sizeof text, "%.*s%s%.*s", (int)r->name.len, l->source + r->name.pos, r->member.len ? "." : "",
                 (int)r->member.len, l->source + r->member.pos);
    if (n < 0 || n >= (int)sizeof text) return 1;
    for (uint32_t i = 0; i < l->resource_count; ++i) {
        Resource* x = &l->resources[i];
        if (x->kind == (char)r->kind && !strcmp(l->names + x->name, text)) {
            x->stages |= 1u << l->stage;
            if (stage_sets && r->kind == SNX_GLSLC_REFLECT_SAMPLER)
                *set = g_gl_program.host.layout.sampler_set[l->stage];
            if (stage_sets) return layout_bind(l, i, binding);
            *binding = 3 + i;
            return 0;
        }
    }
    if (l->final) return 2; /* the second compilation declares what the first did */
    grown = grow(l->resources, l->resource_count, sizeof *grown);
    if (!grown) return l->failed = 1, 3;
    l->resources = grown;
    l->resources[l->resource_count] =
        (Resource){(char)r->kind, (char)(r->member.len != 0),   r->texel_buffer, r->array, r->binding,
                   r->size,       name_add(l, text, (size_t)n), 1u << l->stage};
    if (stage_sets && r->kind == SNX_GLSLC_REFLECT_SAMPLER) *set = g_gl_program.host.layout.sampler_set[l->stage];
    if (stage_sets) return layout_bind(l, l->resource_count++, binding);
    *binding = 3 + l->resource_count++;
    return 0;
}

/* glslc's workspace and module buffer, at the first compilation */
static int compiler_ready(void) {
    size_t size = (snx_glslc_workspace_size() + 15) & ~(size_t)15;
    if (g_gl_program.compiler) return 1;
    g_gl_program.compiler = malloc(size + (size_t)SNX_GLSLC_MAX_WORDS * sizeof(uint32_t));
    if (!g_gl_program.compiler) return 0;
    if (snx_glslc_workspace_init(&g_gl_program.workspace, g_gl_program.compiler, size, &g_gl_program.result) != 0) {
        free(g_gl_program.compiler);
        g_gl_program.compiler = NULL;
        return 0;
    }
    g_gl_program.words = (uint32_t*)(g_gl_program.compiler + size);
    return 1;
}

/* what a binding of a stage's module holds: -2 the stage's default block, a resource's index, or -1 none */
static int32_t resource_at(Link* l, uint32_t binding, int stage) {
    if (!g_gl_program.host.stage_sets) {
        if (binding == (uint32_t)stage && binding < 3) return -2;
        return binding >= 3 && binding - 3 < l->resource_count ? (int32_t)(binding - 3) : -1;
    }
    if (binding == g_gl_program.host.layout.block_base) return -2;
    for (uint32_t i = 0; i < l->resource_count; ++i) {
        uint32_t b;
        if (!layout_bind(l, i, &b) && b == binding) return (int32_t)i;
    }
    return -1;
}

static GlslcStage stage_of(uint32_t type) {
    return type == GL_VERTEX_SHADER_     ? SNX_GLSLC_VERTEX
           : type == GL_FRAGMENT_SHADER_ ? SNX_GLSLC_FRAGMENT
                                         : SNX_GLSLC_COMPUTE;
}
static int stage_index(uint32_t type) { return type == GL_VERTEX_SHADER_ ? 0 : type == GL_FRAGMENT_SHADER_ ? 1 : 2; }
/* the shader compiled into g_gl_program.words, with the binding map when l: 0, or -1 with the error in the result.
   A desktop context's shader may be desktop GLSL */
static int compile(uint32_t ctx, const SnxGlShader* sh, Link* l) {
    const char* source = text_of(sh->source);
    GlslcOptions o = {0};
    if (!compiler_ready()) {
        snprintf(g_gl_program.result.error, sizeof g_gl_program.result.error, "glslc has no memory");
        g_gl_program.result.line = g_gl_program.result.column = 0;
        return -1;
    }
    if (l) l->source = source;
    o.stage = stage_of(sh->type);
    o.bind = l ? link_bind : NULL;
    o.bind_user = l;
    o.map_default = l != NULL;
    /* a framebuffer is drawn with a negative viewport height: gl_FragCoord and gl_PointCoord through the draw's flip */
    o.flip_y = sh->type == GL_FRAGMENT_SHADER_ && !g_gl_program.host.stage_sets;
    o.block = l && g_gl_program.host.stage_sets ? link_block : NULL;
    o.desktop = ctx && snx_gl_context_api(ctx) == SNX_GL_API_DESKTOP;
    return snx_glslc_compile(&g_gl_program.workspace, &o, source, sh->source.size ? sh->source.size - 1 : 0,
                             g_gl_program.words, SNX_GLSLC_MAX_WORDS, &g_gl_program.result);
}

/* ---- shaders ---- */

uint32_t snx_gl_create_shader(uint32_t ctx, uint32_t type) {
    uint32_t name;
    if (type != GL_VERTEX_SHADER_ && type != GL_FRAGMENT_SHADER_ && type != GL_COMPUTE_SHADER_) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return 0;
    }
    name = new_name(ctx);
    if (name) S.shaders[name - 1] = (SnxGlShader){.used = 1, .type = type};
    return name;
}
static void shader_free(SnxGlShader* s) {
    arena_drop(&s->source);
    arena_drop(&s->log);
    *s = (SnxGlShader){0};
}
void snx_gl_delete_shader(uint32_t ctx, uint32_t shader) {
    SnxGlShader* s = snx_gl_shader(shader);
    if (!s) {
        if (shader) error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (s->attached)
        s->deleted = 1;
    else
        shader_free(s);
}
int snx_gl_is_shader(uint32_t shader) { return snx_gl_shader(shader) != NULL; }
char* snx_gl_shader_source(uint32_t ctx, uint32_t shader, size_t size) {
    SnxGlShader* s = snx_gl_shader(shader);
    SnxGlBytes made;
    if (!s) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return NULL;
    }
    arena_drop(&s->source);
    made = size < ARENA_MAX ? arena_take((uint32_t)size + 1) : (SnxGlBytes){0};
    if (!made.size) {
        no_room(ctx, "a shader's source");
        return NULL;
    }
    snx_gl_shader(shader)->source = made;
    return (char*)g_gl_program.arena + made.at;
}
void snx_gl_compile_shader(uint32_t ctx, uint32_t shader) {
    SnxGlShader* s = snx_gl_shader(shader);
    char text[600];
    int compiled;
    if (!s) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    compiled = s->source.size && compile(ctx, s, NULL) == 0;
    if (!compiled)
        snprintf(text, sizeof text, "ERROR: %u:%u: %s\n", g_gl_program.result.line, g_gl_program.result.column,
                 s->source.size ? g_gl_program.result.error : "no source");
    else if (g_gl_program.result.warnings)
        snprintf(text, sizeof text, "WARNING: %s\n", g_gl_program.result.warning);
    else
        text[0] = 0;
    s->compiled = (uint8_t)compiled;
    if (arena_text(&s->log, text)) no_room(ctx, "a shader's log");
    if (!compiled) say("a shader does not compile: %s", text);
}
int snx_gl_get_shaderiv(uint32_t ctx, uint32_t shader, uint32_t pname, int32_t* out) {
    const SnxGlShader* s = snx_gl_shader(shader);
    if (!s) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return 0;
    }
    switch (pname) {
        case 0x8b4f: *out = (int32_t)s->type; return 1; /* GL_SHADER_TYPE */
        case 0x8b80: *out = s->deleted; return 1;       /* GL_DELETE_STATUS */
        case 0x8b81: *out = s->compiled; return 1;      /* GL_COMPILE_STATUS */
        case 0x8b84: *out = s->log.size > 1 ? (int32_t)strlen(text_of(s->log)) + 1 : 0; return 1; /* INFO_LOG_LENGTH */
        case 0x8b88: *out = s->source.size ? (int32_t)s->source.size : 0; return 1; /* GL_SHADER_SOURCE_LENGTH */
        default: error(ctx, SNX_GL_INVALID_ENUM); return 0;
    }
}
void snx_gl_string_out(const char* text, int32_t size, void* length, char* out) {
    size_t n = text ? strlen(text) : 0;
    if (size <= 0)
        n = 0;
    else if (n > (size_t)size - 1)
        n = (size_t)size - 1;
    if (out && size > 0) {
        if (n) memcpy(out, text, n);
        out[n] = 0;
    }
    if (length) {
        int32_t l = (int32_t)n;
        memcpy(length, &l, 4);
    }
}
const char* snx_gl_shader_info_log(uint32_t ctx, uint32_t shader) {
    const SnxGlShader* s = snx_gl_shader(shader);
    if (!s) return error(ctx, SNX_GL_INVALID_VALUE), NULL;
    return text_of(s->log);
}
const char* snx_gl_shader_text(uint32_t ctx, uint32_t shader) {
    const SnxGlShader* s = snx_gl_shader(shader);
    if (!s) return error(ctx, SNX_GL_INVALID_VALUE), NULL;
    return text_of(s->source);
}
void snx_gl_shader_precision_format(uint32_t precision, int32_t range[2], int32_t* bits) {
    if (precision <= 0x8df2) { /* GL_LOW_FLOAT, GL_MEDIUM_FLOAT, GL_HIGH_FLOAT */
        int high = precision == 0x8df2;
        range[0] = range[1] = high ? 127 : 15;
        *bits = high ? 23 : 10;
    } else {
        int high = precision == 0x8df5;
        range[0] = high ? 31 : 15;
        range[1] = high ? 30 : 14;
        *bits = 0;
    }
}

/* ---- programs ---- */

static void program_unlink(uint32_t name) {
    SnxGlProgram* p = &S.programs[name - 1];
    if (p->link.size) changed(name);
    arena_drop(&p->link);
    p->bindings = p->uniforms = p->locations = p->units = p->inputs = p->outputs = p->names = 0;
    for (int i = 0; i < 3; ++i)
        p->defaults[i] = p->spirv[i] = p->default_size[i] = p->spirv_words[i] = p->default_serial[i] = 0;
    p->binding_count = p->uniform_count = p->location_count = p->input_count = p->output_count = 0;
    p->unit_count = p->block_count = p->names_size = 0;
    p->linked = 0;
}

uint32_t snx_gl_create_program(uint32_t ctx) {
    uint32_t name = new_name(ctx);
    if (name) S.programs[name - 1] = (SnxGlProgram){.used = 1};
    return name;
}
static void detach(SnxGlProgram* p, int i) {
    SnxGlShader* s = snx_gl_shader(p->shaders[i]);
    p->shaders[i] = 0;
    if (!s) return;
    if (s->attached) s->attached--;
    if (!s->attached && s->deleted) shader_free(s);
}
static void program_free(uint32_t name) {
    SnxGlProgram* p = &S.programs[name - 1];
    program_unlink(name);
    for (int i = 0; i < 3; ++i) detach(p, i);
    arena_drop(&p->log);
    arena_drop(&p->bound);
    *p = (SnxGlProgram){0};
    changed(name);
}
void snx_gl_delete_program(uint32_t ctx, uint32_t program) {
    if (!snx_gl_program(program)) {
        if (program) error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (ctx && snx_gl_current_program(ctx) == program)
        S.programs[program - 1].deleted = 1;
    else
        program_free(program);
}
int snx_gl_is_program(uint32_t program) { return snx_gl_program(program) != NULL; }
void snx_gl_attach_shader(uint32_t ctx, uint32_t program, uint32_t shader) {
    SnxGlProgram* p = snx_gl_program(program);
    SnxGlShader* s = snx_gl_shader(shader);
    int i;
    if (!p || !s) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    i = stage_index(s->type);
    if (p->shaders[i]) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    p->shaders[i] = shader;
    s->attached++;
}
void snx_gl_detach_shader(uint32_t ctx, uint32_t program, uint32_t shader) {
    SnxGlProgram* p = snx_gl_program(program);
    const SnxGlShader* s = snx_gl_shader(shader);
    int i;
    if (!p || !s) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    i = stage_index(s->type);
    if (p->shaders[i] == shader)
        detach(p, i);
    else
        error(ctx, SNX_GL_INVALID_OPERATION);
}
int32_t snx_gl_attached_shaders(uint32_t ctx, uint32_t program, int32_t max, uint32_t* out) {
    const SnxGlProgram* p = snx_gl_program(program);
    int32_t n = 0;
    if (!p) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return -1;
    }
    for (int i = 0; i < 3; ++i)
        if (p->shaders[i] && n < max) memcpy(out + n++, &p->shaders[i], 4);
    return n;
}
void snx_gl_bind_attrib_location(uint32_t ctx, uint32_t program, uint32_t index, const char* name) {
    SnxGlProgram* p = snx_gl_program(program);
    size_t n, old;
    SnxGlBytes made;
    if (!p || index >= SNX_GL_ATTRIBS || !name) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    n = strlen(name) + 1;
    old = p->bound.size;
    if (old + n + 4 >= ARENA_MAX) {
        no_room(ctx, "glBindAttribLocation's name");
        return;
    }
    made = arena_take((uint32_t)(old + n + 4));
    if (!made.size) {
        no_room(ctx, "glBindAttribLocation's name");
        return;
    }
    p = &S.programs[program - 1];
    if (old) memcpy(g_gl_program.arena + made.at, g_gl_program.arena + p->bound.at, old);
    memcpy(g_gl_program.arena + made.at + old, name, n);
    memcpy(g_gl_program.arena + made.at + old + n, &index, 4);
    arena_drop(&p->bound);
    p->bound = made;
}
static int32_t bound_location(const SnxGlProgram* p, const char* name) {
    const char* bound = (const char*)snx_gl_bytes(p->bound);
    int32_t found = -1;
    for (size_t i = 0; i < p->bound.size;) {
        size_t n = strlen(bound + i) + 1;
        if (!strcmp(bound + i, name)) memcpy(&found, bound + i + n, 4); /* the last bind of it */
        i += n + 4;
    }
    return found;
}

/* ---- the link ---- */

static SnxGlUniform* uniform_named(Link* l, const char* name) {
    for (uint32_t i = 0; i < l->uniform_count; ++i)
        if (!strcmp(l->names + l->uniforms[i].name, name)) return &l->uniforms[i];
    return NULL;
}
static SnxGlUniform* uniform_add(Link* l, const char* name) {
    SnxGlUniform *u = uniform_named(l, name), *grown;
    uint32_t text;
    if (u) return u;
    text = name_add(l, name, strlen(name));
    grown = grow(l->uniforms, l->uniform_count, sizeof *grown);
    if (!grown) return l->failed = 1, NULL;
    l->uniforms = grown;
    u = &l->uniforms[l->uniform_count++];
    *u = (SnxGlUniform){.name = text, .count = 1, .block = -1, .offset = {-1, -1, -1}, .location = -1, .sampler = -1};
    return u;
}

/* the members of type at offset, as GL names them under prefix: each a uniform of the stage (block: its index) */
/* whether member m of struct type holds a GLSL bool, which the module keeps as an unsigned integer (glslc's bools) */
static int bool_member(uint32_t type, uint32_t m) {
    const GlslcResult* r = &g_gl_program.result;
    for (uint32_t i = 0; i < r->bool_count; ++i)
        if (r->bools[i].type == type && r->bools[i].member == m) return 1;
    return 0;
}
/* the GL type of a uniform of SPIR-V type t, a bool's (GL_BOOL, GL_BOOL_VEC*) where it holds GLSL bools */
static uint32_t uniform_type(const Spv* s, uint32_t t, int boolean) {
    uint32_t type = gl_type(s, t);
    if (!boolean) return type;
    return type == GL_UNSIGNED_INT_ ? GL_BOOL_ : type >= 0x8dc6 && type <= 0x8dc8 ? 0x8b57 + (type - 0x8dc6) : type;
}
static void flatten(Link* l, const Spv* s, uint32_t type, const char* prefix, uint32_t offset, uint32_t mstride,
                    int stage, int block, int boolean) {
    char name[256];
    uint32_t o = op(s, type);
    SnxGlUniform* u;
    if (o == OP_TYPE_STRUCT) {
        uint32_t count = (at(s, type)[0] >> 16) - 2;
        for (uint32_t m = 0; m < count; ++m) {
            uint32_t off = 0, ms = 0;
            decoration(s, type, m, DEC_OFFSET, &off, NULL);
            decoration(s, type, m, DEC_MATRIX_STRIDE, &ms, NULL);
            snprintf(name, sizeof name, "%s%s%s", prefix, prefix[0] ? "." : "", member_name(s, type, m));
            flatten(l, s, at(s, type)[2 + m], name, offset + off, ms, stage, block, bool_member(type, m));
        }
        return;
    }
    if (o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) {
        uint32_t elem = at(s, type)[2], len = o == OP_TYPE_ARRAY ? constant(s, at(s, type)[3]) : 1, stride = 0;
        decoration(s, type, UINT32_MAX, DEC_ARRAY_STRIDE, &stride, NULL);
        if (op(s, elem) == OP_TYPE_STRUCT || op(s, elem) == OP_TYPE_ARRAY) {
            for (uint32_t i = 0; i < len && i < 256; ++i) {
                snprintf(name, sizeof name, "%s[%u]", prefix, i);
                flatten(l, s, elem, name, offset + i * stride, mstride, stage, block, boolean);
            }
            return;
        }
        snprintf(name, sizeof name, "%s[0]", prefix);
        u = uniform_add(l, name);
        if (!u) return;
        u->type = uniform_type(s, elem, boolean);
        u->count = (uint16_t)(len ? len : 1);
        u->array_stride = (uint16_t)stride;
        u->matrix_stride = (uint16_t)mstride;
        u->offset[stage] = (int32_t)offset;
        u->block = (int16_t)block;
        return;
    }
    u = uniform_add(l, prefix);
    if (!u) return;
    u->type = uniform_type(s, type, boolean);
    u->matrix_stride = (uint16_t)mstride;
    u->offset[stage] = (int32_t)offset;
    u->block = (int16_t)block;
}

/* what a stage's module says: its default block's uniforms, its samplers' kinds, the vertex shader's outputs, and
   the fragment shader's inputs moved to them */
static void reflect_module(Link* l, uint32_t* words, size_t n, int stage) {
    Spv s;
    if (!spv_open(&s, words, n)) {
        l->failed = 1;
        return;
    }
    for (size_t i = 5; stage == 2 && i < n;) { /* OpExecutionMode LocalSize */
        uint32_t count = words[i] >> 16;
        if (!count) break;
        if ((words[i] & 0xffff) == 16 && count >= 6 && words[i + 2] == 17)
            for (int k = 0; k < 3; ++k) l->local_size[k] = words[i + 3 + k];
        i += count;
    }
    for (uint32_t id = 1; id < s.bound; ++id) {
        const uint32_t* v;
        uint32_t storage, pointer, pointee, binding = 0, location = 0, word = 0;
        if (op(&s, id) != OP_VARIABLE) continue;
        v = at(&s, id);
        storage = v[3], pointer = v[1];
        if (op(&s, pointer) != OP_TYPE_POINTER) continue;
        pointee = at(&s, pointer)[3];
        /* a block read as a storage buffer is one too (snx_gl_layout_block); a window's reads go through a variable
           of its own beside the block's (glslc's gl_ScarletWindow), which holds no uniform */
        if (!strcmp(spv_name(&s, pointee), "gl_ScarletWindow")) continue;
        if ((storage == SC_UNIFORM || storage == SC_STORAGE_BUFFER) &&
            decoration(&s, id, UINT32_MAX, DEC_BINDING, &binding, NULL)) {
            int32_t held = resource_at(l, binding, stage);
            if (held == -2) { /* the default block */
                flatten(l, &s, pointee, "", 0, 0, stage, -1, 0);
            } else if (held >= 0) {
                Resource* r = &l->resources[held];
                if (r->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK) {
                    uint32_t index = 0, t;
                    for (int32_t i = 0; i < held; ++i) index += l->resources[i].kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK;
                    t = op(&s, pointee) == OP_TYPE_ARRAY ? at(&s, pointee)[2] : pointee;
                    flatten(l, &s, t, "", 0, 0, stage, (int)index, 0);
                }
            }
        } else if (storage == SC_UNIFORM_CONSTANT && decoration(&s, id, UINT32_MAX, DEC_BINDING, &binding, NULL) &&
                   resource_at(l, binding, stage) >= 0) {
            Resource* r = &l->resources[resource_at(l, binding, stage)];
            uint8_t dim;
            uint32_t type;
            sampler_kind(&s, pointee, &dim, &type);
            r->size = type; /* a sampler's size field holds its GL type, and texel_buffer's high bits its dimension */
            r->texel_buffer = (uint8_t)(r->texel_buffer | (uint8_t)(dim << 4));
        } else if (storage == SC_OUTPUT && stage == 0 &&
                   decoration(&s, id, UINT32_MAX, DEC_LOCATION, &location, NULL) && l->varying_count < 64) {
            const char* name = spv_name(&s, id);
            l->varyings[l->varying_count].name = name_add(l, name, strlen(name));
            l->varyings[l->varying_count++].location = location;
        } else if (storage == SC_INPUT && stage == 1 &&
                   decoration(&s, id, UINT32_MAX, DEC_LOCATION, &location, &word)) {
            const char* name = spv_name(&s, id);
            for (uint32_t i = 0; i < l->varying_count; ++i)
                if (!strcmp(l->names + l->varyings[i].name, name)) words[word] = l->varyings[i].location;
        }
    }
    spv_close(&s);
}

/* the interface the reflection lists of a stage give: the vertex inputs and the fragment outputs */
static void interface(Link* l, const char* source, int stage) {
    static const uint32_t scalars[4] = {GL_BOOL_, GL_INT_, GL_UNSIGNED_INT_, GL_FLOAT_};
    static const uint32_t matrices[3][3] = {
        {0x8b5a, 0x8b65, 0x8b66}, {0x8b67, 0x8b5b, 0x8b68}, {0x8b69, 0x8b6a, 0x8b5c}};
    const GlslcResult* r = &g_gl_program.result;
    for (uint32_t i = 0; i < r->reflect_count; ++i) {
        const GlslcReflect* e = &r->reflect[i];
        int input = e->kind == SNX_GLSLC_REFLECT_INPUT && stage == 0;
        int output = e->kind == SNX_GLSLC_REFLECT_OUTPUT && stage == 1;
        SnxGlInput** list;
        SnxGlInput* grown;
        uint32_t *count, type, name;
        if (!input && !output) continue;
        if (e->name.len >= 3 && !memcmp(source + e->name.pos, "gl_", 3)) continue;
        list = input ? &l->inputs : &l->outputs;
        count = input ? &l->input_count : &l->output_count;
        name = name_add(l, source + e->name.pos, e->name.len);
        grown = grow(*list, *count, sizeof *grown);
        if (!grown) {
            l->failed = 1;
            return;
        }
        *list = grown;
        type = e->base < 4 ? scalars[e->base] : GL_FLOAT_;
        if (e->cols > 1)
            type = matrices[e->cols - 2][e->rows - 2];
        else if (e->rows > 1)
            type = e->base == SNX_GLSLC_BASE_FLOAT ? 0x8b50 + e->rows - 2u
                   : e->base == SNX_GLSLC_BASE_INT ? 0x8b53 + e->rows - 2u
                                                   : 0x8dc6 + e->rows - 2u;
        (*list)[(*count)++] =
            (SnxGlInput){name, type, (uint16_t)(e->array ? e->array : 1), e->base, e->cols, -1, (int32_t)e->location};
    }
}

/* the GL locations of the vertex inputs: glBindAttribLocation's, else the module's when free, else the first free */
static void attribute_locations(Link* l, const SnxGlProgram* p) {
    uint32_t taken = 0;
    for (uint32_t i = 0; i < l->input_count; ++i) {
        SnxGlInput* in = &l->inputs[i];
        int32_t b = bound_location(p, l->names + in->name);
        if (b >= 0) {
            in->location = b;
            for (uint32_t k = 0; k < (uint32_t)in->count * in->columns && b + k < 32; ++k) taken |= 1u << (b + k);
        }
    }
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < l->input_count; ++i) {
            SnxGlInput* in = &l->inputs[i];
            uint32_t span = (uint32_t)in->count * in->columns, mask = span >= 32 ? 0xffffffffu : (1u << span) - 1;
            int32_t want = -1;
            if (in->location >= 0) continue;
            if (!pass && in->spirv >= 0 && in->spirv + span <= SNX_GL_ATTRIBS && !(taken & (mask << in->spirv)))
                want = in->spirv;
            for (uint32_t loc = 0; pass && want < 0 && loc + span <= SNX_GL_ATTRIBS; ++loc)
                if (!(taken & (mask << loc))) want = (int32_t)loc;
            if (want >= 0) {
                in->location = want;
                taken |= mask << want;
            }
        }
}

/* the samplers as GL's uniforms, after the others: one for each, or one for each element of an array of structs'
   member (ms[i].member). Each takes its first unit in the program's units, in the order of the resources */
static void sampler_uniforms(Link* l) {
    uint32_t unit = 0;
    for (uint32_t i = 0; i < l->resource_count; ++i) {
        const Resource* r = &l->resources[i];
        uint32_t count = r->array ? r->array : 1;
        char name[300];
        const char* dot;
        int struct_array;
        if (r->kind != SNX_GLSLC_REFLECT_SAMPLER) continue;
        snprintf(name, sizeof name, "%s", l->names + r->name);
        dot = strchr(name, '.');
        struct_array = dot && r->array;
        for (uint32_t k = 0; k < (struct_array ? count : 1u); ++k) {
            char text[320];
            SnxGlUniform* u;
            if (struct_array)
                snprintf(text, sizeof text, "%.*s[%u]%s", (int)(dot - name), name, k, dot);
            else if (r->array)
                snprintf(text, sizeof text, "%s[0]", name);
            else
                snprintf(text, sizeof text, "%s", name);
            u = uniform_add(l, text);
            if (!u) return;
            u->type = r->size;
            u->count = (uint16_t)(struct_array ? 1 : count);
            u->sampler = (int32_t)(unit + (struct_array ? k : 0));
        }
        unit += count;
    }
}

/* the link's arrays into the program's one range of the arena: 0, or -1 without room */
static int program_arrays(uint32_t name, Link* l) {
    uint32_t bindings = l->resource_count + 3, units = 0, locations = 0, offsets[13], total = 0;
    size_t sizes[13];
    SnxGlBytes block;
    SnxGlProgram* p;
    uint8_t* base;
    SnxGlBinding* pb;
    SnxGlUniform* pu;
    int32_t *pl, *pn;
    uint32_t nb = 0, unit = 0, block_index = 0, loc = 0;
    for (uint32_t i = 0; i < l->resource_count; ++i)
        if (l->resources[i].kind == SNX_GLSLC_REFLECT_SAMPLER)
            units += l->resources[i].array ? l->resources[i].array : 1;
    for (uint32_t i = 0; i < l->uniform_count; ++i)
        if (l->uniforms[i].block < 0) locations += l->uniforms[i].count;
    sizes[0] = bindings * sizeof(SnxGlBinding);
    sizes[1] = l->uniform_count * sizeof(SnxGlUniform);
    sizes[2] = locations * sizeof(int32_t);
    sizes[3] = units * sizeof(int32_t);
    sizes[4] = l->input_count * sizeof(SnxGlInput);
    sizes[5] = l->output_count * sizeof(SnxGlInput);
    sizes[6] = l->names_size + 1;
    for (int i = 0; i < 3; ++i) sizes[7 + i] = l->default_size[i], sizes[10 + i] = l->spirv_words[i] * 4u;
    for (int i = 0; i < 13; ++i) {
        if (sizes[i] > ARENA_MAX - total) return -1;
        offsets[i] = total;
        total += round16((uint32_t)sizes[i]);
    }
    block = arena_take(total);
    if (!block.size) return -1;
    p = &S.programs[name - 1];
    p->link = block;
    p->bindings = offsets[0], p->uniforms = offsets[1], p->locations = offsets[2], p->units = offsets[3];
    p->inputs = offsets[4], p->outputs = offsets[5], p->names = offsets[6];
    base = g_gl_program.arena + block.at;
    for (int i = 0; i < 3; ++i) {
        p->defaults[i] = offsets[7 + i];
        p->default_size[i] = l->default_size[i];
        p->default_serial[i]++;
        p->spirv[i] = offsets[10 + i];
        p->spirv_words[i] = l->spirv_words[i];
        if (l->spirv_words[i]) memcpy(base + offsets[10 + i], l->spirv[i], l->spirv_words[i] * 4u);
        p->local_size[i] = l->local_size[i];
    }
    if (l->names_size) memcpy(base + offsets[6], l->names, l->names_size);
    p->names_size = (uint32_t)l->names_size;
    /* the bindings: the default blocks, then the resources */
    pb = (SnxGlBinding*)(base + offsets[0]);
    pn = (int32_t*)(base + offsets[3]);
    for (uint32_t i = 0; i < 3; ++i)
        if (l->default_size[i])
            pb[nb++] = (SnxGlBinding){
                .kind = SNX_GLSLC_REFLECT_UNIFORM_BLOCK,
                .stage = (uint8_t)i,
                .count = 1,
                .binding = (uint16_t)(g_gl_program.host.stage_sets ? g_gl_program.host.layout.block_base : i),
                .stages = (uint8_t)(1u << i),
                .size = l->default_size[i]};
    for (uint32_t i = 0; i < l->resource_count; ++i) {
        const Resource* r = &l->resources[i];
        SnxGlBinding* b = &pb[nb++];
        uint32_t binding = 3 + i;
        if (g_gl_program.host.stage_sets) layout_bind(l, i, &binding); /* placed by the link, which it passed */
        *b = (SnxGlBinding){.name = r->name,
                            .kind = (uint8_t)r->kind,
                            .count = (uint16_t)(r->array ? r->array : 1),
                            .binding = (uint16_t)binding,
                            .stages = (uint8_t)r->stages,
                            .size = r->size};
        if (r->kind == SNX_GLSLC_REFLECT_SAMPLER) {
            b->texel_buffer = r->texel_buffer & 1;
            b->dim = (uint8_t)(r->texel_buffer >> 4);
            b->first = (uint16_t)unit;
            for (uint32_t k = 0; k < b->count; ++k)
                pn[unit + k] = r->source_binding >= 0 ? r->source_binding + (int32_t)k : 0;
            unit += b->count;
        } else if (r->kind == SNX_GLSLC_REFLECT_IMAGE || r->kind == SNX_GLSLC_REFLECT_BUFFER_BLOCK) {
            /* GLES 3.1 gives their image units and binding points in the source alone */
            b->first = (uint16_t)(r->source_binding >= 0 ? r->source_binding : 0);
            b->dim = (uint8_t)(r->texel_buffer >> 4);
            b->texel_buffer = r->texel_buffer & 1;
        } else if (r->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK) {
            b->first = (uint16_t)block_index;
            if (block_index < SNX_GL_UBO_BINDINGS)
                p->block_binding[block_index] = r->source_binding >= 0 ? (uint32_t)r->source_binding : 0;
            block_index++;
        }
    }
    p->binding_count = nb;
    p->unit_count = unit;
    p->block_count = block_index;
    /* the uniforms, each element of the default block's and the samplers' with a location */
    pu = (SnxGlUniform*)(base + offsets[1]);
    pl = (int32_t*)(base + offsets[2]);
    if (l->uniform_count) memcpy(pu, l->uniforms, l->uniform_count * sizeof *pu);
    p->uniform_count = l->uniform_count;
    for (uint32_t i = 0; i < p->uniform_count; ++i) {
        SnxGlUniform* u = &pu[i];
        if (u->block >= 0) continue;
        u->location = (int32_t)loc;
        for (uint32_t k = 0; k < u->count; ++k) pl[loc++] = (int32_t)(i * 65536 + k);
    }
    p->location_count = loc;
    if (l->input_count) memcpy(base + offsets[4], l->inputs, l->input_count * sizeof(SnxGlInput));
    p->input_count = l->input_count;
    if (l->output_count) memcpy(base + offsets[5], l->outputs, l->output_count * sizeof(SnxGlInput));
    p->output_count = l->output_count;
    return 0;
}

/* the program's log: text, and the link failed when it is not "" */
static void link_log(uint32_t ctx, uint32_t name, const char* text) {
    SnxGlProgram* p = &S.programs[name - 1];
    if (arena_text(&p->log, text)) no_room(ctx, "a program's log");
    if (text[0]) {
        p->linked = 0;
        say("a program does not link: %s", text);
    }
}

/* the program's stages compiled and linked */
static void link(uint32_t ctx, uint32_t name) {
    SnxGlProgram* p = &S.programs[name - 1];
    Link* l = calloc(1, sizeof *l);
    const SnxGlShader* stages[3];
    int compute;
    program_unlink(name);
    p->generation++;
    if (!l) {
        link_log(ctx, name, "no memory for the link");
        return;
    }
    name_add(l, "", 0); /* offset 0 is the empty name: a default block's */
    for (int i = 0; i < 3; ++i) stages[i] = snx_gl_shader(p->shaders[i]);
    compute = stages[2] != NULL;
    if (!compute && (!stages[0] || !stages[1]) && (!p->separable || (!stages[0] && !stages[1]))) {
        link_log(ctx, name, "a program needs a vertex and a fragment shader");
        goto done;
    }
    /* the first compilations: the resources of every stage */
    for (int i = 0; i < 3; ++i) {
        if (!stages[i]) continue;
        if (!stages[i]->compiled || !stages[i]->source.size) {
            link_log(ctx, name, "a shader of the program did not compile");
            goto done;
        }
        l->stage = (uint32_t)i;
        if (compile(ctx, stages[i], l) != 0) {
            link_log(ctx, name, l->why[0] ? l->why : g_gl_program.result.error);
            goto done;
        }
    }
    /* the second: the modules, in order, the vertex shader's outputs before the fragment shader's inputs */
    l->final = 1;
    for (int i = 0; i < 3; ++i) {
        if (!stages[i]) continue;
        l->stage = (uint32_t)i;
        if (compile(ctx, stages[i], l) != 0) {
            link_log(ctx, name, l->why[0] ? l->why : g_gl_program.result.error);
            goto done;
        }
        for (uint32_t k = 0; k < g_gl_program.result.reflect_count; ++k) {
            const GlslcReflect* e = &g_gl_program.result.reflect[k];
            if (e->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && !e->name.len) l->default_size[i] = e->offset;
        }
        interface(l, text_of(stages[i]->source), i);
        reflect_module(l, g_gl_program.words, g_gl_program.result.words, i);
        l->spirv[i] = malloc((size_t)g_gl_program.result.words * 4);
        if (!l->spirv[i]) l->failed = 1;
        if (l->failed) {
            link_log(ctx, name, "no memory for the link");
            goto done;
        }
        memcpy(l->spirv[i], g_gl_program.words, (size_t)g_gl_program.result.words * 4);
        l->spirv_words[i] = g_gl_program.result.words;
    }
    attribute_locations(l, p);
    sampler_uniforms(l);
    if (l->failed || program_arrays(name, l)) {
        link_log(ctx, name, "no memory for the program");
        goto done;
    }
    S.programs[name - 1].linked = 1;
    link_log(ctx, name, "");
done:
    for (int i = 0; i < 3; ++i) free(l->spirv[i]);
    free(l->resources);
    free(l->uniforms);
    free(l->inputs);
    free(l->outputs);
    free(l->names);
    free(l);
}
void snx_gl_link_program(uint32_t ctx, uint32_t program) {
    if (!snx_gl_program(program)) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    link(ctx, program);
}

/* ---- the program in use, and its uniforms ---- */

void snx_gl_use_program(uint32_t ctx, uint32_t program) {
    const SnxGlProgram* p = snx_gl_program(program);
    uint32_t old = snx_gl_current_program(ctx);
    const SnxGlProgram* was = snx_gl_program(old);
    if (program && (!p || !p->linked)) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (was && was->deleted && old != program) program_free(old);
    snx_gl_context(ctx)->program = program;
}
void snx_gl_validate_program(uint32_t ctx, uint32_t program) {
    if (!snx_gl_program(program)) error(ctx, SNX_GL_INVALID_VALUE);
}

/* the components of a GL type, and its columns (1 for a scalar or vector) */
static int components(uint32_t type, int* columns) {
    *columns = 1;
    switch (type) {
        case 0x8b50:
        case 0x8b53:
        case 0x8dc6:
        case 0x8b57: return 2;
        case 0x8b51:
        case 0x8b54:
        case 0x8dc7:
        case 0x8b58: return 3;
        case 0x8b52:
        case 0x8b55:
        case 0x8dc8:
        case 0x8b59: return 4;
        case 0x8b5a: *columns = 2; return 4;
        case 0x8b5b: *columns = 3; return 9;
        case 0x8b5c: *columns = 4; return 16;
        case 0x8b65: *columns = 2; return 6;  /* mat2x3 */
        case 0x8b66: *columns = 2; return 8;  /* mat2x4 */
        case 0x8b67: *columns = 3; return 6;  /* mat3x2 */
        case 0x8b68: *columns = 3; return 12; /* mat3x4 */
        case 0x8b69: *columns = 4; return 8;  /* mat4x2 */
        case 0x8b6a: *columns = 4; return 12; /* mat4x3 */
        default: return 1;
    }
}
static int is_bool(uint32_t type) { return type == GL_BOOL_ || (type >= 0x8b57 && type <= 0x8b59); }

/* the program glUniform* writes: the current one, else the bound pipeline's active program */
static uint32_t uniform_program(uint32_t ctx) {
    uint32_t current = snx_gl_current_program(ctx);
    const SnxGlPipeline* pp;
    if (current || !snx_gl_current_pipeline(ctx)) return current;
    pp = snx_gl_pipeline(snx_gl_current_pipeline(ctx));
    return pp ? pp->active : 0;
}
/* the uniform and element of a location of a program: NULL (and the error) for none, or for location -1 */
static const SnxGlUniform* uniform_at(uint32_t ctx, const SnxGlProgram* p, int32_t location, uint32_t* element) {
    int32_t e;
    if (!p || !p->linked || location < -1 || location >= (int32_t)p->location_count) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return NULL;
    }
    if (location == -1) return NULL;
    memcpy(&e, snx_gl_bytes(p->link) + p->locations + 4 * (uint32_t)location, 4);
    *element = (uint32_t)e & 0xffff;
    return &snx_gl_program_uniforms(p)[(uint32_t)e >> 16];
}

static void uniform_write(uint32_t ctx, uint32_t name, int32_t location, int32_t count, const void* data, int comps,
                          char kind, int transpose) {
    SnxGlProgram* p = snx_gl_program(name);
    uint32_t element = 0;
    const SnxGlUniform* u = uniform_at(ctx, p, location, &element);
    const unsigned char* bytes = data;
    int columns, n, rows;
    if (!u || count <= 0) return;
    if ((uint32_t)count > u->count - element) count = (int32_t)(u->count - element);
    if (u->sampler >= 0) {
        int32_t* units = snx_gl_program_units(p);
        for (int32_t k = 0; k < count; ++k) {
            int32_t unit;
            memcpy(&unit, bytes + 4 * (size_t)k, 4);
            if (kind == 'f') {
                float f;
                memcpy(&f, bytes + 4 * (size_t)k, 4);
                unit = (int32_t)f;
            }
            units[u->sampler + (int32_t)element + k] = unit;
        }
        return;
    }
    n = components(u->type, &columns);
    if (n != comps) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    rows = n / columns;
    for (int s = 0; s < 3; ++s) {
        uint8_t* block = snx_gl_program_default(p, s);
        if (u->offset[s] < 0 || !p->default_size[s]) continue;
        for (int32_t k = 0; k < count; ++k) {
            size_t base = (size_t)u->offset[s] + (size_t)(element + (uint32_t)k) * u->array_stride;
            const unsigned char* v = bytes + (size_t)k * (size_t)n * 4;
            for (int col = 0; col < columns; ++col)
                for (int row = 0; row < rows; ++row) {
                    size_t to = base + (size_t)col * (columns > 1 ? u->matrix_stride : 0) + (size_t)row * 4;
                    int from = transpose ? row * columns + col : col * rows + row;
                    uint32_t bits;
                    if (to + 4 > p->default_size[s]) continue;
                    memcpy(&bits, v + 4 * from, 4);
                    if (is_bool(u->type)) {
                        float f;
                        memcpy(&f, &bits, 4);
                        bits = kind == 'f' ? f != 0 : bits != 0;
                    }
                    memcpy(block + to, &bits, 4);
                }
        }
        p->default_serial[s]++;
    }
}

void snx_gl_get_uniform(uint32_t ctx, uint32_t program, int32_t location, char kind, void* out) {
    const SnxGlProgram* p = snx_gl_program(program);
    const SnxGlUniform* u;
    uint32_t element = 0;
    unsigned char* o = out;
    int columns, n, rows;
    if (!p || !p->linked || location < 0 || location >= (int32_t)p->location_count) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    u = uniform_at(ctx, p, location, &element);
    if (u->sampler >= 0) {
        int32_t unit = snx_gl_program_units(p)[u->sampler + (int32_t)element];
        if (kind == 'f') {
            float f = (float)unit;
            memcpy(o, &f, 4);
        } else {
            memcpy(o, &unit, 4);
        }
        return;
    }
    n = components(u->type, &columns), rows = n / columns;
    for (int s = 0; s < 3; ++s) {
        const uint8_t* block = snx_gl_program_default(p, s);
        size_t base;
        int fl;
        if (u->offset[s] < 0) continue;
        base = (size_t)u->offset[s] + element * u->array_stride;
        fl = u->type == GL_FLOAT_ || (u->type >= 0x8b50 && u->type <= 0x8b52) || columns > 1;
        for (int col = 0; col < columns; ++col)
            for (int row = 0; row < rows; ++row) {
                uint32_t bits;
                float f;
                memcpy(&bits, block + base + (size_t)col * (columns > 1 ? u->matrix_stride : 0) + 4 * (size_t)row, 4);
                memcpy(&f, &bits, 4);
                if (kind == 'f' && !fl) f = u->type == GL_UNSIGNED_INT_ ? (float)bits : (float)(int32_t)bits;
                if (kind != 'f' && fl) bits = (uint32_t)(int32_t)f;
                memcpy(o + 4 * (col * rows + row), kind == 'f' ? (void*)&f : (void*)&bits, 4);
            }
        return;
    }
}

void snx_gl_uniform(uint32_t ctx, int32_t location, int32_t count, const void* data, int comps, char kind,
                    int transpose) {
    uniform_write(ctx, uniform_program(ctx), location, count, data, comps, kind, transpose);
}
void snx_gl_program_uniform(uint32_t ctx, uint32_t program, int32_t location, int32_t count, const void* data,
                            int comps, char kind, int transpose) {
    uniform_write(ctx, program, location, count, data, comps, kind, transpose);
}

/* ---- program pipelines (GLES 3.1) ---- */

/* the binding of program q of the same kind and name as b of p, or NULL */
static const SnxGlBinding* same_binding(const SnxGlProgram* p, const SnxGlBinding* b, const SnxGlProgram* q) {
    const SnxGlBinding* qb = snx_gl_program_bindings(q);
    for (uint32_t i = 0; i < q->binding_count; ++i)
        if (qb[i].kind == b->kind && b->name && qb[i].name &&
            !strcmp(snx_gl_program_names(q) + qb[i].name, snx_gl_program_names(p) + b->name))
            return &qb[i];
    return NULL;
}
uint32_t snx_gl_draw_program(uint32_t ctx, int compute) {
    uint32_t current = snx_gl_current_program(ctx), hidden;
    SnxGlPipeline* pp;
    const SnxGlProgram *v, *f;
    SnxGlProgram* h;
    if (snx_gl_program(current) || !snx_gl_current_pipeline(ctx)) return snx_gl_program(current) ? current : 0;
    pp = snx_gl_pipeline(snx_gl_current_pipeline(ctx));
    if (!pp) return 0;
    if (compute) return snx_gl_program(pp->stages[2]) ? pp->stages[2] : 0;
    v = snx_gl_program(pp->stages[0]);
    f = snx_gl_program(pp->stages[1]);
    if (!v || !f || !v->linked || !f->linked) return 0;
    hidden = pp->linked;
    h = snx_gl_program(hidden);
    if (!h || pp->names[0] != pp->stages[0] || pp->names[1] != pp->stages[1] || pp->generations[0] != v->generation ||
        pp->generations[1] != f->generation) {
        /* the two stages' shaders linked together, as one program would have them */
        SnxGlShader* vs = snx_gl_shader(v->shaders[0]);
        SnxGlShader* fs = snx_gl_shader(f->shaders[1]);
        uint32_t vsn = v->shaders[0], fsn = f->shaders[1];
        if (!vs || !fs) {
            say("program pipeline %u's programs %u and %u have no shaders left: it is not drawn",
                (unsigned)snx_gl_current_pipeline(ctx), (unsigned)pp->stages[0], (unsigned)pp->stages[1]);
            return 0;
        }
        if (h) program_free(hidden);
        hidden = new_name(ctx);
        if (!hidden) return 0;
        S.programs[hidden - 1] = (SnxGlProgram){.used = 1, .hidden = 1, .shaders = {vsn, fsn, 0}};
        vs->attached++;
        fs->attached++;
        link(ctx, hidden);
        pp->linked = hidden;
        pp->names[0] = pp->stages[0], pp->names[1] = pp->stages[1];
        pp->generations[0] = snx_gl_program(pp->stages[0])->generation;
        pp->generations[1] = snx_gl_program(pp->stages[1])->generation;
        if (!S.programs[hidden - 1].linked) return 0;
    }
    /* the stages' uniforms: each stage's default block, the samplers' units, the blocks' binding points */
    h = &S.programs[hidden - 1];
    v = snx_gl_program(pp->stages[0]);
    f = snx_gl_program(pp->stages[1]);
    for (int stage = 0; stage < 2; ++stage) {
        const SnxGlProgram* from = stage ? f : v;
        uint8_t* to = snx_gl_program_default(h, stage);
        const uint8_t* src = snx_gl_program_default(from, stage);
        if (h->default_size[stage] == from->default_size[stage] && h->default_size[stage] &&
            memcmp(to, src, h->default_size[stage])) {
            memcpy(to, src, h->default_size[stage]);
            h->default_serial[stage]++;
        }
    }
    for (uint32_t i = 0; i < h->binding_count; ++i) {
        const SnxGlBinding* b = &snx_gl_program_bindings(h)[i];
        const SnxGlProgram* owner = v;
        const SnxGlBinding* x = same_binding(h, b, v);
        if (!x) x = same_binding(h, b, f), owner = f;
        if (!x || x->count != b->count) continue;
        if (b->kind == SNX_GLSLC_REFLECT_SAMPLER)
            memcpy(&snx_gl_program_units(h)[b->first], &snx_gl_program_units(owner)[x->first],
                   b->count * sizeof(int32_t));
        else if (b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && b->first < SNX_GL_UBO_BINDINGS &&
                 x->first < SNX_GL_UBO_BINDINGS)
            h->block_binding[b->first] = owner->block_binding[x->first];
    }
    return hidden;
}

void snx_gl_gen_pipelines(uint32_t ctx, int32_t n, uint32_t* out) {
    uint32_t name = 1;
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; ++i) {
        uint32_t v = 0;
        while (name <= SNX_GL_PIPELINES && S.pipelines[name - 1].used) ++name;
        if (name <= SNX_GL_PIPELINES) {
            S.pipelines[name - 1] = (SnxGlPipeline){.used = 1};
            v = name++;
        } else {
            no_room(ctx, "a program pipeline (every one of the 64 is used)");
        }
        memcpy(out + i, &v, 4);
    }
}
void snx_gl_delete_pipelines(uint32_t ctx, int32_t n, const uint32_t* names) {
    for (int32_t i = 0; i < n; ++i) {
        uint32_t name;
        SnxGlPipeline* pp;
        const SnxGlProgram* h;
        memcpy(&name, names + i, 4);
        pp = snx_gl_pipeline(name);
        if (!pp) continue;
        h = snx_gl_program(pp->linked);
        if (h && h->hidden) program_free(pp->linked);
        S.pipelines[name - 1] = (SnxGlPipeline){0};
        if (ctx && snx_gl_current_pipeline(ctx) == name) snx_gl_context(ctx)->pipeline = 0;
    }
}
int snx_gl_is_pipeline(uint32_t name) { return snx_gl_pipeline(name) != NULL; }
void snx_gl_bind_pipeline(uint32_t ctx, uint32_t pipeline) {
    if (pipeline && !snx_gl_pipeline(pipeline))
        error(ctx, SNX_GL_INVALID_OPERATION);
    else
        snx_gl_context(ctx)->pipeline = pipeline;
}
/* stages: GL_VERTEX_SHADER_BIT 1, GL_FRAGMENT_SHADER_BIT 2, GL_COMPUTE_SHADER_BIT 0x20 */
void snx_gl_use_program_stages(uint32_t ctx, uint32_t pipeline, uint32_t stages, uint32_t program) {
    SnxGlPipeline* pp = snx_gl_pipeline(pipeline);
    const SnxGlProgram* p = snx_gl_program(program);
    if (!pp || (program && (!p || !p->linked || !p->separable))) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (stages & 1) pp->stages[0] = program;
    if (stages & 2) pp->stages[1] = program;
    if (stages & 0x20) pp->stages[2] = program;
}
void snx_gl_active_shader_program(uint32_t ctx, uint32_t pipeline, uint32_t program) {
    SnxGlPipeline* pp = snx_gl_pipeline(pipeline);
    if (pp)
        pp->active = program;
    else
        error(ctx, SNX_GL_INVALID_OPERATION);
}
void snx_gl_validate_pipeline(uint32_t ctx, uint32_t pipeline) {
    if (!snx_gl_pipeline(pipeline)) error(ctx, SNX_GL_INVALID_OPERATION);
}
int snx_gl_get_pipelineiv(uint32_t ctx, uint32_t pipeline, uint32_t pname, int32_t* out) {
    const SnxGlPipeline* pp = snx_gl_pipeline(pipeline);
    if (!pp) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return 0;
    }
    switch (pname) {
        case 0x8259: *out = (int32_t)pp->active; return 1; /* GL_ACTIVE_PROGRAM */
        case GL_VERTEX_SHADER_: *out = (int32_t)pp->stages[0]; return 1;
        case GL_FRAGMENT_SHADER_: *out = (int32_t)pp->stages[1]; return 1;
        case GL_COMPUTE_SHADER_: *out = (int32_t)pp->stages[2]; return 1;
        case 0x8b83: *out = pp->stages[2] || (pp->stages[0] && pp->stages[1]); return 1; /* GL_VALIDATE_STATUS */
        case 0x8b84: *out = 0; return 1;                                                 /* GL_INFO_LOG_LENGTH */
        default: error(ctx, SNX_GL_INVALID_ENUM); return 0;
    }
}
uint32_t snx_gl_create_shader_program(uint32_t ctx, uint32_t shader) {
    SnxGlShader* s = snx_gl_shader(shader);
    uint32_t program;
    if (!s) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return 0;
    }
    snx_gl_compile_shader(ctx, shader);
    program = snx_gl_create_program(ctx);
    if (!program) return 0;
    s = snx_gl_shader(shader);
    S.programs[program - 1].separable = 1;
    S.programs[program - 1].shaders[stage_index(s->type)] = shader;
    s->attached++;
    s->deleted = 1; /* it goes with the program */
    link(ctx, program);
    return program;
}

/* ---- the queries of programs ---- */

static const SnxGlProgram* program_arg(uint32_t ctx, uint32_t name) {
    const SnxGlProgram* p = snx_gl_program(name);
    if (!p) error(ctx, SNX_GL_INVALID_VALUE);
    return p;
}
const char* snx_gl_program_info_log(uint32_t ctx, uint32_t program) {
    const SnxGlProgram* p = program_arg(ctx, program);
    return p ? text_of(p->log) : NULL;
}
static uint32_t longest(const SnxGlProgram* p, int uniforms) {
    uint32_t n = 0, count = !p->linked ? 0 : uniforms ? p->uniform_count : p->input_count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t name = uniforms ? snx_gl_program_uniforms(p)[i].name : snx_gl_program_inputs(p)[i].name;
        uint32_t len = (uint32_t)strlen(snx_gl_program_names(p) + name) + 1;
        if (len > n) n = len;
    }
    return n;
}
int snx_gl_get_programiv(uint32_t ctx, uint32_t program, uint32_t pname, int32_t out[3]) {
    const SnxGlProgram* p = program_arg(ctx, program);
    int32_t v = 0;
    if (!p) return 0;
    switch (pname) {
        case 0x8b80: v = p->deleted; break;                                                 /* GL_DELETE_STATUS */
        case 0x8b82: v = p->linked; break;                                                  /* GL_LINK_STATUS */
        case 0x8258: v = p->separable; break;                                               /* GL_PROGRAM_SEPARABLE */
        case 0x8b83: v = p->linked; break;                                                  /* GL_VALIDATE_STATUS */
        case 0x8b84: v = p->log.size > 1 ? (int32_t)strlen(text_of(p->log)) + 1 : 0; break; /* GL_INFO_LOG_LENGTH */
        case 0x8b85: v = (p->shaders[0] != 0) + (p->shaders[1] != 0) + (p->shaders[2] != 0); break; /* ATTACHED */
        case 0x8b86: v = (int32_t)p->uniform_count; break; /* GL_ACTIVE_UNIFORMS */
        case 0x8b87: v = (int32_t)longest(p, 1); break;    /* GL_ACTIVE_UNIFORM_MAX_LENGTH */
        case 0x8b89: v = (int32_t)p->input_count; break;   /* GL_ACTIVE_ATTRIBUTES */
        case 0x8b8a: v = (int32_t)longest(p, 0); break;    /* GL_ACTIVE_ATTRIBUTE_MAX_LENGTH */
        case 0x8a36: v = (int32_t)p->block_count; break;   /* GL_ACTIVE_UNIFORM_BLOCKS */
        case 0x8a35:                                       /* GL_ACTIVE_UNIFORM_BLOCK_MAX_NAME_LENGTH */
            for (uint32_t i = 0; p->linked && i < p->binding_count; ++i) {
                const SnxGlBinding* b = &snx_gl_program_bindings(p)[i];
                int32_t len = (int32_t)strlen(snx_gl_program_names(p) + b->name) + 1;
                if (b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && b->name && len > v) v = len;
            }
            break;
        case 0x8c7f: v = 0x8c8c; break; /* GL_TRANSFORM_FEEDBACK_BUFFER_MODE: GL_INTERLEAVED_ATTRIBS */
        case 0x8c83:                    /* GL_TRANSFORM_FEEDBACK_VARYINGS */
        case 0x8c76:                    /* GL_TRANSFORM_FEEDBACK_VARYING_MAX_LENGTH */
        case 0x8741:                    /* GL_PROGRAM_BINARY_LENGTH */
        case 0x8257: v = 0; break;      /* GL_PROGRAM_BINARY_RETRIEVABLE_HINT */
        case 0x8267:                    /* GL_COMPUTE_WORK_GROUP_SIZE */
            for (int i = 0; i < 3; ++i) out[i] = (int32_t)p->local_size[i];
            return 3;
        default: error(ctx, SNX_GL_INVALID_ENUM); return 0;
    }
    out[0] = v;
    return 1;
}
const char* snx_gl_active(uint32_t ctx, uint32_t program, uint32_t index, int uniforms, int32_t* count,
                          uint32_t* type) {
    const SnxGlProgram* p = program_arg(ctx, program);
    uint32_t name;
    if (!p) return NULL;
    if (index >= (uniforms ? p->uniform_count : p->input_count)) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return NULL;
    }
    if (uniforms) {
        const SnxGlUniform* u = &snx_gl_program_uniforms(p)[index];
        name = u->name, *count = u->count, *type = u->type;
    } else {
        const SnxGlInput* in = &snx_gl_program_inputs(p)[index];
        name = in->name, *count = in->count, *type = in->type;
    }
    return snx_gl_program_names(p) + name;
}
int32_t snx_gl_attrib_location(uint32_t ctx, uint32_t program, const char* name) {
    const SnxGlProgram* p = program_arg(ctx, program);
    int32_t loc = -1;
    if (!p) return -1;
    if (!p->linked) return error(ctx, SNX_GL_INVALID_OPERATION), -1;
    for (uint32_t i = 0; name && i < p->input_count; ++i)
        if (!strcmp(snx_gl_program_names(p) + snx_gl_program_inputs(p)[i].name, name))
            loc = snx_gl_program_inputs(p)[i].location;
    return loc;
}
static const SnxGlInput* program_outputs(const SnxGlProgram* p) {
    return (const SnxGlInput*)(snx_gl_bytes(p->link) + p->outputs);
}
int32_t snx_gl_frag_data_location(uint32_t ctx, uint32_t program, const char* name) {
    const SnxGlProgram* p = program_arg(ctx, program);
    int32_t loc = -1;
    for (uint32_t i = 0; p && p->linked && name && i < p->output_count; ++i)
        if (!strcmp(snx_gl_program_names(p) + program_outputs(p)[i].name, name)) loc = program_outputs(p)[i].spirv;
    return loc;
}
/* a uniform's location by GL's name: "a", "a[0]" and "a[i]" of an array, "s.m", "s[i].m"; -1 for none */
static int32_t location_of(const SnxGlProgram* p, const char* name) {
    size_t len = strlen(name), base = len;
    uint32_t element = 0;
    if (len > 3 && name[len - 1] == ']') {
        const char* open = strrchr(name, '[');
        if (open && open[1] >= '0' && open[1] <= '9') {
            element = (uint32_t)strtoul(open + 1, NULL, 10);
            base = (size_t)(open - name);
        }
    }
    for (uint32_t i = 0; i < p->uniform_count; ++i) {
        const SnxGlUniform* u = &snx_gl_program_uniforms(p)[i];
        const char* n = snx_gl_program_names(p) + u->name;
        size_t nl;
        if (u->location < 0) continue;
        if (!strcmp(n, name)) return u->location;
        /* an array's "a[0]": its "a" and "a[i]" */
        nl = strlen(n);
        if (nl == base + 3 && !memcmp(n, name, base) && !strcmp(n + base, "[0]") && element < u->count)
            return u->location + (int32_t)element;
    }
    return -1;
}
int32_t snx_gl_uniform_location(uint32_t ctx, uint32_t program, const char* name) {
    const SnxGlProgram* p = program_arg(ctx, program);
    if (!p || !name) return -1;
    if (!p->linked) return error(ctx, SNX_GL_INVALID_OPERATION), -1;
    return location_of(p, name);
}
/* the program's uniform block of an index, in order */
static const SnxGlBinding* uniform_block(const SnxGlProgram* p, uint32_t index) {
    for (uint32_t i = 0; p->linked && i < p->binding_count; ++i) {
        const SnxGlBinding* b = &snx_gl_program_bindings(p)[i];
        if (b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && b->name && b->first == index) return b;
    }
    return NULL;
}
uint32_t snx_gl_uniform_block_index(uint32_t ctx, uint32_t program, const char* name) {
    const SnxGlProgram* p = program_arg(ctx, program);
    uint32_t found = 0xffffffffu; /* GL_INVALID_INDEX */
    for (uint32_t i = 0; p && p->linked && name && i < p->binding_count; ++i) {
        const SnxGlBinding* b = &snx_gl_program_bindings(p)[i];
        if (b->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && b->name && !strcmp(snx_gl_program_names(p) + b->name, name))
            found = b->first;
    }
    return found;
}
void snx_gl_uniform_block_binding(uint32_t ctx, uint32_t program, uint32_t index, uint32_t binding) {
    SnxGlProgram* p = snx_gl_program(program);
    if (!p || index >= p->block_count || index >= SNX_GL_UBO_BINDINGS || binding >= SNX_GL_UBO_BINDINGS)
        error(ctx, SNX_GL_INVALID_VALUE);
    else
        p->block_binding[index] = binding;
}
int snx_gl_active_uniform_blockiv(uint32_t ctx, uint32_t program, uint32_t index, uint32_t pname, int32_t* out,
                                  int32_t max) {
    const SnxGlProgram* p = program_arg(ctx, program);
    const SnxGlBinding* b;
    int32_t v = 0, n = 0;
    if (!p) return 0;
    b = uniform_block(p, index);
    if (!b) return error(ctx, SNX_GL_INVALID_VALUE), 0;
    switch (pname) {
        case 0x8a3f: v = (int32_t)p->block_binding[index]; break;                       /* GL_UNIFORM_BLOCK_BINDING */
        case 0x8a40: v = (int32_t)b->size; break;                                       /* DATA_SIZE */
        case 0x8a41: v = (int32_t)strlen(snx_gl_program_names(p) + b->name) + 1; break; /* NAME_LENGTH */
        case 0x8a42:                                                                    /* ACTIVE_UNIFORMS */
            for (uint32_t i = 0; i < p->uniform_count; ++i) v += snx_gl_program_uniforms(p)[i].block == (int16_t)index;
            break;
        case 0x8a43: /* ACTIVE_UNIFORM_INDICES */
            for (uint32_t i = 0; i < p->uniform_count; ++i)
                if (snx_gl_program_uniforms(p)[i].block == (int16_t)index && n < max) memcpy(out + n++, &i, 4);
            return n;
        case 0x8a44:               /* REFERENCED_BY_VERTEX_SHADER */
        case 0x8a46: v = 1; break; /* REFERENCED_BY_FRAGMENT_SHADER */
        default: error(ctx, SNX_GL_INVALID_ENUM); return 0;
    }
    memcpy(out, &v, 4);
    return 1;
}
const char* snx_gl_active_uniform_block_name(uint32_t ctx, uint32_t program, uint32_t index) {
    const SnxGlProgram* p = program_arg(ctx, program);
    const SnxGlBinding* b;
    if (!p) return NULL;
    b = uniform_block(p, index);
    if (!b) return error(ctx, SNX_GL_INVALID_VALUE), NULL;
    return snx_gl_program_names(p) + b->name;
}
uint32_t snx_gl_uniform_index(uint32_t ctx, uint32_t program, const char* name) {
    const SnxGlProgram* p = program_arg(ctx, program);
    uint32_t found = 0xffffffffu;
    const char* dot = name ? strchr(name, '.') : NULL;
    for (uint32_t i = 0; p && p->linked && name && i < p->uniform_count; ++i) {
        const SnxGlUniform* u = &snx_gl_program_uniforms(p)[i];
        const char* n = snx_gl_program_names(p) + u->name;
        size_t nl = strlen(n);
        if (!strcmp(n, name) || (u->block >= 0 && dot && !strcmp(n, dot + 1))) found = i;
        if (found == 0xffffffffu && nl > 3 && !strcmp(n + nl - 3, "[0]") && strlen(name) == nl - 3 &&
            !memcmp(n, name, nl - 3))
            found = i;
    }
    return found;
}
int snx_gl_active_uniformiv(uint32_t ctx, uint32_t program, uint32_t index, uint32_t pname, int32_t* out) {
    const SnxGlProgram* p = program_arg(ctx, program);
    const SnxGlUniform* u;
    int columns;
    if (!p) return 0;
    if (index >= p->uniform_count) return error(ctx, SNX_GL_INVALID_VALUE), 0;
    u = &snx_gl_program_uniforms(p)[index];
    switch (pname) {
        case 0x8a37: *out = (int32_t)u->type; return 1;                                       /* GL_UNIFORM_TYPE */
        case 0x8a38: *out = u->count; return 1;                                               /* GL_UNIFORM_SIZE */
        case 0x8a39: *out = (int32_t)strlen(snx_gl_program_names(p) + u->name) + 1; return 1; /* NAME_LENGTH */
        case 0x8a3a: *out = u->block; return 1;                                               /* BLOCK_INDEX */
        case 0x8a3b: *out = u->block >= 0 ? (u->offset[0] >= 0 ? u->offset[0] : u->offset[1]) : -1; return 1;
        case 0x8a3c: *out = u->block >= 0 ? (u->count > 1 || u->array_stride ? u->array_stride : 0) : -1; return 1;
        case 0x8a3d:
            components(u->type, &columns);
            *out = u->block >= 0 && columns > 1 ? u->matrix_stride : u->block >= 0 ? 0 : -1;
            return 1;
        case 0x8a3e: *out = 0; return 1; /* GL_UNIFORM_IS_ROW_MAJOR */
        default: error(ctx, SNX_GL_INVALID_ENUM); return 0;
    }
}

/* ---- GLES 3.1's program interface queries ---- */

enum { IF_UNIFORM = 0x92e1, IF_UNIFORM_BLOCK, IF_INPUT, IF_OUTPUT, IF_BUFFER_VARIABLE, IF_STORAGE_BLOCK };

/* the binding of the index'th block of a kind */
static const SnxGlBinding* block_of(const SnxGlProgram* p, uint8_t kind, uint32_t index) {
    uint32_t n = 0;
    for (uint32_t i = 0; p->linked && i < p->binding_count; ++i) {
        const SnxGlBinding* b = &snx_gl_program_bindings(p)[i];
        if (b->kind == kind && b->name && n++ == index) return b;
    }
    return NULL;
}
static uint32_t resources(const SnxGlProgram* p, uint32_t interface) {
    uint32_t n = 0;
    uint8_t kind;
    switch (interface) {
        case IF_UNIFORM: return p->uniform_count;
        case IF_INPUT: return p->input_count;
        case IF_OUTPUT: return p->output_count;
        case IF_UNIFORM_BLOCK:
        case IF_STORAGE_BLOCK:
            kind = interface == IF_UNIFORM_BLOCK ? SNX_GLSLC_REFLECT_UNIFORM_BLOCK : SNX_GLSLC_REFLECT_BUFFER_BLOCK;
            while (block_of(p, kind, n)) ++n;
            return n;
        default: return 0; /* buffer variables: not listed yet */
    }
}
static const char* resource_name(const SnxGlProgram* p, uint32_t interface, uint32_t i) {
    const char* names = snx_gl_program_names(p);
    switch (interface) {
        case IF_UNIFORM: return names + snx_gl_program_uniforms(p)[i].name;
        case IF_INPUT: return names + snx_gl_program_inputs(p)[i].name;
        case IF_OUTPUT: return names + program_outputs(p)[i].name;
        case IF_UNIFORM_BLOCK: return names + block_of(p, SNX_GLSLC_REFLECT_UNIFORM_BLOCK, i)->name;
        case IF_STORAGE_BLOCK: return names + block_of(p, SNX_GLSLC_REFLECT_BUFFER_BLOCK, i)->name;
        default: return "";
    }
}
static int valid_interface(uint32_t interface) { return interface >= IF_UNIFORM && interface <= IF_STORAGE_BLOCK; }
int snx_gl_program_interfaceiv(uint32_t ctx, uint32_t program, uint32_t interface, uint32_t pname, int32_t* out) {
    const SnxGlProgram* p = program_arg(ctx, program);
    int32_t v = 0;
    uint32_t n;
    if (!p) return 0;
    if (!valid_interface(interface)) return error(ctx, SNX_GL_INVALID_ENUM), 0;
    n = p->linked ? resources(p, interface) : 0;
    if (pname == 0x92f5) /* GL_ACTIVE_RESOURCES */
        v = (int32_t)n;
    else if (pname == 0x92f6) /* GL_MAX_NAME_LENGTH */
        for (uint32_t i = 0; i < n; ++i) {
            int32_t len = (int32_t)strlen(resource_name(p, interface, i)) + 1;
            if (len > v) v = len;
        }
    else if (pname == 0x92f7) /* GL_MAX_NUM_ACTIVE_VARIABLES */
        for (uint32_t i = 0; interface == IF_UNIFORM_BLOCK && i < n; ++i) {
            int32_t k = 0;
            for (uint32_t u = 0; u < p->uniform_count; ++u) k += snx_gl_program_uniforms(p)[u].block == (int16_t)i;
            if (k > v) v = k;
        }
    else
        return error(ctx, SNX_GL_INVALID_ENUM), 0;
    *out = v;
    return 1;
}
uint32_t snx_gl_program_resource_index(uint32_t ctx, uint32_t program, uint32_t interface, const char* name) {
    const SnxGlProgram* p = program_arg(ctx, program);
    size_t len;
    uint32_t n;
    if (!p || !name || !valid_interface(interface) || !p->linked) return 0xffffffffu;
    len = strlen(name);
    n = resources(p, interface);
    for (uint32_t i = 0; i < n; ++i) {
        const char* r = resource_name(p, interface, i);
        size_t rl = strlen(r);
        /* an array's "a[0]" by its "a" too */
        if (!strcmp(r, name) || (rl == len + 3 && !memcmp(r, name, len) && !strcmp(r + len, "[0]"))) return i;
    }
    return 0xffffffffu;
}
const char* snx_gl_program_resource_name(uint32_t ctx, uint32_t program, uint32_t interface, uint32_t index) {
    const SnxGlProgram* p = program_arg(ctx, program);
    if (!p) return NULL;
    if (!valid_interface(interface) || !p->linked || index >= resources(p, interface)) {
        error(ctx, valid_interface(interface) ? SNX_GL_INVALID_VALUE : SNX_GL_INVALID_ENUM);
        return NULL;
    }
    return resource_name(p, interface, index);
}
/* one property of a resource: its values into out (up to max), their count; -1 for one it has not */
static int32_t resource_property(const SnxGlProgram* p, uint32_t interface, uint32_t i, uint32_t prop, int32_t* out,
                                 int32_t max) {
    const char* name = resource_name(p, interface, i);
    const SnxGlBinding* b;
    if (prop == 0x92f9) return out[0] = (int32_t)strlen(name) + 1, 1; /* GL_NAME_LENGTH */
    if (prop >= 0x9306 && prop <= 0x930b)                             /* GL_REFERENCED_BY_*: the stages it has */
        return out[0] = prop == 0x9306   ? p->shaders[0] != 0
                        : prop == 0x930a ? p->shaders[1] != 0
                        : prop == 0x930b ? p->shaders[2] != 0
                                         : 0,
               1;
    if (interface == IF_UNIFORM) {
        const SnxGlUniform* u = &snx_gl_program_uniforms(p)[i];
        int columns = u->type >= 0x8b5a && u->type <= 0x8b6a ? 2 : 1;
        switch (prop) {
            case 0x92fa: return out[0] = (int32_t)u->type, 1;
            case 0x92fb: return out[0] = u->count, 1;
            case 0x92fc: return out[0] = u->block >= 0 ? (u->offset[0] >= 0 ? u->offset[0] : u->offset[1]) : -1, 1;
            case 0x92fd: return out[0] = u->block, 1;
            case 0x92fe: return out[0] = u->block >= 0 ? u->array_stride : -1, 1;
            case 0x92ff: return out[0] = u->block >= 0 ? (columns > 1 ? u->matrix_stride : 0) : -1, 1;
            case 0x9300: return out[0] = 0, 1;
            case 0x930e: return out[0] = u->location, 1;
            case 0x9301: return out[0] = -1, 1; /* GL_ATOMIC_COUNTER_BUFFER_INDEX */
            default: return -1;
        }
    }
    if (interface == IF_INPUT || interface == IF_OUTPUT) {
        const SnxGlInput* x = interface == IF_INPUT ? &snx_gl_program_inputs(p)[i] : &program_outputs(p)[i];
        switch (prop) {
            case 0x92fa: return out[0] = (int32_t)x->type, 1;
            case 0x92fb: return out[0] = x->count, 1;
            case 0x930e: return out[0] = interface == IF_INPUT ? x->location : x->spirv, 1;
            default: return -1;
        }
    }
    b = block_of(p, interface == IF_UNIFORM_BLOCK ? SNX_GLSLC_REFLECT_UNIFORM_BLOCK : SNX_GLSLC_REFLECT_BUFFER_BLOCK,
                 i);
    switch (prop) {
        case 0x9302: /* GL_BUFFER_BINDING */
            return out[0] = (int32_t)(interface == IF_UNIFORM_BLOCK && b->first < SNX_GL_UBO_BINDINGS
                                          ? p->block_binding[b->first]
                                          : b->first),
                   1;
        case 0x9303: return out[0] = (int32_t)b->size, 1;
        case 0x9304:
        case 0x9305: {
            int32_t n = 0;
            for (uint32_t u = 0; interface == IF_UNIFORM_BLOCK && u < p->uniform_count; ++u)
                if (snx_gl_program_uniforms(p)[u].block == (int16_t)b->first) {
                    if (prop == 0x9305 && n < max) out[n] = (int32_t)u;
                    ++n;
                }
            if (prop == 0x9304) return out[0] = n, 1;
            return n < max ? n : max;
        }
        default: return -1;
    }
}
int32_t snx_gl_program_resourceiv(uint32_t ctx, uint32_t program, uint32_t interface, uint32_t index, int32_t count,
                                  const uint32_t* props, int32_t size, int32_t* params) {
    const SnxGlProgram* p = program_arg(ctx, program);
    int32_t written = 0;
    if (!p) return 0;
    if (!valid_interface(interface) || !p->linked || index >= resources(p, interface)) {
        error(ctx, valid_interface(interface) ? SNX_GL_INVALID_VALUE : SNX_GL_INVALID_ENUM);
        return 0;
    }
    for (int32_t k = 0; k < count && written < size; ++k) {
        uint32_t prop;
        int32_t values[64], n;
        memcpy(&prop, props + k, 4);
        n = resource_property(p, interface, index, prop, values, 64);
        if (n < 0) {
            error(ctx, SNX_GL_INVALID_OPERATION);
            return written;
        }
        for (int32_t v = 0; v < n && written < size; ++v) memcpy(params + written++, &values[v], 4);
    }
    return written;
}
int32_t snx_gl_program_resource_location(uint32_t ctx, uint32_t program, uint32_t interface, const char* name) {
    const SnxGlProgram* p = program_arg(ctx, program);
    int32_t loc = -1;
    if (!p || !name || !p->linked) return -1;
    if (interface == IF_UNIFORM) loc = location_of(p, name);
    for (uint32_t i = 0; interface == IF_INPUT && i < p->input_count; ++i)
        if (!strcmp(snx_gl_program_names(p) + snx_gl_program_inputs(p)[i].name, name))
            loc = snx_gl_program_inputs(p)[i].location;
    for (uint32_t i = 0; interface == IF_OUTPUT && i < p->output_count; ++i)
        if (!strcmp(snx_gl_program_names(p) + program_outputs(p)[i].name, name)) loc = program_outputs(p)[i].spirv;
    return loc;
}
void snx_gl_program_binary(uint32_t ctx, uint32_t program) {
    if (!program_arg(ctx, program)) return;
    program_unlink(program);
    link_log(ctx, program, "no program binary formats");
}
void snx_gl_program_parameter(uint32_t ctx, uint32_t program, uint32_t pname, int32_t value) {
    SnxGlProgram* p = snx_gl_program(program);
    if (!p)
        error(ctx, SNX_GL_INVALID_VALUE);
    else if (pname == 0x8258) /* GL_PROGRAM_SEPARABLE; the binary hint has no effect */
        p->separable = value != 0;
}

/* ---- the save state ---- */

typedef struct {
    uint32_t records, arena; /* the bytes of the records and of the arena that follow */
} SnxGlProgramStateHeader;

size_t snx_gl_program_state_bytes(void) {
    return sizeof(SnxGlProgramStateHeader) + sizeof(SnxGlPrograms) + (S.arena_used ? S.arena_used : 0);
}
void snx_gl_program_state_save(void* out) {
    const SnxGlProgramStateHeader h = {(uint32_t)sizeof(SnxGlPrograms), S.arena_used};
    unsigned char* o = out;
    memcpy(o, &h, sizeof h);
    memcpy(o + sizeof h, &S, sizeof S);
    if (S.arena_used) memcpy(o + sizeof h + sizeof S, g_gl_program.arena, S.arena_used);
}

/* a range of a record against the arena of used bytes: 0, or -1 with why */
static int range_ok(SnxGlBytes b, uint32_t used, const char* what, uint32_t name, char* why, size_t whysize) {
    if (!b.size && !b.at) return 0;
    if (!b.size || b.at < ARENA_RESERVED || b.at > used || b.size > used - b.at) {
        snprintf(why, whysize, "the GL layer's %s %u has the range %u+%u, outside its %u-byte arena", what,
                 (unsigned)name, (unsigned)b.at, (unsigned)b.size, (unsigned)used);
        return -1;
    }
    return 0;
}
/* a linked program's arrays, inside its link range */
static int link_ok(const SnxGlProgram* p, uint32_t name, char* why, size_t whysize) {
    const struct {
        uint32_t at;
        uint64_t size;
        const char* what;
    } parts[] = {
        {p->bindings, (uint64_t)p->binding_count * sizeof(SnxGlBinding), "bindings"},
        {p->uniforms, (uint64_t)p->uniform_count * sizeof(SnxGlUniform), "uniforms"},
        {p->locations, (uint64_t)p->location_count * 4, "locations"},
        {p->units, (uint64_t)p->unit_count * 4, "units"},
        {p->inputs, (uint64_t)p->input_count * sizeof(SnxGlInput), "inputs"},
        {p->outputs, (uint64_t)p->output_count * sizeof(SnxGlInput), "outputs"},
        {p->names, (uint64_t)p->names_size + 1, "names"},
        {p->defaults[0], p->default_size[0], "vertex default block"},
        {p->defaults[1], p->default_size[1], "fragment default block"},
        {p->defaults[2], p->default_size[2], "compute default block"},
        {p->spirv[0], (uint64_t)p->spirv_words[0] * 4, "vertex module"},
        {p->spirv[1], (uint64_t)p->spirv_words[1] * 4, "fragment module"},
        {p->spirv[2], (uint64_t)p->spirv_words[2] * 4, "compute module"},
    };
    for (size_t i = 0; i < sizeof parts / sizeof *parts; i++)
        if ((uint64_t)parts[i].at + parts[i].size > p->link.size) {
            snprintf(why, whysize, "the GL layer's program %u has its %s at %u+%llu, past its %u-byte link",
                     (unsigned)name, parts[i].what, (unsigned)parts[i].at, (unsigned long long)parts[i].size,
                     (unsigned)p->link.size);
            return -1;
        }
    return 0;
}
/* a linked program's arrays against each other, base its link's bytes in the copy: every name inside names, every
   location an element of a uniform, every sampler's units inside units */
static int arrays_ok(const SnxGlProgram* p, const uint8_t* base, uint32_t name, char* why, size_t whysize) {
    const char* names = (const char*)base + p->names;
    if (names[p->names_size]) {
        snprintf(why, whysize, "the GL layer's program %u has names without their last NUL", (unsigned)name);
        return -1;
    }
    for (uint32_t k = 0; k < p->uniform_count; k++) {
        SnxGlUniform u;
        memcpy(&u, base + p->uniforms + k * sizeof u, sizeof u);
        if (u.name > p->names_size || (u.sampler >= 0 && (uint32_t)u.sampler + u.count > p->unit_count)) {
            snprintf(why, whysize,
                     "the GL layer's program %u has uniform %u named at %u of %u bytes, units %d+%u of %u",
                     (unsigned)name, (unsigned)k, (unsigned)u.name, (unsigned)p->names_size, (int)u.sampler,
                     (unsigned)u.count, (unsigned)p->unit_count);
            return -1;
        }
    }
    for (uint32_t k = 0; k < p->location_count; k++) {
        int32_t e;
        SnxGlUniform u;
        memcpy(&e, base + p->locations + 4 * k, 4);
        if ((uint32_t)e >> 16 >= p->uniform_count) {
            snprintf(why, whysize, "the GL layer's program %u has location %u at uniform %u of %u", (unsigned)name,
                     (unsigned)k, (unsigned)((uint32_t)e >> 16), (unsigned)p->uniform_count);
            return -1;
        }
        memcpy(&u, base + p->uniforms + ((uint32_t)e >> 16) * sizeof u, sizeof u);
        if (((uint32_t)e & 0xffff) >= u.count) {
            snprintf(why, whysize, "the GL layer's program %u has location %u at element %u of a uniform of %u",
                     (unsigned)name, (unsigned)k, (unsigned)((uint32_t)e & 0xffff), (unsigned)u.count);
            return -1;
        }
    }
    for (uint32_t k = 0; k < p->binding_count + p->input_count + p->output_count; k++) {
        uint32_t at = k < p->binding_count ? p->bindings + k * (uint32_t)sizeof(SnxGlBinding)
                      : k < p->binding_count + p->input_count
                          ? p->inputs + (k - p->binding_count) * (uint32_t)sizeof(SnxGlInput)
                          : p->outputs + (k - p->binding_count - p->input_count) * (uint32_t)sizeof(SnxGlInput);
        uint32_t text;
        memcpy(&text, base + at, 4); /* the name is each record's first field */
        if (text > p->names_size) {
            snprintf(why, whysize, "the GL layer's program %u names a binding, input or output at %u of %u bytes",
                     (unsigned)name, (unsigned)text, (unsigned)p->names_size);
            return -1;
        }
    }
    return 0;
}
int snx_gl_program_state_check(const void* in, size_t size, char* why, size_t whysize) {
    SnxGlPrograms* const c = &g_gl_program.checked;
    SnxGlProgramStateHeader h;
    const unsigned char* i = in;
    if (size < sizeof h) {
        snprintf(why, whysize, "the GL layer's programs are %zu bytes, less than their %zu-byte header", size,
                 sizeof h);
        return -1;
    }
    memcpy(&h, in, sizeof h);
    if (h.records != sizeof *c || size != sizeof h + (size_t)h.records + h.arena) {
        snprintf(why, whysize,
                 "the GL layer's programs are %u bytes of records and %u of arena (%zu in all); this build reads %zu "
                 "of records",
                 (unsigned)h.records, (unsigned)h.arena, size, sizeof *c);
        return -1;
    }
    memcpy(c, i + sizeof h, sizeof *c);
    if (c->arena_used != h.arena || (h.arena && h.arena < ARENA_RESERVED) || h.arena > ARENA_MAX ||
        c->arena_dead > c->arena_used) {
        snprintf(why, whysize, "the GL layer's arena says %u bytes used and %u dropped, with %u saved",
                 (unsigned)c->arena_used, (unsigned)c->arena_dead, (unsigned)h.arena);
        return -1;
    }
    for (uint32_t n = 1; n <= SNX_GL_PROGRAM_NAMES; n++) {
        const SnxGlShader* sh = &c->shaders[n - 1];
        const SnxGlProgram* p = &c->programs[n - 1];
        if (sh->used > 1 || p->used > 1 || (sh->used && p->used)) {
            snprintf(why, whysize, "the GL layer's name %u is a shader %u times and a program %u times", (unsigned)n,
                     (unsigned)sh->used, (unsigned)p->used);
            return -1;
        }
        if (range_ok(sh->source, h.arena, "shader", n, why, whysize) ||
            range_ok(sh->log, h.arena, "shader", n, why, whysize) ||
            range_ok(p->log, h.arena, "program", n, why, whysize) ||
            range_ok(p->bound, h.arena, "program", n, why, whysize) ||
            range_ok(p->link, h.arena, "program", n, why, whysize))
            return -1;
        if (sh->source.size && i[sizeof h + sizeof *c + sh->source.at + sh->source.size - 1]) {
            snprintf(why, whysize, "the GL layer's shader %u has a source without its NUL", (unsigned)n);
            return -1;
        }
        for (int k = 0; p->used && k < 3; k++)
            if (p->shaders[k] && (p->shaders[k] > SNX_GL_PROGRAM_NAMES || !c->shaders[p->shaders[k] - 1].used)) {
                snprintf(why, whysize, "the GL layer's program %u has shader %u attached, which is not a shader",
                         (unsigned)n, (unsigned)p->shaders[k]);
                return -1;
            }
        if (p->linked &&
            (link_ok(p, n, why, whysize) || arrays_ok(p, i + sizeof h + sizeof *c + p->link.at, n, why, whysize)))
            return -1;
    }
    for (uint32_t n = 1; n <= SNX_GL_PIPELINES; n++) {
        const SnxGlPipeline* pp = &c->pipelines[n - 1];
        for (int k = 0; pp->used && k < 3; k++)
            if (pp->stages[k] > SNX_GL_PROGRAM_NAMES || (pp->stages[k] && !c->programs[pp->stages[k] - 1].used)) {
                snprintf(why, whysize, "the GL layer's program pipeline %u has program %u, which is not a program",
                         (unsigned)n, (unsigned)pp->stages[k]);
                return -1;
            }
    }
    return 0;
}
int snx_gl_program_state_bound(const void* in, uint32_t program, uint32_t pipeline) {
    const unsigned char* records = (const unsigned char*)in + sizeof(SnxGlProgramStateHeader);
    uint8_t used = 1;
    uint32_t pused = 1;
    if (program > SNX_GL_PROGRAM_NAMES || pipeline > SNX_GL_PIPELINES) return 0;
    if (program)
        memcpy(&used,
               records + offsetof(SnxGlPrograms, programs) + (program - 1) * sizeof(SnxGlProgram) +
                   offsetof(SnxGlProgram, used),
               1);
    if (pipeline)
        memcpy(&pused,
               records + offsetof(SnxGlPrograms, pipelines) + (pipeline - 1) * sizeof(SnxGlPipeline) +
                   offsetof(SnxGlPipeline, used),
               4);
    return used == 1 && pused == 1;
}
int snx_gl_program_state_load(const void* in, size_t size) {
    SnxGlProgramStateHeader h;
    const unsigned char* i = in;
    uint8_t* arena;
    uint32_t capacity = 64u << 10;
    (void)size;
    memcpy(&h, in, sizeof h);
    while (capacity < h.arena) capacity *= 2;
    arena = malloc(capacity);
    if (!arena) return -1;
    memcpy(&S, i + sizeof h, sizeof S);
    if (h.arena) memcpy(arena, i + sizeof h + sizeof S, h.arena);
    free(g_gl_program.arena);
    g_gl_program.arena = arena;
    g_gl_program.arena_capacity = capacity;
    for (uint32_t n = 1; n <= SNX_GL_PROGRAM_NAMES; n++) changed(n); /* what a platform made is of the old records */
    return 0;
}
