/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvn's queue, syncs and window (gpu/nvn/nvn_queue.c). The functions are the NVN functions'
   (gpu/nvn/nvn.hook.h); here is the file's state */
#ifndef SNX_NVN_QUEUE_H
#define SNX_NVN_QUEUE_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/tracy.h"

/* the file's state, g_nvn.queue (gpu/nvn/nvn_state.h): the submissions made; the window's frame period on the pacer's
   clock (atomic), which bounds a wait for the display; whether the display is stuck (atomic: it didn't show a present
   in two periods, a window covered, so syncs don't wait for it until it shows one again); the native window a window
   builder named last (atomic: SDK 5's 0x2c bytes of builder have no room for it, and a game has one, vi's layer); and
   how far the pacer's clock is ahead of plat_ns (nvn_now), 0 until a save state's load sets it */
typedef struct {
    _Atomic u64 submits, period;
    _Atomic int stuck;
    _Atomic u64 native_window;
    _Atomic i64 clock;
} NvnQueueState;

/* The pacer's clock, which the syncs' ready times, the queues' and the windows' times in the game's memory count on:
   plat_ns, from where a loaded save state's left it. A state from a process that had run longer than this one has
   (plat_ns counts from the process's start) holds times this one's plat_ns hasn't reached yet, a wait for each */
u64 nvn_now(void);

/* ---- the last NVN queue operation of the calling thread (nvn_queue.c) ----
   The runtime keeps short text for the last queue operation: present, wait, fence, finish, acquire or submit. The
   replay zone and the present and wait messages add this text. The setter builds the text only while a profiler
   client is attached. The text is shorter than 128 bytes. Use it at once: it belongs to this thread and the next
   call can change it. nvn_replay.c uses both functions for its replay zone. */
#define NVN_QUEUE_CONTEXT_SIZE 128
void nvn_queue_context_set(const char* fmt, ...);
/* the arguments too are read only with a client, so a caller's lookup for the text costs nothing without one */
#define nvn_queue_context_set(...) (snx_tracy_attached() ? nvn_queue_context_set(__VA_ARGS__) : (void)0)
const char* nvn_queue_context(void);

#endif
