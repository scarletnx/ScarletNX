/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's Settings (src/gui/settings.c): the runtime's list read into the form's rows, a setting's pick and the
   options a run gets. With a runtime's path, its own --settings list too */
#include "common/utilities/common.h"
#include "common/utilities/config_file.h"
#include "common/utilities/game_ini.h"
#include "gui/editor.h"
#include "gui/settings.h"
#include "snx_settings.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

/* as scarletnx --settings prints them, and a line of something else, an older runtime's of seven fields, values with
   one missing its word, a Windows line end and no last one */
static const char list[] =
    "Display\tdocked\t1\tint\t0\t1\t1 = TV mode\t\t0=0|1=1\n"
    "Display\tfullscreen\t0\tint\t0\t1\t1 = start in fullscreen\t\t0=0|1=1\r\n"
    "Display\tlanguage\t1\tint\t-2.14748e+09\t2.14748e+09\tGame language: 0 Japanese\t\t\n"
    "Graphics\tresolution\t1\tfloat\t0.25\t16\tInternal 3D resolution\t\t\n"
    "Graphics\tanisotropy\t0\tint\t0\t16\tTexture filtering at an angle\t\t"
    "0=0|1=1|2=2|3=3|4=4|5=5|6=6|7=7|8=8|9=9|10=10|11=11|12=12|13=13|14=14|15=15|16=16\n"
    "Graphics\told\t0\ttext\t0\t1\tAn older runtime's line\n"
    "Graphics\tbroken\t0\tint\t0\t1\tA value without its word\t\t0|1=On\n"
    "settings: not a setting\n"
    "On-screen controls\tcontrols\t0\tint\t0\t1\tThe on-screen controls\tShow controls\t0=Off|1=On\n"
    "On-screen controls\ttouch\t2\tint\t0\t2\tThe game's touch screen\tTouch screen\t0=Off|1=Fingers|2=Fingers and mouse\n"
    "On-screen controls/Buttons\tcontrols_a_shown\t\tint\t0\t1\tA control drawn\tA\t0=Hidden|1=Shown\n"
    "On-screen controls/Buttons\tcontrols_b_shown\t\tint\t0\t1\tA control drawn\tB\t0=Hidden|1=Shown\n"
    "Controllers\tgamepad_rumble\t100\tint\t0\t100\tHow strongly\tVibration\t0=Off|50=50%|100=100%\n"
    "Debug\ttracy_capture\t0\tint\t0\t1\tA capture of the run\tTracy capture\t0=Off|1=On\n"
    "Debug\ttracy_port\t\ttext\t0\t0\tThe client's port\tTracy port\t\t\tneeds-build\n"
    "Debug\tcpu_extensions\t0\tint\t0\t1\tNewer CPU extensions\tCPU extensions\t0=Off|1=On\n"
    "Debug\tlog\t1\tint\t0\t2\tLog detail\t\t0=0|1=1|2=2";

/* a row by its key or name: the runtime's first, then the launcher's */
/* the row of a kind, -1 for none */
static int find_kind(const GuiSettings* s, int kind) {
    int i;
    for (i = 0; i < s->count; ++i)
        if (s->values[i].kind == kind) return i;
    return -1;
}
static int at(const GuiSettings* s, const char* name) {
    int i, n;
    for (n = 0; n < s->count; ++n) {
        i = (s->own + n) % s->count;
        if ((s->values[i].key && !strcmp(s->values[i].key, name)) || !strcmp(s->keys[i].key, name)) return i;
    }
    return -1;
}
static const GuiThemeKey* key(const GuiSettings* s, const char* name) {
    int i = at(s, name);
    return i < 0 ? NULL : &s->keys[i];
}
static int words(const GuiThemeKey* k) {
    int n = 0;
    while (k->words[n]) ++n;
    return n;
}
static const char* shown(const GuiSettings* s, const char* name) {
    static char value[64];
    gui_theme_get(s->values, key(s, name), value, sizeof value);
    return value;
}
/* the resolution row set to in, and what it keeps: the rounding, clamping and leading zeros a typed value takes */
static void res(GuiSettings* s, const char* in, const char* want) {
    assert(gui_settings_set(s, "resolution", in));
    assert(!strcmp(gui_settings_value(s, "resolution"), want));
}
/* as the Settings screen shows it */
static const char* on_screen(const GuiSettings* s, const char* name) {
    static char value[96];
    gui_settings_shown(s, at(s, name), value, sizeof value);
    return value;
}

int main(int argc, char** argv) {
    GuiSettings s = {0};
    GuiEditor form;
    const char* options[64];
    int fullscreen, touch, stub, tmpfs, extract, launch, page, all, changed;
    size_t tracy_n;
    assert(!gui_settings_read(&s, list));

    /* the launcher's own and the recompiler's (settings.txt's launcher sections), then a row a setting in the list's
       order, the other line left out; a page's row and its All before its settings */
    int tracy = at(&s, "tracy_capture") >= 0; /* the row a runtime without SNX_TRACY does not list */
    int aot = 4;                              /* the AOT threads' row, after the launcher's own four */
    assert(s.own > 5 && s.count == s.own + 18 && !strcmp(s.keys[0].key, "GDB stub"));
    assert(!strcmp(s.keys[0].section, "Launch") && !strcmp(s.keys[0].about, "Enables gdbstub on port 1234") &&
           words(&s.keys[0]) == 2 && !strcmp(s.values[0].key, "gdbstub"));
    /* shown by its name, kept by its key */
    assert(!strcmp(s.keys[1].section, "Launch") && !strcmp(s.keys[1].key, "Extract to tmpfs") &&
           !strcmp(s.values[1].key, "tmpfs") && words(&s.keys[1]) == 2 && strstr(s.keys[1].about, "RomFS"));
    assert(!strcmp(s.keys[2].section, "Launch") && !strcmp(s.keys[2].key, "Extract dump") &&
           !strcmp(s.values[2].key, "extract") && words(&s.keys[2]) == 2 && strstr(s.keys[2].about, "dump"));
    /* Java home, a typed folder: --java-home for the Android driver when set, never a recompile option */
    assert(!strcmp(s.keys[3].section, "Launch") && !strcmp(s.keys[3].key, "Java home") &&
           !strcmp(s.values[3].key, "java_home") && strstr(s.keys[3].about, "--java-home") &&
           !gui_settings_java_home(&s, options));
    assert(gui_settings_set(&s, "java_home", "/opt/jdk") && gui_settings_java_home(&s, options) == 1 &&
           !strcmp(options[0], "--java-home=/opt/jdk") && !gui_settings_recompile(&s, 0, NULL, options) &&
           gui_settings_set(&s, "java_home", "") && !gui_settings_java_home(&s, options));
    assert(!tracy || !strcmp(s.keys[at(&s, "tracy_capture")].section, "Debug")); /* with the profiler's other rows */
    assert(!gui_settings_extract(&s)); /* off: a dump read in place */
    /* the recompiler's: none passed at its default, then --key=value; the AOT threads a number (0 Auto) and the most
       Auto opens (0 the cores'), the unit size a number of instructions, the unit cache limit a number of MB (0 none) */
    assert(!strcmp(s.keys[aot].section, "Launch") && !strcmp(s.keys[aot].key, "AOT threads") &&
           !strcmp(s.values[aot].key, "aot_threads") && !strcmp(s.values[aot].def, "0") &&
           !gui_settings_recompile(&s, 0, NULL, options));
    assert(!gui_settings_set(&s, "aot_threads", "q") && gui_settings_set(&s, "aot_threads", "4") &&
           gui_settings_recompile(&s, 0, NULL, options) == 1 && !strcmp(options[0], "--aot-threads=4"));
    /* the 3DS, Vita and GBA drivers' recompiles take the threads */
    assert(gui_settings_recompile_shared(&s, options) == 1 && !strcmp(options[0], "--aot-threads=4"));
    assert(!strcmp(s.keys[aot + 1].section, "Launch") && !strcmp(s.keys[aot + 1].key, "AOT threads at most") &&
           !strcmp(s.values[aot + 1].key, "aot_threads_most") &&
           !strcmp(s.values[aot + 1].def, "0")); /* Auto, the default, which is not passed */
    assert(gui_settings_set(&s, "aot_threads_most", "3") && gui_settings_recompile(&s, 0, NULL, options) == 2 &&
           !strcmp(options[1], "--aot-threads-most=3") && gui_settings_recompile_shared(&s, options) == 2 &&
           gui_settings_set(&s, "aot_threads_most", "0") &&
           !gui_settings_value(&s, "aot_threads_most")); /* back to Auto, a step of its list */
    assert(!strcmp(s.keys[aot + 2].section, "Launch") && !strcmp(s.keys[aot + 2].key, "Unit size") &&
           !strcmp(s.values[aot + 2].key, "unit_instructions") && !strcmp(s.values[aot + 2].def, "90000"));
    assert(gui_settings_set(&s, "unit_instructions", "20000") && gui_settings_recompile(&s, 0, NULL, options) == 2 &&
           !strcmp(options[1], "--unit-instructions=20000") && gui_settings_set(&s, "unit_instructions", "90000") &&
           !gui_settings_value(&s, "unit_instructions"));
    assert(!strcmp(s.keys[aot + 3].section, "Launch") && !strcmp(s.keys[aot + 3].key, "Unit cache ceiling") &&
           !strcmp(s.values[aot + 3].key, "unit_cache_ceil_mb") && strstr(s.keys[aot + 3].about, "0 = none held"));
    assert(!strcmp(s.values[aot + 3].def, "512") &&
           strstr(s.keys[aot + 3].about, "a hundredth of the machine's memory at most")); /* 512 MB */
    assert(gui_settings_set(&s, "unit_cache_ceil_mb", "64") && gui_settings_recompile(&s, 0, NULL, options) == 2 &&
           !strcmp(options[1], "--unit-cache-ceil-mb=64") && gui_settings_set(&s, "unit_cache_ceil_mb", "512") &&
           !gui_settings_value(&s, "unit_cache_ceil_mb"));
    /* the level and LLVM's own rows went with LLVM: Launch ends at the unit cache */
    assert(at(&s, "opt") < 0 && at(&s, "llvm_batch") < 0 && strcmp(s.keys[aot + 4].section, "Launch"));
    assert(gui_settings_set(&s, "aot_threads", "0") && !gui_settings_recompile(&s, 0, NULL, options));
    /* Settings > GBA's Covers, the launcher's own after Launch's: None by default, Box art (libretro-thumbnails) when
       picked, an unknown value refused, and never a recompile's option nor a run's */
    {
        int covers = at(&s, "gba_covers");
        assert(covers > aot + 3 && covers < s.own && !strcmp(s.keys[covers].section, "GBA") &&
               !strcmp(s.keys[covers].key, "Covers") && words(&s.keys[covers]) == 2 &&
               strstr(s.keys[covers].about, "libretro-thumbnails") &&
               !strcmp(on_screen(&s, "gba_covers"), "None [default]") && !gui_settings_value(&s, "gba_covers"));
        assert(!gui_settings_set(&s, "gba_covers", "screenshot") && gui_settings_set(&s, "gba_covers", "boxart") &&
               !strcmp(gui_settings_value(&s, "gba_covers"), "boxart") &&
               !strcmp(on_screen(&s, "gba_covers"), "Box art (libretro-thumbnails)"));
        assert(!gui_settings_recompile(&s, 1, NULL, options) && !gui_settings_one(&s, "gba_covers", options));
        assert(gui_settings_set(&s, "gba_covers", "none") && !gui_settings_value(&s, "gba_covers"));
    }
    /* Settings > GBA's Colors after it: GBA screen by default and then no option for the run (the runner's own default),
       Raw picked and then the run's --gba-color=raw, an unknown value refused, and never a recompile's option */
    {
        int colors = at(&s, "gba_color");
        assert(colors == at(&s, "gba_covers") + 1 && colors < s.own && !strcmp(s.keys[colors].section, "GBA") &&
               !strcmp(s.keys[colors].key, "Colors") && words(&s.keys[colors]) == 2 &&
               !strcmp(on_screen(&s, "gba_color"), "GBA screen [default]") && !gui_settings_gba(&s, options));
        assert(!gui_settings_set(&s, "gba_color", "sepia") && gui_settings_set(&s, "gba_color", "raw") &&
               !strcmp(on_screen(&s, "gba_color"), "Raw") && gui_settings_gba(&s, options) == 1 &&
               !strcmp(options[0], "--gba-color=raw"));
        assert(!gui_settings_recompile(&s, 1, NULL, options) && !gui_settings_one(&s, "gba_color", options));
        assert(gui_settings_set(&s, "gba_color", "gba") && !gui_settings_gba(&s, options));
    }
    /* a game's setting alone, as a GBA run takes fullscreen: once picked, and never a launcher's own */
    assert(!gui_settings_one(&s, "cpu_extensions", options) && gui_settings_set(&s, "cpu_extensions", "1"));
    assert(gui_settings_one(&s, "cpu_extensions", options) == 1 && !strcmp(options[0], "--cpu-extensions=1"));
    assert(gui_settings_set(&s, "aot_threads", "4") && !gui_settings_one(&s, "aot_threads", options) &&
           gui_settings_set(&s, "aot_threads", "0"));
    /* a run that makes the game (-r) takes the game's settings the recompile takes too, after the recompiler's: CPU
       extensions, not a setting of the game alone (docked); a run gets them with the game's options, never twice */
    assert(gui_settings_set(&s, "aot_threads", "4") && gui_settings_set(&s, "docked", "0"));
    assert(gui_settings_recompile(&s, 1, NULL, options) == 2 && !strcmp(options[0], "--aot-threads=4") &&
           !strcmp(options[1], "--cpu-extensions=1"));
    assert(gui_settings_recompile(&s, 0, NULL, options) == 1 && !strcmp(options[0], "--aot-threads=4"));
    assert(gui_settings_set(&s, "aot_threads", "0") && gui_settings_set(&s, "docked", "1") &&
           gui_settings_recompile(&s, 1, NULL, options) == 1 && !strcmp(options[0], "--cpu-extensions=1"));
    assert(gui_settings_set(&s, "cpu_extensions", "0") && !gui_settings_one(&s, "cpu_extensions", options) &&
           !gui_settings_recompile(&s, 1, NULL, options));
    assert(!strcmp(s.keys[s.own].section, "Display") && !strcmp(s.keys[s.own].key, "docked"));
    assert(!strcmp(s.keys[s.own].about, "1 = TV mode") && !strcmp(key(&s, "fullscreen")->about, "1 = start in fullscreen"));
    assert(!strcmp(s.keys[s.count - 1].key, "log") && !strcmp(s.keys[s.count - 1].about, "Log detail"));
    /* a setting with values goes round their words, named as the game's menu names it, its default's among them; one
       whose default has no word (an empty one) has .ini besides; the others are typed, an older runtime's and one
       with a value missing its word among them */
    assert(!strcmp(key(&s, "controls")->key, "Show controls") && key(&s, "controls")->type == GUI_KEY_WORD);
    assert(words(key(&s, "touch")) == 3 && !strcmp(key(&s, "touch")->key, "Touch screen"));
    assert(!strcmp(key(&s, "touch")->words[0], "Off") && !strcmp(key(&s, "touch")->words[2], "Fingers and mouse"));
    assert(words(key(&s, "anisotropy")) == 17 && !strcmp(key(&s, "anisotropy")->words[16], "16"));
    assert(words(key(&s, "controls_a_shown")) == 3 && !strcmp(key(&s, "controls_a_shown")->words[0], ".ini"));
    assert(key(&s, "language")->type == GUI_KEY_TEXT && key(&s, "resolution")->type == GUI_KEY_TEXT);
    /* a number's row takes any value of its range and the fraction a scale needs, the closest value in range past
       either end, and refuses what is not a number, as 3x or high were, where it keeps what it had */
    assert(key(&s, "language")->numeric && key(&s, "language")->whole && key(&s, "resolution")->numeric &&
           !key(&s, "resolution")->whole);
    assert(key(&s, "resolution")->lo == 0.25 && key(&s, "resolution")->hi == 16);
    assert(!strcmp(key(&s, "resolution")->def, "1"));
    /* within the range as typed, at most two decimals; past an end, the closest value in it */
    res(&s, "0.1", "0.25");
    res(&s, "0.24", "0.25");
    res(&s, "0.25", "0.25");
    res(&s, "0.3", "0.3");
    res(&s, "0.5", "0.5");
    res(&s, "1.61", "1.61");
    res(&s, "8.2", "8.2");
    res(&s, "16", "16");
    res(&s, "2.223", "2.22");
    res(&s, "3.333", "3.33");
    res(&s, "1.60", "1.6");
    res(&s, "2.0", "2");
    res(&s, "00.25", "0.25");
    res(&s, "01.60", "1.6");
    res(&s, ".25", "0.25");
    res(&s, "007", "7");
    res(&s, "0.05", "0.25");
    res(&s, "0.09", "0.25");
    res(&s, "16.01", "16");
    res(&s, "20", "16");
    res(&s, "999", "16");
    assert(gui_settings_set(&s, "language", "007") && !strcmp(gui_settings_value(&s, "language"), "7") &&
           gui_settings_set(&s, "language", ""));
    assert(!gui_settings_set(&s, "resolution", "3x") && !gui_settings_set(&s, "resolution", "1e1") &&
           !gui_settings_set(&s, "resolution", "high"));
    assert(!strcmp(gui_settings_value(&s, "resolution"), "16"));
    assert(!gui_settings_set(&s, "language", "3.5") && gui_settings_set(&s, "language", "3.0") &&
           !strcmp(gui_settings_value(&s, "language"), "3") && gui_settings_set(&s, "language", ""));
    /* the scale goes to a run as it was picked, any scale within the row's range */
    assert(gui_settings_set(&s, "resolution", "1.22") && gui_settings_options(&s, 0, NULL, options) == 2 &&
           !strcmp(options[0], "--resolution=1.22") && !strcmp(options[1], "--launcher-picks=1"));
    assert(gui_settings_set(&s, "resolution", "") && !gui_settings_value(&s, "resolution"));
    assert(key(&s, "old")->type == GUI_KEY_TEXT && key(&s, "broken")->type == GUI_KEY_TEXT);
    assert(!strcmp(shown(&s, "touch"), "Fingers and mouse") && !strcmp(shown(&s, "log"), "1"));
    /* the page: its row in its section opens the rows after its All, which steps them */
    page = at(&s, "Buttons");
    all = page + 1;
    assert(page == at(&s, "touch") + 1 && !strcmp(s.keys[page].section, "On-screen controls"));
    assert(s.keys[page].type == GUI_KEY_ACTION && s.values[page].kind == GUI_SETTING_PAGE);
    assert(s.values[page].first == all && s.values[page].count == 3 && !strcmp(shown(&s, "Buttons"), ">"));
    assert(!strcmp(s.keys[all].key, "All") && s.values[all].kind == GUI_SETTING_ALL && s.values[all].first == all + 1);
    assert(s.values[all].count == 2 && !strcmp(s.keys[all].section, "On-screen controls/Buttons"));
    assert(!strcmp(key(&s, "controls_b_shown")->key, "B") && at(&s, "controls_b_shown") == all + 2);
    assert(!strcmp(s.keys[all + 2].section, "On-screen controls/Buttons") && !strcmp(shown(&s, "All"), ".ini"));

    /* a pick: a value of its row or a text, never a word; a row at its default is none, the .ini's no value of a
       row whose default is a word */
    assert(!gui_settings_value(&s, "touch") && !gui_settings_value(&s, "language") && !gui_settings_value(&s, "x"));
    assert(gui_settings_set(&s, "touch", "2") && !gui_settings_value(&s, "touch") &&
           !gui_settings_set(&s, "touch", ".ini"));
    assert(gui_settings_set(&s, "touch", "1") && !strcmp(gui_settings_value(&s, "touch"), "1"));
    assert(!strcmp(shown(&s, "touch"), "Fingers"));
    assert(!gui_settings_set(&s, "touch", "3") && !gui_settings_set(&s, "touch", "Fingers and mouse"));
    assert(!strcmp(gui_settings_value(&s, "touch"), "1") && !gui_settings_set(&s, "Touch screen", "2"));
    /* on the screen: a pick as it is; a row not picked the runtime's default in its words, a text as it is, an empty
       one bare, then [default]; an All while none of its rows is; the launcher's own and a page's as they are */
    assert(!strcmp(on_screen(&s, "touch"), "Fingers") && !strcmp(on_screen(&s, "controls"), "Off [default]"));
    assert(!strcmp(on_screen(&s, "gamepad_rumble"), "100% [default]") && !strcmp(on_screen(&s, "log"), "1 [default]"));
    assert(!strcmp(on_screen(&s, "resolution"), "1 [default]") &&
           !strcmp(on_screen(&s, "controls_a_shown"), "[default]"));
    assert(!strcmp(on_screen(&s, "All"), "[default]") && !strcmp(on_screen(&s, "gdbstub"), "off [default]"));
    assert(!strcmp(on_screen(&s, "Buttons"), ">"));
    assert(gui_settings_set(&s, "language", "3") && !strcmp(gui_settings_value(&s, "language"), "3"));
    assert(gui_settings_set(&s, "old", "0.5 0.5 100 1") && gui_settings_set(&s, "language", ""));
    assert(!gui_settings_value(&s, "language") && !gui_settings_set(&s, "nothing", "1"));
    assert(!gui_settings_set(&s, "Buttons", ">") && !gui_settings_set(&s, "All", ".ini"));
    assert(gui_settings_options(&s, 1, NULL, options) == 3 && !strcmp(options[0], "--old=0.5 0.5 100 1"));
    assert(!strcmp(options[1], "--touch=1") && !strcmp(options[2], "--launcher-picks=1"));
    assert(gui_settings_set(&s, "touch", "2") && gui_settings_set(&s, "old", ""));

    /* All: every row of its page to the first one's next word, either way; Mixed when they differ */
    assert(!gui_settings_step_all(&s, page, 1) && !gui_settings_step_all(&s, at(&s, "touch"), 1));
    assert(gui_settings_step_all(&s, all, 1) && !strcmp(shown(&s, "All"), "Hidden"));
    assert(!strcmp(gui_settings_value(&s, "controls_a_shown"), "0") &&
           !strcmp(gui_settings_value(&s, "controls_b_shown"), "0"));
    assert(gui_settings_set(&s, "controls_b_shown", "1"));
    gui_settings_sync(&s);
    assert(!strcmp(shown(&s, "All"), "Mixed") && !strcmp(on_screen(&s, "All"), "Mixed"));
    assert(gui_settings_step_all(&s, all, -1) && !strcmp(shown(&s, "All"), ".ini"));
    assert(gui_settings_step_all(&s, all, 0) && gui_settings_step_all(&s, all, 0) &&
           !strcmp(shown(&s, "All"), "Shown"));
    assert(gui_settings_options(&s, 0, NULL, options) == 3 && !strcmp(options[0], "--controls-a-shown=1") &&
           !strcmp(options[2], "--launcher-picks=1"));
    /* the page's row, or its All, back to where they started: its rows */
    gui_settings_mark(&s);
    gui_settings_step_all(&s, all, 1);
    gui_settings_reset(&s, page);
    assert(!strcmp(shown(&s, "All"), "Shown") && !strcmp(gui_settings_value(&s, "controls_b_shown"), "1"));
    gui_settings_free(&s);
    /* an All of rows whose defaults are words: the word they share with [default] while none is picked, [default]
       alone while their defaults differ, the word without it once one is picked */
    {
        static const char passes[] = "Debug/Passes\tpass_a\t1\tint\t0\t1\tA pass\tA\t0=Off|1=On\n"
                                     "Debug/Passes\tpass_b\t1\tint\t0\t1\tA pass\tB\t0=Off|1=On\n";
        static const char own[] = "Debug/Passes\tpass_a\t1\tint\t0\t1\tA pass\tA\t0=Off|1=On\n"
                                  "Debug/Passes\tpass_b\t1\tint\t0\t1\tA pass\tB\t0=Off|1=On\n"
                                  "Debug/Passes\tother\t\tint\t0\t1\tLLVM's own value\tC\t0=Off|1=On\n";
        int passes_all;
        assert(!gui_settings_read(&s, passes));
        passes_all = at(&s, "pass_a") - 1;
        assert(s.values[passes_all].kind == GUI_SETTING_ALL && !strcmp(on_screen(&s, "pass_a"), "On [default]"));
        {
            char line[64];
            gui_settings_shown(&s, passes_all, line, sizeof line);
            assert(!strcmp(line, "On [default]"));
            assert(gui_settings_step_all(&s, passes_all, 1));
            gui_settings_shown(&s, passes_all, line, sizeof line);
            assert(!strcmp(line, "Off") && !strcmp(gui_settings_value(&s, "pass_b"), "0"));
            assert(gui_settings_step_all(&s, passes_all, 1) && !gui_settings_value(&s, "pass_a"));
            gui_settings_shown(&s, passes_all, line, sizeof line);
            assert(!strcmp(line, "On [default]"));
            gui_settings_free(&s);
            assert(!gui_settings_read(&s, own));
            passes_all = at(&s, "pass_a") - 1;
            gui_settings_shown(&s, passes_all, line, sizeof line);
            assert(!strcmp(line, "[default]") && !strcmp(on_screen(&s, "other"), "[default]"));
            assert(gui_settings_step_all(&s, passes_all, -1)); /* Off for every one, the first's words round */
            gui_settings_shown(&s, passes_all, line, sizeof line);
            assert(!strcmp(line, "Off") && !strcmp(gui_settings_value(&s, "other"), "0"));
            assert(gui_settings_step_all(&s, passes_all, 1) && !gui_settings_value(&s, "pass_a"));
            gui_settings_shown(&s, passes_all, line, sizeof line);
            assert(!strcmp(line, "On") && !strcmp(gui_settings_value(&s, "other"), "1")); /* picked: no [default] */
        }
        gui_settings_free(&s);
    }
    assert(!gui_settings_read(&s, list));

    /* the form edits them: Left and Right go round a row's words, its default's no pick, shown [default] */
    gui_editor_form(&form, s.keys, s.count, s.values, NULL);
    form.row = at(&s, "fullscreen");
    assert(!gui_settings_value(&s, "fullscreen") && !strcmp(on_screen(&s, "fullscreen"), "0 [default]"));
    gui_editor_key(&form, GUI_RIGHT);
    assert(!strcmp(gui_settings_value(&s, "fullscreen"), "1") && !strcmp(on_screen(&s, "fullscreen"), "1"));
    gui_editor_key(&form, GUI_RIGHT);
    assert(!gui_settings_value(&s, "fullscreen") && !strcmp(on_screen(&s, "fullscreen"), "0 [default]"));
    gui_editor_key(&form, GUI_LEFT);
    assert(!strcmp(gui_settings_value(&s, "fullscreen"), "1"));
    gui_editor_key(&form, GUI_LEFT);
    assert(!gui_settings_value(&s, "fullscreen"));
    form.row = at(&s, "touch"); /* three words, the default the last: round them in their order */
    gui_editor_key(&form, GUI_RIGHT);
    assert(!strcmp(gui_settings_value(&s, "touch"), "0") && !strcmp(on_screen(&s, "touch"), "Off"));
    gui_editor_key(&form, GUI_RIGHT);
    assert(!strcmp(on_screen(&s, "touch"), "Fingers"));
    gui_editor_key(&form, GUI_RIGHT);
    assert(!gui_settings_value(&s, "touch") && !strcmp(on_screen(&s, "touch"), "Fingers and mouse [default]"));
    /* the resolution typed, not stepped: 1.61 taken as any scale, a third decimal and a letter left out, and 3x's
       digits are all that reach the row; a value past the range is refused, said with the value typed and the range,
       the row keeping what it had and the typing going on */
    form.row = at(&s, "resolution");
    gui_editor_key(&form, GUI_RIGHT);
    assert(strstr(form.note, "0.25") && strstr(form.note, "16") && strstr(form.note, "two decimals"));
    gui_editor_key(&form, GUI_SELECT);
    gui_editor_type(&form, "2");
    gui_editor_type(&form, ".");
    gui_editor_type(&form, "223");
    gui_editor_type(&form, "x");
    assert(!strcmp(form.text, "2.22") && strstr(form.note, "two decimals"));
    gui_editor_key(&form, GUI_SELECT);
    assert(!strcmp(gui_settings_value(&s, "resolution"), "2.22"));
    gui_editor_key(&form, GUI_SELECT); /* typing it again, now only the digits are left */
    gui_editor_type(&form, "e");
    assert(!strcmp(form.text, "2.22"));
    gui_editor_key(&form, GUI_CANCEL);
    gui_editor_key(&form, GUI_SELECT);
    while (form.text[0]) gui_editor_erase(&form);
    gui_editor_type(&form, "20");
    gui_editor_key(&form, GUI_SELECT);
    assert(!strcmp(gui_settings_value(&s, "resolution"), "2.22") && form.typing);
    assert(!strcmp(form.note, "20 is not a number from 0.25 to 16, two decimals at most"));
    gui_editor_key(&form, GUI_CANCEL);
    assert(!form.typing && !strcmp(gui_settings_value(&s, "resolution"), "2.22"));
    gui_editor_key(&form, GUI_SELECT); /* emptied, Select shows the row's default, which is the value in use */
    while (form.text[0]) gui_editor_erase(&form);
    gui_editor_key(&form, GUI_SELECT);
    assert(!strcmp(gui_settings_value(&s, "resolution"), "1") && !form.note[0]); /* the foot's usual help then */
    { /* 0, however it is typed, is the setting's default 1 as well, and says nothing */
        static const char* const nought[] = {".", "0.", ".0", "0"};
        size_t i;
        for (i = 0; i < sizeof nought / sizeof *nought; ++i) {
            gui_editor_key(&form, GUI_SELECT);
            while (form.text[0]) gui_editor_erase(&form);
            gui_editor_type(&form, nought[i]);
            gui_editor_key(&form, GUI_SELECT);
            assert(!strcmp(gui_settings_value(&s, "resolution"), "1") && !form.note[0]);
        }
    }
    gui_editor_key(&form, GUI_SELECT); /* three digits at most, the point not counted */
    while (form.text[0]) gui_editor_erase(&form);
    gui_editor_type(&form, "123456789");
    assert(!strcmp(form.text, "123")); /* three digits in all, 0.25 and 1.61 too */
    gui_editor_key(&form, GUI_CANCEL);
    assert(gui_settings_set(&s, "resolution", ""));
    gui_editor_key(&form, GUI_SELECT); /* 0.25 and 1.61 fit; 2.0 is the whole number 2 on Enter */
    gui_editor_type(&form, "0.25");
    assert(!strcmp(form.text, "0.25"));
    while (form.text[0]) gui_editor_erase(&form);
    gui_editor_type(&form, "00.01"); /* the leading zero does not use up the three digits */
    assert(!strcmp(form.text, "0.01"));
    while (form.text[0]) gui_editor_erase(&form);
    gui_editor_type(&form, "2.0");
    gui_editor_key(&form, GUI_SELECT);
    assert(!strcmp(gui_settings_value(&s, "resolution"), "2") && !form.note[0]);
    assert(gui_settings_set(&s, "resolution", ""));
    gui_editor_stop(&form);

    /* a run's options: --key=value for each pick in the list's order; the launcher's own with launch */
    for (fullscreen = 0; fullscreen < 2; ++fullscreen)
        for (touch = 0; touch < 3; ++touch)
            for (stub = 0; stub < 2; ++stub)
                for (tmpfs = 0; tmpfs < 2; ++tmpfs)
                    for (extract = 0; extract < 2; ++extract)
                    for (launch = 0; launch < 2; ++launch) {
                        static const char* const picks[] = {"0", "1", "2"}; /* fullscreen's 0, touch's 2: defaults */
                        size_t n, i = 0;
                        char want[32];
                        assert(gui_settings_set(&s, "fullscreen", picks[fullscreen]));
                        assert(gui_settings_set(&s, "touch", picks[touch]));
                        assert(gui_settings_set(&s, "gdbstub", stub ? "on" : "off"));
                        assert(gui_settings_set(&s, "tmpfs", tmpfs ? "on" : "off"));
                        assert(tmpfs ? !strcmp(gui_settings_tmpfs(&s), "--tmpfs") : !gui_settings_tmpfs(&s));
                        assert(gui_settings_set(&s, "extract", extract ? "on" : "off"));
                        assert(extract ? !strcmp(gui_settings_extract(&s), "--extract-container")
                                       : !gui_settings_extract(&s));
                        n = gui_settings_options(&s, launch, NULL, options);
                        assert(n <= (size_t)s.count + 1);
                        if (fullscreen) {
                            snprintf(want, sizeof want, "--fullscreen=%s", picks[fullscreen]);
                            assert(i < n && !strcmp(options[i++], want));
                        }
                        if (touch != 2) {
                            snprintf(want, sizeof want, "--touch=%s", picks[touch]);
                            assert(i < n && !strcmp(options[i++], want));
                        }
                        /* the picks the launcher's for every game, which the game's own go over */
                        if (i) assert(i < n && !strcmp(options[i++], "--launcher-picks=1"));
                        if (launch && tmpfs) assert(i < n && !strcmp(options[i++], "--tmpfs"));
                        if (launch && extract) assert(i < n && !strcmp(options[i++], "--extract-container"));
                        if (launch && stub) {
                            assert(i < n && !strcmp(options[i++], "-S"));
                            assert(i < n && !strcmp(options[i++], "-s"));
                        }
                        assert(i == n);
                    }
    /* a row this build has none of what it needs: the menus show it greyed, and nothing sets it, so no run is passed
       an option its runtime cannot honour */
    assert(gui_settings_set(&s, "tracy_port", "8086") == 0 && !gui_settings_one(&s, "tracy_port", options));
    /* the Debug section's Tracy capture is passed to a run as any other setting of it is, never to the recompiler, and
       a build without the client lists no row for it at all */
    if (tracy) {
        size_t i;
        assert(gui_settings_set(&s, "tracy_capture", "1") && !gui_settings_recompile(&s, 0, NULL, options));
        tracy_n = gui_settings_options(&s, 1, NULL, options);
        for (i = 0; i < tracy_n && strcmp(options[i], "--tracy-capture=1"); ++i) continue;
        assert(i < tracy_n); /* among the run's, where the .ini's other Debug rows are */
        assert(gui_settings_set(&s, "tracy_capture", "0"));
        tracy_n = gui_settings_options(&s, 1, NULL, options);
        for (i = 0; i < tracy_n; ++i) assert(strcmp(options[i], "--tracy-capture=1"));
    }
    gui_settings_free(&s);

    /* kept in a file of the game's sections and keys: what changed from where the rows started, read back over a
       new start; a pick back at its start leaves it */
    {
        char *dir = snx_temp_dir(NULL), *path = snx_path_join(dir, "config/settings.ini"), *text;
        size_t size;
        assert(!gui_settings_read(&s, list) && gui_settings_always(&s, "fullscreen", "0"));
        gui_settings_mark(&s); /* the launcher's own choice, fullscreen 0, is where it starts */
        assert(gui_settings_set(&s, "gdbstub", "on") && gui_settings_set(&s, "touch", "1"));
        assert(gui_settings_set(&s, "language", "3") && gui_settings_set(&s, "fullscreen", "1"));
        assert(gui_settings_set(&s, "log", "2") && gui_settings_set(&s, "log", "1")); /* back at its default */
        assert(gui_settings_set(&s, "controls_b_shown", "0"));
        assert(!gui_settings_save(&s, path));
        text = (char*)snx_read_file(path, &size);
        assert(text &&
               strstr(text, "\n[Launch]\ngdbstub=on\n[Display]\nfullscreen=1\nlanguage=3\n"
                            "[On-screen controls]\ntouch=1\n[On-screen controls/Buttons]\ncontrols_b_shown=0\n"));
        assert(!strstr(text, "log=") && !strstr(text, "docked") && !strstr(text, "All"));
        free(text);
        gui_settings_free(&s);
        assert(!gui_settings_read(&s, list) && gui_settings_always(&s, "fullscreen", "0"));
        gui_settings_mark(&s);
        assert(!gui_settings_load(&s, path, NULL, 0));
        assert(!strcmp(gui_settings_value(&s, "gdbstub"), "on") && !strcmp(gui_settings_value(&s, "language"), "3"));
        assert(!strcmp(gui_settings_value(&s, "fullscreen"), "1") && !strcmp(gui_settings_value(&s, "touch"), "1"));
        assert(!gui_settings_value(&s, "log") && !strcmp(gui_settings_value(&s, "controls_b_shown"), "0"));
        assert(!strcmp(shown(&s, "All"), "Mixed") && gui_settings_options(&s, 1, NULL, options) == 7 &&
               !strcmp(options[4], "--launcher-picks=1") && !strcmp(options[5], "-S"));
        /* a row back to where it started, then every one; how many a reset of every one changes, counted */
        changed = gui_settings_changed(&s); /* gdbstub, language, fullscreen, touch, B; not the All made Mixed */
        assert(changed == 5);
        gui_settings_reset(&s, at(&s, "language"));
        assert(!gui_settings_value(&s, "language") && !strcmp(gui_settings_value(&s, "touch"), "1"));
        assert(gui_settings_changed(&s) == changed - 1);
        gui_settings_reset(&s, -1);
        assert(!gui_settings_changed(&s));
        assert(!strcmp(gui_settings_value(&s, "fullscreen"), "0") && !gui_settings_value(&s, "touch"));
        assert(!gui_settings_value(&s, "gdbstub") && gui_settings_options(&s, 1, NULL, options) == 2);
        assert(!strcmp(shown(&s, "All"), ".ini"));
        gui_settings_load(&s, path, NULL, 0);                        /* and the file's picks again */
        gui_settings_load(&s, "/nonexistent/settings.ini", NULL, 0); /* none: nothing changes */
        assert(!strcmp(gui_settings_value(&s, "language"), "3"));
        /* what the file holds that no row takes: an older version's key, which Clean Old Settings takes out and a save
           keeps; a value its row does not have, said with the row's own; a key given twice, said with both lines */
        {
            char unread[256], old[1024];
            assert(!gui_settings_unread(&s, path, unread, sizeof unread) && !unread[0]);
            assert(!gui_settings_unread(&s, NULL, unread, sizeof unread) && !unread[0]);
            text = (char*)snx_read_file(path, &size);
            assert(text);
            {
                char* more = snx_format("%s[Display]\nrenderer=opengl\ntouch=9\nlanguage=4\n", text);
                assert(!snx_write_file(path, more, strlen(more)));
                free(more);
            }
            free(text);
            assert(gui_settings_unread(&s, path, unread, sizeof unread) == 1);
            assert(!strcmp(unread, "renderer=opengl"));
            assert(!strcmp(gui_settings_value(&s, "language"), "3") && !strcmp(gui_settings_value(&s, "touch"), "1"));
            assert(gui_settings_unread(&s, path, unread, 10) == 1 && !strcmp(unread, "renderer="));
            snx_config_forget();
            assert(gui_settings_load(&s, path, old, sizeof old) == 2);
            assert(strstr(old, "touch=9 is not a value of Touch screen: touch=1 now") &&
                   strstr(old, "language is given twice, language=3 on line ") && strstr(old, "language=4 on line ") &&
                   !strcmp(gui_settings_value(&s, "touch"), "1") && !strcmp(gui_settings_value(&s, "language"), "4"));
            assert(!gui_settings_save(&s, path) && gui_settings_unread(&s, path, unread, sizeof unread) == 1);
            assert(!gui_settings_clean(&s, path) && !gui_settings_unread(&s, path, unread, sizeof unread));
        }
        {
            char *plain = snx_path_join(dir, "a file"), *under = snx_path_join(plain, "settings.ini");
            assert(!snx_write_file(plain, "", 0) && gui_settings_save(&s, under)); /* a folder it cannot make */
            free(under);
            free(plain);
        }
        gui_settings_free(&s);
        assert(!snx_remove_tree(dir));
        free(path);
        free(dir);
    }
    /* Fullscreen and Show controls, the launcher's own choices: Off or On to every run, never the .ini's, [default]
       after the one they start with, which Del and a reset put back. An older version's .ini, which passed nothing,
       is read as that one and said with the value now */
    {
        char *dir = snx_temp_dir(NULL), *path = snx_path_join(dir, "settings.ini"), *text, old[512];
        const char* older = "[Display]\nfullscreen=.ini\nlanguage=3\n[On-screen controls]\ncontrols=.ini\n";
        size_t size;
        int row;
        assert(!gui_settings_read(&s, list));
        /* a word row of the runtime's with values, set to one of them */
        assert(!gui_settings_always(&s, "language", "0") && !gui_settings_always(&s, "gdbstub", "on"));
        assert(!gui_settings_always(&s, "nothing", "0") && !gui_settings_always(&s, "Buttons", ">"));
        assert(!gui_settings_always(&s, "controls", ".ini") && !gui_settings_always(&s, "controls", "2"));
        assert(words(key(&s, "controls")) == 2 && !gui_settings_value(&s, "controls")); /* plain, at its default */
        assert(gui_settings_always(&s, "fullscreen", "0") && gui_settings_always(&s, "controls", "0"));
        assert(words(key(&s, "controls")) == 2 && !strcmp(key(&s, "controls")->words[0], "Off") &&
               !strcmp(key(&s, "controls")->words[1], "On"));
        assert(gui_settings_always(&s, "controls", "0") && words(key(&s, "controls")) == 2); /* again: as it was */
        gui_settings_mark(&s);
        assert(!strcmp(on_screen(&s, "controls"), "Off [default]") && !strcmp(gui_settings_value(&s, "controls"), "0"));
        assert(!strcmp(on_screen(&s, "fullscreen"), "0 [default]") && !gui_settings_changed(&s));
        assert(gui_settings_options(&s, 0, NULL, options) == 3 && !strcmp(options[0], "--fullscreen=0") &&
               !strcmp(options[1], "--controls=0") && !strcmp(options[2], "--launcher-picks=1"));
        /* the form goes round Off and On, either way, and Select steps */
        gui_editor_form(&form, s.keys, s.count, s.values, NULL);
        form.row = row = at(&s, "controls");
        gui_editor_key(&form, GUI_RIGHT);
        assert(!strcmp(gui_settings_value(&s, "controls"), "1") && !strcmp(on_screen(&s, "controls"), "On"));
        gui_editor_key(&form, GUI_RIGHT);
        assert(!strcmp(gui_settings_value(&s, "controls"), "0") && !strcmp(on_screen(&s, "controls"), "Off [default]"));
        gui_editor_key(&form, GUI_LEFT);
        assert(!strcmp(gui_settings_value(&s, "controls"), "1") && !strcmp(on_screen(&s, "controls"), "On"));
        gui_editor_key(&form, GUI_LEFT);
        assert(!strcmp(on_screen(&s, "controls"), "Off [default]") && gui_settings_at_start(&s, row));
        gui_editor_key(&form, GUI_SELECT);
        assert(!strcmp(on_screen(&s, "controls"), "On") && !gui_settings_at_start(&s, row));
        assert(gui_settings_changed(&s) == 1);
        gui_editor_stop(&form);
        /* .ini is no value of theirs; Del puts the start back */
        assert(!gui_settings_set(&s, "controls", ".ini") && !strcmp(gui_settings_value(&s, "controls"), "1"));
        assert(!gui_settings_set(&s, "fullscreen", ".ini") && !strcmp(gui_settings_value(&s, "fullscreen"), "0"));
        gui_settings_reset(&s, row);
        assert(gui_settings_at_start(&s, row) && !strcmp(on_screen(&s, "controls"), "Off [default]"));
        /* kept: one off its start alone */
        assert(gui_settings_set(&s, "fullscreen", "1") && !strcmp(on_screen(&s, "fullscreen"), "1"));
        assert(!gui_settings_save(&s, path));
        text = (char*)snx_read_file(path, &size);
        assert(text && strstr(text, "\n[Display]\nfullscreen=1\n") && !strstr(text, "controls"));
        free(text);
        /* an older version's .ini: each the value its row starts with, a phone's On too, said with it */
        assert(!snx_write_file(path, older, strlen(older)));
        gui_settings_free(&s);
        assert(!gui_settings_read(&s, list) && gui_settings_always(&s, "fullscreen", "0") &&
               gui_settings_always(&s, "controls", "1"));
        gui_settings_mark(&s);
        assert(gui_settings_load(&s, path, old, sizeof old) == 2);
        assert(!strcmp(old, "fullscreen=.ini is no longer a value of fullscreen: fullscreen=0 now; "
                            "controls=.ini is no longer a value of Show controls: controls=1 now"));
        assert(!strcmp(gui_settings_value(&s, "fullscreen"), "0") && !strcmp(gui_settings_value(&s, "controls"), "1"));
        assert(!strcmp(gui_settings_value(&s, "language"), "3") && gui_settings_changed(&s) == 1);
        assert(!strcmp(on_screen(&s, "controls"), "On [default]") && gui_settings_set(&s, "controls", "0") &&
               !strcmp(on_screen(&s, "controls"), "Off") && gui_settings_set(&s, "controls", "1"));
        assert(gui_settings_load(&s, path, old, 20) == 2 && !strcmp(old, "fullscreen=.ini is ")); /* cut */
        assert(gui_settings_load(&s, path, NULL, 0) == 2);
        /* as the next start finds it once written again: neither, nothing said */
        assert(!gui_settings_save(&s, path));
        text = (char*)snx_read_file(path, &size);
        assert(text && strstr(text, "\n[Display]\nlanguage=3\n") && !strstr(text, "fullscreen") &&
               !strstr(text, "controls"));
        free(text);
        assert(!gui_settings_load(&s, path, old, sizeof old) && !old[0]);
        gui_settings_free(&s);
        /* the same for every row whose default is a word: its .ini read as that default, and the default picked, which
           runs were given over the game's .ini, no pick now, each said; an empty default's .ini is still its value */
        {
            const char* older_rows = "[Display]\ndocked=1\n[On-screen controls]\ntouch=.ini\n"
                                     "[On-screen controls/Buttons]\ncontrols_b_shown=.ini\n[Debug]\nlog=2\n";
            assert(!snx_write_file(path, older_rows, strlen(older_rows)));
            assert(!gui_settings_read(&s, list));
            gui_settings_mark(&s);
            assert(gui_settings_load(&s, path, old, sizeof old) == 2);
            assert(!strcmp(old, "docked=1 is the default of docked: no run is given it over the game's .ini; "
                                "touch=.ini is no longer a value of Touch screen: touch=2 now"));
            assert(!gui_settings_value(&s, "docked") && !gui_settings_value(&s, "touch") &&
                   !gui_settings_value(&s, "controls_b_shown") && !strcmp(gui_settings_value(&s, "log"), "2"));
            assert(!strcmp(on_screen(&s, "docked"), "1 [default]") && gui_settings_options(&s, 0, NULL, options) == 2 &&
                   !strcmp(options[0], "--log=2"));
            assert(!gui_settings_save(&s, path));
            text = (char*)snx_read_file(path, &size);
            assert(text && strstr(text, "\n[Debug]\nlog=2\n") && !strstr(text, "docked") && !strstr(text, "touch") &&
                   !strstr(text, "controls_b_shown"));
            free(text);
            assert(!gui_settings_load(&s, path, old, sizeof old) && !old[0]);
            gui_settings_free(&s);
        }
        /* a default none of its words has is no value of an always row either: never its first word */
        assert(!gui_settings_read(&s, "Display\tshape\t2\tint\t0\t2\tA default no word has\tShape\t0=Off|1=On\n"));
        assert(gui_settings_always(&s, "shape", "1") && !gui_settings_set(&s, "shape", "2"));
        assert(!strcmp(gui_settings_value(&s, "shape"), "1"));
        gui_settings_free(&s);
        /* a renamed key's pick (ini.h's ini_renamed): its new row's, said once, and the file written with that one
           alone; a value the row has not is said with the row's; the new key given too, the old one is not read */
        {
            const char* descriptors = "Debug\tvk_descriptors_mode\t3\tint\t0\t3\tGraphics descriptors\t"
                                      "Descriptor buffers\t0=Sets|1=Buffers|2=Heaps|3=Automatic\n";
            const char* picks = "[Debug]\nvk_descriptors=1\n";
            assert(!snx_write_file(path, picks, strlen(picks)));
            assert(!gui_settings_read(&s, descriptors));
            gui_settings_mark(&s);
            assert(gui_settings_load(&s, path, old, sizeof old) == 1);
            assert(!strcmp(old, "vk_descriptors=1 is deprecated: Descriptor buffers is vk_descriptors_mode=1 now"));
            assert(!strcmp(gui_settings_value(&s, "vk_descriptors_mode"), "1") &&
                   gui_settings_options(&s, 0, NULL, options) == 2 && !strcmp(options[0], "--vk-descriptors-mode=1"));
            assert(!gui_settings_save(&s, path));
            text = (char*)snx_read_file(path, &size);
            assert(text && strstr(text, "\n[Debug]\nvk_descriptors_mode=1\n") && !strstr(text, "vk_descriptors="));
            free(text);
            assert(!gui_settings_load(&s, path, old, sizeof old) && !old[0]);
            gui_settings_free(&s);
            picks = "[Debug]\nvk_descriptors=7\n";
            assert(!snx_write_file(path, picks, strlen(picks)));
            assert(!gui_settings_read(&s, descriptors));
            gui_settings_mark(&s);
            assert(gui_settings_load(&s, path, old, sizeof old) == 1 && !gui_settings_value(&s, "vk_descriptors_mode"));
            assert(!strcmp(old, "vk_descriptors=7 is deprecated and no value of Descriptor buffers: "
                                "vk_descriptors_mode=3 now"));
            gui_settings_free(&s);
            picks = "[Debug]\nvk_descriptors=1\nvk_descriptors_mode=2\n";
            assert(!snx_write_file(path, picks, strlen(picks)));
            assert(!gui_settings_read(&s, descriptors));
            gui_settings_mark(&s);
            assert(!gui_settings_load(&s, path, old, sizeof old) && !old[0]);
            assert(!strcmp(gui_settings_value(&s, "vk_descriptors_mode"), "2"));
            gui_settings_free(&s);
        }
        /* emulated_cores's pick, renamed to emulated_cores_mode of the same 0 and 1: On and Off carried over, and the
           row offers Auto, its default, beside them */
        {
            const char* cores = "Debug\temulated_cores_mode\t2\tint\t0\t2\tCores held by guest threads\t"
                                "Emulated cores\t0=Off|1=On|2=Auto\n";
            const char* picks = "[Debug]\nemulated_cores=1\n";
            assert(!snx_write_file(path, picks, strlen(picks)));
            assert(!gui_settings_read(&s, cores));
            gui_settings_mark(&s);
            assert(!gui_settings_value(&s, "emulated_cores_mode")); /* Auto, its default: no pick */
            assert(gui_settings_load(&s, path, old, sizeof old) == 1);
            assert(!strcmp(old, "emulated_cores=1 is deprecated: Emulated cores is emulated_cores_mode=1 now"));
            assert(!strcmp(gui_settings_value(&s, "emulated_cores_mode"), "1") &&
                   gui_settings_options(&s, 0, NULL, options) == 2 && !strcmp(options[0], "--emulated-cores-mode=1"));
            assert(!gui_settings_save(&s, path));
            text = (char*)snx_read_file(path, &size);
            assert(text && strstr(text, "\n[Debug]\nemulated_cores_mode=1\n") && !strstr(text, "emulated_cores="));
            free(text);
            gui_settings_free(&s);
            picks = "[Debug]\nemulated_cores=0\n";
            assert(!snx_write_file(path, picks, strlen(picks)));
            assert(!gui_settings_read(&s, cores));
            gui_settings_mark(&s);
            assert(gui_settings_load(&s, path, old, sizeof old) == 1);
            assert(!strcmp(gui_settings_value(&s, "emulated_cores_mode"), "0"));
            gui_settings_free(&s);
        }
        assert(!snx_remove_tree(dir));
        free(path);
        free(dir);
    }
    assert(!gui_settings_read(&s, "") && s.count == s.own); /* no runtime: the launcher's alone */
    assert(gui_settings_options(&s, 1, NULL, options) == 0 && !gui_settings_set(&s, "touch", "1"));
    /* and a pick of theirs kept then: the runtime's picks in the file, which no row has, stay as they were, byte for
       byte */
    {
        char *dir = snx_temp_dir(NULL), *path = snx_path_join(dir, "settings.ini"), *text;
        const char* picks = "; picks\n[Launch]\ngdbstub=on\n[Display]\nfullscreen = 1\nlanguage=3\n"
                            "[On-screen controls/Buttons]\ncontrols_b_shown=0\n";
        const char* theirs =
            "[Display]\nfullscreen = 1\nlanguage=3\n[On-screen controls/Buttons]\ncontrols_b_shown=0\n";
        size_t size;
        assert(!snx_write_file(path, picks, strlen(picks)));
        gui_settings_mark(&s);
        gui_settings_load(&s, path, NULL, 0);
        assert(!strcmp(gui_settings_value(&s, "gdbstub"), "on") && gui_settings_set(&s, "gdbstub", ".ini"));
        assert(!gui_settings_save(&s, path));
        text = (char*)snx_read_file(path, &size);
        assert(text && strstr(text, theirs) && !strstr(text, "gdbstub") && !strstr(text, "[Launch]"));
        free(text);
        /* the launcher's own again: first, by their section, the runtime's after them */
        assert(gui_settings_set(&s, "gdbstub", "on") && !gui_settings_save(&s, path));
        text = (char*)snx_read_file(path, &size);
        assert(text && strstr(text, "\n[Launch]\ngdbstub=on\n[Display]\nfullscreen = 1\n") && strstr(text, theirs));
        free(text);
#ifndef _WIN32
        if (geteuid()) { /* one it cannot read is not written over, which would lose them */
            assert(!chmod(path, 0) && gui_settings_save(&s, path) && !chmod(path, 0600));
            text = (char*)snx_read_file(path, &size);
            assert(text && strstr(text, theirs) && strstr(text, "gdbstub=on"));
            free(text);
        }
#endif
        assert(!snx_remove_tree(dir));
        free(path);
        free(dir);
    }
    /* a resolution forced into the settings file as nothing, or no number, is no pick: the game's own .ini keeps its
       default 1, which the row shows as it is; a value in range is the pick a run gets */
    {
        char *dir = snx_temp_dir(NULL), *path = snx_path_join(dir, "settings.ini");
        static const char* const bad[] = {"", "0", "e", "3x", "high", "nan", "0.01x"};
        size_t i;
        for (i = 0; i < sizeof bad / sizeof *bad; ++i) {
            char* text = snx_format("[Graphics]\nresolution=%s\n", bad[i]);
            assert(text && !snx_write_file(path, text, strlen(text)));
            free(text);
            gui_settings_free(&s);
            assert(!gui_settings_read(&s, list) && gui_settings_set(&s, "fullscreen", "0"));
            gui_settings_mark(&s);
            gui_settings_load(&s, path, NULL, 0);
            assert(!gui_settings_value(&s, "resolution"));
            assert(!strcmp(on_screen(&s, "resolution"), "1 [default]"));
            {
                size_t n = gui_settings_options(&s, 0, NULL, options), j;
                for (j = 0; j < n; ++j) assert(strncmp(options[j], "--resolution=", 13));
            }
        }
        {
            char* text = snx_format("[Graphics]\nresolution=8.2\n");
            assert(text && !snx_write_file(path, text, strlen(text)));
            free(text);
            gui_settings_free(&s);
            assert(!gui_settings_read(&s, list) && gui_settings_set(&s, "fullscreen", "0"));
            gui_settings_mark(&s);
            gui_settings_load(&s, path, NULL, 0);
            assert(!strcmp(gui_settings_value(&s, "resolution"), "8.2"));
            {
                size_t n = gui_settings_options(&s, 0, NULL, options), j;
                int found = 0;
                for (j = 0; j < n; ++j) found += !strcmp(options[j], "--resolution=8.2");
                assert(found == 1);
            }
        }
        assert(!snx_remove_tree(dir));
        free(path);
        free(dir);
        assert(!gui_settings_read(&s, list));
    }
    gui_settings_free(&s);

    /* the runtime's own list: every line a row, and a page's two; the game's menu's settings named and worded as it
       shows them, but a control's place, its own */
    if (argc > 1) {
        const char* run[] = {argv[1], "--settings", NULL};
        char* output = NULL;
        const char *c, *next, *last = NULL;
        int lines = 0, pages = 0;
        assert(!snx_run(run, NULL, NULL, &output) && output);
        for (c = output; *c; c = next, ++lines) {
            size_t section = strcspn(c, "\t\n");
            /* a page: a run of lines whose section has a '/', a row and an All more */
            if (memchr(c, '/', section) && !(last && strcspn(last, "\t") == section && !strncmp(c, last, section)))
                ++pages;
            last = c;
            next = c + strcspn(c, "\n");
            if (*next) ++next;
        }
        /* and the Room password's row, beside the room's */
        assert(pages > 0 && !gui_settings_read(&s, output) && s.count == s.own + lines + 2 * pages + 1);
        assert(s.values[find_kind(&s, GUI_SETTING_SECRET)].key &&
               !strcmp(s.values[find_kind(&s, GUI_SETTING_SECRET)].key, "room_password"));
        /* each player's own, on Controllers' Player page: a word row whose default is every player's */
        assert(words(key(&s, "gamepad_p1_swap_ab")) == 3 && !strcmp(key(&s, "gamepad_p8_swap_ab")->words[0], "As All"));
        assert(!strcmp(on_screen(&s, "gamepad_p8_swap_ab"), "As All [default]"));
        /* every word row: its words alone, the default's shown [default], or .ini first where its default has none */
        {
            int i, w, plain = 0;
            for (i = 0; i < s.count; ++i) {
                char line[160];
                if (s.keys[i].type != GUI_KEY_WORD || s.values[i].kind) continue;
                gui_settings_shown(&s, i, line, sizeof line);
                assert(strstr(line, "[default]"));
                plain += s.values[i].plain;
                for (w = 0; s.keys[i].words[w]; ++w)
                    assert(!strcmp(s.keys[i].words[w], ".ini") == (!s.values[i].plain && !w));
            }
            assert(plain > 50);
        }
        assert(key(&s, "gamepad_p3_device")->type == GUI_KEY_TEXT &&
               !strcmp(key(&s, "gamepad_p3_device")->section, "Controllers/Player"));
        assert(!strcmp(s.keys[s.own].section, "Game") && !strcmp(s.keys[s.count - 1].section, "Debug"));
        assert(words(key(&s, "fullscreen")) == 2 && words(key(&s, "touch")) == 3 && words(key(&s, "controls")) == 3);
        assert(!strcmp(key(&s, "controls")->key, "Show controls") &&
               !strcmp(key(&s, "touch")->words[2], "Fingers and mouse"));
        assert(!strcmp(key(&s, "gamepad_rumble")->words[0], "Off") && !key(&s, "controls_a"));
        /* the game's Volume, Audio's: a slider of every 5 percent, Mute first */
        assert(s.values[at(&s, "volume")].slider && words(key(&s, "volume")) == 21 &&
               !strcmp(key(&s, "volume")->words[0], "Mute") && !strcmp(key(&s, "volume")->words[20], "100%") &&
               !strcmp(key(&s, "volume")->section, "Audio") && !strcmp(on_screen(&s, "volume"), "100% [default]"));
        assert(gui_settings_set(&s, "fullscreen", "0") && gui_settings_set(&s, "touch", "1"));
        assert(gui_settings_set(&s, "controls", "0") && key(&s, "resolution")->type == GUI_KEY_TEXT);
        /* as the launcher has them: fullscreen Off or On, Off its [default]; the controls Auto, their [default] */
        assert(gui_settings_always(&s, "fullscreen", "0") && gui_settings_always(&s, "controls", "2"));
        gui_settings_mark(&s);
        assert(words(key(&s, "fullscreen")) == 2 && words(key(&s, "controls")) == 3);
        assert(!strcmp(on_screen(&s, "fullscreen"), "Off [default]") &&
               !strcmp(on_screen(&s, "controls"), "Auto [default]"));
        /* a range as settings.txt has it, a large one to the last digit, which a typed value's dialog says; a number
           with steps (settings.txt's steps) a word row of them instead, open to a value between two */
        assert(key(&s, "vita_lockstep_steps")->numeric && key(&s, "vita_lockstep_steps")->hi == 2147483647.0);
        assert(key(&s, "map_limit_mb")->type == GUI_KEY_WORD && s.values[at(&s, "map_limit_mb")].open &&
               !strcmp(key(&s, "map_limit_mb")->words[0], "No limit") && gui_settings_set(&s, "map_limit_mb", "300") &&
               !strcmp(on_screen(&s, "map_limit_mb"), "300") && gui_settings_set(&s, "map_limit_mb", "0"));
        assert(key(&s, "resolution")->lo == 0.25 && key(&s, "resolution")->hi == 16.0);
        page = at(&s, "Buttons");
        assert(page > 0 && s.values[page].count == 1 + 17 && !strcmp(s.keys[page + 2].key, "L"));
        /* a setting of some systems alone says where on every other, last on its line: the Mac's Vulkan driver, the
           phone's buzz; one of every system says nothing, nor do the launcher's own rows */
        assert(!s.values[at(&s, "fullscreen")].only && !s.values[at(&s, "gdbstub")].only);
#if defined(__APPLE__) && !defined(SNX_IOS)
        assert(!s.values[at(&s, "vulkan_driver")].only);
#else
        assert(!strcmp(s.values[at(&s, "vulkan_driver")].only, "macOS only"));
#endif
#if defined(__ANDROID__)
        assert(!s.values[at(&s, "controls_haptics")].only);
#else
        assert(!strcmp(s.values[at(&s, "controls_haptics")].only, "Android only"));
#endif
        assert(words(key(&s, "vulkan_driver")) == 3 && !strcmp(on_screen(&s, "vulkan_driver"), "Auto [default]"));
        /* each game's setting the recompile takes too is a row, which a run that makes the game (-r) gets picked off its
           default, NCE Off among them, and a run with the game's options alone */
        {
            const IniOpt* o;
            int nce = 0;
            for (o = g_recompile_game_ini; o->key; ++o) {
                char value[32], want[96];
                if (o->type == INI_TEXT)
                    snprintf(value, sizeof value, "100000");
                else
                    snprintf(value, sizeof value, "%g", atof(o->def) == o->lo ? o->hi : o->lo);
                ini_option(want, sizeof want, o->key, value);
                assert(at(&s, o->key) >= s.own && gui_settings_set(&s, o->key, value));
                assert(gui_settings_recompile(&s, 1, NULL, options) == 1 && !strcmp(options[0], want));
                assert(!gui_settings_recompile(&s, 0, NULL, options) && gui_settings_set(&s, o->key, o->def) &&
                       !gui_settings_recompile(&s, 1, NULL, options));
                nce += !strcmp(want, "--nce=0");
            }
            assert(nce == 1);
        }
        /* a game's own picks (<name>.picks beside its .ini): its pick of a setting the recompile takes goes to its
           recompile and to its runs over the launcher's, since the recompile reads its options alone; its other picks
           are the runtime's to read, a run given the launcher's with --launcher-picks=1 */
        {
            static const char own[] = "[Performance]\nnce=0\n[Audio]\nvolume=40\n", on[] = "[Performance]\nnce=1\n";
            char *dir = snx_temp_dir(NULL), *ini = snx_path_join(dir, "game.ini"), *picks = snx_game_picks(ini);
            size_t n, j;
            int nce = 0, volume = 0;
            assert(picks && !strcmp(picks + strlen(picks) - 10, "game.picks")); /* after either separator */
            assert(!snx_write_file(picks, own, sizeof own - 1));
            assert(gui_settings_recompile(&s, 1, ini, options) == 1 && !strcmp(options[0], "--nce=0"));
            assert(!gui_settings_recompile(&s, 1, NULL, options) && !gui_settings_recompile(&s, 0, ini, options));
            /* the launcher's picks none of them: the game's NCE alone, its volume the runtime's to read */
            n = gui_settings_options(&s, 0, ini, options);
            for (j = 0; j < n; ++j) {
                nce += !strncmp(options[j], "--nce=", 6) ? !strcmp(options[j], "--nce=0") ? 1 : 100 : 0;
                volume += !strncmp(options[j], "--volume=", 9);
            }
            assert(nce == 1 && !volume && !strcmp(options[n - 1], "--launcher-picks=1"));
            /* the launcher's NCE Off and Volume 75, the game's NCE On: NCE On, the launcher's 75 */
            assert(!snx_write_file(picks, on, sizeof on - 1));
            assert(gui_settings_set(&s, "nce", "0") && gui_settings_set(&s, "volume", "75"));
            assert(gui_settings_recompile(&s, 1, ini, options) == 1 && !strcmp(options[0], "--nce=1"));
            n = gui_settings_options(&s, 0, ini, options);
            for (j = 0, nce = 0; j < n; ++j) {
                nce += !strncmp(options[j], "--nce=", 6) ? !strcmp(options[j], "--nce=1") ? 1 : 100 : 0;
                volume += !strcmp(options[j], "--volume=75");
            }
            assert(nce == 1 && volume == 1 && !strcmp(options[n - 1], "--launcher-picks=1"));
            /* no picks file: the launcher's */
            n = gui_settings_options(&s, 0, "/nonexistent/game.ini", options);
            for (j = 0, nce = 0; j < n; ++j) nce += !strcmp(options[j], "--nce=0");
            assert(nce == 1);
            assert(gui_settings_set(&s, "nce", "1") && gui_settings_set(&s, "volume", "100"));
            assert(!snx_remove_tree(dir));
            free(picks);
            free(ini);
            free(dir);
        }
        gui_settings_free(&s);
        free(output);
    }
    /* where a setting does something, the line's last field, when this system is not one: kept for its greyed row,
       its words and its pick as any row's, passed to a run and saved as they were; none for a line without the
       field (an older runtime's) or with it empty */
    {
        static const char list[] =
            "Graphics\tvulkan_driver\tauto\ttext\t0\t0\tThe driver\tVulkan driver\tauto=Auto|moltenvk=MoltenVK\t"
            "macOS only\n"
            "Graphics\tvsync\t1\tint\t0\t1\tNo tearing\tVSync\t0=Off|1=On\t\n"
            "Graphics\tshadows\t1\tint\t0\t1\tShadow maps\tShadows\t0=Off|1=On\n"
            "Debug\tprofile\t0\tint\t0\t1\tThe profiler\tProfiler\t0=Off|1=On\tWindows only";
        assert(!gui_settings_read(&s, list));
        assert(!strcmp(s.values[at(&s, "vulkan_driver")].only, "macOS only") && !s.values[at(&s, "vsync")].only &&
               !s.values[at(&s, "shadows")].only && !strcmp(s.values[at(&s, "profile")].only, "Windows only"));
        assert(words(key(&s, "vulkan_driver")) == 2 && !strcmp(key(&s, "vulkan_driver")->words[1], "MoltenVK") &&
               words(key(&s, "profile")) == 2 && !strcmp(on_screen(&s, "vulkan_driver"), "Auto [default]"));
        assert(gui_settings_set(&s, "vulkan_driver", "moltenvk") && gui_settings_options(&s, 0, NULL, options) == 2 &&
               !strcmp(options[0], "--vulkan-driver=moltenvk") && !strcmp(on_screen(&s, "vulkan_driver"), "MoltenVK"));
        gui_settings_free(&s);
    }
    /* the Debug settings a run of a game takes off their default: its .ini's, a pick over it, a text by its key alone,
       the GDB stub; another section's never */
    {
        static const char debug[] = "Display\tfullscreen\t0\tint\t0\t1\tWhole screen\tFullscreen\t0=Off|1=On\n"
                                    "Debug\tlog\t1\tint\t0\t2\tLog detail\t\t0=0|1=1|2=2\n"
                                    "Debug\tvk_budget_mb\t0\tint\t0\t2147483647\tBudget\tVulkan memory budget\t\n"
                                    "Debug\trenderdoc\t\ttext\t0\t0\tCaptures\tRenderDoc captures\t";
        static const char file[] =
            "[Display]\nfullscreen=1\n[Debug]\nlog=1\nvk_budget_mb=512\nrenderdoc=/home/someone/caps\n";
        char *dir = snx_temp_dir(NULL), *ini = snx_path_join(dir, "game.ini"), out[256];
        assert(!gui_settings_read(&s, debug));
        assert(gui_settings_debug(&s, NULL, 1, out, sizeof out) == 0 && !out[0]);
        assert(!snx_write_file(ini, file, strlen(file)));
        assert(gui_settings_debug(&s, ini, 0, out, sizeof out) == 2 && !strcmp(out, "vk_budget_mb=512, renderdoc"));
        assert(gui_settings_set(&s, "log", "2") && gui_settings_set(&s, "vk_budget_mb", "0"));
        assert(gui_settings_debug(&s, ini, 0, out, sizeof out) == 2); /* the file's alone: the picks left out */
        assert(gui_settings_debug(&s, ini, 1, out, sizeof out) == 2 && !strcmp(out, "log=2, renderdoc"));
        assert(gui_settings_set(&s, "gdbstub", "on"));
        assert(gui_settings_debug(&s, NULL, 1, out, sizeof out) == 2 && !strcmp(out, "log=2, gdbstub"));
        assert(gui_settings_debug(&s, ini, 1, out, 8) == 3 && !strcmp(out, "log=2, ")); /* cut, every one counted */
        assert(gui_settings_debug(&s, "/nonexistent/game.ini", 0, out, sizeof out) == 0);
        /* the game's own picks over the launcher's and over its .ini, with the launcher's or alone */
        {
            static const char own[] = "[Debug]\nlog=0\nvk_budget_mb=64\n";
            char* picks = snx_game_picks(ini);
            assert(!snx_write_file(picks, own, sizeof own - 1));
            assert(gui_settings_debug(&s, ini, 0, out, sizeof out) == 3 &&
                   !strcmp(out, "log=0, vk_budget_mb=64, renderdoc"));
            assert(gui_settings_debug(&s, ini, 1, out, sizeof out) == 4 &&
                   !strcmp(out, "log=0, vk_budget_mb=64, renderdoc, gdbstub"));
            free(picks);
        }
        gui_settings_free(&s);
        assert(!snx_remove_tree(dir));
        free(ini);
        free(dir);
    }
    /* a game's own picks (gui_settings_read_game): the runtime's rows alone, each starting at Global, which no pick has,
       no plain row nor slider; Global and the launcher's value shown while not picked, a pick saved by section to its
       picks file and read back, its default too, and Global back by a reset; what a game has of its own said, its
       picks then its box's GPU driver and Freedreno variables */
    {
        static const char list[] = "Display\tfullscreen\t0\tint\t0\t1\tWhole screen\tFullscreen\t0=Off|1=On\n"
                                   "Graphics\tresolution\t1\tfloat\t0.25\t16\tScale\tResolution\t\n"
                                   "Audio\tvolume\t100\tint\t0\t100\tLoud\tVolume\t0=Mute|50=50%|100=100%\t\t\tslider";
        GuiSettings game = {0}, global = {0};
        char *dir = snx_temp_dir(NULL), *ini = snx_path_join(dir, "game.ini"), *picks = snx_game_picks(ini), *text,
             out[256];
        size_t size;
        int v;
        assert(!gui_settings_read_game(&game, list) && !gui_settings_read(&global, list));
        assert(game.game && !game.own && game.count == 3 && !global.game && global.own > 0);
        v = at(&game, "volume");
        assert(!game.values[v].plain && !game.values[v].slider && !game.values[v].word &&
               !strcmp(key(&game, "volume")->words[0], "Global") && global.values[at(&global, "volume")].slider);
        gui_settings_mark(&game);
        assert(!gui_settings_value(&game, "volume") && !gui_settings_value(&game, "fullscreen") &&
               !gui_settings_changed(&game));
        assert(gui_settings_set(&global, "volume", "50"));
        gui_settings_shown_game(&game, &global, v, out, sizeof out);
        assert(!strcmp(out, "Global (50%)"));
        gui_settings_shown_game(&game, &global, at(&game, "fullscreen"), out, sizeof out);
        assert(!strcmp(out, "Global (Off [default])"));
        /* its default picked: a pick of its own, over the launcher's 50 */
        assert(gui_settings_set(&game, "volume", "100") && !strcmp(gui_settings_value(&game, "volume"), "100"));
        gui_settings_shown_game(&game, &global, v, out, sizeof out);
        assert(!strcmp(out, "100%") && gui_settings_changed(&game) == 1 && !gui_settings_save(&game, picks));
        assert((text = (char*)snx_read_file(picks, &size)) && strstr(text, "[Audio]\nvolume=100\n") &&
               !strstr(text, "fullscreen") && !strstr(text, "resolution") &&
               !strncmp(text, SNX_GAME_PICKS_HEAD, strlen(SNX_GAME_PICKS_HEAD))); /* as the runtime writes it */
        free(text);
        assert(gui_settings_game_own(ini, out, sizeof out) == 1 && !strcmp(out, "volume=100"));
        assert(!snx_ini_record(ini, "gpu_driver", "Turnip_v32") &&
               !snx_ini_record(ini, "freedreno_env", "TU_DEBUG=noubwc"));
        assert(gui_settings_game_own(ini, out, sizeof out) == 3 &&
               !strcmp(out, "volume=100, gpu_driver=Turnip_v32, freedreno_env"));
        assert(gui_settings_game_own(ini, out, 12) == 3 && !strcmp(out, "volume=100,")); /* cut, every one counted */
        gui_settings_free(&game);
        assert(!gui_settings_read_game(&game, list));
        gui_settings_mark(&game);
        assert(!gui_settings_load(&game, picks, NULL, 0) && !strcmp(gui_settings_value(&game, "volume"), "100"));
        gui_settings_reset(&game, v);
        assert(!gui_settings_value(&game, "volume") && !gui_settings_changed(&game));
        assert(gui_settings_game_own(NULL, out, sizeof out) == 0 && !out[0]);
        gui_settings_free(&game);
        gui_settings_free(&global);
        assert(!snx_remove_tree(dir));
        free(picks);
        free(ini);
        free(dir);
    }
    /* a Switch game's own rows are every one of the runtime's, as they were; a 3DS game's its Console alone, in its
       group, its words Global and the two consoles (Auto is what Global gives, unless the launcher picks one), kept in
       its picks as the driver reads it and back to Global by a reset. A picks file without it loads all Global, and
       one that holds what its rows do not take says so */
    {
        static const char list[] = "Display\tfullscreen\t0\tint\t0\t1\tWhole screen\tFullscreen\t0=Off|1=On\n"
                                   "3DS\tctr_top_resolution\t1\tint\t1\t16\tScale\tTop screen resolution\t\n"
                                   "3DS\tctr_model\tauto\ttext\t\t\tThe console\tConsole\t"
                                   "auto=Auto|old=Old 3DS|new=New 3DS\t\n"
                                   "3DS/Debug\tctr_hooks\t1\tint\t0\t1\tStubs\tIPC hooks\t0=Off|1=On\n"
                                   "Vita/Debug\tvita_hle_trace\t0\tint\t0\t2\tImports\tImport trace\t0=Off|1=On\n"
                                   "Linux\tlinux_isolated\t0\tint\t0\t1\tNo host\tIsolated\t0=Off|1=On\n"
                                   "Android\tandroid_jit_opt\t0\tint\t0\t1\tLevel\tJIT level\t0=Off|1=On\n"
                                   "DS\tnds_aot\t0\tint\t0\t1\tAhead\tAOT\t0=Off|1=On\n"
                                   "Audio\tvolume\t100\tint\t0\t100\tLoud\tVolume\t0=Mute|50=50%|100=100%";
        GuiSettings game = {0}, global = {0};
        char *dir = snx_temp_dir(NULL), *ini = snx_path_join(dir, "game.ini"), *picks = snx_game_picks(ini), *text,
             out[256];
        static const char* const console_words[] = {"Global", "Old 3DS", "New 3DS"};
        static const char* const every[] = {"fullscreen",     "ctr_model",       "ctr_hooks", "vita_hle_trace",
                                            "linux_isolated", "android_jit_opt", "nds_aot",   "volume"};
        size_t size;
        int v, w;
        /* a Switch game's picks with another console's key are read, kept and said as they were */
        assert(!gui_settings_read_game(&game, list) && game.game == GUI_GAME_SWITCH);
        for (w = 0; w < (int)(sizeof every / sizeof *every); ++w) assert(at(&game, every[w]) >= 0);
        gui_settings_mark(&game);
        text = snx_strdup("[3DS]\nctr_model=old\n[DS]\nnds_aot=1\n");
        assert(!snx_write_file(picks, text, strlen(text)) && !gui_settings_load(&game, picks, NULL, 0) &&
               !gui_settings_unread(&game, picks, out, sizeof out) && gui_settings_changed(&game) == 2);
        free(text);
        assert(gui_settings_set(&game, "fullscreen", "1") && !gui_settings_save(&game, picks));
        assert((text = (char*)snx_read_file(picks, &size)) && strstr(text, "ctr_model=old\n") &&
               strstr(text, "nds_aot=1\n") && strstr(text, "fullscreen=1\n"));
        free(text);
        assert(!remove(picks));
        gui_settings_free(&game);
        assert(!gui_settings_read_console(&game, list, GUI_GAME_SWITCH) && at(&game, "ctr_model") >= 0);
        gui_settings_free(&game);
        assert(!gui_settings_read(&global, list) && at(&global, "ctr_hooks") >= 0 && at(&global, "nds_aot") >= 0);
        assert(!gui_settings_read_console(&game, list, GUI_GAME_3DS) && game.game == GUI_GAME_3DS && game.count == 1 &&
               !game.own);
        v = at(&game, "ctr_model");
        assert(v == 0 && !strcmp(key(&game, "ctr_model")->key, "Console") &&
               !strcmp(key(&game, "ctr_model")->section, "3DS") && !game.values[v].word && !game.values[v].plain);
        for (w = 0; key(&game, "ctr_model")->words[w]; ++w)
            assert(w < 3 && !strcmp(key(&game, "ctr_model")->words[w], console_words[w]));
        assert(w == 3);
        gui_settings_mark(&game);
        /* nothing of its own: Global, with what the launcher's is, and no picks to keep; an old game's, with none */
        assert(!gui_settings_value(&game, "ctr_model") && !gui_settings_changed(&game) &&
               !gui_settings_load(&game, picks, NULL, 0) && !gui_settings_unread(&game, picks, out, sizeof out));
        gui_settings_shown_game(&game, &global, v, out, sizeof out);
        assert(!strcmp(out, "Global (Auto [default])"));
        assert(gui_settings_set(&global, "ctr_model", "new"));
        gui_settings_shown_game(&game, &global, v, out, sizeof out);
        assert(!strcmp(out, "Global (New 3DS)"));
        /* its own, over it */
        assert(gui_settings_set(&game, "ctr_model", "old") && !strcmp(gui_settings_value(&game, "ctr_model"), "old"));
        gui_settings_shown_game(&game, &global, v, out, sizeof out);
        assert(!strcmp(out, "Old 3DS") && gui_settings_changed(&game) == 1 && !gui_settings_save(&game, picks));
        assert((text = (char*)snx_read_file(picks, &size)) && strstr(text, "[3DS]\nctr_model=old\n"));
        free(text);
        assert(gui_settings_game_own(ini, out, sizeof out) == 1 && !strcmp(out, "ctr_model=old"));
        gui_settings_free(&game);
        assert(!gui_settings_read_console(&game, list, GUI_GAME_3DS));
        gui_settings_mark(&game);
        assert(!gui_settings_load(&game, picks, NULL, 0) && !strcmp(gui_settings_value(&game, "ctr_model"), "old") &&
               !gui_settings_unread(&game, picks, out, sizeof out));
        gui_settings_reset(&game, v);
        assert(!gui_settings_value(&game, "ctr_model") && !gui_settings_changed(&game));
        /* an old game's, with keys of its own and no ctr_model: Global; and one that is none of its rows' is named, never
           by its value, and stays for Clean Old Settings too, as no older version's key */
        text = snx_strdup("[3DS]\nother=1\n");
        snx_config_forget();
        assert(!snx_write_file(picks, text, strlen(text)) && gui_settings_load(&game, picks, out, sizeof out) == 1 &&
               !strcmp(out, "this version does not read other, which stays as it is"));
        assert(!gui_settings_value(&game, "ctr_model") && !gui_settings_unread(&game, picks, out, sizeof out));
        free(text);
        gui_settings_free(&game);
        gui_settings_free(&global);
        assert(!snx_remove_tree(dir));
        free(picks);
        free(ini);
        free(dir);
    }
    /* a 3DS game's run gets its own table's picks and the Switch's settings the 3DS takes too (vk_memory,
       async_pipelines, pipeline_threads, gamepad_swap_ab, gamepad_deadzone, gamepad_map, touch, vsync, fullscreen,
       anisotropy, fps_limit, volume) only: another Display, Graphics, Debug, Controllers or On-screen controls setting
       picked for a Switch game is not one */
    {
        static const char ctr[] = "Display\tfullscreen\t0\tint\t0\t1\tWhole screen\tFullscreen\t0=Off|1=On\n"
                                  "Display\tdocked\t1\tint\t0\t1\tTV\tDocked\t0=Off|1=On\n"
                                  "3DS\tctr_separate_windows\t0\tint\t0\t1\tTwo windows\tSeparate screens\t0=Off|1=On\n"
                                  "3DS\tctr_top_resolution\t1\tint\t1\t16\tScale\tTop screen resolution\t\n"
                                  "3DS/Debug\tctr_hooks\t1\tint\t0\t1\tStubs\tIPC hooks\t0=Off|1=On\n"
                                  "Graphics\tvsync\t1\tint\t0\t1\tNo tearing\tVSync\t0=Off|1=On\n"
                                  "Graphics\tshadows\t1\tint\t0\t1\tShadow maps\tShadows\t0=Off|1=On\n"
                                  "Graphics\tanisotropy\t0\tint\t0\t16\tSharper\tAnisotropic filtering\t\n"
                                  "Performance\tasync_pipelines\t0\tint\t0\t1\tNo pause\tAsync pipelines\t0=Off|1=On\n"
                                  "Performance\tpipeline_threads\t0\tint\t0\t32\tThreads\tPipeline threads\t\n"
                                  "Performance\tfps_limit\tgame\ttext\t0\t0\tThe pace\tFPS limit\t\n"
                                  "Controllers\tgamepad_swap_ab\t0\tint\t0\t1\tA and B\tSwap A/B\t0=Off|1=On\n"
                                  "Controllers\tgamepad_deadzone\t20\tint\t0\t90\tDead zone\tStick dead zone\t\n"
                                  "Controllers\tgamepad_rumble\t100\tint\t0\t100\tHow strongly\tVibration\t\n"
                                  "Controllers\tgamepad_map\t\ttext\t0\t0\tThe buttons sent\tButton map\t\n"
                                  "On-screen controls\ttouch\t2\tint\t0\t2\tTouch\tTouch screen\t\n"
                                  "On-screen controls\tcontrols_haptics\t1\tint\t0\t1\tBuzz\tHaptics\t\n"
                                  "Debug\tvk_memory\t0\tint\t0\t2\tGuest buffers\tGuest memory\t0=Auto|1=Copy|2=UMA\n"
                                  "Debug\tvk_budget_mb\t0\tint\t0\t65536\tBudget\tVulkan budget\t\n"
                                  "Audio\tvolume\t100\tint\t0\t100\tLoud\tVolume\t0=Mute|50=50%|100=100%\t\t\tslider";
        assert(!gui_settings_read(&s, ctr));
        assert(gui_settings_set(&s, "fullscreen", "1") && gui_settings_set(&s, "ctr_separate_windows", "1") &&
               gui_settings_set(&s, "ctr_top_resolution", "2") && gui_settings_set(&s, "async_pipelines", "1") &&
               gui_settings_set(&s, "pipeline_threads", "2") && gui_settings_set(&s, "gamepad_swap_ab", "1") &&
               gui_settings_set(&s, "gamepad_deadzone", "35") && gui_settings_set(&s, "gamepad_rumble", "50") &&
               gui_settings_set(&s, "gamepad_map", "south:A east:B") && gui_settings_set(&s, "touch", "1") &&
               gui_settings_set(&s, "controls_haptics", "0") && gui_settings_set(&s, "vk_memory", "1") &&
               gui_settings_set(&s, "vk_budget_mb", "512") && gui_settings_set(&s, "vsync", "0") &&
               gui_settings_set(&s, "shadows", "0") && gui_settings_set(&s, "docked", "0") &&
               gui_settings_set(&s, "anisotropy", "8") && gui_settings_set(&s, "fps_limit", "120"));
        assert(gui_settings_set(&s, "volume", "50"));
        /* a row of the 3DS group's Debug page, passed as its screens' are */
        assert(gui_settings_ctr_options(&s, options) == 14 && gui_settings_set(&s, "ctr_hooks", "0") &&
               gui_settings_ctr_options(&s, options) == 15 && !strcmp(options[3], "--ctr-hooks=0") &&
               gui_settings_set(&s, "ctr_hooks", "1"));
        assert(gui_settings_ctr_options(&s, options) == 14 && !strcmp(options[0], "--fullscreen=1") &&
               !strcmp(options[1], "--ctr-separate-windows=1") && !strcmp(options[2], "--ctr-top-resolution=2") &&
               !strcmp(options[3], "--vsync=0") && !strcmp(options[4], "--anisotropy=8") &&
               !strcmp(options[5], "--async-pipelines=1") && !strcmp(options[6], "--pipeline-threads=2") &&
               !strcmp(options[7], "--fps-limit=120") && !strcmp(options[8], "--gamepad-swap-ab=1") &&
               !strcmp(options[9], "--gamepad-deadzone=35") && !strcmp(options[10], "--gamepad-map=south:A east:B") &&
               !strcmp(options[11], "--touch=1") && !strcmp(options[12], "--vk-memory=1"));
        assert(!strcmp(options[13], "--volume=50"));
        gui_settings_free(&s);
    }
    /* a Vita game's run gets its Debug page's picks alone (keys that start with vita_): none of the Switch's or the
       3DS's, which its driver refuses */
    {
        static const char vita[] = "Display\tfullscreen\t0\tint\t0\t1\tWhole screen\tFullscreen\t0=Off|1=On\n"
                                   "3DS/Debug\tctr_hooks\t1\tint\t0\t1\tStubs\tIPC hooks\t0=Off|1=On\n"
                                   "Vita/Debug\tvita_hle_trace\t0\tint\t0\t2\tImports\tImport trace\t"
                                   "0=Off|1=Missing|2=All\n"
                                   "Vita/Debug\tvita_gxm_trace\t0\tint\t0\t1\tScenes\tGXM trace\t0=Off|1=On\n";
        assert(!gui_settings_read(&s, vita));
        assert(!gui_settings_vita_options(&s, options) && gui_settings_set(&s, "fullscreen", "1") &&
               gui_settings_set(&s, "ctr_hooks", "0") && !gui_settings_vita_options(&s, options));
        assert(gui_settings_set(&s, "vita_hle_trace", "2") && gui_settings_vita_options(&s, options) == 1 &&
               !strcmp(options[0], "--vita-hle-trace=2"));
        assert(gui_settings_ctr_options(&s, options) == 2 && !strcmp(options[1], "--ctr-hooks=0"));
        gui_settings_free(&s);
    }
    /* a GBA, a DS or a Vita game's own: the on-screen controls it has and the touch screen, each control by the
       console's name for it, and nothing else of the Switch's; and the launcher's for its run: the picks of those
       settings, every one (the driver says what its console has nothing for), --launcher-picks=1 after them, none with
       nothing picked */
    {
        static const char list[] =
            "Display\tfullscreen\t0\tint\t0\t1\tWhole screen\tFullscreen\t0=Off|1=On\n"
            "On-screen controls\tcontrols\t2\tint\t0\t2\tShown\tShow controls\t0=Off|1=On|2=Auto\n"
            "On-screen controls\tcontrols_rel_center\t1\tint\t0\t1\tMiddle\tRelative stick center\t0=Off|1=On\n"
            "On-screen controls\ttouch\t2\tint\t0\t2\tTouch\tTouch screen\t0=Off|1=Fingers|2=Fingers and mouse\n"
            "On-screen controls/Buttons\tcontrols_zl_shown\t1\tint\t0\t1\tDrawn\tZL\t0=Hidden|1=Shown\n"
            "On-screen controls/Buttons\tcontrols_plus_shown\t1\tint\t0\t1\tDrawn\t+\t0=Hidden|1=Shown\n"
            "On-screen controls/Buttons\tcontrols_stick_l_shown\t1\tint\t0\t1\tDrawn\tLeft stick\t0=Hidden|1=Shown\n"
            "On-screen controls/Buttons\tcontrols_a_shown\t1\tint\t0\t1\tDrawn\tA\t0=Hidden|1=Shown\n"
            "Controllers\tcontroller_applet\t0\tint\t0\t1\tAsked\tController applet\t0=Dialog|1=Quick\n";
        static const struct {
            int game, touch, sticks;
            const char* a; /* its A's name */
        } consoles[] = {{GUI_GAME_GBA, 0, 0, "A"}, {GUI_GAME_NDS, 1, 0, "A"}, {GUI_GAME_VITA, 1, 1, "Circle"}};
        for (size_t c = 0; c < sizeof consoles / sizeof *consoles; ++c) {
            GuiSettings game = {0};
            assert(!gui_settings_read_console(&game, list, consoles[c].game) && game.game == consoles[c].game);
            assert(at(&game, "controls") == 0 && !strcmp(key(&game, "controls")->words[0], "Global"));
            assert(at(&game, "fullscreen") < 0 && at(&game, "controller_applet") < 0);
            assert(at(&game, "controls_zl_shown") < 0);
            assert((at(&game, "touch") >= 0) == consoles[c].touch);
            assert((at(&game, "controls_rel_center") >= 0) == consoles[c].sticks);
            assert((at(&game, "controls_stick_l_shown") >= 0) == consoles[c].sticks);
            assert(!strcmp(key(&game, "controls_plus_shown")->key, "Start"));
            assert(!strcmp(key(&game, "controls_a_shown")->key, consoles[c].a));
            gui_settings_free(&game);
        }
        assert(!gui_settings_read(&s, list) && !gui_settings_controls_options(&s, options));
        assert(gui_settings_set(&s, "fullscreen", "1") && gui_settings_set(&s, "controller_applet", "1") &&
               !gui_settings_controls_options(&s, options));
        assert(gui_settings_set(&s, "controls", "1") && gui_settings_set(&s, "touch", "0") &&
               gui_settings_set(&s, "controls_zl_shown", "0"));
        assert(gui_settings_controls_options(&s, options) == 4 && !strcmp(options[0], "--controls=1") &&
               !strcmp(options[1], "--touch=0") && !strcmp(options[2], "--controls-zl-shown=0") &&
               !strcmp(options[3], "--" SNX_LAUNCHER_PICKS "=1"));
        gui_settings_free(&s);
    }
    /* the launcher's Console for a 3DS run: --launcher-picks=1 after the rest, which puts the game's own over it; with
       no Console picked, or Auto, no mark, the rest as it was */
    {
        static const char ctr[] = "Display\tfullscreen\t0\tint\t0\t1\tWhole screen\tFullscreen\t0=Off|1=On\n"
                                  "3DS\tctr_model\tauto\ttext\t\t\tThe console\tConsole\t"
                                  "auto=Auto|old=Old 3DS|new=New 3DS\t\n"
                                  "Audio\tvolume\t100\tint\t0\t100\tLoud\tVolume\t0=Mute|50=50%|100=100%";
        assert(!gui_settings_read(&s, ctr));
        assert(gui_settings_set(&s, "fullscreen", "1") && gui_settings_set(&s, "volume", "50"));
        assert(gui_settings_ctr_options(&s, options) == 2 && !strcmp(options[0], "--fullscreen=1") &&
               !strcmp(options[1], "--volume=50"));
        assert(gui_settings_set(&s, "ctr_model", "auto"));
        assert(gui_settings_ctr_options(&s, options) == 2 && !strcmp(options[1], "--volume=50"));
        assert(gui_settings_set(&s, "ctr_model", "new"));
        assert(gui_settings_ctr_options(&s, options) == 4 && !strcmp(options[0], "--fullscreen=1") &&
               !strcmp(options[1], "--ctr-model=new") && !strcmp(options[2], "--volume=50") &&
               !strcmp(options[3], "--" SNX_LAUNCHER_PICKS "=1"));
        gui_settings_free(&s);
    }
    /* the Linux, Android and DS drivers': a run's options are its own group's, each only to its own driver; the DS's
       WFC service, which that driver reads from settings.ini, never */
    {
        static const char drivers[] = "Linux\tlinux_isolated\t0\tint\t0\t1\tNo host\tIsolated\t0=Off|1=On\n"
                                      "Linux\tlinux_root\t\ttext\t0\t0\tRoot\tRoot folder\t\t\t\tfolder\n"
                                      "Linux/Debug\tlinux_strace\t0\tint\t0\t1\tCalls\tSystem call trace\t0=Off|1=On\n"
                                      "Android\tandroid_jit_opt\t2\ttext\t0\t0\tLevel\tJIT optimization\t0=O0|2=O2\n"
                                      "Android/Debug\tandroid_sample\t0\tint\t0\t1\tSamples\tThread sample\t"
                                      "0=Off|1=On\n"
                                      "DS\tnds_aot\t1\tint\t0\t1\tFirst\tCompile first\t0=Off|1=On\n"
                                      "DS/Debug\tnds_trace\t0\tint\t0\t2\tTrace\tTrace\t0=Off|1=One|2=Two\n"
                                      "Online\tnds_wfc\toff\ttext\t0\t0\tService\tDS WFC service\t"
                                      "off=Off|kaeru=Kaeru\n";
        assert(!gui_settings_read(&s, drivers));
        assert(!gui_settings_linux_options(&s, options) && !gui_settings_android_options(&s, options) &&
               !gui_settings_nds_options(&s, options));
        assert(gui_settings_set(&s, "linux_isolated", "1") && gui_settings_set(&s, "linux_strace", "1") &&
               gui_settings_set(&s, "linux_root", "/a root") && gui_settings_set(&s, "android_jit_opt", "0") &&
               gui_settings_set(&s, "android_sample", "1") && gui_settings_set(&s, "nds_aot", "0") &&
               gui_settings_set(&s, "nds_trace", "2") && gui_settings_set(&s, "nds_wfc", "kaeru"));
        assert(gui_settings_linux_options(&s, options) == 3 && !strcmp(options[0], "--linux-isolated=1") &&
               !strcmp(options[1], "--linux-root=/a root") && !strcmp(options[2], "--linux-strace=1"));
        assert(gui_settings_android_options(&s, options) == 2 && !strcmp(options[0], "--android-jit-opt=0") &&
               !strcmp(options[1], "--android-sample=1"));
        assert(gui_settings_nds_options(&s, options) == 2 && !strcmp(options[0], "--nds-aot=0") &&
               !strcmp(options[1], "--nds-trace=2"));
        assert(gui_settings_vita_options(&s, options) == 0);
        gui_settings_free(&s);
    }
    /* a slider's row (the runtime's twelfth column, settings.txt's slider): a plain word row whose steps stop at its
       ends, set to the step nearest a place along it; the same words without the mark are no slider, nor are they
       where the default is no step (its row's first word the .ini's) */
    {
        static const char sliders[] =
            "Audio\tvolume\t100\tint\t0\t100\tHow loud\tVolume\t0=Mute|25=25%|50=50%|75=75%|100=100%\t\t\tslider\n"
            "Audio\tplain\t100\tint\t0\t100\tHow loud\tPlain\t0=Mute|50=50%|100=100%\n"
            "Audio\toff_step\t60\tint\t0\t100\tHow loud\tOff step\t0=Mute|50=50%|100=100%\t\t\tslider";
        int v, p, o;
        assert(!gui_settings_read(&s, sliders));
        v = at(&s, "volume");
        p = at(&s, "plain");
        o = at(&s, "off_step");
        assert(v >= 0 && p >= 0 && o >= 0 && s.values[v].slider && !s.values[p].slider && !s.values[o].slider);
        assert(!strcmp(on_screen(&s, "volume"), "100% [default]") && gui_settings_knob(&s, v) == 1000);
        assert(!gui_settings_slide(&s, v, 1) && gui_settings_knob(&s, v) == 1000); /* its end: never round to Mute */
        assert(gui_settings_slide(&s, v, -1) && !strcmp(gui_settings_value(&s, "volume"), "75") &&
               gui_settings_knob(&s, v) == 750);
        assert(gui_settings_options(&s, 0, NULL, options) == 2 && !strcmp(options[0], "--volume=75"));
        assert(gui_settings_slide_to(&s, v, 0) && !strcmp(gui_settings_value(&s, "volume"), "0"));
        assert(!gui_settings_slide(&s, v, -1) && !strcmp(on_screen(&s, "volume"), "Mute")); /* never round to 100% */
        assert(gui_settings_slide_to(&s, v, 374) && !strcmp(gui_settings_value(&s, "volume"), "25")); /* nearer 250 */
        assert(gui_settings_slide_to(&s, v, 376) && !strcmp(gui_settings_value(&s, "volume"), "50")); /* nearer 500 */
        assert(!gui_settings_slide_to(&s, v, 500) && gui_settings_knob(&s, v) == 500);    /* there already */
        assert(gui_settings_slide_to(&s, v, 5000) && !gui_settings_value(&s, "volume") && /* past its end */
               !strcmp(on_screen(&s, "volume"), "100% [default]"));
        assert(!gui_settings_slide(&s, p, -1) && !gui_settings_slide_to(&s, p, 0) && gui_settings_knob(&s, p) == 0);
        assert(!gui_settings_slide(&s, o, -1) && !strcmp(on_screen(&s, "off_step"), "60 [default]"));
        gui_settings_free(&s);
    }
    /* a value none of a row's words is, from a file of picks or a game's .ini, on a slider's row or an open one (the
       twelfth column's open): a number in its range kept as a word of its own after them, shown as itself and passed
       to a run; a slider's knob placed by it and its next step taken from it; a number outside the range, a fraction
       of a whole number's row, a text of a closed row and any value of a row neither a slider nor open refused */
    {
        static const char custom[] =
            "Audio\tvolume\t100\tint\t0\t100\tHow loud\tVolume\t0=Mute|25=25%|50=50%|75=75%|100=100%\t\t\tslider\n"
            "Graphics\tbc7_queue\t0\tint\t0\t16384\tQueued\tBC7 queue\t0=Auto|256=256|512=512\t\t\topen\n"
            "Graphics\tupscaler\toff\ttext\t0\t0\tFilter\tUpscaler\toff=Off|fsr1=AMD FSR 1\n"
            "Graphics\tmode\t0\tint\t0\t9\tClosed\tMode\t0=Off|1=On\n";
        char *dir = snx_temp_dir(NULL), *path = snx_path_join(dir, "settings.ini"), unread[128];
        int v, q, n;
        assert(!gui_settings_read(&s, custom));
        gui_settings_mark(&s);
        v = at(&s, "volume");
        q = at(&s, "bc7_queue");
        n = words(&s.keys[q]);
        assert(gui_settings_set(&s, "volume", "37") && !strcmp(gui_settings_value(&s, "volume"), "37") &&
               !strcmp(on_screen(&s, "volume"), "37") && gui_settings_knob(&s, v) == 370);
        assert(gui_settings_options(&s, 0, NULL, options) == 2 && !strcmp(options[0], "--volume=37"));
        assert(gui_settings_slide(&s, v, 1) && !strcmp(gui_settings_value(&s, "volume"), "50"));
        assert(gui_settings_set(&s, "volume", "37") && gui_settings_slide(&s, v, -1) &&
               !strcmp(gui_settings_value(&s, "volume"), "25"));
        assert(!gui_settings_set(&s, "volume", "101") && !gui_settings_set(&s, "volume", "37.5") &&
               !gui_settings_set(&s, "upscaler", "nis2") && !gui_settings_set(&s, "bc7_queue", "-1") &&
               !gui_settings_set(&s, "mode", "5")); /* a row neither a slider nor open: its words alone */
        assert(gui_settings_set(&s, "bc7_queue", "3000") && words(&s.keys[q]) == n + 1 &&
               !strcmp(on_screen(&s, "bc7_queue"), "3000") && gui_settings_set(&s, "bc7_queue", "512") &&
               gui_settings_set(&s, "bc7_queue", "1024") && words(&s.keys[q]) == n + 1); /* one slot, reused */
        assert(!gui_settings_save(&s, path));
        gui_settings_free(&s);
        assert(!gui_settings_read(&s, custom));
        gui_settings_mark(&s);
        assert(!gui_settings_load(&s, path, NULL, 0) && !strcmp(gui_settings_value(&s, "bc7_queue"), "1024") &&
               !strcmp(gui_settings_value(&s, "volume"), "25"));
        gui_settings_free(&s);
        /* a try of the file's values leaves no word behind on a row that kept none */
        assert(!gui_settings_read(&s, custom) && !gui_settings_unread(&s, path, unread, sizeof unread) &&
               words(&s.keys[at(&s, "bc7_queue")]) == n && !gui_settings_value(&s, "bc7_queue"));
        gui_settings_free(&s);
        assert(!snx_remove_tree(dir));
        free(path);
        free(dir);
    }
    /* a number's typed row stepped as the game's menu steps it: a float by a quarter, or a twentieth within a range of
       2, onto that grid, a whole number by 1, from the default where nothing is typed, stopping at the ends */
    {
        static const char numbers[] = "Graphics\tresolution\t1\tfloat\t0.25\t16\tScale\tResolution scale\t\n"
                                      "Graphics\tsharp\t0\tfloat\t0\t1\tSharpening\tFSR sharpness\t\n"
                                      "Debug\tcount\t5\tint\t0\t99999\tCount\tA count\t\n"
                                      "Debug\tname\t\ttext\t0\t0\tA name\tName\t\n";
        int r, f, c;
        assert(!gui_settings_read(&s, numbers));
        r = at(&s, "resolution");
        f = at(&s, "sharp");
        c = at(&s, "count");
        assert(gui_settings_step_number(&s, r, 1) && !strcmp(gui_settings_value(&s, "resolution"), "1.25"));
        assert(gui_settings_set(&s, "resolution", "1.3") && gui_settings_step_number(&s, r, -1) &&
               !strcmp(gui_settings_value(&s, "resolution"), "1.25")); /* onto the quarters' grid */
        assert(gui_settings_set(&s, "resolution", "0.25") && !gui_settings_step_number(&s, r, -1));
        assert(gui_settings_set(&s, "resolution", "16") && !gui_settings_step_number(&s, r, 1));
        assert(!gui_settings_step_number(&s, f, -1) && gui_settings_step_number(&s, f, 1) &&
               !strcmp(gui_settings_value(&s, "sharp"), "0.05"));
        assert(gui_settings_step_number(&s, c, -1) && !strcmp(gui_settings_value(&s, "count"), "4"));
        assert(gui_settings_options(&s, 0, NULL, options) == 4 && !strcmp(options[0], "--resolution=16"));
        assert(!gui_settings_step_number(&s, at(&s, "name"), 1)); /* a text is no number */
        gui_settings_free(&s);
    }
    /* an open row's words with more the launcher found itself (the recording devices): after its own, none twice, in
       the place of those a call before added; the value a word of them now, or itself still; System default an empty
       value, which is a pick, passed and kept */
    {
        static const char mic[] =
            "Game\tmicrophone\toff\ttext\t0\t0\tHears\tMicrophone\t=System default|off=Off\t\t\topen\n"
            "Game\tclosed\toff\ttext\t0\t0\tClosed\tClosed\toff=Off|on=On\n";
        static const char* const two[] = {"USB Mic", "Headset", "USB Mic", "off"};
        static const char* const one[] = {"Headset"};
        char *dir = snx_temp_dir(NULL), *path = snx_path_join(dir, "settings.ini"), *text;
        size_t size;
        int m;
        assert(!gui_settings_read(&s, mic));
        gui_settings_mark(&s);
        m = at(&s, "microphone");
        assert(words(&s.keys[m]) == 2 && !strcmp(on_screen(&s, "microphone"), "Off [default]"));
        assert(gui_settings_set(&s, "microphone", "usb")); /* a part of a name, as the .ini takes it */
        assert(gui_settings_add_words(&s, m, two, 4) && words(&s.keys[m]) == 5 &&
               !strcmp(s.keys[m].words[2], "USB Mic") && !strcmp(s.keys[m].words[3], "Headset") &&
               !strcmp(on_screen(&s, "microphone"), "usb") && s.values[m].custom == 4);
        assert(gui_settings_set(&s, "microphone", "Headset") && gui_settings_options(&s, 0, NULL, options) == 2 &&
               !strcmp(options[0], "--microphone=Headset"));
        assert(gui_settings_add_words(&s, m, one, 1) && words(&s.keys[m]) == 3 && !s.values[m].custom &&
               !strcmp(gui_settings_value(&s, "microphone"), "Headset") && s.values[m].word == 2);
        assert(gui_settings_add_words(&s, m, NULL, 0) && words(&s.keys[m]) == 3 && s.values[m].custom == 2 &&
               !strcmp(on_screen(&s, "microphone"), "Headset")); /* unplugged: its value stays */
        assert(gui_settings_set(&s, "microphone", "") && !strcmp(gui_settings_value(&s, "microphone"), "") &&
               !strcmp(on_screen(&s, "microphone"), "System default") &&
               gui_settings_options(&s, 0, NULL, options) == 2 && !strcmp(options[0], "--microphone="));
        assert(!gui_settings_save(&s, path));
        text = (char*)snx_read_file(path, &size);
        assert(text && strstr(text, "microphone=\n"));
        free(text);
        assert(!gui_settings_add_words(&s, at(&s, "closed"), one, 1) && !gui_settings_add_words(&s, -1, one, 1));
        gui_settings_free(&s);
        assert(!gui_settings_read(&s, mic));
        gui_settings_mark(&s);
        assert(!gui_settings_load(&s, path, NULL, 0) && !strcmp(on_screen(&s, "microphone"), "System default"));
        gui_settings_reset(&s, -1);
        assert(!gui_settings_value(&s, "microphone"));
        gui_settings_free(&s);
        /* a game's own: Global first, which stays the one it starts with */
        assert(!gui_settings_read_game(&s, mic));
        gui_settings_mark(&s);
        m = at(&s, "microphone");
        assert(gui_settings_add_words(&s, m, one, 1) && words(&s.keys[m]) == 4 &&
               !strcmp(s.keys[m].words[0], "Global") && !gui_settings_value(&s, "microphone") &&
               !gui_settings_changed(&s));
        assert(gui_settings_set(&s, "microphone", "Headset") &&
               !strcmp(gui_settings_value(&s, "microphone"), "Headset") && gui_settings_changed(&s) == 1);
        gui_settings_free(&s);
        assert(!snx_remove_tree(dir));
        free(path);
        free(dir);
    }
    /* a row of boxes (the twelfth column's boxes): its items' labels and abouts, on or off in the row's value, a text's
       separated by commas or spaces, an int's a sum of bits; a name the row has not kept where it is, said; an item
       turned off leaves the others, and nothing on is no value, the default's */
    {
        static const char boxes[] =
            "Debug\thooks_off\t\ttext\t0\t0\tFamilies\tHook families off\t"
            "libc=libc~the C library|sync=sync~clocks, and sleep|cond_var=cond_var~\t\t\tboxes\n"
            "Debug\tfsr\t0\tint\t0\t255\tBits\tFSR inputs\t1=Jitter x|2=Jitter y|64=HDR input\t\t\tboxes\n"
            "Debug\ttyped\t\ttext\t0\t0\tNo items\tTyped\t\n";
        int h, f;
        char said[64];
        assert(!gui_settings_read(&s, boxes));
        h = at(&s, "hooks_off");
        f = at(&s, "fsr");
        assert(s.values[h].items == 3 && !strcmp(s.values[h].item_value[1], "sync") &&
               !strcmp(s.values[h].item_label[1], "sync") && !strcmp(s.values[h].item_about[1], "clocks, and sleep") &&
               !strcmp(s.values[h].item_about[2], "") && s.values[f].items == 3 &&
               !strcmp(s.values[f].item_label[2], "HDR input") && !strcmp(s.values[f].item_about[2], ""));
        assert(s.values[at(&s, "typed")].items == 0 && s.keys[h].type == GUI_KEY_TEXT && !s.keys[f].numeric);
        /* none on: the default, a box turned on is the row's own value, in the order turned on */
        assert(!strcmp(on_screen(&s, "hooks_off"), "None [default]") && !gui_settings_item_on(&s, h, 0, NULL));
        assert(gui_settings_item_toggle(&s, h, 2, NULL) && gui_settings_item_toggle(&s, h, 0, NULL) &&
               !strcmp(gui_settings_value(&s, "hooks_off"), "cond_var,libc") && gui_settings_item_on(&s, h, 0, NULL) &&
               !gui_settings_item_on(&s, h, 1, NULL));
        assert(!strcmp(on_screen(&s, "hooks_off"), "2 on") && gui_settings_options(&s, 0, NULL, options) == 2 &&
               !strcmp(options[0], "--hooks-off=cond_var,libc"));
        /* spaces or commas between, a name the row has not kept and counted as another, a name's box turned off */
        assert(gui_settings_set(&s, "hooks_off", "libc mine, sync"));
        assert(gui_settings_item_on(&s, h, 0, NULL) && gui_settings_item_on(&s, h, 1, NULL));
        assert(!strcmp(on_screen(&s, "hooks_off"), "2 on, 1 other"));
        assert(gui_settings_item_toggle(&s, h, 0, NULL) && !strcmp(gui_settings_value(&s, "hooks_off"), "mine,sync") &&
               gui_settings_item_toggle(&s, h, 1, NULL) && !strcmp(gui_settings_value(&s, "hooks_off"), "mine"));
        assert(gui_settings_item_toggle(&s, h, 1, NULL) && gui_settings_item_toggle(&s, h, 1, NULL) &&
               !strcmp(on_screen(&s, "hooks_off"), "0 on, 1 other"));
        assert(gui_settings_set(&s, "hooks_off", "libc") && gui_settings_item_toggle(&s, h, 0, NULL) &&
               !gui_settings_value(&s, "hooks_off") && !strcmp(on_screen(&s, "hooks_off"), "None [default]"));
        /* an int's: bits, a bit past its items' kept */
        assert(gui_settings_item_toggle(&s, f, 2, NULL) && !strcmp(gui_settings_value(&s, "fsr"), "64"));
        assert(gui_settings_item_toggle(&s, f, 0, NULL) && !strcmp(gui_settings_value(&s, "fsr"), "65") &&
               !strcmp(on_screen(&s, "fsr"), "2 on"));
        assert(gui_settings_set(&s, "fsr", "129") && gui_settings_item_toggle(&s, f, 0, NULL) &&
               !strcmp(gui_settings_value(&s, "fsr"), "128") && !strcmp(on_screen(&s, "fsr"), "0 on, 1 other"));
        assert(gui_settings_set(&s, "fsr", "1") && gui_settings_item_toggle(&s, f, 0, NULL) &&
               !gui_settings_value(&s, "fsr") && !gui_settings_item_toggle(&s, f, 3, NULL)); /* no such item */
        /* a game's own: what it has of the launcher's, until a box of its own */
        assert(gui_settings_item_on(&s, h, 1, "sync") && !gui_settings_item_on(&s, h, 1, NULL));
        assert(gui_settings_item_toggle(&s, h, 0, "sync") && !strcmp(gui_settings_value(&s, "hooks_off"), "sync,libc"));
        gui_settings_items_said(&s, h, "sync,libc", said, sizeof said);
        assert(!strcmp(said, "2 on"));
        gui_settings_free(&s);
    }
    puts("GUI settings: the runtime's list, picks and a run's options passed");
    return 0;
}
