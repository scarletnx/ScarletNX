/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A console's own on-screen controls (common/utilities/vpad.c, vpad_console): its controls alone are drawn, pressed,
   named and reset, the rest of the Switch's not there, where it starts in the window's shape and whole in it until the
   player moves it; and its game's settings (vpad_settings): the .ini's, a Switch
   game's lines reading as they do there, then the game's own picks and the command line's in the order
   --launcher-picks says, each setting the console has nothing for said with its value, a value a setting does not take
   refused with why */
#include "common/utilities/ascii.h"
#include "common/utilities/game_ini.h"
#include "common/utilities/vpad.h"
#include "common/utilities/vpad_showcase.h"
#include "common/utilities/vpad_consoles.h"
#include <math.h>
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
static const SnxVpadControl pad[] = {{SNX_VPAD_PLUS, "Start", "ST", {0, 0}, {0.60f, 0.90f}},
                                     {SNX_VPAD_DPAD, "D-pad", "", {0, 0}, {0.10f, 0.80f}},
                                     {SNX_VPAD_A, "A", "A", {0, 0}, {0.80f, 0.99f}}};
static const SnxVpadConsole none = {"GBA", pad, 3, 0}, touch = {"NDS", pad, 3, 1};
/* the Switch's places, which every console's controls start at */
static SnxVpadConfig g_switch;

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

/* the settings back at their defaults, then read from text in the .ini at ini, "a game"'s of dir, picks beside it (NULL
   for none) and the command line's a and b (NULL for none) */
static const char* dir;
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
    if (!failed) vpad_settings(dir, "a game", 3, argv);
    return !failed;
}

/* a button's bit, as vpad_state gives it */
static u64 bit_of(int control) {
    static const u64 bits[SNX_VPAD_COUNT] = {
        [SNX_VPAD_A] = 1u,       [SNX_VPAD_B] = 2u,       [SNX_VPAD_X] = 4u,          [SNX_VPAD_Y] = 8u,
        [SNX_VPAD_L3] = 1u << 4, [SNX_VPAD_R3] = 1u << 5, [SNX_VPAD_L] = 1u << 6,     [SNX_VPAD_R] = 1u << 7,
        [SNX_VPAD_ZL] = 1u << 8, [SNX_VPAD_ZR] = 1u << 9, [SNX_VPAD_PLUS] = 1u << 10, [SNX_VPAD_MINUS] = 1u << 11};
    return bits[control];
}
/* a finger at each button's middle in window presses that button alone (none lies over another): the controls of
   pad, or the Switch's every one where pad is NULL */
static int alone(const SnxVpadConsole* pad, const int window[4]) {
    for (int c = 0; c < SNX_VPAD_COUNT; ++c) {
        int has = !pad;
        for (int i = 0; pad && i < pad->count; ++i) has |= pad->pad[i].control == c;
        if (!has || !bit_of(c)) continue;
        const SnxVpadPlace* at = &vpad_config()->at[c];
        CHECK(vpad_down(1, at->x * (float)window[2], at->y * (float)window[3], window));
        CHECK(buttons() == bit_of(c) && vpad_up(1));
        vpad_release();
    }
    return 0;
}
/* the option buttons (minus and plus, Select and Start; Capture and Home) apart from every other control shown: a
   fortieth of the window's shorter side between their edges at least, not touching */
static int apart(const SnxVpadConsole* pad, const int window[4]) {
    static const int options[] = {SNX_VPAD_MINUS, SNX_VPAD_PLUS, SNX_VPAD_CAPTURE, SNX_VPAD_HOME};
    float gap = 0.025f * vpad_side(window);
    for (size_t k = 0; k < sizeof options / sizeof *options; ++k)
        for (int c = 0; c < SNX_VPAD_COUNT; ++c) {
            int has = !pad, own = !pad;
            for (int i = 0; pad && i < pad->count; ++i) {
                has |= pad->pad[i].control == c;
                own |= pad->pad[i].control == options[k];
            }
            if (!has || !own || c == options[k]) continue;
            float a[4], b[4];
            vpad_box(options[k], window, a);
            vpad_box(c, window, b);
            float dx = fmaxf(b[0] - (a[0] + a[2]), a[0] - (b[0] + b[2])),
                  dy = fmaxf(b[1] - (a[1] + a[3]), a[1] - (b[1] + b[3]));
            if (fmaxf(dx, dy) < gap) {
                fprintf(stderr, "vpad_console: %s and %s %.0f pixels apart in a %dx%d window, under %.0f\n",
                        vpad_name(options[k]), vpad_name(c), (double)fmaxf(dx, dy), window[2], window[3], (double)gap);
                return 1;
            }
        }
    return 0;
}
/* edit mode's bar apart from every control shown as the option buttons are, at each scale of its letters up to 4 (a
   phone's): under the ones in its way */
static int bar_apart(const SnxVpadConsole* pad, const int window[4]) {
    static SnxUiQuad q[4096];
    float gap = 0.025f * vpad_side(window), a[4], b[4];
    vpad_edit(1);
    for (int scale = 1; scale <= 4; ++scale) {
        SnxUiList l = {q, 0, 4096};
        CHECK(vpad_draw(&l, window, scale) != VPAD_DRAW_HIDDEN && vpad_edit_bar(window, a));
        for (int c = 0; c < SNX_VPAD_COUNT; ++c) {
            int has = !pad;
            for (int i = 0; pad && i < pad->count; ++i) has |= pad->pad[i].control == c;
            if (!has) continue;
            vpad_box(c, window, b);
            float dx = fmaxf(b[0] - (a[0] + a[2]), a[0] - (b[0] + b[2])),
                  dy = fmaxf(b[1] - (a[1] + a[3]), a[1] - (b[1] + b[3]));
            if (fmaxf(dx, dy) < gap) {
                fprintf(stderr,
                        "vpad_console: edit mode's bar and %s %.0f pixels apart in a %dx%d window at scale %d\n",
                        vpad_name(c), (double)fmaxf(dx, dy), window[2], window[3], scale);
                vpad_edit(0);
                return 1;
            }
        }
    }
    vpad_edit(0);
    return 0;
}
/* The Switch's own in a window taller than wide (a phone upright): the Vita's arrangement under the picture, ZL and ZR
   over L and R, minus and plus beside them, Capture and Home at the foot, each button pressed alone; turned back,
   the wide places as before; and a place the game's .ini gave stays in both, as the player put it */
static int switch_tall(void) {
    static SnxUiQuad q[512];
    static const int wide[4] = {0, 0, 2340, 1080}, tall[4] = {0, 0, 1080, 2340};
    SnxUiList l = {q, 0, 512};
    const SnxVpadPlace* at = vpad_config()->at;
    g_settings.show_controls = 1;
    CHECK(vpad_draw(&l, tall, 2) == VPAD_DRAW_LAID);
    CHECK(at[SNX_VPAD_ZL].x == 0.16f && at[SNX_VPAD_ZL].y < at[SNX_VPAD_L].y && at[SNX_VPAD_L].y == 0.67f);
    CHECK(at[SNX_VPAD_MINUS].y == at[SNX_VPAD_L].y && at[SNX_VPAD_MINUS].x > at[SNX_VPAD_L].x);
    CHECK(at[SNX_VPAD_PLUS].y == at[SNX_VPAD_R].y && at[SNX_VPAD_PLUS].x < at[SNX_VPAD_R].x);
    CHECK(at[SNX_VPAD_STICK_L].x == snx_vpad_vita.pad[5].tall.x &&
          at[SNX_VPAD_STICK_L].y == snx_vpad_vita.pad[5].tall.y);
    CHECK(at[SNX_VPAD_HOME].y > 0.9f && at[SNX_VPAD_CAPTURE].y > 0.9f);
    if (alone(NULL, tall) || apart(NULL, tall) || bar_apart(NULL, tall)) return 1;
    l.n = 0;
    CHECK(vpad_draw(&l, wide, 2) == VPAD_DRAW_LAID);
    for (int c = 0; c < SNX_VPAD_COUNT; ++c) CHECK(at[c].x == g_switch.at[c].x && at[c].y == g_switch.at[c].y);
    if (alone(NULL, wide) || apart(NULL, wide) || bar_apart(NULL, wide)) return 1;
    CHECK(vpad_ini("controls_a", "0.400 0.300 100"));
    l.n = 0;
    CHECK(vpad_draw(&l, tall, 2) == VPAD_DRAW_LAID && at[SNX_VPAD_A].x == 0.4f && at[SNX_VPAD_A].y == 0.3f);
    l.n = 0;
    CHECK(vpad_draw(&l, wide, 2) == VPAD_DRAW_LAID && at[SNX_VPAD_A].x == 0.4f && at[SNX_VPAD_A].y == 0.3f);
    vpad_reset_layout();
    return 0;
}
/* Every console's controls in a window wider than tall where the Switch's are, but where the console puts one
   elsewhere (the GBA's and the DS's L right over their D-pad, the Vita's Select and Start beside L and R); in one
   taller than wide its own places, L and R over the D-pad and the buttons, the Vita's sticks where the Switch's are in
   the band under its picture (from 64% of the height down). In both, each button pressed alone */
static int switch_places(void) {
    static const SnxVpadConsole* const consoles[] = {&snx_vpad_gba, &snx_vpad_nds, &snx_vpad_vita, &snx_vpad_3ds};
    static SnxUiQuad q[512];
    static const int wide[4] = {0, 0, 2340, 1080}, tall[4] = {0, 0, 1080, 2340};
    for (size_t k = 0; k < sizeof consoles / sizeof *consoles; ++k) {
        const SnxVpadConsole* c = consoles[k];
        int stick = 0;
        for (int i = 0; i < c->count; ++i) stick |= c->pad[i].control == SNX_VPAD_STICK_L;
        vpad_console(c);
        g_settings.show_controls = 1;
        for (int t = 0; t < 2; ++t) {
            const int* window = t ? tall : wide;
            SnxUiList l = {q, 0, 512};
            const SnxVpadPlace* at = vpad_config()->at;
            CHECK(vpad_draw(&l, window, 2) == VPAD_DRAW_LAID);
            for (int i = 0; i < c->count; ++i) {
                const SnxVpadControl* o = &c->pad[i];
                int own = o->wide.x > 0;
                float x = t ? o->tall.x : own ? o->wide.x : g_switch.at[o->control].x;
                float y = t ? o->tall.y : own ? o->wide.y : g_switch.at[o->control].y;
                if (t && c == &snx_vpad_vita && (o->control == SNX_VPAD_STICK_L || o->control == SNX_VPAD_STICK_R))
                    CHECK(fabsf(x - g_switch.at[o->control].x) < 0.001f &&
                          fabsf(y - (0.64f + 0.36f * g_switch.at[o->control].y)) < 0.001f);
                CHECK(at[o->control].x == x && at[o->control].y == y);
            }
            if (alone(c, window) || apart(c, window) || bar_apart(c, window)) return 1;
            int left = stick && !t ? SNX_VPAD_STICK_L : stick ? SNX_VPAD_STICK_L : SNX_VPAD_DPAD;
            CHECK(at[SNX_VPAD_L].y < at[left].y && fabsf(at[SNX_VPAD_L].x - at[left].x) < 0.07f);
            CHECK(at[SNX_VPAD_R].y < at[SNX_VPAD_A].y && fabsf(at[SNX_VPAD_R].x - at[SNX_VPAD_A].x) < 0.11f);
            CHECK(at[SNX_VPAD_MINUS].x < 0.5f && at[SNX_VPAD_PLUS].x > 0.5f);
            if (!t && !stick) /* the D-pad where the Switch's is, the L over it */
                CHECK(at[SNX_VPAD_DPAD].x == g_switch.at[SNX_VPAD_DPAD].x &&
                      at[SNX_VPAD_DPAD].y == g_switch.at[SNX_VPAD_DPAD].y);
            if (stick) /* the Vita's Select and Start beside L and R */
                CHECK(at[SNX_VPAD_MINUS].y == at[SNX_VPAD_L].y && at[SNX_VPAD_PLUS].y == at[SNX_VPAD_R].y &&
                      at[SNX_VPAD_MINUS].x > at[SNX_VPAD_L].x && at[SNX_VPAD_PLUS].x < at[SNX_VPAD_R].x);
        }
    }
    return 0;
}

int main(void) {
    static SnxUiQuad quads[256];
    char error[256];
    SnxUiList l = {quads, 0, 256};
    vpad_defaults(&g_switch);
    if (switch_tall()) return 1;
    { /* the showcase draws the window's console's controls alone: the GBA's has no ZL, no sticks */
        static SnxUiQuad sq[512];
        static const int at[4] = {0, 0, 2340, 1080};
        SnxUiList whole = {sq, 0, 512}, gba = {sq, 0, 512};
        g_settings.showcase = 1;
        CHECK(vpad_showcase_draw(&whole, at, -1, 2) == 1 && vpad_has(SNX_VPAD_ZL));
        vpad_console(&snx_vpad_gba);
        vpad_showcase_input(1, (const i32[4]){0}); /* a change, so it is laid out again */
        CHECK(vpad_showcase_draw(&gba, at, -1, 2) == 1 && !vpad_has(SNX_VPAD_ZL) && vpad_has(SNX_VPAD_L));
        CHECK(gba.n > 0 && gba.n < whole.n);
        vpad_console(NULL);
        g_settings.showcase = 0;
    }
    vpad_set_say(say);
    vpad_set_store(store);
    vpad_console(&none);
    g_settings.show_controls = 1;
    /* the console's three where the Switch has them, and no other of the Switch's */
    CHECK(vpad_down(1, 1860, 470, safe) && buttons() == 1u && vpad_up(1) && !buttons());
    CHECK(vpad_down(1, 1500, 100, safe) && buttons() == 1u << 10 && vpad_up(1));
    vpad_release();
    CHECK(taken(0.23f, 0.78f) && !taken(0.12f, 0.47f) && !taken(0.87f, 0.60f) && !taken(0.77f, 0.78f));
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
    CHECK(g_test.stores == 2 * 3 && vpad_config()->at[SNX_VPAD_A].x == 0.93f);

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
        CHECK(vpad_config()->at[SNX_VPAD_A].x == 0.93f && vpad_ini("controls_a", "0.3 0.4 100"));
        l.n = 0;
        CHECK(vpad_draw(&l, tall, 2) == VPAD_DRAW_LAID && vpad_config()->at[SNX_VPAD_A].x == 0.3f);
        CHECK(vpad_config()->at[SNX_VPAD_DPAD].y == 0.80f);
        vpad_reset_layout();
        CHECK(vpad_config()->at[SNX_VPAD_A].x == 0.80f);
        l.n = 0;
        CHECK(vpad_draw(&l, safe, 2) == VPAD_DRAW_LAID && vpad_config()->at[SNX_VPAD_A].x == 0.93f);
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

    char *root = snx_temp_root(), *made = root ? snx_temp_dir(root) : NULL;
    char* ini = made ? snx_path_join(made, "a game.ini") : NULL;
    dir = made;
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
    vpad_console(&none);
    g_settings.show_controls = 2;
    vpad_settings(dir, "another game", 0, NULL);
    CHECK(g_settings.show_controls == 2 && vpad_config()->at[SNX_VPAD_A].x == 0.93f && !g_test.said[0]);
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
    /* an installed copy's game: its own picks are in the config folder, where the launcher keeps them. Under the home the
       ctest gives the test alone (SNX_TEST_HOME) */
    if (getenv("SNX_TEST_HOME")) {
        char *data = snx_user_data_dir(), *game = data ? snx_format("%s/recompiled/third game", data) : NULL;
        char *kept = game ? snx_game_ini(game, "third game") : NULL, *own = kept ? snx_game_picks(kept) : NULL;
        char *there = own ? snx_dirname(own) : NULL, *driver = game ? snx_format("%s/third game.ini", game) : NULL;
        const char* home = getenv("SNX_TEST_HOME");
        CHECK(there);
        for (size_t i = 0; home[i]; ++i) /* that home, not the user's: Windows joins with its own separator */
            CHECK(data[i] == home[i] || (snx_is_separator(data[i]) && snx_is_separator(home[i])));
        CHECK(!snx_mkdirs(game) && !snx_mkdirs(there) && !snx_write_file(driver, "controls=0\n", 11) &&
              !snx_write_file(own, "controls=1\n", 11));
        vpad_settings(game, "third game", 0, NULL);
        CHECK(g_settings.show_controls == 1);
        printf("vpad_console: the picks at %s go over the .ini at %s\n", own, driver);
        free(driver);
        free(there);
        free(own);
        free(kept);
        free(game);
        free(data);
    }
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
    free(ini);
    free(made);
    free(root);
    if (switch_places()) return 1;
    puts("vpad_console: a console's controls, their settings' order and what it has nothing for passed");
    return 0;
}
