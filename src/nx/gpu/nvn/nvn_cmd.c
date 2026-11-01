/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/hle_nvn.c */
/* nvn_direct's command buffers: the game's NVN commands recorded into the command memory it adds, in nxnative's
   record format (gpu/nvn/nvn_obj.h), for the queue to submit.

   Memory, as NVN hands it out: the game adds command memory (AddCommandMemory, a pool and a range) and records into
   it; when the game's driver would run out of it, the command buffer calls the game's memory callback, which adds
   more. GetCommandMemoryUsed counts the bytes the game's driver writes for each command (its methods, measured
   against it), not the records' own: games size what they allocate for a recording, and place the
   next one, by it, and one game sizes heaps by a table of what the driver takes on the console, which a recording
   counted bigger overflows. A command nobody measured counts its record's size.

   The records are bigger than the driver's methods for many commands (a bind's 32 bytes against 20), so they stay in
   the game's memory only within the bytes counted so far: the game's memory never holds a byte of ours past what
   GetCommandMemoryUsed has told it. A record that doesn't fit there goes on in a piece of the runtime's own memory
   (spill), linked by a JUMP, and the rest of the recording with it. A piece stands for its recording's bytes in the
   game's memory as the driver would have written them (its anchor, where the recording starts there): it comes back
   once the driver would have written over them, the game's memory reused, or the game finalizes their pool. Adding
   memory again isn't writing over it: one game hands the rest of a block to its next recording at the driver's used
   bytes, adds the whole block again for the next ones, and still calls recordings further in it. A recording that
   takes no command memory on the driver (calls and compute binds go to its control memory) counts 4 bytes, so that it
   has bytes of its own, past which the game places the next one.

   A recording ends with its last record (NVN_REC_LAST), as the driver's takes no bytes at EndRecording: one without
   records is the runtime's one END.

   Every NVN command is only recorded here: the queue's replay runs the records on the GPU thread once they are
   submitted (gpu/nvn/nvn_replay.c) */
#include "nx/gpu/nvn/nvn_cmd.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_state.h"
#include "gpu/nvn/nvn.hook.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <string.h>

#define CB(a) NVN_OBJ(NvnCmdBuf, a)
/* room a piece of the runtime's memory keeps after every record: one JUMP to the next piece */
#define OP_RESERVE 16
/* the driver's bytes of a command nobody measured: its record's size */
#define COST_RECORD             0xFFFFFFFFu
/* the driver's bytes of a bind to the compute stage: the driver keeps compute binds in control memory (measured for
   textures and uniform buffers; storage buffers taken to do the same) */
#define NVN_STAGE_COMPUTE       5
#define BIND_COST(stage, bytes) ((stage) == NVN_STAGE_COMPUTE ? 0u : (bytes))

/* Runs the guest function fn(a0, a1, a2, a3) on this thread from inside an NVN call, as the driver calls the game's
   callbacks: its return (to the NVN call's own return address, which nothing else returns to) ends the run, and the
   NVN call's link register is put back for its own return. The registers the callback may clobber are the ones an
   NVN call may clobber */
static void guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    u64 lr = c->x[30], pc = fn;
    if (g_nvn.direct.a32) { /* a 32-bit game's callback: AAPCS32, each argument in a word, and back to lr */
        SnxArmCPU* a = arm_cpu(c);
        u32 lr32 = a->r[14];
        a->r[0] = (u32)a0;
        a->r[1] = (u32)a1;
        a->r[2] = (u32)a2;
        a->r[3] = (u32)a3;
        a->r[15] = (u32)fn & ~1u;
        a->cpsr = (a->cpsr & ~SNX_ARM_T) | (fn & 1 ? SNX_ARM_T : 0);
        arm_run(a, NULL, lr32);
        a->r[14] = lr32;
        return;
    }
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    do snx_lookup(c, pc)(c, pc);
    while ((pc = c->pc) != lr);
    c->x[30] = lr;
}

/* the return address of the NVN call that c runs: its call site */
static u64 nvn_caller(CPU* c) { return g_nvn.direct.a32 ? arm_cpu(c)->r[14] : c->x[30]; }

/* a record's header: its op, its size, and the driver's bytes for its command (which a copy of it counts again) */
static void put_hdr(u64 at, u32 op, u32 size, u32 cost) {
    NvnRecHdr h;
    h.op = op | (cost > NVN_REC_COST_MAX ? NVN_REC_COST_MAX : cost) << NVN_REC_COST_SHIFT;
    h.size = size;
    memcpy(GP(at), &h, sizeof h);
}
/* the target before the header: a walk of another submission that reads the JUMP reads where it goes (nvn_replay.c) */
static void put_jump(u64 at, u64 to) {
    memcpy(GP(at + 8), &to, 8);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    put_hdr(at, OP_JUMP, 16, 0);
}

/* ---- the runtime's memory for the records the game's memory has no room for */
#define SPILL_SEGMENT (64u << 10)
/* the piece of segment k cb cuts now ends at end: the segment's next piece starts there, or at the segment's start once
   none of its pieces is in use */
static void spill_stop(u32 k, u64 end) {
    NvnSpillSeg* s;
    pm_lock(&g_nvn.cmd.lock);
    s = &g_nvn.cmd.s[k];
    s->owner = 0;
    s->top = (end - s->at + 7) & ~7ull;
    if (!s->live) s->quiet = nvn_replay_submitted();
    pm_unlock(&g_nvn.cmd.lock);
}
/* cb's next piece, for the recording anchored at anchor, room for need bytes and a JUMP after them at least: cut from
   a segment nobody cuts from, or from a new one. Its segment's index, *at its start */
static u32 spill_cut(u64 cb, u64 anchor, u32 need, u64* at) {
    NvnCmdState* st = &g_nvn.cmd;
    u64 want = (u64)need + OP_RESERVE;
    u32 k, i, slot, b;
    pm_lock(&st->lock);
    for (i = 0, k = st->ns; i < st->ns; i++) { /* from the segment the last piece came from */
        u32 t = (st->hint + i) % st->ns;
        NvnSpillSeg* q = &st->s[t];
        /* A segment whose pieces all came back is cut from its start again once the GPU thread has read every
           submission made by then: the game may give the memory a recording is anchored in to another command buffer
           as soon as a sync of its own signals, while a submission that calls the recording waits to be replayed */
        if (!q->owner && !q->live && q->top && (i64)(nvn_replay_walked() - q->quiet) >= 0) {
            q->top = 0;
            q->reset = st->serial;
        }
        if (!st->s[t].owner && st->s[t].size - st->s[t].top >= want) {
            k = t;
            break;
        }
    }
    if (k == st->ns) {
        u64 size = want > SPILL_SEGMENT / 2 ? (want * 2 + 0xffff) & ~0xffffull : SPILL_SEGMENT, was = st->total;
        if (st->ns == NVN_SPILL_SEGS)
            fatal("nvn: command records past the game's command memory need more than the %u segments of the "
                  "runtime's own memory nvn_direct holds (%llu KiB)",
                  NVN_SPILL_SEGS, (unsigned long long)(st->total >> 10));
        st->s[k].at = kmem_alloc(size, MS_KERNEL, PERM_RW);
        st->s[k].size = size;
        st->s[k].top = 0;
        st->ns++;
        st->total += size;
        if ((st->total ^ was) > was) /* a new power of two of the total */
            LOG("nvn: %llu KiB of the runtime's memory carry command records past the game's command memory\n",
                (unsigned long long)(st->total >> 10));
    }
    if (st->free)
        st->free = st->p[slot = st->free].next;
    else if (st->slots < NVN_SPILL_PIECES)
        slot = ++st->slots;
    else
        fatal("nvn: more than %u pieces of the runtime's memory carry command records at once, which nvn_direct holds",
              NVN_SPILL_PIECES);
    b = (u32)(anchor >> 12) % NVN_SPILL_BUCKETS;
    st->p[slot].anchor = anchor;
    st->p[slot].seg = k;
    st->p[slot].serial = ++st->serial;
    st->p[slot].next = st->bucket[b];
    st->bucket[b] = slot;
    st->np++;
    st->hint = k;
    st->s[k].owner = cb;
    st->s[k].live++;
    *at = st->s[k].at + st->s[k].top;
    pm_unlock(&st->lock);
    return k;
}
/* the pieces anchored in [from, to) and cut before serial before come back: the driver would have written over their
   recordings. A segment with none left in use is cut again from its start. The serial of the last piece cut */
static u32 spill_back(u64 from, u64 to, u32 before) {
    NvnCmdState* st = &g_nvn.cmd;
    u64 page, last;
    u32 serial;
    pm_lock(&st->lock);
    serial = st->serial;
    page = from >> 12;
    last = from < to ? (to - 1) >> 12 : 0;
    if (from < to && last - page >= NVN_SPILL_BUCKETS) { /* every bucket, once */
        page = 0;
        last = NVN_SPILL_BUCKETS - 1;
    }
    for (; from < to && page <= last; page++) {
        u32* at = &st->bucket[page % NVN_SPILL_BUCKETS];
        while (*at) {
            NvnSpillPiece* q = &st->p[*at];
            u32 slot = *at;
            NvnSpillSeg* s;
            if (q->anchor < from || q->anchor >= to || q->serial >= before) { /* not over it, or cut since */
                at = &q->next;
                continue;
            }
            *at = q->next;
            q->next = st->free;
            st->free = slot;
            q->anchor = 0; /* free: nvn_cmd_describe lists the others */
            st->np--;
            s = &st->s[q->seg];
            if (!--s->live && !s->owner) s->quiet = nvn_replay_submitted();
        }
    }
    pm_unlock(&st->lock);
    return serial;
}
void nvn_cmd_memory_back(u64 memory, u64 size) { spill_back(memory, memory + size, ~0u); }
int nvn_cmd_unfinished(u64 p) {
    NvnCmdState* st = &g_nvn.cmd;
    u32 k;
    int r = 0;
    pm_lock(&st->lock);
    for (k = 0; k < st->ns; k++)
        if (p >= st->s[k].at && p < st->s[k].at + st->s[k].size) {
            r = st->s[k].owner && p >= st->s[k].at + st->s[k].top;
            break;
        }
    pm_unlock(&st->lock);
    return r;
}
void nvn_cmd_describe(u64 p, char* out, size_t n) {
    NvnCmdState* st = &g_nvn.cmd;
    u32 k, i, at;
    int w;
    if (!n) return;
    out[0] = 0;
    pm_lock(&st->lock);
    for (k = 0; k < st->ns && (p < st->s[k].at || p >= st->s[k].at + st->s[k].size); k++) {}
    if (k < st->ns) {
        const NvnSpillSeg* s = &st->s[k];
        w = snprintf(out, n,
                     " (the runtime's segment %u at %llx: cut to +%llx, %u pieces in use, reset after cut %u of %u;"
                     " its pieces' cuts:",
                     k, (unsigned long long)s->at, (unsigned long long)s->top, s->live, s->reset, st->serial);
        at = w > 0 && (size_t)w < n ? (u32)w : (u32)n - 1;
        for (i = 1; i <= st->slots && at + 16 < n; i++)
            if (st->p[i].seg == k && st->p[i].anchor) {
                w = snprintf(out + at, n - at, " %u", st->p[i].serial);
                if (w > 0) at += (u32)w;
            }
        snprintf(out + at, n - at, ")");
    }
    pm_unlock(&st->lock);
}
/* the bytes cb counted in the game's memory since its last look: the pieces anchored there come back, and the ones cut
   from now on are past this look */
static void written(u64 cb) {
    NvnCmdBuf* o = CB(cb);
    o->seen_serial = spill_back(o->origin + o->seen, o->origin + o->used, o->seen_serial) + 1;
    o->seen = o->used;
}
/* the runtime's one recording without records: an END */
static u64 empty_recording(void) {
    u64 at;
    pm_lock(&g_nvn.cmd.lock);
    if (!g_nvn.cmd.empty) {
        g_nvn.cmd.empty = kmem_alloc(0x1000, MS_KERNEL, PERM_RW);
        put_hdr(g_nvn.cmd.empty, OP_END, 8, 0);
    }
    at = g_nvn.cmd.empty;
    pm_unlock(&g_nvn.cmd.lock);
    return at;
}

/* A record's snapshot pointers (a texture object copied into the record, tex_snap) that point into the record at src,
   moved to its copy at dst */
static void relocate(u64 dst, u64 src, u32 op, u32 size) {
    static const struct {
        u32 op, off, n;
    } snaps[] = {
        {OP_SET_RT, offsetof(NvnPSetRT, color), 8},         {OP_SET_RT, offsetof(NvnPSetRT, depth), 1},
        {OP_COPY_B2T, offsetof(NvnPCopyB2T, texture), 1},   {OP_COPY_T2B, offsetof(NvnPCopyB2T, texture), 1},
        {OP_COPY_T2T, offsetof(NvnPCopyT2T, src), 1},       {OP_COPY_T2T, offsetof(NvnPCopyT2T, dst), 1},
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
/* The recording goes on in a new piece of the runtime's memory, room for need bytes (a record's, and one moved): from
   the game's memory, its records there are linked to it by a JUMP after them, or, with no room for one in the bytes
   counted, the last record moves into the piece and the JUMP takes its place (every record is 16 bytes at least) */
static void divert(u64 cb, u32 need) {
    NvnCmdBuf* o = CB(cb);
    u64 from = 0, at;
    NvnRecHdr h = {0};
    o->game_cur = o->cur;
    if (o->recording == 1 && o->last) { /* the recording's records in the game's memory end at cur */
        if (o->cur + 16 <= o->origin + o->used)
            from = o->cur;
        else {
            from = o->last;
            memcpy(&h, GP(from), sizeof h);
        }
        o->game_cur = from + 16;
    }
    o->lent = spill_cut(cb, o->anchor, need + h.size, &at) + 1;
    o->cur = at;
    if (h.size) {
        memcpy(GP(at), GP(from), h.size);
        relocate(at, from, NVN_REC_OP(h.op), h.size);
        o->last = at;
        o->cur += h.size;
    }
    if (from) put_jump(from, at);
}
/* the piece is full: the recording goes on in the next one */
static void spill_next(u64 cb, u32 need) {
    NvnCmdBuf* o = CB(cb);
    u64 from = o->cur, at;
    u32 old = o->lent - 1, k = spill_cut(cb, o->anchor, need, &at);
    if (o->recording == 1 && o->last) {
        put_jump(from, at);
        from += 16;
    }
    spill_stop(old, from);
    o->lent = k + 1;
    o->cur = at;
}
/* back to the game's memory once a recording is done with the runtime's */
static void spill_end(u64 cb) {
    NvnCmdBuf* o = CB(cb);
    if (!o->lent) return;
    spill_stop(o->lent - 1, o->cur);
    o->lent = 0;
    o->cur = o->game_cur;
}
/* the end of the memory the next record goes in: the game's, or the runtime's piece */
static u64 area_end(const NvnCmdBuf* o) {
    const NvnSpillSeg* s = &g_nvn.cmd.s[o->lent ? o->lent - 1 : 0];
    return o->lent ? s->at + s->size : o->game_end;
}

/* the driver's bytes of a command (cost) taken from the game's memory: past it, the game's callback is asked for more,
   as the driver asks it; with none added, the command counts nothing (0) */
static int count(CPU* c, u64 cb, u32 cost) {
    NvnCmdBuf* o = CB(cb);
    if (o->used + cost > o->game_end - o->origin && o->callback) {
        LOG_LIMIT(4, "nvn: command buffer %llx is out of command memory: the game's callback %llx adds more\n",
                  (unsigned long long)cb, (unsigned long long)o->callback);
        guest_call(c, o->callback, cb, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, cost, o->callback_data);
    }
    if (o->used + cost > o->game_end - o->origin) return 0;
    o->used += cost;
    return 1;
}
/* room for one record of op, size bytes, whose command the driver writes in cost bytes (COST_RECORD: the record's
   size): in the game's memory while it fits in the bytes counted, else in the runtime's. Its guest address, its header
   written */
static u64 alloc_rec(CPU* c, u64 cb, u32 op, u32 size, u32 cost) {
    NvnCmdBuf* o = CB(cb);
    u64 at;
    size = (size + 7) & ~7u;
    if (cost == COST_RECORD) cost = size;
    if (count(c, cb, cost) && o->recording == 2)
        o->anchor = o->origin + o->used - cost; /* where the driver would start the recording */
    else if (o->recording == 2)
        o->anchor = o->origin + o->used;
    /* a recording's records in the game's memory lie in its own bytes: the ones before its anchor are an earlier
       recording's, though that one's records may be in the runtime's memory */
    if (o->recording == 2 && !o->lent && o->cur < ((o->anchor + 7) & ~7ull)) o->cur = (o->anchor + 7) & ~7ull;
    if (!o->lent && (!o->cur || o->cur + size > o->origin + o->used)) divert(cb, size);
    if (o->lent && o->cur + size + OP_RESERVE > area_end(o)) spill_next(cb, size);
    at = o->cur;
    o->cur += size;
    put_hdr(at, op, size, cost);
    if (o->recording == 2) { /* the recording's first record is its handle */
        o->begin = at;
        o->recording = 1;
    }
    o->last = at;
    return at;
}

/* a record of op with payload bytes, zeroed, whose command the driver writes in cost bytes: the payload's address */
static u64 rec(CPU* c, u64 cb, u32 op, size_t payload, u32 cost) {
    u32 size = (u32)((8 + payload + 7) & ~(size_t)7);
    u64 at = alloc_rec(c, cb, op, size, cost);
    memset(GP(at + 8), 0, size - 8); /* guest memory: every byte the game sees */
    return at + 8;
}
#define REC(c, cb, OP, T, COST) ((T*)(void*)GP(rec((c), (cb), (OP), sizeof(T), (COST))))
/* a record of an array type, with its first n elements */
#define RECN(c, cb, OP, T, ARR, N, COST)                                                                               \
    ((T*)(void*)GP(rec((c), (cb), (OP), offsetof(T, ARR) + sizeof(((T*)0)->ARR[0]) * (size_t)(N), (COST))))

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
/* The game's memory from now on (GetCommandMemoryUsed counts from it, across recordings). A recording in progress
   goes on in the runtime's memory, its records so far where they are */
void nvn_CommandBufferAddCommandMemory(CPU* c, u64 cb, u64 pool, i64 offset, u64 size) {
    NvnCmdBuf* o = CB(cb);
    u64 memory = nvn_pool_memory(pool);
    (void)c;
    /* nxnative lends host memory for an empty pool (a game whose callback found none): not needed by any game yet */
    if (!memory)
        fatal("nvn: command buffer %llx was given %llx bytes of command memory from pool %llx, which has no storage",
              (unsigned long long)cb, (unsigned long long)size, (unsigned long long)pool);
    if (o->recording == 1 && !o->lent) divert(cb, 0);
    written(cb);
    o->origin = memory + (u64)offset;
    o->game_end = o->origin + size;
    o->used = 0;
    o->seen = 0;
    if (o->lent)
        o->game_cur = (o->origin + 7) & ~7ull;
    else
        o->cur = (o->origin + 7) & ~7ull;
}
/* the control memory is the driver's own bookkeeping: the records need none. Its size is kept for the getters */
void nvn_CommandBufferAddControlMemory(CPU* c, u64 cb, u64 memory, u64 size) {
    (void)c;
    (void)memory;
    CB(cb)->control_size = size;
}
/* The memory the game last added and what is left of it as the game's driver counts it, which games check before they
   record. The control memory is never used */
u64 nvn_CommandBufferGetCommandMemorySize(CPU* c, u64 cb) {
    const NvnCmdBuf* o = CB(cb);
    (void)c;
    return o->game_end - o->origin;
}
u64 nvn_CommandBufferGetCommandMemoryFree(CPU* c, u64 cb) {
    const NvnCmdBuf* o = CB(cb);
    (void)c;
    return o->game_end - o->origin - o->used;
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
/* a recording starts with its first command, as in NVN */
void nvn_CommandBufferBeginRecording(CPU* c, u64 cb) {
    NvnCmdBuf* o = CB(cb);
    (void)c;
    if (o->recording) LOG_LIMIT(4, "nvn: command buffer %llx began recording twice\n", (unsigned long long)cb);
    spill_end(cb);
    o->recording = 2;
    o->last = 0;
}
/* the recording ends with its last record, which takes no bytes more, as the driver's EndRecording writes none; one
   without records is the runtime's one END */
u64 nvn_CommandBufferEndRecording(CPU* c, u64 cb) {
    NvnCmdBuf* o = CB(cb);
    u64 h;
    (void)c;
    if (o->recording == 1 && o->last) {
        NvnRecHdr r;
        memcpy(&r, GP(o->last), sizeof r);
        r.op |= NVN_REC_LAST;
        memcpy(GP(o->last), &r, sizeof r);
        h = o->begin;
        /* bytes of its own, the driver's being control memory: no callback, which the driver wouldn't call */
        if (o->origin + o->used == o->anchor && o->used + 4 <= o->game_end - o->origin) o->used += 4;
    } else
        h = empty_recording();
    spill_end(cb);
    written(cb);
    o->recording = 0;
    o->last = 0;
    return h;
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
/* NVN copies the commands of recordings into the command buffer, and the game may take the source's memory back at
   once: each record is copied (a recording it calls is still called where it is), across the source's segments, and
   counts the driver's bytes it counted where it was recorded */
void nvn_CommandBufferCopyCommands(CPU* c, u64 cb, i32 n, u64 handles) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 p = gr64(handles + 8 * (u64)i);
        for (;;) {
            NvnRecHdr h;
            u64 at;
            u32 op;
            memcpy(&h, GP(p), sizeof h);
            op = NVN_REC_OP(h.op);
            if (op >= OP_COUNT_ || h.size < 8 || (h.size & 7))
                fatal("nvn: nvnCommandBufferCopyCommands of a record at %llx with op %u and size %u, which nvn_direct "
                      "never writes",
                      (unsigned long long)p, op, h.size);
            if (op == OP_END) break;
            if (op == OP_JUMP) {
                p = gr64(p + 8);
                continue;
            }
            at = alloc_rec(c, cb, op, h.size, NVN_REC_COST(h.op)); /* may run the game's memory callback */
            memcpy(GP(at + 8), GP(p + 8), h.size - 8);
            relocate(at, p, op, h.size);
            if (h.op & NVN_REC_LAST) break;
            p += h.size;
        }
    }
}
void nvn_CommandBufferCallCommands(CPU* c, u64 cb, i32 n, u64 handles) {
    i32 i;
    for (i = 0; i < n; i++) {
        /* the driver writes a call to control memory, none of it command memory */
        u64 at = alloc_rec(c, cb, OP_CALL, 16, 0), h = gr64(handles + 8 * (u64)i);
        memcpy(GP(at + 8), &h, 8);
    }
}

/* ============================================================ effects the CPU sees */
void nvn_CommandBufferFenceSync(CPU* c, u64 cb, u64 sync, i32 condition, i32 flags) {
    (void)condition;
    (void)flags;
    *(u64*)(void*)GP(rec(c, cb, OP_FENCE_SYNC, 8, COST_RECORD)) = sync;
}
/* the event's value written or added to when the GPU reaches it */
void nvn_CommandBufferSignalEvent(CPU* c, u64 cb, u64 e, i32 mode, i32 location, i32 flags, u32 value) {
    u64 address = NVN_OBJ(NvnEvent, e)->gpu;
    NvnPSignalEvent* r = REC(c, cb, OP_SIGNAL_EVENT, NvnPSignalEvent, COST_RECORD);
    (void)location;
    (void)flags;
    r->value = address;
    r->mode = (u32)mode;
    r->v = value;
}
void nvn_CommandBufferReportCounter(CPU* c, u64 cb, i32 type, u64 address) {
    NvnPReportCounter* r = REC(c, cb, OP_REPORT_COUNTER, NvnPReportCounter, 24);
    r->type = (u64)(u32)type;
    r->address = address;
}
/* a 32-bit value written when the GPU reaches it: an event's write */
void nvn_CommandBufferReportValue(CPU* c, u64 cb, u32 value, u64 address) {
    NvnPSignalEvent* r = REC(c, cb, OP_SIGNAL_EVENT, NvnPSignalEvent, COST_RECORD);
    r->value = address;
    r->mode = NVN_EVENT_SIGNAL_MODE_WRITE;
    r->v = value;
}
void nvn_CommandBufferResetCounter(CPU* c, u64 cb, i32 type) {
    *(u64*)(void*)GP(rec(c, cb, OP_RESET_COUNTER, 8, COST_RECORD)) = (u64)(u32)type;
}

/* a copy the GPU makes, in order with the queue's work: phase 2's */
void nvn_CommandBufferCopyBufferToBuffer(CPU* c, u64 cb, u64 src, u64 dst, u64 size, i32 flags) {
    NvnPCopyB2B* r = REC(c, cb, OP_COPY_B2B, NvnPCopyB2B, COST_RECORD);
    (void)flags;
    r->src = src;
    r->dst = dst;
    r->size = size;
}
/* the bytes are copied into the record (the caller may change its own afterwards), and written when the GPU reaches
   them. The driver writes 36 bytes and the data's words */
void nvn_CommandBufferUpdateUniformBuffer(CPU* c, u64 cb, u64 buffer, u64 buffer_size, i64 offset, u64 size,
                                          u64 data) {
    u64 payload = rec(c, cb, OP_UPDATE_UB, sizeof(NvnPUpdateUB) + (size_t)size, (u32)(36 + ((size + 3) & ~3ull)));
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
    /* the driver's 96 bytes measured for one color target without depth */
    payload = rec(c, cb, OP_SET_RT, SNAP_OFF(NvnPSetRT) + (size_t)(n + 1) * NVN_TEX_SNAP,
                  n == 1 && !depth ? 96 : COST_RECORD);
    r = (NvnPSetRT*)(void*)GP(payload);
    r->count = n;
    for (i = 0; i < n; i++) {
        r->color[i] =
            colors ? tex_snap(payload, SNAP_OFF(NvnPSetRT), i, nvn_gptr(colors + nvn_ptr_bytes() * (u64)i)) : 0;
        copy_view(&r->cview[i], &r->has_cview[i], views ? nvn_gptr(views + nvn_ptr_bytes() * (u64)i) : 0);
    }
    r->depth = tex_snap(payload, SNAP_OFF(NvnPSetRT), n, depth);
    copy_view(&r->dview, &r->has_dview, depth_view);
}
void nvn_CommandBufferCopyBufferToTexture(CPU* c, u64 cb, u64 src, u64 t, u64 v, u64 region, i32 flags) {
    u64 payload = rec(c, cb, OP_COPY_B2T, SNAP_OFF(NvnPCopyB2T) + NVN_TEX_SNAP, COST_RECORD);
    NvnPCopyB2T* r = (NvnPCopyB2T*)(void*)GP(payload);
    r->buffer = src;
    r->texture = tex_snap(payload, SNAP_OFF(NvnPCopyB2T), 0, t);
    copy_view(&r->view, &r->has_view, v);
    memcpy(&r->r, GP(region), sizeof r->r);
    r->flags = flags;
}
void nvn_CommandBufferCopyTextureToBuffer(CPU* c, u64 cb, u64 t, u64 v, u64 region, u64 dst, i32 flags) {
    u64 payload = rec(c, cb, OP_COPY_T2B, SNAP_OFF(NvnPCopyB2T) + NVN_TEX_SNAP, COST_RECORD);
    NvnPCopyB2T* r = (NvnPCopyB2T*)(void*)GP(payload);
    r->buffer = dst;
    r->texture = tex_snap(payload, SNAP_OFF(NvnPCopyB2T), 0, t);
    copy_view(&r->view, &r->has_view, v);
    memcpy(&r->r, GP(region), sizeof r->r);
    r->flags = flags;
}
static NvnPCopyT2T* copy_t2t(CPU* c, u64 cb, u64 s, u64 sv, u64 d, u64 dv) {
    u64 payload = rec(c, cb, OP_COPY_T2T, SNAP_OFF(NvnPCopyT2T) + 2 * NVN_TEX_SNAP, COST_RECORD);
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
    u64 payload = rec(c, cb, OP_CLEAR_TEX, SNAP_OFF(NvnPClearTex) + NVN_TEX_SNAP, COST_RECORD);
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
    NvnPCopyStride* r = REC(c, cb, OP_COPY_STRIDE, NvnPCopyStride, COST_RECORD);
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
static void clear_color(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask, u32 cost) {
    NvnPClearColor* r = REC(c, cb, OP_CLEAR_COLOR, NvnPClearColor, cost);
    r->index = index;
    r->mask = mask;
    memcpy(r->rgba, GP(rgba), sizeof r->rgba);
}
void nvn_CommandBufferClearColor(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    clear_color(c, cb, index, rgba, mask, 48);
}
void nvn_CommandBufferClearColori(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    clear_color(c, cb, index, rgba, mask, COST_RECORD);
}
void nvn_CommandBufferClearColorui(CPU* c, u64 cb, i32 index, u64 rgba, i32 mask) {
    clear_color(c, cb, index, rgba, mask, COST_RECORD);
}
void nvn_CommandBufferClearDepthStencil(CPU* c, u64 cb, f32 depth, int depth_mask, i32 stencil, i32 stencil_mask) {
    NvnPClearDS* r = REC(c, cb, OP_CLEAR_DS, NvnPClearDS, 0); /* the driver clears through control memory */
    r->depth = depth;
    r->depth_mask = depth_mask;
    r->stencil = stencil;
    r->stencil_mask = stencil_mask;
}
void nvn_CommandBufferClearBuffer(CPU* c, u64 cb, u64 address, u64 size, u32 value) {
    NvnPClearBuf* r = REC(c, cb, OP_CLEAR_BUF, NvnPClearBuf, COST_RECORD);
    r->address = address;
    r->size = size;
    r->value = value;
}
/* one viewport, as SetViewports(0, 1): the record holds the viewports it sets */
void nvn_CommandBufferSetViewport(CPU* c, u64 cb, i32 x, i32 y, i32 w, i32 h) {
    NvnPViewports* r = RECN(c, cb, OP_VIEWPORTS, NvnPViewports, v, 1, 36);
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
    r = RECN(c, cb, OP_VIEWPORTS, NvnPViewports, v, n, COST_RECORD);
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
    r = REC(c, cb, OP_VP_SWIZZLE, NvnPVpSwizzle, COST_RECORD);
    r->first = first;
    r->count = n;
    for (i = 0; i < n; i++)
        for (k = 0; k < 4; k++) r->sw[i][k] = (u8)((gr32(swizzles + 4 * (u64)(4 * i + k)) ^ (u32)(2 * k)) & 7);
}
void nvn_CommandBufferSetPrimitiveRestart(CPU* c, u64 cb, int on, i32 index) {
    NvnPPrimRestart* r = REC(c, cb, OP_PRIM_RESTART, NvnPPrimRestart, COST_RECORD);
    r->on = on != 0;
    r->index = (u32)index;
}
void nvn_CommandBufferSetScissor(CPU* c, u64 cb, i32 x, i32 y, i32 w, i32 h) {
    NvnPScissor* r = REC(c, cb, OP_SCISSOR, NvnPScissor, 12);
    r->x = x;
    r->y = y;
    r->w = w;
    r->h = h;
}
void nvn_CommandBufferSetDepthRange(CPU* c, u64 cb, f32 n, f32 f) {
    NvnPDepthRange* r = REC(c, cb, OP_DEPTH_RANGE, NvnPDepthRange, 28);
    r->n = n;
    r->f = f;
}
static void stencil(CPU* c, u64 cb, i32 face, i32 value, int which) {
    NvnPStencil* r = REC(c, cb, OP_STENCIL, NvnPStencil, COST_RECORD);
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
    memcpy(REC(c, cb, OP_BLEND_COLOR, NvnPBlendColor, 20)->rgba, GP(rgba), 16);
}
void nvn_CommandBufferSetRasterizerDiscard(CPU* c, u64 cb, int on) {
    REC(c, cb, OP_RAST_DISCARD, NvnPRastDiscard, COST_RECORD)->on = on != 0;
}
void nvn_CommandBufferSetLineWidth(CPU* c, u64 cb, f32 width) {
    memcpy(GP(rec(c, cb, OP_LINE_WIDTH, 4, 12)), &width, 4);
}
/* the vertex whose outputs a flat input takes: the mode is the 3D engine's (1 the last vertex), which the game's
   driver writes as it is (PROVOKING_VERTEX, 0x5A1) */
void nvn_CommandBufferSetProvokingVertexMode(CPU* c, u64 cb, i32 mode) {
    REC(c, cb, OP_PROVOKING, NvnPProvoking, COST_RECORD)->mode = mode;
}
void nvn_CommandBufferSetSampleMask(CPU* c, u64 cb, i32 mask) {
    REC(c, cb, OP_SAMPLE_MASK, NvnPSampleMask, COST_RECORD)->mask = mask;
}
void nvn_CommandBufferSetDepthClamp(CPU* c, u64 cb, int on) {
    REC(c, cb, OP_DEPTH_CLAMP, NvnPDepthClamp, 8)->on = on != 0;
}
void nvn_CommandBufferSetPolygonOffsetClamp(CPU* c, u64 cb, f32 factor, f32 units, f32 clamp) {
    NvnPPolyOffset* r = REC(c, cb, OP_POLY_OFFSET, NvnPPolyOffset, 24);
    r->factor = factor;
    r->units = units;
    r->clamp = clamp;
}
void nvn_CommandBufferSetRenderEnable(CPU* c, u64 cb, int on) {
    REC(c, cb, OP_RENDER_COND, NvnPRenderCond, 4)->mode = on ? 2 : 3;
}
void nvn_CommandBufferSetRenderEnableConditional(CPU* c, u64 cb, i32 mode, u64 address) {
    NvnPRenderCond* r = REC(c, cb, OP_RENDER_COND, NvnPRenderCond, 16);
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
/* the driver writes 12, 16 or 32 bytes by the bits: the fewest */
void nvn_CommandBufferBarrier(CPU* c, u64 cb, i32 bits) { *(i32*)(void*)GP(rec(c, cb, OP_BARRIER, 4, 12)) = bits; }
/* a descriptor pool the command buffer's handles index from now on: its descriptors' address and count */
static void set_pool(CPU* c, u64 cb, u32 op, u64 dp) {
    const NvnDescPool* p = NVN_OBJ(NvnDescPool, dp);
    u64 descriptors = nvn_pool_gpu(p->pool) + p->offset;
    i32 count = p->count;
    NvnPSetPool* r = REC(c, cb, op, NvnPSetPool, 0); /* the driver sets the pools in control memory */
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
    NvnPConservative* r = REC(c, cb, OP_CONSERVATIVE, NvnPConservative, COST_RECORD);
    r->which = 0;
    r->on = on != 0;
}
void nvn_CommandBufferSetConservativeRasterDilate(CPU* c, u64 cb, f32 dilate) {
    NvnPConservative* r = REC(c, cb, OP_CONSERVATIVE, NvnPConservative, COST_RECORD);
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
void nvn_CommandBufferWaitSync(CPU* c, u64 cb, u64 sync) {
    *(u64*)(void*)GP(rec(c, cb, OP_WAIT_SYNC, 8, COST_RECORD)) = sync;
}

/* ============================================================ binds */
/* the program's stages as they are now: the game may set new shaders in it before it submits */
void nvn_CommandBufferBindProgram(CPU* c, u64 cb, u64 program, i32 stages) {
    NvnPBindProgram* r = REC(c, cb, OP_BIND_PROGRAM, NvnPBindProgram, 128);
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
static void bind_state(CPU* c, u64 cb, i32 kind, u64 st, size_t n, u32 cost) {
    NvnPBindState* r = REC(c, cb, OP_BIND_STATE, NvnPBindState, cost);
    r->kind = kind;
    memcpy(r->bytes, GP(st), n);
}
void nvn_CommandBufferBindBlendState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 0, st, 8, 40); }
void nvn_CommandBufferBindColorState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 1, st, 4, 12); }
void nvn_CommandBufferBindChannelMaskState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 2, st, 4, 20); }
void nvn_CommandBufferBindMultisampleState(CPU* c, u64 cb, u64 st) {
    bind_state(c, cb, 3, st, sizeof(NvnMultisample), COST_RECORD);
}
/* the driver writes 20 or 24 bytes for a polygon state and 8 or 16 for a depth-stencil one, by its contents: the fewest */
void nvn_CommandBufferBindPolygonState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 4, st, 4, 20); }
void nvn_CommandBufferBindDepthStencilState(CPU* c, u64 cb, u64 st) { bind_state(c, cb, 5, st, 8, 8); }
void nvn_CommandBufferBindVertexAttribState(CPU* c, u64 cb, i32 n, u64 st) {
    NvnPBindVAttr* r;
    if (n < 0 || n > 16) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferBindVertexAttribState of %d attributes\n", n);
        return;
    }
    r = RECN(c, cb, OP_BIND_VATTR, NvnPBindVAttr, st, n, 68); /* the driver writes all 16 attributes */
    r->count = n;
    memcpy(r->st, GP(st), 4 * (size_t)n);
}
void nvn_CommandBufferBindVertexStreamState(CPU* c, u64 cb, i32 n, u64 st) {
    NvnPBindVStream* r;
    if (n < 0 || n > 16) {
        LOG_LIMIT(4, "nvn: nvnCommandBufferBindVertexStreamState of %d streams\n", n);
        return;
    }
    r = RECN(c, cb, OP_BIND_VSTREAM, NvnPBindVStream, st, n, (u32)(64 + 4 * n));
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
    r = RECN(c, cb, OP_BIND_VBUF, NvnPBindVBuf, r, 1, 24);
    r->first = index;
    r->count = 1;
    r->r[0].address = address;
    r->r[0].size = size;
}
static void bind_buf(CPU* c, u64 cb, u32 op, i32 stage, i32 index, u64 address, u64 size, u32 cost) {
    NvnPBindBuf* r = REC(c, cb, op, NvnPBindBuf, cost);
    r->stage = stage;
    r->index = index;
    r->address = address;
    r->size = size;
}
void nvn_CommandBufferBindUniformBuffer(CPU* c, u64 cb, i32 stage, i32 index, u64 address, u64 size) {
    bind_buf(c, cb, OP_BIND_UBO, stage, index, address, size, BIND_COST(stage, 20));
}
void nvn_CommandBufferBindUniformBuffers(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 ranges) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 at = ranges + (u64)i * sizeof(NVNbufferRange);
        bind_buf(c, cb, OP_BIND_UBO, stage, first + i, gr64(at), gr64(at + 8), COST_RECORD);
    }
}
void nvn_CommandBufferBindStorageBuffer(CPU* c, u64 cb, i32 stage, i32 index, u64 address, u64 size) {
    bind_buf(c, cb, OP_BIND_SSBO, stage, index, address, size, BIND_COST(stage, 20));
}
/* one combined handle, laid out as the hardware's command (nvn_obj.h NvnPBindTex1) */
void nvn_CommandBufferBindTexture(CPU* c, u64 cb, i32 stage, i32 slot, u64 handle) {
    NvnPBindTex1* r = REC(c, cb, OP_BIND_TEX1, NvnPBindTex1, BIND_COST(stage, 20));
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
    r = RECN(c, cb, OP_BIND_VBUF, NvnPBindVBuf, r, n, COST_RECORD);
    r->first = first;
    r->count = n;
    memcpy(r->r, GP(ranges), sizeof(NVNbufferRange) * (size_t)n);
}
void nvn_CommandBufferBindStorageBuffers(CPU* c, u64 cb, i32 stage, i32 first, i32 n, u64 ranges) {
    i32 i;
    for (i = 0; i < n; i++) {
        u64 at = ranges + (u64)i * sizeof(NVNbufferRange);
        bind_buf(c, cb, OP_BIND_SSBO, stage, first + i, gr64(at), gr64(at + 8), COST_RECORD);
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
    r = RECN(c, cb, op, NvnPBindHandles, h, n, COST_RECORD);
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
    r = REC(c, cb, OP_BIND_BUFS, NvnPBindBufs, COST_RECORD);
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
    NvnPBindHandles* r = RECN(c, cb, op, NvnPBindHandles, h, 1, COST_RECORD);
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
        NvnPTfBind* r = REC(c, cb, OP_TF_BIND, NvnPTfBind, COST_RECORD);
        r->index = first + i;
        r->address = gr64(at);
        r->size = gr64(at + 8);
    }
}
void nvn_CommandBufferBindTransformFeedbackBuffer(CPU* c, u64 cb, i32 index, u64 address, u64 size) {
    NvnPTfBind* r = REC(c, cb, OP_TF_BIND, NvnPTfBind, COST_RECORD);
    r->index = index;
    r->address = address;
    r->size = size;
}
static void tf_state(CPU* c, u64 cb, i32 what, u64 control) {
    NvnPTfState* r = REC(c, cb, OP_TF_STATE, NvnPTfState, COST_RECORD);
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
    memcpy(GP(rec(c, cb, OP_DRAW, offsetof(NvnPDraw, ind), COST_RECORD)), &d, offsetof(NvnPDraw, ind));
}
static void draw_idx(CPU* c, u64 cb, i32 prim, i32 type, i32 count, u64 ib, i32 base_vertex, i32 base_instance,
                     i32 instances, u32 cost) {
    NvnPDrawIdx d = {0};
    d.prim = prim;
    d.index_type = type;
    d.count = count;
    d.base_vertex = base_vertex;
    d.base_instance = base_instance;
    d.instances = instances;
    d.index_buffer = ib;
    memcpy(GP(rec(c, cb, OP_DRAW_IDX, offsetof(NvnPDrawIdx, ind), cost)), &d, offsetof(NvnPDrawIdx, ind));
}
/* the arguments at ind are read by the GPU when it draws */
void nvn_CommandBufferDrawArraysIndirect(CPU* c, u64 cb, i32 prim, u64 ind) {
    NvnPDraw d = {0};
    LOG_LIMIT(1, "nvn: the game records indirect draws (DrawArraysIndirect)\n");
    d.prim = prim;
    d.instances = 1;
    d.ind = ind;
    memcpy(GP(rec(c, cb, OP_DRAW, sizeof d, COST_RECORD)), &d, sizeof d);
}
void nvn_CommandBufferDrawElementsIndirect(CPU* c, u64 cb, i32 prim, i32 type, u64 ib, u64 ind) {
    NvnPDrawIdx d = {0};
    LOG_LIMIT(1, "nvn: the game records indirect draws (DrawElementsIndirect)\n");
    d.prim = prim;
    d.index_type = type;
    d.instances = 1;
    d.index_buffer = ib;
    d.ind = ind;
    memcpy(GP(rec(c, cb, OP_DRAW_IDX, sizeof d, COST_RECORD)), &d, sizeof d);
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
    memcpy(GP(rec(c, cb, OP_DRAW_IDX, sizeof d, COST_RECORD)), &d, sizeof d);
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
    memcpy(GP(rec(c, cb, OP_DRAW, sizeof d, COST_RECORD)), &d, sizeof d);
}
void nvn_CommandBufferDrawArrays(CPU* c, u64 cb, i32 prim, i32 first, i32 count) {
    draw(c, cb, prim, first, count, 1, 0);
}
void nvn_CommandBufferDrawArraysInstanced(CPU* c, u64 cb, i32 prim, i32 first, i32 count, i32 base_instance,
                                          i32 instances) {
    draw(c, cb, prim, first, count, instances, base_instance);
}
void nvn_CommandBufferDrawElements(CPU* c, u64 cb, i32 prim, i32 type, i32 count, u64 ib) {
    draw_idx(c, cb, prim, type, count, ib, 0, 0, 1, 24);
}
void nvn_CommandBufferDrawElementsInstanced(CPU* c, u64 cb, i32 prim, i32 type, i32 count, u64 ib, i32 base_vertex,
                                            i32 base_instance, i32 instances) {
    draw_idx(c, cb, prim, type, count, ib, base_vertex, base_instance, instances, 44); /* 44 or 48: the fewest */
}
void nvn_CommandBufferDrawElementsBaseVertex(CPU* c, u64 cb, i32 prim, i32 type, i32 count, u64 ib, i32 base_vertex) {
    draw_idx(c, cb, prim, type, count, ib, base_vertex, 0, 1, 48);
}
/* caller: the game's call site, which names the dispatcher */
void nvn_CommandBufferDispatchCompute(CPU* c, u64 cb, i32 x, i32 y, i32 z) {
    u64 caller = nvn_caller(c);
    NvnPDispatch* r = REC(c, cb, OP_DISPATCH, NvnPDispatch, COST_RECORD);
    r->x = x;
    r->y = y;
    r->z = z;
    r->caller = caller;
}
/* the group counts at ind are read by the GPU when it dispatches */
void nvn_CommandBufferDispatchComputeIndirect(CPU* c, u64 cb, u64 ind) {
    u64 caller = nvn_caller(c);
    NvnPDispatch* r = REC(c, cb, OP_DISPATCH, NvnPDispatch, COST_RECORD);
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
    memcpy(GP(rec(c, cb, OP_DRAW_TEXTURE, sizeof r, COST_RECORD)), &r, sizeof r);
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
