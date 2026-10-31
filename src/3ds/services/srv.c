/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   SRV wire commands/results referenced against Azahar service/sm/{srv.cpp,sm.h}. */
#include "3ds/runtime/internal.h"
#include "3ds/services/dsp.h"
#include "3ds/services/system.h"
#include "services/srv.swipc.inc"
#include <stdio.h>
#include <string.h>
static uint32_t snx_ctr_srv_RegisterClient(SnxCtrIpc* c, uint32_t process) {
    if (process != CTR_PROCESS_ID) return SNX_CTR_IPC_INVALID_STATE;
    c->process->objects[c->object].data.session.client_pid = process;
    return 0;
}
/* The semaphore srv: releases once per notification the process subscribed to (Azahar srv.cpp, at most 16 pending).
   The SDK's notification thread waits on it from its start: without a handle that wait fails and the thread throws a
   fatal error. Nothing here publishes notifications yet, so it is never released */
static uint32_t snx_ctr_srv_EnableNotification(SnxCtrIpc* c, uint32_t* semaphore) {
    SnxCtrProcess* p = c->process;
    if (p->srv_notification) return snx_ctr_handle_add(p, p->srv_notification, semaphore);
    uint32_t result = snx_ctr_object_create(p, CTR_OBJ_SEMAPHORE, semaphore);
    if (result) return result;
    p->srv_notification = snx_ctr_handle_object(p, *semaphore);
    p->objects[p->srv_notification].data.semaphore.max = 16;
    ++p->objects[p->srv_notification].refs; /* the service's own, while the process lives */
    return 0;
}
static uint32_t snx_ctr_srv_GetServiceHandle(SnxCtrIpc* c, const uint8_t* name, uint32_t length, uint32_t flags,
                                             uint32_t* session) {
    SnxCtrProcess* p = c->process;
    if (p->objects[c->object].data.session.client_pid != CTR_PROCESS_ID) return 0xd8e06406u;
    if (!length || length > 8) return 0xd9006405u;
    if (flags > 1) return 0xd8e093edu;
    for (unsigned i = 0; i < length; ++i) {
        if (!name[i]) return 0xd9006407u;
        if (name[i] < 0x21 || name[i] > 0x7e) return 0xd8e06406u;
    }
    if (length == 7 && !memcmp(name, "fs:USER", 7)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_FS, session);
    /* APT:U is the application's, APT:A an applet's and APT:S the system's: the same commands, as Azahar serves them */
    if (length == 5 && (!memcmp(name, "APT:S", 5) || !memcmp(name, "APT:U", 5) || !memcmp(name, "APT:A", 5)))
        return snx_ctr_session_create(p, SNX_CTR_SERVICE_APT, session);
    if ((length == 8 && !memcmp(name, "ptm:sysm", 8)) || (length == 5 && !memcmp(name, "ptm:u", 5)))
        return snx_ctr_session_create(p, SNX_CTR_SERVICE_PTM, session);
    if (length == 8 && (!memcmp(name, "hid:USER", 8) || !memcmp(name, "hid:SPVR", 8)))
        return snx_ctr_session_create(p, SNX_CTR_SERVICE_HID, session);
    if (length == 8 && !memcmp(name, "gsp::Gpu", 8)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_GSP, session);
    if (length == 5 && (!memcmp(name, "cfg:u", 5) || !memcmp(name, "cfg:s", 5) || !memcmp(name, "cfg:i", 5)))
        return snx_ctr_session_create(p, SNX_CTR_SERVICE_CFG, session);
    if (length == 8 && !memcmp(name, "dsp::DSP", 8)) {
        uint32_t result = snx_ctr_dsp_open(p);
        return result ? result : snx_ctr_session_create(p, SNX_CTR_SERVICE_DSP, session);
    }
    if (length == 6 && !memcmp(name, "am:app", 6)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_AM, session);
    if (length == 6 && !memcmp(name, "ldr:ro", 6)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_LDR_RO, session);
    if (length == 5 && !memcmp(name, "y2r:u", 5)) return snx_ctr_session_create(p, SNX_CTR_SERVICE_Y2R, session);
    uint32_t system = snx_ctr_system_open(p, name, length, session);
    if (system != SNX_CTR_SYSTEM_UNKNOWN) return system;
    if (flags & 1) return 0xd0406401u;
    system = snx_ctr_system_stand_in(p, name, length, session);
    if (system != SNX_CTR_SYSTEM_UNKNOWN) return system;
    /* The console waits for the service to be registered (Azahar's GetServiceHandle). No other process runs here
       to register it, so the thread waits forever: say which service it was. */
    snx_ctr_thread_block(p, c->object, 0);
    memcpy(snx_ctr_thread(p)->waiting_service, name, length);
    snx_ctr_thread(p)->waiting_service[length] = 0;
    fprintf(stderr, "3ds srv: the runtime has no service %s. The thread that asked for it waits forever.\n",
            snx_ctr_thread(p)->waiting_service);
    c->state = SNX_CTR_IPC_BLOCKED;
    return 0;
}
