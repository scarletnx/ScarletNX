/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's replay of a queue's submissions on the GPU thread (gpu/nvn/nvn_replay.c) */
#ifndef SNX_NVN_REPLAY_H
#define SNX_NVN_REPLAY_H
#include "nx/gpu/nvn/nvn_obj.h"
#include "nx/gpu/nvn/nvn_draw.h"
#include "common/filesystem/host.h"

/* a new queue's replay state: its index (1 up), whose syncpoint its fences count on */
u32 nvn_replay_queue(void);
u32 nvn_replay_syncpt(u32 queue);
/* the syncpoint the presents count up on as they reach the display (nvn_replay_present), every queue's */
u32 nvn_replay_shown_syncpt(void);
/* Queues a submission of n recordings (their handles, the array at guest address handles, copied) for the GPU
   thread, in order with the queue's other work, and promises the syncpoint increments of the fences the recordings
   hold, fencing those syncs now (each signals once its point is replayed). *ready: when the queue's work is done on
   the pacer's clock; *shown: the value of nvn_replay_shown_syncpt it waits for too (0 none). Both come back held
   behind the syncs the recordings wait for (nvnCommandBufferWaitSync), for the queue's later work */
void nvn_replay_submit(u32 queue, u32 n, u64 handles, u64* ready, u32* shown);
/* work that waits for sync s (if it is fenced) is done no earlier than s signals: *ready and *shown (as above) held
   behind s's */
void nvn_sync_hold(u64* ready, u32* shown, const NvnSync* s);
/* a fence after the work submitted so far: the sync signals once the replay reaches it */
void nvn_replay_fence(u32 queue, u64 sync, u64 ready, u32 shown);
/* returns once the queue's work submitted so far is replayed and done (a fence of its own, waited for) */
void nvn_replay_finish(u32 queue);
/* A window texture to show: its storage's GPU address (0: black), size, the vi pixel format and layout of its bytes
   (block height log2 for a block-linear one, row pitch for a pitch-linear one), the crop (left, top, right, bottom;
   all 0 for the whole texture), and for the renderer's display times and holds (renderer.h present_buffer) when the
   game was given the texture to draw the frame in and the game's pace (0 none) */
typedef struct {
    u64 gva;
    u32 w, h, fmt, linear, bhl, pitch;
    i32 crop[4];
    u64 released_ns, period_ns;
} NvnReplayPresent;
/* shown once the work before it is replayed. Returns the value the queue's syncpoint reaches once it is shown, and in
   *shown the value nvn_replay_shown_syncpt reaches once it is on the display (or known never to be) */
u32 nvn_replay_present(u32 queue, const NvnReplayPresent* p, u32* shown);

/* ---- the file's state, g_nvn.replay (gpu/nvn/nvn_state.h) */
#define NVN_QUEUES      8
/* the effects a submission may have (each fence and event it signals), and the work queued for the GPU thread at
   once: submissions, fences and presents, each with up to NVN_JOB_HANDLES recordings. Five games measured at most
   17 effects, 71 pieces of work queued at once (at a start, while the GPU thread translated shaders) and 115
   recordings in a submission */
#define NVN_EFFECTS     1024
#define NVN_JOBS        512
#define NVN_JOB_HANDLES 512
/* a queue's replay state: its rendering and copy state (gpu/nvn/nvn_draw.c, nvn_copy.c), and the syncpoint its
   fences count */
typedef struct {
    NvnDraw d;
    i64 stride[2]; /* the copies' row and image strides (gpu/nvn/nvn_copy.c) */
    u32 syncpt;
} NvnReplayQueue;
/* a submission's effect on what the CPU sees (nvn_replay.c's FX_*), at gva */
typedef struct {
    u32 kind, len;
    u64 gva;
    u8 data[16];
} NvnEffect;
/* work for the GPU thread (nvn_replay.c's JOB_*): its queue (1 up, so a slot in use is never all zero), and a
   submission's n recordings or a present */
typedef struct {
    u32 queue, kind, n;
    NvnReplayPresent present;
    u64 handles[NVN_JOB_HANDLES];
} NvnJob;
/* the queues (append only: a queue is never given back), the lock that keeps their submissions' promises in the
   order they are queued and that the work's pool is taken from and given back to under, that pool (an all-zero slot
   is free), and the GPU thread's list of the submission being replayed's effects */
typedef struct {
    NvnReplayQueue q[NVN_QUEUES];
    u32 n;
    PMutex lock;
    NvnJob jobs[NVN_JOBS];
    u32 next_job; /* where the search for a free slot starts (hle/pool.h) */
    NvnEffect fx[NVN_EFFECTS];
    u32 nfx;
    u64 unknown; /* the ops logged as not replayed (GPU thread) */
    u32 shown;   /* the presents' syncpoint: up by one as each reaches the display, 0 before the first queue */
} NvnReplayState;

#endif
