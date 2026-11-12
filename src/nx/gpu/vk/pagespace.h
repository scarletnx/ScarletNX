/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The page cache: guest memory as the GPU reads it, kept by guest page and not by the range a draw asked for (step 4
   of the rewrite of the buffer cache). buffer.c's buf_get asks it for a range before it copies the range to the ring
   or gives it a buffer of its own. */
#ifndef SNX_VK_PAGESPACE_H
#define SNX_VK_PAGESPACE_H
#include "nx/gpu/vk/vk.h"

/* the device and the setting allow the cache (decided at the first call, with the device made) */
int vk_pages_on(void);
/* GPU range [gva, gva + len) from the cache: 1 with the buffer and the offset to bind, its pages brought up to date
   ahead of this submission's draws, or after the draws recorded so far where one of them read a page that changed
   since (a range longer than 1 MiB: the rendering pass ends). 0 or -1 when the cache cannot serve it, and the caller
   then copies the range itself. 0: the range is longer than the cache serves, is not one run of guest memory or
   crosses a region, its guest address does not suit align, the write tracking cannot answer for it, another owner
   holds bytes of it, or a page it must read is trapped. -1: a shorter range, a page of which a draw of this
   submission already read and has changed since, which is how it goes for a range on pages the game writes between
   its draws. The caller has found no pending GPU result over the range. vk_lock held */
int vk_pages_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
/* GPU range [gva, gva + len) for a draw or a dispatch to write in place: 1 with the buffer and the offset to bind.
   Its whole pages hold guest memory's bytes and are the GPU's from here on (vk_own_publish), so nothing is copied back
   or compared: the CPU's touch of a page lands it. 0 when the cache cannot take it: as vk_pages_get, and also another
   owner or a watch anywhere on its pages, or pages that cannot be trapped. ordered: the front end orders its draws
   by their place in the stream (Maxwell), so a write waits for the reads and writes of its bytes before it. The caller
   has found no pending result of another kind on the range's pages. vk_lock held */
int vk_pages_write(u64 gva, u64 len, u64 align, int ordered, VkBuffer* buf, u64* off);
/* a range that vk_pages_get served is bound again where it was (buffer.c's entry): the read takes its place in the
   order of work in place */
void vk_pages_read(u64 gva, u64 len);
/* a barrier from all work was recorded (vk_buf_ordered): nothing before it is waited for again. And the front end asks
   for a dependency between the work so far and what follows (vk_buf_order_request) */
void vk_pages_ordered(void);
void vk_pages_order_request(void);
/* once a present: the granules nothing read for a while give their memory back */
void vk_pages_frame(void);
/* every page goes (a save state's load): the next reads take the bytes again */
void vk_pages_forget_all(void);
/* statistics (vk_buffer_stats), every 600 presents */
void vk_pages_log(void);
#endif
