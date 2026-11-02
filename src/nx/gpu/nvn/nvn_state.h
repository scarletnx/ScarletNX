/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's state, as the HLE's is g_hle (hle/hle.h): every variable of static duration the files of gpu/nvn/ keep
   is a member of g_nvn, one struct per file (g_nvn.cmd is nvn_cmd.c's), whose type is declared in the header beside
   that file. (nvn.h is the vendored NVN header, so the global has this header of its own.)
   g_nvn has no initializer, so it is all zero in .bss: nvn_init sets what starts out otherwise (the locks), first
   thing in the program, before anything reads or writes g_nvn. Only .c files include this header */
#ifndef SNX_NVN_STATE_H
#define SNX_NVN_STATE_H
#include "nx/gpu/nvn/nvn_cmd.h"
#include "nx/gpu/nvn/nvn_copy.h"
#include "nx/gpu/nvn/nvn_loader.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "nx/gpu/nvn/nvn_object.h"
#include "nx/gpu/nvn/nvn_queue.h"
#include "nx/gpu/nvn/nvn_replay.h"
#include "nx/memory/mem.h"

typedef struct {
    NvnLoaderState loader;
    NvnObjectState object;
    NvnCmdState cmd;
    NvnQueueState queue;
    NvnReplayState replay;
    NvnDrawState draw;
    NvnCopyState copy;
} Nvn;

extern Nvn g_nvn;
void nvn_init(void);

/* A guest pointer, size_t or ptrdiff_t that the game keeps in memory at at: 4 bytes in a 32-bit game, 8 in a 64-bit
   one. nvn_ptr_bytes is its size, the stride of an array of them */
static inline u64 nvn_ptr_bytes(void) { return g_nvn.loader.a32 ? 4 : 8; }
static inline u64 nvn_gptr(u64 at) { return g_nvn.loader.a32 ? gr32(at) : gr64(at); }
static inline void nvn_gw_isize(u64 at, i64 v) {
    if (g_nvn.loader.a32)
        gw32(at, (u32)v);
    else
        gw64(at, (u64)v);
}
#endif
