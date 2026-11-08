/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "common/utilities/pad_sdl.h"
#include <stdio.h>

/* each control's button; the triggers are axes */
static const SDL_GamepadButton BUTTONS[SNX_PAD_CONTROLS] = {
    SDL_GAMEPAD_BUTTON_SOUTH,       SDL_GAMEPAD_BUTTON_EAST,          SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,       SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_BUTTON_INVALID,     SDL_GAMEPAD_BUTTON_INVALID,       SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK, SDL_GAMEPAD_BUTTON_BACK,          SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_DPAD_UP,     SDL_GAMEPAD_BUTTON_DPAD_DOWN,     SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT};
/* pad_map.h's labels are SDL's, in its order */
typedef char snx_pad_labels_sdl[SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE == SNX_PAD_LABEL_TRIANGLE &&
                                        SDL_GAMEPAD_BUTTON_LABEL_A == SNX_PAD_LABEL_A
                                    ? 1
                                    : -1];

int snx_pad_of_button(int button) {
    for (int c = 0; c < SNX_PAD_CONTROLS; ++c)
        if (BUTTONS[c] != SDL_GAMEPAD_BUTTON_INVALID && (int)BUTTONS[c] == button) return c;
    return -1;
}
int snx_pad_held(SDL_Gamepad* g, int control) {
    if (control == SNX_PAD_LT || control == SNX_PAD_RT)
        return SDL_GetGamepadAxis(g, control == SNX_PAD_LT ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER
                                                           : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > SNX_PAD_TRIGGER;
    return control >= 0 && control < SNX_PAD_CONTROLS && SDL_GetGamepadButton(g, BUTTONS[control]);
}
void snx_pad_labels(SDL_Gamepad* g, int labels[4]) {
    for (int c = SNX_PAD_SOUTH; c <= SNX_PAD_NORTH; ++c) labels[c] = (int)SDL_GetGamepadButtonLabel(g, BUTTONS[c]);
}
int snx_pad_symmetric(SDL_Gamepad* g) {
    switch (SDL_GetGamepadType(g)) {
        case SDL_GAMEPAD_TYPE_PS3:
        case SDL_GAMEPAD_TYPE_PS4:
        case SDL_GAMEPAD_TYPE_PS5: return 1;
        default: return 0;
    }
}
/* each pair's sensors, and their names in the lines snx_pad_motion_enable says */
static const struct {
    SDL_SensorType accel, gyro;
    const char *accel_name, *gyro_name;
} MOTIONS[SNX_PAD_MOTIONS] = {
    {SDL_SENSOR_ACCEL, SDL_SENSOR_GYRO, "accelerometer", "gyroscope"},
    {SDL_SENSOR_ACCEL_L, SDL_SENSOR_GYRO_L, "left accelerometer", "left gyroscope"},
    {SDL_SENSOR_ACCEL_R, SDL_SENSOR_GYRO_R, "right accelerometer", "right gyroscope"},
};
static void enable_sensor(SDL_Gamepad* g, SDL_SensorType type, const char* name, void (*say)(int, const char*)) {
    char line[256];
    const char* pad = SDL_GetGamepadName(g);
    if (!SDL_GamepadHasSensor(g, type)) return;
    if (SDL_SetGamepadSensorEnabled(g, type, true)) {
        snprintf(line, sizeof line, "enabled %s sensor on %s", name, pad ? pad : "a pad with no name");
        say(0, line);
    } else {
        snprintf(line, sizeof line, "cannot enable %s sensor on %s: %s", name, pad ? pad : "a pad with no name",
                 SDL_GetError());
        say(1, line);
    }
}
void snx_pad_motion_enable(SDL_Gamepad* g, void (*say)(int warn, const char* line)) {
    int any = 0;
    for (int m = 0; m < SNX_PAD_MOTIONS; ++m) {
        any |= SDL_GamepadHasSensor(g, MOTIONS[m].accel);
        enable_sensor(g, MOTIONS[m].accel, MOTIONS[m].accel_name, say);
        enable_sensor(g, MOTIONS[m].gyro, MOTIONS[m].gyro_name, say);
    }
    if (!any) {
        char line[256];
        const char* pad = SDL_GetGamepadName(g);
        snprintf(line, sizeof line, "%s exposes no motion sensors through SDL", pad ? pad : "a pad with no name");
        say(0, line);
    }
}
int snx_pad_motion_read(SDL_Gamepad* g, int which, float accel[3], float gyro[3]) {
    if (which < 0 || which >= SNX_PAD_MOTIONS) return 0;
    return SDL_GamepadSensorEnabled(g, MOTIONS[which].accel) && SDL_GamepadSensorEnabled(g, MOTIONS[which].gyro) &&
           SDL_GetGamepadSensorData(g, MOTIONS[which].accel, accel, 3) &&
           SDL_GetGamepadSensorData(g, MOTIONS[which].gyro, gyro, 3);
}
int snx_pad_slot(const char* guid, int alike, const char* const* players, int count, unsigned taken) {
    char want[48];
    if (alike)
        snprintf(want, sizeof want, "%s/%d", guid, alike + 1);
    else
        snprintf(want, sizeof want, "%s", guid);
    for (int i = 0; i < count; i++)
        if (!(taken >> i & 1u) && players[i] && !SDL_strcasecmp(players[i], want)) return i;
    for (int i = 0; i < count; i++)
        if (!(taken >> i & 1u) && (!players[i] || !players[i][0])) return i;
    return -1;
}
