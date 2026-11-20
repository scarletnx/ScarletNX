/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The GL layer's buffers and vertex arrays (gl_state.h's layer), shared by the Android bridge and the console.

   A buffer's bytes live in the platform's memory, where the client reads and writes them through the pointers GL
   gives it: guest memory on the console, so a map (persistent ones too) is an address the game writes through and a
   save state holds them with the game's memory; host memory in the Android bridge, where the guest and the host are
   one. The layer asks its platform for chunks of that memory (SnxGlObjectHost.chunk) and places each buffer's bytes
   in them itself, with a free list among its records, so a save state holds where every buffer is.

   The records are flat, named by index + 1, and one block with the per-context state (each context's buffer
   bindings, its vertex array 0 and the generic attributes' values) that a save state holds as it is. A platform's GPU
   copy of a buffer is its own: the layer tells it which bytes the CPU wrote, asks it for the GPU's writes before the
   CPU reads or writes, and tells it when a buffer's bytes go. The caller holds its platform's GL lock.

   GL's draws read a buffer's bytes as they were when the draw was called. A platform whose GPU side reads them
   later, in order with its calls (SnxGlObjectHost.issued: the console's GPU thread), names the buffers each draw
   reads (snx_gl_buffer_read_by). A write of bytes such a draw still reads then gives the buffer a new range, as a GL
   driver orphans a buffer, the written bytes copied over, and the old range is freed only once the work that reads
   it is done; a write past the bytes written so far keeps the range. Only a map that asks for GL's sync of bytes in
   use waits, and a mapped buffer, which keeps its range. An engine that streams its vertices refills one buffer every
   frame while the GPU side is still drawing the frame before */
#ifndef SNX_GL_OBJECT_H
#define SNX_GL_OBJECT_H
#include <stddef.h>
#include <stdint.h>

#define SNX_GL_BUFFERS          8192 /* buffer names: a game made 1229 */
#define SNX_GL_VERTEX_ARRAYS    1024
#define SNX_GL_VERTEX_ATTRIBS   16 /* GL_MAX_VERTEX_ATTRIBS, and the vertex buffer bindings */
#define SNX_GL_UNIFORM_BINDINGS 36
#define SNX_GL_STORAGE_BINDINGS 8
#define SNX_GL_CHUNKS           64   /* the platform's chunks of memory the buffers live in */
#define SNX_GL_FREE_RANGES      8192 /* free ranges among them */

/* a buffer: size bytes at data, the platform's address of them (0 for none), in capacity bytes it holds */
typedef struct {
    uint8_t used, mapped, immutable, pad;
    uint32_t usage;      /* glBufferData's, GL_STATIC_DRAW and the others */
    uint32_t flags;      /* glBufferStorage's: GL_MAP_PERSISTENT_BIT and the others */
    uint32_t map_access; /* the open map's access bits */
    uint64_t data, size, capacity;
    uint64_t map_offset, map_length;
} SnxGlBuffer;

/* an attribute's format, and the binding it reads (glVertexAttribBinding; glVertexAttribPointer sets binding i of
   attribute i) */
typedef struct {
    uint8_t enabled, normalized, integer, binding;
    int32_t size;
    uint32_t type;
    int32_t stride;    /* glVertexAttribPointer's, for its query */
    uint32_t relative; /* the offset in the binding's vertex */
} SnxGlAttrib;

typedef struct {
    uint32_t buffer;
    uint32_t stride, divisor, pad;
    uint64_t offset; /* in buffer, or the client array's address without one */
} SnxGlVertexBinding;

typedef struct {
    uint32_t used, elements; /* elements: the element array buffer */
    SnxGlAttrib attribs[SNX_GL_VERTEX_ATTRIBS];
    SnxGlVertexBinding bindings[SNX_GL_VERTEX_ATTRIBS];
} SnxGlVertexArray;

/* a generic attribute's current value (glVertexAttrib4f), as 32-bit words of its base (GlslcBase) */
typedef struct {
    uint32_t bits[4];
    uint32_t base;
} SnxGlGeneric;

typedef struct {
    uint32_t buffer, pad;
    int64_t offset, size; /* size -1: to the buffer's end (glBindBufferBase) */
} SnxGlIndexedBuffer;

/* what a context binds of buffers and vertex arrays */
typedef struct {
    uint32_t array, copy_read, copy_write, pixel_pack, pixel_unpack, uniform, feedback, storage, draw_indirect,
        dispatch_indirect, query;
    uint32_t vertex_array; /* 0: the context's own, vao below */
    SnxGlIndexedBuffer uniforms[SNX_GL_UNIFORM_BINDINGS];
    SnxGlIndexedBuffer storages[SNX_GL_STORAGE_BINDINGS];
    SnxGlGeneric generic[SNX_GL_VERTEX_ATTRIBS];
    SnxGlVertexArray vao;
} SnxGlObjectContext;

/* What the layer asks of its platform. Not part of a save state: a platform gives it again after a load */
typedef struct {
    /* a new chunk of at least size bytes for buffers: its address and its size in *got, 0 when there is none. Its
       bytes are zeros, which the layer leaves as they are until something takes them. Chunks are never given back: a
       buffer deleted leaves its range free for the next */
    uint64_t (*chunk)(uint64_t size, uint64_t* got);
    /* the host pointer to the platform's address at */
    uint8_t* (*bytes)(uint64_t at);
    /* the GPU's writes to buffer, brought into its bytes before the CPU reads or writes them; may be NULL */
    void (*read_back)(uint32_t buffer);
    /* the CPU wrote buffer's bytes [lo, hi), newer than the platform's copy and than the GPU's writes there; [0, its
       size) is all of them (glBufferData); may be NULL */
    void (*written)(uint32_t buffer, uint64_t lo, uint64_t hi);
    /* buffer's bytes went (deleted, or placed anew): the platform's copy of them goes too; may be NULL */
    void (*gone)(uint32_t buffer);
    /* A GPU side that reads the platform's memory after the calls that drew with it returned, in order with them; NULL
       for one that reads it at the call. The serial of the last work handed to it, of the last it finished, and a
       wait until it finished serial */
    uint64_t (*issued)(void);
    uint64_t (*finished)(void);
    void (*wait)(uint64_t serial);
} SnxGlObjectHost;
void snx_gl_object_host(const SnxGlObjectHost* host);

/* a context made anew: its bindings none, its vertex array 0 empty, the generic attributes (0, 0, 0, 1) */
void snx_gl_object_context_reset(uint32_t ctx);
SnxGlObjectContext* snx_gl_objects(uint32_t ctx);
SnxGlBuffer* snx_gl_buffer(uint32_t name);
/* a buffer's bytes for the CPU, the GPU's writes brought in: NULL for a buffer without any */
uint8_t* snx_gl_buffer_bytes(uint32_t name);
/* the vertex array bound (the context's own for 0, or a name deleted since) */
SnxGlVertexArray* snx_gl_vertex_array(uint32_t ctx);
/* the host pointer of a client's or the platform's address (the platform's bytes), NULL for 0 */
uint8_t* snx_gl_client_bytes(uint64_t at);
/* size bytes of the platform's memory the layer places things in (buffers' bytes, textures' kept texels): their
   address, 0 when there is none; and given back */
uint64_t snx_gl_memory_take(uint64_t size, uint64_t align); /* at a multiple of align, a power of 2 (0 otherwise) */
/* the same, and zeros: only those bytes an earlier taker may have written are written, the rest being the platform's
   zeros still */
uint64_t snx_gl_memory_take_zeroed(uint64_t size, uint64_t align);
void snx_gl_memory_give(uint64_t at, uint64_t size);
/* after snx_gl_object_state_check passed on a copy: whether that copy's memory holds [at, at + size) in a chunk */
int snx_gl_object_checked_holds(uint64_t at, uint64_t size);
/* glGet*'s binding of buffers and vertex arrays of pname into v[0]: 1, or 0 for a pname not of them */
int snx_gl_object_get(uint32_t ctx, uint32_t pname, double v[4]);
/* the bytes of a vertex attribute of size components of type, packed */
uint32_t snx_gl_vertex_bytes(uint32_t type, int32_t size);

/* GL's calls. Each records GL's error for what GL refuses; client memory goes in and out through host pointers */
void snx_gl_gen_buffers(uint32_t ctx, int32_t n, uint32_t* out);
void snx_gl_delete_buffers(uint32_t ctx, int32_t n, const uint32_t* names);
int snx_gl_is_buffer(uint32_t name);
void snx_gl_bind_buffer(uint32_t ctx, uint32_t target, uint32_t name);
/* glBindBufferBase (size -1) and glBindBufferRange */
void snx_gl_bind_buffer_range(uint32_t ctx, uint32_t target, uint32_t index, uint32_t name, int64_t offset,
                              int64_t size);
/* the buffer bound to target: 0 with GL_INVALID_ENUM recorded for a target GL has not, or GL_INVALID_OPERATION for
   none bound */
uint32_t snx_gl_bound_buffer(uint32_t ctx, uint32_t target);
/* glBufferData and glNamedBufferData of a buffer by name (0: the error of none bound), data NULL for zeros */
void snx_gl_buffer_data(uint32_t ctx, uint32_t name, int64_t size, const void* data, uint32_t usage);
/* glBufferStorage and glNamedBufferStorage: immutable from then on */
void snx_gl_buffer_storage(uint32_t ctx, uint32_t name, int64_t size, const void* data, uint32_t flags);
void snx_gl_buffer_sub_data(uint32_t ctx, uint32_t name, int64_t offset, int64_t size, const void* data);
void snx_gl_get_buffer_sub_data(uint32_t ctx, uint32_t name, int64_t offset, int64_t size, void* out);
/* glMapBufferRange: the platform's address of the bytes mapped, 0 with the error recorded */
uint64_t snx_gl_map_buffer_range(uint32_t ctx, uint32_t name, int64_t offset, int64_t length, uint32_t access);
/* glUnmapBuffer: 1, or 0 with the error recorded for a buffer not mapped */
int snx_gl_unmap_buffer(uint32_t ctx, uint32_t name);
void snx_gl_flush_mapped_range(uint32_t ctx, uint32_t name, int64_t offset, int64_t length);
/* the GPU side's work serial (SnxGlObjectHost.issued) reads buffer's bytes: a write of them after gives the buffer a
   new range, and a map with GL's sync waits for it */
void snx_gl_buffer_read_by(uint32_t name, uint64_t serial);
/* glGetBufferPointerv: the open map's address, 0 for none */
uint64_t snx_gl_buffer_pointer(uint32_t ctx, uint32_t name);
void snx_gl_copy_buffer_sub_data(uint32_t ctx, uint32_t read, uint32_t write, int64_t read_offset, int64_t write_offset,
                                 int64_t size);
/* glGetBufferParameter*: 1 with the value in *v, 0 with GL_INVALID_ENUM recorded */
int snx_gl_buffer_parameter(uint32_t ctx, uint32_t name, uint32_t pname, int64_t* v);

void snx_gl_gen_vertex_arrays(uint32_t ctx, int32_t n, uint32_t* out);
void snx_gl_delete_vertex_arrays(uint32_t ctx, int32_t n, const uint32_t* names);
int snx_gl_is_vertex_array(uint32_t name);
void snx_gl_bind_vertex_array(uint32_t ctx, uint32_t name);
/* glVertexAttribPointer (integer 0) and glVertexAttribIPointer (1): the array buffer bound, pointer its offset */
void snx_gl_vertex_attrib_pointer(uint32_t ctx, uint32_t index, int32_t size, uint32_t type, int normalized,
                                  int integer, int32_t stride, uint64_t pointer);
void snx_gl_vertex_attrib_format(uint32_t ctx, uint32_t index, int32_t size, uint32_t type, int normalized, int integer,
                                 uint32_t relative);
void snx_gl_vertex_attrib_binding(uint32_t ctx, uint32_t index, uint32_t binding);
void snx_gl_bind_vertex_buffer(uint32_t ctx, uint32_t binding, uint32_t buffer, int64_t offset, int32_t stride);
void snx_gl_vertex_binding_divisor(uint32_t ctx, uint32_t binding, uint32_t divisor);
void snx_gl_enable_vertex_attrib(uint32_t ctx, uint32_t index, int on);
void snx_gl_vertex_attrib_divisor(uint32_t ctx, uint32_t index, uint32_t divisor);
/* glVertexAttrib*: n 32-bit words of base (GlslcBase's FLOAT, INT or UINT) at v, the rest (0, 0, 0, 1) */
void snx_gl_vertex_attrib(uint32_t ctx, uint32_t index, const void* v, int n, uint32_t base);
/* glGetVertexAttrib*'s values of pname: how many (1, or 4 for GL_CURRENT_VERTEX_ATTRIB), 0 with the error recorded */
int snx_gl_get_vertex_attrib(uint32_t ctx, uint32_t index, uint32_t pname, double v[4]);

/* the save state's part (gl_state.c's copy holds it after the programs'): its bytes, a copy, the check (why names
   the field and its value) and its load. The buffers' bytes are not in it: they are in the platform's memory, which
   the console's save state holds */
size_t snx_gl_object_state_bytes(void);
void snx_gl_object_state_save(void* out);
int snx_gl_object_state_check(const void* in, size_t size, char* why, size_t whysize);
void snx_gl_object_state_load(const void* in);

#endif
