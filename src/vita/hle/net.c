/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The network, as an offline console has it: in flight mode (its radios off), signed out of PSN. What it does
   without a network works: a library's Init and Term, its callbacks registered and checked, byte order and address
   text, the account's NP ID (known signed out: a game looks its own player up by it) and NP IDs compared, and the
   console's own sockets. A socket sends to the console itself (127.0.0.0/8, or 0.0.0.0), where the socket bound to
   the address's port gets the datagram; any other address can't be reached. A game that hosts its own session, as
   one does offline, talks to itself that way. The state queries and the callbacks say there is no network (SceNetCtl
   disconnected for flight mode, NP signed out): a game waits on its connecting screen until it learns why. Every
   other function of the network's libraries, the ones the runtime doesn't know included, fails with its library's
   error (vita_net_offline_error): a game that took a network function's 0 for success went on to wait for a server
   that never answers, and one whose sceNetInit failed quit as it booted. The trophies (SceNpTrophy,
   vita/hle/dialog.c) and the licenses (SceNpDrm) are the console's own, not the network's */
#include "common/utilities/bits.h"
#include "vita/hle/hle.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

#define SCE_NET_ERROR_EBADF              0x80410109u /* 0x80410100 | errno, sceNet's form of each */
#define SCE_NET_ERROR_EFAULT             0x8041010eu
#define SCE_NET_ERROR_EINVAL             0x80410116u
#define SCE_NET_ERROR_EMFILE             0x80410118u
#define SCE_NET_ERROR_EWOULDBLOCK        0x80410123u
#define SCE_NET_ERROR_ENOTSOCK           0x80410126u
#define SCE_NET_ERROR_EMSGSIZE           0x80410128u
#define SCE_NET_ERROR_EADDRINUSE         0x80410130u
#define SCE_NET_ERROR_ENETDOWN           0x80410132u
#define SCE_NET_ERROR_ENETUNREACH        0x80410133u
#define SCE_NET_ERROR_ENOTCONN           0x80410139u
#define SCE_NET_ERROR_ECONNREFUSED       0x8041013du
#define SCE_NET_CTL_ERROR_NOT_CONNECTED  0x80412108u
#define SCE_HTTP_ERROR_NETWORK           0x80431063u
#define SCE_SSL_ERROR_INTERNAL           0x80435026u
#define SCE_NP_ERROR_INVALID_ARGUMENT    0x80550003u
#define SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT 0x80550503u
#define SCE_NP_MANAGER_ERROR_INVALID_STATE 0x80550506u
#define SCE_NP_UTIL_ERROR_INVALID_ARGUMENT 0x80550601u
#define SCE_NP_UTIL_ERROR_NOT_MATCH      0x80550609u
#define SCE_NET_CTL_ERROR_CALLBACK_MAX   0x80412103u
#define SCE_NET_CTL_ERROR_INVALID_ADDR   0x80412107u
#define SCE_NET_CTL_ERROR_FLIGHT_MODE_ENABLED 0x80412112u
#define SCE_NET_CTL_EVENT_TYPE_DISCONNECTED 1u
#define SCE_NETCTL_STATE_DISCONNECTED    0u
#define SCE_NP_SERVICE_STATE_SIGNED_OUT  1u

#define SCE_NET_SOCK_STREAM     1u
#define SCE_NET_SOCK_DGRAM      2u
#define SCE_NET_SOCK_DGRAM_P2P  6u
#define SCE_NET_SOL_SOCKET      0xffffu
#define SCE_NET_SO_ERROR        0x1007u
#define SCE_NET_SO_TYPE         0x1008u
#define SCE_NET_SO_NBIO         0x1100u
#define SCE_NET_MSG_DONTWAIT    0x80u
#define SCE_NET_EPOLLIN         1u
#define SCE_NET_EPOLLOUT        2u
#define SCE_NET_EPOLL_CTL_ADD   1u
#define SCE_NET_EPOLL_CTL_MOD   2u
#define SCE_NET_EPOLL_CTL_DEL   3u
#define LOOPBACK                0x7f000001u /* 127.0.0.1, host order */
#define EPHEMERAL               49152u
#define SOCKETS   64u
#define QUEUE     32u  /* the datagrams a socket holds unread; more are dropped, as UDP drops them */
#define EPOLLS    8u
#define WATCHES   32u
#define SLICE_NS  1000000u /* how often a blocked call looks again */

/* SceNetSockaddrIn (16 bytes): length, family, port (network order), address (network order), the P2P virtual port
   (network order), 6 zero bytes */
typedef struct {
    uint32_t address;       /* host order */
    uint16_t port, vport;   /* host order */
} Address;
typedef struct {
    unsigned char* bytes;
    uint32_t size;
    Address from;
} Datagram;
typedef struct {
    bool used, nbio, bound, connected;
    uint32_t type;
    Address local, peer;
    Datagram queue[QUEUE];
    unsigned head, count;
} Socket;
typedef struct {
    bool used;
    struct {
        uint32_t socket, events, data[2]; /* socket: its ID, 0 for a free watch */
    } watch[WATCHES];
} Epoll;
struct VitaNet {
    /* NP's service state callback: the game's function and its argument */
    struct {
        uint32_t callback, argument;
    } np;
    /* SceNetCtl's callbacks, by ID: the game's function (0 for a free slot) and its argument; told once that the
       network is disconnected */
    struct {
        uint32_t callback, argument;
    } netctl[8];
    bool netctl_told;
    Socket sockets[SOCKETS]; /* by ID minus 1 */
    Epoll epolls[EPOLLS];    /* by ID minus 1 */
    uint16_t next_port;
};

static bool prefix(const char* library, const char* start) { return !strncmp(library, start, strlen(start)); }
uint32_t vita_net_offline_error(const char* library) {
    if (prefix(library, "SceNetCtl")) return SCE_NET_CTL_ERROR_NOT_CONNECTED;
    if (prefix(library, "SceNet")) return SCE_NET_ERROR_ENETDOWN;
    if (prefix(library, "SceHttp")) return SCE_HTTP_ERROR_NETWORK;
    if (prefix(library, "SceSsl")) return SCE_SSL_ERROR_INTERNAL;
    if (prefix(library, "SceNp") && !prefix(library, "SceNpTrophy") && !prefix(library, "SceNpDrm"))
        return SCE_NP_MANAGER_ERROR_INVALID_STATE;
    return 0;
}
static struct VitaNet* net(VitaProcess* p) {
    if (!p->net) p->net = calloc(1, sizeof *p->net);
    return p->net;
}
static void drop(Socket* s) {
    for (unsigned i = 0; i < s->count; ++i) free(s->queue[(s->head + i) % QUEUE].bytes);
    memset(s, 0, sizeof *s);
}
void vita_net_free(VitaProcess* p) {
    if (!p->net) return;
    for (unsigned i = 0; i < SOCKETS; ++i) drop(&p->net->sockets[i]);
    free(p->net);
    p->net = NULL;
}

static uint32_t zero(VitaProcess* p, VitaThread* t) {
    (void)p, (void)t;
    return 0;
}
/* the guest is little-endian, so its network order is always a swap */
static uint32_t net_htonl(VitaProcess* p, VitaThread* t) { return snx_bswap32(vita_arg(p, t, 0)); }
static uint32_t net_htons(VitaProcess* p, VitaThread* t) { return snx_bswap16((uint16_t)vita_arg(p, t, 0)); }
/* sceNetErrnoLoc: the thread's */
static uint32_t net_errno_loc(VitaProcess* p, VitaThread* t) {
    (void)p;
    return t->tls + VITA_TLS_NET_ERRNO;
}
/* an error returned, its errno the thread's too */
static uint32_t fail(VitaProcess* p, VitaThread* t, uint32_t error) {
    vita_write32(p, t->tls + VITA_TLS_NET_ERRNO, error & 0xffu);
    return error;
}
/* The call made again once SLICE_NS (or less) passed: its SVC run again, with its arguments as they are then (r0 kept
   by the wait) */
static uint32_t again(VitaProcess* p, VitaThread* t, uint64_t ns) {
    t->cpu.r[15] -= t->cpu.cpsr & SNX_ARM_T ? 2u : 4u;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_DELAY, .a = 1, .deadline = vita_now_ns() + (ns ? ns : 1)});
    return 0;
}

/* ---- sockets ---- */
static Socket* socket_of(VitaProcess* p, uint32_t id) {
    struct VitaNet* n = net(p);
    return n && id - 1 < SOCKETS && n->sockets[id - 1].used ? &n->sockets[id - 1] : NULL;
}
/* a SceNetSockaddrIn (16 bytes) at at, read or written only where the game's memory is: address_writable says
   whether address_write can write at and length_at (each optional) */
static Address address_read(VitaProcess* p, uint32_t at) {
    const unsigned char* a = vita_ptr(p, at);
    return (Address){snx_ld_be32(a + 4), snx_ld_be16(a + 2), snx_ld_be16(a + 8)};
}
static bool address_writable(VitaProcess* p, uint32_t at, uint32_t length_at) {
    return (!at || vita_mapped(p, at, 16)) && (!length_at || vita_mapped(p, length_at, 4));
}
static void address_write(VitaProcess* p, uint32_t at, uint32_t length_at, Address a) {
    if (!at) return;
    unsigned char* to = vita_ptr(p, at);
    memset(to, 0, 16);
    to[0] = 16, to[1] = 2; /* AF_INET */
    snx_st_be16(to + 2, a.port), snx_st_be32(to + 4, a.address), snx_st_be16(to + 8, a.vport);
    if (length_at) vita_write32(p, length_at, 16);
}
static bool p2p(const Socket* s) { return s->type == SCE_NET_SOCK_DGRAM_P2P; }
/* the console's own address: the loopback network, or none (the console itself) */
static bool local(uint32_t address) { return !address || address >> 24 == 127; }
/* the socket bound where a datagram to a goes, from s */
static Socket* bound_at(VitaProcess* p, const Socket* s, Address a) {
    for (unsigned i = 0; i < SOCKETS; ++i) {
        Socket* to = &p->net->sockets[i];
        if (to->used && to->bound && p2p(to) == p2p(s) && to->type != SCE_NET_SOCK_STREAM &&
            to->local.port == a.port && (!p2p(s) || to->local.vport == a.vport))
            return to;
    }
    return NULL;
}
static void bind_any(VitaProcess* p, Socket* s) {
    if (s->bound) return;
    struct VitaNet* n = p->net;
    s->local = (Address){0, (uint16_t)(EPHEMERAL + n->next_port++ % 16000u), 0};
    s->bound = true;
}
/* sceNetSocket(name, domain, type, protocol) */
static uint32_t net_socket(VitaProcess* p, VitaThread* t) {
    struct VitaNet* n = net(p);
    uint32_t type = vita_arg(p, t, 2);
    if (!n) return fail(p, t, SCE_NET_ERROR_EMFILE);
    for (unsigned i = 0; i < SOCKETS; ++i)
        if (!n->sockets[i].used) {
            n->sockets[i] = (Socket){.used = true, .type = type};
            return i + 1;
        }
    return fail(p, t, SCE_NET_ERROR_EMFILE);
}
static uint32_t net_close(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    drop(s);
    return 0;
}
/* sceNetBind(s, address, length) */
static uint32_t net_bind(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    uint32_t at = vita_arg(p, t, 1);
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (!at || vita_arg(p, t, 2) < 16) return fail(p, t, SCE_NET_ERROR_EINVAL);
    if (!vita_mapped(p, at, 16)) return fail(p, t, SCE_NET_ERROR_EFAULT);
    Address a = address_read(p, at);
    if (!local(a.address)) return fail(p, t, SCE_NET_ERROR_EINVAL); /* EADDRNOTAVAIL: no other interface is up */
    if (a.port && s->type != SCE_NET_SOCK_STREAM && bound_at(p, s, a)) return fail(p, t, SCE_NET_ERROR_EADDRINUSE);
    s->local = a, s->bound = true;
    if (!a.port) s->bound = false, bind_any(p, s), s->local.vport = a.vport;
    return 0;
}
static uint32_t net_getsockname(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (!address_writable(p, vita_arg(p, t, 1), vita_arg(p, t, 2))) return fail(p, t, SCE_NET_ERROR_EFAULT);
    address_write(p, vita_arg(p, t, 1), vita_arg(p, t, 2), s->local);
    return 0;
}
/* sceNetSetsockopt(s, level, option, value, length): non-blocking or not; the others are taken and change nothing */
static uint32_t net_setsockopt(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    uint32_t level = vita_arg(p, t, 1), option = vita_arg(p, t, 2), value = vita_arg(p, t, 3);
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (value && !vita_mapped(p, value, 4)) return fail(p, t, SCE_NET_ERROR_EFAULT);
    if (level == SCE_NET_SOL_SOCKET && option == SCE_NET_SO_NBIO && value) s->nbio = vita_read32(p, value) != 0;
    return 0;
}
static uint32_t net_getsockopt(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    uint32_t level = vita_arg(p, t, 1), option = vita_arg(p, t, 2), value = vita_arg(p, t, 3);
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    uint32_t v = level != SCE_NET_SOL_SOCKET    ? 0
                 : option == SCE_NET_SO_TYPE    ? s->type
                 : option == SCE_NET_SO_NBIO    ? s->nbio
                                                : 0; /* SO_ERROR: none pending */
    uint32_t length = vita_arg(p, t, 4);
    if ((value && !vita_mapped(p, value, 4)) || (length && !vita_mapped(p, length, 4)))
        return fail(p, t, SCE_NET_ERROR_EFAULT);
    if (value) vita_write32(p, value, v);
    if (length) vita_write32(p, length, 4);
    return 0;
}
/* a datagram of size bytes at data from s to a: queued for the socket bound there (dropped when none is, or its
   queue is full, as UDP drops it) */
static uint32_t send_to(VitaProcess* p, VitaThread* t, Socket* s, uint32_t data, uint32_t size, Address a) {
    if (s->type == SCE_NET_SOCK_STREAM) return fail(p, t, SCE_NET_ERROR_ENOTCONN);
    if (!local(a.address)) return fail(p, t, SCE_NET_ERROR_ENETUNREACH);
    if (size > 0x10000u) return fail(p, t, SCE_NET_ERROR_EMSGSIZE);
    if (size && !vita_mapped(p, data, size)) return fail(p, t, SCE_NET_ERROR_EFAULT);
    bind_any(p, s);
    Socket* to = bound_at(p, s, a);
    if (to && to->count < QUEUE) {
        Datagram* d = &to->queue[(to->head + to->count) % QUEUE];
        d->bytes = malloc(size ? size : 1);
        if (d->bytes) {
            memcpy(d->bytes, vita_ptr(p, data), size);
            d->size = size, d->from = (Address){LOOPBACK, s->local.port, s->local.vport};
            ++to->count;
        }
    }
    return size;
}
/* sceNetSendto(s, data, size, flags, address, length) */
static uint32_t net_sendto(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    uint32_t at = vita_arg(p, t, 4);
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (!at) {
        if (!s->connected) return fail(p, t, SCE_NET_ERROR_ENOTCONN);
        return send_to(p, t, s, vita_arg(p, t, 1), vita_arg(p, t, 2), s->peer);
    }
    if (!vita_mapped(p, at, 16)) return fail(p, t, SCE_NET_ERROR_EFAULT);
    return send_to(p, t, s, vita_arg(p, t, 1), vita_arg(p, t, 2), address_read(p, at));
}
/* sceNetSend(s, data, size, flags) */
static uint32_t net_send(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (!s->connected) return fail(p, t, SCE_NET_ERROR_ENOTCONN);
    return send_to(p, t, s, vita_arg(p, t, 1), vita_arg(p, t, 2), s->peer);
}
/* the next datagram into data (size bytes, the rest dropped), its sender at address; none yet: EWOULDBLOCK for a
   non-blocking socket or call, else the call waits for one */
static uint32_t receive(VitaProcess* p, VitaThread* t, Socket* s, uint32_t data, uint32_t size, uint32_t flags,
                        uint32_t address, uint32_t length) {
    if (s->type == SCE_NET_SOCK_STREAM && !s->connected) return fail(p, t, SCE_NET_ERROR_ENOTCONN);
    if (!s->count) {
        if (s->nbio || (flags & SCE_NET_MSG_DONTWAIT)) return fail(p, t, SCE_NET_ERROR_EWOULDBLOCK);
        return again(p, t, SLICE_NS);
    }
    Datagram* d = &s->queue[s->head];
    uint32_t n = d->size < size ? d->size : size;
    /* a buffer or address outside the game's memory: the datagram stays queued */
    if ((n && !vita_mapped(p, data, n)) || !address_writable(p, address, length))
        return fail(p, t, SCE_NET_ERROR_EFAULT);
    memcpy(vita_ptr(p, data), d->bytes, n);
    address_write(p, address, length, d->from);
    free(d->bytes);
    *d = (Datagram){0};
    s->head = (s->head + 1) % QUEUE, --s->count;
    return n;
}
/* sceNetRecvfrom(s, data, size, flags, address, length*) */
static uint32_t net_recvfrom(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    return receive(p, t, s, vita_arg(p, t, 1), vita_arg(p, t, 2), vita_arg(p, t, 3), vita_arg(p, t, 4),
                   vita_arg(p, t, 5));
}
static uint32_t net_recv(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    return receive(p, t, s, vita_arg(p, t, 1), vita_arg(p, t, 2), vita_arg(p, t, 3), 0, 0);
}
/* sceNetConnect(s, address, length): a datagram socket's peer; a stream finds no one listening on the console */
static uint32_t net_connect(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    uint32_t at = vita_arg(p, t, 1);
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (!at) return fail(p, t, SCE_NET_ERROR_EINVAL);
    if (!vita_mapped(p, at, 16)) return fail(p, t, SCE_NET_ERROR_EFAULT);
    Address a = address_read(p, at);
    if (!local(a.address)) return fail(p, t, SCE_NET_ERROR_ENETUNREACH);
    if (s->type == SCE_NET_SOCK_STREAM) return fail(p, t, SCE_NET_ERROR_ECONNREFUSED);
    bind_any(p, s);
    s->peer = a, s->connected = true;
    return 0;
}
/* sceNetAccept(s, address, length): no one connects */
static uint32_t net_accept(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (s->nbio) return fail(p, t, SCE_NET_ERROR_EWOULDBLOCK);
    return again(p, t, SLICE_NS);
}
static uint32_t net_listen(VitaProcess* p, VitaThread* t) {
    Socket* s = socket_of(p, vita_arg(p, t, 0));
    if (!s) return fail(p, t, SCE_NET_ERROR_EBADF);
    bind_any(p, s);
    return 0;
}

/* ---- epoll ---- */
static Epoll* epoll_of(VitaProcess* p, uint32_t id) {
    struct VitaNet* n = net(p);
    return n && id - 1 < EPOLLS && n->epolls[id - 1].used ? &n->epolls[id - 1] : NULL;
}
/* sceNetEpollCreate(name, flags) */
static uint32_t epoll_create(VitaProcess* p, VitaThread* t) {
    struct VitaNet* n = net(p);
    for (unsigned i = 0; n && i < EPOLLS; ++i)
        if (!n->epolls[i].used) {
            n->epolls[i] = (Epoll){.used = true};
            return i + 1;
        }
    return fail(p, t, SCE_NET_ERROR_EMFILE);
}
static uint32_t epoll_destroy(VitaProcess* p, VitaThread* t) {
    Epoll* e = epoll_of(p, vita_arg(p, t, 0));
    if (!e) return fail(p, t, SCE_NET_ERROR_EBADF);
    memset(e, 0, sizeof *e);
    return 0;
}
/* sceNetEpollControl(id, operation, socket, SceNetEpollEvent*): the event's events (at 0) and data (at 0x18) */
static uint32_t epoll_control(VitaProcess* p, VitaThread* t) {
    Epoll* e = epoll_of(p, vita_arg(p, t, 0));
    uint32_t op = vita_arg(p, t, 1), id = vita_arg(p, t, 2), event = vita_arg(p, t, 3);
    if (!e) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (!socket_of(p, id)) return fail(p, t, SCE_NET_ERROR_ENOTSOCK);
    if (event && !vita_mapped(p, event, 0x20)) return fail(p, t, SCE_NET_ERROR_EFAULT);
    unsigned i = 0, free_watch = WATCHES;
    for (; i < WATCHES && e->watch[i].socket != id; ++i)
        if (!e->watch[i].socket && free_watch == WATCHES) free_watch = i;
    if (op == SCE_NET_EPOLL_CTL_DEL) {
        if (i < WATCHES) e->watch[i].socket = 0;
        return 0;
    }
    if (op != SCE_NET_EPOLL_CTL_ADD && op != SCE_NET_EPOLL_CTL_MOD) return fail(p, t, SCE_NET_ERROR_EINVAL);
    if (i == WATCHES && (op == SCE_NET_EPOLL_CTL_MOD || (i = free_watch) == WATCHES))
        return fail(p, t, SCE_NET_ERROR_EINVAL);
    e->watch[i].socket = id;
    e->watch[i].events = event ? vita_read32(p, event) : 0;
    e->watch[i].data[0] = event ? vita_read32(p, event + 0x18) : 0;
    e->watch[i].data[1] = event ? vita_read32(p, event + 0x1c) : 0;
    return 0;
}
/* sceNetEpollWait(id, SceNetEpollEvent* events, count, timeout): the watched sockets with a datagram to read (or
   room to send, always); none yet: the call waits, timeout microseconds at most (forever for a negative one) */
static uint32_t epoll_wait(VitaProcess* p, VitaThread* t) {
    Epoll* e = epoll_of(p, vita_arg(p, t, 0));
    uint32_t events = vita_arg(p, t, 1), count = vita_arg(p, t, 2);
    int32_t timeout = (int32_t)vita_arg(p, t, 3);
    if (!e) return fail(p, t, SCE_NET_ERROR_EBADF);
    if (count > WATCHES) count = WATCHES; /* no more can be ready */
    if (count && !vita_mapped(p, events, count * 0x20u)) return fail(p, t, SCE_NET_ERROR_EFAULT);
    uint32_t ready = 0;
    for (unsigned i = 0; i < WATCHES && ready < count; ++i) {
        Socket* s = e->watch[i].socket ? socket_of(p, e->watch[i].socket) : NULL;
        if (!s) continue;
        uint32_t got = (e->watch[i].events & SCE_NET_EPOLLIN && s->count ? SCE_NET_EPOLLIN : 0) |
                       (e->watch[i].events & SCE_NET_EPOLLOUT && s->type != SCE_NET_SOCK_STREAM ? SCE_NET_EPOLLOUT : 0);
        if (!got) continue;
        uint32_t to = events + ready++ * 0x20u;
        memset(vita_ptr(p, to), 0, 0x20);
        vita_write32(p, to, got);
        vita_write32(p, to + 0x18, e->watch[i].data[0]);
        vita_write32(p, to + 0x1c, e->watch[i].data[1]);
    }
    if (ready || !timeout) return ready;
    if (timeout > 0) t->cpu.r[3] = timeout > (int32_t)(SLICE_NS / 1000u) ? (uint32_t)timeout - SLICE_NS / 1000u : 0;
    return again(p, t, timeout > 0 && (uint32_t)timeout < SLICE_NS / 1000u ? (uint64_t)timeout * 1000u : SLICE_NS);
}

/* ---- the connection's state ---- */
/* sceNetCtlInetGetState(int* state) */
static uint32_t net_ctl_state(VitaProcess* p, VitaThread* t) {
    uint32_t state = vita_arg(p, t, 0);
    if (state && !vita_mapped(p, state, 4)) return SCE_NET_CTL_ERROR_INVALID_ADDR;
    if (state) vita_write32(p, state, SCE_NETCTL_STATE_DISCONNECTED);
    return 0;
}
/* sceNetCtlInetGetResult(event, int* error): why the network went, for each event the callbacks are told */
static uint32_t net_ctl_result(VitaProcess* p, VitaThread* t) {
    uint32_t error = vita_arg(p, t, 1);
    if (!error || !vita_mapped(p, error, 4)) return SCE_NET_CTL_ERROR_INVALID_ADDR;
    vita_write32(p, error, SCE_NET_CTL_ERROR_FLIGHT_MODE_ENABLED);
    return 0;
}
/* sceNetCtlInetRegisterCallback(callback, argument, int* id) */
static uint32_t net_ctl_register_callback(VitaProcess* p, VitaThread* t) {
    struct VitaNet* n = net(p);
    uint32_t callback = vita_arg(p, t, 0), argument = vita_arg(p, t, 1), id = vita_arg(p, t, 2);
    if (!callback || !id || !vita_mapped(p, id, 4)) return SCE_NET_CTL_ERROR_INVALID_ADDR;
    for (uint32_t i = 0; n && i < sizeof n->netctl / sizeof *n->netctl; ++i)
        if (!n->netctl[i].callback) {
            n->netctl[i].callback = callback, n->netctl[i].argument = argument;
            vita_write32(p, id, i);
            return 0;
        }
    return SCE_NET_CTL_ERROR_CALLBACK_MAX;
}
/* sceNetCtlInetUnregisterCallback(id) */
static uint32_t net_ctl_unregister_callback(VitaProcess* p, VitaThread* t) {
    struct VitaNet* n = net(p);
    uint32_t id = vita_arg(p, t, 0);
    if (n && id < sizeof n->netctl / sizeof *n->netctl) n->netctl[id].callback = 0;
    return 0;
}
/* sceNetCtlCheckCallback: the network's state changed once, from none to disconnected, which the first check tells
   the callbacks registered then: callback(DISCONNECTED, argument), one after another (context: the next ID) */
static uint32_t net_ctl_tell(VitaProcess* p, VitaThread* t, void* context) {
    struct VitaNet* n = p->net;
    for (uintptr_t i = (uintptr_t)context; n && i < sizeof n->netctl / sizeof *n->netctl; ++i)
        if (n->netctl[i].callback) {
            const uint32_t args[2] = {SCE_NET_CTL_EVENT_TYPE_DISCONNECTED, n->netctl[i].argument};
            vita_hle_guest_call(p, t, n->netctl[i].callback, args, 2, net_ctl_tell, (void*)(i + 1));
            return 0;
        }
    return 0;
}
static uint32_t net_ctl_check_callback(VitaProcess* p, VitaThread* t) {
    struct VitaNet* n = net(p);
    if (!n || n->netctl_told) return 0;
    n->netctl_told = true;
    return net_ctl_tell(p, t, NULL);
}
/* sceNpGetServiceState(SceNpServiceState* state) */
static uint32_t np_service_state(VitaProcess* p, VitaThread* t) {
    uint32_t state = vita_arg(p, t, 0);
    if (state && !vita_mapped(p, state, 4)) return SCE_NP_ERROR_INVALID_ARGUMENT;
    if (state) vita_write32(p, state, SCE_NP_SERVICE_STATE_SIGNED_OUT);
    return 0;
}
/* sceNpRegisterServiceStateCallback(callback, argument): one at a time, which each sceNpCheckCallback tells the
   state, callback(state, 0, argument) */
static uint32_t np_register_state_callback(VitaProcess* p, VitaThread* t) {
    struct VitaNet* n = net(p);
    if (n) n->np.callback = vita_arg(p, t, 0), n->np.argument = vita_arg(p, t, 1);
    return 0;
}
static uint32_t np_unregister_state_callback(VitaProcess* p, VitaThread* t) {
    (void)t;
    if (p->net) p->net->np.callback = 0;
    return 0;
}
static uint32_t np_checked(VitaProcess* p, VitaThread* t, void* context) {
    (void)p, (void)t, (void)context;
    return 0;
}
static uint32_t np_check_callback(VitaProcess* p, VitaThread* t) {
    struct VitaNet* n = p->net;
    if (!n || !n->np.callback) return 0;
    const uint32_t args[3] = {SCE_NP_SERVICE_STATE_SIGNED_OUT, 0, n->np.argument};
    vita_hle_guest_call(p, t, n->np.callback, args, 3, np_checked, NULL);
    return 0;
}
/* SceNpId (0x24 bytes): the online ID (16 characters, a NUL, 3 bytes of padding), 4 bytes, the platform ("psp2"),
   whether the ID is valid (a byte), 7 reserved */
#define NP_ID_SIZE     0x24u
#define NP_ID_PLATFORM 0x18u
#define NP_ID_VALID    0x1cu
/* sceNpManagerGetNpId(SceNpId* id): the account's, named as sceAppUtilSystemParamGetString names the user
   (vita/hle/misc.c) */
static uint32_t np_get_id(VitaProcess* p, VitaThread* t) {
    uint32_t id = vita_arg(p, t, 0);
    if (!id) return SCE_NP_MANAGER_ERROR_INVALID_STATE;
    if (!vita_mapped(p, id, NP_ID_SIZE)) return SCE_NP_MANAGER_ERROR_INVALID_ARGUMENT;
    unsigned char* to = vita_ptr(p, id);
    memset(to, 0, NP_ID_SIZE);
    memcpy(to, "Player", 6);
    memcpy(to + NP_ID_PLATFORM, "psp2", 4);
    to[NP_ID_VALID] = 1;
    return 0;
}
/* sceNpCmpNpId(const SceNpId* a, const SceNpId* b): 0 for the same ID, the online IDs, the 4 bytes after them and the
   platforms (unless neither names one) compared. Whether an ID is valid (its byte at 0x1c) isn't: a game signed out
   makes its own player's ID ("Offline_Player", every other byte zero), and its level data names creators the same way,
   and it finds them by comparing those */
static uint32_t np_compare_ids(VitaProcess* p, VitaThread* t) {
    uint32_t x = vita_arg(p, t, 0), y = vita_arg(p, t, 1);
    if (!x || !y || !vita_mapped(p, x, NP_ID_SIZE) || !vita_mapped(p, y, NP_ID_SIZE))
        return SCE_NP_UTIL_ERROR_INVALID_ARGUMENT;
    const unsigned char *a = vita_ptr(p, x), *b = vita_ptr(p, y);
    if (strncmp((const char*)a, (const char*)b, 16) || memcmp(a + 0x14, b + 0x14, 4))
        return SCE_NP_UTIL_ERROR_NOT_MATCH;
    if ((a[NP_ID_PLATFORM] || b[NP_ID_PLATFORM]) && memcmp(a + NP_ID_PLATFORM, b + NP_ID_PLATFORM, 4))
        return SCE_NP_UTIL_ERROR_NOT_MATCH;
    return 0;
}

const VitaHle vita_hle_net[] = {
    {0xeb03e265u, "sceNetInit", zero},
    {0xea3cc286u, "sceNetTerm", zero},
    {0xf084fce3u, "sceNetSocket", net_socket},
    {0x29822b4du, "sceNetSocketClose", net_close},
    {0x891c1b9bu, "sceNetSocketAbort", zero},
    {0x1296a94bu, "sceNetBind", net_bind},
    {0x7a8da094u, "sceNetListen", net_listen},
    {0x1adf9bb1u, "sceNetAccept", net_accept},
    {0x11e5b6f6u, "sceNetConnect", net_connect},
    {0x52db31d5u, "sceNetSendto", net_sendto},
    {0xe3dd8cd9u, "sceNetSend", net_send},
    {0xb226138bu, "sceNetRecvfrom", net_recvfrom},
    {0x023643b7u, "sceNetRecv", net_recv},
    {0x1c66a6dbu, "sceNetGetsockname", net_getsockname},
    {0x065505cau, "sceNetSetsockopt", net_setsockopt},
    {0xba652062u, "sceNetGetsockopt", net_getsockopt},
    {0xe37f34aau, "sceNetErrnoLoc", net_errno_loc},
    {0xf9d102aeu, "sceNetEpollCreate", epoll_create},
    {0x4c8764acu, "sceNetEpollControl", epoll_control},
    {0x45ce337du, "sceNetEpollWait", epoll_wait},
    {0x7915caf3u, "sceNetEpollDestroy", epoll_destroy},
    {0x93fcc4e8u, "sceNetEpollAbort", zero},
    {0x4c30b03cu, "sceNetHtonl", net_htonl},
    {0x9fa3207bu, "sceNetHtons", net_htons},
    {0xd2eaa645u, "sceNetNtohl", net_htonl},
    {0x07845128u, "sceNetNtohs", net_htons},
    {0xd5eeb048u, "sceNetInetPton", zero}, /* 0: not an address */
    {0x98839b74u, "sceNetInetNtop", zero}, /* NULL: no text */
    {0x495ca1dbu, "sceNetCtlInit", zero},
    {0xcd188648u, "sceNetCtlTerm", zero},
    {0x6d26ac68u, "sceNetCtlInetGetState", net_ctl_state},
    {0x6b20ec02u, "sceNetCtlInetGetResult", net_ctl_result},
    {0xeaee6185u, "sceNetCtlInetRegisterCallback", net_ctl_register_callback},
    {0xd0c3bf3fu, "sceNetCtlInetUnregisterCallback", net_ctl_unregister_callback},
    {0xdffc3ed4u, "sceNetCtlCheckCallback", net_ctl_check_callback},
    {0x214926d9u, "sceHttpInit", zero},
    {0xc9076666u, "sceHttpTerm", zero},
    {0x3c733316u, "sceSslInit", zero},
    {0x03ce6e3au, "sceSslTerm", zero},
    {0x04d9f484u, "sceNpInit", zero},
    {0x19e40ae1u, "sceNpTerm", zero},
    {0x54060df6u, "sceNpGetServiceState", np_service_state},
    {0x44239c35u, "sceNpRegisterServiceStateCallback", np_register_state_callback},
    {0xd9e6e56cu, "sceNpUnregisterServiceStateCallback", np_unregister_state_callback},
    {0x3b0ae9a9u, "sceNpCheckCallback", np_check_callback},
    {0x3c94b4b4u, "sceNpManagerGetNpId", np_get_id},
    {0xfb8d82e5u, "sceNpCmpNpId", np_compare_ids},
    {0x441d8b4eu, "sceNpAuthInit", zero},
    {0x6093b689u, "sceNpAuthTerm", zero},
    {0xe807d0bcu, "sceNpSetPlatformType", zero},
    {0x4b6acf47u, "sceNpSignalingInit", zero},
    {0xbc892d18u, "sceNpSignalingTerm", zero},
    {0xc73f209au, "sceNpCommerce2Init", zero},
    {0xb99958aeu, "sceNpCommerce2Term", zero},
    {0x01284c6eu, "sceNpWordFilterInit", zero},
    {0x7d140f70u, "sceNpWordFilterTerm", zero},
    {0x9246a673u, "sceNpLookupInit", zero},
    {0x0158b61bu, "sceNpLookupTerm", zero},
    {0xefb91a99u, "sceNpBasicInit", zero},
    {0x389bcb3bu, "sceNpBasicTerm", zero},
    {0x20146aecu, "sceNpBasicCheckCallback", zero},
    {0x258da4acu, "sceNpMessageInit", zero},
    {0x380230a1u, "sceNpMessageTerm", zero},
    {0xe0ffee97u, "sceNpActivityInit", zero},
    {0x9ea4901fu, "sceNpActivityTerm", zero},
    {0, NULL, NULL},
};
