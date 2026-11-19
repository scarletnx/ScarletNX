/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* frd:u and frd:a, the friends module's account and a game's login to its game server. The account is the player's
   console's own: its files in the folder ctr_console names, laid out as Azahar's NAND folder (common/online's
   snx_online_ctr_console_load: LocalFriendCodeSeed_B, SecureInfo_A and the friends save's local account 2, the one Nimbus
   makes for Pretendo Network). With ctr_online=pretendo and the console online, Login logs in, and a game's
   RequestGameAuthentication posts NASC's LOGIN form to Pretendo's NASC, as a console with Nimbus's patches does: on a
   thread of its own, the game's event signalled when the answer came, which GetGameAuthenticationData then gives
   (the game server's address and port, the token, the server's time). A login that cannot happen (offline, ctr_online
   off, no console files) is said once with why, and the game hears NASC gave no answer: result 0 in that data. The
   friend list, presence and the rest answer success with nothing, as services/system.c answers them */
#include "3ds/services/frd.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/cfg.h"
#include "3ds/services/soc.h"
#include "3ds/services/system.h"
#include "common/network/https.h"
#include "common/platform/thread.h"
#include "common/platform/time.h"
#include "common/utilities/endian.h"
#include "common/online/ctr.h"
#include "snx_settings.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRD_FPD_VERSION 0x000F /* the friends module's version NASC's fpdver and the User-Agent carry */

enum { JOB_RUNNING, JOB_DONE, JOB_LEFT };
/* the NASC request in flight: the thread's own, freed by whichever of it and the process lets it go last */
typedef struct {
    _Atomic int state;
    char form[4096];
    char url[256];
    int status; /* the HTTP status, 0 when no answer came */
    char body[2048];
    char why[512];
} FrdJob;

struct SnxCtrFrd {
    int loaded; /* 1 the console's files read, -1 they could not be (why says it), 0 not yet tried */
    char why[640];
    SnxOnlineCtrConsole console;
    bool logged_in;
    FrdJob* job;
    uint32_t event; /* the game's completion event, a reference held while the job runs */
    uint8_t auth[SNX_ONLINE_CTR_GAME_AUTH_SIZE];
    unsigned said;
};

static struct SnxCtrFrd* state(SnxCtrProcess* p) {
    if (!p->frd) p->frd = calloc(1, sizeof *p->frd);
    return p->frd;
}
static void reply(SnxCtrIpc* c, unsigned normal, unsigned translated, uint32_t result) {
    memset(c->out, 0, sizeof c->out);
    c->out[0] = (c->in[0] & 0xffff0000u) | normal << 6 | translated;
    c->out[1] = result;
}
static void signal_object(SnxCtrProcess* p, uint32_t id) {
    if (!id || p->objects[id].type != CTR_OBJ_EVENT) return;
    p->objects[id].data.event.signaled = 1;
    snx_ctr_wake_waiters(p, id);
}

/* why the console cannot log in now, NULL when it can: the console's files read the first time they are needed */
static const char* cannot_log_in(struct SnxCtrFrd* f) {
    if (strcmp(g_settings.ctr_online_service, "pretendo"))
        return "ctr_online is off: the console logs in to no service";
    if (!snx_ctr_online()) return "the console is offline (network=0, or airplane mode)";
    if (!g_settings.ctr_console_files[0])
        return "ctr_console names no folder of the console's files: Pretendo takes a login from a console it knows";
    if (!f->loaded) {
        char why[512];
        f->loaded = snx_online_ctr_console_load(&f->console, g_settings.ctr_console_files,
                                                SNX_ONLINE_CTR_PRETENDO_ACCOUNT, why, sizeof why)
                        ? -1
                        : 1;
        if (f->loaded < 0)
            snprintf(f->why, sizeof f->why, "ctr_console=%s: %s", g_settings.ctr_console_files, why);
        else
            printf("3ds frd: the console's account, principal ID %u, from %s\n", (unsigned)f->console.pid,
                   g_settings.ctr_console_files);
    }
    return f->loaded < 0 ? f->why : NULL;
}
static void say_once(struct SnxCtrFrd* f, unsigned bit, const char* what, const char* why) {
    if (f->said & bit) return;
    f->said |= bit;
    fprintf(stderr, "3ds frd: %s: %s\n", what, why);
}

static void post(void* arg) {
    FrdJob* job = arg;
    SnxHttpsOptions o = {0};
    SnxHttpsResponse r;
    char agent[32];
    o.timeout_s = 30;
    o.body_max = sizeof job->body - 1;
    snprintf(agent, sizeof agent, "CTR FPD/%04X", FRD_FPD_VERSION);
    if (!snx_https_post(job->url, "application/x-www-form-urlencoded", job->form, strlen(job->form), agent, &o, &r,
                        job->why, sizeof job->why)) {
        job->status = r.status;
        snprintf(job->body, sizeof job->body, "%s", r.body ? r.body : "");
        free(r.body);
    }
    int running = JOB_RUNNING;
    /* release: the answer's fields before DONE, which the emulation's thread reads them after */
    if (!atomic_compare_exchange_strong_explicit(&job->state, &running, JOB_DONE, memory_order_acq_rel,
                                                 memory_order_acquire))
        free(job); /* the process left it */
}

/* RequestGameAuthentication (0x28): the server ID, the game's nickname for the player, its SDK version, the event */
static void game_authentication(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    struct SnxCtrFrd* f = state(p);
    uint32_t event = snx_ctr_handle_object(p, c->in[13]);
    if (!f || !event || p->objects[event].type != CTR_OBJ_EVENT) {
        snx_ctr_ipc_bad(c);
        return;
    }
    memset(f->auth, 0, sizeof f->auth);
    const char* why = f->job ? "a login is already on its way" : cannot_log_in(f);
    FrdJob* job = why ? NULL : calloc(1, sizeof *job);
    if (!why && !job) why = "no memory for the request";
    char form_why[256];
    if (job) {
        SnxOnlineCtrGame g = {0};
        SnxOnlineCtrPlace place = {0};
        time_t now = (time_t)(plat_unix_time() + plat_tz_offset()); /* the console's clock: local time */
        struct tm t;
        g.game_server_id = c->in[1];
        g.title_id = p->title_id;
        g.sdk_major = (uint8_t)c->in[8];
        g.sdk_minor = (uint8_t)c->in[9];
        g.media_type = 1; /* digital: a card's login would want the card's own ID (Azahar's GetProgramLaunchInfo) */
        for (unsigned i = 0; i < 11; ++i) g.ingamesn[i] = (uint16_t)(c->in[2 + i / 2] >> (i & 1) * 16);
        place.language = (uint8_t)snx_ctr_cfg_language(p);
        place.fpd_version = FRD_FPD_VERSION;
#ifdef _WIN32
        gmtime_s(&t, &now);
#else
        gmtime_r(&now, &t);
#endif
        strftime(place.devtime, sizeof place.devtime, "%y%m%d%H%M%S", &t);
        snx_ctr_cfg_username(place.devname);
        if (snx_online_nasc_login_form(job->form, sizeof job->form, &f->console, &g, &place, form_why,
                                       sizeof form_why)) {
            free(job);
            job = NULL;
            why = form_why;
        }
    }
    if (job) {
        snx_https_init(); /* libcurl's global setup, here on the emulation's thread before the request's */
        snprintf(job->url, sizeof job->url, "%s",
                 g_settings.ctr_nasc_url[0] ? g_settings.ctr_nasc_url : SNX_ONLINE_PRETENDO_NASC_URL);
        atomic_init(&job->state, JOB_RUNNING);
        if (plat_thread(post, job, 0)) {
            free(job);
            job = NULL;
            why = "no thread for the request";
        }
    }
    if (!job) { /* answered at once: no NASC answer, result 0 */
        say_once(f, 1, "a game's login to its server does not go out", why);
        signal_object(p, event);
    } else {
        f->job = job;
        f->event = event;
        ++p->objects[event].refs;
    }
    reply(c, 1, 0, 0);
}

void snx_ctr_frd_poll(SnxCtrProcess* p) {
    struct SnxCtrFrd* f = p->frd;
    if (!f || !f->job || atomic_load_explicit(&f->job->state, memory_order_acquire) != JOB_DONE) return;
    FrdJob* job = f->job;
    SnxOnlineNascReply r;
    char why[256];
    f->job = NULL;
    memset(f->auth, 0, sizeof f->auth);
    if (!job->status)
        fprintf(stderr, "3ds frd: the game's login: %s\n", job->why);
    else if (snx_online_nasc_reply_read(job->body, strlen(job->body), &r, why, sizeof why))
        fprintf(stderr, "3ds frd: the game's login: %s answered HTTP %d, %s\n", job->url, job->status, why);
    else {
        snx_online_ctr_game_auth(f->auth, &r, job->status);
        if (snx_online_nasc_result(&r) == 1)
            printf("3ds frd: the game's login: its server is %s:%u\n", r.host, r.port);
        else
            fprintf(stderr, "3ds frd: the game's login: NASC answered returncd %s (retry %d)\n", r.returncd, r.retry);
    }
    if (f->auth[0] == 0 && job->status) snx_st_le32(f->auth + 4, (uint32_t)job->status);
    free(job);
    signal_object(p, f->event);
    snx_ctr_object_release(p, f->event);
    f->event = 0;
}

bool snx_ctr_frd_waiting(const SnxCtrProcess* p) { return p->frd && p->frd->job; }

void snx_ctr_frd_dispatch(SnxCtrIpc* c) {
    SnxCtrProcess* p = c->process;
    struct SnxCtrFrd* f = state(p);
    uint32_t command = c->in[0] >> 16;
    if (!f) {
        reply(c, 1, 0, 0xd86007f3u);
        return;
    }
    switch (command) {
        case 0x0001: /* HasLoggedIn */
        case 0x0002: /* IsOnline */
            reply(c, 2, 0, 0);
            c->out[2] = f->logged_in && snx_ctr_online();
            break;
        case 0x0003: { /* Login: its event signalled at once */
            const char* why = cannot_log_in(f);
            f->logged_in = !why;
            if (why) say_once(f, 2, "the friends module does not log in", why);
            signal_object(p, snx_ctr_handle_object(p, c->in[2]));
            reply(c, 1, 0, 0);
            break;
        }
        case 0x0004: /* Logout */
            f->logged_in = false;
            reply(c, 1, 0, 0);
            break;
        case 0x0005: /* GetMyFriendKey: the account's principal ID and local friend code, zeros without one */
            (void)cannot_log_in(f);
            reply(c, 5, 0, 0);
            if (f->loaded > 0) {
                c->out[2] = f->console.pid;
                c->out[4] = (uint32_t)f->console.friend_code;
                c->out[5] = (uint32_t)(f->console.friend_code >> 32);
            }
            break;
        case 0x0028: game_authentication(c); break;
        case 0x0029: { /* GetGameAuthenticationData: into the caller's static buffer 0 */
            uint8_t d[8];
            uint32_t tls = p->cpu.state.cpu.tpidruro;
            reply(c, 1, 2, 0);
            if (snx_ctr_read_ram(p->space, tls + 0x180, d, 8) != SNX_CTR_MEM_OK) {
                c->transport_result = 0xe0e01bf5u;
                break;
            }
            uint32_t desc = snx_ld_le32(d), address = snx_ld_le32(d + 4);
            uint32_t size = desc >> 14 < SNX_ONLINE_CTR_GAME_AUTH_SIZE ? desc >> 14 : SNX_ONLINE_CTR_GAME_AUTH_SIZE;
            if ((desc & 0xf) != 2 || snx_ctr_write_ram(p->space, address, f->auth, size) != SNX_CTR_MEM_OK) {
                snx_ctr_ipc_bad(c);
                break;
            }
            c->out[2] = size << 14 | 2;
            c->out[3] = address;
            break;
        }
        default: snx_ctr_system_unmodeled(c, "frd:u", (SNX_CTR_SERVICE_FRD << 16) | command); break;
    }
}

void snx_ctr_frd_close(SnxCtrProcess* p) {
    struct SnxCtrFrd* f = p->frd;
    if (!f) return;
    if (f->job) {
        int running = JOB_RUNNING;
        if (!atomic_compare_exchange_strong_explicit(&f->job->state, &running, JOB_LEFT, memory_order_acq_rel,
                                                     memory_order_acquire))
            free(f->job); /* it had ended */
        snx_ctr_object_release(p, f->event);
    }
    free(f);
    p->frd = NULL;
}
