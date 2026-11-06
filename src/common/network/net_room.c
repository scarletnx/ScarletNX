/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The protocol follows Eden's src/network/room.cpp, room_member.cpp and packet.cpp, and the datagram rules its
   src/core/internal_network/socket_proxy.cpp keeps (SPDX-FileCopyrightText: Copyright 2017 Citra Emulator Project,
   2022 yuzu Emulator Project, GPL-2.0-or-later; Copyright 2026 Eden Emulator Project, GPL-3.0-or-later) */
/* A member of an Eden room (net_room.h). The join runs on the caller's thread; from then on the member's thread owns
   ENet: it sends what the calls queued, services the connection, hands ldn's messages to the handler and the
   datagrams to the sockets. The lock order is the member's, then a socket's.

   The log names a member by its address in the room, never by its nickname, and a chat message by its size, never
   its text: players attach the log to public reports, and both can name them */
#include "common/network/net_room.h"
#include "common/network/net_internal.h"
#include "common/utilities/endian.h"
#include <enet/enet.h>
#include <zstd.h>
#include <stdlib.h>
#include <string.h>

/* the room's message types (Eden's RoomMessageTypes) */
enum {
    ID_JOIN_REQUEST = 1,
    ID_JOIN_SUCCESS = 2,
    ID_ROOM_INFORMATION = 3,
    ID_SET_GAME_INFO = 4,
    ID_PROXY_PACKET = 5,
    ID_LDN_PACKET = 6,
    ID_CHAT_MESSAGE = 7,
    ID_NAME_COLLISION = 8,
    ID_IP_COLLISION = 9,
    ID_VERSION_MISMATCH = 10,
    ID_WRONG_PASSWORD = 11,
    ID_CLOSE_ROOM = 12,
    ID_ROOM_IS_FULL = 13,
    ID_STATUS_MESSAGE = 14,
    ID_HOST_KICKED = 15,
    ID_HOST_BANNED = 16,
    ID_JOIN_SUCCESS_AS_MOD = 24
};
/* Eden's Domain and Protocol, as a datagram carries them */
enum { ROOM_INET = 1, ROOM_PROTOCOL_IP = 1, ROOM_PROTOCOL_UDP = 4 };
#define ROOM_MEMBERS                 254 /* MaxConcurrentConnections */
#define ROOM_SERVICE_MS              5
#define ROOM_LEAVE_MS                1000 /* for the room to take what was queued, then the disconnect */
#define ROOM_QUEUE_MAX               256  /* ordinary guest UDP sockets */
/* UDS transports reliable local-wireless inputs over this room socket. Retain
   bursts while its caller is stalled; dropping here cannot be repaired by ENet,
   which already acknowledged delivery. Bound both packet count and payload bytes. */
#define ROOM_LOW_LATENCY_QUEUE_MAX   2048
#define ROOM_LOW_LATENCY_QUEUE_BYTES (4u * 1024u * 1024u)
#define ROOM_DATAGRAM_HEAD                                                                                             \
    21 /* a datagram message before its bytes: type, two endpoints, protocol, broadcast, length */
#define ROOM_PORT_FIRST 49152 /* the ephemeral ports an unbound socket takes */

typedef struct RoomOut {
    struct RoomOut* next;
    size_t size;
    uint8_t data[];
} RoomOut;
typedef struct RoomMember {
    uint8_t ip[4];
    char nickname[SNX_ROOM_NICKNAME_MAX + 1];
} RoomMember;
typedef struct RoomSock RoomSock;

struct SnxRoom {
    SnxLock lk;
    ENetHost* host;
    ENetPeer* peer;
    uint8_t fake_ip[4];
    int state; /* SNX_ROOM_IN, or why it went away */
    int refs;  /* snx_room_leave's, each socket's and each snx_room_ref's */
    int stop, done, running;
    int low_latency;
    RoomOut *out, *out_tail; /* queued for the thread to send */
    RoomMember members[ROOM_MEMBERS];
    int nmembers;
    void (*ldn)(void*, const SnxRoomLdn*);
    void* ldn_arg;
    int in_ldn; /* the thread is in the handler */
    RoomSock* socks;
    uint16_t next_port;
};

/* ============================================================ the wire */
typedef struct Out {
    uint8_t* p;
    size_t n, max;
} Out;
static void put(Out* o, const void* v, size_t n) {
    /* n 0: a packet with no data names none, which memcpy can't take even for 0 bytes */
    if (n && o->n <= o->max && n <= o->max - o->n) memcpy(o->p + o->n, v, n);
    o->n += n; /* past max: the caller sees it didn't fit */
}
static void put8(Out* o, uint8_t v) { put(o, &v, 1); }
static void put16(Out* o, uint16_t v) {
    uint8_t b[2];
    snx_st_be16(b, v);
    put(o, b, 2);
}
static void put32(Out* o, uint32_t v) {
    uint8_t b[4];
    snx_st_be32(b, v);
    put(o, b, 4);
}
static void put64(Out* o, uint64_t v) {
    put32(o, (uint32_t)(v >> 32));
    put32(o, (uint32_t)v);
}
static void put_bytes(Out* o, const void* v, size_t n) { /* a string or a vector<u8> */
    put32(o, (uint32_t)n);
    put(o, v, n);
}
static size_t done_out(const Out* o) { return o->n <= o->max ? o->n : 0; }

typedef struct In {
    const uint8_t* p;
    size_t n, at;
    int bad;
} In;
static const uint8_t* get(In* i, size_t n) {
    if (i->bad || n > i->n - i->at) {
        i->bad = 1;
        return NULL;
    }
    const uint8_t* v = i->p + i->at;
    i->at += n;
    return v;
}
static uint8_t get8(In* i) {
    const uint8_t* v = get(i, 1);
    return v ? v[0] : 0;
}
static uint16_t get16(In* i) {
    const uint8_t* v = get(i, 2);
    return v ? snx_ld_be16(v) : 0;
}
static uint32_t get32(In* i) {
    const uint8_t* v = get(i, 4);
    return v ? snx_ld_be32(v) : 0;
}
static void get_ip(In* i, uint8_t ip[4]) {
    const uint8_t* v = get(i, 4);
    if (v) memcpy(ip, v, 4);
}
/* a string or a vector<u8>: its bytes inside the message */
static const uint8_t* get_bytes(In* i, size_t* n) {
    *n = get32(i);
    return get(i, *n);
}
/* a string into text (cut to size - 1) */
static void get_text(In* i, char* text, size_t size) {
    size_t n;
    const uint8_t* v = get_bytes(i, &n);
    if (n > size - 1) n = size - 1;
    if (v) memcpy(text, v, n);
    text[v ? n : 0] = 0;
}

size_t snx_room_pack_join(uint8_t* out, size_t max, const char* nickname, const char* password) {
    static const uint8_t no_preferred_ip[4] = {255, 255, 255, 255}; /* NoPreferredIP: the room picks one */
    Out o;
    o.p = out;
    o.n = 0;
    o.max = max;
    put8(&o, ID_JOIN_REQUEST);
    put_bytes(&o, nickname, strlen(nickname));
    put(&o, no_preferred_ip, 4);
    put32(&o, SNX_ROOM_VERSION);
    put_bytes(&o, password, strlen(password));
    put_bytes(&o, "", 0); /* the token of a verified user: none */
    return done_out(&o);
}
static size_t pack_game_info(uint8_t* out, size_t max, const char* name, uint64_t id, const char* version) {
    Out o;
    o.p = out;
    o.n = 0;
    o.max = max;
    put8(&o, ID_SET_GAME_INFO);
    put_bytes(&o, name, strlen(name));
    put64(&o, id);
    put_bytes(&o, version, strlen(version));
    return done_out(&o);
}
size_t snx_room_pack_ldn(uint8_t* out, size_t max, const SnxRoomLdn* p) {
    Out o;
    o.p = out;
    o.n = 0;
    o.max = max;
    put8(&o, ID_LDN_PACKET);
    put8(&o, p->type);
    put(&o, p->local_ip, 4);
    put(&o, p->remote_ip, 4);
    put8(&o, (uint8_t)!!p->broadcast);
    put_bytes(&o, p->data, p->size);
    return done_out(&o);
}
int snx_room_parse_ldn(const uint8_t* in, size_t size, SnxRoomLdn* p) {
    In i;
    i.p = in;
    i.n = size;
    i.at = 0;
    i.bad = 0;
    if (get8(&i) != ID_LDN_PACKET) return 0;
    p->type = get8(&i);
    get_ip(&i, p->local_ip);
    get_ip(&i, p->remote_ip);
    p->broadcast = get8(&i) != 0;
    p->data = get_bytes(&i, &p->size);
    return !i.bad && i.at == i.n;
}
/* the protocol byte is the socket's (room_protocol), both endpoints INET */
static size_t pack_datagram(uint8_t* out, size_t max, const SnxRoomDatagram* d, uint8_t protocol) {
    Out o;
    o.p = out;
    o.n = 0;
    o.max = max;
    put8(&o, ID_PROXY_PACKET);
    put8(&o, ROOM_INET);
    put(&o, d->local_ip, 4);
    put16(&o, d->local_port);
    put8(&o, ROOM_INET);
    put(&o, d->remote_ip, 4);
    put16(&o, d->remote_port);
    put8(&o, protocol);
    put8(&o, (uint8_t)!!d->broadcast);
    put_bytes(&o, d->data, d->size);
    return done_out(&o);
}
size_t snx_room_pack_datagram(uint8_t* out, size_t max, const SnxRoomDatagram* d) {
    return pack_datagram(out, max, d, ROOM_PROTOCOL_UDP);
}
static int parse_datagram(const uint8_t* in, size_t size, SnxRoomDatagram* d, uint8_t* protocol) {
    In i;
    i.p = in;
    i.n = size;
    i.at = 0;
    i.bad = 0;
    if (get8(&i) != ID_PROXY_PACKET) return 0;
    if (get8(&i) != ROOM_INET) return 0;
    get_ip(&i, d->local_ip);
    d->local_port = get16(&i);
    if (get8(&i) != ROOM_INET) return 0;
    get_ip(&i, d->remote_ip);
    d->remote_port = get16(&i);
    *protocol = get8(&i);
    d->broadcast = get8(&i) != 0;
    d->data = get_bytes(&i, &d->size);
    return !i.bad && i.at == i.n;
}
int snx_room_parse_datagram(const uint8_t* in, size_t size, SnxRoomDatagram* d) {
    uint8_t protocol;
    return parse_datagram(in, size, d, &protocol);
}

/* ============================================================ the member's state */
const char* snx_room_why_text(int why) {
    switch (why) {
        case SNX_ROOM_IN: return "in the room";
        case SNX_ROOM_NO_ADDRESS: return "the room's host name has no address";
        case SNX_ROOM_LOCAL: return "this computer couldn't open the connection to the room";
        case SNX_ROOM_UNREACHABLE: return "the room didn't answer at that address and port";
        case SNX_ROOM_NO_ANSWER: return "the room took the connection and never answered the join";
        case SNX_ROOM_NAME_COLLISION:
            return "the room has a member of that nickname already, or takes no nickname but 4 to 20 letters, digits, "
                   "spaces, '.', '_' or '-'";
        case SNX_ROOM_IP_COLLISION: return "the room has a member of that address already";
        case SNX_ROOM_VERSION_MISMATCH: return "the room speaks another version of the protocol";
        case SNX_ROOM_WRONG_PASSWORD: return "the room's password is another";
        case SNX_ROOM_FULL: return "the room is full";
        case SNX_ROOM_BANNED: return "the room has banned this address or user";
        case SNX_ROOM_KICKED: return "the room's host kicked this member";
        case SNX_ROOM_CLOSED: return "the room closed, or the connection to it dropped";
        default: return "unknown";
    }
}
void snx_room_fake_ip(const SnxRoom* r, uint8_t ip[4]) { memcpy(ip, r->fake_ip, 4); }
int snx_room_state(SnxRoom* r) {
    snx_lock(&r->lk);
    int s = r->state;
    snx_unlock(&r->lk);
    return s;
}
int snx_room_has_member(SnxRoom* r, const uint8_t ip[4]) {
    int found = 0;
    snx_lock(&r->lk);
    for (int i = 0; i < r->nmembers && !found; i++) found = !memcmp(r->members[i].ip, ip, 4);
    snx_unlock(&r->lk);
    return found;
}

/* the message, queued for the thread; -SNX_ENETDOWN out of the room */
/* o, queued for the thread, which frees it; freed here out of the room (-SNX_ENETDOWN) */
static int queue_owned(SnxRoom* r, RoomOut* o) {
    o->next = NULL;
    snx_lock(&r->lk);
    if (r->state != SNX_ROOM_IN) {
        snx_unlock(&r->lk);
        free(o);
        return -SNX_ENETDOWN;
    }
    if (r->out_tail)
        r->out_tail->next = o;
    else
        r->out = o;
    r->out_tail = o;
    snx_unlock(&r->lk);
    return 0;
}
static int queue(SnxRoom* r, const uint8_t* data, size_t size) {
    RoomOut* o = malloc(sizeof *o + size);
    if (!o) return -SNX_ENOMEM;
    o->size = size;
    memcpy(o->data, data, size);
    return queue_owned(r, o);
}
static void call_ldn(SnxRoom* r, const SnxRoomLdn* p) {
    void (*h)(void*, const SnxRoomLdn*);
    void* arg;
    snx_lock(&r->lk);
    h = r->ldn;
    arg = r->ldn_arg;
    r->in_ldn = h != NULL;
    snx_unlock(&r->lk);
    if (!h) return;
    h(arg, p);
    snx_lock(&r->lk);
    r->in_ldn = 0;
    snx_unlock(&r->lk);
}
int snx_room_set_ldn(SnxRoom* r, void (*handler)(void*, const SnxRoomLdn*), void* arg) {
    int e = 0;
    snx_lock(&r->lk);
    if (r->ldn && r->ldn_arg != arg)
        e = -SNX_EBUSY;
    else {
        r->ldn = handler;
        r->ldn_arg = arg;
    }
    snx_unlock(&r->lk);
    return e;
}
void snx_room_clear_ldn(SnxRoom* r, void* arg) {
    for (;;) {
        snx_lock(&r->lk);
        if (!r->ldn || r->ldn_arg != arg || !r->in_ldn) {
            if (r->ldn && r->ldn_arg == arg) r->ldn = NULL;
            snx_unlock(&r->lk);
            return;
        }
        snx_unlock(&r->lk);
        snx_net_sleep_us(1000);
    }
}
int snx_room_send_ldn(SnxRoom* r, uint8_t type, const uint8_t remote_ip[4], int broadcast, const void* data,
                      size_t size) {
    SnxRoomLdn p;
    p.type = type;
    memcpy(p.local_ip, r->fake_ip, 4);
    if (remote_ip)
        memcpy(p.remote_ip, remote_ip, 4);
    else
        memset(p.remote_ip, 0, 4);
    p.broadcast = broadcast;
    p.data = data;
    p.size = size;
    uint8_t* buf = malloc(size + 32);
    if (!buf) return -SNX_ENOMEM;
    size_t n = snx_room_pack_ldn(buf, size + 32, &p);
    int e = n ? queue(r, buf, n) : -SNX_EMSGSIZE;
    free(buf);
    return e;
}

/* ============================================================ the sockets */
typedef struct RoomDatagramIn {
    struct RoomDatagramIn* next;
    uint8_t ip[4];
    uint16_t port; /* host order */
    size_t size;
    uint8_t data[];
} RoomDatagramIn;
struct RoomSock {
    SnxRoom* room;
    RoomSock* next; /* the member's list, under its lock */
    SnxLock lk;
    SnxHostFd notify[2]; /* readable while a datagram waits */
    uint8_t protocol;    /* the room's protocol byte, which a datagram must match */
    uint8_t ip[4];
    uint16_t port; /* host order; 0 while unbound */
    uint8_t peer_ip[4];
    uint16_t peer_port;
    int connected, broadcast, reuse, shut_rd, shut_wr, sndbuf, rcvbuf;
    RoomDatagramIn *head, *tail;
    int queued;
    size_t queued_bytes;
    int warned_full, told_option;
};

static void sock_destroy(SnxSocket* s) {
    RoomSock* k = s->impl;
    SnxRoom* r = k->room;
    RoomSock** p;
    RoomDatagramIn* d;
    snx_lock(&r->lk);
    for (p = &r->socks; *p && *p != k; p = &(*p)->next) {}
    if (*p) *p = k->next;
    snx_unlock(&r->lk);
    snx_room_unref(r);
    while ((d = k->head)) {
        k->head = d->next;
        free(d);
    }
    snx_net_close_fd(k->notify[0]);
    snx_net_close_fd(k->notify[1]);
    snx_lock_destroy(&k->lk);
    free(k);
}
static int port_taken(SnxRoom* r, const RoomSock* k, uint16_t port, int reuse) {
    for (const RoomSock* o = r->socks; o; o = o->next)
        if (o != k && o->port == port && !(reuse && o->reuse)) return 1;
    return 0;
}
/* binds k to ip and port (0: a free one), with the member's lock taken here and the socket's inside */
static int sock_bind_to(RoomSock* k, const uint8_t ip[4], uint16_t port) {
    SnxRoom* r = k->room;
    int e = 0, tries;
    snx_lock(&r->lk);
    snx_lock(&k->lk);
    if (k->port)
        e = -SNX_EINVAL;
    else if (port) {
        if (port_taken(r, k, port, k->reuse)) e = -SNX_EADDRINUSE;
    } else {
        for (tries = 0; tries < 65536 - ROOM_PORT_FIRST; tries++) {
            port = r->next_port;
            r->next_port = (uint16_t)(port == 65535 ? ROOM_PORT_FIRST : port + 1);
            if (!port_taken(r, k, port, 0)) break;
        }
        if (tries == 65536 - ROOM_PORT_FIRST) e = -SNX_EADDRINUSE;
    }
    if (!e) {
        memcpy(k->ip, ip, 4);
        k->port = port;
    }
    snx_unlock(&k->lk);
    snx_unlock(&r->lk);
    return e;
}
static int sock_bind(SnxSocket* s, const SnxSockAddr* a) {
    RoomSock* k = s->impl;
    static const uint8_t any[4] = {0, 0, 0, 0};
    if (a->family != SNX_AF_INET) return -SNX_EAFNOSUPPORT;
    if (memcmp(a->addr, any, 4) && memcmp(a->addr, k->room->fake_ip, 4)) return -SNX_EADDRNOTAVAIL;
    return sock_bind_to(k, a->addr, snx_ld_be16(&a->port)); /* the port's bytes are in network order */
}
static int bound(RoomSock* k) {
    static const uint8_t any[4] = {0, 0, 0, 0};
    snx_lock(&k->lk);
    int b = k->port != 0;
    snx_unlock(&k->lk);
    return b || !sock_bind_to(k, any, 0) || k->port;
}
static int sock_connect(SnxSocket* s, const SnxSockAddr* a) {
    RoomSock* k = s->impl;
    if (a->family == SNX_AF_UNSPEC) {
        snx_lock(&k->lk);
        k->connected = 0;
        snx_unlock(&k->lk);
        return 0;
    }
    if (a->family != SNX_AF_INET) return -SNX_EAFNOSUPPORT;
    if (!bound(k)) return -SNX_EADDRINUSE;
    snx_lock(&k->lk);
    memcpy(k->peer_ip, a->addr, 4);
    k->peer_port = snx_ld_be16(&a->port);
    k->connected = 1;
    snx_unlock(&k->lk);
    return 0;
}
static int sock_shutdown(SnxSocket* s, int how) {
    RoomSock* k = s->impl;
    snx_lock(&k->lk);
    if (how == SNX_SHUT_RD || how == SNX_SHUT_RDWR) k->shut_rd = 1;
    if (how == SNX_SHUT_WR || how == SNX_SHUT_RDWR) k->shut_wr = 1;
    if (k->shut_rd) snx_notifier_signal(k->notify[1]); /* a blocked receive returns */
    snx_unlock(&k->lk);
    return 0;
}
static int64_t sock_send(SnxSocket* s, const void* data, size_t size, int flags, const SnxSockAddr* to) {
    RoomSock* k = s->impl;
    SnxRoom* r = k->room;
    SnxRoomDatagram d;
    size_t head;
    RoomOut* o;
    uint8_t protocol;
    int e;
    (void)flags;
    if (to && to->family != SNX_AF_INET) return -SNX_EAFNOSUPPORT;
    if (size > SNX_ROOM_DATAGRAM_MAX) return -SNX_EMSGSIZE;
    if (!bound(k)) return -SNX_EADDRINUSE;
    snx_lock(&k->lk);
    if (k->shut_wr)
        e = -SNX_EPIPE;
    else if (!to && !k->connected)
        e = -SNX_EDESTADDRREQ;
    else {
        e = 0;
        memcpy(d.remote_ip, to ? to->addr : k->peer_ip, 4);
        d.remote_port = to ? snx_ld_be16(&to->port) : k->peer_port;
        /* Eden's rule: any address ending in 255 is a broadcast, which needs SO_BROADCAST as on a LAN */
        d.broadcast = d.remote_ip[3] == 255;
        if (d.broadcast && !k->broadcast) e = -SNX_EACCES;
        if (k->ip[0] || k->ip[1] || k->ip[2] || k->ip[3])
            memcpy(d.local_ip, k->ip, 4);
        else
            memcpy(d.local_ip, r->fake_ip, 4);
        d.local_port = k->port;
    }
    protocol = k->protocol;
    snx_unlock(&k->lk);
    if (e) return e;
    /* the message's head with an empty vector, then the zstd frame where the vector's bytes go, then its length */
    size_t bound_size = ZSTD_compressBound(size);
    d.data = NULL;
    d.size = 0;
    head = ROOM_DATAGRAM_HEAD;
    o = malloc(sizeof *o + head + bound_size);
    if (!o) return -SNX_ENOMEM;
    if (pack_datagram(o->data, head, &d, protocol) != head) {
        free(o);
        return -SNX_EIO;
    }
    size_t n = ZSTD_compress(o->data + head, bound_size, data, size, ZSTD_CLEVEL_DEFAULT);
    if (ZSTD_isError(n)) {
        free(o);
        return -SNX_EIO;
    }
    snx_st_be32(o->data + head - 4, (uint32_t)n);
    o->size = head + n;
    e = queue_owned(r, o);
    return e ? e : (int64_t)size;
}
static int64_t sock_recv(SnxSocket* s, void* data, size_t size, int flags, SnxSockAddr* from, int* out_flags) {
    RoomSock* k = s->impl;
    int64_t deadline = snx_net_deadline(s->rcvtimeo_us);
    if (out_flags) *out_flags = 0;
    for (;;) {
        snx_lock(&k->lk);
        RoomDatagramIn* d = k->head;
        if (d) {
            size_t n = d->size < size ? d->size : size;
            int64_t got = (flags & SNX_MSG_TRUNC) ? (int64_t)d->size : (int64_t)n;
            memcpy(data, d->data, n);
            if (from) {
                memset(from, 0, sizeof *from);
                from->family = SNX_AF_INET;
                snx_st_be16(&from->port, d->port);
                memcpy(from->addr, d->ip, 4);
            }
            if (out_flags && d->size > size) *out_flags |= SNX_MSG_TRUNC;
            if (!(flags & SNX_MSG_PEEK)) {
                k->head = d->next;
                if (!k->head) {
                    k->tail = NULL;
                    if (!k->shut_rd) snx_notifier_drain(k->notify[0]);
                }
                k->queued--;
                k->queued_bytes -= d->size;
                free(d);
            }
            snx_unlock(&k->lk);
            return got;
        }
        int r = k->shut_rd;
        snx_unlock(&k->lk);
        if (r) return 0;
        if (s->nonblocking || (flags & SNX_MSG_DONTWAIT)) return -SNX_EAGAIN;
        r = snx_net_wait_until(k->notify[0], POLLIN, deadline);
        if (r < 0) return r;
    }
}
static int sock_name(SnxSocket* s, SnxSockAddr* out, int peer) {
    RoomSock* k = s->impl;
    int e = 0;
    memset(out, 0, sizeof *out);
    out->family = SNX_AF_INET;
    snx_lock(&k->lk);
    if (peer && !k->connected)
        e = -SNX_ENOTCONN;
    else {
        memcpy(out->addr, peer ? k->peer_ip : k->ip, 4);
        snx_st_be16(&out->port, peer ? k->peer_port : k->port);
    }
    snx_unlock(&k->lk);
    return e;
}
/* the int options a room socket keeps itself, or NULL */
static int* sock_int_option(RoomSock* k, int level, int name) {
    if (level != SNX_SOL_SOCKET) return NULL;
    switch (name) {
        case SNX_SO_BROADCAST: return &k->broadcast;
        case SNX_SO_REUSEADDR:
        case SNX_SO_REUSEPORT: return &k->reuse;
        case SNX_SO_SNDBUF: return &k->sndbuf;
        case SNX_SO_RCVBUF: return &k->rcvbuf;
        default: return NULL;
    }
}
static int sock_setsockopt(SnxSocket* s, int level, int name, const void* v, size_t size) {
    RoomSock* k = s->impl;
    int* o = sock_int_option(k, level, name);
    if (o) {
        int value;
        if (size < sizeof value) return -SNX_EINVAL;
        memcpy(&value, v, sizeof value);
        if (SNX_HOST_BE) value = (int)snx_le32((uint32_t)value); /* the guest's little-endian int */
        snx_lock(&k->room->lk);                                  /* port_taken reads reuse under the member's lock */
        snx_lock(&k->lk);
        *o = name == SNX_SO_SNDBUF || name == SNX_SO_RCVBUF ? value : value != 0;
        snx_unlock(&k->lk);
        snx_unlock(&k->room->lk);
        return 0;
    }
    if (snx_net_sockopt_kept(level, name)) return snx_net_host_setsockopt(s, level, name, v, size);
    /* the room has no TTL, multicast group or type of service to set: accepted as Eden's proxy socket accepts every
       option, and said, so a game that leans on one shows where */
    snx_lock(&k->lk);
    if (!k->told_option) snx_net_log("room: a socket option (level %d, name %d) has no effect in a room", level, name);
    k->told_option = 1; /* once a socket: a game sets its options on every one */
    snx_unlock(&k->lk);
    return 0;
}
static int sock_getsockopt(SnxSocket* s, int level, int name, void* v, size_t* size) {
    RoomSock* k = s->impl;
    int* o = sock_int_option(k, level, name);
    if (o) {
        int value;
        if (*size < sizeof value) return -SNX_EINVAL;
        snx_lock(&k->lk);
        value = *o;
        snx_unlock(&k->lk);
        if (SNX_HOST_BE) value = (int)snx_le32((uint32_t)value);
        memcpy(v, &value, sizeof value);
        *size = sizeof value;
        return 0;
    }
    if (level == SNX_SOL_SOCKET && name == SNX_SO_ERROR) {
        int none = 0;
        if (*size < sizeof none) return -SNX_EINVAL;
        memcpy(v, &none, sizeof none);
        *size = sizeof none;
        return 0;
    }
    if (snx_net_sockopt_kept(level, name)) return snx_net_host_getsockopt(s, level, name, v, size);
    return -SNX_ENOPROTOOPT;
}
static int sock_readable_bytes(SnxSocket* s, int* bytes) {
    RoomSock* k = s->impl;
    snx_lock(&k->lk);
    *bytes = k->head ? (int)k->head->size : 0;
    snx_unlock(&k->lk);
    return 0;
}
static uint16_t sock_poll_ready(SnxSocket* s, uint16_t events, short* host_events) {
    (void)s;
    *host_events = (events & (SNX_POLLIN | SNX_POLLRDNORM)) ? POLLIN : 0;
    return events & (SNX_POLLOUT | SNX_POLLWRNORM); /* a send only queues */
}
static const SnxSocketOps g_room_sock_ops = {.destroy = sock_destroy,
                                             .bind = sock_bind,
                                             .connect = sock_connect,
                                             .shutdown = sock_shutdown,
                                             .send = sock_send,
                                             .recv = sock_recv,
                                             .name = sock_name,
                                             .setsockopt = sock_setsockopt,
                                             .getsockopt = sock_getsockopt,
                                             .readable_bytes = sock_readable_bytes,
                                             .poll_ready = sock_poll_ready};

int snx_room_socket_open(SnxRoom* r, SnxSocket** out, int domain, int type, int protocol) {
    RoomSock* k;
    SnxSocket* s;
    if (domain != SNX_AF_INET || type != SNX_SOCK_DGRAM) return -SNX_EPROTONOSUPPORT;
    if (protocol != 0 && protocol != SNX_IPPROTO_UDP) return -SNX_EPROTONOSUPPORT;
    k = calloc(1, sizeof *k);
    if (!k) return -SNX_ENOMEM;
    int e = snx_notifier_open(k->notify);
    if (e) {
        free(k);
        return e;
    }
    snx_lock_init(&k->lk);
    k->room = r;
    /* Eden's bsd turns the guest's protocol number into its own (sockets_translate.cpp): 0 is IP, 17 is UDP. A
       datagram reaches only a socket of the same one, so a ScarletNX and an Eden of one game meet only this way */
    k->protocol = protocol ? ROOM_PROTOCOL_UDP : ROOM_PROTOCOL_IP;
    s = snx_net_socket_new(&g_room_sock_ops, k->notify[0], domain, type, protocol);
    if (!s) {
        snx_net_close_fd(k->notify[0]);
        snx_net_close_fd(k->notify[1]);
        snx_lock_destroy(&k->lk);
        free(k);
        return -SNX_ENOMEM;
    }
    s->impl = k;
    snx_lock(&r->lk);
    k->next = r->socks;
    r->socks = k;
    r->refs++;
    snx_unlock(&r->lk);
    *out = s;
    return 0;
}

int snx_room_socket(const SnxSocket* s) { return s->ops == &g_room_sock_ops; }

/* a datagram from the room to every socket it is for: the same protocol, bound to the port it went to, and for a
   broadcast with SO_BROADCAST */
static void datagram_in(SnxRoom* r, const uint8_t* msg, size_t size) {
    SnxRoomDatagram d;
    uint8_t protocol;
    if (!parse_datagram(msg, size, &d, &protocol)) {
        snx_net_log("room: a datagram message that isn't one (%zu bytes)", size);
        return;
    }
    unsigned long long plain = ZSTD_getFrameContentSize(d.data, d.size);
    if (plain == ZSTD_CONTENTSIZE_ERROR || plain == ZSTD_CONTENTSIZE_UNKNOWN || plain > SNX_ROOM_DATAGRAM_MAX) {
        snx_net_log("room: a datagram from %u.%u.%u.%u:%u isn't a zstd frame of a datagram's size", d.local_ip[0],
                    d.local_ip[1], d.local_ip[2], d.local_ip[3], d.local_port);
        return;
    }
    uint8_t* buf = malloc(plain ? (size_t)plain : 1);
    if (!buf) {
        snx_net_log("room: a datagram of %llu bytes dropped: out of memory", plain);
        return;
    }
    size_t n = ZSTD_decompress(buf, (size_t)plain, d.data, d.size);
    if (ZSTD_isError(n) || n != plain) {
        snx_net_log("room: a datagram from %u.%u.%u.%u:%u doesn't decompress", d.local_ip[0], d.local_ip[1],
                    d.local_ip[2], d.local_ip[3], d.local_port);
        free(buf);
        return;
    }
    snx_lock(&r->lk);
    for (RoomSock* k = r->socks; k; k = k->next) {
        RoomDatagramIn* in;
        snx_lock(&k->lk);
        if (k->protocol != protocol || !k->port || k->port != d.remote_port || k->shut_rd ||
            (d.broadcast && !k->broadcast)) {
            snx_unlock(&k->lk);
            continue;
        }
        int queue_limit = r->low_latency ? ROOM_LOW_LATENCY_QUEUE_MAX : ROOM_QUEUE_MAX;
        if (k->queued >= queue_limit || (r->low_latency && (k->queued_bytes > ROOM_LOW_LATENCY_QUEUE_BYTES ||
                                                            n > ROOM_LOW_LATENCY_QUEUE_BYTES - k->queued_bytes))) {
            if (!k->warned_full)
                snx_net_log("room: the socket on port %u has %d datagrams unread, dropping the next ones", k->port,
                            k->queued);
            k->warned_full = 1;
            snx_unlock(&k->lk);
            continue;
        }
        in = malloc(sizeof *in + n);
        if (in) {
            in->next = NULL;
            memcpy(in->ip, d.local_ip, 4);
            in->port = d.local_port;
            in->size = n;
            memcpy(in->data, buf, n);
            if (k->tail)
                k->tail->next = in;
            else {
                k->head = in;
                snx_notifier_signal(k->notify[1]);
            }
            k->tail = in;
            k->queued++;
            k->queued_bytes += n;
            k->warned_full = 0;
        } else
            snx_net_log("room: a datagram for port %u dropped: out of memory", k->port);
        snx_unlock(&k->lk);
    }
    snx_unlock(&r->lk);
    free(buf);
}

/* ============================================================ the room's messages */
/* the room went away (why): said once, and the ldn handler told */
static void went_away(SnxRoom* r, int why) {
    snx_lock(&r->lk);
    int was = r->state;
    if (was == SNX_ROOM_IN) r->state = why;
    snx_unlock(&r->lk);
    if (was != SNX_ROOM_IN) return;
    snx_net_warn("room: out of the room: %s", snx_room_why_text(why));
    call_ldn(r, NULL);
}
static void room_information(SnxRoom* r, const uint8_t* msg, size_t size) {
    char text[64];
    uint8_t ip[4];
    In in;
    RoomMember m[ROOM_MEMBERS];
    int n = 0;
    in.p = msg;
    in.n = size;
    in.at = 1;
    in.bad = 0;
    get_text(&in, text, sizeof text); /* name */
    get_text(&in, text, sizeof text); /* description */
    get32(&in);                       /* member slots */
    get16(&in);                       /* port */
    get_text(&in, text, sizeof text); /* preferred game */
    get_text(&in, text, sizeof text); /* host username */
    uint32_t count = get32(&in);
    for (uint32_t i = 0; i < count && !in.bad; i++) {
        char nickname[SNX_ROOM_NICKNAME_MAX + 1];
        get_text(&in, nickname, sizeof nickname);
        get_ip(&in, ip);
        get_text(&in, text, sizeof text); /* game name */
        get(&in, 8);                      /* game id */
        get_text(&in, text, sizeof text); /* game version */
        get_text(&in, text, sizeof text); /* username */
        get_text(&in, text, sizeof text); /* display name */
        get_text(&in, text, sizeof text); /* avatar url */
        if (n < ROOM_MEMBERS && !in.bad) {
            memcpy(m[n].ip, ip, 4);
            memcpy(m[n].nickname, nickname, sizeof nickname);
            n++;
        }
    }
    if (in.bad) {
        snx_net_log("room: a room information message that isn't one (%zu bytes)", size);
        return;
    }
    snx_lock(&r->lk);
    for (int k = 0; k < n; k++) r->members[k] = m[k];
    r->nmembers = n;
    snx_unlock(&r->lk);
}
/* the room's address of the member named nickname, by the room's information: 0 when that lists no such member */
static int member_ip(SnxRoom* r, const char* nickname, uint8_t ip[4]) {
    int found = 0;
    snx_lock(&r->lk);
    for (int i = 0; i < r->nmembers && !found; i++)
        if (!strcmp(r->members[i].nickname, nickname)) {
            memcpy(ip, r->members[i].ip, 4);
            found = 1;
        }
    snx_unlock(&r->lk);
    return found;
}
/* Eden says who joins before its information lists them, and who leaves before it stops listing them */
static void status_message(SnxRoom* r, const uint8_t* msg, size_t size) {
    static const char* const what[] = {"joined", "left", "was kicked", "was banned"}; /* IdMemberJoin on */
    char nickname[64];
    uint8_t ip[4];
    In in;
    in.p = msg;
    in.n = size;
    in.at = 1;
    in.bad = 0;
    uint8_t type = get8(&in);
    get_text(&in, nickname, sizeof nickname);
    if (in.bad) return;
    if (type == 5) /* IdAddressUnbanned: the nickname is the username or address */
        snx_net_log("room: a username or address is unbanned");
    else if (type < 1 || type > 4)
        snx_net_log("room: a member's status %u, which Eden doesn't send", type);
    else if (member_ip(r, nickname, ip))
        snx_net_log("room: the member at %u.%u.%u.%u %s", ip[0], ip[1], ip[2], ip[3], what[type - 1]);
    else
        snx_net_log("room: a member %s", what[type - 1]);
}
static void chat_message(SnxRoom* r, const uint8_t* msg, size_t size) {
    char nickname[64];
    uint8_t ip[4];
    size_t n;
    In in;
    in.p = msg;
    in.n = size;
    in.at = 1;
    in.bad = 0;
    get_text(&in, nickname, sizeof nickname);
    get_bytes(&in, &n); /* the username */
    get_bytes(&in, &n); /* the text */
    if (in.bad) return;
    if (member_ip(r, nickname, ip))
        snx_net_log("room: a chat message of %zu bytes from the member at %u.%u.%u.%u", n, ip[0], ip[1], ip[2], ip[3]);
    else
        snx_net_log("room: a chat message of %zu bytes from a member the room's information doesn't list", n);
}
/* one message from the room. In the join, the answer to it: SNX_ROOM_IN for a success, a SNX_ROOM_* refusal, or -1
   for anything else */
static int room_message(SnxRoom* r, const uint8_t* msg, size_t size, int joining) {
    SnxRoomLdn p;
    if (!size) return -1;
    switch (msg[0]) {
        case ID_PROXY_PACKET:
            if (!joining) datagram_in(r, msg, size);
            return -1;
        case ID_LDN_PACKET:
            if (joining) return -1;
            if (snx_room_parse_ldn(msg, size, &p))
                call_ldn(r, &p);
            else
                snx_net_log("room: an ldn message that isn't one (%zu bytes)", size);
            return -1;
        case ID_ROOM_INFORMATION:
            room_information(r, msg, size);
            if (!joining) call_ldn(r, NULL);
            return -1;
        case ID_STATUS_MESSAGE: status_message(r, msg, size); return -1;
        case ID_CHAT_MESSAGE: chat_message(r, msg, size); return -1;
        case ID_JOIN_SUCCESS:
        case ID_JOIN_SUCCESS_AS_MOD:
            if (!joining || size < 5) return -1;
            memcpy(r->fake_ip, msg + 1, 4);
            return SNX_ROOM_IN;
        case ID_NAME_COLLISION: return SNX_ROOM_NAME_COLLISION;
        case ID_IP_COLLISION: return SNX_ROOM_IP_COLLISION;
        case ID_VERSION_MISMATCH: return SNX_ROOM_VERSION_MISMATCH;
        case ID_WRONG_PASSWORD: return SNX_ROOM_WRONG_PASSWORD;
        case ID_ROOM_IS_FULL: return SNX_ROOM_FULL;
        case ID_HOST_BANNED:
            if (!joining) went_away(r, SNX_ROOM_BANNED);
            return SNX_ROOM_BANNED;
        case ID_HOST_KICKED:
            if (!joining) went_away(r, SNX_ROOM_KICKED);
            return SNX_ROOM_KICKED;
        case ID_CLOSE_ROOM:
            if (!joining) went_away(r, SNX_ROOM_CLOSED);
            return SNX_ROOM_CLOSED;
        default: return -1; /* moderation answers: not for a console */
    }
}

/* ============================================================ the thread */
static void room_thread(void* arg) {
    SnxRoom* r = arg;
    int64_t next_report = 0;
    for (;;) {
        ENetEvent ev;
        int k, stop;
        snx_lock(&r->lk);
        stop = r->stop;
        int in = r->state == SNX_ROOM_IN;
        int service_ms = r->low_latency ? 1 : ROOM_SERVICE_MS;
        RoomOut* o = r->out;
        r->out = r->out_tail = NULL;
        snx_unlock(&r->lk);
        while (o) {
            RoomOut* next = o->next;
            ENetPacket* packet = enet_packet_create(o->data, o->size, ENET_PACKET_FLAG_RELIABLE);
            if (!packet || enet_peer_send(r->peer, 0, packet) < 0) {
                snx_net_log("room: a message of %zu bytes to the room dropped: ENet took none", o->size);
                if (packet) enet_packet_destroy(packet);
            }
            free(o);
            o = next;
        }
        if (stop) break;
        if (!in) {
            snx_net_sleep_us(ROOM_SERVICE_MS * 1000);
            continue;
        }
        for (k = enet_host_service(r->host, &ev, service_ms); k > 0; k = enet_host_check_events(r->host, &ev)) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                room_message(r, ev.packet->data, ev.packet->dataLength, 0);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
                went_away(r, SNX_ROOM_CLOSED);
        }
        if (k < 0) went_away(r, SNX_ROOM_CLOSED);
        if (service_ms == 1 && snx_net_now_us() >= next_report) {
            snx_net_log("room timing: relay RTT=%ums variance=%ums loss=%.2f%%", r->peer->roundTripTime,
                        r->peer->roundTripTimeVariance, 100.0 * r->peer->packetLoss / ENET_PEER_PACKET_LOSS_SCALE);
            next_report = snx_net_now_us() + 5000000;
        }
    }
    snx_lock(&r->lk);
    r->done = 1;
    snx_unlock(&r->lk);
}
void snx_room_low_latency(SnxRoom* r) {
    snx_lock(&r->lk);
    r->low_latency = 1;
    snx_unlock(&r->lk);
}

/* ============================================================ joining and leaving */
static void room_free(SnxRoom* r) {
    while (r->out) {
        RoomOut* next = r->out->next;
        free(r->out);
        r->out = next;
    }
    if (r->host) enet_host_destroy(r->host);
    snx_lock_destroy(&r->lk);
    free(r);
    enet_deinitialize();
}
void snx_room_ref(SnxRoom* r) {
    snx_lock(&r->lk);
    r->refs++;
    snx_unlock(&r->lk);
}
void snx_room_unref(SnxRoom* r) {
    snx_lock(&r->lk);
    int last = --r->refs == 0;
    snx_unlock(&r->lk);
    if (last) room_free(r);
}
static int send_now(SnxRoom* r, const uint8_t* data, size_t size) {
    ENetPacket* packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (!packet) return -SNX_ENOMEM;
    if (enet_peer_send(r->peer, 0, packet) < 0) {
        enet_packet_destroy(packet);
        return -SNX_ENETDOWN;
    }
    return 0;
}
int snx_room_join(SnxRoom** out, const char* host, uint16_t port, const char* nickname, const char* password,
                  const char* game_name, uint64_t game_id, const char* game_version, int* why) {
    ENetAddress address;
    ENetEvent ev;
    SnxRoom* r;
    int answer = -1;
    *out = NULL;
    *why = SNX_ROOM_LOCAL;
    int e = snx_net_init();
    if (e) return e;
    if (enet_initialize()) return -SNX_ENETDOWN;
    r = calloc(1, sizeof *r);
    if (!r) {
        enet_deinitialize();
        return -SNX_ENOMEM;
    }
    snx_lock_init(&r->lk);
    r->state = SNX_ROOM_UNREACHABLE;
    r->next_port = ROOM_PORT_FIRST;
    r->host = enet_host_create(NULL, 1, 1, 0, 0);
    if (!r->host) {
        room_free(r);
        return -SNX_ENOMEM;
    }
    if (enet_address_set_host(&address, host) < 0) {
        *why = SNX_ROOM_NO_ADDRESS;
        room_free(r);
        return -SNX_EHOSTUNREACH;
    }
    address.port = port;
    r->peer = enet_host_connect(r->host, &address, 1, 0);
    if (!r->peer) {
        room_free(r);
        return -SNX_ENOMEM;
    }
    /* an ENet error (k < 0) ends either wait as this computer's failure, not the room's silence */
    *why = SNX_ROOM_UNREACHABLE;
    int64_t deadline = snx_net_now_us() + (int64_t)SNX_ROOM_JOIN_MS * 1000;
    for (;;) {
        int64_t left = (deadline - snx_net_now_us()) / 1000;
        if (left <= 0) break;
        int k = enet_host_service(r->host, &ev, (enet_uint32)left);
        if (k < 0) {
            *why = SNX_ROOM_LOCAL;
            e = -SNX_EIO;
            break;
        }
        if (!k) continue;
        if (ev.type == ENET_EVENT_TYPE_CONNECT) {
            answer = 0;
            break;
        }
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
    }
    if (answer) {
        enet_peer_reset(r->peer);
        room_free(r);
        return e ? e : -SNX_ETIMEDOUT;
    }
    /* the join and the game, as Eden's member sends them one after the other */
    size_t max = 64 + strlen(nickname) + strlen(password) + strlen(game_name) + strlen(game_version);
    uint8_t* msg = malloc(max);
    if (!msg) {
        *why = SNX_ROOM_LOCAL;
        enet_peer_reset(r->peer);
        room_free(r);
        return -SNX_ENOMEM;
    }
    size_t n = snx_room_pack_join(msg, max, nickname, password);
    e = send_now(r, msg, n);
    n = pack_game_info(msg, max, game_name, game_id, game_version);
    if (!e) e = send_now(r, msg, n);
    free(msg);
    enet_host_flush(r->host);
    *why = e ? SNX_ROOM_LOCAL : SNX_ROOM_NO_ANSWER;
    answer = -1;
    deadline = snx_net_now_us() + (int64_t)SNX_ROOM_JOIN_MS * 1000;
    while (!e && answer < 0) {
        int64_t left = (deadline - snx_net_now_us()) / 1000;
        if (left <= 0) break;
        int k = enet_host_service(r->host, &ev, (enet_uint32)left);
        if (k < 0) {
            *why = SNX_ROOM_LOCAL;
            e = -SNX_EIO;
            break;
        }
        if (!k) continue;
        if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
            answer = room_message(r, ev.packet->data, ev.packet->dataLength, 1);
            enet_packet_destroy(ev.packet);
        } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT)
            answer = SNX_ROOM_CLOSED;
    }
    if (e || answer != SNX_ROOM_IN) {
        if (answer > 0) *why = answer;
        enet_peer_reset(r->peer);
        room_free(r);
        return e ? e : answer > 0 ? -SNX_ECONNREFUSED : -SNX_ETIMEDOUT;
    }
    r->state = SNX_ROOM_IN;
    r->refs = 1;
    snx_net_log("room: joined %s:%u, at %u.%u.%u.%u, %d member%s", host, port, r->fake_ip[0], r->fake_ip[1],
                r->fake_ip[2], r->fake_ip[3], r->nmembers, r->nmembers == 1 ? "" : "s");
    e = snx_net_thread(room_thread, r);
    if (e) {
        *why = SNX_ROOM_LOCAL;
        enet_peer_reset(r->peer);
        room_free(r);
        return e;
    }
    r->running = 1;
    *why = SNX_ROOM_IN;
    *out = r;
    return 0;
}
void snx_room_leave(SnxRoom* r) {
    ENetEvent ev;
    int64_t deadline;
    if (!r) return;
    snx_lock(&r->lk);
    r->stop = 1;
    snx_unlock(&r->lk);
    for (;;) {
        int done;
        snx_lock(&r->lk);
        done = r->done || !r->running;
        snx_unlock(&r->lk);
        if (done) break;
        snx_net_sleep_us(1000);
    }
    snx_lock(&r->lk);
    int was = r->state;
    if (was == SNX_ROOM_IN) r->state = SNX_ROOM_CLOSED;
    snx_unlock(&r->lk);
    /* a polite leave, so the room tells the others at once rather than at ENet's timeout, after what was queued: the
       goodbyes of an ldn closed just before, which enet_peer_disconnect would drop */
    if (was == SNX_ROOM_IN) {
        enet_peer_disconnect_later(r->peer, 0);
        deadline = snx_net_now_us() + (int64_t)ROOM_LEAVE_MS * 1000;
        while (snx_net_now_us() < deadline) {
            int k = enet_host_service(r->host, &ev, 10);
            if (k < 0 || (k > 0 && ev.type == ENET_EVENT_TYPE_DISCONNECT)) break;
            if (k > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) enet_packet_destroy(ev.packet);
        }
    }
    enet_peer_reset(r->peer);
    snx_room_unref(r);
}
