/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The runtime's startup and the state it sets up for the whole run (main.c), the crash report (crash.c), and showing
   vi's buffers at the game's pace (present.c) */
#ifndef SNX_MAIN_H
#define SNX_MAIN_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/recompiler/variants.h"
#include "common/utilities/run_theme.h"

int snx_main(int argc, char** argv); /* runs the game the arguments or the program's name say; its exit status */
/* The AOT module's runtime code variants, from its snx_variants() (main.c's load_module); NULL for a module built
   without a profile cache */
extern const SnxVariants* g_variants;
/* the launcher's menu (src/gui), for an executable built with it: a bare start of the runtime opens it, with the
   program's own path */
extern int (*snx_menu)(const char* self);
/* the launcher's loading look (gui/theme.h's gui_run_theme), for the runtime's own loading screen: NULL in a runtime
   built without the menu, which keeps the YoRHa theme's built in. Android's is one (src/nx/CMakeLists.txt: the
   launcher is the app's own library there), so its screen is YoRHa's whatever theme the launcher shows */
extern void (*snx_menu_run_theme)(const char* folder, SnxRunTheme* out);
/* Saves what outlives the run (the hints, the renderer's pipeline cache) and reports nvn_trace before a clean exit:
   the main loop's, the unattended limit's, svcExitProcess. The first caller saves; one that comes while it does waits,
   then does nothing */
void snx_save_before_exit(void);
/* The game's folders, each ending in a separator (g_exedir may be empty), and its name: <g_exedir><g_exename>_code.so
   is its code, <g_exedir><g_exename>_data/ (g_datadir) its files */
extern char g_datadir[1024], g_exename[256], g_exedir[1024];
/* the update of the game in its folder this program is: its title ID (0 for none, the game's own program) and its
   data folder, <g_exedir><g_exename>_data/, which lies over the game's. g_gamename is the game's name, g_exename's
   or the one the update is of, and g_datadir its data folder */
extern u64 g_addon;
extern char g_addondir[1024], g_gamename[256];
/* where its settings (config), its log and threads.txt (state) and its caches go: g_exedir, or for a game in an
   installed build's data folder the XDG folders of each kind */
extern char g_confdir[1024], g_statedir[1024], g_cachedir[1024];
extern volatile int g_quit; /* set to leave the main loop, which saves and exits */
extern int g_frames;        /* the frames shown so far, atomic: the presenting thread counts them */
/* the crash handler's report (plat_posix.c, plat_win32.c): the faulting host pc, the address it touched (NULL when not
   a memory fault) and the signal, or the Windows exception code. Ends the process */
void crash_report(void* host_pc, void* fault, int sig);
/* one of vi's queued buffers on the display, once the GPU has drawn it (the GPU thread): nvmap_id's memory from offset,
   its layout, format and size, the crop rectangle (left, top, right, bottom; all 0 for the whole buffer) and the
   transform's flips (bit 0 horizontal, bit 1 vertical); slot is vi's, which present_pace was given at its dequeue */
void vi_show_buffer(u32 nvmap_id, u32 offset, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const i32 crop[4],
                    u32 transform, int slot);
/* the wait before vi hands the game a buffer for its next frame, as fps_limit says: with fps_limit=game until the
   frame before has been on the display for interval refreshes (its swap interval). slot: the buffer's (vi's) */
void present_pace(u32 interval, int slot);
/* A frame period (ns) on the display's clock: the nearest whole number of the display's refreshes (refresh, 0 unknown)
   where that is within 1/32 of it, so a pace kept to it doesn't drift from the display (a 59.94 Hz one), else period */
u64 present_display_period(u64 period, u64 refresh);

#endif
