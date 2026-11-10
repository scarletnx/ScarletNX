/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/runtime/platform/help.h"
#include <stdio.h>

/* It follows keymap (the game's buttons), hotkey and menu_event (the runtime's keys) in backend/sdl3.c: a binding
   changed there changes here */
static const char* const HELP_KEYS =
    "Controller: A, B, X and Y sit where the console has them (gamepad_swap_ab=1 swaps A and B). "
    "Home opens the game's menu.\n\n"
    "Keyboard, by key position on a US layout:\n"
    "  Buttons      A: X or Space    B: Z    X: C    Y: V\n"
    "  Shoulders    L: Q    R: E    ZL: 1    ZR: 3\n"
    "  Plus, minus  +: Enter    -: Backspace\n"
    "  D-Pad        the arrow keys\n"
    "  Left stick   W, A, S and D\n"
    "  Right stick  I, J, K and L\n\n"
    "Runtime keys:\n"
    "  F1  this help\n"
    "  F2  open or close the game's menu (arrows move, Enter or Space picks, Esc or Backspace goes back)\n"
    "  F3  take a screenshot\n"
    "  F4  turn the FPS limit off or back on\n"
    "  F5  write every thread's state to the log\n"
    "  F6  turn showcase mode on or off\n"
    "  Ctrl+D  cycle the TV mode: On, Off and Auto\n"
    "  F7  save the game's state to the selected slot\n"
    "  F8  load the selected slot's state\n"
    "  F9  select the next save state slot\n"
#if defined(SNX_TRACY)
    "  F10 record a 60 second Tracy capture\n"
#endif
    "  Pause  pause the whole game, and resume it\n"
    "  F11 or Alt+Enter  switch fullscreen\n"
    "  F12  turn the stats on or off\n";

int help_text(char* out, size_t size, int capture, const char* debugger) {
    int has = debugger && *debugger;
    return snprintf(out, size, "%s%s  Esc twice  quit the game%s%s", HELP_KEYS,
                    capture ? "  Ctrl+Shift+F12  capture the next frame with RenderDoc\n" : "",
                    has ? "\n\nDebugger attached: " : "", has ? debugger : "");
}
