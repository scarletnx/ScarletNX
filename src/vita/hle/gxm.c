/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   sceGxm's API as Vita3K's modules/SceGxm/SceGxm.cpp answers it (GPL-2.0-or-later, Copyright (C) 2026 Vita3K team) at
   Vita3K/Vita3K f68b6ee: the context's state, the shader patcher's programs, the uniform buffers reserved from the
   context's rings and their data (sceGxmSetUniformDataF's layout), textures' control words, and the display queue,
   whose callback the game's display thread runs (a guest thread at the runtime's display loop, VITA_DISPLAY_LOOP).
   The drawing is the renderer's (vita/gpu/render.h) */
#include "vita/hle/hle.h"
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
static uint32_t arg(VitaProcess* p, VitaThread* t, unsigned n) { return vita_arg(p, t, n); }
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
    return handle >= GXM_HANDLE_FRAGMENT && i < GXM_PROGRAMS && g->fragment[i].references ? &g->fragment[i] : NULL;
}

/* ---- initialization: the display queue's thread ---- */
/* sceGxmInitialize(params): flags, the display queue's most pending entries, its callback, the size of an entry's data,
   the parameter buffer's size */
static uint32_t initialize(VitaProcess* p, VitaThread* t) {
    uint32_t params = arg(p, t, 0);
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
static uint32_t terminate(VitaProcess* p, VitaThread* t) {
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
static uint32_t display_queue_add_entry(VitaProcess* p, VitaThread* t) {
    VitaGxm* g = p->gxm;
    uint32_t data = arg(p, t, 2);
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
static uint32_t display_queue_finish(VitaProcess* p, VitaThread* t) {
    (void)t;
    VitaGxm* g = p->gxm;
    if (!g || !g->initialized) return SCE_GXM_ERROR_UNINITIALIZED;
    if (g->queue_count) vita_wait(p, (VitaWait){.kind = VITA_WAIT_GXM, .a = 1});
    return 0;
}
static uint32_t finish(VitaProcess* p, VitaThread* t) {
    (void)t;
    if (p->gxm && !vita_render_idle(p, false)) vita_wait(p, (VitaWait){.kind = VITA_WAIT_GXM, .a = 0});
    return 0;
}
static uint32_t ok(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}

/* ---- contexts ---- */
/* sceGxmCreateContext(params, SceGxmContext**): the host memory (the context's handle), the VDM, vertex, fragment
   and fragment USSE rings */
static uint32_t create_context(VitaProcess* p, VitaThread* t) {
    uint32_t params = arg(p, t, 0), out = arg(p, t, 1);
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
static uint32_t destroy_context(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_render_idle(p, true);
    c->used = false;
    return 0;
}

/* ---- render targets, surfaces, sync objects ---- */
/* SceGxmRenderTargetParams: flags, width and height (16 bits), scenes per frame and the multisample mode (16 bits),
   the sample locations, the driver's memory block */
static uint32_t create_render_target(VitaProcess* p, VitaThread* t) {
    uint32_t params = arg(p, t, 0), out = arg(p, t, 1);
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
static uint32_t destroy_render_target(VitaProcess* p, VitaThread* t) {
    uint32_t i = (arg(p, t, 0) - GXM_HANDLE_TARGET) / 16;
    if (!p->gxm || i >= GXM_TARGETS) return SCE_GXM_ERROR_INVALID_POINTER;
    p->gxm->targets[i] = (GxmTarget){0};
    return 0;
}
/* sceGxmColorSurfaceInit(surface, format, type, scale mode, output register size, width, height, stride, data): the
   words GxmColorSurface names, the background texture after them cleared */
static uint32_t color_surface_init(VitaProcess* p, VitaThread* t) {
    uint32_t surface = arg(p, t, 0), width = arg(p, t, 5), height = arg(p, t, 6), stride = arg(p, t, 7);
    uint32_t data = arg(p, t, 8);
    if (!surface) return SCE_GXM_ERROR_INVALID_POINTER;
    if (!width || !height || width > 4096 || height > 4096 || stride < width) return SCE_GXM_ERROR_INVALID_VALUE;
    if (data & 3) return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    uint32_t words[12] = {arg(p, t, 3) /* scale mode: downscale */, width, height, stride, data, arg(p, t, 1),
                          arg(p, t, 2), arg(p, t, 4)};
    for (unsigned i = 0; i < 12; ++i) vita_write32(p, surface + i * 4, words[i]);
    return 0;
}
/* sceGxmDepthStencilSurfaceInit(surface, format, type, stride, depth data, stencil data): the control word as the
   runtime keeps it (gxm.h, GXM_DEPTH_*) */
static uint32_t depth_stencil_surface_init(VitaProcess* p, VitaThread* t) {
    uint32_t surface = arg(p, t, 0), stride = arg(p, t, 3);
    if (!surface) return SCE_GXM_ERROR_INVALID_POINTER;
    if (stride > 0x7fffu) return SCE_GXM_ERROR_INVALID_VALUE;
    uint32_t control = (arg(p, t, 1) >> 12 & GXM_DEPTH_FORMAT_MASK) | stride << GXM_DEPTH_STRIDE_SHIFT |
                       (arg(p, t, 2) == SCE_GXM_DEPTH_STENCIL_SURFACE_TILED ? GXM_DEPTH_TILED : 0);
    vita_write32(p, surface, control);
    vita_write32(p, surface + 4, arg(p, t, 4));
    vita_write32(p, surface + 8, arg(p, t, 5));
    vita_write32(p, surface + 12, 0x3f800000u); /* the background depth, 1.0 */
    vita_write32(p, surface + 16, 0);
    return 0;
}
/* sceGxmDepthStencilSurfaceSetForceLoadMode and SetForceStoreMode(surface, mode): whether each scene starts with the
   depth in memory, and leaves its depth there */
static uint32_t depth_set_force(VitaProcess* p, VitaThread* t, uint32_t bit) {
    uint32_t surface = arg(p, t, 0);
    if (!surface) return SCE_GXM_ERROR_INVALID_POINTER;
    uint32_t control = vita_read32(p, surface) & ~bit;
    vita_write32(p, surface, control | (arg(p, t, 1) ? bit : 0));
    return 0;
}
static uint32_t depth_set_force_load(VitaProcess* p, VitaThread* t) {
    return depth_set_force(p, t, GXM_DEPTH_FORCE_LOAD);
}
static uint32_t depth_set_force_store(VitaProcess* p, VitaThread* t) {
    return depth_set_force(p, t, GXM_DEPTH_FORCE_STORE);
}
static uint32_t sync_object_create(VitaProcess* p, VitaThread* t) {
    uint32_t out = arg(p, t, 0);
    if (!out) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_write32(p, out, GXM_HANDLE_SYNC);
    return 0;
}

/* ---- the memory the GPU reads and writes ---- */
static uint32_t map_memory(VitaProcess* p, VitaThread* t) {
    uint32_t base = arg(p, t, 0), size = arg(p, t, 1);
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
static uint32_t unmap_memory(VitaProcess* p, VitaThread* t) {
    uint32_t base = arg(p, t, 0);
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
static uint32_t map_usse_memory(VitaProcess* p, VitaThread* t) {
    uint32_t offset = arg(p, t, 2);
    if (!arg(p, t, 0) || !offset) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_write32(p, offset, arg(p, t, 0));
    return 0;
}

/* ---- the shader patcher and programs ---- */
static uint32_t shader_patcher_create(VitaProcess* p, VitaThread* t) {
    uint32_t out = arg(p, t, 1);
    if (!arg(p, t, 0) || !out) return SCE_GXM_ERROR_INVALID_POINTER;
    vita_write32(p, out, GXM_HANDLE_PATCHER);
    return 0;
}
/* sceGxmShaderPatcherRegisterProgram(patcher, program, *id): the program's address is its ID */
static uint32_t register_program(VitaProcess* p, VitaThread* t) {
    uint32_t program = arg(p, t, 1), id = arg(p, t, 2);
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
/* sceGxmShaderPatcherCreateVertexProgram(patcher, id, attributes, count, streams, count, *program) */
static uint32_t create_vertex_program(VitaProcess* p, VitaThread* t) {
    uint32_t program = arg(p, t, 1), attributes = arg(p, t, 2), attribute_count = arg(p, t, 3);
    uint32_t streams = arg(p, t, 4), stream_count = arg(p, t, 5), out = arg(p, t, 6);
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
    if (slot == GXM_PROGRAMS) return SCE_GXM_ERROR_OUT_OF_MEMORY;
    g->vertex[slot] = want;
    vita_write32(p, out, GXM_HANDLE_VERTEX + slot * GXM_PROGRAM_HANDLE);
    return 0;
}
/* sceGxmShaderPatcherCreateFragmentProgram(patcher, id, output format, multisample mode, blend info, vertex program,
   *program) */
static uint32_t create_fragment_program(VitaProcess* p, VitaThread* t) {
    uint32_t program = arg(p, t, 1), blend = arg(p, t, 4), out = arg(p, t, 6);
    VitaGxm* g = gxm(p);
    if (!g || !program || !out) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmFragmentProgram want = {.references = 1,
                               .blend = blend != 0,
                               .program = program,
                               .vertex_program = arg(p, t, 5),
                               .output_format = arg(p, t, 2),
                               .multisample = arg(p, t, 3),
                               .blend_info = blend ? vita_read32(p, blend) : 0xf}; /* all channels written */
    uint32_t slot = GXM_PROGRAMS;
    for (uint32_t i = 0; i < GXM_PROGRAMS; ++i) {
        GxmFragmentProgram* f = &g->fragment[i];
        if (!f->references) {
            if (slot == GXM_PROGRAMS) slot = i;
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
    if (slot == GXM_PROGRAMS) return SCE_GXM_ERROR_OUT_OF_MEMORY;
    g->fragment[slot] = want;
    vita_write32(p, out, GXM_HANDLE_FRAGMENT + slot * GXM_PROGRAM_HANDLE);
    return 0;
}
static uint32_t release_vertex_program(VitaProcess* p, VitaThread* t) {
    GxmVertexProgram* v = p->gxm ? vertex_of(p->gxm, arg(p, t, 1)) : NULL;
    if (!v) return SCE_GXM_ERROR_INVALID_POINTER;
    if (--v->references) return 0;
    vita_render_idle(p, true);
    vita_render_shader_free(p, v->shader);
    v->shader = NULL;
    return 0;
}
static uint32_t release_fragment_program(VitaProcess* p, VitaThread* t) {
    GxmFragmentProgram* f = p->gxm ? fragment_of(p->gxm, arg(p, t, 1)) : NULL;
    if (!f) return SCE_GXM_ERROR_INVALID_POINTER;
    if (--f->references) return 0;
    vita_render_idle(p, true);
    vita_render_shader_free(p, f->shader);
    f->shader = NULL;
    return 0;
}
static uint32_t program_check(VitaProcess* p, VitaThread* t) {
    uint32_t program = arg(p, t, 0);
    if (!program) return SCE_GXM_ERROR_INVALID_POINTER;
    return vita_read32(p, program) == GXP_MAGIC ? 0 : SCE_GXM_ERROR_INVALID_VALUE;
}
/* sceGxmProgramFindParameterByName(program, name): the parameter's address in the program, NULL for none */
static uint32_t program_find_parameter_by_name(VitaProcess* p, VitaThread* t) {
    uint32_t program = arg(p, t, 0);
    char name[256];
    if (!program || !vita_string(p, arg(p, t, 1), name, sizeof name)) return 0;
    const unsigned char* gxp = vita_ptr(p, program);
    uint32_t count = snx_ld_le32(gxp + GXP_PARAMETER_COUNT);
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char* q = gxp_parameter(gxp, i);
        if (!strcmp(gxp_parameter_name(q), name)) return program + (uint32_t)(q - gxp);
    }
    return 0;
}
static uint32_t parameter_get_resource_index(VitaProcess* p, VitaThread* t) {
    uint32_t parameter = arg(p, t, 0);
    return parameter ? gxp_parameter_resource(vita_ptr(p, parameter)) : 0;
}

/* ---- scenes and draws ---- */
/* sceGxmBeginScene(context, flags, render target, valid region, vertex sync, fragment sync, color surface, depth) */
static uint32_t begin_scene(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    uint32_t target = arg(p, t, 2), color = arg(p, t, 6), depth = arg(p, t, 7);
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
    uint32_t region = arg(p, t, 3), x1 = region ? vita_read32(p, region) : r->width - 1;
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
static uint32_t end_scene(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    uint32_t notifications[2] = {arg(p, t, 1), arg(p, t, 2)};
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
static uint32_t set_vertex_program(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->vertex_program = arg(p, t, 1);
    return 0;
}
static uint32_t set_fragment_program(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->fragment_program = arg(p, t, 1);
    return 0;
}
static uint32_t set_vertex_stream(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    uint32_t index = arg(p, t, 1);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    if (index >= GXM_STREAMS) return SCE_GXM_ERROR_INVALID_VALUE;
    c->streams[index] = arg(p, t, 2);
    return 0;
}
static uint32_t set_texture(VitaProcess* p, VitaThread* t, bool vertex) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    uint32_t index = arg(p, t, 1), texture = arg(p, t, 2);
    if (!c || !texture) return SCE_GXM_ERROR_INVALID_POINTER;
    if (index >= (vertex ? 4u : GXM_TEXTURES)) return SCE_GXM_ERROR_INVALID_VALUE;
    GxmTexture* to = vertex ? &c->vertex_textures[index] : &c->textures[index];
    for (unsigned i = 0; i < 4; ++i) to->w[i] = vita_read32(p, texture + i * 4);
    return 0;
}
static uint32_t set_fragment_texture(VitaProcess* p, VitaThread* t) { return set_texture(p, t, false); }
static uint32_t set_vertex_texture(VitaProcess* p, VitaThread* t) { return set_texture(p, t, true); }
static uint32_t set_front_polygon_mode(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->front_polygon_mode = arg(p, t, 1);
    return 0;
}
static uint32_t set_back_polygon_mode(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->back_polygon_mode = arg(p, t, 1);
    return 0;
}
static uint32_t set_cull_mode(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->cull_mode = arg(p, t, 1);
    return 0;
}
/* sceGxmSetRegionClip(context, mode, xMin, yMin, xMax, yMax) */
static uint32_t set_region_clip(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    c->region_clip_mode = arg(p, t, 1);
    c->clip_x0 = arg(p, t, 2), c->clip_y0 = arg(p, t, 3), c->clip_x1 = arg(p, t, 4), c->clip_y1 = arg(p, t, 5);
    return 0;
}
/* sceGxmSetViewport(context, xOffset, xScale, yOffset, yScale, zOffset, zScale): the floats in S0 to S5, as the
   console's hard-float ABI passes them (a game's call loads them there) */
static uint32_t set_viewport(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    for (unsigned i = 0; i < 3; ++i)
        c->viewport_offset[i] = vita_float(t, i * 2), c->viewport_scale[i] = vita_float(t, i * 2 + 1);
    c->viewport_enabled = true;
    return 0;
}
static uint32_t set_viewport_enable(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->viewport_enabled = arg(p, t, 1) == 0; /* SCE_GXM_VIEWPORT_ENABLED is 0 */
    return 0;
}
static uint32_t set_depth_func(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->depth_func = arg(p, t, 1);
    return 0;
}
static uint32_t set_depth_write_enable(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->depth_write = arg(p, t, 1);
    return 0;
}
/* a default uniform buffer from the context's ring, for the program's default uniforms (its GXP's count of 32-bit
   words): the next aligned room, from the ring's start when the rest is too small */
static uint32_t reserve(VitaProcess* p, VitaThread* t, bool fragment) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    uint32_t out = arg(p, t, 1);
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
static uint32_t reserve_vertex(VitaProcess* p, VitaThread* t) { return reserve(p, t, false); }
static uint32_t reserve_fragment(VitaProcess* p, VitaThread* t) { return reserve(p, t, true); }
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
static uint32_t set_uniform_data_f(VitaProcess* p, VitaThread* t) {
    uint32_t buffer = arg(p, t, 0), parameter = arg(p, t, 1), offset = arg(p, t, 2), count = arg(p, t, 3);
    uint32_t data = arg(p, t, 4);
    if (!buffer || !parameter || !data) return SCE_GXM_ERROR_INVALID_POINTER;
    const unsigned char* q = vita_ptr(p, parameter);
    if (gxp_parameter_category(q) != GXP_CATEGORY_UNIFORM) return SCE_GXM_ERROR_INVALID_VALUE;
    unsigned type = gxp_parameter_type(q), size = gxp_type_size(type), components = gxp_parameter_components(q);
    unsigned char* base = (unsigned char*)vita_ptr(p, buffer) + gxp_parameter_resource(q) * 4;
    bool is_float = type == GXP_TYPE_F32 || type == GXP_TYPE_F16;
    if (gxp_parameter_array_size(q) == 1 || components == 1) {
        if (components == 1) size = 4;
        for (uint32_t i = 0; i < count; ++i)
            put_component(base + (offset + i) * size, components == 1 && type != GXP_TYPE_F32 ? GXP_TYPE_U32 : type,
                          gxp_f32(vita_ptr(p, data + i * 4)));
        return 0;
    }
    unsigned element = components * size, padded = element;
    padded = is_float ? (element + 7) & ~7u : (element + 3) & ~3u;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t at = offset + i, vector = at / components, component = at % components;
        put_component(base + vector * padded + component * size, type, gxp_f32(vita_ptr(p, data + i * 4)));
    }
    return 0;
}
/* sceGxmDraw(context, primitive type, index format, indices, count) */
static uint32_t draw(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    uint32_t count = arg(p, t, 4);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    if (!c->in_scene) return SCE_GXM_ERROR_NOT_WITHIN_SCENE;
    if (!vertex_of(p->gxm, c->vertex_program) || !fragment_of(p->gxm, c->fragment_program))
        return SCE_GXM_ERROR_NULL_PROGRAM;
    if (!count) return SCE_GXM_ERROR_INVALID_INDEX_COUNT;
    ++p->gxm->draws;
    vita_render_draw(p, c, arg(p, t, 1), arg(p, t, 2), arg(p, t, 3), count, 1);
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
static uint32_t texture_init_linear(VitaProcess* p, VitaThread* t) {
    return texture_init(p, arg(p, t, 0), arg(p, t, 1), arg(p, t, 2), arg(p, t, 3), arg(p, t, 4), arg(p, t, 5),
                        GXM_TEXTURE_LINEAR);
}
static uint32_t texture_init_swizzled(VitaProcess* p, VitaThread* t) {
    return texture_init(p, arg(p, t, 0), arg(p, t, 1), arg(p, t, 2), arg(p, t, 3), arg(p, t, 4), arg(p, t, 5),
                        GXM_TEXTURE_SWIZZLED);
}
static uint32_t texture_init_tiled(VitaProcess* p, VitaThread* t) {
    return texture_init(p, arg(p, t, 0), arg(p, t, 1), arg(p, t, 2), arg(p, t, 3), arg(p, t, 4), arg(p, t, 5),
                        GXM_TEXTURE_TILED);
}
/* sceGxmTextureInitLinearStrided(texture, data, format, width, height, stride): one level of rows stride bytes apart,
   the stride in 4-byte units less one where the mip filter, min filter, mip count and LOD bias are kept otherwise
   (gxm_texture_stride), its min filter its mag filter */
static uint32_t texture_init_linear_strided(VitaProcess* p, VitaThread* t) {
    uint32_t address = arg(p, t, 0), stride = arg(p, t, 5);
    if (stride & 3) return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    if (stride < 4 || stride > 0x20000u) return SCE_GXM_ERROR_INVALID_VALUE;
    uint32_t status = texture_init(p, address, arg(p, t, 1), arg(p, t, 2), arg(p, t, 3), arg(p, t, 4), 1,
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
static uint32_t texture_set_mip_filter(VitaProcess* p, VitaThread* t) {
    uint32_t address = arg(p, t, 0);
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmTexture x = texture_read(p, address);
    if (gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED) return SCE_GXM_ERROR_UNSUPPORTED;
    gxm_set_bits(&x.w[0], 9, 1, arg(p, t, 1) != 0);
    texture_write(p, address, &x);
    return 0;
}
/* sceGxmTextureSetWidth and SetHeight(texture, size): a swizzled or cube texture's a power of two, in its log2 */
static uint32_t texture_set_size(VitaProcess* p, VitaThread* t, unsigned linear_at, unsigned pow2_at) {
    uint32_t address = arg(p, t, 0), size = arg(p, t, 1);
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
static uint32_t texture_set_width(VitaProcess* p, VitaThread* t) { return texture_set_size(p, t, 12, 16); }
static uint32_t texture_set_height(VitaProcess* p, VitaThread* t) { return texture_set_size(p, t, 0, 0); }
static uint32_t texture_get_data(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    return gxm_texture_data(&x);
}
static uint32_t texture_get_format(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    return gxm_texture_format(&x);
}
static uint32_t texture_get_width(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    return gxm_texture_width(&x);
}
static uint32_t texture_get_height(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    return gxm_texture_height(&x);
}
static uint32_t texture_set_filter(VitaProcess* p, VitaThread* t, unsigned at) {
    uint32_t address = arg(p, t, 0);
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmTexture x = texture_read(p, address);
    if (at == 10 && gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED) return SCE_GXM_ERROR_UNSUPPORTED;
    gxm_set_bits(&x.w[0], at, 2, arg(p, t, 1));
    texture_write(p, address, &x);
    return 0;
}
static uint32_t texture_set_min_filter(VitaProcess* p, VitaThread* t) { return texture_set_filter(p, t, 10); }
static uint32_t texture_set_mag_filter(VitaProcess* p, VitaThread* t) { return texture_set_filter(p, t, 12); }
static uint32_t texture_get_min_filter(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    return gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED ? gxm_texture_mag_filter(&x) : gxm_texture_min_filter(&x);
}
static uint32_t texture_get_mag_filter(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    return gxm_texture_mag_filter(&x);
}
static uint32_t texture_set_palette(VitaProcess* p, VitaThread* t) {
    uint32_t address = arg(p, t, 0), palette = arg(p, t, 1);
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    if (palette & 0x3f) return SCE_GXM_ERROR_INVALID_ALIGNMENT;
    GxmTexture x = texture_read(p, address);
    gxm_set_bits(&x.w[3], 0, 26, palette >> 6);
    texture_write(p, address, &x);
    return 0;
}
/* the palette of a paletted format (P4 or P8, base formats 0x94 and 0x95 >> 24 here: 0x14, 0x15) */
static uint32_t texture_get_palette(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    uint32_t base = gxm_texture_format(&x) & 0x9f000000u;
    return base == 0x94000000u || base == 0x95000000u ? gxm_texture_palette(&x) : 0;
}
static uint32_t texture_set_data(VitaProcess* p, VitaThread* t) {
    uint32_t address = arg(p, t, 0);
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmTexture x = texture_read(p, address);
    x.w[2] = (x.w[2] & 3u) | (arg(p, t, 1) & ~3u);
    texture_write(p, address, &x);
    return 0;
}
static uint32_t texture_set_addr_mode(VitaProcess* p, VitaThread* t, unsigned at) {
    uint32_t address = arg(p, t, 0);
    if (!address) return SCE_GXM_ERROR_INVALID_POINTER;
    GxmTexture x = texture_read(p, address);
    gxm_set_bits(&x.w[0], at, 3, arg(p, t, 1));
    texture_write(p, address, &x);
    return 0;
}
static uint32_t texture_set_u_addr_mode(VitaProcess* p, VitaThread* t) { return texture_set_addr_mode(p, t, 6); }
static uint32_t texture_set_v_addr_mode(VitaProcess* p, VitaThread* t) { return texture_set_addr_mode(p, t, 3); }

void vita_gxm_free(VitaProcess* p) {
    if (!p->gxm) return;
    vita_render_free(p);
    free(p->gxm);
    p->gxm = NULL;
}

/* ---- a program's parameters, read from its GXP ---- */
static uint32_t program_get_type(VitaProcess* p, VitaThread* t) {
    return gxp_fragment(vita_ptr(p, arg(p, t, 0))) ? 1 : 0; /* SCE_GXM_FRAGMENT_PROGRAM, SCE_GXM_VERTEX_PROGRAM */
}
static uint32_t program_get_parameter_count(VitaProcess* p, VitaThread* t) {
    return snx_ld_le32(vita_ptr(p, arg(p, t, 0) + GXP_PARAMETER_COUNT));
}
static uint32_t program_get_parameter(VitaProcess* p, VitaThread* t) {
    uint32_t program = arg(p, t, 0), index = arg(p, t, 1);
    const unsigned char* g = vita_ptr(p, program);
    if (index >= snx_ld_le32(g + GXP_PARAMETER_COUNT)) return 0;
    return program + (uint32_t)(gxp_parameter(g, index) - g);
}
/* the default uniform buffer's bytes: its 32-bit words */
static uint32_t program_get_default_uniform_buffer_size(VitaProcess* p, VitaThread* t) {
    return snx_ld_le32(vita_ptr(p, arg(p, t, 0) + GXP_DEFAULT_UNIFORM_COUNT)) * 4;
}
/* _sceGxmProgramFindParameterBySemantic(program, semantic, index): the attribute with that semantic and index */
static uint32_t program_find_parameter_by_semantic(VitaProcess* p, VitaThread* t) {
    uint32_t program = arg(p, t, 0), semantic = arg(p, t, 1), index = arg(p, t, 2);
    const unsigned char* g = vita_ptr(p, program);
    for (uint32_t i = 0, n = snx_ld_le32(g + GXP_PARAMETER_COUNT); i < n; ++i) {
        const unsigned char* q = gxp_parameter(g, i);
        if (gxp_parameter_category(q) == GXP_CATEGORY_ATTRIBUTE && gxp_parameter_semantic(q) == semantic &&
            gxp_parameter_semantic_index(q) == index)
            return program + (uint32_t)(q - g);
    }
    return 0;
}
static uint32_t parameter_get_name(VitaProcess* p, VitaThread* t) {
    uint32_t q = arg(p, t, 0);
    return q ? q + snx_ld_le32(vita_ptr(p, q)) : 0;
}
static uint32_t parameter_get_category(VitaProcess* p, VitaThread* t) {
    return gxp_parameter_category(vita_ptr(p, arg(p, t, 0)));
}
static uint32_t parameter_get_type(VitaProcess* p, VitaThread* t) {
    return gxp_parameter_type(vita_ptr(p, arg(p, t, 0)));
}
static uint32_t vertex_program_get_program(VitaProcess* p, VitaThread* t) {
    GxmVertexProgram* v = p->gxm ? vertex_of(p->gxm, arg(p, t, 0)) : NULL;
    return v ? v->program : 0;
}
static uint32_t fragment_program_get_program(VitaProcess* p, VitaThread* t) {
    GxmFragmentProgram* f = p->gxm ? fragment_of(p->gxm, arg(p, t, 0)) : NULL;
    return f ? f->program : 0;
}

/* ---- more of the context's state ---- */
static uint32_t set_back_depth_func(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t; /* the back face's depth state is the front's: two-sided depth isn't drawn */
    return 0;
}
static uint32_t set_front_stencil_func(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (!c) return 0;
    c->stencil_func[0] = arg(p, t, 1);
    c->stencil_ops[0][0] = arg(p, t, 2), c->stencil_ops[0][1] = arg(p, t, 3), c->stencil_ops[0][2] = arg(p, t, 4);
    c->stencil_masks[0][0] = arg(p, t, 5), c->stencil_masks[0][1] = arg(p, t, 6);
    return 0;
}
static uint32_t set_front_stencil_ref(VitaProcess* p, VitaThread* t) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    if (c) c->stencil_ref[0] = arg(p, t, 1);
    return 0;
}
/* sceGxmSet{Vertex,Fragment}UniformBuffer(context, index, buffer) */
static uint32_t set_uniform_buffer(VitaProcess* p, VitaThread* t, bool fragment) {
    GxmContext* c = context_of(p, arg(p, t, 0));
    uint32_t index = arg(p, t, 1);
    if (!c) return SCE_GXM_ERROR_INVALID_POINTER;
    if (index >= GXP_DEFAULT_CONTAINER) return SCE_GXM_ERROR_INVALID_VALUE;
    (fragment ? c->fragment_uniform : c->vertex_uniform)[index] = arg(p, t, 2);
    return 0;
}
static uint32_t set_vertex_uniform_buffer(VitaProcess* p, VitaThread* t) { return set_uniform_buffer(p, t, false); }
static uint32_t set_fragment_uniform_buffer(VitaProcess* p, VitaThread* t) { return set_uniform_buffer(p, t, true); }

/* ---- surfaces: the words color_surface_init writes ---- */
static uint32_t color_surface_init_disabled(VitaProcess* p, VitaThread* t) {
    uint32_t surface = arg(p, t, 0);
    if (!surface) return SCE_GXM_ERROR_INVALID_POINTER;
    memset(vita_ptr(p, surface), 0, 48);
    return 0;
}
static uint32_t color_surface_get_data(VitaProcess* p, VitaThread* t) { return vita_read32(p, arg(p, t, 0) + 16); }
static uint32_t color_surface_get_stride(VitaProcess* p, VitaThread* t) { return vita_read32(p, arg(p, t, 0) + 12); }

/* ---- textures ---- */
static uint32_t texture_get_type(VitaProcess* p, VitaThread* t) { return vita_read32(p, arg(p, t, 0) + 4) & 0xe0000000u; }
static uint32_t texture_get_mipmap_count(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    return gxm_texture_mip_count(&x);
}
static uint32_t texture_get_stride(VitaProcess* p, VitaThread* t) {
    GxmTexture x = texture_read(p, arg(p, t, 0));
    return gxm_texture_type(&x) == GXM_TEXTURE_LINEAR_STRIDED ? gxm_texture_stride(&x) : 0;
}

/* ---- notifications: written when the scene that names them ends, which is at once here ---- */
static uint32_t get_notification_region(VitaProcess* p, VitaThread* t) {
    (void)t;
    VitaGxm* g = gxm(p);
    if (!g) return 0;
    if (!g->notification_region) g->notification_region = vita_alloc(p, &p->user, 0x1000, 0x1000);
    return g->notification_region;
}
/* sceGxmNotificationWait(notification): met once its address holds its value: the scene that names it last drawn,
   waited for (the game's own point to wait, while the GPU drew the scenes it ended before) */
static uint32_t notification_wait(VitaProcess* p, VitaThread* t) {
    uint32_t n = arg(p, t, 0);
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
static uint32_t get_render_target_mem_size(VitaProcess* p, VitaThread* t) {
    if (arg(p, t, 1)) vita_write32(p, arg(p, t, 1), 0x200000);
    return 0;
}
static uint32_t render_target_get_driver_mem_block(VitaProcess* p, VitaThread* t) {
    if (arg(p, t, 1)) vita_write32(p, arg(p, t, 1), 0);
    return 0;
}

const VitaHle vita_hle_gxm[] = {
    {0xb0f1e4ecu, "sceGxmInitialize", initialize},
    {0xb627de66u, "sceGxmTerminate", terminate},
    {0xe84ce5b4u, "sceGxmCreateContext", create_context},
    {0xeddc5fb2u, "sceGxmDestroyContext", destroy_context},
    {0x207af96bu, "sceGxmCreateRenderTarget", create_render_target},
    {0x0b94c50au, "sceGxmDestroyRenderTarget", destroy_render_target},
    {0xed0f6e25u, "sceGxmColorSurfaceInit", color_surface_init},
    {0xca9d41d1u, "sceGxmDepthStencilSurfaceInit", depth_stencil_surface_init},
    {0x6a6013e1u, "sceGxmSyncObjectCreate", sync_object_create},
    {0x889ae88cu, "sceGxmSyncObjectDestroy", ok},
    {0xc61e34fcu, "sceGxmMapMemory", map_memory},
    {0x828c68e8u, "sceGxmUnmapMemory", unmap_memory},
    {0xfa437510u, "sceGxmMapVertexUsseMemory", map_usse_memory},
    {0x099134f5u, "sceGxmUnmapVertexUsseMemory", ok},
    {0x008402c6u, "sceGxmMapFragmentUsseMemory", map_usse_memory},
    {0x80ccedbbu, "sceGxmUnmapFragmentUsseMemory", ok},
    {0x05032658u, "sceGxmShaderPatcherCreate", shader_patcher_create},
    {0xeaa5b100u, "sceGxmShaderPatcherDestroy", ok},
    {0x2b528462u, "sceGxmShaderPatcherRegisterProgram", register_program},
    {0xf103af8au, "sceGxmShaderPatcherUnregisterProgram", ok},
    {0xb7bba6d5u, "sceGxmShaderPatcherCreateVertexProgram", create_vertex_program},
    {0x4ed2e49du, "sceGxmShaderPatcherCreateFragmentProgram", create_fragment_program},
    {0xac1ff2dau, "sceGxmShaderPatcherReleaseVertexProgram", release_vertex_program},
    {0xbe2743d1u, "sceGxmShaderPatcherReleaseFragmentProgram", release_fragment_program},
    {0xed8b6c69u, "sceGxmProgramCheck", program_check},
    {0x277794c4u, "sceGxmProgramFindParameterByName", program_find_parameter_by_name},
    {0x5c79d59au, "sceGxmProgramParameterGetResourceIndex", parameter_get_resource_index},
    {0x8734ff4eu, "sceGxmBeginScene", begin_scene},
    {0xfe300e2fu, "sceGxmEndScene", end_scene},
    {0x31ff8abdu, "sceGxmSetVertexProgram", set_vertex_program},
    {0xad2f48d9u, "sceGxmSetFragmentProgram", set_fragment_program},
    {0x895df2e9u, "sceGxmSetVertexStream", set_vertex_stream},
    {0x29c34df5u, "sceGxmSetFragmentTexture", set_fragment_texture},
    {0x9eb4380fu, "sceGxmSetVertexTexture", set_vertex_texture},
    {0xfd93209du, "sceGxmSetFrontPolygonMode", set_front_polygon_mode},
    {0xf66ec6feu, "sceGxmSetBackPolygonMode", set_back_polygon_mode},
    {0xe1ca72aeu, "sceGxmSetCullMode", set_cull_mode},
    {0x70c86868u, "sceGxmSetRegionClip", set_region_clip},
    {0x3eb3380bu, "sceGxmSetViewport", set_viewport},
    {0x814f61ebu, "sceGxmSetViewportEnable", set_viewport_enable},
    {0x14bd831fu, "sceGxmSetFrontDepthFunc", set_depth_func},
    {0xf32cbf34u, "sceGxmSetFrontDepthWriteEnable", set_depth_write_enable},
    {0x97118913u, "sceGxmReserveVertexDefaultUniformBuffer", reserve_vertex},
    {0x7b1fabb6u, "sceGxmReserveFragmentDefaultUniformBuffer", reserve_fragment},
    {0x65dd0c84u, "sceGxmSetUniformDataF", set_uniform_data_f},
    {0xbc059afcu, "sceGxmDraw", draw},
    {0x4811aecbu, "sceGxmTextureInitLinear", texture_init_linear},
    {0xd572d547u, "sceGxmTextureInitSwizzled", texture_init_swizzled},
    {0xe6f0db27u, "sceGxmTextureInitTiled", texture_init_tiled},
    {0x6679bef0u, "sceGxmTextureInitLinearStrided", texture_init_linear_strided},
    {0x1ca9fe0bu, "sceGxmTextureSetMipFilter", texture_set_mip_filter},
    {0x2ea178beu, "sceGxmTextureSetWidth", texture_set_width},
    {0xaee7fdd1u, "sceGxmTextureSetHeight", texture_set_height},
    {0x5341bd46u, "sceGxmTextureGetData", texture_get_data},
    {0x855814c4u, "sceGxmTextureSetData", texture_set_data},
    {0xe868d2b3u, "sceGxmTextureGetFormat", texture_get_format},
    {0x126a3eb3u, "sceGxmTextureGetWidth", texture_get_width},
    {0x5420a086u, "sceGxmTextureGetHeight", texture_get_height},
    {0x416764e3u, "sceGxmTextureSetMinFilter", texture_set_min_filter},
    {0xfa695fd7u, "sceGxmTextureSetMagFilter", texture_set_mag_filter},
    {0x920666c6u, "sceGxmTextureGetMinFilter", texture_get_min_filter},
    {0xae7fbb51u, "sceGxmTextureGetMagFilter", texture_get_mag_filter},
    {0xdd6aabfau, "sceGxmTextureSetPalette", texture_set_palette},
    {0x0d189c30u, "sceGxmTextureGetPalette", texture_get_palette},
    {0x4281763eu, "sceGxmTextureSetUAddrMode", texture_set_u_addr_mode},
    {0x126cdaa3u, "sceGxmTextureSetVAddrMode", texture_set_v_addr_mode},
    {0xec5c26b5u, "sceGxmDisplayQueueAddEntry", display_queue_add_entry},
    {0xb98c5b0du, "sceGxmDisplayQueueFinish", display_queue_finish},
    {0x0733d8aeu, "sceGxmFinish", finish},
    {0x3d25fce9u, "sceGxmPadHeartbeat", ok},
    {0x04bb3c59u, "sceGxmProgramGetType", program_get_type},
    {0xd5d5fccdu, "sceGxmProgramGetParameterCount", program_get_parameter_count},
    {0x06ff9151u, "sceGxmProgramGetParameter", program_get_parameter},
    {0x8fa3f9c3u, "sceGxmProgramGetDefaultUniformBufferSize", program_get_default_uniform_buffer_size},
    {0x7fffdd7au, "_sceGxmProgramFindParameterBySemantic", program_find_parameter_by_semantic},
    {0x6af88a5du, "sceGxmProgramParameterGetName", parameter_get_name},
    {0x1997dc17u, "sceGxmProgramParameterGetCategory", parameter_get_category},
    {0x7b9023c3u, "sceGxmProgramParameterGetType", parameter_get_type},
    {0xbc52320eu, "sceGxmVertexProgramGetProgram", vertex_program_get_program},
    {0xe0e3b3f8u, "sceGxmFragmentProgramGetProgram", fragment_program_get_program},
    {0xb042a4d2u, "sceGxmSetBackDepthFunc", set_back_depth_func},
    {0xc18b706bu, "sceGxmSetBackDepthWriteEnable", ok},
    {0xb8645a9au, "sceGxmSetFrontStencilFunc", set_front_stencil_func},
    {0x8fa6fe44u, "sceGxmSetFrontStencilRef", set_front_stencil_ref},
    {0x06752183u, "sceGxmSetFrontPointLineWidth", ok},
    {0x575958a8u, "sceGxmSetFrontFragmentProgramEnable", ok},
    {0xe26b4834u, "sceGxmSetBackFragmentProgramEnable", ok},
    {0xc68015e4u, "sceGxmSetVertexUniformBuffer", set_vertex_uniform_buffer},
    {0xea0fc310u, "sceGxmSetFragmentUniformBuffer", set_fragment_uniform_buffer},
    {0x613639fau, "sceGxmColorSurfaceInitDisabled", color_surface_init_disabled},
    {0x2db6026cu, "sceGxmColorSurfaceGetData", color_surface_get_data},
    {0xf33d9980u, "sceGxmColorSurfaceGetStrideInPixels", color_surface_get_stride},
    {0x86456f7bu, "sceGxmColorSurfaceSetClip", ok},
    {0xf5c89643u, "sceGxmColorSurfaceSetGammaMode", ok},
    {0x0c44acd7u, "sceGxmDepthStencilSurfaceSetForceLoadMode", depth_set_force_load},
    {0x12aaa7afu, "sceGxmDepthStencilSurfaceSetForceStoreMode", depth_set_force_store},
    {0xf65d4917u, "sceGxmTextureGetType", texture_get_type},
    {0xf7b7b1e4u, "sceGxmTextureGetMipmapCount", texture_get_mipmap_count},
    {0xb0bd52f3u, "sceGxmTextureGetStride", texture_get_stride},
    {0xf23fce81u, "sceGxmTextureGetGammaMode", ok},
    {0xa6d9f4dau, "sceGxmTextureSetGammaMode", ok},
    {0x8bde825au, "sceGxmGetNotificationRegion", get_notification_region},
    {0x9f448e79u, "sceGxmNotificationWait", notification_wait},
    {0x3276c475u, "sceGxmPushUserMarker", ok},
    {0x4fa073a6u, "sceGxmPopUserMarker", ok},
    {0xb291c959u, "sceGxmGetRenderTargetMemSize", get_render_target_mem_size},
    {0x49553737u, "sceGxmRenderTargetGetDriverMemBlock", render_target_get_driver_mem_block},
    {0, NULL, NULL},
};
