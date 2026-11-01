/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The 3D and compute engines' registers as the neutral draw state (gpu/draw_state.h), for the Vulkan tests
   (tests/vulkan_headless.c), which describe their draws and dispatches by register values. mx_state.c is the
   translation the runtime's Maxwell front end made before NVN became its only GPU path. The renderer only reads the
   DsState it gives */
#ifndef SNX_TEST_MX_STATE_H
#define SNX_TEST_MX_STATE_H
#include "nx/gpu/draw_state.h"
#include "nx/gpu/shader/shader.h"

/* Groups of the 3D engine's registers, a bit each in MxRegs.r3d_dirty: mx_draw_state_sync makes again what it
   derives from a group whose bit is set. A test that sets registers directly sets every bit */
enum {
    R3D_SHADER = 1u << 0,    /* the programs: code address 0x582-0x583, the slots 0x800-0x85F, early fragment tests
                                0x084 */
    R3D_VATTR = 1u << 1,     /* the vertex attributes 0x458-0x477 */
    R3D_VFORMAT = 1u << 2,   /* the vertex streams' stride, enable, divisor and instancing */
    R3D_RTFORMAT = 1u << 3,  /* RT_CONTROL 0x487, each color target's format (0x204 + 0x10 * i), the MSAA mode 0x574 */
    R3D_BLEND = 1u << 4,     /* blending, color masks, the logic operation */
    R3D_PIPEMISC = 1u << 5,  /* the rest of the pipeline key and the programs' keys */
    R3D_XFB = 1u << 6,       /* transform feedback */
    R3D_TARGETS = 1u << 7,   /* the color and depth targets */
    R3D_DYNAMIC = 1u << 8,   /* what mx_draw_dynamic reads */
    R3D_VIEWPORTS = 1u << 9, /* viewports, scissors, transforms */
    R3D_VBUF = 1u << 10,     /* the vertex streams' addresses and limits */
    R3D_INDEX = 1u << 11,    /* the index buffer, primitive restart */
    R3D_BIND = 1u << 12,     /* the texture bank, the TIC and TSC pools, the capture buffers */
    R3D_CBIND = 1u << 13     /* a constant buffer bound in ds.bind directly, not a register's change */
};
#define MX_METHODS 0x1000 /* a command header's 12-bit method reaches all of them */

/* A test's register state: the 3D engine's (r3d) and the compute engine's (rcomp) registers by method, the draw
   state they give (ds), and the draw owner the renderer keys what it keeps between draws by (owner) */
typedef struct {
    u32 id;
    u32 r3d[MX_METHODS];
    u32 rcomp[MX_METHODS];
    u32 r3d_dirty;
    DsState ds;
    DrawOwner owner;
} MxRegs;

/* regs' draw state (MxRegs.ds) brought up to its registers: before each draw, clear and texture draw */
void mx_draw_state_sync(MxRegs* regs);
/* a draw's dynamic state and viewports from the registers r, in guest units */
void mx_draw_dynamic(const u32* r, DsDynamic* d);
void mx_draw_viewports(const u32* r, DsViewports* v);
/* the groups a pipeline and its programs are keyed by, from the registers r */
void mx_draw_programs(const u32* r, DsPrograms* p);
void mx_draw_vertex_format(const u32* r, DsVertexFormat* f);
void mx_draw_blend(const u32* r, DsBlend* b);
void mx_draw_raster_key(const u32* r, DsRasterKey* k);
void mx_draw_targets(const u32* r, DsTargets* t);
/* the vertex streams and the index buffer */
void mx_draw_vertex_buffers(const u32* r, DsVertexBuffers* b);
void mx_draw_index(const u32* r, DsIndex* x);
/* the bindings' registers into b, leaving its constant buffer slots as they are */
void mx_draw_bindings(const u32* r, DsBindings* b);
/* what transform feedback captures, for programs that DsPrograms.xfb_on says capture */
void mx_draw_xfb(const u32* r, ShXfb* x);
/* a clear (CLEAR_SURFACE's word v) and a texture draw (DRAW_TEXTURE) from the registers r */
void mx_draw_clear(const u32* r, u32 v, DrawClear* c);
void mx_draw_texture(const u32* r, DrawTexture* t);
/* a compute launch from its description q (the QMD) and the compute engine's registers r, its programs at code_loc */
void mx_dispatch_state(const u32* q, const u32* r, u64 code_loc, DispatchState* d);
/* a shader cache invalidation (3D 0x087 and 0x369, compute 0x087 and 0x5A6) with the word v: bit 0, the instruction
   cache, is the renderer's method_barrier kind 5 */
void mx_invalidate_shaders(u32 v);
#endif
