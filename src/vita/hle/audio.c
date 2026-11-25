/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SceAudio: the output ports, each an SDL3 stream on the default playback device, which mixes them. A buffer is
   queued when output, and the call returns when the one before it has played, as the console's double-buffered port
   does: the game stays a buffer ahead of what is heard. Without a device the time still passes the same way.
   SceAudioIn: a microphone that hears silence, each grain taking its time as the console's does */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCE_AUDIO_OUT_ERROR_NOT_OPENED          0x80260001u
#define SCE_AUDIO_OUT_ERROR_PORT_FULL           0x80260005u
#define SCE_AUDIO_OUT_ERROR_INVALID_PORT        0x80260003u
#define SCE_AUDIO_OUT_ERROR_INVALID_SIZE        0x80260006u
#define SCE_AUDIO_OUT_ERROR_INVALID_FORMAT      0x80260007u
#define SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ 0x80260008u
#define SCE_AUDIO_OUT_ERROR_INVALID_PORT_TYPE   0x8026000au
#define SCE_AUDIO_OUT_ERROR_INVALID_CONF_TYPE   0x8026000cu
#define PORTS 8u
#define SCE_AUDIO_IN_ERROR_INVALID_PORT_PARAM 0x80260106u
#define SCE_AUDIO_IN_ERROR_NOT_OPENED         0x80260109u
#define SCE_AUDIO_IN_ERROR_PORT_FULL          0x80260107u
#define AUDIO_IN_PORT 0x100u /* the input port's handle */

typedef struct {
    bool open;
    uint32_t type, len, freq, mode; /* mode 0 mono, 1 stereo, 16-bit samples */
    SDL_AudioStream* stream;
    uint64_t busy_until; /* host ns when what was queued has played */
} Port;
struct VitaAudio {
    Port ports[PORTS];
    Port input; /* type, grain (len) and freq; never a stream */
    bool sdl;
};

static struct VitaAudio* audio(VitaProcess* p) {
    if (!p->audio && (p->audio = calloc(1, sizeof *p->audio)) != NULL) {
        p->audio->sdl = SDL_InitSubSystem(SDL_INIT_AUDIO);
        if (!p->audio->sdl) printf("vita: no audio output (%s): sound is not heard\n", SDL_GetError());
    }
    return p->audio;
}
static void stream_open(struct VitaAudio* a, Port* port) {
    if (port->stream) SDL_DestroyAudioStream(port->stream);
    port->stream = NULL;
    if (!a->sdl) return;
    SDL_AudioSpec spec = {SDL_AUDIO_S16LE, port->mode ? 2 : 1, (int)port->freq};
    port->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
    if (port->stream) SDL_ResumeAudioStreamDevice(port->stream);
}
static Port* port_of(VitaProcess* p, uint32_t id) {
    struct VitaAudio* a = p->audio;
    return a && id >= 1 && id <= PORTS && a->ports[id - 1].open ? &a->ports[id - 1] : NULL;
}
static bool valid_freq(uint32_t freq) {
    static const uint32_t rates[] = {8000, 11025, 12000, 16000, 22050, 24000, 32000, 44100, 48000};
    for (size_t i = 0; i < sizeof rates / sizeof *rates; ++i)
        if (rates[i] == freq) return true;
    return false;
}
/* sceAudioOutOpenPort(type, len, freq, mode): the main port plays at 48 kHz only, BGM and voice at the others too */
uint32_t vita_audio_open_port(VitaProcess* p, VitaThread* t, uint32_t type, uint32_t len, uint32_t freq,
                              uint32_t mode) {
    (void)t;
    if (type > 2) return SCE_AUDIO_OUT_ERROR_INVALID_PORT_TYPE;
    if (!valid_freq(freq) || (type == 0 && freq != 48000)) return SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ;
    if (mode > 1) return SCE_AUDIO_OUT_ERROR_INVALID_FORMAT;
    if (!len || len > 65472 || (len & 63)) return SCE_AUDIO_OUT_ERROR_INVALID_SIZE;
    struct VitaAudio* a = audio(p);
    if (!a) return SCE_AUDIO_OUT_ERROR_NOT_OPENED;
    for (uint32_t i = 0; i < PORTS; ++i) {
        Port* port = &a->ports[i];
        if (port->open) continue;
        *port = (Port){.open = true, .type = type, .len = len, .freq = freq, .mode = mode};
        stream_open(a, port);
        return i + 1;
    }
    return SCE_AUDIO_OUT_ERROR_PORT_FULL;
}
uint32_t vita_audio_release_port(VitaProcess* p, VitaThread* t, uint32_t port_id) {
    (void)t;
    Port* port = port_of(p, port_id);
    if (!port) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    if (port->stream) SDL_DestroyAudioStream(port->stream);
    *port = (Port){0};
    return 0;
}
/* sceAudioOutOutput(port, buf): len samples queued; NULL waits for what was queued to play */
uint32_t vita_audio_output(VitaProcess* p, VitaThread* t, uint32_t port_id, uint32_t buffer) {
    Port* port = port_of(p, port_id);
    if (!port) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    uint64_t now = plat_ns(), duration = (uint64_t)port->len * 1000000000u / port->freq;
    if (!buffer) {
        if (port->busy_until > now) vita_wait(p, (VitaWait){.kind = VITA_WAIT_AUDIO, .deadline = port->busy_until});
        return 0;
    }
    uint32_t bytes = port->len * 2 * (port->mode ? 2 : 1);
    if (!vita_mapped(p, buffer, bytes)) return SCE_AUDIO_OUT_ERROR_INVALID_SIZE;
    if (port->stream) SDL_PutAudioStreamData(port->stream, vita_ptr(p, buffer), (int)bytes);
    /* this buffer plays once the one before it has: the call returns then */
    uint64_t start = port->busy_until > now ? port->busy_until : now;
    port->busy_until = start + duration;
    t->cpu.r[0] = port->len;
    if (start > now) vita_wait(p, (VitaWait){.kind = VITA_WAIT_AUDIO, .deadline = start, .a = 1});
    return port->len;
}
/* sceAudioOutSetConfig(port, len, freq, mode): a negative value keeps the setting */
uint32_t vita_audio_set_config(VitaProcess* p, VitaThread* t, uint32_t port_id, int32_t len, int32_t freq,
                               int32_t mode) {
    (void)t;
    Port* port = port_of(p, port_id);
    if (!port) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    if (len >= 0) {
        if (!len || len > 65472 || (len & 63)) return SCE_AUDIO_OUT_ERROR_INVALID_SIZE;
        port->len = (uint32_t)len;
    }
    if (freq >= 0) {
        if (!valid_freq((uint32_t)freq) || (port->type == 0 && freq != 48000))
            return SCE_AUDIO_OUT_ERROR_INVALID_SAMPLE_FREQ;
        port->freq = (uint32_t)freq;
    }
    if (mode >= 0) {
        if (mode > 1) return SCE_AUDIO_OUT_ERROR_INVALID_FORMAT;
        port->mode = (uint32_t)mode;
    }
    stream_open(p->audio, port);
    return 0;
}
uint32_t vita_audio_get_config(VitaProcess* p, VitaThread* t, uint32_t port_id, uint32_t type) {
    (void)t;
    Port* port = port_of(p, port_id);
    if (!port) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    switch (type) {
        case 0: return port->len;
        case 1: return port->freq;
        case 2: return port->mode;
        default: return SCE_AUDIO_OUT_ERROR_INVALID_CONF_TYPE;
    }
}
/* sceAudioOutSetVolume(port, flag, vol[2]): left (flag bit 0) and right (bit 1) of 32768, one gain for both here */
uint32_t vita_audio_set_volume(VitaProcess* p, VitaThread* t, uint32_t port_id, uint32_t flag, uint32_t volumes) {
    (void)t;
    Port* port = port_of(p, port_id);
    if (!port) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    if (!volumes) return 0x80260004u; /* SCE_AUDIO_OUT_ERROR_INVALID_POINTER */
    int32_t left = (int32_t)vita_read32(p, volumes), right = (int32_t)vita_read32(p, volumes + 4);
    int32_t loudest = (flag & 1) && (!(flag & 2) || left >= right) ? left : right;
    if (port->stream) SDL_SetAudioStreamGain(port->stream, (float)loudest / 32768.0f);
    return 0;
}
uint32_t vita_audio_get_rest_sample(VitaProcess* p, VitaThread* t, uint32_t port_id) {
    (void)t;
    Port* port = port_of(p, port_id);
    if (!port) return SCE_AUDIO_OUT_ERROR_INVALID_PORT;
    uint64_t now = plat_ns();
    return port->busy_until > now ? (uint32_t)((port->busy_until - now) * port->freq / 1000000000u) : 0;
}
void vita_audio_free(VitaProcess* p) {
    if (!p->audio) return;
    for (unsigned i = 0; i < PORTS; ++i)
        if (p->audio->ports[i].stream) SDL_DestroyAudioStream(p->audio->ports[i].stream);
    if (p->audio->sdl) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    free(p->audio);
    p->audio = NULL;
}

/* sceAudioInOpenPort(type, grain, freq, param): one port, mono */
uint32_t vita_audio_in_open_port(VitaProcess* p, VitaThread* t, uint32_t type, uint32_t grain, uint32_t freq,
                                 uint32_t param) {
    (void)t, (void)param;
    struct VitaAudio* a = audio(p);
    if (!a) return SCE_AUDIO_IN_ERROR_NOT_OPENED;
    if (a->input.open) return SCE_AUDIO_IN_ERROR_PORT_FULL;
    if (!grain || grain > 4096 || (freq != 16000 && freq != 48000)) return SCE_AUDIO_IN_ERROR_INVALID_PORT_PARAM;
    a->input = (Port){.open = true, .type = type, .len = grain, .freq = freq};
    return AUDIO_IN_PORT;
}
static Port* in_port(VitaProcess* p, uint32_t id) {
    return id == AUDIO_IN_PORT && p->audio && p->audio->input.open ? &p->audio->input : NULL;
}
uint32_t vita_audio_in_release_port(VitaProcess* p, VitaThread* t, uint32_t port_id) {
    (void)t;
    Port* port = in_port(p, port_id);
    if (!port) return SCE_AUDIO_IN_ERROR_NOT_OPENED;
    *port = (Port){0};
    return 0;
}
/* sceAudioInInput(port, dest): a grain of silence, once a grain's time after the last */
uint32_t vita_audio_in_input(VitaProcess* p, VitaThread* t, uint32_t port_id, uint32_t dest) {
    (void)t;
    Port* port = in_port(p, port_id);
    if (!port) return SCE_AUDIO_IN_ERROR_NOT_OPENED;
    if (!dest || !vita_mapped(p, dest, port->len * 2)) return SCE_AUDIO_IN_ERROR_INVALID_PORT_PARAM;
    memset(vita_ptr(p, dest), 0, port->len * 2);
    uint64_t now = plat_ns(), start = port->busy_until > now ? port->busy_until : now;
    port->busy_until = start + (uint64_t)port->len * 1000000000u / port->freq;
    vita_wait(p, (VitaWait){.kind = VITA_WAIT_AUDIO, .deadline = port->busy_until});
    return 0;
}
/* sceAudioInGetStatus(select): nothing muted or in use elsewhere */
uint32_t vita_audio_in_get_status(VitaProcess* p, VitaThread* t, uint32_t select) {
    (void)select;
    (void)p, (void)t;
    return 0;
}
/* sceAudioOutGetAdopt and sceAudioInGetAdopt(type): every port type is there */
uint32_t vita_audio_get_adopt(VitaProcess* p, VitaThread* t, uint32_t type) {
    (void)type;
    (void)p, (void)t;
    return 1;
}
