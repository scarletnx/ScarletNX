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
   - Memory the GPU writes (stores, transfers, fills, Y2R) is the GPU's until the submission that writes it is done.
   Vertex and index data are read in place from the imported RAM. */
/* the renderer of SnxCtrPicaHost (memory is the device's, imported): the draw triggers, DrawArrays indexed or not;
   one immediate-mode vertex, pica->immediate_vertex, kept until snx_ctr_vk_vertices_end draws the ones kept */
SnxCtrPicaResult snx_ctr_vk_draw(SnxCtrPica* pica, SnxCtrMemory* memory, bool indexed);
SnxCtrPicaResult snx_ctr_vk_vertex(SnxCtrPica* pica, SnxCtrMemory* memory);
SnxCtrPicaResult snx_ctr_vk_vertices_end(SnxCtrPica* pica, SnxCtrMemory* memory);
/* A GPU engine (a transfer, a fill, Y2R, the presenter) is about to read [pa, pa + size), or write it (write): the
   render targets newer than memory there are stored first, on the GPU in the open recording, with no wait. A write
   makes what was kept of that memory stale (targets, textures), and the memory the GPU's until the open recording's
   submission is done */
void snx_ctr_vk_sync(uint32_t pa, uint32_t size, bool write);
/* A display transfer from a colour target to a framebuffer in memory (tiled rows to linear ones, no crop with a flip)
   also made at the resolution scale, as the landscape image the window shows: the target (input, in_w pixels a row,
   the one the GPU keeps) read in place, the picture out_w by out_h pixels, flipped as the transfer is, opaque for an
   output with no alpha. The transfer's box filter averages 1 << hs columns and 1 << vs rows to a pixel. It is kept
   until the framebuffer's memory is written. False when no scale is set or no target is kept there: the window shows
   the memory */
bool snx_ctr_vk_hires_make(uint32_t input, uint32_t in_w, uint32_t output, uint32_t out_w, uint32_t out_h,
                           uint32_t out_bytes, bool flip, bool opaque, unsigned hs, unsigned vs);
/* the display made for the framebuffer at pa (width pixels a row, height rows), still what memory holds: its image
   and size. False when the window shows the memory */
bool snx_ctr_vk_hires(uint32_t pa, uint32_t width, uint32_t height, VkImage* image, VkExtent2D* extent);
/* write tracking's handler (snx_ctr_track_handler), which the device installs: an owned page's targets stored and
   its GPU writes waited for, a watched page's textures and targets dropped */
void snx_ctr_vk_track(void* context, uint32_t pa, SnxCtrTrackLevel level);
/* the device's: a recording begins in slot; the open rendering pass ends before a submission; a submission of slot,
   serial, is done (what it used is free again, what it wrote is in memory) */
void snx_ctr_vk_render_begin(unsigned slot);
void snx_ctr_vk_render_close(void);
void snx_ctr_vk_render_retire(unsigned slot, uint64_t serial);
void snx_ctr_vk_render_free(void);
#endif
