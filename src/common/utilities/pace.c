/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/pace.h"
#include "common/utilities/key_said.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void snx_pace_text(int pace, char* out, size_t size) {
    if (pace == SNX_PACE_CONSOLE)
        snprintf(out, size, "game");
    else
        snprintf(out, size, "%d", pace == SNX_PACE_NONE ? 0 : pace);
}
bool snx_pace_parse(const char* text, int* pace, char* line, size_t line_size) {
    const char* v = text;
    char* end;
    char taken[16];
    if (line_size) line[0] = 0;
    while (*v == ' ' || *v == '\t') v++;
    if ((v[0] | 0x20) == 'g' && (v[1] | 0x20) == 'a' && (v[2] | 0x20) == 'm' && (v[3] | 0x20) == 'e' &&
        !v[4 + strspn(v + 4, " \t")]) {
        *pace = SNX_PACE_CONSOLE;
        return true;
    }
    double d = strtod(v, &end);
    if (end == v || d != d) {
        snx_pace_text(*pace, taken, sizeof taken);
        snprintf(line, line_size, "fps_limit=%s is not game or a number of frames a second: it keeps %s", text, taken);
        return false;
    }
    int value = d < 0 || (d >= 1 && d < 2) ? SNX_PACE_CONSOLE
                : d < 1                    ? SNX_PACE_NONE
                : d < 10                   ? 10
                : d > 1000                 ? 1000
                                           : (int)d;
    *pace = value;
    /* given as it is taken: "0", or a whole number from 10 to 1000 with nothing after it */
    bool exact = !end[strspn(end, " \t")] && (value == SNX_PACE_NONE ? d == 0 : value > 0 && d == value);
    if (!exact) {
        snx_pace_text(value, taken, sizeof taken);
        snprintf(line, line_size,
                 "fps_limit=%s is not game, 0 or a whole number of frames a second from 10 to 1000: it takes %s", text,
                 taken);
    }
    return true;
}
void snx_pace_toggle(int* pace, int* previous, char* line, size_t size) {
    char now[16], was[16];
    snx_pace_text(*pace, was, sizeof was);
    if (*pace != SNX_PACE_NONE) {
        *previous = *pace;
        *pace = SNX_PACE_NONE;
    } else
        *pace = *previous;
    snx_pace_text(*pace, now, sizeof now);
    if (size) snx_fps_limit_said(line, size, now, was);
}
