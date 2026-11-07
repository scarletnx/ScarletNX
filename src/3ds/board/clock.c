/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/board/clock.h"
#include "common/utilities/common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* the clock's last change of pace: the host's seconds then (host) and the clock's (at); and the pace before none, for
   F4 (previous). All zero at start: the host's clock at the console's pace */
static struct {
    double host, at;
    int pace, previous;
} g_clock;

static double rate_of(int pace) { return pace > 0 ? pace / 60.0 : 1.0; }
double snx_ctr_clock_seconds(void) { return g_clock.at + (snx_seconds() - g_clock.host) * rate_of(g_clock.pace); }
double snx_ctr_clock_rate(void) { return rate_of(g_clock.pace); }
int snx_ctr_clock_pace(void) { return g_clock.pace; }
void snx_ctr_clock_set_pace(int pace) {
    double host = snx_seconds();
    g_clock.at += (host - g_clock.host) * rate_of(g_clock.pace);
    g_clock.host = host;
    g_clock.pace = pace;
}
int snx_ctr_clock_toggle(void) {
    if (g_clock.pace != SNX_CTR_PACE_NONE) {
        g_clock.previous = g_clock.pace;
        snx_ctr_clock_set_pace(SNX_CTR_PACE_NONE);
    } else
        snx_ctr_clock_set_pace(g_clock.previous);
    return g_clock.pace;
}
double snx_ctr_clock_wait(double deadline) {
    double host = snx_seconds(), now = g_clock.at + (host - g_clock.host) * rate_of(g_clock.pace);
    if (deadline <= now) return 0;
    if (g_clock.pace == SNX_CTR_PACE_NONE) {
        /* the clock is the deadline at this host second: a read after it, even of the same host second (a coarse
           timer's), gives the deadline or more, where adding the gap to the clock could round under it */
        g_clock.at = deadline;
        g_clock.host = host;
        return 0;
    }
    return (deadline - now) / rate_of(g_clock.pace);
}
void snx_ctr_clock_text(int pace, char* out, size_t size) {
    if (pace == SNX_CTR_PACE_CONSOLE)
        snprintf(out, size, "game");
    else
        snprintf(out, size, "%d", pace == SNX_CTR_PACE_NONE ? 0 : pace);
}
bool snx_ctr_clock_parse(const char* text, int* pace, char* line, size_t line_size) {
    const char* v = text;
    char* end;
    char taken[16];
    if (line_size) line[0] = 0;
    while (*v == ' ' || *v == '\t') v++;
    if ((v[0] | 0x20) == 'g' && (v[1] | 0x20) == 'a' && (v[2] | 0x20) == 'm' && (v[3] | 0x20) == 'e' &&
        !v[4 + strspn(v + 4, " \t")]) {
        *pace = SNX_CTR_PACE_CONSOLE;
        return true;
    }
    double d = strtod(v, &end);
    if (end == v || d != d) {
        snx_ctr_clock_text(*pace, taken, sizeof taken);
        snprintf(line, line_size, "fps_limit=%s is not game or a number of frames a second: it keeps %s", text, taken);
        return false;
    }
    int value = d < 0 || (d >= 1 && d < 2) ? SNX_CTR_PACE_CONSOLE
                : d < 1                    ? SNX_CTR_PACE_NONE
                : d < 10                   ? 10
                : d > 1000                 ? 1000
                                           : (int)d;
    *pace = value;
    /* given as it is taken: "0", or a whole number from 10 to 1000 with nothing after it */
    bool exact = !end[strspn(end, " \t")] && (value == SNX_CTR_PACE_NONE ? d == 0 : value > 0 && d == value);
    if (!exact) {
        snx_ctr_clock_text(value, taken, sizeof taken);
        snprintf(line, line_size,
                 "fps_limit=%s is not game, 0 or a whole number of frames a second from 10 to 1000: it takes %s", text,
                 taken);
    }
    return true;
}
