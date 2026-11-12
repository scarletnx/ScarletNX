/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/audio/audio_renderer_manager.cpp, audio_renderer.cpp, audio_device.cpp,
   hardware_opus_decoder.cpp, hardware_opus_decoder_manager.cpp, errors.h; src/audio_core/opus/decoder_manager.cpp,
   opus/decoder.cpp, renderer/system.cpp, common/feature_support.h):
   SPDX-FileCopyrightText: Copyright 2025-2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018-2024 yuzu Emulator Project (GPL-2.0-or-later)
   The hwopus work buffer sizes, from Ryujinx (src/Ryujinx.Horizon/Sdk/Codec/Detail/HardwareOpusDecoderManager.cs):
   SPDX-FileCopyrightText: Ryujinx Team and Contributors (MIT) */
/* audren:u's service side, IAudioDevice and hwopus, with the audio clock that paces them and the native audio outs
   (nn/audio.c): every 5 ms it renders a frame of each started renderer (audren.c), signals the renderer event, and
   plays and releases the audio outs' buffers */
#include "nx/hle/services/services_int.h"
#include "nx/hle/hle.h"
#include "nx/hle/services/audren_dsp.h"
#include "nx/hle/services/services.h"
#include "nx/nn/audio.h"
#include "nx/runtime/clock.h"
#include "nx/runtime/platform/tracy.h"
#include <math.h>
#ifndef SNX_NO_H264
#include "nx/gpu/video/host1x.h"
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#endif

#include "hle/services/audio.swipc.inc"
#include "common/utilities/bits.h"

#define AUDIO_RATE      48000u  /* the DSP's */
#define AUDIO_FRAME_NS  5000000 /* the DSP's frame */
/* the newest renderer revision understood: Eden's CurrentRevision, 15, and the Ounce's 16 (REV@, and REv@ with its
   lowercase v), which an SDK 22 game asks for. No feature of 16's is known: Eglantine (Eden's Ounce fork) takes its
   native 16 as 15, and so does the renderer here (audren_dsp.c's features go by revision 15 and earlier) */
#define AUDIO_REVISION  16
#define AUDIO_NAME_SIZE 0x100   /* an AudioDeviceName */

static const char device_name[] = "AudioTvOutput";

/* the objects' data (AudRen, AudDevice, HwOpus) and their pools: g_hle.audio (services_int.h) */

/* ============================================================ helpers */
/* whether [addr, addr + size) is guest memory with perm, block by block */
int snx_audio_guest_ok(u64 addr, u64 size, u32 perm) {
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
    if (!b.addr || !n || !snx_audio_guest_ok(b.addr, n, PERM_W)) return;
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

/* ============================================================ the native audio manager (aud:u) */
/* The Ounce service that hands back the managers the older services serve: a native-ABI game's
   GetAudioRendererWorkBufferSize reaches GetAudioRendererManager here, then calls GetWorkBufferSize on the returned
   object. GetWorkBufferSize is stateless, so a fresh manager answers it exactly as audren:u would. */
static int nn_audio_detail_INativeAudioManager_GetAudioInManager(SvcObj* self, SvcObj** manager) {
    (void)self;
    *manager = new_audin();
    R_SUCCEED();
}
/* the audio outs are answered in the SDK's own functions (nn/audio.c), with no manager to hand out: a game that
   asks for it called a function there that is not native yet, as one that opens audout:u did (sm.c) */
static int nn_audio_detail_INativeAudioManager_GetAudioOutManager(SvcObj* self, SvcObj** manager) {
    (void)self, (void)manager;
    WARN("\n*** The program asked aud:u for the audio out manager, which the runtime answers only natively ***\n");
    kernel_dump_threads();
    fatal("aud:u: the program asked for the audio out manager, which the runtime answers only in the SDK's own "
          "functions, nn::audio (nn/audio.c). A game got here through an SDK function that is not native yet (the "
          "calls of its thread, in the log's thread list before this, name it)");
}
static int nn_audio_detail_INativeAudioManager_GetAudioRendererManager(SvcObj* self, SvcObj** manager) {
    (void)self;
    *manager = new_audren();
    R_SUCCEED();
}
SvcObj* new_aud(void) { return nn_audio_detail_INativeAudioManager_new(); }

/* ============================================================ audin: no microphone to open */
static int nn_audio_detail_IAudioInManager_OpenAudioIn(SvcObj* self, const nn_audio_AudioInParameter* params,
                                                       u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                       u32* sample_rate, u32* channel_count, u32* sample_format,
                                                       u32* state, SvcObj** in) {
    (void)self, (void)params, (void)aruid, (void)process, (void)name, (void)name_out;
    (void)sample_rate, (void)channel_count, (void)sample_format, (void)state, (void)in;
    R_THROW(SNX_RC_AUDIO_NOT_FOUND);
}
static int nn_audio_detail_IAudioInManager_OpenAudioInAuto(SvcObj* self, const nn_audio_AudioInParameter* params,
                                                           u64 aruid, u32 process, IpcBuf name, IpcBuf name_out,
                                                           u32* sample_rate, u32* channel_count, u32* sample_format,
                                                           u32* state, SvcObj** in) {
    return nn_audio_detail_IAudioInManager_OpenAudioIn(self, params, aruid, process, name, name_out, sample_rate,
                                                       channel_count, sample_format, state, in);
}
static int nn_audio_detail_IAudioInManager_OpenAudioInProtocolSpecified(
    SvcObj* self, const nn_audio_AudioInProtocol* protocol, const nn_audio_AudioInParameter* params, u64 aruid,
    u32 process, IpcBuf name, IpcBuf name_out, u32* sample_rate, u32* channel_count, u32* sample_format, u32* state,
    SvcObj** in) {
    (void)protocol;
    return nn_audio_detail_IAudioInManager_OpenAudioIn(self, params, aruid, process, name, name_out, sample_rate,
                                                       channel_count, sample_format, state, in);
}
SvcObj* new_audin(void) { return nn_audio_detail_IAudioInManager_new(); }

/* ============================================================ the audio renderer */
static int nn_audio_detail_IAudioRenderer_GetSampleRate(SvcObj* self, u32* sample_rate) {
    *sample_rate = ((AudRen*)self->data)->sample_rate;
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRenderer_GetSampleCount(SvcObj* self, u32* sample_count) {
    *sample_count = ((AudRen*)self->data)->sample_count;
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRenderer_GetMixBufferCount(SvcObj* self, u32* mix_buffer_count) {
    *mix_buffer_count = ((AudRen*)self->data)->mix_buffer_count;
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRenderer_GetState(SvcObj* self, u32* state) {
    *state = (u32)__atomic_load_n(&self->u[0], __ATOMIC_ACQUIRE);
    R_SUCCEED();
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
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRenderer_Stop(SvcObj* self) {
    __atomic_store_n(&self->u[0], SNX_AUDREN_STOPPED, __ATOMIC_RELEASE);
    R_SUCCEED();
}
/* the event the clock signals after each frame; a renderer the game executes by itself has none */
static int nn_audio_detail_IAudioRenderer_QuerySystemEvent(SvcObj* self, u32* event) {
    R_UNLESS(!((AudRen*)self->data)->manual, SNX_RC_AUDIO_NOT_SUPPORTED);
    *event = ev_handle(g_hle.audio.ev);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRenderer_SetRenderingTimeLimit(SvcObj* self, u32 percent) {
    __atomic_store_n(&((AudRen*)self->data)->limit, percent, __ATOMIC_RELAXED);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRenderer_GetRenderingTimeLimit(SvcObj* self, u32* percent) {
    *percent = __atomic_load_n(&((AudRen*)self->data)->limit, __ATOMIC_RELAXED);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRenderer_SetVoiceDropParameter(SvcObj* self, float parameter) {
    __atomic_store(&((AudRen*)self->data)->voice_drop, &parameter, __ATOMIC_RELAXED);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRenderer_GetVoiceDropParameter(SvcObj* self, float* parameter) {
    __atomic_load(&((AudRen*)self->data)->voice_drop, parameter, __ATOMIC_RELAXED);
    R_SUCCEED();
}

/* ------------------------------------------------------------ audren:u */

/* the work buffer the game gives OpenAudioRenderer, in the shape of the console's (Eden's System::GetWorkBufferSize).
   The runtime keeps its state in host memory and never touches it, so the sizes of the console's structures are
   generous estimates here */
static u64 work_buffer_size(const nn_audio_detail_AudioRendererParameterInternal* p) {
    u64 mixes = (u64)p->sub_mix_count + 1, size = 0, voices = p->voice_count, dests, infos;
    u32 rev = revision_number(p->revision);
    size += snx_align_up((u64)p->mix_buffer_count * 4, 0x40);
    size += (u64)p->sub_mix_count * 256 * 4; /* the submixes' effect order */
    size += mixes * 0x940;                   /* MixInfo */
    size += voices * (0x220 + 0x70 + 0x100); /* VoiceInfo, VoiceChannelResource, VoiceState */
    size += snx_align_up(mixes * 8, 0x10) + snx_align_up(voices * 8, 0x10);
    size += snx_align_up((((u64)p->sink_count + p->sub_mix_count) * 240 * 4 + (u64)p->sample_count * 4) *
                         ((u64)p->mix_buffer_count + 6),
                     0x40);
    if (rev >= 2) { /* splitters: the mix graph's nodes and edges, then the splitters themselves */
        size += snx_align_up(mixes * 16 + snx_align_up(mixes * mixes / 8 + 1, 0x10), 0x10);
        dests = p->splitter_send_channel_count;
        infos = p->splitter_count;
        size += dests * 0xE0 + infos * 0x20;
        if (rev >= 5) size += snx_align_up(dests * 4, 0x10);
    }
    size += ((u64)p->effect_count + voices * 4) * 0x20; /* MemoryPoolInfo */
    if (rev >= 9) size += (u64)p->effect_count * 0x80;  /* EffectResultState */
    size = snx_align_up(size + 0x50, 0x40);
    size += ((u64)p->sink_count + p->sub_mix_count) * 0x280; /* UpsamplerInfo */
    size += (u64)p->effect_count * 0x1B0;                   /* EffectInfoBase */
    size += snx_align_up(voices * 0x100, 0x40);
    size += (u64)p->sink_count * 0x170;
    if (rev >= 9) size += (u64)p->effect_count * 0x80;
    if (p->performance_frame_count)
        size += snx_align_up((0x18 + (voices + p->effect_count + p->sink_count + mixes + 1) * 0x18 + 100 * 0x18) *
                             ((u64)p->performance_frame_count + 1) +
                             0xC0,
                         0x100);
    size += 0x18000 + (0x40 - 1) * 2; /* the command buffer */
    return snx_align_up(size, 0x1000);
}

static int open_renderer(const nn_audio_detail_AudioRendererParameterInternal* params, u32 process, int manual,
                         SvcObj** renderer) {
    SvcObj* o;
    AudRen* r;
    R_UNLESS(revision_number(params->revision) <= AUDIO_REVISION, SNX_RC_AUDIO_INVALID_REVISION);
    R_UNLESS(process, SNX_RC_AUDIO_INVALID_HANDLE);
    o = nn_audio_detail_IAudioRenderer_new_data(g_hle.audio.rens, g_hle.audio.ren_next, "audio renderers");
    r = o->data;
    r->sample_rate = params->sample_rate;
    r->sample_count = params->sample_count;
    r->mix_buffer_count = params->mix_buffer_count;
    r->limit = 100;
    r->voice_drop = 1.0f;
    r->manual = (u32)manual;
    o->u[0] = SNX_AUDREN_STOPPED;
    if (!audren_open(o, (const u8*)params)) {
        WARN("audren: can't open a renderer of %lu voices, %lu submixes, %lu effects, %lu sinks at %lu Hz\n",
             (unsigned long)params->voice_count, (unsigned long)params->sub_mix_count,
             (unsigned long)params->effect_count, (unsigned long)params->sink_count,
             (unsigned long)params->sample_rate);
        svcobj_unref(o);
        R_THROW(SNX_RC_AUDIO_OPERATION_FAILED);
    }
    *renderer = o;
    R_SUCCEED();
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
    R_UNLESS(revision_number(params->revision) <= AUDIO_REVISION, SNX_RC_AUDIO_INVALID_REVISION);
    *size = work_buffer_size(params);
    R_SUCCEED();
}

/* ------------------------------------------------------------ IAudioDevice: the TV output, two channels */
/* its system, input and output events, signaled when the device is made (the game reads the device then), and its
   input and output device notifications, which a change of the device would signal and the host's never makes: an
   AudDevice (services_int.h) */
static void device_destroy(SvcObj* o) {
    int i;
    for (i = 0; i < 5; i++) kobj_unref(kobj_at(((AudDevice*)o->data)->ev[i]));
}

static SvcObj* new_device(void) {
    static const char* const names[5] = {"IAudioDevice system", "IAudioDevice input", "IAudioDevice output",
                                         "IAudioDevice input notification", "IAudioDevice output notification"};
    SvcObj* o = nn_audio_detail_IAudioDevice_new_data(g_hle.audio.devices, g_hle.audio.device_next, "audio devices");
    int i;
    for (i = 0; i < 5; i++) {
        ((AudDevice*)o->data)->ev[i] = owned_event(names[i]);
        if (i < 3) ev_signal(((AudDevice*)o->data)->ev[i]);
    }
    o->destroy = device_destroy;
    return o;
}

static int nn_audio_detail_IAudioRendererManager_GetAudioDeviceService(SvcObj* self, nn_applet_AppletResourceUserId aruid,
                                                                       SvcObj** device) {
    (void)self, (void)aruid;
    *device = new_device();
    R_SUCCEED();
}
static int nn_audio_detail_IAudioRendererManager_GetAudioDeviceServiceWithRevisionInfo(
    SvcObj* self, nn_applet_AppletResourceUserId aruid, u32 revision, SvcObj** device) {
    (void)self, (void)aruid, (void)revision;
    *device = new_device();
    R_SUCCEED();
}

static int nn_audio_detail_IAudioDevice_ListAudioDeviceName(SvcObj* self, IpcBuf names, i32* count) {
    (void)self;
    *count = (i32)list_names(names, device_name);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_ListAudioDeviceNameAuto(SvcObj* self, IpcBuf names, i32* count) {
    return nn_audio_detail_IAudioDevice_ListAudioDeviceName(self, names, count);
}
/* whether a device name a game sends is the output device's */
static int is_device(IpcBuf name) {
    u64 n = name.size < AUDIO_NAME_SIZE ? name.size : AUDIO_NAME_SIZE;
    if (!name.addr || n < sizeof device_name || !snx_audio_guest_ok(name.addr, n, PERM_R)) return 0;
    return !memcmp(GP(name.addr), device_name, sizeof device_name);
}
/* the output device keeps the volume a game sets, and plays every audio out and renderer that much louder or softer;
   any other name has 1 */
static int nn_audio_detail_IAudioDevice_SetAudioDeviceOutputVolume(SvcObj* self, IpcBuf name, float volume) {
    (void)self;
    if (!is_device(name)) R_SUCCEED();
    pm_lock(&g_host.audio.lk);
    g_hle.audio.device_volume = volume;
    nnaudio_out_device_volume();
    audren_volume(volume);
    pm_unlock(&g_host.audio.lk);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_SetAudioDeviceOutputVolumeAuto(SvcObj* self, IpcBuf name, float volume) {
    return nn_audio_detail_IAudioDevice_SetAudioDeviceOutputVolume(self, name, volume);
}
static int nn_audio_detail_IAudioDevice_GetAudioDeviceOutputVolume(SvcObj* self, IpcBuf name, float* volume) {
    (void)self;
    pm_lock(&g_host.audio.lk);
    *volume = is_device(name) ? g_hle.audio.device_volume : 1.0f;
    pm_unlock(&g_host.audio.lk);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_GetAudioDeviceOutputVolumeAuto(SvcObj* self, IpcBuf name, float* volume) {
    return nn_audio_detail_IAudioDevice_GetAudioDeviceOutputVolume(self, name, volume);
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioDeviceName(SvcObj* self, IpcBuf name) {
    (void)self;
    write_name(name, device_name);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioDeviceNameAuto(SvcObj* self, IpcBuf name) {
    return nn_audio_detail_IAudioDevice_GetActiveAudioDeviceName(self, name);
}
/* the auto-tune mode only changes what the getter answers: the runtime plays at the volume the game sets */
static int nn_audio_detail_IAudioDevice_SetAudioDeviceOutputVolumeAutoTuneEnabled(SvcObj* self, u8 enabled) {
    (void)self;
    pm_lock(&g_host.audio.lk);
    g_hle.audio.auto_tune = enabled != 0;
    pm_unlock(&g_host.audio.lk);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_IsAudioDeviceOutputVolumeAutoTuneEnabled(SvcObj* self, u8* enabled) {
    (void)self;
    pm_lock(&g_host.audio.lk);
    *enabled = g_hle.audio.auto_tune;
    pm_unlock(&g_host.audio.lk);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_GetActiveAudioOutputDeviceName(SvcObj* self, IpcBuf name) {
    return nn_audio_detail_IAudioDevice_GetActiveAudioDeviceName(self, name);
}
/* the outputs there are: the one device, as GetActiveAudioOutputDeviceName names it */
static int nn_audio_detail_IAudioDevice_ListAudioOutputDeviceName(SvcObj* self, IpcBuf names, i32* count) {
    return nn_audio_detail_IAudioDevice_ListAudioDeviceName(self, names, count);
}
static int nn_audio_detail_IAudioDevice_GetActiveChannelCount(SvcObj* self, u32* count) {
    (void)self;
    *count = 2;
    R_SUCCEED();
}
/* the system event is signaled again when it is asked for, as Eden does */
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceSystemEvent(SvcObj* self, u32* event) {
    u32 e = ((AudDevice*)self->data)->ev[0];
    ev_signal(e);
    *event = ev_handle(e);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceInputEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((AudDevice*)self->data)->ev[1]);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_QueryAudioDeviceOutputEvent(SvcObj* self, u32* event) {
    *event = ev_handle(((AudDevice*)self->data)->ev[2]);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
/* 17.0.0 and later. The SDK's wrapper attaches the copy handle to a SystemEvent that owns it, and acquires one a run:
   each acquire gives the device's one event. The u64 is the SDK's own, which the server doesn't need here */
static int device_notification(SvcObj* self, int which, u32* event) {
    *event = ev_handle(((AudDevice*)self->data)->ev[which]);
    R_UNLESS(*event, SNX_RC_SVC_OUT_OF_HANDLES);
    R_SUCCEED();
}
static int nn_audio_detail_IAudioDevice_AcquireAudioInputDeviceNotification(SvcObj* self, u64 id, u32* event) {
    (void)id;
    return device_notification(self, 3, event);
}
static int nn_audio_detail_IAudioDevice_AcquireAudioOutputDeviceNotification(SvcObj* self, u64 id, u32* event) {
    (void)id;
    return device_notification(self, 4, event);
}

SvcObj* new_audren(void) { return nn_audio_detail_IAudioRendererManager_new(); }

/* ============================================================ the clock */
void audio_init(void) {
    g_hle.audio.ev = owned_event("IAudioRenderer");
    pc_init(&g_host.audio.hold_cv);
}

/* every 5 ms of host time: the renderers' frames, their event, the audio outs' buffers. Behind by more than a few
   frames (the host stalled), it starts over from now instead of rendering the backlog at once. The audio outs play in
   guest time, which a game pause stops: the clock itself stops at the top of its turn while one is in force */
void audio_clock(void* arg) {
    u64 next = plat_ns(), now;
    (void)arg;
    SNX_THREAD_NAME("audio");
    for (;;) {
        pm_lock(&g_host.audio.lk);
        if (g_hle.audio.hold) {
            g_hle.audio.held = 1;
            pc_broadcast(&g_host.audio.hold_cv);
            while (g_hle.audio.hold) pc_wait(&g_host.audio.hold_cv, &g_host.audio.lk, -1);
            g_hle.audio.held = 0;
            next = plat_ns();
        }
        pm_unlock(&g_host.audio.lk);
        SNX_ZONE(z_audio, "audio tick");
        audren_tick();
        ev_signal(g_hle.audio.ev);
        now = plat_ns();
        SNX_ZONE_VALUE(z_audio, now > next ? (u64)(now - next) / AUDIO_FRAME_NS : 0);
        nnaudio_out_tick(guest_ns());
        outstream_tick(guest_ns());
        SNX_ZONE_END(z_audio);
        next += AUDIO_FRAME_NS;
        if (now > next + 10 * (u64)AUDIO_FRAME_NS) next = now + AUDIO_FRAME_NS;
        if (next > now) plat_sleep_ns((i64)(next - now));
    }
}

void audio_hold(void) {
    pm_lock(&g_host.audio.lk);
    g_hle.audio.hold = 1;
    while (!g_hle.audio.held) pc_wait(&g_host.audio.hold_cv, &g_host.audio.lk, -1);
    pm_unlock(&g_host.audio.lk);
}
void audio_release(void) {
    pm_lock(&g_host.audio.lk);
    g_hle.audio.hold = 0;
    pc_broadcast(&g_host.audio.hold_cv);
    pm_unlock(&g_host.audio.lk);
}

/* ============================================================ hwopus */
/* a decoder: FFmpeg's, at 48 kHz, its output brought down to the decoder's rate through a low-pass filter keeping
   every step-th sample, and kept in a queue so that every reply is exactly the packet's duration (HwOpus,
   services_int.h) */
/* the queue starts with this much silence at 48 kHz: what FFmpeg's resampler may hold back of the first packets
   (libswresample's most), so that later replies come from the queue instead of silence put in front */
#define HWOPUS_LEAD 24

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
    h->taps = h->hist = NULL;
}

#ifndef SNX_NO_H264
/* the decoder as it starts: the filter's history zero and the queue's lead of silence */
static void hwopus_restart(HwOpus* h) {
    h->phase = h->hpos = 0;
    if (h->ntaps) memset(h->hist, 0, (size_t)h->ntaps * h->channels * sizeof *h->hist);
    h->queued = HWOPUS_LEAD / h->step;
    memset(h->queue, 0, (size_t)h->queued * h->channels * sizeof *h->queue);
}

/* a Blackman-windowed sinc low-pass of 24 * step + 1 taps cutting at 0.45 of the lower rate's half, its gain one */
static void hwopus_filter(HwOpus* h) {
    const double pi = 3.14159265358979323846, fc = 0.45 / h->step;
    double sum = 0.0, m = (double)(h->ntaps - 1), x, w, v;
    u32 i;
    for (i = 0; i < h->ntaps; i++) {
        x = (double)i - m / 2.0;
        w = 0.42 - 0.5 * cos(2.0 * pi * i / m) + 0.08 * cos(4.0 * pi * i / m);
        v = x == 0.0 ? 2.0 * fc : sin(2.0 * pi * fc * x) / (pi * x);
        h->taps[i] = (float)(v * w);
        sum += v * w;
    }
    for (i = 0; i < h->ntaps; i++) h->taps[i] = (float)(h->taps[i] / sum);
}
#endif

/* the firmware keeps SNX_HWOPUS_MAX_DECODERS (services_int.h) decoders and refuses the next with OutOfOpusDecoders,
   which sends the SDK to its software decoder; a game that opens decoders until refused depends on that answer.
   Those open, across every hwopus session, are the pool's slots taken, counted under ipc.c's lock, which guards the
   pool */
static u32 hwopus_count(void) {
    u32 i, n = 0;
    pm_lock(&g_host.ipc.lk);
    for (i = 0; i < HWOPUS_POOL; i++) n += g_hle.audio.opus[i].obj != 0;
    pm_unlock(&g_host.ipc.lk);
    return n;
}

static void hwopus_destroy(SvcObj* o) { hwopus_free(o->data); }

/* opens h for rate and channels: one stream (mapping NULL), or streams of them with stereo_streams coupled and
   mapping placing their channels. 0 when it can't be */
static int hwopus_open(HwOpus* h, u32 rate, u32 channels, u32 streams, u32 stereo_streams, const u8* mapping) {
#ifndef SNX_NO_H264
    const AVCodec* codec;
    u8* extra;
    size_t qsize, fsize;
    if (!opus_rate_ok(rate) || !channels || channels > 255) return 0;
    if (!mapping ? channels > 2 : !streams || stereo_streams > streams || streams + stereo_streams > 255) return 0;
    h->streams = streams;
    h->stereo_streams = stereo_streams;
    h->mapped = mapping != NULL;
    if (mapping) memcpy(h->mapping, mapping, channels);
    ffmpeg_log_init();
    codec = avcodec_find_decoder(AV_CODEC_ID_OPUS);
    if (!codec) return 0;
    h->rate = rate;
    h->channels = channels;
    h->step = AUDIO_RATE / rate;
    h->cap = 3 * 5760 / h->step; /* a packet, and what FFmpeg's resampler may give back with the next */
    h->ntaps = h->step > 1 ? 24 * h->step + 1 : 0;
    qsize = (size_t)h->cap * channels * sizeof *h->queue; /* even: the floats after it are aligned */
    fsize = (size_t)h->ntaps * (channels + 1) * sizeof *h->taps;
    h->queue = malloc(qsize + fsize + 1);
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
    h->taps = (float*)((u8*)h->queue + qsize);
    h->hist = h->taps + h->ntaps;
    if (h->ntaps) hwopus_filter(h);
    hwopus_restart(h);
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

/* a sample as s16: rounded, and clipped */
static i16 to_s16(float v) {
    v = (float)floor(v + 0.5f);
    return v >= 32767.0f ? 32767 : v <= -32768.0f ? -32768 : (i16)v;
}
/* a decoded frame into the queue, filtered down to every step-th sample; the oldest go when it is full */
static void hwopus_take(HwOpus* h, const AVFrame* f) {
    u32 i, ch, k, n = (u32)f->nb_samples, fch = (u32)f->ch_layout.nb_channels;
    for (i = 0; i < n; i++) {
        if (h->ntaps) {
            for (ch = 0; ch < h->channels; ch++)
                h->hist[ch * h->ntaps + h->hpos] = ch < fch ? sample_at(f, ch, fch, i) : 0.0f;
            h->hpos = (h->hpos + 1) % h->ntaps;
        }
        if (++h->phase < h->step) continue;
        h->phase = 0;
        if (h->queued == h->cap) {
            memmove(h->queue, h->queue + h->channels, (size_t)(h->cap - 1) * h->channels * sizeof *h->queue);
            h->queued--;
        }
        for (ch = 0; ch < h->channels; ch++) {
            float v = 0.0f;
            if (!h->ntaps)
                v = ch < fch ? sample_at(f, ch, fch, i) : 0.0f;
            else /* the newest sample is the one before hpos */
                for (k = 0; k < h->ntaps; k++)
                    v += h->taps[k] * h->hist[ch * h->ntaps + (h->hpos + h->ntaps - 1 - k) % h->ntaps];
            h->queue[h->queued * h->channels + ch] = to_s16(v);
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
    R_UNLESS(in_size >= 8, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
    size = (u32)in[0] << 24 | (u32)in[1] << 16 | (u32)in[2] << 8 | in[3];
    R_UNLESS(size && size <= in_size - 8, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
    want = opus_packet_48k(in + 8, size) / h->step;
    R_UNLESS(want && (u64)want * h->channels * 2 <= out_size, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
#ifndef SNX_NO_H264
    {
        u32 pad;
        int err;
        if (reset) {
            avcodec_flush_buffers(h->dec);
            hwopus_restart(h);
        }
        R_UNLESS(av_new_packet(h->pkt, (int)size) >= 0, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
        memcpy(h->pkt->data, in + 8, size);
        err = avcodec_send_packet(h->dec, h->pkt);
        av_packet_unref(h->pkt);
        R_UNLESS(err >= 0, SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
        while (avcodec_receive_frame(h->dec, h->frame) >= 0) {
            hwopus_take(h, h->frame);
            av_frame_unref(h->frame);
        }
        pad = want > h->queued ? want - h->queued : 0;
        memset(out, 0, (size_t)pad * h->channels * sizeof *out);
        memcpy(out + (size_t)pad * h->channels, h->queue, (size_t)(want - pad) * h->channels * sizeof *out);
        snx_le16_array(out, (size_t)want * h->channels); /* the guest's samples are little-endian */
        h->queued -= want - pad;
        memmove(h->queue, h->queue + (size_t)(want - pad) * h->channels, (size_t)h->queued * h->channels * sizeof *out);
    }
#else
    (void)reset;
    (void)out;
#endif
    *consumed = 8 + size;
    *samples = want;
    R_SUCCEED();
}

/* An Ounce game's hardware Opus decoders (hle/hook/codec.c), which its SDK runs on the audio DSP and the hooks run
   here: the same decoding, outside the service's objects and their save states */
HwOpus* hwopus_new(u32 rate, u32 channels) {
    HwOpus* h = calloc(1, sizeof *h);
    if (h && !hwopus_open(h, rate, channels, 1, channels - 1, NULL)) {
        free(h);
        return NULL;
    }
    return h;
}
void hwopus_delete(HwOpus* h) {
    if (!h) return;
    hwopus_free(h);
    free(h);
}
int hwopus_decode_packet(HwOpus* h, const u8* in, u64 in_size, i16* out, u64 out_size, int reset, u32* consumed,
                         u32* samples) {
    int rc;
    pm_lock(&h->lk);
    rc = hwopus_decode(h, in, in_size, out, out_size, reset, consumed, samples);
    pm_unlock(&h->lk);
    return rc;
}

/* a decode command: the guest's buffers checked, the time it took in microseconds */
static int decode(SvcObj* self, int reset, IpcBuf input, IpcBuf output, u32* consumed, u32* samples, u64* perf_us) {
    HwOpus* h = self->data;
    u64 t = plat_ns();
    int rc;
    if (!snx_audio_guest_ok(input.addr, input.size, PERM_R) || !snx_audio_guest_ok(output.addr, output.size, PERM_W))
        R_THROW(SNX_RC_CODEC_LIB_OPUS_INVALID_PACKET);
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
/* The work buffer sizes of the console's hwopus, from Ryujinx: libopus's decoder state, then the stream and frame
   buffers. Ryujinx gives the ExEx commands (8 and 9) no decoder state, as a placeholder: the console's value is not
   known. One game sizes its sound memory from command 9, and with the state in that size its sound init failed. The
   runtime decodes in host memory and never uses the work buffer, so the opens don't check its size */
enum {
    OPUS_STATE_1CH = 0x9564,        /* the decoder (0x5050), SILK (0x2160) and CELT (0x23b4) for one channel */
    OPUS_STATE_2CH = 0xbf74,        /* 0x5850, 0x2160 and 0x45c4 for two */
    OPUS_STREAM_1CH = 0x4564,       /* a multistream decoder's mono stream: the state less its 0x5000 decoder */
    OPUS_STREAM_2CH = 0x6774,       /* a stereo stream: the state less its 0x5800 decoder */
    OPUS_MULTISTREAM_BASE = 0xb920, /* the multistream decoder itself */
};
/* flags: bit 0 asks for frames of 120 ms, not 40 ms. dsp: an ExEx command */
static int work_size(u32 rate, u32 channels, u32 streams, u32 stereo_streams, int multi, u32 flags, int dsp,
                     u32* size) {
    if (multi ? !channels || channels > 255 : channels != 1 && channels != 2)
        R_THROW(SNX_RC_CODEC_INVALID_OPUS_CHANNEL_COUNT);
    R_UNLESS(opus_rate_ok(rate), SNX_RC_CODEC_INVALID_OPUS_SAMPLE_RATE);
    if (multi && (!streams || stereo_streams > streams || streams + stereo_streams > channels))
        R_THROW(SNX_RC_CODEC_INVALID_OPUS_SAMPLE_RATE); /* Eden's answer for a bad stream count */
    u32 frame = (u32)snx_align_up(channels * (flags & 1 ? 5760u : 1920u) / (48000u / rate), 64);
    u32 state = channels == 2 ? OPUS_STATE_2CH : OPUS_STATE_1CH;
    if (multi)
        state = OPUS_STREAM_1CH * (streams - stereo_streams) + OPUS_STREAM_2CH * stereo_streams + OPUS_MULTISTREAM_BASE;
    *size = (dsp ? 0 : state) + (multi ? (u32)snx_align_up(1500u * streams, 64) : 0x600u) + frame;
    R_SUCCEED();
}

static int open_decoder(u32 rate, u32 channels, u32 streams, u32 stereo_streams, const u8* mapping,
                        SvcObj** decoder) {
    SvcObj* o;
    /* the count and the take under the audio's lock, so two opens can't both take the last decoder */
    pm_lock(&g_host.audio.lk);
    if (hwopus_count() >= SNX_HWOPUS_MAX_DECODERS) {
        pm_unlock(&g_host.audio.lk);
        R_THROW(SNX_RC_CODEC_OUT_OF_OPUS_DECODERS);
    }
    o = nn_codec_detail_IHardwareOpusDecoder_new_data(g_hle.audio.opus, g_hle.audio.opus_next, "hwopus decoders");
    pm_unlock(&g_host.audio.lk);
    if (!hwopus_open(o->data, rate, channels, streams, stereo_streams, mapping)) {
        WARN_LIMIT(4, "hwopus: can't open a decoder of %lu channels at %lu Hz\n", (unsigned long)channels,
                   (unsigned long)rate);
        svcobj_unref(o);
        R_THROW(SNX_RC_CODEC_OUT_OF_OPUS_DECODERS);
    }
    o->destroy = hwopus_destroy;
    *decoder = o;
    R_SUCCEED();
}

/* a multistream parameter buffer: rate, channels, streams, coupled streams, then flags and a reserved word in the Ex
   form, then the channel mapping. v[4] is the flags, 0 without the Ex form */
static int multi_params(IpcBuf b, int ex, u32 v[5], const u8** mapping) {
    u64 need = ex ? 0x118 : 0x110;
    if (!b.addr || b.size < need || !snx_audio_guest_ok(b.addr, need, PERM_R)) return 0;
    memcpy(v, GP(b.addr), ex ? 20 : 16);
    snx_le32_array(v, ex ? 5 : 4);
    if (!ex) v[4] = 0;
    *mapping = GP(b.addr + (ex ? 0x18 : 0x10));
    return 1;
}

static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSize(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternal* params, u32* size) {
    (void)self;
    return work_size(params->sample_rate, params->channel_count, 1, 0, 0, 0, 0, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeEx(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternalEx* params, u32* size) {
    (void)self;
    return work_size(params->sample_rate, params->channel_count, 1, 0, 0, params->flags, 0, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeExEx(
    SvcObj* self, const nn_codec_detail_HardwareOpusDecoderParameterInternalEx* params, u32* size) {
    (void)self;
    return work_size(params->sample_rate, params->channel_count, 1, 0, 0, params->flags, 1, size);
}
/* parameters too short to read: the error of the opens */
static int multi_size(IpcBuf params, int ex, int dsp, u32* size) {
    u32 v[5];
    const u8* mapping;
    R_UNLESS(multi_params(params, ex, v, &mapping), SNX_RC_CODEC_OUT_OF_OPUS_DECODERS);
    return work_size(v[0], v[1], v[2], v[3], 1, v[4], dsp, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStream(SvcObj* self, IpcBuf params,
                                                                                      u32* size) {
    (void)self;
    return multi_size(params, 0, 0, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStreamEx(SvcObj* self,
                                                                                        IpcBuf params, u32* size) {
    (void)self;
    return multi_size(params, 1, 0, size);
}
static int nn_codec_detail_IHardwareOpusDecoderManager_GetWorkBufferSizeForMultiStreamExEx(SvcObj* self,
                                                                                          IpcBuf params,
                                                                                          u32* size) {
    (void)self;
    return multi_size(params, 1, 1, size);
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
    u32 v[5];
    const u8* mapping;
    R_UNLESS(multi_params(params, ex, v, &mapping), SNX_RC_CODEC_OUT_OF_OPUS_DECODERS);
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

SvcObj* new_hwopus(void) { return nn_codec_detail_IHardwareOpusDecoderManager_new(); }

/* ============================================================ save states */
void audio_state_drop(void) {
    u32 i;
    nnaudio_out_state_drop();
    for (i = 0; i < HWOPUS_POOL; i++)
        if (g_hle.audio.opus[i].obj) hwopus_free(&g_hle.audio.opus[i]);
}
void audio_state_rebuild(void) {
    u32 i;
    nnaudio_out_state_rebuild();
    for (i = 0; i < HWOPUS_POOL; i++) {
        HwOpus* h = &g_hle.audio.opus[i];
        u8 mapping[255];
        if (!h->obj) continue;
        memcpy(mapping, h->mapping, sizeof mapping);
#ifndef SNX_NO_H264
        h->dec = NULL;
        h->pkt = NULL;
        h->frame = NULL;
#endif
        h->queue = NULL;
        if (!hwopus_open(h, h->rate, h->channels, h->streams, h->stereo_streams, h->mapped ? mapping : NULL))
            WARN("state: an Opus decoder (%u Hz, %u channels) couldn't be made again\n", h->rate, h->channels);
    }
}
/* the functions a save state may hold of this file (state_fns.h) */
STATE_FN_TABLE(g_audio_state_fns,
               STATE_FN("audio", device_destroy),
               STATE_FN("audio", hwopus_destroy));
