/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/settings.h"
#include "3ds/runtime/main.h"
#include "common/utilities/bits.h"
#include "common/utilities/common.h"
#include "common/utilities/config_file.h"
#include "common/utilities/game_ini.h"
#include "common/platform/file.h"
#include "common/utilities/ini_lines.h"
#include "common/utilities/vpad_consoles.h"
#include "gui/i18n.h"
#include "snx_settings.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static GuiSetting* row(GuiSettings* s, const char* section, const char* key, const char* name, const char* about,
                       const char* const* words, int type) {
    GuiThemeKey* k = &s->keys[s->count];
    GuiSetting* v = &s->values[s->count++];
    k->section = section;
    k->key = name && *name ? name : key;
    k->about = about;
    k->words = words;
    k->type = type;
    k->at = type == GUI_KEY_WORD ? (size_t)((char*)&v->word - (char*)s->values)
                                 : (size_t)((char*)v->text - (char*)s->values);
    k->size = type == GUI_KEY_WORD ? sizeof v->word : sizeof v->text;
    k->max = 0;
    k->numeric = k->whole = 0;
    k->lo = k->hi = 0;
    k->def = NULL;
    v->key = key;
    return v;
}
/* a row's values, value=word between '|', split in place: its words (.ini first, a game's Global) from *pool, NULL
   when one has no =. values gets them by word. Each list keeps a slot after its last word for a value none of them is
   (GuiSetting's custom), NULL until one is set */
static const char** words_of(char* list, const char*** pool, const char*** values, int game) {
    const char** words = *pool;
    int n = 1, i;
    char* c;
    for (c = list; *c; ++c) n += *c == '|';
    *values = words + n + 3;
    words[0] = game ? SNX_TR_NOOP("Global") : ".ini";
    (*values)[0] = NULL;
    words[n + 1] = words[n + 2] = (*values)[n + 1] = NULL;
    for (i = 1, c = list; i <= n; ++i) {
        char *end = c + strcspn(c, "|"), *equals;
        if (*end) *end++ = 0;
        equals = strchr(c, '=');
        if (!equals) return NULL;
        *equals = 0;
        (*values)[i] = c;
        words[i] = equals + 1;
        c = end;
    }
    *pool += 2 * n + 5;
    return words;
}
/* a row of boxes' items, value=Label~About between '|', split in place: into v, its three arrays from *pool */
static void items_of(char* list, const char*** pool, GuiSetting* v) {
    int n = 1, i;
    char* c;
    for (c = list; *c; ++c) n += *c == '|';
    v->item_value = *pool;
    v->item_label = *pool + n;
    v->item_about = *pool + 2 * n;
    *pool += 3 * n;
    for (i = 0, c = list; i < n; ++i) {
        char *end = c + strcspn(c, "|"), *equals, *about;
        if (*end) *end++ = 0;
        v->item_value[i] = v->item_label[i] = c;
        v->item_about[i] = "";
        if ((about = strchr(c, '~')) != NULL) {
            *about++ = 0;
            v->item_about[i] = about;
        }
        if ((equals = strchr(c, '=')) != NULL) {
            *equals = 0;
            v->item_label[i] = equals + 1;
        }
        c = end;
    }
    v->items = n;
}
/* row i's words and values past the .ini's, the first: its values alone, its word the same value's */
static void plain(GuiSettings* s, int i) {
    ++s->keys[i].words;
    ++s->values[i].values;
    s->values[i].plain = 1;
}
/* 1 when mark is one of the words of a column, a space between two */
static int has_mark(const char* column, const char* mark) {
    size_t n = strlen(mark);
    for (const char* c = column; (c = strstr(c, mark)) != NULL; c += n)
        if ((c == column || c[-1] == ' ') && (!c[n] || c[n] == ' ')) return 1;
    return 0;
}
#define SHOWN_KEY(name, id) "controls_" id "_shown",
static const char* const SHOWN[SNX_VPAD_COUNT] = {SNX_VPAD_IDS(SHOWN_KEY)};
/* a setting of the on-screen controls or of the touch screen, by its key (settings.txt's g_vpad_ini, g_vpad_shown_ini
   and g_backend_ini) */
static int controls_key(const char* key) {
    return !strcmp(key, "touch") || (!strncmp(key, "controls", 8) && (!key[8] || key[8] == '_'));
}
/* the row of key, named name, in the own Settings of a game of a console with the on-screen controls pad: its name
   there, a control's the console's own for it, NULL for one the console has nothing for */
static const char* controls_name(const SnxVpadConsole* pad, const char* key, const char* name) {
    int sticks = 0;
    if (!controls_key(key)) return NULL;
    if (!strcmp(key, "touch")) return pad->touch ? name : NULL;
    for (int i = 0; i < pad->count; ++i) {
        if (!strcmp(key, SHOWN[pad->pad[i].control])) return pad->pad[i].name;
        sticks += pad->pad[i].control == SNX_VPAD_STICK_L || pad->pad[i].control == SNX_VPAD_STICK_R;
    }
    if (!strcmp(key, "controls_rel_center")) return sticks ? name : NULL;
    for (int i = 0; i < SNX_VPAD_COUNT; ++i)
        if (!strcmp(key, SHOWN[i])) return NULL;
    return name;
}
/* the on-screen controls of a game's console, NULL for the Switch's and the 3DS's */
static const SnxVpadConsole* pad_of(int game) {
    return game == GUI_GAME_GBA    ? &snx_vpad_gba
           : game == GUI_GAME_NDS  ? &snx_vpad_nds
           : game == GUI_GAME_VITA ? &snx_vpad_vita
                                   : NULL;
}
/* the rows of the runtime's list; with game, a game's own, of that console (gui_settings_read_console) */
static int read_list(GuiSettings* s, const char* runtime, int game) {
    /* the launcher's own rows and the recompiler's (settings.txt's launcher sections), listed as the runtime lists the
       game's, then the runtime's; a game's the runtime's alone */
    size_t own = game ? 0 : ini_list_text(g_launch_sections, g_launch_sections_count, NULL, 0), size,
           n = 3 + GUI_SETTING_LINKS, bars = 0, pool;
    const char* c;
    const char** free_words;
    char *line, *next, *text, *page = NULL, *list, *own_end;
    GuiSetting *link = NULL, *all = NULL, *v;
    int w;
    gui_settings_free(s);
    size = own + strlen(runtime) + 1;
    list = malloc(size);
    if (!list) return 1;
    if (!game) ini_list_text(g_launch_sections, g_launch_sections_count, list, own + 1);
    memcpy(list + own, runtime, strlen(runtime) + 1);
    for (c = list; *c; ++c) { /* a row a line, a page's own two before its first; and the words */
        n += *c == '\n' ? 3 : 0;
        bars += *c == '|';
    }
    /* a row's words and values, 2 * words + 5 (words_of), or its items' three (items_of): a line's 3 rows hold the 7 */
    pool = 3 * bars + 10 * n;
    if (n > (SIZE_MAX - size) / (sizeof *s->keys + sizeof *s->values + 10 * sizeof(char*)) ||
        bars > (SIZE_MAX - size) / 6 / sizeof(char*)) {
        free(list);
        return 1;
    }
    s->keys = malloc(n * (sizeof *s->keys + sizeof *s->values) + pool * sizeof(char*) + size);
    if (!s->keys) {
        free(list);
        return 1;
    }
    s->values = (GuiSetting*)(s->keys + n);
    free_words = (const char**)(s->values + n);
    text = memcpy(free_words + pool, list, size);
    free(list);
    own_end = text + own;
    memset(s->values, 0, n * sizeof *s->values);
    for (line = text; *line; line = next) {
        char* field[12];
        if (!game && line >= own_end && !s->own) s->own = s->count; /* the launcher's rows end there */
        const char **words = NULL, **values = NULL;
        int f = 1;
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        field[0] = line;
        while (f < 12 && (field[f] = strchr(field[f - 1], '\t')) != NULL) *field[f++]++ = 0;
        if (f < 7) continue; /* not a setting: another line the runtime printed */
        if (game == GUI_GAME_3DS && !snx_ctr_game_setting(field[1])) continue;
        const char* name = f < 8 ? "" : field[7];
        if (pad_of(game) && !(name = controls_name(pad_of(game), field[1], name))) continue;
        for (; f < 12; ++f) field[f] = line + strlen(line); /* an older runtime's, with no last columns */
        if (page && strcmp(page, field[0])) page = NULL;
        if (!page && strchr(field[0], '/')) { /* a page: its row in the section, and its All first on it */
            page = field[0];
            link = row(s, NULL, strchr(page, '/') + 1, NULL, SNX_TR_NOOP("Its own page, as in the game's menu"), NULL,
                       GUI_KEY_ACTION);
            snprintf(link->option, sizeof link->option, "%.*s", (int)(strchr(page, '/') - page), page);
            s->keys[s->count - 1].section = link->option;
            snprintf(link->text, sizeof link->text, ">");
            link->kind = GUI_SETTING_PAGE;
            link->first = s->count;
            all = row(s, page, SNX_TR_NOOP("All"), NULL, SNX_TR_NOOP("Every row of this page at once"), NULL,
                      GUI_KEY_ACTION);
            all->kind = GUI_SETTING_ALL;
            all->first = s->count;
        }
        int boxes = *field[8] && has_mark(field[11], "boxes");
        if (*field[8] && !boxes) words = words_of(field[8], &free_words, &values, game);
        v = row(s, field[0], field[1], name, field[6], words, words ? GUI_KEY_WORD : GUI_KEY_TEXT);
        if (boxes) items_of(field[8], &free_words, v);
        v->values = values;
        v->type_text = !strcmp(field[3], "text");
        v->number = field[3][0] == 'i' || field[3][0] == 'f' ? field[3][0] : 0;
        v->lo = strtod(field[4], NULL);
        v->hi = strtod(field[5], NULL);
        v->only = *field[9] ? field[9] : NULL;
        v->needs_build = !strcmp(field[10], "needs-build");
        if (!words && !boxes &&
            (field[3][0] == 'i' || field[3][0] == 'f')) { /* typed, not picked: a number of its range */
            GuiThemeKey* k = &s->keys[s->count - 1];
            k->numeric = 1;
            k->whole = field[3][0] == 'i';
            k->lo = strtod(field[4], NULL);
            k->hi = strtod(field[5], NULL);
        }
        v->fallback = v->def = field[2];
        s->keys[s->count - 1].def = v->def;                /* what a number's row shows when it is emptied */
        for (w = 1; words && words[w]; ++w) /* the word of the default's value, which the row starts on */
            if (!strcmp(values ? values[w] : words[w], field[2])) {
                v->fallback = words[w];
                v->word = w;
            }
        if (values && v->word && game == GUI_GAME_3DS) /* its default is what Global gives, and no word of its own */
            for (w = v->word; words[w]; ++w) words[w] = words[w + 1], values[w] = values[w + 1];
        if (values && v->word && game) /* a game's starts at Global, which every row has */
            v->word = 0;
        else if (values && v->word) { /* its default a word: no .ini's besides, which would be that value twice */
            plain(s, s->count - 1);
            --v->word;
        }
        /* the twelfth column's marks: slider, its default a step as the runtime's list makes it, and open */
        v->slider = v->plain && has_mark(field[11], "slider");
        v->open = has_mark(field[11], "open");
        if (!game && !page && !strcmp(field[1], "room")) { /* the Eden room's password beside its address */
            GuiSetting* secret =
                row(s, field[0], "room_password", SNX_TR_NOOP("Room password"),
                    SNX_TR_NOOP("The Eden room's password, kept in users/online.ini, which only you can "
                                "read; never in the launcher's settings nor on a run's line"),
                    NULL, GUI_KEY_ACTION);
            secret->kind = GUI_SETTING_SECRET;
            v = &s->values[s->count - 2];
        }
        v->path = has_mark(field[11], "folder") ? 'd'
                  : has_mark(field[11], "file") ? 'f'
                  : has_mark(field[11], "save") ? 's'
                                                : 0;
        if (page) {
            link->count = s->count - link->first;
            all->count = s->count - all->first;
        }
    }
    if (!s->own && !game) s->own = s->count; /* no runtime listed any */
    s->game = game;
    gui_settings_sync(s);
    return 0;
}
int gui_settings_read(GuiSettings* s, const char* runtime) { return read_list(s, runtime, 0); }
int gui_settings_read_game(GuiSettings* s, const char* runtime) { return read_list(s, runtime, GUI_GAME_SWITCH); }
int gui_settings_read_console(GuiSettings* s, const char* runtime, int console) {
    return read_list(s, runtime, console);
}
int gui_settings_link(GuiSettings* s, int i, const char* name, const char* about, const char* value) {
    GuiThemeKey* k;
    GuiSetting* v;
    if (!s->keys || i < 0 || i >= GUI_SETTING_LINKS) return -1;
    k = &s->keys[s->count + i];
    v = &s->values[s->count + i];
    memset(k, 0, sizeof *k);
    memset(v, 0, sizeof *v);
    k->section = "";
    k->key = name;
    k->about = about;
    k->type = GUI_KEY_ACTION;
    k->at = (size_t)((char*)v->text - (char*)s->values);
    k->size = sizeof v->text;
    v->key = name;
    v->kind = GUI_SETTING_LINK;
    snprintf(v->text, sizeof v->text, "%s", value ? value : "");
    return s->count + i;
}
void gui_settings_free(GuiSettings* s) {
    for (int i = 0; i < s->count; ++i) free(s->values[i].grown);
    free(s->keys);
    memset(s, 0, sizeof *s);
}
static int find(const GuiSettings* s, const char* key) {
    int i;
    for (i = 0; i < s->count && (s->values[i].kind || strcmp(s->values[i].key, key)); ++i) {}
    return i;
}
/* row i's value as a file of picks keeps it: a word row's value (.ini for none), a typed row's text */
static const char* kept(const GuiSettings* s, int i) {
    const GuiSetting* v = &s->values[i];
    if (s->keys[i].type == GUI_KEY_ACTION) return "";
    if (s->keys[i].type == GUI_KEY_TEXT) return v->text;
    return v->values && (v->word || v->plain) ? v->values[v->word] : s->keys[i].words[v->word];
}
/* row i's pick, NULL for the .ini's (the launcher's: off) and a plain row's default; an always row's word */
static const char* picked(const GuiSettings* s, int i) {
    const GuiSetting* v = &s->values[i];
    if (v->needs_build) return NULL; /* shown greyed: this build does nothing with its value */
    if (s->keys[i].type == GUI_KEY_WORD && v->plain)
        return v->always || !v->def || strcmp(kept(s, i), v->def) ? kept(s, i) : NULL;
    if (s->keys[i].type == GUI_KEY_WORD) return v->word ? kept(s, i) : NULL;
    return s->keys[i].type == GUI_KEY_TEXT && v->text[0] ? v->text : NULL;
}
/* whether value is one row i keeps beside its words: a number in its range, a text where it is open */
static int custom_fits(const GuiSetting* v, const char* value) {
    char* end;
    double x;
    if (!*value || strlen(value) >= sizeof v->text || (!v->open && !v->slider)) return 0;
    if (!v->number) return !strchr(value, '|');
    x = strtod(value, &end);
    if (end == value || *end || x != x || x < v->lo || x > v->hi) return 0;
    return v->number == 'f' || x == (double)(long long)x;
}
/* row i set to value, none of its words, kept in its slot after them (words_of) and shown as itself: 1 when it did */
static int set_custom(GuiSettings* s, int i, const char* value) {
    GuiSetting* v = &s->values[i];
    const char** words = (const char**)s->keys[i].words; /* words_of's pool, which the row's keys point into */
    const char** values = (const char**)v->values;
    int slot = v->custom;
    if (!values || !custom_fits(v, value)) return 0;
    if (!slot)
        while (words[slot]) ++slot;
    snprintf(v->text, sizeof v->text, "%s", value);
    words[slot] = values[slot] = v->text;
    v->custom = v->word = slot;
    return 1;
}
/* row i set to value, a value of its words or a text: 1 when it did */
static int set(GuiSettings* s, int i, const char* value) {
    GuiSetting* v = &s->values[i];
    int w;
    if (v->needs_build) return 0; /* shown greyed: this build does nothing with its value, so it takes none */
    if (s->keys[i].type != GUI_KEY_WORD || !v->values) return gui_theme_set(s->values, &s->keys[i], value);
    for (w = 0; s->keys[i].words[w]; ++w)
        if (!strcmp(value, w || v->plain ? v->values[w] : s->keys[i].words[0])) {
            v->word = w;
            return 1;
        }
    /* the default no word has (off, the launcher's): no pick, which a plain row does not have */
    if (!v->plain && v->def && *v->def && !strcmp(value, v->def)) {
        v->word = 0;
        return 1;
    }
    return set_custom(s, i, value);
}
void gui_settings_mark(GuiSettings* s) {
    int i;
    for (i = 0; i < s->count; ++i) snprintf(s->values[i].start, sizeof s->values[i].start, "%s", kept(s, i));
}
int gui_settings_always(GuiSettings* s, const char* key, const char* value) {
    int i = find(s, key), w;
    GuiSetting* v = i >= s->own && i < s->count ? &s->values[i] : NULL; /* the runtime's: a run's option */
    if (!v || s->keys[i].type != GUI_KEY_WORD || !v->values) return 0;
    if (!v->plain) { /* value one of its words past the .ini's, which it no longer has */
        for (w = 1; s->keys[i].words[w] && strcmp(value, v->values[w]); ++w) {}
        if (!s->keys[i].words[w]) return 0;
        plain(s, i);
    }
    if (!set(s, i, value)) return 0;
    v->always = 1;
    return 1;
}
/* 1 when the file of picks at path gives key a value */
static int file_gives(const char* path, const char* key) {
    size_t bytes;
    char *text = (char*)snx_read_file(path, &bytes), *k, *v;
    int found = 0;
    SnxIni ini;
    if (!text) return 0;
    snx_ini_start(&ini, text);
    while (!found && snx_ini_next(&ini, &k, &v)) found = !strcmp(k, key);
    free(text);
    return found;
}
static void settings_left(void* ctx, int line, const char* text, const char* does) {
    (void)text;
    (void)does;
    snx_config_other_line((SnxConfigOthers*)ctx, line);
}
/* one more of what a load says, "; " after the one before */
static void said_more(char* old, size_t size, size_t* n, int* count, const char* fmt, ...) {
    va_list ap;
    if (*n < size) *n += (size_t)snprintf(old + *n, size - *n, "%s", *count ? "; " : "");
    va_start(ap, fmt);
    if (*n < size) *n += (size_t)vsnprintf(old + *n, size - *n, fmt, ap);
    va_end(ap);
    ++*count;
}
int gui_settings_load(GuiSettings* s, const char* path, char* old, size_t size) {
    size_t bytes, n = 0;
    char *text = NULL, *key, *value, why[1024], said[2048];
    int count = 0, i, *line_of;
    const char** value_of;
    SnxIni ini;
    SnxConfigOthers others = {0};
    if (size) old[0] = 0;
    if (!path) return 0;
    if (snx_config_read(path, &text, &bytes, why, sizeof why) < 0) { /* said, and never written over (save) */
        if (size) snprintf(old, size, "%s", why);
        return 1;
    }
    if (!text) return 0;
    line_of = (int*)calloc((size_t)s->count + 1, sizeof *line_of);
    value_of = (const char**)calloc((size_t)s->count + 1, sizeof *value_of);
    snx_ini_start(&ini, text); /* a key names one row whatever its section, as in the game's .ini */
    ini.left = settings_left;
    ini.ctx = &others;
    while (snx_ini_next(&ini, &key, &value)) {
        int gone, given, moved = 0, taken = 0, fits, setok = 0;
        const char* to = NULL;
        i = find(s, key);
        if (i == s->count && (to = ini_renamed(key)) != NULL) { /* a renamed key's pick: its new row's while the file
                                                                   gives that one none */
            int row = find(s, to);
            moved = row < s->count && !file_gives(path, to);
            if (moved) i = row;
            taken = moved && set(s, i, value);
        } else if (i == s->count && !ini_deprecated(key)) /* another version's or program's: kept, named */
            snx_config_other_key(&others, "", key, ini.line);
        if (!moved && i < s->count && line_of && value_of) { /* the last line's, said with both */
            if (line_of[i]) snx_config_twice(&others, "", key, value_of[i], line_of[i], value, ini.line);
            line_of[i] = ini.line;
            value_of[i] = value;
        }
        gone = !moved && i < s->count && s->values[i].plain && !strcmp(value, ".ini"); /* passed nothing: not a value */
        if (gone) set(s, i, s->values[i].start);
        if (!moved && !gone && i < s->count) setok = gui_settings_set(s, key, value);
        /* a row this build does nothing with takes no value, and is no value it refuses */
        fits = setok || moved || gone || i == s->count || s->values[i].needs_build;
        /* the default picked, which runs were given over the game's .ini: no pick now */
        given = !moved && !gone && setok && s->values[i].plain && !picked(s, i);
        if (gone)
            said_more(old, size, &n, &count, snx_tr("%s=.ini is no longer a value of %s: %s=%s now"), key,
                      snx_tr(s->keys[i].key), key, s->values[i].start);
        else if (given)
            said_more(old, size, &n, &count,
                      snx_tr("%s=%s is the default of %s: no run is given it over the game's .ini"), key, value,
                      snx_tr(s->keys[i].key));
        else if (taken)
            said_more(old, size, &n, &count, snx_tr("%s=%s is deprecated: %s is %s=%s now"), key, value,
                      snx_tr(s->keys[i].key), to, kept(s, i));
        else if (moved)
            said_more(old, size, &n, &count, snx_tr("%s=%s is deprecated and no value of %s: %s=%s now"), key, value,
                      snx_tr(s->keys[i].key), to, kept(s, i));
        else if (!fits) /* the row keeps what it had */
            said_more(old, size, &n, &count, snx_tr("%s=%s is not a value of %s: %s=%s now"), key, value,
                      snx_tr(s->keys[i].key), key, kept(s, i));
    }
    if (snx_config_others_said(&others, path, said, sizeof said)) /* without the file's name, which the caller says */
        said_more(old, size, &n, &count, "%s", said + strlen(path) + 2);
    free(line_of);
    free((void*)value_of);
    free(text);
    gui_settings_sync(s);
    return count;
}
/* the key of a row of s, which a write of its file makes; with clean, an older version's too, which it takes out */
typedef struct {
    const GuiSettings* s;
    int clean;
} RowOwns;
static int row_owns(void* ctx, const char* section, const char* key) {
    const RowOwns* o = (const RowOwns*)ctx;
    (void)section;
    /* a renamed key's value goes to its new row (gui_settings_load), which a write makes instead */
    return find(o->s, key) < o->s->count || ini_renamed(key) || (o->clean && ini_deprecated(key));
}
/* the rows that are not at their start, by section, then the lines of the keys no row has, as they were (an older
   version's ones too, but with clean) */
static int settings_write(const GuiSettings* s, const char* path, int clean) {
    size_t room = 256 + (size_t)s->count * 200, at = 0, size = 0; /* a row's section, key, value: < 200 */
    char *old = NULL, *top = NULL, *rest = NULL, *text, why[1024];
    const char* section = "";
    RowOwns owns = {s, clean};
    int i, failed, rows = 0;
    if (snx_config_read(path, &old, &size, why, sizeof why) < 0) return 1; /* what it holds is not known */
    if (old) {
        top = snx_config_kept(old, size, 1, row_owns, &owns);
        rest = snx_config_kept(old, size, 0, row_owns, &owns);
    }
    room += size + (top ? strlen(top) : 0) + (rest ? strlen(rest) : 0);
    text = (char*)malloc(room);
    if (text) /* a game's own picks file starts as the runtime writes it */
        at = (size_t)snprintf(text, room, "%s%s",
                              s->game ? SNX_GAME_PICKS_HEAD
                                      : "; what the launcher's Settings changed, for every game it runs: over each "
                                        "game's .ini\n",
                              top ? top : "");
    for (i = 0; text && i < s->count; ++i) {
        const char* value = kept(s, i);
        if (!strcmp(value, s->values[i].start)) continue;
        if (strcmp(section, s->keys[i].section))
            at += (size_t)snprintf(text + at, room - at, "[%s]\n", section = s->keys[i].section);
        at += (size_t)snprintf(text + at, room - at, "%s=%s\n", s->values[i].key, value);
        ++rows;
    }
    if (text && rest) at += (size_t)snprintf(text + at, room - at, "%s", rest);
    if (s->game && !rows && (!top || !*top) && (!rest || !*rest)) /* a game's with no pick left: none */
        failed = snx_path_exists(path) && remove(path) != 0;
    else
        failed = !text || snx_config_write(path, ".new", text, at, why, sizeof why);
    free(rest);
    free(top);
    free(old);
    free(text);
    return failed;
}
int gui_settings_save(const GuiSettings* s, const char* path) { return settings_write(s, path, 0); }
int gui_settings_clean(const GuiSettings* s, const char* path) { return settings_write(s, path, 1); }
void gui_settings_reset(GuiSettings* s, int row) {
    int i, first = row < 0 ? 0 : row, end = row < 0 ? s->count : row + 1;
    if (row >= 0 && row < s->count && s->values[row].kind) { /* a page's or an All's: their rows */
        first = s->values[row].first;
        end = first + s->values[row].count;
    }
    for (i = first; i < end && i < s->count; ++i) set(s, i, s->values[i].start);
    gui_settings_sync(s);
}
int gui_settings_at_start(const GuiSettings* s, int row) {
    int i, first = row, end = row + 1;
    if (row < 0 || row >= s->count) return 1;
    if (s->values[row].kind) { /* a page's or an All's: their rows */
        first = s->values[row].first;
        end = first + s->values[row].count;
    }
    for (i = first; i < end && i < s->count; ++i)
        if (strcmp(kept(s, i), s->values[i].start)) return 0;
    return 1;
}
int gui_settings_changed(const GuiSettings* s) {
    int i, n = 0;
    for (i = 0; i < s->count; ++i)
        n += strcmp(kept(s, i), s->values[i].start) != 0; /* an action's, a page's or an All's is always "" */
    return n;
}
int gui_settings_set(GuiSettings* s, const char* key, const char* value) {
    int i = find(s, key);
    return i < s->count && set(s, i, value);
}
int gui_settings_unread(GuiSettings* s, const char* path, char* out, size_t size) {
    size_t n = 0, bytes;
    char *text = path ? (char*)snx_read_file(path, &bytes) : NULL, *key, *value;
    int count = 0;
    SnxIni ini;
    if (size) out[0] = 0;
    if (!text) return 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        /* an older version's key no row has: another version's or program's is kept (gui_settings_load) */
        if (find(s, key) < s->count || (!ini_deprecated(key) && !ini_renamed(key))) continue;
        if (n < size) n += (size_t)snprintf(out + n, size - n, "%s%s=%s", count ? ", " : "", key, value);
        ++count;
    }
    free(text);
    return count;
}
int gui_settings_add_words(GuiSettings* s, int row, const char* const* values, int count) {
    GuiSetting* v = row >= 0 && row < s->count ? &s->values[row] : NULL;
    const char **words, **grown;
    char now[GUI_SETTING_TEXT];
    size_t bytes = 0, at;
    int base, n, i, k;
    char* text;
    if (!v || !v->open || s->keys[row].type != GUI_KEY_WORD || !v->values) return 0;
    words = (const char**)s->keys[row].words;
    for (base = 0; words[base]; ++base) {}
    if (v->custom) --base; /* its last, kept in its place after the others */
    if (v->grown) base = v->base;
    snprintf(now, sizeof now, "%s", v->word || v->plain ? v->values[v->word] : "");
    for (i = 0; i < count; ++i) bytes += strlen(values[i]) + 1;
    /* its words and its values, each with the custom one's slot and the NULL, then the values' text */
    at = 2 * ((size_t)base + (size_t)count + 2);
    grown = (const char**)malloc(at * sizeof *grown + bytes);
    if (!grown) return 0;
    text = (char*)(grown + at);
    for (i = 0; i < base; ++i) {
        grown[i] = words[i];
        grown[base + count + 2 + i] = v->values[i];
    }
    for (n = base, i = 0; i < count; ++i) {
        for (k = v->plain ? 0 : 1; k < n; ++k) /* past the .ini's or Global's, which has no value */
            if (!strcmp(grown[base + count + 2 + k], values[i])) break;
        if (k < n || !*values[i] || strlen(values[i]) >= sizeof v->text) continue;
        strcpy(text, values[i]);
        grown[n] = grown[base + count + 2 + n] = text;
        text += strlen(text) + 1;
        ++n;
    }
    for (i = n; i < base + count + 2; ++i) grown[i] = grown[base + count + 2 + i] = NULL;
    free(v->grown);
    v->grown = grown;
    v->base = base;
    s->keys[row].words = grown;
    v->values = grown + base + count + 2;
    if (v->word || v->plain) { /* the same value, a word now or the custom one again */
        int was = v->word;
        v->custom = 0;
        for (k = v->plain ? 0 : 1; k < n && strcmp(v->values[k], now); ++k) {}
        if (k < n)
            v->word = k;
        else if (!set_custom(s, row, now))
            v->word = was < n ? was : 0;
    } else
        v->custom = 0;
    return 1;
}
/* the next word of *at, a set's separated by commas or spaces (or a sum's digits): its length, 0 at the end */
static size_t set_word(const char** at) {
    const char* c = *at + strspn(*at, ", \t");
    size_t n = strcspn(c, ", \t");
    *at = c;
    return n;
}
/* the item of v the word word of n bytes names, -1 for none */
static int item_of(const GuiSetting* v, const char* word, size_t n) {
    int i;
    for (i = 0; i < v->items; ++i)
        if (strlen(v->item_value[i]) == n && !strncmp(v->item_value[i], word, n)) return i;
    return -1;
}
/* what the row's set is: value, else base, else its default */
static const char* set_value(const GuiSetting* v, const char* base) {
    return v->text[0] ? v->text : base ? base : v->def ? v->def : "";
}
static int set_has(const GuiSetting* v, const char* value, int item) {
    const char* c = value;
    size_t n;
    if (v->number) { /* a sum of bits */
        long bit = strtol(v->item_value[item], NULL, 10);
        return (strtol(value, NULL, 10) & bit) != 0;
    }
    for (; (n = set_word(&c)) != 0; c += n)
        if (item_of(v, c, n) == item) return 1;
    return 0;
}
int gui_settings_item_on(const GuiSettings* s, int row, int item, const char* base) {
    const GuiSetting* v = row >= 0 && row < s->count ? &s->values[row] : NULL;
    return v && item >= 0 && item < v->items && set_has(v, set_value(v, base), item);
}
int gui_settings_item_toggle(GuiSettings* s, int row, int item, const char* base) {
    GuiSetting* v = row >= 0 && row < s->count ? &s->values[row] : NULL;
    char text[GUI_SETTING_TEXT] = "", now[GUI_SETTING_TEXT];
    const char* c;
    size_t n, at = 0;
    int on;
    if (!v || item < 0 || item >= v->items || v->needs_build) return 0;
    snprintf(now, sizeof now, "%s", set_value(v, base));
    on = !set_has(v, now, item);
    if (v->number) {
        long sum = strtol(now, NULL, 10) ^ strtol(v->item_value[item], NULL, 10);
        snprintf(text, sizeof text, "%ld", sum);
    } else {
        for (c = now; (n = set_word(&c)) != 0; c += n) { /* every word but this item's, as it was */
            if (item_of(v, c, n) == item) continue;
            if (at + n + 2 > sizeof text) return 0;
            at += (size_t)snprintf(text + at, sizeof text - at, "%s%.*s", at ? "," : "", (int)n, c);
        }
        if (on) {
            if (at + strlen(v->item_value[item]) + 2 > sizeof text) return 0; /* no room for another: nothing changes */
            snprintf(text + at, sizeof text - at, "%s%s", at ? "," : "", v->item_value[item]);
        }
    }
    /* nothing on is the default's or the launcher's again: no value of the row's own */
    snprintf(v->text, sizeof v->text, "%s", !strcmp(text, v->number ? "0" : "") ? "" : text);
    return 1;
}
void gui_settings_items_said(const GuiSettings* s, int row, const char* value, char* out, size_t size) {
    const GuiSetting* v = &s->values[row];
    int on = 0, others = 0, i;
    size_t n;
    const char* c = value;
    if (v->number) {
        long sum = strtol(value, NULL, 10), known = 0;
        for (i = 0; i < v->items; ++i) known |= strtol(v->item_value[i], NULL, 10);
        for (i = 0; i < v->items; ++i) on += (sum & strtol(v->item_value[i], NULL, 10)) != 0;
        others = (sum & ~known) != 0;
    } else
        for (; (n = set_word(&c)) != 0; c += n) {
            if (item_of(v, c, n) >= 0)
                ++on;
            else
                ++others;
        }
    if (!on && !others)
        snprintf(out, size, "%s", snx_tr("None"));
    else if (others)
        snprintf(out, size, snx_tr("%d on, %d other"), on, others);
    else
        snprintf(out, size, snx_tr("%d on"), on);
}
int gui_settings_step_number(GuiSettings* s, int row, int step) {
    const GuiThemeKey* k = row >= 0 && row < s->count ? &s->keys[row] : NULL;
    GuiSetting* v = k ? &s->values[row] : NULL;
    double now, to;
    char text[64];
    if (!k || k->type != GUI_KEY_TEXT || !k->numeric || v->needs_build) return 0;
    now = strtod(v->text[0] ? v->text : v->def ? v->def : "0", NULL);
    if (k->whole)
        to = now + (step < 0 ? -1 : 1);
    else
        to = ini_float_step(now, k->lo, k->hi, step);
    to = to < k->lo ? k->lo : to > k->hi ? k->hi : to;
    if (to == now) return 0;
    snprintf(text, sizeof text, k->whole ? "%.0f" : "%g", to);
    snprintf(v->text, sizeof v->text, "%s", text);
    return 1;
}
/* row's steps, its words: 0 for a row that is no slider's */
static int steps(const GuiSettings* s, int row) {
    int n = 0;
    if (row < 0 || row >= s->count || !s->values[row].slider) return 0;
    while (s->keys[row].words[n]) ++n;
    return s->values[row].custom ? s->values[row].custom : n; /* a value no step has comes after them */
}
/* the steps of a slider's row past value, a number its steps' values are in order of: how many */
static int steps_below(const GuiSettings* s, int row, double value) {
    int n = steps(s, row), k = 0;
    while (k < n && strtod(s->values[row].values[k], NULL) < value) ++k;
    return k;
}
int gui_settings_slide(GuiSettings* s, int row, int step) {
    int n = steps(s, row), to, below;
    if (!n) return 0;
    if (s->values[row].custom && s->values[row].word == s->values[row].custom) { /* off its steps: the next one */
        below = steps_below(s, row, strtod(s->values[row].text, NULL));
        s->values[row].word = snx_clamp_int(step < 0 ? below - 1 : below, 0, n - 1);
        return 1;
    }
    to = snx_clamp_int(s->values[row].word + (step < 0 ? -1 : 1), 0, n - 1);
    if (to == s->values[row].word) return 0;
    s->values[row].word = to;
    return 1;
}
int gui_settings_slide_to(GuiSettings* s, int row, int at) {
    int n = steps(s, row), to;
    if (!n) return 0;
    to = (snx_clamp_int(at, 0, 1000) * (n - 1) + 500) / 1000;
    if (to == s->values[row].word) return 0;
    s->values[row].word = to;
    return 1;
}
int gui_settings_knob(const GuiSettings* s, int row) {
    int n = steps(s, row);
    if (n > 1 && s->values[row].custom && s->values[row].word == s->values[row].custom) { /* by its value */
        double first = strtod(s->values[row].values[0], NULL), last = strtod(s->values[row].values[n - 1], NULL);
        double at = (strtod(s->values[row].text, NULL) - first) * 1000.0 / (last - first);
        return last > first ? snx_clamp_int((int)(at + 0.5), 0, 1000) : 0;
    }
    return n > 1 ? s->values[row].word * 1000 / (n - 1) : 0;
}
int gui_settings_step_all(GuiSettings* s, int row, int step) {
    const GuiSetting* all = row >= 0 && row < s->count ? &s->values[row] : NULL;
    char value[64];
    int i, n = 0, to;
    if (!all || all->kind != GUI_SETTING_ALL || !all->count || s->keys[all->first].type != GUI_KEY_WORD) return 0;
    while (s->keys[all->first].words[n]) ++n;
    to = (s->values[all->first].word + n + (step < 0 ? -1 : 1)) % n; /* the first row's next word, for them all */
    s->values[all->first].word = to;
    snprintf(value, sizeof value, "%s", kept(s, all->first));
    for (i = all->first + 1; i < all->first + all->count; ++i) set(s, i, value);
    gui_settings_sync(s);
    return 1;
}
/* 1 when a row of an All's is picked */
static int any_picked(const GuiSettings* s, const GuiSetting* all) {
    int i;
    for (i = all->first; i < all->first + all->count; ++i)
        if (picked(s, i)) return 1;
    return 0;
}
void gui_settings_sync(GuiSettings* s) {
    int r, i;
    for (r = 0; r < s->count; ++r) {
        GuiSetting* all = &s->values[r];
        char first[64], word[64];
        if (all->kind != GUI_SETTING_ALL || !all->count) continue;
        gui_theme_get(s->values, &s->keys[all->first], first, sizeof first);
        for (i = all->first + 1; i < all->first + all->count; ++i) {
            gui_theme_get(s->values, &s->keys[i], word, sizeof word);
            if (strcmp(word, first)) break;
        }
        /* rows at their defaults, which differ in their words: the .ini's, [default] */
        snprintf(all->text, sizeof all->text, "%s",
                 i == all->first + all->count ? first
                 : any_picked(s, all)         ? snx_tr("Mixed")
                                              : ".ini");
    }
}
void gui_settings_shown(const GuiSettings* s, int row, char* out, size_t size) {
    const GuiSetting* v = &s->values[row];
    if (v->items) {
        char said[64];
        gui_settings_items_said(s, row, v->text[0] ? v->text : v->def ? v->def : "", said, sizeof said);
        snprintf(out, size, "%s%s%s", said, picked(s, row) ? "" : " ", picked(s, row) ? "" : snx_tr("[default]"));
        return;
    }
    if (v->kind == GUI_SETTING_ALL && !strcmp(v->text, ".ini"))
        snprintf(out, size, "%s", snx_tr("[default]"));
    else if (v->kind == GUI_SETTING_ALL) /* the word its rows share, [default] while each is at its default */
        snprintf(out, size, "%s%s%s", v->text, any_picked(s, v) ? "" : " ",
                 any_picked(s, v) ? "" : snx_tr("[default]"));
    else if (v->always) { /* both its words go to a run: [default] marks the one it starts with */
        int start = !strcmp(kept(s, row), v->start);
        gui_theme_get(s->values, &s->keys[row], out, size);
        if (start) snprintf(out + strlen(out), size - strlen(out), " %s", snx_tr("[default]"));
    } else if (!v->fallback || picked(s, row))
        gui_theme_get(s->values, &s->keys[row], out, size);
    else
        snprintf(out, size, "%s%s%s", v->fallback, v->fallback[0] ? " " : "", snx_tr("[default]"));
}
void gui_settings_shown_game(const GuiSettings* game, const GuiSettings* global, int row, char* out, size_t size) {
    const GuiSetting* v = &game->values[row];
    int g = global ? find(global, v->key ? v->key : "") : 0;
    char shown[GUI_SETTING_TEXT + 64];
    if (v->kind || picked(game, row)) {
        gui_settings_shown(game, row, out, size);
        return;
    }
    if (global && g < global->count)
        gui_settings_shown(global, g, shown, sizeof shown);
    else
        snprintf(shown, sizeof shown, "%s", v->fallback ? v->fallback : "");
    snprintf(out, size, snx_tr("Global (%s)"), shown);
}
const char* gui_settings_value(const GuiSettings* s, const char* key) {
    int i = find(s, key);
    return i < s->count ? picked(s, i) : NULL;
}
int gui_settings_game_own(const char* ini, char* out, size_t size) {
    size_t at = 0, bytes;
    char *path = ini ? snx_game_picks(ini) : NULL, *text = path ? (char*)snx_read_file(path, &bytes) : NULL, *key,
         *value, *own;
    int count = 0;
    SnxIni lines;
    if (size) out[0] = 0;
    if (text) {
        snx_ini_start(&lines, text);
        while (snx_ini_next(&lines, &key, &value)) {
            if (at < size) at += (size_t)snprintf(out + at, size - at, "%s%s=%s", count ? ", " : "", key, value);
            ++count;
        }
    }
    /* the GPU driver and Freedreno's variables the game's box picks for it, in its .ini */
    if (ini && snx_ini_has(ini, "gpu_driver")) {
        own = snx_ini_value(ini, "gpu_driver");
        if (at < size)
            at += (size_t)snprintf(out + at, size - at, "%sgpu_driver=%s", count ? ", " : "", own ? own : "");
        ++count;
        free(own);
    }
    if (ini && (own = snx_ini_value(ini, "freedreno_env")) != NULL) {
        if (at < size) snprintf(out + at, size - at, "%sfreedreno_env", count ? ", " : "");
        ++count;
        free(own);
    }
    free(text);
    free(path);
    return count;
}
/* one of the launcher's own rows turned on (its value "on"), as its file of picks keeps it */
static int own_on(const GuiSettings* s, const char* key) {
    int i = find(s, key);
    const char* value = i < s->own ? picked(s, i) : NULL;
    return value && !strcmp(value, "on");
}
/* a and b the same value: the same number when both are one, else the same text */
static int same_value(const char* a, const char* b) {
    char *a_end, *b_end;
    double x = strtod(a, &a_end), y = strtod(b, &b_end);
    if (a_end != a && !*a_end && b_end != b && !*b_end) return x == y;
    return !strcmp(a, b);
}
/* row i in the Debug section, or a page of it */
static int debug_row(const GuiSettings* s, int i) {
    const char* section = s->keys[i].section;
    return !s->values[i].kind && section && !strncmp(section, "Debug", 5) && (!section[5] || section[5] == '/');
}
/* the values a file of keys gives the rows, the last one's of a key given twice as the runtime reads it, into by_row
   (count of them); the file's text they point into, which the caller frees, NULL for none */
static char* file_by_row(const GuiSettings* s, const char* path, const char** by_row) {
    size_t bytes;
    char *text = path && by_row ? (char*)snx_read_file(path, &bytes) : NULL, *key, *value;
    SnxIni lines;
    int i;
    if (!text) return NULL;
    snx_ini_start(&lines, text);
    while (snx_ini_next(&lines, &key, &value))
        if ((i = find(s, key)) < s->count) by_row[i] = value;
    return text;
}
int gui_settings_debug(const GuiSettings* s, const char* ini, int picks, char* out, size_t size) {
    size_t at = 0;
    /* by row: the .ini's values, then the game's picks' */
    const char** file = s->count ? (const char**)calloc(2 * (size_t)s->count, sizeof *file) : NULL;
    const char** own = file ? file + s->count : NULL;
    char *own_path = ini ? snx_game_picks(ini) : NULL, *text = file_by_row(s, ini, file),
         *own_text = file_by_row(s, own_path, own);
    int count = 0, i;
    if (size) out[0] = 0;
    for (i = 0; i < s->count; ++i) {
        const char* now = own && own[i] ? own[i] : picks ? picked(s, i) : NULL;
        const GuiSetting* v = &s->values[i];
        if (!debug_row(s, i) || !v->def) continue;
        if (!now) now = file && file[i] ? file[i] : v->def;
        if (same_value(now, v->def)) continue;
        if (at < size && v->type_text)
            at += (size_t)snprintf(out + at, size - at, "%s%s", count ? ", " : "", v->key);
        else if (at < size)
            at += (size_t)snprintf(out + at, size - at, "%s%s=%s", count ? ", " : "", v->key, now);
        ++count;
    }
    if (picks && own_on(s, "gdbstub")) {
        if (at < size) snprintf(out + at, size - at, "%sgdbstub", count ? ", " : "");
        ++count;
    }
    free(file);
    free(text);
    free(own_text);
    free(own_path);
    return count;
}
const char* gui_settings_tmpfs(const GuiSettings* s) { return own_on(s, "tmpfs") ? "--tmpfs" : NULL; }
const char* gui_settings_extract(const GuiSettings* s) { return own_on(s, "extract") ? "--extract-container" : NULL; }
/* row i's value a run or a recompile is given: the game's own pick of a setting the recompile takes, which reads its
   settings from its options alone (picks: the game's picks file, NULL for none), else the launcher's pick; NULL for
   none. A value of the game's is kept in the row's option, which the caller writes over with its --key=value */
static const char* given(GuiSettings* s, int i, const char* picks) {
    char* own =
        picks && ini_find(g_recompile_game_ini, s->values[i].key) ? snx_ini_value(picks, s->values[i].key) : NULL;
    if (!own) return picked(s, i);
    snprintf(s->values[i].option, sizeof s->values[i].option, "%s", own);
    free(own);
    return s->values[i].option;
}
/* row i's --key=value in its option, for argv */
static const char* option_of(GuiSettings* s, int i, const char* value) {
    char v[sizeof s->values[i].option];
    snprintf(v, sizeof v, "%s", value); /* value may be the option itself (given) */
    ini_option(s->values[i].option, sizeof s->values[i].option, s->values[i].key, v);
    return s->values[i].option;
}
size_t gui_settings_recompile(GuiSettings* s, int make, const char* game_ini, const char** argv) {
    char* picks = make && game_ini ? snx_game_picks(game_ini) : NULL;
    size_t n = 0;
    int i;
    for (i = 0; i < s->own; ++i) {
        const char* value = s->values[i].kind ? NULL : picked(s, i);
        const char* key = s->values[i].key;
        /* the launcher's own tables' rows are its own to act on, no recompile's option */
        if (!value || ini_find(g_launch_ini, key) || ini_find(g_gba_ini, key)) continue;
        ini_option(s->values[i].option, sizeof s->values[i].option, key, value);
        argv[n++] = s->values[i].option;
    }
    /* -r reads the game's settings it recompiles with from its options alone: a run gets them with the game's */
    for (i = s->own; make && i < s->count; ++i) {
        const char* value;
        if (s->values[i].kind || !ini_find(g_recompile_game_ini, s->values[i].key)) continue;
        if ((value = given(s, i, picks)) != NULL) argv[n++] = option_of(s, i, value);
    }
    free(picks);
    return n;
}
size_t gui_settings_recompile_shared(GuiSettings* s, const char** argv) {
    static const char* const shared[] = {"aot_threads", "aot_threads_most"};
    size_t n = 0;
    for (size_t k = 0; k < sizeof shared / sizeof *shared; ++k) {
        int i = find(s, shared[k]);
        const char* value = i < s->own ? picked(s, i) : NULL;
        if (!value) continue;
        ini_option(s->values[i].option, sizeof s->values[i].option, shared[k], value);
        argv[n++] = s->values[i].option;
    }
    return n;
}
size_t gui_settings_java_home(GuiSettings* s, const char** argv) {
    int i = find(s, "java_home");
    const char* value = i < s->own ? picked(s, i) : NULL;
    if (!value) return 0;
    snprintf(s->values[i].option, sizeof s->values[i].option, "--java-home=%s", value);
    argv[0] = s->values[i].option;
    return 1;
}
size_t gui_settings_gba(GuiSettings* s, const char** argv) {
    int i = find(s, "gba_color");
    const char* value = i < s->own ? picked(s, i) : NULL;
    if (!value) return 0;
    ini_option(s->values[i].option, sizeof s->values[i].option, "gba_color", value);
    argv[0] = s->values[i].option;
    return 1;
}
size_t gui_settings_one(GuiSettings* s, const char* key, const char** argv) {
    int i = find(s, key);
    if (i < s->own || i >= s->count || !picked(s, i)) return 0;
    ini_option(s->values[i].option, sizeof s->values[i].option, key, picked(s, i));
    argv[0] = s->values[i].option;
    return 1;
}
/* argv's --key=value of each setting picked; with a driver's prefix, that driver's only: its tables' (keys that start
   with ctr_ or vita_), and for the 3DS the Switch's it takes too. A Switch run's (prefix NULL), the game's .ini at
   game_ini (NULL: none), are the launcher's picks for every game, said by --launcher-picks=1, under the game's own
   picks, which the runtime reads; the recompile, which reads none, gets the game's own of its settings (given). So are
   a 3DS run's, when one of them is a setting a 3DS game has of its own, which its driver reads */
static size_t picked_options(GuiSettings* s, const char** argv, const char* prefix, const char* game_ini) {
    char* picks = !prefix && game_ini ? snx_game_picks(game_ini) : NULL;
    int ctr = prefix && !strcmp(prefix, "ctr_"), over = !prefix; /* what goes over them, said: the game's own picks */
    size_t n = 0;
    int i;
    for (i = s->own; i < s->count; ++i) {
        const char* key = s->values[i].key;
        const char* value;
        if (!key || (prefix && strncmp(key, prefix, strlen(prefix)) && !(ctr && snx_ctr_shared_setting(key)))) continue;
        if (prefix && !strncmp(key, "nds_wfc", 7)) continue; /* the driver reads the launcher's own from settings.ini */
        if ((value = prefix ? picked(s, i) : given(s, i, picks)) == NULL) continue;
        argv[n++] = option_of(s, i, value);
        over = over || (ctr && snx_ctr_game_setting(key));
    }
    if (n && over) argv[n++] = "--" SNX_LAUNCHER_PICKS "=1";
    free(picks);
    return n;
}
size_t gui_settings_options(GuiSettings* s, int launch, const char* game_ini, const char** argv) {
    size_t n = picked_options(s, argv, NULL, game_ini);
    if (launch && gui_settings_tmpfs(s)) argv[n++] = gui_settings_tmpfs(s);
    if (launch && gui_settings_extract(s)) argv[n++] = gui_settings_extract(s);
    if (launch && own_on(s, "gdbstub")) {
        argv[n++] = "-S";
        argv[n++] = "-s";
    }
    return n;
}
size_t gui_settings_ctr_options(GuiSettings* s, const char** argv) { return picked_options(s, argv, "ctr_", NULL); }
size_t gui_settings_vita_options(GuiSettings* s, const char** argv) { return picked_options(s, argv, "vita_", NULL); }
size_t gui_settings_controls_options(GuiSettings* s, const char** argv) {
    size_t n = 0;
    for (int i = s->own; i < s->count; ++i) {
        const char* value = s->values[i].kind || !controls_key(s->values[i].key) ? NULL : picked(s, i);
        if (value) argv[n++] = option_of(s, i, value);
    }
    if (n) argv[n++] = "--" SNX_LAUNCHER_PICKS "=1";
    return n;
}
size_t gui_settings_linux_options(GuiSettings* s, const char** argv) { return picked_options(s, argv, "linux_", NULL); }
size_t gui_settings_android_options(GuiSettings* s, const char** argv) {
    return picked_options(s, argv, "android_", NULL);
}
size_t gui_settings_nds_options(GuiSettings* s, const char** argv) { return picked_options(s, argv, "nds_", NULL); }
