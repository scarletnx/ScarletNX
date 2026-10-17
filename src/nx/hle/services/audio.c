/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/audio/audio_out.cpp, audio_renderer_manager.cpp, audio_renderer.cpp,
   audio_device.cpp, hardware_opus_decoder.cpp, hardware_opus_decoder_manager.cpp, errors.h;
   src/audio_core/out/audio_out_system.cpp, device/audio_buffers.h, opus/decoder_manager.cpp, opus/decoder.cpp,
   renderer/system.cpp, common/feature_support.h):
   SPDX-FileCopyrightText: Copyright 2025-2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018-2024 yuzu Emulator Project (GPL-2.0-or-later) */
/* audout:u, audren:u's service side, IAudioDevice and hwopus, with the audio clock that paces them: every 5 ms it
   renders a frame of each started renderer (audren.c), signals the renderer event, and plays and releases the
   audio outs' buffers at 48 kHz as the console's DSP does. Games sync against that release (CRI's movie player
   waits for its buffers back), so it follows the clock whether or not the host has an audio device */
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/audren_dsp.h"
#ifndef SNX_NO_H264
#include "nx/gpu/video/host1x.h"
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#endif

#include "hle/services/audio.swipc.inc"

#define AUDIO_MOD  153 /* nn::audio's results */
#define HWOPUS_MOD 111

#define AUDIO_NOT_FOUND           RC(AUDIO_MOD, 1)
#define AUDIO_OPERATION_FAILED    RC(AUDIO_MOD, 2)
#define AUDIO_BUFFER_COUNT_FULL   RC(AUDIO_MOD, 8)
#define AUDIO_INVALID_CHANNELS    RC(AUDIO_MOD, 10)
#define AUDIO_NOT_SUPPORTED       RC(AUDIO_MOD, 513)
#define AUDIO_INVALID_HANDLE      RC(AUDIO_MOD, 1536)
#define AUDIO_INVALID_REVISION    RC(AUDIO_MOD, 1537)
#define OPUS_INVALID_PACKET       RC(HWOPUS_MOD, 17)
#define OPUS_OUT_OF_DECODERS      RC(HWOPUS_MOD, 385)
#define OPUS_INVALID_SAMPLE_RATE  RC(HWOPUS_MOD, 1001)
#define OPUS_INVALID_CHANNEL_COUNT RC(HWOPUS_MOD, 1002)

#define AUDIO_RATE      48000u /* the DSP's, the only rate an audio out plays at */
#define AUDIO_FRAME_NS  5000000 /* the DSP's frame */
#define AUDOUT_BUFFERS  64      /* appended and released buffers an audio out holds */
#define AUDIO_REVISION  15      /* the newest renderer revision understood (Eden's CurrentRevision) */
#define AUDIO_NAME_SIZE 0x100   /* an AudioDeviceName */

static const char audout_name[] = "DeviceOut";
static const char device_name[] = "AudioTvOutput";

/* an IAudioOut: its buffers in one ring, the released ones first, oldest to newest, then the ones still queued */
typedef struct AudOut {
    struct AudOut* next; /* in audio.outs */
    PAudio* output;      /* NULL without a host device: the buffers are still played on the clock */
    KEvent* ev;          /* the buffer event: a buffer released */
    u32 channels, started;
    float volume;
    u64 tag[AUDOUT_BUFFERS];
    u32 frames[AUDOUT_BUFFERS];
    u32 first, nreleased, nqueued;
    u32 head_played; /* frames of the first queued buffer already played */
    u64 t0;          /* when the clock started counting the queued buffers */
    u64 t0_frames;   /* frames played since t0 */
    u64 played;      /* GetAudioOutPlayedSampleCount */
} AudOut;

/* an IAudioRenderer. Its play state is the object's u[0] (SNX_AUDREN_STARTED or SNX_AUDREN_STOPPED), which the clock
   reads in audren_tick; audren.c keeps its state in u[1] */
typedef struct {
    nn_audio_detail_AudioRendererParameterInternal params;
    u32 limit;
    u32 manual; /* OpenAudioRendererForManualExecution: no system event */
} AudRen;

static struct {
    PMutex lk;    /* the audio outs */
    KEvent* ev;   /* the renderers' system event, every frame */
    AudOut* outs; /* every open audio out, for the clock */
} audio = {PMUTEX_INIT, NULL, NULL};

/* ============================================================ helpers */
/* whether [addr, addr + size) is guest memory with perm, block by block */
static int guest_ok(u64 addr, u64 size, u32 perm) {
    u64 end = addr + size;
    MemBlock b;
    if (end < addr || end > AS_SIZE) return 0;
    while (addr < end) {
        mem_query(addr, &b);
        if ((b.perm & perm) != perm || b.addr + b.size <= addr) return 0;
        addr = b.addr + b.size;
    }
    return 1;
}

/* a name into an output buffer, cut to fit and zero padded up to an AudioDeviceName */
static void write_name(IpcBuf b, const char* name) {
    u64 n = b.size < AUDIO_NAME_SIZE ? b.size : AUDIO_NAME_SIZE;
    size_t len = strlen(name);
    if (!b.addr || !n || !guest_ok(b.addr, n, PERM_W)) return;
    memset(GP(b.addr), 0, (size_t)n);
    memcpy(GP(b.addr), name, len < n ? len : (size_t)(n - 1));
}

/* names into an array of AudioDeviceName: as many as fit, up to the one there is */
static u32 list_names(IpcBuf b, const char* name) {
    if (!b.addr || b.size < AUDIO_NAME_SIZE) return 0;
    write_name(b, name);
    return 1;
}

/* 'REV' and the revision's digit, or a bare number, as Eden's GetRevisionNum reads it */
static u32 revision_number(u32 rev) {
    if (rev >= 0x100) rev = (rev - 0x30564552u) >> 24; /* 'R' 'E' 'V' '0' */
    return rev;
}

/* ============================================================ audio outs */
/* frames of 48 kHz in ns, without overflowing for days of uptime */
static u64 frames_in(u64 ns) { return ns / 1000000000u * AUDIO_RATE + ns % 1000000000u * AUDIO_RATE / 1000000000u; }

/* plays the queued buffers up to now, releasing the ones finished. Returns how many were. A started out with nothing
   queued is idle: the clock starts over from the next append */
static u32 audout_advance(AudOut* q, u64 now) {
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
static u32 audout_release_all(AudOut* q) {
    u32 n = q->nqueued;
    q->nreleased += n;
    q->nqueued = 0;
    q->head_played = 0;
    plat_audio_clear(q->output);
    return n;
}

static void audout_destroy(SvcObj* o) {
    AudOut* q = o->data;
    AudOut** p;
    pm_lock(&audio.lk);
    for (p = &audio.outs; *p; p = &(*p)->next)
        if (*p == q) {
            *p = q->next;
            break;
        }
    pm_unlock(&audio.lk);
    plat_audio_close(q->output);
    kobj_unref(&q->ev->o);
}

/* the clock's turn: every started out played up to now */
static void audout_tick(u64 now) {
    AudOut* q;
    pm_lock(&audio.lk);
    for (q = audio.outs; q; q = q->next)
        if (audout_advance(q, now)) kevent_signal(q->ev);
    pm_unlock(&audio.lk);
}

/* the out's state brought up to now under the lock, which the caller then holds */
static AudOut* audout_lock(SvcObj* self) {
    AudOut* q = self->data;
    pm_lock(&audio.lk);
    if (audout_advance(q, plat_ns())) kevent_signal(q->ev);
    return q;
}

static int nn_audio_detail_IAudioOut_GetAudioOutState(SvcObj* self, u32* state) {
    AudOut* q = audout_lock(self);
    *state = q->started ? 0 : 1;
    pm_unlock(&audio.lk);
    return 0;
}

static int nn_audio_detail_IAudioOut_Start(SvcObj* self) {
    AudOut* q = audout_lock(self);
    if (!q->started) {
        q->started = 1;
        q->t0 = plat_ns();
        q->t0_frames = 0;
        plat_audio_start(q->output, 1);
    }
    pm_unlock(&audio.lk);
    return 0;
}

/* what wasn't played comes back released, the host's copy dropped (Eden's System::Stop) */
static int nn_audio_detail_IAudioOut_Stop(SvcObj* self) {
    AudOut* q = audout_lock(self);
    if (q->started) {
        q->started = 0;
        plat_audio_start(q->output, 0);
        audout_release_all(q);
        kevent_signal(q->ev);
    }
    pm_unlock(&audio.lk);
    return 0;
}

/* the AudioOutBuffer's samples copied to the host now, so the guest owns its memory again once the buffer is
   released; its length in frames sets when that is */
static int audout_append(SvcObj* self, u64 tag, IpcBuf b) {
    nn_audio_AudioOutBuffer d;
    AudOut* q;
    u32 i;
    if (!b.addr || b.size < sizeof d || !guest_ok(b.addr, sizeof d, PERM_R)) return AUDIO_OPERATION_FAILED;
    memcpy(&d, GP(b.addr), sizeof d);
    q = audout_lock(self);
    if (q->nreleased + q->nqueued >= AUDOUT_BUFFERS) {
        pm_unlock(&audio.lk);
        return AUDIO_BUFFER_COUNT_FULL;
    }
    if (q->started && !q->nqueued) { /* idle until now */
        q->t0 = plat_ns();
        q->t0_frames = 0;
    }
    i = (q->first + q->nreleased + q->nqueued) % AUDOUT_BUFFERS;
    q->tag[i] = tag;
    q->frames[i] = (u32)(d.data_size / (q->channels * 2u));
    q->nqueued++;
    if (!q->output || !d.data_size) { /* without a host device it is played on the clock alone */
    } else if (d.data_size <= 0xFFFFFFFFu && d.buffer + d.data_offset >= d.buffer &&
               guest_ok(d.buffer + d.data_offset, d.data_size, PERM_R))
        plat_audio_queue(q->output, GP(d.buffer + d.data_offset), (u32)d.data_size);
    else
        WARN_LIMIT(4, "audout: buffer %#llx (%llu bytes at offset %llu) isn't readable, played as silence\n",
                   (unsigned long long)d.buffer, (unsigned long long)d.data_size,
                   (unsigned long long)d.data_offset);
    pm_unlock(&audio.lk);
    return 0;
}

static int nn_audio_detail_IAudioOut_AppendAudioOutBuffer(SvcObj* self, u64 tag, IpcBuf buffer) {
    return audout_append(self, tag, buffer);
}
static int nn_audio_detail_IAudioOut_AppendAudioOutBufferAuto(SvcObj* self, u64 tag, IpcBuf buffer) {
    return audout_append(self, tag, buffer);
}

static int nn_audio_detail_IAudioOut_RegisterBufferEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((AudOut*)self->data)->ev);
    return 0;
}

/* the released tags, oldest first. The SDK reads every slot of its array, so the rest is zeroed */
static int audout_released(SvcObj* self, IpcBuf b, u32* count) {
    AudOut* q;
    u32 n = 0, max;
    if (!b.addr || !guest_ok(b.addr, b.size, PERM_W)) return 0;
    max = (u32)(b.size / 8 < AUDOUT_BUFFERS ? b.size / 8 : AUDOUT_BUFFERS);
    memset(GP(b.addr), 0, (size_t)(b.size / 8 * 8));
    q = audout_lock(self);
    while (q->nreleased && n < max) {
        gw64(b.addr + 8 * n++, q->tag[q->first]);
        q->first = (q->first + 1) % AUDOUT_BUFFERS;
        q->nreleased--;
    }
    pm_unlock(&audio.lk);
    *count = n;
    return 0;
}

static int nn_audio_detail_IAudioOut_GetReleasedAudioOutBuffers(SvcObj* self, IpcBuf tags, u32* count) {
    return audout_released(self, tags, count);
}
static int nn_audio_detail_IAudioOut_GetReleasedAudioOutBuffersAuto(SvcObj* self, IpcBuf tags, u32* count) {
    return audout_released(self, tags, count);
}

/* appended and not yet taken back, played or not */
static int nn_audio_detail_IAudioOut_ContainsAudioOutBuffer(SvcObj* self, u64 tag, u8* contains) {
    AudOut* q = audout_lock(self);
    u32 i;
    for (i = 0; i < q->nreleased + q->nqueued; i++)
        if (q->tag[(q->first + i) % AUDOUT_BUFFERS] == tag) *contains = 1;
    pm_unlock(&audio.lk);
    return 0;
}

static int nn_audio_detail_IAudioOut_GetAudioOutBufferCount(SvcObj* self, u32* count) {
    AudOut* q = audout_lock(self);
    *count = q->nqueued;
    pm_unlock(&audio.lk);
    return 0;
}

static int nn_audio_detail_IAudioOut_GetAudioOutPlayedSampleCount(SvcObj* self, u64* count) {
    AudOut* q = audout_lock(self);
    *count = q->played;
    pm_unlock(&audio.lk);
    return 0;
}

static int nn_audio_detail_IAudioOut_FlushAudioOutBuffers(SvcObj* self, u8* flushed) {
    AudOut* q = audout_lock(self);
    if (audout_release_all(q)) kevent_signal(q->ev);
    q->t0 = plat_ns();
    q->t0_frames = 0;
    pm_unlock(&audio.lk);
    *flushed = 1;
    return 0;
}

static int nn_audio_detail_IAudioOut_SetAudioOutVolume(SvcObj* self, float volume) {
    AudOut* q = self->data;
    pm_lock(&audio.lk);
    q->volume = volume;
    plat_audio_volume(q->output, volume);
    pm_unlock(&audio.lk);
    return 0;
}

static int nn_audio_detail_IAudioOut_GetAudioOutVolume(SvcObj* self, float* volume) {
    AudOut* q = self->data;
    pm_lock(&audio.lk);
    *volume = q->volume;
    pm_unlock(&audio.lk);
    return 0;
}

/* ------------------------------------------------------------ audout:u */
static int nn_audio_detail_IAudioOutManager_ListAudioOuts(SvcObj* self, IpcBuf names, u32* count) {
    (void)self;
    *count = list_names(names, audout_name);
    return 0;
}
static int nn_audio_detail_IAudioOutManager_ListAudioOutsAuto(SvcObj* self, IpcBuf names, u32* count) {
    return nn_audio_detail_IAudioOutManager_ListAudioOuts(self, names, count);
}

/* the default device or "DeviceOut", two or six channels; the out plays at 48 kHz, 16-bit, whatever rate was asked
   for (Eden's IsConfigValid refuses another rate, the runtime has always answered with its own) */
static int open_out(const nn_audio_AudioOutParameter* params, IpcBuf name, IpcBuf name_out, u32* sample_rate,
                    u32* channel_count, u32* sample_format, u32* state, SvcObj** out) {
    char want[AUDIO_NAME_SIZE];
    u32 n = 0;
    SvcObj* o;
    AudOut* q;
    memset(want, 0, sizeof want);
    if (name.addr && name.size) {
        n = (u32)(name.size < sizeof want - 1 ? name.size : sizeof want - 1);
        if (guest_ok(name.addr, n, PERM_R)) memcpy(want, GP(name.addr), n);
    }
    if (want[0] && strcmp(want, audout_name)) return AUDIO_NOT_FOUND;
    if (params->channel_count != 0 && params->channel_count != 2 && params->channel_count != 6)
        return AUDIO_INVALID_CHANNELS;
    o = nn_audio_detail_IAudioOut_new(sizeof(AudOut));
    q = o->data;
    q->channels = params->channel_count == 6 ? 6 : 2;
    q->volume = 1.0f;
    q->ev = owned_event("IAudioOut");
    q->output = plat_audio_open(AUDIO_RATE, q->channels);
    o->destroy = audout_destroy;
    pm_lock(&audio.lk);
    q->next = audio.outs;
    audio.outs = q;
    pm_unlock(&audio.lk);
    write_name(name_out, audout_name);
    *sample_rate = AUDIO_RATE;
    *channel_count = q->channels;
    *sample_format = 2; /* PcmInt16 */
    *state = 1;         /* stopped */
    *out = o;
    return 0;
}

static int nn_audio_detail_IAudioOutManager_OpenAudioOut(SvcObj* self, const nn_audio_AudioOutParameter* params,
                                                         u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                         u32* sample_rate, u32* channel_count, u32* sample_format,
                                                         u32* state, SvcObj** out) {
    (void)self, (void)aruid, (void)process;
    return open_out(params, name, name_out, sample_rate, channel_count, sample_format, state, out);
}
static int nn_audio_detail_IAudioOutManager_OpenAudioOutAuto(SvcObj* self, const nn_audio_AudioOutParameter* params,
                                                             u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                             u32* sample_rate, u32* channel_count,
                                                             u32* sample_format, u32* state, SvcObj** out) {
    (void)self, (void)aruid, (void)process;
    return open_out(params, name, name_out, sample_rate, channel_count, sample_format, state, out);
}

SvcObj* new_audout(void) { return nn_audio_detail_IAudioOutManager_new(0); }

/* ============================================================ the audio renderer */
static int nn_audio_detail_IAudioRenderer_GetSampleRate(SvcObj* self, u32* sample_rate) {
    *sample_rate = ((AudRen*)self->data)->params.sample_rate;
    return 0;
}
static int nn_audio_detail_IAudioRenderer_GetSampleCount(SvcObj* self, u32* sample_count) {
    *sample_count = ((AudRen*)self->data)->params.sample_count;
    return 0;
}
static int nn_audio_detail_IAudioRenderer_GetMixBufferCount(SvcObj* self, u32* mix_buffer_count) {
    *mix_buffer_count = ((AudRen*)self->data)->params.mix_buffer_count;
    return 0;
}
static int nn_audio_detail_IAudioRenderer_GetState(SvcObj* self, u32* state) {
    *state = (u32)__atomic_load_n(&self->u[0], __ATOMIC_ACQUIRE);
    return 0;
}
static int nn_audio_detail_IAudioRenderer_RequestUpdate(SvcObj* self, IpcBuf input, IpcBuf output,
                                                        IpcBuf performance) {
    (void)performance; /* no performance metrics are kept */
    return audren_update(self, input, output);
}
static int nn_audio_detail_IAudioRenderer_RequestUpdateAuto(SvcObj* self, IpcBuf input, IpcBuf output,
                                                            IpcBuf performance) {
    return nn_audio_detail_IAudioRenderer_RequestUpdate(self, input, output, performance);
}
static int nn_audio_detail_IAudioRenderer_Start(SvcObj* self) {
    __atomic_store_n(&self->u[0], SNX_AUDREN_STARTED, __ATOMIC_RELEASE);
    return 0;
}
static int nn_audio_detail_IAudioRenderer_Stop(SvcObj* self) {
    __atomic_store_n(&self->u[0], SNX_AUDREN_STOPPED, __ATOMIC_RELEASE);
    return 0;
}
/* the event the clock signals after each frame; a renderer the game executes by itself has none */
static int nn_audio_detail_IAudioRenderer_QuerySystemEvent(SvcObj* self, u32* event) {
    if (((AudRen*)self->data)->manual) return AUDIO_NOT_SUPPORTED;
    *event = ev_handle(audio.ev);
    return 0;
}
static int nn_audio_detail_IAudioRenderer_SetRenderingTimeLimit(SvcObj* self, u32 percent) {
    __atomic_store_n(&((AudRen*)self->data)->limit, percent, __ATOMIC_RELAXED);
    return 0;
}
static int nn_audio_detail_IAudioRenderer_GetRenderingTimeLimit(SvcObj* self, u32* percent) {
    *percent = __atomic_load_n(&((AudRen*)self->data)->limit, __ATOMIC_RELAXED);
    return 0;
}

/* ------------------------------------------------------------ audren:u */
static u64 align_up(u64 v, u64 a) { return (v + a - 1) & ~(a - 1); }

/* the work buffer the game gives OpenAudioRenderer, in the shape of the console's (Eden's System::GetWorkBufferSize).
   The runtime keeps its state in host memory and never touches it, so the sizes of the console's structures are
   generous estimates here */
static u64 work_buffer_size(const nn_audio_detail_AudioRendererParameterInternal* p) {
    u64 mixes = (u64)p->sub_mix_count + 1, size = 0, voices = p->voice_count, dests, infos;
    u32 rev = revision_number(p->revision);
    size += align_up((u64)p->mix_buffer_count * 4, 0x40);
    size += (u64)p->sub_mix_count * 256 * 4; /* the submixes' effect order */
    size += mixes * 0x940;                   /* MixInfo */
    size += voices * (0x220 + 0x70 + 0x100); /* VoiceInfo, VoiceChannelResource, VoiceState */
    size += align_up(mixes * 8, 0x10) + align_up(voices * 8, 0x10);
    size += align_up((((u64)p->sink_count + p->sub_mix_count) * 240 * 4 + (u64)p->sample_count * 4) *
                         ((u64)p->mix_buffer_count + 6),
                     0x40);
    if (rev >= 2) { /* splitters: the mix graph's nodes and edges, then the splitters themselves */
        size += align_up(mixes * 16 + align_up(mixes * mixes / 8 + 1, 0x10), 0x10);
        dests = p->splitter_send_channel_count;
        infos = p->splitter_count;
        size += dests * 0xE0 + infos * 0x20;
        if (rev >= 5) size += align_up(dests * 4, 0x10);
    }
    size += ((u64)p->effect_count + voices * 4) * 0x20; /* MemoryPoolInfo */
    if (rev >= 9) size += (u64)p->effect_count * 0x80;  /* EffectResultState */
    size = align_up(size + 0x50, 0x40);
    size += ((u64)p->sink_count + p->sub_mix_count) * 0x280; /* UpsamplerInfo */
    size += (u64)p->effect_count * 0x1B0;                   /* EffectInfoBase */
    size += align_up(voices * 0x100, 0x40);
    size += (u64)p->sink_count * 0x170;
    if (rev >= 9) size += (u64)p->effect_count * 0x80;
    if (p->performance_frame_count)
        size += align_up((0x18 + (voices + p->effect_count + p->sink_count + mixes + 1) * 0x18 + 100 * 0x18) *
                             ((u64)p->performance_frame_count + 1) +
                             0xC0,
                         0x100);
    size += 0x18000 + (0x40 - 1) * 2; /* the command buffer */
    return align_up(size, 0x1000);
}

static int open_renderer(const nn_audio_detail_AudioRendererParameterInternal* params, u32 process, int manual,
                         SvcObj** renderer) {
    SvcObj* o;
    AudRen* r;
    if (revision_number(params->revision) > AUDIO_REVISION) return AUDIO_INVALID_REVISION;
    if (!process) return AUDIO_INVALID_HANDLE;
    o = nn_audio_detail_IAudioRenderer_new(sizeof(AudRen));
    r = o->data;
    r->params = *params;
    r->limit = 100;
    r->manual = (u32)manual;
    o->u[0] = SNX_AUDREN_STOPPED;
    if (!audren_open(o, (const u8*)params)) {
        WARN("audren: can't open a renderer of %lu voices, %lu submixes, %lu effects, %lu sinks at %lu Hz\n",
             (unsigned long)params->voice_count, (unsigned long)params->sub_mix_count,
             (unsigned long)params->effect_count, (unsigned long)params->sink_count,
             (unsigned long)params->sample_rate);
        svcobj_unref(o);
        return AUDIO_OPERATION_FAILED;
    }
    *renderer = o;
    return 0;
}

static int nn_audio_detail_IAudioRendererManager_OpenAudioRenderer(
    SvcObj* self, const nn_audio_detail_AudioRendererParameterInternal* params, u64 work_buffer_size_, u64 aruid,
    u32 work_buffer, u32 process, SvcObj** renderer) {
    (void)self, (void)work_buffer_size_, (void)aruid, (void)work_buffer;
    return open_renderer(params, process, 0, renderer);
}
static int nn_audio_detail_IAudioRendererManager_OpenAudioRendererForManualExecution(
    SvcObj* self, const nn_audio_detail_AudioRendererParameterInternal* params, u64 work_buffer_address, u32 process,
    u64 work_buffer_size_, u64 aruid, SvcObj** renderer) {
    (void)self, (void)work_buffer_address, (void)work_buffer_size_, (void)aruid;
    return open_renderer(params, process, 1, renderer);
}
static int nn_audio_detail_IAudioRendererManager_GetWorkBufferSize(
    SvcObj* self, const nn_audio_detail_AudioRendererParameterInternal* params, u64* size) {
    (void)self;
    if (revision_number(params->revision) > AUDIO_REVISION) return AUDIO_INVALID_REVISION;
    *size = work_buffer_size(params);
    return 0;
}

/* ------------------------------------------------------------ IAudioDevice: the TV output, two channels */
static void device_destroy(SvcObj* o) { kobj_unref(&((KEvent*)(size_t)o->u[0])->o); }

static SvcObj* new_device(void) {
    SvcObj* o = nn_audio_detail_IAudioDevice_new(0);
    KEvent* e = owned_event("IAudioDevice");
    kevent_signal(e); /* Eden signals it when the device is made */
    o->u[0] = (u64)(size_t)e;
    o->destroy = device_destroy;
    return o;
}

static int nn_audio_detail_IAudioRendererManager_GetAudioDeviceService(SvcObj* self, nn_applet_AppletResourceUserId aruid,
                                                                       SvcObj** device) {
    (void)self, (void)aruid;
    *device = new_device();
    return 0;
}
static int nn_audio_detail_IAudioRendererManager_GetAudioDeviceServiceWithRevisionInfo(
    SvcObj* self, nn_applet_AppletResourceUserId aruid, u32 revision, SvcObj** device) {
    (void)self, (void)aruid, (void)revision;
    *device = new_device();
    return 0;
}

static int nn_audio_detail_IAudioDevice_ListAudioDeviceName(SvcObj* self, IpcBuf names, i32* count) {
    (void)self;
    *count = (i32)list_names(names, device_name);
    return 0;
}
static int nn_audio_detail_IAudioDevice_ListAudioDeviceNameAuto(SvcObj* self, IpcBuf names, i32* count) {
    return nn_audio_detail_IAudioDevice_ListAudioDeviceName(self, names, count);
}
static int nn_audio_detail_IAudioDevice_GetAudioDeviceOutputVolume(SvcObj* self, IpcBuf name, float* volume) {
    (void)self, (void)name;
    *volume = 1.0f;
    return 0;
}
static int nn_audio_detail_IAudioDevice_GetAudioDeviceOutputVolumeAuto(SvcObj* self, IpcBuf name, float* volume) {
    return nn_audio_detail_IAudioDevice_GetAudioDeviceOutputVolume(self, name, volume);
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioDeviceName(SvcObj* self, IpcBuf name) {
    (void)self;
    write_name(name, device_name);
    return 0;
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioDeviceNameAuto(SvcObj* self, IpcBuf name) {
    return nn_audio_detail_IAudioDevice_GetActiveAudioDeviceName(self, name);
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioOutputDeviceName(SvcObj* self, IpcBuf name) {
    return nn_audio_detail_IAudioDevice_GetActiveAudioDeviceName(self, name);
}
static int nn_audio_detail_IAudioDevice_GetActiveChannelCount(SvcObj* self, u32* count) {
    (void)self;
    *count = 2;
    return 0;
}
/* one event for the three, as in Eden; the system one is signaled again when it is asked for */
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceSystemEvent(SvcObj* self, u32* event) {
    KEvent* e = (KEvent*)(size_t)self->u[0];
    kevent_signal(e);
    *event = ev_handle(e);
    return 0;
}
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceInputEvent(SvcObj* self, u32* event) {
    *event = ev_handle((KEvent*)(size_t)self->u[0]);
    return 0;
}
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceOutputEvent(SvcObj* self, u32* event) {
    *event = ev_handle((KEvent*)(size_t)self->u[0]);
    return 0;
}

SvcObj* new_audren(void) { return nn_audio_detail_IAudioRendererManager_new(0); }

/* ============================================================ the clock */
void audio_init(void) { audio.ev = owned_event("IAudioRenderer"); }

/* every 5 ms of host time: the renderers' frames, their event, the audio outs' buffers. Behind by more than a few
   frames (the host stalled), it starts over from now instead of rendering the backlog at once */
void audio_clock(void* arg) {
    u64 next = plat_ns(), now;
    (void)arg;
    for (;;) {
        audren_tick();
        kevent_signal(audio.ev);
        now = plat_ns();
        audout_tick(now);
        next += AUDIO_FRAME_NS;
        if (now > next + 10 * (u64)AUDIO_FRAME_NS) next = now + AUDIO_FRAME_NS;
        if (next > now) plat_sleep_ns((i64)(next - now));
    }
}

/* ============================================================ hwopus */
/* a decoder: FFmpeg's, at 48 kHz, its output brought down to the decoder's rate by averaging step samples, and kept
   in a queue so that every reply is exactly the packet's duration */
typedef struct {
    u32 rate, channels, step;
    PMutex lk;
#ifndef SNX_NO_H264
    AVCodecContext* dec;
    AVPacket* pkt;
    AVFrame* frame;
#endif
    i16* queue;  /* channels * cap samples, interleaved, at the decoder's rate */
    float* acc;  /* a sum per channel of the samples averaged so far */
    u32 queued, cap, phase;
} HwOpus;

static int opus_rate_ok(u32 rate) {
    return rate == 8000 || rate == 12000 || rate == 16000 || rate == 24000 || rate == 48000;
}

/* the samples per channel at 48 kHz an Opus packet holds, from its TOC byte and frame count (RFC 6716 3.1),
   0 for a packet that isn't one or holds more than 120 ms */
static u32 opus_packet_48k(const u8* p, u32 len) {
    static const u16 silk[4] = {480, 960, 1920, 2880}, hybrid[2] = {480, 960}, celt[4] = {120, 240, 480, 960};
    u32 config, frame, frames;
    if (!len) return 0;
    config = p[0] >> 3;
    frame = config < 12 ? silk[config & 3] : config < 16 ? hybrid[config & 1] : celt[config & 3];
    switch (p[0] & 3) {
        case 0: frames = 1; break;
        case 1:
        case 2: frames = 2; break;
        default:
            if (len < 2) return 0;
            frames = p[1] & 0x3F;
    }
    frames *= frame;
    return frames > 5760 ? 0 : frames;
}

static void hwopus_free(HwOpus* h) {
#ifndef SNX_NO_H264
    avcodec_free_context(&h->dec);
    av_packet_free(&h->pkt);
    av_frame_free(&h->frame);
#endif
    free(h->queue);
    h->queue = NULL;
    h->acc = NULL;
}

static void hwopus_destroy(SvcObj* o) { hwopus_free(o->data); }

/* opens h for rate and channels: one stream (mapping NULL), or streams of them with stereo_streams coupled and
   mapping placing their channels. 0 when it can't be */
static int hwopus_open(HwOpus* h, u32 rate, u32 channels, u32 streams, u32 stereo_streams, const u8* mapping) {
#ifndef SNX_NO_H264
    const AVCodec* codec;
    u8* extra;
    size_t qsize;
    if (!opus_rate_ok(rate) || !channels || channels > 255) return 0;
    if (!mapping ? channels > 2 : !streams || stereo_streams > streams || streams + stereo_streams > channels)
        return 0;
    ffmpeg_log_init();
    codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return 0;
    h->rate = rate;
    h->channels = channels;
    h->step = AUDIO_RATE / rate;
    h->cap = 3 * 5760 / h->step; /* a packet, and what FFmpeg's resampler may give back with the next */
    qsize = (size_t)h->cap * channels * sizeof *h->queue;
    h->queue = malloc(qsize + channels * sizeof *h->acc);
    h->dec = avcodec_alloc_context3(codec);
    h->pkt = av_packet_alloc();
    h->frame = av_frame_alloc();
    /* an OpusHead (RFC 7845 5.1): mapping family 0 for a stream of one or two channels, 255 (no layout) for more */
    extra = h->dec ? av_mallocz(21 + 255 + AV_INPUT_BUFFER_PADDING_SIZE) : NULL;
    if (!h->queue || !h->dec || !h->pkt || !h->frame || !extra) {
        av_free(extra);
        hwopus_free(h);
        return 0;
    }
    h->acc = (float*)((u8*)h->queue + qsize);
    memset(h->acc, 0, channels * sizeof *h->acc);
    memcpy(extra, "OpusHead", 8);
    extra[8] = 1;
    extra[9] = (u8)channels;
    extra[12] = (u8)AUDIO_RATE, extra[13] = (u8)(AUDIO_RATE >> 8), extra[14] = (u8)(AUDIO_RATE >> 16);
    extra[18] = mapping ? 255 : 0;
    if (mapping) {
        extra[19] = (u8)streams;
        extra[20] = (u8)stereo_streams;
        memcpy(extra + 21, mapping, channels);
    }
    h->dec->extradata = extra;
    h->dec->extradata_size = mapping ? 21 + (int)channels : 19;
    h->dec->sample_rate = AUDIO_RATE;
    av_channel_layout_default(&h->dec->ch_layout, (int)channels);
    if (avcodec_open2(h->dec, codec, NULL) < 0) {
        hwopus_free(h);
        return 0;
    }
    pm_init(&h->lk);
    return 1;
#else
    (void)h, (void)rate, (void)channels, (void)streams, (void)stereo_streams, (void)mapping;
    return 0; /* a build without FFmpeg has no Opus decoder */
#endif
}

#ifndef SNX_NO_H264
static float sample_at(const AVFrame* f, u32 ch, u32 channels, u32 i) {
    switch (f->format) {
        case AV_SAMPLE_FMT_FLTP: return ((const float*)f->extended_data[ch])[i] * 32768.0f;
        case AV_SAMPLE_FMT_FLT: return ((const float*)f->extended_data[0])[i * channels + ch] * 32768.0f;
        case AV_SAMPLE_FMT_S16P: return ((const i16*)f->extended_data[ch])[i];
        case AV_SAMPLE_FMT_S16: return ((const i16*)f->extended_data[0])[i * channels + ch];
        default: return 0.0f;
    }
}

/* a decoded frame into the queue, step samples averaged into one; the oldest go when it is full */
static void hwopus_take(HwOpus* h, const AVFrame* f) {
    u32 i, ch, n = (u32)f->nb_samples, fch = (u32)f->ch_layout.nb_channels;
    for (i = 0; i < n; i++) {
        for (ch = 0; ch < h->channels; ch++) h->acc[ch] += ch < fch ? sample_at(f, ch, fch, i) : 0.0f;
        if (++h->phase < h->step) continue;
        h->phase = 0;
        if (h->queued == h->cap) {
            memmove(h->queue, h->queue + h->channels, (size_t)(h->cap - 1) * h->channels * sizeof *h->queue);
            h->queued--;
        }
        for (ch = 0; ch < h->channels; ch++) {
            float v = h->acc[ch] / (float)h->step;
            h->queue[h->queued * h->channels + ch] = v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (i16)v;
            h->acc[ch] = 0.0f;
        }
        h->queued++;
    }
}
#endif

/* one packet behind its 8 byte header (a big-endian size and the encoder's final range) into out: exactly the
   packet's duration at the decoder's rate, from the queue, silence first when the decoder has given less so far */
static int hwopus_decode(HwOpus* h, const u8* in, u64 in_size, i16* out, u64 out_size, int reset, u32* consumed,
                         u32* samples) {
    u32 size, want;
    if (in_size < 8) return OPUS_INVALID_PACKET;
    size = (u32)in[0] << 24 | (u32)in[1] << 16 | (u32)in[2] << 8 | in[3];
    if (!size || size > in_size - 8) return OPUS_INVALID_PACKET;
    want = opus_packet_48k(in + 8, size) / h->step;
    if (!want || (u64)want * h->channels * 2 > out_size) return OPUS_INVALID_PACKET;
#ifndef SNX_NO_H264
    {
        u32 pad;
        int err;
        if (reset) {
            avcodec_flush_buffers(h->dec);
            h->queued = h->phase = 0;
            memset(h->acc, 0, h->channels * sizeof *h->acc);
        }
        if (av_new_packet(h->pkt, (int)size) < 0) return OPUS_INVALID_PACKET;
        memcpy(h->pkt->data, in + 8, size);
        err = avcodec_send_packet(h->dec, h->pkt);
        av_packet_unref(h->pkt);
        if (err < 0) return OPUS_INVALID_PACKET;
        while (avcodec_receive_frame(h->dec, h->frame) >= 0) {
            hwopus_take(h, h->frame);
            av_frame_unref(h->frame);
        }
        pad = want > h->queued ? want - h->queued : 0;
        memset(out, 0, (size_t)pad * h->channels * sizeof *out);
        memcpy(out + (size_t)pad * h->channels, h->queue, (size_t)(want - pad) * h->channels * sizeof *out);
        h->queued -= want - pad;
        memmove(h->queue, h->queue + (size_t)(want - pad) * h->channels, (size_t)h->queued * h->channels * sizeof *out);
    }
#else
    (void)reset;
    (void)out;
#endif
    *consumed = 8 + size;
    *samples = want;
    return 0;
}

/* a decode command: the guest's buffers checked, the time it took in microseconds */
static int decode(SvcObj* self, int reset, IpcBuf input, IpcBuf output, u32* consumed, u32* samples, u64* perf_us) {
    HwOpus* h = self->data;
    u64 t = plat_ns();
    int rc;
    if (!guest_ok(input.addr, input.size, PERM_R) || !guest_ok(output.addr, output.size, PERM_W))
        return OPUS_INVALID_PACKET;
    pm_lock(&h->lk);
    rc = hwopus_decode(h, GP(input.addr), input.size, (i16*)GP(output.addr), output.size, reset, consumed, samples);
    pm_unlock(&h->lk);
    if (perf_us) *perf_us = (plat_ns() - t) / 1000;
    return rc;
}

static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedOld(SvcObj* self, IpcBuf input, IpcBuf output,
                                                                     u32* consumed, u32* samples) {
    return decode(self, 0, input, output, consumed, samples, NULL);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedForMultiStreamOld(SvcObj* self, IpcBuf input,
                                                                                   IpcBuf output, u32* consumed,
                                                                                   u32* samples) {
    return decode(self, 0, input, output, consumed, samples, NULL);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedWithPerfOld(SvcObj* self, IpcBuf input,
                                                                             IpcBuf output, u32* consumed,
                                                                             u32* samples, u64* perf_us) {
    return decode(self, 0, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedForMultiStreamWithPerfOld(
    SvcObj* self, IpcBuf input, IpcBuf output, u32* consumed, u32* samples, u64* perf_us) {
    return decode(self, 0, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedWithPerfAndResetOld(SvcObj* self, u8 reset,
                                                                                     IpcBuf input, IpcBuf output,
                                                                                     u32* consumed, u32* samples,
                                                                                     u64* perf_us) {
    return decode(self, reset, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedForMultiStreamWithPerfAndResetOld(
    SvcObj* self, u8 reset, IpcBuf input, IpcBuf output, u32* consumed, u32* samples, u64* perf_us) {
    return decode(self, reset, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleaved(SvcObj* self, u8 reset, IpcBuf input,
                                                                  IpcBuf output, u32* consumed, u32* samples,
                                                                  u64* perf_us) {
    return decode(self, reset, input, output, consumed, samples, perf_us);
}
static int nn_codec_detail_IHardwareOpusDecoder_DecodeInterleavedForMultiStream(SvcObj* self, u8 reset,
                                                                                IpcBuf input, IpcBuf output,
                                                                                u32* consumed, u32* samples,
                                                                                u64* perf_us) {
    return decode(self, reset, input, output, consumed, samples, perf_us);
}

/* ------------------------------------------------------------ hwopus */
/* the work buffer: the runtime decodes in host memory and never touches it, so any plausible size will do */
static int work_size(u32 rate, u32 channels, u32 streams, u32 stereo_streams, int multi, u32* size) {
    if (multi ? !channels || channels > 255 : channels != 1 && channels != 2) return OPUS_INVALID_CHANNEL_COUNT;
    if (!opus_rate_ok(rate)) return OPUS_INVALID_SAMPLE_RATE;
    if (multi && (!streams || stereo_streams > streams || streams + stereo_streams > channels))
        return OPUS_INVALID_SAMPLE_RATE; /* Eden's answer for a bad stream count */
    *size = 0x10000u * (multi ? streams : 1);
    return 0;
}

static int open_decoder(u32 rate, u32 channels, u32 streams, u32 stereo_streams, const u8* mapping,
                        SvcObj** decoder) {
    SvcObj* o = nn_codec_detail_IHardwareOpusDecoder_new(sizeof(HwOpus));
    if (!hwopus_open(o->data, rate, channels, streams, stereo_streams, mapping)) {
        WARN_LIMIT(4, "hwopus: can't open a decoder of %lu channels at %lu Hz\n", (unsigned long)channels,
                   (unsigned long)rate);
        svcobj_unref(o);
        return OPUS_OUT_OF_DECODERS;
    }
    o->destroy = hwopus_destroy;
    *decoder = o;
    return 0;
}

/* a multistream parameter buffer: rate, channels, streams, coupled streams, then flags and a reserved word in the Ex
   form, then the channel mapping */
static int multi_params(IpcBuf b, int ex, u32 v[4], const u8** mapping) {
    u64 need = ex ? 0x118 : 0x110;
    if (!b.addr || b.size < need || !guest_ok(b.addr, need, PERM_R)) return 0;
    memcpy(v, GP(b.addr), 16);
    *mapping = GP(b.addr + (ex ? 0x18 : 0x10));
    return 1;
}

static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSize(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternal* params, u32* size) {
    (void)self;
    return work_size(params->sample_rate, params->channel_count, 1, 0, 0, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeEx(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternalEx* params, u32* size) {
    (void)self;
    return work_size(params->sample_rate, params->channel_count, 1, 0, 0, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeExEx(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternalEx* params, u32* size) {
    (void)self;
    return work_size(params->sample_rate, params->channel_count, 1, 0, 0, size);
}
static int multi_size(IpcBuf params, int ex, u32* size) {
    u32 v[4];
    const u8* mapping;
    if (!multi_params(params, ex, v, &mapping)) return OPUS_INVALID_CHANNEL_COUNT;
    return work_size(v[0], v[1], v[2], v[3], 1, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStream(SvcObj* self, IpcBuf params,
                                                                                      u32* size) {
    (void)self;
    return multi_size(params, 0, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStreamEx(SvcObj* self,
                                                                                        IpcBuf params, u32* size) {
    (void)self;
    return multi_size(params, 1, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStreamExEx(SvcObj* self,
                                                                                          IpcBuf params,
                                                                                          u32* size) {
    (void)self;
    return multi_size(params, 1, size);
}

static int nn_codec_detail_IHardwareOpusDecoderManager_OpenHardwareOpusDecoder(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternal* params, u32 work_buffer_size_,
    u32 work_buffer, SvcObj** decoder) {
    (void)self, (void)work_buffer_size_, (void)work_buffer;
    return open_decoder(params->sample_rate, params->channel_count, 1, params->channel_count - 1, NULL, decoder);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_OpenHardwareOpusDecoderEx(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternalEx* params, u32 work_buffer_size_,
    u32 work_buffer, SvcObj** decoder) {
    (void)self, (void)work_buffer_size_, (void)work_buffer;
    return open_decoder(params->sample_rate, params->channel_count, 1, params->channel_count - 1, NULL, decoder);
}
static int open_multi(IpcBuf params, int ex, SvcObj** decoder) {
    u32 v[4];
    const u8* mapping;
    if (!multi_params(params, ex, v, &mapping)) return OPUS_OUT_OF_DECODERS;
    return open_decoder(v[0], v[1], v[2], v[3], mapping, decoder);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_OpenHardwareOpusDecoderForMultiStream(
    SvcObj* self, IpcBuf params, u32 work_buffer_size_, u32 work_buffer, SvcObj** decoder) {
    (void)self, (void)work_buffer_size_, (void)work_buffer;
    return open_multi(params, 0, decoder);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_OpenHardwareOpusDecoderForMultiStreamEx(
    SvcObj* self, IpcBuf params, u32 work_buffer_size_, u32 work_buffer, SvcObj** decoder) {
    (void)self, (void)work_buffer_size_, (void)work_buffer;
    return open_multi(params, 1, decoder);
}

SvcObj* new_hwopus(void) { return nn_codec_detail_IHardwareOpusDecoderManager_new(0); }
