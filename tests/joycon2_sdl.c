/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* A Joy-Con 2 as an SDL virtual gamepad (runtime/platform/joycon2_sdl.c), read back through the gamepads' code
   (backend/gamepad.c) under SDL's dummy video driver, no Bluetooth and no real controller: SDL's type for each kind, a
   single Joy-Con's buttons and stick as gamepad.c reads one held sideways (and hid.c's stick_upright turns back), the
   pair's buttons where they sit, the motion in the Switch's frame again, the mouse sensors' motion by sensor, and the
   game's vibration by side */
#include "nx/runtime/platform/joycon2_sdl.h"
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/pause.h"
#include "common/utilities/overlay.h"
#include "nx/runtime/platform/plat.h"
#include "common/utilities/common.h"
#include "support/sdl_real_pads.h"
#include <math.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char g_confdir[1024], g_exedir[1024];
char* backend_launcher_folder(void) { return snx_strdup("."); }
void logf_(int lvl, const char* fmt, ...) { (void)lvl, (void)fmt; }
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
void ini_set(const char* k, const char* v) { (void)k, (void)v; }
void overlay_toast(unsigned icon, const char* text, u64 now_ns) { (void)icon, (void)text, (void)now_ns; }
int game_pause(void) { return 0; }
void game_resume(void) {}
int game_paused(void) { return 0; }

static int checks;
#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "joycon2_sdl.c:%d: check failed: %s\n", __LINE__, #e);                                     \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_GAMEPAD_ADDED)
            gamepad_open(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
            gamepad_close(e.gdevice.which);
}
static int slot_of(int pad) {
    for (int i = 0; i < SNX_GAMEPADS; i++)
        if (gamepad_slot_id(i) == jc2_pad_id(pad)) return i;
    return -1;
}
/* hid.c's stick_upright: a single Joy-Con's stick as the host reads it, held upright */
static void upright(i32* x, i32* y, int left) {
    i32 ox = *x, oy = *y;
    *x = left ? oy : -oy;
    *y = left ? -ox : ox;
}
static int close_to(float a, float b) { return fabsf(a - b) < 0.001f; }

/* a single Joy-Con 2 of each side: its type, and what gamepad.c reads of it is what it pressed */
static void single(int kind) {
    int left = kind == JC2_PAD_LEFT, pad = jc2_pad_attach(kind), slot;
    Jc2Pad p = {0};
    u64 b;
    i32 s[4];
    CHECK(pad > 0);
    pump();
    slot = slot_of(pad);
    CHECK(slot >= 0 && jc2_pad_player(pad) == slot);
    CHECK(gamepad_slot_type(slot) == (left ? SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT
                                           : SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT));
    /* every button of its side: the Npad's own bits come back */
    p.buttons = left ? (1ull << 4 | 1ull << 6 | 1ull << 8 | 1ull << 11 | 0xfull << 12 | 3ull << 24)
                     : (0xfull | 1ull << 5 | 1ull << 7 | 1ull << 9 | 1ull << 10 | 3ull << 26);
    p.stick[0] = 12000; /* right and up */
    p.stick[1] = 20000;
    p.reports = 1;
    jc2_pad_feed(pad, left ? &p : NULL, left ? NULL : &p, NULL, NULL);
    SDL_UpdateGamepads();
    gamepad_state_slot(slot, &b, s);
    CHECK(b == p.buttons);
    upright(&s[left ? 0 : 2], &s[left ? 1 : 3], left);
    CHECK(abs(s[left ? 0 : 2] - 12000) < 2 && abs(s[left ? 1 : 3] - 20000) < 2);
    /* the motion: 1 g down its Z, a turn about its X, in the Switch's frame again */
    p.accel[2] = 1.0f;
    p.gyro[0] = 2.0f;
    p.gyro[1] = -0.5f;
    p.reports = 2;
    jc2_pad_feed(pad, left ? &p : NULL, left ? NULL : &p, NULL, NULL);
    SDL_UpdateGamepads();
    {
        PadMotion m[2];
        gamepad_motion(slot, m);
        CHECK(m[0].valid && !m[1].valid);
        CHECK(close_to(m[0].acceleration[0], 0) && close_to(m[0].acceleration[1], 0) &&
              close_to(m[0].acceleration[2], 1.0f));
        CHECK(close_to(m[0].angular_velocity[0], 2.0f) && close_to(m[0].angular_velocity[1], -0.5f) &&
              close_to(m[0].angular_velocity[2], 0));
    }
    /* the mouse: a single's one sensor is the first */
    {
        const i32 d1[2] = {5, -3}, d2[2] = {1, 1};
        float out[2][2];
        jc2_pad_feed(pad, left ? &p : NULL, left ? NULL : &p, left ? d1 : NULL, left ? NULL : d1);
        jc2_pad_feed(pad, left ? &p : NULL, left ? NULL : &p, left ? d2 : NULL, left ? NULL : d2);
        CHECK(joycon2_mouse(jc2_pad_id(pad), out));
        CHECK(out[0][0] == 6.0f && out[0][1] == -2.0f && out[1][0] == 0.0f && out[1][1] == 0.0f);
        CHECK(joycon2_mouse(jc2_pad_id(pad), out) && out[0][0] == 0.0f);
        CHECK(!joycon2_mouse(12345, out));
    }
    /* the vibration: its one actuator plays each band of the stronger side, at that side's frequency */
    {
        const float hz[4] = {100.0f, 200.0f, 150.0f, 300.0f};
        Jc2Vibration v[2];
        CHECK(joycon2_rumble(jc2_pad_id(pad), 0x8000u | 0x1000u << 16, 0x4000u | 0x2000u << 16, hz));
        jc2_pad_rumble(pad, v);
        const Jc2Vibration* own = &v[!left];
        CHECK(close_to(own->amp_low, 0x8000 / 65535.0f) && own->freq_low == 100.0f);
        CHECK(close_to(own->amp_high, 0x2000 / 65535.0f) && own->freq_high == 300.0f);
        CHECK(v[left].amp_low == 0.0f && v[left].amp_high == 0.0f);
        CHECK(!joycon2_rumble(12345, 1, 1, hz));
    }
    jc2_pad_detach(pad);
    pump();
    CHECK(!gamepad_slot_connected(slot) && !jc2_pad_id(pad));
}

/* a left and right one together: the pair's type, buttons where they sit, both sticks, both sensors */
static void pair(void) {
    int pad = jc2_pad_attach(JC2_PAD_PAIR), slot;
    Jc2Pad l = {0}, r = {0};
    u64 b;
    i32 s[4];
    CHECK(pad > 0);
    pump();
    slot = slot_of(pad);
    CHECK(slot >= 0 && gamepad_slot_type(slot) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_PAIR);
    l.buttons = 1ull << 6 | 1ull << 8 | 1ull << 11 | 1ull << 12 | 1ull << 4; /* L ZL Minus Left StickL */
    r.buttons = 1ull << 0 | 1ull << 1 | 1ull << 2 | 1ull << 3 | 1ull << 7 | 1ull << 9 | 1ull << 10; /* A B X Y R ZR + */
    l.stick[1] = 30000;
    r.stick[0] = -30000;
    l.accel[1] = 1.0f;
    r.gyro[2] = 3.0f;
    l.reports = r.reports = 1;
    jc2_pad_feed(pad, &l, &r, NULL, NULL);
    SDL_UpdateGamepads();
    gamepad_state_slot(slot, &b, s);
    CHECK(b == (l.buttons | r.buttons) && !gamepad_slot_sides(slot));
    /* each half's SL and SR reach the controller applet's dialog (as SDL's own pair's paddles), never the game */
    l.buttons |= 1ull << 24 | 1ull << 25;
    r.buttons |= 1ull << 27;
    jc2_pad_feed(pad, &l, &r, NULL, NULL);
    SDL_UpdateGamepads();
    gamepad_state_slot(slot, &b, s);
    CHECK(gamepad_slot_sides(slot) == (1ull << 24 | 1ull << 25 | 1ull << 27) && !(b >> 24 & 0xF));
    l.buttons &= ~(1ull << 24 | 1ull << 25);
    r.buttons &= ~(1ull << 27);
    jc2_pad_feed(pad, &l, &r, NULL, NULL);
    SDL_UpdateGamepads();
    CHECK(!s[0] && abs(s[1] - 30000) < 2 && abs(s[2] + 30000) < 2 && !s[3]);
    {
        PadMotion m[2];
        gamepad_motion(slot, m);
        CHECK(m[0].valid && m[1].valid && close_to(m[0].acceleration[1], 1.0f) &&
              close_to(m[1].angular_velocity[2], 3.0f));
    }
    {
        const i32 dl[2] = {1, 2}, dr[2] = {30, 40};
        float out[2][2];
        jc2_pad_feed(pad, &l, &r, dl, dr);
        CHECK(joycon2_mouse(jc2_pad_id(pad), out));
        CHECK(out[0][0] == 1.0f && out[0][1] == 2.0f && out[1][0] == 30.0f && out[1][1] == 40.0f);
        g_settings.joy_con_2_mouse_scale = 0.5f;
        jc2_pad_feed(pad, &l, &r, dl, dr);
        CHECK(joycon2_mouse(jc2_pad_id(pad), out) && out[1][0] == 15.0f && out[0][1] == 1.0f);
        g_settings.joy_con_2_mouse_scale = 1.0f;
    }
    {
        const float hz[4] = {100.0f, 200.0f, 150.0f, 300.0f};
        Jc2Vibration v[2];
        CHECK(joycon2_rumble(jc2_pad_id(pad), 0xffffu, 0xffffu << 16, hz));
        jc2_pad_rumble(pad, v);
        CHECK(close_to(v[0].amp_low, 1.0f) && v[0].amp_high == 0.0f && v[0].freq_low == 100.0f);
        CHECK(v[1].amp_low == 0.0f && close_to(v[1].amp_high, 1.0f) && v[1].freq_high == 300.0f);
    }
    /* Home and Capture as SDL's guide and misc buttons; C, GL and GR go nowhere */
    {
        u32 buttons;
        i16 axes[SDL_GAMEPAD_AXIS_COUNT];
        r.extra = JC2_X_HOME | JC2_X_C | JC2_X_GR;
        l.extra = JC2_X_CAPTURE | JC2_X_GL;
        jc2_sdl_map(JC2_PAD_PAIR, &l, &r, &buttons, axes);
        CHECK(buttons >> SDL_GAMEPAD_BUTTON_GUIDE & 1 && buttons >> SDL_GAMEPAD_BUTTON_MISC1 & 1);
        CHECK(axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] == 32767 && axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] == 32767);
        r.extra = l.extra = 0;
        /* released triggers rest at the raw axis's minimum, which SDL reads as a trigger at 0, not half pressed */
        {
            Jc2Pad none_l = l, none_r = r;
            none_l.buttons = none_r.buttons = 0;
            jc2_sdl_map(JC2_PAD_PAIR, &none_l, &none_r, &buttons, axes);
            CHECK(axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] == -32768 && axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] == -32768);
        }
    }
    jc2_pad_detach(pad);
    pump();
}

/* a Pro Controller 2: a Pro Controller to SDL and to gamepad.c (a full controller, named Pro Controller 2), every
   button where a pair has it, both sticks of its own, ZL and ZR as triggers, one motion sensor, the game's two
   motors for its two actuators */
static void pro(void) {
    int pad = jc2_pad_attach(JC2_PAD_PRO), slot;
    Jc2Pad p = {0};
    u64 b;
    i32 s[4];
    CHECK(pad > 0);
    pump();
    slot = slot_of(pad);
    CHECK(slot >= 0 && gamepad_slot_type(slot) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_PRO);
    CHECK(!strcmp(SDL_GetGamepadNameForID(jc2_pad_id(pad)), "Pro Controller 2"));
    p.buttons = 0xfull | 1ull << 6 | 1ull << 7 | 1ull << 8 | 1ull << 9 | 1ull << 10 | 1ull << 11 | 0xfull << 12;
    p.stick[0] = 12000;
    p.stick[1] = 20000;
    p.rstick[0] = -15000;
    p.rstick[1] = 9000;
    p.accel[2] = 1.0f;
    p.reports = 1;
    jc2_pad_feed(pad, &p, NULL, NULL, NULL);
    SDL_UpdateGamepads();
    gamepad_state_slot(slot, &b, s);
    CHECK(b == p.buttons && !gamepad_slot_sides(slot));
    CHECK(abs(s[0] - 12000) < 2 && abs(s[1] - 20000) < 2 && abs(s[2] + 15000) < 2 && abs(s[3] - 9000) < 2);
    {
        PadMotion m[2];
        gamepad_motion(slot, m);
        CHECK(m[0].valid && close_to(m[0].acceleration[2], 1.0f));
    }
    CHECK(joycon2_surface(jc2_pad_id(pad)) == 0); /* no mouse sensor */
    jc2_pad_detach(pad);
    pump();
}

/* which controllers make which gamepad: a left and a right that connect together are a pair at once, a lone one waits
   3 s for its partner, one that comes later joins it, the half that stays after the other left is a single at once */
static void join(void) {
    Jc2Join d[3] = {{0}};
    int a = -1, b = -1;
    const u64 s = 1000000000ull;
    d[0] = (Jc2Join){1, JC2_SIDE_LEFT, 10 * s, 0, 0, 0};
    CHECK(jc2_join_next(d, 3, 1, 10 * s, &a, &b) == JC2_JOIN_NONE);
    CHECK(jc2_join_next(d, 3, 1, 12 * s, &a, &b) == JC2_JOIN_NONE);
    CHECK(jc2_join_next(d, 3, 0, 10 * s, &a, &b) == JC2_JOIN_SINGLE && a == 0); /* pairing off: at once */
    CHECK(jc2_join_next(d, 3, 1, 13 * s, &a, &b) == JC2_JOIN_SINGLE && a == 0);
    d[0].gamepad = 1, d[0].kind = JC2_PAD_LEFT, d[0].had = 1;
    CHECK(jc2_join_next(d, 3, 1, 14 * s, &a, &b) == JC2_JOIN_NONE);
    /* the right one comes: the two are a pair, the left one's single goes */
    d[2] = (Jc2Join){1, JC2_SIDE_RIGHT, 20 * s, 0, 0, 0};
    CHECK(jc2_join_next(d, 3, 1, 20 * s, &a, &b) == JC2_JOIN_PAIR && a == 0 && b == 2);
    d[0].gamepad = d[2].gamepad = 2, d[0].kind = d[2].kind = JC2_PAD_PAIR, d[2].had = 1;
    CHECK(jc2_join_next(d, 3, 1, 21 * s, &a, &b) == JC2_JOIN_NONE);
    /* a second left one: it waits, then is a single; the pair stays */
    d[1] = (Jc2Join){1, JC2_SIDE_LEFT, 22 * s, 0, 0, 0};
    CHECK(jc2_join_next(d, 3, 1, 23 * s, &a, &b) == JC2_JOIN_NONE);
    CHECK(jc2_join_next(d, 3, 1, 25 * s, &a, &b) == JC2_JOIN_SINGLE && a == 1);
    d[1].gamepad = 3, d[1].kind = JC2_PAD_LEFT, d[1].had = 1;
    /* the pair's left leaves: its right one is a single at once, then pairs with the other left one */
    d[0] = (Jc2Join){0};
    d[2].gamepad = 0;
    CHECK(jc2_join_next(d, 3, 1, 26 * s, &a, &b) == JC2_JOIN_PAIR && a == 1 && b == 2);
    CHECK(jc2_join_next(d, 3, 0, 26 * s, &a, &b) == JC2_JOIN_SINGLE && a == 2); /* pairing off */
    /* two that set up together: a pair at once */
    Jc2Join two[2] = {{1, JC2_SIDE_RIGHT, 5 * s, 0, 0, 0}, {1, JC2_SIDE_LEFT, 5 * s, 0, 0, 0}};
    CHECK(jc2_join_next(two, 2, 1, 5 * s, &a, &b) == JC2_JOIN_PAIR && a == 1 && b == 0);
    /* a Pro Controller 2 waits for no partner, and never pairs */
    Jc2Join pro_one[2] = {{1, JC2_SIDE_PRO, 5 * s, 0, 0, 0}, {1, JC2_SIDE_LEFT, 5 * s, 0, 0, 0}};
    CHECK(jc2_join_next(pro_one, 2, 1, 5 * s, &a, &b) == JC2_JOIN_SINGLE && a == 0);
}

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    sdl_real_pads_hide();
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    CHECK(gamepad_ini("gamepad_assign", "0"));
    g_settings.joy_con_2_mouse_scale = 1.0f;
    g_settings.stick_dead_zone = 0; /* the sticks as they were fed */
    join();
    single(JC2_PAD_LEFT);
    single(JC2_PAD_RIGHT);
    pair();
    pro();
    SDL_Quit();
    printf("joycon2_sdl: %d checks\n", checks);
    return 0;
}
