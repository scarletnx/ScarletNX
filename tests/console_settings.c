/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Every row of the Switch's settings (settings.txt's g_console_ini) has its line in the consoles' table
   (common/utilities/console_settings.c), a console's own rows aside: each console takes the row, or the line says why
   not. No line names a row the settings lack, none twice, and a driver reads by them (snx_console_arg) */
#include "common/utilities/console_settings.h"
#include "snx_settings.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "console_settings: %s:%d: %s\n", __FILE__, __LINE__, #c);                                  \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static bool own(const char* key) {
    static const char* const prefixes[] = {"ctr_", "vita_", "nds_", "gba_", "linux_", "android_"};
    for (size_t i = 0; i < sizeof prefixes / sizeof *prefixes; ++i)
        if (!strncmp(key, prefixes[i], strlen(prefixes[i]))) return true;
    return false;
}

int main(void) {
    int failed = 0, rows = 0, owned = 0;
    int counts[SNX_SYSTEMS][3] = {{0}}; /* lines a console takes, lacks (not applicable), has not yet */
    for (const IniOpt* o = g_console_ini; o->key; ++o) {
        if (own(o->key)) {
            ++owned;
            continue;
        }
        ++rows;
        if (!snx_console_setting(o->key)) {
            fprintf(stderr,
                    "console_settings: %s, a row of the Switch's settings, has no line in "
                    "src/common/utilities/console_settings.c: each console takes it, or the line says why not\n",
                    o->key);
            failed = 1;
        }
    }
    for (int i = 0; g_console_settings[i].key; ++i) {
        const SnxConsoleSetting* line = &g_console_settings[i];
        bool named = false;
        for (const IniOpt* o = g_console_ini; o->key && !named; ++o) named = snx_console_setting(o->key) == line;
        if (!named) {
            fprintf(stderr, "console_settings: the line of %s names no row of the Switch's settings\n", line->key);
            failed = 1;
        }
        for (int k = 0; k < i; ++k)
            if (!strcmp(g_console_settings[k].key, line->key)) {
                fprintf(stderr, "console_settings: %s has two lines\n", line->key);
                failed = 1;
            }
        if (line->why[SNX_SYSTEM_SWITCH].text) {
            fprintf(stderr,
                    "console_settings: %s's line says why the Switch does not take it: every row is the "
                    "Switch's\n",
                    line->key);
            failed = 1;
        }
        for (int c = 1; c < SNX_SYSTEMS; ++c) {
            const SnxConsoleWhy* why = &line->why[c];
            if (why->text && !why->text[0]) {
                fprintf(stderr, "console_settings: %s says no why for the %s\n", line->key,
                        snx_console_name((SnxSystem)c));
                failed = 1;
            }
            ++counts[c][!why->text ? 0 : why->later ? 2 : 1];
        }
    }
    if (failed) return 1;
    /* a line by its key's pattern: a repeat's number, a list's id, a Vulkan extension's row; a console's own none */
    CHECK(snx_console_setting("gamepad_p3_map") &&
          !strcmp(snx_console_setting("gamepad_p3_map")->key, "gamepad_p<N>_map"));
    CHECK(!strcmp(snx_console_setting("controls_stick_l_shown")->key, "controls_<ID>_shown"));
    CHECK(!strcmp(snx_console_setting("controls_stick_l")->key, "controls_<ID>"));
    CHECK(!strcmp(snx_console_setting("controls_snap")->key, "controls_snap"));
    CHECK(!snx_console_setting("ctr_model") && !snx_console_setting("no_such_setting"));
    {
        char key[128];
        size_t n = 0;
        for (const char* c = g_vk_extension_settings[0].name; *c && n + 1 < sizeof key; ++c)
            key[n++] = (char)tolower((unsigned char)*c);
        key[n] = 0;
        CHECK(snx_console_setting(key) && !strcmp(snx_console_setting(key)->key, "vk_<extension>"));
    }
    /* the Switch takes every row; a console what its line says, its row without the Switch's set function */
    CHECK(!snx_console_why(SNX_SYSTEM_SWITCH, "docked") && snx_console_why(SNX_SYSTEM_SWITCH, "ctr_model"));
    CHECK(snx_console_why(SNX_SYSTEM_GBA, "docked") && !snx_console_opt(SNX_SYSTEM_GBA, "docked"));
    const IniOpt* volume = snx_console_opt(SNX_SYSTEM_3DS, "volume");
    CHECK(volume && !volume->set && volume->var == &g_settings.volume);
    /* a driver's command line: a row it takes read (a value the row does not take keeps the one before, said by
       ini_read), a row it does not take refused with why, another argument left to the driver */
    char error[256] = "";
    CHECK(snx_console_arg(SNX_SYSTEM_3DS, "--volume=37", error, sizeof error) == 1 && g_settings.volume == 37);
    CHECK(snx_console_arg(SNX_SYSTEM_3DS, "--volume=loud", error, sizeof error) == 1 && g_settings.volume == 37);
    CHECK(snx_console_arg(SNX_SYSTEM_GBA, "--docked=1", error, sizeof error) == -1);
    CHECK(!strcmp(error, "--docked=1 does nothing in a GBA game: it has no dock: TV mode is the Switch's"));
    CHECK(snx_console_arg(SNX_SYSTEM_NDS, "--tv_resolution=1", error, sizeof error) == -1 &&
          !strncmp(error, "--tv_resolution=1 does nothing in a DS game: ", 45));
    CHECK(snx_console_arg(SNX_SYSTEM_3DS, "--ctr-model=new", error, sizeof error) == 0);
    CHECK(snx_console_arg(SNX_SYSTEM_GBA, "game.gba", error, sizeof error) == 0);
    CHECK(snx_console_arg(SNX_SYSTEM_GBA, "--no-such-setting=1", error, sizeof error) == 0);
    /* online: network on and airplane mode off, as the 3DS and the DS go by */
    g_settings.network = 1, g_settings.airplane_mode = 0;
    CHECK(snx_console_online());
    g_settings.airplane_mode = 1;
    CHECK(!snx_console_online());
    g_settings.network = 0, g_settings.airplane_mode = 0;
    CHECK(!snx_console_online());
    printf("console_settings: %d rows of the Switch's settings with their lines (%d of the consoles' own aside)\n",
           rows, owned);
    for (int c = 1; c < SNX_SYSTEMS; ++c)
        printf("  %-4s takes %d, lacks %d, not yet %d\n", snx_console_name((SnxSystem)c), counts[c][0], counts[c][1],
               counts[c][2]);
    return 0;
}
