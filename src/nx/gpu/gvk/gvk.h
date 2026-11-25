/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's Vulkan on the host's (the guest_vulkan setting). A game that links NVIDIA's Vulkan driver calls the entry
   points of the loader in the driver's module. gvk.c puts a function of its own into the dispatch slot of each one
   (gvk.hook routes the direct calls to them), and gvk_api.c calls the host's Vulkan on the renderer's device with the
   game's arguments. The driver's code runs only for a call that no slot takes, and nothing goes through nvdrv or the
   Maxwell front end. Where gvk cannot run (gvk_check_device), the slots go back to the driver.

   The structures of Vulkan have one layout on AArch64 and on every 64-bit little-endian host: pointers and handles of
   8 bytes, enums of 4, and natural alignment. A structure with no pointer in it is read where the game wrote it. A
   structure with pointers is copied into scratch memory of the calling thread with each guest pointer made a host one
   (gvk_in), its pNext chain too. Handles pass through as they are: they are the host's own */
#ifndef SNX_GVK_H
#define SNX_GVK_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/memory/mem.h"
#include <stddef.h>

/* Reserve space for GVK entry points that are not exports of the guest driver. */
u64 gvk_proc_space(u64 code_hi);
/* once, after hook_install, with the guest_vulkan setting: the slot of each Vulkan function the game exports takes
   gvk's, and gvk_api's table names which it implements */
void gvk_install(void);
/* once, after gpu_init and before the game's first thread: if the host device cannot back guest-mapped memory, warn
   and restore the game's own driver. Else stop the game when the recompile routes no direct calls to a function gvk
   implements */
void gvk_check_device(void);
/* Dispatch entry to keep across JIT invalidation, or NULL if there is no executable GVK entry here. */
GuestFn gvk_invalidation_slot(u64 addr);
/* 1 once gvk took the game's Vulkan functions: a save state can't hold the host's objects they made */
int gvk_active(void);

/* ---- between gvk.c and gvk_api.c ---- */

/* gvk_api.c's function for the Vulkan function name, NULL for one it doesn't implement */
GuestFn gvk_api_find(const char* name);
/* 1 if the renderer's Vulkan device can back gvk's guest-mapped memory, 0 otherwise */
int gvk_api_supported(void);

/* A pointer field of a structure, which gvk_in copies what it points to for. A structure's fields end at kind 0 */
enum {
    GVK_ONE = 1,        /* one element */
    GVK_ARRAY,          /* count elements, count at count_off (count_size bytes) */
    GVK_BYTES,          /* count bytes, count at count_off (count_size bytes) */
    GVK_STR,            /* a string */
    GVK_EMBED,          /* not a pointer: a structure inside this one, at off, whose own pointers are fixed */
    GVK_OPTIONAL_ARRAY, /* Vulkan may ignore this array even with a nonzero count */
    GVK_OPTIONAL_ONE,   /* Vulkan may ignore this pointer depending on state (e.g. primary CB, no tessellation) */
    GVK_SAMPLE_MASK,    /* optional ceil(rasterizationSamples / 32) words */
    GVK_DRAW_ONE        /* GVK_ONE, ignored where the rasterization state at count_off discards */
};
typedef struct GvkType GvkType;
typedef struct {
    u16 off;
    u8 kind;
    u8 count_size;
    u16 count_off;
    u16 elem;            /* plain elements (type NULL): each one's size */
    const GvkType* type; /* the elements' structure, NULL for plain ones */
} GvkField;
#define GVK_FIELDS 11
struct GvkType {
    const char* name;
    u32 size;
    u32 stype; /* its VkStructureType, with sType at 0 and pNext at 8; 0 for a structure with neither */
    GvkField f[GVK_FIELDS];
};

/* the calling thread's scratch memory, 16-aligned, valid until its next gvk_scratch_reset (each call's start) */
void* gvk_scratch(size_t n);
void gvk_scratch_reset(void);
void gvk_scratch_free(void);
/* Copy count structures and translate their pointers. Returns NULL for an empty input or a malformed copy. */
void* gvk_in(const GvkType* t, u64 g, u64 count);
/* an output structure with a pNext chain: the guest's chain copied as gvk_in does, then once the host has filled the
   copy, each structure's fields written back over the guest's, with the guest's own pNext kept */
void gvk_out(const GvkType* t, u64 g, void* host);
/* An empty chain succeeds with *out == NULL, a malformed chain fails. */
int gvk_chain_in(u32 parent, u64 g, const void** out);
int gvk_writable(u64 g, u64 size);
/* the host address of guest address a, NULL for 0: the caller checks first that the game mapped what it reads */
static inline void* gvk_ptr(u64 a) { return a ? (void*)GP(a) : NULL; }
/* the export of the game's Vulkan function name, 0 when it exports none */
u64 gvk_export(const char* name);
/* The first of the n strings at the game's array names that is none of the nlisted at listed, step bytes apart: 1 and
   the string in name (size bytes), "" for one that isn't a string the game mapped. 0 when all are listed */
int gvk_unlisted(u64 names, u32 n, const char* listed, size_t step, u32 nlisted, char* name, size_t size);
/* The n VkWriteDescriptorSet at writes (host memory) each with the one of its three arrays that its type reads, the
   others cleared, and none for a write of no descriptors. 0, said, when a write lacks its array */
int gvk_writes_take(void* writes, u32 n);
/* A game's SPIR-V module of bytes at w (4-aligned) with what the console's driver takes and Vulkan doesn't mended
   (gvk_spirv.c), in the calling thread's scratch memory.
   - an ArrayStride on a structure's member (one game's push constants hold an int[8] so, with no stride on the array
     type): the stride goes on the member's array type, where Vulkan reads it, when that type has none.
   - a matrix member with a MatrixStride and neither RowMajor nor ColMajor (3,700 of one game's 4,000 modules): it
     gets ColMajor, which GLSL's compilers mean by none, and what that game's matrices are (RowMajor tilts its
     scenes).
   - a pointer into an array of uniform or storage buffers, textures, samplers or other images by an index that isn't
     the same in every invocation (a constant, or push constants' and a uniform buffer's value): NonUniform, with
     SPV_EXT_descriptor_indexing's capabilities, and so is what is loaded and sampled through it. One game picks each
     vertex's matrices from an array of four uniform buffers by the vertex's matrix index. The console's GPU takes
     each vertex's own buffer, and AMD's and lavapipe take the first vertex's of the group unless told: that game's
     planets drew as long thin triangles. Vulkan gives a host the same freedom with an image or a sampler */
enum {
    GVK_SPIRV_UBO_NONUNIFORM = 1,
    GVK_SPIRV_SSBO_NONUNIFORM = 2,
    /* arrays of the images gvk_spirv.c marks where the host can index them so, and leaves as they are where it can't */
    GVK_SPIRV_SAMPLED_NONUNIFORM = 4, /* samplers, sampled images and combined ones */
    GVK_SPIRV_STORAGE_IMAGE_NONUNIFORM = 8,
    GVK_SPIRV_INPUT_NONUNIFORM = 16,
    GVK_SPIRV_UNIFORM_TEXEL_NONUNIFORM = 32,
    GVK_SPIRV_STORAGE_TEXEL_NONUNIFORM = 64,
    GVK_SPIRV_NONUNIFORM_ALL = 127
};
/* Returns w if unchanged, a scratch copy if mended, or NULL if the module cannot be forwarded. */
const u32* gvk_spirv_mend(const u32* w, size_t bytes, size_t* out_bytes, u32 allow);
/* A fragment module (as gvk_spirv_mend takes one) whose outputs at Location 1 are at Location 0, Index 1: the
   second color of dual-source blending, where Vulkan reads it. The console's driver takes it at Location 1 (one
   game's blends of SRC1_ALPHA). NULL for a module with no such output, for one with an entry point of another stage,
   and for one that gives an output at Location 1 an Index itself */
const u32* gvk_spirv_dual_source(const u32* w, size_t bytes, size_t* out_bytes);
/* The size queries of a fragment module's images (OpImageQuerySize and OpImageQuerySizeLod) answer the game's size
   of an image gvk draws at the resolution scale, where the host's answer is its own. A game's shaders divide by the
   size to sample at texels of the game's size (one game's indirect textures and haze, of copies of its targets). The
   module gets a uvec4 in its push constants at byte GVK_SIZE_PUSH: x a bit for each image it queries, set when that
   image is drawn at the scale, and y the scale less one. Each query's width and height are divided by 1 + bit * y.
   slots names bit k's image as set << 14 | binding << 8 | array element, for bindings and elements below 8, which
   gvk follows in descriptor sets. A query of an image gvk can't name (another binding or element, more than
   GVK_SIZE_SLOTS, an element by a variable, a function's parameter, a result of other than 32 bits) stays the host's,
   counted in skipped.
   Returns w with slots->n 0 where nothing changes, a scratch copy with the queries divided, or NULL for a module that
   isn't SPIR-V gvk can read */
#define GVK_SIZE_SLOTS 16
#define GVK_SIZE_PUSH  240
typedef struct {
    u8 n, skipped;
    u16 slot[GVK_SIZE_SLOTS];
} GvkSizeSlots;
const u32* gvk_spirv_scaled_size(const u32* w, size_t bytes, size_t* out_bytes, GvkSizeSlots* slots);

/* the structures gvk_api.c copies (gvk.c's tables) */
extern const GvkType gvk_t_submit, gvk_t_buffer, gvk_t_image, gvk_t_image_view, gvk_t_buffer_view, gvk_t_sampler,
    gvk_t_shader, gvk_t_cache, gvk_t_playout, gvk_t_dslayout, gvk_t_dspool, gvk_t_dsalloc, gvk_t_write, gvk_t_copy,
    gvk_t_rpass, gvk_t_fb, gvk_t_gpipe, gvk_t_cpipe, gvk_t_begin, gvk_t_rpbegin, gvk_t_mbar, gvk_t_bbar, gvk_t_ibar,
    gvk_t_pool, gvk_t_cballoc, gvk_t_fence, gvk_t_sem, gvk_t_query, gvk_t_event, gvk_t_imreq2, gvk_t_bufreq2,
    gvk_t_req2, gvk_t_template, gvk_t_bind_buffer, gvk_t_bind_image;

#endif
