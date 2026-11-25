/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's buffer and vertex array calls on the console (gl_layer.h, gpu/gl/gl.hook's object family): each the
   shared layer's (common/gpu/gl/gl_object.h) on the calling thread's current context, under the layer's object lock
   (buffers are shared between a game's contexts and threads). A buffer's bytes live in guest memory, in chunks the
   layer takes from the kernel's region: a map is an address the game writes through, persistent ones too, and a save
   state holds the bytes with the game's memory. Without a current context a call does nothing and returns 0 */
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_state.h"
#include "common/platform/thread.h"
#include "gpu/gl/gl.hook.h"
#include "nx/gpu/gl/gl_layer.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/memory/mem.h"
#include "nx/runtime/platform/log.h"
#include <string.h>

enum {
    GL_MAP_READ_BIT_ = 0x1,
    GL_MAP_WRITE_BIT_ = 0x2,
    GL_READ_ONLY_ = 0x88b8,
    GL_WRITE_ONLY_ = 0x88b9,
    GL_BUFFER_MAP_POINTER_ = 0x88bd,
    BASE_INT = 1, /* GlslcBase's */
    BASE_UINT = 2,
    BASE_FLOAT = 3,
};

#define GPU_CHUNKS 64 /* the layer's chunks of guest memory, each mapped in the GPU's address space */

/* a chunk of the layer's memory: its guest address, bytes and GPU address */
typedef struct {
    u64 at, size, gpu;
} GpuChunk;

static struct {
    PMutex lock;
    GpuChunk chunks[GPU_CHUNKS];
    u32 chunk_count;
    u64 mapped; /* the bytes of guest memory the layer took (gl_guest_pages) */
} g_gl_object_calls = {.lock = PMUTEX_INIT};

static void* host(u64 a) { return a ? GP(a) : NULL; }

u64 gl_guest_pages(u64 size) {
    /* past every region the console gives a program (the kernel's, the heap, the alias region, the stacks), as the
       hooked allocator's arena (nn/alloc.c): the kernel's own region is a GiB, which a game's textures fill */
    u64 lo = KERN_END, a = 0;
    if (HEAP_BASE + HEAP_SIZE > lo) lo = HEAP_BASE + HEAP_SIZE;
    if (ALIAS_BASE + ALIAS_SIZE > lo) lo = ALIAS_BASE + ALIAS_SIZE;
    if (STACK_BASE + STACK_SIZE > lo) lo = STACK_BASE + STACK_SIZE;
    lo = (lo + 0x1fffff) & ~0x1fffffull;
    size = (size + 0xffff) & ~0xffffull;
    for (int tries = 0; tries < 8 && lo < AS_END; tries++) {
        a = mem_find_free(lo, AS_END, size, 0x10000);
        if (!a) break;
        if (!mem_map(a, size, MS_NORMAL, PERM_RW)) {
            g_gl_object_calls.mapped += size;
            return a;
        }
        a = 0; /* another mapping took it first: look again */
    }
    fatal("gl layer: no room for %llu bytes of its memory past the program's regions (%llu taken so far)",
          (unsigned long long)size, (unsigned long long)g_gl_object_calls.mapped);
}
u64 gl_guest_pages_taken(void) { return g_gl_object_calls.mapped; }

/* the layer's buffers and textures in chunks of guest memory, each mapped in the GPU's address space too, where the
   renderer's draws read them. A chunk is half of what the layer took at least, so the chunks hold a game's whole
   memory */
static u64 chunk(u64 size, u64* got) {
    u64 at, gpu, pages = (size + 0xffff) & ~0xffffull, half = (g_gl_object_calls.mapped / 2 + 0xffff) & ~0xffffull;
    if (g_gl_object_calls.chunk_count == GPU_CHUNKS)
        fatal("gl layer: its memory takes a chunk of %llu bytes past the %d it maps for the GPU",
              (unsigned long long)size, GPU_CHUNKS);
    if (pages < half) pages = half;
    at = gl_guest_pages(pages);
    gpu = at ? gmmu_space_alloc(pages, 0) : 0;
    if (at && !gpu)
        fatal("gl layer: no GPU address space left for %llu bytes of its memory", (unsigned long long)pages);
    if (!at) return *got = 0, 0;
    gmmu_space_map(gpu, at, pages);
    g_gl_object_calls.chunks[g_gl_object_calls.chunk_count++] = (GpuChunk){at, pages, gpu};
    *got = pages;
    return at;
}
u64 gl_gpu_address(u64 guest) {
    for (u32 i = 0; i < g_gl_object_calls.chunk_count; i++)
        if (guest - g_gl_object_calls.chunks[i].at < g_gl_object_calls.chunks[i].size)
            return g_gl_object_calls.chunks[i].gpu + (guest - g_gl_object_calls.chunks[i].at);
    return 0;
}
static u8* bytes(u64 at) { return GP(at); }
/* the CPU's writes of a buffer's bytes that the runtime made (glBufferData, glBufferSubData, glCopyBufferSubData...):
   the GPU's caches are told, as they see the game's own writes but not the runtime's */
static void written(uint32_t buffer, uint64_t lo, uint64_t hi) {
    const SnxGlBuffer* b = snx_gl_buffer(buffer);
    if (b && b->data) gpu_mem_written_cpu(b->data + lo, hi - lo);
}
/* a map's wait for the draws, off the guest core as every wait of the layer's */
static void wait(uint64_t serial) { gl_draw_wait(0, serial); }
void gl_object_calls_init(void) {
    snx_gl_object_host(&(SnxGlObjectHost){.chunk = chunk,
                                          .bytes = bytes,
                                          .written = written,
                                          .issued = gl_draw_recorded,
                                          .finished = gl_draw_completed,
                                          .wait = wait});
}
void gl_object_calls_lock(void) { pm_lock(&g_gl_object_calls.lock); }
void gl_object_calls_unlock(void) { pm_unlock(&g_gl_object_calls.lock); }

#define LOCKED(fallback, ...)                                                                                          \
    do {                                                                                                               \
        u32 ctx = gl_egl_context();                                                                                    \
        (void)c;                                                                                                       \
        if (!ctx) return fallback;                                                                                     \
        pm_lock(&g_gl_object_calls.lock);                                                                              \
        __VA_ARGS__;                                                                                                   \
        pm_unlock(&g_gl_object_calls.lock);                                                                            \
    } while (0)
#define NONE

/* a buffer named by a DSA call: 0 with GL_INVALID_OPERATION recorded for a name that is none */
static u32 named(u32 ctx, u32 buffer) {
    if (snx_gl_buffer(buffer)) return buffer;
    snx_gl_error(ctx, SNX_GL_INVALID_OPERATION);
    return 0;
}

/* ---- buffers ---- */

void gl_gen_buffers(CPU* c, i32 n, u64 buffers) { LOCKED(NONE, snx_gl_gen_buffers(ctx, n, host(buffers))); }
void gl_delete_buffers(CPU* c, i32 n, u64 buffers) { LOCKED(NONE, snx_gl_delete_buffers(ctx, n, host(buffers))); }
u8 gl_is_buffer(CPU* c, u32 buffer) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_buffer(buffer));
    return is;
}
void gl_bind_buffer(CPU* c, u32 target, u32 buffer) { LOCKED(NONE, snx_gl_bind_buffer(ctx, target, buffer)); }
void gl_bind_buffer_base(CPU* c, u32 target, u32 index, u32 buffer) {
    LOCKED(NONE, snx_gl_bind_buffer_range(ctx, target, index, buffer, 0, -1));
}
void gl_bind_buffer_range(CPU* c, u32 target, u32 index, u32 buffer, i64 offset, i64 size) {
    LOCKED(NONE, snx_gl_bind_buffer_range(ctx, target, index, buffer, offset, size));
}
void gl_buffer_data(CPU* c, u32 target, i64 size, u64 data, u32 usage) {
    LOCKED(NONE, u32 b = snx_gl_bound_buffer(ctx, target); if (b) snx_gl_buffer_data(ctx, b, size, host(data), usage));
}
void gl_named_buffer_data(CPU* c, u32 buffer, i64 size, u64 data, u32 usage) {
    LOCKED(NONE, if (named(ctx, buffer)) snx_gl_buffer_data(ctx, buffer, size, host(data), usage));
}
void gl_buffer_storage(CPU* c, u32 target, i64 size, u64 data, u32 flags) {
    LOCKED(NONE, u32 b = snx_gl_bound_buffer(ctx, target);
           if (b) snx_gl_buffer_storage(ctx, b, size, host(data), flags));
}
void gl_named_buffer_storage(CPU* c, u32 buffer, i64 size, u64 data, u32 flags) {
    LOCKED(NONE, if (named(ctx, buffer)) snx_gl_buffer_storage(ctx, buffer, size, host(data), flags));
}
void gl_buffer_sub_data(CPU* c, u32 target, i64 offset, i64 size, u64 data) {
    LOCKED(NONE, u32 b = snx_gl_bound_buffer(ctx, target);
           if (b) snx_gl_buffer_sub_data(ctx, b, offset, size, host(data)));
}
void gl_named_buffer_sub_data(CPU* c, u32 buffer, i64 offset, i64 size, u64 data) {
    LOCKED(NONE, if (named(ctx, buffer)) snx_gl_buffer_sub_data(ctx, buffer, offset, size, host(data)));
}
void gl_get_buffer_sub_data(CPU* c, u32 target, i64 offset, i64 size, u64 data) {
    LOCKED(NONE, u32 b = snx_gl_bound_buffer(ctx, target);
           if (b) snx_gl_get_buffer_sub_data(ctx, b, offset, size, host(data)));
}
void gl_get_named_buffer_sub_data(CPU* c, u32 buffer, i64 offset, i64 size, u64 data) {
    LOCKED(NONE, if (named(ctx, buffer)) snx_gl_get_buffer_sub_data(ctx, buffer, offset, size, host(data)));
}
u64 gl_map_buffer_range(CPU* c, u32 target, i64 offset, i64 length, u32 access) {
    u64 at = 0;
    LOCKED(0, u32 b = snx_gl_bound_buffer(ctx, target);
           if (b) at = snx_gl_map_buffer_range(ctx, b, offset, length, access));
    return at;
}
u64 gl_map_named_buffer_range(CPU* c, u32 buffer, i64 offset, i64 length, u32 access) {
    u64 at = 0;
    LOCKED(0, if (named(ctx, buffer)) at = snx_gl_map_buffer_range(ctx, buffer, offset, length, access));
    return at;
}
/* glMapBuffer's access (GL_READ_ONLY, GL_WRITE_ONLY, GL_READ_WRITE) as glMapBufferRange's bits, of the whole buffer */
static u64 map_whole(u32 ctx, u32 buffer, u32 access) {
    const SnxGlBuffer* b = snx_gl_buffer(buffer);
    u32 bits = access == GL_READ_ONLY_    ? GL_MAP_READ_BIT_
               : access == GL_WRITE_ONLY_ ? GL_MAP_WRITE_BIT_
                                          : GL_MAP_READ_BIT_ | GL_MAP_WRITE_BIT_;
    if (access < GL_READ_ONLY_ || access > GL_READ_ONLY_ + 2) {
        snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
        return 0;
    }
    return b ? snx_gl_map_buffer_range(ctx, buffer, 0, (i64)b->size, bits) : 0;
}
u64 gl_map_buffer(CPU* c, u32 target, u32 access) {
    u64 at = 0;
    LOCKED(0, u32 b = snx_gl_bound_buffer(ctx, target); if (b) at = map_whole(ctx, b, access));
    return at;
}
u64 gl_map_named_buffer(CPU* c, u32 buffer, u32 access) {
    u64 at = 0;
    LOCKED(0, if (named(ctx, buffer)) at = map_whole(ctx, buffer, access));
    return at;
}
u8 gl_unmap_buffer(CPU* c, u32 target) {
    u8 ok = 0;
    LOCKED(0, u32 b = snx_gl_bound_buffer(ctx, target); if (b) ok = (u8)snx_gl_unmap_buffer(ctx, b));
    return ok;
}
u8 gl_unmap_named_buffer(CPU* c, u32 buffer) {
    u8 ok = 0;
    LOCKED(0, if (named(ctx, buffer)) ok = (u8)snx_gl_unmap_buffer(ctx, buffer));
    return ok;
}
void gl_flush_mapped_buffer_range(CPU* c, u32 target, i64 offset, i64 length) {
    LOCKED(NONE, u32 b = snx_gl_bound_buffer(ctx, target); if (b) snx_gl_flush_mapped_range(ctx, b, offset, length));
}
void gl_flush_mapped_named_buffer_range(CPU* c, u32 buffer, i64 offset, i64 length) {
    LOCKED(NONE, if (named(ctx, buffer)) snx_gl_flush_mapped_range(ctx, buffer, offset, length));
}
static void pointer(u32 ctx, u32 buffer, u32 pname, u64 params) {
    if (pname != GL_BUFFER_MAP_POINTER_) {
        snx_gl_error(ctx, SNX_GL_INVALID_ENUM);
        return;
    }
    gw64(params, snx_gl_buffer_pointer(ctx, buffer));
}
void gl_get_buffer_pointerv(CPU* c, u32 target, u32 pname, u64 params) {
    LOCKED(NONE, u32 b = snx_gl_bound_buffer(ctx, target); if (b) pointer(ctx, b, pname, params));
}
void gl_get_named_buffer_pointerv(CPU* c, u32 buffer, u32 pname, u64 params) {
    LOCKED(NONE, if (named(ctx, buffer)) pointer(ctx, buffer, pname, params));
}
void gl_copy_buffer_sub_data(CPU* c, u32 read, u32 write, i64 read_offset, i64 write_offset, i64 size) {
    LOCKED(NONE, u32 r = snx_gl_bound_buffer(ctx, read); u32 w = r ? snx_gl_bound_buffer(ctx, write) : 0;
           if (w) snx_gl_copy_buffer_sub_data(ctx, r, w, read_offset, write_offset, size));
}
void gl_copy_named_buffer_sub_data(CPU* c, u32 read, u32 write, i64 read_offset, i64 write_offset, i64 size) {
    LOCKED(NONE, if (named(ctx, read) && named(ctx, write))
                     snx_gl_copy_buffer_sub_data(ctx, read, write, read_offset, write_offset, size));
}
/* glGetBufferParameter*v's value of pname into params, 32 or 64 bits wide */
static void parameter(u32 ctx, u32 buffer, u32 pname, u64 params, int wide) {
    i64 v = 0;
    if (!snx_gl_buffer_parameter(ctx, buffer, pname, &v)) return;
    if (wide)
        gw64(params, (u64)v);
    else
        gw32(params, (u32)(i32)v);
}
void gl_get_buffer_parameteriv(CPU* c, u32 target, u32 pname, u64 params) {
    LOCKED(NONE, u32 b = snx_gl_bound_buffer(ctx, target); if (b) parameter(ctx, b, pname, params, 0));
}
void gl_get_buffer_parameteri64v(CPU* c, u32 target, u32 pname, u64 params) {
    LOCKED(NONE, u32 b = snx_gl_bound_buffer(ctx, target); if (b) parameter(ctx, b, pname, params, 1));
}
void gl_get_named_buffer_parameteriv(CPU* c, u32 buffer, u32 pname, u64 params) {
    LOCKED(NONE, if (named(ctx, buffer)) parameter(ctx, buffer, pname, params, 0));
}
void gl_get_named_buffer_parameteri64v(CPU* c, u32 buffer, u32 pname, u64 params) {
    LOCKED(NONE, if (named(ctx, buffer)) parameter(ctx, buffer, pname, params, 1));
}

/* ---- vertex arrays ---- */

void gl_gen_vertex_arrays(CPU* c, i32 n, u64 arrays) { LOCKED(NONE, snx_gl_gen_vertex_arrays(ctx, n, host(arrays))); }
void gl_delete_vertex_arrays(CPU* c, i32 n, u64 arrays) {
    LOCKED(NONE, snx_gl_delete_vertex_arrays(ctx, n, host(arrays)));
}
u8 gl_is_vertex_array(CPU* c, u32 array) {
    u8 is = 0;
    LOCKED(0, is = (u8)snx_gl_is_vertex_array(array));
    return is;
}
void gl_bind_vertex_array(CPU* c, u32 array) { LOCKED(NONE, snx_gl_bind_vertex_array(ctx, array)); }
void gl_vertex_attrib_pointer(CPU* c, u32 index, i32 size, u32 type, u8 normalized, i32 stride, u64 pointer) {
    LOCKED(NONE, snx_gl_vertex_attrib_pointer(ctx, index, size, type, normalized != 0, 0, stride, pointer));
}
void gl_vertex_attrib_ipointer(CPU* c, u32 index, i32 size, u32 type, i32 stride, u64 pointer) {
    LOCKED(NONE, snx_gl_vertex_attrib_pointer(ctx, index, size, type, 0, 1, stride, pointer));
}
void gl_vertex_attrib_format(CPU* c, u32 index, i32 size, u32 type, u8 normalized, u32 relative) {
    LOCKED(NONE, snx_gl_vertex_attrib_format(ctx, index, size, type, normalized != 0, 0, relative));
}
void gl_vertex_attrib_iformat(CPU* c, u32 index, i32 size, u32 type, u32 relative) {
    LOCKED(NONE, snx_gl_vertex_attrib_format(ctx, index, size, type, 0, 1, relative));
}
void gl_vertex_attrib_binding(CPU* c, u32 index, u32 binding) {
    LOCKED(NONE, snx_gl_vertex_attrib_binding(ctx, index, binding));
}
void gl_bind_vertex_buffer(CPU* c, u32 binding, u32 buffer, i64 offset, i32 stride) {
    LOCKED(NONE, snx_gl_bind_vertex_buffer(ctx, binding, buffer, offset, stride));
}
void gl_vertex_binding_divisor(CPU* c, u32 binding, u32 divisor) {
    LOCKED(NONE, snx_gl_vertex_binding_divisor(ctx, binding, divisor));
}
void gl_enable_vertex_attrib_array(CPU* c, u32 index) { LOCKED(NONE, snx_gl_enable_vertex_attrib(ctx, index, 1)); }
void gl_disable_vertex_attrib_array(CPU* c, u32 index) { LOCKED(NONE, snx_gl_enable_vertex_attrib(ctx, index, 0)); }
void gl_vertex_attrib_divisor(CPU* c, u32 index, u32 divisor) {
    LOCKED(NONE, snx_gl_vertex_attrib_divisor(ctx, index, divisor));
}
#define ATTRIB_F(n, ...)                                                                                               \
    void gl_vertex_attrib##n##f(CPU* c, u32 index, __VA_ARGS__) {                                                      \
        const f32 v[n] = {ATTRIB_ARGS##n};                                                                             \
        LOCKED(NONE, snx_gl_vertex_attrib(ctx, index, v, n, BASE_FLOAT));                                              \
    }                                                                                                                  \
    void gl_vertex_attrib##n##fv(CPU* c, u32 index, u64 v) {                                                           \
        LOCKED(NONE, snx_gl_vertex_attrib(ctx, index, host(v), n, BASE_FLOAT));                                        \
    }
#define ATTRIB_ARGS1 x
#define ATTRIB_ARGS2 x, y
#define ATTRIB_ARGS3 x, y, z
#define ATTRIB_ARGS4 x, y, z, w
ATTRIB_F(1, f32 x)
ATTRIB_F(2, f32 x, f32 y)
ATTRIB_F(3, f32 x, f32 y, f32 z)
ATTRIB_F(4, f32 x, f32 y, f32 z, f32 w)
void gl_vertex_attrib_i4i(CPU* c, u32 index, i32 x, i32 y, i32 z, i32 w) {
    const i32 v[4] = {x, y, z, w};
    LOCKED(NONE, snx_gl_vertex_attrib(ctx, index, v, 4, BASE_INT));
}
void gl_vertex_attrib_i4ui(CPU* c, u32 index, u32 x, u32 y, u32 z, u32 w) {
    const u32 v[4] = {x, y, z, w};
    LOCKED(NONE, snx_gl_vertex_attrib(ctx, index, v, 4, BASE_UINT));
}
void gl_vertex_attrib_i4iv(CPU* c, u32 index, u64 v) {
    LOCKED(NONE, snx_gl_vertex_attrib(ctx, index, host(v), 4, BASE_INT));
}
void gl_vertex_attrib_i4uiv(CPU* c, u32 index, u64 v) {
    LOCKED(NONE, snx_gl_vertex_attrib(ctx, index, host(v), 4, BASE_UINT));
}
/* glGetVertexAttrib*'s values of pname into params, kind 'i' or 'f' */
static void get_attrib(u32 ctx, u32 index, u32 pname, u64 params, char kind) {
    double v[4];
    int n = snx_gl_get_vertex_attrib(ctx, index, pname, v);
    for (int i = 0; i < n; i++) {
        if (kind == 'f') {
            f32 f = (f32)v[i];
            u32 bits;
            memcpy(&bits, &f, 4);
            gw32(params + 4 * (u64)i, bits);
        } else {
            gw32(params + 4 * (u64)i, (u32)(i32)v[i]);
        }
    }
}
void gl_get_vertex_attribiv(CPU* c, u32 index, u32 pname, u64 params) {
    LOCKED(NONE, get_attrib(ctx, index, pname, params, 'i'));
}
void gl_get_vertex_attribfv(CPU* c, u32 index, u32 pname, u64 params) {
    LOCKED(NONE, get_attrib(ctx, index, pname, params, 'f'));
}
void gl_get_vertex_attrib_pointerv(CPU* c, u32 index, u32 pname, u64 params) {
    (void)pname; /* GL_VERTEX_ATTRIB_ARRAY_POINTER, the only one */
    LOCKED(NONE, const SnxGlVertexArray* a = snx_gl_vertex_array(ctx);
           if (index >= SNX_GL_VERTEX_ATTRIBS) snx_gl_error(ctx, SNX_GL_INVALID_VALUE);
           else gw64(params, a->bindings[a->attribs[index].binding].offset));
}
