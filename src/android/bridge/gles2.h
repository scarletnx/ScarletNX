/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#ifndef SNX_ANDROID_GLES2_H
#define SNX_ANDROID_GLES2_H
/* OpenGL ES 2.0, 3.0 and 3.1 over Vulkan 1.3, as Mesa's zink does it (docs/android/NATIVE.md, "EGL and GLES"), on the
   device and surfaces of gles_vk.h:

   gles2.c          the contexts, the fixed state (enables, blend, depth, stencil, viewport), the queries of state,
                    and the HLE table of libGLESv2 and libGLESv3
   gles2_object.c   buffers, textures and their formats, samplers, renderbuffers, framebuffers, vertex arrays,
                    queries and sync objects
   gles2_program.c  shaders and programs: glslc compiles each stage, the binding map places the resources, the
                    stages are linked by name, and the loose uniforms live in a buffer of each stage
   gles2_draw.c     the render targets (dynamic rendering), the pipelines from the GL state at draw time, the draws,
                    clears, glReadPixels, blits and copies

   The objects of each kind are in one array, by GL name: one share group for every context, as GLES 1.1 has. Each
   call that reaches Vulkan holds g_gles_vk.lock */
#include "android/bridge/gles_vk.h"
#include "android/bridge/hle.h"
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

typedef struct {
    bool used;
    unsigned char* data;   /* what the guest wrote: the copy the CPU reads and writes */
    size_t size;
    unsigned usage;
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
    bool mapped;
    size_t map_offset, map_length;
    unsigned map_access;
} G2Buffer;

typedef struct {
    unsigned min, mag, wrap[3], compare_mode, compare_func;
    float min_lod, max_lod, anisotropy;
} G2SamplerState;

typedef struct {
    bool used;
    G2SamplerState state;
} G2Sampler;

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
enum { G2_FLOAT = 0, G2_INT = 1, G2_UINT = 2, G2_DEPTH = 3, G2_STENCIL = 4, G2_DS = 5 };

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

/* an attachment of a framebuffer: a texture's level and layer, or a renderbuffer */
typedef struct {
    uint8_t kind; /* 0 none, 1 texture, 2 renderbuffer */
    unsigned name;
    int level, layer;
} G2Attachment;
enum { G2_DEPTH_ATTACHMENT = G2_COLORS, G2_STENCIL_ATTACHMENT = G2_COLORS + 1, G2_ATTACHMENTS = G2_COLORS + 2 };

typedef struct {
    bool used;
    G2Attachment at[G2_ATTACHMENTS];
    unsigned draw[G2_COLORS]; /* glDrawBuffers: GL_COLOR_ATTACHMENTi or GL_NONE */
    unsigned read;
    uint32_t generation; /* counts the changes of its attachments */
} G2Framebuffer;

/* an attribute's format, and the binding it reads (GLES 3.1's glVertexAttribBinding; glVertexAttribPointer sets
   binding i of attribute i) */
typedef struct {
    bool enabled, normalized, integer;
    int size;
    unsigned type;
    int stride;        /* glVertexAttribPointer's, for its query */
    uint32_t relative; /* the offset in the binding's vertex */
    uint8_t binding;
} G2Attrib;

typedef struct {
    unsigned buffer;
    uint64_t offset; /* in buffer, or the guest's client array without one */
    uint32_t stride, divisor;
} G2VertexBinding;

typedef struct {
    bool used;
    G2Attrib attribs[G2_ATTRIBS];
    G2VertexBinding bindings[G2_ATTRIBS];
    unsigned elements;
} G2VertexArray;

typedef struct {
    bool used, active;
    unsigned target;
    VkQueryPool pool;
    uint32_t count, capacity; /* the Vulkan queries begun: one for each render pass instance it spans */
    uint64_t serial;          /* the submission whose results it waits for */
    bool ready;
    uint64_t result;
} G2Query;

typedef struct {
    bool used;
    uint64_t serial;
} G2Sync;

typedef struct {
    bool used, deleted, compiled;
    unsigned type; /* GL_VERTEX_SHADER, GL_FRAGMENT_SHADER, GL_COMPUTE_SHADER */
    char* source;
    size_t length;
    char* log;
    unsigned attached; /* how many programs have it */
} G2Shader;

/* a uniform of a program, as GL lists them: a default block's member (of a stage or both), a block's member, or a
   sampler */
typedef struct {
    uint32_t name;       /* the offset of its name in the program's names */
    unsigned type;       /* GL_FLOAT_VEC4, GL_SAMPLER_2D and the others */
    uint16_t count;      /* elements of an array, 1 for others */
    int16_t block;       /* the uniform block's index, -1 for the default block */
    int32_t offset[3];   /* in each stage's default block (or the block), -1 where the stage has none */
    uint16_t array_stride, matrix_stride;
    int32_t location;    /* the first element's, -1 in a block */
    int32_t sampler;     /* the index of its first unit in the program's units, -1 when not a sampler */
} G2Uniform;

/* a descriptor binding of a program's set */
typedef struct {
    uint32_t name;     /* its name in names: a sampler's or a block's */
    uint8_t kind;      /* SNX_GLSLC_REFLECT_SAMPLER, IMAGE, UNIFORM_BLOCK or BUFFER_BLOCK; UNIFORM_BLOCK with an
                          empty name is a stage's default block */
    uint8_t stage;     /* a default block's stage */
    uint8_t dim;       /* a sampler's G2_T*, from its SPIR-V type */
    uint8_t texel_buffer;
    uint16_t count;    /* descriptors */
    uint16_t binding;
    uint16_t first;    /* a sampler's first unit in the program's units; a block's index */
    uint32_t size;     /* a block's bytes */
} G2Binding;

typedef struct {
    uint32_t name;
    unsigned type;
    uint16_t count;    /* elements of an array */
    uint8_t base;      /* GlslcBase */
    uint8_t columns;   /* locations each element takes */
    int32_t location;  /* GL's */
    int32_t spirv;     /* the module's */
} G2Input;

typedef struct {
    bool used, deleted, linked;
    bool separable; /* GL_PROGRAM_SEPARABLE: it links without the other stage */
    bool hidden;    /* a program pipeline's stages linked together, which GL does not name */
    unsigned shaders[3]; /* the vertex, fragment and compute shaders attached */
    char* log;
    /* glBindAttribLocation's: name\0 pairs with their location, at the next link */
    char* bound;
    size_t bound_size;
    /* the link's */
    uint32_t generation; /* counts the links: the pipelines of an older one are stale */
    VkShaderModule modules[3];
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline compute;
    G2Binding* bindings;
    uint32_t binding_count;
    G2Uniform* uniforms;
    uint32_t uniform_count;
    uint32_t location_count; /* the uniform locations: one for each element */
    int32_t* locations;      /* location to uniform index * 65536 + element */
    G2Input* inputs;
    uint32_t input_count;
    G2Input* outputs; /* a fragment shader's, for glGetFragDataLocation */
    uint32_t output_count;
    uint32_t block_count;    /* the uniform blocks: bindings with kind UNIFORM_BLOCK and a name */
    unsigned block_binding[G2_UBO_BINDINGS];
    int32_t* units;          /* each sampler element's texture unit */
    uint32_t unit_count;
    char* names;
    size_t names_size;
    unsigned char* defaults[3]; /* each stage's default uniform block */
    uint32_t default_size[3];
    uint32_t default_binding[3];
    bool default_dirty[3];
    G2Ref default_ref[3];
    uint64_t default_serial[3];
    uint32_t local_size[3];
} G2Program;

/* GLES 3.1's program pipeline: a separable program for each stage. A draw uses a hidden program linked from the
   stages' shaders, its uniforms copied from theirs */
typedef struct {
    bool used;
    unsigned stages[3]; /* the vertex, fragment and compute stages' programs */
    unsigned active;    /* glActiveShaderProgram's, for glUniform* */
    unsigned linked;    /* the hidden program */
    uint32_t generations[2]; /* of the vertex and fragment programs it was linked from */
    unsigned names[2];
} G2ProgramPipeline;

/* the current values of the generic attributes (glVertexAttrib4f) */
typedef struct {
    uint32_t bits[4];
    uint8_t base; /* GlslcBase: FLOAT, INT or UINT */
} G2Generic;

typedef struct {
    unsigned buffer;
    int64_t offset, size;
} G2IndexedBuffer;

/* a context of GLES 2 or 3 */
typedef struct Gles2Context {
    unsigned error;
    int version; /* 2 or 3 */
    GlesSurface* draw;
    GlesSurface* read;
    GlesSurface* own; /* a 1x1 pbuffer, for a context current without a surface */
    bool sized;
    /* the enables */
    bool blend, cull, depth_test, stencil_test, scissor_test, polygon_offset, dither, alpha_to_coverage,
        sample_coverage, rasterizer_discard, primitive_restart;
    unsigned blend_src[2], blend_dst[2], blend_eq[2]; /* [0] RGB, [1] alpha */
    float blend_color[4];
    bool color_mask[4], depth_mask;
    unsigned depth_func;
    float depth_range[2];
    unsigned cull_face, front_face;
    struct {
        unsigned func, fail, zfail, zpass, mask, write;
        int ref;
    } stencil[2]; /* front, back */
    float clear_color[4], clear_depth;
    int clear_stencil;
    int viewport[4], scissor[4];
    float line_width, offset_factor, offset_units;
    float coverage_value;
    bool coverage_invert;
    unsigned hints[2]; /* GL_GENERATE_MIPMAP_HINT, GL_FRAGMENT_SHADER_DERIVATIVE_HINT */
    /* glPixelStorei: pack [0] and unpack [1] */
    struct {
        int alignment, row_length, skip_rows, skip_pixels, image_height, skip_images;
    } pixel[2];
    unsigned active;
    unsigned textures[G2_UNITS][G2_TARGETS];
    unsigned samplers[G2_UNITS];
    unsigned program;
    unsigned program_pipeline;
    /* the bindings */
    unsigned array_buffer, copy_read, copy_write, pixel_pack, pixel_unpack, uniform_buffer, tf_buffer;
    G2IndexedBuffer uniform_buffers[G2_UBO_BINDINGS];
    /* GLES 3.1: the shader storage buffers, the image units, the indirect buffers, the sample mask */
    unsigned storage_buffer, draw_indirect, dispatch_indirect;
    G2IndexedBuffer storage_buffers[G2_SSBO_BINDINGS];
    struct {
        unsigned texture;
        int level, layer;
        bool layered;
        unsigned access, format;
    } images[G2_IMAGE_UNITS];
    bool sample_mask_on;
    uint32_t sample_mask;
    unsigned vertex_array; /* 0: the context's own */
    G2VertexArray default_vao;
    unsigned draw_fbo, read_fbo, renderbuffer;
    G2Generic generic[G2_ATTRIBS];
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
    uint8_t blend, write_mask, pad[2];
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
    G2Buffer* buffers;
    size_t buffer_count;
    G2Texture* textures;
    size_t texture_count;
    G2Sampler* samplers;
    size_t sampler_count;
    G2Renderbuffer* renderbuffers;
    size_t renderbuffer_count;
    G2Framebuffer* framebuffers;
    size_t framebuffer_count;
    G2VertexArray* vertex_arrays;
    size_t vertex_array_count;
    G2Query* queries;
    size_t query_count;
    G2Sync* syncs;
    size_t sync_count;
    G2Shader* shaders;
    size_t shader_count;
    G2Program* programs;
    size_t program_count;
    G2ProgramPipeline* program_pipelines;
    size_t program_pipeline_count;
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
    /* glslc's workspace and module, used under the lock */
    GlslcWorkspace workspace;
    uint32_t* words;
    GlslcResult result;
    bool said[24]; /* the warnings said once */
    char extensions[1024]; /* GL_EXTENSIONS, joined at its first query */
    G2Format astc[28];     /* the ASTC formats' entries, made at their first use */
} Gles2;
extern Gles2 g_gles2;

/* the calling thread's current context, or NULL */
Gles2Context* gles2_current(void);
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
G2Buffer* gles2_buffer(unsigned name);   /* NULL for 0 or a name not made */
G2Texture* gles2_texture(unsigned name);
G2Renderbuffer* gles2_renderbuffer(unsigned name);
G2Framebuffer* gles2_framebuffer(unsigned name);
G2VertexArray* gles2_vertex_array(Gles2Context* gl); /* the bound one */
G2Program* gles2_program(unsigned name);
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
/* the format of a GL internal format (and type, for an unsized one) */
const G2Format* gles2_format(unsigned internal, unsigned format, unsigned type);
/* the texture's image at width, height, depth, levels and layers of format: made again when it differs */
bool gles2_texture_storage(Gles2Context* gl, G2Texture* t, const G2Format* f, int width, int height, int depth,
                           int levels);
/* the query's Vulkan queries: one begun in the rendering that starts, ended when it ends */
void gles2_queries_begin(Gles2Context* gl, VkCommandBuffer cb);
void gles2_queries_end(Gles2Context* gl, VkCommandBuffer cb);
/* the texture of a unit and target, or NULL */
G2Texture* gles2_unit_texture(Gles2Context* gl, unsigned unit, unsigned target);

/* the bytes of a vertex attribute of size components of type, packed */
uint32_t gles2_vertex_bytes(unsigned type, int size);
/* the image unit's view for a storage image of dimension dim (G2_T*), its texture in GENERAL; NULL without one */
VkImageView gles2_image_unit(Gles2Context* gl, unsigned unit, uint8_t dim);
/* a barrier of every write before it to every read and write after it, outside rendering (glMemoryBarrier, a
   dispatch's ends) */
void gles2_memory_barrier(VkCommandBuffer cb);

/* gles2_program.c */
/* the program a draw (or a dispatch, compute) uses: the current one, else the bound program pipeline's; NULL for
   none */
G2Program* gles2_draw_program(Gles2Context* gl, bool compute);
/* the default block's of program p written to the frame's arena when they changed: true when each stage's is */
bool gles2_program_defaults(GlesSlot* slot, G2Program* p);

/* gles2_draw.c */
/* the render target of the bound draw framebuffer, open in the frame: false when it cannot be drawn into */
bool gles2_rendering_begin(Gles2Context* gl);
/* the rendering of the frame ended, if open (before a copy into its images) */
void gles2_rendering_stop(Gles2Context* gl);
/* the pipelines made for a program's link, retired before it links again or goes */
void gles2_pipelines_forget(const G2Program* p);
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
