/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's buffers and vertex arrays (gl_object.h).

   The buffers' bytes are placed in chunks of the platform's memory by a first-fit free list, kept sorted by address
   and merged with its neighbours on a free, every range 256-byte aligned (the uniform buffer offset alignment a
   platform asks at most). A buffer keeps its range while glBufferData's size fits it, and takes a new one, twice
   its size, when it does not: an engine that fills a buffer every frame does not move it.

   Where the platform's GPU side reads the bytes after the draw's call (SnxGlObjectHost.issued), a write of bytes a
   queued draw still reads gives the buffer a new range, as a GL driver orphans a buffer, and the old one is held,
   freed once that work finished; a take that finds no room asks the platform for a chunk before it waits for them */
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_state.h"
#include <stdio.h>
#include <string.h>

enum {
    GL_OUT_OF_MEMORY_ = 0x0505,
    GL_ARRAY_BUFFER_ = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER_ = 0x8893,
    GL_PIXEL_PACK_BUFFER_ = 0x88eb,
    GL_PIXEL_UNPACK_BUFFER_ = 0x88ec,
    GL_UNIFORM_BUFFER_ = 0x8a11,
    GL_TRANSFORM_FEEDBACK_BUFFER_ = 0x8c8e,
    GL_COPY_READ_BUFFER_ = 0x8f36,
    GL_COPY_WRITE_BUFFER_ = 0x8f37,
    GL_DRAW_INDIRECT_BUFFER_ = 0x8f3f,
    GL_SHADER_STORAGE_BUFFER_ = 0x90d2,
    GL_DISPATCH_INDIRECT_BUFFER_ = 0x90ee,
    GL_QUERY_BUFFER_ = 0x9192,
    GL_MAP_READ_BIT_ = 0x1,
    GL_MAP_WRITE_BIT_ = 0x2,
    GL_MAP_INVALIDATE_RANGE_BIT_ = 0x4,
    GL_MAP_INVALIDATE_BUFFER_BIT_ = 0x8,
    GL_MAP_FLUSH_EXPLICIT_BIT_ = 0x10,
    GL_MAP_UNSYNCHRONIZED_BIT_ = 0x20,
    GL_MAP_PERSISTENT_BIT_ = 0x40,
    GL_DYNAMIC_STORAGE_BIT_ = 0x100,
    GL_BUFFER_SIZE_ = 0x8764,
    GL_BUFFER_USAGE_ = 0x8765,
    GL_BUFFER_ACCESS_ = 0x88bb,
    GL_BUFFER_MAPPED_ = 0x88bc,
    GL_BUFFER_ACCESS_FLAGS_ = 0x911f,
    GL_BUFFER_MAP_LENGTH_ = 0x9120,
    GL_BUFFER_MAP_OFFSET_ = 0x9121,
    GL_BUFFER_IMMUTABLE_STORAGE_ = 0x821f,
    GL_BUFFER_STORAGE_FLAGS_ = 0x8220,
    GL_STATIC_DRAW_ = 0x88e4,
    GL_BYTE_ = 0x1400,
    GL_UNSIGNED_BYTE_ = 0x1401,
    GL_SHORT_ = 0x1402,
    GL_UNSIGNED_SHORT_ = 0x1403,
    GL_HALF_FLOAT_ = 0x140b,
    GL_HALF_FLOAT_OES_ = 0x8d61,
    GL_INT_2_10_10_10_REV_ = 0x8d9f,
    GL_UNSIGNED_INT_2_10_10_10_REV_ = 0x8368,
    BASE_INT = 1, /* GlslcBase's */
    BASE_FLOAT = 3,
    ALIGN = 256,
    CHUNK = 16u << 20,
    HELD = 4096, /* ranges given back that work handed over may still read */
};

typedef struct {
    uint64_t at, size;
} SnxGlRange;
typedef struct {
    uint64_t at, size, serial; /* free once the GPU side finished serial */
} SnxGlHeld;

/* what a save state holds */
typedef struct {
    SnxGlBuffer buffers[SNX_GL_BUFFERS];
    SnxGlVertexArray arrays[SNX_GL_VERTEX_ARRAYS];
    SnxGlObjectContext contexts[SNX_GL_CONTEXTS];
    SnxGlRange chunks[SNX_GL_CHUNKS];
    SnxGlRange free[SNX_GL_FREE_RANGES]; /* by address, none touching another of its chunk */
    uint32_t chunk_count, free_count;
    /* each chunk's bytes from here to its end were never taken: still the zeros the platform gave */
    uint64_t untouched[SNX_GL_CHUNKS];
} SnxGlObjects;

static struct {
    SnxGlObjects s;
    SnxGlObjects checked; /* a save state's copy being checked: a check never touches the running state */
    SnxGlObjectHost host;
    /* each buffer's last draw handed over that reads its bytes, and the hull of the bytes written into its range since
       it took it: a write outside them, which no draw reads, keeps the range. Not part of a save state, nor the held
       ranges: a load finds the work done */
    uint64_t read[SNX_GL_BUFFERS];
    uint64_t valid[SNX_GL_BUFFERS][2];
    SnxGlHeld held[HELD];
    uint32_t held_count;
} g_gl_object;

#define S g_gl_object.s
#define H g_gl_object.host

void snx_gl_object_host(const SnxGlObjectHost* host) { H = host ? *host : (SnxGlObjectHost){0}; }

static void error(uint32_t ctx, uint32_t e) {
    if (ctx) snx_gl_error(ctx, e);
}
static void read_back(uint32_t name) {
    if (H.read_back) H.read_back(name);
}
static void written(uint32_t name, uint64_t lo, uint64_t hi) {
    if (H.written && hi > lo) H.written(name, lo, hi);
}
static void gone(uint32_t name) {
    if (H.gone) H.gone(name);
}
static uint8_t* bytes(uint64_t at) { return at && H.bytes ? H.bytes(at) : NULL; }
/* the serial of the last work the GPU side finished: all of it for a platform that reads at the call (one without the
   three calls of a later GPU side) */
static uint64_t finished(void) { return H.issued && H.finished && H.wait ? H.finished() : ~(uint64_t)0; }
/* serial when that work is not finished yet, else 0 */
static uint64_t unfinished(uint64_t serial) { return serial > finished() ? serial : 0; }
/* the GPU side's work on buffer's bytes still to finish, its serial: 0 for none */
static uint64_t busy(uint32_t name) { return unfinished(g_gl_object.read[name - 1]); }
static void settle(uint64_t serial) {
    if (serial && H.wait) H.wait(serial);
}

/* ---- the platform's memory ---- */

/* size bytes of a free range, at a multiple of align (a power of 2, ALIGN or more), taken: their address, 0 when no
   chunk has room and, with grow, the platform gives none. The bytes before them in the range stay free */
static uint64_t memory_find(uint64_t size, uint64_t align, int grow) {
    size = (size + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < S.free_count; i++) {
            SnxGlRange* r = &S.free[i];
            uint64_t at = (r->at + align - 1) & ~(align - 1), pad = at - r->at;
            if (r->size < pad || r->size - pad < size) continue;
            if (pad) { /* the range keeps its bytes before at, and those after the taken ones follow it */
                uint64_t rest = r->size - pad - size;
                if (rest && S.free_count == SNX_GL_FREE_RANGES) continue;
                r->size = pad;
                if (rest) {
                    memmove(&S.free[i + 2], &S.free[i + 1], (S.free_count - i - 1) * sizeof S.free[0]);
                    S.free[i + 1] = (SnxGlRange){at + size, rest};
                    S.free_count++;
                }
                return at;
            }
            r->at += size, r->size -= size;
            if (!r->size) {
                memmove(r, r + 1, (S.free_count - i - 1) * sizeof *r);
                S.free_count--;
            }
            return at;
        }
        if (pass || !grow || !H.chunk || S.chunk_count == SNX_GL_CHUNKS || S.free_count == SNX_GL_FREE_RANGES) return 0;
        {
            /* room for the alignment asked from whatever start the platform's chunk has, which may be off ALIGN's
               multiple, and its end where the platform's ends */
            uint64_t need = size + align - 1, got = 0, raw = H.chunk(need > CHUNK ? need : CHUNK, &got), at, end;
            uint32_t i = 0;
            if (!raw || got < need) return 0;
            at = (raw + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
            end = (raw + got) & ~(uint64_t)(ALIGN - 1);
            S.untouched[S.chunk_count] = at;
            S.chunks[S.chunk_count++] = (SnxGlRange){at, end - at};
            while (i < S.free_count && S.free[i].at < at) i++;
            memmove(&S.free[i + 1], &S.free[i], (S.free_count - i) * sizeof S.free[0]);
            S.free[i] = (SnxGlRange){at, end - at};
            S.free_count++;
        }
    }
    return 0;
}
/* the chunk that holds address at: its index, chunk_count for none */
static uint32_t chunk_of(uint64_t at) {
    uint32_t i = 0;
    while (i < S.chunk_count && at - S.chunks[i].at >= S.chunks[i].size) i++;
    return i;
}
/* size bytes at at back in the free ranges, merged with those beside them in its chunk: 0, or -1 when the list is full
   (they stay taken) */
static int range_free(uint64_t at, uint64_t size) {
    uint32_t i = 0;
    int before, after;
    size = (size + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    if (!at || !size) return 0;
    while (i < S.free_count && S.free[i].at < at) i++;
    /* a neighbour in another chunk stays apart: two chunks the platform gave side by side are two of its allocations,
       which a range across both would read past (macOS's allocator hands them out so) */
    before = i > 0 && S.free[i - 1].at + S.free[i - 1].size == at && chunk_of(S.free[i - 1].at) == chunk_of(at);
    after = i < S.free_count && at + size == S.free[i].at && chunk_of(S.free[i].at) == chunk_of(at);
    if (before && after) {
        S.free[i - 1].size += size + S.free[i].size;
        memmove(&S.free[i], &S.free[i + 1], (S.free_count - i - 1) * sizeof S.free[0]);
        S.free_count--;
    } else if (before) {
        S.free[i - 1].size += size;
    } else if (after) {
        S.free[i].at = at, S.free[i].size += size;
    } else {
        if (S.free_count == SNX_GL_FREE_RANGES) return -1;
        memmove(&S.free[i + 1], &S.free[i], (S.free_count - i) * sizeof S.free[0]);
        S.free[i] = (SnxGlRange){at, size};
        S.free_count++;
    }
    return 0;
}

/* the held ranges whose work finished, free */
static void held_free(void) {
    uint64_t done = finished();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g_gl_object.held_count; i++) {
        const SnxGlHeld h = g_gl_object.held[i];
        if (h.serial <= done)
            range_free(h.at, h.size);
        else
            g_gl_object.held[kept++] = h;
    }
    g_gl_object.held_count = kept;
}
/* every held range free, its work waited for */
static void held_wait(void) {
    uint64_t last = 0;
    for (uint32_t i = 0; i < g_gl_object.held_count; i++)
        if (g_gl_object.held[i].serial > last) last = g_gl_object.held[i].serial;
    settle(unfinished(last));
    held_free();
}
/* memory_find's bytes, and in *dirty how many of them from the first may hold what an earlier taker wrote: the rest
   were never taken, and are the zeros the platform gave. Zeroing those alone spares a game's loading a write of every
   page of its textures (a page fault each, the first time) */
static uint64_t memory_take(uint64_t size, uint64_t align, uint64_t* dirty) {
    uint64_t at, end;
    held_free();
    at = memory_find(size, align, 0);
    if (!at) at = memory_find(size, align, 1);
    if (!at && g_gl_object.held_count) { /* the platform has no more: the room work still reads, once it is done */
        held_wait();
        at = memory_find(size, align, 0);
    }
    end = at + ((size + ALIGN - 1) & ~(uint64_t)(ALIGN - 1));
    *dirty = 0;
    if (!at) return 0;
    for (uint32_t i = 0; i < S.chunk_count; i++)
        if (at - S.chunks[i].at < S.chunks[i].size) {
            uint64_t clean = S.untouched[i];
            *dirty = clean <= at ? 0 : clean >= end ? end - at : clean - at;
            if (end > clean) S.untouched[i] = end;
            return at;
        }
    *dirty = end - at; /* in no chunk: none of it known */
    return at;
}
/* size bytes at at given back: free, or held until the GPU side finished serial (0: nothing reads them). A full
   list of held ones waits for them first */
static void memory_give(uint64_t at, uint64_t size, uint64_t serial) {
    if (!at || !size) return;
    if (!unfinished(serial)) {
        range_free(at, size);
        return;
    }
    if (g_gl_object.held_count == HELD) held_wait();
    g_gl_object.held[g_gl_object.held_count++] = (SnxGlHeld){at, size, serial};
}

uint8_t* snx_gl_client_bytes(uint64_t at) { return bytes(at); }
uint64_t snx_gl_memory_take(uint64_t size, uint64_t align) {
    uint64_t dirty;
    return align & (align - 1) ? 0 : memory_take(size, align < ALIGN ? ALIGN : align, &dirty);
}
uint64_t snx_gl_memory_take_zeroed(uint64_t size, uint64_t align) {
    uint64_t dirty, at = align & (align - 1) ? 0 : memory_take(size, align < ALIGN ? ALIGN : align, &dirty);
    uint8_t* to = bytes(at);
    if (to && dirty) memset(to, 0, dirty < size ? dirty : size);
    return at;
}
/* a texture's or renderbuffer's range: any draw handed over may read it */
void snx_gl_memory_give(uint64_t at, uint64_t size) { memory_give(at, size, unfinished(H.issued ? H.issued() : 0)); }

/* ---- the objects ---- */

SnxGlObjectContext* snx_gl_objects(uint32_t ctx) { return &S.contexts[ctx - 1]; }
SnxGlBuffer* snx_gl_buffer(uint32_t name) {
    return name && name <= SNX_GL_BUFFERS && S.buffers[name - 1].used ? &S.buffers[name - 1] : NULL;
}
uint8_t* snx_gl_buffer_bytes(uint32_t name) {
    const SnxGlBuffer* b = snx_gl_buffer(name);
    if (!b || !b->data) return NULL;
    read_back(name);
    return bytes(b->data);
}
void snx_gl_buffer_read_by(uint32_t name, uint64_t serial) {
    if (snx_gl_buffer(name) && serial > g_gl_object.read[name - 1]) g_gl_object.read[name - 1] = serial;
}
/* buffer's bytes in a range no work reads yet, none of them written */
static void buffer_fresh(uint32_t name) {
    g_gl_object.read[name - 1] = 0;
    g_gl_object.valid[name - 1][0] = g_gl_object.valid[name - 1][1] = 0;
}
/* bytes [lo, hi) of buffer written: into the hull of the written ones */
static void valid_add(uint32_t name, uint64_t lo, uint64_t hi) {
    uint64_t* v = g_gl_object.valid[name - 1];
    if (hi <= lo) return;
    if (v[1] <= v[0]) {
        v[0] = lo, v[1] = hi;
        return;
    }
    if (lo < v[0]) v[0] = lo;
    if (hi > v[1]) v[1] = hi;
}
/* whether a draw handed over may still read bytes [lo, hi) of buffer: one reads its range, and they were written */
static int in_use(uint32_t name, uint64_t lo, uint64_t hi) {
    const uint64_t* v = g_gl_object.valid[name - 1];
    return busy(name) && lo < v[1] && v[0] < hi;
}
SnxGlVertexArray* snx_gl_vertex_array(uint32_t ctx) {
    SnxGlObjectContext* o = snx_gl_objects(ctx);
    uint32_t a = o->vertex_array;
    return a && a <= SNX_GL_VERTEX_ARRAYS && S.arrays[a - 1].used ? &S.arrays[a - 1] : &o->vao;
}
static void vao_init(SnxGlVertexArray* a) {
    *a = (SnxGlVertexArray){.used = 1};
    for (int i = 0; i < SNX_GL_VERTEX_ATTRIBS; i++)
        a->attribs[i] = (SnxGlAttrib){.size = 4, .type = 0x1406, .binding = (uint8_t)i}; /* GL_FLOAT */
}
void snx_gl_object_context_reset(uint32_t ctx) {
    SnxGlObjectContext* o = snx_gl_objects(ctx);
    const float one = 1;
    *o = (SnxGlObjectContext){0};
    vao_init(&o->vao);
    for (int i = 0; i < SNX_GL_VERTEX_ATTRIBS; i++) {
        memcpy(&o->generic[i].bits[3], &one, 4);
        o->generic[i].base = BASE_FLOAT;
    }
}

/* ---- buffers ---- */

static uint32_t* buffer_binding(uint32_t ctx, uint32_t target) {
    SnxGlObjectContext* o = snx_gl_objects(ctx);
    switch (target) {
        case GL_ARRAY_BUFFER_: return &o->array;
        case GL_ELEMENT_ARRAY_BUFFER_: return &snx_gl_vertex_array(ctx)->elements;
        case GL_PIXEL_PACK_BUFFER_: return &o->pixel_pack;
        case GL_PIXEL_UNPACK_BUFFER_: return &o->pixel_unpack;
        case GL_COPY_READ_BUFFER_: return &o->copy_read;
        case GL_COPY_WRITE_BUFFER_: return &o->copy_write;
        case GL_UNIFORM_BUFFER_: return &o->uniform;
        case GL_TRANSFORM_FEEDBACK_BUFFER_: return &o->feedback;
        case GL_SHADER_STORAGE_BUFFER_: return &o->storage;
        case GL_DRAW_INDIRECT_BUFFER_: return &o->draw_indirect;
        case GL_DISPATCH_INDIRECT_BUFFER_: return &o->dispatch_indirect;
        case GL_QUERY_BUFFER_: return &o->query;
        default: return NULL;
    }
}
uint32_t snx_gl_bound_buffer(uint32_t ctx, uint32_t target) {
    const uint32_t* at = buffer_binding(ctx, target);
    if (!at) return error(ctx, SNX_GL_INVALID_ENUM), 0;
    if (!snx_gl_buffer(*at)) return error(ctx, SNX_GL_INVALID_OPERATION), 0;
    return *at;
}
/* the buffer of name, made when a bind names it first (GLES lets a bind make it) */
static SnxGlBuffer* buffer_made(uint32_t name) {
    if (!name || name > SNX_GL_BUFFERS) return NULL;
    if (!S.buffers[name - 1].used) S.buffers[name - 1] = (SnxGlBuffer){.used = 1, .usage = GL_STATIC_DRAW_};
    return &S.buffers[name - 1];
}

void snx_gl_gen_buffers(uint32_t ctx, int32_t n, uint32_t* out) {
    uint32_t name = 1;
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t v = 0;
        while (name <= SNX_GL_BUFFERS && S.buffers[name - 1].used) name++;
        if (name <= SNX_GL_BUFFERS) {
            buffer_made(name);
            v = name++;
        } else {
            error(ctx, GL_OUT_OF_MEMORY_);
        }
        memcpy(out + i, &v, 4);
    }
}
static void buffer_free(uint32_t name) {
    SnxGlBuffer* b = &S.buffers[name - 1];
    memory_give(b->data, b->capacity, busy(name));
    buffer_fresh(name);
    gone(name);
    *b = (SnxGlBuffer){0};
}
void snx_gl_delete_buffers(uint32_t ctx, int32_t n, const uint32_t* names) {
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t name;
        memcpy(&name, names + i, 4);
        if (!snx_gl_buffer(name)) continue;
        buffer_free(name);
        if (!ctx) continue;
        {
            SnxGlObjectContext* o = snx_gl_objects(ctx);
            SnxGlVertexArray* a = snx_gl_vertex_array(ctx);
            uint32_t* bound[] = {&o->array,         &o->copy_read,         &o->copy_write, &o->pixel_pack,
                                 &o->pixel_unpack,  &o->uniform,           &o->feedback,   &o->storage,
                                 &o->draw_indirect, &o->dispatch_indirect, &o->query,      &a->elements};
            for (size_t k = 0; k < sizeof bound / sizeof *bound; k++)
                if (*bound[k] == name) *bound[k] = 0;
            for (int k = 0; k < SNX_GL_UNIFORM_BINDINGS; k++)
                if (o->uniforms[k].buffer == name) o->uniforms[k] = (SnxGlIndexedBuffer){0};
            for (int k = 0; k < SNX_GL_STORAGE_BINDINGS; k++)
                if (o->storages[k].buffer == name) o->storages[k] = (SnxGlIndexedBuffer){0};
            for (int k = 0; k < SNX_GL_VERTEX_ATTRIBS; k++)
                if (a->bindings[k].buffer == name) a->bindings[k].buffer = 0;
        }
    }
}
int snx_gl_is_buffer(uint32_t name) { return snx_gl_buffer(name) != NULL; }
void snx_gl_bind_buffer(uint32_t ctx, uint32_t target, uint32_t name) {
    uint32_t* at = buffer_binding(ctx, target);
    if (!at) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (name && !buffer_made(name)) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    *at = name;
}
void snx_gl_bind_buffer_range(uint32_t ctx, uint32_t target, uint32_t index, uint32_t name, int64_t offset,
                              int64_t size) {
    SnxGlObjectContext* o = snx_gl_objects(ctx);
    if (target != GL_UNIFORM_BUFFER_ && target != GL_TRANSFORM_FEEDBACK_BUFFER_ &&
        target != GL_SHADER_STORAGE_BUFFER_) {
        error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    if (name && !buffer_made(name)) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (target == GL_SHADER_STORAGE_BUFFER_) {
        if (index >= SNX_GL_STORAGE_BINDINGS) {
            error(ctx, SNX_GL_INVALID_VALUE);
            return;
        }
        o->storage = name;
        o->storages[index] = (SnxGlIndexedBuffer){name, 0, offset, size};
    } else if (target == GL_TRANSFORM_FEEDBACK_BUFFER_) {
        o->feedback = name;
    } else {
        if (index >= SNX_GL_UNIFORM_BINDINGS) {
            error(ctx, SNX_GL_INVALID_VALUE);
            return;
        }
        o->uniform = name;
        o->uniforms[index] = (SnxGlIndexedBuffer){name, 0, offset, size};
    }
}

/* a new range of the buffer's capacity while draws handed over still read the old one, which is held until they are
   done; with keep the written bytes copied over, else none written. 0, or -1 when the platform has no room (the old
   range kept) */
static int buffer_renew(uint32_t name, int keep) {
    SnxGlBuffer* b = &S.buffers[name - 1];
    const uint64_t lo = g_gl_object.valid[name - 1][0], hi = g_gl_object.valid[name - 1][1];
    uint64_t dirty, at = memory_take(b->capacity, ALIGN, &dirty);
    uint8_t *to = bytes(at), *from = bytes(b->data);
    if (!at) return -1;
    if (to && dirty) memset(to, 0, dirty < b->size ? dirty : b->size);
    if (keep && to && from && hi > lo) memcpy(to + lo, from + lo, hi - lo);
    memory_give(b->data, b->capacity, busy(name));
    gone(name); /* the platform's copy was of the old range */
    b->data = at;
    g_gl_object.read[name - 1] = 0;
    if (!keep) g_gl_object.valid[name - 1][0] = g_gl_object.valid[name - 1][1] = 0;
    written(name, 0, b->size);
    return 0;
}
/* the buffer's bytes placed for size of them: kept where they fit, else a new range (the old one given back): 0, or
   -1 (GL_OUT_OF_MEMORY recorded) without room */
static int buffer_place(uint32_t ctx, uint32_t name, uint64_t size, uint64_t* dirty) {
    SnxGlBuffer* b = &S.buffers[name - 1];
    uint64_t want, at;
    if (b->data && size <= b->capacity) {
        b->size = size, *dirty = size;
        /* orphaned while draws read it, as GL's glBufferData asks: its zeros made, or with no room its draws waited */
        if (busy(name)) {
            if (!buffer_renew(name, 0))
                *dirty = 0;
            else
                settle(busy(name));
        }
        g_gl_object.valid[name - 1][0] = g_gl_object.valid[name - 1][1] = 0;
        return 0;
    }
    want = size < ALIGN ? ALIGN : size * 2;
    at = memory_take(want, ALIGN, dirty);
    if (!at && want > size) at = memory_take(want = size, ALIGN, dirty);
    if (!at) {
        error(ctx, GL_OUT_OF_MEMORY_);
        return -1;
    }
    if (b->data) {
        memory_give(b->data, b->capacity, busy(name));
        gone(name); /* the platform's copy was of the old range */
    }
    buffer_fresh(name);
    b->data = at, b->capacity = (want + ALIGN - 1) & ~(uint64_t)(ALIGN - 1), b->size = size;
    return 0;
}
/* size bytes of data into buffer's bytes at offset. Bytes a draw handed over still reads go into a new range, the
   written ones copied over (the old range still the draws'); a mapped buffer keeps its range, as does one the platform
   has no room for, and the write waits for those draws */
static void buffer_write(uint32_t name, uint64_t offset, const void* data, uint64_t size) {
    const SnxGlBuffer* b = &S.buffers[name - 1];
    uint8_t* to;
    if (in_use(name, offset, offset + size) && (b->mapped || buffer_renew(name, 1))) settle(busy(name));
    to = bytes(b->data);
    if (!to || !size) return;
    memmove(to + offset, data, (size_t)size); /* glCopyBufferSubData within one buffer */
    written(name, offset, offset + size);
    valid_add(name, offset, offset + size);
}
/* glBufferData's and glBufferStorage's bytes: data's, or zeros (the first dirty of them may hold another's) */
static void buffer_fill(uint32_t name, uint64_t size, const void* data, uint64_t dirty) {
    uint8_t* to = bytes(S.buffers[name - 1].data);
    if (!to) return;
    if (data)
        memcpy(to, data, size), valid_add(name, 0, size);
    else if (dirty)
        memset(to, 0, dirty < size ? dirty : size);
    written(name, 0, size);
}
void snx_gl_buffer_data(uint32_t ctx, uint32_t name, int64_t size, const void* data, uint32_t usage) {
    SnxGlBuffer* b = snx_gl_buffer(name);
    uint64_t dirty;
    if (!b) return;
    if (size < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (b->immutable || b->mapped) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (buffer_place(ctx, name, (uint64_t)size, &dirty)) return;
    S.buffers[name - 1].usage = usage;
    buffer_fill(name, (uint64_t)size, data, dirty);
}
void snx_gl_buffer_storage(uint32_t ctx, uint32_t name, int64_t size, const void* data, uint32_t flags) {
    SnxGlBuffer* b = snx_gl_buffer(name);
    uint64_t dirty;
    if (!b) return;
    if (size <= 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (b->immutable) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (b->mapped) b->mapped = 0;
    if (buffer_place(ctx, name, (uint64_t)size, &dirty)) return;
    S.buffers[name - 1].immutable = 1;
    S.buffers[name - 1].flags = flags;
    buffer_fill(name, (uint64_t)size, data, dirty);
}
/* a range [offset, offset + size) of a buffer's bytes: 1, or 0 with GL_INVALID_VALUE recorded */
static int inside(uint32_t ctx, const SnxGlBuffer* b, int64_t offset, int64_t size) {
    if (offset < 0 || size < 0 || (uint64_t)offset > b->size || (uint64_t)size > b->size - (uint64_t)offset)
        return error(ctx, SNX_GL_INVALID_VALUE), 0;
    return 1;
}
void snx_gl_buffer_sub_data(uint32_t ctx, uint32_t name, int64_t offset, int64_t size, const void* data) {
    const SnxGlBuffer* b = snx_gl_buffer(name);
    if (!b || !inside(ctx, b, offset, size)) return;
    if (b->immutable && !(b->flags & GL_DYNAMIC_STORAGE_BIT_)) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (b->mapped && !(b->map_access & GL_MAP_PERSISTENT_BIT_)) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (!size || !data || !bytes(b->data)) return;
    read_back(name);
    buffer_write(name, (uint64_t)offset, data, (uint64_t)size);
}
void snx_gl_get_buffer_sub_data(uint32_t ctx, uint32_t name, int64_t offset, int64_t size, void* out) {
    SnxGlBuffer* b = snx_gl_buffer(name);
    const uint8_t* from;
    if (!b || !inside(ctx, b, offset, size)) return;
    from = snx_gl_buffer_bytes(name);
    if (from && size && out) memcpy(out, from + offset, (size_t)size);
}
uint64_t snx_gl_map_buffer_range(uint32_t ctx, uint32_t name, int64_t offset, int64_t length, uint32_t access) {
    SnxGlBuffer* b = snx_gl_buffer(name);
    if (!b) return 0;
    if (!inside(ctx, b, offset, length) || !length || !(access & (GL_MAP_READ_BIT_ | GL_MAP_WRITE_BIT_)))
        return error(ctx, SNX_GL_INVALID_VALUE), 0;
    if (b->mapped || ((access & GL_MAP_PERSISTENT_BIT_) && !(b->flags & GL_MAP_PERSISTENT_BIT_)))
        return error(ctx, SNX_GL_INVALID_OPERATION), 0;
    /* GL's map waits for the draws that read the bytes, unless it was told it needs not (unsynchronized) or the bytes
       may go (invalidated): a new range then, as a GL driver gives */
    if (!(access & GL_MAP_UNSYNCHRONIZED_BIT_) && in_use(name, (uint64_t)offset, (uint64_t)(offset + length)) &&
        (access & GL_MAP_INVALIDATE_BUFFER_BIT_  ? buffer_renew(name, 0)
         : access & GL_MAP_INVALIDATE_RANGE_BIT_ ? buffer_renew(name, 1)
                                                 : -1))
        settle(busy(name));
    read_back(name);
    if (access & GL_MAP_PERSISTENT_BIT_) valid_add(name, 0, b->size); /* written through it from now on, unseen */
    b->mapped = 1;
    b->map_offset = (uint64_t)offset, b->map_length = (uint64_t)length, b->map_access = access;
    return b->data + (uint64_t)offset;
}
int snx_gl_unmap_buffer(uint32_t ctx, uint32_t name) {
    SnxGlBuffer* b = snx_gl_buffer(name);
    if (!b) return 0;
    if (!b->mapped) return error(ctx, SNX_GL_INVALID_OPERATION), 0;
    if ((b->map_access & GL_MAP_WRITE_BIT_) && !(b->map_access & GL_MAP_FLUSH_EXPLICIT_BIT_)) {
        written(name, b->map_offset, b->map_offset + b->map_length);
        valid_add(name, b->map_offset, b->map_offset + b->map_length);
    }
    b->mapped = 0;
    return 1;
}
void snx_gl_flush_mapped_range(uint32_t ctx, uint32_t name, int64_t offset, int64_t length) {
    const SnxGlBuffer* b = snx_gl_buffer(name);
    if (!b) return;
    if (!b->mapped || !(b->map_access & GL_MAP_FLUSH_EXPLICIT_BIT_)) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    if (offset < 0 || length < 0 || (uint64_t)offset + (uint64_t)length > b->map_length) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    written(name, b->map_offset + (uint64_t)offset, b->map_offset + (uint64_t)offset + (uint64_t)length);
    valid_add(name, b->map_offset + (uint64_t)offset, b->map_offset + (uint64_t)offset + (uint64_t)length);
}
uint64_t snx_gl_buffer_pointer(uint32_t ctx, uint32_t name) {
    const SnxGlBuffer* b = snx_gl_buffer(name);
    (void)ctx;
    return b && b->mapped ? b->data + b->map_offset : 0;
}
void snx_gl_copy_buffer_sub_data(uint32_t ctx, uint32_t read, uint32_t write, int64_t read_offset, int64_t write_offset,
                                 int64_t size) {
    const SnxGlBuffer *r = snx_gl_buffer(read), *w = snx_gl_buffer(write);
    const uint8_t* from;
    if (!r || !w) return;
    if (!inside(ctx, r, read_offset, size) || !inside(ctx, w, write_offset, size)) return;
    from = snx_gl_buffer_bytes(read);
    if (!from || !bytes(w->data) || !size) return;
    read_back(write);
    buffer_write(write, (uint64_t)write_offset, from + read_offset, (uint64_t)size);
}
int snx_gl_buffer_parameter(uint32_t ctx, uint32_t name, uint32_t pname, int64_t* v) {
    const SnxGlBuffer* b = snx_gl_buffer(name);
    if (!b) return 0;
    switch (pname) {
        case GL_BUFFER_SIZE_: *v = (int64_t)b->size; return 1;
        case GL_BUFFER_USAGE_: *v = b->usage; return 1;
        case GL_BUFFER_MAPPED_: *v = b->mapped; return 1;
        case GL_BUFFER_ACCESS_: /* glMapBuffer's GL_READ_ONLY, GL_WRITE_ONLY or GL_READ_WRITE */
            *v = (b->map_access & 3) == 1 ? 0x88b8 : (b->map_access & 3) == 2 ? 0x88b9 : 0x88ba;
            return 1;
        case GL_BUFFER_ACCESS_FLAGS_: *v = b->mapped ? b->map_access : 0; return 1;
        case GL_BUFFER_MAP_LENGTH_: *v = b->mapped ? (int64_t)b->map_length : 0; return 1;
        case GL_BUFFER_MAP_OFFSET_: *v = b->mapped ? (int64_t)b->map_offset : 0; return 1;
        case GL_BUFFER_IMMUTABLE_STORAGE_: *v = b->immutable; return 1;
        case GL_BUFFER_STORAGE_FLAGS_: *v = b->flags; return 1;
        default: return error(ctx, SNX_GL_INVALID_ENUM), 0;
    }
}

/* ---- vertex arrays ---- */

void snx_gl_gen_vertex_arrays(uint32_t ctx, int32_t n, uint32_t* out) {
    uint32_t name = 1;
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t v = 0;
        while (name <= SNX_GL_VERTEX_ARRAYS && S.arrays[name - 1].used) name++;
        if (name <= SNX_GL_VERTEX_ARRAYS) {
            vao_init(&S.arrays[name - 1]);
            v = name++;
        } else {
            error(ctx, GL_OUT_OF_MEMORY_);
        }
        memcpy(out + i, &v, 4);
    }
}
void snx_gl_delete_vertex_arrays(uint32_t ctx, int32_t n, const uint32_t* names) {
    if (n < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    for (int32_t i = 0; i < n; i++) {
        uint32_t name;
        memcpy(&name, names + i, 4);
        if (!snx_gl_is_vertex_array(name)) continue;
        S.arrays[name - 1] = (SnxGlVertexArray){0};
        if (ctx && snx_gl_objects(ctx)->vertex_array == name) snx_gl_objects(ctx)->vertex_array = 0;
    }
}
int snx_gl_is_vertex_array(uint32_t name) { return name && name <= SNX_GL_VERTEX_ARRAYS && S.arrays[name - 1].used; }
void snx_gl_bind_vertex_array(uint32_t ctx, uint32_t name) {
    if (name && !snx_gl_is_vertex_array(name)) {
        error(ctx, SNX_GL_INVALID_OPERATION);
        return;
    }
    snx_gl_objects(ctx)->vertex_array = name;
}
static SnxGlAttrib* attrib(uint32_t ctx, uint32_t index) {
    if (index >= SNX_GL_VERTEX_ATTRIBS) return error(ctx, SNX_GL_INVALID_VALUE), NULL;
    return &snx_gl_vertex_array(ctx)->attribs[index];
}
int snx_gl_object_get(uint32_t ctx, uint32_t pname, double v[4]) {
    const SnxGlObjectContext* o = snx_gl_objects(ctx);
    switch (pname) {
        case 0x8894: v[0] = o->array; return 1;                           /* GL_ARRAY_BUFFER_BINDING */
        case 0x8895: v[0] = snx_gl_vertex_array(ctx)->elements; return 1; /* GL_ELEMENT_ARRAY_BUFFER_BINDING */
        case 0x85b5: v[0] = o->vertex_array; return 1;                    /* GL_VERTEX_ARRAY_BINDING */
        case 0x8f36: v[0] = o->copy_read; return 1;                       /* GL_COPY_READ_BUFFER_BINDING */
        case 0x8f37: v[0] = o->copy_write; return 1;                      /* GL_COPY_WRITE_BUFFER_BINDING */
        case 0x88ed: v[0] = o->pixel_pack; return 1;                      /* GL_PIXEL_PACK_BUFFER_BINDING */
        case 0x88ef: v[0] = o->pixel_unpack; return 1;                    /* GL_PIXEL_UNPACK_BUFFER_BINDING */
        case 0x8a28: v[0] = o->uniform; return 1;                         /* GL_UNIFORM_BUFFER_BINDING */
        case 0x8c8f: v[0] = o->feedback; return 1;                        /* GL_TRANSFORM_FEEDBACK_BUFFER_BINDING */
        case 0x90d3: v[0] = o->storage; return 1;                         /* GL_SHADER_STORAGE_BUFFER_BINDING */
        case 0x8f43: v[0] = o->draw_indirect; return 1;                   /* GL_DRAW_INDIRECT_BUFFER_BINDING */
        case 0x90ef: v[0] = o->dispatch_indirect; return 1;               /* GL_DISPATCH_INDIRECT_BUFFER_BINDING */
        case 0x9193: v[0] = o->query; return 1;                           /* GL_QUERY_BUFFER_BINDING */
        default: return 0;
    }
}
uint32_t snx_gl_vertex_bytes(uint32_t type, int32_t size) {
    switch (type) {
        case GL_BYTE_:
        case GL_UNSIGNED_BYTE_: return (uint32_t)size;
        case GL_SHORT_:
        case GL_UNSIGNED_SHORT_:
        case GL_HALF_FLOAT_:
        case GL_HALF_FLOAT_OES_: return 2u * (uint32_t)size;
        case GL_INT_2_10_10_10_REV_:
        case GL_UNSIGNED_INT_2_10_10_10_REV_: return 4;
        default: return 4u * (uint32_t)size;
    }
}
void snx_gl_vertex_attrib_pointer(uint32_t ctx, uint32_t index, int32_t size, uint32_t type, int normalized,
                                  int integer, int32_t stride, uint64_t pointer) {
    SnxGlAttrib* a = attrib(ctx, index);
    SnxGlVertexBinding* b;
    if (!a) return;
    if (size < 1 || size > 4 || stride < 0) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    a->size = size, a->type = type, a->normalized = normalized != 0, a->integer = integer != 0;
    a->stride = stride, a->relative = 0, a->binding = (uint8_t)index;
    b = &snx_gl_vertex_array(ctx)->bindings[index];
    b->offset = pointer;
    b->buffer = snx_gl_objects(ctx)->array;
    b->stride = stride ? (uint32_t)stride : snx_gl_vertex_bytes(type, size);
}
void snx_gl_vertex_attrib_format(uint32_t ctx, uint32_t index, int32_t size, uint32_t type, int normalized, int integer,
                                 uint32_t relative) {
    SnxGlAttrib* a = attrib(ctx, index);
    if (!a) return;
    if (size < 1 || size > 4 || relative > 2047) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    a->size = size, a->type = type, a->normalized = normalized != 0, a->integer = integer != 0;
    a->relative = relative;
}
void snx_gl_vertex_attrib_binding(uint32_t ctx, uint32_t index, uint32_t binding) {
    SnxGlAttrib* a = attrib(ctx, index);
    if (a && binding < SNX_GL_VERTEX_ATTRIBS)
        a->binding = (uint8_t)binding;
    else if (a)
        error(ctx, SNX_GL_INVALID_VALUE);
}
void snx_gl_bind_vertex_buffer(uint32_t ctx, uint32_t binding, uint32_t buffer, int64_t offset, int32_t stride) {
    SnxGlVertexBinding* b;
    if (binding >= SNX_GL_VERTEX_ATTRIBS || offset < 0 || stride < 0 || stride > 2048) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    if (buffer && !buffer_made(buffer)) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    b = &snx_gl_vertex_array(ctx)->bindings[binding];
    b->buffer = buffer, b->offset = (uint64_t)offset, b->stride = (uint32_t)stride;
}
void snx_gl_vertex_binding_divisor(uint32_t ctx, uint32_t binding, uint32_t divisor) {
    if (binding >= SNX_GL_VERTEX_ATTRIBS) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    snx_gl_vertex_array(ctx)->bindings[binding].divisor = divisor;
}
void snx_gl_enable_vertex_attrib(uint32_t ctx, uint32_t index, int on) {
    SnxGlAttrib* a = attrib(ctx, index);
    if (a) a->enabled = on != 0;
}
void snx_gl_vertex_attrib_divisor(uint32_t ctx, uint32_t index, uint32_t divisor) {
    SnxGlAttrib* a = attrib(ctx, index);
    if (!a) return;
    a->binding = (uint8_t)index;
    snx_gl_vertex_array(ctx)->bindings[index].divisor = divisor;
}
void snx_gl_vertex_attrib(uint32_t ctx, uint32_t index, const void* v, int n, uint32_t base) {
    SnxGlGeneric* g;
    const float one = 1;
    if (index >= SNX_GL_VERTEX_ATTRIBS) {
        error(ctx, SNX_GL_INVALID_VALUE);
        return;
    }
    g = &snx_gl_objects(ctx)->generic[index];
    *g = (SnxGlGeneric){.base = base};
    if (base == BASE_FLOAT)
        memcpy(&g->bits[3], &one, 4);
    else
        g->bits[3] = 1;
    memcpy(g->bits, v, (size_t)n * 4);
}
int snx_gl_get_vertex_attrib(uint32_t ctx, uint32_t index, uint32_t pname, double v[4]) {
    const SnxGlAttrib* a = attrib(ctx, index);
    const SnxGlVertexArray* array = snx_gl_vertex_array(ctx);
    if (!a) return 0;
    switch (pname) {
        case 0x8622: v[0] = a->enabled; return 1;                          /* GL_VERTEX_ATTRIB_ARRAY_ENABLED */
        case 0x8623: v[0] = a->size; return 1;                             /* _SIZE */
        case 0x8624: v[0] = a->stride; return 1;                           /* _STRIDE */
        case 0x8625: v[0] = a->type; return 1;                             /* _TYPE */
        case 0x886a: v[0] = a->normalized; return 1;                       /* _NORMALIZED */
        case 0x889f: v[0] = array->bindings[a->binding].buffer; return 1;  /* _BUFFER_BINDING */
        case 0x82d4: v[0] = a->binding; return 1;                          /* GL_VERTEX_ATTRIB_BINDING */
        case 0x82d5: v[0] = a->relative; return 1;                         /* GL_VERTEX_ATTRIB_RELATIVE_OFFSET */
        case 0x88fd: v[0] = a->integer; return 1;                          /* _INTEGER */
        case 0x88fe: v[0] = array->bindings[a->binding].divisor; return 1; /* _DIVISOR */
        case 0x8626: {                                                     /* GL_CURRENT_VERTEX_ATTRIB */
            const SnxGlGeneric* g = &snx_gl_objects(ctx)->generic[index];
            for (int i = 0; i < 4; i++) {
                float f;
                memcpy(&f, &g->bits[i], 4);
                v[i] = g->base == BASE_FLOAT ? f : g->base == BASE_INT ? (double)(int32_t)g->bits[i] : g->bits[i];
            }
            return 4;
        }
        default: return error(ctx, SNX_GL_INVALID_ENUM), 0;
    }
}

/* ---- the save state ---- */

size_t snx_gl_object_state_bytes(void) { return sizeof(uint32_t) * 2 + sizeof S; }
/* the held ranges free first, their work waited for: a load finds none of that work */
void snx_gl_object_state_save(void* out) {
    const uint32_t h[2] = {(uint32_t)sizeof S, 0};
    held_wait();
    memcpy(out, h, sizeof h);
    memcpy((unsigned char*)out + sizeof h, &S, sizeof S);
}
/* the first free range of s that ends past at: its index, free_count for none */
static uint32_t free_after(const SnxGlObjects* s, uint64_t at) {
    uint32_t lo = 0, hi = s->free_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->free[mid].at + s->free[mid].size <= at)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
/* whether [at, at + size) lies in one chunk of s */
static int in_chunk(const SnxGlObjects* s, uint64_t at, uint64_t size) {
    for (uint32_t i = 0; i < s->chunk_count; i++)
        if (at >= s->chunks[i].at && size <= s->chunks[i].size && at - s->chunks[i].at <= s->chunks[i].size - size)
            return 1;
    return 0;
}
int snx_gl_object_state_check(const void* in, size_t size, char* why, size_t whysize) {
    SnxGlObjects* const c = &g_gl_object.checked;
    uint32_t h[2];
    if (size < sizeof h) {
        snprintf(why, whysize, "the GL layer's buffers are %zu bytes, less than their %zu-byte header", size, sizeof h);
        return -1;
    }
    memcpy(h, in, sizeof h);
    if (h[0] != sizeof *c || size != sizeof h + sizeof *c) {
        snprintf(why, whysize, "the GL layer's buffers are %u bytes of records (%zu in all); this build reads %zu",
                 (unsigned)h[0], size, sizeof *c);
        return -1;
    }
    memcpy(c, (const unsigned char*)in + sizeof h, sizeof *c);
    if (c->chunk_count > SNX_GL_CHUNKS || c->free_count > SNX_GL_FREE_RANGES) {
        snprintf(why, whysize, "the GL layer's memory has %u chunks and %u free ranges, past %d and %d",
                 (unsigned)c->chunk_count, (unsigned)c->free_count, SNX_GL_CHUNKS, SNX_GL_FREE_RANGES);
        return -1;
    }
    for (uint32_t i = 0; i < c->chunk_count; i++)
        if (c->untouched[i] - c->chunks[i].at > c->chunks[i].size) {
            snprintf(why, whysize, "the GL layer's chunk %u at %llx+%llx has its untouched bytes from %llx, outside it",
                     (unsigned)i, (unsigned long long)c->chunks[i].at, (unsigned long long)c->chunks[i].size,
                     (unsigned long long)c->untouched[i]);
            return -1;
        }
    for (uint32_t i = 0; i < c->free_count; i++)
        if (!in_chunk(c, c->free[i].at, c->free[i].size) ||
            (i && c->free[i].at < c->free[i - 1].at + c->free[i - 1].size)) {
            snprintf(why, whysize, "the GL layer's free range %u at %llx+%llx is outside its chunks or overlaps",
                     (unsigned)i, (unsigned long long)c->free[i].at, (unsigned long long)c->free[i].size);
            return -1;
        }
    for (uint32_t n = 1; n <= SNX_GL_BUFFERS; n++) {
        const SnxGlBuffer* b = &c->buffers[n - 1];
        if (!b->used) continue;
        if (b->size > b->capacity || (b->data && !in_chunk(c, b->data, b->capacity)) || (!b->data && b->size) ||
            (b->mapped && (b->map_offset > b->size || b->map_length > b->size - b->map_offset))) {
            snprintf(why, whysize,
                     "the GL layer's buffer %u has %llu bytes in %llu at %llx, mapped %u at %llu+%llu, outside its "
                     "memory",
                     (unsigned)n, (unsigned long long)b->size, (unsigned long long)b->capacity,
                     (unsigned long long)b->data, (unsigned)b->mapped, (unsigned long long)b->map_offset,
                     (unsigned long long)b->map_length);
            return -1;
        }
        {
            uint32_t f = free_after(c, b->data);
            if (b->capacity && f < c->free_count && c->free[f].at < b->data + b->capacity) {
                snprintf(why, whysize, "the GL layer's buffer %u at %llx+%llx overlaps free range %u at %llx+%llx",
                         (unsigned)n, (unsigned long long)b->data, (unsigned long long)b->capacity, (unsigned)f,
                         (unsigned long long)c->free[f].at, (unsigned long long)c->free[f].size);
                return -1;
            }
        }
    }
    for (uint32_t n = 0; n < SNX_GL_VERTEX_ARRAYS + SNX_GL_CONTEXTS; n++) {
        /* the arrays made, then each context's own */
        const SnxGlVertexArray* a =
            n < SNX_GL_VERTEX_ARRAYS ? &c->arrays[n] : &c->contexts[n - SNX_GL_VERTEX_ARRAYS].vao;
        if (a->elements > SNX_GL_BUFFERS) {
            snprintf(why, whysize, "the GL layer's vertex array %u has element buffer %u, past %d", (unsigned)(n + 1),
                     (unsigned)a->elements, SNX_GL_BUFFERS);
            return -1;
        }
        for (int k = 0; k < SNX_GL_VERTEX_ATTRIBS; k++)
            if (a->attribs[k].binding >= SNX_GL_VERTEX_ATTRIBS || a->bindings[k].buffer > SNX_GL_BUFFERS) {
                snprintf(why, whysize, "the GL layer's vertex array %u has attribute %d at binding %u, buffer %u",
                         (unsigned)(n + 1), k, (unsigned)a->attribs[k].binding, (unsigned)a->bindings[k].buffer);
                return -1;
            }
    }
    for (uint32_t i = 0; i < SNX_GL_CONTEXTS; i++)
        if (c->contexts[i].vertex_array > SNX_GL_VERTEX_ARRAYS) {
            snprintf(why, whysize, "the GL layer's context %u binds vertex array %u, past %d", (unsigned)(i + 1),
                     (unsigned)c->contexts[i].vertex_array, SNX_GL_VERTEX_ARRAYS);
            return -1;
        }
    return 0;
}
int snx_gl_object_checked_holds(uint64_t at, uint64_t size) { return in_chunk(&g_gl_object.checked, at, size); }
void snx_gl_object_state_load(const void* in) {
    memcpy(&S, (const unsigned char*)in + 2 * sizeof(uint32_t), sizeof S);
    memset(g_gl_object.read, 0, sizeof g_gl_object.read);
    memset(g_gl_object.valid, 0, sizeof g_gl_object.valid);
    g_gl_object.held_count = 0;
    for (uint32_t n = 1; n <= SNX_GL_BUFFERS; n++) gone(n); /* what a platform made is of the old records */
}
