/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* hid:USER's accelerometer and gyroscope samples (services/hid.c) from an SDL virtual gamepad's motion sensors, under
   SDL's dummy video driver: in the axes and units of Azahar's motion device, with the raw entries Azahar makes from
   them, a pad's own sensors or a Joy-Con's, and the console at rest with no pad that has them */
#include "3ds/input/input.h"
#include "3ds/ipc/ctr_ipc.h"
#include "3ds/runtime/internal.h"
#include "3ds/services/hid.h"
#include "support/3ds_guest.h"
#include "support/sdl_real_pads.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>

static int checks;
#define CHECK(e)                                                                                                       \
    do {                                                                                                               \
        checks++;                                                                                                      \
        if (!(e)) {                                                                                                    \
            fprintf(stderr, "3ds_motion.c:%d: check failed: %s\n", __LINE__, #e);                                      \
            exit(1);                                                                                                   \
        }                                                                                                              \
    } while (0)

#define G       9.80665f        /* SDL's m/s^2 a g */
/* hid:USER's shared memory: each sensor's index, entries and raw entry */
enum { ACCEL_INDEX = 0x118, ACCEL_ENTRIES = 0x128, ACCEL_RAW = 0x120, GYRO_INDEX = 0x168, GYRO_ENTRIES = 0x178 };
enum { GYRO_RAW = 0x170 };

static void pump(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) snx_ctr_input_event(&e);
    SDL_UpdateGamepads();
}
static bool SDLCALL sensors_enabled(void* pad, bool enabled) {
    (void)pad;
    (void)enabled;
    return true;
}
/* a pad with the sensors given, opened by the input as it connects */
static SDL_Joystick* attach(const SDL_VirtualJoystickSensorDesc* sensors, int count) {
    SDL_VirtualJoystickDesc d;
    SDL_INIT_INTERFACE(&d);
    d.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    d.naxes = SDL_GAMEPAD_AXIS_COUNT;
    d.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
    d.nsensors = count;
    d.sensors = sensors;
    d.SetSensorsEnabled = sensors_enabled;
    d.vendor_id = 0x057e;
    d.product_id = 0x2009;
    SDL_Joystick* j = SDL_OpenJoystick(SDL_AttachVirtualJoystick(&d));
    CHECK(j != NULL);
    pump();
    return j;
}
/* a reading of one sensor, x, y and z as SDL has them */
static void send(SDL_Joystick* j, SDL_SensorType type, float x, float y, float z) {
    const float v[3] = {x, y, z};
    CHECK(SDL_SendJoystickVirtualSensorData(j, type, SDL_GetTicksNS(), v, 3));
    pump();
}
/* the next sample of each sensor: its entry's x, y and z, then its raw entry's */
static void sample(SnxCtrProcess* p, int16_t accel[6], int16_t gyro[6]) {
    uint64_t due = p->hid.next_accelerometer_ns > p->hid.next_gyroscope_ns ? p->hid.next_accelerometer_ns
                                                                           : p->hid.next_gyroscope_ns;
    snx_ctr_hid_update(p, due);
    uint32_t a = ACCEL_ENTRIES + ctr_hid_shared(p, ACCEL_INDEX, 4) * 6,
             g = GYRO_ENTRIES + ctr_hid_shared(p, GYRO_INDEX, 4) * 6;
    for (unsigned i = 0; i < 3; ++i) {
        accel[i] = (int16_t)ctr_hid_shared(p, a + i * 2, 2);
        accel[3 + i] = (int16_t)ctr_hid_shared(p, ACCEL_RAW + i * 2, 2);
        gyro[i] = (int16_t)ctr_hid_shared(p, g + i * 2, 2);
        gyro[3 + i] = (int16_t)ctr_hid_shared(p, GYRO_RAW + i * 2, 2);
    }
}
static bool is(const int16_t* got, int16_t x, int16_t y, int16_t z, int16_t rx, int16_t ry, int16_t rz) {
    bool same = got[0] == x && got[1] == y && got[2] == z && got[3] == rx && got[4] == ry && got[5] == rz;
    if (!same)
        fprintf(stderr, "sample %d %d %d, raw %d %d %d: expected %d %d %d, raw %d %d %d\n", got[0], got[1], got[2],
                got[3], got[4], got[5], x, y, z, rx, ry, rz);
    return same;
}

int main(void) {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    sdl_real_pads_hide();
    CHECK(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD));
    snx_ctr_input_init(false);
    unsigned char code[0x3000] = {0};
    SnxCtrImage image = ctr_image(code, sizeof code, 0x1000);
    SnxCtrProcess* p = snx_ctr_process_new(&image, SNX_CTR_OLD);
    CHECK(p != NULL);
    uint32_t session = 0;
    CHECK(!snx_ctr_session_create(p, SNX_CTR_SERVICE_HID, &session));
    CHECK(ctr_send_header(p, session, 0x000a0000) == 0x000a0047);
    CHECK(ctr_send_header(p, session, 0x00110000) == 0x00110040 &&
          ctr_send_header(p, session, 0x00130000) == 0x00130040);
    int16_t accel[6], gyro[6];

    /* no pad: the console at rest, gravity along -y, still */
    sample(p, accel, gyro);
    CHECK(is(accel, 0, -512, 0, 0, 0, -1024) && is(gyro, 0, 0, 0, 0, 0, 0));
    /* a pad's own sensors: held up in front of the player at rest it reads as the console at rest */
    static const SDL_VirtualJoystickSensorDesc own[2] = {{SDL_SENSOR_ACCEL, 60.0f}, {SDL_SENSOR_GYRO, 60.0f}};
    SDL_Joystick* pad = attach(own, 2);
    send(pad, SDL_SENSOR_ACCEL, 0, G, 0);
    send(pad, SDL_SENSOR_GYRO, 0, 0, 0);
    sample(p, accel, gyro);
    CHECK(is(accel, 0, -512, 0, 0, 0, -1024) && is(gyro, 0, 0, 0, 0, 0, 0));
    /* Azahar's axes: y of the acceleration and x and z of the rotation negated; 512 raw a g and 14.375 a degree a
       second, truncated; the raw entries -2x, -2z, 2y and x, z, -y */
    send(pad, SDL_SENSOR_ACCEL, G / 2, G, -G);
    send(pad, SDL_SENSOR_GYRO, 1, -0.5f, 2);
    sample(p, accel, gyro);
    CHECK(is(accel, 256, -512, -512, -512, 1024, -1024));
    CHECK(is(gyro, -823, -411, -1647, -823, -1647, 411));
    /* a turn past what the entry holds is held to its range */
    send(pad, SDL_SENSOR_GYRO, 100, 0, 0);
    sample(p, accel, gyro);
    CHECK(gyro[0] == -32768 && gyro[3] == -32768);
    /* the pad gone: the console at rest again */
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pad));
    pump();
    sample(p, accel, gyro);
    CHECK(is(accel, 0, -512, 0, 0, 0, -1024) && is(gyro, 0, 0, 0, 0, 0, 0));
    /* a Joy-Con's sensors, as SDL names a left one's */
    static const SDL_VirtualJoystickSensorDesc left[2] = {{SDL_SENSOR_ACCEL_L, 60.0f}, {SDL_SENSOR_GYRO_L, 60.0f}};
    pad = attach(left, 2);
    send(pad, SDL_SENSOR_ACCEL_L, 0, 0, G);
    send(pad, SDL_SENSOR_GYRO_L, 0, 1, 0);
    sample(p, accel, gyro);
    CHECK(is(accel, 0, 0, 512, 0, -1024, 0) && is(gyro, 0, 823, 0, 0, 0, -823));
    SDL_DetachVirtualJoystick(SDL_GetJoystickID(pad));
    pump();

    snx_ctr_process_free(p);
    snx_ctr_input_free();
    SDL_Quit();
    printf("3ds motion: %d checks\n", checks);
    return 0;
}
