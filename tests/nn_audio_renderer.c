/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The native audio renderer (src/nx/nn/audio_renderer.c), its handlers called as the hooks call them, on a stand-in
   CPU over a guest buffer, with audren.c's renderers over a stand-in DSP and a stand-in SDK: the managers
   RequestUpdateAudioRenderer runs are functions of the test's, found by their class and method under the names two
   SDKs give them, which write and read sections of known sizes. Checked: the parameters the renderer is opened with
   for each layout, the handle and what its functions answer, the update's header, sections and order, the answer's
   sections and the frames' count, each failure's result, and audio_renderer.hook's table by SDK version */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>

#include "nx/nn/audio_renderer.c"
#include "nx/hle/services/audren.c"
#include "nx/hle/hle.c"
#include "hle/hook/hooks.hook.inc" /* audio_renderer.hook's table and adapters */

u8* g_mem;
u64 g_as_size = AS_MAX; /* the reservation's size, mem.c's, which this test does not link */
static unsigned test_checks;
static void test_fail(const char* expression, int line) {
    fprintf(stderr, "nn_audio_renderer.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void pm_lock_announce(PMutex* m, const char* name) { (void)m, (void)name; }
static char test_last_log[256]; /* the last warning */
void logf_(int level, const char* format, ...) {
    va_list ap;
    if (level) return;
    va_start(ap, format);
    vsnprintf(test_last_log, sizeof test_last_log, format, ap);
    va_end(ap);
}
struct PAudio {
    int unused;
};
PAudio* plat_audio_open(u32 rate, u32 channels) {
    (void)rate, (void)channels;
    return NULL;
}
void plat_audio_close(PAudio* a) { (void)a; }
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes) {
    (void)a, (void)samples, (void)bytes;
    return 1;
}
u32 plat_audio_queued(PAudio* a) {
    (void)a;
    return 0;
}
void plat_audio_start(PAudio* a, int started) { (void)a, (void)started; }
void plat_audio_volume(PAudio* a, float volume) { (void)a, (void)volume; }
u32 ev_handle(u32 ev) { return ev == 0x77 ? 7 : 0; }

/* ---- the stand-in DSP: the parameters a renderer was made with, the update it was given, and its answer */
static u8 test_params[0x34];
static int test_refuse, test_update_result;
static unsigned test_states, test_updates;
static u64 test_in_size, test_out_size;
static u8 test_in[0x400];
ARState* audren_state_new(const u8* params) {
    ARState* s;
    memcpy(test_params, params, sizeof test_params);
    if (test_refuse) return NULL;
    s = calloc(1, sizeof *s);
    CHECK(s != NULL);
    test_states++;
    return s;
}
void audren_state_free(ARState* s) {
    if (s) test_states--;
    free(s);
}
/* the answer's section sizes, as its header gives them: memory pool, voice, effect, sink, performance, behavior */
static u32 test_answer[6] = {0x20, 0x30, 0x10, 0x40, 0x10, 0xb0};
#define ANSWER_TOTAL (0x40 + 0x20 + 0x30 + 0x10 + 0x40 + 0x10 + 0xb0 + 0x10)
int audren_state_update(ARState* s, const u8* in, u64 in_size, u8* out, u64 out_size) {
    static const u8 word[6] = {0x08, 0x0c, 0x14, 0x1c, 0x20, 0x04};
    u32 at = 0x40;
    (void)s;
    test_updates++;
    test_in_size = in_size;
    test_out_size = out_size;
    CHECK(in_size <= sizeof test_in);
    memcpy(test_in, in, (size_t)in_size);
    if (test_update_result) return test_update_result;
    memset(out, 0, 0x40);
    for (int i = 0; i < 6; i++) {
        snx_st_le32(out + word[i], test_answer[i]);
        at += test_answer[i];
    }
    snx_st_le32(out + 0x28, 0x10);
    snx_st_le64(out + at, 0x123456789ull); /* the frames rendered so far */
    snx_st_le32(out + 0x3c, at + 0x10);
    return 0;
}
int audren_revision(u32 rev) { return (int)(rev >> 24) - '0'; }
int audren_frame(ARState* s, i16* stereo) {
    (void)s, (void)stereo;
    return 0;
}

/* ---- the stand-in SDK: each manager's method at its own address, named as SDK 3.5 or SDK 20.5 names it */
#define SDK_FN(k)  (0x9000u + 0x10u * (u32)(k))
#define SDK_CREATE 0xa000u
#define SDK_SIGNAL 0xa010u
static const char* const test_names[2][M_COUNT] = {
    {"_ZNK2nn5audio15BehaviorManager17UpdateInParameterEPv", "_ZN2nn5audio17MemoryPoolManager17UpdateInParameterEPv",
     "_ZN2nn5audio16VoiceInfoManager28UpdateVoiceRelatedParametersEPvPmS3_",
     "_ZN2nn5audio13EffectManager24UpdateEffectsInParameterEPv",
     "_ZN2nn5audio19SplitterInfoManager17UpdateInParameterEPv", "_ZN2nn5audio10MixManager20UpdateMixInParameterEPv",
     "_ZN2nn5audio11SinkManager21UpdateSinkInParameterEPNS0_6common15SinkInParameterE",
     "_ZN2nn5audio24PerformanceBufferManager17UpdateInParameterEPv",
     "_ZN2nn5audio17MemoryPoolManager15UpdateOutStatusEPv",
     "_ZN2nn5audio16VoiceInfoManager20UpdateVoiceOutStatusEPNS0_9VoiceInfo9OutStatusE",
     "_ZN2nn5audio13EffectManager21UpdateEffectOutStatusEPNS0_10EffectInfo9OutStatusE",
     "_ZN2nn5audio11SinkManager19UpdateSinkOutStatusEPNS0_6common13SinkOutStatusE",
     "_ZN2nn5audio24PerformanceBufferManager15UpdateOutStatusEPKv",
     "_ZN2nn5audio15BehaviorManager15UpdateOutStatusEPKv", "_ZNK2nn5audio15BehaviorManager11GetRevisionEv",
     "_ZN2nn5audio15BehaviorManager11CheckErrorsEv"},
    {"_ZNK2nn5audio15BehaviorManager17UpdateInParameterEPNS0_8renderer3ipc8Behavior10UpdateDataILi0EEE",
     "_ZN2nn5audio17MemoryPoolManager17UpdateInParameterEPNS0_8renderer3ipc10MemoryPool10UpdateDataILi0EEE",
     "_ZN2nn5audio16VoiceInfoManager28UpdateVoiceRelatedParametersEPvPmS3_",
     "_ZN2nn5audio13EffectManager24UpdateEffectsInParameterEPNS0_8renderer3ipc6Effect10UpdateDataILi2EEE",
     "_ZN2nn5audio19SplitterInfoManager17UpdateInParameterEPv", "_ZN2nn5audio10MixManager20UpdateMixInParameterEPv",
     "_ZN2nn5audio11SinkManager21UpdateSinkInParameterEPNS0_8renderer3ipc4Sink10UpdateDataILi0EEE",
     "_ZN2nn5audio24PerformanceBufferManager17UpdateInParameterEPNS0_8renderer3ipc18PerformanceMetrics10UpdateDataILi0EEE",
     "_ZN2nn5audio17MemoryPoolManager15UpdateOutStatusEPKNS0_8renderer3ipc10MemoryPool10StatusDataILi0EEE",
     "_ZN2nn5audio16VoiceInfoManager20UpdateVoiceOutStatusEPKNS0_8renderer3ipc5Voice10StatusDataILi0EEE",
     "_ZN2nn5audio13EffectManager21UpdateEffectOutStatusEPKNS0_8renderer3ipc6Effect10StatusDataILi1EEE",
     "_ZN2nn5audio11SinkManager19UpdateSinkOutStatusEPKNS0_8renderer3ipc4Sink10StatusDataILi0EEE",
     "_ZN2nn5audio24PerformanceBufferManager15UpdateOutStatusEPKNS0_8renderer3ipc18PerformanceMetrics10StatusDataILi0EEE",
     "_ZN2nn5audio15BehaviorManager15UpdateOutStatusEPKNS0_8renderer3ipc8Behavior10StatusDataILi0EEE",
     "_ZNK2nn5audio15BehaviorManager11GetRevisionEv", "_ZN2nn5audio15BehaviorManager11CheckErrorsEv"}};
static int test_sdk, test_missing = -1, test_no_os;
int kernel_symbol(u32 i, u64* addr, const char** name) {
    if (i == 0) { /* a function of another library's first, and one with no name */
        *addr = 0x8000, *name = "_ZN2nn2os9LockMutexEPNS0_9MutexTypeE";
        return 1;
    }
    if (i == 1) {
        *addr = 0x8010, *name = NULL;
        return 1;
    }
    if (i - 2 >= M_COUNT) return 0;
    *addr = SDK_FN(i - 2);
    *name = (int)(i - 2) == test_missing ? "_ZN2nn5audio9SomethingEv" : test_names[test_sdk][i - 2];
    return 1;
}
u64 kernel_find_symbol_exact(const char* name) {
    if (test_no_os) return 0;
    if (!strcmp(name, "_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb")) return SDK_CREATE;
    if (!strcmp(name, "_ZN2nn2os17SignalSystemEventEPNS0_15SystemEventTypeE")) return SDK_SIGNAL;
    return 0;
}
static u64 test_attached_to;
static unsigned test_attached;
static int test_no_attach;
int hook_attach_handle(CPU* c, u64 event, u32 handle) {
    (void)c;
    if (test_no_attach || !handle) return 0;
    CHECK(handle == 7);
    test_attached_to = event;
    test_attached++;
    return 1;
}

static CPU cpu;
#define T_CONFIG       0x1000u /* the game's AudioRendererConfig: its managers at 0x100 * (place / 8 + 1) */
#define T_HANDLE       0x1800u
#define T_PARAM        0x1900u
#define T_EVENT        0x1a00u
#define T_FRAMES       0x1b00u /* where the SDK keeps the rendered frames' count */
#define T_IN           0x2000u
#define T_OUT          0x3000u
#define T_STACK        0x7000u
#define MANAGER(place) (0x100u * ((u32)(place) / 8 + 1) + 0x4000u)
/* the sections the stand-in managers write: behavior, memory pool, voice (two sizes), effect, splitter, mix, sink,
   performance */
static const u32 test_section[8] = {0x18, 0x40, 0x60, 0x08, 0x20, 0x28, 0x30, 0x10};
#define VOICE_RESOURCE 0x50u
#define UPDATE_TOTAL   (0x40u + 0x18 + 0x40 + 0x60 + VOICE_RESOURCE + 0x08 + 0x20 + 0x28 + 0x30 + 0x10)
static int test_calls[64], test_ncalls, test_wrong_section = -1, test_check_errors;
static u64 test_created, test_signaled, test_cursor;
void hook_guest_call(CPU* c, u64 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    int m;
    if (fn == SDK_CREATE) {
        CHECK(a1 == 1 && a2 == 0); /* auto clear, not shared between processes */
        test_created = a0;
        return;
    }
    if (fn == SDK_SIGNAL) {
        test_signaled = a0;
        return;
    }
    m = (int)((fn - SDK_FN(0)) / 0x10);
    CHECK(fn >= SDK_FN(0) && m < M_COUNT && test_ncalls < 64);
    test_calls[test_ncalls++] = m;
    CHECK(a0 == MANAGER(g_methods[m].manager)); /* the manager the config holds for the method */
    CHECK(c->x[31] == T_STACK - 0x20);          /* the handler's two words are above the callee's frames */
    if (m <= M_PERFORMANCE_IN) {
        CHECK(a1 == test_cursor); /* each section where the one before ended */
        memset(GP(a1), 0xA0 + m, test_section[m]);
        test_cursor += test_section[m];
        if (m == M_VOICE_IN) {
            CHECK(a2 == T_STACK - 0x20 && a3 == T_STACK - 0x18);
            memset(GP(a1 + test_section[m]), 0xB0, VOICE_RESOURCE);
            snx_st_le64(GP(a2), test_section[m]);
            snx_st_le64(GP(a3), VOICE_RESOURCE);
            test_cursor += VOICE_RESOURCE;
            c->x[0] = 0xdead; /* its sizes come back through its pointers */
        } else
            c->x[0] = test_section[m];
    } else if (m <= M_BEHAVIOR_OUT) {
        if (m == M_POOL_OUT) test_cursor = T_OUT + 0x40; /* the answer's sections start after its header */
        CHECK(a1 == test_cursor);
        c->x[0] = test_answer[m - M_POOL_OUT] + (m == test_wrong_section ? 8 : 0);
        test_cursor += c->x[0];
    } else if (m == M_REVISION)
        c->x[0] = 0x38564552; /* "REV8" */
    else
        c->x[0] = (u64)test_check_errors;
}

static void config(const HookLayout_nn_audio_AudioRendererConfig* L, u64 in_size, u64 out_size) {
    memset(GP(T_CONFIG), 0, 0x100);
    for (u32 place = 0; place < 0x40; place += 8) snx_st_le64(GP(T_CONFIG + place), MANAGER(place));
    snx_st_le64(GP(T_CONFIG + 0x40), L->in.offset == 0x40 ? 0 : T_FRAMES);
    hook_put_addr(&cpu, L->in, T_CONFIG, T_IN);
    hook_put_u64(&cpu, L->in_size, T_CONFIG, in_size);
    hook_put_addr(&cpu, L->out, T_CONFIG, T_OUT);
    hook_put_u64(&cpu, L->out_size, T_CONFIG, out_size);
    memset(GP(T_IN), 0xEE, 0x400);
    memset(GP(T_OUT), 0xEE, 0x400);
    snx_st_le64(GP(T_FRAMES), 0);
    test_ncalls = 0;
    test_cursor = T_IN + 0x40;
    cpu.x[31] = T_STACK;
}
/* a public AudioRendererParameter: 48 kHz, 240 samples, 6 mix buffers, 2 submixes, 24 voices, 1 sink, 4 effects, 3
   performance frames, voice drop, 2 splitters of 8 channels, the rendering device and execution mode, and the
   revision where the layout has it */
static void parameter(const HookLayout_nn_audio_AudioRendererParameter* L, u32 device, u32 mode, u32 revision) {
    static const u32 counts[8] = {48000, 240, 6, 2, 24, 1, 4, 3};
    memset(GP(T_PARAM), 0, 0x40);
    for (u32 i = 0; i < 8; i++) snx_st_le32(GP(T_PARAM + 4 * i), counts[i]);
    GP(T_PARAM)[0x20] = 1;
    snx_st_le32(GP(T_PARAM + 0x24), 2);
    snx_st_le32(GP(T_PARAM + 0x28), 8);
    snx_st_le32(GP(T_PARAM + 0x2c), device);
    snx_st_le32(GP(T_PARAM + 0x30), mode);
    if (L->revision.offset > 0x34) snx_st_le32(GP(T_PARAM + 0x34), 0x1234); /* the external context's size */
    hook_put_u32(&cpu, L->revision, T_PARAM, revision);
}

int main(void) {
    static const HookLayout_nn_audio_AudioRendererParameter P34 = {0x38, {0x34}}, P38 = {0x3c, {0x38}};
    static const HookLayout_nn_audio_AudioRendererConfig C3 = {0x70, {0x50}, {0x58}, {0x60}, {0x68}},
                                                         C4 = {0x60, {0x40}, {0x48}, {0x50}, {0x58}},
                                                         C10 = {0x68, {0x48}, {0x50}, {0x58}, {0x60}};
    CPU* c = &cpu;
    hle_init();
    g_mem = calloc(1, 0x10000);
    CHECK(g_mem != NULL);
    cpu.mem = g_mem;
    g_hle.audio.ev = 0x77;

    /* OpenAudioRenderer: the parameters as the session took them, for each layout; the handle is the renderer's name
       and nothing */
    parameter(&P34, 0, 0, 0x38564552);
    memset(GP(T_HANDLE), 0xCC, 0x18);
    CHECK(nnaudio_open_renderer(c, &P34, T_HANDLE, T_PARAM, 0x5000, 0x10000) == 0);
    u64 id = snx_ld_le64(GP(T_HANDLE));
    CHECK(id == 1 && snx_ld_le64(GP(T_HANDLE + 8)) == 0 && GP(T_HANDLE)[0x10] == 0xCC && test_states == 1);
    CHECK(!memcmp(test_params, GP(T_PARAM), 0x20) && test_params[0x20] == 1 && test_params[0x22] == 0);
    CHECK(snx_ld_le32(test_params + 0x24) == 2 && snx_ld_le32(test_params + 0x28) == 8);
    CHECK(snx_ld_le32(test_params + 0x2c) == 0 && snx_ld_le32(test_params + 0x30) == 0x38564552);
    /* what the handle's functions answer: the parameters, stopped until Start, the limit and the voice drop as set */
    CHECK(nnaudio_get_renderer_sample_rate(c, id, 0) == 48000 && nnaudio_get_renderer_sample_count(c, id, 0) == 240);
    CHECK(nnaudio_get_renderer_mix_buffer_count(c, id, 0) == 6 && nnaudio_get_renderer_state(c, id, 0) == 1);
    CHECK(nnaudio_start_renderer(c, id, 0) == 0 && nnaudio_get_renderer_state(c, id, 0) == 0);
    nnaudio_stop_renderer(c, id, 0);
    CHECK(nnaudio_get_renderer_state(c, id, 0) == 1 && nnaudio_get_renderer_time_limit(c, id, 0) == 100);
    nnaudio_set_renderer_time_limit(c, id, 0, 75);
    CHECK(nnaudio_get_renderer_time_limit(c, id, 0) == 75 && nnaudio_get_renderer_voice_drop(c, id, 0) == 1.0f);
    nnaudio_set_renderer_voice_drop(c, id, 0, 0.5f);
    CHECK(nnaudio_get_renderer_voice_drop(c, id, 0) == 0.5f && nnaudio_get_renderer_execution_mode(c, id, 0) == 0);
    CHECK(nnaudio_get_renderer_rendering_device(c, id, 0) == 0);
    /* QueryAudioRendererSystemEvent: the clock's event attached to the game's SystemEvent */
    CHECK(nnaudio_query_renderer_event(c, T_HANDLE, T_EVENT) == 0 && test_attached == 1 && test_attached_to == T_EVENT);
    /* a handle that names no renderer: each call named in the log, and the answers of a stopped one */
    test_last_log[0] = 0;
    CHECK(nnaudio_start_renderer(c, 9, 0) == RC(153, 1536) && strstr(test_last_log, "StartAudioRenderer") &&
          strstr(test_last_log, "0x9"));
    CHECK(nnaudio_get_renderer_state(c, 0, 0) == 1 && nnaudio_get_renderer_sample_rate(c, 0x1234567, 0) == 0);
    nnaudio_close_renderer(c, id, 0);
    CHECK(test_states == 0 && audren_at(1) == NULL);

    /* the layout of SDK 6.5 and 8.2: the revision one word later, the word before it the external context's size.
       With the game's SystemEvent: the renderer's event attached */
    parameter(&P38, 0, 0, 0x37564552);
    CHECK(nnaudio_open_renderer_event(c, &P38, T_HANDLE, T_EVENT + 0x40, T_PARAM, 0x5000, 0x10000) == 0);
    CHECK(snx_ld_le32(test_params + 0x2c) == 0x1234 && snx_ld_le32(test_params + 0x30) == 0x37564552);
    CHECK(test_attached == 2 && test_attached_to == T_EVENT + 0x40 && !test_created);
    nnaudio_close_renderer(c, snx_ld_le64(GP(T_HANDLE)), 0);
    /* a renderer the game executes itself, on the device that says so: the game's SystemEvent is made by the SDK, and
       signaled after each ExecuteAudioRendererRendering; no event of the clock's for it */
    parameter(&P34, 1, 1, 0x38564552);
    CHECK(nnaudio_open_renderer_event(c, &P34, T_HANDLE, T_EVENT, T_PARAM, 0x5000, 0x10000) == 0);
    id = snx_ld_le64(GP(T_HANDLE));
    CHECK(test_params[0x22] == 1 && test_params[0x23] == 1 && test_created == T_EVENT && test_attached == 2);
    CHECK(nnaudio_get_renderer_execution_mode(c, id, 0) == 1 && nnaudio_get_renderer_rendering_device(c, id, 0) == 1);
    CHECK(nnaudio_query_renderer_event(c, T_HANDLE, T_EVENT) == RC(153, 513));
    CHECK(nnaudio_execute_renderer(c, id, 0) == 0 && test_signaled == T_EVENT);
    nnaudio_close_renderer(c, id, 0);
    CHECK(nnaudio_execute_renderer(c, id, 0) == RC(153, 1536));
    /* refused: a revision past the newest understood, and parameters the DSP makes no renderer with; the handle is
       left as it was and the log names the numbers */
    parameter(&P34, 0, 0, 0x41564552); /* REVA: 17 */
    memset(GP(T_HANDLE), 0xCC, 0x10);
    test_last_log[0] = 0;
    CHECK(nnaudio_open_renderer(c, &P34, T_HANDLE, T_PARAM, 0x5000, 0x10000) == RC(153, 1537));
    CHECK(GP(T_HANDLE)[0] == 0xCC && strstr(test_last_log, "17") && strstr(test_last_log, "16"));
    parameter(&P34, 0, 0, 0x38564552);
    test_refuse = 1;
    test_last_log[0] = 0;
    CHECK(nnaudio_open_renderer(c, &P34, T_HANDLE, T_PARAM, 0x5000, 0x10000) == RC(153, 2) && GP(T_HANDLE)[0] == 0xCC);
    CHECK(strstr(test_last_log, "24 voices") && strstr(test_last_log, "48000 Hz") && test_states == 0);
    test_refuse = 0;
    /* GetAudioRendererWorkBufferSize: a whole number of pages that grows with the voices */
    u64 small = nnaudio_get_renderer_work_buffer_size(c, &P34, T_PARAM);
    snx_st_le32(GP(T_PARAM + 16), 96);
    CHECK(small && !(small & 0xfff) && nnaudio_get_renderer_work_buffer_size(c, &P34, T_PARAM) > small);

    /* RequestUpdateAudioRenderer, for the config of each SDK read and the managers' names of an old and a new one */
    static const struct {
        const HookLayout_nn_audio_AudioRendererConfig* layout;
        int frames, sdk;
    } cases[4] = {{&C3, 0, 0}, {&C4, 0, 0}, {&C3, 1, 0}, {&C10, 1, 1}};
    static const int order[] = {M_REVISION,       M_BEHAVIOR_IN,     M_POOL_IN,      M_VOICE_IN,
                                M_EFFECT_IN,      M_SPLITTER_IN,     M_MIX_IN,       M_SINK_IN,
                                M_PERFORMANCE_IN, M_POOL_OUT,        M_VOICE_OUT,    M_EFFECT_OUT,
                                M_SINK_OUT,       M_PERFORMANCE_OUT, M_BEHAVIOR_OUT, M_CHECK_ERRORS};
    parameter(&P34, 0, 0, 0x38564552);
    CHECK(nnaudio_open_renderer(c, &P34, T_HANDLE, T_PARAM, 0x5000, 0x10000) == 0);
    id = snx_ld_le64(GP(T_HANDLE));
    for (int i = 0; i < 4; i++) {
        const HookLayout_nn_audio_AudioRendererConfig* L = cases[i].layout;
        u32 (*update)(CPU*, const HookLayout_nn_audio_AudioRendererConfig*, u64, u64, u64) =
            cases[i].frames ? nnaudio_request_update_frames : nnaudio_request_update;
        unsigned before = test_updates;
        test_sdk = cases[i].sdk;
        g_hle.audio.ren_fn_found = 0; /* another game's SDK: its managers looked for again */
        memset(g_hle.audio.ren_fn, 0, sizeof g_hle.audio.ren_fn);
        config(L, 0x400, ANSWER_TOTAL);
        CHECK(update(c, L, id, 0, T_CONFIG) == 0 && test_updates == before + 1 && cpu.x[31] == T_STACK);
        CHECK(test_ncalls == 16 && !memcmp(test_calls, order, sizeof order));
        /* the update: the revision, each section's size in its word, the total, and the sections one after another */
        CHECK(test_in_size == UPDATE_TOTAL && test_out_size == ANSWER_TOTAL);
        CHECK(snx_ld_le32(test_in) == 0x38564552 && snx_ld_le32(test_in + 0x04) == 0x18);
        CHECK(snx_ld_le32(test_in + 0x08) == 0x40 && snx_ld_le32(test_in + 0x0c) == 0x60);
        CHECK(snx_ld_le32(test_in + 0x10) == VOICE_RESOURCE && snx_ld_le32(test_in + 0x14) == 0x08);
        CHECK(snx_ld_le32(test_in + 0x18) == 0x28 && snx_ld_le32(test_in + 0x1c) == 0x30);
        CHECK(snx_ld_le32(test_in + 0x20) == 0x10 && snx_ld_le32(test_in + 0x24) == 0x20);
        CHECK(snx_ld_le32(test_in + 0x28) == 0 && snx_ld_le32(test_in + 0x3c) == UPDATE_TOTAL);
        CHECK(test_in[0x40] == 0xA0 && test_in[0x40 + 0x18] == 0xA1 && test_in[0x40 + 0x18 + 0x40] == 0xA2);
        CHECK(test_in[0x40 + 0x18 + 0x40 + 0x60] == 0xB0 && test_in[UPDATE_TOTAL - 1] == 0xA7);
        CHECK(GP(T_IN)[UPDATE_TOTAL] == 0xEE);
        /* the frames rendered, where the config says, for an SDK that keeps them */
        CHECK(snx_ld_le64(GP(T_FRAMES)) == (cases[i].frames ? 0x123456789ull : 0));
    }
    /* what BehaviorManager::CheckErrors gives is the result */
    config(&C10, 0x400, ANSWER_TOTAL);
    test_check_errors = 0x1234;
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == 0x1234);
    test_check_errors = 0;
    /* the renderer's InvalidUpdateInfo: the SDK reads the errors and gives the result; InsufficientBuffer and any
       other failure are given as they are */
    config(&C10, 0x400, ANSWER_TOTAL);
    test_update_result = -41;
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == RC(153, 41));
    CHECK(test_ncalls == 10 && test_calls[9] == M_CHECK_ERRORS && cpu.x[31] == T_STACK);
    config(&C10, 0x400, ANSWER_TOTAL);
    test_update_result = -4;
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == RC(153, 4) && test_ncalls == 9);
    test_update_result = 0;
    /* an update longer than the config's buffer: InvalidUpdateInfo before the renderer sees it, both sizes named */
    unsigned before = test_updates;
    config(&C10, UPDATE_TOTAL - 1, ANSWER_TOTAL);
    test_last_log[0] = 0;
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == RC(153, 41) && test_updates == before);
    {
        char total[16], room[16];
        snprintf(total, sizeof total, "%u bytes", UPDATE_TOTAL);
        snprintf(room, sizeof room, "of %u", UPDATE_TOTAL - 1);
        CHECK(strstr(test_last_log, total) && strstr(test_last_log, room) && cpu.x[31] == T_STACK);
    }
    /* a config with no room for a header, and an answer whose buffer is not its size or whose section is not the
       size its header says */
    config(&C10, 0x3f, ANSWER_TOTAL);
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == RC(153, 41) && test_ncalls == 0);
    config(&C10, 0x400, ANSWER_TOTAL + 8);
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == RC(153, 41));
    config(&C10, 0x400, ANSWER_TOTAL);
    test_wrong_section = M_EFFECT_OUT;
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == RC(153, 41) &&
          test_calls[test_ncalls - 1] == M_EFFECT_OUT);
    test_wrong_section = -1;
    /* a handle that names no renderer */
    config(&C10, 0x400, ANSWER_TOTAL);
    CHECK(nnaudio_request_update_frames(c, &C10, 7, 0, T_CONFIG) == RC(153, 1536) && test_ncalls == 0);
    /* an SDK that exports one manager's method under no name the handler knows: OperationFailed, said once, with
       the method's name and the result */
    g_hle.audio.ren_fn_found = 0;
    memset(g_hle.audio.ren_fn, 0, sizeof g_hle.audio.ren_fn);
    test_missing = M_MIX_IN;
    config(&C10, 0x400, ANSWER_TOTAL);
    test_last_log[0] = 0;
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == RC(153, 2) && test_ncalls == 0);
    CHECK(strstr(test_last_log, "10MixManager20UpdateMixInParameterE") && strstr(test_last_log, "0x499"));
    test_last_log[0] = 0;
    CHECK(nnaudio_request_update_frames(c, &C10, id, 0, T_CONFIG) == RC(153, 2) && !test_last_log[0]);
    test_missing = -1;
    g_hle.audio.ren_fn_found = 0;
    memset(g_hle.audio.ren_fn, 0, sizeof g_hle.audio.ren_fn);
    nnaudio_close_renderer(c, id, 0);
    CHECK(test_states == 0);
    /* an SDK with no AttachReadableHandleToSystemEvent, or no CreateSystemEvent: the renderer opens with no event,
       and the log says which function is missing */
    parameter(&P34, 0, 0, 0x38564552);
    test_no_attach = 1;
    test_last_log[0] = 0;
    CHECK(nnaudio_open_renderer_event(c, &P34, T_HANDLE, T_EVENT, T_PARAM, 0x5000, 0x10000) == 0);
    CHECK(strstr(test_last_log, "AttachReadableHandleToSystemEvent"));
    CHECK(nnaudio_query_renderer_event(c, T_HANDLE, T_EVENT) == RC(153, 2));
    test_no_attach = 0;
    nnaudio_close_renderer(c, snx_ld_le64(GP(T_HANDLE)), 0);
    parameter(&P34, 1, 1, 0x38564552);
    test_no_os = 1;
    test_created = 0;
    test_last_log[0] = 0;
    CHECK(nnaudio_open_renderer_event(c, &P34, T_HANDLE, T_EVENT, T_PARAM, 0x5000, 0x10000) == 0 && !test_created);
    CHECK(strstr(test_last_log, "CreateSystemEvent"));
    test_no_os = 0;
    nnaudio_close_renderer(c, snx_ld_le64(GP(T_HANDLE)), 0);
    CHECK(test_states == 0);

    /* audio_renderer.hook: the layouts and handlers for each SDK read, and none for the versions between them */
    static const struct {
        u32 sdk, revision, in;
        const char* update;
    } read[] = {{0x030500, 0x34, 0x50, "nnaudio_request_update"},
                {0x040400, 0x34, 0x40, "nnaudio_request_update"},
                {0x060500, 0x38, 0x50, "nnaudio_request_update_frames"},
                {0x080201, 0x38, 0x50, "nnaudio_request_update_frames"},
                {0x0a0400, 0x34, 0x48, "nnaudio_request_update_frames"},
                {0x0d0302, 0x34, 0x48, "nnaudio_request_update_frames"},
                {0x100200, 0x34, 0x48, "nnaudio_request_update_frames"},
                {0x120302, 0x34, 0x48, "nnaudio_request_update_frames"},
                {0x130303, 0x34, 0x48, "nnaudio_request_update_frames"},
                {0x140506, 0x34, 0x48, "nnaudio_request_update_frames"}};
    static const char open_symbol[] =
        "_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm";
    static const char update_symbol[] =
        "_ZN2nn5audio26RequestUpdateAudioRendererENS0_19AudioRendererHandleEPKNS0_19AudioRendererConfigE";
    static const u32 unread[] = {0x020300, 0x050100, 0x090200};
    int first, k;
    for (unsigned i = 0; i < sizeof read / sizeof read[0]; i++) {
        u32 versions[HOOK_KEYS] = {0};
        versions[HOOK_SDK] = read[i].sdk;
        /* through the adapters: the open reads the revision where that SDK has it, the update the buffers */
        k = hook_select(&hook_table, open_symbol, versions, &first);
        CHECK(k >= 0 && !strcmp(hook_handler[k], "nnaudio_open_renderer"));
        memset(GP(T_PARAM), 0, 0x40);
        snx_st_le32(GP(T_PARAM), 32000);
        snx_st_le32(GP(T_PARAM + read[i].revision), 0x36564552);
        c->x[0] = T_HANDLE, c->x[1] = T_PARAM, c->x[2] = 0x5000, c->x[3] = 0x10000, c->x[30] = 0x1234, c->pc = 0;
        hook_fn[k](c, 0);
        CHECK((u32)c->x[0] == 0 && c->pc == 0x1234 && snx_ld_le32(test_params + 0x30) == 0x36564552);
        id = snx_ld_le64(GP(T_HANDLE));
        k = hook_select(&hook_table, update_symbol, versions, &first);
        CHECK(k >= 0 && !strcmp(hook_handler[k], read[i].update));
        HookLayout_nn_audio_AudioRendererConfig L = {
            0, {read[i].in}, {read[i].in + 8}, {read[i].in + 16}, {read[i].in + 24}};
        config(&L, 0x400, ANSWER_TOTAL);
        c->x[0] = id, c->x[1] = 0, c->x[2] = T_CONFIG, c->x[30] = 0x1234, c->pc = 0;
        hook_fn[k](c, 0);
        CHECK((u32)c->x[0] == 0 && c->pc == 0x1234 && test_in_size == UPDATE_TOTAL && test_ncalls == 16);
        nnaudio_close_renderer(c, id, 0);
    }
    for (unsigned i = 0; i < sizeof unread / sizeof unread[0]; i++) {
        u32 versions[HOOK_KEYS] = {0};
        versions[HOOK_SDK] = unread[i];
        CHECK(hook_select(&hook_table, open_symbol, versions, &first) == -2);
        CHECK(hook_select(&hook_table, update_symbol, versions, &first) == -2);
    }
    { /* the handle's other functions hold for any SDK: the handle's two words, then the argument */
        u32 versions[HOOK_KEYS] = {0};
        parameter(&P34, 0, 0, 0x38564552);
        CHECK(nnaudio_open_renderer(c, &P34, T_HANDLE, T_PARAM, 0x5000, 0x10000) == 0);
        id = snx_ld_le64(GP(T_HANDLE));
        k = hook_select(&hook_table, "_ZN2nn5audio34SetAudioRendererRenderingTimeLimitENS0_19AudioRendererHandleEi",
                        versions, &first);
        CHECK(k >= 0 && hook_key[k] == HOOK_ANY);
        c->x[0] = id, c->x[1] = 0xabc, c->x[2] = 60, c->x[30] = 0x1234, c->pc = 0;
        hook_fn[k](c, 0);
        CHECK(c->pc == 0x1234 && audren_at((u32)id)->limit == 60);
        k = hook_select(&hook_table, "_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", versions, &first);
        CHECK(k >= 0 && hook_key[k] == HOOK_ANY);
        c->x[0] = id, c->x[1] = 0xabc, c->x[30] = 0x1234, c->pc = 0;
        hook_fn[k](c, 0);
        CHECK(audren_at((u32)id) == NULL && test_states == 0);
    }
    printf("nn_audio_renderer: %u checks\n", test_checks);
    free(g_mem);
    return 0;
}
