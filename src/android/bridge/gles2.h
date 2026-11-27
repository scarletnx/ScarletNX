/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_GLES2_H
#define SNX_ANDROID_GLES2_H
/* OpenGL ES 2.0, 3.0 and 3.1 over Vulkan 1.3, as Mesa's zink does it (docs/platforms/android-apps/native.md, "EGL and GLES"), on the
   device and surfaces of gles_vk.h:

   gles2.c          the contexts, the fixed state (enables, blend, depth, stencil, viewport), the queries of state,
                    and the HLE table of libGLESv2 and libGLESv3
   gles2_object.c   buffers, textures and their formats, samplers, renderbuffers, framebuffers, vertex arrays,
                    queries and sync objects
   gles2_program.c  shaders and programs: the shared GL layer's (common/gpu/gl/gl_program.h), and each program's
                    Vulkan objects, made from its link
   gles2_draw.c     the render targets (dynamic rendering), the pipelines from the GL state at draw time, the draws,
                    clears, glReadPixels, blits and copies

   The objects of each kind are in one array, by GL name: one share group for every context, as GLES 1.1 has. Each
   call that reaches Vulkan holds g_gles_vk.lock */
#include "android/bridge/gles_vk.h"
#include "android/bridge/hle.h"
#include "common/gpu/gl/gl_format.h"
#include "common/gpu/gl/gl_framebuffer.h"
#include "common/gpu/gl/gl_sync.h"
#include "common/gpu/gl/gl_object.h"
#include "common/gpu/gl/gl_texture.h"
#include "common/gpu/gl/gl_program.h"
#include "common/gpu/gl/gl_state.h"
#include "common/cpu/aarch64/cpu.h"
#include "glslc/glslc.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    G2_UNITS = 32,       /* texture image units, GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS */
    G2_ATTRIBS = 16,     /* GL_MAX_VERTEX_ATTRIBS */
    G2_COLORS = 4,       /* GL_MAX_COLOR_ATTACHMENTS and GL_MAX_DRAW_BUFFERS */
    G2_UBO_BINDINGS = 36,
    G2_SSBO_BINDINGS = 8, /* GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS */
    G2_IMAGE_UNITS = 8,   /* GL_MAX_IMAGE_UNITS */
    G2_TARGETS = 5,      /* the texture targets of a unit: 2D, cube map, 3D, 2D array, 2D multisample */
    G2_VIEWS = 8         /* the image views a texture keeps */
};

/* the texture targets as indices */
enum { G2_T2D = 0, G2_TCUBE = 1, G2_T3D = 2, G2_TARRAY = 3, G2_TMS = 4 };

/* a range of a buffer that commands read */
typedef struct {
    VkBuffer buffer;
    VkDeviceSize offset;
} G2Ref;

/* a buffer's Vulkan copy, by the shared layer's name - 1 (common/gpu/gl/gl_object.h): data and size are the layer's
   bytes as gles2_buffer gives them; a buffer of the bridge's own fills them itself */
typedef struct {
    unsigned char* data; /* what the guest wrote: the copy the CPU reads and writes */
    size_t size;
    /* the Vulkan copy, host-visible */
    VkBuffer buffer;
    VkDeviceMemory memory;
    unsigned char* map;
    size_t capacity;
    bool cached;           /* host-cached memory, for a buffer the GPU writes and the CPU reads */
    uint64_t serial;       /* the last submission that read or wrote the Vulkan copy */
    bool stale;            /* the Vulkan copy is older than data, all of it */
    size_t lo, hi;         /* else data[lo, hi) is newer */
    bool gpu_newer;        /* the GPU wrote the Vulkan copy after data */
    /* a copy in the frame's arena, while the Vulkan copy is in use: for the frame of arena_serial */
    G2Ref arena;
    uint64_t arena_serial;
} G2Buffer;

/* a sampler's state, the shared layer's (common/gpu/gl/gl_texture.h) */
typedef SnxGlSamplerState G2SamplerState;

/* a GL format as Vulkan's (gles2_object.c's table) */
typedef struct {
    unsigned internal;          /* the sized internal format */
    unsigned format, type;      /* the client data that copies into it as it is */
    VkFormat vk, alt;           /* the format, and another with red and blue apart, or 0 */
    VkComponentMapping swizzle; /* of the view (luminance and alpha) */
    VkComponentMapping alt_swizzle;
    uint8_t bytes;              /* of a texel of the client data, or of a compressed block */
    uint8_t expand;             /* 3 components of 1, 2 or 4 bytes, which gles_expand.comp makes 4: their size */
    uint8_t kind;               /* G2_FLOAT (normalized too), G2_INT, G2_UINT, G2_DEPTH, G2_STENCIL, G2_DS */
    uint8_t block;              /* a compressed format's block width and height, 0 for others */
} G2Format;
/* a format's kind, the shared layer's (common/gpu/gl/gl_format.h) */
enum {
    G2_FLOAT = SNX_GL_FLOAT,
    G2_INT = SNX_GL_INT,
    G2_UINT = SNX_GL_UINT,
    G2_DEPTH = SNX_GL_DEPTH,
    G2_STENCIL = SNX_GL_STENCIL,
    G2_DS = SNX_GL_DS
};

/* a texture's image, and its shared record's fields as gles2_texture() copied them (gl_texture.h's) */
typedef struct {
    bool used;
    uint8_t target; /* G2_T*, 0xff until the first bind */
    GlesImage image;
    VkFormat vk;
    const G2Format* format; /* the format table's entry, a const table */
    VkComponentMapping swizzle;
    int width, height, depth; /* depth: a 3D texture's, else 1 */
    int levels, layers;       /* layers: a cube's 6, an array's count, else 1 */
    VkSampleCountFlagBits samples;
    uint32_t defined;         /* the levels that have data */
    bool immutable;
    VkImageLayout layout;     /* of the whole image */
    G2SamplerState sampler;
    int base_level, max_level;
    unsigned swizzles[4];
    uint32_t generation; /* counts the images made: a framebuffer's views of an older one are stale */
    bool stencil_mode;   /* GL_DEPTH_STENCIL_TEXTURE_MODE is GL_STENCIL_INDEX: a sampler reads the stencil */
    struct {
        uint64_t key;
        VkImageView view;
    } views[G2_VIEWS];
    uint32_t view_count;
} G2Texture;

/* a renderbuffer's image, and its shared record's fields as gles2_renderbuffer() copied them */
typedef struct {
    bool used;
    GlesImage image;
    VkFormat vk;
    const G2Format* format;
    int width, height;
    VkSampleCountFlagBits samples;
    bool ready; /* in its attachment layout; else UNDEFINED */
    uint32_t generation;
} G2Renderbuffer;

/* a framebuffer and its attachments, the shared layer's (common/gpu/gl/gl_framebuffer.h) */
typedef SnxGlAttachment G2Attachment;
typedef SnxGlFramebuffer G2Framebuffer;
enum {
    G2_DEPTH_ATTACHMENT = SNX_GL_DEPTH_ATTACHMENT,
    G2_STENCIL_ATTACHMENT = SNX_GL_STENCIL_ATTACHMENT,
    G2_ATTACHMENTS = SNX_GL_ATTACHMENTS
};

typedef struct {
    bool used, active;
    unsigned target;
    VkQueryPool pool;
    uint32_t count, capacity; /* the Vulkan queries begun: one for each render pass instance it spans */
    uint64_t serial;          /* the submission whose results it waits for */
    bool ready;
    uint64_t result;
} G2Query;


/* a program's Vulkan objects, made from its link (common/gpu/gl/gl_program.h) at its first draw, by name - 1 */
typedef struct {
    uint32_t generation; /* the link's they were made from, 0 for none */
    VkShaderModule modules[3];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline compute;
    G2Ref default_ref[3];         /* each stage's default block in its frame's arena */
    uint32_t default_uploaded[3]; /* the program's default_serial it holds */
    uint64_t default_frame[3];    /* the frame it went into */
} G2ProgramVk;

/* a context of GLES 2 or 3: its fixed state is the shared GL layer's (common/gpu/gl/gl_state.h), named by ctx */
typedef struct Gles2Context {
    uint32_t ctx;
    int version; /* 2 or 3 */
    GlesSurface* draw;
    GlesSurface* read;
    GlesSurface* own; /* a 1x1 pbuffer, for a context current without a surface */
    bool sized;
    unsigned query_any; /* the active occlusion query, or 0 */
    unsigned query_any_target;
} Gles2Context;

/* the state a pipeline is made from (gles2_draw.c): its bytes are the key, padding zeroed */
typedef struct {
    uint32_t program, generation;
    uint32_t colors[G2_COLORS], depth;
    uint32_t formats[G2_ATTRIBS]; /* the vertex attributes' formats by location, 0 for none */
    uint16_t rates;               /* bit i: location i's binding steps by instance */
    uint8_t samples, topology, alpha_to_coverage, clip_control;
    uint8_t blend, write_mask;
    uint8_t off; /* bit i: color attachment i written by none (a masked clear of the others) */
    uint8_t pad;
    uint8_t factors[4], ops[2], pad2[2];
    uint32_t sample_mask;
} G2PipelineKey;

typedef struct {
    G2PipelineKey key;
    VkPipeline pipeline;
    uint64_t hash;
} G2PipelineEntry;

/* the module's state: the share group's objects, and the caches of the device's objects */
typedef struct {
    G2Buffer buffer_vk[SNX_GL_BUFFERS];
    G2Texture texture_vk[SNX_GL_TEXTURES];
    G2Renderbuffer renderbuffer_vk[SNX_GL_RENDERBUFFERS];
    G2Query* queries;
    size_t query_count;
    G2ProgramVk program_vk[SNX_GL_PROGRAM_NAMES];
    /* the Vulkan samplers by their state */
    struct {
        G2SamplerState state;
        VkSampler sampler;
    }* vk_samplers;
    size_t vk_sampler_count;
    /* the pipelines by their state: open addressing, a power of 2 */
    G2PipelineEntry* pipelines;
    size_t pipeline_capacity, pipeline_count;
    /* the textures of a sampler without a complete texture: black, one of each G2_T* dimension */
    GlesImage missing[G2_TARGETS];
    bool missing_ready;
    /* the expansion of 3-component texels (gles_expand.comp) */
    VkDescriptorSetLayout expand_set;
    VkPipelineLayout expand_layout;
    VkPipeline expand;
    /* a clear that a mask limits, drawn (gles2_clear.vert and .frag), its color in a push constant: made at its
       first use, its pipelines in pipelines by a key of program 0 */
    G2ProgramVk clear;
    bool said[27];         /* the warnings said once */
    char extensions[1024]; /* GL_EXTENSIONS, joined at its first query */
    G2Format formats[SNX_GL_FORMATS]; /* each format's entry, made at its first use (gles2_format) */
} Gles2;
extern Gles2 g_gles2;

/* the calling thread's current context, or NULL */
Gles2Context* gles2_current(void);
/* a context's fixed state (enables, blending, depth, stencil, viewport, scissor, clear values, pixel store) */
static inline SnxGlFixed* gles2_fixed(const Gles2Context* gl) { return snx_gl_fixed(gl->ctx); }
/* a context of GLES major version 2 or 3; NULL without memory */
Gles2Context* gles2_context_new(int version);
void gles2_context_free(Gles2Context* gl);
/* After the last context and surface: every Vulkan object of the share group and of the caches destroyed, then the
   device and the instance (gles_vk_shutdown). The objects' names stay, without their images and buffers */
void gles2_shutdown(void);
/* gles2_shutdown's part of each file: its objects' Vulkan objects, under the lock */
void gles2_objects_vk_free(void);
void gles2_programs_vk_free(void);
void gles2_draw_vk_free(void);
/* the calling thread's context and its surfaces (NULL, NULL, NULL: none) */
void gles2_make_current(Gles2Context* gl, GlesSurface* draw, GlesSurface* read);
/* GL_VERSION and the others for glGetString, of a context of version (2 or 3) */
const char* gles2_string(int version, unsigned name);

/* the context's draw surface: its own 1x1 pbuffer, made now, when it is current without one */
GlesSurface* gles2_surface(Gles2Context* gl);
void gles2_error(Gles2Context* gl, unsigned error);
/* the argument n of a call: x0 to x7, then the guest's stack */
uint64_t gles2_arg(const CPU* c, unsigned n);
/* a once-said warning, by its slot of g_gles2.said */
void gles2_warn(unsigned slot, const char* message);

/* gles_vk.c's calls: GLES 2's rendering ended in the surface's frame; the surface is going away */
void gles2_rendering_end(GlesSurface* s);
void gles2_surface_gone(GlesSurface* s);

/* gles2_object.c */
/* the shared layer's buffers tell this file what the CPU wrote and when one goes */
void gles2_buffers_init(void);
/* a buffer's Vulkan copy, with the shared layer's bytes and size now: NULL for 0 or a name not made */
G2Buffer* gles2_buffer(unsigned name);
/* the context's buffer bindings, vertex arrays and generic attributes (common/gpu/gl/gl_object.h) */
static inline SnxGlObjectContext* gles2_objects(const Gles2Context* gl) { return snx_gl_objects(gl->ctx); }
/* the shared layer's textures tell this file when one's storage is new, its texels, its mipmaps, when it goes */
void gles2_textures_init(void);
/* a texture's image, with the shared layer's record copied in now: NULL for 0 or a name not made */
G2Texture* gles2_texture(unsigned name);
/* a level of the texture written here (a copy into it): it has texels now */
void gles2_texture_defined(G2Texture* t, int level);
/* the shared layer's renderbuffers tell this file when one's storage is new or goes, and its framebuffers when they
   change */
void gles2_framebuffers_init(void);
/* the shared layer's fences are this file's submissions' serials */
void gles2_syncs_init(void);
/* a renderbuffer's image, with the shared layer's record copied in now: NULL for 0 or a name not made */
G2Renderbuffer* gles2_renderbuffer(unsigned name);
/* the context's framebuffer and renderbuffer bindings (common/gpu/gl/gl_framebuffer.h) */
static inline SnxGlFramebufferContext* gles2_fbos(const Gles2Context* gl) { return snx_gl_framebuffers(gl->ctx); }
G2Framebuffer* gles2_framebuffer(unsigned name);
const G2SamplerState* gles2_unit_sampler(Gles2Context* gl, unsigned unit, const G2Texture* t);
VkSampler gles2_vk_sampler(const G2SamplerState* state, bool integer);
/* the buffer's range as commands read it in the frame of slot; writes: the GPU writes it */
bool gles2_buffer_ref(GlesSlot* slot, G2Buffer* b, bool writes, G2Ref* out);
/* the bytes the CPU reads of a buffer: the GPU's writes brought back first */
const unsigned char* gles2_buffer_bytes(G2Buffer* b);
/* a view of the texture's levels [base, base + levels) and layers [layer, layer + layers), of a type */
VkImageView gles2_texture_view(G2Texture* t, VkImageViewType type, int base, int levels, int layer, int layers,
                               bool swizzled, VkImageAspectFlags aspect);
/* whether the texture can be sampled: the levels a sampler reads have data. *levels: how many from the base */
bool gles2_texture_complete(const G2Texture* t, int* levels);
/* the aspect of a format's image, and its layout at rest */
VkImageAspectFlags gles2_aspect(const G2Format* f);
VkImageLayout gles2_rest_layout(const G2Format* f, bool sampled);
/* the command buffer to copy in, outside rendering: the draw surface's frame */
GlesSlot* gles2_transfer_slot(Gles2Context* gl);
/* the format of a GL internal format (and type, for an unsized one); of the shared layer's index (gl_format.h) */
const G2Format* gles2_format(unsigned internal, unsigned format, unsigned type);
const G2Format* gles2_format_at(int index);
/* the query's Vulkan queries: one begun in the rendering that starts, ended when it ends */
void gles2_queries_begin(Gles2Context* gl, VkCommandBuffer cb);
void gles2_queries_end(Gles2Context* gl, VkCommandBuffer cb);
/* the texture of a unit and target, or NULL */
G2Texture* gles2_unit_texture(Gles2Context* gl, unsigned unit, unsigned target);

/* the image unit's view for a storage image of dimension dim (G2_T*), its texture in GENERAL; NULL without one */
VkImageView gles2_image_unit(Gles2Context* gl, unsigned unit, uint8_t dim);
/* a barrier of every write before it to every read and write after it, outside rendering (glMemoryBarrier, a
   dispatch's ends) */
void gles2_memory_barrier(VkCommandBuffer cb);

/* gles2_program.c */
/* the shared layer's programs tell this file when a link is gone, and say what does not compile or link */
void gles2_programs_init(void);
/* the program a draw (or a dispatch, compute) uses: the current one, else the bound program pipeline's; 0 for none */
uint32_t gles2_draw_program(Gles2Context* gl, bool compute);
/* a linked program's Vulkan objects, made now for its link when they are not: NULL (said once) when Vulkan refuses */
G2ProgramVk* gles2_program_vk(uint32_t name);
/* each stage's default block of a program written to the frame's arena when it changed or the frame is new: true when
   every one is there */
bool gles2_program_defaults(GlesSlot* slot, uint32_t name);

/* gles2_draw.c */
/* the render target of the bound draw framebuffer, open in the frame: false when it cannot be drawn into */
bool gles2_rendering_begin(Gles2Context* gl);
/* the rendering of the frame ended, if open (before a copy into its images) */
void gles2_rendering_stop(Gles2Context* gl);
/* the pipelines made for a program's link, retired before it links again or goes */
void gles2_pipelines_forget(uint32_t program);
/* the textures and renderbuffers that a framebuffer's views were made of are gone or changed */
void gles2_target_forget(void);
/* glReadPixels's copy of a rectangle of the read framebuffer into host-cached memory, then into pixels */
void gles2_read_pixels(Gles2Context* gl, int x, int y, int width, int height, unsigned format, unsigned type,
                       uint64_t pixels);

/* a function of the tables (hle_gles2 and the others of hle.h): data is a G2Fn, called with the calling thread's
   context under the lock; without a context the call does nothing and returns 0 */
typedef void (*G2Fn)(CPU* c, Gles2Context* gl);
void gles2_call(CPU* c, void* data);
#define FN(name, fn) {name, gles2_call, (void*)fn}
#endif
