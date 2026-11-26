/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/mic.h"
#include "common/utilities/denoise.h"
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct SnxMic {
    SnxMicDevice* dev;
    SnxDenoise* denoise; /* NULL: the samples pass as recorded */
    const int* noise;    /* the setting the reducer follows */
    int asked;           /* the method the reducer was made for (the setting's value then) */
    SnxMicSay say;
    uint32_t rate;
    uint32_t block, pending_n; /* the samples of a block, and how many of the next one are recorded so far */
    int16_t* pending;          /* the next block */
    int16_t* ready;            /* a ring of size samples, count of them from head */
    uint32_t size, head, count;
    uint32_t hold, slack; /* SNX_MIC_HOLD_MS (and what shorts added) and SNX_MIC_SLACK_MS in samples */
    int primed;           /* hold was reached, and nothing ran short since */
    uint64_t shorts, dropped;
};
/* each kind of line's count, which says it at most 8 times a run */
enum { SAY_OFF, SAY_NONE, SAY_OPEN, SAY_SHORT, SAY_CHANGE, SAY_KINDS };
static struct {
    _Atomic unsigned said[SAY_KINDS];
} g_mic;

[[gnu::format(printf, 3, 4)]] static void say(const SnxMic* m, int kind, const char* fmt, ...) {
    if (atomic_fetch_add_explicit(&g_mic.said[kind], 1, memory_order_relaxed) >= 8) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    m->say(false, line);
}
static int noise_now(const SnxMic* m) {
    return atomic_load_explicit((const _Atomic int*)m->noise, memory_order_relaxed);
}

void snx_mic_free(SnxMic* m) {
    if (!m) return;
    snx_mic_device_close(m->dev);
    snx_denoise_free(m->denoise);
    free(m->pending);
    free(m->ready);
    free(m);
}
/* the noise reduction a recording runs, and when it is not the one mic_noise_reduction asks for, that value and why:
   the neural one at another rate than it takes, or the spectral one without the system's library */
static void reduction(const SnxMic* m, char* out, size_t size) {
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
            method[runs], m->asked, (unsigned)SNX_DENOISE_NEURAL_RATE, (unsigned)m->rate,
            runs == SNX_DENOISE_OFF ? ", and spectral needs " : "", runs == SNX_DENOISE_OFF ? SNX_DENOISE_LIBRARY : "");
    else if (asked == SNX_DENOISE_SPECTRAL)
        snprintf(out, size, "%s, not spectral as mic_noise_reduction=%d asks: spectral needs %s", method[runs],
                 m->asked, SNX_DENOISE_LIBRARY);
    else
        snprintf(out, size, "%s, not %s as mic_noise_reduction=%d asks: its reducer could not be made", method[runs],
                 method[asked], m->asked);
}

SnxMic* snx_mic_open(uint32_t rate, const char* device, const int* noise, SnxMicSay say_line) {
    SnxMic* m = calloc(1, sizeof *m);
    if (!m) return NULL;
    m->say = say_line;
    m->noise = noise;
    m->block = rate / 100;
    m->size = rate / 2;
    m->hold = (uint32_t)((uint64_t)rate * SNX_MIC_HOLD_MS / 1000);
    m->slack = (uint32_t)((uint64_t)rate * SNX_MIC_SLACK_MS / 1000);
    m->pending = calloc(m->block, sizeof *m->pending);
    m->ready = calloc(m->size, sizeof *m->ready);
    if (!m->pending || !m->ready) {
        snx_mic_free(m);
        return NULL;
    }
    if (strcmp(device, "off")) m->dev = snx_mic_device_open(rate, device, say_line);
    m->rate = rate;
    m->asked = noise_now(m);
    if (m->dev) m->denoise = snx_denoise_new(rate, m->block, m->asked, say_line);
    /* said for the first starts only: a game may start and stop its stream for each recording */
    if (!strcmp(device, "off"))
        say(m, SAY_OFF, "audio: an input stream started with the microphone off: the game hears silence\n");
    else if (!m->dev)
        say(m, SAY_NONE, "audio: an input stream started with no host microphone: the game hears silence\n");
    else {
        char said[192];
        reduction(m, said, sizeof said);
        say(m, SAY_OPEN, "audio: an input stream records the host's microphone at %u Hz, noise reduction %s\n",
            (unsigned)rate, said);
    }
    return m;
}
uint32_t snx_mic_rate(const SnxMic* m) { return m->rate; }
/* n samples to the ready ring, its oldest dropped when it is full */
static void push(SnxMic* m, const int16_t* x, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
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
static void block(SnxMic* m) {
    int want = noise_now(m);
    if (want != m->asked) {
        char said[192];
        snx_denoise_free(m->denoise);
        m->denoise = snx_denoise_new(m->rate, m->block, want, m->say);
        m->asked = want;
        reduction(m, said, sizeof said);
        say(m, SAY_CHANGE, "audio: the microphone's noise reduction is now %s\n", said);
    }
    snx_denoise_run(m->denoise, m->pending);
    push(m, m->pending, m->block);
    m->pending_n = 0;
}
/* what the device recorded since the last turn, through the blocks into the ready ring. More than the ring holds (a
   long stall of the clock) loses its oldest at once */
static void pump(SnxMic* m) {
    int16_t skip[1024];
    uint32_t avail = snx_mic_device_available(m->dev);
    while (avail > m->size) {
        uint32_t got = snx_mic_device_read(m->dev, skip, avail - m->size < 1024 ? avail - m->size : 1024, m->say);
        if (!got) break;
        avail -= got;
        m->dropped += got;
    }
    while (avail) {
        uint32_t room = m->block - m->pending_n, got;
        got = snx_mic_device_read(m->dev, m->pending + m->pending_n, avail < room ? avail : room, m->say);
        if (!got) break;
        avail -= got;
        m->pending_n += got;
        if (m->pending_n == m->block) block(m);
    }
}

void snx_mic_take(SnxMic* m, int16_t* out, uint32_t n) {
    uint32_t k = 0;
    if (m && m->dev) {
        pump(m);
        if (!m->primed && m->count >= m->hold) m->primed = 1;
        if (m->primed) {
            k = n < m->count ? n : m->count;
            for (uint32_t i = 0; i < k; i++) out[i] = m->ready[(m->head + i) % m->size];
            m->head = (m->head + k) % m->size;
            m->count -= k;
            if (k < n) { /* a device that delivers in larger bursts: hold 10 ms more, up to 200 ms */
                m->primed = 0;
                if (m->hold + m->block <= m->size * 2 / 5) m->hold += m->block;
                if (!m->shorts++)
                    say(m, SAY_SHORT,
                        "audio: the host's microphone ran short of guest time: silence until it catches up\n");
            }
        }
        if (m->count > m->hold + m->slack) {
            uint32_t drop = m->count - m->hold;
            m->head = (m->head + drop) % m->size;
            m->count -= drop;
            m->dropped += drop;
        }
    }
    memset(out + k, 0, (size_t)(n - k) * sizeof *out);
}
