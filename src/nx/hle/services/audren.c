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
#include <stdatomic.h>

/* the open renderers (ARRen, services_int.h), each named by its slot in g_hle.audren.rens plus 1, with its play state
   and the parameters it reads back (nn/audio_renderer.c sets them). g_host.audren.lk guards the renderers and every
   renderer's state (an update and a frame never run at once) */

/* a frame is dropped while the host holds more than this many ms: past what playing frames shorter takes back */
#define AUDREN_MAX_QUEUED_MS    200
/* the log says what each playing renderer's host queue was once in this many seconds of frames */
#define AUDREN_QUEUED_LINE_S 10
/* the sound the host is left holding when a frame comes, once it held more through a whole window with no frame
   finding it empty for the calm time: four frames, what a late turn of the clock's thread may take without silence */
#define AUDREN_TARGET_QUEUED_MS 20
#define AUDREN_TARGET_WINDOW_S  1
#define AUDREN_TARGET_CALM_S    10
/* the sound is brought to the target by playing frames shorter, never by dropping one: at most this many millionths
   of a frame's samples left out (0.5%, a pitch change under a tenth of a semitone), the most once the window held
   AUDREN_RATE_SPAN_MS over the target, and at once while the host holds more than AUDREN_FAST_QUEUED_MS */
#define AUDREN_RATE_MAX_PPM     5000
#define AUDREN_RATE_SPAN_MS     20
#define AUDREN_FAST_QUEUED_MS   100

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
    static _Atomic int announced;
    ARState* s = audren_state_new(params);
    ARRen* r;
    u32 id;
    if (!atomic_load_explicit(&announced, memory_order_relaxed) &&
        atomic_exchange_explicit(&announced, 1, memory_order_acq_rel) == 0)
        pm_lock_announce(&g_host.audren.lk, "audren"); /* the renderers' table, the game's threads and the audio's */
    if (!s) return 0;
    pm_lock(&g_host.audren.lk);
    r = POOL_TRY(g_hle.audren.rens, g_hle.audren.ren_next);
    if (r) {
        r->used = 1;
        atomic_store_explicit(&r->state, SNX_AUDREN_STOPPED, memory_order_relaxed);
        r->sample_rate = snx_ld_le32(params);
        r->sample_count = snx_ld_le32(params + 4);
        r->mix_buffer_count = snx_ld_le32(params + 8);
        atomic_store_explicit(&r->limit, 100, memory_order_relaxed);
        atomic_store_explicit(&r->voice_drop, 1.0f, memory_order_relaxed);
        r->manual = (u32)manual;
        r->rendering_device = params[0x22];
        r->s = s;
        g_host.audren.queued[POOL_INDEX(g_hle.audren.rens, r)] = (AudrenQueued){0};
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

/* stereo s16 bytes of r's output as whole milliseconds of sound, the nearest */
static u32 audren_queued_ms(const ARRen* r, u64 bytes) {
    u64 ms_bytes = (u64)r->s->rate * 4;
    return (u32)((bytes * 1000 + ms_bytes / 2) / ms_bytes);
}

/* whether frames of r's are seconds of sound, or more */
static int audren_frames_last(const ARRen* r, u32 frames, u32 seconds) {
    return (u64)frames * r->s->frame >= (u64)seconds * r->s->rate;
}

/* A frame of the clock that found r's output playing with queued bytes left: -1 when it is dropped, not given to the
   host, which happens above AUDREN_MAX_QUEUED_MS; else how many of its samples to leave out. The bytes the host holds
   when a frame comes are how far the sound trails the clock, which a stall of the clock's thread leaves higher (the host
   ran out, then got the stall's frames at once) for good: so once no frame found the host empty for
   AUDREN_TARGET_CALM_S, the bytes over AUDREN_TARGET_QUEUED_MS that the host held through the whole last
   AUDREN_TARGET_WINDOW_S are played away by shortening frames, by up to AUDREN_RATE_MAX_PPM, so a host that keeps
   stalling keeps the sound its stalls need and one that stopped comes back to the target with no click. Every
   AUDREN_QUEUED_LINE_S seconds of frames the log says what the queue was. Only sums on the clock's thread between two
   lines */
static int audren_queued_frame(const ARRen* r, u32 queued) {
    u32 id = POOL_INDEX(g_hle.audren.rens, r) + 1, ms_bytes = r->s->rate * 4 / 1000, cut = 0;
    AudrenQueued* q = &g_host.audren.queued[id - 1];
    u32 span = ms_bytes * AUDREN_RATE_SPAN_MS, target = ms_bytes * AUDREN_TARGET_QUEUED_MS;
    int drop = queued > ms_bytes * AUDREN_MAX_QUEUED_MS;
    if (!q->window || queued < q->floor) q->floor = queued;
    if (!queued) {
        q->calm = 0;
        q->over = 0; /* found empty: what it held is what the stalls need */
    } else if (!audren_frames_last(r, q->calm, AUDREN_TARGET_CALM_S))
        q->calm++;
    if (audren_frames_last(r, ++q->window, AUDREN_TARGET_WINDOW_S)) {
        q->over = audren_frames_last(r, q->calm, AUDREN_TARGET_CALM_S) && q->floor > target ? q->floor - target : 0;
        q->window = 0;
    }
    if (!drop) {
        u32 over = queued > ms_bytes * AUDREN_FAST_QUEUED_MS ? span : q->over;
        u32 ppm = over >= span ? AUDREN_RATE_MAX_PPM : (u32)((u64)AUDREN_RATE_MAX_PPM * over / span);
        q->owed += (u32)((u64)r->s->frame * ppm * 65536 / 1000000);
        cut = q->owed >> 16;
        q->owed &= 0xffff;
        if (cut > r->s->frame / 2) cut = r->s->frame / 2; /* a short frame keeps half, and two samples at least */
        if (r->s->frame < 4) cut = 0;
    }
    if (!q->frames || queued < q->least) q->least = queued;
    if (queued > q->most) q->most = queued;
    q->sum += queued;
    q->empty += !queued;
    q->dropped += (u32)drop;
    q->shortened += cut > 0;
    q->cut += cut;
    if (audren_frames_last(r, ++q->frames, AUDREN_QUEUED_LINE_S)) {
        u32 tenths = (u32)(((u64)q->cut * 10000 + r->s->rate / 2) / r->s->rate);
        LOG("audren %u: the host held %u to %u ms of sound over the last %d s, %u ms on average; %u frames found it "
            "empty, %u found more than %d ms and were dropped, and %u were played shorter, %u.%u ms of sound in all, to "
            "bring it back to %d ms\n",
            id, audren_queued_ms(r, q->least), audren_queued_ms(r, q->most), AUDREN_QUEUED_LINE_S,
            audren_queued_ms(r, q->sum / q->frames), q->empty, q->dropped, AUDREN_MAX_QUEUED_MS, q->shortened,
            tenths / 10, tenths % 10, AUDREN_TARGET_QUEUED_MS);
        *q = (AudrenQueued){.window = q->window, .floor = q->floor, .calm = q->calm, .over = q->over, .owed = q->owed};
    }
    return drop ? -1 : (int)cut;
}

/* n stereo samples of in played as m (1 < m < n): each at its place on in's time, between the two samples around it,
   the first and the last kept, so a frame still meets the next where it did */
static void audren_shorten(const i16* in, u32 n, i16* out, u32 m) {
    for (u32 j = 0; j < m; j++) {
        u64 at = ((u64)j * (n - 1) << 16) / (m - 1);
        u32 k = (u32)(at >> 16), f = (u32)(at & 0xffff);
        for (u32 c = 0; c < 2; c++) {
            i64 a = in[2 * k + c], b = k + 1 < n ? in[2 * (k + 1) + c] : a;
            out[2 * j + c] = (i16)(a + (b - a) * f / 65536);
        }
    }
}

void audren_tick(void) {
    i16* stereo = g_hle.audren.stereo;
    u32 i;
    pm_lock(&g_host.audren.lk);
    for (i = 0; i < SNX_AUDREN_MAX; i++) {
        ARRen* r = &g_hle.audren.rens[i];
        u32 bytes;
        if (!r->used) continue; /* a free slot */
        if (atomic_load_explicit(&r->state, memory_order_acquire) != SNX_AUDREN_STARTED) continue;
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
        SNX_PLOT("audio queued ms", (double)queued / ((double)r->s->rate * 4.0) * 1000.0);
        /* nothing left on the device while the renderer is playing: the sound stopped for as long as the host took
           to give it the next frame, which a stalled run's audio shows as a click */
        SNX_PLOT("audio underruns", r->out_started && !queued ? 1.0 : 0.0);
        int cut = r->out_started ? audren_queued_frame(r, queued) : 0;
        if (cut < 0) continue;
        i16* play = stereo;
        u32 n = r->s->frame;
        if (cut) {
            play = g_host.audren.shorter;
            audren_shorten(stereo, n, play, n - (u32)cut);
            n -= (u32)cut;
            bytes = n * 4;
        }
        snx_le16_array(play, (size_t)n * 2); /* the host plays little-endian samples, as audout's are */
        plat_audio_queue(r->out, play, bytes);
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
