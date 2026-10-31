/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/settings.h"
#include "common/utilities/common.h"
#include "common/utilities/ini_lines.h"
#include "gui/i18n.h"
#include "snx_settings.h"
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
/* a row's values, value=word between '|', split in place: its words (.ini first) from *pool, NULL when one has no =.
   values gets them by word */
static const char** words_of(char* list, const char*** pool, const char*** values) {
    const char** words = *pool;
    int n = 1, i;
    char* c;
    for (c = list; *c; ++c) n += *c == '|';
    *values = words + n + 2;
    words[0] = ".ini";
    (*values)[0] = NULL;
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
    words[n + 1] = NULL;
    *pool += 2 * n + 3;
    return words;
}
int gui_settings_read(GuiSettings* s, const char* runtime) {
    /* the launcher's own rows and the recompiler's (settings.txt's launcher sections), listed as the runtime lists the
       game's, then the runtime's */
    size_t own = ini_list_text(g_launch_sections, g_launch_sections_count, NULL, 0), size, n = 3, bars = 0, pool;
    const char* c;
    const char** free_words;
    char *line, *next, *text, *page = NULL, *list, *own_end;
    GuiSetting *link = NULL, *all = NULL, *v;
    int w;
    gui_settings_free(s);
    size = own + strlen(runtime) + 1;
    list = malloc(size);
    if (!list) return 1;
    ini_list_text(g_launch_sections, g_launch_sections_count, list, own + 1);
    memcpy(list + own, runtime, strlen(runtime) + 1);
    for (c = list; *c; ++c) { /* a row a line, a page's own two before its first; and the words */
        n += *c == '\n' ? 3 : 0;
        bars += *c == '|';
    }
    pool = 2 * bars + 5 * n;
    if (n > (SIZE_MAX - size) / (sizeof *s->keys + sizeof *s->values + 5 * sizeof(char*)) ||
        bars > (SIZE_MAX - size) / 4 / sizeof(char*)) {
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
        char* field[9];
        if (line >= own_end && !s->own) s->own = s->count; /* the launcher's rows end there */
        const char **words = NULL, **values = NULL;
        int f = 1;
        next = line + strcspn(line, "\n");
        if (*next) *next++ = 0;
        line[strcspn(line, "\r")] = 0;
        field[0] = line;
        while (f < 9 && (field[f] = strchr(field[f - 1], '\t')) != NULL) *field[f++]++ = 0;
        if (f < 7) continue;                               /* not a setting: another line the runtime printed */
        for (; f < 9; ++f) field[f] = line + strlen(line); /* an older runtime's: no name, no words */
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
        if (*field[8]) words = words_of(field[8], &free_words, &values);
        v = row(s, field[0], field[1], field[7], field[6], words, words ? GUI_KEY_WORD : GUI_KEY_TEXT);
        v->values = values;
        v->type_text = !strcmp(field[3], "text");
        if (!words && (field[3][0] == 'i' || field[3][0] == 'f')) { /* typed, not picked: a number of its range */
            GuiThemeKey* k = &s->keys[s->count - 1];
            k->numeric = 1;
            k->whole = field[3][0] == 'i';
            k->lo = strtod(field[4], NULL);
            k->hi = strtod(field[5], NULL);
        }
        v->fallback = v->def = field[2];
        s->keys[s->count - 1].def = v->def; /* what a number's row shows when it is emptied */
        for (w = 1; words && words[w]; ++w) /* the word of the default's value */
            if (!strcmp(values ? values[w] : words[w], field[2])) v->fallback = words[w];
        if (page) {
            link->count = s->count - link->first;
            all->count = s->count - all->first;
        }
    }
    if (!s->own) s->own = s->count; /* no runtime listed any */
    gui_settings_sync(s);
    return 0;
}
void gui_settings_free(GuiSettings* s) {
    free(s->keys);
    memset(s, 0, sizeof *s);
}
/* row i's value as a file of picks keeps it: a word row's value (.ini for none), a typed row's text */
static const char* kept(const GuiSettings* s, int i) {
    const GuiSetting* v = &s->values[i];
    if (s->keys[i].type == GUI_KEY_ACTION) return "";
    if (s->keys[i].type == GUI_KEY_TEXT) return v->text;
    return v->values && v->word ? v->values[v->word] : s->keys[i].words[v->word];
}
/* row i set to value, a value of its words or a text: 1 when it did */
static int set(GuiSettings* s, int i, const char* value) {
    GuiSetting* v = &s->values[i];
    int w;
    if (s->keys[i].type != GUI_KEY_WORD || !v->values) return gui_theme_set(s->values, &s->keys[i], value);
    for (w = 0; s->keys[i].words[w]; ++w)
        if (!strcmp(value, w ? v->values[w] : s->keys[i].words[0])) {
            v->word = w;
            return 1;
        }
    if (v->def && *v->def && !strcmp(value, v->def)) { /* the default no word has (off, the launcher's): no pick */
        v->word = 0;
        return 1;
    }
    return 0;
}
void gui_settings_mark(GuiSettings* s) {
    int i;
    for (i = 0; i < s->count; ++i) snprintf(s->values[i].start, sizeof s->values[i].start, "%s", kept(s, i));
}
void gui_settings_load(GuiSettings* s, const char* path) {
    size_t size;
    char *text = path ? (char*)snx_read_file(path, &size) : NULL, *key, *value;
    SnxIni ini;
    if (!text) return;
    snx_ini_start(&ini, text); /* a key names one row whatever its section, as in the game's .ini */
    while (snx_ini_next(&ini, &key, &value)) gui_settings_set(s, key, value);
    free(text);
    gui_settings_sync(s);
}
static int find(const GuiSettings* s, const char* key) {
    int i;
    for (i = 0; i < s->count && (s->values[i].kind || strcmp(s->values[i].key, key)); ++i) {}
    return i;
}
int gui_settings_save(const GuiSettings* s, const char* path) {
    size_t room = 256 + (size_t)s->count * 200, at = 0, size = 0, lines = 1; /* a row's section, key, value: < 200 */
    /* the launcher's rows alone (the runtime listed none): the file's other keys are no row's to leave out */
    char *old = s->count > s->own ? NULL : (char*)snx_read_file(path, &size), *c, *key, *value;
    char *text, *folder, *staged;
    const char* section = "";
    int i, failed;
    SnxIni ini;
    char now[sizeof ini.section]; /* the section the text is in */
    /* one it cannot read: written over, they would be lost */
    if (!old && s->count <= s->own && snx_is_file(path)) return 1;
    for (c = old; c && *c; ++c) lines += *c == '\n';
    room += old ? size + lines * (sizeof ini.section + 4) : 0; /* each line a key's, under a section of its own */
    text = (char*)malloc(room);
    folder = snx_dirname(path);
    staged = snx_format("%s.new", path);
    if (text)
        at = (size_t)snprintf(text, room,
                              "; what the launcher's Settings changed, for every game it runs: over each "
                              "game's .ini\n");
    for (i = 0; text && i < s->count; ++i) {
        const char* value = kept(s, i);
        if (!strcmp(value, s->values[i].start)) continue;
        if (strcmp(section, s->keys[i].section))
            at += (size_t)snprintf(text + at, room - at, "[%s]\n", section = s->keys[i].section);
        at += (size_t)snprintf(text + at, room - at, "%s=%s\n", s->values[i].key, value);
    }
    snprintf(now, sizeof now, "%s", section);
    if (old) snx_ini_start(&ini, old);
    while (text && old && snx_ini_next(&ini, &key, &value)) { /* a key names one row whatever its section */
        if (find(s, key) < s->count) continue;
        if (ini.section[0] && strcmp(now, ini.section))
            at += (size_t)snprintf(text + at, room - at, "[%s]\n", strcpy(now, ini.section));
        at += (size_t)snprintf(text + at, room - at, "%s=%s\n", key, value);
    }
    free(old);
    /* staged beside it, then put in place: a crash mid-write leaves the old picks */
    failed = !text || snx_mkdirs(folder) || snx_write_file(staged, text, at) || snx_replace(staged, path);
    if (failed) remove(staged);
    free(staged);
    free(folder);
    free(text);
    return failed;
}
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
/* row i's pick, NULL for the .ini's (the launcher's: off) */
static const char* picked(const GuiSettings* s, int i) {
    const GuiSetting* v = &s->values[i];
    if (s->keys[i].type == GUI_KEY_WORD) return v->word ? kept(s, i) : NULL;
    return s->keys[i].type == GUI_KEY_TEXT && v->text[0] ? v->text : NULL;
}
int gui_settings_set(GuiSettings* s, const char* key, const char* value) {
    int i = find(s, key);
    return i < s->count && set(s, i, value);
}
int gui_settings_unread(GuiSettings* s, const char* path, char* out, size_t size) {
    size_t n = 0, bytes;
    char *text = path ? (char*)snx_read_file(path, &bytes) : NULL, *key, *value;
    int count = 0, i;
    SnxIni ini;
    if (size) out[0] = 0;
    if (!text) return 0;
    snx_ini_start(&ini, text);
    while (snx_ini_next(&ini, &key, &value)) {
        GuiSetting was;
        int taken;
        if ((i = find(s, key)) < s->count) { /* tried on the row, which is then put back as it was */
            was = s->values[i];
            taken = set(s, i, value);
            s->values[i] = was;
            if (taken) continue;
        }
        if (n < size) n += (size_t)snprintf(out + n, size - n, "%s%s=%s", count ? ", " : "", key, value);
        ++count;
    }
    free(text);
    return count;
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
        snprintf(all->text, sizeof all->text, "%s", i < all->first + all->count ? snx_tr("Mixed") : first);
    }
}
void gui_settings_shown(const GuiSettings* s, int row, char* out, size_t size) {
    const GuiSetting* v = &s->values[row];
    if (v->kind == GUI_SETTING_ALL)
        snprintf(out, size, "%s", strcmp(v->text, ".ini") ? v->text : snx_tr("[default]"));
    else if (!v->fallback || picked(s, row)) {
        size_t at;
        gui_theme_get(s->values, &s->keys[row], out, size);
        at = strlen(out);
        if (gui_settings_is_recommended(s, row) && at < size)
            snprintf(out + at, size - at, " %s", snx_tr("[recommended]"));
    } else
        snprintf(out, size, "%s%s%s", v->fallback, v->fallback[0] ? " " : "", snx_tr("[default]"));
}
const char* gui_settings_value(const GuiSettings* s, const char* key) {
    int i = find(s, key);
    return i < s->count ? picked(s, i) : NULL;
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
void gui_settings_recommended(GuiSettings* s, const RecFile* f) {
    memcpy(s->recommended, f->applied, sizeof s->recommended);
}
int gui_settings_is_recommended(const GuiSettings* s, int row) {
    const char* value = row >= 0 && row < s->count ? picked(s, row) : NULL;
    int k = value ? rec_key(s->values[row].key) : -1;
    return k >= 0 && s->recommended[k][0] && same_value(value, s->recommended[k]);
}
size_t gui_settings_marks(GuiSettings* s, const char** argv) {
    size_t at = 0;
    int i;
    for (i = s->own; i < s->count; ++i)
        if (gui_settings_is_recommended(s, i) && at < sizeof s->marks)
            at += (size_t)snprintf(s->marks + at, sizeof s->marks - at, "%s%s",
                                   at ? "," : "--recommended=", s->values[i].key);
    if (!at || at >= sizeof s->marks) return 0;
    argv[0] = s->marks;
    return 1;
}
/* row i in the Debug section, or a page of it */
static int debug_row(const GuiSettings* s, int i) {
    const char* section = s->keys[i].section;
    return !s->values[i].kind && section && !strncmp(section, "Debug", 5) && (!section[5] || section[5] == '/');
}
int gui_settings_debug(const GuiSettings* s, const char* ini, int picks, char* out, size_t size) {
    size_t at = 0, bytes;
    char *text = ini ? (char*)snx_read_file(ini, &bytes) : NULL, *key, *value;
    const char** file = s->count ? (const char**)calloc((size_t)s->count, sizeof *file) : NULL; /* by row */
    int count = 0, i;
    SnxIni lines;
    if (size) out[0] = 0;
    if (text && file) {
        snx_ini_start(&lines, text);
        while (snx_ini_next(&lines, &key, &value))
            if ((i = find(s, key)) < s->count) file[i] = value; /* the last one's, as the runtime reads it */
    }
    for (i = 0; i < s->count; ++i) {
        const char* now = picks ? picked(s, i) : NULL;
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
    return count;
}
const char* gui_settings_tmpfs(const GuiSettings* s) { return own_on(s, "tmpfs") ? "--tmpfs" : NULL; }
const char* gui_settings_extract(const GuiSettings* s) { return own_on(s, "extract") ? "--extract-container" : NULL; }
size_t gui_settings_recompile(GuiSettings* s, const char** argv) {
    size_t n = 0;
    int i;
    for (i = 0; i < s->own; ++i) {
        const char* value = s->values[i].kind ? NULL : picked(s, i);
        const char* key = s->values[i].key;
        if (!value || !strcmp(key, "gdbstub") || !strcmp(key, "tmpfs") || !strcmp(key, "extract") ||
            !strcmp(key, "tracy_capture"))
            continue;
        snprintf(s->values[i].option, sizeof s->values[i].option, "--%s=%s", key, value);
        argv[n++] = s->values[i].option;
    }
    return n;
}
size_t gui_settings_one(GuiSettings* s, const char* key, const char** argv) {
    int i = find(s, key);
    if (i < s->own || i >= s->count || !picked(s, i)) return 0;
    snprintf(s->values[i].option, sizeof s->values[i].option, "--%s=%s", key, picked(s, i));
    argv[0] = s->values[i].option;
    return 1;
}
size_t gui_settings_options(GuiSettings* s, int launch, const char** argv) {
    size_t n = 0;
    int i;
    for (i = s->own; i < s->count; ++i) {
        const char* value = picked(s, i);
        if (!value) continue;
        snprintf(s->values[i].option, sizeof s->values[i].option, "--%s=%s", s->values[i].key, value);
        argv[n++] = s->values[i].option;
    }
    if (launch && gui_settings_tmpfs(s)) argv[n++] = gui_settings_tmpfs(s);
    if (launch && gui_settings_extract(s)) argv[n++] = gui_settings_extract(s);
    if (launch && own_on(s, "gdbstub")) {
        argv[n++] = "-S";
        argv[n++] = "-s";
    }
    if (launch && own_on(s, "tracy_capture")) argv[n++] = "--tracy-capture";
    return n;
}
