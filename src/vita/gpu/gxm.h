/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sceGxm's state, which the API's functions (vita/hle/gxm.c) keep and the renderer (render.c) draws with */
#ifndef SNX_VITA_GXM_H
#define SNX_VITA_GXM_H
#include "vita/gpu/texture.h"
#include <stdbool.h>
#include <stdint.h>

#define GXM_STREAMS          16u
#define GXM_ATTRIBUTES       16u
#define GXM_TEXTURES         16u
#define GXM_UNIFORM_BUFFERS  15u /* the default one is GXP_DEFAULT_CONTAINER, 14 */
#define GXM_PROGRAMS         4096u /* vertex programs, as the patcher's memory allows */
#define GXM_FRAGMENT_PROGRAMS 32768u /* fragment programs: one game creates over 13000 (a program for each pair of
                                        fragment GXP, vertex program and blend state) while it loads its shaders, and
                                        each draw after a refused creation fails with SCE_GXM_ERROR_NULL_PROGRAM */
#define GXM_TARGETS          16u
#define GXM_MAPPINGS         128u
#define GXM_DISPLAY_QUEUE    16u
#define GXM_NOTES            64u
/* the handles the API gives for what the runtime keeps (programs, render targets, sync objects, the patcher): addresses
   in and past the runtime's page that nothing maps, never read by the game */
#define GXM_HANDLE_TARGET    0x80006000u
#define GXM_HANDLE_SYNC      0x80007000u
#define GXM_HANDLE_PATCHER   0x80008000u
#define GXM_HANDLE_VERTEX    0x80010000u /* GXM_PROGRAMS handles, GXM_PROGRAM_HANDLE apart */
#define GXM_HANDLE_FRAGMENT  0x80020000u /* GXM_FRAGMENT_PROGRAMS handles, up to 0x800a0000 (0x80100000 is the
                                            variables' slots) */
#define GXM_PROGRAM_HANDLE   16u

typedef struct {
    uint16_t stream, offset;
    uint8_t format, components;
    uint16_t reg; /* the attribute's resource index */
} GxmAttribute;
typedef struct {
    uint16_t stride, index_source;
} GxmStream;
/* the shader patcher's programs: creating one the patcher already has (the same GXP and state) gives that one again,
   which is released when every creation of it has been */
typedef struct {
    uint32_t references; /* 0 for a free slot */
    uint32_t program;    /* the GXP, in guest memory */
    GxmAttribute attributes[GXM_ATTRIBUTES];
    GxmStream streams[GXM_STREAMS];
    uint32_t attribute_count, stream_count;
    void* shader; /* the renderer's */
} GxmVertexProgram;
typedef struct {
    uint32_t references;
    bool blend;
    uint32_t program, vertex_program; /* GXPs */
    uint32_t output_format, multisample;
    /* SceGxmBlendInfo: the color mask (bits 0 to 7), color and alpha functions (8 and 12), color source and destination
       (16, 20), alpha source and destination (24, 28) */
    uint32_t blend_info;
    void* shader;
} GxmFragmentProgram;
#define GXM_COLOR_SWIZZLE3_RGB 0x00100000u /* SCE_GXM_COLOR_SWIZZLE3_RGB: a three-component format's B first */
/* SceGxmColorSurfaceType: how a color surface's memory lays its pixels out (rows, 32x32 tiles, or Morton order) */
#define GXM_COLOR_SURFACE_LINEAR   0x00000000u
#define GXM_COLOR_SURFACE_TILED    0x04000000u
#define GXM_COLOR_SURFACE_SWIZZLED 0x08000000u
/* a color surface as sceGxmColorSurfaceInit made it (the guest's SceGxmColorSurface holds these words) */
typedef struct {
    uint32_t flags, width, height, stride, data, format, type, output_register_size;
} GxmColorSurface;
/* a depth and stencil surface as sceGxmDepthStencilSurfaceInit made it: its control word (the runtime's own layout,
   which nothing of the game reads: SceGxmDepthStencilFormat >> 12, the force load and store modes, tiled, the stride
   in samples from bit 17), the depth and stencil data, the background depth and its control */
#define GXM_DEPTH_FORMAT_MASK 0x3fffu
#define GXM_DEPTH_FORCE_LOAD  0x4000u
#define GXM_DEPTH_FORCE_STORE 0x8000u
#define GXM_DEPTH_TILED       0x10000u
#define GXM_DEPTH_STRIDE_SHIFT 17u
#define GXM_DEPTH_D16         0x2444u /* SCE_GXM_DEPTH_STENCIL_FORMAT_D16 >> 12 */
#define GXM_DEPTH_S8D24       0x1266u /* SCE_GXM_DEPTH_STENCIL_FORMAT_S8D24 >> 12 */
typedef struct {
    uint32_t control, depth, stencil;
    float background_depth;
    uint32_t background_control;
} GxmDepthSurface;
typedef struct {
    uint32_t width, height, flags, multisample;
} GxmTarget;
typedef struct {
    bool used;
    uint32_t address; /* the context's host memory, its handle */
    uint32_t vertex_ring, vertex_ring_size, vertex_ring_used;
    uint32_t fragment_ring, fragment_ring_size, fragment_ring_used;
    /* the state draws take */
    uint32_t vertex_program, fragment_program; /* handles */
    uint32_t streams[GXM_STREAMS];
    GxmTexture textures[GXM_TEXTURES]; /* the fragment program's */
    GxmTexture vertex_textures[4];
    uint32_t vertex_uniform[GXM_UNIFORM_BUFFERS], fragment_uniform[GXM_UNIFORM_BUFFERS]; /* addresses */
    uint32_t front_polygon_mode, back_polygon_mode, cull_mode, two_sided;
    uint32_t region_clip_mode, clip_x0, clip_y0, clip_x1, clip_y1;
    bool viewport_enabled;
    float viewport_offset[3], viewport_scale[3];
    /* depth and stencil, front and back */
    uint32_t depth_func, depth_write, stencil_func[2], stencil_ops[2][3], stencil_masks[2][2], stencil_ref[2];
    /* the scene */
    bool in_scene;
    uint32_t target;
    GxmColorSurface color;
    bool has_color;
    GxmDepthSurface depth;
    bool has_depth;
} GxmContext;
typedef struct VitaGxm {
    bool initialized;
    uint32_t notification_region; /* sceGxmGetNotificationRegion's, made at its first call */
    /* the notifications ended scenes name (sceGxmEndScene): each address written with its value once the GPU drew
       the submission of serial, in order */
    struct {
        uint32_t address, value;
        uint64_t serial;
    } notes[GXM_NOTES];
    unsigned note_count;
    uint32_t callback, data_size, max_pending;
    /* the display queue: entries' copies of the game's data in guest memory, oldest first, each with the GPU
       submission it shows the results of */
    uint32_t queue_memory;
    struct {
        uint32_t data;
        uint64_t serial;
    } queue[GXM_DISPLAY_QUEUE];
    uint32_t queue_head, queue_count;
    uint32_t display_thread;
    GxmContext context;
    GxmVertexProgram vertex[GXM_PROGRAMS];
    GxmFragmentProgram fragment[GXM_FRAGMENT_PROGRAMS];
    GxmTarget targets[GXM_TARGETS];
    struct {
        uint32_t address, size;
    } mappings[GXM_MAPPINGS];
    uint64_t scenes, draws;
    struct VitaRender* render; /* render.c's */
} VitaGxm;
#endif
