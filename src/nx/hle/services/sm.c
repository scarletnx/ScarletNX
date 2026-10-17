/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/sm/sm.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* sm:, the service manager: the session to a service by its name */
#include "nx/hle/services/services_int.h"

#include "hle/services/sm.swipc.inc"

#define SM_ALREADY_REGISTERED   RC(21, 4)
#define SM_INVALID_SERVICE_NAME RC(21, 6)
#define SM_NOT_REGISTERED       RC(21, 7)
#define SM_NOT_IMPLEMENTED      0x4201 /* the kernel's NotImplemented */
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
static SvcObj* sm_stand_in(const char* name) {
    SvcObj* o = svcobj_new(name, stub_ok, 9);
    strcpy((char*)o->data, name);
    o->name = (const char*)o->data;
    return o;
}

static int nn_sm_detail_IUserInterface_GetServiceHandle(SvcObj* self, const nn_sm_ServiceName* name, u32* session) {
    char s[9];
    SvcObj* o;
    (void)self;
    if (!sm_name(name, s)) return SM_INVALID_SERVICE_NAME;
    o = svc_open(s);
    if (!o) {
        WARN("sm: %s is not implemented, its commands answer without doing anything\n", s);
        o = sm_stand_in(s);
    }
    LOGV("sm: GetServiceHandle(%s)\n", s);
    *session = session_new(o); /* the session takes the reference, and drops it when there is no handle for it */
    return *session ? 0 : SM_OUT_OF_HANDLES;
}

/* the runtime has no server ports for the game to accept sessions on, so a service of the game's own can't be
   served: the name is refused if it is taken, and otherwise the request fails */
static int nn_sm_detail_IUserInterface_RegisterService(SvcObj* self, const nn_sm_ServiceName* name, u8 is_light,
                                                       i32 max_sessions, u32* port) {
    char s[9];
    (void)self, (void)port;
    if (!sm_name(name, s)) return SM_INVALID_SERVICE_NAME;
    if (svc_registered(s)) return SM_ALREADY_REGISTERED;
    WARN("sm: the game registers a service of its own, %s (%d sessions%s), which isn't supported\n", s, max_sessions,
         is_light ? ", light" : "");
    return SM_NOT_IMPLEMENTED;
}

/* the runtime's own services stay: only the game's could be unregistered, and it has none */
static int nn_sm_detail_IUserInterface_UnregisterService(SvcObj* self, const nn_sm_ServiceName* name) {
    char s[9];
    (void)self;
    if (!sm_name(name, s)) return SM_INVALID_SERVICE_NAME;
    if (!svc_registered(s)) return SM_NOT_REGISTERED;
    WARN("sm: the game unregisters %s, which the runtime serves: it stays\n", s);
    return 0;
}

SvcObj* new_sm(void) { return nn_sm_detail_IUserInterface_new(0); }
