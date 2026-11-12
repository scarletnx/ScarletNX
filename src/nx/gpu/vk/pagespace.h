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
   ahead of this submission's draws. 0 or -1 when the cache cannot serve it, and the caller then copies the range
   itself. 0: the range is longer than the cache serves, is not one run of guest memory or crosses a region, its guest
   address does not suit align, the write tracking cannot answer for it, or one of its pages is trapped for the GPU's
   own bytes. -1: a page that a draw of this submission already read has changed since, which is how it goes for a
   range on pages the game writes between its draws. The caller has found no pending GPU result over the range.
   vk_lock held */
int vk_pages_get(u64 gva, u64 len, u64 align, VkBuffer* buf, u64* off);
/* once a present: the granules nothing read for a while give their memory back */
void vk_pages_frame(void);
/* every page goes (a save state's load): the next reads take the bytes again */
void vk_pages_forget_all(void);
/* statistics (vk_buffer_stats), every 600 presents */
void vk_pages_log(void);
#endif
