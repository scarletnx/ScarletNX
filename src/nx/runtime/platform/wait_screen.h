/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The screens the player waits on while the runtime does something long that the inputs take no part in, each only
   past a threshold, so a quick one doesn't flash: the game closing while the shaders and pipelines it made are saved
   (main.c's save at exit, drawn as the launcher's loading screen by prepare_screen.c), and a save state loading
   (state.c, a box over the game greyed out, overlay.h's overlay_loading). What they decide, apart from the window
   and the renderer, which their callers ask */
#ifndef SNX_WAIT_SCREEN_H
#define SNX_WAIT_SCREEN_H
#include "nx/runtime/platform/prepare_screen.h"
#include <stddef.h>

#define SNX_WAIT_CLOSING_MS 500 /* the save at exit's screen, from this long into it */
#define SNX_WAIT_LOADING_MS 300 /* a save state's box, from this long into its load */

/* The save as the game closes, from began_ms (a monotonic clock's): shown once its screen is up, stopped_ms when a
   close stopped it (0 while none did) */
typedef struct {
    u64 began_ms, stopped_ms;
    int shown;
} SnxClosing;
/* A pass of its wait at now_ms, with closes the closes asked since the last pass (the window's, Alt+F4, SIGTERM) and
   the window seen or not: the screen shows from SNX_WAIT_CLOSING_MS on while the window is seen (w->shown); 1 when the
   save is to stop now, at the first close, 0 otherwise */
int snx_closing_pass(SnxClosing* w, u64 now_ms, int closes, int window_shown);
/* The screen's row for a save of total bytes with packed of them packed: SAVING SHADERS, "96.0 of 158.7 MiB", its
   value and total in KiB (SnxPrepareStep's kib), from began_ms */
void snx_closing_step(const SnxClosing* w, u64 total, u64 packed, SnxPrepareStep* st);
/* The log's line for a save that a close stopped, once it is over, kept of its total bytes written: "a second close
   stopped the shader cache's save 2.4 s in: 96.0 of 158.7 MiB of this session's shaders not saved" */
void snx_closing_said(const SnxClosing* w, u64 total, u64 kept, char* out, size_t size);

/* whether a save state's load begun at began_ms shows its box at now_ms (both ms, a monotonic clock's) */
static inline int snx_loading_shows(u64 began_ms, u64 now_ms) {
    return now_ms >= began_ms && now_ms - began_ms >= SNX_WAIT_LOADING_MS;
}
#endif
