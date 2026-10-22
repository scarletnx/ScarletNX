/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm: (sm.c): the names it accepts, and RegisterService and UnregisterService answering as an application's sm does,
   with the service registry stubbed */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>
#include <stdio.h>

#include "nx/hle/services/sm.c"
#include "nx/hle/hle.c"

u8* g_mem;
static unsigned test_checks;
static SvcObj test_service;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "sm_service.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void logf_(int level, const char* format, ...) {
    (void)level;
    (void)format;
}
/* the runtime has one service here, "set" */
int svc_registered(const char* name) { return !strcmp(name, "set"); }
SvcObj* svc_open(const char* name) { return svc_registered(name) ? &test_service : NULL; }
u32 session_new(SvcObj* obj) {
    (void)obj;
    return 0x1234;
}
SvcObj* svcobj_new(const char* name, int (*dispatch)(SvcObj*, IpcCtx*)) {
    static SvcObj o;
    (void)dispatch;
    o.name = name;
    return &o;
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void out_move_h(IpcCtx* c, u32 handle) {
    (void)c, (void)handle;
}
void ipc_out_overflow(IpcCtx* c) {
    (void)c;
    test_fail("IPC output overflow", __LINE__);
}
int ipc_bad_request(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 1;
}
int ipc_bad_objects(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 1;
}
int stub_ok(SvcObj* o, IpcCtx* c) {
    (void)o, (void)c;
    return 0;
}

static nn_sm_ServiceName name_of(const char* s, size_t n) {
    nn_sm_ServiceName name;
    memset(&name, 0, sizeof name);
    memcpy(name.v, s, n);
    return name;
}

void pm_init(PMutex* m) { (void)m; }

int main(void) {
    hle_init();
    nn_sm_ServiceName name;
    u32 handle = 0, port = 0;

    /* names: one to eight characters, NUL-padded; empty, or anything after the first NUL, is InvalidServiceName */
    name = name_of("set", 3);
    CHECK(nn_sm_detail_IUserInterface_GetServiceHandle(NULL, &name, &handle) == 0 && handle == 0x1234);
    name = name_of("", 0);
    CHECK(nn_sm_detail_IUserInterface_GetServiceHandle(NULL, &name, &handle) == RC(21, 6));
    name = name_of("se\0t", 4);
    CHECK(nn_sm_detail_IUserInterface_GetServiceHandle(NULL, &name, &handle) == RC(21, 6));
    CHECK(nn_sm_detail_IUserInterface_RegisterService(NULL, &name, 0, 1, &port) == RC(21, 6));
    CHECK(nn_sm_detail_IUserInterface_UnregisterService(NULL, &name) == RC(21, 6));
    /* a service the runtime doesn't have still gets a session, to a stand-in under its name */
    name = name_of("abcdefgh", 8);
    handle = 0;
    CHECK(nn_sm_detail_IUserInterface_GetServiceHandle(NULL, &name, &handle) == 0 && handle == 0x1234);

    /* an application may host no service: NotAllowed, whether or not the name is taken */
    name = name_of("mysrv", 5);
    CHECK(nn_sm_detail_IUserInterface_RegisterService(NULL, &name, 0, 4, &port) == RC(21, 8) && !port);
    name = name_of("set", 3);
    CHECK(nn_sm_detail_IUserInterface_RegisterService(NULL, &name, 1, 4, &port) == RC(21, 8));
    /* unregistering: NotRegistered for a name nobody serves, NotAllowed for the runtime's own */
    name = name_of("mysrv", 5);
    CHECK(nn_sm_detail_IUserInterface_UnregisterService(NULL, &name) == RC(21, 7));
    name = name_of("set", 3);
    CHECK(nn_sm_detail_IUserInterface_UnregisterService(NULL, &name) == RC(21, 8));

    printf("PASS sm_service (%u checks)\n", test_checks);
    return 0;
}
