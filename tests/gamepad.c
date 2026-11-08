/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The gamepads (runtime/platform/backend/gamepad.c) on SDL's virtual ones, under its dummy video driver: the launcher's
   SDL mappings, per-slot and legacy combined input, common layouts, focus behavior, motion sensors, and a waiting pad
   taking a freed slot */
#include "nx/runtime/platform/gamepad.h"
#include "nx/runtime/platform/backend.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/pause.h"
#include "common/utilities/common.h"
#include "common/utilities/pad_sdl.h" /* snx_pad_slot */
#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>

char g_confdir[1024], g_exedir[1024];
/* sdl3.c's, as on the desktop: the launcher's files beside the program */
char* backend_launcher_folder(void) { return snx_strdup(g_exedir[0] ? g_exedir : "."); }
/* the last warning the backend logged */
static char warned[512];
void logf_(int lvl, const char* fmt, ...) {
    va_list ap;
    if (lvl) return;
    va_start(ap, fmt);
    vsnprintf(warned, sizeof warned, fmt, ap);
    va_end(ap);
}
void pm_lock(PMutex* m) { (void)m; }
void pm_unlock(PMutex* m) { (void)m; }
/* the game's .ini: the last setting stored */
static char stored[64];
void ini_set(const char* k, const char* v) { snprintf(stored, sizeof stored, "%s=%s", k, v); }
/* gamepad_assign=1's notices and pauses */
static char toasted[128];
void overlay_toast(const char* text, u64 now_ns) {
    (void)now_ns;
    snprintf(toasted, sizeof toasted, "%s", text);
}
static int paused;
int game_pause(void) {
    ++paused;
    return 0;
}
void game_resume(void) { --paused; }
int game_paused(void) { return paused > 0; }

static int checks;
#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "gamepad.c:%d: check failed: %s\n", __LINE__, #e);                                         \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

/* HidNpadButton bits */
enum { A = 1, B = 2, X = 4, Y = 8, ZL = 1 << 8, ZR = 1 << 9, PLUS = 1 << 10, LEFT = 1 << 12, UP = 1 << 13 };
/* the virtual pads, by the vendor and product SDL tells their layout from: only they are read, a real pad plugged in
   stays out */
#define XBOX      0x045e, 0x02ea
#define SWITCH    0x057e, 0x2009
#define SONY      0x054c, 0x09cc
#define GAMECUBE  0x057e, 0x0337
#define JOY_LEFT  0x057e, 0x2006
#define JOY_RIGHT 0x057e, 0x2007
#define PADS_IDS  "0x045e/0x02ea,0x057e/0x2009,0x054c/0x09cc,0x057e/0x0337,0x057e/0x2006,0x057e/0x2007"

/* the backend's event loop (sdl3.c's video_poll), for the gamepads */
static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e))
        if (e.type == SDL_EVENT_GAMEPAD_ADDED)
            gamepad_open(e.gdevice.which);
        else if (e.type == SDL_EVENT_GAMEPAD_REMOVED)
            gamepad_close(e.gdevice.which);
}
/* what each pad's motors were last told: low (the heavy one) | high << 16 */
static Uint32 motors[32];
static int pads;
static bool SDLCALL rumble(void* pad, Uint16 low, Uint16 high) {
    *(Uint32*)pad = low | (Uint32)high << 16;
    return true;
}
static bool SDLCALL sensors_enabled(void* pad, bool enabled) {
    (void)pad;
    (void)enabled;
    return true;
}
/* a pad with every gamepad button and axis (button i is SDL_GamepadButton i) and two motors, at rest */
static SDL_Joystick* attach(Uint16 vendor, Uint16 product) {
    SDL_VirtualJoystickDesc d;
    SDL_Joystick* j;
    int a;
    SDL_INIT_INTERFACE(&d);
    d.Rumble = rumble;
    CHECK(pads < (int)(sizeof motors / sizeof motors[0])); /* a motor each, for every pad the test attaches */
    d.userdata = &motors[pads++];
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.vendor_id = vendor;
    d.product_id = product;
    j = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&d));
    CHECK(j != NULL);
    /* a real pad reports its axes at rest as it opens, a virtual one when they are first set; a trigger rests at the
       bottom of the range */
    for (a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++)
        SDL_SetJoystickVirtualAxis(j, a, a >= SDL_GAMEPAD_AXIS_LEFT_TRIGGER ? SDL_JOYSTICK_AXIS_MIN : 0);
    pump();
    return j;
}
static SDL_Joystick* attach_motion(void) {
    static const SDL_VirtualJoystickSensorDesc sensors[6] = {{SDL_SENSOR_ACCEL, 60.0f},   {SDL_SENSOR_GYRO, 60.0f},
                                                             {SDL_SENSOR_ACCEL_L, 60.0f}, {SDL_SENSOR_GYRO_L, 60.0f},
                                                             {SDL_SENSOR_ACCEL_R, 60.0f}, {SDL_SENSOR_GYRO_R, 60.0f}};
    SDL_VirtualJoystickDesc d;
    SDL_Joystick* j;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.nsensors = 6;
    d.sensors = sensors;
    d.SetSensorsEnabled = sensors_enabled;
    d.vendor_id = 0x057e;
    d.product_id = 0x2009;
    j = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&d));
    CHECK(j != NULL);
    pump();
    return j;
}
static u64 buttons(void) {
    u64 b;
    i32 s[4];
    pump();
    gamepad_state(&b, s);
    return b;
}
/* what the game gets with one button held */
static u64 with(SDL_Joystick* j, SDL_GamepadButton button) {
    u64 b;
    SDL_SetJoystickVirtualButton(j, button, true);
    b = buttons();
    SDL_SetJoystickVirtualButton(j, button, false);
    CHECK(buttons() == 0);
    return b;
}
static void sticks(SDL_Joystick* j, Sint16 lx, Sint16 ly, i32 s[4]) {
    u64 b;
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTX, lx);
    SDL_SetJoystickVirtualAxis(j, SDL_GAMEPAD_AXIS_LEFTY, ly);
    pump();
    gamepad_state(&b, s);
}

int main(void) {
    SDL_Joystick *xbox, *pro, *sony, *cube, *fifth, *motion;
    SDL_Window* w;
    i32 s[4];
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT, PADS_IDS);
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    w = SDL_CreateWindow("gamepad", 64, 64, 0);
    CHECK(w != NULL && SDL_GetKeyboardFocus() == w);

    /* the SDL mappings of the launcher whose games/ the game is in, beside its settings.ini: a portable copy's
       config/gamecontrollerdb.txt. SDL takes a line of a file only with its own platform */
    {
        char line[160];
        char *root = snx_temp_dir(NULL), *copy = snx_path_join(root, ".scarletnx");
        char *config = snx_path_join(copy, "config"), *game = snx_path_join(copy, "games/0100ABCD12345000");
        char *db = snx_path_join(config, "gamecontrollerdb.txt"), *mapping;
        snprintf(line, sizeof line, "03000000341200007856000000000000,Odd Stick,a:b0,b:b1,x:b2,y:b3,platform:%s,\n",
                 SDL_GetPlatform());
        CHECK(root && !snx_mkdirs(config) && !snx_mkdirs(game) && !snx_write_file(db, line, strlen(line)));
        snprintf(g_exedir, sizeof g_exedir, "%s/", game);
        gamepad_scan();
        mapping = SDL_GetGamepadMappingForGUID(SDL_StringToGUID("03000000341200007856000000000000"));
        CHECK(mapping && strstr(mapping, "Odd Stick"));
        SDL_free(mapping);
        g_exedir[0] = 0;
        CHECK(!snx_remove_tree(root));
        free(db);
        free(game);
        free(config);
        free(copy);
        free(root);
    }

    /* the face buttons where the console's are, on every pad: A right, B bottom, X top, Y left */
    xbox = attach(XBOX);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_WEST) == Y);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_NORTH) == X);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_START) == PLUS);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_DPAD_LEFT) == LEFT);
    /* a trigger is ZL or ZR past a quarter of its travel */
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0);
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    CHECK(buttons() == (ZL | ZR));
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    CHECK(buttons() == 0);
    /* the window without the focus (the dummy driver's hidden one has none): SDL drops the pad's presses, unless
       gamepad_background=1 */
    SDL_HideWindow(w);
    CHECK(SDL_GetKeyboardFocus() == NULL);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == 0);
    CHECK(gamepad_ini("gamepad_background", "1"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, 0, s);
    CHECK(s[0] == 32767);
    sticks(xbox, 0, 0, s);
    CHECK(gamepad_ini("gamepad_background", "0") && !gamepad_ini("gamepad", "0"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == 0);
    SDL_ShowWindow(w);

    /* the sticks: up is up, a stick inside the dead zone (20%) is still, and the travel past it spans the range */
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, 0, s);
    CHECK(s[0] == 32767 && s[1] == 0 && s[2] == 0 && s[3] == 0);
    sticks(xbox, 0, SDL_JOYSTICK_AXIS_MIN, s);
    CHECK(s[0] == 0 && s[1] == 32767);
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, SDL_JOYSTICK_AXIS_MIN, s); /* a square gate's corner, on the edge */
    CHECK(s[0] == s[1] && s[0] > 23160 && s[0] < 23180);
    sticks(xbox, 3000, -3000, s);
    CHECK(s[0] == 0 && s[1] == 0);
    sticks(xbox, 16384, 0, s);
    CHECK(s[0] > 12280 && s[0] < 12295 && s[1] == 0); /* (50% - 20%) / 80% */
    CHECK(gamepad_ini("gamepad_deadzone", "0"));
    sticks(xbox, 3000, -3000, s);
    CHECK(s[0] == 3000 && s[1] == 3000);
    CHECK(gamepad_ini("gamepad_deadzone", "50"));
    sticks(xbox, 16000, 0, s);
    CHECK(s[0] == 0);
    sticks(xbox, SDL_JOYSTICK_AXIS_MAX, 0, s);
    CHECK(s[0] == 32767);
    CHECK(gamepad_ini("gamepad_deadzone", "20"));
    sticks(xbox, 0, 0, s);

    /* every pad the same, by where its buttons sit: the console's own, a PlayStation pad's, the older console's */
    pro = attach(SWITCH);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_NORTH) == X);
    CHECK(with(pro, SDL_GAMEPAD_BUTTON_WEST) == Y);
    sony = attach(SONY);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_WEST) == Y);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_NORTH) == X);
    cube = attach(GAMECUBE);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_WEST) == Y);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_NORTH) == X);
    /* the game's menu reads them the same: A selects, B goes back */
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(gamepad_button(SDL_GetJoystickID(pro), SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_DPAD_UP) == UP);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_GUIDE) == 0);

    /* gamepad_swap_ab=1: A and B swapped, X and Y where the console's are; a key that is none of these is no gamepad
       key, and gamepad_buttons is one no longer (docs/user/deprecations.md) */
    CHECK(gamepad_ini("gamepad_swap_ab", "1"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == A);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == B);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_NORTH) == X);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_WEST) == Y);
    CHECK(with(cube, SDL_GAMEPAD_BUTTON_EAST) == B);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_SOUTH) == A);
    CHECK(gamepad_ini("gamepad_swap_ab", "0") && !gamepad_ini("gamepad", "1") && !gamepad_ini("gamepad_buttons", "1"));

    /* gamepad_map: the player's map, each control what it names, the others where the console's are (a PlayStation
       pad's too), a trigger as any button; held controls are told apart from what they send; A and B swapped over it;
       a map it cannot read leaves the one before; an empty one is none */
    CHECK(gamepad_ini("gamepad_map", "south:A east:B lt:R start:None"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == A);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == B);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_WEST) == Y);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_SOUTH) == A);
    CHECK(with(sony, SDL_GAMEPAD_BUTTON_NORTH) == X);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_START) == 0);
    CHECK(gamepad_button(SDL_GetJoystickID(xbox), SDL_GAMEPAD_BUTTON_EAST) == B);
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MAX);
    CHECK(buttons() == (1ull << 7) && (gamepad_held(-1) & 1u << SNX_PAD_LT)); /* R */
    /* the Xbox pad's, not the next one's */
    CHECK(gamepad_held(0) == 1u << SNX_PAD_LT && !gamepad_held(1));
    SDL_SetJoystickVirtualAxis(xbox, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_JOYSTICK_AXIS_MIN);
    CHECK(buttons() == 0 && !gamepad_held(-1) && !gamepad_held(0));
    CHECK(gamepad_ini("gamepad_swap_ab", "1"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == B);
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(gamepad_ini("gamepad_swap_ab", "0"));
    CHECK(gamepad_ini("gamepad_map", "south:Q"));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == A);
    /* said with the map that stays */
    CHECK(!strcmp(warned, "settings: gamepad_map's \"south:Q\" is not a control and the button it sends (south:B): "
                          "it keeps \"south:A east:B lt:R start:None\"\n"));
    CHECK(gamepad_ini("gamepad_map", ""));
    CHECK(with(xbox, SDL_GAMEPAD_BUTTON_SOUTH) == B);

    /* the game's vibration on every pad: each motor the stronger of the left and right devices' bands, as strong as
       gamepad_rumble says */
    gamepad_rumble(0x4000u | 0x1000u << 16, 0x2000u | 0x8000u << 16);
    CHECK(motors[0] == (0x4000u | 0x8000u << 16) && motors[1] == motors[0] && motors[3] == motors[0]);
    CHECK(gamepad_ini("gamepad_rumble", "50"));
    gamepad_rumble(0x4000u | 0x1000u << 16, 0x2000u | 0x8000u << 16);
    CHECK(motors[0] == (0x2000u | 0x4000u << 16));
    CHECK(gamepad_ini("gamepad_rumble", "0")); /* never: a pad that shakes stops */
    gamepad_rumble(0xFFFFu | 0xFFFFu << 16, 0);
    CHECK(motors[0] == 0 && motors[2] == 0);
    CHECK(gamepad_ini("gamepad_rumble", "100"));
    gamepad_rumble(0xFFFFu, 0);
    CHECK(motors[0] == 0xFFFFu);
    gamepad_rumble(0, 0); /* the game stops it */
    CHECK(motors[0] == 0);

    /* the legacy combined state merges every pad, and the first stick that is pushed wins */
    SDL_SetJoystickVirtualButton(xbox, SDL_GAMEPAD_BUTTON_SOUTH, true);
    SDL_SetJoystickVirtualButton(pro, SDL_GAMEPAD_BUTTON_EAST, true);
    CHECK(buttons() == (A | B));
    SDL_SetJoystickVirtualButton(xbox, SDL_GAMEPAD_BUTTON_SOUTH, false);
    SDL_SetJoystickVirtualButton(pro, SDL_GAMEPAD_BUTTON_EAST, false);
    sticks(pro, SDL_JOYSTICK_AXIS_MIN, 0, s);
    CHECK(s[0] <= -32767 && s[1] == 0);
    sticks(pro, 0, 0, s);

    /* each open pad gets its own slot, and the game's menu names them */
    fifth = attach(SWITCH);
    CHECK(with(fifth, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(gamepad_name(4) && !strcmp(gamepad_name(4), SDL_GetJoystickName(fifth)) && !gamepad_name(5));
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(sony));
    pump();
    CHECK(with(fifth, SDL_GAMEPAD_BUTTON_EAST) == A);
    CHECK(!strcmp(gamepad_name(0), SDL_GetJoystickName(xbox)) && !strcmp(gamepad_name(3), SDL_GetJoystickName(fifth)));
    /* the player a pad takes (snx_pad_slot, by which the launcher's button map finds a player's too): the one that names
       its GUID (any case, /2 for the second alike), else the first free one naming none (NULL or "") */
    {
        const char* players[4] = {NULL, "abc", "", "abc/2"};
        CHECK(snx_pad_slot("abc", 0, players, 4, 0) == 1 && snx_pad_slot("ABC", 0, players, 4, 0) == 1);
        CHECK(snx_pad_slot("abc", 1, players, 4, 1u << 1) == 3);
        CHECK(snx_pad_slot("def", 0, players, 4, 0) == 0 && snx_pad_slot("def", 0, players, 4, 1u << 0) == 2);
        CHECK(snx_pad_slot("def", 0, players, 4, 1u << 0 | 1u << 2) == -1);
        CHECK(snx_pad_slot("abc", 0, players, 4, 1u << 1) == 0); /* its player has another: the next naming none */
    }
    /* a player's pad by its slot, as the menu's button map draws it: none where one left */
    CHECK(gamepad_of_player(0) == 0 && gamepad_of_player(2) == -1 && gamepad_of_player(4) == 3);
    CHECK(gamepad_of_player(5) == -1 && gamepad_of_player(-1) == -1 && gamepad_of_player(SNX_GAMEPADS) == -1);
    SDL_SetJoystickVirtualButton(fifth, SDL_GAMEPAD_BUTTON_EAST, true);
    CHECK(buttons() == A && gamepad_held(3) == 1u << SNX_PAD_EAST && !gamepad_held(0));
    SDL_SetJoystickVirtualButton(fifth, SDL_GAMEPAD_BUTTON_EAST, false);
    CHECK(buttons() == 0 && !gamepad_held(3));
    SDL_SetJoystickVirtualButton(pro, SDL_GAMEPAD_BUTTON_EAST, true); /* held as it goes: nothing held after */
    CHECK(buttons() == A && gamepad_held(-1) == 1u << SNX_PAD_EAST);
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pro));
    pump();
    CHECK(!strcmp(gamepad_name(2), SDL_GetJoystickName(fifth)) && !gamepad_name(3));
    CHECK(buttons() == 0 && !gamepad_held(-1));

    /* SDL's left and right Joy-Con sensors take precedence over a primary sensor. The guest receives g and rad/s. */
    motion = attach_motion();
    CHECK(gamepad_slot_connected(1));
    {
        const float accel_left[3] = {9.80665f, 19.6133f, 29.41995f};
        const float gyro_left[3] = {1.0f, 2.0f, 3.0f};
        const float accel_right[3] = {-9.80665f, 0.0f, 9.80665f};
        const float gyro_right[3] = {-1.0f, 0.0f, 2.0f};
        PadMotion sample[2];
        Uint64 now = SDL_GetTicksNS();
        CHECK(SDL_SendJoystickVirtualSensorData(motion, SDL_SENSOR_ACCEL_L, now, accel_left, 3));
        CHECK(SDL_SendJoystickVirtualSensorData(motion, SDL_SENSOR_GYRO_L, now, gyro_left, 3));
        CHECK(SDL_SendJoystickVirtualSensorData(motion, SDL_SENSOR_ACCEL_R, now, accel_right, 3));
        CHECK(SDL_SendJoystickVirtualSensorData(motion, SDL_SENSOR_GYRO_R, now, gyro_right, 3));
        SDL_UpdateGamepads();
        gamepad_motion(1, sample);
        CHECK(sample[0].valid && sample[1].valid);
        CHECK(fabsf(sample[0].acceleration[0] + 1.0f) < 0.001f && fabsf(sample[0].acceleration[1] - 3.0f) < 0.001f &&
              fabsf(sample[0].acceleration[2] + 2.0f) < 0.001f);
        CHECK(fabsf(sample[0].angular_velocity[0] - 1.0f) < 0.001f &&
              fabsf(sample[0].angular_velocity[1] + 3.0f) < 0.001f &&
              fabsf(sample[0].angular_velocity[2] - 2.0f) < 0.001f);
        CHECK(fabsf(sample[1].acceleration[0] - 1.0f) < 0.001f && fabsf(sample[1].acceleration[1] - 1.0f) < 0.001f &&
              sample[1].acceleration[2] == 0.0f);
        gamepad_motion(0, sample);
        CHECK(!sample[0].valid && !sample[1].valid);
    }
    {
        SDL_Joystick* left = attach(JOY_LEFT);
        SDL_Joystick* right = attach(JOY_RIGHT);
        SDL_JoystickID left_id = SDL_GetJoystickID(left), right_id = SDL_GetJoystickID(right);
        CHECK(gamepad_slot_type(2) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_LEFT);
        CHECK(gamepad_slot_type(5) == SDL_GAMEPAD_TYPE_NINTENDO_SWITCH_JOYCON_RIGHT);
        CHECK(gamepad_button(left_id, SDL_GAMEPAD_BUTTON_LEFT_STICK) == (1ull << 4));
        CHECK(gamepad_button(left_id, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1) == (1ull << 6));
        CHECK(gamepad_button(left_id, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2) == (1ull << 8));
        CHECK(gamepad_button(right_id, SDL_GAMEPAD_BUTTON_LEFT_STICK) == (1ull << 5));
        CHECK(gamepad_button(right_id, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1) == (1ull << 7));
        CHECK(gamepad_button(right_id, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2) == (1ull << 9));
        SDL_DetachVirtualJoystick(left_id);
        SDL_DetachVirtualJoystick(right_id);
        pump();
    }

    /* the game's menu changes a setting and keeps it */
    g_settings.stick_dead_zone = 35;
    gamepad_store(&g_settings.stick_dead_zone);
    CHECK(!strcmp(stored, "gamepad_deadzone=35"));
    g_settings.swap_a_b = 1;
    gamepad_store(&g_settings.swap_a_b);
    CHECK(!strcmp(stored, "gamepad_swap_ab=1"));
    g_settings.swap_a_b = 0;
    gamepad_store(&g_settings.swap_a_b);
    g_settings.background_input = 1; /* applied at once */
    gamepad_store(&g_settings.background_input);
    CHECK(!strcmp(stored, "gamepad_background=1") &&
          SDL_GetHintBoolean(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, false));

    /* All eight player slots can be occupied; a ninth pad waits for one to disconnect. */
    {
        SDL_Joystick* extra[5];
        SDL_JoystickID old_id = SDL_GetJoystickID(motion);
        int i;
        for (i = 0; i < 5; i++) extra[i] = attach(SWITCH);
        CHECK(gamepad_slot_connected(0) && gamepad_slot_connected(7));
        CHECK(gamepad_slot_id(1) == old_id);
        CHECK(with(extra[4], SDL_GAMEPAD_BUTTON_SOUTH) == 0);
        SDL_DetachVirtualJoystick(SDL_GetJoystickID(motion));
        pump();
        CHECK(gamepad_slot_id(1) == SDL_GetJoystickID(extra[4]) && gamepad_slot_id(1) != old_id);
        CHECK(with(extra[4], SDL_GAMEPAD_BUTTON_SOUTH) == B);
    }

    /* each player its own, as Eden has them: A and B, the map, the vibration, seen or not and the kind the game sees,
       each over every player's; and the controller it names, which it takes whenever it comes */
    {
        SDL_Joystick* first = SDL_GetJoystickFromID(gamepad_slot_id(0));
        SDL_Joystick* second = SDL_GetJoystickFromID(gamepad_slot_id(1));
        SDL_Joystick *named, *twin;
        char guid[40];
        CHECK(first && second);
        CHECK(gamepad_ini("gamepad_p1_swap_ab", "1"));
        CHECK(with(first, SDL_GAMEPAD_BUTTON_SOUTH) == A && with(second, SDL_GAMEPAD_BUTTON_SOUTH) == B);
        CHECK(gamepad_ini("gamepad_swap_ab", "1") && with(second, SDL_GAMEPAD_BUTTON_SOUTH) == A); /* every player's */
        CHECK(gamepad_ini("gamepad_p2_swap_ab", "0") && with(second, SDL_GAMEPAD_BUTTON_SOUTH) == B); /* its own */
        CHECK(gamepad_ini("gamepad_swap_ab", "0") && gamepad_ini("gamepad_p1_swap_ab", "-1") &&
              gamepad_ini("gamepad_p2_swap_ab", "-1"));
        CHECK(gamepad_ini("gamepad_p2_map", "south:X"));
        CHECK(with(second, SDL_GAMEPAD_BUTTON_SOUTH) == X && with(first, SDL_GAMEPAD_BUTTON_SOUTH) == B);
        CHECK(gamepad_ini("gamepad_p2_map", "south:Q") && with(second, SDL_GAMEPAD_BUTTON_SOUTH) == X); /* kept */
        CHECK(gamepad_ini("gamepad_p2_map", "") && with(second, SDL_GAMEPAD_BUTTON_SOUTH) == B); /* none: all's */
        CHECK(gamepad_ini("gamepad_p1_rumble", "0"));
        gamepad_rumble_slot(0, 0xFFFFu, 0);
        CHECK(motors[0] == 0);
        CHECK(gamepad_ini("gamepad_p1_rumble", "-10"));
        gamepad_rumble_slot(0, 0xFFFFu, 0);
        CHECK(motors[0] == 0xFFFFu);
        CHECK(gamepad_player_seen(1) && gamepad_ini("gamepad_p2_connected", "0") && !gamepad_player_seen(1));
        CHECK(gamepad_ini("gamepad_p1_connected", "0") && gamepad_player_seen(0)); /* player 1 always */
        CHECK(gamepad_player_kind(1, SNX_PAD_FULL_KEY) == SNX_PAD_FULL_KEY);
        CHECK(gamepad_ini("gamepad_p2_type", "3") && gamepad_player_kind(1, SNX_PAD_FULL_KEY) == SNX_PAD_JOY_LEFT);
        CHECK(gamepad_ini("gamepad_p1_connected", "1") && gamepad_ini("gamepad_p2_connected", "1") &&
              gamepad_ini("gamepad_p2_type", "0"));
        /* players 1 and 3 free; player 3 names a PlayStation pad's GUID: one plugged in takes it, not 1; a second
           alike (GUID/2) takes the first free player that names none */
        SDL_DetachVirtualJoystick(gamepad_slot_id(0));
        SDL_DetachVirtualJoystick(gamepad_slot_id(2));
        pump();
        CHECK(!gamepad_slot_connected(0) && !gamepad_slot_connected(2));
        named = attach(SONY);
        CHECK(gamepad_slot_id(0) == SDL_GetJoystickID(named)); /* nobody names it yet: player 1, the first free */
        SDL_GUIDToString(SDL_GetJoystickGUID(named), guid, sizeof guid);
        SDL_DetachVirtualJoystick(SDL_GetJoystickID(named));
        pump();
        CHECK(gamepad_ini("gamepad_p3_device", guid));
        named = attach(SONY);
        CHECK(gamepad_slot_id(2) == SDL_GetJoystickID(named) && !gamepad_slot_connected(0));
        twin = attach(SONY);
        CHECK(gamepad_slot_id(0) == SDL_GetJoystickID(twin));
        CHECK(gamepad_ini("gamepad_p3_device", ""));
    }

    /* gamepad_assign=1, as a console's: the controllers by the order they come, a player kept for one that left and
       taken back, another going on past it, the pause once one has been gone 3 s, and the notices */
    {
        SDL_Joystick *one, *two, *three;
        u64 t;
        int i;
        for (i = 0; i < SNX_GAMEPADS; i++)
            if (gamepad_slot_id(i)) SDL_DetachVirtualJoystick(gamepad_slot_id(i));
        pump();
        CHECK(gamepad_ini("gamepad_assign", "1") && g_settings.controller_assignment == 1);
        gamepad_console(0, SDL_GetTicksNS()); /* the mode's start: none held */
        one = attach(XBOX);
        CHECK(gamepad_slot_id(0) == SDL_GetJoystickID(one) && strstr(toasted, "P1: ") && strstr(toasted, " joined"));
        two = attach(SONY);
        CHECK(gamepad_slot_id(1) == SDL_GetJoystickID(two));
        SDL_DetachVirtualJoystick(SDL_GetJoystickID(two));
        pump();
        t = SDL_GetTicksNS();
        CHECK(!gamepad_slot_connected(1) && gamepad_slot_kept(1) && strstr(toasted, "P2: ") &&
              strstr(toasted, " left"));
        CHECK(gamepad_slot_kept_type(1) == SDL_GAMEPAD_TYPE_PS4);
        three = attach(SWITCH); /* another: past the kept player */
        CHECK(gamepad_slot_id(2) == SDL_GetJoystickID(three) && !gamepad_slot_connected(1));
        gamepad_console(0, t + 2000000000ull);
        CHECK(!paused);
        gamepad_console(0, t + 3200000000ull);
        CHECK(paused == 1 && !strcmp(toasted, "P2 disconnected: reconnect, or press + on another controller to go on"));
        two = attach(SONY); /* back: its player, and the game goes on */
        CHECK(gamepad_slot_id(1) == SDL_GetJoystickID(two));
        gamepad_console(0, t + 3300000000ull);
        CHECK(!paused);
        /* gone again: + held and let go on another goes on without it, its player no longer kept */
        SDL_DetachVirtualJoystick(SDL_GetJoystickID(two));
        pump();
        t = SDL_GetTicksNS();
        gamepad_console(0, t + 3200000000ull);
        CHECK(paused == 1);
        gamepad_console(1, t + 3300000000ull);
        CHECK(paused == 1);
        gamepad_console(0, t + 3400000000ull);
        CHECK(!paused && !strcmp(toasted, "Going on without P2") && !gamepad_slot_kept(1));
        /* manual again: what was kept goes, the pads stay where they are */
        CHECK(gamepad_ini("gamepad_assign", "0"));
        gamepad_console(0, t + 3500000000ull);
        CHECK(gamepad_slot_id(0) == SDL_GetJoystickID(one) && gamepad_slot_id(2) == SDL_GetJoystickID(three));
        SDL_DetachVirtualJoystick(SDL_GetJoystickID(one));
        pump();
        CHECK(!gamepad_slot_kept(0));
        SDL_DetachVirtualJoystick(SDL_GetJoystickID(three));
        pump();
    }

    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("gamepad: %d checks\n", checks);
    return 0;
}
