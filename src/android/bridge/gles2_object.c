/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 2 and 3's objects (gles2.h) as Vulkan's.

   A buffer keeps the guest's bytes in memory of its own (data), which the CPU writes and reads. A draw reads its
   Vulkan copy, which the bytes written since go into first: in place when no submission in flight reads it, else as
   a copy in the frame's arena. Only a buffer the GPU writes (glReadPixels into a pack buffer) moves to host-cached
   memory, which the CPU then reads back.

   A texture is one image with the whole chain of levels of its base level, made when its base level gets a size and
   a format, and made again when they change. It rests in SHADER_READ_ONLY_OPTIMAL (DEPTH_STENCIL_READ_ONLY for a
   depth format), so a draw samples it without a barrier: an upload ends the frame's rendering, copies from the
   frame's staging buffer and returns the image to its rest. A format the device does not have with 3 components
   (GL_RGB8, GL_RGB16F, GL_RGB32F) is the 4-component one, which gles_expand.comp writes from the client's packed
   texels on the GPU. A renderbuffer is an image in its attachment layout */
#include "android/bridge/gles2.h"
#include "android/bridge/gles_expand_comp_spv.h"
#include "android/bridge/hle_impl.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define V g_gles_vk

enum {
    GL_INVALID_ENUM_ = 0x0500,
    GL_INVALID_VALUE_ = 0x0501,
    GL_INVALID_OPERATION_ = 0x0502,
    GL_OUT_OF_MEMORY_ = 0x0505,
    GL_INVALID_FRAMEBUFFER_OPERATION_ = 0x0506,
    GL_BYTE_ = 0x1400,
    GL_UNSIGNED_BYTE_ = 0x1401,
    GL_SHORT_ = 0x1402,
    GL_UNSIGNED_SHORT_ = 0x1403,
    GL_INT_ = 0x1404,
    GL_UNSIGNED_INT_ = 0x1405,
    GL_FLOAT_ = 0x1406,
    GL_HALF_FLOAT_ = 0x140b,
    GL_HALF_FLOAT_OES_ = 0x8d61,
    GL_FIXED_ = 0x140c,
    GL_DEPTH_COMPONENT_ = 0x1902,
    GL_RED_ = 0x1903,
    GL_ALPHA_ = 0x1906,
    GL_RGB_ = 0x1907,
    GL_RGBA_ = 0x1908,
    GL_LUMINANCE_ = 0x1909,
    GL_LUMINANCE_ALPHA_ = 0x190a,
    GL_RG_ = 0x8227,
    GL_RG_INTEGER_ = 0x8228,
    GL_RED_INTEGER_ = 0x8d94,
    GL_RGB_INTEGER_ = 0x8d98,
    GL_RGBA_INTEGER_ = 0x8d99,
    GL_DEPTH_STENCIL_ = 0x84f9,
    GL_BGRA_EXT_ = 0x80e1,
    GL_US_4444_ = 0x8033,
    GL_US_5551_ = 0x8034,
    GL_US_565_ = 0x8363,
    GL_UI_2101010_ = 0x8368,
    GL_UI_10F11F11F_ = 0x8c3b,
    GL_UI_5999_ = 0x8c3e,
    GL_UI_24_8_ = 0x84fa,
    GL_F32_UI_24_8_ = 0x8dad,
    GL_TEXTURE_2D_ = 0x0de1,
    GL_TEXTURE_3D_ = 0x806f,
    GL_TEXTURE_2D_ARRAY_ = 0x8c1a,
    GL_TEXTURE_CUBE_MAP_ = 0x8513,
    GL_TEXTURE_CUBE_MAP_POSITIVE_X_ = 0x8515,
    GL_TEXTURE_2D_MULTISAMPLE_ = 0x9100,
    GL_TEXTURE_MAG_FILTER_ = 0x2800,
    GL_TEXTURE_MIN_FILTER_ = 0x2801,
    GL_TEXTURE_WRAP_S_ = 0x2802,
    GL_TEXTURE_WRAP_T_ = 0x2803,
    GL_TEXTURE_WRAP_R_ = 0x8072,
    GL_TEXTURE_MIN_LOD_ = 0x813a,
    GL_TEXTURE_MAX_LOD_ = 0x813b,
    GL_TEXTURE_BASE_LEVEL_ = 0x813c,
    GL_TEXTURE_MAX_LEVEL_ = 0x813d,
    GL_TEXTURE_COMPARE_MODE_ = 0x884c,
    GL_TEXTURE_COMPARE_FUNC_ = 0x884d,
    GL_TEXTURE_SWIZZLE_R_ = 0x8e42,
    GL_TEXTURE_IMMUTABLE_FORMAT_ = 0x912f,
    GL_TEXTURE_IMMUTABLE_LEVELS_ = 0x82df,
    GL_TEXTURE_MAX_ANISOTROPY_ = 0x84fe,
    GL_NEAREST_ = 0x2600,
    GL_LINEAR_ = 0x2601,
    GL_NEAREST_MIPMAP_NEAREST_ = 0x2700,
    GL_LINEAR_MIPMAP_NEAREST_ = 0x2701,
    GL_NEAREST_MIPMAP_LINEAR_ = 0x2702,
    GL_LINEAR_MIPMAP_LINEAR_ = 0x2703,
    GL_REPEAT_ = 0x2901,
    GL_CLAMP_TO_EDGE_ = 0x812f,
    GL_MIRRORED_REPEAT_ = 0x8370,
    GL_CLAMP_TO_BORDER_ = 0x812d,
    GL_COMPARE_REF_TO_TEXTURE_ = 0x884e,
    GL_RED_COMP_ = 0x1903,
    GL_GREEN_ = 0x1904,
    GL_BLUE_ = 0x1905,
    GL_ZERO_ = 0,
    GL_ONE_ = 1,
    GL_ARRAY_BUFFER_ = 0x8892,
    GL_ELEMENT_ARRAY_BUFFER_ = 0x8893,
    GL_PIXEL_PACK_BUFFER_ = 0x88eb,
    GL_PIXEL_UNPACK_BUFFER_ = 0x88ec,
    GL_COPY_READ_BUFFER_ = 0x8f36,
    GL_COPY_WRITE_BUFFER_ = 0x8f37,
    GL_UNIFORM_BUFFER_ = 0x8a11,
    GL_TRANSFORM_FEEDBACK_BUFFER_ = 0x8c8e,
    GL_BUFFER_SIZE_ = 0x8764,
    GL_BUFFER_USAGE_ = 0x8765,
    GL_BUFFER_MAPPED_ = 0x88bc,
    GL_BUFFER_ACCESS_FLAGS_ = 0x911f,
    GL_BUFFER_MAP_LENGTH_ = 0x9120,
    GL_BUFFER_MAP_OFFSET_ = 0x9121,
    GL_MAP_READ_BIT_ = 0x0001,
    GL_MAP_WRITE_BIT_ = 0x0002,
    GL_MAP_INVALIDATE_BUFFER_BIT_ = 0x0008,
    GL_MAP_FLUSH_EXPLICIT_BIT_ = 0x0010,
    GL_FRAMEBUFFER_ = 0x8d40,
    GL_READ_FRAMEBUFFER_ = 0x8ca8,
    GL_DRAW_FRAMEBUFFER_ = 0x8ca9,
    GL_RENDERBUFFER_ = 0x8d41,
    GL_COLOR_ATTACHMENT0_ = 0x8ce0,
    GL_DEPTH_ATTACHMENT_ = 0x8d00,
    GL_STENCIL_ATTACHMENT_ = 0x8d20,
    GL_DEPTH_STENCIL_ATTACHMENT_ = 0x821a,
    GL_FRAMEBUFFER_COMPLETE_ = 0x8cd5,
    GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_ = 0x8cd6,
    GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_ = 0x8cd7,
    GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_ = 0x8cd9,
    GL_FRAMEBUFFER_UNSUPPORTED_ = 0x8cdd,
    GL_NONE_ = 0,
    GL_BACK_ = 0x0405,
    GL_ANY_SAMPLES_PASSED_ = 0x8c2f,
    GL_ANY_SAMPLES_PASSED_CONSERVATIVE_ = 0x8d6a,
    GL_TF_PRIMITIVES_WRITTEN_ = 0x8c88,
};

/* ---- the name tables ---- */

/* the entry of name in an array of entries of size bytes that grows: NULL without memory or for name 0 */
static void* entry(void** items, size_t* count, size_t size, unsigned name) {
    if (!name) return NULL;
    if (name >= *count) {
        size_t n = (size_t)name + 64;
        unsigned char* grown = realloc(*items, n * size);
        if (!grown) return NULL;
        memset(grown + *count * size, 0, (n - *count) * size);
        *items = grown;
        *count = n;
    }
    return (unsigned char*)*items + (size_t)name * size;
}
/* n unused names written to the guest's out, each made used by make */
#define GEN(kind, count_field, array_field, type, init)                                                                \
    static void gen_##kind(Gles2Context* gl, int n, unsigned char* out) {                                              \
        if (n < 0) {                                                                                                   \
            gles2_error(gl, GL_INVALID_VALUE_);                                                                        \
            return;                                                                                                    \
        }                                                                                                              \
        unsigned name = 1;                                                                                             \
        for (int i = 0; i < n; ++i) {                                                                                  \
            while (name < g_gles2.count_field && g_gles2.array_field[name].used) ++name;                                \
            type* x = entry((void**)&g_gles2.array_field, &g_gles2.count_field, sizeof(type), name);                  \
            uint32_t v = 0;                                                                                            \
            if (x) {                                                                                                   \
                *x = (type){0};                                                                                        \
                x->used = true;                                                                                        \
                init;                                                                                                  \
                v = name++;                                                                                            \
            }                                                                                                          \
            memcpy(out + 4 * (size_t)i, &v, 4);                                                                        \
        }                                                                                                              \
    }

static void texture_init(G2Texture* t) {
    t->target = 0xff;
    t->sampler = (G2SamplerState){.min = GL_NEAREST_MIPMAP_LINEAR_,
                                  .mag = GL_LINEAR_,
                                  .wrap = {GL_REPEAT_, GL_REPEAT_, GL_REPEAT_},
                                  .compare_func = 0x0203, /* GL_LEQUAL */
                                  .min_lod = -1000,
                                  .max_lod = 1000,
                                  .anisotropy = 1};
    t->max_level = 1000;
    t->swizzles[0] = GL_RED_COMP_, t->swizzles[1] = GL_GREEN_, t->swizzles[2] = GL_BLUE_, t->swizzles[3] = GL_ALPHA_;
}
static void sampler_init(G2Sampler* s) {
    G2Texture t;
    texture_init(&t);
    s->state = t.sampler;
}
static void vao_init(G2VertexArray* a) {
    for (int i = 0; i < G2_ATTRIBS; ++i) a->attribs[i].size = 4, a->attribs[i].type = GL_FLOAT_;
}
GEN(buffers, buffer_count, buffers, G2Buffer, (void)0)
GEN(textures, texture_count, textures, G2Texture, texture_init(x))
GEN(samplers, sampler_count, samplers, G2Sampler, sampler_init(x))
GEN(renderbuffers, renderbuffer_count, renderbuffers, G2Renderbuffer, (void)0)
GEN(framebuffers, framebuffer_count, framebuffers, G2Framebuffer, x->draw[0] = GL_COLOR_ATTACHMENT0_;
    x->read = GL_COLOR_ATTACHMENT0_)
GEN(vertex_arrays, vertex_array_count, vertex_arrays, G2VertexArray, vao_init(x))
GEN(queries, query_count, queries, G2Query, (void)0)
#undef GEN

G2Buffer* gles2_buffer(unsigned name) {
    return name && name < g_gles2.buffer_count && g_gles2.buffers[name].used ? &g_gles2.buffers[name] : NULL;
}
G2Texture* gles2_texture(unsigned name) {
    return name && name < g_gles2.texture_count && g_gles2.textures[name].used ? &g_gles2.textures[name] : NULL;
}
G2Renderbuffer* gles2_renderbuffer(unsigned name) {
    return name && name < g_gles2.renderbuffer_count && g_gles2.renderbuffers[name].used ? &g_gles2.renderbuffers[name]
                                                                                         : NULL;
}
G2Framebuffer* gles2_framebuffer(unsigned name) {
    return name && name < g_gles2.framebuffer_count && g_gles2.framebuffers[name].used ? &g_gles2.framebuffers[name]
                                                                                       : NULL;
}
static G2Sampler* sampler_object(unsigned name) {
    return name && name < g_gles2.sampler_count && g_gles2.samplers[name].used ? &g_gles2.samplers[name] : NULL;
}
static G2Query* query_object(unsigned name) {
    return name && name < g_gles2.query_count && g_gles2.queries[name].used ? &g_gles2.queries[name] : NULL;
}
G2VertexArray* gles2_vertex_array(Gles2Context* gl) {
    if (!gl->vertex_array) return &gl->default_vao;
    G2VertexArray* a = gl->vertex_array < g_gles2.vertex_array_count ? &g_gles2.vertex_arrays[gl->vertex_array] : NULL;
    return a && a->used ? a : &gl->default_vao;
}

GlesSlot* gles2_transfer_slot(Gles2Context* gl) {
    GlesSurface* s = gles2_surface(gl);
    if (!s || !gles_frame_begin(s)) return NULL;
    gles_pass_end(s);
    return &s->slots[s->slot];
}

static void gen_call(CPU* c, Gles2Context* gl, void (*gen)(Gles2Context*, int, unsigned char*)) {
    gen(gl, (int)X(0), PTR(1));
}
/* the names of a delete call, each given to forget */
static void each_name(CPU* c, Gles2Context* gl, void (*forget)(Gles2Context*, unsigned)) {
    int n = (int)X(0);
    const unsigned char* names = PTR(1);
    if (n < 0) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    for (int i = 0; i < n; ++i) {
        uint32_t name;
        memcpy(&name, names + 4 * (size_t)i, 4);
        if (name) forget(gl, name);
    }
}

/* ---- buffers ---- */

static unsigned* buffer_binding(Gles2Context* gl, unsigned target) {
    switch (target) {
    case GL_ARRAY_BUFFER_: return &gl->array_buffer;
    case GL_ELEMENT_ARRAY_BUFFER_: return &gles2_vertex_array(gl)->elements;
    case GL_PIXEL_PACK_BUFFER_: return &gl->pixel_pack;
    case GL_PIXEL_UNPACK_BUFFER_: return &gl->pixel_unpack;
    case GL_COPY_READ_BUFFER_: return &gl->copy_read;
    case GL_COPY_WRITE_BUFFER_: return &gl->copy_write;
    case GL_UNIFORM_BUFFER_: return &gl->uniform_buffer;
    case GL_TRANSFORM_FEEDBACK_BUFFER_: return &gl->tf_buffer;
    default: return NULL;
    }
}
static G2Buffer* bound_buffer(Gles2Context* gl, unsigned target) {
    unsigned* b = buffer_binding(gl, target);
    if (!b) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return NULL;
    }
    G2Buffer* x = gles2_buffer(*b);
    if (!x) gles2_error(gl, GL_INVALID_OPERATION_);
    return x;
}
/* the buffer of name, made when a bind names it first (GLES 2 lets a bind make it) */
static G2Buffer* buffer_made(unsigned name) {
    G2Buffer* b = gles2_buffer(name);
    if (b || !name) return b;
    b = entry((void**)&g_gles2.buffers, &g_gles2.buffer_count, sizeof *b, name);
    if (b) {
        *b = (G2Buffer){0};
        b->used = true;
    }
    return b;
}

static void buffer_vk_free(G2Buffer* b) {
    if (b->buffer) gles_vk_retire((GlesGarbage){.buffer = b->buffer, .memory = b->memory});
    b->buffer = VK_NULL_HANDLE;
    b->memory = VK_NULL_HANDLE;
    b->map = NULL;
    b->capacity = 0;
}

static void g_bind_buffer(CPU* c, Gles2Context* gl) {
    unsigned* at = buffer_binding(gl, (unsigned)X(0));
    if (!at) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    unsigned name = (unsigned)X(1);
    if (name && !buffer_made(name)) {
        gles2_error(gl, GL_OUT_OF_MEMORY_);
        return;
    }
    *at = name;
}
static void bind_indexed(Gles2Context* gl, unsigned target, unsigned index, unsigned name, int64_t offset,
                         int64_t size) {
    if (target != GL_UNIFORM_BUFFER_ && target != GL_TRANSFORM_FEEDBACK_BUFFER_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    if (name && !buffer_made(name)) return;
    if (target == GL_TRANSFORM_FEEDBACK_BUFFER_) {
        gl->tf_buffer = name;
        return;
    }
    if (index >= G2_UBO_BINDINGS) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    gl->uniform_buffer = name;
    gl->uniform_buffers[index] = (G2IndexedBuffer){name, offset, size};
}
static void g_bind_buffer_base(CPU* c, Gles2Context* gl) {
    bind_indexed(gl, (unsigned)X(0), (unsigned)X(1), (unsigned)X(2), 0, -1);
}
static void g_bind_buffer_range(CPU* c, Gles2Context* gl) {
    bind_indexed(gl, (unsigned)X(0), (unsigned)X(1), (unsigned)X(2), (int64_t)X(3), (int64_t)X(4));
}
static void g_gen_buffers(CPU* c, Gles2Context* gl) {
    gen_call(c, gl, gen_buffers);
}
static void forget_buffer(Gles2Context* gl, unsigned name) {
    G2Buffer* b = gles2_buffer(name);
    if (!b) return;
    buffer_vk_free(b);
    free(b->data);
    *b = (G2Buffer){0};
    unsigned* bindings[] = {&gl->array_buffer, &gl->copy_read, &gl->copy_write, &gl->pixel_pack,
                            &gl->pixel_unpack, &gl->uniform_buffer, &gl->tf_buffer,
                            &gles2_vertex_array(gl)->elements};
    for (size_t i = 0; i < sizeof bindings / sizeof *bindings; ++i)
        if (*bindings[i] == name) *bindings[i] = 0;
    for (int i = 0; i < G2_UBO_BINDINGS; ++i)
        if (gl->uniform_buffers[i].buffer == name) gl->uniform_buffers[i] = (G2IndexedBuffer){0};
    G2VertexArray* a = gles2_vertex_array(gl);
    for (int i = 0; i < G2_ATTRIBS; ++i)
        if (a->attribs[i].buffer == name) a->attribs[i].buffer = 0;
}
static void g_delete_buffers(CPU* c, Gles2Context* gl) {
    each_name(c, gl, forget_buffer);
}
static void g_is_buffer(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = gles2_buffer((unsigned)X(0)) != NULL;
}

/* data[lo, hi) newer than the Vulkan copy */
static void buffer_dirty(G2Buffer* b, size_t lo, size_t hi) {
    if (b->stale) return;
    if (b->lo == b->hi) b->lo = lo, b->hi = hi;
    else {
        if (lo < b->lo) b->lo = lo;
        if (hi > b->hi) b->hi = hi;
    }
}

/* glBufferData(target, size, data, usage) */
static void g_buffer_data(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    int64_t size = (int64_t)X(1);
    if (!b) return;
    if (size < 0) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    if ((size_t)size != b->size) {
        unsigned char* grown = realloc(b->data, size ? (size_t)size : 1);
        if (!grown) {
            gles2_error(gl, GL_OUT_OF_MEMORY_);
            return;
        }
        b->data = grown;
        b->size = (size_t)size;
    }
    if (X(2)) memcpy(b->data, PTR(2), (size_t)size);
    else memset(b->data, 0, (size_t)size);
    b->usage = (unsigned)X(3);
    b->stale = true;
    b->lo = b->hi = 0;
    b->gpu_newer = false;
    b->arena_serial = 0;
}
/* the bytes the GPU wrote into the Vulkan copy, brought into data */
static void buffer_read_back(G2Buffer* b) {
    if (!b->gpu_newer) return;
    gles_vk_wait(b->serial);
    if (b->map) {
        VkMappedMemoryRange range = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = b->memory,
                                     .size = VK_WHOLE_SIZE};
        V.vk.vkInvalidateMappedMemoryRanges(V.device, 1, &range);
        memcpy(b->data, b->map, b->size);
    }
    b->gpu_newer = false;
    b->stale = false;
    b->lo = b->hi = 0;
}
const unsigned char* gles2_buffer_bytes(G2Buffer* b) {
    buffer_read_back(b);
    return b->data;
}
static void g_buffer_sub_data(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    int64_t offset = (int64_t)X(1), size = (int64_t)X(2);
    if (!b) return;
    if (offset < 0 || size < 0 || (uint64_t)(offset + size) > b->size) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    buffer_read_back(b);
    if (size) memcpy(b->data + offset, PTR(3), (size_t)size);
    buffer_dirty(b, (size_t)offset, (size_t)(offset + size));
    b->arena_serial = 0;
}

/* the Vulkan copy of size bytes: host-visible, host-cached when cached, device-local where it can be too */
static bool buffer_vk_make(G2Buffer* b, size_t size, bool cached) {
    buffer_vk_free(b);
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size ? size : 4,
                               .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT};
    if (V.vk.vkCreateBuffer(V.device, &info, NULL, &b->buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    V.vk.vkGetBufferMemoryRequirements(V.device, b->buffer, &req);
    VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = cached ? gles_vk_memory_type(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                           : gles_vk_memory_type(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = gles_vk_memory_type(req.memoryTypeBits, host);
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                  .allocationSize = req.size,
                                  .memoryTypeIndex = type};
    void* map = NULL;
    if (type == UINT32_MAX || V.vk.vkAllocateMemory(V.device, &alloc, NULL, &b->memory) != VK_SUCCESS ||
        V.vk.vkBindBufferMemory(V.device, b->buffer, b->memory, 0) != VK_SUCCESS ||
        V.vk.vkMapMemory(V.device, b->memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        gles_vk_destroy((GlesGarbage){.buffer = b->buffer, .memory = b->memory});
        b->buffer = VK_NULL_HANDLE;
        b->memory = VK_NULL_HANDLE;
        return false;
    }
    b->map = map;
    b->capacity = size;
    b->cached = cached;
    b->stale = true;
    return true;
}

bool gles2_buffer_ref(GlesSlot* slot, G2Buffer* b, bool writes, G2Ref* out) {
    uint64_t pending = gles_vk_pending();
    if (writes) {
        /* the GPU writes the Vulkan copy, which the CPU reads back: host-cached, and with data in it */
        buffer_read_back(b);
        if ((!b->buffer || !b->cached || b->capacity < b->size) && !buffer_vk_make(b, b->size, true)) return false;
    }
    bool newer = b->stale || b->lo != b->hi;
    if (!writes && b->arena_serial == pending && newer) {
        *out = b->arena; /* this frame's copy is the newest */
        return true;
    }
    if (!b->buffer || b->capacity < b->size) {
        if (!buffer_vk_make(b, b->size, false)) return false;
        newer = true;
    }
    if (newer) {
        bool busy = b->serial > V.completed;
        if (busy && !writes) {
            /* the GPU may read the Vulkan copy still: this frame reads a copy in its arena */
            VkDeviceSize at = gles_vk_arena_take(slot, &slot->vertices, b->size ? b->size : 4, 256, 4u << 20, 0);
            if (at == VK_WHOLE_SIZE) return false;
            memcpy(slot->vertices.map + at, b->data, b->size);
            b->arena = (G2Ref){slot->vertices.buffer, at};
            b->arena_serial = pending;
            *out = b->arena;
            return true;
        }
        if (busy) gles_vk_wait(b->serial);
        if (b->stale) memcpy(b->map, b->data, b->size);
        else memcpy(b->map + b->lo, b->data + b->lo, b->hi - b->lo);
        b->stale = false;
        b->lo = b->hi = 0;
    }
    b->serial = pending;
    if (writes) b->gpu_newer = true;
    *out = (G2Ref){b->buffer, 0};
    return true;
}

/* glMapBufferRange(target, offset, length, access): data itself, which the guest's pointers reach */
static void g_map_buffer_range(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    int64_t offset = (int64_t)X(1), length = (int64_t)X(2);
    unsigned access = (unsigned)X(3);
    X(0) = 0;
    if (!b) return;
    if (offset < 0 || length < 0 || (uint64_t)(offset + length) > b->size || b->mapped ||
        !(access & (GL_MAP_READ_BIT_ | GL_MAP_WRITE_BIT_))) {
        gles2_error(gl, b->mapped ? GL_INVALID_OPERATION_ : GL_INVALID_VALUE_);
        return;
    }
    buffer_read_back(b);
    b->mapped = true;
    b->map_offset = (size_t)offset;
    b->map_length = (size_t)length;
    b->map_access = access;
    X(0) = (uint64_t)(uintptr_t)(b->data + offset);
}
/* glMapBufferOES(target, access): the whole buffer */
static void g_map_buffer(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    X(0) = 0;
    if (!b || b->mapped) return;
    buffer_read_back(b);
    b->mapped = true;
    b->map_offset = 0;
    b->map_length = b->size;
    b->map_access = GL_MAP_WRITE_BIT_;
    X(0) = (uint64_t)(uintptr_t)b->data;
}
static void g_unmap_buffer(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    X(0) = 0;
    if (!b || !b->mapped) return;
    if ((b->map_access & GL_MAP_WRITE_BIT_) && !(b->map_access & GL_MAP_FLUSH_EXPLICIT_BIT_)) {
        buffer_dirty(b, b->map_offset, b->map_offset + b->map_length);
        b->arena_serial = 0;
    }
    b->mapped = false;
    X(0) = 1;
}
static void g_flush_mapped_range(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    int64_t offset = (int64_t)X(1), length = (int64_t)X(2);
    if (!b || !b->mapped || offset < 0 || length < 0 || (uint64_t)(offset + length) > b->map_length) return;
    buffer_dirty(b, b->map_offset + (size_t)offset, b->map_offset + (size_t)(offset + length));
    b->arena_serial = 0;
}
static void g_get_buffer_pointerv(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    uint64_t p = b && b->mapped ? (uint64_t)(uintptr_t)(b->data + b->map_offset) : 0;
    memcpy(PTR(2), &p, 8);
}
/* glCopyBufferSubData(read, write, read offset, write offset, size): the bytes the CPU has */
static void g_copy_buffer_sub_data(CPU* c, Gles2Context* gl) {
    G2Buffer* r = bound_buffer(gl, (unsigned)X(0));
    G2Buffer* w = bound_buffer(gl, (unsigned)X(1));
    int64_t ro = (int64_t)X(2), wo = (int64_t)X(3), size = (int64_t)X(4);
    if (!r || !w) return;
    if (ro < 0 || wo < 0 || size < 0 || (uint64_t)(ro + size) > r->size || (uint64_t)(wo + size) > w->size) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    buffer_read_back(r);
    buffer_read_back(w);
    memmove(w->data + wo, r->data + ro, (size_t)size);
    buffer_dirty(w, (size_t)wo, (size_t)(wo + size));
    w->arena_serial = 0;
}
static bool buffer_parameter(G2Buffer* b, unsigned pname, int64_t* v) {
    switch (pname) {
    case GL_BUFFER_SIZE_: *v = (int64_t)b->size; return true;
    case GL_BUFFER_USAGE_: *v = b->usage ? b->usage : 0x88e4; return true;
    case GL_BUFFER_MAPPED_: *v = b->mapped; return true;
    case GL_BUFFER_ACCESS_FLAGS_: *v = b->mapped ? b->map_access : 0; return true;
    case GL_BUFFER_MAP_LENGTH_: *v = b->mapped ? (int64_t)b->map_length : 0; return true;
    case GL_BUFFER_MAP_OFFSET_: *v = b->mapped ? (int64_t)b->map_offset : 0; return true;
    default: return false;
    }
}
static void g_get_buffer_parameteriv(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    int64_t v = 0;
    if (!b) return;
    if (!buffer_parameter(b, (unsigned)X(1), &v)) gles2_error(gl, GL_INVALID_ENUM_);
    int32_t x = (int32_t)v;
    memcpy(PTR(2), &x, 4);
}
static void g_get_buffer_parameteri64v(CPU* c, Gles2Context* gl) {
    G2Buffer* b = bound_buffer(gl, (unsigned)X(0));
    int64_t v = 0;
    if (!b) return;
    if (!buffer_parameter(b, (unsigned)X(1), &v)) gles2_error(gl, GL_INVALID_ENUM_);
    memcpy(PTR(2), &v, 8);
}

/* ---- formats ---- */

#define SW(r, g, b, a)                                                                                                 \
    { VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, VK_COMPONENT_SWIZZLE_##b, VK_COMPONENT_SWIZZLE_##a }
#define ID SW(IDENTITY, IDENTITY, IDENTITY, IDENTITY)
#define BGRS SW(B, G, R, IDENTITY)
/* internal format, client format and type, Vulkan formats, swizzles, client bytes, expand, kind, block */
static const G2Format formats[] = {
    /* GLES 2's unsized formats, as their format and type */
    {GL_RGBA_, GL_RGBA_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8B8A8_UNORM, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, SW(IDENTITY, IDENTITY, IDENTITY, ONE), SW(IDENTITY, IDENTITY, IDENTITY, ONE), 3, 1, G2_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_US_4444_, VK_FORMAT_R4G4B4A4_UNORM_PACK16, VK_FORMAT_B4G4R4A4_UNORM_PACK16, ID, BGRS, 2, 0, G2_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_US_5551_, VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_FORMAT_B5G5R5A1_UNORM_PACK16, ID, BGRS, 2, 0, G2_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_US_565_, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_B5G6R5_UNORM_PACK16, SW(IDENTITY, IDENTITY, IDENTITY, ONE), SW(B, G, R, ONE), 2, 0, G2_FLOAT, 0},
    {GL_LUMINANCE_, GL_LUMINANCE_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8_UNORM, 0, SW(R, R, R, ONE), ID, 1, 0, G2_FLOAT, 0},
    {GL_ALPHA_, GL_ALPHA_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8_UNORM, 0, SW(ZERO, ZERO, ZERO, R), ID, 1, 0, G2_FLOAT, 0},
    {GL_LUMINANCE_ALPHA_, GL_LUMINANCE_ALPHA_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8_UNORM, 0, SW(R, R, R, G), ID, 2, 0, G2_FLOAT, 0},
    {GL_BGRA_EXT_, GL_BGRA_EXT_, GL_UNSIGNED_BYTE_, VK_FORMAT_B8G8R8A8_UNORM, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_HALF_FLOAT_OES_, VK_FORMAT_R16G16B16A16_SFLOAT, 0, ID, ID, 8, 0, G2_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_HALF_FLOAT_, VK_FORMAT_R16G16B16A16_SFLOAT, 0, ID, ID, 8, 0, G2_FLOAT, 0},
    {GL_RGBA_, GL_RGBA_, GL_FLOAT_, VK_FORMAT_R32G32B32A32_SFLOAT, 0, ID, ID, 16, 0, G2_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_HALF_FLOAT_OES_, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, SW(IDENTITY, IDENTITY, IDENTITY, ONE), SW(IDENTITY, IDENTITY, IDENTITY, ONE), 6, 2, G2_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_HALF_FLOAT_, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, SW(IDENTITY, IDENTITY, IDENTITY, ONE), SW(IDENTITY, IDENTITY, IDENTITY, ONE), 6, 2, G2_FLOAT, 0},
    {GL_RGB_, GL_RGB_, GL_FLOAT_, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, SW(IDENTITY, IDENTITY, IDENTITY, ONE), SW(IDENTITY, IDENTITY, IDENTITY, ONE), 12, 4, G2_FLOAT, 0},
    {GL_LUMINANCE_, GL_LUMINANCE_, GL_HALF_FLOAT_OES_, VK_FORMAT_R16_SFLOAT, 0, SW(R, R, R, ONE), ID, 2, 0, G2_FLOAT, 0},
    {GL_LUMINANCE_, GL_LUMINANCE_, GL_FLOAT_, VK_FORMAT_R32_SFLOAT, 0, SW(R, R, R, ONE), ID, 4, 0, G2_FLOAT, 0},
    {GL_ALPHA_, GL_ALPHA_, GL_HALF_FLOAT_OES_, VK_FORMAT_R16_SFLOAT, 0, SW(ZERO, ZERO, ZERO, R), ID, 2, 0, G2_FLOAT, 0},
    {GL_ALPHA_, GL_ALPHA_, GL_FLOAT_, VK_FORMAT_R32_SFLOAT, 0, SW(ZERO, ZERO, ZERO, R), ID, 4, 0, G2_FLOAT, 0},
    {GL_LUMINANCE_ALPHA_, GL_LUMINANCE_ALPHA_, GL_HALF_FLOAT_OES_, VK_FORMAT_R16G16_SFLOAT, 0, SW(R, R, R, G), ID, 4, 0, G2_FLOAT, 0},
    {GL_LUMINANCE_ALPHA_, GL_LUMINANCE_ALPHA_, GL_FLOAT_, VK_FORMAT_R32G32_SFLOAT, 0, SW(R, R, R, G), ID, 8, 0, G2_FLOAT, 0},
    {GL_RED_, GL_RED_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8_UNORM, 0, ID, ID, 1, 0, G2_FLOAT, 0},
    {GL_RG_, GL_RG_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8_UNORM, 0, ID, ID, 2, 0, G2_FLOAT, 0},
    {GL_DEPTH_COMPONENT_, GL_DEPTH_COMPONENT_, GL_UNSIGNED_SHORT_, VK_FORMAT_D16_UNORM, 0, ID, ID, 2, 0, G2_DEPTH, 0},
    {GL_DEPTH_COMPONENT_, GL_DEPTH_COMPONENT_, GL_UNSIGNED_INT_, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, ID, ID, 4, 0, G2_DEPTH, 0},
    {GL_DEPTH_STENCIL_, GL_DEPTH_STENCIL_, GL_UI_24_8_, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, ID, ID, 4, 0, G2_DS, 0},
    /* GLES 3's sized formats */
    {0x8229, GL_RED_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8_UNORM, 0, ID, ID, 1, 0, G2_FLOAT, 0},
    {0x8f94, GL_RED_, GL_BYTE_, VK_FORMAT_R8_SNORM, 0, ID, ID, 1, 0, G2_FLOAT, 0},
    {0x822d, GL_RED_, GL_HALF_FLOAT_, VK_FORMAT_R16_SFLOAT, 0, ID, ID, 2, 0, G2_FLOAT, 0},
    {0x822e, GL_RED_, GL_FLOAT_, VK_FORMAT_R32_SFLOAT, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x8232, GL_RED_INTEGER_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8_UINT, 0, ID, ID, 1, 0, G2_UINT, 0},
    {0x8231, GL_RED_INTEGER_, GL_BYTE_, VK_FORMAT_R8_SINT, 0, ID, ID, 1, 0, G2_INT, 0},
    {0x8234, GL_RED_INTEGER_, GL_UNSIGNED_SHORT_, VK_FORMAT_R16_UINT, 0, ID, ID, 2, 0, G2_UINT, 0},
    {0x8233, GL_RED_INTEGER_, GL_SHORT_, VK_FORMAT_R16_SINT, 0, ID, ID, 2, 0, G2_INT, 0},
    {0x8236, GL_RED_INTEGER_, GL_UNSIGNED_INT_, VK_FORMAT_R32_UINT, 0, ID, ID, 4, 0, G2_UINT, 0},
    {0x8235, GL_RED_INTEGER_, GL_INT_, VK_FORMAT_R32_SINT, 0, ID, ID, 4, 0, G2_INT, 0},
    {0x822b, GL_RG_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8_UNORM, 0, ID, ID, 2, 0, G2_FLOAT, 0},
    {0x8f95, GL_RG_, GL_BYTE_, VK_FORMAT_R8G8_SNORM, 0, ID, ID, 2, 0, G2_FLOAT, 0},
    {0x822f, GL_RG_, GL_HALF_FLOAT_, VK_FORMAT_R16G16_SFLOAT, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x8230, GL_RG_, GL_FLOAT_, VK_FORMAT_R32G32_SFLOAT, 0, ID, ID, 8, 0, G2_FLOAT, 0},
    {0x8238, GL_RG_INTEGER_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8_UINT, 0, ID, ID, 2, 0, G2_UINT, 0},
    {0x8237, GL_RG_INTEGER_, GL_BYTE_, VK_FORMAT_R8G8_SINT, 0, ID, ID, 2, 0, G2_INT, 0},
    {0x823a, GL_RG_INTEGER_, GL_UNSIGNED_SHORT_, VK_FORMAT_R16G16_UINT, 0, ID, ID, 4, 0, G2_UINT, 0},
    {0x8239, GL_RG_INTEGER_, GL_SHORT_, VK_FORMAT_R16G16_SINT, 0, ID, ID, 4, 0, G2_INT, 0},
    {0x823c, GL_RG_INTEGER_, GL_UNSIGNED_INT_, VK_FORMAT_R32G32_UINT, 0, ID, ID, 8, 0, G2_UINT, 0},
    {0x823b, GL_RG_INTEGER_, GL_INT_, VK_FORMAT_R32G32_SINT, 0, ID, ID, 8, 0, G2_INT, 0},
    {0x8051, GL_RGB_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, ID, ID, 3, 1, G2_FLOAT, 0},
    {0x8c41, GL_RGB_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8B8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, ID, ID, 3, 1, G2_FLOAT, 0},
    {0x8d62, GL_RGB_, GL_US_565_, VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_B5G6R5_UNORM_PACK16, ID, SW(B, G, R, ONE), 2, 0, G2_FLOAT, 0},
    {0x8f96, GL_RGB_, GL_BYTE_, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, ID, ID, 3, 1, G2_FLOAT, 0},
    {0x8c3a, GL_RGB_, GL_UI_10F11F11F_, VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x8c3d, GL_RGB_, GL_UI_5999_, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x881b, GL_RGB_, GL_HALF_FLOAT_, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, ID, ID, 6, 2, G2_FLOAT, 0},
    {0x8815, GL_RGB_, GL_FLOAT_, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, ID, ID, 12, 4, G2_FLOAT, 0},
    {0x8d7d, GL_RGB_INTEGER_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT, ID, ID, 3, 1, G2_UINT, 0},
    {0x8d8f, GL_RGB_INTEGER_, GL_BYTE_, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT, ID, ID, 3, 1, G2_INT, 0},
    {0x8d77, GL_RGB_INTEGER_, GL_UNSIGNED_SHORT_, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT, ID, ID, 6, 2, G2_UINT, 0},
    {0x8d89, GL_RGB_INTEGER_, GL_SHORT_, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT, ID, ID, 6, 2, G2_INT, 0},
    {0x8d71, GL_RGB_INTEGER_, GL_UNSIGNED_INT_, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT, ID, ID, 12, 4, G2_UINT, 0},
    {0x8d83, GL_RGB_INTEGER_, GL_INT_, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT, ID, ID, 12, 4, G2_INT, 0},
    {0x8058, GL_RGBA_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8B8A8_UNORM, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x8c43, GL_RGBA_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8B8A8_SRGB, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x8f97, GL_RGBA_, GL_BYTE_, VK_FORMAT_R8G8B8A8_SNORM, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x8057, GL_RGBA_, GL_US_5551_, VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_FORMAT_B5G5R5A1_UNORM_PACK16, ID, BGRS, 2, 0, G2_FLOAT, 0},
    {0x8056, GL_RGBA_, GL_US_4444_, VK_FORMAT_R4G4B4A4_UNORM_PACK16, VK_FORMAT_B4G4R4A4_UNORM_PACK16, ID, BGRS, 2, 0, G2_FLOAT, 0},
    {0x8059, GL_RGBA_, GL_UI_2101010_, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x881a, GL_RGBA_, GL_HALF_FLOAT_, VK_FORMAT_R16G16B16A16_SFLOAT, 0, ID, ID, 8, 0, G2_FLOAT, 0},
    {0x8814, GL_RGBA_, GL_FLOAT_, VK_FORMAT_R32G32B32A32_SFLOAT, 0, ID, ID, 16, 0, G2_FLOAT, 0},
    {0x8d7c, GL_RGBA_INTEGER_, GL_UNSIGNED_BYTE_, VK_FORMAT_R8G8B8A8_UINT, 0, ID, ID, 4, 0, G2_UINT, 0},
    {0x8d8e, GL_RGBA_INTEGER_, GL_BYTE_, VK_FORMAT_R8G8B8A8_SINT, 0, ID, ID, 4, 0, G2_INT, 0},
    {0x906f, GL_RGBA_INTEGER_, GL_UI_2101010_, VK_FORMAT_A2B10G10R10_UINT_PACK32, 0, ID, ID, 4, 0, G2_UINT, 0},
    {0x8d76, GL_RGBA_INTEGER_, GL_UNSIGNED_SHORT_, VK_FORMAT_R16G16B16A16_UINT, 0, ID, ID, 8, 0, G2_UINT, 0},
    {0x8d88, GL_RGBA_INTEGER_, GL_SHORT_, VK_FORMAT_R16G16B16A16_SINT, 0, ID, ID, 8, 0, G2_INT, 0},
    {0x8d70, GL_RGBA_INTEGER_, GL_UNSIGNED_INT_, VK_FORMAT_R32G32B32A32_UINT, 0, ID, ID, 16, 0, G2_UINT, 0},
    {0x8d82, GL_RGBA_INTEGER_, GL_INT_, VK_FORMAT_R32G32B32A32_SINT, 0, ID, ID, 16, 0, G2_INT, 0},
    {0x93a1, GL_BGRA_EXT_, GL_UNSIGNED_BYTE_, VK_FORMAT_B8G8R8A8_UNORM, 0, ID, ID, 4, 0, G2_FLOAT, 0},
    {0x81a5, GL_DEPTH_COMPONENT_, GL_UNSIGNED_SHORT_, VK_FORMAT_D16_UNORM, 0, ID, ID, 2, 0, G2_DEPTH, 0},
    {0x81a6, GL_DEPTH_COMPONENT_, GL_UNSIGNED_INT_, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, ID, ID, 4, 0, G2_DEPTH, 0},
    {0x8cac, GL_DEPTH_COMPONENT_, GL_FLOAT_, VK_FORMAT_D32_SFLOAT, 0, ID, ID, 4, 0, G2_DEPTH, 0},
    {0x88f0, GL_DEPTH_STENCIL_, GL_UI_24_8_, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, ID, ID, 4, 0, G2_DS, 0},
    {0x8cad, GL_DEPTH_STENCIL_, GL_F32_UI_24_8_, VK_FORMAT_D32_SFLOAT_S8_UINT, 0, ID, ID, 8, 0, G2_DS, 0},
    {0x8d48, 0x1901, GL_UNSIGNED_BYTE_, VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, ID, ID, 1, 0, G2_STENCIL, 0},
    /* compressed: the device's own formats, the bytes of a block, its width and height */
    {0x8d64, 0, 0, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 0, ID, ID, 8, 0, G2_FLOAT, 0x44},
    {0x9274, 0, 0, VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 0, ID, ID, 8, 0, G2_FLOAT, 0x44},
    {0x9275, 0, 0, VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, 0, ID, ID, 8, 0, G2_FLOAT, 0x44},
    {0x9276, 0, 0, VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, 0, ID, ID, 8, 0, G2_FLOAT, 0x44},
    {0x9277, 0, 0, VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, 0, ID, ID, 8, 0, G2_FLOAT, 0x44},
    {0x9278, 0, 0, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 0, ID, ID, 16, 0, G2_FLOAT, 0x44},
    {0x9279, 0, 0, VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, 0, ID, ID, 16, 0, G2_FLOAT, 0x44},
    {0x9270, 0, 0, VK_FORMAT_EAC_R11_UNORM_BLOCK, 0, ID, ID, 8, 0, G2_FLOAT, 0x44},
    {0x9271, 0, 0, VK_FORMAT_EAC_R11_SNORM_BLOCK, 0, ID, ID, 8, 0, G2_FLOAT, 0x44},
    {0x9272, 0, 0, VK_FORMAT_EAC_R11G11_UNORM_BLOCK, 0, ID, ID, 16, 0, G2_FLOAT, 0x44},
    {0x9273, 0, 0, VK_FORMAT_EAC_R11G11_SNORM_BLOCK, 0, ID, ID, 16, 0, G2_FLOAT, 0x44},
};
#undef SW
#undef ID
#undef BGRS
enum { FORMATS = sizeof formats / sizeof *formats };

/* ASTC's: GL's 0x93b0 (4x4) to 0x93bd (12x12) and their sRGB forms at 0x93d0, Vulkan's in the same order */
static const uint8_t astc_blocks[14] = {0x44, 0x54, 0x55, 0x65, 0x66, 0x85, 0x86, 0x88, 0xa5, 0xa6, 0xa8, 0xaa,
                                        0xca, 0xcc};

const G2Format* gles2_format(unsigned internal, unsigned format, unsigned type) {
    bool unsized = internal == GL_RGBA_ || internal == GL_RGB_ || internal == GL_LUMINANCE_ ||
                   internal == GL_ALPHA_ || internal == GL_LUMINANCE_ALPHA_ || internal == GL_BGRA_EXT_ ||
                   internal == GL_DEPTH_COMPONENT_ || internal == GL_DEPTH_STENCIL_ || internal == GL_RED_ ||
                   internal == GL_RG_;
    for (int i = 0; i < FORMATS; ++i) {
        const G2Format* f = &formats[i];
        if (f->internal != internal) continue;
        if (!unsized || (f->format == format && f->type == type)) return f;
    }
    if ((internal >= 0x93b0 && internal <= 0x93bd) || (internal >= 0x93d0 && internal <= 0x93dd)) {
        unsigned k = (internal & 0xf) + (internal >= 0x93d0 ? 14 : 0);
        G2Format* f = &g_gles2.astc[k];
        if (!f->internal)
            *f = (G2Format){internal, 0, 0, (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * (internal & 0xf) +
                                                       (internal >= 0x93d0)),
                            0, {0}, {0}, 16, 0, G2_FLOAT, astc_blocks[internal & 0xf]};
        return f;
    }
    /* GLES 2 with GL_OES_rgb8_rgba8 and GL_OES_depth24 names some sized formats for renderbuffers */
    return NULL;
}

VkImageAspectFlags gles2_aspect(const G2Format* f) {
    switch (f->kind) {
    case G2_DEPTH: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case G2_STENCIL: return VK_IMAGE_ASPECT_STENCIL_BIT;
    case G2_DS: return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
VkImageLayout gles2_rest_layout(const G2Format* f, bool sampled) {
    bool depth = f->kind >= G2_DEPTH;
    if (sampled) return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

/* the Vulkan format of f that the device takes for what usage needs: its own, the other, or the 4-component one of
   expand; 0 when none. *expanded: the 4-component one, *swizzle: the view's */
static VkFormat pick(const G2Format* f, VkFormatFeatureFlags need, bool* expanded, VkComponentMapping* swizzle) {
    *expanded = false;
    VkFormatProperties p;
    V.vk.vkGetPhysicalDeviceFormatProperties(V.physical, f->vk, &p);
    if ((p.optimalTilingFeatures & need) == need) {
        *swizzle = f->swizzle;
        return f->vk;
    }
    if (!f->alt) return VK_FORMAT_UNDEFINED;
    V.vk.vkGetPhysicalDeviceFormatProperties(V.physical, f->alt, &p);
    if ((p.optimalTilingFeatures & need) != need) return VK_FORMAT_UNDEFINED;
    *expanded = f->expand != 0;
    *swizzle = f->alt_swizzle;
    return f->alt;
}

/* the features a texture's or renderbuffer's format needs */
static VkFormatFeatureFlags needs(const G2Format* f, bool texture) {
    VkFormatFeatureFlags n = f->kind >= G2_DEPTH ? VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT : 0;
    if (texture) n |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    else if (f->kind < G2_DEPTH) n |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    return n;
}

/* ---- textures ---- */

static int target_index(unsigned target) {
    switch (target) {
    case GL_TEXTURE_2D_: return G2_T2D;
    case GL_TEXTURE_CUBE_MAP_: return G2_TCUBE;
    case GL_TEXTURE_3D_: return G2_T3D;
    case GL_TEXTURE_2D_ARRAY_: return G2_TARRAY;
    case GL_TEXTURE_2D_MULTISAMPLE_: return G2_TMS;
    default: return -1;
    }
}
G2Texture* gles2_unit_texture(Gles2Context* gl, unsigned unit, unsigned target) {
    return unit < G2_UNITS && target < G2_TARGETS ? gles2_texture(gl->textures[unit][target]) : NULL;
}
/* the texture bound to a GL target of the active unit; a cube map's face in *face */
static G2Texture* bound_texture(Gles2Context* gl, unsigned target, int* face) {
    int f = 0;
    if (target >= GL_TEXTURE_CUBE_MAP_POSITIVE_X_ && target < GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + 6) {
        f = (int)(target - GL_TEXTURE_CUBE_MAP_POSITIVE_X_);
        target = GL_TEXTURE_CUBE_MAP_;
    }
    if (face) *face = f;
    int i = target_index(target);
    if (i < 0) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return NULL;
    }
    G2Texture* t = gles2_unit_texture(gl, gl->active, (unsigned)i);
    if (!t) gles2_error(gl, GL_INVALID_OPERATION_); /* texture 0: GL's default texture, which nothing here has */
    return t;
}

static void views_forget(G2Texture* t) {
    for (uint32_t i = 0; i < t->view_count; ++i) gles_vk_retire((GlesGarbage){.view = t->views[i].view});
    t->view_count = 0;
}
static void texture_free(G2Texture* t) {
    views_forget(t);
    if (t->image.image) gles_vk_retire((GlesGarbage){.image = t->image.image, .memory = t->image.memory});
    t->image = (GlesImage){0};
    t->defined = 0;
}

/* a barrier of levels [base, base + count) of an image */
static void level_barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, int base, int count,
                          VkImageLayout from, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access,
                          VkPipelineStageFlags src, VkPipelineStageFlags dst) {
    VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                              .srcAccessMask = src_access,
                              .dstAccessMask = dst_access,
                              .oldLayout = from,
                              .newLayout = to,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .image = image,
                              .subresourceRange = {aspect, (uint32_t)base, (uint32_t)count, 0,
                                                   VK_REMAINING_ARRAY_LAYERS}};
    V.vk.vkCmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}
static const VkPipelineStageFlags READERS = VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                                            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;

bool gles2_texture_storage(Gles2Context* gl, G2Texture* t, const G2Format* f, int width, int height, int depth,
                           int levels) {
    int layers = t->target == G2_TCUBE ? 6 : t->target == G2_TARRAY ? depth : 1;
    int d = t->target == G2_T3D ? depth : 1;
    if (t->image.image && t->format == f && t->width == width && t->height == height && t->depth == d &&
        t->layers == layers && t->levels >= levels)
        return true;
    bool expanded;
    VkComponentMapping swizzle;
    VkFormat vk = pick(f, needs(f, true), &expanded, &swizzle);
    if (!vk) {
        gles2_warn(0, "a texture format the device does not sample: the texture stays empty");
        return false;
    }
    texture_free(t);
    VkFormatProperties p;
    V.vk.vkGetPhysicalDeviceFormatProperties(V.physical, vk, &p);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .flags = (t->target == G2_TCUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                                       (t->target == G2_T3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
                              .imageType = t->target == G2_T3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                              .format = vk,
                              .extent = {(uint32_t)width, (uint32_t)height, (uint32_t)d},
                              .mipLevels = (uint32_t)levels,
                              .arrayLayers = (uint32_t)layers,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = usage};
    if (!gles_vk_image(&info, VK_IMAGE_VIEW_TYPE_MAX_ENUM, 0, &t->image)) {
        gles2_error(gl, GL_OUT_OF_MEMORY_);
        return false;
    }
    t->vk = vk;
    t->format = f;
    t->swizzle = swizzle;
    t->width = width, t->height = height, t->depth = d;
    t->levels = levels;
    t->layers = layers;
    t->samples = VK_SAMPLE_COUNT_1_BIT;
    t->generation++;
    t->defined = 0;
    (void)expanded;
    /* GL leaves the texels undefined: zeros, and the image at its rest */
    GlesSlot* slot = gles2_transfer_slot(gl);
    VkImageAspectFlags aspect = gles2_aspect(f);
    t->layout = gles2_rest_layout(f, true);
    if (slot) {
        level_barrier(slot->cb, t->image.image, aspect, 0, levels, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageSubresourceRange range = {aspect, 0, (uint32_t)levels, 0, (uint32_t)layers};
        if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue zero = {{0}};
            V.vk.vkCmdClearColorImage(slot->cb, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        } else {
            VkClearDepthStencilValue one = {1, 0};
            V.vk.vkCmdClearDepthStencilImage(slot->cb, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1,
                                             &range);
        }
        level_barrier(slot->cb, t->image.image, aspect, 0, levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->layout,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      READERS);
    }
    gles2_target_forget();
    return true;
}

static int full_levels(int width, int height, int depth) {
    int m = width > height ? width : height;
    if (depth > m) m = depth;
    int n = 1;
    while (m > 1) m >>= 1, ++n;
    return n;
}

/* the expansion's pipeline (gles_expand.comp), at its first use */
static bool expand_ready(void) {
    if (g_gles2.expand) return true;
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
                                         {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 2,
                                           .pBindings = b};
    VkPushConstantRange push = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 12};
    if (V.vk.vkCreateDescriptorSetLayout(V.device, &set, NULL, &g_gles2.expand_set) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &g_gles2.expand_set,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &push};
    if (V.vk.vkCreatePipelineLayout(V.device, &layout, NULL, &g_gles2.expand_layout) != VK_SUCCESS) return false;
    VkShaderModule m = gles_vk_shader(android_gles_expand_comp_spv, (size_t)android_gles_expand_comp_spv_size);
    if (!m) return false;
    VkComputePipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                  .module = m,
                                                  .pName = "main"},
                                        .layout = g_gles2.expand_layout};
    VkResult r = V.vk.vkCreateComputePipelines(V.device, VK_NULL_HANDLE, 1, &info, NULL, &g_gles2.expand);
    V.vk.vkDestroyShaderModule(V.device, m, NULL);
    return r == VK_SUCCESS;
}

/* the 4-component texels of n 3-component ones of size bytes each, at *at of the staging buffer: written by
   gles_expand.comp after it, *at and *size moved to them */
static bool expand_texels(GlesSlot* slot, const G2Format* f, uint32_t n, VkDeviceSize* at, VkDeviceSize* size) {
    if (!expand_ready()) return false;
    VkDeviceSize align = V.props.limits.minStorageBufferOffsetAlignment;
    if (align < 16) align = 16;
    VkDeviceSize out_size = (VkDeviceSize)n * 4 * f->expand;
    VkDeviceSize out = gles_vk_arena_take(slot, &slot->staging, out_size, align, 8u << 20, 0);
    if (out == VK_WHOLE_SIZE) return false;
    VkDescriptorSet set = gles_vk_set(slot, g_gles2.expand_set);
    if (!set) return false;
    VkDescriptorBufferInfo bufs[2] = {{slot->staging.buffer, *at, (*size + 3) & ~(VkDeviceSize)3},
                                      {slot->staging.buffer, out, out_size}};
    VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 0,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &bufs[0]},
                                 {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                  .pBufferInfo = &bufs[1]}};
    V.vk.vkUpdateDescriptorSets(V.device, 2, w, 0, NULL);
    /* the fourth component: one, as each kind has it */
    uint32_t one = f->kind == G2_INT || f->kind == G2_UINT ? 1
                   : f->expand == 1                        ? (f->vk == VK_FORMAT_R8G8B8_SNORM ? 0x7fu : 0xffu)
                   : f->expand == 2                        ? 0x3c00u
                                                           : 0x3f800000u;
    uint32_t push[3] = {n, f->expand, one};
    V.vk.vkCmdBindPipeline(slot->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_gles2.expand);
    V.vk.vkCmdBindDescriptorSets(slot->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_gles2.expand_layout, 0, 1, &set, 0, NULL);
    V.vk.vkCmdPushConstants(slot->cb, g_gles2.expand_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof push, push);
    V.vk.vkCmdDispatch(slot->cb, (n + 63) / 64, 1, 1);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT};
    V.vk.vkCmdPipelineBarrier(slot->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb,
                              0, NULL, 0, NULL);
    *at = out;
    *size = out_size;
    return true;
}

/* the client's pixels of a box of level into the texture: the bytes copied to staging as whole rows, expanded on the
   GPU when the image has 4 components for 3, copied into the image, which returns to its rest. pixels is the guest's
   address, or an offset into the pixel unpack buffer. compressed: size bytes of blocks */
static void upload(Gles2Context* gl, G2Texture* t, int level, int x, int y, int z, int w, int h, int d,
                   uint64_t pixels, bool compressed, size_t csize) {
    if (!t->image.image || w <= 0 || h <= 0 || d <= 0) return;
    const G2Format* f = t->format;
    const unsigned char* src = (const unsigned char*)(uintptr_t)pixels;
    G2Buffer* unpack = gles2_buffer(gl->pixel_unpack);
    if (unpack) src = gles2_buffer_bytes(unpack) + pixels;
    if (!src && !unpack) return;
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (!slot) return;
    size_t row, rows, stride, image;
    if (compressed) {
        int bw = f->block >> 4, bh = f->block & 15;
        row = (size_t)((w + bw - 1) / bw) * f->bytes;
        rows = (size_t)((h + bh - 1) / bh);
        stride = row;
        image = row * rows;
        if (image * (size_t)d > csize) {
            gles2_error(gl, GL_INVALID_VALUE_);
            return;
        }
    } else {
        int row_length = gl->pixel[1].row_length, height = gl->pixel[1].image_height;
        size_t alignment = (size_t)gl->pixel[1].alignment;
        row = (size_t)w * f->bytes;
        rows = (size_t)h;
        size_t length = (size_t)(row_length ? row_length : w) * f->bytes;
        stride = (length + alignment - 1) / alignment * alignment;
        image = stride * (size_t)(height ? height : h);
        src += (size_t)gl->pixel[1].skip_images * image + (size_t)gl->pixel[1].skip_rows * stride +
               (size_t)gl->pixel[1].skip_pixels * f->bytes;
    }
    VkDeviceSize size = (VkDeviceSize)row * rows * (VkDeviceSize)d;
    /* an offset that is a multiple of the texel's or block's bytes and of 4 */
    VkDeviceSize unit = f->bytes % 4 == 0 ? f->bytes : f->bytes % 2 == 0 ? (VkDeviceSize)f->bytes * 2 : (VkDeviceSize)f->bytes * 4;
    /* gles_expand.comp reads the texels as a storage buffer, at its offset alignment */
    bool expanding = t->vk == f->alt && f->expand;
    VkDeviceSize storage = V.props.limits.minStorageBufferOffsetAlignment;
    if (expanding) unit = storage > 16 ? storage : 16;
    VkDeviceSize at = gles_vk_arena_take(slot, &slot->staging, size + unit + 4, 16, 8u << 20, 0);
    if (at == VK_WHOLE_SIZE) {
        gles2_error(gl, GL_OUT_OF_MEMORY_);
        return;
    }
    at = (at + unit - 1) / unit * unit;
    unsigned char* dst = slot->staging.map + at;
    /* the one copy of guest memory, whole rows at a time */
    if (stride == row && (d == 1 || image == row * rows)) memcpy(dst, src, (size_t)size);
    else
        for (int k = 0; k < d; ++k)
            for (size_t r = 0; r < rows; ++r)
                memcpy(dst + ((size_t)k * rows + r) * row, src + (size_t)k * image + r * stride, row);
    VkCommandBuffer cb = slot->cb;
    VkImageAspectFlags aspect = gles2_aspect(f);
    if (aspect == (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) {
        gles2_warn(1, "the client's depth and stencil texels are not copied into a texture yet");
        return;
    }
    if (t->vk == VK_FORMAT_X8_D24_UNORM_PACK32 || (t->vk == VK_FORMAT_D32_SFLOAT && f->type != GL_FLOAT_)) {
        gles2_warn(2, "24-bit depth texels from the client are not converted yet: the texture keeps its depth");
        return;
    }
    if (expanding && !expand_texels(slot, f, (uint32_t)((size_t)w * (size_t)h * (size_t)d), &at, &size))
        return;
    level_barrier(cb, t->image.image, aspect, level, 1, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, READERS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    bool layered = t->target == G2_TCUBE || t->target == G2_TARRAY;
    VkBufferImageCopy copy = {.bufferOffset = at,
                              .imageSubresource = {aspect, (uint32_t)level, layered ? (uint32_t)z : 0,
                                                   layered ? (uint32_t)d : 1},
                              .imageOffset = {x, y, layered ? 0 : z},
                              .imageExtent = {(uint32_t)w, (uint32_t)h, layered ? 1 : (uint32_t)d}};
    V.vk.vkCmdCopyBufferToImage(cb, slot->staging.buffer, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    level_barrier(cb, t->image.image, aspect, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->layout,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, READERS);
}

/* a level's size from the base level's */
static int at_level(int size, int level) {
    int s = size >> level;
    return s > 0 ? s : 1;
}

/* glTexImage2D and glTexImage3D, and the compressed ones: level's size and format, and its texels */
static void tex_image(Gles2Context* gl, unsigned target, int level, unsigned internal, int w, int h, int d,
                      unsigned format, unsigned type, uint64_t pixels, bool compressed, size_t csize) {
    int face = 0;
    G2Texture* t = bound_texture(gl, target, &face);
    if (!t) return;
    if (level < 0 || level > 15 || w < 0 || h < 0 || d < 0 || (uint32_t)w > V.max_texture ||
        (uint32_t)h > V.max_texture) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    if (t->immutable) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    const G2Format* f = gles2_format(internal, format, type);
    if (!f || (!compressed && f->block) || (compressed && !f->block)) {
        if (!g_gles2.said[19]) {
            fprintf(stderr, "scarletnx-android: GLES 2: no texture format for 0x%x (0x%x/0x%x)\n", internal, format,
                    type);
            g_gles2.said[19] = true;
        }
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    if (!compressed && (format != f->format || type != f->type)) {
        gles2_warn(3, "texels of a type that is not the format's own: the texture stays empty");
        pixels = 0;
    }
    if (!w || !h || !d) return;
    /* the base level's size, from this level's */
    int bw = w << level, bh = h << level, bd = t->target == G2_T3D ? d << level : d;
    bool same = t->image.image && t->format == f && at_level(t->width, level) == w && at_level(t->height, level) == h;
    if (!same) {
        if (level && t->image.image) {
            gles2_warn(4, "a level whose size is not the base level's: it is left out");
            return;
        }
        if (!gles2_texture_storage(gl, t, f, bw, bh, bd, full_levels(bw, bh, t->target == G2_T3D ? bd : 1))) return;
    }
    t->defined |= 1u << level;
    if (pixels || gles2_buffer(gl->pixel_unpack))
        upload(gl, t, level, 0, 0, t->target == G2_TCUBE ? face : 0, w, h, t->target == G2_TCUBE ? 1 : d, pixels,
               compressed, csize);
}

static void g_tex_image_2d(CPU* c, Gles2Context* gl) {
    tex_image(gl, (unsigned)X(0), (int)X(1), (unsigned)X(2), (int)X(3), (int)X(4), 1, (unsigned)X(6),
              (unsigned)X(7), gles2_arg(c, 8), false, 0);
}
static void g_tex_image_3d(CPU* c, Gles2Context* gl) {
    tex_image(gl, (unsigned)X(0), (int)X(1), (unsigned)X(2), (int)X(3), (int)X(4), (int)X(5), (unsigned)gles2_arg(c, 7),
              (unsigned)gles2_arg(c, 8), gles2_arg(c, 9), false, 0);
}
static void g_compressed_tex_image_2d(CPU* c, Gles2Context* gl) {
    tex_image(gl, (unsigned)X(0), (int)X(1), (unsigned)X(2), (int)X(3), (int)X(4), 1, 0, 0, X(7), true,
              (size_t)(uint32_t)X(6));
}
static void g_compressed_tex_image_3d(CPU* c, Gles2Context* gl) {
    tex_image(gl, (unsigned)X(0), (int)X(1), (unsigned)X(2), (int)X(3), (int)X(4), (int)X(5), 0, 0, gles2_arg(c, 8),
              true, (size_t)(uint32_t)X(7));
}
static void tex_sub_image(Gles2Context* gl, unsigned target, int level, int x, int y, int z, int w, int h, int d,
                          unsigned format, unsigned type, uint64_t pixels, bool compressed, size_t csize) {
    int face = 0;
    G2Texture* t = bound_texture(gl, target, &face);
    if (!t) return;
    if (!t->image.image || level < 0 || level >= t->levels || x < 0 || y < 0 || z < 0 ||
        x + w > at_level(t->width, level) || y + h > at_level(t->height, level)) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    if (!compressed && (format != t->format->format || type != t->format->type)) {
        gles2_warn(3, "texels of a type that is not the format's own: the texture stays empty");
        return;
    }
    if (t->target == G2_TCUBE) z = face, d = 1;
    upload(gl, t, level, x, y, z, w, h, d, pixels, compressed, csize);
}
static void g_tex_sub_image_2d(CPU* c, Gles2Context* gl) {
    tex_sub_image(gl, (unsigned)X(0), (int)X(1), (int)X(2), (int)X(3), 0, (int)X(4), (int)X(5), 1, (unsigned)X(6),
                  (unsigned)X(7), gles2_arg(c, 8), false, 0);
}
static void g_tex_sub_image_3d(CPU* c, Gles2Context* gl) {
    tex_sub_image(gl, (unsigned)X(0), (int)X(1), (int)X(2), (int)X(3), (int)X(4), (int)X(5), (int)X(6),
                  (int)X(7), (unsigned)gles2_arg(c, 8), (unsigned)gles2_arg(c, 9), gles2_arg(c, 10), false, 0);
}
static void g_compressed_tex_sub_image_2d(CPU* c, Gles2Context* gl) {
    tex_sub_image(gl, (unsigned)X(0), (int)X(1), (int)X(2), (int)X(3), 0, (int)X(4), (int)X(5), 1, 0, 0,
                  gles2_arg(c, 8), true, (size_t)(uint32_t)X(7));
}
static void g_compressed_tex_sub_image_3d(CPU* c, Gles2Context* gl) {
    tex_sub_image(gl, (unsigned)X(0), (int)X(1), (int)X(2), (int)X(3), (int)X(4), (int)X(5), (int)X(6), (int)X(7), 0,
                  0, gles2_arg(c, 10), true, (size_t)(uint32_t)gles2_arg(c, 9));
}
/* glTexStorage2D(target, levels, internal, width, height) and glTexStorage3D(..., depth) */
static void tex_storage(Gles2Context* gl, unsigned target, int levels, unsigned internal, int w, int h, int d) {
    G2Texture* t = bound_texture(gl, target, NULL);
    if (!t) return;
    const G2Format* f = gles2_format(internal, 0, 0);
    if (!f || levels < 1 || w < 1 || h < 1 || d < 1 || levels > full_levels(w, h, t->target == G2_T3D ? d : 1)) {
        gles2_error(gl, f ? GL_INVALID_VALUE_ : GL_INVALID_ENUM_);
        return;
    }
    if (t->immutable) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    if (!gles2_texture_storage(gl, t, f, w, h, d, levels)) return;
    t->immutable = true;
    t->defined = (levels >= 32 ? 0xffffffffu : (1u << levels) - 1);
    t->max_level = t->max_level > levels - 1 ? levels - 1 : t->max_level;
}
static void g_tex_storage_2d(CPU* c, Gles2Context* gl) {
    tex_storage(gl, (unsigned)X(0), (int)X(1), (unsigned)X(2), (int)X(3), (int)X(4), 1);
}
static void g_tex_storage_3d(CPU* c, Gles2Context* gl) {
    tex_storage(gl, (unsigned)X(0), (int)X(1), (unsigned)X(2), (int)X(3), (int)X(4), (int)X(5));
}

static void g_gen_textures(CPU* c, Gles2Context* gl) {
    gen_call(c, gl, gen_textures);
}
static void g_bind_texture(CPU* c, Gles2Context* gl) {
    int i = target_index((unsigned)X(0));
    unsigned name = (unsigned)X(1);
    if (i < 0) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    G2Texture* t = gles2_texture(name);
    if (name && !t) {
        t = entry((void**)&g_gles2.textures, &g_gles2.texture_count, sizeof *t, name);
        if (!t) return;
        *t = (G2Texture){0};
        t->used = true;
        texture_init(t);
    }
    if (t && t->target == 0xff) t->target = (uint8_t)i;
    if (t && t->target != i) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    gl->textures[gl->active][i] = name;
}
static void forget_texture(Gles2Context* gl, unsigned name) {
    G2Texture* t = gles2_texture(name);
    if (!t) return;
    texture_free(t);
    *t = (G2Texture){0};
    for (int u = 0; u < G2_UNITS; ++u)
        for (int k = 0; k < G2_TARGETS; ++k)
            if (gl->textures[u][k] == name) gl->textures[u][k] = 0;
    gles2_target_forget();
}
static void g_delete_textures(CPU* c, Gles2Context* gl) {
    each_name(c, gl, forget_texture);
}
static void g_is_texture(CPU* c, Gles2Context* gl) {
    (void)gl;
    G2Texture* t = gles2_texture((unsigned)X(0));
    X(0) = t && t->target != 0xff;
}
static void g_active_texture(CPU* c, Gles2Context* gl) {
    unsigned unit = (unsigned)X(0) - 0x84c0;
    if (unit < G2_UNITS) gl->active = unit;
    else gles2_error(gl, GL_INVALID_ENUM_);
}

/* a parameter of a sampler's state: false for one that is not a sampler's */
static bool sampler_parameter(G2SamplerState* s, unsigned pname, float v) {
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER_: s->min = (unsigned)v; return true;
    case GL_TEXTURE_MAG_FILTER_: s->mag = (unsigned)v; return true;
    case GL_TEXTURE_WRAP_S_: s->wrap[0] = (unsigned)v; return true;
    case GL_TEXTURE_WRAP_T_: s->wrap[1] = (unsigned)v; return true;
    case GL_TEXTURE_WRAP_R_: s->wrap[2] = (unsigned)v; return true;
    case GL_TEXTURE_MIN_LOD_: s->min_lod = v; return true;
    case GL_TEXTURE_MAX_LOD_: s->max_lod = v; return true;
    case GL_TEXTURE_COMPARE_MODE_: s->compare_mode = (unsigned)v; return true;
    case GL_TEXTURE_COMPARE_FUNC_: s->compare_func = (unsigned)v; return true;
    case GL_TEXTURE_MAX_ANISOTROPY_: s->anisotropy = v < 1 ? 1 : v; return true;
    default: return false;
    }
}
static bool sampler_get(const G2SamplerState* s, unsigned pname, float* v) {
    switch (pname) {
    case GL_TEXTURE_MIN_FILTER_: *v = (float)s->min; return true;
    case GL_TEXTURE_MAG_FILTER_: *v = (float)s->mag; return true;
    case GL_TEXTURE_WRAP_S_: *v = (float)s->wrap[0]; return true;
    case GL_TEXTURE_WRAP_T_: *v = (float)s->wrap[1]; return true;
    case GL_TEXTURE_WRAP_R_: *v = (float)s->wrap[2]; return true;
    case GL_TEXTURE_MIN_LOD_: *v = s->min_lod; return true;
    case GL_TEXTURE_MAX_LOD_: *v = s->max_lod; return true;
    case GL_TEXTURE_COMPARE_MODE_: *v = (float)s->compare_mode; return true;
    case GL_TEXTURE_COMPARE_FUNC_: *v = (float)s->compare_func; return true;
    case GL_TEXTURE_MAX_ANISOTROPY_: *v = s->anisotropy; return true;
    default: return false;
    }
}
static void tex_parameter(Gles2Context* gl, unsigned target, unsigned pname, float v) {
    G2Texture* t = bound_texture(gl, target, NULL);
    if (!t || sampler_parameter(&t->sampler, pname, v)) return;
    switch (pname) {
    case GL_TEXTURE_BASE_LEVEL_: t->base_level = (int)v; break;
    case GL_TEXTURE_MAX_LEVEL_: t->max_level = (int)v; break;
    case GL_TEXTURE_SWIZZLE_R_:
    case GL_TEXTURE_SWIZZLE_R_ + 1:
    case GL_TEXTURE_SWIZZLE_R_ + 2:
    case GL_TEXTURE_SWIZZLE_R_ + 3: t->swizzles[pname - GL_TEXTURE_SWIZZLE_R_] = (unsigned)v; break;
    case 0x8191: break; /* GL_GENERATE_MIPMAP: GLES 1.1's */
    default: gles2_error(gl, GL_INVALID_ENUM_);
    }
}
static void g_tex_parameteri(CPU* c, Gles2Context* gl) {
    tex_parameter(gl, (unsigned)X(0), (unsigned)X(1), (float)(int32_t)X(2));
}
static void g_tex_parameterf(CPU* c, Gles2Context* gl) {
    tex_parameter(gl, (unsigned)X(0), (unsigned)X(1), hle_f(c, 0));
}
static void g_tex_parameteriv(CPU* c, Gles2Context* gl) {
    int32_t v;
    memcpy(&v, PTR(2), 4);
    tex_parameter(gl, (unsigned)X(0), (unsigned)X(1), (float)v);
}
static void g_tex_parameterfv(CPU* c, Gles2Context* gl) {
    float v;
    memcpy(&v, PTR(2), 4);
    tex_parameter(gl, (unsigned)X(0), (unsigned)X(1), v);
}
static bool tex_get(Gles2Context* gl, unsigned target, unsigned pname, float* v) {
    G2Texture* t = bound_texture(gl, target, NULL);
    if (!t) return false;
    if (sampler_get(&t->sampler, pname, v)) return true;
    switch (pname) {
    case GL_TEXTURE_BASE_LEVEL_: *v = (float)t->base_level; return true;
    case GL_TEXTURE_MAX_LEVEL_: *v = (float)t->max_level; return true;
    case GL_TEXTURE_IMMUTABLE_FORMAT_: *v = t->immutable; return true;
    case GL_TEXTURE_IMMUTABLE_LEVELS_: *v = t->immutable ? (float)t->levels : 0; return true;
    case GL_TEXTURE_SWIZZLE_R_:
    case GL_TEXTURE_SWIZZLE_R_ + 1:
    case GL_TEXTURE_SWIZZLE_R_ + 2:
    case GL_TEXTURE_SWIZZLE_R_ + 3: *v = (float)t->swizzles[pname - GL_TEXTURE_SWIZZLE_R_]; return true;
    default: gles2_error(gl, GL_INVALID_ENUM_); return false;
    }
}
static void g_get_tex_parameteriv(CPU* c, Gles2Context* gl) {
    float v = 0;
    if (!tex_get(gl, (unsigned)X(0), (unsigned)X(1), &v)) return;
    int32_t x = (int32_t)v;
    memcpy(PTR(2), &x, 4);
}
static void g_get_tex_parameterfv(CPU* c, Gles2Context* gl) {
    float v = 0;
    if (tex_get(gl, (unsigned)X(0), (unsigned)X(1), &v)) memcpy(PTR(2), &v, 4);
}

bool gles2_texture_complete(const G2Texture* t, int* levels) {
    if (!t || !t->image.image || t->base_level >= t->levels || !(t->defined & (1u << t->base_level))) return false;
    int n = 1;
    int top = t->max_level < t->levels - 1 ? t->max_level : t->levels - 1;
    while (t->base_level + n <= top && (t->defined & (1u << (t->base_level + n)))) ++n;
    *levels = n;
    return true;
}

static VkComponentSwizzle component(unsigned gl_swizzle, VkComponentSwizzle identity) {
    switch (gl_swizzle) {
    case GL_RED_COMP_: return VK_COMPONENT_SWIZZLE_R;
    case GL_GREEN_: return VK_COMPONENT_SWIZZLE_G;
    case GL_BLUE_: return VK_COMPONENT_SWIZZLE_B;
    case GL_ALPHA_: return VK_COMPONENT_SWIZZLE_A;
    case GL_ZERO_: return VK_COMPONENT_SWIZZLE_ZERO;
    case GL_ONE_: return VK_COMPONENT_SWIZZLE_ONE;
    default: return identity;
    }
}
/* swizzle b applied after a: what a's component c reads */
static VkComponentSwizzle compose(VkComponentMapping a, VkComponentSwizzle s, VkComponentSwizzle self) {
    switch (s == VK_COMPONENT_SWIZZLE_IDENTITY ? self : s) {
    case VK_COMPONENT_SWIZZLE_R: return a.r == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_R : a.r;
    case VK_COMPONENT_SWIZZLE_G: return a.g == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_G : a.g;
    case VK_COMPONENT_SWIZZLE_B: return a.b == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_B : a.b;
    case VK_COMPONENT_SWIZZLE_A: return a.a == VK_COMPONENT_SWIZZLE_IDENTITY ? VK_COMPONENT_SWIZZLE_A : a.a;
    default: return s;
    }
}

VkImageView gles2_texture_view(G2Texture* t, VkImageViewType type, int base, int levels, int layer, int layers,
                               bool swizzled, VkImageAspectFlags aspect) {
    uint64_t key = (uint64_t)type | (uint64_t)base << 3 | (uint64_t)levels << 8 | (uint64_t)layer << 13 |
                   (uint64_t)layers << 25 | (uint64_t)swizzled << 37 | (uint64_t)aspect << 38;
    if (swizzled)
        for (int i = 0; i < 4; ++i) key ^= (uint64_t)(t->swizzles[i] & 0xffff) << (42 + 5 * i) ^ t->swizzles[i] >> 3;
    for (uint32_t i = 0; i < t->view_count; ++i)
        if (t->views[i].key == key) return t->views[i].view;
    VkComponentMapping m = {0};
    if (swizzled) {
        VkComponentMapping user = {component(t->swizzles[0], VK_COMPONENT_SWIZZLE_R),
                                   component(t->swizzles[1], VK_COMPONENT_SWIZZLE_G),
                                   component(t->swizzles[2], VK_COMPONENT_SWIZZLE_B),
                                   component(t->swizzles[3], VK_COMPONENT_SWIZZLE_A)};
        m = (VkComponentMapping){compose(t->swizzle, user.r, VK_COMPONENT_SWIZZLE_R),
                                 compose(t->swizzle, user.g, VK_COMPONENT_SWIZZLE_G),
                                 compose(t->swizzle, user.b, VK_COMPONENT_SWIZZLE_B),
                                 compose(t->swizzle, user.a, VK_COMPONENT_SWIZZLE_A)};
    }
    VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = t->image.image,
                                  .viewType = type,
                                  .format = t->vk,
                                  .components = m,
                                  .subresourceRange = {aspect, (uint32_t)base, (uint32_t)levels, (uint32_t)layer,
                                                       (uint32_t)layers}};
    VkImageView view = VK_NULL_HANDLE;
    if (V.vk.vkCreateImageView(V.device, &info, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    if (t->view_count == G2_VIEWS) views_forget(t);
    t->views[t->view_count].key = key;
    t->views[t->view_count++].view = view;
    return view;
}

/* glGenerateMipmap: each level blitted from the one above it, on the GPU */
static void g_generate_mipmap(CPU* c, Gles2Context* gl) {
    G2Texture* t = bound_texture(gl, (unsigned)X(0), NULL);
    if (!t || !t->image.image) return;
    int base = t->base_level;
    if (!(t->defined & (1u << base)) || base >= t->levels - 1) return;
    VkFormatProperties p;
    V.vk.vkGetPhysicalDeviceFormatProperties(V.physical, t->vk, &p);
    VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((p.optimalTilingFeatures & blit) != blit) {
        gles2_warn(5, "glGenerateMipmap of a format the device does not blit: the levels stay as they are");
        return;
    }
    bool linear = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (!slot) return;
    VkCommandBuffer cb = slot->cb;
    VkImageAspectFlags aspect = gles2_aspect(t->format);
    level_barrier(cb, t->image.image, aspect, base, 1, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, READERS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (int l = base + 1; l < t->levels; ++l) {
        level_barrier(cb, t->image.image, aspect, l, 1, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, READERS, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit region = {
            .srcSubresource = {aspect, (uint32_t)(l - 1), 0, (uint32_t)t->layers},
            .srcOffsets = {{0, 0, 0}, {at_level(t->width, l - 1), at_level(t->height, l - 1), at_level(t->depth, l - 1)}},
            .dstSubresource = {aspect, (uint32_t)l, 0, (uint32_t)t->layers},
            .dstOffsets = {{0, 0, 0}, {at_level(t->width, l), at_level(t->height, l), at_level(t->depth, l)}}};
        V.vk.vkCmdBlitImage(cb, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        level_barrier(cb, t->image.image, aspect, l, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        t->defined |= 1u << l;
    }
    level_barrier(cb, t->image.image, aspect, base, t->levels - base, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->layout,
                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, READERS);
}

/* ---- samplers ---- */

const G2SamplerState* gles2_unit_sampler(Gles2Context* gl, unsigned unit, const G2Texture* t) {
    G2Sampler* s = unit < G2_UNITS ? sampler_object(gl->samplers[unit]) : NULL;
    return s ? &s->state : &t->sampler;
}

static VkSamplerAddressMode address(unsigned wrap) {
    switch (wrap) {
    case GL_CLAMP_TO_EDGE_: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case GL_MIRRORED_REPEAT_: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case GL_CLAMP_TO_BORDER_: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

VkSampler gles2_vk_sampler(const G2SamplerState* in, bool integer) {
    G2SamplerState s = *in;
    if (integer) s.min = s.mag = GL_NEAREST_; /* an integer texture is filtered by nearest alone */
    for (size_t i = 0; i < g_gles2.vk_sampler_count; ++i)
        if (!memcmp(&g_gles2.vk_samplers[i].state, &s, sizeof s)) return g_gles2.vk_samplers[i].sampler;
    bool mipmapped = s.min >= GL_NEAREST_MIPMAP_NEAREST_ && s.min <= GL_LINEAR_MIPMAP_LINEAR_;
    bool min_linear = s.min == GL_LINEAR_ || s.min == GL_LINEAR_MIPMAP_NEAREST_ || s.min == GL_LINEAR_MIPMAP_LINEAR_;
    bool mip_linear = s.min == GL_NEAREST_MIPMAP_LINEAR_ || s.min == GL_LINEAR_MIPMAP_LINEAR_;
    float aniso = s.anisotropy > V.props.limits.maxSamplerAnisotropy ? V.props.limits.maxSamplerAnisotropy : s.anisotropy;
    static const VkCompareOp ops[8] = {VK_COMPARE_OP_NEVER,   VK_COMPARE_OP_LESS,      VK_COMPARE_OP_EQUAL,
                                       VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                       VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    VkSamplerCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = s.mag == GL_NEAREST_ ? VK_FILTER_NEAREST : VK_FILTER_LINEAR,
        .minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = mip_linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = address(s.wrap[0]),
        .addressModeV = address(s.wrap[1]),
        .addressModeW = address(s.wrap[2]),
        .anisotropyEnable = V.features.samplerAnisotropy && aniso > 1,
        .maxAnisotropy = aniso,
        .compareEnable = s.compare_mode == GL_COMPARE_REF_TO_TEXTURE_,
        .compareOp = ops[(s.compare_func - 0x0200) & 7],
        /* a sampler without mipmaps reads the base level: Vulkan's lod clamp of 0.25 keeps its min and mag filters */
        .minLod = mipmapped ? (s.min_lod < 0 ? 0 : s.min_lod) : 0,
        .maxLod = mipmapped ? (s.max_lod < 0 ? 0 : s.max_lod) : 0.25f,
        .borderColor = integer ? VK_BORDER_COLOR_INT_TRANSPARENT_BLACK : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    VkSampler sampler = VK_NULL_HANDLE;
    if (V.vk.vkCreateSampler(V.device, &info, NULL, &sampler) != VK_SUCCESS) return VK_NULL_HANDLE;
    void* grown = realloc(g_gles2.vk_samplers, (g_gles2.vk_sampler_count + 1) * sizeof *g_gles2.vk_samplers);
    if (!grown) return sampler;
    g_gles2.vk_samplers = grown;
    g_gles2.vk_samplers[g_gles2.vk_sampler_count].state = s;
    g_gles2.vk_samplers[g_gles2.vk_sampler_count++].sampler = sampler;
    return sampler;
}

static void g_gen_samplers(CPU* c, Gles2Context* gl) {
    gen_call(c, gl, gen_samplers);
}
static void forget_sampler(Gles2Context* gl, unsigned name) {
    G2Sampler* s = sampler_object(name);
    if (!s) return;
    *s = (G2Sampler){0};
    for (int u = 0; u < G2_UNITS; ++u)
        if (gl->samplers[u] == name) gl->samplers[u] = 0;
}
static void g_delete_samplers(CPU* c, Gles2Context* gl) {
    each_name(c, gl, forget_sampler);
}
static void g_is_sampler(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = sampler_object((unsigned)X(0)) != NULL;
}
static void g_bind_sampler(CPU* c, Gles2Context* gl) {
    unsigned unit = (unsigned)X(0), name = (unsigned)X(1);
    if (unit >= G2_UNITS || (name && !sampler_object(name))) {
        gles2_error(gl, unit >= G2_UNITS ? GL_INVALID_VALUE_ : GL_INVALID_OPERATION_);
        return;
    }
    gl->samplers[unit] = name;
}
static void sampler_set(Gles2Context* gl, unsigned name, unsigned pname, float v) {
    G2Sampler* s = sampler_object(name);
    if (!s) gles2_error(gl, GL_INVALID_OPERATION_);
    else if (!sampler_parameter(&s->state, pname, v)) gles2_error(gl, GL_INVALID_ENUM_);
}
static void g_sampler_parameteri(CPU* c, Gles2Context* gl) {
    sampler_set(gl, (unsigned)X(0), (unsigned)X(1), (float)(int32_t)X(2));
}
static void g_sampler_parameterf(CPU* c, Gles2Context* gl) {
    sampler_set(gl, (unsigned)X(0), (unsigned)X(1), hle_f(c, 0));
}
static void g_sampler_parameteriv(CPU* c, Gles2Context* gl) {
    int32_t v;
    memcpy(&v, PTR(2), 4);
    sampler_set(gl, (unsigned)X(0), (unsigned)X(1), (float)v);
}
static void g_sampler_parameterfv(CPU* c, Gles2Context* gl) {
    float v;
    memcpy(&v, PTR(2), 4);
    sampler_set(gl, (unsigned)X(0), (unsigned)X(1), v);
}
static void g_get_sampler_parameteriv(CPU* c, Gles2Context* gl) {
    G2Sampler* s = sampler_object((unsigned)X(0));
    float v = 0;
    if (!s || !sampler_get(&s->state, (unsigned)X(1), &v)) {
        gles2_error(gl, s ? GL_INVALID_ENUM_ : GL_INVALID_OPERATION_);
        return;
    }
    int32_t x = (int32_t)v;
    memcpy(PTR(2), &x, 4);
}
static void g_get_sampler_parameterfv(CPU* c, Gles2Context* gl) {
    G2Sampler* s = sampler_object((unsigned)X(0));
    float v = 0;
    if (!s || !sampler_get(&s->state, (unsigned)X(1), &v)) {
        gles2_error(gl, s ? GL_INVALID_ENUM_ : GL_INVALID_OPERATION_);
        return;
    }
    memcpy(PTR(2), &v, 4);
}

/* ---- renderbuffers ---- */

static void g_gen_renderbuffers(CPU* c, Gles2Context* gl) {
    gen_call(c, gl, gen_renderbuffers);
}
static void g_bind_renderbuffer(CPU* c, Gles2Context* gl) {
    unsigned name = (unsigned)X(1);
    if ((unsigned)X(0) != GL_RENDERBUFFER_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    if (name && !gles2_renderbuffer(name)) {
        G2Renderbuffer* r = entry((void**)&g_gles2.renderbuffers, &g_gles2.renderbuffer_count, sizeof *r, name);
        if (!r) return;
        *r = (G2Renderbuffer){0};
        r->used = true;
    }
    gl->renderbuffer = name;
}
static void renderbuffer_free(G2Renderbuffer* r) {
    if (r->image.image)
        gles_vk_retire((GlesGarbage){.image = r->image.image, .view = r->image.view, .memory = r->image.memory});
    r->image = (GlesImage){0};
}
static void forget_renderbuffer(Gles2Context* gl, unsigned name) {
    G2Renderbuffer* r = gles2_renderbuffer(name);
    if (!r) return;
    renderbuffer_free(r);
    *r = (G2Renderbuffer){0};
    if (gl->renderbuffer == name) gl->renderbuffer = 0;
    gles2_target_forget();
}
void gles2_objects_vk_free(void) {
    for (size_t i = 0; i < g_gles2.buffer_count; ++i) buffer_vk_free(&g_gles2.buffers[i]);
    for (size_t i = 0; i < g_gles2.texture_count; ++i) texture_free(&g_gles2.textures[i]);
    for (size_t i = 0; i < g_gles2.renderbuffer_count; ++i) renderbuffer_free(&g_gles2.renderbuffers[i]);
    for (size_t i = 0; i < g_gles2.query_count; ++i) {
        G2Query* q = &g_gles2.queries[i];
        if (q->pool) V.vk.vkDestroyQueryPool(V.device, q->pool, NULL);
        q->pool = VK_NULL_HANDLE;
    }
    for (size_t i = 0; i < g_gles2.vk_sampler_count; ++i)
        V.vk.vkDestroySampler(V.device, g_gles2.vk_samplers[i].sampler, NULL);
    free(g_gles2.vk_samplers);
    g_gles2.vk_samplers = NULL;
    g_gles2.vk_sampler_count = 0;
    if (g_gles2.expand) V.vk.vkDestroyPipeline(V.device, g_gles2.expand, NULL);
    if (g_gles2.expand_layout) V.vk.vkDestroyPipelineLayout(V.device, g_gles2.expand_layout, NULL);
    if (g_gles2.expand_set) V.vk.vkDestroyDescriptorSetLayout(V.device, g_gles2.expand_set, NULL);
    g_gles2.expand = VK_NULL_HANDLE;
    g_gles2.expand_layout = VK_NULL_HANDLE;
    g_gles2.expand_set = VK_NULL_HANDLE;
}
static void g_delete_renderbuffers(CPU* c, Gles2Context* gl) {
    each_name(c, gl, forget_renderbuffer);
}
static void g_is_renderbuffer(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = gles2_renderbuffer((unsigned)X(0)) != NULL;
}
/* a renderbuffer's format: the sized ones of GLES 2 and 3 */
static const G2Format* renderbuffer_format(unsigned internal) {
    switch (internal) {
    case 0x8d62: return gles2_format(0x8d62, 0, 0); /* GL_RGB565 */
    case 0x8056:
    case 0x8057:
    case 0x8058:
    case 0x8051:
    default: return gles2_format(internal, 0, 0);
    }
}
static void renderbuffer_storage(Gles2Context* gl, int samples, unsigned internal, int w, int h) {
    G2Renderbuffer* r = gles2_renderbuffer(gl->renderbuffer);
    if (!r) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    const G2Format* f = renderbuffer_format(internal);
    if (!f || f->block) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    if (w < 0 || h < 0 || (uint32_t)w > V.max_texture || (uint32_t)h > V.max_texture || samples < 0) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    renderbuffer_free(r);
    r->width = w, r->height = h;
    r->format = f;
    r->generation++;
    r->ready = false;
    gles2_target_forget();
    if (!w || !h) return;
    bool expanded;
    VkComponentMapping swizzle;
    VkFormat vk = pick(f, needs(f, false), &expanded, &swizzle);
    if (!vk && f->kind == G2_STENCIL) vk = VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (!vk) {
        gles2_warn(6, "a renderbuffer format the device does not render to");
        return;
    }
    VkSampleCountFlagBits count = samples > 2 ? VK_SAMPLE_COUNT_4_BIT : samples > 1 ? VK_SAMPLE_COUNT_2_BIT
                                  : samples == 1                         ? VK_SAMPLE_COUNT_2_BIT
                                                                        : VK_SAMPLE_COUNT_1_BIT;
    VkImageAspectFlags aspect = gles2_aspect(f);
    if (vk == VK_FORMAT_D24_UNORM_S8_UINT || vk == VK_FORMAT_D32_SFLOAT_S8_UINT)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = vk,
                              .extent = {(uint32_t)w, (uint32_t)h, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = count,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = (f->kind >= G2_DEPTH ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                           : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (!gles_vk_image(&info, VK_IMAGE_VIEW_TYPE_2D, aspect, &r->image)) {
        gles2_error(gl, GL_OUT_OF_MEMORY_);
        return;
    }
    r->vk = vk;
    r->samples = count;
}
static void g_renderbuffer_storage(CPU* c, Gles2Context* gl) {
    renderbuffer_storage(gl, 0, (unsigned)X(1), (int)X(2), (int)X(3));
}
static void g_renderbuffer_storage_multisample(CPU* c, Gles2Context* gl) {
    renderbuffer_storage(gl, (int)X(1), (unsigned)X(2), (int)X(3), (int)X(4));
}
static void g_get_renderbuffer_parameteriv(CPU* c, Gles2Context* gl) {
    G2Renderbuffer* r = gles2_renderbuffer(gl->renderbuffer);
    int32_t v = 0;
    if (!r) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    const G2Format* f = r->format;
    switch ((unsigned)X(1)) {
    case 0x8d42: v = r->width; break;
    case 0x8d43: v = r->height; break;
    case 0x8d44: v = f ? (int32_t)f->internal : 0x8056; break;
    case 0x8cab: v = r->samples > 1 ? (int32_t)r->samples : 0; break;
    case 0x8d50: /* GL_RENDERBUFFER_RED_SIZE .. ALPHA */
    case 0x8d51:
    case 0x8d52:
    case 0x8d53: v = f && f->kind < G2_DEPTH ? 8 : 0; break;
    case 0x8d54: v = f && (f->kind == G2_DEPTH || f->kind == G2_DS) ? 24 : 0; break;
    case 0x8d55: v = f && (f->kind == G2_STENCIL || f->kind == G2_DS) ? 8 : 0; break;
    default: gles2_error(gl, GL_INVALID_ENUM_); return;
    }
    memcpy(PTR(2), &v, 4);
}

/* ---- framebuffers ---- */

static void g_gen_framebuffers(CPU* c, Gles2Context* gl) {
    gen_call(c, gl, gen_framebuffers);
}
static void g_bind_framebuffer(CPU* c, Gles2Context* gl) {
    unsigned target = (unsigned)X(0), name = (unsigned)X(1);
    if (target != GL_FRAMEBUFFER_ && target != GL_READ_FRAMEBUFFER_ && target != GL_DRAW_FRAMEBUFFER_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    if (name && !gles2_framebuffer(name)) {
        G2Framebuffer* f = entry((void**)&g_gles2.framebuffers, &g_gles2.framebuffer_count, sizeof *f, name);
        if (!f) return;
        *f = (G2Framebuffer){0};
        f->used = true;
        f->draw[0] = GL_COLOR_ATTACHMENT0_;
        f->read = GL_COLOR_ATTACHMENT0_;
    }
    if (target != GL_READ_FRAMEBUFFER_) gl->draw_fbo = name;
    if (target != GL_DRAW_FRAMEBUFFER_) gl->read_fbo = name;
}
static void forget_framebuffer(Gles2Context* gl, unsigned name) {
    G2Framebuffer* f = gles2_framebuffer(name);
    if (!f) return;
    *f = (G2Framebuffer){0};
    if (gl->draw_fbo == name) gl->draw_fbo = 0;
    if (gl->read_fbo == name) gl->read_fbo = 0;
    gles2_target_forget();
}
static void g_delete_framebuffers(CPU* c, Gles2Context* gl) {
    each_name(c, gl, forget_framebuffer);
}
static void g_is_framebuffer(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = gles2_framebuffer((unsigned)X(0)) != NULL;
}
static G2Framebuffer* target_framebuffer(Gles2Context* gl, unsigned target) {
    unsigned name = target == GL_READ_FRAMEBUFFER_ ? gl->read_fbo : gl->draw_fbo;
    if (target != GL_FRAMEBUFFER_ && target != GL_READ_FRAMEBUFFER_ && target != GL_DRAW_FRAMEBUFFER_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return NULL;
    }
    G2Framebuffer* f = gles2_framebuffer(name);
    if (!f) gles2_error(gl, GL_INVALID_OPERATION_);
    return f;
}
/* the attachment points of a GL attachment: up to two (depth and stencil) */
static int attachment_points(unsigned attachment, int out[2]) {
    if (attachment >= GL_COLOR_ATTACHMENT0_ && attachment < GL_COLOR_ATTACHMENT0_ + G2_COLORS) {
        out[0] = (int)(attachment - GL_COLOR_ATTACHMENT0_);
        return 1;
    }
    if (attachment == GL_DEPTH_ATTACHMENT_) return out[0] = G2_DEPTH_ATTACHMENT, 1;
    if (attachment == GL_STENCIL_ATTACHMENT_) return out[0] = G2_STENCIL_ATTACHMENT, 1;
    if (attachment == GL_DEPTH_STENCIL_ATTACHMENT_) {
        out[0] = G2_DEPTH_ATTACHMENT;
        out[1] = G2_STENCIL_ATTACHMENT;
        return 2;
    }
    return 0;
}
static void attach(Gles2Context* gl, unsigned target, unsigned attachment, G2Attachment a) {
    G2Framebuffer* f = target_framebuffer(gl, target);
    int points[2];
    int n = attachment_points(attachment, points);
    if (!f) return;
    if (!n) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    if (!a.name) a = (G2Attachment){0};
    for (int i = 0; i < n; ++i) f->at[points[i]] = a;
    f->generation++;
    gles2_target_forget();
}
static void g_framebuffer_texture_2d(CPU* c, Gles2Context* gl) {
    unsigned textarget = (unsigned)X(2);
    int layer = textarget >= GL_TEXTURE_CUBE_MAP_POSITIVE_X_ && textarget < GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + 6
                    ? (int)(textarget - GL_TEXTURE_CUBE_MAP_POSITIVE_X_)
                    : 0;
    attach(gl, (unsigned)X(0), (unsigned)X(1), (G2Attachment){1, (unsigned)X(3), (int)X(4), layer});
}
static void g_framebuffer_texture_layer(CPU* c, Gles2Context* gl) {
    attach(gl, (unsigned)X(0), (unsigned)X(1), (G2Attachment){1, (unsigned)X(2), (int)X(3), (int)X(4)});
}
static void g_framebuffer_renderbuffer(CPU* c, Gles2Context* gl) {
    attach(gl, (unsigned)X(0), (unsigned)X(1), (G2Attachment){2, (unsigned)X(3), 0, 0});
}
/* the size, format and samples of an attachment; false when it has no image */
static bool attachment_image(const G2Attachment* a, int* w, int* h, const G2Format** f, VkSampleCountFlagBits* s) {
    if (a->kind == 1) {
        G2Texture* t = gles2_texture(a->name);
        if (!t || !t->image.image || a->level >= t->levels) return false;
        *w = at_level(t->width, a->level), *h = at_level(t->height, a->level), *f = t->format, *s = t->samples;
        return true;
    }
    G2Renderbuffer* r = gles2_renderbuffer(a->name);
    if (!r || !r->image.image) return false;
    *w = r->width, *h = r->height, *f = r->format, *s = r->samples;
    return true;
}
static unsigned completeness(G2Framebuffer* fb) {
    int count = 0, width = -1, height = -1;
    VkSampleCountFlagBits samples = 0;
    for (int i = 0; i < G2_ATTACHMENTS; ++i) {
        const G2Attachment* a = &fb->at[i];
        if (!a->kind) continue;
        int w, h;
        const G2Format* f;
        VkSampleCountFlagBits s;
        if (!attachment_image(a, &w, &h, &f, &s)) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_;
        bool depth = f->kind >= G2_DEPTH;
        if ((i < G2_COLORS && depth) || (i >= G2_COLORS && !depth)) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT_;
        if (count && (w != width || h != height)) return GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS_;
        if (count && s != samples) return 0x8d56; /* GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE */
        width = w, height = h, samples = s;
        ++count;
    }
    if (!count) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT_;
    const G2Attachment* d = &fb->at[G2_DEPTH_ATTACHMENT];
    const G2Attachment* st = &fb->at[G2_STENCIL_ATTACHMENT];
    if (d->kind && st->kind && (d->kind != st->kind || d->name != st->name)) return GL_FRAMEBUFFER_UNSUPPORTED_;
    return GL_FRAMEBUFFER_COMPLETE_;
}
static void g_check_framebuffer_status(CPU* c, Gles2Context* gl) {
    unsigned target = (unsigned)X(0);
    if (target != GL_FRAMEBUFFER_ && target != GL_READ_FRAMEBUFFER_ && target != GL_DRAW_FRAMEBUFFER_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        X(0) = 0;
        return;
    }
    unsigned name = target == GL_READ_FRAMEBUFFER_ ? gl->read_fbo : gl->draw_fbo;
    G2Framebuffer* f = gles2_framebuffer(name);
    X(0) = !name || !f ? GL_FRAMEBUFFER_COMPLETE_ : completeness(f);
}
static void g_get_framebuffer_attachment_parameteriv(CPU* c, Gles2Context* gl) {
    unsigned target = (unsigned)X(0), pname = (unsigned)X(2);
    unsigned name = target == GL_READ_FRAMEBUFFER_ ? gl->read_fbo : gl->draw_fbo;
    G2Framebuffer* f = gles2_framebuffer(name);
    int32_t v = 0;
    if (!name || !f) {
        /* the default framebuffer: the window's */
        if (pname == 0x8cd0) v = 0x8218; /* GL_FRAMEBUFFER_DEFAULT */
        else if (pname >= 0x8212 && pname <= 0x8215) v = 8;
        else if (pname == 0x8216) v = gl->draw ? gl->draw->depth_bits : 0;
        else if (pname == 0x8217) v = gl->draw ? gl->draw->stencil_bits : 0;
        else if (pname == 0x8211) v = 0x8c17; /* GL_UNSIGNED_NORMALIZED */
        memcpy(PTR(3), &v, 4);
        return;
    }
    int points[2];
    if (!attachment_points((unsigned)X(1), points)) {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    const G2Attachment* a = &f->at[points[0]];
    switch (pname) {
    case 0x8cd0: v = a->kind == 1 ? 0x1702 : a->kind == 2 ? GL_RENDERBUFFER_ : 0; break; /* GL_TEXTURE, ... */
    case 0x8cd1: v = (int32_t)a->name; break;
    case 0x8cd2: v = a->level; break;
    case 0x8cd3: v = a->kind == 1 ? (int32_t)GL_TEXTURE_CUBE_MAP_POSITIVE_X_ + a->layer : 0; break;
    case 0x8cd4: v = a->layer; break;
    case 0x8210: v = 0x2601; break; /* GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING: GL_LINEAR */
    case 0x8211: v = 0x8c17; break; /* GL_UNSIGNED_NORMALIZED */
    case 0x8212:
    case 0x8213:
    case 0x8214:
    case 0x8215: v = points[0] < G2_COLORS && a->kind ? 8 : 0; break;
    case 0x8216: v = points[0] == G2_DEPTH_ATTACHMENT && a->kind ? 24 : 0; break;
    case 0x8217: v = points[0] == G2_STENCIL_ATTACHMENT && a->kind ? 8 : 0; break;
    default: gles2_error(gl, GL_INVALID_ENUM_); return;
    }
    memcpy(PTR(3), &v, 4);
}
static void g_draw_buffers(CPU* c, Gles2Context* gl) {
    int n = (int)X(0);
    const unsigned char* bufs = PTR(1);
    if (n < 0 || n > G2_COLORS) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    G2Framebuffer* f = gles2_framebuffer(gl->draw_fbo);
    if (!gl->draw_fbo || !f) return; /* the window's: GL_BACK or GL_NONE, both draw into its one buffer here */
    for (int i = 0; i < G2_COLORS; ++i) {
        uint32_t b = 0;
        if (i < n) memcpy(&b, bufs + 4 * i, 4);
        f->draw[i] = b;
    }
    f->generation++;
    gles2_target_forget();
}
static void g_read_buffer(CPU* c, Gles2Context* gl) {
    G2Framebuffer* f = gles2_framebuffer(gl->read_fbo);
    if (gl->read_fbo && f) f->read = (unsigned)X(0);
}
static void g_invalidate_framebuffer(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl; /* a hint: what it drew stays */
}

/* ---- vertex arrays and attributes ---- */

static void g_gen_vertex_arrays(CPU* c, Gles2Context* gl) {
    gen_call(c, gl, gen_vertex_arrays);
}
static void g_bind_vertex_array(CPU* c, Gles2Context* gl) {
    unsigned name = (unsigned)X(0);
    if (name && (name >= g_gles2.vertex_array_count || !g_gles2.vertex_arrays[name].used)) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    gl->vertex_array = name;
}
static void forget_vertex_array(Gles2Context* gl, unsigned name) {
    if (name >= g_gles2.vertex_array_count || !g_gles2.vertex_arrays[name].used) return;
    g_gles2.vertex_arrays[name] = (G2VertexArray){0};
    if (gl->vertex_array == name) gl->vertex_array = 0;
}
static void g_delete_vertex_arrays(CPU* c, Gles2Context* gl) {
    each_name(c, gl, forget_vertex_array);
}
static void g_is_vertex_array(CPU* c, Gles2Context* gl) {
    (void)gl;
    unsigned name = (unsigned)X(0);
    X(0) = name && name < g_gles2.vertex_array_count && g_gles2.vertex_arrays[name].used;
}
static G2Attrib* attrib(Gles2Context* gl, unsigned index) {
    if (index >= G2_ATTRIBS) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return NULL;
    }
    return &gles2_vertex_array(gl)->attribs[index];
}
static void attrib_pointer(Gles2Context* gl, unsigned index, int size, unsigned type, bool normalized, bool integer,
                           int stride, uint64_t pointer) {
    G2Attrib* a = attrib(gl, index);
    if (!a) return;
    if (size < 1 || size > 4 || stride < 0) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    a->size = size;
    a->type = type;
    a->normalized = normalized;
    a->integer = integer;
    a->stride = stride;
    a->pointer = pointer;
    a->buffer = gl->array_buffer;
}
static void g_vertex_attrib_pointer(CPU* c, Gles2Context* gl) {
    attrib_pointer(gl, (unsigned)X(0), (int)X(1), (unsigned)X(2), (X(3) & 0xff) != 0, false, (int)X(4), X(5));
}
static void g_vertex_attrib_ipointer(CPU* c, Gles2Context* gl) {
    attrib_pointer(gl, (unsigned)X(0), (int)X(1), (unsigned)X(2), false, true, (int)X(3), X(4));
}
static void g_enable_vertex_attrib_array(CPU* c, Gles2Context* gl) {
    G2Attrib* a = attrib(gl, (unsigned)X(0));
    if (a) a->enabled = true;
}
static void g_disable_vertex_attrib_array(CPU* c, Gles2Context* gl) {
    G2Attrib* a = attrib(gl, (unsigned)X(0));
    if (a) a->enabled = false;
}
static void g_vertex_attrib_divisor(CPU* c, Gles2Context* gl) {
    G2Attrib* a = attrib(gl, (unsigned)X(0));
    if (a) a->divisor = (unsigned)X(1);
}
static void generic(Gles2Context* gl, unsigned index, const void* v, int n, uint8_t base) {
    if (index >= G2_ATTRIBS) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    G2Generic* g = &gl->generic[index];
    float one = 1;
    uint32_t defaults[4] = {0, 0, 0, 0};
    if (base == SNX_GLSLC_BASE_FLOAT) memcpy(&defaults[3], &one, 4);
    else defaults[3] = 1;
    memcpy(g->bits, defaults, sizeof defaults);
    memcpy(g->bits, v, (size_t)n * 4);
    g->base = base;
}
static void attrib_f(CPU* c, Gles2Context* gl, int n) {
    float v[4];
    for (int i = 0; i < n; ++i) v[i] = hle_f(c, (unsigned)i);
    generic(gl, (unsigned)X(0), v, n, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib1f(CPU* c, Gles2Context* gl) {
    attrib_f(c, gl, 1);
}
static void g_vertex_attrib2f(CPU* c, Gles2Context* gl) {
    attrib_f(c, gl, 2);
}
static void g_vertex_attrib3f(CPU* c, Gles2Context* gl) {
    attrib_f(c, gl, 3);
}
static void g_vertex_attrib4f(CPU* c, Gles2Context* gl) {
    attrib_f(c, gl, 4);
}
static void g_vertex_attrib1fv(CPU* c, Gles2Context* gl) {
    generic(gl, (unsigned)X(0), PTR(1), 1, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib2fv(CPU* c, Gles2Context* gl) {
    generic(gl, (unsigned)X(0), PTR(1), 2, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib3fv(CPU* c, Gles2Context* gl) {
    generic(gl, (unsigned)X(0), PTR(1), 3, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib4fv(CPU* c, Gles2Context* gl) {
    generic(gl, (unsigned)X(0), PTR(1), 4, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib_i4i(CPU* c, Gles2Context* gl) {
    int32_t v[4] = {(int32_t)X(1), (int32_t)X(2), (int32_t)X(3), (int32_t)X(4)};
    generic(gl, (unsigned)X(0), v, 4, SNX_GLSLC_BASE_INT);
}
static void g_vertex_attrib_i4ui(CPU* c, Gles2Context* gl) {
    uint32_t v[4] = {(uint32_t)X(1), (uint32_t)X(2), (uint32_t)X(3), (uint32_t)X(4)};
    generic(gl, (unsigned)X(0), v, 4, SNX_GLSLC_BASE_UINT);
}
static void g_vertex_attrib_i4iv(CPU* c, Gles2Context* gl) {
    generic(gl, (unsigned)X(0), PTR(1), 4, SNX_GLSLC_BASE_INT);
}
static void g_vertex_attrib_i4uiv(CPU* c, Gles2Context* gl) {
    generic(gl, (unsigned)X(0), PTR(1), 4, SNX_GLSLC_BASE_UINT);
}
/* glGetVertexAttrib*: an attribute's state, or GL_CURRENT_VERTEX_ATTRIB's 4 values */
static int vertex_attrib(Gles2Context* gl, unsigned index, unsigned pname, double v[4]) {
    G2Attrib* a = attrib(gl, index);
    if (!a) return 0;
    switch (pname) {
    case 0x8622: v[0] = a->enabled; return 1;
    case 0x8623: v[0] = a->size; return 1;
    case 0x8624: v[0] = a->stride; return 1;
    case 0x8625: v[0] = a->type; return 1;
    case 0x886a: v[0] = a->normalized; return 1;
    case 0x889f: v[0] = a->buffer; return 1;
    case 0x88fd: v[0] = a->integer; return 1;
    case 0x88fe: v[0] = a->divisor; return 1;
    case 0x8626: {
        const G2Generic* g = &gl->generic[index];
        for (int i = 0; i < 4; ++i) {
            float f;
            memcpy(&f, &g->bits[i], 4);
            v[i] = g->base == SNX_GLSLC_BASE_FLOAT ? f : g->base == SNX_GLSLC_BASE_INT ? (double)(int32_t)g->bits[i]
                                                                                      : g->bits[i];
        }
        return 4;
    }
    default: gles2_error(gl, GL_INVALID_ENUM_); return 0;
    }
}
static void g_get_vertex_attribiv(CPU* c, Gles2Context* gl) {
    double v[4];
    int n = vertex_attrib(gl, (unsigned)X(0), (unsigned)X(1), v);
    for (int i = 0; i < n; ++i) {
        int32_t x = (int32_t)v[i];
        memcpy((unsigned char*)PTR(2) + 4 * i, &x, 4);
    }
}
static void g_get_vertex_attribfv(CPU* c, Gles2Context* gl) {
    double v[4];
    int n = vertex_attrib(gl, (unsigned)X(0), (unsigned)X(1), v);
    for (int i = 0; i < n; ++i) {
        float x = (float)v[i];
        memcpy((unsigned char*)PTR(2) + 4 * i, &x, 4);
    }
}
static void g_get_vertex_attrib_pointerv(CPU* c, Gles2Context* gl) {
    G2Attrib* a = attrib(gl, (unsigned)X(0));
    if (a) memcpy(PTR(2), &a->pointer, 8);
}

/* ---- queries ---- */

static void g_gen_queries(CPU* c, Gles2Context* gl) {
    gen_call(c, gl, gen_queries);
}
static void forget_query(Gles2Context* gl, unsigned name) {
    G2Query* q = query_object(name);
    if (!q) return;
    if (gl->query_any == name) gl->query_any = 0;
    if (q->pool) {
        gles_vk_wait(q->serial);
        V.vk.vkDestroyQueryPool(V.device, q->pool, NULL);
    }
    *q = (G2Query){0};
}
static void g_delete_queries(CPU* c, Gles2Context* gl) {
    each_name(c, gl, forget_query);
}
static void g_is_query(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = query_object((unsigned)X(0)) != NULL;
}
static void g_begin_query(CPU* c, Gles2Context* gl) {
    unsigned target = (unsigned)X(0);
    G2Query* q = query_object((unsigned)X(1));
    if (!q || q->active) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    q->target = target;
    q->active = true;
    q->ready = false;
    q->result = 0;
    q->count = 0;
    if (target == GL_TF_PRIMITIVES_WRITTEN_) return; /* no transform feedback here: 0 primitives */
    if (target != GL_ANY_SAMPLES_PASSED_ && target != GL_ANY_SAMPLES_PASSED_CONSERVATIVE_) {
        gles2_error(gl, GL_INVALID_ENUM_);
        q->active = false;
        return;
    }
    if (!q->pool) {
        q->capacity = 64;
        VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                      .queryType = VK_QUERY_TYPE_OCCLUSION,
                                      .queryCount = q->capacity};
        if (V.vk.vkCreateQueryPool(V.device, &info, NULL, &q->pool) != VK_SUCCESS) {
            q->pool = VK_NULL_HANDLE;
            return;
        }
    }
    gles_vk_wait(q->serial); /* its earlier results are read before the reset */
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (!slot) return;
    V.vk.vkCmdResetQueryPool(slot->cb, q->pool, 0, q->capacity);
    gl->query_any = (unsigned)X(1);
}
void gles2_queries_begin(Gles2Context* gl, VkCommandBuffer cb) {
    G2Query* q = query_object(gl->query_any);
    if (!q || !q->pool || q->count >= q->capacity) return;
    V.vk.vkCmdBeginQuery(cb, q->pool, q->count, 0);
    gl->query_any_target = 1;
}
void gles2_queries_end(Gles2Context* gl, VkCommandBuffer cb) {
    G2Query* q = query_object(gl->query_any);
    if (!q || !gl->query_any_target) return;
    V.vk.vkCmdEndQuery(cb, q->pool, q->count++);
    gl->query_any_target = 0;
}
static void g_end_query(CPU* c, Gles2Context* gl) {
    (void)c;
    G2Query* q = query_object(gl->query_any);
    if (q) {
        gles2_rendering_stop(gl); /* the rendering's query ends with it */
        q->active = false;
        q->serial = gles_vk_pending();
        gl->query_any = 0;
        return;
    }
    for (size_t i = 1; i < g_gles2.query_count; ++i)
        if (g_gles2.queries[i].used && g_gles2.queries[i].active && g_gles2.queries[i].target == (unsigned)X(0)) {
            g_gles2.queries[i].active = false;
            g_gles2.queries[i].ready = true;
        }
}
/* the result of a query: waited for when wait, else only when its submission completed */
static bool query_result(Gles2Context* gl, G2Query* q, bool wait) {
    if (q->ready || !q->pool) return q->ready = true;
    if (q->serial > V.completed) {
        if (!wait) return false;
        if (q->serial > V.submitted && gl->draw) gles_flush(gl->draw);
        gles_vk_wait(q->serial);
    }
    q->result = 0;
    for (uint32_t i = 0; i < q->count; ++i) {
        uint64_t r = 0;
        if (V.vk.vkGetQueryPoolResults(V.device, q->pool, i, 1, 8, &r, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
            q->result |= r != 0;
    }
    q->ready = true;
    return true;
}
static void g_get_query_object_uiv(CPU* c, Gles2Context* gl) {
    G2Query* q = query_object((unsigned)X(0));
    uint32_t v = 0;
    if (!q || q->active) {
        gles2_error(gl, GL_INVALID_OPERATION_);
        return;
    }
    if ((unsigned)X(1) == 0x8867) v = query_result(gl, q, false); /* GL_QUERY_RESULT_AVAILABLE */
    else if ((unsigned)X(1) == 0x8866) v = query_result(gl, q, true) ? (uint32_t)q->result : 0;
    else {
        gles2_error(gl, GL_INVALID_ENUM_);
        return;
    }
    memcpy(PTR(2), &v, 4);
}
static void g_get_queryiv(CPU* c, Gles2Context* gl) {
    int32_t v = 0;
    if ((unsigned)X(1) == 0x8865) /* GL_CURRENT_QUERY */
        for (size_t i = 1; i < g_gles2.query_count; ++i)
            if (g_gles2.queries[i].used && g_gles2.queries[i].active && g_gles2.queries[i].target == (unsigned)X(0))
                v = (int32_t)i;
    (void)gl;
    memcpy(PTR(2), &v, 4);
}

/* ---- sync objects ---- */

static G2Sync* sync_object(uint64_t handle) {
    return handle && handle < g_gles2.sync_count && g_gles2.syncs[handle].used ? &g_gles2.syncs[handle] : NULL;
}
static void g_fence_sync(CPU* c, Gles2Context* gl) {
    (void)gl;
    unsigned name = 1;
    while (name < g_gles2.sync_count && g_gles2.syncs[name].used) ++name;
    G2Sync* s = entry((void**)&g_gles2.syncs, &g_gles2.sync_count, sizeof *s, name);
    X(0) = 0;
    if (!s) return;
    *s = (G2Sync){true, gles_vk_pending()};
    X(0) = name;
}
static void g_delete_sync(CPU* c, Gles2Context* gl) {
    (void)gl;
    G2Sync* s = sync_object(X(0));
    if (s) *s = (G2Sync){0};
}
static void g_is_sync(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = sync_object(X(0)) != NULL;
}
/* glClientWaitSync(sync, flags, timeout): GL_ALREADY_SIGNALED, GL_TIMEOUT_EXPIRED or GL_CONDITION_SATISFIED */
static void g_client_wait_sync(CPU* c, Gles2Context* gl) {
    G2Sync* s = sync_object(X(0));
    uint64_t timeout = X(2);
    if (!s) {
        gles2_error(gl, GL_INVALID_VALUE_);
        X(0) = 0x911d; /* GL_WAIT_FAILED */
        return;
    }
    if (s->serial <= V.completed) {
        X(0) = 0x911a;
        return;
    }
    if (!timeout) {
        X(0) = 0x911b;
        return;
    }
    if (s->serial > V.submitted) {
        gles2_rendering_stop(gl);
        if (gl->draw) gles_flush(gl->draw);
    }
    gles_vk_wait(s->serial);
    X(0) = 0x911c;
}
static void g_wait_sync(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl; /* the GPU's own wait: one queue runs in order */
}
static void g_get_synciv(CPU* c, Gles2Context* gl) {
    G2Sync* s = sync_object(X(0));
    int32_t v = 0;
    if (!s) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    switch ((unsigned)X(1)) {
    case 0x9112: v = 0x9116; break;
    case 0x9113: v = 0x9117; break;
    case 0x9114: v = s->serial <= V.completed ? 0x9119 : 0x9118; break;
    case 0x9115: v = 0; break;
    default: gles2_error(gl, GL_INVALID_ENUM_); return;
    }
    if (X(2) >= 1) memcpy(PTR(4), &v, 4);
    if (X(3)) {
        int32_t one = 1;
        memcpy(PTR(3), &one, 4);
    }
}

/* ---- transform feedback objects: names alone (no transform feedback is drawn yet) ---- */

static void g_gen_transform_feedbacks(CPU* c, Gles2Context* gl) {
    int n = (int)X(0);
    for (int i = 0; i < n; ++i) {
        uint32_t v = (uint32_t)i + 1;
        memcpy((unsigned char*)PTR(1) + 4 * i, &v, 4);
    }
    (void)gl;
}
static void g_transform_feedback(CPU* c, Gles2Context* gl) {
    (void)c;
    gles2_warn(7, "transform feedback is not drawn yet");
    (void)gl;
}

static void g_nothing_object(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl;
}

/* glGetInternalformativ(target, internal, pname, size, params): the sample counts */
static void g_get_internalformativ(CPU* c, Gles2Context* gl) {
    (void)gl;
    int32_t size = (int32_t)X(3);
    unsigned pname = (unsigned)X(2);
    int32_t counts[2] = {4, 2};
    if (pname == 0x9380) { /* GL_NUM_SAMPLE_COUNTS */
        int32_t n = 2;
        if (size > 0) memcpy(PTR(4), &n, 4);
    } else if (pname == 0x80a9) { /* GL_SAMPLES */
        for (int i = 0; i < 2 && i < size; ++i) memcpy((unsigned char*)PTR(4) + 4 * i, &counts[i], 4);
    }
}

const HleSymbol hle_gles2_object[] = {
    FN("glGenBuffers", g_gen_buffers),
    FN("glDeleteBuffers", g_delete_buffers),
    FN("glIsBuffer", g_is_buffer),
    FN("glBindBuffer", g_bind_buffer),
    FN("glBindBufferBase", g_bind_buffer_base),
    FN("glBindBufferRange", g_bind_buffer_range),
    FN("glBufferData", g_buffer_data),
    FN("glBufferSubData", g_buffer_sub_data),
    FN("glMapBufferRange", g_map_buffer_range),
    FN("glMapBufferRangeEXT", g_map_buffer_range),
    FN("glMapBufferOES", g_map_buffer),
    FN("glUnmapBuffer", g_unmap_buffer),
    FN("glUnmapBufferOES", g_unmap_buffer),
    FN("glFlushMappedBufferRange", g_flush_mapped_range),
    FN("glFlushMappedBufferRangeEXT", g_flush_mapped_range),
    FN("glGetBufferPointerv", g_get_buffer_pointerv),
    FN("glGetBufferPointervOES", g_get_buffer_pointerv),
    FN("glCopyBufferSubData", g_copy_buffer_sub_data),
    FN("glGetBufferParameteriv", g_get_buffer_parameteriv),
    FN("glGetBufferParameteri64v", g_get_buffer_parameteri64v),
    FN("glGenTextures", g_gen_textures),
    FN("glDeleteTextures", g_delete_textures),
    FN("glIsTexture", g_is_texture),
    FN("glBindTexture", g_bind_texture),
    FN("glActiveTexture", g_active_texture),
    FN("glTexImage2D", g_tex_image_2d),
    FN("glTexImage3D", g_tex_image_3d),
    FN("glTexImage3DOES", g_tex_image_3d),
    FN("glTexSubImage2D", g_tex_sub_image_2d),
    FN("glTexSubImage3D", g_tex_sub_image_3d),
    FN("glTexSubImage3DOES", g_tex_sub_image_3d),
    FN("glCompressedTexImage2D", g_compressed_tex_image_2d),
    FN("glCompressedTexImage3D", g_compressed_tex_image_3d),
    FN("glCompressedTexSubImage2D", g_compressed_tex_sub_image_2d),
    FN("glCompressedTexSubImage3D", g_compressed_tex_sub_image_3d),
    FN("glTexStorage2D", g_tex_storage_2d),
    FN("glTexStorage2DEXT", g_tex_storage_2d),
    FN("glTexStorage3D", g_tex_storage_3d),
    FN("glTexStorage3DEXT", g_tex_storage_3d),
    FN("glTexParameteri", g_tex_parameteri),
    FN("glTexParameterf", g_tex_parameterf),
    FN("glTexParameteriv", g_tex_parameteriv),
    FN("glTexParameterfv", g_tex_parameterfv),
    FN("glGetTexParameteriv", g_get_tex_parameteriv),
    FN("glGetTexParameterfv", g_get_tex_parameterfv),
    FN("glGenerateMipmap", g_generate_mipmap),
    FN("glGenSamplers", g_gen_samplers),
    FN("glDeleteSamplers", g_delete_samplers),
    FN("glIsSampler", g_is_sampler),
    FN("glBindSampler", g_bind_sampler),
    FN("glSamplerParameteri", g_sampler_parameteri),
    FN("glSamplerParameterf", g_sampler_parameterf),
    FN("glSamplerParameteriv", g_sampler_parameteriv),
    FN("glSamplerParameterfv", g_sampler_parameterfv),
    FN("glGetSamplerParameteriv", g_get_sampler_parameteriv),
    FN("glGetSamplerParameterfv", g_get_sampler_parameterfv),
    FN("glGenRenderbuffers", g_gen_renderbuffers),
    FN("glDeleteRenderbuffers", g_delete_renderbuffers),
    FN("glIsRenderbuffer", g_is_renderbuffer),
    FN("glBindRenderbuffer", g_bind_renderbuffer),
    FN("glRenderbufferStorage", g_renderbuffer_storage),
    FN("glRenderbufferStorageMultisample", g_renderbuffer_storage_multisample),
    FN("glRenderbufferStorageMultisampleEXT", g_renderbuffer_storage_multisample),
    FN("glGetRenderbufferParameteriv", g_get_renderbuffer_parameteriv),
    FN("glGenFramebuffers", g_gen_framebuffers),
    FN("glDeleteFramebuffers", g_delete_framebuffers),
    FN("glIsFramebuffer", g_is_framebuffer),
    FN("glBindFramebuffer", g_bind_framebuffer),
    FN("glFramebufferTexture2D", g_framebuffer_texture_2d),
    FN("glFramebufferTextureLayer", g_framebuffer_texture_layer),
    FN("glFramebufferRenderbuffer", g_framebuffer_renderbuffer),
    FN("glCheckFramebufferStatus", g_check_framebuffer_status),
    FN("glGetFramebufferAttachmentParameteriv", g_get_framebuffer_attachment_parameteriv),
    FN("glDrawBuffers", g_draw_buffers),
    FN("glDrawBuffersEXT", g_draw_buffers),
    FN("glReadBuffer", g_read_buffer),
    FN("glInvalidateFramebuffer", g_invalidate_framebuffer),
    FN("glInvalidateSubFramebuffer", g_invalidate_framebuffer),
    FN("glDiscardFramebufferEXT", g_invalidate_framebuffer),
    FN("glGenVertexArrays", g_gen_vertex_arrays),
    FN("glGenVertexArraysOES", g_gen_vertex_arrays),
    FN("glBindVertexArray", g_bind_vertex_array),
    FN("glBindVertexArrayOES", g_bind_vertex_array),
    FN("glDeleteVertexArrays", g_delete_vertex_arrays),
    FN("glDeleteVertexArraysOES", g_delete_vertex_arrays),
    FN("glIsVertexArray", g_is_vertex_array),
    FN("glIsVertexArrayOES", g_is_vertex_array),
    FN("glVertexAttribPointer", g_vertex_attrib_pointer),
    FN("glVertexAttribIPointer", g_vertex_attrib_ipointer),
    FN("glEnableVertexAttribArray", g_enable_vertex_attrib_array),
    FN("glDisableVertexAttribArray", g_disable_vertex_attrib_array),
    FN("glVertexAttribDivisor", g_vertex_attrib_divisor),
    FN("glVertexAttribDivisorEXT", g_vertex_attrib_divisor),
    FN("glVertexAttribDivisorANGLE", g_vertex_attrib_divisor),
    FN("glVertexAttrib1f", g_vertex_attrib1f),
    FN("glVertexAttrib2f", g_vertex_attrib2f),
    FN("glVertexAttrib3f", g_vertex_attrib3f),
    FN("glVertexAttrib4f", g_vertex_attrib4f),
    FN("glVertexAttrib1fv", g_vertex_attrib1fv),
    FN("glVertexAttrib2fv", g_vertex_attrib2fv),
    FN("glVertexAttrib3fv", g_vertex_attrib3fv),
    FN("glVertexAttrib4fv", g_vertex_attrib4fv),
    FN("glVertexAttribI4i", g_vertex_attrib_i4i),
    FN("glVertexAttribI4ui", g_vertex_attrib_i4ui),
    FN("glVertexAttribI4iv", g_vertex_attrib_i4iv),
    FN("glVertexAttribI4uiv", g_vertex_attrib_i4uiv),
    FN("glGetVertexAttribiv", g_get_vertex_attribiv),
    FN("glGetVertexAttribIiv", g_get_vertex_attribiv),
    FN("glGetVertexAttribIuiv", g_get_vertex_attribiv),
    FN("glGetVertexAttribfv", g_get_vertex_attribfv),
    FN("glGetVertexAttribPointerv", g_get_vertex_attrib_pointerv),
    FN("glGenQueries", g_gen_queries),
    FN("glGenQueriesEXT", g_gen_queries),
    FN("glDeleteQueries", g_delete_queries),
    FN("glDeleteQueriesEXT", g_delete_queries),
    FN("glIsQuery", g_is_query),
    FN("glIsQueryEXT", g_is_query),
    FN("glBeginQuery", g_begin_query),
    FN("glBeginQueryEXT", g_begin_query),
    FN("glEndQuery", g_end_query),
    FN("glEndQueryEXT", g_end_query),
    FN("glGetQueryiv", g_get_queryiv),
    FN("glGetQueryivEXT", g_get_queryiv),
    FN("glGetQueryObjectuiv", g_get_query_object_uiv),
    FN("glGetQueryObjectuivEXT", g_get_query_object_uiv),
    FN("glFenceSync", g_fence_sync),
    FN("glDeleteSync", g_delete_sync),
    FN("glIsSync", g_is_sync),
    FN("glClientWaitSync", g_client_wait_sync),
    FN("glWaitSync", g_wait_sync),
    FN("glGetSynciv", g_get_synciv),
    FN("glGenTransformFeedbacks", g_gen_transform_feedbacks),
    FN("glBeginTransformFeedback", g_transform_feedback),
    FN("glEndTransformFeedback", g_nothing_object),
    FN("glPauseTransformFeedback", g_nothing_object),
    FN("glResumeTransformFeedback", g_nothing_object),
    FN("glBindTransformFeedback", g_nothing_object),
    FN("glDeleteTransformFeedbacks", g_nothing_object),
    FN("glGetInternalformativ", g_get_internalformativ),
    {"", NULL, NULL},
};
