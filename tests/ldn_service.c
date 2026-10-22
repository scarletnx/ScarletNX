/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* ldn:u (ldn.c) through its generated dispatcher, with the kernel stubbed: two IUserLocalCommunicationService in
   one process, each a console on the host's loopback (127.0.0.1 and 127.0.0.2), one creating a network and the
   other scanning for it and joining, with the states, the results, the state change events and what the nodes say.
   bsd.c is built beside, for the address a game socket binds while ldn is up */
#include "nx/hle/services/network/network.h"
#include <stdarg.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "nx/hle/services/network/ldn.c"
#include "nx/hle/hle.c"
#include "fake_room.h"

u8* g_mem;
int g_opt_ldn;
static unsigned test_checks, test_objects, test_kobjs;
static u32 test_next_handle = 0x100;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "ldn_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

/* real mutexes: the transport's thread signals the events under the service's lock */
#ifdef _WIN32
void pm_init(PMutex* m) { InitializeSRWLock(&m->l); }
void pm_lock(PMutex* m) { AcquireSRWLockExclusive(&m->l); }
void pm_unlock(PMutex* m) { ReleaseSRWLockExclusive(&m->l); }
#else
void pm_init(PMutex* m) { pthread_mutex_init(&m->l, NULL); }
void pm_lock(PMutex* m) { pthread_mutex_lock(&m->l); }
void pm_unlock(PMutex* m) { pthread_mutex_unlock(&m->l); }
#endif
/* memory/own.c: nothing is GPU-owned here */
void own_acquire(u64 addr, u64 size) { (void)addr, (void)size; }
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

/* kernel objects: a handle takes a reference, and the test counts what is alive */
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
    kobj_set_name(&e->o, name);
    test_kobjs++;
    return e;
}
void kevent_signal(KEvent* e) { __atomic_store_n(&e->signaled, 1, __ATOMIC_RELEASE); }
static KObj* test_handles[64];
u32 h_new(KObj* o) {
    CHECK(test_next_handle - 0x100 < 64);
    kobj_ref(o);
    test_handles[test_next_handle - 0x100] = o;
    return test_next_handle++;
}
static KEvent* handle_event(u32 h) {
    CHECK(h >= 0x100 && h < test_next_handle);
    return (KEvent*)test_handles[h - 0x100];
}
static void close_handles(void) {
    for (u32 h = 0x100; h < test_next_handle; h++) kobj_unref(test_handles[h - 0x100]);
    test_next_handle = 0x100;
}
u32 ev_handle(KEvent* e) { return h_new(&e->o); }
int new_event(u32* event, const char* name, int signaled) {
    KEvent* e = kevent_new(name);
    e->signaled = signaled;
    *event = h_new(&e->o);
    return 0;
}
KEvent* owned_event(const char* name) {
    KEvent* e = kevent_new(name);
    kobj_ref(&e->o);
    return e;
}
/* a poll with nothing to poll sleeps */
static void sleep_ms(int ms) { snx_net_poll(NULL, 0, ms); }
/* takes the event's signal, waiting up to ms for it */
static int took_signal(KEvent* e, int ms) {
    for (int i = 0; i <= ms; i++) {
        if (__atomic_exchange_n(&e->signaled, 0, __ATOMIC_ACQ_REL)) return 1;
        sleep_ms(1);
    }
    return 0;
}

/* ---- requests. The buffers live in guest memory at fixed addresses */
enum { BUF0 = 0x1000, BUF1 = 0x3000, BUF2 = 0x5000 };
typedef struct {
    IpcCtx c;
    u8 raw[0x100];
} Request;
static IpcCtx* request(Request* r, u32 cmd) {
    memset(r, 0, sizeof *r);
    r->c.cmd = cmd;
    r->c.raw = r->raw;
    r->c.raw_size = sizeof r->raw;
    return &r->c;
}
static void in_x(Request* r, u64 addr, u64 size) { /* a pointer buffer in (0x19) */
    r->c.x[r->c.nx].addr = addr;
    r->c.x[r->c.nx++].size = size;
}
static void out_c(Request* r, u64 addr, u64 size) { /* a pointer buffer out (0x1a, 0xa) */
    r->c.c[r->c.nc].addr = addr;
    r->c.c[r->c.nc++].size = size;
}
static void in_auto(Request* r, u64 addr, u64 size) { /* AutoSelect in (0x21): mapped, with an empty X beside */
    r->c.a[r->c.na].addr = addr;
    r->c.a[r->c.na++].size = size;
    r->c.nx++;
}
static void out_auto(Request* r, u64 addr, u64 size) { /* AutoSelect out (0x22) */
    r->c.b[r->c.nb].addr = addr;
    r->c.b[r->c.nb++].size = size;
    r->c.nc++;
}
static u32 call(SvcObj* o, Request* r) { return (u32)o->dispatch(o, &r->c); }
static u32 simple(SvcObj* o, u32 cmd) {
    Request r;
    request(&r, cmd);
    return call(o, &r);
}
static u32 reply32(Request* r, u32 off) {
    CHECK(off + 4 <= r->c.out_size);
    return snx_ld_le32(r->c.out + off); /* the reply is the guest's: little-endian */
}
static u32 get_state(SvcObj* o) {
    Request r;
    request(&r, 0);
    CHECK(call(o, &r) == 0 && r.c.out_size == 4);
    return reply32(&r, 0);
}
static i16 disconnect_reason(SvcObj* o) {
    Request r;
    request(&r, 3);
    CHECK(call(o, &r) == 0 && r.c.out_size == 4);
    return (i16)snx_ld_le16(r.c.out);
}
static u32 network_info(SvcObj* o, SnxLdnNetworkInfo* info) {
    Request r;
    u32 rc;
    request(&r, 1);
    out_c(&r, BUF0, sizeof *info);
    rc = call(o, &r);
    if (!rc) memcpy(info, GP(BUF0), sizeof *info);
    return rc;
}
static u32 latest_update(SvcObj* o, SnxLdnNetworkInfo* info, u8 updates[8], u64 count) {
    Request r;
    u32 rc;
    request(&r, 101);
    out_c(&r, BUF0, sizeof *info);
    out_c(&r, BUF1, count * 8);
    memset(GP(BUF1), 0xEE, 64);
    rc = call(o, &r);
    if (!rc) {
        memcpy(info, GP(BUF0), sizeof *info);
        for (u64 i = 0; i < count; i++) updates[i] = *(u8*)GP(BUF1 + i * 8);
    }
    return rc;
}
static int wait_state(SvcObj* o, u32 state, int ms) {
    for (int i = 0; i <= ms; i++) {
        if (get_state(o) == state) return 1;
        sleep_ms(1);
    }
    return 0;
}
#define LDN_RC(desc) RC(203, desc)
/* a local communication id, 0x0100ABCD12345000, as the raw data carries it */
static const u8 lcid[8] = {0x00, 0x50, 0x34, 0x12, 0xCD, 0xAB, 0x00, 0x01};

/* ldn=room: a console in an Eden room (fake_room.c) initializes at the room's address and hosts; a station of the room
   finds it and joins; the game's datagram sockets go through the room; at the exit ldn_exit closes the network before
   the room is left (svc_core_exit), so the station hears its DestroyNetwork */
static void in_a_room(SvcObj* creator, SvcObj* bsd) {
    FakeRoom* room = fake_room_start();
    SnxRoom *ra, *rb;
    SnxLdn* st;
    SnxLdnNetworkInfo found[2], info;
    Request r;
    SvcObj* a;
    u8 name[SNX_LDN_USER_NAME];
    int why, i, destroys;
    CHECK(snx_room_join(&ra, "127.0.0.1", fake_room_port(room), "Aogo", "secret", "game", 1, "", &why) == 0);
    CHECK(snx_room_join(&rb, "127.0.0.1", fake_room_port(room), "Xuji", "secret", "game", 1, "", &why) == 0);
    network_set_room(ra);
    g_opt_ldn = SNX_OPT_LDN_ROOM;
    g_settings.network = 0;
    request(&r, 0);
    CHECK(call(creator, &r) == 0 && r.c.nout_objs == 1);
    a = r.c.out_objs[0];
    request(&r, 400);
    r.c.has_pid = 1;
    {
        u32 rc = call(a, &r);
        if (rc) fprintf(stderr, "Initialize in the room: 0x%x\n", rc);
        CHECK(rc == 0 && get_state(a) == 1);
    }
    request(&r, 2); /* GetIpv4Address: the room's 192.168.1.1/24, in host order */
    CHECK(call(a, &r) == 0 && reply32(&r, 0) == 0xC0A80101 && reply32(&r, 4) == 0xFFFFFF00);
    CHECK(simple(a, 200) == 0 && get_state(a) == 2);
    request(&r, 202);
    r.raw[0] = 1;
    strcpy((char*)r.raw + 0x44, "Host");
    memcpy(r.raw + 0x78, lcid, 8);
    r.raw[0x78 + 0x12] = 8;
    r.raw[0x78 + 0x14] = 4;
    CHECK(call(a, &r) == 0 && get_state(a) == 3);
    CHECK(network_info(a, &info) == 0 && info.nodes[0].ipv4[3] == 192 && info.nodes[0].ipv4[0] == 1);
    CHECK(snx_ldn_open_room(&st, rb, NULL, NULL) == 0);
    CHECK(snx_ldn_scan(st, NULL, found, 2, 1000) == 1);
    memset(name, 0, sizeof name);
    strcpy((char*)name, "Xuji");
    CHECK(snx_ldn_connect(st, &found[0], name, 4, 2000) == 0);
    for (i = 0; i < 400 && (network_info(a, &info) || info.node_count != 2); i++) sleep_ms(5);
    CHECK(info.node_count == 2);
    /* the game's datagram sockets, protocol 0 and UDP, are the room's: each reaches the station's console with the
       network option off, at a socket of its protocol there (Eden's protocol byte) */
    {
        static const int protocols[2] = {0, SNX_IPPROTO_UDP};
        SnxTimeval tv = {(int64_t)snx_le64(2), 0}; /* option values are the guest's (net.h) */
        int k;
        for (k = 0; k < 2; k++) {
            SnxSocket* rx;
            SnxSockAddr at, from;
            u8 to[16] = {16, 2, 0x04, (u8)(0xD2 + k), 192, 168, 1, 2}, got[8];
            u32 fd;
            CHECK(snx_room_socket_open(rb, &rx, SNX_AF_INET, SNX_SOCK_DGRAM, protocols[k]) == 0);
            memset(&at, 0, sizeof at);
            at.family = SNX_AF_INET;
            memcpy(&at.port, to + 2, 2); /* network order, as the guest's bytes */
            CHECK(snx_socket_bind(rx, &at) == 0);
            snx_socket_setsockopt(rx, SNX_SOL_SOCKET, SNX_SO_RCVTIMEO, &tv, sizeof tv);
            request(&r, 2); /* Socket(AF_INET, SOCK_DGRAM, protocol) */
            memset(r.raw, 0, 12);
            r.raw[0] = 2;
            r.raw[4] = 2;
            r.raw[8] = (u8)protocols[k];
            CHECK(call(bsd, &r) == 0 && (i32)reply32(&r, 0) >= 0);
            fd = reply32(&r, 0);
            request(&r, 11); /* SendTo(fd, 0, "ping", 192.168.1.2:1234 or 1235) */
            memset(r.raw, 0, 8);
            snx_st_le32(r.raw, fd);
            memcpy(GP(BUF0), "ping", 4);
            memcpy(GP(BUF1), to, 16);
            in_auto(&r, BUF0, 4);
            in_auto(&r, BUF1, 16);
            CHECK(call(bsd, &r) == 0 && reply32(&r, 0) == 4 && reply32(&r, 4) == 0);
            CHECK(snx_socket_recv(rx, got, sizeof got, 0, &from, NULL) == 4 && !memcmp(got, "ping", 4));
            CHECK(from.addr[0] == 192 && from.addr[3] == 1); /* from this console's address in the room */
            request(&r, 26);                                 /* Close */
            snx_st_le32(r.raw, fd);
            CHECK(call(bsd, &r) == 0);
            snx_socket_unref(rx);
        }
    }
    destroys = fake_room_ldn_count(room, SNX_ROOM_LDN_DESTROY_NETWORK, NULL);
    ldn_exit();
    network_set_room(NULL);
    snx_room_leave(ra);
    CHECK(get_state(a) == 0);
    for (i = 0; i < 600 && fake_room_ldn_count(room, SNX_ROOM_LDN_DESTROY_NETWORK, NULL) == destroys; i++) sleep_ms(5);
    CHECK(fake_room_ldn_count(room, SNX_ROOM_LDN_DESTROY_NETWORK, NULL) == destroys + 1);
    for (i = 0; i < 400 && snx_ldn_connected(st); i++) sleep_ms(5);
    CHECK(!snx_ldn_connected(st) && snx_ldn_disconnect_reason(st) == SNX_LDN_REASON_DESTROYED_BY_USER);
    snx_ldn_close(st);
    snx_room_leave(rb);
    svcobj_unref(a);
    g_opt_ldn = SNX_OPT_LDN_LAN;
    fake_room_stop(room);
}


int main(void) {
    hle_init();
    SvcObj *creator, *a, *b, *bsd;
    KEvent *ev_a, *ev_b;
    Request r;
    SnxLdnNetworkInfo info, found[2];
    u8 updates[8];
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    network_init();
    snx_net_set_log(NULL);

    creator = new_ldn();
    request(&r, 0); /* CreateUserLocalCommunicationService */
    CHECK(call(creator, &r) == 0 && r.c.nout_objs == 1);
    a = r.c.out_objs[0];
    request(&r, 0);
    CHECK(call(creator, &r) == 0 && r.c.nout_objs == 1);
    b = r.c.out_objs[0];
    request(&r, 1); /* CreateClientProcessMonitor, and its RegisterClient(pid) is a stub */
    CHECK(call(creator, &r) == 0 && r.c.nout_objs == 1);
    {
        SvcObj* monitor = r.c.out_objs[0];
        request(&r, 0);
        r.c.has_pid = 1;
        CHECK(call(monitor, &r) == 0 && r.c.out_size == 0);
        svcobj_unref(monitor);
    }

    /* before Initialize: state None, and nothing else works */
    CHECK(get_state(a) == 0);
    CHECK(simple(a, 200) == LDN_RC(32) && simple(a, 300) == LDN_RC(32) && simple(a, 204) == LDN_RC(32));
    CHECK(network_info(a, &info) == LDN_RC(32));
    request(&r, 2);
    CHECK(call(a, &r) == LDN_RC(33)); /* NoIpAddress */
    CHECK(disconnect_reason(a) == 0);
    request(&r, 100); /* AttachStateChangeEvent: the same event each time */
    CHECK(call(a, &r) == 0 && r.c.nout_copy == 1);
    ev_a = handle_event(r.c.out_copy[0]);
    CHECK(!ev_a->signaled);
    request(&r, 100);
    CHECK(call(a, &r) == 0 && handle_event(r.c.out_copy[0]) == ev_a);
    request(&r, 100);
    CHECK(call(b, &r) == 0);
    ev_b = handle_event(r.c.out_copy[0]);

    /* ldn=off is a console with local wireless disabled, airplane_mode one in airplane mode, and ldn=room one out of
       its room: none initializes, each with its result */
    g_opt_ldn = SNX_OPT_LDN_OFF;
    request(&r, 400);
    r.c.has_pid = 1;
    CHECK(call(a, &r) == LDN_RC(22) && get_state(a) == 0);
    g_opt_ldn = SNX_OPT_LDN_LAN;
    g_settings.airplane_mode = 1;
    request(&r, 400);
    r.c.has_pid = 1;
    CHECK(call(a, &r) == LDN_RC(23) && get_state(a) == 0);
    g_settings.airplane_mode = 0;
    g_opt_ldn = SNX_OPT_LDN_ROOM;
    request(&r, 400);
    r.c.has_pid = 1;
    CHECK(call(a, &r) == LDN_RC(33) && get_state(a) == 0);
    g_opt_ldn = SNX_OPT_LDN_LAN;
    CHECK(!ev_a->signaled);

    /* Initialize: each console gets a loopback address of its own, offline */
    g_settings.network = 0;
    request(&r, 400);
    r.c.has_pid = 1;
    CHECK(call(a, &r) == 0 && get_state(a) == 1);
    request(&r, 2); /* GetIpv4Address: host order */
    CHECK(call(a, &r) == 0 && r.c.out_size == 8);
    CHECK(reply32(&r, 0) == 0x7F000001 && reply32(&r, 4) == 0xFF000000);
    request(&r, 402); /* InitializeWithVersion(i32, pid): version at 0, the pid at 8 */
    r.c.has_pid = 1;
    memset(r.raw, 0, 16);
    r.raw[0] = 1;
    {
        u32 rc = call(b, &r);
        if (rc == LDN_RC(33)) { /* a host whose loopback has 127.0.0.1 alone (macOS) */
            printf("ldn_service: skipped the second console: this host has no 127.0.0.2\n");
            CHECK(simple(a, 401) == 0 && get_state(a) == 0);
            svcobj_unref(a);
            svcobj_unref(b);
            svcobj_unref(creator);
            close_handles();
            CHECK(test_objects == 0 && test_kobjs == 0);
            return 0;
        }
        CHECK(rc == 0 && get_state(b) == 1);
    }
    request(&r, 2);
    CHECK(call(b, &r) == 0 && reply32(&r, 0) == 0x7F000002);
    request(&r, 400); /* initialized twice is still initialized */
    r.c.has_pid = 1;
    CHECK(call(a, &r) == 0 && get_state(a) == 1);
    CHECK(!ev_a->signaled && !ev_b->signaled);
    CHECK(network_info(a, &info) == LDN_RC(32)); /* no network yet */

    /* a game socket bound to INADDR_ANY binds the console's address (the last console initialized, here) */
    bsd = new_bsd_u();
    {
        u32 err, fd;
        u8 any[16] = {16, 2, 0, 0, 0, 0, 0, 0}, name[16];
        request(&r, 2); /* Socket(AF_INET, SOCK_DGRAM) */
        memset(r.raw, 0, 12);
        r.raw[0] = 2;
        r.raw[4] = 2;
        CHECK(call(bsd, &r) == 0 && reply32(&r, 1 * 4) == 0);
        fd = reply32(&r, 0);
        memcpy(GP(BUF0), any, 16);
        request(&r, 13); /* Bind(fd, addr) */
        snx_st_le32(r.raw, fd);
        in_auto(&r, BUF0, 16);
        CHECK(call(bsd, &r) == 0 && reply32(&r, 0) == 0);
        request(&r, 16); /* GetSockName(fd) -> addr */
        snx_st_le32(r.raw, fd);
        out_auto(&r, BUF1, 16);
        CHECK(call(bsd, &r) == 0 && reply32(&r, 0) == 0);
        memcpy(name, GP(BUF1), 16);
        CHECK(name[4] == 127 && name[5] == 0 && name[6] == 0 && name[7] == 2);
        err = reply32(&r, 4);
        CHECK(err == 0);
        /* and it hears both a datagram to its address and one to the subnet's broadcast address, which Linux
           delivers to no socket bound to a unicast address: the companion on 127.255.255.255 takes those */
        {
            SnxSocket* peer;
            SnxSockAddr to;
            u8 port[2] = {name[2], name[3]};
            int one = (int)snx_le32(1), k;
            CHECK(snx_socket_open(&peer, SNX_AF_INET, SNX_SOCK_DGRAM, 0) == 0);
            snx_socket_setsockopt(peer, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &one, sizeof one);
            memset(&to, 0, sizeof to);
            to.family = SNX_AF_INET;
            memcpy(&to.port, port, 2);
            to.addr[0] = 127;
            to.addr[3] = 2;
            CHECK(snx_socket_send(peer, "unicast", 7, 0, &to) == 7);
            memset(to.addr + 1, 255, 3);
            CHECK(snx_socket_send(peer, "broadcast", 9, 0, &to) == 9);
            for (k = 0; k < 2; k++) {
                request(&r, 9); /* RecvFrom(fd, flags) -> (ret, errno, addrlen), the message and the sender */
                snx_st_le32(r.raw, fd);
                out_auto(&r, BUF1, 64);
                out_auto(&r, BUF2, 16);
                CHECK(call(bsd, &r) == 0);
                CHECK(reply32(&r, 0) == (k == 0 ? 7 : 9) && reply32(&r, 4) == 0 && reply32(&r, 8) == 16);
                CHECK(!memcmp(GP(BUF1), k == 0 ? "unicast" : "broadcast", k == 0 ? 7 : 9));
                CHECK(((u8*)GP(BUF2))[4] == 127); /* from the peer, on the loopback */
            }
            request(&r, 6); /* Poll(nfds, timeout) with the fd for POLLIN: nothing left to read */
            r.raw[0] = 1;
            snx_st_le32(GP(BUF1), fd);
            ((u8*)GP(BUF1))[4] = 1; /* POLLIN */
            ((u8*)GP(BUF1))[5] = 0;
            in_auto(&r, BUF1, 8);
            out_auto(&r, BUF2, 8);
            CHECK(call(bsd, &r) == 0 && reply32(&r, 0) == 0);
            memset(to.addr + 1, 255, 3);
            CHECK(snx_socket_send(peer, "again", 5, 0, &to) == 5); /* a broadcast makes Poll see it readable */
            request(&r, 6);
            r.raw[0] = 1;
            r.raw[4] = 100; /* 100 ms */
            in_auto(&r, BUF1, 8);
            out_auto(&r, BUF2, 8);
            CHECK(call(bsd, &r) == 0 && reply32(&r, 0) == 1 && (((u8*)GP(BUF2))[6] & 1));
            snx_socket_unref(peer);
        }
    }

    /* the host: OpenAccessPoint, CreateNetwork, SetAdvertiseData; each state change signals the event */
    CHECK(simple(a, 200) == 0 && get_state(a) == 2 && took_signal(ev_a, 0));
    CHECK(simple(a, 200) == LDN_RC(32)); /* not from AccessPointOpened */
    request(&r, 202);                    /* CreateNetwork(CreateNetworkConfig) */
    r.raw[0] = 1;                        /* security mode: retail */
    strcpy((char*)r.raw + 0x44, "Host");
    memcpy(r.raw + 0x78, lcid, 8); /* local communication id 0x0100ABCD12345000 */
    r.raw[0x78 + 0xa] = 3;         /* scene id */
    r.raw[0x78 + 0x12] = 8;        /* node count max */
    r.raw[0x78 + 0x14] = 4;        /* version */
    CHECK(call(a, &r) == 0 && get_state(a) == 3 && took_signal(ev_a, 0));
    CHECK(latest_update(a, &info, updates, 8) == 0);
    CHECK(info.node_count == 1 && info.nodes[0].is_connected && !strcmp((char*)info.nodes[0].user_name, "Host"));
    CHECK(snx_le64(info.local_communication_id) == 0x0100ABCD12345000ull && snx_le16(info.scene_id) == 3 &&
          snx_le16((u16)info.channel) == 6); /* the guest's little-endian structure */
    CHECK(info.ssid_length == 32 && (int16_t)snx_le16((uint16_t)info.nodes[0].version) == 4 && info.node_count_max == 8);
    CHECK(info.nodes[0].ipv4[0] == 1 && info.nodes[0].ipv4[3] == 127 && info.nodes[0].mac[0] == 2);
    CHECK(updates[0] == 1 && updates[1] == 0); /* node 0 connected */
    CHECK(latest_update(a, &info, updates, 2) == 0 && updates[0] == 0);
    CHECK(latest_update(a, &info, updates, 9) == LDN_RC(50)); /* more nodes than there are */
    request(&r, 206);                                         /* SetAdvertiseData */
    memcpy(GP(BUF2), "game", 4);
    in_auto(&r, BUF2, 4);
    CHECK(call(a, &r) == 0);
    request(&r, 206);
    in_auto(&r, BUF2, SNX_LDN_ADVERTISE_MAX + 1);
    CHECK(call(a, &r) == LDN_RC(10));
    request(&r, 5); /* GetNetworkConfig */
    CHECK(call(a, &r) == 0 && r.c.out_size == 0x20);
    CHECK(!memcmp(r.c.out, lcid, 8) && r.c.out[0x12] == 8 && r.c.out[0x14] == 4);

    /* the station: OpenStation and Scan; the filter is the game's; the host doesn't find itself */
    CHECK(simple(b, 300) == 0 && get_state(b) == 4 && took_signal(ev_b, 0));
    request(&r, 102); /* Scan(i16 channel, ScanFilter at 8) -> i16 count, NetworkInfo[] */
    memcpy(r.raw + 8, lcid, 8);
    r.raw[8 + 0x5c] = 1; /* flag: local communication id */
    out_auto(&r, BUF0, sizeof found);
    CHECK(call(b, &r) == 0 && r.c.out_size == 4);
    CHECK((i16)reply32(&r, 0) == 1);
    memcpy(found, GP(BUF0), sizeof found);
    CHECK(snx_le64(found[0].local_communication_id) == 0x0100ABCD12345000ull && snx_le16(found[0].scene_id) == 3);
    CHECK(snx_le16(found[0].advertise_data_size) == 4 && !memcmp(found[0].advertise_data, "game", 4));
    CHECK(!strcmp((char*)found[0].nodes[0].user_name, "Host") && found[0].node_count == 1);
    request(&r, 102);
    r.raw[8] = 1; /* another game's id */
    r.raw[8 + 0x5c] = 1;
    out_auto(&r, BUF0, sizeof found);
    CHECK(call(b, &r) == 0 && (i16)reply32(&r, 0) == 0);
    request(&r, 103); /* ScanPrivate, the same */
    out_auto(&r, BUF0, sizeof found);
    CHECK(call(a, &r) == 0 && (i16)reply32(&r, 0) == 0);
    request(&r, 102);
    out_auto(&r, BUF0, 0);
    CHECK(call(b, &r) == LDN_RC(96)); /* nowhere to put the networks */
    CHECK(!ev_b->signaled);           /* scanning isn't a state change */

    /* Connect: the station's node reaches the host, both see two nodes, both events fire */
    request(&r, 302); /* Connect(ConnectNetworkData, NetworkInfo in a pointer buffer) */
    strcpy((char*)r.raw + 0x44, "Station");
    r.raw[0x74] = 4;
    memcpy(GP(BUF1), &found[0], sizeof found[0]);
    in_x(&r, BUF1, sizeof found[0]);
    CHECK(call(a, &r) == LDN_RC(32)); /* the host isn't a station */
    CHECK(call(b, &r) == 0 && get_state(b) == 5 && took_signal(ev_b, 0));
    CHECK(took_signal(ev_a, 2000));
    CHECK(latest_update(a, &info, updates, 8) == 0);
    CHECK(info.node_count == 2 && info.nodes[1].is_connected && !strcmp((char*)info.nodes[1].user_name, "Station"));
    CHECK(info.nodes[1].ipv4[0] == 2 && (int16_t)snx_le16((uint16_t)info.nodes[1].version) == 4 && updates[1] == 1 && updates[0] == 0);
    CHECK(latest_update(b, &info, updates, 8) == 0);
    CHECK(info.node_count == 2 && !strcmp((char*)info.nodes[0].user_name, "Host") &&
          !strcmp((char*)info.nodes[1].user_name, "Station"));
    CHECK(updates[0] == 1 && updates[1] == 1 && updates[2] == 0);
    CHECK(snx_le16(info.advertise_data_size) == 4 && !memcmp(info.advertise_data, "game", 4));
    request(&r, 4); /* GetSecurityParameter: the session id the host made */
    CHECK(call(b, &r) == 0 && r.c.out_size == 0x20 && !memcmp(r.c.out + 16, info.session_id, 16));
    CHECK(call(b, &r) == 0); /* and the same once more */
    request(&r, 302);
    in_x(&r, BUF1, sizeof found[0]);
    CHECK(call(b, &r) == LDN_RC(32)); /* already connected */

    /* Disconnect: the station's own choice; the host's node 1 empties and its event fires */
    CHECK(simple(b, 304) == 0 && get_state(b) == 4 && disconnect_reason(b) == 1 && took_signal(ev_b, 0));
    CHECK(simple(b, 304) == LDN_RC(32));
    CHECK(took_signal(ev_a, 2000));
    CHECK(latest_update(a, &info, updates, 8) == 0 && info.node_count == 1 && updates[1] == 2);
    CHECK(network_info(b, &info) == LDN_RC(32));

    /* joined again, the host destroys the network: the station is told, with the reason, by the event */
    request(&r, 302);
    strcpy((char*)r.raw + 0x44, "Station");
    r.raw[0x74] = 4;
    in_x(&r, BUF1, sizeof found[0]);
    CHECK(call(b, &r) == 0 && get_state(b) == 5);
    took_signal(ev_b, 0);
    CHECK(simple(a, 204) == 0 && get_state(a) == 2 && took_signal(ev_a, 0));
    CHECK(wait_state(b, 4, 2000) && took_signal(ev_b, 0) && disconnect_reason(b) == 3);
    /* nobody to join now */
    request(&r, 302);
    strcpy((char*)r.raw + 0x44, "Station");
    in_x(&r, BUF1, sizeof found[0]);
    CHECK(call(b, &r) == LDN_RC(64) && get_state(b) == 4);
    request(&r, 302); /* a network with no nodes */
    memset(GP(BUF1), 0, sizeof found[0]);
    in_x(&r, BUF1, sizeof found[0]);
    CHECK(call(b, &r) == LDN_RC(30));

    /* closing and finalizing: back to Initialized, then None; the bind address goes with the service */
    CHECK(simple(a, 201) == 0 && get_state(a) == 1 && took_signal(ev_a, 0));
    CHECK(simple(a, 201) == LDN_RC(32));
    CHECK(simple(b, 301) == 0 && get_state(b) == 1);
    CHECK(simple(b, 301) == LDN_RC(32));
    CHECK(simple(b, 401) == 0 && get_state(b) == 0);
    {
        u32 fd;
        u8 any[16] = {16, 2, 0, 0, 0, 0, 0, 0}, name[16];
        request(&r, 2);
        memset(r.raw, 0, 12);
        r.raw[0] = 2;
        r.raw[4] = 2;
        CHECK(call(bsd, &r) == 0);
        fd = reply32(&r, 0);
        memcpy(GP(BUF0), any, 16);
        request(&r, 13);
        snx_st_le32(r.raw, fd);
        in_auto(&r, BUF0, 16);
        CHECK(call(bsd, &r) == 0 && reply32(&r, 0) == 0);
        request(&r, 16);
        snx_st_le32(r.raw, fd);
        out_auto(&r, BUF1, 16);
        CHECK(call(bsd, &r) == 0);
        memcpy(name, GP(BUF1), 16);
        CHECK(name[4] == 0 && name[7] == 0); /* INADDR_ANY again */
    }
    CHECK(simple(a, 401) == 0 && get_state(a) == 0);
    /* 127.0.0.1 is free again: the second console takes it now */
    request(&r, 400);
    r.c.has_pid = 1;
    CHECK(call(b, &r) == 0);
    request(&r, 2);
    CHECK(call(b, &r) == 0 && reply32(&r, 0) == 0x7F000001);

    in_a_room(creator, bsd);
    svcobj_unref(bsd);
    svcobj_unref(a);
    svcobj_unref(b); /* still initialized: the transport goes with the object */
    svcobj_unref(creator);
    close_handles();
    CHECK(test_objects == 0 && test_kobjs == 0);
    free(g_mem);
    printf("ldn_service: %u checks passed\n", test_checks);
    return 0;
}
