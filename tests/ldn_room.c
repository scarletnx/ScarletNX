/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* An Eden room (common/network/net_room.c) against a room of this test's own: an ENet server that reads and relays the
   messages as Eden's src/network/room.cpp does, from its own reading of the bytes, so a field the member puts in the
   wrong place shows here. The messages byte for byte; joining and each refusal; ldn's networks through the room (scan,
   join, sync, leave, a station that leaves the room without a word, the host destroying its network); the game's
   datagram sockets (to a member, broadcasts, Eden's protocol byte, poll, timeouts, truncation, a big datagram); and the
   room closing */
#include "common/network/net_ldn.h"
#include "common/utilities/endian.h"
#include "common/network/net_room.h"
#include "common/network/net_internal.h"
#include "fake_room.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "ldn_room.c:%d: check failed: %s\n", __LINE__, #x);                                        \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
static void sleep_ms(int ms) { snx_net_poll(NULL, 0, ms); }
/* the log, which players attach to public reports: each line is read for the members' nicknames and the chat's words
   as it comes, and kept since heard_reset for what it says of them instead. Every member's room thread writes it */
static const char* const g_named_words[] = {"Aogo", "Xuji", "Afugo", "Ada", "bakery"};
static struct {
    SnxLock lk;
    char lines[4096]; /* since heard_reset */
    char named[512];  /* the first line that named a member */
} g_heard;
static void heard(const char* m) {
    size_t n, i;
    snx_lock(&g_heard.lk);
    for (i = 0; i < sizeof g_named_words / sizeof g_named_words[0]; i++)
        if (strstr(m, g_named_words[i]) && !g_heard.named[0]) snprintf(g_heard.named, sizeof g_heard.named, "%s", m);
    n = strlen(g_heard.lines);
    snprintf(g_heard.lines + n, sizeof g_heard.lines - n, "%s", m);
    snx_unlock(&g_heard.lk);
}
static void heard_reset(void) {
    snx_lock(&g_heard.lk);
    g_heard.lines[0] = 0;
    snx_unlock(&g_heard.lk);
}
/* whether a line since heard_reset has text, waiting up to ms for it: a member's room thread logs it */
static int heard_of(const char* text, int ms) {
    int i, has = 0;
    for (i = 0; i <= ms && !has; i++) {
        snx_lock(&g_heard.lk);
        has = strstr(g_heard.lines, text) != NULL;
        snx_unlock(&g_heard.lk);
        if (!has) sleep_ms(1);
    }
    return has;
}
static void unnamed(void) {
    char named[sizeof g_heard.named];
    snx_lock(&g_heard.lk);
    memcpy(named, g_heard.named, sizeof named);
    snx_unlock(&g_heard.lk);
    if (named[0]) fprintf(stderr, "ldn_room: a member's nickname or chat in the log: %s", named);
    CHECK(!named[0]);
}
static SnxLock g_warned_lk;
static char g_warned[512]; /* the last warning, which the player sees: the room's thread writes it */
static void warned(const char* m) {
    snx_lock(&g_warned_lk);
    snprintf(g_warned, sizeof g_warned, "%s", m);
    snx_unlock(&g_warned_lk);
}
static int warned_of(const char* word) {
    int has;
    snx_lock(&g_warned_lk);
    has = strstr(g_warned, word) != NULL;
    snx_unlock(&g_warned_lk);
    return has;
}

/* ============================================================ the room */
static FakeRoom* g_room;
static int join(SnxRoom** out, const char* nickname, const char* password, int* why) {
    return snx_room_join(out, "127.0.0.1", fake_room_port(g_room), nickname, password, "game", 0x0100ABCD12345000ull,
                         "", why);
}

/* ============================================================ the messages */
static void messages(void) {
    static const uint8_t join_bytes[] = {1, 0, 0, 0, 4, 'A', 'o', 'g', 'o', 255, 255, 255, 255, 0,
                                         0, 0, 1, 0, 0, 0,   2,   'p', 'w', 0,   0,   0,   0};
    static const uint8_t ldn_bytes[] = {6, 3, 192, 168, 1, 1, 192, 168, 1, 2, 0, 0, 0, 0, 3, 'n', 'e', 't'};
    static const uint8_t datagram_bytes[] = {5,   1,    192,  168, 1, 2, 0x30, 0x39, 1, 192, 168, 1,
                                             255, 0x04, 0xD2, 4,   1, 0, 0,    0,    2, 'h', 'i'};
    uint8_t out[64];
    SnxRoomLdn p, q;
    SnxRoomDatagram d, e;
    CHECK(snx_room_pack_join(out, sizeof out, "Aogo", "pw") == sizeof join_bytes);
    CHECK(!memcmp(out, join_bytes, sizeof join_bytes));
    CHECK(snx_room_pack_join(out, 8, "Aogo", "pw") == 0);
    memset(&p, 0, sizeof p);
    p.type = SNX_ROOM_LDN_SYNC_NETWORK;
    memcpy(p.local_ip, ldn_bytes + 2, 4);
    memcpy(p.remote_ip, ldn_bytes + 6, 4);
    p.data = (const uint8_t*)"net";
    p.size = 3;
    CHECK(snx_room_pack_ldn(out, sizeof out, &p) == sizeof ldn_bytes && !memcmp(out, ldn_bytes, sizeof ldn_bytes));
    CHECK(snx_room_parse_ldn(ldn_bytes, sizeof ldn_bytes, &q) == 1);
    CHECK(q.type == 3 && !memcmp(q.local_ip, p.local_ip, 4) && !memcmp(q.remote_ip, p.remote_ip, 4) && !q.broadcast);
    CHECK(q.size == 3 && !memcmp(q.data, "net", 3));
    CHECK(snx_room_parse_ldn(ldn_bytes, sizeof ldn_bytes - 1, &q) == 0); /* cut short */
    memcpy(out, ldn_bytes, sizeof ldn_bytes);
    out[sizeof ldn_bytes] = 0;
    CHECK(snx_room_parse_ldn(out, sizeof ldn_bytes + 1, &q) == 0); /* a byte after it */
    memset(&d, 0, sizeof d);
    memcpy(d.local_ip, datagram_bytes + 2, 4);
    d.local_port = 12345;
    memcpy(d.remote_ip, datagram_bytes + 9, 4);
    d.remote_port = 1234;
    d.broadcast = 1;
    d.data = (const uint8_t*)"hi";
    d.size = 2;
    CHECK(snx_room_pack_datagram(out, sizeof out, &d) == sizeof datagram_bytes);
    CHECK(!memcmp(out, datagram_bytes, sizeof datagram_bytes));
    CHECK(snx_room_parse_datagram(datagram_bytes, sizeof datagram_bytes, &e) == 1);
    CHECK(e.local_port == 12345 && e.remote_port == 1234 && e.broadcast && e.size == 2 && !memcmp(e.data, "hi", 2));
}

/* ============================================================ joining */
static void joining(void) {
    SnxRoom *a, *b, *c;
    uint8_t ip[4];
    int why, r;
    char said[128];
    heard_reset();
    r = join(&a, "Aogo", "secret", &why);
    CHECK(r == 0 && why == SNX_ROOM_IN && snx_room_state(a) == SNX_ROOM_IN);
    snprintf(said, sizeof said, "room: joined 127.0.0.1:%u, at 192.168.1.1, 1 member\n", fake_room_port(g_room));
    CHECK(heard_of(said, 0));
    snx_room_fake_ip(a, ip);
    CHECK(ip[0] == 192 && ip[1] == 168 && ip[2] == 1 && ip[3] == 1 && snx_room_has_member(a, ip));
    {
        char game[64];
        uint64_t id;
        fake_room_last_game(g_room, game, sizeof game, &id);
        CHECK(!strcmp(game, "game") && id == 0x0100ABCD12345000ull);
    }
    /* each refusal, as the room says it */
    CHECK(join(&b, "Xuji", "wrong", &why) == -SNX_ECONNREFUSED && why == SNX_ROOM_WRONG_PASSWORD && !b);
    CHECK(join(&b, "Aogo", "secret", &why) == -SNX_ECONNREFUSED && why == SNX_ROOM_NAME_COLLISION);
    CHECK(join(&b, "Ada", "secret", &why) == -SNX_ECONNREFUSED && why == SNX_ROOM_NAME_COLLISION);
    CHECK(snx_room_why_text(SNX_ROOM_NAME_COLLISION)[0] &&
          strstr(snx_room_why_text(SNX_ROOM_NAME_COLLISION), "4 to 20"));
    heard_reset();
    r = join(&b, "Xuji", "secret", &why);
    CHECK(r == 0);
    snx_room_fake_ip(b, ip);
    CHECK(ip[3] == 2);
    /* the members each sees: the other one, by the room's information */
    for (r = 0; r < 200 && !snx_room_has_member(a, ip); r++) sleep_ms(5);
    CHECK(snx_room_has_member(a, ip));
    /* the room says who joins before its information has them: the log says a member did, not who */
    CHECK(heard_of("room: a member joined\n", 2000));
    /* chat in the log by size and the sender's address in the room: neither the text nor the nickname */
    heard_reset();
    fake_room_chat(g_room, "Aogo", "meet at the bakery");
    CHECK(heard_of("room: a chat message of 18 bytes from the member at 192.168.1.1\n", 2000));
    /* nobody at that port: given up after the connect's time out */
    CHECK(snx_room_join(&c, "127.0.0.1", 9, "Afugo", "secret", "game", 1, "", &why) == -SNX_ETIMEDOUT);
    CHECK(why == SNX_ROOM_UNREACHABLE);
    /* a name with no address is said so, not as a room that didn't answer */
    CHECK(snx_room_join(&c, "no-such-room.invalid", 24872, "Afugo", "secret", "game", 1, "", &why) ==
          -SNX_EHOSTUNREACH);
    CHECK(why == SNX_ROOM_NO_ADDRESS && strstr(snx_room_why_text(why), "no address"));
    /* kicked by the room's host: out of the room, and the player is told */
    warned("");
    fake_room_kick(g_room, "Xuji");
    /* both: went_away warns after it sets the state */
    for (r = 0; r < 400 && !(snx_room_state(b) == SNX_ROOM_KICKED && warned_of("kicked")); r++) sleep_ms(5);
    CHECK(snx_room_state(b) == SNX_ROOM_KICKED && warned_of("kicked"));
    CHECK(heard_of("room: the member at 192.168.1.2 was kicked\n", 2000)); /* still in a's members as it hears it */
    CHECK(snx_room_send_ldn(b, SNX_ROOM_LDN_SCAN, NULL, 1, NULL, 0) == -SNX_ENETDOWN);
    snx_room_leave(b);
    snx_room_fake_ip(a, ip);
    ip[3] = 2;
    for (r = 0; r < 200 && snx_room_has_member(a, ip); r++) sleep_ms(5);
    CHECK(!snx_room_has_member(a, ip)); /* the room told the others at once */
    snx_room_leave(a);
    unnamed();
}

/* ============================================================ ldn through the room */
static int changes_a, changes_b;
static void changed_a(void* arg) {
    (void)arg;
    __atomic_add_fetch(&changes_a, 1, __ATOMIC_SEQ_CST);
}
static void changed_b(void* arg) {
    (void)arg;
    __atomic_add_fetch(&changes_b, 1, __ATOMIC_SEQ_CST);
}
static int wait_for(int* counter, int at_least, int ms) {
    int i;
    for (i = 0; i < ms && __atomic_load_n(counter, __ATOMIC_SEQ_CST) < at_least; i++) sleep_ms(1);
    return __atomic_load_n(counter, __ATOMIC_SEQ_CST) >= at_least;
}
/* what a change made, waited for itself: a change counted may be a sync from before it, still on its way */
static int advertises(SnxLdn* l, const char* data, int ms) {
    SnxLdnNetworkInfo info;
    int i;
    for (i = 0; i < ms; i++) {
        if (snx_ldn_network_info(l, &info, NULL) == 1 && !memcmp(info.advertise_data, data, strlen(data))) return 1;
        sleep_ms(1);
    }
    return 0;
}
static int disconnects(SnxLdn* l, int ms) {
    int i;
    for (i = 0; i < ms && snx_ldn_connected(l); i++) sleep_ms(1);
    return !snx_ldn_connected(l);
}
/* why the member went away, or SNX_ROOM_IN after ms: each member's own thread hears the room, so one being out says
   nothing of another */
static int out_of_room(SnxRoom* r, int ms) {
    int i;
    for (i = 0; i < ms && snx_room_state(r) == SNX_ROOM_IN; i++) sleep_ms(1);
    return snx_room_state(r);
}
static void name_of(uint8_t out[SNX_LDN_USER_NAME], const char* name) {
    memset(out, 0, SNX_LDN_USER_NAME);
    strcpy((char*)out, name);
}
static void network_of(SnxLdn* host, SnxLdnNetworkInfo* n) {
    uint8_t ip[4], mask[4], name[SNX_LDN_USER_NAME];
    int i;
    memset(n, 0, sizeof *n);
    n->local_communication_id = 0x0100ABCD12345000ull;
    n->session_id[0] = 0x1122334455667788ull;
    snx_ldn_address(host, ip, mask);
    snx_ldn_fake_mac(ip, n->bssid);
    n->ssid_length = 32;
    memset(n->ssid, 'a', 32);
    n->node_count_max = 8;
    n->node_count = 1;
    for (i = 0; i < SNX_LDN_NODES; i++) n->nodes[i].node_id = (int8_t)i;
    name_of(name, "Aogo");
    snx_ldn_node_init(host, &n->nodes[0], name, 4);
}
static void networks(void) {
    SnxRoom *ra, *rb;
    SnxLdn *a, *b;
    SnxLdnNetworkInfo net, found[4], info;
    uint8_t name[SNX_LDN_USER_NAME], updates[SNX_LDN_NODES], ip[4], mask[4];
    int why;
    CHECK(join(&ra, "Aogo", "secret", &why) == 0 && join(&rb, "Xuji", "secret", &why) == 0);
    CHECK(snx_ldn_open_room(&a, ra, changed_a, NULL) == 0 && snx_ldn_open_room(&b, rb, changed_b, NULL) == 0);
    {
        SnxLdn* again; /* a console has one ldn in a room; the first keeps working (below) */
        CHECK(snx_ldn_open_room(&again, ra, NULL, NULL) == -SNX_EBUSY);
    }
    snx_ldn_address(b, ip, mask);
    CHECK(ip[0] == 192 && ip[3] == 2 && mask[0] == 255 && mask[2] == 255 && mask[3] == 0);
    CHECK(snx_ldn_scan(b, NULL, found, 4, 200) == 0); /* nothing yet */
    network_of(a, &net);
    CHECK(snx_ldn_set_advertise_data(a, "hello", 5) == 0 && snx_ldn_create_network(a, &net) == 0);
    /* the station finds it and joins: node 1 at the room's address. A scan waits its whole time: through a room
       another host may answer later than the first */
    {
        int64_t t0 = snx_net_now_us();
        CHECK(snx_ldn_scan(b, NULL, found, 4, 400) == 1);
        CHECK(snx_net_now_us() - t0 >= 380000);
    }
    CHECK(snx_ldn_scan(b, NULL, found, 4, 1000) == 1);
    CHECK(!memcmp(found[0].bssid, net.bssid, 6) && snx_le16(found[0].advertise_data_size) == 5);
    CHECK(snx_ldn_scan(a, NULL, found, 4, 200) == 0); /* the host doesn't find itself */
    name_of(name, "Xuji");
    changes_a = changes_b = 0;
    heard_reset();
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    CHECK(wait_for(&changes_a, 1, 2000));
    CHECK(heard_of("ldn: node 1 joined from 192.168.1.2, 2 of 8 nodes\n", 2000));
    CHECK(heard_of("ldn: joined the network of the host at 192.168.1.1, 2 of 8 nodes\n", 2000));
    CHECK(snx_ldn_network_info(a, &info, updates) == 1 && info.node_count == 2 && updates[1] == SNX_LDN_NODE_CONNECT);
    CHECK(!strcmp((char*)info.nodes[1].user_name, "Xuji") && info.nodes[1].ipv4[0] == 2 &&
          info.nodes[1].ipv4[3] == 192);
    CHECK(snx_ldn_network_info(b, &info, NULL) == 1 && info.node_count == 2);
    /* the host's changes reach the station */
    changes_b = 0;
    CHECK(snx_ldn_set_advertise_data(a, "again", 5) == 0);
    CHECK(wait_for(&changes_b, 1, 2000) && advertises(b, "again", 2000));
    /* the station leaves */
    changes_a = 0;
    heard_reset();
    snx_ldn_disconnect(b);
    {
        size_t node = 0;
        int i;
        for (i = 0; i < 200 && !fake_room_ldn_count(g_room, SNX_ROOM_LDN_DISCONNECT, &node); i++) sleep_ms(5);
        CHECK(node == sizeof(SnxLdnNodeInfo)); /* Eden's host finds the station by the MAC in it */
    }
    CHECK(!snx_ldn_connected(b) && snx_ldn_disconnect_reason(b) == SNX_LDN_REASON_DISCONNECTED_BY_USER);
    CHECK(wait_for(&changes_a, 1, 2000));
    CHECK(snx_ldn_network_info(a, &info, updates) == 1 && info.node_count == 1 &&
          updates[1] == SNX_LDN_NODE_DISCONNECT);
    CHECK(heard_of("ldn: node 1 left\n", 2000));
    /* joins again, and the host destroys the network: the station is told */
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    changes_b = 0;
    snx_ldn_destroy_network(a);
    CHECK(wait_for(&changes_b, 1, 2000) && disconnects(b, 2000));
    CHECK(snx_ldn_disconnect_reason(b) == SNX_LDN_REASON_DESTROYED_BY_USER);
    /* no network: the connect waits for a sync that never comes */
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 300) == -SNX_ETIMEDOUT);
    /* the station's console leaves the room without a word: the host takes its node out */
    CHECK(snx_ldn_create_network(a, &net) == 0);
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    CHECK(wait_for(&changes_a, 1, 2000));
    changes_a = 0;
    snx_room_leave(rb);
    CHECK(wait_for(&changes_a, 1, 3000));
    CHECK(snx_ldn_network_info(a, &info, NULL) == 1 && info.node_count == 1 && !info.nodes[1].is_connected);
    {
        int64_t t0 = snx_net_now_us(); /* out of the room: no scan, and no wait for one */
        CHECK(snx_ldn_scan(b, NULL, found, 4, 1000) == 0 && snx_net_now_us() - t0 < 200000);
    }
    snx_ldn_close(b); /* its goodbyes can't go: out of the room */
    snx_ldn_close(a);
    snx_room_leave(ra);
    unnamed();
}

/* ============================================================ datagrams */
static SnxSockAddr to_of(const uint8_t ip[4], uint16_t port) {
    SnxSockAddr a;
    memset(&a, 0, sizeof a);
    a.family = SNX_AF_INET;
    memcpy(a.addr, ip, 4);
    snx_st_be16(&a.port, port);
    return a;
}
static int64_t recv_wait(SnxSocket* s, void* buf, size_t size, SnxSockAddr* from, int* flags) {
    SnxTimeval tv = {(int64_t)snx_le64(2), 0}; /* option values are the guest's (net.h) */
    snx_socket_setsockopt(s, SNX_SOL_SOCKET, SNX_SO_RCVTIMEO, &tv, sizeof tv);
    return snx_socket_recv(s, buf, size, 0, from, flags);
}
static void datagrams(void) {
    static const uint8_t any[4] = {0, 0, 0, 0}, bcast[4] = {192, 168, 1, 255};
    SnxRoom *ra, *rb;
    SnxSocket *a, *a2, *a17, *b, *b17, *c;
    SnxSockAddr addr, from;
    SnxPollEntry e;
    SnxTimeval tv = {0, (int64_t)snx_le64(100000)};
    uint8_t ip_a[4], ip_b[4], buf[64], *big;
    int why, one = (int)snx_le32(1), flags, value;
    size_t size;
    int64_t n;
    CHECK(join(&ra, "Aogo", "secret", &why) == 0 && join(&rb, "Xuji", "secret", &why) == 0);
    snx_room_fake_ip(ra, ip_a);
    snx_room_fake_ip(rb, ip_b);
    CHECK(snx_room_socket_open(ra, &a, SNX_AF_INET, SNX_SOCK_STREAM, 0) == -SNX_EPROTONOSUPPORT);
    CHECK(snx_room_socket_open(ra, &a, SNX_AF_INET, SNX_SOCK_DGRAM, 0) == 0 && snx_room_socket(a));
    CHECK(snx_room_socket_open(ra, &a17, SNX_AF_INET, SNX_SOCK_DGRAM, SNX_IPPROTO_UDP) == 0);
    CHECK(snx_room_socket_open(rb, &b, SNX_AF_INET, SNX_SOCK_DGRAM, 0) == 0);
    CHECK(snx_room_socket_open(rb, &b17, SNX_AF_INET, SNX_SOCK_DGRAM, SNX_IPPROTO_UDP) == 0);
    addr = to_of(any, 1234);
    CHECK(snx_socket_bind(a, &addr) == 0);
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_bind(a17, &addr) == -SNX_EADDRINUSE); /* the port is a's */
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_bind(b, &addr) == -SNX_EADDRNOTAVAIL); /* another console's address */
    addr = to_of(ip_a, 1235);
    CHECK(snx_socket_bind(a17, &addr) == 0);
    CHECK(snx_room_socket_open(ra, &a2, SNX_AF_INET, SNX_SOCK_DGRAM, 0) == 0);
    addr = to_of(any, 1236);
    CHECK(snx_socket_bind(a2, &addr) == 0);
    CHECK(snx_socket_name(a, &addr) == 0 && snx_ld_be16(&addr.port) == 1234 && !memcmp(addr.addr, any, 4));
    /* to a member: from the sender's address and the port its first send took; poll says so first */
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_send(b, "hello", 5, 0, &addr) == 5);
    e.socket = a;
    e.host_fd = -1;
    e.events = SNX_POLLIN;
    CHECK(snx_net_poll(&e, 1, 2000) == 1 && (e.revents & SNX_POLLIN));
    CHECK(snx_socket_readable_bytes(a, &value) == 0 && value == 5);
    n = recv_wait(a, buf, sizeof buf, &from, &flags);
    CHECK(n == 5 && !memcmp(buf, "hello", 5) && !memcmp(from.addr, ip_b, 4) && from.port && !flags);
    CHECK(snx_socket_recv(a2, buf, sizeof buf, SNX_MSG_DONTWAIT, NULL, NULL) == -SNX_EAGAIN); /* port 1236's */
    CHECK(snx_socket_name(b, &addr) == 0 && addr.port == from.port);
    /* the answer goes back to it */
    CHECK(snx_socket_send(a, "back", 4, 0, &from) == 4);
    n = recv_wait(b, buf, sizeof buf, &from, NULL);
    CHECK(n == 4 && !memcmp(buf, "back", 4) && !memcmp(from.addr, ip_a, 4));
    /* Eden's protocol byte: protocol 0 is IP (1), UDP is UDP (4), and each hears only its own */
    CHECK(fake_room_last_datagram(g_room, buf, sizeof buf) > 16 && buf[15] == 1);
    addr = to_of(ip_a, 1235);
    CHECK(snx_socket_send(b17, "udp", 3, 0, &addr) == 3);
    n = recv_wait(a17, buf, sizeof buf, NULL, NULL);
    CHECK(n == 3 && !memcmp(buf, "udp", 3));
    CHECK(fake_room_last_datagram(g_room, buf, sizeof buf) > 16 && buf[15] == 4);
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_send(b17, "lost", 4, 0, &addr) == 4); /* to a's port, but a protocol-0 socket */
    snx_socket_setsockopt(a, SNX_SOL_SOCKET, SNX_SO_RCVTIMEO, &tv, sizeof tv);
    CHECK(snx_socket_recv(a, buf, sizeof buf, 0, NULL, NULL) == -SNX_EAGAIN); /* the time out: nothing came */
    /* a broadcast needs SO_BROADCAST to send, and to be heard */
    addr = to_of(bcast, 1234);
    CHECK(snx_socket_send(b, "all", 3, 0, &addr) == -SNX_EACCES);
    CHECK(snx_socket_setsockopt(b, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &one, sizeof one) == 0);
    CHECK(snx_socket_send(b, "all", 3, 0, &addr) == 3);
    CHECK(snx_socket_recv(a, buf, sizeof buf, 0, NULL, NULL) == -SNX_EAGAIN); /* a hasn't SO_BROADCAST */
    CHECK(snx_socket_setsockopt(a, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &one, sizeof one) == 0);
    size = sizeof value;
    CHECK(snx_socket_getsockopt(a, SNX_SOL_SOCKET, SNX_SO_BROADCAST, &value, &size) == 0 &&
          snx_le32((uint32_t)value) == 1);
    CHECK(snx_socket_send(b, "all", 3, 0, &addr) == 3);
    n = recv_wait(a, buf, sizeof buf, &from, NULL);
    CHECK(n == 3 && !memcmp(buf, "all", 3) && !memcmp(from.addr, ip_b, 4));
    /* a datagram bigger than the buffer: its start, with MSG_TRUNC; MSG_PEEK leaves it */
    addr = to_of(ip_a, 1234);
    CHECK(snx_socket_send(b, "0123456789", 10, 0, &addr) == 10);
    e.revents = 0;
    CHECK(snx_net_poll(&e, 1, 2000) == 1);
    CHECK(snx_socket_recv(a, buf, 4, SNX_MSG_PEEK, NULL, &flags) == 4 && (flags & SNX_MSG_TRUNC));
    CHECK(snx_socket_recv(a, buf, 4, 0, NULL, &flags) == 4 && !memcmp(buf, "0123", 4) && (flags & SNX_MSG_TRUNC));
    CHECK(snx_socket_recv(a, buf, sizeof buf, SNX_MSG_DONTWAIT, NULL, NULL) == -SNX_EAGAIN); /* the rest is gone */
    /* a datagram as big as UDP takes, through zstd, whole */
    big = malloc(SNX_ROOM_DATAGRAM_MAX + 1);
    CHECK(big != NULL);
    for (size = 0; size < SNX_ROOM_DATAGRAM_MAX; size++) big[size] = (uint8_t)(size * 7 + (size >> 9));
    CHECK(snx_socket_send(b, big, SNX_ROOM_DATAGRAM_MAX + 1, 0, &addr) == -SNX_EMSGSIZE);
    CHECK(snx_socket_send(b, big, SNX_ROOM_DATAGRAM_MAX, 0, &addr) == SNX_ROOM_DATAGRAM_MAX);
    {
        uint8_t* back = malloc(SNX_ROOM_DATAGRAM_MAX);
        CHECK(back != NULL);
        n = recv_wait(a, back, SNX_ROOM_DATAGRAM_MAX, NULL, NULL);
        CHECK(n == SNX_ROOM_DATAGRAM_MAX && !memcmp(back, big, SNX_ROOM_DATAGRAM_MAX));
        free(back);
    }
    free(big);
    /* a connected socket sends without an address; one with none to send to is told */
    CHECK(snx_room_socket_open(rb, &c, SNX_AF_INET, SNX_SOCK_DGRAM, 0) == 0);
    CHECK(snx_socket_send(c, "x", 1, 0, NULL) == -SNX_EDESTADDRREQ);
    CHECK(snx_socket_connect(c, &addr) == 0 && snx_socket_send(c, "conn", 4, 0, NULL) == 4);
    n = recv_wait(a, buf, sizeof buf, NULL, NULL);
    CHECK(n == 4 && !memcmp(buf, "conn", 4));
    /* UDS uses low-latency rooms. A stalled consumer must retain a reliable
       burst larger than the old 256-datagram socket queue, in FIFO order.
       The fence uses another socket on the same ordered ENet connection, so
       all burst packets have reached the room socket before any are read. */
    snx_room_low_latency(ra);
    {
        uint8_t burst[1480], back[1480];
        memset(burst, 0x5a, sizeof burst);
        addr = to_of(ip_a, 1234);
        for (uint32_t i = 0; i < 512; ++i) {
            uint32_t sequence = snx_le32(i);
            memcpy(burst, &sequence, sizeof sequence);
            CHECK(snx_socket_send(b, burst, sizeof burst, 0, &addr) == sizeof burst);
        }
        addr = to_of(ip_a, 1236);
        CHECK(snx_socket_send(b, "fence", 5, 0, &addr) == 5);
        CHECK(recv_wait(a2, buf, sizeof buf, NULL, NULL) == 5 && !memcmp(buf, "fence", 5));
        for (uint32_t i = 0; i < 512; ++i) {
            uint32_t sequence;
            CHECK(recv_wait(a, back, sizeof back, NULL, NULL) == sizeof back);
            memcpy(&sequence, back, sizeof sequence);
            CHECK(snx_le32(sequence) == i && back[sizeof back - 1] == 0x5a);
        }
        CHECK(snx_socket_recv(a, buf, sizeof buf, SNX_MSG_DONTWAIT, NULL, NULL) == -SNX_EAGAIN);
        /* The byte cap still bounds large datagrams independently of count.
           64 maximum UDP datagrams fit in 4 MiB; the 65th must be dropped. */
        big = malloc(SNX_ROOM_DATAGRAM_MAX);
        uint8_t* large_back = malloc(SNX_ROOM_DATAGRAM_MAX);
        CHECK(big && large_back);
        memset(big, 0x5a, SNX_ROOM_DATAGRAM_MAX);
        addr = to_of(ip_a, 1234);
        for (uint32_t i = 0; i < 65; ++i) {
            uint32_t sequence = snx_le32(i);
            memcpy(big, &sequence, sizeof sequence);
            CHECK(snx_socket_send(b, big, SNX_ROOM_DATAGRAM_MAX, 0, &addr) == SNX_ROOM_DATAGRAM_MAX);
        }
        addr = to_of(ip_a, 1236);
        CHECK(snx_socket_send(b, "fence", 5, 0, &addr) == 5);
        CHECK(recv_wait(a2, buf, sizeof buf, NULL, NULL) == 5);
        for (uint32_t i = 0; i < 64; ++i) {
            uint32_t sequence;
            CHECK(recv_wait(a, large_back, SNX_ROOM_DATAGRAM_MAX, NULL, NULL) == SNX_ROOM_DATAGRAM_MAX);
            memcpy(&sequence, large_back, sizeof sequence);
            CHECK(snx_le32(sequence) == i);
        }
        CHECK(snx_socket_recv(a, buf, sizeof buf, SNX_MSG_DONTWAIT, NULL, NULL) == -SNX_EAGAIN);
        free(large_back);
        free(big);
    }
    /* out of the room: a send says so; the sockets still close after */
    snx_room_leave(rb);
    CHECK(snx_socket_send(c, "late", 4, 0, NULL) == -SNX_ENETDOWN);
    snx_socket_unref(c);
    snx_socket_unref(b17);
    snx_socket_unref(b);
    snx_socket_unref(a17);
    snx_socket_unref(a2);
    snx_socket_unref(a);
    snx_room_leave(ra);
}

/* ============================================================ the goodbyes */
/* ldn closed and the room left at once, as the game's exit does them: the room still gets the host's DestroyNetwork
   and the station's Disconnect, which an Eden on the other side needs (its ldn doesn't watch the members) */
static void goodbyes(void) {
    SnxRoom *ra, *rb;
    SnxLdn *a, *b;
    SnxLdnNetworkInfo net, found[2];
    uint8_t name[SNX_LDN_USER_NAME];
    int why, destroys, disconnects, i;
    CHECK(join(&ra, "Aogo", "secret", &why) == 0 && join(&rb, "Xuji", "secret", &why) == 0);
    CHECK(snx_ldn_open_room(&a, ra, changed_a, NULL) == 0 && snx_ldn_open_room(&b, rb, changed_b, NULL) == 0);
    network_of(a, &net);
    CHECK(snx_ldn_create_network(a, &net) == 0 && snx_ldn_scan(b, NULL, found, 2, 1000) == 1);
    name_of(name, "Xuji");
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    destroys = fake_room_ldn_count(g_room, SNX_ROOM_LDN_DESTROY_NETWORK, NULL);
    snx_ldn_close(a);
    snx_room_leave(ra);
    for (i = 0; i < 600 && fake_room_ldn_count(g_room, SNX_ROOM_LDN_DESTROY_NETWORK, NULL) == destroys; i++)
        sleep_ms(5);
    CHECK(fake_room_ldn_count(g_room, SNX_ROOM_LDN_DESTROY_NETWORK, NULL) == destroys + 1);
    /* the other way round: the station's console goes */
    CHECK(join(&ra, "Aogo", "secret", &why) == 0 && snx_ldn_open_room(&a, ra, changed_a, NULL) == 0);
    network_of(a, &net);
    CHECK(snx_ldn_create_network(a, &net) == 0 && snx_ldn_scan(b, NULL, found, 2, 1000) == 1);
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    disconnects = fake_room_ldn_count(g_room, SNX_ROOM_LDN_DISCONNECT, NULL);
    snx_ldn_close(b);
    snx_room_leave(rb);
    for (i = 0; i < 600 && fake_room_ldn_count(g_room, SNX_ROOM_LDN_DISCONNECT, NULL) == disconnects; i++) sleep_ms(5);
    CHECK(fake_room_ldn_count(g_room, SNX_ROOM_LDN_DISCONNECT, NULL) == disconnects + 1);
    snx_ldn_close(a);
    snx_room_leave(ra);
}

/* ============================================================ the handler */
/* a handler that takes its time on the members' change: the one set after it is set only once it has returned */
static int g_slow_in, g_slow_done;
static void slow(void* arg, const SnxRoomLdn* p) {
    (void)arg;
    if (p) return;
    __atomic_store_n(&g_slow_in, 1, __ATOMIC_SEQ_CST);
    sleep_ms(200);
    __atomic_store_n(&g_slow_done, 1, __ATOMIC_SEQ_CST);
}
static void handler(void) {
    SnxRoom *ra, *rb;
    int why, i;
    CHECK(join(&ra, "Aogo", "secret", &why) == 0);
    CHECK(snx_room_set_ldn(ra, slow, &g_slow_in) == 0);
    CHECK(snx_room_set_ldn(ra, slow, &g_slow_done) == -SNX_EBUSY); /* one handler per member */
    CHECK(join(&rb, "Xuji", "secret", &why) == 0);                 /* the members change: slow runs on ra's thread */
    for (i = 0; i < 400 && !__atomic_load_n(&g_slow_in, __ATOMIC_SEQ_CST); i++) sleep_ms(5);
    CHECK(__atomic_load_n(&g_slow_in, __ATOMIC_SEQ_CST));
    snx_room_clear_ldn(ra, &g_slow_done); /* another's: stays */
    snx_room_clear_ldn(ra, &g_slow_in);
    CHECK(__atomic_load_n(&g_slow_done, __ATOMIC_SEQ_CST)); /* what slow's arg is may go now */
    CHECK(snx_room_set_ldn(ra, slow, &g_slow_done) == 0);   /* and the member takes another */
    snx_room_clear_ldn(ra, &g_slow_done);
    snx_room_leave(rb);
    snx_room_leave(ra);
}

/* ============================================================ the room closing */
static void closing(void) {
    SnxRoom *ra, *rb;
    SnxLdn *a, *b;
    SnxLdnNetworkInfo net, found[2];
    uint8_t name[SNX_LDN_USER_NAME];
    int why;
    CHECK(join(&ra, "Aogo", "secret", &why) == 0 && join(&rb, "Xuji", "secret", &why) == 0);
    CHECK(snx_ldn_open_room(&a, ra, changed_a, NULL) == 0 && snx_ldn_open_room(&b, rb, changed_b, NULL) == 0);
    network_of(a, &net);
    CHECK(snx_ldn_create_network(a, &net) == 0 && snx_ldn_scan(b, NULL, found, 2, 1000) == 1);
    name_of(name, "Xuji");
    CHECK(snx_ldn_connect(b, &found[0], name, 4, 2000) == 0);
    changes_b = 0;
    fake_room_close(g_room);
    CHECK(out_of_room(rb, 1000) == SNX_ROOM_CLOSED);
    CHECK(out_of_room(ra, 1000) == SNX_ROOM_CLOSED);
    CHECK(wait_for(&changes_b, 1, 3000));
    CHECK(disconnects(b, 2000) && snx_ldn_disconnect_reason(b) == SNX_LDN_REASON_DESTROYED_BY_USER);
    snx_ldn_close(b);
    snx_ldn_close(a);
    snx_room_leave(rb);
    snx_room_leave(ra);
}

int main(void) {
    CHECK(snx_net_init() == 0);
    snx_lock_init(&g_warned_lk);
    snx_lock_init(&g_heard.lk);
    snx_net_set_log(heard);
    snx_net_set_warn(warned);
    messages();
    g_room = fake_room_start();
    joining();
    networks();
    datagrams();
    goodbyes();
    handler();
    closing();
    fake_room_stop(g_room);
    unnamed(); /* the datagrams, the goodbyes and the room closing too */
    printf("ldn_room: %u checks passed\n", checks);
    return 0;
}
