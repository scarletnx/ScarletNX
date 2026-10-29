/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn_direct's queue, syncs and window (gpu/nvn/nvn_queue.c). The functions are the NVN functions'
   (gpu/nvn/nvn.hook.h); here is the file's state */
#ifndef SNX_NVN_QUEUE_H
#define SNX_NVN_QUEUE_H
#include "common/cpu/aarch64/cpu.h"

/* the file's state, g_nvn.queue (gpu/nvn/nvn_state.h): the submissions made; the window's frame period on the pacer's
   clock (atomic), which bounds a wait for the display; whether the display is stuck (atomic: it didn't show a present
   in two periods, a window covered, so syncs don't wait for it until it shows one again); the native window a window
   builder named last (atomic: SDK 5's 0x2c bytes of builder have no room for it, and a game has one, vi's layer) */
typedef struct {
    u64 submits, period;
    int stuck;
    u64 native_window;
} NvnQueueState;

#endif
