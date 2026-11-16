/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's settings file, <Game>.ini (options.c): its sections, the command line's overrides, the game's own picks
   beside it (<Game>.picks, game_ini.h's snx_game_picks), reloading both while the game runs and writing back what the
   game's menu changes. A setting takes this run's command line's value, else the game's pick, else the launcher's pick
   for every game (the command line's of a run with --launcher-picks=1), else the .ini's, else its default. Every
   setting's value is in g_settings (settings.h, from settings.txt); what the runtime derives from some of them is
   here */
#ifndef SNX_OPTIONS_H
#define SNX_OPTIONS_H
#include <stddef.h>

#include "nx/runtime/startup.h"
#include "snx_settings.h"

/* --key=value: k's value for this run, over the file's, which keeps its own, and over the game's pick unless the
   run has --launcher-picks=1. Before ini_open; v is kept, not copied */
void ini_override(const char* k, const char* v);
const char* ini_overridden(const char* k); /* k's value for this run, NULL when nothing gave one */
/* deprecated_env.c: the environment variables an older version read for settings the .ini has now
   (docs/user/deprecations.md). ini_open makes each one set this run's value for its setting as ini_override does, unless
   the command line or a variable before it gives that key; then says each, with the value in effect */
void options_old_environment(void);
void options_old_environment_say(void);
/* a value the game's menu set, already applied: saved at the next poll, even over an edit made meanwhile, and played
   over this run's command line from then on. A key the launcher's Settings show goes to the game's picks, another (a
   control's place) to the .ini */
void ini_set(const char* k, const char* v);
/* the same for a value the menu did not apply: the next tick applies it (one read when the game starts at its next
   start), then saves it */
void ini_set_apply(const char* k, const char* v);
/* the game's pick of k taken out, from its menu: at the next tick its value is the one without it (the launcher's for
   every game, else the .ini's, else the default), applied, and the picks file saved without it */
void ini_unpick(const char* k);
/* 1 when the game's picks have k, a change of the menu's not saved yet counted; from any thread */
int ini_picked(const char* k);
/* the value the next start takes for k where it is not this run's, into out (n bytes, cut where it ends): the game's
   pick, a change of the menu's not saved yet counted, else for a setting read at the start whose pick the menu took
   out this run the value without it. 1, else 0 with out empty. From any thread: the menu's row of a setting read at
   the start shows it */
int ini_next_start(const char* k, char* out, size_t n);
/* a count of the changes to the game's picks and to ini_old_settings, which the menu watches to show them again */
const int* ini_picks_changes(void);
/* 1 once after ini_unpick or ini_clean_old: the frontend's loop polls at once (ini_poll) */
int ini_poll_due(void);
/* the keys Clean Old Settings takes out, as the files were last read or written: the .ini's under [Deprecated] and
   the picks' that no pick takes. From any thread */
int ini_old_settings(void);
/* the game's menu's Clean Old Settings: at the next tick those keys are taken out of both files, said in the log */
void ini_clean_old(void);
/* every setting of the file's sections but the game's own (a control's place), a line each on stdout, nine fields
   between tabs: the section (with /page for a page of its own), the key, its default, int | float | text, the lowest
   and highest number, the help on one line, the menus' name, and the values as value=word between '|' (empty for a
   typed one; ini_list_text). scarletnx --settings prints the game's sections' (g_ini_sections) for the launcher's
   Settings, --settings=recompile the launcher's own and the recompiler's (g_launch_sections) */
void ini_list(const IniSection* sections, int count);
/* 1 for a key of the file or an unattended-run option (platform/unattended.h): the driver passes only those on to a
   game (driver.h) */
int snx_setting_known(const char* key);
/* reads path and the game's picks beside it, applies them with the overrides, writes the .ini back with its help, and
   watches both: ini_poll reloads either when it changes and saves what the game's menu set */
void ini_open(const char* path);
/* the Debug section's settings not at their default (ini_changed's flags), then gdbstub when the GDB stub runs, between
   ", ": how many. ini_open and a reload show the list at the top of the picture (hud.h), a reload that changes it in the
   log too */
int options_debug(int flags, char* out, size_t n);
/* the Display, Graphics, Audio and Performance settings not at their default, each "key=value (default D)" between
   ", ": how many. The log's header says them, and a reload that changes them says them again */
int options_settings(char* out, size_t n);
/* memory_fit's value old as memory_fit_mode's (0 Auto, 1 Off): 1 is 0; 0 is 1 where memory_fit's default was 1 (ios),
   else -1, memory_fit_mode's default, as 0 was then memory_fit's own, which every game's .ini had written; -2 for
   any other value */
int options_memory_fit_old(const char* old, int ios);
/* astc_bc's value old as astc_mode's (0 Auto, 1 Exact): 1 is 0; 0 is -1, astc_mode's default, as 0 was astc_bc's own,
   which every game's .ini had written; -2 for any other value */
int options_astc_bc_old(const char* old);
/* vk_descriptors's value old as vk_descriptors_mode's, whose values are the same (0 Sets, 1 Buffers, 2 Heaps, 3
   Automatic): the command line's (given 1) as it is, a choice of that run; the .ini's (given 0) as it is but 0 and 3,
   which are -1, vk_descriptors_mode's default, as 0 was vk_descriptors's own until 3 was, and every game's .ini had
   written its default; -2 for a value that is not a whole number from 0 to 3 */
int options_vk_descriptors_old(const char* old, int given);
/* the path ini_open read, "" before it */
const char* ini_file(void);
/* about once a second, from the main loop. final: at exit, where an edit still being saved is left for the next start
   unless the menu's changes need writing, which is then done over the file as it is */
void ini_poll(int final);
/* fps_limit: 0 = no limit, SNX_FPS_GAME = the game's own pace (its swap interval), else frames a second (10 to 1000) */
#define SNX_FPS_GAME (-1)
extern int g_opt_fps;                              /* atomic: the game's threads read it */
void fps_limit_text(int fps, char* out, size_t n); /* g_opt_fps as fps_limit's value: "game", "0", "30"... */
/* hotkey_fps_limit's, on the frontend's thread: the limit off, *was the one it took off, or back from off to *was (0
   for none yet: the game's pace), written to the .ini. Its line in the log, the limit now and before it, the same words
   in text (size bytes, 0 for none), and the limit now */
int options_fps_limit_step(int* was, char* text, size_t size);
/* hotkey_showcase's and hotkey_stats's: the setting off or on, written to the .ini. Its line in the log, the setting now
   and before it and, when on, the corner it draws in; the same words without the corner in text (size bytes, 0 for
   none); and the setting now */
int options_showcase_step(char* text, size_t size);
int options_stats_step(char* text, size_t size);

/* The TV mode in effect now: 0 handheld, 1 TV (docked). docked's Auto asks the host's power state, and
   options_tv_mode_update keeps g_settings.tv_mode at the mode it picks from the frontend's thread, while the game's
   threads read it */
static inline int snx_tv_mode(void) { return __atomic_load_n(&g_settings.tv_mode, __ATOMIC_RELAXED); }
/* docked's set function: 0 Off, 1 On, 2 Auto, which picks the mode in effect now */
void options_tv_mode_set(const IniOpt* o, const char* v);
/* starts the host's power watcher and logs the mode Auto picks, once the window backend is up */
void options_tv_mode_start(void);
/* the frontend's tick: a step's new mode told to the services; Auto's watcher's change, its events and its log line */
void options_tv_mode_update(void);
/* the choice in effect: docked's (0 Off, 1 On, 2 Auto) until a step changes it */
int options_tv_mode_choice(void);
/* hotkey_tv_mode's and the game's menu's TV mode row's, on the frontend's thread: the next choice round On, Off and
   Auto (step < 0 the other way), in effect at once and for this run only, the .ini's docked as it was. Its line in the
   log, the same words in text (size bytes, 0 for none), and the choice. The services hear of a new mode on the next
   options_tv_mode_update: the menu's pads step it under the input's lock, which hid's change takes */
int options_tv_mode_step(int step, char* text, size_t size);
/* ldn, read: local wireless over the LAN (ldn_mitm's protocol), through an Eden room, or off
   (hle/services/network/ldn.c) */
enum { SNX_OPT_LDN_LAN, SNX_OPT_LDN_ROOM, SNX_OPT_LDN_OFF };
extern int g_opt_ldn;

#endif
