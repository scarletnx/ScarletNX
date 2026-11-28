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
/* on: this thread now holds the renderer's lock (vk/device.c's vk_lock), and so runs renderer code, as the renderer's
   side does. Its guest memory writes and reads then go to memory at once, not through the queue. The front end calls
   the renderer only once the queue is empty (gpu_renderer_now), and a wait on the renderer's side under the lock is a
   deadlock: the renderer's side needs the lock to run what is queued (gmem_before_write) */
void gq_direct(int on);
/* a write of size bytes at src to guest memory at gva, a release (gpu_release_write) and a block linear
   inline-to-memory upload over [at, at + size), each in its place in the queue */
void gq_write(u64 gva, const void* src, u64 size);
/* the same for a constant buffer upload, which gmem_write_cb_now stamps with gpu_mem_written_cb */
void gq_write_cb(u64 gva, const void* src, u64 size);
void gq_release(u64 gva, const void* data, u32 len);
/* a release of the GPU's timestamp, 8 bytes at gva, read as the record runs (gpu_release_ticks_now): the front end's
   reads of those bytes wait for it, as its value is not known until then */
void gq_release_ticks(u64 gva);
/* a timestamp report (gpu_release_stamp): the renderer takes its timestamp where the record runs, after the draws
   queued before it, and the ticks at offset 8 are read then too (gpu_release_stamp_ticked) */
void gq_release_stamp(u64 gva, const void* data, int top);
void gq_i2m_block(const GpuI2m* t, u64 at, u64 size);
/* the front end's read of [gva, gva + size): it waits for the fences of the writes queued over those pages */
void gq_read_wait(u64 gva, u64 size);
/* the front end's read served from the newest queued write that holds all of [gva, gva + size): 1 when it did, and
   the read needs no wait. 0 when no such write is queued */
int gq_forward(u64 gva, void* dst, u64 size);
/* every queued record done, for the front end's points that need the whole renderer side */
void gq_drain_at(const char* file, int line);
#define gq_drain() gq_drain_at(__FILE__, __LINE__) /* counted by where it is asked (the stats line) */
void gq_stats_log(u64 presents); /* the stats line's: the front end's waits and the renderer thread's runs */
void gq_present(void);           /* the renderer presented (any thread): gpu_split=2 measures by presents */
u64 gq_presents(void);  /* the renderer's presents so far */
u64 gq_run_ns(void);    /* the renderer thread's time in records so far */
int gq_split_now(void); /* the renderer thread takes the records now */
/* A call of the renderer's side: fn runs with a copy of the bytes the front end puts at the pointer that
   gq_call_begin returns, in the call's place in the queue. The front end fills them, then calls gq_call_end */
typedef void (*GqFn)(const void* arg, u64 bytes);
void* gq_call_begin(GqFn fn, u64 bytes);
void gq_call_end(void);
/* a call with arg's bytes as they are */
void gq_call(GqFn fn, const void* arg, u64 bytes);
/* a call that writes or reads n ranges of guest memory as it runs: the front end's reads there (gq_read_wait) wait
   for it. A range that the call reads counts too, so that a write of the front end there can wait (nvn_replay.c) */
typedef struct {
    u64 gva, size;
} GqRange;
void gq_call_writes(GqFn fn, const void* arg, u64 bytes, const GqRange* ranges, u32 n);
/* a name for fn's records in the stats line of the records' time, and names for the first word of their payload */
void gq_name(GqFn fn, const char* name, const char* const* subs, u32 nsubs);
void gq_seq(u64* all, u64* cb); /* the records run so far, and the constant buffer writes of them */
/* the next record's payload while the taker runs one, when it is a call of fn (another thread may ask): NULL else */
const void* gq_peek_call(GqFn fn, u64* bytes);
/* the payloads of up to max calls of fn that follow the record the taker runs, while it runs that one: their count */
u32 gq_peek_calls(GqFn fn, const void** args, u64* sizes, u32 max);
/* the records after the one the taker runs stay in the ring until gq_keep_end, for a worker that reads them after the
   taker ran them (draw_batches=2) */
void gq_keep(void);
void gq_keep_end(void);
void gq_peek_log(GqFn base); /* the stats line of what gq_peek_call found, functions by their offset from base */

/* the work of each record, and the front end's thread, in gpu.c */
int gpu_on_thread(void);
int gmem_write_now(u64 gva, const void* src, u64 size);
int gmem_write_cb_now(u64 gva, const void* src, u64 size);
/* guest memory read as it is, with no wait and nothing landed */
void gmem_read_quiet(u64 gva, void* dst, u64 size);
void gpu_release_write_now(u64 gva, const void* data, u32 len);
void gpu_release_ticks_now(u64 gva);
void gpu_release_stamp_ticked(u64 gva, const void* data, int top);
void gpu_i2m_block_now(const GpuI2m* t);

#endif
