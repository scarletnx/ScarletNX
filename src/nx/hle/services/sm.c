/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/sm/sm.cpp):
   SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project (GPL-2.0-or-later) */
/* sm:, the service manager: the session to a service by its name */
#include <string.h>
#include "nx/hle/hle.h"
#include "nx/hle/services/services_int.h"
#include "nx/runtime/platform/tracy.h"

#include "hle/services/sm.swipc.inc"

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

/* the services the runtime answers in the SDK's own functions (nn/), with no session: a game that still asks
   for one called a function there that is not native yet, and a stand-in would give its SDK objects with nothing
   behind them. A program with no SDK, which opens them itself as libnx does, has no function to answer in: the
   runtime keeps no second, IPC copy of these services for it */
static const char* const g_native_only[][3] = {
    {"fsp-srv", "nn::fs (nn/fs.c)", "nn_fs"},
    {"fsp-pr", "nn::fs (nn/fs.c)", "nn_fs"},
    {"fsp-ldr", "nn::fs (nn/fs.c)", "nn_fs"},
    {"time:u", "nn::time (nn/time.c)", "nn_time"},
    {"time:a", "nn::time (nn/time.c)", "nn_time"},
    {"time:s", "nn::time (nn/time.c)", "nn_time"},
    {"time:r", "nn::time (nn/time.c)", "nn_time"},
    {"time:su", "nn::time (nn/time.c)", "nn_time"},
    {"pl:u", "nn::pl (nn/pl.c)", "nn_pl"},
    {"pl:s", "nn::pl (nn/pl.c)", "nn_pl"},
    {"pctl", "nn::pctl (nn/pctl.c)", "nn_pctl"},
    {"pctl:a", "nn::pctl (nn/pctl.c)", "nn_pctl"},
    {"pctl:s", "nn::pctl (nn/pctl.c)", "nn_pctl"},
    {"pctl:r", "nn::pctl (nn/pctl.c)", "nn_pctl"},
    {"nsd:u", "nn::nsd (nn/nsd.c)", "nn_nsd"},
    {"nsd:a", "nn::nsd (nn/nsd.c)", "nn_nsd"},
    {"ngc:u", "nn::ngc (nn/ngc.c)", "nn_ngc"},
    {"ngct:u", "nn::ngc (nn/ngc.c)", "nn_ngc"},
    {"prepo:u", "nn::prepo (nn/prepo.c)", "nn_prepo"},
    {"prepo:s", "nn::prepo (nn/prepo.c)", "nn_prepo"},
    {"prepo:m", "nn::prepo (nn/prepo.c)", "nn_prepo"},
    {"prepo:a", "nn::prepo (nn/prepo.c)", "nn_prepo"},
    {"prepo:a2", "nn::prepo (nn/prepo.c)", "nn_prepo"},
    {"audout:u", "nn::audio (nn/audio.c)", "nn_audio_out"},
    {"audout:a", "nn::audio (nn/audio.c)", "nn_audio_out"},
    {"audout:d", "nn::audio (nn/audio.c)", "nn_audio_out"},
    {"audren:u", "nn::audio (nn/audio_renderer.c)", "nn_audio_renderer"},
    {"audren:a", "nn::audio (nn/audio_renderer.c)", "nn_audio_renderer"},
    {"audren:d", "nn::audio (nn/audio_renderer.c)", "nn_audio_renderer"},
    {"hwopus", "nn::codec (nn/codec.c)", "nn_codec_hwopus"},
    {"audin:u", "nn::audio (nn/audio.c)", "nn_audio_in"},
    {"audin:a", "nn::audio (nn/audio.c)", "nn_audio_in"},
    {"audin:d", "nn::audio (nn/audio.c)", "nn_audio_in"},
};
static int nn_sm_detail_IUserInterface_GetServiceHandle(SvcObj* self, const nn_sm_ServiceName* name, u32* session) {
    char s[9];
    SvcObj* o;
    (void)self;
    R_UNLESS(sm_name(name, s), SNX_RC_SM_INVALID_SERVICE_NAME);
    for (size_t i = 0; i < sizeof g_native_only / sizeof g_native_only[0]; i++)
        if (!strcmp(s, g_native_only[i][0])) {
            char missed[640];  /* what the hooks did not take for the game's SDK version, which may be why */
            char unread[1024]; /* a 32-bit game's SDK that no declaration was read in, which is why */
            char off[384];     /* the setting that turns its native functions off, which is why */
            hook_off_text(off, sizeof off, g_native_only[i][2]);
            hook_missed_text(missed, sizeof missed);
            hook_unread_text(unread, sizeof unread);
            /* the SDK function that asked is in the calls of the thread that sent this request */
            WARN("\n*** The program opened %s, which the runtime answers only natively ***\n", s);
            kernel_dump_threads();
            if (*off)
                fatal("sm: the program opened %s, which the runtime answers only in the SDK's own functions, %s. %s", s,
                      g_native_only[i][1], off);
            if (*unread)
                fatal("sm: the program opened %s, which the runtime answers only in the SDK's own functions, %s. %s", s,
                      g_native_only[i][1], unread);
            fatal("sm: the program opened %s, which the runtime answers only in the SDK's own functions, %s. A game "
                  "got here through an SDK function that is not native yet (the calls of its thread, in the log's "
                  "thread list before this, name it). A homebrew program that opens %s itself, as libnx does, is "
                  "not supported: it has to call the SDK's functions, as edenprobe does with the word hooks in its "
                  "mode file%s%s",
                  s, g_native_only[i][1], s, *missed ? ". " : "", missed);
        }
    o = svc_open(s);
    if (!o) {
        SNX_ZONE_T(z_sm, "svc sm stand-in", "no service %s, a stand-in answers", s);
        WARN("sm: %s is not implemented, its commands answer without doing anything\n", s);
        o = sm_stand_in(s);
    }
    LOGV("sm: GetServiceHandle(%s)\n", s);
    *session = session_new(o); /* the session takes the reference, and drops it when there is no handle for it */
    return *session ? 0 : SNX_RC_SVC_OUT_OF_HANDLES;
}

/* an application's access control lets it host no service, and sm checks it before it looks the name up, so a game
   registering a service of its own is refused whatever the name (the runtime has no server ports to give it anyway) */
static int nn_sm_detail_IUserInterface_RegisterService(SvcObj* self, const nn_sm_ServiceName* name, u8 is_light,
                                                       i32 max_sessions, u32* port) {
    char s[9];
    (void)self, (void)port;
    R_UNLESS(sm_name(name, s), SNX_RC_SM_INVALID_SERVICE_NAME);
    SNX_ZONE_T(z_sm, "svc sm error", "game register of %s refused: not allowed", s);
    WARN("sm: the game registers a service of its own, %s (%d sessions%s): NotAllowed\n", s, max_sessions,
         is_light ? ", light" : "");
    R_THROW(SNX_RC_SM_NOT_ALLOWED);
}

/* only a service's owner may unregister it, and the runtime's services belong to the system */
static int nn_sm_detail_IUserInterface_UnregisterService(SvcObj* self, const nn_sm_ServiceName* name) {
    char s[9];
    (void)self;
    R_UNLESS(sm_name(name, s), SNX_RC_SM_INVALID_SERVICE_NAME);
    R_UNLESS(svc_registered(s), SNX_RC_SM_NOT_REGISTERED);
    WARN("sm: the game unregisters %s, which the runtime serves: NotAllowed\n", s);
    R_THROW(SNX_RC_SM_NOT_ALLOWED);
}

/* The Switch 2 TIPC forms of RegisterService and UnregisterService: a service space may come first (RegisterService
   20 bytes, UnregisterService 12), and RegisterService puts max_sessions before is_light, where CMIF has is_light
   first (the SDK's TIPC command format, whose RegisterService takes name, max_sessions and is_light in that order,
   and libounce's SM client). The CMIF forms are the generated dispatcher's, below. The runtime serves one service
   space, so the space is only logged, as the SM proxy of the public CFW does */
/* sm's TIPC commands take no buffer, receive list, X descriptor or handle: the SDK's TIPC command format checks the
   message's counts against the command's, and sm's have none */
static int sm_tipc_shaped(const IpcCtx* c) {
    return !c->na && !c->nb && !c->nw && !c->nx && !c->nc && !c->ncopy;
}
static int sm_tipc_register(SvcObj* self, IpcCtx* c) {
    nn_sm_ServiceName name;
    char s[9];
    u32 space = 0, off = 0, max_sessions = 0, port = 0;
    u8 light = 0;
    if (!sm_tipc_shaped(c)) return ipc_bad_request(self, c);
    if (c->raw_size == 20) {
        ipc_in(c, 0, &space, sizeof space);
        off = 4;
    } else if (c->raw_size != 16)
        return ipc_bad_request(self, c);
    ipc_in(c, off, &name, sizeof name);
    ipc_in(c, off + 8, &max_sessions, sizeof max_sessions);
    ipc_in(c, off + 12, &light, sizeof light); /* one byte, as the SDK's client writes it */
    space = snx_le32(space), max_sessions = snx_le32(max_sessions);
    if (off && sm_name(&name, s))
        WARN("sm: the game registers %s in service space %u, which the runtime does not have: NotAllowed\n", s, space);
    i32 sessions; /* as the CMIF reader takes it: a cast of a u32 past INT32_MAX is implementation-defined */
    memcpy(&sessions, &max_sessions, sizeof sessions);
    return nn_sm_detail_IUserInterface_RegisterService(self, &name, light != 0, sessions, &port);
}
static int sm_tipc_unregister(SvcObj* self, IpcCtx* c) {
    nn_sm_ServiceName name;
    char s[9];
    u32 space = 0, off = 0;
    if (!sm_tipc_shaped(c)) return ipc_bad_request(self, c);
    if (c->raw_size == 12) {
        ipc_in(c, 0, &space, sizeof space);
        off = 4;
    } else if (c->raw_size != 8)
        return ipc_bad_request(self, c);
    ipc_in(c, off, &name, sizeof name);
    space = snx_le32(space);
    if (off && sm_name(&name, s)) WARN("sm: the game unregisters %s in service space %u: NotAllowed\n", s, space);
    return nn_sm_detail_IUserInterface_UnregisterService(self, &name);
}
/* sm's own dispatcher: the two TIPC forms above, and the generated one for the rest */
static int sm_dispatch(SvcObj* self, IpcCtx* c) {
    if (c->tipc && c->cmd == 2) return sm_tipc_register(self, c);
    if (c->tipc && c->cmd == 3) return sm_tipc_unregister(self, c);
    return nn_sm_detail_IUserInterface_dispatch(self, c);
}

SvcObj* new_sm(void) { return svcobj_new("nn::sm::detail::IUserInterface", sm_dispatch); }
/* the functions a save state may hold of this file (state_fns.h): the dispatcher above, which every sm object holds */
STATE_FN_TABLE(g_sm_state_fns, STATE_FN("sm", sm_dispatch));
