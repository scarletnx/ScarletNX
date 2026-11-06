/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Azahar video_core/pica/pica_core.cpp (ProcessCmdList, WriteInternalReg, HandleSpecialReg,
   SubmitImmediate), shader_setup.cpp, packed_attribute.h and pica_types.h (GPL-2.0-or-later, Copyright
   2015-2026 Citra Emulator Project / Azahar Emulator Project) at azahar-emu/azahar a4b335e. The batched and
   sequential fast paths are written as the per-word writes they are equivalent to. */
#include "3ds/gpu/pica.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

/* a list larger than this (bytes) is refused rather than staged */
#define LIST_LIMIT (16u * 1024 * 1024)

static const uint32_t byte_mask[16] = {
    0x00000000, 0x000000ff, 0x0000ff00, 0x0000ffff, 0x00ff0000, 0x00ff00ff, 0x00ffff00, 0x00ffffff,
    0xff000000, 0xff0000ff, 0xff00ff00, 0xff00ffff, 0xffff0000, 0xffff00ff, 0xffffff00, 0xffffffff,
};

float snx_ctr_f24(uint32_t hex) {
    const int bias = 127 - ((1 << 6) - 1);
    int exponent = (int)((hex >> 16) & 0x7f);
    uint32_t mantissa = hex & 0xffff, sign = ((hex >> 23) & 1) << 31, bits;
    float f;
    if (!exponent && !mantissa)
        bits = sign;
    else if (!exponent) { /* PICA flushes denormals; converted losslessly, as Azahar does */
        exponent = bias + 1;
        while (!(mantissa & (1u << 16))) {
            --exponent;
            mantissa <<= 1;
        }
        mantissa &= 0xffff;
        bits = sign | ((uint32_t)exponent << 23) | (mantissa << 7);
    } else if (exponent == 0x7f)
        bits = sign | (0xffu << 23) | (mantissa << 7);
    else
        bits = sign | ((uint32_t)(bias + exponent) << 23) | (mantissa << 7);
    memcpy(&f, &bits, 4);
    return f;
}

float snx_ctr_pica_mul(float a, float b) {
    float r = a * b;
    if (isnan(r) && !isnan(a) && !isnan(b)) r = 0.0f;
    return r;
}

void snx_ctr_pica_init(SnxCtrPica* p) {
    memset(p, 0, sizeof *p); /* 100 KB: no compound-literal temporary */
    /* values GSP initializes */
    p->regs[SNX_PICA_IRQ_AUTOSTOP] = 1;
    p->regs[SNX_PICA_IRQ_MASK] = 0xfffffff0u;
    /* older libctru did not set it up; hardware has it from earlier software */
    p->regs[SNX_PICA_IRQ_COMPARE] = 0x12345678u;
    /* "Tales of Abyss expects this register to have the following default values": GS max input index 1, VS mode */
    p->regs[SNX_PICA_GS + SNX_PICA_SH_INPUT_CONFIG] = 1u | (0xa0u << 24);
    p->immediate_reset = true;
}

void snx_ctr_pica_free(SnxCtrPica* p) {
    free(p->list);
    p->list = NULL;
}

/* the shader units share configuration unless unit 3 is set exclusive or a geometry shader is in use */
static bool shared_gs(const SnxCtrPica* p) {
    return !(p->regs[SNX_PICA_GS_EXCLUSIVE] & 1) && (p->regs[SNX_PICA_USE_GS] & 3) == 0;
}

/* PackedAttribute: three float24 words (w, z, y, x packed big-end first) or four float32 ones make a vector */
static bool pack(uint32_t* queue, unsigned* queued, uint32_t word, bool float32, float out[4]) {
    queue[(*queued)++] = word;
    if (*queued < (float32 ? 4u : 3u)) return false;
    *queued = 0;
    if (float32) {
        for (unsigned i = 0; i < 4; ++i) memcpy(&out[i], &queue[3 - i], 4); /* AsFloat32: x is the last word */
    } else {
        out[0] = snx_ctr_f24(queue[2] & 0xffffff);
        out[1] = snx_ctr_f24(((queue[1] & 0xffff) << 8) | ((queue[2] >> 24) & 0xff));
        out[2] = snx_ctr_f24(((queue[0] & 0xff) << 16) | ((queue[1] >> 16) & 0xffff));
        out[3] = snx_ctr_f24(queue[0] >> 8);
    }
    return true;
}

static void uniform_bools(SnxCtrShaderSetup* s, uint32_t value) { s->b = value & 0xffff; }
static void uniform_ints(SnxCtrShaderSetup* s, unsigned index, uint32_t value) {
    for (unsigned c = 0; c < 4; ++c) s->i[index][c] = (uint8_t)(value >> (c * 8));
}
/* WriteUniformFloatReg: the index register (bits 0..6) advances with each completed vector; an index past the
   last uniform is ignored, as hardware does ("mainly 0x7F") */
static int uniform_float(SnxCtrPica* p, unsigned block, SnxCtrShaderSetup* s, uint32_t value) {
    uint32_t* setup = &p->regs[block + SNX_PICA_SH_UNIFORM_SETUP];
    float v[4];
    if (!pack(s->queue, &s->queued, value, (*setup >> 31) != 0, v)) return -1;
    unsigned index = *setup & 0x7f;
    if (index >= 96) return -1;
    memcpy(s->f[index], v, sizeof(v));
    *setup = (*setup & ~0x7fu) | ((index + 1) & 0x7f);
    return (int)index;
}

static SnxCtrPicaResult jump(SnxCtrPica* p, SnxCtrMemory* memory, unsigned channel) {
    uint32_t address = (p->regs[SNX_PICA_CMDBUF_ADDR + channel] & 0x0fffffff) * 8;
    uint32_t size = (p->regs[SNX_PICA_CMDBUF_SIZE + channel] & 0xfffff) * 8;
    if (size > LIST_LIMIT) return SNX_CTR_PICA_UNSUPPORTED;
    uint32_t* list = (uint32_t*)malloc(size ? size : 4);
    if (!list) return SNX_CTR_PICA_UNSUPPORTED;
    if (size && snx_ctr_phys_read(memory, address, list, size) != SNX_CTR_MEM_OK) {
        free(list);
        return SNX_CTR_PICA_FAULT;
    }
    free(p->list);
    p->list = list;
    p->list_address = address;
    p->list_words = size / 4;
    p->list_index = 0;
    return SNX_CTR_PICA_OK;
}

/* the 32 pairs of bytes irq_request and irq_compare are compared byte by byte (gbatek; Azahar's any_byte_match) */
static bool any_byte_match(uint32_t a, uint32_t b) {
    for (unsigned i = 0; i < 4; ++i)
        if (((a >> (i * 8)) & 0xff) == ((b >> (i * 8)) & 0xff)) return true;
    return false;
}

static SnxCtrPicaResult special(SnxCtrPica* p, SnxCtrMemory* memory, uint32_t id, uint32_t value,
                                const SnxCtrPicaHost* host, bool* stop) {
    bool share = shared_gs(p);
    switch (id) {
        case SNX_PICA_IRQ_REQUEST:
            if (any_byte_match(p->regs[id], p->regs[SNX_PICA_IRQ_COMPARE])) {
                if (host && host->interrupt) host->interrupt(host->context);
                if (p->regs[SNX_PICA_IRQ_AUTOSTOP]) *stop = true;
            }
            return SNX_CTR_PICA_OK;
        case SNX_PICA_TOPOLOGY: /* PrimitiveAssembler::Reconfigure */
            p->assembler = (SnxCtrAssembler){0};
            p->assembler.topology = (p->regs[id] >> 8) & 3;
            return SNX_CTR_PICA_OK;
        case SNX_PICA_RESTART_PRIMITIVE:
            p->assembler.index = 0;
            p->assembler.strip_ready = p->assembler.winding = false;
            return SNX_CTR_PICA_OK;
        case SNX_PICA_DEFAULT_ATTR_INDEX: /* ImmediateModeState::Reset */
            p->immediate_queued = 0;
            p->immediate_attribute = 0;
            p->immediate_reset = true;
            return SNX_CTR_PICA_OK;
        case SNX_PICA_DEFAULT_ATTR_VALUE:
        case SNX_PICA_DEFAULT_ATTR_VALUE + 1:
        case SNX_PICA_DEFAULT_ATTR_VALUE + 2: {
            float v[4];
            uint32_t* index = &p->regs[SNX_PICA_DEFAULT_ATTR_INDEX];
            if (!pack(p->immediate_queue, &p->immediate_queued, value, false, v)) return SNX_CTR_PICA_OK;
            if ((*index & 0xf) != *index) return SNX_CTR_PICA_OK; /* an invalid index: logged and ignored */
            if (*index < 0xf) {
                memcpy(p->default_attributes[*index], v, sizeof(v));
                ++*index;
                return SNX_CTR_PICA_OK;
            }
            /* index 15: immediate-mode drawing, a vertex once every input attribute is in */
            memcpy(p->immediate_vertex[p->immediate_attribute], v, sizeof(v));
            if (p->immediate_attribute < (p->regs[SNX_PICA_MAX_INPUT_ATTR] & 0xf)) {
                ++p->immediate_attribute;
                return SNX_CTR_PICA_OK;
            }
            p->immediate_attribute = 0;
            if (!host || !host->vertex) return SNX_CTR_PICA_OK;
            p->immediate_given = true;
            return host->vertex(p, memory);
        }
        case SNX_PICA_CMDBUF_TRIGGER:
        case SNX_PICA_CMDBUF_TRIGGER + 1: return jump(p, memory, id - SNX_PICA_CMDBUF_TRIGGER);
        case SNX_PICA_TRIGGER_DRAW:
        case SNX_PICA_TRIGGER_DRAW_INDEXED:
            return host && host->draw ? host->draw(p, memory, id == SNX_PICA_TRIGGER_DRAW_INDEXED) : SNX_CTR_PICA_OK;
        case SNX_PICA_GS + SNX_PICA_SH_BOOL: uniform_bools(&p->gs, p->regs[id]); return SNX_CTR_PICA_OK;
        case SNX_PICA_VS + SNX_PICA_SH_BOOL:
            uniform_bools(&p->vs, p->regs[id]);
            if (share) uniform_bools(&p->gs, p->regs[id]);
            return SNX_CTR_PICA_OK;
        case SNX_PICA_VS + SNX_PICA_SH_OUTPUT_MASK:
            if (share)
                p->regs[SNX_PICA_GS + SNX_PICA_SH_OUTPUT_MASK] =
                    (p->regs[SNX_PICA_GS + SNX_PICA_SH_OUTPUT_MASK] & ~0xffffu) | (value & 0xffff);
            return SNX_CTR_PICA_OK;
        default: break;
    }
    if (id >= SNX_PICA_GS + SNX_PICA_SH_INT && id < SNX_PICA_GS + SNX_PICA_SH_INT + 4)
        uniform_ints(&p->gs, id - (SNX_PICA_GS + SNX_PICA_SH_INT), p->regs[id]);
    else if (id >= SNX_PICA_VS + SNX_PICA_SH_INT && id < SNX_PICA_VS + SNX_PICA_SH_INT + 4) {
        uniform_ints(&p->vs, id - (SNX_PICA_VS + SNX_PICA_SH_INT), p->regs[id]);
        if (share) uniform_ints(&p->gs, id - (SNX_PICA_VS + SNX_PICA_SH_INT), p->regs[id]);
    } else if (id >= SNX_PICA_GS + SNX_PICA_SH_UNIFORM_VALUE && id < SNX_PICA_GS + SNX_PICA_SH_UNIFORM_VALUE + 8)
        (void)uniform_float(p, SNX_PICA_GS, &p->gs, value);
    else if (id >= SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE && id < SNX_PICA_VS + SNX_PICA_SH_UNIFORM_VALUE + 8) {
        int index = uniform_float(p, SNX_PICA_VS, &p->vs, value);
        if (share && index >= 0) memcpy(p->gs.f[index], p->vs.f[index], sizeof(p->vs.f[index]));
    } else if (id >= SNX_PICA_GS + SNX_PICA_SH_PROGRAM_WORD && id < SNX_PICA_GS + SNX_PICA_SH_PROGRAM_WORD + 8) {
        uint32_t* offset = &p->regs[SNX_PICA_GS + SNX_PICA_SH_PROGRAM_OFFSET];
        if (*offset < 4096) p->gs.code[(*offset)++] = value;
    } else if (id >= SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD && id < SNX_PICA_VS + SNX_PICA_SH_PROGRAM_WORD + 8) {
        uint32_t* offset = &p->regs[SNX_PICA_VS + SNX_PICA_SH_PROGRAM_OFFSET];
        ++p->vs_version;
        if (*offset < 512) { /* the vertex units hold 512 instructions */
            p->vs.code[*offset] = value;
            if (share) p->gs.code[*offset] = value;
            ++*offset;
        }
    } else if (id >= SNX_PICA_GS + SNX_PICA_SH_SWIZZLE_WORD && id < SNX_PICA_GS + SNX_PICA_SH_SWIZZLE_WORD + 8) {
        uint32_t* offset = &p->regs[SNX_PICA_GS + SNX_PICA_SH_SWIZZLE_OFFSET];
        if (*offset < 4096) p->gs.swizzle[(*offset)++] = value;
    } else if (id >= SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_WORD && id < SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_WORD + 8) {
        uint32_t* offset = &p->regs[SNX_PICA_VS + SNX_PICA_SH_SWIZZLE_OFFSET];
        ++p->vs_version;
        if (*offset < 4096) {
            p->vs.swizzle[*offset] = value;
            if (share) p->gs.swizzle[*offset] = value;
            ++*offset;
        }
    } else if (id >= SNX_PICA_LIGHTING_LUT_DATA && id < SNX_PICA_LIGHTING_LUT_DATA + 8) {
        uint32_t *config = &p->regs[SNX_PICA_LIGHTING_LUT_CONFIG], index = *config & 0xff, type = (*config >> 8) & 31;
        if (type < 24) p->lighting_lut[type][index] = value;
        *config = (*config & ~0xffu) | ((index + 1) & 0xff);
    } else if (id >= SNX_PICA_FOG_LUT_DATA && id < SNX_PICA_FOG_LUT_DATA + 8) {
        uint32_t* offset = &p->regs[SNX_PICA_FOG_LUT_OFFSET];
        p->fog_lut[(*offset & 0xffff) % 128] = value;
        *offset = (*offset & ~0xffffu) | ((*offset + 1) & 0xffff);
    } else if (id >= SNX_PICA_PROCTEX_LUT_DATA && id < SNX_PICA_PROCTEX_LUT_DATA + 8) {
        uint32_t *config = &p->regs[SNX_PICA_PROCTEX_LUT_CONFIG], index = *config & 0xff;
        switch ((*config >> 8) & 15) {
            case 0: p->proctex_noise[index % 128] = value; break;
            case 2: p->proctex_color_map[index % 128] = value; break;
            case 3: p->proctex_alpha_map[index % 128] = value; break;
            case 4: p->proctex_color[index % 256] = value; break;
            case 5: p->proctex_color_diff[index % 256] = value; break;
            default: break;
        }
        *config = (*config & ~0xffu) | ((index + 1) & 0xff);
    }
    /* gpu_mode needs nothing */
    return SNX_CTR_PICA_OK;
}

static SnxCtrPicaResult end_vertices(SnxCtrPica* p, SnxCtrMemory* memory, const SnxCtrPicaHost* host) {
    p->immediate_given = false;
    return host && host->vertices_end ? host->vertices_end(p, memory) : SNX_CTR_PICA_OK;
}

SnxCtrPicaResult snx_ctr_pica_write(SnxCtrPica* p, SnxCtrMemory* memory, uint32_t id, uint32_t value, uint32_t mask,
                                    const SnxCtrPicaHost* host, bool* stop) {
    if (id >= SNX_CTR_PICA_REGS) return SNX_CTR_PICA_OK; /* writes past the register file do nothing */
    /* the immediate vertices given are drawn with the state they were given in */
    if (p->immediate_given && (id < SNX_PICA_DEFAULT_ATTR_VALUE || id > SNX_PICA_DEFAULT_ATTR_VALUE + 2)) {
        SnxCtrPicaResult r = end_vertices(p, memory, host);
        if (r != SNX_CTR_PICA_OK) return r;
    }
    uint32_t m = byte_mask[mask & 15];
    p->regs[id] = (p->regs[id] & ~m) | (value & m);
    /* the handlers take the unmasked value, as WriteInternalReg passes it */
    return special(p, memory, id, value, host, stop);
}

SnxCtrPicaResult snx_ctr_pica_run(SnxCtrPica* p, SnxCtrMemory* memory, uint32_t address, uint32_t size,
                                  const SnxCtrPicaHost* host) {
    bool stop = false;
    /* GPU::SubmitCmdList: a list outside memory raises P3D and is not run */
    if (size > LIST_LIMIT) return SNX_CTR_PICA_UNSUPPORTED;
    free(p->list);
    p->list = (uint32_t*)malloc(size ? size : 4);
    if (!p->list) return SNX_CTR_PICA_UNSUPPORTED;
    if (size && snx_ctr_phys_read(memory, address, p->list, size) != SNX_CTR_MEM_OK) {
        if (host && host->interrupt) host->interrupt(host->context);
        return SNX_CTR_PICA_OK;
    }
    p->list_address = address;
    p->list_words = size / 4;
    p->list_index = 0;
    while (p->list_index < p->list_words && !stop) {
        if (p->list_index % 2) ++p->list_index; /* commands are 8-byte aligned */
        if (p->list_index + 1 >= p->list_words) break;
        uint32_t value = p->list[p->list_index++];
        uint32_t header = p->list[p->list_index++];
        uint32_t id = header & 0xffff;
        uint32_t mask = (header >> 16) & 15;
        uint32_t count = (header >> 20) & 0xff;
        uint32_t group = header >> 31;
        SnxCtrPicaResult r = snx_ctr_pica_write(p, memory, id, value, mask, host, &stop);
        if (r != SNX_CTR_PICA_OK) {
            p->stop_register = id;
            return r;
        }
        /* a jump started a new list: its words follow, not this command's extra data */
        if (p->list_index == 0) continue;
        if (count > p->list_words - p->list_index) count = p->list_words - p->list_index;
        for (uint32_t i = 0; i < count && !stop; ++i) {
            uint32_t extra = p->list[p->list_index++], at = id + (group ? i + 1 : 0);
            r = snx_ctr_pica_write(p, memory, at, extra, mask, host, &stop);
            if (r != SNX_CTR_PICA_OK) {
                p->stop_register = at;
                return r;
            }
            if (p->list_index == 0) break;
        }
    }
    return p->immediate_given ? end_vertices(p, memory, host) : SNX_CTR_PICA_OK;
}
