/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvdrv's IPC side (nvdrv.c) through its generated dispatcher: Ioctl2's and Ioctl3's inline buffers reaching the
   device, the NvError of a bad fd, QueryEvent, TSG membership, preemption modes, and a Close during an ioctl.
   The GPU and host1x are stand-ins that record what nvdrv asks of them */
#include "nx/hle/services/nvdrv.h"
#include "nx/hle/kernel/ipc.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "nx/hle/services/nvdrv.c"
#include "nx/hle/hle.c"

/* a ctrl fd's 64 event slots: the table of ctrl_evt it took */
#define CTRL_EV(fd) (g_hle.nvdrv.ctrl_evt[g_hle.nvdrv.ctrl_ev[fd] - 1].slot)

u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */
void (*g_gpu_mapping_begin)(u64, u64, int);
void (*g_gpu_mapping_end)(void);
static unsigned test_checks, test_kobjs;
static u32 test_next_handle = 0x100;
static int test_handles_full;
/* A mapped page run for the worker's registration checks. Other addresses are inaccessible. */
static MemBlock test_gpu_memory;
void mem_query(u64 addr, MemBlock* out) {
    if (addr >= test_gpu_memory.addr && addr - test_gpu_memory.addr < test_gpu_memory.size)
        *out = test_gpu_memory;
    else
        *out = (MemBlock){.addr = addr, .size = 0x1000, .state = MS_INACCESSIBLE};
}

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "nvdrv_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

/* single-threaded: the locks only have to be balanced, which the counter checks */
static int test_locks;
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) {
    (void)m;
    test_locks++;
}
void pm_unlock(PMutex* m) {
    (void)m;
    CHECK(test_locks > 0);
    test_locks--;
}
void pc_init(PCond* c) { (void)c; }
void pc_broadcast(PCond* c) { (void)c; }
int pc_wait(PCond* c, PMutex* m, i64 timeout_ns) {
    (void)c, (void)m, (void)timeout_ns;
    test_fail("nothing here waits for a syncpoint", __LINE__);
    return 1;
}
int kthread_wait(PCond* c, PMutex* m, i64 timeout_ns) { return pc_wait(c, m, timeout_ns); }
u64 plat_ns(void) { return 0; }
u64 guest_ns(void) { return 0; }
KThread* kthread_cur(void) { return NULL; } /* no guest thread: requests aren't recorded on one */
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    exit(1);
}

SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    SvcObj* obj = calloc(1, sizeof *obj);
    CHECK(obj != NULL);
    obj->name = name;
    obj->dispatch = dispatch;
    obj->refs = 1;
    return obj;
}
#include "svcobj_data.h"
#include "kobj_ids.h" /* the kernel objects the stubs make, as ids */
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    if (obj->destroy) obj->destroy(obj);
    test_svcobj_data_free(obj);
    free(obj);
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
}
void out_move_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_move < 8);
    c->out_move[c->nout_move++] = handle;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
/* sf's InvalidCmifRequest, for a request that doesn't send what its command declares */
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(11, 420);
}
/* sf's InvalidNumInObjects, for a domain request with another number of objects */
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return RC(10, 235);
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    fprintf(stderr, "unexpected stub: %s command %u\n", o->name, c->cmd);
    test_fail("a command without a handler", __LINE__);
    return -1;
}
int ipc_unknown(SvcObj* obj, IpcCtx* c) { (void)obj; (void)c; return RC(10, 221); }
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}

/* kernel objects, counted; a handle takes a reference and names its object */
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    if (!o) return;
    CHECK(o->refs > 0);
    if (--o->refs) return;
    if (o->type == KO_WEVENT) kobj_unref(kobj_at(((KWEvent*)o)->r));
    if (o->type == KO_SESSION) svcobj_unref(((KSession*)o)->obj);
    free(o);
    test_kobjs--;
}
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.type = KO_EVENT;
    kobj_set_name(&e->o, name);
    test_kobjs++;
    return e;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
void kevent_clear(KEvent* e) { e->signaled = 0; }
static KObj* test_handles[64];
u32 h_new(KObj* o) {
    if (test_handles_full) return 0;
    CHECK(test_next_handle - 0x100 < 64);
    kobj_ref(o);
    test_handles[test_next_handle - 0x100] = o;
    return test_next_handle++;
}
static KObj* handle_object(u32 h) {
    CHECK(h >= 0x100 && h < test_next_handle);
    return test_handles[h - 0x100];
}
KObj* h_get_ref(u32 handle, int type) {
    if (handle < 0x100 || handle >= test_next_handle) return NULL;
    KObj* o = test_handles[handle - 0x100];
    if (!o || (type && o->type != type)) return NULL;
    kobj_ref(o);
    return o;
}
int h_close(u32 handle) {
    KObj* o = test_handles[handle - 0x100];
    test_handles[handle - 0x100] = NULL;
    kobj_unref(o);
    return 0;
}
u32 kevent_write_handle(KEvent* event) {
    KWEvent* w = calloc(1, sizeof *w);
    CHECK(w != NULL);
    w->o.type = KO_WEVENT;
    w->r = kobj_id(&event->o);
    kobj_ref(&event->o);
    test_kobjs++;
    return h_new(&w->o);
}
u32 session_new(SvcObj* obj) {
    KSession* s = calloc(1, sizeof *s);
    CHECK(s != NULL);
    s->o.type = KO_SESSION;
    s->o.refs = 1;
    s->obj = obj;
    ++test_kobjs;
    u32 handle = h_new(&s->o);
    kobj_unref(&s->o);
    return handle;
}

/* the GPU: submissions, calls, kicks and retired channels, recorded */
static struct {
    u64 entries[8];
    u32 n, channel, incs, wait_id, wait_val, expect, submits, closed_channel, calls, call_chans[4], ncall_chans, kicks;
} test_gpu;
void gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect, u64 tsg) {
    (void)tsg;
    CHECK(n <= 8);
    memcpy(test_gpu.entries, entries, n * sizeof *test_gpu.entries);
    snx_le64_array(test_gpu.entries, n); /* the entries as nvdrv found them in guest memory, little-endian */
    test_gpu.n = n;
    test_gpu.channel = channel;
    test_gpu.incs = incs;
    test_gpu.wait_id = wait_id;
    test_gpu.wait_val = wait_val;
    test_gpu.expect = expect;
    test_gpu.submits++;
}
void gpu_call(void (*fn)(void*), void* arg, const u32* chans, u32 nchans) {
    CHECK(nchans <= 4);
    test_gpu.calls++;
    memcpy(test_gpu.call_chans, chans, nchans * sizeof *chans);
    test_gpu.ncall_chans = nchans;
    fn(arg); /* the GPU thread has nothing queued before it */
}
void gpu_call_tsg(void (*fn)(void*), void* arg, u64 tsg) {
    (void)tsg;
    fn(arg);
}
void mme_tu104_tsg_close(u64 tsg) { (void)tsg; }
void gpu_channel_close(u32 channel) { test_gpu.closed_channel = channel; }
void gpu_release_retire(u32 channel) { (void)channel; }
void gpu_kick(void) { test_gpu.kicks++; }
void gpu_mem_written(u64 gva, u64 size) { (void)gva, (void)size; }

/* host1x: one channel per NVDEC or VIC fd. Its ioctl can run a hook in the middle, as another guest thread would */
struct H1Ch {
    int is_vic, closed;
    u32 ioctls;
};
static H1Ch test_channels[4];
static unsigned test_nchannels;
static void (*test_during_ioctl)(void);
H1Ch* host1x_open(int is_vic) {
    CHECK(test_nchannels < 4);
    H1Ch* ch = &test_channels[test_nchannels++];
    ch->is_vic = is_vic;
    return ch;
}
void host1x_close(H1Ch* t) {
    if (!t) return;
    CHECK(!t->closed);
    t->closed = 1;
}
u32 host1x_ioctl(H1Ch* t, u32 cmd, u8* d, u32 n) {
    CHECK(!t->closed);
    t->ioctls++;
    if (test_during_ioctl) {
        void (*hook)(void) = test_during_ioctl;
        test_during_ioctl = NULL;
        hook();
    }
    CHECK(!t->closed); /* the channel outlives a Close made during its ioctl */
    if (n >= 4) snx_st_le32(d, cmd);
    return 0;
}
void host1x_forget_handle(u32 h) { (void)h; }
/* nvdrv.c asks the kernel whether a range is locked before FREE reports it; the test has no memory layer, so no
   range is ever locked */
int svc_mem_range_locked(u64 addr, u64 size) {
    (void)addr, (void)size;
    return 0;
}

/* guest memory: the request's buffers */
enum { IN0 = 0x1000, IN1 = 0x2000, OUT0 = 0x3000, OUT1 = 0x4000, PATH = 0x5000 };
static SvcObj* nvdrv;
static IpcCtx ctx;
static u32 raw[4];
/* the descriptors each command declares, empty: an AutoSelect buffer goes in both its lists (A and X for an input,
   B and C for an output), and the test fills in the ones it uses */
static void request(u32 cmd, u32 a0, u32 a1) {
    memset(&ctx, 0, sizeof ctx);
    raw[0] = snx_le32(a0); /* the message's words are little-endian */
    raw[1] = snx_le32(a1);
    ctx.cmd = cmd;
    ctx.raw = (const u8*)raw;
    ctx.raw_size = sizeof raw;
    ctx.na = cmd == 0 || cmd == 1 || cmd == 12 ? 1 : cmd == 11 ? 2 : 0; /* Open's path, the ioctls' ins */
    ctx.nx = cmd == 1 || cmd == 12 ? 1 : cmd == 11 ? 2 : 0;
    ctx.nb = ctx.nc = cmd == 1 || cmd == 11 ? 1 : cmd == 12 ? 2 : 0; /* the ioctls' outs */
}
static IpcBuf buf(u64 addr, u64 size) {
    IpcBuf b = {addr, size, 0};
    return b;
}
static u32 reply(u32 off) {
    CHECK(off + 4 <= ctx.out_size);
    return snx_ld_le32(ctx.out + off);
}
/* Open (0): the path in A, the fd and the NvError back */
static u32 nv_open(const char* path, u32* error) {
    strcpy((char*)GP(PATH), path);
    request(0, 0, 0);
    ctx.a[0] = buf(PATH, strlen(path) + 1);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 8);
    *error = reply(4);
    return reply(0);
}
static u32 nv_close(u32 fd) {
    request(2, fd, 0);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4);
    return reply(0);
}
/* Ioctl (1) with its in and out buffers mapped */
static u32 nv_ioctl1(u32 fd, u32 cmd, u64 size) {
    request(1, fd, cmd);
    ctx.a[0] = buf(IN0, size);
    ctx.b[0] = buf(OUT0, size);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4);
    return reply(0);
}

static void test_tsg(void) {
    g_hle.kernel.hw = SNX_HW_SWITCH2;
    u32 error, a = nv_open("/dev/nvhost-tsg-gpu", &error);
    CHECK(!error);
    u32 b = nv_open("/dev/nvhost-tsg-gpu", &error);
    CHECK(!error);
    u32 gpu = nv_open("/dev/nvhost-gpu", &error);
    CHECK(!error);
    memset(GP(IN0), 0, 24);
    gw32(IN0, gpu);
    gw32(IN0 + 4, NV_TSG_SUBCONTEXTS);
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL_EX, 24) == NV_RESULT_BAD_PARAMETER);
    gw32(IN0 + 4, NV_TSG_SUBCONTEXTS - 1);
    gw32(IN0 + 8, 1);
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL_EX, 24) == NV_RESULT_BAD_PARAMETER);
    gw32(IN0 + 8, 0);
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL_EX, 4) == NV_RESULT_BAD_PARAMETER);
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL_EX, 24) == 0);
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL, 4) == NV_RESULT_BUSY);
    CHECK(nv_ioctl1(b, NV_TSG_BIND_CHANNEL, 4) == NV_RESULT_BUSY);
    CHECK(nv_ioctl1(b, NV_TSG_UNBIND_CHANNEL, 4) == NV_RESULT_BAD_PARAMETER);
    CHECK(nv_ioctl1(a, NV_TSG_UNBIND_CHANNEL, 4) == 0);
    CHECK(nv_ioctl1(b, NV_TSG_BIND_CHANNEL, 4) == 0);
    CHECK(nv_close(b) == 0);
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL, 4) == 0); /* group close released its channels */
    b = nv_open("/dev/nvhost-tsg-gpu", &error); /* occupy the group's freed fd before recycling the channel */
    CHECK(!error);
    CHECK(nv_close(gpu) == 0);
    gpu = nv_open("/dev/nvhost-gpu", &error);
    CHECK(!error && gr32(IN0) == gpu);
    CHECK(nv_ioctl1(a, NV_TSG_UNBIND_CHANNEL, 4) == NV_RESULT_BAD_PARAMETER); /* fd reuse is not membership */
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL, 4) == 0);
    CHECK(nv_ioctl1(a, NV_TSG_UNBIND_CHANNEL, 4) == 0);
    gw32(IN0, a);
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL, 4) == NV_RESULT_BAD_PARAMETER);
    gw32(IN0, NV_FDS);
    CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL, 4) == NV_RESULT_BAD_PARAMETER);
    gw32(IN0, 0);
    CHECK(nv_ioctl1(a, NV_TSG_SET_TIMESLICE, 4) == NV_RESULT_BAD_PARAMETER);
    gw32(IN0, 1000);
    CHECK(nv_ioctl1(a, NV_TSG_SET_TIMESLICE, 4) == 0);
    CHECK(nv_ioctl1(a, NV_IOCTL(NV_IOCTL_DIR_RW, 'T', 0x7F, 4), 4) == NV_RESULT_NOT_IMPLEMENTED);
    u32 channels[NV_TSG_CHANNELS + 1];
    for (u32 i = 0; i < NV_TSG_CHANNELS + 1; ++i) {
        channels[i] = nv_open("/dev/nvhost-gpu", &error);
        CHECK(!error);
        gw32(IN0, channels[i]);
        CHECK(nv_ioctl1(a, NV_TSG_BIND_CHANNEL, 4) ==
              (i < NV_TSG_CHANNELS ? NV_RESULT_SUCCESS : NV_RESULT_INSUFFICIENT_MEMORY));
    }
    for (u32 i = 0; i < NV_TSG_CHANNELS + 1; ++i) CHECK(nv_close(channels[i]) == 0);

    gw32(IN0, 1);
    gw32(IN0 + 4, 2);
    u32 preempt = NV_IOCTL(NV_IOCTL_DIR_RW, 'H', NVHOST_GPU_SET_PREEMPTION_MODES, 8);
    CHECK(nv_ioctl1(gpu, preempt, 8) == NV_RESULT_INVALID_STATE);
    memset(GP(IN0), 0, 0x20);
    CHECK(nv_ioctl1(gpu, NV_IOCTL(NV_IOCTL_DIR_RW, 'H', NVHOST_GPU_ALLOC_GPFIFO_EX2, 0x20), 0x20) == 0);
    gw32(IN0, 1);
    gw32(IN0 + 4, 2);
    CHECK(nv_ioctl1(gpu, preempt, 8) == 0); /* WFI graphics and CTA compute */
    gw32(IN0, 2);
    CHECK(nv_ioctl1(gpu, preempt, 8) == NV_RESULT_NOT_SUPPORTED);
    gw32(IN0, 0);
    gw32(IN0 + 4, 3);
    CHECK(nv_ioctl1(gpu, preempt, 8) == NV_RESULT_NOT_SUPPORTED);
    CHECK(nv_close(gpu) == 0 && nv_close(a) == 0 && nv_close(b) == 0);
    g_hle.kernel.hw = SNX_HW_SWITCH;
}

/* Ioctl2 (11): SUBMIT_GPFIFO_EX's header in A0 or X0, the GPFIFO entries inline in A1 or X1 */
#define SUBMIT_GPFIFO_EX 0xC018481Bu
static void submit_header(u32 num, u32 flags) {
    u8 h[0x18];
    memset(h, 0, sizeof h);
    snx_st_le32(h + 8, num);
    snx_st_le32(h + 12, flags);
    memcpy(GP(IN0), h, sizeof h);
}
static void test_ioctl2(u32 gpu) {
    const u64 entries[3] = {0x0000000100200000ull, 0x0000000100300000ull, 0x0000000400400000ull};
    for (u32 i = 0; i < 3; i++) gw64(IN1 + 8 * i, entries[i]);
    submit_header(3, 2); /* FenceIncrement: the host adds two increments after the entries */
    memset(GP(OUT0), 0xCC, 0x18);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.a[1] = buf(IN1, sizeof entries);
    ctx.b[0] = buf(OUT0, 0x18);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    CHECK(test_gpu.submits == 1 && test_gpu.n == 3 && !memcmp(test_gpu.entries, entries, sizeof entries));
    u32 sp = test_gpu.channel;
    CHECK(sp >= SYNCPT_FIRST && sp < SYNCPT_N && test_gpu.incs == 2 && test_gpu.expect == 2);
    /* the fence the submission promises comes back in the output */
    CHECK(gr32(OUT0 + 16) == sp && gr32(OUT0 + 20) == 2);

    /* the same through the pointer buffer: the header in X0, the entries in X1, the A list empty */
    submit_header(2, 2);
    memset(GP(OUT0), 0xCC, 0x18);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.x[0] = buf(IN0, 0x18);
    ctx.x[1] = buf(IN1 + 8, 16);
    ctx.c[0] = buf(OUT0, 0x18);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(test_gpu.submits == 2 && test_gpu.n == 2 && !memcmp(test_gpu.entries, entries + 1, 16));
    CHECK(test_gpu.channel == sp && test_gpu.expect == 4 && gr32(OUT0 + 20) == 4);

    /* mapped header, inline entries through X1: an AutoSelect buffer takes index 1 in both lists */
    submit_header(1, 0);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.x[1] = buf(IN1 + 16, 8);
    ctx.b[0] = buf(OUT0, 0x18);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(test_gpu.submits == 3 && test_gpu.n == 1 && test_gpu.entries[0] == entries[2] && test_gpu.incs == 0);

    /* the entries count is cut to what the inline buffer holds */
    submit_header(3, 0);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.a[1] = buf(IN1, 8);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(test_gpu.submits == 4 && test_gpu.n == 1 && test_gpu.entries[0] == entries[0]);

    /* FenceWait (1): the fence goes to the GPU thread as the stream's first wait, whoever increments it, and the
       ioctl returns at once (the pc_wait stand-in fails any wait here) */
    u8 h[0x18];
    u32 fence[2] = {7, 0x1234}, flags = 1, one = 1;
    memset(h, 0, sizeof h);
    snx_st_le32(h + 8, one);
    snx_st_le32(h + 12, flags);
    snx_st_le32(h + 16, fence[0]);
    snx_st_le32(h + 20, fence[1]);
    memcpy(GP(IN0), h, sizeof h);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.a[1] = buf(IN1, 8);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(test_gpu.submits == 5 && test_gpu.wait_id == 7 && test_gpu.wait_val == 0x1234);

    /* IncrementWithValue (0x100) with FenceIncrement: the entries increment fval times themselves, so the job adds only
       the host's two, while the fence promises all five */
    fence[1] = 3;
    flags = 0x102;
    snx_st_le32(h + 12, flags);
    snx_st_le32(h + 16, fence[0]);
    snx_st_le32(h + 20, fence[1]);
    memcpy(GP(IN0), h, sizeof h);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.a[1] = buf(IN1, 8);
    ctx.b[0] = buf(OUT0, 0x18);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(test_gpu.submits == 6 && test_gpu.incs == 2 && test_gpu.expect == 9 && gr32(OUT0 + 20) == 9);
}

/* the ctrl device's syncpoint ioctls: an increment wakes the GPU thread (gpu_kick), an id past the table is
   refused, and an unregistered event slot isn't signaled by a later increment */
#define SYNCPT_READ   0xC0080014u
#define SYNCPT_INCR   0x40040015u
#define EVENT_WAIT_AS 0xC010001Eu
#define EVENT_REG     0xC004001Fu
#define EVENT_UNREG   0xC0040020u
static void put(u32 off, u32 v) { gw32(IN0 + off, v); }
static void test_ctrl_syncpoints(u32 ctrl) {
    u32 kicks = test_gpu.kicks;
    put(0, 40);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && test_gpu.kicks == kicks + 1 && syncpt_read(40) == 1);
    put(0, 40);
    CHECK(nv_ioctl1(ctrl, SYNCPT_READ, 8) == 0 && gr32(OUT0 + 4) == 1);
    put(0, SYNCPT_IDS + 40); /* would land on 40's entry */
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 4 && syncpt_read(40) == 1 && test_gpu.kicks == kicks + 1);
    /* slot 9: registered, armed by an async wait for 40 to reach 2, unregistered, then 40 reaches 2 */
    put(0, 9);
    CHECK(nv_ioctl1(ctrl, EVENT_REG, 4) == 0);
    put(0, 40);
    put(4, 2);
    put(8, 100);
    put(12, 9);
    CHECK(nv_ioctl1(ctrl, EVENT_WAIT_AS, 16) == 5 && CTRL_EV(ctrl)[9].armed);
    CHECK((gr32(OUT0 + 12) & 0xF000FFFFu) == 0x10000009u && ((gr32(OUT0 + 12) >> 16) & 0xFFF) == 40);
    put(0, 9);
    CHECK(nv_ioctl1(ctrl, EVENT_UNREG, 4) == 0 && !CTRL_EV(ctrl)[9].armed);
    put(0, 40);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && !kev(CTRL_EV(ctrl)[9].ev)->signaled);
    /* Canceling a timed-out wait must not report an unfinished fence as complete. Reusing the event
       must still wait for the new threshold, and cancellation also clears a previously signaled event. */
    put(0, 10);
    CHECK(nv_ioctl1(ctrl, EVENT_REG, 4) == 0);
    put(0, 40);
    put(4, 4);
    put(8, 100);
    put(12, 10);
    CHECK(nv_ioctl1(ctrl, EVENT_WAIT_AS, 16) == 5 && CTRL_EV(ctrl)[10].armed);
    put(0, 10);
    CHECK(nv_ioctl1(ctrl, 0xC004001Cu, 4) == 0);
    CHECK(!CTRL_EV(ctrl)[10].armed && !kev(CTRL_EV(ctrl)[10].ev)->signaled && syncpt_read(40) == 2);
    put(0, 40);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && !kev(CTRL_EV(ctrl)[10].ev)->signaled);
    put(0, 40);
    put(4, 4);
    put(8, 100);
    put(12, 10);
    CHECK(nv_ioctl1(ctrl, EVENT_WAIT_AS, 16) == 5 && !kev(CTRL_EV(ctrl)[10].ev)->signaled);
    put(0, 40);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && kev(CTRL_EV(ctrl)[10].ev)->signaled);
    put(0, 10);
    CHECK(nv_ioctl1(ctrl, 0xC004001Cu, 4) == 0 && !kev(CTRL_EV(ctrl)[10].ev)->signaled);
}

/* GET_CONFIG (IocGetConfigParams, 0x183 bytes) finds no setting, as on a production console: an SDK that found one
   would take its debug switch as on */
#define GET_CONFIG 0xC183001Bu
static void test_ctrl_get_config(u32 ctrl) {
    static const char* const names[] = {"NVWSI_FILL", "NVWSI_DUMP"};
    for (u32 i = 0; i < 2; i++) {
        memset(GP(IN0), 0, 0x183);
        strcpy((char*)GP(IN0), "nv");
        strcpy((char*)GP(IN0 + 0x41), names[i]);
        CHECK(nv_ioctl1(ctrl, GET_CONFIG, 0x183) == 0x30006);
    }
}

/* an async wait on ctrl fd fd's slot for syncpoint id to reach th, with a timeout in ms */
static u32 event_wait(u32 fd, u32 id, u32 th, u32 to, u32 slot) {
    put(0, id);
    put(4, th);
    put(8, to);
    put(12, slot);
    return nv_ioctl1(fd, EVENT_WAIT_AS, 16);
}
/* each ctrl fd has slots of its own, as on the console: the game's GPU fence waits and its movie player use slot 0 of
   two fds, and the player resetting its event must not take the other's wake. A wait with no time only asks */
static void test_ctrl_event_fds(u32 ctrl) {
    u32 error;
    u32 other = nv_open("/dev/nvhost-ctrl", &error);
    CHECK(!error && other != ctrl);
    put(0, 0);
    CHECK(nv_ioctl1(ctrl, EVENT_REG, 4) == 0);
    put(0, 0);
    CHECK(nv_ioctl1(other, EVENT_REG, 4) == 0);
    NvEvent* a = &CTRL_EV(ctrl)[0];
    NvEvent* b = &CTRL_EV(other)[0];
    CHECK(a->ev && b->ev && a->ev != b->ev);
    u32 base = syncpt_read(41);
    CHECK(event_wait(ctrl, 41, base + 1, 100, 0) == 5 && a->armed && !b->armed);
    CHECK(event_wait(other, 41, base + 2, 100, 0) == 5 && a->armed && b->armed && a->thresh == base + 1);
    put(0, 41);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && kev(a->ev)->signaled && !a->armed && !kev(b->ev)->signaled &&
          b->armed);
    /* polling the other fd's fence leaves its armed wait, and this fd's signal, as they were */
    CHECK(event_wait(other, 41, base + 2, 0, 0) == 5 && b->armed && kev(a->ev)->signaled);
    CHECK(event_wait(ctrl, 41, base + 2, 0, 0) == 5 && kev(a->ev)->signaled && !a->armed);
    put(0, 41);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && kev(b->ev)->signaled && !b->armed);
    CHECK(nv_close(other) == 0 && !g_hle.nvdrv.ctrl_ev[other]); /* its slots go with it */
}

/* an increment under the renderer's lock (syncpt_add_quiet) wakes the GPU thread but signals an armed slot only at
   the thread's syncpt_events, once, and a zero increment leaves nothing due */
static void test_quiet_increments(u32 ctrl) {
    put(0, 11);
    CHECK(nv_ioctl1(ctrl, EVENT_REG, 4) == 0);
    NvEvent* e = &CTRL_EV(ctrl)[11];
    u32 base = syncpt_read(42), kicks = test_gpu.kicks;
    CHECK(event_wait(ctrl, 42, base + 1, 100, 11) == 5 && e->armed);
    syncpt_add_quiet(42, 1);
    CHECK(syncpt_read(42) == base + 1 && test_gpu.kicks == kicks + 1 && e->armed && !kev(e->ev)->signaled);
    syncpt_events();
    CHECK(!e->armed && kev(e->ev)->signaled);
    /* armed again where it already is, so only a check of the slots signals it: none is due */
    kevent_clear(kev(e->ev));
    e->armed = 1;
    syncpt_events();
    CHECK(e->armed && !kev(e->ev)->signaled);
    syncpt_add_quiet(42, 0);
    syncpt_events();
    CHECK(e->armed && !kev(e->ev)->signaled && syncpt_read(42) == base + 1 && test_gpu.kicks == kicks + 1);
    syncpt_add(42, 1); /* checks at once */
    CHECK(!e->armed && kev(e->ev)->signaled);
    put(0, 11);
    CHECK(nv_ioctl1(ctrl, EVENT_UNREG, 4) == 0);
}

/* Ioctl3 (12): ctrl-gpu's GET_CHARACTERISTICS writes the characteristics to the inline output, B1 or C1 */
#define GET_CHARACTERISTICS 0xC0B04705u
static void test_ioctl3(u32 ctrlgpu) {
    u64 header[2] = {0xA0, 0x7000};
    gw64(IN0, header[0]);
    gw64(IN0 + 8, header[1]);
    memset(GP(OUT1), 0xCC, 0xB0);
    request(12, ctrlgpu, GET_CHARACTERISTICS);
    ctx.a[0] = buf(IN0, 0xB0);
    ctx.b[0] = buf(OUT0, 0xB0);
    ctx.b[1] = buf(OUT1, 0xA0);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    CHECK(gr32(OUT1) == 0x120 && gr32(OUT1 + 4) == 0xB && gr32(OUT1 + 8) == 0xA1); /* GM20B, arch 0x120 */
    CHECK(GP(OUT1)[0xA0] == 0xCC);                                                 /* nothing past its size */
    CHECK(gr64(OUT0) == 0xA0 && gr64(OUT0 + 8) == 0x7000 && gr32(OUT0 + 16) == 0x120);
    /* through C1, B empty */
    memset(GP(OUT1), 0xCC, 0xB0);
    request(12, ctrlgpu, GET_CHARACTERISTICS);
    ctx.x[0] = buf(IN0, 0xB0);
    ctx.c[0] = buf(OUT0, 0xB0);
    ctx.c[1] = buf(OUT1, 0x10);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0);
    CHECK(gr32(OUT1) == 0x120 && GP(OUT1)[0x10] == 0xCC);
    /* Ioctl2's inline buffer is an input: ctrl-gpu writes nothing to it */
    memset(GP(IN1), 0xCC, 0xA0);
    request(11, ctrlgpu, GET_CHARACTERISTICS);
    ctx.a[0] = buf(IN0, 0xB0);
    ctx.a[1] = buf(IN1, 0xA0);
    ctx.b[0] = buf(OUT0, 0xB0);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 0 && GP(IN1)[0] == 0xCC);
    /* VSMS_MAPPING writes {gpc, tpc} of each SM into its own buffer, never to an address taken from it */
    u8 d[8], mapping[4];
    const u8 sms[4] = {0, 0, 0, 1};
    memset(d, 0xCC, sizeof d);
    memset(mapping, 0xCC, sizeof mapping);
    CHECK(ioctl_ctrlgpu(0xC0044714u, d, 4, mapping, 4) == 0 && !memcmp(d, sms, 4) && !memcmp(mapping, sms, 4));
    CHECK(d[4] == 0xCC);
    CHECK(ioctl_ctrlgpu(0xC0084714u & ~0x40000000u, d, 8, NULL, 0) == 0 && d[0] == 7 && d[4] == 1); /* slot mask */
}

/* A Switch 2 driver's GET_CHARACTERISTICS (0xD0 bytes) and VSMS_MAPPING. Its device setup fails on an address space
   level below 6 at 0x78, and on a last {gpc, tpc} pair left at its 0xC0 fill */
static void test_ctrlgpu_switch2(void) {
    u8 d[0xE0], c[0xD8], mapping[8];
    g_hle.kernel.hw = SNX_HW_SWITCH2;
    memset(d, 0xCC, sizeof d);
    memset(c, 0xCC, sizeof c);
    CHECK(ioctl_ctrlgpu(0xC0E04705u, d, sizeof d, c, sizeof c) == 0);
    CHECK(snx_ld_le64(d) == 0xD0 && !memcmp(d + 16, c, 0xD0) && c[0xD0] == 0xCC);
    CHECK(snx_ld_le32(c) == 0x170 && snx_ld_le32(c + 4) == 0xE); /* the driver's chip 0x17E */
    /* SM 8.8 in both version fields: one launch title's driver has its GPU programs for no other version */
    CHECK(snx_ld_le32(c + 0x3C) == 0x808 && snx_ld_le32(c + 0x40) == 0x808);
    CHECK(snx_ld_le32(c + 0x28) == 0x1000 && snx_ld_le32(c + 0x34) == 0x11000);
    CHECK((snx_ld_le64(c + 0x50) >> 8 & 1) && (snx_ld_le64(c + 0x50) >> 22 & 1) && snx_ld_le32(c + 0xB4) == 64);
    CHECK((i16)snx_ld_le16(c + 0x78) >= 6); /* the address space level */
    CHECK(snx_ld_le16(c + 0x70) == 0x1C && snx_ld_le16(c + 0x72) == 0xB && snx_ld_le16(c + 0x74) == 0xFFFF);
    CHECK(snx_ld_le16(c + 0x76) >= 0x1F); /* NvRmGpuChannelSetPreemptionModes rejects older interfaces */
    /* VSMS_MAPPING: a pair for each of the four SMs, the argument left as it came */
    const u8 pairs[8] = {0, 0, 0, 0, 0, 1, 0, 1};
    memset(d, 0xCC, 8);
    memset(mapping, 0xC0, sizeof mapping);
    CHECK(ioctl_ctrlgpu(0xC0084713u, d, 8, mapping, sizeof mapping) == 0 && !memcmp(mapping, pairs, 8));
    CHECK(d[0] == 0xCC && d[7] == 0xCC);
    CHECK(ioctl_ctrlgpu(0xC0084713u, d, 8, mapping, 6) == NV_RESULT_BAD_PARAMETER);
    g_hle.kernel.hw = SNX_HW_SWITCH;
}

/* QueryEvent (4) {fd, event id}: the NvError, then the event as a copy handle */
static KEvent* query_event(u32 fd, u32 id) {
    request(4, fd, id);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    CHECK(ctx.nout_copy == 1 && !ctx.nout_move);
    KObj* o = handle_object(ctx.out_copy[0]);
    CHECK(o->type == KO_EVENT);
    return (KEvent*)o;
}

/* a Close from another thread while an ioctl runs on the fd: the fd stays (closed) until the ioctl leaves, another
   ioctl on it answers NvError 8 (InvalidState), and Open doesn't hand its number out meanwhile */
static u32 race_fd, race_other_fd;
static void close_during_ioctl(void) {
    IpcCtx running = ctx; /* the ioctl's request, which the dispatcher answers into when this returns */
    CHECK(nv_close(race_fd) == 0);
    CHECK(g_hle.nvdrv.nvf[race_fd].dev == DEV_NVDEC && g_hle.nvdrv.nvf[race_fd].closed && g_hle.nvdrv.nvf[race_fd].users == 1);
    CHECK(!test_channels[0].closed);
    CHECK(nv_ioctl1(race_fd, 0xC0044801u, 4) == 8);
    CHECK(nv_close(race_fd) == 8); /* a second Close finds the fd closed, and changes nothing */
    CHECK(g_hle.nvdrv.nvf[race_fd].closed && !test_channels[0].closed);
    u32 error;
    race_other_fd = nv_open("/dev/nvhost-vic", &error);
    CHECK(!error && race_other_fd != race_fd);
    ctx = running;
}
static void test_close_during_ioctl(void) {
    u32 error;
    race_fd = nv_open("/dev/nvhost-nvdec", &error);
    CHECK(!error && race_fd > 0 && race_fd < 256 && test_nchannels == 1);
    CHECK(nv_ioctl1(race_fd, 0xC0044801u, 4) == 0 && gr32(OUT0) == 0xC0044801u && test_channels[0].ioctls == 1);
    test_during_ioctl = close_during_ioctl;
    CHECK(nv_ioctl1(race_fd, 0xC0044802u, 4) == 0);
    CHECK(test_channels[0].ioctls == 2 && test_channels[0].closed); /* the last ioctl out released it */
    CHECK(!g_hle.nvdrv.nvf[race_fd].dev && !g_hle.nvdrv.nvf[race_fd].closed && !g_hle.nvdrv.nvf[race_fd].users);
    CHECK(nv_ioctl1(race_fd, 0xC0044801u, 4) == 8);
    u32 again = nv_open("/dev/nvhost-nvdec", &error);
    CHECK(!error && again == race_fd); /* free now: the lowest fd again */
    CHECK(nv_close(again) == 0 && nv_close(race_other_fd) == 0);
    CHECK(test_channels[1].closed && test_channels[2].closed);
}

/* nvmap's handle lifetime and parameters, and the address space's allocator, pieces, REMAP and regions, through the
   device handlers */
static u32 nvmap_call(u32 cmd, u8* d) { return ioctl_nvmap(cmd, d, 32); }
static u32 nvmap_new(u32 size, u64 addr) {
    u8 d[32] = {0};
    u32 h;
    snx_st_le32(d, size);
    CHECK(nvmap_call(0x01, d) == 0);
    h = snx_ld_le32(d + 4);
    memset(d, 0, sizeof d);
    snx_st_le32(d, h);
    snx_st_le64(d + 24, addr);
    CHECK(nvmap_call(0x04, d) == 0);
    return h;
}
static u32 nvmap_param(u32 h, u32 param, u32* value) {
    u8 d[32] = {0};
    u32 r;
    snx_st_le32(d, h);
    snx_st_le32(d + 4, param);
    r = nvmap_call(0x09, d);
    *value = snx_ld_le32(d + 8);
    return r;
}
static NvFile as_file; /* an nvhost-as-gpu fd with no Switch 2 view */
static u32 as_call(u32 cmd, u8* d, u32 n) { return ioctl_as(&as_file, cmd, d, n, NULL, 0); }
static u64 as_map(u32 flags, u32 h, u64 size, u64 gva) {
    u8 d[40] = {0};
    snx_st_le32(d, flags);
    snx_st_le32(d + 8, h);
    snx_st_le64(d + 24, size);
    snx_st_le64(d + 32, gva);
    CHECK(as_call(0x06, d, sizeof d) == 0);
    gva = snx_ld_le64(d + 32);
    return gva;
}
static void test_nvmap_as(void) {
    u8 d[64] = {0};
    u32 h, again, v, one = 1;
    u64 gva, fixed, addr, size;
    /* CREATE and FROM_ID take a reference each, FREE gives one back; the number is handed out again after the last */
    h = nvmap_new(0x2000, 0x8000);
    snx_st_le32(d, h);
    CHECK(nvmap_call(0x03, d) == 0 && snx_ld_le32(d + 4) == h); /* FROM_ID: the id is the handle's number */
    memset(d, 0, sizeof d);
    snx_st_le32(d, h);
    CHECK(nvmap_call(0x05, d) == 0); /* not the last reference: no address, and the handle lives on */
    addr = snx_ld_le64(d + 8);
    v = snx_ld_le32(d + 16);
    CHECK(addr == 0 && v == 0 && g_hle.nvdrv.nvh[h].alive);
    CHECK(nvmap_param(h, 4, &v) == 0 && v == 0x40000000u); /* Eden's heap for an allocated handle */
    CHECK(nvmap_param(h, 3, &v) == 0 && v == 0x8000);       /* Base: the address's low bits */
    CHECK(nvmap_param(h, 2, &v) == 0 && v == 0x1000);       /* no alignment given */
    CHECK(nvmap_param(h, 1, &v) == 0 && v == 0x2000);
    memset(d, 0, sizeof d);
    snx_st_le32(d + 4, h);
    CHECK(nvmap_call(0x0E, d) == 0 && snx_ld_le32(d) == h); /* GET_ID */
    memset(d, 0, sizeof d);
    snx_st_le32(d, h);
    CHECK(nvmap_call(0x05, d) == 0 && !g_hle.nvdrv.nvh[h].alive);
    CHECK(snx_ld_le64(d + 8) == 0x8000 && snx_ld_le32(d + 16) == 0x2000); /* the last reference reports it */
    CHECK(nvmap_call(0x05, d) == 1 && nvmap_param(h, 1, &v) == 1);
    memset(d, 0, sizeof d);
    snx_st_le32(d + 4, h);
    CHECK(nvmap_call(0x0E, d) == 1);
    snx_st_le32(d, h);
    CHECK(nvmap_call(0x03, d) == 1); /* no live handle with that id */
    again = nvmap_new(0x3000, 0x9000);
    CHECK(again == h);

    /* ALLOC_SPACE: allocator VA at a multiple of the alignment, from VA_BASE up */
    memset(d, 0, sizeof d);
    v = 4;
    snx_st_le32(d, v);
    v = 0x10000;
    snx_st_le32(d + 4, v);
    CHECK(as_call(0x02, d, 24) == 0);
    gva = snx_ld_le64(d + 16);
    CHECK(gva >= VA_BASE && !(gva & 0xFFFF));
    v = 3; /* a page size that isn't a power of two */
    snx_st_le32(d + 4, v);
    memset(d + 16, 0, 8);
    CHECK(as_call(0x02, d, 24) == 4);

    /* a mapping from the allocator, a fixed one over its second page, then UNMAP_BUFFER of the first: only the pages
       still its own are cleared, and its VA goes back */
    gva = as_map(0, again, 0x2000, 0);
    CHECK(gva >= VA_BASE && gmmu_translate(gva) == 0x9000 && gmmu_translate(gva + 0x1FFF) == 0xAFFF);
    h = nvmap_new(0x1000, 0xC000);
    fixed = as_map(1, h, 0x1000, gva + 0x1000);
    CHECK(fixed == gva + 0x1000 && gmmu_translate(fixed) == 0xC000);
    memset(d, 0, sizeof d);
    snx_st_le64(d, gva);
    CHECK(as_call(0x05, d, 8) == 0);
    CHECK(!gmmu_translate(gva) && gmmu_translate(fixed) == 0xC000);
    CHECK(as_map(0, again, 0x1000, 0) == gva); /* first fit takes the freed VA again */
    /* FREE_SPACE clears every page of its range */
    memset(d, 0, sizeof d);
    snx_st_le64(d, gva);
    v = 2;
    snx_st_le32(d + 8, v);
    v = 0x1000;
    snx_st_le32(d + 12, v);
    CHECK(as_call(0x03, d, 16) == 0 && !gmmu_translate(gva) && !gmmu_translate(fixed));
    addr = VA_END - 0x1000;
    snx_st_le64(d, addr);
    CHECK(as_call(0x03, d, 16) == 4); /* past the 40-bit space */

    /* REMAP in big pages: map, a bad handle skipped, then handle 0 unmaps */
    memset(d, 0, sizeof d);
    v = (u32)((VA_BASE + 0x1000000) >> 16);
    snx_st_le32(d + 4, h);
    snx_st_le32(d + 12, v);
    snx_st_le32(d + 16, one);
    v = 0x1FFF; /* no such handle */
    snx_st_le32(d + 24, v);
    CHECK(as_call(0x14, d, 40) == 0 && gmmu_translate(VA_BASE + 0x1000000) == 0xC000);
    memset(d + 4, 0, 4);
    CHECK(as_call(0x14, d, 20) == 0 && !gmmu_translate(VA_BASE + 0x1000000));

    /* GET_VA_REGIONS: the big-page region starts where the allocator does */
    memset(d, 0, sizeof d);
    CHECK(as_call(0x08, d, 64) == 0);
    v = snx_ld_le32(d + 8);
    addr = snx_ld_le64(d + 40);
    size = snx_ld_le64(d + 16);
    CHECK(v == 48 && addr == VA_BASE && size == 0x10000ull << 10);
    v = snx_ld_le32(d + 48);
    CHECK(v == 0x10000);
    /* ALLOC_AS_EX: 0 is 128K big pages */
    memset(d, 0, sizeof d);
    CHECK(as_call(0x09, d, 24) == 0 && g_hle.nvdrv.big_page_bits == 17);
    v = 0x10000;
    snx_st_le32(d + 8, v);
    CHECK(as_call(0x09, d, 24) == 0 && g_hle.nvdrv.big_page_bits == 16);

    /* a GPU mapping pins the handle, as the console's nvmap does: FREE reports no memory and leaves it alive, and
       UNMAP_BUFFER of the last mapping ends it */
    h = nvmap_new(0x2000, 0x8000);
    gva = as_map(0, h, 0x2000, 0);
    memset(d, 0, sizeof d);
    snx_st_le32(d, h);
    CHECK(nvmap_call(0x05, d) == 0);
    CHECK(snx_ld_le64(d + 8) == 0 && g_hle.nvdrv.nvh[h].alive && g_hle.nvdrv.nvh[h].pins == 1);
    memset(d, 0, sizeof d);
    snx_st_le64(d, gva);
    CHECK(as_call(0x05, d, 8) == 0 && !g_hle.nvdrv.nvh[h].alive);
}


/* guest memory the guest freed after the GPU mapped it, [freed, freed + freed_size) (none while freed_size is 0):
   mem_mapped_run's runs stop there */
static u64 freed, freed_size;
u64 mem_mapped_run(u64 addr, u64 size) {
    if (!freed_size || addr + size <= freed || addr >= freed + freed_size) return size;
    return addr < freed ? freed - addr : 0;
}
/* a page the guest freed after the GPU mapped it ends a run there and translates to none, as one with no entry */
static void test_gmmu_freed(void) {
    const u64 g = 0x7100000000ull;
    u64 cpu = 0;
    gmmu_map(g, 0xA0000000ull, 0x4000);
    freed = 0xA0002000ull;
    freed_size = 0x1000;
    CHECK(gmmu_run(g, ~0ull, &cpu) == 0x2000 && cpu == 0xA0000000ull);
    CHECK(!gmmu_run(g + 0x2000, ~0ull, &cpu) && !gmmu_translate(g + 0x2000));
    CHECK(gmmu_translate(g + 0x1FFF) == 0xA0001FFFull && gmmu_translate(g + 0x3000) == 0xA0003000ull);
    CHECK(gmmu_run(g + 0x3000, ~0ull, &cpu) == 0x1000 && cpu == 0xA0003000ull);
    freed_size = 0;
    CHECK(gmmu_run(g, ~0ull, &cpu) == 0x4000);
    gmmu_unmap(g, 0x4000, 0, 0);
}
/* gmmu_run against gmmu_translate page by page: runs longer than an entry's count, across a table's end, broken by a
   page mapped elsewhere, and what an own unmap keeps */
static u64 run_by_pages(u64 gva, u64 max, u64* cpu) {
    u64 n, first = gmmu_translate(gva);
    if (!first) return 0;
    *cpu = first;
    n = 0x1000 - (gva & 0xFFF);
    while (n < max && gmmu_translate(gva + n) == first + n) n += 0x1000;
    return n < max ? n : max;
}
static void check_runs(u64 gva, u64 size) {
    for (u64 o = 0; o < size; o += 0x1000 + 0x123) {
        u64 a = 0, b = 0, na = gmmu_run(gva + o, size - o, &a), nb = run_by_pages(gva + o, size - o, &b);
        CHECK(na == nb && (!na || a == b));
    }
}
static void test_gmmu_runs(void) {
    const u64 g = 0x7000000000ull; /* a table starts here: 4096 pages of 4K each */
    u64 cpu = 0;
    gmmu_map(g - 0x3000, 0x80000000ull - 0x3000, 0xA03000); /* 2563 pages, across the table's start */
    CHECK(gmmu_run(g - 0x3000, ~0ull, &cpu) == 0xA03000 && cpu == 0x80000000ull - 0x3000);
    CHECK(gmmu_run(g + 0x5, 0x10000, &cpu) == 0x10000 && cpu == 0x80000005ull);
    check_runs(g - 0x3000, 0xA04000);
    gmmu_map(g + 0x200000, 0x90000000ull, 0x1000); /* a page in the middle mapped elsewhere */
    CHECK(gmmu_run(g, ~0ull, &cpu) == 0x200000 && cpu == 0x80000000ull);
    CHECK(gmmu_run(g + 0x201000, ~0ull, &cpu) == 0xA00000 - 0x201000 && cpu == 0x80201000ull);
    check_runs(g - 0x3000, 0xA04000);
    gmmu_unmap(g - 0x3000, 0xA03000, 1, 0x80000000ull - 0x3000); /* the first mapping's pages, not the one over it */
    CHECK(!gmmu_run(g, ~0ull, &cpu) && !gmmu_run(g - 0x3000, ~0ull, &cpu));
    CHECK(gmmu_run(g + 0x200000, ~0ull, &cpu) == 0x1000 && cpu == 0x90000000ull);
    check_runs(g - 0x3000, 0xA04000);
    gmmu_unmap(g + 0x200000, 0x1000, 0, 0);
}
/* gmmu_aliases against the page table, read page by page: random mappings over a GPU range, many of them showing the
   same guest pages, own and plain unmaps, maps over maps, and REMAP's and FREE_SPACE's ioctls. After each change the
   runs must be the table's pages and each query must report every GPU page showing the guest range once, clipped */
#define ALIAS_G     (0x7200000000ull - 0x100000) /* across a second-level table's start */
#define ALIAS_PAGES 1024u
#define ALIAS_C     0x10000000ull
#define ALIAS_CPAGES 256u
static struct {
    u32 count[ALIAS_PAGES];
    u64 lo[ALIAS_PAGES], hi[ALIAS_PAGES];
    u32 outside;
} alias_seen;
static u32 alias_rand_state = 12345;
static u32 alias_rand(void) {
    alias_rand_state = alias_rand_state * 1103515245u + 12345u;
    return alias_rand_state >> 8;
}
static void alias_note(void* ctx, u64 gva, u64 n) {
    (void)ctx;
    CHECK(n != 0);
    while (n) {
        u64 off = gva & 0xFFF, k = 0x1000 - off < n ? 0x1000 - off : n, i = (gva - ALIAS_G) >> 12;
        if (gva < ALIAS_G || i >= ALIAS_PAGES) {
            alias_seen.outside++;
        } else {
            alias_seen.count[i]++;
            alias_seen.lo[i] = off;
            alias_seen.hi[i] = off + k;
        }
        gva += k;
        n -= k;
    }
}
static void alias_query(u64 cpu, u64 size) {
    memset(&alias_seen, 0, sizeof alias_seen);
    gmmu_aliases(cpu, size, alias_note, NULL);
    CHECK(alias_seen.outside == 0); /* nothing else maps the test's guest pages */
    for (u32 i = 0; i < ALIAS_PAGES; i++) {
        u64 c = gmmu_translate(ALIAS_G + (u64)i * 0x1000), a, b;
        if (!c || c + 0x1000 <= cpu || c >= cpu + size) {
            CHECK(alias_seen.count[i] == 0);
            continue;
        }
        a = c > cpu ? 0 : cpu - c;
        b = c + 0x1000 < cpu + size ? 0x1000 : cpu + size - c;
        CHECK(alias_seen.count[i] == 1 && alias_seen.lo[i] == a && alias_seen.hi[i] == b);
    }
}
/* the runs are sorted, disjoint, and show what the table does over the test's range */
static void alias_check_runs(void) {
    const GRun* r = g_hle.nvdrv.gruns;
    for (u32 k = 0; k + 1 < g_hle.nvdrv.ngruns; k++) CHECK(r[k].pages && r[k].gpage + r[k].pages <= r[k + 1].gpage);
    for (u32 i = 0; i < ALIAS_PAGES; i++) {
        u64 g = (ALIAS_G >> 12) + i, c = gmmu_translate(g << 12);
        u32 k = grun_find(g);
        int in = k < g_hle.nvdrv.ngruns && r[k].gpage <= g;
        CHECK(in == (c != 0));
        if (in) CHECK(c >> 12 == r[k].cpage + (g - r[k].gpage));
    }
}
static void alias_check(void) {
    alias_check_runs();
    for (int q = 0; q < 8; q++) {
        u64 cpu = ALIAS_C + alias_rand() % ((u64)ALIAS_CPAGES * 0x1000), size = 1 + alias_rand() % 0x9000;
        alias_query(cpu, size);
    }
    alias_query(ALIAS_C, (u64)ALIAS_CPAGES * 0x1000);
    alias_query(ALIAS_C - 0x100000, 0x400000);
}
static void test_gmmu_aliases(void) {
    u64 mg[64], mc[64], mn[64];
    u32 nm = 0;
    for (int step = 0; step < 2000; step++) {
        u32 op = alias_rand() % 8;
        if (op < 4 || !nm) { /* a map, now and then a long one */
            u64 n = alias_rand() % 16 ? 1 + alias_rand() % 48 : 1 + alias_rand() % 400;
            u64 g = alias_rand() % (ALIAS_PAGES - n + 1), c = alias_rand() % (ALIAS_CPAGES - (n < ALIAS_CPAGES ? n : 0) + 1);
            if (n > ALIAS_CPAGES) c = 0;
            gmmu_map(ALIAS_G + g * 0x1000, ALIAS_C + c * 0x1000, n * 0x1000);
            if (nm == 64) nm--;
            mg[nm] = g, mc[nm] = c, mn[nm] = n, nm++;
        } else if (op < 6) { /* an earlier mapping's own unmap: what was mapped over it since stays */
            u32 k = alias_rand() % nm;
            gmmu_unmap(ALIAS_G + mg[k] * 0x1000, mn[k] * 0x1000, 1, ALIAS_C + mc[k] * 0x1000);
            mg[k] = mg[--nm], mc[k] = mc[nm], mn[k] = mn[nm];
        } else { /* a plain unmap of a range */
            u64 n = 1 + alias_rand() % 64, g = alias_rand() % (ALIAS_PAGES - n + 1);
            gmmu_unmap(ALIAS_G + g * 0x1000, n * 0x1000, 0, 0);
        }
        alias_check();
    }
    gmmu_unmap(ALIAS_G, (u64)ALIAS_PAGES * 0x1000, 0, 0);
    alias_check();
    CHECK(!g_hle.nvdrv.alias.stale);
    { /* REMAP of a handle's memory in big pages, then FREE_SPACE over it, through the ioctls */
        u8 request[40] = {0}, remap[20] = {0};
        u32 size = 0x20000, handle, pages = 1, big = 0x10000, gpu_offset = (u32)((ALIAS_G + 0x100000) >> 16), one = 1;
        u64 addr = ALIAS_C, gva = ALIAS_G + 0x100000;
        memcpy(request, &size, 4);
        CHECK(ioctl_nvmap(0x01, request, sizeof request) == 0);
        memcpy(&handle, request + 4, 4);
        memcpy(request, &handle, 4);
        memcpy(request + 24, &addr, 8);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 0);
        memset(request, 0, sizeof request);
        memcpy(request, &pages, 4);
        memcpy(request + 4, &big, 4);
        memcpy(request + 8, &one, 4);
        memcpy(request + 16, &gva, 8);
        CHECK(as_call(0x02, request, sizeof request) == 0); /* ALLOC_SPACE there */
        memcpy(remap + 4, &handle, 4);
        memcpy(remap + 8, &one, 4); /* the handle's second big page */
        memcpy(remap + 12, &gpu_offset, 4);
        memcpy(remap + 16, &pages, 4);
        CHECK(as_call(0x14, remap, sizeof remap) == 0);
        CHECK(gmmu_translate(gva) == ALIAS_C + 0x10000);
        memset(&alias_seen, 0, sizeof alias_seen);
        alias_query(ALIAS_C + 0xF000, 0x3000);
        CHECK(alias_seen.count[0x100] == 1 && alias_seen.lo[0x100] == 0 && alias_seen.count[0x101] == 1);
        alias_check();
        memset(request, 0, sizeof request);
        memcpy(request, &gva, 8);
        memcpy(request + 8, &pages, 4);
        memcpy(request + 12, &big, 4);
        CHECK(as_call(0x03, request, sizeof request) == 0); /* FREE_SPACE */
        CHECK(!gmmu_translate(gva));
        alias_query(ALIAS_C, 0x20000);
        CHECK(alias_seen.count[0x100] == 0);
        alias_check();
    }
}
/* GSV waits use the same counters as nvhost. Verify wraparound, cancellation, shrinking the slot set and
   destruction while the guest still holds an event handle. */
static void test_gsv_syncpoints(void) {
    SvcObj* s = nv_syncpoint_new();
    u32 handle, count;
    u8 wide;
    u64 value;
    u32 id = syncpt_alloc();
    u32 initial = syncpt_read(id);
    CHECK(nn_gsv_ISyncpointSupervisor_GetMaximumEventCount(s, &count) == 0 && count == 63);
    CHECK(nn_gsv_ISyncpointSupervisor_SetEventCount(s, 64) == RC(1, 14));
    CHECK(nn_gsv_ISyncpointSupervisor_SetEventCount(s, 2) == 0);
    CHECK(nn_gsv_ISyncpointSupervisor_GetEvent(s, 2, &handle) == RC(1, 14));
    CHECK(nn_gsv_ISyncpointSupervisor_GetEvent(s, 0, &handle) == 0);
    KEvent* event = (KEvent*)handle_object(handle);
    u8 wait_raw[20];
    memset(wait_raw, 0xA5, sizeof wait_raw);
    snx_st_le32(wait_raw, 0);
    snx_st_le32(wait_raw + 4, ~id);
    snx_st_le64(wait_raw + 8, (u32)(initial + 2));
    wait_raw[16] = 0; /* the SDK writes one mode byte, leaving the three padding bytes unspecified */
    IpcCtx wait = {.cmd = 201, .tipc = 1, .raw = wait_raw, .raw_size = sizeof wait_raw};
    CHECK(s->dispatch(s, &wait) == 0 && !event->signaled);
    syncpt_incr(id);
    CHECK(!event->signaled);
    syncpt_incr(id);
    CHECK(event->signaled);
    CHECK(nn_gsv_ISyncpointSupervisor_QueryCounter(s, ~id, &value, &wide) == 0 &&
          value == (u32)(initial + 2) && wide == 0);
    CHECK(nn_gsv_ISyncpointSupervisor_ArmWait(s, 0, ~id, (u32)(initial + 3), 0) == 0 && !event->signaled);
    CHECK(nn_gsv_ISyncpointSupervisor_CancelWait(s, 0) == 0 && event->signaled);
    g_hle.nvdrv.syncpt[id] = 0xfffffffeu;
    CHECK(nn_gsv_ISyncpointSupervisor_ArmWait(s, 0, ~id, 1, 0) == 0 && !event->signaled);
    syncpt_add(id, 2);
    CHECK(!event->signaled);
    syncpt_incr(id);
    CHECK(event->signaled);
    CHECK(nn_gsv_ISyncpointSupervisor_ArmWait(s, 0, ~id, 2, 0) == 0);
    CHECK(nn_gsv_ISyncpointSupervisor_SetEventCount(s, 0) == 0);
    syncpt_incr(id);
    CHECK(!event->signaled);
    CHECK(nn_gsv_ISyncpointSupervisor_SetEventCount(s, 1) == 0);
    CHECK(nn_gsv_ISyncpointSupervisor_ArmWait(s, 0, ~id, 3, 0) == 0);
    svcobj_unref(s);
    syncpt_incr(id);
    CHECK(!event->signaled && event->o.refs == 1);
    syncpt_free(id);
}
/* The native submission's fence is driver-owned. A Maxwell submission with the same flag still needs its stream
   increment, otherwise the driver would count the game's completion twice. Exercise the event the SDK waits on. */
static void test_submission_completion(void) {
    for (u32 native = 0; native < 2; ++native) {
        g_hle.kernel.hw = native ? SNX_HW_SWITCH2 : SNX_HW_SWITCH;
        u32 error, fd = nv_open("/dev/nvhost-gpu", &error);
        CHECK(!error);
        u8 d[GPU_SUBMIT_HEADER_BYTES] = {0};
        snx_st_le32(d + GPU_SUBMIT_FLAGS, GPU_SUBMIT_INCREMENT_VALUE);
        snx_st_le32(d + GPU_SUBMIT_FENCE_VALUE, 1);
        CHECK(gpu_submit_gpfifo(&g_hle.nvdrv.nvf[fd], SUBMIT_GPFIFO_EX, d, sizeof d, NULL, 0) == 0);
        u32 id = snx_ld_le32(d + GPU_SUBMIT_FENCE_ID), threshold = snx_ld_le32(d + GPU_SUBMIT_FENCE_VALUE);
        SvcObj* s = nv_syncpoint_new();
        u32 handle;
        CHECK(nn_gsv_ISyncpointSupervisor_SetEventCount(s, 1) == 0);
        CHECK(nn_gsv_ISyncpointSupervisor_GetEvent(s, 0, &handle) == 0);
        KEvent* event = (KEvent*)handle_object(handle);
        CHECK(nn_gsv_ISyncpointSupervisor_ArmWait(s, 0, ~id, threshold, 0) == 0 && !event->signaled);
        syncpt_add(id, test_gpu.incs); /* the queued job's completion */
        CHECK(event->signaled == (int)native);
        if (!native) {
            syncpt_incr(id); /* Maxwell's stream-owned completion */
            CHECK(event->signaled);
        }
        svcobj_unref(s);
        CHECK(nv_close(fd) == 0);
    }
    g_hle.kernel.hw = SNX_HW_SWITCH;
}
/* Legacy ioctls must not replace or retire worker pages, even on a different fd. */
static void test_gsv_legacy_isolation(NvFile* view, u64 gva) {
    u8 d[64] = {0};
    const u32 mutations[] = {NVHOST_AS_GPU_ALLOC_SPACE, NVHOST_AS_GPU_FREE_SPACE, NVHOST_AS_GPU_UNMAP_BUFFER,
                             NVHOST_AS_GPU_MAP_BUFFER_EX, NVHOST_AS_GPU_REMAP};
    for (u32 i = 0; i < sizeof mutations / sizeof mutations[0]; ++i)
        CHECK(ioctl_as(view, mutations[i], d, sizeof d, NULL, 0) == NV_RESULT_NOT_SUPPORTED);
    wr64(d, AS_UNMAP_BUFFER_OFFSET, gva);
    CHECK(as_call(NVHOST_AS_GPU_UNMAP_BUFFER, d, 8) == 0);
    CHECK(gmmu_translate(gva) == test_gpu_memory.addr + 0x1000);

    u32 h = nvmap_new(0x1000, 0x9000);
    const u64 protected[] = {gva, NV_SYNCPT_SHIM_GVA};
    for (u32 i = 0; i < sizeof protected / sizeof protected[0]; ++i) {
        memset(d, 0, sizeof d);
        wr32(d, AS_ALLOC_SPACE_PAGES, 1);
        wr32(d, AS_ALLOC_SPACE_PAGE_SIZE, 0x1000);
        wr32(d, AS_ALLOC_SPACE_FLAGS, NVHOST_AS_MAP_FIXED);
        wr64(d, AS_ALLOC_SPACE_OFFSET, protected[i]);
        CHECK(as_call(NVHOST_AS_GPU_ALLOC_SPACE, d, 24) == NV_RESULT_BAD_PARAMETER);
        memset(d, 0, sizeof d);
        wr32(d, AS_MAP_BUFFER_FLAGS, NVHOST_AS_MAP_FIXED);
        wr32(d, AS_MAP_BUFFER_HANDLE, h);
        wr64(d, AS_MAP_BUFFER_OFFSET, protected[i]);
        CHECK(as_call(NVHOST_AS_GPU_MAP_BUFFER_EX, d, 40) == NV_RESULT_BAD_PARAMETER);
        wr32(d, AS_MAP_BUFFER_FLAGS, NVHOST_AS_MAP_REMAP);
        CHECK(as_call(NVHOST_AS_GPU_MAP_BUFFER_EX, d, 40) == NV_RESULT_BAD_PARAMETER);
        memset(d, 0, sizeof d);
        wr64(d, AS_FREE_SPACE_OFFSET, protected[i]);
        wr32(d, AS_FREE_SPACE_PAGES, 1);
        wr32(d, AS_FREE_SPACE_PAGE_SIZE, 0x1000);
        CHECK(as_call(NVHOST_AS_GPU_FREE_SPACE, d, 16) == NV_RESULT_BAD_PARAMETER);
        memset(d, 0, sizeof d);
        wr32(d, AS_REMAP_ENTRY_AS_OFFSET, (u32)(protected[i] >> g_hle.nvdrv.big_page_bits));
        wr32(d, AS_REMAP_ENTRY_BIG_PAGES, 1);
        CHECK(as_call(NVHOST_AS_GPU_REMAP, d, AS_REMAP_ENTRY_BYTES) == NV_RESULT_BAD_PARAMETER);
        CHECK(gmmu_translate(gva) == test_gpu_memory.addr + 0x1000);
    }
    CHECK(!gmmu_translate(NV_SYNCPT_SHIM_GVA));

    /* Earlier cases used a Switch 1 allocator. Start a fresh fixture for the Switch 2 profile. */
    free(g_hle.nvdrv.vafree);
    g_hle.nvdrv.vafree = NULL;
    g_hle.nvdrv.nvafree = g_hle.nvdrv.capvafree = 0;
    g_hle.nvdrv.va_ready = 0;
    memset(d, 0, sizeof d);
    wr32(d, AS_ALLOC_SPACE_PAGES, 1);
    wr32(d, AS_ALLOC_SPACE_PAGE_SIZE, 0x1000);
    CHECK(as_call(NVHOST_AS_GPU_ALLOC_SPACE, d, 24) == 0);
    u64 allocated = rd64(d, AS_ALLOC_SPACE_OFFSET);
    CHECK(allocated == NV_GSV_USER_END);
    u64 mapped = as_map(0, h, 0, 0);
    CHECK(mapped == allocated + VA_UNIT && gmmu_translate(mapped) == 0x9000);
    CHECK(as_unmap_buffer(mapped) == 0 && !gmmu_translate(mapped));
    memset(d, 0, sizeof d);
    wr64(d, AS_FREE_SPACE_OFFSET, allocated);
    wr32(d, AS_FREE_SPACE_PAGES, 1);
    wr32(d, AS_FREE_SPACE_PAGE_SIZE, 0x1000);
    CHECK(as_call(NVHOST_AS_GPU_FREE_SPACE, d, 16) == 0);

    /* Fill the legacy interval. The last allocator granule includes the shim and must stay unavailable. */
    u64 end = NV_SYNCPT_SHIM_GVA & ~(VA_UNIT - 1);
    u32 pages = (u32)((end - NV_GSV_USER_END) / VA_UNIT);
    memset(d, 0, sizeof d);
    wr32(d, AS_ALLOC_SPACE_PAGES, pages);
    wr32(d, AS_ALLOC_SPACE_PAGE_SIZE, (u32)VA_UNIT);
    CHECK(as_call(NVHOST_AS_GPU_ALLOC_SPACE, d, 24) == 0 && rd64(d, AS_ALLOC_SPACE_OFFSET) == NV_GSV_USER_END);
    wr32(d, AS_ALLOC_SPACE_PAGES, 1);
    wr64(d, AS_ALLOC_SPACE_OFFSET, 0);
    CHECK(as_call(NVHOST_AS_GPU_ALLOC_SPACE, d, 24) == NV_RESULT_INSUFFICIENT_MEMORY);
    memset(d, 0, sizeof d);
    wr64(d, AS_FREE_SPACE_OFFSET, NV_GSV_USER_END);
    wr32(d, AS_FREE_SPACE_PAGES, pages);
    wr32(d, AS_FREE_SPACE_PAGE_SIZE, (u32)VA_UNIT);
    CHECK(as_call(NVHOST_AS_GPU_FREE_SPACE, d, 16) == 0);
    memset(d, 0, sizeof d);
    wr32(d, 0, h);
    CHECK(nvmap_call(0x05, d) == 0);
}
/* a sparse map request of 216, as the SDK's util::ExecuteVirtualMachineRemap writes it into IN1 */
static void test_sparse_request(u32 i, u64 gva, u64 size, u32 memory, u64 offset, u8 fallback_kind) {
    u8* q = GP(IN1) + i * 0x28;
    memset(q, 0, 0x28);
    snx_st_le64(q, gva);
    snx_st_le64(q + 8, size);
    q[0x13] = 0xE0;
    q[0x14] = fallback_kind;
    snx_st_le32(q + 0x18, memory);
    snx_st_le64(q + 0x20, offset);
}
/* 216 through the worker's dispatcher, in the SDK's TIPC layout */
static int test_sparse_map(SvcObj* worker, u32 as, u32 count, u64* processed) {
    u8 args[28] = {0};
    snx_st_le64(args, 1);
    snx_st_le32(args + 8, as);
    snx_st_le64(args + 12, 0x28);
    snx_st_le32(args + 20, 0xAABBCC01); /* sync, with the padding the SDK leaves */
    IpcCtx c = {.tipc = 1, .cmd = 216, .raw = args, .raw_size = sizeof args, .na = 1, .nb = 1};
    c.a[0] = buf(IN1, (u64)count * 0x28);
    c.b[0] = buf(OUT1, count);
    int rc = worker->dispatch(worker, &c);
    if (!rc) *processed = snx_ld_le64(c.out);
    return rc;
}
/* A sparse region (214) reserves GPU addresses that only its sparse maps (216) fill, all requests of a batch or none,
   until 215 frees it. The SDK waits for the notification after each */
static void test_gsv_sparse(SvcObj* worker, KEvent* event, u32 as, u32 device, u32 memory, u64 mapped) {
    u64 region = VA_BASE + 0x800000, processed = 0;
    u8 raw[40] = {0};
    snx_st_le32(raw, as);
    snx_st_le64(raw + 4, region);
    snx_st_le64(raw + 12, region + 0xFFFF);
    snx_st_le32(raw + 20, 0x60C); /* page shift 12, and a byte the SDK leaves dirty */
    snx_st_le32(raw + 28, 0xAABBCC00 | device);
    IpcCtx c = {.tipc = 1, .cmd = 214, .raw = raw, .raw_size = sizeof raw};
    kevent_clear(event);
    CHECK(worker->dispatch(worker, &c) == 0 && event->signaled);
    kevent_clear(event);
    CHECK(worker->dispatch(worker, &c) == RC(1, 106) && event->signaled); /* the same addresses again */
    snx_st_le64(raw + 4, mapped);
    snx_st_le64(raw + 12, mapped + 0xFFFF);
    CHECK(worker->dispatch(worker, &c) == RC(1, 106)); /* over a normal map */
    snx_st_le64(raw + 4, region + 0x10000);
    snx_st_le64(raw + 12, region + 0x17FFF);
    snx_st_le32(raw + 20, 16);
    CHECK(worker->dispatch(worker, &c) == RC(1, 101)); /* not whole 64 KiB pages */
    nn_gsv_GpuMapRequest map = {.address_space = as, .memory = memory, .device = device, .fallback_kind = 6};
    snx_st_le64(map.first, region + 0x4000);
    snx_st_le64(map.last, region + 0x4FFF);
    u32 selected = 0;
    CHECK(nn_gsv_IGpuWorker_NormalMapGpuMemory4K(worker, &map, &selected) == RC(1, 106));

    /* the later request of a batch wins where they overlap */
    test_sparse_request(0, region + 0x4000, 0x2000, memory, 0x1000, 6);
    test_sparse_request(1, region + 0x5000, 0x1000, 0, 0, 0);
    memset(GP(OUT1), 0xCC, 2);
    kevent_clear(event);
    CHECK(test_sparse_map(worker, as, 2, &processed) == 0 && processed == 2 && event->signaled);
    CHECK(GP(OUT1)[0] == 6 && GP(OUT1)[1] == 0);
    CHECK(gmmu_translate(region + 0x4000) == test_gpu_memory.addr + 0x1000 && !gmmu_translate(region + 0x5000));
    CHECK(!gmmu_translate(region) && !gmmu_translate(region + 0x6000));
    /* a request outside the region fails the batch before the first request changes anything */
    test_sparse_request(0, region + 0x8000, 0x1000, memory, 0, 6);
    test_sparse_request(1, region + 0xF000, 0x2000, memory, 0, 6);
    kevent_clear(event);
    CHECK(test_sparse_map(worker, as, 2, &processed) == RC(1, 106) && event->signaled);
    CHECK(!gmmu_translate(region + 0x8000));
    test_sparse_request(1, region + 0xF000, 0x1000, memory, test_gpu_memory.size, 6);
    CHECK(test_sparse_map(worker, as, 2, &processed) == RC(1, 101) && !gmmu_translate(region + 0x8000));
    test_sparse_request(0, region + 0x8000, 0x1000, memory, 0, 6);
    GP(IN1)[0x10] = 1; /* read-only: the page table can't keep it */
    CHECK(test_sparse_map(worker, as, 1, &processed) == RC(1, 33) && !gmmu_translate(region + 0x8000));

    /* a piece of a region is no normal map, and the region's destruction takes its pieces */
    nn_gsv_GpuUnmapRequest unmap = {.address_space = as};
    snx_st_le64(unmap.first, region + 0x4000);
    snx_st_le64(unmap.last, region + 0x4FFF);
    u64 unmapped = 0;
    CHECK(nn_gsv_IGpuWorker_NormalUnmapGpuMemory4K(worker, &unmap, &unmapped) == RC(1, 121));
    CHECK(nn_gsv_IGpuWorker_DestroySparseRegion(worker, &unmap) == RC(1, 121));
    CHECK(gmmu_translate(region + 0x4000) == test_gpu_memory.addr + 0x1000);
    snx_st_le64(unmap.first, region);
    snx_st_le64(unmap.last, region + 0xFFFF);
    kevent_clear(event);
    CHECK(nn_gsv_IGpuWorker_DestroySparseRegion(worker, &unmap) == 0 && event->signaled);
    CHECK(!gmmu_translate(region + 0x4000));
    CHECK(nn_gsv_IGpuWorker_DestroySparseRegion(worker, &unmap) == RC(1, 121));
    test_sparse_request(0, region + 0x4000, 0x1000, memory, 0, 6);
    CHECK(test_sparse_map(worker, as, 1, &processed) == RC(1, 106));
    /* one region and its piece stay for the worker's teardown */
    snx_st_le64(raw + 4, region);
    snx_st_le64(raw + 12, region + 0xFFFF);
    snx_st_le32(raw + 20, 12);
    CHECK(worker->dispatch(worker, &c) == 0);
    CHECK(test_sparse_map(worker, as, 1, &processed) == 0 && gmmu_translate(region + 0x4000) == test_gpu_memory.addr);
}
/* Exercise the worker's memory lifetime and VA bounds. The SDK leaves high bytes of its byte arguments dirty. */
static void test_gsv_worker_memory(SvcObj* control, SvcObj* worker, KEvent* event, NvFile* retained_view) {
    u32 device = 0, as = 0, selected = 0;
    g_hle.kernel.hw = SNX_HW_SWITCH2;
    CHECK(nn_gsv_IGpuSession_CreateGraphicsDevice(control, 1, 0, 0, &device) == 0);
    CHECK(nn_gsv_IGpuSession_CreateAddressSpace(control, 0, &as) == 0);
    CHECK(nv_gsv_register_as(as + 1) == RC(1, 121));
    CHECK(nv_gsv_register_as(as) == 0);
    u64 lo, hi, count;
    nn_gsv_GpuPartitionRanges ranges;
    CHECK(nn_gsv_IGpuSession_GetUserPartition(control, as, &lo, &hi) == 0 && lo < hi);
    CHECK(nn_gsv_IGpuSession_GetUserPartitions(control, as, &count, &ranges) == 0);
    CHECK(count == 1 && ranges.v[0] == lo && ranges.v[1] == hi);
    CHECK(nn_gsv_IGpuSession_GetGsvPartition(control, as, &lo, &hi) == 0 && lo > hi);

    NvFile view = {0};
    u8 arg[64] = {0}, regions[48] = {0};
    wr32(arg, AS_ALLOC_AS_EX_FLAGS, 5);
    wr32(arg, AS_ALLOC_AS_EX_AS_FD, as);
    wr32(arg, AS_ALLOC_AS_EX_BIG_PAGE_SIZE, 0x1000);
    wr64(arg, AS_ALLOC_AS_EX_VA_START, NV_GSV_USER_FIRST);
    wr64(arg, AS_ALLOC_AS_EX_VA_END, NV_GSV_USER_END);
    CHECK(ioctl_as(&view, 0x40284109, arg, 40, NULL, 0) == 0);
    CHECK(ioctl_as(&view, 0xC0404108, arg, 64, regions, sizeof regions) == 0);
    CHECK(rd32(arg, AS_GET_VA_REGIONS_BUF_SIZE) == 24);
    CHECK(rd64(regions, AS_VA_REGION_OFFSET) == NV_GSV_USER_FIRST);
    CHECK(rd64(regions, AS_VA_REGION_PAGES) * 0x1000 == NV_GSV_USER_END - NV_GSV_USER_FIRST);
    CHECK(ioctl_as(&view, 0x8010410B, arg, 16, NULL, 0) == 0);
    CHECK(rd64(arg, 0) >= NV_GSV_USER_END && rd32(arg, 8) == 4 && rd32(arg, 12) == NV_SYNCPT_SHIM_COUNT);
    g_hle.nvdrv.syncpt[191] = 0x12345678;
    CHECK(nv_syncpoint_shim_read(rd64(arg, 0) + 191 * 4 + 1, regions, 6));
    CHECK(regions[0] == 0x56 && regions[1] == 0x34 && regions[2] == 0x12 && regions[3] == 0 && regions[5] == 0);

    test_gpu_memory = (MemBlock){.addr = 0x30000, .size = 0x6000, .state = MS_NORMAL, .perm = PERM_RW};
    u8 raw[24] = {0};
    snx_st_le64(raw, test_gpu_memory.addr);
    snx_st_le64(raw + 8, test_gpu_memory.addr + test_gpu_memory.size - 1);
    snx_st_le32(raw + 16, 0xAABBCC00);
    snx_st_le32(raw + 20, 0xAABBCC00 | device);
    IpcCtx c = {.tipc = 1, .cmd = 101, .raw = raw, .raw_size = sizeof raw};
    CHECK(worker->dispatch(worker, &c) == RC(1, 106));
    test_gpu_memory.attr = MA_GPU_ACCESSIBLE;
    CHECK(worker->dispatch(worker, &c) == 0);
    u32 memory = snx_ld_le32(c.out);
    u64 gva = VA_BASE + 0x500000;
    nn_gsv_GpuMapRequest map = {.address_space = as, .memory = memory, .device = device, .kind = 8, .fallback_kind = 6};
    snx_st_le64(map.first, gva);
    snx_st_le64(map.last, gva + 0x1FFF);
    snx_st_le64(map.offset, 0x1000);
    kevent_clear(event);
    CHECK(nn_gsv_IGpuWorker_NormalMapGpuMemory4K(worker, &map, &selected) == 0 && selected == 6);
    CHECK(event->signaled && gmmu_translate(gva) == test_gpu_memory.addr + 0x1000 &&
          gmmu_translate(gva + 0x1FFF) == test_gpu_memory.addr + 0x2FFF);
    test_gsv_legacy_isolation(&view, gva);
    kevent_clear(event);
    CHECK(nn_gsv_IGpuWorker_DestroyGpuAccessibleMemory(worker, memory, 0) == RC(1, 122) && !event->signaled);
    map.address_space = as + 1;
    CHECK(nn_gsv_IGpuWorker_NormalMapGpuMemory4K(worker, &map, &selected) == RC(1, 121));
    CHECK(gmmu_translate(gva) == test_gpu_memory.addr + 0x1000);
    map.address_space = as;
    snx_st_le64(map.offset, test_gpu_memory.size);
    CHECK(nn_gsv_IGpuWorker_NormalMapGpuMemory4K(worker, &map, &selected) == RC(1, 101));

    nn_gsv_GpuModifyRequest modify = {.address_space = as, .kind = 9, .fallback_kind = 6};
    snx_st_le64(modify.first, gva);
    snx_st_le64(modify.last, gva + 0xFFF);
    CHECK(nn_gsv_IGpuWorker_NormalModifyGpuMemory4K(worker, &modify, &selected) == 0 && selected == 6);
    nn_gsv_GpuUnmapRequest unmap = {.address_space = as};
    snx_st_le64(unmap.first, gva);
    snx_st_le64(unmap.last, gva + 0xFFF);
    u64 unmapped = 0;
    CHECK(nn_gsv_IGpuWorker_NormalUnmapGpuMemory4K(worker, &unmap, &unmapped) == RC(1, 121));
    CHECK(gmmu_translate(gva) != 0);
    snx_st_le64(unmap.last, gva + 0x1FFF);
    CHECK(nn_gsv_IGpuWorker_NormalUnmapGpuMemory4K(worker, &unmap, &unmapped) == 0 && unmapped == 0x2000);
    CHECK(!gmmu_translate(gva) && !gmmu_translate(gva + 0x1FFF));
    CHECK(nn_gsv_IGpuWorker_DestroyGpuAccessibleMemory(worker, memory, 0) == 0 && event->signaled);
    kevent_clear(event);
    CHECK(nn_gsv_IGpuWorker_DestroyGpuAccessibleMemory(worker, memory, 0) == RC(1, 121) && !event->signaled);
    /* Leave a live registration and map for worker teardown after the control closes. */
    c = (IpcCtx){.tipc = 1, .cmd = 101, .raw = raw, .raw_size = sizeof raw};
    CHECK(worker->dispatch(worker, &c) == 0);
    map.memory = snx_ld_le32(c.out);
    snx_st_le64(map.offset, 0);
    CHECK(nn_gsv_IGpuWorker_NormalMapGpuMemory4K(worker, &map, &selected) == 0);
    test_gsv_sparse(worker, event, as, device, map.memory, gva);
    /* Native memory over that registration: backed at the guest address of its offset, held while an image lives */
    u32 native = 0, image = 0, second = 0, plain = nvmap_new(0x2000, 0x8000), shown = 0;
    const NvGsvImageShape shape = {0x003f003f, 4, 7, 1}; /* 64 x 64, 4 GOBs a row, 16-GOB blocks, format 1 */
    NvGsvImageShape got;
    CHECK(nv_native_memory_create(0x2000, &native) == 0);
    CHECK(nv_native_image_create(native, 0x2000, 1, &shape, &image) == RC(1, 121)); /* not backed yet */
    CHECK(nv_native_memory_bind(plain, test_gpu_memory.addr + 0x1000, 0x2000, map.memory, 0x1000) == RC(1, 121));
    CHECK(nv_native_memory_bind(native, test_gpu_memory.addr + 0x2000, 0x2000, map.memory, 0x1000) == RC(1, 106));
    CHECK(nv_native_memory_bind(native, test_gpu_memory.addr + 0x5000, 0x2000, map.memory, 0x5000) == RC(1, 106));
    CHECK(nv_native_memory_bind(native, test_gpu_memory.addr + 0x1000, 0x2000, map.memory, 0x1000) == 0);
    CHECK(nvmap_addr_by_id(native) == test_gpu_memory.addr + 0x1000);
    CHECK(nv_native_memory_bind(native, test_gpu_memory.addr + 0x1000, 0x2000, map.memory, 0x1000) == RC(1, 125));
    CHECK(nv_native_image_create(native, 0x2001, 1, &shape, &image) == RC(1, 101));
    CHECK(nv_native_image_create(native, 0x2000, 2, &shape, &image) == 0);
    CHECK(nv_native_image_create(native, 0x1000, 1, &shape, &second) == 0 && second == image + 2);
    CHECK(nv_native_memory_destroy(native) == RC(1, 122));
    CHECK(nv_native_image_destroy(image + 1) == RC(1, 121));
    /* a window's alias pool keeps its image until the window gives it back */
    CHECK(nv_native_image_window_pool(second, 2, 1) == RC(1, 33));
    CHECK(nv_native_image_window_pool(second, 0, 0) == RC(1, 125));
    CHECK(nv_native_window_image(second, &shown, &got) == RC(1, 121)); /* no window's yet */
    CHECK(nv_native_image_window_pool(second, 1, 1) == 0);
    CHECK(nv_native_window_image(second, &shown, &got) == 0 && shown == native && got.extent == shape.extent &&
          got.pitch_gobs == 4 && got.block_height_log2 == 7 && got.pixel_format == 1);
    CHECK(nv_native_image_window_pool(second, 1, 1) == RC(1, 125));
    CHECK(nv_native_image_destroy(second) == RC(1, 122));
    CHECK(nv_native_image_window_pool(second, 0, 0) == 0);
    CHECK(nv_native_image_destroy(image) == 0 && nv_native_image_destroy(second) == 0);
    /* the last handles: a range past them is refused, so no image gets handle 0 */
    u32 last = g_hle.nvdrv.gsv_image_last;
    g_hle.nvdrv.gsv_image_last = UINT32_MAX - 1;
    CHECK(nv_native_image_create(native, 0x1000, 2, &shape, &image) == RC(1, 103));
    CHECK(nv_native_image_create(native, 0x1000, 1, &shape, &image) == 0 && image == UINT32_MAX);
    CHECK(nv_native_image_create(native, 0x1000, 1, &shape, &second) == RC(1, 103));
    CHECK(nv_native_image_destroy(image) == 0);
    g_hle.nvdrv.gsv_image_last = last;
    CHECK(nv_native_memory_destroy(native) == 0 && !nvmap_addr_by_id(native));
    CHECK(nv_native_memory_destroy(native) == RC(1, 121));
    CHECK(nn_gsv_IGpuSession_DestroyGraphicsDevice(control, (u8)device) == 0);
    test_gpu_memory = (MemBlock){0};
    g_hle.kernel.hw = SNX_HW_SWITCH;
    CHECK(!nv_syncpoint_shim_read(NV_SYNCPT_SHIM_GVA, regions, 4));
    *retained_view = view;
}
/* GPU controls retain one worker session. Both share notification state, which lives until the last owner closes. */
static void test_gsv_gpu(void) {
    SvcObj* s = nv_gpu_new();
    SvcObj* other = nv_gpu_new();
    u32 first, last, handle, duplicate, peer, upper;
    CHECK(nn_gsv_IGpuSession_GetNotificationRange(s, &first, &last) == 0);
    CHECK(nn_gsv_IGpuSession_GetNotificationEvent(s, last + 1, &handle) == RC(1, 116));
    CHECK(nn_gsv_IGpuSession_GetNotificationEvent(s, first, &handle) == 0);
    CHECK(nn_gsv_IGpuSession_GetNotificationEvent(s, first, &duplicate) == 0);
    CHECK(nn_gsv_IGpuSession_GetNotificationEvent(s, last, &upper) == 0);
    CHECK(nn_gsv_IGpuSession_GetNotificationEvent(other, first, &peer) == 0);
    KEvent* event = (KEvent*)handle_object(handle);
    CHECK(handle_object(duplicate) == &event->o && handle_object(peer) != &event->o);
    CHECK(!event->signaled && !((KEvent*)handle_object(upper))->signaled);
    u8 raw[4];
    snx_st_le32(raw, first);
    IpcCtx c = {.tipc = 1, .cmd = 22};
    CHECK(s->dispatch(s, &c) == RC(1, 125) && c.tipc_error_moves == 1 && !event->signaled);
    c = (IpcCtx){.tipc = 1, .cmd = 21, .raw = raw, .raw_size = sizeof raw};
    snx_st_le32(raw, last + 1);
    CHECK(s->dispatch(s, &c) == RC(1, 116) && !event->signaled);
    snx_st_le32(raw, first + 1);
    CHECK(s->dispatch(s, &c) == RC(1, 125) && !event->signaled);
    snx_st_le32(raw, first);
    NvGsvGpu* gpu = s->data;
    u32 devices = gpu->devices;
    test_handles_full = 1;
    CHECK(s->dispatch(s, &c) == RC(1, 105) && !event->signaled && !gpu->worker);
    CHECK(g_hle.nvdrv.gsv_devices[devices - 1].refs == 1);
    for (u32 i = 0; i < NV_GSV_GPUS; ++i) CHECK(!g_hle.nvdrv.gsv_workers[i].obj);
    test_handles_full = 0;
    CHECK(s->dispatch(s, &c) == 0 && event->signaled);
    CHECK(g_hle.nvdrv.gsv_devices[devices - 1].refs == 2);
    c = (IpcCtx){.tipc = 1, .cmd = 22};
    CHECK(s->dispatch(s, &c) == 0 && c.nout_move == 1);
    u32 worker_handle = c.out_move[0];
    KSession* worker = (KSession*)handle_object(worker_handle);
    CHECK(worker->o.type == KO_SESSION && kobj_id(&worker->o) == gpu->worker);
    CHECK(((NvGsvWorker*)worker->obj->data)->devices == devices);
    kevent_clear(event);
    c = (IpcCtx){.tipc = 1, .cmd = 21, .raw = raw, .raw_size = sizeof raw};
    CHECK(s->dispatch(s, &c) == 0 && event->signaled && kobj_at(gpu->worker) == &worker->o);
    c = (IpcCtx){.tipc = 1, .cmd = 22};
    CHECK(s->dispatch(s, &c) == 0 && handle_object(c.out_move[0]) == &worker->o);
    h_close(c.out_move[0]);
    test_handles_full = 1;
    c = (IpcCtx){.tipc = 1, .cmd = 22};
    CHECK(s->dispatch(s, &c) == RC(1, 105) && kobj_at(gpu->worker) == &worker->o);
    test_handles_full = 0;
    NvFile view = {0};
    test_gsv_worker_memory(s, worker->obj, event, &view);
    kevent_clear(event);
    svcobj_unref(s);
    svcobj_unref(other);
    CHECK(worker->o.refs == 1 && g_hle.nvdrv.gsv_devices[devices - 1].refs == 1);
    CHECK(event->o.refs == 3 && !event->signaled);
    CHECK(handle_object(upper)->refs == 2 && handle_object(peer)->refs == 1);
    c = (IpcCtx){.cmd = 101};
    CHECK(worker->obj->dispatch(worker->obj, &c) == RC(1, 33) && !event->signaled);
    c = (IpcCtx){.tipc = 1, .cmd = 999};
    CHECK(worker->obj->dispatch(worker->obj, &c) == RC(10, 221) && !event->signaled);
    h_close(worker_handle);
    CHECK(!g_hle.nvdrv.gsv_devices[devices - 1].refs);
    CHECK(!gmmu_translate(VA_BASE + 0x500000) && !gmmu_translate(VA_BASE + 0x804000));
    for (u32 i = 0; i < NV_GSV_SPARSE_REGIONS; ++i) CHECK(!g_hle.nvdrv.gsv_sparse[i].devices);
    CHECK(!g_hle.nvdrv.gsv_as[0].devices);
    for (u32 i = 0; i < NV_GSV_MEMORIES; ++i) CHECK(!g_hle.nvdrv.gsv_mem[i].devices);
    CHECK(event->o.refs == 2 && handle_object(upper)->refs == 1);
    s = nv_gpu_new();
    devices = ((NvGsvGpu*)s->data)->devices;
    g_hle.kernel.hw = SNX_HW_SWITCH2;
    u8 view_arg[64] = {0};
    CHECK(ioctl_as(&view, NVHOST_AS_GPU_GET_VA_REGIONS, view_arg, sizeof view_arg, NULL, 0) == NV_RESULT_INVALID_STATE);
    CHECK(ioctl_as(&view, NVHOST_AS_GPU_SYNCPOINT_SHIM, view_arg, sizeof view_arg, NULL, 0) == NV_RESULT_INVALID_STATE);
    /* Exercise the last nonzero ID, then exhaustion rather than wrapping to the old view's ID. */
    g_hle.nvdrv.gsv_as_last = UINT32_MAX - 1;
    u32 next_as = 0;
    CHECK(nn_gsv_IGpuSession_CreateAddressSpace(s, 0, &next_as) == 0 && next_as == UINT32_MAX);
    CHECK(nv_gsv_register_as(view.view_as) == RC(1, 121));
    CHECK(nv_gsv_register_as(next_as) == 0);
    CHECK(ioctl_as(&view, NVHOST_AS_GPU_GET_VA_REGIONS, view_arg, sizeof view_arg, NULL, 0) == NV_RESULT_INVALID_STATE);
    NvFile new_view = {0};
    wr32(view_arg, AS_ALLOC_AS_EX_FLAGS, 5);
    wr32(view_arg, AS_ALLOC_AS_EX_AS_FD, next_as);
    wr32(view_arg, AS_ALLOC_AS_EX_BIG_PAGE_SIZE, 0x1000);
    wr64(view_arg, AS_ALLOC_AS_EX_VA_START, NV_GSV_USER_FIRST);
    wr64(view_arg, AS_ALLOC_AS_EX_VA_END, NV_GSV_USER_END);
    CHECK(ioctl_as(&new_view, NVHOST_AS_GPU_ALLOC_AS_EX, view_arg, 40, NULL, 0) == 0);
    CHECK(ioctl_as(&new_view, NVHOST_AS_GPU_SYNCPOINT_SHIM, view_arg, 16, NULL, 0) == 0);
    CHECK(nn_gsv_IGpuSession_GetNotificationEvent(s, first, &handle) == 0);
    event = (KEvent*)handle_object(handle);
    c = (IpcCtx){.tipc = 1, .cmd = 21, .raw = raw, .raw_size = sizeof raw};
    CHECK(s->dispatch(s, &c) == 0 && event->signaled);
    svcobj_unref(s); /* no client acquired the worker, so the control owned its last session reference */
    CHECK(!g_hle.nvdrv.gsv_devices[devices - 1].refs && event->o.refs == 1);
    CHECK(ioctl_as(&new_view, NVHOST_AS_GPU_SYNCPOINT_SHIM, view_arg, 16, NULL, 0) == NV_RESULT_INVALID_STATE);
    s = nv_gpu_new();
    CHECK(nn_gsv_IGpuSession_CreateAddressSpace(s, 0, &next_as) == RC(1, 103));
    svcobj_unref(s);
    g_hle.kernel.hw = SNX_HW_SWITCH;
}
static u32 test_tmem(u64 addr, u64 size) {
    KTmem* t = calloc(1, sizeof *t);
    CHECK(t != NULL);
    t->o.type = KO_TMEM;
    t->addr = addr;
    t->size = size;
    test_kobjs++;
    return h_new(&t->o);
}
/* The SDK toggles slot/group bits, signals the writable request and waits for completion before reading counters.
   An allocation must replace the all-complete sentinel without waiting for a request. */
static void test_gsv_cache(void) {
    u32 rw_handle = test_tmem(0x8000, 0x1000), ro_handle = test_tmem(0x9000, 0x2000);
    u32 request = 0, completed = 0;
    u32 id = syncpt_alloc(), group = id / 64;
    g_hle.kernel.hw = SNX_HW_SWITCH2;
    g_hle.nvdrv.syncpt[id] = 0xFFFFFFFEu;
    CHECK(nv_syncpoint_cache_bind(0, ro_handle, &request, &completed) == RC(1, 114));
    CHECK(nv_syncpoint_cache_bind(rw_handle, rw_handle, &request, &completed) == RC(1, 114));
    CHECK(nv_syncpoint_cache_bind(ro_handle, rw_handle, &request, &completed) == RC(1, 101));
    CHECK(!g_hle.nvdrv.gsv_cache.rw);
    CHECK(nv_syncpoint_cache_bind(rw_handle, ro_handle, &request, &completed) == 0);
    CHECK(gr32(0x9000) == 1 && gr32(0x9008) == NV_GSV_COUNTERS - 1);
    CHECK(gr64(0x9040 + id * 8) == 0xFFFFFFFEull && gr64(0x9040 + 191 * 8) == ~0ull);
    KWEvent* write_event = (KWEvent*)handle_object(request);
    KEvent* req = (KEvent*)kobj_at(write_event->r);
    KEvent* done = (KEvent*)handle_object(completed);
    CHECK(write_event->o.type == KO_WEVENT && done->o.type == KO_EVENT);
    u32 duplicate_request = 0, duplicate_completed = 0;
    CHECK(nv_syncpoint_cache_bind(rw_handle, ro_handle, &duplicate_request, &duplicate_completed) == RC(1, 125));
    kevent_signal(req); /* even an unchanged request must finish the event handshake */
    nv_syncpoint_cache_request(g_hle.nvdrv.gsv_cache.request);
    CHECK(!req->signaled && done->signaled);
    kevent_clear(done);
    h_close(rw_handle);
    h_close(ro_handle); /* the application still owns both mappings */
    nv_syncpoint_cache_request(kobj_id(&done->o));
    CHECK(!done->signaled);
    syncpt_add(id, 3);
    CHECK(gr64(0x9040 + id * 8) == 0xFFFFFFFEull); /* stale until the requested observation */
    gw64(0x8008 + group * 8, 1ull << (id % 64));
    gw64(0x8000, 1ull << group);
    kevent_signal(req);
    nv_syncpoint_cache_request(kobj_id(&req->o));
    CHECK(!req->signaled && done->signaled);
    CHECK(gr64(0x9040 + id * 8) == 0x100000001ull);
    CHECK(gr64(0x9640) == (1ull << group) && gr64(0x9648 + group * 8) == (1ull << (id % 64)));
    kevent_clear(done);
    syncpt_incr(id);
    for (u32 repeat = 0; repeat < 2; ++repeat) {
        u64 requests = gr64(0x8000);
        if (repeat) {
            gw64(0x8000, requests ^ (1ull << group));
            gw64(0x8000, requests); /* two toggles cancel before the request is processed */
        }
        kevent_signal(req);
        nv_syncpoint_cache_request(kobj_id(&req->o));
        CHECK(!req->signaled && done->signaled);
        CHECK(gr64(0x9640) == requests && gr64(0x9648 + group * 8) == (1ull << (id % 64)));
        CHECK(gr64(0x9040 + id * 8) == 0x100000001ull); /* no requested observation */
        kevent_clear(done);
    }
    gw64(0x8008 + group * 8, 0);
    gw64(0x8000, 0);
    nv_syncpoint_cache_request(kobj_id(&req->o));
    CHECK(done->signaled && gr64(0x9040 + id * 8) == 0x100000002ull && !gr64(0x9640));
    syncpt_free(id);
    CHECK(gr64(0x9040 + id * 8) == ~0ull);
    CHECK(syncpt_alloc() == id && gr64(0x9040 + id * 8) == 0x100000002ull);
    gw64(0x8018, 1ull << 63);
    gw64(0x8000, 4);
    nv_syncpoint_cache_request(kobj_id(&req->o));
    CHECK(gr64(0x9658) == (1ull << 63) && gr64(0x9040 + 191 * 8) == ~0ull);
    NvGsvCache* c = &g_hle.nvdrv.gsv_cache;
    kobj_unref(kobj_at(c->rw));
    kobj_unref(kobj_at(c->ro));
    kobj_unref(kobj_at(c->request));
    kobj_unref(kobj_at(c->completed));
    memset(c, 0, sizeof *c);
    syncpt_free(id);
    g_hle.kernel.hw = SNX_HW_SWITCH;
}
int main(void) {
    hle_init();
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    nv_init();
    {
        u8 request[40] = {0};
        u32 size = 0x2000, handle, align = 0x1000;
        u64 addr = 0x2000;
        snx_st_le32(request, size);
        CHECK(ioctl_nvmap(0x01, request, sizeof request) == 0);
        handle = snx_ld_le32(request + 4);
        CHECK(handle != 0);
        u64 mapped;
        u32 mapped_size;
        CHECK(!nvmap_get(handle, &mapped, &mapped_size));
        snx_st_le32(request, handle);
        snx_st_le32(request + 12, align);
        snx_st_le64(request + 24, addr);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 0);
        CHECK(nvmap_get(handle, &mapped, &mapped_size) && mapped == addr && mapped_size == size);
        addr = AS_SIZE;
        snx_st_le64(request + 24, addr);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 4);
        CHECK(nvmap_get(handle, &mapped, &mapped_size) && mapped == 0x2000);
        addr = 0x2001;
        snx_st_le64(request + 24, addr);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 4);
    }
    {
        /* A valid aligned allocation must not permit an unaligned MAP offset to expose host memory
           beyond the guest reservation through the rest of a published PTE. */
        u8 request[40] = {0};
        u32 size = 0x1000, handle, flags = 1;
        u64 addr = AS_SIZE - 0x1000, offset = 0xFFE, bytes = 1, gva = VA_BASE;
        snx_st_le32(request, size);
        CHECK(ioctl_nvmap(0x01, request, sizeof request) == 0);
        handle = snx_ld_le32(request + 4);
        snx_st_le32(request, handle);
        snx_st_le64(request + 24, addr);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 0);
        memset(request, 0, sizeof request);
        snx_st_le32(request, flags);
        snx_st_le32(request + 8, handle);
        snx_st_le64(request + 16, offset);
        snx_st_le64(request + 24, bytes);
        snx_st_le64(request + 32, gva);
        CHECK(as_call(0x06, request, sizeof request) == 4);
        CHECK(!gmmu_translate(gva) && !gmmu_translate(gva + 0xFFF));
        offset = 0;
        snx_st_le64(request + 16, offset);
        gva++;
        snx_st_le64(request + 32, gva);
        CHECK(as_call(0x06, request, sizeof request) == 4);
        gva--;
        snx_st_le64(request + 32, gva);
        bytes = 0x1001;
        snx_st_le64(request + 24, bytes);
        CHECK(as_call(0x06, request, sizeof request) == 4);
        CHECK(!gmmu_translate(gva));
        bytes = 1;
        snx_st_le64(request + 24, bytes);
        CHECK(as_call(0x06, request, sizeof request) == 0);
        CHECK(gmmu_translate(gva) == addr && gmmu_translate(gva + 0xFFF) == AS_SIZE - 1);
    }
    test_nvmap_as();
    test_gmmu_freed();
    test_gmmu_runs();
    test_gmmu_aliases();
    nvdrv = new_nvdrv();
    u32 error;

    /* bad fds: never opened, past the table, and the null fd 0 */
    CHECK(nv_ioctl1(7, 0xC0044801u, 4) == 8);
    CHECK(nv_ioctl1(0xFFFF, 0xC0044801u, 4) == 8);
    CHECK(nv_ioctl1(0, 0xC0044801u, 4) == 8);
    request(11, 9, SUBMIT_GPFIFO_EX);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 8);
    request(12, 9, GET_CHARACTERISTICS);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 8);

    u32 gpu = nv_open("/dev/nvhost-gpu", &error);
    CHECK(!error && gpu == 1);
    u32 ctrlgpu = nv_open("/dev/nvhost-ctrl-gpu", &error);
    CHECK(!error && ctrlgpu == 2);
    u32 ctrl = nv_open("/dev/nvhost-ctrl", &error);
    CHECK(!error && ctrl == 3);
    test_ioctl2(gpu);
    test_ioctl3(ctrlgpu);
    test_ctrlgpu_switch2();
    test_ctrl_syncpoints(ctrl);
    test_ctrl_get_config(ctrl);
    test_ctrl_event_fds(ctrl);
    test_quiet_increments(ctrl);

    /* a channel's own events (1 to 3), made on first query and kept by the fd; the ctrl fd's ids name event slots */
    KEvent* sm = query_event(gpu, 1);
    CHECK(query_event(gpu, 1) == sm && query_event(gpu, 3) != sm);
    KEvent* slot = query_event(ctrl, 0x10000005u);
    CHECK(slot == kev(CTRL_EV(ctrl)[5].ev) && query_event(ctrl, 5) == slot);

    /* closing the GPU channel retires its syncpoint once the GPU thread has run what was queued on it: a call after
       the channel's jobs */
    u32 sp = test_gpu.channel, calls = test_gpu.calls;
    CHECK(nv_close(gpu) == 0 && test_gpu.closed_channel == sp && !g_hle.nvdrv.syncpt_used[sp] && !g_hle.nvdrv.nvf[gpu].dev);
    CHECK(test_gpu.calls == calls + 1 && test_gpu.ncall_chans == 1 && test_gpu.call_chans[0] == sp);
    CHECK(sm->o.refs == 2); /* the game's two handles keep it, the fd's reference is gone */
    CHECK(nv_ioctl1(gpu, SUBMIT_GPFIFO_EX, 0x18) == 8);
    CHECK(nv_close(gpu) == 8 && nv_close(0) == 8 && nv_close(0xFFFF) == 8); /* not open: as an ioctl to it */
    /* QueryEvent on an fd that isn't open: the bad-fd NvError, no handle, and no event slot of the ctrl device */
    CHECK(!CTRL_EV(ctrl)[6].ev);
    request(4, gpu, 6);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && reply(0) == 8 && ctx.nout_copy == 1 && ctx.out_copy[0] == 0);
    CHECK(!CTRL_EV(ctrl)[6].ev);
    test_close_during_ioctl();

    CHECK(nv_close(ctrlgpu) == 0 && nv_close(ctrl) == 0);
    test_tsg();
    test_gsv_syncpoints();
    test_submission_completion();
    test_gsv_cache();
    test_gsv_gpu();
    for (u32 h = 0x100; h < test_next_handle; h++) kobj_unref(test_handles[h - 0x100]);
    CHECK(test_kobjs == 0 && test_locks == 0);
    svcobj_unref(nvdrv);
    for (u32 i = 0; i < NV_GMMU_L1; i++) free(g_hle.nvdrv.gmmu[i]);
    free(g_hle.nvdrv.gmaps);
    free(g_hle.nvdrv.vafree);
    free(g_mem);
    printf("nvdrv_service: %u checks passed\n", test_checks);
    return 0;
}
