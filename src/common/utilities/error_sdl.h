/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The error's panel (error_panel.h) in an SDL window: its keys, a pad's buttons, fingers and the mouse, and its
   rectangles drawn by SDL's renderer, no texture and no GPU of the game's. The window is the game's once its renderer let
   go of it, or one of the panel's own: in the panel's own process (scarletnx --error-screen), and for a console whose
   window is gone. Share Log, Copy Text, Open Log and Open Folder are done here, the panel staying and saying what each
   did; the rest end it, for the caller to do */
#ifndef SNX_ERROR_SDL_H
#define SNX_ERROR_SDL_H
#include <SDL3/SDL.h>
#include "common/utilities/error_panel.h"

typedef struct {
    /* the system's share of the file at path (Android's chooser, iOS's share sheet): 1 when it opened. NULL where there
       is none: the panel says where the file is */
    int (*share)(const char* path);
    /* where what the panel did goes, a line without its newline (the game's log); NULL: stderr */
    void (*say)(const char* line);
    int back_key; /* the system has a Back key (Android's), which the foot names */
} SnxErrorSdlHooks;

/* The panel in window until an action ends it: CLOSE, TRY_AGAIN or one of the launcher's (Open Settings, GPU Drivers,
   Updates and DLCs, Recompile, Locate..., Install Keys). The window's close is CLOSE. On the thread SDL's video started
   on, with SDL_INIT_GAMEPAD for the pads. Its renderer: software where the video driver gives a window its own pixels
   (X11's, Windows'), which needs no GPU, else SDL's choice; -1, why in why, when none can be made */
int snx_error_sdl_run(SDL_Window* window, SnxError* e, const SnxErrorSdlHooks* hooks, char* why, size_t why_size);
/* the same in a window of its own, made and destroyed here (SDL's video and pads started when they are not) */
int snx_error_sdl_window(SnxError* e, const SnxErrorSdlHooks* hooks, char* why, size_t why_size);

/* scarletnx --error-screen=RECORD: the panel's own process, the error a record (snx_error_write) names. Its status is
   SNX_ERROR_EXIT_BASE + the action taken (no driver exits with those), or 2 for a record it cannot read or a window
   it cannot make, said on stderr */
#define SNX_ERROR_SCREEN    "--error-screen="
#define SNX_ERROR_EXIT_BASE 100
int snx_error_screen_main(int argc, char** argv);
#endif
