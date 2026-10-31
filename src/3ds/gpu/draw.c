/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's draws in software: vertex loading, the vertex shader and primitive assembly, the triangles going to
   the back end (raster.c). Ported from Azahar video_core/pica/pica_core.cpp (DrawArrays, LoadVertices,
   DrawImmediate), vertex_loader.cpp, primitive_assembly.cpp and output_vertex.cpp (GPL-2.0-or-later, Copyright
   2014-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e, and geometry_pipeline.cpp. */

#include "3ds/gpu/pica.h"
#include "3ds/gpu/raster.h"
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
    VERTEX_OFFSET = 0x22a
};
enum { FMT_BYTE, FMT_UBYTE, FMT_SHORT, FMT_FLOAT };

void snx_ctr_output_vertex(const uint32_t* regs, const float (*output)[4], SnxCtrVertex* vertex) {
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

/* PrimitiveAssembler::SubmitVertex: lists (and the geometry shader's topology) every three vertices, strips and
   fans from the third on */
static void assemble(SnxCtrPica* p, SnxCtrMemory* m, const SnxCtrVertex* v) {
    SnxCtrAssembler* a = &p->assembler;
    switch (a->topology) {
        case 0: /* List */
        case 3: /* Shader */
            if (a->index < 2) {
                a->buffer[a->index++] = *v;
                break;
            }
            a->index = 0;
            if (a->topology == 3 && a->winding) {
                snx_ctr_raster_triangle(p, m, &a->buffer[1], &a->buffer[0], v);
                a->winding = false;
            } else
                snx_ctr_raster_triangle(p, m, &a->buffer[0], &a->buffer[1], v);
            break;
        default: /* Strip, Fan */
            if (a->strip_ready) snx_ctr_raster_triangle(p, m, &a->buffer[0], &a->buffer[1], v);
            a->buffer[a->index] = *v;
            a->strip_ready |= a->index == 1;
            a->index = a->topology == 1 ? !a->index : 1;
            break;
    }
}
static void submit(SnxCtrPica* p, SnxCtrMemory* m, const float (*output)[4]) {
    SnxCtrVertex v;
    snx_ctr_output_vertex(p->regs, output, &v);
    assemble(p, m, &v);
}

/* GeometryPipeline (Azahar video_core/pica/geometry_pipeline.cpp): the vertex shader's outputs gather into the
   geometry shader's inputs (its input registers in Point mode, float uniforms in the two primitive modes), the shader
   runs when a batch is complete, and what it EMITs goes to the primitive assembler three vertices at a time */
enum { PIPE_REGS_USE_GS_VARIABLE = 31, GS_VS_OUTMAP_A = 0x24a, GS_VS_OUTMAP_B = 0x251, GS_CONFIG = 0x252,
       GS_VARIABLE_MAIN = 0x254, GS_MODE_POINT = 0, GS_MODE_VARIABLE = 1, GS_MODE_FIXED = 2, GS_SHADER_MODE = 0x08 };
struct SnxCtrGsPipe {
    SnxCtrShaderUnit unit;
    SnxCtrEmitter emitter;
    SnxCtrPica* pica;
    SnxCtrMemory* memory;
    float emitted[3][16][4]; /* GeometryEmitter's buffer */
    float inputs[16][4];     /* Point mode's input registers' values */
    unsigned mode, vs_outputs, gs_inputs, start, filled, uniform_at, main_remaining, total_remaining, batch;
    bool need_index;
};
static bool gs_used(const SnxCtrPica* p) { return (p->regs[SNX_PICA_USE_GS] & 3) == 2; }
/* GeometryEmitter::Emit: the shader's output registers are the vertex SETEMIT numbered, and a primitive-completing
   EMIT sends the three of them on, the winding SETEMIT asked for set first */
static void gs_emit(void* context, unsigned vertex_id, bool prim_emit, bool winding, float (*output)[4]) {
    struct SnxCtrGsPipe* g = context;
    if (vertex_id >= 3) return;
    memcpy(g->emitted[vertex_id], output, sizeof g->emitted[vertex_id]);
    if (!prim_emit) return;
    if (winding) g->pica->assembler.winding = true;
    for (unsigned i = 0; i < 3; ++i) submit(g->pica, g->memory, (const float (*)[4])g->emitted[i]);
}
/* GeometryPipeline::Reconfigure: NULL for a configuration the pipeline takes, else what it does not */
static const char* gs_reconfigure(SnxCtrPica* p) {
    const uint32_t* r = p->regs;
    if (!p->gs_pipe) {
        p->gs_pipe = calloc(1, sizeof *p->gs_pipe);
        if (!p->gs_pipe) return "the host is out of memory";
        p->gs_pipe->emitter = (SnxCtrEmitter){.context = p->gs_pipe, .emit = gs_emit};
    }
    struct SnxCtrGsPipe* g = p->gs_pipe;
    uint32_t input = r[SNX_PICA_GS + SNX_PICA_SH_INPUT_CONFIG], config = r[GS_CONFIG];
    bool variable = r[SNX_PICA_USE_GS] >> PIPE_REGS_USE_GS_VARIABLE & 1, to_uniform = (input >> 8 & 0xff) != 0;
    *g = (struct SnxCtrGsPipe){.unit = g->unit, .emitter = g->emitter, .pica = p, .mode = config & 0xff,
                                .vs_outputs = (r[GS_VS_OUTMAP_A] & 15) + 1, .gs_inputs = (input & 15) + 1,
                                .start = config >> 16 & 0xff, .need_index = variable};
    if ((input >> 24) != GS_SHADER_MODE) return "the geometry shader's unit is not in geometry mode";
    if ((r[GS_VS_OUTMAP_A] & 15) != (r[GS_VS_OUTMAP_B] & 15)) return "the vertex outputs are counted two ways";
    switch (g->mode) {
        case GS_MODE_POINT:
            if (variable || to_uniform || g->gs_inputs % g->vs_outputs)
                return "a point-mode geometry shader with inputs the pipeline does not take";
            break;
        case GS_MODE_VARIABLE:
            if (!variable || !to_uniform) return "a variable-primitive geometry shader without uniform input";
            break;
        case GS_MODE_FIXED:
            g->batch = (config >> 8 & 15) + 1;
            if (variable || !to_uniform || g->vs_outputs != (config >> 12 & 15) + 1 ||
                g->start + g->vs_outputs * g->batch > 96)
                return "a fixed-primitive geometry shader with a layout the pipeline does not take";
            break;
        default: return "an unknown geometry shader mode";
    }
    return NULL;
}
static void gs_uniforms(SnxCtrPica* p, unsigned at, const float (*vectors)[4], unsigned count) {
    for (unsigned i = 0; i < count && at + i < 96; ++i) memcpy(p->gs.f[at + i], vectors[i], 16);
}
/* GeometryPipeline::SubmitIndex, for variable-primitive mode: the vertex count that opens a batch */
static void gs_index(SnxCtrPica* p, struct SnxCtrGsPipe* g, uint32_t count) {
    if (!count) return; /* a batch of no vertices has nothing to run */
    const float v = (float)count, vector[4] = {v, v, v, v};
    gs_uniforms(p, 0, &vector, 1);
    g->uniform_at = 1;
    g->main_remaining = p->regs[GS_VARIABLE_MAIN] + 1;
    g->total_remaining = count;
    g->need_index = false;
}
/* GeometryPipeline::SubmitVertex: the vertex shader's outputs (compacted by its mask) in, the shader run when the
   batch is whole */
static void gs_vertex(SnxCtrPica* p, SnxCtrMemory* m, const float (*output)[4]) {
    struct SnxCtrGsPipe* g = p->gs_pipe;
    bool ready = false;
    switch (g->mode) {
        case GS_MODE_POINT:
            memcpy(&g->inputs[g->filled], output, g->vs_outputs * 16);
            g->filled += g->vs_outputs;
            if (g->filled == g->gs_inputs) {
                g->filled = 0;
                snx_ctr_shader_load(p->regs, SNX_PICA_GS, &g->unit, (const float (*)[4])g->inputs);
                ready = true;
            }
            break;
        case GS_MODE_VARIABLE:
            if (g->main_remaining) { /* the first vertices of the batch bring every output, the rest only the position */
                gs_uniforms(p, g->uniform_at, output, g->vs_outputs);
                g->uniform_at += g->vs_outputs;
                --g->main_remaining;
            } else
                gs_uniforms(p, g->uniform_at++, output, 1);
            if (!--g->total_remaining) {
                g->need_index = true;
                ready = true;
            }
            break;
        default:
            gs_uniforms(p, g->start + g->filled, output, g->vs_outputs);
            g->filled += g->vs_outputs;
            if (g->filled == g->vs_outputs * g->batch) {
                g->filled = 0;
                ready = true;
            }
            break;
    }
    if (!ready) return;
    g->memory = m;
    snx_ctr_shader_run(&p->gs, &g->unit, p->regs[SNX_PICA_GS + SNX_PICA_SH_MAIN_OFFSET] & 0xffff, &g->emitter);
    p->gs.b |= 1u << 15; /* b15: set after the first run of a batch, for the shader to tell its first */
}
/* a vertex shader's outputs on their way to the assembler: through the geometry shader when one is in use */
static void vertex_out(SnxCtrPica* p, SnxCtrMemory* m, const float (*output)[4]) {
    if (gs_used(p))
        gs_vertex(p, m, output);
    else
        submit(p, m, output);
}

static SnxCtrPicaResult drawable(SnxCtrPica* p, bool reconfigure) {
    const char* missing = NULL;
    if ((p->regs[SNX_PICA_USE_GS] & 3) && !gs_used(p))
        missing = "an unknown geometry shader setting";
    else if (gs_used(p) && (reconfigure || !p->gs_pipe))
        missing = gs_reconfigure(p);
    if (!missing) missing = snx_ctr_raster_check(p);
    if (!missing) return SNX_CTR_PICA_OK;
    p->stop_reason = missing;
    return SNX_CTR_PICA_UNSUPPORTED;
}

SnxCtrPicaResult snx_ctr_pica_draw(SnxCtrPica* p, SnxCtrMemory* m, bool indexed) {
    enum { CACHE = 64 }; /* Azahar's circular-replacement vertex cache for indexed draws */
    SnxCtrPicaResult r = drawable(p, true);
    if (r != SNX_CTR_PICA_OK) return r;
    Loader l;
    loader_setup(p->regs, &l);
    uint32_t base = ((p->regs[ATTR_BASE] >> 1) & 0x0fffffff) * 16, index_info = p->regs[INDEX_ARRAY];
    uint32_t index_address = base + (index_info & 0x0fffffff);
    bool wide = index_info >> 31;
    unsigned char probe;
    /* an invalid base address draws nothing (Mario & Luigi: Superstar Saga sets one) */
    if (snx_ctr_phys_read(m, index_address, &probe, 1) != SNX_CTR_MEM_OK) return SNX_CTR_PICA_OK;
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
    float output[16][4] = {{0}};
    unsigned cache_at = 0, entry = p->regs[SNX_PICA_VS + SNX_PICA_SH_MAIN_OFFSET] & 0xffff;
    for (uint32_t i = 0; i < p->regs[NUM_VERTICES]; ++i) {
        uint32_t vertex = i + p->regs[VERTEX_OFFSET];
        bool hit = false;
        if (indexed) { /* indexed draws do not use the vertex offset */
            unsigned char b[2] = {0, 0};
            (void)snx_ctr_phys_read(m, index_address + i * (wide ? 2 : 1), b, wide ? 2 : 1);
            vertex = wide ? (uint32_t)(b[0] | (b[1] << 8)) : b[0];
        }
        /* a variable-primitive geometry shader takes the vertex that opens a batch as its vertex count */
        if (gs_used(p) && p->gs_pipe->need_index) {
            gs_index(p, p->gs_pipe, vertex);
            continue;
        }
        if (indexed) {
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
        vertex_out(p, m, (const float (*)[4])output);
    }
    free(unit);
    free(cache);
    return SNX_CTR_PICA_OK;
}

SnxCtrPicaResult snx_ctr_pica_draw_immediate(SnxCtrPica* p, SnxCtrMemory* m) {
    SnxCtrPicaResult r = drawable(p, p->immediate_reset);
    if (r != SNX_CTR_PICA_OK) return r;
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
    p->immediate_reset = false;
    vertex_out(p, m, (const float (*)[4])output);
    return SNX_CTR_PICA_OK;
}
