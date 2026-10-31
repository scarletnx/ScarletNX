/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The game's .ini: the sections it is written in (the runtime's own settings here, the window's, the on-screen
   controls' and the gamepads' from their modules), the command line's overrides over it, its reload while the game
   runs, and the values the game's menu changes, written back */
#include "nx/runtime/options.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "nx/hle/hle.h"
#include "nx/hle/kernel/kernel.h"
#include "nx/hle/services/services.h"
#include "nx/memory/mem.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/loader.h"
#include "nx/runtime/main.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/hud.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/platform/vpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_opt_fps = SNX_FPS_GAME;

/* ---- the runtime's own settings ---- */

/* a language the console doesn't have is English (US), as the clamp to the table's range would not say */
void options_language_set(const IniOpt* o, const char* v) {
    char* end;
    long n = strtol(v, &end, 10);
    (void)o;
    if (end != v && (n < 0 || n >= set_language_count())) g_settings.language = 1;
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
        __atomic_store_n(&g_opt_fps, SNX_FPS_GAME, __ATOMIC_RELAXED);
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
    __atomic_store_n(&g_opt_fps, fps, __ATOMIC_RELAXED);
}
void fps_limit_text(int fps, char* out, size_t n) {
    if (fps == SNX_FPS_GAME)
        snprintf(out, n, "game");
    else
        snprintf(out, n, "%d", fps);
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
        for (t = 0; t < 3 && g_ini_sections[s].table[t]; t++) {
            const IniOpt* o = ini_find(g_ini_sections[s].table[t], key);
            if (o) {
                if (own) *own = g_ini_sections[s].own;
                return o;
            }
        }
    return NULL;
}

int snx_setting_known(const char* key) { return find_row(key, NULL) || unattended_known(key); }

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
/* the command line's, set before ini_open and only read after */
typedef struct {
    char key[KEY_MAX];
    const char* value;
} Override;
static Override g_over[OVERRIDE_MAX];
static int g_nover;
/* the menu's, from any thread, until a write of the file has them. g_pending_lock is a leaf: nothing is called while
   it is held */
typedef struct {
    char key[KEY_MAX];
    char value[PENDING_VALUE];
} Pending;
static Pending g_pending[PENDING_MAX];
static int g_npending;
static PMutex g_pending_lock = PMUTEX_INIT;
static PMutex g_poll_lock = PMUTEX_INIT;

static char g_path[1400];
static int g_started; /* ini_open is done: the startup settings are kept from now on */
static u64 g_hash;    /* the file as this run last read or wrote it */
static u64 g_seen;    /* a changed file seen by the last poll, read once a second poll sees it unchanged */

static u64 fnv1a(const unsigned char* p, size_t n) {
    u64 h = 0xCBF29CE484222325ull;
    size_t i;
    for (i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static const char* override_of(const char* key) {
    int i;
    for (i = 0; i < g_nover; i++)
        if (!strcmp(g_over[i].key, key)) return g_over[i].value;
    return NULL;
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
static void store_parse(char* text) {
    SnxIni ini;
    char *key, *value;
    g_nstore = 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) store_put(key, value);
}
static const char* value_of(const IniOpt* o) {
    const char* v = override_of(o->key);
    const Entry* e;
    if (v) return v;
    e = store_find(o->key);
    return e ? e->value : o->def;
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

void ini_set(const char* k, const char* v) {
    size_t kn = strlen(k), vn = strlen(v);
    int i;
    if (override_of(k)) return; /* this run's value, not the file's */
    if (kn >= KEY_MAX || vn >= PENDING_VALUE) {
        WARN_LIMIT(1, "settings: %s's value is too long to save\n", k);
        return;
    }
    pm_lock(&g_pending_lock);
    for (i = 0; i < g_npending && strcmp(g_pending[i].key, k); i++) continue;
    if (i < PENDING_MAX) {
        if (i == g_npending) memcpy(g_pending[g_npending++].key, k, kn + 1);
        memcpy(g_pending[i].value, v, vn + 1);
    }
    pm_unlock(&g_pending_lock);
    if (i == PENDING_MAX) WARN_LIMIT(1, "settings: too many changes at once, %s is not saved\n", k);
}

/* the menu's changes into the store: their copy in taken, for pending_written once the file has them */
static int pending_take(Pending* taken) {
    int n, i;
    pm_lock(&g_pending_lock);
    n = g_npending;
    memcpy(taken, g_pending, (size_t)n * sizeof *taken);
    pm_unlock(&g_pending_lock);
    for (i = 0; i < n; i++) store_put(taken[i].key, taken[i].value);
    return n;
}
/* drops the changes the file now has; one set again meanwhile stays for the next write */
static void pending_written(const Pending* taken, int n) {
    int i, j;
    pm_lock(&g_pending_lock);
    for (i = 0; i < n; i++)
        for (j = 0; j < g_npending; j++)
            if (!strcmp(g_pending[j].key, taken[i].key)) {
                if (!strcmp(g_pending[j].value, taken[i].value)) g_pending[j] = g_pending[--g_npending];
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
        for (t = 0; t < 3 && g_ini_sections[s].table[t]; t++) {
            const IniOpt* o;
            for (o = g_ini_sections[s].table[t]; o->key; o++) {
                const char* v = value_of(o);
                if (v) apply(o->key, v);
            }
        }
    for (i = 0; i < g_nstore; i++)
        if (!find_row(g_store[i].key, NULL) && !override_of(g_store[i].key)) apply(g_store[i].key, g_store[i].value);
    for (i = 0; i < g_nover; i++)
        if (!find_row(g_over[i].key, NULL)) apply(g_over[i].key, g_over[i].value);
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
        for (t = 0; t < 3 && g_ini_sections[s].table[t]; t++) {
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
    for (t = 0; t < 3 && section->table[t]; t++)
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
        for (t = 0; t < 3 && g_ini_sections[s].table[t]; t++) {
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
    g_hash = g_seen = text ? fnv1a(text, size) : 0;
    free(text);
    return 0;
}

/* ini_read's line about a value clamped or left, in the log */
static void say_setting(const char* line) { WARN("%s\n", line); }

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
    ini_set_say(say_setting);
    for (i = 0; i < g_nover; i++)
        if (!snx_setting_known(g_over[i].key)) WARN("--%s is not a setting, so it does nothing\n", g_over[i].key);
    text = snx_read_file(g_path, &size);
    if (!text) snx_os_error(why, sizeof why);
    if (!text && snx_path_exists(g_path)) {
        WARN("settings: cannot read %s (%s): every setting takes its default, and the file is not written over\n",
             g_path, why);
        apply_all();
        debug_shown(0);
        g_started = 1;
        return;
    }
    if (text) {
        store_parse((char*)text);
        g_hash = g_seen = fnv1a(text, size);
        free(text);
    }
    n = pending_take(taken);
    say_deprecated();
    apply_all();
    debug_shown(0);
    if (!write_file()) pending_written(taken, n);
    g_started = 1;
}

const char* ini_file(void) { return g_path; }

void ini_poll(int final) {
    Pending taken[PENDING_MAX];
    unsigned char* text;
    size_t size;
    int pending, n;
    u64 h;
    if (!g_path[0]) return;
    pm_lock(&g_poll_lock);
    pm_lock(&g_pending_lock);
    pending = g_npending;
    pm_unlock(&g_pending_lock);
    text = snx_read_file(g_path, &size);
    if (!text) {
        /* unreadable: left alone. Gone: made again when there is something to save */
        if (!snx_path_exists(g_path) && pending) {
            n = pending_take(taken);
            if (!write_file()) pending_written(taken, n);
        }
        pm_unlock(&g_poll_lock);
        return;
    }
    h = fnv1a(text, size);
    if (h != g_hash && (final ? pending : h == g_seen)) {
        /* changed, and settled (seen the same a second before), or the menu's changes are to be saved at exit */
        store_parse((char*)text);
        n = pending_take(taken);
        apply_all();
        LOG("settings: %s reloaded\n", g_path);
        debug_shown(1);
        if (!write_file()) pending_written(taken, n);
    } else if (h != g_hash)
        g_seen = h; /* changed: maybe still being saved, read at the next poll if it stays so */
    else if (pending) {
        n = pending_take(taken);
        if (!write_file()) pending_written(taken, n);
    }
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
