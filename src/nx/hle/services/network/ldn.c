/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The state machine follows Eden's src/core/hle/service/ldn/lan_discovery.cpp and
   user_local_communication_service.cpp (SPDX-FileCopyrightText: Copyright 2022, 2024 yuzu Emulator Project,
   GPL-2.0-or-later; Copyright 2026 Eden Emulator Project, GPL-3.0-or-later) and Ryujinx's
   Services/Ldn/UserServiceCreator (MIT) for the results the console gives */
/* ldn:u, local wireless: the game creates a network (an access point) or scans for one and joins it (a station),
   and from then on talks to the other consoles through its own sockets, at the addresses the nodes carry. The
   networks travel over the LAN with ldn_mitm's protocol (common/network/net_ldn.c), so the other side may be a
   ScarletNX, a Ryujinx or a console with ldn_mitm. The states and results are the console's (nn::ldn::State,
   module 203).

   Each console needs an address of its own on the LAN, which the other nodes reach: the host's interface with
   network=1, else (offline, or no interface) 127.0.0.N, the first N whose ldn port is free, so two consoles on one
   machine get 127.0.0.1 and 127.0.0.2. While the service is initialized, a game socket bound to INADDR_ANY binds
   that address (bsd.c's bsd_set_bind_address), so the two consoles' game sockets don't collide on their ports either.

   With ldn=room the networks go through the Eden room the console joined at start (room.c), at the address the room
   gave it, and the game's datagram sockets are the room's (bsd.c). ldn=off is a console whose local wireless is
   disabled, and airplane_mode one in airplane mode: Initialize says so */
#include "nx/hle/services/network/network.h"
#include "common/network/net_ldn.h"
#include "common/network/net_room.h"
#include "nx/hle/hle.h"
#include "hle/services/network/ldn.swipc.inc"
#include <openssl/rand.h>

enum {
    LDN_NONE = 0,
    LDN_INITIALIZED = 1,
    LDN_ACCESS_POINT_OPENED = 2,
    LDN_ACCESS_POINT_CREATED = 3,
    LDN_STATION_OPENED = 4,
    LDN_STATION_CONNECTED = 5,
    LDN_ERROR = 6
};
#define LDN_ADVERTISE_DATA_TOO_LARGE RC(203, 10)
#define LDN_DISABLED                 RC(203, 22)
#define LDN_AIRPLANE_MODE_ENABLED    RC(203, 23)
#define LDN_INVALID_NODE_COUNT       RC(203, 30)
#define LDN_BAD_STATE                RC(203, 32)
#define LDN_NO_IP_ADDRESS            RC(203, 33)
#define LDN_INVALID_BUFFER_COUNT     RC(203, 50)
#define LDN_CONNECT_FAILURE          RC(203, 64)
#define LDN_CONNECT_TIMEOUT          RC(203, 66)
#define LDN_BAD_INPUT                RC(203, 96)
#define LDN_SCAN_MS                  1000 /* Eden and Ryujinx wait a second for the replies */
#define LDN_CONNECT_MS               4000 /* Ryujinx's FailureTimeout */

/* the layout of the configs the game sends (Eden's ldn_types.h) */
enum {
    CFG_SECURITY_MODE = 0,        /* SecurityConfig: u16 mode, u16 passphrase size, u8 passphrase[64] */
    CFG_USER_NAME = 0x44,         /* UserConfig: u8 user_name[33], reserved */
    CFG_NETWORK = 0x78,           /* NetworkConfig after 4 bytes of padding */
    CFG_PRIVATE_USER_NAME = 0x64, /* CreateNetworkConfigPrivate: the SecurityParameter sits between */
    CFG_PRIVATE_NETWORK = 0x98,
    NET_INTENT = 0, /* NetworkConfig: IntentId (u64 local communication id, pad, u16 scene id, pad) */
    NET_CHANNEL = 0x10,
    NET_NODE_COUNT_MAX = 0x12,
    NET_VERSION = 0x14,
    CONNECT_VERSION = 0x74 /* ConnectNetworkData: SecurityConfig, UserConfig, i32 version, u32 option */
};

static void set_state(Ldn* d, u32 state) {
    d->state = state;
    ev_signal(d->ev);
}
/* the transport's thread: a station or the host came or went. A Finalize under way has taken the transport away
   under the lock and is waiting for this thread, so the callback leaves at once then */
static void ldn_changed(void* arg) {
    Ldn* d = arg;
    pm_lock(&d->lk);
    if (!d->lan) {
    } else if (d->state == LDN_STATION_CONNECTED && !snx_ldn_connected(d->lan)) {
        d->reason = (i16)snx_ldn_disconnect_reason(d->lan);
        set_state(d, LDN_STATION_OPENED);
    } else
        ev_signal(d->ev);
    pm_unlock(&d->lk);
}
/* takes the transport away under the lock and closes it outside: closing waits for its thread, which may be in
   ldn_changed waiting for the lock */
static void ldn_teardown(Ldn* d) {
    SnxLdn* lan;
    pm_lock(&d->lk);
    lan = d->lan;
    d->lan = NULL;
    d->state = LDN_NONE;
    pm_unlock(&d->lk);
    if (lan) {
        snx_ldn_close(lan);
        if (g_opt_ldn != SNX_OPT_LDN_ROOM) bsd_set_bind_address(NULL, NULL);
    }
}
/* every ldn:u is in g_hle.ldn's list, for ldn_exit to close their networks */
static Ldn* ldn_at(u32 i) { return i ? &g_hle.ldn.ldn[i - 1] : NULL; } /* the index + 1 */
static void ldn_destroy(SvcObj* o) {
    Ldn* d = o->data;
    u32 *p, self = POOL_INDEX(g_hle.ldn.ldn, d) + 1;
    pm_lock(&g_host.ldn.ldns_lk);
    for (p = &g_hle.ldn.ldns; *p && *p != self; p = &ldn_at(*p)->next) {}
    if (*p) *p = d->next;
    pm_unlock(&g_host.ldn.ldns_lk);
    ldn_teardown(d);
    kobj_unref(kobj_at(d->ev));
}
void ldn_exit(void) {
    Ldn* d;
    pm_lock(&g_host.ldn.ldns_lk);
    for (d = ldn_at(g_hle.ldn.ldns); d; d = ldn_at(d->next)) ldn_teardown(d);
    pm_unlock(&g_host.ldn.ldns_lk);
}

/* ============================================================ the console's address */
/* 0 and the transport, or -SNX_E*: the room's address with ldn=room, the host's interface online, else 127.0.0.N
   with the first free ldn port */
static int ldn_open(Ldn* d) {
    SnxNetInterface iface;
    int r;
    if (g_opt_ldn == SNX_OPT_LDN_ROOM) {
        if (!network_room() || snx_room_state(network_room()) != SNX_ROOM_IN) return -SNX_ENETDOWN;
        r = snx_ldn_open_room(&d->lan, network_room(), ldn_changed, d);
        if (!r) snx_ldn_address(d->lan, d->ip, d->mask);
        return r;
    }
    if (!network_interface(&iface)) {
        memcpy(d->ip, iface.ip, 4);
        memcpy(d->mask, iface.mask, 4);
        return snx_ldn_open(&d->lan, d->ip, d->mask, ldn_changed, d);
    }
    memset(d->mask, 0, 4);
    d->mask[0] = 255;
    d->ip[0] = 127;
    d->ip[1] = d->ip[2] = 0;
    for (d->ip[3] = 1; d->ip[3] < 255; d->ip[3]++) {
        r = snx_ldn_open(&d->lan, d->ip, d->mask, ldn_changed, d);
        if (r != -SNX_EADDRINUSE) return r;
    }
    return -SNX_EADDRINUSE;
}

/* ============================================================ the objects */
static int nn_ldn_detail_IUserServiceCreator_CreateUserLocalCommunicationService(SvcObj* self, SvcObj** service) {
    SvcObj* o =
        nn_ldn_detail_IUserLocalCommunicationService_new_data(g_hle.ldn.ldn, g_hle.ldn.ldn_next, "ldn:u services");
    Ldn* d = o->data;
    (void)self;
    pm_init(&d->lk);
    d->ev = owned_event("ldn state change");
    o->destroy = ldn_destroy;
    pm_lock(&g_host.ldn.ldns_lk);
    d->next = g_hle.ldn.ldns;
    g_hle.ldn.ldns = POOL_INDEX(g_hle.ldn.ldn, d) + 1;
    pm_unlock(&g_host.ldn.ldns_lk);
    *service = o;
    R_SUCCEED();
}
static int nn_ldn_detail_IUserServiceCreator_CreateClientProcessMonitor(SvcObj* self, SvcObj** monitor) {
    (void)self;
    *monitor = nn_ldn_detail_IClientProcessMonitor_new();
    R_SUCCEED();
}

/* ============================================================ the service */
static int nn_ldn_detail_IUserLocalCommunicationService_GetState(SvcObj* self, u32* state) {
    Ldn* d = self->data;
    pm_lock(&d->lk);
    *state = d->state == LDN_NONE && d->refused ? (u32)LDN_ERROR : d->state;
    pm_unlock(&d->lk);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_AttachStateChangeEvent(SvcObj* self, u32* event) {
    Ldn* d = self->data;
    *event = ev_handle(d->ev);
    R_SUCCEED();
}
/* an Initialize refused: GetState says Error until one is taken, as Eden and Ryujinx answer */
static u32 refuse(Ldn* d, u32 rc) {
    pm_lock(&d->lk);
    d->refused = 1;
    pm_unlock(&d->lk);
    return rc;
}
static int nn_ldn_detail_IUserLocalCommunicationService_Initialize(SvcObj* self, u64 pid) {
    Ldn* d = self->data;
    int r = 0;
    (void)pid;
    if (g_opt_ldn == SNX_OPT_LDN_OFF) {
        WARN_LIMIT(1, "ldn: the game asked for local wireless, which ldn=off disables\n");
        R_THROW(refuse(d, LDN_DISABLED));
    }
    if (g_settings.airplane_mode) {
        WARN_LIMIT(1, "ldn: the game asked for local wireless, which airplane_mode=1 turns off\n");
        R_THROW(refuse(d, LDN_AIRPLANE_MODE_ENABLED));
    }
    pm_lock(&d->lk);
    if (d->state == LDN_NONE) {
        r = ldn_open(d);
        d->refused = (u8)(r != 0);
        if (r) {
            WARN("ldn: no address for this console: %s (errno %d)\n",
                 g_opt_ldn == SNX_OPT_LDN_ROOM && r == -SNX_EBUSY ? "another of the game's ldn:u is in the room already"
                 : g_opt_ldn == SNX_OPT_LDN_ROOM                  ? "out of the Eden room"
                 : r == -SNX_EADDRINUSE                           ? "every loopback address has a console on port 11452"
                                                                  : "the ldn sockets couldn't be opened",
                 -r);
        } else if (g_opt_ldn == SNX_OPT_LDN_ROOM) {
            LOG("ldn: this console is %u.%u.%u.%u in the Eden room\n", d->ip[0], d->ip[1], d->ip[2], d->ip[3]);
            d->state = LDN_INITIALIZED;
            d->reason = 0;
        } else {
            LOG("ldn: this console is %u.%u.%u.%u/%u.%u.%u.%u, the game's sockets bind there\n", d->ip[0], d->ip[1],
                d->ip[2], d->ip[3], d->mask[0], d->mask[1], d->mask[2], d->mask[3]);
            {
                u8 bcast[4];
                int i;
                for (i = 0; i < 4; i++) bcast[i] = (u8)(d->ip[i] | ~d->mask[i]);
                bsd_set_bind_address(d->ip, bcast);
            }
            d->state = LDN_INITIALIZED;
            d->reason = 0;
        }
    }
    pm_unlock(&d->lk);
    R_UNLESS(!r, LDN_NO_IP_ADDRESS);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_InitializeWithVersion(SvcObj* self, i32 version, u64 pid) {
    (void)version;
    return nn_ldn_detail_IUserLocalCommunicationService_Initialize(self, pid);
}
static int nn_ldn_detail_IUserLocalCommunicationService_Finalize(SvcObj* self) {
    ldn_teardown(self->data);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_GetIpv4Address(SvcObj* self, u32* address, u32* subnet_mask) {
    Ldn* d = self->data;
    int i, r;
    pm_lock(&d->lk);
    r = d->state != LDN_NONE;
    if (r) /* host order: the last octet first in memory */
        for (i = 0; i < 4; i++) {
            *address |= (u32)d->ip[i] << (8 * (3 - i));
            *subnet_mask |= (u32)d->mask[i] << (8 * (3 - i));
        }
    pm_unlock(&d->lk);
    R_UNLESS(r, LDN_NO_IP_ADDRESS);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_GetDisconnectReason(SvcObj* self, i16* reason) {
    Ldn* d = self->data;
    pm_lock(&d->lk);
    *reason = d->reason;
    pm_unlock(&d->lk);
    R_SUCCEED();
}

/* the current network, with the lock held: 1 and the network, or 0 outside AccessPointCreated and StationConnected */
static int current_network(Ldn* d, SnxLdnNetworkInfo* info, u8* updates) {
    if (d->state != LDN_ACCESS_POINT_CREATED && d->state != LDN_STATION_CONNECTED) return 0;
    return snx_ldn_network_info(d->lan, info, updates);
}
static int nn_ldn_detail_IUserLocalCommunicationService_GetNetworkInfo(SvcObj* self, IpcBuf info) {
    Ldn* d = self->data;
    SnxLdnNetworkInfo n;
    int r;
    pm_lock(&d->lk);
    r = current_network(d, &n, NULL);
    pm_unlock(&d->lk);
    R_UNLESS(r, LDN_BAD_STATE);
    R_UNLESS(info.addr && info.size >= sizeof n, LDN_BAD_INPUT);
    memcpy(GP(info.addr), &n, sizeof n);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_GetNetworkInfoLatestUpdate(SvcObj* self, IpcBuf info,
                                                                                   IpcBuf updates) {
    Ldn* d = self->data;
    SnxLdnNetworkInfo n;
    u8 changes[SNX_LDN_NODES], *p;
    u64 count = updates.size / 8, i;
    int r;
    R_UNLESS(updates.addr && count, LDN_BAD_INPUT);
    R_UNLESS(count <= SNX_LDN_NODES, LDN_INVALID_BUFFER_COUNT);
    R_UNLESS(info.addr && info.size >= sizeof n, LDN_BAD_INPUT);
    pm_lock(&d->lk);
    r = current_network(d, &n, changes);
    pm_unlock(&d->lk);
    R_UNLESS(r, LDN_BAD_STATE);
    memcpy(GP(info.addr), &n, sizeof n);
    p = GP(updates.addr);
    memset(p, 0, count * 8);
    for (i = 0; i < count; i++) p[i * 8] = changes[i];
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_GetSecurityParameter(SvcObj* self,
                                                                             nn_ldn_SecurityParameter* parameter) {
    Ldn* d = self->data;
    SnxLdnNetworkInfo n;
    int r;
    pm_lock(&d->lk);
    r = current_network(d, &n, NULL);
    pm_unlock(&d->lk);
    R_UNLESS(r, LDN_BAD_STATE);
    memcpy(parameter->v, n.security_parameter, 16);
    memcpy(parameter->v + 16, n.session_id, 16);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_GetNetworkConfig(SvcObj* self, nn_ldn_NetworkConfig* config) {
    Ldn* d = self->data;
    SnxLdnNetworkInfo n;
    int r;
    pm_lock(&d->lk);
    r = current_network(d, &n, NULL);
    pm_unlock(&d->lk);
    R_UNLESS(r, LDN_BAD_STATE);
    memcpy(config->v + NET_INTENT, &n.local_communication_id, 16); /* the IntentId as the network carries it */
    memcpy(config->v + NET_CHANNEL, &n.channel, 2);
    config->v[NET_NODE_COUNT_MAX] = n.node_count_max;
    memcpy(config->v + NET_VERSION, &n.nodes[0].version, 2);
    R_SUCCEED();
}

/* ============================================================ scanning */
static int scan(SvcObj* self, i16 channel, const nn_ldn_ScanFilter* filter, IpcBuf networks, i16* count) {
    Ldn* d = self->data;
    SnxLdnScanFilter f;
    SnxLdnNetworkInfo* found;
    u64 max = networks.size / sizeof(SnxLdnNetworkInfo);
    int n;
    (void)channel;
    R_UNLESS(networks.addr && max, LDN_BAD_INPUT);
    if (max > 16) max = 16;
    pm_lock(&d->lk);
    n = d->state == LDN_NONE ? -1 : 0;
    pm_unlock(&d->lk);
    R_UNLESS(n == 0, LDN_BAD_STATE);
    found = calloc((size_t)max, sizeof *found);
    R_UNLESS(found, LDN_BAD_INPUT);
    memcpy(&f, filter->v, sizeof f);
    /* the transport is only closed by Finalize, which the game doesn't send while it scans */
    n = snx_ldn_scan(d->lan, &f, found, (int)max, LDN_SCAN_MS);
    memcpy(GP(networks.addr), found, (size_t)n * sizeof *found);
    free(found);
    *count = (i16)n;
    LOG("ldn: scan found %d network%s\n", n, n == 1 ? "" : "s");
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_Scan(SvcObj* self, i16 channel, const nn_ldn_ScanFilter* filter,
                                                             IpcBuf networks, i16* count) {
    return scan(self, channel, filter, networks, count);
}
static int nn_ldn_detail_IUserLocalCommunicationService_ScanPrivate(SvcObj* self, i16 channel,
                                                                    const nn_ldn_ScanFilter* filter, IpcBuf networks,
                                                                    i16* count) {
    return scan(self, channel, filter, networks, count);
}

/* ============================================================ the access point */
static int nn_ldn_detail_IUserLocalCommunicationService_OpenAccessPoint(SvcObj* self) {
    Ldn* d = self->data;
    int ok;
    pm_lock(&d->lk);
    ok = d->state == LDN_INITIALIZED;
    if (ok) {
        d->reason = 0;
        set_state(d, LDN_ACCESS_POINT_OPENED);
    }
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_CloseAccessPoint(SvcObj* self) {
    Ldn* d = self->data;
    int ok;
    pm_lock(&d->lk);
    ok = d->state == LDN_ACCESS_POINT_OPENED || d->state == LDN_ACCESS_POINT_CREATED;
    if (ok) {
        snx_ldn_destroy_network(d->lan);
        set_state(d, LDN_INITIALIZED);
    }
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_SUCCEED();
}
/* the network a CreateNetwork's config describes, with this console as node 0 */
static int create_network(Ldn* d, const u8* security, const u8* user, const u8* network) {
    SnxLdnNetworkInfo n;
    int i, r;
    u16 version;
    memset(&n, 0, sizeof n);
    memcpy(&n.local_communication_id, network + NET_INTENT, 16);
    if (RAND_bytes((u8*)n.session_id, sizeof n.session_id) != 1 || RAND_bytes(n.ssid, 32) != 1)
        fatal("The host's random number generator failed");
    for (i = 0; i < 32; i++) n.ssid[i] = (u8)("0123456789abcdef"[n.ssid[i] & 15]); /* 32 hex digits, as a console's */
    n.ssid_length = 32;
    snx_ldn_fake_mac(d->ip, n.bssid);
    memcpy(&n.channel, network + NET_CHANNEL, 2); /* the network keeps the config's little-endian fields */
    if (!n.channel) n.channel = (i16)snx_le16(6);
    n.link_level = 3;   /* Excellent */
    n.network_type = 2; /* Ldn */
    memcpy(&n.security_mode, security + CFG_SECURITY_MODE, 2);
    n.node_count_max = network[NET_NODE_COUNT_MAX];
    n.node_count = 1;
    version = snx_ld_le16(network + NET_VERSION);
    for (i = 0; i < SNX_LDN_NODES; i++) n.nodes[i].node_id = (i8)i;
    snx_ldn_node_init(d->lan, &n.nodes[0], user, (i16)version);
    r = snx_ldn_create_network(d->lan, &n);
    if (r) WARN("ldn: the network couldn't be created: errno %d\n", -r);
    return r;
}
static int nn_ldn_detail_IUserLocalCommunicationService_CreateNetwork(SvcObj* self,
                                                                      const nn_ldn_CreateNetworkConfig* config) {
    Ldn* d = self->data;
    int r = 0, ok;
    pm_lock(&d->lk);
    ok = d->state == LDN_ACCESS_POINT_OPENED;
    if (ok) {
        r = create_network(d, config->v, config->v + CFG_USER_NAME, config->v + CFG_NETWORK);
        if (!r) set_state(d, LDN_ACCESS_POINT_CREATED);
    }
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_UNLESS(!r, LDN_CONNECT_FAILURE);
    R_SUCCEED();
}
/* the private form carries the same pieces further in, and a list of addresses the LAN has no use for */
static int nn_ldn_detail_IUserLocalCommunicationService_CreateNetworkPrivate(
    SvcObj* self, const nn_ldn_CreateNetworkConfigPrivate* config, IpcBuf addresses) {
    Ldn* d = self->data;
    int r = 0, ok;
    (void)addresses;
    pm_lock(&d->lk);
    ok = d->state == LDN_ACCESS_POINT_OPENED;
    if (ok) {
        r = create_network(d, config->v, config->v + CFG_PRIVATE_USER_NAME, config->v + CFG_PRIVATE_NETWORK);
        if (!r) set_state(d, LDN_ACCESS_POINT_CREATED);
    }
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_UNLESS(!r, LDN_CONNECT_FAILURE);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_DestroyNetwork(SvcObj* self) {
    Ldn* d = self->data;
    int ok;
    pm_lock(&d->lk);
    ok = d->state == LDN_ACCESS_POINT_CREATED;
    if (ok) {
        snx_ldn_destroy_network(d->lan);
        set_state(d, LDN_ACCESS_POINT_OPENED);
    }
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_SetAdvertiseData(SvcObj* self, IpcBuf data) {
    Ldn* d = self->data;
    int r;
    R_UNLESS(data.size <= SNX_LDN_ADVERTISE_MAX, LDN_ADVERTISE_DATA_TOO_LARGE);
    pm_lock(&d->lk);
    r = d->state == LDN_NONE ? -1
                             : snx_ldn_set_advertise_data(d->lan, data.addr ? GP(data.addr) : (const u8*)"", data.size);
    pm_unlock(&d->lk);
    R_UNLESS(!r, LDN_BAD_STATE);
    R_SUCCEED();
}

/* ============================================================ the station */
static int nn_ldn_detail_IUserLocalCommunicationService_OpenStation(SvcObj* self) {
    Ldn* d = self->data;
    int ok;
    pm_lock(&d->lk);
    ok = d->state == LDN_INITIALIZED;
    if (ok) {
        d->reason = 0;
        set_state(d, LDN_STATION_OPENED);
    }
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_CloseStation(SvcObj* self) {
    Ldn* d = self->data;
    int ok;
    pm_lock(&d->lk);
    ok = d->state == LDN_STATION_OPENED || d->state == LDN_STATION_CONNECTED;
    if (ok) {
        snx_ldn_disconnect(d->lan);
        set_state(d, LDN_INITIALIZED);
    }
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_Connect(SvcObj* self, const nn_ldn_ConnectNetworkData* data,
                                                                IpcBuf info) {
    Ldn* d = self->data;
    SnxLdnNetworkInfo n;
    i32 version;
    int ok, r;
    R_UNLESS(info.addr && info.size >= sizeof n, LDN_BAD_INPUT);
    memcpy(&n, GP(info.addr), sizeof n);
    version = (i32)snx_ld_le32(data->v + CONNECT_VERSION);
    pm_lock(&d->lk);
    ok = d->state == LDN_STATION_OPENED;
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_UNLESS(n.node_count, LDN_INVALID_NODE_COUNT);
    LOG("ldn: connecting to %u.%u.%u.%u (%s)\n", n.nodes[0].ipv4[3], n.nodes[0].ipv4[2], n.nodes[0].ipv4[1],
        n.nodes[0].ipv4[0], n.nodes[0].user_name);
    /* the wait is the transport's, without the lock: the state stays StationOpened until the host answers */
    r = snx_ldn_connect(d->lan, &n, data->v + CFG_USER_NAME, (i16)version, LDN_CONNECT_MS);
    pm_lock(&d->lk);
    if (!r && d->state == LDN_STATION_OPENED) set_state(d, LDN_STATION_CONNECTED);
    pm_unlock(&d->lk);
    if (r) WARN("ldn: connect failed: errno %d\n", -r);
    R_UNLESS(r != -SNX_ETIMEDOUT, LDN_CONNECT_TIMEOUT);
    R_UNLESS(!r, LDN_CONNECT_FAILURE);
    R_SUCCEED();
}
static int nn_ldn_detail_IUserLocalCommunicationService_Disconnect(SvcObj* self) {
    Ldn* d = self->data;
    int ok;
    pm_lock(&d->lk);
    ok = d->state == LDN_STATION_CONNECTED;
    if (ok) {
        snx_ldn_disconnect(d->lan);
        d->reason = (i16)snx_ldn_disconnect_reason(d->lan);
        set_state(d, LDN_STATION_OPENED);
    }
    pm_unlock(&d->lk);
    R_UNLESS(ok, LDN_BAD_STATE);
    R_SUCCEED();
}

SvcObj* new_ldn(void) { return nn_ldn_detail_IUserServiceCreator_new(); }
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_ldn_state_fns,
               STATE_FN("ldn", ldn_destroy));
