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

   Every NVN command is only recorded here: the queue's replay runs the records on the GPU thread once they are
   submitted (gpu/nvn/nvn_replay.c) */
#include "nx/gpu/nvn/nvn_cmd.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "nx/gpu/nvn/nvn_verify.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdlib.h>
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
        if (!o->lent) o->used += 16;
    }
    o->cur = start;
    o->end = memory + size;
    o->lent = 0;
}

/* Records take more room than the driver's methods for the same commands (a draw's 32 bytes against its 16, render
   targets' texture snapshots), and a game sizes its command memory by the driver's. One game's display lists have a
   memory callback that adds nothing (a bare ret): when the game's memory runs out and its callback adds none, the
   recording goes on in guest memory of the runtime's own, from the kernel region the game never maps, as nxnative
   lent memory of its own. Its bytes aren't the game's, so they count nothing of GetCommandMemoryUsed.
   A lent segment belongs to the game's memory it continues (its origin): when the game adds that memory again, it is
   done with what it recorded there, as on the console, and so with what the runtime lent after it, which goes back
   for the next lend. That game re-records its display lists into the same buffers every frame */
#define LEND_SEGMENT (64u << 10)
static void lend(u64 cb, u32 need) {
    NvnCmdBuf* o = CB(cb);
    u64 size = need > LEND_SEGMENT / 2 ? ((u64)need * 2 + 0xffff) & ~0xffffull : LEND_SEGMENT, at = 0;
    u32 i;
    pm_lock(&g_nvn.cmd.lock);
    for (i = 0; i < g_nvn.cmd.n && !at; i++)
        if (!g_nvn.cmd.l[i].taken && g_nvn.cmd.l[i].size >= size) at = g_nvn.cmd.l[i].at;
    if (at)
        i--;
    else {
        u64 was = g_nvn.cmd.total;
        if (g_nvn.cmd.n == g_nvn.cmd.cap) {
            u32 cap = g_nvn.cmd.cap ? g_nvn.cmd.cap * 2 : 64;
            NvnLent* l = (NvnLent*)realloc(g_nvn.cmd.l, cap * sizeof *l);
            if (!l) fatal("nvn: out of memory for the command memory lent");
            g_nvn.cmd.l = l;
            g_nvn.cmd.cap = cap;
        }
        at = kmem_alloc(size, MS_KERNEL, PERM_RW);
        i = g_nvn.cmd.n++;
        g_nvn.cmd.l[i].at = at;
        g_nvn.cmd.l[i].size = size;
        g_nvn.cmd.total += size;
        LOG_LIMIT(4, "nvn: command buffer %llx is out of the game's command memory, which its callback doesn't add to: "
                     "%llu KiB of the runtime's lent\n",
                  (unsigned long long)cb, (unsigned long long)(size >> 10));
        if ((g_nvn.cmd.total ^ was) > was) /* a new power of two of the total */
            LOG("nvn: %llu KiB of the runtime's memory lent to command buffers in all\n",
                (unsigned long long)(g_nvn.cmd.total >> 10));
    }
    g_nvn.cmd.l[i].taken = 1;
    g_nvn.cmd.l[i].origin = o->origin;
    size = g_nvn.cmd.l[i].size;
    pm_unlock(&g_nvn.cmd.lock);
    new_segment(o, at, size);
    o->lent = 1;
}
/* the game adds [memory, memory + size) again: what was lent after it comes back */
static void lend_return(u64 memory, u64 size) {
    u32 i;
    pm_lock(&g_nvn.cmd.lock);
    for (i = 0; i < g_nvn.cmd.n; i++)
        if (g_nvn.cmd.l[i].taken && g_nvn.cmd.l[i].origin - memory < size) {
            g_nvn.cmd.l[i].taken = 0;
            nvn_verify_memory_back(g_nvn.cmd.l[i].at, g_nvn.cmd.l[i].size);
        }
    pm_unlock(&g_nvn.cmd.lock);
}

/* room for one record of size bytes with keep bytes after it (OP_RESERVE, 0 for the END), asking the game's memory
   callback for more as NVN does: the record's guest address */
static u64 alloc_keep(CPU* c, u64 cb, u32 size, u32 keep) {
    NvnCmdBuf* o = CB(cb);
    u64 at;
    int tries;
    size = (size + 7) & ~7u;
    for (tries = 0; !o->cur || o->cur + size + keep > o->end; tries++) {
        /* the game's callback once, then memory of the runtime's own (lend) */
        if (!o->callback || tries == 1) {
            lend(cb, size + keep);
            continue;
        }
        LOG_LIMIT(4, "nvn: command buffer %llx is out of command memory: the game's callback %llx adds more\n",
                  (unsigned long long)cb, (unsigned long long)o->callback);
        guest_call(c, o->callback, cb, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, size + keep,
                   o->callback_data);
    }
    at = o->cur;
    o->cur += size;
    if (!o->lent) o->used += size;
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
    memset(GP(at + 8), 0, size - 8); /* guest memory: every byte the game sees */
    return at + 8;
}
#define REC(c, cb, OP, T)            ((T*)(void*)GP(rec((c), (cb), (OP), sizeof(T))))
/* a record of an array type, with its first n elements */
#define RECN(c, cb, OP, T, ARR, N)   ((T*)(void*)GP(rec((c), (cb), (OP), offsetof(T, ARR) + sizeof(((T*)0)->ARR[0]) * (size_t)(N))))

/* ============================================================ memory and recordings */
int nvn_CommandBufferInitialize(CPU* c, u64 cb, u64 d) {
    NvnCmdBuf* o = CB(cb);
    (void)c;
    memset(o, 0, sizeof *o); /* guest memory: every byte the game sees */
    o->magic = NVN_MAGIC('C', 'M', 'D', 'B');
    o->device = d;
    return 1;
}
void nvn_CommandBufferFinalize(CPU* c, u64 cb) {
    (void)c;
    CB(cb)->magic = 0;
}
int nvn_CommandBufferIsRecording(CPU* c, u64 cb) {
    (void)c;
    return CB(cb)->recording != 0;
}
/* the driver's switch for its memory callback: the runtime calls it whenever command memory runs out */
void nvn_CommandBufferSetCommandMemoryCallbackEnabled(CPU* c, u64 cb, int on) {
    (void)c;
    (void)cb;
    (void)on;
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
    if (g_nvn.cmd.n) lend_return(memory + (u64)offset, size);
    new_segment(CB(cb), memory + (u64)offset, size);
    CB(cb)->origin = memory + (u64)offset;
    CB(cb)->game_end = memory + (u64)offset + size;
    CB(cb)->used = 0; /* GetCommandMemoryUsed counts from the memory the game last added, across recordings */
}
/* the control memory is the driver's own bookkeeping: the records need none. Its size is kept for the getters */
void nvn_CommandBufferAddControlMemory(CPU* c, u64 cb, u64 memory, u64 size) {
    (void)c;
    (void)memory;
    CB(cb)->control_size = size;
}
/* The memory the game last added and what is left of it, which games check before they record. The command memory's
   room is what a record can still take there before the runtime lends memory of its own (each record keeps room for a
   JUMP after it): none once a lent segment is in use. The control memory is never used */
u64 nvn_CommandBufferGetCommandMemorySize(CPU* c, u64 cb) {
    const NvnCmdBuf* o = CB(cb);
    (void)c;
    return o->game_end - o->origin;
}
u64 nvn_CommandBufferGetCommandMemoryFree(CPU* c, u64 cb) {
    const NvnCmdBuf* o = CB(cb);
    (void)c;
    return o->lent || !o->cur || o->cur + OP_RESERVE > o->end ? 0 : o->end - o->cur - OP_RESERVE;
}
u64 nvn_CommandBufferGetControlMemorySize(CPU* c, u64 cb) {
    (void)c;
    return CB(cb)->control_size;
}
u64 nvn_CommandBufferGetControlMemoryFree(CPU* c, u64 cb) {
    (void)c;
    return CB(cb)->control_size;
}
/* the game's driver's switch for its callback when control memory runs out: the records never use control memory */
void nvn_CommandBufferSetControlMemoryCallbackEnabled(CPU* c, u64 cb, int on) {
    (void)c;
    (void)cb;
    (void)on;
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
/* A record's snapshot pointers (a texture object copied into the record, tex_snap) that point into the record at src,
   moved to its copy at dst */
static void relocate(u64 dst, u64 src, u32 op, u32 size) {
    static const struct {
        u32 op, off, n;
    } snaps[] = {
        {OP_SET_RT, offsetof(NvnPSetRT, color), 8},     {OP_SET_RT, offsetof(NvnPSetRT, depth), 1},
        {OP_COPY_B2T, offsetof(NvnPCopyB2T, texture), 1}, {OP_COPY_T2B, offsetof(NvnPCopyB2T, texture), 1},
        {OP_COPY_T2T, offsetof(NvnPCopyT2T, src), 1},   {OP_COPY_T2T, offsetof(NvnPCopyT2T, dst), 1},
        {OP_CLEAR_TEX, offsetof(NvnPClearTex, texture), 1},
    };
    size_t i;
    u32 k;
    for (i = 0; i < sizeof snaps / sizeof *snaps; i++)
        for (k = 0; snaps[i].op == op && k < snaps[i].n; k++) {
            u64 at = dst + 8 + snaps[i].off + 8 * (u64)k, v = gr64(at);
            if (v >= src && v < src + size) gw64(at, v - src + dst);
        }
}
/* NVN copies the commands of recordings into the command buffer, and the game may take the source's memory back at
   once: each record is copied (a recording it calls is still called where it is), across the source's segments */
void nvn_CommandBufferCopyCommands(CPU* c, u64 cb, i32 n, u64 handles) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 p = gr64(handles + 8 * (u64)i);
        for (;;) {
            NvnRecHdr h;
            u64 at;
            memcpy(&h, GP(p), sizeof h);
            if (h.op >= OP_COUNT_ || h.size < 8 || (h.size & 7))
                fatal("nvn: nvnCommandBufferCopyCommands of a record at %llx with op %u and size %u, which nvn_direct "
                      "never writes",
                      (unsigned long long)p, h.op, h.size);
            if (h.op == OP_END) break;
            if (h.op == OP_JUMP) {
                p = gr64(p + 8);
                continue;
            }
            at = alloc_keep(c, cb, h.size, OP_RESERVE); /* may run the game's memory callback */
            memcpy(GP(at), GP(p), h.size);
            relocate(at, p, h.op, h.size);
            p += h.size;
        }
    }
}
void nvn_CommandBufferCallCommands(CPU* c, u64 cb, i32 n, u64 handles) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 at = alloc_keep(c, cb, 16, OP_RESERVE), h = gr64(handles + 8 * (u64)i);
        put_hdr(at, OP_CALL, 16);
        memcpy(GP(at + 8), &h, 8);
    }
}

/* ============================================================ effects the CPU sees */
void nvn_CommandBufferFenceSync(CPU* c, u64 cb, u64 sync, i32 condition, i32 flags) {
    (void)condition;
    (void)flags;
    *(u64*)(void*)GP(rec(c, cb, OP_FENCE_SYNC, 8)) = sync;
}
/* the event's value written or added to when the GPU reaches it */
void nvn_CommandBufferSignalEvent(CPU* c, u64 cb, u64 e, i32 mode, i32 location, i32 flags, u32 value) {
    u64 address = NVN_OBJ(NvnEvent, e)->gpu;
    NvnPSignalEvent* r = REC(c, cb, OP_SIGNAL_EVENT, NvnPSignalEvent);
    (void)location;
    (void)flags;
    r->value = address;
    r->mode = (u32)mode;
    r->v = value;
}
void nvn_CommandBufferReportCounter(CPU* c, u64 cb, i32 type, u64 address) {
    NvnPReportCounter* r = REC(c, cb, OP_REPORT_COUNTER, NvnPReportCounter);
    r->type = (u64)(u32)type;
    r->address = address;
}
/* a 32-bit value written when the GPU reaches it: an event's write */
void nvn_CommandBufferReportValue(CPU* c, u64 cb, u32 value, u64 address) {
    NvnPSignalEvent* r = REC(c, cb, OP_SIGNAL_EVENT, NvnPSignalEvent);
    r->value = address;
    r->mode = NVN_EVENT_SIGNAL_MODE_WRITE;
    r->v = value;
}
void nvn_CommandBufferResetCounter(CPU* c, u64 cb, i32 type) {
    *(u64*)(void*)GP(rec(c, cb, OP_RESET_COUNTER, 8)) = (u64)(u32)type;
}

/* a copy the GPU makes, in order with the queue's work: phase 2's */
void nvn_CommandBufferCopyBufferToBuffer(CPU* c, u64 cb, u64 src, u64 dst, u64 size, i32 flags) {
    NvnPCopyB2B* r = REC(c, cb, OP_COPY_B2B, NvnPCopyB2B);
    (void)flags;
    r->src = src;
    r->dst = dst;
    r->size = size;
}
/* the bytes are copied into the record (the caller may change its own afterwards), and written when the GPU reaches
   them */
void nvn_CommandBufferUpdateUniformBuffer(CPU* c, u64 cb, u64 buffer, u64 buffer_size, i64 offset, u64 size,
                                          u64 data) {
    u64 payload = rec(c, cb, OP_UPDATE_UB, sizeof(NvnPUpdateUB) + (size_t)size);
    NvnPUpdateUB* r = (NvnPUpdateUB*)(void*)GP(payload);
    (void)buffer_size;
    r->dst = buffer + (u64)offset;
    r->size = size;
    memcpy(GP(payload + sizeof *r), GP(data), (size_t)size);
}

/* ============================================================ render targets, clears and fixed state */
/* A record with textures carries snapshots of their objects after its payload: once recorded, a command no longer
   depends on the object, which the game may finalize or reuse before it submits. An object that isn't a live
   texture as recorded keeps its address, read at submit */
#define SNAP_OFF(T) ((sizeof(T) + 7) & ~(size_t)7)
static u64 tex_snap(u64 payload, size_t off, int k, u64 t) {
    u64 s = payload + off + (u64)k * NVN_TEX_SNAP;
    if (!t) return 0;
    if (NVN_OBJ(NvnTexture, t)->magic != NVN_MAGIC('T', 'E', 'X', 0)) {
        LOG_LIMIT(4, "nvn: texture %llx isn't a live texture as a command is recorded: read at submit\n",
                  (unsigned long long)t);
        return t;
    }
    memcpy(GP(s), GP(t), NVN_TEX_SNAP);
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
    payload = rec(c, cb, OP_SET_RT, SNAP_OFF(NvnPSetRT) + (size_t)(n + 1) * NVN_TEX_SNAP);
    r = (NvnPSetRT*)(void*)GP(payload);
    r->count = n;
    for (i = 0; i < n; i++) {
        r->color[i] = colors ? tex_snap(payload, SNAP_OFF(NvnPSetRT), i, gr64(colors + 8 * (u64)i)) : 0;
        copy_view(&r->cview[i], &r->has_cview[i], views ? gr64(views + 8 * (u64)i) : 0);
    }
    r->depth = tex_snap(payload, SNAP_OFF(NvnPSetRT), n, depth);
    copy_view(&r->dview, &r->has_dview, depth_view);
}
void nvn_CommandBufferCopyBufferToTexture(CPU* c, u64 cb, u64 src, u64 t, u64 v, u64 region, i32 flags) {
    u64 payload = rec(c, cb, OP_COPY_B2T, SNAP_OFF(NvnPCopyB2T) + NVN_TEX_SNAP);
    NvnPCopyB2T* r = (NvnPCopyB2T*)(void*)GP(payload);
    r->buffer = src;
    r->texture = tex_snap(payload, SNAP_OFF(NvnPCopyB2T), 0, t);
    copy_view(&r->view, &r->has_view, v);
    memcpy(&r->r, GP(region), sizeof r->r);
    r->flags = flags;
}
void nvn_CommandBufferCopyTextureToBuffer(CPU* c, u64 cb, u64 t, u64 v, u64 region, u64 dst, i32 flags) {
    u64 payload = rec(c, cb, OP_COPY_T2B, SNAP_OFF(NvnPCopyB2T) + NVN_TEX_SNAP);
    NvnPCopyB2T* r = (NvnPCopyB2T*)(void*)GP(payload);
    r->buffer = dst;
    r->texture = tex_snap(payload, SNAP_OFF(NvnPCopyB2T), 0, t);
    copy_view(&r->view, &r->has_view, v);
    memcpy(&r->r, GP(region), sizeof r->r);
    r->flags = flags;
}
static NvnPCopyT2T* copy_t2t(CPU* c, u64 cb, u64 s, u64 sv, u64 d, u64 dv) {
    u64 payload = rec(c, cb, OP_COPY_T2T, SNAP_OFF(NvnPCopyT2T) + 2 * NVN_TEX_SNAP);
    NvnPCopyT2T* r = (NvnPCopyT2T*)(void*)GP(payload);
    r->src = tex_snap(payload, SNAP_OFF(NvnPCopyT2T), 0, s);
    r->dst = tex_snap(payload, SNAP_OFF(NvnPCopyT2T), 1, d);
    copy_view(&r->sv, &r->has_sv, sv);
    copy_view(&r->dv, &r->has_dv, dv);
    return r;
}
void nvn_CommandBufferCopyTextureToTexture(CPU* c, u64 cb, u64 s, u64 sv, u64 sr, u64 d, u64 dv, u64 dr, i32 flags) {
    NvnPCopyT2T* r = copy_t2t(c, cb, s, sv, d, dv);
    memcpy(&r->sr, GP(sr), sizeof r->sr);
    memcpy(&r->dr, GP(dr), sizeof r->dr);
    r->flags = flags;
}
/* a multisample resolve of the whole texture, recorded as a copy (nxnative's) */
static void downsample(CPU* c, u64 cb, u64 s, u64 sv, u64 d, u64 dv) {
    NvnPCopyT2T* r = copy_t2t(c, cb, s, sv, d, dv);
    if (s) {
        const NvnTexture* t = NVN_OBJ(const NvnTexture, s);
        r->sr.width = (int)t->d.width;
        r->sr.height = (int)t->d.height;
        r->sr.depth = 1;
    }
    if (d) {
        const NvnTexture* t = NVN_OBJ(const NvnTexture, d);
        r->dr.width = (int)t->d.width;
        r->dr.height = (int)t->d.height;
        r->dr.depth = 1;
    }
    r->flags = NVN_COPY_RESOLVE;
}
void nvn_CommandBufferDownsample(CPU* c, u64 cb, u64 s, u64 d) { downsample(c, cb, s, 0, d, 0); }
void nvn_CommandBufferDownsampleTextureView(CPU* c, u64 cb, u64 s, u64 sv, u64 d, u64 dv) {
    downsample(c, cb, s, sv, d, dv);
}
/* the same resolve, which the game's driver makes in pieces of the tiled cache's tiles: its result is a Downsample's */
void nvn_CommandBufferTiledDownsample(CPU* c, u64 cb, u64 s, u64 d) { downsample(c, cb, s, 0, d, 0); }
void nvn_CommandBufferTiledDownsampleTextureView(CPU* c, u64 cb, u64 s, u64 sv, u64 d, u64 dv) {
    downsample(c, cb, s, sv, d, dv);
}
static void clear_tex(CPU* c, u64 cb, u64 t, u64 v, u64 region, u64 value, i32 mask, u8 is_uint) {
    u64 payload = rec(c, cb, OP_CLEAR_TEX, SNAP_OFF(NvnPClearTex) + NVN_TEX_SNAP);
    NvnPClearTex* r = (NvnPClearTex*)(void*)GP(payload);
    r->texture = tex_snap(payload, SNAP_OFF(NvnPClearTex), 0, t);
    copy_view(&r->view, &r->has_view, v);
    memcpy(&r->r, GP(region), sizeof r->r);
    r->mask = mask;
    r->is_uint = is_uint;
    memcpy(r->c, GP(value), sizeof r->c);
}
void nvn_CommandBufferClearTexture(CPU* c, u64 cb, u64 t, u64 v, u64 region, u64 rgba, i32 mask) {
    clear_tex(c, cb, t, v, region, rgba, mask, 0);
}
void nvn_CommandBufferClearTextureui(CPU* c, u64 cb, u64 t, u64 v, u64 region, u64 rgba, i32 mask) {
    clear_tex(c, cb, t, v, region, rgba, mask, 1);
}
/* signed integers: their words as given, as an integer clear's (the target's format says how to read them) */
void nvn_CommandBufferClearTexturei(CPU* c, u64 cb, u64 t, u64 v, u64 region, u64 rgba, i32 mask) {
    clear_tex(c, cb, t, v, region, rgba, mask, 1);
}
static void copy_stride(CPU* c, u64 cb, i64 stride, i32 image) {
    NvnPCopyStride* r = REC(c, cb, OP_COPY_STRIDE, NvnPCopyStride);
    r->stride = stride;
    r->image = image;
}
/* recorded for the copies after them, and kept for the getters (32 bits, as the game's driver keeps them) */
void nvn_CommandBufferSetCopyRowStride(CPU* c, u64 cb, i64 stride) {
    copy_stride(c, cb, stride, 0);
    CB(cb)->row_stride = (i32)stride;
}
void nvn_CommandBufferSetCopyImageStride(CPU* c, u64 cb, i64 stride) {
    copy_stride(c, cb, stride, 1);
    CB(cb)->image_stride = (i32)stride;
}
i64 nvn_CommandBufferGetCopyRowStride(CPU* c, u64 cb) {
    (void)c;
    return CB(cb)->row_stride;
}
i64 nvn_CommandBufferGetCopyImageStride(CPU* c, u64 cb) {
    (void)c;
    return CB(cb)->image_stride;
}
/* the clear color's words as the game gives them: floats, or an integer target's integers, as the 3D engine's clear
   registers hold them (the renderer reads them by the target's format) */
static void clear_color(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    NvnPClearColor* r = REC(c, cb, OP_CLEAR_COLOR, NvnPClearColor);
    r->index = index;
    r->mask = mask;
    memcpy(r->rgba, GP(rgba), sizeof r->rgba);
}
void nvn_CommandBufferClearColor(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    clear_color(c, cb, index, rgba, mask);
}
void nvn_CommandBufferClearColori(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    clear_color(c, cb, index, rgba, mask);
}
void nvn_CommandBufferClearColorui(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    clear_color(c, cb, index, rgba, mask);
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
void nvn_CommandBufferSetViewports(CPU* c, u64 cb, i32 first, i32 n, u64 v) {
    NvnPViewports* r;
    i32 i;
    if (n < 0 || first < 0 || first + n > 16) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferSetViewports %d from %d\n", n, first);
        return;
    }
    r = RECN(c, cb, OP_VIEWPORTS, NvnPViewports, v, n);
    r->first = first;
    r->count = n;
    for (i = 0; i < n; i++) memcpy(r->v[i], GP(v + 16 * (u64)i), 16);
}
/* nxnative keeps scissor and depth range 0 only, as its replay has one each */
void nvn_CommandBufferSetScissors(CPU* c, u64 cb, i32 first, i32 n, u64 rects) {
    if (first == 0 && n >= 1)
        nvn_CommandBufferSetScissor(c, cb, (i32)gr32(rects), (i32)gr32(rects + 4), (i32)gr32(rects + 8),
                                    (i32)gr32(rects + 12));
    if (first + n > 1) LOG_LIMIT(1, "nvn: scissors %d to %d: only scissor 0 is recorded\n", first, first + n - 1);
}
void nvn_CommandBufferSetDepthRanges(CPU* c, u64 cb, i32 first, i32 n, u64 ranges) {
    if (first == 0 && n >= 1) {
        f32 r[2];
        memcpy(r, GP(ranges), sizeof r);
        nvn_CommandBufferSetDepthRange(c, cb, r[0], r[1]);
    }
    if (first + n > 1)
        LOG_LIMIT(1, "nvn: depth ranges %d to %d: only depth range 0 is recorded\n", first, first + n - 1);
}
void nvn_CommandBufferSetViewportSwizzles(CPU* c, u64 cb, i32 first, i32 n, u64 swizzles) {
    NvnPVpSwizzle* r;
    i32 i, k;
    if (n < 0 || first < 0 || first + n > 16) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferSetViewportSwizzles %d from %d\n", n, first);
        return;
    }
    r = REC(c, cb, OP_VP_SWIZZLE, NvnPVpSwizzle);
    r->first = first;
    r->count = n;
    for (i = 0; i < n; i++)
        for (k = 0; k < 4; k++) r->sw[i][k] = (u8)((gr32(swizzles + 4 * (u64)(4 * i + k)) ^ (u32)(2 * k)) & 7);
}
void nvn_CommandBufferSetPrimitiveRestart(CPU* c, u64 cb, int on, i32 index) {
    NvnPPrimRestart* r = REC(c, cb, OP_PRIM_RESTART, NvnPPrimRestart);
    r->on = on != 0;
    r->index = (u32)index;
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
    memcpy(GP(rec(c, cb, OP_LINE_WIDTH, 4)), &width, 4);
}
/* the vertex whose outputs a flat input takes: the mode is the 3D engine's (1 the last vertex), which the game's
   driver writes as it is (PROVOKING_VERTEX, 0x5A1) */
void nvn_CommandBufferSetProvokingVertexMode(CPU* c, u64 cb, i32 mode) {
    REC(c, cb, OP_PROVOKING, NvnPProvoking)->mode = mode;
}
void nvn_CommandBufferSetSampleMask(CPU* c, u64 cb, i32 mask) {
    REC(c, cb, OP_SAMPLE_MASK, NvnPSampleMask)->mask = mask;
}
void nvn_CommandBufferSetDepthClamp(CPU* c, u64 cb, int on) {
    REC(c, cb, OP_DEPTH_CLAMP, NvnPDepthClamp)->on = on != 0;
}
void nvn_CommandBufferSetPolygonOffsetClamp(CPU* c, u64 cb, f32 factor, f32 units, f32 clamp) {
    NvnPPolyOffset* r = REC(c, cb, OP_POLY_OFFSET, NvnPPolyOffset);
    r->factor = factor;
    r->units = units;
    r->clamp = clamp;
}
void nvn_CommandBufferSetRenderEnable(CPU* c, u64 cb, int on) {
    REC(c, cb, OP_RENDER_COND, NvnPRenderCond)->mode = on ? 2 : 3;
}
void nvn_CommandBufferSetRenderEnableConditional(CPU* c, u64 cb, i32 mode, u64 address) {
    NvnPRenderCond* r = REC(c, cb, OP_RENDER_COND, NvnPRenderCond);
    r->mode = mode & 1;
    r->address = address;
}
/* not recorded, as in nxnative: the depth bounds test has no counterpart there, and the point size comes from the
   shader */
void nvn_CommandBufferSetDepthBounds(CPU* c, u64 cb, int on, f32 min, f32 max) {
    (void)c;
    (void)cb;
    (void)on;
    (void)min;
    (void)max;
}
void nvn_CommandBufferSetPointSize(CPU* c, u64 cb, f32 size) {
    (void)c;
    (void)cb;
    (void)size;
}
void nvn_CommandBufferBarrier(CPU* c, u64 cb, i32 bits) { *(i32*)(void*)GP(rec(c, cb, OP_BARRIER, 4)) = bits; }
/* a descriptor pool the command buffer's handles index from now on: its descriptors' address and count */
static void set_pool(CPU* c, u64 cb, u32 op, u64 dp) {
    const NvnDescPool* p = NVN_OBJ(NvnDescPool, dp);
    u64 descriptors = nvn_pool_gpu(p->pool) + p->offset;
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
void nvn_CommandBufferWaitEvent(CPU* c, u64 cb, u64 e, i32 mode, u32 value) {
    (void)c;
    (void)cb;
    (void)e;
    (void)mode;
    (void)value;
}
/* hints the replay has no use for, as nxnative: an alpha reference without an alpha test, discards (the contents are
   undefined after, so keeping them is right), ZCull */
void nvn_CommandBufferSetAlphaRef(CPU* c, u64 cb, f32 ref) {
    (void)c;
    (void)cb;
    (void)ref;
}
/* Conservative rasterization (CONSERVATIVE_RASTER, 0x452) and its dilation: the game's driver rounds the dilation up
   to quarter pixels, at most 3 (0.75 pixels), and gives it to the hardware through a macro, with the raster bounding
   box's pad set while it is above 0 */
void nvn_CommandBufferSetConservativeRasterEnable(CPU* c, u64 cb, int on) {
    NvnPConservative* r = REC(c, cb, OP_CONSERVATIVE, NvnPConservative);
    r->which = 0;
    r->on = on != 0;
}
void nvn_CommandBufferSetConservativeRasterDilate(CPU* c, u64 cb, f32 dilate) {
    NvnPConservative* r = REC(c, cb, OP_CONSERVATIVE, NvnPConservative);
    f32 q = dilate * 4.0f;
    r->which = 1;
    r->dilate = q > 2.0f ? 3 : q > 1.0f ? 2 : q > 0.0f ? 1 : 0; /* ceil, clamped to 0..3; a NaN takes 0 */
}
/* The hardware's hints and optimizations, with nothing the renderer can show:
   - color reduction (SET_REDUCE_COLOR_THRESHOLDS_ENABLE, 0x367, and the thresholds of each format class): the ROP's
     shortcut that skips blending where the source alpha is past a threshold, which may change a pixel by little. The
     renderer blends exactly, as with it off;
   - the subpixel precision bias (NV_conservative_raster's), the rasterizer's extra bits of subpixel precision, which
     Vulkan has no counterpart of;
   - ZCull's compression of 32-bit float depth, and the tiled cache's subtile size: the renderer has neither;
   - ResolveDepthBuffer, the game's driver's DECOMPRESS_ZETA_SURFACE (0x47F): it writes a compressed depth buffer's
     plane equations out as values, which a depth texture read then sees. The renderer's depth images hold their
     values, so they read the same without it;
   - the coverage modulation table (NV_framebuffer_mixed_samples): the renderer rasterizes at the targets' samples
     (gpu/nvn/nvn_draw.c), so its coverage needs no modulation;
   - the default tessellation levels (TESSELLATION_OUTER 0xC9, INNER 0xCD), which apply only to a program without a
     tessellation control stage. The renderer's tessellation takes the levels from that stage, which Vulkan requires;
   - checkpoints, which only NVIDIA's tools read (the release driver answers 1 and records nothing) */
void nvn_CommandBufferSetColorReductionEnable(CPU* c, u64 cb, int on) {
    (void)c;
    (void)cb;
    (void)on;
}
void nvn_CommandBufferSetColorReductionThresholds(CPU* c, u64 cb, i32 format_class, i32 conservative,
                                                  i32 aggressive) {
    (void)c;
    (void)cb;
    (void)format_class;
    (void)conservative;
    (void)aggressive;
}
void nvn_CommandBufferSetSubpixelPrecisionBias(CPU* c, u64 cb, i32 x, i32 y) {
    (void)c;
    (void)cb;
    (void)x;
    (void)y;
}
void nvn_CommandBufferSetZCullZF32CompressionEnable(CPU* c, u64 cb, int on) {
    (void)c;
    (void)cb;
    (void)on;
}
void nvn_CommandBufferOverrideSubtileSize(CPU* c, u64 cb, int on, i32 size) {
    (void)c;
    (void)cb;
    (void)on;
    (void)size;
}
void nvn_CommandBufferResolveDepthBuffer(CPU* c, u64 cb) {
    (void)c;
    (void)cb;
}
void nvn_CommandBufferBindCoverageModulationTable(CPU* c, u64 cb, u64 entries) {
    (void)c;
    (void)cb;
    (void)entries;
}
void nvn_CommandBufferSetInnerTessellationLevels(CPU* c, u64 cb, u64 levels) {
    (void)c;
    (void)cb;
    (void)levels;
}
void nvn_CommandBufferSetOuterTessellationLevels(CPU* c, u64 cb, u64 levels) {
    (void)c;
    (void)cb;
    (void)levels;
}
int nvn_CommandBufferInsertCheckpointStatic(CPU* c, u64 cb, u64 label) {
    (void)c;
    (void)cb;
    (void)label;
    return 1;
}
int nvn_CommandBufferInsertCheckpointDynamic(CPU* c, u64 cb, u64 label) {
    (void)c;
    (void)cb;
    (void)label;
    return 1;
}
/* a program's subroutine uniforms: the translator runs no subroutines (nvn_ProgramSetSubroutineLinkage) */
void nvn_CommandBufferSetProgramSubroutines(CPU* c, u64 cb, u64 program, i32 stage, i32 first, i32 n, u64 values) {
    (void)c;
    (void)cb;
    (void)stage;
    (void)first;
    (void)values;
    LOG_LIMIT(1, "nvn: %d subroutine uniforms of program %llx: the runtime's programs have no subroutines\n", n,
              (unsigned long long)program);
}
void nvn_CommandBufferDiscardColor(CPU* c, u64 cb, i32 index) {
    (void)c;
    (void)cb;
    (void)index;
}
void nvn_CommandBufferDiscardDepthStencil(CPU* c, u64 cb) {
    (void)c;
    (void)cb;
}
/* ZCull's stencil criteria: what the hardware's coarse culling may assume of the stencil test, which only speeds it
   up. The renderer has no ZCull, so the draws' tests give the same pixels without it */
void nvn_CommandBufferSetStencilCullCriteria(CPU* c, u64 cb, i32 func, i32 ref, i32 mask) {
    (void)c;
    (void)cb;
    (void)func;
    (void)ref;
    (void)mask;
}
void nvn_CommandBufferSaveZCullData(CPU* c, u64 cb, u64 address, u64 size) {
    (void)c;
    (void)cb;
    (void)address;
    (void)size;
}
void nvn_CommandBufferRestoreZCullData(CPU* c, u64 cb, u64 address, u64 size) {
    (void)c;
    (void)cb;
    (void)address;
    (void)size;
}
void nvn_CommandBufferSetPatchSize(CPU* c, u64 cb, i32 size) {
    (void)c;
    (void)cb;
    (void)size;
}
/* debug markers and groups name the work for NVIDIA's tools */
void nvn_CommandBufferInsertDebugMarker(CPU* c, u64 cb, u64 text) {
    (void)c;
    (void)cb;
    (void)text;
}
void nvn_CommandBufferInsertDebugMarkerDynamic(CPU* c, u64 cb, u32 domain, u64 text) {
    (void)c;
    (void)cb;
    (void)domain;
    (void)text;
}
void nvn_CommandBufferInsertDebugMarkerStatic(CPU* c, u64 cb, u32 domain, u64 text) {
    (void)c;
    (void)cb;
    (void)domain;
    (void)text;
}
void nvn_CommandBufferPushDebugGroup(CPU* c, u64 cb, u32 domain, u64 text) {
    (void)c;
    (void)cb;
    (void)domain;
    (void)text;
}
void nvn_CommandBufferPushDebugGroupDynamic(CPU* c, u64 cb, u32 domain, u64 text) {
    (void)c;
    (void)cb;
    (void)domain;
    (void)text;
}
void nvn_CommandBufferPushDebugGroupStatic(CPU* c, u64 cb, u32 domain, u64 text) {
    (void)c;
    (void)cb;
    (void)domain;
    (void)text;
}
void nvn_CommandBufferPopDebugGroup(CPU* c, u64 cb) {
    (void)c;
    (void)cb;
}
void nvn_CommandBufferPopDebugGroupId(CPU* c, u64 cb, u32 domain) {
    (void)c;
    (void)cb;
    (void)domain;
}
/* the work recorded after it waits for the sync: the queue's later work is held behind it at submission, as
   QueueWaitSync holds it (gpu/nvn/nvn_replay.c promise). One game waits for its window texture's sync only this way */
void nvn_CommandBufferWaitSync(CPU* c, u64 cb, u64 sync) { *(u64*)(void*)GP(rec(c, cb, OP_WAIT_SYNC, 8)) = sync; }

/* ============================================================ binds */
/* the program's stages as they are now: the game may set new shaders in it before it submits */
void nvn_CommandBufferBindProgram(CPU* c, u64 cb, u64 program, i32 stages) {
    NvnPBindProgram* r = REC(c, cb, OP_BIND_PROGRAM, NvnPBindProgram);
    const NvnProgram* p = program ? NVN_OBJ(const NvnProgram, program) : NULL;
    r->stages = stages;
    if (p && p->magic == NVN_MAGIC('P', 'R', 'O', 'G')) {
        r->off_a = p->off_a;
        memcpy(r->st, p->st, sizeof r->st);
        r->serial = p->serial;
        memcpy(r->block, p->block, sizeof r->block);
        r->sample_shading = p->sample_shading;
    } else if (program)
        LOG_LIMIT(4, "nvn: program %llx isn't initialized as it is bound\n", (unsigned long long)program);
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
void nvn_CommandBufferBindVertexBuffers(CPU* c, u64 cb, i32 first, i32 n, u64 ranges) {
    NvnPBindVBuf* r;
    if (n < 0 || first < 0 || first + n > 16) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferBindVertexBuffers %d from %d\n", n, first);
        return;
    }
    r = RECN(c, cb, OP_BIND_VBUF, NvnPBindVBuf, r, n);
    r->first = first;
    r->count = n;
    memcpy(r->r, GP(ranges), sizeof(NVNbufferRange) * (size_t)n);
}
void nvn_CommandBufferBindStorageBuffers(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 ranges) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 at = ranges + (u64)i * sizeof(NVNbufferRange);
        bind_buf(c, cb, OP_BIND_SSBO, stage, first + i, gr64(at), gr64(at + 8));
    }
}
/* handles, recorded with their count: textures (combined, or separate in slots 128 on, as the hardware keeps the two
   in tables of their own), samplers and images */
#define NVN_SEPTEX_SLOT0 128
static NvnPBindHandles* bind_handles(CPU* c, u64 cb, u32 op, i32 stage, i32 first, i32 n, u64 handles) {
    NvnPBindHandles* r;
    if (n < 0 || n > 32) {
        LOG_LIMIT(4, "nvn: a bind of %d handles\n", n);
        return NULL;
    }
    r = RECN(c, cb, op, NvnPBindHandles, h, n);
    r->stage = stage;
    r->first = first;
    r->count = n;
    if (n) memcpy(r->h, GP(handles), 8 * (size_t)n);
    return r;
}
void nvn_CommandBufferBindTextures(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 handles) {
    bind_handles(c, cb, OP_BIND_TEX, stage, first, n, handles);
}
/* the GPU reads the handles where they are when it runs the command, not as recorded: the address is recorded, and
   the handles as they were for a replay that has nothing better */
void nvn_CommandBufferBindTexturesDeferred(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 handles) {
    NvnPBindHandles* r = bind_handles(c, cb, OP_BIND_TEX, stage, first, n, handles);
    if (r && handles) {
        r->defer = 1;
        r->src = handles;
    }
}
void nvn_CommandBufferBindSeparateTextures(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 handles) {
    bind_handles(c, cb, OP_BIND_TEX, stage, NVN_SEPTEX_SLOT0 + first, n, handles);
}
void nvn_CommandBufferBindSeparateSamplers(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 handles) {
    bind_handles(c, cb, OP_BIND_SAMP, stage, first, n, handles);
}
void nvn_CommandBufferBindImages(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 handles) {
    bind_handles(c, cb, OP_BIND_IMG, stage, first, n, handles);
}
/* deferred binds of the other kinds, as BindTexturesDeferred: the handles where they are as the GPU runs it */
static void bind_deferred(CPU* c, u64 cb, u32 op, i32 stage, i32 first, i32 n, u64 handles) {
    NvnPBindHandles* r = bind_handles(c, cb, op, stage, first, n, handles);
    if (r && handles) {
        r->defer = 1;
        r->src = handles;
    }
}
void nvn_CommandBufferBindImagesDeferred(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 handles) {
    bind_deferred(c, cb, OP_BIND_IMG, stage, first, n, handles);
}
void nvn_CommandBufferBindSeparateTexturesDeferred(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 handles) {
    bind_deferred(c, cb, OP_BIND_TEX, stage, NVN_SEPTEX_SLOT0 + first, n, handles);
}
void nvn_CommandBufferBindSeparateSamplersDeferred(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 handles) {
    bind_deferred(c, cb, OP_BIND_SAMP, stage, first, n, handles);
}
/* uniform and storage buffers whose NVNbufferRanges the GPU reads where they are as it runs the command */
static void bind_bufs_deferred(CPU* c, u64 cb, u32 op, i32 stage, i32 first, i32 n, u64 ranges) {
    NvnPBindBufs* r;
    if (n <= 0 || !ranges) return;
    r = REC(c, cb, OP_BIND_BUFS, NvnPBindBufs);
    r->op = (i32)op;
    r->stage = stage;
    r->first = first;
    r->count = n;
    r->src = ranges;
}
void nvn_CommandBufferBindUniformBuffersDeferred(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 ranges) {
    bind_bufs_deferred(c, cb, OP_BIND_UBO, stage, first, n, ranges);
}
void nvn_CommandBufferBindStorageBuffersDeferred(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 ranges) {
    bind_bufs_deferred(c, cb, OP_BIND_SSBO, stage, first, n, ranges);
}
static void bind_one(CPU* c, u64 cb, u32 op, i32 stage, i32 slot, u64 handle) {
    NvnPBindHandles* r = RECN(c, cb, op, NvnPBindHandles, h, 1);
    r->stage = stage;
    r->first = slot;
    r->count = 1;
    r->h[0] = handle;
}
/* an image handle is its descriptor's ID in the texture pool */
void nvn_CommandBufferBindImage(CPU* c, u64 cb, i32 stage, i32 slot, u64 handle) {
    bind_one(c, cb, OP_BIND_IMG, stage, slot, handle);
}
void nvn_CommandBufferBindSeparateTexture(CPU* c, u64 cb, i32 stage, i32 slot, u64 handle) {
    bind_one(c, cb, OP_BIND_TEX, stage, NVN_SEPTEX_SLOT0 + slot, handle);
}
void nvn_CommandBufferBindSeparateSampler(CPU* c, u64 cb, i32 stage, i32 slot, u64 handle) {
    bind_one(c, cb, OP_BIND_SAMP, stage, slot, handle);
}
void nvn_CommandBufferBindTransformFeedbackBuffers(CPU* c, u64 cb, i32 first, i32 n, u64 ranges) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 at = ranges + (u64)i * sizeof(NVNbufferRange);
        NvnPTfBind* r = REC(c, cb, OP_TF_BIND, NvnPTfBind);
        r->index = first + i;
        r->address = gr64(at);
        r->size = gr64(at + 8);
    }
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
void nvn_CommandBufferPauseTransformFeedback(CPU* c, u64 cb, u64 control) { tf_state(c, cb, 2, control); }
void nvn_CommandBufferResumeTransformFeedback(CPU* c, u64 cb, u64 control) { tf_state(c, cb, 3, control); }

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
/* the arguments at ind are read by the GPU when it draws */
void nvn_CommandBufferDrawArraysIndirect(CPU* c, u64 cb, i32 prim, u64 ind) {
    NvnPDraw d = {0};
    LOG_LIMIT(1, "nvn: the game records indirect draws (DrawArraysIndirect)\n");
    d.prim = prim;
    d.instances = 1;
    d.ind = ind;
    memcpy(GP(rec(c, cb, OP_DRAW, sizeof d)), &d, sizeof d);
}
void nvn_CommandBufferDrawElementsIndirect(CPU* c, u64 cb, i32 prim, i32 type, u64 ib, u64 ind) {
    NvnPDrawIdx d = {0};
    LOG_LIMIT(1, "nvn: the game records indirect draws (DrawElementsIndirect)\n");
    d.prim = prim;
    d.index_type = type;
    d.instances = 1;
    d.index_buffer = ib;
    d.ind = ind;
    memcpy(GP(rec(c, cb, OP_DRAW_IDX, sizeof d)), &d, sizeof d);
}
/* an indirect-count multi-draw: up to max indirect draws, stride bytes apart, as many as the count the GPU reads at
   count says. One record: the replay hands the whole of it to the GPU */
void nvn_CommandBufferMultiDrawElementsIndirectCount(CPU* c, u64 cb, i32 prim, i32 type, u64 ib, u64 ind, u64 count,
                                                     i32 max, i64 stride) {
    NvnPDrawIdx d = {0};
    LOG_LIMIT(1, "nvn: the game records indirect multi-draws (MultiDrawElementsIndirectCount)\n");
    if (max <= 0) return;
    d.prim = prim;
    d.index_type = type;
    d.instances = 1;
    d.index_buffer = ib;
    d.ind = ind;
    d.mdi_count = count;
    d.mdi_max = max;
    d.mdi_stride = (i32)stride;
    memcpy(GP(rec(c, cb, OP_DRAW_IDX, sizeof d)), &d, sizeof d);
}
void nvn_CommandBufferMultiDrawArraysIndirectCount(CPU* c, u64 cb, i32 prim, u64 ind, u64 count, i32 max,
                                                   i64 stride) {
    NvnPDraw d = {0};
    LOG_LIMIT(1, "nvn: the game records indirect multi-draws (MultiDrawArraysIndirectCount)\n");
    if (max <= 0) return;
    d.prim = prim;
    d.instances = 1;
    d.ind = ind;
    d.mdi_count = count;
    d.mdi_max = max;
    d.mdi_stride = (i32)stride;
    memcpy(GP(rec(c, cb, OP_DRAW, sizeof d)), &d, sizeof d);
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
void nvn_CommandBufferDrawElementsBaseVertex(CPU* c, u64 cb, i32 prim, i32 type, i32 count, u64 ib, i32 base_vertex) {
    draw_idx(c, cb, prim, type, count, ib, base_vertex, 0, 1);
}
/* caller: the game's call site, which names the dispatcher */
void nvn_CommandBufferDispatchCompute(CPU* c, u64 cb, i32 x, i32 y, i32 z) {
    u64 caller = c->x[30];
    NvnPDispatch* r = REC(c, cb, OP_DISPATCH, NvnPDispatch);
    r->x = x;
    r->y = y;
    r->z = z;
    r->caller = caller;
}
/* the group counts at ind are read by the GPU when it dispatches */
void nvn_CommandBufferDispatchComputeIndirect(CPU* c, u64 cb, u64 ind) {
    u64 caller = c->x[30];
    NvnPDispatch* r = REC(c, cb, OP_DISPATCH, NvnPDispatch);
    LOG_LIMIT(1, "nvn: the game records indirect dispatches (DispatchComputeIndirect)\n");
    r->caller = caller;
    r->ind = ind;
}
/* a texture's rectangle drawn onto color target 0 (the 3D engine's DRAW_TEXTURE, which the game's driver writes with
   the state around it): the handle's texture and sampler, the destination in pixels and the source in texels, each
   x0 y0 x1 y1. An empty destination draws nothing */
void nvn_CommandBufferDrawTexture(CPU* c, u64 cb, u64 handle, u64 dst, u64 src) {
    NvnPDrawTexture r;
    memset(&r, 0, sizeof r); /* every byte: the record is guest memory */
    r.handle = handle;
    memcpy(r.dst, GP(dst), sizeof r.dst);
    memcpy(r.src, GP(src), sizeof r.src);
    memcpy(GP(rec(c, cb, OP_DRAW_TEXTURE, sizeof r)), &r, sizeof r);
}
/* drawing the vertex count transform feedback captured: nxnative records nothing, and neither does this */
void nvn_CommandBufferDrawTransformFeedback(CPU* c, u64 cb, i32 prim, u64 control) {
    (void)c;
    (void)cb;
    (void)prim;
    (void)control;
    LOG_LIMIT(1, "nvn: nvnCommandBufferDrawTransformFeedback isn't recorded\n");
}
u64 nvn_CommandBufferGetMemoryCallback(CPU* c, u64 cb) {
    (void)c;
    return CB(cb)->callback;
}
u64 nvn_CommandBufferGetMemoryCallbackData(CPU* c, u64 cb) {
    (void)c;
    return CB(cb)->callback_data;
}
