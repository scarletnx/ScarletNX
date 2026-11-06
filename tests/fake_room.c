/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The tests' Eden room (fake_room.h), read from Eden's src/network/room.cpp: HandleJoinRequest's checks and answers,
   BroadcastRoomInformation, HandleProxyPacket and HandleLdnPacket (to everyone but the sender for a broadcast, else to
   the member at the remote address), HandleModKickPacket, SendStatusMessage where those send it, HandleChatPacket's
   relay and the close. Its own thread runs ENet */
#include "fake_room.h"
#include "common/network/net_internal.h"
#include <enet/enet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 4
typedef struct FakeMember {
    ENetPeer* peer;
    char nickname[64];
    uint8_t ip[4];
} FakeMember;
typedef struct DelayedDatagram {
    struct DelayedDatagram* next;
    ENetPeer* sender;
    int64_t due;
    size_t size;
    uint8_t bytes[];
} DelayedDatagram;
struct FakeRoom {
    SnxLock lk;
    ENetHost* host;
    uint16_t port;
    FakeMember members[SLOTS];
    int stop, done, close_now;
    char kick[64];
    char chat_from[64], chat[512]; /* a chat message to relay as from that member, once */
    uint8_t last_datagram[64];
    size_t last_datagram_size;
    char last_game[64];
    uint64_t last_game_id;
    int ldn_count[8];
    size_t ldn_size[8];
    unsigned delay_ms;
    DelayedDatagram* pending;
};

static void fail(const char* what) {
    fprintf(stderr, "fake_room.c: %s\n", what);
    exit(1);
}
typedef struct Rd {
    const uint8_t* p;
    size_t n, at;
    int bad;
} Rd;
static uint32_t rd_be(Rd* r, int bytes) {
    uint32_t v = 0;
    int i;
    if (r->at + (size_t)bytes > r->n) {
        r->bad = 1;
        return 0;
    }
    for (i = 0; i < bytes; i++) v = v << 8 | r->p[r->at++];
    return v;
}
static void rd_string(Rd* r, char* out, size_t size) {
    uint32_t n = rd_be(r, 4);
    if (r->bad || r->at + n > r->n || n >= size) {
        r->bad = 1;
        out[0] = 0;
        return;
    }
    memcpy(out, r->p + r->at, n);
    out[n] = 0;
    r->at += n;
}
typedef struct Wr {
    uint8_t b[2048];
    size_t n;
} Wr;
static void wr_be(Wr* w, uint32_t v, int bytes) {
    while (bytes--) w->b[w->n++] = (uint8_t)(v >> (8 * bytes));
}
static void wr_string(Wr* w, const char* s) {
    wr_be(w, (uint32_t)strlen(s), 4);
    memcpy(w->b + w->n, s, strlen(s));
    w->n += strlen(s);
}
/* ENet takes the packet only when it is sent: one to a peer that left already (a test's client that closed) is the
   sender's to free */
static void room_send(ENetPeer* peer, const uint8_t* data, size_t size) {
    ENetPacket* packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    if (packet && enet_peer_send(peer, 0, packet) < 0) enet_packet_destroy(packet);
}
static void room_information(FakeRoom* f) {
    Wr w;
    int i, n = 0;
    w.n = 0;
    wr_be(&w, 3, 1);
    wr_string(&w, "Test room");
    wr_string(&w, "");
    wr_be(&w, SLOTS, 4);
    wr_be(&w, f->port, 2);
    wr_string(&w, "");
    wr_string(&w, "");
    for (i = 0; i < SLOTS; i++) n += f->members[i].peer != NULL;
    wr_be(&w, (uint32_t)n, 4);
    for (i = 0; i < SLOTS; i++) {
        if (!f->members[i].peer) continue;
        wr_string(&w, f->members[i].nickname);
        memcpy(w.b + w.n, f->members[i].ip, 4);
        w.n += 4;
        wr_string(&w, "game");
        wr_be(&w, 0, 4);
        wr_be(&w, 0, 4);
        wr_string(&w, "1.0");
        wr_string(&w, "");
        wr_string(&w, "");
        wr_string(&w, "");
    }
    for (i = 0; i < SLOTS; i++)
        if (f->members[i].peer) room_send(f->members[i].peer, w.b, w.n);
}
/* SendStatusMessage: type (IdMemberJoin 1, IdMemberLeave 2, IdMemberKicked 3) of nickname, with no username, to every
   member in the room */
static void room_status(FakeRoom* f, uint8_t type, const char* nickname) {
    Wr w;
    int i;
    w.n = 0;
    wr_be(&w, 14, 1); /* IdStatusMessage */
    wr_be(&w, type, 1);
    wr_string(&w, nickname);
    wr_string(&w, "");
    for (i = 0; i < SLOTS; i++)
        if (f->members[i].peer) room_send(f->members[i].peer, w.b, w.n);
}
/* IsValidNickname, written apart from users.c's */
static int nickname_ok(FakeRoom* f, const char* s) {
    size_t n = strlen(s), i;
    if (n < 4 || n > 20) return 0;
    for (i = 0; i < n; i++)
        if (!strchr("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ._-", s[i])) return 0;
    for (i = 0; i < SLOTS; i++)
        if (f->members[i].peer && !strcmp(f->members[i].nickname, s)) return 0;
    return 1;
}
static void room_join(FakeRoom* f, ENetPeer* peer, const uint8_t* data, size_t size) {
    char nickname[64], password[64], token[64];
    uint8_t answer = 0, ip[4], ok[5];
    uint32_t version;
    Rd r;
    int i, free_slot = -1;
    r.p = data;
    r.n = size;
    r.at = 1;
    r.bad = 0;
    rd_string(&r, nickname, sizeof nickname);
    for (i = 0; i < 4; i++) ip[i] = (uint8_t)rd_be(&r, 1);
    version = rd_be(&r, 4);
    rd_string(&r, password, sizeof password);
    rd_string(&r, token, sizeof token);
    if (r.bad || r.at != size) fail("a join request that isn't one");
    if (ip[0] != 255 || ip[1] != 255 || ip[2] != 255 || ip[3] != 255 || token[0]) fail("a join request's ip or token");
    for (i = 0; i < SLOTS && free_slot < 0; i++)
        if (!f->members[i].peer) free_slot = i;
    if (free_slot < 0)
        answer = 13; /* IdRoomIsFull */
    else if (strcmp(password, "secret"))
        answer = 11; /* IdWrongPassword */
    else if (!nickname_ok(f, nickname))
        answer = 8; /* IdNameCollision */
    else if (version != 1)
        answer = 10; /* IdVersionMismatch */
    if (answer) {
        room_send(peer, &answer, 1);
        return;
    }
    room_status(f, 1, nickname); /* to those already in */
    f->members[free_slot].peer = peer;
    strcpy(f->members[free_slot].nickname, nickname);
    f->members[free_slot].ip[0] = 192; /* GenerateFakeIPAddress: the first free */
    f->members[free_slot].ip[1] = 168;
    f->members[free_slot].ip[2] = 1;
    f->members[free_slot].ip[3] = (uint8_t)(free_slot + 1);
    room_information(f);
    ok[0] = 2; /* IdJoinSuccess */
    memcpy(ok + 1, f->members[free_slot].ip, 4);
    room_send(peer, ok, 5);
}
static void room_relay(FakeRoom* f, ENetPeer* from, const uint8_t* data, size_t size, size_t remote_at,
                       size_t broadcast_at) {
    int i;
    if (size <= broadcast_at) fail("a relayed message too short");
    for (i = 0; i < SLOTS; i++) {
        FakeMember* m = &f->members[i];
        if (!m->peer || m->peer == from) continue;
        if (data[broadcast_at] || !memcmp(m->ip, data + remote_at, 4)) room_send(m->peer, data, size);
    }
}
static void room_message(FakeRoom* f, ENetPeer* peer, const uint8_t* data, size_t size) {
    Rd r;
    switch (data[0]) {
        case 1: room_join(f, peer, data, size); break;
        case 4: /* IdSetGameInfo */
            r.p = data;
            r.n = size;
            r.at = 1;
            r.bad = 0;
            snx_lock(&f->lk);
            rd_string(&r, f->last_game, sizeof f->last_game);
            f->last_game_id = (uint64_t)rd_be(&r, 4) << 32;
            f->last_game_id |= rd_be(&r, 4);
            snx_unlock(&f->lk);
            if (r.bad) fail("a game info that isn't one");
            break;
        case 5: /* IdProxyPacket: type, domain, ip, port, domain, ip (at 9), port, protocol, broadcast (at 16) */
            snx_lock(&f->lk);
            f->last_datagram_size = size;
            memcpy(f->last_datagram, data, size < sizeof f->last_datagram ? size : sizeof f->last_datagram);
            unsigned delay_ms = f->delay_ms;
            snx_unlock(&f->lk);
            if (delay_ms) {
                DelayedDatagram* d = malloc(sizeof *d + size);
                if (!d) fail("delayed datagram allocation");
                d->sender = peer;
                d->due = snx_net_now_us() + delay_ms * 1000;
                d->size = size;
                memcpy(d->bytes, data, size);
                DelayedDatagram** link = &f->pending;
                while (*link && (*link)->due <= d->due) link = &(*link)->next;
                d->next = *link;
                *link = d;
            } else
                room_relay(f, peer, data, size, 9, 16);
            break;
        case 6: /* IdLdnPacket: type, ldn type, local ip, remote ip (at 6), broadcast (at 10), u32 size (at 11) */
            if (size < 15 || data[1] >= 8) fail("an ldn message that isn't one");
            snx_lock(&f->lk);
            f->ldn_count[data[1]]++;
            r.p = data;
            r.n = size;
            r.at = 11;
            r.bad = 0;
            f->ldn_size[data[1]] = rd_be(&r, 4);
            snx_unlock(&f->lk);
            room_relay(f, peer, data, size, 6, 10);
            break;
        default: fail("a message a member doesn't send");
    }
}
static void room_thread(void* arg) {
    FakeRoom* f = arg;
    for (;;) {
        ENetEvent ev;
        char kick[64], chat_from[64], chat[512];
        int i, stop, close_now;
        snx_lock(&f->lk);
        stop = f->stop;
        close_now = f->close_now;
        f->close_now = 0;
        strcpy(kick, f->kick);
        f->kick[0] = 0;
        strcpy(chat_from, f->chat_from);
        strcpy(chat, f->chat);
        f->chat_from[0] = 0;
        snx_unlock(&f->lk);
        if (stop) break;
        DelayedDatagram** link = &f->pending;
        while (*link) {
            DelayedDatagram* d = *link;
            if (snx_net_now_us() >= d->due) {
                *link = d->next;
                room_relay(f, d->sender, d->bytes, d->size, 9, 16);
                free(d);
            } else
                link = &d->next;
        }
        if (close_now) {
            uint8_t closing = 12; /* IdCloseRoom */
            for (i = 0; i < SLOTS; i++)
                if (f->members[i].peer) room_send(f->members[i].peer, &closing, 1);
            enet_host_flush(f->host);
        }
        for (i = 0; kick[0] && i < SLOTS; i++)
            if (f->members[i].peer && !strcmp(f->members[i].nickname, kick)) {
                uint8_t kicked = 15; /* IdHostKicked, then HandleModKickPacket's disconnect */
                room_send(f->members[i].peer, &kicked, 1);
                enet_host_flush(f->host);
                enet_peer_disconnect_later(f->members[i].peer, 0);
                memset(&f->members[i], 0, sizeof f->members[i]);
                room_status(f, 3, kick);
                room_information(f);
            }
        if (chat_from[0]) { /* HandleChatPacket: to everyone but the sender */
            Wr w;
            w.n = 0;
            wr_be(&w, 7, 1); /* IdChatMessage: the nickname, the username, the message */
            wr_string(&w, chat_from);
            wr_string(&w, "");
            wr_string(&w, chat);
            for (i = 0; i < SLOTS; i++)
                if (f->members[i].peer && strcmp(f->members[i].nickname, chat_from))
                    room_send(f->members[i].peer, w.b, w.n);
        }
        for (int k = enet_host_service(f->host, &ev, 5); k > 0; k = enet_host_check_events(f->host, &ev)) {
            if (ev.type == ENET_EVENT_TYPE_RECEIVE) {
                room_message(f, ev.peer, ev.packet->data, ev.packet->dataLength);
                enet_packet_destroy(ev.packet);
            } else if (ev.type == ENET_EVENT_TYPE_DISCONNECT) {
                char left[64] = {0}; /* HandleClientDisconnection: a member the room still had */
                for (i = 0; i < SLOTS; i++)
                    if (f->members[i].peer == ev.peer) {
                        strcpy(left, f->members[i].nickname);
                        memset(&f->members[i], 0, sizeof f->members[i]);
                    }
                if (left[0]) room_status(f, 2, left);
                room_information(f);
            }
        }
    }
    while (f->pending) {
        DelayedDatagram* d = f->pending;
        f->pending = d->next;
        free(d);
    }
    snx_lock(&f->lk);
    f->done = 1;
    snx_unlock(&f->lk);
}
void fake_room_datagram_delay(FakeRoom* f, unsigned milliseconds) {
    snx_lock(&f->lk);
    f->delay_ms = milliseconds;
    snx_unlock(&f->lk);
}

FakeRoom* fake_room_start(void) {
    ENetAddress a;
    FakeRoom* f = calloc(1, sizeof *f);
    if (!f || snx_net_init() || enet_initialize()) fail("no room: out of memory, or no network stack");
    snx_lock_init(&f->lk);
    enet_address_set_host(&a, "127.0.0.1");
    a.port = 0;
    f->host = enet_host_create(&a, SLOTS + 2, 1, 0, 0);
    if (!f->host || enet_socket_get_address(f->host->socket, &a) || !a.port) fail("no room: ENet's host");
    f->port = a.port;
    if (snx_net_thread(room_thread, f)) fail("no room: its thread");
    return f;
}
void fake_room_stop(FakeRoom* f) {
    int done = 0;
    snx_lock(&f->lk);
    f->stop = 1;
    snx_unlock(&f->lk);
    while (!done) {
        snx_net_sleep_us(1000);
        snx_lock(&f->lk);
        done = f->done;
        snx_unlock(&f->lk);
    }
    enet_host_destroy(f->host);
    snx_lock_destroy(&f->lk);
    free(f);
    enet_deinitialize();
}
uint16_t fake_room_port(const FakeRoom* f) { return f->port; }
void fake_room_close(FakeRoom* f) {
    snx_lock(&f->lk);
    f->close_now = 1;
    snx_unlock(&f->lk);
}
void fake_room_kick(FakeRoom* f, const char* nickname) {
    snx_lock(&f->lk);
    snprintf(f->kick, sizeof f->kick, "%s", nickname);
    snx_unlock(&f->lk);
}
void fake_room_chat(FakeRoom* f, const char* nickname, const char* text) {
    snx_lock(&f->lk);
    snprintf(f->chat_from, sizeof f->chat_from, "%s", nickname);
    snprintf(f->chat, sizeof f->chat, "%s", text);
    snx_unlock(&f->lk);
}
int fake_room_ldn_count(FakeRoom* f, int type, size_t* last_size) {
    int n;
    snx_lock(&f->lk);
    n = f->ldn_count[type];
    if (last_size) *last_size = f->ldn_size[type];
    snx_unlock(&f->lk);
    return n;
}
size_t fake_room_last_datagram(FakeRoom* f, uint8_t* out, size_t max) {
    size_t n;
    snx_lock(&f->lk);
    n = f->last_datagram_size;
    memcpy(out, f->last_datagram, n < max ? n : max);
    snx_unlock(&f->lk);
    return n;
}
void fake_room_last_game(FakeRoom* f, char* name, size_t size, uint64_t* id) {
    snx_lock(&f->lk);
    snprintf(name, size, "%s", f->last_game);
    *id = f->last_game_id;
    snx_unlock(&f->lk);
}
