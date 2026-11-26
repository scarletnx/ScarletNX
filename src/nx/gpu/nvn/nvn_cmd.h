/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's command buffers (gpu/nvn/nvn_cmd.c): the game's NVN commands recorded into its command memory. The
   functions are the NVN functions' (gpu/nvn/nvn.hook.h); here is the file's state, and what the pools tell it */
#ifndef SNX_NVN_CMD_H
#define SNX_NVN_CMD_H
#include "common/cpu/aarch64/cpu.h"
#include "common/platform/thread.h"
#include "nx/gpu/nvn/nvn_obj.h"

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
    u32 cutter;        /* the control slot + 1 of the command buffer that last cut from it (NvnCmdBuf.ctl) */
} NvnSpillSeg;
/* a piece in use: where its recording starts in the game's command memory as the driver counts it (anchor) and in its
   control memory (canchor, 0 none), its segment, the order it was cut in, the next piece of its bucket by anchor (or
   the next free slot) and by canchor, 0 for none (slots count from 1), NVN_PIECE_NO_COMMAND, and the control memory
   slot of the command buffer that cut it (NvnCmdBuf.ctl, 0 none), which the census names it by. mark: the target of
   the JUMP its recording's handle is, at the aligned anchor (nvn_cmd.c's spill_link), 0 for none.
   A piece moved into the small store has seg NVN_SPILL_SMALL plus its class, and at its block */
typedef struct {
    u64 anchor, canchor, mark, at;
    u64 csig; /* NVN_PIECE_NO_COMMAND: the 8 bytes at canchor as the recording ended (spill_sweep's test) */
    u32 seg, serial, next, cnext, flags, ctl;
} NvnSpillPiece;
/* its recording counted no bytes of command memory: the driver keeps it all in control memory, so only a look at the
   control memory gives it back */
#define NVN_PIECE_NO_COMMAND 1u
/* a piece spill_sweep gives back in bulk: out of both chains in one pass over them, then freed (nvn_cmd.c) */
#define NVN_PIECE_DYING 2u
/* A command buffer's control memory as the game's driver counts it: the memory last added [start, limit + 0x10), the
   cursor, the cursor at the last look, where the open recording's entries start, the serial at the last look, whether
   a block of command ranges is open, whether commands were counted since the last range entry, and whether the game
   lets the driver call its callback when the memory is full (SetControlMemoryCallbackEnabled). The slot also keeps what
   NvnCmdBuf has no room for, about the command memory last added (nvn_cmd.c's ring_look): the recordings ended in it,
   the bytes it looked at, and the serial at its last look; and the segment its last piece came from */
typedef struct {
    u64 cb; /* 0: a free slot */
    u64 start, cursor, limit, seen, rec;
    u32 seen_serial, block, pending, grow;
    u32 command_callback; /* independent of the control-memory callback switch */
    u64 ring_seen;
    u32 ring_recordings, ring_serial, seg_hint;
} NvnCtl;
#define NVN_CTL_SLOTS     4096
#define NVN_SPILL_SEGS    4096   /* 256 MiB: a segment waits for the GPU thread, which runs seconds late at loads */
#define NVN_SPILL_PIECES  262144 /* one game keeps over 23,000 short recordings in the runtime's memory at once */
#define NVN_SPILL_SMALL   NVN_SPILL_SEGS /* NvnSpillPiece.seg of a block of the small store's first class */
#define NVN_SMALL_CLASSES 9              /* blocks of 64 to 16384 bytes */
/* a block of the small store given back, and the submissions made when it came back (its quiet) */
typedef struct {
    u64 at, quiet;
} NvnSmallFree;
/* A class of the small store (nvn_cmd.c's spill_compact): the rest of its last slab [slab, end), and the blocks given
   back, oldest first, given[first] to given[first + n - 1] of an array of cap that grows. Nothing is written in a block
   while it waits there: a submission made before it came back may still walk the records it holds */
typedef struct {
    u64 slab, end;
    NvnSmallFree* given;
    u32 first, n, cap;
} NvnSmallClass;
/* The pieces by the 256 bytes that hold their anchor, hashed. Short recordings put many pieces on a page of command
   memory, and the pools lie gigabytes apart: by the page and modulo the table, one game walked 519 of them each time a
   command buffer looked at what it had written, 13 percent of them in its range */
#define NVN_SPILL_BUCKETS_LOG2 14
#define NVN_SPILL_BUCKETS      (1u << NVN_SPILL_BUCKETS_LOG2)
#define NVN_SPILL_GRANULE      8
#define NVN_SPILL_PART_BUCKET(part) ((u32)(((u64)(part) * 0x9E3779B97F4A7C15ull) >> (64 - NVN_SPILL_BUCKETS_LOG2)))
#define NVN_SPILL_BUCKET(a)    NVN_SPILL_PART_BUCKET((u64)(a) >> NVN_SPILL_GRANULE)
/* the file's state, g_nvn.cmd (gpu/nvn/nvn_state.h), under the lock (the game's threads record into several command
   buffers at once): the segments (append only), the pieces' slots and their buckets, the pieces in use (np), the slots
   ever taken and the first free one, the serial of the last piece cut, the segment the last cut came from, the
   quarters of the slots in use the census has logged, the pieces in use at which spill_sweep looks next, the small
   store's classes, the bytes of the kernel region taken, and the runtime's one empty recording */
typedef struct {
    PMutex lock;
    NvnSpillSeg s[NVN_SPILL_SEGS];
    NvnSpillPiece p[NVN_SPILL_PIECES + 1];
    u32 bucket[NVN_SPILL_BUCKETS], cbucket[NVN_SPILL_BUCKETS];
    NvnCtl ctl[NVN_CTL_SLOTS];
    u32 ns;
    _Atomic u32 np;
    u32 slots, free, serial, hint, census, sweep;
    u32 found;  /* the segment nvn_cmd_unfinished found last */
    u64 lo, hi; /* the lowest start and the highest end of the segments: an address outside is in none */
    NvnSmallClass small[NVN_SMALL_CLASSES];
    u64 total, empty;
} NvnCmdState;

/* An upscale for the command buffer at cb (an OP_UPSCALE record, nvn_obj.h's NvnPUpscale), which a hook module's CPU
   hook asks for (nn/snx_hook_module.h): its textures and views as the game's objects' guest addresses (0 for
   none), flags gpu.h's GPU_UPSCALE_ */
typedef struct {
    u64 tex[NVN_UPSCALE_TEXTURES], view[NVN_UPSCALE_TEXTURES];
    u32 flags, render[2], out[2], render_max[2];
    i32 out_base[2];
    f32 jitter[2], motion_scale[2], pre_exposure;
    int reset;
    f32 reprojection[16]; /* with GPU_UPSCALE_REPROJECTED_MOTION */
} NvnUpscaleRequest;
/* 1 when recorded, 0 when cb isn't a command buffer that records, the color or the output isn't a live texture, a
   view or texture lies in no mapped memory, or a size is 0, each said in the log */
int nvn_cmd_record_upscale(CPU* c, u64 cb, const NvnUpscaleRequest* r);

/* the game finalizes the pool of [memory, memory + size): the runtime's pieces of recordings there come back */
void nvn_cmd_memory_back(u64 memory, u64 size);
/* what the runtime's record memory knows of address p, for the message of a record nvn never writes: the
   segment it is in, how far it is cut, and the pieces in use there, into out (n bytes); empty when p isn't in one */
void nvn_cmd_describe(u64 p, char* out, size_t n);
/* whether p is in the piece of the runtime's memory a command buffer is cutting now: its records past the ones written
   so far aren't there yet */
int nvn_cmd_unfinished(u64 p);

/* ---- the game's current NVN phase, per game thread (nvn_cmd.c) ----
   The game builds and submits commands on several threads. A Tracy zone says which high-level NVN operation a thread
   was in when it emitted the zone. Set the context at the NVN entry points and read it when a zone needs text.
   op is a string literal: the runtime keeps the pointer and never copies or frees it. id is the command buffer or
   queue address when the call has one, else 0. phase is one of the values below. Neither call stores nor formats
   anything when no profiler client is attached. */
enum {
    NVN_TRACE_NONE = 0,
    NVN_TRACE_RECORD = 1
};
void nvn_trace_set(const char* op, u64 id, int phase);
/* Puts the current thread's context into out, capped at cap bytes, with an empty string when no client is attached.
   Returns out. Keep the text under 128 bytes. */
char* nvn_trace_context(char* out, size_t cap);

#endif
