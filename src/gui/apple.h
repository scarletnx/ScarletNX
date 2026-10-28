/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's side of the iOS and macOS apps (docs/APPLE.md): apple.m, which calls the Swift in dist/apple */
#ifndef SNX_GUI_APPLE_H
#define SNX_GUI_APPLE_H
#include <stddef.h>
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_video.h>

/* iOS: from here on stdout and stderr go to the day's launcher log, logs/scarletnx-console-YYYY-MM-DD.log in the app's
   Documents (gui/log.h), which the Files app shows, after a banner naming the device, the system and the limits it puts
   on the app (a phone shows no console) */
void gui_apple_log_start(void);
/* iOS: one line of what the app uses now (memory it is charged for and may still take, CPU time, thermal state, power
   mode), for the log every few seconds of a run */
void gui_apple_stats(char* out, size_t size);
/* iOS: the working folder is the app's Documents (the data folder, which the Files app shows), with its games/ */
void gui_apple_start(void);
/* iOS: the system's document picker, as SDL has none there: a file, or a folder when folder is set. done gets its path
   or an empty list when none was picked, as an SDL dialog calls back (from the app's main thread); 0 with message when
   it cannot open */
int gui_apple_pick(SDL_Window* window, int folder, SDL_DialogFileCallback done, void* data, char* message,
                   size_t size);
/* A file to the user: iOS's share sheet, the Finder showing it on macOS; 0 with message when it cannot */
int gui_apple_share(SDL_Window* window, const char* path, char* message, size_t size);
/* iOS: the runtime's arguments (argv[0] the first, NULL-terminated) for the game the menu plays once it has closed,
   in this process, which is the only one an iOS app has */
void gui_apple_play(const char* const* argv);
/* What gui_apple_play kept, NULL when no game was picked; argc gets the count. Owned by apple.m, which keeps it until
   the process ends, as the runtime does with its argv */
char** gui_apple_played(int* argc);
#endif
