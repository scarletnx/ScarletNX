/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's command processor: its internal register file (0x300 words), command lists, the shader units'
   program, swizzle and uniform uploads, the lighting, fog and procedural-texture LUT uploads, default attributes
   and the P3D interrupt request. Ported from Azahar video_core/pica/pica_core.cpp (GPL-2.0-or-later),
   shader_setup.cpp and packed_attribute.h, with register positions from regs_internal.h and the regs_*.h it
   includes (GPL-2.0-or-later), at azahar-emu/azahar a4b335e. Copyright 2015-2026 Citra Emulator Project / Azahar
   Emulator Project.
   A draw trigger goes to the host's renderer: the GPU's (vk/render.c) in the runtime, the software pipeline of draw.c
   and raster.c, the CPU reference, in the tests. */
#ifndef SNX_CTR_PICA_H
#define SNX_CTR_PICA_H
#include "3ds/memory/ctr_memory.h"
#include <stdbool.h>
#include <stdint.h>

#define SNX_CTR_PICA_REGS 0x300u

/* register indices (PICA_REG_INDEX in Azahar), the ones with side effects and the ones those read */
enum {
    SNX_PICA_IRQ_REQUEST = 0x010,
    SNX_PICA_IRQ_COMPARE = 0x020,
    SNX_PICA_IRQ_MASK = 0x030,
    SNX_PICA_IRQ_AUTOSTOP = 0x034,
    SNX_PICA_PROCTEX_LUT_CONFIG = 0x0af,
    SNX_PICA_PROCTEX_LUT_DATA = 0x0b0, /* 8 */
    SNX_PICA_FOG_LUT_OFFSET = 0x0e6,
    SNX_PICA_FOG_LUT_DATA = 0x0e8, /* 8 */
    SNX_PICA_LIGHTING_LUT_CONFIG = 0x1c5,
    SNX_PICA_LIGHTING_LUT_DATA = 0x1c8, /* 8 */
    SNX_PICA_TRIGGER_DRAW = 0x22e,
    SNX_PICA_TRIGGER_DRAW_INDEXED = 0x22f,
    SNX_PICA_USE_GS = 0x229,
    SNX_PICA_DEFAULT_ATTR_INDEX = 0x232,
    SNX_PICA_DEFAULT_ATTR_VALUE = 0x233, /* 3 */
    SNX_PICA_CMDBUF_SIZE = 0x238,        /* 2 */
    SNX_PICA_CMDBUF_ADDR = 0x23a,        /* 2 */
    SNX_PICA_CMDBUF_TRIGGER = 0x23c,     /* 2 */
    SNX_PICA_MAX_INPUT_ATTR = 0x242,
    SNX_PICA_GS_EXCLUSIVE = 0x244,
    SNX_PICA_GPU_MODE = 0x245,
    SNX_PICA_TOPOLOGY = 0x25e,
    SNX_PICA_RESTART_PRIMITIVE = 0x25f,
    SNX_PICA_GS = 0x280, /* shader unit register blocks, 0x30 words each */
    SNX_PICA_VS = 0x2b0
};
/* offsets inside a shader unit's block (ShaderRegs) */
enum {
    SNX_PICA_SH_BOOL = 0x00,
    SNX_PICA_SH_INT = 0x01, /* 4 */
    SNX_PICA_SH_INPUT_CONFIG = 0x09,
    SNX_PICA_SH_MAIN_OFFSET = 0x0a,
    SNX_PICA_SH_ATTR_MAP_LOW = 0x0b,
    SNX_PICA_SH_ATTR_MAP_HIGH = 0x0c,
    SNX_PICA_SH_OUTPUT_MASK = 0x0d,
    SNX_PICA_SH_UNIFORM_SETUP = 0x10, /* index (bits 0..6), float32 (bit 31) */
    SNX_PICA_SH_UNIFORM_VALUE = 0x11, /* 8 */
    SNX_PICA_SH_PROGRAM_OFFSET = 0x1b,
    SNX_PICA_SH_PROGRAM_WORD = 0x1c, /* 8 */
    SNX_PICA_SH_SWIZZLE_OFFSET = 0x25,
    SNX_PICA_SH_SWIZZLE_WORD = 0x26 /* 8 */
};

/* a shader unit's uploaded state (ShaderSetup): code, swizzle patterns, uniforms as host floats */
typedef struct {
    uint32_t code[4096], swizzle[4096];
    float f[96][4];
    uint32_t b;        /* 16 bool uniforms */
    uint8_t i[4][4];   /* 4 int uniforms, x y z w */
    uint32_t queue[4]; /* the float uniform being assembled (PackedAttribute) */
    unsigned queued;
} SnxCtrShaderSetup;

/* OutputVertex: the rasterizer's attributes, in the semantic order of RasterizerRegs::VSOutputAttributes */
typedef struct {
    float pos[4], quat[4], color[4], tc0[2], tc1[2], tc0_w, unused0, view[3], unused1, tc2[2];
} SnxCtrVertex;

/* PrimitiveAssembler's state: topology 0 list, 1 strip, 2 fan, 3 geometry shader */
typedef struct {
    SnxCtrVertex buffer[2];
    unsigned index, topology;
    bool strip_ready, winding;
} SnxCtrAssembler;

typedef struct SnxCtrPica SnxCtrPica;
typedef enum {
    SNX_CTR_PICA_OK,          /* the list ran to its end or to an interrupt with autostop */
    SNX_CTR_PICA_UNSUPPORTED, /* a draw using a part not ported yet (stop_reason), or a list past the staging bound */
    SNX_CTR_PICA_FAULT        /* a jump to a list outside the board's memory */
} SnxCtrPicaResult;
/* the host side of the command processor: the P3D interrupt, and the renderer the draws go to (none: they draw
   nothing). Immediate mode gives the renderer each vertex as it is formed (pica->immediate_vertex), then, before a
   register write other than another vertex's attributes and at the end of the list, vertices_end (when set) to draw
   the ones given */
typedef struct {
    void* context;
    void (*interrupt)(void* context);
    SnxCtrPicaResult (*draw)(SnxCtrPica* pica, SnxCtrMemory* memory, bool indexed);
    SnxCtrPicaResult (*vertex)(SnxCtrPica* pica, SnxCtrMemory* memory);
    SnxCtrPicaResult (*vertices_end)(SnxCtrPica* pica, SnxCtrMemory* memory);
} SnxCtrPicaHost;

struct SnxCtrPica {
    uint32_t regs[SNX_CTR_PICA_REGS];
    SnxCtrShaderSetup vs, gs;
    uint32_t lighting_lut[24][256], fog_lut[128];
    uint32_t proctex_noise[128], proctex_color_map[128], proctex_alpha_map[128], proctex_color[256],
        proctex_color_diff[256];
    float default_attributes[16][4];
    /* immediate mode: the attribute being assembled, and the vertex being formed */
    uint32_t immediate_queue[3];
    unsigned immediate_queued, immediate_attribute;
    float immediate_vertex[16][4];
    bool immediate_reset; /* reset_geometry_pipeline: the next immediate vertex starts a new batch */
    bool immediate_given; /* vertices given to the renderer that vertices_end has not drawn yet */
    uint32_t vs_version;  /* counts the vertex program's code and swizzle uploads: a renderer's cache key */
    /* the primitive assembler (PrimitiveAssembler), kept across draws; topology and restart writes reset it */
    SnxCtrAssembler assembler;
    /* the list being run, a staged copy of guest memory; a jump (a command buffer trigger) replaces it */
    struct SnxCtrGsPipe* gs_pipe; /* the geometry pipeline (draw.c), made at the first draw with a geometry shader */
    uint32_t* list;
    uint32_t list_address, list_words, list_index;
    uint32_t stop_register;  /* the register whose write stopped the last run (UNSUPPORTED, FAULT) */
    const char* stop_reason; /* what was unsupported */
};

/* registers as GSP initializes them (PicaCore::InitializeRegs) */
void snx_ctr_pica_init(SnxCtrPica* pica);
void snx_ctr_pica_free(SnxCtrPica* pica);

/* the software renderer's draws (draw.c, the CPU reference): DrawArrays, indexed or not, and DrawImmediate for the
   vertex immediate mode formed */
SnxCtrPicaResult snx_ctr_pica_draw(SnxCtrPica* pica, SnxCtrMemory* memory, bool indexed);
SnxCtrPicaResult snx_ctr_pica_draw_immediate(SnxCtrPica* pica, SnxCtrMemory* memory);
/* Runs the command list of size bytes at physical address (GPU::SubmitCmdList's checks are the caller's). A list
   outside memory raises P3D and does nothing, as the GPU does */
SnxCtrPicaResult snx_ctr_pica_run(SnxCtrPica* pica, SnxCtrMemory* memory, uint32_t address, uint32_t size,
                                  const SnxCtrPicaHost* host);
/* One register write as a command list makes it (mask: the header's 4-bit byte enable) */
SnxCtrPicaResult snx_ctr_pica_write(SnxCtrPica* pica, SnxCtrMemory* memory, uint32_t id, uint32_t value, uint32_t mask,
                                    const SnxCtrPicaHost* host, bool* stop);

/* PICA float24 (sign, 7-bit exponent, 16-bit mantissa) as the host float Azahar computes with: denormals
   renormalized losslessly, an all-ones exponent infinity or NaN (pica_types.h Float::FromRaw) */
float snx_ctr_f24(uint32_t raw);
/* PICA multiplication: infinity times zero is zero, not NaN */
float snx_ctr_pica_mul(float a, float b);
#endif
