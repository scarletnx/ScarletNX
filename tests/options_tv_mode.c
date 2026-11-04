/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* docked's Auto (runtime/options.c) over a faked host power source: the mode each state picks (the battery handheld,
   the charger TV, a state that cannot be read TV), the log line that names both, am's and hid's change on a switch
   and on no other update, a change only when the watcher marks one, the 15 s query of a host with no watcher, and
   Off and On never asking the host. And a key the .ini gives twice: the last line's value, and a line that names the
   key and both values */
#include "common/utilities/common.h"
#include "nx/hle/services/hid.h"
#include "nx/hle/services/services.h"
#include "nx/memory/mem.h"
#include "common/utilities/ini.h"
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
    int watch_calls; /* plat_power_watch's calls */
    int ini_writes; /* backend_ini's calls */
    int am, hid;    /* am_display_mode_changed's and hid_mode_changed's calls */
    int lines;      /* the log's lines */
    char logged[256];
    char said[4096]; /* every line, one after the other */
} g_test;

int plat_power_on_charger(void) {
    g_test.queries++;
    return g_test.on_charger;
}
int plat_power_watch(void) {
    g_test.watch_calls++;
    return g_test.watch;
}
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
    (void)tmp;
    (void)dst;
    return -1;
}
int backend_ini(const char* k, const char* v) {
    (void)k;
    (void)v;
    g_test.ini_writes++;
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
NO_SET(vpad_place_set)
NO_SET(backend_touch_mode_set)
NO_SET(gamepad_background_set)
NO_SET(gamepad_joycon_pair_set)
NO_SET(gamepad_map_set)
NO_SET(gamepad_player_map_set)
NO_SET(unattended_script_set)

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
    options_tv_mode_forget_watcher();
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

/* the docked_key hotkey: On -> Off -> Auto -> On, its events only on a real change, the watcher started once, and no
   write of the .ini */
static void test_cycle(void) {
    int before;
    options_tv_mode_forget_watcher();
    g_test.watch = 0;
    g_test.on_charger = 0;
    g_test.watch_calls = 0;
    g_test.ini_writes = 0;
    docked("1");
    CHECK(mode() == 1);
    before = g_test.am;
    CHECK(options_tv_mode_next() == 0);
    CHECK(mode() == 0 && g_test.am == before + 1 && g_test.hid >= 1);
    CHECK(options_tv_mode_next() == 2);
    CHECK(mode() == 0); /* Auto picks the battery */
    CHECK(g_test.watch_calls <= 1);
    {
        int calls = g_test.watch_calls;
        CHECK(options_tv_mode_next() == 1);
        CHECK(mode() == 1);
        CHECK(options_tv_mode_next() == 0); /* Off */
        CHECK(options_tv_mode_next() == 2); /* Auto again: the watcher it started is not started again */
        CHECK(g_test.watch_calls == calls);
    }
    CHECK(options_tv_mode_next() == 1); /* On */
    CHECK(g_test.ini_writes == 0);
}

int main(void) {
    test_fixed();
    test_auto_watched();
    test_auto_polled();
<<<<<<< HEAD
    test_key_twice(); /* last: the file's every setting applied, which the TV mode tests count from nought */
=======
    test_cycle();
>>>>>>> 214a4cd4 ([runtime] Cycle TV mode and the presented effects live)
    printf("options_tv_mode: ok\n");
    return 0;
}
