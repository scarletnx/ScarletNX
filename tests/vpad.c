/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The on-screen controls (runtime/platform/vpad.c): what fingers on them press, edit mode, the .ini keys, the draw */
#include "nx/runtime/platform/vpad.h"
#include "nx/runtime/platform/controls_atlas.h"
#include <assert.h>
#include <stdarg.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ui.c's lock, without the platform layer */
#include "nx/runtime/platform/plat.h"
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }

/* ui.c's warning for a picture that does not draw */
/* vpad.c's check of settings.txt's keys against its controls: a mismatch fails the test */
void fatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    abort();
}
/* settings.txt's Display table, which holds showcase_corner, names options.c's setter of the game's language */
void options_language_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
void options_tv_mode_set(const IniOpt* o, const char* v) {
    (void)o;
    (void)v;
}
static char logged[256]; /* the last line logged */
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    (void)lvl;
    va_start(ap, fmt);
    vsnprintf(logged, sizeof logged, fmt, ap);
    va_end(ap);
}

static const int safe[4] = {0, 0, 2000, 1000};
static char stored_key[64], stored_value[64];
static int stores;
static void store(const char* key, const char* value) {
    snprintf(stored_key, sizeof stored_key, "%s", key);
    snprintf(stored_value, sizeof stored_value, "%s", value);
    ++stores;
}
/* a control's middle on safe, as box() puts it */
static float mx(int c) { return (float)safe[0] + vpad_config()->at[c].x * (float)safe[2]; }
static float my(int c) { return (float)safe[1] + vpad_config()->at[c].y * (float)safe[3]; }
static u64 buttons(i32 sticks[4]) {
    u64 b;
    i32 s[4];
    vpad_state(&b, sticks ? sticks : s);
    return b;
}
static SnxUiQuad quads[512];
static int draw(SnxUiList* l) {
    l->q = quads;
    l->n = 0;
    l->max = 512;
    vpad_draw(l, safe);
    return l->n;
}
/* the quad drawn with that sprite's texels, or NULL */
static const SnxUiQuad* drawn(const SnxUiList* l, int sprite) {
    int i;
    for (i = 0; i < l->n; ++i)
        if (l->q[i].u == snx_controls_sprites[sprite][0] && l->q[i].v == snx_controls_sprites[sprite][1])
            return &l->q[i];
    return NULL;
}

int main(void) {
    SnxVpadConfig d;
    SnxUiList l;
    i32 st[4];
    vpad_set_store(store);
    vpad_defaults(&d);
    assert(g_settings.controls_scale == 50 && g_settings.controls_opacity == 100 && g_settings.relative_stick_center &&
           g_settings.d_pad_slide && g_settings.haptics && !g_settings.snap_to_grid);
    /* off, nothing is taken nor drawn */
    assert(vpad_ini("controls", "0") && !g_settings.show_controls && !vpad_shown());
    assert(!vpad_down(1, mx(SNX_VPAD_A), my(SNX_VPAD_A), safe) && !draw(&l));
    assert(vpad_ini("controls", "1") && g_settings.show_controls);

    /* a button held by a finger, then by two; the second lift lets it go. Off the controls, a finger is the game's */
    assert(vpad_down(1, mx(SNX_VPAD_A), my(SNX_VPAD_A), safe) && buttons(NULL) == 1);
    assert(vpad_actions() == SNX_VPAD_DO_BUZZ);
    assert(vpad_down(2, mx(SNX_VPAD_A) + 5, my(SNX_VPAD_A), safe) &&
           vpad_down(3, mx(SNX_VPAD_B), my(SNX_VPAD_B), safe));
    assert(buttons(NULL) == 3);
    assert(vpad_up(1) && buttons(NULL) == 3 && vpad_up(2) && buttons(NULL) == 2 && vpad_up(3) && !buttons(NULL));
    assert(!vpad_down(4, 1000, 500, safe) && !vpad_up(4));
    /* a tap let go before the game reads the controls still reaches it, once: a button's and the D-pad's */
    assert(vpad_down(4, mx(SNX_VPAD_A), my(SNX_VPAD_A), safe) && vpad_up(4) && buttons(NULL) == 1 && !buttons(NULL));
    assert(vpad_down(4, mx(SNX_VPAD_DPAD), my(SNX_VPAD_DPAD) + 75, safe) && vpad_up(4)); /* past half its half */
    assert(buttons(NULL) == 1u << 15 && !buttons(NULL));                                 /* down */
    /* and a slide through a direction between two reads: the middle, right, the middle again */
    assert(vpad_down(4, mx(SNX_VPAD_DPAD), my(SNX_VPAD_DPAD), safe) && !buttons(NULL));
    vpad_move(4, mx(SNX_VPAD_DPAD) + 75, my(SNX_VPAD_DPAD), safe);
    vpad_move(4, mx(SNX_VPAD_DPAD), my(SNX_VPAD_DPAD), safe);
    assert(buttons(NULL) == 1u << 14 && !buttons(NULL) && vpad_up(4));
    assert(vpad_down(4, mx(SNX_VPAD_A), my(SNX_VPAD_A), safe));
    vpad_release(); /* the menu opened: what was pressed before is not the game's */
    assert(!buttons(NULL));
    /* the shoulders, the stick buttons, plus and minus: their npad bits */
    assert(vpad_down(5, mx(SNX_VPAD_ZR), my(SNX_VPAD_ZR), safe) && buttons(NULL) == 1u << 9 && vpad_up(5));
    assert(vpad_down(5, mx(SNX_VPAD_L3), my(SNX_VPAD_L3), safe) && buttons(NULL) == 1u << 4 && vpad_up(5));
    assert(vpad_down(5, mx(SNX_VPAD_MINUS), my(SNX_VPAD_MINUS), safe) && buttons(NULL) == 1u << 11 && vpad_up(5));
    /* Home and Capture are no npad buttons: the menu and a screenshot */
    vpad_actions();
    assert(vpad_down(6, mx(SNX_VPAD_HOME), my(SNX_VPAD_HOME), safe) && !buttons(NULL));
    assert(vpad_actions() == (SNX_VPAD_DO_MENU | SNX_VPAD_DO_BUZZ) && vpad_up(6));
    assert(vpad_down(6, mx(SNX_VPAD_CAPTURE), my(SNX_VPAD_CAPTURE), safe) && vpad_actions() & SNX_VPAD_DO_SHOT);
    vpad_up(6);
    /* no buzz with haptics off */
    vpad_ini("controls_haptics", "0");
    vpad_actions();
    assert(vpad_down(7, mx(SNX_VPAD_Y), my(SNX_VPAD_Y), safe) && vpad_up(7) && !vpad_actions());
    assert(buttons(NULL) == 1u << 3 && !buttons(NULL)); /* the tap, read once */
    vpad_ini("controls_haptics", "1");

    /* the D-pad: its middle holds no direction; past half its half, a direction; diagonals; a slide changes it */
    {
        float x = mx(SNX_VPAD_DPAD), y = my(SNX_VPAD_DPAD), half = 0.25f * 1000 / 2;
        assert(vpad_down(8, x, y, safe) && !buttons(NULL));
        vpad_move(8, x + half * 0.6f, y, safe);
        assert(buttons(NULL) == 1u << 14);
        vpad_move(8, x - half * 0.6f, y - half * 0.6f, safe);
        assert(buttons(NULL) == ((1u << 12) | (1u << 13)));
        vpad_ini("controls_dpad_slide", "0");
        vpad_move(8, x, y + half * 0.6f, safe);
        assert(buttons(NULL) == ((1u << 12) | (1u << 13)));
        vpad_ini("controls_dpad_slide", "1");
        vpad_move(8, x, y + half * 0.6f, safe);
        assert(buttons(NULL) == 1u << 15);
        /* drawn as the up sprite turned half round */
        draw(&l);
        assert(drawn(&l, SNX_SPRITE_DPAD_UP) && drawn(&l, SNX_SPRITE_DPAD_UP)->turn == 2 &&
               !drawn(&l, SNX_SPRITE_DPAD));
        assert(vpad_up(8) && !buttons(NULL));
    }

    /* a stick: its middle where the finger lands, up is positive, the edge of its ring is full; a lift centres it */
    {
        float x = mx(SNX_VPAD_STICK_L) + 30, y = my(SNX_VPAD_STICK_L) - 20, radius = 0.30f * 1000 / 2;
        assert(vpad_down(9, x, y, safe) && !buttons(st) && !st[0] && !st[1]);
        vpad_move(9, x + radius / 2, y, safe);
        buttons(st);
        assert(st[0] > 16000 && st[0] < 16700 && !st[1]);
        vpad_move(9, x, y - radius * 3, safe);
        buttons(st);
        assert(!st[0] && st[1] == 32767);
        vpad_move(9, x + radius * 1.5f, y, safe); /* past the ring's edge: no further than it */
        buttons(st);
        assert(st[0] == 32767 && !st[1]);
        vpad_move(9, x + radius * 3, y + radius * 3, safe); /* a diagonal stays on the circle */
        buttons(st);
        assert(st[0] > 23000 && st[0] < 23300 && st[1] < -23000 && st[1] > -23300);
        assert(vpad_up(9) && !buttons(st) && !st[0] && !st[1]);
        /* without a relative middle, the ring's own: a finger off it is pushed already */
        vpad_ini("controls_rel_center", "0");
        assert(vpad_down(9, mx(SNX_VPAD_STICK_R) - radius / 2, my(SNX_VPAD_STICK_R), safe));
        buttons(st);
        assert(st[2] < -16000 && st[2] > -16700 && !st[3] && !st[0]);
        vpad_up(9);
        vpad_ini("controls_rel_center", "1");
    }

    /* a hidden control is not there; release lets every finger go */
    vpad_ini("controls_x", "0.870 0.340 100 0");
    assert(!g_settings.control_shown[SNX_VPAD_X] && !vpad_down(10, mx(SNX_VPAD_X), my(SNX_VPAD_X), safe));
    draw(&l);
    assert(!drawn(&l, SNX_SPRITE_X) && drawn(&l, SNX_SPRITE_Y));
    vpad_ini("controls_x", "0.870 0.340 100 1");
    assert(vpad_down(10, mx(SNX_VPAD_X), my(SNX_VPAD_X), safe) && buttons(NULL) == 4);
    draw(&l);
    assert(drawn(&l, SNX_SPRITE_X_PRESSED) && !drawn(&l, SNX_SPRITE_X));
    vpad_release();
    assert(!buttons(NULL) && !vpad_up(10));

    /* opacity and scale: the alpha of every sprite, the size of every control */
    vpad_ini("controls_opacity", "50");
    draw(&l);
    assert(drawn(&l, SNX_SPRITE_A)->rgba >> 24 == 127);
    assert(drawn(&l, SNX_SPRITE_A)->w > 109 && drawn(&l, SNX_SPRITE_A)->w < 111); /* 11% of 1000 at 100% */
    vpad_ini("controls_scale", "100");
    draw(&l);
    assert(drawn(&l, SNX_SPRITE_A)->w > 164 && drawn(&l, SNX_SPRITE_A)->w < 166);
    vpad_ini("controls_scale", "50");
    vpad_ini("controls_opacity", "100");

    /* the .ini: a place read back clamped, a broken one ignored, others not taken */
    assert(vpad_ini("controls_b", "1.5 -2 900 1") && vpad_config()->at[SNX_VPAD_B].x == 1 &&
           vpad_config()->at[SNX_VPAD_B].y == 0 && vpad_config()->at[SNX_VPAD_B].size == SNX_VPAD_SIZE_MAX);
    assert(vpad_ini("controls_b", "nonsense") && vpad_config()->at[SNX_VPAD_B].x == 1);
    assert(vpad_ini("controls_b", "nan 0.5 100 1") && vpad_config()->at[SNX_VPAD_B].x == 1); /* NaN isn't a place */
    assert(vpad_ini("controls_b", "1 0 99999999999 1") && vpad_config()->at[SNX_VPAD_B].size == SNX_VPAD_SIZE_MAX);
    assert(vpad_ini("controls_scale", "300") && g_settings.controls_scale == 100 && !vpad_ini("touch", "1") &&
           !vpad_ini("seconds", "5"));
    vpad_ini("controls_scale", "50");
    vpad_store_control(SNX_VPAD_B);
    assert(!strcmp(stored_key, "controls_b") && !strcmp(stored_value, "1.000 0.000 200"));
    /* shown in a key of its own; an older version's fourth number of the place still read */
    assert(vpad_ini("controls_b", "0.5 0.5 100") && g_settings.control_shown[SNX_VPAD_B]);
    assert(vpad_ini("controls_b_shown", "0") && !g_settings.control_shown[SNX_VPAD_B]);
    assert(vpad_ini("controls_b", "0.5 0.5 100") && !g_settings.control_shown[SNX_VPAD_B]);
    assert(vpad_ini("controls_b", "0.5 0.5 100 1") && g_settings.control_shown[SNX_VPAD_B]);
    assert(vpad_ini("controls_b", "0.5 0.5 100 x") && g_settings.control_shown[SNX_VPAD_B]); /* not a fourth */
    assert(vpad_ini("controls_b", "0.7 0.5") && vpad_config()->at[SNX_VPAD_B].x == 0.5f);   /* not a place */
    assert(vpad_ini("controls_b_shown", "0"));
    vpad_store_shown(SNX_VPAD_B);
    assert(!strcmp(stored_key, "controls_b_shown") && !strcmp(stored_value, "0"));
    assert(vpad_ini("controls_b_shown", "1") && vpad_ini("controls_b", "1 0 200"));
    vpad_store(&g_settings.haptics);
    assert(!strcmp(stored_key, "controls_haptics") && !strcmp(stored_value, "1"));
    /* the reset: every control back, its place and shown kept */
    stores = 0;
    vpad_reset_layout();
    assert(stores == 2 * SNX_VPAD_COUNT && vpad_config()->at[SNX_VPAD_B].x == d.at[SNX_VPAD_B].x &&
           vpad_config()->at[SNX_VPAD_B].size == 100);

    /* edit mode: every finger is its, a control dragged moves and is kept on its lift, on the grid when snapping */
    vpad_ini("controls", "0");
    vpad_edit(1);
    assert(vpad_shown()); /* off, edit mode still shows the pad */
    vpad_ini("controls", "1");
    assert(vpad_editing() && vpad_down(11, 1000, 500, safe) && vpad_up(11));
    stores = 0;
    {
        float x = mx(SNX_VPAD_A), y = my(SNX_VPAD_A);
        assert(vpad_down(12, x + 3, y + 2, safe) && !buttons(NULL));
        vpad_move(12, x - 197, y - 98, safe);
        assert(!stores && vpad_config()->at[SNX_VPAD_A].x > 0.8299f && vpad_config()->at[SNX_VPAD_A].x < 0.8301f);
        assert(vpad_config()->at[SNX_VPAD_A].y > 0.3699f && vpad_config()->at[SNX_VPAD_A].y < 0.3701f);
        assert(vpad_up(12) && stores == 1 && !strcmp(stored_key, "controls_a"));
        vpad_ini("controls_snap", "1");
        assert(vpad_down(12, mx(SNX_VPAD_A), my(SNX_VPAD_A), safe));
        vpad_move(12, mx(SNX_VPAD_A) + 25, my(SNX_VPAD_A) + 25, safe);
        /* the grid is a 32nd of the shorter side: 31.25 pixels */
        {
            float gx = vpad_config()->at[SNX_VPAD_A].x * 2000 / 31.25f,
                  gy = vpad_config()->at[SNX_VPAD_A].y * 1000 / 31.25f;
            assert(fabsf(gx - floorf(gx + 0.5f)) < 0.01f && fabsf(gy - floorf(gy + 0.5f)) < 0.01f);
            assert(gx > 53.5f && gx < 54.5f && gy > 12.5f && gy < 13.5f); /* the nearest line, not the one before */
        }
        vpad_up(12);
        draw(&l); /* the grid's lines and the bar's text come first, then the controls */
        assert(l.n > SNX_VPAD_COUNT + 60);
        vpad_ini("controls_snap", "0");
        /* a drag that edit mode's end cuts short (Esc, the menu) is kept too */
        stores = 0;
        assert(vpad_down(14, mx(SNX_VPAD_Y), my(SNX_VPAD_Y), safe));
        vpad_move(14, mx(SNX_VPAD_Y) - 100, my(SNX_VPAD_Y) - 50, safe);
        vpad_edit(0);
        assert(stores == 1 && !strcmp(stored_key, "controls_y"));
        vpad_edit(1);
    }
    /* the bar: + on the selected control sizes it, Done ends edit mode */
    {
        SnxUiList bar;
        float plus_x = 0, done_x = 0, y;
        int i, s = 1000 / 360;
        assert(vpad_down(13, mx(SNX_VPAD_A), my(SNX_VPAD_A), safe) && vpad_up(13));
        draw(&bar);
        /* "[ + ]" and "[ Done ]": their glyphs are the font's, at the bar's row */
        y = (float)(8 * s);
        for (i = 0; i < bar.n; ++i)
            if (bar.q[i].y == y && bar.q[i].u == SNX_CONTROLS_FONT_X + '+' % 16 * 8 &&
                bar.q[i].v == SNX_CONTROLS_FONT_Y + '+' / 16 * 16)
                plus_x = bar.q[i].x;
            else if (bar.q[i].y == y && bar.q[i].u == SNX_CONTROLS_FONT_X + 'D' % 16 * 8 &&
                     bar.q[i].v == SNX_CONTROLS_FONT_Y + 'D' / 16 * 16)
                done_x = bar.q[i].x;
        assert(plus_x > 0 && done_x > plus_x);
        stores = 0;
        assert(vpad_down(14, plus_x + 2, y + 2, safe) && vpad_up(14));
        assert(vpad_config()->at[SNX_VPAD_A].size == 110 && stores == 1);
        assert(vpad_down(15, done_x + 2, y + 2, safe) && !vpad_editing());
    }
    /* the showcase: player 1's controller as the game got it, in showcase_corner's corner, the frames a second over it */
    {
        static const i32 none[4], right[4] = {32767, 0, 0, 0}, up[4] = {0, 32767, 0, 0}, corner[4] = {32767, 32767};
        const IniOpt* where = ini_find(g_display_ini, "showcase_corner");
        const SnxUiQuad *ring, *knob, *q;
        float box[4][2] = {{20, 20}, {1480, 20}, {20, 680}, {1480, 680}}; /* 300 high, 500 wide, 20 from the corner */
        int i, k, digits;
        assert(where && !strcmp(g_settings.showcase_corner, "br"));
        l.q = quads;
        l.max = 512;
        l.n = 0;
        g_settings.showcase = 0;
        vpad_showcase_draw(&l, safe, 60);
        assert(!l.n);
        g_settings.showcase = 1;
        vpad_showcase_input(0, none);
        vpad_showcase_draw(&l, safe, 60);
        assert(l.n > 13 && drawn(&l, SNX_SPRITE_A) && !drawn(&l, SNX_SPRITE_A_PRESSED) && drawn(&l, SNX_SPRITE_DPAD));
        assert(!drawn(&l, SNX_SPRITE_HOME) && !drawn(&l, SNX_SPRITE_CAPTURE) && !drawn(&l, SNX_SPRITE_L3) &&
               !drawn(&l, SNX_SPRITE_R3) && !drawn(&l, SNX_SPRITE_STICK_PRESSED));
        /* "60 FPS" at the box's top, in the font */
        for (i = digits = 0; i < l.n; ++i)
            if (l.q[i].v == SNX_CONTROLS_FONT_Y + '6' / 16 * 16 &&
                (l.q[i].u == SNX_CONTROLS_FONT_X + '6' % 16 * 8 || l.q[i].u == SNX_CONTROLS_FONT_X + '0' % 16 * 8)) {
                assert(l.q[i].y > 680 && l.q[i].y < 680 + 30);
                ++digits;
            }
        assert(digits == 2);
        /* each corner holds every quad; one the setting doesn't take keeps the corner, and says which */
        for (k = 0; k < 4; ++k) {
            static const char* const name[4] = {"tl", "tr", "bl", "br"};
            ini_read(where, name[k]);
            l.n = 0;
            vpad_showcase_draw(&l, safe, -1);
            for (i = 0; i < l.n; ++i)
                assert(l.q[i].x >= box[k][0] - 0.01f && l.q[i].x + l.q[i].w <= box[k][0] + 500.01f &&
                       l.q[i].y >= box[k][1] - 0.01f && l.q[i].y + l.q[i].h <= box[k][1] + 300.01f);
            assert(l.q[0].x == box[k][0] && l.q[0].y == box[k][1] && l.q[0].w == 500 && l.q[0].h == 300);
            for (i = 0; i < l.n; ++i) /* no frames a second before the first half second */
                assert(!(l.q[i].u == SNX_CONTROLS_FONT_X + 'F' % 16 * 8 &&
                         l.q[i].v == SNX_CONTROLS_FONT_Y + 'F' / 16 * 16));
        }
        ini_read(where, "tl");
        ini_read(where, "lt");
        assert(!strcmp(g_settings.showcase_corner, "tl"));
        assert(!strcmp(logged, "settings: showcase_corner=lt is not tl, tr, bl or br: it keeps tl\n"));
        ini_read(where, "top"); /* longer than the setting holds: ini.c's own refusal, the corner the same */
        assert(!strcmp(g_settings.showcase_corner, "tl"));
        l.n = 0;
        vpad_showcase_draw(&l, safe, -1);
        assert(l.q[0].x == 20 && l.q[0].y == 20);
        /* pressed: A lit, the D-pad's up, the left stick's click and its knob at the ring's edge */
        vpad_showcase_input(1u << 0 | 1u << 13 | 1u << 4, right);
        l.n = 0;
        vpad_showcase_draw(&l, safe, -1);
        assert(drawn(&l, SNX_SPRITE_A_PRESSED) && !drawn(&l, SNX_SPRITE_A) && drawn(&l, SNX_SPRITE_B));
        assert((q = drawn(&l, SNX_SPRITE_DPAD_UP)) && q->turn == 0 && !drawn(&l, SNX_SPRITE_DPAD));
        ring = drawn(&l, SNX_SPRITE_STICK_RANGE);
        knob = drawn(&l, SNX_SPRITE_STICK_PRESSED);
        assert(ring && knob && drawn(&l, SNX_SPRITE_STICK)); /* the right stick's knob stays up */
        assert(fabsf(knob->x + knob->w / 2 - (ring->x + ring->w)) < 0.01f);
        assert(fabsf(knob->y + knob->h / 2 - (ring->y + ring->h / 2)) < 0.01f);
        vpad_showcase_input(1u << 14, up); /* up is positive, as the game reads it: the knob goes up the screen */
        l.n = 0;
        vpad_showcase_draw(&l, safe, -1);
        assert((q = drawn(&l, SNX_SPRITE_DPAD_UP)) && q->turn == 1);
        ring = drawn(&l, SNX_SPRITE_STICK_RANGE);
        knob = drawn(&l, SNX_SPRITE_STICK);
        assert(fabsf(knob->y + knob->h / 2 - ring->y) < 0.01f &&
               fabsf(knob->x + knob->w / 2 - (ring->x + ring->w / 2)) < 0.01f);
        vpad_showcase_input(0, corner); /* a square of keys: the knob stays on its ring */
        l.n = 0;
        vpad_showcase_draw(&l, safe, -1);
        ring = drawn(&l, SNX_SPRITE_STICK_RANGE);
        knob = drawn(&l, SNX_SPRITE_STICK);
        {
            float dx = knob->x + knob->w / 2 - (ring->x + ring->w / 2),
                  dy = knob->y + knob->h / 2 - (ring->y + ring->h / 2);
            assert(fabsf(sqrtf(dx * dx + dy * dy) - ring->w / 2) < 0.01f && dx > 0 && dy < 0);
        }
        /* the frames a second: none until half a second is over, then the presents over the last half or more */
        assert(vpad_showcase_fps(100, 1000000000ull) == -1 && vpad_showcase_fps(115, 1250000000ull) == -1);
        assert(vpad_showcase_fps(130, 1500000000ull) == 60 && vpad_showcase_fps(131, 1700000000ull) == 60);
        assert(vpad_showcase_fps(148, 2100000000ull) == 30);
        assert(vpad_showcase_fps(170, 2700000000ull) == 37); /* 22 in 0.6 s, to the nearest */
        g_settings.showcase = 0;
        ini_read(where, "br");
    }
    puts("vpad: buttons, D-pad, sticks, actions, edit mode, .ini keys, drawing and the showcase passed");
    return 0;
}
