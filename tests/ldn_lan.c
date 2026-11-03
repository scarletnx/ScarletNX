/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Local wireless over the LAN (common/network/net_ldn): ldn_mitm's frames and their run-length coding, and two
   consoles on the host's loopback (127.0.0.1 and 127.0.0.2) finding and joining each other: the scan with its
   filter, the host's nodes, the station's sync, leaving and the host going away */
#include "common/network/net_ldn.h"
#include "common/network/net_internal.h"
#include "common/utilities/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "ldn_lan.c:%d: check failed: %s\n", __LINE__, #x);                                         \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)
/* a poll with nothing to poll sleeps */
static void sleep_ms(int ms) { snx_net_poll(NULL, 0, ms); }

/* the log, which players attach to public reports: each line is read for the nodes' nicknames as it comes, and kept
   since heard_reset for what it says of the nodes instead. Both consoles' transport threads write it */
static const char* const g_nicknames[] = {"Host", "Station"};
static struct {
    SnxLock lk;
    char lines[4096]; /* since heard_reset */
    char named[512];  /* the first line that named a node */
} g_heard;
static void heard(const char* m) {
    size_t n, i;
    snx_lock(&g_heard.lk);
    for (i = 0; i < sizeof g_nicknames / sizeof g_nicknames[0]; i++)
        if (strstr(m, g_nicknames[i]) && !g_heard.named[0]) snprintf(g_heard.named, sizeof g_heard.named, "%s", m);
    n = strlen(g_heard.lines);
    snprintf(g_heard.lines + n, sizeof g_heard.lines - n, "%s", m);
    snx_unlock(&g_heard.lk);
}
static void heard_reset(void) {
    snx_lock(&g_heard.lk);
    g_heard.lines[0] = 0;
    snx_unlock(&g_heard.lk);
}
/* whether a line since heard_reset has text, waiting up to ms for it: the other console's thread may log it later */
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
    if (named[0]) fprintf(stderr, "ldn_lan: a node's nickname in the log: %s", named);
    CHECK(!named[0]);
}

/* ---- the frames */
static void frames(void) {
    uint8_t payload[300], out[SNX_LDN_FRAME_MAX], back[SNX_LDN_FRAME_MAX], type;
    size_t n, used, i, got;
    int r;
    /* mostly zeros: coded shorter, and back byte for byte */
    memset(payload, 0, sizeof payload);
    payload[0] = 7;
    payload[100] = 9;
    payload[299] = 1;
    n = snx_ldn_frame(SNX_LDN_SYNC_NETWORK, payload, sizeof payload, out, sizeof out);
    CHECK(n > SNX_LDN_HEADER && n < SNX_LDN_HEADER + sizeof payload);
    CHECK(out[0] == 0x00 && out[1] == 0x14 && out[2] == 0x45 && out[3] == 0x11); /* the magic, little-endian */
    CHECK(out[4] == SNX_LDN_SYNC_NETWORK && out[5] == 1);
    CHECK(out[8] == (sizeof payload & 0xFF) && out[9] == (sizeof payload >> 8));
    r = snx_ldn_unframe(out, n, &type, back, sizeof back, &got, &used);
    CHECK(r == 1 && got == sizeof payload && used == n && type == SNX_LDN_SYNC_NETWORK);
    CHECK(!memcmp(back, payload, sizeof payload));
    /* a run of more than 255 zeros takes two runs */
    memset(payload, 0, sizeof payload);
    n = snx_ldn_frame(SNX_LDN_SCAN_RESPONSE, payload, sizeof payload, out, sizeof out);
    CHECK(n == SNX_LDN_HEADER + 4 && out[SNX_LDN_HEADER + 1] == 255 && out[SNX_LDN_HEADER + 3] == 43);
    r = snx_ldn_unframe(out, n, &type, back, sizeof back, &got, &used);
    CHECK(r == 1 && got == sizeof payload && !memcmp(back, payload, sizeof payload));
    /* no zeros: sent as it is, not compressed */
    for (i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i + 1);
    n = snx_ldn_frame(SNX_LDN_CONNECT, payload, sizeof payload, out, sizeof out);
    CHECK(n == SNX_LDN_HEADER + sizeof payload && out[5] == 0 && out[8] == 0 && out[9] == 0);
    r = snx_ldn_unframe(out, n, &type, back, sizeof back, &got, &used);
    CHECK(r == 1 && got == sizeof payload && !memcmp(back, payload, sizeof payload));
    /* a header alone (Scan) is a whole frame with nothing in it */
    n = snx_ldn_frame(SNX_LDN_SCAN, NULL, 0, out, sizeof out);
    CHECK(n == SNX_LDN_HEADER);
    r = snx_ldn_unframe(out, n, &type, back, sizeof back, &got, &used);
    CHECK(r == 1 && got == 0 && used == n && type == SNX_LDN_SCAN);
    /* an incomplete frame is 0, the next bytes make it whole; a wrong magic and a run cut short are -1 */
    n = snx_ldn_frame(SNX_LDN_CONNECT, payload, 20, out, sizeof out);
    CHECK(snx_ldn_unframe(out, n - 1, &type, back, sizeof back, &got, &used) == 0);
    CHECK(snx_ldn_unframe(out, 5, &type, back, sizeof back, &got, &used) == 0);
    CHECK(snx_ldn_unframe(out, n, &type, back, sizeof back, &got, &used) == 1 && got == 20);
    out[3] = 0x12;
    CHECK(snx_ldn_unframe(out, n, &type, back, sizeof back, &got, &used) == -1);
    out[3] = 0x11;
    out[5] = 1; /* claims compression: a stream of non-zero bytes decodes to itself, but the length doesn't match */
    CHECK(snx_ldn_unframe(out, n, &type, back, sizeof back, &got, &used) == -1);
    out[5] = 0;
    out[6] = 0xFF; /* a length past the frame's limit */
    out[7] = 0x7F;
    CHECK(snx_ldn_unframe(out, n, &type, back, sizeof back, &got, &used) == -1);
    /* what doesn't fit the output is refused, not cut */
    CHECK(snx_ldn_frame(SNX_LDN_CONNECT, payload, sizeof payload, out, 100) == 0);
    CHECK(snx_ldn_frame(SNX_LDN_CONNECT, payload, SNX_LDN_FRAME_MAX, out, sizeof out) == 0);
}

/* ---- two consoles */
static int changes_a, changes_b, gone_b; /* gone_b: the station's callbacks that found it out of the network */
static void changed_a(void* arg) {
    (void)arg;
    changes_a++;
}
static void changed_b(void* arg) { /* arg: the station's SnxLdn*, set before anything can change */
    changes_b++;
    if (!snx_ldn_connected(*(SnxLdn* const*)arg)) gone_b++;
}
static const uint8_t lo_mask[4] = {255, 0, 0, 0};
static const uint8_t ip_a[4] = {127, 0, 0, 1}, ip_b[4] = {127, 0, 0, 2};

static void name_of(uint8_t out[SNX_LDN_USER_NAME], const char* name) {
    memset(out, 0, SNX_LDN_USER_NAME);
    strcpy((char*)out, name);
}
/* a network as ldn.c builds one for CreateNetwork */
static void network_of(SnxLdn* host, SnxLdnNetworkInfo* n, uint64_t local_communication_id) {
    int i;
    memset(n, 0, sizeof *n);
    n->local_communication_id = local_communication_id;
    n->scene_id = 3;
    n->session_id[0] = 0x1122334455667788ull;
    n->session_id[1] = 0x99aabbccddeeff00ull;
    snx_ldn_fake_mac(ip_a, n->bssid);
    n->ssid_length = 32;
    memset(n->ssid, 'a', 32);
    n->channel = 6;
    n->link_level = 3;
    n->network_type = 2;
    n->node_count_max = 8;
    n->node_count = 1;
    for (i = 0; i < SNX_LDN_NODES; i++) n->nodes[i].node_id = (int8_t)i;
    {
        uint8_t name[SNX_LDN_USER_NAME];
        name_of(name, "Host");
        snx_ldn_node_init(host, &n->nodes[0], name, 4);
    }
}
static int wait_for(int* counter, int at_least, int ms) {
    int i;
    for (i = 0; i < ms; i++) {
        if (*counter >= at_least) return 1;
        sleep_ms(1);
    }
    return *counter >= at_least;
}

static void two_consoles(void) {
    SnxLdn *a, *b, *c;
    SnxLdnNetworkInfo net, found[4], info;
    SnxLdnScanFilter filter;
    uint8_t name[SNX_LDN_USER_NAME], updates[SNX_LDN_NODES], ip[4], mask[4];
    int r, n;
    r = snx_ldn_open(&a, ip_a, lo_mask, changed_a, NULL);
    CHECK(r == 0);
    r = snx_ldn_open(&b, ip_b, lo_mask, changed_b, &b);
    if (r == -SNX_EADDRNOTAVAIL) { /* a host whose loopback has 127.0.0.1 alone (macOS) */
        printf("ldn_lan: skipped the two consoles: this host has no 127.0.0.2\n");
        snx_ldn_close(a);
        return;
    }
    CHECK(r == 0);
    /* the port is one per console: a third on 127.0.0.1 is refused */
    CHECK(snx_ldn_open(&c, ip_a, lo_mask, NULL, NULL) == -SNX_EADDRINUSE);
    snx_ldn_address(b, ip, mask);
    CHECK(!memcmp(ip, ip_b, 4) && !memcmp(mask, lo_mask, 4));
    CHECK(!snx_ldn_connected(a) && !snx_ldn_connected(b));
    CHECK(snx_ldn_network_info(a, &info, NULL) == 0);

    /* nothing to find before the host creates its network: the scan waits its whole time out */
    CHECK(snx_ldn_scan(b, NULL, found, 4, 150) == 0);

    /* the host's network, with its advertisement set before and after */
    CHECK(snx_ldn_set_advertise_data(a, "before", 6) == 0);
    network_of(a, &net, 0x0100ABCD12345000ull);
    CHECK(snx_ldn_create_network(a, &net) == 0);
    CHECK(snx_ldn_create_network(a, &net) == -SNX_EBUSY);
    CHECK(snx_ldn_connected(a));
    CHECK(snx_ldn_network_info(a, &info, updates) == 1);
    CHECK(info.node_count == 1 && info.nodes[0].is_connected && !strcmp((char*)info.nodes[0].user_name, "Host"));
    CHECK(info.nodes[0].ipv4[0] == 1 && info.nodes[0].ipv4[3] == 127); /* host order */
    CHECK(snx_le16(info.advertise_data_size) == 6 && !memcmp(info.advertise_data, "before", 6));
    CHECK(updates[0] == SNX_LDN_NODE_CONNECT && updates[1] == 0);
    CHECK(snx_ldn_network_info(a, &info, updates) == 1 && updates[0] == 0); /* read once */
    CHECK(snx_ldn_set_advertise_data(a, "after!!", 7) == 0);
    CHECK(snx_ldn_set_advertise_data(a, found, SNX_LDN_ADVERTISE_MAX + 1) == -SNX_EINVAL);

    /* the station finds it, filtered by what the game asks for */
    memset(&filter, 0, sizeof filter);
    filter.flag = snx_le32(SNX_LDN_FILTER_LOCAL_COMMUNICATION_ID); /* the structures are little-endian */
    filter.local_communication_id = 0x0100ABCD12345000ull;
    n = snx_ldn_scan(b, &filter, found, 4, 1000);
    CHECK(n == 1);
    CHECK(found[0].local_communication_id == 0x0100ABCD12345000ull && found[0].scene_id == 3);
    CHECK(snx_le16(found[0].advertise_data_size) == 7 && !memcmp(found[0].advertise_data, "after!!", 7));
    CHECK(!memcmp(found[0].bssid, net.bssid, 6) && found[0].node_count == 1);
    filter.local_communication_id = 0x0100000000010000ull; /* another game's */
    CHECK(snx_ldn_scan(b, &filter, found, 4, 300) == 0);
    filter.flag = snx_le32(SNX_LDN_FILTER_SESSION_ID | SNX_LDN_FILTER_SCENE_ID); /* the structures are little-endian */
    filter.session_id[0] = net.session_id[0];
    filter.session_id[1] = net.session_id[1];
    filter.scene_id = 3;
    CHECK(snx_ldn_scan(b, &filter, found, 4, 1000) == 1);
    filter.scene_id = 4;
    CHECK(snx_ldn_scan(b, &filter, found, 4, 300) == 0);
    /* the host doesn't find itself */
    CHECK(snx_ldn_scan(a, NULL, found, 4, 300) == 0);

    /* joining: the host gets node 1, both sides see two nodes, the station's event fires on the sync (on its
       transport's thread, which may run it after the connect returns) */
    name_of(name, "Station");
    changes_a = changes_b = 0;
    heard_reset();
    CHECK(snx_ldn_connect(b, &net, name, 4, 2000) == 0);
    CHECK(snx_ldn_connected(b));
    CHECK(wait_for(&changes_a, 1, 2000) && wait_for(&changes_b, 1, 2000));
    /* the log says each side's join by node and address, never by nickname */
    CHECK(heard_of("ldn: node 1 joined from 127.0.0.2, 2 of 8 nodes", 2000));
    CHECK(heard_of("ldn: joined the network of the host at 127.0.0.1, 2 of 8 nodes", 2000));
    unnamed();
    CHECK(snx_ldn_network_info(a, &info, updates) == 1);
    CHECK(info.node_count == 2 && info.nodes[1].is_connected && info.nodes[1].node_id == 1);
    CHECK(!strcmp((char*)info.nodes[1].user_name, "Station") && (int16_t)snx_le16((uint16_t)info.nodes[1].version) == 4);
    CHECK(info.nodes[1].ipv4[0] == 2 && info.nodes[1].ipv4[3] == 127);
    CHECK(updates[1] == SNX_LDN_NODE_CONNECT && updates[0] == 0);
    CHECK(snx_ldn_network_info(b, &info, updates) == 1);
    CHECK(info.node_count == 2 && !strcmp((char*)info.nodes[0].user_name, "Host") &&
          !strcmp((char*)info.nodes[1].user_name, "Station"));
    CHECK(!memcmp(info.bssid, net.bssid, 6) && info.session_id[1] == net.session_id[1]);
    CHECK(updates[0] == SNX_LDN_NODE_CONNECT && updates[1] == SNX_LDN_NODE_CONNECT && updates[2] == 0);
    CHECK(snx_ldn_connect(b, &net, name, 4, 100) == -SNX_EBUSY); /* already in one */
    /* an advertisement change reaches the station */
    changes_b = 0;
    CHECK(snx_ldn_set_advertise_data(a, "third", 5) == 0);
    CHECK(wait_for(&changes_b, 1, 2000));
    CHECK(snx_ldn_network_info(b, &info, NULL) == 1 && snx_le16(info.advertise_data_size) == 5);

    /* the station leaves: the host's node 1 empties */
    changes_a = 0;
    heard_reset();
    snx_ldn_disconnect(b);
    CHECK(!snx_ldn_connected(b) && snx_ldn_disconnect_reason(b) == SNX_LDN_REASON_DISCONNECTED_BY_USER);
    CHECK(wait_for(&changes_a, 1, 2000));
    CHECK(snx_ldn_network_info(a, &info, updates) == 1);
    CHECK(info.node_count == 1 && !info.nodes[1].is_connected && info.nodes[1].user_name[0] == 0);
    CHECK(updates[1] == SNX_LDN_NODE_DISCONNECT);
    CHECK(heard_of("ldn: node 1 left", 2000));
    unnamed();

    /* it joins again, and the host goes away: the station is told, with the reason */
    CHECK(snx_ldn_connect(b, &net, name, 4, 2000) == 0);
    gone_b = 0; /* a late callback of the join counts only if it finds the station out, as checked below */
    snx_ldn_destroy_network(a);
    CHECK(!snx_ldn_connected(a));
    CHECK(wait_for(&gone_b, 1, 2000));
    CHECK(!snx_ldn_connected(b) && snx_ldn_disconnect_reason(b) == SNX_LDN_REASON_DESTROYED_BY_USER);
    CHECK(snx_ldn_network_info(b, &info, NULL) == 0);
    /* no network there now: the host's port takes the connection and closes it, and a network with no nodes is
       refused before trying */
    r = snx_ldn_connect(b, &net, name, 4, 2000);
    CHECK(r == -SNX_ECONNRESET);
    snx_ldn_close(a);
    /* nobody at all: refused by the host. Windows tells it only after trying the SYN again, about two seconds, so
       the wait is the service's (LDN_CONNECT_MS) */
    r = snx_ldn_connect(b, &net, name, 4, 4000);
    CHECK(r == -SNX_ECONNREFUSED);
    net.node_count = 0;
    CHECK(snx_ldn_connect(b, &net, name, 4, 100) == -SNX_EINVAL);

    snx_ldn_close(b);
    /* closed: the address is free again */
    CHECK(snx_ldn_open(&c, ip_a, lo_mask, NULL, NULL) == 0);
    snx_ldn_close(c);
}

int main(void) {
    CHECK(snx_net_init() == 0);
    snx_lock_init(&g_heard.lk);
    snx_net_set_log(heard);
    frames();
    two_consoles();
    unnamed(); /* the host going away and the joins it refused too */
    printf("ldn_lan: %u checks passed\n", checks);
    return 0;
}
