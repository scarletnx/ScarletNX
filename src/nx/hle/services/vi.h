/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vi's display clock, which present.c drives */
#ifndef SNX_VI_H
#define SNX_VI_H
#include "nx/runtime/state_fns.h"
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/kernel/kernel.h"     /* KEvent */
#include "nx/runtime/platform/plat.h" /* PMutex, PCond */

/* the display's refresh rate: vsync ticks, and the swap interval a game asks for, count in it */
#define VI_REFRESH_HZ 60
/* The rate the console's display runs at now, which a game's swap interval counts in: VI_REFRESH_HZ, or 120 once an
   Ounce game turned 120 Hz on where the TV takes it (gsv.c). The vsync ticks stay at VI_REFRESH_HZ. Any thread */
extern int g_vi_refresh_hz;
static inline u32 vi_refresh_hz(void) { return (u32)__atomic_load_n(&g_vi_refresh_hz, __ATOMIC_RELAXED); }

/* a swap interval of 0 or less: a multiple of this many vsyncs a second (vi_vsync_period), from 0 down to the least */
#define VI_UNLOCKED_HZ  120
#define VI_INTERVAL_MIN (-3)
#define VI_NO_LIMIT_VSYNC_HZ 1000 /* the vsyncs a second with fps_limit 0 (no limit), not a swap interval's */
u64 vi_vsync_period(void); /* ns from one vsync to the next, as the shown layer's swap interval asks */
void vi_vsync_tick(void);  /* one display refresh: signals the vsync event */
/* ---- HLE state in g_hle (hle.h) */
#define VI_LAYERS 8
#define VI_SLOTS  64    /* BufferQueueDefs::NUM_BUFFER_SLOTS */
#define VI_GBUF   0x16c /* the flattened NvGraphicBuffer */

/* a buffer queue's slot: state is vi.c's SLOT_* */
typedef struct {
    int state, has_buffer;
    u64 freed; /* when it was last freed, so the oldest free buffer is handed out first */
    u8 gbuf[VI_GBUF];
    i32 crop[4];
    u32 transform;
} ViSlot;
typedef struct {
    u64 layer_id; /* 0: unused */
    int connected;
    u32 width, height, format; /* the default size and format, from the preallocated buffers */
    u32 max_buffer_count;      /* what the game asked SetBufferCount for; 2 by default, as its window does */
    i32 interval;              /* the swap interval of the last frame queued (vi_vsync_period) */
    u32 gen;                   /* bumped when the layer closes (vi_key) */
    u32 event;                 /* signaled when a buffer comes free (GetNativeHandle) */
    ViSlot slot[VI_SLOTS];
} ViLayer;
typedef struct {
    u32 vsync;
    u32 mode_changed; /* signaled when the display's mode changes: docked's Auto */
    u32 hotplug;      /* signaled with it, as the display was put in its dock or taken out */
    int tv_mode;      /* the mode the two events last told: g_settings.tv_mode */
    u64 next_stray, frees;
    ViLayer layer[VI_LAYERS];
} HleVi;
/* vi's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lock;
    PCond freed; /* a slot came free */
} HostVi;
extern const StateFnTable g_vi_state_fns; /* the functions a save state may hold of vi.c */

#endif
