/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors
   Ported from Eden (src/core/hle/service/audio/audio_renderer_manager.cpp, audio_renderer.cpp, errors.h;
   src/audio_core/renderer/system.cpp, common/feature_support.h):
   SPDX-FileCopyrightText: Copyright 2025-2026 Eden Emulator Project
   SPDX-FileCopyrightText: Copyright 2018-2024 yuzu Emulator Project (GPL-2.0-or-later) */
/* nn::audio's audio renderer, native (audio_renderer.hook): OpenAudioRenderer and the functions that take the handle it
   fills, with no audren:u session. The renderer itself is audren.c's and audren_dsp.c's.

   A game describes its voices, mixes, effects and sinks to the SDK, which keeps them in the game's own
   AudioRendererConfig; RequestUpdateAudioRenderer writes them out as one update (a header and a section from each of
   the SDK's managers), hands it to the session, and reads the answer back into the same managers. The update's
   format is the renderer's own, by revision, and audren_dsp.c reads it. Here RequestUpdateAudioRenderer does the
   same work in the same order with no session: it runs the SDK's managers on the game's buffers (each is a function
   the SDK exports, found by its class and method, as its argument types differ between SDKs), gives the update to
   the renderer, and runs the managers on what it answered. Where the config keeps its buffers differs between SDKs:
   audio_renderer.hook has the layout of each SDK read.

   The handle is two words, the SDK's session object and a context of its own, and the session object alone before
   SDK 2: every function that takes one is answered here, so the first is the renderer's name (audren.c's, its slot
   plus 1) and the second is unused.

   Before SDK 2 the game's AudioRendererParameter is the session's own, handed over as it is, and the voices' update
   is two of the VoiceInfoManager's methods, one a section; before SDK 1 a config has no SplitterInfoManager and an
   update no splitter section.

   A 32-bit game (g_arm_mod; SDK 0.12.16 is the one read) calls the same handlers through audio_renderer.hook's a32
   declarations. Its handle is a word of 4 bytes, its config holds its managers and buffers in 4-byte fields
   (AudioRendererConfig32), and a manager's method leaves its result in r0 */
#include "nx/nn/audio.h"
#include "nx/hle/hook/hook.h"
#include "hle/hook/hooks.hook.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/audren_dsp.h"
#include "nx/runtime/arm.h"
#include "nx/runtime/platform/log.h"
#include "common/utilities/bits.h"

/* the newest renderer revision understood: Eden's CurrentRevision, 15, and the Ounce's 16 (REV@, and REv@ with its
   lowercase v), which an SDK 22 game asks for. No feature of 16's is known: Eglantine (Eden's Ounce fork) takes its
   native 16 as 15, and so does the renderer here (audren_dsp.c's features go by revision 15 and earlier) */
#define REN_REVISION 16
#define REN_STARTED  0 /* nn::audio::AudioRendererState */
#define REN_STOPPED  1
#define REN_HEADER   0x40 /* an update's header, and its answer's */
#define REN_INFO     0x10 /* the render info after the answer's sections: the frames rendered so far */

/* a pointer in the game's memory: 8 bytes, 4 in a 32-bit game */
static u32 word_size(void) { return g_arm_mod ? 4 : 8; }
static u64 word_at(const CPU* c, u64 addr) {
    return g_arm_mod ? snx_ld_le32(hook_host(c, addr)) : snx_ld_le64(hook_host(c, addr));
}
static void word_put(const CPU* c, u64 addr, u64 value) {
    if (g_arm_mod)
        snx_st_le32(hook_host(c, addr), (u32)value);
    else
        snx_st_le64(hook_host(c, addr), value);
}

/* the parameters as the session takes them (AudioRendererParameterInternal), which audren_state_new reads as bytes */
typedef struct {
    u32 sample_rate, sample_count, mix_buffer_count, sub_mix_count, voice_count, sink_count, effect_count,
        performance_frame_count;
    u8 is_voice_drop_enabled, reserved, rendering_device, execution_mode;
    u32 splitter_count, splitter_send_channel_count, external_context_size, revision;
} Params;
#define PARAMS_SIZE 0x34

/* 'REV' and the revision's digit, or a bare number, as Eden's GetRevisionNum reads it */
static u32 revision_number(u32 rev) {
    if (rev >= 0x100) rev = (rev - 0x30564552u) >> 24; /* 'R' 'E' 'V' '0' */
    return rev;
}

/* the work buffer the game gives OpenAudioRenderer, in the shape of the console's (Eden's System::GetWorkBufferSize).
   The runtime keeps its state in host memory and never touches it, so the sizes of the console's structures are
   generous estimates here */
static u64 work_buffer_size(const Params* p) {
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
    size += (u64)p->effect_count * 0x1B0;                    /* EffectInfoBase */
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

/* the game's AudioRendererParameter as the SDK hands it to its session: the first 32 bytes and the splitter counts as
   they are, the voice drop flag, and the rendering device and execution mode as a byte each. The revision is the
   last word, where audio_renderer.hook's layout has it; an SDK that keeps a word before it there (6 to 8) hands that
   one over as the external context's size. Before SDK 2 the game's struct is the session's, its revision at 0x30 */
static void params_read(const CPU* c, const HookLayout_nn_audio_AudioRendererParameter* L, u64 param, Params* p,
                        u8 raw[PARAMS_SIZE]) {
    const u8* g = hook_host(c, param);
    if (L->revision.offset == 0x30)
        memcpy(raw, g, PARAMS_SIZE);
    else {
        memset(raw, 0, PARAMS_SIZE);
        memcpy(raw, g, 0x20);
        raw[0x20] = g[0x20];
        raw[0x22] = g[0x2c];
        raw[0x23] = g[0x30];
        memcpy(raw + 0x24, g + 0x24, 8);
        snx_st_le32(raw + 0x2c, L->revision.offset > 0x34 ? snx_ld_le32(g + 0x34) : 0);
        snx_st_le32(raw + 0x30, hook_get_u32(c, L->revision, param));
    }
    p->sample_rate = snx_ld_le32(raw), p->sample_count = snx_ld_le32(raw + 4);
    p->mix_buffer_count = snx_ld_le32(raw + 8), p->sub_mix_count = snx_ld_le32(raw + 12);
    p->voice_count = snx_ld_le32(raw + 16), p->sink_count = snx_ld_le32(raw + 20);
    p->effect_count = snx_ld_le32(raw + 24), p->performance_frame_count = snx_ld_le32(raw + 28);
    p->is_voice_drop_enabled = raw[0x20], p->reserved = 0, p->rendering_device = raw[0x22];
    p->execution_mode = raw[0x23];
    p->splitter_count = snx_ld_le32(raw + 0x24), p->splitter_send_channel_count = snx_ld_le32(raw + 0x28);
    p->external_context_size = snx_ld_le32(raw + 0x2c), p->revision = snx_ld_le32(raw + 0x30);
}

u64 nnaudio_get_renderer_work_buffer_size(CPU* c, const HookLayout_nn_audio_AudioRendererParameter* L, u64 param) {
    Params p;
    u8 raw[PARAMS_SIZE];
    params_read(c, L, param, &p, raw);
    if (revision_number(p.revision) > REN_REVISION)
        WARN_LIMIT(4, "audren: a work buffer asked for revision %u, and the newest understood is %d: sized as %d\n",
                   revision_number(p.revision), REN_REVISION, REN_REVISION);
    return work_buffer_size(&p);
}

/* ------------------------------------------------------------ opening and closing */
/* a function of the SDK's own that a handler runs in the game, by its exact name: 0 when it exports none */
static u64 sdk_function(const char* symbol, const char* why) {
    u64 fn = kernel_find_symbol_exact(symbol);
    if (!fn) WARN_LIMIT(4, "audren: the SDK exports no %s, which %s needs\n", symbol, why);
    return fn;
}

/* words: how many the game's AudioRendererHandle holds, 2, or 1 before SDK 2 */
static u32 open_renderer(CPU* c, const HookLayout_nn_audio_AudioRendererParameter* L, u64 handle, u64 event, u64 param,
                         int words) {
    Params p;
    u8 raw[PARAMS_SIZE];
    ARRen* r;
    u32 id;
    params_read(c, L, param, &p, raw);
    if (revision_number(p.revision) > REN_REVISION) {
        WARN_LIMIT(4,
                   "audren: OpenAudioRenderer asks for revision %u, and the newest understood is %d: InvalidRevision\n",
                   revision_number(p.revision), REN_REVISION);
        return SNX_RC_AUDIO_INVALID_REVISION;
    }
    id = audren_open(raw, p.execution_mode == 1);
    if (!id) {
        WARN("audren: can't open a renderer of %lu voices, %lu submixes, %lu effects, %lu sinks at %lu Hz\n",
             (unsigned long)p.voice_count, (unsigned long)p.sub_mix_count, (unsigned long)p.effect_count,
             (unsigned long)p.sink_count, (unsigned long)p.sample_rate);
        return SNX_RC_AUDIO_OPERATION_FAILED;
    }
    word_put(c, handle, id);
    if (words == 2) word_put(c, handle + word_size(), 0);
    if (!event) return 0;
    r = audren_at(id);
    if (r->manual && p.rendering_device == 1) {
        /* a renderer the game executes itself has no event of the session's: the SDK makes the game's SystemEvent
           one of its own, auto clear, and signals it after each ExecuteAudioRendererRendering */
        u64 create = sdk_function("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb",
                                  "OpenAudioRenderer for a manual renderer's event");
        if (create) {
            hook_guest_call(c, create, event, 1, 0, 0);
            r->manual_event = event;
        }
    } else if (!hook_attach_handle(c, event, ev_handle(g_hle.audio.ev)))
        WARN("audren: the SDK exports no AttachReadableHandleToSystemEvent: the renderer has no event, and a game "
             "that waits for one waits forever\n");
    return 0;
}
/* the renderer's state is the runtime's: the game's work buffer stays untouched */
u32 nnaudio_open_renderer_event(CPU* c, const HookLayout_nn_audio_AudioRendererParameter* L, u64 handle, u64 event,
                                u64 param, u64 work, u64 size) {
    (void)work, (void)size;
    return open_renderer(c, L, handle, event, param, 2);
}
u32 nnaudio_open_renderer(CPU* c, const HookLayout_nn_audio_AudioRendererParameter* L, u64 handle, u64 param, u64 work,
                          u64 size) {
    (void)work, (void)size;
    return open_renderer(c, L, handle, 0, param, 2);
}
u32 nnaudio_open_renderer_event_word(CPU* c, const HookLayout_nn_audio_AudioRendererParameter* L, u64 handle, u64 event,
                                     u64 param, u64 work, u64 size) {
    (void)work, (void)size;
    return open_renderer(c, L, handle, event, param, 1);
}
u32 nnaudio_open_renderer_word(CPU* c, const HookLayout_nn_audio_AudioRendererParameter* L, u64 handle, u64 param,
                               u64 work, u64 size) {
    (void)work, (void)size;
    return open_renderer(c, L, handle, 0, param, 1);
}

/* the renderer a handle names, which what names in the log when there is none */
static ARRen* renderer(u64 id, const char* what) {
    ARRen* r = id <= SNX_AUDREN_MAX ? audren_at((u32)id) : NULL;
    if (!r)
        WARN_LIMIT(8, "audren: %s on the handle %#llx, which names no open renderer\n", what, (unsigned long long)id);
    return r;
}

void nnaudio_close_renderer(CPU* c, u64 id, u64 context) {
    (void)c, (void)context;
    if (renderer(id, "CloseAudioRenderer")) audren_close((u32)id);
}

/* ------------------------------------------------------------ the handle's other functions */
u32 nnaudio_start_renderer(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "StartAudioRenderer");
    (void)c, (void)context;
    if (!r) return SNX_RC_AUDIO_INVALID_HANDLE;
    __atomic_store_n(&r->state, SNX_AUDREN_STARTED, __ATOMIC_RELEASE);
    return 0;
}
void nnaudio_stop_renderer(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "StopAudioRenderer");
    (void)c, (void)context;
    if (r) __atomic_store_n(&r->state, SNX_AUDREN_STOPPED, __ATOMIC_RELEASE);
}
u32 nnaudio_get_renderer_state(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "GetAudioRendererState");
    (void)c, (void)context;
    return r && __atomic_load_n(&r->state, __ATOMIC_ACQUIRE) == SNX_AUDREN_STARTED ? REN_STARTED : REN_STOPPED;
}
i32 nnaudio_get_renderer_sample_rate(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "GetAudioRendererSampleRate");
    (void)c, (void)context;
    return r ? (i32)r->sample_rate : 0;
}
i32 nnaudio_get_renderer_sample_count(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "GetAudioRendererSampleCount");
    (void)c, (void)context;
    return r ? (i32)r->sample_count : 0;
}
i32 nnaudio_get_renderer_mix_buffer_count(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "GetAudioRendererMixBufferCount");
    (void)c, (void)context;
    return r ? (i32)r->mix_buffer_count : 0;
}
void nnaudio_set_renderer_time_limit(CPU* c, u64 id, u64 context, i32 percent) {
    ARRen* r = renderer(id, "SetAudioRendererRenderingTimeLimit");
    (void)c, (void)context;
    if (r) __atomic_store_n(&r->limit, (u32)percent, __ATOMIC_RELAXED);
}
/* a one-word handle's: the limit is the second argument */
void nnaudio_set_renderer_time_limit_word(CPU* c, u64 id, i32 percent) {
    nnaudio_set_renderer_time_limit(c, id, 0, percent);
}
i32 nnaudio_get_renderer_time_limit(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "GetAudioRendererRenderingTimeLimit");
    (void)c, (void)context;
    return r ? (i32)__atomic_load_n(&r->limit, __ATOMIC_RELAXED) : 0;
}
/* the voice drop parameter reads back as set: no voice is dropped here */
void nnaudio_set_renderer_voice_drop(CPU* c, u64 id, u64 context, f32 parameter) {
    ARRen* r = renderer(id, "SetAudioRendererVoiceDropParameter");
    (void)c, (void)context;
    if (r) __atomic_store(&r->voice_drop, &parameter, __ATOMIC_RELAXED);
}
f32 nnaudio_get_renderer_voice_drop(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "GetAudioRendererVoiceDropParameter");
    f32 parameter = 1.0f;
    (void)c, (void)context;
    if (r) __atomic_load(&r->voice_drop, &parameter, __ATOMIC_RELAXED);
    return parameter;
}
i32 nnaudio_get_renderer_execution_mode(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "GetAudioRendererExecutionMode");
    (void)c, (void)context;
    return r ? (i32)r->manual : 0;
}
i32 nnaudio_get_renderer_rendering_device(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "GetAudioRendererRenderingDevice");
    (void)c, (void)context;
    return r ? (i32)r->rendering_device : 0;
}
/* the event the clock signals after each frame, attached to the game's SystemEvent; a renderer the game executes by
   itself has none */
u32 nnaudio_query_renderer_event(CPU* c, u64 handle, u64 event) {
    ARRen* r = renderer(handle ? word_at(c, handle) : 0, "QueryAudioRendererSystemEvent");
    if (!r) return SNX_RC_AUDIO_INVALID_HANDLE;
    if (r->manual) return SNX_RC_AUDIO_NOT_SUPPORTED;
    return hook_attach_handle(c, event, ev_handle(g_hle.audio.ev)) ? 0 : SNX_RC_AUDIO_OPERATION_FAILED;
}
/* a manual renderer's frame, which the session's command only acknowledged: nothing is rendered, and the SDK then
   signals the event OpenAudioRenderer made */
u32 nnaudio_execute_renderer(CPU* c, u64 id, u64 context) {
    ARRen* r = renderer(id, "ExecuteAudioRendererRendering");
    u64 signal;
    (void)context;
    if (!r) return SNX_RC_AUDIO_INVALID_HANDLE;
    if (r->manual_event && (signal = sdk_function("_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE",
                                                  "ExecuteAudioRendererRendering")) != 0)
        hook_guest_call(c, signal, r->manual_event, 0, 0, 0);
    return 0;
}

/* ------------------------------------------------------------ RequestUpdateAudioRenderer */
/* the SDK's managers RequestUpdateAudioRenderer runs, each a method of the manager the config holds at its offset:
   the eight that write a section of the update, in the order they are written, then the six that read a section of
   the answer, in theirs, the two of the BehaviorManager around them, and the two that write the voices' sections
   before SDK 2, where UpdateVoiceRelatedParameters writes both */
enum {
    M_BEHAVIOR_IN,
    M_POOL_IN,
    M_VOICE_IN,
    M_EFFECT_IN,
    M_SPLITTER_IN,
    M_MIX_IN,
    M_SINK_IN,
    M_PERFORMANCE_IN,
    M_POOL_OUT,
    M_VOICE_OUT,
    M_EFFECT_OUT,
    M_SINK_OUT,
    M_PERFORMANCE_OUT,
    M_BEHAVIOR_OUT,
    M_REVISION,
    M_CHECK_ERRORS,
    M_VOICE_RESOURCE_IN,
    M_VOICE_INFO_IN,
    M_COUNT
};
_Static_assert(M_COUNT == SNX_AUDREN_METHODS, "g_hle.audio.ren_fn holds one address a method");
static const struct {
    const char* method; /* its mangled class and method, after _ZN2nn5audio or _ZNK2nn5audio */
    u8 manager;         /* the manager's place in the config */
    u8 header;          /* the header's word that holds the section's size */
} g_methods[M_COUNT] = {
    [M_BEHAVIOR_IN] = {"15BehaviorManager17UpdateInParameterE", 0x30, 0x04},
    [M_POOL_IN] = {"17MemoryPoolManager17UpdateInParameterE", 0x28, 0x08},
    [M_VOICE_IN] = {"16VoiceInfoManager28UpdateVoiceRelatedParametersE", 0x00, 0x0c},
    [M_EFFECT_IN] = {"13EffectManager24UpdateEffectsInParameterE", 0x10, 0x14},
    [M_SPLITTER_IN] = {"19SplitterInfoManager17UpdateInParameterE", 0x38, 0x24},
    [M_MIX_IN] = {"10MixManager20UpdateMixInParameterE", 0x08, 0x18},
    [M_SINK_IN] = {"11SinkManager21UpdateSinkInParameterE", 0x18, 0x1c},
    [M_PERFORMANCE_IN] = {"24PerformanceBufferManager17UpdateInParameterE", 0x20, 0x20},
    [M_POOL_OUT] = {"17MemoryPoolManager15UpdateOutStatusE", 0x28, 0x08},
    [M_VOICE_OUT] = {"16VoiceInfoManager20UpdateVoiceOutStatusE", 0x00, 0x0c},
    [M_EFFECT_OUT] = {"13EffectManager21UpdateEffectOutStatusE", 0x10, 0x14},
    [M_SINK_OUT] = {"11SinkManager19UpdateSinkOutStatusE", 0x18, 0x1c},
    [M_PERFORMANCE_OUT] = {"24PerformanceBufferManager15UpdateOutStatusE", 0x20, 0x20},
    [M_BEHAVIOR_OUT] = {"15BehaviorManager15UpdateOutStatusE", 0x30, 0x04},
    [M_REVISION] = {"15BehaviorManager11GetRevisionE", 0x30, 0},
    [M_CHECK_ERRORS] = {"15BehaviorManager11CheckErrorsE", 0x30, 0},
    [M_VOICE_RESOURCE_IN] = {"16VoiceInfoManager37UpdateVoiceChannelResourceInParameterE", 0x00, 0x10},
    [M_VOICE_INFO_IN] = {"16VoiceInfoManager22UpdateVoiceInParameterE", 0x00, 0x0c},
};
#define HEADER_VOICE_RESOURCE 0x10 /* the second size UpdateVoiceRelatedParameters gives */
#define HEADER_INFO           0x28 /* the answer's render info's size */
#define HEADER_TOTAL          0x3c

/* how an SDK's RequestUpdateAudioRenderer differs from the newest's */
#define UPDATE_VOICE_SPLIT 1u /* the voices' two sections each by its own method (before SDK 2) */
#define UPDATE_NO_SPLITTER 2u /* no SplitterInfoManager, and no splitter section (before SDK 1) */

/* whether an update of those differences runs method m */
static int method_used(int m, u32 differences) {
    if (m == M_VOICE_IN) return !(differences & UPDATE_VOICE_SPLIT);
    if (m == M_VOICE_RESOURCE_IN || m == M_VOICE_INFO_IN) return (differences & UPDATE_VOICE_SPLIT) != 0;
    if (m == M_SPLITTER_IN) return !(differences & UPDATE_NO_SPLITTER);
    return 1;
}
/* the managers' functions in the game's SDK, found once (g_hle.audio.ren_fn): 1 when it exports every one an update
   of those differences runs */
static int methods_find(u32 differences) {
    static const char* const prefix[2] = {"_ZN2nn5audio", "_ZNK2nn5audio"};
    HleAudio* a = &g_hle.audio;
    const char* name;
    u64 addr;
    u32 i, k, missing = 0;
    if (a->ren_fn_found) return a->ren_fn_found == 1;
    for (i = 0; kernel_symbol(i, &addr, &name); i++) {
        const char* rest = NULL;
        if (!name) continue;
        for (k = 0; k < 2 && !rest; k++)
            if (!strncmp(name, prefix[k], strlen(prefix[k]))) rest = name + strlen(prefix[k]);
        for (k = 0; rest && k < M_COUNT; k++)
            if (!a->ren_fn[k] && !strncmp(rest, g_methods[k].method, strlen(g_methods[k].method))) a->ren_fn[k] = addr;
    }
    for (k = 0; k < M_COUNT; k++)
        if (!a->ren_fn[k] && method_used((int)k, differences)) {
            WARN("audren: the SDK exports no nn::audio function named %s..., which RequestUpdateAudioRenderer runs: "
                 "updates fail with OperationFailed (%#x)\n",
                 g_methods[k].method, SNX_RC_AUDIO_OPERATION_FAILED);
            missing++;
        }
    a->ren_fn_found = missing ? 2 : 1;
    return a->ren_fn_found == 1;
}
/* method m of its manager in the config, on up to three arguments: what it returns. A 32-bit game's config holds
   the same managers in the same order, a 4-byte pointer each */
static u64 method(CPU* c, u64 config, int m, u64 a1, u64 a2, u64 a3) {
    u64 manager = word_at(c, config + g_methods[m].manager / (8 / word_size()));
    hook_guest_call(c, g_hle.audio.ren_fn[m], manager, a1, a2, a3);
    return g_arm_mod ? arm_cpu(c)->r[0] : c->x[0];
}

/* the update, into and out of the config's two buffers: frames is where the config keeps the address the SDK writes
   the rendered frames' count to, 0 for an SDK that keeps none (before 6) */
static u32 request_update(CPU* c, u64 id, u64 config, u64 in, u64 in_size, u64 out, u64 out_size, u32 frames,
                          u32 differences) {
    int one = !(differences & UPDATE_VOICE_SPLIT); /* the voices' sizes come back through two words of the stack */
    u64 at, sp = c->x[31], slots = 0;
    u8* header;
    u32 total = REN_HEADER, n, rc;
    int m;
    if (!renderer(id, "RequestUpdateAudioRenderer")) return SNX_RC_AUDIO_INVALID_HANDLE;
    if (!methods_find(differences)) return SNX_RC_AUDIO_OPERATION_FAILED;
    if (!in || in_size < REN_HEADER || !out || out_size < REN_HEADER) {
        WARN_LIMIT(4,
                   "audren: an AudioRendererConfig with %llu bytes for the update and %llu for its answer, under a "
                   "header's %d: InvalidUpdateInfo\n",
                   (unsigned long long)in_size, (unsigned long long)out_size, REN_HEADER);
        return SNX_RC_AUDIO_INVALID_UPDATE_INFO;
    }
    /* two words of the game's stack for UpdateVoiceRelatedParameters' sizes, below the handler's caller's frame and
       above the frames of the functions run from here */
    if (one) {
        c->x[31] = sp - 0x20;
        slots = c->x[31];
    }
    header = hook_host(c, in);
    memset(header, 0, REN_HEADER);
    snx_st_le32(header, (u32)method(c, config, M_REVISION, 0, 0, 0));
    at = in + REN_HEADER;
    for (m = M_BEHAVIOR_IN; m <= M_PERFORMANCE_IN; m++) {
        if (!method_used(m, differences)) {
            if (m != M_VOICE_IN) continue;
            /* the channel resources, then the voices, each at the other's end */
            n = (u32)method(c, config, M_VOICE_RESOURCE_IN, at, 0, 0);
            snx_st_le32(header + HEADER_VOICE_RESOURCE, n);
            total += n;
            at += n;
            n = (u32)method(c, config, M_VOICE_INFO_IN, at, 0, 0);
        } else if (m == M_VOICE_IN) {
            snx_st_le64(hook_host(c, slots), 0);
            snx_st_le64(hook_host(c, slots + 8), 0);
            method(c, config, m, at, slots, slots + 8);
            n = (u32)snx_ld_le64(hook_host(c, slots));
            snx_st_le32(header + HEADER_VOICE_RESOURCE, (u32)snx_ld_le64(hook_host(c, slots + 8)));
            total += (u32)snx_ld_le64(hook_host(c, slots + 8));
            at += snx_ld_le64(hook_host(c, slots + 8));
        } else
            n = (u32)method(c, config, m, at, 0, 0);
        snx_st_le32(header + g_methods[m].header, n);
        total += n;
        at += n;
    }
    snx_st_le32(header + HEADER_TOTAL, total);
    if (total > in_size) {
        if (one) c->x[31] = sp;
        WARN_LIMIT(4, "audren: an update of %u bytes in a buffer of %llu: InvalidUpdateInfo\n", total,
                   (unsigned long long)in_size);
        return SNX_RC_AUDIO_INVALID_UPDATE_INFO;
    }
    rc = (u32)audren_update((u32)id, in, total, out, out_size);
    if (!rc) {
        /* the answer's sections, each as long as its header says, or the answer is not one */
        header = hook_host(c, out);
        at = out + REN_HEADER;
        for (m = M_POOL_OUT; m <= M_BEHAVIOR_OUT && !rc; m++) {
            n = (u32)method(c, config, m, at, 0, 0);
            if (n != snx_ld_le32(header + g_methods[m].header)) rc = SNX_RC_AUDIO_INVALID_UPDATE_INFO;
            at += n;
        }
        if (!rc && frames) {
            snx_st_le64(hook_host(c, snx_ld_le64(hook_host(c, config + frames))), snx_ld_le64(hook_host(c, at)));
            if (snx_ld_le32(header + HEADER_INFO) != REN_INFO) rc = SNX_RC_AUDIO_INVALID_UPDATE_INFO;
        }
        if (!rc && snx_ld_le32(header + HEADER_TOTAL) != out_size) rc = SNX_RC_AUDIO_INVALID_UPDATE_INFO;
        if (rc)
            WARN_LIMIT(4, "audren: the renderer's answer doesn't have the sections its header says: "
                          "InvalidUpdateInfo\n");
        else
            rc = (u32)method(c, config, M_CHECK_ERRORS, 0, 0, 0);
    } else if (rc == SNX_RC_AUDIO_INVALID_UPDATE_INFO)
        method(c, config, M_CHECK_ERRORS, 0, 0, 0); /* the SDK reads the renderer's errors, then gives the result */
    if (one) c->x[31] = sp;
    return rc;
}
/* the config's buffers, where an SDK's layout has them */
#define BUFFERS(c, L, config)                                                                                          \
    hook_get_addr(c, (L)->in, config), hook_get_u64(c, (L)->in_size, config), hook_get_addr(c, (L)->out, config),      \
        hook_get_u64(c, (L)->out_size, config)
u32 nnaudio_request_update(CPU* c, const HookLayout_nn_audio_AudioRendererConfig* L, u64 id, u64 context, u64 config) {
    (void)context;
    return request_update(c, id, config, BUFFERS(c, L, config), 0, 0);
}
/* from SDK 6 the config holds, at 0x40, where the rendered frames' count goes */
u32 nnaudio_request_update_frames(CPU* c, const HookLayout_nn_audio_AudioRendererConfig* L, u64 id, u64 context,
                                  u64 config) {
    (void)context;
    return request_update(c, id, config, BUFFERS(c, L, config), 0x40, 0);
}
/* SDK 1, with its one-word handle */
u32 nnaudio_request_update_word(CPU* c, const HookLayout_nn_audio_AudioRendererConfig* L, u64 id, u64 config) {
    return request_update(c, id, config, BUFFERS(c, L, config), 0, UPDATE_VOICE_SPLIT);
}
/* SDK 0, which has no splitters yet */
u32 nnaudio_request_update_first(CPU* c, const HookLayout_nn_audio_AudioRendererConfig* L, u64 id, u64 config) {
    return request_update(c, id, config, BUFFERS(c, L, config), 0, UPDATE_VOICE_SPLIT | UPDATE_NO_SPLITTER);
}
/* a 32-bit game's, SDK 0 */
u32 nnaudio_request_update_first32(CPU* c, const HookLayout_nn_audio_AudioRendererConfig32* L, u64 id, u64 config) {
    return request_update(c, id, config, hook_get_u32(c, L->in, config), hook_get_u32(c, L->in_size, config),
                          hook_get_u32(c, L->out, config), hook_get_u32(c, L->out_size, config), 0,
                          UPDATE_VOICE_SPLIT | UPDATE_NO_SPLITTER);
}
