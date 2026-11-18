/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The guest's Vulkan on the host's (the guest_vulkan setting). A game that links NVIDIA's Vulkan driver calls the entry
   points of the loader in the driver's module. gvk.c puts a function of its own into the dispatch slot of each one
   (gvk.hook routes the direct calls to them), and gvk_api.c calls the host's Vulkan on the renderer's device with the
   game's arguments. The driver itself never runs, and nothing goes through nvdrv or the Maxwell front end.

   The structures of Vulkan have one layout on AArch64 and on every 64-bit little-endian host: pointers and handles of
   8 bytes, enums of 4, and natural alignment. A structure with no pointer in it is read where the game wrote it. A
   structure with pointers is copied into scratch memory of the calling thread with each guest pointer made a host one
   (gvk_in), its pNext chain too. Handles pass through as they are: they are the host's own */
#ifndef SNX_GVK_H
#define SNX_GVK_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/memory/mem.h"
#include <stddef.h>

/* once, after hook_install, with the guest_vulkan setting: the slot of each Vulkan function the game exports takes
   gvk's, and gvk_api's table names which it implements */
void gvk_install(void);
/* 1 once gvk took the game's Vulkan functions: a save state can't hold the host's objects they made */
int gvk_active(void);

/* ---- between gvk.c and gvk_api.c ---- */

/* gvk_api.c's function for the Vulkan function name, NULL for one it doesn't implement */
GuestFn gvk_api_find(const char* name);

/* A pointer field of a structure, which gvk_in copies what it points to for. A structure's fields end at kind 0 */
enum {
    GVK_ONE = 1, /* one element */
    GVK_ARRAY,   /* count elements, count at count_off (count_size bytes) */
    GVK_BYTES,   /* count bytes, count at count_off (count_size bytes) */
    GVK_STR,     /* a string */
    GVK_EMBED    /* not a pointer: a structure inside this one, at off, whose own pointers are fixed */
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
/* count elements of type t at guest address g, copied to scratch with every pointer in them a host one: NULL for g 0,
   or for memory the game hasn't mapped (said once a type) */
void* gvk_in(const GvkType* t, u64 g, u64 count);
/* an output structure with a pNext chain: the guest's chain copied as gvk_in does, then once the host has filled the
   copy, each structure's fields written back over the guest's, with the guest's own pNext kept */
void gvk_out(const GvkType* t, u64 g, void* host);
/* a pNext chain at guest address g copied as gvk_in copies one, NULL for none */
void* gvk_chain_in(u64 g);
/* the host address of guest address a, NULL for 0: the caller checks first that the game mapped what it reads */
static inline void* gvk_ptr(u64 a) { return a ? (void*)GP(a) : NULL; }
/* the export of the game's Vulkan function name, 0 when it exports none */
u64 gvk_export(const char* name);
/* A game's SPIR-V module of bytes at w (4-aligned) with what the console's driver takes and Vulkan doesn't mended
   (gvk_spirv.c), in the calling thread's scratch memory: NULL when nothing needs it, or for no SPIR-V gvk reads.
   - an ArrayStride on a structure's member (one game's push constants hold an int[8] so, with no stride on the array
     type): the stride goes on the member's array type, where Vulkan reads it, when that type has none.
   - a matrix member with a MatrixStride and neither RowMajor nor ColMajor (3,700 of one game's 4,000 modules): it
     gets ColMajor, which GLSL's compilers mean by none, and what that game's matrices are (RowMajor tilts its
     scenes).
   - a pointer into an array of uniform or storage buffers by an index that isn't a constant: NonUniform (with
     SPV_EXT_descriptor_indexing's capabilities). One game picks each vertex's matrices from an array of four uniform
     buffers by the vertex's matrix index. The console's GPU takes each vertex's own buffer, and AMD's and lavapipe
     take the first vertex's of the group unless told: that game's planets drew as long thin triangles */
const u32* gvk_spirv_mend(const u32* w, size_t bytes, size_t* out_bytes);
/* A fragment module (as gvk_spirv_mend takes one) whose outputs at Location 1 are at Location 0, Index 1: the
   second color of dual-source blending, where Vulkan reads it. The console's driver takes it at Location 1 (one
   game's blends of SRC1_ALPHA). NULL for a module with no such output, for one with an entry point of another stage,
   and for one that gives an output at Location 1 an Index itself */
const u32* gvk_spirv_dual_source(const u32* w, size_t bytes, size_t* out_bytes);

/* the structures gvk_api.c copies (gvk.c's tables) */
extern const GvkType gvk_t_submit, gvk_t_buffer, gvk_t_image, gvk_t_image_view, gvk_t_buffer_view, gvk_t_sampler,
    gvk_t_shader, gvk_t_cache, gvk_t_playout, gvk_t_dslayout, gvk_t_dspool, gvk_t_dsalloc, gvk_t_write, gvk_t_copy,
    gvk_t_rpass, gvk_t_fb, gvk_t_gpipe, gvk_t_cpipe, gvk_t_begin, gvk_t_rpbegin, gvk_t_mbar, gvk_t_bbar, gvk_t_ibar,
    gvk_t_pool, gvk_t_cballoc, gvk_t_fence, gvk_t_sem, gvk_t_query, gvk_t_event, gvk_t_range, gvk_t_imreq2,
    gvk_t_bufreq2, gvk_t_req2;

#endif
