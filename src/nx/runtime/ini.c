/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* the .ini's settings tables (ini.h): finding a key, reading a value into its variable, and writing it back */
#include "nx/runtime/ini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const IniOpt* ini_find(const IniOpt* table, const char* key) {
    for (; table->key; ++table)
        if (!strcmp(table->key, key)) return table;
    return NULL;
}

void ini_read(const IniOpt* o, const char* v) {
    char* end;
    double d;
    if (o->type == INI_TEXT) {
        o->set(o, v);
        return;
    }
    d = strtod(v, &end);
    if (end == v || d != d) return; /* NaN would pass the range unclamped */
    if (d < o->lo) d = o->lo;
    if (d > o->hi) d = o->hi;
    /* atomic: other threads read some of the settings while a reload writes them (the GPU thread's vsync) */
    if (o->type == INI_INT)
        __atomic_store_n((int*)o->var, (int)d, __ATOMIC_RELAXED);
    else {
        float f = (float)d;
        __atomic_store((float*)o->var, &f, __ATOMIC_RELAXED);
    }
    if (o->set) o->set(o, v);
}

void ini_format(const IniOpt* o, char* out, size_t n) {
    if (o->type == INI_INT)
        snprintf(out, n, "%d", *(const int*)o->var);
    else if (o->type == INI_FLOAT)
        snprintf(out, n, "%g", *(const float*)o->var);
    else if (n)
        out[0] = 0;
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

/* what became of each: the rewritten file keeps them under [Deprecated] with it, and a run names them, for the player
   to delete (the launcher's Settings > App > Clean old settings does it) */
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
            "or use Settings > App >\n;   Clean old settings in the launcher\n");
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
                 "), under [Deprecated]: delete %s, or use Settings > App > Clean old settings in "
                 "the launcher",
                 n > 1 ? "them" : "it");
    return 1;
}
