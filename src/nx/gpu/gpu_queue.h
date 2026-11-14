/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The queue from the GPU front end to the renderer's side (gpu_queue.c): the front end's writes to guest memory go
   into it in their order with the draws, so a draw that the renderer sets up later reads memory as the front end left
   it at that draw. Each record has a sequence number, its fence: the front end waits only for the fence of the record
   that it needs, never for the whole queue */
#ifndef SNX_GPU_QUEUE_H
#define SNX_GPU_QUEUE_H

#include "nx/gpu/gpu.h"

/* 1 when this thread's guest writes go into the queue: the front end's thread, outside the queue's own records */
int gq_queues(void);
/* a write of size bytes at src to guest memory at gva, a release (gpu_release_write) and a block linear
   inline-to-memory upload over [at, at + size), each in its place in the queue */
void gq_write(u64 gva, const void* src, u64 size);
void gq_release(u64 gva, const void* data, u32 len);
void gq_i2m_block(const GpuI2m* t, u64 at, u64 size);
/* the front end's read of [gva, gva + size): it waits for the fences of the writes queued over those pages */
void gq_read_wait(u64 gva, u64 size);
/* every queued record done, for the front end's points that need the whole renderer side */
void gq_drain(void);

/* the work of each record, and the front end's thread, in gpu.c */
int gpu_on_thread(void);
int gmem_write_now(u64 gva, const void* src, u64 size);
void gpu_release_write_now(u64 gva, const void* data, u32 len);
void gpu_i2m_block_now(const GpuI2m* t);

#endif
