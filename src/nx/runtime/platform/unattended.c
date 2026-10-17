/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* Unattended-run options for both windowed and headless execution through the SDL3 backend. */
#include "nx/runtime/platform/unattended.h"
#include "nx/runtime/ini.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/plat.h"
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_seconds = 0, g_maxframes = -1, g_shot_every = 0;
static char* g_shot; /* owned: --shot= points into argv, a reload's line buffer is gone after it */
static int g_shots, g_shot_bucket = -1;
static u64 g_start;
static u64 g_press = 0;
static int g_press_at = 0, g_press_len = 300; /* ms */
/* script: "ms:hex,ms:hex,...", parsed each time the option is set (a script= under the .ini's [Other] again on
   each reload). "Nf:hex" presses at present N instead: games that advance one step per presented frame (Catherine)
   reach a screen at the same present however slow the machine runs, and a press timed in ms can come before it
   there */
typedef struct {
    u64 t, mask;
    int frames; /* t counts presents */
    u64 began;  /* frames: 1 + the ms it was first due, 0 before */
} ScriptPress;
static ScriptPress* g_script;
static int g_nscript;
static char* g_script_text;

static void script_parse(const char* v) {
    free(g_script);
    g_script = NULL;
    g_nscript = 0;
    int cap = 0;
    const char* p = v;
    while (*p) {
        char* e;
        u64 t = strtoull(p, &e, 10);
        int frames = e != p && *e == 'f';
        if (frames) e++;
        if (e == p || *e != ':') break;
        const char* m0 = e + 1;
        u64 m = strtoull(m0, &e, 16);
        if (e == m0 || (*e && *e != ',')) break;
        if (g_nscript == cap) {
            cap = cap ? cap * 2 : 16;
            ScriptPress* n = realloc(g_script, sizeof *n * (size_t)cap);
            if (!n) {
                WARN("script: out of memory, so the presses from \"%s\" on are ignored\n", p);
                return;
            }
            g_script = n;
        }
        g_script[g_nscript++] = (ScriptPress){t, m, frames, 0};
        p = *e ? e + 1 : e;
    }
    if (*p) WARN("script: \"%s\" is not ms:hex or Nf:hex, so it and the presses after it are ignored\n", p);
}

static void shot_set(const IniOpt* o, const char* v) {
    (void)o;
    free(g_shot);
    g_shot = v[0] ? strdup(v) : NULL;
}
static void press_set(const IniOpt* o, const char* v) {
    (void)o;
    g_press = strtoull(v, NULL, 16);
}
static void script_set(const IniOpt* o, const char* v) {
    (void)o;
    /* An unrelated INI change reapplies script too. Keep the progress of unchanged frame-based presses. */
    if (g_script_text && !strcmp(g_script_text, v)) return;
    char* text = strdup(v);
    if (!text) {
        WARN("script: out of memory, keeping the current script\n");
        return;
    }
    script_parse(v);
    free(g_script_text);
    g_script_text = text;
}
/* given as --key=value, or read from the .ini's [Other] (none is in its sections, so none is written with help) */
static const IniOpt INI[] = {{"seconds", NULL, NULL, INI_INT, 0, &g_seconds, 0, INT_MAX, NULL, NULL, NULL, 0, 0},
                             {"frames", NULL, NULL, INI_INT, 0, &g_maxframes, -1, INT_MAX, NULL, NULL, NULL, 0, 0},
                             {"shot", NULL, NULL, INI_TEXT, 0, NULL, 0, 0, shot_set, NULL, NULL, 0, 0},
                             {"shot_every", NULL, NULL, INI_INT, 0, &g_shot_every, 0, INT_MAX, NULL, NULL, NULL, 0, 0},
                             {"press", NULL, NULL, INI_TEXT, 0, NULL, 0, 0, press_set, NULL, NULL, 0, 0},
                             {"press_at", NULL, NULL, INI_INT, 0, &g_press_at, 0, INT_MAX, NULL, NULL, NULL, 0, 0},
                             {"press_len", NULL, NULL, INI_INT, 0, &g_press_len, 0, INT_MAX, NULL, NULL, NULL, 0, 0},
                             {"script", NULL, NULL, INI_TEXT, 0, NULL, 0, 0, script_set, NULL, NULL, 0, 0},
                             {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0}};
int unattended_ini(const char* k, const char* v) {
    const IniOpt* o = ini_find(INI, k);
    if (o) ini_read(o, v);
    return o != NULL;
}
int unattended_known(const char* k) { return ini_find(INI, k) != NULL; }
void unattended_start(void) { g_start = plat_ns(); }
u64 unattended_ms(void) { return (plat_ns() - g_start) / 1000000ull; }
int unattended_active(void) { return g_seconds || g_maxframes >= 0; }
u64 unattended_buttons(void) {
    u64 el = (plat_ns() - g_start) / 1000000ull, b = 0;
    if (g_press && el >= (u64)g_press_at && el < (u64)g_press_at + (u64)g_press_len) b |= g_press;
    for (int i = 0; i < g_nscript; i++) {
        ScriptPress* s = &g_script[i];
        if (s->frames) {
            if (!s->began && (u64)__atomic_load_n(&g_frames, __ATOMIC_RELAXED) >= s->t) s->began = el + 1;
            if (s->began && el < s->began - 1 + (u64)g_press_len) b |= s->mask;
        } else if (el >= s->t && el < s->t + (u64)g_press_len)
            b |= s->mask;
    }
    return b;
}
const char* unattended_shot(void) { return g_shot; }
int unattended_periodic_shot(char* path, size_t n) {
    if (!g_shot_every || !g_shot) return 0;
    /* by bucket rather than g_frames % shot_every: the SDL3 backend asks once per tick, and frames can go by two
       at a time */
    int b = __atomic_load_n(&g_frames, __ATOMIC_RELAXED) / g_shot_every;
    if (b == g_shot_bucket) return 0;
    g_shot_bucket = b;
    snprintf(path, n, "%s_%04d", g_shot, g_shots++);
    return 1;
}
int unattended_done(void) {
    u64 el = plat_ns() - g_start;
    return (g_maxframes >= 0 && __atomic_load_n(&g_frames, __ATOMIC_RELAXED) >= g_maxframes) ||
           (g_seconds && el >= (u64)g_seconds * 1000000000ull);
}
void unattended_exit(u64 hash, int w, int h) {
    int frames = __atomic_load_n(&g_frames, __ATOMIC_RELAXED);
    if (w > 0 && h > 0)
        fprintf(stderr, "\nframes=%d hash=%016llx %dx%d\n", frames, (unsigned long long)hash, w, h);
    else
        fprintf(stderr, "\nframes=%d hash=%016llx\n", frames, (unsigned long long)hash);
    ini_poll(1); /* what changed in the run since the last poll is saved, as on a quit */
    snx_save_before_exit();
    plat_exit_now(0);
}

static i32 axis(u64 b, int neg, int pos) { return ((b >> pos) & 1 ? 32767 : 0) - ((b >> neg) & 1 ? 32767 : 0); }
void input_key_sticks(u64 k, i32 st[4]) {
    st[0] = axis(k, 40, 42);
    st[1] = axis(k, 43, 41);
    st[2] = axis(k, 44, 46);
    st[3] = axis(k, 47, 45);
}
void input_stick_clamp(i32* x, i32* y) {
    i32 cx = *x < -32767 ? -32767 : *x, cy = *y < -32767 ? -32767 : *y;
    double r = sqrt((double)cx * cx + (double)cy * cy);
    if (r > 32767.0) {
        cx = (i32)(cx * 32767.0 / r);
        cy = (i32)(cy * 32767.0 / r);
    }
    *x = cx;
    *y = cy;
}
