/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's command buffers (gpu/nvn/nvn_cmd.c): the game's NVN commands recorded into its command memory. The
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
    u32 live;          /* its pieces in use, the one being cut included */
    u32 reset;         /* the serial of the last piece cut when it was last cut from its start again */
    u64 quiet;         /* the submissions made when its pieces last all came back: it is cut from its start again
                          once the GPU thread has read that many (nvn_replay_walked) */
} NvnSpillSeg;
/* a piece in use: where its recording starts in the game's command memory as the driver counts it (anchor) and in its
   control memory (canchor, 0 none), its segment, the order it was cut in, the next piece of its bucket by anchor (or
   the next free slot) and by canchor, 0 for none (slots count from 1), and NVN_PIECE_NO_COMMAND */
typedef struct {
    u64 anchor, canchor;
    u32 seg, serial, next, cnext, flags, pad;
} NvnSpillPiece;
/* its recording counted no bytes of command memory: the driver keeps it all in control memory, so only a look at the
   control memory gives it back */
#define NVN_PIECE_NO_COMMAND 1u
/* A command buffer's control memory as the game's driver counts it: the memory last added [start, limit + 0x10), the
   cursor, the cursor at the last look, where the open recording's entries start, the serial at the last look, whether
   a block of command ranges is open, whether commands were counted since the last range entry, and whether the game
   lets the driver call its callback when the memory is full (SetControlMemoryCallbackEnabled) */
typedef struct {
    u64 cb; /* 0: a free slot */
    u64 start, cursor, limit, seen, rec;
    u32 seen_serial, block, pending, grow;
} NvnCtl;
#define NVN_CTL_SLOTS     4096
#define NVN_SPILL_SEGS    4096 /* 256 MiB: a segment waits for the GPU thread, which runs seconds late at loads */
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
    u32 bucket[NVN_SPILL_BUCKETS], cbucket[NVN_SPILL_BUCKETS];
    NvnCtl ctl[NVN_CTL_SLOTS];
    u32 ns, np, slots, free, serial, hint;
    u64 total, empty;
} NvnCmdState;

/* the game finalizes the pool of [memory, memory + size): the runtime's pieces of recordings there come back */
void nvn_cmd_memory_back(u64 memory, u64 size);
/* what the runtime's record memory knows of address p, for the message of a record nvn never writes: the
   segment it is in, how far it is cut, and the pieces in use there, into out (n bytes); empty when p isn't in one */
void nvn_cmd_describe(u64 p, char* out, size_t n);
/* whether p is in the piece of the runtime's memory a command buffer is cutting now: its records past the ones written
   so far aren't there yet */
int nvn_cmd_unfinished(u64 p);

#endif
