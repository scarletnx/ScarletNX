/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sfdnsres (dns.c) and nifm (nifm.c) through their generated dispatchers, with the network option off: what a console
   without a connection answers. bsd.c, built beside this file, says whether the console is online */
#include "nx/hle/services/network/network.h"
#include <stdarg.h>

/* the raw reads the generated unpackers make, recorded: the WithOptions commands' layout is what the SDK's client
   sends, and the handlers don't look at most of those arguments */
static struct {
    u32 off[8], size[8];
    unsigned n;
} test_reads;
static void test_ipc_in(IpcCtx* c, u32 off, void* dst, u32 n) {
    if (test_reads.n < 8) {
        test_reads.off[test_reads.n] = off;
        test_reads.size[test_reads.n] = n;
    }
    test_reads.n++;
    ipc_in(c, off, dst, n);
}
#define ipc_in test_ipc_in
#include "nx/hle/services/network/dns.c"
#include "nx/hle/services/network/nifm.c"
#include "nx/hle/hle.c"
#include "fake_room.h"
#undef ipc_in

u8* g_mem;
static unsigned test_checks, test_objects, test_kobjs;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "network_service.c:%d: check failed: %s\n", line, expression);
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
    if (obj->destroy) obj->destroy(obj);
    test_svcobj_data_free(obj);
    free(obj);
    test_objects--;
}
void out_obj(IpcCtx* c, SvcObj* obj) {
    CHECK(c->nout_objs < 8);
    c->out_objs[c->nout_objs++] = obj;
}
void out_copy_h(IpcCtx* c, u32 handle) {
    CHECK(c->nout_copy < 8);
    c->out_copy[c->nout_copy++] = handle;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
int ipc_warn_once(const char* name, u32 cmd) {
    (void)name, (void)cmd;
    return 0;
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
int quiet_ok(SvcObj* o, IpcCtx* c) { return stub_ok(o, c); }
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
    CHECK(e != NULL);
    kobj_set_name(&e->o, name);
    test_kobjs++;
    return e;
}
void kevent_signal(KEvent* e) { e->signaled = 1; }
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}
/* the reply's handles name their events: the test drops them like the game closing its handles */
static KEvent* test_handles[16];
static u32 test_nhandles;
u32 ev_handle(KEvent* e) {
    CHECK(test_nhandles < 16);
    kobj_ref(&e->o);
    test_handles[test_nhandles++] = e;
    return 0x100 + test_nhandles - 1;
}
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = ev_handle(e);
    return 0;
}

/* guest memory: the request's buffers */
enum { IN0 = 0x1000, IN1 = 0x2000, IN2 = 0x3000, OUT0 = 0x4000 };
static IpcCtx ctx;
static u8 raw[32];
/* the request is the guest's: little-endian */
static void arg32(unsigned i, u32 v) { snx_st_le32(raw + i * 4, v); }
static void request(u32 cmd) {
    memset(&ctx, 0, sizeof ctx);
    memset(raw, 0, sizeof raw);
    memset(&test_reads, 0, sizeof test_reads);
    ctx.cmd = cmd;
    ctx.raw = raw;
    ctx.raw_size = sizeof raw;
}
/* a resolver request: every one of them sends the pid, and each AutoSelect buffer goes in both its lists (A and X
   for an input, B and C for an output), the unused one empty. The test fills in the descriptors it uses */
static void resolver(u32 cmd) {
    request(cmd);
    ctx.has_pid = 1;
    ctx.pid = K_PROCESS_ID;
    ctx.na = cmd == 12 ? 4 : cmd == 10 || cmd == 11 ? 2 : 1; /* (2, 14) the name or the options in A */
    ctx.nx = cmd == 10 || cmd == 11 ? 2 : cmd == 12 ? 1 : 0;
    ctx.nb = cmd == 14 ? 0 : 1;
    ctx.nc = cmd == 10 || cmd == 11 || cmd == 12 ? 1 : 0;
}
static IpcBuf buf(u64 addr, u64 size) {
    IpcBuf b = {addr, size, 0};
    return b;
}
static u32 reply(u32 off) {
    CHECK(off + 4 <= ctx.out_size);
    return snx_ld_le32(ctx.out + off);
}
static void reads(unsigned n, const u32* expected) {
    CHECK(test_reads.n == n);
    for (unsigned i = 0; i < n; i++) CHECK(test_reads.off[i] == expected[i] && test_reads.size[i] == 4);
}
static IpcBuf text(u64 at, const char* s) {
    strcpy((char*)GP(at), s);
    return buf(at, strlen(s) + 1);
}

/* GetHostByNameRequestWithOptions (10): u32 use_nsd_resolve at 0, u32 cancel_handle at 4, the pid's slot at 8; the
   reply is the hostent's size, the netdb error and the errno, in that order (GetHostByNameRequest (2) has them the
   other way round) */
static void test_host_by_name(SvcObj* r) {
    static const u32 offsets[2] = {0, 4};
    resolver(10);
    arg32(0, 1);
    arg32(1, 7);
    arg32(2, 0xDEADBEEF); /* the pid's slot: the kernel's pid, not these bytes */
    ctx.x[0] = text(IN0, "127.0.0.1");
    ctx.x[1] = buf(IN1, 0); /* no options */
    ctx.c[0] = buf(OUT0, 0x400);
    memset(GP(OUT0), 0xCC, 0x400);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 12);
    reads(2, offsets);
    /* the name asked for, no aliases, AF_INET, 4 byte addresses, one of them: 127.0.0.1 passed through htonl again */
    u32 size = reply(0);
    CHECK(size == 10 + 4 + 2 + 2 + 4 + 4 && reply(4) == 0 && reply(8) == 0);
    const u8 tail[] = {0, 0, 0, 0, 0, 2, 0, 4, 0, 0, 0, 1, 1, 0, 0, 127};
    CHECK(!strcmp((char*)GP(OUT0), "127.0.0.1") && !memcmp(GP(OUT0) + 10, tail, sizeof tail));
    CHECK(GP(OUT0)[size] == 0xCC);
    /* offline a name can't be looked up: no size, NETDB_TRY_AGAIN, no errno */
    resolver(10);
    ctx.a[0] = text(IN0, "example.com");
    ctx.b[0] = buf(OUT0, 0x400);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 12);
    CHECK(reply(0) == 0 && reply(4) == NETDB_TRY_AGAIN && reply(8) == 0);
    /* the older command's order: netdb error, errno, size */
    resolver(2);
    ctx.a[0] = text(IN0, "127.0.0.1");
    ctx.b[0] = buf(OUT0, 0x400);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 12);
    CHECK(reply(0) == 0 && reply(4) == 0 && reply(8) == size);
}

/* GetHostByAddrRequestWithOptions (11): four u32s (address length, type, an unknown one, the cancel handle), the
   pid's slot at 16. Offline only loopback can be named */
static void test_host_by_addr(SvcObj* r) {
    static const u32 offsets[4] = {0, 4, 8, 12};
    static const u8 google[4] = {8, 8, 8, 8};
    resolver(11);
    arg32(0, 4);
    arg32(1, 2);
    arg32(3, 1);
    memcpy(GP(IN0), google, 4);
    ctx.a[0] = buf(IN0, 4);
    ctx.b[0] = buf(OUT0, 0x400);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 12);
    reads(4, offsets);
    CHECK(reply(0) == 0 && reply(4) == NETDB_HOST_NOT_FOUND && reply(8) == 0);
    /* loopback: the host names it (or not), and the size says whether it did */
    static const u8 lo[4] = {127, 0, 0, 1};
    memcpy(GP(IN0), lo, 4);
    resolver(11);
    arg32(0, 4);
    arg32(1, 2);
    ctx.x[0] = buf(IN0, 4);
    ctx.c[0] = buf(OUT0, 0x400);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 12 && reply(8) == 0);
    CHECK(reply(4) == 0 ? reply(0) > 16 && GP(OUT0)[0] : reply(4) == NETDB_HOST_NOT_FOUND && !reply(0));
}

/* GetAddrInfoRequestWithOptions (12): u32 use_nsd_resolve at 0, u32 cancel_handle at 4, the pid's slot at 8; the
   host, service and hints in A0 to A2, the options in A3 or X0, the response in B0 or C0. The reply: the response's
   size, the getaddrinfo error, the netdb error and the errno */
static void hints(u32 socktype) {
    u8 h[24] = {0xBE, 0xEF, 0xCA, 0xFE, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 0};
    h[15] = (u8)socktype;
    memcpy(GP(IN2), h, sizeof h);
}
static void test_addr_info(SvcObj* r) {
    static const u32 offsets[2] = {0, 4};
    hints(1); /* SOCK_STREAM */
    resolver(12);
    arg32(1, 3);
    ctx.a[0] = text(IN0, "127.0.0.1");
    ctx.a[1] = text(IN1, "80");
    ctx.a[2] = buf(IN2, 24);
    ctx.x[0] = buf(0x5000, 0);
    ctx.b[0] = buf(OUT0, 0x400);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 16);
    reads(2, offsets);
    CHECK(reply(0) == 0x28 + 1 + 4 && reply(4) == 0 && reply(8) == 0 && reply(12) == 0);
    const u8 entry[] = {0xBE, 0xEF, 0xCA, 0xFE, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 0, 1};
    CHECK(!memcmp(GP(OUT0), entry, sizeof entry));
    /* a service the host doesn't know: EAI_SERVICE, which the console answers with NETDB_SUCCESS and EINVAL */
    resolver(12);
    ctx.a[0] = text(IN0, "127.0.0.1");
    ctx.a[1] = text(IN1, "snx-no-such-service");
    ctx.a[2] = buf(IN2, 24);
    ctx.c[0] = buf(OUT0, 0x400);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 16);
    CHECK(reply(0) == 0 && reply(4) == SNX_EAI_SERVICE && reply(8) == NETDB_SUCCESS && reply(12) == SNX_EINVAL);
    /* offline: a name can't be looked up */
    resolver(12);
    ctx.a[0] = text(IN0, "example.com");
    ctx.a[1] = text(IN1, "443");
    ctx.a[2] = buf(IN2, 24);
    ctx.b[0] = buf(OUT0, 0x400);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 16);
    CHECK(reply(0) == 0 && reply(4) == SNX_EAI_AGAIN && reply(8) == NETDB_TRY_AGAIN && reply(12) == 0);
}

/* nifm IGeneralService with the network off */
static void test_nifm(void) {
    SvcObj* nifm = new_nifm();
    request(5); /* CreateGeneralService(pid) */
    ctx.has_pid = 1;
    ctx.pid = K_PROCESS_ID;
    CHECK(nifm->dispatch(nifm, &ctx) == 0 && ctx.nout_objs == 1);
    SvcObj* g = ctx.out_objs[0];
    request(17); /* IsWirelessCommunicationEnabled */
    CHECK(g->dispatch(g, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    request(20); /* IsEthernetCommunicationEnabled */
    CHECK(g->dispatch(g, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    request(22); /* IsAnyForegroundRequestAccepted */
    CHECK(g->dispatch(g, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0);
    /* GetInternetConnectionStatus fails as GetCurrentIpAddress does without a connection, and answers nothing */
    request(18);
    CHECK(g->dispatch(g, &ctx) == RC(110, 300) && ctx.out_size == 0);
    request(12);
    CHECK(g->dispatch(g, &ctx) == RC(110, 300) && ctx.out_size == 0);
    /* a request is refused: its state event is signaled at once and its state is Invalid */
    request(4);
    CHECK(g->dispatch(g, &ctx) == 0 && ctx.nout_objs == 1);
    SvcObj* req = ctx.out_objs[0];
    request(4); /* Submit */
    CHECK(req->dispatch(req, &ctx) == 0);
    request(0); /* GetRequestState */
    CHECK(req->dispatch(req, &ctx) == 0 && ctx.out_size == 4 && reply(0) == REQ_INVALID);
    request(1); /* GetResult */
    CHECK(req->dispatch(req, &ctx) == RC(110, 300));
    request(2); /* GetSystemEventReadableHandles */
    CHECK(req->dispatch(req, &ctx) == 0 && ctx.nout_copy == 2);
    KEvent* state = test_handles[ctx.out_copy[0] - 0x100];
    CHECK(state->signaled && !test_handles[ctx.out_copy[1] - 0x100]->signaled);
    /* a Submit offline signals the state event again, for the SDK to read the refusal */
    state->signaled = 0;
    request(4);
    CHECK(req->dispatch(req, &ctx) == 0 && state->signaled);
    request(0);
    CHECK(req->dispatch(req, &ctx) == 0 && reply(0) == REQ_INVALID);
    svcobj_unref(req);
    svcobj_unref(g);
    svcobj_unref(nifm);
}

/* in an Eden room the console's connection is the room's: nifm gives its address and says wireless is on, with the
   network option off; airplane mode takes both away */
static void test_nifm_room(void) {
    FakeRoom* room = fake_room_start();
    SnxRoom* r;
    SvcObj *nifm = new_nifm(), *g;
    int why;
    CHECK(snx_room_join(&r, "127.0.0.1", fake_room_port(room), "Aogo", "secret", "game", 1, "", &why) == 0);
    network_set_room(r);
    request(5);
    ctx.has_pid = 1;
    ctx.pid = K_PROCESS_ID;
    CHECK(nifm->dispatch(nifm, &ctx) == 0 && ctx.nout_objs == 1);
    g = ctx.out_objs[0];
    request(12);                                                                     /* GetCurrentIpAddress */
    CHECK(g->dispatch(g, &ctx) == 0 && ctx.out_size == 4 && reply(0) == 0x0101A8C0); /* 192.168.1.1 */
    request(17); /* IsWirelessCommunicationEnabled */
    CHECK(g->dispatch(g, &ctx) == 0 && reply(0) == 1);
    g_settings.airplane_mode = 1;
    request(17);
    CHECK(g->dispatch(g, &ctx) == 0 && reply(0) == 0);
    request(12);
    CHECK(g->dispatch(g, &ctx) == RC(110, 300));
    g_settings.airplane_mode = 0;
    svcobj_unref(g);
    svcobj_unref(nifm);
    network_set_room(NULL);
    snx_room_leave(r);
    fake_room_stop(room);
}


int main(void) {
    hle_init();
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    network_init();
    snx_net_set_log(NULL);
    CHECK(!network_online());
    /* airplane mode is offline with the network option on too: no connection, only the loopback reachable */
    {
        SnxSockAddr out = {SNX_AF_INET, 0, 0, {8, 8, 8, 8}, 0}, lo = {SNX_AF_INET, 0, 0, {127, 0, 0, 1}, 0};
        g_settings.network = 1;
        g_settings.airplane_mode = 1;
        CHECK(!network_online() && !network_reachable(&out) && network_reachable(&lo));
        test_nifm();
        g_settings.airplane_mode = 0;
        CHECK(network_reachable(&out));
        g_settings.network = 0;
    }
    SvcObj* r = new_sfdnsres();
    test_host_by_name(r);
    test_host_by_addr(r);
    test_addr_info(r);
    /* ResolverSetOptionRequest (14) is a stub: success, a result of 0 and no errno */
    resolver(14);
    arg32(0, 0x12345678);
    ctx.a[0] = buf(IN0, 16);
    CHECK(r->dispatch(r, &ctx) == 0 && ctx.out_size == 8 && reply(0) == 0 && reply(4) == 0);
    svcobj_unref(r);
    test_nifm();
    test_nifm_room();
    for (u32 i = 0; i < test_nhandles; i++) kobj_unref(&test_handles[i]->o);
    CHECK(test_objects == 0 && test_kobjs == 0);
    free(g_mem);
    printf("network_service: %u checks passed\n", test_checks);
    return 0;
}
