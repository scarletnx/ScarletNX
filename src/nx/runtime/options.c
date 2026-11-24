/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's .ini: the sections it is written in (the runtime's own settings here, the window's, the on-screen
   controls' and the gamepads' from their modules), the command line's overrides over it, its reload while the game
   runs, and the values the game's menu changes, written back */
#include "nx/runtime/options.h"
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/key_said.h"
#include "common/utilities/mods.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/hid.h"
#include "nx/hle/services/services.h"
#include "nx/memory/mem.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/platform/vpad.h"
#include "common/utilities/bits.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

_Atomic int g_opt_fps = SNX_FPS_GAME;

/* ---- the runtime's own settings ---- */

/* a language the console doesn't have is English (US), as the clamp to the table's range would not say */
void options_language_set(const IniOpt* o, const char* v) {
    char* end;
    long n = strtol(v, &end, 10);
    (void)o;
    if (end != v && (n < 0 || n >= set_language_count())) g_settings.language = 1;
}

/* ---- TV mode ---- */
/* the choice in effect, docked's (0 Off, 1 On, 2 Auto) until a step changes it for the run, and Auto's view of the
   host. g_settings.tv_mode holds the mode in effect, 0 or 1, so every service reads it as it always has */
static struct {
    _Atomic int choice; /* atomic: the game's menu shows it from the renderer's thread */
    int told;       /* the mode the services last heard of: a step there and back is no change */
    int on_charger; /* the host's state at Auto's last read: 1, 0 or -1 */
    int watch;      /* 1 when the platform watches the power source, 0 when Auto polls it */
    u64 poll_next;  /* with no watcher, Auto's next read, in plat_ns */
} g_tv = {.choice = 1, .told = 1};
/* a host with no watcher: rarely, since a charger is not a thing that changes every second */
#define TV_POLL_NS (15ull * 1000000000ull)
/* Auto picks TV mode unless the host runs on its battery. A host whose state cannot be read is on TV mode */
static int tv_mode_now(void) {
    if (atomic_load_explicit(&g_tv.choice, memory_order_relaxed) != 2)
        return atomic_load_explicit(&g_tv.choice, memory_order_relaxed);
    return plat_power_on_charger() != 0;
}
void options_tv_mode_set(const IniOpt* o, const char* v) {
    char* end;
    long n = strtol(v, &end, 10);
    (void)o;
    if (end == v) return;
    atomic_store_explicit(&g_tv.choice, n < 0 ? 0 : n > 2 ? 2 : (int)n, memory_order_relaxed);
    g_tv.told = tv_mode_now(); /* the mode the game starts in is no change */
    atomic_store_explicit((_Atomic int*)&g_settings.tv_mode, g_tv.told, memory_order_relaxed);
}
/* Auto's read of the host's power state: the mode it picks in effect, and its line in the log when say asks for it or
   the state or the mode changed since the last read */
static void tv_auto_read(int say) {
    int on = plat_power_on_charger(), mode = on != 0;
    if (say || on != g_tv.on_charger || mode != snx_tv_mode()) {
        if (on > 0)
            LOG("power: the host is on its charger: TV mode Auto picks %s\n", mode ? "TV mode" : "handheld mode");
        else if (on == 0)
            LOG("power: the host is on its battery: TV mode Auto picks %s\n", mode ? "TV mode" : "handheld mode");
        else
            LOG("power: the host's power state cannot be read: TV mode Auto picks TV mode\n");
    }
    g_tv.on_charger = on;
    if (!g_tv.watch) g_tv.poll_next = plat_ns() + TV_POLL_NS;
    atomic_store_explicit((_Atomic int*)&g_settings.tv_mode, mode, memory_order_relaxed);
}
/* Auto's read now, said in the log, with a change the watcher marked before it */
static void tv_auto_now(void) {
    if (g_tv.watch) (void)plat_power_take_change();
    tv_auto_read(1);
}
/* the services told of the mode in effect when it is not the one they last heard of, as a console put in its dock or
   taken out: am's message and event, hid's handheld Npad */
static void tv_mode_tell(void) {
    int mode = snx_tv_mode();
    if (mode == g_tv.told) return;
    g_tv.told = mode;
    am_display_mode_changed();
    hid_mode_changed();
}
void options_tv_mode_start(void) {
    if (atomic_load_explicit(&g_tv.choice, memory_order_relaxed) != 2) return; /* fixed On and Off need no watcher */
    g_tv.watch = plat_power_watch() == 0;
    tv_auto_now();
    tv_mode_tell();
}
void options_tv_mode_update(void) {
    /* the host read again when its watcher marked a change, or with none every TV_POLL_NS, and nowhere else */
    if (atomic_load_explicit(&g_tv.choice, memory_order_relaxed) == 2 &&
        (g_tv.watch ? plat_power_take_change() : plat_ns() >= g_tv.poll_next))
        tv_auto_read(0);
    /* a step's mode too: told here, out of the input's lock the menu's pads step under, which hid's change takes */
    tv_mode_tell();
}
int options_tv_mode_choice(void) { return atomic_load_explicit(&g_tv.choice, memory_order_relaxed); }
int options_tv_mode_step(int step, char* text, size_t size) {
    /* round On (1), Off (0) and Auto (2): forward a value down, from Off round to Auto */
    int choice = (atomic_load_explicit(&g_tv.choice, memory_order_relaxed) + (step < 0 ? 1 : 2)) % 3;
    char word[16], line[64];
    atomic_store_explicit(&g_tv.choice, choice, memory_order_relaxed);
    if (choice == 2) {
        if (!g_tv.watch) g_tv.watch = plat_power_watch() == 0; /* once: on macOS each call starts a thread */
        tv_auto_now();
    } else
        atomic_store_explicit((_Atomic int*)&g_settings.tv_mode, choice, memory_order_relaxed);
    ini_word(ini_find(g_display_ini, "docked"), choice, word, sizeof word);
    snprintf(line, sizeof line, "TV mode: %s (%s)", word, snx_tv_mode() ? "TV mode" : "handheld mode");
    LOG("%s\n", line);
    if (size && choice == 2)
        snprintf(text, size, "TV mode: %s (%s)", word, snx_tv_mode() ? "TV" : "handheld");
    else if (size)
        snx_key_now(text, size, "TV mode", word);
    return choice;
}

/* "game" in any case, or a number: 0 and below 1 unlimited, 10 to 1000 frames a second (clamped), and the values an
   older file meant the game's pace with (negative ones, and 1, its 60 FPS). Anything else leaves the limit */
void options_fps_set(const IniOpt* o, const char* v) {
    char* end;
    double d;
    int fps;
    (void)o;
    while (*v == ' ' || *v == '\t') v++;
    if ((v[0] == 'g' || v[0] == 'G') && (v[1] == 'a' || v[1] == 'A') && (v[2] == 'm' || v[2] == 'M') &&
        (v[3] == 'e' || v[3] == 'E') && !v[4 + strspn(v + 4, " \t")]) {
        atomic_store_explicit(&g_opt_fps, SNX_FPS_GAME, memory_order_relaxed);
        return;
    }
    d = strtod(v, &end);
    if (end == v || d != d) return;
    if (d < 0 || (d >= 1 && d < 2))
        fps = SNX_FPS_GAME;
    else if (d < 1)
        fps = 0;
    else
        fps = d < 10 ? 10 : d > 1000 ? 1000 : (int)d;
    atomic_store_explicit(&g_opt_fps, fps, memory_order_relaxed);
}
void fps_limit_text(int fps, char* out, size_t n) {
    if (fps == SNX_FPS_GAME)
        snprintf(out, n, "game");
    else
        snprintf(out, n, "%d", fps);
}
int options_fps_limit_step(int* was, char* text, size_t size) {
    int before = atomic_load_explicit(&g_opt_fps, memory_order_relaxed), now;
    char v[16], old[16], line[64];
    if (before) {
        *was = before;
        now = 0;
    } else
        now = *was ? *was : SNX_FPS_GAME;
    atomic_store_explicit(&g_opt_fps, now, memory_order_relaxed); /* present_pace reads it on the game's threads */
    fps_limit_text(now, v, sizeof v);
    ini_set("fps_limit", v);
    fps_limit_text(before, old, sizeof old);
    snx_fps_limit_said(line, sizeof line, v, old);
    LOG("%s\n", line);
    if (size) snx_fps_limit_now(text, size, v);
    return now;
}
/* showcase's and stats's steps: setting off or on, the .ini's key with it (a run's --key=1 stays out of the file);
   the log's line names the corner it draws in */
static int onoff_step(_Atomic int* setting, const char* key, const char* what, const char* corner, char* text,
                      size_t size) {
    int on = !atomic_load_explicit(setting, memory_order_relaxed);
    char line[64];
    atomic_store_explicit(setting, on, memory_order_relaxed); /* the renderer reads it */
    ini_set(key, on ? "1" : "0");
    snx_key_said(line, sizeof line, what, on ? "on" : "off", on ? "off" : "on");
    if (on)
        LOG("%s, %s_corner=%s\n", line, key, corner);
    else
        LOG("%s\n", line);
    if (size) snx_key_now(text, size, what, on ? "on" : "off");
    return on;
}
int options_showcase_step(char* text, size_t size) {
    return onoff_step((_Atomic int*)&g_settings.showcase, "showcase", "Showcase", g_settings.showcase_corner, text,
                      size);
}
int options_stats_step(char* text, size_t size) {
    return onoff_step((_Atomic int*)&g_settings.stats, "stats", "Stats", g_settings.stats_corner, text, size);
}

int g_opt_ldn = SNX_OPT_LDN_LAN;
/* lan, room or off. Both keys are read once, at start: a value they don't take stops the game there, as a player
   who wrote ldn=rooom would otherwise play alone thinking they were in the room */
void options_ldn_set(const IniOpt* o, const char* v) {
    static const char* const words[] = {"lan", "room", "off"};
    int i;
    (void)o;
    for (i = 0; i < 3; i++)
        if (!strcmp(v, words[i])) {
            g_opt_ldn = i;
            return;
        }
    fatal("settings: ldn=%s is not lan, room or off", v);
}
void options_room_set(const IniOpt* o, const char* v) {
    (void)o;
    if (v[strcspn(v, " \t\r")]) fatal("settings: room=%.64s is not host or host:port", v);
}

/* MB: below 1 no cap but the heap region's own, which also caps a larger one */
void options_heap_max_set(const IniOpt* o, const char* v) {
    char* end;
    double mb = strtod(v, &end);
    (void)o;
    if (end == v || mb != mb) return;
    if (mb < 1)
        g_heap_max = 0;
    else
        g_heap_max = mb * 1048576.0 >= (double)HEAP_SIZE ? HEAP_SIZE : (u64)(mb * 1048576.0);
}

/* key's row in a section, with whether it is one of the runtime's own; NULL for none */
static const IniOpt* find_row(const char* key, int* own) {
    int s, t;
    for (s = 0; s < g_ini_sections_count; s++)
        for (t = 0; t < INI_SECTION_TABLES && g_ini_sections[s].table[t]; t++) {
            const IniOpt* o = ini_find(g_ini_sections[s].table[t], key);
            if (o) {
                if (own) *own = g_ini_sections[s].own;
                return o;
            }
        }
    return NULL;
}

int snx_setting_known(const char* key) {
    return find_row(key, NULL) || unattended_known(key) || !strcmp(key, SNX_LAUNCHER_PICKS);
}

/* ---- the file's values, the command line's, and the menu's waiting to be written ---- */

#define KEY_MAX 64
/* every setting the runtime knows, which a file it wrote holds, and room for keys it doesn't (an older or newer
   runtime's). At 128, below the settings it knows, a file it wrote lost the keys after the 128th */
#define STORE_MAX (SETTINGS_KEYS + 64)
#define VALUE_MAX 2048
#define OVERRIDE_MAX 64
#define PENDING_MAX 64
#define PENDING_VALUE 256 /* a gamepad map's text is up to 165 bytes (pad_map.h) */

/* The file as last read, with the menu's changes merged in: only the thread that opens and polls it touches it */
typedef struct {
    char key[KEY_MAX];
    char value[VALUE_MAX];
} Entry;
static Entry g_store[STORE_MAX];
static int g_nstore;
/* the command line's, set before ini_open; after it only the thread that opens and polls the file touches them */
typedef struct {
    char key[KEY_MAX];
    const char* value;
    int launcher; /* given with --launcher-picks=1: the launcher's pick for every game, which the game's own go over */
    int beaten;   /* the game's menu set the key since: its value is the menu's from then on */
} Override;
static Override g_over[OVERRIDE_MAX];
static int g_nover;
/* the menu's, from any thread, until a write of the file has them. g_pending_lock is a leaf: nothing is called while
   it is held */
typedef struct {
    char key[KEY_MAX];
    char value[PENDING_VALUE];
    int unpick; /* the game's pick taken out (ini_unpick): value is empty */
    int apply;  /* the menu did not apply it (ini_set_apply): the poll does */
} Pending;
static Pending g_pending[PENDING_MAX];
static int g_npending;
static PMutex g_pending_lock = PMUTEX_INIT;
static PMutex g_poll_lock = PMUTEX_INIT;

static char g_path[1400];
static int g_started; /* ini_open is done: the startup settings are kept from now on */
static u64 g_hash;    /* the file as this run last read or wrote it */
static u64 g_seen;    /* a changed file seen by the last poll, read once a second poll sees it unchanged */
/* The game's own picks, <name>.picks beside its .ini (picks_path): what its menu and the launcher's Settings of the
   game set for it alone, kept apart from the .ini, which the runtime writes whole, defaults too. Only the thread that
   opens and polls the files changes them, under g_picks_lock (a leaf), so that ini_picked reads them from the menu's */
static struct {
    Entry entry[STORE_MAX];
    int n;
    int dirty;     /* a change the file does not have yet */
    _Atomic int changes;   /* atomic: counts the changes, which the menu watches to show them */
    _Atomic int poll_soon; /* atomic: a pick the menu took out, shown at once: polled at the next tick (ini_poll_due) */
    char path[sizeof g_path + 8];
    u64 hash, seen; /* as g_hash and g_seen are the .ini's */
    _Atomic int old; /* atomic: the keys Clean Old Settings takes out, the .ini's deprecated ones and the picks' no
                       pick takes, as the files were last read or written */
    /* the settings read at the start whose pick the menu took out this run: the value the next start takes without
       it (value_of then), which this run's is not; under g_picks_lock */
    Entry next[16];
    int nnext;
    _Atomic int clean_soon; /* atomic: the game's menu's Clean Old Settings, taken at the next tick */
} g_picks;
static PMutex g_picks_lock = PMUTEX_INIT;

static int override_at(const char* key) {
    int i;
    for (i = 0; i < g_nover; i++)
        if (!strcmp(g_over[i].key, key)) return i;
    return -1;
}
static const char* override_of(const char* key) {
    int i = override_at(key);
    return i < 0 ? NULL : g_over[i].value;
}
/* the command line's value that goes over the game's picks: one the launcher did not mark as its pick for every game,
   nor the menu set since */
static const char* run_value(const char* key) {
    int i = override_at(key);
    return i < 0 || g_over[i].launcher || g_over[i].beaten ? NULL : g_over[i].value;
}
static const char* launcher_value(const char* key) {
    int i = override_at(key);
    return i < 0 || !g_over[i].launcher || g_over[i].beaten ? NULL : g_over[i].value;
}
static Entry* picks_find(const char* key) {
    int i;
    for (i = 0; i < g_picks.n; i++)
        if (!strcmp(g_picks.entry[i].key, key)) return &g_picks.entry[i];
    return NULL;
}
/* a key the game's picks take: one the launcher's Settings show, a row of the .ini's sections that is not the game's
   own (INI_IN_GAME: a control's place, the dump it was made from) */
static int picks_key(const char* key) {
    const IniOpt* o = find_row(key, NULL);
    return o && !(o->flags & INI_IN_GAME);
}
/* key's pick set to value, or taken out (value NULL); one that does not fit is said and left out, never cut */
static void picks_put(const char* key, const char* value) {
    Entry* e;
    int i;
    if (value && (strlen(key) >= KEY_MAX || strlen(value) >= VALUE_MAX)) {
        WARN_LIMIT(1, "settings: %s's value is too long for %s, and this game's pick of it is not saved\n", key,
                   g_picks.path);
        return;
    }
    if (value && !picks_find(key) && g_picks.n == STORE_MAX) {
        WARN_LIMIT(1, "settings: %s holds %d picks already, so %s=%s is not saved\n", g_picks.path, STORE_MAX, key,
                   value);
        return;
    }
    pm_lock(&g_picks_lock);
    for (i = 0; value && i < g_picks.nnext; i++) /* picked again: the pick is the next start's */
        if (!strcmp(g_picks.next[i].key, key)) g_picks.next[i--] = g_picks.next[--g_picks.nnext];
    e = picks_find(key);
    if (!value && e)
        *e = g_picks.entry[--g_picks.n];
    else if (value) {
        if (!e) {
            e = &g_picks.entry[g_picks.n++];
            memcpy(e->key, key, strlen(key) + 1);
        }
        memcpy(e->value, value, strlen(value) + 1);
    }
    pm_unlock(&g_picks_lock);
    atomic_fetch_add_explicit(&g_picks.changes, 1, memory_order_release);
}
static Entry* store_find(const char* key) {
    int i;
    for (i = 0; i < g_nstore; i++)
        if (!strcmp(g_store[i].key, key)) return &g_store[i];
    return NULL;
}
/* a value that doesn't fit is dropped, never cut */
static void store_put(const char* key, const char* value) {
    Entry* e = store_find(key);
    size_t n = strlen(value);
    if (strlen(key) >= KEY_MAX) {
        WARN_LIMIT(1, "settings: key %.40s... is too long and is left out\n", key);
        return;
    }
    if (n >= VALUE_MAX) {
        WARN_LIMIT(1, "settings: the value of %s is longer than %d bytes and is left out\n", key, VALUE_MAX - 1);
        return;
    }
    if (!e) {
        if (g_nstore == STORE_MAX) {
            WARN_LIMIT(1, "settings: more than %d keys, %s and those after it are left out\n", STORE_MAX, key);
            return;
        }
        e = &g_store[g_nstore++];
        memcpy(e->key, key, strlen(key) + 1);
    }
    memcpy(e->value, value, n + 1);
}
/* a line the file's reading leaves, said with its number and its text */
static void line_left(void* ctx, int line, const char* text, const char* does) {
    (void)ctx;
    WARN("settings: line %d of the file, \"%.60s\", %s\n", line, text, does);
}
/* a key the file gives twice takes its last line's value (sections only group the keys), and the file is written back
   with that one line */
static void store_parse(char* text) {
    SnxIni ini;
    char *key, *value;
    g_nstore = 0;
    snx_ini_start(&ini, text);
    ini.left = line_left;
    while (snx_ini_next(&ini, &key, &value)) {
        const Entry* e = store_find(key);
        if (e && strcmp(e->value, value))
            WARN("settings: %s is in the file twice, %s=%s and then %s=%s: it takes %s, the last, and the file keeps "
                 "that line only\n",
                 key, key, e->value, key, value, value);
        store_put(key, value);
    }
}
/* a setting's value: this run's command line's, else the game's pick, else the launcher's pick for every game (its
   command line's too), else the .ini's, else its default */
static const char* value_of(const IniOpt* o) {
    const char* v = run_value(o->key);
    const Entry* e;
    if (v) return v;
    if (!(o->flags & INI_IN_GAME) && (e = picks_find(o->key)) != NULL) return e->value;
    if ((v = launcher_value(o->key)) != NULL) return v;
    e = store_find(o->key);
    return e ? e->value : o->def;
}

const char* ini_deprecated_value(const char* key) {
    const char* v = override_of(key);
    const Entry* e;
    if (!ini_deprecated(key)) return NULL;
    if (v) return v;
    e = store_find(key);
    return e ? e->value : NULL;
}

void ini_override(const char* k, const char* v) {
    size_t n = strlen(k);
    int i;
    if (n >= KEY_MAX) return;
    for (i = 0; i < g_nover && strcmp(g_over[i].key, k); i++) continue;
    if (i == OVERRIDE_MAX) {
        WARN_LIMIT(1, "settings: more than %d settings on the command line, --%s and those after it do nothing\n",
                   OVERRIDE_MAX, k);
        return;
    }
    if (i == g_nover) memcpy(g_over[g_nover++].key, k, n + 1);
    g_over[i].value = v;
}

const char* ini_overridden(const char* k) { return override_of(k); }
/* who gave k its value, in words, the first of value_of's layers that holds one; NULL for nobody */
static const char* given_by(const char* k) {
    const IniOpt* o = find_row(k, NULL);
    const Entry* e;
    int picked;
    if (run_value(k)) return "this run's command line's";
    pm_lock(&g_picks_lock);
    picked = picks_find(k) != NULL;
    pm_unlock(&g_picks_lock);
    if (picked) return "the game's own pick";
    if (launcher_value(k)) return "the launcher's";
    if (override_of(k)) return "the game's menu's"; /* over this run's command line's: in the picks or the .ini now */
    /* the file is written with every row, a row nobody set at its default (put_rows): only another value is a choice */
    e = store_find(k);
    return e && (!o || !o->def || strcmp(e->value, o->def)) ? "the game's .ini's" : NULL;
}
int ini_given(const char* k) { return given_by(k) != NULL; }

/* this run's values for its kind of program that took (options_kind_value), "key Word" between ", ", for the notice,
   and how many */
static struct {
    char list[256];
    int n;
} g_kind;
/* v, a value of o's, as its word ("Off"): v itself where o's words do not name it */
static void word_of(const IniOpt* o, const char* v, char* out, size_t n) {
    if (o->type == INI_INT && o->words)
        ini_word(o, (int)strtod(v, NULL), out, n);
    else
        snprintf(out, n, "%s", v);
}
/* v, a value of o's, as the log says it: "0 (Off)" where o's words name it */
static void value_said(const IniOpt* o, const char* v, char* out, size_t n) {
    char word[64];
    word_of(o, v, word, sizeof word);
    if (strcmp(word, v))
        snprintf(out, n, "%s (%s)", v, word);
    else
        snprintf(out, n, "%s", v);
}
int options_kind_value(const char* key, const char* value, const char* kind) {
    const IniOpt* o = find_row(key, NULL);
    const char* by;
    char now[64], now_said[96], kind_said[96], def_said[96], def_word[64], other[16], other_said[96], option[128],
        word[64];
    int lo, hi, v, def, other_v = -1;
    size_t at;
    if (!o) {
        WARN("settings: %s is no setting, so %s's %s=%s does nothing\n", key, kind, key, value);
        return -1;
    }
    if (o->type == INI_TEXT)
        snprintf(now, sizeof now, "%s", (const char*)o->var);
    else
        ini_format(o, now, sizeof now);
    lo = (int)o->lo;
    hi = (int)o->hi;
    /* a reload would put the files' value back over one read at any other time */
    if (!(o->flags & INI_STARTUP) || o->type != INI_INT || !o->words || !o->def || hi - lo != 2) {
        WARN("settings: %s is no setting of three words read at the start whose default chooses between the other "
             "two, so %s's %s=%s is not taken: %s=%s this run\n",
             key, kind, key, value, key, now);
        return -1;
    }
    v = (int)strtod(value, NULL);
    def = (int)strtod(o->def, NULL);
    value_said(o, value, kind_said, sizeof kind_said);
    value_said(o, o->def, def_said, sizeof def_said);
    if (v < lo || v > hi || v == def) {
        WARN("settings: %s's %s=%s is not one of the two values %s chooses between: %s=%s this run\n", kind, key, value,
             def_said, key, now);
        return -1;
    }
    for (int i = lo; i <= hi; i++)
        if (i != def && i != v) other_v = i;
    word_of(o, o->def, def_word, sizeof def_word);
    if (!ini_at_default(o)) {
        by = given_by(key);
        value_said(o, now, now_said, sizeof now_said);
        if (strtod(now, NULL) != (double)v)
            LOG("settings: %s=%s, %s, over the %s its %s gives %s\n", key, now_said, by ? by : "", kind_said, def_word,
                kind);
        else
            LOG("settings: %s=%s, %s, the value its %s gives %s too\n", key, now_said, by ? by : "", def_word, kind);
        return 0;
    }
    /* the default given by a layer as itself, not a value it could not read (ini_read said that one) */
    by = given_by(key);
    if (!by || strtod(value_of(o), NULL) != (double)def) by = "its default";
    snprintf(other, sizeof other, "%d", other_v);
    value_said(o, other, other_said, sizeof other_said);
    ini_option(option, sizeof option, key, other);
    word_of(o, other, word, sizeof word);
    LOG("settings: %s=%s, %s, gives %s %s this run; %s in the game's own settings, the launcher's or %s runs it %s\n",
        key, def_said, by, kind, kind_said, other_said, option, word);
    word_of(o, value, word, sizeof word);
    at = strlen(g_kind.list);
    if (at + (at ? 2 : 0) + strlen(key) + 1 + strlen(word) >= sizeof g_kind.list) { /* left out, never cut */
        WARN("settings: the start's notice has no room for %s %s, which it does not name\n", key, word);
        return 1;
    }
    snprintf(g_kind.list + at, sizeof g_kind.list - at, "%s%s %s", at ? ", " : "", key, word);
    ++g_kind.n;
    return 1;
}

/* the menu's change, or its taking out of the game's pick (unpick), waiting for the next poll; apply: the poll applies
   it */
static void pending_put(const char* k, const char* v, int unpick, int apply) {
    size_t kn = strlen(k), vn = strlen(v);
    int i;
    if (kn >= KEY_MAX || vn >= PENDING_VALUE) {
        WARN_LIMIT(1, "settings: %s's value is too long to save\n", k);
        return;
    }
    pm_lock(&g_pending_lock);
    for (i = 0; i < g_npending && strcmp(g_pending[i].key, k); i++) continue;
    if (i < PENDING_MAX) {
        if (i == g_npending) memcpy(g_pending[g_npending++].key, k, kn + 1);
        memcpy(g_pending[i].value, v, vn + 1);
        g_pending[i].unpick = unpick;
        g_pending[i].apply = apply;
    }
    pm_unlock(&g_pending_lock);
    if (i == PENDING_MAX) WARN_LIMIT(1, "settings: too many changes at once, %s is not saved\n", k);
}
void ini_set(const char* k, const char* v) { pending_put(k, v, 0, 0); }
void ini_set_apply(const char* k, const char* v) {
    pending_put(k, v, 0, 1);
    atomic_store_explicit(&g_picks.poll_soon, 1, memory_order_release);
}
void ini_unpick(const char* k) {
    if (picks_key(k)) pending_put(k, "", 1, 0);
    atomic_store_explicit(&g_picks.poll_soon, 1, memory_order_release);
}
int ini_picked(const char* k) {
    int i, picked = -1, pickable = picks_key(k);
    pm_lock(&g_pending_lock);
    for (i = 0; i < g_npending; i++)
        if (!strcmp(g_pending[i].key, k)) {
            picked = pickable && !g_pending[i].unpick;
            break;
        }
    pm_unlock(&g_pending_lock);
    if (picked >= 0) return picked;
    pm_lock(&g_picks_lock);
    picked = picks_find(k) != NULL;
    pm_unlock(&g_picks_lock);
    return picked;
}
int ini_next_start(const char* k, char* out, size_t n) {
    const Entry* e;
    int i, found = -1;
    if (n) out[0] = 0;
    pm_lock(&g_pending_lock);
    for (i = 0; i < g_npending; i++)
        if (!strcmp(g_pending[i].key, k)) {
            found = !g_pending[i].unpick;
            if (found && n) snprintf(out, n, "%s", g_pending[i].value);
            break;
        }
    pm_unlock(&g_pending_lock);
    if (found >= 0) return found;
    pm_lock(&g_picks_lock);
    e = picks_find(k);
    for (i = 0; !e && i < g_picks.nnext; i++)
        if (!strcmp(g_picks.next[i].key, k)) e = &g_picks.next[i];
    if (e && n) snprintf(out, n, "%s", e->value);
    pm_unlock(&g_picks_lock);
    return e != NULL;
}
const _Atomic int* ini_picks_changes(void) { return &g_picks.changes; }
int ini_poll_due(void) { return atomic_exchange_explicit(&g_picks.poll_soon, 0, memory_order_acq_rel); }
int ini_old_settings(void) { return atomic_load_explicit(&g_picks.old, memory_order_acquire); }
void ini_clean_old(void) {
    atomic_store_explicit(&g_picks.clean_soon, 1, memory_order_release);
    atomic_store_explicit(&g_picks.poll_soon, 1, memory_order_release);
}

static void apply(const char* key, const char* value);
/* key's value for the next start, a setting read at the start whose pick was taken out this run */
static void next_put(const char* key, const char* value) {
    int i;
    if (strlen(key) >= KEY_MAX || strlen(value) >= VALUE_MAX) return; /* a row's: never */
    pm_lock(&g_picks_lock);
    for (i = 0; i < g_picks.nnext && strcmp(g_picks.next[i].key, key); i++) continue;
    if (i < (int)(sizeof g_picks.next / sizeof *g_picks.next)) {
        if (i == g_picks.nnext) memcpy(g_picks.next[g_picks.nnext++].key, key, strlen(key) + 1);
        memcpy(g_picks.next[i].value, value, strlen(value) + 1);
    }
    pm_unlock(&g_picks_lock);
}
/* the menu's change of key over the command line's value of it, which the run plays no longer */
static void override_beaten(const char* key, int beaten) {
    int i = override_at(key);
    if (i >= 0) g_over[i].beaten = beaten;
}
/* the menu's changes taken: a key the launcher's Settings show into the game's picks, another into the store (the
   .ini), each over this run's command line from then on; a pick taken out back to the value without it, applied at
   once. Their copy in taken, for pending_written once the files have them */
static int pending_take(Pending* taken) {
    int n, i;
    pm_lock(&g_pending_lock);
    n = g_npending;
    memcpy(taken, g_pending, (size_t)n * sizeof *taken);
    pm_unlock(&g_pending_lock);
    for (i = 0; i < n; i++) {
        const char* key = taken[i].key;
        const IniOpt* o;
        if (taken[i].unpick) {
            if (picks_find(key)) {
                picks_put(key, NULL);
                g_picks.dirty = 1;
            }
            override_beaten(key, 0);
            if ((o = find_row(key, NULL)) != NULL && value_of(o)) apply(key, value_of(o));
            if (o && g_started && (o->flags & INI_STARTUP) && value_of(o)) next_put(key, value_of(o));
        } else if (picks_key(key)) {
            picks_put(key, taken[i].value);
            g_picks.dirty = 1;
            if (run_value(key)) override_beaten(key, 1);
            if (taken[i].apply && (o = find_row(key, NULL)) != NULL) apply(key, value_of(o));
        } else {
            store_put(key, taken[i].value);
            override_beaten(key, 1);
        }
    }
    return n;
}
/* drops the changes the files now have; one set again meanwhile stays for the next write */
static void pending_written(const Pending* taken, int n) {
    int i, j;
    pm_lock(&g_pending_lock);
    for (i = 0; i < n; i++)
        for (j = 0; j < g_npending; j++)
            if (!strcmp(g_pending[j].key, taken[i].key)) {
                if (g_pending[j].unpick == taken[i].unpick && !strcmp(g_pending[j].value, taken[i].value))
                    g_pending[j] = g_pending[--g_npending];
                break;
            }
    pm_unlock(&g_pending_lock);
}

/* ---- applying ---- */

static void apply(const char* key, const char* value) {
    int own = 0;
    const IniOpt* o;
    if (ini_deprecated(key)) return; /* kept in the file, never read */
    o = find_row(key, &own);
    if (o && g_started && (o->flags & INI_STARTUP)) return;
    if (o && own)
        ini_read(o, value);
    else if (o || unattended_known(key))
        backend_ini(key, value);
}
/* every setting: the command line's value, else the file's, else its default; then the keys of no section the
   unattended options take */
static void apply_all(void) {
    int s, t, i;
    for (s = 0; s < g_ini_sections_count; s++)
        for (t = 0; t < INI_SECTION_TABLES && g_ini_sections[s].table[t]; t++) {
            const IniOpt* o;
            for (o = g_ini_sections[s].table[t]; o->key; o++) {
                const char* v = value_of(o);
                if (v) apply(o->key, v);
            }
        }
    for (i = 0; i < g_nstore; i++)
        if (!find_row(g_store[i].key, NULL) && !override_of(g_store[i].key)) apply(g_store[i].key, g_store[i].value);
    for (i = 0; i < g_nover; i++)
        if (!find_row(g_over[i].key, NULL) && strcmp(g_over[i].key, SNX_LAUNCHER_PICKS))
            apply(g_over[i].key, g_over[i].value);
}

/* ---- writing ---- */

static void put_help(FILE* f, const char* help) {
    const char* nl;
    fputs("; ", f);
    while ((nl = strchr(help, '\n')) != NULL) {
        fprintf(f, "%.*s\n;   ", (int)(nl - help), help);
        help = nl + 1;
    }
    fprintf(f, "%s\n", help);
}
static void put_header(FILE* f) {
    const char* title = g_mod && g_mod->title && g_mod->title[0] ? g_mod->title : g_exename;
    int s, t, col = 0;
    fprintf(f, "; ==================================================================================\n");
    fprintf(f, ";  %s: ScarletNX settings\n", title);
    fprintf(f, ";  Most settings apply while the game runs. These apply when it next starts:\n;   ");
    for (s = 0; s < g_ini_sections_count; s++)
        for (t = 0; t < INI_SECTION_TABLES && g_ini_sections[s].table[t]; t++) {
            const IniOpt* o;
            for (o = g_ini_sections[s].table[t]; o->key; o++) {
                if (!(o->flags & INI_STARTUP)) continue;
                if (col && col + (int)strlen(o->key) + 2 > 70) {
                    fprintf(f, ",\n;   ");
                    col = 0;
                } else if (col) {
                    fprintf(f, ", ");
                    col += 2;
                }
                fputs(o->key, f);
                col += (int)strlen(o->key);
            }
        }
    fprintf(f, "\n;  In the game: F1 help, F2 the game's menu, F3 screenshot, F4 FPS limit off and on, F5 thread\n");
    fprintf(f, ";  dump, F6 showcase on and off, F7 save state, F8 load state, F9 next save state slot");
#if defined(SNX_TRACY)
    fprintf(f, ", F10 a 60-second Tracy capture");
#endif
    fprintf(f, ", F11 or\n;  Alt+Enter fullscreen\n");
    fprintf(f, "; ==================================================================================\n");
}
static void put_rows(FILE* f, const IniOpt* o) {
    for (; o->key; o++) {
        const Entry* e = store_find(o->key);
        const char* v = e ? e->value : o->def;
        if (!v) continue; /* no default and never set */
        if (o->help) put_help(f, o->help);
        fprintf(f, "%s=%s\n", o->key, v);
    }
}
/* 1 when put_rows writes a row of the section: one with a default, or set */
static int section_set(const IniSection* section) {
    int t;
    const IniOpt* o;
    for (t = 0; t < INI_SECTION_TABLES && section->table[t]; t++)
        for (o = section->table[t]; o->key; o++)
            if (o->def || store_find(o->key)) return 1;
    return 0;
}
/* the store into the file: beside it, then moved over it, so the file is always whole. 0, or -1 (warned once) */
static int write_file(void) {
    char tmp[sizeof g_path + 8], why[256];
    FILE* f;
    int s, t, i, other = 0, failed;
    unsigned char* text;
    size_t size;
    snprintf(tmp, sizeof tmp, "%s.tmp", g_path);
    f = fopen(tmp, "wb");
    if (!f) {
        WARN_LIMIT(1, "settings: could not write %s: %s\n", tmp, snx_os_error(why, sizeof why));
        return -1;
    }
    put_header(f);
    for (s = 0; s < g_ini_sections_count; s++) {
        if (!section_set(&g_ini_sections[s])) continue; /* [Game] of a game made from no dump */
        fprintf(f, "\n[%s]\n", g_ini_sections[s].name);
        for (t = 0; t < INI_SECTION_TABLES && g_ini_sections[s].table[t]; t++) {
            if (g_ini_sections[s].page[t]) fprintf(f, "; -- %s --\n", g_ini_sections[s].page[t]);
            put_rows(f, g_ini_sections[s].table[t]);
        }
    }
    for (i = 0; i < g_nstore; i++) /* a newer version's keys, typos, the unattended options: as they are */
        if (!find_row(g_store[i].key, NULL) && !ini_deprecated(g_store[i].key)) {
            if (!other++) fprintf(f, "\n[Other]\n; keys this version does not know, kept as they are\n");
            fprintf(f, "%s=%s\n", g_store[i].key, g_store[i].value);
        }
    for (i = 0, other = 0; i < g_nstore; i++) /* last, so the launcher's Clean Old Settings cuts to the end */
        if (ini_deprecated(g_store[i].key)) {
            if (!other++) ini_deprecated_head(f);
            ini_deprecated_key(f, g_store[i].key, g_store[i].value);
        }
    failed = ferror(f);
    if (fclose(f)) failed = 1;
    if (failed || plat_replace_file(tmp, g_path)) {
        snx_os_error(why, sizeof why); /* before remove's */
        remove(tmp);
        WARN_LIMIT(1, "settings: could not write %s (%s): this change is not saved\n", g_path, why);
        return -1;
    }
    text = snx_read_file(g_path, &size);
    g_hash = g_seen = text ? snx_fnv1a(SNX_FNV_BASIS, text, size) : 0;
    free(text);
    return 0;
}

/* ---- the game's picks file ---- */

static void picks_path(void) {
    char* path = snx_game_picks(g_path);
    snprintf(g_picks.path, sizeof g_picks.path, "%s", path);
    free(path);
}
/* emulated_cores, deprecated for emulated_cores_mode of the same 0 (Off) and 1 (On): the game's pick of it is its pick
   of emulated_cores_mode while the picks give that one none, the file written with it at the next write, said once */
static void picks_emulated_cores_old(void) {
    static const char* const words[] = {"Off", "On"};
    const Entry* e = picks_find("emulated_cores");
    int now;
    if (!e || picks_find("emulated_cores_mode")) return;
    now = options_emulated_cores_old(e->value, 1);
    if (now < 0) return; /* neither 0 nor 1: kept, said as no pick of this version's */
    picks_put("emulated_cores", NULL);
    picks_put("emulated_cores_mode", now ? "1" : "0");
    g_picks.dirty = 1;
    WARN("settings: emulated_cores=%d in %s, deprecated, is emulated_cores_mode=%d (%s) there now "
         "(docs/user/deprecations.md)\n",
         now, g_picks.path, now, words[now]);
}
/* the picks as they are on the disk now, none where there is no file; a key given twice takes its last line's. One no
   pick takes (a newer version's, a typo, a control's place) is said with its value, kept in the file and does
   nothing */
static void picks_load(void) {
    size_t size;
    unsigned char* text = snx_read_file(g_picks.path, &size);
    SnxIni ini;
    char *key, *value;
    int i;
    pm_lock(&g_picks_lock);
    g_picks.n = 0;
    pm_unlock(&g_picks_lock);
    g_picks.hash = g_picks.seen = text ? snx_fnv1a(SNX_FNV_BASIS, text, size) : 0;
    if (text) {
        snx_ini_start(&ini, (char*)text);
        while (snx_ini_next(&ini, &key, &value)) picks_put(key, value);
    }
    free(text);
    picks_emulated_cores_old();
    for (i = 0; i < g_picks.n; i++)
        if (!picks_key(g_picks.entry[i].key))
            WARN("settings: %s=%s in %s is no setting a game's picks take: it is kept there and does nothing\n",
                 g_picks.entry[i].key, g_picks.entry[i].value, g_picks.path);
    atomic_fetch_add_explicit(&g_picks.changes, 1, memory_order_release);
}
/* the picks into their file by the .ini's sections, the keys no pick takes after them as they were; beside it, then
   moved over it. With no pick left the file is removed. 0, or -1 (warned once, the change kept for the next write) */
static int picks_write(void) {
    char tmp[sizeof g_picks.path + 8], why[256];
    FILE* f;
    int s, t, i, other = 0, failed;
    unsigned char* text;
    size_t size;
    if (!g_picks.n) {
        if (remove(g_picks.path) && snx_path_exists(g_picks.path)) {
            WARN_LIMIT(1, "settings: could not remove %s, which has no pick left: %s\n", g_picks.path,
                       snx_os_error(why, sizeof why));
            return -1;
        }
        g_picks.hash = g_picks.seen = 0;
        g_picks.dirty = 0;
        return 0;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", g_picks.path);
    if (!(f = fopen(tmp, "wb"))) {
        WARN_LIMIT(1, "settings: could not write %s: %s\n", tmp, snx_os_error(why, sizeof why));
        return -1;
    }
    fputs(SNX_GAME_PICKS_HEAD, f);
    for (s = 0; s < g_ini_sections_count; s++) {
        int head = 0;
        for (t = 0; t < 3 && g_ini_sections[s].table[t]; t++) {
            const IniOpt* o;
            const Entry* e;
            for (o = g_ini_sections[s].table[t]; o->key; o++)
                if (!(o->flags & INI_IN_GAME) && (e = picks_find(o->key)) != NULL) {
                    if (!head++) fprintf(f, "\n[%s]\n", g_ini_sections[s].name);
                    fprintf(f, "%s=%s\n", e->key, e->value);
                }
        }
    }
    for (i = 0; i < g_picks.n; i++)
        if (!picks_key(g_picks.entry[i].key)) {
            if (!other++) fprintf(f, "\n[Other]\n; keys no pick of this version takes, kept as they are\n");
            fprintf(f, "%s=%s\n", g_picks.entry[i].key, g_picks.entry[i].value);
        }
    failed = ferror(f);
    if (fclose(f)) failed = 1;
    if (failed || plat_replace_file(tmp, g_picks.path)) {
        snx_os_error(why, sizeof why);
        remove(tmp);
        WARN_LIMIT(1, "settings: could not write %s (%s): this game's picks are not saved\n", g_picks.path, why);
        return -1;
    }
    text = snx_read_file(g_picks.path, &size);
    g_picks.hash = g_picks.seen = text ? snx_fnv1a(SNX_FNV_BASIS, text, size) : 0;
    free(text);
    g_picks.dirty = 0;
    return 0;
}
/* the game's picks in the log, each with what this run's command line gives over it */
static void picks_said(void) {
    char line[2048];
    size_t at;
    int i;
    if (!g_picks.n) return;
    at = (size_t)snprintf(line, sizeof line, "settings: this game's own, from %s:", g_picks.path);
    for (i = 0; i < g_picks.n && at < sizeof line; i++) {
        const char* run = picks_key(g_picks.entry[i].key) ? run_value(g_picks.entry[i].key) : NULL;
        at += (size_t)snprintf(line + at, sizeof line - at, "%s %s=%s", i ? "," : "", g_picks.entry[i].key,
                               g_picks.entry[i].value);
        if (run && at < sizeof line)
            at += (size_t)snprintf(line + at, sizeof line - at, " (this run plays %s=%s, its command line's)",
                                   g_picks.entry[i].key, run);
    }
    LOG("%s\n", line);
}
/* the picks file changed on the disk (the launcher's Settings of the game, a hand) and settled: read again and said.
   1 when it was */
static int picks_poll(void) {
    size_t size;
    unsigned char* text = snx_read_file(g_picks.path, &size);
    u64 h = text ? snx_fnv1a(SNX_FNV_BASIS, text, size) : 0;
    free(text);
    if (h == g_picks.hash) return 0;
    if (h != g_picks.seen) { /* maybe still being saved: read at the next poll if it stays so */
        g_picks.seen = h;
        return 0;
    }
    picks_load();
    LOG("settings: %s reloaded\n", g_picks.path);
    picks_said();
    return 1;
}
/* --launcher-picks=1: this run's settings on the command line are the launcher's picks for every game, under the
   game's own. Before the old environment variables, which stay this run's */
static void launcher_picks(void) {
    const char* v = override_of(SNX_LAUNCHER_PICKS);
    int i;
    if (!v || !strcmp(v, "0")) return;
    if (strcmp(v, "1")) {
        WARN("settings: --%s=%s is not 0 or 1: it is 0 this run, the command line's settings over the game's picks\n",
             SNX_LAUNCHER_PICKS, v);
        return;
    }
    for (i = 0; i < g_nover; i++) g_over[i].launcher = strcmp(g_over[i].key, SNX_LAUNCHER_PICKS) != 0;
}

/* the keys Clean Old Settings would take out, counted for the menu's row */
static void old_counted(void) {
    int i, n = 0;
    for (i = 0; i < g_nstore; i++) n += ini_deprecated(g_store[i].key) != NULL;
    for (i = 0; i < g_picks.n; i++) n += !picks_key(g_picks.entry[i].key);
    if (atomic_exchange_explicit(&g_picks.old, n, memory_order_acq_rel) != n) /* the menu's row comes or goes */
        atomic_fetch_add_explicit(&g_picks.changes, 1, memory_order_release);
}
/* Clean Old Settings from the game's menu: the .ini's deprecated keys and the picks' keys no pick takes taken out,
   both files written without them, and each said in the log with its value */
static void clean_old(void) {
    char line[2048];
    size_t at = (size_t)snprintf(line, sizeof line, "settings: Clean Old Settings took out");
    int i, n = 0;
    for (i = 0; i < g_nstore;)
        if (ini_deprecated(g_store[i].key)) {
            if (at < sizeof line)
                at += (size_t)snprintf(line + at, sizeof line - at, "%s %s=%s", n++ ? "," : "", g_store[i].key,
                                       g_store[i].value);
            memmove(&g_store[i], &g_store[i + 1], (size_t)(--g_nstore - i) * sizeof *g_store); /* in their order */
        } else
            i++;
    for (i = 0; i < g_picks.n;)
        if (!picks_key(g_picks.entry[i].key)) {
            if (at < sizeof line)
                at += (size_t)snprintf(line + at, sizeof line - at, "%s %s=%s (%s)", n++ ? "," : "",
                                       g_picks.entry[i].key, g_picks.entry[i].value, g_picks.path);
            picks_put(g_picks.entry[i].key, NULL);
            g_picks.dirty = 1;
        } else
            i++;
    if (!n) return;
    LOG("%s\n", line);
    write_file();
    if (g_picks.dirty) picks_write();
}
/* the store into the .ini and the picks into theirs, then the menu's changes they hold dropped: each file tried, and
   the changes kept for the next poll while either could not be written */
static void files_write(const Pending* taken, int n) {
    int failed = g_picks.dirty && picks_write();
    if (write_file()) failed = 1;
    if (!failed) pending_written(taken, n);
}

/* ini_read's line about a value clamped or left, in the log */
static void say_setting(const char* line) { WARN("%s\n", line); }

int options_memory_fit_old(const char* old, int ios) {
    if (!strcmp(old, "1")) return 0;
    if (!strcmp(old, "0")) return ios ? 1 : -1;
    return -2;
}
/* memory_fit, deprecated for memory_fit_mode, read while the file and the command line give memory_fit_mode no value:
   the value it maps to goes in the store, which the file is written with, so the next run reads memory_fit_mode */
static void memory_fit_old(void) {
    const char* old = ini_deprecated_value("memory_fit");
    int now;
    if (!old || override_of("memory_fit_mode") || store_find("memory_fit_mode")) return;
#if defined(SNX_IOS)
    now = options_memory_fit_old(old, 1);
#else
    now = options_memory_fit_old(old, 0);
#endif
    if (now == -2)
        WARN("settings: memory_fit=%s, deprecated, is neither 0 nor 1: memory_fit_mode takes its default, 0 (Auto)\n",
             old);
    else if (now == -1)
        WARN(
            "settings: memory_fit=%s, deprecated, was its default: memory_fit_mode takes its own, 0 (Auto), which fits "
            "the game to the device (docs/user/deprecations.md)\n",
            old);
    else {
        store_put("memory_fit_mode", now ? "1" : "0");
        WARN("settings: memory_fit=%s, deprecated, is memory_fit_mode=%d (%s) now (docs/user/deprecations.md)\n", old, now,
             now ? "Off" : "Auto");
    }
}

int options_astc_bc_old(const char* old) {
    if (!strcmp(old, "1")) return 0;
    if (!strcmp(old, "0")) return -1;
    return -2;
}
/* astc_bc, deprecated for astc_mode, read while the file and the command line give astc_mode no value, as memory_fit
   is */
static void astc_bc_old(void) {
    const char* old = ini_deprecated_value("astc_bc");
    int now;
    if (!old || override_of("astc_mode") || store_find("astc_mode")) return;
    now = options_astc_bc_old(old);
    if (now == -2)
        WARN("settings: astc_bc=%s, deprecated, is neither 0 nor 1: astc_mode takes its default, 0 (Auto)\n", old);
    else if (now == -1)
        WARN("settings: astc_bc=%s, deprecated, was its default: astc_mode takes its own, 0 (Auto), which holds ASTC "
             "textures as RGBA8 or BC7 by the GPU's memory where the GPU samples BCn but not ASTC "
             "(docs/user/deprecations.md)\n",
             old);
    else {
        store_put("astc_mode", "0");
        WARN("settings: astc_bc=%s, deprecated, is astc_mode=0 (Auto) now (docs/user/deprecations.md)\n", old);
    }
}

/* compress_shader_cache, deprecated: the shader cache is written when the game closes, always packed. Its value
   chooses nothing, and the line says so with it */
static void compress_shader_cache_old(void) {
    const char* old = ini_deprecated_value("compress_shader_cache");
    if (old)
        WARN("settings: compress_shader_cache=%s, deprecated, chooses nothing: the shaders and pipelines a session "
             "makes are held in memory and written when the game closes, packed with zstd at its highest level "
             "(docs/user/deprecations.md)\n",
             old);
}

/* bc7_cache_gb, deprecated with the BC7 cache's file: its value bounds nothing, and the line says what does */
static void bc7_cache_gb_old(void) {
    const char* old = ini_deprecated_value("bc7_cache_gb");
    if (old)
        WARN("settings: bc7_cache_gb=%s, deprecated, bounds nothing: the BC7 cache has no file now, and holds the "
             "ASTC levels used most in memory, up to an eighth of the machine's, from 64 MB to 2048 MB "
             "(docs/user/deprecations.md)\n",
             old);
}

/* unit_cache_gb, deprecated with the units' folder: its value bounds nothing, and the line says what does */
static void unit_cache_gb_old(void) {
    const char* old = ini_deprecated_value("unit_cache_gb");
    if (old)
        WARN("settings: unit_cache_gb=%s, deprecated, bounds nothing: the code units are not kept on the disk now, "
             "and are held in memory while ScarletNX runs, up to 1%% of the machine's memory and at most "
             "unit_cache_ceil_mb (docs/user/deprecations.md)\n",
             old);
}

int options_vk_descriptors_old(const char* old, int given) {
    char* end;
    long v = strtol(old, &end, 10);
    if (end == old || *end || v < 0 || v > 3) return -2;
    return !given && (v == 0 || v == 3) ? -1 : (int)v;
}
/* vk_descriptors, deprecated for vk_descriptors_mode: the file's read while the file gives vk_descriptors_mode no
   value, into the store, which the file is written with, so the next run reads vk_descriptors_mode; the command line's
   this run's vk_descriptors_mode while the command line gives that one none */
static void vk_descriptors_old(void) {
    static const char* const values[] = {"0", "1", "2", "3"};
    static const char* const words[] = {"Sets", "Buffers", "Heaps", "Automatic"};
    const Entry* file = store_find("vk_descriptors");
    const char *given = override_of("vk_descriptors"), *first = override_of("vk_descriptors_mode");
    int now;
    if (file && !store_find("vk_descriptors_mode")) {
        now = options_vk_descriptors_old(file->value, 0);
        if (now == -2)
            WARN(
                "settings: vk_descriptors=%s, deprecated, is not a whole number from 0 to 3: vk_descriptors_mode takes "
                "its default, 3 (Automatic) (docs/user/deprecations.md)\n",
                file->value);
        else if (now == -1)
            WARN("settings: vk_descriptors=%s, deprecated, was its default: vk_descriptors_mode takes its own, 3 "
                 "(Automatic), sets on a GPU with memory of its own and heaps on one that shares the system's "
                 "(docs/user/deprecations.md)\n",
                 file->value);
        else {
            store_put("vk_descriptors_mode", values[now]);
            WARN("settings: vk_descriptors=%s, deprecated, is vk_descriptors_mode=%d (%s) now (docs/user/deprecations.md)\n",
                 file->value, now, words[now]);
        }
    }
    if (!given) return;
    if (first) {
        WARN("settings: --vk_descriptors=%s is deprecated, and the command line gives --vk-descriptors-mode=%s too: "
             "vk_descriptors_mode=%s this run (docs/user/deprecations.md)\n",
             given, first, first);
        return;
    }
    now = options_vk_descriptors_old(given, 1);
    if (now >= 0) {
        ini_override("vk_descriptors_mode", values[now]);
        WARN("settings: --vk_descriptors=%s is deprecated: this run reads it as --vk-descriptors-mode=%d (%s) "
             "(docs/user/deprecations.md)\n",
             given, now, words[now]);
    } else {
        file = store_find("vk_descriptors_mode");
        WARN("settings: --vk_descriptors=%s, deprecated, is not a whole number from 0 to 3: vk_descriptors_mode=%s "
             "this run, %s (docs/user/deprecations.md)\n",
             given, file ? file->value : "3", file ? "the .ini's" : "its default (Automatic)");
    }
}

int options_write_tracking_hot_old(const char* old, int given) {
    if (!strcmp(old, "1")) return 1;
    if (!strcmp(old, "0")) return given ? 0 : -1;
    return -2;
}
/* write_tracking_hot, deprecated for write_tracking_hot_pages: the file's read while the file and the command line give
   write_tracking_hot_pages no value, into the store, which the file is written with, so the next run reads
   write_tracking_hot_pages; the command line's this run's write_tracking_hot_pages while the command line gives that
   one none */
static void write_tracking_hot_old(void) {
    static const char* const words[] = {"Off", "On", "Auto"};
    const char *old = ini_deprecated_value("write_tracking_hot"), *given = override_of("write_tracking_hot"),
               *first = override_of("write_tracking_hot_pages");
    if (!given) given = override_of("write-tracking-hot"); /* the name, which the old key no longer has */
    int now;
    if (old && !first && !store_find("write_tracking_hot_pages")) {
        now = options_write_tracking_hot_old(old, 0);
        if (now == -2)
            WARN("settings: write_tracking_hot=%s, deprecated, is neither 0 nor 1: write_tracking_hot_pages takes its "
                 "default, 2 (Auto) (docs/user/deprecations.md)\n",
                 old);
        else if (now == -1)
            WARN(
                "settings: write_tracking_hot=%s, deprecated, was its default: write_tracking_hot_pages takes its own, "
                "2 (Auto), hot pages where page protection tracks the writes (docs/user/deprecations.md)\n",
                old);
        else {
            store_put("write_tracking_hot_pages", "1");
            WARN("settings: write_tracking_hot=%s, deprecated, is write_tracking_hot_pages=1 (On) now "
                 "(docs/user/deprecations.md)\n",
                 old);
        }
    }
    if (!given) return;
    if (first) {
        WARN(
            "settings: --write_tracking_hot=%s is deprecated, and the command line gives --write-tracking-hot-pages=%s "
            "too: write_tracking_hot_pages=%s this run (docs/user/deprecations.md)\n",
            given, first, first);
        return;
    }
    now = options_write_tracking_hot_old(given, 1);
    if (now >= 0) {
        ini_override("write_tracking_hot_pages", now ? "1" : "0");
        WARN("settings: --write_tracking_hot=%s is deprecated: this run reads it as --write-tracking-hot-pages=%d (%s) "
             "(docs/user/deprecations.md)\n",
             given, now, words[now]);
    } else {
        const Entry* file = store_find("write_tracking_hot_pages");
        WARN("settings: --write_tracking_hot=%s, deprecated, is neither 0 nor 1: write_tracking_hot_pages=%s this run, "
             "%s (docs/user/deprecations.md)\n",
             given, file ? file->value : "2", file ? "the .ini's" : "its default (Auto)");
    }
}

int options_emulated_cores_old(const char* old, int given) {
    if (!strcmp(old, "0")) return 0;
    if (!strcmp(old, "1")) return given ? 1 : -1;
    return -2;
}
/* emulated_cores, deprecated for emulated_cores_mode: the file's read while the file gives emulated_cores_mode no value,
   into the store, which the file is written with, so the next run reads emulated_cores_mode; the command line's (the
   launcher's for every game too, kept its) this run's emulated_cores_mode while the command line gives that one none */
static void emulated_cores_old(void) {
    static const char* const values[] = {"0", "1"};
    static const char* const words[] = {"Off", "On"};
    const Entry* file = store_find("emulated_cores");
    const char *old = "emulated_cores", *given = override_of(old), *first = override_of("emulated_cores_mode");
    const IniOpt* o = find_row("emulated_cores_mode", NULL);
    int now, launcher;
    if (!given) given = override_of(old = "emulated-cores"); /* the name, which the old key no longer has */
    if (file && !store_find("emulated_cores_mode")) {
        now = options_emulated_cores_old(file->value, 0);
        if (now == -2)
            WARN(
                "settings: emulated_cores=%s, deprecated, is neither 0 nor 1: emulated_cores_mode takes its default, 2 "
                "(Auto) (docs/user/deprecations.md)\n",
                file->value);
        else if (now == -1)
            WARN(
                "settings: emulated_cores=%s, deprecated, was its default: emulated_cores_mode takes its own, 2 (Auto), "
                "Off for an Ounce program and On for the others (docs/user/deprecations.md)\n",
                file->value);
        else {
            store_put("emulated_cores_mode", values[now]);
            WARN("settings: emulated_cores=%s, deprecated, is emulated_cores_mode=%d (%s) now "
                 "(docs/user/deprecations.md)\n",
                 file->value, now, words[now]);
        }
    }
    if (!given) return;
    if (first) {
        WARN("settings: --emulated_cores=%s is deprecated, and the command line gives --emulated-cores-mode=%s too: "
             "emulated_cores_mode=%s this run (docs/user/deprecations.md)\n",
             given, first, first);
        return;
    }
    now = options_emulated_cores_old(given, 1);
    if (now < 0) {
        const char* by = given_by("emulated_cores_mode");
        WARN("settings: --emulated_cores=%s, deprecated, is neither 0 nor 1: emulated_cores_mode=%s this run, %s "
             "(docs/user/deprecations.md)\n",
             given, value_of(o), by ? by : "its default (Auto)");
        return;
    }
    launcher = g_over[override_at(old)].launcher;
    ini_override("emulated_cores_mode", values[now]);
    if (override_at("emulated_cores_mode") >= 0) g_over[override_at("emulated_cores_mode")].launcher = launcher;
    WARN("settings: %s--emulated_cores=%s is deprecated: this run reads it as --emulated-cores-mode=%d (%s) "
         "(docs/user/deprecations.md)\n",
         launcher ? "the launcher's " : "", given, now, words[now]);
}

static void say_deprecated(void) {
    const char* keys[STORE_MAX];
    char line[2048];
    int i, n = 0;
    for (i = 0; i < g_nstore; i++)
        if (ini_deprecated(g_store[i].key)) keys[n++] = g_store[i].key;
    if (ini_deprecated_note(line, sizeof line, g_path, keys, n)) WARN("%s\n", line);
}

int options_debug(int flags, char* out, size_t n) {
    int count = ini_changed(g_debug_ini, flags, out, n);
    size_t at = strlen(out);
    if (g_opt_gdb && at < n) snprintf(out + at, n - at, "%sgdbstub", count++ ? ", " : "");
    return count;
}
int options_settings(char* out, size_t n) {
    const IniOpt* const tables[] = {g_display_ini, g_graphics_ini, g_audio_ini, g_performance_ini};
    char part[1024];
    size_t at = 0, t;
    int count = 0;
    if (n) out[0] = 0;
    for (t = 0; t < sizeof tables / sizeof *tables; t++) {
        int c = ini_changed(tables[t], INI_CHANGED_DEFAULT, part, sizeof part);
        if (c && at < n) at += (size_t)snprintf(out + at, n - at, "%s%s", count ? ", " : "", part);
        count += c;
    }
    return count;
}
/* the value v a layer of value_of gave o is another than o's default: a value of o's that the runtime then changed (this
   kind of program's, options_kind_value) is not the player's. A row with no default is a once text's, empty by default */
static int layer_differs(const IniOpt* o, const char* v) {
    if (!o->def) return v[0] != 0;
    if (o->type == INI_TEXT) return strcmp(v, o->def) != 0;
    return strtod(v, NULL) != strtod(o->def, NULL);
}
/* key appended to out at *at, after ", " but for the first */
static void key_put(char* out, size_t n, size_t* at, const char* key) {
    if (*at < n) *at += (size_t)snprintf(out + *at, n - *at, "%s%s", *at ? ", " : "", key);
}
/* options_perf_notice's lists: the perf rows of the .ini's sections off their default, by the layer of value_of that
   gave each, the game's (its picks, its .ini) into game, the launcher's and this run's command line's into global */
static int perf_changed(char* game, size_t game_n, char* global, size_t global_n) {
    size_t at[2] = {0, 0};
    int s, t, count = 0;
    pm_lock(&g_picks_lock);
    for (s = 0; s < g_ini_sections_count; s++)
        for (t = 0; t < 3 && g_ini_sections[s].table[t]; t++)
            for (const IniOpt* o = g_ini_sections[s].table[t]; o->key; o++) {
                const Entry* e = !(o->flags & INI_IN_GAME) ? picks_find(o->key) : NULL;
                const char* v = run_value(o->key);
                int ours = 0;
                if (!(o->flags & INI_PERF) || ini_at_default(o)) continue;
                if (!v && e) {
                    v = e->value;
                    ours = 1;
                } else if (!v && !(v = launcher_value(o->key)) && (e = store_find(o->key)) != NULL) {
                    v = e->value;
                    ours = 1;
                }
                if (!v || !layer_differs(o, v)) continue;
                if (ours)
                    key_put(game, game_n, &at[0], o->key);
                else
                    key_put(global, global_n, &at[1], o->key);
                ++count;
            }
    pm_unlock(&g_picks_lock);
    return count;
}
int options_perf_notice(const char* base) {
    char game[1024] = "", global[1024] = "", *off = NULL;
    SnxGameMade made;
    const IniOpt* opt = ini_find(g_recompile_ini, "opt");
    size_t at;
    int count = perf_changed(game, sizeof game, global, sizeof global);
    count += g_kind.n;
    if (snx_game_made_read(base, &made)) off = snx_game_made_off(base);
    at = strlen(global);
    /* the code made ahead of time at another level than opt's default: what runs, whatever opt is now */
    if (made.aot == 1 && opt && opt->def && made.opt != opt->def[0]) {
        key_put(global, sizeof global, &at, "opt");
        ++count;
    }
    for (char* k = off; k && *k;) {
        size_t len = strcspn(k, ",");
        char key[KEY_MAX];
        snprintf(key, sizeof key, "%.*s", (int)(len < sizeof key ? len : sizeof key - 1), k);
        key_put(global, sizeof global, &at, key);
        ++count;
        k += len + (k[len] == ',');
    }
    free(off);
    overlay_settings_notice(game, global, g_kind.list);
    return count;
}

/* the Display, Graphics, Audio and Performance settings as the log last said them */
static char g_said[2048];
/* after a reload, the Display, Graphics, Audio and Performance settings again when they changed */
static void settings_reloaded(void) {
    char now[sizeof g_said];
    int n = options_settings(now, sizeof now);
    if (!strcmp(now, g_said)) return;
    if (n)
        LOG("settings: after the reload, %s\n", now);
    else
        LOG("settings: after the reload, every Display, Graphics, Audio and Performance setting is at its default\n");
    memcpy(g_said, now, sizeof g_said);
}
/* the Debug settings in effect at the top of the picture; after a reload that changes them, in the log too */
static void debug_shown(int reloaded) {
    char list[HUD_DEBUG_MAX];
    options_debug(INI_CHANGED_NO_TEXT, list, sizeof list);
    if (!hud_debug_set(list) || !reloaded) return;
    if (options_debug(INI_CHANGED_DEFAULT, list, sizeof list))
        LOG("debug: after the reload, %s\n", list);
    else
        LOG("debug: after the reload, every Debug setting is at its default\n");
}

void ini_open(const char* path) {
    Pending taken[PENDING_MAX];
    unsigned char* text;
    size_t size;
    int i, n;
    char why[256];
    snprintf(g_path, sizeof g_path, "%s", path);
    g_kind.list[0] = 0;
    g_kind.n = 0;
    picks_path();
    ini_set_say(say_setting);
    /* --vk_descriptors, --write_tracking_hot and --emulated_cores are read as their new keys, said there */
    for (i = 0; i < g_nover; i++)
        if (!snx_setting_known(g_over[i].key) && strcmp(g_over[i].key, "vk_descriptors") &&
            strcmp(g_over[i].key, "write_tracking_hot") && strcmp(g_over[i].key, "write-tracking-hot") &&
            strcmp(g_over[i].key, "emulated_cores") && strcmp(g_over[i].key, "emulated-cores"))
            WARN("--%s is not a setting, so it does nothing\n", g_over[i].key);
    launcher_picks();
    options_old_environment();
    picks_load();
    text = snx_read_file(g_path, &size);
    if (!text) snx_os_error(why, sizeof why);
    if (!text && snx_path_exists(g_path)) {
        WARN("settings: cannot read %s (%s): every setting takes its default, and the file is not written over\n",
             g_path, why);
        vk_descriptors_old(); /* the command line's */
        write_tracking_hot_old();
        emulated_cores_old();
        apply_all();
        picks_said();
        options_old_environment_say();
        debug_shown(0);
        options_settings(g_said, sizeof g_said);
        old_counted();
        g_started = 1;
        return;
    }
    if (text) {
        store_parse((char*)text);
        g_hash = g_seen = snx_fnv1a(SNX_FNV_BASIS, text, size);
        free(text);
    }
    memory_fit_old();
    astc_bc_old();
    bc7_cache_gb_old();
    unit_cache_gb_old();
    compress_shader_cache_old();
    vk_descriptors_old();
    write_tracking_hot_old();
    emulated_cores_old();
    n = pending_take(taken);
    say_deprecated();
    apply_all();
    picks_said();
    options_old_environment_say();
    debug_shown(0);
    options_settings(g_said, sizeof g_said);
    files_write(taken, n);
    old_counted();
    g_started = 1;
}

const char* ini_file(void) { return g_path; }

void ini_poll(int final) {
    Pending taken[PENDING_MAX];
    unsigned char* text;
    size_t size;
    int pending, n, picks;
    u64 h;
    if (!g_path[0]) return;
    pm_lock(&g_poll_lock);
    pm_lock(&g_pending_lock);
    pending = g_npending;
    pm_unlock(&g_pending_lock);
    picks = picks_poll();
    if (atomic_exchange_explicit(&g_picks.clean_soon, 0, memory_order_acq_rel)) clean_old();
    text = snx_read_file(g_path, &size);
    h = text ? snx_fnv1a(SNX_FNV_BASIS, text, size) : 0;
    if (text && h != g_hash && (final ? pending : h == g_seen)) {
        /* changed, and settled (seen the same a second before), or the menu's changes are to be saved at exit */
        store_parse((char*)text);
        n = pending_take(taken);
        apply_all();
        LOG("settings: %s reloaded\n", g_path);
        debug_shown(1);
        settings_reloaded();
        files_write(taken, n);
    } else {
        if (text && h != g_hash)
            g_seen = h; /* changed: maybe still being saved, read at the next poll if it stays so */
        else if (pending && (text || !snx_path_exists(g_path))) {
            /* the menu's changes; the file made again when it is gone, and left alone while it cannot be read */
            n = pending_take(taken);
            files_write(taken, n);
        }
        if (picks) {
            apply_all();
            debug_shown(1);
            settings_reloaded();
        }
    }
    old_counted();
    free(text);
    pm_unlock(&g_poll_lock);
}

/* ---- the listing for the launcher ---- */

void ini_list(const IniSection* sections, int count) {
    size_t n = ini_list_text(sections, count, NULL, 0);
    char* text = malloc(n + 1);
    if (!text) return;
    ini_list_text(sections, count, text, n + 1);
    fputs(text, stdout);
    free(text);
}
