/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/sm/sm.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* sm:, the service manager: the session to a service by its name */
#include "nx/hle/hle.h"
#include "nx/hle/services/services_int.h"

#include "hle/services/sm.swipc.inc"

#define SM_INVALID_SERVICE_NAME RC(21, 6)
#define SM_NOT_REGISTERED       RC(21, 7)
#define SM_NOT_ALLOWED          RC(21, 8)
#define SM_OUT_OF_HANDLES       0xD201 /* the kernel's OutOfHandles */

/* a name as sm reads it: 1 to 8 characters, NUL-padded, with nothing after the first NUL. Returns the length, or 0
   for a name sm refuses */
static int sm_name(const nn_sm_ServiceName* in, char out[9]) {
    int n = 0, i;
    while (n < 8 && in->v[n]) n++;
    for (i = n; i < 8; i++)
        if (in->v[i]) return 0;
    memcpy(out, in->v, (size_t)n);
    out[n] = 0;
    return n;
}

/* a service the runtime doesn't have: every command answers success (stub_ok logs it as [undeclared], or
   fails it with UnknownCommandId when auto_stub_ipc is off), under the service's own name. The console's sm would
   keep the client waiting for the service to register, which never happens here */
/* The stand-in's name, kept for the process: IPC's crash ring keeps the names of objects already gone */
static const char* stand_in_name(const char* name) {
    const char* r = "(a service missing from the runtime)";
    int i;
    pm_lock(&g_host.sm.lk);
    for (i = 0; i < g_hle.sm.n && strcmp(g_hle.sm.name[i], name); i++) {}
    if (i == g_hle.sm.n && i < STAND_IN_NAMES) strcpy(g_hle.sm.name[g_hle.sm.n++], name);
    if (i < g_hle.sm.n) r = g_hle.sm.name[i];
    pm_unlock(&g_host.sm.lk);
    return r;
}
static SvcObj* sm_stand_in(const char* name) { return svcobj_new(stand_in_name(name), stub_ok); }

static int nn_sm_detail_IUserInterface_GetServiceHandle(SvcObj* self, const nn_sm_ServiceName* name, u32* session) {
    char s[9];
    SvcObj* o;
    (void)self;
    R_UNLESS(sm_name(name, s), SM_INVALID_SERVICE_NAME);
    o = svc_open(s);
    if (!o) {
        WARN("sm: %s is not implemented, its commands answer without doing anything\n", s);
        o = sm_stand_in(s);
    }
    LOGV("sm: GetServiceHandle(%s)\n", s);
    *session = session_new(o); /* the session takes the reference, and drops it when there is no handle for it */
    return *session ? 0 : SM_OUT_OF_HANDLES;
}

/* an application's access control lets it host no service, and sm checks it before it looks the name up, so a game
   registering a service of its own is refused whatever the name (the runtime has no server ports to give it anyway) */
static int nn_sm_detail_IUserInterface_RegisterService(SvcObj* self, const nn_sm_ServiceName* name, u8 is_light,
                                                       i32 max_sessions, u32* port) {
    char s[9];
    (void)self, (void)port;
    R_UNLESS(sm_name(name, s), SM_INVALID_SERVICE_NAME);
    WARN("sm: the game registers a service of its own, %s (%d sessions%s): NotAllowed\n", s, max_sessions,
         is_light ? ", light" : "");
    R_THROW(SM_NOT_ALLOWED);
}

/* only a service's owner may unregister it, and the runtime's services belong to the system */
static int nn_sm_detail_IUserInterface_UnregisterService(SvcObj* self, const nn_sm_ServiceName* name) {
    char s[9];
    (void)self;
    R_UNLESS(sm_name(name, s), SM_INVALID_SERVICE_NAME);
    R_UNLESS(svc_registered(s), SM_NOT_REGISTERED);
    WARN("sm: the game unregisters %s, which the runtime serves: NotAllowed\n", s);
    R_THROW(SM_NOT_ALLOWED);
}

SvcObj* new_sm(void) { return nn_sm_detail_IUserInterface_new(); }
