/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's clock (board/clock.c), which the Switch's fps_limit paces: the host's at the console's pace, N/60 of it
   for N, its waits skipped with no limit; a change of pace never moving it back; F4's toggle; and fps_limit's text read
   as the Switch reads it, each value not taken as given said with the one in effect */
#include "3ds/board/clock.h"
#include "common/utilities/common.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks;
#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "3ds_clock.c:%d: check failed: %s\n", __LINE__, #e);                                       \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* within a millisecond: what runs between two reads of the clock in this test takes far less */
static bool near(double a, double b) { return fabs(a - b) < 0.001; }
/* fps_limit's text: the pace it gives, whether read, and what is said */
static void parse(const char* text, int start, bool read, int want, const char* line) {
    int pace = start;
    char said[192];
    bool ok = snx_ctr_clock_parse(text, &pace, said, sizeof said);
    if (ok != read || pace != want || strcmp(said, line)) {
        fprintf(stderr, "fps_limit=%s: %s, pace %d, \"%s\"; expected %s, %d, \"%s\"\n", text, ok ? "read" : "not read",
                pace, said, read ? "read" : "not read", want, line);
        exit(1);
    }
    ++checks;
}
int main(void) {
    /* at start, the host's clock at the console's pace */
    CHECK(snx_ctr_clock_pace() == SNX_CTR_PACE_CONSOLE && snx_ctr_clock_rate() == 1);
    CHECK(near(snx_ctr_clock_seconds(), snx_seconds()));
    /* 120 frames a second: twice the console's pace, from where the clock stood; a wait of 10 s of the game's is 5 of
       the host's */
    double before = snx_ctr_clock_seconds();
    snx_ctr_clock_set_pace(120);
    CHECK(snx_ctr_clock_rate() == 2 && near(snx_ctr_clock_seconds(), before));
    CHECK(near(snx_ctr_clock_wait(snx_ctr_clock_seconds() + 10), 5));
    CHECK(snx_ctr_clock_wait(snx_ctr_clock_seconds() - 1) == 0); /* past: none */
    /* no limit: a wait is none, the clock moving to its deadline */
    snx_ctr_clock_set_pace(SNX_CTR_PACE_NONE);
    double deadline = snx_ctr_clock_seconds() + 3600;
    CHECK(snx_ctr_clock_wait(deadline) == 0 && snx_ctr_clock_seconds() >= deadline &&
          near(snx_ctr_clock_seconds(), deadline));
    CHECK(snx_ctr_clock_rate() == 1);
    /* F4: from none back to the pace before it, then off again */
    snx_ctr_clock_set_pace(30);
    CHECK(snx_ctr_clock_toggle() == SNX_CTR_PACE_NONE && snx_ctr_clock_toggle() == 30);
    CHECK(near(snx_ctr_clock_rate(), 0.5));
    /* back to the console's pace: the clock goes on from where it stood, never back */
    before = snx_ctr_clock_seconds();
    snx_ctr_clock_set_pace(SNX_CTR_PACE_CONSOLE);
    CHECK(snx_ctr_clock_seconds() >= before && near(snx_ctr_clock_seconds(), before));

    /* fps_limit's text */
    parse("game", 120, true, SNX_CTR_PACE_CONSOLE, "");
    parse(" GaMe ", 120, true, SNX_CTR_PACE_CONSOLE, "");
    parse("0", SNX_CTR_PACE_CONSOLE, true, SNX_CTR_PACE_NONE, "");
    parse("120", SNX_CTR_PACE_CONSOLE, true, 120, "");
    parse("5", SNX_CTR_PACE_CONSOLE, true, 10,
          "fps_limit=5 is not game, 0 or a whole number of frames a second from 10 to 1000: it takes 10");
    parse("2000", SNX_CTR_PACE_CONSOLE, true, 1000,
          "fps_limit=2000 is not game, 0 or a whole number of frames a second from 10 to 1000: it takes 1000");
    parse("0.5", SNX_CTR_PACE_CONSOLE, true, SNX_CTR_PACE_NONE,
          "fps_limit=0.5 is not game, 0 or a whole number of frames a second from 10 to 1000: it takes 0");
    parse("1", 120, true, SNX_CTR_PACE_CONSOLE,
          "fps_limit=1 is not game, 0 or a whole number of frames a second from 10 to 1000: it takes game");
    parse("30fps", SNX_CTR_PACE_CONSOLE, true, 30,
          "fps_limit=30fps is not game, 0 or a whole number of frames a second from 10 to 1000: it takes 30");
    parse("fast", 120, false, 120, "fps_limit=fast is not game or a number of frames a second: it keeps 120");
    char text[16];
    snx_ctr_clock_text(SNX_CTR_PACE_NONE, text, sizeof text);
    CHECK(!strcmp(text, "0"));
    printf("3ds clock: %d checks\n", checks);
    return 0;
}
