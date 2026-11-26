/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "mic_fake.h"
#include <stdio.h>
#include <stdlib.h>

struct MicFake g_mic_fake;

[[noreturn]] static void fail(const char* why) {
    fprintf(stderr, "mic_fake.c: %s\n", why);
    exit(1);
}
SnxMicDevice* snx_mic_device_open(uint32_t rate, const char* device, SnxMicSay say) {
    (void)say;
    g_mic_fake.opens++;
    snprintf(g_mic_fake.device, sizeof g_mic_fake.device, "%s", device);
    if (g_mic_fake.none) return NULL;
    if (g_mic_fake.dev) fail("a second device opened while one is open");
    if (!(g_mic_fake.dev = calloc(1, sizeof *g_mic_fake.dev))) fail("no memory for a device");
    g_mic_fake.dev->rate = rate;
    return g_mic_fake.dev;
}
void snx_mic_device_close(SnxMicDevice* m) {
    if (!m) return;
    if (m != g_mic_fake.dev) fail("a device closed that is not the one open");
    g_mic_fake.closes++;
    free(m);
    g_mic_fake.dev = NULL;
}
uint32_t snx_mic_device_available(SnxMicDevice* m) { return m ? (uint32_t)(m->recorded - m->read) : 0; }
uint32_t snx_mic_device_read(SnxMicDevice* m, int16_t* samples, uint32_t frames, SnxMicSay say) {
    (void)say;
    if (!m) return 0;
    uint32_t n = frames < m->recorded - m->read ? frames : (uint32_t)(m->recorded - m->read);
    for (uint32_t i = 0; i < n; i++)
        samples[i] = g_mic_fake.signal ? g_mic_fake.signal(m->read + i) : (int16_t)(uint16_t)(m->read + i);
    m->read += n;
    return n;
}
