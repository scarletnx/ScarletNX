/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/hle_nvn.c */
/* nvn_direct's command buffers: the game's NVN commands recorded into the command memory it adds, in nxnative's
   record format (gpu/nvn/nvn_obj.h), for the queue to submit.

   Memory, as NVN hands it out: the game adds command memory (AddCommandMemory, a pool and a range), records into it,
   and when a record doesn't fit, the command buffer calls the game's memory callback, which adds more. A recording
   that crosses into a new segment ends its old one with a JUMP. GetCommandMemoryUsed counts the bytes written since
   the game last added memory, the JUMPs included: games place the next recording, and size what they reserve, by it,
   so a record takes no more room than it must (a draw stops before its indirect fields, an array takes its count).

   Every NVN command that has no effect the CPU can see is only recorded. Phase 1 draws nothing, so nothing reads the
   records but the queue's submit, which runs what the CPU sees (gpu/nvn/nvn_queue.c); phase 2 replays them */
#include "nx/gpu/nvn/nvn_object.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

#define CB(a) NVN_OBJ(NvnCmdBuf, a)
/* room kept after every record but the END: one JUMP, so linking a segment never writes past the bytes the recording
   takes in one piece (the END needs none, and every other record is at least 16 bytes) */
#define OP_RESERVE 16

/* Runs the guest function fn(a0, a1, a2, a3) on this thread from inside an NVN call, as the driver calls the game's
   callbacks: its return (to the NVN call's own return address, which nothing else returns to) ends the run, and the
   NVN call's link register is put back for its own return. The registers the callback may clobber are the ones an
   NVN call may clobber */
static void guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 lr = c->x[30], pc = fn;
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    do snx_lookup(c, pc)(c, pc);
    while ((pc = c->pc) != lr);
    c->x[30] = lr;
}

static void put_hdr(u64 at, u32 op, u32 size) {
    NvnRecHdr h;
    h.op = op;
    h.size = size;
    memcpy(GP(at), &h, sizeof h);
}

/* a new segment [memory, memory + size): a recording with records already ends its old segment with a JUMP to it (and
   counts the JUMP in used, so the game placing the next recording by used doesn't overwrite it). A recording with
   none yet starts in the new segment, its first record its handle */
static void new_segment(NvnCmdBuf* o, u64 memory, u64 size) {
    u64 start = (memory + 7) & ~7ull;
    if (o->recording == 1 && o->cur) {
        put_hdr(o->cur, OP_JUMP, 16);
        memcpy(GP(o->cur + 8), &start, 8);
        o->used += 16;
    }
    o->cur = start;
    o->end = memory + size;
}

/* room for one record of size bytes with keep bytes after it (OP_RESERVE, 0 for the END), asking the game's memory
   callback for more as NVN does: the record's guest address */
static u64 alloc_keep(CPU* c, u64 cb, u32 size, u32 keep) {
    NvnCmdBuf* o = CB(cb);
    u64 at;
    int tries;
    size = (size + 7) & ~7u;
    for (tries = 0; !o->cur || o->cur + size + keep > o->end; tries++) {
        /* nxnative lends a segment of host memory here (the game's callback didn't add enough): the records must be in
           guest memory for the queue and the replay, and no game has needed it yet */
        if (!o->callback || tries == 2)
            fatal("nvn: command buffer %llx needs %u bytes of command memory, and the game's memory callback (%llx) "
                  "didn't add them",
                  (unsigned long long)cb, size + keep, (unsigned long long)o->callback);
        LOG_LIMIT(4, "nvn: command buffer %llx is out of command memory: the game's callback %llx adds more\n",
                  (unsigned long long)cb, (unsigned long long)o->callback);
        guest_call(c, o->callback, cb, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, size + keep,
                   o->callback_data);
    }
    at = o->cur;
    o->cur += size;
    o->used += size;
    put_hdr(at, 0, size);
    if (o->recording == 2) { /* the recording's first record is its handle */
        o->begin = at;
        o->recording = 1;
    }
    return at;
}

/* a record of op with payload bytes, zeroed: the payload's guest address */
static u64 rec(CPU* c, u64 cb, u32 op, size_t payload) {
    u32 size = (u32)((8 + payload + 7) & ~(size_t)7);
    u64 at = alloc_keep(c, cb, size, OP_RESERVE);
    put_hdr(at, op, size);
    memset(GP(at + 8), 0, size - 8);
    return at + 8;
}
#define REC(c, cb, OP, T)            ((T*)(void*)GP(rec((c), (cb), (OP), sizeof(T))))
/* a record of an array type, with its first n elements */
#define RECN(c, cb, OP, T, ARR, N)   ((T*)(void*)GP(rec((c), (cb), (OP), offsetof(T, ARR) + sizeof(((T*)0)->ARR[0]) * (size_t)(N))))

/* ============================================================ memory and recordings */
int nvn_CommandBufferInitialize(CPU* c, u64 cb, u64 d) {
    NvnCmdBuf* o = CB(cb);
    (void)c;
    memset(o, 0, sizeof *o);
    o->magic = NVN_MAGIC('C', 'M', 'D', 'B');
    o->device = d;
    return 1;
}
void nvn_CommandBufferSetMemoryCallback(CPU* c, u64 cb, u64 fn) {
    (void)c;
    CB(cb)->callback = fn;
}
void nvn_CommandBufferSetMemoryCallbackData(CPU* c, u64 cb, u64 data) {
    (void)c;
    CB(cb)->callback_data = data;
}
void nvn_CommandBufferAddCommandMemory(CPU* c, u64 cb, u64 pool, i64 offset, u64 size) {
    u64 memory = nvn_pool_memory(pool);
    (void)c;
    /* nxnative lends host memory for an empty pool (a game whose callback found none): not needed by any game yet */
    if (!memory || size < 64)
        fatal("nvn: command buffer %llx was given %llx bytes of command memory from pool %llx (memory %llx), which "
              "nvn_direct doesn't lend memory for",
              (unsigned long long)cb, (unsigned long long)size, (unsigned long long)pool, (unsigned long long)memory);
    new_segment(CB(cb), memory + (u64)offset, size);
    CB(cb)->used = 0; /* GetCommandMemoryUsed counts from the memory the game last added, across recordings */
}
/* the control memory is the driver's own bookkeeping: the records need none */
void nvn_CommandBufferAddControlMemory(CPU* c, u64 cb, u64 memory, u64 size) {
    (void)c;
    (void)cb;
    (void)memory;
    (void)size;
}
/* a recording starts with its first command, as in NVN: an empty one takes only its END */
void nvn_CommandBufferBeginRecording(CPU* c, u64 cb) {
    NvnCmdBuf* o = CB(cb);
    (void)c;
    if (o->recording) LOG_LIMIT(4, "nvn: command buffer %llx began recording twice\n", (unsigned long long)cb);
    o->recording = 2;
}
u64 nvn_CommandBufferEndRecording(CPU* c, u64 cb) {
    u64 at = alloc_keep(c, cb, 8, 0); /* nothing follows the END: it may take the segment's last bytes */
    put_hdr(at, OP_END, 8);
    CB(cb)->recording = 0;
    return CB(cb)->begin;
}
u64 nvn_CommandBufferGetCommandMemoryUsed(CPU* c, u64 cb) {
    const NvnCmdBuf* o = CB(cb);
    (void)c;
    return o->begin ? o->used : 0;
}
u64 nvn_CommandBufferGetControlMemoryUsed(CPU* c, u64 cb) {
    (void)c;
    (void)cb;
    return 0;
}
void nvn_CommandBufferCallCommands(CPU* c, u64 cb, i32 n, u64 handles) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 at = alloc_keep(c, cb, 16, OP_RESERVE), h = gr64(handles + 8 * (u64)i);
        put_hdr(at, OP_CALL, 16);
        memcpy(GP(at + 8), &h, 8);
    }
}

/* ============================================================ effects the CPU sees at submit */
void nvn_CommandBufferFenceSync(CPU* c, u64 cb, u64 sync, i32 condition, i32 flags) {
    (void)condition;
    (void)flags;
    *(u64*)(void*)GP(rec(c, cb, OP_FENCE_SYNC, 8)) = sync;
}
void nvn_CommandBufferReportCounter(CPU* c, u64 cb, i32 type, u64 address) {
    NvnPReportCounter* r = REC(c, cb, OP_REPORT_COUNTER, NvnPReportCounter);
    r->type = (u64)(u32)type;
    r->address = address;
}
void nvn_CommandBufferResetCounter(CPU* c, u64 cb, i32 type) {
    *(u64*)(void*)GP(rec(c, cb, OP_RESET_COUNTER, 8)) = (u64)(u32)type;
}

/* ============================================================ render targets, clears and fixed state */
/* A record with textures carries snapshots of their objects after its payload: once recorded, a command no longer
   depends on the object, which the game may finalize or reuse before it submits. An object that isn't a live
   texture as recorded keeps its address, read at submit */
#define SNAP_OFF(T) ((sizeof(T) + 7) & ~(size_t)7)
static u64 tex_snap(u64 payload, size_t off, int k, u64 t) {
    u64 s = payload + off + (u64)k * sizeof(NvnTexture);
    if (!t) return 0;
    if (NVN_OBJ(NvnTexture, t)->magic != NVN_MAGIC('T', 'E', 'X', 0)) {
        LOG_LIMIT(4, "nvn: texture %llx isn't a live texture as a command is recorded: read at submit\n",
                  (unsigned long long)t);
        return t;
    }
    memcpy(GP(s), GP(t), sizeof(NvnTexture));
    return s;
}
static void copy_view(NvnView* dst, u8* has, u64 v) {
    *has = v != 0;
    if (v) memcpy(dst, GP(v), sizeof *dst);
}
void nvn_CommandBufferSetRenderTargets(CPU* c, u64 cb, i32 n, u64 colors, u64 views, u64 depth, u64 depth_view) {
    u64 payload;
    NvnPSetRT* r;
    i32 i;
    if (n < 0 || n > 8) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferSetRenderTargets with %d targets\n", n);
        n = n < 0 ? 0 : 8;
    }
    payload = rec(c, cb, OP_SET_RT, SNAP_OFF(NvnPSetRT) + (size_t)(n + 1) * sizeof(NvnTexture));
    r = (NvnPSetRT*)(void*)GP(payload);
    r->count = n;
    for (i = 0; i < n; i++) {
        r->color[i] = colors ? tex_snap(payload, SNAP_OFF(NvnPSetRT), i, gr64(colors + 8 * (u64)i)) : 0;
        copy_view(&r->cview[i], &r->has_cview[i], views ? gr64(views + 8 * (u64)i) : 0);
    }
    r->depth = tex_snap(payload, SNAP_OFF(NvnPSetRT), n, depth);
    copy_view(&r->dview, &r->has_dview, depth_view);
}
void nvn_CommandBufferClearColor(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    NvnPClearColor* r = REC(c, cb, OP_CLEAR_COLOR, NvnPClearColor);
    r->index = index;
    r->mask = mask;
    memcpy(r->rgba, GP(rgba), sizeof r->rgba);
}
/* an integer target's clear, recorded as floats the replay converts back (exact up to 2^24) */
void nvn_CommandBufferClearColori(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    NvnPClearColor* r = REC(c, cb, OP_CLEAR_COLOR, NvnPClearColor);
    int i;
    r->index = index;
    r->mask = mask;
    for (i = 0; i < 4; i++) r->rgba[i] = (f32)(i32)gr32(rgba + 4 * (u64)i);
}
void nvn_CommandBufferClearDepthStencil(CPU* c, u64 cb, f32 depth, int depth_mask, i32 stencil, i32 stencil_mask) {
    NvnPClearDS* r = REC(c, cb, OP_CLEAR_DS, NvnPClearDS);
    r->depth = depth;
    r->depth_mask = depth_mask;
    r->stencil = stencil;
    r->stencil_mask = stencil_mask;
}
void nvn_CommandBufferClearBuffer(CPU* c, u64 cb, u64 address, u64 size, u32 value) {
    NvnPClearBuf* r = REC(c, cb, OP_CLEAR_BUF, NvnPClearBuf);
    r->address = address;
    r->size = size;
    r->value = value;
}
/* one viewport, as SetViewports(0, 1): the record holds the viewports it sets */
void nvn_CommandBufferSetViewport(CPU* c, u64 cb, i32 x, i32 y, i32 w, i32 h) {
    NvnPViewports* r = RECN(c, cb, OP_VIEWPORTS, NvnPViewports, v, 1);
    r->first = 0;
    r->count = 1;
    r->v[0][0] = (f32)x;
    r->v[0][1] = (f32)y;
    r->v[0][2] = (f32)w;
    r->v[0][3] = (f32)h;
}
void nvn_CommandBufferSetScissor(CPU* c, u64 cb, i32 x, i32 y, i32 w, i32 h) {
    NvnPScissor* r = REC(c, cb, OP_SCISSOR, NvnPScissor);
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}
void nvn_CommandBufferSetDepthRange(CPU* c, u64 cb, f32 n, f32 f) {
    NvnPDepthRange* r = REC(c, cb, OP_DEPTH_RANGE, NvnPDepthRange);
    r->n = n;
    r->f = f;
}
static void stencil(CPU* c, u64 cb, i32 face, i32 value, int which) {
    NvnPStencil* r = REC(c, cb, OP_STENCIL, NvnPStencil);
    r->face = face;
    r->which = which;
    if (which == 0)
        r->value_mask = value;
    else if (which == 1)
        r->mask = value;
    else
        r->ref = value;
}
void nvn_CommandBufferSetStencilValueMask(CPU* c, u64 cb, i32 face, i32 mask) { stencil(c, cb, face, mask, 0); }
void nvn_CommandBufferSetStencilMask(CPU* c, u64 cb, i32 face, i32 mask) { stencil(c, cb, face, mask, 1); }
void nvn_CommandBufferSetStencilRef(CPU* c, u64 cb, i32 face, i32 ref) { stencil(c, cb, face, ref, 2); }
void nvn_CommandBufferSetBlendColor(CPU* c, u64 cb, u64 rgba) {
    memcpy(REC(c, cb, OP_BLEND_COLOR, NvnPBlendColor)->rgba, GP(rgba), 16);
}
void nvn_CommandBufferSetRasterizerDiscard(CPU* c, u64 cb, int on) {
    REC(c, cb, OP_RAST_DISCARD, NvnPRastDiscard)->on = on != 0;
}
void nvn_CommandBufferSetLineWidth(CPU* c, u64 cb, f32 width) {
    (void)c;
    (void)cb;
    if (width != 1.0f) LOG_LIMIT(1, "nvn: line width %.2f isn't recorded\n", (double)width);
}
void nvn_CommandBufferBarrier(CPU* c, u64 cb, i32 bits) { *(i32*)(void*)GP(rec(c, cb, OP_BARRIER, 4)) = bits; }
/* a descriptor pool the command buffer's handles index from now on: its descriptors' address and count */
static void set_pool(CPU* c, u64 cb, u32 op, u64 dp) {
    const NvnDescPool* p = NVN_OBJ(NvnDescPool, dp);
    u64 descriptors = nvn_pool_memory(p->pool) + p->offset;
    i32 count = p->count;
    NvnPSetPool* r = REC(c, cb, op, NvnPSetPool);
    r->descriptors = descriptors;
    r->count = count;
}
void nvn_CommandBufferSetTexturePool(CPU* c, u64 cb, u64 tp) { set_pool(c, cb, OP_SET_TEX_POOL, tp); }
void nvn_CommandBufferSetSamplerPool(CPU* c, u64 cb, u64 sp) { set_pool(c, cb, OP_SET_SAMP_POOL, sp); }
/* hints and GPU-side waits with nothing to record: the replay runs a queue's work in order */
void nvn_CommandBufferSetShaderScratchMemory(CPU* c, u64 cb, u64 pool, i64 offset, u64 size) {
    (void)c;
    (void)cb;
    (void)pool;
    (void)offset;
    (void)size;
}
void nvn_CommandBufferSetTiledCacheAction(CPU* c, u64 cb, i32 action) {
    (void)c;
    (void)cb;
    (void)action;
}
void nvn_CommandBufferSetTiledCacheTileSize(CPU* c, u64 cb, i32 w, i32 h) {
    (void)c;
    (void)cb;
    (void)w;
    (void)h;
}
void nvn_CommandBufferWaitSync(CPU* c, u64 cb, u64 sync) {
    (void)c;
    (void)cb;
    (void)sync;
}

/* ============================================================ binds */
void nvn_CommandBufferBindProgram(CPU* c, u64 cb, u64 program, i32 stages) {
    NvnPBindProgram* r = REC(c, cb, OP_BIND_PROGRAM, NvnPBindProgram);
    r->program = program;
    r->stages = stages;
}
static void bind_state(CPU* c, u64 cb, i32 kind, u64 st, size_t n) {
    NvnPBindState* r = REC(c, cb, OP_BIND_STATE, NvnPBindState);
    r->kind = kind;
    memcpy(r->bytes, GP(st), n);
}
void nvn_CommandBufferBindBlendState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 0, st, 8); }
void nvn_CommandBufferBindColorState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 1, st, 4); }
void nvn_CommandBufferBindChannelMaskState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 2, st, 4); }
void nvn_CommandBufferBindMultisampleState(CPU* c, u64 cb, u64 st) {
    bind_state(c, cb, 3, st, sizeof(NvnMultisample));
}
void nvn_CommandBufferBindPolygonState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 4, st, 4); }
void nvn_CommandBufferBindDepthStencilState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 5, st, 8); }
void nvn_CommandBufferBindVertexAttribState(CPU* c, u64 cb, i32 n, u64 st) {
    NvnPBindVAttr* r;
    if (n < 0 || n > 16) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferBindVertexAttribState of %d attributes\n", n);
        return;
    }
    r = RECN(c, cb, OP_BIND_VATTR, NvnPBindVAttr, st, n);
    r->count = n;
    memcpy(r->st, GP(st), 4 * (size_t)n);
}
void nvn_CommandBufferBindVertexStreamState(CPU* c, u64 cb, i32 n, u64 st) {
    NvnPBindVStream* r;
    if (n < 0 || n > 16) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferBindVertexStreamState of %d streams\n", n);
        return;
    }
    r = RECN(c, cb, OP_BIND_VSTREAM, NvnPBindVStream, st, n);
    r->count = n;
    memcpy(r->st, GP(st), 8 * (size_t)n);
}
/* one buffer, as BindVertexBuffers(index, 1) */
void nvn_CommandBufferBindVertexBuffer(CPU* c, u64 cb, i32 index, u64 address, u64 size) {
    NvnPBindVBuf* r;
    if (index < 0 || index >= 16) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferBindVertexBuffer of buffer %d\n", index);
        return;
    }
    r = RECN(c, cb, OP_BIND_VBUF, NvnPBindVBuf, r, 1);
    r->first = index;
    r->count = 1;
    r->r[0].address = address;
    r->r[0].size = size;
}
static void bind_buf(CPU* c, u64 cb, u32 op, i32 stage, i32 index, u64 address, u64 size) {
    NvnPBindBuf* r = REC(c, cb, op, NvnPBindBuf);
    r->stage = stage;
    r->index = index;
    r->address = address;
    r->size = size;
}
void nvn_CommandBufferBindUniformBuffer(CPU* c, u64 cb, i32 stage, i32 index, u64 address, u64 size) {
    bind_buf(c, cb, OP_BIND_UBO, stage, index, address, size);
}
void nvn_CommandBufferBindUniformBuffers(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 ranges) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 at = ranges + (u64)i * sizeof(NVNbufferRange);
        bind_buf(c, cb, OP_BIND_UBO, stage, first + i, gr64(at), gr64(at + 8));
    }
}
void nvn_CommandBufferBindStorageBuffer(CPU* c, u64 cb, i32 stage, i32 index, u64 address, u64 size) {
    bind_buf(c, cb, OP_BIND_SSBO, stage, index, address, size);
}
/* one combined handle, laid out as the hardware's command (nvn_obj.h NvnPBindTex1) */
void nvn_CommandBufferBindTexture(CPU* c, u64 cb, i32 stage, i32 slot, u64 handle) {
    NvnPBindTex1* r = REC(c, cb, OP_BIND_TEX1, NvnPBindTex1);
    r->stage = stage;
    r->slot = slot;
    r->h = handle;
}
/* an image handle is its descriptor's ID in the texture pool */
void nvn_CommandBufferBindImage(CPU* c, u64 cb, i32 stage, i32 slot, u64 handle) {
    NvnPBindHandles* r = RECN(c, cb, OP_BIND_IMG, NvnPBindHandles, h, 1);
    r->stage = stage;
    r->first = slot;
    r->count = 1;
    r->h[0] = handle;
}
void nvn_CommandBufferBindTransformFeedbackBuffer(CPU* c, u64 cb, i32 index, u64 address, u64 size) {
    NvnPTfBind* r = REC(c, cb, OP_TF_BIND, NvnPTfBind);
    r->index = index;
    r->address = address;
    r->size = size;
}
static void tf_state(CPU* c, u64 cb, i32 what, u64 control) {
    NvnPTfState* r = REC(c, cb, OP_TF_STATE, NvnPTfState);
    r->what = what;
    r->control = control;
}
void nvn_CommandBufferBeginTransformFeedback(CPU* c, u64 cb, u64 control) { tf_state(c, cb, 0, control); }
void nvn_CommandBufferEndTransformFeedback(CPU* c, u64 cb, u64 control) { tf_state(c, cb, 1, control); }

/* ============================================================ draws */
/* a plain draw stops before its indirect and multi-draw fields */
static void draw(CPU* c, u64 cb, i32 prim, i32 first, i32 count, i32 instances, i32 base_instance) {
    NvnPDraw d = {0};
    d.prim = prim;
    d.first = first;
    d.count = count;
    d.instances = instances;
    d.base_instance = base_instance;
    memcpy(GP(rec(c, cb, OP_DRAW, offsetof(NvnPDraw, ind))), &d, offsetof(NvnPDraw, ind));
}
static void draw_idx(CPU* c, u64 cb, i32 prim, i32 type, i32 count, u64 ib, i32 base_vertex, i32 base_instance,
                     i32 instances) {
    NvnPDrawIdx d = {0};
    d.prim = prim;
    d.index_type = type;
    d.count = count;
    d.base_vertex = base_vertex;
    d.base_instance = base_instance;
    d.instances = instances;
    d.index_buffer = ib;
    memcpy(GP(rec(c, cb, OP_DRAW_IDX, offsetof(NvnPDrawIdx, ind))), &d, offsetof(NvnPDrawIdx, ind));
}
void nvn_CommandBufferDrawArrays(CPU* c, u64 cb, i32 prim, i32 first, i32 count) {
    draw(c, cb, prim, first, count, 1, 0);
}
void nvn_CommandBufferDrawArraysInstanced(CPU* c, u64 cb, i32 prim, i32 first, i32 count, i32 base_instance,
                                          i32 instances) {
    draw(c, cb, prim, first, count, instances, base_instance);
}
void nvn_CommandBufferDrawElements(CPU* c, u64 cb, i32 prim, i32 type, i32 count, u64 ib) {
    draw_idx(c, cb, prim, type, count, ib, 0, 0, 1);
}
void nvn_CommandBufferDrawElementsInstanced(CPU* c, u64 cb, i32 prim, i32 type, i32 count, u64 ib, i32 base_vertex,
                                            i32 base_instance, i32 instances) {
    draw_idx(c, cb, prim, type, count, ib, base_vertex, base_instance, instances);
}
/* drawing the vertex count transform feedback captured: nxnative records nothing, and neither does this */
void nvn_CommandBufferDrawTransformFeedback(CPU* c, u64 cb, i32 prim, u64 control) {
    (void)c;
    (void)cb;
    (void)prim;
    (void)control;
    LOG_LIMIT(1, "nvn: nvnCommandBufferDrawTransformFeedback isn't recorded\n");
}
