/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_appendf: a line built piece by piece stays inside its buffer, cut there with "...", where
   at += snprintf(line + at, sizeof line - at, ...) writes past it once at passes the size (the recording thread's
   report did, and a race with the GPU profile on stopped on "stack smashing detected") */
#include "common/utilities/common.h"
#include <stdio.h>
#include <string.h>

static int failures;
static void check(int ok, const char* what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        failures++;
    }
}

int main(void) {
    char out[16];
    int at = snx_appendf(out, sizeof out, 0, "%s", "ab");
    at = snx_appendf(out, sizeof out, at, " %d", 42);
    check(at == 5 && !strcmp(out, "ab 42"), "pieces that fit are kept whole");

    /* a line past the buffer, then more pieces: the guard bytes after it stay as they were */
    struct {
        char line[8];
        unsigned char guard[24];
    } s;
    memset(s.guard, 0xA5, sizeof s.guard);
    at = 0;
    for (int i = 0; i < 10; i++) at = snx_appendf(s.line, sizeof s.line, at, "%s piece %d", i ? "," : "", i);
    int guard_kept = 1;
    for (size_t i = 0; i < sizeof s.guard; i++) guard_kept &= s.guard[i] == 0xA5;
    check(guard_kept, "nothing is written past the buffer");
    check(at == 7 && strlen(s.line) == 7, "the length stops at the buffer's last byte");
    if (strcmp(s.line, " pie...")) {
        fprintf(stderr, "a cut line reads \"%s\", not \" pie...\"\n", s.line);
        failures++;
    }
    check(snx_appendf(s.line, sizeof s.line, at, "%s", "more") == 7, "a full buffer takes nothing more");

    char tiny[3];
    at = snx_appendf(tiny, sizeof tiny, 0, "%s", "abcdef");
    check(at == 2 && !strcmp(tiny, "ab"), "a buffer under 4 bytes is cut without the dots");
    check(snx_appendf(tiny, 0, 0, "%s", "x") == 0, "a buffer of 0 bytes takes nothing");

    if (!failures) printf("common_appendf: ok\n");
    return failures != 0;
}
