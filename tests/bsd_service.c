/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* bsd:u's IPC commands, sent as the SDK and libnx send them (FreeBSD's constants and sockaddr layout), over the
   host's loopback, and the network option's offline mode */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>

#include "nx/hle/services/network/bsd.c"
#include "nx/hle/hle.c"

u8* g_mem;
static unsigned failures;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "bsd_service.c:%d: check failed: %s\n", __LINE__, #x);                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
/* memory/own.c: nothing is GPU-owned here */
void own_acquire(u64 addr, u64 size) { (void)addr, (void)size; }
/* platform/plat_posix.c: no page here is protected for the write tracking */
void plat_ww_open(u8* p, u64 size) { (void)p, (void)size; }
void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    SvcObj* o = calloc(1, sizeof *o);
    o->name = name;
    o->dispatch = dispatch;
    o->refs = 1;
    return o;
}
#include "svcobj_data.h"
void svcobj_unref(SvcObj* o) {
    if (!o || --o->refs) return;
    if (o->destroy) o->destroy(o);
    test_svcobj_data_free(o);
    free(o);
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    failures++;
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
    (void)o, (void)c;
    return 0;
}
void svc_register(const char* name, SvcCtor ctor) { (void)name, (void)ctor; }

/* guest memory: buffers at fixed addresses */
enum { IN0 = 0x1000, IN1 = 0x2000, OUT0 = 0x3000, OUT1 = 0x4000, BUFSIZE = 0x1000 };
static SvcObj* bsd;
static IpcCtx ctx;
static u8 raw[64]; /* more than any bsd command takes: sf refuses a request shorter than its arguments */
static void request(u32 cmd, u32 a0, u32 a1, u32 a2) {
    memset(&ctx, 0, sizeof ctx);
    memset(raw, 0, sizeof raw);
    snx_st_le32(raw, a0); /* the request is the guest's: little-endian */
    snx_st_le32(raw + 4, a1);
    snx_st_le32(raw + 8, a2);
    ctx.raw = raw;
    ctx.raw_size = sizeof raw;
    ctx.cmd = cmd;
}
/* bsd's buffers are all AutoSelect, sent in both their lists: mapped here, with an empty pointer descriptor beside */
static void in(u64 addr, u64 size) {
    ctx.a[ctx.na].addr = addr;
    ctx.a[ctx.na++].size = size;
    ctx.nx++;
}
static void out(u64 addr, u64 size) {
    ctx.b[ctx.nb].addr = addr;
    ctx.b[ctx.nb++].size = size;
    ctx.nc++;
}
/* sends the request; *err gets the errno, the call's result is returned */
static i32 call(u32* err) {
    CHECK(bsd->dispatch(bsd, &ctx) == 0);
    u32 expected = ctx.cmd == 0                                                                         ? 4
                   : (ctx.cmd == 9 || ctx.cmd == 12 || ctx.cmd == 15 || ctx.cmd == 16 || ctx.cmd == 17) ? 12
                                                                                                        : 8;
    CHECK(ctx.out_size == expected);
    if (err) *err = snx_ld_le32(ctx.out + 4);
    return (i32)snx_ld_le32(ctx.out);
}
static u32 word(int i) {
    return snx_ld_le32(ctx.out + i * 4);
}
/* FreeBSD's sockaddr_in for 127.0.0.1 */
static void sockaddr_lo(u64 at, u16 port_be) {
    u8 s[16] = {16, 2, 0, 0, 127, 0, 0, 1};
    memcpy(s + 2, &port_be, 2);
    memcpy(GP(at), s, 16);
}


int main(void) {
    hle_init();
    g_mem = calloc(1, 0x10000);
    network_init();
    snx_net_set_log(NULL);
    bsd = new_bsd_u();
    u32 e;

    request(0, 0, 0, 0); /* RegisterClient(config, pid, transfer memory size, transfer memory) */
    ctx.has_pid = 1;
    ctx.ncopy = 1;
    call(NULL);
    request(2, 2, 1, 0); /* Socket(AF_INET, SOCK_STREAM) */
    i32 l = call(&e);
    CHECK(l >= 0 && !e);
    gw32(IN0, 1);
    request(21, (u32)l, 0xFFFF, 0x4); /* SetSockOpt SO_REUSEADDR */
    in(IN0, 4);
    CHECK(call(&e) == 0);
    request(21, (u32)l, 0xFFFF, 0x10000); /* Horizon's SO_NN_SHUTDOWN_EXEMPT: accepted */
    in(IN0, 4);
    CHECK(call(&e) == 0);
    sockaddr_lo(IN0, 0);
    request(13, (u32)l, 0, 0); /* Bind */
    in(IN0, 16);
    CHECK(call(&e) == 0);
    request(18, (u32)l, 4, 0); /* Listen */
    CHECK(call(&e) == 0);
    request(16, (u32)l, 0, 0); /* GetSockName */
    out(OUT0, 16);
    CHECK(call(&e) == 0 && word(2) == 16);
    u8* name = GP(OUT0);
    u16 port;
    memcpy(&port, name + 2, 2);
    CHECK(name[0] == 16 && name[1] == 2 && port && name[4] == 127);
    memset(GP(OUT0), 0xEE, 16);
    request(16, (u32)l, 0, 0); /* GetSockName into 8 bytes: the address cut, and the length cut */
    out(OUT0, 8);
    CHECK(call(&e) == 0 && word(2) == 8 && name[4] == 127 && name[8] == 0xEE);

    request(2, 2, 1 | 0x20000000, 0); /* a SOCK_NONBLOCK client */
    i32 c = call(&e);
    CHECK(c > l);
    request(20, (u32)c, 3, 0); /* Fcntl F_GETFL */
    CHECK((call(&e) & 0x800) == 0x800);
    sockaddr_lo(IN0, port);
    request(14, (u32)c, 0, 0); /* Connect */
    in(IN0, 16);
    i32 r = call(&e);
    CHECK(r == 0 || (r == -1 && e == SNX_EINPROGRESS));
    request(12, (u32)l, 0, 0); /* Accept */
    out(OUT0, 16);
    i32 s = call(&e);
    CHECK(s > c && word(2) == 16);

    /* Poll: the client writable, then readable */
    u8 pfd[8] = {0};
    snx_st_le32(pfd, (u32)c); /* guest memory: little-endian */
    pfd[4] = 0x04; /* POLLOUT */
    memcpy(GP(IN0), pfd, 8);
    request(6, 1, 2000, 0);
    in(IN0, 8);
    out(OUT0, 8);
    CHECK(call(&e) == 1 && (GP(OUT0)[6] & 0x04));
    memcpy(GP(IN1), "switch", 6);
    request(10, (u32)s, 0, 0); /* Send */
    in(IN1, 6);
    CHECK(call(&e) == 6);
    pfd[4] = 0x01;
    memcpy(GP(IN0), pfd, 8);
    request(6, 1, 2000, 0);
    in(IN0, 8);
    out(OUT0, 8);
    CHECK(call(&e) == 1 && (GP(OUT0)[6] & 0x01));
    request(19, (u32)c, BSD_FIONREAD, 1); /* Ioctl FIONREAD: four ins, four outs, the first out used */
    for (int k = 0; k < 4; k++) in(0, 0);
    out(OUT1, 4);
    for (int k = 0; k < 3; k++) out(0, 0);
    CHECK(call(&e) == 0 && gr32(OUT1) == 6);
    request(8, (u32)c, 0x2, 0); /* Recv MSG_PEEK (FreeBSD's) */
    out(OUT0, 3);
    CHECK(call(&e) == 3);
    request(25, (u32)c, 0, 0); /* Read */
    out(OUT0, BUFSIZE);
    CHECK(call(&e) == 6 && !memcmp(GP(OUT0), "switch", 6));
    request(8, (u32)c, 0x80, 0); /* Recv MSG_DONTWAIT: nothing left */
    out(OUT0, BUFSIZE);
    CHECK(call(&e) == -1 && e == SNX_EAGAIN);

    /* Select: s isn't readable, c is writable */
    u8 sets[3][16];
    memset(sets, 0, sizeof sets);
    sets[0][s / 8] |= (u8)(1 << (s % 8));
    sets[1][c / 8] |= (u8)(1 << (c % 8));
    memcpy(GP(IN0), sets[0], 16);
    memcpy(GP(IN1), sets[1], 16);
    request(5, (u32)(s > c ? s : c) + 1, 0, 0); /* raw[2..] stay zero: the timeout at 8 is {0, 0}, not null */
    in(IN0, 16);
    in(IN1, 16);
    in(0, 0);
    out(OUT0, 16);
    out(OUT1, 16);
    out(0, 0);
    CHECK(call(&e) == 1 && !(GP(OUT0)[s / 8] & (1 << (s % 8))) && (GP(OUT1)[c / 8] & (1 << (c % 8))));

    /* options */
    request(17, (u32)s, 0xFFFF, 0x1008); /* GetSockOpt SO_TYPE */
    out(OUT0, 4);
    CHECK(call(&e) == 0 && gr32(OUT0) == 1 && word(2) == 4);
    request(17, (u32)s, 0xFFFF, 0x1007); /* SO_ERROR */
    out(OUT0, 4);
    CHECK(call(&e) == 0 && gr32(OUT0) == 0);
    gw64(IN0, 0);
    gw64(IN0 + 8, 250000);
    request(21, (u32)s, 0xFFFF, 0x1006); /* SO_RCVTIMEO */
    in(IN0, 16);
    CHECK(call(&e) == 0);
    request(8, (u32)s, 0, 0); /* a blocking Recv gives up after it */
    out(OUT0, BUFSIZE);
    CHECK(call(&e) == -1 && e == SNX_EAGAIN);
    gw64(IN0, 0); /* a tv_usec of a second, then a tv_sec before zero: EDOM, as a BSD stack answers */
    gw64(IN0 + 8, 1000000);
    request(21, (u32)s, 0xFFFF, 0x1006);
    in(IN0, 16);
    CHECK(call(&e) == -1 && e == SNX_EDOM);
    gw64(IN0, (u64)-1);
    gw64(IN0 + 8, 0);
    request(21, (u32)s, 0xFFFF, 0x1005); /* SO_SNDTIMEO */
    in(IN0, 16);
    CHECK(call(&e) == -1 && e == SNX_EDOM);

    /* the AutoSelect buffers through the pointer buffer, which the SDK picks for small ones: Send's message in X,
       Recv's in C, GetSockName's address in C, each beside an empty mapped descriptor */
    memcpy(GP(IN1), "pointer", 7);
    request(10, (u32)c, 0, 0);
    in(0, 0);
    ctx.x[0] = (IpcBuf){IN1, 7, 0};
    CHECK(call(&e) == 7);
    memset(GP(OUT0), 0, 16);
    request(8, (u32)s, 0, 0); /* blocking, with its 250 ms timeout */
    out(0, 0);
    ctx.c[0] = (IpcBuf){OUT0, BUFSIZE, 0};
    CHECK(call(&e) == 7 && !memcmp(GP(OUT0), "pointer", 7));
    memset(GP(OUT1), 0, 16);
    request(16, (u32)l, 0, 0);
    out(0, 0);
    ctx.c[0] = (IpcBuf){OUT1, 16, 0};
    CHECK(call(&e) == 0 && word(2) == 16 && GP(OUT1)[1] == 2 && !memcmp(GP(OUT1) + 2, &port, 2));

    /* a command bsd doesn't have (RecvMMsg): success, with the BSD result -1 and EOPNOTSUPP in the raw data */
    request(29, (u32)s, 0, 0);
    CHECK(call(&e) == -1 && e == SNX_EOPNOTSUPP);
    request(100, 0, 0, 0);
    CHECK(bsd->dispatch(bsd, &ctx) == 0 && ctx.out_size == 8 && word(0) == 0xFFFFFFFFu && word(1) == SNX_EOPNOTSUPP);

    /* UDP and RecvFrom's address */
    request(2, 2, 2, 17);
    i32 u1 = call(&e);
    request(2, 2, 2, 17);
    i32 u2 = call(&e);
    sockaddr_lo(IN0, 0);
    request(13, (u32)u1, 0, 0);
    in(IN0, 16);
    CHECK(call(&e) == 0);
    request(16, (u32)u1, 0, 0);
    out(OUT1, 16); /* keep the destination separate from the payload */
    CHECK(call(&e) == 0);
    memcpy(GP(IN1), "udp!", 4);
    request(11, (u32)u2, 0, 0); /* SendTo */
    in(IN1, 4);
    in(OUT1, 16);
    CHECK(call(&e) == 4);
    request(9, (u32)u1, 0, 0); /* RecvFrom */
    out(OUT0, 64);
    out(IN0, 16);
    CHECK(call(&e) == 4 && word(2) == 16 && GP(IN0)[1] == 2 && GP(IN0)[4] == 127);

    /* Zero-size RecvFrom must not expose an uninitialized sender or consume the datagram. */
    request(11, (u32)u2, 0, 0);
    in(IN1, 4);
    in(OUT1, 16);
    CHECK(call(&e) == 4);
    memset(GP(IN0), 0xA5, 16);
    request(9, (u32)u1, 0, 0);
    out(OUT0, 0);
    out(IN0, 16);
    CHECK(call(&e) == 0 && word(2) == 0 && GP(IN0)[0] == 0xA5);
    /* still queued, so readable: polled before the non-blocking RecvFrom, as macOS hands a loopback datagram to its
       socket a moment after SendTo returns (Linux before) */
    {
        u8 ready[8] = {0};
        snx_st_le32(ready, (u32)u1);
        ready[4] = 0x01; /* POLLIN */
        memcpy(GP(IN0), ready, 8);
        request(6, 1, 2000, 0);
        in(IN0, 8);
        out(OUT0, 8);
        CHECK(call(&e) == 1 && (GP(OUT0)[6] & 0x01));
    }
    request(9, (u32)u1, 0x80, 0);
    out(OUT0, 64);
    out(IN0, 16);
    CHECK(call(&e) == 4 && word(2) == 16);

    /* Every error response has exactly the command's ABI size, including the address length. */
    /* each with the buffers it declares: ins, outs */
    const u32 bad_fd_commands[][3] = {{8, 0, 1},  {9, 0, 2},  {10, 1, 0}, {11, 2, 0}, {12, 0, 1},
                                      {15, 0, 1}, {16, 0, 1}, {17, 0, 1}, {24, 1, 0}, {25, 0, 1}};
    for (unsigned i = 0; i < sizeof bad_fd_commands / sizeof *bad_fd_commands; i++) {
        request(bad_fd_commands[i][0], BSD_MAX_FD, 0, 0);
        for (u32 k = 0; k < bad_fd_commands[i][1]; k++) in(k ? IN0 : IN1, k ? 16 : 4);
        for (u32 k = 0; k < bad_fd_commands[i][2]; k++) out(k ? IN0 : OUT0, k ? 16 : 64);
        CHECK(call(&e) == -1 && e == SNX_EBADF);
        if (ctx.out_size == 12) CHECK(word(2) == 0);
    }

    /* EventFd, Close, bad descriptors, offline */
    request(31, 0x4, 0, 2); /* EventFd(2, EFD_NONBLOCK): the u32 flags at 0, the u64 initval at 8 */
    i32 ev = call(&e);
    CHECK(ev > 0);
    request(25, (u32)ev, 0, 0);
    out(OUT0, 8);
    CHECK(call(&e) == 8 && gr32(OUT0) == 2);
    request(26, (u32)c, 0, 0);
    CHECK(call(&e) == 0);
    request(26, (u32)c, 0, 0);
    CHECK(call(&e) == -1 && e == SNX_EBADF);
    request(2, 2, 3, 6); /* bsd:u has no raw TCP sockets */
    CHECK(call(&e) == -1 && e == SNX_EINVAL);
    u8 google[16] = {16, 2, 0, 53, 8, 8, 8, 8};
    memcpy(GP(IN0), google, 16);
    request(14, (u32)u2, 0, 0); /* offline: only loopback is reachable */
    in(IN0, 16);
    CHECK(call(&e) == -1 && e == SNX_ENETUNREACH);
    CHECK(!network_online());

    svcobj_unref(bsd);
    free(g_mem);
    if (failures) {
        fprintf(stderr, "bsd_service: %u checks failed\n", failures);
        return 1;
    }
    puts("bsd_service: ok");
    return 0;
}
