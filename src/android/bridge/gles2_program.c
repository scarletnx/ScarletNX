/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 2 and 3's shaders and programs (gles2.h). glslc compiles each stage into SPIR-V.

   glCompileShader compiles the source once for its errors and info log. glLinkProgram compiles each stage two more
   times. The first time, a binding map collects its samplers, images and blocks. The second time, the map puts each
   resource of one name at one binding of the program's set, the same in both stages: binding 0 is the vertex
   shader's default uniform block, 1 the fragment shader's, then the resources in the order the stages declare them.
   The fragment shader's inputs then take the locations of the vertex shader's outputs of the same names, which the
   link writes into its module's Location decorations: OpenGL ES links the stages by name, Vulkan by location.

   The loose uniforms are the members of each stage's default block. The link reads their offsets, strides and types
   from the module's block type, flattens structs and arrays of structs into GL's names ("s.a", "arr[2].b"), and
   gives each element a location. glUniform* writes the value into each stage's copy of its block, and a draw that
   finds the copy changed puts it into the frame's arena (gles2_program_defaults) */
#include "android/bridge/gles2.h"
#include "android/bridge/hle_impl.h"
#include <stdlib.h>
#include <string.h>

#define V g_gles_vk

enum {
    GL_INVALID_ENUM_ = 0x0500,
    GL_INVALID_VALUE_ = 0x0501,
    GL_INVALID_OPERATION_ = 0x0502,
    GL_FRAGMENT_SHADER_ = 0x8b30,
    GL_VERTEX_SHADER_ = 0x8b31,
    GL_COMPUTE_SHADER_ = 0x91b9,
    GL_FLOAT_ = 0x1406,
    GL_INT_ = 0x1404,
    GL_UNSIGNED_INT_ = 0x1405,
    GL_BOOL_ = 0x8b56,
};

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
    DEC_SET = 34,
    DEC_OFFSET = 35,
    SC_INPUT = 1,
    SC_UNIFORM = 2,
    SC_OUTPUT = 3,
    SC_UNIFORM_CONSTANT = 0,
};

/* a module: the word of each id's definition and of its OpName */
typedef struct {
    uint32_t* w;
    size_t n;
    uint32_t bound;
    uint32_t* def;  /* bound entries */
    uint32_t* name; /* bound entries */
} Spv;

static bool spv_open(Spv* s, uint32_t* words, size_t n) {
    *s = (Spv){words, n, n > 5 ? words[3] : 0, NULL, NULL};
    if (n <= 5 || !s->bound || s->bound > 4u << 20) return false;
    s->def = calloc((size_t)s->bound * 2, sizeof *s->def);
    if (!s->def) return false;
    s->name = s->def + s->bound;
    for (size_t i = 5; i < n;) {
        uint32_t count = s->w[i] >> 16, op = s->w[i] & 0xffff;
        if (!count || i + count > n) break;
        if (op == OP_NAME && s->w[i + 1] < s->bound) s->name[s->w[i + 1]] = (uint32_t)i;
        else if ((op >= OP_TYPE_BOOL && op <= OP_TYPE_POINTER) && s->w[i + 1] < s->bound) s->def[s->w[i + 1]] = (uint32_t)i;
        else if ((op == OP_CONSTANT || op == OP_VARIABLE) && count > 2 && s->w[i + 2] < s->bound)
            s->def[s->w[i + 2]] = (uint32_t)i;
        i += count;
    }
    return true;
}
static void spv_close(Spv* s) {
    free(s->def);
}
static uint32_t op(const Spv* s, uint32_t id) {
    return id < s->bound && s->def[id] ? s->w[s->def[id]] & 0xffff : 0;
}
static const uint32_t* at(const Spv* s, uint32_t id) {
    return &s->w[s->def[id]];
}
static const char* spv_name(const Spv* s, uint32_t id) {
    return id < s->bound && s->name[id] ? (const char*)&s->w[s->name[id] + 2] : "";
}
/* the literal of id's decoration, or of member's when member is not UINT32_MAX */
static bool decoration(const Spv* s, uint32_t id, uint32_t member, uint32_t dec, uint32_t* value, uint32_t* word) {
    for (size_t i = 5; i < s->n;) {
        uint32_t count = s->w[i] >> 16, o = s->w[i] & 0xffff;
        if (!count) break;
        if (member == UINT32_MAX && o == OP_DECORATE && count >= 3 && s->w[i + 1] == id && s->w[i + 2] == dec) {
            if (value) *value = count > 3 ? s->w[i + 3] : 0;
            if (word) *word = (uint32_t)i + 3;
            return true;
        }
        if (member != UINT32_MAX && o == OP_MEMBER_DECORATE && count >= 4 && s->w[i + 1] == id &&
            s->w[i + 2] == member && s->w[i + 3] == dec) {
            if (value) *value = count > 4 ? s->w[i + 4] : 0;
            return true;
        }
        i += count;
    }
    return false;
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
static uint32_t constant(const Spv* s, uint32_t id) {
    return op(s, id) == OP_CONSTANT ? at(s, id)[3] : 0;
}

/* the GL type of a SPIR-V scalar, vector or matrix type; 0 for others */
static unsigned gl_type(const Spv* s, uint32_t t) {
    uint32_t o = op(s, t);
    if (o == OP_TYPE_FLOAT) return GL_FLOAT_;
    if (o == OP_TYPE_INT) return at(s, t)[3] ? GL_INT_ : GL_UNSIGNED_INT_;
    if (o == OP_TYPE_BOOL) return GL_BOOL_;
    if (o == OP_TYPE_VECTOR) {
        unsigned base = gl_type(s, at(s, t)[2]), n = at(s, t)[3];
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
        static const unsigned types[3][3] = {{0x8b5a, 0x8b65, 0x8b66}, {0x8b67, 0x8b5b, 0x8b68}, {0x8b69, 0x8b6a, 0x8b5c}};
        if (cols < 2 || cols > 4 || rows < 2 || rows > 4) return 0;
        return types[cols - 2][rows - 2];
    }
    return 0;
}

/* the sampler's G2_T* dimension and GL type, from the type a variable points to */
static void sampler_kind(const Spv* s, uint32_t pointee, uint8_t* dim, unsigned* type) {
    if (op(s, pointee) == OP_TYPE_ARRAY || op(s, pointee) == OP_TYPE_RUNTIME_ARRAY) pointee = at(s, pointee)[2];
    if (op(s, pointee) == OP_TYPE_SAMPLED_IMAGE) pointee = at(s, pointee)[2];
    *dim = G2_T2D;
    *type = 0x8b5e; /* GL_SAMPLER_2D */
    if (op(s, pointee) != OP_TYPE_IMAGE) return;
    const uint32_t* i = at(s, pointee);
    uint32_t sdim = i[3], depth = i[4], arrayed = i[5], ms = i[6];
    unsigned base = gl_type(s, i[2]);
    int k = base == GL_INT_ ? 1 : base == GL_UNSIGNED_INT_ ? 2 : 0;
    if (ms) {
        *dim = G2_TMS;
        *type = k == 1 ? 0x9109 : k == 2 ? 0x910a : 0x9108;
    } else if (sdim == 2) {
        *dim = G2_T3D;
        *type = k == 1 ? 0x8dcb : k == 2 ? 0x8dd3 : 0x8b5f;
    } else if (sdim == 3) {
        *dim = G2_TCUBE;
        *type = depth ? 0x8dc5 : k == 1 ? 0x8dcc : k == 2 ? 0x8dd4 : 0x8b60;
    } else if (sdim == 5) {
        *dim = G2_T2D;
        *type = k == 1 ? 0x8dd0 : k == 2 ? 0x8dd8 : 0x8dc2; /* buffers */
    } else if (arrayed) {
        *dim = G2_TARRAY;
        *type = depth ? 0x8dc4 : k == 1 ? 0x8dcf : k == 2 ? 0x8dd7 : 0x8dc1;
    } else {
        *type = depth ? 0x8b62 : k == 1 ? 0x8dca : k == 2 ? 0x8dd2 : 0x8b5e;
    }
}

/* ---- the link's growing arrays ---- */

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
    G2Uniform* uniforms;
    uint32_t uniform_count;
    G2Input* inputs;
    uint32_t input_count;
    G2Input* outputs;
    uint32_t output_count;
    char* names;
    size_t names_size, names_capacity;
    char log[1024];
    /* the binding map's state */
    const char* source;
    uint32_t stage;
    bool final;
    uint32_t next;
    /* the vertex shader's outputs: name offset and location */
    struct {
        uint32_t name, location;
    } varyings[64];
    uint32_t varying_count;
    unsigned char* default_data[3];
    uint32_t default_size[3];
    uint32_t default_binding[3];
} Link;

static void* grow(void* p, uint32_t count, size_t size) {
    if (count & (count - 1)) return p; /* room till the next power of 2 */
    return realloc(p, (count ? count * 2 : 8) * size);
}
static uint32_t name_add(Link* l, const char* text, size_t len) {
    if (l->names_size + len + 1 > l->names_capacity) {
        size_t cap = (l->names_capacity ? l->names_capacity * 2 : 1024) + len;
        char* grown = realloc(l->names, cap);
        if (!grown) return 0;
        l->names = grown;
        l->names_capacity = cap;
    }
    uint32_t at = (uint32_t)l->names_size;
    memcpy(l->names + at, text, len);
    l->names[at + len] = 0;
    l->names_size += len + 1;
    return at;
}

/* the binding map: collecting (the first compilation of a stage), then placing (the second) */
static int link_bind(void* user, const GlslcResource* r, uint32_t* set, uint32_t* binding) {
    Link* l = user;
    *set = 0;
    if (r->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && !r->name.len) { /* the stage's default block */
        *binding = l->stage;
        return 0;
    }
    char text[256];
    int n = snprintf(text, sizeof text, "%.*s%s%.*s", (int)r->name.len, l->source + r->name.pos,
                     r->member.len ? "." : "", (int)r->member.len, l->source + r->member.pos);
    if (n < 0 || n >= (int)sizeof text) return 1;
    for (uint32_t i = 0; i < l->resource_count; ++i) {
        Resource* x = &l->resources[i];
        if (x->kind == (char)r->kind && !strcmp(l->names + x->name, text)) {
            x->stages |= 1u << l->stage;
            *binding = 3 + i;
            return 0;
        }
    }
    if (l->final) return 2; /* the second compilation declares what the first did */
    Resource* grown = grow(l->resources, l->resource_count, sizeof *grown);
    if (!grown) return 3;
    l->resources = grown;
    l->resources[l->resource_count] = (Resource){(char)r->kind, (char)(r->member.len != 0), r->texel_buffer,
                                                 r->array, r->binding, r->size, name_add(l, text, (size_t)n),
                                                 1u << l->stage};
    *binding = 3 + l->resource_count++;
    return 0;
}

/* glslc's workspace and module buffer, at the first compilation */
static bool compiler_ready(void) {
    if (g_gles2.words) return true;
    size_t size = snx_glslc_workspace_size();
    void* memory = malloc(size);
    uint32_t* words = malloc((size_t)SNX_GLSLC_MAX_WORDS * sizeof *words);
    if (!memory || !words || snx_glslc_workspace_init(&g_gles2.workspace, memory, size, &g_gles2.result) != 0) {
        free(memory);
        free(words);
        return false;
    }
    g_gles2.words = words;
    return true;
}

static GlslcStage stage_of(unsigned type) {
    return type == GL_VERTEX_SHADER_ ? SNX_GLSLC_VERTEX : type == GL_FRAGMENT_SHADER_ ? SNX_GLSLC_FRAGMENT
                                                                                    : SNX_GLSLC_COMPUTE;
}
/* the shader compiled into g_gles2.words, with the binding map when l: 0, or -1 with the error in g_gles2.result */
static int compile(const G2Shader* sh, Link* l) {
    if (!compiler_ready()) {
        snprintf(g_gles2.result.error, sizeof g_gles2.result.error, "glslc has no memory");
        return -1;
    }
    if (l) l->source = sh->source;
    /* the window is drawn with a negative viewport height: gl_FragCoord and gl_PointCoord through the draw's flip */
    GlslcOptions o = {stage_of(sh->type), l ? link_bind : NULL, l, l != NULL, sh->type == GL_FRAGMENT_SHADER_};
    return snx_glslc_compile(&g_gles2.workspace, &o, sh->source, sh->length, g_gles2.words, SNX_GLSLC_MAX_WORDS,
                             &g_gles2.result);
}

/* ---- shaders ---- */

static G2Shader* shader_object(unsigned name) {
    return name && name < g_gles2.shader_count && g_gles2.shaders[name].used ? &g_gles2.shaders[name] : NULL;
}
G2Program* gles2_program(unsigned name) {
    return name && name < g_gles2.program_count && g_gles2.programs[name].used ? &g_gles2.programs[name] : NULL;
}
/* a free name of shaders and programs, which GL numbers in one space */
static unsigned new_name(void) {
    unsigned name = 1;
    while ((name < g_gles2.shader_count && g_gles2.shaders[name].used) ||
           (name < g_gles2.program_count && g_gles2.programs[name].used))
        ++name;
    return name;
}
static void* slot(void** items, size_t* count, size_t size, unsigned name) {
    if (name >= *count) {
        size_t n = (size_t)name + 32;
        unsigned char* grown = realloc(*items, n * size);
        if (!grown) return NULL;
        memset(grown + *count * size, 0, (n - *count) * size);
        *items = grown;
        *count = n;
    }
    return (unsigned char*)*items + (size_t)name * size;
}

static void g_create_shader(CPU* c, Gles2Context* gl) {
    unsigned type = (unsigned)X(0);
    X(0) = 0;
    if (type != GL_VERTEX_SHADER_ && type != GL_FRAGMENT_SHADER_ && type != GL_COMPUTE_SHADER_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    unsigned name = new_name();
    G2Shader* s = slot((void**)&g_gles2.shaders, &g_gles2.shader_count, sizeof *s, name);
    if (!s) return;
    *s = (G2Shader){.used = true, .type = type};
    X(0) = name;
}
static void shader_free(G2Shader* s) {
    free(s->source);
    free(s->log);
    *s = (G2Shader){0};
}
static void g_delete_shader(CPU* c, Gles2Context* gl) {
    G2Shader* s = shader_object((unsigned)X(0));
    if (!s) {
        if (X(0)) gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    if (s->attached) s->deleted = true;
    else shader_free(s);
}
static void g_is_shader(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = shader_object((unsigned)X(0)) != NULL;
}
/* glShaderSource(shader, count, strings, lengths): the strings joined */
static void g_shader_source(CPU* c, Gles2Context* gl) {
    G2Shader* s = shader_object((unsigned)X(0));
    int count = (int)X(1);
    const unsigned char* strings = PTR(2);
    const unsigned char* lengths = PTR(3);
    if (!s || count < 0) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    size_t total = 0;
    for (int pass = 0; pass < 2; ++pass) {
        size_t n = 0;
        for (int i = 0; i < count; ++i) {
            uint64_t p;
            memcpy(&p, strings + 8 * (size_t)i, 8);
            int32_t len = -1;
            if (lengths) memcpy(&len, lengths + 4 * (size_t)i, 4);
            const char* text = (const char*)(uintptr_t)p;
            size_t l = !text ? 0 : len < 0 ? strlen(text) : (size_t)len;
            if (pass) memcpy(s->source + n, text, l);
            n += l;
        }
        if (!pass) {
            total = n;
            char* grown = realloc(s->source, total + 1);
            if (!grown) return;
            s->source = grown;
        }
    }
    s->source[total] = 0;
    s->length = total;
}
static char* log_of(const char* text) {
    size_t n = strlen(text);
    char* log = malloc(n + 1);
    if (log) memcpy(log, text, n + 1);
    return log;
}
static void g_compile_shader(CPU* c, Gles2Context* gl) {
    G2Shader* s = shader_object((unsigned)X(0));
    if (!s) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    free(s->log);
    s->log = NULL;
    s->compiled = s->source && compile(s, NULL) == 0;
    char text[600];
    if (!s->compiled)
        snprintf(text, sizeof text, "ERROR: %u:%u: %s\n", g_gles2.result.line, g_gles2.result.column,
                 s->source ? g_gles2.result.error : "no source");
    else if (g_gles2.result.warnings)
        snprintf(text, sizeof text, "WARNING: %s\n", g_gles2.result.warning);
    else
        text[0] = 0;
    s->log = log_of(text);
    if (!s->compiled) fprintf(stderr, "scarletnx-android: GLES 2: a shader does not compile: %s", text);
}
static void g_get_shaderiv(CPU* c, Gles2Context* gl) {
    G2Shader* s = shader_object((unsigned)X(0));
    int32_t v = 0;
    if (!s) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    switch ((unsigned)X(1)) {
    case 0x8b4f: v = (int32_t)s->type; break;
    case 0x8b80: v = s->deleted; break;
    case 0x8b81: v = s->compiled; break;
    case 0x8b84: v = s->log && s->log[0] ? (int32_t)strlen(s->log) + 1 : 0; break;
    case 0x8b88: v = s->source ? (int32_t)s->length + 1 : 0; break;
    default: gles2_error(gl, GL_INVALID_ENUM_); return;
    }
    memcpy(PTR(2), &v, 4);
}
/* a string out to the guest's buffer of size bytes, its length (without the NUL) to *length when given */
static void string_out(const char* text, int32_t size, void* length, char* out) {
    size_t n = text ? strlen(text) : 0;
    if (size <= 0) n = 0;
    else if (n > (size_t)size - 1) n = (size_t)size - 1;
    if (out && size > 0) {
        if (n) memcpy(out, text, n);
        out[n] = 0;
    }
    if (length) {
        int32_t l = (int32_t)n;
        memcpy(length, &l, 4);
    }
}
static void g_get_shader_info_log(CPU* c, Gles2Context* gl) {
    G2Shader* s = shader_object((unsigned)X(0));
    if (!s) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    string_out(s->log, (int32_t)X(1), PTR(2), PTR(3));
}
static void g_get_shader_source(CPU* c, Gles2Context* gl) {
    G2Shader* s = shader_object((unsigned)X(0));
    if (!s) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    string_out(s->source, (int32_t)X(1), PTR(2), PTR(3));
}
static void g_get_shader_precision_format(CPU* c, Gles2Context* gl) {
    (void)gl;
    unsigned precision = (unsigned)X(1);
    int32_t range[2], bits;
    if (precision <= 0x8df2) { /* GL_LOW_FLOAT, GL_MEDIUM_FLOAT, GL_HIGH_FLOAT */
        bool high = precision == 0x8df2;
        range[0] = range[1] = high ? 127 : 15;
        bits = high ? 23 : 10;
    } else {
        bool high = precision == 0x8df5;
        range[0] = high ? 31 : 15;
        range[1] = high ? 30 : 14;
        bits = 0;
    }
    memcpy(PTR(2), range, 8);
    memcpy(PTR(3), &bits, 4);
}
static void g_shader_binary(CPU* c, Gles2Context* gl) {
    (void)c;
    gles2_error(gl, GL_INVALID_ENUM_); /* no binary formats */
}

/* ---- programs ---- */

static void program_unlink(G2Program* p) {
    for (int i = 0; i < 3; ++i) {
        if (p->modules[i]) V.vk.vkDestroyShaderModule(V.device, p->modules[i], NULL);
        p->modules[i] = VK_NULL_HANDLE;
    }
    /* the pipelines made with the layout are retired by their cache when the generation changes */
    if (p->compute) gles_vk_retire((GlesGarbage){.pipeline = p->compute});
    p->compute = VK_NULL_HANDLE;
    /* the frame being recorded may have used the layouts: they go when it is done */
    if (p->layout || p->set_layout) gles_vk_retire((GlesGarbage){.layout = p->layout, .set_layout = p->set_layout});
    gles2_pipelines_forget(p);
    p->layout = VK_NULL_HANDLE;
    p->set_layout = VK_NULL_HANDLE;
    free(p->bindings); /* the one block of the link's arrays */
    p->bindings = NULL;
    p->uniforms = NULL;
    p->locations = NULL;
    p->inputs = p->outputs = NULL;
    p->units = NULL;
    p->names = NULL;
    for (int i = 0; i < 3; ++i) p->defaults[i] = NULL, p->default_size[i] = 0;
    p->binding_count = p->uniform_count = p->location_count = p->input_count = p->output_count = 0;
    p->unit_count = p->block_count = 0;
    p->linked = false;
}

static void g_create_program(CPU* c, Gles2Context* gl) {
    (void)gl;
    unsigned name = new_name();
    G2Program* p = slot((void**)&g_gles2.programs, &g_gles2.program_count, sizeof *p, name);
    X(0) = 0;
    if (!p) return;
    *p = (G2Program){.used = true};
    X(0) = name;
}
static void detach(G2Program* p, int i) {
    G2Shader* s = shader_object(p->shaders[i]);
    p->shaders[i] = 0;
    if (!s) return;
    if (s->attached) s->attached--;
    if (!s->attached && s->deleted) shader_free(s);
}
static void program_free(G2Program* p) {
    program_unlink(p);
    for (int i = 0; i < 3; ++i) detach(p, i);
    free(p->log);
    free(p->bound);
    *p = (G2Program){0};
}
void gles2_programs_vk_free(void) {
    for (size_t i = 0; i < g_gles2.program_count; ++i) program_unlink(&g_gles2.programs[i]);
}
static void g_delete_program(CPU* c, Gles2Context* gl) {
    unsigned name = (unsigned)X(0);
    G2Program* p = gles2_program(name);
    if (!p) {
        if (name) gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    if (gl->program == name) p->deleted = true;
    else program_free(p);
}
static void g_is_program(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = gles2_program((unsigned)X(0)) != NULL;
}
static int stage_index(unsigned type) {
    return type == GL_VERTEX_SHADER_ ? 0 : type == GL_FRAGMENT_SHADER_ ? 1 : 2;
}
static void g_attach_shader(CPU* c, Gles2Context* gl) {
    G2Program* p = gles2_program((unsigned)X(0));
    G2Shader* s = shader_object((unsigned)X(1));
    if (!p || !s) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    int i = stage_index(s->type);
    if (p->shaders[i]) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    p->shaders[i] = (unsigned)X(1);
    s->attached++;
}
static void g_detach_shader(CPU* c, Gles2Context* gl) {
    G2Program* p = gles2_program((unsigned)X(0));
    G2Shader* s = shader_object((unsigned)X(1));
    if (!p || !s) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    int i = stage_index(s->type);
    if (p->shaders[i] == (unsigned)X(1)) detach(p, i);
    else gles2_error(gl, GL_INVALID_OPERATION_);
}
static void g_get_attached_shaders(CPU* c, Gles2Context* gl) {
    G2Program* p = gles2_program((unsigned)X(0));
    int32_t max = (int32_t)X(1), n = 0;
    if (!p) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    for (int i = 0; i < 3; ++i)
        if (p->shaders[i] && n < max) memcpy((unsigned char*)PTR(3) + 4 * n++, &p->shaders[i], 4);
    if (X(2)) memcpy(PTR(2), &n, 4);
}
/* glBindAttribLocation(program, index, name): kept for the next link */
static void g_bind_attrib_location(CPU* c, Gles2Context* gl) {
    G2Program* p = gles2_program((unsigned)X(0));
    const char* name = STR(2);
    uint32_t index = (uint32_t)X(1);
    if (!p || index >= G2_ATTRIBS || !name) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    size_t n = strlen(name) + 1;
    char* grown = realloc(p->bound, p->bound_size + n + 4);
    if (!grown) return;
    p->bound = grown;
    memcpy(p->bound + p->bound_size, name, n);
    memcpy(p->bound + p->bound_size + n, &index, 4);
    p->bound_size += n + 4;
}
static int32_t bound_location(const G2Program* p, const char* name) {
    int32_t found = -1;
    for (size_t i = 0; i < p->bound_size;) {
        size_t n = strlen(p->bound + i) + 1;
        if (!strcmp(p->bound + i, name)) memcpy(&found, p->bound + i + n, 4); /* the last bind of it */
        i += n + 4;
    }
    return found;
}

/* ---- the link ---- */

static G2Uniform* uniform_named(Link* l, const char* name) {
    for (uint32_t i = 0; i < l->uniform_count; ++i)
        if (!strcmp(l->names + l->uniforms[i].name, name)) return &l->uniforms[i];
    return NULL;
}
static G2Uniform* uniform_add(Link* l, const char* name) {
    G2Uniform* u = uniform_named(l, name);
    if (u) return u;
    G2Uniform* grown = grow(l->uniforms, l->uniform_count, sizeof *grown);
    if (!grown) return NULL;
    l->uniforms = grown;
    u = &l->uniforms[l->uniform_count++];
    *u = (G2Uniform){.name = name_add(l, name, strlen(name)), .count = 1, .block = -1, .offset = {-1, -1, -1},
                     .location = -1, .sampler = -1};
    return u;
}

/* the members of type at offset, as GL names them under prefix: each a uniform of the stage (block: its index) */
static void flatten(Link* l, const Spv* s, uint32_t type, const char* prefix, uint32_t offset, uint32_t mstride,
                    int stage, int block) {
    char name[256];
    uint32_t o = op(s, type);
    if (o == OP_TYPE_STRUCT) {
        uint32_t count = (at(s, type)[0] >> 16) - 2;
        for (uint32_t m = 0; m < count; ++m) {
            uint32_t off = 0, ms = 0;
            decoration(s, type, m, DEC_OFFSET, &off, NULL);
            decoration(s, type, m, DEC_MATRIX_STRIDE, &ms, NULL);
            snprintf(name, sizeof name, "%s%s%s", prefix, prefix[0] ? "." : "", member_name(s, type, m));
            flatten(l, s, at(s, type)[2 + m], name, offset + off, ms, stage, block);
        }
        return;
    }
    if (o == OP_TYPE_ARRAY || o == OP_TYPE_RUNTIME_ARRAY) {
        uint32_t elem = at(s, type)[2], len = o == OP_TYPE_ARRAY ? constant(s, at(s, type)[3]) : 1, stride = 0;
        decoration(s, type, UINT32_MAX, DEC_ARRAY_STRIDE, &stride, NULL);
        if (op(s, elem) == OP_TYPE_STRUCT || op(s, elem) == OP_TYPE_ARRAY) {
            for (uint32_t i = 0; i < len && i < 256; ++i) {
                snprintf(name, sizeof name, "%s[%u]", prefix, i);
                flatten(l, s, elem, name, offset + i * stride, mstride, stage, block);
            }
            return;
        }
        snprintf(name, sizeof name, "%s[0]", prefix);
        G2Uniform* u = uniform_add(l, name);
        if (!u) return;
        u->type = gl_type(s, elem);
        u->count = (uint16_t)(len ? len : 1);
        u->array_stride = (uint16_t)stride;
        u->matrix_stride = (uint16_t)mstride;
        u->offset[stage] = (int32_t)offset;
        u->block = (int16_t)block;
        return;
    }
    G2Uniform* u = uniform_add(l, prefix);
    if (!u) return;
    u->type = gl_type(s, type);
    u->matrix_stride = (uint16_t)mstride;
    u->offset[stage] = (int32_t)offset;
    u->block = (int16_t)block;
}

/* what a stage's module says: its default block's uniforms, its samplers' kinds, the vertex shader's outputs, and
   the fragment shader's inputs moved to them */
static void reflect_module(Link* l, uint32_t* words, size_t n, int stage) {
    Spv s;
    if (!spv_open(&s, words, n)) return;
    for (uint32_t id = 1; id < s.bound; ++id) {
        if (op(&s, id) != OP_VARIABLE) continue;
        const uint32_t* v = at(&s, id);
        uint32_t storage = v[3], pointer = v[1];
        if (op(&s, pointer) != OP_TYPE_POINTER) continue;
        uint32_t pointee = at(&s, pointer)[3];
        uint32_t binding = 0, location = 0, word = 0;
        if (storage == SC_UNIFORM && decoration(&s, id, UINT32_MAX, DEC_BINDING, &binding, NULL)) {
            if (binding == (uint32_t)stage && binding < 3) { /* the default block */
                flatten(l, &s, pointee, "", 0, 0, stage, -1);
            } else if (binding >= 3 && binding - 3 < l->resource_count) {
                Resource* r = &l->resources[binding - 3];
                if (r->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK) {
                    uint32_t index = 0;
                    for (uint32_t i = 0; i < binding - 3; ++i) index += l->resources[i].kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK;
                    uint32_t t = op(&s, pointee) == OP_TYPE_ARRAY ? at(&s, pointee)[2] : pointee;
                    flatten(l, &s, t, "", 0, 0, stage, (int)index);
                }
            }
        } else if (storage == SC_UNIFORM_CONSTANT && decoration(&s, id, UINT32_MAX, DEC_BINDING, &binding, NULL) &&
                   binding >= 3 && binding - 3 < l->resource_count) {
            Resource* r = &l->resources[binding - 3];
            uint8_t dim;
            unsigned type;
            sampler_kind(&s, pointee, &dim, &type);
            r->size = type; /* a sampler's size field holds its GL type, and member_dot its dimension */
            r->texel_buffer = (uint8_t)(r->texel_buffer | (uint8_t)(dim << 4));
        } else if (storage == SC_OUTPUT && stage == 0 && decoration(&s, id, UINT32_MAX, DEC_LOCATION, &location, NULL) &&
                   l->varying_count < 64) {
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

static void link_fail(G2Program* p, Link* l, const char* what) {
    free(p->log);
    p->log = log_of(what);
    p->linked = false;
    fprintf(stderr, "scarletnx-android: GLES 2: a program does not link: %s\n", what);
    (void)l;
}

/* the interface the reflection lists of a stage give: the vertex inputs and the fragment outputs */
static void interface(Link* l, const G2Shader* sh, int stage) {
    const GlslcResult* r = &g_gles2.result;
    for (uint32_t i = 0; i < r->reflect_count; ++i) {
        const GlslcReflect* e = &r->reflect[i];
        bool input = e->kind == SNX_GLSLC_REFLECT_INPUT && stage == 0;
        bool output = e->kind == SNX_GLSLC_REFLECT_OUTPUT && stage == 1;
        if (!input && !output) continue;
        if (e->name.len >= 3 && !memcmp(sh->source + e->name.pos, "gl_", 3)) continue;
        G2Input** list = input ? &l->inputs : &l->outputs;
        uint32_t* count = input ? &l->input_count : &l->output_count;
        G2Input* grown = grow(*list, *count, sizeof *grown);
        if (!grown) return;
        *list = grown;
        static const unsigned scalars[4] = {0x8b56, GL_INT_, GL_UNSIGNED_INT_, GL_FLOAT_};
        unsigned type = e->base < 4 ? scalars[e->base] : GL_FLOAT_;
        if (e->cols > 1) {
            static const unsigned m[3][3] = {{0x8b5a, 0x8b65, 0x8b66}, {0x8b67, 0x8b5b, 0x8b68}, {0x8b69, 0x8b6a, 0x8b5c}};
            type = m[e->cols - 2][e->rows - 2];
        } else if (e->rows > 1) {
            type = e->base == SNX_GLSLC_BASE_FLOAT ? 0x8b50 + e->rows - 2u
                   : e->base == SNX_GLSLC_BASE_INT ? 0x8b53 + e->rows - 2u
                                                   : 0x8dc6 + e->rows - 2u;
        }
        (*list)[(*count)++] = (G2Input){name_add(l, sh->source + e->name.pos, e->name.len), type,
                                         (uint16_t)(e->array ? e->array : 1), e->base, e->cols, -1,
                                         (int32_t)e->location};
    }
}

/* the GL locations of the vertex inputs: glBindAttribLocation's, else the module's when free, else the first free */
static void attribute_locations(Link* l, const G2Program* p) {
    uint32_t taken = 0;
    for (uint32_t i = 0; i < l->input_count; ++i) {
        G2Input* in = &l->inputs[i];
        int32_t b = bound_location(p, l->names + in->name);
        if (b >= 0) {
            in->location = b;
            for (uint32_t k = 0; k < (uint32_t)in->count * in->columns; ++k) taken |= 1u << ((uint32_t)b + k);
        }
    }
    for (int pass = 0; pass < 2; ++pass)
        for (uint32_t i = 0; i < l->input_count; ++i) {
            G2Input* in = &l->inputs[i];
            if (in->location >= 0) continue;
            uint32_t span = (uint32_t)in->count * in->columns, mask = span >= 32 ? 0xffffffffu : (1u << span) - 1;
            int32_t want = -1;
            if (!pass && in->spirv >= 0 && in->spirv + span <= G2_ATTRIBS && !(taken & (mask << in->spirv)))
                want = in->spirv;
            for (uint32_t loc = 0; pass && want < 0 && loc + span <= G2_ATTRIBS; ++loc)
                if (!(taken & (mask << loc))) want = (int32_t)loc;
            if (want >= 0) {
                in->location = want;
                taken |= mask << want;
            }
        }
}

/* the program's set: its descriptor set layout and pipeline layout */
static bool program_layouts(G2Program* p, bool compute) {
    VkDescriptorSetLayoutBinding b[64];
    uint32_t n = 0;
    VkShaderStageFlags stages = compute ? VK_SHADER_STAGE_COMPUTE_BIT
                                        : VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 0; i < p->binding_count && n < 64; ++i) {
        const G2Binding* x = &p->bindings[i];
        VkDescriptorType type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        if (x->kind == SNX_GLSLC_REFLECT_SAMPLER)
            type = x->texel_buffer ? VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        else if (x->kind == SNX_GLSLC_REFLECT_IMAGE)
            type = x->texel_buffer ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        else if (x->kind == SNX_GLSLC_REFLECT_BUFFER_BLOCK)
            type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[n++] = (VkDescriptorSetLayoutBinding){x->binding, type, x->count, stages, NULL};
    }
    VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = n,
                                           .pBindings = b};
    if (V.vk.vkCreateDescriptorSetLayout(V.device, &set, NULL, &p->set_layout) != VK_SUCCESS) return false;
    /* the fragment shader's flip of gl_FragCoord and gl_PointCoord (GlslcOptions.flip_y) */
    VkPushConstantRange flip = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &p->set_layout,
                                         .pushConstantRangeCount = compute ? 0 : 1,
                                         .pPushConstantRanges = &flip};
    return V.vk.vkCreatePipelineLayout(V.device, &layout, NULL, &p->layout) == VK_SUCCESS;
}

/* the samplers as GL's uniforms, after the others: one for each, or one for each element of an array of structs'
   member (ms[i].member). Each takes its first unit in the program's units, in the order of the resources */
static void sampler_uniforms(Link* l) {
    uint32_t unit = 0;
    for (uint32_t i = 0; i < l->resource_count; ++i) {
        const Resource* r = &l->resources[i];
        if (r->kind != SNX_GLSLC_REFLECT_SAMPLER) continue;
        uint32_t count = r->array ? r->array : 1;
        char name[300];
        snprintf(name, sizeof name, "%s", l->names + r->name);
        const char* dot = strchr(name, '.');
        bool struct_array = dot && r->array;
        for (uint32_t k = 0; k < (struct_array ? count : 1u); ++k) {
            char text[320];
            if (struct_array) snprintf(text, sizeof text, "%.*s[%u]%s", (int)(dot - name), name, k, dot);
            else if (r->array) snprintf(text, sizeof text, "%s[0]", name);
            else snprintf(text, sizeof text, "%s", name);
            G2Uniform* u = uniform_add(l, text);
            if (!u) return;
            u->type = r->size;
            u->count = (uint16_t)(struct_array ? 1 : count);
            u->sampler = (int32_t)(unit + (struct_array ? k : 0));
        }
        unit += count;
    }
}

/* the link's arrays into the program's one block */
static bool program_arrays(G2Program* p, Link* l) {
    uint32_t bindings = l->resource_count + 3, units = 0, locations = 0;
    for (uint32_t i = 0; i < l->resource_count; ++i)
        if (l->resources[i].kind == SNX_GLSLC_REFLECT_SAMPLER) units += l->resources[i].array ? l->resources[i].array : 1;
    for (uint32_t i = 0; i < l->uniform_count; ++i)
        if (l->uniforms[i].block < 0) locations += l->uniforms[i].count;
    size_t sizes[] = {bindings * sizeof(G2Binding),
                      l->uniform_count * sizeof(G2Uniform),
                      locations * sizeof(int32_t),
                      units * sizeof(int32_t),
                      l->input_count * sizeof(G2Input),
                      l->output_count * sizeof(G2Input),
                      l->default_size[0],
                      l->default_size[1],
                      l->default_size[2],
                      l->names_size + 1};
    size_t total = 0, offsets[10];
    for (int i = 0; i < 10; ++i) {
        offsets[i] = total;
        total += (sizes[i] + 15) & ~(size_t)15;
    }
    unsigned char* block = calloc(1, total ? total : 16);
    if (!block) return false;
    p->bindings = (G2Binding*)(block + offsets[0]);
    p->uniforms = (G2Uniform*)(block + offsets[1]);
    p->locations = (int32_t*)(block + offsets[2]);
    p->units = (int32_t*)(block + offsets[3]);
    p->inputs = (G2Input*)(block + offsets[4]);
    p->outputs = (G2Input*)(block + offsets[5]);
    for (int i = 0; i < 3; ++i) {
        p->defaults[i] = block + offsets[6 + i];
        p->default_size[i] = l->default_size[i];
        p->default_binding[i] = (uint32_t)i;
        p->default_dirty[i] = true;
        p->default_serial[i] = 0;
    }
    p->names = (char*)(block + offsets[9]);
    if (l->names_size) memcpy(p->names, l->names, l->names_size);
    p->names_size = l->names_size;
    /* the bindings: the default blocks, then the resources */
    uint32_t nb = 0, unit = 0, block_index = 0;
    for (uint32_t i = 0; i < 3; ++i)
        if (l->default_size[i])
            p->bindings[nb++] = (G2Binding){.kind = SNX_GLSLC_REFLECT_UNIFORM_BLOCK, .stage = (uint8_t)i, .count = 1,
                                            .binding = (uint16_t)i, .size = l->default_size[i]};
    for (uint32_t i = 0; i < l->resource_count; ++i) {
        const Resource* r = &l->resources[i];
        G2Binding* b = &p->bindings[nb++];
        *b = (G2Binding){.name = r->name, .kind = (uint8_t)r->kind, .count = (uint16_t)(r->array ? r->array : 1),
                         .binding = (uint16_t)(3 + i), .size = r->size};
        if (r->kind == SNX_GLSLC_REFLECT_SAMPLER) {
            b->texel_buffer = r->texel_buffer & 1;
            b->dim = (uint8_t)(r->texel_buffer >> 4);
            b->first = (uint16_t)unit;
            for (uint32_t k = 0; k < b->count; ++k)
                p->units[unit + k] = r->source_binding >= 0 ? r->source_binding + (int32_t)k : 0;
            unit += b->count;
        } else if (r->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK) {
            b->first = (uint16_t)block_index;
            if (block_index < G2_UBO_BINDINGS)
                p->block_binding[block_index] = r->source_binding >= 0 ? (unsigned)r->source_binding : 0;
            block_index++;
        }
    }
    p->binding_count = nb;
    p->unit_count = unit;
    p->block_count = block_index;
    /* the uniforms, each element of the default block's and the samplers' with a location */
    if (l->uniform_count) memcpy(p->uniforms, l->uniforms, l->uniform_count * sizeof *p->uniforms);
    p->uniform_count = l->uniform_count;
    uint32_t loc = 0;
    for (uint32_t i = 0; i < p->uniform_count; ++i) {
        G2Uniform* u = &p->uniforms[i];
        if (u->block >= 0) continue;
        u->location = (int32_t)loc;
        for (uint32_t k = 0; k < u->count; ++k) p->locations[loc++] = (int32_t)(i * 65536 + k);
    }
    p->location_count = loc;
    if (l->input_count) memcpy(p->inputs, l->inputs, l->input_count * sizeof *p->inputs);
    p->input_count = l->input_count;
    if (l->output_count) memcpy(p->outputs, l->outputs, l->output_count * sizeof *p->outputs);
    p->output_count = l->output_count;
    return true;
}

static void g_link_program(CPU* c, Gles2Context* gl) {
    G2Program* p = gles2_program((unsigned)X(0));
    if (!p) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    program_unlink(p);
    p->generation++;
    Link* l = calloc(1, sizeof *l);
    if (!l) return;
    name_add(l, "", 0); /* offset 0 is the empty name: a default block's */
    G2Shader* stages[3] = {shader_object(p->shaders[0]), shader_object(p->shaders[1]), shader_object(p->shaders[2])};
    bool compute = stages[2] != NULL;
    if (!compute && (!stages[0] || !stages[1])) {
        link_fail(p, l, "a program needs a vertex and a fragment shader");
        goto done;
    }
    /* the first compilations: the resources of every stage */
    for (int i = 0; i < 3; ++i) {
        if (!stages[i]) continue;
        if (!stages[i]->compiled || !stages[i]->source) {
            link_fail(p, l, "a shader of the program did not compile");
            goto done;
        }
        l->stage = (uint32_t)i;
        if (compile(stages[i], l) != 0) {
            link_fail(p, l, g_gles2.result.error);
            goto done;
        }
    }
    /* the second: the modules, in order, the vertex shader's outputs before the fragment shader's inputs */
    l->final = true;
    for (int i = 0; i < 3; ++i) {
        if (!stages[i]) continue;
        l->stage = (uint32_t)i;
        if (compile(stages[i], l) != 0) {
            link_fail(p, l, g_gles2.result.error);
            goto done;
        }
        for (uint32_t k = 0; k < g_gles2.result.reflect_count; ++k) {
            const GlslcReflect* e = &g_gles2.result.reflect[k];
            if (e->kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && !e->name.len) l->default_size[i] = e->offset;
        }
        if (i == 2)
            for (int k = 0; k < 3; ++k) p->local_size[k] = 1;
        interface(l, stages[i], i);
        reflect_module(l, g_gles2.words, g_gles2.result.words, i);
        p->modules[i] = gles_vk_shader(g_gles2.words, (size_t)g_gles2.result.words * 4);
        if (!p->modules[i]) {
            link_fail(p, l, "Vulkan does not take a stage's module");
            goto done;
        }
    }
    attribute_locations(l, p);
    sampler_uniforms(l);
    if (!program_arrays(p, l) || !program_layouts(p, compute)) {
        link_fail(p, l, "no memory for the program");
        goto done;
    }
    free(p->log);
    p->log = log_of("");
    p->linked = true;
done:
    free(l->resources);
    free(l->uniforms);
    free(l->inputs);
    free(l->outputs);
    free(l->names);
    free(l);
}

/* ---- the program in use, and its uniforms ---- */

static void g_use_program(CPU* c, Gles2Context* gl) {
    unsigned name = (unsigned)X(0);
    G2Program* p = gles2_program(name);
    if (name && (!p || !p->linked)) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    G2Program* old = gles2_program(gl->program);
    if (old && old->deleted && gl->program != name) program_free(old);
    gl->program = name;
}
static void g_validate_program(CPU* c, Gles2Context* gl) {
    if (!gles2_program((unsigned)X(0))) gles2_error(gl, GL_INVALID_VALUE_);
}

/* the components of a GL type, and its columns (1 for a scalar or vector) */
static int components(unsigned type, int* columns) {
    *columns = 1;
    switch (type) {
    case 0x8b50: case 0x8b53: case 0x8dc6: case 0x8b57: return 2;
    case 0x8b51: case 0x8b54: case 0x8dc7: case 0x8b58: return 3;
    case 0x8b52: case 0x8b55: case 0x8dc8: case 0x8b59: return 4;
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
static bool is_bool(unsigned type) {
    return type == GL_BOOL_ || (type >= 0x8b57 && type <= 0x8b59);
}

/* the uniform and element of a location of the program in use; NULL (and the error) for none */
static G2Uniform* uniform_at(Gles2Context* gl, G2Program* p, int32_t location, uint32_t* element) {
    if (!p || !p->linked || location < -1 || location >= (int32_t)p->location_count) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return NULL;
    }
    if (location == -1) return NULL;
    int32_t e = p->locations[location];
    *element = (uint32_t)e & 0xffff;
    return &p->uniforms[(uint32_t)e >> 16];
}

/* count values of comps 32-bit components (a matrix: cols columns) at data into the uniform at location */
static void uniform_write(Gles2Context* gl, int32_t location, int count, const unsigned char* data, int comps,
                          char kind, bool transpose) {
    G2Program* p = gles2_program(gl->program);
    uint32_t element = 0;
    G2Uniform* u = uniform_at(gl, p, location, &element);
    if (!u || count <= 0) return;
    if ((uint32_t)count > u->count - element) count = (int)(u->count - element);
    if (u->sampler >= 0) {
        for (int k = 0; k < count; ++k) {
            int32_t unit;
            memcpy(&unit, data + 4 * (size_t)k, 4);
            if (kind == 'f') {
                float f;
                memcpy(&f, data + 4 * (size_t)k, 4);
                unit = (int32_t)f;
            }
            p->units[u->sampler + (int32_t)element + k] = unit;
        }
        return;
    }
    int columns;
    int n = components(u->type, &columns);
    if (n != comps) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    int rows = n / columns;
    for (int s = 0; s < 3; ++s) {
        if (u->offset[s] < 0 || !p->defaults[s]) continue;
        for (int k = 0; k < count; ++k) {
            size_t base = (size_t)u->offset[s] + (size_t)(element + (uint32_t)k) * u->array_stride;
            const unsigned char* v = data + (size_t)k * (size_t)n * 4;
            for (int col = 0; col < columns; ++col)
                for (int row = 0; row < rows; ++row) {
                    size_t to = base + (size_t)col * (columns > 1 ? u->matrix_stride : 0) + (size_t)row * 4;
                    if (to + 4 > p->default_size[s]) continue;
                    int from = transpose ? row * columns + col : col * rows + row;
                    uint32_t bits;
                    memcpy(&bits, v + 4 * from, 4);
                    if (is_bool(u->type)) {
                        float f;
                        memcpy(&f, &bits, 4);
                        bits = kind == 'f' ? f != 0 : bits != 0;
                    }
                    memcpy(p->defaults[s] + to, &bits, 4);
                }
        }
        p->default_dirty[s] = true;
    }
}

#define UNIFORM_SCALARS(n, suffix, kind)                                                                               \
    static void g_uniform##n##suffix(CPU* c, Gles2Context* gl) {                                                       \
        uint32_t v[4];                                                                                                 \
        for (unsigned i = 0; i < n; ++i) {                                                                             \
            if (kind == 'f') {                                                                                         \
                float f = hle_f(c, i);                                                                                 \
                memcpy(&v[i], &f, 4);                                                                                  \
            } else {                                                                                                   \
                v[i] = (uint32_t)X(1 + i);                                                                             \
            }                                                                                                          \
        }                                                                                                              \
        uniform_write(gl, (int32_t)X(0), 1, (const unsigned char*)v, n, kind, false);                                  \
    }                                                                                                                  \
    static void g_uniform##n##suffix##v(CPU* c, Gles2Context* gl) {                                                    \
        uniform_write(gl, (int32_t)X(0), (int)X(1), PTR(2), n, kind, false);                                           \
    }
UNIFORM_SCALARS(1, f, 'f')
UNIFORM_SCALARS(2, f, 'f')
UNIFORM_SCALARS(3, f, 'f')
UNIFORM_SCALARS(4, f, 'f')
UNIFORM_SCALARS(1, i, 'i')
UNIFORM_SCALARS(2, i, 'i')
UNIFORM_SCALARS(3, i, 'i')
UNIFORM_SCALARS(4, i, 'i')
UNIFORM_SCALARS(1, ui, 'u')
UNIFORM_SCALARS(2, ui, 'u')
UNIFORM_SCALARS(3, ui, 'u')
UNIFORM_SCALARS(4, ui, 'u')
#undef UNIFORM_SCALARS
#define UNIFORM_MATRIX(name, n)                                                                                        \
    static void g_uniform_matrix##name(CPU* c, Gles2Context* gl) {                                                     \
        uniform_write(gl, (int32_t)X(0), (int)X(1), PTR(3), n, 'f', (X(2) & 0xff) != 0);                               \
    }
UNIFORM_MATRIX(2, 4)
UNIFORM_MATRIX(3, 9)
UNIFORM_MATRIX(4, 16)
UNIFORM_MATRIX(2x3, 6)
UNIFORM_MATRIX(3x2, 6)
UNIFORM_MATRIX(2x4, 8)
UNIFORM_MATRIX(4x2, 8)
UNIFORM_MATRIX(3x4, 12)
UNIFORM_MATRIX(4x3, 12)
#undef UNIFORM_MATRIX

/* glGetUniform*v(program, location, params): the value as the first stage that has it holds it */
static void uniform_read(CPU* c, Gles2Context* gl, char kind) {
    G2Program* p = gles2_program((unsigned)X(0));
    int32_t location = (int32_t)X(1);
    if (!p || !p->linked || location < 0 || location >= (int32_t)p->location_count) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    int32_t e = p->locations[location];
    const G2Uniform* u = &p->uniforms[(uint32_t)e >> 16];
    uint32_t element = (uint32_t)e & 0xffff;
    unsigned char* out = PTR(2);
    if (u->sampler >= 0) {
        int32_t unit = p->units[u->sampler + (int32_t)element];
        if (kind == 'f') {
            float f = (float)unit;
            memcpy(out, &f, 4);
        } else {
            memcpy(out, &unit, 4);
        }
        return;
    }
    int columns;
    int n = components(u->type, &columns), rows = n / columns;
    for (int s = 0; s < 3; ++s) {
        if (u->offset[s] < 0) continue;
        size_t base = (size_t)u->offset[s] + element * u->array_stride;
        bool fl = u->type == GL_FLOAT_ || (u->type >= 0x8b50 && u->type <= 0x8b52) || columns > 1;
        for (int col = 0; col < columns; ++col)
            for (int row = 0; row < rows; ++row) {
                uint32_t bits;
                memcpy(&bits, p->defaults[s] + base + (size_t)col * (columns > 1 ? u->matrix_stride : 0) + 4 * (size_t)row,
                       4);
                float f;
                memcpy(&f, &bits, 4);
                if (kind == 'f' && !fl) f = u->type == GL_UNSIGNED_INT_ ? (float)bits : (float)(int32_t)bits;
                if (kind != 'f' && fl) bits = (uint32_t)(int32_t)f;
                memcpy(out + 4 * (col * rows + row), kind == 'f' ? (void*)&f : (void*)&bits, 4);
            }
        return;
    }
}
static void g_get_uniformfv(CPU* c, Gles2Context* gl) {
    uniform_read(c, gl, 'f');
}
static void g_get_uniformiv(CPU* c, Gles2Context* gl) {
    uniform_read(c, gl, 'i');
}

bool gles2_program_defaults(GlesSlot* slot, G2Program* p) {
    uint64_t pending = gles_vk_pending();
    VkDeviceSize align = V.props.limits.minUniformBufferOffsetAlignment;
    if (align < 16) align = 16;
    for (int s = 0; s < 3; ++s) {
        if (!p->default_size[s] || (!p->default_dirty[s] && p->default_serial[s] == pending)) continue;
        VkDeviceSize size = (p->default_size[s] + 15) & ~(VkDeviceSize)15;
        VkDeviceSize at = gles_vk_arena_take(slot, &slot->vertices, size, align, 4u << 20, 0);
        if (at == VK_WHOLE_SIZE) return false;
        memcpy(slot->vertices.map + at, p->defaults[s], p->default_size[s]);
        p->default_ref[s] = (G2Ref){slot->vertices.buffer, at};
        p->default_serial[s] = pending;
        p->default_dirty[s] = false;
    }
    return true;
}

/* ---- the queries of programs ---- */

static G2Program* program_arg(Gles2Context* gl, uint64_t name) {
    G2Program* p = gles2_program((unsigned)name);
    if (!p) gles2_error(gl, GL_INVALID_VALUE_);
    return p;
}
static uint32_t longest(const G2Program* p, bool uniforms) {
    uint32_t n = 0, count = uniforms ? p->uniform_count : p->input_count;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t len = (uint32_t)strlen(p->names + (uniforms ? p->uniforms[i].name : p->inputs[i].name)) + 1;
        if (len > n) n = len;
    }
    return n;
}
static void g_get_programiv(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    int32_t v = 0;
    if (!p) return;
    switch ((unsigned)X(1)) {
    case 0x8b80: v = p->deleted; break;
    case 0x8b82: v = p->linked; break;
    case 0x8b83: v = p->linked; break;
    case 0x8b84: v = p->log && p->log[0] ? (int32_t)strlen(p->log) + 1 : 0; break;
    case 0x8b85: v = (p->shaders[0] != 0) + (p->shaders[1] != 0) + (p->shaders[2] != 0); break;
    case 0x8b86: v = (int32_t)p->uniform_count; break;
    case 0x8b87: v = (int32_t)longest(p, true); break;
    case 0x8b89: v = (int32_t)p->input_count; break;
    case 0x8b8a: v = (int32_t)longest(p, false); break;
    case 0x8a36: v = (int32_t)p->block_count; break;
    case 0x8a35:
        for (uint32_t i = 0; i < p->binding_count; ++i)
            if (p->bindings[i].kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && p->bindings[i].name)
                if ((int32_t)strlen(p->names + p->bindings[i].name) + 1 > v) v = (int32_t)strlen(p->names + p->bindings[i].name) + 1;
        break;
    case 0x8c7f: v = 0x8c8c; break; /* GL_INTERLEAVED_ATTRIBS */
    case 0x8c83:
    case 0x8c76:
    case 0x8741: v = 0; break;
    case 0x8257: v = 0; break;
    case 0x8267: /* GL_COMPUTE_WORK_GROUP_SIZE */
        for (int i = 0; i < 3; ++i) memcpy((unsigned char*)PTR(2) + 4 * i, &p->local_size[i], 4);
        return;
    default: gles2_error(gl, GL_INVALID_ENUM_); return;
    }
    memcpy(PTR(2), &v, 4);
}
static void g_get_program_info_log(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    if (p) string_out(p->log, (int32_t)X(1), PTR(2), PTR(3));
}
/* glGetActiveUniform and glGetActiveAttrib(program, index, size, length, count, type, name) */
static void active(CPU* c, Gles2Context* gl, bool uniforms) {
    G2Program* p = program_arg(gl, X(0));
    uint32_t index = (uint32_t)X(1);
    if (!p) return;
    if (index >= (uniforms ? p->uniform_count : p->input_count)) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    uint32_t name = uniforms ? p->uniforms[index].name : p->inputs[index].name;
    int32_t count = uniforms ? p->uniforms[index].count : p->inputs[index].count;
    uint32_t type = uniforms ? p->uniforms[index].type : p->inputs[index].type;
    string_out(p->names + name, (int32_t)X(2), PTR(3), PTR(6));
    if (X(4)) memcpy(PTR(4), &count, 4);
    if (X(5)) memcpy(PTR(5), &type, 4);
}
static void g_get_active_uniform(CPU* c, Gles2Context* gl) {
    active(c, gl, true);
}
static void g_get_active_attrib(CPU* c, Gles2Context* gl) {
    active(c, gl, false);
}
static void g_get_attrib_location(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    const char* name = STR(1);
    int32_t loc = -1;
    if (p && p->linked && name)
        for (uint32_t i = 0; i < p->input_count; ++i)
            if (!strcmp(p->names + p->inputs[i].name, name)) loc = p->inputs[i].location;
    if (p && !p->linked) gles2_error(gl, GL_INVALID_OPERATION_);
    X(0) = (uint64_t)(int64_t)loc;
}
static void g_get_frag_data_location(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    const char* name = STR(1);
    int32_t loc = -1;
    if (p && p->linked && name)
        for (uint32_t i = 0; i < p->output_count; ++i)
            if (!strcmp(p->names + p->outputs[i].name, name)) loc = p->outputs[i].spirv;
    X(0) = (uint64_t)(int64_t)loc;
}
/* glGetUniformLocation(program, name): "a", "a[0]" and "a[i]" of an array, "s.m", "s[i].m" */
static void g_get_uniform_location(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    const char* name = STR(1);
    X(0) = (uint64_t)(int64_t)-1;
    if (!p || !name) return;
    if (!p->linked) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    size_t len = strlen(name);
    uint32_t element = 0;
    size_t base = len;
    if (len > 3 && name[len - 1] == ']') {
        const char* open = strrchr(name, '[');
        if (open && open[1] >= '0' && open[1] <= '9') {
            element = (uint32_t)strtoul(open + 1, NULL, 10);
            base = (size_t)(open - name);
        }
    }
    for (uint32_t i = 0; i < p->uniform_count; ++i) {
        const G2Uniform* u = &p->uniforms[i];
        const char* n = p->names + u->name;
        if (u->location < 0) continue;
        if (!strcmp(n, name)) {
            X(0) = (uint64_t)(int64_t)u->location;
            return;
        }
        /* an array's "a[0]": its "a" and "a[i]" */
        size_t nl = strlen(n);
        if (nl == base + 3 && !memcmp(n, name, base) && !strcmp(n + base, "[0]") && element < u->count) {
            X(0) = (uint64_t)(int64_t)(u->location + (int32_t)element);
            return;
        }
    }
}
/* the program's uniform blocks: their bindings, in order */
static const G2Binding* uniform_block(const G2Program* p, uint32_t index) {
    for (uint32_t i = 0; i < p->binding_count; ++i)
        if (p->bindings[i].kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && p->bindings[i].name &&
            p->bindings[i].first == index)
            return &p->bindings[i];
    return NULL;
}
static void g_get_uniform_block_index(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    const char* name = STR(1);
    X(0) = 0xffffffffu; /* GL_INVALID_INDEX */
    if (!p || !name) return;
    for (uint32_t i = 0; i < p->binding_count; ++i)
        if (p->bindings[i].kind == SNX_GLSLC_REFLECT_UNIFORM_BLOCK && p->bindings[i].name &&
            !strcmp(p->names + p->bindings[i].name, name))
            X(0) = p->bindings[i].first;
}
static void g_uniform_block_binding(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    uint32_t index = (uint32_t)X(1), binding = (uint32_t)X(2);
    if (!p) return;
    if (index >= p->block_count || index >= G2_UBO_BINDINGS || binding >= G2_UBO_BINDINGS) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    p->block_binding[index] = binding;
}
static void g_get_active_uniform_blockiv(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    uint32_t index = (uint32_t)X(1);
    if (!p) return;
    const G2Binding* b = uniform_block(p, index);
    if (!b) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    unsigned char* out = PTR(3);
    int32_t v = 0, n = 0;
    switch ((unsigned)X(2)) {
    case 0x8a3f: v = (int32_t)p->block_binding[index]; break;
    case 0x8a40: v = (int32_t)b->size; break;
    case 0x8a41: v = (int32_t)strlen(p->names + b->name) + 1; break;
    case 0x8a42:
        for (uint32_t i = 0; i < p->uniform_count; ++i) v += p->uniforms[i].block == (int16_t)index;
        break;
    case 0x8a43:
        for (uint32_t i = 0; i < p->uniform_count; ++i)
            if (p->uniforms[i].block == (int16_t)index) memcpy(out + 4 * n++, &i, 4);
        return;
    case 0x8a44:
    case 0x8a46: v = 1; break;
    default: gles2_error(gl, GL_INVALID_ENUM_); return;
    }
    memcpy(out, &v, 4);
}
static void g_get_active_uniform_block_name(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    if (!p) return;
    const G2Binding* b = uniform_block(p, (uint32_t)X(1));
    if (!b) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    string_out(p->names + b->name, (int32_t)X(2), PTR(3), PTR(4));
}
/* glGetUniformIndices(program, count, names, indices): a block's member by its name, or by "Block.member" */
static void g_get_uniform_indices(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    int32_t count = (int32_t)X(1);
    if (!p) return;
    for (int32_t k = 0; k < count; ++k) {
        uint64_t ptr;
        memcpy(&ptr, (unsigned char*)PTR(2) + 8 * k, 8);
        const char* name = (const char*)(uintptr_t)ptr;
        uint32_t found = 0xffffffffu;
        for (uint32_t i = 0; i < p->uniform_count && name; ++i) {
            const char* n = p->names + p->uniforms[i].name;
            const char* dot = strchr(name, '.');
            if (!strcmp(n, name) || (p->uniforms[i].block >= 0 && dot && !strcmp(n, dot + 1))) found = i;
            size_t nl = strlen(n);
            if (found == 0xffffffffu && nl > 3 && !strcmp(n + nl - 3, "[0]") && strlen(name) == nl - 3 &&
                !memcmp(n, name, nl - 3))
                found = i;
        }
        memcpy((unsigned char*)PTR(3) + 4 * k, &found, 4);
    }
}
static void g_get_active_uniformsiv(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    int32_t count = (int32_t)X(1);
    unsigned pname = (unsigned)X(3);
    if (!p) return;
    for (int32_t k = 0; k < count; ++k) {
        uint32_t i;
        memcpy(&i, (unsigned char*)PTR(2) + 4 * k, 4);
        if (i >= p->uniform_count) {
            gles2_error(gl, GL_INVALID_VALUE_);
            return;
        }
        const G2Uniform* u = &p->uniforms[i];
        int32_t v = 0, columns;
        switch (pname) {
        case 0x8a37: v = (int32_t)u->type; break;
        case 0x8a38: v = u->count; break;
        case 0x8a39: v = (int32_t)strlen(p->names + u->name) + 1; break;
        case 0x8a3a: v = u->block; break;
        case 0x8a3b: v = u->block >= 0 ? (u->offset[0] >= 0 ? u->offset[0] : u->offset[1]) : -1; break;
        case 0x8a3c: v = u->block >= 0 ? (u->count > 1 || u->array_stride ? u->array_stride : 0) : -1; break;
        case 0x8a3d: components(u->type, &columns), v = u->block >= 0 && columns > 1 ? u->matrix_stride : u->block >= 0 ? 0 : -1; break;
        case 0x8a3e: v = 0; break;
        default: gles2_error(gl, GL_INVALID_ENUM_); return;
        }
        memcpy((unsigned char*)PTR(4) + 4 * k, &v, 4);
    }
}
static void g_program_binary(CPU* c, Gles2Context* gl) {
    G2Program* p = program_arg(gl, X(0));
    if (p) link_fail(p, NULL, "no program binary formats");
    (void)c;
}
static void g_get_program_binary(CPU* c, Gles2Context* gl) {
    int32_t zero = 0;
    if (X(2)) memcpy(PTR(2), &zero, 4);
    gles2_error(gl, GL_INVALID_OPERATION_);
}
static void g_program_nothing(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl;
}
static void g_transform_feedback_varyings(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl;
    gles2_warn(7, "transform feedback is not drawn yet");
}

const HleSymbol hle_gles2_program[] = {
    FN("glCreateShader", g_create_shader),
    FN("glDeleteShader", g_delete_shader),
    FN("glIsShader", g_is_shader),
    FN("glShaderSource", g_shader_source),
    FN("glCompileShader", g_compile_shader),
    FN("glGetShaderiv", g_get_shaderiv),
    FN("glGetShaderInfoLog", g_get_shader_info_log),
    FN("glGetShaderSource", g_get_shader_source),
    FN("glGetShaderPrecisionFormat", g_get_shader_precision_format),
    FN("glShaderBinary", g_shader_binary),
    FN("glCreateProgram", g_create_program),
    FN("glDeleteProgram", g_delete_program),
    FN("glIsProgram", g_is_program),
    FN("glAttachShader", g_attach_shader),
    FN("glDetachShader", g_detach_shader),
    FN("glGetAttachedShaders", g_get_attached_shaders),
    FN("glBindAttribLocation", g_bind_attrib_location),
    FN("glLinkProgram", g_link_program),
    FN("glUseProgram", g_use_program),
    FN("glValidateProgram", g_validate_program),
    FN("glGetProgramiv", g_get_programiv),
    FN("glGetProgramInfoLog", g_get_program_info_log),
    FN("glGetActiveUniform", g_get_active_uniform),
    FN("glGetActiveAttrib", g_get_active_attrib),
    FN("glGetAttribLocation", g_get_attrib_location),
    FN("glGetFragDataLocation", g_get_frag_data_location),
    FN("glGetUniformLocation", g_get_uniform_location),
    FN("glGetUniformfv", g_get_uniformfv),
    FN("glGetUniformiv", g_get_uniformiv),
    FN("glGetUniformuiv", g_get_uniformiv),
    FN("glGetUniformBlockIndex", g_get_uniform_block_index),
    FN("glUniformBlockBinding", g_uniform_block_binding),
    FN("glGetActiveUniformBlockiv", g_get_active_uniform_blockiv),
    FN("glGetActiveUniformBlockName", g_get_active_uniform_block_name),
    FN("glGetUniformIndices", g_get_uniform_indices),
    FN("glGetActiveUniformsiv", g_get_active_uniformsiv),
    FN("glProgramBinary", g_program_binary),
    FN("glProgramBinaryOES", g_program_binary),
    FN("glGetProgramBinary", g_get_program_binary),
    FN("glGetProgramBinaryOES", g_get_program_binary),
    FN("glProgramParameteri", g_program_nothing),
    FN("glTransformFeedbackVaryings", g_transform_feedback_varyings),
    FN("glGetTransformFeedbackVarying", g_program_nothing),
    FN("glUniform1f", g_uniform1f),
    FN("glUniform2f", g_uniform2f),
    FN("glUniform3f", g_uniform3f),
    FN("glUniform4f", g_uniform4f),
    FN("glUniform1fv", g_uniform1fv),
    FN("glUniform2fv", g_uniform2fv),
    FN("glUniform3fv", g_uniform3fv),
    FN("glUniform4fv", g_uniform4fv),
    FN("glUniform1i", g_uniform1i),
    FN("glUniform2i", g_uniform2i),
    FN("glUniform3i", g_uniform3i),
    FN("glUniform4i", g_uniform4i),
    FN("glUniform1iv", g_uniform1iv),
    FN("glUniform2iv", g_uniform2iv),
    FN("glUniform3iv", g_uniform3iv),
    FN("glUniform4iv", g_uniform4iv),
    FN("glUniform1ui", g_uniform1ui),
    FN("glUniform2ui", g_uniform2ui),
    FN("glUniform3ui", g_uniform3ui),
    FN("glUniform4ui", g_uniform4ui),
    FN("glUniform1uiv", g_uniform1uiv),
    FN("glUniform2uiv", g_uniform2uiv),
    FN("glUniform3uiv", g_uniform3uiv),
    FN("glUniform4uiv", g_uniform4uiv),
    FN("glUniformMatrix2fv", g_uniform_matrix2),
    FN("glUniformMatrix3fv", g_uniform_matrix3),
    FN("glUniformMatrix4fv", g_uniform_matrix4),
    FN("glUniformMatrix2x3fv", g_uniform_matrix2x3),
    FN("glUniformMatrix3x2fv", g_uniform_matrix3x2),
    FN("glUniformMatrix2x4fv", g_uniform_matrix2x4),
    FN("glUniformMatrix4x2fv", g_uniform_matrix4x2),
    FN("glUniformMatrix3x4fv", g_uniform_matrix3x4),
    FN("glUniformMatrix4x3fv", g_uniform_matrix4x3),
    {"", NULL, NULL},
};
