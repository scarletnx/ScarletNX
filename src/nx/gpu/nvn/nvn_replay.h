/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's replay of a queue's submissions on the GPU thread (gpu/nvn/nvn_replay.c) */
#ifndef SNX_NVN_REPLAY_H
#define SNX_NVN_REPLAY_H
#include "common/cpu/aarch64/cpu.h"

/* a new queue's replay state: its index (1 up), whose syncpoint its fences count on */
u32 nvn_replay_queue(void);
u32 nvn_replay_syncpt(u32 queue);
/* Queues a submission of n recordings (their handles, the array at guest address handles, copied) for the GPU
   thread, in order with the queue's other work, and promises the syncpoint increments of the fences the recordings
   hold, fencing those syncs now (each signals once its point is replayed). ready: when the queue's work is done on
   the pacer's clock */
void nvn_replay_submit(u32 queue, u32 n, u64 handles, u64 ready);
/* a fence after the work submitted so far: the sync signals once the replay reaches it */
void nvn_replay_fence(u32 queue, u64 sync, u64 ready);
/* returns once the queue's work submitted so far is replayed and done (a fence of its own, waited for) */
void nvn_replay_finish(u32 queue);
/* the window's texture shown once the work before it is replayed: its storage's GPU address, size, the vi pixel
   format and layout of its bytes (block height log2 for a block-linear one, row pitch for a pitch-linear one), and the
   crop (left, top, right, bottom; all 0 for the whole texture). Returns the value the queue's syncpoint reaches once
   it is shown */
u32 nvn_replay_present(u32 queue, u64 gva, u32 w, u32 h, u32 fmt, u32 linear, u32 bhl, u32 pitch, const i32 crop[4]);

#endif
