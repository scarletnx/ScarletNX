/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "nx/hle/services/hid_motion.h"
#include "nx/runtime/platform/tracy.h"
#include "common/utilities/endian.h"
#include <math.h>
#include <string.h>

/* q and delta are scalar-first. Compose the gyro step and keep accumulated drift bounded. */
static void quat_multiply_normalized(float q[4], const float delta[4]) {
    float w = q[0] * delta[0] - q[1] * delta[1] - q[2] * delta[2] - q[3] * delta[3];
    float x = q[0] * delta[1] + q[1] * delta[0] + q[2] * delta[3] - q[3] * delta[2];
    float y = q[0] * delta[2] - q[1] * delta[3] + q[2] * delta[0] + q[3] * delta[1];
    float z = q[0] * delta[3] + q[1] * delta[2] - q[2] * delta[1] + q[3] * delta[0];
    float norm = sqrtf(w * w + x * x + y * y + z * z);
    if (norm > 0.0f) {
        q[0] = w / norm;
        q[1] = x / norm;
        q[2] = y / norm;
        q[3] = z / norm;
    }
}

/* The state's direction: row i is the pad's own axis i in world coordinates (q turns the pad's frame into the world's,
   so these are the columns of q's matrix). The console keeps it so: it turns the rows as the pad's axes by the
   gyroscope and pulls ax * row0 + ay * row1 + az * row2 toward (0, 0, -1) */
static void quat_direction(const float q[4], float dir[9]) {
    float w = q[0], x = q[1], y = q[2], z = q[3];
    dir[0] = 1.0f - 2.0f * (y * y + z * z);
    dir[1] = 2.0f * (x * y + z * w);
    dir[2] = 2.0f * (x * z - y * w);
    dir[3] = 2.0f * (x * y - z * w);
    dir[4] = 1.0f - 2.0f * (x * x + z * z);
    dir[5] = 2.0f * (y * z + x * w);
    dir[6] = 2.0f * (x * z + y * w);
    dir[7] = 2.0f * (y * z - x * w);
    dir[8] = 1.0f - 2.0f * (x * x + y * y);
}

int hid_motion_at_rest(const PadMotion* motion) {
    float magnitude;
    float rate;
    if (!motion || !motion->valid) return 1;
    magnitude = motion->acceleration[0] * motion->acceleration[0] + motion->acceleration[1] * motion->acceleration[1] +
                motion->acceleration[2] * motion->acceleration[2];
    rate = motion->angular_velocity[0] * motion->angular_velocity[0] +
           motion->angular_velocity[1] * motion->angular_velocity[1] +
           motion->angular_velocity[2] * motion->angular_velocity[2];
    return magnitude > 0.85f && magnitude < 1.15f && rate < 0.0025f;
}

void hid_motion_sample(HidMotionState* state, const PadMotion* motion, u64 dt_ns, u64 sampling_number,
                       u8 out[HS_HidSixAxisSensorState]) {
    float acc[3] = {0.0f, 0.0f, -1.0f};
    float gyro[3] = {0.0f, 0.0f, 0.0f};
    float corrected[3];
    float dir[9];
    float* q = state->quaternion;
    float rate, theta, s, w, norm, gravity_norm, mx, my, mz;
    float px, py, pz, gain;
    float dq[4];
    u32 attr = 1; /* HidSixAxisSensorAttribute_IsConnected */
    int i;
    /* 1: a live sensor's fusion, 0: a neutral pose from a missing or disabled sensor */
    SNX_ZONE_V(z_hid_motion, "svc hid motion", motion && motion->valid ? 1u : 0u);
    memset(out, 0, HS_HidSixAxisSensorState);
    if (q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f && q[3] == 0.0f) q[0] = 1.0f;
    if (motion && motion->valid) {
        memcpy(acc, motion->acceleration, sizeof acc);
        memcpy(gyro, motion->angular_velocity, sizeof gyro);
        gravity_norm = sqrtf(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
        memcpy(corrected, gyro, sizeof corrected);
        /* Acceleration includes gravity. Use it to establish physical pitch and roll on the first reading,
           then gently correct gyro drift when linear acceleration is small. Yaw has no gravity reference. */
        if (gravity_norm > 0.85f && gravity_norm < 1.15f) {
            mx = acc[0] / gravity_norm;
            my = acc[1] / gravity_norm;
            mz = acc[2] / gravity_norm;
            if (!state->was_valid) {
                w = 1.0f - mz;
                if (w < 0.0001f) {
                    q[0] = 0.0f;
                    q[1] = 1.0f;
                    q[2] = q[3] = 0.0f;
                } else {
                    norm = sqrtf(w * w + mx * mx + my * my);
                    q[0] = w / norm;
                    q[1] = -my / norm;
                    q[2] = mx / norm;
                    q[3] = 0.0f;
                }
            } else {
                px = -2.0f * (q[1] * q[3] - q[2] * q[0]);
                py = -2.0f * (q[2] * q[3] + q[1] * q[0]);
                pz = -1.0f + 2.0f * (q[1] * q[1] + q[2] * q[2]);
                gain = 2.5f;
                corrected[0] += gain * (my * pz - mz * py);
                corrected[1] += gain * (mz * px - mx * pz);
                corrected[2] += gain * (mx * py - my * px);
            }
        }
        state->was_valid = 1;
    } else if (state->was_valid) {
        /* A disconnected or disabled sensor starts from a neutral pose when it returns. */
        memset(state, 0, sizeof *state);
        q[0] = 1.0f;
    }
    if (!motion || !motion->valid) memcpy(corrected, gyro, sizeof corrected);
    /* Guard a suspended host or debugger pause from producing a huge jump. */
    if (dt_ns > 100000000ull) dt_ns = 100000000ull;
    rate = sqrtf(corrected[0] * corrected[0] + corrected[1] * corrected[1] + corrected[2] * corrected[2]);
    theta = rate * ((float)dt_ns * 0.000000001f);
    if (rate > 0.000001f && rate < 1000000.0f) {
        s = sinf(theta * 0.5f) / rate;
        dq[0] = cosf(theta * 0.5f);
        dq[1] = corrected[0] * s;
        dq[2] = corrected[1] * s;
        dq[3] = corrected[2] * s;
        quat_multiply_normalized(q, dq);
    }
    /* The state's angular velocity and angle are in turns (1 = 360 degrees), the fusion's in radians: the console
       scales the gyroscope to turns a second and sums the turns into the angle */
    for (i = 0; i < 3; i++) {
        gyro[i] *= 0.5f / 3.14159265f;
        state->angle[i] += gyro[i] * ((float)dt_ns * 0.000000001f);
    }
    quat_direction(q, dir);
    memcpy(out, &dt_ns, 8);
    memcpy(out + 8, &sampling_number, 8);
    memcpy(out + HO_HidSixAxisSensorState_acceleration, acc, sizeof acc);
    memcpy(out + HO_HidSixAxisSensorState_acceleration + 12, gyro, sizeof gyro);
    memcpy(out + HO_HidSixAxisSensorState_acceleration + 24, state->angle, sizeof state->angle);
    memcpy(out + HO_HidSixAxisSensorState_direction, dir, sizeof dir);
    memcpy(out + HO_HidSixAxisSensorState_attributes, &attr, 4);
    /* the game reads it little-endian: the two counts, then words (floats and the attributes) to the end */
    snx_le64_array(out, 2);
    snx_le32_array(out + 16, (HS_HidSixAxisSensorState - 16) / 4);
}
