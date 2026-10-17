/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* vi.c's binder: the native window OpenLayer gives, and a buffer queue's preallocate, connect, dequeue, request and
   queue transactions as byte-exact parcels, with the GPU thread and the renderer stood in for */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>

#include "nx/hle/services/vi.c"

u8* g_mem;
int g_verbose;
int g_opt_docked;
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
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*), size_t size) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    if (size) obj->data = calloc(1, size);
    test_objects++;
    return obj;
}
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    free(obj->data);
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
    e->o.name = name;
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

/* the GPU thread runs the present at once; the renderer keeps what it was asked to show */
static u32 shown_n, shown_nvmap, shown_w, shown_h, shown_pitch, shown_layout, shown_bhl, shown_fmt, fence_n, fence_id;
static u32 paced;
void nv_gpu_call_after(u32 n, u32* id, const u32* val, void (*fn)(void*), void* arg) {
    fence_n = n;
    fence_id = n ? id[0] : 0;
    CHECK(!n || val[0] == 10);
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

static u32 rd32(u64 a) {
    u32 v;
    memcpy(&v, g_mem + a, 4);
    return v;
}
/* a request parcel at 0x2000: the header, the interface token, then the payload */
static IpcBuf parcel(const void* payload, u32 n) {
    static const char token[] = "android.gui.IGraphicBufferProducer";
    u32 len = (u32)strlen(token), i, hdr[4], data = 8 + ((len + 1) * 2 + 3) / 4 * 4 + n;
    u8* p = g_mem + 0x2000;
    IpcBuf b;
    memset(p, 0, 0x1000);
    hdr[0] = data, hdr[1] = 16, hdr[2] = 0, hdr[3] = 16 + data;
    memcpy(p, hdr, 16);
    memcpy(p + 16, &(u32){0x100}, 4);
    memcpy(p + 20, &len, 4);
    for (i = 0; i < len; i++) p[24 + 2 * i] = (u8)token[i];
    memcpy(p + 16 + data - n, payload, n);
    b.addr = 0x2000, b.size = 16 + data, b.flags = 0;
    return b;
}
static u32 transact(u32 code, const void* payload, u32 n) {
    IpcBuf reply = {0x4000, 0x400, 0};
    memset(g_mem + 0x4000, 0xcc, 0x400);
    CHECK(nns_hosbinder_IHOSBinderDriver_TransactParcel(NULL, 1, code, parcel(payload, n), reply, 0) == 0);
    CHECK(rd32(0x4004) == 16 && rd32(0x4008) == 0 && rd32(0x400c) == 16 + rd32(0x4000));
    return rd32(0x4000); /* the data's size; it starts at 0x4010 and ends with the status */
}

int main(void) {
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
    memcpy(gbuf + 0x04, &(u32){1280}, 4);
    memcpy(gbuf + 0x08, &(u32){720}, 4);
    memcpy(gbuf + 0x10, &(u32){1}, 4);
    memcpy(gbuf + 0x2c, &(u32){7}, 4);
    memcpy(gbuf + 0x6c, &(u32){3}, 4);
    memcpy(gbuf + 0x70, &(u32){5120}, 4);
    memcpy(gbuf + 0x80, &(u32){4}, 4);
    memset(req, 0, sizeof req);
    memcpy(req + 4, &(u32){1}, 4);
    memcpy(req + 8, &(u32){VI_GBUF}, 4);
    memcpy(req + 16, gbuf, VI_GBUF);
    CHECK(transact(BQ_SET_PREALLOCATED_BUFFER, req, sizeof req) == 4 && rd32(0x4010) == 0);

    /* Connect: QueueBufferOutput (the buffers' size), then the status */
    CHECK(transact(BQ_CONNECT, "\0\0\0\0\2\0\0\0\0\0\0\0", 12) == 20);
    CHECK(rd32(0x4010) == 1280 && rd32(0x4014) == 720 && rd32(0x4020) == 0);

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
    memcpy(q + 4, &(u32){VI_QBI}, 4);
    memcpy(q + 12 + 44, &(u32){1}, 4);
    memcpy(q + 12 + 48, &(u32){1}, 4);
    memcpy(q + 12 + 52, &(u32){5}, 4);
    memcpy(q + 12 + 56, &(u32){10}, 4);
    for (v = 1; v < 4; v++) memcpy(q + 12 + 52 + 8 * v, &(u32){~0u}, 4);
    CHECK(transact(BQ_QUEUE_BUFFER, q, sizeof q) == 20);
    CHECK(rd32(0x4010) == 1280 && rd32(0x4014) == 720 && rd32(0x401c) == 1 && rd32(0x4020) == 0);
    CHECK(fence_n == 1 && fence_id == 5 && shown_n == 1 && shown_nvmap == 7 && shown_w == 1280 && shown_h == 720);
    CHECK(shown_pitch == 5120 && shown_layout == 3 && shown_bhl == 4 && shown_fmt == 1);
    CHECK(vi.layer[0].slot[0].state == SLOT_FREE);
    paced = 0;
    CHECK(transact(BQ_DEQUEUE_BUFFER, "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0", 20) == 56);
    CHECK(rd32(0x4010) == 0 && paced == 1);

    /* a slot out of range, and a binder that doesn't exist */
    CHECK(transact(BQ_REQUEST_BUFFER, "\x40\0\0\0", 4) == 4 && (i32)rd32(0x4010) == BQ_BAD_VALUE);
    CHECK(nns_hosbinder_IHOSBinderDriver_TransactParcel(NULL, 9, 1, parcel("", 0), window, 0) == RC(114, 7));

    printf("vi_service: %u checks passed\n", test_checks);
    (void)test_objects, (void)test_kobjs;
    free(g_mem);
    return 0;
}
