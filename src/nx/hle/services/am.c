/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/am/lifecycle_manager.cpp, applet_manager.cpp, am_types.h, am_results.h,
   service/common_state_getter.cpp, service/lock_accessor.cpp, service/application_functions.cpp,
   service/storage_accessor.cpp, frontend/applet_controller.h, frontend/applet_profile_select.h,
   frontend/applet_software_keyboard.cpp, frontend/applet_software_keyboard.h,
   frontend/applet_software_keyboard_types.h):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project (GPL-2.0-or-later)
   SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project (GPL-2.0-or-later) */
/* appletOE and appletAE: the applet manager as the one application sees it. It is in focus from the start, with the
   focus message queued. Library applets answer what the game asked with the console's one user, the connected
   players, or the player's text in the software keyboard's box over the game: at once, but for the software keyboard,
   whose game waits for the box, and the inline one, which lives until the game finalizes it and answers its requests
   through the interactive channels */
#include <ctype.h>
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "nx/hle/services/hid.h"
#include "nx/hle/services/nvdrv.h"
#include "nx/gpu/gpu.h"
#include "nx/hle/filesystem/fs.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/unattended.h"

#include "hle/services/am.swipc.inc"

#define AM_NO_DATA         RC(128, 2)   /* ResultNoDataInChannel */
#define AM_NO_MESSAGES     RC(128, 3)   /* ResultNoMessages */
#define AM_INVALID_OFFSET  RC(128, 503) /* ResultInvalidOffset */
#define AM_INVALID_STORAGE RC(128, 511) /* ResultInvalidStorageType */
#define AM_OUT_OF_MEMORY   RC(128, 600)
#define AM_OUT_OF_HANDLES  0xD201 /* the kernel's OutOfHandles, which the console answers for a session it can't give */

/* A command of the Switch 2 display session that nothing above declares yet (am.swipc lists the ones its SDK sends,
   with the words each asks for): the default stub answers UnknownCommandId unless the setting says otherwise, and a
   title that gets one for its window's commands never acquires a buffer to present. Nothing is shown here, so the
   empty success the fallback gives is what these answer until each one is known */
static int vi2_fallback(SvcObj* self, IpcCtx* c) { return quiet_ok(self, c); }
/* the graphics system commands a Send 2 SDK sends that nothing above declares yet: their words, so the ones a title
   needs a real answer for can be told apart from the ones a zeroed reply serves. A title got as far as the opening
   movie and then made a request on handle 0, which the zeroed reply for one of them is what leaves it with */
static int gsv_unknown(SvcObj* self, IpcCtx* c) {
    if (c->cmd >= 900) {
        u32 n = c->raw_size / 4, i, at = 0;
        char buf[512];
        buf[0] = 0;
        for (i = 0; i < n && i < 16; i++)
            at += snprintf(buf + at, sizeof buf - (size_t)at, "%08x ", snx_ld_le32(c->raw + i * 4));
        LOG("gsv: cmd %u raw %u: %s (A %d B %d X %d W %d copy %d move %d)\n", c->cmd, n, buf, c->na, c->nb, c->nx, c->nw,
            c->ncopy, c->nout_move);
    }
    return quiet_ok(self, c);
}

/* nn::am::AppletMessage */
#define AM_MSG_FOCUS_STATE_CHANGED      15
#define AM_MSG_PERFORMANCE_MODE_CHANGED 31
#define AM_MSG_STARTUP_LOGO_GONE        95 /* StartupLogoDisappeared, after EnableStartupLogoDisappearedMessage */
#define AM_FOCUS_IN                1 /* nn::am::service::FocusState::InFocus */

/* the AppletIds of the library applets that answer with data */
#define AM_APPLET_CONTROLLER     0x0c
#define AM_APPLET_PROFILE_SELECT 0x10
#define AM_APPLET_SWKBD          0x11
#define AM_APPLET_MII_EDIT       0x12
#define AM_APPLET_WEB            0x13
#define AM_APPLET_SHOP           0x14
#define AM_APPLET_OFFLINE_WEB    0x17
#define AM_APPLET_LOGIN_SHARE    0x18
#define AM_APPLET_WEB_AUTH       0x19

#define AM_PRESELECTED_USER_MAGIC 0xc79497cau /* LaunchParameterAccountPreselectedUser */

/* the uid user 1 has when users.ini gives it none (users.c). Fixed: the device id below is made from it, and the
   saves an older version left belong to it (fs.c) */
const u64 g_user_id[2] = {SNX_USERS_FIRST_UID0, SNX_USERS_FIRST_UID1};

/* the application's message queue, the user channel and the rest of am's state, and the pools of its objects' data:
   g_hle.am (services_int.h) */

void am_init(void) {
    pm_init(&g_host.am.lock);
    pc_init(&g_host.am.kb_cv);
    g_hle.am.msg_event = owned_event("am:message");
    g_hle.am.sleep_lock_event = owned_event("am:sleep lock");
    g_hle.am.launchable_event = owned_event("am:library applet launchable");
    g_hle.am.resolution_event = owned_event("am:display resolution change");
    g_hle.am.next_layer = 1;
    /* the application starts in focus, and is told so, then that the performance mode is set (the SDK reads it on
       that message) */
    g_hle.am.msgs[0] = AM_MSG_FOCUS_STATE_CHANGED;
    g_hle.am.msgs[1] = AM_MSG_PERFORMANCE_MODE_CHANGED;
    g_hle.am.count = 2;
    ev_signal(g_hle.am.msg_event);
    ev_signal(g_hle.am.launchable_event);
}

/* The TV mode changed while the game runs (docked's Auto): the display resolution change event is signaled, and the
   applet is told the performance mode changed, as a console that was put in its dock or taken out of it */
void am_display_mode_changed(void) {
    pm_lock(&g_host.am.lock);
    if (g_hle.am.count < AM_MESSAGES) {
        g_hle.am.msgs[(g_hle.am.head + g_hle.am.count++) % AM_MESSAGES] = AM_MSG_PERFORMANCE_MODE_CHANGED;
        ev_signal(g_hle.am.msg_event);
    }
    pm_unlock(&g_host.am.lock);
    ev_signal(g_hle.am.resolution_event);
}

/* ============================================================ storages */
/* IStorage's data is a StorageData, the accessors' an AmAccessor (services_int.h) */
static void am_storage_destroy(SvcObj* o) {
    StorageData* s = (StorageData*)o->data;
    if (s->tmem) kobj_unref(kobj_at(s->tmem));
    free(s->d);
}
static void am_accessor_destroy(SvcObj* o) { svcobj_unref(((AmAccessor*)o->data)->storage); }

/* a storage's numbers are little-endian, as the game reads them; a uid is its two halves */
static void am_put32(u8* p, u32 v) { snx_st_le32(p, v); }
static void am_put_uid(u8* p, const u64 uid[2]) {
    snx_st_le64(p, uid[0]);
    snx_st_le64(p + 8, uid[1]);
}
static u8* am_storage_bytes(SvcObj* o) {
    StorageData* s = (StorageData*)o->data;
    return s->guest ? GP(s->guest) : s->d;
}
/* a storage of size bytes, a copy of data's or zeros; NULL if they can't be had */
static SvcObj* am_storage_new(u64 size, const void* data) {
    SvcObj* o;
    StorageData* s;
    u8* d;
    if (size > 0x10000000u) return NULL;
    /* the bytes the game sized, zeroed: at least one, so a storage over a transfer memory (size 0 here) has some */
    if (!(d = calloc(1, size ? (size_t)size : 1)))
        fatal("Out of memory for an am storage of %llu bytes", (unsigned long long)size);
    o = nn_am_service_IStorage_new_data(g_hle.am.storages, g_hle.am.storage_next, "am storages");
    s = (StorageData*)o->data;
    s->size = size;
    s->d = d;
    if (data && size) memcpy(d, data, (size_t)size);
    o->destroy = am_storage_destroy;
    return o;
}
/* a storage over a transfer memory's pages: the client's size, within the memory */
static int am_storage_tmem(i64 size, u32 handle, int handle_only, SvcObj** storage) {
    KTmem* t = (KTmem*)h_get_ref(handle, KO_TMEM);
    StorageData* s;
    R_UNLESS(t, RC(1, 114)); /* the kernel's InvalidHandle */
    if (size < 0 || (u64)size > t->size) {
        kobj_unref(&t->o);
        R_THROW(AM_INVALID_OFFSET);
    }
    *storage = am_storage_new(0, NULL);
    s = (StorageData*)(*storage)->data;
    s->size = (u64)size;
    s->guest = t->addr;
    s->tmem = kobj_id(&t->o);
    s->handle_only = handle_only;
    R_SUCCEED();
}

static int nn_am_service_IStorage_Open(SvcObj* self, SvcObj** accessor) {
    R_UNLESS(!((StorageData*)self->data)->handle_only, AM_INVALID_STORAGE);
    *accessor = nn_am_service_IStorageAccessor_new_data(g_hle.am.accessors, g_hle.am.accessor_next, "am storage accessors");
    svcobj_ref(self);
    ((AmAccessor*)(*accessor)->data)->storage = self;
    (*accessor)->destroy = am_accessor_destroy;
    R_SUCCEED();
}
static int nn_am_service_IStorage_OpenTransferStorage(SvcObj* self, SvcObj** accessor) {
    R_UNLESS(((StorageData*)self->data)->guest, AM_INVALID_STORAGE);
    *accessor = nn_am_service_ITransferStorageAccessor_new_data(g_hle.am.accessors, g_hle.am.accessor_next,
                                                                  "am storage accessors");
    svcobj_ref(self);
    ((AmAccessor*)(*accessor)->data)->storage = self;
    (*accessor)->destroy = am_accessor_destroy;
    R_SUCCEED();
}

static int nn_am_service_IStorageAccessor_GetSize(SvcObj* self, i64* size) {
    *size = (i64)((StorageData*)((AmAccessor*)self->data)->storage->data)->size;
    R_SUCCEED();
}
static int nn_am_service_IStorageAccessor_Write(SvcObj* self, i64 offset, IpcBuf data) {
    SvcObj* st = ((AmAccessor*)self->data)->storage;
    u64 size = ((StorageData*)st->data)->size;
    R_UNLESS(offset >= 0 && (u64)offset <= size && data.size <= size - (u64)offset, AM_INVALID_OFFSET);
    memcpy(am_storage_bytes(st) + offset, GP(data.addr), (size_t)data.size);
    R_SUCCEED();
}
/* the whole buffer, as Eden checks a read: one that runs past the end is refused, not cut */
static int nn_am_service_IStorageAccessor_Read(SvcObj* self, i64 offset, IpcBuf data) {
    SvcObj* st = ((AmAccessor*)self->data)->storage;
    u64 size = ((StorageData*)st->data)->size;
    R_UNLESS(offset >= 0 && (u64)offset <= size && data.size <= size - (u64)offset, AM_INVALID_OFFSET);
    memcpy(GP(data.addr), am_storage_bytes(st) + offset, (size_t)data.size);
    R_SUCCEED();
}

/* ============================================================ proxies */
static SvcObj* am_proxy(void) { return nn_am_service_IApplicationProxy_new(); }
static int nn_am_service_IApplicationProxyService_OpenApplicationProxy(SvcObj* self, u64 pid, u32 process,
                                                                      SvcObj** proxy) {
    (void)self, (void)pid, (void)process;
    *proxy = am_proxy();
    R_SUCCEED();
}
/* 10, which a Switch 2 SDK's InitializeApplicationInterfaces calls instead of 0 (Eden answers it with 0's handler) */
static int nn_am_service_IApplicationProxyService_OpenApplicationProxySwitch2(SvcObj* self, u64 pid, u32 process,
                                                                              SvcObj** proxy) {
    return nn_am_service_IApplicationProxyService_OpenApplicationProxy(self, pid, process, proxy);
}
static int nn_am_service_IAllSystemAppletProxiesService_OpenSystemApplicationProxy(SvcObj* self, u64 pid,
                                                                                  u32 process, SvcObj** proxy) {
    (void)self, (void)pid, (void)process;
    *proxy = am_proxy();
    R_SUCCEED();
}
SvcObj* new_appletoe(void) { return nn_am_service_IApplicationProxyService_new(); }
SvcObj* new_appletae(void) { return nn_am_service_IAllSystemAppletProxiesService_new(); }

static int nn_am_service_IApplicationProxy_GetCommonStateGetter(SvcObj* self, SvcObj** getter) {
    (void)self;
    *getter = nn_am_service_ICommonStateGetter_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetSelfController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_ISelfController_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetWindowController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_IWindowController_new();
    R_SUCCEED();
}
/* 2000: the graphics system's session a Switch 2 title's gsv boot sends TIPC commands on (am.swipc): a copy of a
   session to it, as Eden's Switch 2 ReturnRawSession pushes one */
static int nn_am_service_ISelfController_GetGraphicsSystemSession(SvcObj* self, u32* session) {
    (void)self;
    *session = session_new(nn_am_service_IGraphicsSystemSession_new());
    return *session ? 0 : AM_OUT_OF_HANDLES;
}
/* gsv 1402: the NVIDIA driver's session, which a Switch 2 title's nvsrv boot speaks nvdrv's protocol on. Eden's
   Switch 2 work serves it with nvdrv's own devices, and so does this: the same interface nvdrv's service gives */
static int nn_am_service_IGraphicsSystemSession_OpenNvsrvSession(SvcObj* self, u32* session) {
    (void)self;
    *session = session_new(nvdrv_session());
    return *session ? 0 : AM_OUT_OF_HANDLES;
}
/* gsv 497: the GPU session a Switch 2 title's nn::gpu boots with, which it then asks for an id range and an event per
   id on (IGpuSession, below) */
static int nn_am_service_IGraphicsSystemSession_OpenGpuSession(SvcObj* self, u64 size, u32* session) {
    SvcObj* o;
    (void)self, (void)size;
    o = nn_am_service_IGpuSession_new_data(g_hle.am.gpu_ids, g_hle.am.gpu_id_next, "nx2 GPU sessions");
    if (!o) return AM_OUT_OF_MEMORY;
    *session = session_new(o);
    return *session ? 0 : AM_OUT_OF_HANDLES;
}
/* gsv 201 and 301: the GPU address spaces a Switch 2 title's SDK asks for before it reserves any, where a Switch 1
   SDK reads the alias and heap regions of GetInfo. The answer is the region a title's NVN reserves in, as Eden's
   Switch 2 work names it for the newer SDK that asks its GPU session for the same */
#define GMMU_USER_FIRST 0x8000000ull
#define GMMU_USER_LAST  0xC000000000ull
static int nn_am_service_IGraphicsSystemSession_GetUserPartition(SvcObj* self, u64* first, u64* last) {
    (void)self;
    *first = GMMU_USER_FIRST;
    *last = GMMU_USER_LAST;
    R_SUCCEED();
}
static int nn_am_service_IGraphicsSystemSession_GetGsvPartition(SvcObj* self, u64* first, u64* last) {
    (void)self;
    *first = 0; /* the server's own partition is empty here, as Eden's Switch 2 work has it */
    *last = 0;
    R_SUCCEED();
}
/* gsv 305: the display session a Switch 2 title's nn::vi2::sdk boots with (IVi2Session, below), which it then asks
   what the display has before it sizes its window, and presents its client buffer on */
static int nn_am_service_IGraphicsSystemSession_OpenViSession(SvcObj* self, u32* session) {
    SvcObj* o;
    (void)self;
    o = nn_am_service_IVi2Session_new_data(g_hle.am.vi2s, g_hle.am.vi2_next, "nx2 vi2 sessions");
    if (!o) return AM_OUT_OF_MEMORY;
    *session = session_new(o);
    return *session ? 0 : AM_OUT_OF_HANDLES;
}

/* The Switch 2 GPU session's ids: an id is signaled once it starts, which here is at once, since the GPU work its
   SDK waits for isn't emulated (Eden's Switch 2 work signals it on Start as well). Its data is services_int.h's
   AmGpuIds, one per gpu session the application opens */
/* an id's event, made on first use (the session keeps a reference of its own, so a game closing its handle doesn't free
   one a later Start signals) */
static u32 am_gpu_event(AmGpuIds* s, u32 id) {
    KEvent* e;
    if (id >= AM_GPU_IDS) return 0;
    if (!s->ev[id]) {
        if (!(e = kevent_new("nx2 gpu id"))) return 0;
        kobj_ref(&e->o);
        s->ev[id] = kobj_id(&e->o);
    }
    return s->ev[id];
}
static AmGpuIds* am_gpu_ids(SvcObj* self) {
    return self && self->data ? (AmGpuIds*)self->data : NULL;
}
static int nn_am_service_IGpuSession_GetIdRange(SvcObj* self, u32* first, u32* last) {
    (void)self;
    *first = 0;
    *last = AM_GPU_IDS - 1;
    R_SUCCEED();
}
static int nn_am_service_IGpuSession_GetIdEvent(SvcObj* self, u32 id, u32* event) {
    AmGpuIds* s = am_gpu_ids(self);
    u32 ev;
    if (!s) return AM_OUT_OF_MEMORY;
    ev = am_gpu_event(s, id);
    if (!ev) return AM_OUT_OF_MEMORY;
    *event = ev_handle(ev);
    return *event ? 0 : AM_OUT_OF_HANDLES;
}
static int nn_am_service_IGpuSession_StartId(SvcObj* self, u32 id) {
    AmGpuIds* s = am_gpu_ids(self);
    u32 ev;
    if (!s) return AM_OUT_OF_MEMORY;
    ev = am_gpu_event(s, id);
    if (ev) ev_signal(ev);
    R_SUCCEED();
}
/* 22: a second session to this same object, which the sub-session commands (a memory's registration, a job) speak
   on. It is the object itself, as the reference serves it */
static int nn_am_service_IGpuSession_OpenSubSession(SvcObj* self, u32* session) {
    if (!am_gpu_ids(self)) return AM_OUT_OF_MEMORY;
    svcobj_ref(self);
    *session = session_new(self);
    svcobj_unref(self);
    return *session ? 0 : AM_OUT_OF_HANDLES;
}
/* 33: a count this SDK's NVN path asks of the GPU session before it uses it (Eden's Switch 2 work answers 1) */
static int nn_am_service_IGpuSession_GetCount33(SvcObj* self, u32* count) {
    (void)self;
    *count = 1;
    R_SUCCEED();
}
/* 201 and 202: the GPU address spaces a Switch 2 title's SDK asks of the GPU session for, where a Switch 1 SDK reads
   the alias and heap regions of GetInfo: the range its NVN reserves in, and the server's own, which is empty here.
   Both name the region Eden's Switch 2 work names for the same asks */
static int nn_am_service_IGpuSession_GetUserPartition(SvcObj* self, u64* first, u64* last) {
    (void)self;
    *first = GMMU_USER_FIRST;
    *last = GMMU_USER_LAST;
    R_SUCCEED();
}
static int nn_am_service_IGpuSession_GetGsvPartition(SvcObj* self, u64* first, u64* last) {
    (void)self;
    *first = 0; /* the server's own partition is empty here, as Eden's Switch 2 work has it */
    *last = 0;
    R_SUCCEED();
}
/* 203: the GPU VA range of the in-process memory manager that serves AllocSpace, which the title's NVN reserves fixed
   addresses in, and the kind of range it is. It is the range NvRm answers AllocAsEx with (the 8 GiB a title's NVN
   reserves in, from 0x8000000 up to 512 GiB), as Eden's Switch 2 work reports it */
static int nn_am_service_IGpuSession_GetVaRange(SvcObj* self, u64* kind, u64* first, u64* size) {
    (void)self;
    *kind = 1;
    *first = GMMU_USER_FIRST;
    *size = GMMU_USER_LAST - GMMU_USER_FIRST;
    R_SUCCEED();
}
/* The sub-session's commands: the GPU-accessible memory a title's SDK registers, which its jobs then map into the
   GPU's view of the process. The SDK keeps the answer's handle and names it in every later job, so each registration
   takes the next one (Eden's Switch 2 work counts the same way) */
static int nn_am_service_IGpuSession_RegisterMemory(SvcObj* self, u64 first, u64 last, u32* handle) {
    AmGpuIds* s = am_gpu_ids(self);
    u32 h;
    if (!s) return AM_OUT_OF_MEMORY;
    if (last < first) return AM_INVALID_OFFSET;
    h = s->mem_next + 1;
    s->mem_next = h;
    if (h - 1 < AM_GPU_MEM) s->mem_first[h - 1] = first;
    (void)handle;
    *handle = h;
    /* the title's NVN never maps its memory through nvhost-as-gpu: the GPU address it uses is the memory's own, so
       every registered range is mapped there (Eden's Switch 2 work identity-maps them all the same way) */
    gmmu_map(first, first, last - first + 1);
    gpu_semaphore_region_add(first, last - first + 1);
    R_SUCCEED();
}
/* 211: a job that maps a range of registered memory into the GPU's view of the process, which completes through the
   event of the slot the request names. Switch 2 titles never map through nvhost-as-gpu: the memory they register is
   used at the same address on both sides, so the job maps it where it says (Eden's Switch 2 work maps it the same
   way). Nothing else runs here, so the slot's event is signaled at once */
static int nn_am_service_IGpuSession_SubmitJob(SvcObj* self, u32 kind, u32 first_lo, u32 first_hi, u32 last_lo,
                                               u32 last_hi, u32 memory, u32 offset_lo, u32 offset_hi, u32 unused0,
                                               u32 unused1, u32 unused2, u32 unused3, u32 unused4, u32 unused5,
                                               u32 unused6, u32 slot) {
    AmGpuIds* s = am_gpu_ids(self);
    u64 first = (u64)first_lo | ((u64)first_hi << 32);
    u64 last = (u64)last_lo | ((u64)last_hi << 32);
    u64 offset = (u64)offset_lo | ((u64)offset_hi << 32);
    u32 ev;
    (void)kind;
    (void)unused0;
    (void)unused1;
    (void)unused2;
    (void)unused3;
    (void)unused4;
    (void)unused5;
    (void)unused6;
    if (!s) return AM_OUT_OF_MEMORY;
    if (slot >= AM_GPU_IDS) return AM_INVALID_OFFSET;
    if (memory && memory - 1 < AM_GPU_MEM && last >= first && s->mem_first[memory - 1])
        gmmu_map(first, s->mem_first[memory - 1] + offset, last - first + 1);
    ev = am_gpu_event(s, slot);
    if (!ev) return AM_OUT_OF_MEMORY;
    ev_signal(ev);
    R_SUCCEED();
}

/* ============================================ the Switch 2 display session (am.swipc) */
/* gsv 305's session (Eden's INx2Vi2Session), which a Switch 2 SDK's nn::vi2::sdk asks what the display has before it
   sizes its window, and presents its client buffer on. Nothing is shown here, so the counts are the console's, and the
   displays' frames follow the main loop's 60 Hz as the console's do */
/* the events of this display session (services_int.h's AmVi2, one per vi2 session the application opens): one per
   event the SDK asks for. Index 0 is the session's own event, which nothing waits on, so it is signaled at once. The
   rest are displays' vsync, which the main loop's 60 Hz signals (am_vi2_vsync_tick, below): a title clears the one it
   waited on before it waits again, so a signal only at the first frame leaves it waiting from the second one on */
static u32 am_vi2_event(AmVi2* s, u32 index) {
    KEvent* e;
    if (index >= AM_VI2_EVENTS) return 0;
    if (!s->ev[index]) {
        if (!(e = kevent_new("nx2 vi2 event"))) return 0;
        kobj_ref(&e->o);
        s->ev[index] = kobj_id(&e->o);
        if (!index) ev_signal(s->ev[index]); /* the session's own: nothing here waits on it */
    }
    return s->ev[index];
}
static AmVi2* am_vi2(SvcObj* self) { return self && self->data ? (AmVi2*)self->data : NULL; }
static int nn_am_service_IVi2Session_Unknown1(SvcObj* self, u32* one) {
    (void)self;
    *one = 1;
    R_SUCCEED();
}
static int nn_am_service_IVi2Session_GetDisplayCount(SvcObj* self, u32* count) {
    (void)self;
    *count = 1; /* the one display a title presents on */
    R_SUCCEED();
}
static int nn_am_service_IVi2Session_GetTimestampRecorderCount(SvcObj* self, u32* count) {
    (void)self;
    *count = AM_VI2_EVENTS - 1; /* must stay under 8: nn::vi::detail::LayerHistory's layers */
    R_SUCCEED();
}
static int nn_am_service_IVi2Session_GetEvent(SvcObj* self, u32* event) {
    AmVi2* s = am_vi2(self);
    u32 ev;
    if (!s) return AM_OUT_OF_MEMORY;
    ev = am_vi2_event(s, 0);
    if (!ev) return AM_OUT_OF_MEMORY;
    *event = ev_handle(ev);
    return *event ? 0 : AM_OUT_OF_HANDLES;
}
/* 31 and 32: the vsync events, which the console signals on every frame. One event per display's index, so a title
   waiting on two fires of it gets one after the other (the main loop's 60 Hz signals them: am_vi2_vsync_tick) */
static int nn_am_service_IVi2Session_GetVsyncEvent(SvcObj* self, u32 kind, u32* event) {
    AmVi2* s = am_vi2(self);
    u32 ev;
    (void)kind;
    if (!s) return AM_OUT_OF_MEMORY;
    ev = am_vi2_event(s, 1 + (kind & 3));
    if (!ev) return AM_OUT_OF_MEMORY;
    *event = ev_handle(ev);
    return *event ? 0 : AM_OUT_OF_HANDLES;
}
static int nn_am_service_IVi2Session_GetDisplayVsyncEvent(SvcObj* self, u32 vsync, u32 display, u32* event) {
    AmVi2* s = am_vi2(self);
    u32 ev;
    (void)vsync; /* the event it duplicates is this session's own: the same one is handed back */
    if (!s) return AM_OUT_OF_MEMORY;
    ev = am_vi2_event(s, 5 + (display & 3));
    if (!ev) return AM_OUT_OF_MEMORY;
    *event = ev_handle(ev);
    return *event ? 0 : AM_OUT_OF_HANDLES;
}
/* the main loop's 60 Hz: a display's frame is done, so every event of every display session is signaled again. A title
   clears the one it waited on before it waits again (the console's are auto-clear), so a signal only on the first
   frame leaves it waiting from the second one on (Eden's Switch 2 work signals its displays' events from its vblank
   timer) */
void am_vi2_vsync_tick(void) {
    u32 i, j;
    for (i = 0; i < AM_VI2_SESSIONS; i++) {
        AmVi2* s = &g_hle.am.vi2s[i];
        for (j = 0; j < AM_VI2_EVENTS; j++)
            if (s->ev[j]) ev_signal(s->ev[j]);
    }
}
static int nn_am_service_IVi2Session_Unknown104(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
static int nn_am_service_IVi2Session_Unknown105(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
/* the present: the frame's buffer slots and the fence its work reaches, answered with the mask of the slots the client
   may acquire next. Nothing is shown here, so every slot is handed back at once, which is what keeps a title from
   waiting on a buffer this machine never holds: the console frees a buffer once its frame's fence has passed, and the
   60 Hz the main loop signals the vsync events with is what paces a title instead (Eden's Switch 2 work reports its
   slots free the same way and hold each present to the next vblank)

   The fence in the request is not waited for: holding a present until its frame reaches the value this SDK asks for
   blocked a title's presentation thread for seconds at a time, since its driver signals its work with semaphore
   releases that reach it much later than the fence does */
static int nn_am_service_IVi2Session_PresentClientBuffer(SvcObj* self, const u8* present, u32* mask_lo,
                                                         u32* mask_hi, u32* zero0, u32* zero1, u32* zero2,
                                                         u32* zero3) {
    (void)self, (void)present;
    *mask_lo = 0x7fffu; /* every slot, as the console hands each back once its frame's fence has passed */
    *mask_hi = 0;
    *zero0 = *zero1 = *zero2 = *zero3 = 0;
    R_SUCCEED();
}

/* ============================================ the Switch 2 syncpoint session (am.swipc) */
/* The shared page AttachSyncpointMemory (gsv 204) laid out: one per application, as the console has one graphics
   system, which gsv 202's syncpoint sessions read. Its offsets are the ones Eden's Switch 2 work writes, and the
   client requires ((count + 0x40) >> 6) - 1 < 0x40 */
#define AM_SYNCPT_RO_VALUES  0x40   /* u64 values, read-only */
#define AM_SYNCPT_RO_SUMMARY 0x1040 /* the summary word, read-only */
#define AM_SYNCPT_RO_BITS    0x1100 /* the armed bits, read-only */
#define AM_SYNCPT_RW_SUMMARY 0x0    /* the summary word, read-write */
#define AM_SYNCPT_RW_BITS    0x100  /* the armed bits, read-write */
#define AM_SYNCPT_RW_BITS_END 0x300 /* where the page's own data ends, past the armed bits it maps */
#define AM_SYNCPT_POLL_NS    500000 /* the poller's period, as the reference's 500 us is */
/* most syncpoints the read-only page's values hold, and the count a client asking for more is given */
#define AM_SYNCPT_MAX ((0x1000 - AM_SYNCPT_RO_VALUES) / 8)

static PMutex g_am_syncpt_lk = PMUTEX_INIT;
static int g_am_syncpt_poller;

/* the syncpoint a counter names: the SDK sends the bitwise NOT of the syncpoint id, as the reference reads it. An id
   nothing allocated here reads 0 and never counts as reached */
static u32 am_syncpt_id(u32 counter) { return ~counter < SYNCPT_IDS ? ~counter : 0; }
/* a session's own data, or NULL when it has none */
static AmSyncpt* am_syncpt(SvcObj* self) {
    return self && self->data ? (AmSyncpt*)self->data : NULL;
}
/* A slot's event, made on first use (the session keeps a reference of its own, so a game closing its handle doesn't
   free one a later syncpoint increment signals). 0 without memory, or for a slot past the table */
static u32 am_syncpt_event(AmSyncpt* s, u32 slot) {
    KEvent* e;
    if (slot >= AM_SYNCPT_SLOTS) return 0;
    if (!s->ev[slot]) {
        if (!(e = kevent_new("nx2 syncpt slot"))) return 0;
        kobj_ref(&e->o);
        s->ev[slot] = kobj_id(&e->o);
    }
    return s->ev[slot];
}
/* Answers the armed slots their syncpoints reached, as the reference's guest action does: the console's graphics
   stack waits on a slot's event, so a wait is answered as the counter moves rather than by a poller of its own.
   nvdrv's syncpt_watch calls this after every increment, from the incrementing thread (a GPU, video or IPC one),
   which is where the console answers it too. The armed bits are answered here as well, since the client reads them
   as its wait's done */
static void am_syncpt_poll(void);
static int am_syncpt_mirror(void);
static void am_syncpt_armed(void) {
    AmSyncpt* s;
    u32 i, slot, id, thresh;
    int acked = 0;
    pm_lock(&g_am_syncpt_lk);
    for (i = 0; i < AM_SYNCPT_SESSIONS; ++i) {
        s = &g_hle.am.syncpts[i];
        for (slot = 0; slot < AM_SYNCPT_SLOTS; ++slot) {
            if (!s->armed[slot]) continue;
            id = am_syncpt_id(s->armed_id[slot]);
            thresh = (u32)(s->armed_thresh[slot] ? s->armed_thresh[slot] : 1);
            if (!id || (i32)(syncpt_read(id) - thresh) >= 0) {
                s->armed[slot] = 0;
                ev_signal(s->ev[slot]);
            }
        }
    }
    /* the values the client polls are mirrored before its armed bits are answered, so a wait it takes back sees the
       counter that ended it */
    if (g_hle.am.syncpt_page.attached) acked = am_syncpt_mirror();
    pm_unlock(&g_am_syncpt_lk);
    if (acked) ev_signal(g_hle.am.syncpt_page.notify_ev);
}
/* Mirrors the syncpoint values and the client's summary and armed bits into the read-only page, which is what it
   polls: its waits are answered by signaling the notify event when the bits it moved reach the page, as Eden's
   Switch 2 work's poller does. The values are the host's own, which the GPU, video and host1x threads increment, and
   the reference keeps a pasted loop for the same reason (its 500 us timing event). 1 when the bits changed */
static int am_syncpt_mirror(void) {
    u8* ro;
    u8* rw;
    u64* values;
    u32 i, count, words;
    int acked = 0;
    if (!(ro = g_hle.am.syncpt_page.attached ? g_host.am.syncpt.ro : NULL)) return 0;
    rw = g_host.am.syncpt.rw;
    values = (u64*)(ro + AM_SYNCPT_RO_VALUES);
    count = g_hle.am.syncpt_page.count;
    for (i = 0; i < count && i < SYNCPT_IDS; ++i) values[i] = syncpt_read(i);
    words = (count + 63) / 64;
    for (i = 0; i < words && i < AM_SYNCPT_SLOTS / 64; ++i) {
        u64 rw_bits = *(u64*)(rw + AM_SYNCPT_RW_BITS + 8 * i);
        u64* ro_bits = (u64*)(ro + AM_SYNCPT_RO_BITS + 8 * i);
        if (*ro_bits != rw_bits) {
            *ro_bits = rw_bits;
            acked = 1;
        }
    }
    *(u64*)(ro + AM_SYNCPT_RO_SUMMARY) = *(u64*)(rw + AM_SYNCPT_RW_SUMMARY);
    return acked;
}
/* the page's mirror: one thread for the run, started when the page is attached */
static void am_syncpt_poll(void) {
    int acked;
    if (!g_hle.am.syncpt_page.attached) return;
    pm_lock(&g_am_syncpt_lk);
    acked = am_syncpt_mirror();
    pm_unlock(&g_am_syncpt_lk);
    if (acked) ev_signal(g_hle.am.syncpt_page.notify_ev);
}
static void am_syncpt_run(void* arg) {
    (void)arg;
    for (;;) {
        am_syncpt_poll();
        plat_sleep_ns(AM_SYNCPT_POLL_NS);
    }
}
static void am_syncpt_start(void) {
    if (!__atomic_exchange_n(&g_am_syncpt_poller, 1, __ATOMIC_ACQ_REL)) {
        if (plat_thread(am_syncpt_run, NULL, 0x4000)) /* no thread: the page is only exact at a reply */
            __atomic_store_n(&g_am_syncpt_poller, 0, __ATOMIC_RELEASE);
    }
    syncpt_watch(am_syncpt_armed); /* a wait is answered where the counter moves */
}

/* The Switch 2 native memory and images a title creates from the graphics system, then binds to pages: the console
   answers an id and a size for one, which is what a game reads back and what a later command binds. A game creates
   few of them, so the id it is given indexes these tables directly (0: the slot is free) */
static PMutex g_am_native_lk = PMUTEX_INIT;

/* The block-linear size and alignment of a surface of width x height, as the console lays a window's out: 4 bytes
   per pixel, in the 128-row blocks its Gob class uses (Eden's Switch 2 work computes a window's requirements the
   same way, with the same pitch and row rounding). The display's own size is what a title sizes its window from,
   which is what vi answers for it */
static void am_surface_layout(u32 width, u32 height, u64* size, u64* align) {
    u64 pitch = ((u64)width * 4 + 63) & ~63ull, rows = ((u64)height + 127) & ~127ull;
    *size = pitch * rows;
    *align = 0x2000; /* the console's alignment of a block-linear surface */
}
/* 1501: the requirements of the surface a title presents, and 1502 those of the pool its other surfaces come from.
   Both are the display's size, since that is the surface a title presents on it */
static int nn_am_service_IGraphicsSystemSession_CalculateRequirements(SvcObj* self, u64* size, u64* alignment) {
    (void)self;
    am_surface_layout(1920, 1080, size, alignment);
    R_SUCCEED();
}
static int nn_am_service_IGraphicsSystemSession_CalculateImagePoolRequirements(SvcObj* self, u64* size,
                                                                             u64* alignment) {
    (void)self;
    am_surface_layout(1920, 1080, size, alignment);
    R_SUCCEED();
}
/* 1510: the block-linear size and alignment of the surface an InterModuleUserImagePreference describes, which
   nn::gsv::shim::CalculateRequirements reads the size of a title's window from. The preference holds the surface's
   dimensions, the log2 of its block height (rows of its Gob class) and its pitch in 64-byte GOBs; a preference outside
   those bounds falls back to 4 bytes a pixel in 128-row blocks, as Eden's Switch 2 work does */
static void am_block_linear(u32 dims, u32 block_height, u32 pitch_gobs, u64* size, u64* align) {
    u64 width = (dims & 0xFFFF) + 1, height = (dims >> 16) + 1, pitch, rows;
    u32 bh = block_height;
    if (bh > 8 || pitch_gobs < (width * 4 + 63) / 64 || pitch_gobs > 4096) { /* not a usable block-linear preference */
        pitch = (width * 4 + 63) & ~(u64)63;
        bh = 7; /* 128 rows */
    } else {
        pitch = (u64)pitch_gobs * 64;
    }
    rows = (height + (((u64)1 << bh) - 1)) & ~(((u64)1 << bh) - 1);
    *size = pitch * rows;
    *align = ((u64)1 << bh) * 64 > 0x1000 ? ((u64)1 << bh) * 64 : 0x1000;
}
static int nn_am_service_IGraphicsSystemSession_CalculateShimRequirements(SvcObj* self, u32 kind, u32 reserved,
                                                                         u32 dims, u32 fmt, u32 block_height,
                                                                         u32 pitch_gobs, u32 w6, u32 w7, u32 w8,
                                                                         u32 w9, u32 w10, u32 w11, u32 w12, u32 w13,
                                                                         u32 w14, u32 w15, u32 count, u64* size,
                                                                         u64* alignment) {
    (void)self;
    (void)reserved;
    (void)fmt;
    (void)w6;
    (void)w7;
    (void)w8;
    (void)w9;
    (void)w10;
    (void)w11;
    (void)w12;
    (void)w13;
    (void)w14;
    (void)w15;
    (void)count;
    if (kind != 2) { /* only a block-linear preference has a block height and a pitch to lay it out with */
        am_surface_layout(1920, 1080, size, alignment);
        R_SUCCEED();
    }
    am_block_linear(dims, block_height, pitch_gobs, size, alignment);
    R_SUCCEED();
}
/* 115: the session is done with what it opened (Eden's Switch 2 work answers its 115 with a plain success) */
static int nn_am_service_IGraphicsSystemSession_Finalize(SvcObj* self) {
    (void)self;
    R_SUCCEED();
}
/* 1101: an image over a native memory, from the preference its SDK builds. The layout the preference describes is
   kept, since a later command asks for the image's size and its handle */
static int nn_am_service_IGraphicsSystemSession_CreateNativeImage(SvcObj* self, u32* image) {
    AmNativeImage* n;
    u64 size, align;
    u32 i;
    (void)self;
    am_surface_layout(1920, 1080, &size, &align); /* the image a title presents is the display's own surface */
    pm_lock(&g_am_native_lk);
    for (i = 0; i < AM_NATIVE_IMAGES && g_hle.am.native_images[i].size; ++i) {}
    if (i == AM_NATIVE_IMAGES) {
        pm_unlock(&g_am_native_lk);
        return AM_OUT_OF_MEMORY;
    }
    n = &g_hle.am.native_images[i];
    n->size = size;
    pm_unlock(&g_am_native_lk);
    *image = i + 1;
    R_SUCCEED();
}
static int nn_am_service_IGraphicsSystemSession_DestroyNativeImage(SvcObj* self) {
    u32 i;
    (void)self;
    pm_lock(&g_am_native_lk);
    for (i = 0; i < AM_NATIVE_IMAGES; ++i) g_hle.am.native_images[i].size = 0; /* the one image is gone */
    pm_unlock(&g_am_native_lk);
    R_SUCCEED();
}
/* 401: the native memory itself, of the size a title names. The console answers an id for it; the pages it describes
   are bound to it later, as Eden's Switch 2 CreateNativeMemory does */
static int nn_am_service_IGraphicsSystemSession_CreateNativeMemory(SvcObj* self, u32* memory) {
    u64 size;
    (void)self;
    am_surface_layout(1920, 1080, &size, &size); /* the memory its images lie in: the display's own surface */
    if (!size || size > 0x100000000ull) return AM_INVALID_OFFSET;
    pm_lock(&g_am_native_lk);
    for (memory[0] = 0; *memory < AM_NATIVE_MEMORIES && g_hle.am.native_memories[*memory].size; ++*memory) {}
    if (*memory == AM_NATIVE_MEMORIES) {
        pm_unlock(&g_am_native_lk);
        return AM_OUT_OF_MEMORY;
    }
    g_hle.am.native_memories[*memory].size = size;
    pm_unlock(&g_am_native_lk);
    ++*memory;
    R_SUCCEED();
}
static int nn_am_service_IGraphicsSystemSession_DestroyNativeMemory(SvcObj* self) {
    u32 i;
    (void)self;
    pm_lock(&g_am_native_lk);
    for (i = 0; i < AM_NATIVE_MEMORIES; ++i) g_hle.am.native_memories[i].size = 0;
    pm_unlock(&g_am_native_lk);
    R_SUCCEED();
}
/* gsv 202: the syncpoint session nn::syncpt::sdk::InitializeEx sends TIPC commands on, a session of its own */
static int nn_am_service_IGraphicsSystemSession_OpenSyncpointSession(SvcObj* self, u32* session) {
    SvcObj* o;
    (void)self;
    o = nn_am_service_ISyncpointSession_new_data(g_hle.am.syncpts, g_hle.am.syncpt_next, "nx2 syncpt sessions");
    if (!o) return AM_OUT_OF_MEMORY;
    *session = session_new(o);
    return *session ? 0 : AM_OUT_OF_HANDLES;
}
/* gsv 204: the two transfer memories the client lays its waits over, whose read-only one the server answers with a
   header naming where each field it reads lies, and the two events it waits between */
static int nn_am_service_IGraphicsSystemSession_AttachSyncpointMemory(SvcObj* self, u32 version, u32 count,
                                                                      u32 read_write, u32 read_only, u32* request,
                                                                      u32* notify) {
    KTmem *rwt, *rot;
    u8 *rw, *ro;
    u32 words;
    (void)self;
    rwt = (KTmem*)h_get_ref(read_write, KO_TMEM);
    if (!rwt) return AM_INVALID_STORAGE;
    rot = (KTmem*)h_get_ref(read_only, KO_TMEM);
    if (!rot) {
        kobj_unref(&rwt->o);
        return AM_INVALID_STORAGE;
    }
    /* the read-only page holds the syncpoint values and the armed bits, so the console asks for 0x2000 of it. The
       count it asks for may name more syncpoints than the values page holds: the console caps it there, and so does
       this (Eden's Switch 2 work takes min(asked, (0x1000 - 0x40) / 8)) */
    rw = GP(rwt->addr);
    ro = GP(rot->addr);
    if (rot->size < 0x2000 || rwt->size < AM_SYNCPT_RW_BITS_END || version > 1 || !count) {
        kobj_unref(&rwt->o);
        kobj_unref(&rot->o);
        return AM_INVALID_OFFSET;
    }
    /* the page's two events are obj ids, as the session's slot events are: ev_handle takes one to make the reply's
       handle, and the reference the session keeps holds the object for the page's lifetime */
    if (!g_hle.am.syncpt_page.request_ev) {
        KEvent* e = kevent_new("nx2 syncpt request");
        if (!e) {
            kobj_unref(&rwt->o);
            kobj_unref(&rot->o);
            return AM_OUT_OF_MEMORY;
        }
        kobj_ref(&e->o);
        g_hle.am.syncpt_page.request_ev = kobj_id(&e->o);
    }
    if (!g_hle.am.syncpt_page.notify_ev) {
        KEvent* e = kevent_new("nx2 syncpt notify");
        if (!e) {
            kobj_unref(&rwt->o);
            kobj_unref(&rot->o);
            return AM_OUT_OF_MEMORY;
        }
        kobj_ref(&e->o);
        g_hle.am.syncpt_page.notify_ev = kobj_id(&e->o);
    }
    /* the page's guest addresses are held, not the objects: the game unmaps them when it has no further use for the
       syncpoint session, and the values it last wrote back are what stays there */
    if (count > AM_SYNCPT_MAX) count = AM_SYNCPT_MAX;
    pm_lock(&g_am_syncpt_lk);
    g_hle.am.syncpt_page.attached = 0;
    g_host.am.syncpt.rw = rw;
    g_host.am.syncpt.ro = ro;
    g_hle.am.syncpt_page.count = count;
    g_hle.am.syncpt_page.attached = 1;
    pm_unlock(&g_am_syncpt_lk);
    kobj_unref(&rwt->o);
    kobj_unref(&rot->o);
    snx_st_le32(ro + 0x00, 1);     /* the version it got, as it asked for */
    snx_st_le32(ro + 0x04, 0);     /* the base syncpoint id */
    snx_st_le32(ro + 0x08, count); /* how many syncpoints the page holds */
    snx_st_le32(ro + 0x0c, AM_SYNCPT_RO_VALUES);
    snx_st_le32(ro + 0x10, AM_SYNCPT_RW_SUMMARY);
    snx_st_le32(ro + 0x14, AM_SYNCPT_RO_SUMMARY);
    snx_st_le32(ro + 0x18, AM_SYNCPT_RW_BITS);
    snx_st_le32(ro + 0x1c, AM_SYNCPT_RO_BITS);
    memset(ro + 0x20, 0, AM_SYNCPT_RO_VALUES - 0x20);
    memset(ro + AM_SYNCPT_RO_VALUES, 0, AM_SYNCPT_RO_SUMMARY - AM_SYNCPT_RO_VALUES);
    memset(rw, 0, AM_SYNCPT_RW_BITS_END);
    words = (count + 63) / 64;
    memset(ro + AM_SYNCPT_RO_SUMMARY, 0, 8);
    memset(ro + AM_SYNCPT_RO_BITS, 0, 8 * words);
    *request = ev_wr_handle(g_hle.am.syncpt_page.request_ev);
    *notify = ev_handle(g_hle.am.syncpt_page.notify_ev);
    am_syncpt_start();
    return 0;
}

/* the syncpoint session's TIPC commands. Its asks are a token it must get back, a slot's event, a counter's value,
   and an armed wait; Eden's Switch 2 work answers them the same way */
static int nn_am_service_ISyncpointSession_Handshake(SvcObj* self, u32 request_lo, u32 request_hi, u32* reply_lo,
                                                     u32* reply_hi) {
    (void)self;
    *reply_lo = request_lo; /* the newer SDKs abort unless the token is echoed back */
    *reply_hi = request_hi;
    R_SUCCEED();
}
static int nn_am_service_ISyncpointSession_GetCount(SvcObj* self, u32* count) {
    (void)self;
    *count = AM_SYNCPT_COUNT;
    R_SUCCEED();
}
static int nn_am_service_ISyncpointSession_GetSlotEvent(SvcObj* self, u32 slot, u32* event) {
    AmSyncpt* s = am_syncpt(self);
    u32 ev;
    if (!s) return AM_OUT_OF_MEMORY;
    ev = am_syncpt_event(s, slot);
    if (!ev) return AM_OUT_OF_MEMORY;
    *event = ev_handle(ev);
    return *event ? 0 : AM_OUT_OF_HANDLES;
}
/* 21: the same slot's event, which this SDK asks for by that number instead of 22 (am.swipc) */
static int nn_am_service_ISyncpointSession_GetSlotEvent21(SvcObj* self, u32 slot) {
    (void)self;
    (void)slot; /* this SDK asks under 21 for nothing but the result (am.swipc) */
    R_SUCCEED();
}
static int nn_am_service_ISyncpointSession_QueryCounter(SvcObj* self, u32 counter, u32* value_low, u32* value_high,
                                                        u32* zero) {
    u32 id;
    u64 v;
    (void)self;
    id = am_syncpt_id(counter);
    v = id ? syncpt_read(id) : 0;
    *value_low = (u32)v;
    *value_high = (u32)(v >> 32);
    *zero = 0;
    R_SUCCEED();
}
static int nn_am_service_ISyncpointSession_RequestSlotEvent(SvcObj* self, u32 slot, u32 counter, u32 value_lo,
                                                           u32 value_hi, u32 mode) {
    AmSyncpt* s = am_syncpt(self);
    u64 value = (u64)value_lo | ((u64)value_hi << 32);
    u32 id, ev;
    (void)mode; /* both modes compare the value against the counter's; only its width differs */
    if (!s) return AM_OUT_OF_MEMORY;
    if (slot >= AM_SYNCPT_SLOTS) return AM_INVALID_OFFSET;
    ev = am_syncpt_event(s, slot);
    if (!ev) return AM_OUT_OF_MEMORY;
    ev_clear(ev); /* a slot is reused by different objects: a new wait starts unsignaled */
    id = am_syncpt_id(counter);
    pm_lock(&g_am_syncpt_lk);
    s->armed_id[slot] = counter;
    s->armed_thresh[slot] = (u32)value;
    s->armed[slot] = 1;
    pm_unlock(&g_am_syncpt_lk);
    if (!id) { /* an id nothing allocated: it never moves, so answer at once */
        ev_signal(ev);
        R_SUCCEED();
    }
    if ((i32)(syncpt_read(id) - (u32)value) >= 0) ev_signal(ev); /* it may have got there already */
    am_syncpt_start();
    R_SUCCEED();
}
static int nn_am_service_ISyncpointSession_CancelSlotEvent(SvcObj* self, u32 slot) {
    AmSyncpt* s = am_syncpt(self);
    u32 ev;
    if (!s) return AM_OUT_OF_MEMORY;
    if (slot >= AM_SYNCPT_SLOTS) return AM_INVALID_OFFSET;
    pm_lock(&g_am_syncpt_lk);
    s->armed[slot] = 0;
    pm_unlock(&g_am_syncpt_lk);
    ev = s->ev[slot];
    if (ev) ev_clear(ev);
    R_SUCCEED();
}

/* IAudioController's volumes, kept on each controller as Eden keeps them (AmVolumes, services_int.h) */
static int nn_am_service_IApplicationProxy_GetAudioController(SvcObj* self, SvcObj** controller) {
    AmVolumes* v;
    (void)self;
    *controller = nn_am_service_IAudioController_new_data(g_hle.am.volumes, g_hle.am.volume_next, "am audio controllers");
    v = (AmVolumes*)(*controller)->data;
    v->main_volume = 1.0f;
    v->library_volume = 1.0f;
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetDisplayController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_IDisplayController_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetProcessWindingController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_IProcessWindingController_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetLibraryAppletCreator(SvcObj* self, SvcObj** creator) {
    (void)self;
    *creator = nn_am_service_ILibraryAppletCreator_new();
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetApplicationFunctions(SvcObj* self, SvcObj** functions) {
    (void)self;
    *functions = nn_am_service_IApplicationFunctions_new();
    R_SUCCEED();
}
/* 40, 50, 70, 80 and 90: the controllers a Switch 2 SDK opens from IApplicationProxy, which a Switch 1 one does not.
   Each is an interface of commands none of which this runtime answers yet, so a call on one succeeds (am.swipc's
   INx2Controller) as Eden's Switch 2 work has its stub interface do */
#define AM_NX2_CONTROLLER(n)                                                                     \
    static int nn_am_service_IApplicationProxy_GetNx2Interface##n(SvcObj* self, SvcObj** c) {    \
        (void)self;                                                                              \
        *c = nn_am_service_INx2Controller_new();                                                 \
        R_SUCCEED();                                                                             \
    }
AM_NX2_CONTROLLER(40)
AM_NX2_CONTROLLER(50)
AM_NX2_CONTROLLER(70)
AM_NX2_CONTROLLER(80)
AM_NX2_CONTROLLER(90)
/* 60: the graphics scale a Switch 2 SDK's nn::oe reads at startup, which scales what it renders by. The console's own
   display is never scaled, so the answer is the unscaled one (Eden's Switch 2 work answers 10 as well) */
static int nn_am_service_IApplicationProxy_GetGraphicsScaleController(SvcObj* self, SvcObj** controller) {
    (void)self;
    *controller = nn_am_service_IGraphicsScaleController_new();
    R_SUCCEED();
}
static int nn_am_service_IGraphicsScaleController_GetCurrentGraphicsScale(SvcObj* self, u32* scale) {
    (void)self;
    *scale = 10; /* the unscaled value of nn::oe's scale enum */
    R_SUCCEED();
}
static int nn_am_service_IApplicationProxy_GetDebugFunctions(SvcObj* self, SvcObj** functions) {
    (void)self;
    *functions = nn_am_service_IDebugFunctions_new();
    R_SUCCEED();
}
/* ============================================================ ICommonStateGetter */
static int nn_am_service_ICommonStateGetter_GetEventHandle(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.msg_event);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_ReceiveMessage(SvcObj* self, nn_am_AppletMessage* message) {
    int rc = 0;
    (void)self;
    pm_lock(&g_host.am.lock);
    if (!g_hle.am.count)
        rc = AM_NO_MESSAGES;
    else {
        *message = g_hle.am.msgs[g_hle.am.head];
        g_hle.am.head = (g_hle.am.head + 1) % AM_MESSAGES;
        g_hle.am.count--;
    }
    if (!g_hle.am.count) ev_clear(g_hle.am.msg_event);
    pm_unlock(&g_host.am.lock);
    return rc;
}
static int nn_am_service_ICommonStateGetter_GetOperationMode(SvcObj* self, u8* mode) {
    (void)self;
    *mode = snx_tv_mode() ? 1 : 0; /* nn::oe::OperationMode: Handheld, Console */
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetPerformanceMode(SvcObj* self, u32* mode) {
    (void)self;
    *mode = snx_tv_mode() ? 1 : 0; /* Boost when docked, as apm says */
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetCurrentFocusState(SvcObj* self, u8* state) {
    (void)self;
    *state = AM_FOCUS_IN;
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_RequestToAcquireSleepLock(SvcObj* self) {
    (void)self;
    ev_signal(g_hle.am.sleep_lock_event);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetAcquiredSleepLockEvent(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.sleep_lock_event);
    R_SUCCEED();
}
static int am_set_vr_mode(u8 on) {
    pm_lock(&g_host.am.lock);
    g_hle.am.vr_mode = on;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}

static int nn_am_service_ICommonStateGetter_SetVrModeEnabled(SvcObj* self, u8 enabled) {
    (void)self;
    return am_set_vr_mode(enabled != 0);
}
static int nn_am_service_ICommonStateGetter_BeginVrModeEx(SvcObj* self) {
    (void)self;
    return am_set_vr_mode(1);
}
static int nn_am_service_ICommonStateGetter_EndVrModeEx(SvcObj* self) {
    (void)self;
    return am_set_vr_mode(0);
}

static int nn_am_service_ICommonStateGetter_GetDefaultDisplayResolution(SvcObj* self, i32* width, i32* height) {
    (void)self;
    *width = snx_tv_mode() ? 1920 : 1280;
    *height = snx_tv_mode() ? 1080 : 720;
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetDefaultDisplayResolutionChangeEvent(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.resolution_event);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetHdcpAuthenticationStateChangeEvent(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.resolution_event);
    R_SUCCEED();
}
/* HDCP authenticates on the console's TV output; Eden answers 1 (common_state_getter.cpp) */
static int nn_am_service_ICommonStateGetter_GetHdcpAuthenticationState(SvcObj* self, i32* state) {
    (void)self;
    *state = 1;
    R_SUCCEED();
}
/* apm's configuration of the mode the console runs in */
static int nn_am_service_ICommonStateGetter_GetCurrentPerformanceConfiguration(SvcObj* self, u32* configuration) {
    (void)self;
    *configuration = apm_configuration(snx_tv_mode() ? 1 : 0);
    R_SUCCEED();
}
/* the clocks a game asks for around its loading screens, which apm keeps as Boost mode's configuration */
static int nn_am_service_ICommonStateGetter_SetCpuBoostMode(SvcObj* self, u32 mode) {
    (void)self;
    apm_cpu_boost(mode);
    R_SUCCEED();
}
/* there is no start-up logo here, so it is gone as soon as a game asks to be told */
static int nn_am_service_ICommonStateGetter_EnableStartupLogoDisappearedMessage(SvcObj* self) {
    (void)self;
    pm_lock(&g_host.am.lock);
    if (g_hle.am.count < AM_MESSAGES) g_hle.am.msgs[(g_hle.am.head + g_hle.am.count++) % AM_MESSAGES] = AM_MSG_STARTUP_LOGO_GONE;
    ev_signal(g_hle.am.msg_event);
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetSettingsPlatformRegion(SvcObj* self, u32* region) {
    (void)self;
    *region = 1; /* Global */
    R_SUCCEED();
}
static void am_set_vr_mode_3d(u8 on) {
    pm_lock(&g_host.am.lock);
    g_hle.am.vr_mode_3d = on;
    pm_unlock(&g_host.am.lock);
}
static int nn_am_service_ICommonStateGetter_BeginVrMode3d(SvcObj* self) {
    (void)self;
    am_set_vr_mode_3d(1);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_EndVrMode3d(SvcObj* self) {
    (void)self;
    am_set_vr_mode_3d(0);
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_IsVrModeEnabled3d(SvcObj* self, u8* enabled) {
    (void)self;
    pm_lock(&g_host.am.lock);
    *enabled = g_hle.am.vr_mode_3d;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}

/* ILockAccessor: granted whenever it is free, with an event signaled when it is released (AmLock, services_int.h) */
static void am_lock_destroy(SvcObj* o) { kobj_unref(kobj_at(((AmLock*)o->data)->event)); }
static SvcObj* am_lock_new(void) {
    SvcObj* o = nn_am_service_ILockAccessor_new_data(g_hle.am.lock_objs, g_hle.am.lock_obj_next, "am lock accessors");
    ((AmLock*)o->data)->event = owned_event("am:lock accessor");
    /* the lock is free: libnx's TryLock and Lock wait on the event before they ask for it */
    ev_signal(((AmLock*)o->data)->event);
    o->destroy = am_lock_destroy;
    return o;
}
static int nn_am_service_ICommonStateGetter_GetHomeButtonReaderLockAccessor(SvcObj* self, SvcObj** accessor) {
    (void)self;
    *accessor = am_lock_new();
    R_SUCCEED();
}

static int nn_am_service_ICommonStateGetter_GetReaderLockAccessorEx(SvcObj* self, u32 button_type,
                                                                   SvcObj** accessor) {
    (void)self, (void)button_type;
    *accessor = am_lock_new();
    R_SUCCEED();
}
static int nn_am_service_ICommonStateGetter_GetWriterLockAccessorEx(SvcObj* self, u32 button_type,
                                                                   SvcObj** accessor) {
    (void)self, (void)button_type;
    *accessor = am_lock_new();
    R_SUCCEED();
}
static int nn_am_service_ILockAccessor_TryLock(SvcObj* self, u8 return_handle, u8* is_locked, u32* event) {
    AmLock* l = (AmLock*)self->data;
    pm_lock(&g_host.am.lock);
    *is_locked = (u8)!l->locked;
    l->locked = 1;
    pm_unlock(&g_host.am.lock);
    if (return_handle) *event = ev_handle(l->event);
    R_SUCCEED();
}
static int nn_am_service_ILockAccessor_Unlock(SvcObj* self) {
    AmLock* l = (AmLock*)self->data;
    pm_lock(&g_host.am.lock);
    l->locked = 0;
    pm_unlock(&g_host.am.lock);
    ev_signal(l->event);
    R_SUCCEED();
}
static int nn_am_service_ILockAccessor_GetEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((AmLock*)self->data)->event);
    R_SUCCEED();
}
static int nn_am_service_ILockAccessor_IsLocked(SvcObj* self, u8* is_locked) {
    AmLock* l = (AmLock*)self->data;
    pm_lock(&g_host.am.lock);
    *is_locked = (u8)l->locked;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}

static int nn_am_service_ICommonStateGetter_IsVrModeEnabled(SvcObj* self, u8* enabled) {
    (void)self;
    pm_lock(&g_host.am.lock);
    *enabled = g_hle.am.vr_mode;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}

/* ============================================================ ISelfController */
static int nn_am_service_ISelfController_Exit(SvcObj* self) {
    (void)self;
    /* the frontend's loop ends the run, saving what outlives it, as when the window closes */
    LOG("am: the application exited\n");
    g_quit = 1;
    R_SUCCEED();
}
static int nn_am_service_ISelfController_GetLibraryAppletLaunchableEvent(SvcObj* self, u32* event) {
    (void)self;
    *event = ev_handle(g_hle.am.launchable_event);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_CreateManagedDisplayLayer(SvcObj* self, u64* layer_id) {
    (void)self;
    pm_lock(&g_host.am.lock);
    *layer_id = g_hle.am.next_layer++;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_CreateManagedDisplaySeparableLayer(SvcObj* self, u64* layer_id,
                                                                           u64* recording_layer_id) {
    (void)self;
    pm_lock(&g_host.am.lock);
    *layer_id = g_hle.am.next_layer++;
    *recording_layer_id = g_hle.am.next_layer++;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_GetAccumulatedSuspendedTickChangedEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:suspended tick changed", 1);
}
static int nn_am_service_ISelfController_SetIdleTimeDetectionExtension(SvcObj* self, u32 extension) {
    (void)self;
    pm_lock(&g_host.am.lock);
    g_hle.am.idle_extension = extension;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_GetIdleTimeDetectionExtension(SvcObj* self, u32* extension) {
    (void)self;
    pm_lock(&g_host.am.lock);
    *extension = g_hle.am.idle_extension;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_SetAutoSleepDisabled(SvcObj* self, u8 disabled) {
    (void)self;
    pm_lock(&g_host.am.lock);
    g_hle.am.auto_sleep_disabled = disabled;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ISelfController_IsAutoSleepDisabled(SvcObj* self, u8* disabled) {
    (void)self;
    pm_lock(&g_host.am.lock);
    *disabled = g_hle.am.auto_sleep_disabled;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}

static int nn_am_service_IWindowController_GetAppletResourceUserId(SvcObj* self,
                                                                   nn_applet_AppletResourceUserId* aruid) {
    (void)self;
    *aruid = K_PROCESS_ID;
    R_SUCCEED();
}

static float am_volume(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }
static int nn_am_service_IAudioController_SetExpectedMasterVolume(SvcObj* self, float main_volume,
                                                                  float library_volume) {
    AmVolumes* v = (AmVolumes*)self->data;
    pm_lock(&g_host.am.lock);
    v->main_volume = am_volume(main_volume);
    v->library_volume = am_volume(library_volume);
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_IAudioController_GetMainAppletExpectedMasterVolume(SvcObj* self, float* volume) {
    pm_lock(&g_host.am.lock);
    *volume = ((AmVolumes*)self->data)->main_volume;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_IAudioController_GetLibraryAppletExpectedMasterVolume(SvcObj* self, float* volume) {
    pm_lock(&g_host.am.lock);
    *volume = ((AmVolumes*)self->data)->library_volume;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
/* the fade is not played: nothing here plays the game at its expected volume */
static int nn_am_service_IAudioController_ChangeMainAppletMasterVolume(SvcObj* self, float volume, i64 fade_time_ns) {
    (void)fade_time_ns;
    pm_lock(&g_host.am.lock);
    ((AmVolumes*)self->data)->main_volume = am_volume(volume);
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}

/* ============================================================ IApplicationFunctions */
static int nn_am_service_IApplicationFunctions_PopLaunchParameter(SvcObj* self, u32 kind, SvcObj** storage) {
    u8* d;
    u64 uid[2];
    u32 v;
    (void)self;
    if (kind == 1) { /* UserChannel: the last storage unpopped, which the channel's reference goes with */
        pm_lock(&g_host.am.lock);
        *storage = g_hle.am.user_count ? g_hle.am.user_channel[--g_hle.am.user_count] : NULL;
        pm_unlock(&g_host.am.lock);
        R_UNLESS(*storage, AM_NO_DATA);
        R_SUCCEED();
    }
    R_UNLESS(kind == 2, AM_NO_DATA); /* PreselectedUser (2) is the only other kind with data */
    pm_lock(&g_host.am.lock);
    v = (u32)g_hle.am.preselected_popped;
    g_hle.am.preselected_popped = 1;
    pm_unlock(&g_host.am.lock);
    R_UNLESS(!v, AM_NO_DATA);
    /* AccountPreselectedUser: the user the game plays as, selected */
    acc_playing(uid, NULL, 0);
    *storage = am_storage_new(0x88, NULL);
    d = am_storage_bytes(*storage);
    am_put32(d, AM_PRESELECTED_USER_MAGIC);
    am_put32(d + 4, 1);
    am_put_uid(d + 8, uid);
    R_SUCCEED();
}

static int nn_am_service_IApplicationFunctions_ClearUserChannel(SvcObj* self) {
    SvcObj* drop[AM_USER_CHANNEL];
    u32 i, n;
    (void)self;
    pm_lock(&g_host.am.lock);
    n = g_hle.am.user_count;
    memcpy(drop, g_hle.am.user_channel, n * sizeof drop[0]);
    g_hle.am.user_count = 0;
    pm_unlock(&g_host.am.lock);
    for (i = 0; i < n; i++) svcobj_unref(drop[i]);
    R_SUCCEED();
}
/* a copy, as Eden keeps one: the game may write its storage again after */
static int nn_am_service_IApplicationFunctions_UnpopToUserChannel(SvcObj* self, SvcObj* storage) {
    SvcObj* copy;
    (void)self;
    if (!storage) R_SUCCEED();
    copy = am_storage_new(((StorageData*)storage->data)->size, am_storage_bytes(storage));
    if (!copy) R_THROW(AM_OUT_OF_MEMORY);
    pm_lock(&g_host.am.lock);
    if (g_hle.am.user_count < AM_USER_CHANNEL) {
        g_hle.am.user_channel[g_hle.am.user_count++] = copy;
        copy = NULL;
    }
    pm_unlock(&g_host.am.lock);
    if (!copy) R_SUCCEED();
    svcobj_unref(copy);
    WARN_LIMIT(4, "am: the user channel holds %d storages, no more\n", AM_USER_CHANNEL);
    R_THROW(AM_OUT_OF_MEMORY);
}

/* nn::oe::ExecuteProgram (am's command 120) switches to another program of the same application: on the console the
   process manager launches the program the game names, and the previous one waits for it. Eden follows it by loading
   the program at the index it is given and restarting its emulation (core.cpp's Load with program_index). ScarletNX
   builds one program per game folder, so the command is a stub, GetPreviousProgramIndex answers -1, and a launcher
   that starts its sub-games through it stays on the loading screen it shows meanwhile. */
/* -1: the game was launched by the system, not by another of its programs */
static int nn_am_service_IApplicationFunctions_GetPreviousProgramIndex(SvcObj* self, i32* index) {
    (void)self;
    *index = -1;
    R_SUCCEED();
}

/* the game's control.nacp (nn::ns::ApplicationControlProperty), as the recompiler staged it beside the game's data:
   n bytes at off, NULL past its end or without one */
static const u8* am_nacp(u64 off, u64 n) {
    pm_lock(&g_host.am.lock);
    if (!g_hle.am.nacp_read) {
        g_hle.am.nacp = fs_read_file("control.nacp", &g_hle.am.nacp_size);
        g_hle.am.nacp_read = 1;
    }
    pm_unlock(&g_host.am.lock);
    return g_hle.am.nacp && off + n <= g_hle.am.nacp_size ? g_hle.am.nacp + off : NULL;
}
u64 am_aoc_base_id(void) {
    const u8* nacp = am_nacp(0, 0x3078);
    return snx_nacp_aoc_base(nacp, nacp ? 0x3078 : 0, g_hle.kernel.title_id);
}
static u64 am_nacp_u64(u64 off) {
    const u8* p = am_nacp(off, 8);
    u64 v = 0;
    if (p) v = snx_ld_le64(p);
    return v;
}
#define NACP_SUPPORTED_LANGUAGES 0x302c
#define NACP_DISPLAY_VERSION     0x3060
#define NACP_ACCOUNT_SAVE_SIZE   0x3080 /* and its journal's, then the device save's and its journal's */

/* nn::ns::ApplicationLanguage of each nn::settings::Language, and back (-1: no settings language has it) */
static const signed char am_app_language[18] = {2, 0, 3, 4, 7, 6, 14, 12, 8, 10, 11, 13, 1, 9, 5, 14, 13, 15};
static const signed char am_settings_language[18] = {1, 12, 0, 2, 3, 14, 5, 4, 8, 13, 9, 10, 7, 16, 15, 17, -1, -1};
/* each ApplicationLanguage's fallbacks, in the order ns tries them */
static const signed char am_priority[16][18] = {
    {0, 1, 5, 9, 3, 4, 6, 7, 8, 10, 11, 2, 14, 13, 12, 16, 17, -1},
    {1, 0, 3, 4, 6, 7, 8, 10, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {2, 0, 1, 5, 9, 3, 4, 6, 7, 8, 10, 11, 14, 13, 12, 16, 17, -1},
    {3, 9, 1, 0, 4, 6, 7, 8, 10, 11, 5, 2, 14, 13, 12, 16, 17, -1},
    {4, 1, 0, 3, 6, 7, 8, 10, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {5, 6, 0, 1, 10, 9, 3, 7, 4, 8, 11, 2, 14, 13, 12, 16, 17, -1},
    {6, 5, 1, 0, 3, 4, 7, 8, 10, 11, 9, 2, 14, 13, 12, 16, 17, -1},
    {7, 1, 0, 3, 4, 6, 8, 10, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {8, 1, 0, 3, 4, 6, 7, 10, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {9, 3, 0, 1, 5, 6, 4, 7, 8, 10, 11, 2, 14, 13, 12, 16, 17, -1},
    {10, 1, 0, 3, 4, 6, 7, 8, 11, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {11, 1, 0, 3, 4, 6, 7, 8, 10, 5, 9, 2, 14, 13, 12, 16, 17, -1},
    {12, 0, 1, 5, 9, 3, 4, 6, 7, 8, 10, 11, 2, 14, 13, 16, 17, -1},
    {13, 14, 0, 1, 2, 5, 9, 3, 4, 6, 7, 8, 10, 11, 12, 16, 17, -1},
    {14, 13, 0, 1, 2, 5, 9, 3, 4, 6, 7, 8, 10, 11, 12, 16, 17, -1},
    {15, 10, 5, 0, 1, 2, 3, 4, 6, 7, 8, 9, 11, 12, 14, 13, 16, 17}};
/* the settings language a game with these SupportedLanguageFlag bits is shown in: the system's when the game has it,
   else the first of its fallbacks the game has (all of them when the flags are 0), as ns picks it */
static int am_desired_language(int lang, u32 supported) {
    const signed char* list;
    int i, app;
    if (lang < 0 || lang >= 18) lang = 1;
    list = am_priority[(int)am_app_language[lang]];
    for (i = 0; i < 18 && list[i] >= 0; i++) {
        app = list[i];
        if (am_settings_language[app] < 0) continue; /* Polish and Thai: no language code here */
        if (!supported || ((supported >> app) & 1)) return i ? am_settings_language[app] : lang;
    }
    return lang;
}
static int nn_am_service_IApplicationFunctions_GetDesiredLanguage(SvcObj* self, nn_settings_LanguageCode* code) {
    const u8* p = am_nacp(NACP_SUPPORTED_LANGUAGES, 4);
    u32 supported = 0;
    (void)self;
    if (p) supported = snx_ld_le32(p);
    set_language_code(am_desired_language(g_settings.language, supported), code->v);
    R_SUCCEED();
}
/* the NACP's user_account_switch_lock: whether the game locks switching users (RawNACP's byte at 0x3026) */
int am_user_account_switch_lock(void) {
    const u8* p = am_nacp(0x3026, 1);
    return p ? p[0] != 0 : 0;
}
/* the NACP's display version, "1.0.0" for a game without one */
void am_display_version(char version[16]) {
    const u8* p = am_nacp(NACP_DISPLAY_VERSION, 16);
    if (p)
        memcpy(version, p, 16);
    else
        memcpy(version, "1.0.0", 6);
    version[15] = 0;
}
static int nn_am_service_IApplicationFunctions_GetDisplayVersion(SvcObj* self, nn_oe_DisplayVersion* version) {
    (void)self;
    am_display_version((char*)version->v);
    R_SUCCEED();
}
/* the account's (type 1) and the device's (3) save data: the size ExtendSaveData gave it, else the NACP's, 64 MiB each
   without a NACP */
#define AM_DEFAULT_SAVE_SIZE 0x4000000
static int nn_am_service_IApplicationFunctions_GetSaveDataSize(SvcObj* self, u8 type, const nn_account_Uid* uid,
                                                              i64* normal_size, i64* journal_size) {
    int device = type == 3, extended = 0;
    (void)self, (void)uid;
    if (type == 1 || type == 3) {
        pm_lock(&g_host.am.lock);
        extended = g_hle.am.save_extended[device];
        if (extended) {
            *normal_size = g_hle.am.save_size[device][0];
            *journal_size = g_hle.am.save_size[device][1];
        }
        pm_unlock(&g_host.am.lock);
    }
    if (extended) R_SUCCEED();
    if (!am_nacp(0, 0)) {
        *normal_size = AM_DEFAULT_SAVE_SIZE;
        *journal_size = AM_DEFAULT_SAVE_SIZE;
    } else if (type == 1 || type == 3) {
        *normal_size = (i64)am_nacp_u64(NACP_ACCOUNT_SAVE_SIZE + (device ? 16 : 0));
        *journal_size = (i64)am_nacp_u64(NACP_ACCOUNT_SAVE_SIZE + 8 + (device ? 16 : 0));
    }
    R_SUCCEED();
}
/* the largest save data the console allows, as Eden answers (application_functions.cpp) */
static int nn_am_service_IApplicationFunctions_GetSaveDataSizeMax(SvcObj* self, i64* normal_size,
                                                                  i64* journal_size) {
    (void)self;
    *normal_size = 0xFFFFFFF;
    *journal_size = 0xFFFFFFF;
    R_SUCCEED();
}
/* a cache storage is made on the console's NAND; this runtime keeps no cache storage, so nothing is required, as
   Eden's stub answers (application_functions.cpp) */
static int nn_am_service_IApplicationFunctions_CreateCacheStorage(SvcObj* self, u16 index, i64 normal_size,
                                                                 i64 journal_size, u32* target_media,
                                                                 i64* required_size) {
    (void)self, (void)index, (void)normal_size, (void)journal_size;
    *target_media = 1; /* Nand */
    *required_size = 0;
    R_SUCCEED();
}
/* a game is running as soon as it tells AM it is: Eden answers true (application_functions.cpp) */
static int nn_am_service_IApplicationFunctions_NotifyRunning(SvcObj* self, u8* became_running) {
    (void)self;
    *became_running = 1;
    R_SUCCEED();
}
static int nn_am_service_IApplicationFunctions_ExtendSaveData(SvcObj* self, u8 type, const nn_account_Uid* uid,
                                                              i64 normal_size, i64 journal_size, i64* required_size) {
    (void)self, (void)uid;
    if (type == 1 || type == 3) {
        pm_lock(&g_host.am.lock);
        g_hle.am.save_size[type == 3][0] = normal_size;
        g_hle.am.save_size[type == 3][1] = journal_size;
        g_hle.am.save_extended[type == 3] = 1;
        pm_unlock(&g_host.am.lock);
    }
    *required_size = 0; /* the space still missing, when there isn't enough: there always is */
    R_SUCCEED();
}
static int nn_am_service_IApplicationFunctions_GetLimitedApplicationLicenseUpgradableEvent(SvcObj* self,
                                                                                          u32* event) {
    (void)self;
    return new_event(event, "am:license upgradable", 0);
}
/* the same for every run of the game on this machine */
static int nn_am_service_IApplicationFunctions_GetPseudoDeviceId(SvcObj* self, nn_util_Uuid* id) {
    (void)self;
    snx_st_le64(id->v, g_hle.kernel.title_id);
    snx_st_le64(id->v + 8, g_user_id[0]);
    R_SUCCEED();
}
static int nn_am_service_IApplicationFunctions_GetGpuErrorDetectedSystemEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:gpu error", 0);
}
static int nn_am_service_IApplicationFunctions_GetFriendInvitationStorageChannelEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:friend invitation", 0);
}
static int nn_am_service_IApplicationFunctions_TryPopFromFriendInvitationStorageChannel(SvcObj* self,
                                                                                       SvcObj** storage) {
    (void)self, (void)storage;
    R_THROW(AM_NO_DATA);
}
static int nn_am_service_IApplicationFunctions_GetNotificationStorageChannelEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:notification", 0);
}
static int nn_am_service_IApplicationFunctions_TryPopFromNotificationStorageChannel(SvcObj* self, SvcObj** storage) {
    (void)self, (void)storage;
    R_THROW(AM_NO_DATA);
}
static int nn_am_service_IApplicationFunctions_GetHealthWarningDisappearedSystemEvent(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:health warning", 0);
}
static int nn_am_service_IApplicationFunctions_GetUnknownEvent210(SvcObj* self, u32* event) {
    (void)self;
    return new_event(event, "am:unknown 210", 0);
}

/* ============================================================ library applets */
/* AM_IN_DATA and AM_OUT_DATA, the storages an applet holds, and the LibApplet: services_int.h */

/* the inline software keyboard (SwkbdState, SwkbdRequestCommand, SwkbdReplyType) */
#define SWKBD_NOT_INITIALIZED 0
#define SWKBD_HIDDEN          1
#define SWKBD_SHOWN           3
#define SWKBD_REQ_FINALIZE    4
#define SWKBD_REQ_CALC        0xa
#define SWKBD_REQ_UNSET_DICTS 0xc
#define SWKBD_REQ_USER_WORDS  6
#define SWKBD_FINISHED_INIT   0
#define SWKBD_DEFAULT         1
#define SWKBD_CHANGED         2
#define SWKBD_ENTER           5
#define SWKBD_CANCEL          6
#define SWKBD_CHANGED_UTF8    7
#define SWKBD_ENTER_UTF8      9
#define SWKBD_RELEASED_WORDS  0xb
#define SWKBD_UNSET_DICTS     0xc
/* SwkbdCalcArgFlags */
#define SWKBD_SET_INIT   (1u << 0)
#define SWKBD_APPEAR     (1u << 2)
#define SWKBD_SET_TEXT   (1u << 3)
#define SWKBD_SET_CURSOR (1u << 4)
#define SWKBD_SET_UTF8   (1u << 5)
#define SWKBD_DISAPPEAR  (1u << 7)
/* SWKBD_TEXT_CHARS, the calc argument's input text (UTF-16): services_int.h */
#define SWKBD_UTF16_SIZE 0x3ec /* a reply's text, in either encoding */
#define SWKBD_UTF8_SIZE  0x7d4
/* where the two SwkbdCalcArg layouts (the request's whole size tells them apart) keep what is read here, from the
   SwkbdCalcArgCommon at the start: SwkbdCalcArgOld puts the appear argument after the volume and cursor, New first */
#define SWKBD_CALC_OLD_SIZE 0x4a0
#define SWKBD_CALC_NEW_SIZE 0x4e8

static int am_is_inline(const LibApplet* a) {
    return a->applet_id == AM_APPLET_SWKBD && (a->mode == 1 || a->mode == 3); /* PartialForeground(IndirectDisplay) */
}
/* the keyboard's box taken away unanswered when it is for a's inline keyboard, which the game took away (Disappear,
   Finalize, the applet closed). Under the lock */
static void am_kb_drop(const LibApplet* a) {
    if (!g_host.am.kb_open || g_host.am.kb_inline != (u32)(a - g_hle.am.applets) + 1) return;
    backend_keyboard_hide(g_host.am.kb_ticket);
    g_host.am.kb_open = 0;
    g_host.am.kb_inline = 0;
}
static void am_applet_destroy(SvcObj* o) {
    LibApplet* a = (LibApplet*)o->data;
    int i;
    pm_lock(&g_host.am.lock);
    am_kb_drop(a);
    pm_unlock(&g_host.am.lock);
    for (i = 0; i < a->nin; i++) svcobj_unref(a->in[i]);
    for (i = 0; i < a->nout; i++) svcobj_unref(a->out[i]);
    kobj_unref(kobj_at(a->state_ev));
    kobj_unref(kobj_at(a->out_ev));
    kobj_unref(kobj_at(a->interactive_ev));
}
static void am_applet_finish(LibApplet* a) {
    a->completed = 1;
    ev_signal(a->state_ev);
}
/* the applet has finished by the time the game looks, its state changed and its output there; an inline keyboard
   runs until it is finalized */
static SvcObj* am_applet_new(u32 applet_id, u32 mode) {
    SvcObj* o = nn_am_service_ILibraryAppletAccessor_new_data(g_hle.am.applets, g_hle.am.applet_next, "library applets");
    LibApplet* a = (LibApplet*)o->data;
    a->applet_id = applet_id;
    a->mode = mode;
    a->state_ev = owned_event("am:applet state changed");
    a->out_ev = owned_event("am:applet out data");
    a->interactive_ev = owned_event("am:applet interactive out data");
    a->kb_max = SWKBD_TEXT_CHARS - 1;
    o->destroy = am_applet_destroy;
    if (!am_is_inline(a)) {
        am_applet_finish(a);
        ev_signal(a->out_ev);
        if (applet_id != AM_APPLET_SWKBD)
            LOG_LIMIT(4, "am: library applet %#x (mode %u) closed right away\n", applet_id, mode);
    } else
        LOG("am: library applet %#x (mode %u)\n", applet_id, mode);
    return o;
}
static int nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(SvcObj* self, u32 applet_id,
                                                                   u32 library_applet_mode, SvcObj** accessor) {
    (void)self;
    *accessor = am_applet_new(applet_id, library_applet_mode);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletCreator_CreateLibraryAppletEx(SvcObj* self, u32 applet_id,
                                                                     u32 library_applet_mode, u64 thread_id,
                                                                     SvcObj** accessor) {
    (void)self, (void)thread_id;
    *accessor = am_applet_new(applet_id, library_applet_mode);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletCreator_CreateStorage(SvcObj* self, i64 size, SvcObj** storage) {
    (void)self;
    R_UNLESS(size >= 0, AM_INVALID_OFFSET);
    *storage = am_storage_new((u64)size, NULL);
    return *storage ? 0 : AM_OUT_OF_MEMORY;
}
static int nn_am_service_ILibraryAppletCreator_CreateTransferMemoryStorage(SvcObj* self, u8 is_writable, i64 size,
                                                                           u32 transfer_memory, SvcObj** storage) {
    (void)self, (void)is_writable;
    return am_storage_tmem(size, transfer_memory, 0, storage);
}
static int nn_am_service_ILibraryAppletCreator_CreateHandleStorage(SvcObj* self, i64 size, u32 transfer_memory,
                                                                   SvcObj** storage) {
    (void)self;
    return am_storage_tmem(size, transfer_memory, 1, storage);
}

static int nn_am_service_ILibraryAppletAccessor_GetAppletStateChangedEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((LibApplet*)self->data)->state_ev);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_IsCompleted(SvcObj* self, u8* completed) {
    LibApplet* a = (LibApplet*)self->data;
    pm_lock(&g_host.am.lock);
    *completed = (u8)a->completed;
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_RequestExit(SvcObj* self) {
    LibApplet* a = (LibApplet*)self->data;
    pm_lock(&g_host.am.lock);
    am_applet_finish(a);
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_Terminate(SvcObj* self) {
    return nn_am_service_ILibraryAppletAccessor_RequestExit(self);
}
static int nn_am_service_ILibraryAppletAccessor_PushInData(SvcObj* self, SvcObj* storage) {
    LibApplet* a = (LibApplet*)self->data;
    if (!storage) R_SUCCEED();
    pm_lock(&g_host.am.lock);
    if (a->nin < AM_IN_DATA) {
        svcobj_ref(storage);
        a->in[a->nin++] = storage;
    }
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}

/* UTF-16 (n units at in, stopping at a 0) to UTF-8, at most cap - 1 bytes and a terminator */
static void am_utf16_to_8(const u16* in, u64 n, char* out, size_t cap) {
    size_t k = 0;
    u64 i;
    for (i = 0; i < n && in[i]; i++) {
        u32 c = in[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < n && in[i + 1] >= 0xdc00 && in[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (in[i + 1] - 0xdc00u);
            i++;
        }
        if (c < 0x80) {
            if (k + 1 >= cap) break;
            out[k++] = (char)c;
        } else if (c < 0x800) {
            if (k + 2 >= cap) break;
            out[k++] = (char)(0xc0 | c >> 6);
            out[k++] = (char)(0x80 | (c & 0x3f));
        } else if (c < 0x10000) {
            if (k + 3 >= cap) break;
            out[k++] = (char)(0xe0 | c >> 12);
            out[k++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        } else {
            if (k + 4 >= cap) break;
            out[k++] = (char)(0xf0 | c >> 18);
            out[k++] = (char)(0x80 | (c >> 12 & 0x3f));
            out[k++] = (char)(0x80 | (c >> 6 & 0x3f));
            out[k++] = (char)(0x80 | (c & 0x3f));
        }
    }
    out[k] = 0;
}
/* UTF-8 to UTF-16, at most max units (a surrogate pair is never split); returns the units written. A byte that
   can't start a character, a sequence cut short, an overlong form, a surrogate or a value past U+10FFFF each become
   one U+FFFD, as the Unicode standard's maximal subparts */
static u32 am_utf8_to_16(const char* in, u16* out, u32 max) {
    const unsigned char* s = (const unsigned char*)in;
    u32 n = 0, c, need, i;
    while (*s) {
        unsigned char b = *s++, lo = 0x80, hi = 0xbf; /* the range of the second byte */
        if (b < 0x80) {
            c = b;
            need = 0;
        } else if (b >= 0xc2 && b <= 0xdf) {
            c = b & 0x1fu;
            need = 1;
        } else if (b >= 0xe0 && b <= 0xef) {
            c = b & 0x0fu;
            need = 2;
            if (b == 0xe0) lo = 0xa0; /* overlong */
            if (b == 0xed) hi = 0x9f; /* surrogates */
        } else if (b >= 0xf0 && b <= 0xf4) {
            c = b & 0x07u;
            need = 3;
            if (b == 0xf0) lo = 0x90; /* overlong */
            if (b == 0xf4) hi = 0x8f; /* past U+10FFFF */
        } else {
            c = 0xfffd;
            need = 0;
        }
        for (i = 0; i < need; i++, lo = 0x80, hi = 0xbf) {
            if (*s < lo || *s > hi) { /* the NUL at the end too: it is never consumed */
                c = 0xfffd;
                break;
            }
            c = c << 6 | (*s++ & 0x3fu);
        }
        if (c >= 0x10000) {
            if (n + 2 > max) break;
            c -= 0x10000;
            out[n++] = (u16)(0xd800 | c >> 10);
            out[n++] = (u16)(0xdc00 | (c & 0x3ff));
        } else {
            if (n + 1 > max) break;
            out[n++] = (u16)c;
        }
    }
    return n;
}

/* ------------------------------------------------------------ the software keyboard's box */
/* The box over the game asks the player (backend_keyboard_show), and its answer comes on the main thread
   (am_keyboard_answered); g_host.am says who the box shown last is for */
/* a new box's ticket, for applet a's inline keyboard or (a NULL) a whole applet's waiting thread. Under the lock */
static u32 am_kb_ticket(const LibApplet* a) {
    if (!++g_host.am.kb_ticket) ++g_host.am.kb_ticket; /* 0 is none */
    g_host.am.kb_open = 1;
    g_host.am.kb_inline = a ? (u32)(a - g_hle.am.applets) + 1 : 0;
    g_host.am.kb_obj = a ? a->obj : 0;
    return g_host.am.kb_ticket;
}
/* a least above the most is the most, said */
static void am_kb_least(OverlayKeyboard* k) {
    if (k->min <= k->max) return;
    WARN("am: the software keyboard asks for %u to %u characters, %u it is\n", k->min, k->max, k->max);
    k->min = k->max;
}
/* the game's UTF-16 text of at most chars units at p (little-endian, stopping at a 0) into out, UTF-8 */
static void am_kb_text(const u8* p, u32 chars, char* out, size_t size) {
    u16 wide[256];
    u32 i;
    for (i = 0; i < chars && i < 256; i++) wide[i] = snx_ld_le16(p + 2 * i);
    am_utf16_to_8(wide, i, out, size);
}
/* text answered in the player's place, from where (swkbd_text, the game's text, the user's nickname): cut to the
   most, and cancelled short of the least, each said. 0 with the answer in out, 1 cancelled */
static int am_kb_given(const char* where, const char* text, u32 min, u32 max, char* out, size_t size) {
    u16 wide[OVERLAY_KB_UNITS];
    u32 all = am_utf8_to_16(text, wide, OVERLAY_KB_UNITS), n = am_utf8_to_16(text, wide, max);
    if (n < min) {
        WARN("am: the software keyboard is cancelled: %s \"%s\" is %u character%s, the game asks for %u at least\n",
             where, text, n, n == 1 ? "" : "s", min);
        return 1;
    }
    am_utf16_to_8(wide, n, out, size);
    if (n < all)
        LOG("am: the software keyboard answers \"%s\" (%s \"%s\", cut to the game's %u character%s)\n", out, where,
            text, max, max == 1 ? "" : "s");
    return 0;
}
/* with no window for the box of what (headless): the game's text, else the nickname of the user playing, as
   am_kb_given takes it. 0 with the answer in out, 1 cancelled */
static int am_kb_unasked(const char* what, const OverlayKeyboard* k, char* out, size_t size) {
    const char* where = k->text[0] ? "the game's text" : "the user's nickname";
    char nickname[SNX_USERS_NICKNAME + 1];
    const char* text = k->text[0] ? k->text : nickname;
    if (!k->text[0]) acc_playing(NULL, nickname, sizeof nickname);
    LOG("am: no window to show the %s's box in: it answers %s \"%s\"\n", what, where, text);
    return am_kb_given(where, text, k->min, k->max, out, size);
}
/* the box for k, its answer waited for on the game's thread, as the console's applet holds the game until it is done:
   a game pause counts the thread as stopped, and a save state's load makes the request again. 0 with the text in
   out, 1 cancelled, -1 when no box can show (no window) */
static int am_kb_wait(OverlayKeyboard* k, char* out, size_t size) {
    int r;
    if (unattended_active()) return -1; /* nobody is at the screen: the box would wait for nobody, and outlive the run */
    pm_lock(&g_host.am.lock);
    k->ticket = am_kb_ticket(NULL);
    pm_unlock(&g_host.am.lock);
    if (backend_keyboard_show(k)) return -1;
    LOG("am: the software keyboard asks in its box, %u to %u characters\n", k->min, k->max);
    pm_lock(&g_host.am.lock);
    while (g_host.am.kb_answered != k->ticket && g_host.am.kb_ticket == k->ticket)
        kthread_wait(&g_host.am.kb_cv, &g_host.am.lock, -1);
    if (g_host.am.kb_answered != k->ticket) {
        WARN("am: another software keyboard took the box: this one is cancelled\n");
        r = 1;
    } else {
        r = g_host.am.kb_cancelled;
        if (!r) snprintf(out, size, "%s", g_host.am.kb_text);
    }
    pm_unlock(&g_host.am.lock);
    return r;
}

/* the software keyboard as a whole applet: swkbd_text's answer (--swkbd_text=, for scripted runs), else the box's
   (am_kb_unasked's without a window). KeyboardConfig (SwkbdConfigCommon, the second storage, as Eden's
   applet_software_keyboard_types.h lays it out) gives the texts, the kind, the length limits, whether the answer is
   UTF-8, and where the initial text is in the work buffer (the third) */
#define AM_SWKBD_OUT   0x7d8                       /* SwkbdResult and the 0x7d4 byte string buffer */
#define AM_SWKBD_UNITS ((AM_SWKBD_OUT - 4) / 2 - 1) /* the UTF-16 text that fits with its terminator */
static SvcObj* am_swkbd(LibApplet* a) {
    OverlayKeyboard k = {0};
    char text[OVERLAY_KB_TEXT];
    u16 wide[AM_SWKBD_UNITS + 1];
    u32 off = 0, len = 0, n = 0, v;
    int utf8 = 0, r;
    SvcObj* out;
    u8* d;
    const char* env = g_settings.keyboard_text[0] ? g_settings.keyboard_text : NULL;
    k.max = AM_SWKBD_UNITS;
    if (a->nin > 1) {
        u64 size = ((StorageData*)a->in[1]->data)->size;
        const u8* cfg = am_storage_bytes(a->in[1]);
        if (size >= 0x3b4) {
            k.numbers = snx_ld_le32(cfg) == 1; /* SwkbdType NumberPad */
            am_kb_text(cfg + 0x4, 8, k.ok, sizeof k.ok);
            am_kb_text(cfg + 0x24, 64, k.header, sizeof k.header);
            am_kb_text(cfg + 0xa6, 128, k.sub, sizeof k.sub);
            am_kb_text(cfg + 0x1a8, 256, k.guide, sizeof k.guide);
            v = snx_ld_le32(cfg + 0x3ac); /* max_text_length: 0 for no limit */
            if (v && v < k.max) k.max = v;
            k.min = snx_ld_le32(cfg + 0x3b0); /* min_text_length */
        }
        if (size >= 0x3d4) {
            k.password = snx_ld_le32(cfg + 0x3b4) != 0; /* password_mode */
            utf8 = cfg[0x3bd];
            off = snx_ld_le32(cfg + 0x3c0); /* initial_string_offset and _length, in the work buffer */
            len = snx_ld_le32(cfg + 0x3c4);
        }
    }
    am_kb_least(&k);
    if (len && a->nin > 2) {
        u64 size = ((StorageData*)a->in[2]->data)->size;
        if (off < size && len <= (size - off) / 2 && len <= AM_SWKBD_UNITS) {
            memcpy(wide, am_storage_bytes(a->in[2]) + off, (size_t)len * 2);
            snx_le16_array(wide, len);
            am_utf16_to_8(wide, len, k.text, sizeof k.text);
        }
    }
    if (env)
        r = am_kb_given("swkbd_text", env, k.min, k.max, text, sizeof text);
    else if ((r = am_kb_wait(&k, text, sizeof text)) < 0)
        r = am_kb_unasked("software keyboard", &k, text, sizeof text);
    if (!r) n = am_utf8_to_16(text, wide, k.max);
    out = am_storage_new(AM_SWKBD_OUT, NULL);
    d = am_storage_bytes(out);
    am_put32(d, r ? 1 : 0); /* SwkbdResult: Ok, Cancel */
    if (!r) {
        if (utf8)
            am_utf16_to_8(wide, n, (char*)d + 4, AM_SWKBD_OUT - 4);
        else {
            memcpy(d + 4, wide, (size_t)n * 2);
            snx_le16_array(d + 4, n);
        }
    }
    LOG("am: swkbd %s with %u UTF-16 code units\n", r ? "cancelled" : "accepted", r ? 0 : n);
    return out;
}

/* ------------------------------------------------------------ the inline keyboard */
/* a reply (SwkbdState, SwkbdReplyType, then size - 8 bytes) queued for PopInteractiveOutData; its body to fill */
static u8* kb_reply(LibApplet* a, u32 type, u32 size) {
    SvcObj* s;
    u8* d;
    if (a->nout >= AM_OUT_DATA) return NULL;
    s = am_storage_new(size, NULL);
    d = am_storage_bytes(s);
    am_put32(d, a->kb_state);
    am_put32(d + 4, type);
    a->out[a->nout++] = s;
    ev_signal(a->interactive_ev);
    return d + 8;
}
static void kb_show(LibApplet* a, u32 state) {
    a->kb_state = state;
    kb_reply(a, SWKBD_DEFAULT, 8);
}
/* the text, in the encoding the game asked for, ahead of its arguments (SwkbdChangedStringArg: the length,
   no dictionary range, the cursor; SwkbdDecidedEnterArg: the length) */
static void kb_text_reply(LibApplet* a, int decided) {
    u32 type = a->kb_utf8 ? (decided ? SWKBD_ENTER_UTF8 : SWKBD_CHANGED_UTF8) : (decided ? SWKBD_ENTER : SWKBD_CHANGED);
    u32 text = a->kb_utf8 ? SWKBD_UTF8_SIZE : SWKBD_UTF16_SIZE;
    u8* d = kb_reply(a, type, 8 + text + (decided ? 4 : 16));
    if (!d) return;
    if (a->kb_utf8)
        am_utf16_to_8(a->kb_text, a->kb_len, (char*)d, SWKBD_UTF8_SIZE);
    else {
        u32 units = a->kb_len < SWKBD_UTF16_SIZE / 2 ? a->kb_len : SWKBD_UTF16_SIZE / 2 - 1;
        memcpy(d, a->kb_text, (size_t)units * 2);
        snx_le16_array(d, units);
    }
    am_put32(d + text, a->kb_len);
    if (!decided) {
        am_put32(d + text + 4, ~0u);
        am_put32(d + text + 8, ~0u);
        am_put32(d + text + 12, (u32)a->kb_cursor);
    }
}
/* the inline keyboard's answer: the text given (NULL: cancelled), its replies, and the keyboard hidden. Under the lock */
static void kb_decide(LibApplet* a, const char* text) {
    if (!text)
        kb_reply(a, SWKBD_CANCEL, 8);
    else {
        a->kb_len = am_utf8_to_16(text, a->kb_text, a->kb_max);
        a->kb_cursor = (i32)a->kb_len;
        kb_text_reply(a, 0);
        kb_text_reply(a, 1);
    }
    kb_show(a, SWKBD_HIDDEN);
    LOG("am: the inline keyboard %s with %u UTF-16 code units\n", text ? "decided" : "cancelled", text ? a->kb_len : 0);
}
/* Appear, its SwkbdAppearArg at arg (the kind, the OK's label, the limits): shown, and answered by swkbd_text at once,
   by the player in the box over the game (kb_decide once am_keyboard_answered has it, the game going on meanwhile),
   or without a window as am_kb_unasked answers. Under the lock */
static void kb_appear(LibApplet* a, const u8* arg) {
    OverlayKeyboard k = {0};
    char text[OVERLAY_KB_TEXT];
    const char* env = g_settings.keyboard_text[0] ? g_settings.keyboard_text : NULL;
    u32 max = snx_ld_le32(arg + 0x20); /* max_text_length */
    kb_show(a, SWKBD_SHOWN);
    a->kb_max = max && max < SWKBD_TEXT_CHARS - 1 ? max : SWKBD_TEXT_CHARS - 1;
    k.max = a->kb_max;
    k.min = snx_ld_le32(arg + 0x24); /* min_text_length */
    am_kb_least(&k);
    k.numbers = snx_ld_le32(arg) == 1; /* SwkbdType NumberPad */
    am_kb_text(arg + 0x4, 8, k.ok, sizeof k.ok);
    am_utf16_to_8(a->kb_text, a->kb_len, k.text, sizeof k.text);
    if (env) {
        kb_decide(a, am_kb_given("swkbd_text", env, k.min, k.max, text, sizeof text) ? NULL : text);
        return;
    }
    k.ticket = am_kb_ticket(a);
    if (!unattended_active() && !backend_keyboard_show(&k)) {
        LOG("am: the inline keyboard asks in its box, %u to %u characters\n", k.min, k.max);
        return;
    }
    g_host.am.kb_open = 0;
    kb_decide(a, am_kb_unasked("inline keyboard", &k, text, sizeof text) ? NULL : text);
}
/* Calc: the SwkbdCalcArg after the command word, either layout */
static void kb_calc(LibApplet* a, const u8* req, u64 size) {
    const u8* arg = req + 4;
    u64 flags;
    u32 appear, cursor, text, i;
    if (size < 4 + 0x18) return;
    flags = snx_ld_le64(arg + 8);
    if (size >= 4 + SWKBD_CALC_NEW_SIZE) {
        appear = 0x18; /* SwkbdCalcArgNew: appear argument, volume, cursor, text, utf8 mode */
        cursor = 0x8c;
        text = 0x90;
    } else if (size >= 4 + SWKBD_CALC_OLD_SIZE) {
        appear = 0x20; /* SwkbdCalcArgOld: volume, cursor, appear argument, text, utf8 mode */
        cursor = 0x1c;
        text = 0x68;
    } else
        return;
    if (flags & SWKBD_SET_TEXT) {
        for (i = 0; i < SWKBD_TEXT_CHARS - 1; i++) {
            a->kb_text[i] = snx_ld_le16(arg + text + 2 * i);
            if (!a->kb_text[i]) break;
        }
        a->kb_len = i;
    }
    if (flags & SWKBD_SET_CURSOR) a->kb_cursor = (i32)snx_ld_le32(arg + cursor);
    if (flags & SWKBD_SET_UTF8) a->kb_utf8 = arg[text + 2 * SWKBD_TEXT_CHARS] != 0;
    if (a->kb_state == SWKBD_NOT_INITIALIZED && (flags & SWKBD_SET_INIT)) {
        a->kb_state = SWKBD_HIDDEN;
        kb_reply(a, SWKBD_FINISHED_INIT, 9);
    }
    if (!(flags & SWKBD_SET_INIT) && (flags & (SWKBD_SET_TEXT | SWKBD_SET_CURSOR)) && a->kb_state != SWKBD_NOT_INITIALIZED)
        kb_text_reply(a, 0);
    if (a->kb_state == SWKBD_HIDDEN && (flags & SWKBD_APPEAR))
        kb_appear(a, arg + appear);
    else if (a->kb_state == SWKBD_SHOWN && (flags & SWKBD_DISAPPEAR)) {
        am_kb_drop(a);
        kb_show(a, SWKBD_HIDDEN);
    }
}
static int nn_am_service_ILibraryAppletAccessor_PushInteractiveInData(SvcObj* self, SvcObj* storage) {
    LibApplet* a = (LibApplet*)self->data;
    StorageData* s;
    const u8* req;
    u32 cmd;
    if (!storage || !am_is_inline(a)) R_SUCCEED();
    s = (StorageData*)storage->data;
    if (s->size < 4) R_SUCCEED();
    req = am_storage_bytes(storage);
    cmd = snx_ld_le32(req);
    pm_lock(&g_host.am.lock);
    switch (cmd) {
    case SWKBD_REQ_FINALIZE:
        am_kb_drop(a);
        a->kb_state = SWKBD_NOT_INITIALIZED;
        am_applet_finish(a);
        break;
    case SWKBD_REQ_CALC: kb_calc(a, req, s->size); break;
    case SWKBD_REQ_USER_WORDS: kb_reply(a, SWKBD_RELEASED_WORDS, 8); break;
    case SWKBD_REQ_UNSET_DICTS: kb_reply(a, SWKBD_UNSET_DICTS, 8); break;
    default: break; /* the dictionaries and the V2 reply flags change nothing here */
    }
    pm_unlock(&g_host.am.lock);
    R_SUCCEED();
}
void am_keyboard_answered(u32 ticket, int cancelled, const char* text) {
    pm_lock(&g_host.am.lock);
    if (!g_host.am.kb_open || ticket != g_host.am.kb_ticket)
        LOG_LIMIT(4,
                  "am: the software keyboard's box %u answered after the game took it away: the game has no answer\n",
                  ticket);
    else if (g_host.am.kb_inline) {
        LibApplet* a = &g_hle.am.applets[g_host.am.kb_inline - 1];
        if (a->obj == g_host.am.kb_obj && a->kb_state == SWKBD_SHOWN) kb_decide(a, cancelled ? NULL : text);
        g_host.am.kb_open = 0;
        g_host.am.kb_inline = 0;
    } else {
        g_host.am.kb_open = 0;
        g_host.am.kb_answered = ticket;
        g_host.am.kb_cancelled = cancelled;
        snprintf(g_host.am.kb_text, sizeof g_host.am.kb_text, "%s", cancelled ? "" : text);
        pc_broadcast(&g_host.am.kb_cv);
    }
    pm_unlock(&g_host.am.lock);
}
void am_state_loaded(void) {
    int i;
    pm_lock(&g_host.am.lock);
    if (g_host.am.kb_open) backend_keyboard_hide(g_host.am.kb_ticket);
    g_host.am.kb_open = 0;
    g_host.am.kb_inline = 0;
    for (i = 0; i < AM_APPLETS; i++) { /* its least isn't in the state: the box gives any line */
        LibApplet* a = &g_hle.am.applets[i];
        OverlayKeyboard k = {0};
        char text[OVERLAY_KB_TEXT];
        if (!a->obj || !am_is_inline(a) || a->kb_state != SWKBD_SHOWN) continue;
        k.max = a->kb_max;
        am_utf16_to_8(a->kb_text, a->kb_len, k.text, sizeof k.text);
        k.ticket = am_kb_ticket(a);
        if (backend_keyboard_show(&k)) {
            g_host.am.kb_open = 0;
            kb_decide(a, am_kb_unasked("inline keyboard", &k, text, sizeof text) ? NULL : text);
        }
        break;
    }
    pm_unlock(&g_host.am.lock);
}

/* the Mii editor (nn::mii::AppletInput, 0x100 bytes: version, mode, then a v4's special key code and CharInfo), with
   no editor and no database to add to: an edit gives the Mii back unchanged, the modes that only show something
   succeed, and adding or creating a Mii is cancelled, as when the player backs out of the editor. The answer is
   nn::mii::AppletOutput {result, index} or, for create and edit, AppletOutputForCharInfoEditing {result, CharInfo} */
#define MII_EDIT_SHOW         0
#define MII_EDIT_APPEND       1
#define MII_EDIT_CREATE       4
#define MII_EDIT_EDIT         5
#define MII_EDIT_SUCCESS      0
#define MII_EDIT_CANCEL       1
#define MII_EDIT_INPUT_SIZE   0x100
#define MII_EDIT_CHARINFO     0xc /* the v4 input's CharInfo, after the version, the mode and the special key code */
#define MII_EDIT_CHARINFO_LEN 0x58
static SvcObj* am_mii_edit(LibApplet* a) {
    const u8* in = NULL;
    u32 mode = MII_EDIT_APPEND, result;
    SvcObj* out;
    int i;
    for (i = 0; i < a->nin && !in; i++)
        if (((StorageData*)a->in[i]->data)->size >= MII_EDIT_INPUT_SIZE) in = am_storage_bytes(a->in[i]);
    if (in) mode = snx_ld_le32(in + 4);
    if (mode == MII_EDIT_CREATE || mode == MII_EDIT_EDIT) {
        result = mode == MII_EDIT_EDIT && in ? MII_EDIT_SUCCESS : MII_EDIT_CANCEL;
        out = am_storage_new(0x80, NULL);
        am_put32(am_storage_bytes(out), result);
        if (result == MII_EDIT_SUCCESS)
            memcpy(am_storage_bytes(out) + 4, in + MII_EDIT_CHARINFO, MII_EDIT_CHARINFO_LEN);
        return out;
    }
    result = mode == MII_EDIT_APPEND || mode > MII_EDIT_EDIT ? MII_EDIT_CANCEL : MII_EDIT_SUCCESS;
    out = am_storage_new(0x20, NULL);
    am_put32(am_storage_bytes(out), result);
    LOG("am: Mii editor mode %u answered %s\n", mode, result == MII_EDIT_SUCCESS ? "success" : "cancel");
    return out;
}
/* A web applet's arguments (Switchbrew's "Web applet"): a storage starting with a WebArgHeader (u16 count, u16
   padding, u32 shim kind) and count entries, each a u16 type, a u16 size, 4 bytes of padding and size bytes. The
   data of the first entry of type, or NULL */
#define WEB_ARG_DOCUMENT_PATH 0x6 /* the SDK's ShowOfflinePageArgDetail::SetDocumentPath */
#define WEB_ARG_DOCUMENT_KIND 0x7 /* u32: 1 an offline page of the game's HtmlDocument content (SetDocumentKind) */
static const u8* am_web_arg(LibApplet* a, u16 type, u16* size) {
    int i;
    for (i = 0; i < a->nin; i++) {
        u64 n = ((StorageData*)a->in[i]->data)->size, at = 8;
        const u8* p = am_storage_bytes(a->in[i]);
        u32 count, k;
        if (n < 8) continue;
        count = snx_ld_le16(p);
        for (k = 0; k < count && at + 8 <= n; k++) {
            u16 t = snx_ld_le16(p + at), s = snx_ld_le16(p + at + 2);
            if (s > n - at - 8) break;
            if (t == type) {
                *size = s;
                return p + at + 8;
            }
            at += 8 + s;
        }
    }
    return NULL;
}
/* the text of a quoted string at s (' or "), copied into out (cap bytes with its 0): its end, or NULL */
static const char* am_js_string(const char* s, char* out, size_t cap) {
    char q = *s;
    size_t n = 0;
    if (q != '\'' && q != '"') return NULL;
    for (s++; *s && *s != q; s++)
        if (n + 1 < cap) out[n++] = *s;
    out[n] = 0;
    return *s ? s + 1 : NULL;
}
static const char* am_skip_space(const char* s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    return s;
}
/* the URL the script function name sends the page to, from its body in text (var name = function() {...}, name =
   function() {...} or function name() {...}: its first location.href = "..."), into url: 0 without one */
static int am_handler_url(const char* text, const char* name, char* url, size_t cap) {
    size_t n = strlen(name);
    const char* h;
    for (h = strstr(text, name); h; h = strstr(h + 1, name)) {
        const char *after = am_skip_space(h + n), *body, *end, *l;
        int depth = 0;
        if ((h > text && (isalnum((unsigned char)h[-1]) || h[-1] == '_' || h[-1] == '$')) ||
            isalnum((unsigned char)h[n]) || h[n] == '_' || h[n] == '$')
            continue; /* a longer name */
        if (*after == '=' && after[1] != '=') {
            after = am_skip_space(after + 1);
            if (strncmp(after, "function", 8)) continue;
        } else if (h - text < 9 || strncmp(h - 9, "function ", 9))
            continue;
        if (!(body = strchr(after, '{'))) return 0;
        for (end = body; *end; end++) { /* the body's end, its braces balanced */
            if (*end == '{') depth++;
            if (*end == '}' && !--depth) break;
        }
        for (l = strstr(body, "location.href"); l && l < end; l = strstr(l + 1, "location.href")) {
            const char* v = am_skip_space(l + 13);
            if (*v == '=' && v[1] != '=' && am_js_string(am_skip_space(v + 1), url, cap)) return 1;
        }
        return 0;
    }
    return 0;
}
/* The page's A button, as the console's offline pages assign it (window.nx.footer.setAssign('A', label, handler)):
   the URL its handler sends the page to (location.href = "..."), from the page and the scripts it loads, into url
   (cap bytes). 0 when the page or such an assignment isn't found */
static int am_page_a_url(const char* page, char* url, size_t cap) {
    u64 size = 0;
    char *text = NULL, *dir, *path;
    const char* s;
    size_t total = 0, keep;
    u8* html = fs_html_document(page, &size);
    int found = 0;
    if (!html) return 0;
    /* the page and each script it loads (<script src="..."> beside it) as one text */
    keep = (size_t)(strrchr(page, '/') ? strrchr(page, '/') - page + 1 : 0);
    dir = (char*)malloc(keep + 1);
    text = (char*)malloc((size_t)size + 1);
    if (!dir || !text) goto done;
    memcpy(dir, page, keep);
    dir[keep] = 0;
    memcpy(text, html, (size_t)size);
    text[size] = 0;
    total = (size_t)size;
    for (s = strstr((char*)text, "<script"); s; s = strstr(s + 1, "<script")) {
        char src[256];
        const char *attr = strstr(s, "src="), *end = strchr(s, '>');
        u64 n = 0;
        u8* js;
        char* grown;
        if (!attr || !end || attr > end || !am_js_string(attr + 4, src, sizeof src)) continue;
        path = snx_format("%s%s", dir, src);
        js = path ? fs_html_document(path, &n) : NULL;
        free(path);
        if (!js) continue;
        keep = (size_t)(s - text);
        grown = (char*)realloc(text, total + (size_t)n + 2);
        if (grown) {
            text = grown;
            text[total] = '\n';
            memcpy(text + total + 1, js, (size_t)n);
            total += (size_t)n + 1;
            text[total] = 0;
        }
        free(js);
        s = text + keep;
    }
    for (s = strstr(text, "setAssign("); s && !found; s = strstr(s + 1, "setAssign(")) {
        char button[8], name[64];
        const char* at = am_js_string(am_skip_space(s + 10), button, sizeof button);
        size_t n = 0;
        if (!at || strcmp(button, "A") || *(at = am_skip_space(at)) != ',') continue;
        at = am_skip_space(at + 1); /* the label, a string (which may hold a comma) or a name, then the handler */
        if (*at == '\'' || *at == '"') at = am_js_string(at, name, sizeof name);
        if (!at || !(at = strchr(at, ','))) continue;
        at = am_skip_space(at + 1);
        while ((isalnum((unsigned char)at[n]) || at[n] == '_' || at[n] == '$') && n + 1 < sizeof name) n++;
        if (!n) continue;
        memcpy(name, at, n);
        name[n] = 0;
        found = am_handler_url(text, name, url, cap);
    }
done:
    free(text);
    free(dir);
    free(html);
    return found;
}

static int nn_am_service_ILibraryAppletAccessor_PopOutData(SvcObj* self, SvcObj** storage) {
    LibApplet* a = (LibApplet*)self->data;
    u8* d;
    u32 first = 0;
    int popped;
    pm_lock(&g_host.am.lock);
    popped = a->out_popped || am_is_inline(a);
    pm_unlock(&g_host.am.lock);
    R_UNLESS(!popped, AM_NO_DATA);
    switch (a->applet_id) {
    case AM_APPLET_CONTROLLER: { /* ControllerSupportResultInfo: the players connected and the first of them */
        /* ControllerSupportArg, the last storage pushed, starts with the fewest players the game accepts */
        int players = hid_connected_players(&first);
        int least = a->nin ? (signed char)am_storage_bytes(a->in[a->nin - 1])[0] : 0;
        *storage = am_storage_new(0xc, NULL);
        d = am_storage_bytes(*storage);
        d[0] = (u8)players;
        am_put32(d + 4, first);
        if (players < least) { /* nobody to pick: the player closed the applet */
            LOG("am: the controller applet wants %d players, %d are connected: cancelled\n", least, players);
            am_put32(d + 8, 2); /* the result: cancelled */
        }
        break;
    }
    case AM_APPLET_PROFILE_SELECT: { /* UiReturnArg: success, the user the game plays as */
        u64 uid[2];
        acc_playing(uid, NULL, 0);
        *storage = am_storage_new(0x18, NULL);
        am_put_uid(am_storage_bytes(*storage) + 8, uid);
        break;
    }
    case AM_APPLET_SWKBD: *storage = am_swkbd(a); break;
    case AM_APPLET_MII_EDIT: *storage = am_mii_edit(a); break;
    case AM_APPLET_WEB:
    case AM_APPLET_SHOP:
    case AM_APPLET_OFFLINE_WEB:
    case AM_APPLET_LOGIN_SHARE:
    case AM_APPLET_WEB_AUTH: { /* WebCommonReturnValue: the page was closed (0), no last URL */
        /* An offline page of the game's own is answered as the player pressing A on it: the URL its A button sends
           it to, which ends the applet with CallbackUrl (3) when it is a callback URL. One game's online agreement
           page sends Accept to http://localhost/?ret=Yes, and the game takes nothing else (exit reason 3 and that
           prefix) as an answer: closed, it asked again, for ever */
        u16 n = 0, k = 0;
        const u8* path = a->applet_id == AM_APPLET_OFFLINE_WEB ? am_web_arg(a, WEB_ARG_DOCUMENT_PATH, &n) : NULL;
        const u8* kind = path ? am_web_arg(a, WEB_ARG_DOCUMENT_KIND, &k) : NULL;
        char page[0x301], url[0x1000];
        *storage = am_storage_new(0x1010, NULL);
        d = am_storage_bytes(*storage);
        if (!path || !kind || k < 4 || snx_ld_le32(kind) != 1) break;
        memcpy(page, path, n < sizeof page - 1 ? n : sizeof page - 1);
        page[n < sizeof page - 1 ? n : sizeof page - 1] = 0;
        if (am_page_a_url(page, url, sizeof url) && !strncmp(url, "http://localhost/", 17)) {
            am_put32(d, 3);
            memcpy(d + 8, url, strlen(url) + 1);
            snx_st_le64(d + 0x1008, strlen(url) + 1);
            LOG_LIMIT(4, "am: offline page %s: its A button sends it to %s, the applet's answer\n", page, url);
        } else
            LOG_LIMIT(4, "am: offline page %s: closed (no callback URL found for its A button)\n", page);
        break;
    }
    default: /* the other applets' results start with a zero result code */
        *storage = am_storage_new(0x10, NULL);
        break;
    }
    /* given once, from here only: the keyboard's thread waits above, and a save state taken meanwhile pops again */
    pm_lock(&g_host.am.lock);
    a->out_popped = 1;
    pm_unlock(&g_host.am.lock);
    ev_clear(a->out_ev);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(SvcObj* self, SvcObj** storage) {
    LibApplet* a = (LibApplet*)self->data;
    int rc = 0;
    pm_lock(&g_host.am.lock);
    if (!a->nout)
        rc = AM_NO_DATA;
    else {
        *storage = a->out[0];
        a->nout--;
        memmove(a->out, a->out + 1, (size_t)a->nout * sizeof a->out[0]);
    }
    if (!a->nout) ev_clear(a->interactive_ev);
    pm_unlock(&g_host.am.lock);
    return rc;
}
static int nn_am_service_ILibraryAppletAccessor_GetPopOutDataEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((LibApplet*)self->data)->out_ev);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_GetPopInteractiveOutDataEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((LibApplet*)self->data)->interactive_ev);
    R_SUCCEED();
}
static int nn_am_service_ILibraryAppletAccessor_GetLibraryAppletInfo(SvcObj* self,
                                                                    nn_am_service_LibraryAppletInfo* info) {
    LibApplet* a = (LibApplet*)self->data;
    info->applet_id = a->applet_id;
    info->library_applet_mode = a->mode;
    R_SUCCEED();
}
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_am_state_fns,
               STATE_FN("am", am_storage_destroy),
               STATE_FN("am", am_accessor_destroy),
               STATE_FN("am", am_lock_destroy),
               STATE_FN("am", am_applet_destroy));
