/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's command buffers (gpu/nvn/nvn_cmd.c): the game's NVN commands recorded into its command memory. The
   functions are the NVN functions' (gpu/nvn/nvn.hook.h); here is the file's state, and what the pools tell it */
#ifndef SNX_NVN_CMD_H
#define SNX_NVN_CMD_H
#include "common/cpu/aarch64/cpu.h"
#include "common/filesystem/host.h"

/* A segment of the runtime's own memory for the records the game's command memory has no room for (nvn_cmd.c's
   spill): pieces are cut from it one after another, each for one recording. A segment whose pieces all came back is
   cut again from its start; segments are never given back to the kernel region */
typedef struct {
    u64 at, size, top; /* top: the end of the pieces cut so far */
    u64 owner;         /* the command buffer cutting a piece from it now, 0 none */
    u32 live, pad;     /* its pieces in use, the one being cut included */
} NvnSpillSeg;
/* a piece in use: where its recording starts in the game's memory as the driver counts it (anchor), its segment, the
   order it was cut in, and the next piece of its bucket (or the next free slot), 0 for none: slots count from 1 */
typedef struct {
    u64 anchor;
    u32 seg, serial, next, pad;
} NvnSpillPiece;
#define NVN_SPILL_SEGS    1024
#define NVN_SPILL_PIECES  65536
#define NVN_SPILL_BUCKETS 4096 /* the pieces by the 4 KiB page of their anchor */
/* the file's state, g_nvn.cmd (gpu/nvn/nvn_state.h), under the lock (the game's threads record into several command
   buffers at once): the segments (append only), the pieces' slots and their buckets, the pieces in use (np), the slots
   ever taken and the first free one, the serial of the last piece cut, the segment the last cut came from, the bytes of
   the kernel region taken, and the runtime's one empty recording */
typedef struct {
    PMutex lock;
    NvnSpillSeg s[NVN_SPILL_SEGS];
    NvnSpillPiece p[NVN_SPILL_PIECES + 1];
    u32 bucket[NVN_SPILL_BUCKETS];
    u32 ns, np, slots, free, serial, hint;
    u64 total, empty;
} NvnCmdState;

/* the game finalizes the pool of [memory, memory + size): the runtime's pieces of recordings there come back */
void nvn_cmd_memory_back(u64 memory, u64 size);

#endif
