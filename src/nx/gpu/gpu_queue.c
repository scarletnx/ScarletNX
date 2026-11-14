/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The queue from the GPU front end to the renderer's side (gpu_queue.h). A ring of records, each with its fence.
   The front end puts a record in and goes on. The records run in their order, now on the front end's thread right
   after they are queued: a renderer thread of their own takes them later, and nothing in their order changes then.

   A guest write of the front end (LOAD_CB's constant buffer data, inline-to-memory, a release) is a record, so a draw
   queued before it reads the memory from before it. The front end's own reads of memory wait for the fence of the
   last write queued over their pages (written[]), and no longer */
#include "nx/gpu/gpu_queue.h"
#include <stdlib.h>
#include <string.h>
#include "nx/runtime/platform/log.h"

enum { GQ_WRITE, GQ_RELEASE, GQ_I2M_BLOCK, GQ_CALL };
typedef struct {
    u32 kind;  /* GQ_ */
    u32 bytes; /* the record's bytes, this header and its payload */
    u64 fence; /* its sequence number */
    u64 gva;   /* the write's address (GQ_CALL: its function) */
    u64 size;  /* its bytes after the header (GQ_I2M_BLOCK: the upload's GpuUploadRegs, then its bytes) */
} GqRecord;
#define GQ_RING       (8u << 20)
#define GQ_ALIGN      16u
/* written[]: for each slot of pages, the highest fence that wrote one of them. Two pages in one slot share it, so a
   read can wait longer than it needs, never less */
#define GQ_SLOT_BITS  16
static struct {
    u8* ring;
    GqRecord* open; /* the call between gq_call_begin and gq_call_end, in the ring */
    void* big;      /* or its bytes outside it, too many for the ring: the call runs from there */
    GqFn big_fn;
    u64 big_bytes;
    u64 head, tail; /* bytes put in and taken out, from the start: modulo GQ_RING in the ring */
    u64 queued;     /* the fence of the last record put in */
    u64 done;       /* the fence of the last record that ran */
    u64 written[1u << GQ_SLOT_BITS];
} g_gq;
static _Thread_local int t_gq_run; /* this thread runs a record: its writes are the record's own */

static u32 gq_slot(u64 page) { return (u32)((page * 0x9E3779B97F4A7C15ull) >> (64 - GQ_SLOT_BITS)); }
static void gq_mark(u64 gva, u64 size, u64 fence) {
    if (!size) return;
    u64 first = gva >> 12, last = (gva + size - 1) >> 12;
    if (last - first >= (1u << GQ_SLOT_BITS)) last = first + (1u << GQ_SLOT_BITS) - 1; /* every slot already */
    for (u64 p = first; p <= last; p++) g_gq.written[gq_slot(p)] = fence;
}
static void gq_run(const GqRecord* r) {
    const u8* payload = (const u8*)(r + 1);
    t_gq_run = 1;
    switch (r->kind) {
        case GQ_WRITE: gmem_write_now(r->gva, payload, r->size); break;
        case GQ_RELEASE: gpu_release_write_now(r->gva, payload, (u32)r->size); break;
        case GQ_I2M_BLOCK: {
            GpuI2m t = {0};
            memcpy(&t.regs, payload, sizeof t.regs);
            t.inner_buffer = (u8*)payload + sizeof t.regs;
            t.copy_size = (u32)(r->size - sizeof t.regs);
            gpu_i2m_block_now(&t);
            break;
        }
        case GQ_CALL: {
            GqFn fn;
            memcpy(&fn, &r->gva, sizeof fn);
            fn(payload, r->size);
            break;
        }
        default: fatal("gpu: queue record of kind %u\n", r->kind);
    }
    t_gq_run = 0;
}
/* the records queued and not run: on this thread until a renderer thread takes them */
static void gq_take(void) {
    while (g_gq.tail != g_gq.head) {
        const GqRecord* r = (const GqRecord*)(g_gq.ring + g_gq.tail % GQ_RING);
        gq_run(r);
        g_gq.tail += r->bytes;
        __atomic_store_n(&g_gq.done, r->fence, __ATOMIC_RELEASE);
    }
}
void gq_drain(void) { gq_take(); }
/* room for a record of payload bytes, its header filled: it doesn't wrap, a filler record takes the ring's end */
static GqRecord* gq_put(u32 kind, u64 gva, u64 size) {
    u64 bytes = (sizeof(GqRecord) + size + GQ_ALIGN - 1) & ~(u64)(GQ_ALIGN - 1);
    if (bytes > GQ_RING / 4) return NULL; /* too big for the ring: the caller writes it at once */
    if (!g_gq.ring && !(g_gq.ring = (u8*)malloc(GQ_RING))) return NULL;
    if (GQ_RING - (g_gq.head - g_gq.tail) < bytes + GQ_RING / 4) gq_take(); /* full: take what is there */
    u64 at = g_gq.head % GQ_RING;
    if (at + bytes > GQ_RING) { /* the end of the ring holds no record: past it, the next one starts at 0 */
        gq_take();
        g_gq.head = g_gq.tail = (g_gq.head + GQ_RING - 1) / GQ_RING * GQ_RING;
        at = 0;
    }
    GqRecord* r = (GqRecord*)(g_gq.ring + at);
    *r = (GqRecord){kind, (u32)bytes, ++g_gq.queued, gva, size};
    return r;
}
static void gq_commit(const GqRecord* r) {
    g_gq.head += r->bytes;
    gq_take(); /* the records run here until a renderer thread takes them */
}
int gq_queues(void) { return gpu_on_thread() && !t_gq_run; }
void gq_write(u64 gva, const void* src, u64 size) {
    GqRecord* r = gq_put(GQ_WRITE, gva, size);
    if (!r) {
        gq_drain();
        gmem_write_now(gva, src, size);
        return;
    }
    memcpy(r + 1, src, (size_t)size);
    gq_mark(gva, size, r->fence);
    gq_commit(r);
}
void gq_release(u64 gva, const void* data, u32 len) {
    GqRecord* r = gq_put(GQ_RELEASE, gva, len);
    if (!r) {
        gq_drain();
        gpu_release_write_now(gva, data, len);
        return;
    }
    memcpy(r + 1, data, len);
    gq_mark(gva, len, r->fence);
    gq_commit(r);
}
void gq_i2m_block(const GpuI2m* t, u64 at, u64 size) {
    GqRecord* r = gq_put(GQ_I2M_BLOCK, t->regs.dest.address, sizeof t->regs + t->copy_size);
    if (!r) {
        gq_drain();
        gpu_i2m_block_now(t);
        return;
    }
    memcpy(r + 1, &t->regs, sizeof t->regs);
    memcpy((u8*)(r + 1) + sizeof t->regs, t->inner_buffer, t->copy_size);
    gq_mark(at, size, r->fence);
    gq_commit(r);
}
void gq_read_wait(u64 gva, u64 size) {
    if (__atomic_load_n(&g_gq.done, __ATOMIC_ACQUIRE) == g_gq.queued || !size || !gq_queues()) return;
    u64 first = gva >> 12, last = (gva + size - 1) >> 12, need = 0;
    if (last - first >= (1u << GQ_SLOT_BITS)) last = first + (1u << GQ_SLOT_BITS) - 1;
    for (u64 p = first; p <= last; p++)
        if (g_gq.written[gq_slot(p)] > need) need = g_gq.written[gq_slot(p)];
    if (need > __atomic_load_n(&g_gq.done, __ATOMIC_ACQUIRE)) gq_take();
}
void* gq_call_begin(GqFn fn, u64 bytes) {
    u64 at = 0;
    memcpy(&at, &fn, sizeof fn);
    g_gq.open = gq_put(GQ_CALL, at, bytes);
    if (g_gq.open) return g_gq.open + 1;
    /* too big for the ring: its bytes on the heap, and the call runs after every record before it */
    g_gq.big = malloc(bytes ? (size_t)bytes : 1);
    if (!g_gq.big) fatal("gpu: out of memory for a queued call of %llu bytes\n", (unsigned long long)bytes);
    g_gq.big_fn = fn;
    g_gq.big_bytes = bytes;
    return g_gq.big;
}
void gq_call_end(void) {
    if (g_gq.open) {
        GqRecord* r = g_gq.open;
        g_gq.open = NULL;
        gq_commit(r);
        return;
    }
    gq_drain();
    t_gq_run = 1;
    g_gq.big_fn(g_gq.big, g_gq.big_bytes);
    t_gq_run = 0;
    free(g_gq.big);
    g_gq.big = NULL;
}
void gq_call(GqFn fn, const void* arg, u64 bytes) {
    void* p = gq_call_begin(fn, bytes);
    if (bytes) memcpy(p, arg, (size_t)bytes);
    gq_call_end();
}
