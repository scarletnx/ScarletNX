/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* docked's Auto (runtime/options.c) over a faked host power source: the mode each state picks (the battery handheld,
   the charger TV, a state that cannot be read TV), the log line that names both, am's and hid's change on a switch
   and on no other update, a change only when the watcher marks one, the 15 s query of a host with no watcher, and
   Off and On never asking the host. And a key the .ini gives twice: the last line's value, and a line that names the
   key and both values; and memory_fit, deprecated, read from an old .ini as memory_fit_mode while the file gives that
   one none, and the file written with memory_fit_mode and memory_fit under [Deprecated]. And controls_skin: a .ini
   without it opens with line, its default, and one that gives it empty keeps the built-in look. And controls_opacity,
   deprecated, read from an old .ini as the controls_alpha of the same look. And vk_descriptors, deprecated: the
   default an older version wrote in every game's .ini is no pick, so the game runs Automatic and nothing is said over
   its picture; a pick is carried to vk_descriptors_mode, and --vk_descriptors=N is this run's */
#include "common/utilities/common.h"
#include "nx/hle/services/hid.h"
#include "nx/hle/services/services.h"
#include "nx/memory/mem.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/unattended.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                                                    \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* the host the runtime sees: its power source, the watcher's mark and the clock; and what the runtime did with them */
static struct {
    int on_charger; /* plat_power_on_charger's answer: 1, 0 or -1 */
    int watch;      /* plat_power_watch's: 0 watching, -1 no watcher */
    int changed;    /* the watcher's mark, which plat_power_take_change takes */
    u64 ns;         /* plat_ns */
    int queries;    /* plat_power_on_charger's calls */
    int am, hid;    /* am_display_mode_changed's and hid_mode_changed's calls */
    int lines;      /* the log's lines */
    char logged[256];
    char said[4096]; /* every line, one after the other */
    int write;       /* plat_replace_file replaces the file: ini_open writes it */
    char skin[64];   /* the controls_skin backend_ini was given, which the on-screen controls read (vpad_ini) */
    char alpha[16];  /* the controls_alpha it was given */
} g_test;

int plat_power_on_charger(void) {
    g_test.queries++;
    return g_test.on_charger;
}
int plat_power_watch(void) { return g_test.watch; }
int plat_power_take_change(void) {
    int was = g_test.changed;
    g_test.changed = 0;
    return was;
}
void plat_power_mark_changed(void) { g_test.changed = 1; }
u64 plat_ns(void) { return g_test.ns; }
void am_display_mode_changed(void) { g_test.am++; }
void hid_mode_changed(void) { g_test.hid++; }
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(g_test.logged, sizeof g_test.logged, fmt, ap);
    va_end(ap);
    if (strlen(g_test.said) + strlen(g_test.logged) < sizeof g_test.said) strcat(g_test.said, g_test.logged);
    g_test.lines++;
}

/* the rest of the runtime options.c calls, which these tests never reach */
char g_exename[256];
u64 g_heap_max;
MemLayout g_layout;
const SnxModule* g_mod;
int g_opt_gdb;
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
int plat_replace_file(const char* tmp, const char* dst) {
    if (!g_test.write) return -1;
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) ? 0 : -1; /* rename fails over an existing file there */
#else
    return rename(tmp, dst);
#endif
}
int backend_ini(const char* k, const char* v) {
    if (!strcmp(k, "controls_skin")) snprintf(g_test.skin, sizeof g_test.skin, "%s", v);
    if (!strcmp(k, "controls_alpha")) snprintf(g_test.alpha, sizeof g_test.alpha, "%s", v);
    return 0;
}
int hud_debug_set(const char* text) {
    (void)text;
    return 0;
}
int unattended_known(const char* k) {
    (void)k;
    return 0;
}
int set_language_count(void) { return 18; }
void options_old_environment(void) {}
void options_old_environment_say(void) {}
/* the set functions of the other modules' tables, which g_ini_sections names */
#define NO_SET(name)                                                                                                   \
    void name(const IniOpt* o, const char* v) {                                                                        \
        (void)o;                                                                                                       \
        (void)v;                                                                                                       \
    }
NO_SET(vpad_showcase_corner_set)
NO_SET(stats_corner_set)
NO_SET(vpad_on_set)
NO_SET(vpad_skin_choose)
NO_SET(vpad_place_set)
NO_SET(backend_touch_mode_set)
NO_SET(gamepad_background_set)
NO_SET(gamepad_joycon_pair_set)
NO_SET(gamepad_map_set)
NO_SET(gamepad_player_map_set)
NO_SET(unattended_script_set)
NO_SET(unattended_record_set)
NO_SET(unattended_replay_set)
NO_SET(hotkey_set)

#define POLL_NS (15ull * 1000000000ull)

/* docked as the .ini or the command line gives it: ini_read, then its set function */
static void docked(const char* v) {
    const IniOpt* o = ini_find(g_display_ini, "docked");
    CHECK(o && o->set == options_tv_mode_set);
    ini_read(o, v);
}
static int mode(void) { return __atomic_load_n(&g_settings.tv_mode, __ATOMIC_RELAXED); }

/* Off and On are the mode, whatever the host's power: the host is never asked, nothing is watched or switched */
static void test_fixed(void) {
    g_test.on_charger = 0;
    docked("1");
    CHECK(mode() == 1);
    options_tv_mode_start();
    options_tv_mode_update();
    CHECK(mode() == 1);
    g_test.on_charger = 1;
    docked("0");
    CHECK(mode() == 0);
    g_test.changed = 1;
    options_tv_mode_start();
    options_tv_mode_update();
    CHECK(mode() == 0);
    CHECK(g_test.queries == 0 && g_test.am == 0 && g_test.hid == 0 && g_test.lines == 0);
    g_test.changed = 0;
}

/* Auto with a watcher: the battery is handheld from the .ini on; a switch follows the watcher's mark, and only it */
static void test_auto_watched(void) {
    g_test.watch = 0;
    g_test.on_charger = 0;
    docked("2");
    CHECK(mode() == 0);
    CHECK(g_test.queries == 1);
    options_tv_mode_start();
    CHECK(mode() == 0);
    CHECK(!strcmp(g_test.logged, "power: the host is on its battery: TV mode Auto picks handheld mode\n"));
    CHECK(g_test.am == 0 && g_test.hid == 0); /* the mode the game starts in is no change */
    /* the charger, which the watcher has not marked yet: nothing asks, nothing switches */
    g_test.on_charger = 1;
    g_test.queries = 0;
    options_tv_mode_update();
    CHECK(mode() == 0 && g_test.queries == 0 && g_test.am == 0);
    /* marked: TV mode, am's event and message and hid's handheld Npad, once */
    plat_power_mark_changed();
    options_tv_mode_update();
    CHECK(mode() == 1);
    CHECK(g_test.am == 1 && g_test.hid == 1);
    CHECK(!strcmp(g_test.logged, "power: the host is on its charger: TV mode Auto picks TV mode\n"));
    CHECK(!g_test.changed);
    options_tv_mode_update();
    CHECK(mode() == 1 && g_test.am == 1 && g_test.queries == 1);
    /* the battery again */
    g_test.on_charger = 0;
    plat_power_mark_changed();
    options_tv_mode_update();
    CHECK(mode() == 0 && g_test.am == 2 && g_test.hid == 2);
    CHECK(!strcmp(g_test.logged, "power: the host is on its battery: TV mode Auto picks handheld mode\n"));
    /* a state that cannot be read is TV mode, and says so */
    g_test.on_charger = -1;
    plat_power_mark_changed();
    options_tv_mode_update();
    CHECK(mode() == 1 && g_test.am == 3 && g_test.hid == 3);
    CHECK(!strcmp(g_test.logged, "power: the host's power state cannot be read: TV mode Auto picks TV mode\n"));
    /* a mark with the same state: no switch, no line */
    {
        int lines = g_test.lines;
        plat_power_mark_changed();
        options_tv_mode_update();
        CHECK(mode() == 1 && g_test.am == 3 && g_test.lines == lines);
    }
}

/* Auto on a host with no watcher: asked as the game starts, then every 15 s and not before */
static void test_auto_polled(void) {
    int queries;
    g_test.watch = -1;
    g_test.on_charger = 0;
    g_test.ns = 100 * POLL_NS;
    options_tv_mode_start();
    CHECK(mode() == 0 && g_test.am == 4 && g_test.hid == 4);
    CHECK(!strcmp(g_test.logged, "power: the host is on its battery: TV mode Auto picks handheld mode\n"));
    queries = g_test.queries;
    g_test.on_charger = 1;
    g_test.ns += POLL_NS - 1;
    options_tv_mode_update();
    CHECK(mode() == 0 && g_test.queries == queries);
    g_test.ns += 1;
    options_tv_mode_update();
    CHECK(mode() == 1 && g_test.queries == queries + 1 && g_test.am == 5 && g_test.hid == 5);
    CHECK(!strcmp(g_test.logged, "power: the host is on its charger: TV mode Auto picks TV mode\n"));
}

/* vk_memory at the top, in no section, and again in [Debug]: the second line's 0 is taken, and a line says so with
   both values, as the runtime writes the file back with that one line */
static void test_key_twice(void) {
    static const char text[] = "vk_memory=2\n[Debug]\nvk_memory=0\n";
    char *dir = snx_temp_dir(NULL), *path;
    CHECK(dir);
    path = snx_path_join(dir, "game.ini");
    CHECK(path && !snx_write_file(path, text, sizeof text - 1));
    g_test.said[0] = 0;
    ini_open(path);
    CHECK(g_settings.guest_memory == 0);
    CHECK(strstr(g_test.said, "settings: vk_memory is in the file twice, vk_memory=2 and then vk_memory=0: it takes 0, "
                              "the last, and the file keeps that line only\n"));
    CHECK(!snx_remove_tree(dir));
    free(path);
    free(dir);
}

/* a game's .ini of text, opened as a run opens it, with the log's lines from then in g_test.said; the file as ini_open
   wrote it back (g_test.write), else NULL */
static char* open_ini(const char* text) {
    char *dir = snx_temp_dir(NULL), *path, *now = NULL;
    size_t size;
    CHECK(dir);
    path = snx_path_join(dir, "game.ini");
    CHECK(path && !snx_write_file(path, text, strlen(text)));
    g_test.said[0] = 0;
    ini_open(path);
    if (g_test.write) CHECK((now = (char*)snx_read_file(path, &size)));
    CHECK(!snx_remove_tree(dir));
    free(path);
    free(dir);
    return now;
}
/* memory_fit (0 Off, 1 On; 1 by default on iOS) in an old .ini: 1 is memory_fit_mode's Auto, 0 the default every
   game's .ini had written, which leaves memory_fit_mode at its own Auto, and on iOS, whose default was 1, an Off. The
   file is written with memory_fit_mode, and memory_fit under [Deprecated], which the next run does not read */
static void test_memory_fit_old(void) {
    char *text, *again;
    CHECK(options_memory_fit_old("1", 0) == 0 && options_memory_fit_old("1", 1) == 0);
    CHECK(options_memory_fit_old("0", 0) == -1 && options_memory_fit_old("0", 1) == 1);
    CHECK(options_memory_fit_old("2", 0) == -2 && options_memory_fit_old("", 1) == -2);
    g_test.write = 1;
    text = open_ini("[Performance]\nmemory_fit=1\n");
    CHECK(strstr(g_test.said, "settings: memory_fit=1, deprecated, is memory_fit_mode=0 (Auto) now "
                              "(docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\nmemory_fit_mode=0\n") && strstr(text, "\n[Deprecated]\n") &&
          strstr(text, "\n[Deprecated]\n") < strstr(text, "\nmemory_fit=1\n"));
    again = open_ini(text); /* the next run: memory_fit_mode given, memory_fit not read again */
    CHECK(!strstr(g_test.said, "settings: memory_fit=") && again && strstr(again, "\nmemory_fit_mode=0\n"));
    free(again);
    free(text);
    text = open_ini("[Performance]\nmemory_fit=0\n");
    CHECK(strstr(g_test.said, "settings: memory_fit=0, deprecated, was its default: memory_fit_mode takes its own, 0 "
                              "(Auto), which fits the game to the device (docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\nmemory_fit_mode=0\n"));
    free(text);
    text = open_ini("[Performance]\nmemory_fit=yes\n");
    CHECK(strstr(g_test.said, "settings: memory_fit=yes, deprecated, is neither 0 nor 1: memory_fit_mode takes its "
                              "default, 0 (Auto)\n"));
    free(text);
    /* the new key given, the old one is not read */
    text = open_ini("[Performance]\nmemory_fit=1\nmemory_fit_mode=1\n");
    CHECK(!strstr(g_test.said, "settings: memory_fit=") && text && strstr(text, "\nmemory_fit_mode=1\n"));
    free(text);
    g_test.write = 0;
}

/* astc_bc, deprecated for astc_mode, read from an old .ini while astc_mode has no value: 1 is astc_mode=0 (Auto),
   written for the next run, and 0, the default every .ini had written, leaves astc_mode at its own */
static void test_astc_bc_old(void) {
    char *text, *again;
    CHECK(options_astc_bc_old("1") == 0 && options_astc_bc_old("0") == -1);
    CHECK(options_astc_bc_old("2") == -2 && options_astc_bc_old("") == -2);
    g_test.write = 1;
    text = open_ini("[Graphics]\nastc_bc=1\n");
    CHECK(strstr(g_test.said, "settings: astc_bc=1, deprecated, is astc_mode=0 (Auto) now (docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\nastc_mode=0\n") && strstr(text, "\n[Deprecated]\n") &&
          strstr(text, "\n[Deprecated]\n") < strstr(text, "\nastc_bc=1\n"));
    again = open_ini(text); /* the next run: astc_mode given, astc_bc not read again */
    CHECK(!strstr(g_test.said, "settings: astc_bc=") && again && strstr(again, "\nastc_mode=0\n"));
    free(again);
    free(text);
    text = open_ini("[Graphics]\nastc_bc=0\n");
    CHECK(strstr(g_test.said, "settings: astc_bc=0, deprecated, was its default: astc_mode takes its own, 0 (Auto), "
                              "which holds ASTC textures as RGBA8 or BC7 by the GPU's memory where the GPU samples BCn "
                              "but not ASTC (docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\nastc_mode=0\n"));
    free(text);
    text = open_ini("[Graphics]\nastc_bc=yes\n");
    CHECK(strstr(g_test.said, "settings: astc_bc=yes, deprecated, is neither 0 nor 1: astc_mode takes its default, 0 "
                              "(Auto)\n"));
    free(text);
    /* the new key given, the old one is not read */
    text = open_ini("[Graphics]\nastc_bc=1\nastc_mode=1\n");
    CHECK(!strstr(g_test.said, "settings: astc_bc=") && text && strstr(text, "\nastc_mode=1\n"));
    free(text);
    g_test.write = 0;
}

/* the log's settings line: the Display, Graphics and Performance settings off their default, each with its default */
static void test_settings_line(void) {
    char list[2048];
    g_settings.astc_mode = 1;
    CHECK(options_settings(list, sizeof list) >= 1 && strstr(list, "astc_mode=1 (default 0)"));
    g_settings.astc_mode = 0;
    options_settings(list, sizeof list);
    CHECK(!strstr(list, "astc_mode"));
}

/* controls_skin, the on-screen controls' look: a .ini without it opens with line, its default, and is written with it; a
   .ini that gives it empty keeps the built-in look, and the file keeps it empty */
static void test_controls_skin(void) {
    char* text;
    g_test.write = 1;
    snprintf(g_test.skin, sizeof g_test.skin, "%s", "not given");
    text = open_ini("[On-screen controls]\ncontrols=1\n");
    CHECK(!strcmp(g_test.skin, "line") && text && strstr(text, "\ncontrols_skin=line\n"));
    free(text);
    snprintf(g_test.skin, sizeof g_test.skin, "%s", "not given");
    text = open_ini("[On-screen controls]\ncontrols=1\ncontrols_skin=\n");
    CHECK(!g_test.skin[0] && text && strstr(text, "\ncontrols_skin=\n"));
    free(text);
    g_test.write = 0;
}

/* controls_opacity, deprecated, in an old .ini: read while controls_alpha has no value, as the value that draws the
   built-in look as it was (the art opaque now, on SNX_VPAD_BUILTIN_CURVE), said once; the file written with controls_alpha and
   controls_opacity under [Deprecated]. 50, the default it had last, and a value that is not a whole number leave
   controls_alpha its default, said */
static void test_controls_opacity_old(void) {
    char *text, *again;
    CHECK(options_controls_opacity_old("100") == 50 && options_controls_opacity_old("75") == 34 &&
          options_controls_opacity_old("0") == 0 && options_controls_opacity_old("150") == 50 &&
          options_controls_opacity_old("-3") == 0 && options_controls_opacity_old("40") == 14);
    CHECK(options_controls_opacity_old("50") == -1); /* the default the .ini files made since it was had written */
    CHECK(options_controls_opacity_old("most") == -2 && options_controls_opacity_old("50.5") == -2 &&
          options_controls_opacity_old("") == -2);
    g_test.write = 1;
    text = open_ini("[On-screen controls]\ncontrols=1\ncontrols_opacity=100\n");
    CHECK(!strcmp(g_test.alpha, "50"));
    CHECK(strstr(g_test.said,
                 "settings: controls_opacity=100, deprecated, is controls_alpha=50 now, the same look: the "
                 "built-in controls' art is opaque (docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\ncontrols_alpha=50\n") && strstr(text, "\n[Deprecated]\n") &&
          strstr(text, "\n[Deprecated]\n") < strstr(text, "\ncontrols_opacity=100\n"));
    snprintf(g_test.alpha, sizeof g_test.alpha, "%s", "not given");
    again = open_ini(text); /* the next run: controls_alpha given, controls_opacity not read again */
    CHECK(!strstr(g_test.said, "settings: controls_opacity=") && !strcmp(g_test.alpha, "50"));
    free(again);
    free(text);
    text = open_ini("[On-screen controls]\ncontrols_opacity=30\n");
    CHECK(!strcmp(g_test.alpha, "10") && strstr(g_test.said, "settings: controls_opacity=30, deprecated, is "
                                                             "controls_alpha=10 now"));
    free(text);
    /* 50, controls_opacity's last default, which a new game's .ini wrote: controls_alpha its own, 50, not 25 */
    snprintf(g_test.alpha, sizeof g_test.alpha, "%s", "not given");
    text = open_ini("[On-screen controls]\ncontrols_opacity=50\n");
    CHECK(!strcmp(g_test.alpha, "50"));
    CHECK(strstr(g_test.said, "settings: controls_opacity=50, deprecated, was its default: controls_alpha takes its "
                              "own, 50 (docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\ncontrols_alpha=50\n") && strstr(text, "\ncontrols_opacity=50\n"));
    free(text);
    text = open_ini("[On-screen controls]\ncontrols_opacity=most\n");
    CHECK(!strcmp(g_test.alpha, "50"));
    CHECK(strstr(g_test.said, "settings: controls_opacity=most, deprecated, is not a whole number: controls_alpha "
                              "takes its default, 50 (docs/DEPRECATIONS.md)\n"));
    free(text);
    /* the new key given, the old one is not read */
    text = open_ini("[On-screen controls]\ncontrols_opacity=100\ncontrols_alpha=80\n");
    CHECK(!strstr(g_test.said, "settings: controls_opacity=") && !strcmp(g_test.alpha, "80"));
    free(text);
    g_test.write = 0;
}

/* the Debug settings over the picture (options_debug) say the descriptors' setting: its key=value, NULL for none */
static const char* debug_descriptors(void) {
    static char list[1024];
    options_debug(INI_CHANGED_NO_TEXT, list, sizeof list);
    return strstr(list, "vk_descriptors");
}
/* vk_descriptors, deprecated: a game's .ini an older version wrote holds its default then, 0 (sets) before Automatic
   was, 3 after. Neither is a pick: vk_descriptors_mode takes its own, said, and the file is written with it and with
   vk_descriptors under [Deprecated], which the next run does not read. A startup setting's value is the process's first
   ini_open's (options_vk_descriptors_first checks it): here the file says what the run read */
static void test_vk_descriptors_old_default(void) {
    char *text, *again;
    g_test.write = 1;
    text = open_ini("[Debug]\nvk_descriptors=0\n");
    CHECK(strstr(g_test.said, "settings: vk_descriptors=0, deprecated, was its default: vk_descriptors_mode takes its "
                              "own, 3 (Automatic), sets on a GPU with memory of its own and heaps on one that shares "
                              "the system's (docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\nvk_descriptors_mode=3\n") && strstr(text, "\n[Deprecated]\n") &&
          strstr(text, "\n[Deprecated]\n") < strstr(text, "\nvk_descriptors=0\n"));
    again = open_ini(text); /* the next run: vk_descriptors_mode given, vk_descriptors not read again */
    CHECK(!strstr(g_test.said, "settings: vk_descriptors=") && again && strstr(again, "\nvk_descriptors_mode=3\n"));
    free(again);
    free(text);
    text = open_ini("[Debug]\nvk_descriptors=3\n");
    CHECK(strstr(g_test.said, "settings: vk_descriptors=3, deprecated, was its default: vk_descriptors_mode takes its "
                              "own, 3"));
    CHECK(text && strstr(text, "\nvk_descriptors_mode=3\n"));
    free(text);
    g_test.write = 0;
}
/* the rest of vk_descriptors: 1 and 2 in an old .ini are picks, carried to vk_descriptors_mode; a value that is no
   whole number from 0 to 3 leaves it its default; the new key given, the old one is not read. On the command line every
   value is that run's choice, 0 too, its override of vk_descriptors_mode, and the file keeps its own */
static void test_vk_descriptors_old(void) {
    char* text;
    CHECK(options_vk_descriptors_old("0", 0) == -1 && options_vk_descriptors_old("3", 0) == -1);
    CHECK(options_vk_descriptors_old("1", 0) == 1 && options_vk_descriptors_old("2", 0) == 2);
    CHECK(options_vk_descriptors_old("0", 1) == 0 && options_vk_descriptors_old("3", 1) == 3);
    CHECK(options_vk_descriptors_old("4", 0) == -2 && options_vk_descriptors_old("-1", 1) == -2 &&
          options_vk_descriptors_old("1.5", 0) == -2 && options_vk_descriptors_old("", 1) == -2);
    g_test.write = 1;
    text = open_ini("[Debug]\nvk_descriptors=1\n");
    CHECK(strstr(g_test.said, "settings: vk_descriptors=1, deprecated, is vk_descriptors_mode=1 (Buffers) now "
                              "(docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\nvk_descriptors_mode=1\n"));
    free(text);
    text = open_ini("[Debug]\nvk_descriptors=sets\n");
    CHECK(strstr(g_test.said, "settings: vk_descriptors=sets, deprecated, is not a whole number from 0 to 3: "
                              "vk_descriptors_mode takes its default, 3 (Automatic) (docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\nvk_descriptors_mode=3\n"));
    free(text);
    text = open_ini("[Debug]\nvk_descriptors=1\nvk_descriptors_mode=2\n");
    CHECK(!strstr(g_test.said, "settings: vk_descriptors=") && text && strstr(text, "\nvk_descriptors_mode=2\n"));
    free(text);
    /* --vk_descriptors=0: Sets this run, said, and no "not a setting"; the file keeps its Automatic */
    ini_override("vk_descriptors", "0");
    text = open_ini("[Debug]\nvk_descriptors_mode=3\n");
    CHECK(!strstr(g_test.said, "is not a setting") && ini_overridden("vk_descriptors_mode") &&
          !strcmp(ini_overridden("vk_descriptors_mode"), "0"));
    CHECK(strstr(g_test.said,
                 "settings: --vk_descriptors=0 is deprecated: this run reads it as --vk_descriptors_mode=0 "
                 "(Sets) (docs/DEPRECATIONS.md)\n"));
    CHECK(text && strstr(text, "\nvk_descriptors_mode=3\n"));
    free(text);
    ini_override("vk_descriptors_mode", NULL); /* the mapping's, which a run keeps: the next file's own again */
    ini_override("vk_descriptors", "many");
    text = open_ini("[Debug]\nvk_descriptors_mode=1\n");
    CHECK(!ini_overridden("vk_descriptors_mode"));
    CHECK(strstr(g_test.said, "settings: --vk_descriptors=many, deprecated, is not a whole number from 0 to 3: "
                              "vk_descriptors_mode=1 this run, the .ini's (docs/DEPRECATIONS.md)\n"));
    free(text);
    ini_override("vk_descriptors", "1");
    ini_override("vk_descriptors_mode", "2");
    text = open_ini("[Debug]\nvk_descriptors_mode=3\n");
    CHECK(!strcmp(ini_overridden("vk_descriptors_mode"), "2"));
    CHECK(strstr(g_test.said, "settings: --vk_descriptors=1 is deprecated, and the command line gives "
                              "--vk_descriptors_mode=2 too: vk_descriptors_mode=2 this run (docs/DEPRECATIONS.md)\n"));
    free(text);
    ini_override("vk_descriptors", NULL);
    ini_override("vk_descriptors_mode", NULL);
    g_test.write = 0;
}
/* a startup setting is read by the process's first ini_open alone, so each of these is a process of its own
   (CMakeLists.txt's options_vk_descriptors_*): what the run plays with and says over its picture. old: an older
   version's .ini with vk_descriptors=0, its default then, plays Automatic with nothing over the picture; pick: its 2,
   a pick, plays heaps, said; cli: --vk_descriptors=0 plays sets this run */
static int options_vk_descriptors_first(const char* which) {
    char* text;
    g_test.write = 1;
    if (!strcmp(which, "--vk-descriptors-old")) {
        text = open_ini("[Debug]\nvk_descriptors=0\n");
        CHECK(g_settings.descriptor_buffers == 3 && !debug_descriptors());
    } else if (!strcmp(which, "--vk-descriptors-pick")) {
        text = open_ini("[Debug]\nvk_descriptors=2\n");
        CHECK(g_settings.descriptor_buffers == 2 && debug_descriptors() &&
              !strncmp(debug_descriptors(), "vk_descriptors_mode=2", 21));
    } else if (!strcmp(which, "--vk-descriptors-cli")) {
        ini_override("vk_descriptors", "0");
        text = open_ini("[Debug]\nvk_descriptors_mode=3\n");
        CHECK(g_settings.descriptor_buffers == 0 && text && strstr(text, "\nvk_descriptors_mode=3\n"));
    } else {
        fprintf(stderr, "options_tv_mode: %s is none of --vk-descriptors-old, -pick, -cli\n", which);
        return 2;
    }
    free(text);
    printf("options_tv_mode %s: ok\n", which);
    return 0;
}

int main(int argc, char** argv) {
    if (argc > 1) return options_vk_descriptors_first(argv[1]);
    test_fixed();
    test_auto_watched();
    test_auto_polled();
    test_key_twice(); /* after the TV mode tests: the file's every setting applied, which they count from nought */
    test_settings_line();
    test_memory_fit_old();
    test_astc_bc_old();
    test_controls_skin();
    test_controls_opacity_old();
    test_vk_descriptors_old_default();
    test_vk_descriptors_old();
    printf("options_tv_mode: ok\n");
    return 0;
}
