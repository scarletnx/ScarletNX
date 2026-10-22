/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vi's display clock, which present.c drives */
#ifndef SNX_VI_H
#define SNX_VI_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/hle/kernel/kernel.h"     /* KEvent */
#include "nx/runtime/platform/plat.h" /* PMutex, PCond */

/* the display's refresh rate: vsync ticks, and the swap interval a game asks for, count in it */
#define VI_REFRESH_HZ 60

void vi_vsync_tick(void); /* one display refresh: signals the vsync event */
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
    u32 interval;              /* the swap interval of the last frame queued */
    u32 gen;                   /* bumped when the layer closes (vi_key) */
    u32 event;                 /* signaled when a buffer comes free (GetNativeHandle) */
    ViSlot slot[VI_SLOTS];
} ViLayer;
typedef struct {
    u32 vsync;
    u64 next_stray, frees;
    ViLayer layer[VI_LAYERS];
} HleVi;
/* vi's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lock;
    PCond freed; /* a slot came free */
} HostVi;
#endif
