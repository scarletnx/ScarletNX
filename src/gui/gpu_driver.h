/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The launcher's side of the GPU driver packages (common/utilities/gpu_driver.h): a package's zip added as a folder of
   the drivers folder, the folders listed and one removed (Settings > App > GPU Drivers); the option each run of a game
   is passed, the game's own choice or the launcher's; and Freedreno Settings' presets, Eden's. No SDL */
#ifndef SNX_GUI_GPU_DRIVER_H
#define SNX_GUI_GPU_DRIVER_H
#include "common/utilities/gpu_driver.h"
#include <stddef.h>

/* the most a package unpacks to: a Qualcomm one is about 40 MB, Turnip about 20 */
#define GUI_GPU_DRIVER_MAX (512ull << 20)
/* an added package: its folder's name, its meta.json, and why that does not read ("" when it does) */
typedef struct {
    char id[100];
    SnxGpuDriverMeta meta;
    char error[400];
} GuiGpuDriver;
enum { GUI_DRIVER_REFUSED, GUI_DRIVER_ADDED, GUI_DRIVER_THERE };
/* the zip of size bytes, named file (its path, or its name), added to dir as a folder named after it
   (snx_gpu_driver_id): its meta.json read and checked, its files at its top, its library arm64's, its minApi at most
   api (0: not checked, away from Android). GUI_DRIVER_ADDED, GUI_DRIVER_THERE when that folder holds the same package
   already, else GUI_DRIVER_REFUSED with nothing written; the folder's name in id, what was done in said */
int gui_gpu_driver_add(const void* zip, size_t size, const char* file, const char* dir, int api, char* id,
                       size_t id_size, char* said, size_t said_size);
/* the packages of dir, by their names: owned, count of them in *count (0 and NULL for none, or no folder). A folder
   whose meta.json does not read is listed with why in its error, so it can be removed */
GuiGpuDriver* gui_gpu_drivers_list(const char* dir, size_t* count);
/* the folder id of dir taken away: 1, else 0; said says either */
int gui_gpu_driver_remove(const char* dir, const char* id, char* said, size_t size);
/* what a game's run is passed for key: nothing (NULL) when the game's .ini at game_ini has a key= line of its own,
   which the run reads, or when the launcher's global is empty; else --key=global. Owned */
char* gui_run_option(const char* key, const char* game_ini, const char* global);

/* Freedreno Settings' presets, as Eden has them: a name, what it does, and the variables it sets in the place of the
   ones set (freedreno_env's text) */
typedef struct {
    const char* name;
    const char* about;
    const char* variables;
} GuiFreedrenoPreset;
extern const GuiFreedrenoPreset gui_freedreno_presets[]; /* a NULL name ends it */
#endif
