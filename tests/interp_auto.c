/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Frame interpolation's Auto gate (common/gpu/vk/interp_auto.c): off until the game's fastest 1 % of frames leaves room
   for two presents in a refresh period for a second, on through a hitch, and off only after two seconds of failing */
#include "common/gpu/vk/interp_auto.h"
#include <stdio.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "interp_auto:%d: %s\n", __LINE__, #x);                                                     \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

#define MS 1000000ull

/* n frames at interval_ns from t on, the display's rate: the gate's answer after each */
static int feed(SnxInterpAuto* a, uint64_t* t, uint64_t interval_ns, unsigned n, int display_hz) {
    int on = 0;
    for (unsigned i = 0; i < n; ++i) {
        *t += interval_ns;
        on = snx_interp_auto_frame(a, *t, display_hz);
    }
    return on;
}

int main(void) {
    SnxInterpAuto a;
    uint64_t t = 0;
    /* 60 frames a second on a 120 Hz display: on after a second of fit */
    snx_interp_auto_init(&a);
    CHECK(!feed(&a, &t, 16666667, 30, 120)); /* half a second: the figures are not taken yet */
    CHECK(feed(&a, &t, 16666667, 120, 120)); /* a second of fit turns it on, with the half second's sampling */
    /* 60 on 60: the two presents would not fit, so never on */
    snx_interp_auto_init(&a);
    CHECK(!feed(&a, &t, 16666667, 240, 60));
    /* 30 on 60: it fits, on after a second */
    snx_interp_auto_init(&a);
    CHECK(!feed(&a, &t, 33333333, 30, 60));
    CHECK(feed(&a, &t, 33333333, 60, 60));
    CHECK(feed(&a, &t, 33333333, 30, 60)); /* the second of fit, then the sample */
    /* then 60 a second on that display: still on for a second, off after two */
    CHECK(feed(&a, &t, 16666667, 60, 60));
    CHECK(!feed(&a, &t, 16666667, 180, 60)); /* two seconds of failing, and the sample */
    /* a game that hitches every other frame is not steady: no blend */
    snx_interp_auto_init(&a);
    for (int i = 0; i < 300; ++i) {
        t += i & 1 ? 100 * MS : 16 * MS;
        CHECK(!snx_interp_auto_frame(&a, t, 120));
    }
    /* 20 a second on a 60 Hz display: the 1.9 rule fits, but the 1 % low is under 25, so no blend */
    snx_interp_auto_init(&a);
    for (int i = 0; i < 300; ++i) {
        t += 50 * MS;
        CHECK(!snx_interp_auto_frame(&a, t, 60));
    }
    /* no display rate: never on */
    snx_interp_auto_init(&a);
    CHECK(!feed(&a, &t, 33333333, 240, 0));
    puts("interp_auto: passed");
    return 0;
}
