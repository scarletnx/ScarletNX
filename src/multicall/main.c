/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* scarletnx's main, the program's only one: every driver the build has is a library linked into it, run by the name
   the program was started under (scarletnx-3ds, scarletnx-vita, scarletnx-gnuabi, scarletnx-agb, scarletnx-android,
   scarletnx-switch: links beside it on Linux and macOS), or by that name as its first argument (scarletnx
   scarletnx-3ds ..., how the launcher runs them on Windows, which has no links). A game's copy runs its driver too:
   a GBA game's, whose .ini says system=gba, and a 3DS or Vita game's, named after its title ID. Any other name is
   the Switch driver, which takes a game's copy's name for the game it runs, or in a build without it the one driver
   there is. One program carries LLVM, SDL and the code they share once */
#ifdef SNX_MULTICALL_SWITCH
#include "nx/runtime/entry.h"
#endif
#ifdef SNX_MULTICALL_3DS
#include "3ds/runtime/main.h"
#endif
#ifdef SNX_MULTICALL_GNUABI
#include "gnuabi/runtime/main.h"
#endif
#ifdef SNX_MULTICALL_VITA
#include "vita/runtime/main.h"
#endif
#ifdef SNX_MULTICALL_AGB
#include "agb/runtime/main.h"
#endif
#ifdef SNX_MULTICALL_NDS
#include "nds/runtime/main.h"
#endif
#ifdef SNX_MULTICALL_ANDROID
#include "android/runtime/main.h"
#endif
#if defined(SNX_MULTICALL_GUI) && defined(_WIN32)
#include "gui/gui.h"
#endif
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
/* UIKit's app is SDL's: this makes the main below SDL_main, run from the real main SDL provides, which starts the app
   (UIApplicationMain) whose run loop the menu's window needs. Without it SDL_Init fails with "Application didn't
   initialize properly" and the screen stays black */
#include <SDL3/SDL_main.h>
#endif
#endif

typedef struct {
    const char* name;                 /* its program's name, the link's */
    int (*main)(int argc, char** argv);
    bool (*copy)(const char* argv0); /* whether the program started as argv0 is a copy of one of its games */
} Driver;

/* first the GBA's, as cmake/deploy.cmake reads a game's .ini before its name: a GBA game is named after its ROM, any
   name. The Switch's last, every other name's */
static const Driver drivers[] = {
#ifdef SNX_MULTICALL_AGB
    {"scarletnx-agb", snx_agb_main, snx_agb_game_name},
#endif
#ifdef SNX_MULTICALL_NDS
    {"scarletnx-nds", snx_nds_main, snx_nds_game_name},
#endif
#ifdef SNX_MULTICALL_3DS
    {"scarletnx-3ds", snx_ctr_main, snx_ctr_title_name},
#endif
#ifdef SNX_MULTICALL_VITA
    {"scarletnx-vita", snx_vita_main, snx_vita_title_name},
#endif
#ifdef SNX_MULTICALL_GNUABI
    {"scarletnx-gnuabi", snx_gnuabi_main, NULL},
#endif
#ifdef SNX_MULTICALL_ANDROID
    {"scarletnx-android", snx_android_main, NULL},
#endif
#ifdef SNX_MULTICALL_SWITCH
    {"scarletnx-switch", snx_switch_main, NULL},
#endif
};
#define DRIVERS (sizeof drivers / sizeof *drivers)

/* the driver named name, the last part of a path, ".exe" left out on Windows; NULL for none */
static const Driver* named(const char* path) {
    const char* name = path;
    for (const char* c = path; *c; ++c)
        if (*c == '/' || *c == '\\') name = c + 1;
    size_t n = strlen(name);
#ifdef _WIN32
    if (n > 4 && (!strcmp(name + n - 4, ".exe") || !strcmp(name + n - 4, ".EXE"))) n -= 4;
#endif
    for (size_t i = 0; i < DRIVERS; ++i)
        if (strlen(drivers[i].name) == n && !strncmp(name, drivers[i].name, n)) return &drivers[i];
    return NULL;
}

#if defined(SNX_MULTICALL_GUI) && defined(_WIN32)
/* the menu's snx_menu_has_driver: Windows has no links to the program, so the menu runs a driver as the program itself,
   with the driver's name first */
static int has_driver(const char* name) { return named(name) != NULL; }
#endif

/* Android's game activity finds it by name in libscarletnx.so (nx/runtime/platform/android_run.c does the same) */
__attribute__((visibility("default"))) int main(int argc, char** argv);

int main(int argc, char** argv) {
    const char* self = argc > 0 && argv[0] ? argv[0] : "";
#if defined(SNX_MULTICALL_GUI) && defined(_WIN32)
    snx_menu_has_driver = has_driver;
#endif
    const Driver* driver = argc > 1 ? named(argv[1]) : NULL;
    if (driver) return driver->main(argc - 1, argv + 1); /* its name as its argv[0] */
    if ((driver = named(self))) return driver->main(argc, argv);
    for (size_t i = 0; i < DRIVERS; ++i)
        if (drivers[i].copy && drivers[i].copy(self)) return drivers[i].main(argc, argv);
#ifdef SNX_MULTICALL_SWITCH
    return snx_switch_main(argc, argv);
#else
    if (DRIVERS == 1) return drivers[0].main(argc, argv);
    fputs("scarletnx: start it as one of its drivers, by its name or with its name first:", stderr);
    for (size_t i = 0; i < DRIVERS; ++i) fprintf(stderr, " %s", drivers[i].name);
    fputc('\n', stderr);
    return 2;
#endif
}
