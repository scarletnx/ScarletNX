/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sm: (sm.c): the names it accepts, and RegisterService and UnregisterService answering as an application's sm does,
   with the service registry stubbed */
#include "nx/hle/services/services_int.h"
#include <setjmp.h>
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

static char test_log[1024];
void logf_(int level, const char* format, ...) {
    va_list args;
    size_t at = strlen(test_log);
    (void)level;
    if (at >= sizeof test_log - 1) return;
    va_start(args, format);
    vsnprintf(test_log + at, sizeof test_log - at, format, args);
    va_end(args);
}
/* log.c's: the stop of a program that opened a service the runtime answers only natively; its message is kept */
static jmp_buf test_stopped;
static int test_expects_stop;
static char test_stop_text[1400];
void fatal(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(test_stop_text, sizeof test_stop_text, format, args);
    va_end(args);
    if (test_expects_stop) longjmp(test_stopped, 1);
    fprintf(stderr, "%s\n", test_stop_text);
    test_fail("fatal", __LINE__);
    exit(1);
}
/* hook.c's: what the hooks left to the game's own code for its SDK version; nothing, until the test gives a text */
static const char* test_missed = "";
void hook_missed_text(char* out, size_t n) { snprintf(out, n, "%s", test_missed); }
static const char* test_unread = "";
void hook_unread_text(char* out, size_t n) { snprintf(out, n, "%s", test_unread); }
/* debug.c's: the threads and their calls, which name the SDK function that asked; counted, and before the stop */
static unsigned test_thread_dumps;
void kernel_dump_threads(void) {
    if (!test_expects_stop) test_fail("a thread dump no stop followed", __LINE__);
    test_thread_dumps++;
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

/* a TIPC request to sm's own dispatcher: cmd, and the raw data after the HIPC headers. kind adds the descriptor or
   handle of that number, which sm takes none of: 1 an A buffer, 2 an empty X descriptor, 3 a C list, 4 a copy handle */
static u32 ask_tipc(u32 cmd, const void* raw, u32 size, int kind) {
    IpcCtx c = {0};
    c.tipc = 1;
    c.cmd = cmd;
    c.raw = (const u8*)raw;
    c.raw_size = size;
    switch (kind) {
        case 1: c.na = 1; break;
        case 2: c.nx = 1; break; /* a descriptor of no size, which sf still counts */
        case 3: c.nc = 1; break;
        case 4: c.ncopy = 1; break;
        default: break;
    }
    return (u32)sm_dispatch(NULL, &c);
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

    /* a service the runtime answers only in the SDK's functions gets no session and no stand-in: the program stops,
       told which service and layer, that an SDK function is not native yet, and what a program with no SDK has to
       do. Its neighbours by name are other services */
    static const char* const native_only[] = {"fsp-srv", "fsp-pr",  "fsp-ldr", "time:u",  "time:a",  "time:s",
                                              "time:r",  "time:su", "pl:u",    "pl:s",    "pctl",    "pctl:a",
                                              "pctl:s",  "pctl:r",  "nsd:u",   "nsd:a",   "ngc:u",   "ngct:u",
                                              "prepo:u", "prepo:s", "prepo:m", "prepo:a", "prepo:a2"};
    for (size_t i = 0; i < sizeof native_only / sizeof native_only[0]; i++) {
        char opened[32], itself[40];
        snprintf(opened, sizeof opened, "opened %s,", native_only[i]);
        snprintf(itself, sizeof itself, "opens %s itself", native_only[i]);
        name = name_of(native_only[i], strlen(native_only[i]));
        test_expects_stop = 1;
        if (!setjmp(test_stopped)) {
            nn_sm_detail_IUserInterface_GetServiceHandle(NULL, &name, &handle);
            test_fail("the program went on with a session of a native-only service", __LINE__);
        }
        test_expects_stop = 0;
        CHECK(test_thread_dumps == i + 1 && strstr(test_stop_text, "the calls of its thread"));
        CHECK(strstr(test_stop_text, opened) && strstr(test_stop_text, itself));
        CHECK(strstr(test_stop_text, "nn::") && strstr(test_stop_text, "not native yet"));
        CHECK(strstr(test_stop_text, "libnx") && strstr(test_stop_text, "the word hooks in its mode file"));
        CHECK(!strcmp(strstr(test_stop_text, "its mode file"), "its mode file")); /* nothing was left for a version */
    }
    /* a game whose SDK has functions the hooks are declared for in other versions: the stop ends with which, so
       the SDK's version is in what the player reports */
    test_missed = "41 of the SDK functions run their own code: the first is f in sdk, of version 3.0.1, declared for "
                  "sdk < 1.0.0";
    name = name_of("fsp-srv", 7);
    test_expects_stop = 1;
    if (!setjmp(test_stopped)) {
        nn_sm_detail_IUserInterface_GetServiceHandle(NULL, &name, &handle);
        test_fail("the program went on with a session of a native-only service", __LINE__);
    }
    test_expects_stop = 0;
    CHECK(strstr(test_stop_text, "opened fsp-srv,"));
    CHECK(strstr(test_stop_text, "its mode file. 41 of the SDK functions run their own code: the first is f in sdk, "
                                 "of version 3.0.1, declared for sdk < 1.0.0"));
    test_missed = "";
    /* a 32-bit game of an SDK nobody read: the stop names the service it opened, then the reason and what to do, in
       place of the advice for a homebrew program */
    test_unread = "This 32-bit game's SDK is 4.4.0 (module sdk) and the runtime's declarations for a 32-bit game were "
                  "read in SDK 0.12.16 alone, so 132 of its SDK functions run their own code (nn_fs 44)";
    name = name_of("pctl", 4);
    test_expects_stop = 1;
    if (!setjmp(test_stopped)) {
        nn_sm_detail_IUserInterface_GetServiceHandle(NULL, &name, &handle);
        test_fail("the program went on with a session of a native-only service", __LINE__);
    }
    test_expects_stop = 0;
    CHECK(!strcmp(test_stop_text, "sm: the program opened pctl, which the runtime answers only in the SDK's own "
                                  "functions, nn::pctl (nn/pctl.c). This 32-bit game's SDK is 4.4.0 (module sdk) and "
                                  "the runtime's declarations for a 32-bit game were read in SDK 0.12.16 alone, so "
                                  "132 of its SDK functions run their own code (nn_fs 44)"));
    test_unread = "";
    name = name_of("time:x", 6);
    handle = 0;
    CHECK(nn_sm_detail_IUserInterface_GetServiceHandle(NULL, &name, &handle) == 0 && handle == 0x1234);
    CHECK(test_thread_dumps == sizeof native_only / sizeof native_only[0] + 2);

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

    /* The Switch 2 TIPC forms: RegisterService's name, max_sessions and is_light (16 bytes), or a service space
       ahead of them (20); UnregisterService's name (8), or a space ahead of it (12). The fields the CMIF form orders
       the other way (max_sessions before is_light) are read where TIPC puts them, and a size that is neither form,
       or a request with a buffer, is one sm can not decode */
    {
        u8 raw[24] = {0};
        u32 space = 3, max_sessions = 4, light = 1;
        memcpy(raw, "mysrv", 5);
        memcpy(raw + 8, &max_sessions, 4);
        raw[12] = light;
        test_log[0] = 0;
        CHECK(ask_tipc(2, raw, 16, 0) == RC(21, 8));
        CHECK(strstr(test_log, "mysrv (4 sessions, light)"));
        memcpy(raw, &space, 4);
        memcpy(raw + 4, "mysrv", 5);
        memcpy(raw + 12, &max_sessions, 4);
        raw[16] = light;
        test_log[0] = 0;
        CHECK(ask_tipc(2, raw, 20, 0) == RC(21, 8));
        CHECK(strstr(test_log, "service space 3") && strstr(test_log, "mysrv (4 sessions, light)"));
        CHECK(ask_tipc(2, raw, 12, 0) == 1 && ask_tipc(2, raw, 24, 0) == 1); /* neither form */
        /* the commands take no buffer, X descriptor, C list or handle, of any size */
        for (u32 kind = 1; kind <= 4; kind++) CHECK(ask_tipc(2, raw, 16, (int)kind) == 1);
        memset(raw, 0, sizeof raw);
        memcpy(raw, "set", 3);
        CHECK(ask_tipc(3, raw, 8, 0) == RC(21, 8)); /* the runtime's own service */
        memcpy(raw, "mysrv", 5);
        CHECK(ask_tipc(3, raw, 8, 0) == RC(21, 7)); /* nobody serves it */
        memcpy(raw, &space, 4);
        memcpy(raw + 4, "mysrv", 5);
        test_log[0] = 0;
        CHECK(ask_tipc(3, raw, 12, 0) == RC(21, 7));
        CHECK(strstr(test_log, "service space 3"));
        CHECK(ask_tipc(3, raw, 16, 0) == 1); /* neither form */
        /* a name sm refuses is refused in either form, not taken for another request */
        memcpy(raw + 4, "se\0t", 4);
        CHECK(ask_tipc(2, raw, 20, 0) == RC(21, 6) && ask_tipc(3, raw, 12, 0) == RC(21, 6));
    }

    printf("PASS sm_service (%u checks)\n", test_checks);
    return 0;
}
