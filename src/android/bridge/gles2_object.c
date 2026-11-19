/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* GLES 2 and 3's objects (gles2.h) as Vulkan's.

   A buffer is the shared GL layer's (common/gpu/gl/gl_object.h), its bytes in chunks of host memory the layer places
   them in, which the CPU writes and reads. A draw reads its Vulkan copy here, which the bytes written since go into
   first: in place when no submission in flight reads it, else as a copy in the frame's arena. Only a buffer the GPU writes (glReadPixels into a pack buffer) moves to host-cached
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

GEN(queries, query_count, queries, G2Query, (void)0)
#undef GEN

static G2Query* query_object(unsigned name) {
    return name && name < g_gles2.query_count && g_gles2.queries[name].used ? &g_gles2.queries[name] : NULL;
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

/* ---- buffers: the shared layer's (common/gpu/gl/gl_object.h), and each one's Vulkan copy here ---- */

G2Buffer* gles2_buffer(unsigned name) {
    const SnxGlBuffer* x = snx_gl_buffer(name);
    if (!x) return NULL;
    G2Buffer* b = &g_gles2.buffer_vk[name - 1];
    b->data = (unsigned char*)(uintptr_t)x->data;
    b->size = (size_t)x->size;
    return b;
}

static void buffer_vk_free(G2Buffer* b) {
    if (b->buffer) gles_vk_retire((GlesGarbage){.buffer = b->buffer, .memory = b->memory});
    b->buffer = VK_NULL_HANDLE;
    b->memory = VK_NULL_HANDLE;
    b->map = NULL;
    b->capacity = 0;
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

/* the bytes the GPU wrote into the Vulkan copy, brought into data */
static void buffer_read_back(G2Buffer* b) {
    if (!b->gpu_newer) return;
    Gles2Context* gl = gles2_current();
    if (b->serial > V.submitted && gl && gl->draw) {
        /* the frame being recorded writes it: its commands go to the GPU now */
        gles2_rendering_stop(gl);
        gles_flush(gl->draw);
    }
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

/* what the shared layer asks and tells of its buffers: their bytes live in chunks of host memory, as the guest's
   memory is the host's */
static uint64_t buffer_chunk(uint64_t size, uint64_t* got) {
    void* p = calloc(1, (size_t)size);
    *got = p ? size : 0;
    return (uint64_t)(uintptr_t)p;
}
static uint8_t* buffer_host(uint64_t at) { return (uint8_t*)(uintptr_t)at; }
static void buffer_read_back_name(uint32_t name) {
    G2Buffer* b = gles2_buffer(name);
    if (b) buffer_read_back(b);
}
static void buffer_written(uint32_t name, uint64_t lo, uint64_t hi) {
    G2Buffer* b = gles2_buffer(name);
    if (!b) return;
    if (!lo && hi >= b->size) { /* all of it, as glBufferData writes: newer than the GPU's writes too */
        b->stale = true;
        b->lo = b->hi = 0;
        b->gpu_newer = false;
    } else {
        buffer_dirty(b, (size_t)lo, (size_t)hi);
    }
    b->arena_serial = 0;
}
static void buffer_gone(uint32_t name) {
    G2Buffer* b = &g_gles2.buffer_vk[name - 1];
    buffer_vk_free(b);
    *b = (G2Buffer){0};
}
void gles2_buffers_init(void) {
    snx_gl_object_host(&(SnxGlObjectHost){.chunk = buffer_chunk,
                                          .bytes = buffer_host,
                                          .read_back = buffer_read_back_name,
                                          .written = buffer_written,
                                          .gone = buffer_gone});
}

/* the buffer bound to target: 0 with GL's error recorded */
static uint32_t bound(Gles2Context* gl, uint64_t target) { return snx_gl_bound_buffer(gl->ctx, (uint32_t)target); }
static void g_gen_buffers(CPU* c, Gles2Context* gl) { snx_gl_gen_buffers(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_delete_buffers(CPU* c, Gles2Context* gl) { snx_gl_delete_buffers(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_is_buffer(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_buffer((uint32_t)X(0));
}
static void g_bind_buffer(CPU* c, Gles2Context* gl) { snx_gl_bind_buffer(gl->ctx, (uint32_t)X(0), (uint32_t)X(1)); }
static void g_bind_buffer_base(CPU* c, Gles2Context* gl) {
    snx_gl_bind_buffer_range(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), 0, -1);
}
static void g_bind_buffer_range(CPU* c, Gles2Context* gl) {
    snx_gl_bind_buffer_range(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), (int64_t)X(3), (int64_t)X(4));
}
/* glBufferData(target, size, data, usage) */
static void g_buffer_data(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    if (b) snx_gl_buffer_data(gl->ctx, b, (int64_t)X(1), PTR(2), (uint32_t)X(3));
}
static void g_buffer_sub_data(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    if (b) snx_gl_buffer_sub_data(gl->ctx, b, (int64_t)X(1), (int64_t)X(2), PTR(3));
}
/* glMapBufferRange(target, offset, length, access): the bytes themselves, which the guest's pointers reach */
static void g_map_buffer_range(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    X(0) = b ? snx_gl_map_buffer_range(gl->ctx, b, (int64_t)X(1), (int64_t)X(2), (uint32_t)X(3)) : 0;
}
/* glMapBufferOES(target, access): the whole buffer, for writing */
static void g_map_buffer(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    const SnxGlBuffer* x = snx_gl_buffer(b);
    X(0) = x ? snx_gl_map_buffer_range(gl->ctx, b, 0, (int64_t)x->size, GL_MAP_WRITE_BIT_) : 0;
}
static void g_unmap_buffer(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    X(0) = b ? (uint64_t)snx_gl_unmap_buffer(gl->ctx, b) : 0;
}
static void g_flush_mapped_range(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    if (b) snx_gl_flush_mapped_range(gl->ctx, b, (int64_t)X(1), (int64_t)X(2));
}
static void g_get_buffer_pointerv(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    uint64_t p = b ? snx_gl_buffer_pointer(gl->ctx, b) : 0;
    memcpy(PTR(2), &p, 8);
}
/* glCopyBufferSubData(read, write, read offset, write offset, size): the bytes the CPU has */
static void g_copy_buffer_sub_data(CPU* c, Gles2Context* gl) {
    uint32_t r = bound(gl, X(0)), w = bound(gl, X(1));
    if (r && w) snx_gl_copy_buffer_sub_data(gl->ctx, r, w, (int64_t)X(2), (int64_t)X(3), (int64_t)X(4));
}
static void g_get_buffer_parameteriv(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    int64_t v = 0;
    if (!b) return;
    snx_gl_buffer_parameter(gl->ctx, b, (uint32_t)X(1), &v);
    int32_t x = (int32_t)v;
    memcpy(PTR(2), &x, 4);
}
static void g_get_buffer_parameteri64v(CPU* c, Gles2Context* gl) {
    uint32_t b = bound(gl, X(0));
    int64_t v = 0;
    if (!b) return;
    snx_gl_buffer_parameter(gl->ctx, b, (uint32_t)X(1), &v);
    memcpy(PTR(2), &v, 8);
}

/* ---- formats ---- */

#define SW(r, g, b, a)                                                                                                 \
    { VK_COMPONENT_SWIZZLE_##r, VK_COMPONENT_SWIZZLE_##g, VK_COMPONENT_SWIZZLE_##b, VK_COMPONENT_SWIZZLE_##a }
#define ID SW(IDENTITY, IDENTITY, IDENTITY, IDENTITY)
#define BGRS SW(B, G, R, IDENTITY)
/* each format's Vulkan forms, in common/gpu/gl/gl_format.c's order: the format, another with red and blue apart or 0,
   and the swizzles of their views (luminance and alpha) */
typedef struct {
    VkFormat vk, alt;
    VkComponentMapping swizzle, alt_swizzle;
} G2VkFormat;
static const G2VkFormat vk_formats[] = {
    /* GLES 2's unsized formats, as their format and type */
    {VK_FORMAT_R8G8B8A8_UNORM, 0, ID, ID},
    {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, SW(IDENTITY, IDENTITY, IDENTITY, ONE),
     SW(IDENTITY, IDENTITY, IDENTITY, ONE)},
    {VK_FORMAT_R4G4B4A4_UNORM_PACK16, VK_FORMAT_B4G4R4A4_UNORM_PACK16, ID, BGRS},
    {VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_FORMAT_B5G5R5A1_UNORM_PACK16, ID, BGRS},
    {VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_B5G6R5_UNORM_PACK16, SW(IDENTITY, IDENTITY, IDENTITY, ONE),
     SW(B, G, R, ONE)},
    {VK_FORMAT_R8_UNORM, 0, SW(R, R, R, ONE), ID},
    {VK_FORMAT_R8_UNORM, 0, SW(ZERO, ZERO, ZERO, R), ID},
    {VK_FORMAT_R8G8_UNORM, 0, SW(R, R, R, G), ID},
    {VK_FORMAT_B8G8R8A8_UNORM, 0, ID, ID},
    {VK_FORMAT_R16G16B16A16_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R16G16B16A16_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R32G32B32A32_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, SW(IDENTITY, IDENTITY, IDENTITY, ONE),
     SW(IDENTITY, IDENTITY, IDENTITY, ONE)},
    {VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, SW(IDENTITY, IDENTITY, IDENTITY, ONE),
     SW(IDENTITY, IDENTITY, IDENTITY, ONE)},
    {VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, SW(IDENTITY, IDENTITY, IDENTITY, ONE),
     SW(IDENTITY, IDENTITY, IDENTITY, ONE)},
    {VK_FORMAT_R16_SFLOAT, 0, SW(R, R, R, ONE), ID},
    {VK_FORMAT_R32_SFLOAT, 0, SW(R, R, R, ONE), ID},
    {VK_FORMAT_R16_SFLOAT, 0, SW(ZERO, ZERO, ZERO, R), ID},
    {VK_FORMAT_R32_SFLOAT, 0, SW(ZERO, ZERO, ZERO, R), ID},
    {VK_FORMAT_R16G16_SFLOAT, 0, SW(R, R, R, G), ID},
    {VK_FORMAT_R32G32_SFLOAT, 0, SW(R, R, R, G), ID},
    {VK_FORMAT_R8_UNORM, 0, ID, ID},
    {VK_FORMAT_R8G8_UNORM, 0, ID, ID},
    {VK_FORMAT_D16_UNORM, 0, ID, ID},
    {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, ID, ID},
    {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, ID, ID},
    /* GLES 3's sized formats */
    {VK_FORMAT_R8_UNORM, 0, ID, ID},
    {VK_FORMAT_R8_SNORM, 0, ID, ID},
    {VK_FORMAT_R16_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R32_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R8_UINT, 0, ID, ID},
    {VK_FORMAT_R8_SINT, 0, ID, ID},
    {VK_FORMAT_R16_UINT, 0, ID, ID},
    {VK_FORMAT_R16_SINT, 0, ID, ID},
    {VK_FORMAT_R32_UINT, 0, ID, ID},
    {VK_FORMAT_R32_SINT, 0, ID, ID},
    {VK_FORMAT_R8G8_UNORM, 0, ID, ID},
    {VK_FORMAT_R8G8_SNORM, 0, ID, ID},
    {VK_FORMAT_R16G16_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R32G32_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R8G8_UINT, 0, ID, ID},
    {VK_FORMAT_R8G8_SINT, 0, ID, ID},
    {VK_FORMAT_R16G16_UINT, 0, ID, ID},
    {VK_FORMAT_R16G16_SINT, 0, ID, ID},
    {VK_FORMAT_R32G32_UINT, 0, ID, ID},
    {VK_FORMAT_R32G32_SINT, 0, ID, ID},
    {VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, ID, ID},
    {VK_FORMAT_R8G8B8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, ID, ID},
    {VK_FORMAT_R5G6B5_UNORM_PACK16, VK_FORMAT_B5G6R5_UNORM_PACK16, ID, SW(B, G, R, ONE)},
    {VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM, ID, ID},
    {VK_FORMAT_B10G11R11_UFLOAT_PACK32, 0, ID, ID},
    {VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, 0, ID, ID},
    {VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, ID, ID},
    {VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, ID, ID},
    {VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT, ID, ID},
    {VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT, ID, ID},
    {VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT, ID, ID},
    {VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT, ID, ID},
    {VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT, ID, ID},
    {VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT, ID, ID},
    {VK_FORMAT_R8G8B8A8_UNORM, 0, ID, ID},
    {VK_FORMAT_R8G8B8A8_SRGB, 0, ID, ID},
    {VK_FORMAT_R8G8B8A8_SNORM, 0, ID, ID},
    {VK_FORMAT_R5G5B5A1_UNORM_PACK16, VK_FORMAT_B5G5R5A1_UNORM_PACK16, ID, BGRS},
    {VK_FORMAT_R4G4B4A4_UNORM_PACK16, VK_FORMAT_B4G4R4A4_UNORM_PACK16, ID, BGRS},
    {VK_FORMAT_A2B10G10R10_UNORM_PACK32, 0, ID, ID},
    {VK_FORMAT_R16G16B16A16_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R32G32B32A32_SFLOAT, 0, ID, ID},
    {VK_FORMAT_R8G8B8A8_UINT, 0, ID, ID},
    {VK_FORMAT_R8G8B8A8_SINT, 0, ID, ID},
    {VK_FORMAT_A2B10G10R10_UINT_PACK32, 0, ID, ID},
    {VK_FORMAT_R16G16B16A16_UINT, 0, ID, ID},
    {VK_FORMAT_R16G16B16A16_SINT, 0, ID, ID},
    {VK_FORMAT_R32G32B32A32_UINT, 0, ID, ID},
    {VK_FORMAT_R32G32B32A32_SINT, 0, ID, ID},
    {VK_FORMAT_B8G8R8A8_UNORM, 0, ID, ID},
    {VK_FORMAT_D16_UNORM, 0, ID, ID},
    {VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D32_SFLOAT, ID, ID},
    {VK_FORMAT_D32_SFLOAT, 0, ID, ID},
    {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT, ID, ID},
    {VK_FORMAT_D32_SFLOAT_S8_UINT, 0, ID, ID},
    {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, ID, ID},
    /* compressed: the device's own formats, the bytes of a block, its width and height */
    {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, 0, ID, ID},
    {VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK, 0, ID, ID},
    {VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, 0, ID, ID},
    {VK_FORMAT_EAC_R11_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_EAC_R11_SNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_EAC_R11G11_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_EAC_R11G11_SNORM_BLOCK, 0, ID, ID},
    /* BCn, where the device has it */
    {VK_FORMAT_BC1_RGB_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC2_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC3_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC1_RGB_SRGB_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC2_SRGB_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC3_SRGB_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC4_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC4_SNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC5_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC5_SNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC7_UNORM_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC7_SRGB_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC6H_SFLOAT_BLOCK, 0, ID, ID},
    {VK_FORMAT_BC6H_UFLOAT_BLOCK, 0, ID, ID},
};
typedef char g2_vk_formats_counted[sizeof vk_formats / sizeof *vk_formats == SNX_GL_TABLE_FORMATS ? 1 : -1];
#undef SW
#undef ID
#undef BGRS

const G2Format* gles2_format(unsigned internal, unsigned format, unsigned type) {
    return gles2_format_at(snx_gl_format(internal, format, type));
}
const G2Format* gles2_format_at(int i) {
    const SnxGlFormat* gl = snx_gl_format_at(i);
    G2Format* f;
    if (!gl) return NULL;
    f = &g_gles2.formats[i];
    if (!f->internal) { /* the shared layer's fields, and this file's Vulkan forms */
        *f = (G2Format){gl->internal, gl->format, gl->type, 0, 0, {0}, {0}, gl->bytes, gl->expand, gl->kind, gl->block};
        if (i < SNX_GL_TABLE_FORMATS) {
            f->vk = vk_formats[i].vk, f->alt = vk_formats[i].alt;
            f->swizzle = vk_formats[i].swizzle, f->alt_swizzle = vk_formats[i].alt_swizzle;
        } else { /* ASTC's, in GL's order */
            f->vk = (VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * (gl->internal & 0xf) + (gl->internal >= 0x93d0));
        }
    }
    return f;
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

/* ---- textures and samplers: the shared layer's (common/gpu/gl/gl_texture.h), and each texture's image here ---- */

/* a texture's image, with the shared layer's record copied in now: NULL for 0 or a name not made */
G2Texture* gles2_texture(unsigned name) {
    const SnxGlTexture* x = snx_gl_texture(name);
    if (!x) return NULL;
    G2Texture* t = &g_gles2.texture_vk[name - 1];
    t->used = true;
    t->target = x->target;
    t->format = gles2_format_at(x->format);
    t->width = x->width, t->height = x->height, t->depth = x->depth;
    t->levels = x->levels, t->layers = x->layers;
    t->samples = x->samples > 1 ? (VkSampleCountFlagBits)x->samples : VK_SAMPLE_COUNT_1_BIT;
    t->defined = x->defined;
    t->immutable = x->immutable;
    t->sampler = x->sampler;
    t->base_level = x->base_level, t->max_level = x->max_level;
    for (int i = 0; i < 4; ++i) t->swizzles[i] = x->swizzles[i];
    t->generation = x->generation;
    t->stencil_mode = x->stencil_mode;
    return t;
}
static unsigned texture_name(const G2Texture* t) { return (unsigned)(t - g_gles2.texture_vk) + 1; }
G2Texture* gles2_unit_texture(Gles2Context* gl, unsigned unit, unsigned target) {
    return gles2_texture(snx_gl_unit_texture(gl->ctx, unit, target));
}
void gles2_texture_defined(G2Texture* t, int level) {
    snx_gl_texture_defined(texture_name(t), level);
    t->defined = snx_gl_texture(texture_name(t))->defined;
}
/* the current context, when the shared layer's call is its: what a transfer is recorded for */
static Gles2Context* texture_context(uint32_t ctx) {
    Gles2Context* gl = gles2_current();
    return gl && gl->ctx == ctx ? gl : NULL;
}

static void views_forget(G2Texture* t) {
    for (uint32_t i = 0; i < t->view_count; ++i) gles_vk_retire((GlesGarbage){.view = t->views[i].view});
    t->view_count = 0;
}
static void texture_free(G2Texture* t) {
    views_forget(t);
    if (t->image.image) gles_vk_retire((GlesGarbage){.image = t->image.image, .memory = t->image.memory});
    t->image = (GlesImage){0};
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

/* the shared layer made the texture's storage anew: its image, of the format the device samples, cleared unless
   compressed (GL leaves the texels undefined: zeros) and at its rest */
static int texture_storage(uint32_t ctx, uint32_t name) {
    Gles2Context* gl = texture_context(ctx);
    G2Texture* t = gles2_texture(name);
    if (!t || !t->format) return 0;
    const G2Format* f = t->format;
    texture_free(t);
    bool expanded;
    VkComponentMapping swizzle;
    VkFormat vk = pick(f, needs(f, true), &expanded, &swizzle);
    if (!vk) {
        gles2_warn(0, "a texture format the device does not sample: the texture stays empty");
        return 0;
    }
    VkFormatProperties p;
    V.vk.vkGetPhysicalDeviceFormatProperties(V.physical, vk, &p);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)
        usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    VkSampleCountFlagBits samples = t->target == G2_TMS ? t->samples : VK_SAMPLE_COUNT_1_BIT;
    if (samples > 1) usage &= ~VK_IMAGE_USAGE_STORAGE_BIT;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .flags = (t->target == G2_TCUBE ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
                                       (t->target == G2_T3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
                              .imageType = t->target == G2_T3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                              .format = vk,
                              .extent = {(uint32_t)t->width, (uint32_t)t->height, (uint32_t)t->depth},
                              .mipLevels = (uint32_t)t->levels,
                              .arrayLayers = (uint32_t)t->layers,
                              .samples = samples,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = usage};
    if (!gles_vk_image(&info, VK_IMAGE_VIEW_TYPE_MAX_ENUM, 0, &t->image)) {
        gles2_error(gl, GL_OUT_OF_MEMORY_);
        return 0;
    }
    (void)expanded;
    t->vk = vk;
    t->swizzle = swizzle;
    VkImageAspectFlags aspect = gles2_aspect(f);
    t->layout = gles2_rest_layout(f, true);
    GlesSlot* slot = gl ? gles2_transfer_slot(gl) : NULL;
    if (slot && f->block) { /* Vulkan clears no compressed image: its blocks stay undefined, as GL leaves them */
        level_barrier(slot->cb, t->image.image, aspect, 0, t->levels, VK_IMAGE_LAYOUT_UNDEFINED, t->layout, 0,
                      VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, READERS);
    } else if (slot) {
        level_barrier(slot->cb, t->image.image, aspect, 0, t->levels, VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageSubresourceRange range = {aspect, 0, (uint32_t)t->levels, 0, (uint32_t)t->layers};
        if (aspect == VK_IMAGE_ASPECT_COLOR_BIT) {
            VkClearColorValue zero = {{0}};
            V.vk.vkCmdClearColorImage(slot->cb, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        } else {
            VkClearDepthStencilValue one = {1, 0};
            V.vk.vkCmdClearDepthStencilImage(slot->cb, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1,
                                             &range);
        }
        level_barrier(slot->cb, t->image.image, aspect, 0, t->levels, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->layout,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, READERS);
    }
    gles2_target_forget();
    return 1;
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

/* the client's texels of a box of a level, which the shared layer found: copied to staging as whole rows, expanded on
   the GPU when the image has 4 components for 3, copied into the image, which returns to its rest */
static void texture_upload(uint32_t ctx, uint32_t name, const SnxGlUpload* u) {
    Gles2Context* gl = texture_context(ctx);
    G2Texture* t = gles2_texture(name);
    if (!gl || !t || !t->image.image) return;
    const G2Format* f = t->format;
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (!slot) return;
    VkDeviceSize size = (VkDeviceSize)u->row * u->rows * (VkDeviceSize)u->d;
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
    if (u->stride == u->row && (u->d == 1 || u->image == u->row * u->rows))
        memcpy(dst, u->src, (size_t)size);
    else
        for (int k = 0; k < u->d; ++k)
            for (size_t r = 0; r < u->rows; ++r)
                memcpy(dst + ((size_t)k * u->rows + r) * u->row, u->src + (size_t)k * u->image + r * u->stride, u->row);
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
    if (expanding && !expand_texels(slot, f, (uint32_t)((size_t)u->w * (size_t)u->h * (size_t)u->d), &at, &size))
        return;
    int level = u->level;
    level_barrier(cb, t->image.image, aspect, level, 1, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, READERS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    bool layered = t->target == G2_TCUBE || t->target == G2_TARRAY;
    VkBufferImageCopy copy = {
        .bufferOffset = at,
        .imageSubresource = {aspect, (uint32_t)level, layered ? (uint32_t)u->z : 0, layered ? (uint32_t)u->d : 1},
        .imageOffset = {u->x, u->y, layered ? 0 : u->z},
        .imageExtent = {(uint32_t)u->w, (uint32_t)u->h, layered ? 1 : (uint32_t)u->d}};
    V.vk.vkCmdCopyBufferToImage(cb, slot->staging.buffer, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    level_barrier(cb, t->image.image, aspect, level, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->layout,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, READERS);
}

/* glGenerateMipmap: each level blitted from the one above it, on the GPU */
static int texture_mipmaps(uint32_t ctx, uint32_t name) {
    Gles2Context* gl = texture_context(ctx);
    G2Texture* t = gles2_texture(name);
    if (!gl || !t || !t->image.image) return 0;
    int base = t->base_level;
    VkFormatProperties p;
    V.vk.vkGetPhysicalDeviceFormatProperties(V.physical, t->vk, &p);
    VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((p.optimalTilingFeatures & blit) != blit) {
        gles2_warn(5, "glGenerateMipmap of a format the device does not blit: the levels stay as they are");
        return 0;
    }
    bool linear = (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (!slot) return 0;
    VkCommandBuffer cb = slot->cb;
    VkImageAspectFlags aspect = gles2_aspect(t->format);
    level_barrier(cb, t->image.image, aspect, base, 1, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT, READERS, VK_PIPELINE_STAGE_TRANSFER_BIT);
    for (int l = base + 1; l < t->levels; ++l) {
        level_barrier(cb, t->image.image, aspect, l, 1, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, READERS, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit region = {.srcSubresource = {aspect, (uint32_t)(l - 1), 0, (uint32_t)t->layers},
                              .srcOffsets = {{0, 0, 0},
                                             {snx_gl_level_size(t->width, l - 1), snx_gl_level_size(t->height, l - 1),
                                              snx_gl_level_size(t->depth, l - 1)}},
                              .dstSubresource = {aspect, (uint32_t)l, 0, (uint32_t)t->layers},
                              .dstOffsets = {{0, 0, 0},
                                             {snx_gl_level_size(t->width, l), snx_gl_level_size(t->height, l),
                                              snx_gl_level_size(t->depth, l)}}};
        V.vk.vkCmdBlitImage(cb, t->image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->image.image,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region,
                            linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        level_barrier(cb, t->image.image, aspect, l, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    level_barrier(cb, t->image.image, aspect, base, t->levels - base, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->layout,
                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, READERS);
    return 1;
}

/* glBindImageTexture: the texture rests in GENERAL from now, which storage images take, and sampling too */
static void texture_image_bound(uint32_t ctx, uint32_t name) {
    Gles2Context* gl = texture_context(ctx);
    G2Texture* t = gles2_texture(name);
    if (!gl || !t || !t->image.image || t->layout == VK_IMAGE_LAYOUT_GENERAL) return;
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (!slot) return;
    level_barrier(slot->cb, t->image.image, gles2_aspect(t->format), 0, t->levels, t->layout, VK_IMAGE_LAYOUT_GENERAL,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, READERS | VK_PIPELINE_STAGE_TRANSFER_BIT,
                  READERS);
    t->layout = VK_IMAGE_LAYOUT_GENERAL;
}
static void texture_gone(uint32_t name) {
    G2Texture* t = &g_gles2.texture_vk[name - 1];
    if (!t->used) return;
    texture_free(t);
    *t = (G2Texture){0};
    gles2_target_forget();
}
static void texture_say(const char* line) { fprintf(stderr, "scarletnx-android: GLES 2: %s\n", line); }
/* the device's, once it is made (gles_vk.c) */
static uint32_t texture_max_size(void) { return V.max_texture ? V.max_texture : 4096; }
void gles2_textures_init(void) {
    snx_gl_texture_host(&(SnxGlTextureHost){.storage = texture_storage,
                                            .upload = texture_upload,
                                            .mipmaps = texture_mipmaps,
                                            .image_bound = texture_image_bound,
                                            .gone = texture_gone,
                                            .say = texture_say,
                                            .max_size = texture_max_size});
}

VkImageView gles2_image_unit(Gles2Context* gl, unsigned unit, uint8_t dim) {
    if (unit >= G2_IMAGE_UNITS) return VK_NULL_HANDLE;
    const SnxGlImageUnit* image = &snx_gl_textures(gl->ctx)->images[unit];
    G2Texture* t = gles2_texture(image->texture);
    int level = image->level;
    if (!t || !t->image.image || t->layout != VK_IMAGE_LAYOUT_GENERAL || level < 0 || level >= t->levels)
        return VK_NULL_HANDLE;
    static const VkImageViewType types[G2_TARGETS] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_CUBE,
                                                      VK_IMAGE_VIEW_TYPE_3D, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                      VK_IMAGE_VIEW_TYPE_2D};
    bool whole = image->layered || t->layers == 1;
    VkImageViewType type = whole ? types[dim < G2_TARGETS ? dim : 0] : VK_IMAGE_VIEW_TYPE_2D;
    if (type == VK_IMAGE_VIEW_TYPE_3D && !whole) type = VK_IMAGE_VIEW_TYPE_2D;
    return gles2_texture_view(t, type, level, 1, whole ? 0 : image->layer, whole ? t->layers : 1, false,
                              VK_IMAGE_ASPECT_COLOR_BIT);
}

void gles2_memory_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                         .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
                         .dstAccessMask = VK_ACCESS_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                                          VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                                          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                                          VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT |
                                          VK_ACCESS_HOST_READ_BIT};
    /* the host's reads and writes are the HOST stage's, which ALL_COMMANDS leaves out */
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT;
    V.vk.vkCmdPipelineBarrier(cb, stages, stages, 0, 1, &b, 0, NULL, 0, NULL);
}
static void g_memory_barrier(CPU* c, Gles2Context* gl) {
    (void)c;
    GlesSlot* slot = gles2_transfer_slot(gl);
    if (slot) gles2_memory_barrier(slot->cb);
}
static void g_sample_maski(CPU* c, Gles2Context* gl) {
    if ((unsigned)X(0) == 0)
        gles2_fixed(gl)->sample_mask = (uint32_t)X(1);
    else gles2_error(gl, GL_INVALID_VALUE_);
}
/* glGetMultisamplefv(GL_SAMPLE_POSITION, index, values): Vulkan's standard positions of 4 samples */
static void g_get_multisamplefv(CPU* c, Gles2Context* gl) {
    static const float positions[4][2] = {{0.375f, 0.125f}, {0.875f, 0.375f}, {0.125f, 0.625f}, {0.625f, 0.875f}};
    unsigned i = (unsigned)X(1);
    if ((unsigned)X(0) != 0x8e50 || i >= 4) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    memcpy(PTR(2), positions[i], 8);
}

static void g_gen_textures(CPU* c, Gles2Context* gl) { snx_gl_gen_textures(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_delete_textures(CPU* c, Gles2Context* gl) { snx_gl_delete_textures(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_is_texture(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_texture((uint32_t)X(0));
}
static void g_bind_texture(CPU* c, Gles2Context* gl) { snx_gl_bind_texture(gl->ctx, (uint32_t)X(0), (uint32_t)X(1)); }
static void g_active_texture(CPU* c, Gles2Context* gl) { snx_gl_active_texture(gl->ctx, (uint32_t)X(0)); }
static void g_tex_image_2d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_image(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (int32_t)X(3), (int32_t)X(4), 1,
                     (uint32_t)X(6), (uint32_t)X(7), gles2_arg(c, 8), 0, 0);
}
static void g_tex_image_3d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_image(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (int32_t)X(3), (int32_t)X(4),
                     (int32_t)X(5), (uint32_t)gles2_arg(c, 7), (uint32_t)gles2_arg(c, 8), gles2_arg(c, 9), 0, 0);
}
static void g_compressed_tex_image_2d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_image(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (int32_t)X(3), (int32_t)X(4), 1, 0, 0,
                     X(7), 1, (uint32_t)X(6));
}
static void g_compressed_tex_image_3d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_image(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (int32_t)X(3), (int32_t)X(4),
                     (int32_t)X(5), 0, 0, gles2_arg(c, 8), 1, (uint32_t)X(7));
}
static void g_tex_sub_image_2d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_sub_image(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (int32_t)X(2), (int32_t)X(3), 0, (int32_t)X(4),
                         (int32_t)X(5), 1, (uint32_t)X(6), (uint32_t)X(7), gles2_arg(c, 8), 0, 0);
}
static void g_tex_sub_image_3d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_sub_image(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (int32_t)X(2), (int32_t)X(3), (int32_t)X(4),
                         (int32_t)X(5), (int32_t)X(6), (int32_t)X(7), (uint32_t)gles2_arg(c, 8),
                         (uint32_t)gles2_arg(c, 9), gles2_arg(c, 10), 0, 0);
}
static void g_compressed_tex_sub_image_2d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_sub_image(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (int32_t)X(2), (int32_t)X(3), 0, (int32_t)X(4),
                         (int32_t)X(5), 1, 0, 0, gles2_arg(c, 8), 1, (uint32_t)X(7));
}
static void g_compressed_tex_sub_image_3d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_sub_image(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (int32_t)X(2), (int32_t)X(3), (int32_t)X(4),
                         (int32_t)X(5), (int32_t)X(6), (int32_t)X(7), 0, 0, gles2_arg(c, 10), 1,
                         (uint32_t)gles2_arg(c, 9));
}
static void g_tex_storage_2d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_storage(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (int32_t)X(3), (int32_t)X(4), 1);
}
static void g_tex_storage_3d(CPU* c, Gles2Context* gl) {
    snx_gl_tex_storage(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (int32_t)X(3), (int32_t)X(4),
                       (int32_t)X(5));
}
/* glTexStorage2DMultisample(target, samples, internal, width, height, fixed locations) */
static void g_tex_storage_2d_multisample(CPU* c, Gles2Context* gl) {
    snx_gl_tex_storage_multisample(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (int32_t)X(3),
                                   (int32_t)X(4));
}
static void g_get_tex_level_parameteriv(CPU* c, Gles2Context* gl) {
    float v = 0;
    if (!snx_gl_get_tex_level_parameter(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), &v)) return;
    int32_t x = (int32_t)v;
    memcpy(PTR(3), &x, 4);
}
static void g_get_tex_level_parameterfv(CPU* c, Gles2Context* gl) {
    float v = 0;
    if (snx_gl_get_tex_level_parameter(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), &v))
        memcpy(PTR(3), &v, 4);
}
/* glBindImageTexture(unit, texture, level, layered, layer, access, format) */
static void g_bind_image_texture(CPU* c, Gles2Context* gl) {
    snx_gl_bind_image_texture(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (int32_t)X(2), (X(3) & 0xff) != 0, (int32_t)X(4),
                              (uint32_t)X(5), (uint32_t)X(6));
}
static void g_tex_parameteri(CPU* c, Gles2Context* gl) {
    snx_gl_tex_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (float)(int32_t)X(2));
}
static void g_tex_parameterf(CPU* c, Gles2Context* gl) {
    snx_gl_tex_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), hle_f(c, 0));
}
static void g_tex_parameteriv(CPU* c, Gles2Context* gl) {
    int32_t v;
    memcpy(&v, PTR(2), 4);
    snx_gl_tex_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (float)v);
}
static void g_tex_parameterfv(CPU* c, Gles2Context* gl) {
    float v;
    memcpy(&v, PTR(2), 4);
    snx_gl_tex_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), v);
}
static void g_get_tex_parameteriv(CPU* c, Gles2Context* gl) {
    float v = 0;
    if (!snx_gl_get_tex_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), &v)) return;
    int32_t x = (int32_t)v;
    memcpy(PTR(2), &x, 4);
}
static void g_get_tex_parameterfv(CPU* c, Gles2Context* gl) {
    float v = 0;
    if (snx_gl_get_tex_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), &v)) memcpy(PTR(2), &v, 4);
}
static void g_generate_mipmap(CPU* c, Gles2Context* gl) { snx_gl_generate_mipmap(gl->ctx, (uint32_t)X(0)); }

bool gles2_texture_complete(const G2Texture* t, int* levels) {
    int32_t n = 0;
    if (!t || !t->image.image || !snx_gl_texture_complete(texture_name(t), &n)) return false;
    *levels = (int)n;
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

/* ---- samplers ---- */

const G2SamplerState* gles2_unit_sampler(Gles2Context* gl, unsigned unit, const G2Texture* t) {
    return snx_gl_unit_sampler(gl->ctx, unit, t ? texture_name(t) : 0);
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

static void g_gen_samplers(CPU* c, Gles2Context* gl) { snx_gl_gen_samplers(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_delete_samplers(CPU* c, Gles2Context* gl) { snx_gl_delete_samplers(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_is_sampler(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_sampler((uint32_t)X(0));
}
static void g_bind_sampler(CPU* c, Gles2Context* gl) { snx_gl_bind_sampler(gl->ctx, (uint32_t)X(0), (uint32_t)X(1)); }
static void g_sampler_parameteri(CPU* c, Gles2Context* gl) {
    snx_gl_sampler_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (float)(int32_t)X(2));
}
static void g_sampler_parameterf(CPU* c, Gles2Context* gl) {
    snx_gl_sampler_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), hle_f(c, 0));
}
static void g_sampler_parameteriv(CPU* c, Gles2Context* gl) {
    int32_t v;
    memcpy(&v, PTR(2), 4);
    snx_gl_sampler_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (float)v);
}
static void g_sampler_parameterfv(CPU* c, Gles2Context* gl) {
    float v;
    memcpy(&v, PTR(2), 4);
    snx_gl_sampler_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), v);
}
static void g_get_sampler_parameteriv(CPU* c, Gles2Context* gl) {
    float v = 0;
    if (!snx_gl_get_sampler_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), &v)) return;
    int32_t x = (int32_t)v;
    memcpy(PTR(2), &x, 4);
}
static void g_get_sampler_parameterfv(CPU* c, Gles2Context* gl) {
    float v = 0;
    if (snx_gl_get_sampler_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), &v)) memcpy(PTR(2), &v, 4);
}

/* ---- renderbuffers and framebuffers: the shared layer's (common/gpu/gl/gl_framebuffer.h), and each renderbuffer's
   image here ---- */

/* a renderbuffer's image, with the shared layer's record copied in now: NULL for 0 or a name not made */
G2Renderbuffer* gles2_renderbuffer(unsigned name) {
    const SnxGlRenderbuffer* x = snx_gl_renderbuffer(name);
    if (!x) return NULL;
    G2Renderbuffer* r = &g_gles2.renderbuffer_vk[name - 1];
    r->used = true;
    r->format = gles2_format_at(x->format);
    r->width = x->width, r->height = x->height;
    r->samples = (VkSampleCountFlagBits)x->samples;
    r->generation = x->generation;
    return r;
}
G2Framebuffer* gles2_framebuffer(unsigned name) { return snx_gl_framebuffer(name); }

static void renderbuffer_free(G2Renderbuffer* r) {
    if (r->image.image)
        gles_vk_retire((GlesGarbage){.image = r->image.image, .view = r->image.view, .memory = r->image.memory});
    r->image = (GlesImage){0};
    r->ready = false;
}
/* the shared layer made the renderbuffer's storage anew: its image, of a format the device renders to */
static int renderbuffer_storage(uint32_t ctx, uint32_t name) {
    Gles2Context* gl = texture_context(ctx);
    G2Renderbuffer* r = gles2_renderbuffer(name);
    if (!r || !r->format) return 0;
    const G2Format* f = r->format;
    renderbuffer_free(r);
    bool expanded;
    VkComponentMapping swizzle;
    VkFormat vk = pick(f, needs(f, false), &expanded, &swizzle);
    if (!vk && f->kind == G2_STENCIL) vk = VK_FORMAT_D32_SFLOAT_S8_UINT;
    if (!vk) {
        gles2_warn(6, "a renderbuffer format the device does not render to");
        return 0;
    }
    VkImageAspectFlags aspect = gles2_aspect(f);
    if (vk == VK_FORMAT_D24_UNORM_S8_UINT || vk == VK_FORMAT_D32_SFLOAT_S8_UINT)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = vk,
                              .extent = {(uint32_t)r->width, (uint32_t)r->height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = r->samples,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = (f->kind >= G2_DEPTH ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                                            : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT};
    if (!gles_vk_image(&info, VK_IMAGE_VIEW_TYPE_2D, aspect, &r->image)) {
        gles2_error(gl, GL_OUT_OF_MEMORY_);
        return 0;
    }
    r->vk = vk;
    return 1;
}
static void renderbuffer_gone(uint32_t name) {
    G2Renderbuffer* r = &g_gles2.renderbuffer_vk[name - 1];
    if (!r->used) return;
    renderbuffer_free(r);
    *r = (G2Renderbuffer){0};
    gles2_target_forget();
}
static void window_bits(uint32_t ctx, int32_t* depth, int32_t* stencil) {
    Gles2Context* gl = texture_context(ctx);
    *depth = gl && gl->draw ? gl->draw->depth_bits : 0;
    *stencil = gl && gl->draw ? gl->draw->stencil_bits : 0;
}
void gles2_framebuffers_init(void) {
    snx_gl_framebuffer_host(&(SnxGlFramebufferHost){.renderbuffer_storage = renderbuffer_storage,
                                                    .renderbuffer_gone = renderbuffer_gone,
                                                    .changed = gles2_target_forget,
                                                    .window_bits = window_bits,
                                                    .max_size = texture_max_size});
}
void gles2_objects_vk_free(void) {
    for (size_t i = 0; i < SNX_GL_BUFFERS; ++i) buffer_vk_free(&g_gles2.buffer_vk[i]);
    for (size_t i = 0; i < SNX_GL_TEXTURES; ++i) texture_free(&g_gles2.texture_vk[i]);
    for (size_t i = 0; i < SNX_GL_RENDERBUFFERS; ++i) renderbuffer_free(&g_gles2.renderbuffer_vk[i]);
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

static void g_gen_renderbuffers(CPU* c, Gles2Context* gl) { snx_gl_gen_renderbuffers(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_delete_renderbuffers(CPU* c, Gles2Context* gl) {
    snx_gl_delete_renderbuffers(gl->ctx, (int32_t)X(0), PTR(1));
}
static void g_is_renderbuffer(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_renderbuffer((uint32_t)X(0));
}
static void g_bind_renderbuffer(CPU* c, Gles2Context* gl) {
    snx_gl_bind_renderbuffer(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
static void g_renderbuffer_storage(CPU* c, Gles2Context* gl) {
    snx_gl_renderbuffer_storage(gl->ctx, (uint32_t)X(0), 0, (uint32_t)X(1), (int32_t)X(2), (int32_t)X(3));
}
static void g_renderbuffer_storage_multisample(CPU* c, Gles2Context* gl) {
    snx_gl_renderbuffer_storage(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (int32_t)X(3), (int32_t)X(4));
}
static void g_get_renderbuffer_parameteriv(CPU* c, Gles2Context* gl) {
    int32_t v = 0;
    if (snx_gl_get_renderbuffer_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), &v)) memcpy(PTR(2), &v, 4);
}

static void g_gen_framebuffers(CPU* c, Gles2Context* gl) { snx_gl_gen_framebuffers(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_delete_framebuffers(CPU* c, Gles2Context* gl) {
    snx_gl_delete_framebuffers(gl->ctx, (int32_t)X(0), PTR(1));
}
static void g_is_framebuffer(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_framebuffer((uint32_t)X(0));
}
static void g_bind_framebuffer(CPU* c, Gles2Context* gl) {
    snx_gl_bind_framebuffer(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
static void g_framebuffer_texture_2d(CPU* c, Gles2Context* gl) {
    snx_gl_framebuffer_texture(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), (uint32_t)X(3), (int32_t)X(4),
                               0);
}
static void g_framebuffer_texture_layer(CPU* c, Gles2Context* gl) {
    snx_gl_framebuffer_texture(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), 0, (uint32_t)X(2), (int32_t)X(3),
                               (int32_t)X(4));
}
static void g_framebuffer_renderbuffer(CPU* c, Gles2Context* gl) {
    snx_gl_framebuffer_renderbuffer(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), (uint32_t)X(3));
}
static void g_check_framebuffer_status(CPU* c, Gles2Context* gl) {
    X(0) = snx_gl_check_framebuffer_status(gl->ctx, (uint32_t)X(0));
}
static void g_get_framebuffer_attachment_parameteriv(CPU* c, Gles2Context* gl) {
    int32_t v = 0;
    if (snx_gl_get_framebuffer_attachment_parameter(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (uint32_t)X(2), &v))
        memcpy(PTR(3), &v, 4);
}
static void g_draw_buffers(CPU* c, Gles2Context* gl) { snx_gl_draw_buffers(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_read_buffer(CPU* c, Gles2Context* gl) { snx_gl_read_buffer(gl->ctx, (uint32_t)X(0)); }
static void g_invalidate_framebuffer(CPU* c, Gles2Context* gl) {
    (void)c, (void)gl; /* a hint: what it drew stays */
}

/* ---- vertex arrays and attributes ---- */

static void g_gen_vertex_arrays(CPU* c, Gles2Context* gl) { snx_gl_gen_vertex_arrays(gl->ctx, (int32_t)X(0), PTR(1)); }
static void g_bind_vertex_array(CPU* c, Gles2Context* gl) { snx_gl_bind_vertex_array(gl->ctx, (uint32_t)X(0)); }
static void g_delete_vertex_arrays(CPU* c, Gles2Context* gl) {
    snx_gl_delete_vertex_arrays(gl->ctx, (int32_t)X(0), PTR(1));
}
static void g_is_vertex_array(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_vertex_array((uint32_t)X(0));
}
static void g_vertex_attrib_pointer(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib_pointer(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (X(3) & 0xff) != 0, 0,
                                 (int32_t)X(4), X(5));
}
static void g_vertex_attrib_ipointer(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib_pointer(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), 0, 1, (int32_t)X(3), X(4));
}
/* GLES 3.1's vertex attribute bindings */
static void g_vertex_attrib_format(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib_format(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), (X(3) & 0xff) != 0, 0,
                                (uint32_t)X(4));
}
static void g_vertex_attrib_iformat(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib_format(gl->ctx, (uint32_t)X(0), (int32_t)X(1), (uint32_t)X(2), 0, 1, (uint32_t)X(3));
}
static void g_vertex_attrib_binding(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib_binding(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
/* glBindVertexBuffer(binding, buffer, offset, stride) */
static void g_bind_vertex_buffer(CPU* c, Gles2Context* gl) {
    snx_gl_bind_vertex_buffer(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), (int64_t)X(2), (int32_t)X(3));
}
static void g_vertex_binding_divisor(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_binding_divisor(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
static void g_enable_vertex_attrib_array(CPU* c, Gles2Context* gl) {
    snx_gl_enable_vertex_attrib(gl->ctx, (uint32_t)X(0), 1);
}
static void g_disable_vertex_attrib_array(CPU* c, Gles2Context* gl) {
    snx_gl_enable_vertex_attrib(gl->ctx, (uint32_t)X(0), 0);
}
static void g_vertex_attrib_divisor(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib_divisor(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
static void attrib_f(CPU* c, Gles2Context* gl, int n) {
    float v[4];
    for (int i = 0; i < n; ++i) v[i] = hle_f(c, (unsigned)i);
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), v, n, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib1f(CPU* c, Gles2Context* gl) { attrib_f(c, gl, 1); }
static void g_vertex_attrib2f(CPU* c, Gles2Context* gl) { attrib_f(c, gl, 2); }
static void g_vertex_attrib3f(CPU* c, Gles2Context* gl) { attrib_f(c, gl, 3); }
static void g_vertex_attrib4f(CPU* c, Gles2Context* gl) { attrib_f(c, gl, 4); }
static void g_vertex_attrib1fv(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), PTR(1), 1, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib2fv(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), PTR(1), 2, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib3fv(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), PTR(1), 3, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib4fv(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), PTR(1), 4, SNX_GLSLC_BASE_FLOAT);
}
static void g_vertex_attrib_i4i(CPU* c, Gles2Context* gl) {
    int32_t v[4] = {(int32_t)X(1), (int32_t)X(2), (int32_t)X(3), (int32_t)X(4)};
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), v, 4, SNX_GLSLC_BASE_INT);
}
static void g_vertex_attrib_i4ui(CPU* c, Gles2Context* gl) {
    uint32_t v[4] = {(uint32_t)X(1), (uint32_t)X(2), (uint32_t)X(3), (uint32_t)X(4)};
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), v, 4, SNX_GLSLC_BASE_UINT);
}
static void g_vertex_attrib_i4iv(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), PTR(1), 4, SNX_GLSLC_BASE_INT);
}
static void g_vertex_attrib_i4uiv(CPU* c, Gles2Context* gl) {
    snx_gl_vertex_attrib(gl->ctx, (uint32_t)X(0), PTR(1), 4, SNX_GLSLC_BASE_UINT);
}
static void g_get_vertex_attribiv(CPU* c, Gles2Context* gl) {
    double v[4];
    int n = snx_gl_get_vertex_attrib(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), v);
    for (int i = 0; i < n; ++i) {
        int32_t x = (int32_t)v[i];
        memcpy((unsigned char*)PTR(2) + 4 * i, &x, 4);
    }
}
static void g_get_vertex_attribfv(CPU* c, Gles2Context* gl) {
    double v[4];
    int n = snx_gl_get_vertex_attrib(gl->ctx, (uint32_t)X(0), (uint32_t)X(1), v);
    for (int i = 0; i < n; ++i) {
        float x = (float)v[i];
        memcpy((unsigned char*)PTR(2) + 4 * i, &x, 4);
    }
}
static void g_get_vertex_attrib_pointerv(CPU* c, Gles2Context* gl) {
    uint32_t i = (uint32_t)X(0);
    const SnxGlVertexArray* a = snx_gl_vertex_array(gl->ctx);
    if (i >= SNX_GL_VERTEX_ATTRIBS) {
        gles2_error(gl, GL_INVALID_VALUE_);
        return;
    }
    memcpy(PTR(2), &a->bindings[a->attribs[i].binding].offset, 8);
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

/* ---- sync objects: the shared layer's (common/gpu/gl/gl_sync.h), fences of this file's submissions ---- */

static uint64_t sync_recorded(void) { return gles_vk_pending(); }
static uint64_t sync_completed(void) { return V.completed; }
/* the work up to serial: the context's rendering submitted first, when it is not yet */
static void sync_wait(uint32_t ctx, uint64_t serial) {
    Gles2Context* gl = texture_context(ctx);
    if (gl && serial > V.submitted) {
        gles2_rendering_stop(gl);
        if (gl->draw) gles_flush(gl->draw);
    }
    gles_vk_wait(serial);
}
void gles2_syncs_init(void) {
    snx_gl_sync_host(&(SnxGlSyncHost){
        .recorded = sync_recorded, .completed = sync_completed, .wait = sync_wait, .say = texture_say});
}
static void g_fence_sync(CPU* c, Gles2Context* gl) {
    X(0) = snx_gl_fence_sync(gl->ctx, (uint32_t)X(0), (uint32_t)X(1));
}
static void g_delete_sync(CPU* c, Gles2Context* gl) { snx_gl_delete_sync(gl->ctx, X(0)); }
static void g_is_sync(CPU* c, Gles2Context* gl) {
    (void)gl;
    X(0) = (uint64_t)snx_gl_is_sync(X(0));
}
/* glClientWaitSync(sync, flags, timeout) */
static void g_client_wait_sync(CPU* c, Gles2Context* gl) {
    X(0) = snx_gl_client_wait_sync(gl->ctx, X(0), (uint32_t)X(1), X(2));
}
static void g_wait_sync(CPU* c, Gles2Context* gl) { snx_gl_wait_sync(gl->ctx, X(0), (uint32_t)X(1), X(2)); }
/* glGetSynciv(sync, pname, count, length, values) */
static void g_get_synciv(CPU* c, Gles2Context* gl) {
    int32_t v = 0, one = 1;
    if (!snx_gl_get_sync(gl->ctx, X(0), (uint32_t)X(1), &v)) return;
    if ((int32_t)X(2) >= 1) memcpy(PTR(4), &v, 4);
    if (X(3)) memcpy(PTR(3), &one, 4);
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
    FN("glTexStorage2DMultisample", g_tex_storage_2d_multisample),
    FN("glGetTexLevelParameteriv", g_get_tex_level_parameteriv),
    FN("glGetTexLevelParameterfv", g_get_tex_level_parameterfv),
    FN("glBindImageTexture", g_bind_image_texture),
    FN("glMemoryBarrier", g_memory_barrier),
    FN("glMemoryBarrierByRegion", g_memory_barrier),
    FN("glSampleMaski", g_sample_maski),
    FN("glGetMultisamplefv", g_get_multisamplefv),
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
    FN("glVertexAttribFormat", g_vertex_attrib_format),
    FN("glVertexAttribIFormat", g_vertex_attrib_iformat),
    FN("glVertexAttribBinding", g_vertex_attrib_binding),
    FN("glBindVertexBuffer", g_bind_vertex_buffer),
    FN("glVertexBindingDivisor", g_vertex_binding_divisor),
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
