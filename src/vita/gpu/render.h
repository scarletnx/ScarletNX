/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GPU's side of sceGxm (render.c): scenes drawn on the host's Vulkan device into the color surfaces' memory */
#ifndef SNX_VITA_RENDER_H
#define SNX_VITA_RENDER_H
#include "vita/gpu/gxm.h"
#include "vita/runtime/process.h"

/* the renderer, on the presenter's device; false without one (the scenes are then not drawn) */
bool vita_render_init(VitaProcess* p);
void vita_render_free(VitaProcess* p);
/* sceGxmMapMemory's range the GPU reads and writes, and its unmapping */
void vita_render_map(VitaProcess* p, uint32_t address, uint32_t size);
void vita_render_unmap(VitaProcess* p, uint32_t address);
void vita_render_begin_scene(VitaProcess* p, GxmContext* c);
/* sceGxmDraw with the context's state: indices at an address, of index_format (SceGxmIndexFormat: 0 16-bit, 1
   32-bit), count of them, instances times */
void vita_render_draw(VitaProcess* p, GxmContext* c, uint32_t primitive, uint32_t index_format, uint32_t indices,
                      uint32_t count, uint32_t instances);
/* the scene submitted: its serial */
uint64_t vita_render_end_scene(VitaProcess* p, GxmContext* c);
/* whether the submission of serial is done (waiting for it: vita_render_wait), and the work submitted so far all
   done (waiting for it when wait) */
bool vita_render_done(VitaProcess* p, uint64_t serial);
bool vita_render_wait(VitaProcess* p, uint64_t serial);
bool vita_render_idle(VitaProcess* p, bool wait);
uint64_t vita_render_submitted(VitaProcess* p);
/* Flush the current recording, preserving a scene in progress; the returned serial is the last submission. */
uint64_t vita_render_finish(VitaProcess* p);
/* A same-format, unkeyed transfer between linear guest buffers, recorded on the GPU. */
bool vita_render_transfer_copy(VitaProcess* p, uint32_t width, uint32_t height, uint32_t bytes_per_pixel,
                               uint32_t src_address, uint32_t src_x, uint32_t src_y, int32_t src_stride,
                               uint32_t dst_address, uint32_t dst_x, uint32_t dst_y, int32_t dst_stride);
/* sceDisplay's framebuffer (A8B8G8R8, pitch pixels a row) shown in the window from its memory on the GPU; false when it
   can't be now (no renderer, or a scene being recorded) */
bool vita_render_show(VitaProcess* p, uint32_t address, uint32_t pitch, uint32_t width, uint32_t height);
/* A decoded picture (planes: Y, U and V, FFmpeg's rows of linesize bytes) of width x height into the game's buffer at
   address as sceAvcdecDecode lays it out: Y in rows of pitch bytes, then the chroma, interleaved V first (pixel type
   0x20, YUV420_PACKED_RASTER) or U's plane then V's (0x10, YUV420_RASTER), on the GPU in the order of the work
   recorded (a scene being recorded goes on after it). False when it can't be */
bool vita_render_picture(VitaProcess* p, uint32_t address, uint32_t pitch, uint32_t pixel_type, uint32_t width,
                         uint32_t height, const uint8_t* const planes[3], const int linesize[3]);
/* a program's shader, made for its first draw, freed with it */
void vita_render_shader_free(VitaProcess* p, void* shader);
#endif
