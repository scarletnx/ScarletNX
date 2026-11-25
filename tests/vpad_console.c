/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's own on-screen controls (common/utilities/vpad.c, vpad_console): its controls alone are drawn, pressed,
   named and reset, the rest of the Switch's not there, where it starts in the window's shape and whole in it until the
   player moves it; and its game's settings (vpad_settings): the .ini's, a Switch
   game's lines reading as they do there, then the game's own picks and the command line's in the order
   --launcher-picks says, each setting the console has nothing for said with its value, a value a setting does not take
   refused with why */
#include "common/utilities/game_ini.h"
#include "common/utilities/vpad.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x)                                                                                                       \
    do {                                                                                                               \
        if (!(x)) {                                                                                                    \
            fprintf(stderr, "vpad_console:%d: %s\n", __LINE__, #x);                                                    \
            return 1;                                                                                                  \
        }                                                                                                              \
    } while (0)

static const int safe[4] = {0, 0, 2000, 1000};
static const SnxVpadControl pad[] = {{SNX_VPAD_PLUS, "Start", "ST", {0.75f, 0.10f}, {0.60f, 0.90f}},
                                     {SNX_VPAD_DPAD, "D-pad", "", {0.25f, 0.75f}, {0.10f, 0.80f}},
                                     {SNX_VPAD_A, "A", "A", {0.90f, 0.50f}, {0.80f, 0.99f}}};
static const SnxVpadConsole none = {"GBA", pad, 3, 0}, touch = {"NDS", pad, 3, 1};

static struct {
    char said[1024]; /* the lines said, a newline after each */
    int stores;
} g_test;
static void say(const char* line) {
    size_t n = strlen(g_test.said);
    snprintf(g_test.said + n, sizeof g_test.said - n, "%s\n", line);
}
static void store(const char* key, const char* value) {
    (void)key, (void)value;
    ++g_test.stores;
}

static u64 buttons(void) {
    u64 b;
    i32 s[4];
    vpad_state(&b, s);
    return b;
}
/* a finger at the middle of control, if it were there as the Switch has it: whether the controls took it, let go */
static int taken(float x, float y) {
    int t = vpad_down(1, x * (float)safe[2], y * (float)safe[3], safe);
    vpad_up(1);
    vpad_release();
    return t;
}

/* the settings back at their defaults, then read from text in the .ini at ini, picks beside it (NULL for none) and the
   command line's a and b (NULL for none) */
static int settings(const SnxVpadConsole* console, const char* ini, const char* text, const char* picks, char* a,
                    char* b) {
    char *argv[] = {"program", a ? a : "--", b ? b : "--"},
         *beside = snx_format("%.*s.picks", (int)strlen(ini) - 4, ini);
    g_test.said[0] = 0;
    vpad_console(console);
    g_settings.show_controls = g_settings.touch_screen = 2;
    g_settings.controls_scale = 50;
    for (int c = 0; c < SNX_VPAD_COUNT; ++c) g_settings.control_shown[c] = 1;
    int failed = snx_write_file(ini, text, strlen(text));
    if (picks)
        failed |= snx_write_file(beside, picks, strlen(picks));
    else if (snx_is_file(beside))
        failed |= snx_remove_tree(beside);
    free(beside);
    if (!failed) vpad_settings(ini, 3, argv);
    return !failed;
}

int main(void) {
    static SnxUiQuad quads[256];
    char error[256];
    SnxUiList l = {quads, 0, 256};
    vpad_set_say(say);
    vpad_set_store(store);
    vpad_console(&none);
    g_settings.show_controls = 1;
    /* the console's three, where it puts them, and no other of the Switch's */
    CHECK(vpad_down(1, 1800, 500, safe) && buttons() == 1u && vpad_up(1) && !buttons());
    CHECK(vpad_down(1, 1500, 100, safe) && buttons() == 1u << 10 && vpad_up(1));
    vpad_release();
    CHECK(taken(0.25f, 0.75f) && !taken(0.93f, 0.47f) && !taken(0.87f, 0.60f) && !taken(0.12f, 0.47f));
    CHECK(!taken(0.64f, 0.90f) && !taken(0.36f, 0.90f) &&
          !(vpad_actions() & ~SNX_VPAD_DO_BUZZ)); /* no Home, no Capture */
    CHECK(!strcmp(vpad_name(SNX_VPAD_PLUS), "Start") && !strcmp(vpad_name(SNX_VPAD_B), "?"));
    /* drawn: Start's and A's shapes with their labels, the D-pad's cross and its four arms */
    CHECK(vpad_draw(&l, safe, 2) == VPAD_DRAW_LAID && l.n == 2 + 5);
    CHECK(l.q[0].v == ('S' | 'T' << 8) && ui_view(&l.q[0]).ink == UI_SHAPE && l.q[l.n - 1].v == 'A');
    l.n = 0;
    CHECK(vpad_draw(&l, safe, 2) == VPAD_DRAW_KEPT && l.n == 7);
    g_settings.show_controls = 0;
    l.n = 0;
    CHECK(vpad_draw(&l, safe, 2) == VPAD_DRAW_HIDDEN && !l.n);
    /* the reset keeps the console's alone: a place and shown for each */
    vpad_config()->at[SNX_VPAD_A].x = 0.1f;
    vpad_reset_layout();
    CHECK(g_test.stores == 2 * 3 && vpad_config()->at[SNX_VPAD_A].x == 0.90f);

    /* a window taller than wide: where they start in one, A off its foot drawn and pressed whole at it; turned back, as
       before; one the player moved stays in both */
    {
        const int tall[4] = {0, 0, 1000, 2000};
        SnxUiQuadView a;
        g_settings.show_controls = 1;
        l.n = 0;
        CHECK(vpad_draw(&l, tall, 2) == VPAD_DRAW_LAID && vpad_config()->at[SNX_VPAD_PLUS].x == 0.60f);
        a = ui_view(&l.q[l.n - 1]);
        CHECK(a.v == 'A' && a.x == 800 - 55 && a.y == 2000 - 110 && a.w == 110 && a.h == 110);
        CHECK(vpad_down(1, 800, 1895, tall) && buttons() == 1u && vpad_up(1) && !buttons());
        CHECK(vpad_down(1, 750, 200, safe) == 0 && vpad_down(1, 1500, 100, safe) && buttons() == 1u << 10 &&
              vpad_up(1));
        vpad_release();
        CHECK(vpad_config()->at[SNX_VPAD_A].x == 0.90f && vpad_ini("controls_a", "0.3 0.4 100"));
        l.n = 0;
        CHECK(vpad_draw(&l, tall, 2) == VPAD_DRAW_LAID && vpad_config()->at[SNX_VPAD_A].x == 0.3f);
        CHECK(vpad_config()->at[SNX_VPAD_DPAD].y == 0.80f);
        vpad_reset_layout();
        CHECK(vpad_config()->at[SNX_VPAD_A].x == 0.80f);
        l.n = 0;
        CHECK(vpad_draw(&l, safe, 2) == VPAD_DRAW_LAID && vpad_config()->at[SNX_VPAD_A].x == 0.90f);
    }

    /* the command line's: which are theirs, and a value refused */
    CHECK(vpad_arg("--controls=1", error, sizeof error) == 1 &&
          vpad_arg("--controls-a-shown=0", error, sizeof error) == 1);
    CHECK(vpad_arg("--touch=1", error, sizeof error) == 1 && vpad_arg("--launcher-picks=1", error, sizeof error) == 1);
    CHECK(!vpad_arg("--fullscreen=1", error, sizeof error) && !vpad_arg("a.gba", error, sizeof error));
    CHECK(vpad_arg("--controls=3", error, sizeof error) == -1 && strstr(error, "controls=3"));
    CHECK(vpad_arg("--controls-scale=big", error, sizeof error) == -1 && strstr(error, "controls_scale=big"));
    CHECK(vpad_arg("--launcher-picks=2", error, sizeof error) == -1 &&
          strstr(error, "--launcher-picks=2 is not 0 or 1"));

    char *root = snx_temp_root(), *dir = root ? snx_temp_dir(root) : NULL;
    char* ini = dir ? snx_path_join(dir, "a game.ini") : NULL;
    CHECK(ini);
    /* a Switch game's .ini read as there: a place, an older version's fourth number (shown), a broken one left */
    CHECK(settings(&none, ini,
                   "[Game]\nsystem=gba\n[On-screen controls]\ncontrols=1\ncontrols_a=0.930 0.470 100\n"
                   "controls_dpad=0.5 0.25 150 0\ncontrols_plus=0.5 0.5\ncontrols_scale=80\n",
                   NULL, NULL, NULL));
    CHECK(vpad_config()->at[SNX_VPAD_A].x == 0.930f && vpad_config()->at[SNX_VPAD_A].y == 0.470f);
    CHECK(vpad_config()->at[SNX_VPAD_A].size == 100 && g_settings.control_shown[SNX_VPAD_A]);
    CHECK(vpad_config()->at[SNX_VPAD_DPAD].size == 150 && !g_settings.control_shown[SNX_VPAD_DPAD]);
    CHECK(vpad_config()->at[SNX_VPAD_PLUS].x == 0.75f && g_settings.show_controls == 1);
    CHECK(g_settings.controls_scale == 80 && !g_test.said[0]);
    /* no .ini yet: the defaults, nothing said */
    char* missing = snx_path_join(dir, "missing.ini");
    vpad_console(&none);
    g_settings.show_controls = 2;
    vpad_settings(missing, 0, NULL);
    CHECK(g_settings.show_controls == 2 && vpad_config()->at[SNX_VPAD_A].x == 0.90f && !g_test.said[0]);
    /* the order: the .ini's, the game's own, the command line's; the launcher's picks under the game's own */
    CHECK(settings(&none, ini, "controls=0\n", "controls=1\n", NULL, NULL) && g_settings.show_controls == 1);
    CHECK(settings(&none, ini, "controls=0\n", "controls=1\n", "--controls=2", NULL) && g_settings.show_controls == 2);
    CHECK(settings(&none, ini, "controls=0\n", "controls=1\n", "--controls=2", "--launcher-picks=1") &&
          g_settings.show_controls == 1);
    CHECK(settings(&none, ini, "controls=0\n", NULL, "--launcher-picks=1", "--controls=2") &&
          g_settings.show_controls == 2);
    /* what the console has nothing for: said with its value, and the one in effect */
    CHECK(settings(&none, ini, "touch=1\ncontrols_zl=0.1 0.2 100\n", NULL, "--controls-home-shown=0",
                   "--controls-rel-center=0"));
    CHECK(!g_settings.touch_screen && g_settings.relative_stick_center);
    CHECK(strstr(g_test.said, "settings: touch=1 does nothing: the GBA has no touch screen, and it keeps 0"));
    CHECK(strstr(g_test.said, "settings: controls_zl=0.1 0.2 100 does nothing: the GBA has no such control\n"));
    CHECK(strstr(g_test.said, "settings: controls_home_shown=0 does nothing: the GBA has no such control\n"));
    CHECK(strstr(g_test.said, "settings: controls_rel_center=0 does nothing: the GBA has no stick\n"));
    CHECK(settings(&touch, ini, "touch=1\n", NULL, NULL, NULL) && g_settings.touch_screen == 1 && !g_test.said[0]);
    CHECK(settings(&touch, ini, "", NULL, NULL, NULL) && g_settings.touch_screen == 2);
    /* a place as a window keeps it: in the controls' section, made where the .ini has none, the rest as it was */
    const char* const before = "[Game]\nsystem=gba\n";
    size_t size;
    CHECK(!snx_write_file(ini, before, strlen(before)));
    CHECK(!snx_ini_record_in(ini, SNX_VPAD_PAGE, "controls_a", "0.500 0.500 100"));
    CHECK(!snx_ini_record_in(ini, SNX_VPAD_PAGE, "controls_b", "0.1 0.2 80"));
    CHECK(!snx_ini_record_in(ini, SNX_VPAD_PAGE, "controls_a", "0.250 0.500 100") && !snx_ini_record(ini, "code", "X"));
    char* text = (char*)snx_read_file(ini, &size);
    CHECK(text && !strcmp(text, "[Game]\ncode=X\nsystem=gba\n\n[On-screen controls]\ncontrols_a=0.250 0.500 100\n"
                                "controls_b=0.1 0.2 80\n"));
    free(text);
    CHECK(!snx_remove_tree(dir));
    free(missing);
    free(ini);
    free(dir);
    free(root);
    puts("vpad_console: a console's controls, their settings' order and what it has nothing for passed");
    return 0;
}
