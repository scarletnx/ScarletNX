/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* nn::audio's output streams of the Ounce's SDK (OpenOutputStream, WriteOutputStream...). Its SDK plays them through
   the audio device manager (auddmg:u) and the audio DSP, which this runtime doesn't have: the functions the game calls
   run here instead, on the host's audio output (plat_audio_*).

   A stream holds a ring of capacity frames, which WriteOutputStream fills as far as there is room. The audio clock
   (audio.c's audio_clock, every 5 ms) plays a started stream in guest time at its rate, as the DSP would: the frames
   played leave the ring for the host's output, converted to signed 16-bit, and the stream's system event, whose
   readable handle OpenOutputStream attaches to the game's nn::os::SystemEvent, is signaled for the room they freed.
   States as the SDK's OutputStreamImpl keeps them: 0 closed, 1 stopped, 2 started. The state is the runtime's, not
   the guest's: a save state doesn't hold it.

   The input streams (OpenInputStream, ReadInputStream...) are the console's microphone, through the same manager: one
   virtual input device, whose started streams fill at their rate in guest time, as the DSP would record, up to their
   capacity. The host's microphone isn't read yet, so the frames are silence. The devices the SDK asks auddmg:u about
   (ListVirtualInputDevices, GetVirtualOutputDeviceProperties) are answered here too */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services_int.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define STREAMS_MAX    16
#define STREAM_STOPPED 1
#define STREAM_STARTED 2
#define STREAM_FAILED  0x1ee6f /* a refused call: an error of codec.c's, for this file's too */
/* nn::audio::SampleFormat: PcmInt8 1, PcmInt16 2, PcmInt24 3, PcmInt32 4, PcmFloat 5 */
#define FORMAT_FLOAT   5

typedef struct {
    u64 guest;
    KEvent* event;
    PAudio* out;
    u32 rate, channels, format, sample_bytes, capacity; /* capacity: frames the ring holds */
    i16* ring;
    u32 head, filled; /* the oldest frame, and how many are in the ring */
    u32 state;
    float gain;
    u64 last, carry; /* the clock's last turn (guest ns), and the remainder of frames it owes */
    u64 played, underruns;
    int input; /* a microphone's stream: no ring, filled counts the frames recorded */
} Stream;

static PMutex g_lk;
static int g_lk_ready;
static Stream g_streams[STREAMS_MAX];

static void lock(void) {
    int ready = __atomic_load_n(&g_lk_ready, __ATOMIC_ACQUIRE);
    if (ready != 2) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&g_lk_ready, &expected, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            pm_init(&g_lk);
            __atomic_store_n(&g_lk_ready, 2, __ATOMIC_RELEASE);
        } else
            while (__atomic_load_n(&g_lk_ready, __ATOMIC_ACQUIRE) != 2) {}
    }
    pm_lock(&g_lk);
}
static void unlock(void) { pm_unlock(&g_lk); }

static Stream* stream_of(u64 guest) {
    for (u32 i = 0; i < STREAMS_MAX; i++)
        if (g_streams[i].guest == guest && guest && !g_streams[i].input) return &g_streams[i];
    return NULL;
}
static Stream* input_of(u64 guest) {
    for (u32 i = 0; i < STREAMS_MAX; i++)
        if (g_streams[i].guest == guest && guest && g_streams[i].input) return &g_streams[i];
    return NULL;
}

/* the default virtual output device's name, which the game hands back to OpenOutputStream */
u32 audio_default_virtual_output(CPU* c, u64 name) {
    static const char device[] = "VirtualAudioOut";
    (void)c;
    if (!name) return STREAM_FAILED;
    memset(GP(name), 0, 256);
    memcpy(GP(name), device, sizeof device);
    return 0;
}

/* the virtual input device: the console's microphone, one of it, whose name the game hands back to OpenInputStream */
#define INPUT_DEVICE "VirtualAudioIn"
i32 audio_list_virtual_inputs(CPU* c, u64 names, i32 count) {
    (void)c;
    if (!names || count < 1) return 0;
    memset(GP(names), 0, 256);
    memcpy(GP(names), INPUT_DEVICE, sizeof INPUT_DEVICE);
    return 1;
}
/* VirtualDeviceProperties: the name, the latency in ns at 0x100 and the channels at 0x108, as the SDK copies them from
   auddmg:u's property block (Eglantine's answer to it: stereo, 10 ms). A game sets its streams up by the channels */
u32 audio_virtual_output_properties(CPU* c, u64 props, u64 name) {
    u8* p = GP(props);
    (void)c;
    if (!props || !name) return STREAM_FAILED;
    memmove(p, GP(name), 256);
    p[255] = 0;
    snx_st_le64(p + 0x100, 10000000ull);
    snx_st_le32(p + 0x108, 2);
    return 0;
}

static u32 open_stream(CPU* c, u64 stream, u64 event, u64 format, i32 capacity) {
    Stream* s;
    u32 f[3];
    if (!stream || !format || capacity <= 0) return STREAM_FAILED;
    memcpy(f, GP(format), sizeof f); /* sample format, channels, rate */
    if (f[0] < 1 || f[0] > FORMAT_FLOAT || f[1] < 1 || f[1] > 8 || f[2] < 8000 || f[2] > 192000) {
        WARN_LIMIT(4, "audio: an output stream of format %u, %u channels at %u Hz, which this runtime doesn't play\n",
                   f[0], f[1], f[2]);
        return STREAM_FAILED;
    }
    lock();
    s = stream_of(stream);
    if (!s)
        for (u32 i = 0; i < STREAMS_MAX && !s; i++)
            if (!g_streams[i].guest) s = &g_streams[i];
    if (!s) {
        unlock();
        WARN_LIMIT(4, "audio: more than %d output streams\n", STREAMS_MAX);
        return STREAM_FAILED;
    }
    free(s->ring);
    if (s->out) plat_audio_close(s->out);
    *s = (Stream){0};
    s->guest = stream;
    s->format = f[0];
    s->channels = f[1];
    s->rate = f[2];
    s->sample_bytes = f[0] == FORMAT_FLOAT || f[0] == 4 ? 4 : f[0];
    s->capacity = (u32)capacity;
    s->ring = calloc((size_t)s->capacity * s->channels, sizeof *s->ring);
    s->state = STREAM_STOPPED;
    s->gain = 1.0f;
    unlock();
    if (!s->ring) return STREAM_FAILED;
    s->out = plat_audio_open(s->rate, s->channels > 2 ? 2 : s->channels);
    if (event) s->event = hook_attach_event(c, event, "audio output stream");
    LOG("audio: output stream %llx: %u channels at %u Hz, sample format %u, %u frames%s\n", (unsigned long long)stream,
        s->channels, s->rate, s->format, s->capacity, s->out ? "" : ", no host output (silent)");
    return 0;
}
/* OpenOutputStream(stream, event, work, size, device, format, capacity, threshold, flags): the work buffer is the
   SDK's, unused here */
u32 audio_open_stream_event(CPU* c, u64 stream, u64 event, u64 work, u64 size, u64 device, u64 format, i32 capacity,
                            i32 threshold) {
    (void)work, (void)size, (void)device, (void)threshold;
    return open_stream(c, stream, event, format, capacity);
}
/* OpenOutputStream(stream, work, size, device, format, capacity, flags) */
u32 audio_open_stream(CPU* c, u64 stream, u64 work, u64 size, u64 device, u64 format, i32 capacity, u32 flags) {
    (void)work, (void)size, (void)device, (void)flags;
    return open_stream(c, stream, 0, format, capacity);
}
void audio_close_stream(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = stream_of(stream);
    if (s) {
        if (s->out) plat_audio_close(s->out);
        if (s->event) kobj_unref(&s->event->o);
        free(s->ring);
        *s = (Stream){0};
    }
    unlock();
}
u32 audio_start_stream(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = stream_of(stream);
    if (s && s->state != STREAM_STARTED) {
        s->state = STREAM_STARTED;
        s->last = guest_ns();
        s->carry = 0;
        plat_audio_start(s->out, 1);
    }
    unlock();
    return s ? 0 : STREAM_FAILED;
}
u32 audio_stop_stream(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = stream_of(stream);
    if (s && s->state == STREAM_STARTED) {
        s->state = STREAM_STOPPED;
        plat_audio_start(s->out, 0);
    }
    unlock();
    return s ? 0 : STREAM_FAILED;
}
/* the frames waiting in the ring dropped */
u32 audio_purge_stream(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = stream_of(stream);
    if (s) {
        s->head = s->filled = 0;
        plat_audio_clear(s->out);
    }
    unlock();
    return s ? 0 : STREAM_FAILED;
}
u32 audio_stream_state(CPU* c, u64 stream) {
    Stream* s;
    u32 state;
    (void)c;
    lock();
    s = stream_of(stream);
    state = s ? s->state : 0;
    unlock();
    return state;
}
i32 audio_stream_free_frames(CPU* c, u64 stream) {
    Stream* s;
    i32 n = 0;
    (void)c;
    lock();
    s = stream_of(stream);
    if (s) n = (i32)(s->capacity - s->filled);
    unlock();
    return n;
}
void audio_set_stream_gain(CPU* c, u64 stream, f32 gain, i64 fade) {
    Stream* s;
    (void)c, (void)fade;
    lock();
    s = stream_of(stream);
    if (s) s->gain = gain < 0 ? 0 : gain;
    unlock();
}
/* the statistics, returned by value through x8: zeros, as the SDK clears them before asking the DSP */
void audio_stream_statistics(CPU* c, u64 stream, int reset) {
    (void)stream, (void)reset;
    if (c->x[8]) memset(GP(c->x[8]), 0, 0x18);
}

i32 audio_stream_filled_frames(CPU* c, u64 stream) {
    Stream* s;
    i32 n = 0;
    (void)c;
    lock();
    s = stream_of(stream);
    if (s) n = (i32)s->filled;
    unlock();
    return n;
}

/* ------------------------------------------------------------ input streams: the microphone */
static u32 open_input(CPU* c, u64 stream, u64 event, u64 format, i32 capacity) {
    Stream* s;
    u32 f[3];
    if (!stream || !format || capacity <= 0) return STREAM_FAILED;
    memcpy(f, GP(format), sizeof f); /* sample format, channels, rate */
    if (f[0] < 1 || f[0] > FORMAT_FLOAT || f[1] < 1 || f[1] > 8 || f[2] < 8000 || f[2] > 192000) {
        WARN_LIMIT(4, "audio: an input stream of format %u, %u channels at %u Hz, which this runtime doesn't record\n",
                   f[0], f[1], f[2]);
        return STREAM_FAILED;
    }
    lock();
    s = input_of(stream);
    if (!s)
        for (u32 i = 0; i < STREAMS_MAX && !s; i++)
            if (!g_streams[i].guest) s = &g_streams[i];
    if (!s) {
        unlock();
        WARN_LIMIT(4, "audio: more than %d streams\n", STREAMS_MAX);
        return STREAM_FAILED;
    }
    if (s->event) kobj_unref(&s->event->o);
    *s = (Stream){0};
    s->guest = stream;
    s->input = 1;
    s->format = f[0];
    s->channels = f[1];
    s->rate = f[2];
    s->sample_bytes = f[0] == FORMAT_FLOAT || f[0] == 4 ? 4 : f[0];
    s->capacity = (u32)capacity;
    s->state = STREAM_STOPPED;
    unlock();
    if (event) s->event = hook_attach_event(c, event, "audio input stream");
    LOG("audio: input stream %llx: %u channels at %u Hz, sample format %u, %u frames, silence (no host microphone)\n",
        (unsigned long long)stream, s->channels, s->rate, s->format, s->capacity);
    return 0;
}
/* OpenInputStream(stream, work, size, device, format, capacity): the work buffer is the SDK's, unused here */
u32 audio_open_input(CPU* c, u64 stream, u64 work, u64 size, u64 device, u64 format, i32 capacity) {
    (void)work, (void)size, (void)device;
    return open_input(c, stream, 0, format, capacity);
}
/* OpenInputStream(stream, event, work, size, device, format, capacity, threshold) */
u32 audio_open_input_event(CPU* c, u64 stream, u64 event, u64 work, u64 size, u64 device, u64 format, i32 capacity,
                           i32 threshold) {
    (void)work, (void)size, (void)device, (void)threshold;
    return open_input(c, stream, event, format, capacity);
}
void audio_close_input(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = input_of(stream);
    if (s) {
        if (s->event) kobj_unref(&s->event->o);
        *s = (Stream){0};
    }
    unlock();
}
u32 audio_start_input(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = input_of(stream);
    if (s && s->state != STREAM_STARTED) {
        s->state = STREAM_STARTED;
        s->last = guest_ns();
        s->carry = 0;
    }
    unlock();
    return s ? 0 : STREAM_FAILED;
}
u32 audio_stop_input(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = input_of(stream);
    if (s) s->state = STREAM_STOPPED;
    unlock();
    return s ? 0 : STREAM_FAILED;
}
/* the frames recorded and not read dropped */
u32 audio_purge_input(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = input_of(stream);
    if (s) s->filled = 0;
    unlock();
    return s ? 0 : STREAM_FAILED;
}
u32 audio_input_state(CPU* c, u64 stream) {
    Stream* s;
    u32 state;
    (void)c;
    lock();
    s = input_of(stream);
    state = s ? s->state : 0;
    unlock();
    return state;
}
/* which: 0 the frames recorded and not read, 1 the room left, 2 the capacity */
static i32 input_count(u64 stream, int which) {
    Stream* s;
    i32 n = 0;
    lock();
    s = input_of(stream);
    if (s) n = (i32)(which == 0 ? s->filled : which == 1 ? s->capacity - s->filled : s->capacity);
    unlock();
    return n;
}
i32 audio_input_filled_frames(CPU* c, u64 stream) {
    (void)c;
    return input_count(stream, 0);
}
i32 audio_input_free_frames(CPU* c, u64 stream) {
    (void)c;
    return input_count(stream, 1);
}
i32 audio_input_capacity_frames(CPU* c, u64 stream) {
    (void)c;
    return input_count(stream, 2);
}
/* as many recorded frames as asked for and there are, in the stream's format: the count read */
i32 audio_read_input(CPU* c, u64 stream, u64 data, i32 frames) {
    Stream* s;
    i32 n = 0;
    (void)c;
    if (frames <= 0 || !data) return 0;
    lock();
    s = input_of(stream);
    if (s) {
        n = (i32)((u32)frames < s->filled ? (u32)frames : s->filled);
        memset(GP(data), 0, (size_t)n * s->sample_bytes * s->channels);
        s->filled -= (u32)n;
    }
    unlock();
    return n;
}

static i16 sample_s16(const u8* p, u32 format, float gain) {
    float v;
    switch (format) {
        case 1: v = (float)(i8)p[0] / 128.0f; break;
        case 2: v = (float)(i16)snx_ld_le16(p) / 32768.0f; break;
        case 3: v = (float)((i32)((u32)p[0] << 8 | (u32)p[1] << 16 | (u32)p[2] << 24) >> 8) / 8388608.0f; break;
        case 4: v = (float)(i32)snx_ld_le32(p) / 2147483648.0f; break;
        default: {
            u32 bits = snx_ld_le32(p);
            memcpy(&v, &bits, 4);
            if (isnan(v)) v = 0;
        }
    }
    v *= gain;
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (i16)lrintf(v * 32767.0f);
}
/* as many of the frames as the ring has room for, converted: the count written */
i32 audio_write_stream(CPU* c, u64 stream, u64 data, i32 frames) {
    Stream* s;
    i32 n = 0;
    (void)c;
    if (frames <= 0 || !data) return 0;
    lock();
    s = stream_of(stream);
    if (s) {
        u32 room = s->capacity - s->filled, frame_bytes = s->sample_bytes * s->channels;
        const u8* in = GP(data);
        n = (i32)((u32)frames < room ? (u32)frames : room);
        for (i32 i = 0; i < n; i++) {
            i16* to = s->ring + (size_t)((s->head + s->filled + (u32)i) % s->capacity) * s->channels;
            for (u32 ch = 0; ch < s->channels; ch++)
                to[ch] = sample_s16(in + (size_t)i * frame_bytes + (size_t)ch * s->sample_bytes, s->format, s->gain);
        }
        s->filled += (u32)n;
    }
    unlock();
    return n;
}

/* The audio clock's turn (audio.c): each started stream plays the frames its rate gives the guest time since its last
   turn, out of the ring to the host's output (the first two channels), and its event is signaled for the room */
void outstream_tick(u64 now) {
    static i16 out[48000 / 20 * 2 * 2];
    if (!__atomic_load_n(&g_lk_ready, __ATOMIC_ACQUIRE)) return;
    lock();
    for (u32 i = 0; i < STREAMS_MAX; i++) {
        Stream* s = &g_streams[i];
        u64 owed, want;
        u32 n, k, wide;
        if (!s->guest || s->state != STREAM_STARTED) continue;
        owed = (now > s->last ? now - s->last : 0) * s->rate + s->carry;
        s->last = now;
        want = owed / 1000000000ull;
        s->carry = owed % 1000000000ull;
        if (s->input) { /* recorded: the frames its rate gives, as far as its capacity holds them */
            s->filled = want < s->capacity - s->filled ? s->filled + (u32)want : s->capacity;
            if (want && s->event) kevent_signal(s->event);
            continue;
        }
        n = want < s->filled ? (u32)want : s->filled;
        if (want > s->filled) s->underruns++;
        wide = s->channels > 2 ? 2 : s->channels;
        while (n) {
            u32 chunk = n < sizeof out / sizeof *out / wide ? n : (u32)(sizeof out / sizeof *out / wide);
            for (k = 0; k < chunk; k++) {
                const i16* from = s->ring + (size_t)((s->head + k) % s->capacity) * s->channels;
                memcpy(out + (size_t)k * wide, from, wide * sizeof *out);
            }
            plat_audio_queue(s->out, out, chunk * wide * (u32)sizeof *out);
            s->head = (s->head + chunk) % s->capacity;
            s->filled -= chunk;
            s->played += chunk;
            n -= chunk;
        }
        if (s->event) kevent_signal(s->event);
    }
    unlock();
}
