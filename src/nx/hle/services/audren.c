/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/audio_core/renderer/system_manager.cpp, renderer/system.cpp, errors in
   src/core/hle/service/audio/errors.h):
   SPDX-FileCopyrightText: Copyright 2025-2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2019-2022 yuzu Emulator Project (GPL-2.0-or-later) */
/* the audio renderers' side that audren_dsp.c doesn't do: each open renderer's state and host output, RequestUpdate
   handed to the state under its lock, and the clock's frame of every started renderer (audio.c's audio_clock) queued
   to the host. The update's parsing, the output buffer's layout and the frame itself are audren_dsp.c's */
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/audren_dsp.h"

/* an open renderer, found from its object's u[1]; the object's u[0] is its play state, which audio.c sets */
typedef struct ARRen {
    struct ARRen* next; /* in the list the clock renders */
    SvcObj* o;
    ARState* s;
    PAudio* out;   /* NULL without a host device */
    int out_started;
} ARRen;

/* the renderers, and every renderer's state: an update and a frame never run at once */
static PMutex audren_lk = PMUTEX_INIT;
static ARRen* audren_list;

/* host output queued past this many ms is dropped, so the latency stays bounded when the host plays a little slower
   than the clock renders */
#define AUDREN_MAX_QUEUED_MS 60

static void audren_destroy(SvcObj* o) {
    ARRen* r = (ARRen*)(size_t)o->u[1];
    ARRen** p;
    pm_lock(&audren_lk);
    for (p = &audren_list; *p; p = &(*p)->next)
        if (*p == r) {
            *p = r->next;
            break;
        }
    pm_unlock(&audren_lk);
    plat_audio_close(r->out);
    audren_state_free(r->s);
    free(r);
}

int audren_open(SvcObj* o, const u8* params) {
    ARRen* r = calloc(1, sizeof *r);
    if (!r) return 0;
    r->s = audren_state_new(params);
    if (!r->s) {
        free(r);
        return 0;
    }
    r->o = o;
    r->out = plat_audio_open(r->s->rate, 2);
    o->u[1] = (u64)(size_t)r;
    o->destroy = audren_destroy;
    pm_lock(&audren_lk);
    r->next = audren_list;
    audren_list = r;
    pm_unlock(&audren_lk);
    return 1;
}

/* RequestUpdate: the input read into the state and the output written by audren_state_update, whose failures are
   the renderer's result descriptions (InvalidUpdateInfo 41, InsufficientBuffer 4) */
int audren_update(SvcObj* o, IpcBuf in, IpcBuf out) {
    ARRen* r = (ARRen*)(size_t)o->u[1];
    int rc;
    if (!in.addr || !out.addr) return RC(153, 41);
    pm_lock(&audren_lk);
    rc = audren_state_update(r->s, GP(in.addr), in.size, GP(out.addr), out.size);
    pm_unlock(&audren_lk);
    return rc < 0 ? RC(153, -rc) : 0;
}

void audren_tick(void) {
    static i16 stereo[2 * 1024]; /* a frame is at most 1024 samples (audren_state_new); only the clock's thread */
    ARRen* r;
    pm_lock(&audren_lk);
    for (r = audren_list; r; r = r->next) {
        u32 bytes;
        if (__atomic_load_n(&r->o->u[0], __ATOMIC_ACQUIRE) != SNX_AUDREN_STARTED) continue;
        if (!audren_frame(r->s, stereo) || !r->out) continue;
        bytes = r->s->frame * 4;
        if (plat_audio_queued(r->out) > r->s->rate * 4 / 1000 * AUDREN_MAX_QUEUED_MS) continue;
        plat_audio_queue(r->out, stereo, bytes);
        if (!r->out_started) {
            r->out_started = 1;
            plat_audio_start(r->out, 1);
        }
    }
    pm_unlock(&audren_lk);
}
