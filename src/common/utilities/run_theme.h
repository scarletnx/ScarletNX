/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's look for a loading screen, as the runtime draws one of its own (the shaders it prepares before the
   game starts, nx/runtime/platform/prepare_screen.c): the [run] part of the theme the launcher's Settings > App picked
   (src/gui/theme.c fills it, docs/user/gui.md describes the keys) */
#ifndef SNX_RUN_THEME_H
#define SNX_RUN_THEME_H
#include <stdint.h>

/* how a step's progress reads, as a theme's [run] progress says (src/gui/steps.h's GuiProgress, the same numbers) */
enum { SNX_RUN_PROGRESS_AMOUNT, SNX_RUN_PROGRESS_PERCENT, SNX_RUN_PROGRESS_TIME };
/* where the turning ring goes, as a theme's [run] ring says (src/gui/theme.h's GuiRing, the same numbers): none, left of
   the title, at the right edge, or after the name */
enum { SNX_RUN_RING_NONE, SNX_RUN_RING_LEFT, SNX_RUN_RING_RIGHT, SNX_RUN_RING_NEAR_TEXT };
typedef struct {
    uint32_t page, ink, soft, dim; /* 0xRRGGBB. All zero (ink as page) when the launcher couldn't say */
    char name[64];                 /* the theme's, as its [theme] name says */
    char title[64];                /* [head] loading: the loading screen's title word */
    int steps;                     /* layout = steps: labelled steps; console: the lines as printed */
    int texture;                   /* faint lines over the page */
    int progress;                  /* a step's progress: SNX_RUN_PROGRESS_* */
    int ring;                      /* SNX_RUN_RING_* */
    int text_size; /* app.ini's [app] ui_scale, the launcher's letters as 8x16 pixels times it (1 to 4); 0 when it
                      has no number, which the launcher takes as twice the display's scale (its Auto) */
    /* [transitions] fade_in_ms and fade_out_ms: how long what comes in from black and what goes out to it fade, 0 for
       no fade. The game's notification takes them (overlay.c's overlay_fades) */
    uint32_t fade_in_ms, fade_out_ms;
} SnxRunTheme;

/* How far a fade of ms milliseconds begun at from is at now (both in ms), 0 to 1: one curve for the launcher's fades
   from black and to it (src/gui/main.c's overlay) and the game's notification coming and going (overlay.c). 1 when ms
   is 0, the fade turned off */
static inline float snx_fade_done(uint64_t now, uint64_t from, uint32_t ms) {
    const uint64_t gone = now > from ? now - from : 0;
    return !ms || gone >= ms ? 1.0f : (float)gone / (float)ms;
}
/* The launcher's black over its screen at a fade's point black, 0 (none) to 1 (all of it): the alpha its shade takes
   (src/gui/main.c's overlay). What it leaves of the screen, 255 less it, is what the game's notification shows of
   itself at the same point (overlay.c's overlay_toast_alpha) */
static inline uint8_t snx_fade_shade(float black) { return (uint8_t)(black * 255); }

/* The seconds a step has left at its pace so far, as the launcher's time left reads it (steps.c gui_step_progress):
   spent_ms on done of total, rounded up. -1 before a second of it or before any of it is done */
static inline int64_t snx_run_seconds_left(uint64_t spent_ms, uint64_t done, uint64_t total) {
    if (!done || spent_ms < 1000 || done > total) return -1;
    return (int64_t)((spent_ms * (total - done) / done + 999) / 1000);
}
#endif
