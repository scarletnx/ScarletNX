/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Command-record relocation from nxnative (MIT), Copyright (c) 2026 seedds: dm/gfx/nvn/hle_nvn.c */
/* nvn's NVN objects (gpu/nvn/nvn_object.c): the device, memory pools, buffers, textures, samplers, programs
   and state objects, and their builders. What the command buffer and queue files share with it */
#ifndef SNX_NVN_OBJECT_H
#define SNX_NVN_OBJECT_H
#include "nx/gpu/nvn/nvn_obj.h"
#include "nx/memory/mem.h"
#include "nx/gpu/shader/shader.h"
#include "common/filesystem/host.h"

/* A record's snapshot pointers (a texture object copied into the record, tex_snap) that point into the record at src,
   moved to its copy at dst */
static inline void nvn_record_relocate(u64 dst, u64 src, u32 op, u32 size) {
    static const struct {
        u32 op, off, n;
    } snaps[] = {
        {OP_SET_RT, offsetof(NvnPSetRT, color), 8},         {OP_SET_RT, offsetof(NvnPSetRT, depth), 1},
        {OP_COPY_B2T, offsetof(NvnPCopyB2T, texture), 1},   {OP_COPY_T2B, offsetof(NvnPCopyB2T, texture), 1},
        {OP_COPY_T2T, offsetof(NvnPCopyT2T, src), 1},       {OP_COPY_T2T, offsetof(NvnPCopyT2T, dst), 1},
        {OP_CLEAR_TEX, offsetof(NvnPClearTex, texture), 1},
    };
    size_t i;
    u32 k;
    for (i = 0; i < sizeof snaps / sizeof *snaps; i++)
        for (k = 0; snaps[i].op == op && k < snaps[i].n; k++) {
            u64 at = dst + 8 + snaps[i].off + 8 * (u64)k, v = gr64(at);
            if (v >= src && v < src + size) gw64(at, v - src + dst);
        }
}

#define NVN_DEVICE_INFO_ITEMS 0x69
/* a program whose last stage before rasterization captures: its serial and its transform feedback layout (128 bytes,
   which don't fit the program object) */
typedef struct {
    u64 serial;
    ShXfb x;
} NvnXfbEntry;
typedef struct {
    u64 serial;
    NvnTessStage stage[2]; /* control, evaluation */
} NvnTessEntry;
/* Immutable subroutine maps contain offsets into their own byte payload. Handles are one-based and survive
   command recordings that retain an older program after SetShaders or control-memory reuse. */
typedef struct {
    u64 serial;
    u32 required, linkage[6];
} NvnSubProgram;
typedef struct {
    NvnStage shader;
    u32 stage, absolute, table, calls, entries, labels;
    u32 call_offset, code_offset, function_offset, label_offset;
    u32 bytes;
    u8 data[];
} NvnSubLinkage;
/* the file's state, g_nvn.object (gpu/nvn/nvn_state.h): the serials handed out, and nvnDeviceGetInteger's answers
   once read from the game's driver (driver: 1; read_api: its API version among them); the transform feedback layouts
   of the programs that capture, by serial, under their lock: programs that capture are few, and their entries stay */
typedef struct {
    u64 serial;
    i32 info[NVN_DEVICE_INFO_ITEMS];
    int driver, read_api;
    u32 formats[8];   /* bit f: a texture of NVN format f was initialized (logged once) */
    u64 device;       /* the device the game made last (atomic): the runtime has one */
    u32 debug_domain; /* the debug domains generated (atomic) */
    u32 device_flags; /* the flags of that device (atomic) */
    PMutex xfb_lock;
    NvnXfbEntry* xfb; /* immutable layouts, sorted by serial and copied out under xfb_lock */
    u32 nxfb, xfb_capacity;
    NvnTessEntry* tess; /* immutable declarations, sorted by serial and copied out under xfb_lock */
    u32 ntess, tess_capacity;
    NvnSubProgram* subprogram;
    u32 nsubprogram, subprogram_capacity;
    NvnSubLinkage** sublinkage;
    u32 nsublinkage, sublinkage_capacity;
} NvnObjectState;

/* an NVN object of type T at guest address a, in place in the game's memory */
#define NVN_OBJ(T, a) ((T*)(void*)GP(a))

/* a new serial for a texture, sampler or program: above 0, never reused */
u64 nvn_serial(void);
/* a memory pool object's storage: its guest address (0 for none), and its GPU address */
u64 nvn_pool_memory(u64 pool);
/* the flags of the device the game made last (the runtime has one) */
u32 nvn_device_flags(void);
/* the device the game made last (the runtime has one), 0 before: what the builders that don't keep one answer */
u64 nvn_device(void);
u64 nvn_pool_gpu(u64 pool);

/* Around a wait for the host inside an NVN call: the game's thread off its guest core and stopped as far as a game
   pause goes, as rt_miss waits for a compile. An NVN call comes straight from the game's code, with no SVC to give the
   core up, and on the console a thread waiting for the GPU leaves its core to the others: one game's main thread held
   its core through the frame's wait for its fence, and a lower-priority loading thread on that core never finished */
void nvn_wait_begin(void);
void nvn_wait_end(void);

/* the transform feedback layout of the program SetShaders gave serial, 0 when it captures nothing */
int nvn_program_xfb(u64 serial, ShXfb* x);
int nvn_program_tess(u64 serial, NvnTessStage stages[2]);
u32 nvn_program_subroutine(u64 program, i32 stage);
int nvn_subroutine_apply(u32 linkage, i32 first, i32 count, const i32* values);
/* whether a linkage map's fields are what its bytes say, every offset and target inside them, as SetSubroutineLinkage
   checks a map before it keeps it: a save state's are checked again (gpu/nvn/nvn_save.c) */
int nvn_sub_linkage_valid(const NvnSubLinkage* l);
/* the game's driver resolved nvnDeviceGetInteger to func: its answers are read from its code (gpu/nvn/nvn_driver.c)
   before the game can ask */
void nvn_device_driver(u64 func);
/* the NVN API version the game's driver answers (nvn_api_pack), 0 until nvn_device_driver read it from the driver's
   code, or when it couldn't */
u32 nvn_device_api(void);

#endif
