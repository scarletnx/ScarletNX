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
   capacity. Their frames are the host's microphone (the microphone setting: the system's default recording device,
   another by its name, or off for silence), recorded only while a stream is started. The console's audio system has a
   noise reduction for its microphone, which a system setting switches (how strong it is isn't known). Here the noise
   is reduced too, by a neural network or spectrally (audio_denoise.h), as the microphone noise reduction setting says,
   read for each block of 10 ms. The devices the SDK asks auddmg:u about
   (the virtual input devices and the devices' properties) are answered here too */
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services_int.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/audio_denoise.h"
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
/* the format's offset in the SDK's input stream object */
#define FORMAT_AT      0x114

/* An input stream's recording: the host's microphone (no device: silence), its samples reduced in blocks of 10 ms, and
   the reduced samples that wait for guest time to take them. A device delivers in bursts (of 10 to 20 ms), so the
   audio clock takes what guest time owes only once MIC_HOLD_MS wait, which keeps a burst late by a few ms from leaving
   a gap. A recording that runs short gives silence and waits until it holds that much again, and 10 ms more each
   time it runs short, up to 200 ms. One that runs ahead (guest time slower than the host's, or stopped by a pause)
   drops its oldest samples past the hold + MIC_SLACK_MS, which keeps the delay from growing */
#define MIC_HOLD_MS  40
#define MIC_SLACK_MS 60
typedef struct {
    PMic* dev;
    SnxDenoise* denoise; /* NULL: the samples pass as recorded */
    int asked;           /* the method the reducer was made for (the setting's value then) */
    u32 rate;
    u32 block, pending_n; /* the samples of a block, and how many of the next one are recorded so far */
    i16* pending;         /* the next block */
    i16* ready;           /* a ring of size samples, count of them from head */
    u32 size, head, count;
    u32 hold, slack; /* MIC_HOLD_MS (and what shorts added) and MIC_SLACK_MS in samples */
    int primed;      /* hold was reached, and nothing ran short since */
    u64 shorts, dropped;
} Mic;

typedef struct {
    u64 guest;
    KEvent* event;
    PAudio* out;
    u32 rate, channels, format, sample_bytes, capacity; /* capacity: frames the ring holds */
    i16* ring;                                          /* an input's: mono frames, which a read gives every channel */
    u32 head, filled;                                   /* the oldest frame, and how many are in the ring */
    u32 state;
    float gain;
    u64 last, carry; /* the clock's last turn (guest ns), and the remainder of frames it owes */
    u64 played, underruns;
    int input; /* a microphone's stream: filled counts the frames recorded and not read */
    Mic* mic;  /* an input's recording while it is started */
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
/* the default virtual input device's name */
u32 audio_default_virtual_input(CPU* c, u64 name) {
    (void)c;
    if (!name) return STREAM_FAILED;
    memset(GP(name), 0, 256);
    memcpy(GP(name), INPUT_DEVICE, sizeof INPUT_DEVICE);
    return 0;
}
/* VirtualDeviceProperties: the name, the latency in ns at 0x100 and the channels at 0x108, as the SDK copies them from
   auddmg:u's property block, the same copy for an output and an input device (Eglantine's answer for the output:
   stereo, 10 ms). A game sets its streams up by the channels, and one game sizes its input's ring by the latency */
static u32 device_properties(u64 props, u64 name) {
    u8* p = GP(props);
    if (!props || !name) return STREAM_FAILED;
    memmove(p, GP(name), 256);
    p[255] = 0;
    snx_st_le64(p + 0x100, 10000000ull);
    snx_st_le32(p + 0x108, 2);
    return 0;
}
u32 audio_virtual_output_properties(CPU* c, u64 props, u64 name) {
    (void)c;
    return device_properties(props, name);
}
/* the microphone's: its mono recording goes to every channel a stream asks for */
u32 audio_virtual_input_properties(CPU* c, u64 props, u64 name) {
    (void)c;
    return device_properties(props, name);
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
static void mic_free(Mic* m) {
    if (!m) return;
    plat_mic_close(m->dev);
    snx_denoise_free(m->denoise);
    free(m->pending);
    free(m->ready);
    free(m);
}
/* the noise reduction a recording runs, and when it is not the one mic_noise_reduction asks for, that value and why:
   the neural one at another rate than it takes, or the spectral one without the system's library */
static void mic_reduction(const Mic* m, char* out, size_t size) {
    static const char* const method[] = {"off", "neural", "spectral"};
    int runs = snx_denoise_method(m->denoise), asked = (unsigned)m->asked < 3 ? m->asked : SNX_DENOISE_OFF;
    if (runs == asked) {
        snprintf(out, size, "%s", method[runs]);
        return;
    }
    if (asked == SNX_DENOISE_NEURAL && m->rate != SNX_DENOISE_NEURAL_RATE)
        snprintf(
            out, size,
            "%s, not neural as mic_noise_reduction=%d asks: neural takes %u Hz and the stream records at %u Hz%s%s",
            method[runs], m->asked, (unsigned)SNX_DENOISE_NEURAL_RATE, m->rate,
            runs == SNX_DENOISE_OFF ? ", and spectral needs " : "", runs == SNX_DENOISE_OFF ? SNX_DENOISE_LIBRARY : "");
    else if (asked == SNX_DENOISE_SPECTRAL)
        snprintf(out, size, "%s, not spectral as mic_noise_reduction=%d asks: spectral needs %s", method[runs],
                 m->asked, SNX_DENOISE_LIBRARY);
    else
        snprintf(out, size, "%s, not %s as mic_noise_reduction=%d asks: its reducer could not be made", method[runs],
                 method[asked], m->asked);
}
/* a recording at rate Hz, from the device the microphone setting names, with its reducer: a recording with no device
   (the setting is off, or the host has none) gives silence. Called outside the lock: a device takes a while to open */
static Mic* mic_open(u32 rate) {
    Mic* m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->block = rate / 100;
    m->size = rate / 2;
    m->hold = (u32)((u64)rate * MIC_HOLD_MS / 1000);
    m->slack = (u32)((u64)rate * MIC_SLACK_MS / 1000);
    m->pending = calloc(m->block, sizeof *m->pending);
    m->ready = calloc(m->size, sizeof *m->ready);
    if (!m->pending || !m->ready) {
        mic_free(m);
        return NULL;
    }
    if (strcmp(g_settings.microphone, "off")) m->dev = plat_mic_open(rate, g_settings.microphone);
    m->rate = rate;
    m->asked = __atomic_load_n(&g_settings.microphone_noise_reduction, __ATOMIC_RELAXED);
    if (m->dev) m->denoise = snx_denoise_new(rate, m->block, m->asked);
    /* said for the first starts only: a game may start and stop its stream for each recording */
    if (!strcmp(g_settings.microphone, "off"))
        LOG_LIMIT(8, "audio: an input stream started with the microphone off: the game hears silence\n");
    else if (!m->dev)
        LOG_LIMIT(8, "audio: an input stream started with no host microphone: the game hears silence\n");
    else {
        char said[192];
        mic_reduction(m, said, sizeof said);
        LOG_LIMIT(8, "audio: an input stream records the host's microphone at %u Hz, noise reduction %s\n", rate, said);
    }
    return m;
}
/* n samples to the ready ring, its oldest dropped when it is full */
static void mic_push(Mic* m, const i16* x, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (m->count == m->size) {
            m->head = (m->head + 1) % m->size;
            m->count--;
            m->dropped++;
        }
        m->ready[(m->head + m->count) % m->size] = x[i];
        m->count++;
    }
}
/* a whole block: reduced as the setting has it now. A change of method makes a new reducer, which learns the noise
   again in a second or so */
static void mic_block(Mic* m) {
    int want = __atomic_load_n(&g_settings.microphone_noise_reduction, __ATOMIC_RELAXED);
    if (want != m->asked) {
        char said[192];
        snx_denoise_free(m->denoise);
        m->denoise = snx_denoise_new(m->rate, m->block, want);
        m->asked = want;
        mic_reduction(m, said, sizeof said);
        LOG_LIMIT(8, "audio: the microphone's noise reduction is now %s\n", said);
    }
    snx_denoise_run(m->denoise, m->pending);
    mic_push(m, m->pending, m->block);
    m->pending_n = 0;
}
/* what the device recorded since the last turn, through the blocks into the ready ring. More than the ring holds (a
   long stall of the clock) loses its oldest at once */
static void mic_pump(Mic* m) {
    i16 skip[1024];
    u32 avail = plat_mic_available(m->dev);
    while (avail > m->size) {
        u32 got = plat_mic_read(m->dev, skip, avail - m->size < 1024 ? avail - m->size : 1024);
        if (!got) break;
        avail -= got;
        m->dropped += got;
    }
    while (avail) {
        u32 room = m->block - m->pending_n, got;
        got = plat_mic_read(m->dev, m->pending + m->pending_n, avail < room ? avail : room);
        if (!got) break;
        avail -= got;
        m->pending_n += got;
        if (m->pending_n == m->block) mic_block(m);
    }
}
/* the next n samples of the recording, or silence where it has none (no device, still filling up, or short) */
static void mic_take(Mic* m, i16* out, u32 n) {
    u32 k = 0;
    if (m && m->dev) {
        mic_pump(m);
        if (!m->primed && m->count >= m->hold) m->primed = 1;
        if (m->primed) {
            k = n < m->count ? n : m->count;
            for (u32 i = 0; i < k; i++) out[i] = m->ready[(m->head + i) % m->size];
            m->head = (m->head + k) % m->size;
            m->count -= k;
            if (k < n) { /* a device that delivers in larger bursts: hold 10 ms more, up to 200 ms */
                m->primed = 0;
                if (m->hold + m->block <= m->size * 2 / 5) m->hold += m->block;
                if (!m->shorts++)
                    LOG_LIMIT(8, "audio: the host's microphone ran short of guest time: silence until it catches up\n");
            }
        }
        if (m->count > m->hold + m->slack) {
            u32 drop = m->count - m->hold;
            m->head = (m->head + drop) % m->size;
            m->count -= drop;
            m->dropped += drop;
        }
    }
    memset(out + k, 0, (size_t)(n - k) * sizeof *out);
}

static u32 open_input(CPU* c, u64 stream, u64 event, u64 format, i32 capacity) {
    Stream* s;
    Mic* old;
    u32 f[3];
    i16* ring;
    if (!stream || !format || capacity <= 0) return STREAM_FAILED;
    memcpy(f, GP(format), sizeof f); /* sample format, channels, rate */
    if (f[0] < 1 || f[0] > FORMAT_FLOAT || f[1] < 1 || f[1] > 8 || f[2] < 8000 || f[2] > 192000) {
        WARN_LIMIT(4, "audio: an input stream of format %u, %u channels at %u Hz, which this runtime doesn't record\n",
                   f[0], f[1], f[2]);
        return STREAM_FAILED;
    }
    ring = calloc((size_t)capacity, sizeof *ring);
    if (!ring) return STREAM_FAILED;
    lock();
    s = input_of(stream);
    if (!s)
        for (u32 i = 0; i < STREAMS_MAX && !s; i++)
            if (!g_streams[i].guest) s = &g_streams[i];
    if (!s) {
        unlock();
        free(ring);
        WARN_LIMIT(4, "audio: more than %d streams\n", STREAMS_MAX);
        return STREAM_FAILED;
    }
    if (s->event) kobj_unref(&s->event->o);
    old = s->mic;
    free(s->ring);
    *s = (Stream){0};
    s->guest = stream;
    s->input = 1;
    s->format = f[0];
    s->channels = f[1];
    s->rate = f[2];
    s->sample_bytes = f[0] == FORMAT_FLOAT || f[0] == 4 ? 4 : f[0];
    s->capacity = (u32)capacity;
    s->ring = ring;
    s->state = STREAM_STOPPED;
    s->gain = 1.0f;
    /* the format where the SDK's own format query finds it, as the SDK's open leaves it: the SDK builds its stream
       object in the game's input stream at +8, keeps that address at +0, and the object holds the format at
       FORMAT_AT. A module recompiled before audio_input_format was hooked calls the SDK's query directly, and it
       reads the format there too */
    memmove(GP(stream + 8 + FORMAT_AT), GP(format), 16);
    snx_st_le64(GP(stream), stream + 8);
    unlock();
    mic_free(old);
    if (event) s->event = hook_attach_event(c, event, "audio input stream");
    LOG("audio: input stream %llx: %u channels at %u Hz, sample format %u, %u frames\n", (unsigned long long)stream,
        s->channels, s->rate, s->format, s->capacity);
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
/* a stream's format, where its open put it */
u64 audio_input_format(CPU* c, u64 stream) {
    (void)c;
    return stream ? stream + 8 + FORMAT_AT : 0;
}
void audio_close_input(CPU* c, u64 stream) {
    Stream* s;
    Mic* old = NULL;
    i16* ring = NULL;
    (void)c;
    lock();
    s = input_of(stream);
    if (s) {
        if (s->event) kobj_unref(&s->event->o);
        old = s->mic;
        ring = s->ring;
        *s = (Stream){0};
        snx_st_le64(GP(stream), 0);
    }
    unlock();
    mic_free(old);
    free(ring);
}
/* the recording opens here, outside the lock, and stays the stream's until it stops: a start that finds the stream
   started already, or closed meanwhile, closes the one it opened */
u32 audio_start_input(CPU* c, u64 stream) {
    Stream* s;
    Mic* fresh = NULL;
    u32 rate = 0;
    (void)c;
    lock();
    s = input_of(stream);
    if (s && s->state != STREAM_STARTED) rate = s->rate;
    unlock();
    if (rate) fresh = mic_open(rate);
    lock();
    s = input_of(stream);
    if (s && s->state != STREAM_STARTED) {
        s->state = STREAM_STARTED;
        s->last = guest_ns();
        s->carry = 0;
        if (!s->mic && fresh && fresh->size == s->rate / 2) { /* opened for this rate: not reopened meanwhile */
            s->mic = fresh;
            fresh = NULL;
        }
    }
    unlock();
    mic_free(fresh);
    return s ? 0 : STREAM_FAILED;
}
/* the host's device closes with the stream's stop: what was recorded and not read stays */
u32 audio_stop_input(CPU* c, u64 stream) {
    Stream* s;
    Mic* old = NULL;
    (void)c;
    lock();
    s = input_of(stream);
    if (s) {
        s->state = STREAM_STOPPED;
        old = s->mic;
        s->mic = NULL;
    }
    unlock();
    mic_free(old);
    return s ? 0 : STREAM_FAILED;
}
/* the frames recorded and not read dropped */
u32 audio_purge_input(CPU* c, u64 stream) {
    Stream* s;
    (void)c;
    lock();
    s = input_of(stream);
    if (s) s->head = s->filled = 0;
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
void audio_set_input_gain(CPU* c, u64 stream, f32 gain, i64 fade) {
    Stream* s;
    (void)c, (void)fade;
    lock();
    s = input_of(stream);
    if (s) s->gain = gain < 0 || isnan(gain) ? 0 : gain;
    unlock();
}
f32 audio_input_gain(CPU* c, u64 stream) {
    Stream* s;
    f32 gain = 0;
    (void)c;
    lock();
    s = input_of(stream);
    if (s) gain = s->gain;
    unlock();
    return gain;
}
/* a recorded sample in the stream's format at p, its gain applied. At a gain of 1 a 16-bit stream gets the samples
   as they are */
static void put_sample(u8* p, u32 format, i16 x, float gain) {
    float v = (float)x * gain; /* in 1/32768ths */
    switch (format) {
        case 1: p[0] = (u8)(i8)lrintf(fminf(fmaxf(v / 256.0f, -128.0f), 127.0f)); break;
        case 2: snx_st_le16(p, (u16)(gain == 1.0f ? x : (i16)lrintf(fminf(fmaxf(v, -32768.0f), 32767.0f)))); break;
        case 3: {
            u32 y = (u32)(i32)lrintf(fminf(fmaxf(v * 256.0f, -8388608.0f), 8388607.0f));
            p[0] = (u8)y, p[1] = (u8)(y >> 8), p[2] = (u8)(y >> 16);
            break;
        }
        case 4: snx_st_le32(p, (u32)(i32)llrint(fmin(fmax((double)v * 65536.0, -2147483648.0), 2147483647.0))); break;
        default: {
            float f = v / 32768.0f;
            u32 bits;
            memcpy(&bits, &f, 4);
            snx_st_le32(p, bits);
        }
    }
}
/* as many recorded frames as asked for and there are, in the stream's format, the one recording on every channel:
   the count read */
i32 audio_read_input(CPU* c, u64 stream, u64 data, i32 frames) {
    Stream* s;
    i32 n = 0;
    (void)c;
    if (frames <= 0 || !data) return 0;
    lock();
    s = input_of(stream);
    if (s) {
        u32 frame_bytes = s->sample_bytes * s->channels;
        u8* out = GP(data);
        n = (i32)((u32)frames < s->filled ? (u32)frames : s->filled);
        for (i32 i = 0; i < n; i++) {
            i16 x = s->ring[(s->head + (u32)i) % s->capacity];
            for (u32 ch = 0; ch < s->channels; ch++)
                put_sample(out + (size_t)i * frame_bytes + (size_t)ch * s->sample_bytes, s->format, x, s->gain);
        }
        s->head = (s->head + (u32)n) % s->capacity;
        s->filled -= (u32)n;
    }
    unlock();
    return n;
}
/* the clock's turn of a started input: want frames of the recording into the ring, as far as its capacity holds them
   (the rest is lost, as on a console whose game doesn't read) */
static void input_tick(Stream* s, u64 want) {
    i16 rec[1024];
    while (want) {
        u32 n = want < 1024 ? (u32)want : 1024;
        mic_take(s->mic, rec, n);
        for (u32 i = 0; i < n && s->filled < s->capacity; i++, s->filled++)
            s->ring[(s->head + s->filled) % s->capacity] = rec[i];
        want -= n;
    }
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
            input_tick(s, want);
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
