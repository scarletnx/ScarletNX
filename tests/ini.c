/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The .ini's settings tables (runtime/ini.c): a value read into its variable, clamped, and written back; and as the
   menus word it and step it */
#include "nx/runtime/ini.h"
#include "snx_settings.h" /* the game's Debug table, g_debug_ini */
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static int number, told;
static float base_resolution_mult;
static char text[16];
static void tell(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
    ++told;
}
static void keep(const IniOpt* o, const char* v) {
    (void)o;
    snprintf(text, sizeof text, "%s", v);
}
static char held[8];
/* what ini_read says about a value it clamped or left, once ini_set_say names this; "" for nothing since emptied */
static char line_said[256];
static void catch_said(const char* line) { snprintf(line_said, sizeof line_said, "%s", line); }
static const IniOpt TABLE[] = {
    {"number", "1", "help", INI_INT, 0, &number, -5, 10, tell, NULL, NULL, 0, 0, 0, NULL},
    {"resolution", "1", NULL, INI_FLOAT, INI_STARTUP, &base_resolution_mult, 0.25, 16, NULL, NULL, NULL, 0, 0, 0, NULL},
    {"text", NULL, NULL, INI_TEXT, 0, held, 0, 0, keep, NULL, NULL, 0, 0, sizeof held, NULL},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0, NULL}};

int main(void) {
    const IniOpt *n = ini_find(TABLE, "number"), *r = ini_find(TABLE, "resolution"), *t = ini_find(TABLE, "text");
    char out[32];
    assert(n == &TABLE[0] && r == &TABLE[1] && t == &TABLE[2] && !ini_find(TABLE, "numbe") && !ini_find(TABLE, ""));
    /* a number, clamped, and set told after each read */
    ini_read(n, "7");
    assert(number == 7 && told == 1);
    ini_read(n, "99");
    assert(number == 10 && told == 2);
    ini_read(n, "-1e9");
    assert(number == -5 && told == 3);
    ini_read(n, "3.9"); /* an int keeps the whole part */
    assert(number == 3);
    /* not a number, or NaN: the variable stays and set isn't called */
    ini_read(n, "seven");
    ini_read(n, "");
    ini_read(n, "nan");
    assert(number == 3 && told == 4);
    ini_read(r, "inf");
    assert(base_resolution_mult == 16);
    ini_read(r, "0.5");
    assert(base_resolution_mult == 0.5f);
    /* written back as it reads */
    ini_format(n, out, sizeof out);
    assert(!strcmp(out, "3"));
    ini_format(r, out, sizeof out);
    assert(!strcmp(out, "0.5"));
    /* a resolution forced as nothing or no number keeps the default it starts with, 1 */
    base_resolution_mult = 1.0f;
    ini_read(r, "high");
    assert(base_resolution_mult == 1.0f);
    ini_read(r, "");
    assert(base_resolution_mult == 1.0f);
    /* text: kept whole at var, then set; written back as it is. One too long for var is left out, set not called */
    ini_read(t, "a b c");
    assert(!strcmp(text, "a b c") && !strcmp(held, "a b c"));
    ini_format(t, out, sizeof out);
    assert(!strcmp(out, "a b c"));
    ini_read(t, "too long for it");
    assert(!strcmp(held, "a b c") && !strcmp(text, "a b c"));
    /* ini_check: whether a value is taken as it is, said why not, nothing read */
    {
        char why[128];
        int was = number, said = told;
        assert(!ini_check(n, "7", why, sizeof why) && !ini_check(n, "-5", why, sizeof why) &&
               !ini_check(n, "10", why, sizeof why) && !ini_check(r, "0.5", why, sizeof why) &&
               !ini_check(t, "1234567", why, sizeof why));
        assert(ini_check(n, "11", why, sizeof why) && !strcmp(why, "number=11 is not a whole number from -5 to 10"));
        assert(ini_check(n, "-6", why, sizeof why) && ini_check(n, "abc", why, sizeof why) &&
               ini_check(n, "1.5", why, sizeof why) && ini_check(n, "", why, sizeof why) &&
               ini_check(n, "7x", why, sizeof why) && ini_check(n, "nan", why, sizeof why));
        assert(ini_check(r, "20", why, sizeof why) && !strcmp(why, "resolution=20 is not a number from 0.25 to 16"));
        assert(ini_check(r, "0.05", why, sizeof why) && ini_check(r, "", why, sizeof why));
        assert(ini_check(t, "12345678", why, sizeof why) && !strcmp(why, "text's value is longer than 7 bytes"));
        assert(number == was && told == said && !strcmp(held, "a b c"));
    }
    /* what ini_read did with a value it did not take as given is said, through ini_set_say, with the value the
       setting keeps: the one it had (3 and "a b c" here, not their defaults), or the one clamped to; a value taken as
       given says nothing */
    ini_set_say(catch_said);
    /* a forced resolution that is no number, or nothing, is 1 and said: the line names the value it keeps */
    base_resolution_mult = 1.0f;
    ini_read(r, "high");
    assert(base_resolution_mult == 1.0f &&
           !strcmp(line_said, "settings: resolution=high is not a number from 0.25 to 16: it keeps 1"));
    base_resolution_mult = 1.0f;
    ini_read(r, "");
    assert(base_resolution_mult == 1.0f &&
           !strcmp(line_said, "settings: resolution= is not a number from 0.25 to 16: it keeps 1"));
    base_resolution_mult = 1.0f;
    ini_read(r, "e");
    assert(base_resolution_mult == 1.0f &&
           !strcmp(line_said, "settings: resolution=e is not a number from 0.25 to 16: it keeps 1"));
    ini_read(r, "0.05");
    assert(base_resolution_mult == 0.25f &&
           !strcmp(line_said, "settings: resolution=0.05 is not a number from 0.25 to 16: it keeps 0.25"));
    ini_read(n, "abc");
    assert(number == 3 && !strcmp(line_said, "settings: number=abc is not a whole number from -5 to 10: it keeps 3"));
    ini_read(n, "11");
    assert(number == 10 && !strcmp(line_said, "settings: number=11 is not a whole number from -5 to 10: it keeps 10"));
    ini_read(n, "1.5");
    assert(number == 1 && !strcmp(line_said, "settings: number=1.5 is not a whole number from -5 to 10: it keeps 1"));
    ini_read(n, "7x");
    assert(number == 7 && !strcmp(line_said, "settings: number=7x is not a whole number from -5 to 10: it keeps 7"));
    ini_read(r, "20");
    assert(base_resolution_mult == 16 &&
           !strcmp(line_said, "settings: resolution=20 is not a number from 0.25 to 16: it keeps 16"));
    ini_read(t, "too long for it");
    assert(!strcmp(line_said, "settings: text's value is longer than 7 bytes: it keeps \"a b c\""));
    line_said[0] = 0;
    ini_read(n, "3");
    ini_read(r, "0.5");
    ini_read(t, "fits");
    assert(number == 3 && base_resolution_mult == 0.5f && !strcmp(held, "fits") && !line_said[0]);
    ini_set_say(NULL);
    /* the menus' words: one a value from lo by step; a '#' word for its value and the ones above, plus shift; the
       number where there is none. Words go round, numbers and a slider's words (INI_SLIDER) stop at the ends */
    {
        static int v;
        static const IniOpt onoff = {"onoff", "0",         NULL,     INI_INT, 0, &v, 0,   1,
                                     NULL,    "On or off", "Off|On", 0,       0, 0,  NULL};
        static const IniOpt three = {"three", "0", NULL, INI_INT, 0, &v, 0, 2, NULL, "Three", "Off|Fingers|Mouse too",
                                     0,       0,   0,    NULL};
        static const IniOpt scale = {"scale", "50", NULL, INI_INT, 0, &v, 0, 100, NULL, "Scale", "#%", 10, 50, 0, NULL};
        static const IniOpt rumble = {"rumble", "100",       NULL,     INI_INT, 0, &v, 0,   100,
                                      NULL,     "Vibration", "Off|#%", 10,      0, 0,  NULL};
        static const IniOpt plain = {"plain", "1", NULL, INI_INT, 0, &v, -5, 10, NULL, NULL, NULL, 0, 0, 0, NULL};
        static const IniOpt level = {"level", "1",     NULL,           INI_INT, INI_SLIDER, &v, 0,   2,
                                     NULL,    "Level", "Low|Mid|High", 0,       0,          0,  NULL};
        ini_word(&onoff, 0, out, sizeof out);
        assert(!strcmp(out, "Off"));
        ini_word(&onoff, 1, out, sizeof out);
        assert(!strcmp(out, "On"));
        ini_word(&three, 2, out, sizeof out);
        assert(!strcmp(out, "Mouse too"));
        ini_word(&three, 7, out, sizeof out); /* past its words: the number */
        assert(!strcmp(out, "7"));
        ini_word(&scale, 50, out, sizeof out);
        assert(!strcmp(out, "100%"));
        ini_word(&scale, 55, out, sizeof out); /* off its steps, as a hand-made .ini has it */
        assert(!strcmp(out, "105%"));
        ini_word(&rumble, 0, out, sizeof out);
        assert(!strcmp(out, "Off"));
        ini_word(&rumble, 5, out, sizeof out);
        assert(!strcmp(out, "5%"));
        ini_word(&rumble, 100, out, sizeof out);
        assert(!strcmp(out, "100%"));
        ini_word(&plain, -3, out, sizeof out);
        assert(!strcmp(out, "-3"));
        ini_word(&scale, 50, out, 3); /* cut to the room given */
        assert(!strcmp(out, "10"));
        assert(ini_step(&onoff, 0, 1) == 1 && ini_step(&onoff, 1, 1) == 0 && ini_step(&onoff, 0, -1) == 1);
        assert(ini_step(&three, 2, 1) == 0 && ini_step(&three, 0, -1) == 2 && ini_step(&three, 1, 1) == 2);
        assert(ini_step(&scale, 50, 1) == 60 && ini_step(&scale, 100, 1) == 100 && ini_step(&scale, 0, -1) == 0);
        assert(ini_step(&rumble, 0, 1) == 10 && ini_step(&rumble, 10, -1) == 0 && ini_step(&rumble, 5, 1) == 15);
        assert(ini_step(&plain, -5, -1) == -5 && ini_step(&plain, 3, 1) == 4);
        assert(ini_step(&level, 2, 1) == 2 && ini_step(&level, 0, -1) == 0 && ini_step(&level, 1, 1) == 2);
        ini_word(&level, 2, out, sizeof out);
        assert(!strcmp(out, "High"));
    }
    /* a slider's row (INI_SLIDER) for the launcher: slider in its last column, after an empty one where it needs no
       build, after needs-build where it does */
    {
        static int v;
        static const IniOpt rows[] = {
            {"volume", "100", "How loud", INI_INT, INI_SLIDER, &v, 0, 100, NULL, "Volume", "Mute|#%", 50, 0, 0, NULL},
            {"gated", "0", "Help", INI_INT, INI_SLIDER | INI_NEEDS_BUILD, &v, 0, 1, NULL, "Gated", "Off|On", 0, 0, 0,
             NULL},
            {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0, NULL}};
        static const IniSection sections[] = {{"Audio", 1, {rows, NULL, NULL}, {NULL, NULL, NULL}}};
        static const char want[] =
            "Audio\tvolume\t100\tint\t0\t100\tHow loud\tVolume\t0=Mute|50=50%|100=100%\t\t\tslider\n"
            "Audio\tgated\t0\tint\t0\t1\tHelp\tGated\t0=Off|1=On\t\tneeds-build\tslider\n";
        char list[512];
        assert(ini_list_text(sections, 1, list, sizeof list) == strlen(want) && !strcmp(list, want));
    }
    /* a float stepped as the menus step one: a quarter, a twentieth within a range of 2, to the next point of that grid
       the way it goes, from one between two the nearer that way, stopping at the ends; a step down never skips a point
       (1.25 down was 0.75: the down step's rounding took the point below as well) */
    {
        assert(ini_float_step(1.25, 0.25, 16, -1) == 1.0 && ini_float_step(1.0, 0.25, 16, 1) == 1.25);
        assert(ini_float_step(1.3, 0.25, 16, -1) == 1.25 && ini_float_step(1.3, 0.25, 16, 1) == 1.5);
        assert(ini_float_step(0.25, 0.25, 16, -1) == 0.25 && ini_float_step(16, 0.25, 16, 1) == 16);
        assert(ini_float_step(0, 0, 1, 1) == 0.05 && ini_float_step(0.07, 0, 1, -1) == 0.05 &&
               ini_float_step(0.1, 0, 1, -1) == 0.05);
        assert(ini_float_step(-0.3, -1, 1, -1) > -0.3501 && ini_float_step(-0.3, -1, 1, -1) < -0.3499); /* 7 twentieths */
    }
    /* a row's steps (settings.txt's steps): its values listed with their words and open in its last column, and the
       menus' arrows going to the next listed value, from one between two too, stopping at the ends; an open text's
       mark after its words */
    {
        static int v;
        static char t[16];
        static const IniOpt rows[] = {
            {"queue", "0", "Queued", INI_INT, 0, &v, 0, 16384, NULL, "Queue", "Auto|# MB", 0, 0, 0, "0|256|1024"},
            {"pace", "game", "Pace", INI_TEXT, INI_OPEN, t, 0, 0, NULL, "Pace", "game=Game's|0=No limit", 0, 0,
             sizeof t, NULL},
            {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0, NULL}};
        static const IniSection sections[] = {{"Performance", 1, {rows, NULL, NULL}, {NULL, NULL, NULL}}};
        static const char want[] =
            "Performance\tqueue\t0\tint\t0\t16384\tQueued\tQueue\t0=Auto|256=256 MB|1024=1024 MB\t\t\topen\n"
            "Performance\tpace\tgame\ttext\t0\t0\tPace\tPace\tgame=Game's|0=No limit\t\t\topen\n";
        char list[512];
        assert(ini_list_text(sections, 1, list, sizeof list) == strlen(want) && !strcmp(list, want));
        assert(ini_step(&rows[0], 0, 1) == 256 && ini_step(&rows[0], 256, 1) == 1024 &&
               ini_step(&rows[0], 1024, 1) == 1024);
        assert(ini_step(&rows[0], 1024, -1) == 256 && ini_step(&rows[0], 0, -1) == 0);
        assert(ini_step(&rows[0], 300, 1) == 1024 && ini_step(&rows[0], 300, -1) == 256 &&
               ini_step(&rows[0], 5000, -1) == 1024);
    }
    /* a row of boxes (INI_BOXES): its items as they are in the values' column, whatever its type, and boxes as the last
       column's mark */
    {
        static int v;
        static char t[16];
        static const IniOpt rows[] = {
            {"bits", "0", "Bits", INI_INT, INI_BOXES, &v, 0, 255, NULL, "Bits", "1=One|64=Six", 0, 0, 0, NULL},
            {"set", "", "A set", INI_TEXT, INI_BOXES, t, 0, 0, NULL, "Set", "a=A~of a|b=B~", 0, 0, sizeof t, NULL},
            {"none", "", "No items", INI_TEXT, INI_BOXES, t, 0, 0, NULL, "None", NULL, 0, 0, sizeof t, NULL},
            {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0, NULL}};
        static const IniSection sections[] = {{"Debug", 1, {rows, NULL, NULL}, {NULL, NULL, NULL}}};
        static const char want[] = "Debug\tbits\t0\tint\t0\t255\tBits\tBits\t1=One|64=Six\t\t\tboxes\n"
                                   "Debug\tset\t\ttext\t0\t0\tA set\tSet\ta=A~of a|b=B~\t\t\tboxes\n"
                                   "Debug\tnone\t\ttext\t0\t0\tNo items\tNone\t\t\t\tboxes\n";
        char list[512];
        assert(ini_list_text(sections, 1, list, sizeof list) == strlen(want) && !strcmp(list, want));
    }
    /* a path's row (INI_PATH): its marks folder, file or save, after the others where it has one */
    {
        static char t[16];
        static const IniOpt rows[] = {
            {"corpus", "", "Keeps", INI_TEXT, INI_PATH_FOLDER, t, 0, 0, NULL, "Corpus", NULL, 0, 0, sizeof t, NULL},
            {"in", "", "Reads", INI_TEXT, INI_PATH_FILE | INI_STARTUP, t, 0, 0, NULL, "In", NULL, 0, 0, sizeof t, NULL},
            {"out", "", "Writes", INI_TEXT, INI_PATH_SAVE | INI_NEEDS_BUILD, t, 0, 0, NULL, "Out", NULL, 0, 0, sizeof t,
             NULL},
            {"jdk", "", "A JDK", INI_TEXT, INI_PATH_FOLDER | INI_OPEN, t, 0, 0, NULL, "JDK", "=Ours", 0, 0, sizeof t,
             NULL},
            {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0, NULL}};
        static const IniSection sections[] = {{"Debug", 1, {rows, NULL, NULL}, {NULL, NULL, NULL}}};
        static const char want[] = "Debug\tcorpus\t\ttext\t0\t0\tKeeps\tCorpus\t\t\t\tfolder\n"
                                   "Debug\tin\t\ttext\t0\t0\tReads\tIn\t\t\t\tfile\n"
                                   "Debug\tout\t\ttext\t0\t0\tWrites\tOut\t\t\tneeds-build\tsave\n"
                                   "Debug\tjdk\t\ttext\t0\t0\tA JDK\tJDK\t=Ours\t\t\topen folder\n";
        char list[512];
        assert(ini_list_text(sections, 1, list, sizeof list) == strlen(want) && !strcmp(list, want));
    }
    /* the settings not at their default: a number, a float, a text by its value or its key alone, with the defaults;
       a row without a default never */
    {
        static int on = 1, level = 1;
        static float speed = 1.0f;
        static char folder[32] = "";
        static const IniOpt changed[] = {
            {"level", "1", NULL, INI_INT, 0, &level, 0, 2, NULL, NULL, NULL, 0, 0, 0, NULL},
            {"speed", "1", NULL, INI_FLOAT, 0, &speed, 0.25, 4, NULL, NULL, NULL, 0, 0, 0, NULL},
            {"folder", "", NULL, INI_TEXT, 0, folder, 0, 0, NULL, NULL, NULL, 0, 0, sizeof folder, NULL},
            {"free", NULL, NULL, INI_INT, 0, &on, 0, 1, NULL, NULL, NULL, 0, 0, 0, NULL},
            {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0, NULL}};
        char list[128];
        assert(ini_changed(changed, 0, list, sizeof list) == 0 && !strcmp(list, ""));
        on = 0; /* no default: never listed */
        assert(ini_changed(changed, 0, list, sizeof list) == 0);
        level = 2;
        speed = 1.5f;
        snprintf(folder, sizeof folder, "/home/someone/corpus");
        assert(ini_changed(changed, 0, list, sizeof list) == 3);
        assert(!strcmp(list, "level=2, speed=1.5, folder=/home/someone/corpus"));
        assert(ini_changed(changed, INI_CHANGED_NO_TEXT, list, sizeof list) == 3);
        assert(!strcmp(list, "level=2, speed=1.5, folder"));
        assert(ini_changed(changed, INI_CHANGED_DEFAULT | INI_CHANGED_NO_TEXT, list, sizeof list) == 3);
        assert(!strcmp(list, "level=2 (default 1), speed=1.5 (default 1), folder (default \"\")"));
        assert(ini_changed(changed, 0, list, 12) == 3 && !strcmp(list, "level=2, sp")); /* cut, every one counted */
        level = 1;
        speed = 1.0f;
        folder[0] = 0;
        assert(ini_changed(changed, INI_CHANGED_DEFAULT, list, sizeof list) == 0 && !strcmp(list, ""));
    }
    /* the perf rows off their default (settings.txt's perf), keys alone with a comma between, one skipped: a row with no
       default is at it as an LLVM option's INT_MIN (LLVM's own, until set) and as a once text's empty value. The length
       the whole list takes, whatever the room, the list cut as snprintf cuts */
    {
        static int level = 1, quiet = 0, threshold = INT_MIN;
        static char driver[16] = "";
        static const IniOpt rows[] = {
            {"level", "1", NULL, INI_INT, INI_PERF, &level, 0, 2, NULL, NULL, NULL, 0, 0, 0, NULL},
            {"quiet", "0", NULL, INI_INT, 0, &quiet, 0, 1, NULL, NULL, NULL, 0, 0, 0, NULL},
            {"threshold", NULL, NULL, INI_INT, INI_PERF, &threshold, INT_MIN + 1, INT_MAX, NULL, NULL, NULL, 0, 0, 0,
             NULL},
            {"driver", NULL, NULL, INI_TEXT, INI_PERF, driver, 0, 0, NULL, NULL, NULL, 0, 0, sizeof driver, NULL},
            {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0, NULL}};
        char list[64];
        assert(ini_at_default(&rows[0]) && ini_at_default(&rows[2]) && ini_at_default(&rows[3]));
        assert(ini_perf_keys(rows, NULL, list, sizeof list) == 0 && !strcmp(list, ""));
        quiet = 1; /* off its default, no perf row */
        assert(ini_perf_keys(rows, NULL, list, sizeof list) == 0 && !strcmp(list, ""));
        level = 2;
        threshold = 225;
        snprintf(driver, sizeof driver, "turnip");
        assert(!ini_at_default(&rows[0]) && !ini_at_default(&rows[2]) && !ini_at_default(&rows[3]));
        assert(ini_perf_keys(rows, NULL, list, sizeof list) == 22 && !strcmp(list, "level,threshold,driver"));
        assert(ini_perf_keys(rows, "level", list, sizeof list) == 16 && !strcmp(list, "threshold,driver"));
        assert(ini_perf_keys(rows, NULL, list, 8) == 22 && !strcmp(list, "level,t"));
        assert(ini_perf_keys(rows, NULL, NULL, 0) == 22);
        level = 1;
        quiet = 0;
        threshold = INT_MIN;
        driver[0] = 0;
        assert(ini_perf_keys(rows, NULL, list, sizeof list) == 0);
    }
    /* settings.txt's perf rows as settingsgen makes them: a game's, the recompile's, every Vulkan extension's and every
       LLVM row; not one the screen shows (shadows), nor the AOT threads, which change only how long a recompile takes.
       Tables whose rows call no runtime function: the test links none */
    {
        const IniOpt* o;
        assert((o = ini_find(g_graphics_ini, "resolution")) && (o->flags & INI_PERF));
        assert((o = ini_find(g_graphics_ini, "shadows")) && !(o->flags & INI_PERF));
        assert((o = ini_find(g_recompile_ini, "aot_threads")) && !(o->flags & INI_PERF));
        for (o = g_vk_extensions_ini; o->key; ++o) assert(o->flags & INI_PERF);
    }
    /* a setting of some systems alone (settings.txt's only): where it does something as a greyed row says it, said on
       every system but those and listed last on its line for the launcher; one of every system says nothing */
    {
        static int v;
        static const IniOpt rows[] = {
            {"mac", "0", "Help", INI_INT, INI_STARTUP | INI_ONLY_MACOS, &v, 0, 1, NULL, "Mac", "Off|On", 0, 0, 0, NULL},
            {"win", "0", "Help", INI_INT, INI_ONLY_WINDOWS, &v, 0, 1, NULL, "Win", "Off|On", 0, 0, 0, NULL},
            {"phone", "0", "Help", INI_INT, INI_ONLY_ANDROID, &v, 0, 1, NULL, "Phone", "Off|On", 0, 0, 0, NULL},
            {"kernel", "0", "Help", INI_INT, INI_ONLY_LINUX, &v, 0, 1, NULL, "Kernel", "Off|On", 0, 0, 0, NULL},
            {"nce", "0", "Help", INI_INT, INI_ONLY_ARM64_LINUX, &v, 0, 1, NULL, "NCE", "Off|On", 0, 0, 0, NULL},
            {"apple", "0", "Help", INI_INT, INI_ONLY_APPLE, &v, 0, 1, NULL, "Apple", "Off|On", 0, 0, 0, NULL},
            {"all", "0", "Help", INI_INT, INI_STARTUP, &v, 0, 1, NULL, "All", "Off|On", 0, 0, 0, NULL},
            {"never", "0", "Help", INI_INT, INI_ONLY_NONE, &v, 0, 0, NULL, "Never", "Off", 0, 0, 0, NULL},
            {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0, NULL}};
        static const IniSection sections[] = {{"Test", 1, {rows, NULL, NULL}, {NULL, NULL, NULL}}};
        const char *mac = "macOS only", *win = "Windows only", *phone = "Android only";
        const char *kernel = "Linux and Android only", *nce = "arm64 Linux and Android only";
        const char* apple = "macOS and iOS only";
        char list[1024], want[1024];
        assert(!strcmp(ini_only_words(INI_STARTUP | INI_ONLY_MACOS), "macOS only") && !ini_only_words(INI_STARTUP));
        assert(!strcmp(ini_only_words(INI_ONLY_WINDOWS), win) && !strcmp(ini_only_words(INI_ONLY_ANDROID), phone));
        assert(!strcmp(ini_only_words(INI_ONLY_LINUX), kernel) && !strcmp(ini_only_words(INI_ONLY_ARM64_LINUX), nce));
        assert(!strcmp(ini_only_words(INI_ONLY_APPLE), apple));
        assert(!ini_only_words(INI_ONLY)); /* no system's: a setting of every one */
        assert(!ini_only(&rows[6]));
        /* none: greyed on every system, this one too */
        assert(!strcmp(ini_only_words(INI_ONLY_NONE), "Not offered") && !strcmp(ini_only(&rows[7]), "Not offered"));
        /* this system's are said by no row: the others' by each */
#if defined(__APPLE__)
        apple = "";
#endif
#if defined(__APPLE__) && !defined(SNX_IOS)
        mac = "";
#elif defined(_WIN32)
        win = "";
#elif defined(__linux__)
        kernel = "";
#if defined(__ANDROID__)
        phone = "";
#endif
#if defined(__aarch64__)
        nce = "";
#endif
#endif
        assert(!ini_only(&rows[0]) == !mac[0] && (!mac[0] || !strcmp(ini_only(&rows[0]), mac)));
        assert(!ini_only(&rows[1]) == !win[0] && !ini_only(&rows[2]) == !phone[0]);
        assert(!ini_only(&rows[3]) == !kernel[0] && !ini_only(&rows[4]) == !nce[0]);
        assert(!ini_only(&rows[5]) == !apple[0] && (!apple[0] || !strcmp(ini_only(&rows[5]), apple)));
        snprintf(want, sizeof want,
                 "Test\tmac\t0\tint\t0\t1\tHelp\tMac\t0=Off|1=On\t%s\n"
                 "Test\twin\t0\tint\t0\t1\tHelp\tWin\t0=Off|1=On\t%s\n"
                 "Test\tphone\t0\tint\t0\t1\tHelp\tPhone\t0=Off|1=On\t%s\n"
                 "Test\tkernel\t0\tint\t0\t1\tHelp\tKernel\t0=Off|1=On\t%s\n"
                 "Test\tnce\t0\tint\t0\t1\tHelp\tNCE\t0=Off|1=On\t%s\n"
                 "Test\tapple\t0\tint\t0\t1\tHelp\tApple\t0=Off|1=On\t%s\n"
                 "Test\tall\t0\tint\t0\t1\tHelp\tAll\t0=Off|1=On\t\n"
                 "Test\tnever\t0\tint\t0\t0\tHelp\tNever\t0=Off\tNot offered\n",
                 mac, win, phone, kernel, nce, apple);
        assert(ini_list_text(sections, 1, list, sizeof list) == strlen(want) && !strcmp(list, want));
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) /* a Linux PC: all but the kernel's greyed */
        assert(mac[0] && win[0] && phone[0] && !kernel[0] && nce[0] && apple[0]);
#endif
        /* the Switch's online service, offered nowhere: its row greyed on every system, Off its one value */
        const IniOpt* online = ini_find(g_switch_online_ini, "switch_online");
        assert(online && !strcmp(ini_only(online), "Not offered") && !strcmp(online->words, "Off") && online->hi == 0);
    }
    /* the game's Debug table: log_max past its default is said at the top of the picture ("Debug on: ...") and in the
       log's header, as every Debug setting not at its default */
    {
        char list[2048];
        assert(g_settings.log_size_limit == 16);
        ini_changed(g_debug_ini, INI_CHANGED_NO_TEXT, list, sizeof list);
        assert(!strstr(list, "log_max"));
        g_settings.log_size_limit = 64;
        assert(ini_changed(g_debug_ini, INI_CHANGED_NO_TEXT, list, sizeof list) >= 1 && strstr(list, "log_max=64"));
        assert(ini_changed(g_debug_ini, INI_CHANGED_DEFAULT, list, sizeof list) >= 1 &&
               strstr(list, "log_max=64 (default 16)"));
        g_settings.log_size_limit = 16;
    }
    /* the ASTC mode is a Graphics setting: astc_mode=1 (Exact) is not said at the top of the picture with the Debug
       ones, and the Graphics settings past their default say it. astc_bc, the key before it, is deprecated */
    {
        char list[2048];
        assert(ini_find(g_graphics_ini, "astc_mode") && !ini_find(g_debug_ini, "astc_mode"));
        assert(!ini_find(g_graphics_ini, "astc_bc") && ini_deprecated("astc_bc"));
        /* hmma_mode took hmma_astat's place, which is deprecated */
        assert(ini_find(g_debug_ini, "hmma_mode") && !ini_find(g_debug_ini, "hmma_astat") &&
               ini_deprecated("hmma_astat"));
        g_settings.astc_mode = 1;
        ini_changed(g_debug_ini, INI_CHANGED_NO_TEXT, list, sizeof list);
        assert(!strstr(list, "astc_mode"));
        assert(ini_changed(g_graphics_ini, INI_CHANGED_DEFAULT, list, sizeof list) >= 1 &&
               strstr(list, "astc_mode=1 (default 0)"));
        g_settings.astc_mode = 0;
    }
    /* NCE's look at its addresses is a Debug setting, its seconds: nce_foreign_scan=10, read from a line as a .ini gives
       it, is said at the top of the picture ("Debug on: ...") and in the log's header, and 0 (off) is not; a value past
       0 to 3600 is refused with the value given and the one it keeps */
    {
        char list[2048];
        const IniOpt* o = ini_find(g_debug_ini, "nce_foreign_scan");
        assert(o && !g_settings.nce_foreign_scan);
        ini_changed(g_debug_ini, INI_CHANGED_NO_TEXT, list, sizeof list);
        assert(!strstr(list, "nce_foreign_scan"));
        ini_read(o, "10");
        assert(g_settings.nce_foreign_scan == 10);
        assert(ini_changed(g_debug_ini, INI_CHANGED_NO_TEXT, list, sizeof list) >= 1 &&
               strstr(list, "nce_foreign_scan=10"));
        assert(ini_changed(g_debug_ini, INI_CHANGED_DEFAULT, list, sizeof list) >= 1 &&
               strstr(list, "nce_foreign_scan=10 (default 0)"));
        ini_read(o, "1");
        assert(g_settings.nce_foreign_scan == 1);
        ini_read(o, "3600");
        assert(g_settings.nce_foreign_scan == 3600);
        ini_set_say(catch_said);
        line_said[0] = 0;
        ini_read(o, "3601");
        assert(
            g_settings.nce_foreign_scan == 3600 &&
            !strcmp(line_said, "settings: nce_foreign_scan=3601 is not a whole number from 0 to 3600: it keeps 3600"));
        ini_read(o, "-1");
        assert(g_settings.nce_foreign_scan == 0 &&
               !strcmp(line_said, "settings: nce_foreign_scan=-1 is not a whole number from 0 to 3600: it keeps 0"));
        ini_set_say(NULL);
        ini_read(o, "0");
        ini_changed(g_debug_ini, INI_CHANGED_NO_TEXT, list, sizeof list);
        assert(!g_settings.nce_foreign_scan && !strstr(list, "nce_foreign_scan"));
    }
    { /* a command line's --key=value */
        char key[8];
        const char* value = ini_arg("--speed=2", key, sizeof key, NULL, 0);
        assert(value && !strcmp(value, "2") && !strcmp(key, "speed"));
        assert(!ini_arg("--speed", key, sizeof key, NULL, 0) && !ini_arg("-s=1", key, sizeof key, NULL, 0));
        assert(!ini_arg("--too_long=1", key, sizeof key, NULL, 0));
        value = ini_arg("--a=b=c", key, sizeof key, NULL, 0);
        assert(value && !strcmp(value, "b=c") && !strcmp(key, "a"));
    }
    { /* and its key's name with a - for each _, of the names given (sorted as strcmp sorts them: a - before a digit,
         which an _ comes after), the first, a middle and the last found. The key itself, the older spelling, is said
         once a run for each name, with the name now; some _ left, or no name, is the key as given and said nothing */
        static const char* const names[] = {"a-b", "a-b-c", "a1-b", "ab-c", "z-z"};
        static const char* const cases[][4] = {
            {"--a-b=1", "a_b", "1", ""},
            {"--a-b-c=x=y", "a_b_c", "x=y", ""},
            {"--a1-b=", "a1_b", "", ""},
            {"--ab-c=2", "ab_c", "2", ""},
            {"--z-z=3", "z_z", "3", ""},
            {"--a_b=4", "a_b", "4", "settings: --a_b is the old spelling of --a-b: it is read as that; write --a-b"},
            {"--a_b=5", "a_b", "5", ""},
            {"--a-b=6", "a_b", "6", ""},
            {"--z_z=7", "z_z", "7", "settings: --z_z is the old spelling of --z-z: it is read as that; write --z-z"},
            {"--a_b-c=8", "a_b-c", "8", ""},
            {"--a-b_c=9", "a-b_c", "9", ""},
            {"--a-c=10", "a-c", "10", ""},
            {"--a_c=11", "a_c", "11", ""},
            {"--a1=12", "a1", "12", ""},
            {"--z-zz=13", "z-zz", "13", ""},
            {"--0-a=14", "0-a", "14", ""},
            {"--jit-opt=1", "jit_opt", "1", ""}, /* an older version's key, deprecated: the key, for the reader */
        };
        char key[8];
        size_t i;
        ini_set_say(catch_said);
        for (i = 0; i < sizeof cases / sizeof *cases; i++) {
            const char* value;
            line_said[0] = 0;
            value = ini_arg(cases[i][0], key, sizeof key, names, 5);
            assert(value && !strcmp(value, cases[i][2]));
            if (strcmp(key, cases[i][1]) || strcmp(line_said, cases[i][3])) {
                fprintf(stderr, "%s: key %s, said \"%s\"; not %s, \"%s\"\n", cases[i][0], key, line_said, cases[i][1],
                        cases[i][3]);
                assert(!"ini_arg's key or line");
            }
        }
        ini_set_say(NULL);
        assert(!ini_arg("--a-b-c-de=1", key, sizeof key, names, 5)); /* a NAME of 8 letters does not fit key's 8 */
    }
    { /* the option that sets a key, as the command line gives it: a - for each _ of the key alone, cut to the room */
        char option[16];
        assert(ini_option(option, sizeof option, "aot_threads", "4") == 15 && !strcmp(option, "--aot-threads=4"));
        assert(ini_option(option, sizeof option, "opt", "s_z") == 9 && !strcmp(option, "--opt=s_z"));
        assert(ini_option(option, 8, "aot_threads", "4") == 15 && !strcmp(option, "--aot-t"));
        assert(ini_option(NULL, 0, "state_slots", "10") == 16);
    }
    puts("ini passed");
    return 0;
}
