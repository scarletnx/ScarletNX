/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_CTR_VK_RENDER_H
#define SNX_CTR_VK_RENDER_H
#include "3ds/gpu/pica.h"
#include <stdbool.h>
#include <volk.h>
/* The PICA200's draws on the GPU (render.c): the vertex program and the fragment pipeline as generated shaders
   (vsgen.c, fsgen.c), the output merger as the pipeline's fixed state, drawn into images of the render targets the
   guest has in memory. What the GPU makes stays on it, and guest memory catches up only when someone reads it:
   - A render target's images are kept, loaded from memory only when memory changed under them. What the draws write
     is newer than memory: those pages are the GPU's (write tracking, memory/ctr_memory.h) until the target is stored,
     on the GPU, for a GPU engine that reads that memory (snx_ctr_vk_sync), or for the CPU when it touches them.
   - Textures are decoded from memory on the GPU (shaders/texture.comp) and kept while their memory is unchanged:
     their pages are watched, and a write to them, the CPU's or a GPU engine's, drops them.
   - Memory the GPU writes (stores, transfers, fills, Y2R) is the GPU's until the submission that writes it is done,
     or, where the work is on a copy of the RAM (vk.h), until the CPU touches it and it is copied back.
   Vertex and index data are read where the work is: the RAM in place, or its copy. A draw with a geometry shader runs
   its vertex and geometry shaders on the CPU instead (draw.c), reading memory as the CPU does, and draws the triangles
   they make. */
/* the renderer of SnxCtrPicaHost (memory is the device's): the draw triggers, DrawArrays indexed or not;
   one immediate-mode vertex, pica->immediate_vertex, kept until snx_ctr_vk_vertices_end draws the ones kept */
SnxCtrPicaResult snx_ctr_vk_draw(SnxCtrPica* pica, SnxCtrMemory* memory, bool indexed);
SnxCtrPicaResult snx_ctr_vk_vertex(SnxCtrPica* pica, SnxCtrMemory* memory);
SnxCtrPicaResult snx_ctr_vk_vertices_end(SnxCtrPica* pica, SnxCtrMemory* memory);
/* A GPU engine (a transfer, a fill, Y2R, the presenter) is about to read [pa, pa + size), or write it (write): the
   render targets newer than memory there are stored first, on the GPU in the open recording, with no wait. A write
   makes what was kept of that memory stale (targets, textures), and the memory the GPU's until the open recording's
   submission is done */
void snx_ctr_vk_sync(uint32_t pa, uint32_t size, bool write);
/* The dirty colour target kept over the shown framebuffer [pa, pa + width*height*4): its image (GENERAL), the scale
   its 3D rendering used and the framebuffer's flip. format is transfer.h's colour format, which the target must have.
   A target at the console's scale is not one. False when no such target is kept. */
bool snx_ctr_vk_target_image(uint32_t pa, unsigned width, unsigned height, unsigned format, VkImageView* view,
                             unsigned* scale, unsigned* flip);
/* write tracking's handler (snx_ctr_track_handler), which the device installs: an owned page's targets stored and
   its GPU writes waited for, or its copy copied back; a watched page's textures and targets dropped, and its copy
   stale */
void snx_ctr_vk_track(void* context, uint32_t pa, SnxCtrTrackLevel level);
/* What the GPU's work took of the board's RAM since the device started: the bytes it read and wrote there, or of its
   copy (in place, a discrete GPU reads and writes each of them across the bus); the bytes copied in from the RAM to
   the copy, and back; and the CPU's accesses that write tracking brought to the renderer first (snx_ctr_vk_track) */
typedef struct {
    uint64_t read, written, copied_in, copied_out, traps;
} SnxCtrVkTraffic;
SnxCtrVkTraffic snx_ctr_vk_traffic(void);
/* the device's: a recording begins in slot; the open rendering pass ends before a submission; a submission of slot,
   serial, is done (what it used is free again, what it wrote is in memory) */
void snx_ctr_vk_render_begin(unsigned slot);
void snx_ctr_vk_render_close(void);
void snx_ctr_vk_render_retire(unsigned slot, uint64_t serial);
void snx_ctr_vk_render_free(void);
#endif
