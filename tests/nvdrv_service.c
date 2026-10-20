/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nvdrv's IPC side (nvdrv.c) through its generated dispatcher: Ioctl2's and Ioctl3's inline buffers reaching the
   device, the NvError of a bad fd, QueryEvent, and a Close that comes while an ioctl is running on the fd. The GPU and
   host1x are stand-ins that record what nvdrv asks of them */
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
void (*g_gpu_mapping_begin)(u64, u64, int);
void (*g_gpu_mapping_end)(void);
static unsigned test_checks, test_kobjs;
static u32 test_next_handle = 0x100;

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
u64 plat_ns(void) { return 0; }
u64 guest_ns(void) { return 0; }
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
void svcobj_unref(SvcObj* obj) {
    if (!obj || --obj->refs) return;
    test_svcobj_data_free(obj);
    free(obj);
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
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
void svc_register(const char* name, SvcCtor ctor) {
    (void)name;
    (void)ctor;
}

/* kernel objects, counted; a handle takes a reference and names its object */
void kobj_ref(KObj* o) { o->refs++; }
void kobj_unref(KObj* o) {
    CHECK(o->refs > 0);
    if (--o->refs) return;
    free(o);
    test_kobjs--;
}
KEvent* kevent_new(const char* name) {
    KEvent* e = calloc(1, sizeof *e);
    CHECK(e != NULL);
    e->o.type = KO_EVENT;
    e->o.name = name;
    test_kobjs++;
    return e;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
void kevent_clear(KEvent* e) { e->signaled = 0; }
static KObj* test_handles[64];
u32 h_new(KObj* o) {
    CHECK(test_next_handle - 0x100 < 64);
    kobj_ref(o);
    test_handles[test_next_handle - 0x100] = o;
    return test_next_handle++;
}
static KObj* handle_object(u32 h) {
    CHECK(h >= 0x100 && h < test_next_handle);
    return test_handles[h - 0x100];
}

/* the GPU: submissions, calls, kicks and retired channels, recorded */
static struct {
    u64 entries[8];
    u32 n, channel, incs, wait_id, wait_val, expect, submits, closed_channel, calls, call_chans[4], ncall_chans, kicks;
} test_gpu;
void gpu_submit(const void* entries, u32 n, u32 channel, u32 incs, u32 wait_id, u32 wait_val, u32 expect) {
    CHECK(n <= 8);
    memcpy(test_gpu.entries, entries, n * sizeof *test_gpu.entries);
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
    if (n >= 4) memcpy(d, &cmd, 4);
    return 0;
}
void host1x_forget_handle(u32 h) { (void)h; }

/* guest memory: the request's buffers */
enum { IN0 = 0x1000, IN1 = 0x2000, OUT0 = 0x3000, OUT1 = 0x4000, PATH = 0x5000 };
static SvcObj* nvdrv;
static IpcCtx ctx;
static u32 raw[4];
/* the descriptors each command declares, empty: an AutoSelect buffer goes in both its lists (A and X for an input,
   B and C for an output), and the test fills in the ones it uses */
static void request(u32 cmd, u32 a0, u32 a1) {
    memset(&ctx, 0, sizeof ctx);
    raw[0] = a0;
    raw[1] = a1;
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
    u32 v;
    CHECK(off + 4 <= ctx.out_size);
    memcpy(&v, ctx.out + off, 4);
    return v;
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

/* Ioctl2 (11): SUBMIT_GPFIFO_EX's header in A0 or X0, the GPFIFO entries inline in A1 or X1 */
#define SUBMIT_GPFIFO_EX 0xC018481Bu
static void submit_header(u32 num, u32 flags) {
    u8 h[0x18];
    memset(h, 0, sizeof h);
    memcpy(h + 8, &num, 4);
    memcpy(h + 12, &flags, 4);
    memcpy(GP(IN0), h, sizeof h);
}
static void test_ioctl2(u32 gpu) {
    const u64 entries[3] = {0x0000000100200000ull, 0x0000000100300000ull, 0x0000000400400000ull};
    memcpy(GP(IN1), entries, sizeof entries);
    submit_header(3, 2); /* FenceIncrement: the host adds two increments after the entries */
    memset(GP(OUT0), 0xCC, 0x18);
    request(11, gpu, SUBMIT_GPFIFO_EX);
    ctx.a[0] = buf(IN0, 0x18);
    ctx.a[1] = buf(IN1, sizeof entries);
    ctx.b[0] = buf(OUT0, 0x18);
    CHECK(nvdrv->dispatch(nvdrv, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    CHECK(test_gpu.submits == 1 && test_gpu.n == 3 && !memcmp(test_gpu.entries, entries, sizeof entries));
    u32 sp = test_gpu.channel;
    CHECK(sp >= 20 && sp < SYNCPT_N && test_gpu.incs == 2 && test_gpu.expect == 2);
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
    memcpy(h + 8, &one, 4);
    memcpy(h + 12, &flags, 4);
    memcpy(h + 16, fence, 8);
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
    memcpy(h + 12, &flags, 4);
    memcpy(h + 16, fence, 8);
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
static void put(u32 off, u32 v) { memcpy(GP(IN0 + off), &v, 4); }
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
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && !CTRL_EV(ctrl)[9].ev->signaled);
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
    CHECK(!CTRL_EV(ctrl)[10].armed && !CTRL_EV(ctrl)[10].ev->signaled && syncpt_read(40) == 2);
    put(0, 40);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && !CTRL_EV(ctrl)[10].ev->signaled);
    put(0, 40);
    put(4, 4);
    put(8, 100);
    put(12, 10);
    CHECK(nv_ioctl1(ctrl, EVENT_WAIT_AS, 16) == 5 && !CTRL_EV(ctrl)[10].ev->signaled);
    put(0, 40);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && CTRL_EV(ctrl)[10].ev->signaled);
    put(0, 10);
    CHECK(nv_ioctl1(ctrl, 0xC004001Cu, 4) == 0 && !CTRL_EV(ctrl)[10].ev->signaled);
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
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && a->ev->signaled && !a->armed && !b->ev->signaled && b->armed);
    /* polling the other fd's fence leaves its armed wait, and this fd's signal, as they were */
    CHECK(event_wait(other, 41, base + 2, 0, 0) == 5 && b->armed && a->ev->signaled);
    CHECK(event_wait(ctrl, 41, base + 2, 0, 0) == 5 && a->ev->signaled && !a->armed);
    put(0, 41);
    CHECK(nv_ioctl1(ctrl, SYNCPT_INCR, 4) == 0 && b->ev->signaled && !b->armed);
    CHECK(nv_close(other) == 0 && !g_hle.nvdrv.ctrl_ev[other]); /* its slots go with it */
}

/* Ioctl3 (12): ctrl-gpu's GET_CHARACTERISTICS writes the characteristics to the inline output, B1 or C1 */
#define GET_CHARACTERISTICS 0xC0B04705u
static void test_ioctl3(u32 ctrlgpu) {
    u64 header[2] = {0xA0, 0x7000};
    memcpy(GP(IN0), header, sizeof header);
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
    memcpy(d, &size, 4);
    CHECK(nvmap_call(0x01, d) == 0);
    memcpy(&h, d + 4, 4);
    memset(d, 0, sizeof d);
    memcpy(d, &h, 4);
    memcpy(d + 24, &addr, 8);
    CHECK(nvmap_call(0x04, d) == 0);
    return h;
}
static u32 nvmap_param(u32 h, u32 param, u32* value) {
    u8 d[32] = {0};
    u32 r;
    memcpy(d, &h, 4);
    memcpy(d + 4, &param, 4);
    r = nvmap_call(0x09, d);
    memcpy(value, d + 8, 4);
    return r;
}
static u32 as_call(u32 cmd, u8* d, u32 n) { return ioctl_as(cmd, d, n, NULL, 0); }
static u64 as_map(u32 flags, u32 h, u64 size, u64 gva) {
    u8 d[40] = {0};
    memcpy(d, &flags, 4);
    memcpy(d + 8, &h, 4);
    memcpy(d + 24, &size, 8);
    memcpy(d + 32, &gva, 8);
    CHECK(as_call(0x06, d, sizeof d) == 0);
    memcpy(&gva, d + 32, 8);
    return gva;
}
static void test_nvmap_as(void) {
    u8 d[64] = {0};
    u32 h, again, v, one = 1;
    u64 gva, fixed, addr, size;
    /* CREATE and FROM_ID take a reference each, FREE gives one back; the number is handed out again after the last */
    h = nvmap_new(0x2000, 0x8000);
    memcpy(d, &h, 4);
    CHECK(nvmap_call(0x03, d) == 0 && !memcmp(d + 4, &h, 4)); /* FROM_ID: the id is the handle's number */
    memset(d, 0, sizeof d);
    memcpy(d, &h, 4);
    CHECK(nvmap_call(0x05, d) == 0); /* FREE reports the address, size and flags */
    memcpy(&addr, d + 8, 8);
    memcpy(&v, d + 16, 4);
    CHECK(addr == 0x8000 && v == 0x2000 && g_hle.nvdrv.nvh[h].alive);
    CHECK(nvmap_param(h, 4, &v) == 0 && v == 0x40000000u); /* Eden's heap for an allocated handle */
    CHECK(nvmap_param(h, 3, &v) == 0 && v == 0x8000);       /* Base: the address's low bits */
    CHECK(nvmap_param(h, 2, &v) == 0 && v == 0x1000);       /* no alignment given */
    CHECK(nvmap_param(h, 1, &v) == 0 && v == 0x2000);
    memset(d, 0, sizeof d);
    memcpy(d + 4, &h, 4);
    CHECK(nvmap_call(0x0E, d) == 0 && !memcmp(d, &h, 4)); /* GET_ID */
    memset(d, 0, sizeof d);
    memcpy(d, &h, 4);
    CHECK(nvmap_call(0x05, d) == 0 && !g_hle.nvdrv.nvh[h].alive);
    CHECK(nvmap_call(0x05, d) == 1 && nvmap_param(h, 1, &v) == 1);
    memset(d, 0, sizeof d);
    memcpy(d + 4, &h, 4);
    CHECK(nvmap_call(0x0E, d) == 1);
    memcpy(d, &h, 4);
    CHECK(nvmap_call(0x03, d) == 1); /* no live handle with that id */
    again = nvmap_new(0x3000, 0x9000);
    CHECK(again == h);

    /* ALLOC_SPACE: allocator VA at a multiple of the alignment, from VA_BASE up */
    memset(d, 0, sizeof d);
    v = 4;
    memcpy(d, &v, 4);
    v = 0x10000;
    memcpy(d + 4, &v, 4);
    CHECK(as_call(0x02, d, 24) == 0);
    memcpy(&gva, d + 16, 8);
    CHECK(gva >= VA_BASE && !(gva & 0xFFFF));
    v = 3; /* a page size that isn't a power of two */
    memcpy(d + 4, &v, 4);
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
    memcpy(d, &gva, 8);
    CHECK(as_call(0x05, d, 8) == 0);
    CHECK(!gmmu_translate(gva) && gmmu_translate(fixed) == 0xC000);
    CHECK(as_map(0, again, 0x1000, 0) == gva); /* first fit takes the freed VA again */
    /* FREE_SPACE clears every page of its range */
    memset(d, 0, sizeof d);
    memcpy(d, &gva, 8);
    v = 2;
    memcpy(d + 8, &v, 4);
    v = 0x1000;
    memcpy(d + 12, &v, 4);
    CHECK(as_call(0x03, d, 16) == 0 && !gmmu_translate(gva) && !gmmu_translate(fixed));
    addr = VA_END - 0x1000;
    memcpy(d, &addr, 8);
    CHECK(as_call(0x03, d, 16) == 4); /* past the 40-bit space */

    /* REMAP in big pages: map, a bad handle skipped, then handle 0 unmaps */
    memset(d, 0, sizeof d);
    v = (u32)((VA_BASE + 0x1000000) >> 16);
    memcpy(d + 4, &h, 4);
    memcpy(d + 12, &v, 4);
    memcpy(d + 16, &one, 4);
    v = 0x1FFF; /* no such handle */
    memcpy(d + 24, &v, 4);
    CHECK(as_call(0x14, d, 40) == 0 && gmmu_translate(VA_BASE + 0x1000000) == 0xC000);
    memset(d + 4, 0, 4);
    CHECK(as_call(0x14, d, 20) == 0 && !gmmu_translate(VA_BASE + 0x1000000));

    /* GET_VA_REGIONS: the big-page region starts where the allocator does */
    memset(d, 0, sizeof d);
    CHECK(as_call(0x08, d, 64) == 0);
    memcpy(&v, d + 8, 4);
    memcpy(&addr, d + 40, 8);
    memcpy(&size, d + 16, 8);
    CHECK(v == 48 && addr == VA_BASE && size == 0x10000ull << 10);
    memcpy(&v, d + 48, 4);
    CHECK(v == 0x10000);
    /* ALLOC_AS_EX: 0 is 128K big pages */
    memset(d, 0, sizeof d);
    CHECK(as_call(0x09, d, 24) == 0 && g_hle.nvdrv.big_page_bits == 17);
    v = 0x10000;
    memcpy(d + 8, &v, 4);
    CHECK(as_call(0x09, d, 24) == 0 && g_hle.nvdrv.big_page_bits == 16);
}

int kobj_never_signaled(KObj* o) {
    (void)o;
    return 0;
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
int main(void) {
    hle_init();
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    nv_init();
    {
        u8 request[40] = {0};
        u32 size = 0x2000, handle, align = 0x1000;
        u64 addr = 0x2000;
        memcpy(request, &size, 4);
        CHECK(ioctl_nvmap(0x01, request, sizeof request) == 0);
        memcpy(&handle, request + 4, 4);
        CHECK(handle != 0);
        u64 mapped;
        u32 mapped_size;
        CHECK(!nvmap_get(handle, &mapped, &mapped_size));
        memcpy(request, &handle, 4);
        memcpy(request + 12, &align, 4);
        memcpy(request + 24, &addr, 8);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 0);
        CHECK(nvmap_get(handle, &mapped, &mapped_size) && mapped == addr && mapped_size == size);
        addr = AS_SIZE;
        memcpy(request + 24, &addr, 8);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 4);
        CHECK(nvmap_get(handle, &mapped, &mapped_size) && mapped == 0x2000);
        addr = 0x2001;
        memcpy(request + 24, &addr, 8);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 4);
    }
    {
        /* A valid aligned allocation must not permit an unaligned MAP offset to expose host memory
           beyond the guest reservation through the rest of a published PTE. */
        u8 request[40] = {0};
        u32 size = 0x1000, handle, flags = 1;
        u64 addr = AS_SIZE - 0x1000, offset = 0xFFE, bytes = 1, gva = VA_BASE;
        memcpy(request, &size, 4);
        CHECK(ioctl_nvmap(0x01, request, sizeof request) == 0);
        memcpy(&handle, request + 4, 4);
        memcpy(request, &handle, 4);
        memcpy(request + 24, &addr, 8);
        CHECK(ioctl_nvmap(0x04, request, sizeof request) == 0);
        memset(request, 0, sizeof request);
        memcpy(request, &flags, 4);
        memcpy(request + 8, &handle, 4);
        memcpy(request + 16, &offset, 8);
        memcpy(request + 24, &bytes, 8);
        memcpy(request + 32, &gva, 8);
        CHECK(ioctl_as(0x06, request, sizeof request, NULL, 0) == 4);
        CHECK(!gmmu_translate(gva) && !gmmu_translate(gva + 0xFFF));
        offset = 0;
        memcpy(request + 16, &offset, 8);
        gva++;
        memcpy(request + 32, &gva, 8);
        CHECK(ioctl_as(0x06, request, sizeof request, NULL, 0) == 4);
        gva--;
        memcpy(request + 32, &gva, 8);
        bytes = 0x1001;
        memcpy(request + 24, &bytes, 8);
        CHECK(ioctl_as(0x06, request, sizeof request, NULL, 0) == 4);
        CHECK(!gmmu_translate(gva));
        bytes = 1;
        memcpy(request + 24, &bytes, 8);
        CHECK(ioctl_as(0x06, request, sizeof request, NULL, 0) == 0);
        CHECK(gmmu_translate(gva) == addr && gmmu_translate(gva + 0xFFF) == AS_SIZE - 1);
    }
    test_nvmap_as();
    test_gmmu_freed();
    test_gmmu_runs();
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
    test_ctrl_syncpoints(ctrl);
    test_ctrl_event_fds(ctrl);

    /* a channel's own events (1 to 3), made on first query and kept by the fd; the ctrl fd's ids name event slots */
    KEvent* sm = query_event(gpu, 1);
    CHECK(query_event(gpu, 1) == sm && query_event(gpu, 3) != sm);
    KEvent* slot = query_event(ctrl, 0x10000005u);
    CHECK(slot == CTRL_EV(ctrl)[5].ev && query_event(ctrl, 5) == slot);

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
    for (u32 h = 0x100; h < test_next_handle; h++) kobj_unref(test_handles[h - 0x100]);
    CHECK(test_kobjs == 0 && test_locks == 0);
    svcobj_unref(nvdrv);
    for (u32 i = 0; i < GL1; i++) free(g_hle.nvdrv.gmmu[i]);
    free(g_hle.nvdrv.gmaps);
    free(g_mem);
    printf("nvdrv_service: %u checks passed\n", test_checks);
    return 0;
}
