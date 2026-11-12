/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/audio_core/renderer/system_manager.cpp, renderer/system.cpp, errors in
   src/core/hle/service/audio/errors.h):
   SPDX-FileCopyrightText: Copyright 2025-2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2019-2022 yuzu Emulator Project (GPL-2.0-or-later) */
/* the audio renderers' side that audren_dsp.c doesn't do: each open renderer's state and host output, an update
   handed to the state under its lock, and the clock's frame of every started renderer (audio.c's audio_clock) queued
   to the host. The update's parsing, the output buffer's layout and the frame itself are audren_dsp.c's */
#include "nx/hle/services/services_int.h"
#include "nx/runtime/platform/tracy.h"
#include "nx/hle/services/audren_dsp.h"
#include "nx/hle/services/services.h"
#include "nx/hle/hle.h"

/* the open renderers (ARRen, services_int.h), each named by its slot in g_hle.audren.rens plus 1, with its play state
   and the parameters it reads back (nn/audio_renderer.c sets them). g_host.audren.lk guards the renderers and every
   renderer's state (an update and a frame never run at once) */

/* a frame is dropped while the host holds more than this many ms, so the latency stays bounded when the host plays a
   little slower than the clock renders */
#define AUDREN_MAX_QUEUED_MS 100

ARRen* audren_at(u32 id) {
    ARRen* r = id && id <= SNX_AUDREN_MAX ? &g_hle.audren.rens[id - 1] : NULL;
    return r && r->used ? r : NULL;
}

void audren_close(u32 id) {
    ARRen* r = audren_at(id);
    PAudio* out;
    if (!r) return;
    pm_lock(&g_host.audren.lk);
    out = r->out;
    audren_state_free(r->s); /* under the lock: its compressors go back to their pool */
    POOL_PUT(r);
    pm_unlock(&g_host.audren.lk);
    plat_audio_close(out);
}

u32 audren_open(const u8* params, int manual) {
    static int announced;
    ARState* s = audren_state_new(params);
    ARRen* r;
    u32 id;
    if (!announced && __atomic_exchange_n(&announced, 1, __ATOMIC_ACQ_REL) == 0)
        pm_lock_announce(&g_host.audren.lk, "audren"); /* the renderers' table, the game's threads and the audio's */
    if (!s) return 0;
    pm_lock(&g_host.audren.lk);
    r = POOL_TRY(g_hle.audren.rens, g_hle.audren.ren_next);
    if (r) {
        r->used = 1;
        r->state = SNX_AUDREN_STOPPED;
        r->sample_rate = snx_ld_le32(params);
        r->sample_count = snx_ld_le32(params + 4);
        r->mix_buffer_count = snx_ld_le32(params + 8);
        r->limit = 100;
        r->voice_drop = 1.0f;
        r->manual = (u32)manual;
        r->rendering_device = params[0x22];
        r->s = s;
    } else
        audren_state_free(s);
    id = r ? POOL_INDEX(g_hle.audren.rens, r) + 1 : 0;
    pm_unlock(&g_host.audren.lk);
    return id;
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
   the renderer's results (InvalidUpdateInfo 41, InsufficientBuffer 4) */
int audren_update(u32 id, u64 in, u64 in_size, u64 out, u64 out_size) {
    ARRen* r = audren_at(id);
    int rc;
    u32 rev;
    R_UNLESS(r, SNX_RC_AUDIO_INVALID_HANDLE);
    R_UNLESS(in && out && in_size >= 0x40, SNX_RC_AUDIO_INVALID_UPDATE_INFO); /* not even an update's header */
    pm_lock(&g_host.audren.lk);
    rc = audren_state_update(r->s, GP(in), in_size, GP(out), out_size);
    pm_unlock(&g_host.audren.lk);
    if (rc == -41) R_THROW(SNX_RC_AUDIO_INVALID_UPDATE_INFO);
    if (rc == -4) R_THROW(SNX_RC_AUDIO_INSUFFICIENT_BUFFER);
    R_UNLESS(rc >= 0, SNX_RC_AUDIO_OPERATION_FAILED);
    rev = snx_ld_le32(GP(in));
    LOG_LIMIT(1, "audren: the first update, revision REV%d\n", audren_revision(rev));
    R_SUCCEED();
}

void audren_tick(void) {
    i16* stereo = g_hle.audren.stereo;
    u32 i;
    pm_lock(&g_host.audren.lk);
    for (i = 0; i < SNX_AUDREN_MAX; i++) {
        ARRen* r = &g_hle.audren.rens[i];
        u32 bytes;
        if (!r->used) continue; /* a free slot */
        if (__atomic_load_n(&r->state, __ATOMIC_ACQUIRE) != SNX_AUDREN_STARTED) continue;
        if (!audren_frame(r->s, stereo)) continue;
        if (!r->out && !r->out_failed) { /* the device sink's first frame: stereo at the renderer's rate */
            r->out = plat_audio_open(r->s->rate, 2);
            r->out_failed = !r->out;
            if (r->out) plat_audio_volume(r->out, g_hle.audren.gain); /* the device's volume, set before it opened */
        }
        if (!r->out) continue;
        bytes = r->s->frame * 4;
        u32 queued = plat_audio_queued(r->out);
        /* stereo s16: the bytes the device still holds, in milliseconds of audio */
        [[maybe_unused]] double ms = (double)queued / ((double)r->s->rate * 4.0) * 1000.0;
        SNX_PLOT("audio queued ms", ms);
        /* nothing left on the device while the renderer is playing: the sound stopped for as long as the host took
           to give it the next frame, which a stalled run's audio shows as a click */
        SNX_PLOT("audio underruns", r->out_started && !queued ? 1.0 : 0.0);
        SNX_PLOT("audio queued ms", (double)queued / ((double)r->s->rate * 4.0) * 1000.0); /* stereo s16 */
        SNX_PLOT("audio underruns", r->out_started && !queued ? 1.0 : 0.0); /* the play nothing left to give it */
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
        if (!r->used) continue;
        audren_state_free(r->s);
        plat_audio_close(r->out);
        r->s = NULL;
        r->out = NULL;
    }
    pm_unlock(&g_host.audren.lk);
}
