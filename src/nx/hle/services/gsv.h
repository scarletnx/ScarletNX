/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The Ounce's graphics supervisor's state (gsv.c), in g_hle so that a save state holds it: the native images and
   memories a game made, the syncpoint cache it bound and its armed waits, and the display session (nn::vi2) with its
   clients, displays and request groups. Its host side is the lock of what the GPU's thread reaches too */
#ifndef SNX_GSV_H
#define SNX_GSV_H
#include "common/cpu/aarch64/cpu.h"
#include "nx/runtime/platform/plat.h" /* PMutex */

#define GSV_HANDLES    4096 /* the native images, native memories and GPU memories a game makes, each */
#define GSV_SYNC_WAITS 63
#define VI2_CLIENTS    16
#define VI2_DISPLAYS   4 /* the console's and GameShare's three (gsv.c's vi2_display_names) */
#define VI2_GROUPS     8

/* a native image, as CreateNativeImage made it: the memory that backs it and its preference words (the layout
   CalculateNativeImageSize reads: [2] width|height<<16, [4] block height log2, [5] pitch in 64-byte GOBs, [6] format) */
typedef struct {
    u32 memory_handle;
    u32 preference[17];
} GsvNativeImage;
typedef struct {
    u64 start, end;
} GsvGpuMemory;
/* an armed syncpoint wait, by slot: the syncpoint and threshold, and the event to signal once it reaches */
typedef struct {
    u32 id, thresh, event;
    int armed;
} GsvSyncWait;
typedef struct {
    u64 token;
    u32 slots;     /* the two slot bits (retire + available notifications) */
    u64 available; /* the free-slot mask */
    u32 retire_notification;
    u32 available_notification;
    u64 display; /* the bound display name, 0 for none */
} Vi2Client;
typedef struct {
    u32 refs;     /* OpenDisplay references */
    u32 endpoint; /* 0, or its endpoint's token */
} Vi2Display;

/* ---- HLE state in g_hle (hle.h): all zero is a game that asked for nothing yet */
typedef struct {
    /* the handles of each kind made so far: the next one is that count plus 1, and 0 is none */
    u32 native_images_made, native_memories_made, gpu_memories_made;
    GsvNativeImage native_images[GSV_HANDLES];
    u64 native_memory_size[GSV_HANDLES];
    u64 native_memory_cpu[GSV_HANDLES]; /* the guest CPU address BindNativeMemory binds the memory to, 0 for none */
    GsvGpuMemory gpu_memory[GSV_HANDLES];
    /* the syncpoint cache: the game's request page (RW) and the value page (RO) it polls, bound by command 204 */
    u64 sync_rw, sync_ro;
    u32 sync_request_event, sync_completed_event;
    GsvSyncWait sync_waits[GSV_SYNC_WAITS]; /* under g_host.gsv.lock */
    /* the display session */
    u32 vi2_event;                     /* the notification event's kobj id */
    u32 vi2_notifications[32];         /* the notification bitmask, under g_host.gsv.lock */
    u32 vi2_group_events[VI2_GROUPS];  /* the request group events' kobj ids */
    u32 vi2_group_vsync[VI2_GROUPS];   /* 1 while a group is registered for the display's vsync (kind 0) */
    u32 vi2_group_refresh[VI2_GROUPS]; /* 1 while a group is registered for the display's refresh rate (kind 1) */
    u32 vi2_group_hdr[VI2_GROUPS];     /* 1 while a group is registered for the display's HDR status (kind 2) */
    u32 vi2_hdr_told; /* what vi2_hdr said when the HDR status groups were last signaled, plus 1: 0 never */
    Vi2Client vi2_clients[VI2_CLIENTS];
    u32 vi2_client_slots;
    Vi2Display vi2_displays[VI2_DISPLAYS];
    u64 vi2_rates_on; /* the rates the game turned on, 60 Hz always among them once it set any */
} HleGsv;
/* ---- the host side in g_host: the lock of the armed waits and of the notifications, which a guest's thread arms and
   reads while the GPU's thread and the video's signal them */
typedef struct {
    PMutex lock;
} HostGsv;
#endif
