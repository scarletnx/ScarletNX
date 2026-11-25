/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   sceGxm's API as Vita3K's modules/SceGxm/SceGxm.cpp answers it (GPL-2.0-or-later, Copyright (C) 2026 Vita3K team) at
   Vita3K/Vita3K f68b6ee: the context's state, the shader patcher's programs, the uniform buffers reserved from the
   context's rings and their data (sceGxmSetUniformDataF's layout), textures' control words, and the display queue,
   whose callback the game's display thread runs (a guest thread at the runtime's display loop, VITA_DISPLAY_LOOP).
   The drawing is the renderer's (vita/gpu/render.h) */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include "vita/gpu/gxm.h"
#include "vita/gpu/gxp.h"
#include "vita/gpu/render.h"
#include "vita/gpu/usse.h"
#include "snx_settings.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_GXM_ERROR_UNINITIALIZED       0x805b0000u
#define SCE_GXM_ERROR_ALREADY_INITIALIZED 0x805b0001u
#define SCE_GXM_ERROR_OUT_OF_MEMORY       0x805b0002u
#define SCE_GXM_ERROR_INVALID_VALUE       0x805b0003u
#define SCE_GXM_ERROR_INVALID_POINTER     0x805b0004u
#define SCE_GXM_ERROR_INVALID_ALIGNMENT   0x805b0005u
#define SCE_GXM_ERROR_UNSUPPORTED         0x805b0009u
#define SCE_GXM_DEPTH_STENCIL_SURFACE_TILED 0x00011000u
#define SCE_GXM_ERROR_NOT_WITHIN_SCENE    0x805b0006u
#define SCE_GXM_ERROR_WITHIN_SCENE        0x805b0007u
#define SCE_GXM_ERROR_NULL_PROGRAM        0x805b0008u
#define SCE_GXM_ERROR_RESERVE_FAILED      0x805b000bu
#define SCE_GXM_ERROR_INVALID_INDEX_COUNT 0x805b000du
/* SceGxmDepthFunc's always, SceGxmDepthWriteMode's enabled */
#define DEPTH_FUNC_ALWAYS 0x01c00000u

static VitaGxm* gxm(VitaProcess* p) {
    if (!p->gxm) p->gxm = calloc(1, sizeof *p->gxm);
    return p->gxm;
}
static GxmContext* context_of(VitaProcess* p, uint32_t address) {
    VitaGxm* g = p->gxm;
    return g && g->context.used && g->context.address == address ? &g->context : NULL;
}
static GxmVertexProgram* vertex_of(VitaGxm* g, uint32_t handle) {
    uint32_t i = (handle - GXM_HANDLE_VERTEX) / GXM_PROGRAM_HANDLE;
    return handle >= GXM_HANDLE_VERTEX && i < GXM_PROGRAMS && g->vertex[i].references ? &g->vertex[i] : NULL;
}
static GxmFragmentProgram* fragment_of(VitaGxm* g, uint32_t handle) {
    uint32_t i = (handle - GXM_HANDLE_FRAGMENT) / GXM_PROGRAM_HANDLE;
    return handle >= GXM_HANDLE_FRAGMENT && i < GXM_FRAGMENT_PROGRAMS && g->fragment[i].references ? &g->fragment[i] : NULL;
}

/* ---- initialization: the display queue's thread ---- */
/* sceGxmInitialize(params): flags, the display queue's most pending entries, its callback, the size of an entry's data,
   the parameter buffer's size */
uint32_t vita_gxm_initialize(VitaProcess* p, VitaThread* t, uint32_t params) {
    (void)t;
    VitaGxm* g = gxm(p);
    if (!g) return SCE_GXM_ERROR_OUT_OF_MEMORY;
    if (g->initialized) return SCE_GXM_ERROR_ALREADY_INITIALIZED;
    if (!params) return SCE_GXM_ERROR_INVALID_POINTER;
    g->max_pending = vita_read32(p, params + 4);
    g->callback = vita_read32(p, params + 8);
    g->data_size = vita_read32(p, params + 12);
    if (!g->max_pending || g->max_pending > GXM_DISPLAY_QUEUE || g->data_size > 512) return SCE_GXM_ERROR_INVALID_VALUE;
    g->queue_memory = g->data_size ? vita_alloc(p, &p->user, g->max_pending * ((g->data_size + 15) & ~15u), 0x1000) : 0;
    if (g->data_size && !g->queue_memory) return SCE_GXM_ERROR_OUT_OF_MEMORY;
    /* the queue's entries are shown by a thread of the game's process, with the callback's work its own */
    int32_t uid = vita_thread_new(p, "SceGxmDisplayQueue", VITA_DISPLAY_LOOP, 64, 0x10000, 0);
    if (uid < 0) return SCE_GXM_ERROR_OUT_OF_MEMORY;
    g->display_thread = (uint32_t)uid;
    vita_thread_start(p, vita_thread(p, (uint32_t)uid), 0, 0);
    g->initialized = true;
    vita_render_init(p);
    return 0;
}
uint32_t vita_gxm_terminate(VitaProcess* p, VitaThread* t) {
    (void)t;
    VitaGxm* g = p->gxm;
    if (!g || !g->initialized) return SCE_GXM_ERROR_UNINITIALIZED;
    vita_render_idle(p, true);
    return 0;
}
/* the display queue's thread at VITA_SVC_DISPLAY_NEXT: the oldest entry once the GPU has drawn it, r0 its data and r1
   the callback, or a wait for one */
static bool display_ready(VitaProcess* p, VitaThread* t) {
    VitaGxm* g = p->gxm;
    if (!g->queue_count || !vita_render_done(p, g->queue[g->queue_head].serial)) return false;
    t->cpu.r[0] = g->queue[g->queue_head].data;
    t->cpu.r[1] = g->callback;
    return true;
}
void vita_gxm_display_next(VitaProcess* p, VitaThread* t) {
    if (!p->gxm || !display_ready(p, t)) vita_wait(p, (VitaWait){.kind = VITA_WAIT_DISPLAY});
}
void vita_gxm_display_done(VitaProcess* p, VitaThread* t) {
    (void)t;
    VitaGxm* g = p->gxm;
    if (!g || !g->queue_count) return;
    g->queue_head = (g->queue_head + 1) % g->max_pending;
    --g->queue_count;
}
/* a frame on its way to the screen that the next vblank may wait for (VBLANK_SLACK_NS in run.c): a queued scene the GPU
   still draws, or one drawn whose display callback has not yet reached its wait for the vblank */
bool vita_gxm_frame_coming(VitaProcess* p) {
    VitaGxm* g = p->gxm;
    if (!g || !g->initialized || !g->queue_count) return false;
    const VitaThread* d = vita_thread(p, g->display_thread);
    return !(d && d->state == VITA_THREAD_WAITING && d->wait.kind == VITA_WAIT_VBLANK);
}
/* the notifications whose scenes the GPU drew written, oldest first */
static void notifications_poll(VitaProcess* p) {
    VitaGxm* g = p->gxm;
    unsigned done = 0;
    while (done < g->note_count && vita_render_done(p, g->notes[done].serial)) {
        vita_write32(p, g->notes[done].address, g->notes[done].value);
        ++done;
    }
    if (!done) return;
    g->note_count -= done;
    memmove(g->notes, g->notes + done, g->note_count * sizeof *g->notes);
}
void vita_gxm_poll(VitaProcess* p) {
    if (p->gxm) notifications_poll(p);
}
/* VITA_WAIT_GXM's a: 0 the GPU idle (sceGxmFinish), 1 the display queue empty (sceGxmDisplayQueueFinish), 2 room in
   it (sceGxmDisplayQueueAddEntry, which calls again: its r0 kept) */
bool vita_gxm_wait_try(VitaProcess* p, VitaThread* t, uint64_t now) {
    (void)now;
    VitaGxm* g = p->gxm;
    if (!g) return true;
    notifications_poll(p);
    if (t->wait.kind == VITA_WAIT_DISPLAY) return display_ready(p, t);
    switch (t->wait.a) {
        case 0:
            if (!vita_render_idle(p, false)) return false;
            break;
        case 1:
            if (g->queue_count) return false;
            break;
        default: return g->queue_count < g->max_pending;
    }
    t->cpu.r[0] = 0;
    return true;
}
/* sceGxmDisplayQueueAddEntry(oldBuffer, newBuffer, callbackData): the data copied for the callback, which runs once
   the scenes submitted so far are drawn. A full queue waits for room and adds it then */
uint32_t vita_gxm_display_queue_add_entry(VitaProcess* p, VitaThread* t, uint32_t old_buffer, uint32_t new_buffer,
                                          uint32_t data) {
    (void)old_buffer, (void)new_buffer;
    VitaGxm* g = p->gxm;
    if (!g || !g->initialized) return SCE_GXM_ERROR_UNINITIALIZED;
    if (g->queue_count == g->max_pending) {
        t->cpu.r[15] -= t->cpu.cpsr & SNX_ARM_T ? 2 : 4; /* the SVC again once there is room */
        vita_wait(p, (VitaWait){.kind = VITA_WAIT_GXM, .a = 2});
        return 0;
    }
    uint32_t slot = (g->queue_head + g->queue_count) % g->max_pending;
    uint32_t copy = g->queue_memory + slot * ((g->data_size + 15) & ~15u);
    if (g->data_size && data) memcpy(vita_ptr(p, copy), vita_ptr(p, data), g->data_size);
    g->queue[slot].data = copy;
    g->queue[slot].serial = vita_render_submitted(p);
    ++g->queue_count;
    if (g_settings.vita_gxm_trace) printf("gxm: display queue entry %u (data 0x%08" PRIx32 ")\n", (unsigned)slot, data);
    return 0;
}
uint32_t vita_gxm_display_queue_finish(VitaProcess* p, VitaThread* t) {
    (void)t;
    VitaGxm* g = p->gxm;
    if (!g || !g->initialized) return SCE_GXM_ERROR_UNINITIALIZED;
    if (g->queue_count) vita_wait(p, (VitaWait){.kind = VITA_WAIT_GXM, .a = 1});
    return 0;
}
uint32_t vita_gxm_finish(VitaProcess* p, VitaThread* t) {
    (void)t;
    vita_render_finish(p);
    if (p->gxm && !vita_render_idle(p, false)) vita_wait(p, (VitaWait){.kind = VITA_WAIT_GXM, .a = 0});
    return 0;
}
uint32_t vita_gxm_ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

/* ---- contexts ---- */
/* sceGxmCreateContext(params, SceGxmContext**): the host memory (the context's handle), the VDM, vertex, fragment
   and fragment USSE rings */
uint32_t vita_gxm_create_context(VitaProcess* p, VitaThread* t, uint32_t params, uint32_t out) {
    (void)t;
    VitaGxm* g = gxm(p);
    if (!g || !g->initialized) return SCE_GXM_ERROR_UNINITIALIZED;
    if (!params || !out) return SCE_GXM_ERROR_INVALID_POINTER;
    if (g->context.used) return SCE_GXM_ERROR_OUT_OF_MEMORY; /* one immediate context, as the console has */
    GxmContext* c = &g->context;
    memset(c, 0, sizeof *c);
    c->used = true;
    c->address = vita_read32(p, params);
    c->vertex_ring = vita_read32(p, params + 16), c->vertex_ring_size = vita_read32(p, params + 20);
    c->fragment_ring = vita_read32(p, params + 24), c->fragment_ring_size = vita_read32(p, params + 28);
    c->depth_func = DEPTH_FUNC_ALWAYS;
    c->depth_write = 0;
    for (unsigned side = 0; side < 2; ++side) {
        c->stencil_func[side] = 0x0e000000u; /* SCE_GXM_STENCIL_FUNC_ALWAYS */
        c->stencil_masks[side][0] = c->stencil_masks[side][1] = 0xff;
    }
    vita_write32(p, out, c->address);
    return 0;
}
uint32_t vita_gxm_destroy_context(VitaProcess* p, VitaThread* t, uint32_t context) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_render_idle(p, true);
    c->used = false;
    return 0;
}

/* ---- render targets, surfaces, sync objects ---- */
/* SceGxmRenderTargetParams: flags, width and height (16 bits), scenes per frame and the multisample mode (16 bits),
   the sample locations, the driver's memory block */
uint32_t vita_gxm_create_render_target(VitaProcess* p, VitaThread* t, uint32_t params, uint32_t out) {
    (void)t;
    VitaGxm* g = gxm(p);
    if (!g || !params || !out) return SCE_GXM_ERROR_INVALID_POINTER;
    for (uint32_t i = 0; i < GXM_TARGETS; ++i) {
        GxmTarget* r = &g->targets[i];
        if (r->width) continue;
        uint32_t size = vita_read32(p, params + 4), modes = vita_read32(p, params + 8);
        r->flags = vita_read32(p, params);
        r->width = size & 0xffff, r->height = size >> 16, r->multisample = modes >> 16;
        if (!r->width || !r->height) {
            *r = (GxmTarget){0};
            return SCE_GXM_ERROR_INVALID_VALUE;
        }
        vita_write32(p, out, GXM_HANDLE_TARGET + i * 16);
        return 0;
    }
    return SCE_GXM_ERROR_OUT_OF_MEMORY;
}
uint32_t vita_gxm_destroy_render_target(VitaProcess* p, VitaThread* t, uint32_t target) {
    (void)t;
    uint32_t i = (target - GXM_HANDLE_TARGET) / 16;
    if (!p->gxm || i >= GXM_TARGETS) return SCE_GXM_ERROR_INVALID_POINTER;
    p->gxm->targets[i] = (GxmTarget){0};
    return 0;
}
/* sceGxmColorSurfaceInit(surface, format, type, scale mode, output register size, width, height, stride, data): the
   words GxmColorSurface names, the background texture after them cleared */
uint32_t vita_gxm_color_surface_init(VitaProcess* p, VitaThread* t, uint32_t surface, uint32_t format, uint32_t type,
                                     uint32_t mode, uint32_t size, uint32_t width, uint32_t height, uint32_t stride,
                                     uint32_t data) {
    (void)t;
    if (!surface || !vita_mapped(p, surface, 12 * 4)) return SCE_GXM_ERROR_INVALID_POINTER;
    if (!width || !height || width > 4096 || height > 4096 || stride < width) return SCE_GXM_ERROR_INVALID_VALUE;
    if (data & 3) return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    uint32_t words[12] = {mode /* scale mode: downscale */, width, height, stride, data, format,
                          type, size};
    for (unsigned i = 0; i < 12; ++i) vita_write32(p, surface + i * 4, words[i]);
    return 0;
}
/* sceGxmDepthStencilSurfaceInit(surface, format, type, stride, depth data, stencil data): the control word as the
   runtime keeps it (gxm.h, GXM_DEPTH_*) */
uint32_t vita_gxm_depth_stencil_surface_init(VitaProcess* p, VitaThread* t, uint32_t surface, uint32_t format,
                                             uint32_t type, uint32_t stride, uint32_t data, uint32_t value) {
    (void)t;
    if (!surface) return SCE_GXM_ERROR_INVALID_POINTER;
    if (stride > 0x7fffu) return SCE_GXM_ERROR_INVALID_VALUE;
    uint32_t control = (format >> 12 & GXM_DEPTH_FORMAT_MASK) | stride << GXM_DEPTH_STRIDE_SHIFT |
                       (type == SCE_GXM_DEPTH_STENCIL_SURFACE_TILED ? GXM_DEPTH_TILED : 0);
    vita_write32(p, surface, control);
    vita_write32(p, surface + 4, data);
    vita_write32(p, surface + 8, value);
    vita_write32(p, surface + 12, 0x3f800000u); /* the background depth, 1.0 */
    vita_write32(p, surface + 16, 0);
    return 0;
}
/* sceGxmDepthStencilSurfaceSetForceLoadMode and SetForceStoreMode(surface, mode): whether each scene starts with the
   depth in memory, and leaves its depth there */
static uint32_t depth_set_force(VitaProcess* p, uint32_t surface, uint32_t enable, uint32_t bit) {
    if (!surface) return SCE_GXM_ERROR_INVALID_POINTER;
    uint32_t control = vita_read32(p, surface) & ~bit;
    vita_write32(p, surface, control | (enable ? bit : 0));
    return 0;
}
uint32_t vita_gxm_depth_set_force_load(VitaProcess* p, VitaThread* t, uint32_t surface, uint32_t enable) {
    (void)t;
    return depth_set_force(p, surface, enable, GXM_DEPTH_FORCE_LOAD);
}
uint32_t vita_gxm_depth_set_force_store(VitaProcess* p, VitaThread* t, uint32_t surface, uint32_t enable) {
    (void)t;
    return depth_set_force(p, surface, enable, GXM_DEPTH_FORCE_STORE);
}
uint32_t vita_gxm_sync_object_create(VitaProcess* p, VitaThread* t, uint32_t out) {
    (void)t;
    if (!out) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_write32(p, out, GXM_HANDLE_SYNC);
    return 0;
}

/* ---- the memory the GPU reads and writes ---- */
uint32_t vita_gxm_map_memory(VitaProcess* p, VitaThread* t, uint32_t base, uint32_t size) {
    (void)t;
    VitaGxm* g = gxm(p);
    if (!g || !base) return SCE_GXM_ERROR_INVALID_POINTER;
    if ((base | size) & 0xfff) return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    if (!size || !vita_mapped(p, base, size)) return SCE_GXM_ERROR_INVALID_VALUE;
    for (uint32_t i = 0; i < GXM_MAPPINGS; ++i)
        if (!g->mappings[i].size) {
            g->mappings[i].address = base, g->mappings[i].size = size;
            vita_render_map(p, base, size);
            return 0;
        }
    return SCE_GXM_ERROR_OUT_OF_MEMORY;
}
uint32_t vita_gxm_unmap_memory(VitaProcess* p, VitaThread* t, uint32_t base) {
    (void)t;
    VitaGxm* g = p->gxm;
    for (uint32_t i = 0; g && i < GXM_MAPPINGS; ++i)
        if (g->mappings[i].size && g->mappings[i].address == base) {
            vita_render_idle(p, true);
            vita_render_unmap(p, base);
            g->mappings[i].size = 0;
            return 0;
        }
    return SCE_GXM_ERROR_INVALID_VALUE;
}
/* sceGxmMapVertexUsseMemory and sceGxmMapFragmentUsseMemory(base, size, *offset): the programs' code, which the
   runtime reads from their GXPs: the offset is the address */
uint32_t vita_gxm_map_usse_memory(VitaProcess* p, VitaThread* t, uint32_t base, uint32_t size, uint32_t offset) {
    (void)t, (void)size;
    if (!base || !offset) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_write32(p, offset, base);
    return 0;
}

/* ---- the shader patcher and programs ---- */
uint32_t vita_gxm_shader_patcher_create(VitaProcess* p, VitaThread* t, uint32_t params, uint32_t out) {
    (void)t;
    if (!params || !out) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_write32(p, out, GXM_HANDLE_PATCHER);
    return 0;
}
/* sceGxmShaderPatcherRegisterProgram(patcher, program, *id): the program's address is its ID */
uint32_t vita_gxm_register_program(VitaProcess* p, VitaThread* t, uint32_t patcher, uint32_t program, uint32_t id) {
    (void)t, (void)patcher;
    if (!program || !id) return SCE_GXM_ERROR_INVALID_POINTER;
    if (vita_read32(p, program) != GXP_MAGIC) return SCE_GXM_ERROR_INVALID_VALUE;
    /* the corpus has each registered program, with no hints until it is drawn */
    if (g_settings.vita_shader_corpus[0]) {
        UsseHints none = {.attribute_count = 0};
        usse_corpus_write(g_settings.vita_shader_corpus, vita_ptr(p, program), vita_read32(p, program + 8), &none);
    }
    vita_write32(p, id, program);
    return 0;
}
/* sceGxmShaderPatcherGetProgramFromId(id): the program's address is its ID (register_program), so it is the answer */
uint32_t vita_gxm_get_program_from_id(VitaProcess* p, VitaThread* t, uint32_t id) {
    (void)p, (void)t;
    return id;
}
/* sceGxmShaderPatcherCreateVertexProgram(patcher, id, attributes, count, streams, count, *program) */
uint32_t vita_gxm_create_vertex_program(VitaProcess* p, VitaThread* t, uint32_t patcher, uint32_t program,
                                        uint32_t attributes, uint32_t attribute_count, uint32_t streams,
                                        uint32_t stream_count, uint32_t out) {
    (void)t, (void)patcher;
    VitaGxm* g = gxm(p);
    if (!g || !program || !out) return SCE_GXM_ERROR_INVALID_POINTER;
    if (attribute_count > GXM_ATTRIBUTES || stream_count > GXM_STREAMS) return SCE_GXM_ERROR_INVALID_VALUE;
    GxmVertexProgram want = {.references = 1, .program = program};
    want.attribute_count = attribute_count, want.stream_count = stream_count;
    for (uint32_t a = 0; a < attribute_count; ++a) {
        const unsigned char* q = vita_ptr(p, attributes + a * 8);
        want.attributes[a] = (GxmAttribute){snx_ld_le16(q), snx_ld_le16(q + 2), q[4], q[5], snx_ld_le16(q + 6)};
    }
    for (uint32_t s = 0; s < stream_count; ++s) {
        const unsigned char* q = vita_ptr(p, streams + s * 4);
        want.streams[s] = (GxmStream){snx_ld_le16(q), snx_ld_le16(q + 2)};
    }
    uint32_t slot = GXM_PROGRAMS;
    for (uint32_t i = 0; i < GXM_PROGRAMS; ++i) {
        GxmVertexProgram* v = &g->vertex[i];
        if (!v->references) {
            if (slot == GXM_PROGRAMS) slot = i;
            continue;
        }
        if (v->program == program && v->attribute_count == attribute_count && v->stream_count == stream_count &&
            !memcmp(v->attributes, want.attributes, sizeof want.attributes) &&
            !memcmp(v->streams, want.streams, sizeof want.streams)) {
            ++v->references;
            vita_write32(p, out, GXM_HANDLE_VERTEX + i * GXM_PROGRAM_HANDLE);
            return 0;
        }
    }
    if (slot == GXM_PROGRAMS) {
        /* silent on real hardware too (SCE_GXM_ERROR_NULL_PROGRAM on every draw with no handle): the one-time flag
           below is this codebase's own, so a trace run always shows it even without --vita_gxm_trace=1 */
        static bool warned;
        if (!warned) warned = true, printf("gxm: the vertex program table (%u) is full\n", (unsigned)GXM_PROGRAMS);
        return SCE_GXM_ERROR_OUT_OF_MEMORY;
    }
    g->vertex[slot] = want;
    vita_write32(p, out, GXM_HANDLE_VERTEX + slot * GXM_PROGRAM_HANDLE);
    return 0;
}
/* sceGxmShaderPatcherCreateFragmentProgram(patcher, id, output format, multisample mode, blend info, vertex program,
   *program) */
uint32_t vita_gxm_create_fragment_program(VitaProcess* p, VitaThread* t, uint32_t patcher, uint32_t program,
                                          uint32_t format, uint32_t mode, uint32_t blend, uint32_t vertex_program,
                                          uint32_t out) {
    (void)t, (void)patcher;
    VitaGxm* g = gxm(p);
    if (!g || !program || !out) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmFragmentProgram want = {.references = 1,
                               .blend = blend != 0,
                               .program = program,
                               .vertex_program = vertex_program,
                               .output_format = format,
                               .multisample = mode,
                               .blend_info = blend ? vita_read32(p, blend) : 0xf}; /* all channels written */
    uint32_t slot = GXM_FRAGMENT_PROGRAMS;
    for (uint32_t i = 0; i < GXM_FRAGMENT_PROGRAMS; ++i) {
        GxmFragmentProgram* f = &g->fragment[i];
        if (!f->references) {
            if (slot == GXM_FRAGMENT_PROGRAMS) slot = i;
            continue;
        }
        if (f->program == program && f->vertex_program == want.vertex_program &&
            f->output_format == want.output_format && f->multisample == want.multisample && f->blend == want.blend &&
            f->blend_info == want.blend_info) {
            ++f->references;
            vita_write32(p, out, GXM_HANDLE_FRAGMENT + i * GXM_PROGRAM_HANDLE);
            return 0;
        }
    }
    if (slot == GXM_FRAGMENT_PROGRAMS) {
        static bool warned;
        if (!warned)
            warned = true, printf("gxm: the fragment program table (%u) is full\n", (unsigned)GXM_FRAGMENT_PROGRAMS);
        return SCE_GXM_ERROR_OUT_OF_MEMORY;
    }
    g->fragment[slot] = want;
    vita_write32(p, out, GXM_HANDLE_FRAGMENT + slot * GXM_PROGRAM_HANDLE);
    return 0;
}
uint32_t vita_gxm_release_vertex_program(VitaProcess* p, VitaThread* t, uint32_t patcher, uint32_t handle) {
    (void)t, (void)patcher;
    GxmVertexProgram* v = p->gxm ? vertex_of(p->gxm, handle) : NULL;
    if (!v) return SCE_GXM_ERROR_INVALID_POINTER;
    if (--v->references) return 0;
    vita_render_idle(p, true);
    vita_render_shader_free(p, v->shader);
    v->shader = NULL;
    return 0;
}
uint32_t vita_gxm_release_fragment_program(VitaProcess* p, VitaThread* t, uint32_t patcher, uint32_t handle) {
    (void)t, (void)patcher;
    GxmFragmentProgram* f = p->gxm ? fragment_of(p->gxm, handle) : NULL;
    if (!f) return SCE_GXM_ERROR_INVALID_POINTER;
    if (--f->references) return 0;
    vita_render_idle(p, true);
    vita_render_shader_free(p, f->shader);
    f->shader = NULL;
    return 0;
}
uint32_t vita_gxm_add_ref_fragment_program(VitaProcess* p, VitaThread* t, uint32_t patcher, uint32_t handle) {
    (void)t;
    GxmFragmentProgram* f = p->gxm ? fragment_of(p->gxm, handle) : NULL;
    if (patcher != GXM_HANDLE_PATCHER || !f) return SCE_GXM_ERROR_INVALID_POINTER;
    ++f->references;
    return 0;
}
uint32_t vita_gxm_program_check(VitaProcess* p, VitaThread* t, uint32_t program) {
    (void)t;
    if (!program) return SCE_GXM_ERROR_INVALID_POINTER;
    return vita_read32(p, program) == GXP_MAGIC ? 0 : SCE_GXM_ERROR_INVALID_VALUE;
}
/* sceGxmProgramFindParameterByName(program, name): the parameter's address in the program, NULL for none */
uint32_t vita_gxm_program_find_parameter_by_name(VitaProcess* p, VitaThread* t, uint32_t program, uint32_t address) {
    (void)t;
    char name[256];
    if (!program || !vita_string(p, address, name, sizeof name)) return 0;
    const unsigned char* gxp = vita_ptr(p, program);
    uint32_t count = snx_ld_le32(gxp + GXP_PARAMETER_COUNT);
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* q = gxp_parameter(gxp, i);
        if (!strcmp(gxp_parameter_name(q), name)) return program + (uint32_t)(q - gxp);
    }
    return 0;
}
uint32_t vita_gxm_parameter_get_resource_index(VitaProcess* p, VitaThread* t, uint32_t parameter) {
    (void)t;
    return parameter ? gxp_parameter_resource(vita_ptr(p, parameter)) : 0;
}

/* ---- scenes and draws ---- */
/* sceGxmBeginScene(context, flags, render target, valid region, vertex sync, fragment sync, color surface, depth) */
uint32_t vita_gxm_begin_scene(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t flags, uint32_t target,
                              uint32_t valid_region, uint32_t vertex_sync, uint32_t fragment_sync, uint32_t color,
                              uint32_t depth) {
    (void)t, (void)flags, (void)vertex_sync, (void)fragment_sync;
    GxmContext* c = context_of(p, context);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    if (c->in_scene) return SCE_GXM_ERROR_WITHIN_SCENE;
    uint32_t index = (target - GXM_HANDLE_TARGET) / 16;
    if (target < GXM_HANDLE_TARGET || index >= GXM_TARGETS || !p->gxm->targets[index].width)
        return SCE_GXM_ERROR_INVALID_POINTER;
    c->target = index;
    c->has_color = color && vita_read32(p, color + 16); /* a disabled surface has no data */
    if (color) {
        uint32_t w[8];
        for (unsigned i = 0; i < 8; ++i) w[i] = vita_read32(p, color + i * 4);
        c->color = (GxmColorSurface){w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]};
    }
    c->has_depth = depth != 0;
    if (depth) {
        c->depth.control = vita_read32(p, depth);
        c->depth.depth = vita_read32(p, depth + 4);
        c->depth.stencil = vita_read32(p, depth + 8);
        uint32_t bits = vita_read32(p, depth + 12);
        memcpy(&c->depth.background_depth, &bits, 4);
        c->depth.background_control = vita_read32(p, depth + 16);
    }
    /* sceGxmSetDefaultRegionClipAndViewport over the valid region (xMax, yMax), else the whole target: everything
       outside clipped, the viewport mapping [-1, 1] onto it with y down and z onto [0, 1] */
    const GxmTarget* r = &p->gxm->targets[index];
    uint32_t region = valid_region, x1 = region ? vita_read32(p, region) : r->width - 1;
    uint32_t y1 = region ? vita_read32(p, region + 4) : r->height - 1;
    c->region_clip_mode = 0x80000000u; /* SCE_GXM_REGION_CLIP_OUTSIDE */
    c->clip_x0 = c->clip_y0 = 0, c->clip_x1 = x1, c->clip_y1 = y1;
    c->viewport_enabled = true;
    c->viewport_offset[0] = 0.5f * (1.0f + (float)x1), c->viewport_scale[0] = 0.5f * (1.0f + (float)x1);
    c->viewport_offset[1] = 0.5f * (1.0f + (float)y1), c->viewport_scale[1] = -0.5f * (1.0f + (float)y1);
    c->viewport_offset[2] = 0.5f, c->viewport_scale[2] = 0.5f;
    c->in_scene = true;
    ++p->gxm->scenes;
    if (g_settings.vita_gxm_trace)
        printf("gxm: scene %" PRIu64 " on %ux%u, color 0x%08" PRIx32 " format 0x%08" PRIx32 " stride %u type 0x%08" PRIx32
               " scale %u, depth 0x%08" PRIx32 " control 0x%08" PRIx32 "\n",
               p->gxm->scenes, (unsigned)r->width, (unsigned)r->height, c->has_color ? c->color.data : 0,
               c->has_color ? c->color.format : 0, c->has_color ? (unsigned)c->color.stride : 0,
               c->has_color ? c->color.type : 0, c->has_color ? (unsigned)c->color.flags : 0,
               c->has_depth ? c->depth.depth : 0, c->has_depth ? c->depth.control : 0);
    vita_render_begin_scene(p, c);
    return 0;
}
/* sceGxmEndScene(context, vertex notification, fragment notification): each written with its value once the scene is
   drawn */
uint32_t vita_gxm_end_scene(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t vertex_notification,
                            uint32_t fragment_notification) {
    (void)t;
    GxmContext* c = context_of(p, context);
    uint32_t notifications[2] = {vertex_notification, fragment_notification};
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    if (!c->in_scene) return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    c->in_scene = false;
    uint64_t serial = vita_render_end_scene(p, c);
    VitaGxm* g = p->gxm;
    for (unsigned i = 0; i < 2; ++i) {
        uint32_t address = notifications[i] ? vita_read32(p, notifications[i]) : 0;
        if (!address) continue;
        if (g->note_count == GXM_NOTES) { /* the oldest drawn first */
            vita_render_wait(p, g->notes[0].serial);
            notifications_poll(p);
        }
        g->notes[g->note_count].address = address, g->notes[g->note_count].value = vita_read32(p, notifications[i] + 4);
        g->notes[g->note_count++].serial = serial;
    }
    notifications_poll(p);
    return 0;
}
uint32_t vita_gxm_set_vertex_program(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t vertex_program) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->vertex_program = vertex_program;
    return 0;
}
uint32_t vita_gxm_set_fragment_program(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t fragment_program) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->fragment_program = fragment_program;
    return 0;
}
uint32_t vita_gxm_set_vertex_stream(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t index, uint32_t data) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    if (index >= GXM_STREAMS) return SCE_GXM_ERROR_INVALID_VALUE;
    c->streams[index] = data;
    return 0;
}
static uint32_t set_texture(VitaProcess* p, uint32_t context, uint32_t index, uint32_t texture, bool vertex) {
    GxmContext* c = context_of(p, context);
    if (!c || !texture) return SCE_GXM_ERROR_INVALID_POINTER;
    if (index >= (vertex ? 4u : GXM_TEXTURES)) return SCE_GXM_ERROR_INVALID_VALUE;
    GxmTexture* to = vertex ? &c->vertex_textures[index] : &c->textures[index];
    for (unsigned i = 0; i < 4; ++i) to->w[i] = vita_read32(p, texture + i * 4);
    return 0;
}
uint32_t vita_gxm_set_fragment_texture(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t index,
                                       uint32_t texture) {
    (void)t;
    return set_texture(p, context, index, texture, false);
}
uint32_t vita_gxm_set_vertex_texture(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t index,
                                     uint32_t texture) {
    (void)t;
    return set_texture(p, context, index, texture, true);
}
uint32_t vita_gxm_set_front_polygon_mode(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t front_polygon_mode) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->front_polygon_mode = front_polygon_mode;
    return 0;
}
uint32_t vita_gxm_set_back_polygon_mode(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t back_polygon_mode) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->back_polygon_mode = back_polygon_mode;
    return 0;
}
uint32_t vita_gxm_set_cull_mode(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t cull_mode) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->cull_mode = cull_mode;
    return 0;
}
/* sceGxmSetTwoSidedEnable(context, mode): kept in the context's state, as Vita3K's context->state.two_sided */
uint32_t vita_gxm_set_two_sided_enable(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t mode) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->two_sided = mode;
    return 0;
}
/* sceGxmSetVisibilityBuffer(context, base, stridePerCore): with no occlusion queries drawn, every primitive counts as
   visible, so the buffer is filled with ones for all four GPU cores (Vita3K's fallback, "Set all visible") */
uint32_t vita_gxm_set_visibility_buffer(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t base,
                                        uint32_t stride) {
    (void)t;
    if (!context_of(p, context)) return SCE_GXM_ERROR_INVALID_POINTER;
    if (base && stride && stride <= 0x100000u && vita_mapped(p, base, 4 * stride)) memset(vita_ptr(p, base), 0xff, 4 * stride);
    return 0;
}
/* sceGxmSetRegionClip(context, mode, xMin, yMin, xMax, yMax) */
uint32_t vita_gxm_set_region_clip(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t mode, uint32_t x_min,
                                  uint32_t y_min, uint32_t x_max, uint32_t y_max) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    c->region_clip_mode = mode;
    c->clip_x0 = x_min, c->clip_y0 = y_min, c->clip_x1 = x_max, c->clip_y1 = y_max;
    return 0;
}
/* sceGxmSetViewport(context, xOffset, xScale, yOffset, yScale, zOffset, zScale): the floats in S0 to S5, as the
   console's hard-float ABI passes them (a game's call loads them there) */
uint32_t vita_gxm_set_viewport(VitaProcess* p, VitaThread* t, uint32_t context, float x_offset, float x_scale,
                               float y_offset, float y_scale, float z_offset, float z_scale) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    c->viewport_offset[0] = x_offset, c->viewport_scale[0] = x_scale;
    c->viewport_offset[1] = y_offset, c->viewport_scale[1] = y_scale;
    c->viewport_offset[2] = z_offset, c->viewport_scale[2] = z_scale;
    c->viewport_enabled = true;
    return 0;
}
uint32_t vita_gxm_set_viewport_enable(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t viewport_enabled) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->viewport_enabled = viewport_enabled == 0; /* SCE_GXM_VIEWPORT_ENABLED is 0 */
    return 0;
}
uint32_t vita_gxm_set_depth_func(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t depth_func) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->depth_func = depth_func;
    return 0;
}
uint32_t vita_gxm_set_depth_write_enable(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t depth_write) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->depth_write = depth_write;
    return 0;
}
/* a default uniform buffer from the context's ring, for the program's default uniforms (its GXP's count of 32-bit
   words): the next aligned room, from the ring's start when the rest is too small */
static uint32_t reserve(VitaProcess* p, uint32_t context, uint32_t out, bool fragment) {
    GxmContext* c = context_of(p, context);
    if (!c || !out) return SCE_GXM_ERROR_INVALID_POINTER;
    uint32_t handle = fragment ? c->fragment_program : c->vertex_program, gxp = 0;
    if (fragment) {
        GxmFragmentProgram* f = fragment_of(p->gxm, handle);
        gxp = f ? f->program : 0;
    } else {
        GxmVertexProgram* v = vertex_of(p->gxm, handle);
        gxp = v ? v->program : 0;
    }
    if (!gxp) return SCE_GXM_ERROR_NULL_PROGRAM;
    uint32_t size = snx_ld_le32(vita_ptr(p, gxp + GXP_DEFAULT_UNIFORM_COUNT)) * 4;
    uint32_t ring = fragment ? c->fragment_ring : c->vertex_ring, ring_size = fragment ? c->fragment_ring_size
                                                                                      : c->vertex_ring_size;
    uint32_t* used = fragment ? &c->fragment_ring_used : &c->vertex_ring_used;
    uint32_t* slot = fragment ? &c->fragment_uniform[GXP_DEFAULT_CONTAINER] : &c->vertex_uniform[GXP_DEFAULT_CONTAINER];
    if (!size) {
        *slot = 0;
        vita_write32(p, out, 0);
        return 0;
    }
    *used = (*used + 15) & ~15u;
    if (size > ring_size) return SCE_GXM_ERROR_RESERVE_FAILED;
    if (*used + size > ring_size) *used = 0;
    *slot = ring + *used;
    *used += size;
    vita_write32(p, out, *slot);
    return 0;
}
uint32_t vita_gxm_reserve_vertex(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t out) {
    (void)t;
    return reserve(p, context, out, false);
}
uint32_t vita_gxm_reserve_fragment(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t out) {
    (void)t;
    return reserve(p, context, out, true);
}
static uint16_t half_of(float value) {
    uint32_t bits;
    memcpy(&bits, &value, 4);
    uint32_t sign = (bits >> 16) & 0x8000u, exponent = (bits >> 23) & 0xff, mantissa = bits & 0x7fffffu;
    if (exponent == 0xff) return (uint16_t)(sign | 0x7c00u | (mantissa ? 0x200u : 0));
    int e = (int)exponent - 127 + 15;
    if (e >= 31) return (uint16_t)(sign | 0x7c00u);
    if (e <= 0) {
        if (e < -10) return (uint16_t)sign;
        mantissa |= 0x800000u;
        uint32_t shift = (uint32_t)(14 - e), half = mantissa >> shift, rest = mantissa & ((1u << shift) - 1);
        if (rest > (1u << (shift - 1)) || (rest == (1u << (shift - 1)) && (half & 1))) ++half;
        return (uint16_t)(sign | half);
    }
    uint32_t half = sign | (uint32_t)e << 10 | mantissa >> 13, rest = mantissa & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (half & 1))) ++half;
    return (uint16_t)half;
}
/* one component in the parameter's type, from a float */
static void put_component(unsigned char* to, unsigned type, float value) {
    uint32_t bits = 0;
    switch (type) {
        case GXP_TYPE_F32: memcpy(&bits, &value, 4); break;
        case GXP_TYPE_F16: bits = half_of(value); break;
        case GXP_TYPE_U32: bits = (uint32_t)value; break;
        case GXP_TYPE_S32: bits = (uint32_t)(int32_t)value; break;
        case GXP_TYPE_U16: bits = (uint16_t)value; break;
        case GXP_TYPE_S16: bits = (uint16_t)(int16_t)value; break;
        case GXP_TYPE_U8: bits = (uint8_t)value; break;
        case GXP_TYPE_S8: bits = (uint8_t)(int8_t)value; break;
        default: memcpy(&bits, &value, 4); break;
    }
    for (unsigned i = 0; i < gxp_type_size(type); ++i) to[i] = (unsigned char)(bits >> (i * 8));
}
/* sceGxmSetUniformDataF(buffer, parameter, componentOffset, componentCount, data): the components from the
   parameter's resource index, each in its type. An array of vectors keeps each element aligned: 8 bytes for floating
   point, 4 otherwise. A scalar array, or one vector, is packed in 32-bit units / its components' size */
uint32_t vita_gxm_set_uniform_data_f(VitaProcess* p, VitaThread* t, uint32_t buffer, uint32_t parameter,
                                     uint32_t offset, uint32_t count, uint32_t data) {
    (void)t;
    if (!buffer || !parameter || !data) return SCE_GXM_ERROR_INVALID_POINTER;
    const unsigned char* q = vita_ptr(p, parameter);
    if (gxp_parameter_category(q) != GXP_CATEGORY_UNIFORM) return SCE_GXM_ERROR_INVALID_VALUE;
    unsigned type = gxp_parameter_type(q), size = gxp_type_size(type), components = gxp_parameter_components(q);
    unsigned char* base = (unsigned char*)vita_ptr(p, buffer) + gxp_parameter_resource(q) * 4;
    bool is_float = type == GXP_TYPE_F32 || type == GXP_TYPE_F16;
    if (gxp_parameter_array_size(q) == 1 || components == 1) {
        if (components == 1) size = 4;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
        if (type == GXP_TYPE_F32 && size == 4) {
            memcpy(base + offset * 4, vita_ptr(p, data), count * 4);
            return 0;
        }
#endif
        for (uint32_t i = 0; i < count; ++i)
            put_component(base + (offset + i) * size, components == 1 && type != GXP_TYPE_F32 ? GXP_TYPE_U32 : type,
                          gxp_f32(vita_ptr(p, data + i * 4)));
        return 0;
    }
    unsigned element = components * size, padded = element;
    padded = is_float ? (element + 7) & ~7u : (element + 3) & ~3u;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    if (type == GXP_TYPE_F32) {
        for (uint32_t i = 0; i < count;) {
            uint32_t at = offset + i, vector = at / components, component = at % components, run = components - component;
            if (run > count - i) run = count - i;
            memcpy(base + vector * padded + component * 4, vita_ptr(p, data + i * 4), run * 4);
            i += run;
        }
        return 0;
    }
#endif
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = offset + i, vector = at / components, component = at % components;
        put_component(base + vector * padded + component * size, type, gxp_f32(vita_ptr(p, data + i * 4)));
    }
    return 0;
}
/* sceGxmDraw(context, primitive type, index format, indices, count) */
uint32_t vita_gxm_draw(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t type, uint32_t format,
                       uint32_t indices, uint32_t count) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    if (!c->in_scene) return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    if (!vertex_of(p->gxm, c->vertex_program) || !fragment_of(p->gxm, c->fragment_program))
        return SCE_GXM_ERROR_NULL_PROGRAM;
    if (!count) return SCE_GXM_ERROR_INVALID_INDEX_COUNT;
    ++p->gxm->draws;
    vita_render_draw(p, c, type, format, indices, count, 1);
    return 0;
}

/* ---- precomputed draws and fragment states: the state's words (program handle, extra data) and what the extra data
   holds (a draw's vertex streams, a fragment state's uniform buffers), all in guest memory ---- */
enum { PD_PROGRAM = 0, PD_EXTRA = 4, PD_TYPE = 8, PD_FORMAT = 12, PD_INDICES = 16, PD_COUNT = 20 };
uint32_t vita_gxm_precomputed_draw_size(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return GXM_STREAMS * 4u;
}
uint32_t vita_gxm_precomputed_fragment_size(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return GXP_DEFAULT_CONTAINER * 4u;
}
static uint32_t precomputed_init(VitaProcess* p, uint32_t state, uint32_t program, uint32_t extra,
                                 uint32_t extra_bytes) {
    if (!state || !program || !extra) return SCE_GXM_ERROR_INVALID_POINTER;
    if (extra & 15) return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    memset(vita_ptr(p, state), 0, 24);
    memset(vita_ptr(p, extra), 0, extra_bytes);
    vita_write32(p, state + PD_PROGRAM, program);
    vita_write32(p, state + PD_EXTRA, extra);
    return 0;
}
uint32_t vita_gxm_precomputed_draw_init(VitaProcess* p, VitaThread* t, uint32_t state, uint32_t program,
                                        uint32_t extra) {
    (void)t;
    return precomputed_init(p, state, program, extra, GXM_STREAMS * 4u);
}
uint32_t vita_gxm_precomputed_fragment_init(VitaProcess* p, VitaThread* t, uint32_t state, uint32_t program,
                                            uint32_t extra) {
    (void)t;
    return precomputed_init(p, state, program, extra, GXP_DEFAULT_CONTAINER * 4u);
}
uint32_t vita_gxm_precomputed_draw_set_stream(VitaProcess* p, VitaThread* t, uint32_t state, uint32_t index,
                                              uint32_t value) {
    (void)t;
    if (!state) return SCE_GXM_ERROR_INVALID_POINTER;
    if (index >= GXM_STREAMS) return SCE_GXM_ERROR_INVALID_VALUE;
    vita_write32(p, vita_read32(p, state + PD_EXTRA) + index * 4u, value);
    return 0;
}
/* sceGxmPrecomputedDrawSetParams(state, primitive type, index format, indices, count) */
uint32_t vita_gxm_precomputed_draw_set_params(VitaProcess* p, VitaThread* t, uint32_t state, uint32_t type,
                                              uint32_t format, uint32_t indices, uint32_t count) {
    (void)t;
    if (!state || !indices) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_write32(p, state + PD_TYPE, type);
    vita_write32(p, state + PD_FORMAT, format);
    vita_write32(p, state + PD_INDICES, indices);
    vita_write32(p, state + PD_COUNT, count);
    return 0;
}
uint32_t vita_gxm_precomputed_fragment_set_uniform_buffer(VitaProcess* p, VitaThread* t, uint32_t state, uint32_t index,
                                                          uint32_t value) {
    (void)t;
    if (!state) return SCE_GXM_ERROR_INVALID_POINTER;
    if (index >= GXP_DEFAULT_CONTAINER) return SCE_GXM_ERROR_INVALID_VALUE;
    vita_write32(p, vita_read32(p, state + PD_EXTRA) + index * 4u, value);
    return 0;
}
/* sceGxmSetPrecomputedFragmentState(context, state) */
uint32_t vita_gxm_set_precomputed_fragment_state(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t state) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (!c || !state) return 0;
    c->fragment_program = vita_read32(p, state + PD_PROGRAM);
    uint32_t extra = vita_read32(p, state + PD_EXTRA);
    for (unsigned i = 0; i < GXP_DEFAULT_CONTAINER; ++i) c->fragment_uniform[i] = vita_read32(p, extra + i * 4u);
    return 0;
}
/* sceGxmDrawPrecomputed(context, draw) */
uint32_t vita_gxm_draw_precomputed(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t d) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (!c || !d) return SCE_GXM_ERROR_INVALID_POINTER;
    if (!c->in_scene) return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    uint32_t count = vita_read32(p, d + PD_COUNT), extra = vita_read32(p, d + PD_EXTRA);
    c->vertex_program = vita_read32(p, d + PD_PROGRAM);
    for (unsigned i = 0; i < GXM_STREAMS; ++i) {
        uint32_t stream = vita_read32(p, extra + i * 4u);
        if (stream) c->streams[i] = stream;
    }
    if (!vertex_of(p->gxm, c->vertex_program) || !fragment_of(p->gxm, c->fragment_program))
        return SCE_GXM_ERROR_NULL_PROGRAM;
    if (!count) return SCE_GXM_ERROR_INVALID_INDEX_COUNT;
    ++p->gxm->draws;
    vita_render_draw(p, c, vita_read32(p, d + PD_TYPE), vita_read32(p, d + PD_FORMAT), vita_read32(p, d + PD_INDICES),
                     count, 1);
    return 0;
}

/* ---- textures: their control words (vita/gpu/texture.h) ---- */
static GxmTexture texture_read(VitaProcess* p, uint32_t address) {
    GxmTexture texture;
    for (unsigned i = 0; i < 4; ++i) texture.w[i] = vita_read32(p, address + i * 4);
    return texture;
}
static void texture_write(VitaProcess* p, uint32_t address, const GxmTexture* texture) {
    for (unsigned i = 0; i < 4; ++i) vita_write32(p, address + i * 4, texture->w[i]);
}
/* init_texture_base: clamped, point sampled, the size in the type's form, the data's address in word 2 */
static uint32_t texture_init(VitaProcess* p, uint32_t address, uint32_t data, uint32_t format, uint32_t width,
                             uint32_t height, uint32_t levels, uint32_t type) {
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    if (width > 4096 || height > 4096 || levels > 13 || !width || !height) return SCE_GXM_ERROR_INVALID_VALUE;
    GxmTexture x = {{0}};
    gxm_set_bits(&x.w[0], 17, 4, (levels ? levels - 1 : 0) > 15 ? 15 : (levels ? levels - 1 : 0));
    gxm_set_bits(&x.w[0], 31, 1, format >> 31);
    gxm_set_bits(&x.w[0], 21, 6, 31); /* the LOD bias */
    gxm_set_bits(&x.w[0], 6, 3, 1);   /* SCE_GXM_TEXTURE_ADDR_CLAMP, u and v */
    gxm_set_bits(&x.w[0], 3, 3, 1);
    if (type == GXM_TEXTURE_SWIZZLED || type == GXM_TEXTURE_CUBE) {
        unsigned w = 0, h = 0;
        while ((1u << (w + 1)) <= width) ++w;
        while ((1u << (h + 1)) <= height) ++h;
        gxm_set_bits(&x.w[1], 0, 4, h);
        gxm_set_bits(&x.w[1], 16, 4, w);
    } else {
        gxm_set_bits(&x.w[1], 0, 12, height - 1);
        gxm_set_bits(&x.w[1], 12, 12, width - 1);
    }
    gxm_set_bits(&x.w[1], 24, 5, (format >> 24) & 0x1f);
    gxm_set_bits(&x.w[1], 29, 3, type);
    x.w[2] = data & ~3u;
    gxm_set_bits(&x.w[3], 28, 3, (format >> 12) & 7);
    gxm_set_bits(&x.w[3], 31, 1, 1); /* normalized */
    texture_write(p, address, &x);
    return 0;
}
uint32_t vita_gxm_texture_init_linear(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t data, uint32_t format,
                                      uint32_t width, uint32_t height, uint32_t levels) {
    (void)t;
    return texture_init(p, address, data, format, width, height, levels,
                        GXM_TEXTURE_LINEAR);
}
uint32_t vita_gxm_texture_init_swizzled(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t data, uint32_t format,
                                        uint32_t width, uint32_t height, uint32_t levels) {
    (void)t;
    return texture_init(p, address, data, format, width, height, levels,
                        GXM_TEXTURE_SWIZZLED);
}
uint32_t vita_gxm_texture_init_tiled(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t data, uint32_t format,
                                     uint32_t width, uint32_t height, uint32_t levels) {
    (void)t;
    return texture_init(p, address, data, format, width, height, levels,
                        GXM_TEXTURE_TILED);
}
uint32_t vita_gxm_texture_init_cube(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t data, uint32_t format,
                                    uint32_t width, uint32_t height, uint32_t levels) {
    (void)t;
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    if (!width || !height || (width & (width - 1)) || (height & (height - 1)))
        return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    return texture_init(p, address, data, format, width, height, levels,
                        GXM_TEXTURE_CUBE);
}
/* sceGxmTextureInitLinearStrided(texture, data, format, width, height, stride): one level of rows stride bytes apart,
   the stride in 4-byte units less one where the mip filter, min filter, mip count and LOD bias are kept otherwise
   (gxm_texture_stride), its min filter its mag filter */
uint32_t vita_gxm_texture_init_linear_strided(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t data,
                                              uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    (void)t;
    if (stride & 3) return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    if (stride < 4 || stride > 0x20000u) return SCE_GXM_ERROR_INVALID_VALUE;
    uint32_t status = texture_init(p, address, data, format, width, height, 1,
                                   GXM_TEXTURE_LINEAR_STRIDED);
    if (status) return status;
    GxmTexture x = texture_read(p, address);
    uint32_t units = stride / 4 - 1;
    gxm_set_bits(&x.w[0], 9, 1, units & 1);
    gxm_set_bits(&x.w[0], 10, 2, (units >> 1) & 3);
    gxm_set_bits(&x.w[0], 17, 4, (units >> 3) & 15);
    gxm_set_bits(&x.w[0], 21, 6, (units >> 7) & 63);
    texture_write(p, address, &x);
    return 0;
}
/* sceGxmTextureSetMipFilter(texture, SCE_GXM_TEXTURE_MIP_FILTER_DISABLED 0 or _ENABLED 0x200): a strided texture's
   bit is its stride's */
uint32_t vita_gxm_texture_set_mip_filter(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t x200) {
    (void)t;
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmTexture x = texture_read(p, address);
    if (gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED) return SCE_GXM_ERROR_UNSUPPORTED;
    gxm_set_bits(&x.w[0], 9, 1, x200 != 0);
    texture_write(p, address, &x);
    return 0;
}
/* sceGxmTransferCopy's raw format sizes (bytes per pixel). */
static uint32_t transfer_bytes_per_pixel(uint32_t format) {
    switch (format) {
        case 0x00000000u: return 1; /* U8_R */
        case 0x00010000u: /* U4U4U4U4_ABGR */
        case 0x00020000u: /* U1U5U5U5_ABGR */
        case 0x00030000u: /* U5U6U5_BGR */
        case 0x00040000u: /* U8U8_GR */
        case 0x00070000u: /* VYUY422 */
        case 0x00080000u: /* YVYU422 */
        case 0x00090000u: /* UYVY422 */
        case 0x000a0000u: /* YUYV422 */
        case 0x000f0000u: return 2; /* RAW16 */
        case 0x00050000u: return 3; /* U8U8U8_BGR */
        case 0x00060000u: /* U8U8U8U8_ABGR */
        case 0x000d0000u: /* U2U10U10U10_ABGR */
        case 0x00110000u: return 4; /* RAW32 */
        case 0x00120000u: return 8; /* RAW64 */
        case 0x00130000u: return 16; /* RAW128 */
        default: return 0;
    }
}
/* sceGxmTransferCopy(width, height, key value/mask/mode, source format/type/address/x/y/stride,
   destination format/type/address/x/y/stride, sync object/flags, notification) */
uint32_t vita_gxm_transfer_copy(VitaProcess* p, VitaThread* t, uint32_t width, uint32_t height, uint32_t key_value,
                                uint32_t key_mask, uint32_t key_mode, uint32_t src_format, uint32_t src_type,
                                uint32_t src_address, uint32_t src_x, uint32_t src_y, int32_t src_stride,
                                uint32_t dst_format, uint32_t dst_type, uint32_t dst_address, uint32_t dst_x,
                                uint32_t dst_y, int32_t dst_stride, uint32_t sync_object, uint32_t sync_flags,
                                uint32_t notification) {
    (void)t, (void)key_value, (void)key_mask;
    uint32_t bytes = transfer_bytes_per_pixel(src_format);
    if (!src_address || !dst_address) return SCE_GXM_ERROR_INVALID_POINTER;
    if ((src_type == 0x00400000u && dst_type == 0x00800000u) ||
        (src_type == 0x00800000u && dst_type == 0x00400000u) ||
        (src_type == 0x00800000u && dst_type == 0x00800000u))
        return SCE_GXM_ERROR_INVALID_VALUE;
    if (src_format != dst_format || !bytes || src_type || dst_type || key_mode != 0)
        return SCE_GXM_ERROR_UNSUPPORTED;
    if (sync_object || sync_flags) return SCE_GXM_ERROR_UNSUPPORTED;
    if (width && height && (src_stride <= 0 || dst_stride <= 0)) return SCE_GXM_ERROR_INVALID_VALUE;
    uint32_t notify_address = 0, notify_value = 0;
    if (notification) {
        if (!vita_mapped(p, notification, 8)) return SCE_GXM_ERROR_INVALID_POINTER;
        notify_address = vita_read32(p, notification);
        notify_value = vita_read32(p, notification + 4);
        if (notify_address && !vita_mapped(p, notify_address, 4)) return SCE_GXM_ERROR_INVALID_POINTER;
        if (notify_address && !p->gxm) return SCE_GXM_ERROR_UNINITIALIZED;
    }
    if (!vita_render_transfer_copy(p, width, height, bytes, src_address, src_x, src_y, src_stride,
                                   dst_address, dst_x, dst_y, dst_stride))
        return SCE_GXM_ERROR_UNSUPPORTED;

    if (notify_address) {
        VitaGxm* g = p->gxm;
        if (g->note_count == GXM_NOTES) {
            vita_render_wait(p, g->notes[0].serial);
            notifications_poll(p);
        }
        uint64_t serial = g->render ? vita_render_submitted(p) + 1 : 0;
        g->notes[g->note_count].address = notify_address;
        g->notes[g->note_count].value = notify_value;
        g->notes[g->note_count++].serial = serial;
        notifications_poll(p);
    }
    return 0;
}
/* sceGxmTextureSetLodBias(texture, bias 0 to 63): a strided texture's bits are its stride's */
uint32_t vita_gxm_texture_set_lod_bias(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t to) {
    (void)t;
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    if (to > 63) return SCE_GXM_ERROR_INVALID_VALUE;
    GxmTexture x = texture_read(p, address);
    if (gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED) return SCE_GXM_ERROR_UNSUPPORTED;
    gxm_set_bits(&x.w[0], 21, 6, to);
    texture_write(p, address, &x);
    return 0;
}
/* sceGxmTextureSetWidth and SetHeight(texture, size): a swizzled or cube texture's a power of two, in its log2 */
static uint32_t texture_set_size(VitaProcess* p, uint32_t address, uint32_t size, unsigned linear_at,
                                 unsigned pow2_at) {
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    if (!size || size > 4096) return SCE_GXM_ERROR_INVALID_VALUE;
    GxmTexture x = texture_read(p, address);
    if (gxm_texture_pow2(&x)) {
        if (size & (size - 1)) return SCE_GXM_ERROR_INVALID_VALUE;
        unsigned log = 0;
        while ((1u << log) < size) ++log;
        gxm_set_bits(&x.w[1], pow2_at, 4, log);
    } else
        gxm_set_bits(&x.w[1], linear_at, 12, size - 1);
    texture_write(p, address, &x);
    return 0;
}
uint32_t vita_gxm_texture_set_width(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t size) {
    (void)t;
    return texture_set_size(p, address, size, 12, 16);
}
uint32_t vita_gxm_texture_set_height(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t size) {
    (void)t;
    return texture_set_size(p, address, size, 0, 0);
}
uint32_t vita_gxm_texture_get_data(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    return gxm_texture_data(&x);
}
uint32_t vita_gxm_texture_get_format(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    return gxm_texture_format(&x);
}
uint32_t vita_gxm_texture_get_width(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    return gxm_texture_width(&x);
}
uint32_t vita_gxm_texture_get_height(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    return gxm_texture_height(&x);
}
static uint32_t texture_set_filter(VitaProcess* p, uint32_t address, uint32_t filter, unsigned at) {
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmTexture x = texture_read(p, address);
    if (at == 10 && gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED) return SCE_GXM_ERROR_UNSUPPORTED;
    gxm_set_bits(&x.w[0], at, 2, filter);
    texture_write(p, address, &x);
    return 0;
}
uint32_t vita_gxm_texture_set_min_filter(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t filter) {
    (void)t;
    return texture_set_filter(p, address, filter, 10);
}
uint32_t vita_gxm_texture_set_mag_filter(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t filter) {
    (void)t;
    return texture_set_filter(p, address, filter, 12);
}
uint32_t vita_gxm_texture_get_min_filter(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    return gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED ? gxm_texture_mag_filter(&x) : gxm_texture_min_filter(&x);
}
uint32_t vita_gxm_texture_get_mag_filter(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    return gxm_texture_mag_filter(&x);
}
uint32_t vita_gxm_texture_set_palette(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t palette) {
    (void)t;
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    if (palette & 0x3f) return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    GxmTexture x = texture_read(p, address);
    gxm_set_bits(&x.w[3], 0, 26, palette >> 6);
    texture_write(p, address, &x);
    return 0;
}
/* the palette of a paletted format (P4 or P8, base formats 0x94 and 0x95 >> 24 here: 0x14, 0x15) */
uint32_t vita_gxm_texture_get_palette(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    uint32_t base = gxm_texture_format(&x) & 0x9f000000u;
    return base == 0x94000000u || base == 0x95000000u ? gxm_texture_palette(&x) : 0;
}
uint32_t vita_gxm_texture_set_data(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t data) {
    (void)t;
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmTexture x = texture_read(p, address);
    x.w[2] = (x.w[2] & 3u) | (data & ~3u);
    texture_write(p, address, &x);
    return 0;
}
static uint32_t texture_set_addr_mode(VitaProcess* p, uint32_t address, uint32_t mode, unsigned at) {
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmTexture x = texture_read(p, address);
    gxm_set_bits(&x.w[0], at, 3, mode);
    texture_write(p, address, &x);
    return 0;
}
uint32_t vita_gxm_texture_set_u_addr_mode(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t mode) {
    (void)t;
    return texture_set_addr_mode(p, address, mode, 6);
}
uint32_t vita_gxm_texture_set_v_addr_mode(VitaProcess* p, VitaThread* t, uint32_t address, uint32_t mode) {
    (void)t;
    return texture_set_addr_mode(p, address, mode, 3);
}

void vita_gxm_free(VitaProcess* p) {
    if (!p->gxm) return;
    vita_render_free(p);
    free(p->gxm);
    p->gxm = NULL;
}

/* ---- a program's parameters, read from its GXP ---- */
uint32_t vita_gxm_program_get_type(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return gxp_fragment(vita_ptr(p, a)) ? 1 : 0; /* SCE_GXM_FRAGMENT_PROGRAM, SCE_GXM_VERTEX_PROGRAM */
}
uint32_t vita_gxm_program_get_parameter_count(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return snx_ld_le32(vita_ptr(p, a + GXP_PARAMETER_COUNT));
}
uint32_t vita_gxm_program_get_parameter(VitaProcess* p, VitaThread* t, uint32_t program, uint32_t index) {
    (void)t;
    const unsigned char* g = vita_ptr(p, program);
    if (index >= snx_ld_le32(g + GXP_PARAMETER_COUNT)) return 0;
    return program + (uint32_t)(gxp_parameter(g, index) - g);
}
/* the default uniform buffer's bytes: its 32-bit words */
uint32_t vita_gxm_program_get_default_uniform_buffer_size(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return snx_ld_le32(vita_ptr(p, a + GXP_DEFAULT_UNIFORM_COUNT)) * 4;
}
/* _sceGxmProgramFindParameterBySemantic(program, semantic, index): the attribute with that semantic and index */
uint32_t vita_gxm_program_find_parameter_by_semantic(VitaProcess* p, VitaThread* t, uint32_t program, uint32_t semantic,
                                                     uint32_t index) {
    (void)t;
    const unsigned char* g = vita_ptr(p, program);
    for (uint32_t i = 0, n = snx_ld_le32(g + GXP_PARAMETER_COUNT); i < n; ++i) {
        const unsigned char* q = gxp_parameter(g, i);
        if (gxp_parameter_category(q) == GXP_CATEGORY_ATTRIBUTE && gxp_parameter_semantic(q) == semantic &&
            gxp_parameter_semantic_index(q) == index)
            return program + (uint32_t)(q - g);
    }
    return 0;
}
uint32_t vita_gxm_parameter_get_name(VitaProcess* p, VitaThread* t, uint32_t q) {
    (void)t;
    return q ? q + snx_ld_le32(vita_ptr(p, q)) : 0;
}
uint32_t vita_gxm_parameter_get_category(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return gxp_parameter_category(vita_ptr(p, a));
}
uint32_t vita_gxm_parameter_get_type(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return gxp_parameter_type(vita_ptr(p, a));
}
uint32_t vita_gxm_parameter_get_component_count(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return gxp_parameter_components(vita_ptr(p, a));
}
/* sceGxmProgramParameterGetIndex(program, parameter): its place among the program's parameters */
uint32_t vita_gxm_parameter_get_index(VitaProcess* p, VitaThread* t, uint32_t program, uint32_t parameter) {
    (void)t;
    const unsigned char* g = vita_ptr(p, program);
    uint32_t first = program + (uint32_t)(gxp_parameter(g, 0) - g);
    return parameter >= first ? (parameter - first) / 16 : 0;
}
uint32_t vita_gxm_parameter_get_container_index(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return gxp_parameter_container(vita_ptr(p, a));
}
uint32_t vita_gxm_program_is_native_color_used(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return (snx_ld_le32(vita_ptr(p, a + GXP_FLAGS)) & GXP_FLAG_NATIVECOLOR_USED) != 0;
}
uint32_t vita_gxm_parameter_get_array_size(VitaProcess* p, VitaThread* t, uint32_t a) {
    (void)t;
    return gxp_parameter_array_size(vita_ptr(p, a));
}
uint32_t vita_gxm_vertex_program_get_program(VitaProcess* p, VitaThread* t, uint32_t handle) {
    (void)t;
    GxmVertexProgram* v = p->gxm ? vertex_of(p->gxm, handle) : NULL;
    return v ? v->program : 0;
}
uint32_t vita_gxm_fragment_program_get_program(VitaProcess* p, VitaThread* t, uint32_t handle) {
    (void)t;
    GxmFragmentProgram* f = p->gxm ? fragment_of(p->gxm, handle) : NULL;
    return f ? f->program : 0;
}

/* sceGxmFragmentProgramGetPassType(program): SceGxmPassType (opaque 0, translucent 1, discard 2, mask update 3, depth
   replace 4) from what the program does: it writes its depth, it discards pixels, or it blends. (Vita3K returns opaque
   for all; mask update is never told apart, no GXP flag of it is known here) */
uint32_t vita_gxm_fragment_program_get_pass_type(VitaProcess* p, VitaThread* t, uint32_t program) {
    (void)t;
    GxmFragmentProgram* f = p->gxm ? fragment_of(p->gxm, program) : NULL;
    if (!f) return 0;
    uint32_t flags = f->program ? vita_read32(p, f->program + GXP_FLAGS) : 0;
    return flags & GXP_FLAG_DEPTH_USED ? 4 : flags & GXP_FLAG_DISCARD_USED ? 2 : f->blend ? 1 : 0;
}

/* ---- more of the context's state ---- */
uint32_t vita_gxm_set_back_depth_func(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t; /* the back face's depth state is the front's: two-sided depth isn't drawn */
    return 0;
}
uint32_t vita_gxm_set_front_stencil_func(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t func,
                                         uint32_t stencil_fail, uint32_t depth_fail, uint32_t depth_pass,
                                         uint32_t compare_mask, uint32_t write_mask) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (!c) return 0;
    c->stencil_func[0] = func;
    c->stencil_ops[0][0] = stencil_fail, c->stencil_ops[0][1] = depth_fail, c->stencil_ops[0][2] = depth_pass;
    c->stencil_masks[0][0] = compare_mask, c->stencil_masks[0][1] = write_mask;
    return 0;
}
uint32_t vita_gxm_set_front_stencil_ref(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t ref) {
    (void)t;
    GxmContext* c = context_of(p, context);
    if (c) c->stencil_ref[0] = ref;
    return 0;
}
/* sceGxmSet{Vertex,Fragment}UniformBuffer(context, index, buffer) */
static uint32_t set_uniform_buffer(VitaProcess* p, uint32_t context, uint32_t index, uint32_t buffer, bool fragment) {
    GxmContext* c = context_of(p, context);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    if (index >= GXP_DEFAULT_CONTAINER) return SCE_GXM_ERROR_INVALID_VALUE;
    (fragment ? c->fragment_uniform : c->vertex_uniform)[index] = buffer;
    return 0;
}
uint32_t vita_gxm_set_vertex_uniform_buffer(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t index,
                                            uint32_t buffer) {
    (void)t;
    return set_uniform_buffer(p, context, index, buffer, false);
}
uint32_t vita_gxm_set_fragment_uniform_buffer(VitaProcess* p, VitaThread* t, uint32_t context, uint32_t index,
                                              uint32_t buffer) {
    (void)t;
    return set_uniform_buffer(p, context, index, buffer, true);
}

/* ---- surfaces: the words color_surface_init writes ---- */
uint32_t vita_gxm_color_surface_init_disabled(VitaProcess* p, VitaThread* t, uint32_t surface) {
    (void)t;
    if (!surface) return SCE_GXM_ERROR_INVALID_POINTER;
    memset(vita_ptr(p, surface), 0, 48);
    return 0;
}
uint32_t vita_gxm_color_surface_get_data(VitaProcess* p, VitaThread* t, uint32_t i) {
    (void)t;
    return vita_read32(p, i + 16);
}
uint32_t vita_gxm_color_surface_get_stride(VitaProcess* p, VitaThread* t, uint32_t i) {
    (void)t;
    return vita_read32(p, i + 12);
}

/* ---- textures ---- */
uint32_t vita_gxm_texture_get_type(VitaProcess* p, VitaThread* t, uint32_t i) {
    (void)t;
    return vita_read32(p, i + 4) & 0xe0000000u;
}
uint32_t vita_gxm_texture_get_mipmap_count(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    return gxm_texture_mip_count(&x);
}
uint32_t vita_gxm_texture_get_stride(VitaProcess* p, VitaThread* t, uint32_t address) {
    (void)t;
    GxmTexture x = texture_read(p, address);
    return gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED ? gxm_texture_stride(&x) : 0;
}

/* ---- notifications: written when the scene that names them ends, which is at once here ---- */
uint32_t vita_gxm_get_notification_region(VitaProcess* p, VitaThread* t) {
    (void)t;
    VitaGxm* g = gxm(p);
    if (!g) return 0;
    if (!g->notification_region) g->notification_region = vita_alloc(p, &p->user, 0x1000, 0x1000);
    return g->notification_region;
}
/* sceGxmNotificationWait(notification): met once its address holds its value: the scene that names it last drawn,
   waited for (the game's own point to wait, while the GPU drew the scenes it ended before) */
uint32_t vita_gxm_notification_wait(VitaProcess* p, VitaThread* t, uint32_t n) {
    (void)t;
    if (!n) return SCE_GXM_ERROR_INVALID_POINTER;
    VitaGxm* g = p->gxm;
    uint32_t address = vita_read32(p, n);
    for (unsigned i = g ? g->note_count : 0; i-- > 0;)
        if (g->notes[i].address == address) {
            vita_render_wait(p, g->notes[i].serial);
            break;
        }
    notifications_poll(p);
    return 0;
}
/* sceGxmGetRenderTargetMemSize(params, size*): the driver memory a render target takes, which the game allocates
   and maps; the console's driver asks for some, this one leaves it unused (2 MiB, as Vita3K answers) */
uint32_t vita_gxm_get_render_target_mem_size(VitaProcess* p, VitaThread* t, uint32_t params, uint32_t size) {
    (void)t, (void)params;
    if (size) vita_write32(p, size, 0x200000);
    return 0;
}
uint32_t vita_gxm_render_target_get_driver_mem_block(VitaProcess* p, VitaThread* t, uint32_t target, uint32_t out) {
    (void)t, (void)target;
    if (out) vita_write32(p, out, 0);
    return 0;
}
