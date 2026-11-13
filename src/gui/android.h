/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's side of the Android app (docs/hosts/android-app.md) */
#ifndef SNX_GUI_ANDROID_H
#define SNX_GUI_ANDROID_H
#include <stddef.h>
#include <SDL3/SDL_dialog.h>

/* The app's own files as HOME, for the keys (.switch/), and the working folder until the data folder is gone into */
void gui_android_start(void);
/* Changes to the data folder, made with its games/: a .scarletnx/ (SNX_PORTABLE_DIR) in the app's own files, which
   every process of the app divides by kind (snx_app_dir); NULL when it cannot be. The user sees it through the Files
   app (DataProvider) */
const char* gui_android_own_folder(void);
/* The file under a picked document without a path (Downloads' own list, Recent): what its descriptor opens, when that
   is a file on the device's storage; NULL else. Owned */
char* gui_android_document_file(const char* uri);
/* The system's folder picker, as SDL has none on Android: done gets the folder's path, or the tree the picker gave
   when it is no folder of the device's storage, or an empty list when none was picked, as an SDL dialog calls back
   (from the app's main thread); 0 with message when it cannot open */
int gui_android_pick_folder(SDL_DialogFileCallback done, void* data, char* message, size_t size);
/* The folder of the app's native libraries, where the driver is (libscarletnx-run.so); NULL when unknown */
const char* gui_android_libraries(void);
/* Runs the runtime with argv in the app's game activity, a process of its own; 0 with message when it cannot */
int gui_android_play(const char* const* argv, char* message, size_t size);
/* The app behind the others, as it is, for Back on the menu's first screen; 0 with message when it cannot */
int gui_android_background(char* message, size_t size);
/* Opens a folder of the data folder or of the shared storage in the system's Files app; 0 with why in message when it
   cannot (one elsewhere, which no app's provider shows) */
int gui_android_open_folder(const char* path, char* message, size_t size);
/* A run's game, the step it shows and its percent (-1 while unknown) in a notification, which keeps the app running
   behind others; the first call of a run starts it */
void gui_android_running(const char* game, const char* step, int percent);
/* The run is over: its notification goes, and one saying result stays when that is not NULL */
void gui_android_ran(const char* result);
/* What Android says of the end of the app's process pid, started then (seconds since 1970, 0 when unknown), for a
   start's mark (common/utilities/run_mark.h): MainActivity's exitReason, ApplicationExitInfo from Android 11, else
   that the system says none. Owned */
char* gui_android_exit_reason(long long pid, long long started);
/* The same end as fields, a "key=value" line each (MainActivity.exitFields), for the menu's Last Run (gui/last_run.h):
   a line none= saying why there is none when Android gives none. Owned */
char* gui_android_exit_fields(long long pid, long long started);
/* The launcher log's state: line now and at each change of the thermal status (Android 10) or of the battery saver,
   until the process ends, as gui_apple_state_watch's: 1, or 0 with why in message */
int gui_android_state_watch(char* message, size_t size);
#endif
