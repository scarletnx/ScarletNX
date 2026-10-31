/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the .ini's settings tables (ini.h): finding a key, reading a value into its variable, and writing it back */
#include "nx/runtime/ini.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const IniOpt* ini_find(const IniOpt* table, const char* key) {
    for (; table->key; ++table)
        if (!strcmp(table->key, key)) return table;
    return NULL;
}

static void (*g_say)(const char* line);
void ini_set_say(void (*say)(const char* line)) { g_say = say; }
/* a value not taken as given: ini_check's why, and the value the setting has now, as the .ini writes it (the default,
   or the value before a reload) */
static void say(const IniOpt* o, const char* v) {
    char why[256], value[64], line[1024];
    ini_check(o, v, why, sizeof why);
    if (o->type == INI_TEXT)
        snprintf(line, sizeof line, "settings: %s: it keeps \"%s\"", why, (const char*)o->var);
    else {
        ini_format(o, value, sizeof value);
        snprintf(line, sizeof line, "settings: %s: it keeps %s", why, value);
    }
    if (g_say)
        g_say(line);
    else
        fprintf(stderr, "%s\n", line);
}

void ini_read(const IniOpt* o, const char* v) {
    char why[256];
    char* end;
    double d;
    if (o->type == INI_TEXT) {
        size_t n = strlen(v);
        if (n >= o->size) {
            say(o, v);
            return;
        }
        memcpy(o->var, v, n + 1);
        if (o->set) o->set(o, v);
        return;
    }
    d = strtod(v, &end);
    if (end == v || d != d) { /* NaN would pass the range unclamped */
        say(o, v);
        return;
    }
    if (d < o->lo) d = o->lo;
    if (d > o->hi) d = o->hi;
    /* atomic: other threads read some of the settings while a reload writes them (the GPU thread's vsync) */
    if (o->type == INI_INT)
        __atomic_store_n((int*)o->var, (int)d, __ATOMIC_RELAXED);
    else {
        float f = (float)d;
        __atomic_store((float*)o->var, &f, __ATOMIC_RELAXED);
    }
    /* clamped, an int's whole part, or a number with something after it */
    if (ini_check(o, v, why, sizeof why)) say(o, v);
    if (o->set) o->set(o, v);
}

int ini_check(const IniOpt* o, const char* v, char* out, size_t n) {
    char* end;
    double d;
    if (o->type == INI_TEXT) {
        if (strlen(v) < o->size) return 0;
        snprintf(out, n, "%s's value is longer than %u bytes", o->key, (unsigned)o->size - 1);
        return -1;
    }
    d = strtod(v, &end);
    if (end != v && !*end && d == d && d >= o->lo && d <= o->hi && (o->type == INI_FLOAT || d == (double)(long)d))
        return 0;
    if (o->type == INI_INT)
        snprintf(out, n, "%s=%s is not a whole number from %.0f to %.0f", o->key, v, o->lo, o->hi);
    else
        snprintf(out, n, "%s=%s is not a number from %g to %g", o->key, v, o->lo, o->hi);
    return -1;
}

void ini_format(const IniOpt* o, char* out, size_t n) {
    if (o->type == INI_INT)
        snprintf(out, n, "%d", *(const int*)o->var);
    else if (o->type == INI_FLOAT)
        snprintf(out, n, "%g", *(const float*)o->var);
    else
        snprintf(out, n, "%s", (const char*)o->var);
}

/* whether key is one of list's, a comma between */
static int listed(const char* list, const char* key) {
    size_t n = strlen(key);
    while (list && *list) {
        size_t len = strcspn(list, ",");
        if (len == n && !strncmp(list, key, n)) return 1;
        list += len + (list[len] == ',');
    }
    return 0;
}
int ini_changed(const IniOpt* table, int flags, char* out, size_t n) {
    return ini_changed_marked(table, flags, NULL, out, n);
}
int ini_changed_marked(const IniOpt* table, int flags, const char* marked, char* out, size_t n) {
    size_t at = 0;
    int count = 0;
    if (n) out[0] = 0;
    for (; table->key; ++table) {
        char value[64];
        const char* shown = value;
        int same;
        if (!table->def) continue;
        if (table->type == INI_TEXT) {
            shown = (const char*)table->var;
            same = !strcmp(shown, table->def);
        } else {
            double def = strtod(table->def, NULL);
            if (table->type == INI_INT)
                same = __atomic_load_n((const int*)table->var, __ATOMIC_RELAXED) == (int)def;
            else {
                float f;
                __atomic_load((const float*)table->var, &f, __ATOMIC_RELAXED);
                same = f == (float)def;
            }
            ini_format(table, value, sizeof value);
        }
        if (same) continue;
        if (at < n) {
            if (table->type == INI_TEXT && (flags & INI_CHANGED_NO_TEXT))
                at += (size_t)snprintf(out + at, n - at, "%s%s", count ? ", " : "", table->key);
            else
                at += (size_t)snprintf(out + at, n - at, "%s%s=%s", count ? ", " : "", table->key, shown);
        }
        if (at < n && listed(marked, table->key)) at += (size_t)snprintf(out + at, n - at, " [recommended]");
        if (at < n && (flags & INI_CHANGED_DEFAULT))
            at += (size_t)snprintf(out + at, n - at, table->type == INI_TEXT ? " (default \"%s\")" : " (default %s)",
                                   table->def);
        ++count;
    }
    return count;
}

static int step_of(const IniOpt* o) { return o->step > 0 ? o->step : 1; }
void ini_word(const IniOpt* o, int v, char* out, size_t n) {
    const char* w = o->words;
    double at = o->lo;
    size_t len;
    for (; w; w += len + 1, at += step_of(o)) {
        const char* mark;
        len = strcspn(w, "|");
        mark = (const char*)memchr(w, '#', len);
        if (mark) { /* a number from here on */
            snprintf(out, n, "%.*s%d%.*s", (int)(mark - w), w, v + o->shift, (int)(len - (size_t)(mark - w) - 1),
                     mark + 1);
            return;
        }
        if ((double)v == at) {
            snprintf(out, n, "%.*s", (int)len, w);
            return;
        }
        if (!w[len]) break;
    }
    snprintf(out, n, "%d", v); /* no word for it */
}
int ini_step(const IniOpt* o, int v, int dir) {
    int step = step_of(o), count;
    if (!o->words || strchr(o->words, '#')) {
        double n = (double)v + (double)dir * step;
        return n < o->lo ? (int)o->lo : n > o->hi ? (int)o->hi : (int)n;
    }
    count = (int)((o->hi - o->lo) / step) + 1;
    return (int)o->lo + (((v - (int)o->lo) / step + dir) % count + count) % count * step;
}

/* the listing, appended to out as it fits */
typedef struct {
    char* out;
    size_t size, at;
} Listing;
static void put(Listing* l, const char* fmt, ...) {
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(l->at < l->size ? l->out + l->at : NULL, l->at < l->size ? l->size - l->at : 0, fmt, ap);
    va_end(ap);
    if (n > 0) l->at += (size_t)n;
}
static void list_help(Listing* l, const char* help) {
    for (; *help; help++)
        if (*help == '\n') {
            while (help[1] == ' ' || help[1] == ';') help++;
            put(l, " ");
        } else
            put(l, "%c", *help);
}
static void list_values(Listing* l, const IniOpt* o) {
    int step = o->step > 0 ? o->step : 1, v, first = 1;
    char word[64];
    if (o->type == INI_TEXT && o->words) { /* value=word already */
        put(l, "%s", o->words);
        return;
    }
    if (o->type != INI_INT) return;
    /* typed: a number of no words, or more than the menus step through */
    if (!o->words && (o->lo < 0 || (o->hi - o->lo) / step + 1 > 16)) return;
    if (o->words && strchr(o->words, '#') && (o->hi - o->lo) / step + 1 > 128) return;
    for (v = (int)o->lo; v <= (int)o->hi; v += step) {
        if (o->words)
            ini_word(o, v, word, sizeof word);
        else
            snprintf(word, sizeof word, "%d", v);
        put(l, "%s%d=%s", first ? "" : "|", v, word);
        first = 0;
    }
}
/* the help of a repeated setting's later instance (a player's), which only its first has: that of the row before it
   with its name, "" for none */
static const char* repeated_help(const IniOpt* table, const IniOpt* o) {
    const IniOpt* p;
    for (p = o; p > table;) {
        --p;
        if (p->help && p->name && o->name && !strcmp(p->name, o->name)) return p->help;
    }
    return "";
}
size_t ini_list_text(const IniSection* sections, int count, char* out, size_t size) {
    static const char* const types[] = {"int", "float", "text"};
    Listing l;
    const char* help = "";
    int s, t;
    l.out = out;
    l.size = size;
    l.at = 0;
    if (size) out[0] = 0;
    for (s = 0; s < count; s++)
        for (t = 0; t < 3 && sections[s].table[t]; t++) {
            const IniOpt* o;
            for (o = sections[s].table[t]; o->key; o++) {
                if (o->help)
                    help = o->help;
                else if (repeated_help(sections[s].table[t], o)[0]) /* a later instance: its first's */
                    help = repeated_help(sections[s].table[t], o);
                /* else none: the one before covers it */
                if (o->flags & INI_IN_GAME) continue;
                /* the range in its digits, which the launcher's Settings take and say as they read: %g's six cut
                   1048576 to 1.04858e+06 */
                put(&l, "%s%s%s\t%s\t%s\t%s\t%.15g\t%.15g\t", sections[s].name, sections[s].page[t] ? "/" : "",
                    sections[s].page[t] ? sections[s].page[t] : "", o->key, o->def ? o->def : "", types[o->type], o->lo,
                    o->hi);
                list_help(&l, help);
                put(&l, "\t%s\t", o->name ? o->name : "");
                list_values(&l, o);
                put(&l, "\n");
            }
        }
    return l.at;
}

/* what became of each: the rewritten file keeps them under [Deprecated] with it, and a run names them, for the player
   to delete (the launcher's Settings > App > Clean Old Settings does it) */
static const struct {
    const char *key, *now;
} INI_DEPRECATED[] = {{"audio_device", "playback follows the system's default"},
                      {"renderer", "Vulkan is the only renderer"},
                      {"dynamic_resolution", "went with the OpenGL renderer"},
                      {"dynamic_resolution_min", "went with the OpenGL renderer"},
                      {"game_dynamic_resolution", "went with the OpenGL renderer"},
                      {"low_priority_shader_compiler", "went with the OpenGL renderer"},
                      {"forced_vsync", "now vsync"},
                      {"smooth_recording", "the F7 recorder went with the OpenGL renderer"},
                      {"recording_fps", "the F7 recorder went with the OpenGL renderer"},
                      {"lsfg", "LSFG went with the OpenGL renderer"},
                      {"lsfg_dll", "LSFG went with the OpenGL renderer"},
                      {"lsfg_target", "LSFG went with the OpenGL renderer"},
                      {"lsfg_multiplier", "LSFG went with the OpenGL renderer"},
                      {"lsfg_flow", "LSFG went with the OpenGL renderer"},
                      {"lsfg_resolution", "LSFG went with the OpenGL renderer"},
                      {"lossless_scaling", "LSFG went with the OpenGL renderer"},
                      {"lossless_path", "LSFG went with the OpenGL renderer"},
                      {"touch_controls", "now controls"},
                      {"touch_controls_opacity", "now controls_opacity"},
                      {"modo_tv", "now docked"},
                      {"idioma", "now language"},
                      {"resolucion", "now resolution"},
                      {"sombras", "now shadows"},
                      {"perfil", "now profile"},
                      {"gamepad_buttons", "now gamepad_swap_ab; gamepad_map always applies"},
                      {"cpu_fallback", "the runtime JIT compiles the code the recompiler missed"},
                      {NULL, NULL}};
const char* ini_deprecated(const char* k) {
    int i;
    for (i = 0; INI_DEPRECATED[i].key; i++)
        if (!strcmp(k, INI_DEPRECATED[i].key)) return INI_DEPRECATED[i].now;
    return NULL;
}
void ini_deprecated_head(FILE* f) {
    fprintf(f,
            "\n[Deprecated]\n; keys an older version used, which do nothing now (docs/DEPRECATIONS.md): delete them, "
            "or use Settings > App >\n;   Clean Old Settings in the launcher\n");
}
void ini_deprecated_key(FILE* f, const char* key, const char* value) {
    fprintf(f, "; %s: %s\n%s=%s\n", key, ini_deprecated(key), key, value);
}
int ini_deprecated_note(char* out, size_t size, const char* path, const char* const* keys, int n) {
    size_t at;
    int i;
    if (n <= 0 || !size) return 0;
    at = (size_t)snprintf(out, size, "settings: %s keeps %d key%s this version does not use (", path, n,
                          n > 1 ? "s" : "");
    for (i = 0; i < n && at < size; i++) at += (size_t)snprintf(out + at, size - at, "%s%s", i ? ", " : "", keys[i]);
    if (at < size)
        snprintf(out + at, size - at,
                 "), under [Deprecated]: delete %s, or use Settings > App > Clean Old Settings in "
                 "the launcher",
                 n > 1 ? "them" : "it");
    return 1;
}
