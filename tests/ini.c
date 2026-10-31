/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The .ini's settings tables (runtime/ini.c): a value read into its variable, clamped, and written back; and as the
   menus word it and step it */
#include "nx/runtime/ini.h"
#include "snx_settings.h" /* the game's Debug table, g_debug_ini */
#include <assert.h>
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
    {"number", "1", "help", INI_INT, 0, &number, -5, 10, tell, NULL, NULL, 0, 0, 0},
    {"resolution", "1", NULL, INI_FLOAT, INI_STARTUP, &base_resolution_mult, 0.25, 16, NULL, NULL, NULL, 0, 0, 0},
    {"text", NULL, NULL, INI_TEXT, 0, held, 0, 0, keep, NULL, NULL, 0, 0, sizeof held},
    {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0}};

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
       number where there is none. Words go round, numbers stop at the ends */
    {
        static int v;
        static const IniOpt onoff = {"onoff", "0", NULL, INI_INT, 0, &v, 0, 1, NULL, "On or off", "Off|On", 0, 0, 0};
        static const IniOpt three = {"three", "0", NULL, INI_INT, 0, &v, 0, 2, NULL, "Three", "Off|Fingers|Mouse too",
                                     0,       0, 0};
        static const IniOpt scale = {"scale", "50", NULL, INI_INT, 0, &v, 0, 100, NULL, "Scale", "#%", 10, 50, 0};
        static const IniOpt rumble = {"rumble", "100", NULL,        INI_INT,  0,  &v, 0,
                                      100,      NULL,  "Vibration", "Off|#%", 10, 0, 0};
        static const IniOpt plain = {"plain", "1", NULL, INI_INT, 0, &v, -5, 10, NULL, NULL, NULL, 0, 0, 0};
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
    }
    /* the settings not at their default: a number, a float, a text by its value or its key alone, with the defaults;
       a row without a default never */
    {
        static int on = 1, level = 1;
        static float speed = 1.0f;
        static char folder[32] = "";
        static const IniOpt changed[] = {
            {"level", "1", NULL, INI_INT, 0, &level, 0, 2, NULL, NULL, NULL, 0, 0, 0},
            {"speed", "1", NULL, INI_FLOAT, 0, &speed, 0.25, 4, NULL, NULL, NULL, 0, 0, 0},
            {"folder", "", NULL, INI_TEXT, 0, folder, 0, 0, NULL, NULL, NULL, 0, 0, sizeof folder},
            {"free", NULL, NULL, INI_INT, 0, &on, 0, 1, NULL, NULL, NULL, 0, 0, 0},
            {NULL, NULL, NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, 0, 0, 0}};
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
    /* ASTC as BC7 is a Graphics setting: astc_bc=1 is not said at the top of the picture with the Debug ones, and the
       Graphics settings past their default say it */
    {
        char list[2048];
        assert(ini_find(g_graphics_ini, "astc_bc") && !ini_find(g_debug_ini, "astc_bc"));
        g_settings.astc_bc = 1;
        ini_changed(g_debug_ini, INI_CHANGED_NO_TEXT, list, sizeof list);
        assert(!strstr(list, "astc_bc"));
        assert(ini_changed(g_graphics_ini, INI_CHANGED_DEFAULT, list, sizeof list) >= 1 &&
               strstr(list, "astc_bc=1 (default 0)"));
        g_settings.astc_bc = 0;
    }
    puts("ini passed");
    return 0;
}
