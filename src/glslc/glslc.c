/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A compilation: the caller's workspace holds its every array (internal.h), sized by glslc.h's bounds and carved by
   offsets anew each time, so nothing is allocated and nothing lives in it between two compilations. An error
   longjmps back to glslc_run. */
#include "glslc/internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* the SPIR-V sections' capacities, in words: a section past its own says so */
#define WORDS_CAPS        64u
#define WORDS_IMPORTS     16u
#define WORDS_ENTRY       1100u
#define WORDS_MODES       64u
#define WORDS_NAMES       32768u
#define WORDS_ANNOTATIONS 32768u
#define WORDS_GLOBALS     65536u
#define WORDS_FUNCTIONS   SNX_GLSLC_MAX_WORDS
#define WORDS_LOCALS      16384u
#define WORDS_BODY        131072u

static size_t place(size_t* at, size_t count, size_t size) {
    size_t start = (*at + 15u) & ~(size_t)15u;
    *at = start + count * size;
    return start;
}

typedef struct {
    size_t synth, toks, macros, mtoks, exp, marks, nodes, types, members, syms, sym_hash, params, calls, walk;
    size_t type_ids, sym_ids, sym_types, sampler_ids, hash, stack;
    size_t caps, imports, entry, modes, names, annotations, globals, functions, locals, body;
    size_t total;
} Layout;

static void layout(Layout* l) {
    size_t at = sizeof(Glslc);
    l->synth = place(&at, SNX_GLSLC_MAX_SYNTH, 1);
    l->toks = place(&at, SNX_GLSLC_MAX_TOKENS, sizeof(GlslcToken));
    l->macros = place(&at, SNX_GLSLC_MAX_MACROS, sizeof(GlslcMacro));
    l->mtoks = place(&at, SNX_GLSLC_MAX_MACRO_TOKENS, sizeof(GlslcToken));
    l->exp = place(&at, SNX_GLSLC_MAX_EXPANSION, sizeof(GlslcToken));
    l->marks = place(&at, SNX_GLSLC_MAX_LINE_MARKS, sizeof(GlslcLineMark));
    l->nodes = place(&at, SNX_GLSLC_MAX_NODES, sizeof(GlslcNode));
    l->types = place(&at, SNX_GLSLC_MAX_TYPES, sizeof(GlslcType));
    l->members = place(&at, SNX_GLSLC_MAX_MEMBERS, sizeof(GlslcMember));
    l->syms = place(&at, SNX_GLSLC_MAX_SYMBOLS, sizeof(GlslcSymbol));
    l->sym_hash = place(&at, GLSLC_SYM_BUCKETS, sizeof(uint16_t));
    l->params = place(&at, SNX_GLSLC_MAX_PARAMS, sizeof(GlslcParam));
    l->calls = place(&at, SNX_GLSLC_MAX_CALLS, sizeof(GlslcCall));
    l->walk = place(&at, (size_t)SNX_GLSLC_MAX_SYMBOLS * 3u, sizeof(uint16_t));
    l->type_ids = place(&at, (size_t)SNX_GLSLC_MAX_TYPES * 3u, sizeof(uint32_t));
    l->sym_ids = place(&at, SNX_GLSLC_MAX_SYMBOLS, sizeof(uint32_t));
    l->sym_types = place(&at, SNX_GLSLC_MAX_SYMBOLS, sizeof(uint32_t));
    l->sampler_ids = place(&at, SNX_GLSLC_MAX_SYMBOLS, sizeof(uint32_t));
    l->hash = place(&at, GLSLC_HASH_SIZE, sizeof(GlslcHashEntry));
    l->stack = place(&at, GLSLC_STACK_WORDS, sizeof(uint32_t));
    l->caps = place(&at, WORDS_CAPS, sizeof(uint32_t));
    l->imports = place(&at, WORDS_IMPORTS, sizeof(uint32_t));
    l->entry = place(&at, WORDS_ENTRY, sizeof(uint32_t));
    l->modes = place(&at, WORDS_MODES, sizeof(uint32_t));
    l->names = place(&at, WORDS_NAMES, sizeof(uint32_t));
    l->annotations = place(&at, WORDS_ANNOTATIONS, sizeof(uint32_t));
    l->globals = place(&at, WORDS_GLOBALS, sizeof(uint32_t));
    l->functions = place(&at, WORDS_FUNCTIONS, sizeof(uint32_t));
    l->locals = place(&at, WORDS_LOCALS, sizeof(uint32_t));
    l->body = place(&at, WORDS_BODY, sizeof(uint32_t));
    l->total = at;
}

static void words_at(GlslcWords* w, char* block, size_t offset, uint32_t cap, const char* what) {
    w->w = (uint32_t*)(void*)(block + offset);
    w->n = 0;
    w->cap = cap;
    w->what = what;
}

size_t snx_glslc_workspace_size(void) {
    Layout l;
    layout(&l);
    return l.total;
}

/* 0 when memory can be a workspace; otherwise -1 and the error */
static int check_workspace(const void* memory, size_t size, GlslcResult* result) {
    size_t need = snx_glslc_workspace_size();
    if (!memory) {
        snprintf(result->error, sizeof result->error,
                 "the workspace's memory is NULL, and a compilation needs %zu bytes", need);
        return -1;
    }
    if ((uintptr_t)memory % 16u) {
        snprintf(
            result->error, sizeof result->error,
            "the workspace at %p is %u bytes past a multiple of 16: a workspace is 16-byte aligned, as malloc's memory is",
            memory, (unsigned)((uintptr_t)memory % 16u));
        return -1;
    }
    if (size < need) {
        snprintf(result->error, sizeof result->error,
                 "the workspace is %zu bytes, and a compilation needs %zu (snx_glslc_workspace_size)", size, need);
        return -1;
    }
    return 0;
}

int snx_glslc_workspace_init(GlslcWorkspace* workspace, void* memory, size_t size, GlslcResult* result) {
    memset(result, 0, sizeof *result);
    workspace->memory = NULL;
    workspace->size = 0;
    if (check_workspace(memory, size, result)) return -1;
    workspace->memory = memory;
    workspace->size = size;
    return 0;
}

Glslc* glslc_open(void* memory, size_t memory_size, GlslcStage stage, const char* source, size_t size,
                  GlslcResult* result) {
    if (check_workspace(memory, memory_size, result)) return NULL;
    if ((unsigned)stage > (unsigned)SNX_GLSLC_COMPUTE) {
        snprintf(result->error, sizeof result->error,
                 "stage %u is not one glslc compiles: vertex (0), fragment (1) or compute (2)", (unsigned)stage);
        return NULL;
    }
    if (size > SNX_GLSLC_MAX_SOURCE) {
        snprintf(result->error, sizeof result->error,
                 "the source is %zu bytes, more than the %u glslc takes (a token's place is 16 bits)", size,
                 SNX_GLSLC_MAX_SOURCE);
        return NULL;
    }
    if (!source && size) {
        snprintf(result->error, sizeof result->error, "the source is NULL with a size of %zu bytes", size);
        return NULL;
    }
    if (!source) source = "";
    Layout l;
    layout(&l);
    char* block = memory;
    /* what the passes read before writing: the counts, the hash heads, the emitter's ids */
    memset(block, 0, sizeof(Glslc));
    Glslc* g = (Glslc*)(void*)block;
    g->src = source;
    g->src_len = (uint32_t)size;
    g->stage = stage;
    g->smask = stage == SNX_GLSLC_VERTEX ? GLSLC_SV : stage == SNX_GLSLC_FRAGMENT ? GLSLC_SF : GLSLC_SC;
    g->vi = GLSLC_V100;
    g->version = 100;
    g->result = result;
    g->bol = 1;
    g->synth = block + l.synth;
    g->toks = (GlslcToken*)(void*)(block + l.toks);
    g->macros = (GlslcMacro*)(void*)(block + l.macros);
    g->nmacros = 1; /* 0 is no macro */
    g->mtoks = (GlslcToken*)(void*)(block + l.mtoks);
    g->exp = (GlslcToken*)(void*)(block + l.exp);
    g->marks = (GlslcLineMark*)(void*)(block + l.marks);
    g->nodes = (GlslcNode*)(void*)(block + l.nodes);
    memset(&g->nodes[0], 0, sizeof g->nodes[0]);
    g->nnodes = 1;
    g->types = (GlslcType*)(void*)(block + l.types);
    g->members = (GlslcMember*)(void*)(block + l.members);
    memset(&g->members[0], 0, sizeof g->members[0]);
    g->nmembers = 1;
    g->syms = (GlslcSymbol*)(void*)(block + l.syms);
    memset(&g->syms[0], 0, sizeof g->syms[0]);
    g->nsyms = 1;
    g->sym_hash = (uint16_t*)(void*)(block + l.sym_hash);
    memset(g->sym_hash, 0, GLSLC_SYM_BUCKETS * sizeof(uint16_t));
    g->params = (GlslcParam*)(void*)(block + l.params);
    g->calls = (GlslcCall*)(void*)(block + l.calls);
    g->walk = (uint16_t*)(void*)(block + l.walk);
    g->e.type_ids = (uint32_t*)(void*)(block + l.type_ids);
    memset(g->e.type_ids, 0, (size_t)SNX_GLSLC_MAX_TYPES * 3u * sizeof(uint32_t));
    g->e.sym_ids = (uint32_t*)(void*)(block + l.sym_ids);
    memset(g->e.sym_ids, 0, SNX_GLSLC_MAX_SYMBOLS * sizeof(uint32_t));
    g->e.sym_types = (uint32_t*)(void*)(block + l.sym_types);
    memset(g->e.sym_types, 0, SNX_GLSLC_MAX_SYMBOLS * sizeof(uint32_t));
    g->e.sampler_ids = (uint32_t*)(void*)(block + l.sampler_ids);
    memset(g->e.sampler_ids, 0, SNX_GLSLC_MAX_SYMBOLS * sizeof(uint32_t));
    g->e.hash = (GlslcHashEntry*)(void*)(block + l.hash);
    memset(g->e.hash, 0, GLSLC_HASH_SIZE * sizeof(GlslcHashEntry));
    g->e.stack = (uint32_t*)(void*)(block + l.stack);
    words_at(&g->e.caps, block, l.caps, WORDS_CAPS, "capabilities");
    words_at(&g->e.imports, block, l.imports, WORDS_IMPORTS, "imports");
    words_at(&g->e.entry, block, l.entry, WORDS_ENTRY, "entry point");
    words_at(&g->e.modes, block, l.modes, WORDS_MODES, "execution modes");
    words_at(&g->e.names, block, l.names, WORDS_NAMES, "names");
    words_at(&g->e.annotations, block, l.annotations, WORDS_ANNOTATIONS, "decorations");
    words_at(&g->e.globals, block, l.globals, WORDS_GLOBALS, "types, constants and global variables");
    words_at(&g->e.functions, block, l.functions, WORDS_FUNCTIONS, "functions");
    words_at(&g->e.locals, block, l.locals, WORDS_LOCALS, "one function's local variables");
    words_at(&g->e.body, block, l.body, WORDS_BODY, "one function's code");
    return g;
}

int glslc_run(Glslc* g, int last, uint32_t* words, size_t capacity) {
    if (setjmp(g->fail)) return -1;
    glslc_lex(g);
    g->result->version = g->version;
    if (last == GLSLC_PASS_LEX) return 0;
    glslc_parse(g);
    if (last == GLSLC_PASS_PARSE) return 0;
    glslc_emit(g, words, capacity);
    return 0;
}

int snx_glslc_compile(GlslcWorkspace* workspace, const GlslcOptions* options, const char* source, size_t size,
                      uint32_t* words, size_t capacity, GlslcResult* result) {
    memset(result, 0, sizeof *result);
    if (!options) {
        snprintf(result->error, sizeof result->error, "the options are NULL: they give the stage at least");
        return -1;
    }
    Glslc* g = glslc_open(workspace->memory, workspace->size, options->stage, source, size, result);
    if (!g) return -1;
    g->bind = options->bind;
    g->bind_user = options->bind_user;
    return glslc_run(g, GLSLC_PASS_EMIT, words, capacity);
}

/* a line ends at a line feed, or at a carriage return with no line feed after it */
static int line_end(const Glslc* g, uint32_t i) {
    return g->src[i] == '\n' || (g->src[i] == '\r' && (i + 1 >= g->src_len || g->src[i + 1] != '\n'));
}

void glslc_locate(const Glslc* g, uint32_t site, uint32_t* line, uint32_t* column) {
    uint32_t l = 1, start = 0, end = site < g->src_len ? site : g->src_len;
    for (uint32_t i = 0; i < end; i++) {
        if (line_end(g, i)) {
            l++;
            start = i + 1;
        }
    }
    for (uint32_t k = g->nmarks; k-- > 0;) {
        if (g->marks[k].pos <= site) {
            l = g->marks[k].line;
            for (uint32_t i = g->marks[k].pos; i < end; i++) l += line_end(g, i);
            break;
        }
    }
    *line = l;
    *column = site - start + 1;
}

void glslc_fail(Glslc* g, uint32_t site, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g->result->error, sizeof g->result->error, fmt, ap);
    va_end(ap);
    glslc_locate(g, site, &g->result->line, &g->result->column);
    longjmp(g->fail, 1);
}

void glslc_warn(Glslc* g, uint32_t site, const char* fmt, ...) {
    if (g->result->warnings++ == 0) {
        uint32_t line, column;
        char text[sizeof g->result->warning];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(text, sizeof text, fmt, ap);
        va_end(ap);
        glslc_locate(g, site, &line, &column);
        snprintf(g->result->warning, sizeof g->result->warning, "line %u, column %u: %s", line, column, text);
    }
    if (g->result->warnings == 0) g->result->warnings = 0xffffu; /* the count stops at its largest */
}

uint32_t glslc_hash(const char* text, uint32_t len) {
    uint32_t h = 2166136261u;
    for (uint32_t i = 0; i < len; i++) h = (h ^ (unsigned char)text[i]) * 16777619u;
    return h;
}

const char* glslc_view_text(const Glslc* g, GlslcView v) { return g->src + v.pos; }

const char* glslc_token_text(const Glslc* g, const GlslcToken* t) {
    return (t->flags & GLSLC_TF_SYNTH) ? g->synth + t->text.pos : g->src + t->text.pos;
}

int glslc_view_is(const Glslc* g, GlslcView v, const char* word) {
    size_t n = strlen(word);
    return v.len == n && !memcmp(g->src + v.pos, word, n);
}

int glslc_views_equal(const Glslc* g, GlslcView a, GlslcView b) {
    return a.len == b.len && !memcmp(g->src + a.pos, g->src + b.pos, a.len);
}
