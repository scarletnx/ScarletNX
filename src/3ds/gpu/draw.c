/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's draws in software: vertex loading, the vertex shader, the geometry pipeline and shader, and primitive
   assembly, each triangle going to the caller (raster.c's back end in the software renderer). Ported from Azahar
   video_core/pica/pica_core.cpp (DrawArrays, LoadVertices, DrawImmediate), vertex_loader.cpp, geometry_pipeline.cpp,
   shader_unit.cpp (GeometryEmitter), primitive_assembly.cpp and output_vertex.cpp (GPL-2.0-or-later, Copyright
   2014-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e. */
#include "3ds/gpu/pica.h"
#include "3ds/gpu/shader.h"
#include "common/utilities/endian.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* pipeline registers (PipelineRegs) */
enum {
    ATTR_BASE = 0x200,      /* base_address: bits 1..28, times 16 */
    ATTR_FORMAT_LO = 0x201, /* attributes 0-7: format (2 bits), elements - 1 (2 bits) */
    ATTR_FORMAT_HI = 0x202, /* 8-11, then the default-attribute mask (bits 16..27), max_attribute_index (28..31) */
    ATTR_LOADERS = 0x203,   /* 12 loaders of 3 words: data_offset; components 0-7; 8-11, byte_count, count */
    INDEX_ARRAY = 0x227,    /* offset (bits 0..27) from the base, format bit 31: 16-bit indices */
    NUM_VERTICES = 0x228,
    VERTEX_OFFSET = 0x22a,
    VS_OUTPUTS = 0x24a,      /* vs_outmap_total_minus_1_a */
    VS_OUTPUTS_B = 0x251,    /* vs_outmap_total_minus_1_b */
    GS_CONFIG = 0x252,       /* mode (bits 0..7), vertices - 1 (8..11), stride - 1 (12..15), first uniform (16..23) */
    GS_MAIN_VERTICES = 0x254 /* variable_vertex_main_num_minus_1 */
};
enum { FMT_BYTE, FMT_UBYTE, FMT_SHORT, FMT_FLOAT };
/* the geometry pipeline Reconfigure set up: none, or GSMode + 1 */
enum { GS_NONE, GS_POINT, GS_VARIABLE, GS_FIXED };
enum { UNIT_GS = 0x08 }; /* ShaderMode::GS, in bits 24..31 of a unit's input buffer configuration */

/* OutputVertex(regs, output): the vertex shader's output registers through the output map, colours clamped to
   |c| <= 1 */
static void output_vertex(const uint32_t* regs, const float (*output)[4], SnxCtrVertex* vertex) {
    float slots[32];
    unsigned total = regs[0x4f] & 7;
    for (unsigned i = 0; i < 32; ++i) slots[i] = 1.0f;
    for (unsigned attr = 0; attr < total; ++attr) {
        uint32_t map = regs[0x50 + attr];
        for (unsigned c = 0; c < 4; ++c) slots[(map >> (8 * c)) & 31] = output[attr][c];
    }
    memcpy(vertex, slots, sizeof *vertex);
    for (unsigned i = 0; i < 4; ++i) {
        float c = fabsf(vertex->color[i]);
        vertex->color[i] = c < 1.0f ? c : 1.0f;
    }
}

/* VertexLoader: where each of the 12 array attributes is, set up from the loaders once per draw */
typedef struct {
    uint32_t source[16], stride[16], format[16], elements[16];
    bool fixed[16]; /* IsDefaultAttribute: from the default attributes rather than an array */
    unsigned total;
} Loader;
static unsigned element_bytes(unsigned format) { return format == FMT_FLOAT ? 4 : format == FMT_SHORT ? 2 : 1; }
static void loader_setup(const uint32_t* regs, Loader* l) {
    uint32_t lo = regs[ATTR_FORMAT_LO], hi = regs[ATTR_FORMAT_HI];
    unsigned format[12], elements[12];
    *l = (Loader){0};
    for (unsigned i = 0; i < 16; ++i) l->source[i] = 0xdeadbeefu;
    for (unsigned n = 0; n < 12; ++n) {
        uint32_t word = n < 8 ? lo : hi, at = (n % 8) * 4;
        format[n] = (word >> at) & 3;
        elements[n] = ((word >> (at + 2)) & 3) + 1;
    }
    l->total = (hi >> 28) + 1;
    for (unsigned i = 0; i < 16; ++i) l->fixed[i] = i >= 12 || ((hi >> 16) & 0xfff & (1u << i));
    for (unsigned n = 0; n < 12; ++n) {
        const uint32_t* config = &regs[ATTR_LOADERS + n * 3];
        uint64_t components = config[1] | ((uint64_t)(config[2] & 0xffff) << 32);
        uint32_t offset = 0, count = config[2] >> 28;
        for (unsigned c = 0; c < count; ++c) {
            if (c >= 12) continue; /* an overflowing loader: Azahar logs it and skips the component */
            unsigned attr = (components >> (c * 4)) & 15;
            if (attr < 12) {
                uint32_t align = element_bytes(format[attr]);
                offset = (offset + align - 1) / align * align;
                l->source[attr] = (config[0] & 0x0fffffff) + offset;
                l->stride[attr] = (config[2] >> 16) & 0xff;
                l->format[attr] = format[attr];
                l->elements[attr] = elements[attr];
                offset += elements[attr] * align;
            } else { /* 12-15: padding of 1 to 4 words */
                offset = (offset + 3) / 4 * 4;
                offset += (attr - 11) * 4;
            }
        }
    }
}
static void load_vertex(const SnxCtrPica* p, SnxCtrMemory* m, const Loader* l, uint32_t base, uint32_t vertex,
                        float (*input)[4]) {
    for (unsigned i = 0; i < l->total; ++i) {
        unsigned char bytes[16] = {0};
        if (l->fixed[i]) {
            memcpy(input[i], p->default_attributes[i], 16);
            continue;
        }
        if (!l->elements[i]) continue; /* "vertex retention", unimplemented in Azahar too */
        uint32_t size = l->elements[i] * element_bytes(l->format[i]);
        uint32_t at = base + l->source[i] + l->stride[i] * vertex;
        /* an attribute outside memory reads as zeros (Azahar would read a null host pointer) */
        (void)snx_ctr_phys_read(m, at, bytes, size);
        for (unsigned c = 0; c < l->elements[i]; ++c) {
            switch (l->format[i]) {
                case FMT_BYTE: input[i][c] = (float)(int8_t)bytes[c]; break;
                case FMT_UBYTE: input[i][c] = (float)bytes[c]; break;
                case FMT_SHORT: input[i][c] = (float)(int16_t)snx_ld_le16(bytes + c * 2); break;
                default: {
                    uint32_t w = snx_ld_le32(bytes + c * 4);
                    memcpy(&input[i][c], &w, 4);
                }
            }
        }
        for (unsigned c = l->elements[i]; c < 4; ++c) input[i][c] = c == 3 ? 1.0f : 0.0f;
    }
}

/* where a draw's triangles go, and the PICA it runs on */
typedef struct {
    SnxCtrPica* pica;
    SnxCtrTriangle triangle;
    void* context;
} Sink;

/* PrimitiveAssembler::SubmitVertex: lists (and the geometry shader's topology) every three vertices, strips and
   fans from the third on */
static void assemble(Sink* s, const SnxCtrVertex* v) {
    SnxCtrAssembler* a = &s->pica->assembler;
    switch (a->topology) {
        case 0: /* List */
        case 3: /* Shader */
            if (a->index < 2) {
                a->buffer[a->index++] = *v;
                break;
            }
            a->index = 0;
            if (a->topology == 3 && a->winding) {
                s->triangle(s->context, &a->buffer[1], &a->buffer[0], v);
                a->winding = false;
            } else
                s->triangle(s->context, &a->buffer[0], &a->buffer[1], v);
            break;
        default: /* Strip, Fan */
            if (a->strip_ready) s->triangle(s->context, &a->buffer[0], &a->buffer[1], v);
            a->buffer[a->index] = *v;
            a->strip_ready |= a->index == 1;
            a->index = a->topology == 1 ? !a->index : 1;
            break;
    }
}
static void submit(Sink* s, const float (*output)[4]) {
    SnxCtrVertex v;
    output_vertex(s->pica->regs, output, &v);
    assemble(s, &v);
}

/* GeometryPipeline::Reconfigure: the geometry pipeline of what is drawn next, its inputs emptied. NULL when it is one
   Azahar runs, else a sentence naming what it is not */
static const char* geometry_reconfigure(SnxCtrPica* p) {
    const uint32_t* r = p->regs;
    SnxCtrGeometry* g = &p->geometry;
    uint32_t use = r[SNX_PICA_USE_GS], input = r[SNX_PICA_GS + SNX_PICA_SH_INPUT_CONFIG], config = r[GS_CONFIG];
    uint32_t outputs = (r[VS_OUTPUTS] & 15) + 1, inputs = (input & 15) + 1, to_uniform = input >> 8 & 0xff;
    uint32_t mode = (config & 0xff) + 1;
    bool variable = use >> 31;
    g->mode = GS_NONE;
    g->at = 0;
    g->need_index = 0;
    if (!(use & 3)) return NULL;
    if ((use & 3) != 2) return "a geometry shader setting the PICA does not have";
    if (input >> 24 != UNIT_GS) return "a geometry shader on a unit not set up for one";
    if ((r[VS_OUTPUTS_B] & 15) + 1 != outputs) return "a geometry shader given two vertex output counts";
    switch (mode) {
        case GS_POINT:
            if (variable || to_uniform != 0) return "a point mode geometry shader set up inconsistently";
            if (inputs % outputs) return "a point mode geometry shader whose inputs are not whole vertices";
            g->end = (uint16_t)inputs;
            break;
        case GS_VARIABLE:
            if (!variable || to_uniform != 1) return "a variable primitive geometry shader set up inconsistently";
            g->need_index = 1;
            break;
        case GS_FIXED:
            if (variable || to_uniform != 1 || outputs != (config >> 12 & 15) + 1)
                return "a fixed primitive geometry shader set up inconsistently";
            g->begin = (uint16_t)(config >> 16 & 0xff);
            g->end = (uint16_t)(g->begin + outputs * ((config >> 8 & 15) + 1));
            g->at = g->begin;
            break;
        default: return "a geometry shader mode the PICA does not have";
    }
    g->mode = (uint8_t)mode;
    g->outputs = (uint8_t)outputs;
    return NULL;
}
/* GeometryPipeline_VariablePrimitive::SubmitIndex: the first index of a batch is its vertex count, put in c0, and its
   vertices go to the uniforms from c1 on */
static void geometry_index(SnxCtrPica* p, uint32_t index) {
    SnxCtrGeometry* g = &p->geometry;
    for (unsigned c = 0; c < 4; ++c) p->gs.f[0][c] = (float)index;
    g->at = 1;
    g->main_left = p->regs[GS_MAIN_VERTICES] + 1;
    g->total_left = index;
    g->need_index = 0;
}
/* the next geometry shader input in the uniforms; one past c95 is dropped, as an upload past it is */
static void geometry_uniform(SnxCtrPica* p, const float v[4]) {
    SnxCtrGeometry* g = &p->geometry;
    if (g->at < 96) memcpy(p->gs.f[g->at], v, 16);
    ++g->at;
}
/* GeometryEmitter::Emit: the output registers the geometry shader's output mask enables, into the vertex SETEMIT
   names (there is no vertex 3: Azahar asserts it never comes). With the primitive flag, the three vertices go to the
   primitive assembler, after the winding flag */
static void emit(void* context, unsigned vertex_id, bool prim_emit, bool winding, float (*output)[4]) {
    Sink* s = context;
    SnxCtrGeometry* g = &s->pica->geometry;
    uint32_t mask = s->pica->regs[SNX_PICA_GS + SNX_PICA_SH_OUTPUT_MASK] & 0xffff;
    unsigned n = 0;
    if (vertex_id < 3)
        for (unsigned reg = 0; reg < 16; ++reg)
            if (mask >> reg & 1) memcpy(g->emitted[vertex_id][n++], output[reg], 16);
    if (!prim_emit) return;
    if (winding) s->pica->assembler.winding = true;
    for (unsigned i = 0; i < 3; ++i) submit(s, (const float (*)[4])g->emitted[i]);
}
/* GeometryPipeline::SubmitVertex: a vertex shader's outputs go to the primitive assembler, or to the geometry shader's
   inputs, which run it once they are complete. b15 is set after each run, so a program that clears it knows its
   first run of a batch */
static void geometry_vertex(Sink* s, const float (*output)[4]) {
    SnxCtrPica* p = s->pica;
    SnxCtrGeometry* g = &p->geometry;
    bool run;
    switch (g->mode) {
        case GS_NONE: submit(s, output); return;
        case GS_POINT:
            for (unsigned i = 0; i < g->outputs; ++i) memcpy(g->inputs[g->at++], output[i], 16);
            run = g->at == g->end;
            if (run) {
                g->at = 0;
                snx_ctr_shader_load(p->regs, SNX_PICA_GS, &g->unit, (const float (*)[4])g->inputs);
            }
            break;
        case GS_VARIABLE: {
            /* main vertices give every output, the others their first (usually the position) */
            unsigned count = g->main_left ? g->outputs : 1u;
            for (unsigned i = 0; i < count; ++i) geometry_uniform(p, output[i]);
            if (g->main_left) --g->main_left;
            run = --g->total_left == 0;
            g->need_index = run;
            break;
        }
        default: /* GS_FIXED */
            for (unsigned i = 0; i < g->outputs; ++i) geometry_uniform(p, output[i]);
            run = g->at == g->end;
            if (run) g->at = g->begin;
            break;
    }
    if (!run) return;
    SnxCtrEmitter e = {s, emit, g->vertex_id, g->prim_emit, g->winding};
    snx_ctr_shader_run(&p->gs, &g->unit, p->regs[SNX_PICA_GS + SNX_PICA_SH_MAIN_OFFSET] & 0xffff, &e);
    g->vertex_id = e.vertex_id & 3;
    g->prim_emit = e.prim_emit;
    g->winding = e.winding;
    p->gs.b |= 1u << 15;
}

SnxCtrPicaResult snx_ctr_pica_triangles(SnxCtrPica* p, SnxCtrMemory* m, bool indexed, SnxCtrTriangle triangle,
                                        void* context) {
    enum { CACHE = 64 }; /* Azahar's circular-replacement vertex cache for indexed draws */
    Loader l;
    loader_setup(p->regs, &l);
    uint32_t base = ((p->regs[ATTR_BASE] >> 1) & 0x0fffffff) * 16, index_info = p->regs[INDEX_ARRAY];
    uint32_t index_address = base + (index_info & 0x0fffffff);
    bool wide = index_info >> 31;
    unsigned char probe;
    /* an invalid base address draws nothing (Mario & Luigi: Superstar Saga sets one) */
    if (snx_ctr_phys_read(m, index_address, &probe, 1) != SNX_CTR_MEM_OK) return SNX_CTR_PICA_OK;
    const char* why = geometry_reconfigure(p);
    if (!why && p->geometry.need_index && !indexed)
        why = "a variable primitive geometry shader in a draw without indices";
    if (why) {
        p->stop_reason = why;
        return SNX_CTR_PICA_UNSUPPORTED;
    }
    SnxCtrShaderUnit* unit = calloc(1, sizeof *unit);
    struct {
        bool valid;
        uint16_t id;
        float output[16][4];
    }* cache = indexed ? calloc(CACHE, sizeof *cache) : NULL;
    if (!unit || (indexed && !cache)) {
        free(unit);
        free(cache);
        p->stop_reason = "the host is out of memory";
        return SNX_CTR_PICA_UNSUPPORTED;
    }
    Sink s = {p, triangle, context};
    float output[16][4] = {{0}};
    unsigned cache_at = 0, entry = p->regs[SNX_PICA_VS + SNX_PICA_SH_MAIN_OFFSET] & 0xffff;
    for (uint32_t i = 0; i < p->regs[NUM_VERTICES]; ++i) {
        uint32_t vertex = i + p->regs[VERTEX_OFFSET];
        bool hit = false;
        if (indexed) { /* indexed draws do not use the vertex offset */
            unsigned char b[2] = {0, 0};
            (void)snx_ctr_phys_read(m, index_address + i * (wide ? 2 : 1), b, wide ? 2 : 1);
            vertex = wide ? (uint32_t)(b[0] | (b[1] << 8)) : b[0];
            if (p->geometry.need_index) {
                geometry_index(p, vertex);
                continue;
            }
            for (unsigned c = 0; c < CACHE; ++c)
                if (cache[c].valid && cache[c].id == vertex) {
                    memcpy(output, cache[c].output, sizeof output);
                    hit = true;
                    break;
                }
        }
        if (!hit) {
            float input[16][4] = {{0}};
            load_vertex(p, m, &l, base, vertex, input);
            snx_ctr_shader_load(p->regs, SNX_PICA_VS, unit, (const float (*)[4])input);
            snx_ctr_shader_run(&p->vs, unit, entry, NULL);
            snx_ctr_shader_store(p->regs, SNX_PICA_VS, unit, output);
            if (indexed) {
                cache[cache_at].valid = true;
                cache[cache_at].id = (uint16_t)vertex;
                memcpy(cache[cache_at].output, output, sizeof output);
                cache_at = (cache_at + 1) % CACHE;
            }
        }
        geometry_vertex(&s, (const float (*)[4])output);
    }
    free(unit);
    free(cache);
    return SNX_CTR_PICA_OK;
}

SnxCtrPicaResult snx_ctr_pica_triangles_immediate(SnxCtrPica* p, SnxCtrTriangle triangle, void* context) {
    SnxCtrShaderUnit* unit = calloc(1, sizeof *unit);
    float output[16][4] = {{0}};
    if (!unit) {
        p->stop_reason = "the host is out of memory";
        return SNX_CTR_PICA_UNSUPPORTED;
    }
    snx_ctr_shader_load(p->regs, SNX_PICA_VS, unit, (const float (*)[4])p->immediate_vertex);
    snx_ctr_shader_run(&p->vs, unit, p->regs[SNX_PICA_VS + SNX_PICA_SH_MAIN_OFFSET] & 0xffff, NULL);
    snx_ctr_shader_store(p->regs, SNX_PICA_VS, unit, output);
    free(unit);
    /* the first vertex after a default attribute index write sets the geometry pipeline up again */
    const char* why = NULL;
    if (p->immediate_reset) {
        why = geometry_reconfigure(p);
        p->immediate_reset = false;
    }
    if (!why && p->geometry.need_index) why = "a variable primitive geometry shader in immediate mode";
    if (why) {
        p->stop_reason = why;
        return SNX_CTR_PICA_UNSUPPORTED;
    }
    Sink s = {p, triangle, context};
    geometry_vertex(&s, (const float (*)[4])output);
    return SNX_CTR_PICA_OK;
}
