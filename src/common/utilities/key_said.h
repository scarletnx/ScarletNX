/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* What a key that changes a value says, over the game and in the log, in every console's window (the Switch's FPS
   limit, showcase, stats and pause keys, the 3DS' F4): the value in effect now and the one before it, so the player
   sees what the press did. The Switch's notification says the value now alone (snx_key_now), the least that tells the
   press, its log line both */
#ifndef SNX_KEY_SAID_H
#define SNX_KEY_SAID_H
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* "what: now (was was)" into out (n bytes) */
static inline void snx_key_said(char* out, size_t n, const char* what, const char* now, const char* was) {
    snprintf(out, n, "%s: %s (was %s)", what, now, was);
}
/* "what: now" into out (n bytes) */
static inline void snx_key_now(char* out, size_t n, const char* what, const char* now) {
    snprintf(out, n, "%s: %s", what, now);
}
/* fps_limit's value as it is written ("game", "0" or the frames a second) in words */
static inline const char* snx_fps_limit_words(const char* v) {
    return !strcmp(v, "game") ? "the game's pace" : !strcmp(v, "0") ? "off" : v;
}
/* the FPS limit key's line, now and was as fps_limit writes them: "FPS limit: off (was the game's pace)" */
static inline void snx_fps_limit_said(char* out, size_t n, const char* now, const char* was) {
    snx_key_said(out, n, "FPS limit", snx_fps_limit_words(now), snx_fps_limit_words(was));
}
/* the same without was: "FPS limit: off" */
static inline void snx_fps_limit_now(char* out, size_t n, const char* now) {
    snx_key_now(out, n, "FPS limit", snx_fps_limit_words(now));
}
#endif
