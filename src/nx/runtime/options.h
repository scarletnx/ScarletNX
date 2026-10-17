/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the settings from <Game>.ini (options.c) */
#ifndef SNX_OPTIONS_H
#define SNX_OPTIONS_H
#include <stddef.h>

#include "nx/runtime/startup.h"

extern int g_opt_fullscreen; /* fullscreen: the .ini's, or the command line's for this run (startup.h) */

void ini_override(const char* k, const char* v); /* a --key=value from the command line, before ini_open. Keeps v */
void ini_set(const char* k, const char* v);
/* every setting of the file's sections but the game's own (a control's place) on stdout, a line each: section (with
   /page for a page of its own), key, default, int | float | text, the lowest and highest number, the help on one line,
   the menus' name, and the values value=word between '|' (none: typed), tabs between (scarletnx --settings, the
   launcher's Settings) */
void ini_list(void);
/* 1 when key is a setting of the file, or an unattended-run option (platform/unattended.h): the driver passes only
   those to a game (driver.h) */
int snx_setting_known(const char* key);
void ini_open(const char* path); /* watch the file: ini_poll reloads it when it changes, and writes back what changed */
/* final: at exit. An edit that hasn't settled is left for the next start, unless a toggle needs saving: then it is
   read as it is */
void ini_poll(int final);
extern int g_opt_docked, g_opt_lang;
/* fps_limit: 0 = no limit, SNX_FPS_GAME = the game's own pace (its swap interval), else frames a second (10 to 1000) */
#define SNX_FPS_GAME (-1)
extern int g_opt_fps;
void fps_limit_text(int fps, char* out, size_t n); /* g_opt_fps as fps_limit's value: "game", "0", "30"... */
extern int g_opt_diag, g_opt_shadows, g_opt_prof;
/* auto_stub_ipc: 1 = a service command the runtime doesn't declare answers success with no outputs, 0 = the
   console's UnknownCommandId (hle/kernel/ipc.c's stub_ok, sm's stand-in for a service there isn't) */
extern int g_opt_auto_stub_ipc;
extern int g_opt_network;     /* network: 1 = the game is online through the host (hle/services/network) */
extern int g_opt_jit_threads; /* jit_threads: the runtime JIT's compile threads, 0 by the CPUs. Startup only */
extern float g_opt_resf;
extern int g_opt_vsync; /* vsync: 1 = presents wait for the display's refresh (FIFO) */
extern int g_opt_aniso; /* anisotropy: 0 = the game's, else at least that much for mipmapped 2D textures */

#endif
