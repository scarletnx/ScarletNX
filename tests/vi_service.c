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
void pm_unlock(PMutex* m) { (void)m; }
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    (void)c, (void)m, (void)timeout_ns;
    test_fail("a dequeue that would wait forever", __LINE__);
    return 1;
}
int kthread_wait(PCond* c, PMutex* m, i64 timeout_ns) { return pc_wait(c, m, timeout_ns); }
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
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    test_svcobj_data_free(obj);
    free(obj);
    test_objects--;
}
void out_obj(IpcCtx* c, SvcObj* obj) { c->out_objs[c->nout_objs++] = obj; }
void out_copy_h(IpcCtx* c, u32 handle) { c->out_copy[c->nout_copy++] = handle; }
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
void svc_register(const char* name, SvcCtor ctor) { (void)name, (void)ctor; }
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
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
u32 h_new(KObj* o) {
    kobj_ref(o);
    return test_next_handle++;
}
u32 ev_handle(KEvent* e) { return h_new(&e->o); }
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = 0;
    free(e);
    test_kobjs--;
    return 0;
}
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
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
                    u32 transform) {
    (void)offset, (void)crop, (void)transform;
    shown_n++;
    shown_nvmap = nvmap_id, shown_w = w, shown_h = h, shown_pitch = pitch;
    shown_layout = layout, shown_bhl = bhl, shown_fmt = fmt;
}
void present_pace(u32 interval) { paced = interval; }

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

    printf("vi_service: %u checks passed\n", test_checks);
    (void)test_objects, (void)test_kobjs;
    free(g_mem);
    return 0;
}
