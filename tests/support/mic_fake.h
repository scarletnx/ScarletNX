/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The host's recording device in the tests of a game's microphone (common/platform/mic.h): a fake one in the SDL
   one's place, so no test opens a real microphone. Its object, linked before snx_mic, defines every function of
   common/platform/mic.c, whose own object is then never linked. One device at a time: the test raises what it has
   recorded, and its sample n is the test's signal(n) */
#ifndef SNX_TEST_MIC_FAKE_H
#define SNX_TEST_MIC_FAKE_H
#include "common/platform/mic.h"

struct SnxMicDevice {
    uint32_t rate;
    uint64_t recorded, read; /* samples it has recorded since it opened, and those taken */
};
extern struct MicFake {
    SnxMicDevice* dev;             /* the device open, NULL for none */
    unsigned opens, closes;        /* each open asked (none found counts too), each close of a device */
    bool none;                     /* each open finds no device */
    char device[64];               /* the text the last open was given */
    int16_t (*signal)(uint64_t n); /* sample n of what it records; NULL: n itself, as a 16-bit ramp */
} g_mic_fake;
#endif
