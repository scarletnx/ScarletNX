/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The program's entry: the MCP server (--mcp) and the host driver's commands (extract, recompile) first, and anything
   else is a game to run */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/utilities/setup.h"
#include "nx/runtime/driver/driver.h"
#include "nx/runtime/driver/mcp.h"
#include "nx/runtime/entry.h"
#include "nx/runtime/main.h"
#include "nx/runtime/options.h" /* g_ini_sections, ini_list_text */
#ifdef SNX_WITH_GUI
#include "gui/gui.h"
#include "gui/theme.h"
#if defined(__APPLE__)
#include <TargetConditionals.h>
#if TARGET_OS_IOS
#include "gui/apple.h"
#define SNX_MENU_THEN_GAME 1
#endif
#endif
#endif
#ifdef SNX_WITH_GUI
/* the game's settings as scarletnx --settings prints them, for the menu built into this program (snx_menu_settings) */
static char* runtime_settings(void) {
    size_t n = ini_list_text(g_ini_sections, g_ini_sections_count, NULL, 0);
    char* text = (char*)malloc(n + 1);
    if (text) ini_list_text(g_ini_sections, g_ini_sections_count, text, n + 1);
    return text;
}
#endif
int snx_switch_main(int argc, char** argv) {
    int result;
    char why[160];
#ifdef SNX_MENU_THEN_GAME
    gui_apple_log_start(); /* before anything prints: a phone has no console */
#endif
    /* a launcher reads our progress through pipes: lines as they end, errors at once */
    setvbuf(stdout, NULL, _IOLBF, BUFSIZ);
    setvbuf(stderr, NULL, _IONBF, 0);
    /* the setup's own options, for the driver and the menu, before anything reads the rest */
    argc = snx_setup_options(argc, argv, why, sizeof why);
    if (argc < 0) {
        fprintf(stderr, "scarletnx: %s\n", why);
        return 2;
    }
#ifdef SNX_WITH_GUI
    snx_menu = snx_gui_main;
    snx_menu_run_theme = gui_run_theme;
    snx_menu_settings = runtime_settings;
#endif
    if (argc > 1 && !strcmp(argv[1], "--mcp")) return snx_mcp(argc, argv);
#ifdef SNX_WITH_GUI
    /* an error's panel's action, from a game no launcher waited for: the menu, opened at that page */
    if (argc == 2 && !strncmp(argv[1], "--open=", 7)) {
        char* self = snx_executable(NULL); /* the program that runs Switch inputs: this one */
        int status;
        snx_gui_open(argv[1] + 7);
        status = snx_menu(self ? self : argv[0]);
        free(self);
        return status;
    }
#endif
#if defined(SNX_WITH_GUI) && !defined(__ANDROID__) && !defined(SNX_MENU_THEN_GAME)
    if (argc > 1 && !strncmp(argv[1], "--migrate", 9) && (!argv[1][9] || argv[1][9] == '=')) {
        if (argc > 2) {
            fprintf(stderr, "scarletnx: %s takes nothing after it: %s\n", argv[1], argv[2]);
            return 2;
        }
        return snx_gui_migrate(argv[0], argv[1]);
    }
#endif
    result = snx_driver(argc, argv);
    if (result >= 0) return result;
#ifdef SNX_MENU_THEN_GAME
    /* iOS has one process: the menu runs first, then the game it picked, here */
    if (argc <= 1) {
        char** played;
        int played_argc = 0;
        result = snx_gui_main(argv[0]);
        played = gui_apple_played(&played_argc);
        if (!played) return result;
        return snx_main(played_argc, played);
    }
#endif
    return snx_main(argc, argv);
}
