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

void vi_vsync_tick(void); /* one display refresh: signals the vsync event */
struct SvcObj;
/* the Switch 2 display session that IGraphicsSupervisor 305 opens */
struct SvcObj* vi2_session_new(void);
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
#define VI2_SESSIONS           4
#define VI2_DISPLAYS           4 /* default and the three disconnected stream-play outputs */
/* GetNotificationLimit's answer (eden-ns2's), the last notification index. nn::vi2's user library keeps (last + 32) / 32
   words of notification bits and gives out every bit in them: to OpenDisplay and to each client's two */
#define VI2_LAST_NOTIFICATION  0
#define VI2_NOTIFICATION_WORDS ((VI2_LAST_NOTIFICATION + 32) / 32)
#define VI2_NOTIFICATIONS      (VI2_NOTIFICATION_WORDS * 32)
#define VI2_CLIENTS            (VI2_NOTIFICATIONS / 2) /* each client holds two notification bits */
#define VI2_GROUPS             8                       /* request groups */
#define VI2_MONITOR_KINDS      6                       /* the display changes 261 to 272 watch */
#define VI2_BUFFER_SLOTS       15 /* a client's buffer slots, as eden-ns2's swap chain has them */
/* a client's slot on its way to the display: the image presented, the extent shown, and the present's number, which
   the GPU thread's callback checks the slot still holds (0 for a slot not queued) */
typedef struct {
    u32 present, image, extent;
} Vi2Queued;
/* a client of the display session (RegisterClientState) */
typedef struct {
    u64 token;                     /* the SDK's object address, an identity only; 0 for a free entry */
    u32 retire_bit, available_bit; /* its notification bits: it retired, a buffer came free */
    u64 free_slots;                /* the buffer slots it may fill, a bit for each */
    u8 bound;                      /* 1 + the bound display index, or 0 when unbound */
    u8 encoded;                    /* SetClientOutputEncoding chose the display's SDR encoding */
    u8 shown;                      /* 1 + the slot on the display, which comes free when another replaces it */
    Vi2Queued queued[VI2_BUFFER_SLOTS];
} Vi2Client;
/* a Switch 2 display session (IGraphicsSupervisor 305), under HostVi.lock */
typedef struct {
    u32 obj;
    u32 event;                                 /* retained KO_EVENT id, made by GetNotificationEvent */
    u32 notifications[VI2_NOTIFICATION_WORDS]; /* the pending bits, which GetNotifications takes */
    u32 display_refs[VI2_DISPLAYS];         /* OpenDisplay less CloseDisplay, independently for each display */
    u32 display_notification[VI2_DISPLAYS]; /* the bit set when that display's last reference closes */
    Vi2Client clients[VI2_CLIENTS];
    u32 client_slots;             /* the notification bits the clients hold */
    u32 group_events[VI2_GROUPS]; /* retained KO_EVENT ids, made by CreateRequestGroupEvent */
    u32 group_limit;              /* ConfigureRequestGroups' value, below VI2_GROUPS */
    u8 groups_configured;
    u8 monitor_kind[VI2_GROUPS];     /* what a group's event reports (the command's pair, 0 the vsync) */
    u64 monitor_display[VI2_GROUPS]; /* the display a group's event watches, 0 for none */
    u64 endpoint;                    /* the token of the client that holds the default display's endpoint */
} Vi2Session;
typedef struct {
    u32 vsync;
    u64 next_stray, frees;
    ViLayer layer[VI_LAYERS];
    Vi2Session vi2[VI2_SESSIONS];
    u32 vi2_next;
    u32 vi2_live;     /* the sessions vi_vsync_tick visits, under HostVi.lock; IPC's pool allocation is not */
    u32 vi2_presents; /* the last present's number, never 0 */
} HleVi;
/* whether a loaded save state's display sessions hold what the handlers let in: vsync membership for the sessions in
   use, and notification indices and client bits in range, which the session indexes and shifts by. The event ids
   are state.c's to check */
int vi2_state_valid(const HleVi* v);
/* vi's host side: locks and condition variables, which a save state never holds (hle.h's g_host) */
typedef struct {
    PMutex lock;
    PCond freed; /* a slot came free */
} HostVi;
extern const StateFnTable g_vi_state_fns; /* the functions a save state may hold of vi.c */

#endif
