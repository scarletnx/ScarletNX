/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Joy-Con 2 as SDL virtual gamepads (joycon2_sdl.h) */
#include "nx/runtime/platform/joycon2_sdl.h"
#include "nx/runtime/platform/gamepad.h"
#include "snx_settings.h"
#include <stdio.h>
#include <string.h>

typedef struct {
    SDL_Joystick* stick; /* NULL: the slot is free */
    int kind;
    u32 reports[2];         /* the left and right pads' reports when their motion last went to SDL */
    float mouse[2][2];      /* the sensors' motion not yet taken, by sensor, X and Y */
    Jc2Vibration rumble[2]; /* the game's, for the left (or only) Joy-Con and the right one */
    char who[40];           /* who it is for connect.h's players (jc2_pad_who), "" none */
    u8 surface;             /* the sensors on a surface, a bit each in PadMotion's order */
    /* the reports per second of its right (or only) Joy-Con, counted over a second: from that count at that time */
    u32 rate_from;
    u64 rate_ns;
    int rate;
} Jc2Gamepad;
static struct {
    Jc2Gamepad pad[JC2_PADS];
} g_joycon2_sdl;

/* an Npad button bit and the SDL button it is pressed as */
typedef struct {
    u8 bit, button;
} Jc2Button;
/* a single Joy-Con held sideways: gamepad.c's joycon_button_bit turned around */
static const Jc2Button SINGLE_LEFT[] = {
    {12, SDL_GAMEPAD_BUTTON_SOUTH},         {15, SDL_GAMEPAD_BUTTON_EAST},          {13, SDL_GAMEPAD_BUTTON_WEST},
    {14, SDL_GAMEPAD_BUTTON_NORTH},         {11, SDL_GAMEPAD_BUTTON_START},         {4, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {24, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {25, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {6, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    {8, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2}};
static const Jc2Button SINGLE_RIGHT[] = {
    {0, SDL_GAMEPAD_BUTTON_SOUTH},          {2, SDL_GAMEPAD_BUTTON_EAST},           {1, SDL_GAMEPAD_BUTTON_WEST},
    {3, SDL_GAMEPAD_BUTTON_NORTH},          {10, SDL_GAMEPAD_BUTTON_START},         {5, SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {26, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {27, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER}, {7, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
    {9, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2}};
/* the pair, each button where it sits (pad_map.c's POSITION turned around); ZL and ZR are the triggers' axes */
static const Jc2Button PAIR[] = {
    {1, SDL_GAMEPAD_BUTTON_SOUTH},          {0, SDL_GAMEPAD_BUTTON_EAST},       {3, SDL_GAMEPAD_BUTTON_WEST},
    {2, SDL_GAMEPAD_BUTTON_NORTH},          {6, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER}, {7, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {4, SDL_GAMEPAD_BUTTON_LEFT_STICK},     {5, SDL_GAMEPAD_BUTTON_RIGHT_STICK}, {11, SDL_GAMEPAD_BUTTON_BACK},
    {10, SDL_GAMEPAD_BUTTON_START},         {13, SDL_GAMEPAD_BUTTON_DPAD_UP},   {15, SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {12, SDL_GAMEPAD_BUTTON_DPAD_LEFT},     {14, SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    /* each half's SL and SR as SDL's own pair has them (SDL_gamepad.h's paddles), which the controller applet's
       dialog reads for its gestures */
    {24, SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},  {25, SDL_GAMEPAD_BUTTON_LEFT_PADDLE2}, {26, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {27, SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1}};
#define COUNT(a) (sizeof(a) / sizeof *(a))

static u32 buttons_of(u64 npad, const Jc2Button* map, size_t n) {
    u32 b = 0;
    for (size_t i = 0; i < n; i++)
        if (npad >> map[i].bit & 1) b |= 1u << map[i].button;
    return b;
}
/* SDL's axes point down */
static i16 down(i32 v) { return (i16)(v <= -32767 ? 32767 : v >= 32767 ? -32767 : -v); }
static i16 axis(i32 v) { return (i16)(v < -32767 ? -32767 : v > 32767 ? 32767 : v); }
void jc2_sdl_map(int kind, const Jc2Pad* left, const Jc2Pad* right, u32* buttons, i16 axes[SDL_GAMEPAD_AXIS_COUNT]) {
    u8 extra = (u8)((left ? left->extra : 0) | (right ? right->extra : 0));
    memset(axes, 0, SDL_GAMEPAD_AXIS_COUNT * sizeof *axes);
    *buttons = 0;
    if (kind == JC2_PAD_PAIR) {
        u64 b = (left ? left->buttons : 0) | (right ? right->buttons : 0);
        *buttons = buttons_of(b, PAIR, COUNT(PAIR));
        if (left) {
            axes[SDL_GAMEPAD_AXIS_LEFTX] = axis(left->stick[0]);
            axes[SDL_GAMEPAD_AXIS_LEFTY] = down(left->stick[1]);
        }
        if (right) {
            axes[SDL_GAMEPAD_AXIS_RIGHTX] = axis(right->stick[0]);
            axes[SDL_GAMEPAD_AXIS_RIGHTY] = down(right->stick[1]);
        }
        /* a trigger's raw axis rests at its minimum: SDL maps a joystick axis of -32768..32767 onto the trigger's
           0..32767, so a raw 0 read as half pressed and held ZL and ZR down all the time */
        axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = b >> 8 & 1 ? 32767 : -32768; /* ZL */
        axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = b >> 9 & 1 ? 32767 : -32768; /* ZR */
    } else if (kind == JC2_PAD_PRO) { /* both sticks of its own, every button where a pair has it */
        if (!left) return;
        *buttons = buttons_of(left->buttons, PAIR, COUNT(PAIR));
        axes[SDL_GAMEPAD_AXIS_LEFTX] = axis(left->stick[0]);
        axes[SDL_GAMEPAD_AXIS_LEFTY] = down(left->stick[1]);
        axes[SDL_GAMEPAD_AXIS_RIGHTX] = axis(left->rstick[0]);
        axes[SDL_GAMEPAD_AXIS_RIGHTY] = down(left->rstick[1]);
        axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = left->buttons >> 8 & 1 ? 32767 : -32768;  /* ZL */
        axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = left->buttons >> 9 & 1 ? 32767 : -32768; /* ZR */
    } else {
        const Jc2Pad* p = kind == JC2_PAD_LEFT ? left : right;
        if (!p) return;
        if (kind == JC2_PAD_LEFT) {
            /* upright X, Y held sideways (hid.c's stick_sideways): the stick's up is the pad's left */
            *buttons = buttons_of(p->buttons, SINGLE_LEFT, COUNT(SINGLE_LEFT));
            axes[SDL_GAMEPAD_AXIS_LEFTX] = axis(-p->stick[1]);
            axes[SDL_GAMEPAD_AXIS_LEFTY] = down(p->stick[0]);
        } else {
            *buttons = buttons_of(p->buttons, SINGLE_RIGHT, COUNT(SINGLE_RIGHT));
            axes[SDL_GAMEPAD_AXIS_LEFTX] = axis(p->stick[1]);
            axes[SDL_GAMEPAD_AXIS_LEFTY] = down(-p->stick[0]);
        }
    }
    if (extra & JC2_X_HOME) *buttons |= 1u << SDL_GAMEPAD_BUTTON_GUIDE;
    if (extra & JC2_X_CAPTURE) *buttons |= 1u << SDL_GAMEPAD_BUTTON_MISC1;
}
/* gamepad.c's sample_sensor: Switch x = -SDL x, y = SDL z, z = -SDL y (acceleration in g there, m/s^2 here); angular
   velocity x = SDL x, y = -SDL z, z = SDL y */
void jc2_sdl_motion(const float accel[3], const float gyro[3], float sdl_accel[3], float sdl_gyro[3]) {
    sdl_accel[0] = -accel[0] * 9.80665f;
    sdl_accel[1] = -accel[2] * 9.80665f;
    sdl_accel[2] = accel[1] * 9.80665f;
    sdl_gyro[0] = gyro[0];
    sdl_gyro[1] = gyro[2];
    sdl_gyro[2] = -gyro[1];
}

/* ---- which controllers make which gamepad */
int jc2_join_next(const Jc2Join* d, int n, int pair, u64 now_ns, int* a, int* b) {
    int left = -1, right = -1;
    for (int i = 0; i < n; i++) {
        if (!d[i].ready || (d[i].gamepad && d[i].kind == JC2_PAD_PAIR)) continue;
        if (d[i].side == JC2_SIDE_LEFT && left < 0) left = i;
        if (d[i].side == JC2_SIDE_RIGHT && right < 0) right = i;
    }
    if (pair && left >= 0 && right >= 0 && (!d[left].gamepad || !d[right].gamepad)) {
        *a = left;
        *b = right;
        return JC2_JOIN_PAIR;
    }
    for (int i = 0; i < n; i++)
        if (d[i].ready && !d[i].gamepad &&
            (!pair || d[i].had || d[i].side == JC2_SIDE_PRO || now_ns >= d[i].ready_ns + JC2_PAIR_WAIT_NS)) {
            *a = i;
            return JC2_JOIN_SINGLE;
        }
    return JC2_JOIN_NONE;
}

/* ---- the virtual gamepads */
static bool SDLCALL takes(void* user, Uint16 low, Uint16 high) { /* the vibration comes through joycon2_rumble */
    (void)user, (void)low, (void)high;
    return true;
}
static bool SDLCALL sensors_on(void* user, bool on) {
    (void)user, (void)on;
    return true;
}
int jc2_pad_attach(int kind) {
    static const SDL_VirtualJoystickSensorDesc one[2] = {{SDL_SENSOR_ACCEL, 200.0f}, {SDL_SENSOR_GYRO, 200.0f}};
    static const SDL_VirtualJoystickSensorDesc two[4] = {{SDL_SENSOR_ACCEL_L, 200.0f}, {SDL_SENSOR_GYRO_L, 200.0f},
                                                         {SDL_SENSOR_ACCEL_R, 200.0f}, {SDL_SENSOR_GYRO_R, 200.0f}};
    SDL_VirtualJoystickDesc d;
    int i;
    for (i = 0; i < JC2_PADS && g_joycon2_sdl.pad[i].stick; i++) {}
    if (i == JC2_PADS) return 0;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.vendor_id = JC2_VENDOR;
    /* a pair as SDL's combined Joy-Cons (2008), a Pro Controller 2 as a Pro Controller (2009): the types SDL gives
       those ids are what gamepad.c reads */
    d.product_id = kind == JC2_PAD_LEFT    ? JC2_PID_LEFT
                   : kind == JC2_PAD_RIGHT ? JC2_PID_RIGHT
                   : kind == JC2_PAD_PRO   ? 0x2009
                                           : 0x2008;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
    d.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
    d.nsensors = kind == JC2_PAD_PAIR ? 4 : 2;
    d.sensors = kind == JC2_PAD_PAIR ? two : one;
    d.name = kind == JC2_PAD_LEFT    ? "Joy-Con 2 (L)"
             : kind == JC2_PAD_RIGHT ? "Joy-Con 2 (R)"
             : kind == JC2_PAD_PRO   ? "Pro Controller 2"
                                     : "Joy-Con 2 (L/R)";
    d.Rumble = takes;
    d.SetSensorsEnabled = sensors_on;
    SDL_JoystickID id = SDL_AttachVirtualJoystick(&d);
    SDL_Joystick* stick = id ? SDL_OpenJoystick(id) : NULL;
    if (!stick) {
        if (id) SDL_DetachVirtualJoystick(id);
        return 0;
    }
    g_joycon2_sdl.pad[i] = (Jc2Gamepad){0};
    g_joycon2_sdl.pad[i].stick = stick;
    g_joycon2_sdl.pad[i].kind = kind;
    for (int s = 0; s < 2; s++) g_joycon2_sdl.pad[i].rumble[s] = (Jc2Vibration){0.0f, 160.0f, 0.0f, 320.0f};
    return i + 1;
}
static Jc2Gamepad* pad_at(int pad) {
    return pad > 0 && pad <= JC2_PADS && g_joycon2_sdl.pad[pad - 1].stick ? &g_joycon2_sdl.pad[pad - 1] : NULL;
}
void jc2_pad_detach(int pad) {
    Jc2Gamepad* g = pad_at(pad);
    if (!g) return;
    SDL_JoystickID id = SDL_GetJoystickID(g->stick);
    SDL_CloseJoystick(g->stick);
    SDL_DetachVirtualJoystick(id);
    *g = (Jc2Gamepad){0};
}
u32 jc2_pad_id(int pad) {
    Jc2Gamepad* g = pad_at(pad);
    return g ? SDL_GetJoystickID(g->stick) : 0;
}
static void send_motion(SDL_Joystick* stick, const Jc2Pad* p, SDL_SensorType accel, SDL_SensorType gyro, u64 now) {
    float a[3], w[3];
    jc2_sdl_motion(p->accel, p->gyro, a, w);
    SDL_SendJoystickVirtualSensorData(stick, accel, now, a, 3);
    SDL_SendJoystickVirtualSensorData(stick, gyro, now, w, 3);
}
void jc2_pad_feed(int pad, const Jc2Pad* left, const Jc2Pad* right, const i32* mouse_left, const i32* mouse_right) {
    Jc2Gamepad* g = pad_at(pad);
    u32 buttons;
    i16 axes[SDL_GAMEPAD_AXIS_COUNT];
    if (!g) return;
    jc2_sdl_map(g->kind, left, right, &buttons, axes);
    SDL_LockJoysticks();
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) SDL_SetJoystickVirtualButton(g->stick, b, buttons >> b & 1);
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) SDL_SetJoystickVirtualAxis(g->stick, a, axes[a]);
    u64 now = SDL_GetTicksNS();
    if (g->kind == JC2_PAD_PAIR) {
        if (left && left->reports != g->reports[0])
            send_motion(g->stick, left, SDL_SENSOR_ACCEL_L, SDL_SENSOR_GYRO_L, now);
        if (right && right->reports != g->reports[1])
            send_motion(g->stick, right, SDL_SENSOR_ACCEL_R, SDL_SENSOR_GYRO_R, now);
    } else { /* a single, or a Pro Controller 2 (fed as the left) */
        const Jc2Pad* p = g->kind == JC2_PAD_RIGHT ? right : left;
        if (p && p->reports != g->reports[g->kind == JC2_PAD_RIGHT])
            send_motion(g->stick, p, SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO, now);
    }
    SDL_UnlockJoysticks();
    {
        const Jc2Pad* counted = right ? right : left;
        if (counted && !g->rate_ns) {
            g->rate_from = counted->reports;
            g->rate_ns = now;
        } else if (counted && now - g->rate_ns >= 1000000000ull) {
            g->rate = (int)((u64)(counted->reports - g->rate_from) * 1000000000ull / (now - g->rate_ns));
            g->rate_from = counted->reports;
            g->rate_ns = now;
        }
    }
    if (left) g->reports[0] = left->reports;
    if (right) g->reports[1] = right->reports;
    /* a single's one sensor is the first, as PadMotion has a single Joy-Con's */
    int second = g->kind == JC2_PAD_PAIR;
    g->surface = (u8)((left && left->on_surface ? 1u : 0u) | (right && right->on_surface ? 1u << second : 0u));
    for (int i = 0; i < 2; i++) {
        if (mouse_left) g->mouse[0][i] += (float)mouse_left[i];
        if (mouse_right) g->mouse[second][i] += (float)mouse_right[i];
    }
}
void jc2_pad_rumble(int pad, Jc2Vibration out[2]) {
    Jc2Gamepad* g = pad_at(pad);
    for (int s = 0; s < 2; s++) out[s] = g ? g->rumble[s] : (Jc2Vibration){0.0f, 160.0f, 0.0f, 320.0f};
}
int jc2_pad_player(int pad) {
    u32 id = jc2_pad_id(pad);
    for (int slot = 0; id && slot < SNX_GAMEPADS; slot++)
        if (gamepad_slot_id(slot) == id) return slot;
    return -1;
}

static Jc2Gamepad* pad_of(u32 id) {
    for (int i = 0; id && i < JC2_PADS; i++)
        if (g_joycon2_sdl.pad[i].stick && SDL_GetJoystickID(g_joycon2_sdl.pad[i].stick) == id) return &g_joycon2_sdl.pad[i];
    return NULL;
}
static Jc2Vibration vibration(u32 amps, float low_hz, float high_hz) {
    return (Jc2Vibration){(float)(amps & 0xffff) / 65535.0f, low_hz, (float)(amps >> 16) / 65535.0f, high_hz};
}
int joycon2_rumble(u32 id, u32 left, u32 right, const float hz[4]) {
    Jc2Gamepad* g = pad_of(id);
    if (!g) return 0;
    if (g->kind == JC2_PAD_PAIR) {
        g->rumble[0] = vibration(left, hz[0], hz[1]);
        g->rumble[1] = vibration(right, hz[2], hz[3]);
        return 1;
    }
    /* one actuator: each band the stronger side's, at that side's frequency (as gamepad.c's hd_play does for a single
       Joy-Con) */
    int lo = (right & 0xffff) > (left & 0xffff), hi = right >> 16 > left >> 16;
    u32 amps = ((lo ? right : left) & 0xffffu) | ((hi ? right : left) & 0xffff0000u);
    g->rumble[g->kind == JC2_PAD_RIGHT] = vibration(amps, hz[2 * lo], hz[2 * hi + 1]);
    return 1;
}
void jc2_pad_who(int pad, const char* who) {
    Jc2Gamepad* g = pad_at(pad);
    if (g) snprintf(g->who, sizeof g->who, "%s", who ? who : "");
}
int joycon2_surface(u32 id) {
    Jc2Gamepad* g = pad_of(id);
    return g ? g->surface : -1;
}
int joycon2_rate(u32 id) {
    Jc2Gamepad* g = pad_of(id);
    return g ? g->rate : 0;
}
int joycon2_who(u32 id, char* out, size_t n) {
    Jc2Gamepad* g = pad_of(id);
    if (!g || !g->who[0]) return 0;
    snprintf(out, n, "%s", g->who);
    return 1;
}
int joycon2_mouse(u32 id, float out[2][2]) {
    Jc2Gamepad* g = pad_of(id);
    if (!g) return 0;
    float scale = g_settings.joy_con_2_mouse_scale;
    for (int s = 0; s < 2; s++)
        for (int i = 0; i < 2; i++) {
            out[s][i] = g->mouse[s][i] * scale;
            g->mouse[s][i] = 0;
        }
    return 1;
}
