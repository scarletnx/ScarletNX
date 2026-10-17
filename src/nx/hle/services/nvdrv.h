/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the nvhost devices (nvdrv.c): nvmap, the GPU address space, syncpoints */
#ifndef SNX_NVDRV_H
#define SNX_NVDRV_H
#include "common/cpu/aarch64/cpu.h"

void nv_init(void);
/* a live handle's address and size, read together: 0 for a handle that isn't alive (host1x, any thread) */
int nvmap_get(u32 h, u64* addr, u32* size);
u64 nvmap_addr_by_id(u32 id);
u64 gmmu_translate(u64 gva);
u32 syncpt_alloc(void);   /* fatal past the 128 ids NVIDIA's stream library has room for */
void syncpt_free(u32 id); /* the id can be handed out again */
/* the ids the syncpoint table covers. A larger one would land on a smaller id's entry, so the functions below leave it
   alone (syncpt_read answers 0, syncpt_wait doesn't wait) and the ioctls refuse it */
#define SYNCPT_IDS 256
u32 syncpt_read(u32 id);
void syncpt_incr(u32 id);
void syncpt_add(u32 id, u32 n); /* n increments at once, with one wake of whoever waits */
/* n increments a channel will make (a GPU submission's, a host1x job's), promised in the order it makes them: the
   value they bring the syncpoint to, which the ioctl hands back as the fence and nvhost-ctrl's max reads */
u32 syncpt_promise(u32 id, u32 n);
int syncpt_wait(u32 id, u32 thresh, i64 timeout_ns);
/* queues fn(arg) on the GPU thread for vi's presents, after the submissions that promised the fences id[0..n) (n at
   most 4): a GPU channel's promised fence is reached by then unless a stream fell short (fn checks it). Any other
   fence is waited for here, 1 s at most, and its id set to ~0 (none) so fn doesn't wait for it again */
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg);

#endif
