/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The input streams of the Ounce's SDK (src/nx/hle/hook/audio_stream.c) over a synthetic microphone: their handlers
   called as the hooks call them, on a guest buffer, with a clock the test moves and a host recording device
   (plat_mic_*) that plays a generated signal in bursts, never a real one. Checked: what an open leaves for the
   format query, the device properties, the frames' count in guest time and their format on every channel, the
   device opened by a start and closed by a stop, the microphone setting off, the hold before the first frames, a
   device that runs short or ahead, the gain. Then the noise reduction (src/nx/runtime/platform/audio_denoise.c, the
   system's libspeexdsp): off, the game gets the signal sample for sample; on, a steady noise under tone bursts is
   lowered by a measured amount while the bursts pass. Without the library those two checks are skipped, said */
#include "nx/hle/services/services_int.h"
#include <stdarg.h>
#include <time.h>

#include "nx/hle/hook/audio_stream.c"
#include "nx/runtime/platform/audio_denoise.c"

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
void kobj_unref(KObj* o) { o->refs--; }
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
static i16 signal_at(u64 n) {
    double v = (test_tone ? tone_at(n) : 0.0) + (test_noise ? noise_at(n) : 0.0);
    long s = lrint(v * 32767.0);
    return (i16)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
}

/* the device: it records at 48 kHz scaled by test_speed (in 1/1000ths), and hands its samples over every 20 ms of
   host time, as a device with a buffer of 960 frames does. test_stall stops it */
struct PMic {
    u32 rate;
    u64 recorded, read; /* samples the device has, and those taken */
};
static PMic* test_mic;
static unsigned test_opens, test_closes;
static int test_no_mic, test_stall;
static u32 test_speed = 1000;
static char test_device[64];
PMic* plat_mic_open(u32 rate, const char* device) {
    test_opens++;
    snprintf(test_device, sizeof test_device, "%s", device);
    if (test_no_mic) return NULL;
    CHECK(test_mic == NULL);
    test_mic = calloc(1, sizeof *test_mic);
    CHECK(test_mic != NULL);
    test_mic->rate = rate;
    return test_mic;
}
void plat_mic_close(PMic* m) {
    if (!m) return;
    CHECK(m == test_mic);
    test_closes++;
    free(m);
    test_mic = NULL;
}
u32 plat_mic_available(PMic* m) { return (u32)(m->recorded - m->read); }
u32 plat_mic_read(PMic* m, i16* samples, u32 frames) {
    u32 n = frames < m->recorded - m->read ? frames : (u32)(m->recorded - m->read);
    for (u32 i = 0; i < n; i++) samples[i] = signal_at(m->read + i);
    m->read += n;
    return n;
}
static u64 test_host_ns; /* the device's own time, which bursts of 20 ms */
static void device_run(u64 ns) {
    u64 before = test_host_ns / 20000000ull;
    test_host_ns += ns;
    if (!test_mic || test_stall) return;
    for (u64 b = before; b < test_host_ns / 20000000ull; b++)
        test_mic->recorded += (u64)test_mic->rate / 50 * test_speed / 1000;
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
    CHECK(test_opens == 0); /* no device before the start */
}

/* frames in guest time, with the device's hold before them: s16 stereo, the signal sample for sample with the
   reduction off. Returns the frames of silence before the signal */
static u32 test_pace(void) {
    static i16 got[48000 * 2];
    u32 n = 0, total = 0, lead = 0;
    g_settings.microphone_noise_reduction = 0;
    test_signals = 0;
    CHECK(audio_start_input(&cpu, STREAM) == 0 && test_opens == 1 && test_mic != NULL && test_device[0] == 0);
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
    CHECK(test_mic->read == test_mic->recorded); /* nothing held back past a block: the reducer's block is 10 ms */
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
        Mic* m = input_of(STREAM)->mic;
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
    CHECK(audio_stop_input(&cpu, STREAM) == 0 && test_closes == 1 && test_mic == NULL);
    i32 left = audio_input_filled_frames(&cpu, STREAM);
    CHECK(left > 0);
    tick();
    CHECK(audio_input_filled_frames(&cpu, STREAM) == left);
    CHECK(audio_purge_input(&cpu, STREAM) == 0 && audio_input_filled_frames(&cpu, STREAM) == 0);
}

/* the setting off: no device opened, silence at the same pace. A host without a device: the same */
static void test_off(void) {
    u32 n = 0;
    static i16 got[4800];
    snprintf(g_settings.microphone, sizeof g_settings.microphone, "off");
    unsigned opens = test_opens;
    CHECK(audio_start_input(&cpu, STREAM) == 0 && test_opens == opens && test_mic == NULL);
    for (int t = 0; t < 10; t++) tick();
    CHECK(read_all(STREAM, got, &n, 4800) == 2400);
    for (u32 i = 0; i < n; i++) CHECK(got[i] == 0);
    CHECK(audio_stop_input(&cpu, STREAM) == 0);
    g_settings.microphone[0] = 0;
    test_no_mic = 1;
    CHECK(audio_start_input(&cpu, STREAM) == 0 && test_opens == opens + 1 && test_mic == NULL);
    for (int t = 0; t < 10; t++) tick();
    n = 0;
    CHECK(read_all(STREAM, got, &n, 4800) == 2400 && got[0] == 0 && got[n - 1] == 0);
    CHECK(audio_stop_input(&cpu, STREAM) == 0);
    test_no_mic = 0;
}

/* another format: 32-bit float, mono, 16 kHz, and a device named by the setting */
static void test_float(void) {
    format(FORMAT, 5, 1, 16000);
    snprintf(g_settings.microphone, sizeof g_settings.microphone, "USB");
    CHECK(audio_open_input(&cpu, STREAM2, 0, 0, NAMES, FORMAT, 1600) == 0);
    CHECK(audio_start_input(&cpu, STREAM2) == 0 && !strcmp(test_device, "USB") && test_mic->rate == 16000);
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
    CHECK(test_mic == NULL && snx_ld_le64(GP(STREAM2)) == 0 && audio_input_state(&cpu, STREAM2) == 0);
    g_settings.microphone[0] = 0;
    format(FORMAT, 2, 2, 48000);
}

/* power in dB of x[from, to) */
static double power_db(const double* x, u32 from, u32 to) {
    double sum = 0;
    for (u32 i = from; i < to; i++) sum += x[i] * x[i];
    return 10.0 * log10(sum / (double)(to - from) + 1e-20);
}
/* the reduction: 8 s of the signal with it on, measured over the last 4 s (the reducer learns the noise over the
   first seconds). Each gap's power (the noise alone) and each burst's (the tone and what noise is left), 30 ms away
   from their edges, against the same stretch of the signal recorded */
static void test_reduction(u32 lead) {
    static i16 got[48000 * 9];
    static double in_x[48000 * 9], out_x[48000 * 9];
    const u32 edge = 1440;
    double nin = 0, nout = 0, tin = 0, tout = 0;
    u32 n = 0, nn = 0, tn = 0;
    struct timespec t0, t1;
    SnxDenoise* d = snx_denoise_new(48000, 480, SNX_DENOISE_SUPPRESS_DB);
    if (!d) {
        printf("audio_mic: %s is not there: the noise reduction checks are skipped\n", SNX_DENOISE_LIBRARY);
        return;
    }
    /* the cost of a block of 10 ms at 48 kHz */
    static i16 block[480];
    for (u32 i = 0; i < 480; i++) block[i] = signal_at(i);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < 2000; r++) snx_denoise_run(d, block);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((double)(t1.tv_sec - t0.tv_sec) * 1e9 + (double)(t1.tv_nsec - t0.tv_nsec)) / 2000.0 / 1000.0;
    printf("audio_mic: a block of 10 ms takes %.1f us to reduce, %.2f%% of one core\n", us, us / 100.0);
    snx_denoise_free(d);

    g_settings.microphone_noise_reduction = 1;
    test_host_ns = 0; /* the device's bursts in the same phase as test_pace's: the same hold before the signal */
    CHECK(audio_purge_input(&cpu, STREAM) == 0);
    CHECK(audio_start_input(&cpu, STREAM) == 0 && test_mic != NULL && input_of(STREAM)->mic->denoise != NULL);
    for (int t = 0; t < 1600; t++) {
        tick();
        read_all(STREAM, got, &n, sizeof got / sizeof *got);
    }
    CHECK(n == 8 * 48000);
    for (u32 i = 0; i + lead < n; i++) {
        in_x[i] = (double)signal_at(i) / 32768.0;
        out_x[i] = (double)got[i + lead] / 32768.0;
    }
    for (u32 start = (4 * 48000 + BURST - 1) / BURST * BURST; start + BURST + lead <= n; start += BURST) {
        double pi = power_db(in_x, start + edge, start + BURST - edge);
        double po = power_db(out_x, start + edge, start + BURST - edge);
        if (burst_on(start))
            tin += pi, tout += po, tn++;
        else
            nin += pi, nout += po, nn++;
    }
    CHECK(nn >= 4 && tn >= 4);
    nin /= nn, nout /= nn, tin /= tn, tout /= tn;
    printf("audio_mic: the noise alone %.1f dB -> %.1f dB (%.1f dB lower), the bursts %.1f dB -> %.1f dB (%+.1f dB)\n",
           nin, nout, nin - nout, tin, tout, tout - tin);
    CHECK(nin - nout >= 10.0);      /* a steady noise lowered */
    CHECK(fabs(tout - tin) <= 3.0); /* the bursts pass */
    CHECK(audio_stop_input(&cpu, STREAM) == 0);
}

int main(void) {
    g_mem = calloc(1, 0x40000);
    CHECK(g_mem != NULL);
    test_open();
    u32 lead = test_pace();
    test_short();
    test_gain_stop();
    test_off();
    test_float();
    test_reduction(lead);
    audio_close_input(&cpu, STREAM);
    CHECK(test_event.o.refs == 0 && test_mic == NULL);
    printf("audio_mic: %u checks passed\n", test_checks);
    free(g_mem);
    return 0;
}
