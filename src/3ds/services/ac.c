/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Copyright 2014-2026 Citra Emulator Project / Azahar Emulator Project */
/* ac:u and ac:i, the console's connection manager: connected to the internet when the console is online (network=1,
   not in airplane mode), as Azahar's ac.cpp (GPL-2.0-or-later) answers always, and not connected otherwise. A game
   connects with ConnectAsync and an event, which is signalled at once with the result GetConnectResult then gives */
#include "3ds/services/ac.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/soc.h"
#include "3ds/services/system.h"
#include "common/utilities/endian.h"
#include <string.h>

/* Azahar's CancelConnectAsync code for a console that is not connected (description 302, AC, invalid state, usage) */
#define AC_NOT_CONNECTED 0xE0A09D2Eu
#define AC_CONFIG        0x200u

static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated, uint32_t result) {
    memset(c->out, 0, sizeof c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = result;
}
/* the event of the handle at word i, signalled; false for a handle that is not an event */
static bool signal_event(SnxCtrIpc* c, unsigned i) {
    SnxCtrProcess* p = c->process;
    uint32_t id = snx_ctr_handle_object(p, c->in[i]);
    if (!id || p->objects[id].type != CTR_OBJ_EVENT) return false;
    p->objects[id].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, id);
    return true;
}
/* the event of the handle at word i kept as the service's (one reference), the one before released */
static void keep_event(SnxCtrIpc* c, unsigned i, uint32_t* kept) {
    SnxCtrProcess* p = c->process;
    uint32_t id = snx_ctr_handle_object(p, c->in[i]);
    if (*kept) snx_ctr_object_release(p, *kept);
    *kept = id && p->objects[id].type == CTR_OBJ_EVENT ? id : 0;
    if (*kept) ++p->objects[*kept].refs;
}
static void config_out(SnxCtrIpc* c) {
    static const uint8_t config[AC_CONFIG];
    uint8_t d[8];
    uint32_t tls = c->process->cpu.state.cpu.tpidruro;
    reply(c, 1, 2, 0);
    if (snx_ctr_read_ram(c->process->space, tls + 0x180, d, 8) != SNX_CTR_MEM_OK) {
        c->transport_result = 0xe0e01bf5u;
        return;
    }
    uint32_t desc = snx_ld_le32(d), address = snx_ld_le32(d + 4),
             size = desc >> 14 < AC_CONFIG ? desc >> 14 : AC_CONFIG;
    if ((desc & 0xf) != 2 || snx_ctr_write_ram(c->process->space, address, config, size) != SNX_CTR_MEM_OK) {
        snx_ctr_ipc_bad(c);
        return;
    }
    c->out[2] = size << 14 | 2;
    c->out[3] = address;
}

void snx_ctr_ac_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    uint32_t command = c->in[0] >> 16;
    bool online = snx_ctr_online();
    switch (command) {
        case 0x0001: config_out(c); break; /* CreateDefaultConfig: an empty config */
        case 0x0004:                       /* ConnectAsync: its event signalled once the connection is decided */
            p->ac_connected = online;
            if (!signal_event(c, 4)) {
                snx_ctr_ipc_bad(c);
                break;
            }
            reply(c, 1, 0, 0);
            break;
        case 0x0005: reply(c, 1, 0, p->ac_connected ? 0 : AC_NOT_CONNECTED); break;           /* GetConnectResult */
        case 0x0007: reply(c, 1, 0, p->ac_connected ? 0xE0A09D2Du : AC_NOT_CONNECTED); break; /* CancelConnectAsync */
        case 0x0008: /* CloseAsync: the disconnect event too, when it was connected */
            if (p->ac_connected && p->ac_disconnect_event) {
                p->objects[p->ac_disconnect_event].data.event.signaled = 1;
                snx_ctr_wake_waiters(p, p->ac_disconnect_event);
            }
            p->ac_connected = false;
            (void)signal_event(c, 4);
            reply(c, 1, 0, 0);
            break;
        case 0x0009: reply(c, 1, 0, 0); break; /* GetCloseResult */
        case 0x000C:                           /* GetStatus: 3 is the internet's, 0 none */
            reply(c, 2, 0, 0);
            c->out[2] = p->ac_connected && online ? 3 : 0;
            break;
        case 0x000D: /* GetWifiStatus: 1 is the first slot's connection */
            reply(c, 2, 0, 0);
            c->out[2] = p->ac_connected && online ? 1 : 0;
            break;
        case 0x0027: /* GetInfraPriority */
        case 0x0036: /* GetConnectingProxyEnable: no proxy */ reply(c, 2, 0, 0); break;
        case 0x002D: config_out(c); break; /* SetRequestEulaVersion: the config given back */
        case 0x0030:                       /* RegisterDisconnectEvent */
            keep_event(c, 4, &p->ac_disconnect_event);
            reply(c, 1, 0, 0);
            break;
        case 0x003E: /* IsConnected */
            reply(c, 2, 0, 0);
            c->out[2] = p->ac_connected && online;
            break;
        case 0x0040: reply(c, 1, 0, 0); break; /* SetClientVersion */
        default: snx_ctr_system_unmodeled(c, "ac:u", (SNX_CTR_SERVICE_AC << 16) | command); break;
    }
}

void snx_ctr_ac_close(SnxCtrProcess* p) {
    if (p->ac_disconnect_event) snx_ctr_object_release(p, p->ac_disconnect_event);
    p->ac_disconnect_event = 0;
}
