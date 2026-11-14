/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A title's process tells macOS what the work is, so that it runs the way a game does. App Nap throttles an app whose
   window is not frontmost and coalesces its timers, which stretches a title's own threads while it plays (a title
   measured between 50 and 91 frames a second as the window took and lost that state). Game Mode is the system's own
   for a fullscreen app and needs nothing here; this is the part a process asks for itself.

   Taken when a title begins presenting and given back when it stops, so a menu that sits idle costs the machine
   nothing. plat_apple.m on Apple platforms, nothing everywhere else. */
#ifndef SNX_PLAT_APPLE_H
#define SNX_PLAT_APPLE_H
#if defined(__APPLE__)
void plat_apple_title_begin(void);
void plat_apple_title_end(void);
#else /* every other host has no App Nap, and nothing to ask it with: the calls compile to nothing */
static inline void plat_apple_title_begin(void) {}
static inline void plat_apple_title_end(void) {}
#endif
#endif
