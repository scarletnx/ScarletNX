/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The PICA200's fixed-function back end in software: clipping, triangle setup, rasterization, texturing, the
   texture combiners, fog, the alpha, stencil and depth tests and blending into the framebuffer in physical memory.
   Ported from Azahar video_core/renderer_software/{sw_rasterizer,sw_clipper,sw_texturing,sw_framebuffer}.cpp and
   video_core/pica/output_vertex.cpp (GPL-2.0-or-later, Copyright 2014-2026 Citra Emulator Project / Azahar
   Emulator Project) at azahar-emu/azahar a4b335e, fragment lighting in lighting.c. Procedural textures and shadow
   textures and rendering are not ported yet: a draw that enables one stops (snx_ctr_raster_check). */
#ifndef SNX_CTR_RASTER_H
#define SNX_CTR_RASTER_H
#include "3ds/gpu/pica.h"
#include <stddef.h>

/* Sends the pica's triangles (draw.c) to this back end */
void snx_ctr_raster_attach(SnxCtrPica* pica);
/* NULL when the configured state is drawable, else a sentence naming the first unported feature it enables */
const char* snx_ctr_raster_check(const SnxCtrPica* pica);
/* ComputeFragmentsColors (lighting.c): the primary and secondary fragment colours of a fragment with the
   normalized quaternion and view vector */
void snx_ctr_lighting(const SnxCtrPica* pica, const float quat[4], const float view[3], uint8_t textures[4][4],
                      uint8_t primary[4], uint8_t secondary[4]);
/* RasterizerSoftware::AddTriangle: clips the triangle and rasterizes what is left */
void snx_ctr_raster_triangle(const SnxCtrPica* pica, SnxCtrMemory* memory, const SnxCtrVertex* v0,
                             const SnxCtrVertex* v1, const SnxCtrVertex* v2);
#endif
