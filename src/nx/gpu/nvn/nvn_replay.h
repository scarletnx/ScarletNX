/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's replay of a queue's submissions on the GPU thread (gpu/nvn/nvn_replay.c) */
#ifndef SNX_NVN_REPLAY_H
#define SNX_NVN_REPLAY_H
#include "common/cpu/aarch64/cpu.h"

/* a new queue's replay state: its index (1 up), whose syncpoint its fences count on */
u32 nvn_replay_queue(void);
u32 nvn_replay_syncpt(u32 queue);
/* the syncpoint the presents count up on as they reach the display (nvn_replay_present), every queue's */
u32 nvn_replay_shown_syncpt(void);
/* Queues a submission of n recordings (their handles, the array at guest address handles, copied) for the GPU
   thread, in order with the queue's other work, and promises the syncpoint increments of the fences the recordings
   hold, fencing those syncs now (each signals once its point is replayed). ready: when the queue's work is done on
   the pacer's clock; shown: the value of nvn_replay_shown_syncpt it waits for too (0 none) */
void nvn_replay_submit(u32 queue, u32 n, u64 handles, u64 ready, u32 shown);
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

#endif
