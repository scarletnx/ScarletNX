/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* sceAudioIn (src/vita/hle/audio.c) over a fake recording device (support/mic_fake.c), never a real one: the port
   records at its rate from its open, each grain is the recording's next samples, little-endian, silence before the
   recording holds its 40 ms or with no device, the release closes the device, and a state loaded with the port open
   records again */
#include "vita/hle/hle.h"
#include "vita/hle/hle.hook.h"
#include "common/utilities/common.h"
#include "common/utilities/endian.h"
#include "common/utilities/state_io.h"
#include "snx_settings.h"
#include "support/mic_fake.h"
#include <SDL3/SDL.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GRAIN 256u
#define RATE  16000u

/* a state in memory */
static struct {
    unsigned char bytes[4096];
    u64 size, at;
} g_state;
static int put(void* ctx, const void* p, u64 n) {
    (void)ctx;
    if (g_state.size + n > sizeof g_state.bytes) return -1;
    memcpy(g_state.bytes + g_state.size, p, n);
    g_state.size += n;
    return 0;
}
static int get(void* ctx, void* p, u64 n) {
    (void)ctx;
    if (g_state.at + n > g_state.size) return -1;
    memcpy(p, g_state.bytes + g_state.at, n);
    g_state.at += n;
    return 0;
}

int main(void) {
    SDL_SetHint(SDL_HINT_AUDIO_DRIVER, "dummy"); /* the process's audio starts SDL's, which reaches no sound server */
    g_settings.microphone[0] = 0;              /* the system's default device, the fake one; "off" by default */
    g_settings.microphone_noise_reduction = 0; /* the samples as recorded */
    char *root = snx_temp_dir(NULL), *folder = root ? snx_path_join(root, "game") : NULL, error[256] = {0};
    assert(folder && !snx_mkdirs(folder));
    VitaImage image = {0};
    VitaProcess* p = vita_process_new(&image, folder, error, sizeof error);
    if (!p) fprintf(stderr, "vita_process_new: %s\n", error);
    assert(p);
    VitaThread* t = &p->threads[0];
    p->current = t;
    uint32_t dest = vita_alloc(p, &p->user, 0x1000, 0x1000);
    assert(dest);
    /* the port opens the device at its rate */
    uint32_t port = vita_audio_in_open_port(p, t, 0, GRAIN, RATE, 0);
    assert(port == 0x100 && g_mic_fake.opens == 1 && g_mic_fake.dev && g_mic_fake.dev->rate == RATE);
    /* 32 ms recorded: short of its 40 ms (640), the grain is silence */
    g_mic_fake.dev->recorded = 512;
    memset(vita_ptr(p, dest), 0xa5, GRAIN * 2);
    assert(!vita_audio_in_input(p, t, port, dest));
    for (uint32_t i = 0; i < GRAIN; ++i) assert(snx_ld_le16((uint8_t*)vita_ptr(p, dest) + i * 2) == 0);
    /* 50 ms: the ramp from its first sample, a grain at a time */
    g_mic_fake.dev->recorded = 800;
    assert(!vita_audio_in_input(p, t, port, dest));
    for (uint32_t i = 0; i < GRAIN; ++i) assert(snx_ld_le16((uint8_t*)vita_ptr(p, dest) + i * 2) == i);
    assert(!vita_audio_in_input(p, t, port, dest));
    for (uint32_t i = 0; i < GRAIN; ++i) assert(snx_ld_le16((uint8_t*)vita_ptr(p, dest) + i * 2) == GRAIN + i);
    /* a state saved with the port open, loaded: the port records again, from a device of its own */
    StateIo io = {.put = put, .get = get};
    assert(!vita_audio_state_save(p, &io));
    void* staged = vita_audio_state_parse(&io, error, sizeof error);
    assert(staged);
    vita_audio_state_install(p, staged);
    assert(g_mic_fake.opens == 2 && g_mic_fake.closes == 1 && g_mic_fake.dev && g_mic_fake.dev->rate == RATE);
    /* the release closes it */
    assert(!vita_audio_in_release_port(p, t, port) && g_mic_fake.closes == 2 && !g_mic_fake.dev);
    /* no device: silence */
    g_mic_fake.none = true;
    port = vita_audio_in_open_port(p, t, 0, GRAIN, RATE, 0);
    assert(port == 0x100 && g_mic_fake.opens == 3 && !g_mic_fake.dev);
    memset(vita_ptr(p, dest), 0xa5, GRAIN * 2);
    assert(!vita_audio_in_input(p, t, port, dest));
    for (uint32_t i = 0; i < GRAIN; ++i) assert(snx_ld_le16((uint8_t*)vita_ptr(p, dest) + i * 2) == 0);
    assert(!vita_audio_in_release_port(p, t, port));
    vita_process_free(p);
    assert(!snx_remove_tree(root));
    free(folder);
    free(root);
    puts("vita_audio_in: ok");
    return 0;
}
