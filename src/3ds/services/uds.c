/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project */
/* Experimental two-player UDS over Eden room datagrams. Guest wire structures and
   command layouts referenced against Azahar nwm_uds.cpp/uds_beacon.cpp, Copyright
   2014-2026 Citra/Azahar contributors, GPL-2.0-or-later. This transport is private
   to this prototype: it does not interoperate with consoles, Azahar, or Switch LDN. */
#include "3ds/services/uds.h"
#include "3ds/runtime/internal.h"
#include "common/network/net_room.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/users.h"
#include "snx_settings.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UDS_PORT      24873u
#define UDS_DATA      1478u
#define UDS_QUEUE     256u
#define UDS_BAD       0xd9001830u
#define UDS_STATE     0xc86113eau
#define UDS_NOT_FOUND 0xc8a113fau
#define UDS_NETWORK   0xc8a113f8u
static struct {
    char password[256], nickname[21];
} g_uds;
void snx_ctr_uds_settings(const char* data) {
    g_uds.password[0] = g_uds.nickname[0] = 0;
    char* config = snx_menu_dir(data, SNX_USER_CONFIG);
    char* settings = config ? snx_path_join(config, "settings.ini") : NULL;
    size_t size = 0;
    char* text = settings ? (char*)snx_read_file(settings, &size) : NULL;
    if (text) {
        SnxIni ini;
        char *key, *value;
        snx_ini_start(&ini, text);
        while (snx_ini_next(&ini, &key, &value)) {
            if (!strcmp(ini.section, "User") && !strcmp(key, "user")) {
                char* end = NULL;
                long user = strtol(value, &end, 10);
                if (*value && !*end && user >= 1 && user <= SNX_USERS_MAX) g_settings.user = (int)user;
                continue;
            }
            if (strcmp(ini.section, "Network")) continue;
            if (strcmp(key, "room") && strcmp(key, "ldn") && strcmp(key, "airplane_mode")) continue;
            if (!strcmp(key, "room") && strlen(value) < sizeof g_settings.eden_room)
                snprintf(g_settings.eden_room, sizeof g_settings.eden_room, "%s", value);
            else if (!strcmp(key, "ldn") && (!strcmp(value, "lan") || !strcmp(value, "room") || !strcmp(value, "off")))
                snprintf(g_settings.local_wireless, sizeof g_settings.local_wireless, "%s", value);
            else if (!strcmp(key, "airplane_mode") && (!strcmp(value, "0") || !strcmp(value, "1")))
                g_settings.airplane_mode = value[0] == '1';
        }
    }
    free(text);
    free(settings);
    free(config);
    char* users = snx_users_dir(data);
    char why[256];
    if (users && !snx_users_load(users, g_settings.user, why, sizeof why)) {
        const SnxUser* user = snx_users_selected();
        if (user) snprintf(g_uds.nickname, sizeof g_uds.nickname, "%.20s", user->nickname);
    }
    char* online = users ? snx_path_join(users, "online.ini") : NULL;
    text = online ? (char*)snx_read_file(online, &size) : NULL;
    if (text) {
        SnxIni ini;
        char *key, *value;
        snx_ini_start(&ini, text);
        while (snx_ini_next(&ini, &key, &value))
            if (!ini.section[0] && !strcmp(key, "room_password") && strlen(value) < sizeof g_uds.password)
                snprintf(g_uds.password, sizeof g_uds.password, "%s", value);
    }
    free(text);
    free(online);
    free(users);
}
enum { MSG_BEACON = 1, MSG_JOIN, MSG_SYNC, MSG_DATA, MSG_LEAVE, MSG_CLOSE, MSG_PING, MSG_PONG };
typedef struct {
    uint8_t bytes[40];
} Node;
typedef struct {
    uint8_t bytes[264];
} Network;
typedef struct {
    uint32_t size;
    uint16_t source;
    uint8_t channel;
    uint8_t bytes[UDS_DATA];
} Packet;
typedef struct {
    uint32_t id, event, limit;
    uint16_t source;
    uint8_t channel;
    Packet queue[UDS_QUEUE];
    uint32_t head, count, used;
} Binding;
struct SnxCtrUds {
    SnxRoom* room;
    SnxSocket* socket;
    uint8_t ip[4], mac[6], peer[4], secret[32];
    uint64_t session, peer_seen, next_beacon, last_poll;
    uint64_t next_ping, ping_sent, next_report, rtt_ns;
    uint32_t sent_packets, received_packets, dropped_packets, rtt_samples;
    uint32_t event, status, reason, probe_oui;
    int32_t probe_data;
    uint16_t own, changed, mask, allowed_nodes;
    uint32_t scan_queries, scan_last_count;
    uint32_t join_thread, join_header;
    uint64_t next_join;
    Network net;
    Node me, nodes[16];
    Network found[16];
    Node found_nodes[16][16];
    uint64_t found_session[16], found_at[16];
    Binding binds[16];
};
static unsigned population(uint16_t m) {
    unsigned n = 0;
    while (m) {
        n += m & 1;
        m >>= 1;
    }
    return n;
}
static void trace_packet(const SnxCtrProcess* p, const char* direction, const uint8_t* bytes, unsigned size) {
    const char* enabled = getenv("SNX_3DS_UDS_PACKET_TRACE");
    (void)p;
    if (!enabled || strcmp(enabled, "1")) return;
    fprintf(stderr, "uds packet %s %.3f %u:", direction, snx_ctr_now_ns() / 1000000.0, size);
    for (unsigned i = 0; i < size && i < 96; ++i) fprintf(stderr, "%02x", bytes[i]);
    fputc('\n', stderr);
}
static void signal_event(SnxCtrProcess* p, uint32_t id) {
    if (id && p->objects[id].type == CTR_OBJ_EVENT) {
        p->objects[id].data.event.signaled = 1;
        snx_ctr_wake_waiters(p, id);
    }
}
static void reply(SnxCtrIpc* c, unsigned n, unsigned t, uint32_t result) {
    memset(c->out, 0, sizeof c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | (n << 6) | t;
    c->out[1] = result;
}
static int event_new(SnxCtrProcess* p, uint32_t* id) {
    uint32_t h = 0, r = snx_ctr_object_create(p, CTR_OBJ_EVENT, &h);
    if (r) return 0;
    *id = snx_ctr_handle_object(p, h);
    ++p->objects[*id].refs;
    p->objects[*id].data.event.reset_type = CTR_RESET_ONESHOT;
    snx_ctr_handle_close(p, h);
    return 1;
}
static int handle_reply(SnxCtrIpc* c, uint32_t id) {
    uint32_t h = 0, r = snx_ctr_handle_add(c->process, id, &h);
    reply(c, 1, r ? 0 : 2, r);
    if (!r) snx_ctr_ipc_output_handle(c, 3, h);
    return !r;
}
static int input_buffer(SnxCtrIpc* c, unsigned id, void* out, uint32_t size) {
    unsigned n = (c->in[0] >> 6) & 63, t = c->in[0] & 63, end = 1 + n + t;
    for (unsigned at = 1 + n; at < end;) {
        uint32_t d = c->in[at++];
        unsigned words = 1;
        if ((d & 15) == 2) {
            if (((d >> 10) & 15) == id) {
                if ((d >> 14) < size || at >= end) break;
                if (size && snx_ctr_read_ram(c->process->space, c->in[at], out, size) != SNX_CTR_MEM_OK) break;
                return 1;
            }
        } else if (!(d & 0x03ffffefu))
            words = (d >> 26) + 1;
        at += words;
    }
    snx_ctr_ipc_bad(c);
    return 0;
}
static int output_buffer(SnxCtrIpc* c, const void* data, uint32_t size, unsigned word) {
    uint8_t d[8];
    uint32_t tls = c->process->cpu.state.cpu.tpidruro;
    if (snx_ctr_read_ram(c->process->space, tls + 0x180, d, 8) != SNX_CTR_MEM_OK) {
        snx_ctr_ipc_bad(c);
        return 0;
    }
    uint32_t desc = snx_ld_le32(d), address = snx_ld_le32(d + 4);
    if ((desc & 0x3fff) != 2 || (desc >> 14) < size ||
        (size && snx_ctr_write_ram(c->process->space, address, data, size) != SNX_CTR_MEM_OK)) {
        snx_ctr_ipc_bad(c);
        return 0;
    }
    c->out[word] = (size << 14) | 2;
    c->out[word + 1] = address;
    return 1;
}
static void address(SnxSockAddr* a, const uint8_t ip[4]) {
    memset(a, 0, sizeof *a);
    a->family = SNX_AF_INET;
    a->port = (uint16_t)((UDS_PORT >> 8) | (UDS_PORT << 8));
    memcpy(a->addr, ip, 4);
}
static int send_message(struct SnxCtrUds* u, unsigned type, const uint8_t* ip, const void* data, size_t size) {
    /* lan hosts a network without a relay socket: no datagram leaves it. */
    if (!u->socket) return 0;
    uint8_t wire[2048] = {'S', 'N', 'U', 'D', 1, 0, 0, 0};
    if (size > sizeof wire - 16) return 0;
    wire[5] = (uint8_t)type;
    snx_st_le16(wire + 6, (uint16_t)size);
    snx_st_le64(wire + 8, u->session);
    if (size) memcpy(wire + 16, data, size);
    SnxSockAddr a;
    const uint8_t broadcast[4] = {192, 168, 255, 255};
    address(&a, ip ? ip : broadcast);
    return snx_socket_send(u->socket, wire, size + 16, SNX_MSG_DONTWAIT, &a) == (int64_t)(size + 16);
}
static void send_state(struct SnxCtrUds* u, unsigned type, const uint8_t* ip) {
    uint8_t b[936];
    memcpy(b, u->net.bytes, 264);
    memcpy(b + 264, u->nodes, 640);
    /* Discovery is public; only an authenticated unicast SYNC carries the hash. */
    memset(b + 904, 0, 32);
    if (type == MSG_SYNC && ip) memcpy(b + 904, u->secret, 32);
    send_message(u, type, ip, b, sizeof b);
}
static void clear_bindings(SnxCtrProcess* p) {
    struct SnxCtrUds* u = p->uds;
    for (unsigned i = 0; i < 16; i++)
        if (u->binds[i].event) {
            signal_event(p, u->binds[i].event);
            snx_ctr_object_release(p, u->binds[i].event);
        }
    memset(u->binds, 0, sizeof u->binds);
}
/* Retain the caller while its IPC reply is pending, never a guest RAM pointer.
   Completion targets its TLS even when a different guest thread is running. */
static void finish_join(SnxCtrProcess* p, uint32_t result) {
    struct SnxCtrUds* u = p->uds;
    uint32_t id = u->join_thread;
    if (!id) return;
    CtrThread* t = &p->objects[id].data.thread;
    if (!t->dead) {
        uint8_t out[256] = {0};
        snx_st_le32(out, u->join_header);
        snx_st_le32(out + 4, result);
        uint32_t transport = snx_ctr_write_ram(p->space, t->context.tpidruro + 0x80, out, sizeof out) == SNX_CTR_MEM_OK
                                 ? 0
                                 : 0xe0e01bf5u;
        t->context.r[0] = transport;
        t->context.exit = t->context.syndrome = 0;
        t->wake_ns = 0;
        if (p->current_thread == id) {
            p->cpu.state.cpu.r[0] = transport;
            p->cpu.state.cpu.exit = p->cpu.state.cpu.syndrome = 0;
        }
        snx_ctr_thread_ready(p, id);
    }
    u->join_thread = 0;
    u->next_join = 0;
    snx_ctr_object_release(p, id);
}
void snx_ctr_uds_close(SnxCtrProcess* p) {
    struct SnxCtrUds* u = p->uds;
    if (!u) return;
    finish_join(p, UDS_NETWORK);
    if (u->socket && (u->status == 6 || u->status == 9))
        send_message(u, u->status == 6 ? MSG_CLOSE : MSG_LEAVE, u->peer, NULL, 0);
    clear_bindings(p);
    if (u->event) snx_ctr_object_release(p, u->event);
    if (u->socket) snx_socket_unref(u->socket);
    if (u->room) { snx_room_leave(u->room); }
    free(u);
    p->uds = NULL;
}
static void status_changed(SnxCtrProcess* p) { signal_event(p, p->uds->event); }
uint64_t snx_ctr_uds_rtt_ns(const SnxCtrProcess* p) { return p->uds ? p->uds->rtt_ns : 0; }
void snx_ctr_uds_poll(SnxCtrProcess* p) {
    struct SnxCtrUds* u = p->uds;
    if (!u || !u->socket) return;
    uint64_t now = snx_ctr_now_ns();
    if (u->join_thread) {
        CtrThread* caller = &p->objects[u->join_thread].data.thread;
        if (caller->dead || now >= caller->wake_ns) {
            send_message(u, MSG_LEAVE, u->peer, NULL, 0);
            u->status = 3;
            finish_join(p, UDS_NETWORK);
        } else if (now >= u->next_join) {
            uint8_t join[72];
            memcpy(join, u->me.bytes, 40);
            memcpy(join + 40, u->secret, 32);
            send_message(u, MSG_JOIN, u->peer, join, sizeof join);
            u->next_join = now + 250000000ull;
        }
    }
    if (u->mask == 3 && u->last_poll && now - u->last_poll > 100000000ull)
        fprintf(stderr, "3ds uds trace %.3f: local poll gap=%.1fms status=%u sent=%u received=%u\n", now / 1000000.0,
                (now - u->last_poll) / 1000000.0, u->status, u->sent_packets, u->received_packets);
    u->last_poll = now;
    for (unsigned tries = 0; tries < 64; tries++) {
        uint8_t b[2048];
        SnxSockAddr from;
        int64_t got = snx_socket_recv(u->socket, b, sizeof b, SNX_MSG_DONTWAIT, &from, NULL);
        if (got < 0) break;
        if (got < 16 || memcmp(b, "SNUD", 4) || b[4] != 1 || snx_ld_le16(b + 6) != (unsigned)got - 16 ||
            !memcmp(from.addr, u->ip, 4) || !snx_room_has_member(u->room, from.addr))
            continue;
        unsigned type = b[5], size = (unsigned)got - 16;
        uint64_t session = snx_ld_le64(b + 8);
        uint8_t* v = b + 16;
        if (type == MSG_BEACON && size == 936 && v[29] >= 2 && v[29] <= 16 && v[63] <= 200 && v[0] == 2 &&
            v[1] == 0x53 && !memcmp(v + 2, from.addr, 4)) {
            unsigned slot = 16;
            for (unsigned i = 0; i < 16; i++)
                if (u->found_at[i] && !memcmp(u->found[i].bytes, v, 6)) {
                    slot = i;
                    break;
                }
            if (slot == 16)
                for (unsigned i = 0; i < 16; i++)
                    if (!u->found_at[i] || now - u->found_at[i] > 3000000000ull) {
                        slot = i;
                        break;
                    }
            if (slot < 16) {
                memcpy(u->found[slot].bytes, v, 264);
                memcpy(u->found_nodes[slot], v + 264, 640);
                u->found_session[slot] = session;
                u->found_at[slot] = now;
            }
        } else if (type == MSG_JOIN && size == 72 && u->status == 6 && session == u->session &&
                   !memcmp(v + 40, u->secret, 32)) {
            if ((u->allowed_nodes & 2) && (u->mask == 1 || !memcmp(u->peer, from.addr, 4))) {
                /* A high-latency join can be retried before the first sync
                   arrives. Re-acknowledge it without announcing another join. */
                if (u->mask == 3) {
                    u->peer_seen = now;
                    send_state(u, MSG_SYNC, u->peer);
                    continue;
                }
                memcpy(u->peer, from.addr, 4);
                memcpy(u->nodes[1].bytes, v, 40);
                snx_st_le16(u->nodes[1].bytes + 32, 2);
                u->mask = 3;
                u->changed |= 2;
                u->reason = 1;
                u->peer_seen = now;
                u->net.bytes[28] = 2;
                send_state(u, MSG_SYNC, u->peer);
                status_changed(p);
                fprintf(stderr, "3ds uds: peer joined; two nodes connected\n");
            }
        } else if (type == MSG_SYNC && size == 936 && (u->status == 7 || u->status == 9) && session == u->session &&
                   !memcmp(from.addr, u->peer, 4) && !memcmp(v, u->net.bytes, 6) && !memcmp(v + 904, u->secret, 32)) {
            int first = u->status == 7;
            memcpy(u->net.bytes, v, 264);
            memcpy(u->nodes, v + 264, 640);
            u->own = 2;
            u->status = 9;
            u->mask = 3;
            u->peer_seen = now;
            u->reason = 1;
            if (first) {
                u->changed = 3;
                finish_join(p, 0);
                status_changed(p);
                fprintf(stderr, "3ds uds: connected to host\n");
            }
        } else if ((type == MSG_PING || type == MSG_PONG) && size == 8 && u->mask == 3 && session == u->session &&
                   !memcmp(from.addr, u->peer, 4)) {
            u->peer_seen = now;
            if (type == MSG_PING) {
                send_message(u, MSG_PONG, u->peer, v, 8);
            } else if (u->ping_sent && snx_ld_le64(v) == u->ping_sent && now >= u->ping_sent) {
                u->rtt_ns = now - u->ping_sent;
                ++u->rtt_samples;
                u->ping_sent = 0;
            }
        } else if (type == MSG_DATA && size >= 6 && (u->status == 6 || u->status == 9) && u->mask == 3 &&
                   session == u->session && !memcmp(from.addr, u->peer, 4)) {
            uint16_t src = snx_ld_le16(v), dest = snx_ld_le16(v + 2);
            unsigned payload = size - 6;
            if (src != (u->own == 1 ? 2 : 1) || (dest != u->own && dest != 65535) || payload > UDS_DATA) continue;
            u->peer_seen = now;
            if (v[4]) ++u->received_packets;
            if (v[4]) trace_packet(p, "recv", v, size);
            for (unsigned i = 0; i < 16; i++) {
                Binding* k = &u->binds[i];
                if (!k->id || k->channel != v[4] || (k->source != 65535 && k->source != src)) continue;
                if (k->count == UDS_QUEUE || payload > k->limit - k->used) {
                    ++u->dropped_packets;
                    continue;
                }
                Packet* q = &k->queue[(k->head + k->count) % UDS_QUEUE];
                q->size = payload;
                q->source = src;
                q->channel = v[4];
                memcpy(q->bytes, v + 6, payload);
                ++k->count;
                k->used += payload;
                signal_event(p, k->event);
            }
        } else if ((type == MSG_LEAVE || type == MSG_CLOSE) && !size && session == u->session &&
                   !memcmp(from.addr, u->peer, 4)) {
            fprintf(stderr, "3ds uds trace %.3f: received peer %s status=%u mask=%x sent=%u received=%u\n",
                    now / 1000000.0, type == MSG_CLOSE ? "CLOSE" : "LEAVE", u->status, u->mask, u->sent_packets,
                    u->received_packets);
            u->changed |= u->own ? u->mask & (uint16_t)~(1u << (u->own - 1)) : u->mask;
            u->mask = u->own == 1 ? 1 : 0;
            u->reason = 4;
            if (u->status == 7 || u->status == 9) u->status = 3;
            finish_join(p, UDS_NETWORK);
            u->net.bytes[28] = (uint8_t)population(u->mask);
            status_changed(p);
        }
    }
    if (u->mask == 3 && (u->status == 6 || u->status == 9)) {
        if (now >= u->next_ping && (!u->ping_sent || now - u->ping_sent >= 5000000000ull)) {
            uint8_t ping[8];
            snx_st_le64(ping, now);
            if (send_message(u, MSG_PING, u->peer, ping, sizeof ping)) u->ping_sent = now;
            u->next_ping = now + 1000000000ull;
        }
        if (now >= u->next_report) {
            fprintf(stderr, "3ds uds timing: peer RTT=%s%.1fms samples=%u game sent=%u received=%u queue drops=%u\n",
                    u->rtt_samples ? "" : "unavailable/", (double)u->rtt_ns / 1000000.0, u->rtt_samples,
                    u->sent_packets, u->received_packets, u->dropped_packets);
            fflush(stderr);
            u->next_report = now + 5000000000ull;
        }
    }
    if (now >= u->next_beacon) {
        u->next_beacon = now + 100000000ull;
        if (u->status == 6) {
            send_state(u, MSG_BEACON, NULL);
            if (u->mask == 3) send_state(u, MSG_SYNC, u->peer);
        }
        if (u->mask == 3 && (u->status == 6 || u->status == 9)) {
            uint8_t ping[6];
            snx_st_le16(ping, u->own);
            snx_st_le16(ping + 2, 65535);
            ping[4] = 0;
            ping[5] = 0;
            send_message(u, MSG_DATA, u->peer, ping, 6);
        }
    }
    if (u->mask == 3 && now - u->peer_seen > 10000000000ull) {
        u->changed |= u->own == 1 ? 2 : 1;
        u->mask = u->own == 1 ? 1 : 0;
        u->reason = 4;
        if (u->status == 9) u->status = 3;
        u->net.bytes[28] = (uint8_t)population(u->mask);
        status_changed(p);
        fprintf(stderr, "3ds uds: peer timed out\n");
    }
}
static int room_open(SnxCtrProcess* p) {
    struct SnxCtrUds* u = p->uds;
    const char* room = getenv("SNX_3DS_ROOM");
    if (!room || !*room) room = g_settings.eden_room;
    if (!*room) {
        fprintf(stderr, "3ds uds: select an Eden room in Network settings before enabling room wireless\n");
        return 0;
    }
    char host[256], nickname[21];
    const char* colon = strrchr(room, ':');
    size_t n = colon ? (size_t)(colon - room) : strlen(room);
    if (!n || n >= sizeof host) return 0;
    memcpy(host, room, n);
    host[n] = 0;
    uint16_t port = SNX_ROOM_PORT;
    if (colon) {
        char* end;
        unsigned long v = strtoul(colon + 1, &end, 10);
        if (!colon[1] || *end || !v || v > 65535) return 0;
        port = (uint16_t)v;
    }
    const char* name = getenv("SNX_3DS_NICKNAME");
    if (name && strlen(name) >= 4 && strlen(name) <= 20)
        snprintf(nickname, sizeof nickname, "%s", name);
    else {
        uint32_t random = 0;
        if (RAND_bytes((unsigned char*)&random, 4) != 1) return 0;
        snprintf(nickname, sizeof nickname, "%.11s-%08x", g_uds.nickname[0] ? g_uds.nickname : "Player", random);
    }
    const char* password = getenv("SNX_3DS_ROOM_PASSWORD");
    int why = 0;
    /* The room's title metadata is Switch-specific. Its unknown-title sentinel
       permits a 3DS name without sending an invalid Switch title ID. Actual UDS
       network and session identifiers travel inside the relay datagrams. */
    int result = snx_room_join(&u->room, host, port, nickname, password ? password : g_uds.password, p->title, 0,
                               "UDS-prototype-1", &why);
    if (result) {
        fprintf(stderr, "3ds uds: room join failed: %s\n", snx_room_why_text(why));
        return 0;
    }
    snx_room_low_latency(u->room);
    snx_room_fake_ip(u->room, u->ip);
    u->mac[0] = 2;
    u->mac[1] = 0x53;
    memcpy(u->mac + 2, u->ip, 4);
    if (snx_room_socket_open(u->room, &u->socket, SNX_AF_INET, SNX_SOCK_DGRAM, 0)) return 0;
    SnxSockAddr a;
    const uint8_t any[4] = {0};
    address(&a, any);
    uint32_t enabled = 1;
    if (snx_socket_set_nonblocking(u->socket, 1) ||
        snx_socket_setsockopt(u->socket, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &enabled, 4) ||
        snx_socket_bind(u->socket, &a))
        return 0;
    fprintf(stderr, "3ds uds: joined relay as %s, %u.%u.%u.%u\n", nickname, u->ip[0], u->ip[1], u->ip[2], u->ip[3]);
    return 1;
}
static int passphrase(SnxCtrIpc* c, unsigned id, uint32_t size, uint8_t hash[32]) {
    uint8_t b[256];
    unsigned n = 0;
    if (size > sizeof b) {
        snx_ctr_ipc_bad(c);
        return 0;
    }
    if (!input_buffer(c, id, b, size)) return 0;
    return EVP_Digest(b, size, hash, &n, EVP_sha256(), NULL) == 1 && n == 32;
}
/* Standard beacon framing, using the same zero AES key as Citra/Azahar for
   emulator-only discovery. All hashes/ciphers are OpenSSL implementations. */
static int cipher(const Network* net, uint8_t* b, size_t size) {
    uint8_t iv[16] = {0}, key[16] = {0};
    memcpy(iv, net->bytes, 6);
    for (unsigned i = 0; i < 4; i++) iv[6 + i] = net->bytes[19 - i];
    iv[10] = net->bytes[20];
    for (unsigned i = 0; i < 4; i++) iv[12 + i] = net->bytes[27 - i];
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    int out = 0, tail = 0;
    int ok = ctx && EVP_EncryptInit_ex(ctx, EVP_aes_128_ctr(), NULL, key, iv) == 1 &&
             EVP_EncryptUpdate(ctx, b, &out, b, (int)size) == 1 && EVP_EncryptFinal_ex(ctx, b + out, &tail) == 1 &&
             (size_t)(out + tail) == size;
    EVP_CIPHER_CTX_free(ctx);
    return ok;
}
static size_t beacon(const Network* net, const Node* nodes, uint8_t out[1200]) {
    uint8_t crypt[498] = {0};
    unsigned count = net->bytes[29], digest = 0;
    if (count < 2 || count > 16 || net->bytes[63] > 200) return 0;
    for (unsigned i = 0; i < count; i++) {
        uint8_t* v = crypt + 18 + 30 * i;
        const uint8_t* node = nodes[i].bytes;
        for (unsigned j = 0; j < 8; j++) v[j] = node[7 - j];
        for (unsigned j = 0; j < 10; j++) {
            v[8 + j * 2] = node[9 + j * 2];
            v[9 + j * 2] = node[8 + j * 2];
        }
        v[28] = node[33];
        v[29] = node[32];
    }
    size_t length = 18 + 30 * count;
    if (EVP_Digest(crypt + 16, length - 16, crypt, &digest, EVP_md5(), NULL) != 1 || digest != 16 ||
        !cipher(net, crypt, length))
        return 0;
    memset(out, 0, 1200);
    snx_st_le64(out, 900000000);
    snx_st_le16(out + 8, 100);
    snx_st_le16(out + 10, 0x0431);
    size_t at = 12;
    out[at++] = 0;
    out[at++] = 8;
    at += 8;
    const uint8_t dummy[9] = {221, 7, 0, 31, 50, 20, 10, 0, 0};
    memcpy(out + at, dummy, 9);
    at += 9;
    size_t tag = at;
    out[at++] = 221;
    out[at++] = (uint8_t)(52 + net->bytes[63]);
    memcpy(out + at, net->bytes + 12, 31);
    at += 31;
    size_t hash = at;
    at += 20;
    out[at++] = net->bytes[63];
    memcpy(out + at, net->bytes + 64, net->bytes[63]);
    at += net->bytes[63];
    if (EVP_Digest(out + tag + 2, at - tag - 2, out + hash, &digest, EVP_sha1(), NULL) != 1 || digest != 20) return 0;
    for (unsigned part = 0; part < 2; part++) {
        size_t start = part ? 250 : 0, len = length > start ? length - start : 0;
        if (len > 250) len = 250;
        out[at++] = 221;
        out[at++] = (uint8_t)(4 + len);
        out[at++] = 0;
        out[at++] = 31;
        out[at++] = 50;
        out[at++] = (uint8_t)(24 + part);
        memcpy(out + at, crypt + start, len);
        at += len;
    }
    return at;
}
static void initialize(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, (c->in[0] >> 16) == 0x1b ? 12 : 11, 2)) return;
    /* lan initializes without a relay socket. Off, airplane mode and unknown values refuse. */
    const char* wireless = g_settings.local_wireless;
    int room_wireless = !strcmp(wireless, "room"), lan_wireless = !strcmp(wireless, "lan");
    if (g_settings.airplane_mode || (!room_wireless && !lan_wireless)) {
        reply(c, 1, 0, UDS_NETWORK);
        return;
    }
    if (c->process->uds) {
        reply(c, 1, 0, UDS_STATE);
        return;
    }
    uint32_t shared = c->handle_count ? snx_ctr_handle_object(c->process, c->handle_in[0]) : 0;
    if (!shared || c->process->objects[shared].type != CTR_OBJ_SHARED ||
        c->process->objects[shared].data.shared.size != c->in[1]) {
        reply(c, 1, 0, UDS_BAD);
        return;
    }
    struct SnxCtrUds* u = calloc(1, sizeof *u);
    if (!u) {
        reply(c, 1, 0, 0xd86007f3u);
        return;
    }
    c->process->uds = u;
    snx_ctr_ipc_get_bytes(c, 2, u->me.bytes, 40);
    u->status = 3;
    if (!snx_ld_le64(u->me.bytes) && RAND_bytes(u->me.bytes, 8) != 1) {
        snx_ctr_uds_close(c->process);
        reply(c, 1, 0, UDS_NETWORK);
        return;
    }
    if (!event_new(c->process, &u->event) || (room_wireless && !room_open(c->process))) {
        snx_ctr_uds_close(c->process);
        reply(c, 1, 0, UDS_NETWORK);
        return;
    }
    if (!handle_reply(c, u->event)) snx_ctr_uds_close(c->process);
}
static void host_network(SnxCtrIpc* c) {
    struct SnxCtrUds* u = c->process->uds;
    Network n = {0};
    unsigned cmd = c->in[0] >> 16;
    uint32_t size;
    if (u->status != 3) {
        reply(c, 1, 0, UDS_STATE);
        return;
    }
    if (cmd == 4) {
        if (!snx_ctr_ipc_expect(c, 16, 2)) return;
        snx_ctr_ipc_get_bytes(c, 1, n.bytes, 60);
        size = c->in[16];
        if (!passphrase(c, 0, size, u->secret)) return;
    } else {
        if (!snx_ctr_ipc_expect(c, 1, 4) || !input_buffer(c, 1, n.bytes, 264)) return;
        size = c->in[1];
        if (!passphrase(c, 0, size, u->secret)) return;
    }
    if (n.bytes[29] < 2 || n.bytes[29] > 16 || n.bytes[63] > 200) {
        reply(c, 1, 0, UDS_BAD);
        return;
    }
    if (RAND_bytes((unsigned char*)&u->session, 8) != 1) {
        reply(c, 1, 0, UDS_NETWORK);
        return;
    }
    u->net = n;
    memcpy(u->net.bytes, u->mac, 6);
    u->net.bytes[6] = n.bytes[6] ? n.bytes[6] : 11;
    const uint8_t oui[4] = {0, 31, 50, 21};
    memcpy(u->net.bytes + 12, oui, 4);
    u->net.bytes[28] = 1;
    u->status = 6;
    u->reason = 1;
    u->own = 1;
    u->mask = 1;
    u->allowed_nodes = 0xffff;
    u->changed = 1;
    memset(u->nodes, 0, sizeof u->nodes);
    u->nodes[0] = u->me;
    snx_st_le16(u->nodes[0].bytes + 32, 1);
    status_changed(c->process);
    reply(c, 1, 0, 0);
    fprintf(stderr, "3ds uds: hosting network\n");
}
static void connect_network(SnxCtrIpc* c) {
    struct SnxCtrUds* u = c->process->uds;
    Network n = {0};
    unsigned cmd = c->in[0] >> 16;
    uint32_t size, type;
    if (u->status != 3) {
        reply(c, 1, 0, UDS_STATE);
        return;
    }
    if (cmd == 9) {
        if (!snx_ctr_ipc_expect(c, 17, 2)) return;
        snx_ctr_ipc_get_bytes(c, 1, n.bytes, 60);
        type = c->in[16];
        size = c->in[17];
        if (!passphrase(c, 0, size, u->secret)) return;
    } else {
        if (!snx_ctr_ipc_expect(c, 2, 4) || !input_buffer(c, 1, n.bytes, 264)) return;
        type = c->in[1];
        size = c->in[2];
        if (!passphrase(c, 0, size, u->secret)) return;
    }
    if (type != 1) {
        reply(c, 1, 0, UDS_BAD);
        return;
    } /* client only; spectators not modeled */
    unsigned i;
    for (i = 0; i < 16; i++)
        if (u->found_at[i] && !memcmp(u->found[i].bytes, n.bytes, 6) &&
            !memcmp(u->found[i].bytes + 16, n.bytes + 16, 12))
            break;
    if (i == 16) {
        reply(c, 1, 0, UDS_NOT_FOUND);
        return;
    }
    u->net = u->found[i];
    u->session = u->found_session[i];
    memcpy(u->peer, n.bytes + 2, 4);
    u->status = 7;
    u->own = u->mask = 0;
    u->join_thread = c->process->current_thread;
    u->join_header = (c->in[0] & 0xffff0000u) | 0x40;
    ++c->process->objects[u->join_thread].refs;
    snx_ctr_thread(c->process)->wake_ns = snx_ctr_now_ns() + 5000000000ull;
    c->state = SNX_CTR_IPC_BLOCKED;
    snx_ctr_uds_poll(c->process);
}
static void scan(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 16, 4)) return;
    struct SnxCtrUds* u = c->process->uds;
    uint32_t desc = c->in[19], dest = c->in[20], limit = c->in[1];
    if ((desc & 15) != 12 || (desc >> 4) < limit || limit < 12 || limit > 65536) {
        snx_ctr_ipc_bad(c);
        return;
    }
    uint8_t* out = calloc(1, limit);
    if (!out) {
        reply(c, 1, 0, 0xd86007f3u);
        return;
    }
    /* The console/Azahar suspend the calling guest thread during a scan, not
       the host execution thread. Poll once and return a snapshot; the scheduler
       will let rendering and the other guest threads run during the delay. */
    snx_ctr_uds_poll(c->process);
    size_t used = 12;
    unsigned count = 0;
    uint8_t mac[6];
    snx_ctr_ipc_get_bytes(c, 4, mac, 6);
    uint64_t now = snx_ctr_now_ns();
    for (unsigned i = 0; i < 16; i++) {
        Network* n = &u->found[i];
        if (!u->found_at[i] || now - u->found_at[i] > 3000000000ull) continue;
        if (memcmp(mac, "\xff\xff\xff\xff\xff\xff", 6) && memcmp(mac, n->bytes, 6)) continue;
        /* Like Azahar, deliver matching-sender beacons to the SDK. The SDK
           interprets WLAN communication/application identifiers itself. */
        uint8_t frame[1200];
        size_t bytes = beacon(n, u->found_nodes[i], frame);
        if (!bytes || bytes + 28 > limit - used) continue;
        uint8_t* v = out + used;
        snx_st_le32(v, (uint32_t)bytes + 28);
        v[5] = n->bytes[6];
        memcpy(v + 8, n->bytes, 6);
        snx_st_le32(v + 20, (uint32_t)bytes + 28);
        snx_st_le32(v + 24, 28);
        memcpy(v + 28, frame, bytes);
        used += 28 + bytes;
        ++count;
    }
    snx_st_le32(out, limit);
    snx_st_le32(out + 4, (uint32_t)used);
    snx_st_le32(out + 8, count);
    if (snx_ctr_write_ram(c->process->space, dest, out, used) != SNX_CTR_MEM_OK) snx_ctr_ipc_bad(c);
    free(out);
    reply(c, 1, 2, 0);
    c->out[2] = desc;
    c->out[3] = dest;
    if (!c->transport_result) {
        snx_ctr_thread_deadline(c->process, 300000000u, 0);
        if (!u->scan_queries || count != u->scan_last_count)
            fprintf(stderr,
                    "3ds uds: scan %u, %u networks, requested WLAN=%08x id=%u sender=%02x:%02x:%02x:%02x:%02x:%02x\n",
                    u->scan_queries + 1, count, c->in[15], c->in[16], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        ++u->scan_queries;
        u->scan_last_count = count;
    }
}
static void decrypt_beacon(SnxCtrIpc* c) {
    if (!snx_ctr_ipc_expect(c, 0, 6)) return;
    Network n;
    if (!input_buffer(c, 1, n.bytes, 264)) return;
    /* These emulator beacons originate in this room. Use the matching, verified
       advertisement's node list instead of accepting unbounded encrypted inputs. */
    struct SnxCtrUds* u = c->process->uds;
    for (unsigned i = 0; i < 16; i++)
        if (u->found_at[i] && !memcmp(u->found[i].bytes, n.bytes, 6) &&
            !memcmp(u->found[i].bytes + 16, n.bytes + 16, 12)) {
            reply(c, 1, 2, 0);
            output_buffer(c, u->found_nodes[i], 640, 2);
            return;
        }
    reply(c, 1, 0, UDS_NOT_FOUND);
}
void snx_ctr_uds_dispatch(SnxCtrIpc* c) {
    unsigned cmd = c->in[0] >> 16;
    if (cmd == 1 || cmd == 0x1b) {
        initialize(c);
        return;
    }
    struct SnxCtrUds* u = c->process->uds;
    if (!u) {
        reply(c, 1, 0, UDS_STATE);
        return;
    }
    snx_ctr_uds_poll(c->process);
    switch (cmd) {
        case 3:
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            snx_ctr_uds_close(c->process);
            reply(c, 1, 0, 0);
            return;
        case 4:
        case 0x1d: host_network(c); return;
        case 9:
        case 0x1e: connect_network(c); return;
        case 0xf: scan(c); return;
        case 0xe:
        case 0x1f: decrypt_beacon(c); return;
        case 0xb: {
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            uint8_t status[48] = {0};
            snx_st_le32(status, u->status);
            snx_st_le32(status + 4, u->reason);
            snx_st_le16(status + 8, u->own);
            snx_st_le16(status + 10, u->changed);
            for (unsigned i = 0; i < 16; i++)
                if (u->mask & (1u << i)) snx_st_le16(status + 12 + 2 * i, (uint16_t)(i + 1));
            status[44] = (uint8_t)population(u->mask);
            status[45] = u->net.bytes[29];
            snx_st_le16(status + 46, u->mask);
            reply(c, 13, 0, 0);
            snx_ctr_ipc_put_bytes(c, 2, status, 48);
            u->changed = 0;
            return;
        }
        case 0xd: {
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            unsigned id = c->in[1];
            if (id < 1 || id > 16 || !(u->mask & (1u << (id - 1)))) {
                reply(c, 1, 0, UDS_NOT_FOUND);
                return;
            }
            reply(c, 11, 0, 0);
            snx_ctr_ipc_put_bytes(c, 2, u->nodes[id - 1].bytes, 40);
            return;
        }
        case 0x12: {
            if (!snx_ctr_ipc_expect(c, 4, 0)) return;
            if (!c->in[1] || c->in[2] < 0x5f4 || c->in[2] > 1048576 || !c->in[3] || c->in[3] > 255 ||
                c->in[4] > 65535) {
                reply(c, 1, 0, UDS_BAD);
                return;
            }
            for (unsigned i = 0; i < 16; i++)
                if (u->binds[i].id == c->in[1] || (u->binds[i].id && u->binds[i].channel == c->in[3])) {
                    reply(c, 1, 0, UDS_BAD);
                    return;
                }
            for (unsigned i = 0; i < 16; i++)
                if (!u->binds[i].id) {
                    Binding* k = &u->binds[i];
                    if (!event_new(c->process, &k->event)) {
                        reply(c, 1, 0, 0xd86007f3u);
                        return;
                    }
                    k->id = c->in[1];
                    /* Stage reliable room traffic across brief JIT/frontend
                       stalls. This is host storage, not guest shared memory. */
                    k->limit = c->in[2] < UDS_QUEUE * UDS_DATA ? UDS_QUEUE * UDS_DATA : c->in[2];
                    k->channel = (uint8_t)c->in[3];
                    k->source = (uint16_t)c->in[4];
                    if (!handle_reply(c, k->event)) {
                        snx_ctr_object_release(c->process, k->event);
                        memset(k, 0, sizeof *k);
                    }
                    return;
                }
            reply(c, 1, 0, 0xd86007f3u);
            return;
        }
        case 0x13: {
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            for (unsigned i = 0; i < 16; i++)
                if (u->binds[i].id == c->in[1]) {
                    snx_ctr_object_release(c->process, u->binds[i].event);
                    memset(&u->binds[i], 0, sizeof u->binds[i]);
                    break;
                }
            reply(c, 1, 0, 0);
            return;
        }
        case 0x17: {
            if (!snx_ctr_ipc_expect(c, 6, 2)) return;
            unsigned dest = c->in[2], size = c->in[5];
            if ((u->status != 6 && u->status != 9) || u->mask != 3) {
                reply(c, 1, 0, UDS_STATE);
                return;
            }
            if ((dest != 65535 && dest != (u->own == 1 ? 2u : 1u)) || size > UDS_DATA || !c->in[3] || c->in[3] > 255 ||
                c->in[6] > 3) {
                reply(c, 1, 0, UDS_BAD);
                return;
            }
            uint8_t b[UDS_DATA + 6];
            snx_st_le16(b, u->own);
            snx_st_le16(b + 2, (uint16_t)dest);
            b[4] = (uint8_t)c->in[3];
            b[5] = (uint8_t)c->in[6];
            if (!input_buffer(c, 5, b + 6, size)) return;
            trace_packet(c->process, "send", b, size + 6);
            int sent = send_message(u, MSG_DATA, u->peer, b, size + 6);
            if (sent) ++u->sent_packets;
            reply(c, 1, 0, sent ? 0 : UDS_NETWORK);
            return;
        }
        case 0x14: {
            if (!snx_ctr_ipc_expect(c, 3, 0)) return;
            unsigned capacity = c->in[2] > 0x172 ? 0x5c8 : c->in[2] * 4;
            if (c->in[3] > 1048576) {
                reply(c, 1, 0, UDS_BAD);
                return;
            }
            for (unsigned i = 0; i < 16; i++)
                if (u->binds[i].id == c->in[1]) {
                    Binding* k = &u->binds[i];
                    Packet* q = &k->queue[k->head];
                    unsigned size = k->count ? q->size : 0;
                    if (size > c->in[3] || size > capacity) {
                        reply(c, 1, 0, UDS_BAD);
                        return;
                    }
                    uint8_t out[0x5c8] = {0};
                    if (size) memcpy(out, q->bytes, size);
                    reply(c, 3, 2, 0);
                    c->out[2] = size;
                    c->out[3] = k->count ? q->source : 0;
                    if (!output_buffer(c, out, capacity, 4)) return;
                    if (k->count) {
                        trace_packet(c->process, "deliver", q->bytes, q->size);
                        k->used -= q->size;
                        --k->count;
                        k->head = (k->head + 1) % UDS_QUEUE;
                    }
                    if (k->count) signal_event(c->process, k->event);
                    return;
                }
            reply(c, 1, 0, UDS_NOT_FOUND);
            return;
        }
        case 0x10: {
            if (!snx_ctr_ipc_expect(c, 1, 2)) return;
            unsigned size = c->in[1];
            if (size > 200 || u->status != 6) {
                reply(c, 1, 0, UDS_BAD);
                return;
            }
            if (!input_buffer(c, 4, u->net.bytes + 64, size)) return;
            u->net.bytes[63] = (uint8_t)size;
            reply(c, 1, 0, 0);
            return;
        }
        case 0x11: {
            if (!snx_ctr_ipc_expect(c, 1, 0)) return;
            unsigned size = u->net.bytes[63];
            if (c->in[1] < size) {
                reply(c, 2, 0, 0);
                return;
            }
            reply(c, 2, 2, 0);
            c->out[2] = size;
            output_buffer(c, u->net.bytes + 64, size, 3);
            return;
        }
        case 0x1a:
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            reply(c, 2, 0, 0);
            c->out[2] = (u->status == 6 || u->status == 9) ? u->net.bytes[6] : 0;
            return;
        case 0x21:
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            u->probe_oui = c->in[1];
            u->probe_data = (int8_t)c->in[2];
            reply(c, 1, 0, 0);
            return;
        case 7:
            if (!snx_ctr_ipc_expect(c, 2, 0)) return;
            if (u->status != 6) {
                reply(c, 1, 0, UDS_STATE);
                return;
            }
            if (c->in[1] > 65535 || c->in[2] > 1) {
                reply(c, 1, 0, UDS_BAD);
                return;
            }
            u->allowed_nodes = c->in[2] ? (uint16_t)c->in[1] : 0xffff;
            if (u->mask == 3 && !(u->allowed_nodes & 2)) {
                fprintf(stderr, "3ds uds trace %.3f: guest excludes peer allowed=%x\n", snx_ctr_now_ns() / 1000000.0,
                        u->allowed_nodes);
                send_message(u, MSG_CLOSE, u->peer, NULL, 0);
                u->mask = 1;
                u->changed |= 2;
                u->net.bytes[28] = 1;
                memset(&u->nodes[1], 0, sizeof u->nodes[1]);
                status_changed(c->process);
            }
            reply(c, 1, 0, 0);
            return;
        case 8:
        case 0xa:
            if (!snx_ctr_ipc_expect(c, 0, 0)) return;
            if ((cmd == 8 && u->status != 6) || (cmd == 0xa && u->status == 6)) {
                reply(c, 1, 0, UDS_STATE);
                return;
            }
            if (u->status == 6 || u->status == 9)
                send_message(u, u->status == 6 ? MSG_CLOSE : MSG_LEAVE, u->peer, NULL, 0);
            fprintf(stderr, "3ds uds trace %.3f: guest requests %s status=%u mask=%x sent=%u received=%u\n",
                    snx_ctr_now_ns() / 1000000.0, cmd == 8 ? "destroy network" : "disconnect", u->status, u->mask,
                    u->sent_packets, u->received_packets);
            finish_join(c->process, UDS_NETWORK);
            u->changed = u->mask;
            u->mask = 0;
            u->status = 3;
            u->reason = 4;
            clear_bindings(c->process);
            status_changed(c->process);
            reply(c, 1, 0, 0);
            return;
        default:
            fprintf(stderr, "3ds uds: unsupported command 0x%04x (prototype does not fake success)\n", cmd);
            c->state = SNX_CTR_IPC_UNSUPPORTED;
            return;
    }
}
