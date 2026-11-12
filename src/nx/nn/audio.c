/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/audio/audio_out.cpp, errors.h; src/audio_core/out/audio_out_system.cpp,
   device/audio_buffers.h):
   SPDX-FileCopyrightText: Copyright 2025-2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018-2024 yuzu Emulator Project (GPL-2.0-or-later) */
/* nn::audio's audio outs, native (audio.hook): OpenAudioOut and the functions that take the AudioOut it filled, with
   no audout:u session. What an out does is what the console's does, as the service's port had it: it plays at 48 kHz,
   16-bit, with the channels asked for when they are 1, 2, 4 or 6 and two otherwise, and releases its buffers on the
   audio clock (audio.c's audio_clock) in guest time, whether or not the host has an audio device. Games sync against
   that release (a movie player waits for its buffers back).

   The SDK keeps an AudioOut's rate, channels, sample format and name in the game's struct and reads them there, in
   code a game may have inlined, so OpenAudioOut writes them as the SDK does (the layout is audio.hook's, the same in
   every SDK read, 0.12 to 20.5). Everything else the SDK asks its session for: here an out is found by the address
   of the game's AudioOut, and its state is the runtime's (g_hle.audio.outs_pool, which a save state holds).

   The buffer event is a kernel event attached to the game's nn::os::SystemEvent through the SDK's own
   AttachReadableHandleToSystemEvent, as the SDK attaches the handle RegisterBufferEvent answers.

   Where the SDK stops the game (a failed result from Stop, GetReleasedAudioOutBuffer and the like), nothing here
   fails. An AudioOut that was never opened makes the SDK read a session it doesn't have: here the call is logged and
   answers as a stopped, empty out */
#include "nx/nn/audio.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services_int.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"

#define OUT_NAME_SIZE    0x100 /* an AudioOutInfo, and the name in an AudioOut */
/* an AudioOutBuffer, which the SDK hands its session whole: the next buffer, the samples' address, their capacity
   and size, and the offset into them, 8 bytes each in a 64-bit and a 32-bit game alike */
#define OUT_BUFFER_SIZE  0x28
#define OUT_STARTED      0 /* nn::audio::AudioOutState */
#define OUT_STOPPED      1
#define OUT_PCM_INT16    2 /* nn::audio::SampleFormat */

static const char out_name[] = "DeviceOut";

/* frames of 48 kHz in ns, without overflowing for days of uptime */
static u64 frames_in(u64 ns) {
    return ns / 1000000000u * SNX_AUDIO_OUT_RATE + ns % 1000000000u * SNX_AUDIO_OUT_RATE / 1000000000u;
}

/* plays the queued buffers up to now, releasing the ones finished. Returns how many were. A started out with nothing
   queued is idle: the clock starts over from the next append */
static u32 out_advance(AudOut* q, u64 now) {
    u32 released = 0;
    u64 avail;
    if (!q->started || !q->nqueued) return 0;
    avail = frames_in(now - q->t0) - q->t0_frames;
    while (q->nqueued) {
        u32 i = (q->first + q->nreleased) % AUDOUT_BUFFERS;
        u32 left = q->frames[i] - q->head_played;
        u32 n = avail < left ? (u32)avail : left;
        q->head_played += n;
        q->t0_frames += n;
        q->played += n;
        avail -= n;
        if (q->head_played < q->frames[i]) break;
        q->head_played = 0;
        q->nreleased++;
        q->nqueued--;
        released++;
    }
    if (!q->nqueued) {
        q->t0 = now;
        q->t0_frames = 0;
    }
    return released;
}

/* hands every queued buffer back, unplayed: Stop and Flush */
static u32 out_release_all(AudOut* q) {
    u32 n = q->nqueued;
    q->nreleased += n;
    q->nqueued = 0;
    q->head_played = 0;
    plat_audio_clear(q->output);
    return n;
}

void nnaudio_out_tick(u64 now) {
    u32 i;
    pm_lock(&g_host.audio.lk);
    for (i = 0; i < AUDOUT_MAX; i++) {
        AudOut* q = &g_hle.audio.outs_pool[i];
        if (q->guest && out_advance(q, now)) ev_signal(q->ev);
    }
    pm_unlock(&g_host.audio.lk);
}

/* the out of the game's AudioOut at guest, under the lock, which the caller holds from here whether there is one or
   not: its state brought up to now. NULL for an AudioOut that isn't open, which what names in the log */
static AudOut* out_lock(u64 guest, const char* what) {
    AudOut* q = NULL;
    u32 i;
    pm_lock(&g_host.audio.lk);
    for (i = 0; i < AUDOUT_MAX && !q; i++)
        if (guest && g_hle.audio.outs_pool[i].guest == guest) q = &g_hle.audio.outs_pool[i];
    if (!q) {
        if (what)
            WARN_LIMIT(8, "audio: %s on the AudioOut at %#llx, which isn't open\n", what, (unsigned long long)guest);
        return NULL;
    }
    if (out_advance(q, guest_ns())) ev_signal(q->ev);
    return q;
}

/* a new buffer event attached to the game's SystemEvent: its id, with a reference of the out's beside the handle's,
   or 0 when the SDK exports no function to attach it with */
static u32 attach_event(CPU* c, u64 event) {
    KEvent* e = hook_attach_event(c, event, "audio out buffers");
    if (!e) return 0;
    kobj_ref(&e->o);
    return kobj_id(&e->o);
}
static void drop_event(u32 ev) {
    KObj* o = kobj_at(ev);
    if (o) kobj_unref(o);
}

/* ------------------------------------------------------------ opening and closing */
static u32 open_out(CPU* c, const HookLayout_nn_audio_AudioOut* L, u64 out, u64 event, u64 name, u64 params) {
    char want[OUT_NAME_SIZE];
    u32 channels, n = 0;
    AudOut* q;
    PAudio* host;
    memset(want, 0, sizeof want);
    if (name)
        while (n < sizeof want - 1 && (want[n] = (char)*hook_host(c, name + n)) != 0) n++;
    if (want[0] && strcmp(want, out_name)) {
        WARN_LIMIT(4, "audio: OpenAudioOut names the device %s, and the only one is %s: NotFound\n", want, out_name);
        return SNX_RC_AUDIO_NOT_FOUND;
    }
    switch (snx_ld_le16(hook_host(c, params + 4))) { /* an AudioOutParameter: the rate, then the channels */
        case 1: channels = 1; break;
        case 4: channels = 4; break;
        case 6: channels = 6; break;
        default: channels = 2; break;
    }
    /* the SDK's own results, with no session asked: an AudioOut opened twice, and more outs than it has room for */
    q = out_lock(out, NULL);
    if (q) {
        pm_unlock(&g_host.audio.lk);
        return SNX_RC_AUDIO_ALREADY_OPEN;
    }
    q = POOL_TRY(g_hle.audio.outs_pool, g_hle.audio.out_next);
    if (!q) {
        pm_unlock(&g_host.audio.lk);
        WARN_LIMIT(4, "audio: OpenAudioOut with all %d audio outs open: the SDK's result for its own limit (%#x)\n",
                   AUDOUT_MAX, SNX_RC_AUDIO_TOO_MANY_OPEN);
        return SNX_RC_AUDIO_TOO_MANY_OPEN;
    }
    q->guest = out;
    q->channels = channels;
    q->volume = 1.0f;
    pm_unlock(&g_host.audio.lk);
    /* outside the lock: the host's device may take long to open, and the event's attach runs the game's code */
    host = plat_audio_open(SNX_AUDIO_OUT_RATE, channels);
    pm_lock(&g_host.audio.lk);
    q->output = host;
    plat_audio_volume(host, q->volume * g_hle.audio.device_volume);
    pm_unlock(&g_host.audio.lk);
    hook_put_u32(c, L->sample_rate, out, SNX_AUDIO_OUT_RATE);
    hook_put_u32(c, L->channel_count, out, channels);
    hook_put_u32(c, L->sample_format, out, OUT_PCM_INT16);
    hook_put_u32(c, L->state, out, OUT_STOPPED);
    memset(hook_host(c, out + L->name.offset), 0, OUT_NAME_SIZE);
    memcpy(hook_host(c, out + L->name.offset), out_name, sizeof out_name);
    if (event) nnaudio_register_buffer_event(c, out, event);
    LOG("audio: audio out %llx: %u channels at %u Hz, 16-bit%s\n", (unsigned long long)out, channels,
        SNX_AUDIO_OUT_RATE, host ? "" : ", no host output (played on the clock alone)");
    return 0;
}
u32 nnaudio_open_out(CPU* c, const HookLayout_nn_audio_AudioOut* L, u64 out, u64 name, u64 params) {
    return open_out(c, L, out, 0, name, params);
}
u32 nnaudio_open_out_event(CPU* c, const HookLayout_nn_audio_AudioOut* L, u64 out, u64 event, u64 name, u64 params) {
    return open_out(c, L, out, event, name, params);
}
u32 nnaudio_open_default_out(CPU* c, const HookLayout_nn_audio_AudioOut* L, u64 out, u64 params) {
    return open_out(c, L, out, 0, 0, params);
}
u32 nnaudio_open_default_out_event(CPU* c, const HookLayout_nn_audio_AudioOut* L, u64 out, u64 event, u64 params) {
    return open_out(c, L, out, event, 0, params);
}

/* the buffer event, one for an out: a second SystemEvent gets a handle to the same event */
void nnaudio_register_buffer_event(CPU* c, u64 out, u64 event) {
    AudOut* q = out_lock(out, "RegisterBufferEvent");
    u32 ev = q ? q->ev : 0;
    pm_unlock(&g_host.audio.lk);
    if (!q || !event) return;
    if (!ev) {
        ev = attach_event(c, event);
        if (!ev) {
            WARN("audio: the SDK exports no AttachReadableHandleToSystemEvent: the audio out at %#llx has no buffer "
                 "event, and a game that waits for one waits forever\n",
                 (unsigned long long)out);
            return;
        }
        q = out_lock(out, NULL);
        if (q && !q->ev) {
            q->ev = ev;
            ev = 0;
        }
        pm_unlock(&g_host.audio.lk);
        if (ev) drop_event(ev); /* closed, or given an event, while the game's code ran */
        return;
    }
    hook_attach_handle(c, event, ev_handle(ev));
}

/* the SDK marks the game's AudioOut stopped, then lets its session go */
void nnaudio_close_out(CPU* c, const HookLayout_nn_audio_AudioOut* L, u64 out) {
    AudOut* q = out_lock(out, "CloseAudioOut");
    PAudio* host = NULL;
    u32 ev = 0;
    if (q) {
        host = q->output;
        ev = q->ev;
        POOL_PUT(q);
    }
    pm_unlock(&g_host.audio.lk);
    if (!q) return;
    hook_put_u32(c, L->state, out, OUT_STOPPED);
    plat_audio_close(host);
    drop_event(ev);
}

/* ------------------------------------------------------------ playing */
/* the state of an open out is its session's; one that isn't open reads what OpenAudioOut and CloseAudioOut left in
   the game's struct, as the SDK does */
u32 nnaudio_get_out_state(CPU* c, const HookLayout_nn_audio_AudioOut* L, u64 out) {
    AudOut* q = out_lock(out, NULL);
    u32 state = q ? (q->started ? OUT_STARTED : OUT_STOPPED) : hook_get_u32(c, L->state, out);
    pm_unlock(&g_host.audio.lk);
    return state;
}

/* an out already started is refused, as Eden's and Ryujinx's are */
u32 nnaudio_start_out(CPU* c, u64 out) {
    AudOut* q = out_lock(out, "StartAudioOut");
    u32 rc = SNX_RC_AUDIO_OPERATION_FAILED;
    (void)c;
    if (q && !q->started) {
        q->started = 1;
        q->t0 = guest_ns();
        q->t0_frames = 0;
        plat_audio_start(q->output, 1);
        rc = 0;
    }
    pm_unlock(&g_host.audio.lk);
    return rc;
}

/* what wasn't played comes back released, the host's copy dropped (Eden's System::Stop) */
void nnaudio_stop_out(CPU* c, u64 out) {
    AudOut* q = out_lock(out, "StopAudioOut");
    (void)c;
    if (q && q->started) {
        q->started = 0;
        plat_audio_start(q->output, 0);
        out_release_all(q);
        ev_signal(q->ev);
    }
    pm_unlock(&g_host.audio.lk);
}

/* the AudioOutBuffer's samples copied to the host now, so the guest owns its memory again once the buffer is
   released; its length in frames sets when that is. False for a full ring, as the SDK answers BufferRingFull */
int nnaudio_append_out_buffer(CPU* c, u64 out, u64 buffer) {
    u64 d[OUT_BUFFER_SIZE / 8], samples, capacity, size, offset;
    AudOut* q;
    u32 i;
    if (!buffer || !snx_audio_guest_ok(buffer, OUT_BUFFER_SIZE, PERM_R)) {
        WARN_LIMIT(4,
                   "audio: AppendAudioOutBuffer with an AudioOutBuffer at %#llx, which isn't readable: not appended\n",
                   (unsigned long long)buffer);
        return 0;
    }
    for (i = 0; i < OUT_BUFFER_SIZE / 8; i++) d[i] = snx_ld_le64(hook_host(c, buffer + 8 * i));
    samples = d[1], capacity = d[2], size = d[3], offset = d[4];
    q = out_lock(out, "AppendAudioOutBuffer");
    if (!q || q->nreleased + q->nqueued >= AUDOUT_BUFFERS) {
        pm_unlock(&g_host.audio.lk);
        return 0;
    }
    if (q->started && !q->nqueued) { /* idle until now */
        q->t0 = guest_ns();
        q->t0_frames = 0;
    }
    i = (q->first + q->nreleased + q->nqueued) % AUDOUT_BUFFERS;
    q->tag[i] = buffer;
    q->frames[i] = (u32)((size < 0xFFFFFFFFu ? size : 0xFFFFFFFFu) / (q->channels * 2u));
    q->nqueued++;
    if (q->output && q->frames[i]) { /* without a host device it is played on the clock alone */
        /* an offset past the capacity is no place in the buffer (a game leaves the field unset): the buffer plays
           from its start, as Eden and yuzu play every buffer, which never read the field */
        if (offset > capacity) {
            WARN_LIMIT(4, "audio: buffer %#llx's data offset %#llx is past its capacity %llu: played from offset 0\n",
                       (unsigned long long)samples, (unsigned long long)offset, (unsigned long long)capacity);
            offset = 0;
        }
        if (size <= 0xFFFFFFFFu && size <= capacity - offset && samples + offset >= samples &&
            snx_audio_guest_ok(samples + offset, size, PERM_R))
            /* the frames the clock plays, a part of one at the end left out: SDL refuses it, and the buffer with it */
            plat_audio_queue(q->output, hook_host(c, samples + offset), q->frames[i] * q->channels * 2u);
        else
            WARN_LIMIT(4, "audio: buffer %#llx (%llu bytes at offset %llu) isn't readable, played as silence\n",
                       (unsigned long long)samples, (unsigned long long)size, (unsigned long long)offset);
    }
    pm_unlock(&g_host.audio.lk);
    return 1;
}

/* the oldest released buffer, one a call as the SDK asks its session for one: 0 when none is */
u64 nnaudio_get_released_out_buffer(CPU* c, u64 out) {
    AudOut* q = out_lock(out, "GetReleasedAudioOutBuffer");
    u64 buffer = 0;
    (void)c;
    if (q && q->nreleased) {
        buffer = q->tag[q->first];
        q->first = (q->first + 1) % AUDOUT_BUFFERS;
        q->nreleased--;
    }
    pm_unlock(&g_host.audio.lk);
    return buffer;
}

/* appended and not yet taken back, played or not */
int nnaudio_contains_out_buffer(CPU* c, u64 out, u64 buffer) {
    AudOut* q = out_lock(out, "ContainsAudioOutBuffer");
    int contains = 0;
    u32 i;
    (void)c;
    for (i = 0; q && i < q->nreleased + q->nqueued; i++)
        if (q->tag[(q->first + i) % AUDOUT_BUFFERS] == buffer) contains = 1;
    pm_unlock(&g_host.audio.lk);
    return contains;
}

i32 nnaudio_get_out_buffer_count(CPU* c, u64 out) {
    AudOut* q = out_lock(out, "GetAudioOutBufferCount");
    i32 count = q ? (i32)q->nqueued : 0;
    (void)c;
    pm_unlock(&g_host.audio.lk);
    return count;
}

i64 nnaudio_get_out_played_sample_count(CPU* c, u64 out) {
    AudOut* q = out_lock(out, "GetAudioOutPlayedSampleCount");
    i64 count = q ? (i64)q->played : 0;
    (void)c;
    pm_unlock(&g_host.audio.lk);
    return count;
}

/* a stopped out keeps what was appended until it starts: only a started one is flushed, and says so */
int nnaudio_flush_out_buffers(CPU* c, u64 out) {
    AudOut* q = out_lock(out, "FlushAudioOutBuffers");
    int flushed = q && q->started;
    (void)c;
    if (flushed) {
        if (out_release_all(q)) ev_signal(q->ev);
        q->t0 = guest_ns();
        q->t0_frames = 0;
    }
    pm_unlock(&g_host.audio.lk);
    return flushed;
}

void nnaudio_set_out_volume(CPU* c, u64 out, f32 volume) {
    AudOut* q = out_lock(out, "SetAudioOutVolume");
    (void)c;
    if (q) {
        q->volume = volume;
        plat_audio_volume(q->output, volume * g_hle.audio.device_volume);
    }
    pm_unlock(&g_host.audio.lk);
}

f32 nnaudio_get_out_volume(CPU* c, u64 out) {
    AudOut* q = out_lock(out, "GetAudioOutVolume");
    f32 volume = q ? q->volume : 1.0f;
    (void)c;
    pm_unlock(&g_host.audio.lk);
    return volume;
}

/* the one device, into an array of AudioOutInfo (a name each) when it has room for one: the count */
i32 nnaudio_list_outs(CPU* c, u64 infos, i32 count) {
    if (!infos || count < 1) return 0;
    memset(hook_host(c, infos), 0, OUT_NAME_SIZE);
    memcpy(hook_host(c, infos), out_name, sizeof out_name);
    return 1;
}

/* ============================================================ the device's volume, and save states */
void nnaudio_out_device_volume(void) {
    u32 i;
    for (i = 0; i < AUDOUT_MAX; i++) {
        AudOut* q = &g_hle.audio.outs_pool[i];
        if (q->guest) plat_audio_volume(q->output, q->volume * g_hle.audio.device_volume);
    }
}

void nnaudio_out_state_drop(void) {
    u32 i;
    for (i = 0; i < AUDOUT_MAX; i++) {
        AudOut* q = &g_hle.audio.outs_pool[i];
        if (q->guest && q->output) plat_audio_close(q->output);
        q->output = NULL;
    }
}
/* the outs play from the loaded state's clock, their host streams empty: what was queued there is the old game's */
void nnaudio_out_state_rebuild(void) {
    u32 i;
    for (i = 0; i < AUDOUT_MAX; i++) {
        AudOut* q = &g_hle.audio.outs_pool[i];
        if (!q->guest) continue;
        q->output = plat_audio_open(SNX_AUDIO_OUT_RATE, q->channels);
        plat_audio_volume(q->output, q->volume * g_hle.audio.device_volume);
        if (q->started) plat_audio_start(q->output, 1);
    }
}
