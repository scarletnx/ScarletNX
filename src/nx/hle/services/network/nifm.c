/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   The online half ported from Eden's src/core/hle/service/nifm/nifm.cpp (branch lizzie/refactor-networking-12):
     SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project, GPL-3.0-or-later
     SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project, GPL-2.0-or-later
   nifm, which tells the game whether the console is connected. Offline (the network option off, or a host without a
   connection) it answers what misc.c answered before the network stack existed: no request is accepted, and the
   queries about the connection fail like GetCurrentIpAddress does on a console without one. Online it reports the
   host's interface, the one with the default route. */
#include "nx/hle/services/network/network.h"
#include "nx/hle/hle.h"
#include "hle/services/network/nifm.swipc.inc"

enum { REQ_INVALID = 1, REQ_ON_HOLD = 2, REQ_ACCEPTED = 3 }; /* nn::nifm::RequestState (1 is also NotSubmitted) */
#define NIFM_NOT_CONNECTED RC(110, 300)

/* IRequest: u[0] the state (an atomic: game threads submit and poll at once), data the two events (NifmRequest) */
static void nifm_req_destroy(SvcObj* o) {
    NifmRequest* r = o->data;
    for (int i = 0; i < 2; i++) kobj_unref(kobj_at(r->ev[i]));
}
static int nn_nifm_detail_IRequest_GetRequestState(SvcObj* self, i32* state) {
    *state = network_online() ? (i32)__atomic_load_n(&self->u[0], __ATOMIC_RELAXED) : REQ_INVALID;
    R_SUCCEED();
}
static int nn_nifm_detail_IRequest_GetResult(SvcObj* self) {
    return network_online() && __atomic_load_n(&self->u[0], __ATOMIC_RELAXED) != REQ_INVALID ? 0 : NIFM_NOT_CONNECTED;
}
static int nn_nifm_detail_IRequest_GetSystemEventReadableHandles(SvcObj* self, u32* state_event, u32* other_event) {
    NifmRequest* r = self->data;
    *state_event = ev_handle(r->ev[0]);
    *other_event = ev_handle(r->ev[1]);
    R_SUCCEED();
}
static int nn_nifm_detail_IRequest_Cancel(SvcObj* self) {
    __atomic_store_n(&self->u[0], REQ_INVALID, __ATOMIC_RELAXED);
    R_SUCCEED();
}
/* the host is connected already, so it is accepted at once. Offline it is refused at once, and the state event says
   so either way, as it does when the network went down after the request was made */
static int nn_nifm_detail_IRequest_Submit(SvcObj* self) {
    __atomic_store_n(&self->u[0], network_online() ? REQ_ACCEPTED : REQ_INVALID, __ATOMIC_RELAXED);
    ev_signal(((NifmRequest*)self->data)->ev[0]);
    R_SUCCEED();
}
static SvcObj* nifm_req_new(void) {
    SvcObj* o = nn_nifm_detail_IRequest_new_data(g_hle.nifm.reqs, g_hle.nifm.req_next, "nifm requests");
    NifmRequest* r = o->data;
    r->ev[0] = owned_event("nifm0");
    r->ev[1] = owned_event("nifm1");
    /* offline the state event starts signaled: the SDK reads the state, which says the request was refused */
    if (!network_online()) kev(r->ev[0])->signaled = 1;
    o->u[0] = REQ_INVALID;
    o->destroy = nifm_req_destroy;
    return o;
}
static int nn_nifm_detail_IScanRequest_GetSystemEventReadableHandle(SvcObj* self, u32* event) {
    return new_event(event, "nifm scan", 1);
}

/* nn::nifm::IpAddressSetting (0xD) then DnsSetting (0x9), the start of IpSettingData and GetCurrentIpConfigInfo's
   reply */
static void ip_config(const SnxNetInterface* i, u8 out[0x16]) {
    uint8_t dns[2][4] = {{1, 1, 1, 1}, {1, 0, 0, 1}};
    uint8_t found[4][4];
    int n = snx_net_dns_servers(found, 4, NULL, 0), k = 0;
    /* a resolver on the host itself (systemd-resolved) means nothing on the console's side: Eden's public ones then */
    for (int j = 0; j < n && k < 2; j++)
        if (found[j][0] != 127) memcpy(dns[k++], found[j], 4);
    memset(out, 0, 0x16);
    out[0] = 1; /* is_automatic */
    memcpy(out + 1, i->ip, 4);
    memcpy(out + 5, i->mask, 4);
    memcpy(out + 9, i->gateway, 4);
    out[0xD] = 1;
    memcpy(out + 0xE, dns[0], 4);
    memcpy(out + 0x12, dns[1], 4);
}
/* nn::nifm::detail::sf::NetworkProfileData (0x17C) for the host's interface */
static void network_profile(const SnxNetInterface* i, u8* p) {
    memset(p, 0, 0x17C);
    ip_config(i, p);
    p[0xC0] = 1500 & 0xFF; /* IpSettingData's MTU, after the 0xAA bytes of proxy settings, all off */
    p[0xC1] = 1500 >> 8;
    /* the profile's id: two FNV-1a hashes of the interface's name, as Eden makes it from the SSID */
    uint64_t h[2] = {0xcbf29ce484222325ull, 0xcbf29ce484222325ull ^ 0x9e3779b97f4a7c15ull};
    for (const char* s = i->name; *s; s++) {
        h[0] = (h[0] ^ (u8)*s) * 0x100000001b3ull;
        h[1] = (h[1] ^ (u8)(*s ^ 0x5A)) * 0x100000001b3ull;
    }
    snx_st_le64(p + 0xC2, h[0]); /* the same id as a little-endian host's */
    snx_st_le64(p + 0xCA, h[1]);
    snprintf((char*)p + 0xD2, 0x40, "%s", i->name);
    p[0x112] = 0;                   /* profile type: user */
    p[0x113] = i->wireless ? 1 : 2; /* interface type: Wi-Fi or Ethernet */
    p[0x114] = 1;                   /* is_auto_connect */
    if (i->wireless) {              /* the SSID isn't known: the interface's name stands for it */
        size_t n = strlen(i->name) < 0x20 ? strlen(i->name) : 0x20;
        p[0x116] = (u8)n;
        memcpy(p + 0x117, i->name, n);
        p[0x139] = 1; /* is_secured */
    }
}

static int nn_nifm_detail_IGeneralService_GetClientId(SvcObj* self, IpcBuf client_id) {
    if (client_id.addr && client_id.size >= 4) gw32(client_id.addr, 1);
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_CreateScanRequest(SvcObj* self, SvcObj** request) {
    *request = nn_nifm_detail_IScanRequest_new();
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_CreateRequest(SvcObj* self, i32 requirement_preset, SvcObj** request) {
    *request = nifm_req_new();
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_GetCurrentNetworkProfile(SvcObj* self, IpcBuf profile) {
    SnxNetInterface i;
    u8 p[0x17C];
    R_UNLESS(!network_interface(&i), NIFM_NOT_CONNECTED);
    network_profile(&i, p);
    if (profile.addr) memcpy(GP(profile.addr), p, profile.size < sizeof p ? profile.size : sizeof p);
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_GetCurrentIpAddress(SvcObj* self, nn_nifm_IpV4Address* address) {
    SnxNetInterface i;
    R_UNLESS(!network_interface(&i), NIFM_NOT_CONNECTED);
    memcpy(address->v, i.ip, 4);
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_GetCurrentIpConfigInfo(SvcObj* self, nn_nifm_IpAddressSetting* ip_setting,
                                                                 nn_nifm_DnsSetting* dns_setting) {
    SnxNetInterface i;
    u8 cfg[0x16];
    R_UNLESS(!network_interface(&i), NIFM_NOT_CONNECTED);
    ip_config(&i, cfg);
    memcpy(ip_setting->v, cfg, 0xD);
    memcpy(dns_setting->v, cfg + 0xD, 9);
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_IsWirelessCommunicationEnabled(SvcObj* self, u8* enabled) {
    *enabled = (u8)network_online();
    R_SUCCEED();
}
/* the interface's type, the signal bars and the state */
static int nn_nifm_detail_IGeneralService_GetInternetConnectionStatus(SvcObj* self, u8* interface_type,
                                                                      u8* wifi_strength, u8* status) {
    SnxNetInterface i;
    R_UNLESS(!network_interface(&i), NIFM_NOT_CONNECTED);
    *interface_type = i.wireless ? 1 : 2;
    *wifi_strength = 3;
    *status = 4; /* connected */
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_IsEthernetCommunicationEnabled(SvcObj* self, u8* enabled) {
    *enabled = (u8)network_online();
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_IsAnyInternetRequestAccepted(SvcObj* self, IpcBuf client_id, u8* accepted) {
    *accepted = (u8)network_online();
    R_SUCCEED();
}
static int nn_nifm_detail_IGeneralService_IsAnyForegroundRequestAccepted(SvcObj* self, u8* accepted) {
    *accepted = (u8)network_online();
    R_SUCCEED();
}
static int general_service(SvcObj** service) {
    *service = nn_nifm_detail_IGeneralService_new();
    R_SUCCEED();
}
static int nn_nifm_detail_IStaticService_CreateGeneralServiceOld(SvcObj* self, SvcObj** service) {
    return general_service(service);
}
static int nn_nifm_detail_IStaticService_CreateGeneralService(SvcObj* self, u64 pid, SvcObj** service) {
    return general_service(service);
}
SvcObj* new_nifm(void) { return nn_nifm_detail_IStaticService_new(); }
