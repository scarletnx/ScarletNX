/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/hle/services/hid_motion.h"
#include "common/utilities/endian.h"
#include <assert.h>
#include <math.h>
#include <string.h>

static float f32_at(const u8* state, int offset) {
    u32 bits = snx_ld_le32(state + offset);
    float value;
    memcpy(&value, &bits, 4);
    return value;
}
static u64 u64_at(const u8* state, int offset) {
    return snx_ld_le64(state + offset);
}
static u32 u32_at(const u8* state, int offset) {
    return snx_ld_le32(state + offset);
}
static int float_close(float a, float b) { return fabsf(a - b) < 0.0005f; }
static void check_direction(const u8* state, const float expected[9]) {
    float rows[3][3];
    for (int row = 0; row < 3; row++)
        for (int column = 0; column < 3; column++) {
            rows[row][column] = f32_at(state, HO_HidSixAxisSensorState_direction + 4 * (row * 3 + column));
            assert(float_close(rows[row][column], expected[row * 3 + column]));
        }
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++) {
            float dot = 0.0f;
            for (int column = 0; column < 3; column++) dot += rows[a][column] * rows[b][column];
            assert(float_close(dot, a == b ? 1.0f : 0.0f));
        }
    /* Unit-length rows can also describe a reflection; expected entries above catch a transposition. */
    float handedness = (rows[0][1] * rows[1][2] - rows[0][2] * rows[1][1]) * rows[2][0] +
                       (rows[0][2] * rows[1][0] - rows[0][0] * rows[1][2]) * rows[2][1] +
                       (rows[0][0] * rows[1][1] - rows[0][1] * rows[1][0]) * rows[2][2];
    assert(float_close(handedness, 1.0f));
}

int main(void) {
    HidMotionState state;
    PadMotion motion;
    u8 out[HS_HidSixAxisSensorState];
    memset(&state, 0, sizeof state);
    memset(&motion, 0, sizeof motion);
    hid_motion_sample(&state, NULL, 0, 1, out);
    assert(u64_at(out, 8) == 1 && float_close(f32_at(out, HO_HidSixAxisSensorState_acceleration + 8), -1.0f));
    const float identity[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    check_direction(out, identity);
    assert(u32_at(out, HO_HidSixAxisSensorState_attributes) == 1);
    assert(hid_motion_at_rest(NULL));

    motion.valid = 1;
    motion.acceleration[2] = -1.0f;
    motion.angular_velocity[2] = 1.57079632679f;
    hid_motion_sample(&state, &motion, 1000000000ull, 2, out);
    /* A delayed frame integrates only 100 ms, preventing a one-second jump. */
    assert(u64_at(out, 0) == 100000000ull && u64_at(out, 8) == 2);
    assert(float_close(f32_at(out, HO_HidSixAxisSensorState_acceleration + 8), -1.0f));
    /* the state's angular velocity and angle are in turns: 90 degrees/s is 0.25 */
    assert(float_close(f32_at(out, HO_HidSixAxisSensorState_acceleration + 20), 0.25f));
    assert(float_close(f32_at(out, HO_HidSixAxisSensorState_acceleration + 32), 0.025f));
    /* the rows are the pad's axes in world coordinates: a turn to the left moves the X row toward +Y */
    assert(f32_at(out, HO_HidSixAxisSensorState_direction + 4) > 0.15f);
    assert(!hid_motion_at_rest(&motion));

    motion.angular_velocity[2] = 0.0f;
    motion.acceleration[0] = 0.0f;
    motion.acceleration[1] = 0.0f;
    motion.acceleration[2] = -1.0f;
    assert(hid_motion_at_rest(&motion));
    hid_motion_sample(&state, NULL, 16000000, 3, out);
    check_direction(out, identity);
    assert(float_close(f32_at(out, HO_HidSixAxisSensorState_acceleration + 32), 0.0f));
    /* The first accelerometer sample determines pitch and roll even without a prior gyro movement. */
    memset(&state, 0, sizeof state);
    memset(&motion, 0, sizeof motion);
    motion.valid = 1;
    motion.acceleration[1] = 1.0f;
    hid_motion_sample(&state, &motion, 0, 4, out);
    const float tilted[9] = {1, 0, 0, 0, 0, -1, 0, 1, 0};
    check_direction(out, tilted);
    /* A short gyro impulse disturbs that tilt. Gravity brings it back when the pad rests. */
    motion.angular_velocity[0] = 1.0f;
    hid_motion_sample(&state, &motion, 100000000, 5, out);
    motion.angular_velocity[0] = 0.0f;
    {
        int i;
        for (i = 0; i < 20; i++) {
            hid_motion_sample(&state, &motion, 100000000, (u64)(6 + i), out);
            assert(float_close(f32_at(out, HO_HidSixAxisSensorState_acceleration + 12), 0.0f));
            assert(float_close(f32_at(out, HO_HidSixAxisSensorState_acceleration + 24), 0.1f / 6.28318531f));
        }
    }
    assert(f32_at(out, HO_HidSixAxisSensorState_direction + 28) > 0.999f);
    memset(&state, 0, sizeof state);
    memset(&motion, 0, sizeof motion);
    motion.valid = 1;
    motion.acceleration[2] = -1.0f;
    motion.angular_velocity[2] = 15.7079632679f;
    hid_motion_sample(&state, &motion, 100000000, 26, out);
    const float yaw[9] = {0, 1, 0, -1, 0, 0, 0, 0, 1};
    check_direction(out, yaw);
    /* Rotations around two axes distinguish q * delta from delta * q. */
    memset(&state, 0, sizeof state);
    memset(&motion, 0, sizeof motion);
    motion.valid = 1;
    motion.angular_velocity[0] = 15.7079632679f;
    hid_motion_sample(&state, &motion, 100000000, 27, out);
    motion.angular_velocity[0] = 0.0f;
    motion.angular_velocity[1] = 15.7079632679f;
    hid_motion_sample(&state, &motion, 100000000, 28, out);
    const float compound[9] = {0, 1, 0, 0, 0, 1, 1, 0, 0};
    check_direction(out, compound);
    /* The inverted gravity pose keeps the existing deterministic X-axis choice. */
    memset(&state, 0, sizeof state);
    memset(&motion, 0, sizeof motion);
    motion.valid = 1;
    motion.acceleration[2] = 1.0f;
    hid_motion_sample(&state, &motion, 0, 29, out);
    const float inverted[9] = {1, 0, 0, 0, -1, 0, 0, 0, -1};
    check_direction(out, inverted);
    return 0;
}
