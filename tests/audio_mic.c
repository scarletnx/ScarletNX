/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The input streams of the Ounce's SDK (src/nx/nn/audio_stream.c) over a synthetic microphone: their handlers called as
   the hooks call them, on a guest buffer, with a clock the test moves and a host recording device (support/mic_fake.c,
   in common/platform/mic.h's place) that plays a generated signal in bursts, never a real one. Checked: what an open
   leaves for the format query, the device properties, the frames' count in guest time and their format on every
   channel, the device opened by a start and closed by a stop, the microphone setting off, the hold before the first
   frames, a device that runs short or ahead, the gain. Then the noise reduction (src/common/utilities/denoise.c). Off,
   the game gets the signal sample for sample. On, speech-like syllables over a steady noise and over babble go through
   the neural method (RNNoise, built in) and the spectral one (the system's libspeexdsp, skipped and said without it),
   each measured: the noise lowered between the syllables, the syllables' signal-to-noise ratio, the speech kept, the
   delay and the cost */
#include "nx/hle/services/services_int.h"
#include "nx/runtime/ini.h"
#include <stdarg.h>
#include <time.h>

#include "nx/nn/audio_stream.c"
#include "common/utilities/mic.c"
#include "common/utilities/denoise.c"
#include "support/mic_fake.h"

u8* g_mem;
Settings g_settings;
static u64 test_now;
static unsigned test_checks, test_signals;
static CPU cpu;

static void test_fail(const char* expression, int line) {
    fprintf(stderr, "audio_mic.c:%d: check failed: %s\n", line, expression);
    exit(1);
}
#define CHECK(expr)                                                                                                    \
    do {                                                                                                               \
        test_checks++;                                                                                                 \
        if (!(expr)) test_fail(#expr, __LINE__);                                                                       \
    } while (0)

/* single-threaded: the timing and the samples are what is checked */
void pm_init(PMutex* m) { (void)m; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
u64 guest_ns(void) { return test_now; }
static char test_last_log[512];
void logf_(int level, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    vsnprintf(test_last_log, sizeof test_last_log, format, ap);
    va_end(ap);
    if (level <= 1) fputs(test_last_log, stdout);
}
void kobj_unref(KObj* o) {
    if (o) o->refs--;
}
void kevent_signal(KEvent* e) {
    e->signaled = 1;
    test_signals++;
}
static KEvent test_event;
KEvent* hook_attach_event(CPU* c, u64 event, const char* name) {
    (void)c, (void)name;
    CHECK(event != 0);
    test_event.o.refs = 1;
    return &test_event;
}
/* the outputs, which no check here opens */
PAudio* plat_audio_open(u32 rate, u32 channels) {
    (void)rate, (void)channels;
    return NULL;
}
void plat_audio_close(PAudio* a) { (void)a; }
int plat_audio_queue(PAudio* a, const void* samples, u32 bytes) {
    (void)a, (void)samples, (void)bytes;
    return 0;
}
void plat_audio_start(PAudio* a, int started) { (void)a, (void)started; }
void plat_audio_clear(PAudio* a) { (void)a; }

/* ------------------------------------------------------------ the synthetic microphone */
/* The signal: white noise of about -26 dBFS (the sum of four uniform numbers) under tone bursts of 440 Hz at -10 dBFS,
   300 ms on and 300 ms off, each burst faded in and out over 5 ms. Sample n of it, the same every time */
#define TONE_HZ   440.0
#define TONE_AMP  0.3
#define NOISE_AMP 0.05
#define BURST     14400u /* 300 ms at 48 kHz */
static int test_tone = 1, test_noise = 1;
static u32 noise_state(u64 n) { /* a hash of n: the noise is the same sample for sample, whenever it is asked */
    u64 x = n * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    return (u32)(x >> 16);
}
static double noise_at(u64 n) {
    u32 h = noise_state(n), g = noise_state(n + 0x10000000000ull);
    double sum = (double)(h & 0xFFFF) + (double)(h >> 16) + (double)(g & 0xFFFF) + (double)(g >> 16);
    return (sum / 65536.0 - 2.0) * 0.866 * NOISE_AMP * 2.0; /* the sum's deviation is 0.577: about NOISE_AMP */
}
static int burst_on(u64 n) { return (n / BURST) % 2 == 1; }
static double tone_at(u64 n) {
    u64 in = n % BURST;
    double fade = in < 240 ? (double)in / 240.0 : in > BURST - 240 ? (double)(BURST - in) / 240.0 : 1.0;
    return burst_on(n) ? TONE_AMP * fade * sin(2.0 * M_PI * TONE_HZ * (double)n / 48000.0) : 0.0;
}
static const i16* test_source; /* a signal made beforehand, in place of the tone and noise */
static u64 test_source_len;
static i16 signal_at(u64 n) {
    if (test_source) return n < test_source_len ? test_source[n] : 0;
    double v = (test_tone ? tone_at(n) : 0.0) + (test_noise ? noise_at(n) : 0.0);
    long s = lrint(v * 32767.0);
    return (i16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

/* the device (support/mic_fake.c) records signal_at at 48 kHz scaled by test_speed (in 1/1000ths), and hands its
   samples over every 20 ms of host time, as a device with a buffer of 960 frames does. test_stall stops it */
static int test_stall;
static u32 test_speed = 1000;
static u64 test_host_ns; /* the device's own time, which bursts of 20 ms */
static void device_run(u64 ns) {
    u64 before = test_host_ns / 20000000ull;
    test_host_ns += ns;
    if (!g_mic_fake.dev || test_stall) return;
    for (u64 b = before; b < test_host_ns / 20000000ull; b++)
        g_mic_fake.dev->recorded += (u64)g_mic_fake.dev->rate / 50 * test_speed / 1000;
}

/* ------------------------------------------------------------ the game's side */
#define STREAM  0x10000u /* the game's input stream */
#define FORMAT  0x10800u /* its format */
#define EVENT   0x10900u /* its event */
#define NAMES   0x11000u
#define PROPS   0x12000u
#define DATA    0x20000u /* the game's read buffer */
#define STREAM2 0x14000u

static void format(u64 at, u32 sample_format, u32 channels, u32 rate) {
    snx_st_le32(GP(at), sample_format);
    snx_st_le32(GP(at + 4), channels);
    snx_st_le32(GP(at + 8), rate);
    snx_st_le32(GP(at + 12), 1);
}
/* one turn of the audio clock, 5 ms of both clocks */
static void tick(void) {
    device_run(5000000ull);
    test_now += 5000000ull;
    outstream_tick(test_now);
}
/* the game's read of everything recorded, as 16-bit stereo, into out from *got on: the frames read */
static u32 read_all(u64 stream, i16* out, u32* got, u32 room) {
    u32 total = 0;
    for (;;) {
        i32 n = audio_read_input(&cpu, stream, DATA, 1024);
        CHECK(n >= 0 && n <= 1024);
        for (i32 i = 0; i < n; i++) {
            i16 l = (i16)snx_ld_le16(GP(DATA + (u64)i * 4)), r = (i16)snx_ld_le16(GP(DATA + (u64)i * 4 + 2));
            CHECK(l == r); /* the one recording on both channels */
            if (out && *got < room) out[(*got)++] = l;
        }
        total += (u32)n;
        if (n < 1024) return total;
    }
}

/* the format, the device and the properties: what the game reads after its open */
static void test_open(void) {
    format(FORMAT, 2, 2, 48000);
    memset(GP(STREAM), 0xCC, 0x400);
    CHECK(audio_list_virtual_inputs(&cpu, NAMES, 8) == 1);
    CHECK(audio_virtual_input_properties(&cpu, PROPS, NAMES) == 0);
    CHECK(!strcmp((const char*)GP(PROPS), (const char*)GP(NAMES)));
    CHECK(snx_ld_le64(GP(PROPS + 0x100)) == 10000000ull && snx_ld_le32(GP(PROPS + 0x108)) == 2);
    CHECK(audio_default_virtual_input(&cpu, NAMES + 0x100) == 0 &&
          !strcmp((const char*)GP(NAMES + 0x100), (const char*)GP(NAMES)));
    /* the game's: 50 ms (its period, over twice the device's latency), half of it the event's threshold */
    CHECK(audio_open_input_event(&cpu, STREAM, EVENT, 0, 0, NAMES, FORMAT, 2400, 1200) == 0);
    CHECK(audio_input_format(&cpu, STREAM) == STREAM + 8 + 0x114 && !memcmp(GP(STREAM + 8 + 0x114), GP(FORMAT), 16));
    /* the SDK's own format query, which a module recompiled before the hook calls: its stream object's address at
       +0, the format at +0x114 in it */
    CHECK(snx_ld_le64(GP(STREAM)) + 0x114 == audio_input_format(&cpu, STREAM));
    CHECK(GP(STREAM + 8)[0] == 0xCC && GP(STREAM + 8 + 0x114 + 16)[0] == 0xCC); /* nothing else written */
    CHECK(audio_input_state(&cpu, STREAM) == STREAM_STOPPED && audio_input_capacity_frames(&cpu, STREAM) == 2400);
    CHECK(audio_input_gain(&cpu, STREAM) == 1.0f);
    CHECK(g_mic_fake.opens == 0); /* no device before the start */
}

/* frames in guest time, with the device's hold before them: s16 stereo, the signal sample for sample with the
   reduction off. Returns the frames of silence before the signal */
static u32 test_pace(void) {
    static i16 got[48000 * 2];
    u32 n = 0, total = 0, lead = 0;
    g_settings.microphone[0] = 0; /* the system's default device, whatever the settings' table starts it at */
    g_settings.microphone_noise_reduction = 0;
    test_signals = 0;
    CHECK(audio_start_input(&cpu, STREAM) == 0 && g_mic_fake.opens == 1 && g_mic_fake.dev != NULL &&
          g_mic_fake.device[0] == 0);
    CHECK(audio_input_state(&cpu, STREAM) == STREAM_STARTED);
    for (int t = 0; t < 400; t++) { /* 2 s */
        tick();
        CHECK(audio_input_filled_frames(&cpu, STREAM) == 240); /* 5 ms at 48 kHz, every turn */
        total += read_all(STREAM, got, &n, sizeof got / sizeof *got);
    }
    CHECK(total == 96000 && test_signals == 400);
    while (lead < n && got[lead] == 0) lead++;
    CHECK(lead >= 1440 && lead <= 2880); /* the hold of 40 ms, less the first turn's frames */
    for (u32 i = lead; i < n; i++)
        if (got[i] != signal_at(i - lead)) {
            fprintf(stderr, "sample %u: %d, not %d\n", i, got[i], signal_at(i - lead));
            CHECK(!"the signal as recorded");
        }
    /* nothing held back past a block: the reducer's block is 10 ms */
    CHECK(g_mic_fake.dev->read == g_mic_fake.dev->recorded);
    return lead;
}

/* a stall of the device: silence, said once, and the signal back after the hold. Then a device 2% fast: the delay
   doesn't grow past hold + slack */
static void test_short(void) {
    static i16 got[48000];
    u32 n = 0;
    test_stall = 1;
    for (int t = 0; t < 40; t++) {
        tick();
        read_all(STREAM, got, &n, sizeof got / sizeof *got);
    }
    test_stall = 0;
    CHECK(n == 9600 && got[n - 1] == 0 && got[n - 2] == 0);
    CHECK(strstr(test_last_log, "ran short") != NULL);
    n = 0;
    for (int t = 0; t < 40; t++) {
        tick();
        read_all(STREAM, got, &n, sizeof got / sizeof *got);
    }
    CHECK(n == 9600 && got[n - 1] != 0); /* back */
    test_speed = 1020;
    for (int t = 0; t < 2000; t++) { /* 10 s */
        tick();
        read_all(STREAM, NULL, &n, 0);
        SnxMic* m = input_of(STREAM)->mic;
        CHECK(m->count <= m->hold + m->slack);
    }
    CHECK(input_of(STREAM)->mic->dropped > 0);
    test_speed = 1000;
}

/* the gain on a read, and the stop: the device closed, the frames recorded still there, no more of them */
static void test_gain_stop(void) {
    i16 x;
    u32 n = 0;
    audio_set_input_gain(&cpu, STREAM, 0.5f, 0);
    CHECK(audio_input_gain(&cpu, STREAM) == 0.5f);
    read_all(STREAM, NULL, &n, 0);
    tick();
    Stream* s = input_of(STREAM);
    x = s->ring[s->head];
    CHECK(audio_read_input(&cpu, STREAM, DATA, 1) == 1);
    CHECK((i16)snx_ld_le16(GP(DATA)) == (i16)lrintf((float)x * 0.5f));
    audio_set_input_gain(&cpu, STREAM, 1.0f, 0);
    tick();
    CHECK(audio_stop_input(&cpu, STREAM) == 0 && g_mic_fake.closes == 1 && g_mic_fake.dev == NULL);
    i32 left = audio_input_filled_frames(&cpu, STREAM);
    CHECK(left > 0);
    tick();
    CHECK(audio_input_filled_frames(&cpu, STREAM) == left);
    CHECK(audio_purge_input(&cpu, STREAM) == 0 && audio_input_filled_frames(&cpu, STREAM) == 0);
}

/* the setting at its default, off: no device opened until the player picks one, silence at the same pace. A host
   without a device: the same */
static void test_off(void) {
    u32 n = 0;
    static i16 got[4800];
    const IniOpt* setting = ini_find(g_game_ini, "microphone");
    CHECK(setting && setting->def && !strcmp(setting->def, "off"));
    snprintf(g_settings.microphone, sizeof g_settings.microphone, "%s", setting->def);
    unsigned opens = g_mic_fake.opens;
    CHECK(audio_start_input(&cpu, STREAM) == 0 && g_mic_fake.opens == opens && g_mic_fake.dev == NULL);
    for (int t = 0; t < 10; t++) tick();
    CHECK(read_all(STREAM, got, &n, 4800) == 2400);
    for (u32 i = 0; i < n; i++) CHECK(got[i] == 0);
    CHECK(audio_stop_input(&cpu, STREAM) == 0);
    g_settings.microphone[0] = 0;
    g_mic_fake.none = 1;
    CHECK(audio_start_input(&cpu, STREAM) == 0 && g_mic_fake.opens == opens + 1 && g_mic_fake.dev == NULL);
    for (int t = 0; t < 10; t++) tick();
    n = 0;
    CHECK(read_all(STREAM, got, &n, 4800) == 2400 && got[0] == 0 && got[n - 1] == 0);
    CHECK(audio_stop_input(&cpu, STREAM) == 0);
    g_mic_fake.none = 0;
}

/* another format: 32-bit float, mono, 16 kHz, and a device named by the setting */
static void test_float(void) {
    format(FORMAT, 5, 1, 16000);
    snprintf(g_settings.microphone, sizeof g_settings.microphone, "USB");
    CHECK(audio_open_input(&cpu, STREAM2, 0, 0, NAMES, FORMAT, 1600) == 0);
    CHECK(audio_start_input(&cpu, STREAM2) == 0 && !strcmp(g_mic_fake.device, "USB") && g_mic_fake.dev->rate == 16000);
    for (int t = 0; t < 40; t++) {
        tick();
        CHECK(audio_input_free_frames(&cpu, STREAM2) == 1600 - audio_input_filled_frames(&cpu, STREAM2));
    }
    CHECK(audio_input_filled_frames(&cpu, STREAM2) == 1600); /* 200 ms, past its capacity of 100 ms: full */
    CHECK(audio_read_input(&cpu, STREAM2, DATA, 1600) == 1600);
    u32 lead = 0;
    while (lead < 1600 && snx_ld_le32(GP(DATA + (u64)lead * 4)) == 0) lead++;
    /* the hold of 40 ms at 16 kHz, less up to a burst (the device's bursts fall where test_pace's clock left them) */
    CHECK(lead >= 240 && lead < 1600);
    u32 bits = snx_ld_le32(GP(DATA + (u64)lead * 4));
    float f;
    memcpy(&f, &bits, 4);
    CHECK(f == (float)signal_at(0) / 32768.0f);
    audio_close_input(&cpu, STREAM2);
    CHECK(g_mic_fake.dev == NULL && snx_ld_le64(GP(STREAM2)) == 0 && audio_input_state(&cpu, STREAM2) == 0);
    g_settings.microphone[0] = 0;
    format(FORMAT, 2, 2, 48000);
}

/* ticks of the clock until the log says text, 8 at most: 1 when it did */
static int said_within(const char* text) {
    for (int t = 0; t < 8; t++) {
        tick();
        if (strstr(test_last_log, text)) return 1;
    }
    return 0;
}
/* what the log says of the noise reduction: the method mic_noise_reduction asks for beside the one that runs, when
   they differ (neural on a 16 kHz stream, spectral without libspeexdsp), and each change of the setting while the
   stream records. Which one runs in place of neural depends on whether the host has libspeexdsp */
static void test_reduction_said(void) {
    format(FORMAT, 2, 1, 16000);
    g_settings.microphone_noise_reduction = SNX_DENOISE_NEURAL;
    CHECK(audio_open_input(&cpu, STREAM2, 0, 0, NAMES, FORMAT, 1600) == 0);
    CHECK(audio_start_input(&cpu, STREAM2) == 0 && input_of(STREAM2)->mic != NULL);
    int spectral = snx_denoise_method(input_of(STREAM2)->mic->denoise) == SNX_DENOISE_SPECTRAL;
    CHECK(strstr(test_last_log, spectral ? "16000 Hz, noise reduction spectral, not neural as mic_noise_reduction=1 "
                                           "asks: neural takes 48000 Hz and the stream records at 16000 Hz\n"
                                         : "16000 Hz, noise reduction off, not neural as mic_noise_reduction=1 asks: "
                                           "neural takes 48000 Hz and the stream records at 16000 Hz, and spectral "
                                           "needs " SNX_DENOISE_LIBRARY "\n") != NULL);
    g_settings.microphone_noise_reduction = SNX_DENOISE_OFF;
    CHECK(said_within("audio: the microphone's noise reduction is now off\n"));
    g_settings.microphone_noise_reduction = SNX_DENOISE_SPECTRAL;
    CHECK(said_within(spectral ? "audio: the microphone's noise reduction is now spectral\n"
                               : "audio: the microphone's noise reduction is now off, not spectral as "
                                 "mic_noise_reduction=2 asks: spectral needs " SNX_DENOISE_LIBRARY "\n"));
    audio_close_input(&cpu, STREAM2);
    g_settings.microphone_noise_reduction = SNX_DENOISE_OFF;
    format(FORMAT, 2, 2, 48000);
}

/* ------------------------------------------------------------ the reduction, on speech-like signals */
/* Speech-like: syllables of 220 ms, one every 350 ms, each a harmonic sound on a pitch that glides (a talker's base
   between 110 and 230 Hz), its harmonics up to 4 kHz shaped by three formants that move from syllable to syllable, a
   tilt of 3 dB an octave and a raised-cosine envelope. RNNoise is trained on speech: a steady tone is noise to it */
#define SYLLABLE 16800u /* 350 ms */
#define VOICED   10560u /* 220 ms */
#define TOTAL    (48000u * 8)
static u32 hash32(u32 a, u32 b) {
    u64 x = (u64)a * 0x9E3779B97F4A7C15ull ^ ((u64)b + 0x632BE59BD9B4E019ull);
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 29;
    return (u32)(x >> 16);
}
static void make_speech(float* out, u32 n, u32 seed, u32 offset) {
    double phase[40] = {0};
    for (u32 i = 0; i < n; i++) {
        u32 t = i + offset, syl = t / SYLLABLE, in = t % SYLLABLE;
        out[i] = 0;
        if (in >= VOICED) continue;
        u32 h = hash32(seed, syl);
        double f0 = (110.0 + (double)(h % 120)) * (1.0 + 0.15 * sin(M_PI * in / VOICED));
        double formant[3] = {300.0 + (double)(hash32(h, 1) % 600), 900.0 + (double)(hash32(h, 2) % 1400),
                             2300.0 + (double)(hash32(h, 3) % 700)};
        const double width[3] = {120.0, 180.0, 250.0};
        double env = 0.5 - 0.5 * cos(2.0 * M_PI * in / VOICED), v = 0;
        for (int k = 1; k < 40 && k * f0 < 4000.0; k++) {
            double f = k * f0, gain = 0.02;
            for (int j = 0; j < 3; j++) gain += exp(-((f - formant[j]) / width[j]) * ((f - formant[j]) / width[j]));
            phase[k] += 2.0 * M_PI * f / 48000.0;
            v += gain / sqrt((double)k) * sin(phase[k]);
        }
        out[i] = (float)(env * v);
    }
}
/* x scaled so that its power over the voiced cores is rms^2, or over everything */
static void scale_to(float* x, u32 n, double rms, int voiced_only) {
    double sum = 0;
    u32 count = 0;
    for (u32 i = 0; i < n; i++)
        if (!voiced_only || i % SYLLABLE < VOICED) sum += (double)x[i] * x[i], count++;
    double k = rms / sqrt(sum / count + 1e-30);
    for (u32 i = 0; i < n; i++) x[i] = (float)(x[i] * k);
}
static void to_i16(i16* out, const float* a, const float* b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        long s = lrint((double)(a[i] + (b ? b[i] : 0.0f)) * 32767.0);
        out[i] = (i16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
    }
}
/* each block of 480 samples of in through a new reducer of method, into out, lined up for its delay */
static u32 run_method(int method, const i16* in, i16* out, u32 n) {
    SnxDenoise* d = snx_denoise_new(48000, 480, method, mic_say);
    CHECK(d != NULL && snx_denoise_method(d) == method);
    u32 delay = snx_denoise_delay(d);
    static i16 block[480];
    for (u32 i = 0; i + 480 <= n; i += 480) {
        memcpy(block, in + i, sizeof block);
        snx_denoise_run(d, block);
        for (u32 k = 0; k < 480; k++)
            if (i + k >= delay) out[i + k - delay] = block[k];
    }
    snx_denoise_free(d);
    return delay;
}
/* powers over the last 4 s, as dB: the gaps' cores and the syllables' cores (30 ms in from each edge) */
static double part_db(const i16* x, const float* ref, int voiced) {
    double sum = 0;
    u32 count = 0;
    for (u32 i = 4 * 48000; i + 480 < TOTAL; i++) {
        u32 in = i % SYLLABLE;
        int core = voiced ? in >= 1440 && in < VOICED - 1440 : in >= VOICED + 1440 && in < SYLLABLE - 1440;
        if (!core) continue;
        double v = x ? x[i] / 32768.0 : ref[i];
        sum += v * v;
        count++;
    }
    return 10.0 * log10(sum / count + 1e-20);
}
/* the error against the clean speech over the syllables' cores, as dB, x read shift samples later */
static double error_db(const i16* x, const float* speech, u32 shift) {
    double sum = 0;
    u32 count = 0;
    for (u32 i = 4 * 48000; i + 480 + shift < TOTAL; i++) {
        u32 in = i % SYLLABLE;
        if (in < 1440 || in >= VOICED - 1440) continue;
        double e = x[i + shift] / 32768.0 - speech[i];
        sum += e * e;
        count++;
    }
    return 10.0 * log10(sum / count + 1e-20);
}

static float g_speech[TOTAL], g_steady[TOTAL], g_babble[TOTAL], g_talker[TOTAL];
static i16 g_mix[TOTAL], g_out[TOTAL], g_clean[TOTAL], g_clean_out[TOTAL];

/* one method on one noise: the noise alone lowered (the gaps), the speech kept (its power with the noise in, and
   alone), and the signal-to-noise ratio over the syllables before and after */
static void measure(const char* method_name, int method, const char* noise_name, const float* noise, double* lowered,
                    double* snr_gain, double* kept) {
    to_i16(g_mix, g_speech, noise, TOTAL);
    to_i16(g_clean, g_speech, NULL, TOTAL);
    memset(g_out, 0, sizeof g_out);
    memset(g_clean_out, 0, sizeof g_clean_out);
    run_method(method, g_mix, g_out, TOTAL);
    run_method(method, g_clean, g_clean_out, TOTAL);
    double noise_in = part_db(NULL, noise, 0), noise_out = part_db(g_out, NULL, 0);
    double speech_in = part_db(NULL, g_speech, 1), speech_alone = part_db(g_clean_out, NULL, 1);
    /* the lag of the speech through it beyond what it says: the least error of the clean speech's output */
    u32 shift = 0;
    double best = 1e9;
    for (u32 k = 0; k <= 960; k++) {
        double e = error_db(g_clean_out, g_speech, k);
        if (e < best) best = e, shift = k;
    }
    CHECK(shift <= 2); /* the delay it says is the delay it has */
    double snr_in = speech_in - part_db(NULL, noise, 1), snr_out = speech_in - error_db(g_out, g_speech, shift);
    *lowered = noise_in - noise_out;
    *snr_gain = snr_out - snr_in;
    *kept = speech_alone - speech_in;
    printf("audio_mic: %s on %s noise: the noise alone %.1f -> %.1f dB (%.1f dB lower); syllables SNR %.1f -> %.1f dB "
           "(%+.1f); the speech alone through it %+.1f dB, %u samples later than said, its own change to it %.1f dB "
           "under it (the SNR it could reach)\n",
           method_name, noise_name, noise_in, noise_out, *lowered, snr_in, snr_out, *snr_gain, *kept, shift,
           speech_in - best);
}

/* the cost of a block of 10 ms at 48 kHz, in us */
static double block_us(int method) {
    SnxDenoise* d = snx_denoise_new(48000, 480, method, mic_say);
    static i16 block[480];
    struct timespec t0, t1;
    CHECK(d != NULL);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < 1000; r++) {
        memcpy(block, g_mix + (u32)(r % 700) * 480, sizeof block);
        snx_denoise_run(d, block);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    snx_denoise_free(d);
    return ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / 1000.0 / 1000.0;
}

static void test_reduction(void) {
    double lowered, gain, kept;
    make_speech(g_speech, TOTAL, 1, 0);
    scale_to(g_speech, TOTAL, 0.1, 1); /* -20 dBFS over the syllables */
    for (u32 i = 0; i < TOTAL; i++) g_steady[i] = (float)noise_at(i);
    scale_to(g_steady, TOTAL, 0.03, 0);          /* -30 dBFS */
    for (u32 talker = 0; talker < 6; talker++) { /* six other talkers, out of step: a room's babble */
        make_speech(g_talker, TOTAL, 100 + talker, talker * 2900 + 1000);
        for (u32 i = 0; i < TOTAL; i++) g_babble[i] += g_talker[i];
    }
    scale_to(g_babble, TOTAL, 0.03, 0);

    /* the neural method, the copy built in, checked: a steady noise and babble lowered, the speech kept, its SNR
       better */
    measure("neural", SNX_DENOISE_NEURAL, "steady", g_steady, &lowered, &gain, &kept);
    CHECK(lowered >= 10.0 && gain >= 3.0 && kept > -6.0);
    measure("neural", SNX_DENOISE_NEURAL, "babble", g_babble, &lowered, &gain, &kept);
    CHECK(lowered >= 3.0 && kept > -6.0);
    /* it is the copy built in, whatever librnnoise the host has: each block what that copy's own call makes of it. A
       system librnnoise 0.4.1 gave other samples and another delay */
    {
        SnxDenoise* neural = snx_denoise_new(48000, 480, SNX_DENOISE_NEURAL, mic_say);
        DenoiseState* copy = rnnoise_create(NULL);
        static i16 block[480];
        static float in[480], out[480];
        u32 differ = 0;
        CHECK(neural != NULL && copy != NULL);
        for (u32 i = 0; i + 480 <= 48000; i += 480) {
            memcpy(block, g_mix + i, sizeof block);
            for (u32 k = 0; k < 480; k++) in[k] = block[k];
            snx_denoise_run(neural, block);
            rnnoise_process_frame(copy, out, in);
            for (u32 k = 0; k < 480; k++)
                differ += block[k] != (i16)(out[k] > 32767.0f ? 32767 : out[k] < -32768.0f ? -32768 : lrintf(out[k]));
        }
        CHECK(differ == 0);
        rnnoise_destroy(copy);
        snx_denoise_free(neural);
    }
    printf(
        "audio_mic: neural: a block of 10 ms takes %.1f us (%.2f%% of one core), and the output lags by %u samples\n",
        block_us(SNX_DENOISE_NEURAL), block_us(SNX_DENOISE_NEURAL) / 100.0, 480u);
    /* not 48 kHz: the spectral one, or none */
    SnxDenoise* d = snx_denoise_new(16000, 160, SNX_DENOISE_NEURAL, mic_say);
    CHECK(!d || snx_denoise_method(d) == SNX_DENOISE_SPECTRAL);
    snx_denoise_free(d);
    CHECK(snx_denoise_new(48000, 480, SNX_DENOISE_OFF, mic_say) == NULL);

    d = snx_denoise_new(48000, 480, SNX_DENOISE_SPECTRAL, mic_say);
    if (!d) {
        printf("audio_mic: %s is not there: the spectral checks are skipped\n", SNX_DENOISE_LIBRARY);
    } else {
        snx_denoise_free(d);
        measure("spectral", SNX_DENOISE_SPECTRAL, "steady", g_steady, &lowered, &gain, &kept);
        CHECK(lowered >= 10.0 && kept > -6.0);
        measure("spectral", SNX_DENOISE_SPECTRAL, "babble", g_babble, &lowered, &gain, &kept);
        printf("audio_mic: spectral: a block of 10 ms takes %.1f us\n", block_us(SNX_DENOISE_SPECTRAL));
    }

    /* through the stream, the setting at Neural: the speech over the steady noise, the noise lowered in the gaps */
    to_i16(g_mix, g_speech, g_steady, TOTAL);
    test_source = g_mix;
    test_source_len = TOTAL;
    g_settings.microphone_noise_reduction = SNX_DENOISE_NEURAL;
    test_host_ns = 0;
    u32 n = 0;
    CHECK(audio_purge_input(&cpu, STREAM) == 0);
    CHECK(audio_start_input(&cpu, STREAM) == 0 &&
          snx_denoise_method(input_of(STREAM)->mic->denoise) == SNX_DENOISE_NEURAL);
    for (int t = 0; t < 1600; t++) {
        tick();
        read_all(STREAM, g_out, &n, TOTAL);
    }
    CHECK(n == TOTAL);
    u32 lead = 0; /* the hold's silence, then the reducer's frame of delay, then the signal */
    while (lead < n && g_out[lead] == 0) lead++;
    CHECK(lead >= 1440 && lead <= 2880 + 480);
    memmove(g_out, g_out + lead, (TOTAL - lead) * sizeof *g_out);
    memset(g_out + TOTAL - lead, 0, lead * sizeof *g_out);
    /* the lead counts the reducer's frame too: put it back so that the output lines up with the source */
    double gaps = part_db(g_out, NULL, 0), noise_in = part_db(NULL, g_steady, 0);
    printf("audio_mic: through the stream (neural): the noise alone %.1f -> %.1f dB\n", noise_in, gaps);
    CHECK(noise_in - gaps >= 10.0);
    /* the setting changed to Off while it records: the next blocks pass as recorded */
    g_settings.microphone_noise_reduction = SNX_DENOISE_OFF;
    for (int t = 0; t < 4; t++) tick();
    CHECK(input_of(STREAM)->mic->denoise == NULL);
    CHECK(audio_stop_input(&cpu, STREAM) == 0);
    test_source = NULL;
}

int main(void) {
    g_mic_fake.signal = signal_at;
    g_mem = calloc(1, 0x40000);
    CHECK(g_mem != NULL);
    test_open();
    u32 lead = test_pace();
    test_short();
    test_gain_stop();
    test_off();
    test_float();
    test_reduction_said();
    (void)lead;
    test_reduction();
    audio_close_input(&cpu, STREAM);
    CHECK(test_event.o.refs == 0 && g_mic_fake.dev == NULL);
    printf("audio_mic: %u checks passed\n", test_checks);
    free(g_mem);
    return 0;
}
