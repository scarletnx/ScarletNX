/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "3ds/board/clock.h"
#include "common/utilities/common.h"
#include "common/utilities/pace.h"

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
int snx_ctr_clock_toggle(char* line, size_t size) {
    int pace = g_clock.pace;
    snx_pace_toggle(&pace, &g_clock.previous, line, size);
    snx_ctr_clock_set_pace(pace);
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
