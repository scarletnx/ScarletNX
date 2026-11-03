/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Local wireless over the LAN (net_ldn.h), with ldn_mitm's wire protocol as Ryujinx's LdnMitm client speaks it:
   a 12 byte header before each payload, the payload's zeros run-length encoded, a Scan broadcast answered with a
   ScanResponse over UDP, a Connect from the station and SyncNetwork from the host over the host's TCP.

   Two UDP sockets on 11452, as ldn_mitm and Ryujinx open them: one bound to the interface's broadcast address, which
   is the only one Linux delivers broadcasts to, and one bound to the console's own address, where the unicast
   replies arrive and from which everything is sent, so the datagrams carry this console's address and the others
   can tell them from their own. On a host that refuses a bind to the broadcast address (Windows does) the unicast
   socket alone does both. One thread per console polls them, the listening socket and every session, and makes the
   changes under the lock; the guest's calls take the same lock and wait, unlocked, on an eventfd the thread writes.

   Through an Eden room (snx_ldn_open_room) the same calls travel as the room's ldn messages, as Eden's
   lan_discovery.cpp sends them: a Scan to every member, answered with the host's NetworkInfo, a Connect with the
   station's node to the host, a SyncNetwork to every station whenever the network changes, a Disconnect from a
   station that leaves, a DestroyNetwork to each when the host closes it. The room's thread delivers them; there is
   no thread here then.

   The log names a node by its number and address, never by its user_name: players attach the log to public reports,
   and a nickname can name them */
#include "common/network/net_ldn.h"
#include "common/network/net_room.h"
#include "common/network/net_internal.h"
#include "common/utilities/endian.h"
#include <stdlib.h>
#include <string.h>

#define LDN_MAGIC           0x11451400u
#define LDN_SCAN_RESULTS    16
#define LDN_SEND_TIMEOUT_US 2000000 /* a session's send, from the thread */
#define LDN_SCAN_SETTLE_MS  100     /* after the first reply, for the others */

typedef struct LdnStation {
    SnxSocket* s;  /* on the LAN */
    uint8_t ip[4]; /* in a room: the station's address there */
    int node;      /* 1 to 7 while a Connect has arrived, 0 before */
    uint8_t rx[SNX_LDN_FRAME_MAX];
    size_t rxn;
} LdnStation;

enum { ROLE_NONE, ROLE_HOST, ROLE_STATION };

struct SnxLdn {
    SnxLock lk;
    SnxRoom* room; /* through an Eden room, else on the LAN */
    uint8_t ip[4], mask[4], bcast[4];
    SnxSocket *ub, *uu;    /* the scan sockets: broadcast bound (may be NULL), unicast bound */
    SnxSocket* wake;       /* the thread's eventfd: written to make it look again, or stop */
    SnxSocket* changed_ev; /* written on every change, for a guest call that waits */
    void (*changed)(void*);
    void* arg;
    int stop, done, running;
    int role;
    SnxLdnNetworkInfo info;
    uint8_t updates[SNX_LDN_NODES];
    uint8_t adv[SNX_LDN_ADVERTISE_MAX];
    size_t adv_size;
    /* the host's */
    SnxSocket* listen;
    LdnStation st[SNX_LDN_STATIONS];
    /* the station's */
    SnxSocket* client;
    uint8_t host_ip[4]; /* in a room */
    SnxLdnNodeInfo me;
    uint8_t client_rx[SNX_LDN_FRAME_MAX];
    size_t client_rxn;
    int connected, reason;
    /* the scanner's */
    int scanning, nscan;
    SnxLdnNetworkInfo scan[LDN_SCAN_RESULTS];
};

/* ============================================================ frames */
/* ldn_mitm's run-length coding: a zero byte is followed by how many more zeros it stands for (up to 255). Returns
   the coded size, or 0 when it wouldn't fit max */
static size_t rle(const uint8_t* in, size_t n, uint8_t* out, size_t max) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t b = in[i++];
        if (o >= max) return 0;
        out[o++] = b;
        if (b == 0) {
            size_t count = 0;
            while (i < n && in[i] == 0 && count < 255) {
                count++;
                i++;
            }
            if (o >= max) return 0;
            out[o++] = (uint8_t)count;
        }
    }
    return o;
}
/* the inverse. Returns the decoded size, or 0 for a stream that ends inside a run or outgrows max */
static size_t unrle(const uint8_t* in, size_t n, uint8_t* out, size_t max) {
    size_t i = 0, o = 0;
    while (i < n) {
        uint8_t b = in[i++];
        if (o >= max) return 0;
        out[o++] = b;
        if (b == 0) {
            if (i >= n) return 0;
            size_t count = in[i++];
            for (size_t k = 0; k < count; k++) {
                if (o >= max) return 0;
                out[o++] = 0;
            }
        }
    }
    return o;
}
size_t snx_ldn_frame(uint8_t type, const void* payload, size_t size, uint8_t* out, size_t max) {
    size_t coded = 0;
    uint8_t compressed = 0;
    if (max < SNX_LDN_HEADER || size > SNX_LDN_FRAME_MAX - SNX_LDN_HEADER) return 0;
    if (size) {
        coded = rle(payload, size, out + SNX_LDN_HEADER, max - SNX_LDN_HEADER);
        if (coded && coded < size)
            compressed = 1;
        else {
            if (max - SNX_LDN_HEADER < size) return 0;
            memcpy(out + SNX_LDN_HEADER, payload, size);
            coded = size;
        }
    }
    snx_st_le32(out, LDN_MAGIC);
    out[4] = type;
    out[5] = compressed;
    snx_st_le16(out + 6, (uint16_t)coded);
    snx_st_le16(out + 8, (uint16_t)(compressed ? size : 0));
    out[10] = out[11] = 0;
    return SNX_LDN_HEADER + coded;
}
int snx_ldn_unframe(const uint8_t* in, size_t size, uint8_t* type, uint8_t* payload, size_t max, size_t* payload_size,
                    size_t* used) {
    if (size < SNX_LDN_HEADER) return 0;
    if (snx_ld_le32(in) != LDN_MAGIC) return -1;
    size_t length = snx_ld_le16(in + 6);
    size_t total = SNX_LDN_HEADER + length;
    if (total > SNX_LDN_FRAME_MAX) return -1;
    if (size < total) return 0;
    *type = in[4];
    *used = total;
    if (in[5]) {
        size_t n = unrle(in + SNX_LDN_HEADER, length, payload, max);
        if (!n || n != snx_ld_le16(in + 8)) return -1;
        *payload_size = n;
        return 1;
    }
    if (length > max) return -1;
    memcpy(payload, in + SNX_LDN_HEADER, length);
    *payload_size = length;
    return 1;
}

/* ============================================================ helpers */
void snx_ldn_fake_mac(const uint8_t ip[4], uint8_t mac[6]) {
    mac[0] = 0x02;
    mac[1] = 0x00;
    memcpy(mac + 2, ip, 4);
}
static void addr_of(SnxSockAddr* a, const uint8_t ip[4], uint16_t port) {
    memset(a, 0, sizeof *a);
    a->family = SNX_AF_INET;
    snx_st_be16(&a->port, port); /* network order */
    memcpy(a->addr, ip, 4);
}
static void signal_event(SnxSocket* ev) {
    uint8_t one[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    snx_socket_send(ev, one, 8, SNX_MSG_DONTWAIT, NULL);
}
static void drain_event(SnxSocket* ev) {
    uint8_t v[8];
    snx_socket_recv(ev, v, 8, SNX_MSG_DONTWAIT, NULL, NULL);
}
/* waits until ev is written or timeout_ms passes, and takes the write */
static void wait_event(SnxSocket* ev, int timeout_ms) {
    SnxPollEntry e;
    e.socket = ev;
    e.host_fd = -1;
    e.events = SNX_POLLIN;
    e.revents = 0;
    if (snx_net_poll(&e, 1, timeout_ms) > 0) drain_event(ev);
}
/* a change the guest sees: the waiting call, then the callback, both without the lock */
static void notify(SnxLdn* l) {
    signal_event(l->changed_ev);
    if (l->changed) l->changed(l->arg);
}
/* the session socket, listening from the start: UDP lets two sockets share an address and port (SO_REUSEADDR is
   how broadcasts reach both), so the TCP port is what tells a second console on the same address to look for
   another. A connection that arrives while this console hosts nothing is closed at once (accept_station) */
static int listen_socket(SnxSocket** out, const uint8_t ip[4]) {
    SnxSocket* s;
    SnxSockAddr a;
    int r = snx_socket_open(&s, SNX_AF_INET, SNX_SOCK_STREAM, 0);
    if (r) return r;
#ifndef _WIN32
    { /* to listen again on a port a closed session left in TIME_WAIT. Windows' SO_REUSEADDR lets a second socket take
         a port in use, so the second console would never be told to look for another address */
        int one = (int)snx_le32(1); /* option values are the guest's (net.h) */
        snx_socket_setsockopt(s, SNX_SOL_SOCKET, SNX_SO_REUSEADDR, &one, sizeof one);
    }
#endif
    addr_of(&a, ip, SNX_LDN_PORT);
    r = snx_socket_bind(s, &a);
    if (!r) r = snx_socket_listen(s, SNX_LDN_STATIONS);
    if (r) {
        snx_socket_unref(s);
        return r;
    }
    snx_socket_set_nonblocking(s, 1);
    *out = s;
    return 0;
}
static int udp_socket(SnxSocket** out, const uint8_t ip[4]) {
    SnxSocket* s;
    SnxSockAddr a;
    int one = (int)snx_le32(1), r = snx_socket_open(&s, SNX_AF_INET, SNX_SOCK_DGRAM, 0); /* the guest's order */
    if (r) return r;
    snx_socket_setsockopt(s, SNX_SOL_SOCKET, SNX_SO_REUSEADDR, &one, sizeof one);
    snx_socket_setsockopt(s, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &one, sizeof one);
    addr_of(&a, ip, SNX_LDN_PORT);
    r = snx_socket_bind(s, &a);
    if (r) {
        snx_socket_unref(s);
        return r;
    }
    *out = s;
    return 0;
}
static void send_frame_udp(SnxLdn* l, uint8_t type, const void* payload, size_t size, const SnxSockAddr* to) {
    uint8_t buf[SNX_LDN_FRAME_MAX];
    size_t n = snx_ldn_frame(type, payload, size, buf, sizeof buf);
    if (n) snx_socket_send(l->uu, buf, n, SNX_MSG_DONTWAIT, to);
}
static int send_frame_tcp(SnxSocket* s, uint8_t type, const void* payload, size_t size) {
    uint8_t buf[SNX_LDN_FRAME_MAX];
    size_t n = snx_ldn_frame(type, payload, size, buf, sizeof buf), sent = 0;
    while (sent < n) {
        int64_t r = snx_socket_send(s, buf + sent, n - sent, 0, NULL);
        if (r <= 0) return r < 0 ? (int)r : -SNX_EPIPE;
        sent += (size_t)r;
    }
    return 0;
}
/* the nodes' connect and disconnect bits between the network as it was and as it is */
static void note_changes(SnxLdn* l, const SnxLdnNetworkInfo* before, const SnxLdnNetworkInfo* after) {
    int i;
    for (i = 0; i < SNX_LDN_NODES; i++)
        if (before->nodes[i].is_connected != after->nodes[i].is_connected)
            l->updates[i] |= after->nodes[i].is_connected ? SNX_LDN_NODE_CONNECT : SNX_LDN_NODE_DISCONNECT;
}
static void close_station(LdnStation* st) {
    snx_socket_unref(st->s);
    st->s = NULL;
    memset(st->ip, 0, 4);
    st->node = 0;
    st->rxn = 0;
}
/* the host's network to every connected station */
static void sync_stations(SnxLdn* l) {
    int r;
    for (int i = 0; i < SNX_LDN_STATIONS; i++) {
        if (!l->st[i].node) continue;
        if (l->room)
            r = snx_room_send_ldn(l->room, SNX_ROOM_LDN_SYNC_NETWORK, l->st[i].ip, 0, &l->info, sizeof l->info);
        else
            r = l->st[i].s ? send_frame_tcp(l->st[i].s, SNX_LDN_SYNC_NETWORK, &l->info, sizeof l->info) : 0;
        if (r) snx_net_log("ldn: SyncNetwork to node %d failed (errno %d)", l->st[i].node, -r);
    }
}
static void drop_client(SnxLdn* l) {
    snx_socket_unref(l->client);
    l->client = NULL;
    l->client_rxn = 0;
}

/* ============================================================ what arrives */
/* the host's network, at the station */
static void station_sync(SnxLdn* l, const uint8_t* payload) {
    SnxLdnNetworkInfo before = l->info;
    memcpy(&l->info, payload, sizeof l->info);
    note_changes(l, &before, &l->info);
    if (!l->connected)
        snx_net_log("ldn: joined the network of the host at %u.%u.%u.%u, %u of %u nodes", l->info.nodes[0].ipv4[3],
                    l->info.nodes[0].ipv4[2], l->info.nodes[0].ipv4[1], l->info.nodes[0].ipv4[0], l->info.node_count,
                    l->info.node_count_max);
    else if (before.node_count != l->info.node_count)
        snx_net_log("ldn: the network has %u nodes now", l->info.node_count);
    l->connected = 1;
    l->reason = SNX_LDN_REASON_NONE;
}
/* a network a scan found (a NetworkInfo), kept once per bssid: 2 for the waiting Scan, 0 when there's no room */
static int scan_reply(SnxLdn* l, const uint8_t* payload) {
    int i;
    for (i = 0; i < l->nscan; i++)
        if (!memcmp(l->scan[i].bssid, payload + 0x20, 6)) break; /* bssid, at CommonNetworkInfo's start */
    if (i == LDN_SCAN_RESULTS) return 0;
    memcpy(&l->scan[i], payload, sizeof l->scan[i]);
    if (i == l->nscan) l->nscan++;
    return 2;
}
/* the frames a session's stream holds, with l locked. 1 when the network changed, 0 when nothing did, -1 when the
   other side is gone or sent what it shouldn't */
static int frames_in(SnxLdn* l, uint8_t* rx, size_t* rxn, SnxSocket* s, int station) {
    uint8_t payload[SNX_LDN_FRAME_MAX];
    int changed = 0;
    for (;;) {
        int64_t r = snx_socket_recv(s, rx + *rxn, SNX_LDN_FRAME_MAX - *rxn, SNX_MSG_DONTWAIT, NULL, NULL);
        size_t used, n;
        uint8_t type;
        int k;
        if (r == -SNX_EAGAIN) return changed;
        if (r <= 0) return -1; /* the other side is gone */
        *rxn += (size_t)r;
        for (;;) {
            k = snx_ldn_unframe(rx, *rxn, &type, payload, sizeof payload, &n, &used);
            if (k < 0) {
                snx_net_log("ldn: a session sent what isn't a frame");
                return -1;
            }
            if (k == 0 && *rxn == SNX_LDN_FRAME_MAX) return -1; /* a header that promises more than fits */
            if (k == 0) break;
            memmove(rx, rx + used, *rxn - used);
            *rxn -= used;
            if (station < 0) { /* I am the station: the host's network */
                if (type == SNX_LDN_SYNC_NETWORK && n == sizeof l->info) {
                    station_sync(l, payload);
                    changed = 1;
                }
            } else if (type == SNX_LDN_CONNECT && n == sizeof(SnxLdnNodeInfo)) { /* a station's node */
                LdnStation* st = &l->st[station];
                int i;
                for (i = 1; i < SNX_LDN_NODES && l->info.nodes[i].is_connected; i++) {}
                if (i == SNX_LDN_NODES || st->node) return -1; /* full, or it connected twice */
                memcpy(&l->info.nodes[i], payload, sizeof(SnxLdnNodeInfo));
                l->info.nodes[i].node_id = (int8_t)i;
                l->info.nodes[i].is_connected = 1;
                st->node = i;
                l->info.node_count = (uint8_t)(l->info.node_count + 1);
                l->updates[i] |= SNX_LDN_NODE_CONNECT;
                snx_net_log("ldn: node %d joined from %u.%u.%u.%u, %u of %u nodes", i, l->info.nodes[i].ipv4[3],
                            l->info.nodes[i].ipv4[2], l->info.nodes[i].ipv4[1], l->info.nodes[i].ipv4[0],
                            l->info.node_count, l->info.node_count_max);
                sync_stations(l);
                changed = 1;
            }
        }
    }
}
/* a datagram on one of the scan sockets, with l locked: 2 for a scan reply (the waiting Scan's), 0 otherwise */
static int datagram_in(SnxLdn* l, SnxSocket* s) {
    uint8_t buf[SNX_LDN_FRAME_MAX], payload[SNX_LDN_FRAME_MAX], type;
    SnxSockAddr from;
    size_t used, n;
    int64_t r = snx_socket_recv(s, buf, sizeof buf, SNX_MSG_DONTWAIT, &from, NULL);
    if (r <= 0) return 0;
    if (from.family != SNX_AF_INET || !memcmp(from.addr, l->ip, 4)) return 0; /* my own broadcast */
    if (snx_ldn_unframe(buf, (size_t)r, &type, payload, sizeof payload, &n, &used) != 1) {
        snx_net_log("ldn: a datagram from %u.%u.%u.%u isn't a frame", from.addr[0], from.addr[1], from.addr[2],
                    from.addr[3]);
        return 0;
    }
    if (type == SNX_LDN_SCAN && l->role == ROLE_HOST) {
        send_frame_udp(l, SNX_LDN_SCAN_RESPONSE, &l->info, sizeof l->info, &from);
        return 0;
    }
    if (type == SNX_LDN_SCAN_RESPONSE && l->scanning && n == sizeof(SnxLdnNetworkInfo)) return scan_reply(l, payload);
    return 0;
}
static void accept_station(SnxLdn* l) {
    SnxSocket* c;
    SnxSockAddr peer;
    SnxTimeval tv;
    int i, r = snx_socket_accept(l->listen, &c, &peer, 0);
    if (r) return;
    for (i = 0; i < SNX_LDN_STATIONS && l->st[i].s; i++) {}
    if (i == SNX_LDN_STATIONS || l->role != ROLE_HOST) {
        snx_socket_unref(c); /* full, or no network here: closing is ldn_mitm's answer too */
        return;
    }
    tv.sec = (int64_t)snx_le64(LDN_SEND_TIMEOUT_US / 1000000); /* the guest's order, as every option value */
    tv.usec = (int64_t)snx_le64(LDN_SEND_TIMEOUT_US % 1000000);
    snx_socket_setsockopt(c, SNX_SOL_SOCKET, SNX_SO_SNDTIMEO, &tv, sizeof tv);
    l->st[i].s = c;
    l->st[i].node = 0;
    l->st[i].rxn = 0;
}
/* the node a station held leaves the network */
static void station_gone(SnxLdn* l, LdnStation* st) {
    if (st->node) {
        snx_net_log("ldn: node %d left", st->node);
        memset(&l->info.nodes[st->node], 0, sizeof(SnxLdnNodeInfo));
        l->info.nodes[st->node].node_id = (int8_t)st->node;
        l->info.node_count = (uint8_t)(l->info.node_count - 1);
        l->updates[st->node] |= SNX_LDN_NODE_DISCONNECT;
    }
    close_station(st);
    sync_stations(l);
}

/* ============================================================ through a room */
/* the station at ip whose node data is joins, or joins again: 1 when the network changed */
static int room_station_in(SnxLdn* l, const uint8_t ip[4], const uint8_t* data) {
    LdnStation* st = NULL;
    int i, j, fresh = 0;
    for (j = 0; j < SNX_LDN_STATIONS && !st; j++)
        if (l->st[j].node && !memcmp(l->st[j].ip, ip, 4)) st = &l->st[j];
    if (!st) {
        for (j = 0; j < SNX_LDN_STATIONS && l->st[j].node; j++) {}
        for (i = 1; i < SNX_LDN_NODES && l->info.nodes[i].is_connected; i++) {}
        if (j == SNX_LDN_STATIONS || i == SNX_LDN_NODES || l->info.node_count >= l->info.node_count_max) {
            snx_net_log("ldn: %u.%u.%u.%u asked to join a full network", ip[0], ip[1], ip[2], ip[3]);
            return 0;
        }
        st = &l->st[j];
        memcpy(st->ip, ip, 4);
        st->node = i;
        l->info.node_count = (uint8_t)(l->info.node_count + 1);
        fresh = 1;
    }
    i = st->node;
    memcpy(&l->info.nodes[i], data, sizeof(SnxLdnNodeInfo));
    l->info.nodes[i].node_id = (int8_t)i;
    l->info.nodes[i].is_connected = 1;
    if (fresh) {
        l->updates[i] |= SNX_LDN_NODE_CONNECT;
        snx_net_log("ldn: node %d joined from %u.%u.%u.%u, %u of %u nodes", i, ip[0], ip[1], ip[2], ip[3],
                    l->info.node_count, l->info.node_count_max);
    }
    sync_stations(l);
    return 1;
}
/* the station's host is gone (its DestroyNetwork, or it left the room) */
static void room_host_gone(SnxLdn* l) {
    if (l->connected) {
        snx_net_log("ldn: the host went away");
        l->reason = SNX_LDN_REASON_DESTROYED_BY_USER;
    }
    l->connected = 0;
    l->role = ROLE_NONE;
}
/* the room's thread: an ldn message, or (p NULL) the members changed or the room went away. A station that left
   the room without its Disconnect leaves the network too, and so does a station whose host did */
static void room_in(void* arg, const SnxRoomLdn* p) {
    SnxLdn* l = arg;
    int changed = 0, replies = 0, i, in;
    snx_lock(&l->lk);
    if (!p) {
        in = snx_room_state(l->room) == SNX_ROOM_IN;
        if (l->role == ROLE_HOST)
            for (i = 0; i < SNX_LDN_STATIONS; i++)
                if (l->st[i].node && (!in || !snx_room_has_member(l->room, l->st[i].ip))) {
                    station_gone(l, &l->st[i]);
                    changed = 1;
                }
        if (l->role == ROLE_STATION && (!in || !snx_room_has_member(l->room, l->host_ip))) {
            room_host_gone(l);
            changed = 1;
        }
    } else
        switch (p->type) {
            case SNX_ROOM_LDN_SCAN:
                if (l->role == ROLE_HOST &&
                    snx_room_send_ldn(l->room, SNX_ROOM_LDN_SCAN_RESPONSE, p->local_ip, 0, &l->info, sizeof l->info))
                    snx_net_log("ldn: the answer to %u.%u.%u.%u's scan didn't go", p->local_ip[0], p->local_ip[1],
                                p->local_ip[2], p->local_ip[3]);
                break;
            case SNX_ROOM_LDN_SCAN_RESPONSE:
                if (l->scanning && p->size == sizeof(SnxLdnNetworkInfo)) replies = scan_reply(l, p->data);
                break;
            case SNX_ROOM_LDN_CONNECT:
                if (l->role == ROLE_HOST && p->size == sizeof(SnxLdnNodeInfo))
                    changed = room_station_in(l, p->local_ip, p->data);
                break;
            case SNX_ROOM_LDN_DISCONNECT:
                if (l->role == ROLE_HOST)
                    for (i = 0; i < SNX_LDN_STATIONS; i++)
                        if (l->st[i].node && !memcmp(l->st[i].ip, p->local_ip, 4)) {
                            station_gone(l, &l->st[i]);
                            changed = 1;
                        }
                break;
            case SNX_ROOM_LDN_DESTROY_NETWORK:
                if (l->role == ROLE_STATION && !memcmp(p->local_ip, l->host_ip, 4)) {
                    room_host_gone(l);
                    changed = 1;
                }
                break;
            case SNX_ROOM_LDN_SYNC_NETWORK:
                if (l->role == ROLE_STATION && !memcmp(p->local_ip, l->host_ip, 4) && p->size == sizeof l->info) {
                    station_sync(l, p->data);
                    changed = 1;
                }
                break;
            default: break;
        }
    snx_unlock(&l->lk);
    if (changed)
        notify(l);
    else if (replies)
        signal_event(l->changed_ev);
}

/* ============================================================ the thread */
static void ldn_thread(void* arg) {
    SnxLdn* l = arg;
    for (;;) {
        SnxPollEntry e[4 + SNX_LDN_STATIONS + 1];
        SnxSocket* own[4 + SNX_LDN_STATIONS + 1];
        int i, n = 0, r, changed = 0, replies = 0;
        snx_lock(&l->lk);
        if (l->stop) {
            l->done = 1;
            snx_unlock(&l->lk);
            return;
        }
        /* each socket polled holds a reference: a call that closes one meanwhile only drops its own */
#define POLLED(sock)                                                                                                   \
    if (sock) {                                                                                                        \
        own[n] = snx_socket_ref(sock);                                                                                 \
        e[n].socket = sock;                                                                                            \
        e[n].host_fd = -1;                                                                                             \
        e[n].events = SNX_POLLIN;                                                                                      \
        e[n].revents = 0;                                                                                              \
        n++;                                                                                                           \
    }
        POLLED(l->wake)
        POLLED(l->ub)
        POLLED(l->uu)
        POLLED(l->listen)
        POLLED(l->client)
        for (i = 0; i < SNX_LDN_STATIONS; i++) POLLED(l->st[i].s)
#undef POLLED
        snx_unlock(&l->lk);
        r = snx_net_poll(e, (size_t)n, 1000);
        if (r == -SNX_EINTR) snx_net_sleep_us(10000); /* the stack is cancelled: the guest is going away */
        if (r > 0) {
            snx_lock(&l->lk);
            for (i = 0; i < n; i++) {
                SnxSocket* s = own[i];
                if (!e[i].revents) continue;
                if (s == l->wake)
                    drain_event(s);
                else if (s == l->ub || s == l->uu)
                    replies |= datagram_in(l, s);
                else if (s == l->listen)
                    accept_station(l);
                else if (s == l->client) {
                    int k = frames_in(l, l->client_rx, &l->client_rxn, s, -1);
                    if (k < 0) { /* the host went away, or refused me */
                        drop_client(l);
                        l->role = ROLE_NONE;
                        if (l->connected) snx_net_log("ldn: the host went away");
                        if (l->connected) l->reason = SNX_LDN_REASON_DESTROYED_BY_USER;
                        l->connected = 0;
                    }
                    changed |= k != 0;
                } else {
                    int j;
                    for (j = 0; j < SNX_LDN_STATIONS; j++)
                        if (l->st[j].s == s) {
                            int k = frames_in(l, l->st[j].rx, &l->st[j].rxn, s, j);
                            if (k < 0) station_gone(l, &l->st[j]);
                            changed |= k != 0;
                        }
                }
            }
            snx_unlock(&l->lk);
        }
        for (i = 0; i < n; i++) snx_socket_unref(own[i]);
        if (changed)
            notify(l);
        else if (replies)
            signal_event(l->changed_ev);
    }
}

/* ============================================================ the calls */
int snx_ldn_open(SnxLdn** out, const uint8_t ip[4], const uint8_t mask[4], void (*changed)(void*), void* arg) {
    SnxLdn* l;
    int r = snx_net_init();
    if (r) return r;
    l = calloc(1, sizeof *l);
    if (!l) return -SNX_ENOMEM;
    snx_lock_init(&l->lk);
    memcpy(l->ip, ip, 4);
    memcpy(l->mask, mask, 4);
    for (int i = 0; i < 4; i++) l->bcast[i] = (uint8_t)(ip[i] | ~mask[i]);
    l->changed = changed;
    l->arg = arg;
    r = listen_socket(&l->listen, ip);
    if (!r) r = udp_socket(&l->uu, ip);
    if (!r && udp_socket(&l->ub, l->bcast)) l->ub = NULL; /* a host that can't bind a broadcast address */
    if (!r) r = snx_socket_eventfd(&l->wake, 0, 0);
    if (!r) r = snx_socket_eventfd(&l->changed_ev, 0, 0);
    if (!r) {
        snx_socket_set_nonblocking(l->wake, 1);
        snx_socket_set_nonblocking(l->changed_ev, 1);
        r = snx_net_thread(ldn_thread, l);
        if (!r) l->running = 1;
    }
    if (r) {
        snx_ldn_close(l);
        return r;
    }
    *out = l;
    return 0;
}
int snx_ldn_open_room(SnxLdn** out, SnxRoom* room, void (*changed)(void*), void* arg) {
    static const uint8_t mask[4] = {255, 255, 255, 0}; /* the room's addresses are 192.168.x.y */
    SnxLdn* l;
    int r = snx_net_init();
    if (r) return r;
    l = calloc(1, sizeof *l);
    if (!l) return -SNX_ENOMEM;
    snx_lock_init(&l->lk);
    l->room = room;
    snx_room_fake_ip(room, l->ip);
    memcpy(l->mask, mask, 4);
    for (int i = 0; i < 4; i++) l->bcast[i] = (uint8_t)(l->ip[i] | ~mask[i]);
    l->changed = changed;
    l->arg = arg;
    r = snx_socket_eventfd(&l->changed_ev, 0, 0);
    if (r) {
        snx_lock_destroy(&l->lk);
        free(l);
        return r;
    }
    snx_socket_set_nonblocking(l->changed_ev, 1);
    r = snx_room_set_ldn(room, room_in, l);
    if (r) { /* another ldn of this console is in the room: one is all a console has */
        snx_socket_unref(l->changed_ev);
        snx_lock_destroy(&l->lk);
        free(l);
        return r;
    }
    snx_room_ref(room);
    *out = l;
    return 0;
}
void snx_ldn_close(SnxLdn* l) {
    if (!l) return;
    if (l->room) { /* no more messages for l, then the goodbyes Eden's Finalize sends */
        snx_room_clear_ldn(l->room, l);
        snx_ldn_destroy_network(l);
        snx_ldn_disconnect(l);
        snx_room_unref(l->room);
    }
    if (l->running) {
        snx_lock(&l->lk);
        l->stop = 1;
        snx_unlock(&l->lk);
        signal_event(l->wake);
        for (;;) {
            int done;
            snx_lock(&l->lk);
            done = l->done;
            snx_unlock(&l->lk);
            if (done) break;
            snx_net_sleep_us(1000);
        }
    }
    snx_socket_unref(l->listen);
    for (int i = 0; i < SNX_LDN_STATIONS; i++) snx_socket_unref(l->st[i].s);
    snx_socket_unref(l->client);
    snx_socket_unref(l->ub);
    snx_socket_unref(l->uu);
    snx_socket_unref(l->wake);
    snx_socket_unref(l->changed_ev);
    snx_lock_destroy(&l->lk);
    free(l);
}
void snx_ldn_address(const SnxLdn* l, uint8_t ip[4], uint8_t mask[4]) {
    memcpy(ip, l->ip, 4);
    memcpy(mask, l->mask, 4);
}
void snx_ldn_node_init(const SnxLdn* l, SnxLdnNodeInfo* node, const uint8_t user_name[SNX_LDN_USER_NAME],
                       int16_t version) {
    memset(node, 0, sizeof *node);
    for (int i = 0; i < 4; i++) node->ipv4[i] = l->ip[3 - i]; /* host order */
    snx_ldn_fake_mac(l->ip, node->mac);
    node->is_connected = 1;
    memcpy(node->user_name, user_name, SNX_LDN_USER_NAME);
    node->user_name[SNX_LDN_USER_NAME - 1] = 0;
    node->version = (int16_t)snx_le16((uint16_t)version); /* the structures stay little-endian */
}

int snx_ldn_create_network(SnxLdn* l, const SnxLdnNetworkInfo* info) {
    snx_lock(&l->lk);
    if (l->role != ROLE_NONE) {
        snx_unlock(&l->lk);
        return -SNX_EBUSY;
    }
    l->info = *info;
    memcpy(l->info.advertise_data, l->adv, sizeof l->adv);
    l->info.advertise_data_size = snx_le16((uint16_t)l->adv_size);
    memset(l->updates, 0, sizeof l->updates);
    l->updates[0] |= SNX_LDN_NODE_CONNECT;
    l->role = ROLE_HOST;
    snx_unlock(&l->lk);
    return 0;
}
void snx_ldn_destroy_network(SnxLdn* l) {
    snx_lock(&l->lk);
    if (l->role == ROLE_HOST) {
        for (int i = 0; i < SNX_LDN_STATIONS; i++) {
            if (l->room && l->st[i].node &&
                snx_room_send_ldn(l->room, SNX_ROOM_LDN_DESTROY_NETWORK, l->st[i].ip, 0, NULL, 0))
                snx_net_log("ldn: node %d wasn't told the network is gone", l->st[i].node);
            if (l->st[i].s || l->st[i].node) close_station(&l->st[i]);
        }
        l->role = ROLE_NONE;
    }
    snx_unlock(&l->lk);
}
int snx_ldn_set_advertise_data(SnxLdn* l, const void* data, size_t size) {
    if (size > SNX_LDN_ADVERTISE_MAX) return -SNX_EINVAL;
    snx_lock(&l->lk);
    memset(l->adv, 0, sizeof l->adv);
    memcpy(l->adv, data, size);
    l->adv_size = size;
    if (l->role == ROLE_HOST) {
        memcpy(l->info.advertise_data, l->adv, sizeof l->adv);
        l->info.advertise_data_size = snx_le16((uint16_t)size);
        sync_stations(l);
    }
    snx_unlock(&l->lk);
    return 0;
}

/* the fields compared are little-endian on both sides: only the flags and the wider network type are numbers */
static int filter_accepts(const SnxLdnScanFilter* f, const SnxLdnNetworkInfo* i) {
    uint32_t flag;
    if (!f) return 1;
    flag = snx_le32(f->flag);
    if ((flag & SNX_LDN_FILTER_LOCAL_COMMUNICATION_ID) && f->local_communication_id != i->local_communication_id)
        return 0;
    if ((flag & SNX_LDN_FILTER_SESSION_ID) &&
        (f->session_id[0] != i->session_id[0] || f->session_id[1] != i->session_id[1]))
        return 0;
    if ((flag & SNX_LDN_FILTER_NETWORK_TYPE) && snx_le32(f->network_type) != i->network_type) return 0;
    if ((flag & SNX_LDN_FILTER_SSID) &&
        (f->ssid_length != i->ssid_length || memcmp(f->ssid, i->ssid, f->ssid_length)))
        return 0;
    if ((flag & SNX_LDN_FILTER_SCENE_ID) && f->scene_id != i->scene_id) return 0;
    return 1;
}
int snx_ldn_scan(SnxLdn* l, const SnxLdnScanFilter* filter, SnxLdnNetworkInfo* out, int max, int wait_ms) {
    SnxSockAddr to;
    int64_t deadline = snx_net_now_us() + (int64_t)wait_ms * 1000, first = 0;
    int n = 0;
    addr_of(&to, l->bcast, SNX_LDN_PORT);
    snx_lock(&l->lk);
    l->scanning = 1;
    l->nscan = 0;
    drain_event(l->changed_ev);
    if (l->room) {
        int e = snx_room_send_ldn(l->room, SNX_ROOM_LDN_SCAN, NULL, 1, NULL, 0);
        if (e) { /* out of the room nobody hears it: no wait for an answer that can't come */
            l->scanning = 0;
            snx_unlock(&l->lk);
            snx_net_log("ldn: no scan: the room took no message (errno %d)", -e);
            return 0;
        }
    } else
        send_frame_udp(l, SNX_LDN_SCAN, NULL, 0, &to);
    snx_unlock(&l->lk);
    for (;;) {
        int64_t now = snx_net_now_us(), until = deadline;
        /* on the LAN the replies come together; through a room, from wherever each member is, so the whole wait */
        if (first && !l->room && first + LDN_SCAN_SETTLE_MS * 1000 < until) until = first + LDN_SCAN_SETTLE_MS * 1000;
        if (now >= until) break;
        int64_t ms = (until - now + 999) / 1000;
        wait_event(l->changed_ev, (int)ms);
        snx_lock(&l->lk);
        int count = l->nscan;
        snx_unlock(&l->lk);
        if (count && !first) first = snx_net_now_us();
    }
    snx_lock(&l->lk);
    l->scanning = 0;
    for (int i = 0; i < l->nscan && n < max; i++)
        if (filter_accepts(filter, &l->scan[i]) && l->scan[i].nodes[0].user_name[0]) out[n++] = l->scan[i];
    snx_unlock(&l->lk);
    return n;
}

/* the station's side through a room: the Connect to the host, then the wait for its SyncNetwork */
static int room_connect(SnxLdn* l, const SnxLdnNodeInfo* me, const uint8_t host[4], int wait_ms) {
    snx_lock(&l->lk);
    memset(&l->info, 0, sizeof l->info);
    memset(l->updates, 0, sizeof l->updates);
    memcpy(l->host_ip, host, 4);
    l->me = *me;
    l->connected = 0;
    l->reason = SNX_LDN_REASON_NONE;
    l->role = ROLE_STATION;
    drain_event(l->changed_ev);
    int r = snx_room_send_ldn(l->room, SNX_ROOM_LDN_CONNECT, host, 0, me, sizeof *me);
    if (r) l->role = ROLE_NONE;
    snx_unlock(&l->lk);
    if (r) return r;
    int64_t deadline = snx_net_now_us() + (int64_t)wait_ms * 1000;
    for (;;) {
        int64_t now = snx_net_now_us();
        int connected;
        snx_lock(&l->lk);
        connected = l->connected;
        int gone = l->role != ROLE_STATION;
        snx_unlock(&l->lk);
        if (connected) return 0;
        if (gone) return -SNX_ECONNRESET; /* the host destroyed the network, or left the room */
        if (now >= deadline) break;
        wait_event(l->changed_ev, (int)((deadline - now + 999) / 1000));
    }
    snx_ldn_disconnect(l); /* a host that adds the node late takes it out again */
    return -SNX_ETIMEDOUT;
}
int snx_ldn_connect(SnxLdn* l, const SnxLdnNetworkInfo* info, const uint8_t user_name[SNX_LDN_USER_NAME],
                    int16_t version, int wait_ms) {
    SnxLdnNodeInfo me;
    SnxSocket* s;
    SnxSockAddr a;
    SnxTimeval tv;
    uint8_t host[4];
    if (info->node_count == 0) return -SNX_EINVAL;
    snx_lock(&l->lk);
    if (l->role != ROLE_NONE) {
        snx_unlock(&l->lk);
        return -SNX_EBUSY;
    }
    snx_ldn_node_init(l, &me, user_name, version);
    snx_unlock(&l->lk);
    for (int i = 0; i < 4; i++) host[i] = info->nodes[0].ipv4[3 - i];
    if (l->room) return room_connect(l, &me, host, wait_ms);
    int r = snx_socket_open(&s, SNX_AF_INET, SNX_SOCK_STREAM, 0);
    if (r) return r;
    tv.sec = (int64_t)snx_le64((uint64_t)(wait_ms / 1000)); /* the guest's order, as every option value */
    tv.usec = (int64_t)snx_le64((uint64_t)((wait_ms % 1000) * 1000));
    snx_socket_setsockopt(s, SNX_SOL_SOCKET, SNX_SO_SNDTIMEO, &tv, sizeof tv);
    addr_of(&a, host, SNX_LDN_PORT);
    r = snx_socket_connect(s, &a);
    if (r == -SNX_EINPROGRESS) r = -SNX_ETIMEDOUT;
    if (!r) r = send_frame_tcp(s, SNX_LDN_CONNECT, &me, sizeof me);
    if (r) {
        snx_socket_unref(s);
        return r;
    }
    snx_lock(&l->lk);
    memset(&l->info, 0, sizeof l->info);
    memset(l->updates, 0, sizeof l->updates);
    l->client = s;
    l->client_rxn = 0;
    l->connected = 0;
    l->reason = SNX_LDN_REASON_NONE;
    l->role = ROLE_STATION;
    drain_event(l->changed_ev);
    snx_unlock(&l->lk);
    signal_event(l->wake);
    int64_t deadline = snx_net_now_us() + (int64_t)wait_ms * 1000;
    for (;;) {
        int64_t now = snx_net_now_us();
        int connected;
        snx_lock(&l->lk);
        connected = l->connected;
        int gone = l->client == NULL;
        snx_unlock(&l->lk);
        if (connected) return 0;
        if (gone) return -SNX_ECONNRESET; /* the host closed on me: full, or going away */
        if (now >= deadline) break;
        wait_event(l->changed_ev, (int)((deadline - now + 999) / 1000));
    }
    snx_lock(&l->lk);
    if (l->client) drop_client(l);
    l->role = ROLE_NONE;
    snx_unlock(&l->lk);
    return -SNX_ETIMEDOUT;
}
void snx_ldn_disconnect(SnxLdn* l) {
    snx_lock(&l->lk);
    if (l->role == ROLE_STATION) {
        if (l->room && snx_room_send_ldn(l->room, SNX_ROOM_LDN_DISCONNECT, l->host_ip, 0, &l->me, sizeof l->me))
            snx_net_log("ldn: the host wasn't told this station left");
        if (l->client) drop_client(l);
        l->connected = 0;
        l->reason = SNX_LDN_REASON_DISCONNECTED_BY_USER;
        l->role = ROLE_NONE;
    }
    snx_unlock(&l->lk);
}

int snx_ldn_network_info(SnxLdn* l, SnxLdnNetworkInfo* out, uint8_t updates[SNX_LDN_NODES]) {
    snx_lock(&l->lk);
    int r = l->role == ROLE_HOST || (l->role == ROLE_STATION && l->connected);
    if (r) {
        *out = l->info;
        if (updates) {
            memcpy(updates, l->updates, SNX_LDN_NODES);
            memset(l->updates, 0, sizeof l->updates);
        }
    }
    snx_unlock(&l->lk);
    return r;
}
int snx_ldn_connected(SnxLdn* l) {
    snx_lock(&l->lk);
    int r = l->role == ROLE_HOST || (l->role == ROLE_STATION && l->connected);
    snx_unlock(&l->lk);
    return r;
}
int snx_ldn_disconnect_reason(SnxLdn* l) {
    snx_lock(&l->lk);
    int r = l->reason;
    snx_unlock(&l->lk);
    return r;
}
