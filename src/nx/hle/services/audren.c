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
#include "nx/runtime/platform/tracy.h"
#include "nx/hle/services/audren_dsp.h"
#include "nx/hle/services/services.h"
#include "nx/hle/hle.h"

/* the open renderers (ARRen, services_int.h), each found from its object's u[1], its slot in g_hle.audren.rens plus
   1; the object's u[0] is its play state, which audio.c sets. g_host.audren.lk guards the renderers and every
   renderer's state (an update and a frame never run at once) */

/* a frame is dropped while the host holds more than this many ms, so the latency stays bounded when the host plays a
   little slower than the clock renders */
#define AUDREN_MAX_QUEUED_MS 100

static ARRen* audren_of(const SvcObj* o) { return &g_hle.audren.rens[o->u[1] - 1]; }

static void audren_destroy(SvcObj* o) {
    ARRen* r = audren_of(o);
    PAudio* out;
    pm_lock(&g_host.audren.lk);
    out = r->out;
    audren_state_free(r->s); /* under the lock: its compressors go back to their pool */
    POOL_PUT(r);
    pm_unlock(&g_host.audren.lk);
    plat_audio_close(out);
}

int audren_open(SvcObj* o, const u8* params) {
    static int announced;
    ARState* s = audren_state_new(params);
    ARRen* r;
    if (!announced && __atomic_exchange_n(&announced, 1, __ATOMIC_ACQ_REL) == 0)
        pm_lock_announce(&g_host.audren.lk, "audren"); /* the renderers' table, the game's threads and the audio's */
    if (!s) return 0;
    pm_lock(&g_host.audren.lk);
    r = POOL_TAKE(g_hle.audren.rens, g_hle.audren.ren_next, "audio renderers");
    r->o = o;
    r->s = s;
    o->u[1] = (u64)POOL_INDEX(g_hle.audren.rens, r) + 1;
    pm_unlock(&g_host.audren.lk);
    o->destroy = audren_destroy;
    return 1;
}

void audren_volume(float gain) {
    u32 i;
    pm_lock(&g_host.audren.lk);
    g_hle.audren.gain = gain;
    for (i = 0; i < SNX_AUDREN_MAX; i++)
        if (g_hle.audren.rens[i].out) plat_audio_volume(g_hle.audren.rens[i].out, gain);
    pm_unlock(&g_host.audren.lk);
}

/* RequestUpdate: the input read into the state and the output written by audren_state_update, whose failures are
   the renderer's result descriptions (InvalidUpdateInfo 41, InsufficientBuffer 4) */
int audren_update(SvcObj* o, IpcBuf in, IpcBuf out) {
    ARRen* r = audren_of(o);
    int rc;
    u32 rev;
    R_UNLESS(in.addr && out.addr && in.size >= 0x40, RC(153, 41)); /* not even an update's header */
    pm_lock(&g_host.audren.lk);
    rc = audren_state_update(r->s, GP(in.addr), in.size, GP(out.addr), out.size);
    pm_unlock(&g_host.audren.lk);
    if (rc == -41 || rc == -4) R_THROW(RC(153, -rc));
    R_UNLESS(rc >= 0, RC(153, 2)); /* OperationFailed */
    rev = snx_ld_le32(GP(in.addr));
    /* the magic "REV" and the revision as a number (REV11 and the like) */
    LOG_LIMIT(1, "audren: the first update, revision %c%c%c%u\n", (char)(rev & 0xff), (char)(rev >> 8 & 0xff),
              (char)(rev >> 16 & 0xff), (unsigned)(rev >> 24));
    R_SUCCEED();
}

void audren_tick(void) {
    i16* stereo = g_hle.audren.stereo;
    u32 i;
    pm_lock(&g_host.audren.lk);
    for (i = 0; i < SNX_AUDREN_MAX; i++) {
        ARRen* r = &g_hle.audren.rens[i];
        u32 bytes;
        if (!r->o) continue; /* a free slot */
        if (__atomic_load_n(&r->o->u[0], __ATOMIC_ACQUIRE) != SNX_AUDREN_STARTED) continue;
        if (!audren_frame(r->s, stereo)) continue;
        if (!r->out && !r->out_failed) { /* the device sink's first frame: stereo at the renderer's rate */
            r->out = plat_audio_open(r->s->rate, 2);
            r->out_failed = !r->out;
            if (r->out) plat_audio_volume(r->out, g_hle.audren.gain); /* the device's volume, set before it opened */
        }
        if (!r->out) continue;
        bytes = r->s->frame * 4;
        u32 queued = plat_audio_queued(r->out);
#if defined(SNX_TRACY)
        SNX_PLOT("audio queued ms", (double)queued / ((double)r->s->rate * 4.0) * 1000.0); /* stereo s16 */
#endif
        if (queued > r->s->rate * 4 / 1000 * AUDREN_MAX_QUEUED_MS) continue;
        snx_le16_array(stereo, (size_t)r->s->frame * 2); /* the host plays little-endian samples, as audout's are */
        plat_audio_queue(r->out, stereo, bytes);
        if (!r->out_started) {
            r->out_started = 1;
            plat_audio_start(r->out, 1);
        }
    }
    pm_unlock(&g_host.audren.lk);
}

/* ============================================================ save states */
void audren_state_drop(void) {
    u32 i;
    pm_lock(&g_host.audren.lk);
    for (i = 0; i < SNX_AUDREN_MAX; i++) {
        ARRen* r = &g_hle.audren.rens[i];
        if (!r->o) continue;
        audren_state_free(r->s);
        plat_audio_close(r->out);
        r->s = NULL;
        r->out = NULL;
    }
    pm_unlock(&g_host.audren.lk);
}
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_audren_state_fns,
               STATE_FN("audren", audren_destroy));
