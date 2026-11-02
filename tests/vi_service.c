/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vi.c's binder: the native window OpenLayer gives, and a buffer queue's preallocate, connect, dequeue, request and
   queue transactions as byte-exact parcels, with the GPU thread and the renderer stood in for */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>

#include "nx/hle/services/vi.c"
#include "nx/hle/hle.c"

u8* g_mem;
static unsigned test_checks, test_objects, test_kobjs;
static u32 test_next_handle = 0x100;
/* A session can close after the present callback drops the display lock but before it signals its event. */
static SvcObj* close_on_unlock;
static u32 unlocks_until_close;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "vi_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) {
    (void)m;
    if (unlocks_until_close && !--unlocks_until_close) {
        SvcObj* session = close_on_unlock;
        close_on_unlock = NULL;
        svcobj_unref(session);
    }
}
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    (void)c, (void)m, (void)timeout_ns;
    test_fail("a dequeue that would wait forever", __LINE__);
    return 1;
}
int kthread_wait(PCond* c, PMutex* m, i64 timeout_ns) { return pc_wait(c, m, timeout_ns); }
void kthread_commit(void) {}
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    test_objects++;
    return obj;
}
#include "svcobj_data.h"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    if (obj->destroy) obj->destroy(obj); /* as ipc.c's, before the data goes */
    test_svcobj_data_free(obj);
    free(obj);
    test_objects--;
}
void out_obj(IpcCtx* c, SvcObj* obj) { c->out_objs[c->nout_objs++] = obj; }
void out_copy_h(IpcCtx* c, u32 handle) { c->out_copy[c->nout_copy++] = handle; }
void out_move_h(IpcCtx* c, u32 handle) { c->out_move[c->nout_move++] = handle; }
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}
int ipc_unknown(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 221);
}
void svc_register(const char* name, SvcCtor ctor) { (void)name, (void)ctor; }
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    if (!o) return; /* as the kernel's: the id of no object */
    CHECK(o->refs > 0);
    if (--o->refs) return;
    free(o);
    test_kobjs--;
}
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    e->o.type = KO_EVENT;
    kobj_set_name(&e->o, name);
    test_kobjs++;
    return e;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
static KObj* test_handles[256]; /* what each handle from 0x100 on holds */
static int test_handles_full;   /* h_new fails, as with a full handle table */
u32 h_new(KObj* o) {
    if (test_handles_full) return 0;
    kobj_ref(o);
    CHECK(test_next_handle - 0x100 < sizeof test_handles / sizeof *test_handles);
    test_handles[test_next_handle - 0x100] = o;
    return test_next_handle++;
}
KObj* h_get_ref(u32 h, int t) {
    KObj* o = h - 0x100 < sizeof test_handles / sizeof *test_handles ? test_handles[h - 0x100] : NULL;
    if (!o || o->type != t) return NULL;
    kobj_ref(o);
    return o;
}
int h_close(u32 h) {
    KObj* o = h - 0x100 < sizeof test_handles / sizeof *test_handles ? test_handles[h - 0x100] : NULL;
    CHECK(o != NULL);
    test_handles[h - 0x100] = NULL;
    kobj_unref(o);
    return 0;
}
u32 ev_handle(u32 ev) { return h_new(kobj_at(ev)); }
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = 0;
    free(e);
    test_kobjs--;
    return 0;
}
u32 owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return kobj_id(&e->o);
}

/* the GPU thread runs the present at once, or keeps it for later while hold is set (a frame whose fences are still
   ahead); the renderer keeps what it was asked to show */
static u32 shown_n, shown_nvmap, shown_w, shown_h, shown_pitch, shown_layout, shown_bhl, shown_fmt, fence_n, fence_id;
static u32 paced;
static int hold;
static void (*held_fn)(void*);
static void* held_arg;
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg) {
    fence_n = n;
    fence_id = n ? id[0] : 0;
    CHECK(!n || val[0] == 10);
    if (hold)
        held_fn = fn, held_arg = arg;
    else
        fn(arg);
}
void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform, int slot) {
    (void)offset, (void)crop, (void)transform, (void)slot;
    shown_n++;
    shown_nvmap = nvmap_id, shown_w = w, shown_h = h, shown_pitch = pitch;
    shown_layout = layout, shown_bhl = bhl, shown_fmt = fmt;
}
void present_pace(u32 interval, int slot) {
    (void)slot;
    paced = interval;
}
/* one native window image, 77: 1280 x 720 RGBA8 in 16-GOB blocks, 20 GOBs a row, over nvmap handle 9 */
int nv_native_window_image(u32 image, u32* memory, NvGsvImageShape* shape) {
    if (image != 77) return RC(1, 121);
    *memory = 9;
    *shape = (NvGsvImageShape){0x02CF04FF, 20, 7, 1};
    return 0;
}

static u32 rd32(u64 a) { return snx_ld_le32(g_mem + a); } /* the parcels' words are the guest's, little-endian */
/* a request parcel at 0x2000: the header, the interface token, then the payload */
static IpcBuf parcel(const void* payload, u32 n) {
    static const char token[] = "android.gui.IGraphicBufferProducer";
    u32 len = (u32)strlen(token), i, hdr[4], data = 8 + ((len + 1) * 2 + 3) / 4 * 4 + n;
    u8* p = g_mem + 0x2000;
    IpcBuf b;
    memset(p, 0, 0x1000);
    hdr[0] = data, hdr[1] = 16, hdr[2] = 0, hdr[3] = 16 + data;
    snx_le32_array(hdr, 4);
    memcpy(p, hdr, 16);
    snx_st_le32(p + 16, 0x100);
    snx_st_le32(p + 20, len);
    for (i = 0; i < len; i++) p[24 + 2 * i] = (u8)token[i];
    memcpy(p + 16 + data - n, payload, n);
    b.addr = 0x2000, b.size = 16 + data, b.flags = 0;
    return b;
}
static i32 binder = 1; /* the layer transact talks to */
static u32 transact(u32 code, const void* payload, u32 n) {
    IpcBuf reply = {0x4000, 0x400, 0};
    memset(g_mem + 0x4000, 0xcc, 0x400);
    CHECK(nns_hosbinder_IHOSBinderDriver_TransactParcel(NULL, binder, code, parcel(payload, n), reply, 0) == 0);
    CHECK(rd32(0x4004) == 16 && rd32(0x4008) == 0 && rd32(0x400c) == 16 + rd32(0x4000));
    return rd32(0x4000); /* the data's size; it starts at 0x4010 and ends with the status */
}

/* the Switch 2 display session: what its notification bits and request-group events tell the guest */
static void test_vi2(void) {
    SvcObj* s = vi2_session_new();
    Vi2Session* d = s->data;
    u32 event = 0, last = 9, g0 = 0, g1 = 0, dup = 0;
    IpcBuf out = {0x6000, 8, 0};
    CHECK(nn_gsv_IVi2Session_GetNotificationLimit(s, &last) == 0 && last == 0);
    CHECK(nn_gsv_IVi2Session_GetNotificationEvent(s, 1, &event) == RC(1, 116) && !event);
    CHECK(nn_gsv_IVi2Session_GetNotificationEvent(s, 0, &event) == 0 && event);
    KEvent* note = kev(d->event);
    CHECK(note && !note->signaled);

    /* a client's bits are its own; its retirement is a notification the guest takes once, and frees its bits */
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x1000, 3, 4) == 0);
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x2000, 4, 5) == RC(1, 125));
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x1000, 6, 7) == RC(1, 125));
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x2000, 6, 6) == RC(1, 116));
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x2000, 6, VI2_NOTIFICATIONS) == RC(1, 116));
    u64 free_slots = 0;
    CHECK(nn_gsv_IVi2Session_GetClientAvailability(s, 0x1000, &free_slots) == 0 && free_slots == 0x7FFF);
    CHECK(nn_gsv_IVi2Session_UnregisterClientState(s, 0x1000) == 0 && note->signaled);
    CHECK(nn_gsv_IVi2Session_UnregisterClientState(s, 0x1000) == RC(1, 121));
    /* the guest keeps (last + 32) / 32 words of bits: a longer buffer gets only those, its rest stays as it was */
    memset(g_mem + 0x6000, 0xcc, 8);
    CHECK(nn_gsv_IVi2Session_GetNotifications(s, out) == 0 && rd32(0x6000) == 1u << 3 && rd32(0x6004) == 0xCCCCCCCC);
    CHECK(nn_gsv_IVi2Session_GetNotifications(s, out) == 0 && rd32(0x6000) == 0);
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x2000, 4, 5) == 0);

    /* the display: the last close sets the bit it was opened with; a client binds to it while it is open, and one
       bound client holds its endpoint */
    CHECK(nn_gsv_IVi2Session_BindClientDisplay(s, 0x2000, VI2_DEFAULT_DISPLAY) == RC(1, 121));
    /* Stream-play opens all three output identities during initialization, before any guest connects.
       Closing one must neither close the local display nor signal another display's notification bit. */
    const u64 players[] = {0x0031726579616c70ull, 0x0032726579616c70ull, 0x0033726579616c70ull};
    CHECK(nn_gsv_IVi2Session_OpenDisplay(s, VI2_DEFAULT_DISPLAY, 31) == 0);
    for (u32 player = 0; player < 3; ++player) {
        u64 format = 0;
        u16 white = 0;
        CHECK(nn_gsv_IVi2Session_OpenDisplay(s, players[player], 24 + player) == 0);
        CHECK(nn_gsv_IVi2Session_OpenDisplay(s, players[player], 24 + player) == 0);
        CHECK(nn_gsv_IVi2Session_QueryHdrStatus(s, players[player], &format, &white) == 0 && format == 1);
        CHECK(nn_gsv_IVi2Session_QueryHdrStatus(s, VI2_DEFAULT_DISPLAY, &format, &white) == 0);
        CHECK(nn_gsv_IVi2Session_BindClientDisplay(s, 0x2000, players[player]) == 0);
        CHECK(nn_gsv_IVi2Session_OpenEndpoint(s, VI2_DEFAULT_DISPLAY, VI2_DEFAULT_DISPLAY, 0x2000) == RC(1, 121));
        CHECK(vi2_state_valid(&g_hle.vi));
        CHECK(nn_gsv_IVi2Session_UnbindClientDisplay(s, 0x2000) == 0);
        note->signaled = 0;
        CHECK(nn_gsv_IVi2Session_CloseDisplay(s, players[player]) == 0 && !note->signaled);
        CHECK(nn_gsv_IVi2Session_CloseDisplay(s, players[player]) == 0 && note->signaled);
        CHECK(nn_gsv_IVi2Session_QueryHdrStatus(s, players[player], &format, &white) == VI_NOT_FOUND);
        CHECK(nn_gsv_IVi2Session_CloseDisplay(s, players[player]) == RC(1, 121));
        CHECK(nn_gsv_IVi2Session_GetNotifications(s, out) == 0 && rd32(0x6000) == 1u << (24 + player));
    }
    CHECK(nn_gsv_IVi2Session_CloseDisplay(s, VI2_DEFAULT_DISPLAY) == 0);
    CHECK(nn_gsv_IVi2Session_GetNotifications(s, out) == 0 && rd32(0x6000) == 1u << 31);
    CHECK(nn_gsv_IVi2Session_OpenDisplay(s, VI2_DEFAULT_DISPLAY, VI2_NOTIFICATIONS) == RC(1, 116));
    CHECK(nn_gsv_IVi2Session_OpenDisplay(s, VI2_DEFAULT_DISPLAY, 9) == 0);
    CHECK(nn_gsv_IVi2Session_OpenDisplay(s, VI2_DEFAULT_DISPLAY, 9) == 0);
    CHECK(nn_gsv_IVi2Session_OpenEndpoint(s, VI2_DEFAULT_DISPLAY, VI2_DEFAULT_DISPLAY, 0x2000) == RC(1, 121));
    CHECK(nn_gsv_IVi2Session_BindClientDisplay(s, 0x2000, VI2_DEFAULT_DISPLAY) == 0);
    CHECK(nn_gsv_IVi2Session_BindClientDisplay(s, 0x2000, VI2_DEFAULT_DISPLAY) == RC(1, 125));
    CHECK(nn_gsv_IVi2Session_OpenEndpoint(s, VI2_DEFAULT_DISPLAY, VI2_DEFAULT_DISPLAY, 0x2000) == 0);
    CHECK(nn_gsv_IVi2Session_OpenEndpoint(s, VI2_DEFAULT_DISPLAY, VI2_DEFAULT_DISPLAY, 0x2000) == RC(1, 125));
    note->signaled = 0;
    CHECK(nn_gsv_IVi2Session_CloseDisplay(s, VI2_DEFAULT_DISPLAY) == 0 && !note->signaled);
    CHECK(nn_gsv_IVi2Session_CloseDisplay(s, VI2_DEFAULT_DISPLAY) == 0 && note->signaled);
    CHECK(nn_gsv_IVi2Session_CloseDisplay(s, VI2_DEFAULT_DISPLAY) == RC(1, 121));
    CHECK(nn_gsv_IVi2Session_GetNotifications(s, out) == 0 && rd32(0x6000) == 1u << 9);
    CHECK(nn_gsv_IVi2Session_UnregisterClientState(s, 0x2000) == 0 && !d->endpoint);

    /* request groups: a vsync monitor's event follows the display's refresh; another kind's is signaled once, to
       be read; unregistering signals it too */
    CHECK(nn_gsv_IVi2Session_CreateRequestGroupEvent(s, VI2_GROUPS, &g0) == RC(114, 4));
    CHECK(nn_gsv_IVi2Session_RegisterVsyncMonitor(s, VI2_DEFAULT_DISPLAY, 0) == VI_NOT_FOUND);
    CHECK(nn_gsv_IVi2Session_CreateRequestGroupEvent(s, 0, &g0) == 0 && g0);
    CHECK(nn_gsv_IVi2Session_CreateRequestGroupEvent(s, 0, &g1) == RC(114, 5));
    CHECK(nn_gsv_IVi2Session_CreateRequestGroupEvent(s, 1, &g1) == 0 && g1);
    /* a group whose handle could not be made stays free: the caller can make it again */
    u32 g2 = 0;
    unsigned kobjs = test_kobjs;
    test_handles_full = 1;
    CHECK(nn_gsv_IVi2Session_CreateRequestGroupEvent(s, 2, &g2) == RC(1, 105) && !g2 && !d->group_events[2]);
    CHECK(test_kobjs == kobjs);
    test_handles_full = 0;
    CHECK(nn_gsv_IVi2Session_CreateRequestGroupEvent(s, 2, &g2) == 0 && g2 && d->group_events[2]);
    KEvent* e0 = kev(d->group_events[0]);
    KEvent* e1 = kev(d->group_events[1]);
    CHECK(nn_gsv_IVi2Session_RegisterVsyncMonitor(s, VI2_DEFAULT_DISPLAY, 0) == 0 && !e0->signaled);
    CHECK(nn_gsv_IVi2Session_RegisterVsyncMonitor(s, VI2_DEFAULT_DISPLAY, 0) == RC(114, 5));
    CHECK(nn_gsv_IVi2Session_RegisterHdrStatusMonitor(s, VI2_DEFAULT_DISPLAY, 1) == 0 && e1->signaled);
    e1->signaled = 0;
    vi_vsync_tick();
    CHECK(e0->signaled && !e1->signaled);
    e0->signaled = 0;
    CHECK(nn_gsv_IVi2Session_UnregisterVsyncMonitor(s, VI2_DEFAULT_DISPLAY, 1) == 0 && !e0->signaled);
    CHECK(nn_gsv_IVi2Session_UnregisterVsyncMonitor(s, VI2_DEFAULT_DISPLAY, 0) == 0 && e0->signaled);
    e0->signaled = 0;
    vi_vsync_tick();
    CHECK(!e0->signaled);

    /* another handle to a group's event, for a caller that holds that event */
    CHECK(nn_gsv_IVi2Session_DuplicateRequestGroupEvent(s, 0, g1, &dup) == RC(1, 114) && !dup);
    CHECK(nn_gsv_IVi2Session_DuplicateRequestGroupEvent(s, 3, g0, &dup) == VI_NOT_FOUND && !dup);
    CHECK(nn_gsv_IVi2Session_DuplicateRequestGroupEvent(s, 0, g0, &dup) == 0 && test_handles[dup - 0x100] == &e0->o);

    /* a save state's sessions: what the handlers let in, and nothing past it */
    static HleVi bad;
    CHECK(vi2_state_valid(&g_hle.vi));
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x3000, 7, 8) == 0);
    CHECK(nn_gsv_IVi2Session_BindClientDisplay(s, 0x3000, VI2_DEFAULT_DISPLAY) == RC(1, 121)); /* the display closed */
    u32 i = (u32)(d - g_hle.vi.vi2), c = 0;
    while (d->clients[c].token != 0x3000) c++;
    CHECK(vi2_state_valid(&g_hle.vi));
    bad = g_hle.vi, bad.vi2[i].display_notification[0] = VI2_NOTIFICATIONS;
    CHECK(!vi2_state_valid(&bad));
    bad = g_hle.vi, bad.vi2[i].clients[c].retire_bit = 40;
    CHECK(!vi2_state_valid(&bad));
    bad = g_hle.vi, bad.vi2[i].clients[c].retire_bit = 6;
    CHECK(!vi2_state_valid(&bad)); /* client_slots no longer matches */
    bad = g_hle.vi, bad.vi2[i].clients[c].token = 0x1234, bad.vi2[i].endpoint = 0x1234;
    CHECK(!vi2_state_valid(&bad)); /* an endpoint for a client not bound */
    bad = g_hle.vi, bad.vi2[i].monitor_kind[1] = VI2_MONITOR_KINDS;
    CHECK(!vi2_state_valid(&bad));
    bad = g_hle.vi, bad.vi2_live = 0;
    CHECK(!vi2_state_valid(&bad));

    /* the session's end lets go of its events, and the refresh no longer visits it; the guest's handles keep them */
    CHECK(e0->o.refs == 3 && e1->o.refs == 2 && note->o.refs == 2);
    CHECK(nn_gsv_IVi2Session_RegisterVsyncMonitor(s, VI2_DEFAULT_DISPLAY, 0) == 0);
    svcobj_unref(s);
    CHECK(e0->o.refs == 2 && e1->o.refs == 1 && note->o.refs == 1);
    CHECK(!g_hle.vi.vi2_live && vi2_state_valid(&g_hle.vi));
    e0->signaled = 0;
    vi_vsync_tick();
    CHECK(!e0->signaled);
}

/* the Switch 2 display's state and presents: a frame is shown once its fence is reached, and its slot stays the
   display's until the next frame replaces it, which frees it and tells the guest */
static void test_vi2_present(void) {
    SvcObj* s = vi2_session_new();
    Vi2Session* d = s->data;
    u32 event = 0;
    u64 v0 = 9, v1 = 9, v2 = 9, free_slots = 0;
    u16 v16 = 9;
    nn_gsv_Vi2BufferInfo info = {0};
    nn_gsv_NativeSyncHandle fence = {~5u, 0, 10}, release = {0};
    info.extent = 0x02CF04FF, info.layout = 3;
    CHECK(nn_gsv_IVi2Session_GetNotificationEvent(s, 0, &event) == 0);
    KEvent* note = kev(d->event);
    /* the local display's queries: SDR at one refresh rate and no variable one */
    CHECK(nn_gsv_IVi2Session_QueryHdrStatus(s, VI2_DEFAULT_DISPLAY, &v0, &v16) == VI_NOT_FOUND); /* not open */
    CHECK(nn_gsv_IVi2Session_OpenDisplay(s, VI2_DEFAULT_DISPLAY, 9) == 0);
    CHECK(nn_gsv_IVi2Session_QueryHdrStatus(s, VI2_DEFAULT_DISPLAY, &v0, &v16) == 0 && v0 == 1 && v16 == 0);
    CHECK(nn_gsv_IVi2Session_QueryHdrPaperWhite(s, 0, &v16) == RC(114, 4));
    CHECK(nn_gsv_IVi2Session_QueryModeCapability(s, VI2_DEFAULT_DISPLAY, &v0, &v1, &v2) == 0 && v0 == 1 && v1 == 1 &&
          v2 == 0);
    /* a client on the display, which presents only after it chose the SDR encoding */
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x5000, 10, 11) == 0);
    CHECK(nn_gsv_IVi2Session_BindClientDisplay(s, 0x5000, VI2_DEFAULT_DISPLAY) == 0);
    CHECK(nn_gsv_IVi2Session_OpenEndpoint(s, VI2_DEFAULT_DISPLAY, VI2_DEFAULT_DISPLAY, 0x5000) == 0);
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 0, 77, &info, &fence, &free_slots, &release) ==
          RC(1, 33));
    CHECK(nn_gsv_IVi2Session_SetClientOutputEncoding(s, 0x6000, 0x14) == RC(1, 121));
    CHECK(nn_gsv_IVi2Session_SetClientOutputEncoding(s, 0x5000, 0x15) == RC(1, 33));
    CHECK(nn_gsv_IVi2Session_SetClientOutputEncoding(s, 0x5000, 0x50) == RC(1, 33));
    CHECK(nn_gsv_IVi2Session_SetClientOutputEncoding(s, 0x5000, 0x5) == 0);
    CHECK(nn_gsv_IVi2Session_SetClientOutputEncoding(s, 0x5000, 0x14) == 0);
    /* the first frame: drawn after syncpoint 5 reaches 10, shown from its image, and its slot the display's */
    shown_n = 0, paced = 0;
    note->signaled = 0;
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 0, 77, &info, &fence, &free_slots, &release) == 0);
    CHECK(free_slots == 0x7FFE && fence_n == 1 && fence_id == 5 && paced == 1);
    CHECK(shown_n == 1 && shown_nvmap == 9 && shown_w == 1280 && shown_h == 720 && shown_pitch == 20 * 64);
    CHECK(shown_layout == 3 && shown_bhl == 4 && shown_fmt == 1 && !note->signaled);
    CHECK(nn_gsv_IVi2Session_GetClientAvailability(s, 0x5000, &free_slots) == 0 && free_slots == 0x7FFE);
    /* the second frame frees the first one's slot, and the availability bit tells the guest */
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 1, 77, &info, &fence, &free_slots, &release) == 0);
    CHECK(shown_n == 2 && note->signaled);
    CHECK(nn_gsv_IVi2Session_GetClientAvailability(s, 0x5000, &free_slots) == 0 && free_slots == 0x7FFD);
    /* what the presenter can't show, a slot not the client's, and a fence no syncpoint has */
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 1, 77, &info, &fence, &free_slots, &release) ==
          RC(1, 125));
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 15, 77, &info, &fence, &free_slots, &release) ==
          RC(1, 116));
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 0, 78, &info, &fence, &free_slots, &release) ==
          RC(1, 121));
    info.layout = 1;
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 0, 77, &info, &fence, &free_slots, &release) ==
          RC(1, 33));
    info.layout = 3, info.extent = 0x02D004FF; /* a row more than the image */
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 0, 77, &info, &fence, &free_slots, &release) ==
          RC(1, 33));
    info.extent = 0x02CF04FF;
    fence.inverted_id = ~(u32)SYNCPT_IDS;
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 0, 77, &info, &fence, &free_slots, &release) ==
          RC(1, 116));
    fence.inverted_id = ~5u;
    CHECK(nn_gsv_IVi2Session_GetClientAvailability(s, 0x5000, &free_slots) == 0 && free_slots == 0x7FFD);
    /* a frame cancelled before its fence is reached is never shown, and its slot is free at once */
    hold = 1;
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 2, 77, &info, &fence, &free_slots, &release) == 0);
    CHECK(free_slots == 0x7FF9);
    CHECK(nn_gsv_IVi2Session_CancelClientBuffer(s, 0x5000, 2, &info, &free_slots, &release) == 0);
    CHECK(free_slots == 0x7FFD);
    hold = 0;
    held_fn(held_arg);
    CHECK(shown_n == 2);
    CHECK(nn_gsv_IVi2Session_GetClientAvailability(s, 0x5000, &free_slots) == 0 && free_slots == 0x7FFD);
    /* a save state's client: no slot both free and the display's */
    static HleVi bad;
    u32 i = (u32)(d - g_hle.vi.vi2), c = 0;
    while (d->clients[c].token != 0x5000) c++;
    CHECK(vi2_state_valid(&g_hle.vi));
    bad = g_hle.vi, bad.vi2[i].clients[c].free_slots |= 2;
    CHECK(!vi2_state_valid(&bad));
    bad = g_hle.vi, bad.vi2[i].clients[c].shown = VI2_BUFFER_SLOTS + 1;
    CHECK(!vi2_state_valid(&bad));
    /* off the display, a frame is not shown: it and the one the display held come free at once */
    CHECK(nn_gsv_IVi2Session_CloseEndpoint(s, VI2_DEFAULT_DISPLAY, VI2_DEFAULT_DISPLAY) == 0);
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 3, 77, &info, &fence, &free_slots, &release) == 0);
    CHECK(shown_n == 2);
    CHECK(nn_gsv_IVi2Session_GetClientAvailability(s, 0x5000, &free_slots) == 0 && free_slots == 0x7FFF);
    /* a present still on its way when the session ends changes nothing, nor does a key no handler makes */
    hold = 1;
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x5000, 4, 77, &info, &fence, &free_slots, &release) == 0);
    hold = 0;
    CHECK(nn_gsv_IVi2Session_UnregisterClientState(s, 0x5000) == 0);
    CHECK(nn_gsv_IVi2Session_CloseDisplay(s, VI2_DEFAULT_DISPLAY) == 0);
    h_close(event);
    svcobj_unref(s);
    unsigned kobjs = test_kobjs;
    held_fn(held_arg);
    vi2_present((void*)vi2_key(VI2_SESSIONS, 0, 0, 1));
    vi2_present((void*)vi2_key(0, 0, VI2_BUFFER_SLOTS, 1));
    vi2_present((void*)vi2_key(0, 0, 0, 0));
    CHECK(shown_n == 2 && test_kobjs == kobjs);
}

static void test_vi2_present_event_lifetime(void) {
    unsigned objects = test_objects, kobjs = test_kobjs;
    SvcObj* s = vi2_session_new();
    u32 event = 0;
    CHECK(nn_gsv_IVi2Session_GetNotificationEvent(s, VI2_LAST_NOTIFICATION, &event) == 0);
    CHECK(nn_gsv_IVi2Session_RegisterClientState(s, 0x6000, 3, 4) == 0);
    CHECK(nn_gsv_IVi2Session_OpenDisplay(s, VI2_DEFAULT_DISPLAY, 5) == 0);
    CHECK(nn_gsv_IVi2Session_BindClientDisplay(s, 0x6000, VI2_DEFAULT_DISPLAY) == 0);
    CHECK(nn_gsv_IVi2Session_OpenEndpoint(s, VI2_DEFAULT_DISPLAY, VI2_DEFAULT_DISPLAY, 0x6000) == 0);
    CHECK(nn_gsv_IVi2Session_SetClientOutputEncoding(s, 0x6000, 0x14) == 0);
    nn_gsv_Vi2BufferInfo info = {.extent = 1279u | 719u << 16, .layout = 3};
    nn_gsv_NativeSyncHandle fence = {0}, release = {0};
    u64 free_slots = 0;
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x6000, 0, 77, &info, &fence, &free_slots, &release) == 0);
    hold = 1;
    CHECK(nn_gsv_IVi2Session_PresentClientBuffer(s, 0x6000, 1, 77, &info, &fence, &free_slots, &release) == 0);
    hold = 0;
    h_close(event); /* only the session owns the event until the callback retains it */
    close_on_unlock = s;
    unlocks_until_close = 2;
    held_fn(held_arg); /* the second unlock destroys the session just before signaling the retained event */
    CHECK(!close_on_unlock && !unlocks_until_close);
    CHECK(test_objects == objects && test_kobjs == kobjs);
}


int main(void) {
    g_settings.tv_mode = 0; /* handheld, as the test was written for */
    hle_init();
    u8 gbuf[VI_GBUF], req[4 + 4 + 8 + VI_GBUF], q[4 + 8 + VI_QBI];
    u32 v, n;
    i64 size = 0;
    u64 layer = 0;
    nn_vi_DisplayName name;
    IpcBuf window = {0x1000, 0x100, 0};
    g_mem = calloc(1, 0x10000);
    svc_vi_init();

    /* the native window: a parcel with one flattened binder, id 1 */
    memset(&name, 0, sizeof name);
    memcpy(name.v, "Default", 8);
    CHECK(nn_visrv_sf_IApplicationDisplayService_OpenLayer(NULL, &name, 1, 0x51, window, &size) == 0);
    CHECK(size == 0x3c && rd32(0x1000) == 0x28 && rd32(0x1004) == 0x10 && rd32(0x1008) == 4 && rd32(0x100c) == 0x38);
    CHECK(rd32(0x1010) == 2 && rd32(0x1014) == 1 && rd32(0x1018) == 1 && !memcmp(g_mem + 0x1028, "dispdrv", 8));
    memcpy(name.v, "Nowhere", 8);
    CHECK(nn_visrv_sf_IApplicationDisplayService_OpenLayer(NULL, &name, 1, 0x51, window, &size) == RC(114, 1));
    CHECK(nn_visrv_sf_IApplicationDisplayService_CreateStrayLayer(NULL, 0, 0, window, &layer, &size) == 0);
    CHECK(layer >= VI_STRAY_ID && rd32(0x1018) == 2);

    /* SetPreallocatedBuffer(slot 0): present, its flattened size and fd count, the NvGraphicBuffer */
    memset(gbuf, 0, sizeof gbuf);
    snx_st_le32(gbuf + 0x04, 1280);
    snx_st_le32(gbuf + 0x08, 720);
    snx_st_le32(gbuf + 0x10, 1);
    snx_st_le32(gbuf + 0x2c, 7);
    snx_st_le32(gbuf + 0x6c, 3);
    snx_st_le32(gbuf + 0x70, 5120);
    snx_st_le32(gbuf + 0x80, 4);
    memset(req, 0, sizeof req);
    snx_st_le32(req + 4, 1);
    snx_st_le32(req + 8, VI_GBUF);
    memcpy(req + 16, gbuf, VI_GBUF);
    CHECK(transact(BQ_SET_PREALLOCATED_BUFFER, req, sizeof req) == 4 && rd32(0x4010) == 0);

    /* Connect: QueueBufferOutput (the buffers' size), then the status */
    CHECK(transact(BQ_CONNECT, "\0\0\0\0\2\0\0\0\0\0\0\0", 12) == 20);
    CHECK(rd32(0x4010) == 1280 && rd32(0x4014) == 720 && rd32(0x4020) == 0);

    /* MinUndequeuedBuffers: HOS never uses the async buffer path, so Eden's GetMinUndequeuedBufferCountLocked(false)
       is 0. Answering 1 makes NVN ask for 3 buffers where its window expects 2 */
    CHECK(transact(BQ_QUERY, "\x03\0\0\0", 4) == 8 && rd32(0x4010) == 0 && rd32(0x4014) == 0);

    /* The queue's buffer count: 2 by default, SetBufferCount changes it, 0 puts the default back. The NVN window
       initializes as many textures as this query answers, so it must be the count requested, not the 64 slots */
    CHECK(transact(BQ_QUERY, "\x0c\0\0\0", 4) == 8 && rd32(0x4010) == 2 && rd32(0x4014) == 0);
    CHECK(transact(BQ_SET_BUFFER_COUNT, "\x03\0\0\0", 4) == 4 && rd32(0x4010) == 0);
    CHECK(transact(BQ_QUERY, "\x0c\0\0\0", 4) == 8 && rd32(0x4010) == 3 && rd32(0x4014) == 0);
    CHECK(transact(BQ_SET_BUFFER_COUNT, "\0\0\0\0", 4) == 4 && rd32(0x4010) == 0);
    CHECK(transact(BQ_QUERY, "\x0c\0\0\0", 4) == 8 && rd32(0x4010) == 2 && rd32(0x4014) == 0);
    CHECK(transact(BQ_SET_BUFFER_COUNT, "\x41\0\0\0", 4) == 4 && (i32)rd32(0x4010) == BQ_BAD_VALUE);

    /* DequeueBuffer: the slot, a flattened Fence with none in it, the status */
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\5\0\0\xd0\2\0\0\1\0\0\0\0\0\0\0", 20) == 56);
    CHECK(rd32(0x4010) == 0 && rd32(0x4014) == 1 && rd32(0x4018) == VI_FENCE && rd32(0x401c) == 0);
    CHECK(rd32(0x4020) == 0 && rd32(0x4024) == ~0u && rd32(0x4044) == 0);

    /* RequestBuffer: the NvGraphicBuffer as it was preallocated */
    n = transact(BQ_REQUEST_BUFFER, "\0\0\0\0", 4);
    CHECK(n == 4 + 8 + VI_GBUF + 4 && rd32(0x4010) == 1 && rd32(0x4014) == VI_GBUF);
    CHECK(!memcmp(g_mem + 0x401c, gbuf, VI_GBUF) && rd32(0x4010 + n - 4) == 0);

    /* QueueBuffer: swap interval 1 and one fence (syncpoint 5 at 10); shown, and free again */
    memset(q, 0, sizeof q);
    snx_st_le32(q + 4, VI_QBI);
    snx_st_le32(q + 12 + 44, 1);
    snx_st_le32(q + 12 + 48, 1);
    snx_st_le32(q + 12 + 52, 5);
    snx_st_le32(q + 12 + 56, 10);
    for (v = 1; v < 4; v++) snx_st_le32(q + 12 + 52 + 8 * v, ~0u);
    CHECK(transact(BQ_QUEUE_BUFFER, q, sizeof q) == 20);
    CHECK(rd32(0x4010) == 1280 && rd32(0x4014) == 720 && rd32(0x401c) == 1 && rd32(0x4020) == 0);
    CHECK(fence_n == 1 && fence_id == 5 && shown_n == 1 && shown_nvmap == 7 && shown_w == 1280 && shown_h == 720);
    CHECK(shown_pitch == 5120 && shown_layout == 3 && shown_bhl == 4 && shown_fmt == 1);
    CHECK(g_hle.vi.layer[0].slot[0].state == SLOT_FREE);
    paced = 0;
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56);
    CHECK(rd32(0x4010) == 0 && paced == 1);

    /* a slot out of range, and a binder that doesn't exist */
    CHECK(transact(BQ_REQUEST_BUFFER, "\x40\0\0\0", 4) == 4 && (i32)rd32(0x4010) == BQ_BAD_VALUE);
    CHECK(nns_hosbinder_IHOSBinderDriver_TransactParcel(NULL, 9, 1, parcel("", 0), window, 0) == RC(114, 7));

    /* the stray layer's queue is its own: its frame isn't shown and doesn't wait for the display, and its slot comes
       free once the fences are reached */
    binder = 2;
    CHECK(transact(BQ_SET_PREALLOCATED_BUFFER, req, sizeof req) == 4 && rd32(0x4010) == 0);
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56);
    CHECK(rd32(0x4010) == 0 && rd32(0x4044) == 0);
    paced = 0;
    v = shown_n;
    CHECK(transact(BQ_QUEUE_BUFFER, q, sizeof q) == 20);
    CHECK(shown_n == v && g_hle.vi.layer[1].slot[0].state == SLOT_FREE);
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56 && paced == 0);

    /* DestroyStrayLayer: its binder is gone */
    CHECK(nn_visrv_sf_IApplicationDisplayService_DestroyStrayLayer(NULL, layer) == 0);
    CHECK(nns_hosbinder_IHOSBinderDriver_TransactParcel(NULL, 2, 1, parcel("", 0), window, 0) == RC(114, 7));

    /* CloseLayer with a frame still waiting for its fences: dropped, and the layer opened next takes its place with a
       queue of its own, empty, and is shown */
    binder = 1; /* its buffer was dequeued above */
    hold = 1;
    CHECK(transact(BQ_QUEUE_BUFFER, q, sizeof q) == 20 && held_fn);
    hold = 0;
    CHECK(nn_visrv_sf_IApplicationDisplayService_CloseLayer(NULL, 1) == 0);
    CHECK(nns_hosbinder_IHOSBinderDriver_TransactParcel(NULL, 1, 1, parcel("", 0), window, 0) == RC(114, 7));
    memcpy(name.v, "Default", 8);
    CHECK(nn_visrv_sf_IApplicationDisplayService_OpenLayer(NULL, &name, 2, 0x51, window, &size) == 0);
    CHECK(rd32(0x1018) == 1);
    v = shown_n;
    held_fn(held_arg);
    CHECK(shown_n == v && g_hle.vi.layer[0].slot[0].state == SLOT_FREE && !g_hle.vi.layer[0].slot[0].has_buffer);
    CHECK(transact(BQ_REQUEST_BUFFER, "\0\0\0\0", 4) == 8 && rd32(0x4010) == 0);
    CHECK(transact(BQ_SET_PREALLOCATED_BUFFER, req, sizeof req) == 4 && rd32(0x4010) == 0);
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56);
    CHECK(transact(BQ_QUEUE_BUFFER, q, sizeof q) == 20 && shown_n == v + 1);

    /* the swap interval queued is the next dequeue's pace: 0 to 4, a negative one 1, past 4 capped */
    snx_st_le32(q + 12 + 44, 9);
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56);
    CHECK(transact(BQ_QUEUE_BUFFER, q, sizeof q) == 20 && g_hle.vi.layer[0].interval == 4);
    snx_st_le32(q + 12 + 44, ~0u);
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56);
    CHECK(transact(BQ_QUEUE_BUFFER, q, sizeof q) == 20 && g_hle.vi.layer[0].interval == 1);
    snx_st_le32(q + 12 + 44, 0);
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56);
    CHECK(transact(BQ_QUEUE_BUFFER, q, sizeof q) == 20 && g_hle.vi.layer[0].interval == 0);
    /* a QueueBufferInput cut short: the whole buffer at interval 1 */
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56);
    CHECK(transact(BQ_QUEUE_BUFFER, q, 12 + 40) == 20 && g_hle.vi.layer[0].interval == 1);

    /* a transaction without a parcel header fails; a graphic buffer short of its first plane clears the slot */
    {
        IpcBuf tiny = {0x2000, 8, 0}, reply = {0x4000, 0x400, 0};
        u8 short_req[16 + 0x40];
        CHECK(nns_hosbinder_IHOSBinderDriver_TransactParcel(NULL, 1, BQ_QUERY, tiny, reply, 0) == RC(114, 1));
        memset(short_req, 0, sizeof short_req);
        snx_st_le32(short_req + 4, 1);
        snx_st_le32(short_req + 8, 0x40);
        memcpy(short_req + 16, gbuf, 0x40);
        CHECK(transact(BQ_SET_PREALLOCATED_BUFFER, short_req, sizeof short_req) == 4 && rd32(0x4010) == 0);
        CHECK(!g_hle.vi.layer[0].slot[0].has_buffer);
    }

    /* a native window buffer too small for the parcel: its size still, nothing written */
    {
        IpcBuf small = {0x1000, 0x20, 0};
        memset(g_mem + 0x1000, 0xcc, 0x100);
        CHECK(nn_visrv_sf_IApplicationDisplayService_CreateStrayLayer(NULL, 0, 0, small, &layer, &size) == 0);
        CHECK(size == 0x3c && g_mem[0x1000] == 0xcc);
    }

    /* IManagerDisplayService 6002, SetLayerVisibility: Minecraft calls it at boot. The runtime has one layer and
       always presents it, so the command succeeds and changes nothing */
    {
        SvcObj* manager = nn_visrv_sf_IManagerDisplayService_new();
        IpcCtx c;
        u8 raw[16];
        memset(raw, 0, sizeof raw);
        memset(&c, 0, sizeof c);
        c.cmd = 6002;
        c.raw = raw;
        c.raw_size = sizeof raw;
        CHECK(manager->dispatch(manager, &c) == 0 && c.out_size == 0);
        svcobj_unref(manager);
    }

    test_vi2();
    test_vi2_present();
    test_vi2_present_event_lifetime();
    printf("vi_service: %u checks passed\n", test_checks);
    (void)test_objects, (void)test_kobjs;
    free(g_mem);
    return 0;
}
