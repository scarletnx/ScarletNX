/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_VSGEN_H
#define SNX_CTR_VSGEN_H
#include "3ds/gpu/pica.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* The vertex shader of a draw: the PICA vertex program decompiled to a SPIR-V 1.3 module for Vulkan (vsgen.c, on
   common/utilities/spirv_emit.h's writer), with the outputs mapped to the rasterizer's semantics as Azahar's shader does without a
   geometry shader. Interface, written in GLSL's terms:
   - set 0 binding 0, std140: uniform vs_pica_data { uint b; uvec4 i[4]; vec4 f[96]; } (SnxCtrVsPica)
   - set 0 binding 1, std140: uniform vs_data { bool enable_clip1; bool flip_viewport; vec4 clip_coef; }
     (SnxCtrVsData: each bool a 32-bit uint, read as != 0)
   - inputs: location n is input register n, an attribute of input_type[n] (SNX_CTR_VS_FLOAT, _SINT, _UINT)
     with input_size[n] components (the rest are read as 0, 0, 0, 1); unused registers are not declared
   - outputs, the fragment shader's inputs (vk/fsgen.h): primary_color 0, texcoord0 1, texcoord1 2, texcoord2 3,
     texcoord0_w 4, normquat 5, view 6; gl_Position, and gl_ClipDistance[2] when clip_planes is set
   - the entry point "main", and SignedZeroInfNanPreserve 32 (SPV_KHR_float_controls) when asked for */
enum { SNX_CTR_VS_UNUSED, SNX_CTR_VS_FLOAT, SNX_CTR_VS_SINT, SNX_CTR_VS_UINT };
/* what picks a vertex shader: plain data, zeroed before it is filled (padding too), compared with memcmp */
typedef struct {
    uint64_t program_hash; /* of the uploaded program code and swizzle patterns */
    uint32_t main_offset;
    uint32_t num_outputs;
    uint8_t output_map[16]; /* output register -> output attribute index, 16 for an output the mask leaves out */
    uint32_t vs_output_total;
    uint32_t vs_output_attributes[7]; /* the rasterizer's semantic map words */
    uint8_t input_type[16], input_size[16];
    uint8_t clip_planes;
    uint8_t passthrough; /* the vertices are the host's (a geometry shader's): the 24 semantic slots in six vec4 inputs,
                            locations 0 to 5, with no program; every other member but clip_planes is then zero */
    uint8_t pad[2];
} SnxCtrVsConfig;
typedef struct {
    uint32_t b, pad[3];
    uint32_t i[4][4];
    float f[96][4];
} SnxCtrVsPica;
typedef struct {
    uint32_t enable_clip1, flip_viewport, pad[2];
    float clip_coef[4];
} SnxCtrVsData;
/* the hash of a program's code and swizzle patterns */
uint64_t snx_ctr_vs_hash(const SnxCtrShaderSetup* setup);
/* the config's parts the registers give (the program hash and the inputs are the caller's) */
void snx_ctr_vs_config(const SnxCtrPica* pica, SnxCtrVsConfig* config);
/* the vertex shader's SPIR-V module, *count words in *words (the caller frees it); false with the reason in *why when
   the program cannot be decompiled (a recursion, an instruction Azahar does not translate either, a program that does
   not always end) or the host ran out of memory. preserve_infnan declares SignedZeroInfNanPreserve, which PICA
   programs rely on, for a device that supports it */
bool snx_ctr_vs_spirv(const SnxCtrShaderSetup* setup, const SnxCtrVsConfig* config, bool preserve_infnan,
                      uint32_t** words, size_t* count, const char** why);
void snx_ctr_vs_uniforms(const SnxCtrPica* pica, SnxCtrVsPica* uniforms, SnxCtrVsData* data);
#endif
