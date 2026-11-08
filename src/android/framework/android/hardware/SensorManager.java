// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright 2026 the ScarletNX contributors
// Parts follow the Android Open Source Project's source of this class (API level 36), with ScarletNX's changes:
// Copyright (C) 2008 The Android Open Source Project
// SPDX-License-Identifier: Apache-2.0
package android.hardware;

import android.os.Handler;
import java.util.ArrayList;
import java.util.List;

/**
 * The device's sensors: none (Off in docs/platforms/android-apps/interfaces.md), as a desktop has none, so a registration fails and
 * no event comes. The rotation helpers compute what Android's do, from their documented definitions.
 */
public class SensorManager {
    public static final float GRAVITY_EARTH = 9.80665f, STANDARD_GRAVITY = 9.80665f;
    public static final int SENSOR_DELAY_FASTEST = 0, SENSOR_DELAY_GAME = 1, SENSOR_DELAY_UI = 2, SENSOR_DELAY_NORMAL = 3;
    public static final int SENSOR_STATUS_NO_CONTACT = -1, SENSOR_STATUS_UNRELIABLE = 0, SENSOR_STATUS_ACCURACY_LOW = 1,
        SENSOR_STATUS_ACCURACY_MEDIUM = 2, SENSOR_STATUS_ACCURACY_HIGH = 3;
    public static final int AXIS_X = 1, AXIS_Y = 2, AXIS_Z = 3, AXIS_MINUS_X = AXIS_X | 0x80, AXIS_MINUS_Y = AXIS_Y | 0x80,
        AXIS_MINUS_Z = AXIS_Z | 0x80;

    public SensorManager() {}

    public List<Sensor> getSensorList(int type) { return new ArrayList<>(); }
    public Sensor getDefaultSensor(int type) { return null; }
    public Sensor getDefaultSensor(int type, boolean wakeUp) { return null; }
    public boolean registerListener(SensorEventListener listener, Sensor sensor, int samplingPeriodUs) { return false; }
    public boolean registerListener(SensorEventListener listener, Sensor sensor, int samplingPeriodUs, int maxReportLatencyUs) { return false; }
    public boolean registerListener(SensorEventListener listener, Sensor sensor, int samplingPeriodUs, Handler handler) { return false; }
    public boolean registerListener(SensorEventListener listener, Sensor sensor, int samplingPeriodUs, int maxReportLatencyUs, Handler handler) { return false; }
    public void unregisterListener(SensorEventListener listener) {}
    public void unregisterListener(SensorEventListener listener, Sensor sensor) {}

    /**
     * The rotation from the device's frame to the world's (east, north, up) by the gravity and the geomagnetic field,
     * row major, 3x3 or 4x4; the inclination matrix too when I is given. False in free fall or near the poles.
     */
    public static boolean getRotationMatrix(float[] R, float[] I, float[] gravity, float[] geomagnetic) {
        float ax = gravity[0], ay = gravity[1], az = gravity[2];
        float normsqA = ax * ax + ay * ay + az * az;
        final float freeFall = GRAVITY_EARTH * GRAVITY_EARTH / 100f; /* below a tenth of g */
        if (normsqA < freeFall) return false;
        float ex = geomagnetic[0], ey = geomagnetic[1], ez = geomagnetic[2];
        /* east = field x gravity, normalized; north = gravity x east */
        float hx = ey * az - ez * ay, hy = ez * ax - ex * az, hz = ex * ay - ey * ax;
        float normH = (float) Math.sqrt(hx * hx + hy * hy + hz * hz);
        if (normH < 0.1f) return false;
        float invH = 1.0f / normH;
        hx *= invH;
        hy *= invH;
        hz *= invH;
        float invA = 1.0f / (float) Math.sqrt(normsqA);
        ax *= invA;
        ay *= invA;
        az *= invA;
        float mx = ay * hz - az * hy, my = az * hx - ax * hz, mz = ax * hy - ay * hx;
        if (R != null && R.length == 9) {
            R[0] = hx; R[1] = hy; R[2] = hz;
            R[3] = mx; R[4] = my; R[5] = mz;
            R[6] = ax; R[7] = ay; R[8] = az;
        } else if (R != null && R.length == 16) {
            R[0] = hx; R[1] = hy; R[2] = hz; R[3] = 0;
            R[4] = mx; R[5] = my; R[6] = mz; R[7] = 0;
            R[8] = ax; R[9] = ay; R[10] = az; R[11] = 0;
            R[12] = 0; R[13] = 0; R[14] = 0; R[15] = 1;
        }
        if (I != null) {
            /* the field's inclination from the horizontal plane */
            float invE = 1.0f / (float) Math.sqrt(ex * ex + ey * ey + ez * ez);
            float c = (ex * mx + ey * my + ez * mz) * invE, s = (ex * ax + ey * ay + ez * az) * invE;
            if (I.length == 9) {
                I[0] = 1; I[1] = 0; I[2] = 0;
                I[3] = 0; I[4] = c; I[5] = s;
                I[6] = 0; I[7] = -s; I[8] = c;
            } else if (I.length == 16) {
                I[0] = 1; I[1] = 0; I[2] = 0; I[3] = 0;
                I[4] = 0; I[5] = c; I[6] = s; I[7] = 0;
                I[8] = 0; I[9] = -s; I[10] = c; I[11] = 0;
                I[12] = 0; I[13] = 0; I[14] = 0; I[15] = 1;
            }
        }
        return true;
    }

    /** azimuth, pitch and roll (radians) of a rotation matrix (3x3 or 4x4) */
    public static float[] getOrientation(float[] R, float[] values) {
        if (R.length == 9) {
            values[0] = (float) Math.atan2(R[1], R[4]);
            values[1] = (float) Math.asin(Math.max(-1, Math.min(1, -R[7])));
            values[2] = (float) Math.atan2(-R[6], R[8]);
        } else {
            values[0] = (float) Math.atan2(R[1], R[5]);
            values[1] = (float) Math.asin(Math.max(-1, Math.min(1, -R[9])));
            values[2] = (float) Math.atan2(-R[8], R[10]);
        }
        return values;
    }

    /** the rotation matrix of a rotation vector (the x, y, z of a unit quaternion, its w given or derived) */
    public static void getRotationMatrixFromVector(float[] R, float[] rotationVector) {
        float q1 = rotationVector[0], q2 = rotationVector[1], q3 = rotationVector[2], q0;
        if (rotationVector.length >= 4) {
            q0 = rotationVector[3];
        } else {
            q0 = 1 - q1 * q1 - q2 * q2 - q3 * q3;
            q0 = q0 > 0 ? (float) Math.sqrt(q0) : 0;
        }
        float sq1 = 2 * q1 * q1, sq2 = 2 * q2 * q2, sq3 = 2 * q3 * q3;
        float q1q2 = 2 * q1 * q2, q3q0 = 2 * q3 * q0, q1q3 = 2 * q1 * q3, q2q0 = 2 * q2 * q0, q2q3 = 2 * q2 * q3,
            q1q0 = 2 * q1 * q0;
        if (R.length == 9) {
            R[0] = 1 - sq2 - sq3; R[1] = q1q2 - q3q0; R[2] = q1q3 + q2q0;
            R[3] = q1q2 + q3q0; R[4] = 1 - sq1 - sq3; R[5] = q2q3 - q1q0;
            R[6] = q1q3 - q2q0; R[7] = q2q3 + q1q0; R[8] = 1 - sq1 - sq2;
        } else if (R.length == 16) {
            R[0] = 1 - sq2 - sq3; R[1] = q1q2 - q3q0; R[2] = q1q3 + q2q0; R[3] = 0;
            R[4] = q1q2 + q3q0; R[5] = 1 - sq1 - sq3; R[6] = q2q3 - q1q0; R[7] = 0;
            R[8] = q1q3 - q2q0; R[9] = q2q3 + q1q0; R[10] = 1 - sq1 - sq2; R[11] = 0;
            R[12] = 0; R[13] = 0; R[14] = 0; R[15] = 1;
        }
    }
}
